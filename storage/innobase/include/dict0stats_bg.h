/*****************************************************************************

Copyright (c) 2012, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/dict0stats_bg.h
Code used for background table and index stats gathering.

Created Apr 26, 2012 Vasil Dimov
*******************************************************/

#ifndef dict0stats_bg_h
#define dict0stats_bg_h

#include "dict0types.h"
#include "os0thread.h"

#ifdef HAVE_PSI_INTERFACE
extern mysql_pfs_key_t	recalc_pool_mutex_key;
#endif /* HAVE_PSI_INTERFACE */

#ifdef UNIV_DEBUG
/** Value of MySQL global used to disable dict_stats thread. */
extern my_bool		innodb_dict_stats_disabled_debug;
#endif /* UNIV_DEBUG */

/*****************************************************************//**
Delete a given table from the auto recalc pool.
dict_stats_recalc_pool_del() */
void
dict_stats_recalc_pool_del(
/*=======================*/
	const dict_table_t*	table);	/*!< in: table to remove */

/** Yield the data dictionary latch when waiting
for the background thread to stop accessing a table.
@param trx	transaction holding the data dictionary locks */
#define DICT_BG_YIELD	do {	\
	dict_sys.unlock();	\
	std::this_thread::sleep_for(std::chrono::milliseconds(250));	\
	dict_sys.lock(SRW_LOCK_CALL); \
} while (0)

/*****************************************************************//**
Request the background collection of statistics to stop for a table.
@retval true when no background process is active
@retval false when it is not safe to modify the table definition */
UNIV_INLINE
bool
dict_stats_stop_bg(
/*===============*/
	dict_table_t*	table)	/*!< in/out: table */
{
	ut_ad(!srv_read_only_mode);
	ut_ad(dict_sys.locked());

	if (!(table->stats_bg_flag & BG_STAT_IN_PROGRESS)) {
		return(true);
	}

	/* In dict_stats_update_persistent() this flag is being read
	while holding the mutex, not dict_sys.latch. */
	table->stats_mutex_lock();
	table->stats_bg_flag |= BG_STAT_SHOULD_QUIT;
	table->stats_mutex_unlock();
	return(false);
}

/*****************************************************************//**
Wait until background stats thread has stopped using the specified table.
The background stats thread is guaranteed not to start using the specified
table after this function returns and before the caller releases
dict_sys.latch. */
void dict_stats_wait_bg_to_stop_using_table(dict_table_t *table);

/*****************************************************************//**
Initialize global variables needed for the operation of dict_stats_thread().
Must be called before dict_stats task is started. */
void dict_stats_init();

/*****************************************************************//**
Free resources allocated by dict_stats_thread_init(), must be called
after dict_stats task has exited. */
void dict_stats_deinit();

#ifdef UNIV_DEBUG
/** Disables dict stats thread. It's used by:
	SET GLOBAL innodb_dict_stats_disabled_debug = 1 (0).
@param[in]	save		immediate result from check function */
void dict_stats_disabled_debug_update(THD*, st_mysql_sys_var*, void*,
				      const void* save);
#endif /* UNIV_DEBUG */

/** Start the dict stats timer. */
void dict_stats_start();

/** Shut down the dict_stats timer. */
void dict_stats_shutdown();

/** Reschedule dict stats timer to run now. */
void dict_stats_schedule_now();

#endif /* dict0stats_bg_h */
