/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file row/row0ins.cc
Insert into a table

Created 4/20/1996 Heikki Tuuri
*******************************************************/

#include "row0ins.h"
#include "dict0dict.h"
#include "trx0rec.h"
#include "trx0undo.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "mach0data.h"
#include "ibuf0ibuf.h"
#include "que0que.h"
#include "row0upd.h"
#include "row0sel.h"
#include "row0log.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "log0log.h"
#include "eval0eval.h"
#include "data0data.h"
#include "buf0lru.h"
#include "fts0fts.h"
#include "fts0types.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "btr0sea.h"
#endif
#ifdef WITH_WSREP
#include "wsrep_mysqld.h"
#endif /* WITH_WSREP */

/*************************************************************************
IMPORTANT NOTE: Any operation that generates redo MUST check that there
is enough space in the redo log before for that operation. This is
done by calling log_free_check(). The reason for checking the
availability of the redo log space before the start of the operation is
that we MUST not hold any synchonization objects when performing the
check.
If you make a change in this module make sure that no codepath is
introduced where a call to log_free_check() is bypassed. */

/** Create an row template for each index of a table. */
static void ins_node_create_entry_list(ins_node_t *node)
{
  node->entry_list.reserve(UT_LIST_GET_LEN(node->table->indexes));

  for (dict_index_t *index= dict_table_get_first_index(node->table); index;
       index= dict_table_get_next_index(index))
  {
    /* Corrupted or incomplete secondary indexes will be filtered out in
    row_ins(). */
    dtuple_t *entry= index->online_status >= ONLINE_INDEX_ABORTED
      ? dtuple_create(node->entry_sys_heap, 0)
      : row_build_index_entry_low(node->row, NULL, index, node->entry_sys_heap,
				  ROW_BUILD_FOR_INSERT);
    node->entry_list.push_back(entry);
  }
}

/*****************************************************************//**
Adds system field buffers to a row. */
static
void
row_ins_alloc_sys_fields(
/*=====================*/
	ins_node_t*	node)	/*!< in: insert node */
{
	dtuple_t*		row;
	dict_table_t*		table;
	const dict_col_t*	col;
	dfield_t*		dfield;

	row = node->row;
	table = node->table;

	ut_ad(dtuple_get_n_fields(row) == dict_table_get_n_cols(table));

	/* allocate buffer to hold the needed system created hidden columns. */
	compile_time_assert(DATA_ROW_ID_LEN
			    + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN
			    == sizeof node->sys_buf);
	memset(node->sys_buf, 0, sizeof node->sys_buf);
	/* Assign DB_ROLL_PTR to 1 << ROLL_PTR_INSERT_FLAG_POS */
	node->sys_buf[DATA_ROW_ID_LEN + DATA_TRX_ID_LEN] = 0x80;
	ut_ad(!memcmp(node->sys_buf + DATA_ROW_ID_LEN, reset_trx_id,
		      sizeof reset_trx_id));

	/* 1. Populate row-id */
	col = dict_table_get_sys_col(table, DATA_ROW_ID);

	dfield = dtuple_get_nth_field(row, dict_col_get_no(col));

	dfield_set_data(dfield, node->sys_buf, DATA_ROW_ID_LEN);

	/* 2. Populate trx id */
	col = dict_table_get_sys_col(table, DATA_TRX_ID);

	dfield = dtuple_get_nth_field(row, dict_col_get_no(col));

	dfield_set_data(dfield, &node->sys_buf[DATA_ROW_ID_LEN],
			DATA_TRX_ID_LEN);

	col = dict_table_get_sys_col(table, DATA_ROLL_PTR);

	dfield = dtuple_get_nth_field(row, dict_col_get_no(col));

	dfield_set_data(dfield, &node->sys_buf[DATA_ROW_ID_LEN
					       + DATA_TRX_ID_LEN],
			DATA_ROLL_PTR_LEN);
}

/*********************************************************************//**
Sets a new row to insert for an INS_DIRECT node. This function is only used
if we have constructed the row separately, which is a rare case; this
function is quite slow. */
void
ins_node_set_new_row(
/*=================*/
	ins_node_t*	node,	/*!< in: insert node */
	dtuple_t*	row)	/*!< in: new row (or first row) for the node */
{
	node->state = INS_NODE_SET_IX_LOCK;
	node->index = NULL;
	node->entry_list.clear();
	node->entry = node->entry_list.end();

	node->row = row;

	mem_heap_empty(node->entry_sys_heap);

	/* Create templates for index entries */

	ins_node_create_entry_list(node);

	/* Allocate from entry_sys_heap buffers for sys fields */

	row_ins_alloc_sys_fields(node);

	/* As we allocated a new trx id buf, the trx id should be written
	there again: */

	node->trx_id = 0;
}

/*******************************************************************//**
Does an insert operation by updating a delete-marked existing record
in the index. This situation can occur if the delete-marked record is
kept in the index for consistent reads.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_sec_index_entry_by_modify(
/*==============================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether mtr holds just a leaf
				latch or also a tree latch */
	btr_cur_t*	cursor,	/*!< in: B-tree cursor */
	rec_offs**	offsets,/*!< in/out: offsets on cursor->page_cur.rec */
	mem_heap_t*	offsets_heap,
				/*!< in/out: memory heap that can be emptied */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	const dtuple_t*	entry,	/*!< in: index entry to insert */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr)	/*!< in: mtr; must be committed before
				latching any further pages */
{
	big_rec_t*	dummy_big_rec;
	upd_t*		update;
	rec_t*		rec;
	dberr_t		err;

	rec = btr_cur_get_rec(cursor);

	ut_ad(!dict_index_is_clust(cursor->index));
	ut_ad(rec_offs_validate(rec, cursor->index, *offsets));
	ut_ad(!entry->info_bits);

	/* We know that in the alphabetical ordering, entry and rec are
	identified. But in their binary form there may be differences if
	there are char fields in them. Therefore we have to calculate the
	difference. */

	update = row_upd_build_sec_rec_difference_binary(
		rec, cursor->index, *offsets, entry, heap);

	if (!rec_get_deleted_flag(rec, rec_offs_comp(*offsets))) {
		/* We should never insert in place of a record that
		has not been delete-marked. The only exception is when
		online CREATE INDEX copied the changes that we already
		made to the clustered index, and completed the
		secondary index creation before we got here. In this
		case, the change would already be there. The CREATE
		INDEX should be waiting for a MySQL meta-data lock
		upgrade at least until this INSERT or UPDATE
		returns. After that point, set_committed(true)
		would be invoked in commit_inplace_alter_table(). */
		ut_a(update->n_fields == 0);
		ut_a(!cursor->index->is_committed());
		ut_ad(!dict_index_is_online_ddl(cursor->index));
		return(DB_SUCCESS);
	}

	if (mode == BTR_MODIFY_LEAF) {
		/* Try an optimistic updating of the record, keeping changes
		within the page */

		/* TODO: pass only *offsets */
		err = btr_cur_optimistic_update(
			flags | BTR_KEEP_SYS_FLAG, cursor,
			offsets, &offsets_heap, update, 0, thr,
			thr_get_trx(thr)->id, mtr);
		switch (err) {
		case DB_OVERFLOW:
		case DB_UNDERFLOW:
		case DB_ZIP_OVERFLOW:
			err = DB_FAIL;
		default:
			break;
		}
	} else {
		ut_a(mode == BTR_MODIFY_TREE);
		if (buf_pool.running_out()) {

			return(DB_LOCK_TABLE_FULL);
		}

		err = btr_cur_pessimistic_update(
			flags | BTR_KEEP_SYS_FLAG, cursor,
			offsets, &offsets_heap,
			heap, &dummy_big_rec, update, 0,
			thr, thr_get_trx(thr)->id, mtr);
		ut_ad(!dummy_big_rec);
	}

	return(err);
}

/*******************************************************************//**
Does an insert operation by delete unmarking and updating a delete marked
existing record in the index. This situation can occur if the delete marked
record is kept in the index for consistent reads.
@return DB_SUCCESS, DB_FAIL, or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_clust_index_entry_by_modify(
/*================================*/
	btr_pcur_t*	pcur,	/*!< in/out: a persistent cursor pointing
				to the clust_rec that is being modified. */
	ulint		flags,	/*!< in: undo logging and locking flags */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether mtr holds just a leaf
				latch or also a tree latch */
	rec_offs**	offsets,/*!< out: offsets on cursor->page_cur.rec */
	mem_heap_t**	offsets_heap,
				/*!< in/out: pointer to memory heap that can
				be emptied, or NULL */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	const dtuple_t*	entry,	/*!< in: index entry to insert */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr)	/*!< in: mtr; must be committed before
				latching any further pages */
{
	const rec_t*	rec;
	upd_t*		update;
	dberr_t		err = DB_SUCCESS;
	btr_cur_t*	cursor	= btr_pcur_get_btr_cur(pcur);
	TABLE*		mysql_table = NULL;
	ut_ad(dict_index_is_clust(cursor->index));

	rec = btr_cur_get_rec(cursor);

	ut_ad(rec_get_deleted_flag(rec,
				   dict_table_is_comp(cursor->index->table)));
	/* In delete-marked records, DB_TRX_ID must
	always refer to an existing undo log record. */
	ut_ad(rec_get_trx_id(rec, cursor->index));

	/* Build an update vector containing all the fields to be modified;
	NOTE that this vector may NOT contain system columns trx_id or
	roll_ptr */
	if (thr->prebuilt != NULL) {
		mysql_table = thr->prebuilt->m_mysql_table;
		ut_ad(thr->prebuilt->trx == thr_get_trx(thr));
	}

	update = row_upd_build_difference_binary(
		cursor->index, entry, rec, NULL, true,
		thr_get_trx(thr), heap, mysql_table, &err);
	if (err != DB_SUCCESS) {
		return(err);
	}

	if (mode != BTR_MODIFY_TREE) {
		ut_ad((mode & ulint(~BTR_ALREADY_S_LATCHED))
		      == BTR_MODIFY_LEAF);

		/* Try optimistic updating of the record, keeping changes
		within the page */

		err = btr_cur_optimistic_update(
			flags, cursor, offsets, offsets_heap, update, 0, thr,
			thr_get_trx(thr)->id, mtr);
		switch (err) {
		case DB_OVERFLOW:
		case DB_UNDERFLOW:
		case DB_ZIP_OVERFLOW:
			err = DB_FAIL;
		default:
			break;
		}
	} else {
		if (buf_pool.running_out()) {
			return DB_LOCK_TABLE_FULL;
		}

		big_rec_t*	big_rec	= NULL;

		err = btr_cur_pessimistic_update(
			flags | BTR_KEEP_POS_FLAG,
			cursor, offsets, offsets_heap, heap,
			&big_rec, update, 0, thr, thr_get_trx(thr)->id, mtr);

		if (big_rec) {
			ut_a(err == DB_SUCCESS);

			DEBUG_SYNC_C("before_row_ins_upd_extern");
			err = btr_store_big_rec_extern_fields(
				pcur, *offsets, big_rec, mtr,
				BTR_STORE_INSERT_UPDATE);
			DEBUG_SYNC_C("after_row_ins_upd_extern");
			dtuple_big_rec_free(big_rec);
		}
	}

	return(err);
}

/*********************************************************************//**
Returns TRUE if in a cascaded update/delete an ancestor node of node
updates (not DELETE, but UPDATE) table.
@return TRUE if an ancestor updates table */
static
ibool
row_ins_cascade_ancestor_updates_table(
/*===================================*/
	que_node_t*	node,	/*!< in: node in a query graph */
	dict_table_t*	table)	/*!< in: table */
{
	que_node_t*	parent;

	for (parent = que_node_get_parent(node);
	     que_node_get_type(parent) == QUE_NODE_UPDATE;
	     parent = que_node_get_parent(parent)) {

		upd_node_t*	upd_node;

		upd_node = static_cast<upd_node_t*>(parent);

		if (upd_node->table == table && !upd_node->is_delete) {

			return(TRUE);
		}
	}

	return(FALSE);
}

/*********************************************************************//**
Returns the number of ancestor UPDATE or DELETE nodes of a
cascaded update/delete node.
@return number of ancestors */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
ulint
row_ins_cascade_n_ancestors(
/*========================*/
	que_node_t*	node)	/*!< in: node in a query graph */
{
	que_node_t*	parent;
	ulint		n_ancestors = 0;

	for (parent = que_node_get_parent(node);
	     que_node_get_type(parent) == QUE_NODE_UPDATE;
	     parent = que_node_get_parent(parent)) {

		n_ancestors++;
	}

	return(n_ancestors);
}

/******************************************************************//**
Calculates the update vector node->cascade->update for a child table in
a cascaded update.
@return whether any FULLTEXT INDEX is affected */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
bool
row_ins_cascade_calc_update_vec(
/*============================*/
	upd_node_t*	node,		/*!< in: update node of the parent
					table */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint whose
					type is != 0 */
	mem_heap_t*	heap,		/*!< in: memory heap to use as
					temporary storage */
	trx_t*		trx)		/*!< in: update transaction */
{
	upd_node_t*     cascade         = node->cascade_node;
	dict_table_t*	table		= foreign->foreign_table;
	dict_index_t*	index		= foreign->foreign_index;
	upd_t*		update;
	dict_table_t*	parent_table;
	dict_index_t*	parent_index;
	upd_t*		parent_update;
	ulint		n_fields_updated;
	ulint		parent_field_no;
	ulint		i;
	ulint		j;
	bool		doc_id_updated = false;
	unsigned	doc_id_pos = 0;
	doc_id_t	new_doc_id = FTS_NULL_DOC_ID;
	ulint		prefix_col;

	ut_a(cascade);
	ut_a(table);
	ut_a(index);

	/* Calculate the appropriate update vector which will set the fields
	in the child index record to the same value (possibly padded with
	spaces if the column is a fixed length CHAR or FIXBINARY column) as
	the referenced index record will get in the update. */

	parent_table = node->table;
	ut_a(parent_table == foreign->referenced_table);
	parent_index = foreign->referenced_index;
	parent_update = node->update;

	update = cascade->update;

	update->info_bits = 0;

	n_fields_updated = 0;

	bool affects_fulltext = foreign->affects_fulltext();

	if (table->fts) {
		doc_id_pos = dict_table_get_nth_col_pos(
			table, table->fts->doc_col, &prefix_col);
	}

	for (i = 0; i < foreign->n_fields; i++) {

		parent_field_no = dict_table_get_nth_col_pos(
			parent_table,
			dict_index_get_nth_col_no(parent_index, i),
			&prefix_col);

		for (j = 0; j < parent_update->n_fields; j++) {
			const upd_field_t*	parent_ufield
				= &parent_update->fields[j];

			if (parent_ufield->field_no == parent_field_no) {

				ulint			min_size;
				const dict_col_t*	col;
				ulint			ufield_len;
				upd_field_t*		ufield;

				col = dict_index_get_nth_col(index, i);

				/* A field in the parent index record is
				updated. Let us make the update vector
				field for the child table. */

				ufield = update->fields + n_fields_updated;

				ufield->field_no = static_cast<uint16_t>(
					dict_table_get_nth_col_pos(
						table, dict_col_get_no(col),
						&prefix_col));

				ufield->orig_len = 0;
				ufield->exp = NULL;

				ufield->new_val = parent_ufield->new_val;
				dfield_get_type(&ufield->new_val)->prtype |=
					col->prtype & DATA_VERSIONED;
				ufield_len = dfield_get_len(&ufield->new_val);

				/* Clear the "external storage" flag */
				dfield_set_len(&ufield->new_val, ufield_len);

				/* Do not allow a NOT NULL column to be
				updated as NULL */

				if (dfield_is_null(&ufield->new_val)
				    && (col->prtype & DATA_NOT_NULL)) {
					goto err_exit;
				}

				/* If the new value would not fit in the
				column, do not allow the update */

				if (!dfield_is_null(&ufield->new_val)
				    && dtype_get_at_most_n_mbchars(
					col->prtype,
					col->mbminlen, col->mbmaxlen,
					col->len,
					ufield_len,
					static_cast<char*>(
						dfield_get_data(
							&ufield->new_val)))
				    < ufield_len) {
					goto err_exit;
				}

				/* If the parent column type has a different
				length than the child column type, we may
				need to pad with spaces the new value of the
				child column */

				min_size = dict_col_get_min_size(col);

				/* Because UNIV_SQL_NULL (the marker
				of SQL NULL values) exceeds all possible
				values of min_size, the test below will
				not hold for SQL NULL columns. */

				if (min_size > ufield_len) {

					byte*	pad;
					ulint	pad_len;
					byte*	padded_data;
					ulint	mbminlen;

					padded_data = static_cast<byte*>(
						mem_heap_alloc(
							heap, min_size));

					pad = padded_data + ufield_len;
					pad_len = min_size - ufield_len;

					memcpy(padded_data,
					       dfield_get_data(&ufield
							       ->new_val),
					       ufield_len);

					mbminlen = dict_col_get_mbminlen(col);

					ut_ad(!(ufield_len % mbminlen));
					ut_ad(!(min_size % mbminlen));

					if (mbminlen == 1
					    && dtype_get_charset_coll(
						    col->prtype)
					    == DATA_MYSQL_BINARY_CHARSET_COLL) {
						/* Do not pad BINARY columns */
						goto err_exit;
					}

					row_mysql_pad_col(mbminlen,
							  pad, pad_len);
					dfield_set_data(&ufield->new_val,
							padded_data, min_size);
				}

				/* If Doc ID is updated, check whether the
				Doc ID is valid */
				if (table->fts
				    && ufield->field_no == doc_id_pos) {
					doc_id_t	n_doc_id;

					n_doc_id =
						table->fts->cache->next_doc_id;

					new_doc_id = fts_read_doc_id(
						static_cast<const byte*>(
							dfield_get_data(
							&ufield->new_val)));

					affects_fulltext = true;
					doc_id_updated = true;

					if (new_doc_id <= 0) {
						ib::error() << "FTS Doc ID"
							" must be larger than"
							" 0";
						goto err_exit;
					}

					if (new_doc_id < n_doc_id) {
						ib::error() << "FTS Doc ID"
							" must be larger than "
							<< n_doc_id - 1
							<< " for table "
							<< table->name;
						goto err_exit;
					}
				}

				n_fields_updated++;
			}
		}
	}

	if (affects_fulltext) {
		ut_ad(table->fts);

		if (DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)) {
			doc_id_t	doc_id;
			doc_id_t*	next_doc_id;
			upd_field_t*	ufield;

			next_doc_id = static_cast<doc_id_t*>(mem_heap_alloc(
				heap, sizeof(doc_id_t)));

			ut_ad(!doc_id_updated);
			ufield = update->fields + n_fields_updated;
			fts_get_next_doc_id(table, next_doc_id);
			doc_id = fts_update_doc_id(table, ufield, next_doc_id);
			n_fields_updated++;
			fts_trx_add_op(trx, table, doc_id, FTS_INSERT, NULL);
		} else  {
			if (doc_id_updated) {
				ut_ad(new_doc_id);
				fts_trx_add_op(trx, table, new_doc_id,
					       FTS_INSERT, NULL);
			} else {
				ib::error() << "FTS Doc ID must be updated"
					" along with FTS indexed column for"
					" table " << table->name;
err_exit:
				n_fields_updated = ULINT_UNDEFINED;
			}
		}
	}

	update->n_fields = n_fields_updated;

	return affects_fulltext;
}

/*********************************************************************//**
Set detailed error message associated with foreign key errors for
the given transaction. */
static
void
row_ins_set_detailed(
/*=================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_foreign_t*	foreign)	/*!< in: foreign key constraint */
{
	ut_ad(!srv_read_only_mode);

	mysql_mutex_lock(&srv_misc_tmpfile_mutex);
	rewind(srv_misc_tmpfile);

	if (os_file_set_eof(srv_misc_tmpfile)) {
		ut_print_name(srv_misc_tmpfile, trx,
			      foreign->foreign_table_name);
		std::string fk_str = dict_print_info_on_foreign_key_in_create_format(
			trx, foreign, FALSE);
		fputs(fk_str.c_str(), srv_misc_tmpfile);
		trx_set_detailed_error_from_file(trx, srv_misc_tmpfile);
	} else {
		trx_set_detailed_error(trx, "temp file operation failed");
	}

	mysql_mutex_unlock(&srv_misc_tmpfile_mutex);
}

/*********************************************************************//**
Acquires dict_foreign_err_mutex, rewinds dict_foreign_err_file
and displays information about the given transaction.
The caller must release dict_foreign_err_mutex. */
TRANSACTIONAL_TARGET
static
void
row_ins_foreign_trx_print(
/*======================*/
	trx_t*	trx)	/*!< in: transaction */
{
	ulint	n_rec_locks;
	ulint	n_trx_locks;
	ulint	heap_size;

	ut_ad(!srv_read_only_mode);

	{
		TMLockMutexGuard g{SRW_LOCK_CALL};
		n_rec_locks = trx->lock.n_rec_locks;
		n_trx_locks = UT_LIST_GET_LEN(trx->lock.trx_locks);
		heap_size = mem_heap_get_size(trx->lock.lock_heap);
	}

	mysql_mutex_lock(&dict_foreign_err_mutex);
	rewind(dict_foreign_err_file);
	ut_print_timestamp(dict_foreign_err_file);
	fputs(" Transaction:\n", dict_foreign_err_file);

	trx_print_low(dict_foreign_err_file, trx, 600,
		      n_rec_locks, n_trx_locks, heap_size);

	mysql_mutex_assert_owner(&dict_foreign_err_mutex);
}

/*********************************************************************//**
Reports a foreign key error associated with an update or a delete of a
parent table index entry. */
static
void
row_ins_foreign_report_err(
/*=======================*/
	const char*	errstr,		/*!< in: error string from the viewpoint
					of the parent table */
	que_thr_t*	thr,		/*!< in: query thread whose run_node
					is an update node */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	const rec_t*	rec,		/*!< in: a matching index record in the
					child table */
	const dtuple_t*	entry)		/*!< in: index entry in the parent
					table */
{
	std::string fk_str;

	if (srv_read_only_mode) {
		return;
	}

	FILE*	ef	= dict_foreign_err_file;
	trx_t*	trx	= thr_get_trx(thr);

	row_ins_set_detailed(trx, foreign);

	row_ins_foreign_trx_print(trx);

	fputs("Foreign key constraint fails for table ", ef);
	ut_print_name(ef, trx, foreign->foreign_table_name);
	fputs(":\n", ef);
	fk_str = dict_print_info_on_foreign_key_in_create_format(trx, foreign,
							TRUE);
	fputs(fk_str.c_str(), ef);
	putc('\n', ef);
	fputs(errstr, ef);
	fprintf(ef, " in parent table, in index %s",
		foreign->referenced_index->name());
	if (entry) {
		fputs(" tuple:\n", ef);
		dtuple_print(ef, entry);
	}
	fputs("\nBut in child table ", ef);
	ut_print_name(ef, trx, foreign->foreign_table_name);
	fprintf(ef, ", in index %s", foreign->foreign_index->name());
	if (rec) {
		fputs(", there is a record:\n", ef);
		rec_print(ef, rec, foreign->foreign_index);
	} else {
		fputs(", the record is not available\n", ef);
	}
	putc('\n', ef);

	mysql_mutex_unlock(&dict_foreign_err_mutex);
}

/*********************************************************************//**
Reports a foreign key error to dict_foreign_err_file when we are trying
to add an index entry to a child table. Note that the adding may be the result
of an update, too. */
static
void
row_ins_foreign_report_add_err(
/*===========================*/
	trx_t*		trx,		/*!< in: transaction */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint */
	const rec_t*	rec,		/*!< in: a record in the parent table:
					it does not match entry because we
					have an error! */
	const dtuple_t*	entry)		/*!< in: index entry to insert in the
					child table */
{
	std::string fk_str;

	if (srv_read_only_mode) {
		return;
	}

	FILE*	ef	= dict_foreign_err_file;

	row_ins_set_detailed(trx, foreign);

	row_ins_foreign_trx_print(trx);

	fputs("Foreign key constraint fails for table ", ef);
	ut_print_name(ef, trx, foreign->foreign_table_name);
	fputs(":\n", ef);
	fk_str = dict_print_info_on_foreign_key_in_create_format(trx, foreign,
							TRUE);
	fputs(fk_str.c_str(), ef);
	if (foreign->foreign_index) {
		fprintf(ef, " in parent table, in index %s",
			foreign->foreign_index->name());
	} else {
		fputs(" in parent table", ef);
	}
	if (entry) {
		fputs(" tuple:\n", ef);
		/* TODO: DB_TRX_ID and DB_ROLL_PTR may be uninitialized.
		It would be better to only display the user columns. */
		dtuple_print(ef, entry);
	}
	fputs("\nBut in parent table ", ef);
	ut_print_name(ef, trx, foreign->referenced_table_name);
	fprintf(ef, ", in index %s,\n"
		"the closest match we can find is record:\n",
		foreign->referenced_index->name());
	if (rec && page_rec_is_supremum(rec)) {
		/* If the cursor ended on a supremum record, it is better
		to report the previous record in the error message, so that
		the user gets a more descriptive error message. */
		rec = page_rec_get_prev_const(rec);
	}

	if (rec) {
		rec_print(ef, rec, foreign->referenced_index);
	}
	putc('\n', ef);

	mysql_mutex_unlock(&dict_foreign_err_mutex);
}

/*********************************************************************//**
Invalidate the query cache for the given table. */
static
void
row_ins_invalidate_query_cache(
/*===========================*/
	que_thr_t*	thr,		/*!< in: query thread whose run_node
					is an update node */
	const char*	name)		/*!< in: table name prefixed with
					database name and a '/' character */
{
	innobase_invalidate_query_cache(thr_get_trx(thr), name);
}


/** Fill virtual column information in cascade node for the child table.
@param[out]	cascade		child update node
@param[in]	rec		clustered rec of child table
@param[in]	index		clustered index of child table
@param[in]	node		parent update node
@param[in]	foreign		foreign key information
@return		error code. */
static
dberr_t
row_ins_foreign_fill_virtual(
	upd_node_t*		cascade,
	const rec_t*		rec,
	dict_index_t*		index,
	upd_node_t*		node,
	dict_foreign_t*		foreign)
{
	THD*		thd = current_thd;
	row_ext_t*	ext;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets_);
	const rec_offs*	offsets =
		rec_get_offsets(rec, index, offsets_, index->n_core_fields,
				ULINT_UNDEFINED, &cascade->heap);
	TABLE*		mysql_table= NULL;
	upd_t*		update = cascade->update;
	ulint		n_v_fld = index->table->n_v_def;
	ulint		n_diff;
	upd_field_t*	upd_field;
	dict_vcol_set*	v_cols = foreign->v_cols;
	update->old_vrow = row_build(
		ROW_COPY_DATA, index, rec,
		offsets, index->table, NULL, NULL,
		&ext, update->heap);
	n_diff = update->n_fields;

	if (index->table->vc_templ == NULL) {
		/** This can occur when there is a cascading
		delete or update after restart. */
		innobase_init_vc_templ(index->table);
	}

	ib_vcol_row vc(NULL);
	uchar *record = vc.record(thd, index, &mysql_table);
	if (!record) {
		return DB_OUT_OF_MEMORY;
	}

	for (uint16_t i = 0; i < n_v_fld; i++) {

		dict_v_col_t*     col = dict_table_get_nth_v_col(
				index->table, i);

		dict_vcol_set::iterator it = v_cols->find(col);

		if (it == v_cols->end()) {
			continue;
		}

		dfield_t*	vfield = innobase_get_computed_value(
				update->old_vrow, col, index,
				&vc.heap, update->heap, NULL, thd, mysql_table,
                                record, NULL, NULL, NULL);

		if (vfield == NULL) {
			return DB_COMPUTE_VALUE_FAILED;
		}

		upd_field = update->fields + n_diff;

		upd_field->old_v_val = static_cast<dfield_t*>(
			mem_heap_alloc(update->heap,
				       sizeof *upd_field->old_v_val));

		dfield_copy(upd_field->old_v_val, vfield);

		upd_field_set_v_field_no(upd_field, i, index);

		bool set_null =
			node->is_delete
			? (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL)
			: (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL);

		dfield_t* new_vfield = innobase_get_computed_value(
				update->old_vrow, col, index,
				&vc.heap, update->heap, NULL, thd,
				mysql_table, record, NULL,
				set_null ? update : node->update, foreign);

		if (new_vfield == NULL) {
			return DB_COMPUTE_VALUE_FAILED;
		}

		dfield_copy(&upd_field->new_val, new_vfield);

		if (!dfield_datas_are_binary_equal(
				upd_field->old_v_val,
				&upd_field->new_val, 0))
			n_diff++;
	}

	update->n_fields = n_diff;
	return DB_SUCCESS;
}

#ifdef WITH_WSREP
dberr_t wsrep_append_foreign_key(trx_t *trx,
			       dict_foreign_t*	foreign,
			       const rec_t*	clust_rec,
			       dict_index_t*	clust_index,
			       ibool		referenced,
			       Wsrep_service_key_type	key_type);
#endif /* WITH_WSREP */

/*********************************************************************//**
Perform referential actions or checks when a parent row is deleted or updated
and the constraint had an ON DELETE or ON UPDATE condition which was not
RESTRICT.
@return DB_SUCCESS, DB_LOCK_WAIT, or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_foreign_check_on_constraint(
/*================================*/
	que_thr_t*	thr,		/*!< in: query thread whose run_node
					is an update node */
	dict_foreign_t*	foreign,	/*!< in: foreign key constraint whose
					type is != 0 */
	btr_pcur_t*	pcur,		/*!< in: cursor placed on a matching
					index record in the child table */
	dtuple_t*	entry,		/*!< in: index entry in the parent
					table */
	mtr_t*		mtr)		/*!< in: mtr holding the latch of pcur
					page */
{
	upd_node_t*	node;
	upd_node_t*	cascade;
	dict_table_t*	table		= foreign->foreign_table;
	dict_index_t*	index;
	dict_index_t*	clust_index;
	dtuple_t*	ref;
	const rec_t*	rec;
	const rec_t*	clust_rec;
	const buf_block_t* clust_block;
	upd_t*		update;
	dberr_t		err;
	trx_t*		trx;
	mem_heap_t*	tmp_heap	= NULL;
	doc_id_t	doc_id = FTS_NULL_DOC_ID;

	DBUG_ENTER("row_ins_foreign_check_on_constraint");

	trx = thr_get_trx(thr);

	/* Since we are going to delete or update a row, we have to invalidate
	the MySQL query cache for table. A deadlock of threads is not possible
	here because the caller of this function does not hold any latches with
	the mutex rank above the lock_sys.latch. The query cache mutex
	has a rank just above the lock_sys.latch. */

	row_ins_invalidate_query_cache(thr, table->name.m_name);

	node = static_cast<upd_node_t*>(thr->run_node);

	if (node->is_delete && 0 == (foreign->type
				     & (DICT_FOREIGN_ON_DELETE_CASCADE
					| DICT_FOREIGN_ON_DELETE_SET_NULL))) {

		row_ins_foreign_report_err("Trying to delete",
					   thr, foreign,
					   btr_pcur_get_rec(pcur), entry);

		DBUG_RETURN(DB_ROW_IS_REFERENCED);
	}

	if (!node->is_delete && 0 == (foreign->type
				      & (DICT_FOREIGN_ON_UPDATE_CASCADE
					 | DICT_FOREIGN_ON_UPDATE_SET_NULL))) {

		/* This is an UPDATE */

		row_ins_foreign_report_err("Trying to update",
					   thr, foreign,
					   btr_pcur_get_rec(pcur), entry);

		DBUG_RETURN(DB_ROW_IS_REFERENCED);
	}

	if (node->cascade_node == NULL) {
		node->cascade_heap = mem_heap_create(128);
		node->cascade_node = row_create_update_node_for_mysql(
			table, node->cascade_heap);
		que_node_set_parent(node->cascade_node, node);

	}
	cascade = node->cascade_node;
	cascade->table = table;
	cascade->foreign = foreign;

	if (node->is_delete
	    && (foreign->type & DICT_FOREIGN_ON_DELETE_CASCADE)) {
		cascade->is_delete = PLAIN_DELETE;
	} else {
		cascade->is_delete = NO_DELETE;

		if (foreign->n_fields > cascade->update_n_fields) {
			/* We have to make the update vector longer */

			cascade->update = upd_create(foreign->n_fields,
						     node->cascade_heap);
			cascade->update_n_fields = foreign->n_fields;
		}

		/* We do not allow cyclic cascaded updating (DELETE is
		allowed, but not UPDATE) of the same table, as this
		can lead to an infinite cycle. Check that we are not
		updating the same table which is already being
		modified in this cascade chain. We have to check this
		also because the modification of the indexes of a
		'parent' table may still be incomplete, and we must
		avoid seeing the indexes of the parent table in an
		inconsistent state! */

		if (row_ins_cascade_ancestor_updates_table(cascade, table)) {

			/* We do not know if this would break foreign key
			constraints, but play safe and return an error */

			err = DB_ROW_IS_REFERENCED;

			row_ins_foreign_report_err(
				"Trying an update, possibly causing a cyclic"
				" cascaded update\n"
				"in the child table,", thr, foreign,
				btr_pcur_get_rec(pcur), entry);

			goto nonstandard_exit_func;
		}
	}

	if (row_ins_cascade_n_ancestors(cascade) >= FK_MAX_CASCADE_DEL) {
		err = DB_FOREIGN_EXCEED_MAX_CASCADE;

		row_ins_foreign_report_err(
			"Trying a too deep cascaded delete or update\n",
			thr, foreign, btr_pcur_get_rec(pcur), entry);

		goto nonstandard_exit_func;
	}

	index = btr_pcur_get_btr_cur(pcur)->index;

	ut_a(index == foreign->foreign_index);

	rec = btr_pcur_get_rec(pcur);

	tmp_heap = mem_heap_create(256);

	if (dict_index_is_clust(index)) {
		/* pcur is already positioned in the clustered index of
		the child table */

		clust_index = index;
		clust_rec = rec;
		clust_block = btr_pcur_get_block(pcur);
	} else {
		/* We have to look for the record in the clustered index
		in the child table */

		clust_index = dict_table_get_first_index(table);

		ref = row_build_row_ref(ROW_COPY_POINTERS, index, rec,
					tmp_heap);
		btr_pcur_open_with_no_init(clust_index, ref,
					   PAGE_CUR_LE, BTR_SEARCH_LEAF,
					   cascade->pcur, 0, mtr);

		clust_rec = btr_pcur_get_rec(cascade->pcur);
		clust_block = btr_pcur_get_block(cascade->pcur);

		if (!page_rec_is_user_rec(clust_rec)
		    || btr_pcur_get_low_match(cascade->pcur)
		    < dict_index_get_n_unique(clust_index)) {

			ib::error() << "In cascade of a foreign key op index "
				<< index->name
				<< " of table " << index->table->name;

			fputs("InnoDB: record ", stderr);
			rec_print(stderr, rec, index);
			fputs("\n"
			      "InnoDB: clustered record ", stderr);
			rec_print(stderr, clust_rec, clust_index);
			fputs("\n"
			      "InnoDB: Submit a detailed bug report to"
			      " https://jira.mariadb.org/\n", stderr);
			ut_ad(0);
			err = DB_SUCCESS;

			goto nonstandard_exit_func;
		}
	}

	/* Set an X-lock on the row to delete or update in the child table */

	err = lock_table(table, LOCK_IX, thr);

	if (err == DB_SUCCESS) {
		/* Here it suffices to use a LOCK_REC_NOT_GAP type lock;
		we already have a normal shared lock on the appropriate
		gap if the search criterion was not unique */

		err = lock_clust_rec_read_check_and_lock_alt(
			0, clust_block, clust_rec, clust_index,
			LOCK_X, LOCK_REC_NOT_GAP, thr);
	}

	if (err != DB_SUCCESS) {

		goto nonstandard_exit_func;
	}

	if (rec_get_deleted_flag(clust_rec, dict_table_is_comp(table))) {
		/* In delete-marked records, DB_TRX_ID must
		always refer to an existing undo log record. */
		ut_ad(rec_get_trx_id(clust_rec, clust_index));
		/* This can happen if there is a circular reference of
		rows such that cascading delete comes to delete a row
		already in the process of being delete marked */
		err = DB_SUCCESS;

		goto nonstandard_exit_func;
	}

	if (table->fts) {
		doc_id = fts_get_doc_id_from_rec(
			clust_rec, clust_index,
			rec_get_offsets(clust_rec, clust_index, NULL,
					clust_index->n_core_fields,
					ULINT_UNDEFINED, &tmp_heap));
	}

	if (node->is_delete
	    ? (foreign->type & DICT_FOREIGN_ON_DELETE_SET_NULL)
	    : (foreign->type & DICT_FOREIGN_ON_UPDATE_SET_NULL)) {
		/* Build the appropriate update vector which sets
		foreign->n_fields first fields in rec to SQL NULL */

		update = cascade->update;

		update->info_bits = 0;
		update->n_fields = foreign->n_fields;
		MEM_UNDEFINED(update->fields,
			      update->n_fields * sizeof *update->fields);

		for (ulint i = 0; i < foreign->n_fields; i++) {
			upd_field_t*	ufield = &update->fields[i];
			ulint		col_no = dict_index_get_nth_col_no(
						index, i);
			ulint		prefix_col;

			ufield->field_no = static_cast<uint16_t>(
				dict_table_get_nth_col_pos(
					table, col_no, &prefix_col));
			dict_col_t*	col = dict_table_get_nth_col(
				table, col_no);
			dict_col_copy_type(col, dfield_get_type(&ufield->new_val));

			ufield->orig_len = 0;
			ufield->exp = NULL;
			dfield_set_null(&ufield->new_val);
		}

		if (foreign->affects_fulltext()) {
			fts_trx_add_op(trx, table, doc_id, FTS_DELETE, NULL);
		}

		if (foreign->v_cols != NULL
		    && foreign->v_cols->size() > 0) {
			err = row_ins_foreign_fill_virtual(
				cascade, clust_rec, clust_index,
				node, foreign);

			if (err != DB_SUCCESS) {
				goto nonstandard_exit_func;
			}
		}
	} else if (table->fts && cascade->is_delete == PLAIN_DELETE
		   && foreign->affects_fulltext()) {
		/* DICT_FOREIGN_ON_DELETE_CASCADE case */
		fts_trx_add_op(trx, table, doc_id, FTS_DELETE, NULL);
	}

	if (!node->is_delete
	    && (foreign->type & DICT_FOREIGN_ON_UPDATE_CASCADE)) {

		/* Build the appropriate update vector which sets changing
		foreign->n_fields first fields in rec to new values */

		bool affects_fulltext = row_ins_cascade_calc_update_vec(
			node, foreign, tmp_heap, trx);

		if (foreign->v_cols && !foreign->v_cols->empty()) {
			err = row_ins_foreign_fill_virtual(
				cascade, clust_rec, clust_index,
				node, foreign);

			if (err != DB_SUCCESS) {
				goto nonstandard_exit_func;
			}
		}

		switch (cascade->update->n_fields) {
		case ULINT_UNDEFINED:
			err = DB_ROW_IS_REFERENCED;

			row_ins_foreign_report_err(
				"Trying a cascaded update where the"
				" updated value in the child\n"
				"table would not fit in the length"
				" of the column, or the value would\n"
				"be NULL and the column is"
				" declared as not NULL in the child table,",
				thr, foreign, btr_pcur_get_rec(pcur), entry);

			goto nonstandard_exit_func;
		case 0:
			/* The update does not change any columns referred
			to in this foreign key constraint: no need to do
			anything */

			err = DB_SUCCESS;

			goto nonstandard_exit_func;
		}

		/* Mark the old Doc ID as deleted */
		if (affects_fulltext) {
			ut_ad(table->fts);
			fts_trx_add_op(trx, table, doc_id, FTS_DELETE, NULL);
		}
	}

	if (table->versioned() && cascade->is_delete != PLAIN_DELETE
	    && cascade->update->affects_versioned()) {
		ut_ad(!cascade->historical_heap);
		cascade->historical_heap = mem_heap_create(srv_page_size);
		cascade->historical_row = row_build(
			ROW_COPY_DATA, clust_index, clust_rec, NULL, table,
			NULL, NULL, NULL, cascade->historical_heap);
	}

	/* Store pcur position and initialize or store the cascade node
	pcur stored position */

	btr_pcur_store_position(pcur, mtr);

	if (index == clust_index) {
		btr_pcur_copy_stored_position(cascade->pcur, pcur);
	} else {
		btr_pcur_store_position(cascade->pcur, mtr);
	}

#ifdef WITH_WSREP
	err = wsrep_append_foreign_key(trx, foreign, clust_rec, clust_index,
				       FALSE, WSREP_SERVICE_KEY_EXCLUSIVE);
	if (err != DB_SUCCESS) {
		ib::info() << "WSREP: foreign key append failed: " <<  err;
		goto nonstandard_exit_func;
	}
#endif /* WITH_WSREP */
	mtr_commit(mtr);

	ut_a(cascade->pcur->rel_pos == BTR_PCUR_ON);

	cascade->state = UPD_NODE_UPDATE_CLUSTERED;

	err = row_update_cascade_for_mysql(thr, cascade,
					   foreign->foreign_table);

	mtr_start(mtr);

	/* Restore pcur position */

	btr_pcur_restore_position(BTR_SEARCH_LEAF, pcur, mtr);

	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	DBUG_RETURN(err);

nonstandard_exit_func:

	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	btr_pcur_store_position(pcur, mtr);

	mtr_commit(mtr);
	mtr_start(mtr);

	btr_pcur_restore_position(BTR_SEARCH_LEAF, pcur, mtr);

	DBUG_RETURN(err);
}

/*********************************************************************//**
Sets a shared lock on a record. Used in locking possible duplicate key
records and also in checking foreign key constraints.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, or error code */
static
dberr_t
row_ins_set_shared_rec_lock(
/*========================*/
	unsigned		type,	/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP type lock */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: record */
	dict_index_t*		index,	/*!< in: index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;

	ut_ad(rec_offs_validate(rec, index, offsets));

	if (dict_index_is_clust(index)) {
		err = lock_clust_rec_read_check_and_lock(
			0, block, rec, index, offsets, LOCK_S, type, thr);
	} else {
		err = lock_sec_rec_read_check_and_lock(
			0, block, rec, index, offsets, LOCK_S, type, thr);
	}

	return(err);
}

/*********************************************************************//**
Sets a exclusive lock on a record. Used in locking possible duplicate key
records
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, or error code */
static
dberr_t
row_ins_set_exclusive_rec_lock(
/*===========================*/
	unsigned		type,	/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP type lock */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: record */
	dict_index_t*		index,	/*!< in: index */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(rec, index) */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;

	ut_ad(rec_offs_validate(rec, index, offsets));

	if (dict_index_is_clust(index)) {
		err = lock_clust_rec_read_check_and_lock(
			0, block, rec, index, offsets, LOCK_X, type, thr);
	} else {
		err = lock_sec_rec_read_check_and_lock(
			0, block, rec, index, offsets, LOCK_X, type, thr);
	}

	return(err);
}

/***************************************************************//**
Checks if foreign key constraint fails for an index entry. Sets shared locks
which lock either the success or the failure of the constraint. NOTE that
the caller must have a shared latch on dict_sys.latch.
@return DB_SUCCESS, DB_NO_REFERENCED_ROW, or DB_ROW_IS_REFERENCED */
dberr_t
row_ins_check_foreign_constraint(
/*=============================*/
	ibool		check_ref,/*!< in: TRUE if we want to check that
				the referenced table is ok, FALSE if we
				want to check the foreign key table */
	dict_foreign_t*	foreign,/*!< in: foreign constraint; NOTE that the
				tables mentioned in it must be in the
				dictionary cache if they exist at all */
	dict_table_t*	table,	/*!< in: if check_ref is TRUE, then the foreign
				table, else the referenced table */
	dtuple_t*	entry,	/*!< in: index entry for index */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t		err;
	upd_node_t*	upd_node;
	dict_table_t*	check_table;
	dict_index_t*	check_index;
	ulint		n_fields_cmp;
	btr_pcur_t	pcur;
	int		cmp;
	mtr_t		mtr;
	trx_t*		trx		= thr_get_trx(thr);
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;

	bool		skip_gap_lock;

	skip_gap_lock = (trx->isolation_level <= TRX_ISO_READ_COMMITTED);

	DBUG_ENTER("row_ins_check_foreign_constraint");

	rec_offs_init(offsets_);

#ifdef WITH_WSREP
	upd_node= NULL;
#endif /* WITH_WSREP */

	err = DB_SUCCESS;

	if (trx->check_foreigns == FALSE) {
		/* The user has suppressed foreign key checks currently for
		this session */
		goto exit_func;
	}

	/* If any of the foreign key fields in entry is SQL NULL, we
	suppress the foreign key check: this is compatible with Oracle,
	for example */
	for (ulint i = 0; i < entry->n_fields; i++) {
		dfield_t* field = dtuple_get_nth_field(entry, i);
		if (i < foreign->n_fields && dfield_is_null(field)) {
			goto exit_func;
		}
		/* System Versioning: if row_end != Inf, we
		suppress the foreign key check */
		if (field->type.vers_sys_end() && field->vers_history_row()) {
			goto exit_func;
		}
	}

	if (que_node_get_type(thr->run_node) == QUE_NODE_UPDATE) {
		upd_node = static_cast<upd_node_t*>(thr->run_node);

		if (upd_node->is_delete != PLAIN_DELETE
		    && upd_node->foreign == foreign) {
			/* If a cascaded update is done as defined by a
			foreign key constraint, do not check that
			constraint for the child row. In ON UPDATE CASCADE
			the update of the parent row is only half done when
			we come here: if we would check the constraint here
			for the child row it would fail.

			A QUESTION remains: if in the child table there are
			several constraints which refer to the same parent
			table, we should merge all updates to the child as
			one update? And the updates can be contradictory!
			Currently we just perform the update associated
			with each foreign key constraint, one after
			another, and the user has problems predicting in
			which order they are performed. */

			goto exit_func;
		}
	}

	if (que_node_get_type(thr->run_node) == QUE_NODE_INSERT) {
		ins_node_t* insert_node =
			static_cast<ins_node_t*>(thr->run_node);
		dict_table_t* table = insert_node->index->table;
		if (table->versioned()) {
			dfield_t* row_end = dtuple_get_nth_field(
				insert_node->row, table->vers_end);
			if (row_end->vers_history_row()) {
				goto exit_func;
			}
		}
	}

	if (check_ref) {
		check_table = foreign->referenced_table;
		check_index = foreign->referenced_index;
	} else {
		check_table = foreign->foreign_table;
		check_index = foreign->foreign_index;
	}

	if (check_table == NULL
	    || !check_table->is_readable()
	    || check_index == NULL) {

		FILE*	ef = dict_foreign_err_file;
		std::string fk_str;

		row_ins_set_detailed(trx, foreign);
		row_ins_foreign_trx_print(trx);

		fputs("Foreign key constraint fails for table ", ef);
		ut_print_name(ef, trx, check_ref
			      ? foreign->foreign_table_name
			      : foreign->referenced_table_name);
		fputs(":\n", ef);
		fk_str = dict_print_info_on_foreign_key_in_create_format(
			trx, foreign, TRUE);
		fputs(fk_str.c_str(), ef);
		if (check_ref) {
			if (foreign->foreign_index) {
				fprintf(ef, "\nTrying to add to index %s"
					" tuple:\n",
					foreign->foreign_index->name());
			} else {
				fputs("\nTrying to add tuple:\n", ef);
			}
			dtuple_print(ef, entry);
			fputs("\nBut the parent table ", ef);
			ut_print_name(ef, trx, foreign->referenced_table_name);
			fputs("\nor its .ibd file or the required index does"
			      " not currently exist!\n", ef);
			err = DB_NO_REFERENCED_ROW;
		} else {
			if (foreign->referenced_index) {
				fprintf(ef, "\nTrying to modify index %s"
					" tuple:\n",
					foreign->referenced_index->name());
			} else {
				fputs("\nTrying to modify tuple:\n", ef);
			}
			dtuple_print(ef, entry);
			fputs("\nBut the referencing table ", ef);
			ut_print_name(ef, trx, foreign->foreign_table_name);
			fputs("\nor its .ibd file or the required index does"
			      " not currently exist!\n", ef);
			err = DB_ROW_IS_REFERENCED;
		}

		mysql_mutex_unlock(&dict_foreign_err_mutex);
		goto exit_func;
	}

	if (check_table != table) {
		/* We already have a LOCK_IX on table, but not necessarily
		on check_table */

		err = lock_table(check_table, LOCK_IS, thr);

		if (err != DB_SUCCESS) {

			goto do_possible_lock_wait;
		}
	}

	mtr_start(&mtr);

	/* Store old value on n_fields_cmp */

	n_fields_cmp = dtuple_get_n_fields_cmp(entry);

	dtuple_set_n_fields_cmp(entry, foreign->n_fields);

	btr_pcur_open(check_index, entry, PAGE_CUR_GE,
		      BTR_SEARCH_LEAF, &pcur, &mtr);

	/* Scan index records and check if there is a matching record */

	do {
		const rec_t*		rec = btr_pcur_get_rec(&pcur);
		const buf_block_t*	block = btr_pcur_get_block(&pcur);

		if (page_rec_is_infimum(rec)) {

			continue;
		}

		offsets = rec_get_offsets(rec, check_index, offsets,
					  check_index->n_core_fields,
					  ULINT_UNDEFINED, &heap);

		if (page_rec_is_supremum(rec)) {

			if (skip_gap_lock) {

				continue;
			}

			err = row_ins_set_shared_rec_lock(LOCK_ORDINARY, block,
							  rec, check_index,
							  offsets, thr);
			switch (err) {
			case DB_SUCCESS_LOCKED_REC:
			case DB_SUCCESS:
				continue;
			default:
				goto end_scan;
			}
		}

		cmp = cmp_dtuple_rec(entry, rec, offsets);

		if (cmp == 0) {
			if (rec_get_deleted_flag(rec,
						 rec_offs_comp(offsets))) {
				/* In delete-marked records, DB_TRX_ID must
				always refer to an existing undo log record. */
				ut_ad(!dict_index_is_clust(check_index)
				      || row_get_rec_trx_id(rec, check_index,
							    offsets));

				err = row_ins_set_shared_rec_lock(
					skip_gap_lock
					? LOCK_REC_NOT_GAP
					: LOCK_ORDINARY, block,
					rec, check_index, offsets, thr);
				switch (err) {
				case DB_SUCCESS_LOCKED_REC:
				case DB_SUCCESS:
					break;
				default:
					goto end_scan;
				}
			} else {
				if (check_table->versioned()) {
					bool history_row = false;

					if (check_index->is_primary()) {
						history_row = check_index->
							vers_history_row(rec,
									 offsets);
					} else if (check_index->
						vers_history_row(rec,
								 history_row)) {
						break;
					}

					if (history_row) {
						continue;
					}
				}
				/* Found a matching record. Lock only
				a record because we can allow inserts
				into gaps */

				err = row_ins_set_shared_rec_lock(
					LOCK_REC_NOT_GAP, block,
					rec, check_index, offsets, thr);

				switch (err) {
				case DB_SUCCESS_LOCKED_REC:
				case DB_SUCCESS:
					break;
				default:
					goto end_scan;
				}

				if (check_ref) {
					err = DB_SUCCESS;
#ifdef WITH_WSREP
					err = wsrep_append_foreign_key(
						thr_get_trx(thr),
						foreign,
						rec,
						check_index,
						check_ref,
						(upd_node != NULL
						 && wsrep_protocol_version < 4)
						? WSREP_SERVICE_KEY_SHARED
						: WSREP_SERVICE_KEY_REFERENCE);
					if (err != DB_SUCCESS) {
						fprintf(stderr,
							"WSREP: foreign key append failed: %d\n", err);
					}
#endif /* WITH_WSREP */
					goto end_scan;
				} else if (foreign->type != 0) {
					/* There is an ON UPDATE or ON DELETE
					condition: check them in a separate
					function */

					err = row_ins_foreign_check_on_constraint(
						thr, foreign, &pcur, entry,
						&mtr);
					if (err != DB_SUCCESS) {
						/* Since reporting a plain
						"duplicate key" error
						message to the user in
						cases where a long CASCADE
						operation would lead to a
						duplicate key in some
						other table is very
						confusing, map duplicate
						key errors resulting from
						FK constraints to a
						separate error code. */

						if (err == DB_DUPLICATE_KEY) {
							err = DB_FOREIGN_DUPLICATE_KEY;
						}

						goto end_scan;
					}

					/* row_ins_foreign_check_on_constraint
					may have repositioned pcur on a
					different block */
					block = btr_pcur_get_block(&pcur);
				} else {
					row_ins_foreign_report_err(
						"Trying to delete or update",
						thr, foreign, rec, entry);

					err = DB_ROW_IS_REFERENCED;
					goto end_scan;
				}
			}
		} else {
			ut_a(cmp < 0);

			err = skip_gap_lock
				? DB_SUCCESS
				: row_ins_set_shared_rec_lock(
					LOCK_GAP, block,
					rec, check_index, offsets, thr);

			switch (err) {
			case DB_SUCCESS_LOCKED_REC:
				err = DB_SUCCESS;
				/* fall through */
			case DB_SUCCESS:
				if (check_ref) {
					err = DB_NO_REFERENCED_ROW;
					row_ins_foreign_report_add_err(
						trx, foreign, rec, entry);
				}
			default:
				break;
			}

			goto end_scan;
		}
	} while (btr_pcur_move_to_next(&pcur, &mtr));

	if (check_ref) {
		row_ins_foreign_report_add_err(
			trx, foreign, btr_pcur_get_rec(&pcur), entry);
		err = DB_NO_REFERENCED_ROW;
	} else {
		err = DB_SUCCESS;
	}

end_scan:
	btr_pcur_close(&pcur);

	mtr_commit(&mtr);

	/* Restore old value */
	dtuple_set_n_fields_cmp(entry, n_fields_cmp);

do_possible_lock_wait:
	if (err == DB_LOCK_WAIT) {
		trx->error_state = err;

		thr->lock_state = QUE_THR_LOCK_ROW;

		err = lock_wait(thr);

		thr->lock_state = QUE_THR_LOCK_NOLOCK;

		if (err != DB_SUCCESS) {
		} else if (check_table->name.is_temporary()) {
			err = DB_LOCK_WAIT_TIMEOUT;
		} else {
			err = DB_LOCK_WAIT;
		}
	}

exit_func:
	if (heap != NULL) {
		mem_heap_free(heap);
	}

	DBUG_RETURN(err);
}

/** Sets the values of the dtuple fields in ref_entry from the values of
foreign columns in entry.
@param[in]	foreign		foreign key constraint
@param[in]	index		clustered index
@param[in]	entry		tuple of clustered index
@param[in]	ref_entry	tuple of foreign columns
@return true if all foreign key fields present in clustered index */
static
bool row_ins_foreign_index_entry(dict_foreign_t *foreign,
                                 const dict_index_t *index,
                                 const dtuple_t *entry,
                                 dtuple_t *ref_entry)
{
  for (ulint i= 0; i < foreign->n_fields; i++)
  {
    for (ulint j= 0; j < index->n_fields; j++)
    {
      const dict_col_t *col= dict_index_get_nth_col(index, j);

      /* A clustered index may contain instantly dropped columns,
      which must be skipped. */
      if (col->is_dropped())
        continue;

      const char *col_name= dict_table_get_col_name(index->table, col->ind);
      if (0 == innobase_strcasecmp(col_name, foreign->foreign_col_names[i]))
      {
        dfield_copy(&ref_entry->fields[i], &entry->fields[j]);
        goto got_match;
      }
    }
    return false;
got_match:
    continue;
  }

  return true;
}

/***************************************************************//**
Checks if foreign key constraints fail for an index entry. If index
is not mentioned in any constraint, this function does nothing,
Otherwise does searches to the indexes of referenced tables and
sets shared locks which lock either the success or the failure of
a constraint.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_check_foreign_constraints(
/*==============================*/
	dict_table_t*	table,	/*!< in: table */
	dict_index_t*	index,	/*!< in: index */
	bool		pk,	/*!< in: index->is_primary() */
	dtuple_t*	entry,	/*!< in: index entry for index */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dict_foreign_t*	foreign;
	dberr_t		err = DB_SUCCESS;
	mem_heap_t*	heap = NULL;

	DBUG_ASSERT(index->is_primary() == pk);

	DEBUG_SYNC_C_IF_THD(thr_get_trx(thr)->mysql_thd,
			    "foreign_constraint_check_for_ins");

	for (dict_foreign_set::iterator it = table->foreign_set.begin();
	     err == DB_SUCCESS && it != table->foreign_set.end();
	     ++it) {

		foreign = *it;

		if (foreign->foreign_index == index
		    || (pk && !foreign->foreign_index)) {

			dtuple_t*	ref_tuple = entry;
			if (UNIV_UNLIKELY(!foreign->foreign_index)) {
				/* Change primary key entry to
				foreign key index entry */
				if (!heap) {
					heap = mem_heap_create(1000);
				} else {
					mem_heap_empty(heap);
				}

				ref_tuple = dtuple_create(
					heap, foreign->n_fields);
				dtuple_set_n_fields_cmp(
					ref_tuple, foreign->n_fields);
				if (!row_ins_foreign_index_entry(
					foreign, index, entry, ref_tuple)) {
					err = DB_NO_REFERENCED_ROW;
					break;
				}

			}

			dict_table_t*	ref_table = NULL;
			dict_table_t*	referenced_table
						= foreign->referenced_table;

			if (referenced_table == NULL) {

				ref_table = dict_table_open_on_name(
					foreign->referenced_table_name_lookup,
					false, DICT_ERR_IGNORE_NONE);
			}

			err = row_ins_check_foreign_constraint(
				TRUE, foreign, table, ref_tuple, thr);

			if (ref_table) {
				dict_table_close(ref_table);
			}
		}
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return err;
}

/***************************************************************//**
Checks if a unique key violation to rec would occur at the index entry
insert.
@return TRUE if error */
static
ibool
row_ins_dupl_error_with_rec(
/*========================*/
	const rec_t*	rec,	/*!< in: user record; NOTE that we assume
				that the caller already has a record lock on
				the record! */
	const dtuple_t*	entry,	/*!< in: entry to insert */
	dict_index_t*	index,	/*!< in: index */
	const rec_offs*	offsets)/*!< in: rec_get_offsets(rec, index) */
{
	ulint	matched_fields;
	ulint	n_unique;
	ulint	i;

	ut_ad(rec_offs_validate(rec, index, offsets));

	n_unique = dict_index_get_n_unique(index);

	matched_fields = 0;

	cmp_dtuple_rec_with_match(entry, rec, offsets, &matched_fields);

	if (matched_fields < n_unique) {

		return(FALSE);
	}

	/* In a unique secondary index we allow equal key values if they
	contain SQL NULLs */

	if (!dict_index_is_clust(index) && !index->nulls_equal) {

		for (i = 0; i < n_unique; i++) {
			if (dfield_is_null(dtuple_get_nth_field(entry, i))) {

				return(FALSE);
			}
		}
	}

	return(!rec_get_deleted_flag(rec, rec_offs_comp(offsets)));
}

/***************************************************************//**
Scans a unique non-clustered index at a given index entry to determine
whether a uniqueness violation has occurred for the key value of the entry.
Set shared locks on possible duplicate records.
@return DB_SUCCESS, DB_DUPLICATE_KEY, or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_scan_sec_index_for_duplicate(
/*=================================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	dict_index_t*	index,	/*!< in: non-clustered unique index */
	dtuple_t*	entry,	/*!< in: index entry */
	que_thr_t*	thr,	/*!< in: query thread */
	bool		s_latch,/*!< in: whether index->lock is being held */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	mem_heap_t*	offsets_heap)
				/*!< in/out: memory heap that can be emptied */
{
	ulint		n_unique;
	int		cmp;
	ulint		n_fields_cmp;
	btr_pcur_t	pcur;
	dberr_t		err		= DB_SUCCESS;
	ulint		allow_duplicates;
	rec_offs	offsets_[REC_OFFS_SEC_INDEX_SIZE];
	rec_offs*	offsets		= offsets_;
	DBUG_ENTER("row_ins_scan_sec_index_for_duplicate");

	rec_offs_init(offsets_);

	ut_ad(s_latch == (index->lock.have_u_not_x() || index->lock.have_s()));

	n_unique = dict_index_get_n_unique(index);

	/* If the secondary index is unique, but one of the fields in the
	n_unique first fields is NULL, a unique key violation cannot occur,
	since we define NULL != NULL in this case */

	if (!index->nulls_equal) {
		for (ulint i = 0; i < n_unique; i++) {
			if (UNIV_SQL_NULL == dfield_get_len(
					dtuple_get_nth_field(entry, i))) {

				DBUG_RETURN(DB_SUCCESS);
			}
		}
	}

	/* Store old value on n_fields_cmp */

	n_fields_cmp = dtuple_get_n_fields_cmp(entry);

	dtuple_set_n_fields_cmp(entry, n_unique);

	btr_pcur_open(index, entry, PAGE_CUR_GE,
		      s_latch
		      ? BTR_SEARCH_LEAF_ALREADY_S_LATCHED
		      : BTR_SEARCH_LEAF,
		      &pcur, mtr);

	allow_duplicates = thr_get_trx(thr)->duplicates;

	/* Scan index records and check if there is a duplicate */

	do {
		const rec_t*		rec	= btr_pcur_get_rec(&pcur);
		const buf_block_t*	block	= btr_pcur_get_block(&pcur);
		const ulint		lock_type = LOCK_ORDINARY;

		if (page_rec_is_infimum(rec)) {

			continue;
		}

		offsets = rec_get_offsets(rec, index, offsets,
					  index->n_core_fields,
					  ULINT_UNDEFINED, &offsets_heap);

		if (flags & BTR_NO_LOCKING_FLAG) {
			/* Set no locks when applying log
			in online table rebuild. */
		} else if (allow_duplicates) {

			/* If the SQL-query will update or replace
			duplicate key we will take X-lock for
			duplicates ( REPLACE, LOAD DATAFILE REPLACE,
			INSERT ON DUPLICATE KEY UPDATE). */

			err = row_ins_set_exclusive_rec_lock(
				lock_type, block, rec, index, offsets, thr);
		} else {

			err = row_ins_set_shared_rec_lock(
				lock_type, block, rec, index, offsets, thr);
		}

		switch (err) {
		case DB_SUCCESS_LOCKED_REC:
			err = DB_SUCCESS;
		case DB_SUCCESS:
			break;
		default:
			goto end_scan;
		}

		if (page_rec_is_supremum(rec)) {

			continue;
		}

		cmp = cmp_dtuple_rec(entry, rec, offsets);

		if (cmp == 0) {
			if (row_ins_dupl_error_with_rec(rec, entry,
							index, offsets)) {
				err = DB_DUPLICATE_KEY;

				thr_get_trx(thr)->error_info = index;

				/* If the duplicate is on hidden FTS_DOC_ID,
				state so in the error log */
				if (index == index->table->fts_doc_id_index
				    && DICT_TF2_FLAG_IS_SET(
					index->table,
					DICT_TF2_FTS_HAS_DOC_ID)) {

					ib::error() << "Duplicate FTS_DOC_ID"
						" value on table "
						<< index->table->name;
				}

				goto end_scan;
			}
		} else {
			ut_a(cmp < 0);
			goto end_scan;
		}
	} while (btr_pcur_move_to_next(&pcur, mtr));

end_scan:
	/* Restore old value */
	dtuple_set_n_fields_cmp(entry, n_fields_cmp);

	DBUG_RETURN(err);
}

/** Checks for a duplicate when the table is being rebuilt online.
@retval DB_SUCCESS when no duplicate is detected
@retval DB_SUCCESS_LOCKED_REC when rec is an exact match of entry or
a newer version of entry (the entry should not be inserted)
@retval DB_DUPLICATE_KEY when entry is a duplicate of rec */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_duplicate_online(
/*=====================*/
	ulint		n_uniq,	/*!< in: offset of DB_TRX_ID */
	const dtuple_t*	entry,	/*!< in: entry that is being inserted */
	const rec_t*	rec,	/*!< in: clustered index record */
	rec_offs*	offsets)/*!< in/out: rec_get_offsets(rec) */
{
	ulint	fields	= 0;

	/* During rebuild, there should not be any delete-marked rows
	in the new table. */
	ut_ad(!rec_get_deleted_flag(rec, rec_offs_comp(offsets)));
	ut_ad(dtuple_get_n_fields_cmp(entry) == n_uniq);

	/* Compare the PRIMARY KEY fields and the
	DB_TRX_ID, DB_ROLL_PTR. */
	cmp_dtuple_rec_with_match_low(
		entry, rec, offsets, n_uniq + 2, &fields);

	if (fields < n_uniq) {
		/* Not a duplicate. */
		return(DB_SUCCESS);
	}

	ulint trx_id_len;

	if (fields == n_uniq + 2
	    && memcmp(rec_get_nth_field(rec, offsets, n_uniq, &trx_id_len),
		      reset_trx_id, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)) {
		ut_ad(trx_id_len == DATA_TRX_ID_LEN);
		/* rec is an exact match of entry, and DB_TRX_ID belongs
		to a transaction that started after our ALTER TABLE. */
		return(DB_SUCCESS_LOCKED_REC);
	}

	return(DB_DUPLICATE_KEY);
}

/** Checks for a duplicate when the table is being rebuilt online.
@retval DB_SUCCESS when no duplicate is detected
@retval DB_SUCCESS_LOCKED_REC when rec is an exact match of entry or
a newer version of entry (the entry should not be inserted)
@retval DB_DUPLICATE_KEY when entry is a duplicate of rec */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_duplicate_error_in_clust_online(
/*====================================*/
	ulint		n_uniq,	/*!< in: offset of DB_TRX_ID */
	const dtuple_t*	entry,	/*!< in: entry that is being inserted */
	const btr_cur_t*cursor,	/*!< in: cursor on insert position */
	rec_offs**	offsets,/*!< in/out: rec_get_offsets(rec) */
	mem_heap_t**	heap)	/*!< in/out: heap for offsets */
{
	dberr_t		err	= DB_SUCCESS;
	const rec_t*	rec	= btr_cur_get_rec(cursor);

	ut_ad(!cursor->index->is_instant());

	if (cursor->low_match >= n_uniq && !page_rec_is_infimum(rec)) {
		*offsets = rec_get_offsets(rec, cursor->index, *offsets,
					   cursor->index->n_fields,
					   ULINT_UNDEFINED, heap);
		err = row_ins_duplicate_online(n_uniq, entry, rec, *offsets);
		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	rec = page_rec_get_next_const(btr_cur_get_rec(cursor));

	if (cursor->up_match >= n_uniq && !page_rec_is_supremum(rec)) {
		*offsets = rec_get_offsets(rec, cursor->index, *offsets,
					   cursor->index->n_fields,
					   ULINT_UNDEFINED, heap);
		err = row_ins_duplicate_online(n_uniq, entry, rec, *offsets);
	}

	return(err);
}

/***************************************************************//**
Checks if a unique key violation error would occur at an index entry
insert. Sets shared locks on possible duplicate records. Works only
for a clustered index!
@retval DB_SUCCESS if no error
@retval DB_DUPLICATE_KEY if error,
@retval DB_LOCK_WAIT if we have to wait for a lock on a possible duplicate
record */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_duplicate_error_in_clust(
	ulint		flags,	/*!< in: undo logging and locking flags */
	btr_cur_t*	cursor,	/*!< in: B-tree cursor */
	const dtuple_t*	entry,	/*!< in: entry to insert */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t	err;
	rec_t*	rec;
	ulint	n_unique;
	trx_t*	trx		= thr_get_trx(thr);
	mem_heap_t*heap		= NULL;
	rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs* offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(dict_index_is_clust(cursor->index));

	/* NOTE: For unique non-clustered indexes there may be any number
	of delete marked records with the same value for the non-clustered
	index key (remember multiversioning), and which differ only in
	the row refererence part of the index record, containing the
	clustered index key fields. For such a secondary index record,
	to avoid race condition, we must FIRST do the insertion and after
	that check that the uniqueness condition is not breached! */

	/* NOTE: A problem is that in the B-tree node pointers on an
	upper level may match more to the entry than the actual existing
	user records on the leaf level. So, even if low_match would suggest
	that a duplicate key violation may occur, this may not be the case. */

	n_unique = dict_index_get_n_unique(cursor->index);

	if (cursor->low_match >= n_unique) {

		rec = btr_cur_get_rec(cursor);

		if (!page_rec_is_infimum(rec)) {
			offsets = rec_get_offsets(rec, cursor->index, offsets,
						  cursor->index->n_core_fields,
						  ULINT_UNDEFINED, &heap);

			/* We set a lock on the possible duplicate: this
			is needed in logical logging of MySQL to make
			sure that in roll-forward we get the same duplicate
			errors as in original execution */

			if (flags & BTR_NO_LOCKING_FLAG) {
				/* Do nothing if no-locking is set */
				err = DB_SUCCESS;
			} else if (trx->duplicates) {

				/* If the SQL-query will update or replace
				duplicate key we will take X-lock for
				duplicates ( REPLACE, LOAD DATAFILE REPLACE,
				INSERT ON DUPLICATE KEY UPDATE). */

				err = row_ins_set_exclusive_rec_lock(
					LOCK_REC_NOT_GAP,
					btr_cur_get_block(cursor),
					rec, cursor->index, offsets, thr);
			} else {

				err = row_ins_set_shared_rec_lock(
					LOCK_REC_NOT_GAP,
					btr_cur_get_block(cursor), rec,
					cursor->index, offsets, thr);
			}

			switch (err) {
			case DB_SUCCESS_LOCKED_REC:
			case DB_SUCCESS:
				break;
			default:
				goto func_exit;
			}

			if (row_ins_dupl_error_with_rec(
				    rec, entry, cursor->index, offsets)) {
duplicate:
				trx->error_info = cursor->index;
				err = DB_DUPLICATE_KEY;
				if (cursor->index->table->versioned()
				    && entry->vers_history_row())
				{
					ulint trx_id_len;
					byte *trx_id = rec_get_nth_field(
						rec, offsets, n_unique,
						&trx_id_len);
					ut_ad(trx_id_len == DATA_TRX_ID_LEN);
					if (trx->id == trx_read_trx_id(trx_id)) {
						err = DB_FOREIGN_DUPLICATE_KEY;
					}
				}
				goto func_exit;
			}
		}
	}

	if (cursor->up_match >= n_unique) {

		rec = page_rec_get_next(btr_cur_get_rec(cursor));

		if (!page_rec_is_supremum(rec)) {
			offsets = rec_get_offsets(rec, cursor->index, offsets,
						  cursor->index->n_core_fields,
						  ULINT_UNDEFINED, &heap);

			if (trx->duplicates) {

				/* If the SQL-query will update or replace
				duplicate key we will take X-lock for
				duplicates ( REPLACE, LOAD DATAFILE REPLACE,
				INSERT ON DUPLICATE KEY UPDATE). */

				err = row_ins_set_exclusive_rec_lock(
					LOCK_REC_NOT_GAP,
					btr_cur_get_block(cursor),
					rec, cursor->index, offsets, thr);
			} else {

				err = row_ins_set_shared_rec_lock(
					LOCK_REC_NOT_GAP,
					btr_cur_get_block(cursor),
					rec, cursor->index, offsets, thr);
			}

			switch (err) {
			case DB_SUCCESS_LOCKED_REC:
			case DB_SUCCESS:
				break;
			default:
				goto func_exit;
			}

			if (row_ins_dupl_error_with_rec(
				    rec, entry, cursor->index, offsets)) {
				goto duplicate;
			}
		}

		/* This should never happen */
		ut_error;
	}

	err = DB_SUCCESS;
func_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(err);
}

/***************************************************************//**
Checks if an index entry has long enough common prefix with an
existing record so that the intended insert of the entry must be
changed to a modify of the existing record. In the case of a clustered
index, the prefix must be n_unique fields long. In the case of a
secondary index, all fields must be equal.  InnoDB never updates
secondary index records in place, other than clearing or setting the
delete-mark flag. We could be able to update the non-unique fields
of a unique secondary index record by checking the cursor->up_match,
but we do not do so, because it could have some locking implications.
@return TRUE if the existing record should be updated; FALSE if not */
UNIV_INLINE
ibool
row_ins_must_modify_rec(
/*====================*/
	const btr_cur_t*	cursor)	/*!< in: B-tree cursor */
{
	/* NOTE: (compare to the note in row_ins_duplicate_error_in_clust)
	Because node pointers on upper levels of the B-tree may match more
	to entry than to actual user records on the leaf level, we
	have to check if the candidate record is actually a user record.
	A clustered index node pointer contains index->n_unique first fields,
	and a secondary index node pointer contains all index fields. */

	return(cursor->low_match
	       >= dict_index_get_n_unique_in_tree(cursor->index)
	       && !page_rec_is_infimum(btr_cur_get_rec(cursor)));
}

/** Insert the externally stored fields (off-page columns)
of a clustered index entry.
@param[in]	entry	index entry to insert
@param[in]	big_rec	externally stored fields
@param[in,out]	offsets	rec_get_offsets()
@param[in,out]	heap	memory heap
@param[in]	thd	client connection, or NULL
@param[in]	index	clustered index
@return	error code
@retval	DB_SUCCESS
@retval DB_OUT_OF_FILE_SPACE */
static
dberr_t
row_ins_index_entry_big_rec(
	const dtuple_t*		entry,
	const big_rec_t*	big_rec,
	rec_offs*		offsets,
	mem_heap_t**		heap,
	dict_index_t*		index,
	const void*		thd __attribute__((unused)))
{
	mtr_t		mtr;
	btr_pcur_t	pcur;
	rec_t*		rec;
	dberr_t		error;

	ut_ad(dict_index_is_clust(index));

	DEBUG_SYNC_C_IF_THD(thd, "before_row_ins_extern_latch");

	mtr.start();
	if (index->table->is_temporary()) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(mtr);
	}

	btr_pcur_open(index, entry, PAGE_CUR_LE, BTR_MODIFY_TREE,
		      &pcur, &mtr);
	rec = btr_pcur_get_rec(&pcur);
	offsets = rec_get_offsets(rec, index, offsets, index->n_core_fields,
				  ULINT_UNDEFINED, heap);

	DEBUG_SYNC_C_IF_THD(thd, "before_row_ins_extern");
	error = btr_store_big_rec_extern_fields(
		&pcur, offsets, big_rec, &mtr, BTR_STORE_INSERT);
	DEBUG_SYNC_C_IF_THD(thd, "after_row_ins_extern");

	if (error == DB_SUCCESS
	    && dict_index_is_online_ddl(index)) {
		row_log_table_insert(btr_pcur_get_rec(&pcur), index, offsets);
	}

	mtr.commit();

	btr_pcur_close(&pcur);

	return(error);
}

#ifdef HAVE_REPLICATION /* Working around MDEV-24622 */
extern "C" int thd_is_slave(const MYSQL_THD thd);
#else
# define thd_is_slave(thd) 0
#endif

#if defined __aarch64__&&defined __GNUC__&&__GNUC__==4&&!defined __clang__
/* Avoid GCC 4.8.5 internal compiler error due to srw_mutex::wr_unlock().
We would only need this for row_ins_clust_index_entry_low(),
but GCC 4.8.5 does not support pop_options. */
# pragma GCC optimize ("no-expensive-optimizations")
#endif

/***************************************************************//**
Tries to insert an entry into a clustered index, ignoring foreign key
constraints. If a record with the same unique key is found, the other
record is necessarily marked deleted by a committed transaction, or a
unique key violation error occurs. The delete marked record is then
updated to an existing record, and we must write an undo log record on
the delete marked record.
@retval DB_SUCCESS on success
@retval DB_LOCK_WAIT on lock wait when !(flags & BTR_NO_LOCKING_FLAG)
@retval DB_FAIL if retry with BTR_MODIFY_TREE is needed
@return error code */
dberr_t
row_ins_clust_index_entry_low(
/*==========================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether we wish optimistic or
				pessimistic descent down the index tree */
	dict_index_t*	index,	/*!< in: clustered index */
	ulint		n_uniq,	/*!< in: 0 or index->n_uniq */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	que_thr_t*	thr)	/*!< in: query thread */
{
	btr_pcur_t	pcur;
	btr_cur_t*	cursor;
	dberr_t		err		= DB_SUCCESS;
	big_rec_t*	big_rec		= NULL;
	mtr_t		mtr;
	ib_uint64_t	auto_inc	= 0;
	mem_heap_t*	offsets_heap	= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets         = offsets_;
	rec_offs_init(offsets_);
	trx_t*		trx	= thr_get_trx(thr);
	buf_block_t*	block;

	DBUG_ENTER("row_ins_clust_index_entry_low");

	ut_ad(dict_index_is_clust(index));
	ut_ad(!dict_index_is_unique(index)
	      || n_uniq == dict_index_get_n_unique(index));
	ut_ad(!n_uniq || n_uniq == dict_index_get_n_unique(index));
	ut_ad(!trx->in_rollback);

	mtr_start(&mtr);

	if (index->table->is_temporary()) {
		/* Disable REDO logging as the lifetime of temp-tables is
		limited to server or connection lifetime and so REDO
		information is not needed on restart for recovery.
		Disable locking as temp-tables are local to a connection. */

		ut_ad(flags & BTR_NO_LOCKING_FLAG);
		ut_ad(!dict_index_is_online_ddl(index));
		ut_ad(!index->table->persistent_autoinc);
		ut_ad(!index->is_instant());
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(mtr);

		if (UNIV_UNLIKELY(entry->is_metadata())) {
			ut_ad(index->is_instant());
			ut_ad(!dict_index_is_online_ddl(index));
			ut_ad(mode == BTR_MODIFY_TREE);
		} else {
			if (mode == BTR_MODIFY_LEAF
			    && dict_index_is_online_ddl(index)) {
				mode = BTR_MODIFY_LEAF_ALREADY_S_LATCHED;
				mtr_s_lock_index(index, &mtr);
			}

			if (unsigned ai = index->table->persistent_autoinc) {
				/* Prepare to persist the AUTO_INCREMENT value
				from the index entry to PAGE_ROOT_AUTO_INC. */
				const dfield_t* dfield = dtuple_get_nth_field(
					entry, ai - 1);
				if (!dfield_is_null(dfield)) {
					auto_inc = row_parse_int(
						static_cast<const byte*>(
							dfield->data),
						dfield->len,
						dfield->type.mtype,
						dfield->type.prtype
						& DATA_UNSIGNED);
				}
			}
		}
	}

	/* Note that we use PAGE_CUR_LE as the search mode, because then
	the function will return in both low_match and up_match of the
	cursor sensible values */
 	err = btr_pcur_open_low(index, 0, entry, PAGE_CUR_LE, mode, &pcur,
				auto_inc, &mtr);
	if (err != DB_SUCCESS) {
		index->table->file_unreadable = true;
commit_exit:
		mtr.commit();
		goto func_exit;
	}

	cursor = btr_pcur_get_btr_cur(&pcur);
	cursor->thr = thr;

#ifdef UNIV_DEBUG
	{
		page_t*	page = btr_cur_get_page(cursor);
		rec_t*	first_rec = page_rec_get_next(
			page_get_infimum_rec(page));

		ut_ad(page_rec_is_supremum(first_rec)
		      || rec_n_fields_is_sane(index, first_rec, entry));
	}
#endif /* UNIV_DEBUG */

	block = btr_cur_get_block(cursor);

	DBUG_EXECUTE_IF("row_ins_row_level", goto skip_bulk_insert;);

	if (!(flags & BTR_NO_UNDO_LOG_FLAG)
	    && page_is_empty(block->frame)
	    && !entry->is_metadata() && !trx->duplicates
	    && !trx->check_unique_secondary && !trx->check_foreigns
	    && !trx->dict_operation
	    && block->page.id().page_no() == index->page
	    && !index->table->skip_alter_undo
	    && !index->table->n_rec_locks
	    && !trx->is_wsrep() /* FIXME: MDEV-24623 */
	    && !thd_is_slave(trx->mysql_thd) /* FIXME: MDEV-24622 */) {
		DEBUG_SYNC_C("empty_root_page_insert");

		if (!index->table->is_temporary()) {
			err = lock_table(index->table, LOCK_X, thr);

			if (err != DB_SUCCESS) {
				trx->error_state = err;
				goto commit_exit;
			}

			if (index->table->n_rec_locks) {
				goto skip_bulk_insert;
			}

#ifdef BTR_CUR_HASH_ADAPT
			if (btr_search_enabled) {
				btr_search_x_lock_all();
				index->table->bulk_trx_id = trx->id;
				btr_search_x_unlock_all();
			} else {
				index->table->bulk_trx_id = trx->id;
			}
#else /* BTR_CUR_HASH_ADAPT */
			index->table->bulk_trx_id = trx->id;
#endif /* BTR_CUR_HASH_ADAPT */
		}

		trx->bulk_insert = true;
	}

skip_bulk_insert:
	if (UNIV_UNLIKELY(entry->info_bits != 0)) {
		ut_ad(entry->is_metadata());
		ut_ad(flags == BTR_NO_LOCKING_FLAG);
		ut_ad(index->is_instant());
		ut_ad(!dict_index_is_online_ddl(index));

		const rec_t* rec = btr_cur_get_rec(cursor);

		if (rec_get_info_bits(rec, page_rec_is_comp(rec))
		    & REC_INFO_MIN_REC_FLAG) {
			trx->error_info = index;
			err = DB_DUPLICATE_KEY;
			goto err_exit;
		}

		ut_ad(!row_ins_must_modify_rec(cursor));
		goto do_insert;
	}

	if (rec_is_metadata(btr_cur_get_rec(cursor), *index)) {
		goto do_insert;
	}

	if (n_uniq
	    && (cursor->up_match >= n_uniq || cursor->low_match >= n_uniq)) {

		if (flags
		    == (BTR_CREATE_FLAG | BTR_NO_LOCKING_FLAG
			| BTR_NO_UNDO_LOG_FLAG | BTR_KEEP_SYS_FLAG)) {
			/* Set no locks when applying log
			in online table rebuild. Only check for duplicates. */
			err = row_ins_duplicate_error_in_clust_online(
				n_uniq, entry, cursor,
				&offsets, &offsets_heap);

			switch (err) {
			case DB_SUCCESS:
				break;
			default:
				ut_ad(0);
				/* fall through */
			case DB_SUCCESS_LOCKED_REC:
			case DB_DUPLICATE_KEY:
				trx->error_info = cursor->index;
			}
		} else {
			/* Note that the following may return also
			DB_LOCK_WAIT */

			err = row_ins_duplicate_error_in_clust(
				flags, cursor, entry, thr);
		}

		if (err != DB_SUCCESS) {
err_exit:
			mtr_commit(&mtr);
			goto func_exit;
		}
	}

	/* Note: Allowing duplicates would qualify for modification of
	an existing record as the new entry is exactly same as old entry. */
	if (row_ins_must_modify_rec(cursor)) {
		/* There is already an index entry with a long enough common
		prefix, we must convert the insert into a modify of an
		existing record */
		mem_heap_t*	entry_heap	= mem_heap_create(1024);

		err = row_ins_clust_index_entry_by_modify(
			&pcur, flags, mode, &offsets, &offsets_heap,
			entry_heap, entry, thr, &mtr);

		if (err == DB_SUCCESS && dict_index_is_online_ddl(index)) {
			row_log_table_insert(btr_cur_get_rec(cursor),
					     index, offsets);
		}

		mtr_commit(&mtr);
		mem_heap_free(entry_heap);
	} else {
		if (index->is_instant()) entry->trim(*index);
do_insert:
		rec_t*	insert_rec;

		if (mode != BTR_MODIFY_TREE) {
			ut_ad((mode & ulint(~BTR_ALREADY_S_LATCHED))
			      == BTR_MODIFY_LEAF);
			err = btr_cur_optimistic_insert(
				flags, cursor, &offsets, &offsets_heap,
				entry, &insert_rec, &big_rec,
				n_ext, thr, &mtr);
		} else {
			if (buf_pool.running_out()) {
				err = DB_LOCK_TABLE_FULL;
				goto err_exit;
			}

			DEBUG_SYNC_C("before_insert_pessimitic_row_ins_clust");

			err = btr_cur_optimistic_insert(
				flags, cursor,
				&offsets, &offsets_heap,
				entry, &insert_rec, &big_rec,
				n_ext, thr, &mtr);

			if (err == DB_FAIL) {
				err = btr_cur_pessimistic_insert(
					flags, cursor,
					&offsets, &offsets_heap,
					entry, &insert_rec, &big_rec,
					n_ext, thr, &mtr);
			}
		}

		if (big_rec != NULL) {
			mtr_commit(&mtr);

			/* Online table rebuild could read (and
			ignore) the incomplete record at this point.
			If online rebuild is in progress, the
			row_ins_index_entry_big_rec() will write log. */

			DBUG_EXECUTE_IF(
				"row_ins_extern_checkpoint",
				log_write_up_to(mtr.commit_lsn(), true););
			err = row_ins_index_entry_big_rec(
				entry, big_rec, offsets, &offsets_heap, index,
				trx->mysql_thd);
			dtuple_convert_back_big_rec(index, entry, big_rec);
		} else {
			if (err == DB_SUCCESS
			    && dict_index_is_online_ddl(index)) {
				row_log_table_insert(
					insert_rec, index, offsets);
			}

			mtr_commit(&mtr);
		}
	}

func_exit:
	if (offsets_heap != NULL) {
		mem_heap_free(offsets_heap);
	}

	btr_pcur_close(&pcur);

	DBUG_RETURN(err);
}

/** Start a mini-transaction and check if the index will be dropped.
@param[in,out]	mtr		mini-transaction
@param[in,out]	index		secondary index
@param[in]	check		whether to check
@param[in]	search_mode	flags
@return true if the index is to be dropped */
static MY_ATTRIBUTE((warn_unused_result))
bool
row_ins_sec_mtr_start_and_check_if_aborted(
	mtr_t*		mtr,
	dict_index_t*	index,
	bool		check,
	ulint		search_mode)
{
	ut_ad(!dict_index_is_clust(index));
	ut_ad(mtr->is_named_space(index->table->space));

	const mtr_log_t	log_mode = mtr->get_log_mode();

	mtr->start();
	index->set_modified(*mtr);
	mtr->set_log_mode(log_mode);

	if (!check) {
		return(false);
	}

	if (search_mode & BTR_ALREADY_S_LATCHED) {
		mtr_s_lock_index(index, mtr);
	} else {
		mtr_sx_lock_index(index, mtr);
	}

	switch (index->online_status) {
	case ONLINE_INDEX_ABORTED:
	case ONLINE_INDEX_ABORTED_DROPPED:
		ut_ad(!index->is_committed());
		return(true);
	case ONLINE_INDEX_COMPLETE:
		return(false);
	case ONLINE_INDEX_CREATION:
		break;
	}

	ut_error;
	return(true);
}

/***************************************************************//**
Tries to insert an entry into a secondary index. If a record with exactly the
same fields is found, the other record is necessarily marked deleted.
It is then unmarked. Otherwise, the entry is just inserted to the index.
@retval DB_SUCCESS on success
@retval DB_LOCK_WAIT on lock wait when !(flags & BTR_NO_LOCKING_FLAG)
@retval DB_FAIL if retry with BTR_MODIFY_TREE is needed
@return error code */
dberr_t
row_ins_sec_index_entry_low(
/*========================*/
	ulint		flags,	/*!< in: undo logging and locking flags */
	ulint		mode,	/*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE,
				depending on whether we wish optimistic or
				pessimistic descent down the index tree */
	dict_index_t*	index,	/*!< in: secondary index */
	mem_heap_t*	offsets_heap,
				/*!< in/out: memory heap that can be emptied */
	mem_heap_t*	heap,	/*!< in/out: memory heap */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	trx_id_t	trx_id,	/*!< in: PAGE_MAX_TRX_ID during
				row_log_table_apply(), or 0 */
	que_thr_t*	thr)	/*!< in: query thread */
{
	DBUG_ENTER("row_ins_sec_index_entry_low");

	btr_cur_t	cursor;
	ulint		search_mode	= mode;
	dberr_t		err		= DB_SUCCESS;
	ulint		n_unique;
	mtr_t		mtr;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets         = offsets_;
	rec_offs_init(offsets_);
	rtr_info_t	rtr_info;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(mode == BTR_MODIFY_LEAF || mode == BTR_MODIFY_TREE);

	cursor.thr = thr;
	cursor.rtr_info = NULL;
	ut_ad(thr_get_trx(thr)->id != 0);

	mtr.start();

	if (index->table->is_temporary()) {
		/* Disable locking, because temporary tables are never
		shared between transactions or connections. */
		ut_ad(flags & BTR_NO_LOCKING_FLAG);
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	} else {
		index->set_modified(mtr);
		if (!dict_index_is_spatial(index)) {
			search_mode |= BTR_INSERT;
		}
	}

	/* Ensure that we acquire index->lock when inserting into an
	index with index->online_status == ONLINE_INDEX_COMPLETE, but
	could still be subject to rollback_inplace_alter_table().
	This prevents a concurrent change of index->online_status.
	The memory object cannot be freed as long as we have an open
	reference to the table, or index->table->n_ref_count > 0. */
	const bool	check = !index->is_committed();
	if (check) {
		DEBUG_SYNC_C("row_ins_sec_index_enter");
		if (mode == BTR_MODIFY_LEAF) {
			search_mode |= BTR_ALREADY_S_LATCHED;
			mtr_s_lock_index(index, &mtr);
		} else {
			mtr_sx_lock_index(index, &mtr);
		}

		if (row_log_online_op_try(
			    index, entry, thr_get_trx(thr)->id)) {
			goto func_exit;
		}
	}

	/* Note that we use PAGE_CUR_LE as the search mode, because then
	the function will return in both low_match and up_match of the
	cursor sensible values */

	if (!thr_get_trx(thr)->check_unique_secondary) {
		search_mode |= BTR_IGNORE_SEC_UNIQUE;
	}

	if (dict_index_is_spatial(index)) {
		cursor.index = index;
		rtr_init_rtr_info(&rtr_info, false, &cursor, index, false);
		rtr_info_update_btr(&cursor, &rtr_info);

		err = btr_cur_search_to_nth_level(
			index, 0, entry, PAGE_CUR_RTREE_INSERT,
			search_mode,
			&cursor, 0, &mtr);

		if (mode == BTR_MODIFY_LEAF && rtr_info.mbr_adj) {
			mtr_commit(&mtr);
			rtr_clean_rtr_info(&rtr_info, true);
			rtr_init_rtr_info(&rtr_info, false, &cursor,
					  index, false);
			rtr_info_update_btr(&cursor, &rtr_info);
			mtr_start(&mtr);
			index->set_modified(mtr);
			search_mode &= ulint(~BTR_MODIFY_LEAF);
			search_mode |= BTR_MODIFY_TREE;
			err = btr_cur_search_to_nth_level(
				index, 0, entry, PAGE_CUR_RTREE_INSERT,
				search_mode,
				&cursor, 0, &mtr);
			mode = BTR_MODIFY_TREE;
		}

		DBUG_EXECUTE_IF(
			"rtree_test_check_count", {
			goto func_exit;});

	} else {
		err = btr_cur_search_to_nth_level(
			index, 0, entry, PAGE_CUR_LE,
			search_mode,
			&cursor, 0, &mtr);
	}

	if (err != DB_SUCCESS) {
		if (err == DB_DECRYPTION_FAILED) {
			ib_push_warning(thr_get_trx(thr)->mysql_thd,
				DB_DECRYPTION_FAILED,
				"Table %s is encrypted but encryption service or"
				" used key_id is not available. "
				" Can't continue reading table.",
				index->table->name.m_name);
			index->table->file_unreadable = true;
		}
		goto func_exit;
	}

	if (cursor.flag == BTR_CUR_INSERT_TO_IBUF) {
		ut_ad(!dict_index_is_spatial(index));
		/* The insert was buffered during the search: we are done */
		goto func_exit;
	}

#ifdef UNIV_DEBUG
	{
		page_t*	page = btr_cur_get_page(&cursor);
		rec_t*	first_rec = page_rec_get_next(
			page_get_infimum_rec(page));

		ut_ad(page_rec_is_supremum(first_rec)
		      || rec_n_fields_is_sane(index, first_rec, entry));
	}
#endif /* UNIV_DEBUG */

	n_unique = dict_index_get_n_unique(index);

	if (dict_index_is_unique(index)
	    && (cursor.low_match >= n_unique || cursor.up_match >= n_unique)) {
		mtr_commit(&mtr);

		DEBUG_SYNC_C("row_ins_sec_index_unique");

		if (row_ins_sec_mtr_start_and_check_if_aborted(
			    &mtr, index, check, search_mode)) {
			goto func_exit;
		}

		err = row_ins_scan_sec_index_for_duplicate(
			flags, index, entry, thr, check, &mtr, offsets_heap);

		mtr_commit(&mtr);

		switch (err) {
		case DB_SUCCESS:
			break;
		case DB_DUPLICATE_KEY:
			if (!index->is_committed()) {
				ut_ad(!thr_get_trx(thr)
				      ->dict_operation_lock_mode);
				index->type |= DICT_CORRUPT;
				/* Do not return any error to the
				caller. The duplicate will be reported
				by ALTER TABLE or CREATE UNIQUE INDEX.
				Unfortunately we cannot report the
				duplicate key value to the DDL thread,
				because the altered_table object is
				private to its call stack. */
				err = DB_SUCCESS;
			}
			/* fall through */
		default:
			if (dict_index_is_spatial(index)) {
				rtr_clean_rtr_info(&rtr_info, true);
			}
			DBUG_RETURN(err);
		}

		if (row_ins_sec_mtr_start_and_check_if_aborted(
			    &mtr, index, check, search_mode)) {
			goto func_exit;
		}

		DEBUG_SYNC_C("row_ins_sec_index_entry_dup_locks_created");

		/* We did not find a duplicate and we have now
		locked with s-locks the necessary records to
		prevent any insertion of a duplicate by another
		transaction. Let us now reposition the cursor and
		continue the insertion. */
		btr_cur_search_to_nth_level(
			index, 0, entry, PAGE_CUR_LE,
			(search_mode
			 & ~(BTR_INSERT | BTR_IGNORE_SEC_UNIQUE)),
			&cursor, 0, &mtr);
	}

	if (row_ins_must_modify_rec(&cursor)) {
		/* There is already an index entry with a long enough common
		prefix, we must convert the insert into a modify of an
		existing record */
		offsets = rec_get_offsets(
			btr_cur_get_rec(&cursor), index, offsets,
			index->n_core_fields,
			ULINT_UNDEFINED, &offsets_heap);

		err = row_ins_sec_index_entry_by_modify(
			flags, mode, &cursor, &offsets,
			offsets_heap, heap, entry, thr, &mtr);

		if (err == DB_SUCCESS && dict_index_is_spatial(index)
		    && rtr_info.mbr_adj) {
			err = rtr_ins_enlarge_mbr(&cursor, &mtr);
		}
	} else {
		rec_t*		insert_rec;
		big_rec_t*	big_rec;

		if (mode == BTR_MODIFY_LEAF) {
			err = btr_cur_optimistic_insert(
				flags, &cursor, &offsets, &offsets_heap,
				entry, &insert_rec,
				&big_rec, 0, thr, &mtr);
			if (err == DB_SUCCESS
			    && dict_index_is_spatial(index)
			    && rtr_info.mbr_adj) {
				err = rtr_ins_enlarge_mbr(&cursor, &mtr);
			}
		} else {
			ut_ad(mode == BTR_MODIFY_TREE);
			if (buf_pool.running_out()) {
				err = DB_LOCK_TABLE_FULL;
				goto func_exit;
			}

			err = btr_cur_optimistic_insert(
				flags, &cursor,
				&offsets, &offsets_heap,
				entry, &insert_rec,
				&big_rec, 0, thr, &mtr);
			if (err == DB_FAIL) {
				err = btr_cur_pessimistic_insert(
					flags, &cursor,
					&offsets, &offsets_heap,
					entry, &insert_rec,
					&big_rec, 0, thr, &mtr);
			}
			if (err == DB_SUCCESS
				   && dict_index_is_spatial(index)
				   && rtr_info.mbr_adj) {
				err = rtr_ins_enlarge_mbr(&cursor, &mtr);
			}
		}

		if (err == DB_SUCCESS && trx_id) {
			page_update_max_trx_id(
				btr_cur_get_block(&cursor),
				btr_cur_get_page_zip(&cursor),
				trx_id, &mtr);
		}

		ut_ad(!big_rec);
	}

func_exit:
	if (dict_index_is_spatial(index)) {
		rtr_clean_rtr_info(&rtr_info, true);
	}

	mtr_commit(&mtr);
	DBUG_RETURN(err);
}

/***************************************************************//**
Inserts an entry into a clustered index. Tries first optimistic,
then pessimistic descent down the tree. If the entry matches enough
to a delete marked record, performs the insert by updating or delete
unmarking the delete marked record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_DUPLICATE_KEY, or some other error code */
dberr_t
row_ins_clust_index_entry(
/*======================*/
	dict_index_t*	index,	/*!< in: clustered index */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	que_thr_t*	thr,	/*!< in: query thread */
	ulint		n_ext)	/*!< in: number of externally stored columns */
{
	dberr_t	err;
	ulint	n_uniq;

	DBUG_ENTER("row_ins_clust_index_entry");

	if (!index->table->foreign_set.empty()) {
		err = row_ins_check_foreign_constraints(
			index->table, index, true, entry, thr);
		if (err != DB_SUCCESS) {

			DBUG_RETURN(err);
		}
	}

	n_uniq = dict_index_is_unique(index) ? index->n_uniq : 0;

#ifdef WITH_WSREP
	const bool skip_locking
		= wsrep_thd_skip_locking(thr_get_trx(thr)->mysql_thd);
	ulint	flags = index->table->no_rollback() ? BTR_NO_ROLLBACK
		: (index->table->is_temporary() || skip_locking)
		? BTR_NO_LOCKING_FLAG : 0;
#ifdef UNIV_DEBUG
	if (skip_locking && strcmp(wsrep_get_sr_table_name(),
                                   index->table->name.m_name)) {
		WSREP_ERROR("Record locking is disabled in this thread, "
			    "but the table being modified is not "
			    "`%s`: `%s`.", wsrep_get_sr_table_name(),
			    index->table->name.m_name);
		ut_error;
	}
#endif /* UNIV_DEBUG */
#else
	ulint	flags = index->table->no_rollback() ? BTR_NO_ROLLBACK
		: index->table->is_temporary()
		? BTR_NO_LOCKING_FLAG : 0;
#endif /* WITH_WSREP */
	const ulint	orig_n_fields = entry->n_fields;

	/* Try first optimistic descent to the B-tree */
	log_free_check();

	/* For intermediate table during copy alter table,
	   skip the undo log and record lock checking for
	   insertion operation.
	*/
	if (index->table->skip_alter_undo) {
		flags |= BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG;
	}

	/* Try first optimistic descent to the B-tree */
	log_free_check();

	err = row_ins_clust_index_entry_low(
		flags, BTR_MODIFY_LEAF, index, n_uniq, entry,
		n_ext, thr);

	entry->n_fields = orig_n_fields;

	DEBUG_SYNC_C_IF_THD(thr_get_trx(thr)->mysql_thd,
			    "after_row_ins_clust_index_entry_leaf");

	if (err != DB_FAIL) {
		DEBUG_SYNC_C("row_ins_clust_index_entry_leaf_after");
		DBUG_RETURN(err);
	}

	/* Try then pessimistic descent to the B-tree */
	log_free_check();

	err = row_ins_clust_index_entry_low(
		flags, BTR_MODIFY_TREE, index, n_uniq, entry,
		n_ext, thr);

	entry->n_fields = orig_n_fields;

	DBUG_RETURN(err);
}

/***************************************************************//**
Inserts an entry into a secondary index. Tries first optimistic,
then pessimistic descent down the tree. If the entry matches enough
to a delete marked record, performs the insert by updating or delete
unmarking the delete marked record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_DUPLICATE_KEY, or some other error code */
dberr_t
row_ins_sec_index_entry(
/*====================*/
	dict_index_t*	index,	/*!< in: secondary index */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	que_thr_t*	thr,	/*!< in: query thread */
	bool		check_foreign) /*!< in: true if check
				foreign table is needed, false otherwise */
{
	dberr_t		err;
	mem_heap_t*	offsets_heap;
	mem_heap_t*	heap;
	trx_id_t	trx_id  = 0;

	DBUG_EXECUTE_IF("row_ins_sec_index_entry_timeout", {
			DBUG_SET("-d,row_ins_sec_index_entry_timeout");
			return(DB_LOCK_WAIT);});

	if (check_foreign && !index->table->foreign_set.empty()) {
		err = row_ins_check_foreign_constraints(index->table, index,
							false, entry, thr);
		if (err != DB_SUCCESS) {

			return(err);
		}
	}

	ut_ad(thr_get_trx(thr)->id != 0);

	offsets_heap = mem_heap_create(1024);
	heap = mem_heap_create(1024);

	/* Try first optimistic descent to the B-tree */

	log_free_check();
	ulint flags = index->table->is_temporary()
		? BTR_NO_LOCKING_FLAG
		: 0;

	/* For intermediate table during copy alter table,
	   skip the undo log and record lock checking for
	   insertion operation.
	*/
	if (index->table->skip_alter_undo) {
		trx_id = thr_get_trx(thr)->id;
		flags |= BTR_NO_UNDO_LOG_FLAG | BTR_NO_LOCKING_FLAG;
	}

	err = row_ins_sec_index_entry_low(
		flags, BTR_MODIFY_LEAF, index, offsets_heap, heap, entry,
		trx_id, thr);
	if (err == DB_FAIL) {
		mem_heap_empty(heap);

		if (index->table->space == fil_system.sys_space
		    && !(index->type & (DICT_UNIQUE | DICT_SPATIAL))) {
			ibuf_free_excess_pages();
		}

		/* Try then pessimistic descent to the B-tree */
		log_free_check();

		err = row_ins_sec_index_entry_low(
			flags, BTR_MODIFY_TREE, index,
			offsets_heap, heap, entry, 0, thr);
	}

	mem_heap_free(heap);
	mem_heap_free(offsets_heap);
	return(err);
}

/***************************************************************//**
Inserts an index entry to index. Tries first optimistic, then pessimistic
descent down the tree. If the entry matches enough to a delete marked record,
performs the insert by updating or delete unmarking the delete marked
record.
@return DB_SUCCESS, DB_LOCK_WAIT, DB_DUPLICATE_KEY, or some other error code */
static
dberr_t
row_ins_index_entry(
/*================*/
	dict_index_t*	index,	/*!< in: index */
	dtuple_t*	entry,	/*!< in/out: index entry to insert */
	que_thr_t*	thr)	/*!< in: query thread */
{
	ut_ad(thr_get_trx(thr)->id || index->table->no_rollback()
	      || index->table->is_temporary());

	DBUG_EXECUTE_IF("row_ins_index_entry_timeout", {
			DBUG_SET("-d,row_ins_index_entry_timeout");
			return(DB_LOCK_WAIT);});

	if (index->is_primary()) {
		return row_ins_clust_index_entry(index, entry, thr, 0);
	} else {
		return row_ins_sec_index_entry(index, entry, thr);
	}
}


/*****************************************************************//**
This function generate MBR (Minimum Bounding Box) for spatial objects
and set it to spatial index field. */
static
void
row_ins_spatial_index_entry_set_mbr_field(
/*======================================*/
	dfield_t*	field,		/*!< in/out: mbr field */
	const dfield_t*	row_field)	/*!< in: row field */
{
	ulint		dlen = 0;
	double		mbr[SPDIMS * 2];

	/* This must be a GEOMETRY datatype */
	ut_ad(DATA_GEOMETRY_MTYPE(field->type.mtype));

	const byte* dptr = static_cast<const byte*>(
		dfield_get_data(row_field));
	dlen = dfield_get_len(row_field);

	/* obtain the MBR */
	rtree_mbr_from_wkb(dptr + GEO_DATA_HEADER_SIZE,
			   static_cast<uint>(dlen - GEO_DATA_HEADER_SIZE),
			   SPDIMS, mbr);

	/* Set mbr as index entry data */
	dfield_write_mbr(field, mbr);
}

/** Sets the values of the dtuple fields in entry from the values of appropriate
columns in row.
@param[in]	index	index handler
@param[out]	entry	index entry to make
@param[in]	row	row
@return DB_SUCCESS if the set is successful */
static
dberr_t
row_ins_index_entry_set_vals(
	const dict_index_t*	index,
	dtuple_t*		entry,
	const dtuple_t*		row)
{
	ulint	n_fields;
	ulint	i;
	ulint	num_v = dtuple_get_n_v_fields(entry);

	n_fields = dtuple_get_n_fields(entry);

	for (i = 0; i < n_fields + num_v; i++) {
		dict_field_t*	ind_field = NULL;
		dfield_t*	field;
		const dfield_t*	row_field;
		ulint		len;
		dict_col_t*	col;

		if (i >= n_fields) {
			/* This is virtual field */
			field = dtuple_get_nth_v_field(entry, i - n_fields);
			col = &dict_table_get_nth_v_col(
				index->table, i - n_fields)->m_col;
		} else {
			field = dtuple_get_nth_field(entry, i);
			ind_field = dict_index_get_nth_field(index, i);
			col = ind_field->col;
		}

		if (col->is_virtual()) {
			const dict_v_col_t*     v_col
				= reinterpret_cast<const dict_v_col_t*>(col);
			ut_ad(dtuple_get_n_fields(row)
			      == dict_table_get_n_cols(index->table));
			row_field = dtuple_get_nth_v_field(row, v_col->v_pos);
		} else if (col->is_dropped()) {
			ut_ad(index->is_primary());

			if (!(col->prtype & DATA_NOT_NULL)) {
				field->data = NULL;
				field->len = UNIV_SQL_NULL;
				field->type.prtype = DATA_BINARY_TYPE;
			} else {
				ut_ad(ind_field->fixed_len <= col->len);
				dfield_set_data(field, field_ref_zero,
						ind_field->fixed_len);
				field->type.prtype = DATA_NOT_NULL;
			}

			field->type.mtype = col->len
				? DATA_FIXBINARY : DATA_BINARY;
			continue;
		} else {
			row_field = dtuple_get_nth_field(
				row, ind_field->col->ind);
		}

		len = dfield_get_len(row_field);

		/* Check column prefix indexes */
		if (ind_field != NULL && ind_field->prefix_len > 0
		    && len != UNIV_SQL_NULL) {

			const	dict_col_t*	col
				= dict_field_get_col(ind_field);

			len = dtype_get_at_most_n_mbchars(
				col->prtype, col->mbminlen, col->mbmaxlen,
				ind_field->prefix_len,
				len,
				static_cast<const char*>(
					dfield_get_data(row_field)));

			ut_ad(!dfield_is_ext(row_field));
		}

		/* Handle spatial index. For the first field, replace
		the data with its MBR (Minimum Bounding Box). */
		if ((i == 0) && dict_index_is_spatial(index)) {
			if (!row_field->data
			    || row_field->len < GEO_DATA_HEADER_SIZE) {
				return(DB_CANT_CREATE_GEOMETRY_OBJECT);
			}
			row_ins_spatial_index_entry_set_mbr_field(
				field, row_field);
			continue;
		}

		dfield_set_data(field, dfield_get_data(row_field), len);
		if (dfield_is_ext(row_field)) {
			ut_ad(dict_index_is_clust(index));
			dfield_set_ext(field);
		}
	}

	return(DB_SUCCESS);
}

/***********************************************************//**
Inserts a single index entry to the table.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins_index_entry_step(
/*=====================*/
	ins_node_t*	node,	/*!< in: row insert node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	dberr_t	err;

	DBUG_ENTER("row_ins_index_entry_step");

	ut_ad(dtuple_check_typed(node->row));

	err = row_ins_index_entry_set_vals(node->index, *node->entry,
					   node->row);

	if (err != DB_SUCCESS) {
		DBUG_RETURN(err);
	}

	ut_ad(dtuple_check_typed(*node->entry));

	err = row_ins_index_entry(node->index, *node->entry, thr);

	DEBUG_SYNC_C_IF_THD(thr_get_trx(thr)->mysql_thd,
			    "after_row_ins_index_entry_step");

	DBUG_RETURN(err);
}

/***********************************************************//**
Allocates a row id for row and inits the node->index field. */
UNIV_INLINE
void
row_ins_alloc_row_id_step(
/*======================*/
	ins_node_t*	node)	/*!< in: row insert node */
{
  ut_ad(node->state == INS_NODE_ALLOC_ROW_ID);
  if (dict_table_get_first_index(node->table)->is_gen_clust())
    dict_sys_write_row_id(node->sys_buf, dict_sys.get_new_row_id());
}

/***********************************************************//**
Gets a row to insert from the values list. */
UNIV_INLINE
void
row_ins_get_row_from_values(
/*========================*/
	ins_node_t*	node)	/*!< in: row insert node */
{
	que_node_t*	list_node;
	dfield_t*	dfield;
	dtuple_t*	row;
	ulint		i;

	/* The field values are copied in the buffers of the select node and
	it is safe to use them until we fetch from select again: therefore
	we can just copy the pointers */

	row = node->row;

	i = 0;
	list_node = node->values_list;

	while (list_node) {
		eval_exp(list_node);

		dfield = dtuple_get_nth_field(row, i);
		dfield_copy_data(dfield, que_node_get_val(list_node));

		i++;
		list_node = que_node_get_next(list_node);
	}
}

/***********************************************************//**
Gets a row to insert from the select list. */
UNIV_INLINE
void
row_ins_get_row_from_select(
/*========================*/
	ins_node_t*	node)	/*!< in: row insert node */
{
	que_node_t*	list_node;
	dfield_t*	dfield;
	dtuple_t*	row;
	ulint		i;

	/* The field values are copied in the buffers of the select node and
	it is safe to use them until we fetch from select again: therefore
	we can just copy the pointers */

	row = node->row;

	i = 0;
	list_node = node->select->select_list;

	while (list_node) {
		dfield = dtuple_get_nth_field(row, i);
		dfield_copy_data(dfield, que_node_get_val(list_node));

		i++;
		list_node = que_node_get_next(list_node);
	}
}

inline
bool ins_node_t::vers_history_row() const
{
	if (!table->versioned())
		return false;
	dfield_t* row_end = dtuple_get_nth_field(row, table->vers_end);
	return row_end->vers_history_row();
}


/***********************************************************//**
Inserts a row to a table.
@return DB_SUCCESS if operation successfully completed, else error
code or DB_LOCK_WAIT */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
row_ins(
/*====*/
	ins_node_t*	node,	/*!< in: row insert node */
	que_thr_t*	thr)	/*!< in: query thread */
{
	DBUG_ENTER("row_ins");

	DBUG_PRINT("row_ins", ("table: %s", node->table->name.m_name));

	if (node->state == INS_NODE_ALLOC_ROW_ID) {

		row_ins_alloc_row_id_step(node);

		node->index = dict_table_get_first_index(node->table);
		ut_ad(node->entry_list.empty() == false);
		node->entry = node->entry_list.begin();

		if (node->ins_type == INS_SEARCHED) {

			row_ins_get_row_from_select(node);

		} else if (node->ins_type == INS_VALUES) {

			row_ins_get_row_from_values(node);
		}

		node->state = INS_NODE_INSERT_ENTRIES;
	}

	ut_ad(node->state == INS_NODE_INSERT_ENTRIES);

	while (node->index != NULL) {
		dict_index_t *index = node->index;
		/*
		   We do not insert history rows into FTS_DOC_ID_INDEX because
		   it is unique by FTS_DOC_ID only and we do not want to add
		   row_end to unique key. Fulltext field works the way new
		   FTS_DOC_ID is created on every fulltext UPDATE, so holding only
		   FTS_DOC_ID for history is enough.
		*/
		const unsigned type = index->type;
		if (index->type & DICT_FTS) {
		} else if (!(type & DICT_UNIQUE) || index->n_uniq > 1
			   || !node->vers_history_row()) {

			dberr_t err = row_ins_index_entry_step(node, thr);

			if (err != DB_SUCCESS) {
				DBUG_RETURN(err);
			}
		} else {
			/* Unique indexes with system versioning must contain
			the version end column. The only exception is a hidden
			FTS_DOC_ID_INDEX that InnoDB may create on a hidden or
			user-created FTS_DOC_ID column. */
			ut_ad(!strcmp(index->name, FTS_DOC_ID_INDEX_NAME));
			ut_ad(!strcmp(index->fields[0].name, FTS_DOC_ID_COL_NAME));
		}

		node->index = dict_table_get_next_index(node->index);
		++node->entry;

		/* Skip corrupted secondary index and its entry */
		while (node->index && node->index->is_corrupted()) {
			node->index = dict_table_get_next_index(node->index);
			++node->entry;
		}
	}

	ut_ad(node->entry == node->entry_list.end());

	node->state = INS_NODE_ALLOC_ROW_ID;

	DBUG_RETURN(DB_SUCCESS);
}

/***********************************************************//**
Inserts a row to a table. This is a high-level function used in SQL execution
graphs.
@return query thread to run next or NULL */
que_thr_t*
row_ins_step(
/*=========*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	ins_node_t*	node;
	que_node_t*	parent;
	sel_node_t*	sel_node;
	trx_t*		trx;
	dberr_t		err;

	ut_ad(thr);

	DEBUG_SYNC_C("innodb_row_ins_step_enter");

	trx = thr_get_trx(thr);

	node = static_cast<ins_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_INSERT);

	parent = que_node_get_parent(node);
	sel_node = node->select;

	if (thr->prev_node == parent) {
		node->state = INS_NODE_SET_IX_LOCK;
	}

	/* If this is the first time this node is executed (or when
	execution resumes after wait for the table IX lock), set an
	IX lock on the table and reset the possible select node. MySQL's
	partitioned table code may also call an insert within the same
	SQL statement AFTER it has used this table handle to do a search.
	This happens, for example, when a row update moves it to another
	partition. In that case, we have already set the IX lock on the
	table during the search operation, and there is no need to set
	it again here. But we must write trx->id to node->sys_buf. */

	if (node->table->no_rollback()) {
		/* No-rollback tables should only be written to by a
		single thread at a time, but there can be multiple
		concurrent readers. We must hold an open table handle. */
		DBUG_ASSERT(node->table->get_ref_count() > 0);
		DBUG_ASSERT(node->ins_type == INS_DIRECT);
		/* No-rollback tables can consist only of a single index. */
		DBUG_ASSERT(node->entry_list.size() == 1);
		DBUG_ASSERT(UT_LIST_GET_LEN(node->table->indexes) == 1);
		/* There should be no possibility for interruption and
		restarting here. In theory, we could allow resumption
		from the INS_NODE_INSERT_ENTRIES state here. */
		DBUG_ASSERT(node->state == INS_NODE_SET_IX_LOCK);
		node->index = dict_table_get_first_index(node->table);
		node->entry = node->entry_list.begin();
		node->state = INS_NODE_INSERT_ENTRIES;
		goto do_insert;
	}

	if (node->state == INS_NODE_SET_IX_LOCK) {

		node->state = INS_NODE_ALLOC_ROW_ID;

		if (node->table->is_temporary()) {
			node->trx_id = trx->id;
		}

		/* It may be that the current session has not yet started
		its transaction, or it has been committed: */

		if (trx->id == node->trx_id) {
			/* No need to do IX-locking */

			goto same_trx;
		}

		err = lock_table(node->table, LOCK_IX, thr);

		DBUG_EXECUTE_IF("ib_row_ins_ix_lock_wait",
				err = DB_LOCK_WAIT;);

		if (err != DB_SUCCESS) {
			node->state = INS_NODE_SET_IX_LOCK;
			goto error_handling;
		}

		node->trx_id = trx->id;
same_trx:
		if (node->ins_type == INS_SEARCHED) {
			/* Reset the cursor */
			sel_node->state = SEL_NODE_OPEN;

			/* Fetch a row to insert */

			thr->run_node = sel_node;

			return(thr);
		}
	}

	if ((node->ins_type == INS_SEARCHED)
	    && (sel_node->state != SEL_NODE_FETCH)) {

		ut_ad(sel_node->state == SEL_NODE_NO_MORE_ROWS);

		/* No more rows to insert */
		thr->run_node = parent;

		return(thr);
	}
do_insert:
	/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */

	err = row_ins(node, thr);

error_handling:
	trx->error_state = err;

	if (err != DB_SUCCESS) {
		/* err == DB_LOCK_WAIT or SQL error detected */
		return(NULL);
	}

	/* DO THE TRIGGER ACTIONS HERE */

	if (node->ins_type == INS_SEARCHED) {
		/* Fetch a row to insert */

		thr->run_node = sel_node;
	} else {
		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}
