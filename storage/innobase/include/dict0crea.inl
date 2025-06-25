/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

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
@file include/dict0crea.ic
Database object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

/** Compose a column number for a virtual column, stored in the "POS" field
of Sys_columns. The column number includes both its virtual column sequence
(the "nth" virtual column) and its actual column position in original table
@param[in]	v_pos		virtual column sequence
@param[in]	col_pos		column position in original table definition
@return composed column position number */
UNIV_INLINE
ulint
dict_create_v_col_pos(
	ulint	v_pos,
	ulint	col_pos)
{
	ut_ad(v_pos <= REC_MAX_N_FIELDS);
	ut_ad(col_pos <= REC_MAX_N_FIELDS);

	return(((v_pos + 1) << 16) + col_pos);
}

/** Get the column number for a virtual column (the column position in
original table), stored in the "POS" field of Sys_columns
@param[in]	pos		virtual column position
@return column position in original table */
UNIV_INLINE
ulint
dict_get_v_col_mysql_pos(
	ulint	pos)
{
	return(pos & 0xFFFF);
}

/** Get a virtual column sequence (the "nth" virtual column) for a
virtual column, stord in the "POS" field of Sys_columns
@param[in]	pos		virtual column position
@return virtual column sequence */
UNIV_INLINE
ulint
dict_get_v_col_pos(
	ulint	pos)
{
	return((pos >> 16) - 1);
}
