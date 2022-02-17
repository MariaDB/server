/*****************************************************************************
Copyright (c) 2020 MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/*
The  group commit synchronization used in log_write_up_to()
works as follows

For simplicity, lets consider only write operation,synchronozation of
flush operation works the same.

Rules of the game

A thread enters log_write_up_to() with lsn of the current transaction
1. If last written lsn is greater than wait lsn (another thread already
   wrote the log buffer),then there is no need to do anything.
2. If no other thread is currently writing, write the log buffer,
   and update last written lsn.
3. Otherwise, wait, and go to step 1.

Synchronization can be done in different ways, e.g

a) Simple mutex locking the entire check and write operation
Disadvantage that threads that could continue after updating
last written lsn, still wait.

b) Spinlock, with periodic checks for last written lsn.
Fixes a) but burns CPU unnecessary.

c) Mutex / condition variable  combo.

Condtion variable notifies (broadcast) all waiters, whenever
last written lsn is changed.

Has a disadvantage of many suprious wakeups, stress on OS scheduler,
and mutex contention.

d) Something else.
Make use of the waiter's lsn parameter, and only wakeup "right" waiting
threads.

We chose d). Even if implementation is more complicated than alternatves
due to the need to maintain list of waiters, it provides the best performance.

See group_commit_lock implementation for details.

Note that if write operation is very fast, a) or b) can be fine as alternative.
*/
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <my_cpu.h>

#include <log0types.h>
#include "log0sync.h"
#include <mysql/service_thd_wait.h>
#include <sql_class.h>
/**
  Helper class , used in group commit lock.

  Binary semaphore, or (same thing), an auto-reset event
  Has state (signalled or not), and provides 2 operations.
  wait() and wake()

  The implementation uses efficient locking primitives on Linux and Windows.
  Or, mutex/condition combo elsewhere.
*/

class binary_semaphore
{
public:
  /**Wait until semaphore becomes signalled, and atomically reset the state
  to non-signalled*/
  void wait();
  /** signals the semaphore */
  void wake();

private:
#if defined(__linux__) || defined (_WIN32)
  std::atomic<int> m_signalled;
  static constexpr std::memory_order mem_order= std::memory_order_acq_rel;
public:
  binary_semaphore() :m_signalled(0) {}
#else
  std::mutex m_mtx{};
  std::condition_variable m_cv{};
  bool m_signalled = false;
#endif
};

#if defined (__linux__) || defined (_WIN32)
void binary_semaphore::wait()
{
  for (;;)
  {
    if (m_signalled.exchange(0, mem_order) == 1)
    {
      break;
    }
#ifdef _WIN32
    int zero = 0;
    WaitOnAddress(&m_signalled, &zero, sizeof(m_signalled), INFINITE);
#else
    syscall(SYS_futex, &m_signalled, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
#endif
  }
}

void binary_semaphore::wake()
{
  if (m_signalled.exchange(1, mem_order) == 0)
  {
#ifdef _WIN32
    WakeByAddressSingle(&m_signalled);
#else
    syscall(SYS_futex, &m_signalled, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#endif
  }
}
#else
void binary_semaphore::wait()
{
  std::unique_lock<std::mutex> lk(m_mtx);
  while (!m_signalled)
    m_cv.wait(lk);
  m_signalled = false;
}
void binary_semaphore::wake()
{
  std::unique_lock<std::mutex> lk(m_mtx);
  m_signalled = true;
  m_cv.notify_one();
}
#endif

/* A thread helper structure, used in group commit lock below*/
struct group_commit_waiter_t
{
  lsn_t m_value=0;
  binary_semaphore m_sema{};
  group_commit_waiter_t* m_next= nullptr;
  bool m_group_commit_leader=false;
};

group_commit_lock::group_commit_lock() :
  m_mtx(), m_value(0), m_pending_value(0), m_lock(false), m_waiters_list()
{
}

group_commit_lock::value_type group_commit_lock::value() const
{
  return m_value.load(std::memory_order::memory_order_relaxed);
}

group_commit_lock::value_type group_commit_lock::pending() const
{
  return m_pending_value.load(std::memory_order::memory_order_relaxed);
}

void group_commit_lock::set_pending(group_commit_lock::value_type num)
{
  ut_a(num >= value());
  m_pending_value.store(num, std::memory_order::memory_order_relaxed);
}

const unsigned int MAX_SPINS = 1; /** max spins in acquire */
thread_local group_commit_waiter_t thread_local_waiter;

static inline void do_completion_callback(const completion_callback* cb)
{
  if (cb)
    cb->m_callback(cb->m_param);
}

group_commit_lock::lock_return_code group_commit_lock::acquire(value_type num, const completion_callback *callback)
{
  unsigned int spins = MAX_SPINS;

  for(;;)
  {
    if (num <= value())
    {
      /* No need to wait.*/
      do_completion_callback(callback);
      return lock_return_code::EXPIRED;
    }

    if(spins-- == 0)
      break;
    if (num > pending())
    {
      /* Longer wait expected (longer than currently running operation),
        don't spin.*/
      break;
    }
    ut_delay(1);
  }

  thread_local_waiter.m_value = num;
  thread_local_waiter.m_group_commit_leader= false;
  std::unique_lock<std::mutex> lk(m_mtx, std::defer_lock);
  while (num > value() || thread_local_waiter.m_group_commit_leader)
  {
    lk.lock();

    /* Re-read current value after acquiring the lock*/
    if (num <= value() &&
       (!thread_local_waiter.m_group_commit_leader || m_lock))
    {
      lk.unlock();
      do_completion_callback(callback);
      return lock_return_code::EXPIRED;
    }

    if (!m_lock)
    {
      /* Take the lock, become group commit leader.*/
      m_lock = true;
#ifndef DBUG_OFF
      m_owner_id = std::this_thread::get_id();
#endif
      if (callback)
        m_pending_callbacks.push_back({num,*callback});
      return lock_return_code::ACQUIRED;
    }

    if (callback && (m_waiters_list || num <= pending()))
    {
      /*
      If num > pending(), we have a good candidate for the next group
      commit lead, that will be taking over the lock after current owner
      releases it.  We put current thread into waiter's list so it sleeps
      and can be signaled and marked as group commit lead  during lock release.

      For this to work well, pending() must deliver a good approximation for N
      in the next call to group_commit_lock::release(N).
      */
      m_pending_callbacks.push_back({num, *callback});
      return lock_return_code::CALLBACK_QUEUED;
    }

    /* Add yourself to waiters list.*/
    thread_local_waiter.m_group_commit_leader= false;
    thread_local_waiter.m_next = m_waiters_list;
    m_waiters_list = &thread_local_waiter;
    lk.unlock();

    /* Sleep until woken in release().*/
    thd_wait_begin(0,THD_WAIT_GROUP_COMMIT);
    thread_local_waiter.m_sema.wait();
    thd_wait_end(0);

  }
  do_completion_callback(callback);
  return lock_return_code::EXPIRED;
}

group_commit_lock::value_type group_commit_lock::release(value_type num)
{
  completion_callback callbacks[1000];
  size_t callback_count = 0;
  value_type ret = 0;
  std::unique_lock<std::mutex> lk(m_mtx);
  m_lock = false;

  /* Update current value. */
  ut_a(num >= value());
  m_value.store(num, std::memory_order_relaxed);

  /*
    Wake waiters for value <= current value.
    Wake one more waiter, who will become the group commit lead.
  */
  group_commit_waiter_t* cur, * prev, * next;
  group_commit_waiter_t* wakeup_list = nullptr;
  for (auto& c : m_pending_callbacks)
  {
    if (c.first <= num)
    {
      if (callback_count < array_elements(callbacks))
        callbacks[callback_count++] = c.second;
      else
        c.second.m_callback(c.second.m_param);
    }
  }

  for (prev= nullptr, cur= m_waiters_list; cur; cur= next)
  {
    next= cur->m_next;
    if (cur->m_value <= num)
    {
      /* Move current waiter to wakeup_list*/

      if (!prev)
      {
        /* Remove from the start of the list.*/
        m_waiters_list = next;
      }
      else
      {
        /* Remove from the middle of the list.*/
        prev->m_next= cur->m_next;
      }

      /* Append entry to the wakeup list.*/
      cur->m_next = wakeup_list;
      wakeup_list = cur;
    }
    else
    {
      prev= cur;
    }
  }

  auto it= std::remove_if(
      m_pending_callbacks.begin(), m_pending_callbacks.end(),
      [num](const pending_cb &c) { return c.first <= num; });

  m_pending_callbacks.erase(it, m_pending_callbacks.end());

  if (m_pending_callbacks.size() || m_waiters_list)
  {
    /*
     Ensure that after this thread released the lock,
     there is a new group commit leader
     We take this from waiters list or wakeup list. It
     might look like a spurious wake, but in fact we just
     ensure the waiter do not wait for eternity.
    */
    if (m_waiters_list)
    {
      /* Move one waiter to wakeup list */
      auto e= m_waiters_list;
      m_waiters_list= m_waiters_list->m_next;
      e->m_next= wakeup_list;
      e->m_group_commit_leader= true;
      wakeup_list = e;
    }
    else if (wakeup_list)
    {
      wakeup_list->m_group_commit_leader=true;
    }
    else
    {
      /* Tell the caller that some pending callbacks left, and he should
      do something to prevent stalls. This should be a rare situation.*/
      ret= m_pending_callbacks[0].first;
    }
  }

  lk.unlock();

  /*
    Release designated next group commit lead first,
    to minimize spurious wakeups.
  */
  if (wakeup_list && wakeup_list->m_group_commit_leader)
  {
    next = wakeup_list->m_next;
    wakeup_list->m_sema.wake();
    wakeup_list= next;
  }

  for (size_t i = 0; i < callback_count; i++)
    callbacks[i].m_callback(callbacks[i].m_param);

  for (cur= wakeup_list; cur; cur= next)
  {
    next= cur->m_next;
    cur->m_sema.wake();
  }
  return ret;
}

#ifndef DBUG_OFF
bool group_commit_lock::is_owner()
{
  return m_lock && std::this_thread::get_id() == m_owner_id;
}
#endif

