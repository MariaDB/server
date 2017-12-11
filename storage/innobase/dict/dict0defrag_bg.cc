/*****************************************************************************

Copyright (c) 2016, 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file dict/dict0defrag_bg.cc
Defragmentation routines.

Created 25/08/2016 Jan Lindström
*******************************************************/

#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "dict0defrag_bg.h"
#include "row0mysql.h"
#include "srv0start.h"
#include "trx0roll.h"
#include "ut0new.h"

#include <vector>

static ib_mutex_t		defrag_pool_mutex;

#ifdef MYSQL_PFS
static mysql_pfs_key_t		defrag_pool_mutex_key;
#endif

/** Indices whose defrag stats need to be saved to persistent storage.*/
struct defrag_pool_item_t {
	table_id_t	table_id;
	index_id_t	index_id;
};

/** Allocator type, used by std::vector */
typedef ut_allocator<defrag_pool_item_t>
	defrag_pool_allocator_t;

/** The multitude of tables to be defragmented- an STL vector */
typedef std::vector<defrag_pool_item_t, defrag_pool_allocator_t>
	defrag_pool_t;

/** Iterator type for iterating over the elements of objects of type
defrag_pool_t. */
typedef defrag_pool_t::iterator		defrag_pool_iterator_t;

/** Pool where we store information on which tables are to be processed
by background defragmentation. */
static defrag_pool_t*			defrag_pool;

extern bool dict_stats_start_shutdown;

/*****************************************************************//**
Initialize the defrag pool, called once during thread initialization. */
void
dict_defrag_pool_init(void)
/*=======================*/
{
	ut_ad(!srv_read_only_mode);
	/* JAN: TODO: MySQL 5.7 PSI
	const PSI_memory_key	key2 = mem_key_dict_defrag_pool_t;

	defrag_pool = UT_NEW(defrag_pool_t(defrag_pool_allocator_t(key2)), key2);

	recalc_pool->reserve(RECALC_POOL_INITIAL_SLOTS);
	*/
	defrag_pool = new std::vector<defrag_pool_item_t, defrag_pool_allocator_t>();

	/* We choose SYNC_STATS_DEFRAG to be below SYNC_FSP_PAGE. */
	mutex_create(LATCH_ID_DEFRAGMENT_MUTEX, &defrag_pool_mutex);
}

/*****************************************************************//**
Free the resources occupied by the defrag pool, called once during
thread de-initialization. */
void
dict_defrag_pool_deinit(void)
/*=========================*/
{
	ut_ad(!srv_read_only_mode);

	defrag_pool->clear();
	mutex_free(&defrag_pool_mutex);

	UT_DELETE(defrag_pool);
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

	if (defrag_pool->empty()) {
		mutex_exit(&defrag_pool_mutex);
		return(false);
	}

	defrag_pool_item_t& item = defrag_pool->back();
	*table_id = item.table_id;
	*index_id = item.index_id;

	defrag_pool->pop_back();

	mutex_exit(&defrag_pool_mutex);

	return(true);
}

/*****************************************************************//**
Add an index in a table to the defrag pool, which is processed by the
background stats gathering thread. Only the table id and index id are
added to the list, so the table can be closed after being enqueued and
it will be opened when needed. If the table or index does not exist later
(has been DROPped), then it will be removed from the pool and skipped. */
void
dict_stats_defrag_pool_add(
/*=======================*/
	const dict_index_t*	index)	/*!< in: table to add */
{
	defrag_pool_item_t item;

	ut_ad(!srv_read_only_mode);

	mutex_enter(&defrag_pool_mutex);

	/* quit if already in the list */
	for (defrag_pool_iterator_t iter = defrag_pool->begin();
	     iter != defrag_pool->end();
	     ++iter) {
		if ((*iter).table_id == index->table->id
		    && (*iter).index_id == index->id) {
			mutex_exit(&defrag_pool_mutex);
			return;
		}
	}

	item.table_id = index->table->id;
	item.index_id = index->id;
	defrag_pool->push_back(item);

	mutex_exit(&defrag_pool_mutex);

	os_event_set(dict_stats_event);
}

/*****************************************************************//**
Delete a given index from the auto defrag pool. */
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

	defrag_pool_iterator_t iter = defrag_pool->begin();
	while (iter != defrag_pool->end()) {
		if ((table && (*iter).table_id == table->id)
		    || (index
			&& (*iter).table_id == index->table->id
			&& (*iter).index_id == index->id)) {
			/* erase() invalidates the iterator */
			iter = defrag_pool->erase(iter);
			if (index)
				break;
		} else {
			iter++;
		}
	}

	mutex_exit(&defrag_pool_mutex);
}

/** Get the first index that has been added for updating persistent defrag
stats and eventually save its stats.
@param[in,out]	trx	transaction that will be started and committed  */
static
void
dict_stats_process_entry_from_defrag_pool(trx_t* trx)
{
	table_id_t	table_id;
	index_id_t	index_id;

	ut_ad(!srv_read_only_mode);
	ut_ad(trx->persistent_stats);

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
	trx->error_state = DB_SUCCESS;
	++trx->will_lock;
	dberr_t err = dict_stats_save_defrag_stats(index, trx);

	if (err != DB_SUCCESS) {
		trx_rollback_to_savepoint(trx, NULL);
		ib::error() << "Saving defragmentation status for table "
			    << index->table->name
			    << " index " << index->name
			    << " failed " << err;
	} else if (trx->state != TRX_STATE_NOT_STARTED) {
		trx_commit_for_mysql(trx);
	}

	dict_table_close(table, FALSE, FALSE);
}

/** Process indexes that have been scheduled for defragmenting.
@param[in,out]	trx	transaction that will be started and committed  */
void
dict_defrag_process_entries_from_defrag_pool(trx_t* trx)
{
	while (defrag_pool->size() && !dict_stats_start_shutdown) {
		dict_stats_process_entry_from_defrag_pool(trx);
	}
}

/** Save defragmentation result.
@param[in]	index	index that was defragmented
@param[in,out]	trx	transaction
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_defrag_summary(dict_index_t* index, trx_t* trx)
{
	ut_ad(trx->persistent_stats);

	if (dict_index_is_ibuf(index)) {
		return DB_SUCCESS;
	}

	return dict_stats_save_index_stat(index, ut_time(), "n_pages_freed",
					  index->stat_defrag_n_pages_freed,
					  NULL,
					  "Number of pages freed during"
					  " last defragmentation run.",
					  trx);
}

/** Save defragmentation stats for a given index.
@param[in]	index	index that is being defragmented
@param[in,out]	trx	transaction
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_defrag_stats(dict_index_t* index, trx_t* trx)
{
	ut_ad(trx->error_state == DB_SUCCESS);
	ut_ad(trx->persistent_stats);

	if (dict_index_is_ibuf(index)) {
		return DB_SUCCESS;
	}

	if (!index->is_readable()) {
		return dict_stats_report_error(index->table, true);
	}

	mtr_t	mtr;
	ulint	n_leaf_pages;
	ulint	n_leaf_reserved;
	mtr_start(&mtr);
	mtr_s_lock(dict_index_get_lock(index), &mtr);
	n_leaf_reserved = btr_get_size_and_reserved(index, BTR_N_LEAF_PAGES,
						    &n_leaf_pages, &mtr);
	mtr_commit(&mtr);

	if (n_leaf_reserved == ULINT_UNDEFINED) {
		// The index name is different during fast index creation,
		// so the stats won't be associated with the right index
		// for later use. We just return without saving.
		return DB_SUCCESS;
	}

	ib_time_t now = ut_time();
	dberr_t err = dict_stats_save_index_stat(
		index, now, "n_page_split",
		index->stat_defrag_n_page_split,
		NULL,
		"Number of new page splits on leaves"
		" since last defragmentation.",
		trx);
	if (err == DB_SUCCESS) {
		err = dict_stats_save_index_stat(
			index, now, "n_leaf_pages_defrag",
			n_leaf_pages,
			NULL,
			"Number of leaf pages when this stat is saved to disk",
			trx);
	}

	if (err == DB_SUCCESS) {
		err = dict_stats_save_index_stat(
			index, now, "n_leaf_pages_reserved",
			n_leaf_reserved,
			NULL,
			"Number of pages reserved for this "
			"index leaves when this stat "
			"is saved to disk",
			trx);
	}

	return err;
}
