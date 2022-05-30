/*****************************************************************************

Copyright (C) 2013, 2017, MariaDB Corporation. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/******************************************************************//**
@file include/dict0pagecompress.ic
Inline implementation for helper functions for extracting/storing
page compression and atomic writes information to dictionary.

Created 11/12/2013 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

/********************************************************************//**
Extract the page compression level from dict_table_t::flags.
These flags are in memory, so assert that they are valid.
@return	page compression level, or 0 if not compressed */
UNIV_INLINE
ulint
dict_tf_get_page_compression_level(
/*===============================*/
	ulint	flags)	/*!< in: flags */
{
        ulint page_compression_level = DICT_TF_GET_PAGE_COMPRESSION_LEVEL(flags);

	ut_ad(page_compression_level <= 9);

	return(page_compression_level);
}

/********************************************************************//**
Check whether the table uses the page compression page format.
@return	page compression level, or 0 if not compressed */
UNIV_INLINE
ulint
dict_table_page_compression_level(
/*==============================*/
	const dict_table_t*	table)	/*!< in: table */
{
	ut_ad(table);
	ut_ad(dict_tf_get_page_compression(table->flags));

	return(dict_tf_get_page_compression_level(table->flags));
}

/********************************************************************//**
Check whether the table uses the page compression page format.
@return	true if page compressed, false if not */
UNIV_INLINE
ibool
dict_tf_get_page_compression(
/*=========================*/
	ulint	flags)	/*!< in: flags */
{
	return(DICT_TF_GET_PAGE_COMPRESSION(flags));
}

/********************************************************************//**
Check whether the table uses the page compression page format.
@return	true if page compressed, false if not */
UNIV_INLINE
ibool
dict_table_is_page_compressed(
/*==========================*/
	const dict_table_t* table)	/*!< in: table */
{
	return (dict_tf_get_page_compression(table->flags));
}
