/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2023, MariaDB Corporation.
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
#include "buf0lru.h"
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
#include "scope.h"

/** Number of pages flushed via LRU. Protected by buf_pool.mutex.
Also included in buf_pool.stat.n_pages_written. */
ulint buf_lru_flush_page_count;

/** Number of pages freed without flushing. Protected by buf_pool.mutex. */
ulint buf_lru_freed_page_count;

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
static void buf_flush_validate_low() noexcept;

/** Validates the flush list some of the time. */
static void buf_flush_validate_skip() noexcept
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

void buf_pool_t::page_cleaner_wakeup(bool for_LRU) noexcept
{
  ut_d(buf_flush_validate_skip());
  if (!page_cleaner_idle())
  {
    if (for_LRU)
      /* Ensure that the page cleaner is not in a timed wait. */
      pthread_cond_signal(&do_flush_list);
    return;
  }
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
  if (for_LRU ||
      (pct_lwm != 0.0 && (pct_lwm <= dirty_pct ||
                          last_activity_count == srv_get_activity_count())) ||
      srv_max_buf_pool_modified_pct <= dirty_pct)
  {
    page_cleaner_status-= PAGE_CLEANER_IDLE;
    pthread_cond_signal(&do_flush_list);
  }
}

/** Remove a block from flush_list.
@param bpage   buffer pool page */
void buf_pool_t::delete_from_flush_list(buf_page_t *bpage) noexcept
{
  ut_ad(!fsp_is_system_temporary(bpage->id().space()));
  mysql_mutex_assert_owner(&flush_list_mutex);
  flush_hp.adjust(bpage);
  UT_LIST_REMOVE(flush_list, bpage);
  flush_list_bytes-= bpage->physical_size();
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
void buf_flush_remove_pages(uint32_t id) noexcept
{
  const page_id_t first(id, 0), end(id + 1, 0);
  ut_ad(id);

  for (;;)
  {
    mysql_mutex_lock(&buf_pool.mutex);
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

    mysql_mutex_unlock(&buf_pool.mutex);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (!deferred)
      break;

    os_aio_wait_until_no_pending_writes(true);
  }
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
  noexcept
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
		buf_pool.flush_list_bytes -= dpage->physical_size();
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

void buf_page_t::write_complete(space_type type, bool error,
                                uint32_t state) noexcept
{
  ut_ad(type == EXT_BUF ||
        (type == TEMPORARY) == fsp_is_system_temporary(id().space()));
  ut_ad(state >= WRITE_FIX);
  ut_ad(!frame ||
        frame == reinterpret_cast<buf_block_t*>(this)->frame_address());

  if (UNIV_LIKELY(!error))
  {
    bool persistent = (type == PERSISTENT);
    ut_d(lsn_t om= oldest_modification());
    ut_ad(type == EXT_BUF || om >= 2);
    ut_ad(persistent == (om > 2));
    ut_ad(type != EXT_BUF || !oldest_modification());
    /* We use release memory order to guarantee that callers of
    oldest_modification_acquire() will observe the block as
    being detached from buf_pool.flush_list, after reading the value 0. */
    if (type != EXT_BUF)
      oldest_modification_.store(persistent, std::memory_order_release);
  }
  zip.fix.fetch_sub((state >= WRITE_FIX_REINIT)
                    ? (WRITE_FIX_REINIT - UNFIXED)
                    : (WRITE_FIX - UNFIXED));
  lock.u_unlock(true);
}

inline void buf_pool_t::n_flush_inc() noexcept
{
  mysql_mutex_assert_owner(&flush_list_mutex);
  page_cleaner_status+= LRU_FLUSH;
}

inline void buf_pool_t::n_flush_dec() noexcept
{
  mysql_mutex_assert_owner(&flush_list_mutex);
  ut_ad(page_cleaner_status >= LRU_FLUSH);
  if ((page_cleaner_status-= LRU_FLUSH) < LRU_FLUSH)
    pthread_cond_broadcast(&done_flush_LRU);
}

/** Complete write of a file page from buf_pool.
@param request write request
@param error   whether the write may have failed */
void buf_page_write_complete(const IORequest &request, bool error) noexcept
{
  ut_ad(request.is_write());
  ut_ad(!srv_read_only_mode);
  buf_page_t *bpage= request.bpage();
  ut_ad(bpage);
  const auto state= bpage->state();
  /* io-fix can only be cleared by buf_page_t::write_complete()
  and buf_page_t::read_complete() */
  ut_ad(state >= buf_page_t::WRITE_FIX);
  ut_ad(!buf_dblwr.is_inside(bpage->id()));
  ut_ad(request.ext_buf() || request.node()->space->id == bpage->id().space());

  if (request.slot)
    request.slot->release();

  if (UNIV_UNLIKELY(MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)))
    buf_page_monitor(*bpage, false);
  DBUG_PRINT("ib_buf", ("write page %u:%u",
                        bpage->id().space(), bpage->id().page_no()));

  mysql_mutex_assert_not_owner(&buf_pool.mutex);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);

  buf_page_t::space_type type= request.ext_buf()
                                   ? buf_page_t::EXT_BUF
                                   : static_cast<buf_page_t::space_type>(
                                         bpage->oldest_modification() == 2);

  if (UNIV_UNLIKELY(type != buf_page_t::PERSISTENT) && UNIV_LIKELY(!error))
  {
    if (type == buf_page_t::EXT_BUF)
    {
      ut_d(if (DBUG_IF("ib_ext_bp_count_io_only_for_t")) {
        if (fil_space_t *space= fil_space_t::get(bpage->id_.space()))
        {
          auto space_name= space->name();
          if (fil_page_get_type(bpage->frame) == FIL_PAGE_INDEX &&
              space_name.data() &&
              !strncmp(space_name.data(), "test/t.ibd", space_name.size()))
          {
            ++buf_pool.stat.n_pages_written_to_ebp;
          }
          space->release();
        }
      } else)
        ++buf_pool.stat.n_pages_written_to_ebp;
    }
    /* We must hold buf_pool.mutex while releasing the block, so that
    no other thread can access it before we have freed it. */
    mysql_mutex_lock(&buf_pool.mutex);
    bpage->write_complete(type, error, state);
    buf_LRU_free_page(bpage, true,
                      request.ext_buf() ? request.ext_buf_page() : nullptr);
    mysql_mutex_unlock(&buf_pool.mutex);
  }
  else
  {
    if (error && type == buf_page_t::EXT_BUF &&
        fil_system.ext_buf_pool_enabled())
    {
      sql_print_warning("InnoDB: There was IO error during writing to "
                        "external buffer pool file, external buffer pool is "
                          "disabled.");
        fil_system.ext_buf_pool_disable();
    }
    bpage->write_complete(type, error, state);
    if (request.is_doublewritten())
    {
      ut_ad(state < buf_page_t::WRITE_FIX_REINIT);
      ut_ad(type == buf_page_t::PERSISTENT);
      buf_dblwr.write_completed();
    }
  }
}

/** Calculate a ROW_FORMAT=COMPRESSED page checksum and update the page.
@param[in,out]	page		page to update
@param[in]	size		compressed page size */
void buf_flush_update_zip_checksum(buf_frame_t *page, ulint size) noexcept
{
  ut_ad(size > 0);
  mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
                  page_zip_calc_checksum(page, size, false));
}

/** Assign the full crc32 checksum for non-compressed page.
@param[in,out]	page	page to be updated */
void buf_flush_assign_full_crc32_checksum(byte* page) noexcept
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
	bool			use_full_checksum) noexcept
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
		uint16_t page_type = fil_page_get_type(page);
		uint16_t reset_type = page_type;

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
static void buf_tmp_reserve_compression_buf(buf_tmp_buffer_t* slot) noexcept
{
  if (slot->comp_buf)
    return;
  /* Both Snappy and LZO compression methods require that the output
  buffer be bigger than input buffer. Adjust the allocated size. */
  ulint size= srv_page_size;
  if (provider_service_lzo->is_loaded)
    size= LZO1X_1_15_MEM_COMPRESS;
  else if (provider_service_snappy->is_loaded)
    size= snappy_max_compressed_length(size);
  slot->comp_buf= static_cast<byte*>(aligned_malloc(size, srv_page_size));
}

/** Encrypt a buffer of temporary tablespace
@param[in]      offset  Page offset
@param[in]      s       Page to encrypt
@param[in,out]  d       Output buffer
@return encrypted buffer or NULL */
static byte *buf_tmp_page_encrypt(ulint offset, const byte *s, byte *d)
  noexcept
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
static byte *buf_page_encrypt(fil_space_t *space, buf_page_t *bpage, byte *s,
                              buf_tmp_buffer_t **slot, size_t *size) noexcept
{
  ut_ad(!bpage->is_freed());
  ut_ad(space->id == bpage->id().space());
  ut_ad(!*slot);

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
  if (space->is_temporary())
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
  *slot= buf_pool.io_buf_reserve(true);
  ut_a(*slot);
  (*slot)->allocate();

  byte *d= (*slot)->crypt_buf;

  if (!page_compressed)
  {
not_compressed:
    d= space->is_temporary()
      ? buf_tmp_page_encrypt(page_no, s, d)
      : fil_space_encrypt(space, page_no, s, d);
  }
  else
  {
    ut_ad(!space->is_temporary());
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

    if (encrypted)
      tmp= fil_space_encrypt(space, page_no, tmp, d);

    if (full_crc32)
    {
      static_assert(FIL_PAGE_FCRC32_CHECKSUM == 4, "alignment");
      mach_write_to_4(tmp + len - 4, my_crc32c(0, tmp, len - 4));
    }

    d= tmp;
  }

  (*slot)->out_buf= d;
  return d;
}

/** Free a page whose underlying file page has been freed. */
ATTRIBUTE_COLD void buf_pool_t::release_freed_page(buf_page_t *bpage) noexcept
{
  mysql_mutex_assert_owner(&mutex);
  ut_d(const lsn_t oldest_modification= bpage->oldest_modification();)
  if (fsp_is_system_temporary(bpage->id().space()))
  {
    ut_ad(bpage->frame);
    ut_ad(oldest_modification == 2);
    bpage->clear_oldest_modification();
  }
  else
  {
    mysql_mutex_lock(&flush_list_mutex);
    ut_ad(oldest_modification > 2);
    delete_from_flush_list(bpage);
    mysql_mutex_unlock(&flush_list_mutex);
  }

  bpage->lock.u_unlock(true);
  buf_LRU_free_page(bpage, true);
}

/** Write a flushable page to a file or free a freeable block.
@param space       tablespace
@param to_ext_buf  wherher to write the page to external buffer pull file
@return whether a page write was initiated and buf_pool.mutex released */
bool buf_page_t::flush(fil_space_t *space, bool to_ext_buf) noexcept
{
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  ut_ad(in_file());
  ut_ad(in_LRU_list);
  ut_ad((space->is_temporary()) == (space == fil_system.temp_space));
  ut_ad(space->referenced());

  const auto s= state();

  const lsn_t lsn=
    mach_read_from_8(my_assume_aligned<8>
                     (FIL_PAGE_LSN + (zip.data ? zip.data : frame)));
  ut_ad(to_ext_buf ||
        (lsn ? lsn >= oldest_modification() || oldest_modification() == 2
             : (space->is_temporary() || space->is_being_imported())));

  if (s < UNFIXED)
  {
    ut_a(s >= FREED);
    if (to_ext_buf)
      return false;
    if (!space->is_temporary() && !space->is_being_imported())
    {
    freed:
      if (lsn > log_sys.get_flushed_lsn())
      {
        mysql_mutex_unlock(&buf_pool.mutex);
        log_write_up_to(lsn, true);
        mysql_mutex_lock(&buf_pool.mutex);
      }
    }
    buf_pool.release_freed_page(this);
    return false;
  }

  if (UNIV_UNLIKELY(lsn < space->get_create_lsn()))
  {
    ut_ad(!space->is_temporary());
    ut_ad(!space->is_being_imported());
    if (to_ext_buf)
      return false;
    goto freed;
  }

  ext_buf_page_t *ext_page= nullptr;
  if (to_ext_buf && !(ext_page= buf_pool.alloc_ext_page(id())))
    return false;

  ut_d(const auto f=) zip.fix.fetch_add(WRITE_FIX - UNFIXED);
  ut_ad(f >= UNFIXED);
  ut_ad(f < READ_FIX);
  ut_ad(to_ext_buf || ((space == fil_system.temp_space)
        ? oldest_modification() == 2
        : oldest_modification() > 2));

  /* Increment the I/O operation count used for selecting LRU policy. */
  buf_LRU_stat_inc_io();
  mysql_mutex_unlock(&buf_pool.mutex);

  IORequest::Type type= IORequest::WRITE_ASYNC;

  /* Apart from the U-lock, this block will also be protected by
  is_write_fixed() and oldest_modification()>1.
  Thus, it cannot be relocated or removed. */

  buf_block_t *block= reinterpret_cast<buf_block_t*>(this);
  page_t *write_frame= zip.data;

  if (!to_ext_buf)
    space->reacquire();
  size_t size;
#if defined HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE || defined _WIN32
  size_t orig_size;
#endif
  buf_tmp_buffer_t *slot= nullptr;
  byte *page= frame;

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
        type= IORequest::PUNCH;
        break;
      case 2:
        size= orig_size;
      }
    }
#endif
    write_frame= page;
  }

  if (to_ext_buf) {
    ut_ad(ext_page);
    fil_system.ext_bp_io(*this, *ext_page, IORequest::WRITE_ASYNC, slot, size,
                         write_frame);
  }
  else if ((s & LRU_MASK) == REINIT || !space->use_doublewrite())
  {
    if (!space->is_temporary() && !space->is_being_imported() &&
        lsn > log_sys.get_flushed_lsn())
      log_write_up_to(lsn, true);
    ut_ad(space->is_temporary() || !space->full_crc32() ||
          !buf_page_is_corrupted(true, write_frame, space->flags));
    space->io(IORequest{type, this, slot}, physical_offset(), size,
              write_frame, this);
  }
  else
    buf_dblwr.add_to_batch(IORequest{this, slot, space->chain.start, type},
                           size);
  return true;
}

/** Check whether a page can be flushed from the buf_pool.
@param id          page identifier
@param fold        id.fold()
@return whether the page can be flushed */
static bool buf_flush_check_neighbor(const page_id_t id, ulint fold) noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  ut_ad(fold == id.fold());

  /* FIXME: cell_get() is being invoked while holding buf_pool.mutex */
  const buf_page_t *bpage=
    buf_pool.page_hash.get(id, buf_pool.page_hash.cell_get(fold));

  return bpage && bpage->oldest_modification() > 1 && !bpage->is_io_fixed();
}

/** Check which neighbors of a page can be flushed from the buf_pool.
@param space       tablespace
@param id          page identifier of a dirty page
@param contiguous  whether to consider contiguous areas of pages
@return last page number that can be flushed */
static page_id_t buf_flush_check_neighbors(const fil_space_t &space,
                                           page_id_t &id, bool contiguous)
  noexcept
{
  ut_ad(id.page_no() < space.size +
        (space.physical_size() == 2048 ? 1
         : space.physical_size() == 1024 ? 3 : 0));
  /* When flushed, dirty blocks are searched in neighborhoods of this
  size, and flushed along with the original page. */
  const ulint s= buf_pool.curr_size() / 16;
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
      if (!buf_flush_check_neighbor(i, fold))
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
    if (!buf_flush_check_neighbor(i, fold))
      break;
  }

  mysql_mutex_unlock(&buf_pool.mutex);
  return i;
}

MY_ATTRIBUTE((warn_unused_result))
/** Apply freed_ranges to the file.
@param writable whether the file is writable
@return number of pages written or hole-punched */
uint32_t fil_space_t::flush_freed(bool writable) noexcept
{
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  mysql_mutex_assert_not_owner(&buf_pool.mutex);

  const bool punch_hole= chain.start->punch_hole == 1;
  if (!punch_hole && !srv_immediate_scrub_data_uncompressed)
    return 0;
  if (srv_is_undo_tablespace(id))
    /* innodb_undo_log_truncate=ON can take care of these better */
    return 0;

  for (;;)
  {
    freed_range_mutex.lock();
    if (freed_ranges.empty())
    {
      freed_range_mutex.unlock();
      return 0;
    }
    const lsn_t flush_lsn= last_freed_lsn;
    if (log_sys.get_flushed_lsn() >= flush_lsn)
      break;
    freed_range_mutex.unlock();
    log_write_up_to(flush_lsn, true);
  }

  const unsigned physical{physical_size()};

  range_set freed= std::move(freed_ranges);
  uint32_t written= 0;

  if (!writable);
  else if (punch_hole)
  {
    for (const auto &range : freed)
    {
      written+= range.last - range.first + 1;
      reacquire();
      io(IORequest(IORequest::PUNCH_RANGE),
         os_offset_t{range.first} * physical,
         (range.last - range.first + 1) * physical, nullptr);
    }
  }
  else
  {
    for (const auto &range : freed)
    {
      written+= range.last - range.first + 1;
      for (os_offset_t i= range.first; i <= range.last; i++)
      {
        reacquire();
        io(IORequest(IORequest::WRITE_ASYNC), i * physical, physical,
           const_cast<byte*>(field_ref_zero));
      }
    }
  }

  freed_range_mutex.unlock();
  return written;
}

/** Flushes to disk all flushable pages within the flush area
and also write zeroes or punch the hole for the freed ranges of pages.
@param space       tablespace
@param page_id     page identifier
@param bpage       buffer page
@param contiguous  whether to consider contiguous areas of pages
@param n_flushed   number of pages flushed so far in this batch
@param n_to_flush  maximum number of pages we are allowed to flush
@return number of pages flushed */
static ulint buf_flush_try_neighbors(fil_space_t *space,
                                     const page_id_t page_id,
                                     buf_page_t *bpage,
                                     bool contiguous,
                                     ulint n_flushed,
                                     ulint n_to_flush) noexcept
{
  ut_ad(space->id == page_id.space());
  ut_ad(bpage->id() == page_id);

  {
    const lsn_t lsn=
      mach_read_from_8(my_assume_aligned<8>
                       (FIL_PAGE_LSN +
                        (bpage->zip.data ? bpage->zip.data : bpage->frame)));
    ut_ad(lsn >= bpage->oldest_modification());
    if (UNIV_UNLIKELY(lsn < space->get_create_lsn()))
    {
      ut_a(!bpage->flush(space));
      mysql_mutex_unlock(&buf_pool.mutex);
      return 0;
    }
  }

  mysql_mutex_unlock(&buf_pool.mutex);

  ulint count= 0;
  page_id_t id= page_id;
  page_id_t high= buf_flush_check_neighbors(*space, id, contiguous);

  ut_ad(page_id >= id);
  ut_ad(page_id < high);

  for (ulint id_fold= id.fold(); id < high; ++id, ++id_fold)
  {
    if (UNIV_UNLIKELY(space->is_stopping_writes()))
    {
      if (bpage)
        bpage->lock.u_unlock(true);
      break;
    }

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

    if (buf_page_t *b= buf_pool.page_hash.get(id, chain))
    {
      ut_ad(b->in_file());
      if (id == page_id)
      {
        ut_ad(bpage == b);
        bpage= nullptr;
        ut_ad(b->oldest_modification() > 1);
      flush:
        if (b->flush(space))
        {
          ++count;
          continue;
        }
      }
      /* We avoid flushing 'non-old' blocks in an eviction flush,
      because the flushed blocks are soon freed */
      else if (b->oldest_modification() > 1 && b->lock.u_lock_try(true))
      {
        /* For the buf_pool.watch[] sentinels, oldest_modification() == 0 */
        if (b->oldest_modification() < 2)
          b->lock.u_unlock(true);
        else
          goto flush;
      }
    }

    mysql_mutex_unlock(&buf_pool.mutex);
  }

  if (count > 1)
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_NEIGHBOR_TOTAL_PAGE,
                                 MONITOR_FLUSH_NEIGHBOR_COUNT,
                                 MONITOR_FLUSH_NEIGHBOR_PAGES, count - 1);
  }

  return count;
}

/*******************************************************************//**
This utility moves the uncompressed frames of pages to the free list.
Note that this function does not actually flush any data to disk. It
just detaches the uncompressed frames from the compressed pages at the
tail of the unzip_LRU and puts those freed frames in the free list.
@return number of blocks moved to the free list. */
static ulint buf_free_from_unzip_LRU_list_batch() noexcept
{
	ulint		scanned = 0;
	ulint		count = 0;

	mysql_mutex_assert_owner(&buf_pool.mutex);

	buf_block_t*	block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);

	while (block
	       && UT_LIST_GET_LEN(buf_pool.free) < buf_pool.LRU_scan_depth
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

/** Acquire a tablespace reference for writing.
@param id      tablespace identifier
@return tablespace
@retval nullptr if the tablespace is missing or inaccessible */
fil_space_t *fil_space_t::get_for_write(uint32_t id) noexcept
{
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(id);
  const uint32_t n= space ? space->acquire_low(STOPPING_WRITES) : 0;

  if (n & STOPPING_WRITES)
    space= nullptr;
  else if ((n & CLOSING) && !space->prepare_acquired())
    space= nullptr;

  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}

/** Start writing out pages for a tablespace.
@param id   tablespace identifier
@return tablespace and number of pages written */
static std::pair<fil_space_t*, uint32_t> buf_flush_space(const uint32_t id)
  noexcept
{
  if (fil_space_t *space= fil_space_t::get_for_write(id))
    return {space, space->flush_freed(true)};
  return {nullptr, 0};
}

struct flush_counters_t
{
  /** number of dirty pages flushed */
  ulint flushed;
  /** number of clean pages evicted */
  ulint evicted;
};

/** Discard a dirty page, and release buf_pool.flush_list_mutex.
@param bpage      dirty page whose tablespace is not accessible */
static void buf_flush_discard_page(buf_page_t *bpage) noexcept
{
  ut_ad(bpage->in_file());
  ut_ad(bpage->oldest_modification());

  buf_pool.delete_from_flush_list(bpage);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  ut_d(const auto state= bpage->state());
  ut_ad(state == buf_page_t::FREED || state == buf_page_t::UNFIXED ||
        state == buf_page_t::REINIT);
  bpage->lock.u_unlock(true);
  buf_LRU_free_page(bpage, true);
}

/** Adjust to_withdraw during buf_pool_t::shrink() */
ATTRIBUTE_COLD static size_t buf_flush_LRU_to_withdraw(size_t to_withdraw,
                                                       const buf_page_t &bpage)
  noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);
  if (!buf_pool.is_shrinking())
    return 0;
  const size_t size{buf_pool.size_in_bytes_requested};
  if (buf_pool.will_be_withdrawn(bpage.frame, size) ||
      buf_pool.will_be_withdrawn(bpage.zip.data, size))
    to_withdraw--;
  return to_withdraw;
}

/** Flush dirty blocks from the end buf_pool.LRU,
and move clean blocks to buf_pool.free.
@param max         maximum number of blocks to flush
@param n           counts of flushed and evicted pages
@param to_withdraw buf_pool.to_withdraw() */
static void buf_flush_LRU_list_batch(ulint max, flush_counters_t *n,
                                     size_t to_withdraw) noexcept
{
  size_t scanned= 0;
  mysql_mutex_assert_owner(&buf_pool.mutex);
  size_t free_limit{buf_pool.LRU_scan_depth};
  if (UNIV_UNLIKELY(to_withdraw > free_limit))
    to_withdraw= free_limit;
  const auto neighbors= UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN
    ? 0 : buf_pool.flush_neighbors;
  fil_space_t *space= nullptr;
  uint32_t last_space_id= FIL_NULL;
  static_assert(FIL_NULL > SRV_TMP_SPACE_ID, "consistency");
  static_assert(FIL_NULL > SRV_SPACE_ID_UPPER_BOUND, "consistency");

  /* BUF_LRU_MIN_LEN (256) is too high value for low buffer pool(BP) size. For
  example, for BP size lower than 80M and 16 K page size, the limit is more than
  5% of total BP and for lowest BP 6M, it is 80% of the BP. Non-data objects
  like explicit locks could occupy part of the BP pool reducing the pages
  available for LRU. If LRU reaches minimum limit and if no free pages are
  available, server would hang with page cleaner not able to free any more
  pages. To avoid such hang, we adjust the LRU limit lower than the limit for
  data objects as checked in buf_LRU_check_size_of_non_data_objects() i.e. one
  page less than 5% of BP. */
  const size_t buf_lru_min_len=
    std::min((buf_pool.usable_size()) / 20 - 1, size_t{BUF_LRU_MIN_LEN});

  ulint free_or_flush= 0;
  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.LRU);
       bpage &&
       (ut_d(buf_pool.force_LRU_eviction_to_ebp ||)
        (UT_LIST_GET_LEN(buf_pool.LRU) > buf_lru_min_len &&
         UT_LIST_GET_LEN(buf_pool.free) < free_limit) ||
        to_withdraw ||
        recv_recovery_is_on());
       ++scanned, bpage= buf_pool.lru_hp.get())
  {
    buf_page_t *prev= UT_LIST_GET_PREV(LRU, bpage);
    buf_pool.lru_hp.set(prev);
    auto state= bpage->state();
    ut_ad(state >= buf_page_t::FREED);
    ut_ad(bpage->in_LRU_list);

    bool flush_to_ebp= false;
    if (!bpage->oldest_modification())
    {
    evict:
      if (state != buf_page_t::FREED &&
          (state >= buf_page_t::READ_FIX || (~buf_page_t::LRU_MASK & state)))
        continue;
      if (UNIV_UNLIKELY(to_withdraw != 0))
        to_withdraw= buf_flush_LRU_to_withdraw(to_withdraw, *bpage);
      DBUG_EXECUTE_IF(
          "ib_ext_bp_disable_LRU_eviction_for_t",
          if (fil_space_t *space= fil_space_t::get(bpage->id_.space())) {
            SCOPE_EXIT([space]() { space->release(); });
            auto space_name= space->name();
            if (space_name.data() &&
                !strncmp(space_name.data(), "test/t.ibd", space_name.size()))
              continue;
          });
      DBUG_EXECUTE_IF(
          "ib_ext_bp_count_io_only_for_t",
          if (fil_space_t *space= fil_space_t::get(bpage->id_.space())) {
            SCOPE_EXIT([space]() { space->release(); });
            auto space_name= space->name();
            if (!space_name.data() ||
                strncmp(space_name.data(), "test/t.ibd", space_name.size()))
              goto free_page;;
          });
      // FIXME: currently every second page is flushed, consider more
      // suitable algorithm there
      if (state != buf_page_t::FREED && fil_system.ext_bp_size &&
          !buf_pool.done_flush_list_waiters_count &&
          (ut_d(buf_pool.force_LRU_eviction_to_ebp ||)((++free_or_flush) & 1)))
      {
        flush_to_ebp= true;
        goto flush_to_ebp;
      }
      else
      {
      free_page:
        buf_LRU_free_page(bpage, true);
        ++n->evicted;
      }
      if (UNIV_LIKELY(scanned & 31))
        continue;
      mysql_mutex_unlock(&buf_pool.mutex);
    reacquire_mutex:
      mysql_mutex_lock(&buf_pool.mutex);
      continue;
    }

flush_to_ebp:
  if ((flush_to_ebp || state < buf_page_t::READ_FIX) &&
      bpage->lock.u_lock_try(true))
  {
    ut_ad(!bpage->is_io_fixed());
    switch (bpage->oldest_modification())
    {
    case 2:
      /* LRU flushing will always evict pages of the temporary tablespace,
      in buf_page_write_complete(). */
      ++n->evicted;
      break;
    case 1:
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      if (ut_d(lsn_t lsn=) bpage->oldest_modification())
      {
        ut_ad(lsn == 1); /* It must be clean while we hold bpage->lock */
        buf_pool.delete_from_flush_list(bpage);
      }
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      /* fall through */
    case 0:
      if (!flush_to_ebp)
      {
        bpage->lock.u_unlock(true);
        goto evict;
      }
    }
    /* Block is ready for flush. Dispatch an IO request. */
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
        if (!space)
        {
          mysql_mutex_lock(&buf_pool.mutex);
          goto no_space;
        }
        mysql_mutex_lock(&buf_pool.mutex);
        buf_pool.stat.n_pages_written+= p.second;
      }
      else
      {
        ut_ad(!space);
        goto no_space;
      }
    }
    else if (space->is_stopping_writes())
    {
      space->release();
      space= nullptr;
    no_space:
      if (flush_to_ebp && !bpage->oldest_modification()) {
        bpage->lock.u_unlock(true);
        buf_LRU_free_page(bpage, true);
      } else {
        mysql_mutex_lock(&buf_pool.flush_list_mutex);
        buf_flush_discard_page(bpage);
      }
      ++n->evicted;
      continue;
    }

      if (!flush_to_ebp && state < buf_page_t::UNFIXED)
        goto flush;

      if (n->flushed >= max && !recv_recovery_is_on())
      {
        bpage->lock.u_unlock(true);
        break;
      }

      if (flush_to_ebp)
      {
        /* The page latch will be released in io callback */
        if (!bpage->flush(space, true))
        {
          buf_LRU_free_page(bpage, true);
          bpage->lock.u_unlock(true);
          ++n->evicted;
          continue;
        }
        /*
        ut_d(if (DBUG_IF("ib_ext_bp_count_io_only_for_t")) {
          auto space_name= space->name();
          if (space_name.data() &&
              !strncmp(space_name.data(), "test/t.ibd", space_name.size()) &&
              fil_page_get_type(bpage->frame) == FIL_PAGE_INDEX) {
            ++buf_pool.stat.n_pages_written_to_ebp;
            }
        } else)
        ++ buf_pool.stat.n_pages_written_to_ebp;
        */
      }
      else if (neighbors && space->is_rotational() &&
               UNIV_LIKELY(!to_withdraw) &&
               /* Skip neighbourhood flush from LRU list if we haven't yet
               reached half of the free page target. */
               UT_LIST_GET_LEN(buf_pool.free) * 2 >= free_limit)
        n->flushed+= buf_flush_try_neighbors(space, page_id, bpage,
                                             neighbors == 1, n->flushed, max);
      else
      {
      flush:
        if (UNIV_UNLIKELY(to_withdraw != 0))
          to_withdraw= buf_flush_LRU_to_withdraw(to_withdraw, *bpage);
        if (bpage->flush(space))
          ++n->flushed;
        else
          continue;
      }

      goto reacquire_mutex;
    }
    else
      /* Can't evict or dispatch this block. Go to previous. */
      ut_ad(buf_pool.lru_hp.is_hp(prev));
  }

  buf_pool.lru_hp.set(nullptr);

  if (space)
    space->release();

  if (scanned)
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_BATCH_SCANNED,
                                 MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_LRU_BATCH_SCANNED_PER_CALL,
                                 scanned);
  }
}

/** Flush and move pages from LRU or unzip_LRU list to the free list.
Whether LRU or unzip_LRU is used depends on the state of the system.
@param max    maximum number of blocks to flush
@param n      counts of flushed and evicted pages */
static void buf_do_LRU_batch(ulint max, flush_counters_t *n) noexcept
{
  const size_t to_withdraw= buf_pool.to_withdraw();
  if (!to_withdraw && buf_LRU_evict_from_unzip_LRU())
    buf_free_from_unzip_LRU_list_batch();
  n->evicted= 0;
  n->flushed= 0;
  buf_flush_LRU_list_batch(max, n, to_withdraw);

  mysql_mutex_assert_owner(&buf_pool.mutex);
  buf_lru_freed_page_count+= n->evicted;
  buf_lru_flush_page_count+= n->flushed;
  buf_pool.stat.n_pages_written+= n->flushed;
}

/** This utility flushes dirty blocks from the end of the flush_list.
The calling thread is not allowed to own any latches on pages!
@param max_n    maximum mumber of blocks to flush
@param lsn      once an oldest_modification>=lsn is found, terminate the batch
@return number of blocks for which the write request was queued */
static ulint buf_do_flush_list_batch(ulint max_n, lsn_t lsn) noexcept
{
  ulint count= 0;
  ulint scanned= 0;

  mysql_mutex_assert_owner(&buf_pool.mutex);
  mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

  const auto neighbors= UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN
    ? 0 : buf_pool.flush_neighbors;
  fil_space_t *space= nullptr;
  uint32_t last_space_id= FIL_NULL;
  static_assert(FIL_NULL > SRV_TMP_SPACE_ID, "consistency");
  static_assert(FIL_NULL > SRV_SPACE_ID_UPPER_BOUND, "consistency");

  /* Start from the end of the list looking for a suitable block to be
  flushed. */
  ulint len= UT_LIST_GET_LEN(buf_pool.flush_list);

  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list);
       bpage && len && count < max_n; ++scanned, len--)
  {
    const lsn_t oldest_modification= bpage->oldest_modification();
    if (oldest_modification >= lsn)
      break;
    ut_ad(bpage->in_file());

    {
      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      if (oldest_modification == 1)
      {
      clear:
        buf_pool.delete_from_flush_list(bpage);
      skip:
        bpage= prev;
        if (scanned & 31)
          continue;
        /* Release the buf_pool.flush_list_mutex every now and then
        in order to reduce the wait time in buf_flush_ahead().
        We attempt to preserve the pointer position while yielding.
        Any thread that would remove 'prev' from buf_pool.flush_list
        must adjust the hazard pointer. */
        buf_pool.flush_hp.set(prev);
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        goto next;
      }

      ut_ad(oldest_modification > 2);

      if (!bpage->lock.u_lock_try(true))
        goto skip;

      ut_ad(!bpage->is_io_fixed());

      if (bpage->oldest_modification() == 1)
      {
        bpage->lock.u_unlock(true);
        goto clear;
      }

      /* In order not to degenerate this scan to O(n*n) we attempt to
      preserve the pointer position. Any thread that would remove 'prev'
      from buf_pool.flush_list must adjust the hazard pointer.

      Note: A concurrent execution of buf_flush_list_space() may
      terminate this scan prematurely. The buf_pool.flush_list_active
      should prevent multiple threads from executing
      buf_do_flush_list_batch() concurrently,
      but buf_flush_list_space() is ignoring that. */
      buf_pool.flush_hp.set(prev);
    }

    {
      const page_id_t page_id(bpage->id());
      const uint32_t space_id= page_id.space();
      if (!space || space->id != space_id)
      {
        if (last_space_id != space_id)
        {
          mysql_mutex_unlock(&buf_pool.flush_list_mutex);
          mysql_mutex_unlock(&buf_pool.mutex);
          if (space)
            space->release();
          auto p= buf_flush_space(space_id);
          space= p.first;
          last_space_id= space_id;
          mysql_mutex_lock(&buf_pool.mutex);
          buf_pool.stat.n_pages_written+= p.second;
          mysql_mutex_lock(&buf_pool.flush_list_mutex);
        }
        else
          ut_ad(!space);
      }
      else if (space->is_stopping_writes())
      {
        space->release();
        space= nullptr;
      }

      if (!space)
        buf_flush_discard_page(bpage);
      else
      {
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        do
        {
          if (neighbors && space->is_rotational())
            count+= buf_flush_try_neighbors(space, page_id, bpage,
                                            neighbors == 1, count, max_n);
          else if (bpage->flush(space))
            ++count;
          else
            continue;
          mysql_mutex_lock(&buf_pool.mutex);
        }
        while (0);
      }
    }

  next:
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    bpage= buf_pool.flush_hp.get();
  }

  buf_pool.flush_hp.set(nullptr);

  if (space)
    space->release();

  if (scanned)
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_SCANNED,
                                 MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
                                 scanned);
  }

  return count;
}

/** Wait until a LRU flush batch ends. */
void buf_flush_wait_LRU_batch_end() noexcept
{
  mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);
  mysql_mutex_assert_not_owner(&buf_pool.mutex);

  if (buf_pool.n_flush())
  {
    tpool::tpool_wait_begin();
    thd_wait_begin(nullptr, THD_WAIT_DISKIO);
    do
      my_cond_wait(&buf_pool.done_flush_LRU,
                   &buf_pool.flush_list_mutex.m_mutex);
    while (buf_pool.n_flush());
    tpool::tpool_wait_end();
    thd_wait_end(nullptr);
  }
}

/** Write out dirty blocks from buf_pool.flush_list.
The caller must invoke buf_dblwr.flush_buffered_writes()
after releasing buf_pool.mutex.
@param max_n    wished maximum mumber of blocks flushed
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target
@return the number of processed pages
@retval 0 if a buf_pool.flush_list batch is already running */
static ulint buf_flush_list_holding_mutex(ulint max_n= ULINT_UNDEFINED,
                                          lsn_t lsn= LSN_MAX) noexcept
{
  ut_ad(lsn);
  mysql_mutex_assert_owner(&buf_pool.mutex);

  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  if (buf_pool.flush_list_active())
  {
nothing_to_do:
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    return 0;
  }
  if (!buf_pool.get_oldest_modification(0))
  {
    pthread_cond_broadcast(&buf_pool.done_flush_list);
    goto nothing_to_do;
  }
  buf_pool.flush_list_set_active();
  const ulint n_flushed= buf_do_flush_list_batch(max_n, lsn);
  if (n_flushed)
    buf_pool.stat.n_pages_written+= n_flushed;
  buf_pool.flush_list_set_inactive();
  pthread_cond_broadcast(&buf_pool.done_flush_list);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (n_flushed)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_TOTAL_PAGE,
                                 MONITOR_FLUSH_BATCH_COUNT,
                                 MONITOR_FLUSH_BATCH_PAGES,
                                 n_flushed);

  DBUG_PRINT("ib_buf", ("flush_list completed, " ULINTPF " pages", n_flushed));
  return n_flushed;
}

/** Write out dirty blocks from buf_pool.flush_list.
@param max_n    wished maximum mumber of blocks flushed
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target
@return the number of processed pages
@retval 0 if a buf_pool.flush_list batch is already running */
static ulint buf_flush_list(ulint max_n= ULINT_UNDEFINED,
                            lsn_t lsn= LSN_MAX) noexcept
{
  mysql_mutex_lock(&buf_pool.mutex);
  ulint n= buf_flush_list_holding_mutex(max_n, lsn);
  mysql_mutex_unlock(&buf_pool.mutex);
  buf_dblwr.flush_buffered_writes();
  return n;
}

/** Try to flush all the dirty pages that belong to a given tablespace.
@param space       tablespace
@param n_flushed   number of pages written
@return whether the flush for some pages might not have been initiated */
bool buf_flush_list_space(fil_space_t *space, ulint *n_flushed) noexcept
{
  const auto space_id= space->id;
  ut_ad(space_id < SRV_SPACE_ID_UPPER_BOUND);

  bool may_have_skipped= false;
  ulint max_n_flush= srv_io_capacity;
  ulint n_flush= 0;

  bool acquired= space->acquire_for_write();
  {
    const uint32_t written{space->flush_freed(acquired)};
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
    if (bpage->oldest_modification() == 1)
    clear:
      buf_pool.delete_from_flush_list(bpage);
    else if (bpage->id().space() != space_id);
    else if (!bpage->lock.u_lock_try(true))
      may_have_skipped= true;
    else if (bpage->oldest_modification() == 1)
    {
      bpage->lock.u_unlock(true);
      goto clear;
    }
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

      if (!acquired)
      was_freed:
        buf_flush_discard_page(bpage);
      else
      {
        if (space->is_stopping_writes())
        {
          space->release();
          acquired= false;
          goto was_freed;
        }
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        if (bpage->flush(space))
        {
          ++n_flush;
          if (!--max_n_flush)
          {
            mysql_mutex_lock(&buf_pool.mutex);
            mysql_mutex_lock(&buf_pool.flush_list_mutex);
            may_have_skipped= true;
            goto done;
          }
          mysql_mutex_lock(&buf_pool.mutex);
        }
      }

      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      if (!buf_pool.flush_hp.is_hp(prev))
        may_have_skipped= true;
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
done:
  buf_pool.flush_hp.set(nullptr);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  buf_pool.stat.n_pages_written+= n_flush;

  buf_pool.try_LRU_scan= true;
  pthread_cond_broadcast(&buf_pool.done_free);
  mysql_mutex_unlock(&buf_pool.mutex);

  if (n_flushed)
    *n_flushed= n_flush;

  if (acquired)
    space->release();

  if (space->is_being_imported())
    os_aio_wait_until_no_pending_writes(true);
  else
    buf_dblwr.flush_buffered_writes();

  return may_have_skipped;
}

/** Write out dirty blocks from buf_pool.LRU,
and move clean blocks to buf_pool.free.
The caller must invoke buf_dblwr.flush_buffered_writes()
after releasing buf_pool.mutex.
@param max_n    wished maximum mumber of blocks flushed
@return number of pages written */
static ulint buf_flush_LRU(ulint max_n) noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);

  flush_counters_t n;
  buf_do_LRU_batch(max_n, &n);

  ulint pages= n.flushed;
  ulint evicted= n.evicted;

  /* If we have exhausted flush quota, it is likely we exited before
  generating enough free pages. Call once more with 0 flush to generate
  free pages immediately as required. */
  if (pages >= max_n)
    buf_do_LRU_batch(0, &n);

  evicted+= n.evicted;
  if (evicted)
  {
    buf_pool.try_LRU_scan= true;
    pthread_cond_broadcast(&buf_pool.done_free);
  }
  else if (!pages && !buf_pool.try_LRU_scan)
    /* For example, with the minimum innodb_buffer_pool_size=6M and
    the default innodb_page_size=16k there are only a little over 316
    pages in the buffer pool. The buffer pool can easily be exhausted
    by a workload of some dozen concurrent connections. The system could
    reach a deadlock like the following:

    (1) Many threads are waiting in buf_LRU_get_free_block()
    for buf_pool.done_free.
    (2) Some threads are waiting for a page latch which is held by
    another thread that is waiting in buf_LRU_get_free_block().
    (3) This thread is the only one that could make progress, but
    we fail to do so because all the pages that we scanned are
    buffer-fixed or latched by some thread. */
    buf_pool.LRU_warn();

  return pages;
}

#ifdef HAVE_PMEM
# include "cache.h"
#endif

/** Write checkpoint information to the log header and release mutex.
@param end_lsn    start LSN of the FILE_CHECKPOINT mini-transaction */
inline void log_t::write_checkpoint(lsn_t end_lsn) noexcept
{
  ut_ad(!srv_read_only_mode);
  ut_ad(end_lsn >= next_checkpoint_lsn);
  ut_d(const lsn_t current_lsn{get_lsn()});
  ut_ad(end_lsn <= current_lsn);
  ut_ad(end_lsn + SIZE_OF_FILE_CHECKPOINT <= current_lsn ||
        srv_shutdown_state > SRV_SHUTDOWN_INITIATED);

  DBUG_PRINT("ib_log",
             ("checkpoint at " LSN_PF " written", next_checkpoint_lsn));

  auto n= next_checkpoint_no;
  const size_t offset{(n & 1) ? CHECKPOINT_2 : CHECKPOINT_1};
  static_assert(CPU_LEVEL1_DCACHE_LINESIZE >= 64, "efficiency");
  static_assert(CPU_LEVEL1_DCACHE_LINESIZE <= 4096, "compatibility");
  byte* c= my_assume_aligned<CPU_LEVEL1_DCACHE_LINESIZE>
    (is_mmap() ? buf + offset : checkpoint_buf);
  memset_aligned<CPU_LEVEL1_DCACHE_LINESIZE>(c, 0, CPU_LEVEL1_DCACHE_LINESIZE);
  mach_write_to_8(my_assume_aligned<8>(c), next_checkpoint_lsn);
  mach_write_to_8(my_assume_aligned<8>(c + 8), end_lsn);
  mach_write_to_4(my_assume_aligned<4>(c + 60), my_crc32c(0, c, 60));

  lsn_t resizing;

#ifdef HAVE_PMEM
  if (is_mmap())
  {
    ut_ad(!is_opened());
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
    ut_ad(!is_mmap());
    ut_ad(!checkpoint_pending);
    checkpoint_pending= true;
    latch.wr_unlock();
    log_write_and_flush_prepare();
    resizing= resize_lsn.load(std::memory_order_relaxed);
    ut_ad(ut_is_2pow(write_size));
    ut_ad(write_size >= 512);
    ut_ad(write_size <= 4096);
    log.write(offset, {c, write_size});
    if (resizing > 1 && resizing <= next_checkpoint_lsn)
    {
      resize_log.write(CHECKPOINT_1, {c, write_size});
      byte *buf= static_cast<byte*>(aligned_malloc(4096, 4096));
      memset_aligned<4096>(buf, 0, 4096);
      header_write(buf, resizing, is_encrypted());
      resize_log.write(0, {buf, 4096});
      aligned_free(buf);
    }

    if (!log_write_through)
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
  if (overwrite_warned)
  {
    sql_print_information("InnoDB: Crash recovery was broken "
                          "between LSN=" LSN_PF
                          " and checkpoint LSN=" LSN_PF ".",
                          overwrite_warned, checkpoint_lsn);
    overwrite_warned= 0;
  }

  lsn_t resizing_completed= 0;

  if (resizing > 1 && resizing <= checkpoint_lsn)
  {
    ut_ad(is_mmap() == !resize_flush_buf);
    ut_ad(is_mmap() == !resize_log.is_opened());

    if (!is_mmap())
    {
      if (!log_write_through)
        ut_a(resize_log.flush());
      IF_WIN(log.close(),);
    }

    if (resize_rename())
    {
      /* Resizing failed. Discard the ib_logfile101. */
#ifdef HAVE_PMEM
      if (is_mmap())
      {
        ut_ad(!is_opened());
        my_munmap(resize_buf, resize_target);
      }
      else
#endif
      {
        ut_ad(!is_mmap());
        ut_free_dodump(resize_buf, buf_size);
        ut_free_dodump(resize_flush_buf, buf_size);
#ifdef _WIN32
        ut_ad(!log.is_opened());
        bool success;
        log.m_file=
          os_file_create_func(get_log_file_path().c_str(), OS_FILE_OPEN,
                              OS_LOG_FILE, false, &success);
        ut_a(success);
        ut_a(log.is_opened());
#endif
      }
    }
    else
    {
      /* Adopt the resized log. */
#ifdef HAVE_PMEM
      if (is_mmap())
      {
        ut_ad(!is_opened());
        my_munmap(buf, file_size);
        buf= resize_buf;
        buf_size= unsigned(std::min<uint64_t>(resize_target - START_OFFSET,
                                              buf_size_max));
      }
      else
#endif
      {
        ut_ad(!is_mmap());
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
    resize_initiator= nullptr;
    writer_update(false);
  }

  log_resize_release();

  if (UNIV_LIKELY(resizing <= 1));
  else if (resizing > checkpoint_lsn)
    buf_flush_ahead(resizing, false);
  else if (resizing_completed)
    ib::info() << "Resized log to " << ib::bytes_iec{resizing_completed}
      << "; start LSN=" << resizing;
  else
    buf_flush_ahead(end_lsn + 1, false);
}

/** Initiate a log checkpoint, discarding the start of the log.
@param oldest_lsn   the checkpoint LSN
@param end_lsn      log_sys.get_lsn()
@return true if success, false if a checkpoint write was already running */
static bool log_checkpoint_low(lsn_t oldest_lsn, lsn_t end_lsn) noexcept
{
  ut_ad(!srv_read_only_mode);
  ut_ad(log_sys.latch_have_wr());
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
static bool log_checkpoint() noexcept
{
  if (recv_recovery_is_on())
    recv_sys.apply(true);

#if defined HAVE_valgrind && !__has_feature(memory_sanitizer)
  /* The built-in scheduler in Valgrind may neglect some threads for a
  long time.  Under Valgrind, let us explicitly wait for page write
  completion in order to avoid a result difference in the test
  innodb.page_cleaner. */
  os_aio_wait_until_no_pending_writes(false);
#endif

  fil_flush_file_spaces();

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  const lsn_t end_lsn= log_sys.get_lsn();
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  const lsn_t oldest_lsn= buf_pool.get_oldest_modification(end_lsn);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  return log_checkpoint_low(oldest_lsn, end_lsn);
}

/** Make a checkpoint. */
ATTRIBUTE_COLD void log_make_checkpoint() noexcept
{
  buf_flush_wait_flushed(log_get_lsn());
  while (!log_checkpoint());
}

/** Wait for all dirty pages up to an LSN to be written out.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
static void buf_flush_wait(lsn_t lsn) noexcept
{
  lsn_t oldest_lsn;

  while ((oldest_lsn= buf_pool.get_oldest_modification(lsn)) < lsn)
  {
    if (buf_flush_sync_lsn < lsn)
    {
      buf_flush_sync_lsn= lsn;
      buf_pool.page_cleaner_set_idle(false);
      ++buf_pool.done_flush_list_waiters_count;
      pthread_cond_signal(&buf_pool.do_flush_list);
      my_cond_wait(&buf_pool.done_flush_list,
                   &buf_pool.flush_list_mutex.m_mutex);
      --buf_pool.done_flush_list_waiters_count;
      oldest_lsn= buf_pool.get_oldest_modification(lsn);
      if (oldest_lsn >= lsn)
        break;
    }
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    os_aio_wait_until_no_pending_writes(false);
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
  }

  if (oldest_lsn >= buf_flush_sync_lsn)
  {
    buf_flush_sync_lsn= 0;
    pthread_cond_broadcast(&buf_pool.done_flush_list);
  }
}

/** Wait until all persistent pages are flushed up to a limit.
@param sync_lsn   buf_pool.get_oldest_modification(LSN_MAX) to wait for */
ATTRIBUTE_COLD void buf_flush_wait_flushed(lsn_t sync_lsn) noexcept
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
        if (n_pages)
        {
          MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_SYNC_TOTAL_PAGE,
                                       MONITOR_FLUSH_SYNC_COUNT,
                                       MONITOR_FLUSH_SYNC_PAGES, n_pages);
        }
        os_aio_wait_until_no_pending_writes(false);
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
    DBUG_EXECUTE_IF("ib_log_checkpoint_avoid_hard", return;);
    log_checkpoint();
  }
}

/** Initiate more eager page flushing if the log checkpoint age is too old.
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target
@param furious  true=furious flushing, false=limit to innodb_io_capacity */
ATTRIBUTE_COLD void buf_flush_ahead(lsn_t lsn, bool furious) noexcept
{
  ut_ad(!srv_read_only_mode);

  if (recv_recovery_is_on())
    recv_sys.apply(true);

  DBUG_EXECUTE_IF("ib_log_checkpoint_avoid_hard", return;);

  Atomic_relaxed<lsn_t> &limit= furious
    ? buf_flush_sync_lsn : buf_flush_async_lsn;

  if (limit < lsn)
  {
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    if (limit < lsn)
    {
      limit= lsn;
      if (furious)
      {
        /* Request any concurrent threads to wait for this batch to complete,
        in log_free_check(). */
        log_sys.set_check_for_checkpoint();
        /* Immediately wake up buf_flush_page_cleaner(), even when it
        is in the middle of a 1-second my_cond_timedwait(). */
      wake:
        buf_pool.page_cleaner_set_idle(false);
        pthread_cond_signal(&buf_pool.do_flush_list);
      }
      else if (buf_pool.page_cleaner_idle())
        /* In non-furious mode, concurrent writes to the log will remain
        possible, and we are gently requesting buf_flush_page_cleaner()
        to do more work to avoid a later call with furious=true.
        We will only wake the buf_flush_page_cleaner() from an indefinite
        my_cond_wait(), but we will not disturb the regular 1-second sleep. */
        goto wake;
    }
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  }
}

/** Conduct checkpoint-related flushing for innodb_flush_sync=ON,
and try to initiate checkpoints until the target is met.
@param lsn   minimum value of buf_pool.get_oldest_modification(LSN_MAX) */
ATTRIBUTE_COLD ATTRIBUTE_NOINLINE
static void buf_flush_sync_for_checkpoint(lsn_t lsn) noexcept
{
  ut_ad(!srv_read_only_mode);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);

  /* During furious flush, we need to keep generating free pages. Otherwise
  concurrent mtrs could be blocked holding latches for the pages to be flushed
  causing deadlock in rare occasion.

  Ideally we should be acquiring buffer pool mutex for the check but it is more
  expensive and we are not using the mutex while calling need_LRU_eviction() as
  of today. It is a quick and dirty read of the LRU and free list length.
  Atomic read of try_LRU_scan should eventually let us do the eviction.
  Correcting the inaccuracy would need more consideration to avoid any possible
  performance regression. */
  if (buf_pool.need_LRU_eviction())
  {
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    buf_pool.page_cleaner_set_idle(false);
    buf_pool.n_flush_inc();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    mysql_mutex_lock(&buf_pool.mutex);
    /* Confirm that eviction is needed after acquiring buffer pool mutex. */
    if (buf_pool.need_LRU_eviction())
      /* We intend to only evict pages keeping maximum flush bandwidth for
      flush list pages advancing checkpoint. However, if the LRU tail is full
      of dirty pages, we might need some flushing. */
      std::ignore= buf_flush_LRU(srv_io_capacity);
    mysql_mutex_unlock(&buf_pool.mutex);

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    buf_pool.n_flush_dec();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  }

  if (ulint n_flushed= buf_flush_list(srv_max_io_capacity, lsn))
  {
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_SYNC_TOTAL_PAGE,
                                 MONITOR_FLUSH_SYNC_COUNT,
                                 MONITOR_FLUSH_SYNC_PAGES, n_flushed);
  }

  os_aio_wait_until_no_pending_writes(false);
  fil_flush_file_spaces();

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
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Check if the adaptive flushing threshold is recommended based on
redo log capacity filled threshold.
@param oldest_lsn     buf_pool.get_oldest_modification()
@return true if adaptive flushing is recommended. */
static bool af_needed_for_redo(lsn_t oldest_lsn) noexcept
{
  /* We may have oldest_lsn > log_sys.get_lsn_approx() if
  log_t::write_buf() or log_t::persist() are executing concurrently
  with this. In that case, age > af_lwm should hold, and
  buf_flush_page_cleaner() would execute one more timed wait. (Not a
  big problem.) */

  lsn_t age= log_sys.get_lsn_approx() - oldest_lsn;
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

/** This function is called approximately once every second by
buf_flush_page_cleaner() if innodb_max_dirty_pages_pct_lwm>0
and innodb_adaptive_flushing=ON.
Based on various factors it decides if there is a need to do flushing.
@return number of pages recommended to be flushed
@param last_pages_in  number of pages flushed in previous batch
@param oldest_lsn     buf_pool.get_oldest_modification(0)
@param pct_lwm        innodb_max_dirty_pages_pct_lwm, or 0 to ignore it
@param dirty_blocks   UT_LIST_GET_LEN(buf_pool.flush_list)
@param dirty_pct      100*flush_list.count / (LRU.count + free.count) */
static ulint page_cleaner_flush_pages_recommendation(ulint last_pages_in,
                                                     lsn_t oldest_lsn,
                                                     double pct_lwm,
                                                     ulint dirty_blocks,
                                                     double dirty_pct) noexcept
{
	static	lsn_t		prev_lsn = 0;
	static	ulint		sum_pages = 0;
	static	ulint		avg_page_rate = 0;
	static	ulint		n_iterations = 0;
	static	time_t		prev_time;
	lsn_t			lsn_rate;
	ulint			n_pages = 0;

	const lsn_t cur_lsn = log_sys.get_lsn_approx();
	/* We may have cur_lsn < oldest_lsn if
	log_t::write_buf() or log_t::persist() were executing concurrently. */
	ulint pct_for_lsn = cur_lsn < oldest_lsn
                ? 0 : af_get_pct_for_lsn(cur_lsn - oldest_lsn);
	time_t curr_time = time(nullptr);
	const double max_pct = srv_max_buf_pool_modified_pct;

	if (!prev_lsn || !pct_for_lsn) {
		prev_time = curr_time;
		prev_lsn = cur_lsn;

		if (srv_io_capacity >= dirty_blocks) {
			n_pages = dirty_blocks;
		} else {
			if (max_pct > 1.0) {
				dirty_pct/= max_pct;
			}
			n_pages= ulint(dirty_pct * double(srv_io_capacity));

			if (n_pages < dirty_blocks) {
				n_pages= srv_io_capacity;

			} else {
				/* Set maximum IO capacity upper bound. */
				n_pages= std::min<ulint>(srv_max_io_capacity,
							 dirty_blocks);
			}
		}

func_exit:
		page_cleaner.flush_pass++;
		return n_pages;
	}

	sum_pages += last_pages_in;

	const ulint time_elapsed = std::max<ulint>(ulint(curr_time - prev_time), 1);

	/* We update our variables every innodb_flushing_avg_loops
	iterations to smooth out transition in workload. */
	if (++n_iterations >= srv_flushing_avg_loops
	    || time_elapsed >= srv_flushing_avg_loops) {

		avg_page_rate = (sum_pages / time_elapsed + avg_page_rate) / 2;

		/* How much LSN we have generated since last call. */
		lsn_rate = (cur_lsn - prev_lsn) / time_elapsed;

		lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;

		if (page_cleaner.flush_pass) {
			page_cleaner.flush_time /= page_cleaner.flush_pass;
		}

		prev_lsn = cur_lsn;
		prev_time = curr_time;

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME,
			    page_cleaner.flush_time);
		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_PASS,
			    page_cleaner.flush_pass);

		page_cleaner.flush_time = 0;
		page_cleaner.flush_pass = 0;

		n_iterations = 0;
		sum_pages = 0;
	}

	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_LSN, pct_for_lsn);

	double total_ratio;
	if (pct_lwm == 0.0 || max_pct == 0.0) {
		total_ratio = 1;
	} else {
		total_ratio = std::max(double(pct_for_lsn) / 100,
				       (dirty_pct / max_pct));
	}

	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_DIRTY, ulint(total_ratio * 100));

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

	n_pages = (ulint(double(srv_io_capacity) * total_ratio)
		   + avg_page_rate + pages_for_lsn) / 3;

	if (n_pages > srv_max_io_capacity) {
		n_pages = srv_max_io_capacity;
	}

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_REQUESTED, n_pages);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_BY_AGE, pages_for_lsn);

	MONITOR_SET(MONITOR_FLUSH_AVG_PAGE_RATE, avg_page_rate);
	MONITOR_SET(MONITOR_FLUSH_LSN_AVG_RATE, lsn_avg_rate);

	goto func_exit;
}

TPOOL_SUPPRESS_TSAN
bool buf_pool_t::running_out() const noexcept
{
  return !recv_recovery_is_on() &&
    UT_LIST_GET_LEN(free) + UT_LIST_GET_LEN(LRU) < n_blocks / 4;
}

TPOOL_SUPPRESS_TSAN
bool buf_pool_t::need_LRU_eviction() const noexcept
{
  /* try_LRU_scan==false means that buf_LRU_get_free_block() is waiting
  for buf_flush_page_cleaner() to evict some blocks */
  return UNIV_UNLIKELY(ut_d(force_LRU_eviction_to_ebp ||) !try_LRU_scan ||
                       (UT_LIST_GET_LEN(LRU) > BUF_LRU_MIN_LEN &&
                        UT_LIST_GET_LEN(free) < LRU_scan_depth / 2));
}

#if defined __aarch64__&&defined __GNUC__&&__GNUC__==4&&!defined __clang__
/* Avoid GCC 4.8.5 internal compiler error "could not split insn". */
__attribute__((optimize(0)))
#endif
/** page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator. */
static void buf_flush_page_cleaner() noexcept
{
  my_thread_init();
#ifdef UNIV_PFS_THREAD
  pfs_register_thread(page_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */
  my_thread_set_name("page_cleaner");
  ut_ad(!srv_read_only_mode);
  ut_ad(buf_page_cleaner_is_active);

  ulint last_pages= 0;
  timespec abstime;
  set_timespec(abstime, 1);

  lsn_t lsn_limit;
  ulint last_activity_count= srv_get_activity_count();

  for (;;)
  {
    DBUG_EXECUTE_IF("ib_page_cleaner_sleep",
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      /* Cover the logging code in debug mode. */
      buf_pool.print_flush_info();
      buf_dblwr.lock();
      buf_dblwr.print_info();
      buf_dblwr.unlock();
    });
    lsn_limit= buf_flush_sync_lsn;

    if (UNIV_UNLIKELY(lsn_limit != 0) && UNIV_LIKELY(srv_flush_sync))
    {
    furious_flush:
      buf_flush_sync_for_checkpoint(lsn_limit);
      last_pages= 0;
      set_timespec(abstime, 1);
      continue;
    }

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    if (!buf_pool.need_LRU_eviction())
    {
      if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED)
        break;

      if (buf_pool.page_cleaner_idle() &&
          (!UT_LIST_GET_LEN(buf_pool.flush_list) ||
           srv_max_dirty_pages_pct_lwm == 0.0))
      {
        buf_pool.LRU_warned_clear();
        /* We are idle; wait for buf_pool.page_cleaner_wakeup() */
        my_cond_wait(&buf_pool.do_flush_list,
                     &buf_pool.flush_list_mutex.m_mutex);
      }
      else
        my_cond_timedwait(&buf_pool.do_flush_list,
                          &buf_pool.flush_list_mutex.m_mutex, &abstime);
    }
    set_timespec(abstime, 1);

    lsn_limit= buf_flush_sync_lsn;
    lsn_t oldest_lsn= buf_pool.get_oldest_modification(0);

    if (!oldest_lsn)
    {
    fully_unemployed:
      buf_flush_sync_lsn= 0;
    set_idle:
      buf_pool.page_cleaner_set_idle(true);
    set_almost_idle:
      pthread_cond_broadcast(&buf_pool.done_flush_LRU);
      pthread_cond_broadcast(&buf_pool.done_flush_list);
      if (UNIV_UNLIKELY(srv_shutdown_state > SRV_SHUTDOWN_INITIATED))
        break;
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      buf_dblwr.flush_buffered_writes();

      do
      {
        IF_DBUG(if (_db_keyword_(nullptr, "ib_log_checkpoint_avoid", 1) ||
                    _db_keyword_(nullptr, "ib_log_checkpoint_avoid_hard", 1))
                  continue,);
        if (!recv_recovery_is_on() &&
            !srv_startup_is_before_trx_rollback_phase &&
            srv_operation <= SRV_OPERATION_EXPORT_RESTORED)
          log_checkpoint();
      }
      while (false);

      if (!buf_pool.need_LRU_eviction())
        continue;
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      oldest_lsn= buf_pool.get_oldest_modification(0);
    }

    lsn_t soft_lsn_limit= buf_flush_async_lsn;

    if (UNIV_UNLIKELY(lsn_limit != 0))
    {
      if (srv_flush_sync)
        goto do_furious_flush;
      if (oldest_lsn >= lsn_limit)
      {
        buf_flush_sync_lsn= 0;
        pthread_cond_broadcast(&buf_pool.done_flush_list);
      }
      else if (lsn_limit > soft_lsn_limit)
        soft_lsn_limit= lsn_limit;
    }

    double pct_lwm= 0.0;
    ulint n_flushed= 0, n;

    if (UNIV_UNLIKELY(soft_lsn_limit != 0))
    {
      if (oldest_lsn >= soft_lsn_limit)
        buf_flush_async_lsn= soft_lsn_limit= 0;
    }
    else if (buf_pool.need_LRU_eviction())
    {
      buf_pool.page_cleaner_set_idle(false);
      buf_pool.n_flush_inc();
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
      n= srv_max_io_capacity;
      os_aio_wait_until_no_pending_writes(false);
      mysql_mutex_lock(&buf_pool.mutex);
    LRU_flush:
      n= buf_flush_LRU(n);
      mysql_mutex_unlock(&buf_pool.mutex);
      last_pages+= n;
    check_oldest_and_set_idle:
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      buf_pool.n_flush_dec();
      oldest_lsn= buf_pool.get_oldest_modification(0);
      if (!oldest_lsn)
        goto fully_unemployed;
      if (oldest_lsn >= buf_flush_async_lsn)
        buf_flush_async_lsn= 0;
      buf_pool.page_cleaner_set_idle(false);
      goto set_almost_idle;
    }
    else if (UNIV_UNLIKELY(srv_shutdown_state > SRV_SHUTDOWN_INITIATED))
      break;

    const ulint dirty_blocks= UT_LIST_GET_LEN(buf_pool.flush_list);
    /* We perform dirty reads of the LRU+free list lengths here.
    Division by zero is not possible, because buf_pool.flush_list is
    guaranteed to be nonempty, and it is a subset of buf_pool.LRU. */
    const double dirty_pct= double(dirty_blocks) * 100.0 /
      double(UT_LIST_GET_LEN(buf_pool.LRU) + UT_LIST_GET_LEN(buf_pool.free));
    pct_lwm= srv_max_dirty_pages_pct_lwm;
    if (pct_lwm != 0.0)
    {
      const ulint activity_count= srv_get_activity_count();
      if (activity_count != last_activity_count)
      {
        last_activity_count= activity_count;
        goto maybe_unemployed;
      }
      else if (buf_pool.page_cleaner_idle() && !os_aio_pending_reads())
      {
        /* reaching here means 3 things:
           - last_activity_count == activity_count: suggesting server is idle
           (no trx_t::commit() activity)
           - page cleaner is idle (dirty_pct < srv_max_dirty_pages_pct_lwm)
           - there are no pending reads but there are dirty pages to flush */
        buf_pool.update_last_activity_count(activity_count);
        buf_pool.n_flush_inc();
        mysql_mutex_unlock(&buf_pool.flush_list_mutex);
        goto idle_flush;
      }
      else
      {
      maybe_unemployed:
        if (dirty_pct < pct_lwm)
        {
          pct_lwm= 0.0;
          goto possibly_unemployed;
        }
      }
    }
    else if (dirty_pct < srv_max_buf_pool_modified_pct)
    possibly_unemployed:
      if (!soft_lsn_limit && !af_needed_for_redo(oldest_lsn))
        goto set_idle;

    buf_pool.page_cleaner_set_idle(false);
    buf_pool.n_flush_inc();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (UNIV_UNLIKELY(soft_lsn_limit != 0))
    {
      n= srv_max_io_capacity;
      goto background_flush;
    }

    if (!srv_adaptive_flushing)
    {
    idle_flush:
      n= srv_io_capacity;
      soft_lsn_limit= LSN_MAX;
    background_flush:
      mysql_mutex_lock(&buf_pool.mutex);
      n_flushed= buf_flush_list_holding_mutex(n, soft_lsn_limit);
      MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BACKGROUND_TOTAL_PAGE,
                                   MONITOR_FLUSH_BACKGROUND_COUNT,
                                   MONITOR_FLUSH_BACKGROUND_PAGES,
                                   n_flushed);
    }
    else if ((n= page_cleaner_flush_pages_recommendation(last_pages,
                                                         oldest_lsn,
                                                         pct_lwm,
                                                         dirty_blocks,
                                                         dirty_pct)) != 0)
    {
      const ulint tm= ut_time_ms();
      mysql_mutex_lock(&buf_pool.mutex);
      last_pages= n_flushed= buf_flush_list_holding_mutex(n);
      page_cleaner.flush_time+= ut_time_ms() - tm;
      MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_ADAPTIVE_TOTAL_PAGE,
                                   MONITOR_FLUSH_ADAPTIVE_COUNT,
                                   MONITOR_FLUSH_ADAPTIVE_PAGES,
                                   n_flushed);
    }
    else if (buf_flush_async_lsn <= oldest_lsn &&
             !buf_pool.need_LRU_eviction())
      goto check_oldest_and_set_idle;
    else
    {
      mysql_mutex_lock(&buf_pool.mutex);
      os_aio_wait_until_no_pending_writes(false);
    }

    n= srv_max_io_capacity;
    n= n >= n_flushed ? n - n_flushed : 0;
    /* It is critical to generate free pages to keep the system alive. Make
    sure we are not hindered by dirty pages in LRU tail. */
    n= std::max<ulint>(n, std::min<ulint>(srv_max_io_capacity,
                                          buf_pool.LRU_scan_depth));
    goto LRU_flush;
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (srv_fast_shutdown != 2)
  {
    buf_dblwr.flush_buffered_writes();
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    buf_flush_wait_LRU_batch_end();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    os_aio_wait_until_no_pending_writes(false);
  }

  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  lsn_limit= buf_flush_sync_lsn;
  if (UNIV_UNLIKELY(lsn_limit != 0))
  {
  do_furious_flush:
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    goto furious_flush;
  }
  buf_page_cleaner_is_active= false;
  pthread_cond_broadcast(&buf_pool.done_flush_list);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  my_thread_end();

#ifdef UNIV_PFS_THREAD
  pfs_delete_thread();
#endif
}

ATTRIBUTE_COLD void buf_pool_t::LRU_warn() noexcept
{
  mysql_mutex_assert_owner(&mutex);
  try_LRU_scan= false;
  if (!LRU_warned)
  {
    LRU_warned= true;
    sql_print_warning("InnoDB: Could not free any blocks in the buffer pool!"
                      " Consider increasing innodb_buffer_pool_size.");
    print_flush_info();
  }
}

/** Initialize page_cleaner. */
ATTRIBUTE_COLD void buf_flush_page_cleaner_init() noexcept
{
  ut_ad(!buf_page_cleaner_is_active);
  ut_ad(srv_operation <= SRV_OPERATION_EXPORT_RESTORED ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_EXPORT);
  buf_flush_async_lsn= 0;
  buf_flush_sync_lsn= 0;
  buf_page_cleaner_is_active= true;
  std::thread(buf_flush_page_cleaner).detach();
}

/** Flush the buffer pool on shutdown. */
ATTRIBUTE_COLD void buf_flush_buffer_pool() noexcept
{
  ut_ad(!buf_page_cleaner_is_active);
  ut_ad(!buf_flush_sync_lsn);

  service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                 "Waiting to flush the buffer pool");
  os_aio_wait_until_no_pending_reads(false);

  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  while (buf_pool.get_oldest_modification(0))
  {
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    buf_flush_list(srv_max_io_capacity);
    os_aio_wait_until_no_pending_writes(false);
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                   "Waiting to flush " ULINTPF " pages",
                                   UT_LIST_GET_LEN(buf_pool.flush_list));
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  ut_ad(!os_aio_pending_reads());
}

/** Synchronously flush dirty blocks during recv_sys_t::apply().
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync_batch(lsn_t lsn) noexcept
{
  lsn= std::max(lsn, log_get_lsn());
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_wait(lsn);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync() noexcept
{
  if (recv_recovery_is_on())
  {
    mysql_mutex_lock(&recv_sys.mutex);
    recv_sys.apply(true);
    mysql_mutex_unlock(&recv_sys.mutex);
  }

  thd_wait_begin(nullptr, THD_WAIT_DISKIO);
  tpool::tpool_wait_begin();
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  for (lsn_t lsn= log_sys.get_lsn();;)
  {
    log_sys.latch.wr_unlock();
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    ++buf_pool.done_flush_list_waiters_count;
    buf_flush_wait(lsn);
    /* Wait for the page cleaner to be idle (for log resizing at startup) */
    while (buf_flush_sync_lsn)
      my_cond_wait(&buf_pool.done_flush_list,
                   &buf_pool.flush_list_mutex.m_mutex);

    --buf_pool.done_flush_list_waiters_count;
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    lsn_t new_lsn= log_sys.get_lsn();
    if (lsn == new_lsn)
      break;
    lsn= new_lsn;
  }

  log_sys.latch.wr_unlock();
  tpool::tpool_wait_end();
  thd_wait_end(nullptr);
}

ATTRIBUTE_COLD void buf_pool_t::print_flush_info() const noexcept
{
  /* We do dirty read of UT_LIST count variable. */
  size_t lru_size= UT_LIST_GET_LEN(LRU);
  size_t dirty_size= UT_LIST_GET_LEN(flush_list);
  size_t free_size= UT_LIST_GET_LEN(free);
  size_t dirty_pct= lru_size ? dirty_size * 100 / (lru_size + free_size) : 0;
  sql_print_information("InnoDB: Buffer Pool pages\n"
    "-------------------\n"
    "LRU Pages  : %zu\n"
    "Free Pages : %zu\n"
    "Dirty Pages: %zu : %zu%%\n"
    "-------------------",
    lru_size, free_size, dirty_size, dirty_pct);

  lsn_t lsn= log_get_lsn();
  lsn_t clsn= log_sys.last_checkpoint_lsn;
  sql_print_information("InnoDB: LSN flush parameters\n"
    "-------------------\n"
    "System LSN     : %" PRIu64 "\n"
    "Checkpoint  LSN: %" PRIu64 "\n"
    "Flush ASync LSN: %" PRIu64 "\n"
    "Flush Sync  LSN: %" PRIu64 "\n"
    "-------------------",
    lsn, clsn, buf_flush_async_lsn.load(), buf_flush_sync_lsn.load());

  lsn_t age= lsn - clsn;
  lsn_t age_pct= log_sys.max_checkpoint_age
      ? age * 100 / log_sys.max_checkpoint_age : 0;
  sql_print_information("InnoDB: LSN age parameters\n"
    "-------------------\n"
    "Current Age   : %" PRIu64 " : %" PRIu64 "%%\n"
    "Max Age(Async): %" PRIu64 "\n"
    "Max Age(Sync) : %" PRIu64 "\n"
    "Capacity      : %" PRIu64 "\n"
    "-------------------",
    age, age_pct, log_sys.max_modified_age_async, log_sys.max_checkpoint_age,
    log_sys.log_capacity);

  sql_print_information("InnoDB: Pending IO count\n"
    "-------------------\n"
    "Pending Read : %zu\n"
    "Pending Write: %zu\n"
    "-------------------",
    os_aio_pending_reads_approx(), os_aio_pending_writes_approx());
}

#ifdef UNIV_DEBUG
/** Functor to validate the flush list. */
struct	Check {
	void operator()(const buf_page_t* elem) const noexcept
	{
		ut_ad(elem->oldest_modification());
		ut_ad(!fsp_is_system_temporary(elem->id().space()));
	}
};

/** Validate the flush list. */
static void buf_flush_validate_low() noexcept
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
void buf_flush_validate() noexcept
{
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_validate_low();
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}
#endif /* UNIV_DEBUG */
