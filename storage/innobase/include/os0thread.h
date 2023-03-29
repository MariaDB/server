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
@file include/os0thread.h
The interface to the operating system
process and thread control primitives

Created 9/8/1995 Heikki Tuuri
*******************************************************/

#ifndef os0thread_h
#define os0thread_h

#include "univ.i"

/* Possible fixed priorities for threads */
#define OS_THREAD_PRIORITY_NONE		100
#define OS_THREAD_PRIORITY_BACKGROUND	1
#define OS_THREAD_PRIORITY_NORMAL	2
#define OS_THREAD_PRIORITY_ABOVE_NORMAL	3

#ifdef _WIN32
typedef DWORD			os_thread_t;
typedef DWORD			os_thread_id_t;	/*!< In Windows the thread id
						is an unsigned long int */
extern "C"  {
typedef LPTHREAD_START_ROUTINE	os_thread_func_t;
}

/** Macro for specifying a Windows thread start function. */
#define DECLARE_THREAD(func)	WINAPI func
#else

typedef pthread_t		os_thread_t;
typedef pthread_t		os_thread_id_t;	/*!< In Unix we use the thread
						handle itself as the id of
						the thread */
extern "C"  { typedef void*	(*os_thread_func_t)(void*); }

/** Macro for specifying a POSIX thread start function. */
#define DECLARE_THREAD(func)	func
#endif /* _WIN32 */

/* Define a function pointer type to use in a typecast */
typedef void* (*os_posix_f_t) (void*);

#ifdef HAVE_PSI_INTERFACE
/* Define for performance schema registration key */
typedef unsigned int    mysql_pfs_key_t;
#endif /* HAVE_PSI_INTERFACE */

#ifndef _WIN32
#define os_thread_eq(a,b) pthread_equal(a, b)
#define os_thread_yield() sched_yield()
#define os_thread_get_curr_id() pthread_self()
#else
bool os_thread_eq(os_thread_id_t a, os_thread_id_t b);
void os_thread_yield();
os_thread_id_t os_thread_get_curr_id();
#endif

/****************************************************************//**
Creates a new thread of execution. The execution starts from
the function given.
NOTE: We count the number of threads in os_thread_exit(). A created
thread should always use that to exit so thatthe thread count will be
decremented.
We do not return an error code because if there is one, we crash here. */
os_thread_t os_thread_create(os_thread_func_t func, void *arg= nullptr);

/** Detach and terminate the current thread. */
os_thread_ret_t os_thread_exit();

/*****************************************************************//**
The thread sleeps at least the time given in microseconds. */
void
os_thread_sleep(
/*============*/
	ulint	tm);	/*!< in: time in microseconds */

#endif
