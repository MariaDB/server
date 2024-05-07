/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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
@file row/row0purge.cc
Purge obsolete records

Created 3/14/1997 Heikki Tuuri
*******************************************************/

#include "row0purge.h"
#include "btr0cur.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "dict0crea.h"
#include "dict0stats.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "row0upd.h"
#include "row0vers.h"
#include "row0mysql.h"
#include "log0log.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "handler.h"
#include "ha_innodb.h"
#include "fil0fil.h"
#include "debug_sync.h"
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

/***********************************************************//**
Repositions the pcur in the purge node on the clustered index record,
if found. If the record is not found, close pcur.
@return TRUE if the record was found */
static
ibool
row_purge_reposition_pcur(
/*======================*/
	btr_latch_mode	mode,	/*!< in: latching mode */
	purge_node_t*	node,	/*!< in: row purge node */
	mtr_t*		mtr)	/*!< in: mtr */
{
	if (node->found_clust) {
		ut_ad(node->validate_pcur());

		node->found_clust =
		  node->pcur.restore_position(mode, mtr) ==
		    btr_pcur_t::SAME_ALL;

	} else {
		node->found_clust = row_search_on_row_ref(
			&node->pcur, mode, node->table, node->ref, mtr);

		if (node->found_clust) {
			btr_pcur_store_position(&node->pcur, mtr);
		}
	}

	/* Close the current cursor if we fail to position it correctly. */
	if (!node->found_clust) {
		btr_pcur_close(&node->pcur);
	}

	return(node->found_clust);
}

/***********************************************************//**
Removes a delete marked clustered index record if possible.
@retval true if the row was not found, or it was successfully removed
@retval false if the row was modified after the delete marking */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
row_purge_remove_clust_if_poss_low(
/*===============================*/
	purge_node_t*	node,	/*!< in/out: row purge node */
	btr_latch_mode	mode)	/*!< in: BTR_MODIFY_LEAF or BTR_PURGE_TREE */
{
	dict_index_t* index = dict_table_get_first_index(node->table);
	table_id_t table_id = 0;
	index_id_t index_id = 0;
	dict_table_t *table = nullptr;
	pfs_os_file_t f = OS_FILE_CLOSED;

	if (table_id) {
retry:
		dict_sys.lock(SRW_LOCK_CALL);
		table = dict_sys.find_table(table_id);
		if (!table) {
			dict_sys.unlock();
		} else if (table->n_rec_locks) {
			for (dict_index_t* ind = UT_LIST_GET_FIRST(
				     table->indexes); ind;
			     ind = UT_LIST_GET_NEXT(indexes, ind)) {
				if (ind->id == index_id) {
					lock_discard_for_index(*ind);
				}
			}
		}
	}
	mtr_t mtr;
	mtr.start();
	index->set_modified(mtr);
	log_free_check();
	bool success = true;

	if (!row_purge_reposition_pcur(mode, node, &mtr)) {
		/* The record was already removed. */
removed:
		mtr.commit();
close_and_exit:
		if (table) {
			dict_sys.unlock();
		}
		return success;
	}

	if (node->table->id == DICT_INDEXES_ID) {
		/* If this is a record of the SYS_INDEXES table, then
		we have to free the file segments of the index tree
		associated with the index */
		if (!table_id) {
			const rec_t* rec = btr_pcur_get_rec(&node->pcur);

			table_id = mach_read_from_8(rec);
			index_id = mach_read_from_8(rec + 8);
			if (table_id) {
				mtr.commit();
				goto retry;
			}
			ut_ad("corrupted SYS_INDEXES record" == 0);
		}

		const uint32_t space_id = dict_drop_index_tree(
			&node->pcur, nullptr, &mtr);
		if (space_id) {
			if (table) {
				if (table->get_ref_count() == 0) {
					dict_sys.remove(table);
				} else if (table->space_id == space_id) {
					table->space = nullptr;
					table->file_unreadable = true;
				}
				dict_sys.unlock();
				table = nullptr;
			}
			f = fil_delete_tablespace(space_id);
		}

		mtr.commit();

		if (table) {
			dict_sys.unlock();
			table = nullptr;
		}

		mtr.start();
		index->set_modified(mtr);

		if (!row_purge_reposition_pcur(mode, node, &mtr)) {
			goto removed;
		}
	}

	rec_t* rec = btr_pcur_get_rec(&node->pcur);
	rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets_);
	mem_heap_t* heap = NULL;
	rec_offs* offsets = rec_get_offsets(rec, index, offsets_,
					    index->n_core_fields,
					    ULINT_UNDEFINED, &heap);

	if (node->roll_ptr != row_get_rec_roll_ptr(rec, index, offsets)) {
		/* Someone else has modified the record later: do not remove */
		goto func_exit;
	}

	ut_ad(rec_get_deleted_flag(rec, rec_offs_comp(offsets)));
	/* In delete-marked records, DB_TRX_ID must
	always refer to an existing undo log record. */
	ut_ad(row_get_rec_trx_id(rec, index, offsets));

	if (mode == BTR_MODIFY_LEAF) {
		success = DB_FAIL != btr_cur_optimistic_delete(
			btr_pcur_get_btr_cur(&node->pcur), 0, &mtr);
	} else {
		dberr_t	err;
		ut_ad(mode == BTR_PURGE_TREE);
		btr_cur_pessimistic_delete(
			&err, FALSE, btr_pcur_get_btr_cur(&node->pcur), 0,
			false, &mtr);
		success = err == DB_SUCCESS;
	}

func_exit:
	if (heap) {
		mem_heap_free(heap);
	}

	/* Persistent cursor is closed if reposition fails. */
	if (node->found_clust) {
		btr_pcur_commit_specify_mtr(&node->pcur, &mtr);
	} else {
		mtr_commit(&mtr);
	}

	goto close_and_exit;
}

/***********************************************************//**
Removes a clustered index record if it has not been modified after the delete
marking.
@retval true if the row was not found, or it was successfully removed
@retval false the purge needs to be suspended because of running out
of file space. */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
row_purge_remove_clust_if_poss(
/*===========================*/
	purge_node_t*	node)	/*!< in/out: row purge node */
{
	if (row_purge_remove_clust_if_poss_low(node, BTR_MODIFY_LEAF)) {
		return(true);
	}

	for (ulint n_tries = 0;
	     n_tries < BTR_CUR_RETRY_DELETE_N_TIMES;
	     n_tries++) {
		if (row_purge_remove_clust_if_poss_low(node, BTR_PURGE_TREE)) {
			return(true);
		}

		std::this_thread::sleep_for(BTR_CUR_RETRY_SLEEP_TIME);
	}

	return(false);
}

/** Determines if it is possible to remove a secondary index entry.
Removal is possible if the secondary index entry does not refer to any
not delete marked version of a clustered index record where DB_TRX_ID
is newer than the purge view.

NOTE: This function should only be called by the purge thread, only
while holding a latch on the leaf page of the secondary index entry.
It is possible that this function first returns true and then false,
if a user transaction inserts a record that the secondary index entry
would refer to.
However, in that case, the user transaction would also re-insert the
secondary index entry after purge has removed it and released the leaf
page latch.
@param[in,out]	node		row purge node
@param[in]	index		secondary index
@param[in]	entry		secondary index entry
@param[in,out]	sec_pcur	secondary index cursor or NULL
				if it is called for purge buffering
				operation.
@param[in,out]	sec_mtr		mini-transaction which holds
				secondary index entry or NULL if it is
				called for purge buffering operation.
@param[in]	is_tree		true=pessimistic purge,
				false=optimistic (leaf-page only)
@return true if the secondary index record can be purged */
static
bool
row_purge_poss_sec(
	purge_node_t*	node,
	dict_index_t*	index,
	const dtuple_t*	entry,
	btr_pcur_t*	sec_pcur,
	mtr_t*		sec_mtr,
	bool		is_tree)
{
	bool	can_delete;
	mtr_t	mtr;

	ut_ad(!dict_index_is_clust(index));

	mtr_start(&mtr);

	can_delete = !row_purge_reposition_pcur(BTR_SEARCH_LEAF, node, &mtr)
		|| !row_vers_old_has_index_entry(true,
						 btr_pcur_get_rec(&node->pcur),
						 &mtr, index, entry,
						 node->roll_ptr, node->trx_id);

	/* Persistent cursor is closed if reposition fails. */
	if (node->found_clust) {
		btr_pcur_commit_specify_mtr(&node->pcur, &mtr);
	} else {
		mtr.commit();
	}

	ut_ad(mtr.has_committed());

	return can_delete;
}

/***************************************************************
Removes a secondary index entry if possible, by modifying the
index tree.  Does not try to buffer the delete.
@return TRUE if success or if not found */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
ibool
row_purge_remove_sec_if_poss_tree(
/*==============================*/
	purge_node_t*	node,	/*!< in: row purge node */
	dict_index_t*	index,	/*!< in: index */
	const dtuple_t*	entry)	/*!< in: index entry */
{
	btr_pcur_t		pcur;
	ibool			success	= TRUE;
	dberr_t			err;
	mtr_t			mtr;

	log_free_check();
	mtr.start();
	index->set_modified(mtr);
	pcur.btr_cur.page_cur.index = index;

	if (index->is_spatial()) {
		if (rtr_search(entry, BTR_PURGE_TREE, &pcur, nullptr, &mtr)) {
			goto func_exit;
		}
	} else if (!row_search_index_entry(entry, BTR_PURGE_TREE,
					   &pcur, &mtr)) {
		/* Not found.  This is a legitimate condition.  In a
		rollback, InnoDB will remove secondary recs that would
		be purged anyway.  Then the actual purge will not find
		the secondary index record.  Also, the purge itself is
		eager: if it comes to consider a secondary index
		record, and notices it does not need to exist in the
		index, it will remove it.  Then if/when the purge
		comes to consider the secondary index record a second
		time, it will not exist any more in the index. */
		goto func_exit;
	}

	/* We should remove the index record if no later version of the row,
	which cannot be purged yet, requires its existence. If some requires,
	we should do nothing. */

	if (row_purge_poss_sec(node, index, entry, &pcur, &mtr, true)) {

		/* Remove the index record, which should have been
		marked for deletion. */
		if (!rec_get_deleted_flag(btr_cur_get_rec(
						btr_pcur_get_btr_cur(&pcur)),
					  dict_table_is_comp(index->table))) {
			ib::error()
				<< "tried to purge non-delete-marked record"
				" in index " << index->name
				<< " of table " << index->table->name
				<< ": tuple: " << *entry
				<< ", record: " << rec_index_print(
					btr_cur_get_rec(
						btr_pcur_get_btr_cur(&pcur)),
					index);

			ut_ad(0);

			goto func_exit;
		}

		btr_cur_pessimistic_delete(&err, FALSE,
					   btr_pcur_get_btr_cur(&pcur),
					   0, false, &mtr);
		switch (UNIV_EXPECT(err, DB_SUCCESS)) {
		case DB_SUCCESS:
			break;
		case DB_OUT_OF_FILE_SPACE:
			success = FALSE;
			break;
		default:
			ut_error;
		}
	}

func_exit:
	btr_pcur_close(&pcur); // FIXME: need this?
	mtr.commit();

	return(success);
}

/***************************************************************
Removes a secondary index entry without modifying the index tree,
if possible.
@retval true if success or if not found
@retval false if row_purge_remove_sec_if_poss_tree() should be invoked */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
row_purge_remove_sec_if_poss_leaf(
/*==============================*/
	purge_node_t*	node,	/*!< in: row purge node */
	dict_index_t*	index,	/*!< in: index */
	const dtuple_t*	entry)	/*!< in: index entry */
{
	mtr_t			mtr;
	btr_pcur_t		pcur;
	bool			success	= true;

	log_free_check();
	ut_ad(index->table == node->table);
	ut_ad(!index->table->is_temporary());
	mtr.start();
	index->set_modified(mtr);

	pcur.btr_cur.page_cur.index = index;

	if (index->is_spatial()) {
		if (!rtr_search(entry, BTR_MODIFY_LEAF, &pcur, nullptr,
				&mtr)) {
			goto found;
		}
	} else if (btr_pcur_open(entry, PAGE_CUR_LE, BTR_MODIFY_LEAF, &pcur,
				 &mtr)
		   == DB_SUCCESS
		   && !btr_pcur_is_before_first_on_page(&pcur)
		   && btr_pcur_get_low_match(&pcur)
		   == dtuple_get_n_fields(entry)) {
found:
		/* Before attempting to purge a record, check
		if it is safe to do so. */
		if (row_purge_poss_sec(node, index, entry, &pcur, &mtr, false)) {
			btr_cur_t* btr_cur = btr_pcur_get_btr_cur(&pcur);

			/* Only delete-marked records should be purged. */
			if (!rec_get_deleted_flag(
				btr_cur_get_rec(btr_cur),
				dict_table_is_comp(index->table))) {

				ib::error()
					<< "tried to purge non-delete-marked"
					" record" " in index " << index->name
					<< " of table " << index->table->name
					<< ": tuple: " << *entry
					<< ", record: "
					<< rec_index_print(
						btr_cur_get_rec(btr_cur),
						index);
				mtr.commit();
				dict_set_corrupted(index, "purge");
				goto cleanup;
			}

			if (index->is_spatial()) {
				const buf_block_t* block = btr_cur_get_block(
					btr_cur);
                                const page_id_t id{block->page.id()};

				if (id.page_no() != index->page
				    && page_get_n_recs(block->page.frame) < 2
				    && !lock_test_prdt_page_lock(nullptr, id)){
					/* this is the last record on page,
					and it has a "page" lock on it,
					which mean search is still depending
					on it, so do not delete */
					DBUG_LOG("purge",
						 "skip purging last"
						 " record on page " << id);
					goto func_exit;
				}
			}

			success = btr_cur_optimistic_delete(btr_cur, 0, &mtr)
				!= DB_FAIL;
		}
	}

func_exit:
	mtr.commit();
cleanup:
	btr_pcur_close(&pcur);
	return success;
}

/***********************************************************//**
Removes a secondary index entry if possible. */
UNIV_INLINE MY_ATTRIBUTE((nonnull(1,2)))
void
row_purge_remove_sec_if_poss(
/*=========================*/
	purge_node_t*	node,	/*!< in: row purge node */
	dict_index_t*	index,	/*!< in: index */
	const dtuple_t*	entry)	/*!< in: index entry */
{
	ibool	success;
	ulint	n_tries		= 0;

	/*	fputs("Purge: Removing secondary record\n", stderr); */

	if (!entry) {
		/* The node->row must have lacked some fields of this
		index. This is possible when the undo log record was
		written before this index was created. */
		return;
	}

	if (row_purge_remove_sec_if_poss_leaf(node, index, entry)) {

		return;
	}
retry:
	success = row_purge_remove_sec_if_poss_tree(node, index, entry);
	/* The delete operation may fail if we have little
	file space left: TODO: easiest to crash the database
	and restart with more file space */

	if (!success && n_tries < BTR_CUR_RETRY_DELETE_N_TIMES) {

		n_tries++;

		std::this_thread::sleep_for(BTR_CUR_RETRY_SLEEP_TIME);

		goto retry;
	}

	ut_a(success);
}

/***********************************************************//**
Purges a delete marking of a record.
@retval true if the row was not found, or it was successfully removed
@retval false the purge needs to be suspended because of
running out of file space */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool row_purge_del_mark(purge_node_t *node)
{
  if (node->index)
  {
    mem_heap_t *heap= mem_heap_create(1024);

    do
    {
      if (node->index->type & (DICT_FTS | DICT_CORRUPT))
        continue;
      if (!node->index->is_committed())
        continue;
      dtuple_t* entry= row_build_index_entry_low(node->row, nullptr,
                                                 node->index, heap,
                                                 ROW_BUILD_FOR_PURGE);
      row_purge_remove_sec_if_poss(node, node->index, entry);
      mem_heap_empty(heap);
    }
    while ((node->index= dict_table_get_next_index(node->index)));

    mem_heap_free(heap);
  }

  bool result= row_purge_remove_clust_if_poss(node);

#ifdef ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("enable_row_purge_del_mark_exit_sync_point",
                  debug_sync_set_action
                  (current_thd,
                   STRING_WITH_LEN("now SIGNAL row_purge_del_mark_finished"));
                  );
#endif

  return result;
}

/** Reset DB_TRX_ID, DB_ROLL_PTR of a clustered index record
whose old history can no longer be observed.
@param[in,out]	node	purge node
@param[in,out]	mtr	mini-transaction (will be started and committed) */
static void row_purge_reset_trx_id(purge_node_t* node, mtr_t* mtr)
{
	/* Reset DB_TRX_ID, DB_ROLL_PTR for old records. */
	mtr->start();

	if (row_purge_reposition_pcur(BTR_MODIFY_LEAF, node, mtr)) {
		dict_index_t*	index = dict_table_get_first_index(
			node->table);
		ulint	trx_id_pos = index->n_uniq ? index->n_uniq : 1;
		rec_t*	rec = btr_pcur_get_rec(&node->pcur);
		mem_heap_t*	heap = NULL;
		/* Reserve enough offsets for the PRIMARY KEY and 2 columns
		so that we can access DB_TRX_ID, DB_ROLL_PTR. */
		rec_offs offsets_[REC_OFFS_HEADER_SIZE + MAX_REF_PARTS + 2];
		rec_offs_init(offsets_);
		rec_offs*	offsets = rec_get_offsets(
			rec, index, offsets_, index->n_core_fields,
			trx_id_pos + 2, &heap);
		ut_ad(heap == NULL);

		ut_ad(dict_index_get_nth_field(index, trx_id_pos)
		      ->col->mtype == DATA_SYS);
		ut_ad(dict_index_get_nth_field(index, trx_id_pos)
		      ->col->prtype == (DATA_TRX_ID | DATA_NOT_NULL));
		ut_ad(dict_index_get_nth_field(index, trx_id_pos + 1)
		      ->col->mtype == DATA_SYS);
		ut_ad(dict_index_get_nth_field(index, trx_id_pos + 1)
		      ->col->prtype == (DATA_ROLL_PTR | DATA_NOT_NULL));

		/* Only update the record if DB_ROLL_PTR matches (the
		record has not been modified after this transaction
		became purgeable) */
		if (node->roll_ptr
		    == row_get_rec_roll_ptr(rec, index, offsets)) {
			ut_ad(!rec_get_deleted_flag(
					rec, rec_offs_comp(offsets))
			      || rec_is_alter_metadata(rec, *index));
			DBUG_LOG("purge", "reset DB_TRX_ID="
				 << ib::hex(row_get_rec_trx_id(
						    rec, index, offsets)));

			index->set_modified(*mtr);
			buf_block_t* block = btr_pcur_get_block(&node->pcur);
			if (UNIV_LIKELY_NULL(block->page.zip.data)) {
				page_zip_write_trx_id_and_roll_ptr(
					block, rec, offsets, trx_id_pos,
					0, 1ULL << ROLL_PTR_INSERT_FLAG_POS,
					mtr);
			} else {
				ulint	len;
				byte*	ptr = rec_get_nth_field(
					rec, offsets, trx_id_pos, &len);
				ut_ad(len == DATA_TRX_ID_LEN);
				size_t offs = page_offset(ptr);
				mtr->memset(block, offs, DATA_TRX_ID_LEN, 0);
				offs += DATA_TRX_ID_LEN;
				mtr->write<1,mtr_t::MAYBE_NOP>(
					*block, block->page.frame + offs,
					0x80U);
				mtr->memset(block, offs + 1,
					    DATA_ROLL_PTR_LEN - 1, 0);
			}
		}
	}

	mtr->commit();
}

/***********************************************************//**
Purges an update of an existing record. Also purges an update of a delete
marked record if that record contained an externally stored field. */
static
void
row_purge_upd_exist_or_extern_func(
/*===============================*/
#ifdef UNIV_DEBUG
	const que_thr_t*thr,		/*!< in: query thread */
#endif /* UNIV_DEBUG */
	purge_node_t*	node,		/*!< in: row purge node */
	const trx_undo_rec_t*	undo_rec)	/*!< in: record to purge */
{
	mem_heap_t*	heap;

	ut_ad(!node->table->skip_alter_undo);

	if (node->rec_type == TRX_UNDO_UPD_DEL_REC
	    || (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)
	    || !node->index) {

		goto skip_secondaries;
	}

	heap = mem_heap_create(1024);

	do {
		if (node->index->type & (DICT_FTS | DICT_CORRUPT)) {
			continue;
		}

		if (!node->index->is_committed()) {
			continue;
		}

		if (row_upd_changes_ord_field_binary(node->index, node->update,
						     thr, NULL, NULL)) {
			/* Build the older version of the index entry */
			dtuple_t*	entry = row_build_index_entry_low(
				node->row, NULL, node->index,
				heap, ROW_BUILD_FOR_PURGE);
			row_purge_remove_sec_if_poss(node, node->index, entry);

			ut_ad(node->table);

			mem_heap_empty(heap);
		}
	} while ((node->index = dict_table_get_next_index(node->index)));

	mem_heap_free(heap);

skip_secondaries:
	mtr_t		mtr;
	dict_index_t*	index = dict_table_get_first_index(node->table);
	/* Free possible externally stored fields */
	for (ulint i = 0; i < upd_get_n_fields(node->update); i++) {

		const upd_field_t*	ufield
			= upd_get_nth_field(node->update, i);

		if (dfield_is_ext(&ufield->new_val)) {
			bool		is_insert;
			ulint		rseg_id;
			uint32_t	page_no;
			uint16_t	offset;

			/* We use the fact that new_val points to
			undo_rec and get thus the offset of
			dfield data inside the undo record. Then we
			can calculate from node->roll_ptr the file
			address of the new_val data */

			const uint16_t internal_offset = uint16_t(
				static_cast<const byte*>
				(dfield_get_data(&ufield->new_val))
				- undo_rec);

			ut_a(internal_offset < srv_page_size);

			trx_undo_decode_roll_ptr(node->roll_ptr,
						 &is_insert, &rseg_id,
						 &page_no, &offset);

			const trx_rseg_t &rseg = trx_sys.rseg_array[rseg_id];
			ut_ad(rseg.is_persistent());

			mtr.start();

			/* We have to acquire an SX-latch to the clustered
			index tree (exclude other tree changes) */

			mtr_sx_lock_index(index, &mtr);

			index->set_modified(mtr);

			/* NOTE: we must also acquire a U latch to the
			root page of the tree. We will need it when we
			free pages from the tree. If the tree is of height 1,
			the tree X-latch does NOT protect the root page,
			because it is also a leaf page. Since we will have a
			latch on an undo log page, we would break the
			latching order if we would only later latch the
			root page of such a tree! */

			dberr_t err;
			if (!btr_root_block_get(index, RW_SX_LATCH, &mtr,
						&err)) {
			} else if (buf_block_t* block =
				   buf_page_get(page_id_t(rseg.space->id,
							  page_no),
						0, RW_X_LATCH, &mtr)) {
				buf_page_make_young_if_needed(&block->page);

				byte* data_field = block->page.frame
					+ offset + internal_offset;

				ut_a(dfield_get_len(&ufield->new_val)
				     >= BTR_EXTERN_FIELD_REF_SIZE);
				btr_free_externally_stored_field(
					index,
					data_field
					+ dfield_get_len(&ufield->new_val)
					- BTR_EXTERN_FIELD_REF_SIZE,
					NULL, NULL, block, 0, false, &mtr);
			}

			mtr.commit();
		}
	}

	row_purge_reset_trx_id(node, &mtr);
}

#ifdef UNIV_DEBUG
# define row_purge_upd_exist_or_extern(thr,node,undo_rec)	\
	row_purge_upd_exist_or_extern_func(thr,node,undo_rec)
#else /* UNIV_DEBUG */
# define row_purge_upd_exist_or_extern(thr,node,undo_rec)	\
	row_purge_upd_exist_or_extern_func(node,undo_rec)
#endif /* UNIV_DEBUG */

/** Build a partial row from an update undo log record for purge.
Any columns which occur as ordering in any index of the table are present.
Any missing columns are indicated by col->mtype == DATA_MISSING.

@param ptr    remaining part of the undo log record
@param index  clustered index
@param node   purge node
@return pointer to remaining part of undo record */
static byte *row_purge_get_partial(const byte *ptr, const dict_index_t &index,
                                   purge_node_t *node)
{
  bool first_v_col= true;
  bool is_undo_log= true;

  ut_ad(index.is_primary());
  ut_ad(index.n_uniq == node->ref->n_fields);

  node->row= dtuple_create_with_vcol(node->heap, index.table->n_cols,
                                     index.table->n_v_cols);

  /* Mark all columns in the row uninitialized, so that
  we can distinguish missing fields from fields that are SQL NULL. */
  for (ulint i= 0; i < index.table->n_cols; i++)
    node->row->fields[i].type.mtype= DATA_MISSING;

  dtuple_init_v_fld(node->row);

  for (const upd_field_t *uf= node->update->fields, *const ue=
         node->update->fields + node->update->n_fields; uf != ue; uf++)
  {
    if (!uf->old_v_val)
    {
      const dict_col_t &c= *dict_index_get_nth_col(&index, uf->field_no);
      if (!c.is_dropped())
        node->row->fields[c.ind]= uf->new_val;
    }
  }

  const byte *end_ptr= ptr + mach_read_from_2(ptr);
  ptr+= 2;

  while (ptr != end_ptr)
  {
    dfield_t *dfield;
    const byte *field;
    const dict_col_t *col;
    uint32_t len, orig_len, field_no= mach_read_next_compressed(&ptr);

    if (field_no >= REC_MAX_N_FIELDS)
    {
      ptr= trx_undo_read_v_idx(index.table, ptr, first_v_col, &is_undo_log,
                               &field_no);
      first_v_col= false;

      ptr= trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

      if (field_no == FIL_NULL)
        continue; /* there no longer is an index on the virtual column */

      dict_v_col_t *vcol= dict_table_get_nth_v_col(index.table, field_no);
      col =&vcol->m_col;
      dfield= dtuple_get_nth_v_field(node->row, vcol->v_pos);
      dict_col_copy_type(&vcol->m_col, &dfield->type);
    }
    else
    {
      ptr= trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);
      col= dict_index_get_nth_col(&index, field_no);
      if (col->is_dropped())
        continue;
      dfield= dtuple_get_nth_field(node->row, col->ind);
      ut_ad(dfield->type.mtype == DATA_MISSING ||
            dict_col_type_assert_equal(col, &dfield->type));
      ut_ad(dfield->type.mtype == DATA_MISSING ||
            dfield->len == len ||
            (len != UNIV_SQL_NULL && len >= UNIV_EXTERN_STORAGE_FIELD));
      dict_col_copy_type(dict_table_get_nth_col(index.table, col->ind),
                         &dfield->type);
    }

    dfield_set_data(dfield, field, len);

    if (len == UNIV_SQL_NULL || len < UNIV_EXTERN_STORAGE_FIELD)
      continue;

    spatial_status_t spatial_status= static_cast<spatial_status_t>
      ((len & SPATIAL_STATUS_MASK) >> SPATIAL_STATUS_SHIFT);
    len&= ~SPATIAL_STATUS_MASK;

    /* Keep compatible with 5.7.9 format. */
    if (spatial_status == SPATIAL_UNKNOWN)
      spatial_status= dict_col_get_spatial_status(col);

    switch (UNIV_EXPECT(spatial_status, SPATIAL_NONE)) {
    case SPATIAL_ONLY:
      ut_ad(len - UNIV_EXTERN_STORAGE_FIELD == DATA_MBR_LEN);
      dfield_set_len(dfield, len - UNIV_EXTERN_STORAGE_FIELD);
      break;

    case SPATIAL_MIXED:
      dfield_set_len(dfield, len - UNIV_EXTERN_STORAGE_FIELD - DATA_MBR_LEN);
      break;

    default:
      dfield_set_len(dfield, len - UNIV_EXTERN_STORAGE_FIELD);
      break;
    }

    dfield_set_ext(dfield);
    dfield_set_spatial_status(dfield, spatial_status);

    if (!col->ord_part || spatial_status == SPATIAL_ONLY ||
        node->rec_type == TRX_UNDO_UPD_DEL_REC)
      continue;
    /* If the prefix of this BLOB column is indexed, ensure that enough
    prefix is stored in the undo log record. */
    ut_a(dfield_get_len(dfield) >= BTR_EXTERN_FIELD_REF_SIZE);
    ut_a(dict_table_has_atomic_blobs(index.table) ||
         dfield_get_len(dfield) >=
         REC_ANTELOPE_MAX_INDEX_COL_LEN + BTR_EXTERN_FIELD_REF_SIZE);
  }

  for (ulint i= 0; i < index.n_uniq; i++)
  {
    dfield_t &field= node->row->fields[index.fields[i].col->ind];
    if (field.type.mtype == DATA_MISSING)
      field= node->ref->fields[i];
  }

  return const_cast<byte*>(ptr);
}

MY_ATTRIBUTE((nonnull,warn_unused_result))
/** Parses the row reference and other info in a modify undo log record.
@param[in]	node		row undo node
@param[in]	undo_rec	record to purge
@param[in]	thr		query thread
@param[out]	updated_extern	true if an externally stored field was
				updated
@return true if purge operation required */
static
bool
row_purge_parse_undo_rec(
	purge_node_t*		node,
	const trx_undo_rec_t*	undo_rec,
	que_thr_t*		thr,
	bool*			updated_extern)
{
	dict_index_t*	clust_index;
	undo_no_t	undo_no;
	table_id_t	table_id;
	roll_ptr_t	roll_ptr;
	byte		info_bits;
	byte		type;

	const byte* ptr = trx_undo_rec_get_pars(
		undo_rec, &type, &node->cmpl_info,
		updated_extern, &undo_no, &table_id);

	node->rec_type = type;

	switch (type) {
	case TRX_UNDO_RENAME_TABLE:
		return false;
	case TRX_UNDO_EMPTY:
	case TRX_UNDO_INSERT_METADATA:
	case TRX_UNDO_INSERT_REC:
		/* These records do not store any transaction identifier. */
		node->trx_id = TRX_ID_MAX;
		break;
	default:
#ifdef UNIV_DEBUG
		ut_ad("unknown undo log record type" == 0);
		return false;
	case TRX_UNDO_UPD_DEL_REC:
	case TRX_UNDO_UPD_EXIST_REC:
	case TRX_UNDO_DEL_MARK_REC:
#endif /* UNIV_DEBUG */
		ptr = trx_undo_update_rec_get_sys_cols(ptr, &node->trx_id,
						       &roll_ptr, &info_bits);
		break;
	}

	auto &tables_entry= node->tables[table_id];
	node->table = tables_entry.first;
	if (!node->table) {
		return false;
	}

#ifndef DBUG_OFF
	if (MDL_ticket* mdl = tables_entry.second) {
		static_cast<MDL_context*>(thd_mdl_context(current_thd))
			->lock_warrant = mdl->get_ctx();
	}
#endif
	ut_ad(!node->table->is_temporary());

	clust_index = dict_table_get_first_index(node->table);

	if (clust_index->is_corrupted()) {
		/* The table was corrupt in the data dictionary.
		dict_set_corrupted() works on an index, and
		we do not have an index to call it with. */
		DBUG_ASSERT(table_id == node->table->id);
		return false;
	}

	switch (type) {
	case TRX_UNDO_INSERT_METADATA:
		node->ref = &trx_undo_metadata;
		return true;
	case TRX_UNDO_EMPTY:
		node->ref = nullptr;
		return true;
	}

	ptr = trx_undo_rec_get_row_ref(ptr, clust_index, &(node->ref),
				       node->heap);

	if (type == TRX_UNDO_INSERT_REC) {
		return(true);
	}

	ptr = trx_undo_update_rec_get_update(ptr, clust_index, type,
					     node->trx_id,
					     roll_ptr, info_bits,
					     node->heap, &(node->update));

	/* Read to the partial row the fields that occur in indexes */

	if (!(node->cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
		ut_ad(!(node->update->info_bits & REC_INFO_MIN_REC_FLAG));
		ptr = row_purge_get_partial(ptr, *clust_index, node);
	} else if (node->update->info_bits & REC_INFO_MIN_REC_FLAG) {
		node->ref = &trx_undo_metadata;
	}

	return(true);
}

/** Purges the parsed record.
@param[in]	node		row purge node
@param[in]	undo_rec	record to purge
@param[in]	thr		query thread
@param[in]	updated_extern	whether external columns were updated
@return true if purged, false if skipped */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
row_purge_record_func(
	purge_node_t*	node,
	const trx_undo_rec_t*	undo_rec,
#if defined UNIV_DEBUG || defined WITH_WSREP
	const que_thr_t*thr,
#endif /* UNIV_DEBUG || WITH_WSREP */
	bool		updated_extern)
{
	ut_ad(!node->found_clust);
	ut_ad(!node->table->skip_alter_undo);
	ut_ad(!trx_undo_roll_ptr_is_insert(node->roll_ptr));

	node->index = dict_table_get_next_index(
		dict_table_get_first_index(node->table));

	bool purged = true;

	switch (node->rec_type) {
	case TRX_UNDO_EMPTY:
		break;
	case TRX_UNDO_DEL_MARK_REC:
		purged = row_purge_del_mark(node);
		if (purged) {
			if (node->table->stat_initialized
			    && srv_stats_include_delete_marked) {
				dict_stats_update_if_needed(
					node->table, *thr->graph->trx);
			}
			MONITOR_INC(MONITOR_N_DEL_ROW_PURGE);
		}
		break;
	case TRX_UNDO_INSERT_METADATA:
	case TRX_UNDO_INSERT_REC:
		node->roll_ptr |= 1ULL << ROLL_PTR_INSERT_FLAG_POS;
		/* fall through */
	default:
		if (!updated_extern) {
			mtr_t		mtr;
			row_purge_reset_trx_id(node, &mtr);
			break;
		}
		/* fall through */
	case TRX_UNDO_UPD_EXIST_REC:
		row_purge_upd_exist_or_extern(thr, node, undo_rec);
		MONITOR_INC(MONITOR_N_UPD_EXIST_EXTERN);
		break;
	}

	if (node->found_clust) {
		node->found_clust = false;
		btr_pcur_close(&node->pcur);
	}

	return(purged);
}

#if defined UNIV_DEBUG || defined WITH_WSREP
# define row_purge_record(node,undo_rec,thr,updated_extern)	\
	row_purge_record_func(node,undo_rec,thr,updated_extern)
#else /* UNIV_DEBUG || WITH_WSREP */
# define row_purge_record(node,undo_rec,thr,updated_extern)	\
	row_purge_record_func(node,undo_rec,updated_extern)
#endif /* UNIV_DEBUG || WITH_WSREP */

/***********************************************************//**
Fetches an undo log record and does the purge for the recorded operation.
If none left, or the current purge completed, returns the control to the
parent node, which is always a query thread node. */
static MY_ATTRIBUTE((nonnull))
void
row_purge(
/*======*/
	purge_node_t*	node,		/*!< in: row purge node */
	const trx_undo_rec_t*	undo_rec,	/*!< in: record to purge */
	que_thr_t*	thr)		/*!< in: query thread */
{
	if (undo_rec != reinterpret_cast<trx_undo_rec_t*>(-1)) {
		bool	updated_extern;

		while (row_purge_parse_undo_rec(
			       node, undo_rec, thr, &updated_extern)) {

			bool purged = row_purge_record(
				node, undo_rec, thr, updated_extern);

			if (purged
			    || srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
				return;
			}

			/* Retry the purge in a second. */
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
}

inline void purge_node_t::start()
{
  ut_ad(in_progress);
  DBUG_ASSERT(common.type == QUE_NODE_PURGE);

  row= nullptr;
  ref= nullptr;
  index= nullptr;
  update= nullptr;
  found_clust= false;
  rec_type= 0;
  cmpl_info= 0;
}

/** Reset the state at end
@return the query graph parent */
inline que_node_t *purge_node_t::end(THD *thd)
{
  DBUG_ASSERT(common.type == QUE_NODE_PURGE);
  ut_ad(undo_recs.empty());
  ut_d(in_progress= false);
  innobase_reset_background_thd(thd);
#ifndef DBUG_OFF
  static_cast<MDL_context*>(thd_mdl_context(thd))->lock_warrant= nullptr;
#endif
  mem_heap_empty(heap);
  return common.parent;
}


/***********************************************************//**
Does the purge operation.
@return query thread to run next */
que_thr_t*
row_purge_step(
/*===========*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	purge_node_t*	node;

	node = static_cast<purge_node_t*>(thr->run_node);

	node->start();

	while (!node->undo_recs.empty()) {
		trx_purge_rec_t purge_rec = node->undo_recs.front();
		node->undo_recs.pop();
		node->roll_ptr = purge_rec.roll_ptr;

		row_purge(node, purge_rec.undo_rec, thr);
	}

	thr->run_node = node->end(current_thd);
	return(thr);
}

#ifdef UNIV_DEBUG
/***********************************************************//**
Validate the persisent cursor. The purge node has two references
to the clustered index record - one via the ref member, and the
other via the persistent cursor.  These two references must match
each other if the found_clust flag is set.
@return true if the stored copy of persistent cursor is consistent
with the ref member.*/
bool
purge_node_t::validate_pcur()
{
	if (!found_clust) {
		return(true);
	}

	if (index == NULL) {
		return(true);
	}

	if (index->type == DICT_FTS) {
		return(true);
	}

	if (!pcur.old_rec) {
		return(true);
	}

	dict_index_t* clust_index = pcur.index();

	rec_offs* offsets = rec_get_offsets(
		pcur.old_rec, clust_index, NULL, pcur.old_n_core_fields,
		pcur.old_n_fields, &heap);

	/* Here we are comparing the purge ref record and the stored initial
	part in persistent cursor. Both cases we store n_uniq fields of the
	cluster index and so it is fine to do the comparison. We note this
	dependency here as pcur and ref belong to different modules. */
	int st = cmp_dtuple_rec(ref, pcur.old_rec, clust_index, offsets);

	if (st != 0) {
		ib::error() << "Purge node pcur validation failed";
		ib::error() << rec_printer(ref).str();
		ib::error() << rec_printer(pcur.old_rec, offsets).str();
		return(false);
	}

	return(true);
}
#endif /* UNIV_DEBUG */
