/* Copyright(C) 2019 MariaDB Corporation.

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#ifndef _WIN32
#include <unistd.h> /* pread(), pwrite() */
#endif
#include "tpool.h"
#include "tpool_structs.h"
#include <stdlib.h>
#include <string.h>

namespace tpool
{
#ifdef _WIN32

/*
  In order to be able to execute synchronous IO even on file opened
  with FILE_FLAG_OVERLAPPED, and to bypass  to completion port,
  we use valid event handle for the hEvent member of the OVERLAPPED structure,
  with its low-order bit set.

  See MSDN docs for GetQueuedCompletionStatus() for description of this trick.
*/
static DWORD fls_sync_io= FLS_OUT_OF_INDEXES;
HANDLE win_get_syncio_event()
{
  HANDLE h;

  h= (HANDLE) FlsGetValue(fls_sync_io);
  if (h)
  {
    return h;
  }
  h= CreateEventA(NULL, FALSE, FALSE, NULL);
  /* Set low-order bit to keeps I/O completion from being queued */
  h= (HANDLE)((uintptr_t) h | 1);
  FlsSetValue(fls_sync_io, h);
  return h;
}
#include <WinIoCtl.h>
static void __stdcall win_free_syncio_event(void *data)
{
  if (data)
  {
    CloseHandle((HANDLE) data);
  }
}

struct WinIoInit
{
  WinIoInit()
  {
    fls_sync_io= FlsAlloc(win_free_syncio_event);
    if(fls_sync_io == FLS_OUT_OF_INDEXES)
	  abort();
  }
  ~WinIoInit() { FlsFree(fls_sync_io); }
};

static WinIoInit win_io_init;


SSIZE_T pread(const native_file_handle &h, void *buf, size_t count,
                 unsigned long long offset)
{
  OVERLAPPED ov{};
  ULARGE_INTEGER uli;
  uli.QuadPart= offset;
  ov.Offset= uli.LowPart;
  ov.OffsetHigh= uli.HighPart;
  ov.hEvent= win_get_syncio_event();
  if (count > 0xFFFFFFFF)
    count= 0xFFFFFFFF;

  if (ReadFile(h, buf, (DWORD) count, 0, &ov) ||
      (GetLastError() == ERROR_IO_PENDING))
  {
    DWORD n_bytes;
    if (GetOverlappedResult(h, &ov, &n_bytes, TRUE))
      return n_bytes;
  }

  return -1;
}

SSIZE_T pwrite(const native_file_handle &h, void *buf, size_t count,
                  unsigned long long offset)
{
  OVERLAPPED ov{};
  ULARGE_INTEGER uli;
  uli.QuadPart= offset;
  ov.Offset= uli.LowPart;
  ov.OffsetHigh= uli.HighPart;
  ov.hEvent= win_get_syncio_event();
  if (count > 0xFFFFFFFF)
    count= 0xFFFFFFFF;
  if (WriteFile(h, buf, (DWORD) count, 0, &ov) ||
      (GetLastError() == ERROR_IO_PENDING))
  {
    DWORD n_bytes;
    if (GetOverlappedResult(h, &ov, &n_bytes, TRUE))
      return n_bytes;
  }
  return -1;
}
#endif

/**
  Simulated AIO.

  Executes IO synchronously in worker pool
  and then calls the completion routine.
*/
class simulated_aio : public aio
{
  thread_pool *m_pool;

public:
  simulated_aio(thread_pool *tp)
      : m_pool(tp)
  {
  }

  static void simulated_aio_callback(void *param)
  {
    aiocb *cb= (aiocb *) param;
    synchronous(cb);
    cb->m_internal_task.m_func= cb->m_callback;
    thread_pool *pool= (thread_pool *)cb->m_internal;
    pool->submit_task(&cb->m_internal_task);
  }

  virtual int submit_io(aiocb *aiocb) override
  {
    aiocb->m_internal_task.m_func = simulated_aio_callback;
    aiocb->m_internal_task.m_arg = aiocb;
    aiocb->m_internal_task.m_group = aiocb->m_group;
    aiocb->m_internal = m_pool;
    m_pool->submit_task(&aiocb->m_internal_task);
    return 0;
  }

  virtual int bind(native_file_handle &fd) override { return 0; }
  virtual int unbind(const native_file_handle &fd) override { return 0; }
};

aio *create_simulated_aio(thread_pool *tp)
{
  return new simulated_aio(tp);
}

} // namespace tpool
