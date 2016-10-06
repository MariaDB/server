/*
   Copyright (c) 2015, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* This C++ file's header file */
#include "./rdb_mutex_wrapper.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./rdb_utils.h"

// Internal MySQL APIs not exposed in any header.
extern "C"
{
void thd_enter_cond(MYSQL_THD thd, mysql_cond_t *cond, mysql_mutex_t *mutex,
                    const PSI_stage_info *stage, PSI_stage_info *old_stage);
void thd_exit_cond(MYSQL_THD thd, const PSI_stage_info *stage);
}

using namespace rocksdb;

namespace myrocks {

static
PSI_stage_info stage_waiting_on_row_lock2= { 0, "Waiting for row lock", 0};

static const int64_t MICROSECS= 1000*1000;
// A timeout as long as one full non-leap year worth of microseconds is as
// good as infinite timeout.
static const int64_t BIG_TIMEOUT= MICROSECS * 60 * 60 * 24 * 7 * 365;

Rdb_cond_var::Rdb_cond_var() {
  mysql_cond_init(0, &m_cond, nullptr);
}

Rdb_cond_var::~Rdb_cond_var() {
  mysql_cond_destroy(&m_cond);
}

Status Rdb_cond_var::Wait(std::shared_ptr<TransactionDBMutex> mutex_arg) {
  return WaitFor(mutex_arg, BIG_TIMEOUT);
}


/*
  @brief
    Wait on condition variable.  The caller must make sure that we own
    *mutex_ptr.  The mutex is released and re-acquired by the wait function.

  @param
     timeout_micros  Timeout in microseconds. Negative value means no timeout.

  @return
    Status::OK()       - Wait successfull
    Status::TimedOut() - Timed out or wait killed (the caller can check
                         thd_killed() to determine which occurred)
*/

Status
Rdb_cond_var::WaitFor(std::shared_ptr<TransactionDBMutex> mutex_arg,
                      int64_t timeout_micros)
{
  auto *mutex_obj= reinterpret_cast<Rdb_mutex*>(mutex_arg.get());
  DBUG_ASSERT(mutex_obj != nullptr);

  mysql_mutex_t * const mutex_ptr= &mutex_obj->m_mutex;

  int res= 0;
  struct timespec wait_timeout;

  if (timeout_micros < 0)
    timeout_micros= BIG_TIMEOUT;
  set_timespec_nsec(wait_timeout, timeout_micros*1000);

#ifndef STANDALONE_UNITTEST
  PSI_stage_info old_stage;
  mysql_mutex_assert_owner(mutex_ptr);

  if (current_thd && mutex_obj->m_old_stage_info.count(current_thd) == 0)
  {
    my_core::thd_enter_cond(current_thd, &m_cond, mutex_ptr,
                            &stage_waiting_on_row_lock2, &old_stage);
    /*
      After the mysql_cond_timedwait we need make this call

        my_core::thd_exit_cond(thd, &old_stage);

      to inform the SQL layer that KILLable wait has ended. However,
      that will cause mutex to be released. Defer the release until the mutex
      that is unlocked by RocksDB's Pessimistic Transactions system.
    */
    mutex_obj->set_unlock_action(&old_stage);
  }

#endif
  bool killed= false;

  do
  {
    res= mysql_cond_timedwait(&m_cond, mutex_ptr, &wait_timeout);

#ifndef STANDALONE_UNITTEST
    if (current_thd)
      killed= my_core::thd_killed(current_thd);
#endif
  } while (!killed && res == EINTR);

  if (res || killed)
    return Status::TimedOut();
  else
    return Status::OK();
}


/*

  @note
  This function may be called while not holding the mutex that is used to wait
  on the condition variable.

  The manual page says ( http://linux.die.net/man/3/pthread_cond_signal):

  The pthread_cond_broadcast() or pthread_cond_signal() functions may be called
  by a thread whether or not it currently owns the mutex that threads calling
  pthread_cond_wait() or pthread_cond_timedwait() have associated with the
  condition variable during their waits; however, IF PREDICTABLE SCHEDULING
  BEHAVIOR IS REQUIRED, THEN THAT MUTEX SHALL BE LOCKED by the thread calling
  pthread_cond_broadcast() or pthread_cond_signal().

  What's "predicate scheduling" and do we need it? The explanation is here:

  https://groups.google.com/forum/?hl=ky#!msg/comp.programming.threads/wEUgPq541v8/ZByyyS8acqMJ
  "The problem (from the realtime side) with condition variables is that
  if you can signal/broadcast without holding the mutex, and any thread
  currently running can acquire an unlocked mutex and check a predicate
  without reference to the condition variable, then you can have an
  indirect priority inversion."

  Another possible consequence is that one can create spurious wake-ups when
  there are multiple threads signaling the condition.

  None of this looks like a problem for our use case.
*/

void Rdb_cond_var::Notify()
{
  mysql_cond_signal(&m_cond);
}


/*
  @note
    This is called without holding the mutex that's used for waiting on the
    condition. See ::Notify().
*/
void Rdb_cond_var::NotifyAll()
{
  mysql_cond_broadcast(&m_cond);
}


Rdb_mutex::Rdb_mutex()
{
  mysql_mutex_init(0 /* Don't register in P_S. */, &m_mutex,
                   MY_MUTEX_INIT_FAST);
}

Rdb_mutex::~Rdb_mutex() {
    mysql_mutex_destroy(&m_mutex);
}

Status Rdb_mutex::Lock() {
  mysql_mutex_lock(&m_mutex);
  DBUG_ASSERT(m_old_stage_info.count(current_thd) == 0);
  return Status::OK();
}

// Attempt to acquire lock.  If timeout is non-negative, operation may be
// failed after this many milliseconds.
// If implementing a custom version of this class, the implementation may
// choose to ignore the timeout.
// Return OK on success, or other Status on failure.
Status Rdb_mutex::TryLockFor(int64_t timeout_time __attribute__((__unused__)))
{
  /*
    Note: PThreads API has pthread_mutex_timedlock(), but mysql's
    mysql_mutex_* wrappers do not wrap that function.
  */
  mysql_mutex_lock(&m_mutex);
  return Status::OK();
}


#ifndef STANDALONE_UNITTEST
void Rdb_mutex::set_unlock_action(PSI_stage_info *old_stage_arg)
{
  DBUG_ASSERT(old_stage_arg != nullptr);

  mysql_mutex_assert_owner(&m_mutex);
  DBUG_ASSERT(m_old_stage_info.count(current_thd) == 0);

  m_old_stage_info[current_thd] =
      std::make_shared<PSI_stage_info>(*old_stage_arg);
}
#endif

// Unlock Mutex that was successfully locked by Lock() or TryLockUntil()
void Rdb_mutex::UnLock() {
#ifndef STANDALONE_UNITTEST
  if (m_old_stage_info.count(current_thd) > 0)
  {
    std::shared_ptr<PSI_stage_info> old_stage = m_old_stage_info[current_thd];
    m_old_stage_info.erase(current_thd);
    /* The following will call mysql_mutex_unlock */
    my_core::thd_exit_cond(current_thd, old_stage.get());
    return;
  }
#endif
  mysql_mutex_unlock(&m_mutex);
}

}  // namespace myrocks
