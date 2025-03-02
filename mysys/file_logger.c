/* Copyright (C) 2012 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


#ifndef FLOGGER_SKIP_INCLUDES
#include "my_global.h"
#include <my_sys.h>
#include <m_string.h>
#include <mysql/service_logger.h>
#include <my_pthread.h>
#endif /*FLOGGER_SKIP_INCLUDES*/

#ifdef HAVE_PSI_INTERFACE
/* These belong to the service initialization */
static PSI_mutex_key key_LOCK_logger_service, key_LOCK_logger_service_buffer;
static PSI_mutex_info mutex_list[]=
{{ &key_LOCK_logger_service, "logger_service_file_st::lock", PSI_FLAG_GLOBAL},
 { &key_LOCK_logger_service_buffer, "logger_service_file_st::lock_buffer", PSI_FLAG_GLOBAL}};
#endif

typedef struct logger_handle_st {
  File file;
  char path[FN_REFLEN];
  unsigned long long size_limit;
  unsigned int rotations;
  size_t path_len;
  mysql_mutex_t lock;
  mysql_mutex_t lock_buffer;

  uchar *buffer, *idle_buffer;
  size_t buffer_size, buffer_left;
  uchar *buffer_pos;
  unsigned long long in_file;
} LSFS;


#define LOG_FLAGS (O_APPEND | O_CREAT | O_WRONLY)

static unsigned int n_dig(unsigned int i)
{
  return (i == 0) ? 0 : ((i < 10) ? 1 : ((i < 100) ? 2 : 3));
}


static my_off_t loc_tell(File fd)
{
  os_off_t pos= IF_WIN(_telli64(fd),lseek(fd, 0, SEEK_END));
  if (pos == (os_off_t) -1)
  {
    my_errno= errno;
  }
  return (my_off_t) pos;
}


LOGGER_HANDLE *logger_open(
    const char *path, unsigned long long size_limit,
    unsigned int rotations, size_t buffer_size)
{
  LOGGER_HANDLE new_log, *l_perm;
  my_off_t cur_fsize;

  /*
    I don't think we ever need more rotations,
    but if it's so, the rotation procedure should be adapted to it.
  */
  if (rotations > 999)
    return 0;

  if (buffer_size > 0 && buffer_size < 1024)
    buffer_size= 1024;

  if (size_limit < buffer_size)
    buffer_size= size_limit;

  new_log.rotations= rotations;
  new_log.size_limit= size_limit;
  new_log.path_len= strlen(fn_format(new_log.path, path,
        mysql_data_home, "", MY_UNPACK_FILENAME));

  if (new_log.path_len+n_dig(rotations)+1 > FN_REFLEN)
  {
    errno= ENAMETOOLONG;
    /* File path too long */
    return 0;
  }
  if ((new_log.file= my_open(new_log.path, LOG_FLAGS, MYF(0))) < 0)
  {
    errno= my_errno;
    /* Check errno for the cause */
    return 0;
  }

  if (!(l_perm= (LOGGER_HANDLE *) my_malloc(PSI_INSTRUMENT_ME,
                   sizeof(LOGGER_HANDLE) + buffer_size * 2, MYF(0))))
  {
    my_close(new_log.file, MYF(0));
    new_log.file= -1;
    return 0; /* End of memory */
  }
  *l_perm= new_log;
  mysql_mutex_init(key_LOCK_logger_service, &l_perm->lock,
                   MY_MUTEX_INIT_FAST);

  l_perm->buffer_size= buffer_size;
  mysql_mutex_init(key_LOCK_logger_service, &l_perm->lock_buffer,
                   MY_MUTEX_INIT_FAST);

  l_perm->buffer= ((uchar *) l_perm) + sizeof(LOGGER_HANDLE);
  l_perm->idle_buffer= l_perm->buffer + buffer_size;

  l_perm->buffer_pos= l_perm->buffer;

  if ((cur_fsize= loc_tell(l_perm->file)) == (my_off_t) -1)
  {
    l_perm->in_file= 0; /* do something on error? */
    l_perm->buffer_left= buffer_size;
  }
  else
  {
    l_perm->in_file= (unsigned long long) cur_fsize;
    if (cur_fsize >= size_limit)
      l_perm->buffer_left= 0;
    else
    {
      unsigned long long file_left= size_limit - cur_fsize;
      l_perm->buffer_left= buffer_size > file_left ? file_left : buffer_size;
    }
  }

  return l_perm;
}


static void set_buffer_pos_and_left(LOGGER_HANDLE *log)
{
  if (log->in_file <= log->size_limit)
  {
    unsigned long long file_left= log->size_limit - log->in_file;
    log->buffer_left= (file_left < log->buffer_size) ?
                        file_left : log->buffer_size;
  }
  else
  {
    log->buffer_left= log->buffer_size;
    DBUG_ASSERT(0);  /* should never happen.*/
  }
  log->buffer_pos= log->buffer;
}


static int flush_buffer(LOGGER_HANDLE *log)
{
  int result= 0;
  size_t in_buffer=  log->buffer_pos - log->buffer;

  if (my_write(log->file, log->buffer, in_buffer, MYF(0)) != in_buffer)
  {
    errno= my_errno;
    result= 1;
  }
  log->in_file+= in_buffer;

  set_buffer_pos_and_left(log);

  return result;
}


int logger_close(LOGGER_HANDLE *log)
{
  File file;
  int result;

  flush_buffer(log);
  mysql_mutex_destroy(&log->lock);
  file= log->file;
  my_free(log);
  if ((result= my_close(file, MYF(0))))
    errno= my_errno;
  return result;
}


static char *logname(LOGGER_HANDLE *log, char *buf, unsigned int n_log)
{
  sprintf(buf+log->path_len, ".%0*u", n_dig(log->rotations), n_log);
  return buf;
}


static int do_rotate(LOGGER_HANDLE *log)
{
  char namebuf[FN_REFLEN];
  int result;
  unsigned int i;
  char *buf_old, *buf_new, *tmp;

  if (log->rotations == 0)
    return 0;

  memcpy(namebuf, log->path, log->path_len);

  buf_new= logname(log, namebuf, log->rotations);
  buf_old= log->path;
  for (i=log->rotations-1; i>0; i--)
  {
    logname(log, buf_old, i);
    if (!access(buf_old, F_OK) &&
        (result= my_rename(buf_old, buf_new, MYF(0))))
      goto exit;
    tmp= buf_old;
    buf_old= buf_new;
    buf_new= tmp;
  }
  if ((result= my_close(log->file, MYF(0))))
    goto exit;
  namebuf[log->path_len]= 0;
  result= my_rename(namebuf, logname(log, log->path, 1), MYF(0));
  log->file= my_open(namebuf, LOG_FLAGS, MYF(0));
  log->in_file= 0;
exit:
  errno= my_errno;
  return log->file < 0 || result;
}


int logger_vprintf(LOGGER_HANDLE *log, const char* fmt, va_list ap)
{
  char cvtbuf[1024];
  size_t n_bytes;

  n_bytes= my_vsnprintf(cvtbuf, sizeof(cvtbuf), fmt, ap);
  if (n_bytes >= sizeof(cvtbuf))
    n_bytes= sizeof(cvtbuf) - 1;

  return logger_write(log, cvtbuf, n_bytes);
}


int logger_write(LOGGER_HANDLE *log, const void *data, size_t size)
{
  int result;
  size_t in_buffer;

  mysql_mutex_lock(&log->lock);

  if (log->buffer_left >= size)
  {
    memcpy(log->buffer_pos, data, size);
    log->buffer_pos+= size;
    log->buffer_left-= size;
    mysql_mutex_unlock(&log->lock);
    return size;
  }

  mysql_mutex_lock(&log->lock_buffer);

  in_buffer= log->buffer_pos - log->buffer;

  { /* swap buffers. */
    uchar *tmp_buf= log->buffer;
    log->buffer= log->idle_buffer;
    log->idle_buffer= tmp_buf;
  }

  set_buffer_pos_and_left(log);

  /* Now other threads can write to the new buffer. */
  mysql_mutex_unlock(&log->lock);

  if ((in_buffer &&
        my_write(log->file, log->idle_buffer, in_buffer, MYF(0)) != in_buffer)||
      my_write(log->file, (uchar *) data, size, MYF(0)) != size)
  {
    result= -1;
    errno= my_errno;
    goto exit_buf;
  }
  else
    result= 0;

  log->in_file+= in_buffer + size;
  if (log->rotations > 0 && log->in_file >= log->size_limit)
  {
    if (do_rotate(log))
    {
      result= -1;
      errno= my_errno;
      goto exit_buf; /* Log rotation needed but failed */
    }
  }

exit_buf:
  mysql_mutex_unlock(&log->lock_buffer);
  return result;
}


int logger_rotate(LOGGER_HANDLE *log)
{
  int result;
  mysql_mutex_lock(&log->lock);
  mysql_mutex_lock(&log->lock_buffer);
  result= flush_buffer(log) || do_rotate(log);
  mysql_mutex_unlock(&log->lock_buffer);
  mysql_mutex_unlock(&log->lock);
  return result;
}


int logger_sync(LOGGER_HANDLE *log)
{
  int result;
  mysql_mutex_lock(&log->lock);
  mysql_mutex_lock(&log->lock_buffer);
  if ((result= flush_buffer(log)))
  {
    result= fsync(log->file);
  }
  mysql_mutex_unlock(&log->lock_buffer);
  mysql_mutex_unlock(&log->lock);
  return result;
}


int logger_printf(LOGGER_HANDLE *log, const char *fmt, ...)
{
  int result;
  va_list args;
  va_start(args,fmt);
  result= logger_vprintf(log, fmt, args);
  va_end(args);
  return result;
}

void logger_init_mutexes()
{
#ifdef HAVE_PSI_INTERFACE
  if (unlikely(PSI_server))
  {
    PSI_server->register_mutex("sql_logger", mutex_list, 1);
    PSI_server->register_mutex("sql_logger_buffer", mutex_list, 1);
  }
#endif
}

