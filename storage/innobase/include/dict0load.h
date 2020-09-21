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

/** enum that defines all system table IDs. @see SYSTEM_TABLE_NAME[] */
enum dict_system_id_t {
	SYS_TABLES = 0,
	SYS_INDEXES,
	SYS_COLUMNS,
	SYS_FIELDS,
	SYS_FOREIGN,
	SYS_FOREIGN_COLS,
	SYS_VIRTUAL,

	/* This must be last item. Defines the number of system tables. */
	SYS_NUM_SYSTEM_TABLES
};

/** Check each tablespace found in the data dictionary.
Then look at each table defined in SYS_TABLES that has a space_id > 0
to find all the file-per-table tablespaces.

In a crash recovery we already have some tablespace objects created from
processing the REDO log. We will compare the
space_id information in the data dictionary to what we find in the
tablespace file. In addition, more validation will be done if recovery
was needed and force_recovery is not set.

We also scan the biggest space id, and store it to fil_system. */
void dict_check_tablespaces_and_store_max_id();

/********************************************************************//**
Finds the first table name in the given database.
@return own: table name, NULL if does not exist; the caller must free
the memory in the string! */
char*
dict_get_first_table_name_in_db(
/*============================*/
	const char*	name);	/*!< in: database name which ends to '/' */

/** Make sure the data_file_name is saved in dict_table_t if needed.
@param[in]	table		Table object
@param[in]	dict_mutex_own	true if dict_sys.mutex is owned already */
void
dict_get_and_save_data_dir_path(
	dict_table_t*	table,
	bool		dict_mutex_own);

/** Loads a table definition and also all its index definitions, and also
the cluster definition if the table is a member in a cluster. Also loads
all foreign key constraints where the foreign key is in the table or where
a foreign key references columns in this table.
@param[in]	name		Table name in the dbname/tablename format
@param[in]	ignore_err	Error to be ignored when loading
				table and its index definition
@return table, NULL if does not exist; if the table is stored in an
.ibd file, but the file does not exist, then we set the file_unreadable
flag in the table object we return. */
dict_table_t* dict_load_table(const char* name, dict_err_ignore_t ignore_err);

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
	dict_system_id_t system_id);	/*!< in: which system table to open */
/********************************************************************//**
This function get the next system table record as we scan the table.
@return the record if found, NULL if end of scan. */
const rec_t*
dict_getnext_system(
/*================*/
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor
					to the record */
	mtr_t*		mtr);		/*!< in: the mini-transaction */
/********************************************************************//**
This function processes one SYS_TABLES record and populate the dict_table_t
struct for the table.
@return error message, or NULL on success */
const char*
dict_process_sys_tables_rec_and_mtr_commit(
/*=======================================*/
	mem_heap_t*	heap,		/*!< in: temporary memory heap */
	const rec_t*	rec,		/*!< in: SYS_TABLES record */
	dict_table_t**	table,		/*!< out: dict_table_t to fill */
	bool		cached,		/*!< in: whether to load from cache */
	mtr_t*		mtr);		/*!< in/out: mini-transaction,
					will be committed */
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
#ifdef WITH_INNODB_FOREIGN_UPGRADE
/********************************************************************//**
This function parses a SYS_FOREIGN record and populate a dict_foreign_t
structure with the information from the record. For detail information
about SYS_FOREIGN fields, please refer to dict_load_foreign() function
@return error message, or NULL on success */
const char*
dict_process_sys_foreign_rec(
/*=========================*/
	mem_heap_t*	heap,		/*!< in/out: heap memory */
	const rec_t*	rec,		/*!< in: current SYS_FOREIGN rec */
	dict_foreign_t*	foreign);	/*!< out: dict_foreign_t to be
					filled */
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
	ulint*		pos);		/*!< out: column position */
#endif /* WITH_INNODB_FOREIGN_UPGRADE */
#endif
