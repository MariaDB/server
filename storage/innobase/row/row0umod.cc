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
@file row/row0umod.cc
Undo modify of a row

Created 2/27/1997 Heikki Tuuri
*******************************************************/

#include "row0umod.h"
#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0boot.h"
#include "trx0undo.h"
#include "trx0roll.h"
#include "trx0purge.h"
#include "btr0btr.h"
#include "mach0data.h"
#include "ibuf0ibuf.h"
#include "row0undo.h"
#include "row0vers.h"
#include "trx0trx.h"
#include "trx0rec.h"
#include "row0row.h"
#include "row0upd.h"
#include "que0que.h"
#include "log0log.h"

/* Considerations on undoing a modify operation.
(1) Undoing a delete marking: all index records should be found. Some of
them may have delete mark already FALSE, if the delete mark operation was
stopped underway, or if the undo operation ended prematurely because of a
system crash.
(2) Undoing an update of a delete unmarked record: the newer version of
an updated secondary index entry should be removed if no prior version
of the clustered index record requires its existence. Otherwise, it should
be delete marked.
(3) Undoing an update of a delete marked record. In this kind of update a
delete marked clustered index record was delete unmarked and possibly also
some of its fields were changed. Now, it is possible that the delete marked
version has become obsolete at the time the undo is started. */

/*************************************************************************
IMPORTANT NOTE: Any operation that generates redo MUST check that there
is enough space in the redo log before for that operation. This is
done by calling log_free_check(). The reason for checking the
availability of the redo log space before the start of the operation is
that we MUST not hold any synchonization objects when performing the
check.
If you make a change in this module make sure that no codepath is
introduced where a call to log_free_check() is bypassed. */

/***********************************************************//**
Undoes a modify in a clustered index record.
@return DB_SUCCESS, DB_FAIL, or error code: we may run out of file space */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_clust_low(
/*===================*/
	undo_node_t*	node,	/*!< in: row undo node */
	rec_offs**	offsets,/*!< out: rec_get_offsets() on the record */
	mem_heap_t**	offsets_heap,
				/*!< in/out: memory heap that can be emptied */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	byte*		sys,	/*!< out: DB_TRX_ID, DB_ROLL_PTR
				for row_log_table_delete() */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in: mtr; must be committed before
				latching any further pages */
	ulint		mode)	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE */
{
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	dberr_t		err;

	pcur = &node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	ut_d(auto pcur_restore_result =)
	pcur->restore_position(mode, mtr);

	ut_ad(pcur_restore_result == btr_pcur_t::SAME_ALL);
	ut_ad(rec_get_trx_id(btr_cur_get_rec(btr_cur),
			     btr_cur_get_index(btr_cur))
	      == thr_get_trx(thr)->id
	      || btr_cur_get_index(btr_cur)->table->is_temporary());
	ut_ad(node->ref != &trx_undo_metadata
	      || node->update->info_bits == REC_INFO_METADATA_ADD
	      || node->update->info_bits == REC_INFO_METADATA_ALTER);

	if (mode != BTR_MODIFY_TREE) {
		ut_ad((mode & ulint(~BTR_ALREADY_S_LATCHED))
		      == BTR_MODIFY_LEAF);

		err = btr_cur_optimistic_update(
			BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG
			| BTR_KEEP_SYS_FLAG,
			btr_cur, offsets, offsets_heap,
			node->update, node->cmpl_info,
			thr, thr_get_trx(thr)->id, mtr);
		ut_ad(err != DB_SUCCESS || node->ref != &trx_undo_metadata);
	} else {
		big_rec_t*	dummy_big_rec;

		err = btr_cur_pessimistic_update(
			BTR_NO_LOCKING_FLAG
			| BTR_NO_UNDO_LOG_FLAG
			| BTR_KEEP_SYS_FLAG,
			btr_cur, offsets, offsets_heap, heap,
			&dummy_big_rec, node->update,
			node->cmpl_info, thr, thr_get_trx(thr)->id, mtr);

		ut_a(!dummy_big_rec);

		if (err == DB_SUCCESS
		    && node->ref == &trx_undo_metadata
		    && btr_cur_get_index(btr_cur)->table->instant
		    && node->update->info_bits == REC_INFO_METADATA_ADD) {
			btr_reset_instant(*btr_cur_get_index(btr_cur), false,
					  mtr);
		}
	}

	if (err != DB_SUCCESS) {
		return err;
	}

	switch (const auto id = btr_cur_get_index(btr_cur)->table->id) {
		unsigned c;
	case DICT_TABLES_ID:
		if (node->trx != trx_roll_crash_recv_trx) {
			break;
		}
		c = DICT_COL__SYS_TABLES__ID;
		goto evict;
	case DICT_INDEXES_ID:
		if (node->trx != trx_roll_crash_recv_trx) {
			break;
		} else if (node->rec_type == TRX_UNDO_DEL_MARK_REC
			   && btr_cur_get_rec(btr_cur)
			   [8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]
			   == static_cast<byte>(*TEMP_INDEX_PREFIX_STR)) {
			/* We are rolling back the DELETE of metadata
			for a failed ADD INDEX operation. This does
			not affect any cached table definition,
			because we are filtering out such indexes in
			dict_load_indexes(). */
			break;
		}
		/* fall through */
	case DICT_COLUMNS_ID:
		static_assert(!DICT_COL__SYS_INDEXES__TABLE_ID, "");
		static_assert(!DICT_COL__SYS_COLUMNS__TABLE_ID, "");
		c = DICT_COL__SYS_COLUMNS__TABLE_ID;
		/* This is rolling back an UPDATE or DELETE on SYS_COLUMNS.
		If it was part of an instant ALTER TABLE operation, we
		must evict the table definition, so that it can be
		reloaded after the dictionary operation has been
		completed. At this point, any corresponding operation
		to the metadata record will have been rolled back. */
	evict:
		const dfield_t& table_id = *dtuple_get_nth_field(node->row, c);
		ut_ad(dfield_get_len(&table_id) == 8);
		node->trx->evict_table(mach_read_from_8(
					       static_cast<byte*>(
						       table_id.data)),
				       id == DICT_COLUMNS_ID);
	}

	return DB_SUCCESS;
}

/** Get the byte offset of the DB_TRX_ID column
@param[in]	rec	clustered index record
@param[in]	index	clustered index
@return	the byte offset of DB_TRX_ID, from the start of rec */
static ulint row_trx_id_offset(const rec_t* rec, const dict_index_t* index)
{
	ut_ad(index->n_uniq <= MAX_REF_PARTS);
	ulint trx_id_offset = index->trx_id_offset;
	if (!trx_id_offset) {
		/* Reserve enough offsets for the PRIMARY KEY and 2 columns
		so that we can access DB_TRX_ID, DB_ROLL_PTR. */
		rec_offs offsets_[REC_OFFS_HEADER_SIZE + MAX_REF_PARTS + 2];
		rec_offs_init(offsets_);
		mem_heap_t* heap = NULL;
		const ulint trx_id_pos = index->n_uniq ? index->n_uniq : 1;
		rec_offs* offsets = rec_get_offsets(rec, index, offsets_,
						    index->n_core_fields,
						    trx_id_pos + 1, &heap);
		ut_ad(!heap);
		ulint len;
		trx_id_offset = rec_get_nth_field_offs(
			offsets, trx_id_pos, &len);
		ut_ad(len == DATA_TRX_ID_LEN);
	}

	return trx_id_offset;
}

/** Determine if rollback must execute a purge-like operation.
@param[in,out]	node	row undo
@param[in,out]	mtr	mini-transaction
@return	whether the record should be purged */
static bool row_undo_mod_must_purge(undo_node_t* node, mtr_t* mtr)
{
	ut_ad(node->rec_type == TRX_UNDO_UPD_DEL_REC);
	ut_ad(!node->table->is_temporary());

	btr_cur_t* btr_cur = btr_pcur_get_btr_cur(&node->pcur);
	ut_ad(btr_cur->index->is_primary());
	DEBUG_SYNC_C("rollback_purge_clust");

	if (!purge_sys.changes_visible(node->new_trx_id, node->table->name)) {
		return false;
	}

	const rec_t* rec = btr_cur_get_rec(btr_cur);

	return trx_read_trx_id(rec + row_trx_id_offset(rec, btr_cur->index))
		== node->new_trx_id;
}

/***********************************************************//**
Undoes a modify in a clustered index record. Sets also the node state for the
next round of undo.
@return DB_SUCCESS or error code: we may run out of file space */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_clust(
/*===============*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	btr_pcur_t*	pcur;
	mtr_t		mtr;
	bool		have_latch = false;
	dberr_t		err;
	dict_index_t*	index;

	ut_ad(thr_get_trx(thr) == node->trx);
	ut_ad(node->trx->in_rollback);

	log_free_check();
	pcur = &node->pcur;
	index = btr_cur_get_index(btr_pcur_get_btr_cur(pcur));
	ut_ad(index->is_primary());

	mtr.start();
	if (index->table->is_temporary()) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(mtr);
		ut_ad(lock_table_has_locks(index->table));
	}

	mem_heap_t*	heap		= mem_heap_create(1024);
	mem_heap_t*	offsets_heap	= NULL;
	rec_offs*	offsets		= NULL;
	byte		sys[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN];

	/* Try optimistic processing of the record, keeping changes within
	the index page */

	err = row_undo_mod_clust_low(node, &offsets, &offsets_heap,
				     heap, sys, thr, &mtr, BTR_MODIFY_LEAF);

	if (err != DB_SUCCESS) {
		btr_pcur_commit_specify_mtr(pcur, &mtr);

		/* We may have to modify tree structure: do a pessimistic
		descent down the index tree */

		mtr.start();
		if (index->table->is_temporary()) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		} else {
			index->set_modified(mtr);
		}

		err = row_undo_mod_clust_low(node, &offsets, &offsets_heap,
					     heap, sys, thr, &mtr,
					     BTR_MODIFY_TREE);
		ut_ad(err == DB_SUCCESS || err == DB_OUT_OF_FILE_SPACE);
	}

	/**
	* when scrubbing, and records gets cleared,
	*   the transaction id is not present afterwards.
	*   this is safe as: since the record is on free-list
	*   it can be reallocated at any time after this mtr-commits
	*   which is just below
	*/
	ut_ad(srv_immediate_scrub_data_uncompressed
	      || row_get_rec_trx_id(btr_pcur_get_rec(pcur), index, offsets)
	      == node->new_trx_id);

	btr_pcur_commit_specify_mtr(pcur, &mtr);
	DEBUG_SYNC_C("rollback_undo_pk");

	if (err != DB_SUCCESS) {
		goto func_exit;
	}

	/* FIXME: Perform the below operations in the above
	mini-transaction when possible. */

	if (node->rec_type == TRX_UNDO_UPD_DEL_REC) {
		/* In delete-marked records, DB_TRX_ID must
		always refer to an existing update_undo log record. */
		ut_ad(node->new_trx_id);

		mtr.start();
		if (pcur->restore_position(BTR_MODIFY_LEAF, &mtr) !=
		    btr_pcur_t::SAME_ALL) {
			goto mtr_commit_exit;
		}

		ut_ad(rec_get_deleted_flag(btr_pcur_get_rec(pcur),
					   dict_table_is_comp(node->table)));

		if (index->table->is_temporary()) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
			if (btr_cur_optimistic_delete(&pcur->btr_cur, 0,
						      &mtr)) {
				goto mtr_commit_exit;
			}
			btr_pcur_commit_specify_mtr(pcur, &mtr);
		} else {
			index->set_modified(mtr);
			have_latch = true;
			purge_sys.latch.rd_lock(SRW_LOCK_CALL);
			if (!row_undo_mod_must_purge(node, &mtr)) {
				goto mtr_commit_exit;
			}
			if (btr_cur_optimistic_delete(&pcur->btr_cur, 0,
						      &mtr)) {
				goto mtr_commit_exit;
			}
			purge_sys.latch.rd_unlock();
			btr_pcur_commit_specify_mtr(pcur, &mtr);
			have_latch = false;
		}

		mtr.start();
		if (pcur->restore_position(
			    BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE, &mtr) !=
		    btr_pcur_t::SAME_ALL) {
			goto mtr_commit_exit;
		}

		ut_ad(rec_get_deleted_flag(btr_pcur_get_rec(pcur),
					   dict_table_is_comp(node->table)));

		if (index->table->is_temporary()) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		} else {
			have_latch = true;
			purge_sys.latch.rd_lock(SRW_LOCK_CALL);
			if (!row_undo_mod_must_purge(node, &mtr)) {
				goto mtr_commit_exit;
			}
			index->set_modified(mtr);
		}

		/* This operation is analogous to purge, we can free
		also inherited externally stored fields. We can also
		assume that the record was complete (including BLOBs),
		because it had been delete-marked after it had been
		completely inserted. Therefore, we are passing
		rollback=false, just like purge does. */
		btr_cur_pessimistic_delete(&err, FALSE, &pcur->btr_cur, 0,
					   false, &mtr);
		ut_ad(err == DB_SUCCESS || err == DB_OUT_OF_FILE_SPACE);
	} else if (!index->table->is_temporary() && node->new_trx_id) {
		/* We rolled back a record so that it still exists.
		We must reset the DB_TRX_ID if the history is no
		longer accessible by any active read view. */

		mtr.start();
		if (pcur->restore_position(BTR_MODIFY_LEAF, &mtr)
		    != btr_pcur_t::SAME_ALL) {
			goto mtr_commit_exit;
		}
		rec_t* rec = btr_pcur_get_rec(pcur);
		have_latch = true;
		purge_sys.latch.rd_lock(SRW_LOCK_CALL);
		if (!purge_sys.changes_visible(node->new_trx_id,
					       node->table->name)) {
			goto mtr_commit_exit;
		}

		ulint trx_id_offset = index->trx_id_offset;
		ulint trx_id_pos = index->n_uniq ? index->n_uniq : 1;
		/* Reserve enough offsets for the PRIMARY KEY and
		2 columns so that we can access DB_TRX_ID, DB_ROLL_PTR. */
		rec_offs offsets_[REC_OFFS_HEADER_SIZE + MAX_REF_PARTS + 2];
		if (trx_id_offset) {
#ifdef UNIV_DEBUG
			ut_ad(rec_offs_validate(NULL, index, offsets));
			if (buf_block_get_page_zip(
				    btr_pcur_get_block(&node->pcur))) {
				/* Below, page_zip_write_trx_id_and_roll_ptr()
				needs offsets to access DB_TRX_ID,DB_ROLL_PTR.
				We already computed offsets for possibly
				another record in the clustered index.
				Because the PRIMARY KEY is fixed-length,
				the offsets for the PRIMARY KEY and
				DB_TRX_ID,DB_ROLL_PTR are still valid.
				Silence the rec_offs_validate() assertion. */
				rec_offs_make_valid(rec, index, true, offsets);
			}
#endif
		} else if (rec_is_metadata(rec, *index)) {
			ut_ad(!buf_block_get_page_zip(btr_pcur_get_block(
							      pcur)));
			for (unsigned i = index->first_user_field(); i--; ) {
				trx_id_offset += index->fields[i].fixed_len;
			}
		} else {
			ut_ad(index->n_uniq <= MAX_REF_PARTS);
			rec_offs_init(offsets_);
			offsets = rec_get_offsets(rec, index, offsets_,
						  index->n_core_fields,
						  trx_id_pos + 2, &heap);
			ulint len;
			trx_id_offset = rec_get_nth_field_offs(
				offsets, trx_id_pos, &len);
			ut_ad(len == DATA_TRX_ID_LEN);
		}

		if (trx_read_trx_id(rec + trx_id_offset) == node->new_trx_id) {
			ut_ad(!rec_get_deleted_flag(
				      rec, dict_table_is_comp(node->table))
			      || rec_is_alter_metadata(rec, *index));
			index->set_modified(mtr);
			buf_block_t* block = btr_pcur_get_block(pcur);
			if (UNIV_LIKELY_NULL(block->page.zip.data)) {
				page_zip_write_trx_id_and_roll_ptr(
					block, rec, offsets, trx_id_pos,
					0, 1ULL << ROLL_PTR_INSERT_FLAG_POS,
					&mtr);
			} else {
				size_t offs = page_offset(rec + trx_id_offset);
				mtr.memset(block, offs, DATA_TRX_ID_LEN, 0);
				offs += DATA_TRX_ID_LEN;
				mtr.write<1,mtr_t::MAYBE_NOP>(*block,
							      block->page.frame
							      + offs, 0x80U);
				mtr.memset(block, offs + 1,
					   DATA_ROLL_PTR_LEN - 1, 0);
			}
		}
	} else {
		goto func_exit;
	}

mtr_commit_exit:
	if (have_latch) {
		purge_sys.latch.rd_unlock();
	}

	btr_pcur_commit_specify_mtr(pcur, &mtr);

func_exit:
	if (offsets_heap) {
		mem_heap_free(offsets_heap);
	}
	mem_heap_free(heap);
	return(err);
}

/***********************************************************//**
Delete marks or removes a secondary index entry if found.
@return DB_SUCCESS, DB_FAIL, or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_del_mark_or_remove_sec_low(
/*====================================*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr,	/*!< in: query thread */
	dict_index_t*	index,	/*!< in: index */
	dtuple_t*	entry,	/*!< in: index entry */
	ulint		mode)	/*!< in: latch mode BTR_MODIFY_LEAF or
				BTR_MODIFY_TREE */
{
	btr_pcur_t		pcur;
	btr_cur_t*		btr_cur;
	dberr_t			err	= DB_SUCCESS;
	mtr_t			mtr;
	mtr_t			mtr_vers;
	row_search_result	search_result;
	const bool		modify_leaf = mode == BTR_MODIFY_LEAF;

	row_mtr_start(&mtr, index, !modify_leaf);

	if (!index->is_committed()) {
		/* The index->online_status may change if the index is
		or was being created online, but not committed yet. It
		is protected by index->lock. */
		if (modify_leaf) {
			mode = BTR_MODIFY_LEAF | BTR_ALREADY_S_LATCHED;
			mtr_s_lock_index(index, &mtr);
		} else {
			ut_ad(mode == (BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE));
			mtr_sx_lock_index(index, &mtr);
		}
	} else {
		/* For secondary indexes,
		index->online_status==ONLINE_INDEX_COMPLETE if
		index->is_committed(). */
		ut_ad(!dict_index_is_online_ddl(index));
	}

	btr_cur = btr_pcur_get_btr_cur(&pcur);

	if (dict_index_is_spatial(index)) {
		if (modify_leaf) {
			btr_cur->thr = thr;
			mode |= BTR_RTREE_DELETE_MARK;
		}
		mode |= BTR_RTREE_UNDO_INS;
	}

	search_result = row_search_index_entry(index, entry, mode,
					       &pcur, &mtr);

	switch (UNIV_EXPECT(search_result, ROW_FOUND)) {
	case ROW_NOT_FOUND:
		/* In crash recovery, the secondary index record may
		be missing if the UPDATE did not have time to insert
		the secondary index records before the crash.  When we
		are undoing that UPDATE in crash recovery, the record
		may be missing.

		In normal processing, if an update ends in a deadlock
		before it has inserted all updated secondary index
		records, then the undo will not find those records. */
		goto func_exit;
	case ROW_FOUND:
		break;
	case ROW_BUFFERED:
	case ROW_NOT_DELETED_REF:
		/* These are invalid outcomes, because the mode passed
		to row_search_index_entry() did not include any of the
		flags BTR_INSERT, BTR_DELETE, or BTR_DELETE_MARK. */
		ut_error;
	}

	/* We should remove the index record if no prior version of the row,
	which cannot be purged yet, requires its existence. If some requires,
	we should delete mark the record. */

	mtr_vers.start();

	ut_a(node->pcur.restore_position(BTR_SEARCH_LEAF, &mtr_vers) ==
	      btr_pcur_t::SAME_ALL);

	/* For temporary table, we can skip to check older version of
	clustered index entry, because there is no MVCC or purge. */
	if (node->table->is_temporary()
	    || row_vers_old_has_index_entry(
		    false, btr_pcur_get_rec(&node->pcur),
		    &mtr_vers, index, entry, 0, 0)) {
		btr_rec_set_deleted<true>(btr_cur_get_block(btr_cur),
					  btr_cur_get_rec(btr_cur), &mtr);
	} else {
		/* Remove the index record */

		if (dict_index_is_spatial(index)) {
			rec_t*	rec = btr_pcur_get_rec(&pcur);
			if (rec_get_deleted_flag(rec,
						 dict_table_is_comp(index->table))) {
				ib::error() << "Record found in index "
					<< index->name << " is deleted marked"
					" on rollback update.";
				ut_ad(0);
			}
		}

		if (modify_leaf) {
			err = btr_cur_optimistic_delete(btr_cur, 0, &mtr)
				? DB_SUCCESS : DB_FAIL;
		} else {
			/* Passing rollback=false,
			because we are deleting a secondary index record:
			the distinction only matters when deleting a
			record that contains externally stored columns. */
			ut_ad(!index->is_primary());
			btr_cur_pessimistic_delete(&err, FALSE, btr_cur, 0,
						   false, &mtr);

			/* The delete operation may fail if we have little
			file space left: TODO: easiest to crash the database
			and restart with more file space */
		}
	}

	btr_pcur_commit_specify_mtr(&(node->pcur), &mtr_vers);

func_exit:
	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	return(err);
}

/***********************************************************//**
Delete marks or removes a secondary index entry if found.
NOTE that if we updated the fields of a delete-marked secondary index record
so that alphabetically they stayed the same, e.g., 'abc' -> 'aBc', we cannot
return to the original values because we do not know them. But this should
not cause problems because in row0sel.cc, in queries we always retrieve the
clustered index record or an earlier version of it, if the secondary index
record through which we do the search is delete-marked.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_del_mark_or_remove_sec(
/*================================*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr,	/*!< in: query thread */
	dict_index_t*	index,	/*!< in: index */
	dtuple_t*	entry)	/*!< in: index entry */
{
	dberr_t	err;

	err = row_undo_mod_del_mark_or_remove_sec_low(node, thr, index,
						      entry, BTR_MODIFY_LEAF);
	if (err == DB_SUCCESS) {

		return(err);
	}

	err = row_undo_mod_del_mark_or_remove_sec_low(node, thr, index,
		entry, BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE);
	return(err);
}

/***********************************************************//**
Delete unmarks a secondary index entry which must be found. It might not be
delete-marked at the moment, but it does not harm to unmark it anyway. We also
need to update the fields of the secondary index record if we updated its
fields but alphabetically they stayed the same, e.g., 'abc' -> 'aBc'.
@retval DB_SUCCESS on success
@retval DB_FAIL if BTR_MODIFY_TREE should be tried
@retval DB_OUT_OF_FILE_SPACE when running out of tablespace
@retval DB_DUPLICATE_KEY if the value was missing
	and an insert would lead to a duplicate exists */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_del_unmark_sec_and_undo_update(
/*========================================*/
	ulint		mode,	/*!< in: search mode: BTR_MODIFY_LEAF or
				BTR_MODIFY_TREE */
	que_thr_t*	thr,	/*!< in: query thread */
	dict_index_t*	index,	/*!< in: index */
	dtuple_t*	entry)	/*!< in: index entry */
{
	btr_pcur_t		pcur;
	btr_cur_t*		btr_cur		= btr_pcur_get_btr_cur(&pcur);
	upd_t*			update;
	dberr_t			err		= DB_SUCCESS;
	big_rec_t*		dummy_big_rec;
	mtr_t			mtr;
	trx_t*			trx		= thr_get_trx(thr);
	const ulint		flags
		= BTR_KEEP_SYS_FLAG | BTR_NO_LOCKING_FLAG;
	row_search_result	search_result;
	ulint			orig_mode = mode;

	ut_ad(trx->id != 0);

	if (dict_index_is_spatial(index)) {
		/* FIXME: Currently we do a 2-pass search for the undo
		due to avoid undel-mark a wrong rec in rolling back in
		partial update.  Later, we could log some info in
		secondary index updates to avoid this. */
		ut_ad(mode & BTR_MODIFY_LEAF);
		mode |= BTR_RTREE_DELETE_MARK;
	}

try_again:
	row_mtr_start(&mtr, index, !(mode & BTR_MODIFY_LEAF));

	btr_cur->thr = thr;

	search_result = row_search_index_entry(index, entry, mode,
					       &pcur, &mtr);

	switch (search_result) {
		mem_heap_t*	heap;
		mem_heap_t*	offsets_heap;
		rec_offs*	offsets;
	case ROW_BUFFERED:
	case ROW_NOT_DELETED_REF:
		/* These are invalid outcomes, because the mode passed
		to row_search_index_entry() did not include any of the
		flags BTR_INSERT, BTR_DELETE, or BTR_DELETE_MARK. */
		ut_error;
	case ROW_NOT_FOUND:
		/* For spatial index, if first search didn't find an
		undel-marked rec, try to find a del-marked rec. */
		if (dict_index_is_spatial(index) && btr_cur->rtr_info->fd_del) {
			if (mode != orig_mode) {
				mode = orig_mode;
				btr_pcur_close(&pcur);
				mtr_commit(&mtr);
				goto try_again;
			}
		}

		if (btr_cur->up_match >= dict_index_get_n_unique(index)
		    || btr_cur->low_match >= dict_index_get_n_unique(index)) {
			ib::warn() << "Record in index " << index->name
				<< " of table " << index->table->name
				<< " was not found on rollback, and"
				" a duplicate exists: "
				<< *entry
				<< " at: " << rec_index_print(
					btr_cur_get_rec(btr_cur), index);
			err = DB_DUPLICATE_KEY;
			break;
		}

		ib::warn() << "Record in index " << index->name
			<< " of table " << index->table->name
			<< " was not found on rollback, trying to insert: "
			<< *entry
			<< " at: " << rec_index_print(
				btr_cur_get_rec(btr_cur), index);

		/* Insert the missing record that we were trying to
		delete-unmark. */
		big_rec_t*	big_rec;
		rec_t*		insert_rec;
		offsets = NULL;
		offsets_heap = NULL;

		err = btr_cur_optimistic_insert(
			flags, btr_cur, &offsets, &offsets_heap,
			entry, &insert_rec, &big_rec,
			0, thr, &mtr);
		ut_ad(!big_rec);

		if (err == DB_FAIL && mode == BTR_MODIFY_TREE) {
			err = btr_cur_pessimistic_insert(
				flags, btr_cur,
				&offsets, &offsets_heap,
				entry, &insert_rec, &big_rec,
				0, thr, &mtr);
			/* There are no off-page columns in
			secondary indexes. */
			ut_ad(!big_rec);
		}

		if (err == DB_SUCCESS) {
			page_update_max_trx_id(
				btr_cur_get_block(btr_cur),
				btr_cur_get_page_zip(btr_cur),
				trx->id, &mtr);
		}

		if (offsets_heap) {
			mem_heap_free(offsets_heap);
		}

		break;
	case ROW_FOUND:
		btr_rec_set_deleted<false>(btr_cur_get_block(btr_cur),
					   btr_cur_get_rec(btr_cur), &mtr);
		heap = mem_heap_create(
			sizeof(upd_t)
			+ dtuple_get_n_fields(entry) * sizeof(upd_field_t));
		offsets_heap = NULL;
		offsets = rec_get_offsets(
			btr_cur_get_rec(btr_cur),
			index, nullptr, index->n_core_fields, ULINT_UNDEFINED,
			&offsets_heap);
		update = row_upd_build_sec_rec_difference_binary(
			btr_cur_get_rec(btr_cur), index, offsets, entry, heap);
		if (upd_get_n_fields(update) == 0) {

			/* Do nothing */

		} else if (mode != BTR_MODIFY_TREE) {
			/* Try an optimistic updating of the record, keeping
			changes within the page */

			/* TODO: pass offsets, not &offsets */
			err = btr_cur_optimistic_update(
				flags, btr_cur, &offsets, &offsets_heap,
				update, 0, thr, thr_get_trx(thr)->id, &mtr);
			switch (err) {
			case DB_OVERFLOW:
			case DB_UNDERFLOW:
			case DB_ZIP_OVERFLOW:
				err = DB_FAIL;
			default:
				break;
			}
		} else {
			err = btr_cur_pessimistic_update(
				flags, btr_cur, &offsets, &offsets_heap,
				heap, &dummy_big_rec,
				update, 0, thr, thr_get_trx(thr)->id, &mtr);
			ut_a(!dummy_big_rec);
		}

		mem_heap_free(heap);
		mem_heap_free(offsets_heap);
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	return(err);
}

/***********************************************************//**
Undoes a modify in secondary indexes when undo record type is UPD_DEL.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_upd_del_sec(
/*=====================*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	mem_heap_t*	heap;
	dberr_t		err	= DB_SUCCESS;

	ut_ad(node->rec_type == TRX_UNDO_UPD_DEL_REC);
	ut_ad(!node->undo_row);

	heap = mem_heap_create(1024);

	while (node->index != NULL) {
		dict_index_t*	index	= node->index;
		dtuple_t*	entry;

		if (index->type & DICT_FTS || !index->is_committed()) {
			dict_table_next_uncorrupted_index(node->index);
			continue;
		}

		/* During online index creation,
		HA_ALTER_INPLACE_COPY_NO_LOCK or HA_ALTER_INPLACE_NOCOPY_NO_LOCk
		should guarantee that any active transaction has not modified
		indexed columns such that col->ord_part was 0 at the
		time when the undo log record was written. When we get
		to roll back an undo log entry TRX_UNDO_DEL_MARK_REC,
		it should always cover all affected indexes. */
		entry = row_build_index_entry(
			node->row, node->ext, index, heap);

		if (UNIV_UNLIKELY(!entry)) {
			/* The database must have crashed after
			inserting a clustered index record but before
			writing all the externally stored columns of
			that record.  Because secondary index entries
			are inserted after the clustered index record,
			we may assume that the secondary index record
			does not exist.  However, this situation may
			only occur during the rollback of incomplete
			transactions. */
			ut_a(thr_get_trx(thr) == trx_roll_crash_recv_trx);
		} else {
			err = row_undo_mod_del_mark_or_remove_sec(
				node, thr, index, entry);

			if (UNIV_UNLIKELY(err != DB_SUCCESS)) {

				break;
			}
		}

		mem_heap_empty(heap);
		dict_table_next_uncorrupted_index(node->index);
	}

	mem_heap_free(heap);

	return(err);
}

/***********************************************************//**
Undoes a modify in secondary indexes when undo record type is DEL_MARK.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_del_mark_sec(
/*======================*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	mem_heap_t*	heap;
	dberr_t		err	= DB_SUCCESS;

	ut_ad(!node->undo_row);

	heap = mem_heap_create(1024);

	while (node->index != NULL) {
		dict_index_t*	index	= node->index;
		dtuple_t*	entry;

		if (index->type == DICT_FTS || !index->is_committed()) {
			dict_table_next_uncorrupted_index(node->index);
			continue;
		}

		/* During online index creation,
		HA_ALTER_INPLACE_COPY_NO_LOCK or HA_ALTER_INPLACE_NOCOPY_NO_LOCK
		should guarantee that any active transaction has not modified
		indexed columns such that col->ord_part was 0 at the
		time when the undo log record was written. When we get
		to roll back an undo log entry TRX_UNDO_DEL_MARK_REC,
		it should always cover all affected indexes. */
		entry = row_build_index_entry(
			node->row, node->ext, index, heap);

		ut_a(entry);

		err = row_undo_mod_del_unmark_sec_and_undo_update(
			BTR_MODIFY_LEAF, thr, index, entry);
		if (err == DB_FAIL) {
			err = row_undo_mod_del_unmark_sec_and_undo_update(
				BTR_MODIFY_TREE, thr, index, entry);
		}

		if (err == DB_DUPLICATE_KEY) {
			index->type |= DICT_CORRUPT;
			err = DB_SUCCESS;
			/* Do not return any error to the caller. The
			duplicate will be reported by ALTER TABLE or
			CREATE UNIQUE INDEX. Unfortunately we cannot
			report the duplicate key value to the DDL
			thread, because the altered_table object is
			private to its call stack. */
		} else if (err != DB_SUCCESS) {
			break;
		}

		mem_heap_empty(heap);
		dict_table_next_uncorrupted_index(node->index);
	}

	mem_heap_free(heap);

	return(err);
}

/***********************************************************//**
Undoes a modify in secondary indexes when undo record type is UPD_EXIST.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_undo_mod_upd_exist_sec(
/*=======================*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	mem_heap_t*	heap;
	dberr_t		err	= DB_SUCCESS;

	if (node->index == NULL
	    || ((node->cmpl_info & UPD_NODE_NO_ORD_CHANGE))) {
		/* No change in secondary indexes */

		return(err);
	}

	heap = mem_heap_create(1024);


	while (node->index != NULL) {

		if (!node->index->is_committed()) {
			dict_table_next_uncorrupted_index(node->index);
			continue;
		}

		dict_index_t*	index	= node->index;
		dtuple_t*	entry;

		if (dict_index_is_spatial(index)) {
			if (!row_upd_changes_ord_field_binary_func(
				index, node->update,
#ifdef UNIV_DEBUG
				thr,
#endif /* UNIV_DEBUG */
                                node->row,
				node->ext, ROW_BUILD_FOR_UNDO)) {
				dict_table_next_uncorrupted_index(node->index);
				continue;
			}
		} else {
			if (index->type == DICT_FTS
			    || !row_upd_changes_ord_field_binary(index,
								 node->update,
								 thr, node->row,
								 node->ext)) {
				dict_table_next_uncorrupted_index(node->index);
				continue;
			}
		}

		/* Build the newest version of the index entry */
		entry = row_build_index_entry(node->row, node->ext,
					      index, heap);
		if (UNIV_UNLIKELY(!entry)) {
			/* The server must have crashed in
			row_upd_clust_rec_by_insert() before
			the updated externally stored columns (BLOBs)
			of the new clustered index entry were written. */

			/* The table must be in DYNAMIC or COMPRESSED
			format.  REDUNDANT and COMPACT formats
			store a local 768-byte prefix of each
			externally stored column. */
			ut_a(dict_table_has_atomic_blobs(index->table));

			/* This is only legitimate when
			rolling back an incomplete transaction
			after crash recovery. */
			ut_a(thr_get_trx(thr)->is_recovered);

			/* The server must have crashed before
			completing the insert of the new
			clustered index entry and before
			inserting to the secondary indexes.
			Because node->row was not yet written
			to this index, we can ignore it.  But
			we must restore node->undo_row. */
		} else {
			/* NOTE that if we updated the fields of a
			delete-marked secondary index record so that
			alphabetically they stayed the same, e.g.,
			'abc' -> 'aBc', we cannot return to the
			original values because we do not know them.
			But this should not cause problems because
			in row0sel.cc, in queries we always retrieve
			the clustered index record or an earlier
			version of it, if the secondary index record
			through which we do the search is
			delete-marked. */

			err = row_undo_mod_del_mark_or_remove_sec(
				node, thr, index, entry);
			if (err != DB_SUCCESS) {
				break;
			}
		}

		mem_heap_empty(heap);
		/* We may have to update the delete mark in the
		secondary index record of the previous version of
		the row. We also need to update the fields of
		the secondary index record if we updated its fields
		but alphabetically they stayed the same, e.g.,
		'abc' -> 'aBc'. */
		if (dict_index_is_spatial(index)) {
			entry = row_build_index_entry_low(node->undo_row,
							  node->undo_ext,
							  index, heap,
							  ROW_BUILD_FOR_UNDO);
		} else {
			entry = row_build_index_entry(node->undo_row,
						      node->undo_ext,
						      index, heap);
		}

		ut_a(entry);

		err = row_undo_mod_del_unmark_sec_and_undo_update(
			BTR_MODIFY_LEAF, thr, index, entry);
		if (err == DB_FAIL) {
			err = row_undo_mod_del_unmark_sec_and_undo_update(
				BTR_MODIFY_TREE, thr, index, entry);
		}

		if (err == DB_DUPLICATE_KEY) {
			index->type |= DICT_CORRUPT;
			err = DB_SUCCESS;
		} else if (err != DB_SUCCESS) {
			break;
		}

		mem_heap_empty(heap);
		dict_table_next_uncorrupted_index(node->index);
	}

	mem_heap_free(heap);

	return(err);
}

/** Parse an update undo record.
@param[in,out]	node		row rollback state
@param[in]	dict_locked	whether the data dictionary cache is locked */
static bool row_undo_mod_parse_undo_rec(undo_node_t* node, bool dict_locked)
{
	dict_index_t*	clust_index;
	byte*		ptr;
	undo_no_t	undo_no;
	table_id_t	table_id;
	trx_id_t	trx_id;
	roll_ptr_t	roll_ptr;
	byte		info_bits;
	ulint		type;
	ulint		cmpl_info;
	bool		dummy_extern;

	ut_ad(node->state == UNDO_UPDATE_PERSISTENT
	      || node->state == UNDO_UPDATE_TEMPORARY);
	ut_ad(node->trx->in_rollback);
	ut_ad(!trx_undo_roll_ptr_is_insert(node->roll_ptr));

	ptr = trx_undo_rec_get_pars(node->undo_rec, &type, &cmpl_info,
				    &dummy_extern, &undo_no, &table_id);
	node->rec_type = type;

	if (node->state == UNDO_UPDATE_PERSISTENT) {
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

	ut_ad(!node->table->skip_alter_undo);

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
	}

	clust_index = dict_table_get_first_index(node->table);

	ptr = trx_undo_update_rec_get_sys_cols(ptr, &trx_id, &roll_ptr,
					       &info_bits);

	ptr = trx_undo_rec_get_row_ref(ptr, clust_index, &(node->ref),
				       node->heap);

	ptr = trx_undo_update_rec_get_update(ptr, clust_index, type, trx_id,
				       roll_ptr, info_bits,
				       node->heap, &(node->update));
	node->new_trx_id = trx_id;
	node->cmpl_info = cmpl_info;
	ut_ad(!node->ref->info_bits);

	if (node->update->info_bits & REC_INFO_MIN_REC_FLAG) {
		if ((node->update->info_bits & ~REC_INFO_DELETED_FLAG)
		    != REC_INFO_MIN_REC_FLAG) {
			ut_ad("wrong info_bits in undo log record" == 0);
			goto close_table;
		}
		/* This must be an undo log record for a subsequent
		instant ALTER TABLE, extending the metadata record. */
		ut_ad(clust_index->is_instant());
		ut_ad(clust_index->table->instant
		      || !(node->update->info_bits & REC_INFO_DELETED_FLAG));
		node->ref = &trx_undo_metadata;
		node->update->info_bits = (node->update->info_bits
					   & REC_INFO_DELETED_FLAG)
			? REC_INFO_METADATA_ALTER
			: REC_INFO_METADATA_ADD;
	}

	if (!row_undo_search_clust_to_pcur(node)) {
		/* As long as this rolling-back transaction exists,
		the PRIMARY KEY value pointed to by the undo log
		record should exist.

		However, if InnoDB is killed during a rollback, or
		shut down during the rollback of recovered
		transactions, then after restart we may try to roll
		back some of the same undo log records again, because
		trx_roll_try_truncate() is not being invoked after
		every undo log record.

		It is also possible that the record
		was not modified yet (the DB_ROLL_PTR does not match
		node->roll_ptr) and thus there is nothing to roll back.

		btr_cur_upd_lock_and_undo() only writes the undo log
		record after successfully acquiring an exclusive lock
		on the the clustered index record. That lock will not
		be released before the transaction is committed or
		fully rolled back. (Exception: if the server was
		killed, restarted, and shut down again before the
		rollback of the recovered transaction was completed,
		it is possible that the transaction was partially
		rolled back and locks released.) */
		goto close_table;
	}

	/* Extract indexed virtual columns from undo log */
	if (node->ref != &trx_undo_metadata && node->table->n_v_cols) {
		row_upd_replace_vcol(node->row, node->table,
				     node->update, false, node->undo_row,
				     (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)
					? NULL : ptr);
	}

	return true;
}

/***********************************************************//**
Undoes a modify operation on a row of a table.
@return DB_SUCCESS or error code */
dberr_t
row_undo_mod(
/*=========*/
	undo_node_t*	node,	/*!< in: row undo node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t	err;
	ut_ad(thr_get_trx(thr) == node->trx);
	const bool dict_locked = node->trx->dict_operation_lock_mode;

	if (!row_undo_mod_parse_undo_rec(node, dict_locked)) {
		return DB_SUCCESS;
	}

	ut_ad(node->table->is_temporary()
	      || lock_table_has_locks(node->table));
	node->index = dict_table_get_first_index(node->table);
	ut_ad(dict_index_is_clust(node->index));

	if (node->ref->info_bits) {
		ut_ad(node->ref->is_metadata());
		goto rollback_clust;
	}

	/* Skip the clustered index (the first index) */
	node->index = dict_table_get_next_index(node->index);

	/* Skip all corrupted secondary index */
	dict_table_skip_corrupt_index(node->index);

	switch (node->rec_type) {
	case TRX_UNDO_UPD_EXIST_REC:
		err = row_undo_mod_upd_exist_sec(node, thr);
		break;
	case TRX_UNDO_DEL_MARK_REC:
		err = row_undo_mod_del_mark_sec(node, thr);
		break;
	case TRX_UNDO_UPD_DEL_REC:
		err = row_undo_mod_upd_del_sec(node, thr);
		break;
	default:
		ut_error;
		err = DB_ERROR;
	}

	if (err == DB_SUCCESS) {
rollback_clust:
		err = row_undo_mod_clust(node, thr);

		bool update_statistics
			= !(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE);

		if (err == DB_SUCCESS && node->table->stat_initialized) {
			switch (node->rec_type) {
			case TRX_UNDO_UPD_EXIST_REC:
				break;
			case TRX_UNDO_DEL_MARK_REC:
				dict_table_n_rows_inc(node->table);
				update_statistics = update_statistics
					|| !srv_stats_include_delete_marked;
				break;
			case TRX_UNDO_UPD_DEL_REC:
				dict_table_n_rows_dec(node->table);
				update_statistics = update_statistics
					|| !srv_stats_include_delete_marked;
				break;
			}

			/* Do not attempt to update statistics when
			executing ROLLBACK in the InnoDB SQL
			interpreter, because in that case we would
			already be holding dict_sys.latch, which
			would be acquired when updating statistics. */
			if (update_statistics && !dict_locked) {
				dict_stats_update_if_needed(node->table,
							    *node->trx);
			} else {
				node->table->stat_modified_counter++;
			}
		}
	}

	dict_table_close(node->table, dict_locked);

	node->table = NULL;

	return(err);
}
