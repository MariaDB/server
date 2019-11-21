/*****************************************************************************

Copyright (c) 2012, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
@file dict/dict0stats_bg.cc
Code used for background table and index stats gathering.

Created Apr 25, 2012 Vasil Dimov
*******************************************************/

#include "row0mysql.h"
#include "srv0start.h"
#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"

#ifdef UNIV_NONINL
# include "dict0stats_bg.ic"
#endif

#include <vector>

/** Minimum time interval between stats recalc for a given table */
#define MIN_RECALC_INTERVAL	10 /* seconds */

/** Event to wake up dict_stats_thread on dict_stats_recalc_pool_add()
or shutdown. Not protected by any mutex. */
UNIV_INTERN os_event_t		dict_stats_event;

/** Variable to initiate shutdown the dict stats thread. Note we don't
use 'srv_shutdown_state' because we want to shutdown dict stats thread
before purge thread. */
static bool			dict_stats_start_shutdown;

/** Event to wait for shutdown of the dict stats thread */
static os_event_t		dict_stats_shutdown_event;

/** This mutex protects the "recalc_pool" variable. */
static ib_mutex_t		recalc_pool_mutex;
static ib_mutex_t		defrag_pool_mutex;
#ifdef HAVE_PSI_INTERFACE
static mysql_pfs_key_t		recalc_pool_mutex_key;
static mysql_pfs_key_t		defrag_pool_mutex_key;
#endif /* HAVE_PSI_INTERFACE */

/** The number of tables that can be added to "recalc_pool" before
it is enlarged */
static const ulint RECALC_POOL_INITIAL_SLOTS = 128;

/** The multitude of tables whose stats are to be automatically
recalculated - an STL vector */
typedef std::vector<table_id_t>	recalc_pool_t;
static recalc_pool_t		recalc_pool;

typedef recalc_pool_t::iterator	recalc_pool_iterator_t;

/** Indices whose defrag stats need to be saved to persistent storage.*/
struct defrag_pool_item_t {
	table_id_t	table_id;
	index_id_t	index_id;
};
typedef std::vector<defrag_pool_item_t>	defrag_pool_t;
static defrag_pool_t			defrag_pool;
typedef defrag_pool_t::iterator		defrag_pool_iterator_t;

/*****************************************************************//**
Initialize the recalc pool, called once during thread initialization. */
static
void
dict_stats_pool_init()
/*=========================*/
{
	ut_ad(!srv_read_only_mode);

	recalc_pool.reserve(RECALC_POOL_INITIAL_SLOTS);
	defrag_pool.reserve(RECALC_POOL_INITIAL_SLOTS);
}

/*****************************************************************//**
Free the resources occupied by the recalc pool, called once during
thread de-initialization. */
static void dict_stats_pool_deinit()
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
UNIV_INTERN
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

	*id = recalc_pool[0];

	recalc_pool.erase(recalc_pool.begin());

	mutex_exit(&recalc_pool_mutex);

	return(true);
}

/*****************************************************************//**
Delete a given table from the auto recalc pool.
dict_stats_recalc_pool_del() */
UNIV_INTERN
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
Add an index in a table to the defrag pool, which is processed by the
background stats gathering thread. Only the table id and index id are
added to the list, so the table can be closed after being enqueued and
it will be opened when needed. If the table or index does not exist later
(has been DROPped), then it will be removed from the pool and skipped. */
UNIV_INTERN
void
dict_stats_defrag_pool_add(
/*=======================*/
	const dict_index_t*	index)	/*!< in: table to add */
{
	defrag_pool_item_t item;

	ut_ad(!srv_read_only_mode);

	mutex_enter(&defrag_pool_mutex);

	/* quit if already in the list */
	for (defrag_pool_iterator_t iter = defrag_pool.begin();
	     iter != defrag_pool.end();
	     ++iter) {
		if ((*iter).table_id == index->table->id
		    && (*iter).index_id == index->id) {
			mutex_exit(&defrag_pool_mutex);
			return;
		}
	}

	item.table_id = index->table->id;
	item.index_id = index->id;
	defrag_pool.push_back(item);

	mutex_exit(&defrag_pool_mutex);

	os_event_set(dict_stats_event);
}

/*****************************************************************//**
Get an index from the auto defrag pool. The returned index id is removed
from the pool.
@return true if the pool was non-empty and "id" was set, false otherwise */
static
bool
dict_stats_defrag_pool_get(
/*=======================*/
	table_id_t*	table_id,	/*!< out: table id, or unmodified if
					list is empty */
	index_id_t*	index_id)	/*!< out: index id, or unmodified if
					list is empty */
{
	ut_ad(!srv_read_only_mode);

	mutex_enter(&defrag_pool_mutex);

	if (defrag_pool.empty()) {
		mutex_exit(&defrag_pool_mutex);
		return(false);
	}

	defrag_pool_item_t& item = defrag_pool.back();
	*table_id = item.table_id;
	*index_id = item.index_id;

	defrag_pool.pop_back();

	mutex_exit(&defrag_pool_mutex);

	return(true);
}

/*****************************************************************//**
Delete a given index from the auto defrag pool. */
UNIV_INTERN
void
dict_stats_defrag_pool_del(
/*=======================*/
	const dict_table_t*	table,	/*!<in: if given, remove
					all entries for the table */
	const dict_index_t*	index)	/*!< in: if given, remove this index */
{
	ut_a((table && !index) || (!table && index));
	ut_ad(!srv_read_only_mode);
	ut_ad(mutex_own(&dict_sys->mutex));

	mutex_enter(&defrag_pool_mutex);

	defrag_pool_iterator_t iter = defrag_pool.begin();
	while (iter != defrag_pool.end()) {
		if ((table && (*iter).table_id == table->id)
		    || (index
			&& (*iter).table_id == index->table->id
			&& (*iter).index_id == index->id)) {
			/* erase() invalidates the iterator */
			iter = defrag_pool.erase(iter);
			if (index)
				break;
		} else {
			iter++;
		}
	}

	mutex_exit(&defrag_pool_mutex);
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
UNIV_INTERN
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
UNIV_INTERN
void
dict_stats_thread_init()
{
	ut_a(!srv_read_only_mode);

	dict_stats_event = os_event_create();
	dict_stats_shutdown_event = os_event_create();

	/* The recalc_pool_mutex is acquired from:
	1) the background stats gathering thread before any other latch
	   and released without latching anything else in between (thus
	   any level would do here)
	2) from row_update_statistics_if_needed()
	   and released without latching anything else in between. We know
	   that dict_sys->mutex (SYNC_DICT) is not acquired when
	   row_update_statistics_if_needed() is called and it may be acquired
	   inside that function (thus a level <=SYNC_DICT would do).
	3) from row_drop_table_for_mysql() after dict_sys->mutex (SYNC_DICT)
	   and dict_operation_lock (SYNC_DICT_OPERATION) have been locked
	   (thus a level <SYNC_DICT && <SYNC_DICT_OPERATION would do)
	So we choose SYNC_STATS_AUTO_RECALC to be about below SYNC_DICT. */
	mutex_create(recalc_pool_mutex_key, &recalc_pool_mutex,
		     SYNC_STATS_AUTO_RECALC);

	/* We choose SYNC_STATS_DEFRAG to be below SYNC_FSP_PAGE. */
	mutex_create(defrag_pool_mutex_key, &defrag_pool_mutex,
	     SYNC_STATS_DEFRAG);
	dict_stats_pool_init();
}

/*****************************************************************//**
Free resources allocated by dict_stats_thread_init(), must be called
after dict_stats_thread() has exited. */
UNIV_INTERN
void
dict_stats_thread_deinit()
/*======================*/
{
	ut_a(!srv_read_only_mode);
	ut_ad(!srv_dict_stats_thread_active);

	dict_stats_pool_deinit();

	mutex_free(&recalc_pool_mutex);
	memset(&recalc_pool_mutex, 0x0, sizeof(recalc_pool_mutex));

	mutex_free(&defrag_pool_mutex);
	memset(&defrag_pool_mutex, 0x0, sizeof(defrag_pool_mutex));

	os_event_free(dict_stats_event);
	dict_stats_event = NULL;
	os_event_free(dict_stats_shutdown_event);
	dict_stats_shutdown_event = NULL;
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

	/* Check whether table is corrupted */
	if (table->corrupted) {
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

	table->stats_bg_flag &= ~BG_STAT_IN_PROGRESS;

	dict_table_close(table, TRUE, FALSE);

	mutex_exit(&dict_sys->mutex);
}

/*****************************************************************//**
Get the first index that has been added for updating persistent defrag
stats and eventually save its stats. */
static
void
dict_stats_process_entry_from_defrag_pool()
{
	table_id_t	table_id;
	index_id_t	index_id;

	ut_ad(!srv_read_only_mode);

	/* pop the first index from the auto defrag pool */
	if (!dict_stats_defrag_pool_get(&table_id, &index_id)) {
		/* no index in defrag pool */
		return;
	}

	dict_table_t*	table;

	mutex_enter(&dict_sys->mutex);

	/* If the table is no longer cached, we've already lost the in
	memory stats so there's nothing really to write to disk. */
	table = dict_table_open_on_id(table_id, TRUE,
				      DICT_TABLE_OP_OPEN_ONLY_IF_CACHED);

	dict_index_t* index = table && !table->corrupted
		? dict_table_find_index_on_id(table, index_id)
		: NULL;

	if (!index || dict_index_is_corrupted(index)) {
		if (table) {
			dict_table_close(table, TRUE, FALSE);
		}
		mutex_exit(&dict_sys->mutex);
		return;
	}

	mutex_exit(&dict_sys->mutex);
	dict_stats_save_defrag_stats(index);
	dict_table_close(table, FALSE, FALSE);
}

/*****************************************************************//**
This is the thread for background stats gathering. It pops tables, from
the auto recalc list and proceeds them, eventually recalculating their
statistics.
@return this function does not return, it calls os_thread_exit() */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(dict_stats_thread)(void*)
{
	my_thread_init();
	ut_a(!srv_read_only_mode);

	while (!dict_stats_start_shutdown) {

		/* Wake up periodically even if not signaled. This is
		because we may lose an event - if the below call to
		dict_stats_process_entry_from_recalc_pool() puts the entry back
		in the list, the os_event_set() will be lost by the subsequent
		os_event_reset(). */
		os_event_wait_time(
			dict_stats_event, MIN_RECALC_INTERVAL * 1000000);

		if (dict_stats_start_shutdown) {
			break;
		}

		dict_stats_process_entry_from_recalc_pool();

		while (defrag_pool.size())
			dict_stats_process_entry_from_defrag_pool();

		os_event_reset(dict_stats_event);
	}

	srv_dict_stats_thread_active = false;

	os_event_set(dict_stats_shutdown_event);
	my_thread_end();
	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit instead of return(). */
	os_thread_exit(NULL);

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
