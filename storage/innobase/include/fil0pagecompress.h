/*****************************************************************************

Copyright (C) 2013, 2017 MariaDB Corporation. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

#ifndef fil0pagecompress_h
#define fil0pagecompress_h

#include "fsp0fsp.h"
#include "fsp0pagecompress.h"

/******************************************************************//**
@file include/fil0pagecompress.h
Helper functions for extracting/storing page compression and
atomic writes information to table space.

Created 11/12/2013 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

/****************************************************************//**
For page compressed pages compress the page before actual write
operation.
@return compressed page to be written*/
UNIV_INTERN
byte*
fil_compress_page(
/*==============*/
	fil_space_t*	space,	/*!< in,out: tablespace (NULL during IMPORT) */
	byte*	buf,		/*!< in: buffer from which to write; in aio
				this must be appropriately aligned */
	byte*	out_buf,	/*!< out: compressed buffer */
	ulint	len,		/*!< in: length of input buffer.*/
	ulint	level,		/* in: compression level */
	ulint	block_size,	/*!< in: block size */
	bool	encrypted,	/*!< in: is page also encrypted */
	ulint*	out_len);	/*!< out: actual length of compressed
				page */

/****************************************************************//**
For page compressed pages decompress the page after actual read
operation. */
UNIV_INTERN
void
fil_decompress_page(
/*================*/
	byte*	page_buf,	/*!< in: preallocated buffer or NULL */
	byte*	buf,		/*!< out: buffer from which to read; in aio
				this must be appropriately aligned */
	ulong	len,		/*!< in: length of output buffer.*/
	ulint*	write_size,	/*!< in/out: Actual payload size of
				the compressed data. */
	bool	return_error=false);
				/*!< in: true if only an error should
				be produced when decompression fails.
				By default this parameter is false. */
#endif
