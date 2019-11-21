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

/*****************************************************************//**
Update the DICT_TF2_DISCARDED flag in SYS_TABLES.
@return DB_SUCCESS or error code. */
dberr_t
row_import_update_discarded_flag(
/*=============================*/
	trx_t*		trx,			/*!< in/out: transaction that
						covers the update */
	table_id_t	table_id,		/*!< in: Table for which we want
						to set the root table->flags2 */
	bool		discarded,		/*!< in: set MIX_LEN column bit
						to discarded, if true */
	bool		dict_locked)		/*!< in: Set to true if the
						caller already owns the
						dict_sys_t:: mutex. */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Update the root page numbers and tablespace ID of a table.
@param[in,out]	trx	dictionary transaction
@param[in,out]	table	persistent table
@param[in]	reset	whether to reset the fields to FIL_NULL
@return DB_SUCCESS or error code */
dberr_t
row_import_update_index_root(trx_t* trx, dict_table_t* table, bool reset)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#endif /* row0import_h */
