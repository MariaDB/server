/*****************************************************************************

Copyright (C) 2013, 2021, MariaDB Corporation.

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

#ifndef fil0pagecompress_h
#define fil0pagecompress_h

#include "fsp0fsp.h"

/******************************************************************//**
@file include/fil0pagecompress.h
Helper functions for extracting/storing page compression and
atomic writes information to table space.

Created 11/12/2013 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

/** Compress a page_compressed page before writing to a data file.
@param[in]	buf		page to be compressed
@param[out]	out_buf		compressed page
@param[in]	flags		tablespace flags
@param[in]	block_size	file system block size
@param[in]	encrypted	whether the page will be subsequently encrypted
@return actual length of compressed page
@retval	0	if the page was not compressed */
ulint fil_page_compress(
	const byte*	buf,
	byte*		out_buf,
	uint32_t	flags,
	ulint		block_size,
	bool		encrypted)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Decompress a page that may be subject to page_compressed compression.
@param[in,out]	tmp_buf		temporary buffer (of innodb_page_size)
@param[in,out]	buf		compressed page buffer
@param[in]	flags		tablespace flags
@return size of the compressed data
@retval	0		if decompression failed
@retval	srv_page_size	if the page was not compressed */
ulint fil_page_decompress(byte *tmp_buf, byte *buf, uint32_t flags)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
#endif
