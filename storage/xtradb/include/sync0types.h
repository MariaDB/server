/*****************************************************************************

Copyright (c) 1995, 2009, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/sync0types.h
Global types for sync

Created 9/5/1995 Heikki Tuuri
*******************************************************/

#ifndef sync0types_h
#define sync0types_h

struct ib_mutex_t;

/* The relative priority of the current thread.  If 0, low priority; if 1, high
priority.  */
extern UNIV_THREAD_LOCAL ulint srv_current_thread_priority;

struct ib_prio_mutex_t;

/** Priority mutex and rwlatch acquisition priorities */
enum ib_sync_priority {
	IB_DEFAULT_PRIO,
	IB_LOW_PRIO,
	IB_HIGH_PRIO
};

#endif
