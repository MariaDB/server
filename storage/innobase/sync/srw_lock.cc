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
#include "my_rdtsc.h"

// TODO MIN_EXP_BACKOFF may need to depend on PAUSE duration.
/** Minimum exponential backoff value, used in spinloops initialization */
constexpr uint32_t MIN_EXP_BACKOFF= 32;
/* MIN_EXP_BACKOFF must be a power of two */
static_assert(
  MIN_EXP_BACKOFF > 0 &&
  (MIN_EXP_BACKOFF & (MIN_EXP_BACKOFF - 1)) == 0,
  "MIN_EXP_BACKOFF must be a power of two");

#ifdef NO_ELISION
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64)
# include <intrin.h>
bool have_transactional_memory;
bool transactional_lock_enabled() noexcept
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
bool transactional_lock_enabled() noexcept
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
bool xtest() noexcept { return have_transactional_memory && _xtest(); }
# endif
#elif defined __powerpc64__ || defined __s390__
# include <htmxlintrin.h>
# include <setjmp.h>
# include <signal.h>

__attribute__((target("htm"),hot))
bool xbegin() noexcept
{
  return have_transactional_memory &&
    __TM_simple_begin() == _HTM_TBEGIN_STARTED;
}

__attribute__((target("htm"),hot))
void xabort() noexcept { __TM_abort(); }

__attribute__((target("htm"),hot))
void xend() noexcept { __TM_end(); }

bool have_transactional_memory;
static sigjmp_buf ill_jmp;
static void ill_handler(int sig) noexcept
{
  siglongjmp(ill_jmp, sig);
}
/**
  Here we are testing we can do a transaction without SIGILL
  and a 1 instruction store can succeed.
*/
__attribute__((noinline))
static void test_tm(bool *r) noexcept
{
  if (__TM_simple_begin() == _HTM_TBEGIN_STARTED)
  {
    *r= true;
    __TM_end();
  }
}
bool transactional_lock_enabled() noexcept
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
bool xtest() noexcept
{
# ifdef __s390x__
  return have_transactional_memory &&
    __builtin_tx_nesting_depth() > 0;
# else
  return have_transactional_memory &&
    _HTM_STATE (__builtin_ttest ()) == _HTM_TRANSACTIONAL;
# endif
}
# endif
#endif

/** @return the parameter for srw_pause() */
static inline unsigned srw_pause_delay() noexcept
{
  return my_cpu_relax_multiplier / 4 * srv_spin_wait_delay;
}

/** Pause the CPU for some time, with no memory accesses. */
static inline void srw_pause(unsigned delay) noexcept
{
  HMT_low();
  while (delay--)
    MY_RELAX_CPU();
  HMT_medium();
}

static inline uint32_t backoff_max_delay() noexcept
{
  /* To avoid overflow in backoff() */
  constexpr uint32_t max_safe_delay= std::numeric_limits<uint32_t>::max() / 4;
  return uint32_t(std::min<uint64_t>(
    uint64_t(srw_pause_delay()) * srv_n_spin_wait_rounds,
    max_safe_delay
  ));
}

static inline uint32_t backoff_jitter() noexcept
{
  return uint32_t(my_timer_cycles());
}

template<bool abort_over_max>
static inline bool backoff(
  uint32_t& exp_backoff,
  const uint32_t max_delay,
  const uint32_t jitter
) noexcept
{
  /* delay = exp_backoff + rand(0, exp_backoff - 1) */
  const uint32_t delay= exp_backoff + (jitter & (exp_backoff - 1));
  const bool over_max= delay > max_delay;
  if (abort_over_max && over_max)
    return false;
  else if (!over_max)
    exp_backoff*= 2;
  // TODO Maybe abort on exp_backoff or total delay greater than something...
  srw_pause(delay);
  return true;
}

#ifndef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
template<> void pthread_mutex_wrapper<true>::wr_wait() noexcept
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
#endif

#ifdef SUX_LOCK_GENERIC
template void ssux_lock_impl<false>::init() noexcept;
template void ssux_lock_impl<true>::init() noexcept;
template void ssux_lock_impl<false>::destroy() noexcept;
template void ssux_lock_impl<true>::destroy() noexcept;

template<bool spinloop>
inline void srw_mutex_impl<spinloop>::wait(uint32_t lk) noexcept
{
  pthread_mutex_lock(&mutex);
  while (lock.load(std::memory_order_relaxed) == lk)
    pthread_cond_wait(&cond, &mutex);
  pthread_mutex_unlock(&mutex);
}

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wait(uint32_t lk) noexcept
{
  pthread_mutex_lock(&writer.mutex);
  while (readers.load(std::memory_order_relaxed) == lk)
    pthread_cond_wait(&readers_cond, &writer.mutex);
  pthread_mutex_unlock(&writer.mutex);
}

template<bool spinloop>
void srw_mutex_impl<spinloop>::wake() noexcept
{
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);
}
template<bool spinloop>
inline void srw_mutex_impl<spinloop>::wake_all() noexcept
{
  pthread_mutex_lock(&mutex);
  pthread_cond_broadcast(&cond);
  pthread_mutex_unlock(&mutex);
}
template<bool spinloop>
void ssux_lock_impl<spinloop>::wake() noexcept
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
inline void srw_mutex_impl<spinloop>::wait(uint32_t lk) noexcept
{ WaitOnAddress(&lock, &lk, 4, INFINITE); }
template<bool spinloop>
void srw_mutex_impl<spinloop>::wake() noexcept { WakeByAddressSingle(&lock); }
template<bool spinloop>
inline void srw_mutex_impl<spinloop>::wake_all() noexcept { WakeByAddressAll(&lock); }

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wait(uint32_t lk) noexcept
{ WaitOnAddress(&readers, &lk, 4, INFINITE); }
template<bool spinloop>
void ssux_lock_impl<spinloop>::wake() noexcept { WakeByAddressSingle(&readers); }
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
inline void srw_mutex_impl<spinloop>::wait(uint32_t lk) noexcept
{ SRW_FUTEX(&lock, WAIT, lk); }
template<bool spinloop>
void srw_mutex_impl<spinloop>::wake() noexcept { SRW_FUTEX(&lock, WAKE, 1); }
template<bool spinloop>
void srw_mutex_impl<spinloop>::wake_all() noexcept { SRW_FUTEX(&lock, WAKE, INT_MAX); }

template<bool spinloop>
inline void ssux_lock_impl<spinloop>::wait(uint32_t lk) noexcept
{ SRW_FUTEX(&readers, WAIT, lk); }
template<bool spinloop>
void ssux_lock_impl<spinloop>::wake() noexcept { SRW_FUTEX(&readers, WAKE, 1); }
# endif
#endif

template void srw_mutex_impl<false>::wake() noexcept;
template void ssux_lock_impl<false>::wake() noexcept;
template void srw_mutex_impl<true>::wake() noexcept;
template void ssux_lock_impl<true>::wake() noexcept;

template<bool spinloop>
void srw_mutex_impl<spinloop>::wait_and_lock() noexcept
{
  uint32_t lk= WAITER + lock.fetch_add(WAITER, std::memory_order_relaxed);

  if (spinloop)
  {
    const uint32_t max_delay= backoff_max_delay();
    const uint32_t jitter= backoff_jitter();
    uint32_t exp_backoff= MIN_EXP_BACKOFF;

    for (;;)
    {
      DBUG_ASSERT(~HOLDER & lk);
      lk= lock.load(std::memory_order_relaxed);
      if (!(lk & HOLDER))
      {
#if defined __i386__||defined __x86_64__||defined _M_IX86||defined _M_X64
        lk |= HOLDER;
# ifdef _MSC_VER
        static_assert(HOLDER == (1U << 0), "compatibility");
        if (!_interlockedbittestandset
            (reinterpret_cast<volatile long*>(&lock), 0))
# else
        if (!(lock.fetch_or(HOLDER, std::memory_order_relaxed) & HOLDER))
# endif
          goto acquired;
#else
        if (!((lk= lock.fetch_or(HOLDER, std::memory_order_relaxed)) & HOLDER))
          goto acquired;
#endif
      }
      if (!backoff<true>(exp_backoff, max_delay, jitter))
        break;
    }
  }

  for (;;)
  {
    DBUG_ASSERT(~HOLDER & lk);
    if (lk & HOLDER)
    {
      wait(lk);
#if defined __i386__||defined __x86_64__||defined _M_IX86||defined _M_X64
reload:
#endif
      lk= lock.load(std::memory_order_relaxed);
    }
    else
    {
#if defined __i386__||defined __x86_64__||defined _M_IX86||defined _M_X64
# ifdef _MSC_VER
      static_assert(HOLDER == (1U << 0), "compatibility");
      if (_interlockedbittestandset
          (reinterpret_cast<volatile long*>(&lock), 0))
# else
      if (lock.fetch_or(HOLDER, std::memory_order_relaxed) & HOLDER)
# endif
        goto reload;
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

template void srw_mutex_impl<false>::wait_and_lock() noexcept;
template void srw_mutex_impl<true>::wait_and_lock() noexcept;

template<bool spinloop>
void ssux_lock_impl<spinloop>::wr_wait(uint32_t lk) noexcept
{
  DBUG_ASSERT(writer.is_locked());
  DBUG_ASSERT(lk);
  DBUG_ASSERT(lk < WRITER);

  if (spinloop)
  {
    const uint32_t max_delay= backoff_max_delay();
    const uint32_t jitter= backoff_jitter();
    uint32_t exp_backoff= MIN_EXP_BACKOFF;

    for (;;)
    {
      if (!backoff<true>(exp_backoff, max_delay, jitter))
        break;
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

template void ssux_lock_impl<true>::wr_wait(uint32_t) noexcept;
template void ssux_lock_impl<false>::wr_wait(uint32_t) noexcept;

template<bool spinloop>
void ssux_lock_impl<spinloop>::rd_wait() noexcept
{
  const uint32_t max_delay= backoff_max_delay();
  const uint32_t jitter= backoff_jitter();
  uint32_t exp_backoff= MIN_EXP_BACKOFF;

  if (spinloop)
  {
    for (;;)
    {
      if (!backoff<true>(exp_backoff, max_delay, jitter))
        break;
      if (rd_lock_try())
        return;
    }
  }

  /* Subscribe to writer.wake() or write.wake_all() calls by
  concurrently executing rd_wait() or writer.wr_unlock(). */
  uint32_t wl= writer.WAITER +
    writer.lock.fetch_add(writer.WAITER, std::memory_order_acquire);

  exp_backoff= MIN_EXP_BACKOFF;
  for (;;)
  {
    if (UNIV_LIKELY(writer.HOLDER & wl))
      writer.wait(wl);
    uint32_t lk= rd_lock_try_low();
    if (!lk)
      break;
    if (UNIV_UNLIKELY(lk == WRITER)) /* A wr_lock() just succeeded. */
      /* Immediately wake up (also) wr_lock(). We may also unnecessarily
      wake up other concurrent threads that are executing rd_wait().
      If we invoked writer.wake() here to wake up just one thread,
      we could wake up a rd_wait(), which then would invoke writer.wake(),
      waking up possibly another rd_wait(), and we could end up doing
      lots of non-productive context switching until the wr_lock()
      is finally woken up. */
      writer.wake_all();
    backoff<false>(exp_backoff, max_delay, jitter);
    wl= writer.lock.load(std::memory_order_acquire);
    ut_ad(wl);
  }

  /* Unsubscribe writer.wake() and writer.wake_all(). */
  wl= writer.lock.fetch_sub(writer.WAITER, std::memory_order_release);
  ut_ad(wl);

  /* Wake any other threads that may be blocked in writer.wait().
  All other waiters than this rd_wait() would end up acquiring writer.lock
  and waking up other threads on unlock(). */
  if (wl > writer.WAITER)
    writer.wake_all();
}

template void ssux_lock_impl<true>::rd_wait() noexcept;
template void ssux_lock_impl<false>::rd_wait() noexcept;

#if defined _WIN32 || defined SUX_LOCK_GENERIC
template<> void srw_lock_<true>::rd_wait() noexcept
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

template<> void srw_lock_<true>::wr_wait() noexcept
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
template void srw_lock_impl<false>::psi_rd_lock(const char*, unsigned) noexcept;
template void srw_lock_impl<false>::psi_wr_lock(const char*, unsigned) noexcept;
template void srw_lock_impl<true>::psi_rd_lock(const char*, unsigned) noexcept;
template void srw_lock_impl<true>::psi_wr_lock(const char*, unsigned) noexcept;

template<bool spinloop>
void srw_lock_impl<spinloop>::psi_rd_lock(const char *file, unsigned line) noexcept
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
void srw_lock_impl<spinloop>::psi_wr_lock(const char *file, unsigned line) noexcept
{
  PSI_rwlock_locker_state state;
# if defined _WIN32 || defined SUX_LOCK_GENERIC
  const bool nowait2= lock.wr_lock_try();
# else
  const bool nowait1= lock.writer.wr_lock_try();
  uint32_t lk= 0;
  const bool nowait2= nowait1 &&
    lock.readers.compare_exchange_strong(lk, lock.WRITER,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed);
# endif
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi,
       nowait2 ? PSI_RWLOCK_TRYWRITELOCK : PSI_RWLOCK_WRITELOCK, file, line))
  {
# if defined _WIN32 || defined SUX_LOCK_GENERIC
    if (!nowait2)
      lock.wr_lock();
# else
    if (!nowait1)
      lock.wr_lock();
    else if (!nowait2)
      lock.u_wr_upgrade();
# endif
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
# if defined _WIN32 || defined SUX_LOCK_GENERIC
  else if (!nowait2)
    lock.wr_lock();
# else
  else if (!nowait1)
    lock.wr_lock();
  else if (!nowait2)
    lock.u_wr_upgrade();
# endif
}

void ssux_lock::psi_rd_lock(const char *file, unsigned line) noexcept
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

void ssux_lock::psi_u_lock(const char *file, unsigned line) noexcept
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

void ssux_lock::psi_wr_lock(const char *file, unsigned line) noexcept
{
  PSI_rwlock_locker_state state;
# if defined _WIN32 || defined SUX_LOCK_GENERIC
  const bool nowait2= lock.wr_lock_try();
# else
  const bool nowait1= lock.writer.wr_lock_try();
  uint32_t lk= 0;
  const bool nowait2= nowait1 &&
    lock.readers.compare_exchange_strong(lk, lock.WRITER,
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed);
# endif
  if (PSI_rwlock_locker *locker= PSI_RWLOCK_CALL(start_rwlock_wrwait)
      (&state, pfs_psi,
       nowait2 ? PSI_RWLOCK_TRYEXCLUSIVELOCK : PSI_RWLOCK_EXCLUSIVELOCK,
       file, line))
  {
# if defined _WIN32 || defined SUX_LOCK_GENERIC
    if (!nowait2)
      lock.wr_lock();
# else
    if (!nowait1)
      lock.wr_lock();
    else if (!nowait2)
      lock.u_wr_upgrade();
# endif
    PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
  }
# if defined _WIN32 || defined SUX_LOCK_GENERIC
  else if (!nowait2)
    lock.wr_lock();
# else
  else if (!nowait1)
    lock.wr_lock();
  else if (!nowait2)
    lock.u_wr_upgrade();
# endif
}

void ssux_lock::psi_u_wr_upgrade(const char *file, unsigned line) noexcept
{
  PSI_rwlock_locker_state state;
  DBUG_ASSERT(lock.writer.is_locked());
  uint32_t lk= 0;
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
template void ssux_lock_impl<false>::rd_lock() noexcept;
template void ssux_lock_impl<false>::rd_unlock() noexcept;
template void ssux_lock_impl<false>::u_unlock() noexcept;
template void ssux_lock_impl<false>::wr_unlock() noexcept;
#endif /* UNIV_PFS_RWLOCK */

#ifdef UNIV_DEBUG
void srw_lock_debug::SRW_LOCK_INIT(mysql_pfs_key_t key) noexcept
{
  srw_lock::SRW_LOCK_INIT(key);
  readers_lock.init();
  ut_ad(!readers.load(std::memory_order_relaxed));
  ut_ad(!have_any());
}

void srw_lock_debug::destroy() noexcept
{
  ut_ad(!writer);
  if (auto r= readers.load(std::memory_order_relaxed))
  {
    readers.store(0, std::memory_order_relaxed);
    ut_ad(r->empty());
    delete r;
  }
  readers_lock.destroy();
  srw_lock::destroy();
}

bool srw_lock_debug::wr_lock_try() noexcept
{
  ut_ad(!have_any());
  if (!srw_lock::wr_lock_try())
    return false;
  ut_ad(!writer);
  writer.store(pthread_self(), std::memory_order_relaxed);
  return true;
}

void srw_lock_debug::wr_lock(SRW_LOCK_ARGS(const char *file, unsigned line)) noexcept
{
  ut_ad(!have_any());
  srw_lock::wr_lock(SRW_LOCK_ARGS(file, line));
  ut_ad(!writer);
  writer.store(pthread_self(), std::memory_order_relaxed);
}

void srw_lock_debug::wr_unlock() noexcept
{
  ut_ad(have_wr());
  writer.store(0, std::memory_order_relaxed);
  srw_lock::wr_unlock();
}

# if defined _WIN32 || defined SUX_LOCK_GENERIC
# else
void srw_lock_debug::wr_rd_downgrade
(SRW_LOCK_ARGS(const char *file, unsigned line)) noexcept
{
  ut_ad(have_wr());
  writer.store(0, std::memory_order_relaxed);
  readers_register();
  srw_lock::wr_rd_downgrade(SRW_LOCK_ARGS(file, line));
}
# endif

void srw_lock_debug::readers_register() noexcept
{
  readers_lock.wr_lock();
  auto r= readers.load(std::memory_order_relaxed);
  if (!r)
  {
    r= new std::unordered_multiset<pthread_t>();
    readers.store(r, std::memory_order_relaxed);
  }
  r->emplace(pthread_self());
  readers_lock.wr_unlock();
}

bool srw_lock_debug::rd_lock_try() noexcept
{
  ut_ad(!have_any());
  if (!srw_lock::rd_lock_try())
    return false;
  readers_register();
  return true;
}

void srw_lock_debug::rd_lock(SRW_LOCK_ARGS(const char *file, unsigned line)) noexcept
{
  ut_ad(!have_any());
  srw_lock::rd_lock(SRW_LOCK_ARGS(file, line));
  readers_register();
}

void srw_lock_debug::rd_unlock() noexcept
{
  const pthread_t self= pthread_self();
  ut_ad(writer != self);
  readers_lock.wr_lock();
  auto r= readers.load(std::memory_order_relaxed);
  ut_ad(r);
  auto i= r->find(self);
  ut_ad(i != r->end());
  r->erase(i);
  readers_lock.wr_unlock();

  srw_lock::rd_unlock();
}

bool srw_lock_debug::have_rd() const noexcept
{
  if (auto r= readers.load(std::memory_order_relaxed))
  {
    readers_lock.wr_lock();
    bool found= r->find(pthread_self()) != r->end();
    readers_lock.wr_unlock();
# ifndef SUX_LOCK_GENERIC
    ut_ad(!found || is_locked());
# endif
    return found;
  }
  return false;
}

bool srw_lock_debug::have_wr() const noexcept
{
  if (writer != pthread_self())
    return false;
# ifndef SUX_LOCK_GENERIC
  ut_ad(is_write_locked());
# endif
  return true;
}

bool srw_lock_debug::have_any() const noexcept
{
  return have_wr() || have_rd();
}
#endif
