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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */

#include <winsock2.h>
#include <my_global.h>
#include <violite.h>
#include "threadpool_winsockets.h"
#include <algorithm>
#include <vector>
#include <mutex>

/*
 A cache for IO buffers for asynchronous socket(or named pipe) reads.

 Considerations on Windows : since Windows locks the AIO buffers in physical memory,
 it is important that these buffers are compactly allocated.
 We try to to prevent any kinds of memory fragmentation

 A relatively small region (at most 1MB) is allocated, for equally sized smallish(256 bytes)
 This allow buffers. The region is pagesize-aligned (via VirtualAlloc allocation)

 We use smallish IO buffers, 256 bytes is probably large enough for most of
 the queries. Larger buffers could have funny effects(thread hogginng)
 on threadpool scheduling in case client is using protocol pipelining.

 Also note, that even in an unlikely situation where cache runs out of buffers,
 this does not lead to errors, zero szed reads will be used in WSARecv then.
*/

constexpr size_t READ_BUFSIZ= 256;
class AIO_buffer_cache
{
  const size_t ITEM_SIZE= READ_BUFSIZ;

  /** Limit the whole cache to 1MB*/
  const size_t MAX_SIZE= 1048576;

  /* Allocation base */
  char *m_base= 0;

  /* "Free list" with LIFO policy */
  std::vector<char *> m_cache;
  std::mutex m_mtx;
  size_t m_elements=0;

public:
  void set_size(size_t n_items);
  char *acquire_buffer();
  void release_buffer(char *v);
  void clear();
  ~AIO_buffer_cache();
};


void AIO_buffer_cache::set_size(size_t n_items)
{
  DBUG_ASSERT(!m_base);
  m_elements= std::min(n_items, MAX_SIZE / ITEM_SIZE);
  auto sz= m_elements * ITEM_SIZE;

  m_base=
      (char *) VirtualAlloc(0, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!m_base)
  {
    m_elements= 0;
    return;
  }

  /* Try to help memory manager here, by prelocking region in memory*/
  (void) VirtualLock(m_base, sz);

  m_cache.reserve(m_elements);
  for (ssize_t i= m_elements - 1; i >= 0 ; i--)
    m_cache.push_back(m_base + i * ITEM_SIZE);
}

/*
  Returns a buffer, or NULL if no free buffers.

  LIFO policy is implemented, so we do not touch too many
  pages (no std::stack though)
*/
char *AIO_buffer_cache::acquire_buffer()
{
  std::unique_lock<std::mutex> lk(m_mtx);
  if (m_cache.empty())
    return nullptr;
  auto p= m_cache.back();
  m_cache.pop_back();
  return p;
}

void AIO_buffer_cache::release_buffer(char *v)
{
  std::unique_lock<std::mutex> lk(m_mtx);
  m_cache.push_back(v);
}

void AIO_buffer_cache::clear()
{
  if (!m_base)
    return;

  /* Check that all items are returned to the cache. */
  DBUG_ASSERT(m_cache.size() == m_elements);
  VirtualFree(m_base, 0, MEM_RELEASE);
  m_cache.clear();
  m_base= 0;
  m_elements= 0;
}

AIO_buffer_cache::~AIO_buffer_cache() { clear(); }

/* Global variable for the cache buffers.*/
AIO_buffer_cache read_buffers;

win_aiosocket::~win_aiosocket()
{
  if (m_buf_ptr)
    read_buffers.release_buffer(m_buf_ptr);
}


/** Return number of unread bytes.*/
size_t win_aiosocket::buffer_remaining()
{
  return m_buf_datalen - m_buf_off;
}

static my_bool my_vio_has_data(st_vio *vio)
{
  auto sock= (win_aiosocket *) vio->tp_ctx;
  return sock->buffer_remaining() || sock->m_orig_vio_has_data(vio);
}

/*
 (Half-)buffered read.

 The buffer is filled once, by completion of the async IO.

 We do not refill the buffer once it is read off,
 does not make sense.
*/
static size_t my_vio_read(st_vio *vio, uchar *dest, size_t sz)
{
  auto sock= (win_aiosocket *) vio->tp_ctx;
  DBUG_ASSERT(sock);

  auto nbytes= std::min(sock->buffer_remaining(), sz);

  if (nbytes > 0)
  {
    /* Copy to output, adjust the offset.*/
    memcpy(dest, sock->m_buf_ptr + sock->m_buf_off, nbytes);
    sock->m_buf_off += nbytes;
    return nbytes;
  }

  return sock->m_orig_vio_read(vio, dest, sz);
}

DWORD win_aiosocket::begin_read()
{
  DWORD err = ERROR_SUCCESS;
  static char c;
  WSABUF buf;

  DBUG_ASSERT(!buffer_remaining());

  /*
    If there is no internal buffer to store data,
    we do zero size read, but still need a valid
    pointer for the buffer parameter.
  */
  if (m_buf_ptr)
    buf= {(ULONG)READ_BUFSIZ, m_buf_ptr};
  else
    buf= {0, &c};


  if (!m_is_pipe)
  {
    /* Do async io (sockets). */
    DWORD flags= 0;
    if (WSARecv((SOCKET) m_handle, &buf, 1, 0, &flags, &m_overlapped, NULL))
      err= WSAGetLastError();
  }
  else
  {
    /* Do async read (named pipe) */
    if (!ReadFile(m_handle, buf.buf, buf.len, 0, &m_overlapped))
      err= GetLastError();
  }

  if (!err || err == ERROR_IO_PENDING)
    return 0;
  return err;
}

void win_aiosocket::end_read(ULONG nbytes, DWORD err)
{
  DBUG_ASSERT(!buffer_remaining());
  DBUG_ASSERT(!nbytes || m_buf_ptr);
  m_buf_off= 0;
  m_buf_datalen= nbytes;
}

void win_aiosocket::init(Vio *vio)
{
  m_is_pipe= vio->type == VIO_TYPE_NAMEDPIPE;
  m_handle=
      m_is_pipe ? vio->hPipe : (HANDLE) mysql_socket_getfd(vio->mysql_socket);

  SetFileCompletionNotificationModes(m_handle, FILE_SKIP_SET_EVENT_ON_HANDLE);
  if (vio->type == VIO_TYPE_SSL)
  {
    /*
    TODO : This requires fixing viossl to call our manipulated VIO
    */
    return;
  }

  if (!(m_buf_ptr = read_buffers.acquire_buffer()))
  {
    /* Ran out of buffers, that's fine.*/
    return;
  }

  vio->tp_ctx= this;

  m_orig_vio_has_data= vio->has_data;
  vio->has_data= my_vio_has_data;

  m_orig_vio_read= vio->read;
  vio->read= my_vio_read;
}

void init_win_aio_buffers(unsigned int n_buffers)
{
  read_buffers.set_size(n_buffers);
}

extern void destroy_win_aio_buffers()
{
  read_buffers.clear();
}
