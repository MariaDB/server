/* Copyright (C) 2019, 2022, MariaDB Corporation.

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
#include <limits.h>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits.h>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>
#include <vector>
#include "tpool.h"
#include <assert.h>
#include <my_global.h>
#include <my_dbug.h>
#include <thr_timer.h>
#include <stdlib.h>

namespace tpool
{

#ifdef __linux__
#if defined(HAVE_URING) || defined(LINUX_NATIVE_AIO)
  extern aio* create_linux_aio(thread_pool* tp, int max_io);
#else
  aio *create_linux_aio(thread_pool *, int) { return nullptr; };
#endif
#endif
#ifdef _WIN32
  extern aio* create_win_aio(thread_pool* tp, int max_io);
#endif

  static const std::chrono::milliseconds LONG_TASK_DURATION = std::chrono::milliseconds(500);
  static const int  OVERSUBSCRIBE_FACTOR = 2;

/**
  Process the cb synchronously
*/
void aio::synchronous(aiocb *cb)
{
#ifdef _WIN32
  size_t ret_len;
#else
  ssize_t ret_len;
#endif
  int err= 0;
  switch (cb->m_opcode)
  {
  case aio_opcode::AIO_PREAD:
    ret_len= pread(cb->m_fh, cb->m_buffer, cb->m_len, cb->m_offset);
    break;
  case aio_opcode::AIO_PWRITE:
    ret_len= pwrite(cb->m_fh, cb->m_buffer, cb->m_len, cb->m_offset);
    break;
  default:
    abort();
  }
#ifdef _WIN32
  if (static_cast<int>(ret_len) < 0)
    err= GetLastError();
#else
  if (ret_len < 0)
  {
    err= errno;
    ret_len= 0;
  }
#endif
  cb->m_ret_len = ret_len;
  cb->m_err = err;
  if (ret_len)
    finish_synchronous(cb);
}


/**
  Implementation of generic threadpool.
  This threadpool consists of the following components

  - The task queue. This queue is populated by submit()
  - Worker that execute the  work items.
  - Timer thread that takes care of pool health

  The task queue is populated by submit() method.
  on submit(), a worker thread  can be woken, or created
  to execute tasks.

  The timer thread watches if work items  are being dequeued, and if not,
  this can indicate potential deadlock.
  Thus the timer thread can also wake or create a thread, to ensure some progress.

  Optimizations:

  - worker threads that are idle for long time will shutdown.
  - worker threads are woken in LIFO order, which minimizes context switching
  and also ensures that idle timeout works well. LIFO wakeup order ensures
  that active threads stay active, and idle ones stay idle.

*/

/**
 Worker wakeup flags.
*/
enum worker_wake_reason
{
  WAKE_REASON_NONE,
  WAKE_REASON_TASK,
  WAKE_REASON_SHUTDOWN
};



/* A per-worker  thread structure.*/
struct alignas(CPU_LEVEL1_DCACHE_LINESIZE)  worker_data
{
  /** Condition variable to wakeup this worker.*/
  std::condition_variable m_cv;

  /** Reason why worker was woken. */
  worker_wake_reason m_wake_reason;

  /**
    If worker wakes up with WAKE_REASON_TASK, this the task it needs to execute.
  */
  task* m_task;

  /** Struct is member of intrusive doubly linked list */
  worker_data* m_prev;
  worker_data* m_next;

  /* Current state of the worker.*/
  enum state
  {
    NONE = 0,
    EXECUTING_TASK = 1,
    LONG_TASK = 2,
    WAITING = 4
  };

  int m_state;

  bool is_executing_task()
  {
    return m_state & EXECUTING_TASK;
  }
  bool is_long_task()
  {
    return m_state & LONG_TASK;
  }
  bool is_waiting()
  {
    return m_state & WAITING;
  }
  std::chrono::system_clock::time_point m_task_start_time;
  worker_data() :
    m_cv(),
    m_wake_reason(WAKE_REASON_NONE),
    m_task(),
    m_prev(),
    m_next(),
    m_state(NONE),
    m_task_start_time()
  {}

  /*Define custom new/delete because of overaligned structure. */
  void* operator new(size_t size)
  {
#ifdef _WIN32
    return _aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE);
#else
    void* ptr;
    int ret = posix_memalign(&ptr, CPU_LEVEL1_DCACHE_LINESIZE, size);
    return ret ? 0 : ptr;
#endif
  }
  void operator delete(void* p)
  {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
  }
};


static thread_local worker_data* tls_worker_data;

class thread_pool_generic : public thread_pool
{
  /** Cache for per-worker structures */
  cache<worker_data> m_thread_data_cache;

  /** The task queue */
  circular_queue<task*> m_task_queue;

  /** List of standby (idle) workers */
  doubly_linked_list<worker_data> m_standby_threads;

  /** List of threads that are executing tasks */
  doubly_linked_list<worker_data> m_active_threads;

  /* Mutex that protects the whole struct, most importantly
  the standby threads list, and task queue */
  std::mutex m_mtx;

  /** Timeout after which idle worker shuts down */
  std::chrono::milliseconds m_thread_timeout;

  /** How often should timer wakeup.*/
  std::chrono::milliseconds m_timer_interval;

  /** Another condition variable, used in pool shutdown */
  std::condition_variable m_cv_no_threads;

  /** Condition variable for the timer thread. Signaled on shutdown. */
  std::condition_variable m_cv_timer;

  /** Overall number of enqueues*/
  unsigned long long m_tasks_enqueued;
  unsigned long long m_group_enqueued;
  /** Overall number of dequeued tasks. */
  unsigned long long m_tasks_dequeued;

  /** Statistic related, number of worker thread wakeups */
  int m_wakeups;

  /**
  Statistic related, number of spurious thread wakeups
  (i.e thread woke up, and the task queue is empty)
  */
  int m_spurious_wakeups;

  /** The desired concurrency.  This number of workers should be
  actively executing. */
  unsigned int m_concurrency;

  /** True, if threadpool is being shutdown, false otherwise */
  bool m_in_shutdown;

  /** Maintenance timer state : true = active(ON),false = inactive(OFF)*/
  enum class timer_state_t
  {
    OFF, ON
  };
  timer_state_t m_timer_state= timer_state_t::OFF;
  void switch_timer(timer_state_t state);

  /* Updates idle_since, and maybe switches the timer off */
  void check_idle(std::chrono::system_clock::time_point now);

  /** time point when timer last ran, used as a coarse clock. */
  std::chrono::system_clock::time_point m_timestamp;

  /** Number of long running tasks. The long running tasks are excluded when
  adjusting concurrency */
  unsigned int m_long_tasks_count;

  unsigned int m_waiting_task_count;

  /** Last time thread was created*/
  std::chrono::system_clock::time_point m_last_thread_creation;

  /** Minimumum number of threads in this pool.*/
  unsigned int m_min_threads;

  /** Maximimum number of threads in this pool. */
  unsigned int m_max_threads;

  /* maintenance related statistics (see maintenance()) */
  size_t m_last_thread_count;
  unsigned long long m_last_activity;

  void worker_main(worker_data *thread_data);
  void worker_end(worker_data* thread_data);

  /* Checks threadpool responsiveness, adjusts thread_counts */
  void maintenance();
  static void maintenance_func(void* arg)
  {
    ((thread_pool_generic *)arg)->maintenance();
  }
  bool add_thread();
  bool wake(worker_wake_reason reason, task *t = nullptr);
  void maybe_wake_or_create_thread();
  bool too_many_active_threads();
  bool get_task(worker_data *thread_var, task **t);
  bool wait_for_tasks(std::unique_lock<std::mutex> &lk,
                      worker_data *thread_var);
  void cancel_pending(task* t);

  size_t thread_count()
  {
    return m_active_threads.size() + m_standby_threads.size();
  }
public:
  thread_pool_generic(int min_threads, int max_threads);
  ~thread_pool_generic();
  void wait_begin() override;
  void wait_end() override;
  void submit_task(task *task) override;
  virtual aio *create_native_aio(int max_io) override
  {
#ifdef _WIN32
    return create_win_aio(this, max_io);
#elif defined(__linux__)
    return create_linux_aio(this,max_io);
#else
    return nullptr;
#endif
  }

  class timer_generic : public thr_timer_t, public timer
  {
    thread_pool_generic* m_pool;
    waitable_task m_task;
    callback_func m_callback;
    void* m_data;
    int m_period;
    std::mutex m_mtx;
    bool m_on;
    std::atomic<bool> m_running;

    void run()
    {
      /*
        In rare cases, multiple callbacks can be scheduled,
        e.g with set_time(0,0) in a loop.
        We do not allow parallel execution, as user is not prepared.
      */
      bool expected = false;
      if (!m_running.compare_exchange_strong(expected, true))
        return;

      m_callback(m_data);
      m_running = false;

      if (m_pool && m_period)
      {
        std::unique_lock<std::mutex> lk(m_mtx);
        if (m_on)
        {
          DBUG_PUSH_EMPTY;
          thr_timer_end(this);
          thr_timer_settime(this, 1000ULL * m_period);
          DBUG_POP_EMPTY;
        }
      }
    }

    static void execute(void* arg)
    {
      auto timer = (timer_generic*)arg;
      timer->run();
    }

    static void submit_task(void* arg)
    {
      timer_generic* timer = (timer_generic*)arg;
      timer->m_pool->submit_task(&timer->m_task);
    }

  public:
    timer_generic(callback_func func, void* data, thread_pool_generic * pool):
      m_pool(pool),
      m_task(timer_generic::execute,this),
      m_callback(func),m_data(data),m_period(0),m_mtx(),
      m_on(true),m_running()
    {
      if (pool)
      {
        /* EXecute callback in threadpool*/
        thr_timer_init(this, submit_task, this);
      }
      else
      {
        /* run in "timer" thread */
        thr_timer_init(this, m_task.get_func(), m_task.get_arg());
      }
    }

    void set_time(int initial_delay_ms, int period_ms) override
    {
      std::unique_lock<std::mutex> lk(m_mtx);
      if (!m_on)
        return;
      thr_timer_end(this);
      if (!m_pool)
        thr_timer_set_period(this, 1000ULL * period_ms);
      else
        m_period = period_ms;
      thr_timer_settime(this, 1000ULL * initial_delay_ms);
    }

    /*
      Change only period of a periodic timer
      (after the next execution). Workarounds
      mysys timer deadlocks
    */
    void set_period(int period_ms)
    {
      std::unique_lock<std::mutex> lk(m_mtx);
      if (!m_on)
        return;
      if (!m_pool)
       thr_timer_set_period(this, 1000ULL * period_ms);
      else
         m_period = period_ms;
    }

    void disarm() override
    {
      std::unique_lock<std::mutex> lk(m_mtx);
      m_on = false;
      thr_timer_end(this);
      lk.unlock();

      if (m_task.m_group)
      {
        m_task.m_group->cancel_pending(&m_task);
      }
      if (m_pool)
      {
        m_pool->cancel_pending(&m_task);
      }
      m_task.wait();
    }

    virtual ~timer_generic()
    {
      disarm();
    }
  };
  timer_generic m_maintenance_timer;
  virtual timer* create_timer(callback_func func, void *data) override
  {
    return new timer_generic(func, data, this);
  }
};

void thread_pool_generic::cancel_pending(task* t)
{
  std::unique_lock <std::mutex> lk(m_mtx);
  for (auto it = m_task_queue.begin(); it != m_task_queue.end(); it++)
  {
    if (*it == t)
    {
      t->release();
      *it = nullptr;
    }
  }
}
/**
  Register worker in standby list, and wait to be woken.

  @retval true  if thread was woken
  @retval false idle wait timeout exceeded (the current thread must shutdown)
*/
bool thread_pool_generic::wait_for_tasks(std::unique_lock<std::mutex> &lk,
                                         worker_data *thread_data)
{
  assert(m_task_queue.empty());
  assert(!m_in_shutdown);

  thread_data->m_wake_reason= WAKE_REASON_NONE;
  m_active_threads.erase(thread_data);
  m_standby_threads.push_back(thread_data);

  for (;;)
  {
    thread_data->m_cv.wait_for(lk, m_thread_timeout);

    if (thread_data->m_wake_reason != WAKE_REASON_NONE)
    {
      /* Woke up not due to timeout.*/
      return true;
    }

    if (thread_count() <= m_min_threads)
    {
      /* Do not shutdown thread, maintain required minimum of worker
        threads.*/
      continue;
    }

    /*
      Woke up due to timeout, remove this thread's  from the standby list. In
      all other cases where it is signaled it is removed by the signaling
      thread.
    */
    m_standby_threads.erase(thread_data);
    m_active_threads.push_back(thread_data);
    return false;
  }
}


/**
 Workers "get next task" routine.

 A task can be handed over to the current thread directly during submit().
 if thread_var->m_wake_reason == WAKE_REASON_TASK.

 Or a task can be taken from the task queue.
 In case task queue is empty, the worker thread will park (wait for wakeup).
*/
bool thread_pool_generic::get_task(worker_data *thread_var, task **t)
{
  std::unique_lock<std::mutex> lk(m_mtx);

  if (thread_var->is_long_task())
  {
    DBUG_ASSERT(m_long_tasks_count);
    m_long_tasks_count--;
  }
  DBUG_ASSERT(!thread_var->is_waiting());
  thread_var->m_state = worker_data::NONE;

  while (m_task_queue.empty())
  {
    if (m_in_shutdown)
      return false;

    if (!wait_for_tasks(lk, thread_var))
      return false;
    if (m_task_queue.empty())
    {
      m_spurious_wakeups++;
      continue;
    }
  }

  /* Dequeue from the task queue.*/
  *t= m_task_queue.front();
  m_task_queue.pop();
  m_tasks_dequeued++;
  thread_var->m_state |= worker_data::EXECUTING_TASK;
  thread_var->m_task_start_time = m_timestamp;
  return true;
}

/** Worker thread shutdown routine. */
void thread_pool_generic::worker_end(worker_data* thread_data)
{
  std::lock_guard<std::mutex> lk(m_mtx);
  DBUG_ASSERT(!thread_data->is_long_task());
  m_active_threads.erase(thread_data);
  m_thread_data_cache.put(thread_data);

  if (!thread_count() && m_in_shutdown)
  {
    /* Signal the destructor that no more threads are left. */
    m_cv_no_threads.notify_all();
  }
}

extern "C" void set_tls_pool(tpool::thread_pool* pool);

/* The worker get/execute task loop.*/
void thread_pool_generic::worker_main(worker_data *thread_var)
{
  task* task;
  set_tls_pool(this);
  if(m_worker_init_callback)
   m_worker_init_callback();

  tls_worker_data = thread_var;

  while (get_task(thread_var, &task) && task)
  {
    task->execute();
  }

  if (m_worker_destroy_callback)
    m_worker_destroy_callback();

  worker_end(thread_var);
}


/*
  Check if threadpool had been idle for a while
  Switch off maintenance timer if it is in idle state
  for too long.

  Helper function, to be used inside maintenance callback,
  before m_last_activity is updated
*/

static const auto invalid_timestamp=  std::chrono::system_clock::time_point::max();
constexpr auto max_idle_time= std::chrono::minutes(1);

/* Time since maintenance timer had nothing to do */
static std::chrono::system_clock::time_point idle_since= invalid_timestamp;
void thread_pool_generic::check_idle(std::chrono::system_clock::time_point now)
{
  DBUG_ASSERT(m_task_queue.empty());

  /*
   We think that there is no activity, if there were at most 2 tasks
   since last time, and there is a spare thread.
   The 2 tasks (and not 0) is to account for some periodic timers.
  */
  bool idle= m_standby_threads.m_count > 0;

  if (!idle)
  {
    idle_since= invalid_timestamp;
    return;
  }

  if (idle_since == invalid_timestamp)
  {
    idle_since= now;
    return;
  }

  /* Switch timer off after 1 minute of idle time */
  if (now - idle_since > max_idle_time)
  {
    idle_since= invalid_timestamp;
    switch_timer(timer_state_t::OFF);
  }
}


/*
  Periodic job to fix thread count and concurrency,
  in case of long tasks, etc
*/
void thread_pool_generic::maintenance()
{
  /*
    If pool is busy (i.e the its mutex is currently locked), we can
    skip the maintenance task, some times, to lower mutex contention
  */
  static int skip_counter;
  const int MAX_SKIPS = 10;
  std::unique_lock<std::mutex> lk(m_mtx, std::defer_lock);
  if (skip_counter == MAX_SKIPS)
  {
    lk.lock();
  }
  else if (!lk.try_lock())
  {
    skip_counter++;
    return;
  }

  skip_counter = 0;

  m_timestamp = std::chrono::system_clock::now();

  if (m_task_queue.empty())
  {
    check_idle(m_timestamp);
    m_last_activity = m_tasks_dequeued + m_wakeups;
    return;
  }

  m_long_tasks_count = 0;
  for (auto thread_data = m_active_threads.front();
    thread_data;
    thread_data = thread_data->m_next)
  {
    if (thread_data->is_executing_task() &&
       !thread_data->is_waiting() &&
      (thread_data->is_long_task()
      || (m_timestamp - thread_data->m_task_start_time > LONG_TASK_DURATION)))
    {
      thread_data->m_state |= worker_data::LONG_TASK;
      m_long_tasks_count++;
    }
  }

  maybe_wake_or_create_thread();

  size_t thread_cnt = (int)thread_count();
  if (m_last_activity == m_tasks_dequeued + m_wakeups &&
      m_last_thread_count <= thread_cnt && m_active_threads.size() == thread_cnt)
  {
    // no progress made since last iteration. create new
    // thread
    add_thread();
  }
  m_last_activity = m_tasks_dequeued + m_wakeups;
  m_last_thread_count= thread_cnt;
}

/*
  Heuristic used for thread creation throttling.
  Returns interval in milliseconds between thread creation
  (depending on number of threads already in the pool, and
  desired concurrency level)
*/
static int  throttling_interval_ms(size_t n_threads,size_t concurrency)
{
  if (n_threads < concurrency*4)
    return 0;

  if (n_threads < concurrency*8)
    return 50;

  if (n_threads < concurrency*16)
    return 100;

  return 200;
}

/* Create a new worker.*/
bool thread_pool_generic::add_thread()
{
  size_t n_threads = thread_count();

  if (n_threads >= m_max_threads)
    return false;

  if (n_threads >= m_min_threads)
  {
    auto now = std::chrono::system_clock::now();
    if (now - m_last_thread_creation <
      std::chrono::milliseconds(throttling_interval_ms(n_threads, m_concurrency)))
    {
      /*
        Throttle thread creation and wakeup deadlock detection timer,
        if is it off.
      */
      switch_timer(timer_state_t::ON);

      return false;
    }
  }

  worker_data *thread_data = m_thread_data_cache.get();
  m_active_threads.push_back(thread_data);
  try
  {
    std::thread thread(&thread_pool_generic::worker_main, this, thread_data);
    m_last_thread_creation = std::chrono::system_clock::now();
    thread.detach();
  }
  catch (std::system_error& e)
  {
    m_active_threads.erase(thread_data);
    m_thread_data_cache.put(thread_data);
    static bool warning_written;
    if (!warning_written)
    {
      fprintf(stderr, "Warning : threadpool thread could not be created :%s,"
        "current number of threads in pool %zu\n", e.what(), thread_count());
      warning_written = true;
    }
    return false;
  }
  return true;
}

/** Wake a standby thread, and hand the given task over to this thread. */
bool thread_pool_generic::wake(worker_wake_reason reason, task *)
{
  assert(reason != WAKE_REASON_NONE);

  if (m_standby_threads.empty())
    return false;
  auto var= m_standby_threads.back();
  m_standby_threads.pop_back();
  m_active_threads.push_back(var);
  assert(var->m_wake_reason == WAKE_REASON_NONE);
  var->m_wake_reason= reason;
  var->m_cv.notify_one();
  m_wakeups++;
  return true;
}


thread_pool_generic::thread_pool_generic(int min_threads, int max_threads) :
  m_thread_data_cache(max_threads),
  m_task_queue(10000),
  m_standby_threads(),
  m_active_threads(),
  m_mtx(),
  m_thread_timeout(std::chrono::milliseconds(60000)),
  m_timer_interval(std::chrono::milliseconds(400)),
  m_cv_no_threads(),
  m_cv_timer(),
  m_tasks_enqueued(),
  m_tasks_dequeued(),
  m_wakeups(),
  m_spurious_wakeups(),
  m_concurrency(std::thread::hardware_concurrency()*2),
  m_in_shutdown(),
  m_timestamp(),
  m_long_tasks_count(),
  m_waiting_task_count(),
  m_last_thread_creation(),
  m_min_threads(min_threads),
  m_max_threads(max_threads),
  m_last_thread_count(),
  m_last_activity(),
  m_maintenance_timer(thread_pool_generic::maintenance_func, this, nullptr)
{

  if (m_max_threads < m_concurrency)
    m_concurrency = m_max_threads;
  if (m_min_threads > m_concurrency)
    m_concurrency = min_threads;
  if (!m_concurrency)
    m_concurrency = 1;

  // start the timer
  m_maintenance_timer.set_time(0, (int)m_timer_interval.count());
}


void thread_pool_generic::maybe_wake_or_create_thread()
{
  if (m_task_queue.empty())
    return;
  DBUG_ASSERT(m_active_threads.size() >= static_cast<size_t>(m_long_tasks_count + m_waiting_task_count));
  if (m_active_threads.size() - m_long_tasks_count - m_waiting_task_count > m_concurrency)
    return;
  if (!m_standby_threads.empty())
  {
    wake(WAKE_REASON_TASK);
  }
  else
  {
    add_thread();
  }
}

bool thread_pool_generic::too_many_active_threads()
{
  return m_active_threads.size() - m_long_tasks_count - m_waiting_task_count >
    m_concurrency* OVERSUBSCRIBE_FACTOR;
}

/** Submit a new task*/
void thread_pool_generic::submit_task(task* task)
{
  std::unique_lock<std::mutex> lk(m_mtx);
  if (m_in_shutdown)
    return;
  task->add_ref();
  m_tasks_enqueued++;
  m_task_queue.push(task);
  maybe_wake_or_create_thread();
}


/* Notify thread pool that current thread is going to wait */
void thread_pool_generic::wait_begin()
{
  if (!tls_worker_data || tls_worker_data->is_long_task())
    return;
  std::unique_lock<std::mutex> lk(m_mtx);
  if(tls_worker_data->is_long_task())
  {
    /*
     Current task flag could have become "long-running"
     while waiting for the lock, thus recheck.
    */
    return;
  }
  DBUG_ASSERT(!tls_worker_data->is_waiting());
  tls_worker_data->m_state |= worker_data::WAITING;
  m_waiting_task_count++;

  /* Maintain concurrency */
  maybe_wake_or_create_thread();
}


void thread_pool_generic::wait_end()
{
  if (tls_worker_data && tls_worker_data->is_waiting())
  {
    std::unique_lock<std::mutex> lk(m_mtx);
    tls_worker_data->m_state &= ~worker_data::WAITING;
    m_waiting_task_count--;
  }
}


void thread_pool_generic::switch_timer(timer_state_t state)
{
  if (m_timer_state == state)
    return;
  /*
    We can't use timer::set_time, because mysys timers are deadlock
    prone.

    Instead, to switch off we increase the  timer period
    and decrease period to switch on.

    This might introduce delays in thread creation when needed,
    as period will only be changed when timer fires next time.
    For this reason, we can't use very long periods for the "off" state.
  */
  m_timer_state= state;
  long long period= (state == timer_state_t::OFF) ?
     m_timer_interval.count()*10: m_timer_interval.count();

  m_maintenance_timer.set_period((int)period);
}


/**
  Wake  up all workers, and wait until they are gone
  Stop the timer.
*/
thread_pool_generic::~thread_pool_generic()
{
  /*
    Stop AIO early.
    This is needed to prevent AIO completion thread
    from calling submit_task() on an object that is being destroyed.
  */
  m_aio.reset();

  /* Also stop the maintanence task early. */
  m_maintenance_timer.disarm();

  std::unique_lock<std::mutex> lk(m_mtx);
  m_in_shutdown= true;

  /* Wake up idle threads. */
  while (wake(WAKE_REASON_SHUTDOWN))
  {
  }

  while (thread_count())
  {
    m_cv_no_threads.wait(lk);
  }

  lk.unlock();
}

thread_pool *create_thread_pool_generic(int min_threads, int max_threads)
{
 return new thread_pool_generic(min_threads, max_threads);
}

} // namespace tpool
