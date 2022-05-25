/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2022, MariaDB Corporation.

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
@file dict/dict0load.cc
Loads to the memory cache database object definitions
from dictionary tables

Created 4/24/1996 Heikki Tuuri
*******************************************************/

#include "dict0load.h"

#include "log.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "dict0stats.h"
#include "fsp0file.h"
#include "fts0priv.h"
#include "mach0data.h"
#include "page0page.h"
#include "rem0cmp.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "fts0opt.h"
#include "row0vers.h"

/** Loads a table definition and also all its index definitions.

Loads those foreign key constraints whose referenced table is already in
dictionary cache.  If a foreign key constraint is not loaded, then the
referenced table is pushed into the output stack (fk_tables), if it is not
NULL.  These tables must be subsequently loaded so that all the foreign
key constraints are loaded into memory.

@param[in]	name		Table name in the db/tablename format
@param[in]	ignore_err	Error to be ignored when loading table
				and its index definition
@param[out]	fk_tables	Related table names that must also be
				loaded to ensure that all foreign key
				constraints are loaded.
@return table, possibly with file_unreadable flag set
@retval nullptr if the table does not exist */
static dict_table_t *dict_load_table_one(const span<const char> &name,
                                         dict_err_ignore_t ignore_err,
                                         dict_names_t &fk_tables);

/** Load an index definition from a SYS_INDEXES record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_index_low(
	byte*		table_id,	/*!< in/out: table id (8 bytes),
					an "in" value if mtr
					and "out" when !mtr */
	mem_heap_t*	heap,		/*!< in/out: temporary memory heap */
	const rec_t*	rec,		/*!< in: SYS_INDEXES record */
	mtr_t*		mtr,		/*!< in/out: mini-transaction,
					or nullptr if a pre-allocated
					*index is to be filled in */
	dict_table_t*	table,		/*!< in/out: table, or NULL */
	dict_index_t**	index);		/*!< out,own: index, or NULL */

/** Load a table column definition from a SYS_COLUMNS record to dict_table_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_column_low(
	dict_table_t*	table,		/*!< in/out: table, could be NULL
					if we just populate a dict_column_t
					struct with information from
					a SYS_COLUMNS record */
	mem_heap_t*	heap,		/*!< in/out: memory heap
					for temporary storage */
	dict_col_t*	column,		/*!< out: dict_column_t to fill,
					or NULL if table != NULL */
	table_id_t*	table_id,	/*!< out: table id */
	const char**	col_name,	/*!< out: column name */
	const rec_t*	rec,		/*!< in: SYS_COLUMNS record */
	mtr_t*		mtr,		/*!< in/out: mini-transaction */
	ulint*		nth_v_col);	/*!< out: if not NULL, this
					records the "n" of "nth" virtual
					column */

/** Load a virtual column "mapping" (to base columns) information
from a SYS_VIRTUAL record
@param[in,out]	table		table
@param[in,out]	column		mapped base column's dict_column_t
@param[in,out]	table_id	table id
@param[in,out]	pos		virtual column position
@param[in,out]	base_pos	base column position
@param[in]	rec		SYS_VIRTUAL record
@return	error message
@retval	NULL on success */
static
const char*
dict_load_virtual_low(
	dict_table_t*	table,
	dict_col_t**	column,
	table_id_t*	table_id,
	ulint*		pos,
	ulint*		base_pos,
	const rec_t*	rec);

/** Load an index field definition from a SYS_FIELDS record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_field_low(
	byte*		index_id,	/*!< in/out: index id (8 bytes)
					an "in" value if index != NULL
					and "out" if index == NULL */
	dict_index_t*	index,		/*!< in/out: index, could be NULL
					if we just populate a dict_field_t
					struct with information from
					a SYS_FIELDS record */
	dict_field_t*	sys_field,	/*!< out: dict_field_t to be
					filled */
	ulint*		pos,		/*!< out: Field position */
	byte*		last_index_id,	/*!< in: last index id */
	mem_heap_t*	heap,		/*!< in/out: memory heap
					for temporary storage */
	mtr_t*		mtr,		/*!< in/out: mini-transaction */
	const rec_t*	rec);		/*!< in: SYS_FIELDS record */

#ifdef UNIV_DEBUG
/****************************************************************//**
Compare the name of an index column.
@return TRUE if the i'th column of index is 'name'. */
static
ibool
name_of_col_is(
/*===========*/
	const dict_table_t*	table,	/*!< in: table */
	const dict_index_t*	index,	/*!< in: index */
	ulint			i,	/*!< in: index field offset */
	const char*		name)	/*!< in: name to compare to */
{
	ulint	tmp = dict_col_get_no(dict_field_get_col(
					      dict_index_get_nth_field(
						      index, i)));

	return(strcmp(name, dict_table_get_col_name(table, tmp)) == 0);
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
This function gets the next system table record as it scans the table.
@return the next record if found, NULL if end of scan */
static
const rec_t*
dict_getnext_system_low(
/*====================*/
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor to the
					record*/
	mtr_t*		mtr)		/*!< in: the mini-transaction */
{
	rec_t*	rec = NULL;

	while (!rec) {
		btr_pcur_move_to_next_user_rec(pcur, mtr);

		rec = btr_pcur_get_rec(pcur);

		if (!btr_pcur_is_on_user_rec(pcur)) {
			/* end of index */
			btr_pcur_close(pcur);

			return(NULL);
		}
	}

	/* Get a record, let's save the position */
	btr_pcur_store_position(pcur, mtr);

	return(rec);
}

/********************************************************************//**
This function opens a system table, and returns the first record.
@return first record of the system table */
const rec_t*
dict_startscan_system(
/*==================*/
	btr_pcur_t*	pcur,		/*!< out: persistent cursor to
					the record */
	mtr_t*		mtr,		/*!< in: the mini-transaction */
	dict_table_t*	table)		/*!< in: system table */
{
  btr_pcur_open_at_index_side(true, table->indexes.start, BTR_SEARCH_LEAF,
                              pcur, true, 0, mtr);
  const rec_t *rec;
  do
    rec= dict_getnext_system_low(pcur, mtr);
  while (rec && rec_get_deleted_flag(rec, 0));
  return rec;
}

/********************************************************************//**
This function gets the next system table record as it scans the table.
@return the next record if found, NULL if end of scan */
const rec_t*
dict_getnext_system(
/*================*/
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor
					to the record */
	mtr_t*		mtr)		/*!< in: the mini-transaction */
{
	const rec_t*	rec;

	/* Restore the position */
	pcur->restore_position(BTR_SEARCH_LEAF, mtr);

	/* Get the next record */
	do {
		rec = dict_getnext_system_low(pcur, mtr);
	} while (rec && rec_get_deleted_flag(rec, 0));

	return(rec);
}

/********************************************************************//**
This function parses a SYS_INDEXES record and populate a dict_index_t
structure with the information from the record. For detail information
about SYS_INDEXES fields, please refer to dict_boot() function.
@return error message, or NULL on success */
const char*
dict_process_sys_indexes_rec(
/*=========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_INDEXES rec */
	dict_index_t*	index,		/*!< out: index to be filled */
	table_id_t*	table_id)	/*!< out: index table id */
{
	const char*	err_msg;
	byte		buf[8];

	ut_d(index->is_dummy = true);
	ut_d(index->in_instant_init = false);

	/* Parse the record, and get "dict_index_t" struct filled */
	err_msg = dict_load_index_low(buf, heap, rec, nullptr, nullptr, &index);

	*table_id = mach_read_from_8(buf);

	return(err_msg);
}

/********************************************************************//**
This function parses a SYS_COLUMNS record and populate a dict_column_t
structure with the information from the record.
@return error message, or NULL on success */
const char*
dict_process_sys_columns_rec(
/*=========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_COLUMNS rec */
	dict_col_t*	column,		/*!< out: dict_col_t to be filled */
	table_id_t*	table_id,	/*!< out: table id */
	const char**	col_name,	/*!< out: column name */
	ulint*		nth_v_col)	/*!< out: if virtual col, this is
					record's sequence number */
{
	const char*	err_msg;

	/* Parse the record, and get "dict_col_t" struct filled */
	err_msg = dict_load_column_low(NULL, heap, column,
				       table_id, col_name, rec, nullptr,
				       nth_v_col);

	return(err_msg);
}

/** This function parses a SYS_VIRTUAL record and extracts virtual column
information
@param[in]	rec		current SYS_COLUMNS rec
@param[in,out]	table_id	table id
@param[in,out]	pos		virtual column position
@param[in,out]	base_pos	base column position
@return error message, or NULL on success */
const char*
dict_process_sys_virtual_rec(
	const rec_t*	rec,
	table_id_t*	table_id,
	ulint*		pos,
	ulint*		base_pos)
{
  return dict_load_virtual_low(nullptr, nullptr, table_id, pos, base_pos, rec);
}

/********************************************************************//**
This function parses a SYS_FIELDS record and populates a dict_field_t
structure with the information from the record.
@return error message, or NULL on success */
const char*
dict_process_sys_fields_rec(
/*========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_FIELDS rec */
	dict_field_t*	sys_field,	/*!< out: dict_field_t to be
					filled */
	ulint*		pos,		/*!< out: Field position */
	index_id_t*	index_id,	/*!< out: current index id */
	index_id_t	last_id)	/*!< in: previous index id */
{
	byte		buf[8];
	byte		last_index_id[8];
	const char*	err_msg;

	mach_write_to_8(last_index_id, last_id);

	err_msg = dict_load_field_low(buf, NULL, sys_field,
				      pos, last_index_id, heap, nullptr, rec);

	*index_id = mach_read_from_8(buf);

	return(err_msg);

}

/********************************************************************//**
This function parses a SYS_FOREIGN record and populate a dict_foreign_t
structure with the information from the record. For detail information
about SYS_FOREIGN fields, please refer to dict_load_foreign() function.
@return error message, or NULL on success */
const char*
dict_process_sys_foreign_rec(
/*=========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_FOREIGN rec */
	dict_foreign_t*	foreign)	/*!< out: dict_foreign_t struct
					to be filled */
{
	ulint		len;
	const byte*	field;

	if (rec_get_deleted_flag(rec, 0)) {
		return("delete-marked record in SYS_FOREIGN");
	}

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_FOREIGN) {
		return("wrong number of columns in SYS_FOREIGN record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__ID, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
err_len:
		return("incorrect column length in SYS_FOREIGN");
	}

	/* This receives a dict_foreign_t* that points to a stack variable.
	So dict_foreign_free(foreign) is not used as elsewhere.
	Since the heap used here is freed elsewhere, foreign->heap
	is not assigned. */
	foreign->id = mem_heap_strdupl(heap, (const char*) field, len);

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_FOREIGN__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_FOREIGN__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	/* The _lookup versions of the referenced and foreign table names
	 are not assigned since they are not used in this dict_foreign_t */

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__FOR_NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}
	foreign->foreign_table_name = mem_heap_strdupl(
		heap, (const char*) field, len);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__REF_NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}
	foreign->referenced_table_name = mem_heap_strdupl(
		heap, (const char*) field, len);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__N_COLS, &len);
	if (len != 4) {
		goto err_len;
	}
	uint32_t n_fields_and_type = mach_read_from_4(field);

	foreign->type = n_fields_and_type >> 24 & ((1U << 6) - 1);
	foreign->n_fields = n_fields_and_type & dict_index_t::MAX_N_FIELDS;

	return(NULL);
}

/********************************************************************//**
This function parses a SYS_FOREIGN_COLS record and extract necessary
information from the record and return to caller.
@return error message, or NULL on success */
const char*
dict_process_sys_foreign_col_rec(
/*=============================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_FOREIGN_COLS rec */
	const char**	name,		/*!< out: foreign key constraint name */
	const char**	for_col_name,	/*!< out: referencing column name */
	const char**	ref_col_name,	/*!< out: referenced column name
					in referenced table */
	ulint*		pos)		/*!< out: column position */
{
	ulint		len;
	const byte*	field;

	if (rec_get_deleted_flag(rec, 0)) {
		return("delete-marked record in SYS_FOREIGN_COLS");
	}

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_FOREIGN_COLS) {
		return("wrong number of columns in SYS_FOREIGN_COLS record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN_COLS__ID, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
err_len:
		return("incorrect column length in SYS_FOREIGN_COLS");
	}
	*name = mem_heap_strdupl(heap, (char*) field, len);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN_COLS__POS, &len);
	if (len != 4) {
		goto err_len;
	}
	*pos = mach_read_from_4(field);

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_FOREIGN_COLS__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_FOREIGN_COLS__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN_COLS__FOR_COL_NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}
	*for_col_name = mem_heap_strdupl(heap, (char*) field, len);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN_COLS__REF_COL_NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}
	*ref_col_name = mem_heap_strdupl(heap, (char*) field, len);

	return(NULL);
}

/** Check the validity of a SYS_TABLES record
Make sure the fields are the right length and that they
do not contain invalid contents.
@param[in]	rec	SYS_TABLES record
@return error message, or NULL on success */
static
const char*
dict_sys_tables_rec_check(
	const rec_t*	rec)
{
	const byte*	field;
	ulint		len;

	ut_ad(dict_sys.locked());

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_TABLES) {
		return("wrong number of columns in SYS_TABLES record");
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
err_len:
		return("incorrect column length in SYS_TABLES");
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(rec, DICT_FLD__SYS_TABLES__ID, &len);
	if (len != 8) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__N_COLS, &len);
	if (field == NULL || len != 4) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(rec, DICT_FLD__SYS_TABLES__TYPE, &len);
	if (len != 4) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__MIX_ID, &len);
	if (len != 8) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__MIX_LEN, &len);
	if (field == NULL || len != 4) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_TABLES__CLUSTER_ID, &len);
	if (len != UNIV_SQL_NULL) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__SPACE, &len);
	if (field == NULL || len != 4) {
		goto err_len;
	}

	return(NULL);
}

/** Check if SYS_TABLES.TYPE is valid
@param[in]	type		SYS_TABLES.TYPE
@param[in]	not_redundant	whether ROW_FORMAT=REDUNDANT is not used
@return	whether the SYS_TABLES.TYPE value is valid */
static
bool
dict_sys_tables_type_valid(ulint type, bool not_redundant)
{
	/* The DATA_DIRECTORY flag can be assigned fully independently
	of all other persistent table flags. */
	type &= ~DICT_TF_MASK_DATA_DIR;

	if (type == 1) {
		return(true); /* ROW_FORMAT=REDUNDANT or ROW_FORMAT=COMPACT */
	}

	if (!(type & 1)) {
		/* For ROW_FORMAT=REDUNDANT and ROW_FORMAT=COMPACT,
		SYS_TABLES.TYPE=1. Else, it is the same as
		dict_table_t::flags, and the least significant bit
		would be set. So, the bit never can be 0. */
		return(false);
	}

	if (!not_redundant) {
		/* SYS_TABLES.TYPE must be 1 or 1|DICT_TF_MASK_NO_ROLLBACK
		for ROW_FORMAT=REDUNDANT. */
		return !(type & ~(1U | DICT_TF_MASK_NO_ROLLBACK));
	}

	if (type >= 1U << DICT_TF_POS_UNUSED) {
		/* Some unknown bits are set. */
		return(false);
	}

	return(dict_tf_is_valid_not_redundant(type));
}

/** Convert SYS_TABLES.TYPE to dict_table_t::flags.
@param[in]	type		SYS_TABLES.TYPE
@param[in]	not_redundant	whether ROW_FORMAT=REDUNDANT is not used
@return	table flags */
static
uint32_t dict_sys_tables_type_to_tf(uint32_t type, bool not_redundant)
{
	ut_ad(dict_sys_tables_type_valid(type, not_redundant));
	uint32_t flags = not_redundant ? 1 : 0;

	/* ZIP_SSIZE, ATOMIC_BLOBS, DATA_DIR, PAGE_COMPRESSION,
	PAGE_COMPRESSION_LEVEL are the same. */
	flags |= type & (DICT_TF_MASK_ZIP_SSIZE
			 | DICT_TF_MASK_ATOMIC_BLOBS
			 | DICT_TF_MASK_DATA_DIR
			 | DICT_TF_MASK_PAGE_COMPRESSION
			 | DICT_TF_MASK_PAGE_COMPRESSION_LEVEL
			 | DICT_TF_MASK_NO_ROLLBACK);

	ut_ad(dict_tf_is_valid(flags));
	return(flags);
}

/** Outcome of dict_sys_tables_rec_read() */
enum table_read_status { READ_OK= 0, READ_ERROR, READ_NOT_FOUND };

/** Read and return 5 integer fields from a SYS_TABLES record.
@param[in]	rec		A record of SYS_TABLES
@param[in]	mtr		mini-transaction
@param[out]	table_id	Pointer to the table_id for this table
@param[out]	space_id	Pointer to the space_id for this table
@param[out]	n_cols		Pointer to number of columns for this table.
@param[out]	flags		Pointer to table flags
@param[out]	flags2		Pointer to table flags2
@param[out]	trx_id		DB_TRX_ID of the committed SYS_TABLES record,
				or nullptr to perform READ UNCOMMITTED
@return whether the record was read correctly */
MY_ATTRIBUTE((warn_unused_result))
static
table_read_status
dict_sys_tables_rec_read(
	const rec_t*		rec,
	mtr_t*			mtr,
	table_id_t*		table_id,
	uint32_t*		space_id,
	uint32_t*		n_cols,
	uint32_t*		flags,
	uint32_t*		flags2,
	trx_id_t*		trx_id)
{
	const byte*	field;
	ulint		len;
	mem_heap_t*	heap = nullptr;

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &len);
	ut_ad(len == 6 || len == UNIV_SQL_NULL);
	trx_id_t id = len == 6 ? trx_read_trx_id(field) : 0;
	if (id && trx_sys.find(nullptr, id, false)) {
		const auto savepoint = mtr->get_savepoint();
		heap = mem_heap_create(1024);
		dict_index_t* index = UT_LIST_GET_FIRST(
			dict_sys.sys_tables->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!rec) {
			mem_heap_free(heap);
			return READ_NOT_FOUND;
		}
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_TABLES__DB_TRX_ID, &len);
		if (UNIV_UNLIKELY(len != 6)) {
			mem_heap_free(heap);
			return READ_ERROR;
		}
		id = trx_read_trx_id(field);
	}

	if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(id);
		if (trx_id) {
			return READ_NOT_FOUND;
		}
	}

	if (trx_id) {
		*trx_id = id;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__ID, &len);
	ut_ad(len == 8);
	*table_id = static_cast<table_id_t>(mach_read_from_8(field));

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__SPACE, &len);
	ut_ad(len == 4);
	*space_id = mach_read_from_4(field);

	/* Read the 4 byte flags from the TYPE field */
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__TYPE, &len);
	ut_a(len == 4);
	uint32_t type = mach_read_from_4(field);

	/* Handle MDEV-12873 InnoDB SYS_TABLES.TYPE incompatibility
	for PAGE_COMPRESSED=YES in MariaDB 10.2.2 to 10.2.6.

	MariaDB 10.2.2 introduced the SHARED_SPACE flag from MySQL 5.7,
	shifting the flags PAGE_COMPRESSION, PAGE_COMPRESSION_LEVEL,
	ATOMIC_WRITES (repurposed to NO_ROLLBACK in 10.3.1) by one bit.
	The SHARED_SPACE flag would always
	be written as 0 by MariaDB, because MariaDB does not support
	CREATE TABLESPACE or CREATE TABLE...TABLESPACE for InnoDB.

	So, instead of the bits AALLLLCxxxxxxx we would have
	AALLLLC0xxxxxxx if the table was created with MariaDB 10.2.2
	to 10.2.6. (AA=ATOMIC_WRITES, LLLL=PAGE_COMPRESSION_LEVEL,
	C=PAGE_COMPRESSED, xxxxxxx=7 bits that were not moved.)

	The case LLLLC=00000 is not a problem. The problem is the case
	AALLLL10DB00001 where D is the (mostly ignored) DATA_DIRECTORY
	flag and B is the ATOMIC_BLOBS flag (1 for ROW_FORMAT=DYNAMIC
	and 0 for ROW_FORMAT=COMPACT in this case). Other low-order
	bits must be so, because PAGE_COMPRESSED=YES is only allowed
	for ROW_FORMAT=DYNAMIC and ROW_FORMAT=COMPACT, not for
	ROW_FORMAT=REDUNDANT or ROW_FORMAT=COMPRESSED.

	Starting with MariaDB 10.2.4, the flags would be
	00LLLL10DB00001, because ATOMIC_WRITES is always written as 0.

	We will concentrate on the PAGE_COMPRESSION_LEVEL and
	PAGE_COMPRESSED=YES. PAGE_COMPRESSED=NO implies
	PAGE_COMPRESSION_LEVEL=0, and in that case all the affected
	bits will be 0. For PAGE_COMPRESSED=YES, the values 1..9 are
	allowed for PAGE_COMPRESSION_LEVEL. That is, we must interpret
	the bits AALLLL10DB00001 as AALLLL1DB00001.

	If someone created a table in MariaDB 10.2.2 or 10.2.3 with
	the attribute ATOMIC_WRITES=OFF (value 2) and without
	PAGE_COMPRESSED=YES or PAGE_COMPRESSION_LEVEL, that should be
	rejected. The value ATOMIC_WRITES=ON (1) would look like
	ATOMIC_WRITES=OFF, but it would be ignored starting with
	MariaDB 10.2.4. */
	compile_time_assert(DICT_TF_POS_PAGE_COMPRESSION == 7);
	compile_time_assert(DICT_TF_POS_UNUSED == 14);

	if ((type & 0x19f) != 0x101) {
		/* The table cannot have been created with MariaDB
		10.2.2 to 10.2.6, because they would write the
		low-order bits of SYS_TABLES.TYPE as 0b10xx00001 for
		PAGE_COMPRESSED=YES. No adjustment is applicable. */
	} else if (type >= 3 << 13) {
		/* 10.2.2 and 10.2.3 write ATOMIC_WRITES less than 3,
		and no other flags above that can be set for the
		SYS_TABLES.TYPE to be in the 10.2.2..10.2.6 format.
		This would in any case be invalid format for 10.2 and
		earlier releases. */
		ut_ad(!dict_sys_tables_type_valid(type, true));
	} else {
		/* SYS_TABLES.TYPE is of the form AALLLL10DB00001.  We
		must still validate that the LLLL bits are between 0
		and 9 before we can discard the extraneous 0 bit. */
		ut_ad(!DICT_TF_GET_PAGE_COMPRESSION(type));

		if ((((type >> 9) & 0xf) - 1) < 9) {
			ut_ad(DICT_TF_GET_PAGE_COMPRESSION_LEVEL(type) & 1);

			type = (type & 0x7fU) | (type >> 1 & ~0x7fU);

			ut_ad(DICT_TF_GET_PAGE_COMPRESSION(type));
			ut_ad(DICT_TF_GET_PAGE_COMPRESSION_LEVEL(type) >= 1);
			ut_ad(DICT_TF_GET_PAGE_COMPRESSION_LEVEL(type) <= 9);
		} else {
			ut_ad(!dict_sys_tables_type_valid(type, true));
		}
	}

	/* The low order bit of SYS_TABLES.TYPE is always set to 1. But in
	dict_table_t::flags the low order bit is used to determine if the
	ROW_FORMAT=REDUNDANT (0) or anything else (1).
	Read the 4 byte N_COLS field and look at the high order bit.  It
	should be set for COMPACT and later.  It should not be set for
	REDUNDANT. */
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_TABLES__N_COLS, &len);
	ut_a(len == 4);
	*n_cols = mach_read_from_4(field);

	const bool not_redundant = 0 != (*n_cols & DICT_N_COLS_COMPACT);

	if (!dict_sys_tables_type_valid(type, not_redundant)) {
		sql_print_error("InnoDB: Table %.*s in InnoDB"
				" data dictionary contains invalid flags."
				" SYS_TABLES.TYPE=" UINT32PF
				" SYS_TABLES.N_COLS=" UINT32PF,
				int(rec_get_field_start_offs(rec, 1)), rec,
				type, *n_cols);
err_exit:
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
		return READ_ERROR;
	}

	*flags = dict_sys_tables_type_to_tf(type, not_redundant);

	/* For tables created before MySQL 4.1, there may be
	garbage in SYS_TABLES.MIX_LEN where flags2 are found. Such tables
	would always be in ROW_FORMAT=REDUNDANT which do not have the
	high bit set in n_cols, and flags would be zero.
	MySQL 4.1 was the first version to support innodb_file_per_table,
	that is, *space_id != 0. */
	if (not_redundant || *space_id != 0 || *n_cols & DICT_N_COLS_COMPACT) {

		/* Get flags2 from SYS_TABLES.MIX_LEN */
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_TABLES__MIX_LEN, &len);
		*flags2 = mach_read_from_4(field);

		if (!dict_tf2_is_valid(*flags, *flags2)) {
			sql_print_error("InnoDB: Table %.*s in InnoDB"
					" data dictionary"
					" contains invalid flags."
					" SYS_TABLES.TYPE=" UINT32PF
					" SYS_TABLES.MIX_LEN=" UINT32PF,
					int(rec_get_field_start_offs(rec, 1)),
					rec,
					type, *flags2);
			goto err_exit;
		}

		/* DICT_TF2_FTS will be set when indexes are being loaded */
		*flags2 &= ~DICT_TF2_FTS;

		/* Now that we have used this bit, unset it. */
		*n_cols &= ~DICT_N_COLS_COMPACT;
	} else {
		*flags2 = 0;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return READ_OK;
}

/** Load and check each non-predefined tablespace mentioned in SYS_TABLES.
Search SYS_TABLES and check each tablespace mentioned that has not
already been added to the fil_system.  If it is valid, add it to the
file_system list.
@return the highest space ID found. */
static uint32_t dict_check_sys_tables()
{
	uint32_t	max_space_id = 0;
	btr_pcur_t	pcur;
	mtr_t		mtr;

	DBUG_ENTER("dict_check_sys_tables");

	ut_ad(dict_sys.locked());

	mtr_start(&mtr);

	for (const rec_t *rec = dict_startscan_system(&pcur, &mtr,
						      dict_sys.sys_tables);
	     rec; rec = dict_getnext_system_low(&pcur, &mtr)) {
		ulint		len;
		table_id_t	table_id;
		uint32_t	space_id;
		uint32_t	n_cols;
		uint32_t	flags;
		uint32_t	flags2;

		/* If a table record is not useable, ignore it and continue
		on to the next record. Error messages were logged. */
		if (dict_sys_tables_rec_check(rec)) {
			continue;
		}

		const char *field = reinterpret_cast<const char*>(
			rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__NAME,
					      &len));

		DBUG_PRINT("dict_check_sys_tables",
			   ("name: %*.s", static_cast<int>(len), field));

		if (dict_sys_tables_rec_read(rec, &mtr, &table_id, &space_id,
					     &n_cols, &flags, &flags2, nullptr)
		    != READ_OK
		    || space_id == TRX_SYS_SPACE) {
			continue;
		}

		if (flags2 & DICT_TF2_DISCARDED) {
			sql_print_information("InnoDB: Ignoring tablespace"
					      " for %.*s because "
					      "the DISCARD flag is set",
					      static_cast<int>(len), field);
			continue;
		}

		/* For tables or partitions using .ibd files, the flag
		DICT_TF2_USE_FILE_PER_TABLE was not set in MIX_LEN
		before MySQL 5.6.5. The flag should not have been
		introduced in persistent storage. MariaDB will keep
		setting the flag when writing SYS_TABLES entries for
		newly created or rebuilt tables or partitions, but
		will otherwise ignore the flag. */

		if (fil_space_for_table_exists_in_mem(space_id, flags)) {
			continue;
		}

		const span<const char> name{field, len};

		char*	filepath = fil_make_filepath(nullptr, name,
						     IBD, false);

		const bool not_dropped{!rec_get_deleted_flag(rec, 0)};

		/* Check that the .ibd file exists. */
		if (fil_ibd_open(not_dropped, FIL_TYPE_TABLESPACE,
				 space_id, dict_tf_to_fsp_flags(flags),
				 name, filepath)) {
		} else if (!not_dropped) {
		} else if (srv_operation == SRV_OPERATION_NORMAL
			   && srv_start_after_restore
			   && srv_force_recovery < SRV_FORCE_NO_BACKGROUND
			   && dict_table_t::is_temporary_name(filepath)) {
			/* Mariabackup will not copy files whose
			names start with #sql-. This table ought to
			be dropped by drop_garbage_tables_after_restore()
			a little later. */
		} else {
			sql_print_warning("InnoDB: Ignoring tablespace for"
					  " %.*s because it"
					  " could not be opened.",
					  static_cast<int>(len), field);
		}

		max_space_id = ut_max(max_space_id, space_id);

		ut_free(filepath);
	}

	mtr_commit(&mtr);

	DBUG_RETURN(max_space_id);
}

/** Check each tablespace found in the data dictionary.
Then look at each table defined in SYS_TABLES that has a space_id > 0
to find all the file-per-table tablespaces.

In a crash recovery we already have some tablespace objects created from
processing the REDO log. We will compare the
space_id information in the data dictionary to what we find in the
tablespace file. In addition, more validation will be done if recovery
was needed and force_recovery is not set.

We also scan the biggest space id, and store it to fil_system. */
void dict_check_tablespaces_and_store_max_id()
{
	mtr_t	mtr;

	DBUG_ENTER("dict_check_tablespaces_and_store_max_id");

	dict_sys.lock(SRW_LOCK_CALL);

	/* Initialize the max space_id from sys header */
	mtr.start();
	uint32_t max_space_id = mach_read_from_4(DICT_HDR_MAX_SPACE_ID
						 + DICT_HDR
						 + dict_hdr_get(&mtr)
						 ->page.frame);
	mtr.commit();

	fil_set_max_space_id_if_bigger(max_space_id);

	/* Open all tablespaces referenced in SYS_TABLES. */
	max_space_id = dict_check_sys_tables();
	fil_set_max_space_id_if_bigger(max_space_id);

	dict_sys.unlock();

	DBUG_VOID_RETURN;
}

/** Error message for a delete-marked record in dict_load_column_low() */
static const char *dict_load_column_del= "delete-marked record in SYS_COLUMNS";
static const char *dict_load_column_none= "SYS_COLUMNS record not found";

/** Load a table column definition from a SYS_COLUMNS record to dict_table_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_column_low(
	dict_table_t*	table,		/*!< in/out: table, could be NULL
					if we just populate a dict_column_t
					struct with information from
					a SYS_COLUMNS record */
	mem_heap_t*	heap,		/*!< in/out: memory heap
					for temporary storage */
	dict_col_t*	column,		/*!< out: dict_column_t to fill,
					or NULL if table != NULL */
	table_id_t*	table_id,	/*!< out: table id */
	const char**	col_name,	/*!< out: column name */
	const rec_t*	rec,		/*!< in: SYS_COLUMNS record */
	mtr_t*		mtr,		/*!< in/out: mini-transaction */
	ulint*		nth_v_col)	/*!< out: if not NULL, this
					records the "n" of "nth" virtual
					column */
{
	char*		name;
	const byte*	field;
	ulint		len;
	ulint		mtype;
	ulint		prtype;
	ulint		col_len;
	ulint		pos;
	ulint		num_base;

	ut_ad(!table == !!column);

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_COLUMNS) {
		return("wrong number of columns in SYS_COLUMNS record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__TABLE_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_COLUMNS");
	}

	if (table_id) {
		*table_id = mach_read_from_8(field);
	} else if (table->id != mach_read_from_8(field)) {
		return dict_load_column_none;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__POS, &len);
	if (len != 4) {
		goto err_len;
	}

	pos = mach_read_from_4(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);

	if (trx_id && mtr && trx_sys.find(nullptr, trx_id, false)) {
		const auto savepoint = mtr->get_savepoint();
		dict_index_t* index = UT_LIST_GET_FIRST(
			dict_sys.sys_columns->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!old_vers) {
			return dict_load_column_none;
		}
		ut_ad(!rec_get_deleted_flag(rec, 0));
	}

	if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(trx_id);
		return dict_load_column_del;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_COLUMNS__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}

	*col_name = name = mem_heap_strdupl(heap, (const char*) field, len);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__MTYPE, &len);
	if (len != 4) {
		goto err_len;
	}

	mtype = mach_read_from_4(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__PRTYPE, &len);
	if (len != 4) {
		goto err_len;
	}
	prtype = mach_read_from_4(field);

	if (dtype_get_charset_coll(prtype) == 0
	    && dtype_is_string_type(mtype)) {
		/* The table was created with < 4.1.2. */

		if (dtype_is_binary_string_type(mtype, prtype)) {
			/* Use the binary collation for
			string columns of binary type. */

			prtype = dtype_form_prtype(
				prtype,
				DATA_MYSQL_BINARY_CHARSET_COLL);
		} else {
			/* Use the default charset for
			other than binary columns. */

			prtype = dtype_form_prtype(
				prtype,
				data_mysql_default_charset_coll);
		}
	}

	if (table && table->n_def != pos && !(prtype & DATA_VIRTUAL)) {
		return("SYS_COLUMNS.POS mismatch");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__LEN, &len);
	if (len != 4) {
		goto err_len;
	}
	col_len = mach_read_from_4(field);
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_COLUMNS__PREC, &len);
	if (len != 4) {
		goto err_len;
	}
	num_base = mach_read_from_4(field);

	if (table) {
		if (prtype & DATA_VIRTUAL) {
#ifdef UNIV_DEBUG
			dict_v_col_t*	vcol =
#endif
			dict_mem_table_add_v_col(
				table, heap, name, mtype,
				prtype, col_len,
				dict_get_v_col_mysql_pos(pos), num_base);
			ut_ad(vcol->v_pos == dict_get_v_col_pos(pos));
		} else {
			ut_ad(num_base == 0);
			dict_mem_table_add_col(table, heap, name, mtype,
					       prtype, col_len);
		}

		if (trx_id > table->def_trx_id) {
			table->def_trx_id = trx_id;
		}
	} else {
		dict_mem_fill_column_struct(column, pos, mtype,
					    prtype, col_len);
	}

	/* Report the virtual column number */
	if ((prtype & DATA_VIRTUAL) && nth_v_col != NULL) {
		*nth_v_col = dict_get_v_col_pos(pos);
	}

	return(NULL);
}

/** Error message for a delete-marked record in dict_load_virtual_low() */
static const char *dict_load_virtual_del= "delete-marked record in SYS_VIRTUAL";
static const char *dict_load_virtual_none= "SYS_VIRTUAL record not found";

/** Load a virtual column "mapping" (to base columns) information
from a SYS_VIRTUAL record
@param[in,out]	table		table
@param[in,out]	column		mapped base column's dict_column_t
@param[in,out]	table_id	table id
@param[in,out]	pos		virtual column position
@param[in,out]	base_pos	base column position
@param[in]	rec		SYS_VIRTUAL record
@return	error message
@retval	NULL on success */
static
const char*
dict_load_virtual_low(
	dict_table_t*	table,
	dict_col_t**	column,
	table_id_t*	table_id,
	ulint*		pos,
	ulint*		base_pos,
	const rec_t*	rec)
{
	const byte*	field;
	ulint		len;
	ulint		base;

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_VIRTUAL) {
		return("wrong number of columns in SYS_VIRTUAL record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__TABLE_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_VIRTUAL");
	}

	if (table_id != NULL) {
		*table_id = mach_read_from_8(field);
	} else if (table->id != mach_read_from_8(field)) {
		return dict_load_virtual_none;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__POS, &len);
	if (len != 4) {
		goto err_len;
	}

	if (pos != NULL) {
		*pos = mach_read_from_4(field);
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__BASE_POS, &len);
	if (len != 4) {
		goto err_len;
	}

	base = mach_read_from_4(field);

	if (base_pos != NULL) {
		*base_pos = base;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_VIRTUAL__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_VIRTUAL__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);

	if (trx_id && column && trx_sys.find(nullptr, trx_id, false)) {
		if (!rec_get_deleted_flag(rec, 0)) {
			return dict_load_virtual_none;
                }
	} else if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(trx_id != 0);
		return dict_load_virtual_del;
	}

	if (column != NULL) {
		*column = dict_table_get_nth_col(table, base);
	}

	return(NULL);
}

/********************************************************************//**
Loads definitions for table columns. */
static
void
dict_load_columns(
/*==============*/
	dict_table_t*	table,	/*!< in/out: table */
	mem_heap_t*	heap)	/*!< in/out: memory heap
				for temporary storage */
{
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	const rec_t*	rec;
	byte*		buf;
	ulint		i;
	mtr_t		mtr;
	ulint		n_skipped = 0;

	ut_ad(dict_sys.locked());

	mtr.start();

	dict_index_t* sys_index = dict_sys.sys_columns->indexes.start;
	ut_ad(!dict_sys.sys_columns->not_redundant());

	ut_ad(name_of_col_is(dict_sys.sys_columns, sys_index,
			     DICT_FLD__SYS_COLUMNS__NAME, "NAME"));
	ut_ad(name_of_col_is(dict_sys.sys_columns, sys_index,
			     DICT_FLD__SYS_COLUMNS__PREC, "PREC"));

	tuple = dtuple_create(heap, 1);
	dfield = dtuple_get_nth_field(tuple, 0);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, table->id);

	dfield_set_data(dfield, buf, 8);
	dict_index_copy_types(tuple, sys_index, 1);

	btr_pcur_open_on_user_rec(sys_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);

	ut_ad(table->n_t_cols == static_cast<ulint>(
	      table->n_cols) + static_cast<ulint>(table->n_v_cols));

	for (i = 0;
	     i + DATA_N_SYS_COLS < table->n_t_cols + n_skipped;
	     i++) {
		const char*	err_msg;
		const char*	name = NULL;
		ulint		nth_v_col = ULINT_UNDEFINED;

		rec = btr_pcur_get_rec(&pcur);

		err_msg = btr_pcur_is_on_user_rec(&pcur)
			? dict_load_column_low(table, heap, NULL, NULL,
					       &name, rec, &mtr, &nth_v_col)
			: dict_load_column_none;

		if (!err_msg) {
		} else if (err_msg == dict_load_column_del) {
			n_skipped++;
			goto next_rec;
		} else if (err_msg == dict_load_column_none
			   && strstr(table->name.m_name,
				     "/" TEMP_FILE_PREFIX_INNODB)) {
			break;
		} else {
			ib::fatal() << err_msg << " for table " << table->name;
		}

		/* Note: Currently we have one DOC_ID column that is
		shared by all FTS indexes on a table. And only non-virtual
		column can be used for FULLTEXT index */
		if (innobase_strcasecmp(name,
					FTS_DOC_ID_COL_NAME) == 0
		    && nth_v_col == ULINT_UNDEFINED) {
			dict_col_t*	col;
			/* As part of normal loading of tables the
			table->flag is not set for tables with FTS
			till after the FTS indexes are loaded. So we
			create the fts_t instance here if there isn't
			one already created.

			This case does not arise for table create as
			the flag is set before the table is created. */
			if (table->fts == NULL) {
				table->fts = fts_create(table);
			}

			ut_a(table->fts->doc_col == ULINT_UNDEFINED);

			col = dict_table_get_nth_col(table, i - n_skipped);

			ut_ad(col->len == sizeof(doc_id_t));

			if (col->prtype & DATA_FTS_DOC_ID) {
				DICT_TF2_FLAG_SET(
					table, DICT_TF2_FTS_HAS_DOC_ID);
				DICT_TF2_FLAG_UNSET(
					table, DICT_TF2_FTS_ADD_DOC_ID);
			}

			table->fts->doc_col = i - n_skipped;
		}
next_rec:
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	mtr.commit();
}

/** Loads SYS_VIRTUAL info for one virtual column
@param[in,out]	table		table
@param[in]	nth_v_col	virtual column sequence num
@param[in,out]	v_col		virtual column
@param[in,out]	heap		memory heap
*/
static
void
dict_load_virtual_one_col(
	dict_table_t*	table,
	ulint		nth_v_col,
	dict_v_col_t*	v_col,
	mem_heap_t*	heap)
{
	dict_index_t*	sys_virtual_index;
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	ulint		i = 0;
	mtr_t		mtr;
	ulint		skipped = 0;

	ut_ad(dict_sys.locked());

	if (v_col->num_base == 0) {
		return;
	}

	mtr.start();

	sys_virtual_index = dict_sys.sys_virtual->indexes.start;
	ut_ad(!dict_sys.sys_virtual->not_redundant());

	ut_ad(name_of_col_is(dict_sys.sys_virtual, sys_virtual_index,
			     DICT_FLD__SYS_VIRTUAL__POS, "POS"));

	tuple = dtuple_create(heap, 2);

	/* table ID field */
	dfield = dtuple_get_nth_field(tuple, 0);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, table->id);

	dfield_set_data(dfield, buf, 8);

	/* virtual column pos field */
	dfield = dtuple_get_nth_field(tuple, 1);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 4));
	ulint	vcol_pos = dict_create_v_col_pos(nth_v_col, v_col->m_col.ind);
	mach_write_to_4(buf, vcol_pos);

	dfield_set_data(dfield, buf, 4);

	dict_index_copy_types(tuple, sys_virtual_index, 2);

	btr_pcur_open_on_user_rec(sys_virtual_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);

	for (i = 0; i < unsigned{v_col->num_base} + skipped; i++) {
		ulint		pos;
		const char*	err_msg
			= btr_pcur_is_on_user_rec(&pcur)
			? dict_load_virtual_low(table,
						&v_col->base_col[i - skipped],
						NULL,
					        &pos, NULL,
						btr_pcur_get_rec(&pcur))
			: dict_load_virtual_none;

		if (!err_msg) {
			ut_ad(pos == vcol_pos);
		} else if (err_msg == dict_load_virtual_del) {
			skipped++;
		} else if (err_msg == dict_load_virtual_none
			   && strstr(table->name.m_name,
				     "/" TEMP_FILE_PREFIX_INNODB)) {
			break;
		} else {
			ib::fatal() << err_msg << " for table " << table->name;
		}

		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	mtr.commit();
}

/** Loads info from SYS_VIRTUAL for virtual columns.
@param[in,out]	table	table
@param[in]	heap	memory heap
*/
static
void
dict_load_virtual(
	dict_table_t*	table,
	mem_heap_t*	heap)
{
	for (ulint i = 0; i < table->n_v_cols; i++) {
		dict_v_col_t*	v_col = dict_table_get_nth_v_col(table, i);

		dict_load_virtual_one_col(table, i, v_col, heap);
	}
}

/** Error message for a delete-marked record in dict_load_field_low() */
static const char *dict_load_field_del= "delete-marked record in SYS_FIELDS";

static const char *dict_load_field_none= "SYS_FIELDS record not found";

/** Load an index field definition from a SYS_FIELDS record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_field_low(
	byte*		index_id,	/*!< in/out: index id (8 bytes)
					an "in" value if index != NULL
					and "out" if index == NULL */
	dict_index_t*	index,		/*!< in/out: index, could be NULL
					if we just populate a dict_field_t
					struct with information from
					a SYS_FIELDS record */
	dict_field_t*	sys_field,	/*!< out: dict_field_t to be
					filled */
	ulint*		pos,		/*!< out: Field position */
	byte*		last_index_id,	/*!< in: last index id */
	mem_heap_t*	heap,		/*!< in/out: memory heap
					for temporary storage */
	mtr_t*		mtr,		/*!< in/out: mini-transaction */
	const rec_t*	rec)		/*!< in: SYS_FIELDS record */
{
	const byte*	field;
	ulint		len;
	unsigned	pos_and_prefix_len;
	unsigned	prefix_len;
	bool		descending;
	bool		first_field;
	ulint		position;

	/* Either index or sys_field is supplied, not both */
	ut_ad((!index) != (!sys_field));
	ut_ad((!index) == !mtr);

	if (rec_get_n_fields_old(rec) != DICT_NUM_FIELDS__SYS_FIELDS) {
		return("wrong number of columns in SYS_FIELDS record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__INDEX_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_FIELDS");
	}

	if (!index) {
		ut_a(last_index_id);
		memcpy(index_id, (const char*) field, 8);
		first_field = memcmp(index_id, last_index_id, 8);
	} else {
		first_field = (index->n_def == 0);
		if (memcmp(field, index_id, 8)) {
			return dict_load_field_none;
		}
	}

	/* The next field stores the field position in the index and a
	possible column prefix length if the index field does not
	contain the whole column. The storage format is like this: if
	there is at least one prefix field in the index, then the HIGH
	2 bytes contain the field number (index->n_def) and the low 2
	bytes the prefix length for the field. Otherwise the field
	number (index->n_def) is contained in the 2 LOW bytes. */

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__POS, &len);
	if (len != 4) {
		goto err_len;
	}

	pos_and_prefix_len = mach_read_from_4(field);

	if (index && UNIV_UNLIKELY
	    ((pos_and_prefix_len & 0xFFFFUL) != index->n_def
	     && (pos_and_prefix_len >> 16 & 0xFFFF) != index->n_def)) {
		return("SYS_FIELDS.POS mismatch");
	}

	if (first_field || pos_and_prefix_len > 0xFFFFUL) {
		prefix_len = pos_and_prefix_len & 0x7FFFUL;
		descending = (pos_and_prefix_len & 0x8000UL);
		position = (pos_and_prefix_len & 0xFFFF0000UL)  >> 16;
	} else {
		prefix_len = 0;
		descending = false;
		position = pos_and_prefix_len & 0xFFFFUL;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_FIELDS__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);

	if (!trx_id) {
		ut_ad(!rec_get_deleted_flag(rec, 0));
	} else if (mtr && trx_sys.find(nullptr, trx_id, false)) {
		const auto savepoint = mtr->get_savepoint();
		dict_index_t* sys_field = UT_LIST_GET_FIRST(
			dict_sys.sys_fields->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, sys_field, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, sys_field, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!old_vers || rec_get_deleted_flag(rec, 0)) {
			return dict_load_field_none;
		}
	}

	if (rec_get_deleted_flag(rec, 0)) {
		return(dict_load_field_del);
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FIELDS__COL_NAME, &len);
	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}

	if (index) {
		dict_mem_index_add_field(
			index, mem_heap_strdupl(heap, (const char*) field, len),
			prefix_len, descending);
	} else {
		sys_field->name = mem_heap_strdupl(
			heap, (const char*) field, len);
		sys_field->prefix_len = prefix_len & ((1U << 12) - 1);
		sys_field->descending = descending;
		*pos = position;
	}

	return(NULL);
}

/********************************************************************//**
Loads definitions for index fields.
@return DB_SUCCESS if ok, DB_CORRUPTION if corruption */
static
ulint
dict_load_fields(
/*=============*/
	dict_index_t*	index,	/*!< in/out: index whose fields to load */
	mem_heap_t*	heap)	/*!< in: memory heap for temporary storage */
{
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	byte*		buf;
	ulint		i;
	mtr_t		mtr;
	dberr_t		error;

	ut_ad(dict_sys.locked());

	mtr.start();

	dict_index_t* sys_index = dict_sys.sys_fields->indexes.start;
	ut_ad(!dict_sys.sys_fields->not_redundant());
	ut_ad(name_of_col_is(dict_sys.sys_fields, sys_index,
			     DICT_FLD__SYS_FIELDS__COL_NAME, "COL_NAME"));

	tuple = dtuple_create(heap, 1);
	dfield = dtuple_get_nth_field(tuple, 0);

	buf = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(buf, index->id);

	dfield_set_data(dfield, buf, 8);
	dict_index_copy_types(tuple, sys_index, 1);

	btr_pcur_open_on_user_rec(sys_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);
	for (i = 0; i < index->n_fields; i++) {
		const char *err_msg = btr_pcur_is_on_user_rec(&pcur)
			? dict_load_field_low(buf, index, NULL, NULL, NULL,
					      heap, &mtr,
					      btr_pcur_get_rec(&pcur))
			: dict_load_field_none;

		if (!err_msg) {
		} else if (err_msg == dict_load_field_del) {
			/* There could be delete marked records in
			SYS_FIELDS because SYS_FIELDS.INDEX_ID can be
			updated by ALTER TABLE ADD INDEX. */
		} else {
			if (err_msg != dict_load_field_none
			    || strstr(index->table->name.m_name,
				      "/" TEMP_FILE_PREFIX_INNODB)) {
				ib::error() << err_msg << " for index "
					    << index->name
					    << " of table "
					    << index->table->name;
			}
			error = DB_CORRUPTION;
			goto func_exit;
		}

		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	error = DB_SUCCESS;
func_exit:
	mtr.commit();
	return error;
}

/** Error message for a delete-marked record in dict_load_index_low() */
static const char *dict_load_index_del= "delete-marked record in SYS_INDEXES";
/** Error message for table->id mismatch in dict_load_index_low() */
static const char *dict_load_index_none= "SYS_INDEXES record not found";
/** Error message for SYS_TABLES flags mismatch in dict_load_table_low() */
static const char *dict_load_table_flags= "incorrect flags in SYS_TABLES";

/** Load an index definition from a SYS_INDEXES record to dict_index_t.
@return	error message
@retval	NULL on success */
static
const char*
dict_load_index_low(
	byte*		table_id,	/*!< in/out: table id (8 bytes),
					an "in" value if mtr
					and "out" when !mtr */
	mem_heap_t*	heap,		/*!< in/out: temporary memory heap */
	const rec_t*	rec,		/*!< in: SYS_INDEXES record */
	mtr_t*		mtr,		/*!< in/out: mini-transaction,
					or nullptr if a pre-allocated
					*index is to be filled in */
	dict_table_t*	table,		/*!< in/out: table, or NULL */
	dict_index_t**	index)		/*!< out,own: index, or NULL */
{
	const byte*	field;
	ulint		len;
	index_id_t	id;
	ulint		n_fields;
	ulint		type;
	unsigned	merge_threshold;

	if (mtr) {
		*index = NULL;
	}

	if (rec_get_n_fields_old(rec) == DICT_NUM_FIELDS__SYS_INDEXES) {
		/* MERGE_THRESHOLD exists */
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__MERGE_THRESHOLD, &len);
		switch (len) {
		case 4:
			merge_threshold = mach_read_from_4(field);
			break;
		case UNIV_SQL_NULL:
			merge_threshold = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
			break;
		default:
			return("incorrect MERGE_THRESHOLD length"
			       " in SYS_INDEXES");
		}
	} else if (rec_get_n_fields_old(rec)
		   == DICT_NUM_FIELDS__SYS_INDEXES - 1) {
		/* MERGE_THRESHOLD doesn't exist */

		merge_threshold = DICT_INDEX_MERGE_THRESHOLD_DEFAULT;
	} else {
		return("wrong number of columns in SYS_INDEXES record");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__TABLE_ID, &len);
	if (len != 8) {
err_len:
		return("incorrect column length in SYS_INDEXES");
	}

	if (!mtr) {
		/* We are reading a SYS_INDEXES record. Copy the table_id */
		memcpy(table_id, (const char*) field, 8);
	} else if (memcmp(field, table_id, 8)) {
		/* Caller supplied table_id, verify it is the same
		id as on the index record */
		return dict_load_index_none;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__ID, &len);
	if (len != 8) {
		goto err_len;
	}

	id = mach_read_from_8(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__DB_TRX_ID, &len);
	if (len != DATA_TRX_ID_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}
	rec_get_nth_field_offs_old(
		rec, DICT_FLD__SYS_INDEXES__DB_ROLL_PTR, &len);
	if (len != DATA_ROLL_PTR_LEN && len != UNIV_SQL_NULL) {
		goto err_len;
	}

	const trx_id_t trx_id = trx_read_trx_id(field);
	if (!trx_id) {
		ut_ad(!rec_get_deleted_flag(rec, 0));
	} else if (!mtr) {
	} else if (trx_sys.find(nullptr, trx_id, false)) {
		const auto savepoint = mtr->get_savepoint();
		dict_index_t* sys_index = UT_LIST_GET_FIRST(
			dict_sys.sys_indexes->indexes);
		rec_offs* offsets = rec_get_offsets(
			rec, sys_index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, mtr, sys_index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr->rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!old_vers || rec_get_deleted_flag(rec, 0)) {
			return dict_load_index_none;
		}
	} else if (rec_get_deleted_flag(rec, 0)
		   && rec[8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]
		   != static_cast<byte>(*TEMP_INDEX_PREFIX_STR)
		   && table->def_trx_id < trx_id) {
		table->def_trx_id = trx_id;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__N_FIELDS, &len);
	if (len != 4) {
		goto err_len;
	}
	n_fields = mach_read_from_4(field);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__TYPE, &len);
	if (len != 4) {
		goto err_len;
	}
	type = mach_read_from_4(field);
	if (type & (~0U << DICT_IT_BITS)) {
		return("unknown SYS_INDEXES.TYPE bits");
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);
	if (len != 4) {
		goto err_len;
	}

	ut_d(const auto name_offs =)
	rec_get_nth_field_offs_old(rec, DICT_FLD__SYS_INDEXES__NAME, &len);
	ut_ad(name_offs == 8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

	if (len == 0 || len == UNIV_SQL_NULL) {
		goto err_len;
	}

	if (rec_get_deleted_flag(rec, 0)) {
		return dict_load_index_del;
	}

	char* name = mem_heap_strdupl(heap, reinterpret_cast<const char*>(rec)
				      + (8 + 8 + DATA_TRX_ID_LEN
					 + DATA_ROLL_PTR_LEN),
				      len);

	if (mtr) {
		*index = dict_mem_index_create(table, name, type, n_fields);
	} else {
		dict_mem_fill_index_struct(*index, nullptr, name,
					   type, n_fields);
	}

	(*index)->id = id;
	(*index)->page = mach_read_from_4(field);
	ut_ad((*index)->page);
	(*index)->merge_threshold = merge_threshold & ((1U << 6) - 1);

	return(NULL);
}

/********************************************************************//**
Loads definitions for table indexes. Adds them to the data dictionary
cache.
@return DB_SUCCESS if ok, DB_CORRUPTION if corruption of dictionary
table or DB_UNSUPPORTED if table has unknown index type */
static MY_ATTRIBUTE((nonnull))
dberr_t
dict_load_indexes(
/*==============*/
	dict_table_t*	table,	/*!< in/out: table */
	mem_heap_t*	heap,	/*!< in: memory heap for temporary storage */
	dict_err_ignore_t ignore_err)
				/*!< in: error to be ignored when
				loading the index definition */
{
	dict_index_t*	sys_index;
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	const rec_t*	rec;
	byte		buf[8];
	mtr_t		mtr;
	dberr_t		error = DB_SUCCESS;

	ut_ad(dict_sys.locked());

	mtr.start();

	sys_index = dict_sys.sys_indexes->indexes.start;
	ut_ad(!dict_sys.sys_indexes->not_redundant());
	ut_ad(name_of_col_is(dict_sys.sys_indexes, sys_index,
			     DICT_FLD__SYS_INDEXES__NAME, "NAME"));
	ut_ad(name_of_col_is(dict_sys.sys_indexes, sys_index,
			     DICT_FLD__SYS_INDEXES__PAGE_NO, "PAGE_NO"));

	tuple = dtuple_create(heap, 1);
	dfield = dtuple_get_nth_field(tuple, 0);

	mach_write_to_8(buf, table->id);

	dfield_set_data(dfield, buf, 8);
	dict_index_copy_types(tuple, sys_index, 1);

	btr_pcur_open_on_user_rec(sys_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);
	while (btr_pcur_is_on_user_rec(&pcur)) {
		dict_index_t*	index = NULL;
		const char*	err_msg;

		rec = btr_pcur_get_rec(&pcur);
		if ((ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)
		    && (rec_get_n_fields_old(rec)
			== DICT_NUM_FIELDS__SYS_INDEXES
			/* a record for older SYS_INDEXES table
			(missing merge_threshold column) is acceptable. */
			|| rec_get_n_fields_old(rec)
			   == DICT_NUM_FIELDS__SYS_INDEXES - 1)) {
			const byte*	field;
			ulint		len;
			field = rec_get_nth_field_old(
				rec, DICT_FLD__SYS_INDEXES__NAME, &len);

			if (len != UNIV_SQL_NULL
			    && static_cast<char>(*field)
			    == static_cast<char>(*TEMP_INDEX_PREFIX_STR)) {
				/* Skip indexes whose name starts with
				TEMP_INDEX_PREFIX_STR, because they will
				be dropped by row_merge_drop_temp_indexes()
				during crash recovery. */
				goto next_rec;
			}
		}

		err_msg = dict_load_index_low(buf, heap, rec, &mtr, table,
					      &index);
		ut_ad(!index == !!err_msg);

		if (err_msg == dict_load_index_none) {
			/* We have ran out of index definitions for
			the table. */
			break;
		}

		if (err_msg == dict_load_index_del) {
			goto next_rec;
		} else if (err_msg) {
			ib::error() << err_msg;
			if (ignore_err & DICT_ERR_IGNORE_INDEX) {
				goto next_rec;
			}
			error = DB_CORRUPTION;
			goto func_exit;
		} else if (rec[8 + 8 + DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]
			   == static_cast<byte>(*TEMP_INDEX_PREFIX_STR)) {
			dict_mem_index_free(index);
			goto next_rec;
		} else {
			const trx_id_t id = trx_read_trx_id(rec + 8 + 8);
			if (id > table->def_trx_id) {
				table->def_trx_id = id;
			}
		}

		ut_ad(index);
		ut_ad(!dict_index_is_online_ddl(index));

		/* Check whether the index is corrupted */
		if (ignore_err != DICT_ERR_IGNORE_DROP
		    && index->is_corrupted() && index->is_clust()) {
			dict_mem_index_free(index);
			error = DB_INDEX_CORRUPT;
			goto func_exit;
		}

		if (index->type & DICT_FTS
		    && !dict_table_has_fts_index(table)) {
			/* This should have been created by now. */
			ut_a(table->fts != NULL);
			DICT_TF2_FLAG_SET(table, DICT_TF2_FTS);
		}

		/* We check for unsupported types first, so that the
		subsequent checks are relevant for the supported types. */
		if (index->type & ~(DICT_CLUSTERED | DICT_UNIQUE
				    | DICT_CORRUPT | DICT_FTS
				    | DICT_SPATIAL | DICT_VIRTUAL)) {

			ib::error() << "Unknown type " << index->type
				<< " of index " << index->name
				<< " of table " << table->name;

			error = DB_UNSUPPORTED;
			dict_mem_index_free(index);
			goto func_exit;
		} else if (index->page == FIL_NULL
			   && table->is_readable()
			   && (!(index->type & DICT_FTS))) {
			if (ignore_err != DICT_ERR_IGNORE_DROP) {
				ib::error_or_warn(!(ignore_err
						    & DICT_ERR_IGNORE_INDEX))
					<< "Index " << index->name
					<< " for table " << table->name
					<< " has been freed!";
			}

			if (!(ignore_err & DICT_ERR_IGNORE_INDEX)) {
corrupted:
				dict_mem_index_free(index);
				error = DB_CORRUPTION;
				goto func_exit;
			}
			/* If caller can tolerate this error,
			we will continue to load the index and
			let caller deal with this error. However
			mark the index and table corrupted. We
			only need to mark such in the index
			dictionary cache for such metadata corruption,
			since we would always be able to set it
			when loading the dictionary cache */
			dict_set_corrupted_index_cache_only(index);
		} else if (!dict_index_is_clust(index)
			   && NULL == dict_table_get_first_index(table)) {

			ib::error() << "Trying to load index " << index->name
				<< " for table " << table->name
				<< ", but the first index is not clustered!";

			goto corrupted;
		} else if (dict_is_sys_table(table->id)
			   && (dict_index_is_clust(index)
			       || ((table == dict_sys.sys_tables)
				   && !strcmp("ID_IND", index->name)))) {

			/* The index was created in memory already at booting
			of the database server */
			dict_mem_index_free(index);
		} else {
			dict_load_fields(index, heap);

			/* The data dictionary tables should never contain
			invalid index definitions.  If we ignored this error
			and simply did not load this index definition, the
			.frm file would disagree with the index definitions
			inside InnoDB. */
			if ((error = dict_index_add_to_cache(index,
							     index->page))
			    != DB_SUCCESS) {
				goto func_exit;
			}

#ifdef UNIV_DEBUG
			// The following assertion doesn't hold for FTS indexes
			// as it may have prefix_len=1 with any charset
			if (index->type != DICT_FTS) {
				for (uint i = 0; i < index->n_fields; i++) {
					dict_field_t &f = index->fields[i];
					ut_ad(f.col->mbmaxlen == 0
					      || f.prefix_len
					      % f.col->mbmaxlen == 0);
				}
			}
#endif /* UNIV_DEBUG */
		}
next_rec:
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	if (!dict_table_get_first_index(table)
	    && !(ignore_err & DICT_ERR_IGNORE_INDEX)) {
		ib::warn() << "No indexes found for table " << table->name;
		error = DB_CORRUPTION;
		goto func_exit;
	}

	ut_ad(table->fts_doc_id_index == NULL);

	if (table->fts != NULL) {
		table->fts_doc_id_index = dict_table_get_index_on_name(
			table, FTS_DOC_ID_INDEX_NAME);
	}

	/* If the table contains FTS indexes, populate table->fts->indexes */
	if (dict_table_has_fts_index(table)) {
		ut_ad(table->fts_doc_id_index != NULL);
		/* table->fts->indexes should have been created. */
		ut_a(table->fts->indexes != NULL);
		dict_table_get_all_fts_indexes(table, table->fts->indexes);
	}

func_exit:
	mtr.commit();
	return error;
}

/** Load a table definition from a SYS_TABLES record to dict_table_t.
Do not load any columns or indexes.
@param[in,out]	mtr		mini-transaction
@param[in]	rec		SYS_TABLES record
@param[out,own]	table		table, or nullptr
@return	error message
@retval	nullptr on success */
const char *dict_load_table_low(mtr_t *mtr,
                                const rec_t *rec, dict_table_t **table)
{
	table_id_t	table_id;
	uint32_t	space_id, t_num, flags, flags2;
	ulint		n_cols, n_v_col;
	trx_id_t	trx_id;

	if (const char* error_text = dict_sys_tables_rec_check(rec)) {
		*table = NULL;
		return(error_text);
	}

	if (auto r = dict_sys_tables_rec_read(rec, mtr, &table_id, &space_id,
					      &t_num, &flags, &flags2,
					      &trx_id)) {
		*table = NULL;
		return r == READ_ERROR ? dict_load_table_flags : nullptr;
	}

	dict_table_decode_n_col(t_num, &n_cols, &n_v_col);

	*table = dict_table_t::create(
		span<const char>(reinterpret_cast<const char*>(rec),
				 rec_get_field_start_offs(rec, 1)),
		nullptr, n_cols + n_v_col, n_v_col, flags, flags2);
	(*table)->space_id = space_id;
	(*table)->id = table_id;
	(*table)->file_unreadable = !!(flags2 & DICT_TF2_DISCARDED);
	(*table)->def_trx_id = trx_id;
	return(NULL);
}

/** Make sure the data_file_name is saved in dict_table_t if needed.
@param[in,out]	table		Table object */
void dict_get_and_save_data_dir_path(dict_table_t *table)
{
  ut_ad(!table->is_temporary());
  ut_ad(!table->space || table->space->id == table->space_id);

  if (!table->data_dir_path && table->space_id && table->space)
  {
    const char *filepath= table->space->chain.start->name;
    if (strncmp(fil_path_to_mysql_datadir, filepath,
                strlen(fil_path_to_mysql_datadir)))
    {
      table->lock_mutex_lock();
      table->flags|= 1 << DICT_TF_POS_DATA_DIR & ((1U << DICT_TF_BITS) - 1);
      table->data_dir_path= mem_heap_strdup(table->heap, filepath);
      os_file_make_data_dir_path(table->data_dir_path);
      table->lock_mutex_unlock();
    }
  }
}

/** Opens a tablespace for dict_load_table_one()
@param[in,out]	table		A table that refers to the tablespace to open
@param[in]	ignore_err	Whether to ignore an error. */
UNIV_INLINE
void
dict_load_tablespace(
	dict_table_t*		table,
	dict_err_ignore_t	ignore_err)
{
	ut_ad(!table->is_temporary());
	ut_ad(!table->space);
	ut_ad(table->space_id < SRV_SPACE_ID_UPPER_BOUND);
	ut_ad(fil_system.sys_space);

	if (table->space_id == TRX_SYS_SPACE) {
		table->space = fil_system.sys_space;
		return;
	}

	if (table->flags2 & DICT_TF2_DISCARDED) {
		ib::warn() << "Tablespace for table " << table->name
			<< " is set as discarded.";
		table->file_unreadable = true;
		return;
	}

	/* The tablespace may already be open. */
	table->space = fil_space_for_table_exists_in_mem(table->space_id,
							 table->flags);
	if (table->space) {
		return;
	}

	if (ignore_err >= DICT_ERR_IGNORE_TABLESPACE) {
		table->file_unreadable = true;
		return;
	}

	if (!(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)) {
		ib::error() << "Failed to find tablespace for table "
			<< table->name << " in the cache. Attempting"
			" to load the tablespace with space id "
			<< table->space_id;
	}

	/* Use the remote filepath if needed. This parameter is optional
	in the call to fil_ibd_open(). If not supplied, it will be built
	from the table->name. */
	char* filepath = NULL;
	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		/* This will set table->data_dir_path from fil_system */
		dict_get_and_save_data_dir_path(table);

		if (table->data_dir_path) {
			filepath = fil_make_filepath(
				table->data_dir_path, table->name, IBD, true);
		}
	}

	table->space = fil_ibd_open(
		2, FIL_TYPE_TABLESPACE, table->space_id,
		dict_tf_to_fsp_flags(table->flags),
		{table->name.m_name, strlen(table->name.m_name)}, filepath);

	if (!table->space) {
		/* We failed to find a sensible tablespace file */
		table->file_unreadable = true;
	}

	ut_free(filepath);
}

/** Loads a table definition and also all its index definitions.

Loads those foreign key constraints whose referenced table is already in
dictionary cache.  If a foreign key constraint is not loaded, then the
referenced table is pushed into the output stack (fk_tables), if it is not
NULL.  These tables must be subsequently loaded so that all the foreign
key constraints are loaded into memory.

@param[in]	name		Table name in the db/tablename format
@param[in]	ignore_err	Error to be ignored when loading table
				and its index definition
@param[out]	fk_tables	Related table names that must also be
				loaded to ensure that all foreign key
				constraints are loaded.
@return table, possibly with file_unreadable flag set
@retval nullptr if the table does not exist */
static dict_table_t *dict_load_table_one(const span<const char> &name,
                                         dict_err_ignore_t ignore_err,
                                         dict_names_t &fk_tables)
{
	dberr_t		err;
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	mem_heap_t*	heap;
	dfield_t*	dfield;
	const rec_t*	rec;
	mtr_t		mtr;

	DBUG_ENTER("dict_load_table_one");
	DBUG_PRINT("dict_load_table_one",
		   ("table: %.*s", int(name.size()), name.data()));

	ut_ad(dict_sys.locked());

	heap = mem_heap_create(32000);

	mtr.start();

	dict_index_t *sys_index = dict_sys.sys_tables->indexes.start;
	ut_ad(!dict_sys.sys_tables->not_redundant());
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__ID, "ID"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__N_COLS, "N_COLS"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__TYPE, "TYPE"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__MIX_LEN, "MIX_LEN"));
	ut_ad(name_of_col_is(dict_sys.sys_tables, sys_index,
			     DICT_FLD__SYS_TABLES__SPACE, "SPACE"));

	tuple = dtuple_create(heap, 1);
	dfield = dtuple_get_nth_field(tuple, 0);

	dfield_set_data(dfield, name.data(), name.size());
	dict_index_copy_types(tuple, sys_index, 1);

	btr_pcur_open_on_user_rec(sys_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);
	rec = btr_pcur_get_rec(&pcur);

	if (!btr_pcur_is_on_user_rec(&pcur)) {
		/* Not found */
err_exit:
		mtr.commit();
		mem_heap_free(heap);

		DBUG_RETURN(NULL);
	}

	/* Check if the table name in record is the searched one */
	if (rec_get_field_start_offs(rec, 1) != name.size()
            || memcmp(name.data(), rec, name.size())) {
		goto err_exit;
	}

	dict_table_t* table;
	if (const char* err_msg = dict_load_table_low(&mtr, rec, &table)) {
		if (err_msg != dict_load_table_flags) {
			ib::error() << err_msg;
		}
		goto err_exit;
	}
	if (!table) {
		goto err_exit;
	}

	mtr.commit();

	dict_load_tablespace(table, ignore_err);

	dict_load_columns(table, heap);

	dict_load_virtual(table, heap);

	dict_table_add_system_columns(table, heap);

	table->can_be_evicted = true;
	table->add_to_cache();

	mem_heap_empty(heap);

	ut_ad(dict_tf2_is_valid(table->flags, table->flags2));

	/* If there is no tablespace for the table then we only need to
	load the index definitions. So that we can IMPORT the tablespace
	later. When recovering table locks for resurrected incomplete
	transactions, the tablespace should exist, because DDL operations
	were not allowed while the table is being locked by a transaction. */
	dict_err_ignore_t index_load_err =
		!(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK)
		&& !table->is_readable()
		? DICT_ERR_IGNORE_ALL
		: ignore_err;

	err = dict_load_indexes(table, heap, index_load_err);

	if (err == DB_INDEX_CORRUPT || !UT_LIST_GET_FIRST(table->indexes)) {
		/* Refuse to load the table if the table has a corrupted
		cluster index */
		ut_ad(index_load_err != DICT_ERR_IGNORE_DROP);
		ib::error() << "Refusing to load corrupted table "
			    << table->name;
evict:
		dict_sys.remove(table);
		table = NULL;
		goto func_exit;
	}

	if (err != DB_SUCCESS || !table->is_readable()) {
	} else if (dict_index_t* pk = dict_table_get_first_index(table)) {
		ut_ad(pk->is_primary());
		if (pk->is_corrupted()
		    || pk->page >= table->space->get_size()) {
corrupted:
			table->corrupted = true;
			table->file_unreadable = true;
			err = DB_CORRUPTION;
		} else if (table->space->id
			   && ignore_err == DICT_ERR_IGNORE_DROP) {
			/* Do not bother to load data from .ibd files
			only to delete the .ibd files. */
			goto corrupted;
		} else {
			const page_id_t page_id{table->space->id, pk->page};
			mtr.start();
			buf_block_t* block = buf_page_get(
				page_id, table->space->zip_size(),
				RW_S_LATCH, &mtr);
			const bool corrupted = !block
				|| page_get_space_id(block->page.frame)
				!= page_id.space()
				|| page_get_page_no(block->page.frame)
				!= page_id.page_no()
				|| (mach_read_from_2(FIL_PAGE_TYPE
						    + block->page.frame)
				    != FIL_PAGE_INDEX
				    && mach_read_from_2(FIL_PAGE_TYPE
							+ block->page.frame)
				    != FIL_PAGE_TYPE_INSTANT);
			mtr.commit();
			if (corrupted) {
				goto corrupted;
			}

			if (table->supports_instant()) {
				err = btr_cur_instant_init(table);
			}
		}
	} else {
		ut_ad(ignore_err & DICT_ERR_IGNORE_INDEX);
		if (ignore_err != DICT_ERR_IGNORE_DROP) {
			err = DB_CORRUPTION;
			goto evict;
		}
	}

	/* Initialize table foreign_child value. Its value could be
	changed when dict_load_foreigns() is called below */
	table->fk_max_recusive_level = 0;

	/* If the force recovery flag is set, we open the table irrespective
	of the error condition, since the user may want to dump data from the
	clustered index. However we load the foreign key information only if
	all indexes were loaded. */
	if (!table->is_readable()) {
		/* Don't attempt to load the indexes from disk. */
	} else if (err == DB_SUCCESS) {
		err = dict_load_foreigns(table->name.m_name, nullptr,
					 0, true, ignore_err, fk_tables);

		if (err != DB_SUCCESS) {
			ib::warn() << "Load table " << table->name
				<< " failed, the table has missing"
				" foreign key indexes. Turn off"
				" 'foreign_key_checks' and try again.";
			goto evict;
		} else {
			dict_mem_table_fill_foreign_vcol_set(table);
			table->fk_max_recusive_level = 0;
		}
	}

func_exit:
	mem_heap_free(heap);

	ut_ad(!table
	      || (ignore_err & ~DICT_ERR_IGNORE_FK_NOKEY)
	      || !table->is_readable()
	      || !table->corrupted);

	if (table && table->fts) {
		if (!(dict_table_has_fts_index(table)
		      || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID)
		      || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID))) {
			/* the table->fts could be created in dict_load_column
			when a user defined FTS_DOC_ID is present, but no
			FTS */
			fts_free(table);
		} else if (fts_optimize_wq) {
			fts_optimize_add_table(table);
		} else if (table->can_be_evicted) {
			/* fts_optimize_thread is not started yet.
			So make the table as non-evictable from cache. */
			dict_sys.prevent_eviction(table);
		}
	}

	ut_ad(err != DB_SUCCESS || dict_foreign_set_validate(*table));

	DBUG_RETURN(table);
}

dict_table_t *dict_sys_t::load_table(const span<const char> &name,
                                     dict_err_ignore_t ignore)
{
  if (dict_table_t *table= find_table(name))
    return table;
  dict_names_t fk_list;
  dict_table_t *table= dict_load_table_one(name, ignore, fk_list);
  while (!fk_list.empty())
  {
    const char *f= fk_list.front();
    const span<const char> name{f, strlen(f)};
    if (!find_table(name))
      dict_load_table_one(name, ignore, fk_list);
    fk_list.pop_front();
  }

  return table;
}

/***********************************************************************//**
Loads a table object based on the table id.
@return table; NULL if table does not exist */
dict_table_t*
dict_load_table_on_id(
/*==================*/
	table_id_t		table_id,	/*!< in: table id */
	dict_err_ignore_t	ignore_err)	/*!< in: errors to ignore
						when loading the table */
{
	byte		id_buf[8];
	btr_pcur_t	pcur;
	const byte*	field;
	ulint		len;
	mtr_t		mtr;

	ut_ad(dict_sys.locked());

	/* NOTE that the operation of this function is protected by
	dict_sys.latch, and therefore no deadlocks can occur
	with other dictionary operations. */

	mtr.start();
	/*---------------------------------------------------*/
	/* Get the secondary index based on ID for table SYS_TABLES */
	dict_index_t *sys_table_ids =
		dict_sys.sys_tables->indexes.start->indexes.next;

	dfield_t dfield;
	dtuple_t tuple{
		0,1,1,&dfield,0,nullptr
#ifdef UNIV_DEBUG
		, DATA_TUPLE_MAGIC_N
#endif
	};

	/* Write the table id in byte format to id_buf */
	mach_write_to_8(id_buf, table_id);
	dfield_set_data(&dfield, id_buf, 8);
	dict_index_copy_types(&tuple, sys_table_ids, 1);

	btr_pcur_open_on_user_rec(sys_table_ids, &tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);


	const rec_t* rec = btr_pcur_get_rec(&pcur);
	dict_table_t* table = nullptr;

	if (page_rec_is_user_rec(rec)) {
		/*---------------------------------------------------*/
		/* Now we have the record in the secondary index
		containing the table ID and NAME */
check_rec:
		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_TABLE_IDS__ID, &len);
		ut_ad(len == 8);

		/* Check if the table id in record is the one searched for */
		if (table_id == mach_read_from_8(field)) {
			field = rec_get_nth_field_old(rec,
				DICT_FLD__SYS_TABLE_IDS__NAME, &len);
			table = dict_sys.load_table(
				{reinterpret_cast<const char*>(field),
				 len}, ignore_err);
			if (table && table->id != table_id) {
				ut_ad(rec_get_deleted_flag(rec, 0));
				table = nullptr;
			}
			if (!table) {
				while (btr_pcur_move_to_next(&pcur, &mtr)) {
					rec = btr_pcur_get_rec(&pcur);

					if (page_rec_is_user_rec(rec)) {
						goto check_rec;
					}
				}
			}
		}
	}

	mtr.commit();
	return table;
}

/********************************************************************//**
This function is called when the database is booted. Loads system table
index definitions except for the clustered index which is added to the
dictionary cache at booting before calling this function. */
void
dict_load_sys_table(
/*================*/
	dict_table_t*	table)	/*!< in: system table */
{
	mem_heap_t*	heap;

	ut_ad(dict_sys.locked());

	heap = mem_heap_create(1000);

	dict_load_indexes(table, heap, DICT_ERR_IGNORE_NONE);

	mem_heap_free(heap);
}

/********************************************************************//**
Loads foreign key constraint col names (also for the referenced table).
Members that must be set (and valid) in foreign:
foreign->heap
foreign->n_fields
foreign->id ('\0'-terminated)
Members that will be created and set by this function:
foreign->foreign_col_names[i]
foreign->referenced_col_names[i]
(for i=0..foreign->n_fields-1) */
static void dict_load_foreign_cols(dict_foreign_t *foreign, trx_id_t trx_id)
{
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	const rec_t*	rec;
	const byte*	field;
	ulint		len;
	ulint		i;
	mtr_t		mtr;
	size_t		id_len;

	ut_ad(dict_sys.locked());

	id_len = strlen(foreign->id);

	foreign->foreign_col_names = static_cast<const char**>(
		mem_heap_alloc(foreign->heap,
			       foreign->n_fields * sizeof(void*)));

	foreign->referenced_col_names = static_cast<const char**>(
		mem_heap_alloc(foreign->heap,
			       foreign->n_fields * sizeof(void*)));

	mtr.start();

	dict_index_t* sys_index = dict_sys.sys_foreign_cols->indexes.start;
	ut_ad(!dict_sys.sys_foreign_cols->not_redundant());

	tuple = dtuple_create(foreign->heap, 1);
	dfield = dtuple_get_nth_field(tuple, 0);

	dfield_set_data(dfield, foreign->id, id_len);
	dict_index_copy_types(tuple, sys_index, 1);

	mem_heap_t* heap = nullptr;
	btr_pcur_open_on_user_rec(sys_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);
	for (i = 0; i < foreign->n_fields; i++) {
retry:
		ut_a(btr_pcur_is_on_user_rec(&pcur));

		rec = btr_pcur_get_rec(&pcur);

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_FOREIGN_COLS__DB_TRX_ID, &len);
		ut_a(len == DATA_TRX_ID_LEN);

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_empty(heap);
		}

		const trx_id_t id = trx_read_trx_id(field);
		if (!id) {
		} else if (id != trx_id && trx_sys.find(nullptr, id, false)) {
			const auto savepoint = mtr.get_savepoint();
			rec_offs* offsets = rec_get_offsets(
				rec, sys_index, nullptr, true, ULINT_UNDEFINED,
				&heap);
			const rec_t* old_vers;
			row_vers_build_for_semi_consistent_read(
				nullptr, rec, &mtr, sys_index, &offsets, &heap,
				heap, &old_vers, nullptr);
			mtr.rollback_to_savepoint(savepoint);
			rec = old_vers;
			if (!rec || rec_get_deleted_flag(rec, 0)) {
				goto next;
			}
		}

		if (rec_get_deleted_flag(rec, 0)) {
			ut_ad(id);
next:
			btr_pcur_move_to_next_user_rec(&pcur, &mtr);
			goto retry;
		}

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_FOREIGN_COLS__ID, &len);

		if (len != id_len || memcmp(foreign->id, field, len)) {
			const rec_t*	pos;
			ulint		pos_len;
			const rec_t*	for_col_name;
			ulint		for_col_name_len;
			const rec_t*	ref_col_name;
			ulint		ref_col_name_len;

			pos = rec_get_nth_field_old(
				rec, DICT_FLD__SYS_FOREIGN_COLS__POS,
				&pos_len);

			for_col_name = rec_get_nth_field_old(
				rec, DICT_FLD__SYS_FOREIGN_COLS__FOR_COL_NAME,
				&for_col_name_len);

			ref_col_name = rec_get_nth_field_old(
				rec, DICT_FLD__SYS_FOREIGN_COLS__REF_COL_NAME,
				&ref_col_name_len);

			ib::fatal	sout;

			sout << "Unable to load column names for foreign"
				" key '" << foreign->id
				<< "' because it was not found in"
				" InnoDB internal table SYS_FOREIGN_COLS. The"
				" closest entry we found is:"
				" (ID='";
			sout.write(field, len);
			sout << "', POS=" << mach_read_from_4(pos)
				<< ", FOR_COL_NAME='";
			sout.write(for_col_name, for_col_name_len);
			sout << "', REF_COL_NAME='";
			sout.write(ref_col_name, ref_col_name_len);
			sout << "')";
		}

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_FOREIGN_COLS__POS, &len);
		ut_a(len == 4);
		ut_a(i == mach_read_from_4(field));

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_FOREIGN_COLS__FOR_COL_NAME, &len);
		foreign->foreign_col_names[i] = mem_heap_strdupl(
			foreign->heap, (char*) field, len);

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_FOREIGN_COLS__REF_COL_NAME, &len);
		foreign->referenced_col_names[i] = mem_heap_strdupl(
			foreign->heap, (char*) field, len);

		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	mtr.commit();
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

/***********************************************************************//**
Loads a foreign key constraint to the dictionary cache. If the referenced
table is not yet loaded, it is added in the output parameter (fk_tables).
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
dict_load_foreign(
/*==============*/
	const char*		table_name,	/*!< in: table name */
	const char**		col_names,
				/*!< in: column names, or NULL
				to use foreign->foreign_table->col_names */
	trx_id_t		trx_id,
				/*!< in: current transaction id, or 0 */
	bool			check_recursive,
				/*!< in: whether to record the foreign table
				parent count to avoid unlimited recursive
				load of chained foreign tables */
	bool			check_charsets,
				/*!< in: whether to check charset
				compatibility */
	span<const char>	id,
				/*!< in: foreign constraint id */
	dict_err_ignore_t	ignore_err,
				/*!< in: error to be ignored */
	dict_names_t&	fk_tables)
				/*!< out: the foreign key constraint is added
				to the dictionary cache only if the referenced
				table is already in cache.  Otherwise, the
				foreign key constraint is not added to cache,
				and the referenced table is added to this
				stack. */
{
	dict_foreign_t*	foreign;
	btr_pcur_t	pcur;
	const byte*	field;
	ulint		len;
	mtr_t		mtr;
	dict_table_t*	for_table;
	dict_table_t*	ref_table;
        byte		dtuple_buf[DTUPLE_EST_ALLOC(1)];

	DBUG_ENTER("dict_load_foreign");
	DBUG_PRINT("dict_load_foreign",
		   ("id: '%.*s', check_recursive: %d",
		    int(id.size()), id.data(), check_recursive));

	ut_ad(dict_sys.locked());

	dict_index_t* sys_index = dict_sys.sys_foreign->indexes.start;
	ut_ad(!dict_sys.sys_foreign->not_redundant());

	dtuple_t* tuple = dtuple_create_from_mem(dtuple_buf, sizeof dtuple_buf,
                                                 1, 0);
	dfield_set_data(dtuple_get_nth_field(tuple, 0), id.data(), id.size());
	dict_index_copy_types(tuple, sys_index, 1);

	mtr.start();

	btr_pcur_open_on_user_rec(sys_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);
	const rec_t* rec = btr_pcur_get_rec(&pcur);
	mem_heap_t* heap = nullptr;

	if (!btr_pcur_is_on_user_rec(&pcur)) {
	not_found:
		mtr.commit();
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
		DBUG_RETURN(DB_NOT_FOUND);
	}

	static_assert(DICT_FLD__SYS_FOREIGN__ID == 0, "compatibility");
	field = rec_get_nth_field_old(rec, DICT_FLD__SYS_FOREIGN__ID, &len);

	/* Check if the id in record is the searched one */
	if (len != id.size() || memcmp(id.data(), field, id.size())) {
		goto not_found;
	}

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__DB_TRX_ID, &len);
	ut_a(len == DATA_TRX_ID_LEN);

	const trx_id_t tid = trx_read_trx_id(field);

	if (tid && tid != trx_id && trx_sys.find(nullptr, tid, false)) {
		const auto savepoint = mtr.get_savepoint();
		rec_offs* offsets = rec_get_offsets(
			rec, sys_index, nullptr, true, ULINT_UNDEFINED, &heap);
		const rec_t* old_vers;
		row_vers_build_for_semi_consistent_read(
			nullptr, rec, &mtr, sys_index, &offsets, &heap,
			heap, &old_vers, nullptr);
		mtr.rollback_to_savepoint(savepoint);
		rec = old_vers;
		if (!rec) {
			goto not_found;
		}
	}

	if (rec_get_deleted_flag(rec, 0)) {
		ut_ad(tid);
		goto not_found;
	}

	/* Read the table names and the number of columns associated
	with the constraint */

	foreign = dict_mem_foreign_create();

	uint32_t n_fields_and_type = mach_read_from_4(
		rec_get_nth_field_old(
			rec, DICT_FLD__SYS_FOREIGN__N_COLS, &len));

	ut_a(len == 4);

	/* We store the type in the bits 24..29 of n_fields_and_type. */

	foreign->type = (n_fields_and_type >> 24) & ((1U << 6) - 1);
	foreign->n_fields = n_fields_and_type & dict_index_t::MAX_N_FIELDS;

	foreign->id = mem_heap_strdupl(foreign->heap, id.data(), id.size());

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__FOR_NAME, &len);

	foreign->foreign_table_name = mem_heap_strdupl(
		foreign->heap, (char*) field, len);
	dict_mem_foreign_table_name_lookup_set(foreign, TRUE);

	const size_t foreign_table_name_len = len;
	const size_t table_name_len = strlen(table_name);

	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN__REF_NAME, &len);

	if (!my_charset_latin1.strnncoll(table_name, table_name_len,
					 foreign->foreign_table_name,
					 foreign_table_name_len)) {
	} else if (!check_recursive
		   && !my_charset_latin1.strnncoll(table_name, table_name_len,
						   (const char*) field, len)) {
	} else {
		dict_foreign_free(foreign);
		goto not_found;
	}

	foreign->referenced_table_name = mem_heap_strdupl(
		foreign->heap, (const char*) field, len);
	dict_mem_referenced_table_name_lookup_set(foreign, TRUE);

	mtr.commit();
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	dict_load_foreign_cols(foreign, trx_id);

	ref_table = dict_sys.find_table(
		{foreign->referenced_table_name_lookup,
		 strlen(foreign->referenced_table_name_lookup)});
	for_table = dict_sys.find_table(
		{foreign->foreign_table_name_lookup,
		 strlen(foreign->foreign_table_name_lookup)});

	if (!for_table) {
		/* To avoid recursively loading the tables related through
		the foreign key constraints, the child table name is saved
		here.  The child table will be loaded later, along with its
		foreign key constraint. */

		ut_a(ref_table != NULL);
		fk_tables.push_back(
			mem_heap_strdupl(ref_table->heap,
					 foreign->foreign_table_name_lookup,
					 foreign_table_name_len));

		dict_foreign_remove_from_cache(foreign);
		DBUG_RETURN(DB_SUCCESS);
	}

	ut_a(for_table || ref_table);

	/* Note that there may already be a foreign constraint object in
	the dictionary cache for this constraint: then the following
	call only sets the pointers in it to point to the appropriate table
	and index objects and frees the newly created object foreign.
	Adding to the cache should always succeed since we are not creating
	a new foreign key constraint but loading one from the data
	dictionary. */

	DBUG_RETURN(dict_foreign_add_to_cache(foreign, col_names,
					      check_charsets,
					      ignore_err));
}

/***********************************************************************//**
Loads foreign key constraints where the table is either the foreign key
holder or where the table is referenced by a foreign key. Adds these
constraints to the data dictionary.

The foreign key constraint is loaded only if the referenced table is also
in the dictionary cache.  If the referenced table is not in dictionary
cache, then it is added to the output parameter (fk_tables).

@return DB_SUCCESS or error code */
dberr_t
dict_load_foreigns(
	const char*		table_name,	/*!< in: table name */
	const char**		col_names,	/*!< in: column names, or NULL
						to use table->col_names */
	trx_id_t		trx_id,		/*!< in: DDL transaction id,
						or 0 to check
						recursive load of tables
						chained by FK */
	bool			check_charsets,	/*!< in: whether to check
						charset compatibility */
	dict_err_ignore_t	ignore_err,	/*!< in: error to be ignored */
	dict_names_t&		fk_tables)
						/*!< out: stack of table
						names which must be loaded
						subsequently to load all the
						foreign key constraints. */
{
	ulint		tuple_buf[(DTUPLE_EST_ALLOC(1) + sizeof(ulint) - 1)
				/ sizeof(ulint)];
	btr_pcur_t	pcur;
	dtuple_t*	tuple;
	dfield_t*	dfield;
	mtr_t		mtr;

	DBUG_ENTER("dict_load_foreigns");

	ut_ad(dict_sys.locked());

	if (!dict_sys.sys_foreign || !dict_sys.sys_foreign_cols) {
		if (ignore_err & DICT_ERR_IGNORE_FK_NOKEY) {
			DBUG_RETURN(DB_SUCCESS);
		}
		sql_print_information("InnoDB: No foreign key system tables"
				      " in the database");
		DBUG_RETURN(DB_ERROR);
	}

	ut_ad(!dict_sys.sys_foreign->not_redundant());

	dict_index_t *sec_index = dict_table_get_next_index(
		dict_table_get_first_index(dict_sys.sys_foreign));
	ut_ad(!strcmp(sec_index->fields[0].name, "FOR_NAME"));
	bool check_recursive = !trx_id;

start_load:
	mtr.start();

	tuple = dtuple_create_from_mem(tuple_buf, sizeof(tuple_buf), 1, 0);
	dfield = dtuple_get_nth_field(tuple, 0);

	dfield_set_data(dfield, table_name, strlen(table_name));
	dict_index_copy_types(tuple, sec_index, 1);

	btr_pcur_open_on_user_rec(sec_index, tuple, PAGE_CUR_GE,
				  BTR_SEARCH_LEAF, &pcur, &mtr);
loop:
	const rec_t* rec = btr_pcur_get_rec(&pcur);
	const byte* field;
	const auto maybe_deleted = rec_get_deleted_flag(rec, 0);

	if (!btr_pcur_is_on_user_rec(&pcur)) {
		/* End of index */

		goto load_next_index;
	}

	/* Now we have the record in the secondary index containing a table
	name and a foreign constraint ID */

	ulint len;
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN_FOR_NAME__NAME, &len);

	/* Check if the table name in the record is the one searched for; the
	following call does the comparison in the latin1_swedish_ci
	charset-collation, in a case-insensitive way. */

	if (cmp_data(dfield_get_type(dfield)->mtype,
		     dfield_get_type(dfield)->prtype,
		     false,
		     static_cast<const byte*>(dfield_get_data(dfield)),
		     dfield_get_len(dfield),
		     field, len)) {
		goto load_next_index;
	}

	/* Since table names in SYS_FOREIGN are stored in a case-insensitive
	order, we have to check that the table name matches also in a binary
	string comparison. On Unix, MySQL allows table names that only differ
	in character case.  If lower_case_table_names=2 then what is stored
	may not be the same case, but the previous comparison showed that they
	match with no-case.  */

	if (lower_case_table_names != 2 && memcmp(field, table_name, len)) {
		goto next_rec;
	}

	/* Now we get a foreign key constraint id */
	field = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_FOREIGN_FOR_NAME__ID, &len);

	/* Copy the string because the page may be modified or evicted
	after mtr.commit() below. */
	char	fk_id[MAX_TABLE_NAME_LEN + 1];

	ut_a(len <= MAX_TABLE_NAME_LEN);
	memcpy(fk_id, field, len);

	btr_pcur_store_position(&pcur, &mtr);

	mtr.commit();

	/* Load the foreign constraint definition to the dictionary cache */

	switch (dberr_t err
		= dict_load_foreign(table_name, col_names, trx_id,
				    check_recursive, check_charsets,
				    {fk_id, len}, ignore_err, fk_tables)) {
	case DB_SUCCESS:
		break;
	case DB_NOT_FOUND:
		if (maybe_deleted) {
			break;
		}
		sql_print_error("InnoDB: Cannot load foreign constraint %.*s:"
				" could not find the relevant record in "
				"SYS_FOREIGN", int(len), fk_id);
		/* fall through */
	default:
		ut_free(pcur.old_rec_buf);
		DBUG_RETURN(err);
	}

	mtr.start();
	pcur.restore_position(BTR_SEARCH_LEAF, &mtr);
next_rec:
	btr_pcur_move_to_next_user_rec(&pcur, &mtr);

	goto loop;

load_next_index:
	mtr.commit();
	ut_free(pcur.old_rec_buf);

	sec_index = dict_table_get_next_index(sec_index);

	if (sec_index) {
		/* Switch to scan index on REF_NAME, fk_max_recusive_level
		already been updated when scanning FOR_NAME index, no need to
		update again */
		check_recursive = false;
		goto start_load;
	}

	DBUG_RETURN(DB_SUCCESS);
}
