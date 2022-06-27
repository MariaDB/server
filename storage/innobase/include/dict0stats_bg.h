/*****************************************************************************

Copyright (c) 2012, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file include/dict0stats_bg.h
Code used for background table and index stats gathering.

Created Apr 26, 2012 Vasil Dimov
*******************************************************/

#ifndef dict0stats_bg_h
#define dict0stats_bg_h

#include "dict0types.h"

#ifdef HAVE_PSI_INTERFACE
extern mysql_pfs_key_t	recalc_pool_mutex_key;
#endif /* HAVE_PSI_INTERFACE */

/** Delete a table from the auto recalc pool, and ensure that
no statistics are being updated on it. */
void dict_stats_recalc_pool_del(table_id_t id, bool have_mdl_exclusive);

/*****************************************************************//**
Initialize global variables needed for the operation of dict_stats_thread().
Must be called before dict_stats task is started. */
void dict_stats_init();

/*****************************************************************//**
Free resources allocated by dict_stats_thread_init(), must be called
after dict_stats task has exited. */
void dict_stats_deinit();

/** Start the dict stats timer. */
void dict_stats_start();

/** Shut down the dict_stats timer. */
void dict_stats_shutdown();

/** Reschedule dict stats timer to run now. */
void dict_stats_schedule_now();

#endif /* dict0stats_bg_h */
