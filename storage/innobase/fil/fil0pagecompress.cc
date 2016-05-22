/*****************************************************************************

Copyright (C) 2013, 2016, MariaDB Corporation. All Rights Reserved.

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

/* Used for debugging */
//#define UNIV_PAGECOMPRESS_DEBUG 1

/****************************************************************//**
For page compressed pages compress the page before actual write
operation.
@return compressed page to be written*/
UNIV_INTERN
byte*
fil_compress_page(
/*==============*/
	ulint	space_id,	/*!< in: tablespace id of the
				table. */
	byte*	buf,		/*!< in: buffer from which to write; in aio
				this must be appropriately aligned */
	byte*	out_buf,	/*!< out: compressed buffer */
	ulint	len,		/*!< in: length of input buffer.*/
	ulint	level,		/* in: compression level */
	ulint	block_size,	/*!< in: block size */
	bool	encrypted,	/*!< in: is page also encrypted */
	ulint*	out_len,	/*!< out: actual length of compressed
				page */
	byte*	lzo_mem)	/*!< in: temporal memory used by LZO */
{
	int err = Z_OK;
	int comp_level = level;
	ulint header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;
	ulint write_size=0;
	/* Cache to avoid change during function execution */
	ulint comp_method = innodb_compression_algorithm;
	ulint orig_page_type;
	bool allocated=false;

	if (encrypted) {
		header_len += FIL_PAGE_COMPRESSION_METHOD_SIZE;
	}

	if (!out_buf) {
		allocated = true;
		out_buf = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
#ifdef HAVE_LZO
		if (comp_method == PAGE_LZO_ALGORITHM) {
			lzo_mem = static_cast<byte *>(ut_malloc(LZO1X_1_15_MEM_COMPRESS));
			memset(lzo_mem, 0, LZO1X_1_15_MEM_COMPRESS);
		}
#endif
	}

	ut_ad(buf);
	ut_ad(out_buf);
	ut_ad(len);
	ut_ad(out_len);

	/* read original page type */
	orig_page_type = mach_read_from_2(buf + FIL_PAGE_TYPE);

	fil_system_enter();
	fil_space_t* space = fil_space_get_by_id(space_id);
	fil_system_exit();

	/* Let's not compress file space header or
	extent descriptor */
	if (orig_page_type == 0 ||
	    orig_page_type == FIL_PAGE_TYPE_FSP_HDR ||
	    orig_page_type == FIL_PAGE_TYPE_XDES ||
		orig_page_type == FIL_PAGE_PAGE_COMPRESSED) {
		*out_len = len;

		goto err_exit;
	}

	/* If no compression level was provided to this table, use system
	default level */
	if (comp_level == 0) {
		comp_level = page_zip_level;
	}

#ifdef UNIV_PAGECOMPRESS_DEBUG
	ib_logf(IB_LOG_LEVEL_INFO,
		"Preparing for compress for space %lu name %s len %lu.",
		space_id, fil_space_name(space), len);
#endif /* UNIV_PAGECOMPRESS_DEBUG */

	write_size = UNIV_PAGE_SIZE - header_len;

	switch(comp_method) {
#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:
		err = LZ4_compress_limitedOutput((const char *)buf,
			(char *)out_buf+header_len, len, write_size);
		write_size = err;

		if (err == 0) {
			/* If error we leave the actual page as it was */

#ifndef UNIV_PAGECOMPRESS_DEBUG
			if (space->printed_compression_failure == false) {
#endif
				ib_logf(IB_LOG_LEVEL_WARN,
					"Compression failed for space %lu name %s len %lu rt %d write %lu.",
					space_id, fil_space_name(space), len, err, write_size);
				space->printed_compression_failure = true;
#ifndef UNIV_PAGECOMPRESS_DEBUG
			}
#endif
			srv_stats.pages_page_compression_error.inc();
			*out_len = len;
			goto err_exit;
		}
		break;
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM:
		err = lzo1x_1_15_compress(
			buf, len, out_buf+header_len, &write_size, lzo_mem);

		if (err != LZO_E_OK || write_size > UNIV_PAGE_SIZE-header_len) {
			if (space->printed_compression_failure == false) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Compression failed for space %lu name %s len %lu err %d write_size %lu.",
					space_id, fil_space_name(space), len, err, write_size);
				space->printed_compression_failure = true;
			}

			srv_stats.pages_page_compression_error.inc();
			*out_len = len;
			goto err_exit;
		}

		break;
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM: {
		size_t out_pos=0;

		err = lzma_easy_buffer_encode(
			comp_level,
			LZMA_CHECK_NONE,
			NULL, 	/* No custom allocator, use malloc/free */
			reinterpret_cast<uint8_t*>(buf),
			len,
			reinterpret_cast<uint8_t*>(out_buf + header_len),
			&out_pos,
			(size_t)write_size);

		if (err != LZMA_OK || out_pos > UNIV_PAGE_SIZE-header_len) {
			if (space->printed_compression_failure == false) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Compression failed for space %lu name %s len %lu err %d write_size %lu",
					space_id, fil_space_name(space), len, err, out_pos);
				space->printed_compression_failure = true;
			}

			srv_stats.pages_page_compression_error.inc();
			*out_len = len;
			goto err_exit;
		}

		write_size = out_pos;

		break;
	}
#endif /* HAVE_LZMA */

#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM: {

		err = BZ2_bzBuffToBuffCompress(
			(char *)(out_buf + header_len),
			(unsigned int *)&write_size,
			(char *)buf,
			len,
			1,
			0,
			0);

		if (err != BZ_OK || write_size > UNIV_PAGE_SIZE-header_len) {
			if (space->printed_compression_failure == false) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Compression failed for space %lu name %s len %lu err %d write_size %lu.",
					space_id, fil_space_name(space), len, err, write_size);
				space->printed_compression_failure = true;
			}

			srv_stats.pages_page_compression_error.inc();
			*out_len = len;
			goto err_exit;
		}
		break;
	}
#endif /* HAVE_BZIP2 */

#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM:
	{
		snappy_status cstatus;

		cstatus = snappy_compress(
			(const char *)buf,
			(size_t)len,
			(char *)(out_buf+header_len),
			(size_t*)&write_size);

		if (cstatus != SNAPPY_OK || write_size > UNIV_PAGE_SIZE-header_len) {
			if (space->printed_compression_failure == false) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Compression failed for space %lu name %s len %lu err %d write_size %lu.",
					space_id, fil_space_name(space), len, (int)cstatus, write_size);
				space->printed_compression_failure = true;
			}

			srv_stats.pages_page_compression_error.inc();
			*out_len = len;
			goto err_exit;
		}
		break;
	}
#endif /* HAVE_SNAPPY */

	case PAGE_ZLIB_ALGORITHM:
		err = compress2(out_buf+header_len, (ulong*)&write_size, buf, len, comp_level);

		if (err != Z_OK) {
			/* If error we leave the actual page as it was */

			if (space->printed_compression_failure == false) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Compression failed for space %lu name %s len %lu rt %d write %lu.",
					space_id, fil_space_name(space), len, err, write_size);
				space->printed_compression_failure = true;
			}

			srv_stats.pages_page_compression_error.inc();
			*out_len = len;
			goto err_exit;
		}
		break;

	case PAGE_UNCOMPRESSED:
		*out_len = len;
		return (buf);
		break;
	default:
		ut_error;
		break;
	}

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

#ifdef UNIV_DEBUG
	/* Verify */
	ut_ad(fil_page_is_compressed(out_buf) || fil_page_is_compressed_encrypted(out_buf));
	ut_ad(mach_read_from_4(out_buf+FIL_PAGE_SPACE_OR_CHKSUM) == BUF_NO_CHECKSUM_MAGIC);
	ut_ad(mach_read_from_2(out_buf+FIL_PAGE_DATA) == write_size);
	ut_ad(mach_read_from_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION) == (ulint)comp_method ||
		mach_read_from_2(out_buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE) == (ulint)comp_method);

	/* Verify that page can be decompressed */
	{
		byte *comp_page;
		byte *uncomp_page;

		comp_page = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
		uncomp_page = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
		memcpy(comp_page, out_buf, UNIV_PAGE_SIZE);

		fil_decompress_page(uncomp_page, comp_page, len, NULL);

		if(buf_page_is_corrupted(false, uncomp_page, 0)) {
			buf_page_print(uncomp_page, 0, BUF_PAGE_PRINT_NO_CRASH);
			ut_error;
		}

		ut_free(comp_page);
		ut_free(uncomp_page);
	}
#endif /* UNIV_DEBUG */

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

#ifdef UNIV_PAGECOMPRESS_DEBUG
	ib_logf(IB_LOG_LEVEL_INFO,
		"Compression succeeded for space %lu name %s len %lu out_len %lu.",
		space_id, fil_space_name(space), len, write_size);
#endif /* UNIV_PAGECOMPRESS_DEBUG */

	srv_stats.page_compression_saved.add((len - write_size));
	srv_stats.pages_page_compressed.inc();

	/* If we do not persistently trim rest of page, we need to write it
	all */
	if (!srv_use_trim) {
		memset(out_buf+write_size,0,len-write_size);
		write_size = len;
	}

	*out_len = write_size;

	if (allocated) {
		/* TODO: reduce number of memcpy's */
		memcpy(buf, out_buf, len);
	} else {
		return(out_buf);
	}

err_exit:
	if (allocated) {
		ut_free(out_buf);
#ifdef HAVE_LZO
		if (comp_method == PAGE_LZO_ALGORITHM) {
			ut_free(lzo_mem);
		}
#endif
	}

	return (buf);

}

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
	bool	return_error)	/*!< in: true if only an error should
				be produced when decompression fails.
				By default this parameter is false. */
{
	int err = 0;
	ulint actual_size = 0;
	ulint compression_alg = 0;
	byte *in_buf;
	ulint ptype;
	ulint header_len = FIL_PAGE_DATA + FIL_PAGE_COMPRESSED_SIZE;

	ut_ad(buf);
	ut_ad(len);

	ptype = mach_read_from_2(buf+FIL_PAGE_TYPE);

	if (ptype == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		header_len += FIL_PAGE_COMPRESSION_METHOD_SIZE;
	}

	/* Do not try to uncompressed pages that are not compressed */
	if (ptype !=  FIL_PAGE_PAGE_COMPRESSED &&
		ptype != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED &&
		ptype != FIL_PAGE_TYPE_COMPRESSED) {
		return;
	}

	// If no buffer was given, we need to allocate temporal buffer
	if (page_buf == NULL) {
		in_buf = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
		memset(in_buf, 0, UNIV_PAGE_SIZE);
	} else {
		in_buf = page_buf;
	}

	/* Before actual decompress, make sure that page type is correct */

	if (mach_read_from_4(buf+FIL_PAGE_SPACE_OR_CHKSUM) != BUF_NO_CHECKSUM_MAGIC ||
		(ptype != FIL_PAGE_PAGE_COMPRESSED &&
		 ptype != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED)) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Corruption: We try to uncompress corrupted page"
			" CRC %lu type %lu len %lu.",
			mach_read_from_4(buf+FIL_PAGE_SPACE_OR_CHKSUM),
			mach_read_from_2(buf+FIL_PAGE_TYPE), len);

		fflush(stderr);
		if (return_error) {
			goto error_return;
		}
		ut_error;
	}

	/* Get compression algorithm */
	if (ptype == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		compression_alg = mach_read_from_2(buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE);
	} else {
		compression_alg = mach_read_from_8(buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	}

	/* Get the actual size of compressed page */
	actual_size = mach_read_from_2(buf+FIL_PAGE_DATA);
	/* Check if payload size is corrupted */
	if (actual_size == 0 || actual_size > UNIV_PAGE_SIZE) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Corruption: We try to uncompress corrupted page"
			" actual size %lu compression %s.",
			actual_size, fil_get_compression_alg_name(compression_alg));
		fflush(stderr);
		if (return_error) {
			goto error_return;
		}
		ut_error;
	}

	/* Store actual payload size of the compressed data. This pointer
	points to buffer pool. */
	if (write_size) {
		*write_size = actual_size;
	}

#ifdef UNIV_PAGECOMPRESS_DEBUG
	ib_logf(IB_LOG_LEVEL_INFO,
		"Preparing for decompress for len %lu\n",
		actual_size);
#endif /* UNIV_PAGECOMPRESS_DEBUG */


	switch(compression_alg) {
	case PAGE_ZLIB_ALGORITHM:
		err= uncompress(in_buf, &len, buf+header_len, (unsigned long)actual_size);

		/* If uncompress fails it means that page is corrupted */
		if (err != Z_OK) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Corruption: Page is marked as compressed"
				" but uncompress failed with error %d "
				" size %lu len %lu.",
				err, actual_size, len);

			fflush(stderr);

			if (return_error) {
				goto error_return;
			}
			ut_error;
		}
		break;

#ifdef HAVE_LZ4
	case PAGE_LZ4_ALGORITHM:
		err = LZ4_decompress_fast((const char *)buf+header_len, (char *)in_buf, len);

		if (err != (int)actual_size) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Corruption: Page is marked as compressed"
				" but decompression read only %d bytes "
				" size %lu len %lu.",
				err, actual_size, len);
			fflush(stderr);

			if (return_error) {
				goto error_return;
			}
			ut_error;
		}
		break;
#endif /* HAVE_LZ4 */
#ifdef HAVE_LZO
	case PAGE_LZO_ALGORITHM: {
               	ulint olen=0;
		err = lzo1x_decompress((const unsigned char *)buf+header_len,
			actual_size,(unsigned char *)in_buf, &olen, NULL);

		if (err != LZO_E_OK || (olen == 0 || olen > UNIV_PAGE_SIZE)) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Corruption: Page is marked as compressed"
				" but decompression read only %ld bytes"
				" size %lu len %lu.",
				olen, actual_size, len);
			fflush(stderr);

			if (return_error) {
				goto error_return;
			}
			ut_error;
		}
		break;
        }
#endif /* HAVE_LZO */
#ifdef HAVE_LZMA
	case PAGE_LZMA_ALGORITHM: {

		lzma_ret	ret;
		size_t		src_pos = 0;
		size_t		dst_pos = 0;
		uint64_t 	memlimit = UINT64_MAX;

		ret = lzma_stream_buffer_decode(
			&memlimit,
			0,
			NULL,
			buf+header_len,
			&src_pos,
			actual_size,
			in_buf,
			&dst_pos,
			len);


		if (ret != LZMA_OK || (dst_pos == 0 || dst_pos > UNIV_PAGE_SIZE)) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Corruption: Page is marked as compressed"
				" but decompression read only %ld bytes"
				" size %lu len %lu.",
				dst_pos, actual_size, len);
			fflush(stderr);

			if (return_error) {
				goto error_return;
			}
			ut_error;
		}

		break;
	}
#endif /* HAVE_LZMA */
#ifdef HAVE_BZIP2
	case PAGE_BZIP2_ALGORITHM: {
		unsigned int dst_pos = UNIV_PAGE_SIZE;

		err = BZ2_bzBuffToBuffDecompress(
			(char *)in_buf,
			&dst_pos,
			(char *)(buf+header_len),
			actual_size,
			1,
			0);

		if (err != BZ_OK || (dst_pos == 0 || dst_pos > UNIV_PAGE_SIZE)) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Corruption: Page is marked as compressed"
				" but decompression read only %du bytes"
				" size %lu len %lu err %d.",
				dst_pos, actual_size, len, err);
			fflush(stderr);

			if (return_error) {
				goto error_return;
			}
			ut_error;
		}
		break;
	}
#endif /* HAVE_BZIP2 */
#ifdef HAVE_SNAPPY
	case PAGE_SNAPPY_ALGORITHM:
	{
		snappy_status cstatus;
		ulint olen = 0;

		cstatus = snappy_uncompress(
			(const char *)(buf+header_len),
			(size_t)actual_size,
			(char *)in_buf,
			(size_t*)&olen);

		if (cstatus != SNAPPY_OK || (olen == 0 || olen > UNIV_PAGE_SIZE)) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Corruption: Page is marked as compressed"
				" but decompression read only %lu bytes"
				" size %lu len %lu err %d.",
				olen, actual_size, len, (int)cstatus);
			fflush(stderr);

			if (return_error) {
				goto error_return;
			}
			ut_error;
		}
		break;
	}
#endif /* HAVE_SNAPPY */
	default:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Corruption: Page is marked as compressed"
			" but compression algorithm %s"
			" is not known."
			,fil_get_compression_alg_name(compression_alg));

		fflush(stderr);
		if (return_error) {
			goto error_return;
		}
		ut_error;
		break;
	}

	srv_stats.pages_page_decompressed.inc();

	/* Copy the uncompressed page to the buffer pool, not
	really any other options. */
	memcpy(buf, in_buf, len);

error_return:
	// Need to free temporal buffer if no buffer was given
	if (page_buf == NULL) {
		ut_free(in_buf);
	}
}
