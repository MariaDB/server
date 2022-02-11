/*****************************************************************************

Copyright (c) 2020, 2022, MariaDB Corporation.

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
#include "my_cpu.h"
#include "transactional_lock_guard.h"

#ifdef NO_ELISION
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
# include <intrin.h>
bool have_transactional_memory;
bool transactional_lock_enabled()
{
  int regs[4];
  __cpuid(regs, 0);
  if (regs[0] < 7)
    return false;
  __cpuidex(regs, 7, 0);
  /* Restricted Transactional Memory (RTM) */
  have_transactional_memory= regs[1] & 1U << 11;
  return have_transactional_memory;
}
#elif defined __GNUC__ && (defined __i386__ || defined __x86_64__)
# include <cpuid.h>
bool have_transactional_memory;
bool transactional_lock_enabled()
{
  if (__get_cpuid_max(0, nullptr) < 7)
    return false;
  unsigned eax, ebx, ecx, edx;
  __cpuid_count(7, 0, eax, ebx, ecx, edx);
  /* Restricted Transactional Memory (RTM) */
  have_transactional_memory= ebx & 1U << 11;
  return have_transactional_memory;
}

# ifdef UNIV_DEBUG
TRANSACTIONAL_TARGET
bool xtest() { return have_transactional_memory && _xtest(); }
# endif
#elif defined __powerpc64__
# ifdef __linux__
#  include <sys/auxv.h>

#  ifndef PPC_FEATURE2_HTM_NOSC
#   define PPC_FEATURE2_HTM_NOSC 0x01000000
#  endif
#  ifndef PPC_FEATURE2_HTM_NO_SUSPEND
#   define PPC_FEATURE2_HTM_NO_SUSPEND 0x00080000
#  endif

#  ifndef AT_HWCAP2
#   define AT_HWCAP2 26
#  endif
# endif
bool have_transactional_memory;
bool transactional_lock_enabled()
{
# ifdef __linux__
  return getauxval(AT_HWCAP2) &
    (PPC_FEATURE2_HTM_NOSC | PPC_FEATURE2_HTM_NO_SUSPEND);
# endif
}

# ifdef UNIV_DEBUG
TRANSACTIONAL_TARGET bool xtest()
{
  return have_transactional_memory &&
    _HTM_STATE (__builtin_ttest ()) == _HTM_TRANSACTIONAL;
}
# endif
#endif

/** @return the parameter for srw_pause() */
static inline unsigned srw_pause_delay()
{
  return my_cpu_relax_multiplier / 4 * srv_spin_wait_delay;
}

/** Pause the CPU for some time, with no memory accesses. */
static inline void srw_pause(unsigned delay)
{
  HMT_low();
  while (delay--)
    MY_RELAX_CPU();
  HMT_medium();
}

#ifdef SUX_LOCK_GENERIC
template<> void srw_mutex_impl<true>::wr_wait()
{
  const unsigned delay= srw_pause_delay();

  for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
  {
    srw_pause(delay);
    if (wr_lock_try())
      return;
  }

  pthread_mutex_lock(&lock);
}

template<bool spinloop>
void ssux_lock_impl<spinloop>::init()
{
  DBUG_ASSERT(!is_locked_or_waiting());
  pthread_mutex_init(&mutex, nullptr);
  pthread_cond_init(&cond_shared, nullptr);
  pthread_cond_init(&cond_exclusive, nullptr);
}

template<bool spinloop>
void ssux_lock_impl<spinloop>::destroy()
{
  DBUG_ASSERT(!is_locked_or_waiting());
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond_shared);
  pthread_cond_destroy(&cond_exclusive);
}

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::writer_wait(uint32_t l)
{
  pthread_mutex_lock(&mutex);
  while (value() == l)
    pthread_cond_wait(&cond_exclusive, &mutex);
  pthread_mutex_unlock(&mutex);
}

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::readers_wait(uint32_t l)
{
  pthread_mutex_lock(&mutex);
  while (value() == l)
    pthread_cond_wait(&cond_shared, &mutex);
  pthread_mutex_unlock(&mutex);
}

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wake()
{
  pthread_mutex_lock(&mutex);
  uint32_t l= value();
  if (l & WRITER)
    DBUG_ASSERT(!(l & ~WRITER_PENDING));
  else
  {
    pthread_cond_broadcast(&cond_exclusive);
    if (!(l & WRITER_PENDING))
      pthread_cond_broadcast(&cond_shared);
  }
  pthread_mutex_unlock(&mutex);
}

/** Wait for a read lock.
@param lock word value from a failed read_trylock() */
template<bool spinloop>
void ssux_lock_impl<spinloop>::read_lock(uint32_t l)
{
  do
  {
    if (l == WRITER_WAITING)
    {
    wake_writer:
      pthread_mutex_lock(&mutex);
      for (;;)
      {
        if (l == WRITER_WAITING)
          pthread_cond_signal(&cond_exclusive);
        l= value();
        if (!(l & WRITER_PENDING))
          break;
        pthread_cond_wait(&cond_shared, &mutex);
      }
      pthread_mutex_unlock(&mutex);
      continue;
    }
    else if (spinloop)
    {
      const unsigned delay= srw_pause_delay();

      for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
      {
        srw_pause(delay);
        if (read_trylock<true>(l))
          return;
        else if (l == WRITER_WAITING)
          goto wake_writer;
      }
    }

    readers_wait(l);
  }
  while (!read_trylock<true>(l));
}

/** Wait for an update lock.
@param lock word value from a failed update_trylock() */
template<bool spinloop>
void ssux_lock_impl<spinloop>::update_lock(uint32_t l)
{
  do
  {
    if ((l | UPDATER) == (UPDATER | WRITER_WAITING))
    {
    wake_writer:
      pthread_mutex_lock(&mutex);
      for (;;)
      {
        if ((l | UPDATER) == (UPDATER | WRITER_WAITING))
          pthread_cond_signal(&cond_exclusive);
        l= value();
        if (!(l & WRITER_PENDING))
          break;
        pthread_cond_wait(&cond_shared, &mutex);
      }
      pthread_mutex_unlock(&mutex);
      continue;
    }
    else if (spinloop)
    {
      const unsigned delay= srw_pause_delay();

      for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
      {
        srw_pause(delay);
        if (update_trylock(l))
          return;
        else if ((l | UPDATER) == (UPDATER | WRITER_WAITING))
          goto wake_writer;
      }
    }

    readers_wait(l);
  }
  while (!update_trylock(l));
}

/** Wait for a write lock after a failed write_trylock() or upgrade_trylock()
@param holding_u  whether we already hold u_lock() */
template<bool spinloop>
void ssux_lock_impl<spinloop>::write_lock(bool holding_u)
{
  for (;;)
  {
    write_lock_wait_start();

    const uint32_t e= holding_u ? WRITER_WAITING | UPDATER : WRITER_WAITING;
    uint32_t l= e;
    if (write_lock_wait_try(l))
      return;

    if (!(l & WRITER_WAITING))
    {
      switch (l) {
      case UNLOCKED:
        DBUG_ASSERT(!holding_u);
        if (write_trylock())
          return;
        break;
      case UPDATER:
        if (holding_u && upgrade_trylock())
          return;
      }

      for (l= write_lock_wait_start_read() | WRITER_WAITING;
           (l | WRITER_WAITING) == e; )
        if (write_lock_wait_try(l))
          return;
    }
    else
      DBUG_ASSERT(~WRITER_WAITING & l);

    writer_wait(l);
  }
}

template<bool spinloop>
void ssux_lock_impl<spinloop>::rd_unlock() { if (read_unlock()) wake(); }
template<bool spinloop>
void ssux_lock_impl<spinloop>::u_unlock() { update_unlock(); wake(); }
template<bool spinloop>
void ssux_lock_impl<spinloop>::wr_unlock() { write_unlock(); wake(); }

template void ssux_lock_impl<false>::init();
template void ssux_lock_impl<false>::destroy();
template void ssux_lock_impl<false>::rd_unlock();
template void ssux_lock_impl<false>::u_unlock();
template void ssux_lock_impl<false>::wr_unlock();
template void ssux_lock_impl<true>::init();
template void ssux_lock_impl<true>::destroy();
template void ssux_lock_impl<true>::read_lock(uint32_t);
template void ssux_lock_impl<true>::rd_unlock();
template void ssux_lock_impl<true>::u_unlock();
template void ssux_lock_impl<true>::wr_unlock();
template void ssux_lock_impl<true>::write_lock(bool);
template void ssux_lock_impl<true>::update_lock(uint32_t);
#else /* SUX_LOCK_GENERIC */
static_assert(4 == sizeof(rw_lock), "ABI");
# ifdef _WIN32
#  include <synchapi.h>

template<bool spinloop>
inline void srw_mutex_impl<spinloop>::wait(uint32_t lk)
{ WaitOnAddress(&lock, &lk, 4, INFINITE); }
template<bool spinloop>
void srw_mutex_impl<spinloop>::wake() { WakeByAddressSingle(&lock); }

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wait(uint32_t lk)
{ WaitOnAddress(&readers, &lk, 4, INFINITE); }
template<bool spinloop>
void ssux_lock_impl<spinloop>::wake() { WakeByAddressSingle(&readers); }
# else
#  ifdef __linux__
#   include <linux/futex.h>
#   include <sys/syscall.h>
#   define SRW_FUTEX(a,op,n) \
    syscall(SYS_futex, a, FUTEX_ ## op ## _PRIVATE, n, nullptr, nullptr, 0)
#  elif defined __OpenBSD__
#   include <sys/time.h>
#   include <sys/futex.h>
#   define SRW_FUTEX(a,op,n) \
    futex((volatile uint32_t*) a, FUTEX_ ## op, n, nullptr, nullptr)
#  else
#   error "no futex support"
#  endif

template<bool spinloop>
inline void srw_mutex_impl<spinloop>::wait(uint32_t lk)
{ SRW_FUTEX(&lock, WAIT, lk); }
template<bool spinloop>
void srw_mutex_impl<spinloop>::wake() { SRW_FUTEX(&lock, WAKE, 1); }

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wait(uint32_t lk)
{ SRW_FUTEX(&readers, WAIT, lk); }
template<bool spinloop>
void ssux_lock_impl<spinloop>::wake() { SRW_FUTEX(&readers, WAKE, 1); }

# endif

template void srw_mutex_impl<false>::wake();
template void ssux_lock_impl<false>::wake();
template void srw_mutex_impl<true>::wake();
template void ssux_lock_impl<true>::wake();

/*

Unfortunately, compilers targeting IA-32 or AMD64 currently cannot
translate the following single-bit operations into Intel 80386 instructions:

     m.fetch_or(1<<b) & 1<<b       LOCK BTS b, m
     m.fetch_and(~(1<<b)) & 1<<b   LOCK BTR b, m
     m.fetch_xor(1<<b) & 1<<b      LOCK BTC b, m

Hence, we will manually translate fetch_or() using GCC-style inline
assembler code or a Microsoft intrinsic function.

*/

#if defined __clang_major__ && __clang_major__ < 10
/* Only clang-10 introduced support for asm goto */
#elif defined __GNUC__ && (defined __i386__ || defined __x86_64__)
# define IF_FETCH_OR_GOTO(mem, bit, label)				\
  __asm__ goto("lock btsl $" #bit ", %0\n\t"				\
               "jc %l1" : : "m" (mem) : "cc", "memory" : label);
# define IF_NOT_FETCH_OR_GOTO(mem, bit, label)				\
  __asm__ goto("lock btsl $" #bit ", %0\n\t"				\
               "jnc %l1" : : "m" (mem) : "cc", "memory" : label);
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
# define IF_FETCH_OR_GOTO(mem, bit, label)				\
  if (_interlockedbittestandset(reinterpret_cast<volatile long*>(&mem), bit)) \
    goto label;
# define IF_NOT_FETCH_OR_GOTO(mem, bit, label)				\
  if (!_interlockedbittestandset(reinterpret_cast<volatile long*>(&mem), bit))\
    goto label;
#endif

template<>
void srw_mutex_impl<true>::wait_and_lock()
{
  uint32_t lk= 1 + lock.fetch_add(1, std::memory_order_relaxed);

  const unsigned delay= srw_pause_delay();

  for (auto spin= srv_n_spin_wait_rounds;;)
  {
    DBUG_ASSERT(~HOLDER & lk);
    if (lk & HOLDER)
      lk= lock.load(std::memory_order_relaxed);
    else
    {
#ifdef IF_NOT_FETCH_OR_GOTO
      static_assert(HOLDER == (1U << 31), "compatibility");
      IF_NOT_FETCH_OR_GOTO(*this, 31, acquired);
      lk|= HOLDER;
#else
      if (!((lk= lock.fetch_or(HOLDER, std::memory_order_relaxed)) & HOLDER))
        goto acquired;
#endif
      srw_pause(delay);
    }
    if (!--spin)
      break;
  }

  for (;;)
  {
    DBUG_ASSERT(~HOLDER & lk);
    if (lk & HOLDER)
    {
      wait(lk);
#ifdef IF_FETCH_OR_GOTO
reload:
#endif
      lk= lock.load(std::memory_order_relaxed);
    }
    else
    {
#ifdef IF_FETCH_OR_GOTO
      static_assert(HOLDER == (1U << 31), "compatibility");
      IF_FETCH_OR_GOTO(*this, 31, reload);
#else
      if ((lk= lock.fetch_or(HOLDER, std::memory_order_relaxed)) & HOLDER)
        continue;
      DBUG_ASSERT(lk);
#endif
acquired:
      std::atomic_thread_fence(std::memory_order_acquire);
      return;
    }
  }
}

template<>
void srw_mutex_impl<false>::wait_and_lock()
{
  for (uint32_t lk= 1 + lock.fetch_add(1, std::memory_order_relaxed);;)
  {
    DBUG_ASSERT(~HOLDER & lk);
    if (lk & HOLDER)
    {
      wait(lk);
#ifdef IF_FETCH_OR_GOTO
reload:
#endif
      lk= lock.load(std::memory_order_relaxed);
    }
    else
    {
#ifdef IF_FETCH_OR_GOTO
      static_assert(HOLDER == (1U << 31), "compatibility");
      IF_FETCH_OR_GOTO(*this, 31, reload);
#else
      if ((lk= lock.fetch_or(HOLDER, std::memory_order_relaxed)) & HOLDER)
        continue;
      DBUG_ASSERT(lk);
#endif
      std::atomic_thread_fence(std::memory_order_acquire);
      return;
    }
  }
}

template<bool spinloop>
void ssux_lock_impl<spinloop>::wr_wait(uint32_t lk)
{
  DBUG_ASSERT(writer.is_locked());
  DBUG_ASSERT(lk);
  DBUG_ASSERT(lk < WRITER);

  if (spinloop)
  {
    const unsigned delay= srw_pause_delay();

    for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
    {
      srw_pause(delay);
      lk= readers.load(std::memory_order_acquire);
      if (lk == WRITER)
        return;
      DBUG_ASSERT(lk > WRITER);
    }
  }

  lk|= WRITER;

  do
  {
    DBUG_ASSERT(lk > WRITER);
    wait(lk);
    lk= readers.load(std::memory_order_acquire);
  }
  while (lk != WRITER);
}

template void ssux_lock_impl<true>::wr_wait(uint32_t);
template void ssux_lock_impl<false>::wr_wait(uint32_t);

template<bool spinloop>
void ssux_lock_impl<spinloop>::rd_wait()
{
  for (;;)
  {
    writer.wr_lock();
    bool acquired= rd_lock_try();
    writer.wr_unlock();
    if (acquired)
      break;
  }
}

template void ssux_lock_impl<true>::rd_wait();
template void ssux_lock_impl<false>::rd_wait();
#endif /* SUX_LOCK_GENERIC */

#if defined _WIN32 || defined SUX_LOCK_GENERIC
template<> void srw_lock_<true>::rd_wait()
{
  const unsigned delay= srw_pause_delay();

  for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
  {
    srw_pause(delay);
    if (rd_lock_try())
      return;
  }

  IF_WIN(AcquireSRWLockShared(&lk), rw_rdlock(&lk));
}

template<> void srw_lock_<true>::wr_wait()
{
  const unsigned delay= srw_pause_delay();

  for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
  {
    srw_pause(delay);
    if (wr_lock_try())
      return;
  }

  IF_WIN(AcquireSRWLockExclusive(&lk), rw_wrlock(&lk));
}
#endif

#ifdef UNIV_PFS_RWLOCK
template void srw_lock_impl<false>::psi_rd_lock(const char*, unsigned);
template void srw_lock_impl<false>::psi_wr_lock(const char*, unsigned);
template void srw_lock_impl<true>::psi_rd_lock(const char*, unsigned);
template void srw_lock_impl<true>::psi_wr_lock(const char*, unsigned);

template<bool spinloop>
void srw_lock_impl<spinloop>::psi_rd_lock(const char *file, unsigned line)
{
  PSI_rwlock_locker_state state;
  const bool nowait= lock.rd_lock_try();
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_rdwait)
      (&state, pfs_psi,
       nowait ? PSI_RWLOCK_TRYREADLOCK : PSI_RWLOCK_READLOCK, file, line))
  {
    if (!nowait)
      lock.rd_lock();
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
  else if (!nowait)
    lock.rd_lock();
}

template<bool spinloop>
void srw_lock_impl<spinloop>::psi_wr_lock(const char *file, unsigned line)
{
  PSI_rwlock_locker_state state;
  const bool nowait= lock.wr_lock_try();
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi,
       nowait ? PSI_RWLOCK_TRYWRITELOCK : PSI_RWLOCK_WRITELOCK, file, line))
  {
    if (!nowait)
      lock.wr_lock();
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
  else if (!nowait)
    lock.wr_lock();
}

void ssux_lock::psi_rd_lock(const char *file, unsigned line)
{
  PSI_rwlock_locker_state state;
  const bool nowait= lock.rd_lock_try();
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_rdwait)
      (&state, pfs_psi,
       nowait ? PSI_RWLOCK_TRYSHAREDLOCK : PSI_RWLOCK_SHAREDLOCK, file, line))
  {
    if (!nowait)
      lock.rd_lock();
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
  else if (!nowait)
    lock.rd_lock();
}

void ssux_lock::psi_u_lock(const char *file, unsigned line)
{
  PSI_rwlock_locker_state state;
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi, PSI_RWLOCK_SHAREDEXCLUSIVELOCK, file, line))
  {
    lock.u_lock();
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
  else
    lock.u_lock();
}

void ssux_lock::psi_wr_lock(const char *file, unsigned line)
{
  PSI_rwlock_locker_state state;
  const bool nowait= lock.wr_lock_try();
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi,
       nowait ? PSI_RWLOCK_TRYEXCLUSIVELOCK : PSI_RWLOCK_EXCLUSIVELOCK,
       file, line))
  {
    if (!nowait)
      lock.wr_lock();
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
  else if (!nowait)
    lock.wr_lock();
}

void ssux_lock::psi_u_wr_upgrade(const char *file, unsigned line)
{
  PSI_rwlock_locker_state state;
# ifdef SUX_LOCK_GENERIC
  const bool nowait= lock.upgrade_trylock();
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi,
       nowait ? PSI_RWLOCK_TRYEXCLUSIVELOCK : PSI_RWLOCK_EXCLUSIVELOCK,
       file, line))
  {
    if (!nowait)
      lock.write_lock(true);
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
# else /* SUX_LOCK_GENERIC */
  DBUG_ASSERT(lock.writer.is_locked());
  uint32_t lk= 1;
  const bool nowait=
    lock.readers.compare_exchange_strong(lk, ssux_lock_impl<false>::WRITER,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed);
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi,
       nowait ? PSI_RWLOCK_TRYEXCLUSIVELOCK : PSI_RWLOCK_EXCLUSIVELOCK,
       file, line))
  {
    if (!nowait)
      lock.u_wr_upgrade();
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
# endif /* SUX_LOCK_GENERIC */
  else if (!nowait)
    lock.u_wr_upgrade();
}
#else /* UNIV_PFS_RWLOCK */
template void ssux_lock_impl<false>::rd_lock();
# ifdef SUX_LOCK_GENERIC
template void ssux_lock_impl<false>::write_lock(bool);
template void ssux_lock_impl<false>::update_lock(uint32_t);
# else
template void ssux_lock_impl<false>::rd_unlock();
template void ssux_lock_impl<false>::u_unlock();
template void ssux_lock_impl<false>::wr_unlock();
# endif
#endif /* UNIV_PFS_RWLOCK */
