/*****************************************************************************

Copyright (c) 2020, MariaDB Corporation.

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

#include "srw_lock.h"
#include "srv0srv.h"

#ifdef SRW_LOCK_DUMMY
void srw_lock_low::init()
{
  DBUG_ASSERT(!is_locked_or_waiting());
  pthread_mutex_init(&mutex, nullptr);
  pthread_cond_init(&cond, nullptr);
}

void srw_lock_low::destroy()
{
  DBUG_ASSERT(!is_locked_or_waiting());
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}

inline void srw_lock_low::wait(uint32_t l)
{
  pthread_mutex_lock(&mutex);
  if (value() == l)
    pthread_cond_wait(&cond, &mutex);
  pthread_mutex_unlock(&mutex);
}

inline void srw_lock_low::wake_one()
{
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
}

inline void srw_lock_low::wake_all()
{
  pthread_mutex_lock(&mutex);
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&mutex);
}
#else
static_assert(4 == sizeof(rw_lock), "ABI");
# ifdef _WIN32
#  include <synchapi.h>

inline void srw_lock_low::wait(uint32_t l)
{
  WaitOnAddress(word(), &l, 4, INFINITE);
}
inline void srw_lock_low::wake_one() { WakeByAddressSingle(word()); }
inline void srw_lock_low::wake_all() { WakeByAddressAll(word()); }
# else
#  ifdef __linux__
#   include <linux/futex.h>
#   include <sys/syscall.h>
#   define SRW_FUTEX(a,op,n) \
    syscall(SYS_futex, a, FUTEX_ ## op ## _PRIVATE, n, nullptr, nullptr, 0)
#  elif defined __OpenBSD__
#  include <sys/time.h>
#  include <sys/futex.h>
#   define SRW_FUTEX(a,op,n) \
    futex((volatile uint32_t*) a, FUTEX_ ## op, n, nullptr, nullptr)
#  else
#   error "no futex support"
#  endif

inline void srw_lock_low::wait(uint32_t l) { SRW_FUTEX(word(), WAIT, l); }
inline void srw_lock_low::wake_one() { SRW_FUTEX(word(), WAKE, 1); }
inline void srw_lock_low::wake_all() { SRW_FUTEX(word(), WAKE, INT_MAX); }
# endif
#endif

/** Wait for a read lock.
@param lock word value from a failed read_trylock() */
void srw_lock_low::read_lock(uint32_t l)
{
  do
  {
    if (l == WRITER_WAITING)
    {
    wake_writer:
#ifdef SRW_LOCK_DUMMY
      pthread_mutex_lock(&mutex);
      {
        pthread_cond_signal(&cond);
        pthread_cond_wait(&cond, &mutex);
        l= value();
      }
      while (l == WRITER_WAITING);
      pthread_mutex_unlock(&mutex);
      continue;
#else
      wake_one();
#endif
    }
    else
      for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
      {
        ut_delay(srv_spin_wait_delay);
        if (read_trylock(l))
          return;
        else if (l == WRITER_WAITING)
          goto wake_writer;
      }

    wait(l);
  }
  while (!read_trylock(l));
}

/** Wait for a write lock after a failed write_trylock() */
void srw_lock_low::write_lock()
{
  for (;;)
  {
    uint32_t l= write_lock_wait_start();
    /* We are the first writer to be granted the lock. Spin for a while. */
    for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
    {
      if (write_lock_wait_try(l))
        return;
      if (!(l & WRITER_WAITING))
        l= write_lock_wait_start();
      ut_delay(srv_spin_wait_delay);
    }

    if (write_lock_wait_try(l))
      return;

    if (!(l & WRITER_WAITING))
    {
      if (l == UNLOCKED && write_trylock())
        return;
      l= write_lock_wait_start() | WRITER_WAITING;
    }
    else
      DBUG_ASSERT(~WRITER_WAITING & l);

    wait(l);
  }
}

void srw_lock_low::rd_unlock() { if (read_unlock()) wake_one(); }

void srw_lock_low::wr_unlock() { write_unlock(); wake_all(); }
