/*****************************************************************************

Copyright (C) 2013, 2020, MariaDB Corporation.

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
@file fil/fil0pagecompress.cc
Implementation for page compressed file spaces.

Created 11/12/2013 Jan Lindström jan.lindstrom@mariadb.com
Updated 14/02/2015
***********************************************************************/

#include "fil0fil.h"
#include "fil0pagecompress.h"

#include <debug_sync.h>
#include <my_dbug.h>

#include "mem0mem.h"
#include "hash0hash.h"
#include "os0file.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "log0recv.h"
#include "fsp0fsp.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "dict0dict.h"
#include "page0page.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "row0mysql.h"
#include "ha_prototypes.h"  // IB_LOG_
#ifndef UNIV_HOTBACKUP
# include "buf0lru.h"
# include "ibuf0ibuf.h"
# include "sync0sync.h"
# include "os0sync.h"
#else /* !UNIV_HOTBACKUP */
# include "srv0srv.h"
static ulint srv_data_read, srv_data_written;
#endif /* !UNIV_HOTBACKUP */
#include "zlib.h"
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#endif
#include "row0mysql.h"
#ifdef HAVE_LZ4
#include "lz4.h"
#endif
#ifdef HAVE_LZO
#include "lzo/lzo1x.h"
#endif
#ifdef HAVE_LZMA
#include "lzma.h"
#endif
#ifdef HAVE_BZIP2
#include "bzlib.h"
#endif
#ifdef HAVE_SNAPPY
#include "snappy-c.h"
#endif

/** Compress a page_compressed page before writing to a data file.
@param[in]	buf		page to be compressed
@param[out]	out_buf		compressed page
@param[in]	level		compression level
@param[in]	block_size	file system block size
@param[in]	encrypted	whether the page will be subsequently encrypted
@return actual length of compressed page
@retval	0	if the page was not compressed */
UNIV_INTERN ulint fil_page_compress(const byte* buf, byte* out_buf, ulint level,
				    ulint block_size, bool encrypted)
{
	int comp_level = int(level);
	ulint header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;
	/* Cache to avoid change during function execution */
	ulint comp_method = innodb_compression_algorithm;

	if (encrypted) {
		header_len += FIL_PAGE_COMPRESSION_METHOD_SIZE;
	}

	/* Let's not compress file space header or
	extent descriptor */
	switch (fil_page_get_type(buf)) {
	case 0:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_PAGE_COMPRESSED:
		return 0;
	}

	/* If no compression level was provided to this table, use system
	default level */
	if (comp_level == 0) {
		comp_level = page_zip_level;
	}

	ulint write_size = srv_page_size - header_len;

	switch (comp_method) {
	default:
		ut_ad(!"unknown compression method");
		/* fall through */
	case PAGE_UNCOMPRESSED:
		return 0;
	case PAGE_ZLIB_ALGORITHM:
		{
			ulong len = uLong(write_size);
			if (Z_OK == compress2(
				    out_buf + header_len, &len,
				    buf, uLong(srv_page_size), comp_level)) {
				write_size = len;
				goto success;
			}
		}
		break;
#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:
# ifdef HAVE_LZ4_COMPRESS_DEFAULT
		write_size = LZ4_compress_default(
			reinterpret_cast<const char*>(buf),
			reinterpret_cast<char*>(out_buf) + header_len,
			int(srv_page_size), int(write_size));
# else
		write_size = LZ4_compress_limitedOutput(
			reinterpret_cast<const char*>(buf),
			reinterpret_cast<char*>(out_buf) + header_len,
			int(srv_page_size), int(write_size));
# endif

		if (write_size) {
			goto success;
		}
		break;
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM: {
		lzo_uint len = write_size;

		if (LZO_E_OK == lzo1x_1_15_compress(
			    buf, srv_page_size,
			    out_buf + header_len, &len,
			    out_buf + srv_page_size)
		    && len <= write_size) {
			write_size = len;
			goto success;
		}
		break;
	}
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM: {
		size_t out_pos = 0;

		if (LZMA_OK == lzma_easy_buffer_encode(
			    comp_level, LZMA_CHECK_NONE, NULL,
			    buf, srv_page_size, out_buf + header_len,
			    &out_pos, write_size)
		     && out_pos <= write_size) {
			write_size = out_pos;
			goto success;
		}
		break;
	}
#endif /* HAVE_LZMA */

#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM: {
		unsigned len = unsigned(write_size);
		if (BZ_OK == BZ2_bzBuffToBuffCompress(
			    reinterpret_cast<char*>(out_buf + header_len),
			    &len,
			    const_cast<char*>(
				    reinterpret_cast<const char*>(buf)),
			    unsigned(srv_page_size), 1, 0, 0)
		    && len <= write_size) {
			write_size = len;
			goto success;
		}
		break;
	}
#endif /* HAVE_BZIP2 */

#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM: {
		size_t len = snappy_max_compressed_length(srv_page_size);

		if (SNAPPY_OK == snappy_compress(
			    reinterpret_cast<const char*>(buf),
			    srv_page_size,
			    reinterpret_cast<char*>(out_buf) + header_len,
			    &len)
		    && len <= write_size) {
			write_size = len;
			goto success;
		}
		break;
	}
#endif /* HAVE_SNAPPY */
	}

	srv_stats.pages_page_compression_error.inc();
	return 0;
success:
	/* Set up the page header */
	memcpy(out_buf, buf, FIL_PAGE_DATA);
	/* Set up the checksum */
	mach_write_to_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);

	/* Set up the compression algorithm */
	mach_write_to_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, comp_method);

	if (encrypted) {
		/* Set up the correct page type */
		mach_write_to_2(out_buf+FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
		mach_write_to_2(out_buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE, comp_method);
	} else {
		/* Set up the correct page type */
		mach_write_to_2(out_buf+FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED);
	}

	/* Set up the actual payload lenght */
	mach_write_to_2(out_buf+FIL_PAGE_DATA, write_size);

	ut_ad(fil_page_is_compressed(out_buf) || fil_page_is_compressed_encrypted(out_buf));
	ut_ad(mach_read_from_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM) == BUF_NO_CHECKSUM_MAGIC);
	ut_ad(mach_read_from_2(out_buf+FIL_PAGE_DATA) == write_size);
	ut_ad(mach_read_from_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION) == (ulint)comp_method ||
		mach_read_from_2(out_buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE) == (ulint)comp_method);

	write_size+=header_len;

	if (block_size <= 0) {
		block_size = 512;
	}

	ut_ad(write_size > 0 && block_size > 0);

	/* Actual write needs to be alligned on block size */
	if (write_size % block_size) {
		size_t tmp = write_size;
		write_size =  (size_t)ut_uint64_align_up((ib_uint64_t)write_size, block_size);
		/* Clean up the end of buffer */
		memset(out_buf+tmp, 0, write_size - tmp);
#ifdef UNIV_DEBUG
		ut_a(write_size > 0 && ((write_size % block_size) == 0));
		ut_a(write_size >= tmp);
#endif
	}

	srv_stats.page_compression_saved.add(srv_page_size - write_size);
	srv_stats.pages_page_compressed.inc();

	/* If we do not persistently trim rest of page, we need to write it
	all */
	if (!srv_use_trim) {
		memset(out_buf + write_size, 0, srv_page_size - write_size);
	}

	return write_size;
}

/** Decompress a page that may be subject to page_compressed compression.
@param[in,out]	tmp_buf		temporary buffer (of innodb_page_size)
@param[in,out]	buf		possibly compressed page buffer
@return size of the compressed data
@retval	0		if decompression failed
@retval	srv_page_size	if the page was not compressed */
UNIV_INTERN ulint fil_page_decompress(byte* tmp_buf, byte* buf)
{
	const unsigned	ptype = mach_read_from_2(buf+FIL_PAGE_TYPE);
	ulint header_len;
	ib_uint64_t compression_alg;
	switch (ptype) {
	case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
		header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE
			+ FIL_PAGE_COMPRESSION_METHOD_SIZE;
		compression_alg = mach_read_from_2(
			FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE + buf);
		break;
	case FIL_PAGE_PAGE_COMPRESSED:
		header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;
		compression_alg = mach_read_from_8(
			FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + buf);
		break;
	default:
		return srv_page_size;
	}

	if (mach_read_from_4(buf + FIL_PAGE_SPACE_OR_CHKSUM)
	    != BUF_NO_CHECKSUM_MAGIC) {
		return 0;
	}

	ulint actual_size = mach_read_from_2(buf + FIL_PAGE_DATA);

	/* Check if payload size is corrupted */
	if (actual_size == 0 || actual_size > srv_page_size - header_len) {
		return 0;
	}

	switch (compression_alg) {
	default:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown compression algorithm " UINT64PF,
			compression_alg);
		return 0;
	case PAGE_ZLIB_ALGORITHM:
		{
			uLong len = srv_page_size;
			if (Z_OK == uncompress(tmp_buf, &len,
					       buf + header_len,
					       uLong(actual_size))
			    && len == srv_page_size) {
				break;
			}
		}
		return 0;
#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:
		if (LZ4_decompress_safe(reinterpret_cast<const char*>(buf)
					+ header_len,
					reinterpret_cast<char*>(tmp_buf),
					actual_size, srv_page_size)
		    == int(srv_page_size)) {
			break;
		}
		return 0;
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM: {
		lzo_uint len_lzo = srv_page_size;
		if (LZO_E_OK == lzo1x_decompress_safe(
			    buf + header_len,
			    actual_size, tmp_buf, &len_lzo, NULL)
		    && len_lzo == srv_page_size) {
			break;
		}
		return 0;
        }
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM: {
		size_t		src_pos = 0;
		size_t		dst_pos = 0;
		uint64_t 	memlimit = UINT64_MAX;

		if (LZMA_OK == lzma_stream_buffer_decode(
			    &memlimit, 0, NULL, buf + header_len,
			    &src_pos, actual_size, tmp_buf, &dst_pos,
			    srv_page_size)
		    && dst_pos == srv_page_size) {
			break;
		}
		return 0;
	}
#endif /* HAVE_LZMA */
#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM: {
		unsigned int dst_pos = srv_page_size;
		if (BZ_OK == BZ2_bzBuffToBuffDecompress(
			    reinterpret_cast<char*>(tmp_buf),
			    &dst_pos,
			    reinterpret_cast<char*>(buf) + header_len,
			    actual_size, 1, 0)
		    && dst_pos == srv_page_size) {
			break;
		}
		return 0;
	}
#endif /* HAVE_BZIP2 */
#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM: {
		size_t olen = srv_page_size;

		if (SNAPPY_OK == snappy_uncompress(
			    reinterpret_cast<const char*>(buf) + header_len,
			    actual_size,
			    reinterpret_cast<char*>(tmp_buf), &olen)
		    && olen == srv_page_size) {
			break;
		}
		return 0;
	}
#endif /* HAVE_SNAPPY */
	}

	srv_stats.pages_page_decompressed.inc();
	memcpy(buf, tmp_buf, srv_page_size);
	return actual_size;
}
