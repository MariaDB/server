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

/*******************************************************************//**
Find out wheather the page is index page or not
@return	true if page type index page, false if not */
UNIV_INLINE
ibool
fil_page_is_index_page(
/*===================*/
	byte	*buf);	/*!< in: page */

/****************************************************************//**
Get the name of the compression algorithm used for page
compression.
@return compression algorithm name or "UNKNOWN" if not known*/
UNIV_INLINE
const char*
fil_get_compression_alg_name(
/*=========================*/
       ulint	comp_alg);	/*!<in: compression algorithm number */

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

/****************************************************************//**
Get space id from fil node
@return space id*/
UNIV_INTERN
ulint
fil_node_get_space_id(
/*==================*/
        fil_node_t*	node);	/*!< in: Node where to get space id*/

/****************************************************************//**
Get block size from fil node
@return block size*/
UNIV_INLINE
ulint
fil_node_get_block_size(
	fil_node_t*	node);	/*!< in: Node where to get block
				size */
/*******************************************************************//**
Find out wheather the page is page compressed
@return	true if page is page compressed*/
UNIV_INLINE
ibool
fil_page_is_compressed(
/*===================*/
	byte*	buf);	/*!< in: page */

/*******************************************************************//**
Find out wheather the page is page compressed
@return	true if page is page compressed*/
UNIV_INLINE
ibool
fil_page_is_compressed_encrypted(
/*=============================*/
	byte*	buf);	/*!< in: page */

/*******************************************************************//**
Find out wheather the page is page compressed with lzo method
@return	true if page is page compressed with lzo method*/
UNIV_INLINE
ibool
fil_page_is_lzo_compressed(
/*=======================*/
	byte*	buf);	/*!< in: page */
#endif
