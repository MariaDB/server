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
#include <vio.h>
#include "threadpool_winsockets.h"
#include <algorithm>
#include <new>
#include <vector>
#include <mutex>

/*
 A cache for IO buffers for asynchronous socket(or named pipe) reads.

 Considerations on Windows : since Windows locks the AIO buffers in physical memory,
 it is important that these buffers are compactly allocated.
 We try to prevent any kinds of memory fragmentation

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

  std::unique_lock<std::mutex> lk(m_mtx, std::defer_lock);
  for(;;)
  {
    if (lk.try_lock())
    {
      if (m_cache.size() == m_elements)
        break;
      lk.unlock();
    }
    Sleep(100);
  }
  VirtualFree(m_base, 0, MEM_RELEASE);
  m_cache.clear();
  m_base= 0;
  m_elements= 0;
}

AIO_buffer_cache::~AIO_buffer_cache() { clear(); }

/* Global variable for the cache buffers.*/
AIO_buffer_cache read_buffers;

class win_aiosocket::Prefetched_vio final : public Vio_filter
{
  uchar *m_buffer;
  size_t m_capacity;
  size_t m_read_pos{};
  size_t m_read_end{};
public:
  Prefetched_vio(uchar *buffer, size_t capacity)
    : m_buffer(buffer), m_capacity(capacity)
  {
  }

  uchar *buffer() const
  {
    return m_buffer;
  }

  size_t capacity() const
  {
    return m_capacity;
  }

  size_t readable_size() const
  {
    return m_read_end - m_read_pos;
  }
  void set_readable_size(size_t size)
  {
    DBUG_ASSERT(!readable_size());
    DBUG_ASSERT(size <= m_capacity);
    m_read_pos= 0;
    m_read_end= size;
  }

  size_t read(uchar *buf, size_t size) override
  {
    size_t available= readable_size();
    if (!available)
      return m_underlying->read(buf, size);
    size_t count= std::min(size, available);
    memcpy(buf, m_buffer + m_read_pos, count);
    m_read_pos+= count;
    return count;
  }
  my_bool has_data() const override
  {
    return readable_size() != 0 || m_underlying->has_data();
  }
};

win_aiosocket::~win_aiosocket()
{
  /*
    m_buf_ptr is on loan to the Prefetched_vio filter, which is owned by the
    VIO chain.
    Thus it is important this destructor runs after vio delete()
    vio_delete() destroys the chain first, thus the buffer can be
    safely returned to the cache.
  */
  if (m_buf_ptr)
    read_buffers.release_buffer(m_buf_ptr);
}


/** Return number of unread bytes.*/
size_t win_aiosocket::buffer_remaining() const
{
  return m_prefetched ? m_prefetched->readable_size() : 0;
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
  if (m_prefetched)
    buf= {(ULONG)m_prefetched->capacity(),
          reinterpret_cast<char *>(m_prefetched->buffer())};
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
  if (m_prefetched)
    m_prefetched->set_readable_size(nbytes);
}

void win_aiosocket::init(st_vio **vio)
{
  m_is_pipe= vio_get_transport(*vio)->type() == VIO_TYPE_NAMEDPIPE;
  m_handle= vio_handle(*vio);

  SetFileCompletionNotificationModes(m_handle, FILE_SKIP_SET_EVENT_ON_HANDLE);
  if (!(m_buf_ptr = read_buffers.acquire_buffer()))
  {
    /* Ran out of buffers, that's fine.*/
    return;
  }

  Prefetched_vio *prefetched= new (std::nothrow)
    Prefetched_vio(reinterpret_cast<uchar *>(m_buf_ptr), READ_BUFSIZ);
  if (!prefetched)
  {
    read_buffers.release_buffer(m_buf_ptr);
    m_buf_ptr= nullptr;
    return;
  }
  m_prefetched= prefetched;
  *vio= vio_wrap_transport(*vio, prefetched);
}

void init_win_aio_buffers(unsigned int n_buffers)
{
  read_buffers.set_size(n_buffers);
}

extern void destroy_win_aio_buffers()
{
  read_buffers.clear();
}
