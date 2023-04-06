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

#include "tpool_structs.h"
#include <algorithm>
#include <assert.h>
#include <condition_variable>
#include <iostream>
#include <limits.h>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>
#include <vector>
#include <tpool.h>

namespace tpool
{

/*
  Windows AIO implementation, completion port based.
  A single thread collects the completion notification with
  GetQueuedCompletionStatus(), and forwards io completion callback
  the worker threadpool
*/
class tpool_generic_win_aio : public aio
{
  /* Thread that does collects completion status from the completion port. */
  std::thread m_thread;

  /* IOCP Completion port.*/
  HANDLE m_completion_port;

  /* The worker pool where completion routine is executed, as task. */
  thread_pool* m_pool;
public:
  tpool_generic_win_aio(thread_pool* pool, int max_io) : m_pool(pool)
  {
    m_completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    m_thread = std::thread(aio_completion_thread_proc, this);
  }

  /**
   Task to be executed in the work pool.
  */
  static void io_completion_task(void* data)
  {
    auto cb = (aiocb*)data;
    cb->execute_callback();
  }

  void completion_thread_work()
  {
    for (;;)
    {
      DWORD n_bytes;
      aiocb* aiocb;
      ULONG_PTR key;
      if (!GetQueuedCompletionStatus(m_completion_port, &n_bytes, &key,
        (LPOVERLAPPED*)& aiocb, INFINITE))
        break;

      aiocb->m_err = 0;
      aiocb->m_ret_len = n_bytes;

      if (n_bytes != aiocb->m_len)
      {
        if (GetOverlappedResult(aiocb->m_fh, aiocb,
          (LPDWORD)& aiocb->m_ret_len, FALSE))
        {
           aiocb->m_err = GetLastError();
        }
      }
      aiocb->m_internal_task.m_func = aiocb->m_callback;
      aiocb->m_internal_task.m_arg = aiocb;
      aiocb->m_internal_task.m_group = aiocb->m_group;
      m_pool->submit_task(&aiocb->m_internal_task);
    }
  }

  static void aio_completion_thread_proc(tpool_generic_win_aio* aio)
  {
    aio->completion_thread_work();
  }

  ~tpool_generic_win_aio()
  {
    if (m_completion_port)
      CloseHandle(m_completion_port);
    m_thread.join();
  }

  virtual int submit_io(aiocb* cb) override
  {
    memset((OVERLAPPED *)cb, 0, sizeof(OVERLAPPED));
    cb->m_internal = this;
    ULARGE_INTEGER uli;
    uli.QuadPart = cb->m_offset;
    cb->Offset = uli.LowPart;
    cb->OffsetHigh = uli.HighPart;

    BOOL ok;
    if (cb->m_opcode == aio_opcode::AIO_PREAD)
      ok = ReadFile(cb->m_fh.m_handle, cb->m_buffer, cb->m_len, 0, cb);
    else
      ok = WriteFile(cb->m_fh.m_handle, cb->m_buffer, cb->m_len, 0, cb);

    if (ok || (GetLastError() == ERROR_IO_PENDING))
      return 0;
    return -1;
  }

  // Inherited via aio
  virtual int bind(native_file_handle& fd) override
  {
    return CreateIoCompletionPort(fd, m_completion_port, 0, 0) ? 0
      : GetLastError();
  }
  virtual int unbind(const native_file_handle& fd) override { return 0; }
};

aio* create_win_aio(thread_pool* pool, int max_io)
{
  return new tpool_generic_win_aio(pool, max_io);
}

} // namespace tpool
