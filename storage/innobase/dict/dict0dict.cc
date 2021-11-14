/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2021, MariaDB Corporation.

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

/******************************************************************//**
@file dict/dict0dict.cc
Data dictionary system

Created 1/8/1996 Heikki Tuuri
***********************************************************************/

#include <my_config.h>
#include <string>

#include "ha_prototypes.h"
#include <mysqld.h>
#include <strfunc.h>

#include "dict0dict.h"
#include "fts0fts.h"
#include "fil0fil.h"
#include <algorithm>

/** dummy index for ROW_FORMAT=REDUNDANT supremum and infimum records */
dict_index_t*	dict_ind_redundant;

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/** Flag to control insert buffer debugging. */
extern uint	ibuf_debug;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "buf0buf.h"
#include "data0type.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "dict0mem.h"
#include "dict0priv.h"
#include "dict0stats.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "lock0lock.h"
#include "mach0data.h"
#include "mem0mem.h"
#include "page0page.h"
#include "page0zip.h"
#include "pars0pars.h"
#include "pars0sym.h"
#include "que0que.h"
#include "rem0cmp.h"
#include "row0log.h"
#include "row0merge.h"
#include "row0mysql.h"
#include "row0upd.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "sync0sync.h"
#include "trx0undo.h"

#include <vector>
#include <algorithm>

/** the dictionary system */
dict_sys_t*	dict_sys	= NULL;

/** @brief the data dictionary rw-latch protecting dict_sys

table create, drop, etc. reserve this in X-mode; implicit or
backround operations purge, rollback, foreign key checks reserve this
in S-mode; we cannot trust that MySQL protects implicit or background
operations a table drop since MySQL does not know of them; therefore
we need this; NOTE: a transaction which reserves this must keep book
on the mode in trx_t::dict_operation_lock_mode */
rw_lock_t	dict_operation_lock;

/** Percentage of compression failures that are allowed in a single
round */
ulong	zip_failure_threshold_pct = 5;

/** Maximum percentage of a page that can be allowed as a pad to avoid
compression failures */
ulong	zip_pad_max = 50;

#define	DICT_HEAP_SIZE		100	/*!< initial memory heap size when
					creating a table or index object */
#define DICT_POOL_PER_TABLE_HASH 512	/*!< buffer pool max size per table
					hash table fixed size in bytes */
#define DICT_POOL_PER_VARYING	4	/*!< buffer pool max size per data
					dictionary varying size in bytes */

/** Identifies generated InnoDB foreign key names */
static char	dict_ibfk[] = "_ibfk_";

bool		innodb_table_stats_not_found = false;
bool		innodb_index_stats_not_found = false;
static bool	innodb_table_stats_not_found_reported = false;
static bool	innodb_index_stats_not_found_reported = false;

/*******************************************************************//**
Tries to find column names for the index and sets the col field of the
index.
@param[in]	table	table
@param[in]	index	index
@param[in]	add_v	new virtual columns added along with an add index call
@return TRUE if the column names were found */
static
ibool
dict_index_find_cols(
	const dict_table_t*	table,
	dict_index_t*		index,
	const dict_add_v_col_t*	add_v);
/*******************************************************************//**
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the clustered index */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index);	/*!< in: user representation of
					a clustered index */
/*******************************************************************//**
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the non-clustered index */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index);	/*!< in: user representation of
					a non-clustered index */
/**********************************************************************//**
Builds the internal dictionary cache representation for an FTS index.
@return own: the internal representation of the FTS index */
static
dict_index_t*
dict_index_build_internal_fts(
/*==========================*/
	dict_table_t*	table,	/*!< in: table */
	dict_index_t*	index);	/*!< in: user representation of an FTS index */

/**********************************************************************//**
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in/out: table */
	dict_index_t*	index,		/*!< in, own: index */
	ibool		lru_evict);	/*!< in: TRUE if page being evicted
					to make room in the table LRU list */
#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate the dictionary table LRU list.
@return TRUE if validate OK */
static
ibool
dict_lru_validate(void);
/*===================*/
/**********************************************************************//**
Check if table is in the dictionary table LRU list.
@return TRUE if table found */
static
ibool
dict_lru_find_table(
/*================*/
	const dict_table_t*	find_table);	/*!< in: table to find */
/**********************************************************************//**
Check if a table exists in the dict table non-LRU list.
@return TRUE if table found */
static
ibool
dict_non_lru_find_table(
/*====================*/
	const dict_table_t*	find_table);	/*!< in: table to find */
#endif /* UNIV_DEBUG */

/* Stream for storing detailed information about the latest foreign key
and unique key errors. Only created if !srv_read_only_mode */
FILE*	dict_foreign_err_file		= NULL;
/* mutex protecting the foreign and unique error buffers */
ib_mutex_t	dict_foreign_err_mutex;

/********************************************************************//**
Checks if the database name in two table names is the same.
@return TRUE if same db name */
ibool
dict_tables_have_same_db(
/*=====================*/
	const char*	name1,	/*!< in: table name in the form
				dbname '/' tablename */
	const char*	name2)	/*!< in: table name in the form
				dbname '/' tablename */
{
	for (; *name1 == *name2; name1++, name2++) {
		if (*name1 == '/') {
			return(TRUE);
		}
		ut_a(*name1); /* the names must contain '/' */
	}
	return(FALSE);
}

/********************************************************************//**
Return the end of table name where we have removed dbname and '/'.
@return table name */
const char*
dict_remove_db_name(
/*================*/
	const char*	name)	/*!< in: table name in the form
				dbname '/' tablename */
{
	const char*	s = strchr(name, '/');
	ut_a(s);

	return(s + 1);
}

/********************************************************************//**
Get the database name length in a table name.
@return database name length */
ulint
dict_get_db_name_len(
/*=================*/
	const char*	name)	/*!< in: table name in the form
				dbname '/' tablename */
{
	const char*	s;
	s = strchr(name, '/');
	ut_a(s);
	return(s - name);
}

/** Reserve the dictionary system mutex. */
void
dict_mutex_enter_for_mysql_func(const char *file, unsigned line)
{
	mutex_enter_loc(&dict_sys->mutex, file, line);
}

/********************************************************************//**
Releases the dictionary system mutex for MySQL. */
void
dict_mutex_exit_for_mysql(void)
/*===========================*/
{
	mutex_exit(&dict_sys->mutex);
}

/**********************************************************************//**
Try to drop any indexes after an aborted index creation.
This can also be after a server kill during DROP INDEX. */
static
void
dict_table_try_drop_aborted(
/*========================*/
	dict_table_t*	table,		/*!< in: table, or NULL if it
					needs to be looked up again */
	table_id_t	table_id,	/*!< in: table identifier */
	int32		ref_count)	/*!< in: expected table->n_ref_count */
{
	trx_t*		trx;

	trx = trx_allocate_for_background();
	trx->op_info = "try to drop any indexes after an aborted index creation";
	row_mysql_lock_data_dictionary(trx);
	trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);

	if (table == NULL) {
		table = dict_table_open_on_id_low(
			table_id, DICT_ERR_IGNORE_FK_NOKEY, FALSE);
	} else {
		ut_ad(table->id == table_id);
	}

	if (table && table->get_ref_count() == ref_count && table->drop_aborted
	    && !UT_LIST_GET_FIRST(table->locks)) {
		/* Silence a debug assertion in row_merge_drop_indexes(). */
		ut_d(table->acquire());
		row_merge_drop_indexes(trx, table, true);
		ut_d(table->release());
		ut_ad(table->get_ref_count() == ref_count);
		trx_commit_for_mysql(trx);
	}

	row_mysql_unlock_data_dictionary(trx);
	trx_free_for_background(trx);
}

/**********************************************************************//**
When opening a table,
try to drop any indexes after an aborted index creation.
Release the dict_sys->mutex. */
static
void
dict_table_try_drop_aborted_and_mutex_exit(
/*=======================================*/
	dict_table_t*	table,		/*!< in: table (may be NULL) */
	ibool		try_drop)	/*!< in: FALSE if should try to
					drop indexes whose online creation
					was aborted */
{
	if (try_drop
	    && table != NULL
	    && table->drop_aborted
	    && table->get_ref_count() == 1
	    && dict_table_get_first_index(table)) {

		/* Attempt to drop the indexes whose online creation
		was aborted. */
		table_id_t	table_id = table->id;

		mutex_exit(&dict_sys->mutex);

		dict_table_try_drop_aborted(table, table_id, 1);
	} else {
		mutex_exit(&dict_sys->mutex);
	}
}

/********************************************************************//**
Decrements the count of open handles to a table. */
void
dict_table_close(
/*=============*/
	dict_table_t*	table,		/*!< in/out: table */
	ibool		dict_locked,	/*!< in: TRUE=data dictionary locked */
	ibool		try_drop)	/*!< in: TRUE=try to drop any orphan
					indexes after an aborted online
					index creation */
{
	if (!dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_a(table->get_ref_count() > 0);

	const bool last_handle = table->release();

	/* Force persistent stats re-read upon next open of the table
	so that FLUSH TABLE can be used to forcibly fetch stats from disk
	if they have been manually modified. We reset table->stat_initialized
	only if table reference count is 0 because we do not want too frequent
	stats re-reads (e.g. in other cases than FLUSH TABLE). */
	if (last_handle && strchr(table->name.m_name, '/') != NULL
	    && dict_stats_is_persistent_enabled(table)) {

		dict_stats_deinit(table);
	}

	MONITOR_DEC(MONITOR_TABLE_REFERENCE);

	ut_ad(dict_lru_validate());

#ifdef UNIV_DEBUG
	if (table->can_be_evicted) {
		ut_ad(dict_lru_find_table(table));
	} else {
		ut_ad(dict_non_lru_find_table(table));
	}
#endif /* UNIV_DEBUG */

	if (!dict_locked) {
		table_id_t	table_id	= table->id;
		const bool	drop_aborted	= last_handle && try_drop
			&& table->drop_aborted
			&& dict_table_get_first_index(table);

		mutex_exit(&dict_sys->mutex);

		/* dict_table_try_drop_aborted() can generate undo logs.
		So it should be avoided after shutdown of background
		threads */
		if (drop_aborted && !srv_undo_sources) {
			dict_table_try_drop_aborted(NULL, table_id, 0);
		}
	}
}

/********************************************************************//**
Closes the only open handle to a table and drops a table while assuring
that dict_sys->mutex is held the whole time.  This assures that the table
is not evicted after the close when the count of open handles goes to zero.
Because dict_sys->mutex is held, we do not need to call
dict_table_prevent_eviction().  */
void
dict_table_close_and_drop(
/*======================*/
	trx_t*		trx,		/*!< in: data dictionary transaction */
	dict_table_t*	table)		/*!< in/out: table */
{
	dberr_t err = DB_SUCCESS;

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(rw_lock_own(&dict_operation_lock, RW_LOCK_X));
	ut_ad(trx->dict_operation != TRX_DICT_OP_NONE);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));

	dict_table_close(table, TRUE, FALSE);

#if defined UNIV_DEBUG || defined UNIV_DDL_DEBUG
	/* Nobody should have initialized the stats of the newly created
	table when this is called. So we know that it has not been added
	for background stats gathering. */
	ut_a(!table->stat_initialized);
#endif /* UNIV_DEBUG || UNIV_DDL_DEBUG */

	err = row_merge_drop_table(trx, table);

	if (err != DB_SUCCESS) {
		ib::error() << "At " << __FILE__ << ":" << __LINE__
			    << " row_merge_drop_table returned error: " << err
			    << " table: " << table->name;
	}
}

/** Check if the table has a given (non_virtual) column.
@param[in]	table		table object
@param[in]	col_name	column name
@param[in]	col_nr		column number guessed, 0 as default
@return column number if the table has the specified column,
otherwise table->n_def */
ulint
dict_table_has_column(
	const dict_table_t*	table,
	const char*		col_name,
	ulint			col_nr)
{
	ulint		col_max = table->n_def;

	ut_ad(table);
	ut_ad(col_name);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	if (col_nr < col_max
	    && innobase_strcasecmp(
		col_name, dict_table_get_col_name(table, col_nr)) == 0) {
		return(col_nr);
	}

	/** The order of column may changed, check it with other columns */
	for (ulint i = 0; i < col_max; i++) {
		if (i != col_nr
		    && innobase_strcasecmp(
			col_name, dict_table_get_col_name(table, i)) == 0) {

			return(i);
		}
	}

	return(col_max);
}

/**********************************************************************//**
Returns a column's name.
@return column name. NOTE: not guaranteed to stay valid if table is
modified in any way (columns added, etc.). */
const char*
dict_table_get_col_name(
/*====================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			col_nr)	/*!< in: column number */
{
	ulint		i;
	const char*	s;

	ut_ad(table);
	ut_ad(col_nr < table->n_def);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	s = table->col_names;
	if (s) {
		for (i = 0; i < col_nr; i++) {
			s += strlen(s) + 1;
		}
	}

	return(s);
}

/** Returns a virtual column's name.
@param[in]	table	target table
@param[in]	col_nr	virtual column number (nth virtual column)
@return column name or NULL if column number out of range. */
const char*
dict_table_get_v_col_name(
	const dict_table_t*	table,
	ulint			col_nr)
{
	const char*	s;

	ut_ad(table);
	ut_ad(col_nr < table->n_v_def);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	if (col_nr >= table->n_v_def) {
		return(NULL);
	}

	s = table->v_col_names;

	if (s != NULL) {
		for (ulint i = 0; i < col_nr; i++) {
			s += strlen(s) + 1;
		}
	}

	return(s);
}

/** Search virtual column's position in InnoDB according to its position
in original table's position
@param[in]	table	target table
@param[in]	col_nr	column number (nth column in the MySQL table)
@return virtual column's position in InnoDB, ULINT_UNDEFINED if not find */
static
ulint
dict_table_get_v_col_pos_for_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i;

	ut_ad(table);
	ut_ad(col_nr < static_cast<ulint>(table->n_t_def));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	for (i = 0; i < table->n_v_def; i++) {
		if (col_nr == dict_get_v_col_mysql_pos(
				table->v_cols[i].m_col.ind)) {
			break;
		}
	}

	if (i == table->n_v_def) {
		return(ULINT_UNDEFINED);
	}

	return(i);
}

/** Returns a virtual column's name according to its original
MySQL table position.
@param[in]	table	target table
@param[in]	col_nr	column number (nth column in the table)
@return column name. */
static
const char*
dict_table_get_v_col_name_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i = dict_table_get_v_col_pos_for_mysql(table, col_nr);

	if (i == ULINT_UNDEFINED) {
		return(NULL);
	}

	return(dict_table_get_v_col_name(table, i));
}

/** Get nth virtual column according to its original MySQL table position
@param[in]	table	target table
@param[in]	col_nr	column number in MySQL Table definition
@return dict_v_col_t ptr */
dict_v_col_t*
dict_table_get_nth_v_col_mysql(
	const dict_table_t*	table,
	ulint			col_nr)
{
	ulint	i = dict_table_get_v_col_pos_for_mysql(table, col_nr);

	if (i == ULINT_UNDEFINED) {
		return(NULL);
	}

	return(dict_table_get_nth_v_col(table, i));
}

/********************************************************************//**
Acquire the autoinc lock. */
void
dict_table_autoinc_lock(
/*====================*/
	dict_table_t*	table)	/*!< in/out: table */
{
	mysql_mutex_lock(&table->autoinc_mutex);
}

/** Acquire the zip_pad_mutex latch.
@param[in,out]	index	the index whose zip_pad_mutex to acquire.*/
static
void
dict_index_zip_pad_lock(
	dict_index_t*	index)
{
	mysql_mutex_lock(&index->zip_pad.mutex);
}

/** Get all the FTS indexes on a table.
@param[in]	table	table
@param[out]	indexes	all FTS indexes on this table
@return number of FTS indexes */
ulint
dict_table_get_all_fts_indexes(
	const dict_table_t*	table,
	ib_vector_t*		indexes)
{
	dict_index_t* index;

	ut_a(ib_vector_size(indexes) == 0);

	for (index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {

		if (index->type == DICT_FTS) {
			ib_vector_push(indexes, &index);
		}
	}

	return(ib_vector_size(indexes));
}

/********************************************************************//**
Release the autoinc lock. */
void
dict_table_autoinc_unlock(
/*======================*/
	dict_table_t*	table)	/*!< in/out: table */
{
	mysql_mutex_unlock(&table->autoinc_mutex);
}

/** Looks for column n in an index.
@param[in]	index		index
@param[in]	n		column number
@param[in]	inc_prefix	true=consider column prefixes too
@param[in]	is_virtual	true==virtual column
@param[out]	prefix_col_pos	col num if prefix
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_col_or_prefix_pos(
	const dict_index_t*	index,
	ulint			n,
	bool			inc_prefix,
	bool			is_virtual,
	ulint*			prefix_col_pos)
{
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	if (prefix_col_pos) {
		*prefix_col_pos = ULINT_UNDEFINED;
	}

	if (is_virtual) {
		col = &(dict_table_get_nth_v_col(index->table, n)->m_col);
	} else {
		col = dict_table_get_nth_col(index->table, n);
	}

	if (dict_index_is_clust(index)) {

		return(dict_col_get_clust_pos(col, index));
	}

	n_fields = dict_index_get_n_fields(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {
			if (prefix_col_pos) {
				*prefix_col_pos = pos;
			}
			if (inc_prefix || field->prefix_len == 0) {
				return(pos);
			}
		}
	}

	return(ULINT_UNDEFINED);
}

/** Returns TRUE if the index contains a column or a prefix of that column.
@param[in]	index		index
@param[in]	n		column number
@param[in]	is_virtual	whether it is a virtual col
@return TRUE if contains the column or its prefix */
ibool
dict_index_contains_col_or_prefix(
	const dict_index_t*	index,
	ulint			n,
	bool			is_virtual)
{
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	if (dict_index_is_clust(index)) {
		return(!is_virtual);
	}

	if (is_virtual) {
		col = &dict_table_get_nth_v_col(index->table, n)->m_col;
	} else {
		col = dict_table_get_nth_col(index->table, n);
	}

	n_fields = dict_index_get_n_fields(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/********************************************************************//**
Looks for a matching field in an index. The column has to be the same. The
column in index must be complete, or must contain a prefix longer than the
column in index2. That is, we must be able to construct the prefix in index2
from the prefix in index.
@return position in internal representation of the index;
ULINT_UNDEFINED if not contained */
ulint
dict_index_get_nth_field_pos(
/*=========================*/
	const dict_index_t*	index,	/*!< in: index from which to search */
	const dict_index_t*	index2,	/*!< in: index */
	ulint			n)	/*!< in: field number in index2 */
{
	const dict_field_t*	field;
	const dict_field_t*	field2;
	ulint			n_fields;
	ulint			pos;

	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	field2 = dict_index_get_nth_field(index2, n);

	n_fields = dict_index_get_n_fields(index);

	/* Are we looking for a MBR (Minimum Bound Box) field of
	a spatial index */
	bool	is_mbr_fld = (n == 0 && dict_index_is_spatial(index2));

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		/* The first field of a spatial index is a transformed
		MBR (Minimum Bound Box) field made out of original column,
		so its field->col still points to original cluster index
		col, but the actual content is different. So we cannot
		consider them equal if neither of them is MBR field */
		if (pos == 0 && dict_index_is_spatial(index) && !is_mbr_fld) {
			continue;
		}

		if (field->col == field2->col
		    && (field->prefix_len == 0
			|| (field->prefix_len >= field2->prefix_len
			    && field2->prefix_len != 0))) {

			return(pos);
		}
	}

	return(ULINT_UNDEFINED);
}

/**********************************************************************//**
Returns a table object based on table id.
@return table, NULL if does not exist */
dict_table_t*
dict_table_open_on_id(
/*==================*/
	table_id_t	table_id,	/*!< in: table id */
	ibool		dict_locked,	/*!< in: TRUE=data dictionary locked */
	dict_table_op_t	table_op)	/*!< in: operation to perform */
{
	dict_table_t*	table;

	if (!dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

	ut_ad(mutex_own(&dict_sys->mutex));

	table = dict_table_open_on_id_low(
		table_id,
		table_op == DICT_TABLE_OP_LOAD_TABLESPACE
		? DICT_ERR_IGNORE_RECOVER_LOCK
		: DICT_ERR_IGNORE_FK_NOKEY,
		table_op == DICT_TABLE_OP_OPEN_ONLY_IF_CACHED);

	if (table != NULL) {

		if (table->can_be_evicted) {
			dict_move_to_mru(table);
		}

		table->acquire();

		MONITOR_INC(MONITOR_TABLE_REFERENCE);
	}

	if (!dict_locked) {
		dict_table_try_drop_aborted_and_mutex_exit(
			table, table_op == DICT_TABLE_OP_DROP_ORPHAN);
	}

	return(table);
}

/********************************************************************//**
Looks for column n position in the clustered index.
@return position in internal representation of the clustered index */
ulint
dict_table_get_nth_col_pos(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n,	/*!< in: column number */
	ulint*			prefix_col_pos)
{
	return(dict_index_get_nth_col_pos(dict_table_get_first_index(table),
					  n, prefix_col_pos));
}

/********************************************************************//**
Checks if a column is in the ordering columns of the clustered index of a
table. Column prefixes are treated like whole columns.
@return TRUE if the column, or its prefix, is in the clustered key */
ibool
dict_table_col_in_clustered_key(
/*============================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			n)	/*!< in: column number */
{
	const dict_index_t*	index;
	const dict_field_t*	field;
	const dict_col_t*	col;
	ulint			pos;
	ulint			n_fields;

	col = dict_table_get_nth_col(table, n);

	index = dict_table_get_first_index(table);

	n_fields = dict_index_get_n_unique(index);

	for (pos = 0; pos < n_fields; pos++) {
		field = dict_index_get_nth_field(index, pos);

		if (col == field->col) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/**********************************************************************//**
Inits the data dictionary module. */
void
dict_init(void)
/*===========*/
{
	dict_sys = static_cast<dict_sys_t*>(ut_zalloc_nokey(sizeof(*dict_sys)));

	UT_LIST_INIT(dict_sys->table_LRU, &dict_table_t::table_LRU);
	UT_LIST_INIT(dict_sys->table_non_LRU, &dict_table_t::table_LRU);

	mutex_create(LATCH_ID_DICT_SYS, &dict_sys->mutex);

	dict_sys->table_hash = hash_create(
		buf_pool_get_curr_size()
		/ (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE));

	dict_sys->table_id_hash = hash_create(
		buf_pool_get_curr_size()
		/ (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE));

	rw_lock_create(dict_operation_lock_key,
		       &dict_operation_lock, SYNC_DICT_OPERATION);

	if (!srv_read_only_mode) {
		dict_foreign_err_file = os_file_create_tmpfile(NULL);
		ut_a(dict_foreign_err_file);
	}

	mutex_create(LATCH_ID_DICT_FOREIGN_ERR, &dict_foreign_err_mutex);
}

/**********************************************************************//**
Move to the most recently used segment of the LRU list. */
void
dict_move_to_mru(
/*=============*/
	dict_table_t*	table)		/*!< in: table to move to MRU */
{
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(dict_lru_validate());
	ut_ad(dict_lru_find_table(table));

	ut_a(table->can_be_evicted);

	UT_LIST_REMOVE(dict_sys->table_LRU, table);

	UT_LIST_ADD_FIRST(dict_sys->table_LRU, table);

	ut_ad(dict_lru_validate());
}

/**********************************************************************//**
Returns a table object and increment its open handle count.
NOTE! This is a high-level function to be used mainly from outside the
'dict' module. Inside this directory dict_table_get_low
is usually the appropriate function.
@return table, NULL if does not exist */
dict_table_t*
dict_table_open_on_name(
/*====================*/
	const char*	table_name,	/*!< in: table name */
	ibool		dict_locked,	/*!< in: TRUE=data dictionary locked */
	ibool		try_drop,	/*!< in: TRUE=try to drop any orphan
					indexes after an aborted online
					index creation */
	dict_err_ignore_t
			ignore_err)	/*!< in: error to be ignored when
					loading a table definition */
{
	dict_table_t*	table;
	DBUG_ENTER("dict_table_open_on_name");
	DBUG_PRINT("dict_table_open_on_name", ("table: '%s'", table_name));

	if (!dict_locked) {
		mutex_enter(&dict_sys->mutex);
	}

	ut_ad(table_name);
	ut_ad(mutex_own(&dict_sys->mutex));

	table = dict_table_check_if_in_cache_low(table_name);

	if (table == NULL) {
		table = dict_load_table(table_name, ignore_err);
	}

	ut_ad(!table || table->cached);

	if (table != NULL) {

		/* If table is encrypted or corrupted */
		if (!(ignore_err & ~DICT_ERR_IGNORE_FK_NOKEY)
		    && !table->is_readable()) {
			/* Make life easy for drop table. */
			dict_table_prevent_eviction(table);

			if (table->corrupted) {

				ib::error() << "Table " << table->name
					<< " is corrupted. Please "
					"drop the table and recreate.";
				if (!dict_locked) {
					mutex_exit(&dict_sys->mutex);
				}

				DBUG_RETURN(NULL);
			}

			if (table->can_be_evicted) {
				dict_move_to_mru(table);
			}

			table->acquire();

			if (!dict_locked) {
				mutex_exit(&dict_sys->mutex);
			}

			DBUG_RETURN(table);
		}

		if (table->can_be_evicted) {
			dict_move_to_mru(table);
		}

		table->acquire();

		MONITOR_INC(MONITOR_TABLE_REFERENCE);
	}

	ut_ad(dict_lru_validate());

	if (!dict_locked) {
		dict_table_try_drop_aborted_and_mutex_exit(table, try_drop);
	}

	DBUG_RETURN(table);
}

/**********************************************************************//**
Adds system columns to a table object. */
void
dict_table_add_system_columns(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table */
	mem_heap_t*	heap)	/*!< in: temporary heap */
{
	ut_ad(table->n_def == table->n_cols - DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(!table->cached);

	/* NOTE: the system columns MUST be added in the following order
	(so that they can be indexed by the numerical value of DATA_ROW_ID,
	etc.) and as the last columns of the table memory object.
	The clustered index will not always physically contain all system
	columns. */

	dict_mem_table_add_col(table, heap, "DB_ROW_ID", DATA_SYS,
			       DATA_ROW_ID | DATA_NOT_NULL,
			       DATA_ROW_ID_LEN);

#if DATA_ROW_ID != 0
#error "DATA_ROW_ID != 0"
#endif
	dict_mem_table_add_col(table, heap, "DB_TRX_ID", DATA_SYS,
			       DATA_TRX_ID | DATA_NOT_NULL,
			       DATA_TRX_ID_LEN);
#if DATA_TRX_ID != 1
#error "DATA_TRX_ID != 1"
#endif

	dict_mem_table_add_col(table, heap, "DB_ROLL_PTR", DATA_SYS,
			       DATA_ROLL_PTR | DATA_NOT_NULL,
			       DATA_ROLL_PTR_LEN);
#if DATA_ROLL_PTR != 2
#error "DATA_ROLL_PTR != 2"
#endif

	/* This check reminds that if a new system column is added to
	the program, it should be dealt with here */
#if DATA_N_SYS_COLS != 3
#error "DATA_N_SYS_COLS != 3"
#endif
}

/**********************************************************************//**
Adds a table object to the dictionary cache. */
void
dict_table_add_to_cache(
/*====================*/
	dict_table_t*	table,		/*!< in: table */
	bool		can_be_evicted,	/*!< in: whether can be evicted */
	mem_heap_t*	heap)		/*!< in: temporary heap */
{
	ulint	fold;
	ulint	id_fold;

	ut_ad(dict_lru_validate());
	ut_ad(mutex_own(&dict_sys->mutex));

	dict_table_add_system_columns(table, heap);

	mysql_mutex_init(0, &table->autoinc_mutex, NULL);

	table->cached = TRUE;

	fold = ut_fold_string(table->name.m_name);
	id_fold = ut_fold_ull(table->id);

	/* Look for a table with the same name: error if such exists */
	{
		dict_table_t*	table2;
		HASH_SEARCH(name_hash, dict_sys->table_hash, fold,
			    dict_table_t*, table2, ut_ad(table2->cached),
			    !strcmp(table2->name.m_name, table->name.m_name));
		ut_a(table2 == NULL);

#ifdef UNIV_DEBUG
		/* Look for the same table pointer with a different name */
		HASH_SEARCH_ALL(name_hash, dict_sys->table_hash,
				dict_table_t*, table2, ut_ad(table2->cached),
				table2 == table);
		ut_ad(table2 == NULL);
#endif /* UNIV_DEBUG */
	}

	/* Look for a table with the same id: error if such exists */
	{
		dict_table_t*	table2;
		HASH_SEARCH(id_hash, dict_sys->table_id_hash, id_fold,
			    dict_table_t*, table2, ut_ad(table2->cached),
			    table2->id == table->id);
		ut_a(table2 == NULL);

#ifdef UNIV_DEBUG
		/* Look for the same table pointer with a different id */
		HASH_SEARCH_ALL(id_hash, dict_sys->table_id_hash,
				dict_table_t*, table2, ut_ad(table2->cached),
				table2 == table);
		ut_ad(table2 == NULL);
#endif /* UNIV_DEBUG */
	}

	/* Add table to hash table of tables */
	HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash, fold,
		    table);

	/* Add table to hash table of tables based on table id */
	HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash, id_fold,
		    table);

	table->can_be_evicted = can_be_evicted;

	if (table->can_be_evicted) {
		UT_LIST_ADD_FIRST(dict_sys->table_LRU, table);
	} else {
		UT_LIST_ADD_FIRST(dict_sys->table_non_LRU, table);
	}

	ut_ad(dict_lru_validate());
}

/**********************************************************************//**
Test whether a table can be evicted from the LRU cache.
@return TRUE if table can be evicted. */
static
ibool
dict_table_can_be_evicted(
/*======================*/
	dict_table_t*	table)		/*!< in: table to test */
{
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(rw_lock_own(&dict_operation_lock, RW_LOCK_X));

	ut_a(table->can_be_evicted);
	ut_a(table->foreign_set.empty());
	ut_a(table->referenced_set.empty());

	if (table->get_ref_count() == 0) {
		/* The transaction commit and rollback are called from
		outside the handler interface. This means that there is
		a window where the table->n_ref_count can be zero but
		the table instance is in "use". */

		if (lock_table_has_locks(table)) {
			return(FALSE);
		}

#ifdef BTR_CUR_HASH_ADAPT
		/* We cannot really evict the table if adaptive hash
		index entries are pointing to any of its indexes. */
		for (dict_index_t* index = dict_table_get_first_index(table);
		     index != NULL;
		     index = dict_table_get_next_index(index)) {
			if (index->n_ahi_pages()) {
				return(FALSE);
			}
		}
#endif /* BTR_CUR_HASH_ADAPT */

		return(TRUE);
	}

	return(FALSE);
}

#ifdef BTR_CUR_HASH_ADAPT
/** @return a clone of this */
dict_index_t *dict_index_t::clone() const
{
  ut_ad(n_fields);
  ut_ad(!(type & (DICT_IBUF | DICT_SPATIAL | DICT_FTS)));
  ut_ad(online_status == ONLINE_INDEX_COMPLETE);
  ut_ad(is_committed());
  ut_ad(!is_dummy);
  ut_ad(!parser);
  ut_ad(!index_fts_syncing);
  ut_ad(!online_log);
  ut_ad(!rtr_track);

  const size_t size= sizeof *this + n_fields * sizeof(*fields) +
#ifdef BTR_CUR_ADAPT
    sizeof *search_info +
#endif
    1 + strlen(name) +
    n_uniq * (sizeof *stat_n_diff_key_vals +
              sizeof *stat_n_sample_sizes +
              sizeof *stat_n_non_null_key_vals);

  mem_heap_t* heap= mem_heap_create(size);
  dict_index_t *index= static_cast<dict_index_t*>(mem_heap_dup(heap, this,
                                                               sizeof *this));
  *index= *this;
  rw_lock_create(index_tree_rw_lock_key, &index->lock, SYNC_INDEX_TREE);
  index->heap= heap;
  index->name= mem_heap_strdup(heap, name);
  index->fields= static_cast<dict_field_t*>
    (mem_heap_dup(heap, fields, n_fields * sizeof *fields));
#ifdef BTR_CUR_ADAPT
  index->search_info= btr_search_info_create(index->heap);
#endif /* BTR_CUR_ADAPT */
  index->stat_n_diff_key_vals= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_diff_key_vals));
  index->stat_n_sample_sizes= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_sample_sizes));
  index->stat_n_non_null_key_vals= static_cast<ib_uint64_t*>
    (mem_heap_zalloc(heap, n_uniq * sizeof *stat_n_non_null_key_vals));
  mysql_mutex_init(0, &index->zip_pad.mutex, NULL);
  return index;
}

/** Clone this index for lazy dropping of the adaptive hash.
@return this or a clone */
dict_index_t *dict_index_t::clone_if_needed()
{
  if (!search_info->ref_count)
    return this;
  dict_index_t *prev= UT_LIST_GET_PREV(indexes, this);

  mysql_mutex_lock(&table->autoinc_mutex);
  UT_LIST_REMOVE(table->indexes, this);
  UT_LIST_ADD_LAST(table->freed_indexes, this);
  dict_index_t *index= clone();
  set_freed();
  if (prev)
    UT_LIST_INSERT_AFTER(table->indexes, prev, index);
  else
    UT_LIST_ADD_FIRST(table->indexes, index);
  mysql_mutex_unlock(&table->autoinc_mutex);
  return index;
}
#endif /* BTR_CUR_HASH_ADAPT */

/**********************************************************************//**
Make room in the table cache by evicting an unused table. The unused table
should not be part of FK relationship and currently not used in any user
transaction. There is no guarantee that it will remove a table.
@return number of tables evicted. If the number of tables in the dict_LRU
is less than max_tables it will not do anything. */
ulint
dict_make_room_in_cache(
/*====================*/
	ulint		max_tables,	/*!< in: max tables allowed in cache */
	ulint		pct_check)	/*!< in: max percent to check */
{
	ulint		i;
	ulint		len;
	dict_table_t*	table;
	ulint		check_up_to;
	ulint		n_evicted = 0;

	ut_a(pct_check > 0);
	ut_a(pct_check <= 100);
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(rw_lock_own(&dict_operation_lock, RW_LOCK_X));
	ut_ad(dict_lru_validate());

	i = len = UT_LIST_GET_LEN(dict_sys->table_LRU);

	if (len < max_tables) {
		return(0);
	}

	check_up_to = len - ((len * pct_check) / 100);

	/* Check for overflow */
	ut_a(i == 0 || check_up_to <= i);

	/* Find a suitable candidate to evict from the cache. Don't scan the
	entire LRU list. Only scan pct_check list entries. */

	for (table = UT_LIST_GET_LAST(dict_sys->table_LRU);
	     table != NULL
	     && i > check_up_to
	     && (len - n_evicted) > max_tables;
	     --i) {

		dict_table_t*	prev_table;

	        prev_table = UT_LIST_GET_PREV(table_LRU, table);

		if (dict_table_can_be_evicted(table)) {
			ut_ad(!table->fts);
			dict_table_remove_from_cache_low(table, TRUE);

			++n_evicted;
		}

		table = prev_table;
	}

	return(n_evicted);
}

/**********************************************************************//**
Move a table to the non-LRU list from the LRU list. */
void
dict_table_move_from_lru_to_non_lru(
/*================================*/
	dict_table_t*	table)	/*!< in: table to move from LRU to non-LRU */
{
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(dict_lru_find_table(table));

	ut_a(table->can_be_evicted);

	UT_LIST_REMOVE(dict_sys->table_LRU, table);

	UT_LIST_ADD_LAST(dict_sys->table_non_LRU, table);

	table->can_be_evicted = FALSE;
}

/** Looks for an index with the given id given a table instance.
@param[in]	table	table instance
@param[in]	id	index id
@return index or NULL */
dict_index_t*
dict_table_find_index_on_id(
	const dict_table_t*	table,
	index_id_t		id)
{
	dict_index_t*	index;

	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		if (id == index->id) {
			/* Found */

			return(index);
		}
	}

	return(NULL);
}

/**********************************************************************//**
Looks for an index with the given id. NOTE that we do not reserve
the dictionary mutex: this function is for emergency purposes like
printing info of a corrupt database page!
@return index or NULL if not found in cache */
dict_index_t*
dict_index_find_on_id_low(
/*======================*/
	index_id_t	id)	/*!< in: index id */
{
	dict_table_t*	table;

	/* This can happen if the system tablespace is the wrong page size */
	if (dict_sys == NULL) {
		return(NULL);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys->table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		dict_index_t*	index = dict_table_find_index_on_id(table, id);

		if (index != NULL) {
			return(index);
		}
	}

	for (table = UT_LIST_GET_FIRST(dict_sys->table_non_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		dict_index_t*	index = dict_table_find_index_on_id(table, id);

		if (index != NULL) {
			return(index);
		}
	}

	return(NULL);
}

/** Function object to remove a foreign key constraint from the
referenced_set of the referenced table.  The foreign key object is
also removed from the dictionary cache.  The foreign key constraint
is not removed from the foreign_set of the table containing the
constraint. */
struct dict_foreign_remove_partial
{
	void operator()(dict_foreign_t* foreign) {
		dict_table_t*	table = foreign->referenced_table;
		if (table != NULL) {
			table->referenced_set.erase(foreign);
		}
		dict_foreign_free(foreign);
	}
};

/**********************************************************************//**
Renames a table object.
@return TRUE if success */
dberr_t
dict_table_rename_in_cache(
/*=======================*/
	dict_table_t*	table,		/*!< in/out: table */
	const char*	new_name,	/*!< in: new name */
	bool		rename_also_foreigns,
					/*!< in: in ALTER TABLE we want
					to preserve the original table name
					in constraints which reference it */
	bool		replace_new_file)
					/*!< in: whether to replace the
					file with the new name
					(as part of rolling back TRUNCATE) */
{
	dberr_t		err;
	dict_foreign_t*	foreign;
	dict_index_t*	index;
	ulint		fold;
	char		old_name[MAX_FULL_NAME_LEN + 1];
	os_file_type_t	ftype;

	ut_ad(mutex_own(&dict_sys->mutex));

	/* store the old/current name to an automatic variable */
	ut_a(strlen(table->name.m_name) < sizeof old_name);
	strcpy(old_name, table->name.m_name);

	fold = ut_fold_string(new_name);

	/* Look for a table with the same name: error if such exists */
	dict_table_t*	table2;
	HASH_SEARCH(name_hash, dict_sys->table_hash, fold,
			dict_table_t*, table2, ut_ad(table2->cached),
			(ut_strcmp(table2->name.m_name, new_name) == 0));
	DBUG_EXECUTE_IF("dict_table_rename_in_cache_failure",
		if (table2 == NULL) {
			table2 = (dict_table_t*) -1;
		} );
	if (table2) {
		ib::error() << "Cannot rename table '" << old_name
			<< "' to '" << new_name << "' since the"
			" dictionary cache already contains '" << new_name << "'.";
		return(DB_ERROR);
	}

	/* If the table is stored in a single-table tablespace, rename the
	.ibd file and rebuild the .isl file if needed. */

	if (dict_table_is_discarded(table)) {
		bool		exists;
		char*		filepath;

		ut_ad(dict_table_is_file_per_table(table));
		ut_ad(!dict_table_is_temporary(table));

		/* Make sure the data_dir_path is set. */
		dict_get_and_save_data_dir_path(table, true);

		if (DICT_TF_HAS_DATA_DIR(table->flags)) {
			ut_a(table->data_dir_path);

			filepath = fil_make_filepath(
				table->data_dir_path, table->name.m_name,
				IBD, true);
		} else {
			filepath = fil_make_filepath(
				NULL, table->name.m_name, IBD, false);
		}

		if (filepath == NULL) {
			return(DB_OUT_OF_MEMORY);
		}

		fil_delete_tablespace(table->space,
				      dict_table_is_discarded(table));

		/* Delete any temp file hanging around. */
		if (os_file_status(filepath, &exists, &ftype)
		    && exists
		    && !os_file_delete_if_exists(innodb_temp_file_key,
						 filepath, NULL)) {

			ib::info() << "Delete of " << filepath << " failed.";
		}
		ut_free(filepath);

	} else if (dict_table_is_file_per_table(table)) {
		char*	new_path = NULL;
		char*	old_path = fil_space_get_first_path(table->space);

		ut_ad(!dict_table_is_temporary(table));

		if (DICT_TF_HAS_DATA_DIR(table->flags)) {
			new_path = os_file_make_new_pathname(
				old_path, new_name);
			err = RemoteDatafile::create_link_file(
				new_name, new_path);

			if (err != DB_SUCCESS) {
				ut_free(new_path);
				ut_free(old_path);
				return(DB_TABLESPACE_EXISTS);
			}
		} else {
			new_path = fil_make_filepath(
				NULL, new_name, IBD, false);
		}

		/* New filepath must not exist. */
		err = fil_rename_tablespace_check(
			table->space, old_path, new_path, false,
			replace_new_file);
		if (err != DB_SUCCESS) {
			ut_free(old_path);
			ut_free(new_path);
			return(err);
		}

		fil_name_write_rename(table->space, old_path, new_path);

		bool	success = fil_rename_tablespace(
			table->space, old_path, new_name, new_path);

		ut_free(old_path);
		ut_free(new_path);

		/* If the tablespace is remote, a new .isl file was created
		If success, delete the old one. If not, delete the new one. */
		if (DICT_TF_HAS_DATA_DIR(table->flags)) {
			RemoteDatafile::delete_link_file(
				success ? old_name : new_name);
		}

		if (!success) {
			return(DB_ERROR);
		}
	}

	/* Remove table from the hash tables of tables */
	HASH_DELETE(dict_table_t, name_hash, dict_sys->table_hash,
		    ut_fold_string(old_name), table);

	if (strlen(new_name) > strlen(table->name.m_name)) {
		/* We allocate MAX_FULL_NAME_LEN + 1 bytes here to avoid
		memory fragmentation, we assume a repeated calls of
		ut_realloc() with the same size do not cause fragmentation */
		ut_a(strlen(new_name) <= MAX_FULL_NAME_LEN);

		table->name.m_name = static_cast<char*>(
			ut_realloc(table->name.m_name, MAX_FULL_NAME_LEN + 1));
	}
	strcpy(table->name.m_name, new_name);

	/* Add table to hash table of tables */
	HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash, fold,
		    table);

	/* Update the table_name field in indexes */
	for (index = dict_table_get_first_index(table);
	     index != NULL;
	     index = dict_table_get_next_index(index)) {

		index->table_name = table->name.m_name;
	}

	if (!rename_also_foreigns) {
		/* In ALTER TABLE we think of the rename table operation
		in the direction table -> temporary table (#sql...)
		as dropping the table with the old name and creating
		a new with the new name. Thus we kind of drop the
		constraints from the dictionary cache here. The foreign key
		constraints will be inherited to the new table from the
		system tables through a call of dict_load_foreigns. */

		/* Remove the foreign constraints from the cache */
		std::for_each(table->foreign_set.begin(),
			      table->foreign_set.end(),
			      dict_foreign_remove_partial());
		table->foreign_set.clear();

		/* Reset table field in referencing constraints */
		for (dict_foreign_set::iterator it
			= table->referenced_set.begin();
		     it != table->referenced_set.end();
		     ++it) {

			foreign = *it;
			foreign->referenced_table = NULL;
			foreign->referenced_index = NULL;

		}

		/* Make the set of referencing constraints empty */
		table->referenced_set.clear();

		return(DB_SUCCESS);
	}

	/* Update the table name fields in foreign constraints, and update also
	the constraint id of new format >= 4.0.18 constraints. Note that at
	this point we have already changed table->name to the new name. */

	dict_foreign_set	fk_set;

	for (;;) {

		dict_foreign_set::iterator	it
			= table->foreign_set.begin();

		if (it == table->foreign_set.end()) {
			break;
		}

		foreign = *it;

		if (foreign->referenced_table) {
			foreign->referenced_table->referenced_set.erase(foreign);
		}

		if (ut_strlen(foreign->foreign_table_name)
		    < ut_strlen(table->name.m_name)) {
			/* Allocate a longer name buffer;
			TODO: store buf len to save memory */

			foreign->foreign_table_name = mem_heap_strdup(
				foreign->heap, table->name.m_name);
			dict_mem_foreign_table_name_lookup_set(foreign, TRUE);
		} else {
			strcpy(foreign->foreign_table_name,
			       table->name.m_name);
			dict_mem_foreign_table_name_lookup_set(foreign, FALSE);
		}
		if (strchr(foreign->id, '/')) {
			/* This is a >= 4.0.18 format id */

			ulint	db_len;
			char*	old_id;
			char    old_name_cs_filename[MAX_FULL_NAME_LEN+1];
			uint    errors = 0;

			/* All table names are internally stored in charset
			my_charset_filename (except the temp tables and the
			partition identifier suffix in partition tables). The
			foreign key constraint names are internally stored
			in UTF-8 charset.  The variable fkid here is used
			to store foreign key constraint name in charset
			my_charset_filename for comparison further below. */
			char    fkid[MAX_TABLE_NAME_LEN+20];
			ibool	on_tmp = FALSE;

			/* The old table name in my_charset_filename is stored
			in old_name_cs_filename */

			strcpy(old_name_cs_filename, old_name);
			old_name_cs_filename[MAX_FULL_NAME_LEN] = '\0';
			if (strstr(old_name, TEMP_TABLE_PATH_PREFIX) == NULL) {

				innobase_convert_to_system_charset(
					strchr(old_name_cs_filename, '/') + 1,
					strchr(old_name, '/') + 1,
					MAX_TABLE_NAME_LEN, &errors);

				if (errors) {
					/* There has been an error to convert
					old table into UTF-8.  This probably
					means that the old table name is
					actually in UTF-8. */
					innobase_convert_to_filename_charset(
						strchr(old_name_cs_filename,
						       '/') + 1,
						strchr(old_name, '/') + 1,
						MAX_TABLE_NAME_LEN);
				} else {
					/* Old name already in
					my_charset_filename */
					strcpy(old_name_cs_filename, old_name);
					old_name_cs_filename[MAX_FULL_NAME_LEN]
						= '\0';
				}
			}

			strncpy(fkid, foreign->id, MAX_TABLE_NAME_LEN);

			if (strstr(fkid, TEMP_TABLE_PATH_PREFIX) == NULL) {
				innobase_convert_to_filename_charset(
					strchr(fkid, '/') + 1,
					strchr(foreign->id, '/') + 1,
					MAX_TABLE_NAME_LEN+20);
			} else {
				on_tmp = TRUE;
			}

			old_id = mem_strdup(foreign->id);

			if (ut_strlen(fkid) > ut_strlen(old_name_cs_filename)
			    + ((sizeof dict_ibfk) - 1)
			    && !memcmp(fkid, old_name_cs_filename,
				       ut_strlen(old_name_cs_filename))
			    && !memcmp(fkid + ut_strlen(old_name_cs_filename),
				       dict_ibfk, (sizeof dict_ibfk) - 1)) {

				/* This is a generated >= 4.0.18 format id */

				char	table_name[MAX_TABLE_NAME_LEN + 1];
				uint	errors = 0;

				if (strlen(table->name.m_name)
				    > strlen(old_name)) {
					foreign->id = static_cast<char*>(
						mem_heap_alloc(
						foreign->heap,
						strlen(table->name.m_name)
						+ strlen(old_id) + 1));
				}

				/* Convert the table name to UTF-8 */
				strncpy(table_name, table->name.m_name,
					MAX_TABLE_NAME_LEN);
				table_name[MAX_TABLE_NAME_LEN] = '\0';
				innobase_convert_to_system_charset(
					strchr(table_name, '/') + 1,
					strchr(table->name.m_name, '/') + 1,
					MAX_TABLE_NAME_LEN, &errors);

				if (errors) {
					/* Table name could not be converted
					from charset my_charset_filename to
					UTF-8. This means that the table name
					is already in UTF-8 (#mysql50#). */
					strncpy(table_name, table->name.m_name,
						MAX_TABLE_NAME_LEN);
					table_name[MAX_TABLE_NAME_LEN] = '\0';
				}

				/* Replace the prefix 'databasename/tablename'
				with the new names */
				strcpy(foreign->id, table_name);
				if (on_tmp) {
					strcat(foreign->id,
					       old_id + ut_strlen(old_name));
				} else {
					sprintf(strchr(foreign->id, '/') + 1,
						"%s%s",
						strchr(table_name, '/') +1,
						strstr(old_id, "_ibfk_") );
				}

			} else {
				/* This is a >= 4.0.18 format id where the user
				gave the id name */
				db_len = dict_get_db_name_len(
					table->name.m_name) + 1;

				if (db_len - 1
				    > dict_get_db_name_len(foreign->id)) {

					foreign->id = static_cast<char*>(
						mem_heap_alloc(
						foreign->heap,
						db_len + strlen(old_id) + 1));
				}

				/* Replace the database prefix in id with the
				one from table->name */

				ut_memcpy(foreign->id,
					  table->name.m_name, db_len);

				strcpy(foreign->id + db_len,
				       dict_remove_db_name(old_id));
			}

			ut_free(old_id);
		}

		table->foreign_set.erase(it);
		fk_set.insert(foreign);

		if (foreign->referenced_table) {
			foreign->referenced_table->referenced_set.insert(foreign);
		}
	}

	ut_a(table->foreign_set.empty());
	table->foreign_set.swap(fk_set);

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;

		if (ut_strlen(foreign->referenced_table_name)
		    < ut_strlen(table->name.m_name)) {
			/* Allocate a longer name buffer;
			TODO: store buf len to save memory */

			foreign->referenced_table_name = mem_heap_strdup(
				foreign->heap, table->name.m_name);

			dict_mem_referenced_table_name_lookup_set(
				foreign, TRUE);
		} else {
			/* Use the same buffer */
			strcpy(foreign->referenced_table_name,
			       table->name.m_name);

			dict_mem_referenced_table_name_lookup_set(
				foreign, FALSE);
		}
	}

	return(DB_SUCCESS);
}

/**********************************************************************//**
Change the id of a table object in the dictionary cache. This is used in
DISCARD TABLESPACE. */
void
dict_table_change_id_in_cache(
/*==========================*/
	dict_table_t*	table,	/*!< in/out: table object already in cache */
	table_id_t	new_id)	/*!< in: new id to set */
{
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Remove the table from the hash table of id's */

	HASH_DELETE(dict_table_t, id_hash, dict_sys->table_id_hash,
		    ut_fold_ull(table->id), table);
	table->id = new_id;

	/* Add the table back to the hash table */
	HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash,
		    ut_fold_ull(table->id), table);
}

/**********************************************************************//**
Removes a table object from the dictionary cache. */
void
dict_table_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in, own: table */
	ibool		lru_evict)	/*!< in: TRUE if table being evicted
					to make room in the table LRU list */
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;

	ut_ad(dict_lru_validate());
	ut_a(table->get_ref_count() == 0);
	ut_a(table->n_rec_locks == 0);
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Remove the foreign constraints from the cache */
	std::for_each(table->foreign_set.begin(), table->foreign_set.end(),
		      dict_foreign_remove_partial());
	table->foreign_set.clear();

	/* Reset table field in referencing constraints */
	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;
		foreign->referenced_table = NULL;
		foreign->referenced_index = NULL;
	}

	/* Remove the indexes from the cache */

	for (index = UT_LIST_GET_LAST(table->indexes);
	     index != NULL;
	     index = UT_LIST_GET_LAST(table->indexes)) {

		dict_index_remove_from_cache_low(table, index, lru_evict);
	}

	/* Remove table from the hash tables of tables */

	HASH_DELETE(dict_table_t, name_hash, dict_sys->table_hash,
		    ut_fold_string(table->name.m_name), table);

	HASH_DELETE(dict_table_t, id_hash, dict_sys->table_id_hash,
		    ut_fold_ull(table->id), table);

	/* Remove table from LRU or non-LRU list. */
	if (table->can_be_evicted) {
		ut_ad(dict_lru_find_table(table));
		UT_LIST_REMOVE(dict_sys->table_LRU, table);
	} else {
		ut_ad(dict_non_lru_find_table(table));
		UT_LIST_REMOVE(dict_sys->table_non_LRU, table);
	}

	ut_ad(dict_lru_validate());

	if (lru_evict && table->drop_aborted) {
		/* When evicting the table definition,
		drop the orphan indexes from the data dictionary
		and free the index pages. */
		trx_t* trx = trx_allocate_for_background();

		ut_ad(mutex_own(&dict_sys->mutex));
		ut_ad(rw_lock_own(&dict_operation_lock, RW_LOCK_X));

		/* Mimic row_mysql_lock_data_dictionary(). */
		trx->dict_operation_lock_mode = RW_X_LATCH;

		trx_set_dict_operation(trx, TRX_DICT_OP_INDEX);
		row_merge_drop_indexes_dict(trx, table->id);
		trx_commit_for_mysql(trx);
		trx->dict_operation_lock_mode = 0;
		trx_free_for_background(trx);
	}

	/* Free virtual column template if any */
	if (table->vc_templ != NULL) {
		dict_free_vc_templ(table->vc_templ);
		UT_DELETE(table->vc_templ);
	}

#ifdef BTR_CUR_HASH_ADAPT
	if (table->fts) {
		fts_optimize_remove_table(table);
		fts_free(table);
		table->fts = NULL;
	}

	mysql_mutex_lock(&table->autoinc_mutex);

	ulint freed = UT_LIST_GET_LEN(table->freed_indexes);

	table->vc_templ = NULL;
	table->id = 0;
	mysql_mutex_unlock(&table->autoinc_mutex);

	if (UNIV_UNLIKELY(freed != 0)) {
		return;
	}
#endif /* BTR_CUR_HASH_ADAPT */

	mysql_mutex_destroy(&table->autoinc_mutex);
	dict_mem_table_free(table);
}

/**********************************************************************//**
Removes a table object from the dictionary cache. */
void
dict_table_remove_from_cache(
/*=========================*/
	dict_table_t*	table)	/*!< in, own: table */
{
	dict_table_remove_from_cache_low(table, FALSE);
}

/****************************************************************//**
If the given column name is reserved for InnoDB system columns, return
TRUE.
@return TRUE if name is reserved */
ibool
dict_col_name_is_reserved(
/*======================*/
	const char*	name)	/*!< in: column name */
{
	/* This check reminds that if a new system column is added to
	the program, it should be dealt with here. */
#if DATA_N_SYS_COLS != 3
#error "DATA_N_SYS_COLS != 3"
#endif

	static const char*	reserved_names[] = {
		"DB_ROW_ID", "DB_TRX_ID", "DB_ROLL_PTR"
	};

	ulint			i;

	for (i = 0; i < UT_ARR_SIZE(reserved_names); i++) {
		if (innobase_strcasecmp(name, reserved_names[i]) == 0) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/** Clears the virtual column's index list before index is
being freed.
@param[in]  index   Index being freed */
void
dict_index_remove_from_v_col_list(dict_index_t* index) {
	/* Index is not completely formed */
	if (!index->cached) {
		return;
	}
        if (dict_index_has_virtual(index)) {
                const dict_col_t*       col;
                const dict_v_col_t*     vcol;

                for (ulint i = 0; i < dict_index_get_n_fields(index); i++) {
                        col =  dict_index_get_nth_col(index, i);
                        if (col && col->is_virtual()) {
                                vcol = reinterpret_cast<const dict_v_col_t*>(
                                        col);
				/* This could be NULL, when we do add
                                virtual column, add index together. We do not
                                need to track this virtual column's index */
				if (vcol->v_indexes == NULL) {
                                        continue;
                                }
				dict_v_idx_list::iterator       it;
				for (it = vcol->v_indexes->begin();
                                     it != vcol->v_indexes->end(); ++it) {
                                        dict_v_idx_t    v_index = *it;
                                        if (v_index.index == index) {
                                                vcol->v_indexes->erase(it);
                                                break;
                                        }
				}
			}
		}
	}
}

/** Adds an index to the dictionary cache, with possible indexing newly
added column.
@param[in,out]	table	table on which the index is
@param[in,out]	index	index; NOTE! The index memory
			object is freed in this function!
@param[in]	page_no	root page number of the index
@param[in]	add_v	virtual columns being added along with ADD INDEX
@return DB_SUCCESS, or DB_CORRUPTION */
dberr_t
dict_index_add_to_cache(
	dict_table_t*		table,
	dict_index_t*&		index,
	ulint			page_no,
	const dict_add_v_col_t* add_v)
{
	dict_index_t*	new_index;
	ulint		n_ord;
	ulint		i;

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(index->n_def == index->n_fields);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(!dict_index_is_ibuf(index));

	ut_d(mem_heap_validate(index->heap));
	ut_a(!dict_index_is_clust(index)
	     || UT_LIST_GET_LEN(table->indexes) == 0);

	if (!dict_index_find_cols(table, index, add_v)) {

		dict_mem_index_free(index);
		index = NULL;
		return DB_CORRUPTION;
	}

	/* Build the cache internal representation of the index,
	containing also the added system fields */

	if (index->type == DICT_FTS) {
		new_index = dict_index_build_internal_fts(table, index);
	} else if (dict_index_is_clust(index)) {
		new_index = dict_index_build_internal_clust(table, index);
	} else {
		new_index = dict_index_build_internal_non_clust(table, index);
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields in the cache internal representation */

	new_index->n_fields = new_index->n_def;
	new_index->trx_id = index->trx_id;
	new_index->set_committed(index->is_committed());
	new_index->nulls_equal = index->nulls_equal;
#ifdef MYSQL_INDEX_DISABLE_AHI
	new_index->disable_ahi = index->disable_ahi;
#endif

	n_ord = new_index->n_uniq;
	/* Flag the ordering columns and also set column max_prefix */

	for (i = 0; i < n_ord; i++) {
		const dict_field_t*	field
			= dict_index_get_nth_field(new_index, i);

		/* Check the column being added in the index for
		the first time and flag the ordering column. */
		if (field->col->ord_part == 0 ) {
			field->col->max_prefix = field->prefix_len;
			field->col->ord_part = 1;
		} else if (field->prefix_len == 0) {
			/* Set the max_prefix for a column to 0 if
			its prefix length is 0 (for this index)
			even if it was a part of any other index
			with some prefix length. */
			field->col->max_prefix = 0;
		} else if (field->col->max_prefix != 0
			   && field->prefix_len
			   > field->col->max_prefix) {
			/* Set the max_prefix value based on the
			prefix_len. */
			field->col->max_prefix = field->prefix_len;
		}
		ut_ad(field->col->ord_part == 1);
	}

	new_index->stat_n_diff_key_vals =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_diff_key_vals)));

	new_index->stat_n_sample_sizes =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_sample_sizes)));

	new_index->stat_n_non_null_key_vals =
		static_cast<ib_uint64_t*>(mem_heap_zalloc(
			new_index->heap,
			dict_index_get_n_unique(new_index)
			* sizeof(*new_index->stat_n_non_null_key_vals)));

	new_index->stat_index_size = 1;
	new_index->stat_n_leaf_pages = 1;

	new_index->stat_defrag_n_pages_freed = 0;
	new_index->stat_defrag_n_page_split = 0;

	new_index->stat_defrag_sample_next_slot = 0;
	memset(&new_index->stat_defrag_data_size_sample,
	       0x0, sizeof(ulint) * STAT_DEFRAG_DATA_SIZE_N_SAMPLE);

	/* Add the new index as the last index for the table */

	UT_LIST_ADD_LAST(table->indexes, new_index);
	new_index->table = table;
	new_index->table_name = table->name.m_name;
#ifdef BTR_CUR_ADAPT
	new_index->search_info = btr_search_info_create(new_index->heap);
#endif /* BTR_CUR_ADAPT */

	new_index->page = unsigned(page_no);
	rw_lock_create(index_tree_rw_lock_key, &new_index->lock,
		       SYNC_INDEX_TREE);

	dict_mem_index_free(index);
	index = new_index;
	return DB_SUCCESS;
}

/**********************************************************************//**
Removes an index from the dictionary cache. */
static
void
dict_index_remove_from_cache_low(
/*=============================*/
	dict_table_t*	table,		/*!< in/out: table */
	dict_index_t*	index,		/*!< in, own: index */
	ibool		lru_evict)	/*!< in: TRUE if index being evicted
					to make room in the table LRU list */
{
	ut_ad(table && index);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(table->id);
#ifdef BTR_CUR_HASH_ADAPT
	ut_ad(!index->freed());
#endif /* BTR_CUR_HASH_ADAPT */

	/* No need to acquire the dict_index_t::lock here because
	there can't be any active operations on this index (or table). */

	if (index->online_log) {
		ut_ad(index->online_status == ONLINE_INDEX_CREATION);
		row_log_free(index->online_log);
		index->online_log = NULL;
	}

	/* Remove the index from the list of indexes of the table */
	UT_LIST_REMOVE(table->indexes, index);

	/* The index is being dropped, remove any compression stats for it. */
	if (!lru_evict && DICT_TF_GET_ZIP_SSIZE(index->table->flags)) {
		mutex_enter(&page_zip_stat_per_index_mutex);
		page_zip_stat_per_index.erase(index->id);
		mutex_exit(&page_zip_stat_per_index_mutex);
	}

	/* Remove the index from affected virtual column index list */
	index->detach_columns();

#ifdef BTR_CUR_HASH_ADAPT
	/* We always create search info whether or not adaptive
	hash index is enabled or not. */
	/* We are not allowed to free the in-memory index struct
	dict_index_t until all entries in the adaptive hash index
	that point to any of the page belonging to his b-tree index
	are dropped. This is so because dropping of these entries
	require access to dict_index_t struct. To avoid such scenario
	We keep a count of number of such pages in the search_info and
	only free the dict_index_t struct when this count drops to
	zero. See also: dict_table_can_be_evicted() */

	if (index->n_ahi_pages()) {
		mysql_mutex_lock(&table->autoinc_mutex);
		index->set_freed();
		UT_LIST_ADD_LAST(table->freed_indexes, index);
		mysql_mutex_unlock(&table->autoinc_mutex);
		return;
	}
#endif /* BTR_CUR_HASH_ADAPT */

	rw_lock_free(&index->lock);

	dict_mem_index_free(index);
}

/**********************************************************************//**
Removes an index from the dictionary cache. */
void
dict_index_remove_from_cache(
/*=========================*/
	dict_table_t*	table,	/*!< in/out: table */
	dict_index_t*	index)	/*!< in, own: index */
{
	dict_index_remove_from_cache_low(table, index, FALSE);
}

/** Tries to find column names for the index and sets the col field of the
index.
@param[in]	table	table
@param[in,out]	index	index
@param[in]	add_v	new virtual columns added along with an add index call
@return TRUE if the column names were found */
static
ibool
dict_index_find_cols(
	const dict_table_t*	table,
	dict_index_t*		index,
	const dict_add_v_col_t*	add_v)
{
	std::vector<ulint, ut_allocator<ulint> >	col_added;
	std::vector<ulint, ut_allocator<ulint> >	v_col_added;

	ut_ad(table != NULL && index != NULL);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(mutex_own(&dict_sys->mutex));

	for (ulint i = 0; i < index->n_fields; i++) {
		ulint		j;
		dict_field_t*	field = dict_index_get_nth_field(index, i);

		for (j = 0; j < table->n_cols; j++) {
			if (!innobase_strcasecmp(dict_table_get_col_name(table, j),
				    field->name)) {

				/* Check if same column is being assigned again
				which suggest that column has duplicate name. */
				bool exists =
					std::find(col_added.begin(),
						  col_added.end(), j)
					!= col_added.end();

				if (exists) {
					/* Duplicate column found. */
					goto dup_err;
				}

				field->col = dict_table_get_nth_col(table, j);

				col_added.push_back(j);

				goto found;
			}
		}

		/* Let's check if it is a virtual column */
		for (j = 0; j < table->n_v_cols; j++) {
			if (!strcmp(dict_table_get_v_col_name(table, j),
				    field->name)) {

				/* Check if same column is being assigned again
				which suggest that column has duplicate name. */
				bool exists =
					std::find(v_col_added.begin(),
						  v_col_added.end(), j)
					!= v_col_added.end();

				if (exists) {
					/* Duplicate column found. */
					break;
				}

				field->col = reinterpret_cast<dict_col_t*>(
					dict_table_get_nth_v_col(table, j));

				v_col_added.push_back(j);

				goto found;
			}
		}

		if (add_v) {
			for (j = 0; j < add_v->n_v_col; j++) {
				if (!strcmp(add_v->v_col_name[j],
					    field->name)) {
					field->col = const_cast<dict_col_t*>(
						&add_v->v_col[j].m_col);
					goto found;
				}
			}
		}

dup_err:
#ifdef UNIV_DEBUG
		/* It is an error not to find a matching column. */
		ib::error() << "No matching column for " << field->name
			<< " in index " << index->name
			<< " of table " << table->name;
#endif /* UNIV_DEBUG */
		return(FALSE);

found:
		;
	}

	return(TRUE);
}

/*******************************************************************//**
Adds a column to index. */
void
dict_index_add_col(
/*===============*/
	dict_index_t*		index,		/*!< in/out: index */
	const dict_table_t*	table,		/*!< in: table */
	dict_col_t*		col,		/*!< in: column */
	ulint			prefix_len)	/*!< in: column prefix length */
{
	dict_field_t*	field;
	const char*	col_name;

	if (dict_col_is_virtual(col)) {
		dict_v_col_t*	v_col = reinterpret_cast<dict_v_col_t*>(col);

		/* When v_col->v_indexes==NULL,
		ha_innobase::commit_inplace_alter_table(commit=true)
		will evict and reload the table definition, and
		v_col->v_indexes will not be NULL for the new table. */
		if (v_col->v_indexes != NULL) {
			/* Register the index with the virtual column index
			list */
			struct dict_v_idx_t	new_idx
				 = {index, index->n_def};

			v_col->v_indexes->push_back(new_idx);

		}

		col_name = dict_table_get_v_col_name_mysql(
			table, dict_col_get_no(col));
	} else {
		col_name = dict_table_get_col_name(table, dict_col_get_no(col));
	}

	dict_mem_index_add_field(index, col_name, prefix_len);

	field = dict_index_get_nth_field(index, index->n_def - 1);

	field->col = col;
	field->fixed_len = static_cast<unsigned int>(
		dict_col_get_fixed_size(
			col, dict_table_is_comp(table)));

	if (prefix_len && field->fixed_len > prefix_len) {
		field->fixed_len = (unsigned int) prefix_len;
	}

	/* Long fixed-length fields that need external storage are treated as
	variable-length fields, so that the extern flag can be embedded in
	the length word. */

	if (field->fixed_len > DICT_MAX_FIXED_COL_LEN) {
		field->fixed_len = 0;
	}
#if DICT_MAX_FIXED_COL_LEN != 768
	/* The comparison limit above must be constant.  If it were
	changed, the disk format of some fixed-length columns would
	change, which would be a disaster. */
# error "DICT_MAX_FIXED_COL_LEN != 768"
#endif

	if (!(col->prtype & DATA_NOT_NULL)) {
		index->n_nullable++;
	}
}

/*******************************************************************//**
Copies fields contained in index2 to index1. */
static
void
dict_index_copy(
/*============*/
	dict_index_t*		index1,	/*!< in: index to copy to */
	dict_index_t*		index2,	/*!< in: index to copy from */
	const dict_table_t*	table,	/*!< in: table */
	ulint			start,	/*!< in: first position to copy */
	ulint			end)	/*!< in: last position to copy */
{
	dict_field_t*	field;
	ulint		i;

	/* Copy fields contained in index2 */

	for (i = start; i < end; i++) {

		field = dict_index_get_nth_field(index2, i);

		dict_index_add_col(index1, table, field->col,
				   field->prefix_len);
	}
}

/*******************************************************************//**
Copies types of fields contained in index to tuple. */
void
dict_index_copy_types(
/*==================*/
	dtuple_t*		tuple,		/*!< in/out: data tuple */
	const dict_index_t*	index,		/*!< in: index */
	ulint			n_fields)	/*!< in: number of
						field types to copy */
{
	ulint		i;

	if (dict_index_is_ibuf(index)) {
		dtuple_set_types_binary(tuple, n_fields);

		return;
	}

	for (i = 0; i < n_fields; i++) {
		const dict_field_t*	ifield;
		dtype_t*		dfield_type;

		ifield = dict_index_get_nth_field(index, i);
		dfield_type = dfield_get_type(dtuple_get_nth_field(tuple, i));
		dict_col_copy_type(dict_field_get_col(ifield), dfield_type);
		if (dict_index_is_spatial(index)
		    && DATA_GEOMETRY_MTYPE(dfield_type->mtype)) {
			dfield_type->prtype |= DATA_GIS_MBR;
		}
	}
}

/** Copies types of virtual columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create().
@param[in,out]	tuple	data tuple
@param[in]	table	table
*/
void
dict_table_copy_v_types(
	dtuple_t*		tuple,
	const dict_table_t*	table)
{
	/* tuple could have more virtual columns than existing table,
	if we are calling this for creating index along with adding
	virtual columns */
	ulint	n_fields = ut_min(dtuple_get_n_v_fields(tuple),
				  static_cast<ulint>(table->n_v_def));

	for (ulint i = 0; i < n_fields; i++) {

		dfield_t*	dfield	= dtuple_get_nth_v_field(tuple, i);
		dtype_t*	dtype	= dfield_get_type(dfield);

		dfield_set_null(dfield);
		dict_col_copy_type(
			&(dict_table_get_nth_v_col(table, i)->m_col),
			dtype);
	}
}
/*******************************************************************//**
Copies types of columns contained in table to tuple and sets all
fields of the tuple to the SQL NULL value.  This function should
be called right after dtuple_create(). */
void
dict_table_copy_types(
/*==================*/
	dtuple_t*		tuple,	/*!< in/out: data tuple */
	const dict_table_t*	table)	/*!< in: table */
{
	ulint		i;

	for (i = 0; i < dtuple_get_n_fields(tuple); i++) {

		dfield_t*	dfield	= dtuple_get_nth_field(tuple, i);
		dtype_t*	dtype	= dfield_get_type(dfield);

		dfield_set_null(dfield);
		dict_col_copy_type(dict_table_get_nth_col(table, i), dtype);
	}

	dict_table_copy_v_types(tuple, table);
}

/*******************************************************************//**
Builds the internal dictionary cache representation for a clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the clustered index */
static
dict_index_t*
dict_index_build_internal_clust(
/*============================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index)	/*!< in: user representation of
					a clustered index */
{
	dict_index_t*	new_index;
	dict_field_t*	field;
	ulint		trx_id_pos;
	ulint		i;
	ibool*		indexed;

	ut_ad(table && index);
	ut_ad(dict_index_is_clust(index));
	ut_ad(!dict_index_is_ibuf(index));

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Create a new index object with certainly enough fields */
	new_index = dict_mem_index_create(table->name.m_name,
					  index->name, table->space,
					  index->type,
					  index->n_fields + table->n_cols);

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy the fields of index */
	dict_index_copy(new_index, index, table, 0, index->n_fields);

	if (dict_index_is_unique(index)) {
		/* Only the fields defined so far are needed to identify
		the index entry uniquely */

		new_index->n_uniq = new_index->n_def;
	} else {
		/* Also the row id is needed to identify the entry */
		new_index->n_uniq = 1 + new_index->n_def;
	}

	new_index->trx_id_offset = 0;

	/* Add system columns, trx id first */

	trx_id_pos = new_index->n_def;

#if DATA_ROW_ID != 0
# error "DATA_ROW_ID != 0"
#endif
#if DATA_TRX_ID != 1
# error "DATA_TRX_ID != 1"
#endif
#if DATA_ROLL_PTR != 2
# error "DATA_ROLL_PTR != 2"
#endif

	if (!dict_index_is_unique(index)) {
		dict_index_add_col(new_index, table,
				   dict_table_get_sys_col(
					   table, DATA_ROW_ID),
				   0);
		trx_id_pos++;
	}

	dict_index_add_col(
		new_index, table,
		dict_table_get_sys_col(table, DATA_TRX_ID), 0);

	for (i = 0; i < trx_id_pos; i++) {

		ulint	fixed_size = dict_col_get_fixed_size(
			dict_index_get_nth_col(new_index, i),
			dict_table_is_comp(table));

		if (fixed_size == 0) {
			new_index->trx_id_offset = 0;

			break;
		}

		dict_field_t* field = dict_index_get_nth_field(
			new_index, i);
		if (field->prefix_len > 0) {
			new_index->trx_id_offset = 0;

			break;
		}

		/* Add fixed_size to new_index->trx_id_offset.
		Because the latter is a bit-field, an overflow
		can theoretically occur. Check for it. */
		fixed_size += new_index->trx_id_offset;

		new_index->trx_id_offset = unsigned(fixed_size);

		if (new_index->trx_id_offset != fixed_size) {
			/* Overflow. Pretend that this is a
			variable-length PRIMARY KEY. */
			ut_ad(0);
			new_index->trx_id_offset = 0;
			break;
		}
	}

	dict_index_add_col(
		new_index, table,
		dict_table_get_sys_col(table, DATA_ROLL_PTR), 0);

	/* Remember the table columns already contained in new_index */
	indexed = static_cast<ibool*>(
		ut_zalloc_nokey(table->n_cols * sizeof *indexed));

	/* Mark the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

			indexed[field->col->ind] = TRUE;
		}
	}

	/* Add to new_index non-system columns of table not yet included
	there */
	for (i = 0; i + DATA_N_SYS_COLS < ulint(table->n_cols); i++) {
		dict_col_t*	col = dict_table_get_nth_col(table, i);
		ut_ad(col->mtype != DATA_SYS);

		if (!indexed[col->ind]) {
			dict_index_add_col(new_index, table, col, 0);
		}
	}

	ut_free(indexed);

	ut_ad(UT_LIST_GET_LEN(table->indexes) == 0);

	new_index->cached = TRUE;

	return(new_index);
}

/*******************************************************************//**
Builds the internal dictionary cache representation for a non-clustered
index, containing also system fields not defined by the user.
@return own: the internal representation of the non-clustered index */
static
dict_index_t*
dict_index_build_internal_non_clust(
/*================================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index)	/*!< in: user representation of
					a non-clustered index */
{
	dict_field_t*	field;
	dict_index_t*	new_index;
	dict_index_t*	clust_index;
	ulint		i;
	ibool*		indexed;

	ut_ad(table && index);
	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* The clustered index should be the first in the list of indexes */
	clust_index = UT_LIST_GET_FIRST(table->indexes);

	ut_ad(clust_index);
	ut_ad(dict_index_is_clust(clust_index));
	ut_ad(!dict_index_is_ibuf(clust_index));

	/* Create a new index */
	new_index = dict_mem_index_create(
		table->name.m_name, index->name, index->space, index->type,
		index->n_fields + 1 + clust_index->n_uniq);

	/* Copy other relevant data from the old index
	struct to the new struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, table, 0, index->n_fields);

	/* Remember the table columns already contained in new_index */
	indexed = static_cast<ibool*>(
		ut_zalloc_nokey(table->n_cols * sizeof *indexed));

	/* Mark the table columns already contained in new_index */
	for (i = 0; i < new_index->n_def; i++) {

		field = dict_index_get_nth_field(new_index, i);

		if (dict_col_is_virtual(field->col)) {
			continue;
		}

		/* If there is only a prefix of the column in the index
		field, do not mark the column as contained in the index */

		if (field->prefix_len == 0) {

			indexed[field->col->ind] = TRUE;
		}
	}

	/* Add to new_index the columns necessary to determine the clustered
	index entry uniquely */

	for (i = 0; i < clust_index->n_uniq; i++) {

		field = dict_index_get_nth_field(clust_index, i);

		if (!indexed[field->col->ind]) {
			dict_index_add_col(new_index, table, field->col,
					   field->prefix_len);
		} else if (dict_index_is_spatial(index)) {
			/*For spatial index, we still need to add the
			field to index. */
			dict_index_add_col(new_index, table, field->col,
					   field->prefix_len);
		}
	}

	ut_free(indexed);

	if (dict_index_is_unique(index)) {
		new_index->n_uniq = index->n_fields;
	} else {
		new_index->n_uniq = new_index->n_def;
	}

	/* Set the n_fields value in new_index to the actual defined
	number of fields */

	new_index->n_fields = new_index->n_def;

	new_index->cached = TRUE;

	return(new_index);
}

/***********************************************************************
Builds the internal dictionary cache representation for an FTS index.
@return own: the internal representation of the FTS index */
static
dict_index_t*
dict_index_build_internal_fts(
/*==========================*/
	dict_table_t*	table,	/*!< in: table */
	dict_index_t*	index)	/*!< in: user representation of an FTS index */
{
	dict_index_t*	new_index;

	ut_ad(table && index);
	ut_ad(index->type == DICT_FTS);
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/* Create a new index */
	new_index = dict_mem_index_create(
		table->name.m_name, index->name, index->space, index->type,
		index->n_fields);

	/* Copy other relevant data from the old index struct to the new
	struct: it inherits the values */

	new_index->n_user_defined_cols = index->n_fields;

	new_index->id = index->id;

	/* Copy fields from index to new_index */
	dict_index_copy(new_index, index, table, 0, index->n_fields);

	new_index->n_uniq = 0;
	new_index->cached = TRUE;

	if (table->fts->cache == NULL) {
		table->fts->cache = fts_cache_create(table);
	}

	rw_lock_x_lock(&table->fts->cache->init_lock);
	/* Notify the FTS cache about this index. */
	fts_cache_index_cache_create(table, new_index);
	rw_lock_x_unlock(&table->fts->cache->init_lock);

	return(new_index);
}
/*====================== FOREIGN KEY PROCESSING ========================*/

/** Check whether the dict_table_t is a partition.
A partitioned table on the SQL level is composed of InnoDB tables,
where each InnoDB table is a [sub]partition including its secondary indexes
which belongs to the partition.
@param[in]	table	Table to check.
@return true if the dict_table_t is a partition else false. */
UNIV_INLINE
bool
dict_table_is_partition(
	const dict_table_t*	table)
{
	/* Check both P and p on all platforms in case it was moved to/from
	WIN. */
	return(strstr(table->name.m_name, "#p#")
	       || strstr(table->name.m_name, "#P#"));
}

/*********************************************************************//**
Checks if a table is referenced by foreign keys.
@return TRUE if table is referenced by a foreign key */
ibool
dict_table_is_referenced_by_foreign_key(
/*====================================*/
	const dict_table_t*	table)	/*!< in: InnoDB table */
{
	return(!table->referenced_set.empty());
}

/**********************************************************************//**
Removes a foreign constraint struct from the dictionary cache. */
void
dict_foreign_remove_from_cache(
/*===========================*/
	dict_foreign_t*	foreign)	/*!< in, own: foreign constraint */
{
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_a(foreign);

	if (foreign->referenced_table != NULL) {
		foreign->referenced_table->referenced_set.erase(foreign);
	}

	if (foreign->foreign_table != NULL) {
		foreign->foreign_table->foreign_set.erase(foreign);
	}

	dict_foreign_free(foreign);
}

/**********************************************************************//**
Looks for the foreign constraint from the foreign and referenced lists
of a table.
@return foreign constraint */
static
dict_foreign_t*
dict_foreign_find(
/*==============*/
	dict_table_t*	table,		/*!< in: table object */
	dict_foreign_t*	foreign)	/*!< in: foreign constraint */
{
	ut_ad(mutex_own(&dict_sys->mutex));

	ut_ad(dict_foreign_set_validate(table->foreign_set));
	ut_ad(dict_foreign_set_validate(table->referenced_set));

	dict_foreign_set::iterator it = table->foreign_set.find(foreign);

	if (it != table->foreign_set.end()) {
		return(*it);
	}

	it = table->referenced_set.find(foreign);

	if (it != table->referenced_set.end()) {
		return(*it);
	}

	return(NULL);
}

/*********************************************************************//**
Tries to find an index whose first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return matching index, NULL if not found */
dict_index_t*
dict_foreign_find_index(
/*====================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
{
	ut_ad(mutex_own(&dict_sys->mutex));

	if (error) {
		*error = FK_INDEX_NOT_FOUND;
	}

	for (dict_index_t* index = dict_table_get_first_index(table);
	     index;
	     index = dict_table_get_next_index(index)) {
		if (types_idx != index
		    && !index->to_be_dropped
		    && !dict_index_is_online_ddl(index)
		    && dict_foreign_qualify_index(
			    table, col_names, columns, n_cols,
			    index, types_idx,
			    check_charsets, check_null,
			    error, err_col_no, err_index)) {
			if (error) {
				*error = FK_SUCCESS;
			}

			return(index);
		}
	}

	return(NULL);
}
/**********************************************************************//**
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report_low(
/*==========================*/
	FILE*		file,	/*!< in: output stream */
	const char*	name)	/*!< in: table name */
{
	rewind(file);
	ut_print_timestamp(file);
	fprintf(file, " Error in foreign key constraint of table %s:\n",
		name);
}

/**********************************************************************//**
Report an error in a foreign key definition. */
static
void
dict_foreign_error_report(
/*======================*/
	FILE*		file,	/*!< in: output stream */
	dict_foreign_t*	fk,	/*!< in: foreign key constraint */
	const char*	msg)	/*!< in: the error message */
{
	std::string fk_str;
	mutex_enter(&dict_foreign_err_mutex);
	dict_foreign_error_report_low(file, fk->foreign_table_name);
	fputs(msg, file);
	fputs(" Constraint:\n", file);
	fk_str = dict_print_info_on_foreign_key_in_create_format(NULL, fk, TRUE);
	fputs(fk_str.c_str(), file);
	putc('\n', file);
	if (fk->foreign_index) {
		fprintf(file, "The index in the foreign key in table is"
			" %s\n%s\n", fk->foreign_index->name(),
			FOREIGN_KEY_CONSTRAINTS_MSG);
	}
	mutex_exit(&dict_foreign_err_mutex);
}

/**********************************************************************//**
Adds a foreign key constraint object to the dictionary cache. May free
the object if there already is an object with the same identifier in.
At least one of the foreign table and the referenced table must already
be in the dictionary cache!
@return DB_SUCCESS or error code */
dberr_t
dict_foreign_add_to_cache(
/*======================*/
	dict_foreign_t*		foreign,
				/*!< in, own: foreign key constraint */
	const char**		col_names,
				/*!< in: column names, or NULL to use
				foreign->foreign_table->col_names */
	bool			check_charsets,
				/*!< in: whether to check charset
				compatibility */
	dict_err_ignore_t	ignore_err)
				/*!< in: error to be ignored */
{
	dict_table_t*	for_table;
	dict_table_t*	ref_table;
	dict_foreign_t*	for_in_cache		= NULL;
	dict_index_t*	index;
	ibool		added_to_referenced_list= FALSE;
	FILE*		ef			= dict_foreign_err_file;

	DBUG_ENTER("dict_foreign_add_to_cache");
	DBUG_PRINT("dict_foreign_add_to_cache", ("id: %s", foreign->id));

	ut_ad(mutex_own(&dict_sys->mutex));

	for_table = dict_table_check_if_in_cache_low(
		foreign->foreign_table_name_lookup);

	ref_table = dict_table_check_if_in_cache_low(
		foreign->referenced_table_name_lookup);
	ut_a(for_table || ref_table);

	if (for_table) {
		for_in_cache = dict_foreign_find(for_table, foreign);
	}

	if (!for_in_cache && ref_table) {
		for_in_cache = dict_foreign_find(ref_table, foreign);
	}

	if (for_in_cache) {
		dict_foreign_free(foreign);
	} else {
		for_in_cache = foreign;

	}

	if (ref_table && !for_in_cache->referenced_table) {
		index = dict_foreign_find_index(
			ref_table, NULL,
			for_in_cache->referenced_col_names,
			for_in_cache->n_fields, for_in_cache->foreign_index,
			check_charsets, false);

		if (index == NULL
		    && !(ignore_err & DICT_ERR_IGNORE_FK_NOKEY)) {
			dict_foreign_error_report(
				ef, for_in_cache,
				"there is no index in referenced table"
				" which would contain\n"
				"the columns as the first columns,"
				" or the data types in the\n"
				"referenced table do not match"
				" the ones in table.");

			if (for_in_cache == foreign) {
				dict_foreign_free(foreign);
			}

			DBUG_RETURN(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->referenced_table = ref_table;
		for_in_cache->referenced_index = index;

		std::pair<dict_foreign_set::iterator, bool>	ret
			= ref_table->referenced_set.insert(for_in_cache);

		ut_a(ret.second);	/* second is true if the insertion
					took place */
		added_to_referenced_list = TRUE;
	}

	if (for_table && !for_in_cache->foreign_table) {
		index = dict_foreign_find_index(
			for_table, col_names,
			for_in_cache->foreign_col_names,
			for_in_cache->n_fields,
			for_in_cache->referenced_index, check_charsets,
			for_in_cache->type
			& (DICT_FOREIGN_ON_DELETE_SET_NULL
			   | DICT_FOREIGN_ON_UPDATE_SET_NULL));

		if (index == NULL
		    && !(ignore_err & DICT_ERR_IGNORE_FK_NOKEY)) {
			dict_foreign_error_report(
				ef, for_in_cache,
				"there is no index in the table"
				" which would contain\n"
				"the columns as the first columns,"
				" or the data types in the\n"
				"table do not match"
				" the ones in the referenced table\n"
				"or one of the ON ... SET NULL columns"
				" is declared NOT NULL.");

			if (for_in_cache == foreign) {
				if (added_to_referenced_list) {
					const dict_foreign_set::size_type
						n = ref_table->referenced_set
						  .erase(for_in_cache);

					ut_a(n == 1);	/* the number of
							elements removed must
							be one */
				}

				dict_foreign_free(foreign);
			}

			DBUG_RETURN(DB_CANNOT_ADD_CONSTRAINT);
		}

		for_in_cache->foreign_table = for_table;
		for_in_cache->foreign_index = index;

		std::pair<dict_foreign_set::iterator, bool>	ret
			= for_table->foreign_set.insert(for_in_cache);

		ut_a(ret.second);	/* second is true if the insertion
					took place */
	}

	/* We need to move the table to the non-LRU end of the table LRU
	list. Otherwise it will be evicted from the cache. */

	if (ref_table != NULL) {
		dict_table_prevent_eviction(ref_table);
	}

	if (for_table != NULL) {
		dict_table_prevent_eviction(for_table);
	}

	ut_ad(dict_lru_validate());
	DBUG_RETURN(DB_SUCCESS);
}

/*********************************************************************//**
Scans from pointer onwards. Stops if is at the start of a copy of
'string' where characters are compared without case sensitivity, and
only outside `` or "" quotes. Stops also at NUL.
@return scanned up to this */
static
const char*
dict_scan_to(
/*=========*/
	const char*	ptr,	/*!< in: scan from */
	const char*	string)	/*!< in: look for this */
{
	char	quote	= '\0';
	bool	escape	= false;

	for (; *ptr; ptr++) {
		if (*ptr == quote) {
			/* Closing quote character: do not look for
			starting quote or the keyword. */

			/* If the quote character is escaped by a
			backslash, ignore it. */
			if (escape) {
				escape = false;
			} else {
				quote = '\0';
			}
		} else if (quote) {
			/* Within quotes: do nothing. */
			if (escape) {
				escape = false;
			} else if (*ptr == '\\') {
				escape = true;
			}
		} else if (*ptr == '`' || *ptr == '"' || *ptr == '\'') {
			/* Starting quote: remember the quote character. */
			quote = *ptr;
		} else {
			/* Outside quotes: look for the keyword. */
			ulint	i;
			for (i = 0; string[i]; i++) {
				if (toupper((int)(unsigned char)(ptr[i]))
				    != toupper((int)(unsigned char)
					       (string[i]))) {
					goto nomatch;
				}
			}
			break;
nomatch:
			;
		}
	}

	return(ptr);
}

/*********************************************************************//**
Accepts a specified string. Comparisons are case-insensitive.
@return if string was accepted, the pointer is moved after that, else
ptr is returned */
static
const char*
dict_accept(
/*========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scan from this */
	const char*	string,	/*!< in: accept only this string as the next
				non-whitespace string */
	ibool*		success)/*!< out: TRUE if accepted */
{
	const char*	old_ptr = ptr;
	const char*	old_ptr2;

	*success = FALSE;

	while (my_isspace(cs, *ptr)) {
		ptr++;
	}

	old_ptr2 = ptr;

	ptr = dict_scan_to(ptr, string);

	if (*ptr == '\0' || old_ptr2 != ptr) {
		return(old_ptr);
	}

	*success = TRUE;

	return(ptr + ut_strlen(string));
}

/*********************************************************************//**
Scans an id. For the lexical definition of an 'id', see the code below.
Strips backquotes or double quotes from around the id.
@return scanned to */
static
const char*
dict_scan_id(
/*=========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scanned to */
	mem_heap_t*	heap,	/*!< in: heap where to allocate the id
				(NULL=id will not be allocated, but it
				will point to string near ptr) */
	const char**	id,	/*!< out,own: the id; NULL if no id was
				scannable */
	ibool		table_id,/*!< in: TRUE=convert the allocated id
				as a table name; FALSE=convert to UTF-8 */
	ibool		accept_also_dot)
				/*!< in: TRUE if also a dot can appear in a
				non-quoted id; in a quoted id it can appear
				always */
{
	char		quote	= '\0';
	ulint		len	= 0;
	const char*	s;
	char*		str;
	char*		dst;

	*id = NULL;

	while (my_isspace(cs, *ptr)) {
		ptr++;
	}

	if (*ptr == '\0') {

		return(ptr);
	}

	if (*ptr == '`' || *ptr == '"') {
		quote = *ptr++;
	}

	s = ptr;

	if (quote) {
		for (;;) {
			if (!*ptr) {
				/* Syntax error */
				return(ptr);
			}
			if (*ptr == quote) {
				ptr++;
				if (*ptr != quote) {
					break;
				}
			}
			ptr++;
			len++;
		}
	} else {
		while (!my_isspace(cs, *ptr) && *ptr != '(' && *ptr != ')'
		       && (accept_also_dot || *ptr != '.')
		       && *ptr != ',' && *ptr != '\0') {

			ptr++;
		}

		len = ptr - s;
	}

	if (heap == NULL) {
		/* no heap given: id will point to source string */
		*id = s;
		return(ptr);
	}

	if (quote) {
		char*	d;

		str = d = static_cast<char*>(
			mem_heap_alloc(heap, len + 1));

		while (len--) {
			if ((*d++ = *s++) == quote) {
				s++;
			}
		}
		*d++ = 0;
		len = d - str;
		ut_ad(*s == quote);
		ut_ad(s + 1 == ptr);
	} else {
		str = mem_heap_strdupl(heap, s, len);
	}

	if (!table_id) {
convert_id:
		/* Convert the identifier from connection character set
		to UTF-8. */
		len = 3 * len + 1;
		*id = dst = static_cast<char*>(mem_heap_alloc(heap, len));

		innobase_convert_from_id(cs, dst, str, len);
	} else if (!strncmp(str, srv_mysql50_table_name_prefix,
			    sizeof(srv_mysql50_table_name_prefix) - 1)) {
		/* This is a pre-5.1 table name
		containing chars other than [A-Za-z0-9].
		Discard the prefix and use raw UTF-8 encoding. */
		str += sizeof(srv_mysql50_table_name_prefix) - 1;
		len -= sizeof(srv_mysql50_table_name_prefix) - 1;
		goto convert_id;
	} else {
		/* Encode using filename-safe characters. */
		len = 5 * len + 1;
		*id = dst = static_cast<char*>(mem_heap_alloc(heap, len));

		innobase_convert_from_table_id(cs, dst, str, len);
	}

	return(ptr);
}

/*********************************************************************//**
Tries to scan a column name.
@return scanned to */
static
const char*
dict_scan_col(
/*==========*/
	CHARSET_INFO*		cs,	/*!< in: the character set of ptr */
	const char*		ptr,	/*!< in: scanned to */
	ibool*			success,/*!< out: TRUE if success */
	dict_table_t*		table,	/*!< in: table in which the column is */
	const dict_col_t**	column,	/*!< out: pointer to column if success */
	mem_heap_t*		heap,	/*!< in: heap where to allocate */
	const char**		name)	/*!< out,own: the column name;
					NULL if no name was scannable */
{
	ulint		i;

	*success = FALSE;

	ptr = dict_scan_id(cs, ptr, heap, name, FALSE, TRUE);

	if (*name == NULL) {

		return(ptr);	/* Syntax error */
	}

	if (table == NULL) {
		*success = TRUE;
		*column = NULL;
	} else {
		for (i = 0; i < dict_table_get_n_cols(table); i++) {

			const char*	col_name = dict_table_get_col_name(
				table, i);

			if (0 == innobase_strcasecmp(col_name, *name)) {
				/* Found */

				*success = TRUE;
				*column = dict_table_get_nth_col(table, i);
				strcpy((char*) *name, col_name);

				break;
			}
		}

		for (i = 0; i < dict_table_get_n_v_cols(table); i++) {

			const char*	col_name = dict_table_get_v_col_name(
				table, i);

			if (0 == innobase_strcasecmp(col_name, *name)) {
				/* Found */
				dict_v_col_t * vcol;
				*success = TRUE;
				vcol = dict_table_get_nth_v_col(table, i);
				*column = &vcol->m_col;
				strcpy((char*) *name, col_name);

				break;
			}
		}
	}

	return(ptr);
}

/*********************************************************************//**
Open a table from its database and table name, this is currently used by
foreign constraint parser to get the referenced table.
@return complete table name with database and table name, allocated from
heap memory passed in */
char*
dict_get_referenced_table(
/*======================*/
	const char*	name,		/*!< in: foreign key table name */
	const char*	database_name,	/*!< in: table db name */
	ulint		database_name_len, /*!< in: db name length */
	const char*	table_name,	/*!< in: table name */
	ulint		table_name_len, /*!< in: table name length */
	dict_table_t**	table,		/*!< out: table object or NULL */
	mem_heap_t*	heap)		/*!< in/out: heap memory */
{
	char*		ref;
	const char*	db_name;

	if (!database_name) {
		/* Use the database name of the foreign key table */

		db_name = name;
		database_name_len = dict_get_db_name_len(name);
	} else {
		db_name = database_name;
	}

	/* Copy database_name, '/', table_name, '\0' */
	ref = static_cast<char*>(
		mem_heap_alloc(heap, database_name_len + table_name_len + 2));

	memcpy(ref, db_name, database_name_len);
	ref[database_name_len] = '/';
	memcpy(ref + database_name_len + 1, table_name, table_name_len + 1);

	/* Values;  0 = Store and compare as given; case sensitive
	            1 = Store and compare in lower; case insensitive
	            2 = Store as given, compare in lower; case semi-sensitive */
	if (innobase_get_lower_case_table_names() == 2) {
		innobase_casedn_str(ref);
		*table = dict_table_get_low(ref);
		memcpy(ref, db_name, database_name_len);
		ref[database_name_len] = '/';
		memcpy(ref + database_name_len + 1, table_name, table_name_len + 1);

	} else {
#ifndef _WIN32
		if (innobase_get_lower_case_table_names() == 1) {
			innobase_casedn_str(ref);
		}
#else
		innobase_casedn_str(ref);
#endif /* !_WIN32 */
		*table = dict_table_get_low(ref);
	}

	return(ref);
}
/*********************************************************************//**
Scans a table name from an SQL string.
@return scanned to */
static
const char*
dict_scan_table_name(
/*=================*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scanned to */
	dict_table_t**	table,	/*!< out: table object or NULL */
	const char*	name,	/*!< in: foreign key table name */
	ibool*		success,/*!< out: TRUE if ok name found */
	mem_heap_t*	heap,	/*!< in: heap where to allocate the id */
	const char**	ref_name)/*!< out,own: the table name;
				NULL if no name was scannable */
{
	const char*	database_name	= NULL;
	ulint		database_name_len = 0;
	const char*	table_name	= NULL;
	const char*	scan_name;

	*success = FALSE;
	*table = NULL;

	ptr = dict_scan_id(cs, ptr, heap, &scan_name, TRUE, FALSE);

	if (scan_name == NULL) {

		return(ptr);	/* Syntax error */
	}

	if (*ptr == '.') {
		/* We scanned the database name; scan also the table name */

		ptr++;

		database_name = scan_name;
		database_name_len = strlen(database_name);

		ptr = dict_scan_id(cs, ptr, heap, &table_name, TRUE, FALSE);

		if (table_name == NULL) {

			return(ptr);	/* Syntax error */
		}
	} else {
		/* To be able to read table dumps made with InnoDB-4.0.17 or
		earlier, we must allow the dot separator between the database
		name and the table name also to appear within a quoted
		identifier! InnoDB used to print a constraint as:
		... REFERENCES `databasename.tablename` ...
		starting from 4.0.18 it is
		... REFERENCES `databasename`.`tablename` ... */
		const char* s;

		for (s = scan_name; *s; s++) {
			if (*s == '.') {
				database_name = scan_name;
				database_name_len = s - scan_name;
				scan_name = ++s;
				break;/* to do: multiple dots? */
			}
		}

		table_name = scan_name;
	}

	*ref_name = dict_get_referenced_table(
		name, database_name, database_name_len,
		table_name, strlen(table_name), table, heap);

	*success = TRUE;
	return(ptr);
}

/*********************************************************************//**
Skips one id. The id is allowed to contain also '.'.
@return scanned to */
static
const char*
dict_skip_word(
/*===========*/
	CHARSET_INFO*	cs,	/*!< in: the character set of ptr */
	const char*	ptr,	/*!< in: scanned to */
	ibool*		success)/*!< out: TRUE if success, FALSE if just spaces
				left in string or a syntax error */
{
	const char*	start;

	*success = FALSE;

	ptr = dict_scan_id(cs, ptr, NULL, &start, FALSE, TRUE);

	if (start) {
		*success = TRUE;
	}

	return(ptr);
}

/*********************************************************************//**
Removes MySQL comments from an SQL string. A comment is either
(a) '#' to the end of the line,
(b) '--[space]' to the end of the line, or
(c) '[slash][asterisk]' till the next '[asterisk][slash]' (like the familiar
C comment syntax).
@return own: SQL string stripped from comments; the caller must free
this with ut_free()! */
static
char*
dict_strip_comments(
/*================*/
	const char*	sql_string,	/*!< in: SQL string */
	size_t		sql_length)	/*!< in: length of sql_string */
{
	char*		str;
	const char*	sptr;
	const char*	eptr	= sql_string + sql_length;
	char*		ptr;
	/* unclosed quote character (0 if none) */
	char		quote	= 0;
	bool		escape = false;

	DBUG_ENTER("dict_strip_comments");

	DBUG_PRINT("dict_strip_comments", ("%s", sql_string));

	str = static_cast<char*>(ut_malloc_nokey(sql_length + 1));

	sptr = sql_string;
	ptr = str;

	for (;;) {
scan_more:
		if (sptr >= eptr || *sptr == '\0') {
end_of_string:
			*ptr = '\0';

			ut_a(ptr <= str + sql_length);

			DBUG_PRINT("dict_strip_comments", ("%s", str));
			DBUG_RETURN(str);
		}

		if (*sptr == quote) {
			/* Closing quote character: do not look for
			starting quote or comments. */

			/* If the quote character is escaped by a
			backslash, ignore it. */
			if (escape) {
				escape = false;
			} else {
				quote = 0;
			}
		} else if (quote) {
			/* Within quotes: do not look for
			starting quotes or comments. */
			if (escape) {
				escape = false;
			} else if (*sptr == '\\') {
				escape = true;
			}
		} else if (*sptr == '"' || *sptr == '`' || *sptr == '\'') {
			/* Starting quote: remember the quote character. */
			quote = *sptr;
		} else if (*sptr == '#'
			   || (sptr[0] == '-' && sptr[1] == '-'
			       && sptr[2] == ' ')) {
			for (;;) {
				if (++sptr >= eptr) {
					goto end_of_string;
				}

				/* In Unix a newline is 0x0A while in Windows
				it is 0x0D followed by 0x0A */

				switch (*sptr) {
				case (char) 0X0A:
				case (char) 0x0D:
				case '\0':
					goto scan_more;
				}
			}
		} else if (!quote && *sptr == '/' && *(sptr + 1) == '*') {
			sptr += 2;
			for (;;) {
				if (sptr >= eptr) {
					goto end_of_string;
				}

				switch (*sptr) {
				case '\0':
					goto scan_more;
				case '*':
					if (sptr[1] == '/') {
						sptr += 2;
						goto scan_more;
					}
				}

				sptr++;
			}
		}

		*ptr = *sptr;

		ptr++;
		sptr++;
	}
}

/*********************************************************************//**
Finds the highest [number] for foreign key constraints of the table. Looks
only at the >= 4.0.18-format id's, which are of the form
databasename/tablename_ibfk_[number].
@return highest number, 0 if table has no new format foreign key constraints */
ulint
dict_table_get_highest_foreign_id(
/*==============================*/
	dict_table_t*	table)	/*!< in: table in the dictionary memory cache */
{
	dict_foreign_t*	foreign;
	char*		endp;
	ulint		biggest_id	= 0;
	ulint		id;
	ulint		len;

	DBUG_ENTER("dict_table_get_highest_foreign_id");

	ut_a(table);

	len = ut_strlen(table->name.m_name);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {
		char    fkid[MAX_TABLE_NAME_LEN+20];
		foreign = *it;

		strcpy(fkid, foreign->id);
		/* Convert foreign key identifier on dictionary memory
		cache to filename charset. */
		innobase_convert_to_filename_charset(
				strchr(fkid, '/') + 1,
				strchr(foreign->id, '/') + 1,
				MAX_TABLE_NAME_LEN);

		if (ut_strlen(fkid) > ((sizeof dict_ibfk) - 1) + len
		    && 0 == ut_memcmp(fkid, table->name.m_name, len)
		    && 0 == ut_memcmp(fkid + len,
				      dict_ibfk, (sizeof dict_ibfk) - 1)
		    && fkid[len + ((sizeof dict_ibfk) - 1)] != '0') {
			/* It is of the >= 4.0.18 format */

			id = strtoul(fkid + len
				     + ((sizeof dict_ibfk) - 1),
				     &endp, 10);
			if (*endp == '\0') {
				ut_a(id != biggest_id);

				if (id > biggest_id) {
					biggest_id = id;
				}
			}
		}
	}

	DBUG_PRINT("dict_table_get_highest_foreign_id",
		   ("id: " ULINTPF, biggest_id));

	DBUG_RETURN(biggest_id);
}

/*********************************************************************//**
Reports a simple foreign key create clause syntax error. */
static
void
dict_foreign_report_syntax_err(
/*===========================*/
	const char*     fmt,		/*!< in: syntax err msg */
	const char*	oper,		/*!< in: operation */
	const char*	name,		/*!< in: table name */
	const char*	start_of_latest_foreign,
					/*!< in: start of the foreign key clause
					in the SQL string */
	const char*	ptr)		/*!< in: place of the syntax error */
{
	ut_ad(!srv_read_only_mode);

	FILE*	ef = dict_foreign_err_file;

	mutex_enter(&dict_foreign_err_mutex);
	dict_foreign_error_report_low(ef, name);
	fprintf(ef, fmt, oper, name, start_of_latest_foreign, ptr);
	mutex_exit(&dict_foreign_err_mutex);
}

/*********************************************************************//**
Push warning message to SQL-layer based on foreign key constraint
index match error. */
static
void
dict_foreign_push_index_error(
/*==========================*/
	trx_t*		trx,		/*!< in: trx */
	const char*	operation,	/*!< in: operation create or alter
					*/
	const char*	create_name,	/*!< in: table name in create or
					alter table */
	const char*	latest_foreign,	/*!< in: start of latest foreign key
					constraint name */
	const char**	columns,	/*!< in: foreign key columns */
	fkerr_t		index_error,	/*!< in: error code */
	ulint		err_col,	/*!< in: column where error happened
					*/
	dict_index_t*	err_index,	/*!< in: index where error happened
					*/
	dict_table_t*	table,		/*!< in: table */
	FILE*		ef)		/*!< in: output stream */
{
	switch (index_error) {
	case FK_SUCCESS:
		break;
	case FK_INDEX_NOT_FOUND:
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. There is no index in the referenced"
			" table where the referenced columns appear"
			" as the first columns near '%s'.\n",
			operation, create_name, latest_foreign);
		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. There is no index in the referenced"
			" table where the referenced columns appear"
			" as the first columns near '%s'.",
			operation, create_name, latest_foreign);
		return;
	case FK_IS_PREFIX_INDEX:
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. There is only prefix index in the referenced"
			" table where the referenced columns appear"
			" as the first columns near '%s'.\n",
			operation, create_name, latest_foreign);
		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. There is only prefix index in the referenced"
			" table where the referenced columns appear"
			" as the first columns near '%s'.",
			operation, create_name, latest_foreign);
		return;
	case FK_COL_NOT_NULL:
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. You have defined a SET NULL condition but "
			"column '%s' on index is defined as NOT NULL near '%s'.\n",
			operation, create_name, columns[err_col], latest_foreign);
		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. You have defined a SET NULL condition but "
			"column '%s' on index is defined as NOT NULL near '%s'.",
			operation, create_name, columns[err_col], latest_foreign);
		return;
	case FK_COLS_NOT_EQUAL:
		dict_field_t*	field;
		const char*	col_name;
		field = dict_index_get_nth_field(err_index, err_col);

		col_name = dict_col_is_virtual(field->col)
			? "(null)"
			: dict_table_get_col_name(
				table, dict_col_get_no(field->col));
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. Field type or character set for column '%s' "
			"does not mach referenced column '%s' near '%s'.\n",
			operation, create_name, columns[err_col], col_name, latest_foreign);
		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Field type or character set for column '%s' "
			"does not mach referenced column '%s' near '%s'.",
			operation, create_name, columns[err_col], col_name, latest_foreign);
		return;
	}
	DBUG_ASSERT(!"unknown error");
}

/*********************************************************************//**
Scans a table create SQL string and adds to the data dictionary the foreign key
constraints declared in the string. This function should be called after the
indexes for a table have been created. Each foreign key constraint must be
accompanied with indexes in bot participating tables. The indexes are allowed
to contain more fields than mentioned in the constraint.
@return error code or DB_SUCCESS */
static
dberr_t
dict_create_foreign_constraints_low(
	trx_t*			trx,
	mem_heap_t*		heap,
	CHARSET_INFO*		cs,
	const char*		sql_string,
	const char*		name,
	ibool			reject_fks)
{
	dict_table_t*	table			= NULL;
	dict_table_t*	referenced_table	= NULL;
	dict_table_t*	table_to_alter		= NULL;
	dict_table_t*	table_to_create		= NULL;
	ulint		highest_id_so_far	= 0;
	ulint		number			= 1;
	dict_index_t*	index			= NULL;
	dict_foreign_t*	foreign			= NULL;
	const char*	ptr			= sql_string;
	const char*	start_of_latest_foreign	= sql_string;
	const char*	start_of_latest_set     = NULL;
	FILE*		ef			= dict_foreign_err_file;
	fkerr_t		index_error		= FK_SUCCESS;
	dict_index_t*	err_index		= NULL;
	ulint		err_col;
	const char*	constraint_name;
	ibool		success;
	dberr_t		error;
	const char*	ptr1;
	const char*	ptr2;
	ulint		i;
	ulint		j;
	ibool		is_on_delete;
	ulint		n_on_deletes;
	ulint		n_on_updates;
	const dict_col_t*columns[500];
	const char*	column_names[500];
	const char*	ref_column_names[500];
	const char*	referenced_table_name;
	dict_foreign_set	local_fk_set;
	dict_foreign_set_free	local_fk_set_free(local_fk_set);
	const char*	create_table_name;
	const char*	orig;
	char	create_name[MAX_TABLE_NAME_LEN + 1];

	ut_ad(!srv_read_only_mode);
	ut_ad(mutex_own(&dict_sys->mutex));

	table = dict_table_get_low(name);
	/* First check if we are actually doing an ALTER TABLE, and in that
	case look for the table being altered */
	orig = ptr;
	ptr = dict_accept(cs, ptr, "ALTER", &success);

	const char* const operation = success ? "Alter " : "Create ";

	if (!success) {
		orig = ptr;
		ptr = dict_scan_to(ptr, "CREATE");
		ptr = dict_scan_to(ptr, "TABLE");
		ptr = dict_accept(cs, ptr, "TABLE", &success);
		create_table_name = NULL;

		if (success) {
			ptr = dict_scan_table_name(cs, ptr, &table_to_create, name,
						   &success, heap, &create_table_name);
		}

		ptr = orig;
		const char* n = create_table_name ? create_table_name : name;
		char *bufend = innobase_convert_name(create_name, MAX_TABLE_NAME_LEN,
						     n, strlen(n), trx->mysql_thd);
		create_name[bufend-create_name] = '\0';
	} else {
		strncpy(create_name, name, sizeof create_name);
		create_name[(sizeof create_name) - 1] = '\0';
	}

	if (table == NULL) {
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, create_name);
		fprintf(ef, "%s table %s with foreign key constraint"
			" failed. Table %s not found from data dictionary."
			" Error close to %s.\n",
			operation, create_name, create_name, start_of_latest_foreign);
		mutex_exit(&dict_foreign_err_mutex);
		ib_push_warning(trx, DB_ERROR,
			"%s table %s with foreign key constraint"
			" failed. Table %s not found from data dictionary."
			" Error close to %s.",
			operation, create_name, create_name, start_of_latest_foreign);

		return(DB_ERROR);
	}

	/* If not alter table jump to loop */
	if (!success) {

		goto loop;
	}

	orig = ptr;
	for (;;) {
		ptr = dict_accept(cs, ptr, "TABLE", &success);
		if (success) {
			break;
		}
		ptr = dict_accept(cs, ptr, "ONLINE", &success);
		if (success) {
			continue;
		}
		ptr = dict_accept(cs, ptr, "IGNORE", &success);
		if (!success) {
			goto loop;
		}
	}

	/* We are doing an ALTER TABLE: scan the table name we are altering */

	orig = ptr;
	ptr = dict_scan_table_name(cs, ptr, &table_to_alter, name,
				   &success, heap, &referenced_table_name);

	{
		const char* n = table_to_alter
			? table_to_alter->name.m_name : referenced_table_name;
		char* bufend = innobase_convert_name(
			create_name, MAX_TABLE_NAME_LEN, n, strlen(n),
			trx->mysql_thd);
		create_name[bufend-create_name]='\0';
	}

	if (!success) {
		ib::error() << "Could not find the table " << create_name << " being" << operation << " near to "
			<< orig;

		ib_push_warning(trx, DB_ERROR,
			"%s table %s with foreign key constraint"
			" failed. Table %s not found from data dictionary."
			" Error close to %s.",
			operation, create_name, create_name, orig);

		return(DB_ERROR);
	}

	/* Starting from 4.0.18 and 4.1.2, we generate foreign key id's in the
	format databasename/tablename_ibfk_[number], where [number] is local
	to the table; look for the highest [number] for table_to_alter, so
	that we can assign to new constraints higher numbers. */

	/* If we are altering a temporary table, the table name after ALTER
	TABLE does not correspond to the internal table name, and
	table_to_alter is NULL. TODO: should we fix this somehow? */

	if (table_to_alter == NULL) {
		highest_id_so_far = 0;
	} else {
		highest_id_so_far = dict_table_get_highest_foreign_id(
			table_to_alter);
	}

	number = highest_id_so_far + 1;
	/* Scan for foreign key declarations in a loop */
loop:
	/* Scan either to "CONSTRAINT" or "FOREIGN", whichever is closer */

	ptr1 = dict_scan_to(ptr, "CONSTRAINT");
	ptr2 = dict_scan_to(ptr, "FOREIGN");

	constraint_name = NULL;

	if (ptr1 < ptr2) {
		/* The user may have specified a constraint name. Pick it so
		that we can store 'databasename/constraintname' as the id of
		of the constraint to system tables. */
		ptr = ptr1;

		orig = ptr;
		ptr = dict_accept(cs, ptr, "CONSTRAINT", &success);

		ut_a(success);

		if (!my_isspace(cs, *ptr) && *ptr != '"' && *ptr != '`') {
			goto loop;
		}

		while (my_isspace(cs, *ptr)) {
			ptr++;
		}

		/* read constraint name unless got "CONSTRAINT FOREIGN" */
		if (ptr != ptr2) {
			ptr = dict_scan_id(cs, ptr, heap,
					   &constraint_name, FALSE, FALSE);
		}
	} else {
		ptr = ptr2;
	}

	if (*ptr == '\0') {
		/* The proper way to reject foreign keys for temporary
		tables would be to split the lexing and syntactical
		analysis of foreign key clauses from the actual adding
		of them, so that ha_innodb.cc could first parse the SQL
		command, determine if there are any foreign keys, and
		if so, immediately reject the command if the table is a
		temporary one. For now, this kludge will work. */
		if (reject_fks && !local_fk_set.empty()) {
			mutex_enter(&dict_foreign_err_mutex);
			dict_foreign_error_report_low(ef, create_name);
			fprintf(ef, "%s table %s with foreign key constraint"
				" failed. Temporary tables can't have foreign key constraints."
				" Error close to %s.\n",
				operation, create_name, start_of_latest_foreign);
			mutex_exit(&dict_foreign_err_mutex);

			ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
				"%s table %s with foreign key constraint"
				" failed. Temporary tables can't have foreign key constraints."
				" Error close to %s.",
				operation, create_name, start_of_latest_foreign);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}

		if (dict_foreigns_has_s_base_col(local_fk_set, table)) {
			return(DB_NO_FK_ON_S_BASE_COL);
		}

		/**********************************************************/
		/* The following call adds the foreign key constraints
		to the data dictionary system tables on disk */

		error = dict_create_add_foreigns_to_dictionary(
			local_fk_set, table, trx);

		if (error == DB_SUCCESS) {

			table->foreign_set.insert(local_fk_set.begin(),
						  local_fk_set.end());
			std::for_each(local_fk_set.begin(),
				      local_fk_set.end(),
				      dict_foreign_add_to_referenced_table());
			local_fk_set.clear();

			dict_mem_table_fill_foreign_vcol_set(table);
		}
		return(error);
	}

	start_of_latest_foreign = ptr;

	orig = ptr;
	ptr = dict_accept(cs, ptr, "FOREIGN", &success);

	if (!success) {
		goto loop;
	}

	if (!my_isspace(cs, *ptr)) {
		goto loop;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "KEY", &success);

	if (!success) {
		goto loop;
	}

	if (my_isspace(cs, *ptr)) {
		ptr1 = dict_accept(cs, ptr, "IF", &success);

		if (success) {
			if (!my_isspace(cs, *ptr1)) {
				goto loop;
			}
			ptr1 = dict_accept(cs, ptr1, "NOT", &success);
			if (!success) {
				goto loop;
			}
			ptr1 = dict_accept(cs, ptr1, "EXISTS", &success);
			if (!success) {
				goto loop;
			}
			ptr = ptr1;
		}
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "(", &success);

	if (!success) {
		if (constraint_name) {
			/* MySQL allows also an index id before the '('; we
			skip it */
			ptr = dict_skip_word(cs, ptr, &success);
			if (!success) {
				dict_foreign_report_syntax_err(
					"%s table %s with foreign key constraint"
					" failed. Parse error in '%s'"
					" near '%s'.\n",
					operation, create_name, start_of_latest_foreign, orig);

				ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
					"%s table %s with foreign key constraint"
					" failed. Parse error in '%s'"
					" near '%s'.",
					operation, create_name, start_of_latest_foreign, orig);
				return(DB_CANNOT_ADD_CONSTRAINT);
			}
		} else {
			while (my_isspace(cs, *ptr)) {
				ptr++;
			}

			ptr = dict_scan_id(cs, ptr, heap,
				     &constraint_name, FALSE, FALSE);
		}

		ptr = dict_accept(cs, ptr, "(", &success);

		if (!success) {
			/* We do not flag a syntax error here because in an
			ALTER TABLE we may also have DROP FOREIGN KEY abc */

			goto loop;
		}
	}

	i = 0;

	/* Scan the columns in the first list */
col_loop1:
	ut_a(i < (sizeof column_names) / sizeof *column_names);
	orig = ptr;
	ptr = dict_scan_col(cs, ptr, &success, table, columns + i,
			    heap, column_names + i);
	if (!success) {
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, create_name);
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, orig);

		mutex_exit(&dict_foreign_err_mutex);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, orig);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	i++;

	ptr = dict_accept(cs, ptr, ",", &success);

	if (success) {
		goto col_loop1;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, ")", &success);

	if (!success) {
		dict_foreign_report_syntax_err(
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, orig);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, orig);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Try to find an index which contains the columns
	as the first fields and in the right order. There is
	no need to check column type match (on types_idx), since
	the referenced table can be NULL if foreign_key_checks is
	set to 0 */

	index = dict_foreign_find_index(
		table, NULL, column_names, i,
		NULL, TRUE, FALSE, &index_error, &err_col, &err_index);

	if (!index) {
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, create_name);
		fputs("There is no index in table ", ef);
		ut_print_name(ef, NULL, create_name);
		fprintf(ef, " where the columns appear\n"
			"as the first columns. Constraint:\n%s\n%s",
			start_of_latest_foreign,
			FOREIGN_KEY_CONSTRAINTS_MSG);
		dict_foreign_push_index_error(trx, operation, create_name, start_of_latest_foreign,
			column_names, index_error, err_col, err_index, table, ef);

		mutex_exit(&dict_foreign_err_mutex);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "REFERENCES", &success);

	if (!success || !my_isspace(cs, *ptr)) {
		dict_foreign_report_syntax_err(
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, orig);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, orig);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Don't allow foreign keys on partitioned tables yet. */
	ptr1 = dict_scan_to(ptr, "PARTITION");
	if (ptr1) {
		ptr1 = dict_accept(cs, ptr1, "PARTITION", &success);
		if (success && my_isspace(cs, *ptr1)) {
			ptr2 = dict_accept(cs, ptr1, "BY", &success);
			if (success) {
				my_error(ER_FOREIGN_KEY_ON_PARTITIONED,MYF(0));
				return(DB_CANNOT_ADD_CONSTRAINT);
			}
		}
	}
	if (dict_table_is_partition(table)) {
		my_error(ER_FOREIGN_KEY_ON_PARTITIONED,MYF(0));
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Don't allow foreign keys on partitioned tables yet. */
	ptr1 = dict_scan_to(ptr, "PARTITION");
	if (ptr1) {
		ptr1 = dict_accept(cs, ptr1, "PARTITION", &success);
		if (success && my_isspace(cs, *ptr1)) {
			ptr2 = dict_accept(cs, ptr1, "BY", &success);
			if (success) {
				my_error(ER_FOREIGN_KEY_ON_PARTITIONED,MYF(0));
				return(DB_CANNOT_ADD_CONSTRAINT);
			}
		}
	}
	if (dict_table_is_partition(table)) {
		my_error(ER_FOREIGN_KEY_ON_PARTITIONED,MYF(0));
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Let us create a constraint struct */

	foreign = dict_mem_foreign_create();

	if (constraint_name) {
		ulint	db_len;

		/* Catenate 'databasename/' to the constraint name specified
		by the user: we conceive the constraint as belonging to the
		same MySQL 'database' as the table itself. We store the name
		to foreign->id. */

		db_len = dict_get_db_name_len(table->name.m_name);

		foreign->id = static_cast<char*>(mem_heap_alloc(
			foreign->heap, db_len + strlen(constraint_name) + 2));

		ut_memcpy(foreign->id, table->name.m_name, db_len);
		foreign->id[db_len] = '/';
		strcpy(foreign->id + db_len + 1, constraint_name);
	}

	if (foreign->id == NULL) {
		error = dict_create_add_foreign_id(
			&number, table->name.m_name, foreign);
		if (error != DB_SUCCESS) {
			dict_foreign_free(foreign);
			return(error);
		}
	}

	std::pair<dict_foreign_set::iterator, bool>	ret
		= local_fk_set.insert(foreign);

	if (!ret.second) {
		/* A duplicate foreign key name has been found */
		dict_foreign_free(foreign);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	foreign->foreign_table = table;
	foreign->foreign_table_name = mem_heap_strdup(
		foreign->heap, table->name.m_name);
	dict_mem_foreign_table_name_lookup_set(foreign, TRUE);

	foreign->foreign_index = index;
	foreign->n_fields = (unsigned int) i;

	foreign->foreign_col_names = static_cast<const char**>(
		mem_heap_alloc(foreign->heap, i * sizeof(void*)));

	for (i = 0; i < foreign->n_fields; i++) {
		foreign->foreign_col_names[i] = mem_heap_strdup(
                        foreign->heap, column_names[i]);
	}

	ptr = dict_scan_table_name(cs, ptr, &referenced_table, name,
				   &success, heap, &referenced_table_name);

	/* Note that referenced_table can be NULL if the user has suppressed
	checking of foreign key constraints! */

	if (!success || (!referenced_table && trx->check_foreigns)) {
		char	buf[MAX_TABLE_NAME_LEN + 1] = "";
		char*	bufend;

		bufend = innobase_convert_name(buf, MAX_TABLE_NAME_LEN,
				referenced_table_name, strlen(referenced_table_name),
				trx->mysql_thd);
		buf[bufend - buf] = '\0';

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint failed. Referenced table %s not found in the data dictionary "
			"near '%s'.",
			operation, create_name, buf, start_of_latest_foreign);
		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, create_name);
		fprintf(ef,
			"%s table %s with foreign key constraint failed. Referenced table %s not found in the data dictionary "
			"near '%s'.\n",
			operation, create_name, buf, start_of_latest_foreign);

		mutex_exit(&dict_foreign_err_mutex);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Don't allow foreign keys on partitioned tables yet. */
	if (referenced_table && dict_table_is_partition(referenced_table)) {
		/* How could one make a referenced table to be a partition? */
		ut_ad(0);
		my_error(ER_FOREIGN_KEY_ON_PARTITIONED,MYF(0));
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	ptr = dict_accept(cs, ptr, "(", &success);

	if (!success) {
		dict_foreign_report_syntax_err(
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, orig);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, orig);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Scan the columns in the second list */
	i = 0;

col_loop2:
	orig = ptr;
	ptr = dict_scan_col(cs, ptr, &success, referenced_table, columns + i,
			    heap, ref_column_names + i);
	i++;

	if (!success) {

		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, create_name);
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, orig);
		mutex_exit(&dict_foreign_err_mutex);
		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, orig);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, ",", &success);

	if (success) {
		goto col_loop2;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, ")", &success);

	if (!success || foreign->n_fields != i) {

		dict_foreign_report_syntax_err(
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s' near '%s'.  Referencing column count does not match referenced column count.\n",
			operation, create_name, start_of_latest_foreign, orig);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s' near '%s'.  Referencing column count %d does not match referenced column count %d.\n",
			operation, create_name, start_of_latest_foreign, orig, i, foreign->n_fields);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	n_on_deletes = 0;
	n_on_updates = 0;

scan_on_conditions:
	/* Loop here as long as we can find ON ... conditions */

	start_of_latest_set = ptr;
	ptr = dict_accept(cs, ptr, "ON", &success);

	if (!success) {

		goto try_find_index;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "DELETE", &success);

	if (!success) {
		orig = ptr;
		ptr = dict_accept(cs, ptr, "UPDATE", &success);

		if (!success) {

			dict_foreign_report_syntax_err(
				"%s table %s with foreign key constraint"
				" failed. Parse error in '%s'"
				" near '%s'.\n",
				operation, create_name, start_of_latest_foreign, start_of_latest_set);

			ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
				"%s table %s with foreign key constraint"
				" failed. Parse error in '%s'"
				" near '%s'.",
				operation, create_name, start_of_latest_foreign, start_of_latest_set);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}

		is_on_delete = FALSE;
		n_on_updates++;
	} else {
		is_on_delete = TRUE;
		n_on_deletes++;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "RESTRICT", &success);

	if (success) {
		goto scan_on_conditions;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "CASCADE", &success);

	if (success) {
		if (is_on_delete) {
			foreign->type |= DICT_FOREIGN_ON_DELETE_CASCADE;
		} else {
			foreign->type |= DICT_FOREIGN_ON_UPDATE_CASCADE;
		}

		goto scan_on_conditions;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "NO", &success);

	if (success) {
		orig = ptr;
		ptr = dict_accept(cs, ptr, "ACTION", &success);

		if (!success) {
			dict_foreign_report_syntax_err(
				"%s table %s with foreign key constraint"
				" failed. Parse error in '%s'"
				" near '%s'.\n",
				operation, create_name, start_of_latest_foreign, start_of_latest_set);

			ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
				"%s table %s with foreign key constraint"
				" failed. Parse error in '%s'"
				" near '%s'.",
				operation, create_name, start_of_latest_foreign, start_of_latest_set);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}

		if (is_on_delete) {
			foreign->type |= DICT_FOREIGN_ON_DELETE_NO_ACTION;
		} else {
			foreign->type |= DICT_FOREIGN_ON_UPDATE_NO_ACTION;
		}

		goto scan_on_conditions;
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "SET", &success);

	if (!success) {
		dict_foreign_report_syntax_err(
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, start_of_latest_set);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, start_of_latest_set);
		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	orig = ptr;
	ptr = dict_accept(cs, ptr, "NULL", &success);

	if (!success) {
		dict_foreign_report_syntax_err(
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.\n",
			operation, create_name, start_of_latest_foreign, start_of_latest_set);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. Parse error in '%s'"
			" near '%s'.",
			operation, create_name, start_of_latest_foreign, start_of_latest_set);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	for (j = 0; j < foreign->n_fields; j++) {
		if ((dict_index_get_nth_col(foreign->foreign_index, j)->prtype)
		    & DATA_NOT_NULL) {
			const dict_col_t*	col
				= dict_index_get_nth_col(foreign->foreign_index, j);
			const char* col_name = dict_table_get_col_name(foreign->foreign_index->table,
				dict_col_get_no(col));

			/* It is not sensible to define SET NULL
			if the column is not allowed to be NULL! */

			mutex_enter(&dict_foreign_err_mutex);
			dict_foreign_error_report_low(ef, create_name);
			fprintf(ef,
				"%s table %s with foreign key constraint"
				" failed. You have defined a SET NULL condition but column '%s' is defined as NOT NULL"
				" in '%s' near '%s'.\n",
				operation, create_name, col_name, start_of_latest_foreign, start_of_latest_set);
			mutex_exit(&dict_foreign_err_mutex);

			ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
				"%s table %s with foreign key constraint"
				" failed. You have defined a SET NULL condition but column '%s' is defined as NOT NULL"
				" in '%s' near '%s'.",
				operation, create_name, col_name, start_of_latest_foreign, start_of_latest_set);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}
	}

	if (is_on_delete) {
		foreign->type |= DICT_FOREIGN_ON_DELETE_SET_NULL;
	} else {
		foreign->type |= DICT_FOREIGN_ON_UPDATE_SET_NULL;
	}

	goto scan_on_conditions;

try_find_index:
	if (n_on_deletes > 1 || n_on_updates > 1) {
		/* It is an error to define more than 1 action */

		mutex_enter(&dict_foreign_err_mutex);
		dict_foreign_error_report_low(ef, create_name);
		fprintf(ef,
			"%s table %s with foreign key constraint"
			" failed. You have more than one on delete or on update clause"
			" in '%s' near '%s'.\n",
			operation, create_name, start_of_latest_foreign, start_of_latest_set);
		mutex_exit(&dict_foreign_err_mutex);

		ib_push_warning(trx, DB_CANNOT_ADD_CONSTRAINT,
			"%s table %s with foreign key constraint"
			" failed. You have more than one on delete or on update clause"
			" in '%s' near '%s'.",
			operation, create_name, start_of_latest_foreign, start_of_latest_set);

		dict_foreign_free(foreign);

		return(DB_CANNOT_ADD_CONSTRAINT);
	}

	/* Try to find an index which contains the columns as the first fields
	and in the right order, and the types are the same as in
	foreign->foreign_index */

	if (referenced_table) {
		index = dict_foreign_find_index(referenced_table, NULL,
						ref_column_names, i,
						foreign->foreign_index,
			TRUE, FALSE, &index_error, &err_col, &err_index);

		if (!index) {
			mutex_enter(&dict_foreign_err_mutex);
			dict_foreign_error_report_low(ef, create_name);
			fprintf(ef, "%s:\n"
				"Cannot find an index in the"
				" referenced table where the\n"
				"referenced columns appear as the"
				" first columns, or column types\n"
				"in the table and the referenced table"
				" do not match for constraint.\n"
				"Note that the internal storage type of"
				" ENUM and SET changed in\n"
				"tables created with >= InnoDB-4.1.12,"
				" and such columns in old tables\n"
				"cannot be referenced by such columns"
				" in new tables.\n%s\n",
				start_of_latest_foreign,
				FOREIGN_KEY_CONSTRAINTS_MSG);

			dict_foreign_push_index_error(trx, operation, create_name, start_of_latest_foreign,
				column_names, index_error, err_col, err_index, referenced_table, ef);

			mutex_exit(&dict_foreign_err_mutex);

			return(DB_CANNOT_ADD_CONSTRAINT);
		}
	} else {
		ut_a(trx->check_foreigns == FALSE);
		index = NULL;
	}

	foreign->referenced_index = index;
	foreign->referenced_table = referenced_table;

	foreign->referenced_table_name = mem_heap_strdup(
		foreign->heap, referenced_table_name);
	dict_mem_referenced_table_name_lookup_set(foreign, TRUE);

	foreign->referenced_col_names = static_cast<const char**>(
		mem_heap_alloc(foreign->heap, i * sizeof(void*)));

	for (i = 0; i < foreign->n_fields; i++) {
		foreign->referenced_col_names[i]
			= mem_heap_strdup(foreign->heap, ref_column_names[i]);
	}

	goto loop;
}

/** Scans a table create SQL string and adds to the data dictionary
the foreign key constraints declared in the string. This function
should be called after the indexes for a table have been created.
Each foreign key constraint must be accompanied with indexes in
bot participating tables. The indexes are allowed to contain more
fields than mentioned in the constraint.

@param[in]	trx		transaction
@param[in]	sql_string	table create statement where
				foreign keys are declared like:
				FOREIGN KEY (a, b) REFERENCES table2(c, d),
				table2 can be written also with the database
				name before it: test.table2; the default
				database id the database of parameter name
@param[in]	sql_length	length of sql_string
@param[in]	name		table full name in normalized form
@param[in]	reject_fks	if TRUE, fail with error code
				DB_CANNOT_ADD_CONSTRAINT if any
				foreign keys are found.
@return error code or DB_SUCCESS */
dberr_t
dict_create_foreign_constraints(
	trx_t*			trx,
	const char*		sql_string,
	size_t			sql_length,
	const char*		name,
	ibool			reject_fks)
{
	char*		str;
	dberr_t		err;
	mem_heap_t*	heap;

	ut_a(trx);
	ut_a(trx->mysql_thd);

	str = dict_strip_comments(sql_string, sql_length);
	heap = mem_heap_create(10000);

	err = dict_create_foreign_constraints_low(
		trx, heap, thd_charset(trx->mysql_thd),
		str, name, reject_fks);

	mem_heap_free(heap);
	ut_free(str);

	return(err);
}

/**********************************************************************//**
Parses the CONSTRAINT id's to be dropped in an ALTER TABLE statement.
@return DB_SUCCESS or DB_CANNOT_DROP_CONSTRAINT if syntax error or the
constraint id does not match */
dberr_t
dict_foreign_parse_drop_constraints(
/*================================*/
	mem_heap_t*	heap,			/*!< in: heap from which we can
						allocate memory */
	trx_t*		trx,			/*!< in: transaction */
	dict_table_t*	table,			/*!< in: table */
	ulint*		n,			/*!< out: number of constraints
						to drop */
	const char***	constraints_to_drop)	/*!< out: id's of the
						constraints to drop */
{
	ibool			success;
	char*			str;
	size_t			len;
	const char*		ptr;
	const char*		ptr1;
	const char*		id;
	CHARSET_INFO*		cs;

	ut_a(trx->mysql_thd);

	cs = thd_charset(trx->mysql_thd);

	*n = 0;

	*constraints_to_drop = static_cast<const char**>(
		mem_heap_alloc(heap, 1000 * sizeof(char*)));

	ptr = innobase_get_stmt_unsafe(trx->mysql_thd, &len);

	str = dict_strip_comments(ptr, len);

	ptr = str;

	ut_ad(mutex_own(&dict_sys->mutex));
loop:
	ptr = dict_scan_to(ptr, "DROP");

	if (*ptr == '\0') {
		ut_free(str);

		return(DB_SUCCESS);
	}

	ptr = dict_accept(cs, ptr, "DROP", &success);

	if (!my_isspace(cs, *ptr)) {

		goto loop;
	}

	ptr = dict_accept(cs, ptr, "FOREIGN", &success);

	if (!success || !my_isspace(cs, *ptr)) {

		goto loop;
	}

	ptr = dict_accept(cs, ptr, "KEY", &success);

	if (!success) {

		goto syntax_error;
	}

	ptr1 = dict_accept(cs, ptr, "IF", &success);

	if (success && my_isspace(cs, *ptr1)) {
		ptr1 = dict_accept(cs, ptr1, "EXISTS", &success);
		if (success) {
			ptr = ptr1;
		}
	}

	ptr = dict_scan_id(cs, ptr, heap, &id, FALSE, TRUE);

	if (id == NULL) {

		goto syntax_error;
	}

	ut_a(*n < 1000);
	(*constraints_to_drop)[*n] = id;
	(*n)++;

	if (std::find_if(table->foreign_set.begin(),
			 table->foreign_set.end(),
			 dict_foreign_matches_id(id))
	    == table->foreign_set.end()) {

		if (!srv_read_only_mode) {
			FILE*	ef = dict_foreign_err_file;

			mutex_enter(&dict_foreign_err_mutex);
			rewind(ef);
			ut_print_timestamp(ef);
			fputs(" Error in dropping of a foreign key"
			      " constraint of table ", ef);
			ut_print_name(ef, NULL, table->name.m_name);
			fprintf(ef, ",\nin SQL command\n%s"
				"\nCannot find a constraint with the"
				" given id %s.\n", str, id);
			mutex_exit(&dict_foreign_err_mutex);
		}

		ut_free(str);

		return(DB_CANNOT_DROP_CONSTRAINT);
	}

	goto loop;

syntax_error:
	if (!srv_read_only_mode) {
		FILE*	ef = dict_foreign_err_file;

		mutex_enter(&dict_foreign_err_mutex);
		rewind(ef);
		ut_print_timestamp(ef);
		fputs(" Syntax error in dropping of a"
		      " foreign key constraint of table ", ef);
		ut_print_name(ef, NULL, table->name.m_name);
		fprintf(ef, ",\n"
			"close to:\n%s\n in SQL command\n%s\n", ptr, str);
		mutex_exit(&dict_foreign_err_mutex);
	}

	ut_free(str);

	return(DB_CANNOT_DROP_CONSTRAINT);
}

/*==================== END OF FOREIGN KEY PROCESSING ====================*/

/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
Assumes that dict_sys->mutex is already being held.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache_low(
/*===========================*/
	index_id_t	index_id)	/*!< in: index id */
{
	ut_ad(mutex_own(&dict_sys->mutex));

	return(dict_index_find_on_id_low(index_id));
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/**********************************************************************//**
Returns an index object if it is found in the dictionary cache.
@return index, NULL if not found */
dict_index_t*
dict_index_get_if_in_cache(
/*=======================*/
	index_id_t	index_id)	/*!< in: index id */
{
	dict_index_t*	index;

	if (dict_sys == NULL) {
		return(NULL);
	}

	mutex_enter(&dict_sys->mutex);

	index = dict_index_get_if_in_cache_low(index_id);

	mutex_exit(&dict_sys->mutex);

	return(index);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

#ifdef UNIV_DEBUG
/**********************************************************************//**
Checks that a tuple has n_fields_cmp value in a sensible range, so that
no comparison can occur with the page number field in a node pointer.
@return TRUE if ok */
ibool
dict_index_check_search_tuple(
/*==========================*/
	const dict_index_t*	index,	/*!< in: index tree */
	const dtuple_t*		tuple)	/*!< in: tuple used in a search */
{
	ut_ad(dtuple_get_n_fields_cmp(tuple)
	      <= dict_index_get_n_unique_in_tree(index));
	return(TRUE);
}
#endif /* UNIV_DEBUG */

/**********************************************************************//**
Builds a node pointer out of a physical record and a page number.
@return own: node pointer */
dtuple_t*
dict_index_build_node_ptr(
/*======================*/
	const dict_index_t*	index,	/*!< in: index */
	const rec_t*		rec,	/*!< in: record for which to build node
					pointer */
	ulint			page_no,/*!< in: page number to put in node
					pointer */
	mem_heap_t*		heap,	/*!< in: memory heap where pointer
					created */
	ulint			level)	/*!< in: level of rec in tree:
					0 means leaf level */
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;
	ulint		n_unique;

	if (dict_index_is_ibuf(index)) {
		/* In a universal index tree, we take the whole record as
		the node pointer if the record is on the leaf level,
		on non-leaf levels we remove the last field, which
		contains the page number of the child page */

		ut_a(!dict_table_is_comp(index->table));
		n_unique = rec_get_n_fields_old(rec);

		if (level > 0) {
			ut_a(n_unique > 1);
			n_unique--;
		}
	} else {
		n_unique = dict_index_get_n_unique_in_tree_nonleaf(index);
	}

	tuple = dtuple_create(heap, n_unique + 1);

	/* When searching in the tree for the node pointer, we must not do
	comparison on the last field, the page number field, as on upper
	levels in the tree there may be identical node pointers with a
	different page number; therefore, we set the n_fields_cmp to one
	less: */

	dtuple_set_n_fields_cmp(tuple, n_unique);

	dict_index_copy_types(tuple, index, n_unique);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));

	mach_write_to_4(buf, page_no);

	field = dtuple_get_nth_field(tuple, n_unique);
	dfield_set_data(field, buf, 4);

	dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);

	rec_copy_prefix_to_dtuple(tuple, rec, index, !level, n_unique, heap);
	dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple)
			     | REC_STATUS_NODE_PTR);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/**********************************************************************//**
Copies an initial segment of a physical record, long enough to specify an
index entry uniquely.
@return pointer to the prefix record */
rec_t*
dict_index_copy_rec_order_prefix(
/*=============================*/
	const dict_index_t*	index,	/*!< in: index */
	const rec_t*		rec,	/*!< in: record for which to
					copy prefix */
	ulint*			n_fields,/*!< out: number of fields copied */
	byte**			buf,	/*!< in/out: memory buffer for the
					copied prefix, or NULL */
	ulint*			buf_size)/*!< in/out: buffer size */
{
	ulint		n;

	UNIV_PREFETCH_R(rec);

	if (dict_index_is_ibuf(index)) {
		ut_a(!dict_table_is_comp(index->table));
		n = rec_get_n_fields_old(rec);
	} else {
		if (page_rec_is_leaf(rec)) {
			n = dict_index_get_n_unique_in_tree(index);
		} else {
			n = dict_index_get_n_unique_in_tree_nonleaf(index);
			/* For internal node of R-tree, since we need to
			compare the page no field, so, we need to copy this
			field as well. */
			if (dict_index_is_spatial(index)) {
				n++;
			}
		}
	}

	*n_fields = n;
	return(rec_copy_prefix_to_buf(rec, index, n, buf, buf_size));
}

/** Convert a physical record into a search tuple.
@param[in]	rec		index record (not necessarily in an index page)
@param[in]	index		index
@param[in]	leaf		whether rec is in a leaf page
@param[in]	n_fields	number of data fields
@param[in,out]	heap		memory heap for allocation
@return own: data tuple */
dtuple_t*
dict_index_build_data_tuple_func(
	const rec_t*		rec,
	const dict_index_t*	index,
#ifdef UNIV_DEBUG
	bool			leaf,
#endif /* UNIV_DEBUG */
	ulint			n_fields,
	mem_heap_t*		heap)
{
	dtuple_t*	tuple;

	ut_ad(dict_table_is_comp(index->table)
	      || n_fields <= rec_get_n_fields_old(rec));

	tuple = dtuple_create(heap, n_fields);

	dict_index_copy_types(tuple, index, n_fields);

	rec_copy_prefix_to_dtuple(tuple, rec, index, leaf, n_fields, heap);

	ut_ad(dtuple_check_typed(tuple));

	return(tuple);
}

/*********************************************************************//**
Calculates the minimum record length in an index. */
ulint
dict_index_calc_min_rec_len(
/*========================*/
	const dict_index_t*	index)	/*!< in: index */
{
	ulint	sum	= 0;
	ulint	i;
	ulint	comp	= dict_table_is_comp(index->table);

	if (comp) {
		ulint nullable = 0;
		sum = REC_N_NEW_EXTRA_BYTES;
		for (i = 0; i < dict_index_get_n_fields(index); i++) {
			const dict_col_t*	col
				= dict_index_get_nth_col(index, i);
			ulint	size = dict_col_get_fixed_size(col, comp);
			sum += size;
			if (!size) {
				size = col->len;
				sum += size < 128 ? 1 : 2;
			}
			if (!(col->prtype & DATA_NOT_NULL)) {
				nullable++;
			}
		}

		/* round the NULL flags up to full bytes */
		sum += UT_BITS_IN_BYTES(nullable);

		return(sum);
	}

	for (i = 0; i < dict_index_get_n_fields(index); i++) {
		sum += dict_col_get_fixed_size(
			dict_index_get_nth_col(index, i), comp);
	}

	if (sum > 127) {
		sum += 2 * dict_index_get_n_fields(index);
	} else {
		sum += dict_index_get_n_fields(index);
	}

	sum += REC_N_OLD_EXTRA_BYTES;

	return(sum);
}

/**********************************************************************//**
Outputs info on a foreign key of a table in a format suitable for
CREATE TABLE. */
std::string
dict_print_info_on_foreign_key_in_create_format(
/*============================================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	ibool		add_newline)	/*!< in: whether to add a newline */
{
	const char*	stripped_id;
	ulint	i;
	std::string	str;

	if (strchr(foreign->id, '/')) {
		/* Strip the preceding database name from the constraint id */
		stripped_id = foreign->id + 1
			+ dict_get_db_name_len(foreign->id);
	} else {
		stripped_id = foreign->id;
	}

	str.append(",");

	if (add_newline) {
		/* SHOW CREATE TABLE wants constraints each printed nicely
		on its own line, while error messages want no newlines
		inserted. */
		str.append("\n ");
	}

	str.append(" CONSTRAINT ");

	str.append(innobase_quote_identifier(trx, stripped_id));
	str.append(" FOREIGN KEY (");

	for (i = 0;;) {
		str.append(innobase_quote_identifier(trx, foreign->foreign_col_names[i]));

		if (++i < foreign->n_fields) {
			str.append(", ");
		} else {
			break;
		}
	}

	str.append(") REFERENCES ");

	if (dict_tables_have_same_db(foreign->foreign_table_name_lookup,
				     foreign->referenced_table_name_lookup)) {
		/* Do not print the database name of the referenced table */
		str.append(ut_get_name(trx,
			      dict_remove_db_name(
				      foreign->referenced_table_name)));
	} else {
		str.append(ut_get_name(trx,
				foreign->referenced_table_name));
	}

	str.append(" (");

	for (i = 0;;) {
		str.append(innobase_quote_identifier(trx,
				foreign->referenced_col_names[i]));

		if (++i < foreign->n_fields) {
			str.append(", ");
		} else {
			break;
		}
	}

	str.append(")");

	if (foreign->type & DICT_FOREIGN_ON_DELETE_CASCADE) {
		str.append(" ON DELETE CASCADE");
	}

	if (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL) {
		str.append(" ON DELETE SET NULL");
	}

	if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
		str.append(" ON DELETE NO ACTION");
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
		str.append(" ON UPDATE CASCADE");
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
		str.append(" ON UPDATE SET NULL");
	}

	if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
		str.append(" ON UPDATE NO ACTION");
	}

	return str;
}

/**********************************************************************//**
Outputs info on foreign keys of a table. */
std::string
dict_print_info_on_foreign_keys(
/*============================*/
	ibool		create_table_format, /*!< in: if TRUE then print in
				a format suitable to be inserted into
				a CREATE TABLE, otherwise in the format
				of SHOW TABLE STATUS */
	trx_t*		trx,	/*!< in: transaction */
	dict_table_t*	table)	/*!< in: table */
{
	dict_foreign_t*	foreign;
	std::string 	str;

	mutex_enter(&dict_sys->mutex);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (create_table_format) {
			str.append(
				dict_print_info_on_foreign_key_in_create_format(
					trx, foreign, TRUE));
		} else {
			ulint	i;
			str.append("; (");

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					str.append(" ");
				}

				str.append(innobase_quote_identifier(trx,
						foreign->foreign_col_names[i]));
			}

			str.append(") REFER ");
			str.append(ut_get_name(trx,
					foreign->referenced_table_name));
			str.append(")");

			for (i = 0; i < foreign->n_fields; i++) {
				if (i) {
					str.append(" ");
				}
				str.append(innobase_quote_identifier(
						trx,
						foreign->referenced_col_names[i]));
			}

			str.append(")");

			if (foreign->type == DICT_FOREIGN_ON_DELETE_CASCADE) {
				str.append(" ON DELETE CASCADE");
			}

			if (foreign->type == DICT_FOREIGN_ON_DELETE_SET_NULL) {
				str.append(" ON DELETE SET NULL");
			}

			if (foreign->type & DICT_FOREIGN_ON_DELETE_NO_ACTION) {
				str.append(" ON DELETE NO ACTION");
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE) {
				str.append(" ON UPDATE CASCADE");
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL) {
				str.append(" ON UPDATE SET NULL");
			}

			if (foreign->type & DICT_FOREIGN_ON_UPDATE_NO_ACTION) {
				str.append(" ON UPDATE NO ACTION");
			}
		}
	}

	mutex_exit(&dict_sys->mutex);
	return str;
}

/** Given a space_id of a file-per-table tablespace, search the
dict_sys->table_LRU list and return the dict_table_t* pointer for it.
@param	space_id	Tablespace ID
@return table if found, NULL if not */
static
dict_table_t*
dict_find_single_table_by_space(
	ulint	space_id)
{
	dict_table_t*	table;
	ulint		num_item;
	ulint		count = 0;

	ut_ad(space_id > 0);

	if (dict_sys == NULL) {
		/* This could happen when it's in redo processing. */
		return(NULL);
	}

	table = UT_LIST_GET_FIRST(dict_sys->table_LRU);
	num_item =  UT_LIST_GET_LEN(dict_sys->table_LRU);

	/* This function intentionally does not acquire mutex as it is used
	by error handling code in deep call stack as last means to avoid
	killing the server, so it worth to risk some consequences for
	the action. */
	while (table && count < num_item) {
		if (table->space == space_id) {
			if (dict_table_is_file_per_table(table)) {
				return(table);
			}
			return(NULL);
		}

		table = UT_LIST_GET_NEXT(table_LRU, table);
		count++;
	}

	return(NULL);
}

/**********************************************************************//**
Flags a table with specified space_id corrupted in the data dictionary
cache
@return TRUE if successful */
ibool
dict_set_corrupted_by_space(
/*========================*/
	ulint	space_id)		/*!< in: space ID */
{
	dict_table_t*   table;

	table = dict_find_single_table_by_space(space_id);

	if (!table) {
		return(FALSE);
	}

	/* mark the table->corrupted bit only, since the caller
	could be too deep in the stack for SYS_INDEXES update */
	table->corrupted = true;
	table->file_unreadable = true;

	return(TRUE);
}


/** Flag a table with specified space_id encrypted in the data dictionary
cache
@param[in]	space_id	Tablespace id */
UNIV_INTERN
void
dict_set_encrypted_by_space(ulint	space_id)
{
	dict_table_t*   table;

	table = dict_find_single_table_by_space(space_id);

	if (table) {
		table->file_unreadable = true;
	}
}

/**********************************************************************//**
Flags an index corrupted both in the data dictionary cache
and in the SYS_INDEXES */
void
dict_set_corrupted(
/*===============*/
	dict_index_t*	index,	/*!< in/out: index */
	trx_t*		trx,	/*!< in/out: transaction */
	const char*	ctx)	/*!< in: context */
{
	mem_heap_t*	heap;
	mtr_t		mtr;
	dict_index_t*	sys_index;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	const char*	status;
	btr_cur_t	cursor;
	bool		locked	= RW_X_LATCH == trx->dict_operation_lock_mode;

	if (!locked) {
		row_mysql_lock_data_dictionary(trx);
	}

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(!dict_table_is_comp(dict_sys->sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys->sys_indexes));
	ut_ad(!sync_check_iterate(dict_sync_check()));

	/* Mark the table as corrupted only if the clustered index
	is corrupted */
	if (dict_index_is_clust(index)) {
		index->table->corrupted = TRUE;
	}

	if (index->type & DICT_CORRUPT) {
		/* The index was already flagged corrupted. */
		ut_ad(!dict_index_is_clust(index) || index->table->corrupted);
		goto func_exit;
	}

	/* If this is read only mode, do not update SYS_INDEXES, just
	mark it as corrupted in memory */
	if (srv_read_only_mode) {
		index->type |= DICT_CORRUPT;
		goto func_exit;
	}

	heap = mem_heap_create(sizeof(dtuple_t) + 2 * (sizeof(dfield_t)
			       + sizeof(que_fork_t) + sizeof(upd_node_t)
			       + sizeof(upd_t) + 12));
	mtr_start(&mtr);
	index->type |= DICT_CORRUPT;

	sys_index = UT_LIST_GET_FIRST(dict_sys->sys_indexes->indexes);

	/* Find the index row in SYS_INDEXES */
	tuple = dtuple_create(heap, 2);

	dfield = dtuple_get_nth_field(tuple, 0);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->table->id);
	dfield_set_data(dfield, buf, 8);

	dfield = dtuple_get_nth_field(tuple, 1);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);
	dfield_set_data(dfield, buf, 8);

	dict_index_copy_types(tuple, sys_index, 2);

	btr_cur_search_to_nth_level(sys_index, 0, tuple, PAGE_CUR_LE,
				    BTR_MODIFY_LEAF,
				    &cursor, 0, __FILE__, __LINE__, &mtr, 0);

	if (cursor.low_match == dtuple_get_n_fields(tuple)) {
		/* UPDATE SYS_INDEXES SET TYPE=index->type
		WHERE TABLE_ID=index->table->id AND INDEX_ID=index->id */
		ulint	len;
		byte*	field	= rec_get_nth_field_old(
			btr_cur_get_rec(&cursor),
			DICT_FLD__SYS_INDEXES__TYPE, &len);
		if (len != 4) {
			goto fail;
		}
		mlog_write_ulint(field, index->type, MLOG_4BYTES, &mtr);
		status = "Flagged";
	} else {
fail:
		status = "Unable to flag";
	}

	mtr_commit(&mtr);
	mem_heap_empty(heap);
	ib::error() << status << " corruption of " << index->name
		<< " in table " << index->table->name << " in " << ctx;
	mem_heap_free(heap);

func_exit:
	if (!locked) {
		row_mysql_unlock_data_dictionary(trx);
	}
}

/** Flags an index corrupted in the data dictionary cache only. This
is used mostly to mark a corrupted index when index's own dictionary
is corrupted, and we force to load such index for repair purpose
@param[in,out]	index	index which is corrupted */
void
dict_set_corrupted_index_cache_only(
	dict_index_t*	index)
{
	ut_ad(index != NULL);
	ut_ad(index->table != NULL);
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(!dict_table_is_comp(dict_sys->sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys->sys_indexes));

	/* Mark the table as corrupted only if the clustered index
	is corrupted */
	if (dict_index_is_clust(index)) {
		index->table->corrupted = TRUE;
		index->table->file_unreadable = true;
	}

	index->type |= DICT_CORRUPT;
}

/** Sets merge_threshold in the SYS_INDEXES
@param[in,out]	index		index
@param[in]	merge_threshold	value to set */
void
dict_index_set_merge_threshold(
	dict_index_t*	index,
	ulint		merge_threshold)
{
	mem_heap_t*	heap;
	mtr_t		mtr;
	dict_index_t*	sys_index;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	btr_cur_t	cursor;

	ut_ad(index != NULL);
	ut_ad(!dict_table_is_comp(dict_sys->sys_tables));
	ut_ad(!dict_table_is_comp(dict_sys->sys_indexes));

	rw_lock_x_lock(&dict_operation_lock);
	mutex_enter(&(dict_sys->mutex));

	heap = mem_heap_create(sizeof(dtuple_t) + 2 * (sizeof(dfield_t)
			       + sizeof(que_fork_t) + sizeof(upd_node_t)
			       + sizeof(upd_t) + 12));

	mtr_start(&mtr);

	sys_index = UT_LIST_GET_FIRST(dict_sys->sys_indexes->indexes);

	/* Find the index row in SYS_INDEXES */
	tuple = dtuple_create(heap, 2);

	dfield = dtuple_get_nth_field(tuple, 0);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->table->id);
	dfield_set_data(dfield, buf, 8);

	dfield = dtuple_get_nth_field(tuple, 1);
	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);
	dfield_set_data(dfield, buf, 8);

	dict_index_copy_types(tuple, sys_index, 2);

	btr_cur_search_to_nth_level(sys_index, 0, tuple, PAGE_CUR_GE,
				    BTR_MODIFY_LEAF,
				    &cursor, 0, __FILE__, __LINE__, &mtr, 0);

	if (cursor.up_match == dtuple_get_n_fields(tuple)
	    && rec_get_n_fields_old(btr_cur_get_rec(&cursor))
	       == DICT_NUM_FIELDS__SYS_INDEXES) {
		ulint	len;
		byte*	field	= rec_get_nth_field_old(
			btr_cur_get_rec(&cursor),
			DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD, &len);

		ut_ad(len == 4);

		if (len == 4) {
			mlog_write_ulint(field, merge_threshold,
					 MLOG_4BYTES, &mtr);
		}
	}

	mtr_commit(&mtr);
	mem_heap_free(heap);

	mutex_exit(&(dict_sys->mutex));
	rw_lock_x_unlock(&dict_operation_lock);
}

#ifdef UNIV_DEBUG
/** Sets merge_threshold for all indexes in the list of tables
@param[in]	list	pointer to the list of tables */
inline
void
dict_set_merge_threshold_list_debug(
	UT_LIST_BASE_NODE_T(dict_table_t)*	list,
	uint					merge_threshold_all)
{
	for (dict_table_t* table = UT_LIST_GET_FIRST(*list);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {
		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {
			rw_lock_x_lock(dict_index_get_lock(index));
			index->merge_threshold = merge_threshold_all;
			rw_lock_x_unlock(dict_index_get_lock(index));
		}
	}
}

/** Sets merge_threshold for all indexes in dictionary cache for debug.
@param[in]	merge_threshold_all	value to set for all indexes */
void
dict_set_merge_threshold_all_debug(
	uint	merge_threshold_all)
{
	mutex_enter(&dict_sys->mutex);

	dict_set_merge_threshold_list_debug(
		&dict_sys->table_LRU, merge_threshold_all);
	dict_set_merge_threshold_list_debug(
		&dict_sys->table_non_LRU, merge_threshold_all);

	mutex_exit(&dict_sys->mutex);
}

#endif /* UNIV_DEBUG */

/** Initialize dict_ind_redundant. */
void
dict_ind_init()
{
	dict_table_t*		table;

	/* create dummy table and index for REDUNDANT infimum and supremum */
	table = dict_mem_table_create("SYS_DUMMY1", DICT_HDR_SPACE, 1, 0, 0, 0);
	dict_mem_table_add_col(table, NULL, NULL, DATA_CHAR,
			       DATA_ENGLISH | DATA_NOT_NULL, 8);

	dict_ind_redundant = dict_mem_index_create("SYS_DUMMY1", "SYS_DUMMY1",
						   DICT_HDR_SPACE, 0, 1);
	dict_index_add_col(dict_ind_redundant, table,
			   dict_table_get_nth_col(table, 0), 0);
	dict_ind_redundant->table = table;
	/* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
	dict_ind_redundant->cached = TRUE;
}

/** Free dict_ind_redundant. */
void
dict_ind_free()
{
	dict_table_t*	table = dict_ind_redundant->table;
	dict_mem_index_free(dict_ind_redundant);
	dict_ind_redundant = NULL;
	dict_mem_table_free(table);
}

/** Get an index by name.
@param[in]	table		the table where to look for the index
@param[in]	name		the index name to look for
@return index, NULL if does not exist */
dict_index_t*
dict_table_get_index_on_name(dict_table_t* table, const char* name)
{
	dict_index_t*	index;

	index = dict_table_get_first_index(table);

	while (index != NULL) {
		if (index->is_committed() && !strcmp(index->name, name)) {
			return(index);
		}

		index = dict_table_get_next_index(index);
	}

	return(NULL);
}

/**********************************************************************//**
Replace the index passed in with another equivalent index in the
foreign key lists of the table.
@return whether all replacements were found */
bool
dict_foreign_replace_index(
/*=======================*/
	dict_table_t*		table,  /*!< in/out: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const dict_index_t*	index)	/*!< in: index to be replaced */
{
	bool		found	= true;
	dict_foreign_t*	foreign;

	ut_ad(index->to_be_dropped);
	ut_ad(index->table == table);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;
		if (foreign->foreign_index == index) {
			ut_ad(foreign->foreign_table == index->table);

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->foreign_table, col_names,
				foreign->foreign_col_names,
				foreign->n_fields, index,
				/*check_charsets=*/TRUE, /*check_null=*/FALSE,
				NULL, NULL, NULL);
			if (new_index) {
				ut_ad(new_index->table == index->table);
				ut_ad(!new_index->to_be_dropped);
			} else {
				found = false;
			}

			foreign->foreign_index = new_index;
		}
	}

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;
		if (foreign->referenced_index == index) {
			ut_ad(foreign->referenced_table == index->table);

			dict_index_t* new_index = dict_foreign_find_index(
				foreign->referenced_table, NULL,
				foreign->referenced_col_names,
				foreign->n_fields, index,
				/*check_charsets=*/TRUE, /*check_null=*/FALSE,
				NULL, NULL, NULL);
			/* There must exist an alternative index,
			since this must have been checked earlier. */
			if (new_index) {
				ut_ad(new_index->table == index->table);
				ut_ad(!new_index->to_be_dropped);
			} else {
				found = false;
			}

			foreign->referenced_index = new_index;
		}
	}

	return(found);
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Check for duplicate index entries in a table [using the index name] */
void
dict_table_check_for_dup_indexes(
/*=============================*/
	const dict_table_t*	table,	/*!< in: Check for dup indexes
					in this table */
	enum check_name		check)	/*!< in: whether and when to allow
					temporary index names */
{
	/* Check for duplicates, ignoring indexes that are marked
	as to be dropped */

	const dict_index_t*	index1;
	const dict_index_t*	index2;

	ut_ad(mutex_own(&dict_sys->mutex));

	/* The primary index _must_ exist */
	ut_a(UT_LIST_GET_LEN(table->indexes) > 0);

	index1 = UT_LIST_GET_FIRST(table->indexes);

	do {
		if (!index1->is_committed()) {
			ut_a(!dict_index_is_clust(index1));

			switch (check) {
			case CHECK_ALL_COMPLETE:
				ut_error;
			case CHECK_ABORTED_OK:
				switch (dict_index_get_online_status(index1)) {
				case ONLINE_INDEX_COMPLETE:
				case ONLINE_INDEX_CREATION:
					ut_error;
					break;
				case ONLINE_INDEX_ABORTED:
				case ONLINE_INDEX_ABORTED_DROPPED:
					break;
				}
				/* fall through */
			case CHECK_PARTIAL_OK:
				break;
			}
		}

		for (index2 = UT_LIST_GET_NEXT(indexes, index1);
		     index2 != NULL;
		     index2 = UT_LIST_GET_NEXT(indexes, index2)) {
			ut_ad(index1->is_committed()
			      != index2->is_committed()
			      || strcmp(index1->name, index2->name) != 0);
		}

		index1 = UT_LIST_GET_NEXT(indexes, index1);
	} while (index1);
}
#endif /* UNIV_DEBUG */

/** Auxiliary macro used inside dict_table_schema_check(). */
#define CREATE_TYPES_NAMES() \
	dtype_sql_name((unsigned) req_schema->columns[i].mtype, \
		       (unsigned) req_schema->columns[i].prtype_mask, \
		       (unsigned) req_schema->columns[i].len, \
		       req_type, sizeof(req_type)); \
	dtype_sql_name(table->cols[j].mtype, \
		       table->cols[j].prtype, \
		       table->cols[j].len, \
		       actual_type, sizeof(actual_type))

/*********************************************************************//**
Checks whether a table exists and whether it has the given structure.
The table must have the same number of columns with the same names and
types. The order of the columns does not matter.
The caller must own the dictionary mutex.
dict_table_schema_check() @{
@return DB_SUCCESS if the table exists and contains the necessary columns */
dberr_t
dict_table_schema_check(
/*====================*/
	dict_table_schema_t*	req_schema,	/*!< in/out: required table
						schema */
	char*			errstr,		/*!< out: human readable error
						message if != DB_SUCCESS is
						returned */
	size_t			errstr_sz)	/*!< in: errstr size */
{
	char		buf[MAX_FULL_NAME_LEN];
	char		req_type[64];
	char		actual_type[64];
	dict_table_t*	table;
	ulint		i;

	ut_ad(mutex_own(&dict_sys->mutex));

	table = dict_table_get_low(req_schema->table_name);

	if (table == NULL) {
		bool should_print=true;
		/* no such table */

		if (innobase_strcasecmp(req_schema->table_name, "mysql/innodb_table_stats") == 0) {
			if (innodb_table_stats_not_found_reported == false) {
				innodb_table_stats_not_found = true;
				innodb_table_stats_not_found_reported = true;
			} else {
				should_print = false;
			}
		} else if (innobase_strcasecmp(req_schema->table_name, "mysql/innodb_index_stats") == 0 ) {
			if (innodb_index_stats_not_found_reported == false) {
				innodb_index_stats_not_found = true;
				innodb_index_stats_not_found_reported = true;
			} else {
				should_print = false;
			}
		}

		if (should_print) {
			snprintf(errstr, errstr_sz,
				"Table %s not found.",
				ut_format_name(req_schema->table_name,
					   buf, sizeof(buf)));
			return(DB_TABLE_NOT_FOUND);
		} else {
			return(DB_STATS_DO_NOT_EXIST);
		}
	}

	if (!table->is_readable() &&
	    fil_space_get(table->space) == NULL) {
		/* missing tablespace */

		snprintf(errstr, errstr_sz,
			    "Tablespace for table %s is missing.",
			    ut_format_name(req_schema->table_name,
					   buf, sizeof(buf)));

		return(DB_TABLE_NOT_FOUND);
	}

	if (ulint(table->n_def) - DATA_N_SYS_COLS != req_schema->n_cols) {
		/* the table has a different number of columns than required */
		snprintf(errstr, errstr_sz,
			 "%s has " ULINTPF " columns but should have "
			 ULINTPF ".",
			 ut_format_name(req_schema->table_name, buf,
					sizeof buf),
			 ulint(table->n_def) - DATA_N_SYS_COLS,
			 req_schema->n_cols);

		return(DB_ERROR);
	}

	/* For each column from req_schema->columns[] search
	whether it is present in table->cols[].
	The following algorithm is O(n_cols^2), but is optimized to
	be O(n_cols) if the columns are in the same order in both arrays. */

	for (i = 0; i < req_schema->n_cols; i++) {
		ulint	j = dict_table_has_column(
			table, req_schema->columns[i].name, i);

		if (j == table->n_def) {

			snprintf(errstr, errstr_sz,
				    "required column %s"
				    " not found in table %s.",
				    req_schema->columns[i].name,
				    ut_format_name(
					    req_schema->table_name,
					    buf, sizeof(buf)));

			return(DB_ERROR);
		}

		/* we found a column with the same name on j'th position,
		compare column types and flags */

		/* check length for exact match */
		if (req_schema->columns[i].len == table->cols[j].len) {
		} else if (!strcmp(req_schema->table_name, TABLE_STATS_NAME)
			   || !strcmp(req_schema->table_name,
				      INDEX_STATS_NAME)) {
			ut_ad(table->cols[j].len < req_schema->columns[i].len);
			ib::warn() << "Table " << req_schema->table_name
				   << " has length mismatch in the"
				   << " column name "
				   << req_schema->columns[i].name
				   << ".  Please run mysql_upgrade";
		} else {
			CREATE_TYPES_NAMES();

			snprintf(errstr, errstr_sz,
				    "Column %s in table %s is %s"
				    " but should be %s (length mismatch).",
				    req_schema->columns[i].name,
				    ut_format_name(req_schema->table_name,
						   buf, sizeof(buf)),
				    actual_type, req_type);

			return(DB_ERROR);
		}

		/*
                  check mtype for exact match.
                  This check is relaxed to allow use to use TIMESTAMP
                  (ie INT) for last_update instead of DATA_BINARY.
                  We have to test for both values as the innodb_table_stats
                  table may come from MySQL and have the old type.
                */
		if (req_schema->columns[i].mtype != table->cols[j].mtype &&
                    !(req_schema->columns[i].mtype == DATA_INT &&
                      table->cols[j].mtype == DATA_FIXBINARY))
                {
			CREATE_TYPES_NAMES();

			snprintf(errstr, errstr_sz,
				    "Column %s in table %s is %s"
				    " but should be %s (type mismatch).",
				    req_schema->columns[i].name,
				    ut_format_name(req_schema->table_name,
						   buf, sizeof(buf)),
				    actual_type, req_type);

			return(DB_ERROR);
		}

		/* check whether required prtype mask is set */
		if (req_schema->columns[i].prtype_mask != 0
		    && (table->cols[j].prtype
			& req_schema->columns[i].prtype_mask)
		       != req_schema->columns[i].prtype_mask) {

			CREATE_TYPES_NAMES();

			snprintf(errstr, errstr_sz,
				    "Column %s in table %s is %s"
				    " but should be %s (flags mismatch).",
				    req_schema->columns[i].name,
				    ut_format_name(req_schema->table_name,
						   buf, sizeof(buf)),
				    actual_type, req_type);

			return(DB_ERROR);
		}
	}

	if (req_schema->n_foreign != table->foreign_set.size()) {
		snprintf(
			errstr, errstr_sz,
			"Table %s has " ULINTPF " foreign key(s) pointing"
			" to other tables, but it must have " ULINTPF ".",
			ut_format_name(req_schema->table_name,
				       buf, sizeof(buf)),
			static_cast<ulint>(table->foreign_set.size()),
			req_schema->n_foreign);
		return(DB_ERROR);
	}

	if (req_schema->n_referenced != table->referenced_set.size()) {
		snprintf(
			errstr, errstr_sz,
			"There are " ULINTPF " foreign key(s) pointing to %s, "
			"but there must be " ULINTPF ".",
			static_cast<ulint>(table->referenced_set.size()),
			ut_format_name(req_schema->table_name,
				       buf, sizeof(buf)),
			req_schema->n_referenced);
		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}
/* @} */

/*********************************************************************//**
Converts a database and table name from filesystem encoding
(e.g. d@i1b/a@q1b@1Kc, same format as used in dict_table_t::name) in two
strings in UTF8 encoding (e.g. dцb and aюbØc). The output buffers must be
at least MAX_DB_UTF8_LEN and MAX_TABLE_UTF8_LEN bytes. */
void
dict_fs2utf8(
/*=========*/
	const char*	db_and_table,	/*!< in: database and table names,
					e.g. d@i1b/a@q1b@1Kc */
	char*		db_utf8,	/*!< out: database name, e.g. dцb */
	size_t		db_utf8_size,	/*!< in: dbname_utf8 size */
	char*		table_utf8,	/*!< out: table name, e.g. aюbØc */
	size_t		table_utf8_size)/*!< in: table_utf8 size */
{
	char	db[MAX_DATABASE_NAME_LEN + 1];
	ulint	db_len;
	uint	errors;

	db_len = dict_get_db_name_len(db_and_table);

	ut_a(db_len <= sizeof(db));

	memcpy(db, db_and_table, db_len);
	db[db_len] = '\0';

	strconvert(
		&my_charset_filename, db, uint(db_len), system_charset_info,
		db_utf8, uint(db_utf8_size), &errors);

	/* convert each # to @0023 in table name and store the result in buf */
	const char*	table = dict_remove_db_name(db_and_table);
	const char*	table_p;
	char		buf[MAX_TABLE_NAME_LEN * 5 + 1];
	char*		buf_p;
	for (table_p = table, buf_p = buf; table_p[0] != '\0'; table_p++) {
		if (table_p[0] != '#') {
			buf_p[0] = table_p[0];
			buf_p++;
		} else {
			buf_p[0] = '@';
			buf_p[1] = '0';
			buf_p[2] = '0';
			buf_p[3] = '2';
			buf_p[4] = '3';
			buf_p += 5;
		}
		ut_a((size_t) (buf_p - buf) < sizeof(buf));
	}
	buf_p[0] = '\0';

	errors = 0;
	strconvert(
		&my_charset_filename, buf, (uint) (buf_p - buf),
		system_charset_info,
		table_utf8, uint(table_utf8_size),
		&errors);

	if (errors != 0) {
		snprintf(table_utf8, table_utf8_size, "%s%s",
			    srv_mysql50_table_name_prefix, table);
	}
}

/** Resize the hash tables besed on the current buffer pool size. */
void
dict_resize()
{
	dict_table_t*	table;

	mutex_enter(&dict_sys->mutex);

	/* all table entries are in table_LRU and table_non_LRU lists */
	hash_table_free(dict_sys->table_hash);
	hash_table_free(dict_sys->table_id_hash);

	dict_sys->table_hash = hash_create(
		buf_pool_get_curr_size()
		/ (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE));

	dict_sys->table_id_hash = hash_create(
		buf_pool_get_curr_size()
		/ (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE));

	for (table = UT_LIST_GET_FIRST(dict_sys->table_LRU); table;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {
		ulint	fold = ut_fold_string(table->name.m_name);
		ulint	id_fold = ut_fold_ull(table->id);

		HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash,
			    fold, table);

		HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash,
			    id_fold, table);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys->table_non_LRU); table;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {
		ulint	fold = ut_fold_string(table->name.m_name);
		ulint	id_fold = ut_fold_ull(table->id);

		HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash,
			    fold, table);

		HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash,
			    id_fold, table);
	}

	mutex_exit(&dict_sys->mutex);
}

/**********************************************************************//**
Closes the data dictionary module. */
void
dict_close(void)
/*============*/
{
	ulint	i;

	/* Free the hash elements. We don't remove them from the table
	because we are going to destroy the table anyway. */
	for (i = 0; i < hash_get_n_cells(dict_sys->table_hash); i++) {
		dict_table_t*	table;

		table = static_cast<dict_table_t*>(
			HASH_GET_FIRST(dict_sys->table_hash, i));

		while (table) {
			dict_table_t*	prev_table = table;

			table = static_cast<dict_table_t*>(
				HASH_GET_NEXT(name_hash, prev_table));
			ut_ad(prev_table->magic_n == DICT_TABLE_MAGIC_N);
			/* Acquire only because it's a pre-condition. */
			mutex_enter(&dict_sys->mutex);

			dict_table_remove_from_cache(prev_table);

			mutex_exit(&dict_sys->mutex);
		}
	}

	hash_table_free(dict_sys->table_hash);

	/* The elements are the same instance as in dict_sys->table_hash,
	therefore we don't delete the individual elements. */
	hash_table_free(dict_sys->table_id_hash);

	mutex_free(&dict_sys->mutex);

	rw_lock_free(&dict_operation_lock);

	mutex_free(&dict_foreign_err_mutex);

	ut_free(dict_sys);

	dict_sys = NULL;
}

#ifdef UNIV_DEBUG
/**********************************************************************//**
Validate the dictionary table LRU list.
@return TRUE if valid */
static
ibool
dict_lru_validate(void)
/*===================*/
{
	dict_table_t*	table;

	ut_ad(mutex_own(&dict_sys->mutex));

	for (table = UT_LIST_GET_FIRST(dict_sys->table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(table->can_be_evicted);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys->table_non_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(!table->can_be_evicted);
	}

	return(TRUE);
}

/**********************************************************************//**
Check if a table exists in the dict table LRU list.
@return TRUE if table found in LRU list */
static
ibool
dict_lru_find_table(
/*================*/
	const dict_table_t*	find_table)	/*!< in: table to find */
{
	dict_table_t*		table;

	ut_ad(find_table != NULL);
	ut_ad(mutex_own(&dict_sys->mutex));

	for (table = UT_LIST_GET_FIRST(dict_sys->table_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(table->can_be_evicted);

		if (table == find_table) {
			return(TRUE);
		}
	}

	return(FALSE);
}

/**********************************************************************//**
Check if a table exists in the dict table non-LRU list.
@return TRUE if table found in non-LRU list */
static
ibool
dict_non_lru_find_table(
/*====================*/
	const dict_table_t*	find_table)	/*!< in: table to find */
{
	dict_table_t*		table;

	ut_ad(find_table != NULL);
	ut_ad(mutex_own(&dict_sys->mutex));

	for (table = UT_LIST_GET_FIRST(dict_sys->table_non_LRU);
	     table != NULL;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		ut_a(!table->can_be_evicted);

		if (table == find_table) {
			return(TRUE);
		}
	}

	return(FALSE);
}
#endif /* UNIV_DEBUG */
/*********************************************************************//**
Check an index to see whether its first fields are the columns in the array,
in the same order and is not marked for deletion and is not the same
as types_idx.
@return true if the index qualifies, otherwise false */
bool
dict_foreign_qualify_index(
/*=======================*/
	const dict_table_t*	table,	/*!< in: table */
	const char**		col_names,
					/*!< in: column names, or NULL
					to use table->col_names */
	const char**		columns,/*!< in: array of column names */
	ulint			n_cols,	/*!< in: number of columns */
	const dict_index_t*	index,	/*!< in: index to check */
	const dict_index_t*	types_idx,
					/*!< in: NULL or an index
					whose types the column types
					must match */
	bool			check_charsets,
					/*!< in: whether to check
					charsets.  only has an effect
					if types_idx != NULL */
	ulint			check_null,
					/*!< in: nonzero if none of
					the columns must be declared
					NOT NULL */
	fkerr_t*		error,	/*!< out: error code */
	ulint*			err_col_no,
					/*!< out: column number where
					error happened */
	dict_index_t**		err_index)
					/*!< out: index where error
					happened */
{
	if (dict_index_get_n_fields(index) < n_cols) {
		return(false);
	}

	if (index->type & (DICT_SPATIAL | DICT_FTS | DICT_CORRUPT)) {
		return false;
	}

	if (index->online_status >= ONLINE_INDEX_ABORTED) {
		return false;
	}

	for (ulint i = 0; i < n_cols; i++) {
		dict_field_t*	field;
		const char*	col_name;
		ulint		col_no;

		field = dict_index_get_nth_field(index, i);
		col_no = dict_col_get_no(field->col);

		if (field->prefix_len != 0) {
			/* We do not accept column prefix
			indexes here */
			if (error && err_col_no && err_index) {
				*error = FK_IS_PREFIX_INDEX;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}
			return(false);
		}

		if (check_null
		    && (field->col->prtype & DATA_NOT_NULL)) {
			if (error && err_col_no && err_index) {
				*error = FK_COL_NOT_NULL;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}
			return(false);
		}

		if (dict_col_is_virtual(field->col)) {
			col_name = "";
			for (ulint j = 0; j < table->n_v_def; j++) {
				col_name = dict_table_get_v_col_name(table, j);
				if (innobase_strcasecmp(field->name,col_name) == 0) {
					break;
				}
			}
		} else {
			col_name = col_names
				? col_names[col_no]
				: dict_table_get_col_name(table, col_no);
		}

		if (0 != innobase_strcasecmp(columns[i], col_name)) {
			return(false);
		}

		if (types_idx && !cmp_cols_are_equal(
			    dict_index_get_nth_col(index, i),
			    dict_index_get_nth_col(types_idx, i),
			    check_charsets)) {
			if (error && err_col_no && err_index) {
				*error = FK_COLS_NOT_EQUAL;
				*err_col_no = i;
				*err_index = (dict_index_t*)index;
			}

			return(false);
		}
	}

	return(true);
}

/*********************************************************************//**
Update the state of compression failure padding heuristics. This is
called whenever a compression operation succeeds or fails.
The caller must be holding info->mutex */
static
void
dict_index_zip_pad_update(
/*======================*/
	zip_pad_info_t*	info,	/*<! in/out: info to be updated */
	ulint	zip_threshold)	/*<! in: zip threshold value */
{
	ulint	total;
	ulint	fail_pct;

	ut_ad(info);

	total = info->success + info->failure;

	ut_ad(total > 0);

	if (zip_threshold == 0) {
		/* User has just disabled the padding. */
		return;
	}

	if (total < ZIP_PAD_ROUND_LEN) {
		/* We are in middle of a round. Do nothing. */
		return;
	}

	/* We are at a 'round' boundary. Reset the values but first
	calculate fail rate for our heuristic. */
	fail_pct = (info->failure * 100) / total;
	info->failure = 0;
	info->success = 0;

	if (fail_pct > zip_threshold) {
		/* Compression failures are more then user defined
		threshold. Increase the pad size to reduce chances of
		compression failures. */
		ut_ad(info->pad % ZIP_PAD_INCR == 0);

		/* Only do increment if it won't increase padding
		beyond max pad size. */
		if (info->pad + ZIP_PAD_INCR
		    < (UNIV_PAGE_SIZE * zip_pad_max) / 100) {
			/* Use atomics even though we have the mutex.
			This is to ensure that we are able to read
			info->pad atomically. */
			my_atomic_addlint(&info->pad, ZIP_PAD_INCR);

			MONITOR_INC(MONITOR_PAD_INCREMENTS);
		}

		info->n_rounds = 0;

	} else {
		/* Failure rate was OK. Another successful round
		completed. */
		++info->n_rounds;

		/* If enough successful rounds are completed with
		compression failure rate in control, decrease the
		padding. */
		if (info->n_rounds >= ZIP_PAD_SUCCESSFUL_ROUND_LIMIT
		    && info->pad > 0) {

			ut_ad(info->pad % ZIP_PAD_INCR == 0);
			/* Use atomics even though we have the mutex.
			This is to ensure that we are able to read
			info->pad atomically. */
			my_atomic_addlint(&info->pad, -ZIP_PAD_INCR);

			info->n_rounds = 0;

			MONITOR_INC(MONITOR_PAD_DECREMENTS);
		}
	}
}

/*********************************************************************//**
This function should be called whenever a page is successfully
compressed. Updates the compression padding information. */
void
dict_index_zip_success(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
{
	ulint zip_threshold = zip_failure_threshold_pct;
	if (!zip_threshold) {
		/* Disabled by user. */
		return;
	}

	dict_index_zip_pad_lock(index);
	++index->zip_pad.success;
	dict_index_zip_pad_update(&index->zip_pad, zip_threshold);
	dict_index_zip_pad_unlock(index);
}

/*********************************************************************//**
This function should be called whenever a page compression attempt
fails. Updates the compression padding information. */
void
dict_index_zip_failure(
/*===================*/
	dict_index_t*	index)	/*!< in/out: index to be updated. */
{
	ulint zip_threshold = zip_failure_threshold_pct;
	if (!zip_threshold) {
		/* Disabled by user. */
		return;
	}

	dict_index_zip_pad_lock(index);
	++index->zip_pad.failure;
	dict_index_zip_pad_update(&index->zip_pad, zip_threshold);
	dict_index_zip_pad_unlock(index);
}

/*********************************************************************//**
Return the optimal page size, for which page will likely compress.
@return page size beyond which page might not compress */
ulint
dict_index_zip_pad_optimal_page_size(
/*=================================*/
	dict_index_t*	index)	/*!< in: index for which page size
				is requested */
{
	ulint	pad;
	ulint	min_sz;
	ulint	sz;

	if (!zip_failure_threshold_pct) {
		/* Disabled by user. */
		return(UNIV_PAGE_SIZE);
	}

	pad = my_atomic_loadlint(&index->zip_pad.pad);

	ut_ad(pad < UNIV_PAGE_SIZE);
	sz = UNIV_PAGE_SIZE - pad;

	/* Min size allowed by user. */
	ut_ad(zip_pad_max < 100);
	min_sz = (UNIV_PAGE_SIZE * (100 - zip_pad_max)) / 100;

	return(ut_max(sz, min_sz));
}

/*************************************************************//**
Convert table flag to row format string.
@return row format name. */
const char*
dict_tf_to_row_format_string(
/*=========================*/
	ulint	table_flag)		/*!< in: row format setting */
{
	switch (dict_tf_get_rec_format(table_flag)) {
	case REC_FORMAT_REDUNDANT:
		return("ROW_TYPE_REDUNDANT");
	case REC_FORMAT_COMPACT:
		return("ROW_TYPE_COMPACT");
	case REC_FORMAT_COMPRESSED:
		return("ROW_TYPE_COMPRESSED");
	case REC_FORMAT_DYNAMIC:
		return("ROW_TYPE_DYNAMIC");
	}

	ut_error;
	return(0);
}

/** Calculate the used memory occupied by the data dictionary
table and index objects.
@return number of bytes occupied. */
UNIV_INTERN
ulint
dict_sys_get_size()
{
	/* No mutex; this is a very crude approximation anyway */
	ulint size = UT_LIST_GET_LEN(dict_sys->table_LRU)
		+ UT_LIST_GET_LEN(dict_sys->table_non_LRU);
	size *= sizeof(dict_table_t)
		+ sizeof(dict_index_t) * 2
		+ (sizeof(dict_col_t) + sizeof(dict_field_t)) * 10
		+ sizeof(dict_field_t) * 5 /* total number of key fields */
		+ 200; /* arbitrary, covering names and overhead */

	return size;
}

size_t
dict_table_t::get_overflow_field_local_len() const
{
	if (dict_table_get_format(this) < UNIV_FORMAT_B) {
		/* up to MySQL 5.1: store a 768-byte prefix locally */
		return BTR_EXTERN_FIELD_REF_SIZE
		       + DICT_ANTELOPE_MAX_INDEX_COL_LEN;
	}
	/* new-format table: do not store any BLOB prefix locally */
	return BTR_EXTERN_FIELD_REF_SIZE;
}

bool dict_table_t::is_stats_table() const
{
  return !strcmp(name.m_name, TABLE_STATS_NAME) ||
         !strcmp(name.m_name, INDEX_STATS_NAME);
}
