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
static PSI_mutex_key key_LOCK_logger_service;
static PSI_mutex_info mutex_list[]=
{{ &key_LOCK_logger_service, "logger_service_file_st::lock", PSI_FLAG_GLOBAL}};
#endif

typedef struct logger_handle_st {
  File file;
  IO_CACHE cache;
  char path[FN_REFLEN];
  unsigned long long size_limit;
  unsigned int rotations;
  size_t path_len, buffer_size;
  mysql_mutex_t lock;
  my_off_t filesize;
} LSFS;


#define LOG_FLAGS (O_APPEND | O_CREAT | O_WRONLY)

static unsigned int n_dig(unsigned int i)
{
  return (i == 0) ? 0 : ((i < 10) ? 1 : ((i < 100) ? 2 : 3));
}


LOGGER_HANDLE *logger_open(const char *path,
                           unsigned long long size_limit,
                           unsigned int rotations, size_t buffer_size)
{
  LOGGER_HANDLE new_log, *l_perm;
  /*
    I don't think we ever need more rotations,
    but if it's so, the rotation procedure should be adapted to it.
  */
  if (rotations > 999)
    return 0;

  if (buffer_size > 0 && buffer_size < 1024)
    buffer_size= 1024;

  if (rotations > 0 && size_limit < buffer_size)
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
                                            sizeof(LOGGER_HANDLE), MYF(0))))
  {
    my_close(new_log.file, MYF(0));
    new_log.file= -1;
    return 0; /* End of memory */
  }
  *l_perm= new_log;
  mysql_mutex_init(key_LOCK_logger_service, &l_perm->lock,
                    MY_MUTEX_INIT_FAST);

  l_perm->buffer_size= buffer_size;
  l_perm->filesize= my_seek(new_log.file, 0L, MY_SEEK_END, MYF(0));

  if (l_perm->buffer_size)
    init_io_cache(&l_perm->cache, new_log.file, l_perm->buffer_size,
                  WRITE_CACHE, l_perm->filesize, FALSE, MYF(MY_WME | MY_NABP));

  return l_perm;
}

int logger_close(LOGGER_HANDLE *log)
{
  int result;
  File file= log->file;
  mysql_mutex_destroy(&log->lock);
  if (log->buffer_size)
    (void) end_io_cache(&log->cache);
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

  if (log->buffer_size)
    (void) end_io_cache(&log->cache);

  if ((result= my_close(log->file, MYF(0))))
    goto exit;
  namebuf[log->path_len]= 0;
  if ((result= my_rename(namebuf, logname(log, log->path, 1), MYF(0))))
    goto exit;

  if ((result= (log->file= my_open(namebuf, LOG_FLAGS, MYF(0))) < 0))
    goto exit;

  if (log->buffer_size)
    result= init_io_cache(&log->cache, log->file, log->buffer_size,
                          WRITE_CACHE, 0L, FALSE, MYF(MY_WME | MY_NABP));
  log->filesize= 0;
exit:
  errno= my_errno;
  return result;
}


/*
   Return 1 if we should rotate the log
*/

static my_bool logger_time_to_rotate(LOGGER_HANDLE *log)
{
  return log->rotations > 0 && log->filesize >= log->size_limit;
}


int logger_vprintf(LOGGER_HANDLE *log, const char* fmt, va_list ap)
{
  char cvtbuf[1024];
  size_t n_bytes;

  n_bytes= my_vsnprintf(cvtbuf, sizeof(cvtbuf), fmt, ap);
  if (n_bytes >= sizeof(cvtbuf))
    n_bytes= sizeof(cvtbuf) - 1;

  return logger_write(log, (uchar *) cvtbuf, n_bytes);
}


int logger_write(LOGGER_HANDLE *log, const void *buffer, size_t size)
{
  int result;

  mysql_mutex_lock(&log->lock);
  if (logger_time_to_rotate(log) && do_rotate(log))
  {
    result= -1;
    errno= my_errno;
    goto exit; /* Log rotation needed but failed */
  }

  if (log->buffer_size)
  {
    result= my_b_write(&log->cache, (uchar *) buffer, size) ? 0 : (int) size;
  }
  else
  {
    result= (int) my_write(log->file, (uchar *) buffer, size, MYF(0));
  }
 
  log->filesize+= result;

exit:
  mysql_mutex_unlock(&log->lock);
  return result;
}


int logger_rotate(LOGGER_HANDLE *log)
{
  int result;
  mysql_mutex_lock(&log->lock);
  result= do_rotate(log);
  mysql_mutex_unlock(&log->lock);
  return result;
}



int logger_sync(LOGGER_HANDLE *log)
{
  int result= 0;

  mysql_mutex_lock(&log->lock);

  if (log->buffer_size == 0)
    goto sync_file;

  result= my_b_flush_io_cache(&log->cache, 0);

sync_file:
  if (!result)
    result= my_sync(log->file, MYF(0));

  mysql_mutex_unlock(&log->lock);

  return result;
}


int logger_resize_buffer(LOGGER_HANDLE *log, size_t new_buffer_size)
{
  int result= 0;

  if (new_buffer_size > 0 && new_buffer_size < 1024)
    new_buffer_size= 1024;

  if (log->rotations > 0 && log->size_limit < new_buffer_size)
    new_buffer_size= log->size_limit;


  mysql_mutex_lock(&log->lock);
  if (log->buffer_size)
  {
    if (end_io_cache(&log->cache))
      goto exit;
  }

  if ((log->buffer_size= new_buffer_size))
    result= init_io_cache(&log->cache, log->file, new_buffer_size,
                  WRITE_CACHE, log->filesize, FALSE, MYF(MY_WME | MY_NABP));

exit:
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


int logger_set_filesize_limit(LOGGER_HANDLE *log,
                              unsigned long long new_file_limit)
{
  log->size_limit= new_file_limit;
  return 0;
}


int logger_set_rotations(LOGGER_HANDLE *log, unsigned int new_rotations)
{
  log->rotations= new_rotations;
  return 0;
}

void logger_init_mutexes(void)
{
#ifdef HAVE_PSI_INTERFACE
  if (unlikely(PSI_server))
    PSI_server->register_mutex("sql_logger", mutex_list, 1);
#endif
}

