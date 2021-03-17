/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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

#pragma once
#include "univ.i"

#ifdef _WIN32
typedef DWORD			os_thread_id_t;	/*!< In Windows the thread id
						is an unsigned long int */
#else

typedef pthread_t		os_thread_id_t;	/*!< In Unix we use the thread
						handle itself as the id of
						the thread */
#endif /* _WIN32 */

#define os_thread_eq(a,b) IF_WIN(a == b, pthread_equal(a, b))
#define os_thread_get_curr_id() IF_WIN(GetCurrentThreadId(), pthread_self())
