/*****************************************************************************

Copyright (c) 2016, 2019, MariaDB Corporation.

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
@file dict/dict0defrag_bg.cc
Defragmentation routines.

Created 25/08/2016 Jan Lindström
*******************************************************/

#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "dict0defrag_bg.h"
#include "btr0btr.h"
#include "srv0start.h"

static ib_mutex_t		defrag_pool_mutex;

#ifdef MYSQL_PFS
static mysql_pfs_key_t		defrag_pool_mutex_key;
#endif

/** Iterator type for iterating over the elements of objects of type
defrag_pool_t. */
typedef defrag_pool_t::iterator		defrag_pool_iterator_t;

/** Pool where we store information on which tables are to be processed
by background defragmentation. */
defrag_pool_t			defrag_pool;

extern bool dict_stats_start_shutdown;

/*****************************************************************//**
Initialize the defrag pool, called once during thread initialization. */
void
dict_defrag_pool_init(void)
/*=======================*/
{
	ut_ad(!srv_read_only_mode);

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

	mutex_free(&defrag_pool_mutex);
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

	if (!index || index->is_corrupted()) {
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
Get the first index that has been added for updating persistent defrag
stats and eventually save its stats. */
void
dict_defrag_process_entries_from_defrag_pool()
/*==========================================*/
{
	while (defrag_pool.size() && !dict_stats_start_shutdown) {
		dict_stats_process_entry_from_defrag_pool();
	}
}

/*********************************************************************//**
Save defragmentation result.
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_defrag_summary(
/*============================*/
	dict_index_t*	index)	/*!< in: index */
{
	dberr_t	ret=DB_SUCCESS;

	if (dict_index_is_ibuf(index)) {
		return DB_SUCCESS;
	}

	rw_lock_x_lock(&dict_operation_lock);
	mutex_enter(&dict_sys->mutex);

	ret = dict_stats_save_index_stat(index, time(NULL), "n_pages_freed",
					 index->stat_defrag_n_pages_freed,
					 NULL,
					 "Number of pages freed during"
					 " last defragmentation run.",
					 NULL);

	mutex_exit(&dict_sys->mutex);
	rw_lock_x_unlock(&dict_operation_lock);

	return (ret);
}

/*********************************************************************//**
Save defragmentation stats for a given index.
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_defrag_stats(
/*============================*/
	dict_index_t*	index)	/*!< in: index */
{
	dberr_t	ret;

	if (dict_index_is_ibuf(index)) {
		return DB_SUCCESS;
	}

	if (!index->is_readable()) {
		return dict_stats_report_error(index->table, true);
	}

	const time_t now = time(NULL);
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

	rw_lock_x_lock(&dict_operation_lock);

	mutex_enter(&dict_sys->mutex);
	ret = dict_stats_save_index_stat(index, now, "n_page_split",
					 index->stat_defrag_n_page_split,
					 NULL,
					 "Number of new page splits on leaves"
					 " since last defragmentation.",
					 NULL);
	if (ret != DB_SUCCESS) {
		goto end;
	}

	ret = dict_stats_save_index_stat(
		index, now, "n_leaf_pages_defrag",
		n_leaf_pages,
		NULL,
		"Number of leaf pages when this stat is saved to disk",
		NULL);
	if (ret != DB_SUCCESS) {
		goto end;
	}

	ret = dict_stats_save_index_stat(
		index, now, "n_leaf_pages_reserved",
		n_leaf_reserved,
		NULL,
		"Number of pages reserved for this index leaves when this stat "
		"is saved to disk",
		NULL);

end:
	mutex_exit(&dict_sys->mutex);
	rw_lock_x_unlock(&dict_operation_lock);

	return (ret);
}
