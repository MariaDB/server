/*****************************************************************************

Copyright (c) 2012, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2019, MariaDB Corporation.

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
@file include/row0import.h
Header file for import tablespace functions.

Created 2012-02-08 by Sunny Bains
*******************************************************/

#ifndef row0import_h
#define row0import_h

#include "dict0types.h"

// Forward declarations
struct trx_t;
struct dict_table_t;
struct row_prebuilt_t;
struct HA_CREATE_INFO;

/*****************************************************************//**
Imports a tablespace. The space id in the .ibd file must match the space id
of the table in the data dictionary.
@return error code or DB_SUCCESS */
dberr_t
row_import_for_mysql(
/*=================*/
	dict_table_t*	table,		/*!< in/out: table */
	row_prebuilt_t*	prebuilt)	/*!< in: prebuilt struct
						in MySQL */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Update the DICT_TF2_DISCARDED flag in SYS_TABLES.MIX_LEN.
@param[in,out]	trx		dictionary transaction
@param[in]	table_id	table identifier
@param[in]	discarded	whether to set or clear the flag
@return DB_SUCCESS or error code */
dberr_t row_import_update_discarded_flag(trx_t* trx, table_id_t table_id,
					 bool discarded)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Update the root page numbers and tablespace ID of a table.
@param[in,out]	trx	dictionary transaction
@param[in,out]	table	persistent table
@param[in]	reset	whether to reset the fields to FIL_NULL
@return DB_SUCCESS or error code */
dberr_t
row_import_update_index_root(trx_t* trx, dict_table_t* table, bool reset)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Prepare the create info to create a new stub table for import.
@param thd          Connection
@param name         Table name, format: "db/table_name".
@param create_info  The create info for creating a stub.
@return ER_ error code
@retval 0 on success */
int prepare_create_stub_for_import(THD *thd, const char *name,
                                   HA_CREATE_INFO& create_info);

#endif /* row0import_h */
