/* Copyright (c) 2011, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2026, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "vio_priv.h"

#ifdef _WIN32

/*
  Disable posting IO completion event to the port.
  In some cases (synchronous timed IO) we want to skip IOCP notifications.
*/
static void disable_iocp_notification(OVERLAPPED *overlapped)
{
  HANDLE *handle = &(overlapped->hEvent);
  *handle = ((HANDLE)((ULONG_PTR) *handle|1));
}

/* Enable posting IO completion event to the port */
static void enable_iocp_notification(OVERLAPPED *overlapped)
{
  HANDLE *handle = &(overlapped->hEvent);
  *handle = (HANDLE)((ULONG_PTR) *handle & ~1);
}

Named_pipe_vio::Named_pipe_vio(HANDLE pipe)
  : Vio_transport(VIO_TYPE_NAMEDPIPE, VIO_LOCALHOST, "named pipe"),
    m_pipe(pipe), m_overlapped{}, m_overlapped_result(0), m_shutdown_flag(0)
{
  m_overlapped.hEvent= CreateEvent(NULL, FALSE, FALSE, NULL);
}

Named_pipe_vio::~Named_pipe_vio()= default;

int Named_pipe_vio::io_wait(
  enum enum_vio_io_event event __attribute__((unused)), int timeout)
{
  DWORD error, transferred, wait_status, timeout_ms;
  HANDLE event_handle;

  timeout_ms= timeout >= 0 ? timeout : INFINITE;
  /* The low bit suppresses IOCP delivery; it is not part of the event handle. */
  event_handle= (HANDLE)((ULONG_PTR)m_overlapped.hEvent & ~(ULONG_PTR)1);

  /* Wait for the overlapped operation to be completed. */
  vio_wait_begin(timeout);
  wait_status= WaitForSingleObject(event_handle, timeout_ms);
  vio_wait_end(timeout);

  /* The operation might have completed, attempt to retrieve the result. */
  if (wait_status == WAIT_OBJECT_0)
  {
    /* If retrieval fails, a error code will have been set. */
    if (GetOverlappedResult(m_pipe, &m_overlapped, &transferred, FALSE))
    {
      m_overlapped_result= transferred;
      return 1;
    }
    vio_set_was_timeout(FALSE);
    return -1;
  }
  else
  {
    error= wait_status == WAIT_TIMEOUT ? SOCKET_ETIMEDOUT : GetLastError();

    /*
      Cancel this operation and consume its completion before its OVERLAPPED
      structure or caller-owned buffer can be reused.
    */
    CancelIoEx(m_pipe, &m_overlapped);
    GetOverlappedResult(m_pipe, &m_overlapped, &transferred, TRUE);
    SetLastError(error);

    /*
      If the wait timed out, set error code to indicate a
      timeout error. Otherwise, wait_status is WAIT_FAILED
      and extended error information was already set.
    */
    if (wait_status == WAIT_TIMEOUT)
    {
      vio_set_was_timeout(TRUE);
      return 0;
    }
    vio_set_was_timeout(FALSE);
    return -1;
  }
}


size_t Named_pipe_vio::read(uchar *buf, size_t count)
{
  DWORD transferred;
  size_t ret= (size_t) -1;
  DBUG_ENTER("vio_read_pipe");
  if (m_shutdown_flag)
  {
    vio_set_was_timeout(FALSE);
    return ret;
  }

  disable_iocp_notification(&m_overlapped);

  /* Attempt to read from the pipe (overlapped I/O). */
  if (ReadFile(m_pipe, buf, (DWORD)count, &transferred, &m_overlapped))
  {
    /* The operation completed immediately. */
    ret= transferred;
  }
  /* Read operation is pending completion asynchronously? */
  else if (GetLastError() == ERROR_IO_PENDING)
  {
    if (m_shutdown_flag)
      CancelIo(m_pipe);
    if (io_wait(VIO_IO_EVENT_READ, m_read_timeout) > 0)
      ret= m_overlapped_result;
  }
  else
    vio_set_was_timeout(FALSE);
  if (!ret)
    vio_set_was_timeout(FALSE);
  enable_iocp_notification(&m_overlapped);

  DBUG_RETURN(ret);
}


size_t Named_pipe_vio::write(const uchar *buf, size_t count)
{
  DWORD transferred;
  size_t ret= (size_t) -1;
  DBUG_ENTER("vio_write_pipe");
  if (m_shutdown_flag == SHUT_RDWR)
  {
    vio_set_was_timeout(FALSE);
    return ret;
  }
  disable_iocp_notification(&m_overlapped);
  /* Attempt to write to the pipe (overlapped I/O). */
  if (WriteFile(m_pipe, buf, (DWORD)count, &transferred, &m_overlapped))
  {
    /* The operation completed immediately. */
    ret= transferred;
  }
  /* Write operation is pending completion asynchronously? */
  else if (GetLastError() == ERROR_IO_PENDING)
  {
    if (m_shutdown_flag == SHUT_RDWR)
      CancelIo(m_pipe);
    if (io_wait(VIO_IO_EVENT_WRITE, m_write_timeout) > 0)
      ret= m_overlapped_result;
  }
  else
    vio_set_was_timeout(FALSE);
  if (!ret && count)
    vio_set_was_timeout(FALSE);
  enable_iocp_notification(&m_overlapped);
  DBUG_RETURN(ret);
}


my_bool Named_pipe_vio::is_connected()
{
  if (PeekNamedPipe(m_pipe, NULL, 0, NULL, NULL, NULL))
    return TRUE;
  else
    return (GetLastError() != ERROR_BROKEN_PIPE);
}


int Named_pipe_vio::close()
{
  BOOL ret;
  DBUG_ENTER("vio_close_pipe");

  CloseHandle(m_overlapped.hEvent);
  ret= CloseHandle(m_pipe);

  m_type= VIO_CLOSED;
  m_state= VIO_STATE_CLOSED;
  m_pipe= NULL;

  DBUG_RETURN(ret);
}

/* return number of bytes readable from pipe.*/
ssize_t Named_pipe_vio::pending()
{
  DWORD bytes;
  return PeekNamedPipe(m_pipe, NULL, 0, NULL, &bytes, NULL) ? bytes : 0;
}

int Named_pipe_vio::set_timeout(uint which, int timeout_ms)
{
  if (which)
    m_write_timeout= timeout_ms;
  else
    m_read_timeout= timeout_ms;
  return 0;
}

int Named_pipe_vio::shutdown(int how)
{
  m_shutdown_flag= how;
  m_state= VIO_STATE_SHUTDOWN;
  return CancelIoEx(m_pipe, NULL);
}
#endif
