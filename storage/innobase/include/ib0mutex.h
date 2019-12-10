/*****************************************************************************

Copyright (c) 2013, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation. All Rights Reserved.

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

/******************************************************************//**
@file include/ib0mutex.h
Policy based mutexes.

Created 2013-03-26 Sunny Bains.
***********************************************************************/

#ifndef UNIV_INNOCHECKSUM

#ifndef ib0mutex_h
#define ib0mutex_h

#include "ut0ut.h"
#include "ut0rnd.h"
#include "os0event.h"
#include "sync0arr.h"

/** OS mutex for tracking lock/unlock for debugging */
template <template <typename> class Policy = NoPolicy>
struct OSTrackMutex {

	typedef Policy<OSTrackMutex> MutexPolicy;

	explicit OSTrackMutex(bool destroy_mutex_at_exit = true)
		UNIV_NOTHROW
	{
		ut_d(m_freed = true);
		ut_d(m_locked = false);
		ut_d(m_destroy_at_exit = destroy_mutex_at_exit);
	}

	~OSTrackMutex() UNIV_NOTHROW
	{
		ut_ad(!m_destroy_at_exit || !m_locked);
	}

	/** Initialise the mutex.
	@param[in]	id              Mutex ID
	@param[in]	filename	File where mutex was created
	@param[in]	line		Line in filename */
	void init(
		latch_id_t	id,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
		ut_ad(m_freed);
		ut_ad(!m_locked);

		m_mutex.init();

		ut_d(m_freed = false);
	}

	/** Destroy the mutex */
	void destroy() UNIV_NOTHROW
	{
		ut_ad(!m_locked);
		ut_ad(!m_freed);

		m_mutex.destroy();

		ut_d(m_freed = true);
	}

	/** Release the mutex. */
	void exit() UNIV_NOTHROW
	{
		ut_ad(m_locked);
		ut_d(m_locked = false);
		ut_ad(!m_freed);

		m_mutex.exit();
	}

	/** Acquire the mutex.
	@param[in]	max_spins	max number of spins
	@param[in]	max_delay	max delay per spin
	@param[in]	filename	from where called
	@param[in]	line		within filename */
	void enter(
		uint32_t	max_spins,
		uint32_t	max_delay,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
		ut_ad(!m_freed);

		m_mutex.enter();

		ut_ad(!m_locked);
		ut_d(m_locked = true);
	}

	/** @return true if locking succeeded */
	bool try_lock() UNIV_NOTHROW
	{
		ut_ad(!m_freed);

		bool	locked = m_mutex.try_lock();

		if (locked) {
			ut_ad(!m_locked);
			ut_d(m_locked = locked);
		}

		return(locked);
	}

	/** @return non-const version of the policy */
	MutexPolicy& policy()
		UNIV_NOTHROW
	{
		return(m_policy);
	}

	/** @return the const version of the policy */
	const MutexPolicy& policy() const
		UNIV_NOTHROW
	{
		return(m_policy);
	}

private:
#ifdef UNIV_DEBUG
	/** true if the mutex has not be initialized */
	bool			m_freed;

	/** true if the mutex has been locked. */
	bool			m_locked;

	/** Do/Dont destroy mutex at exit */
	bool			m_destroy_at_exit;
#endif /* UNIV_DEBUG */

	/** OS Mutex instance */
	OSMutex			m_mutex;

	/** Policy data */
	MutexPolicy		m_policy;
};


#ifdef HAVE_IB_LINUX_FUTEX

#include <linux/futex.h>
#include <sys/syscall.h>

/** Mutex implementation that used the Linux futex. */
template <template <typename> class Policy = NoPolicy>
struct TTASFutexMutex {

	typedef Policy<TTASFutexMutex> MutexPolicy;

	TTASFutexMutex() UNIV_NOTHROW
		:
		m_lock_word(MUTEX_STATE_UNLOCKED)
	{
		/* Check that lock_word is aligned. */
		ut_ad(!((ulint) &m_lock_word % sizeof(ulint)));
	}

	~TTASFutexMutex()
	{
		ut_a(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Called when the mutex is "created". Note: Not from the constructor
	but when the mutex is initialised.
	@param[in]	id		Mutex ID
	@param[in]	filename	File where mutex was created
	@param[in]	line		Line in filename */
	void init(
		latch_id_t	id,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
		ut_a(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Destroy the mutex. */
	void destroy() UNIV_NOTHROW
	{
		/* The destructor can be called at shutdown. */
		ut_a(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Acquire the mutex.
	@param[in]	max_spins	max number of spins
	@param[in]	max_delay	max delay per spin
	@param[in]	filename	from where called
	@param[in]	line		within filename */
	void enter(
		uint32_t	max_spins,
		uint32_t	max_delay,
		const char*	filename,
		uint32_t	line) UNIV_NOTHROW
	{
		uint32_t n_spins, n_waits;

		for (n_spins= 0; n_spins < max_spins; n_spins++) {
			if (try_lock()) {
				m_policy.add(n_spins, 0);
				return;
			}

			ut_delay(max_delay);
		}

		for (n_waits= 0;; n_waits++) {
			if (my_atomic_fas32_explicit(&m_lock_word,
						     MUTEX_STATE_WAITERS,
						     MY_MEMORY_ORDER_ACQUIRE)
			    == MUTEX_STATE_UNLOCKED) {
				break;
			}

			syscall(SYS_futex, &m_lock_word,
				FUTEX_WAIT_PRIVATE, MUTEX_STATE_WAITERS,
				0, 0, 0);
		}

		m_policy.add(n_spins, n_waits);
	}

	/** Release the mutex. */
	void exit() UNIV_NOTHROW
	{
		if (my_atomic_fas32_explicit(&m_lock_word,
					     MUTEX_STATE_UNLOCKED,
					     MY_MEMORY_ORDER_RELEASE)
		    == MUTEX_STATE_WAITERS) {
			syscall(SYS_futex, &m_lock_word, FUTEX_WAKE_PRIVATE,
				1, 0, 0, 0);
		}
	}

	/** Try and lock the mutex.
	@return true if successful */
	bool try_lock() UNIV_NOTHROW
	{
		int32 oldval = MUTEX_STATE_UNLOCKED;
		return(my_atomic_cas32_strong_explicit(&m_lock_word, &oldval,
						       MUTEX_STATE_LOCKED,
						       MY_MEMORY_ORDER_ACQUIRE,
						       MY_MEMORY_ORDER_RELAXED));
	}

	/** @return non-const version of the policy */
	MutexPolicy& policy() UNIV_NOTHROW
	{
		return(m_policy);
	}

	/** @return const version of the policy */
	const MutexPolicy& policy() const UNIV_NOTHROW
	{
		return(m_policy);
	}
private:
	/** Policy data */
	MutexPolicy		m_policy;

	/** lock_word is the target of the atomic test-and-set instruction
	when atomic operations are enabled. */
	int32			m_lock_word;
};

#endif /* HAVE_IB_LINUX_FUTEX */

template <template <typename> class Policy = NoPolicy>
struct TTASMutex {

	typedef Policy<TTASMutex> MutexPolicy;

	TTASMutex() UNIV_NOTHROW
		:
		m_lock_word(MUTEX_STATE_UNLOCKED)
	{
		/* Check that lock_word is aligned. */
		ut_ad(!((ulint) &m_lock_word % sizeof(ulint)));
	}

	~TTASMutex()
	{
		ut_ad(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Called when the mutex is "created". Note: Not from the constructor
	but when the mutex is initialised.
	@param[in]	id		Mutex ID
	@param[in]	filename	File where mutex was created
	@param[in]	line		Line in filename */
	void init(
		latch_id_t	id,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
		ut_ad(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Destroy the mutex. */
	void destroy() UNIV_NOTHROW
	{
		/* The destructor can be called at shutdown. */
		ut_ad(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Try and lock the mutex.
	@return true on success */
	bool try_lock() UNIV_NOTHROW
	{
		int32 oldval = MUTEX_STATE_UNLOCKED;
		return(my_atomic_cas32_strong_explicit(&m_lock_word, &oldval,
						       MUTEX_STATE_LOCKED,
						       MY_MEMORY_ORDER_ACQUIRE,
						       MY_MEMORY_ORDER_RELAXED));
	}

	/** Release the mutex. */
	void exit() UNIV_NOTHROW
	{
		ut_ad(m_lock_word == MUTEX_STATE_LOCKED);
		my_atomic_store32_explicit(&m_lock_word, MUTEX_STATE_UNLOCKED,
					   MY_MEMORY_ORDER_RELEASE);
	}

	/** Acquire the mutex.
	@param max_spins	max number of spins
	@param max_delay	max delay per spin
	@param filename		from where called
	@param line		within filename */
	void enter(
		uint32_t	max_spins,
		uint32_t	max_delay,
		const char*	filename,
		uint32_t	line) UNIV_NOTHROW
	{
		const uint32_t	step = max_spins;
		uint32_t n_spins = 0;

		while (!try_lock()) {
			ut_delay(max_delay);
			if (++n_spins == max_spins) {
				os_thread_yield();
				max_spins+= step;
			}
		}

		m_policy.add(n_spins, 0);
	}

	/** @return non-const version of the policy */
	MutexPolicy& policy() UNIV_NOTHROW
	{
		return(m_policy);
	}

	/** @return const version of the policy */
	const MutexPolicy& policy() const UNIV_NOTHROW
	{
		return(m_policy);
	}

private:
	// Disable copying
	TTASMutex(const TTASMutex&);
	TTASMutex& operator=(const TTASMutex&);

	/** Policy data */
	MutexPolicy		m_policy;

	/** lock_word is the target of the atomic test-and-set instruction
	when atomic operations are enabled. */
	int32			m_lock_word;
};

template <template <typename> class Policy = NoPolicy>
struct TTASEventMutex {

	typedef Policy<TTASEventMutex> MutexPolicy;

	TTASEventMutex()
		UNIV_NOTHROW
		:
		m_lock_word(MUTEX_STATE_UNLOCKED),
		m_event()
	{
		/* Check that lock_word is aligned. */
		ut_ad(!((ulint) &m_lock_word % sizeof(ulint)));
	}

	~TTASEventMutex()
		UNIV_NOTHROW
	{
		ut_ad(m_lock_word == MUTEX_STATE_UNLOCKED);
	}

	/** Called when the mutex is "created". Note: Not from the constructor
	but when the mutex is initialised.
	@param[in]	id		Mutex ID
	@param[in]	filename	File where mutex was created
	@param[in]	line		Line in filename */
	void init(
		latch_id_t	id,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
		ut_a(m_event == 0);
		ut_a(m_lock_word == MUTEX_STATE_UNLOCKED);

		m_event = os_event_create(sync_latch_get_name(id));
	}

	/** This is the real desctructor. This mutex can be created in BSS and
	its desctructor will be called on exit(). We can't call
	os_event_destroy() at that stage. */
	void destroy()
		UNIV_NOTHROW
	{
		ut_ad(m_lock_word == MUTEX_STATE_UNLOCKED);

		/* We have to free the event before InnoDB shuts down. */
		os_event_destroy(m_event);
		m_event = 0;
	}

	/** Try and lock the mutex. Note: POSIX returns 0 on success.
	@return true on success */
	bool try_lock()
		UNIV_NOTHROW
	{
		int32 oldval = MUTEX_STATE_UNLOCKED;
		return(my_atomic_cas32_strong_explicit(&m_lock_word, &oldval,
						       MUTEX_STATE_LOCKED,
						       MY_MEMORY_ORDER_ACQUIRE,
						       MY_MEMORY_ORDER_RELAXED));
	}

	/** Release the mutex. */
	void exit()
		UNIV_NOTHROW
	{
		if (my_atomic_fas32_explicit(&m_lock_word,
					     MUTEX_STATE_UNLOCKED,
					     MY_MEMORY_ORDER_RELEASE)
		    == MUTEX_STATE_WAITERS) {
			os_event_set(m_event);
			sync_array_object_signalled();
		}
	}

	/** Acquire the mutex.
	@param[in]	max_spins	max number of spins
	@param[in]	max_delay	max delay per spin
	@param[in]	filename	from where called
	@param[in]	line		within filename */
	void enter(
		uint32_t	max_spins,
		uint32_t	max_delay,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
		uint32_t	n_spins = 0;
		uint32_t	n_waits = 0;
		const uint32_t	step = max_spins;

		while (!try_lock()) {
			if (n_spins++ == max_spins) {
				max_spins += step;
				n_waits++;
				os_thread_yield();

				sync_cell_t*	cell;
				sync_array_t *sync_arr = sync_array_get_and_reserve_cell(
					this,
					(m_policy.get_id() == LATCH_ID_BUF_BLOCK_MUTEX
					 || m_policy.get_id() == LATCH_ID_BUF_POOL_ZIP)
					? SYNC_BUF_BLOCK
					: SYNC_MUTEX,
					filename, line, &cell);

				int32 oldval = MUTEX_STATE_LOCKED;
				my_atomic_cas32_strong_explicit(&m_lock_word, &oldval,
								MUTEX_STATE_WAITERS,
								MY_MEMORY_ORDER_RELAXED,
								MY_MEMORY_ORDER_RELAXED);

				if (oldval == MUTEX_STATE_UNLOCKED) {
					sync_array_free_cell(sync_arr, cell);
				} else {
					sync_array_wait_event(sync_arr, cell);
				}
			} else {
				ut_delay(max_delay);
			}
		}

		m_policy.add(n_spins, n_waits);
	}

	/** @return the lock state. */
	int32 state() const
		UNIV_NOTHROW
	{
		return(my_atomic_load32_explicit(const_cast<int32*>
						 (&m_lock_word),
						 MY_MEMORY_ORDER_RELAXED));
	}

	/** The event that the mutex will wait in sync0arr.cc
	@return even instance */
	os_event_t event()
		UNIV_NOTHROW
	{
		return(m_event);
	}

	/** @return non-const version of the policy */
	MutexPolicy& policy()
		UNIV_NOTHROW
	{
		return(m_policy);
	}

	/** @return const version of the policy */
	const MutexPolicy& policy() const
		UNIV_NOTHROW
	{
		return(m_policy);
	}

private:
	/** Disable copying */
	TTASEventMutex(const TTASEventMutex&);
	TTASEventMutex& operator=(const TTASEventMutex&);

	/** lock_word is the target of the atomic test-and-set instruction
	when atomic operations are enabled. */
	int32			m_lock_word;

	/** Used by sync0arr.cc for the wait queue */
	os_event_t		m_event;

	/** Policy data */
	MutexPolicy		m_policy;
};

/** Mutex interface for all policy mutexes. This class handles the interfacing
with the Performance Schema instrumentation. */
template <typename MutexImpl>
struct PolicyMutex
{
	typedef MutexImpl MutexType;
	typedef typename MutexImpl::MutexPolicy Policy;

	PolicyMutex() UNIV_NOTHROW : m_impl()
	{
#ifdef UNIV_PFS_MUTEX
		m_ptr = 0;
#endif /* UNIV_PFS_MUTEX */
	}

	~PolicyMutex() { }

	/** @return non-const version of the policy */
	Policy& policy() UNIV_NOTHROW
	{
		return(m_impl.policy());
	}

	/** @return const version of the policy */
	const Policy& policy() const UNIV_NOTHROW
	{
		return(m_impl.policy());
	}

	/** Release the mutex. */
	void exit() UNIV_NOTHROW
	{
#ifdef UNIV_PFS_MUTEX
		pfs_exit();
#endif /* UNIV_PFS_MUTEX */

		policy().release(m_impl);

		m_impl.exit();
	}

	/** Acquire the mutex.
	@param n_spins	max number of spins
	@param n_delay	max delay per spin
	@param name	filename where locked
	@param line	line number where locked */
	void enter(
		uint32_t	n_spins,
		uint32_t	n_delay,
		const char*	name,
		uint32_t	line) UNIV_NOTHROW
	{
#ifdef UNIV_PFS_MUTEX
		/* Note: locker is really an alias for state. That's why
		it has to be in the same scope during pfs_end(). */

		PSI_mutex_locker_state	state;
		PSI_mutex_locker*	locker;

		locker = pfs_begin_lock(&state, name, line);
#endif /* UNIV_PFS_MUTEX */

		policy().enter(m_impl, name, line);

		m_impl.enter(n_spins, n_delay, name, line);

		policy().locked(m_impl, name, line);
#ifdef UNIV_PFS_MUTEX
		pfs_end(locker, 0);
#endif /* UNIV_PFS_MUTEX */
	}

	/** Try and lock the mutex, return 0 on SUCCESS and 1 otherwise.
	@param name	filename where locked
	@param line	line number where locked */
	int trylock(const char* name, uint32_t line) UNIV_NOTHROW
	{
#ifdef UNIV_PFS_MUTEX
		/* Note: locker is really an alias for state. That's why
		it has to be in the same scope during pfs_end(). */

		PSI_mutex_locker_state	state;
		PSI_mutex_locker*	locker;

		locker = pfs_begin_trylock(&state, name, line);
#endif /* UNIV_PFS_MUTEX */

		/* There is a subtlety here, we check the mutex ordering
		after locking here. This is only done to avoid add and
		then remove if the trylock was unsuccesful. */

		int ret = m_impl.try_lock() ? 0 : 1;

		if (ret == 0) {

			policy().enter(m_impl, name, line);

			policy().locked(m_impl, name, line);
		}

#ifdef UNIV_PFS_MUTEX
		pfs_end(locker, 0);
#endif /* UNIV_PFS_MUTEX */

		return(ret);
	}

#ifdef UNIV_DEBUG
	/** @return true if the thread owns the mutex. */
	bool is_owned() const UNIV_NOTHROW
	{
		return(policy().is_owned());
	}
#endif /* UNIV_DEBUG */

	/**
	Initialise the mutex.

	@param[in]	id              Mutex ID
	@param[in]	filename	file where created
	@param[in]	line		line number in file where created */
	void init(
		latch_id_t      id,
		const char*	filename,
		uint32_t	line)
		UNIV_NOTHROW
	{
#ifdef UNIV_PFS_MUTEX
		pfs_add(sync_latch_get_pfs_key(id));
#endif /* UNIV_PFS_MUTEX */

		m_impl.init(id, filename, line);
		policy().init(m_impl, id, filename, line);
	}

	/** Free resources (if any) */
	void destroy() UNIV_NOTHROW
	{
#ifdef UNIV_PFS_MUTEX
		pfs_del();
#endif /* UNIV_PFS_MUTEX */
		m_impl.destroy();
		policy().destroy();
	}

	/** Required for os_event_t */
	operator sys_mutex_t*() UNIV_NOTHROW
	{
		return(m_impl.operator sys_mutex_t*());
	}

#ifdef UNIV_PFS_MUTEX
	/** Performance schema monitoring - register mutex with PFS.

	Note: This is public only because we want to get around an issue
	with registering a subset of buffer pool pages with PFS when
	PFS_GROUP_BUFFER_SYNC is defined. Therefore this has to then
	be called by external code (see buf0buf.cc).

	@param key - Performance Schema key. */
	void pfs_add(mysql_pfs_key_t key) UNIV_NOTHROW
	{
		ut_ad(m_ptr == 0);
		m_ptr = PSI_MUTEX_CALL(init_mutex)(key, this);
	}

private:

	/** Performance schema monitoring.
	@param state - PFS locker state
	@param name - file name where locked
	@param line - line number in file where locked */
	PSI_mutex_locker* pfs_begin_lock(
		PSI_mutex_locker_state*	state,
		const char*		name,
		uint32_t		line) UNIV_NOTHROW
	{
		if (m_ptr != 0) {
			return(PSI_MUTEX_CALL(start_mutex_wait)(
					state, m_ptr,
					PSI_MUTEX_LOCK, name, (uint) line));
		}

		return(0);
	}

	/** Performance schema monitoring.
	@param state - PFS locker state
	@param name - file name where locked
	@param line - line number in file where locked */
	PSI_mutex_locker* pfs_begin_trylock(
		PSI_mutex_locker_state*	state,
		const char*		name,
		uint32_t		line) UNIV_NOTHROW
	{
		if (m_ptr != 0) {
			return(PSI_MUTEX_CALL(start_mutex_wait)(
					state, m_ptr,
					PSI_MUTEX_TRYLOCK, name, (uint) line));
		}

		return(0);
	}

	/** Performance schema monitoring
	@param locker - PFS identifier
	@param ret - 0 for success and 1 for failure */
	void pfs_end(PSI_mutex_locker* locker, int ret) UNIV_NOTHROW
	{
		if (locker != 0) {
			PSI_MUTEX_CALL(end_mutex_wait)(locker, ret);
		}
	}

	/** Performance schema monitoring - register mutex release */
	void pfs_exit()
	{
		if (m_ptr != 0) {
			PSI_MUTEX_CALL(unlock_mutex)(m_ptr);
		}
	}

	/** Performance schema monitoring - deregister */
	void pfs_del()
	{
		if (m_ptr != 0) {
			PSI_MUTEX_CALL(destroy_mutex)(m_ptr);
			m_ptr = 0;
		}
	}
#endif /* UNIV_PFS_MUTEX */

private:
	/** The mutex implementation */
	MutexImpl		m_impl;

#ifdef UNIV_PFS_MUTEX
	/** The performance schema instrumentation hook. */
	PSI_mutex*		m_ptr;
#endif /* UNIV_PFS_MUTEX */

};

#endif /* ib0mutex_h */

#endif /* !UNIV_INNOCHECKSUM */
