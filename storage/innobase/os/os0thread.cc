/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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

#ifdef _WIN32
bool os_thread_eq(os_thread_id_t a, os_thread_id_t b) { return a == b; }
void os_thread_yield() { SwitchToThread(); }
os_thread_id_t os_thread_get_curr_id() { return GetCurrentThreadId(); }
#endif

/****************************************************************//**
Creates a new thread of execution. The execution starts from
the function given.
NOTE: We count the number of threads in os_thread_exit(). A created
thread should always use that to exit so thatthe thread count will be
decremented.
We do not return an error code because if there is one, we crash here. */
os_thread_t os_thread_create(os_thread_func_t func, void *arg)
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

	ret = pthread_create(&new_thread_id, &attr, func, arg);

	ut_a(ret == 0);

	pthread_attr_destroy(&attr);

#endif /* not _WIN32 */

	return((os_thread_t)new_thread_id);
}

/** Detach and terminate the current thread. */
os_thread_ret_t os_thread_exit()
{
#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "Thread exits, id " << os_thread_get_curr_id();
#endif

#ifdef UNIV_PFS_THREAD
	pfs_delete_thread();
#endif

#ifdef _WIN32
	ExitThread(0);
#else
	pthread_detach(pthread_self());
#endif
	OS_THREAD_DUMMY_RETURN;
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
