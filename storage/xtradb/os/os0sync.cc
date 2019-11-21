/*****************************************************************************

Copyright (c) 1995, 2015, Oracle and/or its affiliates. All Rights Reserved.

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
@file os/os0sync.cc
The interface to the operating system
synchronization primitives.

Created 9/6/1995 Heikki Tuuri
*******************************************************/

#include "os0sync.h"
#ifdef UNIV_NONINL
#include "os0sync.ic"
#endif

#ifdef __WIN__
#include <windows.h>
#endif

#include "ut0mem.h"
#include "srv0start.h"
#include "srv0srv.h"

/* Type definition for an operating system mutex struct */
struct os_mutex_t{
	os_event_t	event;	/*!< Used by sync0arr.cc for queing threads */
	void*		handle;	/*!< OS handle to mutex */
	ulint		count;	/*!< we use this counter to check
				that the same thread does not
				recursively lock the mutex: we
				do not assume that the OS mutex
				supports recursive locking, though
				NT seems to do that */
};

// All the os_*_count variables are accessed atomically

/** This is incremented by 1 in os_thread_create and decremented by 1 in
os_thread_exit. */
UNIV_INTERN ulint	os_thread_count		= 0;

UNIV_INTERN ulint	os_event_count		= 0;
UNIV_INTERN ulint	os_mutex_count		= 0;
UNIV_INTERN ulint	os_fast_mutex_count	= 0;

#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t	event_os_mutex_key;
UNIV_INTERN mysql_pfs_key_t	os_mutex_key;
#endif

/*********************************************************//**
Initialitze condition variable */
UNIV_INLINE
void
os_cond_init(
/*=========*/
	os_cond_t*	cond)	/*!< in: condition variable. */
{
	ut_a(cond);

#ifdef __WIN__
	InitializeConditionVariable(cond);
#else
	ut_a(pthread_cond_init(cond, NULL) == 0);
#endif
}

/*********************************************************//**
Do a timed wait on condition variable.
@return TRUE if timed out, FALSE otherwise */
UNIV_INLINE
ibool
os_cond_wait_timed(
/*===============*/
	os_cond_t*		cond,		/*!< in: condition variable. */
	os_fast_mutex_t*	fast_mutex,	/*!< in: fast mutex */
#ifndef __WIN__
	const struct timespec*	abstime		/*!< in: timeout */
#else
	DWORD			time_in_ms	/*!< in: timeout in
						milliseconds*/
#endif /* !__WIN__ */
)
{
	fast_mutex_t*	mutex = &fast_mutex->mutex;
#ifdef __WIN__
	BOOL	ret;
	DWORD	err;


	ret = SleepConditionVariableCS(cond, mutex, time_in_ms);

	if (!ret) {
		err = GetLastError();
		/* From http://msdn.microsoft.com/en-us/library/ms686301%28VS.85%29.aspx,
		"Condition variables are subject to spurious wakeups
		(those not associated with an explicit wake) and stolen wakeups
		(another thread manages to run before the woken thread)."
		Check for both types of timeouts.
		Conditions are checked by the caller.*/
		if ((err == WAIT_TIMEOUT) || (err == ERROR_TIMEOUT)) {
			return(TRUE);
		}
	}

	ut_a(ret);

	return(FALSE);
#else
	int	ret;

	ret = pthread_cond_timedwait(cond, mutex, abstime);

	switch (ret) {
	case 0:
	case ETIMEDOUT:
	/* We play it safe by checking for EINTR even though
	according to the POSIX documentation it can't return EINTR. */
	case EINTR:
		break;

	default:
		fprintf(stderr, "  InnoDB: pthread_cond_timedwait() returned: "
				"%d: abstime={%lu,%lu}\n",
				ret, (ulong) abstime->tv_sec, (ulong) abstime->tv_nsec);
		ut_error;
	}

	return(ret == ETIMEDOUT);
#endif
}
/*********************************************************//**
Wait on condition variable */
UNIV_INLINE
void
os_cond_wait(
/*=========*/
	os_cond_t*		cond,	/*!< in: condition variable. */
	os_fast_mutex_t*	fast_mutex)/*!< in: fast mutex */
{
	fast_mutex_t*	mutex = &fast_mutex->mutex;
	ut_a(cond);
	ut_a(mutex);

#ifdef __WIN__
	ut_a(SleepConditionVariableCS(cond, mutex, INFINITE));
#else
	ut_a(pthread_cond_wait(cond, mutex) == 0);
#endif
}

/*********************************************************//**
Wakes all threads  waiting for condition variable */
UNIV_INLINE
void
os_cond_broadcast(
/*==============*/
	os_cond_t*	cond)	/*!< in: condition variable. */
{
	ut_a(cond);

#ifdef __WIN__
	WakeAllConditionVariable(cond);
#else
	ut_a(pthread_cond_broadcast(cond) == 0);
#endif
}

/*********************************************************//**
Destroys condition variable */
UNIV_INLINE
void
os_cond_destroy(
/*============*/
	os_cond_t*	cond)	/*!< in: condition variable. */
{
#ifdef __WIN__
	/* Do nothing */
#else
	ut_a(pthread_cond_destroy(cond) == 0);
#endif
}

/*********************************************************//**
Initializes global event and OS 'slow' mutex lists. */
UNIV_INTERN
void
os_sync_init(void)
/*==============*/
{
}

/** Create an event semaphore, i.e., a semaphore which may just have two
states: signaled and nonsignaled. The created event is manual reset: it must be
reset explicitly by calling sync_os_reset_event.
@param[in,out]	event	memory block where to create the event */
UNIV_INTERN
void
os_event_create(os_event_t event)
{
#ifndef PFS_SKIP_EVENT_MUTEX
	os_fast_mutex_init(event_os_mutex_key, &event->os_mutex);
#else
	os_fast_mutex_init(PFS_NOT_INSTRUMENTED, &event->os_mutex);
#endif

	os_cond_init(&(event->cond_var));

		event->init_count_and_set();

	os_atomic_increment_ulint(&os_event_count, 1);
}

/*********************************************************//**
Creates an event semaphore, i.e., a semaphore which may just have two
states: signaled and nonsignaled. The created event is manual reset: it
must be reset explicitly by calling sync_os_reset_event.
@return	the event handle */
UNIV_INTERN
os_event_t
os_event_create(void)
/*==================*/
{
	os_event_t event = static_cast<os_event_t>(ut_malloc(sizeof(*event)));

	os_event_create(event);

	return(event);
}

/**********************************************************//**
Sets an event semaphore to the signaled state: lets waiting threads
proceed. */
UNIV_INTERN
void
os_event_set(
/*=========*/
	os_event_t	event)	/*!< in: event to set */
{
	ut_a(event);

	os_fast_mutex_lock(&(event->os_mutex));

	if (UNIV_UNLIKELY(event->is_set())) {
		/* Do nothing */
	} else {
		event->set();
		event->inc_signal_count();
		os_cond_broadcast(&(event->cond_var));
	}

	os_fast_mutex_unlock(&(event->os_mutex));
}

/**********************************************************//**
Resets an event semaphore to the nonsignaled state. Waiting threads will
stop to wait for the event.
The return value should be passed to os_even_wait_low() if it is desired
that this thread should not wait in case of an intervening call to
os_event_set() between this os_event_reset() and the
os_event_wait_low() call. See comments for os_event_wait_low().
@return	current signal_count. */
UNIV_INTERN
ib_int64_t
os_event_reset(
/*===========*/
	os_event_t	event)	/*!< in: event to reset */
{
	ib_int64_t	ret = 0;

	ut_a(event);

	os_fast_mutex_lock(&(event->os_mutex));

	if (UNIV_UNLIKELY(!event->is_set())) {
		/* Do nothing */
	} else {
		event->reset();
	}
	ret = event->signal_count();

	os_fast_mutex_unlock(&(event->os_mutex));
	return(ret);
}

/**********************************************************//**
Frees an event object. */
UNIV_INTERN
void
os_event_free(
/*==========*/
	os_event_t	event,	/*!< in: event to free */
	bool		free_memory)/*!< in: if true, deallocate the event
				    memory block too */

{
	ut_a(event);

	os_fast_mutex_free(&(event->os_mutex));

	os_cond_destroy(&(event->cond_var));

	os_atomic_decrement_ulint(&os_event_count, 1);

	if (free_memory)
		ut_free(event);
}

/**********************************************************//**
Waits for an event object until it is in the signaled state.

Typically, if the event has been signalled after the os_event_reset()
we'll return immediately because event->is_set == TRUE.
There are, however, situations (e.g.: sync_array code) where we may
lose this information. For example:

thread A calls os_event_reset()
thread B calls os_event_set()   [event->is_set == TRUE]
thread C calls os_event_reset() [event->is_set == FALSE]
thread A calls os_event_wait()  [infinite wait!]
thread C calls os_event_wait()  [infinite wait!]

Where such a scenario is possible, to avoid infinite wait, the
value returned by os_event_reset() should be passed in as
reset_sig_count. */
UNIV_INTERN
void
os_event_wait_low(
/*==============*/
	os_event_t	event,		/*!< in: event to wait */
	ib_int64_t	reset_sig_count)/*!< in: zero or the value
					returned by previous call of
					os_event_reset(). */
{

	os_fast_mutex_lock(&event->os_mutex);

	if (!reset_sig_count) {
		reset_sig_count = event->signal_count();
	}

	while (!event->is_set() && event->signal_count() == reset_sig_count) {
		os_cond_wait(&(event->cond_var), &(event->os_mutex));

		/* Solaris manual said that spurious wakeups may occur: we
		have to check if the event really has been signaled after
		we came here to wait */
	}

	os_fast_mutex_unlock(&event->os_mutex);
}

/**********************************************************//**
Waits for an event object until it is in the signaled state or
a timeout is exceeded.
@return	0 if success, OS_SYNC_TIME_EXCEEDED if timeout was exceeded */
UNIV_INTERN
ulint
os_event_wait_time_low(
/*===================*/
	os_event_t	event,			/*!< in: event to wait */
	ulint		time_in_usec,		/*!< in: timeout in
						microseconds, or
						OS_SYNC_INFINITE_TIME */
	ib_int64_t	reset_sig_count)	/*!< in: zero or the value
						returned by previous call of
						os_event_reset(). */
{
	ibool		timed_out = FALSE;

#ifdef __WIN__
	DWORD		time_in_ms;
	if (time_in_usec != OS_SYNC_INFINITE_TIME) {
		time_in_ms = static_cast<DWORD>(time_in_usec / 1000);
	} else {
		time_in_ms = INFINITE;
	}
#else
	struct timespec	abstime;

	if (time_in_usec != OS_SYNC_INFINITE_TIME) {
		ulonglong usec = ulonglong(time_in_usec) + my_hrtime().val;
		abstime.tv_sec = usec / 1000000;
		abstime.tv_nsec = (usec % 1000000) * 1000;
	} else {
		abstime.tv_nsec = 999999999;
		abstime.tv_sec = (time_t) ULINT_MAX;
	}

	ut_a(abstime.tv_nsec <= 999999999);

#endif /* __WIN__ */

	os_fast_mutex_lock(&event->os_mutex);

	if (!reset_sig_count) {
		reset_sig_count = event->signal_count();
	}

	do {
		if (event->is_set()
		    || event->signal_count() != reset_sig_count) {

			break;
		}

		timed_out = os_cond_wait_timed(
			&event->cond_var, &event->os_mutex,
#ifndef __WIN__
			&abstime
#else
			time_in_ms
#endif /* !__WIN__ */
		);

	} while (!timed_out);

	os_fast_mutex_unlock(&event->os_mutex);

	return(timed_out ? OS_SYNC_TIME_EXCEEDED : 0);
}

/*********************************************************//**
Creates an operating system mutex semaphore. Because these are slow, the
mutex semaphore of InnoDB itself (ib_mutex_t) should be used where possible.
@return	the mutex handle */
UNIV_INTERN
os_ib_mutex_t
os_mutex_create(void)
/*=================*/
{
	os_fast_mutex_t*	mutex;
	os_ib_mutex_t		mutex_str;

	mutex = static_cast<os_fast_mutex_t*>(
		ut_malloc(sizeof(os_fast_mutex_t)));

	os_fast_mutex_init(os_mutex_key, mutex);

	mutex_str = static_cast<os_ib_mutex_t>(ut_malloc(sizeof *mutex_str));

	mutex_str->handle = mutex;
	mutex_str->count = 0;
	mutex_str->event = os_event_create();

	os_atomic_increment_ulint(&os_mutex_count, 1);

	return(mutex_str);
}

/**********************************************************//**
Acquires ownership of a mutex semaphore. */
UNIV_INTERN
void
os_mutex_enter(
/*===========*/
	os_ib_mutex_t	mutex)	/*!< in: mutex to acquire */
{
	os_fast_mutex_lock(static_cast<os_fast_mutex_t*>(mutex->handle));

	(mutex->count)++;

	ut_a(mutex->count == 1);
}

/**********************************************************//**
Releases ownership of a mutex. */
UNIV_INTERN
void
os_mutex_exit(
/*==========*/
	os_ib_mutex_t	mutex)	/*!< in: mutex to release */
{
	ut_a(mutex);

	ut_a(mutex->count == 1);

	(mutex->count)--;
	os_fast_mutex_unlock(static_cast<os_fast_mutex_t*>(mutex->handle));
}

/**********************************************************//**
Frees a mutex object. */
UNIV_INTERN
void
os_mutex_free(
/*==========*/
	os_ib_mutex_t	mutex)	/*!< in: mutex to free */
{
	ut_a(mutex);

	os_event_free(mutex->event);

	os_atomic_decrement_ulint(&os_mutex_count, 1);

	os_fast_mutex_free(static_cast<os_fast_mutex_t*>(mutex->handle));
	ut_free(mutex->handle);
	ut_free(mutex);
}

/*********************************************************//**
Initializes an operating system fast mutex semaphore. */
UNIV_INTERN
void
os_fast_mutex_init_func(
/*====================*/
	fast_mutex_t*		fast_mutex)	/*!< in: fast mutex */
{
#ifdef __WIN__
	ut_a(fast_mutex);

	InitializeCriticalSection((LPCRITICAL_SECTION) fast_mutex);
#else
	ut_a(0 == pthread_mutex_init(fast_mutex, MY_MUTEX_INIT_FAST));
#endif
	os_atomic_increment_ulint(&os_fast_mutex_count, 1);
}

/**********************************************************//**
Acquires ownership of a fast mutex. */
UNIV_INTERN
void
os_fast_mutex_lock_func(
/*====================*/
	fast_mutex_t*		fast_mutex)	/*!< in: mutex to acquire */
{
#ifdef __WIN__
	EnterCriticalSection((LPCRITICAL_SECTION) fast_mutex);
#else
	pthread_mutex_lock(fast_mutex);
#endif
}

/**********************************************************//**
Releases ownership of a fast mutex. */
UNIV_INTERN
void
os_fast_mutex_unlock_func(
/*======================*/
	fast_mutex_t*		fast_mutex)	/*!< in: mutex to release */
{
#ifdef __WIN__
	LeaveCriticalSection(fast_mutex);
#else
	pthread_mutex_unlock(fast_mutex);
#endif
}

/**********************************************************//**
Releases ownership of a fast mutex. Implies a full memory barrier even on
platforms such as PowerPC where this is not normally required. */
UNIV_INTERN
void
os_fast_mutex_unlock_full_barrier(
/*=================*/
	os_fast_mutex_t*	fast_mutex)	/*!< in: mutex to release */
{
#ifdef __WIN__
	LeaveCriticalSection(&fast_mutex->mutex);
#else
	pthread_mutex_unlock(&fast_mutex->mutex);
#ifdef __powerpc__
	os_mb;
#endif
#endif
}

/**********************************************************//**
Frees a mutex object. */
UNIV_INTERN
void
os_fast_mutex_free_func(
/*====================*/
	fast_mutex_t*		fast_mutex)	/*!< in: mutex to free */
{
#ifdef __WIN__
	ut_a(fast_mutex);

	DeleteCriticalSection((LPCRITICAL_SECTION) fast_mutex);
#else
	int	ret;

	ret = pthread_mutex_destroy(fast_mutex);

	if (UNIV_UNLIKELY(ret != 0)) {
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"  InnoDB: error: return value %lu when calling\n"
			"InnoDB: pthread_mutex_destroy().\n", (ulint) ret);
		fprintf(stderr,
			"InnoDB: Byte contents of the pthread mutex at %p:\n",
			(void*) fast_mutex);
		ut_print_buf(stderr, fast_mutex, sizeof(os_fast_mutex_t));
		putc('\n', stderr);
	}
#endif

	os_atomic_decrement_ulint(&os_fast_mutex_count, 1);
}
