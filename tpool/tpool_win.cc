/* Copyright (C) 2019, 2021, MariaDB Corporation.

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
#include <tpool.h>
#include <windows.h>
#include <atomic>

/**
 Implementation of tpool/aio based on Windows native threadpool.
*/

namespace tpool
{
/**
 Pool, based on Windows native(Vista+) threadpool.
*/
class thread_pool_win : public thread_pool
{
  /**
    Handle per-thread init/term functions.
    Since it is Windows that creates thread, and not us,
    it is tricky. We employ thread local storage data
    and check whether init function was called, inside every callback.
  */
  struct tls_data
  {
    thread_pool_win *m_pool;
    ~tls_data()
    {
      /* Call thread termination function. */
      if (!m_pool)
        return;

      if (m_pool->m_worker_destroy_callback)
        m_pool->m_worker_destroy_callback();

      m_pool->m_thread_count--;
    }
    /** This needs to be called before every IO or simple task callback.*/
    void callback_prolog(thread_pool_win* pool)
    {
      assert(pool);
      assert(!m_pool || (m_pool == pool));
      if (m_pool)
      {
        // TLS data already initialized.
        return;
      }
      m_pool = pool;
      m_pool->m_thread_count++;
      // Call the thread init function.
      if (m_pool->m_worker_init_callback)
        m_pool->m_worker_init_callback();
    }
  };

  static thread_local struct tls_data tls_data;
  /** Timer */
  class native_timer : public timer
  {
    std::mutex m_mtx; // protects against parallel execution
    std::mutex m_shutdown_mtx; // protects m_on
    PTP_TIMER m_ptp_timer;
    callback_func m_func;
    void *m_data;
    thread_pool_win& m_pool;
    int m_period;
    bool m_on;

    static void CALLBACK timer_callback(PTP_CALLBACK_INSTANCE callback_instance, void *context,
                                        PTP_TIMER callback_timer)
    {
      native_timer *timer= (native_timer *) context;
      tls_data.callback_prolog(&timer->m_pool);
      std::unique_lock<std::mutex> lk(timer->m_mtx, std::defer_lock);
      if (!lk.try_lock())
      {
        /* Do not try to run timers in parallel */
        return;
      }
      timer->m_func(timer->m_data);
      if (timer->m_period)
        timer->set_time(timer->m_period, timer->m_period);
    }

  public:
     native_timer(thread_pool_win& pool, callback_func func, void* data) :
          m_mtx(), m_func(func), m_data(data), m_pool(pool), m_period(), m_on(true)
    {
      m_ptp_timer= CreateThreadpoolTimer(timer_callback, this, &pool.m_env);
    }
    void set_time(int initial_delay_ms, int period_ms) override
    {
      std::unique_lock<std::mutex> lk(m_shutdown_mtx);
      if (!m_on)
        return;
      long long initial_delay = -10000LL * initial_delay_ms;
      SetThreadpoolTimer(m_ptp_timer, NULL, 0, 0);
      SetThreadpoolTimer(m_ptp_timer, (PFILETIME)&initial_delay, 0, 100);
      m_period = period_ms;
    }
    void disarm() override
    {
      std::unique_lock<std::mutex> lk(m_shutdown_mtx);
      m_on = false;
      SetThreadpoolTimer(m_ptp_timer, NULL , 0, 0);
      lk.unlock();
      /* Don't do it in timer callback, that will hang*/
      WaitForThreadpoolTimerCallbacks(m_ptp_timer, TRUE);
    }

    ~native_timer()
    {
      disarm();
      CloseThreadpoolTimer(m_ptp_timer);
    }
  };
  /** AIO handler */
  class native_aio : public aio
  {
    thread_pool_win& m_pool;

  public:
    native_aio(thread_pool_win &pool, int max_io)
      : m_pool(pool)
    {
    }

    /**
     Submit async IO.
    */
    virtual int submit_io(aiocb* cb) override
    {
      memset((OVERLAPPED *)cb, 0, sizeof(OVERLAPPED));

      ULARGE_INTEGER uli;
      uli.QuadPart = cb->m_offset;
      cb->Offset = uli.LowPart;
      cb->OffsetHigh = uli.HighPart;
      cb->m_internal = this;
      StartThreadpoolIo(cb->m_fh.m_ptp_io);

      BOOL ok;
      if (cb->m_opcode == aio_opcode::AIO_PREAD)
        ok = ReadFile(cb->m_fh.m_handle, cb->m_buffer, cb->m_len, 0, cb);
      else
        ok = WriteFile(cb->m_fh.m_handle, cb->m_buffer, cb->m_len, 0, cb);

      if (ok || (GetLastError() == ERROR_IO_PENDING))
        return 0;

      CancelThreadpoolIo(cb->m_fh.m_ptp_io);
      return -1;
    }

    /**
     PTP_WIN32_IO_CALLBACK-typed function, required parameter for
     CreateThreadpoolIo(). The user callback and other auxiliary data is put into
     the extended OVERLAPPED parameter.
    */
    static void CALLBACK io_completion_callback(PTP_CALLBACK_INSTANCE instance,
      PVOID context, PVOID overlapped,
      ULONG io_result, ULONG_PTR nbytes,
      PTP_IO io)
    {
      aiocb* cb = (aiocb*)overlapped;
      native_aio* aio = (native_aio*)cb->m_internal;
      tls_data.callback_prolog(&aio->m_pool);
      cb->m_err = io_result;
      cb->m_ret_len = (int)nbytes;
      cb->m_internal_task.m_func = cb->m_callback;
      cb->m_internal_task.m_group = cb->m_group;
      cb->m_internal_task.m_arg = cb;
      cb->m_internal_task.execute();
    }

    /**
      Binds the file handle via CreateThreadpoolIo().
    */
    virtual int bind(native_file_handle& fd) override
    {
      fd.m_ptp_io =
        CreateThreadpoolIo(fd.m_handle, io_completion_callback, 0, &(m_pool.m_env));
      if (fd.m_ptp_io)
        return 0;
      return -1;
    }

    /**
      Unbind the file handle via CloseThreadpoolIo.
    */
    virtual int unbind(const native_file_handle& fd) override
    {
      if (fd.m_ptp_io)
        CloseThreadpoolIo(fd.m_ptp_io);
      return 0;
    }
  };

  PTP_POOL m_ptp_pool;
  TP_CALLBACK_ENVIRON m_env;
  PTP_CLEANUP_GROUP m_cleanup;
  const int TASK_CACHE_SIZE= 10000;

  struct task_cache_entry
  {
    thread_pool_win *m_pool;
    task* m_task;
  };
  cache<task_cache_entry> m_task_cache;
  std::atomic<int> m_thread_count;
public:
  thread_pool_win(int min_threads= 0, int max_threads= 0)
      : m_task_cache(TASK_CACHE_SIZE),m_thread_count(0)
  {
    InitializeThreadpoolEnvironment(&m_env);
    m_ptp_pool= CreateThreadpool(NULL);
    m_cleanup= CreateThreadpoolCleanupGroup();
    SetThreadpoolCallbackPool(&m_env, m_ptp_pool);
    SetThreadpoolCallbackCleanupGroup(&m_env, m_cleanup, 0);
    if (min_threads)
      SetThreadpoolThreadMinimum(m_ptp_pool, min_threads);
    if (max_threads)
      SetThreadpoolThreadMaximum(m_ptp_pool, max_threads);
  }
  ~thread_pool_win()
  {
    CloseThreadpoolCleanupGroupMembers(m_cleanup, TRUE, NULL);
    CloseThreadpoolCleanupGroup(m_cleanup);
    CloseThreadpool(m_ptp_pool);

    // Wait until all threads finished and TLS destructors ran.
    while(m_thread_count)
      Sleep(1);
  }
  /**
   PTP_SIMPLE_CALLBACK-typed function, used by TrySubmitThreadpoolCallback()
  */
  static void CALLBACK task_callback(PTP_CALLBACK_INSTANCE, void *param)
  {
    auto entry= (task_cache_entry *) param;
    auto task= entry->m_task;

    tls_data.callback_prolog(entry->m_pool);

    entry->m_pool->m_task_cache.put(entry);

    task->execute();
  }
  virtual void submit_task(task *task) override
  {
    auto entry= m_task_cache.get();
    task->add_ref();
    entry->m_pool= this;
    entry->m_task= task;
    if (!TrySubmitThreadpoolCallback(task_callback, entry, &m_env))
      abort();
  }

  aio *create_native_aio(int max_io) override
  {
    return new native_aio(*this, max_io);
  }

  timer* create_timer(callback_func func, void* data)  override
  {
    return new native_timer(*this, func, data);
  }
};

thread_local struct thread_pool_win::tls_data thread_pool_win::tls_data;

thread_pool *create_thread_pool_win(int min_threads, int max_threads)
{
  return new thread_pool_win(min_threads, max_threads);
}
} // namespace tpool
