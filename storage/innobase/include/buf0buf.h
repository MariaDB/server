/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/buf0buf.h
The database buffer pool high-level routines

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#ifndef buf0buf_h
#define buf0buf_h

/** Magic value to use instead of checksums when they are disabled */
#define BUF_NO_CHECKSUM_MAGIC 0xDEADBEEFUL

#include "fil0fil.h"
#include "mtr0types.h"
#include "buf0types.h"
#include "span.h"
#ifndef UNIV_INNOCHECKSUM
#include "hash0hash.h"
#include "ut0byte.h"
#include "page0types.h"
#include "ut0rbt.h"
#include "os0proc.h"
#include "log0log.h"
#include "srv0srv.h"
#include <ostream>

// Forward declaration
struct fil_addr_t;

/** @name Modes for buf_page_get_gen */
/* @{ */
#define BUF_GET			10	/*!< get always */
#define	BUF_GET_IF_IN_POOL	11	/*!< get if in pool */
#define BUF_PEEK_IF_IN_POOL	12	/*!< get if in pool, do not make
					the block young in the LRU list */
#define BUF_GET_NO_LATCH	14	/*!< get and bufferfix, but
					set no latch; we have
					separated this case, because
					it is error-prone programming
					not to set a latch, and it
					should be used with care */
#define BUF_GET_IF_IN_POOL_OR_WATCH	15
					/*!< Get the page only if it's in the
					buffer pool, if not then set a watch
					on the page. */
#define BUF_GET_POSSIBLY_FREED		16
					/*!< Like BUF_GET, but do not mind
					if the file page has been freed. */
#define BUF_EVICT_IF_IN_POOL	20	/*!< evict a clean block if found */
/* @} */

#define BUF_POOL_WATCH_SIZE		(srv_n_purge_threads + 1)
					/*!< Maximum number of concurrent
					buffer pool watches */
#define MAX_PAGE_HASH_LOCKS	1024	/*!< The maximum number of
					page_hash locks */

# ifdef UNIV_DEBUG
extern my_bool	buf_disable_resize_buffer_pool_debug; /*!< if TRUE, resizing
					buffer pool is not allowed. */
# endif /* UNIV_DEBUG */

/** @brief States of a control block
@see buf_page_t

The enumeration values must be 0..7. */
enum buf_page_state {
	BUF_BLOCK_POOL_WATCH,		/*!< a sentinel for the buffer pool
					watch, element of buf_pool.watch[] */
	BUF_BLOCK_ZIP_PAGE,		/*!< contains a clean
					compressed page */
	BUF_BLOCK_ZIP_DIRTY,		/*!< contains a compressed
					page that is in the
					buf_pool.flush_list */

	BUF_BLOCK_NOT_USED,		/*!< is in the free list;
					must be after the BUF_BLOCK_ZIP_
					constants for compressed-only pages
					@see buf_block_state_valid() */
	BUF_BLOCK_READY_FOR_USE,	/*!< when buf_LRU_get_free_block
					returns a block, it is in this state */
	BUF_BLOCK_FILE_PAGE,		/*!< contains a buffered file page */
	BUF_BLOCK_MEMORY,		/*!< contains some main memory
					object */
	BUF_BLOCK_REMOVE_HASH		/*!< hash index should be removed
					before putting to the free list */
};

/** This structure defines information we will fetch from each buffer pool. It
will be used to print table IO stats */
struct buf_pool_info_t
{
	/* General buffer pool info */
	ulint	pool_size;		/*!< Buffer Pool size in pages */
	ulint	lru_len;		/*!< Length of buf_pool.LRU */
	ulint	old_lru_len;		/*!< buf_pool.LRU_old_len */
	ulint	free_list_len;		/*!< Length of buf_pool.free list */
	ulint	flush_list_len;		/*!< Length of buf_pool.flush_list */
	ulint	n_pend_unzip;		/*!< buf_pool.n_pend_unzip, pages
					pending decompress */
	ulint	n_pend_reads;		/*!< buf_pool.n_pend_reads, pages
					pending read */
	ulint	n_pending_flush_lru;	/*!< Pages pending flush in LRU */
	ulint	n_pending_flush_single_page;/*!< Pages pending to be
					flushed as part of single page
					flushes issued by various user
					threads */
	ulint	n_pending_flush_list;	/*!< Pages pending flush in FLUSH
					LIST */
	ulint	n_pages_made_young;	/*!< number of pages made young */
	ulint	n_pages_not_made_young;	/*!< number of pages not made young */
	ulint	n_pages_read;		/*!< buf_pool.n_pages_read */
	ulint	n_pages_created;	/*!< buf_pool.n_pages_created */
	ulint	n_pages_written;	/*!< buf_pool.n_pages_written */
	ulint	n_page_gets;		/*!< buf_pool.n_page_gets */
	ulint	n_ra_pages_read_rnd;	/*!< buf_pool.n_ra_pages_read_rnd,
					number of pages readahead */
	ulint	n_ra_pages_read;	/*!< buf_pool.n_ra_pages_read, number
					of pages readahead */
	ulint	n_ra_pages_evicted;	/*!< buf_pool.n_ra_pages_evicted,
					number of readahead pages evicted
					without access */
	ulint	n_page_get_delta;	/*!< num of buffer pool page gets since
					last printout */

	/* Buffer pool access stats */
	double	page_made_young_rate;	/*!< page made young rate in pages
					per second */
	double	page_not_made_young_rate;/*!< page not made young rate
					in pages per second */
	double	pages_read_rate;	/*!< num of pages read per second */
	double	pages_created_rate;	/*!< num of pages create per second */
	double	pages_written_rate;	/*!< num of  pages written per second */
	ulint	page_read_delta;	/*!< num of pages read since last
					printout */
	ulint	young_making_delta;	/*!< num of pages made young since
					last printout */
	ulint	not_young_making_delta;	/*!< num of pages not make young since
					last printout */

	/* Statistics about read ahead algorithm.  */
	double	pages_readahead_rnd_rate;/*!< random readahead rate in pages per
					second */
	double	pages_readahead_rate;	/*!< readahead rate in pages per
					second */
	double	pages_evicted_rate;	/*!< rate of readahead page evicted
					without access, in pages per second */

	/* Stats about LRU eviction */
	ulint	unzip_lru_len;		/*!< length of buf_pool.unzip_LRU
					list */
	/* Counters for LRU policy */
	ulint	io_sum;			/*!< buf_LRU_stat_sum.io */
	ulint	io_cur;			/*!< buf_LRU_stat_cur.io, num of IO
					for current interval */
	ulint	unzip_sum;		/*!< buf_LRU_stat_sum.unzip */
	ulint	unzip_cur;		/*!< buf_LRU_stat_cur.unzip, num
					pages decompressed in current
					interval */
};
#endif /* !UNIV_INNOCHECKSUM */

/** Print the given page_id_t object.
@param[in,out]	out	the output stream
@param[in]	page_id	the page_id_t object to be printed
@return the output stream */
std::ostream&
operator<<(
	std::ostream&		out,
	const page_id_t		page_id);

#ifndef UNIV_INNOCHECKSUM
/*********************************************************************//**
Gets the current size of buffer buf_pool in bytes.
@return size in bytes */
UNIV_INLINE
ulint
buf_pool_get_curr_size(void);
/*========================*/
/**
@return the smallest oldest_modification lsn for any page.
@retval 0	if all modified persistent pages have been flushed */
lsn_t
buf_pool_get_oldest_modification();

/********************************************************************//**
Allocates a buf_page_t descriptor. This function must succeed. In case
of failure we assert in this function. */
UNIV_INLINE
buf_page_t*
buf_page_alloc_descriptor(void)
/*===========================*/
	MY_ATTRIBUTE((malloc));
/********************************************************************//**
Free a buf_page_t descriptor. */
UNIV_INLINE
void
buf_page_free_descriptor(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: bpage descriptor to free. */
	MY_ATTRIBUTE((nonnull));

/** Allocate a buffer block.
@return own: the allocated block, in state BUF_BLOCK_MEMORY */
buf_block_t*
buf_block_alloc();
/********************************************************************//**
Frees a buffer block which does not contain a file page. */
UNIV_INLINE
void
buf_block_free(
/*===========*/
	buf_block_t*	block);	/*!< in, own: block to be freed */

/**************************************************************//**
NOTE! The following macros should be used instead of buf_page_get_gen,
to improve debugging. Only values RW_S_LATCH and RW_X_LATCH are allowed
in LA! */
#define buf_page_get(ID, SIZE, LA, MTR)					\
	buf_page_get_gen(ID, SIZE, LA, NULL, BUF_GET, __FILE__, __LINE__, MTR)

/**************************************************************//**
Use these macros to bufferfix a page with no latching. Remember not to
read the contents of the page unless you know it is safe. Do not modify
the contents of the page! We have separated this case, because it is
error-prone programming not to set a latch, and it should be used
with care. */
#define buf_page_get_with_no_latch(ID, SIZE, MTR)	\
	buf_page_get_gen(ID, SIZE, RW_NO_LATCH, NULL, BUF_GET_NO_LATCH, \
			 __FILE__, __LINE__, MTR)
/********************************************************************//**
This is the general function used to get optimistic access to a database
page.
@return TRUE if success */
ibool
buf_page_optimistic_get(
/*====================*/
	ulint		rw_latch,/*!< in: RW_S_LATCH, RW_X_LATCH */
	buf_block_t*	block,	/*!< in: guessed block */
	ib_uint64_t	modify_clock,/*!< in: modify clock value */
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line where called */
	mtr_t*		mtr);	/*!< in: mini-transaction */

/** Given a tablespace id and page number tries to get that page. If the
page is not in the buffer pool it is not loaded and NULL is returned.
Suitable for using when holding the lock_sys_t::mutex.
@param[in]	page_id	page id
@param[in]	file	file name
@param[in]	line	line where called
@param[in]	mtr	mini-transaction
@return pointer to a page or NULL */
buf_block_t*
buf_page_try_get_func(
	const page_id_t		page_id,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr);

/** Tries to get a page.
If the page is not in the buffer pool it is not loaded. Suitable for using
when holding the lock_sys_t::mutex.
@param[in]	page_id	page identifier
@param[in]	mtr	mini-transaction
@return the page if in buffer pool, NULL if not */
#define buf_page_try_get(page_id, mtr)	\
	buf_page_try_get_func((page_id), __FILE__, __LINE__, mtr);

/** Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with buf_page_release_zip().
NOTE: the page is not protected by any latch.  Mutual exclusion has to
be implemented at a higher level.  In other words, all possible
accesses to a given page through this function must be protected by
the same set of mutexes or latches.
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size
@return pointer to the block */
buf_page_t* buf_page_get_zip(const page_id_t page_id, ulint zip_size);

/** Get access to a database page. Buffered redo log may be applied.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	file			file name
@param[in]	line			line where called
@param[in]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@param[in]	allow_ibuf_merge	Allow change buffer merge while
reading the pages from file.
@return pointer to the block or NULL */
buf_block_t*
buf_page_get_gen(
	const page_id_t		page_id,
	ulint			zip_size,
	ulint			rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr,
	dberr_t*		err = NULL,
	bool			allow_ibuf_merge = false);

/** This is the low level function used to get access to a database page.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
@param[in]	file			file name
@param[in]	line			line where called
@param[in]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@param[in]	allow_ibuf_merge	Allow change buffer merge to happen
while reading the page from file
then it makes sure that it does merging of change buffer changes while
reading the page from file.
@return pointer to the block or NULL */
buf_block_t*
buf_page_get_low(
	const page_id_t		page_id,
	ulint			zip_size,
	ulint			rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	const char*		file,
	unsigned		line,
	mtr_t*			mtr,
	dberr_t*		err,
	bool			allow_ibuf_merge);

/** Initialize a page in the buffer pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED =>
FILE_PAGE (the other is buf_page_get_gen).
@param[in]	page_id		page id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create(const page_id_t page_id, ulint zip_size, mtr_t *mtr);

/********************************************************************//**
Releases a compressed-only page acquired with buf_page_get_zip(). */
UNIV_INLINE
void
buf_page_release_zip(
/*=================*/
	buf_page_t*	bpage);		/*!< in: buffer block */
/********************************************************************//**
Releases a latch, if specified. */
UNIV_INLINE
void
buf_page_release_latch(
/*=====================*/
	buf_block_t*	block,		/*!< in: buffer block */
	ulint		rw_latch);	/*!< in: RW_S_LATCH, RW_X_LATCH,
					RW_NO_LATCH */
/********************************************************************//**
Moves a page to the start of the buffer pool LRU list. This high-level
function can be used to prevent an important page from slipping out of
the buffer pool. */
void
buf_page_make_young(
/*================*/
	buf_page_t*	bpage);	/*!< in: buffer block of a file page */

/** Mark the page status as FREED for the given tablespace id and
page number. If the page is not in buffer pool then ignore it.
@param[in]	page_id	page_id
@param[in,out]	mtr	mini-transaction
@param[in]	file	file name
@param[in]	line	line where called */
void buf_page_free(const page_id_t page_id,
                   mtr_t *mtr,
                   const char *file,
                   unsigned line);

/********************************************************************//**
Reads the freed_page_clock of a buffer block.
@return freed_page_clock */
UNIV_INLINE
unsigned
buf_page_get_freed_page_clock(
/*==========================*/
	const buf_page_t*	bpage)	/*!< in: block */
	MY_ATTRIBUTE((warn_unused_result));
/********************************************************************//**
Reads the freed_page_clock of a buffer block.
@return freed_page_clock */
UNIV_INLINE
unsigned
buf_block_get_freed_page_clock(
/*===========================*/
	const buf_block_t*	block)	/*!< in: block */
	MY_ATTRIBUTE((warn_unused_result));

/** Determine if a block is still close enough to the MRU end of the LRU list
meaning that it is not in danger of getting evicted and also implying
that it has been accessed recently.
Note that this is for heuristics only and does not reserve buffer pool
mutex.
@param[in]	bpage		buffer pool page
@return whether bpage is close to MRU end of LRU */
inline bool buf_page_peek_if_young(const buf_page_t *bpage);

/** Determine if a block should be moved to the start of the LRU list if
there is danger of dropping from the buffer pool.
@param[in]	bpage		buffer pool page
@return true if bpage should be made younger */
inline bool buf_page_peek_if_too_old(const buf_page_t *bpage);

/** Move a page to the start of the buffer pool LRU list if it is too old.
@param[in,out]	bpage		buffer pool page */
inline void buf_page_make_young_if_needed(buf_page_t *bpage)
{
	if (UNIV_UNLIKELY(buf_page_peek_if_too_old(bpage))) {
		buf_page_make_young(bpage);
	}
}

/********************************************************************//**
Increments the modify clock of a frame by 1. The caller must (1) own the
buf_pool.mutex and block bufferfix count has to be zero, (2) or own an x-lock
on the block. */
UNIV_INLINE
void
buf_block_modify_clock_inc(
/*=======================*/
	buf_block_t*	block);	/*!< in: block */
/********************************************************************//**
Returns the value of the modify clock. The caller must have an s-lock
or x-lock on the block.
@return value */
UNIV_INLINE
ib_uint64_t
buf_block_get_modify_clock(
/*=======================*/
	buf_block_t*	block);	/*!< in: block */
/*******************************************************************//**
Increments the bufferfix count. */
UNIV_INLINE
void
buf_block_buf_fix_inc_func(
/*=======================*/
# ifdef UNIV_DEBUG
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line */
# endif /* UNIV_DEBUG */
	buf_block_t*	block)	/*!< in/out: block to bufferfix */
	MY_ATTRIBUTE((nonnull));

# ifdef UNIV_DEBUG
/** Increments the bufferfix count.
@param[in,out]	b	block to bufferfix
@param[in]	f	file name where requested
@param[in]	l	line number where requested */
#  define buf_block_buf_fix_inc(b,f,l) buf_block_buf_fix_inc_func(f,l,b)
# else /* UNIV_DEBUG */
/** Increments the bufferfix count.
@param[in,out]	b	block to bufferfix
@param[in]	f	file name where requested
@param[in]	l	line number where requested */
#  define buf_block_buf_fix_inc(b,f,l) buf_block_buf_fix_inc_func(b)
# endif /* UNIV_DEBUG */
#endif /* !UNIV_INNOCHECKSUM */

/** Check if a buffer is all zeroes.
@param[in]	buf	data to check
@return whether the buffer is all zeroes */
bool buf_is_zeroes(st_::span<const byte> buf);

/** Checks if the page is in crc32 checksum format.
@param[in]	read_buf		database page
@param[in]	checksum_field1		new checksum field
@param[in]	checksum_field2		old checksum field
@return true if the page is in crc32 checksum format. */
bool
buf_page_is_checksum_valid_crc32(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2)
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/** Checks if the page is in innodb checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in innodb checksum format. */
bool
buf_page_is_checksum_valid_innodb(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2)
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/** Checks if the page is in none checksum format.
@param[in]	read_buf	database page
@param[in]	checksum_field1	new checksum field
@param[in]	checksum_field2	old checksum field
@return true if the page is in none checksum format. */
bool
buf_page_is_checksum_valid_none(
	const byte*			read_buf,
	ulint				checksum_field1,
	ulint				checksum_field2)
	MY_ATTRIBUTE((nonnull(1), warn_unused_result));

/** Check if a page is corrupt.
@param[in]	check_lsn	whether the LSN should be checked
@param[in]	read_buf	database page
@param[in]	fsp_flags	tablespace flags
@return whether the page is corrupted */
bool
buf_page_is_corrupted(
	bool			check_lsn,
	const byte*		read_buf,
	ulint			fsp_flags)
	MY_ATTRIBUTE((warn_unused_result));

inline void *aligned_malloc(size_t size, size_t align)
{
#ifdef _MSC_VER
  return _aligned_malloc(size, align);
#else
  void *result;
  if (posix_memalign(&result, align, size))
    result= NULL;
  return result;
#endif
}

inline void aligned_free(void *ptr)
{
#ifdef _MSC_VER
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

/** Read the key version from the page. In full crc32 format,
key version is stored at {0-3th} bytes. In other format, it is
stored in 26th position.
@param[in]	read_buf	database page
@param[in]	fsp_flags	tablespace flags
@return key version of the page. */
inline uint32_t buf_page_get_key_version(const byte* read_buf, ulint fsp_flags)
{
	return fil_space_t::full_crc32(fsp_flags)
		? mach_read_from_4(read_buf + FIL_PAGE_FCRC32_KEY_VERSION)
		: mach_read_from_4(read_buf
				   + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
}

/** Read the compression info from the page. In full crc32 format,
compression info is at MSB of page type. In other format, it is
stored in page type.
@param[in]	read_buf	database page
@param[in]	fsp_flags	tablespace flags
@return true if page is compressed. */
inline bool buf_page_is_compressed(const byte* read_buf, ulint fsp_flags)
{
	ulint page_type = mach_read_from_2(read_buf + FIL_PAGE_TYPE);
	return fil_space_t::full_crc32(fsp_flags)
		? !!(page_type & 1U << FIL_PAGE_COMPRESS_FCRC32_MARKER)
		: page_type == FIL_PAGE_PAGE_COMPRESSED;
}

/** Get the compressed or uncompressed size of a full_crc32 page.
@param[in]	buf	page_compressed or uncompressed page
@param[out]	comp	whether the page could be compressed
@param[out]	cr	whether the page could be corrupted
@return the payload size in the file page */
inline uint buf_page_full_crc32_size(const byte* buf, bool* comp, bool* cr)
{
	uint t = mach_read_from_2(buf + FIL_PAGE_TYPE);
	uint page_size = uint(srv_page_size);

	if (!(t & 1U << FIL_PAGE_COMPRESS_FCRC32_MARKER)) {
		return page_size;
	}

	t &= ~(1U << FIL_PAGE_COMPRESS_FCRC32_MARKER);
	t <<= 8;

	if (t < page_size) {
		page_size = t;
		if (comp) {
			*comp = true;
		}
	} else if (cr) {
		*cr = true;
	}

	return page_size;
}

#ifndef UNIV_INNOCHECKSUM
/**********************************************************************//**
Gets the hash value of a block. This can be used in searches in the
lock hash table.
@return lock hash value */
UNIV_INLINE
unsigned
buf_block_get_lock_hash_val(
/*========================*/
	const buf_block_t*	block)	/*!< in: block */
	MY_ATTRIBUTE((warn_unused_result));
#ifdef UNIV_DEBUG
/** Find a block in the buffer pool that points to a given compressed page.
@param[in]	data		pointer to compressed page
@return buffer block pointing to the compressed page
@retval NULL if not found */
buf_block_t* buf_pool_contains_zip(const void* data);
#endif /* UNIV_DEBUG */

/***********************************************************************
FIXME_FTS: Gets the frame the pointer is pointing to. */
UNIV_INLINE
buf_frame_t*
buf_frame_align(
/*============*/
                        /* out: pointer to frame */
        byte*   ptr);   /* in: pointer to a frame */


/** Dump a page to stderr.
@param[in]	read_buf	database page
@param[in]	zip_size	compressed page size, or 0 */
void buf_page_print(const byte* read_buf, ulint zip_size = 0)
	ATTRIBUTE_COLD __attribute__((nonnull));
/********************************************************************//**
Decompress a block.
@return TRUE if successful */
ibool
buf_zip_decompress(
/*===============*/
	buf_block_t*	block,	/*!< in/out: block */
	ibool		check);	/*!< in: TRUE=verify the page checksum */

#ifdef UNIV_DEBUG
/** @return the number of latched pages in the buffer pool */
ulint buf_get_latched_pages_number();
#endif /* UNIV_DEBUG */
/*********************************************************************//**
Prints info of the buffer i/o. */
void
buf_print_io(
/*=========*/
	FILE*	file);	/*!< in: file where to print */
/** Collect buffer pool metadata.
@param[out]	pool_info	buffer pool metadata */
void buf_stats_get_pool_info(buf_pool_info_t *pool_info);

/** Refresh the statistics used to print per-second averages. */
void buf_refresh_io_stats();

/** Check that there currently are no I/O operations pending.
@return number of pending i/o */
ulint buf_pool_check_no_pending_io();

/** Invalidate all pages in the buffer pool.
All pages must be in a replaceable state (not modified or latched). */
void buf_pool_invalidate();

/*========================================================================
--------------------------- LOWER LEVEL ROUTINES -------------------------
=========================================================================*/

#ifdef UNIV_DEBUG
/*********************************************************************//**
Adds latch level info for the rw-lock protecting the buffer frame. This
should be called in the debug version after a successful latching of a
page if we know the latching order level of the acquired latch. */
UNIV_INLINE
void
buf_block_dbg_add_level(
/*====================*/
	buf_block_t*	block,	/*!< in: buffer page
				where we have acquired latch */
	latch_level_t	level);	/*!< in: latching order level */
#else /* UNIV_DEBUG */
# define buf_block_dbg_add_level(block, level) /* nothing */
#endif /* UNIV_DEBUG */
/*********************************************************************//**
Gets the state of a block.
@return state */
UNIV_INLINE
enum buf_page_state
buf_page_get_state(
/*===============*/
	const buf_page_t*	bpage);	/*!< in: pointer to the control
					block */
/*********************************************************************//**
Gets the state of a block.
@return state */
UNIV_INLINE
enum buf_page_state
buf_block_get_state(
/*================*/
	const buf_block_t*	block)	/*!< in: pointer to the control block */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Sets the state of a block. */
UNIV_INLINE
void
buf_page_set_state(
/*===============*/
	buf_page_t*		bpage,	/*!< in/out: pointer to control block */
	enum buf_page_state	state);	/*!< in: state */
/*********************************************************************//**
Sets the state of a block. */
UNIV_INLINE
void
buf_block_set_state(
/*================*/
	buf_block_t*		block,	/*!< in/out: pointer to control block */
	enum buf_page_state	state);	/*!< in: state */
/*********************************************************************//**
Determines if a block is mapped to a tablespace.
@return TRUE if mapped */
UNIV_INLINE
ibool
buf_page_in_file(
/*=============*/
	const buf_page_t*	bpage)	/*!< in: pointer to control block */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Determines if a block should be on unzip_LRU list.
@return TRUE if block belongs to unzip_LRU */
UNIV_INLINE
ibool
buf_page_belongs_to_unzip_LRU(
/*==========================*/
	const buf_page_t*	bpage)	/*!< in: pointer to control block */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Gets the mutex of a block.
@return pointer to mutex protecting bpage */
UNIV_INLINE
BPageMutex*
buf_page_get_mutex(
/*===============*/
	const buf_page_t*	bpage)	/*!< in: pointer to control block */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Get the flush type of a page.
@return flush type */
UNIV_INLINE
buf_flush_t
buf_page_get_flush_type(
/*====================*/
	const buf_page_t*	bpage)	/*!< in: buffer page */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Set the flush type of a page. */
UNIV_INLINE
void
buf_page_set_flush_type(
/*====================*/
	buf_page_t*	bpage,		/*!< in: buffer page */
	buf_flush_t	flush_type);	/*!< in: flush type */

/** Map a block to a file page.
@param[in,out]	block	pointer to control block
@param[in]	page_id	page id */
UNIV_INLINE
void
buf_block_set_file_page(
	buf_block_t*		block,
	const page_id_t		page_id);

/*********************************************************************//**
Gets the io_fix state of a block.
@return io_fix state */
UNIV_INLINE
enum buf_io_fix
buf_page_get_io_fix(
/*================*/
	const buf_page_t*	bpage)	/*!< in: pointer to the control block */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Gets the io_fix state of a block.
@return io_fix state */
UNIV_INLINE
enum buf_io_fix
buf_block_get_io_fix(
/*================*/
	const buf_block_t*	block)	/*!< in: pointer to the control block */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Sets the io_fix state of a block. */
UNIV_INLINE
void
buf_page_set_io_fix(
/*================*/
	buf_page_t*	bpage,	/*!< in/out: control block */
	enum buf_io_fix	io_fix);/*!< in: io_fix state */
/*********************************************************************//**
Sets the io_fix state of a block. */
UNIV_INLINE
void
buf_block_set_io_fix(
/*=================*/
	buf_block_t*	block,	/*!< in/out: control block */
	enum buf_io_fix	io_fix);/*!< in: io_fix state */
/*********************************************************************//**
Makes a block sticky. A sticky block implies that even after we release
the buf_pool.mutex and the block->mutex:
* it cannot be removed from the flush_list
* the block descriptor cannot be relocated
* it cannot be removed from the LRU list
Note that:
* the block can still change its position in the LRU list
* the next and previous pointers can change. */
UNIV_INLINE
void
buf_page_set_sticky(
/*================*/
	buf_page_t*	bpage);	/*!< in/out: control block */
/*********************************************************************//**
Removes stickiness of a block. */
UNIV_INLINE
void
buf_page_unset_sticky(
/*==================*/
	buf_page_t*	bpage);	/*!< in/out: control block */
/********************************************************************//**
Determine if a buffer block can be relocated in memory.  The block
can be dirty, but it must not be I/O-fixed or bufferfixed. */
UNIV_INLINE
ibool
buf_page_can_relocate(
/*==================*/
	const buf_page_t*	bpage)	/*!< control block being relocated */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Determine if a block has been flagged old.
@return TRUE if old */
UNIV_INLINE
ibool
buf_page_is_old(
/*============*/
	const buf_page_t*	bpage)	/*!< in: control block */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Flag a block old. */
UNIV_INLINE
void
buf_page_set_old(
/*=============*/
	buf_page_t*	bpage,	/*!< in/out: control block */
	bool		old);	/*!< in: old */
/*********************************************************************//**
Determine the time of first access of a block in the buffer pool.
@return ut_time_ms() at the time of first access, 0 if not accessed */
UNIV_INLINE
unsigned
buf_page_is_accessed(
/*=================*/
	const buf_page_t*	bpage)	/*!< in: control block */
	MY_ATTRIBUTE((warn_unused_result));
/*********************************************************************//**
Flag a block accessed. */
UNIV_INLINE
void
buf_page_set_accessed(
/*==================*/
	buf_page_t*	bpage)		/*!< in/out: control block */
	MY_ATTRIBUTE((nonnull));
/*********************************************************************//**
Gets the buf_block_t handle of a buffered file block if an uncompressed
page frame exists, or NULL. Note: even though bpage is not declared a
const we don't update its value.
@return control block, or NULL */
UNIV_INLINE
buf_block_t*
buf_page_get_block(
/*===============*/
	buf_page_t*	bpage)	/*!< in: control block, or NULL */
	MY_ATTRIBUTE((warn_unused_result));

#ifdef UNIV_DEBUG
/*********************************************************************//**
Gets a pointer to the memory frame of a block.
@return pointer to the frame */
UNIV_INLINE
buf_frame_t*
buf_block_get_frame(
/*================*/
	const buf_block_t*	block)	/*!< in: pointer to the control block */
	MY_ATTRIBUTE((warn_unused_result));
#else /* UNIV_DEBUG */
# define buf_block_get_frame(block) (block)->frame
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Gets the compressed page descriptor corresponding to an uncompressed page
if applicable. */
#define buf_block_get_page_zip(block) \
	(UNIV_LIKELY_NULL((block)->page.zip.data) ? &(block)->page.zip : NULL)
#define is_buf_block_get_page_zip(block) \
        UNIV_LIKELY_NULL((block)->page.zip.data)

/** Initialize a page for read to the buffer buf_pool. If the page is
(1) already in buf_pool, or
(2) if we specify to read only ibuf pages and the page is not an ibuf page, or
(3) if the space is deleted or being deleted,
then this function does nothing.
Sets the io_fix flag to BUF_IO_READ and sets a non-recursive exclusive lock
on the buffer frame. The io-handler must take care that the flag is cleared
and the lock released later.
@param[out]	err			DB_SUCCESS or DB_TABLESPACE_DELETED
@param[in]	mode			BUF_READ_IBUF_PAGES_ONLY, ...
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	unzip			whether the uncompressed page is
					requested (for ROW_FORMAT=COMPRESSED)
@return pointer to the block
@retval	NULL	in case of an error */
buf_page_t*
buf_page_init_for_read(
	dberr_t*		err,
	ulint			mode,
	const page_id_t		page_id,
	ulint			zip_size,
	bool			unzip);

/** Complete a read or write request of a file page to or from the buffer pool.
@param[in,out]	bpage	page to complete
@param[in]	dblwr	whether the doublewrite buffer was used (on write)
@param[in]	evict	whether or not to evict the page from LRU list
@return whether the operation succeeded
@retval	DB_SUCCESS		always when writing, or if a read page was OK
@retval	DB_PAGE_CORRUPTED	if the checksum fails on a page read
@retval	DB_DECRYPTION_FAILED	if page post encryption checksum matches but
				after decryption normal page checksum does
				not match */
UNIV_INTERN
dberr_t
buf_page_io_complete(
	buf_page_t*	bpage,
	bool		dblwr = false,
	bool		evict = false)
	MY_ATTRIBUTE((nonnull));

/** Returns the control block of a file page, NULL if not found.
@param[in]	page_id		page id
@return block, NULL if not found */
inline buf_page_t *buf_page_hash_get_low(const page_id_t page_id);

/** Returns the control block of a file page, NULL if not found.
If the block is found and lock is not NULL then the appropriate
page_hash lock is acquired in the specified lock mode. Otherwise,
mode value is ignored. It is up to the caller to release the
lock. If the block is found and the lock is NULL then the page_hash
lock is released by this function.
@param[in]	page_id		page id
@param[in,out]	lock		lock of the page hash acquired if bpage is
found, NULL otherwise. If NULL is passed then the hash_lock is released by
this function.
@param[in]	lock_mode	RW_LOCK_X or RW_LOCK_S. Ignored if
lock == NULL
@param[in]	watch		if true, return watch sentinel also.
@return pointer to the bpage or NULL; if NULL, lock is also NULL or
a watch sentinel. */
UNIV_INLINE
buf_page_t*
buf_page_hash_get_locked(
	const page_id_t		page_id,
	rw_lock_t**		lock,
	ulint			lock_mode,
	bool			watch = false);

/** Returns the control block of a file page, NULL if not found.
If the block is found and lock is not NULL then the appropriate
page_hash lock is acquired in the specified lock mode. Otherwise,
mode value is ignored. It is up to the caller to release the
lock. If the block is found and the lock is NULL then the page_hash
lock is released by this function.
@param[in]	page_id		page id
@param[in,out]	lock		lock of the page hash acquired if bpage is
found, NULL otherwise. If NULL is passed then the hash_lock is released by
this function.
@param[in]	lock_mode	RW_LOCK_X or RW_LOCK_S. Ignored if
lock == NULL
@return pointer to the block or NULL; if NULL, lock is also NULL. */
UNIV_INLINE
buf_block_t*
buf_block_hash_get_locked(
	const page_id_t		page_id,
	rw_lock_t**		lock,
	ulint			lock_mode);

/* There are four different ways we can try to get a bpage or block
from the page hash:
1) Caller already holds the appropriate page hash lock: in the case call
buf_page_hash_get_low() function.
2) Caller wants to hold page hash lock in x-mode
3) Caller wants to hold page hash lock in s-mode
4) Caller doesn't want to hold page hash lock */
#define buf_page_hash_get_s_locked(page_id, l)		\
	buf_page_hash_get_locked(page_id, l, RW_LOCK_S)
#define buf_page_hash_get_x_locked(page_id, l)		\
	buf_page_hash_get_locked(page_id, l, RW_LOCK_X)
#define buf_page_hash_get(page_id)				\
	buf_page_hash_get_locked(page_id, NULL, 0)
#define buf_page_get_also_watch(page_id)			\
	buf_page_hash_get_locked(page_id, NULL, 0, true)

#define buf_block_hash_get_s_locked(page_id, l)		\
	buf_block_hash_get_locked(page_id, l, RW_LOCK_S)
#define buf_block_hash_get_x_locked(page_id, l)		\
	buf_block_hash_get_locked(page_id, l, RW_LOCK_X)
#define buf_block_hash_get(page_id)				\
	buf_block_hash_get_locked(page_id, NULL, 0)

/** Determine if a block is a sentinel for a buffer pool watch.
@param[in]	bpage		block
@return whether bpage a sentinel for a buffer pool watch */
bool buf_pool_watch_is_sentinel(const buf_page_t* bpage)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Stop watching if the page has been read in.
buf_pool_watch_set(space,offset) must have returned NULL before.
@param[in]	page_id	page id */
void buf_pool_watch_unset(const page_id_t page_id);

/** Check if the page has been read in.
This may only be called after buf_pool_watch_set(space,offset)
has returned NULL and before invoking buf_pool_watch_unset(space,offset).
@param[in]	page_id	page id
@return FALSE if the given page was not read in, TRUE if it was */
bool buf_pool_watch_occurred(const page_id_t page_id)
MY_ATTRIBUTE((warn_unused_result));

/** Calculate aligned buffer pool size based on srv_buf_pool_chunk_unit,
if needed.
@param[in]	size	size in bytes
@return	aligned size */
UNIV_INLINE
ulint
buf_pool_size_align(
	ulint	size);

/** Verify that post encryption checksum match with the calculated checksum.
This function should be called only if tablespace contains crypt data metadata.
@param[in]	page		page frame
@param[in]	fsp_flags	tablespace flags
@return true if page is encrypted and OK, false otherwise */
bool buf_page_verify_crypt_checksum(
	const byte*	page,
	ulint		fsp_flags);

/** Calculate a ROW_FORMAT=COMPRESSED page checksum and update the page.
@param[in,out]	page		page to update
@param[in]	size		compressed page size */
void buf_flush_update_zip_checksum(buf_frame_t* page, ulint size);

/** @brief The temporary memory structure.

NOTE! The definition appears here only for other modules of this
directory (buf) to see it. Do not use from outside! */

class buf_tmp_buffer_t
{
  /** whether this slot is reserved */
  std::atomic<bool> reserved;
public:
  /** For encryption, the data needs to be copied to a separate buffer
  before it's encrypted&written. The buffer block itself can be replaced
  while a write of crypt_buf to file is in progress. */
  byte *crypt_buf;
  /** buffer for fil_page_compress(), for flushing page_compressed pages */
  byte *comp_buf;
  /** pointer to resulting buffer after encryption or compression;
  not separately allocated memory */
  byte *out_buf;

  /** Release the slot */
  void release() { reserved.store(false, std::memory_order_relaxed); }

  /** Acquire the slot
  @return whether the slot was acquired */
  bool acquire() { return !reserved.exchange(true, std::memory_order_relaxed);}

  /** Allocate a buffer for encryption, decryption or decompression. */
  void allocate()
  {
    if (!crypt_buf)
      crypt_buf= static_cast<byte*>
      (aligned_malloc(srv_page_size, srv_page_size));
  }
};

/** The common buffer control block structure
for compressed and uncompressed frames */

/** Number of bits used for buffer page states. */
#define BUF_PAGE_STATE_BITS	3

class buf_page_t {
public:
	/** @name General fields
	None of these bit-fields must be modified without holding
	buf_page_get_mutex() [buf_block_t::mutex or
	buf_pool.zip_mutex], since they can be stored in the same
	machine word.  Some of these fields are additionally protected
	by buf_pool.mutex. */
	/* @{ */

	/** Page id. Protected by buf_pool mutex. */
	page_id_t	id;
	buf_page_t*	hash;		/*!< node used in chaining to
					buf_pool.page_hash or
					buf_pool.zip_hash */

	/** Count of how manyfold this block is currently bufferfixed. */
	Atomic_counter<uint32_t>	buf_fix_count;

	/** type of pending I/O operation; also protected by
	buf_pool.mutex for writes only */
	buf_io_fix	io_fix;

	/** Block state. @see buf_page_in_file */
	buf_page_state	state;

	unsigned	flush_type:2;	/*!< if this block is currently being
					flushed to disk, this tells the
					flush_type.
					@see buf_flush_t */
	/* @} */
	page_zip_des_t	zip;		/*!< compressed page; zip.data
					(but not the data it points to) is
					also protected by buf_pool.mutex;
					state == BUF_BLOCK_ZIP_PAGE and
					zip.data == NULL means an active
					buf_pool.watch */

	ulint           write_size;	/* Write size is set when this
					page is first time written and then
					if written again we check is TRIM
					operation needed. */

	ulint           real_size;	/*!< Real size of the page
					Normal pages == srv_page_size
					page compressed pages, payload
					size alligned to sector boundary.
					*/

	buf_tmp_buffer_t* slot;		/*!< Slot for temporary memory
					used for encryption/compression
					or NULL */
#ifdef UNIV_DEBUG
	/** whether the page is in buf_pool.page_hash;
	protected by buf_pool.mutex(!) and the hash bucket rw-latch */
	ibool		in_page_hash;
	ibool		in_zip_hash;	/*!< TRUE if in buf_pool.zip_hash */
#endif /* UNIV_DEBUG */

	/** @name Page flushing fields
	All these are protected by buf_pool.mutex. */
	/* @{ */

	UT_LIST_NODE_T(buf_page_t) list;
					/*!< based on state, this is a
					list node, protected either by
					buf_pool.mutex or by
					buf_pool.flush_list_mutex,
					in one of the following lists in
					buf_pool:

					- BUF_BLOCK_NOT_USED:	free, withdraw
					- BUF_BLOCK_FILE_PAGE:	flush_list
					- BUF_BLOCK_ZIP_DIRTY:	flush_list
					- BUF_BLOCK_ZIP_PAGE:	zip_clean

					If bpage is part of flush_list
					then the node pointers are
					covered by buf_pool.flush_list_mutex.
					Otherwise these pointers are
					protected by buf_pool.mutex.

					The contents of the list node
					is undefined if !in_flush_list
					&& state == BUF_BLOCK_FILE_PAGE,
					or if state is one of
					BUF_BLOCK_MEMORY,
					BUF_BLOCK_REMOVE_HASH or
					BUF_BLOCK_READY_IN_USE. */

#ifdef UNIV_DEBUG
	ibool		in_flush_list;	/*!< TRUE if in buf_pool.flush_list;
					when buf_pool.flush_list_mutex is
					free, the following should hold:
					in_flush_list
					== (state == BUF_BLOCK_FILE_PAGE
					    || state == BUF_BLOCK_ZIP_DIRTY)
					Writes to this field must be
					covered by both block->mutex
					and buf_pool.flush_list_mutex. Hence
					reads can happen while holding
					any one of the two mutexes */
	ibool		in_free_list;	/*!< TRUE if in buf_pool.free; when
					buf_pool.mutex is free, the following
					should hold: in_free_list
					== (state == BUF_BLOCK_NOT_USED) */
#endif /* UNIV_DEBUG */

	lsn_t		oldest_modification;
					/*!< log sequence number of
					the START of the log entry
					written of the oldest
					modification to this block
					which has not yet been flushed
					on disk; zero if all
					modifications are on disk.
					Writes to this field must be
					covered by both block->mutex
					and buf_pool.flush_list_mutex. Hence
					reads can happen while holding
					any one of the two mutexes */
	/* @} */
	/** @name LRU replacement algorithm fields
	These fields are protected by buf_pool.mutex only (not
	buf_pool.zip_mutex or buf_block_t::mutex). */
	/* @{ */

	UT_LIST_NODE_T(buf_page_t) LRU;
					/*!< node of the LRU list */
#ifdef UNIV_DEBUG
	ibool		in_LRU_list;	/*!< TRUE if the page is in
					the LRU list; used in
					debugging */
#endif /* UNIV_DEBUG */
	unsigned	old:1;		/*!< TRUE if the block is in the old
					blocks in buf_pool.LRU_old */
	unsigned	freed_page_clock:31;/*!< the value of
					buf_pool.freed_page_clock
					when this block was the last
					time put to the head of the
					LRU list; a thread is allowed
					to read this for heuristic
					purposes without holding any
					mutex or latch */
	/* @} */
	unsigned	access_time;	/*!< time of first access, or
					0 if the block was never accessed
					in the buffer pool. Protected by
					block mutex for buf_page_in_file()
					blocks.

					For state==BUF_BLOCK_MEMORY
					blocks, this field can be repurposed
					for something else.

					When this field counts log records
					and bytes allocated for recv_sys.pages,
					the field is protected by
					recv_sys_t::mutex. */
  /** Change buffer entries for the page exist.
  Protected by io_fix==BUF_IO_READ or by buf_block_t::lock. */
  bool ibuf_exist;

  /** Block initialization status. Can be modified while holding io_fix
  or buf_block_t::lock X-latch */
  enum {
    /** the page was read normally and should be flushed normally */
    NORMAL = 0,
    /** the page was (re)initialized, and the doublewrite buffer can be
    skipped on the next flush */
    INIT_ON_FLUSH,
    /** the page was freed and need to be flushed.
    For page_compressed, page flush will punch a hole to free space.
    Else if innodb_immediate_scrub_data_uncompressed, the page will
    be overwritten with zeroes. */
    FREED
  } status;

  void fix() { buf_fix_count++; }
  uint32_t unfix()
  {
    uint32_t count= buf_fix_count--;
    ut_ad(count != 0);
    return count - 1;
  }

  /** @return the physical size, in bytes */
  ulint physical_size() const
  {
    return zip.ssize ? (UNIV_ZIP_SIZE_MIN >> 1) << zip.ssize : srv_page_size;
  }

  /** @return the ROW_FORMAT=COMPRESSED physical size, in bytes
  @retval 0 if not compressed */
  ulint zip_size() const
  {
    return zip.ssize ? (UNIV_ZIP_SIZE_MIN >> 1) << zip.ssize : 0;
  }
};

/** The buffer control block structure */

struct buf_block_t{

	/** @name General fields */
	/* @{ */

	buf_page_t	page;		/*!< page information; this must
					be the first field, so that
					buf_pool.page_hash can point
					to buf_page_t or buf_block_t */
	byte*		frame;		/*!< pointer to buffer frame which
					is of size srv_page_size, and
					aligned to an address divisible by
					srv_page_size */
	BPageLock	lock;		/*!< read-write lock of the buffer
					frame */
	UT_LIST_NODE_T(buf_block_t) unzip_LRU;
					/*!< node of the decompressed LRU list;
					a block is in the unzip_LRU list
					if page.state == BUF_BLOCK_FILE_PAGE
					and page.zip.data != NULL */
#ifdef UNIV_DEBUG
	ibool		in_unzip_LRU_list;/*!< TRUE if the page is in the
					decompressed LRU list;
					used in debugging */
	ibool		in_withdraw_list;
#endif /* UNIV_DEBUG */
	uint32_t	lock_hash_val;	/*!< hashed value of the page address
					in the record lock hash table;
					protected by buf_block_t::lock
					(or buf_block_t::mutex, buf_pool.mutex
				        in buf_page_get_gen(),
					buf_page_init_for_read()
					and buf_page_create()) */
	/* @} */
	/** @name Optimistic search field */
	/* @{ */

	ib_uint64_t	modify_clock;	/*!< this clock is incremented every
					time a pointer to a record on the
					page may become obsolete; this is
					used in the optimistic cursor
					positioning: if the modify clock has
					not changed, we know that the pointer
					is still valid; this field may be
					changed if the thread (1) owns the
					pool mutex and the page is not
					bufferfixed, or (2) the thread has an
					x-latch on the block */
	/* @} */
#ifdef BTR_CUR_HASH_ADAPT
	/** @name Hash search fields (unprotected)
	NOTE that these fields are NOT protected by any semaphore! */
	/* @{ */

	volatile uint16_t n_bytes;	/*!< recommended prefix length for hash
					search: number of bytes in
					an incomplete last field */
	volatile uint16_t n_fields;	/*!< recommended prefix length for hash
					search: number of full fields */
	uint16_t	n_hash_helps;	/*!< counter which controls building
					of a new hash index for the page */
	volatile bool	left_side;	/*!< true or false, depending on
					whether the leftmost record of several
					records with the same prefix should be
					indexed in the hash index */
	/* @} */

	/** @name Hash search fields
	These 5 fields may only be modified when:
	we are holding the appropriate x-latch in btr_search_latches[], and
	one of the following holds:
	(1) the block state is BUF_BLOCK_FILE_PAGE, and
	we are holding an s-latch or x-latch on buf_block_t::lock, or
	(2) buf_block_t::buf_fix_count == 0, or
	(3) the block state is BUF_BLOCK_REMOVE_HASH.

	An exception to this is when we init or create a page
	in the buffer pool in buf0buf.cc.

	Another exception for buf_pool_t::clear_hash_index() is that
	assigning block->index = NULL (and block->n_pointers = 0)
	is allowed whenever btr_search_own_all(RW_LOCK_X).

	Another exception is that ha_insert_for_fold_func() may
	decrement n_pointers without holding the appropriate latch
	in btr_search_latches[]. Thus, n_pointers must be
	protected by atomic memory access.

	This implies that the fields may be read without race
	condition whenever any of the following hold:
	- the btr_search_latches[] s-latch or x-latch is being held, or
	- the block state is not BUF_BLOCK_FILE_PAGE or BUF_BLOCK_REMOVE_HASH,
	and holding some latch prevents the state from changing to that.

	Some use of assert_block_ahi_empty() or assert_block_ahi_valid()
	is prone to race conditions while buf_pool_t::clear_hash_index() is
	executing (the adaptive hash index is being disabled). Such use
	is explicitly commented. */

	/* @{ */

# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
	Atomic_counter<ulint>
			n_pointers;	/*!< used in debugging: the number of
					pointers in the adaptive hash index
					pointing to this frame;
					protected by atomic memory access
					or btr_search_own_all(). */
#  define assert_block_ahi_empty(block)					\
	ut_a((block)->n_pointers == 0)
#  define assert_block_ahi_empty_on_init(block) do {			\
	UNIV_MEM_VALID(&(block)->n_pointers, sizeof (block)->n_pointers); \
	assert_block_ahi_empty(block);					\
} while (0)
#  define assert_block_ahi_valid(block)					\
	ut_a((block)->index || (block)->n_pointers == 0)
# else /* UNIV_AHI_DEBUG || UNIV_DEBUG */
#  define assert_block_ahi_empty(block) /* nothing */
#  define assert_block_ahi_empty_on_init(block) /* nothing */
#  define assert_block_ahi_valid(block) /* nothing */
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	unsigned	curr_n_fields:10;/*!< prefix length for hash indexing:
					number of full fields */
	unsigned	curr_n_bytes:15;/*!< number of bytes in hash
					indexing */
	unsigned	curr_left_side:1;/*!< TRUE or FALSE in hash indexing */
	dict_index_t*	index;		/*!< Index for which the
					adaptive hash index has been
					created, or NULL if the page
					does not exist in the
					index. Note that it does not
					guarantee that the index is
					complete, though: there may
					have been hash collisions,
					record deletions, etc. */
	/* @} */
#else /* BTR_CUR_HASH_ADAPT */
# define assert_block_ahi_empty(block) /* nothing */
# define assert_block_ahi_empty_on_init(block) /* nothing */
# define assert_block_ahi_valid(block) /* nothing */
#endif /* BTR_CUR_HASH_ADAPT */
	bool		skip_flush_check;
					/*!< Skip check in buf_dblwr_check_block
					during bulk load, protected by lock.*/
# ifdef UNIV_DEBUG
	/** @name Debug fields */
	/* @{ */
	rw_lock_t*	debug_latch;	/*!< in the debug version, each thread
					which bufferfixes the block acquires
					an s-latch here; so we can use the
					debug utilities in sync0rw */
	/* @} */
# endif
	BPageMutex	mutex;		/*!< mutex protecting this block:
					state (also protected by the buffer
					pool mutex), io_fix, buf_fix_count,
					and accessed; we introduce this new
					mutex in InnoDB-5.1 to relieve
					contention on the buffer pool mutex */

  void fix() { page.fix(); }
  uint32_t unfix() { return page.unfix(); }

  /** @return the physical size, in bytes */
  ulint physical_size() const { return page.physical_size(); }

  /** @return the ROW_FORMAT=COMPRESSED physical size, in bytes
  @retval 0 if not compressed */
  ulint zip_size() const { return page.zip_size(); }
};

/** Check if a buf_block_t object is in a valid state
@param block buffer block
@return TRUE if valid */
#define buf_block_state_valid(block)				\
(buf_block_get_state(block) >= BUF_BLOCK_NOT_USED		\
 && (buf_block_get_state(block) <= BUF_BLOCK_REMOVE_HASH))


/**********************************************************************//**
Compute the hash fold value for blocks in buf_pool.zip_hash. */
/* @{ */
#define BUF_POOL_ZIP_FOLD_PTR(ptr) (ulint(ptr) >> srv_page_size_shift)
#define BUF_POOL_ZIP_FOLD(b) BUF_POOL_ZIP_FOLD_PTR((b)->frame)
#define BUF_POOL_ZIP_FOLD_BPAGE(b) BUF_POOL_ZIP_FOLD((buf_block_t*) (b))
/* @} */

/** A "Hazard Pointer" class used to iterate over page lists
inside the buffer pool. A hazard pointer is a buf_page_t pointer
which we intend to iterate over next and we want it remain valid
even after we release the buffer pool mutex. */
class HazardPointer
{
public:
  virtual ~HazardPointer() {}

  /** @return current value */
  buf_page_t *get() const { ut_ad(mutex_own(m_mutex)); return m_hp; }

  /** Set current value
  @param bpage	buffer block to be set as hp */
  void set(buf_page_t *bpage)
  {
    ut_ad(mutex_own(m_mutex));
    ut_ad(!bpage || buf_page_in_file(bpage));
    m_hp= bpage;
  }

  /** Checks if a bpage is the hp
  @param bpage  buffer block to be compared
  @return true if it is hp */
  bool is_hp(const buf_page_t *bpage) const
  { ut_ad(mutex_own(m_mutex)); return bpage == m_hp; }

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list. */
  virtual void adjust(const buf_page_t*) = 0;

#ifdef UNIV_DEBUG
  /** mutex that protects access to the m_hp. */
  const ib_mutex_t *m_mutex= nullptr;
#endif /* UNIV_DEBUG */

protected:
  /** hazard pointer */
  buf_page_t *m_hp= nullptr;
};

/** Class implementing buf_pool.flush_list hazard pointer */
class FlushHp : public HazardPointer
{
public:
  ~FlushHp() override {}

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list.
  @param bpage  buffer block to be compared */
  void adjust(const buf_page_t *bpage) override
  {
    ut_ad(bpage != NULL);

    /* We only support reverse traversal for now. */
    if (is_hp(bpage))
      m_hp= UT_LIST_GET_PREV(list, m_hp);

    ut_ad(!m_hp || m_hp->in_flush_list);
  }
};

/** Class implementing buf_pool.LRU hazard pointer */
class LRUHp : public HazardPointer {
public:
  ~LRUHp() override {}

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list.
  @param bpage  buffer block to be compared */
  void adjust(const buf_page_t *bpage) override
  {
    ut_ad(bpage);
    /** We only support reverse traversal for now. */
    if (is_hp(bpage))
      m_hp= UT_LIST_GET_PREV(LRU, m_hp);

    ut_ad(!m_hp || m_hp->in_LRU_list);
  }
};

/** Special purpose iterators to be used when scanning the LRU list.
The idea is that when one thread finishes the scan it leaves the
itr in that position and the other thread can start scan from
there */
class LRUItr : public LRUHp {
public:
  LRUItr() : LRUHp() {}
  ~LRUItr() override {}

  /** Select from where to start a scan. If we have scanned
  too deep into the LRU list it resets the value to the tail
  of the LRU list.
  @return buf_page_t from where to start scan. */
  inline buf_page_t *start();
};

/** Struct that is embedded in the free zip blocks */
struct buf_buddy_free_t {
	union {
		ulint	size;	/*!< size of the block */
		byte	bytes[FIL_PAGE_DATA];
				/*!< stamp[FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID]
				== BUF_BUDDY_FREE_STAMP denotes a free
				block. If the space_id field of buddy
				block != BUF_BUDDY_FREE_STAMP, the block
				is not in any zip_free list. If the
				space_id is BUF_BUDDY_FREE_STAMP then
				stamp[0] will contain the
				buddy block size. */
	} stamp;

	buf_page_t	bpage;	/*!< Embedded bpage descriptor */
	UT_LIST_NODE_T(buf_buddy_free_t) list;
				/*!< Node of zip_free list */
};

/** @brief The buffer pool statistics structure. */
struct buf_pool_stat_t{
	ulint	n_page_gets;	/*!< number of page gets performed;
				also successful searches through
				the adaptive hash index are
				counted as page gets; this field
				is NOT protected by the buffer
				pool mutex */
	ulint	n_pages_read;	/*!< number read operations */
	ulint	n_pages_written;/*!< number write operations */
	ulint	n_pages_created;/*!< number of pages created
				in the pool with no read */
	ulint	n_ra_pages_read_rnd;/*!< number of pages read in
				as part of random read ahead */
	ulint	n_ra_pages_read;/*!< number of pages read in
				as part of read ahead */
	ulint	n_ra_pages_evicted;/*!< number of read ahead
				pages that are evicted without
				being accessed */
	ulint	n_pages_made_young; /*!< number of pages made young, in
				calls to buf_LRU_make_block_young() */
	ulint	n_pages_not_made_young; /*!< number of pages not made
				young because the first access
				was not long enough ago, in
				buf_page_peek_if_too_old() */
	ulint	LRU_bytes;	/*!< LRU size in bytes */
	ulint	flush_list_bytes;/*!< flush_list size in bytes */
};

/** Statistics of buddy blocks of a given size. */
struct buf_buddy_stat_t {
	/** Number of blocks allocated from the buddy system. */
	ulint		used;
	/** Number of blocks relocated by the buddy system. */
	ib_uint64_t	relocated;
	/** Total duration of block relocations, in microseconds. */
	ib_uint64_t	relocated_usec;
};

/** The buffer pool */
class buf_pool_t
{
  /** A chunk of buffers */
  struct chunk_t
  {
    /** number of elements in blocks[] */
    size_t size;
    /** memory allocated for the page frames */
    unsigned char *mem;
    /** descriptor of mem */
    ut_new_pfx_t mem_pfx;
    /** array of buffer control blocks */
    buf_block_t *blocks;

    /** Map of first page frame address to chunks[] */
    using map= std::map<const void*, chunk_t*, std::less<const void*>,
                        ut_allocator<std::pair<const void* const,chunk_t*>>>;
    /** Chunk map that may be under construction by buf_resize_thread() */
    static map *map_reg;
    /** Current chunk map for lookup only */
    static map *map_ref;

    /** @return the memory size bytes. */
    size_t mem_size() const { return mem_pfx.m_size; }

    /** Register the chunk */
    void reg() { map_reg->emplace(map::value_type(blocks->frame, this)); }

    /** Allocate a chunk of buffer frames.
    @param bytes    requested size
    @return whether the allocation succeeded */
    inline bool create(size_t bytes);

#ifdef UNIV_DEBUG
    /** Find a block that points to a ROW_FORMAT=COMPRESSED page
    @param data  pointer to the start of a ROW_FORMAT=COMPRESSED page frame
    @return the block
    @retval nullptr  if not found */
    const buf_block_t *contains_zip(const void *data) const
    {
      const buf_block_t *block= blocks;
      for (auto i= size; i--; block++)
        if (block->page.zip.data == data)
          return block;
      return nullptr;
    }

    /** Check that all blocks are in a replaceable state.
    @return address of a non-free block
    @retval nullptr if all freed */
    inline const buf_block_t *not_freed() const;
#endif /* UNIV_DEBUG */
  };

  /** Withdraw blocks from the buffer pool until meeting withdraw_target.
  @return whether retry is needed */
  inline bool withdraw_blocks();

  /** Determine if a pointer belongs to a buf_block_t. It can be a pointer to
  the buf_block_t itself or a member of it.
  @param ptr    a pointer that will not be dereferenced
  @return whether the ptr belongs to a buf_block_t struct */
  bool is_block_field(const void *ptr) const
  {
    const chunk_t *chunk= chunks;
    const chunk_t *const echunk= chunk + ut_min(n_chunks, n_chunks_new);

    /* TODO: protect chunks with a mutex (the older pointer will
    currently remain during resize()) */
    for (; chunk < echunk; chunk++)
      if (ptr >= reinterpret_cast<const void*>(chunk->blocks) &&
          ptr < reinterpret_cast<const void*>(chunk->blocks + chunk->size))
        return true;
    return false;
  }

  /** Try to reallocate a control block.
  @param block  control block to reallocate
  @return whether the reallocation succeeded */
  inline bool realloc(buf_block_t *block);

public:
  bool is_initialised() const { return chunks != nullptr; }

  /** Create the buffer pool.
  @return whether the creation failed */
  bool create();

  /** Clean up after successful create() */
  void close();

  /** Resize from srv_buf_pool_old_size to srv_buf_pool_size. */
  inline void resize();

  /** @return whether resize() is in progress */
  bool resize_in_progress() const
  {
    return UNIV_UNLIKELY(resizing.load(std::memory_order_relaxed) ||
                         withdrawing.load(std::memory_order_relaxed));
  }

  /** @return the withdraw_clock */
  ulint withdraw_clock() const
  { return withdraw_clock_.load(std::memory_order_relaxed); }

  /** Verify the possibility that a stored page is not in buffer pool.
  @param withdraw_clock  the withdraw clock of the page
  @return whether the page might be relocated */
  bool is_obsolete(ulint withdraw_clock) const
  {
    return UNIV_UNLIKELY(withdrawing.load(std::memory_order_relaxed) ||
                         this->withdraw_clock() != withdraw_clock);
  }

  /** @return the current size in blocks */
  size_t get_n_pages() const
  {
    ut_ad(is_initialised());
    size_t size= 0;
    for (auto j= n_chunks; j--; )
      size+= chunks[j].size;
    return size;
  }

  /** Determine whether a frame is intended to be withdrawn during resize().
  @param ptr    pointer within a buf_block_t::frame
  @return whether the frame will be withdrawn */
  bool will_be_withdrawn(const byte *ptr) const
  {
    ut_ad(curr_size < old_size);
    ut_ad(!resizing.load(std::memory_order_relaxed) || mutex_own(&mutex));

    for (const chunk_t *chunk= chunks + n_chunks_new,
         * const echunk= chunks + n_chunks;
         chunk != echunk; chunk++)
      if (ptr >= chunk->blocks->frame &&
          ptr < (chunk->blocks + chunk->size - 1)->frame + srv_page_size)
        return true;
    return false;
  }

  /** Determine whether a block is intended to be withdrawn during resize().
  @param bpage  buffer pool block
  @return whether the frame will be withdrawn */
  bool will_be_withdrawn(const buf_page_t &bpage) const
  {
    ut_ad(curr_size < old_size);
    ut_ad(!resizing.load(std::memory_order_relaxed) || mutex_own(&mutex));

    for (const chunk_t *chunk= chunks + n_chunks_new,
         * const echunk= chunks + n_chunks;
         chunk != echunk; chunk++)
      if (&bpage >= &chunk->blocks->page &&
          &bpage < &chunk->blocks[chunk->size].page)
        return true;
    return false;
  }

#ifdef UNIV_DEBUG
  /** Find a block that points to a ROW_FORMAT=COMPRESSED page
  @param data  pointer to the start of a ROW_FORMAT=COMPRESSED page frame
  @return the block
  @retval nullptr  if not found */
  const buf_block_t *contains_zip(const void *data) const
  {
    ut_ad(mutex_own(&mutex));
    for (const chunk_t *chunk= chunks, * const end= chunks + n_chunks;
         chunk != end; chunk++)
      if (const buf_block_t *block= chunk->contains_zip(data))
        return block;
    return nullptr;
  }

  /** Assert that all buffer pool pages are in a replaceable state */
  void assert_all_freed();
#endif /* UNIV_DEBUG */

#ifdef BTR_CUR_HASH_ADAPT
  /** Clear the adaptive hash index on all pages in the buffer pool. */
  inline void clear_hash_index();

  /** Get a buffer block from an adaptive hash index pointer.
  This function does not return if the block is not identified.
  @param ptr  pointer to within a page frame
  @return pointer to block, never NULL */
  inline buf_block_t* block_from_ahi(const byte *ptr) const;
#endif /* BTR_CUR_HASH_ADAPT */

  bool is_block_mutex(const BPageMutex *m) const
  { return is_block_field(reinterpret_cast<const void*>(m)); }
  bool is_block_lock(const BPageLock *l) const
  { return is_block_field(reinterpret_cast<const void*>(l)); }

  /** Determine if a buffer block was created by chunk_t::create().
  @param block  block descriptor (not dereferenced)
  @return whether block has been created by chunk_t::create() */
  bool is_uncompressed(const buf_block_t *block) const
  {
    /* The pointer should be aligned. */
    return !(size_t(block) % sizeof *block) &&
      is_block_field(reinterpret_cast<const void*>(block));
  }

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  /** Validate the buffer pool. */
  void validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
  /** Write information of the buf_pool to the error log. */
  void print();
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */

	/** @name General fields */
	/* @{ */
	BufPoolMutex	mutex;		/*!< Buffer pool mutex */
	BufPoolZipMutex	zip_mutex;	/*!< Zip mutex, protects compressed
					only pages (of type buf_page_t, not
					buf_block_t */
	ulint		curr_pool_size;	/*!< Current pool size in bytes */
	ulint		LRU_old_ratio;  /*!< Reserve this much of the buffer
					pool for "old" blocks */
#ifdef UNIV_DEBUG
	ulint		buddy_n_frames; /*!< Number of frames allocated from
					the buffer pool to the buddy system */
#endif
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ulint		mutex_exit_forbidden; /*!< Forbid release mutex */
#endif
	ut_allocator<unsigned char>	allocator;	/*!< Allocator used for
					allocating memory for the the "chunks"
					member. */
	volatile ulint	n_chunks;	/*!< number of buffer pool chunks */
	volatile ulint	n_chunks_new;	/*!< new number of buffer pool chunks */
	chunk_t*	chunks;		/*!< buffer pool chunks */
	chunk_t*	chunks_old;	/*!< old buffer pool chunks to be freed
					after resizing buffer pool */
	ulint		curr_size;	/*!< current pool size in pages */
	ulint		old_size;	/*!< previous pool size in pages */
	ulint		read_ahead_area;/*!< size in pages of the area which
					the read-ahead algorithms read if
					invoked */
	hash_table_t*	page_hash;	/*!< hash table of buf_page_t or
					buf_block_t file pages,
					buf_page_in_file() == TRUE,
					indexed by (space_id, offset).
					page_hash is protected by an
					array of mutexes.
					Changes in page_hash are protected
					by buf_pool.mutex and the relevant
					page_hash mutex. Lookups can happen
					while holding the buf_pool.mutex or
					the relevant page_hash mutex. */
	hash_table_t*	page_hash_old;	/*!< old pointer to page_hash to be
					freed after resizing buffer pool */
	hash_table_t*	zip_hash;	/*!< hash table of buf_block_t blocks
					whose frames are allocated to the
					zip buddy system,
					indexed by block->frame */
	ulint		n_pend_reads;	/*!< number of pending read
					operations */
	Atomic_counter<ulint>
			n_pend_unzip;	/*!< number of pending decompressions */

	time_t		last_printout_time;
					/*!< when buf_print_io was last time
					called */
	buf_buddy_stat_t buddy_stat[BUF_BUDDY_SIZES_MAX + 1];
					/*!< Statistics of buddy system,
					indexed by block size */
	buf_pool_stat_t	stat;		/*!< current statistics */
	buf_pool_stat_t	old_stat;	/*!< old statistics */

	/* @} */

	/** @name Page flushing algorithm fields */

	/* @{ */

	FlushListMutex	flush_list_mutex;/*!< mutex protecting the
					flush list access. This mutex
					protects flush_list, flush_rbt
					and bpage::list pointers when
					the bpage is on flush_list. It
					also protects writes to
					bpage::oldest_modification and
					flush_list_hp */
	FlushHp			flush_hp;/*!< "hazard pointer"
					used during scan of flush_list
					while doing flush list batch.
					Protected by flush_list_mutex */
	UT_LIST_BASE_NODE_T(buf_page_t) flush_list;
					/*!< base node of the modified block
					list */
	ibool		init_flush[BUF_FLUSH_N_TYPES];
					/*!< this is TRUE when a flush of the
					given type is being initialized */
	ulint		n_flush[BUF_FLUSH_N_TYPES];
					/*!< this is the number of pending
					writes in the given flush type */
	os_event_t	no_flush[BUF_FLUSH_N_TYPES];
					/*!< this is in the set state
					when there is no flush batch
					of the given type running;
					os_event_set() and os_event_reset()
					are protected by buf_pool_t::mutex */
	ib_rbt_t*	flush_rbt;	/*!< a red-black tree is used
					exclusively during recovery to
					speed up insertions in the
					flush_list. This tree contains
					blocks in order of
					oldest_modification LSN and is
					kept in sync with the
					flush_list.
					Each member of the tree MUST
					also be on the flush_list.
					This tree is relevant only in
					recovery and is set to NULL
					once the recovery is over.
					Protected by flush_list_mutex */
	unsigned	freed_page_clock;/*!< a sequence number used
					to count the number of buffer
					blocks removed from the end of
					the LRU list; NOTE that this
					counter may wrap around at 4
					billion! A thread is allowed
					to read this for heuristic
					purposes without holding any
					mutex or latch */
	ibool		try_LRU_scan;	/*!< Set to FALSE when an LRU
					scan for free block fails. This
					flag is used to avoid repeated
					scans of LRU list when we know
					that there is no free block
					available in the scan depth for
					eviction. Set to TRUE whenever
					we flush a batch from the
					buffer pool. Protected by the
					buf_pool.mutex */
	/* @} */

	/** @name LRU replacement algorithm fields */
	/* @{ */

	UT_LIST_BASE_NODE_T(buf_page_t) free;
					/*!< base node of the free
					block list */

	UT_LIST_BASE_NODE_T(buf_page_t) withdraw;
					/*!< base node of the withdraw
					block list. It is only used during
					shrinking buffer pool size, not to
					reuse the blocks will be removed */

	ulint		withdraw_target;/*!< target length of withdraw
					block list, when withdrawing */

	/** "hazard pointer" used during scan of LRU while doing
	LRU list batch.  Protected by buf_pool_t::mutex. */
	LRUHp		lru_hp;

	/** Iterator used to scan the LRU list when searching for
	replacable victim. Protected by buf_pool_t::mutex. */
	LRUItr		lru_scan_itr;

	/** Iterator used to scan the LRU list when searching for
	single page flushing victim.  Protected by buf_pool_t::mutex. */
	LRUItr		single_scan_itr;

	UT_LIST_BASE_NODE_T(buf_page_t) LRU;
					/*!< base node of the LRU list */

	buf_page_t*	LRU_old;	/*!< pointer to the about
					LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV
					oldest blocks in the LRU list;
					NULL if LRU length less than
					BUF_LRU_OLD_MIN_LEN;
					NOTE: when LRU_old != NULL, its length
					should always equal LRU_old_len */
	ulint		LRU_old_len;	/*!< length of the LRU list from
					the block to which LRU_old points
					onward, including that block;
					see buf0lru.cc for the restrictions
					on this value; 0 if LRU_old == NULL;
					NOTE: LRU_old_len must be adjusted
					whenever LRU_old shrinks or grows! */

	UT_LIST_BASE_NODE_T(buf_block_t) unzip_LRU;
					/*!< base node of the
					unzip_LRU list */

	/* @} */
	/** @name Buddy allocator fields
	The buddy allocator is used for allocating compressed page
	frames and buf_page_t descriptors of blocks that exist
	in the buffer pool only in compressed form. */
	/* @{ */
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	UT_LIST_BASE_NODE_T(buf_page_t)	zip_clean;
					/*!< unmodified compressed pages */
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
	UT_LIST_BASE_NODE_T(buf_buddy_free_t) zip_free[BUF_BUDDY_SIZES_MAX];
					/*!< buddy free lists */
#if BUF_BUDDY_LOW > UNIV_ZIP_SIZE_MIN
# error "BUF_BUDDY_LOW > UNIV_ZIP_SIZE_MIN"
#endif
	/* @} */

	buf_page_t*			watch;
					/*!< Sentinel records for buffer
					pool watches. Protected by
					buf_pool.mutex. */


  /** Reserve a buffer. */
  buf_tmp_buffer_t *io_buf_reserve() { return io_buf.reserve(); }
private:
  /** Temporary memory for page_compressed and encrypted I/O */
  struct io_buf_t
  {
    /** number of elements in slots[] */
    ulint n_slots;
    /** array of slots */
    buf_tmp_buffer_t *slots;

    void create(ulint n_slots)
    {
      this->n_slots= n_slots;
      slots= static_cast<buf_tmp_buffer_t*>
        (ut_malloc_nokey(n_slots * sizeof *slots));
      memset((void*) slots, 0, n_slots * sizeof *slots);
    }

    void close()
    {
      for (buf_tmp_buffer_t *s= slots, *e= slots + n_slots; s != e; s++)
      {
        aligned_free(s->crypt_buf);
        aligned_free(s->comp_buf);
      }
      ut_free(slots);
      slots= nullptr;
      n_slots= 0;
    }

    /** Reserve a buffer */
    buf_tmp_buffer_t *reserve()
    {
      for (buf_tmp_buffer_t *s= slots, *e= slots + n_slots; s != e; s++)
        if (s->acquire())
          return s;
      return nullptr;
    }
  } io_buf;

  /** whether resize() is in the critical path */
  std::atomic<bool> resizing;

  /** whether withdrawing buffer pool pages might cause page relocation */
  std::atomic<bool> withdrawing;

  /** a counter that is incremented every time a pointer to a page may
  become obsolete */
  std::atomic<ulint> withdraw_clock_;
};

/** The InnoDB buffer pool */
extern buf_pool_t buf_pool;

/** @name Accessors for buffer pool mutexes
Use these instead of accessing buffer pool mutexes directly. */
/* @{ */

/** Test if block->mutex is owned. */
#define buf_page_mutex_own(b)	(b)->mutex.is_owned()

/** Acquire the block->mutex. */
#define buf_page_mutex_enter(b) do {			\
	mutex_enter(&(b)->mutex);			\
} while (0)

/** Release the trx->mutex. */
#define buf_page_mutex_exit(b) do {			\
	(b)->mutex.exit();				\
} while (0)


/** Get appropriate page_hash_lock. */
UNIV_INLINE
rw_lock_t*
buf_page_hash_lock_get(const page_id_t& page_id)
{
	return hash_get_lock(buf_pool.page_hash, page_id.fold());
}

/** If not appropriate page_hash_lock, relock until appropriate. */
# define buf_page_hash_lock_s_confirm(hash_lock, page_id)\
	hash_lock_s_confirm(hash_lock, buf_pool.page_hash, (page_id).fold())

# define buf_page_hash_lock_x_confirm(hash_lock, page_id)\
	hash_lock_x_confirm(hash_lock, buf_pool.page_hash, (page_id).fold())

#ifdef UNIV_DEBUG
/** Test if page_hash lock is held in s-mode. */
# define buf_page_hash_lock_held_s(bpage)	\
	rw_lock_own(buf_page_hash_lock_get((bpage)->id), RW_LOCK_S)

/** Test if page_hash lock is held in x-mode. */
# define buf_page_hash_lock_held_x(bpage)	\
	rw_lock_own(buf_page_hash_lock_get((bpage)->id), RW_LOCK_X)

/** Test if page_hash lock is held in x or s-mode. */
# define buf_page_hash_lock_held_s_or_x(bpage)\
	(buf_page_hash_lock_held_s(bpage)	\
	 || buf_page_hash_lock_held_x(bpage))

# define buf_block_hash_lock_held_s(block)	\
	buf_page_hash_lock_held_s(&(block)->page)

# define buf_block_hash_lock_held_x(block)	\
	buf_page_hash_lock_held_x(&(block)->page)

# define buf_block_hash_lock_held_s_or_x(block)	\
	buf_page_hash_lock_held_s_or_x(&(block)->page)
#else /* UNIV_DEBUG */
# define buf_page_hash_lock_held_s(p)	(TRUE)
# define buf_page_hash_lock_held_x(p)	(TRUE)
# define buf_page_hash_lock_held_s_or_x(p)	(TRUE)
# define buf_block_hash_lock_held_s(p)	(TRUE)
# define buf_block_hash_lock_held_x(p)	(TRUE)
# define buf_block_hash_lock_held_s_or_x(p)	(TRUE)
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Forbid the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_forbid() do {	\
	ut_ad(mutex_own(&buf_pool.mutex));	\
	buf_pool.mutex_exit_forbidden++;	\
} while (0)
/** Allow the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_allow() do {		\
	ut_ad(mutex_own(&buf_pool.mutex));		\
	ut_ad(buf_pool.mutex_exit_forbidden--);	\
} while (0)
#else
/** Forbid the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_forbid() ((void) 0)
/** Allow the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_allow() ((void) 0)
#endif
/* @} */

/**********************************************************************
Let us list the consistency conditions for different control block states.

NOT_USED:	is in free list, not in LRU list, not in flush list, nor
		page hash table
READY_FOR_USE:	is not in free list, LRU list, or flush list, nor page
		hash table
MEMORY:		is not in free list, LRU list, or flush list, nor page
		hash table
FILE_PAGE:	space and offset are defined, is in page hash table
		if io_fix == BUF_IO_WRITE,
			pool: no_flush[flush_type] is in reset state,
			pool: n_flush[flush_type] > 0

		(1) if buf_fix_count == 0, then
			is in LRU list, not in free list
			is in flush list,
				if and only if oldest_modification > 0
			is x-locked,
				if and only if io_fix == BUF_IO_READ
			is s-locked,
				if and only if io_fix == BUF_IO_WRITE

		(2) if buf_fix_count > 0, then
			is not in LRU list, not in free list
			is in flush list,
				if and only if oldest_modification > 0
			if io_fix == BUF_IO_READ,
				is x-locked
			if io_fix == BUF_IO_WRITE,
				is s-locked

State transitions:

NOT_USED => READY_FOR_USE
READY_FOR_USE => MEMORY
READY_FOR_USE => FILE_PAGE
MEMORY => NOT_USED
FILE_PAGE => NOT_USED	NOTE: This transition is allowed if and only if
				(1) buf_fix_count == 0,
				(2) oldest_modification == 0, and
				(3) io_fix == 0.
*/

/** Select from where to start a scan. If we have scanned
too deep into the LRU list it resets the value to the tail
of the LRU list.
@return buf_page_t from where to start scan. */
inline buf_page_t *LRUItr::start()
{
  ut_ad(mutex_own(m_mutex));

  if (!m_hp || m_hp->old)
    m_hp= UT_LIST_GET_LAST(buf_pool.LRU);

  return m_hp;
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Functor to validate the LRU list. */
struct	CheckInLRUList {
	void	operator()(const buf_page_t* elem) const
	{
		ut_a(elem->in_LRU_list);
	}

	static void validate()
	{
		ut_list_validate(buf_pool.LRU, CheckInLRUList());
	}
};

/** Functor to validate the LRU list. */
struct	CheckInFreeList {
	void	operator()(const buf_page_t* elem) const
	{
		ut_a(elem->in_free_list);
	}

	static void validate()
	{
		ut_list_validate(buf_pool.free, CheckInFreeList());
	}
};

struct	CheckUnzipLRUAndLRUList {
	void	operator()(const buf_block_t* elem) const
	{
                ut_a(elem->page.in_LRU_list);
                ut_a(elem->in_unzip_LRU_list);
	}

	static void validate()
	{
		ut_list_validate(buf_pool.unzip_LRU,
				 CheckUnzipLRUAndLRUList());
	}
};
#endif /* UNIV_DEBUG || defined UNIV_BUF_DEBUG */

#include "buf0buf.ic"

#endif /* !UNIV_INNOCHECKSUM */

#endif
