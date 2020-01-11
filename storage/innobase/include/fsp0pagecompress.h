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
@file include/fsp0pagecompress.h
Helper functions for extracting/storing page compression and
atomic writes information to file space.

Created 11/12/2013 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

#ifndef fsp0pagecompress_h
#define fsp0pagecompress_h

/* Supported page compression methods */

#define PAGE_UNCOMPRESSED	0
#define PAGE_ZLIB_ALGORITHM	1
#define PAGE_LZ4_ALGORITHM	2
#define PAGE_LZO_ALGORITHM	3
#define PAGE_LZMA_ALGORITHM	4
#define PAGE_BZIP2_ALGORITHM	5
#define PAGE_SNAPPY_ALGORITHM	6
#define PAGE_ALGORITHM_LAST	PAGE_SNAPPY_ALGORITHM

/**********************************************************************//**
Reads the page compression level from the first page of a tablespace.
@return	page compression level, or 0 if uncompressed */
UNIV_INTERN
ulint
fsp_header_get_compression_level(
/*=============================*/
	const page_t*	page);	/*!< in: first page of a tablespace */

/********************************************************************//**
Extract the page compression level from tablespace flags.
A tablespace has only one physical page compression level
whether that page is compressed or not.
@return	page compression level of the file-per-table tablespace,
or zero if the table is not compressed.  */
UNIV_INLINE
ulint
fsp_flags_get_page_compression_level(
/*=================================*/
	ulint	flags);	/*!< in: tablespace flags */

/********************************************************************//**
Determine the tablespace is using atomic writes from dict_table_t::flags.
@return	true if atomic writes is used, false if not */
UNIV_INLINE
atomic_writes_t
fsp_flags_get_atomic_writes(
/*========================*/
	ulint	flags);	/*!< in: tablespace flags */

#ifndef UNIV_NONINL
#include "fsp0pagecompress.ic"
#endif

#endif
