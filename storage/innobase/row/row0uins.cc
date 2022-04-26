/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
#include "trx0trx.h"
#include "trx0rec.h"
#include "row0row.h"
#include "row0upd.h"
#include "que0que.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "fil0fil.h"
#include <mysql/service_thd_mdl.h>

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
	dberr_t		err;
	ulint		n_tries	= 0;
	mtr_t		mtr;
	dict_index_t*	index	= node->pcur.btr_cur.index;
	table_id_t table_id = 0;
	const bool dict_locked = node->trx->dict_operation_lock_mode;
restart:
	MDL_ticket* mdl_ticket = nullptr;
	ut_ad(!table_id || dict_locked
	      || !node->trx->dict_operation_lock_mode);
	dict_table_t *table = table_id
		? dict_table_open_on_id(table_id, dict_locked,
					DICT_TABLE_OP_OPEN_ONLY_IF_CACHED,
					node->trx->mysql_thd, &mdl_ticket)
		: nullptr;

	ut_ad(index->is_primary());
	ut_ad(node->trx->in_rollback);

	mtr.start();
	if (index->table->is_temporary()) {
		ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		ut_ad(index->table->id >= DICT_HDR_FIRST_ID);
	} else {
		index->set_modified(mtr);
		ut_ad(lock_table_has_locks(index->table));
	}

	/* This is similar to row_undo_mod_clust(). The DDL thread may
	already have copied this row from the log to the new table.
	We must log the removal, so that the row will be correctly
	purged. However, we can log the removal out of sync with the
	B-tree modification. */
	ut_a(node->pcur.restore_position(
	      (node->rec_type == TRX_UNDO_INSERT_METADATA)
		? BTR_MODIFY_TREE
		: BTR_MODIFY_LEAF,
	      &mtr) == btr_pcur_t::SAME_ALL);
	rec_t* rec = btr_pcur_get_rec(&node->pcur);

	ut_ad(rec_get_trx_id(rec, index) == node->trx->id
	      || node->table->is_temporary());
	ut_ad(!rec_get_deleted_flag(rec, index->table->not_redundant())
	      || rec_is_alter_metadata(rec, index->table->not_redundant()));
	ut_ad(rec_is_metadata(rec, index->table->not_redundant())
	      == (node->rec_type == TRX_UNDO_INSERT_METADATA));

	switch (node->table->id) {
	case DICT_COLUMNS_ID:
		/* This is rolling back an INSERT into SYS_COLUMNS.
		If it was part of an instant ALTER TABLE operation, we
		must evict the table definition, so that it can be
		reloaded after the dictionary operation has been
		completed. At this point, any corresponding operation
		to the metadata record will have been rolled back. */
		ut_ad(node->trx->dict_operation_lock_mode);
		ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);
		if (rec_get_n_fields_old(rec)
		    != DICT_NUM_FIELDS__SYS_COLUMNS
		    || (rec_get_1byte_offs_flag(rec)
			? rec_1_get_field_end_info(rec, 0) != 8
			: rec_2_get_field_end_info(rec, 0) != 8)) {
			break;
		}
		static_assert(!DICT_FLD__SYS_COLUMNS__TABLE_ID, "");
		node->trx->evict_table(mach_read_from_8(rec));
		break;
	case DICT_INDEXES_ID:
		ut_ad(node->trx->dict_operation_lock_mode);
		ut_ad(node->rec_type == TRX_UNDO_INSERT_REC);
		if (!table_id) {
			table_id = mach_read_from_8(rec);
			if (table_id) {
				mtr.commit();
				goto restart;
			}
			ut_ad("corrupted SYS_INDEXES record" == 0);
		}

		pfs_os_file_t d = OS_FILE_CLOSED;

		if (const uint32_t space_id = dict_drop_index_tree(
			    &node->pcur, node->trx, &mtr)) {
			if (table) {
				lock_release_on_rollback(node->trx,
							 table);
				if (!dict_locked) {
					dict_sys.lock(SRW_LOCK_CALL);
				}
				if (table->release()) {
					dict_sys.remove(table);
				} else if (table->space_id
					   == space_id) {
					table->space = nullptr;
					table->file_unreadable = true;
				}
				if (!dict_locked) {
					dict_sys.unlock();
				}
				table = nullptr;
				if (!mdl_ticket);
				else if (MDL_context* mdl_context =
					 static_cast<MDL_context*>(
						 thd_mdl_context(
							 node->trx->
							 mysql_thd))) {
					mdl_context->release_lock(
						mdl_ticket);
					mdl_ticket = nullptr;
				}
			}

			d = fil_delete_tablespace(space_id);
		}

		mtr.commit();

		if (d != OS_FILE_CLOSED) {
			os_file_close(d);
		}

		mtr.start();
		ut_a(node->pcur.restore_position(
			BTR_MODIFY_LEAF, &mtr) == btr_pcur_t::SAME_ALL);
	}

	if (btr_cur_optimistic_delete(&node->pcur.btr_cur, 0, &mtr)) {
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
	ut_a(
	    node->pcur.restore_position(BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE,
	      &mtr) == btr_pcur_t::SAME_ALL);

	btr_cur_pessimistic_delete(&err, FALSE, &node->pcur.btr_cur, 0, true,
				   &mtr);

	/* The delete operation may fail if we have little
	file space left: TODO: easiest to crash the database
	and restart with more file space */

	if (err == DB_OUT_OF_FILE_SPACE
	    && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES) {

		btr_pcur_commit_specify_mtr(&(node->pcur), &mtr);

		n_tries++;

		std::this_thread::sleep_for(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

func_exit:
	if (err == DB_SUCCESS && node->rec_type == TRX_UNDO_INSERT_METADATA) {
		/* When rolling back the very first instant ADD COLUMN
		operation, reset the root page to the basic state. */
		btr_reset_instant(*index, true, &mtr);
	}

	btr_pcur_commit_specify_mtr(&node->pcur, &mtr);

	if (UNIV_LIKELY_NULL(table)) {
		dict_table_close(table, dict_locked,
				 node->trx->mysql_thd, mdl_ticket);
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
	dberr_t			err	= DB_SUCCESS;
	mtr_t			mtr;
	const bool		modify_leaf = mode == BTR_MODIFY_LEAF;

	row_mtr_start(&mtr, index, !modify_leaf);

	if (modify_leaf) {
		mode = BTR_MODIFY_LEAF | BTR_ALREADY_S_LATCHED;
		mtr_s_lock_index(index, &mtr);
	} else {
		ut_ad(mode == (BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE));
		mtr_sx_lock_index(index, &mtr);
	}

	if (dict_index_is_spatial(index)) {
		if (modify_leaf) {
			mode |= BTR_RTREE_DELETE_MARK;
		}
		btr_pcur_get_btr_cur(&pcur)->thr = thr;
		mode |= BTR_RTREE_UNDO_INS;
	}

	switch (row_search_index_entry(index, entry, mode, &pcur, &mtr)) {
	case ROW_BUFFERED:
	case ROW_NOT_DELETED_REF:
		/* These are invalid outcomes, because the mode passed
		to row_search_index_entry() did not include any of the
		flags BTR_INSERT, BTR_DELETE, or BTR_DELETE_MARK. */
		ut_error;
	case ROW_NOT_FOUND:
		break;
	case ROW_FOUND:
		if (dict_index_is_spatial(index)
		    && rec_get_deleted_flag(
			    btr_pcur_get_rec(&pcur),
			    dict_table_is_comp(index->table))) {
			ib::error() << "Record found in index " << index->name
				<< " is deleted marked on insert rollback.";
			ut_ad(0);
		}

		btr_cur_t* btr_cur = btr_pcur_get_btr_cur(&pcur);

		if (modify_leaf) {
			err = btr_cur_optimistic_delete(btr_cur, 0, &mtr)
				? DB_SUCCESS : DB_FAIL;
		} else {
			/* Passing rollback=false here, because we are
			deleting a secondary index record: the distinction
			only matters when deleting a record that contains
			externally stored columns. */
			btr_cur_pessimistic_delete(&err, FALSE, btr_cur, 0,
						   false, &mtr);
		}
	}

	btr_pcur_close(&pcur);
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

		std::this_thread::sleep_for(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

	return(err);
}

/** Parse an insert undo record.
@param[in,out]	node		row rollback state
@param[in]	dict_locked	whether the data dictionary cache is locked */
static bool row_undo_ins_parse_undo_rec(undo_node_t* node, bool dict_locked)
{
	dict_index_t*	clust_index;
	byte*		ptr;
	undo_no_t	undo_no;
	table_id_t	table_id;
	ulint		dummy;
	bool		dummy_extern;

	ut_ad(node->state == UNDO_INSERT_PERSISTENT
	      || node->state == UNDO_INSERT_TEMPORARY);
	ut_ad(node->trx->in_rollback);
	ut_ad(trx_undo_roll_ptr_is_insert(node->roll_ptr));

	ptr = trx_undo_rec_get_pars(node->undo_rec, &node->rec_type, &dummy,
				    &dummy_extern, &undo_no, &table_id);

	node->update = NULL;
	if (node->state == UNDO_INSERT_PERSISTENT) {
		node->table = dict_table_open_on_id(table_id, dict_locked,
						    DICT_TABLE_OP_NORMAL);
	} else if (!dict_locked) {
		dict_sys.freeze(SRW_LOCK_CALL);
		node->table = dict_sys.acquire_temporary_table(table_id);
		dict_sys.unfreeze();
	} else {
		node->table = dict_sys.acquire_temporary_table(table_id);
	}

	if (!node->table) {
		return false;
	}

	switch (node->rec_type) {
	default:
		ut_ad("wrong undo record type" == 0);
		goto close_table;
	case TRX_UNDO_INSERT_METADATA:
	case TRX_UNDO_INSERT_REC:
	case TRX_UNDO_EMPTY:
		break;
	case TRX_UNDO_RENAME_TABLE:
		dict_table_t* table = node->table;
		ut_ad(!table->is_temporary());
		ut_ad(table->file_unreadable
		      || dict_table_is_file_per_table(table)
		      == !is_system_tablespace(table->space_id));
		size_t len = mach_read_from_2(node->undo_rec)
			+ size_t(node->undo_rec - ptr) - 2;
		ptr[len] = 0;
		const char* name = reinterpret_cast<char*>(ptr);
		if (strcmp(table->name.m_name, name)) {
			dict_table_rename_in_cache(
				table, name,
				!dict_table_t::is_temporary_name(name),
				true);
		} else if (table->space) {
			const auto s = table->space->name();
			if (len != s.size() || memcmp(name, s.data(), len)) {
				table->rename_tablespace(name, true);
			}
		}
		goto close_table;
	}

	if (UNIV_UNLIKELY(!node->table->is_accessible())) {
close_table:
		/* Normally, tables should not disappear or become
		unaccessible during ROLLBACK, because they should be
		protected by InnoDB table locks. Corruption could be
		a valid exception.

		FIXME: When running out of temporary tablespace, it
		would probably be better to just drop all temporary
		tables (and temporary undo log records) of the current
		connection, instead of doing this rollback. */
		dict_table_close(node->table, dict_locked);
		node->table = NULL;
		return false;
	} else {
		ut_ad(!node->table->skip_alter_undo);
		clust_index = dict_table_get_first_index(node->table);

		if (clust_index != NULL) {
			switch (node->rec_type) {
			case TRX_UNDO_INSERT_REC:
				ptr = trx_undo_rec_get_row_ref(
					ptr, clust_index, &node->ref,
					node->heap);
				break;
			case TRX_UNDO_EMPTY:
				node->ref = nullptr;
				return true;
			default:
				node->ref = &trx_undo_metadata;
				if (!row_undo_search_clust_to_pcur(node)) {
					/* An error probably occurred during
					an insert into the clustered index,
					after we wrote the undo log record. */
					goto close_table;
				}
				return true;
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

	return true;
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

		if (index->type & DICT_FTS || !index->is_committed()) {
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
	const bool dict_locked = node->trx->dict_operation_lock_mode;

	if (!row_undo_ins_parse_undo_rec(node, dict_locked)) {
		return DB_SUCCESS;
	}

	ut_ad(node->table->is_temporary()
	      || lock_table_has_locks(node->table));

	/* Iterate over all the indexes and undo the insert.*/

	node->index = dict_table_get_first_index(node->table);
	ut_ad(dict_index_is_clust(node->index));

	switch (node->rec_type) {
	default:
		ut_ad("wrong undo record type" == 0);
		/* fall through */
	case TRX_UNDO_INSERT_REC:
		/* Skip the clustered index (the first index) */
		node->index = dict_table_get_next_index(node->index);

		dict_table_skip_corrupt_index(node->index);

		err = row_undo_ins_remove_sec_rec(node, thr);

		if (err != DB_SUCCESS) {
			break;
		}

		log_free_check();

		if (!dict_locked && node->table->id == DICT_INDEXES_ID) {
			dict_sys.lock(SRW_LOCK_CALL);
			err = row_undo_ins_remove_clust_rec(node);
			dict_sys.unlock();
		} else {
			ut_ad(node->table->id != DICT_INDEXES_ID
			      || !node->table->is_temporary());
			err = row_undo_ins_remove_clust_rec(node);
		}

		if (err == DB_SUCCESS && node->table->stat_initialized) {
			/* Not protected by dict_sys.latch
			or table->stats_mutex_lock() for
			performance reasons, we would rather get garbage
			in stat_n_rows (which is just an estimate anyway)
			than protecting the following code with a latch. */
			dict_table_n_rows_dec(node->table);

			/* Do not attempt to update statistics when
			executing ROLLBACK in the InnoDB SQL
			interpreter, because in that case we would
			already be holding dict_sys.latch, which
			would be acquired when updating statistics. */
			if (!dict_locked) {
				dict_stats_update_if_needed(node->table,
							    *node->trx);
			}
		}
		break;

	case TRX_UNDO_INSERT_METADATA:
		log_free_check();
		ut_ad(!node->table->is_temporary());
		err = row_undo_ins_remove_clust_rec(node);
		break;
	case TRX_UNDO_EMPTY:
		node->table->clear(thr);
		err = DB_SUCCESS;
		break;
	}

	dict_table_close(node->table, dict_locked);

	node->table = NULL;

	return(err);
}
