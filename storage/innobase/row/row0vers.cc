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
@file row/row0vers.cc
Row versions

Created 2/6/1997 Heikki Tuuri
*******************************************************/

#include "row0vers.h"
#include "dict0dict.h"
#include "dict0boot.h"
#include "btr0btr.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "row0upd.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "row0mysql.h"

/** Check whether all non-virtual index fields are equal.
@param[in]	index	the secondary index
@param[in]	a	first index entry to compare
@param[in]	b	second index entry to compare
@return	whether all non-virtual fields are equal */
static
bool
row_vers_non_virtual_fields_equal(
	const dict_index_t*	index,
	const dfield_t*		a,
	const dfield_t*		b)
{
	const dict_field_t* end = &index->fields[index->n_fields];

	for (const dict_field_t* ifield = index->fields; ifield != end;
	     ifield++) {
		if (!ifield->col->is_virtual()
		    && cmp_dfield_dfield(a++, b++)) {
			return false;
		}
	}

	return true;
}

/** Determine if an active transaction has inserted or modified a secondary
index record.
@param[in]	clust_rec	clustered index record
@param[in]	clust_index	clustered index
@param[in]	rec		secondary index record
@param[in]	index		secondary index
@param[in]	offsets		rec_get_offsets(rec, index)
@param[in,out]	mtr		mini-transaction
@return	the active transaction; state must be rechecked after
acquiring trx->mutex, and trx->release_reference() must be invoked
@retval	NULL if the record was committed */
UNIV_INLINE
trx_t*
row_vers_impl_x_locked_low(
	const rec_t*	clust_rec,
	dict_index_t*	clust_index,
	const rec_t*	rec,
	dict_index_t*	index,
	const rec_offs*	offsets,
	mtr_t*		mtr)
{
	trx_id_t	trx_id;
	rec_t*		prev_version = NULL;
	rec_offs	clust_offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	clust_offsets;
	mem_heap_t*	heap;
	dtuple_t*	ientry = NULL;
	mem_heap_t*	v_heap = NULL;
	dtuple_t*	cur_vrow = NULL;

	rec_offs_init(clust_offsets_);

	DBUG_ENTER("row_vers_impl_x_locked_low");

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mtr->memo_contains_page_flagged(clust_rec,
					      MTR_MEMO_PAGE_S_FIX
					      | MTR_MEMO_PAGE_X_FIX));

	if (ulint trx_id_offset = clust_index->trx_id_offset) {
		trx_id = mach_read_from_6(clust_rec + trx_id_offset);
		if (trx_id == 0) {
			/* The transaction history was already purged. */
			DBUG_RETURN(0);
		}
	}

	heap = mem_heap_create(1024);

	clust_offsets = rec_get_offsets(clust_rec, clust_index, clust_offsets_,
					clust_index->n_core_fields,
					ULINT_UNDEFINED, &heap);

	trx_t* trx = nullptr;
	trx_id = row_get_rec_trx_id(clust_rec, clust_index, clust_offsets);
	if (trx_id <= mtr->trx->max_inactive_id) {
		/* The transaction history was already purged. */
	done:
		mem_heap_free(heap);
		DBUG_RETURN(trx);
	}

	ut_ad(!clust_index->table->is_temporary());

	if (trx_id == mtr->trx->id) {
		trx = mtr->trx;
		trx->reference();
		goto done;
	} else {
		trx = trx_sys.find(mtr->trx, trx_id);
		if (trx == 0) {
			/* The transaction that modified or inserted
			clust_rec is no longer active, or it is
			corrupt: no implicit lock on rec */
			lock_check_trx_id_sanity(trx_id, clust_rec,
						 clust_index, clust_offsets);
			mem_heap_free(heap);
			DBUG_RETURN(0);
		}
	}

	const bool comp = index->table->not_redundant();
        ut_ad(!!page_rec_is_comp(rec) == comp);
	ut_ad(index->table == clust_index->table);
	ut_ad(!comp == !page_rec_is_comp(clust_rec));

	const ulint rec_del = rec_get_deleted_flag(rec, comp);

	if (dict_index_has_virtual(index)) {
		ulint	est_size = DTUPLE_EST_ALLOC(index->n_fields);

		/* Allocate the dtuple for virtual columns extracted from undo
		log with its own heap, so to avoid it being freed as we
		iterating in the version loop below. */
		v_heap = mem_heap_create(est_size);
		ientry = row_rec_to_index_entry(rec, index, offsets, v_heap);
	}

	/* We look up if some earlier version, which was modified by
	the trx_id transaction, of the clustered index record would
	require rec to be in a different state (delete marked or
	unmarked, or have different field values, or not existing). If
	there is such a version, then rec was modified by the trx_id
	transaction, and it has an implicit x-lock on rec. Note that
	if clust_rec itself would require rec to be in a different
	state, then the trx_id transaction has not yet had time to
	modify rec, and does not necessarily have an implicit x-lock
	on rec. */

	for (const rec_t* version = clust_rec;; version = prev_version) {
		row_ext_t*	ext;
		dtuple_t*	row;
		dtuple_t*	entry;
		ulint		vers_del;
		trx_id_t	prev_trx_id;
		mem_heap_t*	old_heap = heap;
		dtuple_t*	vrow = NULL;

		/* We keep the semaphore in mtr on the clust_rec page, so
		that no other transaction can update it and get an
		implicit x-lock on rec until mtr_commit(mtr). */

		heap = mem_heap_create(1024);

		trx_undo_prev_version_build(
			version, clust_index, clust_offsets,
			heap, &prev_version, mtr, 0, NULL,
			dict_index_has_virtual(index) ? &vrow : NULL);
		ut_d(bool owns_trx_mutex = trx->mutex_is_owner());
		ut_d(if (!owns_trx_mutex)
		    trx->mutex_lock();)
		const bool committed = trx_state_eq(
			trx, TRX_STATE_COMMITTED_IN_MEMORY);
		ut_d(if (!owns_trx_mutex)
		  trx->mutex_unlock();)

		/* The oldest visible clustered index version must not be
		delete-marked, because we never start a transaction by
		inserting a delete-marked record. */
		ut_ad(committed || prev_version
		      || !rec_get_deleted_flag(version, comp));

		/* Free version and clust_offsets. */
		mem_heap_free(old_heap);

		if (committed) {
			goto not_locked;
		}

		if (prev_version == NULL) {

			/* We reached the oldest visible version without
			finding an older version of clust_rec that would
			match the secondary index record.  If the secondary
			index record is not delete marked, then clust_rec
			is considered the correct match of the secondary
			index record and hence holds the implicit lock. */

			if (rec_del) {
				/* The secondary index record is del marked.
				So, the implicit lock holder of clust_rec
				did not modify the secondary index record yet,
				and is not holding an implicit lock on it.

				This assumes that whenever a row is inserted
				or updated, the leaf page record always is
				created with a clear delete-mark flag.
				(We never insert a delete-marked record.) */
not_locked:
				trx->release_reference();
				trx = 0;
			}

			break;
		}

		clust_offsets = rec_get_offsets(
			prev_version, clust_index, clust_offsets_,
			clust_index->n_core_fields,
			ULINT_UNDEFINED, &heap);

		vers_del = rec_get_deleted_flag(prev_version, comp);

		prev_trx_id = row_get_rec_trx_id(prev_version, clust_index,
						 clust_offsets);

		/* The stack of versions is locked by mtr.  Thus, it
		is safe to fetch the prefixes for externally stored
		columns. */

		row = row_build(ROW_COPY_POINTERS, clust_index, prev_version,
				clust_offsets,
				NULL, NULL, NULL, &ext, heap);

		if (dict_index_has_virtual(index)) {
			if (vrow) {
				/* Keep the virtual row info for the next
				version */
				cur_vrow = dtuple_copy(vrow, v_heap);
				dtuple_dup_v_fld(cur_vrow, v_heap);
			}

			if (!cur_vrow) {
				/* Build index entry out of row */
				entry = row_build_index_entry(row, ext, index,
							      heap);

				/* entry could only be NULL (the
				clustered index record could contain
				BLOB pointers that are NULL) if we
				were accessing a freshly inserted
				record before it was fully inserted.
				prev_version cannot possibly be such
				an incomplete record, because its
				transaction would have to be committed
				in order for later versions of the
				record to be able to exist. */
				ut_ad(entry);

				/* If the indexed virtual columns has changed,
				there must be log record to generate vrow.
				Otherwise, it is not changed, so no need
				to compare */
				if (!row_vers_non_virtual_fields_equal(
					    index,
					    ientry->fields, entry->fields)) {
					if (rec_del != vers_del) {
						break;
					}
				} else if (!rec_del) {
					break;
				}

				goto result_check;
			} else {
				ut_ad(row->n_v_fields == cur_vrow->n_v_fields);
				dtuple_copy_v_fields(row, cur_vrow);
			}
		}

		entry = row_build_index_entry(row, ext, index, heap);

		/* entry could only be NULL (the clustered index
		record could contain BLOB pointers that are NULL) if
		we were accessing a freshly inserted record before it
		was fully inserted.  prev_version cannot possibly be
		such an incomplete record, because its transaction
		would have to be committed in order for later versions
		of the record to be able to exist. */
		ut_ad(entry);

		/* If we get here, we know that the trx_id transaction
		modified prev_version. Let us check if prev_version
		would require rec to be in a different state. */

		/* The previous version of clust_rec must be
		accessible, because clust_rec was not a fresh insert.
		There is no guarantee that the transaction is still
		active. */

		/* We check if entry and rec are identified in the alphabetical
		ordering */
		if (0 == cmp_dtuple_rec(entry, rec, index, offsets)) {
			/* The delete marks of rec and prev_version should be
			equal for rec to be in the state required by
			prev_version */

			if (rec_del != vers_del) {

				break;
			}

			/* It is possible that the row was updated so that the
			secondary index record remained the same in
			alphabetical ordering, but the field values changed
			still. For example, 'abc' -> 'ABC'. Check also that. */

			dtuple_set_types_binary(
				entry, dtuple_get_n_fields(entry));

			if (cmp_dtuple_rec(entry, rec, index, offsets)) {

				break;
			}

		} else if (!rec_del) {
			/* The delete mark should be set in rec for it to be
			in the state required by prev_version */

			break;
		}

result_check:
		if (trx->id != prev_trx_id) {
			/* prev_version was the first version modified by
			the trx_id transaction: no implicit x-lock */
			goto not_locked;
		}
	}

	if (trx) {
		DBUG_PRINT("info", ("Implicit lock is held by trx:" TRX_ID_FMT,
				    trx_id));
	}

	if (v_heap != NULL) {
		mem_heap_free(v_heap);
	}

	mem_heap_free(heap);
	DBUG_RETURN(trx);
}

/** Determine if an active transaction has inserted or modified a secondary
index record.
@param[in,out]	caller_trx	trx of current thread
@param[in]	rec	secondary index record
@param[in]	index	secondary index
@param[in]	offsets	rec_get_offsets(rec, index)
@return	the active transaction; state must be rechecked after
acquiring trx->mutex, and trx->release_reference() must be invoked
@retval	NULL if the record was committed */
trx_t*
row_vers_impl_x_locked(
	trx_t*		caller_trx,
	const rec_t*	rec,
	dict_index_t*	index,
	const rec_offs*	offsets)
{
	mtr_t		mtr{caller_trx};
	trx_t*		trx;
	const rec_t*	clust_rec;
	dict_index_t*	clust_index;

	/* The function must not be invoked under lock_sys latch to prevent
	latching order violation, i.e. page latch must be acquired before
	lock_sys latch */
	lock_sys.assert_unlocked();
	/* The current function can be called from lock_rec_unlock_unmodified()
	under lock_sys.wr_lock() */

	mtr_start(&mtr);

	/* Search for the clustered index record. The latch on the
	page of clust_rec locks the top of the stack of versions. The
	bottom of the version stack is not locked; oldest versions may
	disappear by the fact that transactions may be committed and
	collected by the purge. This is not a problem, because we are
	only interested in active transactions. */

	clust_rec = row_get_clust_rec(
		BTR_SEARCH_LEAF, rec, index, &clust_index, &mtr);

	if (!clust_rec) {
		/* In a rare case it is possible that no clust rec is found
		for a secondary index record: if in row0umod.cc
		row_undo_mod_remove_clust_low() we have already removed the
		clust rec, while purge is still cleaning and removing
		secondary index records associated with earlier versions of
		the clustered index record. In that case there cannot be
		any implicit lock on the secondary index record, because
		an active transaction which has modified the secondary index
		record has also modified the clustered index record. And in
		a rollback we always undo the modifications to secondary index
		records before the clustered index record. */

		trx = 0;
	} else {
		trx = row_vers_impl_x_locked_low(
				clust_rec, clust_index, rec, index,
				offsets, &mtr);

		ut_ad(trx == 0 || trx->is_referenced());
	}

	mtr_commit(&mtr);

	return(trx);
}

/** build virtual column value from current cluster index record data
@param[in,out]	row		the cluster index row in dtuple form
@param[in]	clust_index	clustered index
@param[in]	index		the secondary index
@param[in]	heap		heap used to build virtual dtuple. */
bool
row_vers_build_clust_v_col(
	dtuple_t*		row,
	dict_index_t*		clust_index,
	dict_index_t*		index,
	mem_heap_t*		heap)
{
	THD*		thd= current_thd;
	TABLE*		maria_table= 0;

	ut_ad(dict_index_has_virtual(index));
	ut_ad(index->table == clust_index->table);

	DEBUG_SYNC(current_thd, "ib_clust_v_col_before_row_allocated");

	ib_vcol_row vc(nullptr);
	byte *record = vc.record(thd, index, &maria_table);

	ut_ad(maria_table);

	for (ulint i = 0; i < dict_index_get_n_fields(index); i++) {
		const dict_col_t* c = dict_index_get_nth_col(index, i);

		if (c->is_virtual()) {
			const dict_v_col_t* col
				= reinterpret_cast<const dict_v_col_t*>(c);

			dfield_t *vfield = innobase_get_computed_value(
				row, col, clust_index, &vc.heap,
				heap, NULL, thd, maria_table, record, NULL,
				NULL);
			if (!vfield) {
				innobase_report_computed_value_failed(row);
				ut_ad(0);
				return false;
			}
		}
	}

	return true;
}

/** Build latest virtual column data from undo log
@param[in]	rec		clustered index record
@param[in]	clust_index	clustered index
@param[in,out]	clust_offsets	offsets on the clustered index record
@param[in]	index		the secondary index
@param[in]	trx_id		transaction ID on the purging record,
				or 0 if called outside purge
@param[in]	roll_ptr	the rollback pointer for the purging record
@param[in,out]	v_heap		heap used to build vrow
@param[out]	v_row		dtuple holding the virtual rows
@param[in,out]	mtr		mtr holding the latch on rec */
static
void
row_vers_build_cur_vrow_low(
	const rec_t*		rec,
	dict_index_t*		clust_index,
	rec_offs*		clust_offsets,
	dict_index_t*		index,
	trx_id_t		trx_id,
	roll_ptr_t		roll_ptr,
	mem_heap_t*		v_heap,
	dtuple_t**		vrow,
	mtr_t*			mtr)
{
	const rec_t*	version;
	rec_t*		prev_version;
	mem_heap_t*	heap = NULL;
	const auto	num_v = dict_table_get_n_v_cols(index->table);
	const dfield_t* field;
	ulint		i;
	bool		all_filled = false;

	*vrow = dtuple_create_with_vcol(v_heap, 0, num_v);
	dtuple_init_v_fld(*vrow);

	for (i = 0; i < num_v; i++) {
		dfield_get_type(dtuple_get_nth_v_field(*vrow, i))->mtype
			 = DATA_MISSING;
	}

	ut_ad(mtr->memo_contains_page_flagged(rec,
					      MTR_MEMO_PAGE_S_FIX
					      | MTR_MEMO_PAGE_X_FIX));

	version = rec;

	/* If this is called by purge thread, set TRX_UNDO_PREV_IN_PURGE
	bit to search the undo log until we hit the current undo log with
	roll_ptr */
	const ulint	status = trx_id
		? TRX_UNDO_PREV_IN_PURGE | TRX_UNDO_GET_OLD_V_VALUE
		: TRX_UNDO_GET_OLD_V_VALUE;

	while (!all_filled) {
		mem_heap_t*	heap2 = heap;
		heap = mem_heap_create(1024);
		roll_ptr_t	cur_roll_ptr = row_get_rec_roll_ptr(
			version, clust_index, clust_offsets);

		trx_undo_prev_version_build(
			version, clust_index, clust_offsets,
			heap, &prev_version, mtr, status, nullptr, vrow);

		if (heap2) {
			mem_heap_free(heap2);
		}

		if (!prev_version) {
			/* Versions end here */
			break;
		}

		clust_offsets = rec_get_offsets(prev_version, clust_index,
						NULL,
						clust_index->n_core_fields,
						ULINT_UNDEFINED, &heap);

		ulint	entry_len = dict_index_get_n_fields(index);

		all_filled = true;

		for (i = 0; i < entry_len; i++) {
			const dict_col_t* col
				= dict_index_get_nth_col(index, i);

			if (!col->is_virtual()) {
				continue;
			}

			const dict_v_col_t*	v_col
				= reinterpret_cast<const dict_v_col_t*>(col);
			field = dtuple_get_nth_v_field(*vrow, v_col->v_pos);

			if (dfield_get_type(field)->mtype == DATA_MISSING) {
				all_filled = false;
				break;
			}

		}

		trx_id_t	rec_trx_id = row_get_rec_trx_id(
			prev_version, clust_index, clust_offsets);

		if (rec_trx_id < trx_id || roll_ptr == cur_roll_ptr) {
			break;
		}

		version = prev_version;
	}

	mem_heap_free(heap);
}

/** Build a dtuple contains virtual column data for current cluster index
@param[in]	in_purge	called by purge thread
@param[in]	rec		cluster index rec
@param[in]	clust_index	cluster index
@param[in]	clust_offsets	cluster rec offset
@param[in]	index		secondary index
@param[in]	trx_id		transaction ID on the purging record,
				or 0 if called outside purge
@param[in]	roll_ptr	roll_ptr for the purge record
@param[in,out]	heap		heap memory
@param[in,out]	v_heap		heap memory to keep virtual column tuple
@param[in,out]	mtr		mini-transaction
@return dtuple contains virtual column data */
dtuple_t*
row_vers_build_cur_vrow(
	const rec_t*		rec,
	dict_index_t*		clust_index,
	rec_offs**		clust_offsets,
	dict_index_t*		index,
	trx_id_t		trx_id,
	roll_ptr_t		roll_ptr,
	mem_heap_t*		heap,
	mem_heap_t*		v_heap,
	mtr_t*			mtr)
{
	dtuple_t* cur_vrow = NULL;

	roll_ptr_t t_roll_ptr = row_get_rec_roll_ptr(
		rec, clust_index, *clust_offsets);

	/* if the row is newly inserted, then the virtual
	columns need to be computed */
	if (trx_undo_roll_ptr_is_insert(t_roll_ptr)) {

		ut_ad(!rec_get_deleted_flag(rec, page_rec_is_comp(rec)));

		/* This is a newly inserted record and cannot
		be deleted, So the externally stored field
		cannot be freed yet. */
		dtuple_t* row = row_build(ROW_COPY_POINTERS, clust_index,
					  rec, *clust_offsets,
					  NULL, NULL, NULL, NULL, heap);

		if (!row_vers_build_clust_v_col(row, clust_index, index,
						heap)) {
			return nullptr;
		}

		cur_vrow = dtuple_copy(row, v_heap);
		dtuple_dup_v_fld(cur_vrow, v_heap);
	} else {
		/* Try to fetch virtual column data from undo log */
		row_vers_build_cur_vrow_low(
			rec, clust_index, *clust_offsets,
			index, trx_id, roll_ptr, v_heap, &cur_vrow, mtr);
	}

	*clust_offsets = rec_get_offsets(rec, clust_index, NULL,
					 clust_index->n_core_fields,
					 ULINT_UNDEFINED, &heap);
	return(cur_vrow);
}

/** Find out whether data tuple has missing data type
for indexed virtual column.
@param tuple   data tuple
@param index   virtual index
@return true if tuple has missing column type */
bool dtuple_vcol_data_missing(const dtuple_t &tuple,
                              const dict_index_t &index)
{
  for (ulint i= 0; i < index.n_uniq; i++)
  {
    dict_col_t *col= index.fields[i].col;
    if (!col->is_virtual())
      continue;
    dict_v_col_t *vcol= reinterpret_cast<dict_v_col_t*>(col);
    for (ulint j= 0; j < index.table->n_v_cols; j++)
      if (vcol == &index.table->v_cols[j] &&
          tuple.v_fields[j].type.mtype == DATA_MISSING)
        return true;
  }
  return false;
}

/*****************************************************************//**
Constructs the version of a clustered index record which a consistent
read should see. We assume that the trx id stored in rec is such that
the consistent read should not see rec in its present version.
@return error code
@retval DB_SUCCESS if a previous version was fetched
@retval DB_MISSING_HISTORY if the history is missing (a sign of corruption) */
dberr_t
row_vers_build_for_consistent_read(
/*===============================*/
	const rec_t*	rec,	/*!< in: record in a clustered index; the
				caller must have a latch on the page; this
				latch locks the top of the stack of versions
				of this records */
	mtr_t*		mtr,	/*!< in: mtr holding the latch on rec */
	dict_index_t*	index,	/*!< in: the clustered index */
	rec_offs**	offsets,/*!< in/out: offsets returned by
				rec_get_offsets(rec, index) */
	ReadView*	view,	/*!< in: the consistent read view */
	mem_heap_t**	offset_heap,/*!< in/out: memory heap from which
				the offsets are allocated */
	mem_heap_t*	in_heap,/*!< in: memory heap from which the memory for
				*old_vers is allocated; memory for possible
				intermediate versions is allocated and freed
				locally within the function */
	rec_t**		old_vers,/*!< out, own: old version, or NULL
				if the history is missing or the record
				does not exist in the view, that is,
				it was freshly inserted afterwards */
	dtuple_t**	vrow)	/*!< out: virtual row */
{
	const rec_t*	version;
	rec_t*		prev_version;
	trx_id_t	trx_id;
	mem_heap_t*	heap		= NULL;
	byte*		buf;
	dberr_t		err;

	ut_ad(index->is_primary());
	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));

	ut_ad(rec_offs_validate(rec, index, *offsets));

	trx_id = row_get_rec_trx_id(rec, index, *offsets);

	ut_ad(!view->changes_visible(trx_id));

	ut_ad(!vrow || !(*vrow));

	version = rec;

	for (;;) {
		mem_heap_t*	prev_heap = heap;

		heap = mem_heap_create(1024);

		if (vrow) {
			*vrow = NULL;
		}

		/* If purge can't see the record then we can't rely on
		the UNDO log record. */

		err = trx_undo_prev_version_build(
			version, index, *offsets, heap,
			&prev_version, mtr, 0, NULL, vrow);

		if (prev_heap != NULL) {
			mem_heap_free(prev_heap);
		}

		if (prev_version == NULL) {
			/* It was a freshly inserted version */
			*old_vers = NULL;
			ut_ad(!vrow || !(*vrow));
			break;
		}

		*offsets = rec_get_offsets(
			prev_version, index, *offsets,
			index->n_core_fields, ULINT_UNDEFINED, offset_heap);

#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
		ut_a(!rec_offs_any_null_extern(prev_version, *offsets));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

		trx_id = row_get_rec_trx_id(prev_version, index, *offsets);

		if (view->changes_visible(trx_id)) {

			/* The view already sees this version: we can copy
			it to in_heap and return */

			buf = static_cast<byte*>(
				mem_heap_alloc(
					in_heap, rec_offs_size(*offsets)));

			*old_vers = rec_copy(buf, prev_version, *offsets);
			rec_offs_make_valid(*old_vers, index, true, *offsets);

			if (vrow && *vrow) {
				*vrow = dtuple_copy(*vrow, in_heap);
				dtuple_dup_v_fld(*vrow, in_heap);
			}
			break;
		} else if (trx_id >= view->low_limit_id()
			   && trx_id >= trx_sys.get_max_trx_id()) {
			err = DB_CORRUPTION;
			break;
		}
		version = prev_version;
	}

	mem_heap_free(heap);

	return(err);
}

#if defined __aarch64__&&defined __GNUC__&&__GNUC__==4&&!defined __clang__
/* Avoid GCC 4.8.5 internal compiler error "could not split insn". */
# pragma GCC optimize ("O0")
#endif
/*****************************************************************//**
Constructs the last committed version of a clustered index record,
which should be seen by a semi-consistent read. */
void
row_vers_build_for_semi_consistent_read(
/*====================================*/
	const rec_t*	rec,	/*!< in: record in a clustered index; the
				caller must have a latch on the page; this
				latch locks the top of the stack of versions
				of this records */
	mtr_t*		mtr,	/*!< in: mtr holding the latch on rec */
	dict_index_t*	index,	/*!< in: the clustered index */
	rec_offs**	offsets,/*!< in/out: offsets returned by
				rec_get_offsets(rec, index) */
	mem_heap_t**	offset_heap,/*!< in/out: memory heap from which
				the offsets are allocated */
	mem_heap_t*	in_heap,/*!< in: memory heap from which the memory for
				*old_vers is allocated; memory for possible
				intermediate versions is allocated and freed
				locally within the function */
	const rec_t**	old_vers,/*!< out: rec, old version, or NULL if the
				record does not exist in the view, that is,
				it was freshly inserted afterwards */
	dtuple_t**	vrow)	/*!< out: virtual row, old version, or NULL
				if it is not updated in the view */
{
	const rec_t*	version;
	mem_heap_t*	heap		= NULL;
	byte*		buf;
	trx_id_t	rec_trx_id	= 0;

	ut_ad(index->is_primary());
	ut_ad(mtr->memo_contains_page_flagged(rec, MTR_MEMO_PAGE_X_FIX
					      | MTR_MEMO_PAGE_S_FIX));

	ut_ad(rec_offs_validate(rec, index, *offsets));

	version = rec;
	ut_ad(!vrow || !(*vrow));

	for (;;) {
		mem_heap_t*	heap2;
		rec_t*		prev_version;
		trx_id_t	version_trx_id;

		version_trx_id = row_get_rec_trx_id(version, index, *offsets);
		if (rec == version) {
			rec_trx_id = version_trx_id;
		}

		if (!trx_sys.is_registered(mtr->trx, version_trx_id)) {
committed_version_trx:
			/* We found a version that belongs to a
			committed transaction: return it. */

#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
			ut_a(!rec_offs_any_null_extern(version, *offsets));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

			if (rec == version) {
				*old_vers = rec;
				if (vrow) {
					*vrow = NULL;
				}
				break;
			}

			/* We assume that a rolled-back transaction stays in
			TRX_STATE_ACTIVE state until all the changes have been
			rolled back and the transaction is removed from
			the global list of transactions. */

			if (rec_trx_id == version_trx_id) {
				/* The transaction was committed while
				we searched for earlier versions.
				Return the current version as a
				semi-consistent read. */

				version = rec;
				*offsets = rec_get_offsets(
					version, index, *offsets,
					index->n_core_fields, ULINT_UNDEFINED,
					offset_heap);
			}

			buf = static_cast<byte*>(
				mem_heap_alloc(
					in_heap, rec_offs_size(*offsets)));

			*old_vers = rec_copy(buf, version, *offsets);
			rec_offs_make_valid(*old_vers, index, true, *offsets);
			if (vrow && *vrow) {
				*vrow = dtuple_copy(*vrow, in_heap);
				dtuple_dup_v_fld(*vrow, in_heap);
			}
			break;
		}

		DEBUG_SYNC_C("after_row_vers_check_trx_active");

		heap2 = heap;
		heap = mem_heap_create(1024);

		if (trx_undo_prev_version_build(version, index, *offsets, heap,
						&prev_version, mtr, 0,
						in_heap, vrow) != DB_SUCCESS) {
			mem_heap_free(heap);
			heap = heap2;
			heap2 = NULL;
			goto committed_version_trx;
		}

		if (heap2) {
			mem_heap_free(heap2); /* free version */
		}

		if (prev_version == NULL) {
			/* It was a freshly inserted version */
			*old_vers = NULL;
			ut_ad(!vrow || !(*vrow));
			break;
		}

		version = prev_version;
		*offsets = rec_get_offsets(version, index, *offsets,
					   index->n_core_fields,
					   ULINT_UNDEFINED, offset_heap);
#if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
		ut_a(!rec_offs_any_null_extern(version, *offsets));
#endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */
	}/* for (;;) */

	if (heap) {
		mem_heap_free(heap);
	}
}
