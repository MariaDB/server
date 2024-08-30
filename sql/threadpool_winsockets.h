/* Copyright (C) 2020 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */
#pragma once

#include <WinSock2.h>
#include <windows.h>

struct st_vio;

struct win_aiosocket
{
  /** OVERLAPPED is needed by all Windows AIO*/
  OVERLAPPED m_overlapped{};
  /** Handle to pipe, or socket */
  HANDLE m_handle{};
  /** Whether the m_handle refers to pipe*/
  bool m_is_pipe{};
 
  /* Read buffer handling */

  /** Pointer to buffer of size READ_BUFSIZ. Can be NULL.*/
  char *m_buf_ptr{};
  /** Offset to current buffer position*/
  size_t m_buf_off{};
  /** Size of valid data in the buffer*/
  size_t m_buf_datalen{};

  /*  Vio handling */
  /** Pointer to original vio->vio_read/vio->has_data function */
  size_t (*m_orig_vio_read)(st_vio *, unsigned char *, size_t){};
  char (*m_orig_vio_has_data)(st_vio *){};



  /**
   Begins asynchronnous reading from socket/pipe. 
   On IO completion, pre-read some bytes into internal buffer
  */
  DWORD begin_read();

  /**
   Update number of bytes returned, and IO error status

   Should be called right after IO is completed
   GetQueuedCompletionStatus() , or threadpool IO completion
   callback would return nbytes and the error.

   Sets the valid data length in the read buffer.
  */
  void end_read(ULONG nbytes, DWORD err);

  /**
    Override VIO routines with ours, accounting for
    one-shot buffering.
  */
  void init(st_vio *vio);

  /** Return number of unread bytes.*/
  size_t buffer_remaining();

  /* Frees the read buffer.*/
  ~win_aiosocket();
};

/* Functions related to IO buffers caches.*/
extern void init_win_aio_buffers(unsigned int n_buffers);
extern void destroy_win_aio_buffers();
