/* Copyright (C) 2012-2025 Monty Program Ab

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
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#endif /*FLOGGER_SKIP_INCLUDES*/

#ifndef flogger_mutex_init
#define flogger_mutex_init(A,B,C) mysql_mutex_init(A,B,C)
#define flogger_mutex_destroy(A) mysql_mutex_destroy(A)
#define flogger_mutex_lock(A) mysql_mutex_lock(A)
#define flogger_mutex_unlock(A) mysql_mutex_unlock(A)
#endif /*flogger_mutex_init*/

#ifdef HAVE_PSI_INTERFACE
/* These belong to the service initialization */
static PSI_mutex_key key_LOCK_logger_service;
static PSI_mutex_info mutex_list[]=
{{ &key_LOCK_logger_service, "logger_service_file_st::lock", PSI_FLAG_GLOBAL}};
#endif

struct logger_handle_st
{
  logger_handle_st() : data(nullptr), size_limit(0), buffer_limit(0), rotations(0)
  {}
  // We are using a pointer to string to guarantee that we can decrease the size
  // of the buffer
  // memory buffer storing the log messages
  std::unique_ptr<std::string> data;
  unsigned long long size_limit;
  unsigned long long buffer_limit;
  unsigned int rotations;
  File file;
  char path[FN_REFLEN];
  size_t path_len;
  mysql_mutex_t lock;

  void resize_buffer(void);
};

void logger_handle_st::resize_buffer(void)
{
  std::unique_ptr<std::string> new_buffer(new std::string());
  new_buffer->reserve(buffer_limit);
  data.swap(new_buffer);
}

#define LOG_FLAGS (O_APPEND | O_CREAT | O_WRONLY)

static unsigned int n_dig(unsigned int i)
{
  unsigned int n=1;
  if (i < 10)
    n= 1;
  else if (i < 100)
    n= 2;
  else if (i < 1000)
    n= 3;
  else if (i < 10000)
    n= 4;
  else if (i < 100000)
    n= 5;
  else if (i < 1000000)
    n= 6;
  return n;
}


LOGGER_HANDLE *logger_open(const char *path,
                           unsigned long long size_limit,
                           unsigned long long buffer_limit,
                           unsigned int rotations)
{
  LOGGER_HANDLE new_log;
  /*
    I don't think we ever need more rotations,
    but if it's so, the rotation procedure should be adapted to it.
  */
  if (rotations > 9999999)
    return 0;

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

  struct logger_handle_st* l_handle(new logger_handle_st());

  l_handle->rotations= rotations;
  l_handle->size_limit= size_limit;
  l_handle->buffer_limit= buffer_limit;
  l_handle->path_len= new_log.path_len;
  strcpy(l_handle->path, new_log.path);
  l_handle->file= new_log.file;

  flogger_mutex_init(key_LOCK_logger_service, &l_handle->lock,
                     MY_MUTEX_INIT_FAST);

  if (!buffer_limit)
    return (LOGGER_HANDLE*)l_handle;

  l_handle->resize_buffer();

  return (LOGGER_HANDLE*)l_handle;
}

int logger_close(LOGGER_HANDLE *log)
{
  int result;
  File file= log->file;
  flogger_mutex_destroy(&log->lock);
  if ((result= my_close(file, MYF(0))))
    errno= my_errno;
  delete log;
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
exit:
  errno= my_errno;
  return log->file < 0 || result;
}


/*
   Return 1 if we should rotate the log
*/

my_bool logger_time_to_rotate(LOGGER_HANDLE *log)
{
  my_off_t filesize;
  if (log->rotations > 0)
  {
    filesize= my_tell(log->file, MYF(0));
    if (filesize != (my_off_t) -1)
    {
      filesize+= log->data->size(); // Add buffer
      if ((unsigned long long)filesize >= log->size_limit)
        return 1;
    }
  }

  return 0;
}


int logger_vprintf(LOGGER_HANDLE *log, const char* fmt, va_list ap)
{
  int result=0;
  char cvtbuf[1024];
  size_t n_bytes;

  flogger_mutex_lock(&log->lock);
  if (logger_time_to_rotate(log) && do_rotate(log))
  {
    result= -1;
    errno= my_errno;
    goto exit; /* Log rotation needed but failed */
  }

  n_bytes= my_vsnprintf(cvtbuf, sizeof(cvtbuf), fmt, ap);
  if (n_bytes >= sizeof(cvtbuf))
    n_bytes= sizeof(cvtbuf) - 1;

  if (log->data.get() == nullptr || log->data->capacity() == 0)
  {
    result= (int)my_write(log->file, (uchar *) cvtbuf, n_bytes, MYF(0));
  }
  else
  {
    // Buffered logging
    const size_t msg_size = log->data->size() + n_bytes;
    if (msg_size > log->data->capacity() - 1)
      logger_flush(log);
    *(log->data)+= cvtbuf;
  }

exit:
  flogger_mutex_unlock(&log->lock);
  return result;
}


static int logger_write_r(LOGGER_HANDLE *log, my_bool allow_rotations,
                          const char *buffer, size_t size)
{
  int result=0;

  flogger_mutex_lock(&log->lock);
  if (allow_rotations && logger_time_to_rotate(log) && do_rotate(log))
  {
    result= -1;
    errno= my_errno;
    goto exit; /* Log rotation needed but failed */
  }

  if (log->data.get() == nullptr || log->data->capacity() == 0)
  {
    result= (int)my_write(log->file, (uchar *) buffer, size, MYF(0));
  }
  else
  {
    // Buffered logging
    const size_t msg_size = log->data->size() + size;
    if (msg_size > log->data->capacity() - 1)
      result= logger_flush(log);
    *(log->data)+= buffer;
  }

exit:
  flogger_mutex_unlock(&log->lock);
  return result;
}


int logger_write(LOGGER_HANDLE *log, const char *buffer, size_t size)
{
  return logger_write_r(log, TRUE, buffer, size);
}

int logger_rotate(LOGGER_HANDLE *log, const unsigned int n_rotations)
{
  int result=0;
  flogger_mutex_lock(&log->lock);
  if (n_rotations)
    log->rotations= n_rotations;
  result= do_rotate(log);
  flogger_mutex_unlock(&log->lock);

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
    PSI_server->register_mutex("sql_logger", mutex_list, 1);
#endif
}

/** Resize buffer size.

@param[in,out]  log         log handle

@return 0 success, !0 for error  */
int logger_resize_buffer(LOGGER_HANDLE *log, unsigned long long buffer_limit)
{
  if (log->data.get() != nullptr && buffer_limit == log->data->capacity())
    return 0;
   
  flogger_mutex_lock(&log->lock);
  // Write out what we have currently, that way we don't have
  // to deal with old data in the buffer
  logger_flush(log);
  log->buffer_limit= buffer_limit;

  if (buffer_limit == 0)
  {
    log->data.reset();
  }
  else
  {
    log->resize_buffer();
  }
  
  flogger_mutex_unlock(&log->lock);
  return 0;
}

/** Flush buffered log to the disk.

Note that this function does not use mutexes, thus
it should be called from function that does or
in shutdown or from signal handler.

@param[in,out]  log         log handle

@return 0 success, !0 for error  */
int logger_flush(LOGGER_HANDLE *log)
{
  size_t result= 0;

  if (log->data.get() == nullptr || log->data->size() == 0)
    return 0;

  const size_t curr_size= log->data->capacity();
  if (logger_time_to_rotate(log) && do_rotate(log))
  {
    /* Check errno for the cause */
    errno= my_errno;
    result= 1;
    goto exit;
  }

  result= my_write(log->file, (uchar *) log->data->data(), log->data->size(), MYF(0));

  if (result == (size_t)-1)
  {
    /* Check errno for the cause */
    errno= my_errno;
    goto exit;
  }

  result= 0;
  log->data->clear();
  log->data->reserve(curr_size);

exit:
  return result;
}

/** Set a new file size limit.

If file size limit is changed we rotate to new log file
if limit is reached.

@param[in,out]  log         log handle
@param[in] size_limit       file size limit

@return 0 success, !0 for error  */
int logger_resize_size(LOGGER_HANDLE *log, unsigned long long size_limit)
{
  int ret= 0;

  if (size_limit == log->size_limit)
    return ret; // nothing to do

  flogger_mutex_lock(&log->lock);

  if ((ret= logger_flush(log)))
    goto exit;

  log->size_limit= size_limit;

exit:
  flogger_mutex_unlock(&log->lock);
  return ret;
}


/** Set a new file name.

@param[in,out]  log         log handle
@param[in] path             new file name

@return 0 success, !0 for error  */
int logger_rename_file(LOGGER_HANDLE *log, const char *path)
{
  int res=0;

  flogger_mutex_lock(&log->lock);

  if ((res= logger_flush(log)))
    goto exit;

  if ((res= my_close(log->file, MYF(0))))
  {
    /* Check errno for the cause */
    errno= my_errno;
    goto exit;
  }

  log->path_len= strlen(fn_format(log->path, path,
        mysql_data_home, "", MY_UNPACK_FILENAME));

  if ((log->file= my_open(log->path, LOG_FLAGS, MYF(0))) < 0)
  {
    errno= my_errno;
    /* Check errno for the cause */
    res= 1;
  }
exit:
  flogger_mutex_unlock(&log->lock);
  return res;
}
