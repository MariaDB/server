/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2023, MariaDB Corporation.

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

#pragma once

/** Magic value to use instead of checksums when they are disabled */
#define BUF_NO_CHECKSUM_MAGIC 0xDEADBEEFUL

#include "fil0fil.h"
#include "mtr0types.h"
#include "span.h"
#include "assume_aligned.h"
#include "buf0types.h"
#ifndef UNIV_INNOCHECKSUM
#include "page0types.h"
#include "log0log.h"
#include "srv0srv.h"
#include "transactional_lock_guard.h"
#include <ostream>

struct trx_t;

/** The allocation granularity of innodb_buffer_pool_size */
constexpr size_t innodb_buffer_pool_extent_size=
  sizeof(size_t) < 8 ? 2 << 20 : 8 << 20;

/** @name Modes for buf_page_get_gen */
/* @{ */
#define BUF_GET			10	/*!< get always */
#define BUF_GET_RECOVER		9	/*!< like BUF_GET, but in recv_sys.recover() */
#define	BUF_GET_IF_IN_POOL	11	/*!< get if in pool */
#define BUF_PEEK_IF_IN_POOL	12	/*!< get if in pool, do not make
					the block young in the LRU list */
#define BUF_GET_POSSIBLY_FREED		16
					/*!< Like BUF_GET, but do not mind
					if the file page has been freed. */
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
	ulint	free_list_len;		/*!< free + lazy_allocate_size() */
	ulint	flush_list_len;		/*!< Length of buf_pool.flush_list */
	ulint	n_pend_unzip;		/*!< buf_pool.n_pend_unzip, pages
					pending decompress */
	ulint	n_pend_reads;		/*!< os_aio_pending_reads() */
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
# define buf_block_free(block) buf_pool.free_block(block)
# define buf_page_get(ID, SIZE, LA, MTR)                        \
	buf_page_get_gen(ID, SIZE, LA, NULL, BUF_GET, MTR)

/** Try to buffer-fix a page.
@param block         guessed block
@param id            expected block->page.id()
@return block if it was buffer-fixed
@retval nullptr if the block no longer is valid */
buf_block_t *buf_page_optimistic_fix(buf_block_t *block, page_id_t id) noexcept
  MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Try to acquire a page latch after buf_page_optimistic_fix().
@param block         buffer-fixed block
@param rw_latch      RW_S_LATCH or RW_X_LATCH
@param modify_clock  expected value of block->modify_clock
@param mtr           mini-transaction
@return block if the latch was acquired
@retval nullptr if block->unfix() was called because it no longer is valid */
buf_block_t *buf_page_optimistic_get(buf_block_t *block,
                                     rw_lock_type_t rw_latch,
                                     uint64_t modify_clock,
                                     mtr_t *mtr) noexcept
  MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Try to S-latch a page.
Suitable for using when holding the lock_sys latches (as it avoids deadlock).
@param[in]	page_id	page identifier
@param[in,out]	mtr	mini-transaction
@return the block
@retval nullptr if an S-latch cannot be granted immediately */
buf_block_t *buf_page_try_get(const page_id_t page_id, mtr_t *mtr) noexcept;

/** Get read access to a compressed page (usually of type
FIL_PAGE_TYPE_ZBLOB or FIL_PAGE_TYPE_ZBLOB2).
The page must be released with s_unlock().
@param page_id   page identifier
@return pointer to the block, s-latched */
buf_page_t *buf_page_get_zip(const page_id_t page_id) noexcept;

/** Get access to a database page. Buffered redo log may be applied.
@param[in]	page_id			page id
@param[in]	zip_size		ROW_FORMAT=COMPRESSED page size, or 0
@param[in]	rw_latch		latch mode
@param[in]	guess			guessed block or NULL
@param[in]	mode			BUF_GET, BUF_GET_IF_IN_POOL,
or BUF_PEEK_IF_IN_POOL
@param[in,out]	mtr			mini-transaction
@param[out]	err			DB_SUCCESS or error code
@return pointer to the block
@retval nullptr	if the block is corrupted or unavailable */
buf_block_t*
buf_page_get_gen(
	const page_id_t		page_id,
	ulint			zip_size,
	rw_lock_type_t		rw_latch,
	buf_block_t*		guess,
	ulint			mode,
	mtr_t*			mtr,
	dberr_t*		err = nullptr) noexcept;

/** Initialize a page in the buffer pool. The page is usually not read
from a file even if it cannot be found in the buffer buf_pool. This is one
of the functions which perform to a block a state transition NOT_USED => LRU
(the other is buf_page_get_gen()).
@param[in,out]	space		space object
@param[in]	offset		offset of the tablespace
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@param[in,out]	free_block	pre-allocated buffer block
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create(fil_space_t *space, uint32_t offset,
                ulint zip_size, mtr_t *mtr, buf_block_t *free_block)
  noexcept;

/** Initialize a page in buffer pool while initializing the
deferred tablespace
@param space_id         space identfier
@param zip_size         ROW_FORMAT=COMPRESSED page size or 0
@param mtr              mini-transaction
@param free_block       pre-allocated buffer block
@return pointer to the block, page bufferfixed */
buf_block_t*
buf_page_create_deferred(uint32_t space_id, ulint zip_size, mtr_t *mtr,
                         buf_block_t *free_block) noexcept;

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

/********************************************************************//**
Increments the modify clock of a frame by 1. The caller must (1) own the
buf_pool.mutex and block bufferfix count has to be zero, (2) or own an x-lock
on the block. */
UNIV_INLINE
void
buf_block_modify_clock_inc(
/*=======================*/
	buf_block_t*	block);	/*!< in: block */

/** Increment the pages_accessed count. */
void buf_inc_get(trx_t *trx) noexcept;

/** Increment the pages_accessed count. */
void buf_inc_get() noexcept;
#endif /* !UNIV_INNOCHECKSUM */

/** Check if a buffer is all zeroes.
@param[in]	buf	data to check
@return whether the buffer is all zeroes */
bool buf_is_zeroes(st_::span<const byte> buf) noexcept;

/** Reason why buf_page_is_corrupted() fails */
enum buf_page_is_corrupted_reason
{
  CORRUPTED_FUTURE_LSN= -1,
  NOT_CORRUPTED= 0,
  CORRUPTED_OTHER
};

/** Check if a page is corrupt.
@param check_lsn   whether FIL_PAGE_LSN should be checked
@param read_buf    database page
@param fsp_flags   contents of FIL_SPACE_FLAGS
@return whether the page is corrupted */
buf_page_is_corrupted_reason
buf_page_is_corrupted(bool check_lsn, const byte *read_buf, uint32_t fsp_flags)
  noexcept MY_ATTRIBUTE((warn_unused_result));

/** Read the key version from the page. In full crc32 format,
key version is stored at {0-3th} bytes. In other format, it is
stored in 26th position.
@param[in]	read_buf	database page
@param[in]	fsp_flags	tablespace flags
@return key version of the page. */
inline uint32_t buf_page_get_key_version(const byte* read_buf,
                                         uint32_t fsp_flags) noexcept
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
inline bool buf_page_is_compressed(const byte* read_buf, uint32_t fsp_flags) noexcept
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
inline uint buf_page_full_crc32_size(const byte *buf, bool *comp, bool *cr)
  noexcept
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
void buf_page_print(const byte* read_buf, ulint zip_size = 0) noexcept
	ATTRIBUTE_COLD __attribute__((nonnull));
/** Decompress a ROW_FORMAT=COMPRESSED block.
@param block   buffer page
@param check whether to verify the page checksum
@return true if successful */
bool buf_zip_decompress(buf_block_t *block, bool check) noexcept;

#ifdef UNIV_DEBUG
/** @return the number of latched pages in the buffer pool */
ulint buf_get_latched_pages_number() noexcept;
#endif /* UNIV_DEBUG */
/*********************************************************************//**
Prints info of the buffer i/o. */
void
buf_print_io(
/*=========*/
	FILE*	file);	/*!< in: file where to print */

/** Refresh the statistics used to print per-second averages. */
void buf_refresh_io_stats() noexcept;

/** Invalidate all pages in the buffer pool.
All pages must be in a replaceable state (not modified or latched). */
void buf_pool_invalidate() noexcept;

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
ATTRIBUTE_COLD void buf_page_monitor(const buf_page_t &bpage, bool read)
  noexcept;

/** Verify that post encryption checksum match with the calculated checksum.
This function should be called only if tablespace contains crypt data metadata.
@param page       page frame
@param fsp_flags  contents of FSP_SPACE_FLAGS
@return whether the page is encrypted and valid */
bool buf_page_verify_crypt_checksum(const byte *page, uint32_t fsp_flags) noexcept;

/** Calculate a ROW_FORMAT=COMPRESSED page checksum and update the page.
@param[in,out]	page		page to update
@param[in]	size		compressed page size */
void buf_flush_update_zip_checksum(buf_frame_t* page, ulint size) noexcept;

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
  void release() noexcept { reserved.store(false, std::memory_order_relaxed); }

  /** Acquire the slot
  @return whether the slot was acquired */
  bool acquire() noexcept
  { return !reserved.exchange(true, std::memory_order_relaxed);}

  /** Allocate a buffer for encryption, decryption or decompression. */
  void allocate() noexcept
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
  union {
    /** for in_file(): buf_pool.page_hash link;
    protected by buf_pool.page_hash.lock_get() */
    buf_page_t *hash;
    /** for state()==MEMORY that are part of recv_sys.pages and
    protected by recv_sys.mutex, or part of btr_sea::partition::table
    and protected by btr_sea::partition::blocks_mutex */
    struct {
      /** number of recv_sys.pages entries stored in the block */
      uint16_t used_records;
      /** the offset of the next free record */
      uint16_t free_offset;
    };
  };
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
  /* unused state: 1U<<29 */
  /** smallest state() for a block that belongs to buf_pool.LRU */
  static constexpr uint32_t UNFIXED= 2U << 29;
  /** smallest state() of a (re)initialized page (no doublewrite needed) */
  static constexpr uint32_t REINIT= 3U << 29;
  /** smallest state() for an io-fixed block */
  static constexpr uint32_t READ_FIX= 4U << 29;
  /* unused state: 5U<<29 */
  /** smallest state() for a write-fixed block */
  static constexpr uint32_t WRITE_FIX= 6U << 29;
  /** smallest state() for a write-fixed block (no doublewrite was used) */
  static constexpr uint32_t WRITE_FIX_REINIT= 7U << 29;
  /** buf_pool.LRU status mask in state() */
  static constexpr uint32_t LRU_MASK= 7U << 29;

  /** lock covering the contents of frame() */
  block_lock lock;
  /** pointer to aligned, uncompressed page frame of innodb_page_size */
  byte *frame;
  /* @} */
  /** ROW_FORMAT=COMPRESSED page; zip.data (but not the data it points to)
  is also protected by buf_pool.mutex */
  page_zip_des_t zip;
#ifdef UNIV_DEBUG
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

  state() == NOT_USED: buf_pool.free

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
					in the buffer pool. */
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
    in_LRU_list(b.in_LRU_list),
    in_page_hash(b.in_page_hash), in_free_list(b.in_free_list),
#endif /* UNIV_DEBUG */
    list(b.list), LRU(b.LRU), old(b.old), freed_page_clock(b.freed_page_clock),
    access_time(b.access_time)
  {
    lock.init();
  }

  /** Initialize some more fields */
  void init(uint32_t state, page_id_t id) noexcept
  {
    ut_ad(state < REMOVE_HASH || state >= UNFIXED);
    ut_ad(!lock.is_locked_or_waiting());
    id_= id;
    zip.fix.store(state, std::memory_order_release);
    oldest_modification_= 0;
    ut_d(in_free_list= false);
    ut_d(in_LRU_list= false);
    ut_d(in_page_hash= false);
    old= 0;
    freed_page_clock= 0;
    access_time= 0;
  }

  void set_os_unused() const
  {
    MEM_NOACCESS(frame, srv_page_size);
  }

  void set_os_used() const
  {
    MEM_MAKE_ADDRESSABLE(frame, srv_page_size);
  }
public:
  const page_id_t &id() const noexcept { return id_; }
  uint32_t state() const noexcept { return zip.fix; }
  static uint32_t buf_fix_count(uint32_t s) noexcept
  { ut_ad(s >= FREED); return s < UNFIXED ? (s - FREED) : (~LRU_MASK & s); }

  uint32_t buf_fix_count() const { return buf_fix_count(state()); }
  /** Check if a file block is io-fixed.
  @param s   state()
  @return whether s corresponds to an io-fixed block */
  static bool is_io_fixed(uint32_t s) noexcept
  { ut_ad(s >= FREED); return s >= READ_FIX; }
  /** Check if a file block is read-fixed.
  @param s   state()
  @return whether s corresponds to a read-fixed block */
  static bool is_read_fixed(uint32_t s) noexcept
  { return is_io_fixed(s) && s < WRITE_FIX; }
  /** Check if a file block is write-fixed.
  @param s   state()
  @return whether s corresponds to a write-fixed block */
  static bool is_write_fixed(uint32_t s) noexcept
  { ut_ad(s >= FREED); return s >= WRITE_FIX; }

  /** @return whether this block is read or write fixed;
  read_complete() or write_complete() will always release
  the io-fix before releasing U-lock or X-lock */
  bool is_io_fixed() const noexcept { return is_io_fixed(state()); }
  /** @return whether this block is write fixed;
  write_complete() will always release the write-fix before releasing U-lock */
  bool is_write_fixed() const noexcept { return is_write_fixed(state()); }
  /** @return whether this block is read fixed */
  bool is_read_fixed() const noexcept { return is_read_fixed(state()); }

  /** @return if this belongs to buf_pool.unzip_LRU */
  bool belongs_to_unzip_LRU() const noexcept
  { return UNIV_LIKELY_NULL(zip.data) && frame; }

  static bool is_freed(uint32_t s) noexcept
  { ut_ad(s >= FREED); return s < UNFIXED; }
  bool is_freed() const noexcept { return is_freed(state()); }
  bool is_reinit() const { return !(~state() & REINIT); }

  void set_reinit(uint32_t prev_state) noexcept
  {
    ut_ad(prev_state < READ_FIX);
    ut_d(const auto s=) zip.fix.fetch_add(REINIT - prev_state);
    ut_ad(s > prev_state);
    ut_ad(s < prev_state + UNFIXED);
  }

  uint32_t read_unfix(uint32_t s) noexcept
  {
    ut_ad(lock.is_write_locked());
    ut_ad(s == UNFIXED + 1 || s == REINIT + 1);
    uint32_t old_state= zip.fix.fetch_add(s - READ_FIX);
    ut_ad(old_state >= READ_FIX);
    ut_ad(old_state < WRITE_FIX);
    return old_state + (s - READ_FIX);
  }

  void set_freed(uint32_t prev_state, uint32_t count= 0) noexcept
  {
    ut_ad(lock.is_write_locked());
    ut_ad(prev_state >= UNFIXED);
    ut_ad(prev_state < READ_FIX);
    ut_d(auto s=) zip.fix.fetch_sub((prev_state & LRU_MASK) - FREED - count);
    ut_ad(!((prev_state ^ s) & LRU_MASK));
  }

  inline void set_state(uint32_t s) noexcept;
  inline void set_corrupt_id() noexcept;

  /** @return the log sequence number of the oldest pending modification
  @retval 0 if the block is being removed from (or not in) buf_pool.flush_list
  @retval 1 if the block is in buf_pool.flush_list but not modified
  @retval 2 if the block belongs to the temporary tablespace and
  has unwritten changes */
  lsn_t oldest_modification() const noexcept { return oldest_modification_; }
  /** @return the log sequence number of the oldest pending modification,
  @retval 0 if the block is definitely not in buf_pool.flush_list
  @retval 1 if the block is in buf_pool.flush_list but not modified
  @retval 2 if the block belongs to the temporary tablespace and
  has unwritten changes */
  lsn_t oldest_modification_acquire() const noexcept
  { return oldest_modification_.load(std::memory_order_acquire); }
  /** Set oldest_modification when adding to buf_pool.flush_list */
  inline void set_oldest_modification(lsn_t lsn) noexcept;
  /** Clear oldest_modification after removing from buf_pool.flush_list */
  inline void clear_oldest_modification() noexcept;
  /** Reset the oldest_modification when marking a persistent page freed */
  void reset_oldest_modification() noexcept
  {
    ut_ad(oldest_modification() > 2);
    oldest_modification_.store(1, std::memory_order_release);
  }

  /** Complete a read of a page.
  @param node     data file
  @return whether the operation succeeded
  @retval DB_PAGE_CORRUPTED    if the checksum or the page ID is incorrect
  @retval DB_DECRYPTION_FAILED if the page cannot be decrypted */
  dberr_t read_complete(const fil_node_t &node) noexcept;

  /** Release a write fix after a page write was completed.
  @param persistent  whether the page belongs to a persistent tablespace
  @param error       whether an error may have occurred while writing
  @param state       recently read state() value with the correct io-fix */
  void write_complete(bool persistent, bool error, uint32_t state) noexcept;

  /** Write a flushable page to a file or free a freeable block.
  @param space       tablespace
  @return whether a page write was initiated and buf_pool.mutex released */
  bool flush(fil_space_t *space) noexcept;

  /** Notify that a page in a temporary tablespace has been modified. */
  void set_temp_modified() noexcept
  {
    ut_ad(fsp_is_system_temporary(id().space()));
    ut_ad(in_file());
    ut_ad((oldest_modification() | 2) == 2);
    oldest_modification_= 2;
  }

  /** Prepare to release a file page to buf_pool.free. */
  void free_file_page() noexcept
  {
    assert((zip.fix.fetch_sub(REMOVE_HASH - MEMORY)) == REMOVE_HASH);
    /* buf_LRU_block_free_non_file_page() asserts !oldest_modification() */
    ut_d(oldest_modification_= 0;)
    id_= page_id_t(~0ULL);
  }

  void fix_on_recovery() noexcept
  {
    ut_d(const auto f=) zip.fix.fetch_sub(READ_FIX - UNFIXED - 1);
    ut_ad(f >= READ_FIX);
    ut_ad(f < WRITE_FIX);
  }

  uint32_t fix(uint32_t count= 1) noexcept
  {
    ut_ad(count);
    ut_ad(count < REINIT);
    uint32_t f= zip.fix.fetch_add(count);
    ut_ad(f >= FREED);
    ut_ad(!((f ^ (f + 1)) & LRU_MASK));
    return f;
  }

  uint32_t unfix() noexcept
  {
    uint32_t f= zip.fix.fetch_sub(1);
    ut_ad(f > FREED);
    ut_ad(!((f ^ (f - 1)) & LRU_MASK));
    return f - 1;
  }

  /** @return the physical size, in bytes */
  ulint physical_size() const noexcept
  {
    return zip.ssize ? (UNIV_ZIP_SIZE_MIN >> 1) << zip.ssize : srv_page_size;
  }

  /** @return the ROW_FORMAT=COMPRESSED physical size, in bytes
  @retval 0 if not compressed */
  ulint zip_size() const noexcept
  {
    return zip.ssize ? (UNIV_ZIP_SIZE_MIN >> 1) << zip.ssize : 0;
  }

  /** @return the byte offset of the page within a file */
  os_offset_t physical_offset() const noexcept
  {
    os_offset_t o= id().page_no();
    return zip.ssize
      ? o << (zip.ssize + (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
      : o << srv_page_size_shift;
  }

  /** @return whether the block is mapped to a data file */
  bool in_file() const noexcept { return state() >= FREED; }

  /** @return whether the block can be relocated in memory.
  The block can be dirty, but it must not be I/O-fixed or bufferfixed. */
  inline bool can_relocate() const noexcept;
  /** @return whether the block has been flagged old in buf_pool.LRU */
  inline bool is_old() const noexcept;
  /** Set whether a block is old in buf_pool.LRU */
  inline void set_old(bool old) noexcept;
  /** Flag a page accessed in buf_pool
  @return whether this is not the first access */
  bool set_accessed() noexcept
  {
    if (is_accessed()) return true;
    access_time= static_cast<uint32_t>(ut_time_ms());
    return false;
  }
  /** @return ut_time_ms() at the time of first access of a block in buf_pool
  @retval 0 if not accessed */
  unsigned is_accessed() const noexcept
  { ut_ad(in_file()); return access_time; }
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
  /** @name Hash search fields */
  /* @{ */
  /** flag: (true=first, false=last) identical-prefix key is included */
  static constexpr uint32_t LEFT_SIDE= 1U << 31;

  /** AHI parameters: LEFT_SIDE | prefix_bytes << 16 | prefix_fields.
  Protected by the btr_sea::partition::latch and
  (1) in_file(), and we are holding lock in any mode, or
  (2) !is_read_fixed()&&(state()>=UNFIXED||state()==REMOVE_HASH). */
  Atomic_relaxed<uint32_t> ahi_left_bytes_fields;

  /** counter which controls building of a new hash index for the page;
  may be nonzero even if !index */
  Atomic_relaxed<uint16_t> n_hash_helps;
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  /** number of pointers from the btr_sea::partition::table;
  !index implies n_pointers == 0 */
  Atomic_counter<uint16_t> n_pointers;
#  define assert_block_ahi_empty(block) ut_a(!(block)->n_pointers)
#  define assert_block_ahi_valid(b) ut_a((b)->index || !(b)->n_pointers)
# else /* UNIV_AHI_DEBUG || UNIV_DEBUG */
#  define assert_block_ahi_empty(block) /* nothing */
#  define assert_block_ahi_valid(block) /* nothing */
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  /** index for which the adaptive hash index has been created,
  or nullptr if the page does not exist in the index.
  May be modified while holding exclusive btr_sea::partition::latch. */
  Atomic_relaxed<dict_index_t*> index;
  /* @} */
#else /* BTR_CUR_HASH_ADAPT */
# define assert_block_ahi_empty(block) /* nothing */
# define assert_block_ahi_valid(block) /* nothing */
#endif /* BTR_CUR_HASH_ADAPT */
  void fix() noexcept { page.fix(); }
  uint32_t unfix() noexcept { return page.unfix(); }

  /** @return the physical size, in bytes */
  ulint physical_size() const noexcept { return page.physical_size(); }

  /** @return the ROW_FORMAT=COMPRESSED physical size, in bytes
  @retval 0 if not compressed */
  ulint zip_size() const noexcept { return page.zip_size(); }

  /** Initialize the block.
  @param page_id  page identifier
  @param zip_size ROW_FORMAT=COMPRESSED page size, or 0
  @param state    initial state() */
  void initialise(const page_id_t page_id, ulint zip_size, uint32_t state)
    noexcept;

  /** Calculate the page frame address */
  IF_DBUG(,inline) byte *frame_address() const noexcept;
};

/** A "Hazard Pointer" class used to iterate over buf_pool.LRU or
buf_pool.flush_list. A hazard pointer is a buf_page_t pointer
which we intend to iterate over next and we want it remain valid
even after we release the mutex that protects the list. */
class HazardPointer
{
public:
  virtual ~HazardPointer() = default;

  /** @return current value */
  buf_page_t *get() const noexcept
  { mysql_mutex_assert_owner(m_mutex); return m_hp; }

  /** Set current value
  @param bpage buffer block to be set as hp */
  void set(buf_page_t *bpage) noexcept
  {
    mysql_mutex_assert_owner(m_mutex);
    ut_ad(!bpage || bpage->in_file());
    m_hp= bpage;
  }

  /** Checks if a bpage is the hp
  @param bpage  buffer block to be compared
  @return true if it is hp */
  bool is_hp(const buf_page_t *bpage) const noexcept
  { mysql_mutex_assert_owner(m_mutex); return bpage == m_hp; }

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list. */
  virtual void adjust(const buf_page_t*) noexcept = 0;

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
  ~FlushHp() override = default;

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list.
  @param bpage  buffer block to be compared */
  MY_ATTRIBUTE((nonnull))
  void adjust(const buf_page_t *bpage) noexcept override
  {
    /* We only support reverse traversal for now. */
    if (is_hp(bpage))
      m_hp= UT_LIST_GET_PREV(list, m_hp);

    ut_ad(!m_hp || m_hp->oldest_modification());
  }
};

/** Class implementing buf_pool.LRU hazard pointer */
class LRUHp : public HazardPointer {
public:
  ~LRUHp() override = default;

  /** Adjust the value of hp. This happens when some
  other thread working on the same list attempts to
  remove the hp from the list.
  @param bpage  buffer block to be compared */
  MY_ATTRIBUTE((nonnull))
  void adjust(const buf_page_t *bpage) noexcept override
  {
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
  ~LRUItr() override = default;

  /** Select from where to start a scan. If we have scanned
  too deep into the LRU list it resets the value to the tail
  of the LRU list.
  @return buf_page_t from where to start scan. */
  inline buf_page_t *start() noexcept;
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

/** @brief The buffer pool statistics structure;
protected by buf_pool.mutex unless otherwise noted. */
struct buf_pool_stat_t{
	/** Initialize the counters */
	void init() noexcept { memset((void*) this, 0, sizeof *this); }

	buf_pool_stat_t& operator=(const buf_pool_stat_t& other) noexcept {
		memcpy(reinterpret_cast<void*>(this), &other, sizeof *this);
		return *this;
	}

	/** number of pages accessed; aggregates trx_t::pages_accessed */
	union {
		Atomic_counter<ulint> n_page_gets{0};
		ulint n_page_gets_nonatomic;
	};
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
	/** number of waits for eviction */
	ulint	LRU_waits;
	ulint	LRU_bytes;	/*!< LRU size in bytes */
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
  /** arrays of buf_block_t followed by page frames;
  aliged to and repeating every innodb_buffer_pool_extent_size;
  each extent comprises pages_in_extent[] blocks */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) char *memory;
  /** the allocation of the above memory, possibly including some
  alignment loss at the beginning */
  char *memory_unaligned;
  /** the virtual address range size of memory_unaligned */
  size_t size_unaligned;
#ifdef UNIV_PFS_MEMORY
  /** the "owner thread" of the buffer pool allocation */
  PSI_thread *owner;
#endif
  /** initialized number of block descriptors */
  size_t n_blocks;
  /** number of blocks that need to be freed in shrink() */
  size_t n_blocks_to_withdraw;
  /** first block to withdraw in shrink() */
  const buf_page_t *first_to_withdraw;

  /** amount of memory allocated to the buffer pool and descriptors;
  protected by mutex */
  Atomic_relaxed<size_t> size_in_bytes;

public:
  /** The requested innodb_buffer_pool_size */
  size_t size_in_bytes_requested;
#if defined __linux__ || !defined DBUG_OFF
  /** The minimum allowed innodb_buffer_pool_size in garbage_collect() */
  size_t size_in_bytes_auto_min;
#endif
  /** The maximum allowed innodb_buffer_pool_size */
  size_t size_in_bytes_max;

  /** @return the current size of the buffer pool, in bytes */
  size_t curr_pool_size() const noexcept { return size_in_bytes; }

  /** @return the current size of the buffer pool, in pages */
  TPOOL_SUPPRESS_TSAN size_t curr_size() const noexcept { return n_blocks; }
  /** @return the maximum usable size of the buffer pool, in pages */
  TPOOL_SUPPRESS_TSAN size_t usable_size() const noexcept
  { return n_blocks - n_blocks_to_withdraw - UT_LIST_GET_LEN(withdrawn); }

  /** Determine the used size of the buffer pool in bytes.
  @param n_blocks   size of the buffer pool in blocks
  @return the size needed for n_blocks in bytes, for innodb_page_size */
  static size_t blocks_in_bytes(size_t n_blocks) noexcept;

#if defined(DBUG_OFF) && defined(HAVE_MADVISE) && defined(MADV_DODUMP)
  /** Enable buffers to be dumped to core files.

  A convenience function, not called anyhwere directly however
  it is left available for gdb or any debugger to call
  in the event that you want all of the memory to be dumped
  to a core file.

  @return number of errors found in madvise() calls */
  static int madvise_do_dump() noexcept;
#endif

  /** Hash cell chain in page_hash_table */
  struct hash_chain
  {
    /** pointer to the first block */
    buf_page_t *first;
  };
private:
  /** Determine the number of blocks in a buffer pool of a particular size.
  @param size_in_bytes    innodb_buffer_pool_size in bytes
  @return number of buffer pool pages */
  static size_t get_n_blocks(size_t size_in_bytes) noexcept;

  /** The outcome of shrink() */
  enum shrink_status{SHRINK_DONE= -1, SHRINK_IN_PROGRESS= 0, SHRINK_ABORT};

  /** Attempt to shrink the buffer pool.
  @param size   requested innodb_buffer_pool_size in bytes
  @retval whether the shrinking was completed */
  ATTRIBUTE_COLD shrink_status shrink(size_t size) noexcept;

  /** Finish shrinking the buffer pool.
  @param size    the new innodb_buffer_pool_size in bytes
  @param reduced how much the innodb_buffer_pool_size was reduced */
  inline void shrunk(size_t size, size_t reduced) noexcept;

public:
  bool is_initialised() const noexcept { return memory != nullptr; }

  /** Create the buffer pool.
  @return whether the creation failed */
  bool create() noexcept;

  /** Clean up after successful create() */
  void close() noexcept;

  /** Resize the buffer pool.
  @param size   requested innodb_buffer_pool_size in bytes
  @param trx    current connnection */
  ATTRIBUTE_COLD void resize(size_t size, THD *thd) noexcept;

  /** Collect garbage (release pages from the LRU list) */
  inline void garbage_collect() noexcept;

  /** Determine whether a frame needs to be withdrawn during resize().
  @param ptr    pointer within a buf_page_t::frame
  @param size   size_in_bytes_requested
  @return whether the frame will be withdrawn */
  bool will_be_withdrawn(const byte *ptr, size_t size) const noexcept
  {
    const char *p= reinterpret_cast<const char*>(ptr);
    ut_ad(!p || p >= memory);
    ut_ad(p < memory + size_in_bytes_max);
    return p >= memory + size;
  }

  /** Withdraw a block if needed in case resize() is shrinking.
  @param bpage  buffer pool block
  @return whether the block was withdrawn */
  ATTRIBUTE_COLD bool withdraw(buf_page_t &bpage) noexcept;

  /** Release and evict a corrupted page.
  @param bpage    x-latched page that was found corrupted
  @param state    expected current state of the page */
  ATTRIBUTE_COLD void corrupted_evict(buf_page_t *bpage, uint32_t state)
    noexcept;

  /** Release a memory block to the buffer pool. */
  ATTRIBUTE_COLD void free_block(buf_block_t *block) noexcept;

#ifdef UNIV_DEBUG
  /** Find a block that points to a ROW_FORMAT=COMPRESSED page
  @param data  pointer to the start of a ROW_FORMAT=COMPRESSED page frame
  @param shift number of least significant address bits to ignore
  @return the block
  @retval nullptr  if not found */
  const buf_block_t *contains_zip(const void *data, size_t shift= 0)
    const noexcept;
  /** Assert that all buffer pool pages are in a replaceable state */
  void assert_all_freed() noexcept;
#endif /* UNIV_DEBUG */

#ifdef BTR_CUR_HASH_ADAPT
  /** Clear the adaptive hash index on all pages in the buffer pool. */
  void clear_hash_index() noexcept;
#endif /* BTR_CUR_HASH_ADAPT */

  /**
  @return the smallest oldest_modification lsn for any page
  @retval empty_lsn if all modified persistent pages have been flushed */
  lsn_t get_oldest_modification(lsn_t empty_lsn) noexcept
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

  /** Look up the block descriptor for a page frame address.
  @param ptr   address within a valid page frame
  @return the corresponding block descriptor */
  static buf_block_t *block_from(const void *ptr) noexcept;

  /** Access a block while holding the buffer pool mutex.
  @param pos    position between 0 and get_n_pages()
  @return the block descriptor */
  buf_block_t *get_nth_page(size_t pos) const noexcept;

#ifdef UNIV_DEBUG
  /** Determine if an object is within the curr_pool_size()
  and associated with an uncompressed page.
  @param ptr   memory object (not dereferenced)
  @return whether the object is valid in the current buffer pool */
  bool is_uncompressed_current(const void *ptr) const noexcept
  {
    const ptrdiff_t d= static_cast<const char*>(ptr) - memory;
    return d >= 0 && size_t(d) < curr_pool_size();
  }
#endif

public:
  /** page_fix() mode of operation */
  enum page_fix_conflicts{
    /** Fetch if in the buffer pool, also blocks marked as free */
    FIX_ALSO_FREED= -1,
    /** Fetch, waiting for page read completion */
    FIX_WAIT_READ,
    /** Fetch, but avoid any waits for */
    FIX_NOWAIT
  };

  /** Look up and buffer-fix a page.
  Note: If the page is read-fixed (being read into the buffer pool),
  we would have to wait for the page latch before determining if the page
  is accessible (it could be corrupted and have been evicted again).
  If the caller is holding other page latches so that waiting for this
  page latch could lead to lock order inversion (latching order violation),
  the mode c=FIX_WAIT_READ must not be used.
  @param id        page identifier
  @param err       error code (will only be assigned when returning nullptr)
  @param trx       transaction attached to current connection
  @param c         how to handle conflicts
  @return undo log page, buffer-fixed
  @retval -1       if c=FIX_NOWAIT and buffer-fixing would require waiting
  @retval nullptr  if the undo page was corrupted or freed */
  buf_block_t *page_fix(const page_id_t id, dberr_t *err, trx_t *trx,
                        page_fix_conflicts c) noexcept;

  buf_block_t *page_fix(const page_id_t id, trx_t *trx) noexcept
  { return page_fix(id, nullptr, trx, FIX_WAIT_READ); }

  /** Validate a block descriptor.
  @param b     block descriptor that may be invalid after shrink()
  @param latch page_hash latch for id
  @param id    page identifier
  @return b->page.fix() if b->page.id() == id
  @retval 0 if b is invalid */
  uint32_t page_guess(buf_block_t *b, page_hash_latch &latch,
                      const page_id_t id) noexcept;

  /** Decompress a page and relocate the block descriptor
  @param b      buffer-fixed compressed-only ROW_FORMAT=COMPRESSED page
  @param chain  hash table chain for b->id().fold()
  @return the decompressed block, x-latched and read-fixed
  @retval nullptr if the decompression failed (b->unfix() will be invoked) */
  ATTRIBUTE_COLD __attribute__((nonnull, warn_unused_result))
  buf_block_t *unzip(buf_page_t *b, hash_chain &chain) noexcept;

  /** @return whether the buffer pool contains a page
  @param page_id       page identifier
  @param chain         hash table chain for page_id.fold() */
  TRANSACTIONAL_TARGET
  bool page_hash_contains(const page_id_t page_id, hash_chain &chain) noexcept;

  /** @return whether less than 1/4 of the buffer pool is available */
  bool running_out() const noexcept;

  /** @return whether the buffer pool is running low */
  bool need_LRU_eviction() const noexcept;

  /** @return number of blocks resize() needs to evict from the buffer pool */
  size_t is_shrinking() const noexcept
  {
    mysql_mutex_assert_owner(&mutex);
    return n_blocks_to_withdraw + UT_LIST_GET_LEN(withdrawn);
  }

  /** @return number of blocks in resize() waiting to be withdrawn */
  size_t to_withdraw() const noexcept
  {
    mysql_mutex_assert_owner(&mutex);
    return n_blocks_to_withdraw;
  }

  /** @return the shrinking size of the buffer pool, in bytes
  @retval 0 if resize() is not shrinking the buffer pool */
  size_t shrinking_size() const noexcept
  { return is_shrinking() ? size_in_bytes_requested : 0; }

#ifdef UNIV_DEBUG
  /** Validate the buffer pool. */
  void validate() noexcept;
#endif /* UNIV_DEBUG */
#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG
  /** Write information of the buf_pool to the error log. */
  void print() noexcept;
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG */

  /** Remove a block from the LRU list.
  @return the predecessor in the LRU list */
  buf_page_t *LRU_remove(buf_page_t *bpage) noexcept
  {
    mysql_mutex_assert_owner(&mutex);
    ut_ad(bpage->in_LRU_list);
    ut_ad(bpage->in_page_hash);
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

  /** current statistics; protected by mutex */
  buf_pool_stat_t stat;
  /** old statistics; protected by mutex */
  buf_pool_stat_t old_stat;

	/** @name General fields */
	/* @{ */
	ulint		LRU_old_ratio;  /*!< Reserve this much of the buffer
					pool for "old" blocks */
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
    void create(ulint n) noexcept;

    /** Free the hash table. */
    void free() noexcept { aligned_free(array); array= nullptr; }

    /** @return the index of an array element */
    ulint calc_hash(ulint fold) const noexcept
    { return calc_hash(fold, n_cells); }
    /** @return raw array index converted to padded index */
    static ulint pad(ulint h) noexcept
    {
      ulint latches= h / ELEMENTS_PER_LATCH;
      ulint empty_slots= latches * EMPTY_SLOTS_PER_LATCH;
      return 1 + latches + empty_slots + h;
    }
  private:
    /** @return the index of an array element */
    static ulint calc_hash(ulint fold, ulint n_cells) noexcept
    {
      return pad(fold % n_cells);
    }
  public:
    /** @return the latch covering a hash table chain */
    static page_hash_latch &lock_get(hash_chain &chain) noexcept
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
    void append(hash_chain &chain, buf_page_t *bpage) noexcept;

    /** Remove a block descriptor from a hash bucket chain. */
    inline void remove(hash_chain &chain, buf_page_t *bpage) noexcept;
    /** Replace a block descriptor with another. */
    inline void replace(hash_chain &chain, buf_page_t *old, buf_page_t *bpage)
      noexcept;

    /** Look up a page in a hash bucket chain. */
    inline buf_page_t *get(const page_id_t id, const hash_chain &chain) const
      noexcept;
  };

  /** Buffer pool mutex */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t mutex;

  /** innodb_lru_scan_depth; number of blocks scanned in LRU flush batch;
  protected by buf_pool_t::mutex */
  ulong LRU_scan_depth;
  /** innodb_flush_neighbors; whether or not to flush neighbors of a block;
  protected by buf_pool_t::mutex */
  ulong flush_neighbors;

  /** Hash table of file pages (buf_page_t::in_file() holds),
  indexed by page_id_t. Protected by both mutex and page_hash.lock_get(). */
  page_hash_table page_hash;

  /** number of pending unzip() */
  Atomic_counter<ulint> n_pend_unzip;

	time_t		last_printout_time;
					/*!< when buf_print_io was last time
					called */
	buf_buddy_stat_t buddy_stat[BUF_BUDDY_SIZES_MAX + 1];
					/*!< Statistics of buddy system,
					indexed by block size */

	/* @} */

  /** number of index page splits */
  Atomic_counter<ulint> pages_split;

  /** @name Page flushing algorithm fields */
  /* @{ */

  /** mutex protecting flush_list, buf_page_t::set_oldest_modification()
  and buf_page_t::list pointers when !oldest_modification() */
  alignas(CPU_LEVEL1_DCACHE_LINESIZE) mysql_mutex_t flush_list_mutex;
  /** "hazard pointer" for flush_list scans; protected by flush_list_mutex */
  FlushHp flush_hp;
  /** flush_list size in bytes; protected by flush_list_mutex */
  ulint flush_list_bytes;
  /** possibly modified persistent pages (a subset of LRU);
  os_aio_pending_writes() is approximately COUNT(is_write_fixed()) */
  UT_LIST_BASE_NODE_T(buf_page_t) flush_list;
  /** number of blocks ever added to flush_list;
  sometimes protected by flush_list_mutex */
  size_t flush_list_requests;

  TPOOL_SUPPRESS_TSAN void add_flush_list_requests(size_t size)
  { flush_list_requests+= size; }
private:
  static constexpr unsigned PAGE_CLEANER_IDLE= 1;
  static constexpr unsigned FLUSH_LIST_ACTIVE= 2;
  static constexpr unsigned LRU_FLUSH= 4;

  /** Number of pending LRU flush * LRU_FLUSH +
  PAGE_CLEANER_IDLE + FLUSH_LIST_ACTIVE flags */
  unsigned page_cleaner_status;
  /** track server activity count for signaling idle flushing */
  ulint last_activity_count;
public:
  /** signalled to wake up the page_cleaner; protected by flush_list_mutex */
  pthread_cond_t do_flush_list;
  /** broadcast when !n_flush(); protected by flush_list_mutex */
  pthread_cond_t done_flush_LRU;
  /** broadcast when a batch completes; protected by flush_list_mutex */
  pthread_cond_t done_flush_list;

  /** @return number of pending LRU flush */
  unsigned n_flush() const noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    return page_cleaner_status / LRU_FLUSH;
  }

  /** Increment the number of pending LRU flush */
  inline void n_flush_inc() noexcept;

  /** Decrement the number of pending LRU flush */
  inline void n_flush_dec() noexcept;

  /** @return whether flush_list flushing is active */
  bool flush_list_active() const noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    return page_cleaner_status & FLUSH_LIST_ACTIVE;
  }

  void flush_list_set_active() noexcept
  {
    ut_ad(!flush_list_active());
    page_cleaner_status+= FLUSH_LIST_ACTIVE;
  }
  void flush_list_set_inactive() noexcept
  {
    ut_ad(flush_list_active());
    page_cleaner_status-= FLUSH_LIST_ACTIVE;
  }

  /** @return whether the page cleaner must sleep due to being idle */
  bool page_cleaner_idle() const noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    return page_cleaner_status & PAGE_CLEANER_IDLE;
  }

  /** @return whether the page cleaner may be initiating writes */
  bool page_cleaner_active() const noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    static_assert(PAGE_CLEANER_IDLE == 1, "efficiency");
    return page_cleaner_status > PAGE_CLEANER_IDLE;
  }

  /** Wake up the page cleaner if needed.
  @param for_LRU  whether to wake up for LRU eviction */
  void page_cleaner_wakeup(bool for_LRU= false) noexcept;

  /** Register whether an explicit wakeup of the page cleaner is needed */
  void page_cleaner_set_idle(bool deep_sleep) noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    page_cleaner_status= (page_cleaner_status & ~PAGE_CLEANER_IDLE) |
      (PAGE_CLEANER_IDLE * deep_sleep);
  }

  /** Update server last activity count */
  void update_last_activity_count(ulint activity_count) noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    last_activity_count= activity_count;
  }

	unsigned	freed_page_clock;/*!< a sequence number used
					to count the number of buffer
					blocks removed from the end of
					the LRU list; NOTE that this
					counter may wrap around at 4
					billion! A thread is allowed
					to read this for heuristic
					purposes without holding any
					mutex or latch */
  /** Cleared when buf_LRU_get_free_block() fails.
  Set whenever the free list grows, along with a broadcast of done_free.
  Protected by buf_pool.mutex. */
  Atomic_relaxed<bool> try_LRU_scan;

	/* @} */

	/** @name LRU replacement algorithm fields */
	/* @{ */

private:
  /** Whether we have warned to be running out of buffer pool;
  only modified by buf_flush_page_cleaner():
  set while holding mutex, cleared while holding flush_list_mutex */
  Atomic_relaxed<bool> LRU_warned;

  /** withdrawn blocks during resize() */
  UT_LIST_BASE_NODE_T(buf_page_t) withdrawn;

public:
  /** list of blocks available for allocate() */
  UT_LIST_BASE_NODE_T(buf_page_t) free;

  /** broadcast each time when the free list grows or try_LRU_scan is set;
  protected by mutex */
  pthread_cond_t done_free;

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

  /** Clear LRU_warned */
  void LRU_warned_clear() noexcept
  {
    mysql_mutex_assert_owner(&flush_list_mutex);
    LRU_warned= false;
  }

  /** Reserve a buffer. */
  buf_tmp_buffer_t *io_buf_reserve(bool wait_for_reads) noexcept
  { return io_buf.reserve(wait_for_reads); }

  /** Try to allocate a block.
  @return a buffer block
  @retval nullptr if no blocks are available */
  buf_block_t *allocate() noexcept;
  /** Remove a block from flush_list.
  @param bpage   buffer pool page */
  void delete_from_flush_list(buf_page_t *bpage) noexcept;

  /** Prepare to insert a modified blcok into flush_list.
  @param lsn start LSN of the mini-transaction
  @return insert position for insert_into_flush_list() */
  inline buf_page_t *prepare_insert_into_flush_list(lsn_t lsn) noexcept;

  /** Insert a modified block into the flush list.
  @param prev     insert position (from prepare_insert_into_flush_list())
  @param block    modified block
  @param lsn      start LSN of the mini-transaction that modified the block */
  inline void insert_into_flush_list(buf_page_t *prev, buf_block_t *block,
                                     lsn_t lsn) noexcept;

  /** Free a page whose underlying file page has been freed. */
  ATTRIBUTE_COLD void release_freed_page(buf_page_t *bpage) noexcept;

  /** Issue a warning that we could not free up buffer pool pages. */
  ATTRIBUTE_COLD void LRU_warn() noexcept;

  /** Print buffer pool flush state information. */
  ATTRIBUTE_COLD void print_flush_info() const noexcept;

  /** Collect buffer pool metadata.
  @param pool_info    buffer pool metadata */
  void get_info(buf_pool_info_t *pool_info) noexcept;

private:
  /** Temporary memory for page_compressed and encrypted I/O */
  struct io_buf_t
  {
    /** number of elements in slots[] */
    ulint n_slots;
    /** array of slots */
    buf_tmp_buffer_t *slots;

    void create(ulint n_slots) noexcept;

    void close() noexcept;

    /** Reserve a buffer */
    buf_tmp_buffer_t *reserve(bool wait_for_reads) noexcept;
  } io_buf;
};

/** The InnoDB buffer pool */
extern buf_pool_t buf_pool;

inline buf_page_t *buf_pool_t::page_hash_table::get(const page_id_t id,
                                                    const hash_chain &chain)
  const noexcept
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
inline void page_hash_latch::lock_shared() noexcept
{
  mysql_mutex_assert_not_owner(&buf_pool.mutex);
  if (!read_trylock())
    read_lock_wait();
}

inline void page_hash_latch::lock() noexcept
{
  if (!write_trylock())
    write_lock_wait();
}
#endif /* SUX_LOCK_GENERIC */

inline void buf_page_t::set_state(uint32_t s) noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(s <= REMOVE_HASH || s >= UNFIXED);
  ut_ad(s < WRITE_FIX);
  ut_ad(s <= READ_FIX || zip.fix == READ_FIX);
  zip.fix= s;
}

inline void buf_page_t::set_corrupt_id() noexcept
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
  id_.set_corrupted();
}

/** Set oldest_modification when adding to buf_pool.flush_list */
inline void buf_page_t::set_oldest_modification(lsn_t lsn) noexcept
{
  mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
  ut_ad(oldest_modification() <= 1);
  ut_ad(lsn > 2);
  oldest_modification_= lsn;
}

/** Clear oldest_modification after removing from buf_pool.flush_list */
inline void buf_page_t::clear_oldest_modification() noexcept
{
#ifdef SAFE_MUTEX
  if (oldest_modification() != 2)
    mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
#endif /* SAFE_MUTEX */
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

/** @return whether the block can be relocated in memory.
The block can be dirty, but it must not be I/O-fixed or bufferfixed. */
inline bool buf_page_t::can_relocate() const noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  const auto f= state();
  ut_ad(f >= FREED);
  ut_ad(in_LRU_list);
  return (f == FREED || (f < READ_FIX && !(f & ~LRU_MASK))) &&
    !lock.is_locked_or_waiting();
}

/** @return whether the block has been flagged old in buf_pool.LRU */
inline bool buf_page_t::is_old() const noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(in_file());
  ut_ad(in_LRU_list);
  return old;
}

/** Set whether a block is old in buf_pool.LRU */
inline void buf_page_t::set_old(bool old) noexcept
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
inline buf_page_t *LRUItr::start() noexcept
{
  mysql_mutex_assert_owner(m_mutex);

  if (!m_hp || m_hp->old)
    m_hp= UT_LIST_GET_LAST(buf_pool.LRU);

  return m_hp;
}

#ifdef UNIV_DEBUG
/** Functor to validate the LRU list. */
struct	CheckInLRUList {
	void	operator()(const buf_page_t* elem) const noexcept
	{
		ut_a(elem->in_LRU_list);
	}

	static void validate() noexcept
	{
		ut_list_validate(buf_pool.LRU, CheckInLRUList());
	}
};

/** Functor to validate the LRU list. */
struct	CheckInFreeList {
	void	operator()(const buf_page_t* elem) const noexcept
	{
		ut_a(elem->in_free_list);
	}

	static void validate() noexcept
	{
		ut_list_validate(buf_pool.free, CheckInFreeList());
	}
};

struct	CheckUnzipLRUAndLRUList {
	void	operator()(const buf_block_t* elem) const noexcept
	{
                ut_a(elem->page.in_LRU_list);
                ut_a(elem->in_unzip_LRU_list);
	}

	static void validate() noexcept
	{
		ut_list_validate(buf_pool.unzip_LRU,
				 CheckUnzipLRUAndLRUList());
	}
};
#endif /* UNIV_DEBUG */

#include "buf0buf.inl"

#endif /* !UNIV_INNOCHECKSUM */
