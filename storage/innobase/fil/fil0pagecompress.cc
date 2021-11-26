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

/******************************************************************//**
@file fil/fil0pagecompress.cc
Implementation for page compressed file spaces.

Created 11/12/2013 Jan Lindström jan.lindstrom@mariadb.com
Updated 14/02/2015
***********************************************************************/

#include "fil0fil.h"
#include "fil0pagecompress.h"

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
#include "buf0lru.h"
#include "ibuf0ibuf.h"
#include "zlib.h"
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#endif
#include "row0mysql.h"
#include "lz4.h"
#include "lzo/lzo1x.h"
#include "lzma.h"
#include "bzlib.h"
#include "snappy-c.h"

/** Compress a page for the given compression algorithm.
@param[in]	buf		page to be compressed
@param[out]	out_buf		compressed page
@param[in]	header_len	header length of the page
@param[in]	comp_algo	compression algorithm
@param[in]	comp_level	compression level
@return actual length of compressed page data
@retval 0 if the page was not compressed */
static ulint fil_page_compress_low(
	const byte*	buf,
	byte*		out_buf,
	ulint		header_len,
	ulint		comp_algo,
	unsigned	comp_level)
{
	ulint write_size = srv_page_size - header_len;

	switch (comp_algo) {
	default:
		ut_ad("unknown compression method" == 0);
		/* fall through */
	case PAGE_UNCOMPRESSED:
		return 0;

	case PAGE_ZLIB_ALGORITHM:
		{
			ulong len = uLong(write_size);
			if (Z_OK == compress2(
				    out_buf + header_len, &len, buf,
				    uLong(srv_page_size), int(comp_level))) {
				return len;
			}
		}
		break;

	case PAGE_LZ4_ALGORITHM:
		write_size = LZ4_compress_default(
			reinterpret_cast<const char*>(buf),
			reinterpret_cast<char*>(out_buf) + header_len,
			int(srv_page_size), int(write_size));

		return write_size;

	case PAGE_LZO_ALGORITHM: {
		lzo_uint len = write_size;

		if (LZO_E_OK == lzo1x_1_15_compress(
			    buf, srv_page_size,
			    out_buf + header_len, &len,
			    out_buf + srv_page_size)
		    && len <= write_size) {
			return len;
		}
		break;
	}

	case PAGE_LZMA_ALGORITHM: {
		size_t out_pos = 0;

		if (LZMA_OK == lzma_easy_buffer_encode(
			    comp_level, LZMA_CHECK_NONE, NULL,
			    buf, srv_page_size, out_buf + header_len,
			    &out_pos, write_size)
		     && out_pos <= write_size) {
			return out_pos;
		}
		break;
	}

	case PAGE_BZIP2_ALGORITHM: {
		unsigned len = unsigned(write_size);
		if (BZ_OK == BZ2_bzBuffToBuffCompress(
			    reinterpret_cast<char*>(out_buf + header_len),
			    &len,
			    const_cast<char*>(
				    reinterpret_cast<const char*>(buf)),
			    unsigned(srv_page_size), 1, 0, 0)
		    && len <= write_size) {
			return len;
		}
		break;
	}

	case PAGE_SNAPPY_ALGORITHM: {
		size_t len = snappy_max_compressed_length(srv_page_size);

		if (SNAPPY_OK == snappy_compress(
			    reinterpret_cast<const char*>(buf),
			    srv_page_size,
			    reinterpret_cast<char*>(out_buf) + header_len,
			    &len)
		    && len <= write_size) {
			return len;
		}
		break;
	}
	}

	return 0;
}

/** Compress a page_compressed page for full crc32 format.
@param[in]	buf		page to be compressed
@param[out]	out_buf		compressed page
@param[in]	flags		tablespace flags
@param[in]	block_size	file system block size
@return actual length of compressed page
@retval 0 if the page was not compressed */
static ulint fil_page_compress_for_full_crc32(
	const byte*	buf,
	byte*		out_buf,
	uint32_t	flags,
	ulint		block_size,
	bool		encrypted)
{
	ulint comp_level = FSP_FLAGS_GET_PAGE_COMPRESSION_LEVEL(flags);

	if (comp_level == 0) {
		comp_level = page_zip_level;
	}

	const ulint header_len = FIL_PAGE_COMP_ALGO;

	ulint write_size = fil_page_compress_low(
		buf, out_buf, header_len,
		fil_space_t::get_compression_algo(flags),
		static_cast<unsigned>(comp_level));

	if (write_size == 0) {
fail:
		srv_stats.pages_page_compression_error.inc();
		return 0;
	}

	write_size += header_len;
	const ulint actual_size = write_size;
	/* Write the actual length of the data & page type
	for full crc32 format. */
	const bool lsb = fil_space_t::full_crc32_page_compressed_len(flags);
	/* In the MSB, store the rounded-up page size. */
	write_size = (write_size + lsb + (4 + 255)) & ~255;
	if (write_size >= srv_page_size) {
		goto fail;
	}

	/* Set up the page header */
	memcpy(out_buf, buf, header_len);
	out_buf[FIL_PAGE_TYPE] = 1U << (FIL_PAGE_COMPRESS_FCRC32_MARKER - 8);
	out_buf[FIL_PAGE_TYPE + 1] = byte(write_size >> 8);
	/* Clean up the buffer for the remaining write_size (except checksum) */
	memset(out_buf + actual_size, 0, write_size - actual_size - 4);
	if (lsb) {
		/* Store the LSB */
		out_buf[write_size - 5] = byte(actual_size + (1 + 4));
	}

	if (!block_size) {
		block_size = 512;
	}

	ut_ad(write_size);
	if (write_size & (block_size - 1)) {
		size_t tmp = write_size;
		write_size = (write_size + (block_size - 1))
			& ~(block_size - 1);
		memset(out_buf + tmp, 0, write_size - tmp);
	}

	srv_stats.page_compression_saved.add(srv_page_size - write_size);
	srv_stats.pages_page_compressed.inc();

	return write_size;
}

/** Compress a page_compressed page for non full crc32 format.
@param[in]	buf		page to be compressed
@param[out]	out_buf		compressed page
@param[in]	flags		tablespace flags
@param[in]	block_size	file system block size
@param[in]	encrypted	whether the page will be subsequently encrypted
@return actual length of compressed page
@retval        0       if the page was not compressed */
static ulint fil_page_compress_for_non_full_crc32(
	const byte*	buf,
	byte*		out_buf,
	ulint		flags,
	ulint		block_size,
	bool		encrypted)
{
	uint comp_level = static_cast<uint>(
		FSP_FLAGS_GET_PAGE_COMPRESSION_LEVEL(flags));
	ulint header_len = FIL_PAGE_DATA + FIL_PAGE_COMP_METADATA_LEN;
	/* Cache to avoid change during function execution */
	ulint comp_algo = innodb_compression_algorithm;

	if (encrypted) {
		header_len += FIL_PAGE_ENCRYPT_COMP_ALGO;
	}

	/* If no compression level was provided to this table, use system
	default level */
	if (comp_level == 0) {
		comp_level = page_zip_level;
	}

	ulint write_size = fil_page_compress_low(
				buf, out_buf,
				header_len, comp_algo, comp_level);

	if (write_size == 0) {
		srv_stats.pages_page_compression_error.inc();
		return 0;
	}

	/* Set up the page header */
	memcpy(out_buf, buf, FIL_PAGE_DATA);
	/* Set up the checksum */
	mach_write_to_4(out_buf + FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);

	/* Set up the compression algorithm */
	mach_write_to_8(out_buf + FIL_PAGE_COMP_ALGO, comp_algo);

	if (encrypted) {
		/* Set up the correct page type */
		mach_write_to_2(out_buf + FIL_PAGE_TYPE,
				FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);

		mach_write_to_2(out_buf + FIL_PAGE_DATA
				+ FIL_PAGE_ENCRYPT_COMP_ALGO, comp_algo);
	} else {
		/* Set up the correct page type */
		mach_write_to_2(out_buf + FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED);
	}

	/* Set up the actual payload lenght */
	mach_write_to_2(out_buf + FIL_PAGE_DATA + FIL_PAGE_COMP_SIZE,
			write_size);

	ut_ad(mach_read_from_4(out_buf + FIL_PAGE_SPACE_OR_CHKSUM)
	      == BUF_NO_CHECKSUM_MAGIC);

	ut_ad(mach_read_from_2(out_buf + FIL_PAGE_DATA + FIL_PAGE_COMP_SIZE)
	      == write_size);

#ifdef UNIV_DEBUG
	bool is_compressed = (mach_read_from_8(out_buf + FIL_PAGE_COMP_ALGO)
			      == (ulint) comp_algo);

	bool is_encrypted_compressed =
		(mach_read_from_2(out_buf + FIL_PAGE_DATA
				  + FIL_PAGE_ENCRYPT_COMP_ALGO)
		 == (ulint) comp_algo);
#endif /* UNIV_DEBUG */

	ut_ad(is_compressed || is_encrypted_compressed);

	write_size+=header_len;

	if (block_size <= 0) {
		block_size = 512;
	}

	ut_ad(write_size > 0 && block_size > 0);

	/* Actual write needs to be alligned on block size */
	if (write_size % block_size) {
		size_t tmp = write_size;
		write_size =  (size_t)ut_uint64_align_up(
				(ib_uint64_t)write_size, block_size);
		/* Clean up the end of buffer */
		memset(out_buf+tmp, 0, write_size - tmp);
#ifdef UNIV_DEBUG
		ut_a(write_size > 0 && ((write_size % block_size) == 0));
		ut_a(write_size >= tmp);
#endif
	}

	srv_stats.page_compression_saved.add(srv_page_size - write_size);
	srv_stats.pages_page_compressed.inc();

	return write_size;
}

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
{
	/* The full_crc32 page_compressed format assumes this. */
	ut_ad(!(block_size & 255));
	ut_ad(ut_is_2pow(block_size));

	/* Let's not compress file space header or
	extent descriptor */
	switch (fil_page_get_type(buf)) {
	case 0:
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_PAGE_COMPRESSED:
		return 0;
	}

	if (fil_space_t::full_crc32(flags)) {
		return fil_page_compress_for_full_crc32(
				buf, out_buf, flags, block_size, encrypted);
	}

	return fil_page_compress_for_non_full_crc32(
			buf, out_buf, flags, block_size, encrypted);
}

/** Decompress a page that may be subject to page_compressed compression.
@param[in,out]	tmp_buf		temporary buffer (of innodb_page_size)
@param[in,out]	buf		possibly compressed page buffer
@param[in]	comp_algo	compression algorithm
@param[in]	header_len	header length of the page
@param[in]	actual size	actual size of the page
@retval true if the page is decompressed or false */
static bool fil_page_decompress_low(
	byte*		tmp_buf,
	byte*		buf,
	ulint		comp_algo,
	ulint		header_len,
	ulint		actual_size)
{
	switch (comp_algo) {
	default:
		ib::error() << "Unknown compression algorithm "
			    << comp_algo;
		return false;
	case PAGE_ZLIB_ALGORITHM:
		{
			uLong len = srv_page_size;
			return (Z_OK == uncompress(tmp_buf, &len,
					       buf + header_len,
					       uLong(actual_size))
				&& len == srv_page_size);
		}

	case PAGE_LZ4_ALGORITHM:
		return LZ4_decompress_safe(
			reinterpret_cast<const char*>(buf) + header_len,
			reinterpret_cast<char*>(tmp_buf),
			static_cast<int>(actual_size),
			static_cast<int>(srv_page_size)) ==
			static_cast<int>(srv_page_size);

	case PAGE_LZO_ALGORITHM:
		{
			lzo_uint len_lzo = srv_page_size;
			return (LZO_E_OK == lzo1x_decompress_safe(
					buf + header_len,
					actual_size, tmp_buf, &len_lzo, NULL)
				&& len_lzo == srv_page_size);
		}

	case PAGE_LZMA_ALGORITHM:
		{
			size_t		src_pos = 0;
			size_t		dst_pos = 0;
			uint64_t 	memlimit = UINT64_MAX;

			return LZMA_OK == lzma_stream_buffer_decode(
				&memlimit, 0, NULL, buf + header_len,
				&src_pos, actual_size, tmp_buf, &dst_pos,
				srv_page_size)
				&& dst_pos == srv_page_size;
		}

	case PAGE_BZIP2_ALGORITHM:
		{
			uint dst_pos = static_cast<uint>(srv_page_size);
			return BZ_OK == BZ2_bzBuffToBuffDecompress(
				reinterpret_cast<char*>(tmp_buf),
				&dst_pos,
				reinterpret_cast<char*>(buf) + header_len,
				static_cast<uint>(actual_size), 1, 0)
				&& dst_pos == srv_page_size;
		}

	case PAGE_SNAPPY_ALGORITHM:
		{
			size_t olen = srv_page_size;

			return SNAPPY_OK == snappy_uncompress(
				reinterpret_cast<const char*>(buf)
				+ header_len,
				actual_size,
				reinterpret_cast<char*>(tmp_buf), &olen)
				&& olen == srv_page_size;
		}
	}

	return false;
}

/** Decompress a page for full crc32 format.
@param[in,out]	tmp_buf	temporary buffer (of innodb_page_size)
@param[in,out]	buf	possibly compressed page buffer
@param[in]	flags	tablespace flags
@return size of the compressed data
@retval	0		if decompression failed
@retval	srv_page_size	if the page was not compressed */
static size_t fil_page_decompress_for_full_crc32(byte *tmp_buf, byte *buf,
                                                 uint32_t flags)
{
	ut_ad(fil_space_t::full_crc32(flags));
	bool compressed = false;
	size_t size = buf_page_full_crc32_size(buf, &compressed, NULL);
	if (!compressed) {
		ut_ad(size == srv_page_size);
		return size;
	}

	if (!fil_space_t::is_compressed(flags)) {
		return 0;
	}

	if (size >= srv_page_size) {
		return 0;
	}

	if (fil_space_t::full_crc32_page_compressed_len(flags)) {
		compile_time_assert(FIL_PAGE_FCRC32_CHECKSUM == 4);
		if (size_t lsb = buf[size - 5]) {
			size += lsb - 0x100;
		}
		size -= 5;
	}

	const size_t header_len = FIL_PAGE_COMP_ALGO;

	if (!fil_page_decompress_low(tmp_buf, buf,
				     fil_space_t::get_compression_algo(flags),
				     header_len, size - header_len)) {
		return 0;
	}

	srv_stats.pages_page_decompressed.inc();
	memcpy(buf, tmp_buf, srv_page_size);
	return size;
}

/** Decompress a page for non full crc32 format.
@param[in,out] tmp_buf	temporary buffer (of innodb_page_size)
@param[in,out] buf	possibly compressed page buffer
@return size of the compressed data
@retval	0		if decompression failed
@retval	srv_page_size	if the page was not compressed */
static size_t fil_page_decompress_for_non_full_crc32(byte *tmp_buf, byte *buf)
{
	ulint header_len;
	uint comp_algo;
	switch (fil_page_get_type(buf)) {
	case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
		header_len= FIL_PAGE_DATA + FIL_PAGE_ENCRYPT_COMP_METADATA_LEN;
		comp_algo = mach_read_from_2(
			FIL_PAGE_DATA + FIL_PAGE_ENCRYPT_COMP_ALGO + buf);
		break;
	case FIL_PAGE_PAGE_COMPRESSED:
		header_len = FIL_PAGE_DATA + FIL_PAGE_COMP_METADATA_LEN;
		if (mach_read_from_6(FIL_PAGE_COMP_ALGO + buf)) {
			return 0;
		}
		comp_algo = mach_read_from_2(FIL_PAGE_COMP_ALGO + 6 + buf);
		break;
	default:
		return srv_page_size;
	}

	if (mach_read_from_4(buf + FIL_PAGE_SPACE_OR_CHKSUM)
	    != BUF_NO_CHECKSUM_MAGIC) {
		return 0;
	}

	ulint actual_size = mach_read_from_2(buf + FIL_PAGE_DATA
					     + FIL_PAGE_COMP_SIZE);

	/* Check if payload size is corrupted */
	if (actual_size == 0 || actual_size > srv_page_size - header_len) {
		return 0;
	}

	if (!fil_page_decompress_low(tmp_buf, buf, comp_algo, header_len,
				     actual_size)) {
		return 0;
	}

	srv_stats.pages_page_decompressed.inc();
	memcpy(buf, tmp_buf, srv_page_size);
	return actual_size;
}

/** Decompress a page that may be subject to page_compressed compression.
@param[in,out]	tmp_buf		temporary buffer (of innodb_page_size)
@param[in,out]	buf		possibly compressed page buffer
@param[in]	flags		tablespace flags
@return size of the compressed data
@retval	0		if decompression failed
@retval	srv_page_size	if the page was not compressed */
ulint fil_page_decompress(byte *tmp_buf, byte *buf, uint32_t flags)
{
	if (fil_space_t::full_crc32(flags)) {
		return fil_page_decompress_for_full_crc32(tmp_buf, buf, flags);
	}

	return fil_page_decompress_for_non_full_crc32(tmp_buf, buf);
}
