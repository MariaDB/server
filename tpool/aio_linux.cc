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

#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include "tpool.h"
#include <thread>
#ifdef LINUX_NATIVE_AIO
#include <libaio.h>
#endif
/*
  Linux AIO implementation, based on native AIO.
  Needs libaio.h and -laio at the compile time.

  submit_io() is used to submit async IO.

  There is a single thread, that collects the completion notification
  with  io_getevent(), and forwards io completion callback
  the worker threadpool.
*/
namespace tpool
{
#ifdef LINUX_NATIVE_AIO

class aio_linux : public aio
{
  thread_pool* m_pool;
  io_context_t m_io_ctx;
  bool m_in_shutdown;
  std::thread m_getevent_thread;

  static void getevent_thread_routine(aio_linux* aio)
  {
    for (;;)
    {
      io_event event;
      struct timespec ts{0, 500000000};
      int ret = io_getevents(aio->m_io_ctx, 1, 1, &event, &ts);

      if (aio->m_in_shutdown)
        break;

      if (ret > 0)
      {
        aiocb* iocb = (aiocb*)event.obj;
        long long res = event.res;
        if (res < 0)
        {
          iocb->m_err = static_cast<int>(-res);
          iocb->m_ret_len = 0;
        }
        else
        {
          iocb->m_ret_len = ret;
          iocb->m_err = 0;
        }

        iocb->m_internal_task.m_func = iocb->m_callback;
        iocb->m_internal_task.m_arg = iocb;
        iocb->m_internal_task.m_group = iocb->m_group;
        aio->m_pool->submit_task(&iocb->m_internal_task);
        continue;
      }
      switch (ret)
      {
      case -EAGAIN:
        usleep(1000);
        continue;
      case -EINTR:
      case 0:
        continue;
      default:
        fprintf(stderr, "io_getevents returned %d\n", ret);
        abort();
      }
    }
  }

public:
  aio_linux(io_context_t ctx, thread_pool* pool)
    : m_pool(pool), m_io_ctx(ctx),
    m_in_shutdown(), m_getevent_thread(getevent_thread_routine, this)
  {
  }

  ~aio_linux()
  {
    m_in_shutdown = true;
    m_getevent_thread.join();
    io_destroy(m_io_ctx);
  }

  // Inherited via aio
  virtual int submit_io(aiocb* cb) override
  {

    if (cb->m_opcode == aio_opcode::AIO_PREAD)
      io_prep_pread((iocb *)cb, cb->m_fh, cb->m_buffer, cb->m_len,
        cb->m_offset);
    else
      io_prep_pwrite((iocb *)cb, cb->m_fh, cb->m_buffer, cb->m_len,
        cb->m_offset);

    int ret;
    ret = io_submit(m_io_ctx, 1, (iocb * *)& cb);
    if (ret == 1)
      return 0;
    errno = -ret;
    return -1;
  }

  // Inherited via aio
  virtual int bind(native_file_handle& fd) override
  {
    return 0;
  }
  virtual int unbind(const native_file_handle& fd) override
  {
    return 0;
  }
};

aio* create_linux_aio(thread_pool* pool, int max_io)
{
  io_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  int ret = io_setup(max_io, &ctx);
  if (ret)
  {
    fprintf(stderr, "io_setup(%d) returned %d\n", max_io, ret);
    return nullptr;
  }
  return new aio_linux(ctx, pool);
}
#else
aio* create_linux_aio(thread_pool* pool, int max_aio)
{
  return nullptr;
}
#endif
}
