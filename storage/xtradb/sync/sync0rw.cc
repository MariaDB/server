/*****************************************************************************

Copyright (c) 1995, 2013, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.

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
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file sync/sync0rw.cc
The read-write lock (for thread synchronization)

Created 9/11/1995 Heikki Tuuri
*******************************************************/

#include "sync0rw.h"
#ifdef UNIV_NONINL
#include "sync0rw.ic"
#include "sync0arr.ic"
#endif

#include "os0thread.h"
#include "mem0mem.h"
#include "srv0srv.h"
#include "os0sync.h" /* for INNODB_RW_LOCKS_USE_ATOMICS */
#include "ha_prototypes.h"
#include "my_cpu.h"

/*
	IMPLEMENTATION OF THE RW_LOCK
	=============================
The status of a rw_lock is held in lock_word. The initial value of lock_word is
X_LOCK_DECR. lock_word is decremented by 1 for each s-lock and by X_LOCK_DECR
for each x-lock. This describes the lock state for each value of lock_word:

lock_word == X_LOCK_DECR:      Unlocked.
0 < lock_word < X_LOCK_DECR:   Read locked, no waiting writers.
			       (X_LOCK_DECR - lock_word) is the
			       number of readers that hold the lock.
lock_word == 0:		       Write locked
-X_LOCK_DECR < lock_word < 0:  Read locked, with a waiting writer.
			       (-lock_word) is the number of readers
			       that hold the lock.
lock_word <= -X_LOCK_DECR:     Recursively write locked. lock_word has been
			       decremented by X_LOCK_DECR for the first lock
			       and the first recursive lock, then by 1 for
			       each recursive lock thereafter.
			       So the number of locks is:
			       (lock_copy == 0) ? 1 : 2 - (lock_copy + X_LOCK_DECR)

The lock_word is always read and updated atomically and consistently, so that
it always represents the state of the lock, and the state of the lock changes
with a single atomic operation. This lock_word holds all of the information
that a thread needs in order to determine if it is eligible to gain the lock
or if it must spin or sleep. The one exception to this is that writer_thread
must be verified before recursive write locks: to solve this scenario, we make
writer_thread readable by all threads, but only writeable by the x-lock holder.

The other members of the lock obey the following rules to remain consistent:

recursive:	This and the writer_thread field together control the
		behaviour of recursive x-locking.
		lock->recursive must be FALSE in following states:
			1) The writer_thread contains garbage i.e.: the
			lock has just been initialized.
			2) The lock is not x-held and there is no
			x-waiter waiting on WAIT_EX event.
			3) The lock is x-held or there is an x-waiter
			waiting on WAIT_EX event but the 'pass' value
			is non-zero.
		lock->recursive is TRUE iff:
			1) The lock is x-held or there is an x-waiter
			waiting on WAIT_EX event and the 'pass' value
			is zero.
		This flag must be set after the writer_thread field
		has been updated with a memory ordering barrier.
		It is unset before the lock_word has been incremented.
writer_thread:	Is used only in recursive x-locking. Can only be safely
		read iff lock->recursive flag is TRUE.
		This field is uninitialized at lock creation time and
		is updated atomically when x-lock is acquired or when
		move_ownership is called. A thread is only allowed to
		set the value of this field to it's thread_id i.e.: a
		thread cannot set writer_thread to some other thread's
		id.
waiters:	May be set to 1 anytime, but to avoid unnecessary wake-up
		signals, it should only be set to 1 when there are threads
		waiting on event. Must be 1 when a writer starts waiting to
		ensure the current x-locking thread sends a wake-up signal
		during unlock. May only be reset to 0 immediately before a
		a wake-up signal is sent to event. On most platforms, a
		memory barrier is required after waiters is set, and before
		verifying lock_word is still held, to ensure some unlocker
		really does see the flags new value.
event:		Threads wait on event for read or writer lock when another
		thread has an x-lock or an x-lock reservation (wait_ex). A
		thread may only	wait on event after performing the following
		actions in order:
		   (1) Record the counter value of event (with os_event_reset).
		   (2) Set waiters to 1.
		   (3) Verify lock_word <= 0.
		(1) must come before (2) to ensure signal is not missed.
		(2) must come before (3) to ensure a signal is sent.
		These restrictions force the above ordering.
		Immediately before sending the wake-up signal, we should:
		   (1) Verify lock_word == X_LOCK_DECR (unlocked)
		   (2) Reset waiters to 0.
wait_ex_event:	A thread may only wait on the wait_ex_event after it has
		performed the following actions in order:
		   (1) Decrement lock_word by X_LOCK_DECR.
		   (2) Record counter value of wait_ex_event (os_event_reset,
		       called from sync_array_reserve_cell).
		   (3) Verify that lock_word < 0.
		(1) must come first to ensures no other threads become reader
		or next writer, and notifies unlocker that signal must be sent.
		(2) must come before (3) to ensure the signal is not missed.
		These restrictions force the above ordering.
		Immediately before sending the wake-up signal, we should:
		   Verify lock_word == 0 (waiting thread holds x_lock)
*/

UNIV_INTERN rw_lock_stats_t	rw_lock_stats;

/* The global list of rw-locks */
UNIV_INTERN rw_lock_list_t	rw_lock_list;
UNIV_INTERN ib_mutex_t		rw_lock_list_mutex;

#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t	rw_lock_list_mutex_key;
UNIV_INTERN mysql_pfs_key_t	rw_lock_mutex_key;
#endif /* UNIV_PFS_MUTEX */

#ifdef UNIV_SYNC_DEBUG
/* The global mutex which protects debug info lists of all rw-locks.
To modify the debug info list of an rw-lock, this mutex has to be
acquired in addition to the mutex protecting the lock. */

UNIV_INTERN os_fast_mutex_t	rw_lock_debug_mutex;

# ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t	rw_lock_debug_mutex_key;
# endif

/******************************************************************//**
Creates a debug info struct. */
static
rw_lock_debug_t*
rw_lock_debug_create(void);
/*======================*/
/******************************************************************//**
Frees a debug info struct. */
static
void
rw_lock_debug_free(
/*===============*/
	rw_lock_debug_t* info);

/******************************************************************//**
Creates a debug info struct.
@return	own: debug info struct */
static
rw_lock_debug_t*
rw_lock_debug_create(void)
/*======================*/
{
	return((rw_lock_debug_t*) mem_alloc(sizeof(rw_lock_debug_t)));
}

/******************************************************************//**
Frees a debug info struct. */
static
void
rw_lock_debug_free(
/*===============*/
	rw_lock_debug_t* info)
{
	mem_free(info);
}
#endif /* UNIV_SYNC_DEBUG */

/******************************************************************//**
Creates, or rather, initializes an rw-lock object in a specified memory
location (which must be appropriately aligned). The rw-lock is initialized
to the non-locked state. Explicit freeing of the rw-lock with rw_lock_free
is necessary only if the memory block containing it is freed. */
UNIV_INTERN
void
rw_lock_create_func(
/*================*/
	rw_lock_t*	lock,		/*!< in: pointer to memory */
#ifdef UNIV_DEBUG
# ifdef UNIV_SYNC_DEBUG
	ulint		level,		/*!< in: level */
# endif /* UNIV_SYNC_DEBUG */
#endif /* UNIV_DEBUG */
	const char*	cmutex_name,	/*!< in: mutex name */
	const char*	cfile_name,	/*!< in: file name where created */
	ulint		cline)		/*!< in: file line where created */
{
	/* If this is the very first time a synchronization object is
	created, then the following call initializes the sync system. */

#ifndef INNODB_RW_LOCKS_USE_ATOMICS
	mutex_create(rw_lock_mutex_key, rw_lock_get_mutex(lock),
		     SYNC_NO_ORDER_CHECK);

	lock->mutex.cfile_name = cfile_name;
	lock->mutex.cline = cline;
	lock->mutex.lock_name = cmutex_name;
	ut_d(lock->mutex.ib_mutex_type = 1);

#else /* INNODB_RW_LOCKS_USE_ATOMICS */
# ifdef UNIV_DEBUG
	UT_NOT_USED(cmutex_name);
# endif
#endif /* INNODB_RW_LOCKS_USE_ATOMICS */

	lock->lock_word = X_LOCK_DECR;
	lock->waiters = 0;

	/* We set this value to signify that lock->writer_thread
	contains garbage at initialization and cannot be used for
	recursive x-locking. */
	lock->recursive = FALSE;
	/* Silence Valgrind when UNIV_DEBUG_VALGRIND is not enabled. */
	memset((void*) &lock->writer_thread, 0, sizeof lock->writer_thread);
	UNIV_MEM_INVALID(&lock->writer_thread, sizeof lock->writer_thread);

#ifdef UNIV_SYNC_DEBUG
	UT_LIST_INIT(lock->debug_list);

	lock->level = level;
#endif /* UNIV_SYNC_DEBUG */

	ut_d(lock->magic_n = RW_LOCK_MAGIC_N);

	lock->cfile_name = cfile_name;
	lock->cline = (unsigned int) cline;
	lock->lock_name = cmutex_name;
	lock->count_os_wait = 0;
	lock->file_name = "not yet reserved";
	lock->line = 0;
	lock->last_s_file_name = "not yet reserved";
	lock->last_x_file_name = "not yet reserved";
	lock->last_s_line = 0;
	lock->last_x_line = 0;
	os_event_create(&lock->event);
	os_event_create(&lock->wait_ex_event);

	mutex_enter(&rw_lock_list_mutex);

	ut_ad(UT_LIST_GET_FIRST(rw_lock_list) == NULL
	      || UT_LIST_GET_FIRST(rw_lock_list)->magic_n == RW_LOCK_MAGIC_N);

	UT_LIST_ADD_FIRST(list, rw_lock_list, lock);

	mutex_exit(&rw_lock_list_mutex);
}

/******************************************************************//**
Creates, or rather, initializes a priority rw-lock object in a specified memory
location (which must be appropriately aligned). The rw-lock is initialized
to the non-locked state. Explicit freeing of the rw-lock with rw_lock_free
is necessary only if the memory block containing it is freed. */
UNIV_INTERN
void
rw_lock_create_func(
/*================*/
	prio_rw_lock_t*	lock,		/*!< in: pointer to memory */
#ifdef UNIV_DEBUG
# ifdef UNIV_SYNC_DEBUG
	ulint		level,		/*!< in: level */
# endif /* UNIV_SYNC_DEBUG */
#endif /* UNIV_DEBUG */
	const char*	cmutex_name,	/*!< in: mutex name */
	const char*	cfile_name,	/*!< in: file name where created */
	ulint		cline)		/*!< in: file line where created */
{
	rw_lock_create_func(&lock->base_lock,
#ifdef UNIV_DEBUG
# ifdef UNIV_SYNC_DEBUG
			    level,
# endif
#endif
			    cmutex_name,
			    cfile_name,
			    cline);

	lock->high_priority_s_waiters = 0;
	os_event_create(&lock->high_priority_s_event);
	lock->high_priority_x_waiters = 0;
	os_event_create(&lock->high_priority_x_event);
	lock->high_priority_wait_ex_waiter = 0;
}

/******************************************************************//**
Calling this function is obligatory only if the memory buffer containing
the rw-lock is freed. Removes an rw-lock object from the global list. The
rw-lock is checked to be in the non-locked state. */
UNIV_INTERN
void
rw_lock_free_func(
/*==============*/
	rw_lock_t*	lock)	/*!< in: rw-lock */
{
#ifndef INNODB_RW_LOCKS_USE_ATOMICS
	ib_mutex_t*	mutex;
#endif /* !INNODB_RW_LOCKS_USE_ATOMICS */

	os_rmb;
	ut_ad(rw_lock_validate(lock));
	ut_a(lock->lock_word == X_LOCK_DECR);

	mutex_enter(&rw_lock_list_mutex);

#ifndef INNODB_RW_LOCKS_USE_ATOMICS
	mutex = rw_lock_get_mutex(lock);
#endif /* !INNODB_RW_LOCKS_USE_ATOMICS */

	os_event_free(&lock->event, false);

	os_event_free(&lock->wait_ex_event, false);

	ut_ad(UT_LIST_GET_PREV(list, lock) == NULL
	      || UT_LIST_GET_PREV(list, lock)->magic_n == RW_LOCK_MAGIC_N);
	ut_ad(UT_LIST_GET_NEXT(list, lock) == NULL
	      || UT_LIST_GET_NEXT(list, lock)->magic_n == RW_LOCK_MAGIC_N);

	UT_LIST_REMOVE(list, rw_lock_list, lock);

	mutex_exit(&rw_lock_list_mutex);

	ut_d(lock->magic_n = 0);

#ifndef INNODB_RW_LOCKS_USE_ATOMICS
	/* We have merely removed the rw_lock from the list, the memory
	has not been freed. Therefore the pointer to mutex is valid. */
	mutex_free(mutex);
#endif /* !INNODB_RW_LOCKS_USE_ATOMICS */
}

/******************************************************************//**
Calling this function is obligatory only if the memory buffer containing
the priority rw-lock is freed. Removes an rw-lock object from the global list.
The rw-lock is checked to be in the non-locked state. */
UNIV_INTERN
void
rw_lock_free_func(
/*==============*/
	prio_rw_lock_t*	lock)	/*!< in: rw-lock */
{
	os_event_free(&lock->high_priority_s_event, false);
	os_event_free(&lock->high_priority_x_event, false);
	rw_lock_free_func(&lock->base_lock);
}

#ifdef UNIV_DEBUG
/******************************************************************//**
Checks that the rw-lock has been initialized and that there are no
simultaneous shared and exclusive locks.
@return	TRUE */
UNIV_INTERN
ibool
rw_lock_validate(
/*=============*/
	rw_lock_t*	lock)	/*!< in: rw-lock */
{
	ulint	waiters;
	lint	lock_word;

	ut_ad(lock);

	waiters = rw_lock_get_waiters(lock);
	lock_word = lock->lock_word;

	ut_ad(lock->magic_n == RW_LOCK_MAGIC_N);
	ut_ad(waiters == 0 || waiters == 1);
	ut_ad(lock_word > -(2 * X_LOCK_DECR));
	ut_ad(lock_word <= X_LOCK_DECR);

	return(TRUE);
}

/******************************************************************//**
Checks that the priority rw-lock has been initialized and that there are no
simultaneous shared and exclusive locks.
@return	TRUE */
UNIV_INTERN
ibool
rw_lock_validate(
/*=============*/
	prio_rw_lock_t*	lock)	/*!< in: rw-lock */
{
	return(rw_lock_validate(&lock->base_lock));
}

#endif /* UNIV_DEBUG */

/******************************************************************//**
Lock a regular or priority rw-lock in shared mode for the current thread. If
the rw-lock is locked in exclusive mode, or there is an exclusive lock request
waiting, the function spins a preset time (controlled by SYNC_SPIN_ROUNDS),
waiting for the lock, before suspending the thread. */
UNIV_INTERN
void
rw_lock_s_lock_spin(
/*================*/
	void*		_lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the lock
				will be passed to another thread to unlock */
	bool		priority_lock,
				/*!< in: whether the lock is a priority lock */
	bool		high_priority,
				/*!< in: whether we are acquiring a priority
				lock with high priority */
	const char*	file_name, /*!< in: file name where lock requested */
	ulint		line)	/*!< in: line where requested */
{
	ulint		index;	/* index of the reserved wait cell */
	ulint		i = 0;	/* spin round count */
	sync_array_t*	sync_arr;
	size_t		counter_index;
	rw_lock_t*	lock = (rw_lock_t *) _lock;

	/* We reuse the thread id to index into the counter, cache
	it here for efficiency. */

	counter_index = (size_t) os_thread_get_curr_id();

	ut_ad(rw_lock_validate(lock));

	rw_lock_stats.rw_s_spin_wait_count.add(counter_index, 1);
lock_loop:

	if (!rw_lock_higher_prio_waiters_exist(priority_lock, high_priority,
					       lock)) {

		/* Spin waiting for the writer field to become free */
		os_rmb;
		HMT_low();
		while (i < SYNC_SPIN_ROUNDS && lock->lock_word <= 0) {
			if (srv_spin_wait_delay) {
				ut_delay(ut_rnd_interval(0,
							 srv_spin_wait_delay));
			}

			i++;
			os_rmb;
		}

		HMT_medium();
		if (i >= SYNC_SPIN_ROUNDS) {
			os_thread_yield();
		}

		if (srv_print_latch_waits) {
			fprintf(stderr,
				"Thread " ULINTPF " spin wait rw-s-lock at %p"
				" '%s' rnds " ULINTPF "\n",
				os_thread_pf(os_thread_get_curr_id()),
				(void*) lock, lock->lock_name, i);
		}
	} else {

		/* In case of higher priority waiters already present, perform
		only this part of the spinning code path.  */
		os_thread_yield();
	}

	/* We try once again to obtain the lock */
	if (!rw_lock_higher_prio_waiters_exist(priority_lock, high_priority,
					       lock)
	    && (TRUE == rw_lock_s_lock_low(lock, pass, file_name, line))) {
		rw_lock_stats.rw_s_spin_round_count.add(counter_index, i);

		return; /* Success */
	} else {

		prio_rw_lock_t*	prio_rw_lock = NULL;

		if (i > 0 && i < SYNC_SPIN_ROUNDS) {
			goto lock_loop;
		}

		rw_lock_stats.rw_s_spin_round_count.add(counter_index, i);

		sync_arr = sync_array_get_and_reserve_cell(lock,
							   high_priority
							   ? PRIO_RW_LOCK_SHARED
							   : RW_LOCK_SHARED,
							   file_name,
							   line, &index);

		/* Set waiters before checking lock_word to ensure wake-up
		signal is sent. This may lead to some unnecessary signals. */
		if (high_priority) {

			prio_rw_lock = reinterpret_cast<prio_rw_lock_t *>
				(_lock);
			os_atomic_increment_ulint(
				&prio_rw_lock->high_priority_s_waiters,
				1);
		} else {

			rw_lock_set_waiter_flag(lock);
		}

		if (!rw_lock_higher_prio_waiters_exist(priority_lock,
						       high_priority, lock)
		    && (TRUE == rw_lock_s_lock_low(lock, pass,
						   file_name, line))) {
			sync_array_free_cell(sync_arr, index);
			if (prio_rw_lock) {

				os_atomic_decrement_ulint(
					&prio_rw_lock->high_priority_s_waiters,
					1);
			}
			return; /* Success */
		}

		if (srv_print_latch_waits) {
			fprintf(stderr,
				"Thread " ULINTPF " OS wait rw-s-lock at %p"
				" '%s'\n",
				os_thread_pf(os_thread_get_curr_id()),
				(void*) lock, lock->lock_name);
		}

		/* these stats may not be accurate */
		lock->count_os_wait++;
		rw_lock_stats.rw_s_os_wait_count.add(counter_index, 1);

		sync_array_wait_event(sync_arr, index);

		if (prio_rw_lock) {

			os_atomic_decrement_ulint(
				&prio_rw_lock->high_priority_s_waiters,
				1);
		}

		i = 0;
		goto lock_loop;
	}
}

/******************************************************************//**
This function is used in the insert buffer to move the ownership of an
x-latch on a buffer frame to the current thread. The x-latch was set by
the buffer read operation and it protected the buffer frame while the
read was done. The ownership is moved because we want that the current
thread is able to acquire a second x-latch which is stored in an mtr.
This, in turn, is needed to pass the debug checks of index page
operations. */
UNIV_INTERN
void
rw_lock_x_lock_move_ownership(
/*==========================*/
	rw_lock_t*	lock)	/*!< in: lock which was x-locked in the
				buffer read */
{
	ut_ad(rw_lock_is_locked(lock, RW_LOCK_EX));

	rw_lock_set_writer_id_and_recursion_flag(lock, TRUE);
}

/******************************************************************//**
Function for the next writer to call. Waits for readers to exit.
The caller must have already decremented lock_word by X_LOCK_DECR. */
UNIV_INLINE
void
rw_lock_x_lock_wait(
/*================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	bool		high_priority,
				/*!< in: if true, the rw lock is a priority
				lock and is being acquired with high
				priority  */
#ifdef UNIV_SYNC_DEBUG
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				be passed to another thread to unlock */
#endif
	const char*	file_name,/*!< in: file name where lock requested */
	ulint		line)	/*!< in: line where requested */
{
	ulint		index;
	ulint		i = 0;
	sync_array_t*	sync_arr;
	size_t		counter_index;
	prio_rw_lock_t*	prio_rw_lock = NULL;

	/* We reuse the thread id to index into the counter, cache
	it here for efficiency. */

	counter_index = (size_t) os_thread_get_curr_id();

	os_rmb;
	ut_ad(lock->lock_word <= 0);

        HMT_low();
	if (high_priority) {

		prio_rw_lock = reinterpret_cast<prio_rw_lock_t *>(lock);
		prio_rw_lock->high_priority_wait_ex_waiter = 1;
	}

	while (lock->lock_word < 0) {
		if (srv_spin_wait_delay) {
			ut_delay(ut_rnd_interval(0, srv_spin_wait_delay));
		}
		if(i < SYNC_SPIN_ROUNDS) {
			i++;
			os_rmb;
			continue;
		}
		HMT_medium();

		/* If there is still a reader, then go to sleep.*/
		rw_lock_stats.rw_x_spin_round_count.add(counter_index, i);

		sync_arr = sync_array_get_and_reserve_cell(lock,
							   RW_LOCK_WAIT_EX,
							   file_name,
							   line, &index);

		i = 0;

		/* Check lock_word to ensure wake-up isn't missed.*/
		if (lock->lock_word < 0) {

			/* these stats may not be accurate */
			lock->count_os_wait++;
			rw_lock_stats.rw_x_os_wait_count.add(counter_index, 1);

			/* Add debug info as it is needed to detect possible
			deadlock. We must add info for WAIT_EX thread for
			deadlock detection to work properly. */
#ifdef UNIV_SYNC_DEBUG
			rw_lock_add_debug_info(lock, pass, RW_LOCK_WAIT_EX,
					       file_name, line);
#endif

			if (srv_instrument_semaphores) {
				lock->thread_id = os_thread_get_curr_id();
				lock->file_name = file_name;
				lock->line = line;
			}

			sync_array_wait_event(sync_arr, index);
#ifdef UNIV_SYNC_DEBUG
			rw_lock_remove_debug_info(
				lock, pass, RW_LOCK_WAIT_EX);
#endif
			/* It is possible to wake when lock_word < 0.
			We must pass the while-loop check to proceed.*/
		} else {
			sync_array_free_cell(sync_arr, index);
		}
		HMT_low();
	}
	HMT_medium();

	if (prio_rw_lock) {

		prio_rw_lock->high_priority_wait_ex_waiter = 0;
	}

	rw_lock_stats.rw_x_spin_round_count.add(counter_index, i);
}

/******************************************************************//**
Low-level function for acquiring an exclusive lock.
@return	FALSE if did not succeed, TRUE if success. */
UNIV_INLINE
ibool
rw_lock_x_lock_low(
/*===============*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	bool		high_priority,
				/*!< in: if true, the rw lock is a priority
			        lock and is being acquired with high
				priority  */
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				be passed to another thread to unlock */
	const char*	file_name,/*!< in: file name where lock requested */
	ulint		line)	/*!< in: line where requested */
{
	ibool local_recursive= lock->recursive;

	if (rw_lock_lock_word_decr(lock, X_LOCK_DECR)) {

		/* lock->recursive also tells us if the writer_thread
		field is stale or active. As we are going to write
		our own thread id in that field it must be that the
		current writer_thread value is not active. */
		ut_a(!lock->recursive);

		/* Decrement occurred: we are writer or next-writer. */
		rw_lock_set_writer_id_and_recursion_flag(
			lock, pass ? FALSE : TRUE);

		rw_lock_x_lock_wait(lock, high_priority,
#ifdef UNIV_SYNC_DEBUG
				    pass,
#endif
				    file_name, line);

	} else {
		os_thread_id_t	thread_id = os_thread_get_curr_id();

		/* Decrement failed: relock or failed lock
		Note: recursive must be loaded before writer_thread see
		comment for rw_lock_set_writer_id_and_recursion_flag().
		To achieve this we load it before rw_lock_lock_word_decr(),
		which implies full memory barrier in current implementation. */
		if (!pass && local_recursive
		    && os_thread_eq(lock->writer_thread, thread_id)) {
			/* Relock */
			if (lock->lock_word == 0) {
				lock->lock_word -= X_LOCK_DECR;
			} else {
				--lock->lock_word;
			}

		} else {
			/* Another thread locked before us */
			return(FALSE);
		}
	}
#ifdef UNIV_SYNC_DEBUG
	rw_lock_add_debug_info(lock, pass, RW_LOCK_EX, file_name, line);
#endif
	lock->last_x_file_name = file_name;
	lock->last_x_line = (unsigned int) line;

	if (srv_instrument_semaphores) {
		lock->thread_id = os_thread_get_curr_id();
		lock->file_name = file_name;
		lock->line = line;
	}

	return(TRUE);
}

/******************************************************************//**
NOTE! Use the corresponding macro, not directly this function! Lock an
rw-lock in exclusive mode for the current thread. If the rw-lock is locked
in shared or exclusive mode, or there is an exclusive lock request waiting,
the function spins a preset time (controlled by SYNC_SPIN_ROUNDS), waiting
for the lock before suspending the thread. If the same thread has an x-lock
on the rw-lock, locking succeed, with the following exception: if pass != 0,
only a single x-lock may be taken on the lock. NOTE: If the same thread has
an s-lock, locking does not succeed! */
UNIV_INTERN
void
rw_lock_x_lock_func(
/*================*/
	rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				be passed to another thread to unlock */
	const char*	file_name,/*!< in: file name where lock requested */
	ulint		line,	/*!< in: line where requested */
	bool		priority_lock,
				/*!< in: whether the lock is a priority lock */
	bool		high_priority)
				/*!< in: whether we are acquiring a priority
				lock with high priority */
{
	ulint		i;	/*!< spin round count */
	ulint		index;	/*!< index of the reserved wait cell */
	sync_array_t*	sync_arr;
	ibool		spinning = FALSE;
	size_t		counter_index;
	prio_rw_lock_t*	prio_lock = NULL;

	/* We reuse the thread id to index into the counter, cache
	it here for efficiency. */

	counter_index = (size_t) os_thread_get_curr_id();

	ut_ad(rw_lock_validate(lock));
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!rw_lock_own(lock, RW_LOCK_SHARED));
#endif /* UNIV_SYNC_DEBUG */

	i = 0;

	ut_ad(priority_lock || !high_priority);

lock_loop:

	if (!rw_lock_higher_prio_waiters_exist(priority_lock, high_priority,
					       lock)
	    && rw_lock_x_lock_low(lock, high_priority, pass,
				  file_name, line)) {
		rw_lock_stats.rw_x_spin_round_count.add(counter_index, i);

		return;	/* Locking succeeded */

	} else if (!rw_lock_higher_prio_waiters_exist(priority_lock,
						      high_priority, lock)) {

		if (!spinning) {
			spinning = TRUE;

			rw_lock_stats.rw_x_spin_wait_count.add(
				counter_index, 1);
		}

		/* Spin waiting for the lock_word to become free */
		os_rmb;
		HMT_low();
		while (i < SYNC_SPIN_ROUNDS
		       && lock->lock_word <= 0) {
			if (srv_spin_wait_delay) {
				ut_delay(ut_rnd_interval(0,
							 srv_spin_wait_delay));
			}

			i++;
			os_rmb;
		}
		HMT_medium();
		if (i >= SYNC_SPIN_ROUNDS) {
			os_thread_yield();
		} else {
			goto lock_loop;
		}
	} else {

		/* In case we skipped spinning because of higher-priority
		waiters already waiting, perform only this bit of the spinning
		code path.  */
		os_thread_yield();
	}

	if (spinning) {

		rw_lock_stats.rw_x_spin_round_count.add(counter_index, i);

		if (srv_print_latch_waits) {
			fprintf(stderr,
				"Thread " ULINTPF " spin wait rw-x-lock at %p"
				" '%s' rnds " ULINTPF "\n",
				os_thread_pf(os_thread_get_curr_id()),
				(void*) lock,lock->lock_name, i);
		}
	}

	sync_arr = sync_array_get_and_reserve_cell(lock,
						   high_priority
						   ? PRIO_RW_LOCK_EX
						   : RW_LOCK_EX,
						   file_name, line, &index);

	/* Waiters must be set before checking lock_word, to ensure signal
	is sent. This could lead to a few unnecessary wake-up signals. */
	if (high_priority) {

		prio_lock = reinterpret_cast<prio_rw_lock_t *>(lock);
		os_atomic_increment_ulint(&prio_lock->high_priority_x_waiters,
					  1);
	} else {
		rw_lock_set_waiter_flag(lock);
	}

	if (rw_lock_x_lock_low(lock, high_priority, pass, file_name, line)) {
		sync_array_free_cell(sync_arr, index);
		if (prio_lock) {

			os_atomic_decrement_ulint(
				&prio_lock->high_priority_x_waiters,
				1);
		}
		return; /* Locking succeeded */
	}

	if (srv_print_latch_waits) {
		fprintf(stderr,
			"Thread " ULINTPF " OS wait for rw-x-lock at %p"
			" '%s'\n",
			os_thread_pf(os_thread_get_curr_id()), (void*) lock,
			lock->lock_name);
	}

	/* these stats may not be accurate */
	lock->count_os_wait++;
	rw_lock_stats.rw_x_os_wait_count.add(counter_index, 1);

	sync_array_wait_event(sync_arr, index);

	if (prio_lock) {

		os_atomic_decrement_ulint(&prio_lock->high_priority_x_waiters,
					  1);
	}

	i = 0;
	goto lock_loop;
}

/******************************************************************//**
NOTE! Use the corresponding macro, not directly this function! Lock a priority
rw-lock in exclusive mode for the current thread. If the rw-lock is locked
in shared or exclusive mode, or there is an exclusive lock request waiting,
the function spins a preset time (controlled by SYNC_SPIN_ROUNDS), waiting
for the lock, before suspending the thread. If the same thread has an x-lock
on the rw-lock, locking succeed, with the following exception: if pass != 0,
only a single x-lock may be taken on the lock. NOTE: If the same thread has
an s-lock, locking does not succeed! */
UNIV_INTERN
void
rw_lock_x_lock_func(
/*================*/
	prio_rw_lock_t*	lock,	/*!< in: pointer to rw-lock */
	ulint		pass,	/*!< in: pass value; != 0, if the lock will
				  be passed to another thread to unlock */
	const char*	file_name,/*!< in: file name where lock requested */
	ulint		line)	/*!< in: line where requested */
{
	rw_lock_x_lock_func(&lock->base_lock, pass, file_name, line, true,
			    srv_current_thread_priority > 0);
}

#ifdef UNIV_SYNC_DEBUG
/******************************************************************//**
Acquires the debug mutex. We cannot use the mutex defined in sync0sync,
because the debug mutex is also acquired in sync0arr while holding the OS
mutex protecting the sync array, and the ordinary mutex_enter might
recursively call routines in sync0arr, leading to a deadlock on the OS
mutex. */
UNIV_INTERN
void
rw_lock_debug_mutex_enter(void)
/*===========================*/
{
	os_fast_mutex_lock(&rw_lock_debug_mutex);
}

/******************************************************************//**
Releases the debug mutex. */
UNIV_INTERN
void
rw_lock_debug_mutex_exit(void)
/*==========================*/
{
	os_fast_mutex_unlock(&rw_lock_debug_mutex);
}

/******************************************************************//**
Inserts the debug information for an rw-lock. */
UNIV_INTERN
void
rw_lock_add_debug_info(
/*===================*/
	rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		pass,		/*!< in: pass value */
	ulint		lock_type,	/*!< in: lock type */
	const char*	file_name,	/*!< in: file where requested */
	ulint		line)		/*!< in: line where requested */
{
	rw_lock_debug_t*	info;

	ut_ad(lock);
	ut_ad(file_name);

	info = rw_lock_debug_create();

	rw_lock_debug_mutex_enter();

	info->file_name = file_name;
	info->line	= line;
	info->lock_type = lock_type;
	info->thread_id = os_thread_get_curr_id();
	info->pass	= pass;

	UT_LIST_ADD_FIRST(list, lock->debug_list, info);

	rw_lock_debug_mutex_exit();

	if ((pass == 0) && (lock_type != RW_LOCK_WAIT_EX)) {
		sync_thread_add_level(lock, lock->level,
				      lock_type == RW_LOCK_EX
				      && lock->lock_word < 0);
	}
}

/******************************************************************//**
Removes a debug information struct for an rw-lock. */
UNIV_INTERN
void
rw_lock_remove_debug_info(
/*======================*/
	rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		pass,		/*!< in: pass value */
	ulint		lock_type)	/*!< in: lock type */
{
	rw_lock_debug_t*	info;

	ut_ad(lock);

	if ((pass == 0) && (lock_type != RW_LOCK_WAIT_EX)) {
		sync_thread_reset_level(lock);
	}

	rw_lock_debug_mutex_enter();

	info = UT_LIST_GET_FIRST(lock->debug_list);

	while (info != NULL) {
		if ((pass == info->pass)
		    && ((pass != 0)
			|| os_thread_eq(info->thread_id,
					os_thread_get_curr_id()))
		    && (info->lock_type == lock_type)) {

			/* Found! */
			UT_LIST_REMOVE(list, lock->debug_list, info);
			rw_lock_debug_mutex_exit();

			rw_lock_debug_free(info);

			return;
		}

		info = UT_LIST_GET_NEXT(list, info);
	}

	ut_error;
}
#endif /* UNIV_SYNC_DEBUG */

#ifdef UNIV_SYNC_DEBUG
/******************************************************************//**
Checks if the thread has locked the rw-lock in the specified mode, with
the pass value == 0.
@return	TRUE if locked */
UNIV_INTERN
ibool
rw_lock_own(
/*========*/
	rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		lock_type)	/*!< in: lock type: RW_LOCK_SHARED,
					RW_LOCK_EX */
{
	rw_lock_debug_t*	info;

	ut_ad(lock);
	ut_ad(rw_lock_validate(lock));

	rw_lock_debug_mutex_enter();

	info = UT_LIST_GET_FIRST(lock->debug_list);

	while (info != NULL) {

		if (os_thread_eq(info->thread_id, os_thread_get_curr_id())
		    && (info->pass == 0)
		    && (info->lock_type == lock_type)) {

			rw_lock_debug_mutex_exit();
			/* Found! */

			return(TRUE);
		}

		info = UT_LIST_GET_NEXT(list, info);
	}
	rw_lock_debug_mutex_exit();

	return(FALSE);
}

/******************************************************************//**
Checks if the thread has locked the priority rw-lock in the specified mode,
with the pass value == 0. */
UNIV_INTERN
ibool
rw_lock_own(
/*========*/
	prio_rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		lock_type)	/*!< in: lock type: RW_LOCK_SHARED,
					RW_LOCK_EX */
{
	return(rw_lock_own(&lock->base_lock, lock_type));
}

#endif /* UNIV_SYNC_DEBUG */

/******************************************************************//**
Checks if somebody has locked the rw-lock in the specified mode.
@return	TRUE if locked */
UNIV_INTERN
ibool
rw_lock_is_locked(
/*==============*/
	rw_lock_t*	lock,		/*!< in: rw-lock */
	ulint		lock_type)	/*!< in: lock type: RW_LOCK_SHARED,
					RW_LOCK_EX */
{
	ibool	ret	= FALSE;

	ut_ad(lock);
	ut_ad(rw_lock_validate(lock));

	if (lock_type == RW_LOCK_SHARED) {
		if (rw_lock_get_reader_count(lock) > 0) {
			ret = TRUE;
		}
	} else if (lock_type == RW_LOCK_EX) {
		if (rw_lock_get_writer(lock) == RW_LOCK_EX) {
			ret = TRUE;
		}
	} else {
		ut_error;
	}

	return(ret);
}

#ifdef UNIV_SYNC_DEBUG
/***************************************************************//**
Prints debug info of currently locked rw-locks. */
UNIV_INTERN
void
rw_lock_list_print_info(
/*====================*/
	FILE*	file)		/*!< in: file where to print */
{
	rw_lock_t*	lock;
	ulint		count		= 0;
	rw_lock_debug_t* info;

	mutex_enter(&rw_lock_list_mutex);

	fputs("-------------\n"
	      "RW-LATCH INFO\n"
	      "-------------\n", file);

	lock = UT_LIST_GET_FIRST(rw_lock_list);

	while (lock != NULL) {

		count++;

#ifndef INNODB_RW_LOCKS_USE_ATOMICS
		mutex_enter(&(lock->mutex));
#endif
		if (lock->lock_word != X_LOCK_DECR) {

			fprintf(file, "RW-LOCK: %p ", (void*) lock);

			if (rw_lock_get_waiters(lock)) {
				fputs(" Waiters for the lock exist\n", file);
			} else {
				putc('\n', file);
			}

			rw_lock_debug_mutex_enter();
			info = UT_LIST_GET_FIRST(lock->debug_list);
			while (info != NULL) {
				rw_lock_debug_print(file, info);
				info = UT_LIST_GET_NEXT(list, info);
			}
			rw_lock_debug_mutex_exit();
		}
#ifndef INNODB_RW_LOCKS_USE_ATOMICS
		mutex_exit(&(lock->mutex));
#endif

		lock = UT_LIST_GET_NEXT(list, lock);
	}

	fprintf(file, "Total number of rw-locks %ld\n", count);
	mutex_exit(&rw_lock_list_mutex);
}

/***************************************************************//**
Prints debug info of an rw-lock. */
UNIV_INTERN
void
rw_lock_print(
/*==========*/
	rw_lock_t*	lock)	/*!< in: rw-lock */
{
	rw_lock_debug_t* info;

	fprintf(stderr,
		"-------------\n"
		"RW-LATCH INFO\n"
		"RW-LATCH: %p ", (void*) lock);

#ifndef INNODB_RW_LOCKS_USE_ATOMICS
	/* We used to acquire lock->mutex here, but it would cause a
	recursive call to sync_thread_add_level() if UNIV_SYNC_DEBUG
	is defined.  Since this function is only invoked from
	sync_thread_levels_g(), let us choose the smaller evil:
	performing dirty reads instead of causing bogus deadlocks or
	assertion failures. */
#endif
	if (lock->lock_word != X_LOCK_DECR) {

		if (rw_lock_get_waiters(lock)) {
			fputs(" Waiters for the lock exist\n", stderr);
		} else {
			putc('\n', stderr);
		}

		rw_lock_debug_mutex_enter();
		info = UT_LIST_GET_FIRST(lock->debug_list);
		while (info != NULL) {
			rw_lock_debug_print(stderr, info);
			info = UT_LIST_GET_NEXT(list, info);
		}
		rw_lock_debug_mutex_exit();
	}
}

/*********************************************************************//**
Prints info of a debug struct. */
UNIV_INTERN
void
rw_lock_debug_print(
/*================*/
	FILE*			f,	/*!< in: output stream */
	rw_lock_debug_t*	info)	/*!< in: debug struct */
{
	ulint	rwt;

	rwt	  = info->lock_type;

	fprintf(f, "Locked: thread %lu file %s line %lu  ",
		(ulong) os_thread_pf(info->thread_id), info->file_name,
		(ulong) info->line);
	if (rwt == RW_LOCK_SHARED) {
		fputs("S-LOCK", f);
	} else if (rwt == RW_LOCK_EX) {
		fputs("X-LOCK", f);
	} else if (rwt == RW_LOCK_WAIT_EX) {
		fputs("WAIT X-LOCK", f);
	} else {
		ut_error;
	}
	if (info->pass != 0) {
		fprintf(f, " pass value %lu", (ulong) info->pass);
	}
	putc('\n', f);
}

/***************************************************************//**
Returns the number of currently locked rw-locks. Works only in the debug
version.
@return	number of locked rw-locks */
UNIV_INTERN
ulint
rw_lock_n_locked(void)
/*==================*/
{
	rw_lock_t*	lock;
	ulint		count		= 0;

	mutex_enter(&rw_lock_list_mutex);

	lock = UT_LIST_GET_FIRST(rw_lock_list);

	while (lock != NULL) {

		if (lock->lock_word != X_LOCK_DECR) {
			count++;
		}

		lock = UT_LIST_GET_NEXT(list, lock);
	}

	mutex_exit(&rw_lock_list_mutex);

	return(count);
}
#endif /* UNIV_SYNC_DEBUG */
