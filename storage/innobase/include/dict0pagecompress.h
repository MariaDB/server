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
@file include/dict0pagecompress.h
Helper functions for extracting/storing page compression information
to dictionary.

Created 11/12/2013 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

#ifndef dict0pagecompress_h
#define dict0pagecompress_h

/********************************************************************//**
Extract the page compression level from table flags.
@return	page compression level, or 0 if not compressed */
UNIV_INLINE
ulint
dict_tf_get_page_compression_level(
/*===============================*/
	ulint	flags)			/*!< in: flags */
	__attribute__((const));
/********************************************************************//**
Extract the page compression flag from table flags
@return	page compression flag, or false if not compressed */
UNIV_INLINE
ibool
dict_tf_get_page_compression(
/*==========================*/
	ulint	flags)			/*!< in: flags */
	__attribute__((const));

/********************************************************************//**
Check whether the table uses the page compressed page format.
@return	page compression level, or 0 if not compressed */
UNIV_INLINE
ulint
dict_table_page_compression_level(
/*==============================*/
	const dict_table_t*	table)	/*!< in: table */
	__attribute__((const));

/********************************************************************//**
Extract the atomic writes flag from table flags.
@return	true if atomic writes are used, false if not used  */
UNIV_INLINE
atomic_writes_t
dict_tf_get_atomic_writes(
/*======================*/
	ulint	flags)			/*!< in: flags */
	__attribute__((const));

/********************************************************************//**
Check whether the table uses the atomic writes.
@return	true if atomic writes is used, false if not */
UNIV_INLINE
atomic_writes_t
dict_table_get_atomic_writes(
/*=========================*/
	const dict_table_t* table);	/*!< in: table */


#ifndef UNIV_NONINL
#include "dict0pagecompress.ic"
#endif

#endif
