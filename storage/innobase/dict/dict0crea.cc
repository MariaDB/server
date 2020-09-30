/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file dict/dict0crea.cc
Database object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#include "dict0crea.h"
#include "btr0pcur.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "btr0sea.h"
#endif /* BTR_CUR_HASH_ADAPT */
#include "page0page.h"
#include "mach0data.h"
#include "dict0boot.h"
#include "dict0dict.h"
#include "que0que.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "pars0pars.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "ut0vec.h"
#include "dict0priv.h"
#include "fts0priv.h"
#include "srv0start.h"

/*****************************************************************//**
Based on a table object, this function builds the entry to be inserted
in the SYS_TABLES system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_tables_tuple(
/*=========================*/
	const dict_table_t*	table,	/*!< in: table */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dtuple_t*	entry;
	dfield_t*	dfield;
	byte*		ptr;
	ulint		type;

	ut_ad(table);
	ut_ad(!table->space || table->space->id == table->space_id);
	ut_ad(heap);
	ut_ad(table->n_cols >= DATA_N_SYS_COLS);

	entry = dtuple_create(heap, 8 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, dict_sys.sys_tables);

	/* 0: NAME -----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__NAME);

	dfield_set_data(dfield,
			table->name.m_name, strlen(table->name.m_name));

	/* 1: DB_TRX_ID added later */
	/* 2: DB_ROLL_PTR added later */
	/* 3: ID -------------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 4: N_COLS ---------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__N_COLS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));

	/* If there is any virtual column, encode it in N_COLS */
	mach_write_to_4(ptr, dict_table_encode_n_col(
				ulint(table->n_cols - DATA_N_SYS_COLS),
				ulint(table->n_v_def))
			| (ulint(table->flags & DICT_TF_COMPACT) << 31));
	dfield_set_data(dfield, ptr, 4);

	/* 5: TYPE (table flags) -----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__TYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));

	/* Validate the table flags and convert them to what is saved in
	SYS_TABLES.TYPE.  Table flag values 0 and 1 are both written to
	SYS_TABLES.TYPE as 1. */
	type = dict_tf_to_sys_tables_type(table->flags);
	mach_write_to_4(ptr, type);

	dfield_set_data(dfield, ptr, 4);

	/* 6: MIX_ID (obsolete) ---------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__MIX_ID);

	ptr = static_cast<byte*>(mem_heap_zalloc(heap, 8));

	dfield_set_data(dfield, ptr, 8);

	/* 7: MIX_LEN (additional flags) --------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__MIX_LEN);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	/* Be sure all non-used bits are zero. */
	ut_a(!(table->flags2 & DICT_TF2_UNUSED_BIT_MASK));
	mach_write_to_4(ptr, table->flags2);

	dfield_set_data(dfield, ptr, 4);

	/* 8: CLUSTER_NAME ---------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__CLUSTER_ID);
	dfield_set_null(dfield); /* not supported */

	/* 9: SPACE ----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__SPACE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, table->space_id);

	dfield_set_data(dfield, ptr, 4);
	/*----------------------------------*/

	return(entry);
}

/*****************************************************************//**
Based on a table object, this function builds the entry to be inserted
in the SYS_COLUMNS system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_columns_tuple(
/*==========================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			i,	/*!< in: column number */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dtuple_t*		entry;
	const dict_col_t*	column;
	dfield_t*		dfield;
	byte*			ptr;
	const char*		col_name;
	ulint			num_base = 0;
	ulint			v_col_no = ULINT_UNDEFINED;

	ut_ad(table);
	ut_ad(heap);

	/* Any column beyond table->n_def would be virtual columns */
        if (i >= table->n_def) {
		dict_v_col_t*	v_col = dict_table_get_nth_v_col(
					table, i - table->n_def);
		column = &v_col->m_col;
		num_base = v_col->num_base;
		v_col_no = column->ind;
	} else {
		column = dict_table_get_nth_col(table, i);
		ut_ad(!column->is_virtual());
	}

	entry = dtuple_create(heap, 7 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, dict_sys.sys_columns);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__TABLE_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: POS ----------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__POS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));

	if (v_col_no != ULINT_UNDEFINED) {
		/* encode virtual column's position in MySQL table and InnoDB
		table in "POS" */
		mach_write_to_4(ptr, dict_create_v_col_pos(
				i - table->n_def, v_col_no));
	} else {
		mach_write_to_4(ptr, i);
	}

	dfield_set_data(dfield, ptr, 4);

	/* 2: DB_TRX_ID added later */
	/* 3: DB_ROLL_PTR added later */
	/* 4: NAME ---------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__NAME);

        if (i >= table->n_def) {
		col_name = dict_table_get_v_col_name(table, i - table->n_def);
	} else {
		col_name = dict_table_get_col_name(table, i);
	}

	dfield_set_data(dfield, col_name, strlen(col_name));

	/* 5: MTYPE --------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__MTYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, column->mtype);

	dfield_set_data(dfield, ptr, 4);

	/* 6: PRTYPE -------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__PRTYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, column->prtype);

	dfield_set_data(dfield, ptr, 4);

	/* 7: LEN ----------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__LEN);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, column->len);

	dfield_set_data(dfield, ptr, 4);

	/* 8: PREC ---------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__PREC);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, num_base);

	dfield_set_data(dfield, ptr, 4);
	/*---------------------------------*/

	return(entry);
}

/** Based on a table object, this function builds the entry to be inserted
in the SYS_VIRTUAL system table. Each row maps a virtual column to one of
its base column.
@param[in]	table	table
@param[in]	v_col_n	virtual column number
@param[in]	b_col_n	base column sequence num
@param[in]	heap	memory heap
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_virtual_tuple(
	const dict_table_t*	table,
	ulint			v_col_n,
	ulint			b_col_n,
	mem_heap_t*		heap)
{
	dtuple_t*		entry;
	const dict_col_t*	base_column;
	dfield_t*		dfield;
	byte*			ptr;

	ut_ad(table);
	ut_ad(heap);

	ut_ad(v_col_n < table->n_v_def);
	dict_v_col_t*	v_col = dict_table_get_nth_v_col(table, v_col_n);
	base_column = v_col->base_col[b_col_n];

	entry = dtuple_create(heap, DICT_NUM_COLS__SYS_VIRTUAL
			      + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, dict_sys.sys_virtual);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_VIRTUAL__TABLE_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: POS ---------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_VIRTUAL__POS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	ulint	v_col_no = dict_create_v_col_pos(v_col_n, v_col->m_col.ind);
	mach_write_to_4(ptr, v_col_no);

	dfield_set_data(dfield, ptr, 4);

	/* 2: BASE_POS ----------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_VIRTUAL__BASE_POS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, base_column->ind);

	dfield_set_data(dfield, ptr, 4);

	/* 3: DB_TRX_ID added later */
	/* 4: DB_ROLL_PTR added later */

	/*---------------------------------*/
	return(entry);
}

/***************************************************************//**
Builds a table definition to insert.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
dict_build_table_def_step(
/*======================*/
	que_thr_t*	thr,	/*!< in: query thread */
	tab_node_t*	node)	/*!< in: table create node */
{
	dict_sys.assert_locked();
	dict_table_t*	table = node->table;
	trx_t* trx = thr_get_trx(thr);
	ut_ad(!table->is_temporary());
	ut_ad(!table->space);
	ut_ad(table->space_id == ULINT_UNDEFINED);
	dict_hdr_get_new_id(&table->id, NULL, NULL);
	trx->table_id = table->id;

	/* Always set this bit for all new created tables */
	DICT_TF2_FLAG_SET(table, DICT_TF2_FTS_AUX_HEX_NAME);
	DBUG_EXECUTE_IF("innodb_test_wrong_fts_aux_table_name",
			DICT_TF2_FLAG_UNSET(table,
					    DICT_TF2_FTS_AUX_HEX_NAME););

	if (DICT_TF2_FLAG_IS_SET(table, DICT_TF2_USE_FILE_PER_TABLE)) {
		/* This table will need a new tablespace. */

		ut_ad(DICT_TF_GET_ZIP_SSIZE(table->flags) == 0
		      || dict_table_has_atomic_blobs(table));
		mtr_t mtr;
		trx_undo_t* undo = trx->rsegs.m_redo.undo;
		if (undo && !undo->table_id
		    && trx_get_dict_operation(trx) == TRX_DICT_OP_TABLE) {
			/* This must be a TRUNCATE operation where
			the empty table is created after the old table
			was renamed. Be sure to mark the transaction
			associated with the new empty table, so that
			we can remove it on recovery. */
			mtr.start();
			undo->table_id = trx->table_id;
			undo->dict_operation = TRUE;
			buf_block_t* block = trx_undo_page_get(
				page_id_t(trx->rsegs.m_redo.rseg->space->id,
					  undo->hdr_page_no),
				&mtr);
			mtr.write<1,mtr_t::MAYBE_NOP>(
				*block,
				block->frame + undo->hdr_offset
				+ TRX_UNDO_DICT_TRANS, 1U);
			mtr.write<8,mtr_t::MAYBE_NOP>(
				*block,
				block->frame + undo->hdr_offset
				+ TRX_UNDO_TABLE_ID, trx->table_id);
			mtr.commit();
			log_write_up_to(mtr.commit_lsn(), true);
		}
		/* Get a new tablespace ID */
		ulint space_id;
		dict_hdr_get_new_id(NULL, NULL, &space_id);

		DBUG_EXECUTE_IF(
			"ib_create_table_fail_out_of_space_ids",
			space_id = ULINT_UNDEFINED;
		);

		if (space_id == ULINT_UNDEFINED) {
			return DB_ERROR;
		}

		/* Determine the tablespace flags. */
		bool	has_data_dir = DICT_TF_HAS_DATA_DIR(table->flags);
		ulint	fsp_flags = dict_tf_to_fsp_flags(table->flags);
		ut_ad(!has_data_dir || table->data_dir_path);
		char*	filepath = has_data_dir
			? fil_make_filepath(table->data_dir_path,
					    table->name.m_name, IBD, true)
			: fil_make_filepath(NULL,
					    table->name.m_name, IBD, false);

		/* We create a new single-table tablespace for the table.
		We initially let it be 4 pages:
		- page 0 is the fsp header and an extent descriptor page,
		- page 1 is an ibuf bitmap page,
		- page 2 is the first inode page,
		- page 3 will contain the root of the clustered index of
		the table we create here. */

		dberr_t err;
		table->space = fil_ibd_create(
			space_id, table->name.m_name, filepath, fsp_flags,
			FIL_IBD_FILE_INITIAL_SIZE,
			node->mode, node->key_id, &err);

		ut_free(filepath);

		if (!table->space) {
			ut_ad(err != DB_SUCCESS);
			return err;
		}

		table->space_id = space_id;
		mtr.start();
		mtr.set_named_space(table->space);
		fsp_header_init(table->space, FIL_IBD_FILE_INITIAL_SIZE, &mtr);
		mtr.commit();
	} else {
		ut_ad(dict_tf_get_rec_format(table->flags)
		      != REC_FORMAT_COMPRESSED);
		table->space = fil_system.sys_space;
		table->space_id = TRX_SYS_SPACE;
	}

	ins_node_set_new_row(node->tab_def,
			     dict_create_sys_tables_tuple(table, node->heap));
	return DB_SUCCESS;
}

/** Builds a SYS_VIRTUAL row definition to insert.
@param[in]	node	table create node */
static
void
dict_build_v_col_def_step(
	tab_node_t*	node)
{
	dtuple_t*	row;

	row = dict_create_sys_virtual_tuple(node->table, node->col_no,
					    node->base_col_no,
					    node->heap);
	ins_node_set_new_row(node->v_col_def, row);
}

/*****************************************************************//**
Based on an index object, this function builds the entry to be inserted
in the SYS_INDEXES system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_indexes_tuple(
/*==========================*/
	const dict_index_t*	index,	/*!< in: index */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dtuple_t*	entry;
	dfield_t*	dfield;
	byte*		ptr;

	dict_sys.assert_locked();
	ut_ad(index);
	ut_ad(index->table->space || index->table->file_unreadable);
	ut_ad(!index->table->space
	      || index->table->space->id == index->table->space_id);
	ut_ad(heap);

	entry = dtuple_create(
		heap, DICT_NUM_COLS__SYS_INDEXES + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, dict_sys.sys_indexes);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__TABLE_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, index->table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: ID ----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, index->id);

	dfield_set_data(dfield, ptr, 8);

	/* 2: DB_TRX_ID added later */
	/* 3: DB_ROLL_PTR added later */
	/* 4: NAME --------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__NAME);

	if (!index->is_committed()) {
		ulint	len	= strlen(index->name) + 1;
		char*	name	= static_cast<char*>(
			mem_heap_alloc(heap, len));
		*name = *TEMP_INDEX_PREFIX_STR;
		memcpy(name + 1, index->name, len - 1);
		dfield_set_data(dfield, name, len);
	} else {
		dfield_set_data(dfield, index->name, strlen(index->name));
	}

	/* 5: N_FIELDS ----------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__N_FIELDS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, index->n_fields);

	dfield_set_data(dfield, ptr, 4);

	/* 6: TYPE --------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__TYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, index->type);

	dfield_set_data(dfield, ptr, 4);

	/* 7: SPACE --------------------------*/

	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__SPACE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, index->table->space_id);

	dfield_set_data(dfield, ptr, 4);

	/* 8: PAGE_NO --------------------------*/

	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__PAGE_NO);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, FIL_NULL);

	dfield_set_data(dfield, ptr, 4);

	/* 9: MERGE_THRESHOLD ----------------*/

	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__MERGE_THRESHOLD);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, DICT_INDEX_MERGE_THRESHOLD_DEFAULT);

	dfield_set_data(dfield, ptr, 4);

	/*--------------------------------*/

	return(entry);
}

/*****************************************************************//**
Based on an index object, this function builds the entry to be inserted
in the SYS_FIELDS system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_fields_tuple(
/*=========================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			fld_no,	/*!< in: field number */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dtuple_t*	entry;
	dict_field_t*	field;
	dfield_t*	dfield;
	byte*		ptr;
	ibool		index_contains_column_prefix_field	= FALSE;
	ulint		j;

	ut_ad(index);
	ut_ad(heap);

	for (j = 0; j < index->n_fields; j++) {
		if (dict_index_get_nth_field(index, j)->prefix_len > 0) {
			index_contains_column_prefix_field = TRUE;
			break;
		}
	}

	field = dict_index_get_nth_field(index, fld_no);

	entry = dtuple_create(heap, 3 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, dict_sys.sys_fields);

	/* 0: INDEX_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_FIELDS__INDEX_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, index->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: POS; FIELD NUMBER & PREFIX LENGTH -----------------------*/

	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_FIELDS__POS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));

	if (index_contains_column_prefix_field) {
		/* If there are column prefix fields in the index, then
		we store the number of the field to the 2 HIGH bytes
		and the prefix length to the 2 low bytes, */

		mach_write_to_4(ptr, (fld_no << 16) + field->prefix_len);
	} else {
		/* Else we store the number of the field to the 2 LOW bytes.
		This is to keep the storage format compatible with
		InnoDB versions < 4.0.14. */

		mach_write_to_4(ptr, fld_no);
	}

	dfield_set_data(dfield, ptr, 4);

	/* 2: DB_TRX_ID added later */
	/* 3: DB_ROLL_PTR added later */
	/* 4: COL_NAME -------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_FIELDS__COL_NAME);

	dfield_set_data(dfield, field->name, strlen(field->name));
	/*---------------------------------*/

	return(entry);
}

/*****************************************************************//**
Creates the tuple with which the index entry is searched for writing the index
tree root page number, if such a tree is created.
@return the tuple for search */
static
dtuple_t*
dict_create_search_tuple(
/*=====================*/
	const dtuple_t*	tuple,	/*!< in: the tuple inserted in the SYS_INDEXES
				table */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory for
				the built tuple is allocated */
{
	dtuple_t*	search_tuple;
	const dfield_t*	field1;
	dfield_t*	field2;

	ut_ad(tuple && heap);

	search_tuple = dtuple_create(heap, 2);

	field1 = dtuple_get_nth_field(tuple, 0);
	field2 = dtuple_get_nth_field(search_tuple, 0);

	dfield_copy(field2, field1);

	field1 = dtuple_get_nth_field(tuple, 1);
	field2 = dtuple_get_nth_field(search_tuple, 1);

	dfield_copy(field2, field1);

	ut_ad(dtuple_validate(search_tuple));

	return(search_tuple);
}

/***************************************************************//**
Builds an index definition row to insert.
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
dict_build_index_def_step(
/*======================*/
	que_thr_t*	thr,	/*!< in: query thread */
	ind_node_t*	node)	/*!< in: index create node */
{
	dict_table_t*	table;
	dict_index_t*	index;
	dtuple_t*	row;
	trx_t*		trx;

	dict_sys.assert_locked();

	trx = thr_get_trx(thr);

	index = node->index;

	table = index->table = node->table = dict_table_open_on_name(
		node->table_name, TRUE, FALSE, DICT_ERR_IGNORE_NONE);

	if (table == NULL) {
		return(DB_TABLE_NOT_FOUND);
	}

	if (!trx->table_id) {
		/* Record only the first table id. */
		trx->table_id = table->id;
	}

	ut_ad((UT_LIST_GET_LEN(table->indexes) > 0)
	      || dict_index_is_clust(index));

	dict_hdr_get_new_id(NULL, &index->id, NULL);

	/* Inherit the space id from the table; we store all indexes of a
	table in the same tablespace */

	node->page_no = FIL_NULL;
	row = dict_create_sys_indexes_tuple(index, node->heap);
	node->ind_row = row;

	ins_node_set_new_row(node->ind_def, row);

	/* Note that the index was created by this transaction. */
	index->trx_id = trx->id;
	ut_ad(table->def_trx_id <= trx->id);
	table->def_trx_id = trx->id;
	dict_table_close(table, true, false);

	return(DB_SUCCESS);
}

/***************************************************************//**
Builds an index definition without updating SYSTEM TABLES.
@return DB_SUCCESS or error code */
void
dict_build_index_def(
/*=================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index,	/*!< in/out: index */
	trx_t*			trx)	/*!< in/out: InnoDB transaction handle */
{
	dict_sys.assert_locked();

	if (trx->table_id == 0) {
		/* Record only the first table id. */
		trx->table_id = table->id;
	}

	ut_ad((UT_LIST_GET_LEN(table->indexes) > 0)
	      || dict_index_is_clust(index));

	dict_hdr_get_new_id(NULL, &index->id, NULL);

	/* Note that the index was created by this transaction. */
	index->trx_id = trx->id;
}

/***************************************************************//**
Builds a field definition row to insert. */
static
void
dict_build_field_def_step(
/*======================*/
	ind_node_t*	node)	/*!< in: index create node */
{
	dict_index_t*	index;
	dtuple_t*	row;

	index = node->index;

	row = dict_create_sys_fields_tuple(index, node->field_no, node->heap);

	ins_node_set_new_row(node->field_def, row);
}

/***************************************************************//**
Creates an index tree for the index if it is not a member of a cluster.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
dict_create_index_tree_step(
/*========================*/
	ind_node_t*	node)	/*!< in: index create node */
{
	mtr_t		mtr;
	btr_pcur_t	pcur;
	dict_index_t*	index;
	dtuple_t*	search_tuple;

	dict_sys.assert_locked();

	index = node->index;

	if (index->type == DICT_FTS) {
		/* FTS index does not need an index tree */
		return(DB_SUCCESS);
	}

	/* Run a mini-transaction in which the index tree is allocated for
	the index and its root address is written to the index entry in
	sys_indexes */

	mtr.start();

	search_tuple = dict_create_search_tuple(node->ind_row, node->heap);

	btr_pcur_open(UT_LIST_GET_FIRST(dict_sys.sys_indexes->indexes),
		      search_tuple, PAGE_CUR_L, BTR_MODIFY_LEAF,
		      &pcur, &mtr);

	btr_pcur_move_to_next_user_rec(&pcur, &mtr);


	dberr_t		err = DB_SUCCESS;

	if (!index->is_readable()) {
		node->page_no = FIL_NULL;
	} else {
		index->set_modified(mtr);

		node->page_no = btr_create(
			index->type, index->table->space,
			index->id, index, &mtr);

		if (node->page_no == FIL_NULL) {
			err = DB_OUT_OF_FILE_SPACE;
		}

		DBUG_EXECUTE_IF("ib_import_create_index_failure_1",
				node->page_no = FIL_NULL;
				err = DB_OUT_OF_FILE_SPACE; );
	}

	ulint	len;
	byte*	data = rec_get_nth_field_old(btr_pcur_get_rec(&pcur),
					     DICT_FLD__SYS_INDEXES__PAGE_NO,
					     &len);
	ut_ad(len == 4);
	mtr.write<4,mtr_t::MAYBE_NOP>(*btr_pcur_get_block(&pcur), data,
				      node->page_no);

	mtr.commit();

	return(err);
}

/***************************************************************//**
Creates an index tree for the index if it is not a member of a cluster.
Don't update SYSTEM TABLES.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
dberr_t
dict_create_index_tree_in_mem(
/*==========================*/
	dict_index_t*	index,	/*!< in/out: index */
	const trx_t*	trx)	/*!< in: InnoDB transaction handle */
{
	mtr_t		mtr;

	dict_sys.assert_locked();
	ut_ad(!(index->type & DICT_FTS));

	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	/* Currently this function is being used by temp-tables only.
	Import/Discard of temp-table is blocked and so this assert. */
	ut_ad(index->is_readable());
	ut_ad(!(index->table->flags2 & DICT_TF2_DISCARDED));

	index->page = btr_create(index->type, index->table->space,
				 index->id, index, &mtr);
	mtr_commit(&mtr);

	index->trx_id = trx->id;

	return index->page == FIL_NULL ? DB_OUT_OF_FILE_SPACE : DB_SUCCESS;
}

/** Drop the index tree associated with a row in SYS_INDEXES table.
@param[in,out]	pcur	persistent cursor on rec
@param[in,out]	trx	dictionary transaction
@param[in,out]	mtr	mini-transaction */
void dict_drop_index_tree(btr_pcur_t* pcur, trx_t* trx, mtr_t* mtr)
{
	rec_t*	rec = btr_pcur_get_rec(pcur);
	byte*	ptr;
	ulint	len;

	dict_sys.assert_locked();
	ut_a(!dict_table_is_comp(dict_sys.sys_indexes));

	ptr = rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);

	ut_ad(len == 4);

	btr_pcur_store_position(pcur, mtr);

	const uint32_t root_page_no = mach_read_from_4(ptr);

	if (root_page_no == FIL_NULL) {
		/* The tree has already been freed */
		return;
	}

	compile_time_assert(FIL_NULL == 0xffffffff);
	mtr->memset(btr_pcur_get_block(pcur), page_offset(ptr), 4, 0xff);

	ptr = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__SPACE, &len);

	ut_ad(len == 4);

	const uint32_t space_id = mach_read_from_4(ptr);
	ut_ad(space_id < SRV_TMP_SPACE_ID);
	if (space_id != TRX_SYS_SPACE
	    && trx_get_dict_operation(trx) == TRX_DICT_OP_TABLE) {
		/* We are about to delete the entire .ibd file;
		do not bother to free pages inside it. */
		return;
	}

	ptr = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__ID, &len);

	ut_ad(len == 8);

	if (fil_space_t* s = fil_space_t::get(space_id)) {
		/* Ensure that the tablespace file exists
		in order to avoid a crash in buf_page_get_gen(). */
		if (root_page_no < s->get_size()) {
			btr_free_if_exists(page_id_t(space_id, root_page_no),
					   s->zip_size(),
					   mach_read_from_8(ptr), mtr);
		}
		s->release();
	}
}

/*********************************************************************//**
Creates a table create graph.
@return own: table create node */
tab_node_t*
tab_create_graph_create(
/*====================*/
	dict_table_t*	table,	/*!< in: table to create, built as a memory data
				structure */
	mem_heap_t*	heap,	/*!< in: heap where created */
	fil_encryption_t mode,	/*!< in: encryption mode */
	uint32_t	key_id)	/*!< in: encryption key_id */
{
	tab_node_t*	node;

	node = static_cast<tab_node_t*>(
		mem_heap_alloc(heap, sizeof(tab_node_t)));

	node->common.type = QUE_NODE_CREATE_TABLE;

	node->table = table;

	node->state = TABLE_BUILD_TABLE_DEF;
	node->heap = mem_heap_create(256);
	node->mode = mode;
	node->key_id = key_id;

	node->tab_def = ins_node_create(INS_DIRECT, dict_sys.sys_tables,
					heap);
	node->tab_def->common.parent = node;

	node->col_def = ins_node_create(INS_DIRECT, dict_sys.sys_columns,
					heap);
	node->col_def->common.parent = node;

	node->v_col_def = ins_node_create(INS_DIRECT, dict_sys.sys_virtual,
                                          heap);
	node->v_col_def->common.parent = node;

	return(node);
}

/** Creates an index create graph.
@param[in]	index	index to create, built as a memory data structure
@param[in]	table	table name
@param[in,out]	heap	heap where created
@param[in]	add_v	new virtual columns added in the same clause with
			add index
@return own: index create node */
ind_node_t*
ind_create_graph_create(
	dict_index_t*		index,
	const char*		table,
	mem_heap_t*		heap,
	const dict_add_v_col_t*	add_v)
{
	ind_node_t*	node;

	node = static_cast<ind_node_t*>(
		mem_heap_alloc(heap, sizeof(ind_node_t)));

	node->common.type = QUE_NODE_CREATE_INDEX;

	node->index = index;

	node->table_name = table;

	node->add_v = add_v;

	node->state = INDEX_BUILD_INDEX_DEF;
	node->page_no = FIL_NULL;
	node->heap = mem_heap_create(256);

	node->ind_def = ins_node_create(INS_DIRECT,
					dict_sys.sys_indexes, heap);
	node->ind_def->common.parent = node;

	node->field_def = ins_node_create(INS_DIRECT,
					  dict_sys.sys_fields, heap);
	node->field_def->common.parent = node;

	return(node);
}

/***********************************************************//**
Creates a table. This is a high-level function used in SQL execution graphs.
@return query thread to run next or NULL */
que_thr_t*
dict_create_table_step(
/*===================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	tab_node_t*	node;
	dberr_t		err	= DB_ERROR;
	trx_t*		trx;

	ut_ad(thr);
	dict_sys.assert_locked();

	trx = thr_get_trx(thr);

	node = static_cast<tab_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_CREATE_TABLE);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = TABLE_BUILD_TABLE_DEF;
	}

	if (node->state == TABLE_BUILD_TABLE_DEF) {

		/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */

		err = dict_build_table_def_step(thr, node);
		if (err != DB_SUCCESS) {

			goto function_exit;
		}

		node->state = TABLE_BUILD_COL_DEF;
		node->col_no = 0;

		thr->run_node = node->tab_def;

		return(thr);
	}

	if (node->state == TABLE_BUILD_COL_DEF) {

		if (node->col_no + DATA_N_SYS_COLS
		    < (static_cast<ulint>(node->table->n_def)
		       + static_cast<ulint>(node->table->n_v_def))) {

			ulint i = node->col_no++;
			if (i + DATA_N_SYS_COLS >= node->table->n_def) {
				i += DATA_N_SYS_COLS;
			}

			ins_node_set_new_row(
				node->col_def,
				dict_create_sys_columns_tuple(node->table, i,
							      node->heap));

			thr->run_node = node->col_def;

			return(thr);
		} else {
			/* Move on to SYS_VIRTUAL table */
			node->col_no = 0;
                        node->base_col_no = 0;
                        node->state = TABLE_BUILD_V_COL_DEF;
		}
	}

	if (node->state == TABLE_BUILD_V_COL_DEF) {

		if (node->col_no < static_cast<ulint>(node->table->n_v_def)) {
			dict_v_col_t*   v_col = dict_table_get_nth_v_col(
						node->table, node->col_no);

			/* If no base column */
			while (v_col->num_base == 0) {
				node->col_no++;
				if (node->col_no == static_cast<ulint>(
					(node->table)->n_v_def)) {
					node->state = TABLE_ADD_TO_CACHE;
					break;
				}

				v_col = dict_table_get_nth_v_col(
					node->table, node->col_no);
				node->base_col_no = 0;
			}

			if (node->state != TABLE_ADD_TO_CACHE) {
				ut_ad(node->col_no == v_col->v_pos);
				dict_build_v_col_def_step(node);

				if (node->base_col_no
				    < unsigned{v_col->num_base} - 1) {
					/* move on to next base column */
					node->base_col_no++;
				} else {
					/* move on to next virtual column */
					node->col_no++;
					node->base_col_no = 0;
				}

				thr->run_node = node->v_col_def;

				return(thr);
			}
		} else {
			node->state = TABLE_ADD_TO_CACHE;
		}
	}

	if (node->state == TABLE_ADD_TO_CACHE) {
		DBUG_EXECUTE_IF("ib_ddl_crash_during_create", DBUG_SUICIDE(););

		node->table->can_be_evicted = true;
		node->table->add_to_cache();

		err = DB_SUCCESS;
	}

function_exit:
	trx->error_state = err;

	if (err == DB_SUCCESS) {
		/* Ok: do nothing */

	} else if (err == DB_LOCK_WAIT) {

		return(NULL);
	} else {
		/* SQL error detected */

		return(NULL);
	}

	thr->run_node = que_node_get_parent(node);

	return(thr);
}

/***********************************************************//**
Creates an index. This is a high-level function used in SQL execution
graphs.
@return query thread to run next or NULL */
que_thr_t*
dict_create_index_step(
/*===================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	ind_node_t*	node;
	dberr_t		err	= DB_ERROR;
	trx_t*		trx;

	ut_ad(thr);
	dict_sys.assert_locked();

	trx = thr_get_trx(thr);

	node = static_cast<ind_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_CREATE_INDEX);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = INDEX_BUILD_INDEX_DEF;
	}

	if (node->state == INDEX_BUILD_INDEX_DEF) {
		/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */
		err = dict_build_index_def_step(thr, node);

		if (err != DB_SUCCESS) {

			goto function_exit;
		}

		node->state = INDEX_BUILD_FIELD_DEF;
		node->field_no = 0;

		thr->run_node = node->ind_def;

		return(thr);
	}

	if (node->state == INDEX_BUILD_FIELD_DEF) {

		if (node->field_no < (node->index)->n_fields) {

			dict_build_field_def_step(node);

			node->field_no++;

			thr->run_node = node->field_def;

			return(thr);
		} else {
			node->state = INDEX_ADD_TO_CACHE;
		}
	}

	if (node->state == INDEX_ADD_TO_CACHE) {
		ut_ad(node->index->table == node->table);
		err = dict_index_add_to_cache(node->index, FIL_NULL,
					      node->add_v);

		ut_ad((node->index == NULL) == (err != DB_SUCCESS));

		if (!node->index) {
			goto function_exit;
		}

		ut_ad(!node->index->is_instant());
		ut_ad(node->index->n_core_null_bytes
		      == ((dict_index_is_clust(node->index)
			   && node->table->supports_instant())
			  ? dict_index_t::NO_CORE_NULL_BYTES
			  : UT_BITS_IN_BYTES(
				  unsigned(node->index->n_nullable))));
		node->index->n_core_null_bytes = static_cast<uint8_t>(
			UT_BITS_IN_BYTES(unsigned(node->index->n_nullable)));
		node->state = INDEX_CREATE_INDEX_TREE;
	}

	if (node->state == INDEX_CREATE_INDEX_TREE) {

		err = dict_create_index_tree_step(node);

		DBUG_EXECUTE_IF("ib_dict_create_index_tree_fail",
				err = DB_OUT_OF_MEMORY;);

		if (err != DB_SUCCESS) {
			/* If this is a FTS index, we will need to remove
			it from fts->cache->indexes list as well */
			if ((node->index->type & DICT_FTS)
			    && node->table->fts) {
				fts_index_cache_t*	index_cache;

				mysql_mutex_lock(
					&node->table->fts->cache->init_lock);

				index_cache = (fts_index_cache_t*)
					 fts_find_index_cache(
						node->table->fts->cache,
						node->index);

				if (index_cache->words) {
					rbt_free(index_cache->words);
					index_cache->words = 0;
				}

				ib_vector_remove(
					node->table->fts->cache->indexes,
					*reinterpret_cast<void**>(index_cache));

				mysql_mutex_unlock(
					&node->table->fts->cache->init_lock);
			}

#ifdef BTR_CUR_HASH_ADAPT
			ut_ad(!node->index->search_info->ref_count);
#endif /* BTR_CUR_HASH_ADAPT */
			dict_index_remove_from_cache(node->table, node->index);
			node->index = NULL;

			goto function_exit;
		}

		node->index->page = node->page_no;
		/* These should have been set in
		dict_build_index_def_step() and
		dict_index_add_to_cache(). */
		ut_ad(node->index->trx_id == trx->id);
		ut_ad(node->index->table->def_trx_id == trx->id);
	}

function_exit:
	trx->error_state = err;

	if (err == DB_SUCCESS) {
		/* Ok: do nothing */

	} else if (err == DB_LOCK_WAIT) {

		return(NULL);
	} else {
		/* SQL error detected */

		return(NULL);
	}

	thr->run_node = que_node_get_parent(node);

	return(thr);
}

/****************************************************************//**
Check whether a system table exists.  Additionally, if it exists,
move it to the non-LRU end of the table LRU list.  This is only used
for system tables that can be upgraded or added to an older database,
which include SYS_FOREIGN and SYS_FOREIGN_COLS.
@return DB_SUCCESS if the sys table exists, DB_CORRUPTION if it exists
but is not current, DB_TABLE_NOT_FOUND if it does not exist*/
static
dberr_t
dict_check_if_system_table_exists(
/*==============================*/
	const char*	tablename,	/*!< in: name of table */
	ulint		num_fields,	/*!< in: number of fields */
	ulint		num_indexes)	/*!< in: number of indexes */
{
	dict_table_t*	sys_table;
	dberr_t		error = DB_SUCCESS;

	ut_ad(!srv_any_background_activity());

	dict_sys.mutex_lock();

	sys_table = dict_table_get_low(tablename);

	if (sys_table == NULL) {
		error = DB_TABLE_NOT_FOUND;

	} else if (UT_LIST_GET_LEN(sys_table->indexes) != num_indexes
		   || sys_table->n_cols != num_fields) {
		error = DB_CORRUPTION;

	} else {
		/* This table has already been created, and it is OK.
		Ensure that it can't be evicted from the table LRU cache. */

		dict_table_prevent_eviction(sys_table);
	}

	dict_sys.mutex_unlock();

	return(error);
}

/** Creates the virtual column system table (SYS_VIRTUAL) inside InnoDB
at server bootstrap or server start if the table is not found or is
not of the right form.
@return DB_SUCCESS or error code */
dberr_t
dict_create_or_check_sys_virtual()
{
	trx_t*		trx;
	my_bool		srv_file_per_table_backup;
	dberr_t		err;

	ut_ad(!srv_any_background_activity());

	/* Note: The master thread has not been started at this point. */
	err = dict_check_if_system_table_exists(
		"SYS_VIRTUAL", DICT_NUM_FIELDS__SYS_VIRTUAL + 1, 1);

	if (err == DB_SUCCESS) {
		dict_sys.mutex_lock();
		dict_sys.sys_virtual = dict_table_get_low("SYS_VIRTUAL");
		dict_sys.mutex_unlock();
		return(DB_SUCCESS);
	}

	if (srv_read_only_mode
	    || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO) {
		return(DB_READ_ONLY);
	}

	trx = trx_create();

	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	trx->op_info = "creating sys_virtual tables";

	row_mysql_lock_data_dictionary(trx);

	/* Check which incomplete table definition to drop. */

	if (err == DB_CORRUPTION) {
		row_drop_table_after_create_fail("SYS_VIRTUAL", trx);
	}

	ib::info() << "Creating sys_virtual system tables.";

	srv_file_per_table_backup = srv_file_per_table;

	/* We always want SYSTEM tables to be created inside the system
	tablespace. */

	srv_file_per_table = 0;

	err = que_eval_sql(
		NULL,
		"PROCEDURE CREATE_SYS_VIRTUAL_TABLES_PROC () IS\n"
		"BEGIN\n"
		"CREATE TABLE\n"
		"SYS_VIRTUAL(TABLE_ID BIGINT, POS INT,"
		" BASE_POS INT);\n"
		"CREATE UNIQUE CLUSTERED INDEX BASE_IDX"
		" ON SYS_VIRTUAL(TABLE_ID, POS, BASE_POS);\n"
		"END;\n",
		FALSE, trx);

	if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
		ib::error() << "Creation of SYS_VIRTUAL"
			" failed: " << err << ". Tablespace is"
			" full or too many transactions."
			" Dropping incompletely created tables.";

		ut_ad(err == DB_OUT_OF_FILE_SPACE
		      || err == DB_TOO_MANY_CONCURRENT_TRXS);

		row_drop_table_after_create_fail("SYS_VIRTUAL", trx);

		if (err == DB_OUT_OF_FILE_SPACE) {
			err = DB_MUST_GET_MORE_FILE_SPACE;
		}
	}

	trx_commit_for_mysql(trx);

	row_mysql_unlock_data_dictionary(trx);

	trx->free();

	srv_file_per_table = srv_file_per_table_backup;

	/* Note: The master thread has not been started at this point. */
	/* Confirm and move to the non-LRU part of the table LRU list. */
	dberr_t sys_virtual_err = dict_check_if_system_table_exists(
		"SYS_VIRTUAL", DICT_NUM_FIELDS__SYS_VIRTUAL + 1, 1);
	ut_a(sys_virtual_err == DB_SUCCESS);
	dict_sys.mutex_lock();
	dict_sys.sys_virtual = dict_table_get_low("SYS_VIRTUAL");
	dict_sys.mutex_unlock();

	return(err);
}

/** Check if a foreign constraint is on the given column name.
@param[in]	col_name	column name to be searched for fk constraint
@param[in]	table		table to which foreign key constraint belongs
@return true if fk constraint is present on the table, false otherwise. */
static
bool
dict_foreign_base_for_stored(
	const char*		col_name,
	const dict_table_t*	table)
{
	/* Loop through each stored column and check if its base column has
	the same name as the column name being checked */
	dict_s_col_list::const_iterator	it;
	for (it = table->s_cols->begin();
	     it != table->s_cols->end(); ++it) {
		dict_s_col_t	s_col = *it;

		for (ulint j = 0; j < s_col.num_base; j++) {
			if (strcmp(col_name, dict_table_get_col_name(
						table,
						s_col.base_col[j]->ind)) == 0) {
				return(true);
			}
		}
	}

	return(false);
}

/** Check if a foreign constraint is on columns served as base columns
of any stored column. This is to prevent creating SET NULL or CASCADE
constraint on such columns
@param[in]	local_fk_set	set of foreign key objects, to be added to
the dictionary tables
@param[in]	table		table to which the foreign key objects in
local_fk_set belong to
@return true if yes, otherwise, false */
bool
dict_foreigns_has_s_base_col(
	const dict_foreign_set&	local_fk_set,
	const dict_table_t*	table)
{
	dict_foreign_t*	foreign;

	if (table->s_cols == NULL) {
		return (false);
	}

	for (dict_foreign_set::const_iterator it = local_fk_set.begin();
	     it != local_fk_set.end(); ++it) {

		foreign = *it;
		ulint	type = foreign->type;

		type &= ~(DICT_FOREIGN_ON_DELETE_NO_ACTION
			  | DICT_FOREIGN_ON_UPDATE_NO_ACTION);

		if (type == 0) {
			continue;
		}

		for (ulint i = 0; i < foreign->n_fields; i++) {
			/* Check if the constraint is on a column that
			is a base column of any stored column */
			if (dict_foreign_base_for_stored(
				foreign->foreign_col_names[i], table)) {
				return(true);
			}
		}
	}

	return(false);
}
