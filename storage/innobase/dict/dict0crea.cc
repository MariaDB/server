/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
#include "lock0lock.h"
#include "que0que.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "pars0pars.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "ut0vec.h"
#include "fts0priv.h"
#include "srv0start.h"
#include "log.h"

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
	ut_ad(dict_sys.locked());
	dict_table_t*	table = node->table;
	ut_ad(!table->is_temporary());
	ut_ad(!table->space);
	ut_ad(table->space_id == UINT32_MAX);
	dict_hdr_get_new_id(&table->id, nullptr, nullptr);

	/* Always set this bit for all new created tables */
	DICT_TF2_FLAG_SET(table, DICT_TF2_FTS_AUX_HEX_NAME);
	DBUG_EXECUTE_IF("innodb_test_wrong_fts_aux_table_name",
			DICT_TF2_FLAG_UNSET(table,
					    DICT_TF2_FTS_AUX_HEX_NAME););

	if (DICT_TF2_FLAG_IS_SET(table, DICT_TF2_USE_FILE_PER_TABLE)) {
		/* This table will need a new tablespace. */

		ut_ad(DICT_TF_GET_ZIP_SSIZE(table->flags) == 0
		      || dict_table_has_atomic_blobs(table));
		/* Get a new tablespace ID */
		dict_hdr_get_new_id(NULL, NULL, &table->space_id);

		DBUG_EXECUTE_IF(
			"ib_create_table_fail_out_of_space_ids",
			table->space_id = UINT32_MAX;
		);

		if (table->space_id == UINT32_MAX) {
			return DB_ERROR;
		}
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
static MY_ATTRIBUTE((nonnull, warn_unused_result))
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

	ut_ad(dict_sys.locked());
	ut_ad(index);
	ut_ad(index->table->space || !UT_LIST_GET_LEN(index->table->indexes)
	      || index->table->file_unreadable);
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
	bool		wide_pos = false;

	ut_ad(index);
	ut_ad(heap);

	for (unsigned j = 0; j < index->n_fields; j++) {
		const dict_field_t* f = dict_index_get_nth_field(index, j);
		if (f->prefix_len || f->descending) {
			wide_pos = true;
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

	if (wide_pos) {
		/* If there are column prefixes or columns with
		descending order in the index, then we write the
		field number to the 16 most significant bits,
		the DESC flag to bit 15, and the prefix length
		in the 15 least significant bits. */
		mach_write_to_4(ptr, (fld_no << 16)
				| (!!field->descending) << 15
				| field->prefix_len);
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

	ut_ad(dict_sys.locked());

	trx = thr_get_trx(thr);

	index = node->index;

	table = dict_table_open_on_name(
		node->table_name, true, DICT_ERR_IGNORE_TABLESPACE);

	if (!table) {
		return DB_TABLE_NOT_FOUND;
	}

	index->table = table;

	ut_ad((UT_LIST_GET_LEN(table->indexes) > 0)
	      || dict_index_is_clust(index));

	dict_hdr_get_new_id(NULL, &index->id, NULL);

	node->page_no = FIL_NULL;
	row = dict_create_sys_indexes_tuple(index, node->heap);
	node->ind_row = row;

	ins_node_set_new_row(node->ind_def, row);

	/* Note that the index was created by this transaction. */
	index->trx_id = trx->id;
	ut_ad(table->def_trx_id <= trx->id);
	table->def_trx_id = trx->id;
	table->release();

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
	ut_ad(dict_sys.locked());

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

	ut_ad(dict_sys.locked());

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

	ut_ad(dict_sys.locked());
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
@param[in,out]	mtr	mini-transaction
@return tablespace ID to drop (if this is the clustered index)
@retval 0 if no tablespace is to be dropped */
uint32_t dict_drop_index_tree(btr_pcur_t *pcur, trx_t *trx, mtr_t *mtr)
{
  rec_t *rec= btr_pcur_get_rec(pcur);

  ut_ad(!trx || dict_sys.locked());
  ut_ad(!dict_table_is_comp(dict_sys.sys_indexes));
  btr_pcur_store_position(pcur, mtr);

  static_assert(DICT_FLD__SYS_INDEXES__TABLE_ID == 0, "compatibility");
  static_assert(DICT_FLD__SYS_INDEXES__ID == 1, "compatibility");

  ulint len= rec_get_n_fields_old(rec);
  if (len < DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD ||
      len > DICT_NUM_FIELDS__SYS_INDEXES)
  {
rec_corrupted:
    sql_print_error("InnoDB: Corrupted SYS_INDEXES record");
    return 0;
  }

  if (rec_get_1byte_offs_flag(rec))
  {
    if (rec_1_get_field_end_info(rec, 0) != 8 ||
        rec_1_get_field_end_info(rec, 1) != 8 + 8)
      goto rec_corrupted;
  }
  else if (rec_2_get_field_end_info(rec, 0) != 8 ||
           rec_2_get_field_end_info(rec, 1) != 8 + 8)
    goto rec_corrupted;

  const byte *p= rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__TYPE, &len);
  if (len != 4)
    goto rec_corrupted;
  const uint32_t type= mach_read_from_4(p);
  p= rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);
  if (len != 4)
    goto rec_corrupted;
  const uint32_t root_page_no= mach_read_from_4(p);
  p= rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__SPACE, &len);
  if (len != 4)
    goto rec_corrupted;

  const uint32_t space_id= mach_read_from_4(p);
  ut_ad(root_page_no == FIL_NULL || space_id <= SRV_SPACE_ID_UPPER_BOUND);

  if (space_id && (type & DICT_CLUSTERED))
    return space_id;

  if (root_page_no == FIL_NULL)
    /* The tree has already been freed */;
  else if (fil_space_t*s= fil_space_t::get(space_id))
  {
    /* Ensure that the tablespace file exists
    in order to avoid a crash in buf_page_get_gen(). */
    if (root_page_no < s->get_size())
    {
      static_assert(FIL_NULL == 0xffffffff, "compatibility");
      static_assert(DICT_FLD__SYS_INDEXES__PAGE_NO ==
                    DICT_FLD__SYS_INDEXES__SPACE + 1, "compatibility");
      mtr->memset(btr_pcur_get_block(pcur), page_offset(p + 4), 4, 0xff);
      btr_free_if_exists(s, root_page_no, mach_read_from_8(rec + 8), mtr);
    }
    s->release();
  }

  return 0;
}

/*********************************************************************//**
Creates a table create graph.
@return own: table create node */
tab_node_t*
tab_create_graph_create(
/*====================*/
	dict_table_t*	table,	/*!< in: table to create, built as a memory data
				structure */
	mem_heap_t*	heap)	/*!< in: heap where created */
{
	tab_node_t*	node;

	node = static_cast<tab_node_t*>(
		mem_heap_alloc(heap, sizeof(tab_node_t)));

	node->common.type = QUE_NODE_CREATE_TABLE;

	node->table = table;

	node->state = TABLE_BUILD_TABLE_DEF;
	node->heap = mem_heap_create(256);

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
@param[in]	mode	encryption mode (for creating a table)
@param[in]	key_id	encryption key identifier (for creating a table)
@param[in]	add_v	new virtual columns added in the same clause with
			add index
@return own: index create node */
ind_node_t*
ind_create_graph_create(
	dict_index_t*		index,
	const char*		table,
	mem_heap_t*		heap,
	fil_encryption_t	mode,
	uint32_t		key_id,
	const dict_add_v_col_t*	add_v)
{
	ind_node_t*	node;

	node = static_cast<ind_node_t*>(
		mem_heap_alloc(heap, sizeof(ind_node_t)));

	node->common.type = QUE_NODE_CREATE_INDEX;

	node->index = index;

	node->table_name = table;

	node->key_id = key_id;
	node->mode = mode;
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
	ut_ad(dict_sys.locked());

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

static dberr_t dict_create_index_space(const ind_node_t &node)
{
  dict_table_t *table= node.index->table;
  if (table->space || (table->flags2 & DICT_TF2_DISCARDED))
    return DB_SUCCESS;
  ut_ad(table->space_id);
  ut_ad(table->space_id < SRV_TMP_SPACE_ID);
  /* Determine the tablespace flags. */
  const bool has_data_dir= DICT_TF_HAS_DATA_DIR(table->flags);
  ut_ad(!has_data_dir || table->data_dir_path);
  char* filepath= fil_make_filepath(has_data_dir
                                    ? table->data_dir_path : nullptr,
                                    table->name, IBD, has_data_dir);
  if (!filepath)
    return DB_OUT_OF_MEMORY;

  /* We create a new single-table tablespace for the table.
  We initially let it be 4 pages:
  - page 0 is the fsp header and an extent descriptor page,
  - page 1 is an ibuf bitmap page,
  - page 2 is the first inode page,
  - page 3 will contain the root of the clustered index of
  the table we create here. */
  dberr_t err;
  table->space= fil_ibd_create(table->space_id, table->name, filepath,
                               dict_tf_to_fsp_flags(table->flags),
                               FIL_IBD_FILE_INITIAL_SIZE,
                               node.mode, node.key_id, &err);
  ut_ad((err != DB_SUCCESS) == !table->space);
  ut_free(filepath);

  return err;
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
	ut_ad(dict_sys.locked());

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
		err = dict_create_index_space(*node);
		if (err != DB_SUCCESS) {
			dict_mem_index_free(node->index);
			node->index = nullptr;
			goto function_exit;
		}

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
		err = dict_index_add_to_cache(node->index, FIL_NULL,
					      node->add_v);

		ut_ad(!node->index == (err != DB_SUCCESS));

		if (!node->index) {
			goto function_exit;
		}

		ut_ad(!node->index->is_instant());
		ut_ad(node->index->n_core_null_bytes
		      == ((dict_index_is_clust(node->index)
			   && node->index->table->supports_instant())
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
			dict_table_t* table = node->index->table;
			/* If this is a FTS index, we will need to remove
			it from fts->cache->indexes list as well */
			if (!(node->index->type & DICT_FTS)) {
			} else if (auto fts = table->fts) {
				fts_index_cache_t*	index_cache;

				mysql_mutex_lock(&fts->cache->init_lock);

				index_cache = (fts_index_cache_t*)
					 fts_find_index_cache(
						fts->cache,
						node->index);

				if (index_cache->words) {
					rbt_free(index_cache->words);
					index_cache->words = 0;
				}

				ib_vector_remove(
					fts->cache->indexes,
					*reinterpret_cast<void**>(index_cache));

				mysql_mutex_unlock(&fts->cache->init_lock);
			}

#ifdef BTR_CUR_HASH_ADAPT
			ut_ad(!node->index->search_info->ref_count);
#endif /* BTR_CUR_HASH_ADAPT */
			dict_index_remove_from_cache(table, node->index);
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

bool dict_sys_t::load_sys_tables()
{
  ut_ad(!srv_any_background_activity());
  bool mismatch= false;
  lock(SRW_LOCK_CALL);
  if (!(sys_foreign= load_table(SYS_TABLE[SYS_FOREIGN],
                                DICT_ERR_IGNORE_FK_NOKEY)));
  else if (UT_LIST_GET_LEN(sys_foreign->indexes) == 3 &&
           sys_foreign->n_cols == DICT_NUM_COLS__SYS_FOREIGN + DATA_N_SYS_COLS)
    prevent_eviction(sys_foreign);
  else
  {
    sys_foreign= nullptr;
    mismatch= true;
    sql_print_error("InnoDB: Invalid definition of SYS_FOREIGN");
  }
  if (!(sys_foreign_cols= load_table(SYS_TABLE[SYS_FOREIGN_COLS],
                                     DICT_ERR_IGNORE_FK_NOKEY)));
  else if (UT_LIST_GET_LEN(sys_foreign_cols->indexes) == 1 &&
           sys_foreign_cols->n_cols ==
           DICT_NUM_COLS__SYS_FOREIGN_COLS + DATA_N_SYS_COLS)
    prevent_eviction(sys_foreign_cols);
  else
  {
    sys_foreign_cols= nullptr;
    mismatch= true;
    sql_print_error("InnoDB: Invalid definition of SYS_FOREIGN_COLS");
  }
  if (!(sys_virtual= load_table(SYS_TABLE[SYS_VIRTUAL],
                                DICT_ERR_IGNORE_FK_NOKEY)));
  else if (UT_LIST_GET_LEN(sys_virtual->indexes) == 1 &&
           sys_virtual->n_cols == DICT_NUM_COLS__SYS_VIRTUAL + DATA_N_SYS_COLS)
    prevent_eviction(sys_virtual);
  else
  {
    sys_virtual= nullptr;
    mismatch= true;
    sql_print_error("InnoDB: Invalid definition of SYS_VIRTUAL");
  }
  unlock();
  return mismatch;
}

dberr_t dict_sys_t::create_or_check_sys_tables()
{
  if (sys_tables_exist())
    return DB_SUCCESS;

  if (srv_read_only_mode || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO)
    return DB_READ_ONLY;

  if (load_sys_tables())
  {
    sql_print_information("InnoDB: Set innodb_read_only=1 "
                          "or innodb_force_recovery=3 to start up");
    return DB_CORRUPTION;
  }

  if (sys_tables_exist())
    return DB_SUCCESS;

  trx_t *trx= trx_create();
  trx_start_for_ddl(trx);

  {
    /* Do not bother with transactional memory; this is only
    executed at startup, with no conflicts present. */
    LockMutexGuard g{SRW_LOCK_CALL};
    trx->mutex_lock();
    lock_table_create(dict_sys.sys_tables, LOCK_X, trx);
    lock_table_create(dict_sys.sys_columns, LOCK_X, trx);
    lock_table_create(dict_sys.sys_indexes, LOCK_X, trx);
    lock_table_create(dict_sys.sys_fields, LOCK_X, trx);
    trx->mutex_unlock();
  }

  row_mysql_lock_data_dictionary(trx);

  /* NOTE: when designing InnoDB's foreign key support in 2001, Heikki Tuuri
  made a mistake and defined table names and the foreign key id to be of type
  CHAR (internally, really VARCHAR). The type should have been VARBINARY. */

  /* System tables are always created inside the system tablespace. */
  const auto srv_file_per_table_backup= srv_file_per_table;
  srv_file_per_table= 0;
  dberr_t error;
  span<const char> tablename;

  if (!sys_foreign)
  {
    error= que_eval_sql(nullptr, "PROCEDURE CREATE_FOREIGN() IS\n"
                        "BEGIN\n"
                        "CREATE TABLE\n"
                        "SYS_FOREIGN(ID CHAR, FOR_NAME CHAR,"
                        " REF_NAME CHAR, N_COLS INT);\n"
                        "CREATE UNIQUE CLUSTERED INDEX ID_IND"
                        " ON SYS_FOREIGN (ID);\n"
                        "CREATE INDEX FOR_IND"
                        " ON SYS_FOREIGN (FOR_NAME);\n"
                        "CREATE INDEX REF_IND"
                        " ON SYS_FOREIGN (REF_NAME);\n"
                        "END;\n", trx);
    if (UNIV_UNLIKELY(error != DB_SUCCESS))
    {
      tablename= SYS_TABLE[SYS_FOREIGN];
err_exit:
      sql_print_error("InnoDB: Creation of %.*s failed: %s",
                      int(tablename.size()), tablename.data(),
                      ut_strerr(error));
      trx->rollback();
      row_mysql_unlock_data_dictionary(trx);
      trx->free();
      srv_file_per_table= srv_file_per_table_backup;
      return error;
    }
  }
  if (!sys_foreign_cols)
  {
    error= que_eval_sql(nullptr, "PROCEDURE CREATE_FOREIGN_COLS() IS\n"
                        "BEGIN\n"
                        "CREATE TABLE\n"
                        "SYS_FOREIGN_COLS(ID CHAR, POS INT,"
                        " FOR_COL_NAME CHAR, REF_COL_NAME CHAR);\n"
                        "CREATE UNIQUE CLUSTERED INDEX ID_IND"
                        " ON SYS_FOREIGN_COLS (ID, POS);\n"
                        "END;\n", trx);
    if (UNIV_UNLIKELY(error != DB_SUCCESS))
    {
      tablename= SYS_TABLE[SYS_FOREIGN_COLS];
      goto err_exit;
    }
  }
  if (!sys_virtual)
  {
    error= que_eval_sql(nullptr, "PROCEDURE CREATE_VIRTUAL() IS\n"
                        "BEGIN\n"
                        "CREATE TABLE\n"
                        "SYS_VIRTUAL(TABLE_ID BIGINT,POS INT,BASE_POS INT);\n"
                        "CREATE UNIQUE CLUSTERED INDEX BASE_IDX"
                        " ON SYS_VIRTUAL(TABLE_ID, POS, BASE_POS);\n"
                        "END;\n", trx);
    if (UNIV_UNLIKELY(error != DB_SUCCESS))
    {
      tablename= SYS_TABLE[SYS_VIRTUAL];
      goto err_exit;
    }
  }

  trx->commit();
  row_mysql_unlock_data_dictionary(trx);
  trx->free();
  srv_file_per_table= srv_file_per_table_backup;

  lock(SRW_LOCK_CALL);
  if (sys_foreign);
  else if (!(sys_foreign= load_table(SYS_TABLE[SYS_FOREIGN])))
  {
    tablename= SYS_TABLE[SYS_FOREIGN];
load_fail:
    unlock();
    sql_print_error("InnoDB: Failed to CREATE TABLE %.*s",
                    int(tablename.size()), tablename.data());
    return DB_TABLE_NOT_FOUND;
  }
  else
    prevent_eviction(sys_foreign);

  if (sys_foreign_cols);
  else if (!(sys_foreign_cols= load_table(SYS_TABLE[SYS_FOREIGN_COLS])))
  {
    tablename= SYS_TABLE[SYS_FOREIGN_COLS];
    goto load_fail;
  }
  else
    prevent_eviction(sys_foreign_cols);

  if (sys_virtual);
  else if (!(sys_virtual= load_table(SYS_TABLE[SYS_VIRTUAL])))
  {
    tablename= SYS_TABLE[SYS_VIRTUAL];
    goto load_fail;
  }
  else
    prevent_eviction(sys_virtual);

  unlock();
  return DB_SUCCESS;
}

/****************************************************************//**
Evaluate the given foreign key SQL statement.
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
dict_foreign_eval_sql(
/*==================*/
	pars_info_t*	info,	/*!< in: info struct */
	const char*	sql,	/*!< in: SQL string to evaluate */
	const char*	name,	/*!< in: table name (for diagnostics) */
	const char*	id,	/*!< in: foreign key id */
	trx_t*		trx)	/*!< in/out: transaction */
{
	FILE*	ef	= dict_foreign_err_file;

	dberr_t error = que_eval_sql(info, sql, trx);

	switch (error) {
	case DB_SUCCESS:
		break;
	case DB_DUPLICATE_KEY:
		mysql_mutex_lock(&dict_foreign_err_mutex);
		rewind(ef);
		ut_print_timestamp(ef);
		fputs(" Error in foreign key constraint creation for table ",
		      ef);
		ut_print_name(ef, trx, name);
		fputs(".\nA foreign key constraint of name ", ef);
		ut_print_name(ef, trx, id);
		fputs("\nalready exists."
		      " (Note that internally InnoDB adds 'databasename'\n"
		      "in front of the user-defined constraint name.)\n"
		      "Note that InnoDB's FOREIGN KEY system tables store\n"
		      "constraint names as case-insensitive, with the\n"
		      "MariaDB standard latin1_swedish_ci collation. If you\n"
		      "create tables or databases whose names differ only in\n"
		      "the character case, then collisions in constraint\n"
		      "names can occur. Workaround: name your constraints\n"
		      "explicitly with unique names.\n",
		      ef);
		goto release;
	default:
		sql_print_error("InnoDB: "
				"Foreign key constraint creation failed: %s",
				ut_strerr(error));

		mysql_mutex_lock(&dict_foreign_err_mutex);
		ut_print_timestamp(ef);
		fputs(" Internal error in foreign key constraint creation"
		      " for table ", ef);
		ut_print_name(ef, trx, name);
		fputs(".\n"
		      "See the MariaDB .err log in the datadir"
		      " for more information.\n", ef);
release:
		mysql_mutex_unlock(&dict_foreign_err_mutex);
	}

	return error;
}

/********************************************************************//**
Add a single foreign key field definition to the data dictionary tables in
the database.
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
dict_create_add_foreign_field_to_dictionary(
/*========================================*/
	ulint			field_nr,	/*!< in: field number */
	const char*		table_name,	/*!< in: table name */
	const dict_foreign_t*	foreign,	/*!< in: foreign */
	trx_t*			trx)		/*!< in/out: transaction */
{
	DBUG_ENTER("dict_create_add_foreign_field_to_dictionary");

	pars_info_t*	info = pars_info_create();

	pars_info_add_str_literal(info, "id", foreign->id);

	pars_info_add_int4_literal(info, "pos", field_nr);

	pars_info_add_str_literal(info, "for_col_name",
				  foreign->foreign_col_names[field_nr]);

	pars_info_add_str_literal(info, "ref_col_name",
				  foreign->referenced_col_names[field_nr]);

	DBUG_RETURN(dict_foreign_eval_sql(
		       info,
		       "PROCEDURE P () IS\n"
		       "BEGIN\n"
		       "INSERT INTO SYS_FOREIGN_COLS VALUES"
		       "(:id, :pos, :for_col_name, :ref_col_name);\n"
		       "END;\n",
		       table_name, foreign->id, trx));
}

/********************************************************************//**
Construct foreign key constraint defintion from data dictionary information.
*/
static
char*
dict_foreign_def_get(
/*=================*/
	dict_foreign_t*	foreign,/*!< in: foreign */
	trx_t*		trx)	/*!< in: trx */
{
	char* fk_def = (char *)mem_heap_alloc(foreign->heap, 4*1024);
	const char* tbname;
	char tablebuf[MAX_TABLE_NAME_LEN + 1] = "";
	unsigned i;
	char* bufend;

	tbname = dict_remove_db_name(foreign->id);
	bufend = innobase_convert_name(tablebuf, MAX_TABLE_NAME_LEN,
				tbname, strlen(tbname), trx->mysql_thd);
	tablebuf[bufend - tablebuf] = '\0';

	sprintf(fk_def,
		(char *)"CONSTRAINT %s FOREIGN KEY (", (char *)tablebuf);

	for(i = 0; i < foreign->n_fields; i++) {
		char	buf[MAX_TABLE_NAME_LEN + 1] = "";
		innobase_convert_name(buf, MAX_TABLE_NAME_LEN,
				foreign->foreign_col_names[i],
				strlen(foreign->foreign_col_names[i]),
				trx->mysql_thd);
		strcat(fk_def, buf);
		if (i < static_cast<unsigned>(foreign->n_fields-1)) {
			strcat(fk_def, (char *)",");
		}
	}

	strcat(fk_def,(char *)") REFERENCES ");

	bufend = innobase_convert_name(tablebuf, MAX_TABLE_NAME_LEN,
	        	        foreign->referenced_table_name,
			        strlen(foreign->referenced_table_name),
			        trx->mysql_thd);
	tablebuf[bufend - tablebuf] = '\0';

	strcat(fk_def, tablebuf);
	strcat(fk_def, " (");

	for(i = 0; i < foreign->n_fields; i++) {
		char	buf[MAX_TABLE_NAME_LEN + 1] = "";
		bufend = innobase_convert_name(buf, MAX_TABLE_NAME_LEN,
				foreign->referenced_col_names[i],
				strlen(foreign->referenced_col_names[i]),
				trx->mysql_thd);
		buf[bufend - buf] = '\0';
		strcat(fk_def, buf);
		if (i < (uint)foreign->n_fields-1) {
			strcat(fk_def, (char *)",");
		}
	}
	strcat(fk_def, (char *)")");

	return fk_def;
}

/********************************************************************//**
Convert foreign key column names from data dictionary to SQL-layer.
*/
static
void
dict_foreign_def_get_fields(
/*========================*/
	dict_foreign_t*	foreign,/*!< in: foreign */
	trx_t*		trx,	/*!< in: trx */
	char**		field,  /*!< out: foreign column */
	char**		field2, /*!< out: referenced column */
	ulint		col_no) /*!< in: column number */
{
	char* bufend;
	char* fieldbuf = (char *)mem_heap_alloc(foreign->heap, MAX_TABLE_NAME_LEN+1);
	char* fieldbuf2 = (char *)mem_heap_alloc(foreign->heap, MAX_TABLE_NAME_LEN+1);

	bufend = innobase_convert_name(fieldbuf, MAX_TABLE_NAME_LEN,
			foreign->foreign_col_names[col_no],
			strlen(foreign->foreign_col_names[col_no]),
			trx->mysql_thd);

	fieldbuf[bufend - fieldbuf] = '\0';

	bufend = innobase_convert_name(fieldbuf2, MAX_TABLE_NAME_LEN,
			foreign->referenced_col_names[col_no],
			strlen(foreign->referenced_col_names[col_no]),
			trx->mysql_thd);

	fieldbuf2[bufend - fieldbuf2] = '\0';
	*field = fieldbuf;
	*field2 = fieldbuf2;
}

/********************************************************************//**
Add a foreign key definition to the data dictionary tables.
@return error code or DB_SUCCESS */
dberr_t
dict_create_add_foreign_to_dictionary(
/*==================================*/
	const char*		name,	/*!< in: table name */
	const dict_foreign_t*	foreign,/*!< in: foreign key */
	trx_t*			trx)	/*!< in/out: dictionary transaction */
{
	dberr_t		error;

	DBUG_ENTER("dict_create_add_foreign_to_dictionary");

	pars_info_t*	info = pars_info_create();

	pars_info_add_str_literal(info, "id", foreign->id);

	pars_info_add_str_literal(info, "for_name", name);

	pars_info_add_str_literal(info, "ref_name",
				  foreign->referenced_table_name);

	pars_info_add_int4_literal(info, "n_cols",
				   ulint(foreign->n_fields)
				   | (ulint(foreign->type) << 24));

	DBUG_PRINT("dict_create_add_foreign_to_dictionary",
		   ("'%s', '%s', '%s', %d", foreign->id, name,
		    foreign->referenced_table_name,
		    foreign->n_fields + (foreign->type << 24)));

	error = dict_foreign_eval_sql(info,
				      "PROCEDURE P () IS\n"
				      "BEGIN\n"
				      "INSERT INTO SYS_FOREIGN VALUES"
				      "(:id, :for_name, :ref_name, :n_cols);\n"
				      "END;\n"
				      , name, foreign->id, trx);

	if (error != DB_SUCCESS) {

		if (error == DB_DUPLICATE_KEY) {
			char	buf[MAX_TABLE_NAME_LEN + 1] = "";
			char	tablename[MAX_TABLE_NAME_LEN + 1] = "";
			char*	fk_def;

			innobase_convert_name(tablename, MAX_TABLE_NAME_LEN,
				name, strlen(name), trx->mysql_thd);

			innobase_convert_name(buf, MAX_TABLE_NAME_LEN,
				foreign->id, strlen(foreign->id), trx->mysql_thd);

			fk_def = dict_foreign_def_get((dict_foreign_t*)foreign, trx);

			ib_push_warning(trx, error,
				"Create or Alter table %s with foreign key constraint"
				" failed. Foreign key constraint %s"
				" already exists on data dictionary."
				" Foreign key constraint names need to be unique in database."
				" Error in foreign key definition: %s.",
				tablename, buf, fk_def);
		}

		DBUG_RETURN(error);
	}

	for (ulint i = 0; i < foreign->n_fields; i++) {
		error = dict_create_add_foreign_field_to_dictionary(
			i, name, foreign, trx);

		if (error != DB_SUCCESS) {
			char	buf[MAX_TABLE_NAME_LEN + 1] = "";
			char	tablename[MAX_TABLE_NAME_LEN + 1] = "";
			char*	field=NULL;
			char*	field2=NULL;
			char*	fk_def;

			innobase_convert_name(tablename, MAX_TABLE_NAME_LEN,
				name, strlen(name), trx->mysql_thd);
			innobase_convert_name(buf, MAX_TABLE_NAME_LEN,
				foreign->id, strlen(foreign->id), trx->mysql_thd);
			fk_def = dict_foreign_def_get((dict_foreign_t*)foreign, trx);
			dict_foreign_def_get_fields((dict_foreign_t*)foreign, trx, &field, &field2, i);

			ib_push_warning(trx, error,
				"Create or Alter table %s with foreign key constraint"
				" failed. Error adding foreign  key constraint name %s"
				" fields %s or %s to the dictionary."
				" Error in foreign key definition: %s.",
				tablename, buf, i+1, fk_def);

			DBUG_RETURN(error);
		}
	}

	DBUG_RETURN(error);
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

/** Adds the given set of foreign key objects to the dictionary tables
in the database. This function does not modify the dictionary cache. The
caller must ensure that all foreign key objects contain a valid constraint
name in foreign->id.
@param[in]	local_fk_set	set of foreign key objects, to be added to
the dictionary tables
@param[in]	table		table to which the foreign key objects in
local_fk_set belong to
@param[in,out]	trx		transaction
@return error code or DB_SUCCESS */
dberr_t
dict_create_add_foreigns_to_dictionary(
/*===================================*/
	const dict_foreign_set&	local_fk_set,
	const dict_table_t*	table,
	trx_t*			trx)
{
  ut_ad(dict_sys.locked());

  if (!dict_sys.sys_foreign)
  {
    sql_print_error("InnoDB: Table SYS_FOREIGN not found"
                    " in internal data dictionary");
    return DB_ERROR;
  }

  for (auto fk : local_fk_set)
    if (dberr_t error=
        dict_create_add_foreign_to_dictionary(table->name.m_name, fk, trx))
      return error;

  return DB_SUCCESS;
}
