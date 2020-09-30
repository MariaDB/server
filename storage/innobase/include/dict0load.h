/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/dict0load.h
Loads to the memory cache database object definitions
from dictionary tables

Created 4/24/1996 Heikki Tuuri
*******************************************************/

#ifndef dict0load_h
#define dict0load_h

#include "dict0types.h"
#include "trx0types.h"
#include "ut0byte.h"
#include "mem0mem.h"
#include "btr0types.h"

#include <set>


/** Check MAX(SPACE) FROM SYS_TABLES and store it in fil_system.
Open each data file if an encryption plugin has been loaded.

@param spaces  set of tablespace files to open
@param upgrade whether we need to invoke ibuf_upgrade() */
void dict_load_tablespaces(const std::set<uint32_t> *spaces= nullptr,
                           bool upgrade= false);

/** Make sure the data_file_name is saved in dict_table_t if needed.
@param[in,out]	table		Table object */
void dict_get_and_save_data_dir_path(dict_table_t* table);

/***********************************************************************//**
Loads a table object based on the table id.
@return table; NULL if table does not exist */
dict_table_t*
dict_load_table_on_id(
/*==================*/
	table_id_t		table_id,	/*!< in: table id */
	dict_err_ignore_t	ignore_err);	/*!< in: errors to ignore
						when loading the table */
/********************************************************************//**
This function is called when the database is booted.
Loads system table index definitions except for the clustered index which
is added to the dictionary cache at booting before calling this function. */
void
dict_load_sys_table(
/*================*/
	dict_table_t*	table);	/*!< in: system table */

/********************************************************************//**
This function opens a system table, and return the first record.
@return first record of the system table */
const rec_t*
dict_startscan_system(
/*==================*/
	btr_pcur_t*	pcur,		/*!< out: persistent cursor to
					the record */
	mtr_t*		mtr,		/*!< in: the mini-transaction */
	dict_table_t*	table);		/*!< in: system table */
/********************************************************************//**
This function get the next system table record as we scan the table.
@return the record if found, NULL if end of scan. */
const rec_t*
dict_getnext_system(
/*================*/
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor
					to the record */
	mtr_t*		mtr);		/*!< in: the mini-transaction */

/** Load a table definition from a SYS_TABLES record to dict_table_t.
Do not load any columns or indexes.
@param[in,out]	mtr		mini-transaction
@param[in]	uncommitted	whether to use READ UNCOMMITTED isolation level
@param[in]	rec		SYS_TABLES record
@param[out,own]	table		table, or nullptr
@return	error message
@retval	nullptr on success */
const char *dict_load_table_low(mtr_t *mtr, bool uncommitted,
                                const rec_t *rec, dict_table_t **table)
  MY_ATTRIBUTE((nonnull, warn_unused_result));

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
	dict_index_t*	index,		/*!< out: dict_index_t to be
					filled */
	table_id_t*	table_id);	/*!< out: table id */
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
	ulint*		nth_v_col);	/*!< out: if virtual col, this is
					records its sequence number */

/** This function parses a SYS_VIRTUAL record and extract virtual column
information
@param[in,out]	heap		heap memory
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
	ulint*		base_pos);
/********************************************************************//**
This function parses a SYS_FIELDS record and populate a dict_field_t
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
	index_id_t	last_id);	/*!< in: previous index id */
#endif
