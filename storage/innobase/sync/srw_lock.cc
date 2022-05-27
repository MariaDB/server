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
#elif defined __powerpc64__ || defined __s390__
# include <htmxlintrin.h>
# include <setjmp.h>
# include <signal.h>

__attribute__((target("htm"),hot))
bool xbegin()
{
  return have_transactional_memory &&
    __TM_simple_begin() == _HTM_TBEGIN_STARTED;
}

__attribute__((target("htm"),hot))
void xabort() { __TM_abort(); }

__attribute__((target("htm"),hot))
void xend() { __TM_end(); }

bool have_transactional_memory;
static sigjmp_buf ill_jmp;
static void ill_handler(int sig)
{
  siglongjmp(ill_jmp, sig);
}
/**
  Here we are testing we can do a transaction without SIGILL
  and a 1 instruction store can succeed.
*/
__attribute__((noinline))
static void test_tm(bool *r)
{
  if (__TM_simple_begin() == _HTM_TBEGIN_STARTED)
  {
    *r= true;
    __TM_end();
  }
}
bool transactional_lock_enabled()
{
  bool r= false;
  sigset_t oset;
  struct sigaction ill_act, oact_ill;

  memset(&ill_act, 0, sizeof(ill_act));
  ill_act.sa_handler = ill_handler;
  sigfillset(&ill_act.sa_mask);
  sigdelset(&ill_act.sa_mask, SIGILL);

  sigprocmask(SIG_SETMASK, &ill_act.sa_mask, &oset);
  sigaction(SIGILL, &ill_act, &oact_ill);
  if (sigsetjmp(ill_jmp, 1) == 0)
  {
    test_tm(&r);
  }
  sigaction(SIGILL, &oact_ill, NULL);
  sigprocmask(SIG_SETMASK, &oset, NULL);
  return r;
}

# ifdef UNIV_DEBUG
__attribute__((target("htm"),hot))
bool xtest()
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
# ifndef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
template<> void pthread_mutex_wrapper<true>::wr_wait()
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
# endif

template void ssux_lock_impl<false>::init();
template void ssux_lock_impl<true>::init();
template void ssux_lock_impl<false>::destroy();
template void ssux_lock_impl<true>::destroy();

template<bool spinloop>
inline void srw_mutex_impl<spinloop>::wait(uint32_t lk)
{
  pthread_mutex_lock(&mutex);
  while (lock.load(std::memory_order_relaxed) == lk)
    pthread_cond_wait(&cond, &mutex);
  pthread_mutex_unlock(&mutex);
}

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wait(uint32_t lk)
{
  pthread_mutex_lock(&writer.mutex);
  while (readers.load(std::memory_order_relaxed) == lk)
    pthread_cond_wait(&readers_cond, &writer.mutex);
  pthread_mutex_unlock(&writer.mutex);
}

template<bool spinloop>
void srw_mutex_impl<spinloop>::wake()
{
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
}
template<bool spinloop>
void ssux_lock_impl<spinloop>::wake()
{
  pthread_mutex_lock(&writer.mutex);
  pthread_cond_signal(&readers_cond);
  pthread_mutex_unlock(&writer.mutex);
}
#else
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
#  elif defined __FreeBSD__
#   include <sys/types.h>
#   include <sys/umtx.h>
#   define FUTEX_WAKE UMTX_OP_WAKE_PRIVATE
#   define FUTEX_WAIT UMTX_OP_WAIT_UINT_PRIVATE
#   define SRW_FUTEX(a,op,n) _umtx_op(a, FUTEX_ ## op, n, nullptr, nullptr)
#  elif defined __DragonFly__
#   include <unistd.h>
#   define FUTEX_WAKE(a,n) umtx_wakeup(a,n)
#   define FUTEX_WAIT(a,n) umtx_sleep(a,n,0)
#   define SRW_FUTEX(a,op,n) FUTEX_ ## op((volatile int*) a, int(n))
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
#endif

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
#elif defined __APPLE__
/* At least some versions of Apple Xcode do not support asm goto */
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

template<bool spinloop>
void srw_mutex_impl<spinloop>::wait_and_lock()
{
  uint32_t lk= 1 + lock.fetch_add(1, std::memory_order_relaxed);

  if (spinloop)
  {
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

template void srw_mutex_impl<false>::wait_and_lock();
template void srw_mutex_impl<true>::wait_and_lock();

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
  else if (!nowait)
    lock.u_wr_upgrade();
}
#else /* UNIV_PFS_RWLOCK */
template void ssux_lock_impl<false>::rd_lock();
template void ssux_lock_impl<false>::rd_unlock();
template void ssux_lock_impl<false>::u_unlock();
template void ssux_lock_impl<false>::wr_unlock();
#endif /* UNIV_PFS_RWLOCK */
