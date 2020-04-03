/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

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
@file os/os0proc.cc
The interface to the operating system
process control primitives

Created 9/30/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"

/** The total amount of memory currently allocated from the operating
system with allocacte_large(). */
Atomic_counter<ulint>	os_total_large_mem_allocated;

/** Converts the current process id to a number.
@return process id as a number */
ulint
os_proc_get_number(void)
/*====================*/
{
#ifdef _WIN32
	return(static_cast<ulint>(GetCurrentProcessId()));
#else
	return(static_cast<ulint>(getpid()));
#endif
}
