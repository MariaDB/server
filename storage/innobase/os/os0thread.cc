/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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
@file os/os0thread.cc
The interface to the operating system thread control primitives

Created 9/8/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include "srv0srv.h"

/** Number of threads active. */
ulint	os_thread_count;

/***************************************************************//**
Compares two thread ids for equality.
@return TRUE if equal */
ibool
os_thread_eq(
/*=========*/
	os_thread_id_t	a,	/*!< in: OS thread or thread id */
	os_thread_id_t	b)	/*!< in: OS thread or thread id */
{
#ifdef _WIN32
	if (a == b) {
		return(TRUE);
	}

	return(FALSE);
#else
	if (pthread_equal(a, b)) {
		return(TRUE);
	}

	return(FALSE);
#endif
}

/****************************************************************//**
Converts an OS thread id to a ulint. It is NOT guaranteed that the ulint is
unique for the thread though!
@return thread identifier as a number */
ulint
os_thread_pf(
/*=========*/
	os_thread_id_t	a)	/*!< in: OS thread identifier */
{
	return((ulint) a);
}

/*****************************************************************//**
Returns the thread identifier of current thread. Currently the thread
identifier in Unix is the thread handle itself. Note that in HP-UX
pthread_t is a struct of 3 fields.
@return current thread identifier */
os_thread_id_t
os_thread_get_curr_id(void)
/*=======================*/
{
#ifdef _WIN32
	return(GetCurrentThreadId());
#else
	return(pthread_self());
#endif
}

/****************************************************************//**
Creates a new thread of execution. The execution starts from
the function given.
NOTE: We count the number of threads in os_thread_exit(). A created
thread should always use that to exit so thatthe thread count will be
decremented.
We do not return an error code because if there is one, we crash here. */
os_thread_t
os_thread_create_func(
/*==================*/
	os_thread_func_t	func,		/*!< in: pointer to function
						from which to start */
	void*			arg,		/*!< in: argument to start
						function */
	os_thread_id_t*		thread_id)	/*!< out: id of the created
						thread, or NULL */
{
	os_thread_id_t	new_thread_id;

#ifdef _WIN32
	HANDLE		handle;

	handle = CreateThread(NULL,	/* no security attributes */
			      0,	/* default size stack */
			      func,
			      arg,
			      0,	/* thread runs immediately */
			      &new_thread_id);

	if (!handle) {
		/* If we cannot start a new thread, life has no meaning. */
		ib::fatal() << "CreateThread returned " << GetLastError();
	}

	CloseHandle(handle);

	my_atomic_addlint(&os_thread_count, 1);

	return((os_thread_t)new_thread_id);
#else /* _WIN32 else */

	pthread_attr_t	attr;

	int	ret = pthread_attr_init(&attr);
	if (UNIV_UNLIKELY(ret)) {
		fprintf(stderr,
			"InnoDB: Error: pthread_attr_init() returned %d\n",
			ret);
		abort();
	}

	my_atomic_addlint(&os_thread_count, 1);

	ret = pthread_create(&new_thread_id, &attr, func, arg);

	ut_a(ret == 0);

	pthread_attr_destroy(&attr);

#endif /* not _WIN32 */

	ut_a(os_thread_count <= OS_THREAD_MAX_N);

	/* Return the thread_id if the caller requests it. */
	if (thread_id != NULL) {
		*thread_id = new_thread_id;
	}
	return((os_thread_t)new_thread_id);
}

/** Waits until the specified thread completes and joins it.
Its return value is ignored.
@param[in,out]	thread	thread to join */
void
os_thread_join(
	os_thread_id_t	thread)
{
#ifdef _WIN32
	/* Do nothing. */
#else
#ifdef UNIV_DEBUG
	const int	ret =
#endif /* UNIV_DEBUG */
	pthread_join(thread, NULL);

	/* Waiting on already-quit threads is allowed. */
	ut_ad(ret == 0 || ret == ESRCH);
#endif /* _WIN32 */
}

/** Exits the current thread.
@param[in]	detach	if true, the thread will be detached right before
exiting. If false, another thread is responsible for joining this thread */
ATTRIBUTE_NORETURN
void
os_thread_exit(bool detach)
{
#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Thread exits, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif

#ifdef UNIV_PFS_THREAD
	pfs_delete_thread();
#endif

	my_atomic_addlint(&os_thread_count, -1);

#ifdef _WIN32
	ExitThread(0);
#else
	if (detach) {
		pthread_detach(pthread_self());
	}
	pthread_exit(NULL);
#endif
}

/*****************************************************************//**
Advises the os to give up remainder of the thread's time slice. */
void
os_thread_yield(void)
/*=================*/
{
#if defined(_WIN32)
	SwitchToThread();
#else
	sched_yield();
#endif
}

/*****************************************************************//**
The thread sleeps at least the time given in microseconds. */
void
os_thread_sleep(
/*============*/
	ulint	tm)	/*!< in: time in microseconds */
{
#ifdef _WIN32
	Sleep((DWORD) tm / 1000);
#elif defined(HAVE_NANOSLEEP)
	struct timespec	t;

	t.tv_sec = tm / 1000000;
	t.tv_nsec = (tm % 1000000) * 1000;

	::nanosleep(&t, NULL);
#else
	struct timeval  t;

	t.tv_sec = tm / 1000000;
	t.tv_usec = tm % 1000000;

	select(0, NULL, NULL, NULL, &t);
#endif /* _WIN32 */
}
