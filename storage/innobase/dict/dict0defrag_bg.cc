/*****************************************************************************

Copyright (c) 2016, 2022, MariaDB Corporation.

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
#include "trx0trx.h"
#include "lock0lock.h"
#include "row0mysql.h"

static mysql_mutex_t defrag_pool_mutex;

/** Iterator type for iterating over the elements of objects of type
defrag_pool_t. */
typedef defrag_pool_t::iterator		defrag_pool_iterator_t;

/** Pool where we store information on which tables are to be processed
by background defragmentation. */
defrag_pool_t			defrag_pool;


/*****************************************************************//**
Initialize the defrag pool, called once during thread initialization. */
void
dict_defrag_pool_init(void)
/*=======================*/
{
	ut_ad(!srv_read_only_mode);
	mysql_mutex_init(0, &defrag_pool_mutex, nullptr);
}

/*****************************************************************//**
Free the resources occupied by the defrag pool, called once during
thread de-initialization. */
void
dict_defrag_pool_deinit(void)
/*=========================*/
{
	ut_ad(!srv_read_only_mode);

	mysql_mutex_destroy(&defrag_pool_mutex);
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

	mysql_mutex_lock(&defrag_pool_mutex);

	if (defrag_pool.empty()) {
		mysql_mutex_unlock(&defrag_pool_mutex);
		return(false);
	}

	defrag_pool_item_t& item = defrag_pool.back();
	*table_id = item.table_id;
	*index_id = item.index_id;

	defrag_pool.pop_back();

	mysql_mutex_unlock(&defrag_pool_mutex);

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

	mysql_mutex_lock(&defrag_pool_mutex);

	/* quit if already in the list */
	for (defrag_pool_iterator_t iter = defrag_pool.begin();
	     iter != defrag_pool.end();
	     ++iter) {
		if ((*iter).table_id == index->table->id
		    && (*iter).index_id == index->id) {
			mysql_mutex_unlock(&defrag_pool_mutex);
			return;
		}
	}

	item.table_id = index->table->id;
	item.index_id = index->id;
	defrag_pool.push_back(item);
	if (defrag_pool.size() == 1) {
		/* Kick off dict stats optimizer work */
		dict_stats_schedule_now();
	}
	mysql_mutex_unlock(&defrag_pool_mutex);
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
	ut_ad(dict_sys.frozen());

	mysql_mutex_lock(&defrag_pool_mutex);

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

	mysql_mutex_unlock(&defrag_pool_mutex);
}

/*****************************************************************//**
Get the first index that has been added for updating persistent defrag
stats and eventually save its stats. */
static void dict_stats_process_entry_from_defrag_pool(THD *thd)
{
  table_id_t table_id;
  index_id_t index_id;

  ut_ad(!srv_read_only_mode);

  /* pop the first index from the auto defrag pool */
  if (!dict_stats_defrag_pool_get(&table_id, &index_id))
    /* no index in defrag pool */
    return;

  /* If the table is no longer cached, we've already lost the in
  memory stats so there's nothing really to write to disk. */
  MDL_ticket *mdl= nullptr;
  if (dict_table_t *table=
      dict_table_open_on_id(table_id, false, DICT_TABLE_OP_OPEN_ONLY_IF_CACHED,
                            thd, &mdl))
  {
    if (dict_index_t *index= !table->corrupted
        ? dict_table_find_index_on_id(table, index_id) : nullptr)
      if (!index->is_corrupted())
        dict_stats_save_defrag_stats(index);
    dict_table_close(table, false, thd, mdl);
  }
}

/**
Get the first index that has been added for updating persistent defrag
stats and eventually save its stats. */
void dict_defrag_process_entries_from_defrag_pool(THD *thd)
{
  while (!defrag_pool.empty())
    dict_stats_process_entry_from_defrag_pool(thd);
}

/*********************************************************************//**
Save defragmentation result.
@return DB_SUCCESS or error code */
dberr_t dict_stats_save_defrag_summary(dict_index_t *index, THD *thd)
{
  if (index->is_ibuf())
    return DB_SUCCESS;

  MDL_ticket *mdl_table= nullptr, *mdl_index= nullptr;
  dict_table_t *table_stats= dict_table_open_on_name(TABLE_STATS_NAME, false,
                                                     DICT_ERR_IGNORE_NONE);
  if (table_stats)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    table_stats= dict_acquire_mdl_shared<false>(table_stats, thd, &mdl_table);
    dict_sys.unfreeze();
  }
  if (!table_stats || strcmp(table_stats->name.m_name, TABLE_STATS_NAME))
  {
release_and_exit:
    if (table_stats)
      dict_table_close(table_stats, false, thd, mdl_table);
    return DB_STATS_DO_NOT_EXIST;
  }

  dict_table_t *index_stats= dict_table_open_on_name(INDEX_STATS_NAME, false,
                                                     DICT_ERR_IGNORE_NONE);
  if (index_stats)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    index_stats= dict_acquire_mdl_shared<false>(index_stats, thd, &mdl_index);
    dict_sys.unfreeze();
  }
  if (!index_stats)
    goto release_and_exit;
  if (strcmp(index_stats->name.m_name, INDEX_STATS_NAME))
  {
    dict_table_close(index_stats, false, thd, mdl_index);
    goto release_and_exit;
  }

  trx_t *trx= trx_create();
  trx->mysql_thd= thd;
  trx_start_internal(trx);
  dberr_t ret= trx->read_only
    ? DB_READ_ONLY
    : lock_table_for_trx(table_stats, trx, LOCK_X);
  if (ret == DB_SUCCESS)
    ret= lock_table_for_trx(index_stats, trx, LOCK_X);
  row_mysql_lock_data_dictionary(trx);
  if (ret == DB_SUCCESS)
    ret= dict_stats_save_index_stat(index, time(nullptr), "n_pages_freed",
                                    index->stat_defrag_n_pages_freed,
                                    nullptr,
                                    "Number of pages freed during"
                                    " last defragmentation run.",
                                    trx);
  if (ret == DB_SUCCESS)
    trx->commit();
  else
    trx->rollback();

  if (table_stats)
    dict_table_close(table_stats, true, thd, mdl_table);
  if (index_stats)
    dict_table_close(index_stats, true, thd, mdl_index);

  row_mysql_unlock_data_dictionary(trx);
  trx->free();

  return ret;
}

/**************************************************************//**
Gets the number of reserved and used pages in a B-tree.
@return	number of pages reserved, or ULINT_UNDEFINED if the index
is unavailable */
static
ulint
btr_get_size_and_reserved(
	dict_index_t*	index,	/*!< in: index */
	ulint		flag,	/*!< in: BTR_N_LEAF_PAGES or BTR_TOTAL_SIZE */
	ulint*		used,	/*!< out: number of pages used (<= reserved) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction where index
				is s-latched */
{
	ulint		dummy;

	ut_ad(mtr->memo_contains(index->lock, MTR_MEMO_S_LOCK));
	ut_a(flag == BTR_N_LEAF_PAGES || flag == BTR_TOTAL_SIZE);

	if (index->page == FIL_NULL
	    || dict_index_is_online_ddl(index)
	    || !index->is_committed()
	    || !index->table->space) {
		return(ULINT_UNDEFINED);
	}

	buf_block_t* root = btr_root_block_get(index, RW_SX_LATCH, mtr);
	*used = 0;
	if (!root) {
		return ULINT_UNDEFINED;
	}

	mtr->x_lock_space(index->table->space);

	ulint n = fseg_n_reserved_pages(*root, PAGE_HEADER + PAGE_BTR_SEG_LEAF
					+ root->page.frame, used, mtr);
	if (flag == BTR_TOTAL_SIZE) {
		n += fseg_n_reserved_pages(*root,
					   PAGE_HEADER + PAGE_BTR_SEG_TOP
					   + root->page.frame, &dummy, mtr);
		*used += dummy;
	}

	return(n);
}

/*********************************************************************//**
Save defragmentation stats for a given index.
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_defrag_stats(
/*============================*/
	dict_index_t*	index)	/*!< in: index */
{
  if (index->is_ibuf())
    return DB_SUCCESS;
  if (!index->is_readable())
    return dict_stats_report_error(index->table, true);

  const time_t now= time(nullptr);
  mtr_t mtr;
  ulint n_leaf_pages;
  mtr.start();
  mtr_s_lock_index(index, &mtr);
  ulint n_leaf_reserved= btr_get_size_and_reserved(index, BTR_N_LEAF_PAGES,
                                                   &n_leaf_pages, &mtr);
  mtr.commit();

  if (n_leaf_reserved == ULINT_UNDEFINED)
    return DB_SUCCESS;

  THD *thd= current_thd;
  MDL_ticket *mdl_table= nullptr, *mdl_index= nullptr;
  dict_table_t* table_stats= dict_table_open_on_name(TABLE_STATS_NAME, false,
                                                     DICT_ERR_IGNORE_NONE);
  if (table_stats)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    table_stats= dict_acquire_mdl_shared<false>(table_stats, thd, &mdl_table);
    dict_sys.unfreeze();
  }
  if (!table_stats || strcmp(table_stats->name.m_name, TABLE_STATS_NAME))
  {
release_and_exit:
    if (table_stats)
      dict_table_close(table_stats, false, thd, mdl_table);
    return DB_STATS_DO_NOT_EXIST;
  }

  dict_table_t *index_stats= dict_table_open_on_name(INDEX_STATS_NAME, false,
                                                     DICT_ERR_IGNORE_NONE);
  if (index_stats)
  {
    dict_sys.freeze(SRW_LOCK_CALL);
    index_stats= dict_acquire_mdl_shared<false>(index_stats, thd, &mdl_index);
    dict_sys.unfreeze();
  }
  if (!index_stats)
    goto release_and_exit;

  if (strcmp(index_stats->name.m_name, INDEX_STATS_NAME))
  {
    dict_table_close(index_stats, false, thd, mdl_index);
    goto release_and_exit;
  }

  trx_t *trx= trx_create();
  trx->mysql_thd= thd;
  trx_start_internal(trx);
  dberr_t ret= trx->read_only
    ? DB_READ_ONLY
    : lock_table_for_trx(table_stats, trx, LOCK_X);
  if (ret == DB_SUCCESS)
    ret= lock_table_for_trx(index_stats, trx, LOCK_X);

  row_mysql_lock_data_dictionary(trx);

  if (ret == DB_SUCCESS)
    ret= dict_stats_save_index_stat(index, now, "n_page_split",
                                    index->stat_defrag_n_page_split, nullptr,
                                    "Number of new page splits on leaves"
                                    " since last defragmentation.", trx);

  if (ret == DB_SUCCESS)
    ret= dict_stats_save_index_stat(index, now, "n_leaf_pages_defrag",
                                    n_leaf_pages, nullptr,
                                    "Number of leaf pages when"
                                    " this stat is saved to disk", trx);

  if (ret == DB_SUCCESS)
    ret= dict_stats_save_index_stat(index, now, "n_leaf_pages_reserved",
                                    n_leaf_reserved, nullptr,
                                    "Number of pages reserved for"
                                    " this index leaves"
                                    " when this stat is saved to disk", trx);

  if (ret == DB_SUCCESS)
    trx->commit();
  else
    trx->rollback();

  if (table_stats)
    dict_table_close(table_stats, true, thd, mdl_table);
  if (index_stats)
    dict_table_close(index_stats, true, thd, mdl_index);
  row_mysql_unlock_data_dictionary(trx);
  trx->free();

  return ret;
}
