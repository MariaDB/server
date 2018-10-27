/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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
@file row/row0uins.cc
Fresh insert undo

Created 2/25/1997 Heikki Tuuri
*******************************************************/

#include "row0uins.h"
#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "trx0undo.h"
#include "trx0roll.h"
#include "btr0btr.h"
#include "mach0data.h"
#include "row0undo.h"
#include "row0vers.h"
#include "row0log.h"
#include "trx0trx.h"
#include "trx0rec.h"
#include "row0row.h"
#include "row0upd.h"
#include "que0que.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "fil0fil.h"

/*************************************************************************
IMPORTANT NOTE: Any operation that generates redo MUST check that there
is enough space in the redo log before for that operation. This is
done by calling log_free_check(). The reason for checking the
availability of the redo log space before the start of the operation is
that we MUST not hold any synchonization objects when performing the
check.
If you make a change in this module make sure that no codepath is
introduced where a call to log_free_check() is bypassed. */

/***************************************************************//**
Removes a clustered index record. The pcur in node was positioned on the
record, now it is detached.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static  MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_ins_remove_clust_rec(
/*==========================*/
	undo_node_t*	node)	/*!< in: undo node */
{
	btr_cur_t*	btr_cur;
	ibool		success;
	dberr_t		err;
	ulint		n_tries	= 0;
	mtr_t		mtr;
	dict_index_t*	index	= node->pcur.btr_cur.index;
	bool		online;

	ut_ad(dict_index_is_clust(index));
	ut_ad(node->trx->in_rollback);

	mtr.start();
	if (index->table->is_temporary()) {
		ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(mtr);
	}

	/* This is similar to row_undo_mod_clust(). The DDL thread may
	already have copied this row from the log to the new table.
	We must log the removal, so that the row will be correctly
	purged. However, we can log the removal out of sync with the
	B-tree modification. */

	online = dict_index_is_online_ddl(index);
	if (online) {
		ut_ad(node->trx->dict_operation_lock_mode
		      != RW_X_LATCH);
		ut_ad(node->table->id != DICT_INDEXES_ID);
		mtr_s_lock(dict_index_get_lock(index), &mtr);
	}

	success = btr_pcur_restore_position(
		online
		? BTR_MODIFY_LEAF | BTR_ALREADY_S_LATCHED
		: BTR_MODIFY_LEAF, &node->pcur, &mtr);
	ut_a(success);

	btr_cur = btr_pcur_get_btr_cur(&node->pcur);

	ut_ad(rec_get_trx_id(btr_cur_get_rec(btr_cur), btr_cur->index)
	      == node->trx->id);
	ut_ad(!rec_get_deleted_flag(
		      btr_cur_get_rec(btr_cur),
		      dict_table_is_comp(btr_cur->index->table)));

	if (online && dict_index_is_online_ddl(index)) {
		const rec_t*	rec	= btr_cur_get_rec(btr_cur);
		mem_heap_t*	heap	= NULL;
		const ulint*	offsets	= rec_get_offsets(
			rec, index, NULL, true, ULINT_UNDEFINED, &heap);
		row_log_table_delete(rec, index, offsets, NULL);
		mem_heap_free(heap);
	}

	switch (node->table->id) {
	case DICT_INDEXES_ID:
		ut_ad(!online);
		ut_ad(node->trx->dict_operation_lock_mode == RW_X_LATCH);
		ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);

		dict_drop_index_tree(
			btr_pcur_get_rec(&node->pcur), &(node->pcur), &mtr);

		mtr.commit();

		mtr.start();

		success = btr_pcur_restore_position(
			BTR_MODIFY_LEAF, &node->pcur, &mtr);
		ut_a(success);
		break;
	case DICT_COLUMNS_ID:
		/* This is rolling back an INSERT into SYS_COLUMNS.
		If it was part of an instant ADD COLUMN operation, we
		must modify the table definition. At this point, any
		corresponding operation to the metadata record will have
		been rolled back. */
		ut_ad(!online);
		ut_ad(node->trx->dict_operation_lock_mode == RW_X_LATCH);
		ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);
		const rec_t* rec = btr_pcur_get_rec(&node->pcur);
		if (rec_get_n_fields_old(rec)
		    != DICT_NUM_FIELDS__SYS_COLUMNS) {
			break;
		}
		ulint len;
		const byte* data = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_COLUMNS__TABLE_ID, &len);
		if (len != 8) {
			break;
		}
		const table_id_t table_id = mach_read_from_8(data);
		data = rec_get_nth_field_old(rec, DICT_FLD__SYS_COLUMNS__POS,
					     &len);
		if (len != 4) {
			break;
		}
		const unsigned pos = mach_read_from_4(data);
		if (pos == 0 || pos >= (1U << 16)) {
			break;
		}
		dict_table_t* table = dict_table_open_on_id(
			table_id, true, DICT_TABLE_OP_OPEN_ONLY_IF_CACHED);
		if (!table) {
			break;
		}

		dict_index_t* index = dict_table_get_first_index(table);

		if (index && index->is_instant()
		    && DATA_N_SYS_COLS + 1 + pos == table->n_cols) {
			/* This is the rollback of an instant ADD COLUMN.
			Remove the column from the dictionary cache,
			but keep the system columns. */
			table->rollback_instant(pos);
		}

		dict_table_close(table, true, false);
	}

	if (btr_cur_optimistic_delete(btr_cur, 0, &mtr)) {
		err = DB_SUCCESS;
		goto func_exit;
	}

	btr_pcur_commit_specify_mtr(&node->pcur, &mtr);
retry:
	/* If did not succeed, try pessimistic descent to tree */
	mtr.start();
	if (index->table->is_temporary()) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(mtr);
	}

	success = btr_pcur_restore_position(
			BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE,
			&node->pcur, &mtr);
	ut_a(success);

	btr_cur_pessimistic_delete(&err, FALSE, btr_cur, 0, true, &mtr);

	/* The delete operation may fail if we have little
	file space left: TODO: easiest to crash the database
	and restart with more file space */

	if (err == DB_OUT_OF_FILE_SPACE
	    && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES) {

		btr_pcur_commit_specify_mtr(&(node->pcur), &mtr);

		n_tries++;

		os_thread_sleep(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

func_exit:
	btr_pcur_commit_specify_mtr(&node->pcur, &mtr);
	if (err == DB_SUCCESS && node->rec_type == TRX_UNDO_INSERT_METADATA) {
		/* When rolling back the very first instant ADD COLUMN
		operation, reset the root page to the basic state. */
		ut_ad(!index->table->is_temporary());
		mtr.start();
		if (page_t* root = btr_root_get(index, &mtr)) {
			byte* page_type = root + FIL_PAGE_TYPE;
			ut_ad(mach_read_from_2(page_type)
			      == FIL_PAGE_TYPE_INSTANT
			      || mach_read_from_2(page_type)
			      == FIL_PAGE_INDEX);
			index->set_modified(mtr);
			mlog_write_ulint(page_type, FIL_PAGE_INDEX,
					 MLOG_2BYTES, &mtr);
			byte* instant = PAGE_INSTANT + PAGE_HEADER + root;
			mlog_write_ulint(instant,
					 page_ptr_get_direction(instant + 1),
					 MLOG_2BYTES, &mtr);
		}
		mtr.commit();
	}

	return(err);
}

/***************************************************************//**
Removes a secondary index entry if found.
@return DB_SUCCESS, DB_FAIL, or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_ins_remove_sec_low(
/*========================*/
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether we wish optimistic or
				pessimistic descent down the index tree */
	dict_index_t*	index,	/*!< in: index */
	dtuple_t*	entry,	/*!< in: index entry to remove */
	que_thr_t*	thr)	/*!< in: query thread */
{
	btr_pcur_t		pcur;
	btr_cur_t*		btr_cur;
	dberr_t			err	= DB_SUCCESS;
	mtr_t			mtr;
	enum row_search_result	search_result;
	const bool		modify_leaf = mode == BTR_MODIFY_LEAF;

	row_mtr_start(&mtr, index, !modify_leaf);

	if (modify_leaf) {
		mode = BTR_MODIFY_LEAF | BTR_ALREADY_S_LATCHED;
		mtr_s_lock(dict_index_get_lock(index), &mtr);
	} else {
		ut_ad(mode == (BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE));
		mtr_sx_lock(dict_index_get_lock(index), &mtr);
	}

	if (row_log_online_op_try(index, entry, 0)) {
		goto func_exit_no_pcur;
	}

	if (dict_index_is_spatial(index)) {
		if (modify_leaf) {
			mode |= BTR_RTREE_DELETE_MARK;
		}
		btr_pcur_get_btr_cur(&pcur)->thr = thr;
		mode |= BTR_RTREE_UNDO_INS;
	}

	search_result = row_search_index_entry(index, entry, mode,
					       &pcur, &mtr);

	switch (search_result) {
	case ROW_NOT_FOUND:
		goto func_exit;
	case ROW_FOUND:
		if (dict_index_is_spatial(index)
		    && rec_get_deleted_flag(
			    btr_pcur_get_rec(&pcur),
			    dict_table_is_comp(index->table))) {
			ib::error() << "Record found in index " << index->name
				<< " is deleted marked on insert rollback.";
			ut_ad(0);
		}
		break;

	case ROW_BUFFERED:
	case ROW_NOT_DELETED_REF:
		/* These are invalid outcomes, because the mode passed
		to row_search_index_entry() did not include any of the
		flags BTR_INSERT, BTR_DELETE, or BTR_DELETE_MARK. */
		ut_error;
	}

	btr_cur = btr_pcur_get_btr_cur(&pcur);

	if (modify_leaf) {
		err = btr_cur_optimistic_delete(btr_cur, 0, &mtr)
			? DB_SUCCESS : DB_FAIL;
	} else {
		/* Passing rollback=false here, because we are
		deleting a secondary index record: the distinction
		only matters when deleting a record that contains
		externally stored columns. */
		ut_ad(!dict_index_is_clust(index));
		btr_cur_pessimistic_delete(&err, FALSE, btr_cur, 0,
					   false, &mtr);
	}
func_exit:
	btr_pcur_close(&pcur);
func_exit_no_pcur:
	mtr_commit(&mtr);

	return(err);
}

/***************************************************************//**
Removes a secondary index entry from the index if found. Tries first
optimistic, then pessimistic descent down the tree.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_ins_remove_sec(
/*====================*/
	dict_index_t*	index,	/*!< in: index */
	dtuple_t*	entry,	/*!< in: index entry to insert */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	n_tries	= 0;

	/* Try first optimistic descent to the B-tree */

	err = row_undo_ins_remove_sec_low(BTR_MODIFY_LEAF, index, entry, thr);

	if (err == DB_SUCCESS) {

		return(err);
	}

	/* Try then pessimistic descent to the B-tree */
retry:
	err = row_undo_ins_remove_sec_low(
		BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE,
		index, entry, thr);

	/* The delete operation may fail if we have little
	file space left: TODO: easiest to crash the database
	and restart with more file space */

	if (err != DB_SUCCESS && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES) {

		n_tries++;

		os_thread_sleep(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

	return(err);
}

/***********************************************************//**
Parses the row reference and other info in a fresh insert undo record. */
static
void
row_undo_ins_parse_undo_rec(
/*========================*/
	undo_node_t*	node,		/*!< in/out: row undo node */
	ibool		dict_locked)	/*!< in: TRUE if own dict_sys->mutex */
{
	dict_index_t*	clust_index;
	byte*		ptr;
	undo_no_t	undo_no;
	table_id_t	table_id;
	ulint		dummy;
	bool		dummy_extern;

	ut_ad(node);

	ptr = trx_undo_rec_get_pars(node->undo_rec, &node->rec_type, &dummy,
				    &dummy_extern, &undo_no, &table_id);

	node->update = NULL;
	node->table = dict_table_open_on_id(
		table_id, dict_locked, DICT_TABLE_OP_NORMAL);

	/* Skip the UNDO if we can't find the table or the .ibd file. */
	if (UNIV_UNLIKELY(node->table == NULL)) {
		return;
	}

	switch (node->rec_type) {
	default:
		ut_ad(!"wrong undo record type");
		goto close_table;
	case TRX_UNDO_INSERT_METADATA:
	case TRX_UNDO_INSERT_REC:
		break;
	case TRX_UNDO_RENAME_TABLE:
		dict_table_t* table = node->table;
		ut_ad(!table->is_temporary());
		ut_ad(dict_table_is_file_per_table(table)
		      == !is_system_tablespace(table->space->id));
		size_t len = mach_read_from_2(node->undo_rec)
			+ size_t(node->undo_rec - ptr) - 2;
		ptr[len] = 0;
		const char* name = reinterpret_cast<char*>(ptr);
		if (strcmp(table->name.m_name, name)) {
			dict_table_rename_in_cache(table, name, false,
						   table_id != 0);
		}
		goto close_table;
	}

	if (UNIV_UNLIKELY(!fil_table_accessible(node->table))) {
close_table:
		/* Normally, tables should not disappear or become
		unaccessible during ROLLBACK, because they should be
		protected by InnoDB table locks. Corruption could be
		a valid exception.

		FIXME: When running out of temporary tablespace, it
		would probably be better to just drop all temporary
		tables (and temporary undo log records) of the current
		connection, instead of doing this rollback. */
		dict_table_close(node->table, dict_locked, FALSE);
		node->table = NULL;
	} else {
		ut_ad(!node->table->skip_alter_undo);
		clust_index = dict_table_get_first_index(node->table);

		if (clust_index != NULL) {
			if (node->rec_type == TRX_UNDO_INSERT_REC) {
				ptr = trx_undo_rec_get_row_ref(
					ptr, clust_index, &node->ref,
					node->heap);
			} else {
				node->ref = &trx_undo_metadata;
			}

			if (!row_undo_search_clust_to_pcur(node)) {
				/* An error probably occurred during
				an insert into the clustered index,
				after we wrote the undo log record. */
				goto close_table;
			}
			if (node->table->n_v_cols) {
				trx_undo_read_v_cols(node->table, ptr,
						     node->row, false);
			}

		} else {
			ib::warn() << "Table " << node->table->name
				 << " has no indexes,"
				" ignoring the table";
			goto close_table;
		}
	}
}

/***************************************************************//**
Removes secondary index records.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_ins_remove_sec_rec(
/*========================*/
	undo_node_t*	node,	/*!< in/out: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t		err	= DB_SUCCESS;
	dict_index_t*	index	= node->index;
	mem_heap_t*	heap;

	heap = mem_heap_create(1024);

	while (index != NULL) {
		dtuple_t*	entry;

		if (index->type & DICT_FTS) {
			dict_table_next_uncorrupted_index(index);
			continue;
		}

		/* An insert undo record TRX_UNDO_INSERT_REC will
		always contain all fields of the index. It does not
		matter if any indexes were created afterwards; all
		index entries can be reconstructed from the row. */
		entry = row_build_index_entry(
			node->row, node->ext, index, heap);
		if (UNIV_UNLIKELY(!entry)) {
			/* The database must have crashed after
			inserting a clustered index record but before
			writing all the externally stored columns of
			that record, or a statement is being rolled
			back because an error occurred while storing
			off-page columns.

			Because secondary index entries are inserted
			after the clustered index record, we may
			assume that the secondary index record does
			not exist. */
		} else {
			err = row_undo_ins_remove_sec(index, entry, thr);

			if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
				goto func_exit;
			}
		}

		mem_heap_empty(heap);
		dict_table_next_uncorrupted_index(index);
	}

func_exit:
	node->index = index;
	mem_heap_free(heap);
	return(err);
}

/***********************************************************//**
Undoes a fresh insert of a row to a table. A fresh insert means that
the same clustered index unique key did not have any record, even delete
marked, at the time of the insert.  InnoDB is eager in a rollback:
if it figures out that an index record will be removed in the purge
anyway, it will remove it in the rollback.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
dberr_t
row_undo_ins(
/*=========*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t	err;
	ibool	dict_locked;

	ut_ad(node->state == UNDO_NODE_INSERT);
	ut_ad(node->trx->in_rollback);
	ut_ad(trx_undo_roll_ptr_is_insert(node->roll_ptr));

	dict_locked = node->trx->dict_operation_lock_mode == RW_X_LATCH;

	row_undo_ins_parse_undo_rec(node, dict_locked);

	if (node->table == NULL) {
		return(DB_SUCCESS);
	}

	/* Iterate over all the indexes and undo the insert.*/

	node->index = dict_table_get_first_index(node->table);
	ut_ad(dict_index_is_clust(node->index));

	switch (node->rec_type) {
	default:
		ut_ad(!"wrong undo record type");
	case TRX_UNDO_INSERT_REC:
		/* Skip the clustered index (the first index) */
		node->index = dict_table_get_next_index(node->index);

		dict_table_skip_corrupt_index(node->index);

		err = row_undo_ins_remove_sec_rec(node, thr);

		if (err != DB_SUCCESS) {
			break;
		}

		/* fall through */
	case TRX_UNDO_INSERT_METADATA:
		log_free_check();

		if (node->table->id == DICT_INDEXES_ID) {
			ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);

			if (!dict_locked) {
				mutex_enter(&dict_sys->mutex);
			}
		}

		// FIXME: We need to update the dict_index_t::space and
		// page number fields too.
		err = row_undo_ins_remove_clust_rec(node);

		if (node->table->id == DICT_INDEXES_ID
		    && !dict_locked) {

			mutex_exit(&dict_sys->mutex);
		}

		if (err == DB_SUCCESS && node->table->stat_initialized) {
			/* Not protected by dict_table_stats_lock() for
			performance reasons, we would rather get garbage
			in stat_n_rows (which is just an estimate anyway)
			than protecting the following code with a latch. */
			dict_table_n_rows_dec(node->table);

			/* Do not attempt to update statistics when
			executing ROLLBACK in the InnoDB SQL
			interpreter, because in that case we would
			already be holding dict_sys->mutex, which
			would be acquired when updating statistics. */
			if (!dict_locked) {
				dict_stats_update_if_needed(
					node->table, node->trx->mysql_thd);
			}
		}
	}

	dict_table_close(node->table, dict_locked, FALSE);

	node->table = NULL;

	return(err);
}
