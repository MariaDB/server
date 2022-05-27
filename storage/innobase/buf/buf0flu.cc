/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.
Copyright (c) 2013, 2014, Fusion-io

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
@file buf/buf0flu.cc
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <my_service_manager.h>
#include <mysql/service_thd_wait.h>
#include <sql_class.h>

#include "buf0flu.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "buf0dblwr.h"
#include "srv0start.h"
#include "page0zip.h"
#include "fil0fil.h"
#include "log0crypt.h"
#include "srv0mon.h"
#include "fil0pagecompress.h"
#include "lzo/lzo1x.h"
#include "snappy-c.h"

/** Number of pages flushed via LRU. Protected by buf_pool.mutex.
Also included in buf_flush_page_count. */
ulint buf_lru_flush_page_count;

/** Number of pages freed without flushing. Protected by buf_pool.mutex. */
ulint buf_lru_freed_page_count;

/** Number of pages flushed. Protected by buf_pool.mutex. */
ulint buf_flush_page_count;

/** Flag indicating if the page_cleaner is in active state. */
Atomic_relaxed<bool> buf_page_cleaner_is_active;

/** Factor for scan length to determine n_pages for intended oldest LSN
progress */
static constexpr ulint buf_flush_lsn_scan_factor = 3;

/** Average redo generation rate */
static lsn_t lsn_avg_rate = 0;

/** Target oldest_modification for the page cleaner background flushing;
writes are protected by buf_pool.flush_list_mutex */
static Atomic_relaxed<lsn_t> buf_flush_async_lsn;
/** Target oldest_modification for the page cleaner furious flushing;
writes are protected by buf_pool.flush_list_mutex */
static Atomic_relaxed<lsn_t> buf_flush_sync_lsn;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t page_cleaner_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Page cleaner structure */
static struct
{
  /** total elapsed time in adaptive flushing, in seconds */
  ulint flush_time;
  /** number of adaptive flushing passes */
  ulint flush_pass;
} page_cleaner;

/* @} */

#ifdef UNIV_DEBUG
/** Validate the flush list. */
static void buf_flush_validate_low();

/** Validates the flush list some of the time. */
static void buf_flush_validate_skip()
{
/** Try buf_flush_validate_low() every this many times */
# define BUF_FLUSH_VALIDATE_SKIP	23

	/** The buf_flush_validate_low() call skip counter.
	Use a signed type because of the race condition below. */
	static int buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly buf_flush_validate_low()
	check in debug builds. */
	if (--buf_flush_validate_count > 0) {
		return;
	}

	buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;
	buf_flush_validate_low();
}
#endif /* UNIV_DEBUG */

/** Wake up the page cleaner if needed */
void buf_pool_t::page_cleaner_wakeup()
{
  ut_d(buf_flush_validate_skip());
  if (!page_cleaner_idle())
    return;
  double dirty_pct= double(UT_LIST_GET_LEN(buf_pool.flush_list)) * 100.0 /
    double(UT_LIST_GET_LEN(buf_pool.LRU) + UT_LIST_GET_LEN(buf_pool.free));
  double pct_lwm= srv_max_dirty_pages_pct_lwm;

  /* if pct_lwm != 0.0, adaptive flushing is enabled.
  signal buf page cleaner thread
  - if pct_lwm <= dirty_pct then it will invoke apdative flushing flow
  - if pct_lwm > dirty_pct then it will invoke idle flushing flow.

  idle_flushing:
  dirty_pct < innodb_max_dirty_pages_pct_lwm so it could be an
  idle flushing use-case.

  Why is last_activity_count not updated always?
  - let's first understand when is server activity count updated.
  - it is updated on commit of a transaction trx_t::commit() and not
    on adding a page to the flush list.
  - page_cleaner_wakeup is called when a page is added to the flush list.

  - now let's say the first user thread, updates the count from X -> Y but
    is yet to commit the transaction (so activity count is still Y).
    followup user threads will see the updated count as (Y) that is matching
    the universal server activity count (Y), giving a false impression that
    the server is idle.

  How to avoid this?
  - by allowing last_activity_count to updated when page-cleaner is made
    active and has work to do. This ensures that the last_activity signal
    is consumed by the page-cleaner before the next one is generated. */
  if ((pct_lwm != 0.0 && pct_lwm <= dirty_pct) ||
      (pct_lwm != 0.0 && last_activity_count == srv_get_activity_count()) ||
      srv_max_buf_pool_modified_pct <= dirty_pct)
  {
    page_cleaner_is_idle= false;
    pthread_cond_signal(&do_flush_list);
  }
}

inline void buf_pool_t::delete_from_flush_list_low(buf_page_t *bpage) noexcept
{
  ut_ad(!fsp_is_system_temporary(bpage->id().space()));
  mysql_mutex_assert_owner(&flush_list_mutex);
  flush_hp.adjust(bpage);
  UT_LIST_REMOVE(flush_list, bpage);
}

/** Insert a modified block into the flush list.
@param block    modified block
@param lsn      start LSN of the mini-transaction that modified the block */
void buf_pool_t::insert_into_flush_list(buf_block_t *block, lsn_t lsn) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(recv_recovery_is_on() || log_sys.latch.is_locked());
#endif
  ut_ad(lsn > 2);
  static_assert(log_t::FIRST_LSN >= 2, "compatibility");
  ut_ad(!fsp_is_system_temporary(block->page.id().space()));

  mysql_mutex_lock(&flush_list_mutex);
  if (ut_d(const lsn_t old=) block->page.oldest_modification())
  {
    ut_ad(old == 1);
    delete_from_flush_list_low(&block->page);
  }
  else
    stat.flush_list_bytes+= block->physical_size();
  ut_ad(stat.flush_list_bytes <= curr_pool_size);
  ut_ad(lsn >= log_sys.last_checkpoint_lsn);

  block->page.set_oldest_modification(lsn);
  MEM_CHECK_DEFINED(block->page.zip.data
                    ? block->page.zip.data : block->page.frame,
                    block->physical_size());
rescan:
  if (buf_page_t *prev= UT_LIST_GET_FIRST(flush_list))
  {
    lsn_t om= prev->oldest_modification();
    if (om == 1)
    {
      delete_from_flush_list(prev);
      goto rescan;
    }
    ut_ad(om > 2);
    if (om <= lsn)
      goto insert_first;
    while (buf_page_t *next= UT_LIST_GET_NEXT(list, prev))
    {
      om= next->oldest_modification();
      if (om == 1)
      {
        delete_from_flush_list(next);
        continue;
      }
      ut_ad(om > 2);
      if (om <= lsn)
        break;
      else
        prev= next;
    }
    flush_hp.adjust(prev);
    UT_LIST_INSERT_AFTER(flush_list, prev, &block->page);
  }
  else
  insert_first:
    UT_LIST_ADD_FIRST(flush_list, &block->page);
  mysql_mutex_unlock(&flush_list_mutex);
}

/** Remove a block from flush_list.
@param bpage   buffer pool page
@param clear   whether to invoke buf_page_t::clear_oldest_modification() */
void buf_pool_t::delete_from_flush_list(buf_page_t *bpage, bool clear) noexcept
{
  delete_from_flush_list_low(bpage);
  stat.flush_list_bytes-= bpage->physical_size();
  if (clear)
    bpage->clear_oldest_modification();
#ifdef UNIV_DEBUG
  buf_flush_validate_skip();
#endif /* UNIV_DEBUG */
}

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param id    tablespace identifier */
void buf_flush_remove_pages(uint32_t id)
{
  const page_id_t first(id, 0), end(id + 1, 0);
  ut_ad(id);
  mysql_mutex_lock(&buf_pool.mutex);

  for (;;)
  {
    bool deferred= false;

    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
    {
      const auto s= bpage->state();
      ut_ad(s >= buf_page_t::REMOVE_HASH);
      ut_ad(s < buf_page_t::READ_FIX || s >= buf_page_t::WRITE_FIX);
      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      const page_id_t bpage_id(bpage->id());

      if (bpage_id < first || bpage_id >= end);
      else if (s >= buf_page_t::WRITE_FIX)
        deferred= true;
      else
        buf_pool.delete_from_flush_list(bpage);

      bpage= prev;
    }

    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (!deferred)
      break;

    mysql_mutex_unlock(&buf_pool.mutex);
    std::this_thread::yield();
    mysql_mutex_lock(&buf_pool.mutex);
    buf_flush_wait_batch_end(false);
  }

  mysql_mutex_unlock(&buf_pool.mutex);
}

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage have already been
copied to dpage.
IMPORTANT: When this function is called bpage and dpage are not
exact copies of each other. For example, they both will have different
::state. Also the ::list pointers in dpage may be stale. We need to
use the current list node (bpage) to do the list manipulation because
the list pointers could have changed between the time that we copied
the contents of bpage to the dpage and the flush list manipulation
below. */
ATTRIBUTE_COLD
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage)	/*!< in/out: destination block */
{
	buf_page_t*	prev;

	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
	ut_ad(!fsp_is_system_temporary(bpage->id().space()));

	const lsn_t lsn = bpage->oldest_modification();

	if (!lsn) {
		return;
	}

	ut_ad(lsn == 1 || lsn > 2);
	ut_ad(dpage->oldest_modification() == lsn);

	/* Important that we adjust the hazard pointer before removing
	the bpage from the flush list. */
	buf_pool.flush_hp.adjust(bpage);

	prev = UT_LIST_GET_PREV(list, bpage);
	UT_LIST_REMOVE(buf_pool.flush_list, bpage);

	bpage->clear_oldest_modification();

	if (lsn == 1) {
		buf_pool.stat.flush_list_bytes -= dpage->physical_size();
		dpage->list.prev = nullptr;
		dpage->list.next = nullptr;
		dpage->clear_oldest_modification();
	} else if (prev) {
		ut_ad(prev->oldest_modification());
		UT_LIST_INSERT_AFTER(buf_pool.flush_list, prev, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.flush_list, dpage);
	}

	ut_d(buf_flush_validate_low());
}

/** Note that a block is no longer dirty, while not removing
it from buf_pool.flush_list */
inline void buf_page_t::write_complete(bool temporary)
{
  ut_ad(temporary == fsp_is_system_temporary(id().space()));
  if (temporary)
  {
    ut_ad(oldest_modification() == 2);
    oldest_modification_= 0;
  }
  else
  {
    /* We use release memory order to guarantee that callers of
    oldest_modification_acquire() will observe the block as
    being detached from buf_pool.flush_list, after reading the value 0. */
    ut_ad(oldest_modification() > 2);
    oldest_modification_.store(1, std::memory_order_release);
  }
  const auto s= state();
  ut_ad(s >= WRITE_FIX);
  zip.fix.fetch_sub((s >= WRITE_FIX_REINIT)
                    ? (WRITE_FIX_REINIT - UNFIXED)
                    : (WRITE_FIX - UNFIXED));
  lock.u_unlock(true);
}

/** Complete write of a file page from buf_pool.
@param request write request */
void buf_page_write_complete(const IORequest &request)
{
  ut_ad(request.is_write());
  ut_ad(!srv_read_only_mode);
  buf_page_t *bpage= request.bpage;
  ut_ad(bpage);
  const auto state= bpage->state();
  /* io-fix can only be cleared by buf_page_t::write_complete()
  and buf_page_t::read_complete() */
  ut_ad(state >= buf_page_t::WRITE_FIX);
  ut_ad(!buf_dblwr.is_inside(bpage->id()));
  ut_ad(request.node->space->id == bpage->id().space());

  if (state < buf_page_t::WRITE_FIX_REINIT &&
      request.node->space->use_doublewrite())
  {
    ut_ad(request.node->space != fil_system.temp_space);
    buf_dblwr.write_completed();
  }

  if (request.slot)
    request.slot->release();

  if (UNIV_UNLIKELY(MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)))
    buf_page_monitor(*bpage, false);
  DBUG_PRINT("ib_buf", ("write page %u:%u",
                        bpage->id().space(), bpage->id().page_no()));
  const bool temp= fsp_is_system_temporary(bpage->id().space());

  mysql_mutex_lock(&buf_pool.mutex);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  buf_pool.stat.n_pages_written++;
  bpage->write_complete(temp);

  if (request.is_LRU())
  {
    buf_LRU_free_page(bpage, true);

    ut_ad(buf_pool.n_flush_LRU_);
    if (!--buf_pool.n_flush_LRU_)
    {
      pthread_cond_broadcast(&buf_pool.done_flush_LRU);
      pthread_cond_signal(&buf_pool.done_free);
    }
  }
  else
  {
    ut_ad(!temp);
    ut_ad(buf_pool.n_flush_list_);
    if (!--buf_pool.n_flush_list_)
      pthread_cond_broadcast(&buf_pool.done_flush_list);
  }

  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Calculate a ROW_FORMAT=COMPRESSED page checksum and update the page.
@param[in,out]	page		page to update
@param[in]	size		compressed page size */
void buf_flush_update_zip_checksum(buf_frame_t *page, ulint size)
{
  ut_ad(size > 0);
  mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
                  page_zip_calc_checksum(page, size, false));
}

/** Assign the full crc32 checksum for non-compressed page.
@param[in,out]	page	page to be updated */
void buf_flush_assign_full_crc32_checksum(byte* page)
{
	ut_d(bool compressed = false);
	ut_d(bool corrupted = false);
	ut_d(const uint size = buf_page_full_crc32_size(page, &compressed,
							&corrupted));
	ut_ad(!compressed);
	ut_ad(!corrupted);
	ut_ad(size == uint(srv_page_size));
	const ulint payload = srv_page_size - FIL_PAGE_FCRC32_CHECKSUM;
	mach_write_to_4(page + payload, my_crc32c(0, page, payload));
}

/** Initialize a page for writing to the tablespace.
@param[in]	block			buffer block; NULL if bypassing
					the buffer pool
@param[in,out]	page			page frame
@param[in,out]	page_zip_		compressed page, or NULL if
					uncompressed
@param[in]	use_full_checksum	whether tablespace uses full checksum */
void
buf_flush_init_for_writing(
	const buf_block_t*	block,
	byte*			page,
	void*			page_zip_,
	bool			use_full_checksum)
{
	if (block && block->page.frame != page) {
		/* If page is encrypted in full crc32 format then
		checksum stored already as a part of fil_encrypt_buf() */
		ut_ad(use_full_checksum);
		return;
	}

	ut_ad(!block || block->page.frame == page);
	ut_ad(page);

	if (page_zip_) {
		page_zip_des_t*	page_zip;
		ulint		size;

		page_zip = static_cast<page_zip_des_t*>(page_zip_);
		ut_ad(!block || &block->page.zip == page_zip);
		size = page_zip_get_size(page_zip);

		ut_ad(size);
		ut_ad(ut_is_2pow(size));
		ut_ad(size <= UNIV_ZIP_SIZE_MAX);

		switch (fil_page_get_type(page)) {
		case FIL_PAGE_TYPE_ALLOCATED:
		case FIL_PAGE_INODE:
		case FIL_PAGE_IBUF_BITMAP:
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			/* These are essentially uncompressed pages. */
			memcpy(page_zip->data, page, size);
			/* fall through */
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
		case FIL_PAGE_INDEX:
		case FIL_PAGE_RTREE:
			buf_flush_update_zip_checksum(page_zip->data, size);
			return;
		}

		ib::error() << "The compressed page to be written"
			" seems corrupt:";
		ut_print_buf(stderr, page, size);
		fputs("\nInnoDB: Possibly older version of the page:", stderr);
		ut_print_buf(stderr, page_zip->data, size);
		putc('\n', stderr);
		ut_error;
	}

	if (use_full_checksum) {
		static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "aligned");
		static_assert(FIL_PAGE_LSN % 4 == 0, "aligned");
		memcpy_aligned<4>(page + srv_page_size
				  - FIL_PAGE_FCRC32_END_LSN,
				  FIL_PAGE_LSN + 4 + page, 4);
		return buf_flush_assign_full_crc32_checksum(page);
	}

	static_assert(FIL_PAGE_END_LSN_OLD_CHKSUM % 8 == 0, "aligned");
	static_assert(FIL_PAGE_LSN % 8 == 0, "aligned");
	memcpy_aligned<8>(page + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM,
			  FIL_PAGE_LSN + page, 8);

	if (block && srv_page_size == 16384) {
		/* The page type could be garbage in old files
		created before MySQL 5.5. Such files always
		had a page size of 16 kilobytes. */
		ulint	page_type = fil_page_get_type(page);
		ulint	reset_type = page_type;

		switch (block->page.id().page_no() % 16384) {
		case 0:
			reset_type = block->page.id().page_no() == 0
				? FIL_PAGE_TYPE_FSP_HDR
				: FIL_PAGE_TYPE_XDES;
			break;
		case 1:
			reset_type = FIL_PAGE_IBUF_BITMAP;
			break;
		case FSP_TRX_SYS_PAGE_NO:
			if (block->page.id()
			    == page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO)) {
				reset_type = FIL_PAGE_TYPE_TRX_SYS;
				break;
			}
			/* fall through */
		default:
			switch (page_type) {
			case FIL_PAGE_INDEX:
			case FIL_PAGE_TYPE_INSTANT:
			case FIL_PAGE_RTREE:
			case FIL_PAGE_UNDO_LOG:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_FREE_LIST:
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_TYPE_SYS:
			case FIL_PAGE_TYPE_TRX_SYS:
			case FIL_PAGE_TYPE_BLOB:
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
			case FIL_PAGE_IBUF_BITMAP:
				/* These pages should have
				predetermined page numbers
				(see above). */
			default:
				reset_type = FIL_PAGE_TYPE_UNKNOWN;
				break;
			}
		}

		if (UNIV_UNLIKELY(page_type != reset_type)) {
			ib::info()
				<< "Resetting invalid page "
				<< block->page.id() << " type "
				<< page_type << " to "
				<< reset_type << " when flushing.";
			fil_page_set_type(page, reset_type);
		}
	}

	const uint32_t checksum = buf_calc_page_crc32(page);
	mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
	mach_write_to_4(page + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM,
			checksum);
}

/** Reserve a buffer for compression.
@param[in,out]  slot    reserved slot */
static void buf_tmp_reserve_compression_buf(buf_tmp_buffer_t* slot)
{
  if (slot->comp_buf)
    return;
  /* Both Snappy and LZO compression methods require that the output
  buffer be bigger than input buffer. Adjust the allocated size. */
  ulint size= srv_page_size;
  if (provider_service_lzo->is_loaded)
    size+= LZO1X_1_15_MEM_COMPRESS;
  else if (provider_service_snappy->is_loaded)
    size= snappy_max_compressed_length(size);
  slot->comp_buf= static_cast<byte*>(aligned_malloc(size, srv_page_size));
}

/** Encrypt a buffer of temporary tablespace
@param[in]      offset  Page offset
@param[in]      s       Page to encrypt
@param[in,out]  d       Output buffer
@return encrypted buffer or NULL */
static byte* buf_tmp_page_encrypt(ulint offset, const byte* s, byte* d)
{
  /* Calculate the start offset in a page */
  uint srclen= static_cast<uint>(srv_page_size) -
    (FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION +
     FIL_PAGE_FCRC32_CHECKSUM);
  const byte* src= s + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
  byte* dst= d + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;

  memcpy(d, s, FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

  if (!log_tmp_block_encrypt(src, srclen, dst, (offset * srv_page_size), true))
    return NULL;

  const ulint payload= srv_page_size - FIL_PAGE_FCRC32_CHECKSUM;
  mach_write_to_4(d + payload, my_crc32c(0, d, payload));

  srv_stats.pages_encrypted.inc();
  srv_stats.n_temp_blocks_encrypted.inc();
  return d;
}

/** Encryption and page_compression hook that is called just before
a page is written to disk.
@param[in,out]  space   tablespace
@param[in,out]  bpage   buffer page
@param[in]      s       physical page frame that is being encrypted
@param[in,out]  size    payload size in bytes
@return page frame to be written to file
(may be src_frame or an encrypted/compressed copy of it) */
static byte *buf_page_encrypt(fil_space_t* space, buf_page_t* bpage, byte* s,
                              buf_tmp_buffer_t **slot, size_t *size)
{
  ut_ad(!bpage->is_freed());
  ut_ad(space->id == bpage->id().space());
  ut_ad(!*slot);

  ut_d(fil_page_type_validate(space, s));
  const uint32_t page_no= bpage->id().page_no();

  switch (page_no) {
  case TRX_SYS_PAGE_NO:
    if (bpage->id().space() != TRX_SYS_SPACE)
      break;
    /* The TRX_SYS page is neither encrypted nor compressed, because
    it contains the address of the doublewrite buffer. */
    /* fall through */
  case 0:
    /* Page 0 of a tablespace is not encrypted/compressed */
    return s;
  }

  fil_space_crypt_t *crypt_data= space->crypt_data;
  bool encrypted, page_compressed;
  if (space->purpose == FIL_TYPE_TEMPORARY)
  {
    ut_ad(!crypt_data);
    encrypted= innodb_encrypt_temporary_tables;
    page_compressed= false;
  }
  else
  {
    encrypted= crypt_data && !crypt_data->not_encrypted() &&
      crypt_data->type != CRYPT_SCHEME_UNENCRYPTED &&
      (!crypt_data->is_default_encryption() || srv_encrypt_tables);
    page_compressed= space->is_compressed();
  }

  const bool full_crc32= space->full_crc32();

  if (!encrypted && !page_compressed)
  {
    /* No need to encrypt or compress. Clear key-version & crypt-checksum. */
    static_assert(FIL_PAGE_FCRC32_KEY_VERSION % 4 == 0, "alignment");
    static_assert(FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION % 4 == 2,
                  "not perfect alignment");
    if (full_crc32)
      memset_aligned<4>(s + FIL_PAGE_FCRC32_KEY_VERSION, 0, 4);
    else
      memset_aligned<2>(s + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
    return s;
  }

  static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");
  static_assert(FIL_PAGE_LSN % 8 == 0, "alignment");
  if (full_crc32)
    memcpy_aligned<4>(s + srv_page_size - FIL_PAGE_FCRC32_END_LSN,
                      FIL_PAGE_LSN + 4 + s, 4);

  ut_ad(!bpage->zip_size() || !page_compressed);
  /* Find free slot from temporary memory array */
  *slot= buf_pool.io_buf_reserve();
  ut_a(*slot);
  (*slot)->allocate();

  byte *d= (*slot)->crypt_buf;

  if (!page_compressed)
  {
not_compressed:
    d= space->purpose == FIL_TYPE_TEMPORARY
      ? buf_tmp_page_encrypt(page_no, s, d)
      : fil_space_encrypt(space, page_no, s, d);
  }
  else
  {
    ut_ad(space->purpose != FIL_TYPE_TEMPORARY);
    /* First we compress the page content */
    buf_tmp_reserve_compression_buf(*slot);
    byte *tmp= (*slot)->comp_buf;
    ulint len= fil_page_compress(s, tmp, space->flags,
                                 fil_space_get_block_size(space, page_no),
                                 encrypted);

    if (!len)
      goto not_compressed;

    *size= len;

    if (full_crc32)
    {
      ut_d(bool compressed = false);
      len= buf_page_full_crc32_size(tmp,
#ifdef UNIV_DEBUG
                                    &compressed,
#else
                                    NULL,
#endif
                                    NULL);
      ut_ad(compressed);
    }

    /* Workaround for MDEV-15527. */
    memset(tmp + len, 0 , srv_page_size - len);
    ut_d(fil_page_type_validate(space, tmp));

    if (encrypted)
      tmp= fil_space_encrypt(space, page_no, tmp, d);

    if (full_crc32)
    {
      static_assert(FIL_PAGE_FCRC32_CHECKSUM == 4, "alignment");
      mach_write_to_4(tmp + len - 4, my_crc32c(0, tmp, len - 4));
      ut_ad(!buf_page_is_corrupted(true, tmp, space->flags));
    }

    d= tmp;
  }

  ut_d(fil_page_type_validate(space, d));
  (*slot)->out_buf= d;
  return d;
}

/** Free a page whose underlying file page has been freed. */
inline void buf_pool_t::release_freed_page(buf_page_t *bpage) noexcept
{
  mysql_mutex_assert_owner(&mutex);
  mysql_mutex_lock(&flush_list_mutex);
  ut_d(const lsn_t oldest_modification= bpage->oldest_modification();)
  if (fsp_is_system_temporary(bpage->id().space()))
  {
    ut_ad(bpage->frame);
    ut_ad(oldest_modification == 2);
  }
  else
  {
    ut_ad(oldest_modification > 2);
    delete_from_flush_list(bpage, false);
  }
  bpage->clear_oldest_modification();
  mysql_mutex_unlock(&flush_list_mutex);
  bpage->lock.u_unlock(true);

  buf_LRU_free_page(bpage, true);
}

/** Write a flushable page to a file. buf_pool.mutex must be held.
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@param space       tablespace
@return whether the page was flushed and buf_pool.mutex was released */
inline bool buf_page_t::flush(bool lru, fil_space_t *space)
{
  ut_ad(in_file());
  ut_ad(in_LRU_list);
  ut_ad((space->purpose == FIL_TYPE_TEMPORARY) ==
        (space == fil_system.temp_space));
  ut_ad(space->referenced());
  ut_ad(lru || space != fil_system.temp_space);

  if (!lock.u_lock_try(true))
    return false;

  const auto s= state();
  ut_a(s >= FREED);

  if (s < UNFIXED)
  {
    buf_pool.release_freed_page(this);
    mysql_mutex_unlock(&buf_pool.mutex);
    return true;
  }

  if (s >= READ_FIX || oldest_modification() < 2)
  {
    lock.u_unlock(true);
    return false;
  }

  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);

  /* Apart from the U-lock, this block will also be protected by
  is_write_fixed() and oldest_modification()>1.
  Thus, it cannot be relocated or removed. */

  DBUG_PRINT("ib_buf", ("%s %u page %u:%u",
                        lru ? "LRU" : "flush_list",
                        id().space(), id().page_no()));
  ut_d(const auto f=) zip.fix.fetch_add(WRITE_FIX - UNFIXED);
  ut_ad(f >= UNFIXED);
  ut_ad(f < READ_FIX);
  ut_ad(space == fil_system.temp_space
        ? oldest_modification() == 2
        : oldest_modification() > 2);
  if (lru)
  {
    ut_ad(buf_pool.n_flush_LRU_ < ULINT_UNDEFINED);
    buf_pool.n_flush_LRU_++;
  }
  else
  {
    ut_ad(buf_pool.n_flush_list_ < ULINT_UNDEFINED);
    buf_pool.n_flush_list_++;
  }
  buf_flush_page_count++;

  mysql_mutex_unlock(&buf_pool.mutex);

  buf_block_t *block= reinterpret_cast<buf_block_t*>(this);
  page_t *write_frame= zip.data;

  space->reacquire();
  size_t size;
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
  size_t orig_size;
#endif
  IORequest::Type type= lru ? IORequest::WRITE_LRU : IORequest::WRITE_ASYNC;
  buf_tmp_buffer_t *slot= nullptr;

  if (UNIV_UNLIKELY(!frame)) /* ROW_FORMAT=COMPRESSED */
  {
    ut_ad(!space->full_crc32());
    ut_ad(!space->is_compressed()); /* not page_compressed */
    size= zip_size();
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
    orig_size= size;
#endif
    buf_flush_update_zip_checksum(write_frame, size);
    write_frame= buf_page_encrypt(space, this, write_frame, &slot, &size);
    ut_ad(size == zip_size());
  }
  else
  {
    byte *page= frame;
    size= block->physical_size();
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
    orig_size= size;
#endif

    if (space->full_crc32())
    {
      /* innodb_checksum_algorithm=full_crc32 is not implemented for
      ROW_FORMAT=COMPRESSED pages. */
      ut_ad(!write_frame);
      page= buf_page_encrypt(space, this, page, &slot, &size);
      buf_flush_init_for_writing(block, page, nullptr, true);
    }
    else
    {
      buf_flush_init_for_writing(block, page, write_frame ? &zip : nullptr,
                                 false);
      page= buf_page_encrypt(space, this, write_frame ? write_frame : page,
                             &slot, &size);
    }

#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
    if (size != orig_size)
    {
      switch (space->chain.start->punch_hole) {
      case 1:
        type= lru ? IORequest::PUNCH_LRU : IORequest::PUNCH;
        break;
      case 2:
        size= orig_size;
      }
    }
#endif
    write_frame= page;
  }

  if ((s & LRU_MASK) == REINIT || !space->use_doublewrite())
  {
    if (UNIV_LIKELY(space->purpose == FIL_TYPE_TABLESPACE))
    {
      const lsn_t lsn=
        mach_read_from_8(my_assume_aligned<8>(FIL_PAGE_LSN +
                                              (write_frame ? write_frame
                                               : frame)));
      ut_ad(lsn >= oldest_modification());
      log_write_up_to(lsn, true);
    }
    space->io(IORequest{type, this, slot}, physical_offset(), size,
              write_frame, this);
  }
  else
    buf_dblwr.add_to_batch(IORequest{this, slot, space->chain.start, type},
                           size);

  /* Increment the I/O operation count used for selecting LRU policy. */
  buf_LRU_stat_inc_io();
  return true;
}

/** Check whether a page can be flushed from the buf_pool.
@param id          page identifier
@param fold        id.fold()
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@return whether the page can be flushed */
static bool buf_flush_check_neighbor(const page_id_t id, ulint fold, bool lru)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(fold == id.fold());

  /* FIXME: cell_get() is being invoked while holding buf_pool.mutex */
  const buf_page_t *bpage=
    buf_pool.page_hash.get(id, buf_pool.page_hash.cell_get(fold));

  if (!bpage || buf_pool.watch_is_sentinel(*bpage))
    return false;

  /* We avoid flushing 'non-old' blocks in an LRU flush, because the
  flushed blocks are soon freed */
  if (lru && !bpage->is_old())
    return false;

  return bpage->oldest_modification() > 1 && bpage->ready_for_flush();
}

/** Check which neighbors of a page can be flushed from the buf_pool.
@param space       tablespace
@param id          page identifier of a dirty page
@param contiguous  whether to consider contiguous areas of pages
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@return last page number that can be flushed */
static page_id_t buf_flush_check_neighbors(const fil_space_t &space,
                                           page_id_t &id, bool contiguous,
                                           bool lru)
{
  ut_ad(id.page_no() < space.size +
        (space.physical_size() == 2048 ? 1
         : space.physical_size() == 1024 ? 3 : 0));
  /* When flushed, dirty blocks are searched in neighborhoods of this
  size, and flushed along with the original page. */
  const ulint s= buf_pool.curr_size / 16;
  const uint32_t read_ahead= buf_pool.read_ahead_area;
  const uint32_t buf_flush_area= read_ahead > s
    ? static_cast<uint32_t>(s) : read_ahead;
  page_id_t low= id - (id.page_no() % buf_flush_area);
  page_id_t high= low + buf_flush_area;
  high.set_page_no(std::min(high.page_no(), space.last_page_number()));

  if (!contiguous)
  {
    high= std::max(id + 1, high);
    id= low;
    return high;
  }

  /* Determine the contiguous dirty area around id. */
  const ulint id_fold= id.fold();

  mysql_mutex_lock(&buf_pool.mutex);

  if (id > low)
  {
    ulint fold= id_fold;
    for (page_id_t i= id - 1;; --i)
    {
      fold--;
      if (!buf_flush_check_neighbor(i, fold, lru))
      {
        low= i + 1;
        break;
      }
      if (i == low)
        break;
    }
  }

  page_id_t i= id;
  id= low;
  ulint fold= id_fold;
  while (++i < high)
  {
    ++fold;
    if (!buf_flush_check_neighbor(i, fold, lru))
      break;
  }

  mysql_mutex_unlock(&buf_pool.mutex);
  return i;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Write punch-hole or zeroes of the freed ranges when
innodb_immediate_scrub_data_uncompressed from the freed ranges.
@param space    tablespace which may contain ranges of freed pages
@param writable whether the tablespace is writable
@return number of pages written or hole-punched */
static uint32_t buf_flush_freed_pages(fil_space_t *space, bool writable)
{
  const bool punch_hole= space->chain.start->punch_hole == 1;
  if (!punch_hole && !srv_immediate_scrub_data_uncompressed)
    return 0;

  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  mysql_mutex_assert_not_owner(&buf_pool.mutex);

  space->freed_range_mutex.lock();
  if (space->freed_ranges.empty() ||
      log_sys.get_flushed_lsn() < space->get_last_freed_lsn())
  {
    space->freed_range_mutex.unlock();
    return 0;
  }

  const unsigned physical_size{space->physical_size()};

  range_set freed_ranges= std::move(space->freed_ranges);
  uint32_t written= 0;

  if (!writable);
  else if (punch_hole)
  {
    for (const auto &range : freed_ranges)
    {
      written+= range.last - range.first + 1;
      space->reacquire();
      space->io(IORequest(IORequest::PUNCH_RANGE),
                          os_offset_t{range.first} * physical_size,
                          (range.last - range.first + 1) * physical_size,
                          nullptr);
    }
  }
  else
  {
    for (const auto &range : freed_ranges)
    {
      written+= range.last - range.first + 1;
      for (os_offset_t i= range.first; i <= range.last; i++)
      {
        space->reacquire();
        space->io(IORequest(IORequest::WRITE_ASYNC),
                  i * physical_size, physical_size,
                  const_cast<byte*>(field_ref_zero));
      }
    }
  }

  space->freed_range_mutex.unlock();
  return written;
}

/** Flushes to disk all flushable pages within the flush area
and also write zeroes or punch the hole for the freed ranges of pages.
@param space       tablespace
@param page_id     page identifier
@param contiguous  whether to consider contiguous areas of pages
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@param n_flushed   number of pages flushed so far in this batch
@param n_to_flush  maximum number of pages we are allowed to flush
@return number of pages flushed */
static ulint buf_flush_try_neighbors(fil_space_t *space,
                                     const page_id_t page_id,
                                     bool contiguous, bool lru,
                                     ulint n_flushed, ulint n_to_flush)
{
  ut_ad(space->id == page_id.space());

  ulint count= 0;
  page_id_t id= page_id;
  page_id_t high= buf_flush_check_neighbors(*space, id, contiguous, lru);

  ut_ad(page_id >= id);
  ut_ad(page_id < high);

  for (ulint id_fold= id.fold(); id < high && !space->is_stopping();
       ++id, ++id_fold)
  {
    if (count + n_flushed >= n_to_flush)
    {
      if (id > page_id)
        break;
      /* If the page whose neighbors we are flushing has not been
      flushed yet, we must flush the page that we selected originally. */
      id= page_id;
      id_fold= id.fold();
    }

    const buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id_fold);
    mysql_mutex_lock(&buf_pool.mutex);

    if (buf_page_t *bpage= buf_pool.page_hash.get(id, chain))
    {
      ut_ad(bpage->in_file());
      /* We avoid flushing 'non-old' blocks in an LRU flush,
      because the flushed blocks are soon freed */
      if (!lru || id == page_id || bpage->is_old())
      {
        if (!buf_pool.watch_is_sentinel(*bpage) &&
            bpage->oldest_modification() > 1 && bpage->ready_for_flush() &&
            bpage->flush(lru, space))
        {
          ++count;
          continue;
        }
      }
    }

    mysql_mutex_unlock(&buf_pool.mutex);
  }

  if (auto n= count - 1)
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_NEIGHBOR_TOTAL_PAGE,
                                 MONITOR_FLUSH_NEIGHBOR_COUNT,
                                 MONITOR_FLUSH_NEIGHBOR_PAGES, n);
  }

  return count;
}

/*******************************************************************//**
This utility moves the uncompressed frames of pages to the free list.
Note that this function does not actually flush any data to disk. It
just detaches the uncompressed frames from the compressed pages at the
tail of the unzip_LRU and puts those freed frames in the free list.
Note that it is a best effort attempt and it is not guaranteed that
after a call to this function there will be 'max' blocks in the free
list.
@param[in]	max		desired number of blocks in the free_list
@return number of blocks moved to the free list. */
static ulint buf_free_from_unzip_LRU_list_batch(ulint max)
{
	ulint		scanned = 0;
	ulint		count = 0;

	mysql_mutex_assert_owner(&buf_pool.mutex);

	buf_block_t*	block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);

	while (block
	       && count < max
	       && UT_LIST_GET_LEN(buf_pool.free) < srv_LRU_scan_depth
	       && UT_LIST_GET_LEN(buf_pool.unzip_LRU)
	       > UT_LIST_GET_LEN(buf_pool.LRU) / 10) {

		++scanned;
		if (buf_LRU_free_page(&block->page, false)) {
			/* Block was freed. buf_pool.mutex potentially
			released and reacquired */
			++count;
			block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
		} else {
			block = UT_LIST_GET_PREV(unzip_LRU, block);
		}
	}

	mysql_mutex_assert_owner(&buf_pool.mutex);

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	return(count);
}

/** Start writing out pages for a tablespace.
@param id   tablespace identifier
@return tablespace and number of pages written */
static std::pair<fil_space_t*, uint32_t> buf_flush_space(const uint32_t id)
{
  if (fil_space_t *space= fil_space_t::get(id))
    return {space, buf_flush_freed_pages(space, true)};
  return {nullptr, 0};
}

struct flush_counters_t
{
  /** number of dirty pages flushed */
  ulint flushed;
  /** number of clean pages evicted */
  ulint evicted;
};

/** Try to discard a dirty page.
@param bpage      dirty page whose tablespace is not accessible */
static void buf_flush_discard_page(buf_page_t *bpage)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  ut_ad(bpage->in_file());
  ut_ad(bpage->oldest_modification());

  if (!bpage->lock.u_lock_try(false))
    return;

  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_pool.delete_from_flush_list(bpage);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  ut_d(const auto state= bpage->state());
  ut_ad(state == buf_page_t::FREED || state == buf_page_t::UNFIXED ||
        state == buf_page_t::IBUF_EXIST || state == buf_page_t::REINIT);
  bpage->lock.u_unlock();

  buf_LRU_free_page(bpage, true);
}

/** Flush dirty blocks from the end of the LRU list.
@param max   maximum number of blocks to make available in buf_pool.free
@param n     counts of flushed and evicted pages */
static void buf_flush_LRU_list_batch(ulint max, flush_counters_t *n)
{
  ulint scanned= 0;
  ulint free_limit= srv_LRU_scan_depth;

  mysql_mutex_assert_owner(&buf_pool.mutex);
  if (buf_pool.withdraw_target && buf_pool.is_shrinking())
    free_limit+= buf_pool.withdraw_target - UT_LIST_GET_LEN(buf_pool.withdraw);

  const auto neighbors= UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN
    ? 0 : srv_flush_neighbors;
  fil_space_t *space= nullptr;
  uint32_t last_space_id= FIL_NULL;
  static_assert(FIL_NULL > SRV_TMP_SPACE_ID, "consistency");
  static_assert(FIL_NULL > SRV_SPACE_ID_UPPER_BOUND, "consistency");

  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.LRU);
       bpage &&
       ((UT_LIST_GET_LEN(buf_pool.LRU) > BUF_LRU_MIN_LEN &&
         UT_LIST_GET_LEN(buf_pool.free) < free_limit &&
         n->flushed + n->evicted < max) ||
        recv_recovery_is_on()); ++scanned)
  {
  retry:
    buf_page_t *prev= UT_LIST_GET_PREV(LRU, bpage);
    const lsn_t oldest_modification= bpage->oldest_modification();
    buf_pool.lru_hp.set(prev);
    const auto state= bpage->state();
    ut_ad(state >= buf_page_t::FREED);
    ut_ad(bpage->in_LRU_list);

    if (oldest_modification <= 1)
    {
      if (state != buf_page_t::FREED &&
          (state >= buf_page_t::READ_FIX || (~buf_page_t::LRU_MASK & state)))
        goto must_skip;
      if (buf_LRU_free_page(bpage, true))
        ++n->evicted;
    }
    else if (state < buf_page_t::READ_FIX)
    {
      /* Block is ready for flush. Dispatch an IO request. The IO
      helper thread will put it on free list in IO completion routine. */
      const page_id_t page_id(bpage->id());
      const uint32_t space_id= page_id.space();
      if (!space || space->id != space_id)
      {
        if (last_space_id != space_id)
        {
          buf_pool.lru_hp.set(bpage);
          mysql_mutex_unlock(&buf_pool.mutex);
          if (space)
            space->release();
          auto p= buf_flush_space(space_id);
          space= p.first;
          last_space_id= space_id;
          mysql_mutex_lock(&buf_pool.mutex);
          if (p.second)
            buf_pool.stat.n_pages_written+= p.second;
          bpage= buf_pool.lru_hp.get();
          goto retry;
        }
        else
          ut_ad(!space);
      }
      else if (space->is_stopping())
      {
        space->release();
        space= nullptr;
      }

      if (!space)
        buf_flush_discard_page(bpage);
      else if (neighbors && space->is_rotational())
      {
        mysql_mutex_unlock(&buf_pool.mutex);
        n->flushed+= buf_flush_try_neighbors(space, page_id, neighbors == 1,
                                             true, n->flushed, max);
reacquire_mutex:
        mysql_mutex_lock(&buf_pool.mutex);
      }
      else if (bpage->flush(true, space))
      {
        ++n->flushed;
        goto reacquire_mutex;
      }
    }
    else
    must_skip:
      /* Can't evict or dispatch this block. Go to previous. */
      ut_ad(buf_pool.lru_hp.is_hp(prev));
    bpage= buf_pool.lru_hp.get();
  }

  buf_pool.lru_hp.set(nullptr);

  if (space)
    space->release();

  if (scanned)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_BATCH_SCANNED,
                                 MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_LRU_BATCH_SCANNED_PER_CALL,
                                 scanned);
}

/** Flush and move pages from LRU or unzip_LRU list to the free list.
Whether LRU or unzip_LRU is used depends on the state of the system.
@param max   maximum number of blocks to make available in buf_pool.free
@return number of flushed pages */
static ulint buf_do_LRU_batch(ulint max)
{
  const ulint n_unzip_LRU_evicted= buf_LRU_evict_from_unzip_LRU()
    ? buf_free_from_unzip_LRU_list_batch(max)
    : 0;
  flush_counters_t n;
  n.flushed= 0;
  n.evicted= n_unzip_LRU_evicted;
  buf_flush_LRU_list_batch(max, &n);
  mysql_mutex_assert_owner(&buf_pool.mutex);

  if (const ulint evicted= n.evicted - n_unzip_LRU_evicted)
    buf_lru_freed_page_count+= evicted;

  if (n.flushed)
    buf_lru_flush_page_count+= n.flushed;

  return n.flushed;
}

/** This utility flushes dirty blocks from the end of the flush_list.
The calling thread is not allowed to own any latches on pages!
@param max_n    maximum mumber of blocks to flush
@param lsn      once an oldest_modification>=lsn is found, terminate the batch
@return number of blocks for which the write request was queued */
static ulint buf_do_flush_list_batch(ulint max_n, lsn_t lsn)
{
  ulint count= 0;
  ulint scanned= 0;

  mysql_mutex_assert_owner(&buf_pool.mutex);

  const auto neighbors= UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN
    ? 0 : srv_flush_neighbors;
  fil_space_t *space= nullptr;
  uint32_t last_space_id= FIL_NULL;
  static_assert(FIL_NULL > SRV_TMP_SPACE_ID, "consistency");
  static_assert(FIL_NULL > SRV_SPACE_ID_UPPER_BOUND, "consistency");

  /* Start from the end of the list looking for a suitable block to be
  flushed. */
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  ulint len= UT_LIST_GET_LEN(buf_pool.flush_list);

  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list);
       bpage && len && count < max_n; ++scanned, len--)
  {
    const lsn_t oldest_modification= bpage->oldest_modification();
    if (oldest_modification >= lsn)
      break;
    ut_ad(bpage->in_file());

    buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

    if (oldest_modification == 1)
    {
      buf_pool.delete_from_flush_list(bpage);
    skip:
      bpage= prev;
      continue;
    }

    ut_ad(oldest_modification > 2);

    if (!bpage->ready_for_flush())
      goto skip;

    /* In order not to degenerate this scan to O(n*n) we attempt to
    preserve the pointer position. Any thread that would remove 'prev'
    from buf_pool.flush_list must adjust the hazard pointer.

    Note: A concurrent execution of buf_flush_list_space() may
    terminate this scan prematurely. The buf_pool.n_flush_list()
    should prevent multiple threads from executing
    buf_do_flush_list_batch() concurrently,
    but buf_flush_list_space() is ignoring that. */
    buf_pool.flush_hp.set(prev);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    const page_id_t page_id(bpage->id());
    const uint32_t space_id= page_id.space();
    if (!space || space->id != space_id)
    {
      if (last_space_id != space_id)
      {
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        buf_pool.flush_hp.set(bpage);
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        mysql_mutex_unlock(&buf_pool.mutex);
        if (space)
          space->release();
        auto p= buf_flush_space(space_id);
        space= p.first;
        last_space_id= space_id;
        mysql_mutex_lock(&buf_pool.mutex);
        if (p.second)
          buf_pool.stat.n_pages_written+= p.second;
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        bpage= buf_pool.flush_hp.get();
        if (!bpage)
          break;
        if (bpage->id() != page_id)
          continue;
        buf_pool.flush_hp.set(UT_LIST_GET_PREV(list, bpage));
        if (bpage->oldest_modification() <= 1 || !bpage->ready_for_flush())
          goto next;
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      }
      else
        ut_ad(!space);
    }
    else if (space->is_stopping())
    {
      space->release();
      space= nullptr;
    }

    if (!space)
      buf_flush_discard_page(bpage);
    else if (neighbors && space->is_rotational())
    {
      mysql_mutex_unlock(&buf_pool.mutex);
      count+= buf_flush_try_neighbors(space, page_id, neighbors == 1,
                                      false, count, max_n);
    reacquire_mutex:
      mysql_mutex_lock(&buf_pool.mutex);
    }
    else if (bpage->flush(false, space))
    {
      ++count;
      goto reacquire_mutex;
    }

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
  next:
    bpage= buf_pool.flush_hp.get();
  }

  buf_pool.flush_hp.set(nullptr);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (space)
    space->release();

  if (scanned)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_SCANNED,
                                 MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
                                 scanned);
  if (count)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_TOTAL_PAGE,
                                 MONITOR_FLUSH_BATCH_COUNT,
                                 MONITOR_FLUSH_BATCH_PAGES,
                                 count);
  mysql_mutex_assert_owner(&buf_pool.mutex);
  return count;
}

/** Wait until a flush batch ends.
@param lru    true=buf_pool.LRU; false=buf_pool.flush_list */
void buf_flush_wait_batch_end(bool lru)
{
  const auto &n_flush= lru ? buf_pool.n_flush_LRU_ : buf_pool.n_flush_list_;

  if (n_flush)
  {
    auto cond= lru ? &buf_pool.done_flush_LRU : &buf_pool.done_flush_list;
    tpool::tpool_wait_begin();
    thd_wait_begin(nullptr, THD_WAIT_DISKIO);
    do
      my_cond_wait(cond, &buf_pool.mutex.m_mutex);
    while (n_flush);
    tpool::tpool_wait_end();
    thd_wait_end(nullptr);
    pthread_cond_broadcast(cond);
  }
}

/** Write out dirty blocks from buf_pool.flush_list.
@param max_n    wished maximum mumber of blocks flushed
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target
@return the number of processed pages
@retval 0 if a buf_pool.flush_list batch is already running */
static ulint buf_flush_list(ulint max_n= ULINT_UNDEFINED, lsn_t lsn= LSN_MAX)
{
  ut_ad(lsn);

  if (buf_pool.n_flush_list())
    return 0;

  mysql_mutex_lock(&buf_pool.mutex);
  const bool running= buf_pool.n_flush_list_ != 0;
  /* FIXME: we are performing a dirty read of buf_pool.flush_list.count
  while not holding buf_pool.flush_list_mutex */
  if (running || !UT_LIST_GET_LEN(buf_pool.flush_list))
  {
    if (!running)
      pthread_cond_broadcast(&buf_pool.done_flush_list);
    mysql_mutex_unlock(&buf_pool.mutex);
    return 0;
  }

  buf_pool.n_flush_list_++;
  const ulint n_flushed= buf_do_flush_list_batch(max_n, lsn);
  const ulint n_flushing= --buf_pool.n_flush_list_;

  buf_pool.try_LRU_scan= true;

  mysql_mutex_unlock(&buf_pool.mutex);

  if (!n_flushing)
    pthread_cond_broadcast(&buf_pool.done_flush_list);

  buf_dblwr.flush_buffered_writes();

  DBUG_PRINT("ib_buf", ("flush_list completed, " ULINTPF " pages", n_flushed));
  return n_flushed;
}

/** Try to flush all the dirty pages that belong to a given tablespace.
@param space       tablespace
@param n_flushed   number of pages written
@return whether the flush for some pages might not have been initiated */
bool buf_flush_list_space(fil_space_t *space, ulint *n_flushed)
{
  const auto space_id= space->id;
  ut_ad(space_id <= SRV_SPACE_ID_UPPER_BOUND);

  bool may_have_skipped= false;
  ulint max_n_flush= srv_io_capacity;

  bool acquired= space->acquire();
  {
    const uint32_t written{buf_flush_freed_pages(space, acquired)};
    mysql_mutex_lock(&buf_pool.mutex);
    if (written)
      buf_pool.stat.n_pages_written+= written;
  }
  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
  {
    ut_ad(bpage->oldest_modification());
    ut_ad(bpage->in_file());

    buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);
    if (bpage->id().space() != space_id);
    else if (bpage->oldest_modification() == 1)
      buf_pool.delete_from_flush_list(bpage);
    else if (!bpage->ready_for_flush())
      may_have_skipped= true;
    else
    {
      /* In order not to degenerate this scan to O(n*n) we attempt to
      preserve the pointer position. Any thread that would remove 'prev'
      from buf_pool.flush_list must adjust the hazard pointer.

      Note: Multiple executions of buf_flush_list_space() may be
      interleaved, and also buf_do_flush_list_batch() may be running
      concurrently. This may terminate our iteration prematurely,
      leading us to return may_have_skipped=true. */
      buf_pool.flush_hp.set(prev);
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);

      if (!acquired)
      {
      was_freed:
        buf_flush_discard_page(bpage);
      }
      else
      {
        if (space->is_stopping())
        {
          space->release();
          acquired= false;
          goto was_freed;
        }
        if (!bpage->flush(false, space))
        {
          may_have_skipped= true;
          mysql_mutex_lock(&buf_pool.flush_list_mutex);
          goto next_after_skip;
        }
        if (n_flushed)
          ++*n_flushed;
        if (!--max_n_flush)
        {
          mysql_mutex_lock(&buf_pool.mutex);
          mysql_mutex_lock(&buf_pool.flush_list_mutex);
          may_have_skipped= true;
          break;
        }
        mysql_mutex_lock(&buf_pool.mutex);
      }

      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      if (!buf_pool.flush_hp.is_hp(prev))
        may_have_skipped= true;
    next_after_skip:
      bpage= buf_pool.flush_hp.get();
      continue;
    }

    bpage= prev;
  }

  /* Note: this loop may have been executed concurrently with
  buf_do_flush_list_batch() as well as other threads executing
  buf_flush_list_space(). We should always return true from
  buf_flush_list_space() if that should be the case; in
  buf_do_flush_list_batch() we will simply perform less work. */

  buf_pool.flush_hp.set(nullptr);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  buf_pool.try_LRU_scan= true;

  mysql_mutex_unlock(&buf_pool.mutex);

  if (acquired)
    space->release();

  if (space->purpose == FIL_TYPE_IMPORT)
    os_aio_wait_until_no_pending_writes();
  else
    buf_dblwr.flush_buffered_writes();

  return may_have_skipped;
}

/** Write out dirty blocks from buf_pool.LRU.
@param max_n    wished maximum mumber of blocks flushed
@return the number of processed pages
@retval 0 if a buf_pool.LRU batch is already running */
ulint buf_flush_LRU(ulint max_n)
{
  if (buf_pool.n_flush_LRU())
    return 0;

  log_buffer_flush_to_disk();

  mysql_mutex_lock(&buf_pool.mutex);
  if (buf_pool.n_flush_LRU_)
  {
    mysql_mutex_unlock(&buf_pool.mutex);
    return 0;
  }
  buf_pool.n_flush_LRU_++;

  ulint n_flushed= buf_do_LRU_batch(max_n);

  const ulint n_flushing= --buf_pool.n_flush_LRU_;

  buf_pool.try_LRU_scan= true;

  mysql_mutex_unlock(&buf_pool.mutex);

  if (!n_flushing)
  {
    pthread_cond_broadcast(&buf_pool.done_flush_LRU);
    pthread_cond_signal(&buf_pool.done_free);
  }

  buf_dblwr.flush_buffered_writes();

  DBUG_PRINT("ib_buf", ("LRU flush completed, " ULINTPF " pages", n_flushed));
  return n_flushed;
}

#ifdef HAVE_PMEM
# include <libpmem.h>
#endif

/** Write checkpoint information to the log header and release mutex.
@param end_lsn    start LSN of the FILE_CHECKPOINT mini-transaction */
inline void log_t::write_checkpoint(lsn_t end_lsn) noexcept
{
  ut_ad(!srv_read_only_mode);
  ut_ad(end_lsn >= next_checkpoint_lsn);
  ut_ad(end_lsn <= get_lsn());
  ut_ad(end_lsn + SIZE_OF_FILE_CHECKPOINT <= get_lsn() ||
        srv_shutdown_state > SRV_SHUTDOWN_INITIATED);

  DBUG_PRINT("ib_log",
             ("checkpoint at " LSN_PF " written", next_checkpoint_lsn));

  auto n= next_checkpoint_no;
  const size_t offset{(n & 1) ? CHECKPOINT_2 : CHECKPOINT_1};
  static_assert(CPU_LEVEL1_DCACHE_LINESIZE >= 64, "efficiency");
  static_assert(CPU_LEVEL1_DCACHE_LINESIZE <= 4096, "compatibility");
  byte* c= my_assume_aligned<CPU_LEVEL1_DCACHE_LINESIZE>
    (is_pmem() ? buf + offset : checkpoint_buf);
  memset_aligned<CPU_LEVEL1_DCACHE_LINESIZE>(c, 0, CPU_LEVEL1_DCACHE_LINESIZE);
  mach_write_to_8(my_assume_aligned<8>(c), next_checkpoint_lsn);
  mach_write_to_8(my_assume_aligned<8>(c + 8), end_lsn);
  mach_write_to_4(my_assume_aligned<4>(c + 60), my_crc32c(0, c, 60));

  lsn_t resizing;

#ifdef HAVE_PMEM
  if (is_pmem())
  {
    resizing= resize_lsn.load(std::memory_order_relaxed);

    if (resizing > 1 && resizing <= next_checkpoint_lsn)
    {
      memcpy_aligned<64>(resize_buf + CHECKPOINT_1, c, 64);
      header_write(resize_buf, resizing, is_encrypted());
      pmem_persist(resize_buf, resize_target);
    }
    pmem_persist(c, 64);
  }
  else
#endif
  {
    ut_ad(!checkpoint_pending);
    checkpoint_pending= true;
    latch.wr_unlock();
    log_write_and_flush_prepare();
    resizing= resize_lsn.load(std::memory_order_relaxed);
    /* FIXME: issue an asynchronous write */
    log.write(offset, {c, get_block_size()});
    if (resizing > 1 && resizing <= next_checkpoint_lsn)
    {
      byte *buf= static_cast<byte*>(aligned_malloc(4096, 4096));
      memset_aligned<4096>(buf, 0, 4096);
      header_write(buf, resizing, is_encrypted());
      resize_log.write(0, {buf, 4096});
      aligned_free(buf);
      resize_log.write(CHECKPOINT_1, {c, get_block_size()});
    }

    if (srv_file_flush_method != SRV_O_DSYNC)
      ut_a(log.flush());
    latch.wr_lock(SRW_LOCK_CALL);
    ut_ad(checkpoint_pending);
    checkpoint_pending= false;
    resizing= resize_lsn.load(std::memory_order_relaxed);
  }

  ut_ad(!checkpoint_pending);
  next_checkpoint_no++;
  const lsn_t checkpoint_lsn{next_checkpoint_lsn};
  last_checkpoint_lsn= checkpoint_lsn;

  DBUG_PRINT("ib_log", ("checkpoint ended at " LSN_PF ", flushed to " LSN_PF,
                        checkpoint_lsn, get_flushed_lsn()));
  lsn_t resizing_completed= 0;

  if (resizing > 1 && resizing <= checkpoint_lsn)
  {
    ut_ad(is_pmem() == !resize_flush_buf);

    if (!is_pmem())
    {
      if (srv_file_flush_method != SRV_O_DSYNC)
        ut_a(resize_log.flush());
      IF_WIN(log.close(),);
    }

    if (resize_rename())
    {
      /* Resizing failed. Discard the log_sys.resize_log. */
#ifdef HAVE_PMEM
      if (is_pmem())
        my_munmap(resize_buf, resize_target);
      else
#endif
      {
        ut_free_dodump(resize_buf, buf_size);
        ut_free_dodump(resize_flush_buf, buf_size);
#ifdef _WIN32
        ut_ad(!log.is_opened());
        bool success;
        log.m_file=
          os_file_create_func(get_log_file_path().c_str(),
                              OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
                              OS_FILE_NORMAL, OS_LOG_FILE, false, &success);
        ut_a(success);
        ut_a(log.is_opened());
#endif
      }
    }
    else
    {
      /* Adopt the resized log. */
#ifdef HAVE_PMEM
      if (is_pmem())
      {
        my_munmap(buf, file_size);
        buf= resize_buf;
        buf_free= START_OFFSET + (get_lsn() - resizing);
      }
      else
#endif
      {
        IF_WIN(,log.close());
        std::swap(log, resize_log);
        ut_free_dodump(buf, buf_size);
        ut_free_dodump(flush_buf, buf_size);
        buf= resize_buf;
        flush_buf= resize_flush_buf;
      }
      srv_log_file_size= resizing_completed= file_size= resize_target;
      first_lsn= resizing;
      set_capacity();
    }
    ut_ad(!resize_log.is_opened());
    resize_buf= nullptr;
    resize_flush_buf= nullptr;
    resize_target= 0;
    resize_lsn.store(0, std::memory_order_relaxed);
  }

  log_resize_release();

  if (UNIV_LIKELY(resizing <= 1));
  else if (resizing > checkpoint_lsn)
    buf_flush_ahead(resizing, false);
  else if (resizing_completed)
    ib::info() << "Resized log to " << ib::bytes_iec{resizing_completed}
      << "; start LSN=" << resizing;
  else
    sql_print_error("InnoDB: Resize of log failed at " LSN_PF,
                    get_flushed_lsn());
}

/** Initiate a log checkpoint, discarding the start of the log.
@param oldest_lsn   the checkpoint LSN
@param end_lsn      log_sys.get_lsn()
@return true if success, false if a checkpoint write was already running */
static bool log_checkpoint_low(lsn_t oldest_lsn, lsn_t end_lsn)
{
  ut_ad(!srv_read_only_mode);
#ifndef SUX_LOCK_GENERIC
  ut_ad(log_sys.latch.is_write_locked());
#endif
  ut_ad(oldest_lsn <= end_lsn);
  ut_ad(end_lsn == log_sys.get_lsn());

  if (oldest_lsn == log_sys.last_checkpoint_lsn ||
      (oldest_lsn == end_lsn &&
       !log_sys.resize_in_progress() &&
       oldest_lsn == log_sys.last_checkpoint_lsn +
       (log_sys.is_encrypted()
        ? SIZE_OF_FILE_CHECKPOINT + 8 : SIZE_OF_FILE_CHECKPOINT)))
  {
    /* Do nothing, because nothing was logged (other than a
    FILE_CHECKPOINT record) since the previous checkpoint. */
  do_nothing:
    log_sys.latch.wr_unlock();
    return true;
  }

  ut_ad(!recv_no_log_write);
  ut_ad(oldest_lsn > log_sys.last_checkpoint_lsn);
  /* Repeat the FILE_MODIFY records after the checkpoint, in case some
  log records between the checkpoint and log_sys.lsn need them.
  Finally, write a FILE_CHECKPOINT record. Redo log apply expects to
  see a FILE_CHECKPOINT after the checkpoint, except on clean
  shutdown, where the log will be empty after the checkpoint.

  It is important that we write out the redo log before any further
  dirty pages are flushed to the tablespace files.  At this point,
  because we hold exclusive log_sys.latch,
  mtr_t::commit() in other threads will be blocked,
  and no pages can be added to buf_pool.flush_list. */
  const lsn_t flush_lsn{fil_names_clear(oldest_lsn)};
  ut_ad(flush_lsn >= end_lsn + SIZE_OF_FILE_CHECKPOINT);
  log_sys.latch.wr_unlock();
  log_write_up_to(flush_lsn, true);
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  if (log_sys.last_checkpoint_lsn >= oldest_lsn)
    goto do_nothing;

  ut_ad(log_sys.get_flushed_lsn() >= flush_lsn);

  if (log_sys.checkpoint_pending)
  {
    /* A checkpoint write is running */
    log_sys.latch.wr_unlock();
    return false;
  }

  log_sys.next_checkpoint_lsn= oldest_lsn;
  log_sys.write_checkpoint(end_lsn);

  return true;
}

/** Make a checkpoint. Note that this function does not flush dirty
blocks from the buffer pool: it only checks what is lsn of the oldest
modification in the pool, and writes information about the lsn in
log file. Use log_make_checkpoint() to flush also the pool.
@retval true if the checkpoint was or had been made
@retval false if a checkpoint write was already running */
static bool log_checkpoint()
{
  if (recv_recovery_is_on())
    recv_sys.apply(true);

  switch (srv_file_flush_method) {
  case SRV_NOSYNC:
  case SRV_O_DIRECT_NO_FSYNC:
    break;
  default:
    fil_flush_file_spaces();
  }

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  const lsn_t end_lsn= log_sys.get_lsn();
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  const lsn_t oldest_lsn= buf_pool.get_oldest_modification(end_lsn);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  return log_checkpoint_low(oldest_lsn, end_lsn);
}

/** Make a checkpoint. */
ATTRIBUTE_COLD void log_make_checkpoint()
{
  buf_flush_wait_flushed(log_sys.get_lsn(std::memory_order_acquire));
  while (!log_checkpoint());
}

/** Wait for all dirty pages up to an LSN to be written out.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
static void buf_flush_wait(lsn_t lsn)
{
  ut_ad(lsn <= log_sys.get_lsn());

  while (buf_pool.get_oldest_modification(lsn) < lsn)
  {
    if (buf_flush_sync_lsn < lsn)
    {
      buf_flush_sync_lsn= lsn;
      buf_pool.page_cleaner_set_idle(false);
      pthread_cond_signal(&buf_pool.do_flush_list);
    }
    my_cond_wait(&buf_pool.done_flush_list,
                 &buf_pool.flush_list_mutex.m_mutex);
  }
}

/** Wait until all persistent pages are flushed up to a limit.
@param sync_lsn   buf_pool.get_oldest_modification(LSN_MAX) to wait for */
ATTRIBUTE_COLD void buf_flush_wait_flushed(lsn_t sync_lsn)
{
  ut_ad(sync_lsn);
  ut_ad(sync_lsn < LSN_MAX);
  ut_ad(!srv_read_only_mode);

  if (recv_recovery_is_on())
    recv_sys.apply(true);

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  if (buf_pool.get_oldest_modification(sync_lsn) < sync_lsn)
  {
    MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
#if 1 /* FIXME: remove this, and guarantee that the page cleaner serves us */
    if (UNIV_UNLIKELY(!buf_page_cleaner_is_active))
    {
      do
      {
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        ulint n_pages= buf_flush_list(srv_max_io_capacity, sync_lsn);
        buf_flush_wait_batch_end_acquiring_mutex(false);
        if (n_pages)
        {
          MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_SYNC_TOTAL_PAGE,
                                       MONITOR_FLUSH_SYNC_COUNT,
                                       MONITOR_FLUSH_SYNC_PAGES, n_pages);
        }
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
      }
      while (buf_pool.get_oldest_modification(sync_lsn) < sync_lsn);
    }
    else
#endif
    {
      thd_wait_begin(nullptr, THD_WAIT_DISKIO);
      tpool::tpool_wait_begin();
      buf_flush_wait(sync_lsn);
      tpool::tpool_wait_end();
      thd_wait_end(nullptr);
    }
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (UNIV_UNLIKELY(log_sys.last_checkpoint_lsn < sync_lsn))
  {
    /* If the buffer pool was clean, no log write was guaranteed
    to happen until now. There could be an outstanding FILE_CHECKPOINT
    record from a previous fil_names_clear() call, which we must
    write out before we can advance the checkpoint. */
    log_write_up_to(sync_lsn, true);
    log_checkpoint();
  }
}

/** Initiate more eager page flushing if the log checkpoint age is too old.
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target
@param furious  true=furious flushing, false=limit to innodb_io_capacity */
ATTRIBUTE_COLD void buf_flush_ahead(lsn_t lsn, bool furious)
{
  ut_ad(!srv_read_only_mode);

  if (recv_recovery_is_on())
    recv_sys.apply(true);

  Atomic_relaxed<lsn_t> &limit= furious
    ? buf_flush_sync_lsn : buf_flush_async_lsn;

  if (limit < lsn)
  {
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    if (limit < lsn)
    {
      limit= lsn;
      buf_pool.page_cleaner_set_idle(false);
      pthread_cond_signal(&buf_pool.do_flush_list);
    }
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  }
}

/** Wait for pending flushes to complete. */
void buf_flush_wait_batch_end_acquiring_mutex(bool lru)
{
  if (lru ? buf_pool.n_flush_LRU() : buf_pool.n_flush_list())
  {
    mysql_mutex_lock(&buf_pool.mutex);
    buf_flush_wait_batch_end(lru);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
}

/** Conduct checkpoint-related flushing for innodb_flush_sync=ON,
and try to initiate checkpoints until the target is met.
@param lsn   minimum value of buf_pool.get_oldest_modification(LSN_MAX) */
ATTRIBUTE_COLD static void buf_flush_sync_for_checkpoint(lsn_t lsn)
{
  ut_ad(!srv_read_only_mode);

  for (;;)
  {
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (ulint n_flushed= buf_flush_list(srv_max_io_capacity, lsn))
    {
      MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_SYNC_TOTAL_PAGE,
                                   MONITOR_FLUSH_SYNC_COUNT,
                                   MONITOR_FLUSH_SYNC_PAGES, n_flushed);
    }

    switch (srv_file_flush_method) {
    case SRV_NOSYNC:
    case SRV_O_DIRECT_NO_FSYNC:
      break;
    default:
      fil_flush_file_spaces();
    }

    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    const lsn_t newest_lsn= log_sys.get_lsn();
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    lsn_t measure= buf_pool.get_oldest_modification(0);
    const lsn_t checkpoint_lsn= measure ? measure : newest_lsn;

    if (!recv_recovery_is_on() &&
        checkpoint_lsn > log_sys.last_checkpoint_lsn + SIZE_OF_FILE_CHECKPOINT)
    {
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      log_checkpoint_low(checkpoint_lsn, newest_lsn);
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      measure= buf_pool.get_oldest_modification(LSN_MAX);
    }
    else
    {
      log_sys.latch.wr_unlock();
      if (!measure)
        measure= LSN_MAX;
    }

    /* After attempting log checkpoint, check if we have reached our target. */
    const lsn_t target= buf_flush_sync_lsn;

    if (measure >= target)
      buf_flush_sync_lsn= 0;
    else if (measure >= buf_flush_async_lsn)
      buf_flush_async_lsn= 0;

    /* wake up buf_flush_wait() */
    pthread_cond_broadcast(&buf_pool.done_flush_list);

    lsn= std::max(lsn, target);

    if (measure >= lsn)
      return;
  }
}

/** Check if the adpative flushing threshold is recommended based on
redo log capacity filled threshold.
@param oldest_lsn     buf_pool.get_oldest_modification()
@return true if adaptive flushing is recommended. */
static bool af_needed_for_redo(lsn_t oldest_lsn)
{
  lsn_t age= (log_sys.get_lsn() - oldest_lsn);
  lsn_t af_lwm= static_cast<lsn_t>(srv_adaptive_flushing_lwm *
    static_cast<double>(log_sys.log_capacity) / 100);

  /* if age > af_lwm adaptive flushing is recommended */
  return (age > af_lwm);
}

/*********************************************************************//**
Calculates if flushing is required based on redo generation rate.
@return percent of io_capacity to flush to manage redo space */
static
ulint
af_get_pct_for_lsn(
/*===============*/
	lsn_t	age)	/*!< in: current age of LSN. */
{
	lsn_t	af_lwm = static_cast<lsn_t>(
		srv_adaptive_flushing_lwm
		* static_cast<double>(log_sys.log_capacity) / 100);

	if (age < af_lwm) {
		/* No adaptive flushing. */
		return(0);
	}

	lsn_t lsn_age_factor = (age * 100) / log_sys.max_modified_age_async;

	ut_ad(srv_max_io_capacity >= srv_io_capacity);
	return static_cast<ulint>(
		(static_cast<double>(srv_max_io_capacity / srv_io_capacity
				     * lsn_age_factor)
		 * sqrt(static_cast<double>(lsn_age_factor))
		 / 7.5));
}

/** This function is called approximately once every second by the
page_cleaner thread if innodb_adaptive_flushing=ON.
Based on various factors it decides if there is a need to do flushing.
@return number of pages recommended to be flushed
@param last_pages_in  number of pages flushed in previous batch
@param oldest_lsn     buf_pool.get_oldest_modification(0)
@param dirty_blocks   UT_LIST_GET_LEN(buf_pool.flush_list)
@param dirty_pct      100*flush_list.count / (LRU.count + free.count) */
static ulint page_cleaner_flush_pages_recommendation(ulint last_pages_in,
                                                     lsn_t oldest_lsn,
                                                     ulint dirty_blocks,
                                                     double dirty_pct)
{
	static	lsn_t		prev_lsn = 0;
	static	ulint		sum_pages = 0;
	static	ulint		avg_page_rate = 0;
	static	ulint		n_iterations = 0;
	static	time_t		prev_time;
	lsn_t			lsn_rate;
	ulint			n_pages = 0;

	const lsn_t cur_lsn = log_sys.get_lsn();
	ut_ad(oldest_lsn <= cur_lsn);
	ulint pct_for_lsn = af_get_pct_for_lsn(cur_lsn - oldest_lsn);
	time_t curr_time = time(nullptr);
	const double max_pct = srv_max_buf_pool_modified_pct;

	if (!prev_lsn || !pct_for_lsn) {
		prev_time = curr_time;
		prev_lsn = cur_lsn;
		if (max_pct > 0.0) {
			dirty_pct /= max_pct;
		}

		n_pages = ulint(dirty_pct * double(srv_io_capacity));
		if (n_pages < dirty_blocks) {
			n_pages= std::min<ulint>(srv_io_capacity, dirty_blocks);
		}

		return n_pages;
	}

	sum_pages += last_pages_in;

	double	time_elapsed = difftime(curr_time, prev_time);

	/* We update our variables every srv_flushing_avg_loops
	iterations to smooth out transition in workload. */
	if (++n_iterations >= srv_flushing_avg_loops
	    || time_elapsed >= static_cast<double>(srv_flushing_avg_loops)) {

		if (time_elapsed < 1) {
			time_elapsed = 1;
		}

		avg_page_rate = static_cast<ulint>(
			((static_cast<double>(sum_pages)
			  / time_elapsed)
			 + static_cast<double>(avg_page_rate)) / 2);

		/* How much LSN we have generated since last call. */
		lsn_rate = static_cast<lsn_t>(
			static_cast<double>(cur_lsn - prev_lsn)
			/ time_elapsed);

		lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;

		ulint	flush_tm = page_cleaner.flush_time;
		ulint	flush_pass = page_cleaner.flush_pass;

		page_cleaner.flush_time = 0;
		page_cleaner.flush_pass = 0;

		if (flush_pass) {
			flush_tm /= flush_pass;
		}

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME, flush_tm);
		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_PASS, flush_pass);

		prev_lsn = cur_lsn;
		prev_time = curr_time;

		n_iterations = 0;

		sum_pages = 0;
	}

	const ulint pct_for_dirty = srv_max_dirty_pages_pct_lwm == 0
		? (dirty_pct >= max_pct ? 100 : 0)
		: static_cast<ulint>
		(max_pct > 0.0 ? dirty_pct / max_pct : dirty_pct);
	ulint pct_total = std::max(pct_for_dirty, pct_for_lsn);

	/* Estimate pages to be flushed for the lsn progress */
	lsn_t	target_lsn = oldest_lsn
		+ lsn_avg_rate * buf_flush_lsn_scan_factor;
	ulint	pages_for_lsn = 0;

	mysql_mutex_lock(&buf_pool.flush_list_mutex);

	for (buf_page_t* b = UT_LIST_GET_LAST(buf_pool.flush_list);
	     b != NULL;
	     b = UT_LIST_GET_PREV(list, b)) {
		if (b->oldest_modification() > target_lsn) {
			break;
		}
		if (++pages_for_lsn >= srv_max_io_capacity) {
			break;
		}
	}
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

	pages_for_lsn /= buf_flush_lsn_scan_factor;
	if (pages_for_lsn < 1) {
		pages_for_lsn = 1;
	}

	n_pages = (ulint(double(srv_io_capacity) * double(pct_total) / 100.0)
		   + avg_page_rate + pages_for_lsn) / 3;

	if (n_pages > srv_max_io_capacity) {
		n_pages = srv_max_io_capacity;
	}

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_REQUESTED, n_pages);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_BY_AGE, pages_for_lsn);

	MONITOR_SET(MONITOR_FLUSH_AVG_PAGE_RATE, avg_page_rate);
	MONITOR_SET(MONITOR_FLUSH_LSN_AVG_RATE, lsn_avg_rate);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_DIRTY, pct_for_dirty);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_LSN, pct_for_lsn);

	return(n_pages);
}

#if defined __aarch64__&&defined __GNUC__&&__GNUC__==4&&!defined __clang__
/* Avoid GCC 4.8.5 internal compiler error "could not split insn".
We would only need this for buf_flush_page_cleaner(),
but GCC 4.8.5 does not support pop_options. */
# pragma GCC optimize ("O0")
#endif
/** page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator. */
static void buf_flush_page_cleaner()
{
  my_thread_init();
#ifdef UNIV_PFS_THREAD
  pfs_register_thread(page_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */
  ut_ad(!srv_read_only_mode);
  ut_ad(buf_page_cleaner_is_active);

  ulint last_pages= 0;
  timespec abstime;
  set_timespec(abstime, 1);

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  lsn_t lsn_limit;
  ulint last_activity_count= srv_get_activity_count();

  for (;;)
  {
    lsn_limit= buf_flush_sync_lsn;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
furious_flush:
      if (UNIV_LIKELY(srv_flush_sync))
      {
        buf_flush_sync_for_checkpoint(lsn_limit);
        last_pages= 0;
        set_timespec(abstime, 1);
        continue;
      }
    }
    else if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED)
      break;

    /* If buf pager cleaner is idle and there is no work
    (either dirty pages are all flushed or adaptive flushing
    is not enabled) then opt for non-timed wait */
    if (buf_pool.page_cleaner_idle() &&
        (!UT_LIST_GET_LEN(buf_pool.flush_list) ||
         srv_max_dirty_pages_pct_lwm == 0.0))
      my_cond_wait(&buf_pool.do_flush_list, &buf_pool.flush_list_mutex.m_mutex);
    else
      my_cond_timedwait(&buf_pool.do_flush_list,
                        &buf_pool.flush_list_mutex.m_mutex, &abstime);

    set_timespec(abstime, 1);

    lsn_t soft_lsn_limit= buf_flush_async_lsn;
    lsn_limit= buf_flush_sync_lsn;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
      if (UNIV_LIKELY(srv_flush_sync))
        goto furious_flush;
    }
    else if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED)
      break;

    const lsn_t oldest_lsn= buf_pool.get_oldest_modification(0);

    if (!oldest_lsn)
    {
      if (UNIV_UNLIKELY(lsn_limit != 0))
      {
        buf_flush_sync_lsn= 0;
        /* wake up buf_flush_wait() */
        pthread_cond_broadcast(&buf_pool.done_flush_list);
      }
unemployed:
      buf_flush_async_lsn= 0;
      buf_pool.page_cleaner_set_idle(true);

      DBUG_EXECUTE_IF("ib_log_checkpoint_avoid", continue;);

      mysql_mutex_unlock(&buf_pool.flush_list_mutex);

      if (!recv_recovery_is_on() &&
          !srv_startup_is_before_trx_rollback_phase &&
          srv_operation == SRV_OPERATION_NORMAL)
        log_checkpoint();

      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      continue;
    }

    const ulint dirty_blocks= UT_LIST_GET_LEN(buf_pool.flush_list);
    ut_ad(dirty_blocks);
    /* We perform dirty reads of the LRU+free list lengths here.
    Division by zero is not possible, because buf_pool.flush_list is
    guaranteed to be nonempty, and it is a subset of buf_pool.LRU. */
    const double dirty_pct= double(dirty_blocks) * 100.0 /
      double(UT_LIST_GET_LEN(buf_pool.LRU) + UT_LIST_GET_LEN(buf_pool.free));

    bool idle_flush= false;

    if (lsn_limit || soft_lsn_limit);
    else if (af_needed_for_redo(oldest_lsn));
    else if (srv_max_dirty_pages_pct_lwm != 0.0)
    {
      const ulint activity_count= srv_get_activity_count();
      if (activity_count != last_activity_count)
        last_activity_count= activity_count;
      else if (buf_pool.page_cleaner_idle() && buf_pool.n_pend_reads == 0)
      {
         /* reaching here means 3 things:
         - last_activity_count == activity_count: suggesting server is idle
           (no trx_t::commit activity)
         - page cleaner is idle (dirty_pct < srv_max_dirty_pages_pct_lwm)
         - there are no pending reads but there are dirty pages to flush */
        idle_flush= true;
        buf_pool.update_last_activity_count(activity_count);
      }

      if (!idle_flush && dirty_pct < srv_max_dirty_pages_pct_lwm)
        goto unemployed;
    }
    else if (dirty_pct < srv_max_buf_pool_modified_pct)
      goto unemployed;

    if (UNIV_UNLIKELY(lsn_limit != 0) && oldest_lsn >= lsn_limit)
      lsn_limit= buf_flush_sync_lsn= 0;
    if (UNIV_UNLIKELY(soft_lsn_limit != 0) && oldest_lsn >= soft_lsn_limit)
      soft_lsn_limit= buf_flush_async_lsn= 0;

    buf_pool.page_cleaner_set_idle(false);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (!lsn_limit)
      lsn_limit= soft_lsn_limit;

    ulint n_flushed;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
      n_flushed= buf_flush_list(srv_max_io_capacity, lsn_limit);
      /* wake up buf_flush_wait() */
      pthread_cond_broadcast(&buf_pool.done_flush_list);
      goto try_checkpoint;
    }
    else if (idle_flush || !srv_adaptive_flushing)
    {
      n_flushed= buf_flush_list(srv_io_capacity);
try_checkpoint:
      if (n_flushed)
      {
        MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BACKGROUND_TOTAL_PAGE,
                                     MONITOR_FLUSH_BACKGROUND_COUNT,
                                     MONITOR_FLUSH_BACKGROUND_PAGES,
                                     n_flushed);
do_checkpoint:
        /* The periodic log_checkpoint() call here makes it harder to
        reproduce bugs in crash recovery or mariabackup --prepare, or
        in code that writes the redo log records. Omitting the call
        here should not affect correctness, because log_free_check()
        should still be invoking checkpoints when needed. */
        DBUG_EXECUTE_IF("ib_log_checkpoint_avoid", goto next;);

        if (!recv_recovery_is_on() && srv_operation == SRV_OPERATION_NORMAL)
          log_checkpoint();
      }
    }
    else if (ulint n= page_cleaner_flush_pages_recommendation(last_pages,
                                                              oldest_lsn,
                                                              dirty_blocks,
                                                              dirty_pct))
    {
      page_cleaner.flush_pass++;
      const ulint tm= ut_time_ms();
      last_pages= n_flushed= buf_flush_list(n);
      page_cleaner.flush_time+= ut_time_ms() - tm;

      if (n_flushed)
      {
        MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_ADAPTIVE_TOTAL_PAGE,
                                     MONITOR_FLUSH_ADAPTIVE_COUNT,
                                     MONITOR_FLUSH_ADAPTIVE_PAGES,
                                     n_flushed);
        goto do_checkpoint;
      }
    }
    else if (buf_flush_async_lsn <= oldest_lsn)
    {
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      goto unemployed;
    }

#ifndef DBUG_OFF
next:
#endif /* !DBUG_OFF */
    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    /* when idle flushing kicks in page_cleaner is marked active.
    reset it back to idle since the it was made active as part of
    idle flushing stage. */
    if (idle_flush)
      buf_pool.page_cleaner_set_idle(true);
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (srv_fast_shutdown != 2)
  {
    buf_flush_wait_batch_end_acquiring_mutex(true);
    buf_flush_wait_batch_end_acquiring_mutex(false);
  }

  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  lsn_limit= buf_flush_sync_lsn;
  if (UNIV_UNLIKELY(lsn_limit != 0))
    goto furious_flush;
  buf_page_cleaner_is_active= false;
  pthread_cond_broadcast(&buf_pool.done_flush_list);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  my_thread_end();

#ifdef UNIV_PFS_THREAD
  pfs_delete_thread();
#endif
}

/** Initialize page_cleaner. */
ATTRIBUTE_COLD void buf_flush_page_cleaner_init()
{
  ut_ad(!buf_page_cleaner_is_active);
  ut_ad(srv_operation == SRV_OPERATION_NORMAL ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_EXPORT);
  buf_flush_async_lsn= 0;
  buf_flush_sync_lsn= 0;
  buf_page_cleaner_is_active= true;
  std::thread(buf_flush_page_cleaner).detach();
}

#if defined(HAVE_SYSTEMD) && !defined(EMBEDDED_LIBRARY)
/** @return the number of dirty pages in the buffer pool */
static ulint buf_flush_list_length()
{
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  const ulint len= UT_LIST_GET_LEN(buf_pool.flush_list);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  return len;
}
#endif

/** Flush the buffer pool on shutdown. */
ATTRIBUTE_COLD void buf_flush_buffer_pool()
{
  ut_ad(!buf_page_cleaner_is_active);
  ut_ad(!buf_flush_sync_lsn);

  service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                 "Waiting to flush the buffer pool");

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  while (buf_pool.get_oldest_modification(0))
  {
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    buf_flush_list(srv_max_io_capacity);
    if (buf_pool.n_flush_list())
    {
      timespec abstime;
      service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                     "Waiting to flush " ULINTPF " pages",
                                     buf_flush_list_length());
      set_timespec(abstime, INNODB_EXTEND_TIMEOUT_INTERVAL / 2);
      mysql_mutex_lock(&buf_pool.mutex);
      while (buf_pool.n_flush_list_)
        my_cond_timedwait(&buf_pool.done_flush_list, &buf_pool.mutex.m_mutex,
                          &abstime);
      mysql_mutex_unlock(&buf_pool.mutex);
    }
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  ut_ad(!buf_pool.any_io_pending());
}

/** Synchronously flush dirty blocks during recv_sys_t::apply().
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync_batch(lsn_t lsn)
{
  thd_wait_begin(nullptr, THD_WAIT_DISKIO);
  tpool::tpool_wait_begin();
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_wait(lsn);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  tpool::tpool_wait_end();
  thd_wait_end(nullptr);
}

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync()
{
  if (recv_recovery_is_on())
  {
    mysql_mutex_lock(&recv_sys.mutex);
    recv_sys.apply(true);
    mysql_mutex_unlock(&recv_sys.mutex);
  }

  thd_wait_begin(nullptr, THD_WAIT_DISKIO);
  tpool::tpool_wait_begin();
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  for (;;)
  {
    const lsn_t lsn= log_sys.get_lsn();
    buf_flush_wait(lsn);
    /* Wait for the page cleaner to be idle (for log resizing at startup) */
    while (buf_flush_sync_lsn)
      my_cond_wait(&buf_pool.done_flush_list,
                   &buf_pool.flush_list_mutex.m_mutex);
    if (lsn == log_sys.get_lsn())
      break;
  }
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  tpool::tpool_wait_end();
  thd_wait_end(nullptr);
}

#ifdef UNIV_DEBUG
/** Functor to validate the flush list. */
struct	Check {
	void operator()(const buf_page_t* elem) const
	{
		ut_ad(elem->oldest_modification());
		ut_ad(!fsp_is_system_temporary(elem->id().space()));
	}
};

/** Validate the flush list. */
static void buf_flush_validate_low()
{
	buf_page_t*		bpage;

	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

	ut_list_validate(buf_pool.flush_list, Check());

	bpage = UT_LIST_GET_FIRST(buf_pool.flush_list);

	while (bpage != NULL) {
		const lsn_t	om = bpage->oldest_modification();
		/* A page in buf_pool.flush_list can be in
		BUF_BLOCK_REMOVE_HASH state. This happens when a page
		is in the middle of being relocated. In that case the
		original descriptor can have this state and still be
		in the flush list waiting to acquire the
		buf_pool.flush_list_mutex to complete the relocation. */
		ut_d(const auto s= bpage->state());
		ut_ad(s >= buf_page_t::REMOVE_HASH);
		ut_ad(om == 1 || om > 2);

		bpage = UT_LIST_GET_NEXT(list, bpage);
		ut_ad(om == 1 || !bpage || recv_recovery_is_on()
		      || om >= bpage->oldest_modification());
	}
}

/** Validate the flush list. */
void buf_flush_validate()
{
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_validate_low();
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}
#endif /* UNIV_DEBUG */
