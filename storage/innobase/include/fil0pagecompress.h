/*****************************************************************************

Copyright (C) 2013, 2014 SkySQL Ab. All Rights Reserved.

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
Returns the page compression level flag of the space, or 0 if the space
is not compressed. The tablespace must be cached in the memory cache.
@return	page compression level if page compressed, ULINT_UNDEFINED if space not found */
ulint
fil_space_get_page_compression_level(
/*=================================*/
	ulint	id);	/*!< in: space id */
/*******************************************************************//**
Returns the page compression flag of the space, or false if the space
is not compressed. The tablespace must be cached in the memory cache.
@return	true if page compressed, false if not or space not found */
ibool
fil_space_is_page_compressed(
/*=========================*/
	ulint   id);	/*!< in: space id */
/*******************************************************************//**
Returns the atomic writes flag of the space, or false if the space
is not using atomic writes. The tablespace must be cached in the memory cache.
@return	atomic write table option value */
atomic_writes_t
fil_space_get_atomic_writes(
/*=========================*/
	ulint   id);	/*!< in: space id */
/*******************************************************************//**
Find out wheather the page is index page or not
@return	true if page type index page, false if not */
ibool
fil_page_is_index_page(
/*===================*/
	byte *buf);	/*!< in: page */

/****************************************************************//**
Get the name of the compression algorithm used for page
compression.
@return compression algorithm name or "UNKNOWN" if not known*/
const char*
fil_get_compression_alg_name(
/*=========================*/
       ulint           comp_alg);    /*!<in: compression algorithm number */

/****************************************************************//**
For page compressed pages compress the page before actual write
operation.
@return compressed page to be written*/
byte*
fil_compress_page(
/*==============*/
	ulint		space_id,      /*!< in: tablespace id of the
				       table. */
	byte*           buf,           /*!< in: buffer from which to write; in aio
				       this must be appropriately aligned */
        byte*           out_buf,       /*!< out: compressed buffer */
        ulint           len,           /*!< in: length of input buffer.*/
        ulint           compression_level, /*!< in: compression level */
	ulint*          out_len,       /*!< out: actual length of compressed
				       page */
	byte*		lzo_mem);      /*!< in: temporal memory used by LZO */

/****************************************************************//**
For page compressed pages decompress the page after actual read
operation.
@return uncompressed page */
void
fil_decompress_page(
/*================*/
	byte*           page_buf,      /*!< in: preallocated buffer or NULL */
	byte*           buf,           /*!< out: buffer from which to read; in aio
				       this must be appropriately aligned */
        ulong           len,           /*!< in: length of output buffer.*/
	ulint*		write_size);   /*!< in/out: Actual payload size of
				       the compressed data. */

/****************************************************************//**
Get space id from fil node
@return space id*/
ulint
fil_node_get_space_id(
/*==================*/
        fil_node_t*     node);         /*!< in: Node where to get space id*/

/*******************************************************************//**
Find out wheather the page is page compressed
@return	true if page is page compressed*/
ibool
fil_page_is_compressed(
/*===================*/
	byte *buf);	/*!< in: page */

#endif
