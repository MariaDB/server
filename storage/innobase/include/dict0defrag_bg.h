/*****************************************************************************

Copyright (c) 2016, 2021, MariaDB Corporation.

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
@file include/dict0defrag_bg.h
Code used for background table and index
defragmentation

Created 25/08/2016 Jan Lindström
*******************************************************/

#ifndef dict0defrag_bg_h
#define dict0defrag_bg_h

#include "dict0types.h"

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

/** Pool where we store information on which tables are to be processed
by background defragmentation. */
extern defrag_pool_t		defrag_pool;

/*****************************************************************//**
Initialize the defrag pool, called once during thread initialization. */
void
dict_defrag_pool_init(void);
/*========================*/

/*****************************************************************//**
Free the resources occupied by the defrag pool, called once during
thread de-initialization. */
void
dict_defrag_pool_deinit(void);
/*==========================*/

/*****************************************************************//**
Add an index in a table to the defrag pool, which is processed by the
background stats gathering thread. Only the table id and index id are
added to the list, so the table can be closed after being enqueued and
it will be opened when needed. If the table or index does not exist later
(has been DROPped), then it will be removed from the pool and skipped. */
void
dict_stats_defrag_pool_add(
/*=======================*/
	const dict_index_t*	index);	/*!< in: table to add */

/*****************************************************************//**
Delete a given index from the auto defrag pool. */
void
dict_stats_defrag_pool_del(
/*=======================*/
	const dict_table_t*	table,	/*!<in: if given, remove
					all entries for the table */
	const dict_index_t*	index);	/*!< in: index to remove */

/**
Get the first index that has been added for updating persistent defrag
stats and eventually save its stats. */
void dict_defrag_process_entries_from_defrag_pool(THD *thd);

/*********************************************************************//**
Save defragmentation result.
@return DB_SUCCESS or error code */
dberr_t dict_stats_save_defrag_summary(dict_index_t *index, THD *thd)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/*********************************************************************//**
Save defragmentation stats for a given index.
@return DB_SUCCESS or error code */
dberr_t
dict_stats_save_defrag_stats(
/*============================*/
	dict_index_t*	index);	/*!< in: index */
#endif /* dict0defrag_bg_h */
