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
@file dict/dict0stats_bg.cc
Code used for background table and index stats gathering.

Created Apr 25, 2012 Vasil Dimov
*******************************************************/

#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "dict0defrag_bg.h"
#include "row0mysql.h"
#include "srv0start.h"
#include "fil0fil.h"
#ifdef WITH_WSREP
# include "trx0trx.h"
# include "mysql/service_wsrep.h"
# include "wsrep.h"
# include "log.h"
# include "wsrep_mysqld.h"
#endif

#include <vector>

/** Minimum time interval between stats recalc for a given table */
#define MIN_RECALC_INTERVAL	10 /* seconds */

/** Event to wake up dict_stats_thread on dict_stats_recalc_pool_add()
or shutdown. Not protected by any mutex. */
os_event_t			dict_stats_event;

/** Variable to initiate shutdown the dict stats thread. Note we don't
use 'srv_shutdown_state' because we want to shutdown dict stats thread
before purge thread. */
bool				dict_stats_start_shutdown;

/** Event to wait for shutdown of the dict stats thread */
os_event_t			dict_stats_shutdown_event;

#ifdef UNIV_DEBUG
/** Used by SET GLOBAL innodb_dict_stats_disabled_debug = 1; */
my_bool				innodb_dict_stats_disabled_debug;

static os_event_t		dict_stats_disabled_event;
#endif /* UNIV_DEBUG */

/** This mutex protects the "recalc_pool" variable. */
static ib_mutex_t		recalc_pool_mutex;

/** Allocator type, used by std::vector */
typedef ut_allocator<table_id_t>
	recalc_pool_allocator_t;

/** The multitude of tables whose stats are to be automatically
recalculated - an STL vector */
typedef std::vector<table_id_t, recalc_pool_allocator_t>
	recalc_pool_t;

/** Iterator type for iterating over the elements of objects of type
recalc_pool_t. */
typedef recalc_pool_t::iterator
	recalc_pool_iterator_t;

/** Pool where we store information on which tables are to be processed
by background statistics gathering. */
static recalc_pool_t		recalc_pool;

/*****************************************************************//**
Free the resources occupied by the recalc pool, called once during
thread de-initialization. */
static void dict_stats_recalc_pool_deinit()
{
	ut_ad(!srv_read_only_mode);

	recalc_pool.clear();
	defrag_pool.clear();
        /*
          recalc_pool may still have its buffer allocated. It will free it when
          its destructor is called.
          The problem is, memory leak detector is run before the recalc_pool's
          destructor is invoked, and will report recalc_pool's buffer as leaked
          memory.  To avoid that, we force recalc_pool to surrender its buffer
          to empty_pool object, which will free it when leaving this function:
        */
	recalc_pool_t recalc_empty_pool;
	defrag_pool_t defrag_empty_pool;
	recalc_pool.swap(recalc_empty_pool);
	defrag_pool.swap(defrag_empty_pool);
}

/*****************************************************************//**
Add a table to the recalc pool, which is processed by the
background stats gathering thread. Only the table id is added to the
list, so the table can be closed after being enqueued and it will be
opened when needed. If the table does not exist later (has been DROPped),
then it will be removed from the pool and skipped. */
static
void
dict_stats_recalc_pool_add(
/*=======================*/
	const dict_table_t*	table)	/*!< in: table to add */
{
	ut_ad(!srv_read_only_mode);

	mutex_enter(&recalc_pool_mutex);

	/* quit if already in the list */
	for (recalc_pool_iterator_t iter = recalc_pool.begin();
	     iter != recalc_pool.end();
	     ++iter) {

		if (*iter == table->id) {
			mutex_exit(&recalc_pool_mutex);
			return;
		}
	}

	recalc_pool.push_back(table->id);

	mutex_exit(&recalc_pool_mutex);

	os_event_set(dict_stats_event);
}

#ifdef WITH_WSREP
/** Update the table modification counter and if necessary,
schedule new estimates for table and index statistics to be calculated.
@param[in,out]	table	persistent or temporary table
@param[in]	thd	current session */
void dict_stats_update_if_needed(dict_table_t *table, const trx_t &trx)
#else
/** Update the table modification counter and if necessary,
schedule new estimates for table and index statistics to be calculated.
@param[in,out]	table	persistent or temporary table */
void dict_stats_update_if_needed_func(dict_table_t *table)
#endif
{
	ut_ad(!mutex_own(&dict_sys->mutex));

	if (UNIV_UNLIKELY(!table->stat_initialized)) {
		/* The table may have been evicted from dict_sys
		and reloaded internally by InnoDB for FOREIGN KEY
		processing, but not reloaded by the SQL layer.

		We can (re)compute the transient statistics when the
		table is actually loaded by the SQL layer.

		Note: If InnoDB persistent statistics are enabled,
		we will skip the updates. We must do this, because
		dict_table_get_n_rows() below assumes that the
		statistics have been initialized. The DBA may have
		to execute ANALYZE TABLE. */
		return;
	}

	ulonglong	counter = table->stat_modified_counter++;
	ulonglong	n_rows = dict_table_get_n_rows(table);

	if (dict_stats_is_persistent_enabled(table)) {
		if (counter > n_rows / 10 /* 10% */
		    && dict_stats_auto_recalc_is_enabled(table)) {

#ifdef WITH_WSREP
			/* Do not add table to background
			statistic calculation if this thread is not a
			applier (as all DDL, which is replicated (i.e
			is binlogged in master node), will be executed
			with high priority (a.k.a BF) in slave nodes)
			and is BF. This could again lead BF lock
			waits in applier node but it is better than
			no persistent index/table statistics at
			applier nodes. TODO: allow BF threads
			wait for these InnoDB internal SQL-parser
			generated row locks and allow BF thread
			lock waits to be enqueued at head of waiting
			queue. */
			if (trx.is_wsrep()
			    && !wsrep_thd_is_applier(trx.mysql_thd)
			    && wsrep_thd_is_BF(trx.mysql_thd, 0)) {
				WSREP_DEBUG("Avoiding background statistics"
					    " calculation for table %s.",
					table->name.m_name);
				return;
			}
#endif /* WITH_WSREP */

			dict_stats_recalc_pool_add(table);
			table->stat_modified_counter = 0;
		}
		return;
	}

	/* Calculate new statistics if 1 / 16 of table has been modified
	since the last time a statistics batch was run.
	We calculate statistics at most every 16th round, since we may have
	a counter table which is very small and updated very often. */
	ulonglong threshold = 16 + n_rows / 16; /* 6.25% */

	if (srv_stats_modified_counter) {
		threshold = std::min(srv_stats_modified_counter, threshold);
	}

	if (counter > threshold) {
		/* this will reset table->stat_modified_counter to 0 */
		dict_stats_update(table, DICT_STATS_RECALC_TRANSIENT);
	}
}

/*****************************************************************//**
Get a table from the auto recalc pool. The returned table id is removed
from the pool.
@return true if the pool was non-empty and "id" was set, false otherwise */
static
bool
dict_stats_recalc_pool_get(
/*=======================*/
	table_id_t*	id)	/*!< out: table id, or unmodified if list is
				empty */
{
	ut_ad(!srv_read_only_mode);

	mutex_enter(&recalc_pool_mutex);

	if (recalc_pool.empty()) {
		mutex_exit(&recalc_pool_mutex);
		return(false);
	}

	*id = recalc_pool.at(0);

	recalc_pool.erase(recalc_pool.begin());

	mutex_exit(&recalc_pool_mutex);

	return(true);
}

/*****************************************************************//**
Delete a given table from the auto recalc pool.
dict_stats_recalc_pool_del() */
void
dict_stats_recalc_pool_del(
/*=======================*/
	const dict_table_t*	table)	/*!< in: table to remove */
{
	ut_ad(!srv_read_only_mode);
	ut_ad(mutex_own(&dict_sys->mutex));

	mutex_enter(&recalc_pool_mutex);

	ut_ad(table->id > 0);

	for (recalc_pool_iterator_t iter = recalc_pool.begin();
	     iter != recalc_pool.end();
	     ++iter) {

		if (*iter == table->id) {
			/* erase() invalidates the iterator */
			recalc_pool.erase(iter);
			break;
		}
	}

	mutex_exit(&recalc_pool_mutex);
}

/*****************************************************************//**
Wait until background stats thread has stopped using the specified table.
The caller must have locked the data dictionary using
row_mysql_lock_data_dictionary() and this function may unlock it temporarily
and restore the lock before it exits.
The background stats thread is guaranteed not to start using the specified
table after this function returns and before the caller unlocks the data
dictionary because it sets the BG_STAT_IN_PROGRESS bit in table->stats_bg_flag
under dict_sys->mutex. */
void
dict_stats_wait_bg_to_stop_using_table(
/*===================================*/
	dict_table_t*	table,	/*!< in/out: table */
	trx_t*		trx)	/*!< in/out: transaction to use for
				unlocking/locking the data dict */
{
	while (!dict_stats_stop_bg(table)) {
		DICT_BG_YIELD(trx);
	}
}

/*****************************************************************//**
Initialize global variables needed for the operation of dict_stats_thread()
Must be called before dict_stats_thread() is started. */
void
dict_stats_thread_init()
{
	ut_a(!srv_read_only_mode);

	dict_stats_event = os_event_create(0);
	dict_stats_shutdown_event = os_event_create(0);

	ut_d(dict_stats_disabled_event = os_event_create(0));

	/* The recalc_pool_mutex is acquired from:
	1) the background stats gathering thread before any other latch
	   and released without latching anything else in between (thus
	   any level would do here)
	2) from dict_stats_update_if_needed()
	   and released without latching anything else in between. We know
	   that dict_sys->mutex (SYNC_DICT) is not acquired when
	   dict_stats_update_if_needed() is called and it may be acquired
	   inside that function (thus a level <=SYNC_DICT would do).
	3) from row_drop_table_for_mysql() after dict_sys->mutex (SYNC_DICT)
	   and dict_operation_lock (SYNC_DICT_OPERATION) have been locked
	   (thus a level <SYNC_DICT && <SYNC_DICT_OPERATION would do)
	So we choose SYNC_STATS_AUTO_RECALC to be about below SYNC_DICT. */

	mutex_create(LATCH_ID_RECALC_POOL, &recalc_pool_mutex);

	dict_defrag_pool_init();
}

/*****************************************************************//**
Free resources allocated by dict_stats_thread_init(), must be called
after dict_stats_thread() has exited. */
void
dict_stats_thread_deinit()
/*======================*/
{
	ut_a(!srv_read_only_mode);
	ut_ad(!srv_dict_stats_thread_active);

	dict_stats_recalc_pool_deinit();
	dict_defrag_pool_deinit();

	mutex_free(&recalc_pool_mutex);

	ut_d(os_event_destroy(dict_stats_disabled_event));
	os_event_destroy(dict_stats_event);
	os_event_destroy(dict_stats_shutdown_event);
	dict_stats_start_shutdown = false;
}

/*****************************************************************//**
Get the first table that has been added for auto recalc and eventually
update its stats. */
static
void
dict_stats_process_entry_from_recalc_pool()
/*=======================================*/
{
	table_id_t	table_id;

	ut_ad(!srv_read_only_mode);

	/* pop the first table from the auto recalc pool */
	if (!dict_stats_recalc_pool_get(&table_id)) {
		/* no tables for auto recalc */
		return;
	}

	dict_table_t*	table;

	mutex_enter(&dict_sys->mutex);

	table = dict_table_open_on_id(table_id, TRUE, DICT_TABLE_OP_NORMAL);

	if (table == NULL) {
		/* table does not exist, must have been DROPped
		after its id was enqueued */
		mutex_exit(&dict_sys->mutex);
		return;
	}

	ut_ad(!dict_table_is_temporary(table));

	if (!fil_table_accessible(table)) {
		dict_table_close(table, TRUE, FALSE);
		mutex_exit(&dict_sys->mutex);
		return;
	}

	table->stats_bg_flag |= BG_STAT_IN_PROGRESS;

	mutex_exit(&dict_sys->mutex);

	/* time() could be expensive, the current function
	is called once every time a table has been changed more than 10% and
	on a system with lots of small tables, this could become hot. If we
	find out that this is a problem, then the check below could eventually
	be replaced with something else, though a time interval is the natural
	approach. */

	if (difftime(time(NULL), table->stats_last_recalc)
	    < MIN_RECALC_INTERVAL) {

		/* Stats were (re)calculated not long ago. To avoid
		too frequent stats updates we put back the table on
		the auto recalc list and do nothing. */

		dict_stats_recalc_pool_add(table);

	} else {

		dict_stats_update(table, DICT_STATS_RECALC_PERSISTENT);
	}

	mutex_enter(&dict_sys->mutex);

	table->stats_bg_flag = BG_STAT_NONE;

	dict_table_close(table, TRUE, FALSE);

	mutex_exit(&dict_sys->mutex);
}

#ifdef UNIV_DEBUG
/** Disables dict stats thread. It's used by:
	SET GLOBAL innodb_dict_stats_disabled_debug = 1 (0).
@param[in]	thd		thread handle
@param[in]	var		pointer to system variable
@param[out]	var_ptr		where the formal string goes
@param[in]	save		immediate result from check function */
void
dict_stats_disabled_debug_update(
	THD*				thd,
	struct st_mysql_sys_var*	var,
	void*				var_ptr,
	const void*			save)
{
	/* This method is protected by mutex, as every SET GLOBAL .. */
	ut_ad(dict_stats_disabled_event != NULL);

	const bool disable = *static_cast<const my_bool*>(save);

	const int64_t sig_count = os_event_reset(dict_stats_disabled_event);

	innodb_dict_stats_disabled_debug = disable;

	if (disable) {
		os_event_set(dict_stats_event);
		os_event_wait_low(dict_stats_disabled_event, sig_count);
	}
}
#endif /* UNIV_DEBUG */


/*****************************************************************//**
This is the thread for background stats gathering. It pops tables, from
the auto recalc list and proceeds them, eventually recalculating their
statistics.
@return this function does not return, it calls os_thread_exit() */
extern "C"
os_thread_ret_t
DECLARE_THREAD(dict_stats_thread)(void*)
{
	my_thread_init();
	ut_a(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	/* JAN: TODO: MySQL 5.7 PSI
	pfs_register_thread(dict_stats_thread_key);
	*/
#endif /* UNIV_PFS_THREAD */

	while (!dict_stats_start_shutdown) {

		/* Wake up periodically even if not signaled. This is
		because we may lose an event - if the below call to
		dict_stats_process_entry_from_recalc_pool() puts the entry back
		in the list, the os_event_set() will be lost by the subsequent
		os_event_reset(). */
		os_event_wait_time(
			dict_stats_event, MIN_RECALC_INTERVAL * 1000000);

#ifdef UNIV_DEBUG
		while (innodb_dict_stats_disabled_debug) {
			os_event_set(dict_stats_disabled_event);
			if (dict_stats_start_shutdown) {
				break;
			}
			os_event_wait_time(
				dict_stats_event, 100000);
		}
#endif /* UNIV_DEBUG */

		if (dict_stats_start_shutdown) {
			break;
		}

		dict_stats_process_entry_from_recalc_pool();
		dict_defrag_process_entries_from_defrag_pool();

		os_event_reset(dict_stats_event);
	}

	srv_dict_stats_thread_active = false;

	os_event_set(dict_stats_shutdown_event);
	my_thread_end();

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit instead of return(). */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/** Shut down the dict_stats_thread. */
void
dict_stats_shutdown()
{
	dict_stats_start_shutdown = true;
	os_event_set(dict_stats_event);
	os_event_wait(dict_stats_shutdown_event);
}
