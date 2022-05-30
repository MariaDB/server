/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

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
@file row/row0upd.cc
Update of a row

Created 12/27/1996 Heikki Tuuri
*******************************************************/

#include "row0upd.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "trx0undo.h"
#include "rem0rec.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "mach0data.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "que0que.h"
#include "row0ext.h"
#include "row0ins.h"
#include "row0log.h"
#include "row0row.h"
#include "row0sel.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "log0log.h"
#include "pars0sym.h"
#include "eval0eval.h"
#include "buf0lru.h"
#include "trx0rec.h"
#include "fts0fts.h"
#include "fts0types.h"
#include <algorithm>
#include <mysql/plugin.h>
#include <mysql/service_wsrep.h>
#ifdef WITH_WSREP
#include "log.h"
#include "wsrep.h"
#endif /* WITH_WSREP */


/* What kind of latch and lock can we assume when the control comes to
   -------------------------------------------------------------------
an update node?
--------------
Efficiency of massive updates would require keeping an x-latch on a
clustered index page through many updates, and not setting an explicit
x-lock on clustered index records, as they anyway will get an implicit
x-lock when they are updated. A problem is that the read nodes in the
graph should know that they must keep the latch when passing the control
up to the update node, and not set any record lock on the record which
will be updated. Another problem occurs if the execution is stopped,
as the kernel switches to another query thread, or the transaction must
wait for a lock. Then we should be able to release the latch and, maybe,
acquire an explicit x-lock on the record.
	Because this seems too complicated, we conclude that the less
efficient solution of releasing all the latches when the control is
transferred to another node, and acquiring explicit x-locks, is better. */

/* How is a delete performed? If there is a delete without an
explicit cursor, i.e., a searched delete, there are at least
two different situations:
the implicit select cursor may run on (1) the clustered index or
on (2) a secondary index. The delete is performed by setting
the delete bit in the record and substituting the id of the
deleting transaction for the original trx id, and substituting a
new roll ptr for previous roll ptr. The old trx id and roll ptr
are saved in the undo log record. Thus, no physical changes occur
in the index tree structure at the time of the delete. Only
when the undo log is purged, the index records will be physically
deleted from the index trees.

The query graph executing a searched delete would consist of
a delete node which has as a subtree a select subgraph.
The select subgraph should return a (persistent) cursor
in the clustered index, placed on page which is x-latched.
The delete node should look for all secondary index records for
this clustered index entry and mark them as deleted. When is
the x-latch freed? The most efficient way for performing a
searched delete is obviously to keep the x-latch for several
steps of query graph execution. */

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
Checks if an update vector changes some of the first ordering fields of an
index record. This is only used in foreign key checks and we can assume
that index does not contain column prefixes.
@return TRUE if changes */
static
ibool
row_upd_changes_first_fields_binary(
/*================================*/
	dtuple_t*	entry,	/*!< in: old value of index entry */
	dict_index_t*	index,	/*!< in: index of entry */
	const upd_t*	update,	/*!< in: update vector for the row */
	ulint		n);	/*!< in: how many first fields to check */

/*********************************************************************//**
Checks if index currently is mentioned as a referenced index in a foreign
key constraint.

@return true if referenced */
static
bool
row_upd_index_is_referenced(
/*========================*/
	dict_index_t*	index,	/*!< in: index */
	trx_t*		trx)	/*!< in: transaction */
{
  dict_table_t *table= index->table;
  /* The pointers in table->referenced_set are safe to dereference
  thanks to the SQL layer having acquired MDL on all (grand)parent tables. */
  dict_foreign_set::iterator end= table->referenced_set.end();
  return end != std::find_if(table->referenced_set.begin(), end,
                             dict_foreign_with_index(index));
}

#ifdef WITH_WSREP
static
bool
wsrep_row_upd_index_is_foreign(
/*========================*/
	dict_index_t*	index,	/*!< in: index */
	trx_t*		trx)	/*!< in: transaction */
{
  if (!trx->is_wsrep())
    return false;

  dict_table_t *table= index->table;

  if (table->foreign_set.empty())
    return false;

  /* No MDL protects dereferencing the members of table->foreign_set. */
  const bool no_lock= !trx->dict_operation_lock_mode;
  if (no_lock)
    dict_sys.freeze(SRW_LOCK_CALL);

  auto end= table->foreign_set.end();
  const bool is_referenced= end !=
    std::find_if(table->foreign_set.begin(), end,
                 [index](const dict_foreign_t* f)
                 {return f->foreign_index == index;});
  if (no_lock)
    dict_sys.unfreeze();

  return is_referenced;
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Checks if possible foreign key constraints hold after a delete of the record
under pcur.

NOTE that this function will temporarily commit mtr and lose the
pcur position!

@return DB_SUCCESS or an error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_check_references_constraints(
/*=================================*/
	upd_node_t*	node,	/*!< in: row update node */
	btr_pcur_t*	pcur,	/*!< in: cursor positioned on a record; NOTE: the
				cursor position is lost in this function! */
	dict_table_t*	table,	/*!< in: table in question */
	dict_index_t*	index,	/*!< in: index of the cursor */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets(pcur.rec, index) */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr)	/*!< in: mtr */
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;
	dtuple_t*	entry;
	const rec_t*	rec;
	dberr_t		err;

	DBUG_ENTER("row_upd_check_references_constraints");

	if (table->referenced_set.empty()) {
		DBUG_RETURN(DB_SUCCESS);
	}

	rec = btr_pcur_get_rec(pcur);
	ut_ad(rec_offs_validate(rec, index, offsets));

	heap = mem_heap_create(500);

	entry = row_rec_to_index_entry(rec, index, offsets, heap);

	mtr_commit(mtr);

	DEBUG_SYNC_C("foreign_constraint_check_for_update");

	mtr->start();

	DEBUG_SYNC_C_IF_THD(thr_get_trx(thr)->mysql_thd,
			    "foreign_constraint_check_for_insert");

	for (dict_foreign_set::iterator it = table->referenced_set.begin();
	     it != table->referenced_set.end();
	     ++it) {

		foreign = *it;

		/* Note that we may have an update which updates the index
		record, but does NOT update the first fields which are
		referenced in a foreign key constraint. Then the update does
		NOT break the constraint. */

		if (foreign->referenced_index == index
		    && (node->is_delete
			|| row_upd_changes_first_fields_binary(
				entry, index, node->update,
				foreign->n_fields))) {
			dict_table_t*	ref_table = nullptr;

			if (!foreign->foreign_table) {
				ref_table = dict_table_open_on_name(
					foreign->foreign_table_name_lookup,
					false, DICT_ERR_IGNORE_NONE);
			}

			err = row_ins_check_foreign_constraint(
				FALSE, foreign, table, entry, thr);

			if (ref_table) {
				dict_table_close(ref_table);
			}

			if (err != DB_SUCCESS) {
				goto func_exit;
			}
		}
	}

	err = DB_SUCCESS;

func_exit:
	mem_heap_free(heap);

	DEBUG_SYNC_C("foreign_constraint_check_for_update_done");
	DBUG_RETURN(err);
}

#ifdef WITH_WSREP
static
dberr_t
wsrep_row_upd_check_foreign_constraints(
/*=================================*/
	upd_node_t*	node,	/*!< in: row update node */
	btr_pcur_t*	pcur,	/*!< in: cursor positioned on a record; NOTE: the
				cursor position is lost in this function! */
	dict_table_t*	table,	/*!< in: table in question */
	dict_index_t*	index,	/*!< in: index of the cursor */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets(pcur.rec, index) */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr)	/*!< in: mtr */
{
	dict_foreign_t*	foreign;
	mem_heap_t*	heap;
	dtuple_t*	entry;
	const rec_t*	rec;
	dberr_t		err;

	if (table->foreign_set.empty()) {
		return(DB_SUCCESS);
	}

	/* TODO: make native slave thread bail out here */

	rec = btr_pcur_get_rec(pcur);
	ut_ad(rec_offs_validate(rec, index, offsets));

	heap = mem_heap_create(500);

	entry = row_rec_to_index_entry(rec, index, offsets, heap);

	mtr_commit(mtr);

	mtr_start(mtr);

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     it != table->foreign_set.end();
	     ++it) {

		foreign = *it;
		/* Note that we may have an update which updates the index
		record, but does NOT update the first fields which are
		referenced in a foreign key constraint. Then the update does
		NOT break the constraint. */

		if (foreign->foreign_index == index
		    && (node->is_delete
			|| row_upd_changes_first_fields_binary(
				entry, index, node->update,
				foreign->n_fields))) {

			dict_table_t *opened = nullptr;

			if (!foreign->referenced_table) {
				foreign->referenced_table =
					dict_table_open_on_name(
					  foreign->referenced_table_name_lookup,
					  false, DICT_ERR_IGNORE_NONE);
				opened = foreign->referenced_table;
			}

			err = row_ins_check_foreign_constraint(
				TRUE, foreign, table, entry, thr);

			if (opened) {
				dict_table_close(opened);
			}

			if (err != DB_SUCCESS) {
				goto func_exit;
			}
		}
	}

	err = DB_SUCCESS;
func_exit:
	mem_heap_free(heap);

	return(err);
}

/** Determine if a FOREIGN KEY constraint needs to be processed.
@param[in]	node	query node
@param[in]	trx	transaction
@return	whether the node cannot be ignored */

inline bool wsrep_must_process_fk(const upd_node_t* node, const trx_t* trx)
{
	if (!trx->is_wsrep()) {
		return false;
	}
	return que_node_get_type(node->common.parent) != QUE_NODE_UPDATE
		|| static_cast<upd_node_t*>(node->common.parent)->cascade_node
		!= node;
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Creates an update node for a query graph.
@return own: update node */
upd_node_t*
upd_node_create(
/*============*/
	mem_heap_t*	heap)	/*!< in: mem heap where created */
{
	upd_node_t*	node;

	node = static_cast<upd_node_t*>(
		mem_heap_zalloc(heap, sizeof(upd_node_t)));

	node->common.type = QUE_NODE_UPDATE;
	node->state = UPD_NODE_UPDATE_CLUSTERED;
	node->heap = mem_heap_create(128);
	node->magic_n = UPD_NODE_MAGIC_N;

	return(node);
}

/***********************************************************//**
Returns TRUE if row update changes size of some field in index or if some
field to be updated is stored externally in rec or update.
@return TRUE if the update changes the size of some field in index or
the field is external in rec or update */
ibool
row_upd_changes_field_size_or_external(
/*===================================*/
	dict_index_t*	index,	/*!< in: index */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update)	/*!< in: update vector */
{
	const upd_field_t*	upd_field;
	const dfield_t*		new_val;
	ulint			old_len;
	ulint			new_len;
	ulint			n_fields;
	ulint			i;

	ut_ad(rec_offs_validate(NULL, index, offsets));
	ut_ad(!index->table->skip_alter_undo);
	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);

		/* We should ignore virtual field if the index is not
		a virtual index */
		if (upd_fld_is_virtual_col(upd_field)
		    && !index->has_virtual()) {
			continue;
		}

		new_val = &(upd_field->new_val);
		if (dfield_is_ext(new_val)) {
			return(TRUE);
		}
		new_len = dfield_get_len(new_val);
		ut_ad(new_len != UNIV_SQL_DEFAULT);

		if (dfield_is_null(new_val) && !rec_offs_comp(offsets)) {
			new_len = dict_col_get_sql_null_size(
				dict_index_get_nth_col(index,
						       upd_field->field_no),
				0);
		}

		if (rec_offs_nth_default(offsets, upd_field->field_no)) {
			/* This is an instantly added column that is
			at the initial default value. */
			return(TRUE);
		}

		if (rec_offs_comp(offsets)
		    && rec_offs_nth_sql_null(offsets, upd_field->field_no)) {
			/* Note that in the compact table format, for a
			variable length field, an SQL NULL will use zero
			bytes in the offset array at the start of the physical
			record, but a zero-length value (empty string) will
			use one byte! Thus, we cannot use update-in-place
			if we update an SQL NULL varchar to an empty string! */

			old_len = UNIV_SQL_NULL;
		} else {
			old_len = rec_offs_nth_size(offsets,
						    upd_field->field_no);
		}

		if (old_len != new_len
		    || rec_offs_nth_extern(offsets, upd_field->field_no)) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/***********************************************************//**
Returns true if row update contains disowned external fields.
@return true if the update contains disowned external fields. */
bool
row_upd_changes_disowned_external(
/*==============================*/
	const upd_t*	update)	/*!< in: update vector */
{
	const upd_field_t*	upd_field;
	const dfield_t*		new_val;
	ulint			new_len;
	ulint                   n_fields;
	ulint			i;

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		const byte*	field_ref;

		upd_field = upd_get_nth_field(update, i);
		new_val = &(upd_field->new_val);
		new_len = dfield_get_len(new_val);

		if (!dfield_is_ext(new_val)) {
			continue;
		}

		ut_ad(new_len >= BTR_EXTERN_FIELD_REF_SIZE);

		field_ref = static_cast<const byte*>(dfield_get_data(new_val))
			    + new_len - BTR_EXTERN_FIELD_REF_SIZE;

		if (field_ref[BTR_EXTERN_LEN] & BTR_EXTERN_OWNER_FLAG) {
			return(true);
		}
	}

	return(false);
}

/***************************************************************//**
Builds an update vector from those fields which in a secondary index entry
differ from a record that has the equal ordering fields. NOTE: we compare
the fields as binary strings!
@return own: update vector of differing fields */
upd_t*
row_upd_build_sec_rec_difference_binary(
/*====================================*/
	const rec_t*	rec,	/*!< in: secondary index record */
	dict_index_t*	index,	/*!< in: index */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec, index) */
	const dtuple_t*	entry,	/*!< in: entry to insert */
	mem_heap_t*	heap)	/*!< in: memory heap from which allocated */
{
	upd_field_t*	upd_field;
	const dfield_t*	dfield;
	const byte*	data;
	ulint		len;
	upd_t*		update;
	ulint		n_diff;

	/* This function is used only for a secondary index */
	ut_a(!dict_index_is_clust(index));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(rec_offs_n_fields(offsets) == dtuple_get_n_fields(entry));
	ut_ad(!rec_offs_any_extern(offsets));
	ut_ad(!rec_offs_any_default(offsets));
	ut_ad(!index->table->skip_alter_undo);

	update = upd_create(dtuple_get_n_fields(entry), heap);

	n_diff = 0;

	for (uint16_t i = 0; i < dtuple_get_n_fields(entry); i++) {

		data = rec_get_nth_field(rec, offsets, i, &len);

		dfield = dtuple_get_nth_field(entry, i);

		/* NOTE that it may be that len != dfield_get_len(dfield) if we
		are updating in a character set and collation where strings of
		different length can be equal in an alphabetical comparison,
		and also in the case where we have a column prefix index
		and the last characters in the index field are spaces; the
		latter case probably caused the assertion failures reported at
		row0upd.cc line 713 in versions 4.0.14 - 4.0.16. */

		/* NOTE: we compare the fields as binary strings!
		(No collation) */

		if (!dfield_data_is_binary_equal(dfield, len, data)) {

			upd_field = upd_get_nth_field(update, n_diff);

			dfield_copy(&(upd_field->new_val), dfield);

			upd_field_set_field_no(upd_field, i, index);

			n_diff++;
		}
	}

	update->n_fields = n_diff;

	return(update);
}


/** Builds an update vector from those fields, excluding the roll ptr and
trx id fields, which in an index entry differ from a record that has
the equal ordering fields. NOTE: we compare the fields as binary strings!
@param[in]	index		clustered index
@param[in]	entry		clustered index entry to insert
@param[in]	rec		clustered index record
@param[in]	offsets		rec_get_offsets(rec,index), or NULL
@param[in]	no_sys		skip the system columns
				DB_TRX_ID and DB_ROLL_PTR
@param[in]	trx		transaction (for diagnostics),
				or NULL
@param[in]	heap		memory heap from which allocated
@param[in]	mysql_table	NULL, or mysql table object when
				user thread invokes dml
@param[out]	error		error number in case of failure
@return own: update vector of differing fields, excluding roll ptr and
trx id,if error is not equal to DB_SUCCESS, return NULL */
upd_t*
row_upd_build_difference_binary(
	dict_index_t*	index,
	const dtuple_t*	entry,
	const rec_t*	rec,
	const rec_offs*	offsets,
	bool		no_sys,
	trx_t*		trx,
	mem_heap_t*	heap,
	TABLE*		mysql_table,
	dberr_t*	error)
{
	ulint		len;
	upd_t*		update;
	ulint		n_diff;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	const ulint	n_v_fld = dtuple_get_n_v_fields(entry);
	rec_offs_init(offsets_);

	/* This function is used only for a clustered index */
	ut_a(dict_index_is_clust(index));
	ut_ad(!index->table->skip_alter_undo);
	ut_ad(entry->n_fields <= index->n_fields);
	ut_ad(entry->n_fields >= index->n_core_fields);

	update = upd_create(index->n_fields + n_v_fld, heap);

	n_diff = 0;

	if (!offsets) {
		offsets = rec_get_offsets(rec, index, offsets_,
					  index->n_core_fields,
					  ULINT_UNDEFINED, &heap);
	} else {
		ut_ad(rec_offs_validate(rec, index, offsets));
	}

	for (uint16_t i = 0; i < entry->n_fields; i++) {
		const byte* data = rec_get_nth_cfield(rec, index, offsets, i,
						      &len);
		const dfield_t* dfield = dtuple_get_nth_field(entry, i);

		/* NOTE: we compare the fields as binary strings!
		(No collation) */
		if (no_sys && (i == index->db_trx_id()
			       || i == index->db_roll_ptr())) {
			continue;
		}

		if (!dfield_is_ext(dfield)
		    != !rec_offs_nth_extern(offsets, i)
		    || !dfield_data_is_binary_equal(dfield, len, data)) {
			upd_field_t* uf = upd_get_nth_field(update, n_diff++);
			dfield_copy(&uf->new_val, dfield);
			upd_field_set_field_no(uf, i, index);
		}
	}

	for (uint16_t i = static_cast<uint16_t>(entry->n_fields);
	     i < index->n_fields; i++) {
		upd_field_t* uf = upd_get_nth_field(update, n_diff++);
		const dict_col_t* col = dict_index_get_nth_col(index, i);
		/* upd_create() zero-initialized uf */
		uf->new_val.data = const_cast<byte*>(col->instant_value(&len));
		uf->new_val.len = static_cast<unsigned>(len);
		dict_col_copy_type(col, &uf->new_val.type);
		upd_field_set_field_no(uf, i, index);
	}

	/* Check the virtual columns updates. Even if there is no non-virtual
	column (base columns) change, we will still need to build the
	indexed virtual column value so that undo log would log them (
	for purge/mvcc purpose) */
	if (n_v_fld > 0) {
		row_ext_t*	ext;
		THD*		thd;

		if (trx == NULL) {
			thd = current_thd;
		} else {
			thd = trx->mysql_thd;
		}

		ut_ad(!update->old_vrow);

		ib_vcol_row vc(NULL);
		uchar *record = vc.record(thd, index, &mysql_table);

		for (uint16_t i = 0; i < n_v_fld; i++) {
			const dict_v_col_t*     col
                                = dict_table_get_nth_v_col(index->table, i);

			if (!col->m_col.ord_part) {
				continue;
			}

			if (update->old_vrow == NULL) {
				update->old_vrow = row_build(
					ROW_COPY_POINTERS, index, rec, offsets,
					index->table, NULL, NULL, &ext, heap);
			}

			dfield_t*	vfield = innobase_get_computed_value(
				update->old_vrow, col, index,
				&vc.heap, heap, NULL, thd, mysql_table, record,
				NULL, NULL);
			if (vfield == NULL) {
				*error = DB_COMPUTE_VALUE_FAILED;
				return(NULL);
			}

			const dfield_t* dfield = dtuple_get_nth_v_field(
				entry, i);

			if (!dfield_data_is_binary_equal(
				    dfield, vfield->len,
				    static_cast<byte*>(vfield->data))) {
				upd_field_t* uf = upd_get_nth_field(update,
								    n_diff++);
				uf->old_v_val = static_cast<dfield_t*>(
					mem_heap_alloc(heap,
						       sizeof *uf->old_v_val));
				dfield_copy(uf->old_v_val, vfield);
				dfield_copy(&uf->new_val, dfield);
				upd_field_set_v_field_no(uf, i, index);
			}
		}
	}

	update->n_fields = n_diff;
	ut_ad(update->validate());

	return(update);
}

/** Fetch a prefix of an externally stored column.
This is similar to row_ext_lookup(), but the row_ext_t holds the old values
of the column and must not be poisoned with the new values.
@param[in]	data		'internally' stored part of the field
containing also the reference to the external part
@param[in]	local_len	length of data, in bytes
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	len		input - length of prefix to
fetch; output: fetched length of the prefix
@param[in,out]	heap		heap where to allocate
@return BLOB prefix
@retval NULL if the record is incomplete (should only happen
in row_vers_vc_matches_cluster() executed concurrently with another purge) */
static
byte*
row_upd_ext_fetch(
	const byte*		data,
	ulint			local_len,
	ulint			zip_size,
	ulint*			len,
	mem_heap_t*		heap)
{
	byte*	buf = static_cast<byte*>(mem_heap_alloc(heap, *len));

	*len = btr_copy_externally_stored_field_prefix(
		buf, *len, zip_size, data, local_len);

	return *len ? buf : NULL;
}

/** Replaces the new column value stored in the update vector in
the given index entry field.
@param[in,out]	dfield		data field of the index entry
@param[in]	field		index field
@param[in]	col		field->col
@param[in]	uf		update field
@param[in,out]	heap		memory heap for allocating and copying
the new value
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@return whether the previous version was built successfully */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
bool
row_upd_index_replace_new_col_val(
	dfield_t*		dfield,
	const dict_field_t*	field,
	const dict_col_t*	col,
	const upd_field_t*	uf,
	mem_heap_t*		heap,
	ulint			zip_size)
{
	ulint		len;
	const byte*	data;

	dfield_copy_data(dfield, &uf->new_val);

	if (dfield_is_null(dfield)) {
		return true;
	}

	len = dfield_get_len(dfield);
	data = static_cast<const byte*>(dfield_get_data(dfield));

	if (field->prefix_len > 0) {
		ibool		fetch_ext = dfield_is_ext(dfield)
			&& len < (ulint) field->prefix_len
			+ BTR_EXTERN_FIELD_REF_SIZE;

		if (fetch_ext) {
			ulint	l = len;

			len = field->prefix_len;

			data = row_upd_ext_fetch(data, l, zip_size,
						 &len, heap);
			if (UNIV_UNLIKELY(!data)) {
				return false;
			}
		}

		len = dtype_get_at_most_n_mbchars(col->prtype,
						  col->mbminlen, col->mbmaxlen,
						  field->prefix_len, len,
						  (const char*) data);

		dfield_set_data(dfield, data, len);

		if (!fetch_ext) {
			dfield_dup(dfield, heap);
		}

		return true;
	}

	switch (uf->orig_len) {
		byte*	buf;
	case BTR_EXTERN_FIELD_REF_SIZE:
		/* Restore the original locally stored
		part of the column.  In the undo log,
		InnoDB writes a longer prefix of externally
		stored columns, so that column prefixes
		in secondary indexes can be reconstructed. */
		dfield_set_data(dfield,
				data + len - BTR_EXTERN_FIELD_REF_SIZE,
				BTR_EXTERN_FIELD_REF_SIZE);
		dfield_set_ext(dfield);
		/* fall through */
	case 0:
		dfield_dup(dfield, heap);
		break;
	default:
		/* Reconstruct the original locally
		stored part of the column.  The data
		will have to be copied. */
		ut_a(uf->orig_len > BTR_EXTERN_FIELD_REF_SIZE);
		buf = static_cast<byte*>(mem_heap_alloc(heap, uf->orig_len));

		/* Copy the locally stored prefix. */
		memcpy(buf, data,
		       unsigned(uf->orig_len) - BTR_EXTERN_FIELD_REF_SIZE);

		/* Copy the BLOB pointer. */
		memcpy(buf + uf->orig_len - BTR_EXTERN_FIELD_REF_SIZE,
		       data + len - BTR_EXTERN_FIELD_REF_SIZE,
		       BTR_EXTERN_FIELD_REF_SIZE);

		dfield_set_data(dfield, buf, uf->orig_len);
		dfield_set_ext(dfield);
		break;
	}

	return true;
}

/** Apply an update vector to an metadata entry.
@param[in,out]	entry	clustered index metadata record to be updated
@param[in]	index	index of the entry
@param[in]	update	update vector built for the entry
@param[in,out]	heap	memory heap for copying off-page columns */
static
void
row_upd_index_replace_metadata(
	dtuple_t*		entry,
	const dict_index_t*	index,
	const upd_t*		update,
	mem_heap_t*		heap)
{
	ut_ad(!index->table->skip_alter_undo);
	ut_ad(update->is_alter_metadata());
	ut_ad(entry->info_bits == update->info_bits);
	ut_ad(entry->n_fields == ulint(index->n_fields) + 1);
	const ulint zip_size = index->table->space->zip_size();
	const ulint first = index->first_user_field();
	ut_d(bool found_mblob = false);

	for (ulint i = upd_get_n_fields(update); i--; ) {
		const upd_field_t* uf = upd_get_nth_field(update, i);
		ut_ad(!upd_fld_is_virtual_col(uf));
		ut_ad(uf->field_no >= first - 2);
		ulint f = uf->field_no;
		dfield_t* dfield = dtuple_get_nth_field(entry, f);

		if (f == first) {
			ut_d(found_mblob = true);
			ut_ad(!dfield_is_null(&uf->new_val));
			ut_ad(dfield_is_ext(dfield));
			ut_ad(dfield_get_len(dfield) == FIELD_REF_SIZE);
			ut_ad(!dfield_is_null(dfield));
			dfield_set_data(dfield, uf->new_val.data,
					uf->new_val.len);
			if (dfield_is_ext(&uf->new_val)) {
				dfield_set_ext(dfield);
			}
			continue;
		}

		f -= f > first;
		const dict_field_t* field = dict_index_get_nth_field(index, f);
		if (!row_upd_index_replace_new_col_val(dfield, field,
						       field->col,
						       uf, heap, zip_size)) {
			ut_error;
		}
	}

	ut_ad(found_mblob);
}

/** Apply an update vector to an index entry.
@param[in,out]	entry	index entry to be updated; the clustered index record
			must be covered by a lock or a page latch to prevent
			deletion (rollback or purge)
@param[in]	index	index of the entry
@param[in]	update	update vector built for the entry
@param[in,out]	heap	memory heap for copying off-page columns */
void
row_upd_index_replace_new_col_vals_index_pos(
	dtuple_t*		entry,
	const dict_index_t*	index,
	const upd_t*		update,
	mem_heap_t*		heap)
{
	ut_ad(!index->table->skip_alter_undo);
	ut_ad(!entry->is_metadata() || entry->info_bits == update->info_bits);

	if (UNIV_UNLIKELY(entry->is_alter_metadata())) {
		row_upd_index_replace_metadata(entry, index, update, heap);
		return;
	}

	const ulint zip_size = index->table->space->zip_size();

	dtuple_set_info_bits(entry, update->info_bits);

	for (uint16_t i = index->n_fields; i--; ) {
		const dict_field_t*	field;
		const dict_col_t*	col;
		const upd_field_t*	uf;

		field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(field);
		if (col->is_virtual()) {
			const dict_v_col_t*	vcol = reinterpret_cast<
							const dict_v_col_t*>(
								col);

			uf = upd_get_field_by_field_no(
				update, vcol->v_pos, true);
		} else {
			uf = upd_get_field_by_field_no(
				update, i, false);
		}

		if (uf && UNIV_UNLIKELY(!row_upd_index_replace_new_col_val(
						dtuple_get_nth_field(entry, i),
						field, col, uf, heap,
						zip_size))) {
			ut_error;
		}
	}
}

/** Replace the new column values stored in the update vector,
during trx_undo_prev_version_build().
@param entry   clustered index tuple where the values are replaced
               (the clustered index leaf page latch must be held)
@param index   clustered index
@param update  update vector for the clustered index
@param heap    memory heap for allocating and copying values
@return whether the previous version was built successfully */
bool
row_upd_index_replace_new_col_vals(dtuple_t *entry, const dict_index_t &index,
                                   const upd_t *update, mem_heap_t *heap)
{
  ut_ad(index.is_primary());
  const ulint zip_size= index.table->space->zip_size();

  ut_ad(!index.table->skip_alter_undo);
  dtuple_set_info_bits(entry, update->info_bits);

  for (ulint i= 0; i < index.n_fields; i++)
  {
   const dict_field_t *field= &index.fields[i];
   const dict_col_t* col= dict_field_get_col(field);
   const upd_field_t *uf;

   if (col->is_virtual())
   {
     const dict_v_col_t *vcol= reinterpret_cast<const dict_v_col_t*>(col);
     uf= upd_get_field_by_field_no(update, vcol->v_pos, true);
   }
   else
     uf= upd_get_field_by_field_no(update, static_cast<uint16_t>
                                   (dict_col_get_clust_pos(col, &index)),
                                   false);

   if (!uf)
     continue;

   if (!row_upd_index_replace_new_col_val(dtuple_get_nth_field(entry, i),
                                          field, col, uf, heap, zip_size))
     return false;
  }

  return true;
}

/** Replaces the virtual column values stored in the update vector.
@param[in,out]	row	row whose column to be set
@param[in]	field	data to set
@param[in]	len	data length
@param[in]	vcol	virtual column info */
static
void
row_upd_set_vcol_data(
	dtuple_t*		row,
	const byte*             field,
	ulint                   len,
	dict_v_col_t*		vcol)
{
	dfield_t*	dfield = dtuple_get_nth_v_field(row, vcol->v_pos);

	if (dfield_get_type(dfield)->mtype == DATA_MISSING) {
		dict_col_copy_type(&vcol->m_col, dfield_get_type(dfield));

		dfield_set_data(dfield, field, len);
	}
}

/** Replaces the virtual column values stored in a dtuple with that of
a update vector.
@param[in,out]	row	row whose column to be updated
@param[in]	table	table
@param[in]	update	an update vector built for the clustered index
@param[in]	upd_new	update to new or old value
@param[in,out]	undo_row undo row (if needs to be updated)
@param[in]	ptr	remaining part in update undo log */
void
row_upd_replace_vcol(
	dtuple_t*		row,
	const dict_table_t*	table,
	const upd_t*		update,
	bool			upd_new,
	dtuple_t*		undo_row,
	const byte*		ptr)
{
	ulint			col_no;
	ulint			i;
	ulint			n_cols;

	ut_ad(!table->skip_alter_undo);

	n_cols = dtuple_get_n_v_fields(row);
	for (col_no = 0; col_no < n_cols; col_no++) {
		dfield_t*		dfield;

		const dict_v_col_t*	col
			= dict_table_get_nth_v_col(table, col_no);

		/* If there is no index on the column, do not bother for
		value update */
		if (!col->m_col.ord_part) {
			continue;
		}

		dfield = dtuple_get_nth_v_field(row, col_no);

		for (i = 0; i < upd_get_n_fields(update); i++) {
			const upd_field_t*	upd_field
				= upd_get_nth_field(update, i);
			if (!upd_fld_is_virtual_col(upd_field)
			    || upd_field->field_no != col->v_pos) {
				continue;
			}

			if (upd_new) {
				dfield_copy_data(dfield, &upd_field->new_val);
			} else {
				dfield_copy_data(dfield, upd_field->old_v_val);
			}

			dfield->type = upd_field->new_val.type;
			break;
		}
	}

	bool	first_v_col = true;
	bool	is_undo_log = true;

	/* We will read those unchanged (but indexed) virtual columns in */
	if (ptr != NULL) {
		const byte*	end_ptr;

		end_ptr = ptr + mach_read_from_2(ptr);
		ptr += 2;

		while (ptr != end_ptr) {
			const byte* field;
			uint32_t field_no, len, orig_len;

			field_no = mach_read_next_compressed(&ptr);

			const bool is_v = (field_no >= REC_MAX_N_FIELDS);

			if (is_v) {
				ptr = trx_undo_read_v_idx(
					table, ptr, first_v_col, &is_undo_log,
					&field_no);
				first_v_col = false;
			}

			ptr = trx_undo_rec_get_col_val(
				ptr, &field, &len, &orig_len);

			if (field_no == FIL_NULL) {
				ut_ad(is_v);
				continue;
			}

			if (is_v) {
				dict_v_col_t* vcol = dict_table_get_nth_v_col(
							table, field_no);

				row_upd_set_vcol_data(row, field, len, vcol);

				if (undo_row) {
					row_upd_set_vcol_data(
						undo_row, field, len, vcol);
				}
			}
			ut_ad(ptr<= end_ptr);
		}
	}
}

/***********************************************************//**
Replaces the new column values stored in the update vector. */
void
row_upd_replace(
/*============*/
	dtuple_t*		row,	/*!< in/out: row where replaced,
					indexed by col_no;
					the clustered index record must be
					covered by a lock or a page latch to
					prevent deletion (rollback or purge) */
	row_ext_t**		ext,	/*!< out, own: NULL, or externally
					stored column prefixes */
	const dict_index_t*	index,	/*!< in: clustered index */
	const upd_t*		update,	/*!< in: an update vector built for the
					clustered index */
	mem_heap_t*		heap)	/*!< in: memory heap */
{
	ulint			col_no;
	ulint			i;
	ulint			n_cols;
	ulint			n_ext_cols;
	ulint*			ext_cols;
	const dict_table_t*	table;

	ut_ad(row);
	ut_ad(ext);
	ut_ad(index);
	ut_ad(dict_index_is_clust(index));
	ut_ad(update);
	ut_ad(heap);
	ut_ad(update->validate());

	n_cols = dtuple_get_n_fields(row);
	table = index->table;
	ut_ad(n_cols == dict_table_get_n_cols(table));

	ext_cols = static_cast<ulint*>(
		mem_heap_alloc(heap, n_cols * sizeof *ext_cols));

	n_ext_cols = 0;

	dtuple_set_info_bits(row, update->info_bits);

	for (col_no = 0; col_no < n_cols; col_no++) {

		const dict_col_t*	col
			= dict_table_get_nth_col(table, col_no);
		const ulint		clust_pos
			= dict_col_get_clust_pos(col, index);
		dfield_t*		dfield;

		if (UNIV_UNLIKELY(clust_pos == ULINT_UNDEFINED)) {

			continue;
		}

		dfield = dtuple_get_nth_field(row, col_no);

		for (i = 0; i < upd_get_n_fields(update); i++) {

			const upd_field_t*	upd_field
				= upd_get_nth_field(update, i);

			if (upd_field->field_no != clust_pos
			    || upd_fld_is_virtual_col(upd_field)) {

				continue;
			}

			dfield_copy_data(dfield, &upd_field->new_val);
			break;
		}

		if (dfield_is_ext(dfield) && col->ord_part) {
			ext_cols[n_ext_cols++] = col_no;
		}
	}

	if (n_ext_cols) {
		*ext = row_ext_create(n_ext_cols, ext_cols, *table, row, heap);
	} else {
		*ext = NULL;
	}

	row_upd_replace_vcol(row, table, update, true, NULL, NULL);
}

/***********************************************************//**
Checks if an update vector changes an ordering field of an index record.

This function is fast if the update vector is short or the number of ordering
fields in the index is small. Otherwise, this can be quadratic.
NOTE: we compare the fields as binary strings!
@return TRUE if update vector changes an ordering field in the index record */
ibool
row_upd_changes_ord_field_binary_func(
/*==================================*/
	dict_index_t*	index,	/*!< in: index of the record */
	const upd_t*	update,	/*!< in: update vector for the row; NOTE: the
				field numbers in this MUST be clustered index
				positions! */
#ifdef UNIV_DEBUG
	const que_thr_t*thr,	/*!< in: query thread */
#endif /* UNIV_DEBUG */
	const dtuple_t*	row,	/*!< in: old value of row, or NULL if the
				row and the data values in update are not
				known when this function is called, e.g., at
				compile time */
	const row_ext_t*ext,	/*!< NULL, or prefixes of the externally
				stored columns in the old row */
	ulint		flag)	/*!< in: ROW_BUILD_NORMAL,
				ROW_BUILD_FOR_PURGE or ROW_BUILD_FOR_UNDO */
{
	ulint			n_unique;
	ulint			i;
	const dict_index_t*	clust_index;

	ut_ad(thr);
	ut_ad(thr->graph);
	ut_ad(thr->graph->trx);
	ut_ad(!index->table->skip_alter_undo);

	n_unique = dict_index_get_n_unique(index);

	clust_index = dict_table_get_first_index(index->table);

	for (i = 0; i < n_unique; i++) {

		const dict_field_t*	ind_field;
		const dict_col_t*	col;
		ulint			col_no;
		const upd_field_t*	upd_field;
		const dfield_t*		dfield;
		dfield_t		dfield_ext;
		ulint			dfield_len= 0;
		const byte*		buf;
		bool			is_virtual;
		const dict_v_col_t*	vcol = NULL;

		ind_field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(ind_field);
		col_no = dict_col_get_no(col);
		is_virtual = col->is_virtual();

		if (is_virtual) {
			vcol = reinterpret_cast<const dict_v_col_t*>(col);

			upd_field = upd_get_field_by_field_no(
				update, vcol->v_pos, true);
		} else {
			upd_field = upd_get_field_by_field_no(
				update, static_cast<uint16_t>(
					dict_col_get_clust_pos(
						col, clust_index)),
				false);
		}

		if (upd_field == NULL) {
			continue;
		}

		if (row == NULL) {
			ut_ad(ext == NULL);
			return(TRUE);
		}

		if (is_virtual) {
			dfield = dtuple_get_nth_v_field(
				row,  vcol->v_pos);
		} else {
			dfield = dtuple_get_nth_field(row, col_no);
		}

		/* For spatial index update, since the different geometry
		data could generate same MBR, so, if the new index entry is
		same as old entry, which means the MBR is not changed, we
		don't need to do anything. */
		if (dict_index_is_spatial(index) && i == 0) {
			double		mbr1[SPDIMS * 2];
			double		mbr2[SPDIMS * 2];
			rtr_mbr_t*	old_mbr;
			rtr_mbr_t*	new_mbr;
			const uchar*	dptr = NULL;
			ulint		flen = 0;
			ulint		dlen = 0;
			mem_heap_t*	temp_heap = NULL;
			const dfield_t*	new_field = &upd_field->new_val;

			const ulint zip_size = ext
				? ext->zip_size
				: index->table->space->zip_size();

			ut_ad(dfield->data != NULL
			      && dfield->len > GEO_DATA_HEADER_SIZE);
			ut_ad(dict_col_get_spatial_status(col) != SPATIAL_NONE);

			/* Get the old mbr. */
			if (dfield_is_ext(dfield)) {
				/* For off-page stored data, we
				need to read the whole field data. */
				flen = dfield_get_len(dfield);
				dptr = static_cast<const byte*>(
					dfield_get_data(dfield));
				temp_heap = mem_heap_create(1000);

				dptr = btr_copy_externally_stored_field(
					&dlen, dptr,
					zip_size,
					flen,
					temp_heap);
			} else {
				dptr = static_cast<const uchar*>(dfield->data);
				dlen = dfield->len;
			}

			rtree_mbr_from_wkb(dptr + GEO_DATA_HEADER_SIZE,
					   static_cast<uint>(dlen
					   - GEO_DATA_HEADER_SIZE),
					   SPDIMS, mbr1);
			old_mbr = reinterpret_cast<rtr_mbr_t*>(mbr1);

			/* Get the new mbr. */
			if (dfield_is_ext(new_field)) {
				if (flag == ROW_BUILD_FOR_UNDO
				    && dict_table_has_atomic_blobs(
					    index->table)) {
					/* For ROW_FORMAT=DYNAMIC
					or COMPRESSED, a prefix of
					off-page records is stored
					in the undo log record
					(for any column prefix indexes).
					For SPATIAL INDEX, we must
					ignore this prefix. The
					full column value is stored in
					the BLOB.
					For non-spatial index, we
					would have already fetched a
					necessary prefix of the BLOB,
					available in the "ext" parameter.

					Here, for SPATIAL INDEX, we are
					fetching the full column, which is
					potentially wasting a lot of I/O,
					memory, and possibly involving a
					concurrency problem, similar to ones
					that existed before the introduction
					of row_ext_t.

					MDEV-11657 FIXME: write the MBR
					directly to the undo log record,
					and avoid recomputing it here! */
					flen = BTR_EXTERN_FIELD_REF_SIZE;
					ut_ad(dfield_get_len(new_field) >=
					      BTR_EXTERN_FIELD_REF_SIZE);
					dptr = static_cast<const byte*>(
						dfield_get_data(new_field))
						+ dfield_get_len(new_field)
						- BTR_EXTERN_FIELD_REF_SIZE;
				} else {
					flen = dfield_get_len(new_field);
					dptr = static_cast<const byte*>(
						dfield_get_data(new_field));
				}

				if (temp_heap == NULL) {
					temp_heap = mem_heap_create(1000);
				}

				dptr = btr_copy_externally_stored_field(
					&dlen, dptr,
					zip_size,
					flen,
					temp_heap);
			} else {
				dptr = static_cast<const byte*>(
					upd_field->new_val.data);
				dlen = upd_field->new_val.len;
			}
			rtree_mbr_from_wkb(dptr + GEO_DATA_HEADER_SIZE,
					   static_cast<uint>(dlen
					   - GEO_DATA_HEADER_SIZE),
					   SPDIMS, mbr2);
			new_mbr = reinterpret_cast<rtr_mbr_t*>(mbr2);

			if (temp_heap) {
				mem_heap_free(temp_heap);
			}

			if (!MBR_EQUAL_CMP(old_mbr, new_mbr)) {
				return(TRUE);
			} else {
				continue;
			}
		}

		/* This treatment of column prefix indexes is loosely
		based on row_build_index_entry(). */

		if (UNIV_LIKELY(ind_field->prefix_len == 0)
		    || dfield_is_null(dfield)) {
			/* do nothing special */
		} else if (ext) {
			/* Silence a compiler warning without
			silencing a Valgrind error. */
			dfield_len = 0;
			MEM_UNDEFINED(&dfield_len, sizeof dfield_len);
			/* See if the column is stored externally. */
			buf = row_ext_lookup(ext, col_no, &dfield_len);

			ut_ad(col->ord_part);

			if (UNIV_LIKELY_NULL(buf)) {
				if (UNIV_UNLIKELY(buf == field_ref_zero)) {
					/* The externally stored field
					was not written yet. This
					record should only be seen by
					trx_rollback_recovered()
					when the server had crashed before
					storing the field. */
					ut_ad(thr->graph->trx->is_recovered);
					ut_ad(thr->graph->trx
					      == trx_roll_crash_recv_trx);
					return(TRUE);
				}

				goto copy_dfield;
			}
		} else if (dfield_is_ext(dfield)) {
			dfield_len = dfield_get_len(dfield);
			ut_a(dfield_len > BTR_EXTERN_FIELD_REF_SIZE);
			dfield_len -= BTR_EXTERN_FIELD_REF_SIZE;
			ut_a(dict_index_is_clust(index)
			     || ind_field->prefix_len <= dfield_len);

			buf= static_cast<const byte*>(dfield_get_data(dfield));
copy_dfield:
			ut_a(dfield_len > 0);
			dfield_copy(&dfield_ext, dfield);
			dfield_set_data(&dfield_ext, buf, dfield_len);
			dfield = &dfield_ext;
		}

		if (!dfield_datas_are_binary_equal(
			    dfield, &upd_field->new_val,
			    ind_field->prefix_len)) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/***********************************************************//**
Checks if an update vector changes an ordering field of an index record.
NOTE: we compare the fields as binary strings!
@return TRUE if update vector may change an ordering field in an index
record */
ibool
row_upd_changes_some_index_ord_field_binary(
/*========================================*/
	const dict_table_t*	table,	/*!< in: table */
	const upd_t*		update)	/*!< in: update vector for the row */
{
	upd_field_t*	upd_field;
	dict_index_t*	index;
	ulint		i;

	index = dict_table_get_first_index(table);

	for (i = 0; i < upd_get_n_fields(update); i++) {

		upd_field = upd_get_nth_field(update, i);

		if (upd_fld_is_virtual_col(upd_field)) {
			if (dict_table_get_nth_v_col(index->table,
						     upd_field->field_no)
			    ->m_col.ord_part) {
				return(TRUE);
			}
		} else {
			if (dict_field_get_col(dict_index_get_nth_field(
				index, upd_field->field_no))->ord_part) {
				return(TRUE);
			}
		}
	}

	return(FALSE);
}

/***********************************************************//**
Checks if an FTS Doc ID column is affected by an UPDATE.
@return whether the Doc ID column is changed */
bool
row_upd_changes_doc_id(
/*===================*/
	dict_table_t*	table,		/*!< in: table */
	upd_field_t*	upd_field)	/*!< in: field to check */
{
	ulint		col_no;
	dict_index_t*	clust_index;
	fts_t*		fts = table->fts;

	ut_ad(!table->skip_alter_undo);

	clust_index = dict_table_get_first_index(table);

	/* Convert from index-specific column number to table-global
	column number. */
	col_no = dict_index_get_nth_col_no(clust_index, upd_field->field_no);

	return(col_no == fts->doc_col);
}
/***********************************************************//**
Checks if an FTS indexed column is affected by an UPDATE.
@return offset within fts_t::indexes if FTS indexed column updated else
ULINT_UNDEFINED */
ulint
row_upd_changes_fts_column(
/*=======================*/
	dict_table_t*	table,		/*!< in: table */
	upd_field_t*	upd_field)	/*!< in: field to check */
{
	ulint		col_no;
	dict_index_t*	clust_index;
	fts_t*		fts = table->fts;

	ut_ad(!table->skip_alter_undo);

	if (upd_fld_is_virtual_col(upd_field)) {
		col_no = upd_field->field_no;
		return(dict_table_is_fts_column(fts->indexes, col_no, true));
	} else {
		clust_index = dict_table_get_first_index(table);

		/* Convert from index-specific column number to table-global
		column number. */
		col_no = dict_index_get_nth_col_no(clust_index,
						   upd_field->field_no);
		return(dict_table_is_fts_column(fts->indexes, col_no, false));
	}

}

/***********************************************************//**
Checks if an update vector changes some of the first ordering fields of an
index record. This is only used in foreign key checks and we can assume
that index does not contain column prefixes.
@return TRUE if changes */
static
ibool
row_upd_changes_first_fields_binary(
/*================================*/
	dtuple_t*	entry,	/*!< in: index entry */
	dict_index_t*	index,	/*!< in: index of entry */
	const upd_t*	update,	/*!< in: update vector for the row */
	ulint		n)	/*!< in: how many first fields to check */
{
	ulint		n_upd_fields;
	ulint		i, j;
	dict_index_t*	clust_index;

	ut_ad(update && index);
	ut_ad(n <= dict_index_get_n_fields(index));

	n_upd_fields = upd_get_n_fields(update);
	clust_index = dict_table_get_first_index(index->table);

	for (i = 0; i < n; i++) {

		const dict_field_t*	ind_field;
		const dict_col_t*	col;
		ulint			col_pos;

		ind_field = dict_index_get_nth_field(index, i);
		col = dict_field_get_col(ind_field);
		col_pos = dict_col_get_clust_pos(col, clust_index);

		ut_a(ind_field->prefix_len == 0);

		for (j = 0; j < n_upd_fields; j++) {

			upd_field_t*	upd_field
				= upd_get_nth_field(update, j);

			if (col_pos == upd_field->field_no
			    && !dfield_datas_are_binary_equal(
				    dtuple_get_nth_field(entry, i),
				    &upd_field->new_val, 0)) {

				return(TRUE);
			}
		}
	}

	return(FALSE);
}

/*********************************************************************//**
Copies the column values from a record. */
UNIV_INLINE
void
row_upd_copy_columns(
/*=================*/
	rec_t*		rec,	/*!< in: record in a clustered index */
	const rec_offs*	offsets,/*!< in: array returned by rec_get_offsets() */
	const dict_index_t*	index, /*!< in: index of rec */
	sym_node_t*	column)	/*!< in: first column in a column list, or
				NULL */
{
	ut_ad(dict_index_is_clust(index));

	const byte*	data;
	ulint	len;

	while (column) {
		data = rec_get_nth_cfield(
			rec, index, offsets,
			column->field_nos[SYM_CLUST_FIELD_NO], &len);
		eval_node_copy_and_alloc_val(column, data, len);

		column = UT_LIST_GET_NEXT(col_var_list, column);
	}
}

/*********************************************************************//**
Calculates the new values for fields to update. Note that row_upd_copy_columns
must have been called first. */
UNIV_INLINE
void
row_upd_eval_new_vals(
/*==================*/
	upd_t*	update)	/*!< in/out: update vector */
{
	que_node_t*	exp;
	upd_field_t*	upd_field;
	ulint		n_fields;
	ulint		i;

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		upd_field = upd_get_nth_field(update, i);

		exp = upd_field->exp;

		eval_exp(exp);

		dfield_copy_data(&(upd_field->new_val), que_node_get_val(exp));
	}
}

/** Stores to the heap the virtual columns that need for any indexes
@param[in,out]	node		row update node
@param[in]	update		an update vector if it is update
@param[in]	thd		mysql thread handle
@param[in,out]	mysql_table	mysql table object
@return true if success
	false if virtual column value computation fails. */
static
bool
row_upd_store_v_row(
	upd_node_t*	node,
	const upd_t*	update,
	THD*		thd,
	TABLE*		mysql_table)
{
	dict_index_t*	index = dict_table_get_first_index(node->table);
	ib_vcol_row	vc(NULL);

	for (ulint col_no = 0; col_no < dict_table_get_n_v_cols(node->table);
	     col_no++) {

		const dict_v_col_t*     col
			= dict_table_get_nth_v_col(node->table, col_no);

		if (col->m_col.ord_part) {
			dfield_t*	dfield
				= dtuple_get_nth_v_field(node->row, col_no);
			ulint		n_upd
				= update ? upd_get_n_fields(update) : 0;
			ulint		i = 0;

			/* Check if the value is already in update vector */
			for (i = 0; i < n_upd; i++) {
				const upd_field_t*      upd_field
					= upd_get_nth_field(update, i);
				if (!(upd_field->new_val.type.prtype
				      & DATA_VIRTUAL)
				    || upd_field->field_no != col->v_pos) {
					continue;
				}

				dfield_copy_data(dfield, upd_field->old_v_val);
				dfield_dup(dfield, node->heap);
				break;
			}

			/* Not updated */
			if (i >= n_upd) {
				/* If this is an update, then the value
				should be in update->old_vrow */
				if (update) {
					if (update->old_vrow == NULL) {
						/* This only happens in
						cascade update. And virtual
						column can't be affected,
						so it is Ok to set it to NULL */
						dfield_set_null(dfield);
					} else {
						dfield_t*       vfield
							= dtuple_get_nth_v_field(
								update->old_vrow,
								col_no);
						dfield_copy_data(dfield, vfield);
						dfield_dup(dfield, node->heap);
					}
				} else {
					uchar *record = vc.record(thd, index,
								  &mysql_table);
					/* Need to compute, this happens when
					deleting row */
					dfield_t* vfield =
						innobase_get_computed_value(
							node->row, col, index,
							&vc.heap, node->heap,
							NULL, thd, mysql_table,
							record, NULL, NULL);
					if (vfield == NULL) {
						return false;
					}
				}
			}
		}
	}

	return true;
}

/** Stores to the heap the row on which the node->pcur is positioned.
@param[in]	node		row update node
@param[in]	thd		mysql thread handle
@param[in,out]	mysql_table	NULL, or mysql table object when
				user thread invokes dml
@return false if virtual column value computation fails
	true otherwise. */
static
bool
row_upd_store_row(
	upd_node_t*	node,
	THD*		thd,
	TABLE*		mysql_table)
{
	dict_index_t*	clust_index;
	rec_t*		rec;
	mem_heap_t*	heap		= NULL;
	row_ext_t**	ext;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	const rec_offs*	offsets;
	rec_offs_init(offsets_);

	ut_ad(node->pcur->latch_mode != BTR_NO_LATCHES);

	if (node->row != NULL) {
		mem_heap_empty(node->heap);
	}

	clust_index = dict_table_get_first_index(node->table);

	rec = btr_pcur_get_rec(node->pcur);

	offsets = rec_get_offsets(rec, clust_index, offsets_,
				  clust_index->n_core_fields,
				  ULINT_UNDEFINED, &heap);

	if (dict_table_has_atomic_blobs(node->table)) {
		/* There is no prefix of externally stored columns in
		the clustered index record. Build a cache of column
		prefixes. */
		ext = &node->ext;
	} else {
		/* REDUNDANT and COMPACT formats store a local
		768-byte prefix of each externally stored column.
		No cache is needed. */
		ext = NULL;
		node->ext = NULL;
	}

	node->row = row_build(ROW_COPY_DATA, clust_index, rec, offsets,
			      NULL, NULL, NULL, ext, node->heap);

	if (node->table->n_v_cols) {
		bool ok = row_upd_store_v_row(node,
				    node->is_delete ? NULL : node->update,
				    thd, mysql_table);
		if (!ok) {
			return false;
		}
	}

	if (node->is_delete == PLAIN_DELETE) {
		node->upd_row = NULL;
		node->upd_ext = NULL;
	} else {
		node->upd_row = dtuple_copy(node->row, node->heap);
		row_upd_replace(node->upd_row, &node->upd_ext,
				clust_index, node->update, node->heap);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return true;
}

/***********************************************************//**
Updates a secondary index entry of a row.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_sec_index_entry(
/*====================*/
	upd_node_t*	node,	/*!< in: row update node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	mtr_t			mtr;
	const rec_t*		rec;
	btr_pcur_t		pcur;
	mem_heap_t*		heap;
	dtuple_t*		entry;
	dict_index_t*		index;
	btr_cur_t*		btr_cur;
	dberr_t			err	= DB_SUCCESS;
	trx_t*			trx	= thr_get_trx(thr);
	ulint			mode;
	ulint			flags;
	enum row_search_result	search_result;

	ut_ad(trx->id != 0);

	index = node->index;
	if (!index->is_committed()) {
		return DB_SUCCESS;
	}

	/* For secondary indexes, index->online_status==ONLINE_INDEX_COMPLETE
	if index->is_committed(). */
	ut_ad(!dict_index_is_online_ddl(index));

	const bool referenced = row_upd_index_is_referenced(index, trx);
#ifdef WITH_WSREP
	const bool foreign = wsrep_row_upd_index_is_foreign(index, trx);
#endif /* WITH_WSREP */

	heap = mem_heap_create(1024);

	/* Build old index entry */
	entry = row_build_index_entry(node->row, node->ext, index, heap);
	ut_a(entry);

	log_free_check();

	DEBUG_SYNC_C_IF_THD(trx->mysql_thd,
			    "before_row_upd_sec_index_entry");

	mtr.start();

	switch (index->table->space_id) {
	case SRV_TMP_SPACE_ID:
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		flags = BTR_NO_LOCKING_FLAG;
		mode = index->is_spatial()
			? ulint(BTR_MODIFY_LEAF | BTR_RTREE_DELETE_MARK)
			: ulint(BTR_MODIFY_LEAF);
		break;
	default:
		index->set_modified(mtr);
		/* fall through */
	case IBUF_SPACE_ID:
		flags = index->table->no_rollback() ? BTR_NO_ROLLBACK : 0;
		/* We can only buffer delete-mark operations if there
		are no foreign key constraints referring to the index. */
		mode = index->is_spatial()
			? ulint(BTR_MODIFY_LEAF | BTR_RTREE_DELETE_MARK)
			: referenced
			? ulint(BTR_MODIFY_LEAF) : ulint(BTR_DELETE_MARK_LEAF);
		break;
	}

	/* Set the query thread, so that ibuf_insert_low() will be
	able to invoke thd_get_trx(). */
	btr_pcur_get_btr_cur(&pcur)->thr = thr;

	search_result = row_search_index_entry(index, entry, mode,
					       &pcur, &mtr);

	btr_cur = btr_pcur_get_btr_cur(&pcur);

	rec = btr_cur_get_rec(btr_cur);

	switch (search_result) {
	case ROW_NOT_DELETED_REF:	/* should only occur for BTR_DELETE */
		ut_error;
		break;
	case ROW_BUFFERED:
		/* Entry was delete marked already. */
		break;

	case ROW_NOT_FOUND:
		if (dict_index_is_spatial(index) && btr_cur->rtr_info->fd_del) {
			/* We found the record, but a delete marked */
			break;
		}

		ib::error()
			<< "Record in index " << index->name
			<< " of table " << index->table->name
			<< " was not found on update: " << *entry
			<< " at: " << rec_index_print(rec, index);
#ifdef UNIV_DEBUG
		mtr_commit(&mtr);
		mtr_start(&mtr);
		ut_ad(btr_validate_index(index, 0) == DB_SUCCESS);
		ut_ad(0);
#endif /* UNIV_DEBUG */
		break;
	case ROW_FOUND:
		ut_ad(err == DB_SUCCESS);

		/* Delete mark the old index record; it can already be
		delete marked if we return after a lock wait in
		row_ins_sec_index_entry() below */
		if (!rec_get_deleted_flag(
			    rec, dict_table_is_comp(index->table))) {
			err = lock_sec_rec_modify_check_and_lock(
				flags,
				btr_cur_get_block(btr_cur),
				btr_cur_get_rec(btr_cur), index, thr, &mtr);
			if (err != DB_SUCCESS) {
				break;
			}

			btr_rec_set_deleted<true>(btr_cur_get_block(btr_cur),
						  btr_cur_get_rec(btr_cur),
						  &mtr);
#ifdef WITH_WSREP
			if (!referenced && foreign
			    && wsrep_must_process_fk(node, trx)
			    && !wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {

				rec_offs* offsets = rec_get_offsets(
					rec, index, NULL, index->n_core_fields,
					ULINT_UNDEFINED, &heap);

				err = wsrep_row_upd_check_foreign_constraints(
					node, &pcur, index->table,
					index, offsets, thr, &mtr);

				switch (err) {
				case DB_SUCCESS:
				case DB_NO_REFERENCED_ROW:
					err = DB_SUCCESS;
					break;
				case DB_LOCK_WAIT:
				case DB_DEADLOCK:
				case DB_LOCK_WAIT_TIMEOUT:
					WSREP_DEBUG("Foreign key check fail: "
						"%s on table %s index %s query %s",
						ut_strerr(err), index->name(), index->table->name.m_name,
						wsrep_thd_query(trx->mysql_thd));
					break;
				default:
					WSREP_ERROR("Foreign key check fail: "
						"%s on table %s index %s query %s",
						ut_strerr(err), index->name(), index->table->name.m_name,
						wsrep_thd_query(trx->mysql_thd));
					break;
				}
			}
#endif /* WITH_WSREP */
		}

#ifdef WITH_WSREP
		ut_ad(err == DB_SUCCESS || err == DB_LOCK_WAIT
		      || err == DB_DEADLOCK || err == DB_LOCK_WAIT_TIMEOUT);
#else
		ut_ad(err == DB_SUCCESS);
#endif

		if (referenced) {
			rec_offs* offsets = rec_get_offsets(
				rec, index, NULL, index->n_core_fields,
				ULINT_UNDEFINED, &heap);

			/* NOTE that the following call loses
			the position of pcur ! */
			err = row_upd_check_references_constraints(
				node, &pcur, index->table,
				index, offsets, thr, &mtr);
		}
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	if (node->is_delete == PLAIN_DELETE || err != DB_SUCCESS) {

		goto func_exit;
	}

	mem_heap_empty(heap);

	DEBUG_SYNC_C_IF_THD(trx->mysql_thd,
			    "before_row_upd_sec_new_index_entry");

	/* Build a new index entry */
	entry = row_build_index_entry(node->upd_row, node->upd_ext,
				      index, heap);
	ut_a(entry);

	/* Insert new index entry */
	err = row_ins_sec_index_entry(index, entry, thr, !node->is_delete);

func_exit:
	mem_heap_free(heap);

	return(err);
}

/***********************************************************//**
Updates the secondary index record if it is changed in the row update or
deletes it if this is a delete.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_sec_step(
/*=============*/
	upd_node_t*	node,	/*!< in: row update node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	ut_ad((node->state == UPD_NODE_UPDATE_ALL_SEC)
	      || (node->state == UPD_NODE_UPDATE_SOME_SEC));
	ut_ad(!dict_index_is_clust(node->index));

	if (node->state == UPD_NODE_UPDATE_ALL_SEC
	    || row_upd_changes_ord_field_binary(node->index, node->update,
						thr, node->row, node->ext)) {
		return(row_upd_sec_index_entry(node, thr));
	}

	return(DB_SUCCESS);
}

#ifdef UNIV_DEBUG
# define row_upd_clust_rec_by_insert_inherit(rec,index,offsets,entry,update) \
	row_upd_clust_rec_by_insert_inherit_func(rec,index,offsets,entry,update)
#else /* UNIV_DEBUG */
# define row_upd_clust_rec_by_insert_inherit(rec,index,offsets,entry,update) \
	row_upd_clust_rec_by_insert_inherit_func(rec,entry,update)
#endif /* UNIV_DEBUG */
/*******************************************************************//**
Mark non-updated off-page columns inherited when the primary key is
updated. We must mark them as inherited in entry, so that they are not
freed in a rollback. A limited version of this function used to be
called btr_cur_mark_dtuple_inherited_extern().
@return whether any columns were inherited */
static
bool
row_upd_clust_rec_by_insert_inherit_func(
/*=====================================*/
	const rec_t*	rec,	/*!< in: old record, or NULL */
#ifdef UNIV_DEBUG
	dict_index_t*	index,	/*!< in: index, or NULL */
	const rec_offs*	offsets,/*!< in: rec_get_offsets(rec), or NULL */
#endif /* UNIV_DEBUG */
	dtuple_t*	entry,	/*!< in/out: updated entry to be
				inserted into the clustered index */
	const upd_t*	update)	/*!< in: update vector */
{
	bool	inherit	= false;

	ut_ad(!rec == !offsets);
	ut_ad(!rec == !index);
	ut_ad(!rec || rec_offs_validate(rec, index, offsets));
	ut_ad(!rec || rec_offs_any_extern(offsets));

	for (uint16_t i = 0; i < dtuple_get_n_fields(entry); i++) {
		dfield_t*	dfield	= dtuple_get_nth_field(entry, i);
		byte*		data;
		ulint		len;

		ut_ad(!offsets
		      || !rec_offs_nth_extern(offsets, i)
		      == !dfield_is_ext(dfield)
		      || (!dict_index_get_nth_field(index, i)->name
			  && !dfield_is_ext(dfield)
			  && (dfield_is_null(dfield) || dfield->len == 0))
		      || upd_get_field_by_field_no(update, i, false));
		if (!dfield_is_ext(dfield)
		    || upd_get_field_by_field_no(update, i, false)) {
			continue;
		}

#ifdef UNIV_DEBUG
		if (UNIV_LIKELY(rec != NULL)) {
			ut_ad(!rec_offs_nth_default(offsets, i));
			const byte* rec_data
				= rec_get_nth_field(rec, offsets, i, &len);
			ut_ad(len == dfield_get_len(dfield));
			ut_ad(len != UNIV_SQL_NULL);
			ut_ad(len >= BTR_EXTERN_FIELD_REF_SIZE);

			rec_data += len - BTR_EXTERN_FIELD_REF_SIZE;

			/* The pointer must not be zero. */
			ut_ad(memcmp(rec_data, field_ref_zero,
				     BTR_EXTERN_FIELD_REF_SIZE));
			/* The BLOB must be owned. */
			ut_ad(!(rec_data[BTR_EXTERN_LEN]
				& BTR_EXTERN_OWNER_FLAG));
		}
#endif /* UNIV_DEBUG */

		len = dfield_get_len(dfield);
		ut_a(len != UNIV_SQL_NULL);
		ut_a(len >= BTR_EXTERN_FIELD_REF_SIZE);

		data = static_cast<byte*>(dfield_get_data(dfield));

		data += len - BTR_EXTERN_FIELD_REF_SIZE;
		/* The pointer must not be zero. */
		ut_a(memcmp(data, field_ref_zero, BTR_EXTERN_FIELD_REF_SIZE));

		/* The BLOB must be owned, unless we are resuming from
		a lock wait and we already had disowned the BLOB. */
		ut_a(rec == NULL
		     || !(data[BTR_EXTERN_LEN] & BTR_EXTERN_OWNER_FLAG));
		data[BTR_EXTERN_LEN] &= byte(~BTR_EXTERN_OWNER_FLAG);
		data[BTR_EXTERN_LEN] |= BTR_EXTERN_INHERITED_FLAG;
		/* The BTR_EXTERN_INHERITED_FLAG only matters in
		rollback of a fresh insert. Purge will always free
		the extern fields of a delete-marked row. */

		inherit = true;
	}

	return(inherit);
}

/***********************************************************//**
Marks the clustered index record deleted and inserts the updated version
of the record to the index. This function should be used when the ordering
fields of the clustered index record change. This should be quite rare in
database applications.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_clust_rec_by_insert(
/*========================*/
	upd_node_t*	node,	/*!< in/out: row update node */
	dict_index_t*	index,	/*!< in: clustered index of the record */
	que_thr_t*	thr,	/*!< in: query thread */
	bool		referenced,/*!< in: whether index may be referenced in
				a foreign key constraint */
#ifdef WITH_WSREP
	bool		foreign,/*!< in: whether this is a foreign key */
#endif
	mtr_t*		mtr)	/*!< in/out: mini-transaction,
				may be committed and restarted */
{
	mem_heap_t*	heap;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	trx_t*		trx;
	dict_table_t*	table;
	dtuple_t*	entry;
	dberr_t		err;
	rec_t*		rec;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets			= offsets_;

	ut_ad(dict_index_is_clust(index));

	rec_offs_init(offsets_);

	trx = thr_get_trx(thr);
	table = node->table;
	pcur = node->pcur;
	btr_cur	= btr_pcur_get_btr_cur(pcur);

	heap = mem_heap_create(1000);

	entry = row_build_index_entry_low(node->upd_row, node->upd_ext,
					  index, heap, ROW_BUILD_FOR_INSERT);
	if (index->is_instant()) entry->trim(*index);
	ut_ad(dtuple_get_info_bits(entry) == 0);

	{
		dfield_t* t = dtuple_get_nth_field(entry, index->db_trx_id());
		ut_ad(t->len == DATA_TRX_ID_LEN);
		trx_write_trx_id(static_cast<byte*>(t->data), trx->id);
	}

	switch (node->state) {
	default:
		ut_error;
	case UPD_NODE_INSERT_CLUSTERED:
		/* A lock wait occurred in row_ins_clust_index_entry() in
		the previous invocation of this function. */
		row_upd_clust_rec_by_insert_inherit(
			NULL, NULL, NULL, entry, node->update);
		break;
	case UPD_NODE_UPDATE_CLUSTERED:
		/* This is the first invocation of the function where
		we update the primary key.  Delete-mark the old record
		in the clustered index and prepare to insert a new entry. */
		rec = btr_cur_get_rec(btr_cur);
		offsets = rec_get_offsets(rec, index, offsets,
					  index->n_core_fields,
					  ULINT_UNDEFINED, &heap);
		ut_ad(page_rec_is_user_rec(rec));

		if (rec_get_deleted_flag(rec, rec_offs_comp(offsets))) {
			/* If the clustered index record is already delete
			marked, then we are here after a DB_LOCK_WAIT.
			Skip delete marking clustered index and disowning
			its blobs. */
			ut_ad(row_get_rec_trx_id(rec, index, offsets)
			      == trx->id);
			ut_ad(!trx_undo_roll_ptr_is_insert(
			              row_get_rec_roll_ptr(rec, index,
							   offsets)));
			goto check_fk;
		}

		err = btr_cur_del_mark_set_clust_rec(
			btr_cur_get_block(btr_cur), rec, index, offsets,
			thr, node->row, mtr);
		if (err != DB_SUCCESS) {
			goto err_exit;
		}

		/* If the the new row inherits externally stored
		fields (off-page columns a.k.a. BLOBs) from the
		delete-marked old record, mark them disowned by the
		old record and owned by the new entry. */

		if (rec_offs_any_extern(offsets)) {
			if (row_upd_clust_rec_by_insert_inherit(
				    rec, index, offsets,
				    entry, node->update)) {
				/* The blobs are disowned here, expecting the
				insert down below to inherit them.  But if the
				insert fails, then this disown will be undone
				when the operation is rolled back. */
				btr_cur_disown_inherited_fields(
					btr_cur_get_block(btr_cur),
					rec, index, offsets, node->update,
					mtr);
			}
		}
check_fk:
		if (referenced) {
			/* NOTE that the following call loses
			the position of pcur ! */

			err = row_upd_check_references_constraints(
				node, pcur, table, index, offsets, thr, mtr);

			if (err != DB_SUCCESS) {
				goto err_exit;
			}
#ifdef WITH_WSREP
		} else if (foreign && wsrep_must_process_fk(node, trx)) {
			err = wsrep_row_upd_check_foreign_constraints(
				node, pcur, table, index, offsets, thr, mtr);

			switch (err) {
			case DB_SUCCESS:
			case DB_NO_REFERENCED_ROW:
				err = DB_SUCCESS;
				break;
			case DB_LOCK_WAIT:
			case DB_DEADLOCK:
			case DB_LOCK_WAIT_TIMEOUT:
				WSREP_DEBUG("Foreign key check fail: "
					    "%s on table %s index %s query %s",
					    ut_strerr(err), index->name(), index->table->name.m_name,
					    wsrep_thd_query(trx->mysql_thd));

				goto err_exit;
			default:
				WSREP_ERROR("Foreign key check fail: "
					    "%s on table %s index %s query %s",
					    ut_strerr(err), index->name(), index->table->name.m_name,
					    wsrep_thd_query(trx->mysql_thd));

				goto err_exit;
			}
#endif /* WITH_WSREP */
		}
	}

	mtr->commit();
	mtr->start();

	node->state = UPD_NODE_INSERT_CLUSTERED;
	err = row_ins_clust_index_entry(index, entry, thr,
					dtuple_get_n_ext(entry));
err_exit:
	mem_heap_free(heap);
	return(err);
}

/***********************************************************//**
Updates a clustered index record of a row when the ordering fields do
not change.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_clust_rec(
/*==============*/
	ulint		flags,  /*!< in: undo logging and locking flags */
	upd_node_t*	node,	/*!< in: row update node */
	dict_index_t*	index,	/*!< in: clustered index */
	rec_offs*	offsets,/*!< in: rec_get_offsets() on node->pcur */
	mem_heap_t**	offsets_heap,
				/*!< in/out: memory heap, can be emptied */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr)	/*!< in,out: mini-transaction; may be
				committed and restarted here */
{
	mem_heap_t*	heap		= NULL;
	big_rec_t*	big_rec		= NULL;
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	dberr_t		err;

	ut_ad(dict_index_is_clust(index));
	ut_ad(!thr_get_trx(thr)->in_rollback);
	ut_ad(!node->table->skip_alter_undo);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	ut_ad(btr_cur_get_index(btr_cur) == index);
	ut_ad(!rec_get_deleted_flag(btr_cur_get_rec(btr_cur),
				    dict_table_is_comp(index->table)));
	ut_ad(rec_offs_validate(btr_cur_get_rec(btr_cur), index, offsets));

	/* Try optimistic updating of the record, keeping changes within
	the page; we do not check locks because we assume the x-lock on the
	record to update */

	if (node->cmpl_info & UPD_NODE_NO_SIZE_CHANGE) {
		err = btr_cur_update_in_place(
			flags | BTR_NO_LOCKING_FLAG, btr_cur,
			offsets, node->update,
			node->cmpl_info, thr, thr_get_trx(thr)->id, mtr);
	} else {
		err = btr_cur_optimistic_update(
			flags | BTR_NO_LOCKING_FLAG, btr_cur,
			&offsets, offsets_heap, node->update,
			node->cmpl_info, thr, thr_get_trx(thr)->id, mtr);
	}

	if (err == DB_SUCCESS) {
		goto func_exit;
	}

	if (buf_pool.running_out()) {
		err = DB_LOCK_TABLE_FULL;
		goto func_exit;
	}

	/* We may have to modify the tree structure: do a pessimistic descent
	down the index tree */

	mtr->commit();
	mtr->start();

	if (index->table->is_temporary()) {
		/* Disable locking, because temporary tables are never
		shared between transactions or connections. */
		flags |= BTR_NO_LOCKING_FLAG;
		mtr->set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(*mtr);
	}

	/* NOTE: this transaction has an s-lock or x-lock on the record and
	therefore other transactions cannot modify the record when we have no
	latch on the page. In addition, we assume that other query threads of
	the same transaction do not modify the record in the meantime.
	Therefore we can assert that the restoration of the cursor succeeds. */

	ut_a(pcur->restore_position(BTR_MODIFY_TREE, mtr) ==
	    btr_pcur_t::SAME_ALL);

	ut_ad(!rec_get_deleted_flag(btr_pcur_get_rec(pcur),
				    dict_table_is_comp(index->table)));

	if (!heap) {
		heap = mem_heap_create(1024);
	}

	err = btr_cur_pessimistic_update(
		flags | BTR_NO_LOCKING_FLAG | BTR_KEEP_POS_FLAG, btr_cur,
		&offsets, offsets_heap, heap, &big_rec,
		node->update, node->cmpl_info,
		thr, thr_get_trx(thr)->id, mtr);
	if (big_rec) {
		ut_a(err == DB_SUCCESS);

		DEBUG_SYNC_C("before_row_upd_extern");
		err = btr_store_big_rec_extern_fields(
			pcur, offsets, big_rec, mtr, BTR_STORE_UPDATE);
		DEBUG_SYNC_C("after_row_upd_extern");
	}

func_exit:
	if (heap) {
		mem_heap_free(heap);
	}

	if (big_rec) {
		dtuple_big_rec_free(big_rec);
	}

	return(err);
}

/***********************************************************//**
Delete marks a clustered index record.
@return DB_SUCCESS if operation successfully completed, else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_del_mark_clust_rec(
/*=======================*/
	upd_node_t*	node,	/*!< in: row update node */
	dict_index_t*	index,	/*!< in: clustered index */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets() for the
				record under the cursor */
	que_thr_t*	thr,	/*!< in: query thread */
	bool		referenced,
				/*!< in: whether index may be referenced in
				a foreign key constraint */
#ifdef WITH_WSREP
	bool		foreign,/*!< in: whether this is a foreign key */
#endif
	mtr_t*		mtr)	/*!< in,out: mini-transaction;
				will be committed and restarted */
{
	btr_pcur_t*	pcur;
	btr_cur_t*	btr_cur;
	rec_t*		rec;
	trx_t*		trx = thr_get_trx(thr);

	ut_ad(dict_index_is_clust(index));
	ut_ad(node->is_delete == PLAIN_DELETE);

	pcur = node->pcur;
	btr_cur = btr_pcur_get_btr_cur(pcur);

	/* Store row because we have to build also the secondary index
	entries */

	if (!row_upd_store_row(node, trx->mysql_thd,
			  thr->prebuilt  && thr->prebuilt->table == node->table
			  ? thr->prebuilt->m_mysql_table : NULL)) {
		return DB_COMPUTE_VALUE_FAILED;
	}

	/* Mark the clustered index record deleted; we do not have to check
	locks, because we assume that we have an x-lock on the record */

	rec = btr_cur_get_rec(btr_cur);

	dberr_t err = btr_cur_del_mark_set_clust_rec(
		btr_cur_get_block(btr_cur), rec,
		index, offsets, thr, node->row, mtr);

	if (err != DB_SUCCESS) {
	} else if (referenced) {
		/* NOTE that the following call loses the position of pcur ! */

		err = row_upd_check_references_constraints(
			node, pcur, index->table, index, offsets, thr, mtr);
#ifdef WITH_WSREP
	} else if (foreign && wsrep_must_process_fk(node, trx)) {
		err = wsrep_row_upd_check_foreign_constraints(
			node, pcur, index->table, index, offsets, thr, mtr);

		switch (err) {
		case DB_SUCCESS:
		case DB_NO_REFERENCED_ROW:
			err = DB_SUCCESS;
			break;
		case DB_LOCK_WAIT:
		case DB_DEADLOCK:
		case DB_LOCK_WAIT_TIMEOUT:
			WSREP_DEBUG("Foreign key check fail: "
				    "%d on table %s index %s query %s",
				    err, index->name(), index->table->name.m_name,
				    wsrep_thd_query(trx->mysql_thd));
			break;
		default:
			WSREP_ERROR("Foreign key check fail: "
				    "%d on table %s index %s query %s",
				    err, index->name(), index->table->name.m_name,
				    wsrep_thd_query(trx->mysql_thd));
			break;
		}
#endif /* WITH_WSREP */
	}

	return(err);
}

/***********************************************************//**
Updates the clustered index record.
@return DB_SUCCESS if operation successfully completed, DB_LOCK_WAIT
in case of a lock wait, else error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_upd_clust_step(
/*===============*/
	upd_node_t*	node,	/*!< in: row update node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dict_index_t*	index;
	btr_pcur_t*	pcur;
	dberr_t		err;
	mtr_t		mtr;
	rec_t*		rec;
	mem_heap_t*	heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets;
	ulint		flags;
	trx_t*		trx = thr_get_trx(thr);

	rec_offs_init(offsets_);

	index = dict_table_get_first_index(node->table);

	const bool referenced = row_upd_index_is_referenced(index, trx);
#ifdef WITH_WSREP
	const bool foreign = wsrep_row_upd_index_is_foreign(index, trx);
#endif

	pcur = node->pcur;

	/* We have to restore the cursor to its position */

	mtr.start();

	if (node->table->is_temporary()) {
		/* Disable locking, because temporary tables are
		private to the connection (no concurrent access). */
		flags = node->table->no_rollback()
			? BTR_NO_ROLLBACK
			: BTR_NO_LOCKING_FLAG;
		/* Redo logging only matters for persistent tables. */
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		flags = node->table->no_rollback() ? BTR_NO_ROLLBACK : 0;
		index->set_modified(mtr);
	}

	/* If the restoration does not succeed, then the same
	transaction has deleted the record on which the cursor was,
	and that is an SQL error. If the restoration succeeds, it may
	still be that the same transaction has successively deleted
	and inserted a record with the same ordering fields, but in
	that case we know that the transaction has at least an
	implicit x-lock on the record. */

	ut_a(pcur->rel_pos == BTR_PCUR_ON);

	ulint	mode;

	DEBUG_SYNC_C_IF_THD(trx->mysql_thd, "innodb_row_upd_clust_step_enter");

	if (dict_index_is_online_ddl(index)) {
		ut_ad(node->table->id != DICT_INDEXES_ID);
		mode = BTR_MODIFY_LEAF | BTR_ALREADY_S_LATCHED;
		mtr_s_lock_index(index, &mtr);
	} else {
		mode = BTR_MODIFY_LEAF;
	}

	if (pcur->restore_position(mode, &mtr) != btr_pcur_t::SAME_ALL) {
		err = DB_RECORD_NOT_FOUND;
		goto exit_func;
	}

	rec = btr_pcur_get_rec(pcur);
	offsets = rec_get_offsets(rec, index, offsets_, index->n_core_fields,
				  ULINT_UNDEFINED, &heap);

	if (!flags && !node->has_clust_rec_x_lock) {
		err = lock_clust_rec_modify_check_and_lock(
			btr_pcur_get_block(pcur),
			rec, index, offsets, thr);
		if (err != DB_SUCCESS) {
			goto exit_func;
		}
	}

	ut_ad(index->table->no_rollback() || index->table->is_temporary()
	      || row_get_rec_trx_id(rec, index, offsets) == trx->id
	      || lock_trx_has_expl_x_lock(*trx, *index->table,
					  btr_pcur_get_block(pcur)->page.id(),
					  page_rec_get_heap_no(rec)));

	if (node->is_delete == PLAIN_DELETE) {
		err = row_upd_del_mark_clust_rec(
			node, index, offsets, thr, referenced,
#ifdef WITH_WSREP
			foreign,
#endif
			&mtr);
		goto all_done;
	}

	/* If the update is made for MySQL, we already have the update vector
	ready, else we have to do some evaluation: */

	if (UNIV_UNLIKELY(!node->in_mysql_interface)) {
		/* Copy the necessary columns from clust_rec and calculate the
		new values to set */
		row_upd_copy_columns(rec, offsets, index,
				     UT_LIST_GET_FIRST(node->columns));
		row_upd_eval_new_vals(node->update);
	}

	if (!node->is_delete && node->cmpl_info & UPD_NODE_NO_ORD_CHANGE) {
		err = row_upd_clust_rec(
			flags, node, index, offsets, &heap, thr, &mtr);
		goto exit_func;
	}

	if (!row_upd_store_row(node, trx->mysql_thd, thr->prebuilt
			       ? thr->prebuilt->m_mysql_table : NULL)) {
		err = DB_COMPUTE_VALUE_FAILED;
		goto exit_func;
	}

	if (row_upd_changes_ord_field_binary(index, node->update, thr,
					     node->row, node->ext)) {

		/* Update causes an ordering field (ordering fields within
		the B-tree) of the clustered index record to change: perform
		the update by delete marking and inserting.

		TODO! What to do to the 'Halloween problem', where an update
		moves the record forward in index so that it is again
		updated when the cursor arrives there? Solution: the
		read operation must check the undo record undo number when
		choosing records to update. MySQL solves now the problem
		externally! */

		err = row_upd_clust_rec_by_insert(
			node, index, thr, referenced,
#ifdef WITH_WSREP
			foreign,
#endif
			&mtr);
all_done:
		if (err == DB_SUCCESS) {
			node->state = UPD_NODE_UPDATE_ALL_SEC;
success:
			node->index = dict_table_get_next_index(index);
		}
	} else {
		err = row_upd_clust_rec(
			flags, node, index, offsets, &heap, thr, &mtr);

		if (err == DB_SUCCESS) {
			ut_ad(node->is_delete != PLAIN_DELETE);
			node->state = node->is_delete
				? UPD_NODE_UPDATE_ALL_SEC
				: UPD_NODE_UPDATE_SOME_SEC;
			goto success;
		}
	}

exit_func:
	mtr.commit();
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return err;
}

/***********************************************************//**
Updates the affected index records of a row. When the control is transferred
to this node, we assume that we have a persistent cursor which was on a
record, and the position of the cursor is stored in the cursor.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static
dberr_t
row_upd(
/*====*/
	upd_node_t*	node,	/*!< in: row update node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t		err	= DB_SUCCESS;
	DBUG_ENTER("row_upd");

	ut_ad(!thr_get_trx(thr)->in_rollback);

	DBUG_PRINT("row_upd", ("table: %s", node->table->name.m_name));
	DBUG_PRINT("row_upd", ("info bits in update vector: 0x%x",
			       node->update ? node->update->info_bits: 0));
	DBUG_PRINT("row_upd", ("foreign_id: %s",
			       node->foreign ? node->foreign->id: "NULL"));

	if (UNIV_LIKELY(node->in_mysql_interface)) {

		/* We do not get the cmpl_info value from the MySQL
		interpreter: we must calculate it on the fly: */

		if (node->is_delete == PLAIN_DELETE
		    || row_upd_changes_some_index_ord_field_binary(
			    node->table, node->update)) {
			node->cmpl_info = 0;
		} else {
			node->cmpl_info = UPD_NODE_NO_ORD_CHANGE;
		}
	}

	switch (node->state) {
	case UPD_NODE_UPDATE_CLUSTERED:
	case UPD_NODE_INSERT_CLUSTERED:
		log_free_check();

		err = row_upd_clust_step(node, thr);

		if (err != DB_SUCCESS) {

			DBUG_RETURN(err);
		}
	}

	DEBUG_SYNC_C_IF_THD(thr_get_trx(thr)->mysql_thd,
			    "after_row_upd_clust");

	if (node->index == NULL
	    || (!node->is_delete
		&& (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE))) {

		DBUG_RETURN(DB_SUCCESS);
	}

	DBUG_EXECUTE_IF("row_upd_skip_sec", node->index = NULL;);

	do {
		/* Skip corrupted index */
		dict_table_skip_corrupt_index(node->index);

		if (!node->index) {
			break;
		}

		if (!(node->index->type & DICT_FTS)
		    && node->index->is_committed()) {
			err = row_upd_sec_step(node, thr);

			if (err != DB_SUCCESS) {

				DBUG_RETURN(err);
			}
		}

		node->index = dict_table_get_next_index(node->index);
	} while (node->index != NULL);

	ut_ad(err == DB_SUCCESS);

	/* Do some cleanup */

	if (node->row != NULL) {
		node->row = NULL;
		node->ext = NULL;
		node->upd_row = NULL;
		node->upd_ext = NULL;
		mem_heap_empty(node->heap);
	}

	node->state = UPD_NODE_UPDATE_CLUSTERED;

	DBUG_RETURN(err);
}

/***********************************************************//**
Updates a row in a table. This is a high-level function used in SQL execution
graphs.
@return query thread to run next or NULL */
que_thr_t*
row_upd_step(
/*=========*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	upd_node_t*	node;
	sel_node_t*	sel_node;
	que_node_t*	parent;
	dberr_t		err		= DB_SUCCESS;
	trx_t*		trx;
	DBUG_ENTER("row_upd_step");

	ut_ad(thr);

	trx = thr_get_trx(thr);

	node = static_cast<upd_node_t*>(thr->run_node);

	sel_node = node->select;

	parent = que_node_get_parent(node);

	ut_ad(que_node_get_type(node) == QUE_NODE_UPDATE);

	if (thr->prev_node == parent) {
		node->state = UPD_NODE_SET_IX_LOCK;
	}

	if (node->state == UPD_NODE_SET_IX_LOCK) {

		if (!node->has_clust_rec_x_lock) {
			/* It may be that the current session has not yet
			started its transaction, or it has been committed: */

			err = lock_table(node->table, nullptr, LOCK_IX, thr);

			if (err != DB_SUCCESS) {

				goto error_handling;
			}
		}

		node->state = UPD_NODE_UPDATE_CLUSTERED;

		if (node->searched_update) {
			/* Reset the cursor */
			sel_node->state = SEL_NODE_OPEN;

			/* Fetch a row to update */

			thr->run_node = sel_node;

			DBUG_RETURN(thr);
		}
	}

	/* sel_node is NULL if we are in the MySQL interface */

	if (sel_node && (sel_node->state != SEL_NODE_FETCH)) {

		if (!node->searched_update) {
			/* An explicit cursor should be positioned on a row
			to update */

			ut_error;

			err = DB_ERROR;

			goto error_handling;
		}

		ut_ad(sel_node->state == SEL_NODE_NO_MORE_ROWS);

		/* No more rows to update, or the select node performed the
		updates directly in-place */

		thr->run_node = parent;

		DBUG_RETURN(thr);
	}

	/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */

	err = row_upd(node, thr);

error_handling:
	trx->error_state = err;

	if (err != DB_SUCCESS) {
		DBUG_RETURN(NULL);
	}

	/* DO THE TRIGGER ACTIONS HERE */

	if (node->searched_update) {
		/* Fetch next row to update */

		thr->run_node = sel_node;
	} else {
		/* It was an explicit cursor update */

		thr->run_node = parent;
	}

	node->state = UPD_NODE_UPDATE_CLUSTERED;

	DBUG_RETURN(thr);
}

/** Write query start time as SQL field data to a buffer. Needed by InnoDB.
@param	thd	Thread object
@param	buf	Buffer to hold start time data */
void thd_get_query_start_data(THD *thd, char *buf);

/** Appends row_start or row_end field to update vector and sets a
CURRENT_TIMESTAMP/trx->id value to it.
Supposed to be called only by make_versioned_update() and
make_versioned_delete().
@param[in]	trx	transaction
@param[in]	vers_sys_idx	table->row_start or table->row_end */
void upd_node_t::vers_update_fields(const trx_t *trx, ulint idx)
{
  ut_ad(in_mysql_interface); // otherwise needs to recalculate node->cmpl_info
  ut_ad(idx == table->vers_start || idx == table->vers_end);

  dict_index_t *clust_index= dict_table_get_first_index(table);
  const dict_col_t *col= dict_table_get_nth_col(table, idx);
  ulint field_no= dict_col_get_clust_pos(col, clust_index);
  upd_field_t *ufield;

  for (ulint i= 0; i < update->n_fields; ++i)
  {
    if (update->fields[i].field_no == field_no)
    {
      ufield= &update->fields[i];
      goto skip_append;
    }
  }

  /* row_create_update_node_for_mysql() pre-allocated this much.
  At least one PK column always remains unchanged. */
  ut_ad(update->n_fields < ulint(table->n_cols + table->n_v_cols));

  update->n_fields++;
  ufield= upd_get_nth_field(update, update->n_fields - 1);
  upd_field_set_field_no(ufield, static_cast<uint16_t>(field_no), clust_index);

skip_append:
  char *where= reinterpret_cast<char *>(update->vers_sys_value);
  if (col->vers_native())
    mach_write_to_8(where, trx->id);
  else
    thd_get_query_start_data(trx->mysql_thd, where);

  dfield_set_data(&ufield->new_val, update->vers_sys_value, col->len);

  for (ulint col_no= 0; col_no < dict_table_get_n_v_cols(table); col_no++)
  {
    const dict_v_col_t *v_col= dict_table_get_nth_v_col(table, col_no);
    if (!v_col->m_col.ord_part)
      continue;
    for (ulint i= 0; i < unsigned(v_col->num_base); i++)
    {
      dict_col_t *base_col= v_col->base_col[i];
      if (base_col->ind == col->ind)
      {
        /* Virtual column depends on system field value
        which we updated above. Remove it from update
        vector, so it is recalculated in
        row_upd_store_v_row() (see !update branch). */
        update->remove(v_col->v_pos);
        break;
      }
    }
  }
}
