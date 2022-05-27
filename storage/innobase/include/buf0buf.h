/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.

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
#include "span.h"
#include "assume_aligned.h"
#include "buf0types.h"
#ifndef UNIV_INNOCHECKSUM
#include "ut0byte.h"
#include "page0types.h"
#include "log0log.h"
#include "srv0srv.h"
#include "transactional_lock_guard.h"
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

/** If LRU list of a buf_pool is less than this size then LRU eviction
should not happen. This is because when we do LRU flushing we also put
the blocks on free list. If LRU list is very small then we can end up
in thrashing. */
#define BUF_LRU_MIN_LEN		256

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
# define buf_pool_get_curr_size() srv_buf_pool_curr_size

/** Allocate a buffer block.
@return own: the allocated block, state()==MEMORY */
inline buf_block_t *buf_block_alloc();
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
	buf_page_get_gen(ID, SIZE, LA, NULL, BUF_GET, MTR)

/**************************************************************//**
Use these macros to bufferfix a page with no latching. Remember not to
read the contents of the page unless you know it is safe. Do not modify
the contents of the page! We have separated this case, because it is
error-prone programming not to set a latch, and it should be used
with care. */
#define buf_page_get_with_no_latch(ID, SIZE, MTR)	\
	buf_page_get_gen(ID, SIZE, RW_NO_LATCH, NULL, BUF_GET_NO_LATCH, MTR)
/** Try to acquire a page latch.
@param rw_latch      RW_S_LATCH or RW_X_LATCH
@param block         guessed block
@param modify_clock  expected value of block->modify_clock
@param mtr           mini-transaction
@return whether the latch was acquired (the page is an allocated file page) */
bool buf_page_optimistic_get(ulint rw_latch, buf_block_t *block,
                             uint64_t modify_clock, mtr_t *mtr);

/** Try to S-latch a page.
Suitable for using when holding the lock_sys latches (as it avoids deadlock).
@param[in]	page_id	page identifier
@param[in,out]	mtr	mini-transaction
@return the block
@retval nullptr if an S-latch cannot be granted immediately */
buf_block_t *buf_page_try_get(const page_id_t page_id, mtr_t *mtr);

/** Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with unfix().
NOTE: the page is not protected by any latch.  Mutual exclusion has to
be implemented at a higher level.  In other words, all possible
accesses to a given page through this function must be protected by
the same set of mutexes or latches.
@param page_id   page identifier
@param zip_size  ROW_FORMAT=COMPRESSED page size in bytes
@return pointer to the block, s-latched */
buf_page_t *buf_page_get_zip(const page_id_t page_id, ulint zip_size);

/** Get access to a database page. Buffered redo log may be applied.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		RW_S_LATCH, RW_X_LATCH, RW_NO_LATCH
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
BUF_PEEK_IF_IN_POOL, BUF_GET_NO_LATCH, or BUF_GET_IF_IN_POOL_OR_WATCH
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
	mtr_t*			mtr,
	dberr_t*		err,
	bool			allow_ibuf_merge);

/** Initialize a page in the buffer pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED => LRU
(the other is buf_page_get_low()).
@param[in,out]	space		space object
@param[in]	offset		offset of the tablespace
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@param[in,out]	free_block	pre-allocated buffer block
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create(fil_space_t *space, uint32_t offset,
                ulint zip_size, mtr_t *mtr, buf_block_t *free_block);

/** Initialize a page in buffer pool while initializing the
deferred tablespace
@param space_id         space identfier
@param zip_size         ROW_FORMAT=COMPRESSED page size or 0
@param mtr              mini-transaction
@param free_block       pre-allocated buffer block
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create_deferred(uint32_t space_id, ulint zip_size, mtr_t *mtr,
                         buf_block_t *free_block);

/** Move a block to the start of the LRU list. */
void buf_page_make_young(buf_page_t *bpage);
/** Mark the page status as FREED for the given tablespace and page number.
@param[in,out]	space	tablespace
@param[in]	page	page number
@param[in,out]	mtr	mini-transaction */
void buf_page_free(fil_space_t *space, uint32_t page, mtr_t *mtr);

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
#endif /* !UNIV_INNOCHECKSUM */

/** Check if a buffer is all zeroes.
@param[in]	buf	data to check
@return whether the buffer is all zeroes */
bool buf_is_zeroes(st_::span<const byte> buf);

/** Check if a page is corrupt.
@param check_lsn   whether FIL_PAGE_LSN should be checked
@param read_buf    database page
@param fsp_flags   contents of FIL_SPACE_FLAGS
@return whether the page is corrupted */
bool buf_page_is_corrupted(bool check_lsn, const byte *read_buf,
                           uint32_t fsp_flags)
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
inline uint32_t buf_page_get_key_version(const byte* read_buf,
                                         uint32_t fsp_flags)
{
  static_assert(FIL_PAGE_FCRC32_KEY_VERSION == 0, "compatibility");
  return fil_space_t::full_crc32(fsp_flags)
    ? mach_read_from_4(my_assume_aligned<4>(read_buf))
    : mach_read_from_4(my_assume_aligned<2>
		       (read_buf + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION));
}

/** Read the compression info from the page. In full crc32 format,
compression info is at MSB of page type. In other format, it is
stored in page type.
@param[in]	read_buf	database page
@param[in]	fsp_flags	tablespace flags
@return true if page is compressed. */
inline bool buf_page_is_compressed(const byte* read_buf, uint32_t fsp_flags)
{
  uint16_t page_type= fil_page_get_type(read_buf);
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
	uint t = fil_page_get_type(buf);
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

/** Invalidate all pages in the buffer pool.
All pages must be in a replaceable state (not modified or latched). */
void buf_pool_invalidate();

/*========================================================================
--------------------------- LOWER LEVEL ROUTINES -------------------------
=========================================================================*/

#define buf_block_get_frame(block) (block)->page.frame

/*********************************************************************//**
Gets the compressed page descriptor corresponding to an uncompressed page
if applicable. */
#define buf_block_get_page_zip(block) \
	(UNIV_LIKELY_NULL((block)->page.zip.data) ? &(block)->page.zip : NULL)
#define is_buf_block_get_page_zip(block) \
        UNIV_LIKELY_NULL((block)->page.zip.data)

/** Monitor the buffer page read/write activity, and increment corresponding
counter value in MONITOR_MODULE_BUF_PAGE.
@param bpage   buffer page whose read or write was completed
@param read    true=read, false=write */
ATTRIBUTE_COLD void buf_page_monitor(const buf_page_t &bpage, bool read);

/** Calculate aligned buffer pool size based on srv_buf_pool_chunk_unit,
if needed.
@param[in]	size	size in bytes
@return	aligned size */
ulint
buf_pool_size_align(
	ulint	size);

/** Verify that post encryption checksum match with the calculated checksum.
This function should be called only if tablespace contains crypt data metadata.
@param page       page frame
@param fsp_flags  contents of FSP_SPACE_FLAGS
@return whether the page is encrypted and valid */
bool buf_page_verify_crypt_checksum(const byte *page, uint32_t fsp_flags);

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

class buf_pool_t;

class buf_page_t
{
  friend buf_pool_t;
  friend buf_block_t;

  /** @name General fields */
  /* @{ */

public: // FIXME: fix fil_iterate()
  /** Page id. Protected by buf_pool.page_hash.lock_get() when
  the page is in buf_pool.page_hash. */
  page_id_t id_;
  /** buf_pool.page_hash link; protected by buf_pool.page_hash.lock_get() */
  buf_page_t *hash;
private:
  /** log sequence number of the START of the log entry written of the
  oldest modification to this block which has not yet been written
  to the data file;

  0 if no modifications are pending;
  1 if no modifications are pending, but the block is in buf_pool.flush_list;
  2 if modifications are pending, but the block is not in buf_pool.flush_list
  (because id().space() is the temporary tablespace). */
  Atomic_relaxed<lsn_t> oldest_modification_;

public:
  /** state() of unused block (in buf_pool.free list) */
  static constexpr uint32_t NOT_USED= 0;
  /** state() of block allocated as general-purpose memory */
  static constexpr uint32_t MEMORY= 1;
  /** state() of block that is being freed */
  static constexpr uint32_t REMOVE_HASH= 2;
  /** smallest state() of a buffer page that is freed in the tablespace */
  static constexpr uint32_t FREED= 3;
  /** smallest state() for a block that belongs to buf_pool.LRU */
  static constexpr uint32_t UNFIXED= 1U << 29;
  /** smallest state() of a block for which buffered changes may exist */
  static constexpr uint32_t IBUF_EXIST= 2U << 29;
  /** smallest state() of a (re)initialized page (no doublewrite needed) */
  static constexpr uint32_t REINIT= 3U << 29;
  /** smallest state() for an io-fixed block */
  static constexpr uint32_t READ_FIX= 4U << 29;
  /** smallest state() for a write-fixed block */
  static constexpr uint32_t WRITE_FIX= 5U << 29;
  /** smallest state() for a write-fixed block with buffered changes */
  static constexpr uint32_t WRITE_FIX_IBUF= 6U << 29;
  /** smallest state() for a write-fixed block (no doublewrite was used) */
  static constexpr uint32_t WRITE_FIX_REINIT= 7U << 29;
  /** buf_pool.LRU status mask in state() */
  static constexpr uint32_t LRU_MASK= 7U << 29;

  /** lock covering the contents of frame */
  block_lock lock;
  /** pointer to aligned, uncompressed page frame of innodb_page_size */
  byte *frame;
  /* @} */
  /** ROW_FORMAT=COMPRESSED page; zip.data (but not the data it points to)
  is also protected by buf_pool.mutex;
  !frame && !zip.data means an active buf_pool.watch */
  page_zip_des_t zip;
#ifdef UNIV_DEBUG
  /** whether this->list is in buf_pool.zip_hash; protected by buf_pool.mutex */
  bool in_zip_hash;
  /** whether this->LRU is in buf_pool.LRU (in_file());
  protected by buf_pool.mutex */
  bool in_LRU_list;
  /** whether this is in buf_pool.page_hash (in_file());
  protected by buf_pool.mutex */
  bool in_page_hash;
  /** whether this->list is in buf_pool.free (state() == NOT_USED);
  protected by buf_pool.flush_list_mutex */
  bool in_free_list;
#endif /* UNIV_DEBUG */
  /** list member in one of the lists of buf_pool; protected by
  buf_pool.mutex or buf_pool.flush_list_mutex

  state() == NOT_USED: buf_pool.free or buf_pool.withdraw

  in_file() && oldest_modification():
  buf_pool.flush_list (protected by buf_pool.flush_list_mutex)

  The contents is undefined if in_file() && !oldest_modification(),
  or if state() == MEMORY or state() == REMOVE_HASH. */
  UT_LIST_NODE_T(buf_page_t) list;

	/** @name LRU replacement algorithm fields.
	Protected by buf_pool.mutex. */
	/* @{ */

	UT_LIST_NODE_T(buf_page_t) LRU;
					/*!< node of the LRU list */
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
	Atomic_counter<unsigned> access_time;	/*!< time of first access, or
					0 if the block was never accessed
					in the buffer pool.

					For state() == MEMORY
					blocks, this field can be repurposed
					for something else.

					When this field counts log records
					and bytes allocated for recv_sys.pages,
					the field is protected by
					recv_sys_t::mutex. */
  buf_page_t() : id_{0}
  {
    static_assert(NOT_USED == 0, "compatibility");
    memset((void*) this, 0, sizeof *this);
  }

  buf_page_t(const buf_page_t &b) :
    id_(b.id_), hash(b.hash),
    oldest_modification_(b.oldest_modification_),
    lock() /* not copied */,
    frame(b.frame), zip(b.zip),
#ifdef UNIV_DEBUG
    in_zip_hash(b.in_zip_hash), in_LRU_list(b.in_LRU_list),
    in_page_hash(b.in_page_hash), in_free_list(b.in_free_list),
#endif /* UNIV_DEBUG */
    list(b.list), LRU(b.LRU), old(b.old), freed_page_clock(b.freed_page_clock),
    access_time(b.access_time)
  {
    lock.init();
  }

  /** Initialize some more fields */
  void init(uint32_t state, page_id_t id)
  {
    ut_ad(state < REMOVE_HASH || state >= UNFIXED);
    id_= id;
    zip.fix= state;
    oldest_modification_= 0;
    lock.init();
    ut_d(in_zip_hash= false);
    ut_d(in_free_list= false);
    ut_d(in_LRU_list= false);
    ut_d(in_page_hash= false);
    old= 0;
    freed_page_clock= 0;
    access_time= 0;
  }

public:
  const page_id_t &id() const { return id_; }
  uint32_t state() const { return zip.fix; }
  uint32_t buf_fix_count() const
  {
    uint32_t f= state();
    ut_ad(f >= FREED);
    return f < UNFIXED ? (f - FREED) : (~LRU_MASK & f);
  }
  /** @return whether this block is read or write fixed;
  read_complete() or write_complete() will always release
  the io-fix before releasing U-lock or X-lock */
  bool is_io_fixed() const
  { const auto s= state(); ut_ad(s >= FREED); return s >= READ_FIX; }
  /** @return whether this block is write fixed;
  write_complete() will always release the write-fix before releasing U-lock */
  bool is_write_fixed() const { return state() >= WRITE_FIX; }
  /** @return whether this block is read fixed; this should never hold
  when a thread is holding the block lock in any mode */
  bool is_read_fixed() const { return is_io_fixed() && !is_write_fixed(); }

  /** @return if this belongs to buf_pool.unzip_LRU */
  bool belongs_to_unzip_LRU() const
  { return UNIV_LIKELY_NULL(zip.data) && frame; }

  bool is_freed() const
  { const auto s= state(); ut_ad(s >= FREED); return s < UNFIXED; }
  bool is_ibuf_exist() const
  {
    const auto s= state();
    ut_ad(s >= UNFIXED);
    ut_ad(s < READ_FIX);
    return (s & LRU_MASK) == IBUF_EXIST;
  }
  bool is_reinit() const { return !(~state() & REINIT); }

  void set_reinit(uint32_t prev_state)
  {
    ut_ad(prev_state < READ_FIX);
    ut_d(const auto s=) zip.fix.fetch_add(REINIT - prev_state);
    ut_ad(s > prev_state);
    ut_ad(s < prev_state + UNFIXED);
  }

  void set_ibuf_exist()
  {
    ut_ad(lock.is_write_locked());
    ut_ad(id() < page_id_t(SRV_SPACE_ID_UPPER_BOUND, 0));
    const auto s= state();
    ut_ad(s >= UNFIXED);
    ut_ad(s < READ_FIX);
    ut_ad(s < IBUF_EXIST || s >= REINIT);
    zip.fix.fetch_add(IBUF_EXIST - (LRU_MASK & s));
  }
  void clear_ibuf_exist()
  {
    ut_ad(lock.is_write_locked());
    ut_ad(id() < page_id_t(SRV_SPACE_ID_UPPER_BOUND, 0));
    ut_d(const auto s=) zip.fix.fetch_sub(IBUF_EXIST - UNFIXED);
    ut_ad(s >= IBUF_EXIST);
    ut_ad(s < REINIT);
  }

  void read_unfix(uint32_t s)
  {
    ut_ad(lock.is_write_locked());
    ut_ad(s == UNFIXED + 1 || s == IBUF_EXIST + 1 || s == REINIT + 1);
    ut_d(auto old_state=) zip.fix.fetch_add(s - READ_FIX);
    ut_ad(old_state >= READ_FIX);
    ut_ad(old_state < WRITE_FIX);
  }

  void set_freed(uint32_t prev_state, uint32_t count= 0)
  {
    ut_ad(lock.is_write_locked());
    ut_ad(prev_state >= UNFIXED);
    ut_ad(prev_state < READ_FIX);
    ut_d(auto s=) zip.fix.fetch_sub((prev_state & LRU_MASK) - FREED - count);
    ut_ad(!((prev_state ^ s) & LRU_MASK));
  }

  inline void set_state(uint32_t s);
  inline void set_corrupt_id();

  /** @return the log sequence number of the oldest pending modification
  @retval 0 if the block is being removed from (or not in) buf_pool.flush_list
  @retval 1 if the block is in buf_pool.flush_list but not modified
  @retval 2 if the block belongs to the temporary tablespace and
  has unwritten changes */
  lsn_t oldest_modification() const { return oldest_modification_; }
  /** @return the log sequence number of the oldest pending modification,
  @retval 0 if the block is definitely not in buf_pool.flush_list
  @retval 1 if the block is in buf_pool.flush_list but not modified
  @retval 2 if the block belongs to the temporary tablespace and
  has unwritten changes */
  lsn_t oldest_modification_acquire() const
  { return oldest_modification_.load(std::memory_order_acquire); }
  /** Set oldest_modification when adding to buf_pool.flush_list */
  inline void set_oldest_modification(lsn_t lsn);
  /** Clear oldest_modification after removing from buf_pool.flush_list */
  inline void clear_oldest_modification();
  /** Reset the oldest_modification when marking a persistent page freed */
  void reset_oldest_modification()
  {
    ut_ad(oldest_modification() > 2);
    oldest_modification_.store(1, std::memory_order_release);
  }

  /** Complete a read of a page.
  @param node     data file
  @return whether the operation succeeded
  @retval DB_PAGE_CORRUPTED    if the checksum fails
  @retval DB_DECRYPTION_FAILED if the page cannot be decrypted */
  dberr_t read_complete(const fil_node_t &node);

  /** Note that a block is no longer dirty, while not removing
  it from buf_pool.flush_list */
  inline void write_complete(bool temporary);

  /** Write a flushable page to a file. buf_pool.mutex must be held.
  @param lru         true=buf_pool.LRU; false=buf_pool.flush_list
  @param space       tablespace
  @return whether the page was flushed and buf_pool.mutex was released */
  inline bool flush(bool lru, fil_space_t *space);

  /** Notify that a page in a temporary tablespace has been modified. */
  void set_temp_modified()
  {
    ut_ad(fsp_is_system_temporary(id().space()));
    ut_ad(in_file());
    ut_ad(!oldest_modification());
    oldest_modification_= 2;
  }

  /** Prepare to release a file page to buf_pool.free. */
  void free_file_page()
  {
    ut_ad((zip.fix.fetch_sub(REMOVE_HASH - MEMORY)) == REMOVE_HASH);
    /* buf_LRU_block_free_non_file_page() asserts !oldest_modification() */
    ut_d(oldest_modification_= 0;)
    id_= page_id_t(~0ULL);
  }

  void fix_on_recovery()
  {
    ut_d(const auto f=) zip.fix.fetch_sub(READ_FIX - UNFIXED - 1);
    ut_ad(f >= READ_FIX);
    ut_ad(f < WRITE_FIX);
  }

  uint32_t fix(uint32_t count= 1)
  {
    ut_ad(count);
    ut_ad(count < IBUF_EXIST);
    uint32_t f= zip.fix.fetch_add(count);
    ut_ad(f >= FREED);
    ut_ad(!((f ^ (f + 1)) & LRU_MASK));
    return f;
  }

  uint32_t unfix()
  {
    uint32_t f= zip.fix.fetch_sub(1);
    ut_ad(f > FREED);
    ut_ad(!((f ^ (f - 1)) & LRU_MASK));
    return f - 1;
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

  /** @return the byte offset of the page within a file */
  os_offset_t physical_offset() const
  {
    os_offset_t o= id().page_no();
    return zip.ssize
      ? o << (zip.ssize + (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
      : o << srv_page_size_shift;
  }

  /** @return whether the block is mapped to a data file */
  bool in_file() const { return state() >= FREED; }

  /** @return whether the block is modified and ready for flushing */
  inline bool ready_for_flush() const;
  /** @return whether the block can be relocated in memory.
  The block can be dirty, but it must not be I/O-fixed or bufferfixed. */
  inline bool can_relocate() const;
  /** @return whether the block has been flagged old in buf_pool.LRU */
  inline bool is_old() const;
  /** Set whether a block is old in buf_pool.LRU */
  inline void set_old(bool old);
  /** Flag a page accessed in buf_pool
  @return whether this is not the first access */
  bool set_accessed()
  {
    if (is_accessed()) return true;
    access_time= static_cast<uint32_t>(ut_time_ms());
    return false;
  }
  /** @return ut_time_ms() at the time of first access of a block in buf_pool
  @retval 0 if not accessed */
  unsigned is_accessed() const { ut_ad(in_file()); return access_time; }
};

/** The buffer control block structure */

struct buf_block_t{

	/** @name General fields */
	/* @{ */

	buf_page_t	page;		/*!< page information; this must
					be the first field, so that
					buf_pool.page_hash can point
					to buf_page_t or buf_block_t */
#ifdef UNIV_DEBUG
  /** whether page.list is in buf_pool.withdraw
  ((state() == NOT_USED)) and the buffer pool is being shrunk;
  protected by buf_pool.mutex */
  bool in_withdraw_list;
  /** whether unzip_LRU is in buf_pool.unzip_LRU
  (in_file() && frame && zip.data);
  protected by buf_pool.mutex */
  bool in_unzip_LRU_list;
#endif
  /** member of buf_pool.unzip_LRU (if belongs_to_unzip_LRU()) */
  UT_LIST_NODE_T(buf_block_t) unzip_LRU;
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
	(1) in_file(), and we are holding lock in any mode, or
	(2) !is_read_fixed()&&(state()>=UNFIXED||state()==REMOVE_HASH).

	An exception to this is when we init or create a page
	in the buffer pool in buf0buf.cc.

	Another exception for buf_pool_t::clear_hash_index() is that
	assigning block->index = NULL (and block->n_pointers = 0)
	is allowed whenever all AHI latches are exclusively locked.

	Another exception is that ha_insert_for_fold() may
	decrement n_pointers without holding the appropriate latch
	in btr_search_latches[]. Thus, n_pointers must be
	protected by atomic memory access.

	This implies that the fields may be read without race
	condition whenever any of the following hold:
	- the btr_search_sys.partition[].latch is being held, or
	- state() == NOT_USED || state() == MEMORY,
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
					pointing to this frame */
#  define assert_block_ahi_empty(block)					\
	ut_a((block)->n_pointers == 0)
#  define assert_block_ahi_empty_on_init(block) do {			\
	MEM_MAKE_DEFINED(&(block)->n_pointers, sizeof (block)->n_pointers); \
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
  void fix() { page.fix(); }
  uint32_t unfix() { return page.unfix(); }

  /** @return the physical size, in bytes */
  ulint physical_size() const { return page.physical_size(); }

  /** @return the ROW_FORMAT=COMPRESSED physical size, in bytes
  @retval 0 if not compressed */
  ulint zip_size() const { return page.zip_size(); }

  /** Initialize the block.
  @param page_id  page identifier
  @param zip_size ROW_FORMAT=COMPRESSED page size, or 0
  @param state    initial state() */
  void initialise(const page_id_t page_id, ulint zip_size, uint32_t state);
};

/**********************************************************************//**
Compute the hash fold value for blocks in buf_pool.zip_hash. */
/* @{ */
#define BUF_POOL_ZIP_FOLD_PTR(ptr) (ulint(ptr) >> srv_page_size_shift)
#define BUF_POOL_ZIP_FOLD(b) BUF_POOL_ZIP_FOLD_PTR((b)->page.frame)
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
  buf_page_t *get() const { mysql_mutex_assert_owner(m_mutex); return m_hp; }

  /** Set current value
  @param bpage buffer block to be set as hp */
  void set(buf_page_t *bpage)
  {
    mysql_mutex_assert_owner(m_mutex);
    ut_ad(!bpage || bpage->in_file());
    m_hp= bpage;
  }

  /** Checks if a bpage is the hp
  @param bpage  buffer block to be compared
  @return true if it is hp */
  bool is_hp(const buf_page_t *bpage) const
  { mysql_mutex_assert_owner(m_mutex); return bpage == m_hp; }

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list. */
  virtual void adjust(const buf_page_t*) = 0;

#ifdef UNIV_DEBUG
  /** mutex that protects access to the m_hp. */
  const mysql_mutex_t *m_mutex= nullptr;
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

    ut_ad(!m_hp || m_hp->oldest_modification());
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
	/** Initialize the counters */
	void init() { memset((void*) this, 0, sizeof *this); }

	ib_counter_t<ulint, ib_counter_element_t>	n_page_gets;
				/*!< number of page gets performed;
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
				buf_page_make_young() */
	ulint	n_pages_not_made_young; /*!< number of pages not made
				young because the first access
				was not long enough ago, in
				buf_page_peek_if_too_old() */
	/** number of waits for eviction; writes protected by buf_pool.mutex */
	ulint	LRU_waits;
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
    void reg() { map_reg->emplace(map::value_type(blocks->page.frame, this)); }

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
public:
  /** Hash cell chain in page_hash_table */
  struct hash_chain
  {
    /** pointer to the first block */
    buf_page_t *first;
  };
private:
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
    return UNIV_UNLIKELY(resizing.load(std::memory_order_relaxed));
  }

  /** @return the current size in blocks */
  size_t get_n_pages() const
  {
    ut_ad(is_initialised());
    size_t size= 0;
    for (auto j= ut_min(n_chunks_new, n_chunks); j--; )
      size+= chunks[j].size;
    return size;
  }

  /** Determine whether a frame is intended to be withdrawn during resize().
  @param ptr    pointer within a buf_page_t::frame
  @return whether the frame will be withdrawn */
  bool will_be_withdrawn(const byte *ptr) const
  {
    ut_ad(n_chunks_new < n_chunks);
#ifdef SAFE_MUTEX
    if (resize_in_progress())
      mysql_mutex_assert_owner(&mutex);
#endif /* SAFE_MUTEX */

    for (const chunk_t *chunk= chunks + n_chunks_new,
         * const echunk= chunks + n_chunks;
         chunk != echunk; chunk++)
      if (ptr >= chunk->blocks->page.frame &&
          ptr < (chunk->blocks + chunk->size - 1)->page.frame + srv_page_size)
        return true;
    return false;
  }

  /** Determine whether a block is intended to be withdrawn during resize().
  @param bpage  buffer pool block
  @return whether the frame will be withdrawn */
  bool will_be_withdrawn(const buf_page_t &bpage) const
  {
    ut_ad(n_chunks_new < n_chunks);
#ifdef SAFE_MUTEX
    if (resize_in_progress())
      mysql_mutex_assert_owner(&mutex);
#endif /* SAFE_MUTEX */

    for (const chunk_t *chunk= chunks + n_chunks_new,
         * const echunk= chunks + n_chunks;
         chunk != echunk; chunk++)
      if (&bpage >= &chunk->blocks->page &&
          &bpage < &chunk->blocks[chunk->size].page)
        return true;
    return false;
  }

  /** Release and evict a corrupted page.
  @param bpage    page that was being read */
  ATTRIBUTE_COLD void corrupted_evict(buf_page_t *bpage);

  /** Release a memory block to the buffer pool. */
  ATTRIBUTE_COLD void free_block(buf_block_t *block);

#ifdef UNIV_DEBUG
  /** Find a block that points to a ROW_FORMAT=COMPRESSED page
  @param data  pointer to the start of a ROW_FORMAT=COMPRESSED page frame
  @return the block
  @retval nullptr  if not found */
  const buf_block_t *contains_zip(const void *data) const
  {
    mysql_mutex_assert_owner(&mutex);
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
  inline buf_block_t *block_from_ahi(const byte *ptr) const;
#endif /* BTR_CUR_HASH_ADAPT */

  /**
  @return the smallest oldest_modification lsn for any page
  @retval empty_lsn if all modified persistent pages have been flushed */
  lsn_t get_oldest_modification(lsn_t empty_lsn)
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    while (buf_page_t *bpage= UT_LIST_GET_LAST(flush_list))
    {
      ut_ad(!fsp_is_system_temporary(bpage->id().space()));
      lsn_t lsn= bpage->oldest_modification();
      if (lsn != 1)
      {
        ut_ad(lsn > 2);
        return lsn;
      }
      delete_from_flush_list(bpage);
    }
    return empty_lsn;
  }

  /** Determine if a buffer block was created by chunk_t::create().
  @param block  block descriptor (not dereferenced)
  @return whether block has been created by chunk_t::create() */
  bool is_uncompressed(const buf_block_t *block) const
  {
    return is_block_field(reinterpret_cast<const void*>(block));
  }

public:
  /** @return whether the buffer pool contains a page
  @tparam allow_watch  whether to allow watch_is_sentinel()
  @param page_id       page identifier
  @param chain         hash table chain for page_id.fold() */
  template<bool allow_watch= false>
  TRANSACTIONAL_INLINE
  bool page_hash_contains(const page_id_t page_id, hash_chain &chain)
  {
    transactional_shared_lock_guard<page_hash_latch> g
      {page_hash.lock_get(chain)};
    buf_page_t *bpage= page_hash.get(page_id, chain);
    if (bpage >= &watch[0] && bpage < &watch[UT_ARR_SIZE(watch)])
    {
      ut_ad(!bpage->in_zip_hash);
      ut_ad(!bpage->zip.data);
      if (!allow_watch)
        bpage= nullptr;
    }
    return bpage;
  }

  /** Determine if a block is a sentinel for a buffer pool watch.
  @param bpage page descriptor
  @return whether bpage a sentinel for a buffer pool watch */
  bool watch_is_sentinel(const buf_page_t &bpage)
  {
#ifdef SAFE_MUTEX
    DBUG_ASSERT(mysql_mutex_is_owner(&mutex) ||
                page_hash.lock_get(page_hash.cell_get(bpage.id().fold())).
                is_locked());
#endif /* SAFE_MUTEX */
    ut_ad(bpage.in_file());
    if (&bpage < &watch[0] || &bpage >= &watch[array_elements(watch)])
      return false;
    ut_ad(!bpage.in_zip_hash);
    ut_ad(!bpage.zip.data);
    return true;
  }

  /** Check if a watched page has been read.
  This may only be called after !watch_set() and before invoking watch_unset().
  @param id   page identifier
  @return whether the page was read to the buffer pool */
  TRANSACTIONAL_INLINE
  bool watch_occurred(const page_id_t id)
  {
    hash_chain &chain= page_hash.cell_get(id.fold());
    transactional_shared_lock_guard<page_hash_latch> g
      {page_hash.lock_get(chain)};
    /* The page must exist because watch_set() increments buf_fix_count. */
    return !watch_is_sentinel(*page_hash.get(id, chain));
  }

  /** Register a watch for a page identifier. The caller must hold an
  exclusive page hash latch. The *hash_lock may be released,
  relocated, and reacquired.
  @param id         page identifier
  @param chain      hash table chain with exclusively held page_hash
  @return a buffer pool block corresponding to id
  @retval nullptr   if the block was not present, and a watch was installed */
  inline buf_page_t *watch_set(const page_id_t id, hash_chain &chain);

  /** Stop watching whether a page has been read in.
  watch_set(id) must have returned nullptr before.
  @param id         page identifier
  @param chain      unlocked hash table chain */
  void watch_unset(const page_id_t id, hash_chain &chain);

  /** Remove the sentinel block for the watch before replacing it with a
  real block. watch_unset() or watch_occurred() will notice
  that the block has been replaced with the real block.
  @param w          sentinel
  @param chain      locked hash table chain
  @return           w->state() */
  inline uint32_t watch_remove(buf_page_t *w, hash_chain &chain);

  /** @return whether less than 1/4 of the buffer pool is available */
  TPOOL_SUPPRESS_TSAN
  bool running_out() const
  {
    return !recv_recovery_is_on() &&
      UT_LIST_GET_LEN(free) + UT_LIST_GET_LEN(LRU) <
        n_chunks_new / 4 * chunks->size;
  }

  /** @return whether the buffer pool is shrinking */
  inline bool is_shrinking() const
  {
    return n_chunks_new < n_chunks;
  }

#ifdef UNIV_DEBUG
  /** Validate the buffer pool. */
  void validate();
#endif /* UNIV_DEBUG */
#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG
  /** Write information of the buf_pool to the error log. */
  void print();
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG */

  /** Remove a block from the LRU list.
  @return the predecessor in the LRU list */
  buf_page_t *LRU_remove(buf_page_t *bpage)
  {
    mysql_mutex_assert_owner(&mutex);
    ut_ad(bpage->in_LRU_list);
    ut_ad(bpage->in_page_hash);
    ut_ad(!bpage->in_zip_hash);
    ut_ad(bpage->in_file());
    lru_hp.adjust(bpage);
    lru_scan_itr.adjust(bpage);
    ut_d(bpage->in_LRU_list= false);
    buf_page_t *prev= UT_LIST_GET_PREV(LRU, bpage);
    UT_LIST_REMOVE(LRU, bpage);
    return prev;
  }

  /** Number of pages to read ahead */
  static constexpr uint32_t READ_AHEAD_PAGES= 64;

  /** Buffer pool mutex */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t mutex;
  /** Number of pending LRU flush; protected by mutex. */
  ulint n_flush_LRU_;
  /** broadcast when n_flush_LRU reaches 0; protected by mutex */
  pthread_cond_t done_flush_LRU;
  /** Number of pending flush_list flush; protected by mutex */
  ulint n_flush_list_;
  /** broadcast when n_flush_list reaches 0; protected by mutex */
  pthread_cond_t done_flush_list;

  TPOOL_SUPPRESS_TSAN ulint n_flush_LRU() const { return n_flush_LRU_; }
  TPOOL_SUPPRESS_TSAN ulint n_flush_list() const { return n_flush_list_; }

	/** @name General fields */
	/* @{ */
	ulint		curr_pool_size;	/*!< Current pool size in bytes */
	ulint		LRU_old_ratio;  /*!< Reserve this much of the buffer
					pool for "old" blocks */
#ifdef UNIV_DEBUG
	ulint		buddy_n_frames; /*!< Number of frames allocated from
					the buffer pool to the buddy system */
	ulint		mutex_exit_forbidden; /*!< Forbid release mutex */
#endif
	ut_allocator<unsigned char>	allocator;	/*!< Allocator used for
					allocating memory for the the "chunks"
					member. */
	ulint		n_chunks;	/*!< number of buffer pool chunks */
	ulint		n_chunks_new;	/*!< new number of buffer pool chunks.
					both n_chunks{,new} are protected under
					mutex */
	chunk_t*	chunks;		/*!< buffer pool chunks */
	chunk_t*	chunks_old;	/*!< old buffer pool chunks to be freed
					after resizing buffer pool */
	/** current pool size in pages */
	Atomic_counter<ulint> curr_size;
	/** read-ahead request size in pages */
	Atomic_counter<uint32_t> read_ahead_area;

  /** Hash table with singly-linked overflow lists */
  struct page_hash_table
  {
    static_assert(CPU_LEVEL1_DCACHE_LINESIZE >= 64, "less than 64 bytes");
    static_assert(!(CPU_LEVEL1_DCACHE_LINESIZE & 63),
      "not a multiple of 64 bytes");

    /** Number of array[] elements per page_hash_latch.
    Must be one less than a power of 2. */
    static constexpr size_t ELEMENTS_PER_LATCH= 64 / sizeof(void*) - 1;
    static constexpr size_t EMPTY_SLOTS_PER_LATCH=
      ((CPU_LEVEL1_DCACHE_LINESIZE / 64) - 1) * (64 / sizeof(void*));

    /** number of payload elements in array[] */
    Atomic_relaxed<ulint> n_cells;
    /** the hash table, with pad(n_cells) elements, aligned to L1 cache size */
    hash_chain *array;

    /** Create the hash table.
    @param n  the lower bound of n_cells */
    void create(ulint n);

    /** Free the hash table. */
    void free() { aligned_free(array); array= nullptr; }

    /** @return the index of an array element */
    ulint calc_hash(ulint fold) const { return calc_hash(fold, n_cells); }
    /** @return raw array index converted to padded index */
    static ulint pad(ulint h)
    {
      ulint latches= h / ELEMENTS_PER_LATCH;
      ulint empty_slots= latches * EMPTY_SLOTS_PER_LATCH;
      return 1 + latches + empty_slots + h;
    }
  private:
    /** @return the hash value before any ELEMENTS_PER_LATCH padding */
    static ulint hash(ulint fold, ulint n) { return ut_hash_ulint(fold, n); }

    /** @return the index of an array element */
    static ulint calc_hash(ulint fold, ulint n_cells)
    {
      return pad(hash(fold, n_cells));
    }
  public:
    /** @return the latch covering a hash table chain */
    static page_hash_latch &lock_get(hash_chain &chain)
    {
      static_assert(!((ELEMENTS_PER_LATCH + 1) & ELEMENTS_PER_LATCH),
                    "must be one less than a power of 2");
      const size_t addr= reinterpret_cast<size_t>(&chain);
      ut_ad(addr & (ELEMENTS_PER_LATCH * sizeof chain));
      return *reinterpret_cast<page_hash_latch*>
        (addr & ~(ELEMENTS_PER_LATCH * sizeof chain));
    }

    /** Get a hash table slot. */
    hash_chain &cell_get(ulint fold) const
    { return array[calc_hash(fold, n_cells)]; }

    /** Append a block descriptor to a hash bucket chain. */
    void append(hash_chain &chain, buf_page_t *bpage)
    {
      ut_ad(!bpage->in_page_hash);
      ut_ad(!bpage->hash);
      ut_d(bpage->in_page_hash= true);
      buf_page_t **prev= &chain.first;
      while (*prev)
      {
        ut_ad((*prev)->in_page_hash);
        prev= &(*prev)->hash;
      }
      *prev= bpage;
    }

    /** Remove a block descriptor from a hash bucket chain. */
    void remove(hash_chain &chain, buf_page_t *bpage)
    {
      ut_ad(bpage->in_page_hash);
      buf_page_t **prev= &chain.first;
      while (*prev != bpage)
      {
        ut_ad((*prev)->in_page_hash);
        prev= &(*prev)->hash;
      }
      *prev= bpage->hash;
      ut_d(bpage->in_page_hash= false);
      bpage->hash= nullptr;
    }

    /** Replace a block descriptor with another. */
    void replace(hash_chain &chain, buf_page_t *old, buf_page_t *bpage)
    {
      ut_ad(old->in_page_hash);
      ut_ad(bpage->in_page_hash);
      ut_d(old->in_page_hash= false);
      ut_ad(bpage->hash == old->hash);
      old->hash= nullptr;
      buf_page_t **prev= &chain.first;
      while (*prev != old)
      {
        ut_ad((*prev)->in_page_hash);
        prev= &(*prev)->hash;
      }
      *prev= bpage;
    }

    /** Look up a page in a hash bucket chain. */
    inline buf_page_t *get(const page_id_t id, const hash_chain &chain) const;

    /** Exclusively aqcuire all latches */
    inline void write_lock_all();

    /** Release all latches */
    inline void write_unlock_all();
  };

  /** Hash table of file pages (buf_page_t::in_file() holds),
  indexed by page_id_t. Protected by both mutex and page_hash.lock_get(). */
  page_hash_table page_hash;

  /** map of block->frame to buf_block_t blocks that belong
  to buf_buddy_alloc(); protected by buf_pool.mutex */
  hash_table_t zip_hash;
	/** number of pending read operations */
	Atomic_counter<ulint> n_pend_reads;
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

  /** mutex protecting flush_list, buf_page_t::set_oldest_modification()
  and buf_page_t::list pointers when !oldest_modification() */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t flush_list_mutex;
  /** "hazard pointer" for flush_list scans; protected by flush_list_mutex */
  FlushHp flush_hp;
  /** modified blocks (a subset of LRU) */
  UT_LIST_BASE_NODE_T(buf_page_t) flush_list;
  /** number of blocks ever added to flush_list;
  protected by flush_list_mutex */
  size_t flush_list_requests;
private:
  /** whether the page cleaner needs wakeup from indefinite sleep */
  bool page_cleaner_is_idle;
  /** track server activity count for signaling idle flushing */
  ulint last_activity_count;
public:
  /** signalled to wake up the page_cleaner; protected by flush_list_mutex */
  pthread_cond_t do_flush_list;

  /** @return whether the page cleaner must sleep due to being idle */
  bool page_cleaner_idle() const noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    return page_cleaner_is_idle;
  }
  /** Wake up the page cleaner if needed */
  void page_cleaner_wakeup();

  /** Register whether an explicit wakeup of the page cleaner is needed */
  void page_cleaner_set_idle(bool deep_sleep)
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    page_cleaner_is_idle= deep_sleep;
  }

  /** Update server last activity count */
  void update_last_activity_count(ulint activity_count)
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    last_activity_count= activity_count;
  }

  // n_flush_LRU() + n_flush_list()
  // is approximately COUNT(is_write_fixed()) in flush_list

	unsigned	freed_page_clock;/*!< a sequence number used
					to count the number of buffer
					blocks removed from the end of
					the LRU list; NOTE that this
					counter may wrap around at 4
					billion! A thread is allowed
					to read this for heuristic
					purposes without holding any
					mutex or latch */
	bool		try_LRU_scan;	/*!< Cleared when an LRU
					scan for free block fails. This
					flag is used to avoid repeated
					scans of LRU list when we know
					that there is no free block
					available in the scan depth for
					eviction. Set whenever
					we flush a batch from the
					buffer pool. Protected by the
					buf_pool.mutex */
	/* @} */

	/** @name LRU replacement algorithm fields */
	/* @{ */

	UT_LIST_BASE_NODE_T(buf_page_t) free;
					/*!< base node of the free
					block list */
  /** signaled each time when the free list grows; protected by mutex */
  pthread_cond_t done_free;

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
  /** free ROW_FORMAT=COMPRESSED page frames */
  UT_LIST_BASE_NODE_T(buf_buddy_free_t) zip_free[BUF_BUDDY_SIZES_MAX];
#if BUF_BUDDY_LOW > UNIV_ZIP_SIZE_MIN
# error "BUF_BUDDY_LOW > UNIV_ZIP_SIZE_MIN"
#endif

  /** Sentinels to detect if pages are read into the buffer pool while
  a delete-buffering operation is pending. Protected by mutex. */
  buf_page_t watch[innodb_purge_threads_MAX + 1];
  /** Reserve a buffer. */
  buf_tmp_buffer_t *io_buf_reserve() { return io_buf.reserve(); }

  /** @return whether any I/O is pending */
  bool any_io_pending()
  {
    if (n_pend_reads)
      return true;
    mysql_mutex_lock(&mutex);
    const bool any_pending{n_flush_LRU_ || n_flush_list_};
    mysql_mutex_unlock(&mutex);
    return any_pending;
  }
  /** @return total amount of pending I/O */
  ulint io_pending() const
  {
    return n_pend_reads + n_flush_LRU() + n_flush_list();
  }

private:
  /** Remove a block from the flush list. */
  inline void delete_from_flush_list_low(buf_page_t *bpage) noexcept;
  /** Remove a block from flush_list.
  @param bpage   buffer pool page
  @param clear   whether to invoke buf_page_t::clear_oldest_modification() */
  void delete_from_flush_list(buf_page_t *bpage, bool clear) noexcept;
public:
  /** Remove a block from flush_list.
  @param bpage   buffer pool page */
  void delete_from_flush_list(buf_page_t *bpage) noexcept
  { delete_from_flush_list(bpage, true); }

  /** Insert a modified block into the flush list.
  @param block    modified block
  @param lsn      start LSN of the mini-transaction that modified the block */
  void insert_into_flush_list(buf_block_t *block, lsn_t lsn) noexcept;

  /** Free a page whose underlying file page has been freed. */
  inline void release_freed_page(buf_page_t *bpage) noexcept;

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
};

/** The InnoDB buffer pool */
extern buf_pool_t buf_pool;

inline buf_page_t *buf_pool_t::page_hash_table::get(const page_id_t id,
                                                    const hash_chain &chain)
  const
{
#ifdef SAFE_MUTEX
  DBUG_ASSERT(mysql_mutex_is_owner(&buf_pool.mutex) ||
              lock_get(const_cast<hash_chain&>(chain)).is_locked());
#endif /* SAFE_MUTEX */
  for (buf_page_t *bpage= chain.first; bpage; bpage= bpage->hash)
  {
    ut_ad(bpage->in_page_hash);
    ut_ad(bpage->in_file());
    if (bpage->id() == id)
      return bpage;
  }
  return nullptr;
}

#ifdef SUX_LOCK_GENERIC
inline void page_hash_latch::lock_shared()
{
  mysql_mutex_assert_not_owner(&buf_pool.mutex);
  if (!read_trylock())
    read_lock_wait();
}

inline void page_hash_latch::lock()
{
  if (!write_trylock())
    write_lock_wait();
}
#endif /* SUX_LOCK_GENERIC */

inline void buf_page_t::set_state(uint32_t s)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(s <= REMOVE_HASH || s >= UNFIXED);
  ut_ad(s < WRITE_FIX);
  ut_ad(s <= READ_FIX || zip.fix == READ_FIX);
  zip.fix= s;
}

inline void buf_page_t::set_corrupt_id()
{
#ifdef UNIV_DEBUG
  switch (oldest_modification()) {
  case 0:
    break;
  case 2:
    ut_ad(fsp_is_system_temporary(id().space()));
    /* buf_LRU_block_free_non_file_page() asserts !oldest_modification() */
    ut_d(oldest_modification_= 0;)
    break;
  default:
    ut_ad("block is dirty" == 0);
  }
  const auto f= state();
  if (f != REMOVE_HASH)
  {
    ut_ad(f >= UNFIXED);
    ut_ad(buf_pool.page_hash.lock_get(buf_pool.page_hash.cell_get(id_.fold())).
          is_write_locked());
  }
#endif
  id_= page_id_t(~0ULL);
}

/** Set oldest_modification when adding to buf_pool.flush_list */
inline void buf_page_t::set_oldest_modification(lsn_t lsn)
{
  mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
  ut_ad(oldest_modification() <= 1);
  ut_ad(lsn > 2);
  oldest_modification_= lsn;
}

/** Clear oldest_modification after removing from buf_pool.flush_list */
inline void buf_page_t::clear_oldest_modification()
{
  mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
  ut_d(const auto s= state());
  ut_ad(s >= REMOVE_HASH);
  ut_ad(oldest_modification());
  ut_ad(!list.prev);
  ut_ad(!list.next);
  /* We must use release memory order to guarantee that callers of
  oldest_modification_acquire() will observe the block as
  being detached from buf_pool.flush_list, after reading the value 0. */
  oldest_modification_.store(0, std::memory_order_release);
}

/** @return whether the block is modified and ready for flushing */
inline bool buf_page_t::ready_for_flush() const
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(in_LRU_list);
  const auto s= state();
  ut_a(s >= FREED);
  ut_ad(!fsp_is_system_temporary(id().space()) || oldest_modification() == 2);
  return s < READ_FIX;
}

/** @return whether the block can be relocated in memory.
The block can be dirty, but it must not be I/O-fixed or bufferfixed. */
inline bool buf_page_t::can_relocate() const
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  const auto f= state();
  ut_ad(f >= FREED);
  ut_ad(in_LRU_list);
  return (f == FREED || (f < READ_FIX && !(f & ~LRU_MASK))) &&
    !lock.is_locked_or_waiting();
}

/** @return whether the block has been flagged old in buf_pool.LRU */
inline bool buf_page_t::is_old() const
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(in_file());
  ut_ad(in_LRU_list);
  return old;
}

/** Set whether a block is old in buf_pool.LRU */
inline void buf_page_t::set_old(bool old)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(in_LRU_list);

#ifdef UNIV_LRU_DEBUG
  ut_a((buf_pool.LRU_old_len == 0) == (buf_pool.LRU_old == nullptr));
  /* If a block is flagged "old", the LRU_old list must exist. */
  ut_a(!old || buf_pool.LRU_old);

  if (UT_LIST_GET_PREV(LRU, this) && UT_LIST_GET_NEXT(LRU, this))
  {
    const buf_page_t *prev= UT_LIST_GET_PREV(LRU, this);
    const buf_page_t *next = UT_LIST_GET_NEXT(LRU, this);
    if (prev->old == next->old)
      ut_a(prev->old == old);
    else
    {
      ut_a(!prev->old);
      ut_a(buf_pool.LRU_old == (old ? this : next));
    }
  }
#endif /* UNIV_LRU_DEBUG */

  this->old= old;
}

#ifdef UNIV_DEBUG
/** Forbid the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_forbid() do {		\
	mysql_mutex_assert_owner(&buf_pool.mutex);	\
	buf_pool.mutex_exit_forbidden++;		\
} while (0)
/** Allow the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_allow() do {		\
	mysql_mutex_assert_owner(&buf_pool.mutex);	\
	ut_ad(buf_pool.mutex_exit_forbidden--);		\
} while (0)
#else
/** Forbid the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_forbid() ((void) 0)
/** Allow the release of the buffer pool mutex. */
# define buf_pool_mutex_exit_allow() ((void) 0)
#endif

/**********************************************************************
Let us list the consistency conditions for different control block states.

NOT_USED:	is in free list, not LRU, not flush_list, nor page_hash
MEMORY:		is not in any of free, LRU, flush_list, page_hash
in_file():	is not in free list, is in LRU list, id() is defined,
		is in page_hash (not necessarily if is_read_fixed())

		is in buf_pool.flush_list, if and only
		if oldest_modification == 1 || oldest_modification > 2

		(1) if is_write_fixed(): is u-locked
		(2) if is_read_fixed(): is x-locked

State transitions:

NOT_USED => MEMORY
MEMORY => NOT_USED
MEMORY => UNFIXED
UNFIXED => in_file()
in_file() => UNFIXED or FREED
UNFIXED or FREED => REMOVE_HASH
REMOVE_HASH => NOT_USED	(if and only if !oldest_modification())
*/

/** Select from where to start a scan. If we have scanned
too deep into the LRU list it resets the value to the tail
of the LRU list.
@return buf_page_t from where to start scan. */
inline buf_page_t *LRUItr::start()
{
  mysql_mutex_assert_owner(m_mutex);

  if (!m_hp || m_hp->old)
    m_hp= UT_LIST_GET_LAST(buf_pool.LRU);

  return m_hp;
}

#ifdef UNIV_DEBUG
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
#endif /* UNIV_DEBUG */

#include "buf0buf.inl"

#endif /* !UNIV_INNOCHECKSUM */

#endif
