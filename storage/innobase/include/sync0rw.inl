/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2017, 2020, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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

/**************************************************//**
@file include/sync0rw.ic
The read-write lock (for threads)

Created 9/11/1995 Heikki Tuuri
*******************************************************/

#include "os0event.h"

/******************************************************************//**
Lock an rw-lock in shared mode for the current thread. If the rw-lock is
locked in exclusive mode, or there is an exclusive lock request waiting,
the function spins a preset time (controlled by srv_n_spin_wait_rounds),
waiting for the lock before suspending the thread. */
void
rw_lock_s_lock_spin(
/*================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				be passed to another thread to unlock */
	const char*	file_name,/*!< in: file name where lock requested */
	unsigned	line);	/*!< in: line where requested */
#ifdef UNIV_DEBUG
/******************************************************************//**
Inserts the debug information for an rw-lock. */
void
rw_lock_add_debug_info(
/*===================*/
	rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		pass,		/*!< in: pass value */
	ulint		lock_type,	/*!< in: lock type */
	const char*	file_name,	/*!< in: file where requested */
	unsigned	line);		/*!< in: line where requested */
/******************************************************************//**
Removes a debug information struct for an rw-lock. */
void
rw_lock_remove_debug_info(
/*======================*/
	rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		pass,		/*!< in: pass value */
	ulint		lock_type);	/*!< in: lock type */
#endif /* UNIV_DEBUG */

/******************************************************************//**
Returns the write-status of the lock - this function made more sense
with the old rw_lock implementation.
@return RW_LOCK_NOT_LOCKED, RW_LOCK_X, RW_LOCK_X_WAIT, RW_LOCK_SX */
UNIV_INLINE
ulint
rw_lock_get_writer(
/*===============*/
	const rw_lock_t*	lock)	/*!< in: rw-lock */
{
	int32_t lock_word = lock->lock_word;

	ut_ad(lock_word <= X_LOCK_DECR);
	if (lock_word > X_LOCK_HALF_DECR) {
		/* return NOT_LOCKED in s-lock state, like the writer
		member of the old lock implementation. */
		return(RW_LOCK_NOT_LOCKED);
	} else if (lock_word > 0) {
		/* sx-locked, no x-locks */
		return(RW_LOCK_SX);
	} else if (lock_word == 0
		   || lock_word == -X_LOCK_HALF_DECR
		   || lock_word <= -X_LOCK_DECR) {
		/* x-lock with sx-lock is also treated as RW_LOCK_EX */
		return(RW_LOCK_X);
	} else {
		/* x-waiter with sx-lock is also treated as RW_LOCK_WAIT_EX
		e.g. -X_LOCK_HALF_DECR < lock_word < 0 : without sx
		     -X_LOCK_DECR < lock_word < -X_LOCK_HALF_DECR : with sx */
		return(RW_LOCK_X_WAIT);
	}
}

/******************************************************************//**
Returns the number of readers (s-locks).
@return number of readers */
UNIV_INLINE
ulint
rw_lock_get_reader_count(
/*=====================*/
	const rw_lock_t*	lock)	/*!< in: rw-lock */
{
	int32_t lock_word = lock->lock_word;
	ut_ad(lock_word <= X_LOCK_DECR);

	if (lock_word > X_LOCK_HALF_DECR) {
		/* s-locked, no x-waiter */
		return ulint(X_LOCK_DECR - lock_word);
	} else if (lock_word > 0) {
		/* s-locked, with sx-locks only */
		return ulint(X_LOCK_HALF_DECR - lock_word);
	} else if (lock_word == 0) {
		/* x-locked */
		return(0);
	} else if (lock_word > -X_LOCK_HALF_DECR) {
		/* s-locked, with x-waiter */
		return((ulint)(-lock_word));
	} else if (lock_word == -X_LOCK_HALF_DECR) {
		/* x-locked with sx-locks */
		return(0);
	} else if (lock_word > -X_LOCK_DECR) {
		/* s-locked, with x-waiter and sx-lock */
		return((ulint)(-(lock_word + X_LOCK_HALF_DECR)));
	}
	/* no s-locks */
	return(0);
}

/******************************************************************//**
Returns the value of writer_count for the lock. Does not reserve the lock
mutex, so the caller must be sure it is not changed during the call.
@return value of writer_count */
UNIV_INLINE
ulint
rw_lock_get_x_lock_count(
/*=====================*/
	const rw_lock_t*	lock)	/*!< in: rw-lock */
{
	int32_t lock_copy = lock->lock_word;
	ut_ad(lock_copy <= X_LOCK_DECR);

	if (lock_copy == 0 || lock_copy == -X_LOCK_HALF_DECR) {
		/* "1 x-lock" or "1 x-lock + sx-locks" */
		return(1);
	} else if (lock_copy > -X_LOCK_DECR) {
		/* s-locks, one or more sx-locks if > 0, or x-waiter if < 0 */
		return(0);
	} else if (lock_copy > -(X_LOCK_DECR + X_LOCK_HALF_DECR)) {
		/* no s-lock, no sx-lock, 2 or more x-locks.
		First 2 x-locks are set with -X_LOCK_DECR,
		all other recursive x-locks are set with -1 */
		return ulint(2 - X_LOCK_DECR - lock_copy);
	} else {
		/* no s-lock, 1 or more sx-lock, 2 or more x-locks.
		First 2 x-locks are set with -(X_LOCK_DECR + X_LOCK_HALF_DECR),
		all other recursive x-locks are set with -1 */
		return ulint(2 - X_LOCK_DECR - X_LOCK_HALF_DECR - lock_copy);
	}
}

/******************************************************************//**
Returns the number of sx-lock for the lock. Does not reserve the lock
mutex, so the caller must be sure it is not changed during the call.
@return value of sx-lock count */
UNIV_INLINE
ulint
rw_lock_get_sx_lock_count(
/*======================*/
	const rw_lock_t*	lock)	/*!< in: rw-lock */
{
#ifdef UNIV_DEBUG
	int32_t lock_copy = lock->lock_word;

	ut_ad(lock_copy <= X_LOCK_DECR);

	while (lock_copy < 0) {
		lock_copy += X_LOCK_DECR;
	}

	if (lock_copy > 0 && lock_copy <= X_LOCK_HALF_DECR) {
		return(lock->sx_recursive);
	}

	return(0);
#else /* UNIV_DEBUG */
	return(lock->sx_recursive);
#endif /* UNIV_DEBUG */
}

/******************************************************************//**
Recursive x-locks are not supported: they should be handled by the caller and
need not be atomic since they are performed by the current lock holder.
Returns true if the decrement was made, false if not.
@return true if decr occurs */
UNIV_INLINE
bool
rw_lock_lock_word_decr(
/*===================*/
	rw_lock_t*	lock,		/*!< in/out: rw-lock */
	int32_t		amount,		/*!< in: amount to decrement */
	int32_t		threshold)	/*!< in: threshold of judgement */
{
	int32_t lock_copy = lock->lock_word;

	while (lock_copy > threshold) {
		if (lock->lock_word.compare_exchange_strong(
			lock_copy,
			lock_copy - amount,
			std::memory_order_acquire,
			std::memory_order_relaxed)) {

			return(true);
		}
	}
	return(false);
}

/******************************************************************//**
Low-level function which tries to lock an rw-lock in s-mode. Performs no
spinning.
@return TRUE if success */
UNIV_INLINE
ibool
rw_lock_s_lock_low(
/*===============*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass MY_ATTRIBUTE((unused)),
				/*!< in: pass value; != 0, if the lock will be
				passed to another thread to unlock */
	const char*	file_name, /*!< in: file name where lock requested */
	unsigned	line)	/*!< in: line where requested */
{
	if (!rw_lock_lock_word_decr(lock, 1, 0)) {
		/* Locking did not succeed */
		return(FALSE);
	}

	ut_d(rw_lock_add_debug_info(lock, pass, RW_LOCK_S, file_name, line));

	return(TRUE);	/* locking succeeded */
}

/******************************************************************//**
NOTE! Use the corresponding macro, not directly this function! Lock an
rw-lock in shared mode for the current thread. If the rw-lock is locked
in exclusive mode, or there is an exclusive lock request waiting, the
function spins a preset time (controlled by srv_n_spin_wait_rounds), waiting for
the lock, before suspending the thread. */
UNIV_INLINE
void
rw_lock_s_lock_func(
/*================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				be passed to another thread to unlock */
	const char*	file_name,/*!< in: file name where lock requested */
	unsigned	line)	/*!< in: line where requested */
{
	/* NOTE: As we do not know the thread ids for threads which have
	s-locked a latch, and s-lockers will be served only after waiting
	x-lock requests have been fulfilled, then if this thread already
	owns an s-lock here, it may end up in a deadlock with another thread
	which requests an x-lock here. Therefore, we will forbid recursive
	s-locking of a latch: the following assert will warn the programmer
	of the possibility of this kind of a deadlock. If we want to implement
	safe recursive s-locking, we should keep in a list the thread ids of
	the threads which have s-locked a latch. This would use some CPU
	time. */

	ut_ad(!rw_lock_own_flagged(lock, RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	if (!rw_lock_s_lock_low(lock, pass, file_name, line)) {

		/* Did not succeed, try spin wait */

		rw_lock_s_lock_spin(lock, pass, file_name, line);
	}
}

/******************************************************************//**
NOTE! Use the corresponding macro, not directly this function! Lock an
rw-lock in exclusive mode for the current thread if the lock can be
obtained immediately.
@return TRUE if success */
UNIV_INLINE
ibool
rw_lock_x_lock_func_nowait(
/*=======================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	const char*	file_name,/*!< in: file name where lock requested */
	unsigned	line)	/*!< in: line where requested */
{
	int32_t oldval = X_LOCK_DECR;

	if (lock->lock_word.compare_exchange_strong(oldval, 0,
						std::memory_order_acquire,
						std::memory_order_relaxed)) {
		lock->writer_thread = os_thread_get_curr_id();

	} else if (os_thread_eq(lock->writer_thread, os_thread_get_curr_id())) {
		/* Relock: even though no other thread can modify (lock, unlock
		or reserve) lock_word while there is an exclusive writer and
		this is the writer thread, we still want concurrent threads to
		observe consistent values. */
		if (oldval == 0 || oldval == -X_LOCK_HALF_DECR) {
			/* There are 1 x-locks */
			lock->lock_word.fetch_sub(X_LOCK_DECR,
						  std::memory_order_relaxed);
		} else if (oldval <= -X_LOCK_DECR) {
			/* There are 2 or more x-locks */
			lock->lock_word.fetch_sub(1,
						  std::memory_order_relaxed);
			/* Watch for too many recursive locks */
			ut_ad(oldval < 1);
		} else {
			/* Failure */
			return(FALSE);
		}
	} else {
		/* Failure */
		return(FALSE);
	}

	ut_d(rw_lock_add_debug_info(lock, 0, RW_LOCK_X, file_name, line));

	lock->last_x_file_name = file_name;
	lock->last_x_line = line;

	ut_ad(rw_lock_validate(lock));

	return(TRUE);
}

/******************************************************************//**
Releases a shared mode lock. */
UNIV_INLINE
void
rw_lock_s_unlock_func(
/*==================*/
#ifdef UNIV_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the lock may have
				been passed to another thread to unlock */
#endif /* UNIV_DEBUG */
	rw_lock_t*	lock)	/*!< in/out: rw-lock */
{
	ut_d(rw_lock_remove_debug_info(lock, pass, RW_LOCK_S));

	/* Increment lock_word to indicate 1 less reader */
	int32_t lock_word = lock->lock_word.fetch_add(
		1, std::memory_order_release);

	if (lock_word == -1 || lock_word == -X_LOCK_HALF_DECR - 1) {
		/* wait_ex waiter exists. It may not be asleep, but we signal
		anyway. We do not wake other waiters, because they can't
		exist without wait_ex waiter and wait_ex waiter goes first.*/
		os_event_set(lock->wait_ex_event);
		sync_array_object_signalled();
	} else {
		ut_ad(lock_word > -X_LOCK_DECR);
		ut_ad(lock_word < X_LOCK_DECR);
	}

	ut_ad(rw_lock_validate(lock));
}

/******************************************************************//**
Releases an exclusive mode lock. */
UNIV_INLINE
void
rw_lock_x_unlock_func(
/*==================*/
#ifdef UNIV_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the lock may have
				been passed to another thread to unlock */
#endif /* UNIV_DEBUG */
	rw_lock_t*	lock)	/*!< in/out: rw-lock */
{
	int32_t lock_word = lock->lock_word;

	if (lock_word == 0) {
		/* Last caller in a possible recursive chain. */
		lock->writer_thread = 0;
	}

	ut_d(rw_lock_remove_debug_info(lock, pass, RW_LOCK_X));

	if (lock_word == 0 || lock_word == -X_LOCK_HALF_DECR) {
		/* Last X-lock owned by this thread, it may still hold SX-locks.
		ACQ_REL due to...
		RELEASE: we release rw-lock
		ACQUIRE: we want waiters to be loaded after lock_word is stored */
		lock->lock_word.fetch_add(X_LOCK_DECR,
					  std::memory_order_acq_rel);

		/* This no longer has an X-lock but it may still have
		an SX-lock. So it is now free for S-locks by other threads.
		We need to signal read/write waiters.
		We do not need to signal wait_ex waiters, since they cannot
		exist when there is a writer. */
		if (lock->waiters) {
			lock->waiters = 0;
			os_event_set(lock->event);
			sync_array_object_signalled();
		}
	} else if (lock_word == -X_LOCK_DECR
		   || lock_word == -(X_LOCK_DECR + X_LOCK_HALF_DECR)) {
		/* There are 2 x-locks */
		lock->lock_word.fetch_add(X_LOCK_DECR);
	} else {
		/* There are more than 2 x-locks. */
		ut_ad(lock_word < -X_LOCK_DECR);
		lock->lock_word.fetch_add(1);
	}

	ut_ad(rw_lock_validate(lock));
}

/******************************************************************//**
Releases a sx mode lock. */
UNIV_INLINE
void
rw_lock_sx_unlock_func(
/*===================*/
#ifdef UNIV_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the lock may have
				been passed to another thread to unlock */
#endif /* UNIV_DEBUG */
	rw_lock_t*	lock)	/*!< in/out: rw-lock */
{
	ut_ad(rw_lock_get_sx_lock_count(lock));
	ut_ad(lock->sx_recursive > 0);

	--lock->sx_recursive;

	ut_d(rw_lock_remove_debug_info(lock, pass, RW_LOCK_SX));

	if (lock->sx_recursive == 0) {
		int32_t lock_word = lock->lock_word;
		/* Last caller in a possible recursive chain. */
		if (lock_word > 0) {
			lock->writer_thread = 0;
			ut_ad(lock_word <= INT_MAX32 - X_LOCK_HALF_DECR);

			/* Last SX-lock owned by this thread, doesn't own X-lock.
			ACQ_REL due to...
			RELEASE: we release rw-lock
			ACQUIRE: we want waiters to be loaded after lock_word is stored */
			lock->lock_word.fetch_add(X_LOCK_HALF_DECR,
						  std::memory_order_acq_rel);

			/* Lock is now free. May have to signal read/write
			waiters. We do not need to signal wait_ex waiters,
			since they cannot exist when there is an sx-lock
			holder. */
			if (lock->waiters) {
				lock->waiters = 0;
				os_event_set(lock->event);
				sync_array_object_signalled();
			}
		} else {
			/* still has x-lock */
			ut_ad(lock_word == -X_LOCK_HALF_DECR ||
			      lock_word <= -(X_LOCK_DECR + X_LOCK_HALF_DECR));
			lock->lock_word.fetch_add(X_LOCK_HALF_DECR);
		}
	}

	ut_ad(rw_lock_validate(lock));
}

#ifdef UNIV_PFS_RWLOCK

/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_create_func().
NOTE! Please use the corresponding macro rw_lock_create(), not directly
this function! */
UNIV_INLINE
void
pfs_rw_lock_create_func(
/*====================*/
	mysql_pfs_key_t	key,		/*!< in: key registered with
					performance schema */
	rw_lock_t*	lock,		/*!< in/out: pointer to memory */
# ifdef UNIV_DEBUG
	latch_level_t	level,		/*!< in: level */
# endif /* UNIV_DEBUG */
	const char*	cfile_name,	/*!< in: file name where created */
	unsigned	cline)		/*!< in: file line where created */
{
	ut_d(new(lock) rw_lock_t());

	/* Initialize the rwlock for performance schema */
	lock->pfs_psi = PSI_RWLOCK_CALL(init_rwlock)(key, lock);

	/* The actual function to initialize an rwlock */
	rw_lock_create_func(lock,
#ifdef UNIV_DEBUG
			    level,
#endif /* UNIV_DEBUG */
			    cfile_name,
			    cline);
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_x_lock_func()
NOTE! Please use the corresponding macro rw_lock_x_lock(), not directly
this function! */
UNIV_INLINE
void
pfs_rw_lock_x_lock_func(
/*====================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				be passed to another thread to unlock */
	const char*	file_name,/*!< in: file name where lock requested */
	unsigned	line)	/*!< in: line where requested */
{
	if (lock->pfs_psi != NULL) {
		PSI_rwlock_locker*	locker;
		PSI_rwlock_locker_state	state;

		/* Record the acquisition of a read-write lock in exclusive
		mode in performance schema */
/* MySQL 5.7 New PSI */
#define PSI_RWLOCK_EXCLUSIVELOCK PSI_RWLOCK_WRITELOCK

		locker = PSI_RWLOCK_CALL(start_rwlock_wrwait)(
			&state, lock->pfs_psi, PSI_RWLOCK_EXCLUSIVELOCK,
			file_name, static_cast<uint>(line));

		rw_lock_x_lock_func(
			lock, pass, file_name, static_cast<uint>(line));

		if (locker != NULL) {
			PSI_RWLOCK_CALL(end_rwlock_wrwait)(locker, 0);
		}
	} else {
		rw_lock_x_lock_func(lock, pass, file_name, line);
	}
}
/******************************************************************//**
Performance schema instrumented wrap function for
rw_lock_x_lock_func_nowait()
NOTE! Please use the corresponding macro rw_lock_x_lock_func(),
not directly this function!
@return TRUE if success */
UNIV_INLINE
ibool
pfs_rw_lock_x_lock_func_nowait(
/*===========================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	const char*	file_name,/*!< in: file name where lock
				requested */
	unsigned	line)	/*!< in: line where requested */
{
	ibool		ret;

	if (lock->pfs_psi != NULL) {
		PSI_rwlock_locker*	locker;
		PSI_rwlock_locker_state	state;

		/* Record the acquisition of a read-write trylock in exclusive
		mode in performance schema */

#define PSI_RWLOCK_TRYEXCLUSIVELOCK PSI_RWLOCK_TRYWRITELOCK
		locker = PSI_RWLOCK_CALL(start_rwlock_wrwait)(
			&state, lock->pfs_psi, PSI_RWLOCK_TRYEXCLUSIVELOCK,
			file_name, static_cast<uint>(line));

		ret = rw_lock_x_lock_func_nowait(lock, file_name, line);

		if (locker != NULL) {
			PSI_RWLOCK_CALL(end_rwlock_wrwait)(
				locker, static_cast<int>(ret));
		}
	} else {
		ret = rw_lock_x_lock_func_nowait(lock, file_name, line);
	}

	return(ret);
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_free_func()
NOTE! Please use the corresponding macro rw_lock_free(), not directly
this function! */
UNIV_INLINE
void
pfs_rw_lock_free_func(
/*==================*/
	rw_lock_t*	lock)	/*!< in: pointer to rw-lock */
{
	if (lock->pfs_psi != NULL) {
		PSI_RWLOCK_CALL(destroy_rwlock)(lock->pfs_psi);
		lock->pfs_psi = NULL;
	}

	rw_lock_free_func(lock);
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_s_lock_func()
NOTE! Please use the corresponding macro rw_lock_s_lock(), not
directly this function! */
UNIV_INLINE
void
pfs_rw_lock_s_lock_func(
/*====================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock will be passed to another
				thread to unlock */
	const char*	file_name,/*!< in: file name where lock
				requested */
	unsigned	line)	/*!< in: line where requested */
{
	if (lock->pfs_psi != NULL) {
		PSI_rwlock_locker*	locker;
		PSI_rwlock_locker_state	state;

#define  PSI_RWLOCK_SHAREDLOCK  PSI_RWLOCK_READLOCK
		/* Instrumented to inform we are aquiring a shared rwlock */
		locker = PSI_RWLOCK_CALL(start_rwlock_rdwait)(
			&state, lock->pfs_psi, PSI_RWLOCK_SHAREDLOCK,
			file_name, static_cast<uint>(line));

		rw_lock_s_lock_func(lock, pass, file_name, line);

		if (locker != NULL) {
			PSI_RWLOCK_CALL(end_rwlock_rdwait)(locker, 0);
		}
	} else {
		rw_lock_s_lock_func(lock, pass, file_name, line);
	}
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_sx_lock_func()
NOTE! Please use the corresponding macro rw_lock_sx_lock(), not
directly this function! */
UNIV_INLINE
void
pfs_rw_lock_sx_lock_func(
/*====================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock will be passed to another
				thread to unlock */
	const char*	file_name,/*!< in: file name where lock
				requested */
	unsigned	line)	/*!< in: line where requested */
{
	if (lock->pfs_psi != NULL) {
		PSI_rwlock_locker*	locker;
		PSI_rwlock_locker_state	state;

#define PSI_RWLOCK_SHAREDEXCLUSIVELOCK PSI_RWLOCK_WRITELOCK
		/* Instrumented to inform we are aquiring a shared rwlock */
		locker = PSI_RWLOCK_CALL(start_rwlock_wrwait)(
			&state, lock->pfs_psi, PSI_RWLOCK_SHAREDEXCLUSIVELOCK,
			file_name, static_cast<uint>(line));

		rw_lock_sx_lock_func(lock, pass, file_name, line);

		if (locker != NULL) {
			PSI_RWLOCK_CALL(end_rwlock_wrwait)(locker, 0);
		}
	} else {
		rw_lock_sx_lock_func(lock, pass, file_name, line);
	}
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_s_lock_func()
NOTE! Please use the corresponding macro rw_lock_s_lock(), not
directly this function!
@return TRUE if success */
UNIV_INLINE
ibool
pfs_rw_lock_s_lock_low(
/*===================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock will be passed to another
				thread to unlock */
	const char*	file_name, /*!< in: file name where lock requested */
	unsigned	line)	/*!< in: line where requested */
{
	ibool		ret;

	if (lock->pfs_psi != NULL) {
		PSI_rwlock_locker*	locker;
		PSI_rwlock_locker_state	state;

#define PSI_RWLOCK_TRYSHAREDLOCK PSI_RWLOCK_TRYREADLOCK
		/* Instrumented to inform we are aquiring a shared rwlock */
		locker = PSI_RWLOCK_CALL(start_rwlock_rdwait)(
			&state, lock->pfs_psi, PSI_RWLOCK_TRYSHAREDLOCK,
			file_name, static_cast<uint>(line));

		ret = rw_lock_s_lock_low(lock, pass, file_name, line);

		if (locker != NULL) {
			PSI_RWLOCK_CALL(end_rwlock_rdwait)(
				locker, static_cast<int>(ret));
		}
	} else {
		ret = rw_lock_s_lock_low(lock, pass, file_name, line);
	}

	return(ret);
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_sx_lock_nowait()
NOTE! Please use the corresponding macro, not
directly this function!
@return TRUE if success */
UNIV_INLINE
ibool
pfs_rw_lock_sx_lock_low(
/*====================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock will be passed to another
				thread to unlock */
	const char*	file_name, /*!< in: file name where lock requested */
	unsigned	line)	/*!< in: line where requested */
{
	ibool		ret;

	if (lock->pfs_psi != NULL) {
		PSI_rwlock_locker*	locker;
		PSI_rwlock_locker_state	state;

#define PSI_RWLOCK_TRYSHAREDEXCLUSIVELOCK PSI_RWLOCK_TRYWRITELOCK
		/* Instrumented to inform we are aquiring a shared
		exclusive rwlock */
		locker = PSI_RWLOCK_CALL(start_rwlock_rdwait)(
			&state, lock->pfs_psi,
			PSI_RWLOCK_TRYSHAREDEXCLUSIVELOCK,
			file_name, static_cast<uint>(line));

		ret = rw_lock_sx_lock_low(lock, pass, file_name, line);

		if (locker != NULL) {
			PSI_RWLOCK_CALL(end_rwlock_rdwait)(
				locker, static_cast<int>(ret));
		}
	} else {
		ret = rw_lock_sx_lock_low(lock, pass, file_name, line);
	}

	return(ret);
}
/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_x_unlock_func()
NOTE! Please use the corresponding macro rw_lock_x_unlock(), not directly
this function! */
UNIV_INLINE
void
pfs_rw_lock_x_unlock_func(
/*======================*/
#ifdef UNIV_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock may have been passed to another
				thread to unlock */
#endif /* UNIV_DEBUG */
	rw_lock_t*	lock)	/*!< in/out: rw-lock */
{
	/* Inform performance schema we are unlocking the lock */
	if (lock->pfs_psi != NULL) {
		PSI_RWLOCK_CALL(unlock_rwlock)(lock->pfs_psi);
	}

	rw_lock_x_unlock_func(
#ifdef UNIV_DEBUG
		pass,
#endif /* UNIV_DEBUG */
		lock);
}

/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_sx_unlock_func()
NOTE! Please use the corresponding macro rw_lock_sx_unlock(), not directly
this function! */
UNIV_INLINE
void
pfs_rw_lock_sx_unlock_func(
/*======================*/
#ifdef UNIV_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock may have been passed to another
				thread to unlock */
#endif /* UNIV_DEBUG */
	rw_lock_t*	lock)	/*!< in/out: rw-lock */
{
	/* Inform performance schema we are unlocking the lock */
	if (lock->pfs_psi != NULL) {
		PSI_RWLOCK_CALL(unlock_rwlock)(lock->pfs_psi);
	}

	rw_lock_sx_unlock_func(
#ifdef UNIV_DEBUG
		pass,
#endif /* UNIV_DEBUG */
		lock);
}

/******************************************************************//**
Performance schema instrumented wrap function for rw_lock_s_unlock_func()
NOTE! Please use the corresponding macro pfs_rw_lock_s_unlock(), not
directly this function! */
UNIV_INLINE
void
pfs_rw_lock_s_unlock_func(
/*======================*/
#ifdef UNIV_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the
				lock may have been passed to another
				thread to unlock */
#endif /* UNIV_DEBUG */
	rw_lock_t*	lock)	/*!< in/out: rw-lock */
{
	/* Inform performance schema we are unlocking the lock */
	if (lock->pfs_psi != NULL) {
		PSI_RWLOCK_CALL(unlock_rwlock)(lock->pfs_psi);
	}

	rw_lock_s_unlock_func(
#ifdef UNIV_DEBUG
		pass,
#endif /* UNIV_DEBUG */
		lock);

}
#endif /* UNIV_PFS_RWLOCK */
