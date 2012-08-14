/* Copyright (c) 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

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


/*
  Finish pending IO on pipe. Honor wait timeout
*/

static size_t wait_overlapped_result(Vio *vio, int timeout)
{
  size_t ret= (size_t) -1;
  DWORD transferred, wait_status, timeout_ms;
  DBUG_ENTER("wait_overlapped_result");

  timeout_ms= timeout >= 0 ? timeout : INFINITE;

  /* Wait for the overlapped operation to be completed. */
  ret= WaitForSingleObjectEx(vio->pipe_overlapped.hEvent, timeout_ms, TRUE);

  /*
    WaitForSingleObjects will normally return WAIT_OBJECT_O (success,
    IO completed) or WAIT_TIMEOUT.
  */
  if (ret != WAIT_OBJECT_0)
  {
    CancelIo(vio->hPipe);
    DBUG_PRINT("error",("WaitForSingleObject() returned  %d", ret));
    if (wait_status == WAIT_TIMEOUT)
      SetLastError(SOCKET_ETIMEDOUT);
    DBUG_RETURN((size_t)-1);
  }

  if (!GetOverlappedResult(vio->hPipe,&(vio->overlapped),&transferred, FALSE))
  {
    DBUG_PRINT("error",("GetOverlappedResult() returned last error  %d", 
                        GetLastError()));
    DBUG_RETURN((size_t)-1);
  }    
  DBUG_RETURN(transferred);
}


size_t vio_read_pipe(Vio *vio, uchar *buf, size_t count)
{
  DWORD transferred;
  size_t ret= (size_t) -1;
  DBUG_ENTER("vio_read_pipe");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %d", vio->sd, buf,
                       (int) count));

  disable_iocp_notification(&vio->overlapped);
  /* Attempt to read from the pipe (overlapped I/O). */
  if (ReadFile(vio->hPipe, buf, count, &transferred, &vio->overlapped))
  {
    /* The operation completed immediately. */
    ret= transferred;
  }
  /* Read operation is pending completion asynchronously? */
  else
  {
    if (GetLastError() != ERROR_IO_PENDING)
    {
      enable_iocp_notification(&vio->pipe_overlapped);
      DBUG_PRINT("error",("ReadFile() returned last error %d",
                          GetLastError()));
      DBUG_RETURN((size_t)-1);
    }
    ret= wait_overlapped_result(vio, vio->read_timeout);
  }
  enable_iocp_notification(&vio->overlapped);
  DBUG_PRINT("exit", ("%d", (int) ret));
  DBUG_RETURN(ret);
}


size_t vio_write_pipe(Vio *vio, const uchar *buf, size_t count)
{
  DWORD transferred;
  size_t ret= (size_t) -1;
  DBUG_ENTER("vio_write_pipe");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %d", vio->sd, buf,
                       (int) count));

  disable_iocp_notification(&vio->pipe_overlapped);
  /* Attempt to write to the pipe (overlapped I/O). */
  if (WriteFile(vio->hPipe, buf, count, &transferred, &vio->overlapped))
  {
    /* The operation completed immediately. */
    ret= transferred;
  }
  else
  {
    enable_iocp_notification(&vio->pipe_overlapped);
    if (GetLastError() != ERROR_IO_PENDING)
    {
      DBUG_PRINT("vio_error",("WriteFile() returned last error %d",
                              GetLastError()));
      DBUG_RETURN((size_t)-1);
    }
    ret= wait_overlapped_result(vio, vio->write_timeout);
  }
  enable_iocp_notification(&vio->pipe_overlapped);
  DBUG_PRINT("exit", ("%d", (int) ret));    
  DBUG_RETURN(ret);
}


my_bool vio_is_connected_pipe(Vio *vio)
{
  if (PeekNamedPipe(vio->hPipe, NULL, 0, NULL, NULL, NULL))
    return TRUE;
  else
    return (GetLastError() != ERROR_BROKEN_PIPE);
}


int vio_close_pipe(Vio *vio)
{
  BOOL ret;
  DBUG_ENTER("vio_close_pipe");

  CancelIo(vio->hPipe);
  CloseHandle(vio->overlapped.hEvent);
  DisconnectNamedPipe(vio->hPipe);
  ret= CloseHandle(vio->hPipe);

  vio->type= VIO_CLOSED;
  vio->hPipe= NULL;
  vio->mysql_socket= MYSQL_INVALID_SOCKET;

  DBUG_RETURN(ret);
}

#endif

