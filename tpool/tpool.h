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

#pragma once
#include <memory> /* unique_ptr */
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <tpool_structs.h>
#ifdef LINUX_NATIVE_AIO
#include <libaio.h>
#endif
#ifdef HAVE_URING
#include <sys/uio.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
/**
  Windows-specific native file handle struct.
  Apart from the actual handle, contains PTP_IO
  used by the Windows threadpool.
*/
struct native_file_handle
{
  HANDLE m_handle;
  PTP_IO m_ptp_io;
  native_file_handle(){};
  native_file_handle(HANDLE h) : m_handle(h), m_ptp_io() {}
  operator HANDLE() const { return m_handle; }
};
#else
#include <unistd.h>
typedef int native_file_handle;
#endif

namespace tpool
{
/**
 Task callback function
 */
typedef void (*callback_func)(void *);
typedef void (*callback_func_np)(void);
class task;

/** A class that can be used e.g. for
restricting concurrency for specific class of tasks. */

class task_group
{
private:
  circular_queue<task*> m_queue;
  std::mutex m_mtx;
  std::condition_variable m_cv;
  unsigned int m_tasks_running;
  unsigned int m_max_concurrent_tasks;
public:
  task_group(unsigned int max_concurrency= 100000);
  void set_max_tasks(unsigned int max_concurrent_tasks);
  void execute(task* t);
  void cancel_pending(task *t);
  ~task_group();
};


class task
{
public:
  callback_func m_func;
  void *m_arg;
  task_group* m_group;
  virtual void add_ref() {};
  virtual void release() {};
  task() {};
  task(callback_func func, void* arg, task_group* group = nullptr);
  void* get_arg() { return m_arg; }
  callback_func get_func() { return m_func; }
  virtual void execute();
  virtual ~task() {}
};

class waitable_task :public task
{
  std::mutex m_mtx;
  std::condition_variable m_cv;
  int m_ref_count;
  int m_waiter_count;
  callback_func m_original_func;
  void wait(std::unique_lock<std::mutex>&lk);
public:
  waitable_task(callback_func func, void* arg, task_group* group = nullptr);
  void add_ref() override;
  void release() override;
  TPOOL_SUPPRESS_TSAN bool is_running() { return get_ref_count() > 0; }
  TPOOL_SUPPRESS_TSAN int get_ref_count() {return m_ref_count;}
  void wait();
  void disable();
  void enable();
  virtual ~waitable_task() {};
};
enum class aio_opcode
{
  AIO_PREAD,
  AIO_PWRITE
};
constexpr size_t MAX_AIO_USERDATA_LEN= 4 * sizeof(void*);

/** IO control block, includes parameters for the IO, and the callback*/

struct aiocb
#ifdef _WIN32
  :OVERLAPPED
#elif defined LINUX_NATIVE_AIO
  :iocb
#elif defined HAVE_URING
  :iovec
#endif
{
  native_file_handle m_fh;
  aio_opcode m_opcode;
  unsigned long long m_offset;
  void *m_buffer;
  unsigned int m_len;
  callback_func m_callback;
  task_group* m_group;
  /* Returned length and error code*/
  size_t m_ret_len;
  int m_err;
  void *m_internal;
  task m_internal_task;
  alignas(8) char m_userdata[MAX_AIO_USERDATA_LEN];

  aiocb() : m_internal_task(nullptr, nullptr)
  {}
  void execute_callback()
  {
    task t(m_callback, this,m_group);
    t.execute();
  }
};


/**
 AIO interface
*/
class aio
{
public:
  /**
    Submit asynchronous IO.
    On completion, cb->m_callback is executed.
  */
  virtual int submit_io(aiocb *cb)= 0;
  /** "Bind" file to AIO handler (used on Windows only) */
  virtual int bind(native_file_handle &fd)= 0;
  /** "Unind" file to AIO handler (used on Windows only) */
  virtual int unbind(const native_file_handle &fd)= 0;
  virtual ~aio(){};
protected:
  static void synchronous(aiocb *cb);
  /** finish a partial read/write callback synchronously */
  static inline void finish_synchronous(aiocb *cb)
  {
    if (!cb->m_err && cb->m_ret_len != cb->m_len)
    {
      /* partial read/write */
      cb->m_buffer= (char *) cb->m_buffer + cb->m_ret_len;
      cb->m_len-= (unsigned int) cb->m_ret_len;
      cb->m_offset+= cb->m_ret_len;
      synchronous(cb);
    }
  }
};

class timer
{
public:
  virtual void set_time(int initial_delay_ms, int period_ms) = 0;
  virtual void disarm() = 0;
  virtual ~timer(){}
};

class thread_pool;

extern aio *create_simulated_aio(thread_pool *tp);

class thread_pool
{
protected:
  /* AIO handler */
  std::unique_ptr<aio> m_aio;
  virtual aio *create_native_aio(int max_io)= 0;

  /**
    Functions to be called at worker thread start/end
    can be used for example to set some TLS variables
  */
  void (*m_worker_init_callback)(void);
  void (*m_worker_destroy_callback)(void);

public:
  thread_pool() : m_aio(), m_worker_init_callback(), m_worker_destroy_callback()
  {
  }
  virtual void submit_task(task *t)= 0;
  virtual timer* create_timer(callback_func func, void *data=nullptr) = 0;
  void set_thread_callbacks(void (*init)(), void (*destroy)())
  {
    m_worker_init_callback= init;
    m_worker_destroy_callback= destroy;
  }
  int configure_aio(bool use_native_aio, int max_io)
  {
    if (use_native_aio)
      m_aio.reset(create_native_aio(max_io));
    else
      m_aio.reset(create_simulated_aio(this));
    return !m_aio ? -1 : 0;
  }
  void disable_aio()
  {
    m_aio.reset();
  }
  int bind(native_file_handle &fd) { return m_aio->bind(fd); }
  void unbind(const native_file_handle &fd) { if (m_aio) m_aio->unbind(fd); }
  int submit_io(aiocb *cb) { return m_aio->submit_io(cb); }
  virtual void wait_begin() {};
  virtual void wait_end() {};
  virtual ~thread_pool() {}
};
const int DEFAULT_MIN_POOL_THREADS= 1;
const int DEFAULT_MAX_POOL_THREADS= 500;
extern thread_pool *
create_thread_pool_generic(int min_threads= DEFAULT_MIN_POOL_THREADS,
                           int max_threads= DEFAULT_MAX_POOL_THREADS);
extern "C" void tpool_wait_begin();
extern "C" void tpool_wait_end();
#ifdef _WIN32
extern thread_pool *
create_thread_pool_win(int min_threads= DEFAULT_MIN_POOL_THREADS,
                       int max_threads= DEFAULT_MAX_POOL_THREADS);

/*
  Helper functions, to execute pread/pwrite even if file is
  opened with FILE_FLAG_OVERLAPPED, and bound to completion
  port.
*/
SSIZE_T pwrite(const native_file_handle &h, void *buf, size_t count,
           unsigned long long offset);
SSIZE_T pread(const native_file_handle &h, void *buf, size_t count,
          unsigned long long offset);
HANDLE win_get_syncio_event();
#endif
} // namespace tpool
