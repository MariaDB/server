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
	mtr_t mtr{node->trx};
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

#ifdef ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("enable_row_purge_remove_clust_if_poss_low_sync_point",
                  debug_sync_set_action
                  (current_thd,
                   STRING_WITH_LEN(
                     "now SIGNAL "
                       "row_purge_remove_clust_if_poss_low_before_delete "
                     "WAIT_FOR "
                       "row_purge_remove_clust_if_poss_low_cont"));
                  );
#endif
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

/** Check a virtual column value index secondary virtual index matches
that of current cluster index record, which is recreated from information
stored in undo log
@param[in]	rec		record in the clustered index
@param[in]	icentry		the index entry built from a cluster row
@param[in]	clust_index	cluster index
@param[in]	clust_offsets	offsets on the cluster record
@param[in]	index		the secondary index
@param[in]	ientry		the secondary index entry
@param[in]	node		purge node
@param[in,out]	mtr		mini-transaction
@param[in,out]	v_row		dtuple holding the virtual rows (if needed)
@return true if matches, false otherwise */
static
bool
row_purge_vc_matches_cluster(
	const rec_t*	rec,
	const dtuple_t* icentry,
	dict_index_t*	clust_index,
	rec_offs*	clust_offsets,
	dict_index_t*	index,
	const dtuple_t* ientry,
	const purge_node_t&node,
	mtr_t*		mtr,
	dtuple_t**	vrow)
{
	const rec_t*	version;
	rec_t*          prev_version;
	mem_heap_t*	heap2;
	mem_heap_t*	heap = NULL;
	mem_heap_t*	tuple_heap;
	ulint		num_v = dict_table_get_n_v_cols(index->table);
	bool		compare[REC_MAX_N_FIELDS];
	ulint		n_fields = dtuple_get_n_fields(ientry);
	ulint		n_non_v_col = 0;
	ulint		n_cmp_v_col = 0;
	const dfield_t* field1;
	dfield_t*	field2;
	ulint		i;

	/* First compare non-virtual columns (primary keys) */
	ut_ad(index->n_fields == n_fields);
	ut_ad(n_fields == dtuple_get_n_fields(icentry));
	ut_ad(mtr->memo_contains_page_flagged(rec,
					      MTR_MEMO_PAGE_S_FIX
					      | MTR_MEMO_PAGE_X_FIX));

	{
		const dfield_t* a = ientry->fields;
		const dfield_t* b = icentry->fields;

		for (const dict_field_t *ifield = index->fields,
			     *const end = &index->fields[index->n_fields];
		     ifield != end; ifield++, a++, b++) {
			if (!ifield->col->is_virtual()) {
				if (cmp_dfield_dfield(a, b)) {
					return false;
				}
				n_non_v_col++;
			}
		}
	}

	tuple_heap = mem_heap_create(1024);

	ut_ad(n_fields > n_non_v_col);

	*vrow = dtuple_create_with_vcol(tuple_heap, 0, num_v);
	dtuple_init_v_fld(*vrow);

	for (i = 0; i < num_v; i++) {
		dfield_get_type(dtuple_get_nth_v_field(*vrow, i))->mtype
			 = DATA_MISSING;
		compare[i] = false;
	}

	version = rec;

	while (n_cmp_v_col < n_fields - n_non_v_col) {
		heap2 = heap;
		heap = mem_heap_create(1024);
		roll_ptr_t	cur_roll_ptr = row_get_rec_roll_ptr(
			version, clust_index, clust_offsets);

		ut_ad(cur_roll_ptr != 0);
		ut_ad(node.roll_ptr != 0);

		trx_undo_prev_version_build(
			version, clust_index, clust_offsets,
			heap, &prev_version, mtr,
			TRX_UNDO_PREV_IN_PURGE | TRX_UNDO_GET_OLD_V_VALUE,
			nullptr, vrow);

		if (heap2) {
			mem_heap_free(heap2);
		}

		if (!prev_version) {
			/* Versions end here */
			goto func_exit;
		}

		clust_offsets = rec_get_offsets(prev_version, clust_index,
						NULL,
						clust_index->n_core_fields,
						ULINT_UNDEFINED, &heap);

		ulint	entry_len = dict_index_get_n_fields(index);

		for (i = 0; i < entry_len; i++) {
			const dict_field_t*	ind_field
				 = dict_index_get_nth_field(index, i);
			const dict_col_t*	col = ind_field->col;
			field1 = dtuple_get_nth_field(ientry, i);

			if (!col->is_virtual()) {
				continue;
			}

			const dict_v_col_t*     v_col
                                = reinterpret_cast<const dict_v_col_t*>(col);
			field2
				= dtuple_get_nth_v_field(*vrow, v_col->v_pos);

			if ((dfield_get_type(field2)->mtype != DATA_MISSING)
			    && (!compare[v_col->v_pos])) {

				if (ind_field->prefix_len != 0
				    && !dfield_is_null(field2)) {
					field2->len = unsigned(
						dtype_get_at_most_n_mbchars(
							field2->type.prtype,
							field2->type.mbminlen,
							field2->type.mbmaxlen,
							ind_field->prefix_len,
							field2->len,
							static_cast<char*>
							(field2->data)));
				}

				/* The index field mismatch */
				if (cmp_dfield_dfield(field2, field1)) {
					mem_heap_free(tuple_heap);
					mem_heap_free(heap);
					return(false);
				}

				compare[v_col->v_pos] = true;
				n_cmp_v_col++;
			}
		}

		if (node.roll_ptr == cur_roll_ptr
		    || row_get_rec_trx_id(
			prev_version, clust_index, clust_offsets)
		    < node.trx_id) {
			break;
		}

		version = prev_version;
	}

func_exit:
	if (n_cmp_v_col == 0) {
		*vrow = NULL;
	}

	mem_heap_free(tuple_heap);
	mem_heap_free(heap);

	/* FIXME: In the case of n_cmp_v_col is not the same as
	n_fields - n_non_v_col, callback is needed to compare the rest
	columns. At the timebeing, we will need to return true */
	return (true);
}

/** @return whether two data tuples are equal */
bool dtuple_coll_eq(const dtuple_t &tuple1, const dtuple_t &tuple2)
{
  ut_ad(tuple1.magic_n == DATA_TUPLE_MAGIC_N);
  ut_ad(tuple2.magic_n == DATA_TUPLE_MAGIC_N);
  ut_ad(dtuple_check_typed(&tuple1));
  ut_ad(dtuple_check_typed(&tuple2));
  ut_ad(tuple1.n_fields == tuple2.n_fields);

  for (ulint i= 0; i < tuple1.n_fields; i++)
    if (cmp_dfield_dfield(&tuple1.fields[i], &tuple2.fields[i]))
      return false;
  return true;
}

/** Finds out if a version of the record, where the version >= the current
purge_sys.view, should have ientry as its secondary index entry. We check
if there is any not delete marked version of the record where the trx
id >= purge view, and the secondary index entry == ientry; exactly in
this case we return TRUE.
@param node    purge node
@param index   secondary index
@param ientry  secondary index entry
@param mtr     mini-transaction
@return whether ientry cannot be purged */
static bool row_purge_is_unsafe(const purge_node_t &node,
                                dict_index_t *index,
                                const dtuple_t *ientry, mtr_t *mtr)
{
	const rec_t*	rec = btr_pcur_get_rec(&node.pcur);
	roll_ptr_t	roll_ptr = node.roll_ptr;
	trx_id_t	trx_id = node.trx_id;
	const rec_t*	version;
	rec_t*		prev_version;
	dict_index_t*	clust_index = node.pcur.index();
	rec_offs*	clust_offsets;
	mem_heap_t*	heap;
	dtuple_t*	row;
	const dtuple_t*	entry;
	dtuple_t*	vrow = NULL;
	mem_heap_t*	v_heap = NULL;
	dtuple_t*	cur_vrow = NULL;

	ut_ad(index->table == clust_index->table);
	heap = mem_heap_create(1024);
	clust_offsets = rec_get_offsets(rec, clust_index, NULL,
					clust_index->n_core_fields,
					ULINT_UNDEFINED, &heap);

	if (dict_index_has_virtual(index)) {
		v_heap = mem_heap_create(100);
	}

	if (!rec_get_deleted_flag(rec, rec_offs_comp(clust_offsets))) {
		row_ext_t*	ext;

		/* The top of the stack of versions is locked by the
		mtr holding a latch on the page containing the
		clustered index record. The bottom of the stack is
		locked by the fact that the purge_sys.view must
		'overtake' any read view of an active transaction.
		Thus, it is safe to fetch the prefixes for
		externally stored columns. */
		row = row_build(ROW_COPY_POINTERS, clust_index,
				rec, clust_offsets,
				NULL, NULL, NULL, &ext, heap);

		if (dict_index_has_virtual(index)) {


#ifdef DBUG_OFF
# define dbug_v_purge false
#else /* DBUG_OFF */
                        bool    dbug_v_purge = false;
#endif /* DBUG_OFF */

			DBUG_EXECUTE_IF(
				"ib_purge_virtual_index_callback",
				dbug_v_purge = true;);

			roll_ptr_t t_roll_ptr = row_get_rec_roll_ptr(
				rec, clust_index, clust_offsets);

			/* if the row is newly inserted, then the virtual
			columns need to be computed */
			if (trx_undo_roll_ptr_is_insert(t_roll_ptr)
			    || dbug_v_purge) {

				if (!row_vers_build_clust_v_col(
					    row, clust_index, index, heap)) {
					goto unsafe_to_purge;
				}

				entry = row_build_index_entry(
					row, ext, index, heap);
				if (entry && dtuple_coll_eq(*ientry, *entry)) {
					goto unsafe_to_purge;
				}
			} else {
				/* Build index entry out of row */
				entry = row_build_index_entry(row, ext, index, heap);
				/* entry could only be NULL if
				the clustered index record is an uncommitted
				inserted record whose BLOBs have not been
				written yet. The secondary index record
				can be safely removed, because it cannot
				possibly refer to this incomplete
				clustered index record. (Insert would
				always first be completed for the
				clustered index record, then proceed to
				secondary indexes.) */

				if (entry && row_purge_vc_matches_cluster(
					    rec, entry,
					    clust_index, clust_offsets,
					    index, ientry, node, mtr, &vrow)) {
					goto unsafe_to_purge;
				}
			}
			clust_offsets = rec_get_offsets(rec, clust_index, NULL,
							clust_index
							->n_core_fields,
							ULINT_UNDEFINED, &heap);
		} else {

			entry = row_build_index_entry(
				row, ext, index, heap);

			/* If entry == NULL, the record contains unset BLOB
			pointers.  This must be a freshly inserted record.  If
			this is called from
			row_purge_remove_sec_if_poss_low(), the thread will
			hold latches on the clustered index and the secondary
			index.  Because the insert works in three steps:

				(1) insert the record to clustered index
				(2) store the BLOBs and update BLOB pointers
				(3) insert records to secondary indexes

			the purge thread can safely ignore freshly inserted
			records and delete the secondary index record.  The
			thread that inserted the new record will be inserting
			the secondary index records. */

			/* NOTE that we cannot do the comparison as binary
			fields because the row is maybe being modified so that
			the clustered index record has already been updated to
			a different binary value in a char field, but the
			collation identifies the old and new value anyway! */
			if (entry && dtuple_coll_eq(*ientry, *entry)) {
unsafe_to_purge:
				mem_heap_free(heap);

				if (v_heap) {
					mem_heap_free(v_heap);
				}
				return true;
			}
		}
	} else if (dict_index_has_virtual(index)) {
		/* The current cluster index record could be
		deleted, but the previous version of it might not. We will
		need to get the virtual column data from undo record
		associated with current cluster index */

		cur_vrow = row_vers_build_cur_vrow(
			rec, clust_index, &clust_offsets,
			index, trx_id, roll_ptr, heap, v_heap, mtr);
	}

	version = rec;

	for (;;) {
		mem_heap_t* heap2 = heap;
		heap = mem_heap_create(1024);
		vrow = NULL;

		trx_undo_prev_version_build(version,
					    clust_index, clust_offsets,
					    heap, &prev_version, mtr,
					    TRX_UNDO_CHECK_PURGE_PAGES,
					    nullptr,
					    dict_index_has_virtual(index)
					    ? &vrow : nullptr);
		mem_heap_free(heap2); /* free version and clust_offsets */

		if (!prev_version) {
			/* Versions end here */
			mem_heap_free(heap);

			if (v_heap) {
				mem_heap_free(v_heap);
			}

			return false;
		}

		clust_offsets = rec_get_offsets(prev_version, clust_index,
						NULL,
						clust_index->n_core_fields,
						ULINT_UNDEFINED, &heap);

		if (dict_index_has_virtual(index)) {
			if (vrow) {
				if (dtuple_vcol_data_missing(*vrow, *index)) {
					goto nochange_index;
				}
				/* Keep the virtual row info for the next
				version, unless it is changed */
				mem_heap_empty(v_heap);
				cur_vrow = dtuple_copy(vrow, v_heap);
				dtuple_dup_v_fld(cur_vrow, v_heap);
			}

			if (!cur_vrow) {
				/* Nothing for this index has changed,
				continue */
nochange_index:
				version = prev_version;
				continue;
			}
		}

		if (!rec_get_deleted_flag(prev_version,
					  rec_offs_comp(clust_offsets))) {
			row_ext_t*	ext;

			/* The stack of versions is locked by mtr.
			Thus, it is safe to fetch the prefixes for
			externally stored columns. */
			row = row_build(ROW_COPY_POINTERS, clust_index,
					prev_version, clust_offsets,
					NULL, NULL, NULL, &ext, heap);

			if (dict_index_has_virtual(index)) {
				ut_ad(cur_vrow);
				ut_ad(row->n_v_fields == cur_vrow->n_v_fields);
				dtuple_copy_v_fields(row, cur_vrow);
			}

			entry = row_build_index_entry(row, ext, index, heap);

			/* If entry == NULL, the record contains unset
			BLOB pointers.  This must be a freshly
			inserted record that we can safely ignore.
			For the justification, see the comments after
			the previous row_build_index_entry() call. */

			/* NOTE that we cannot do the comparison as binary
			fields because maybe the secondary index record has
			already been updated to a different binary value in
			a char field, but the collation identifies the old
			and new value anyway! */

			if (entry && dtuple_coll_eq(*ientry, *entry)) {
				goto unsafe_to_purge;
			}
		}

		version = prev_version;
	}
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
@param node   row purge node
@param index  secondary index
@param entry  secondary index entry
@param mtr    mini-transaction for looking up clustered index
@return whether the secondary index record can be purged */
static bool row_purge_poss_sec(purge_node_t *node, dict_index_t *index,
			       const dtuple_t *entry, mtr_t *mtr)
{
  ut_ad(mtr->trx == node->trx);
  ut_ad(!index->is_clust());
  const auto savepoint= mtr->get_savepoint();
  bool can_delete= !row_purge_reposition_pcur(BTR_SEARCH_LEAF, node, mtr);

  if (!can_delete)
  {
    ut_ad(node->pcur.pos_state == BTR_PCUR_IS_POSITIONED);
    can_delete= !row_purge_is_unsafe(*node, index, entry, mtr);
    node->pcur.pos_state = BTR_PCUR_WAS_POSITIONED;
    node->pcur.latch_mode= BTR_NO_LATCHES;
  }

  mtr->rollback_to_savepoint(savepoint);
  return can_delete;
}

/** Report an error about not delete-marked secondary index record
that was about to be purged.
@param cur   cursor on the secondary index record
@param entry search key */
ATTRIBUTE_COLD ATTRIBUTE_NOINLINE
static void row_purge_del_mark_error(const btr_cur_t &cursor,
                                     const dtuple_t &entry)
{
  const dict_index_t *index= cursor.index();
  ib::error() << "tried to purge non-delete-marked record in index "
              << index->name << " of table " << index->table->name
              << ": tuple: " << entry
              << ", record: " << rec_index_print(cursor.page_cur.rec, index);
  ut_ad(0);
}

__attribute__((nonnull, warn_unused_result))
/** Remove a secondary index entry if possible, by modifying the index tree.
@param node             purge node
@param index            secondary index
@param entry            index entry
@param page_max_trx_id  the PAGE_MAX_TRX_ID
                        when row_purge_remove_sec_if_poss_leaf() was invoked
@return whether the operation succeeded */
static bool row_purge_remove_sec_if_poss_tree(purge_node_t *node,
					      dict_index_t *index,
					      const dtuple_t *entry,
					      trx_id_t page_max_trx_id)
{
	btr_pcur_t		pcur;
	bool			success	= true;
	dberr_t			err;
	mtr_t			mtr{node->trx};

	log_free_check();
#ifdef ENABLED_DEBUG_SYNC
	DBUG_EXECUTE_IF("enable_row_purge_sec_tree_sync",
		debug_sync_set_action(node->trx->mysql_thd, STRING_WITH_LEN(
			"now SIGNAL "
			"purge_sec_tree_begin"));
		debug_sync_set_action(node->trx->mysql_thd, STRING_WITH_LEN(
			"now WAIT_FOR "
			"purge_sec_tree_execute"));
	);
#endif
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

	if (page_max_trx_id
	    == page_get_max_trx_id(btr_cur_get_page(&pcur.btr_cur))
	    || row_purge_poss_sec(node, index, entry, &mtr)) {

		/* Remove the index record, which should have been
		marked for deletion. */
		if (!rec_get_deleted_flag(btr_pcur_get_rec(&pcur),
					  index->table->not_redundant())) {
			row_purge_del_mark_error(pcur.btr_cur, *entry);
			goto func_exit;
		}

		btr_cur_pessimistic_delete(&err, FALSE, &pcur.btr_cur,
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
	return success;
}

/** Compute a nonzero return value of row_purge_remove_sec_if_poss_leaf().
@param page  latched secondary index page
@return PAGE_MAX_TRX_ID for row_purge_remove_sec_if_poss_tree()
@retval 1 if a further row_purge_poss_sec() check is necessary */
ATTRIBUTE_NOINLINE ATTRIBUTE_COLD
static trx_id_t row_purge_check(const page_t *page) noexcept
{
  trx_id_t id= page_get_max_trx_id(page);
  ut_ad(id);
  if (trx_sys.find_same_or_older_in_purge(purge_sys.query->trx, id))
    /* Because an active transaction may modify the secondary index
    but not PAGE_MAX_TRX_ID, row_purge_poss_sec() must be invoked
    again after re-latching the page. Let us return a bogus ID. Yes,
    an actual transaction with ID 1 would create the InnoDB dictionary
    tables in dict_sys_t::create_or_check_sys_tables(), but it would
    exclusively write TRX_UNDO_INSERT_REC records. Purging those
    records never involves row_purge_remove_sec_if_poss_tree(). */
    id= 1;
  return id;
}

__attribute__((nonnull, warn_unused_result))
/** Remove a secondary index entry if possible, without modifying the tree.
@param node             purge node
@param index            secondary index
@param entry            index entry
@return PAGE_MAX_TRX_ID for row_purge_remove_sec_if_poss_tree()
@retval 1 if a further row_purge_poss_sec() check is necessary
@retval 0 if success or if not found */
static trx_id_t row_purge_remove_sec_if_poss_leaf(purge_node_t *node,
                                                  dict_index_t *index,
                                                  const dtuple_t *entry)
{
	mtr_t			mtr{node->trx};
	btr_pcur_t		pcur;
	trx_id_t		page_max_trx_id = 0;

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
		if (row_purge_poss_sec(node, index, entry, &mtr)) {
			/* Only delete-marked records should be purged. */
			if (!rec_get_deleted_flag(btr_pcur_get_rec(&pcur),
						  index->table
						  ->not_redundant())) {
				row_purge_del_mark_error(pcur.btr_cur, *entry);
				mtr.commit();
				dict_set_corrupted(node->trx, index, "purge");
				goto cleanup;
			}

			if (index->is_spatial()) {
				const buf_block_t* block = btr_pcur_get_block(
					&pcur);
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

			if (btr_cur_optimistic_delete(&pcur.btr_cur, 0, &mtr)
			    == DB_FAIL) {
				page_max_trx_id = row_purge_check(
					btr_pcur_get_page(&pcur));
			}
		}
	}

func_exit:
	mtr.commit();
cleanup:
	btr_pcur_close(&pcur);
	return page_max_trx_id;
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
  if (UNIV_UNLIKELY(!entry))
    /* The node->row must have lacked some fields of this index. This
    is possible when the undo log record was written before this index
    was created. */
    return;

  if (trx_id_t page_max_trx_id=
      row_purge_remove_sec_if_poss_leaf(node, index, entry))
    for (auto n_tries= BTR_CUR_RETRY_DELETE_N_TIMES;
         !row_purge_remove_sec_if_poss_tree(node, index, entry,
                                            page_max_trx_id);
         std::this_thread::sleep_for(BTR_CUR_RETRY_SLEEP_TIME))
      /* The delete operation may fail if we have little
      file space left (if innodb_file_per_table=0?) */
      ut_a(--n_tries);
}

/**
Purges a delete marking of a record.
@param node   row purge node
@retval true if the row was not found, or it was successfully removed
@retval false the purge needs to be suspended because of
running out of file space */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool row_purge_del_mark(purge_node_t *node) noexcept
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
                  (node->trx->mysql_thd,
                   STRING_WITH_LEN("now SIGNAL row_purge_del_mark_finished"));
                  );
#endif

  return result;
}

/***********************************************************//**
Purges an update of an existing record. Also purges an update of a delete
marked record if that record contained an externally stored field. */
static
void
row_purge_upd_exist_or_extern(
	const que_thr_t*thr,		/*!< in: query thread */
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
			row_purge_remove_sec_if_poss(
				node, node->index, entry);

			ut_ad(node->table);

			mem_heap_empty(heap);
		}
	} while ((node->index = dict_table_get_next_index(node->index)));

	mem_heap_free(heap);

skip_secondaries:
	mtr_t mtr{node->trx};
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
				block->page.flag_accessed();

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
}

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
row_purge_record(
	purge_node_t*	node,
	const trx_undo_rec_t*	undo_rec,
	const que_thr_t*thr,
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
			if (node->table->stat_initialized()
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
