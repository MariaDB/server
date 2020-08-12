/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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
@file buf/buf0lru.cc
The database buffer replacement algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "buf0lru.h"
#include "ut0byte.h"
#include "ut0rnd.h"
#include "sync0rw.h"
#include "hash0hash.h"
#include "os0event.h"
#include "fil0fil.h"
#include "btr0btr.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "buf0dblwr.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "os0file.h"
#include "page0zip.h"
#include "log0recv.h"
#include "srv0srv.h"
#include "srv0mon.h"

/** The number of blocks from the LRU_old pointer onward, including
the block pointed to, must be buf_pool.LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV
of the whole LRU list length, except that the tolerance defined below
is allowed. Note that the tolerance must be small enough such that for
even the BUF_LRU_OLD_MIN_LEN long LRU list, the LRU_old pointer is not
allowed to point to either end of the LRU list. */

static const ulint BUF_LRU_OLD_TOLERANCE = 20;

/** The minimum amount of non-old blocks when the LRU_old list exists
(that is, when there are more than BUF_LRU_OLD_MIN_LEN blocks).
@see buf_LRU_old_adjust_len */
#define BUF_LRU_NON_OLD_MIN_LEN	5

#ifdef BTR_CUR_HASH_ADAPT
/** When dropping the search hash index entries before deleting an ibd
file, we build a local array of pages belonging to that tablespace
in the buffer pool. Following is the size of that array.
We also release buf_pool.mutex after scanning this many pages of the
flush_list when dropping a table. This is to ensure that other threads
are not blocked for extended period of time when using very large
buffer pools. */
static const ulint BUF_LRU_DROP_SEARCH_SIZE = 1024;
#endif /* BTR_CUR_HASH_ADAPT */

/** We scan these many blocks when looking for a clean page to evict
during LRU eviction. */
static const ulint BUF_LRU_SEARCH_SCAN_THRESHOLD = 100;

/** If we switch on the InnoDB monitor because there are too few available
frames in the buffer pool, we set this to TRUE */
static bool buf_lru_switched_on_innodb_mon = false;

/** True if diagnostic message about difficult to find free blocks
in the buffer bool has already printed. */
static bool	buf_lru_free_blocks_error_printed;

/******************************************************************//**
These statistics are not 'of' LRU but 'for' LRU.  We keep count of I/O
and page_zip_decompress() operations.  Based on the statistics,
buf_LRU_evict_from_unzip_LRU() decides if we want to evict from
unzip_LRU or the regular LRU.  From unzip_LRU, we will only evict the
uncompressed frame (meaning we can evict dirty blocks as well).  From
the regular LRU, we will evict the entire block (i.e.: both the
uncompressed and compressed data), which must be clean. */

/* @{ */

/** Number of intervals for which we keep the history of these stats.
Each interval is 1 second, defined by the rate at which
srv_error_monitor_thread() calls buf_LRU_stat_update(). */
static const ulint BUF_LRU_STAT_N_INTERVAL = 50;

/** Co-efficient with which we multiply I/O operations to equate them
with page_zip_decompress() operations. */
static const ulint BUF_LRU_IO_TO_UNZIP_FACTOR = 50;

/** Sampled values buf_LRU_stat_cur.
Not protected by any mutex.  Updated by buf_LRU_stat_update(). */
static buf_LRU_stat_t		buf_LRU_stat_arr[BUF_LRU_STAT_N_INTERVAL];

/** Cursor to buf_LRU_stat_arr[] that is updated in a round-robin fashion. */
static ulint			buf_LRU_stat_arr_ind;

/** Current operation counters.  Not protected by any mutex.  Cleared
by buf_LRU_stat_update(). */
buf_LRU_stat_t	buf_LRU_stat_cur;

/** Running sum of past values of buf_LRU_stat_cur.
Updated by buf_LRU_stat_update().  Not Protected by any mutex. */
buf_LRU_stat_t	buf_LRU_stat_sum;

/* @} */

/** @name Heuristics for detecting index scan @{ */
/** Move blocks to "new" LRU list only if the first access was at
least this many milliseconds ago.  Not protected by any mutex or latch. */
uint	buf_LRU_old_threshold_ms;
/* @} */

/** Remove bpage from buf_pool.LRU and buf_pool.page_hash.

If bpage->state() == BUF_BLOCK_ZIP_PAGE && !bpage->oldest_modification(),
the object will be freed.

@param bpage      buffer block
@param id         page identifier
@param hash_lock  buf_pool.page_hash latch (will be released here)
@param zip        whether bpage->zip of BUF_BLOCK_FILE_PAGE should be freed

If a compressed page is freed other compressed pages may be relocated.
@retval true if BUF_BLOCK_FILE_PAGE was removed from page_hash. The
caller needs to free the page to the free list
@retval false if BUF_BLOCK_ZIP_PAGE was removed from page_hash. In
this case the block is already returned to the buddy allocator. */
static bool buf_LRU_block_remove_hashed(buf_page_t *bpage, const page_id_t id,
                                        page_hash_latch *hash_lock, bool zip);

/** Free a block to buf_pool */
static void buf_LRU_block_free_hashed_page(buf_block_t *block)
{
  block->page.free_file_page();
  buf_LRU_block_free_non_file_page(block);
}

/** Increase LRU size in bytes by the page size.
@param[in]	bpage		control block */
static inline void incr_LRU_size_in_bytes(const buf_page_t* bpage)
{
	/* FIXME: use atomics, not mutex */
	ut_ad(mutex_own(&buf_pool.mutex));

	buf_pool.stat.LRU_bytes += bpage->physical_size();

	ut_ad(buf_pool.stat.LRU_bytes <= buf_pool.curr_pool_size);
}

/** @return whether the unzip_LRU list should be used for evicting a victim
instead of the general LRU list */
bool buf_LRU_evict_from_unzip_LRU()
{
	ut_ad(mutex_own(&buf_pool.mutex));

	/* If the unzip_LRU list is empty, we can only use the LRU. */
	if (UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0) {
		return false;
	}

	/* If unzip_LRU is at most 10% of the size of the LRU list,
	then use the LRU.  This slack allows us to keep hot
	decompressed pages in the buffer pool. */
	if (UT_LIST_GET_LEN(buf_pool.unzip_LRU)
	    <= UT_LIST_GET_LEN(buf_pool.LRU) / 10) {
		return false;
	}

	/* If eviction hasn't started yet, we assume by default
	that a workload is disk bound. */
	if (buf_pool.freed_page_clock == 0) {
		return true;
	}

	/* Calculate the average over past intervals, and add the values
	of the current interval. */
	ulint	io_avg = buf_LRU_stat_sum.io / BUF_LRU_STAT_N_INTERVAL
		+ buf_LRU_stat_cur.io;

	ulint	unzip_avg = buf_LRU_stat_sum.unzip / BUF_LRU_STAT_N_INTERVAL
		+ buf_LRU_stat_cur.unzip;

	/* Decide based on our formula.  If the load is I/O bound
	(unzip_avg is smaller than the weighted io_avg), evict an
	uncompressed frame from unzip_LRU.  Otherwise we assume that
	the load is CPU bound and evict from the regular LRU. */
	return(unzip_avg <= io_avg * BUF_LRU_IO_TO_UNZIP_FACTOR);
}

#ifdef BTR_CUR_HASH_ADAPT
/**
While flushing (or removing dirty) pages from a tablespace we don't
want to hog the CPU and resources. Release the buffer pool and block
mutex and try to force a context switch. Then reacquire the same mutexes.
The current page is "fixed" before the release of the mutexes and then
"unfixed" again once we have reacquired the mutexes.
@param[in,out]	bpage	current page */
static void buf_flush_yield(buf_page_t *bpage)
{
  mutex_exit(&buf_pool.flush_list_mutex);
  ut_ad(bpage->oldest_modification());
  ut_ad(bpage->in_file());
  ut_ad(bpage->io_fix() == BUF_IO_NONE);
  /** Make the block sticky, so that even after we release buf_pool.mutex:
  (1) it cannot be removed from the buf_pool.flush_list
  (2) bpage cannot be relocated in buf_pool
  (3) bpage->in_LRU_list cannot change
  However, bpage->LRU can change. */
  bpage->set_io_fix(BUF_IO_PIN);
  mutex_exit(&buf_pool.mutex);

  /* Try and force a context switch. */
  os_thread_yield();

  mutex_enter(&buf_pool.mutex);
  bpage->io_unfix();
  mutex_enter(&buf_pool.flush_list_mutex);
  /* Should not have been removed from the flush
  list during the yield. However, this check is
  not sufficient to catch a remove -> add. */
  ut_ad(bpage->oldest_modification());
}

/******************************************************************//**
If we have hogged the resources for too long then release the buffer
pool and flush list mutex and do a thread yield. Set the current page
to "sticky" so that it is not relocated during the yield.
@return true if yielded */
static	MY_ATTRIBUTE((warn_unused_result))
bool
buf_flush_try_yield(
/*================*/
	buf_page_t*	bpage,		/*!< in/out: bpage to remove */
	ulint		processed)	/*!< in: number of pages processed */
{
	/* Every BUF_LRU_DROP_SEARCH_SIZE iterations in the
	loop we release buf_pool.mutex to let other threads
	do their job but only if the block is not IO fixed. This
	ensures that the block stays in its position in the
	flush_list. */

	if (bpage != NULL
	    && processed >= BUF_LRU_DROP_SEARCH_SIZE
	    && bpage->io_fix() == BUF_IO_NONE) {

		/* Release the buf_pool.mutex
		to give the other threads a go. */

		buf_flush_yield(bpage);
		return(true);
	}

	return(false);
}
#endif /* BTR_CUR_HASH_ADAPT */

/** Remove a single page from flush_list.
@param[in,out]	bpage		buffer page to remove
@param[in]	flush		whether to flush the page before removing
@return true if page was removed. */
static bool buf_flush_or_remove_page(buf_page_t *bpage, bool flush)
{
	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(mutex_own(&buf_pool.flush_list_mutex));

	/* bpage->id and bpage->io_fix are protected by
	buf_pool.mutex (and bpage->id additionally by hash_lock).
	It is safe to check them while holding buf_pool.mutex only. */

	if (bpage->io_fix() != BUF_IO_NONE) {

		/* We cannot remove this page during this scan
		yet; maybe the system is currently reading it
		in, or flushing the modifications to the file */
		return(false);

	}

	bool		processed = false;

	/* We have to release the flush_list_mutex to obey the
	latching order. We are however guaranteed that the page
	will stay in the flush_list and won't be relocated because
	buf_flush_remove() and buf_flush_relocate_on_flush_list()
	need buf_pool.mutex as well. */

	mutex_exit(&buf_pool.flush_list_mutex);

	ut_ad(bpage->oldest_modification());

	if (!flush) {
		buf_flush_remove(bpage);
		processed = true;
	} else if (bpage->ready_for_flush()) {
		processed = buf_flush_page(bpage, IORequest::SINGLE_PAGE,
					   nullptr, false);

		if (processed) {
			mutex_enter(&buf_pool.mutex);
		}
	}

	mutex_enter(&buf_pool.flush_list_mutex);

	ut_ad(mutex_own(&buf_pool.mutex));

	return(processed);
}

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param[in]	id		tablespace identifier
@param[in]	flush		whether to flush the pages before removing
@param[in]	first		first page to be flushed or evicted
@return	whether all matching dirty pages were removed */
static bool buf_flush_or_remove_pages(ulint id, bool flush, ulint first)
{
	buf_page_t*	prev;
	buf_page_t*	bpage;
	ulint		processed = 0;

	mutex_enter(&buf_pool.flush_list_mutex);
rescan:
	bool	all_freed = true;

	for (bpage = UT_LIST_GET_LAST(buf_pool.flush_list);
	     bpage != NULL;
	     bpage = prev) {

		ut_a(bpage->in_file());

		/* Save the previous link because once we free the
		page we can't rely on the links. */

		prev = UT_LIST_GET_PREV(list, bpage);

		const page_id_t bpage_id(bpage->id());

		if (id != bpage_id.space()) {
			/* Skip this block, because it is for a
			different tablespace. */
		} else if (bpage_id.page_no() < first) {
			/* Skip this block, because it is below the limit. */
		} else if (!buf_flush_or_remove_page(bpage, flush)) {

			/* Remove was unsuccessful, we have to try again
			by scanning the entire list from the end.
			This also means that we never released the
			buf_pool mutex. Therefore we can trust the prev
			pointer.
			buf_flush_or_remove_page() released the
			flush list mutex but not the buf_pool mutex.
			Therefore it is possible that a new page was
			added to the flush list. For example, in case
			where we are at the head of the flush list and
			prev == NULL. That is OK because we have the
			tablespace quiesced and no new pages for this
			space-id should enter flush_list. This is
			because the only callers of this function are
			DROP TABLE and FLUSH TABLE FOR EXPORT.
			We know that we'll have to do at least one more
			scan but we don't break out of loop here and
			try to do as much work as we can in this
			iteration. */

			all_freed = false;
		} else if (flush) {

			/* The processing was successful. And during the
			processing we have released the buf_pool mutex
			when calling buf_page_flush(). We cannot trust
			prev pointer. */
			goto rescan;
		}

#ifdef BTR_CUR_HASH_ADAPT
		++processed;

		/* Yield if we have hogged the CPU and mutexes for too long. */
		if (buf_flush_try_yield(prev, processed)) {
			/* Reset the batch size counter if we had to yield. */
			processed = 0;
		}
#endif /* BTR_CUR_HASH_ADAPT */
	}

	mutex_exit(&buf_pool.flush_list_mutex);

	return(all_freed);
}

/** Remove or flush all the dirty pages that belong to a given tablespace.
The pages will remain in the LRU list and will be evicted from the LRU list
as they age and move towards the tail of the LRU list.
@param[in]	id		tablespace identifier
@param[in]	flush		whether to flush the pages before removing
@param[in]	first		first page to be flushed or evicted */
static void buf_flush_dirty_pages(ulint id, bool flush, ulint first)
{
  mutex_enter(&buf_pool.mutex);
  while (!buf_flush_or_remove_pages(id, flush, first))
  {
    mutex_exit(&buf_pool.mutex);
    ut_d(buf_flush_validate());
    os_thread_sleep(2000);
    mutex_enter(&buf_pool.mutex);
  }

#ifdef UNIV_DEBUG
  if (!first)
  {
    mutex_enter(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_FIRST(buf_pool.flush_list); bpage;
         bpage= UT_LIST_GET_NEXT(list, bpage))
    {
      ut_ad(bpage->in_file());
      ut_ad(bpage->oldest_modification());
      ut_ad(id != bpage->id().space());
    }

    mutex_exit(&buf_pool.flush_list_mutex);
  }
#endif

  mutex_exit(&buf_pool.mutex);
}

/** Empty the flush list for all pages belonging to a tablespace.
@param[in]	id		tablespace identifier
@param[in]	flush		whether to write the pages to files
@param[in]	first		first page to be flushed or evicted */
void buf_LRU_flush_or_remove_pages(ulint id, bool flush, ulint first)
{
	/* Pages in the system tablespace must never be discarded. */
	ut_ad(id || flush);

	buf_flush_dirty_pages(id, flush, first);

	if (flush) {
		/* Ensure that all asynchronous IO is completed. */
		os_aio_wait_until_no_pending_writes();
		fil_flush(id);
	}
}

#ifdef UNIV_DEBUG
/********************************************************************//**
Insert a compressed block into buf_pool.zip_clean in the LRU order. */
void
buf_LRU_insert_zip_clean(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(bpage->state() == BUF_BLOCK_ZIP_PAGE);
	ut_ad(!bpage->oldest_modification());

	/* Find the first successor of bpage in the LRU list
	that is in the zip_clean list. */
	buf_page_t*	b = bpage;

	do {
		b = UT_LIST_GET_NEXT(LRU, b);
	} while (b && (b->state() != BUF_BLOCK_ZIP_PAGE
		       || b->oldest_modification()));

	/* Insert bpage before b, i.e., after the predecessor of b. */
	if (b != NULL) {
		b = UT_LIST_GET_PREV(list, b);
	}

	if (b != NULL) {
		UT_LIST_INSERT_AFTER(buf_pool.zip_clean, b, bpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.zip_clean, bpage);
	}
}
#endif /* UNIV_DEBUG */

/** Try to free an uncompressed page of a compressed block from the unzip
LRU list.  The compressed page is preserved, and it need not be clean.
@param[in]	scan_all	true=scan the whole list;
				false=scan srv_LRU_scan_depth / 2 blocks
@return true if freed */
static bool buf_LRU_free_from_unzip_LRU_list(bool scan_all)
{
	ut_ad(mutex_own(&buf_pool.mutex));

	if (!buf_LRU_evict_from_unzip_LRU()) {
		return(false);
	}

	ulint	scanned = 0;
	const ulint limit = scan_all ? ULINT_UNDEFINED : srv_LRU_scan_depth;
	bool	freed = false;

	for (buf_block_t* block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
	     block && scanned < limit; ++scanned) {
		buf_block_t* prev_block = UT_LIST_GET_PREV(unzip_LRU, block);

		ut_ad(block->page.state() == BUF_BLOCK_FILE_PAGE);
		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);

		freed = buf_LRU_free_page(&block->page, false);
		if (freed) {
			break;
		}

		block = prev_block;
	}

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_UNZIP_SEARCH_SCANNED,
			MONITOR_LRU_UNZIP_SEARCH_SCANNED_NUM_CALL,
			MONITOR_LRU_UNZIP_SEARCH_SCANNED_PER_CALL,
			scanned);
	}

	return(freed);
}

/** Try to free a clean page from the common LRU list.
@param[in]	scan_all	true=scan the whole LRU list
				false=use BUF_LRU_SEARCH_SCAN_THRESHOLD
@return whether a page was freed */
static bool buf_LRU_free_from_common_LRU_list(bool scan_all)
{
	ut_ad(mutex_own(&buf_pool.mutex));

	ulint		scanned = 0;
	bool		freed = false;

	for (buf_page_t* bpage = buf_pool.lru_scan_itr.start();
	     bpage && (scan_all || scanned < BUF_LRU_SEARCH_SCAN_THRESHOLD);
	     ++scanned, bpage = buf_pool.lru_scan_itr.get()) {
		buf_page_t*	prev = UT_LIST_GET_PREV(LRU, bpage);
		buf_pool.lru_scan_itr.set(prev);

		const auto accessed = bpage->is_accessed();
		freed = bpage->ready_for_replace();

		if (freed) {
			freed = buf_LRU_free_page(bpage, true);
			if (!freed) {
				continue;
			}

			if (!accessed) {
				/* Keep track of pages that are evicted without
				ever being accessed. This gives us a measure of
				the effectiveness of readahead */
				++buf_pool.stat.n_ra_pages_evicted;
			}

			break;
		}
	}

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_SEARCH_SCANNED,
			MONITOR_LRU_SEARCH_SCANNED_NUM_CALL,
			MONITOR_LRU_SEARCH_SCANNED_PER_CALL,
			scanned);
	}

	return(freed);
}

/** Try to free a replaceable block.
@param[in]	scan_all	true=scan the whole LRU list,
				false=use BUF_LRU_SEARCH_SCAN_THRESHOLD
@return true if found and freed */
bool buf_LRU_scan_and_free_block(bool scan_all)
{
	ut_ad(mutex_own(&buf_pool.mutex));

	return(buf_LRU_free_from_unzip_LRU_list(scan_all)
	       || buf_LRU_free_from_common_LRU_list(scan_all));
}

/** @return a buffer block from the buf_pool.free list
@retval	NULL	if the free list is empty */
buf_block_t* buf_LRU_get_free_only()
{
	buf_block_t*	block;

	ut_ad(mutex_own(&buf_pool.mutex));

	block = reinterpret_cast<buf_block_t*>(
		UT_LIST_GET_FIRST(buf_pool.free));

	while (block != NULL) {
		ut_ad(block->page.in_free_list);
		ut_d(block->page.in_free_list = FALSE);
		ut_ad(!block->page.oldest_modification());
		ut_ad(!block->page.in_LRU_list);
		ut_a(!block->page.in_file());
		UT_LIST_REMOVE(buf_pool.free, &block->page);

		if (buf_pool.curr_size >= buf_pool.old_size
		    || UT_LIST_GET_LEN(buf_pool.withdraw)
			>= buf_pool.withdraw_target
		    || !buf_pool.will_be_withdrawn(block->page)) {
			/* No adaptive hash index entries may point to
			a free block. */
			assert_block_ahi_empty(block);

			block->page.set_state(BUF_BLOCK_MEMORY);
			MEM_MAKE_ADDRESSABLE(block->frame, srv_page_size);
			break;
		}

		/* This should be withdrawn */
		UT_LIST_ADD_LAST(
			buf_pool.withdraw,
			&block->page);
		ut_d(block->in_withdraw_list = true);

		block = reinterpret_cast<buf_block_t*>(
			UT_LIST_GET_FIRST(buf_pool.free));
	}

	return(block);
}

/******************************************************************//**
Checks how much of buf_pool is occupied by non-data objects like
AHI, lock heaps etc. Depending on the size of non-data objects this
function will either assert or issue a warning and switch on the
status monitor. */
static void buf_LRU_check_size_of_non_data_objects()
{
	ut_ad(mutex_own(&buf_pool.mutex));

	if (!recv_recovery_is_on()
	    && buf_pool.curr_size == buf_pool.old_size
	    && UT_LIST_GET_LEN(buf_pool.free)
	    + UT_LIST_GET_LEN(buf_pool.LRU) < buf_pool.curr_size / 20) {

		ib::fatal() << "Over 95 percent of the buffer pool is"
			" occupied by lock heaps"
#ifdef BTR_CUR_HASH_ADAPT
			" or the adaptive hash index!"
#endif /* BTR_CUR_HASH_ADAPT */
			" Check that your transactions do not set too many"
			" row locks, or review if"
			" innodb_buffer_pool_size="
			<< (buf_pool.curr_size >> (20U - srv_page_size_shift))
			<< "M could be bigger.";
	} else if (!recv_recovery_is_on()
		   && buf_pool.curr_size == buf_pool.old_size
		   && (UT_LIST_GET_LEN(buf_pool.free)
		       + UT_LIST_GET_LEN(buf_pool.LRU))
		   < buf_pool.curr_size / 3) {

		if (!buf_lru_switched_on_innodb_mon && srv_monitor_timer) {

			/* Over 67 % of the buffer pool is occupied by lock
			heaps or the adaptive hash index. This may be a memory
			leak! */

			ib::warn() << "Over 67 percent of the buffer pool is"
				" occupied by lock heaps"
#ifdef BTR_CUR_HASH_ADAPT
				" or the adaptive hash index!"
#endif /* BTR_CUR_HASH_ADAPT */
				" Check that your transactions do not"
				" set too many row locks."
				" innodb_buffer_pool_size="
				<< (buf_pool.curr_size >>
				    (20U - srv_page_size_shift)) << "M."
				" Starting the InnoDB Monitor to print"
				" diagnostics.";

			buf_lru_switched_on_innodb_mon = true;
			srv_print_innodb_monitor = TRUE;
			srv_monitor_timer_schedule_now();
		}

	} else if (buf_lru_switched_on_innodb_mon) {

		/* Switch off the InnoDB Monitor; this is a simple way
		to stop the monitor if the situation becomes less urgent,
		but may also surprise users if the user also switched on the
		monitor! */

		buf_lru_switched_on_innodb_mon = false;
		srv_print_innodb_monitor = FALSE;
	}
}

/** Get a free block from the buf_pool. The block is taken off the
free list. If free list is empty, blocks are moved from the end of the
LRU list to the free list.

This function is called from a user thread when it needs a clean
block to read in a page. Note that we only ever get a block from
the free list. Even when we flush a page or find a page in LRU scan
we put it to free list to be used.
* iteration 0:
  * get a block from free list, success:done
  * if buf_pool.try_LRU_scan is set
    * scan LRU up to srv_LRU_scan_depth to find a clean block
    * the above will put the block on free list
    * success:retry the free list
  * flush one dirty page from tail of LRU to disk
    * the above will put the block on free list
    * success: retry the free list
* iteration 1:
  * same as iteration 0 except:
    * scan whole LRU list
    * scan LRU list even if buf_pool.try_LRU_scan is not set
* iteration > 1:
  * same as iteration 1 but sleep 10ms

@param have_mutex  whether buf_pool.mutex is already being held
@return the free control block, in state BUF_BLOCK_MEMORY */
buf_block_t* buf_LRU_get_free_block(bool have_mutex)
{
	buf_block_t*	block		= NULL;
	bool		freed		= false;
	ulint		n_iterations	= 0;
	ulint		flush_failures	= 0;
	MONITOR_INC(MONITOR_LRU_GET_FREE_SEARCH);
	if (have_mutex) {
		ut_ad(mutex_own(&buf_pool.mutex));
		goto got_mutex;
	}
loop:
	mutex_enter(&buf_pool.mutex);
got_mutex:

	buf_LRU_check_size_of_non_data_objects();

	DBUG_EXECUTE_IF("ib_lru_force_no_free_page",
		if (!buf_lru_free_blocks_error_printed) {
			n_iterations = 21;
			goto not_found;});

	/* If there is a block in the free list, take it */
	block = buf_LRU_get_free_only();

	if (block) {
		if (!have_mutex) {
			mutex_exit(&buf_pool.mutex);
		}
		memset(&block->page.zip, 0, sizeof block->page.zip);
		return(block);
	}

	MONITOR_INC( MONITOR_LRU_GET_FREE_LOOPS );
	freed = false;
	if (n_iterations || buf_pool.try_LRU_scan) {
		/* If no block was in the free list, search from the
		end of the LRU list and try to free a block there.
		If we are doing for the first time we'll scan only
		tail of the LRU list otherwise we scan the whole LRU
		list. */
		freed = buf_LRU_scan_and_free_block(n_iterations > 0);

		if (!freed && n_iterations == 0) {
			/* Tell other threads that there is no point
			in scanning the LRU list. This flag is set to
			TRUE again when we flush a batch from this
			buffer pool. */
			buf_pool.try_LRU_scan = false;

			/* Also tell the page_cleaner thread that
			there is work for it to do. */
			os_event_set(buf_flush_event);
		}
	}

#ifndef DBUG_OFF
not_found:
#endif

	mutex_exit(&buf_pool.mutex);

	if (freed) {
		goto loop;
	}

	if (n_iterations > 20 && !buf_lru_free_blocks_error_printed
	    && srv_buf_pool_old_size == srv_buf_pool_size) {

		ib::warn() << "Difficult to find free blocks in the buffer pool"
			" (" << n_iterations << " search iterations)! "
			<< flush_failures << " failed attempts to"
			" flush a page!"
			" Consider increasing innodb_buffer_pool_size."
			" Pending flushes (fsync) log: "
			<< log_sys.get_pending_flushes()
			<< "; buffer pool: "
			<< fil_n_pending_tablespace_flushes
			<< ". " << os_n_file_reads << " OS file reads, "
			<< os_n_file_writes << " OS file writes, "
			<< os_n_fsyncs
			<< " OS fsyncs.";

		buf_lru_free_blocks_error_printed = true;
	}

	/* If we have scanned the whole LRU and still are unable to
	find a free block then we should sleep here to let the
	page_cleaner do an LRU batch for us. */

	if (!srv_read_only_mode) {
		os_event_set(buf_flush_event);
	}

	if (n_iterations > 1) {

		MONITOR_INC( MONITOR_LRU_GET_FREE_WAITS );
		os_thread_sleep(10000);
	}

	/* No free block was found: try to flush the LRU list.
	This call will flush one page from the LRU and put it on the
	free list. That means that the free block is up for grabs for
	all user threads.

	TODO: A more elegant way would have been to return the freed
	up block to the caller here but the code that deals with
	removing the block from page_hash and LRU_list is fairly
	involved (particularly in case of compressed pages). We
	can do that in a separate patch sometime in future. */

	if (!buf_flush_single_page_from_LRU()) {
		MONITOR_INC(MONITOR_LRU_SINGLE_FLUSH_FAILURE_COUNT);
		++flush_failures;
	}

	srv_stats.buf_pool_wait_free.inc();

	n_iterations++;

	goto loop;
}

/** Move the LRU_old pointer so that the length of the old blocks list
is inside the allowed limits. */
static void buf_LRU_old_adjust_len()
{
	ulint	old_len;
	ulint	new_len;

	ut_a(buf_pool.LRU_old);
	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(buf_pool.LRU_old_ratio >= BUF_LRU_OLD_RATIO_MIN);
	ut_ad(buf_pool.LRU_old_ratio <= BUF_LRU_OLD_RATIO_MAX);
	compile_time_assert(BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN
			    > BUF_LRU_OLD_RATIO_DIV
			    * (BUF_LRU_OLD_TOLERANCE + 5));
	compile_time_assert(BUF_LRU_NON_OLD_MIN_LEN < BUF_LRU_OLD_MIN_LEN);

#ifdef UNIV_LRU_DEBUG
	/* buf_pool.LRU_old must be the first item in the LRU list
	whose "old" flag is set. */
	ut_a(buf_pool.LRU_old->old);
	ut_a(!UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)
	     || !UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)->old);
	ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)
	     || UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */

	old_len = buf_pool.LRU_old_len;
	new_len = ut_min(UT_LIST_GET_LEN(buf_pool.LRU)
			 * buf_pool.LRU_old_ratio / BUF_LRU_OLD_RATIO_DIV,
			 UT_LIST_GET_LEN(buf_pool.LRU)
			 - (BUF_LRU_OLD_TOLERANCE
			    + BUF_LRU_NON_OLD_MIN_LEN));

	for (;;) {
		buf_page_t*	LRU_old = buf_pool.LRU_old;

		ut_a(LRU_old);
		ut_ad(LRU_old->in_LRU_list);
#ifdef UNIV_LRU_DEBUG
		ut_a(LRU_old->old);
#endif /* UNIV_LRU_DEBUG */

		/* Update the LRU_old pointer if necessary */

		if (old_len + BUF_LRU_OLD_TOLERANCE < new_len) {

			buf_pool.LRU_old = LRU_old = UT_LIST_GET_PREV(
				LRU, LRU_old);
#ifdef UNIV_LRU_DEBUG
			ut_a(!LRU_old->old);
#endif /* UNIV_LRU_DEBUG */
			old_len = ++buf_pool.LRU_old_len;
			LRU_old->set_old(true);

		} else if (old_len > new_len + BUF_LRU_OLD_TOLERANCE) {

			buf_pool.LRU_old = UT_LIST_GET_NEXT(LRU, LRU_old);
			old_len = --buf_pool.LRU_old_len;
			LRU_old->set_old(false);
		} else {
			return;
		}
	}
}

/** Initialize the old blocks pointer in the LRU list. This function should be
called when the LRU list grows to BUF_LRU_OLD_MIN_LEN length. */
static void buf_LRU_old_init()
{
	ut_ad(mutex_own(&buf_pool.mutex));
	ut_a(UT_LIST_GET_LEN(buf_pool.LRU) == BUF_LRU_OLD_MIN_LEN);

	/* We first initialize all blocks in the LRU list as old and then use
	the adjust function to move the LRU_old pointer to the right
	position */

	for (buf_page_t* bpage = UT_LIST_GET_LAST(buf_pool.LRU);
	     bpage != NULL;
	     bpage = UT_LIST_GET_PREV(LRU, bpage)) {

		ut_ad(bpage->in_LRU_list);

		/* This loop temporarily violates the
		assertions of buf_page_t::set_old(). */
		bpage->old = true;
	}

	buf_pool.LRU_old = UT_LIST_GET_FIRST(buf_pool.LRU);
	buf_pool.LRU_old_len = UT_LIST_GET_LEN(buf_pool.LRU);

	buf_LRU_old_adjust_len();
}

/** Remove a block from the unzip_LRU list if it belonged to the list.
@param[in]	bpage	control block */
static void buf_unzip_LRU_remove_block_if_needed(buf_page_t* bpage)
{
	ut_ad(bpage->in_file());
	ut_ad(mutex_own(&buf_pool.mutex));

	if (bpage->belongs_to_unzip_LRU()) {
		buf_block_t*	block = reinterpret_cast<buf_block_t*>(bpage);

		ut_ad(block->in_unzip_LRU_list);
		ut_d(block->in_unzip_LRU_list = false);

		UT_LIST_REMOVE(buf_pool.unzip_LRU, block);
	}
}

/** Removes a block from the LRU list.
@param[in]	bpage	control block */
static inline void buf_LRU_remove_block(buf_page_t* bpage)
{
	/* Important that we adjust the hazard pointers before removing
	bpage from the LRU list. */
	buf_page_t* prev_bpage = buf_pool.LRU_remove(bpage);

	/* If the LRU_old pointer is defined and points to just this block,
	move it backward one step */

	if (bpage == buf_pool.LRU_old) {

		/* Below: the previous block is guaranteed to exist,
		because the LRU_old pointer is only allowed to differ
		by BUF_LRU_OLD_TOLERANCE from strict
		buf_pool.LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV of the LRU
		list length. */
		ut_a(prev_bpage);
#ifdef UNIV_LRU_DEBUG
		ut_a(!prev_bpage->old);
#endif /* UNIV_LRU_DEBUG */
		buf_pool.LRU_old = prev_bpage;
		prev_bpage->set_old(true);

		buf_pool.LRU_old_len++;
	}

	buf_pool.stat.LRU_bytes -= bpage->physical_size();

	buf_unzip_LRU_remove_block_if_needed(bpage);

	/* If the LRU list is so short that LRU_old is not defined,
	clear the "old" flags and return */
	if (UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN) {

		for (buf_page_t* bpage = UT_LIST_GET_FIRST(buf_pool.LRU);
		     bpage != NULL;
		     bpage = UT_LIST_GET_NEXT(LRU, bpage)) {

			/* This loop temporarily violates the
			assertions of buf_page_t::set_old(). */
			bpage->old = false;
		}

		buf_pool.LRU_old = NULL;
		buf_pool.LRU_old_len = 0;

		return;
	}

	ut_ad(buf_pool.LRU_old);

	/* Update the LRU_old_len field if necessary */
	if (bpage->old) {
		buf_pool.LRU_old_len--;
	}

	/* Adjust the length of the old block list if necessary */
	buf_LRU_old_adjust_len();
}

/******************************************************************//**
Adds a block to the LRU list of decompressed zip pages. */
void
buf_unzip_LRU_add_block(
/*====================*/
	buf_block_t*	block,	/*!< in: control block */
	ibool		old)	/*!< in: TRUE if should be put to the end
				of the list, else put to the start */
{
	ut_ad(mutex_own(&buf_pool.mutex));
	ut_a(block->page.belongs_to_unzip_LRU());
	ut_ad(!block->in_unzip_LRU_list);
	ut_d(block->in_unzip_LRU_list = true);

	if (old) {
		UT_LIST_ADD_LAST(buf_pool.unzip_LRU, block);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.unzip_LRU, block);
	}
}

/******************************************************************//**
Adds a block to the LRU list. Please make sure that the page_size is
already set when invoking the function, so that we can get correct
page_size from the buffer page when adding a block into LRU */
void
buf_LRU_add_block(
	buf_page_t*	bpage,	/*!< in: control block */
	bool		old)	/*!< in: true if should be put to the old blocks
				in the LRU list, else put to the start; if the
				LRU list is very short, the block is added to
				the start, regardless of this parameter */
{
	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(!bpage->in_LRU_list);

	if (!old || (UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN)) {

		UT_LIST_ADD_FIRST(buf_pool.LRU, bpage);

		bpage->freed_page_clock = buf_pool.freed_page_clock
			& ((1U << 31) - 1);
	} else {
#ifdef UNIV_LRU_DEBUG
		/* buf_pool.LRU_old must be the first item in the LRU list
		whose "old" flag is set. */
		ut_a(buf_pool.LRU_old->old);
		ut_a(!UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)
		     || !UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)->old);
		ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)
		     || UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)->old);
#endif /* UNIV_LRU_DEBUG */
		UT_LIST_INSERT_AFTER(buf_pool.LRU, buf_pool.LRU_old,
			bpage);

		buf_pool.LRU_old_len++;
	}

	ut_d(bpage->in_LRU_list = TRUE);

	incr_LRU_size_in_bytes(bpage);

	if (UT_LIST_GET_LEN(buf_pool.LRU) > BUF_LRU_OLD_MIN_LEN) {

		ut_ad(buf_pool.LRU_old);

		/* Adjust the length of the old block list if necessary */

		bpage->set_old(old);
		buf_LRU_old_adjust_len();

	} else if (UT_LIST_GET_LEN(buf_pool.LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init();
	} else {
		bpage->set_old(buf_pool.LRU_old != NULL);
	}

	/* If this is a zipped block with decompressed frame as well
	then put it on the unzip_LRU list */
	if (bpage->belongs_to_unzip_LRU()) {
		buf_unzip_LRU_add_block((buf_block_t*) bpage, old);
	}
}

/** Move a block to the start of the LRU list. */
void buf_page_make_young(buf_page_t *bpage)
{
  ut_ad(bpage->in_file());

  mutex_enter(&buf_pool.mutex);

  if (UNIV_UNLIKELY(bpage->old))
    buf_pool.stat.n_pages_made_young++;

  buf_LRU_remove_block(bpage);
  buf_LRU_add_block(bpage, false);

  mutex_exit(&buf_pool.mutex);
}

/** Try to free a block. If bpage is a descriptor of a compressed-only
ROW_FORMAT=COMPRESSED page, the buf_page_t object will be freed as well.
The caller must hold buf_pool.mutex.
@param bpage      block to be freed
@param zip        whether to remove both copies of a ROW_FORMAT=COMPRESSED page
@retval true if freed and buf_pool.mutex may have been temporarily released
@retval false if the page was not freed */
bool buf_LRU_free_page(buf_page_t *bpage, bool zip)
{
	const page_id_t id(bpage->id());
	buf_page_t*	b = nullptr;

	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(bpage->in_file());
	ut_ad(bpage->in_LRU_list);

	/* First, perform a quick check before we acquire hash_lock. */
	if (!bpage->can_relocate()) {
		return false;
	}

	/* We must hold an exclusive hash_lock to prevent
	bpage->can_relocate() from changing due to a concurrent
	execution of buf_page_get_low(). */
	const ulint fold = id.fold();
	page_hash_latch* hash_lock = buf_pool.page_hash.lock_get(fold);
	hash_lock->write_lock();

	if (UNIV_UNLIKELY(!bpage->can_relocate())) {
		/* Do not free buffer fixed and I/O-fixed blocks. */
		goto func_exit;
	}

	if (zip || !bpage->zip.data) {
		/* This would completely free the block. */
		/* Do not completely free dirty blocks. */

		if (bpage->oldest_modification()) {
			goto func_exit;
		}
	} else if (bpage->oldest_modification()
		   && bpage->state() != BUF_BLOCK_FILE_PAGE) {
func_exit:
		hash_lock->write_unlock();
		return(false);

	} else if (bpage->state() == BUF_BLOCK_FILE_PAGE) {
		b = buf_page_alloc_descriptor();
		ut_a(b);
		new (b) buf_page_t(*bpage);
		b->set_state(BUF_BLOCK_ZIP_PAGE);
	}

	ut_ad(mutex_own(&buf_pool.mutex));
	ut_ad(bpage->in_file());
	ut_ad(bpage->in_LRU_list);

	DBUG_PRINT("ib_buf", ("free page %u:%u",
			      id.space(), id.page_no()));

	ut_ad(bpage->can_relocate());

	if (!buf_LRU_block_remove_hashed(bpage, id, hash_lock, zip)) {
		return(true);
	}

	/* We have just freed a BUF_BLOCK_FILE_PAGE. If b != nullptr
	then it was a compressed page with an uncompressed frame and
	we are interested in freeing only the uncompressed frame.
	Therefore we have to reinsert the compressed page descriptor
	into the LRU and page_hash (and possibly flush_list).
	if !b then it was a regular page that has been freed */

	if (UNIV_LIKELY_NULL(b)) {
		buf_page_t*	prev_b	= UT_LIST_GET_PREV(LRU, b);

		hash_lock->write_lock();

		ut_ad(!buf_pool.page_hash_get_low(id, fold));
		ut_ad(b->zip_size());

		/* The field in_LRU_list of
		the to-be-freed block descriptor should have
		been cleared in
		buf_LRU_block_remove_hashed(), which
		invokes buf_LRU_remove_block(). */
		ut_ad(!bpage->in_LRU_list);

		/* bpage->state was BUF_BLOCK_FILE_PAGE because
		b != nullptr. The type cast below is thus valid. */
		ut_ad(!((buf_block_t*) bpage)->in_unzip_LRU_list);

		/* The fields of bpage were copied to b before
		buf_LRU_block_remove_hashed() was invoked. */
		ut_ad(!b->in_zip_hash);
		ut_ad(b->in_LRU_list);
		ut_ad(b->in_page_hash);

		HASH_INSERT(buf_page_t, hash, &buf_pool.page_hash, fold, b);

		/* Insert b where bpage was in the LRU list. */
		if (prev_b) {
			ulint	lru_len;

			ut_ad(prev_b->in_LRU_list);
			ut_ad(prev_b->in_file());

			UT_LIST_INSERT_AFTER(buf_pool.LRU, prev_b, b);

			incr_LRU_size_in_bytes(b);

			if (b->is_old()) {
				buf_pool.LRU_old_len++;
				if (buf_pool.LRU_old
				    == UT_LIST_GET_NEXT(LRU, b)) {

					buf_pool.LRU_old = b;
				}
			}

			lru_len = UT_LIST_GET_LEN(buf_pool.LRU);

			if (lru_len > BUF_LRU_OLD_MIN_LEN) {
				ut_ad(buf_pool.LRU_old);
				/* Adjust the length of the
				old block list if necessary */
				buf_LRU_old_adjust_len();
			} else if (lru_len == BUF_LRU_OLD_MIN_LEN) {
				/* The LRU list is now long
				enough for LRU_old to become
				defined: init it */
				buf_LRU_old_init();
			}
#ifdef UNIV_LRU_DEBUG
			/* Check that the "old" flag is consistent
			in the block and its neighbours. */
			b->set_old(b->is_old());
#endif /* UNIV_LRU_DEBUG */
		} else {
			ut_d(b->in_LRU_list = FALSE);
			buf_LRU_add_block(b, b->old);
		}

		if (!b->oldest_modification()) {
#ifdef UNIV_DEBUG
			buf_LRU_insert_zip_clean(b);
#endif /* UNIV_DEBUG */
		} else {
			/* Relocate on buf_pool.flush_list. */
			buf_flush_relocate_on_flush_list(bpage, b);
		}

		bpage->zip.data = nullptr;

		page_zip_set_size(&bpage->zip, 0);

		/* Prevent buf_page_get_gen() from
		decompressing the block while we release
		hash_lock. */
		b->set_io_fix(BUF_IO_PIN);
		hash_lock->write_unlock();
	}

	mutex_exit(&buf_pool.mutex);

	/* Remove possible adaptive hash index on the page.
	The page was declared uninitialized by
	buf_LRU_block_remove_hashed().  We need to flag
	the contents of the page valid (which it still is) in
	order to avoid bogus Valgrind or MSAN warnings.*/
	buf_block_t* block = reinterpret_cast<buf_block_t*>(bpage);

	MEM_MAKE_DEFINED(block->frame, srv_page_size);
	btr_search_drop_page_hash_index(block);
	MEM_UNDEFINED(block->frame, srv_page_size);

	if (UNIV_LIKELY_NULL(b)) {
		ut_ad(b->zip_size());
		b->io_unfix();
	}

	mutex_enter(&buf_pool.mutex);
	buf_LRU_block_free_hashed_page(block);

	return(true);
}

/******************************************************************//**
Puts a block back to the free list. */
void
buf_LRU_block_free_non_file_page(
/*=============================*/
	buf_block_t*	block)	/*!< in: block, must not contain a file page */
{
	void*		data;

	ut_ad(block->page.state() == BUF_BLOCK_MEMORY);
	assert_block_ahi_empty(block);
	ut_ad(!block->page.in_free_list);
	ut_ad(!block->page.oldest_modification());
	ut_ad(!block->page.in_LRU_list);

	block->page.set_state(BUF_BLOCK_NOT_USED);

	MEM_UNDEFINED(block->frame, srv_page_size);
	/* Wipe page_no and space_id */
	static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");
	memset_aligned<4>(block->frame + FIL_PAGE_OFFSET, 0xfe, 4);
	static_assert(FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID % 4 == 2,
		      "not perfect alignment");
	memset_aligned<2>(block->frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
			  0xfe, 4);
	data = block->page.zip.data;

	if (data != NULL) {
		block->page.zip.data = NULL;
		buf_pool_mutex_exit_forbid();

		ut_ad(block->zip_size());

		buf_buddy_free(data, block->zip_size());

		buf_pool_mutex_exit_allow();
		page_zip_set_size(&block->page.zip, 0);
	}

	if (buf_pool.curr_size < buf_pool.old_size
	    && UT_LIST_GET_LEN(buf_pool.withdraw) < buf_pool.withdraw_target
	    && buf_pool.will_be_withdrawn(block->page)) {
		/* This should be withdrawn */
		UT_LIST_ADD_LAST(
			buf_pool.withdraw,
			&block->page);
		ut_d(block->in_withdraw_list = true);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.free, &block->page);
		ut_d(block->page.in_free_list = true);
	}

	MEM_NOACCESS(block->frame, srv_page_size);
}

/** Remove bpage from buf_pool.LRU and buf_pool.page_hash.

If bpage->state() == BUF_BLOCK_ZIP_PAGE && !bpage->oldest_modification(),
the object will be freed.

@param bpage      buffer block
@param id         page identifier
@param hash_lock  buf_pool.page_hash latch (will be released here)
@param zip        whether bpage->zip of BUF_BLOCK_FILE_PAGE should be freed

If a compressed page is freed other compressed pages may be relocated.
@retval true if BUF_BLOCK_FILE_PAGE was removed from page_hash. The
caller needs to free the page to the free list
@retval false if BUF_BLOCK_ZIP_PAGE was removed from page_hash. In
this case the block is already returned to the buddy allocator. */
static bool buf_LRU_block_remove_hashed(buf_page_t *bpage, const page_id_t id,
                                        page_hash_latch *hash_lock, bool zip)
{
	ut_ad(mutex_own(&buf_pool.mutex));
        ut_ad(hash_lock->is_write_locked());

	ut_a(bpage->io_fix() == BUF_IO_NONE);
	ut_a(!bpage->buf_fix_count());

	buf_LRU_remove_block(bpage);

	buf_pool.freed_page_clock += 1;

	switch (bpage->state()) {
	case BUF_BLOCK_FILE_PAGE:
		MEM_CHECK_ADDRESSABLE(bpage, sizeof(buf_block_t));
		MEM_CHECK_ADDRESSABLE(((buf_block_t*) bpage)->frame,
				      srv_page_size);
		buf_block_modify_clock_inc((buf_block_t*) bpage);
		if (bpage->zip.data) {
			const page_t*	page = ((buf_block_t*) bpage)->frame;

			ut_a(!zip || !bpage->oldest_modification());
			ut_ad(bpage->zip_size());

			switch (fil_page_get_type(page)) {
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_BITMAP:
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
				/* These are essentially uncompressed pages. */
				if (!zip) {
					/* InnoDB writes the data to the
					uncompressed page frame.  Copy it
					to the compressed page, which will
					be preserved. */
					memcpy(bpage->zip.data, page,
					       bpage->zip_size());
				}
				break;
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_INDEX:
			case FIL_PAGE_RTREE:
#if defined UNIV_ZIP_DEBUG && defined BTR_CUR_HASH_ADAPT
				/* During recovery, we only update the
				compressed page, not the uncompressed one. */
				ut_a(recv_recovery_is_on()
				     || page_zip_validate(
					     &bpage->zip, page,
					     ((buf_block_t*) bpage)->index));
#endif /* UNIV_ZIP_DEBUG && BTR_CUR_HASH_ADAPT */
				break;
			default:
				ib::error() << "The compressed page to be"
					" evicted seems corrupt:";
				ut_print_buf(stderr, page, srv_page_size);

				ib::error() << "Possibly older version of"
					" the page:";

				ut_print_buf(stderr, bpage->zip.data,
					     bpage->zip_size());
				putc('\n', stderr);
				ut_error;
			}

			break;
		}
		/* fall through */
	case BUF_BLOCK_ZIP_PAGE:
		ut_a(!bpage->oldest_modification());
		MEM_CHECK_ADDRESSABLE(bpage->zip.data, bpage->zip_size());
		break;
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	}

	ut_ad(!bpage->in_zip_hash);
	HASH_DELETE(buf_page_t, hash, &buf_pool.page_hash, id.fold(), bpage);

	switch (bpage->state()) {
	case BUF_BLOCK_ZIP_PAGE:
		ut_ad(!bpage->in_free_list);
		ut_ad(!bpage->in_LRU_list);
		ut_a(bpage->zip.data);
		ut_a(bpage->zip.ssize);
		ut_ad(!bpage->oldest_modification());

#ifdef UNIV_DEBUG
		UT_LIST_REMOVE(buf_pool.zip_clean, bpage);
#endif /* UNIV_DEBUG */
		hash_lock->write_unlock();
		buf_pool_mutex_exit_forbid();

		buf_buddy_free(bpage->zip.data, bpage->zip_size());

		buf_pool_mutex_exit_allow();
		buf_page_free_descriptor(bpage);
		return(false);

	case BUF_BLOCK_FILE_PAGE:
		static_assert(FIL_NULL == 0xffffffffU, "fill pattern");
		static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");
		memset_aligned<4>(reinterpret_cast<buf_block_t*>(bpage)->frame
				  + FIL_PAGE_OFFSET, 0xff, 4);
		static_assert(FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID % 4 == 2,
			      "not perfect alignment");
		memset_aligned<2>(reinterpret_cast<buf_block_t*>(bpage)->frame
				  + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		MEM_UNDEFINED(((buf_block_t*) bpage)->frame, srv_page_size);
		bpage->set_state(BUF_BLOCK_REMOVE_HASH);

		/* Question: If we release hash_lock here
		then what protects us against:
		1) Some other thread buffer fixing this page
		2) Some other thread trying to read this page and
		not finding it in buffer pool attempting to read it
		from the disk.
		Answer:
		1) Cannot happen because the page is no longer in the
		page_hash. Only possibility is when while invalidating
		a tablespace we buffer fix the prev_page in LRU to
		avoid relocation during the scan. But that is not
		possible because we are holding buf_pool mutex.

		2) Not possible because in buf_page_init_for_read()
		we do a look up of page_hash while holding buf_pool
		mutex and since we are holding buf_pool mutex here
		and by the time we'll release it in the caller we'd
		have inserted the compressed only descriptor in the
		page_hash. */
		hash_lock->write_unlock();

		if (zip && bpage->zip.data) {
			/* Free the compressed page. */
			void*	data = bpage->zip.data;
			bpage->zip.data = NULL;

			ut_ad(!bpage->in_free_list);
			ut_ad(!bpage->oldest_modification());
			ut_ad(!bpage->in_LRU_list);
			buf_pool_mutex_exit_forbid();

			buf_buddy_free(data, bpage->zip_size());

			buf_pool_mutex_exit_allow();

			page_zip_set_size(&bpage->zip, 0);
		}

		return(true);

	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		break;
	}

	ut_error;
	return(false);
}

/** Remove one page from LRU list and put it to free list.
@param bpage     file page to be freed
@param id        page identifier
@param hash_lock buf_pool.page_hash latch (will be released here) */
void buf_LRU_free_one_page(buf_page_t *bpage, const page_id_t id,
                           page_hash_latch *hash_lock)
{
  while (bpage->buf_fix_count())
    /* Wait for other threads to release the fix count
    before releasing the bpage from LRU list. */
    (void) LF_BACKOFF();

  if (buf_LRU_block_remove_hashed(bpage, id, hash_lock, true))
    buf_LRU_block_free_hashed_page(reinterpret_cast<buf_block_t*>(bpage));
}

/** Update buf_pool.LRU_old_ratio.
@param[in]	old_pct		Reserve this percentage of
				the buffer pool for "old" blocks
@param[in]	adjust		true=adjust the LRU list;
				false=just assign buf_pool.LRU_old_ratio
				during the initialization of InnoDB
@return updated old_pct */
uint buf_LRU_old_ratio_update(uint old_pct, bool adjust)
{
	uint	ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100;
	if (ratio < BUF_LRU_OLD_RATIO_MIN) {
		ratio = BUF_LRU_OLD_RATIO_MIN;
	} else if (ratio > BUF_LRU_OLD_RATIO_MAX) {
		ratio = BUF_LRU_OLD_RATIO_MAX;
	}

	if (adjust) {
		mutex_enter(&buf_pool.mutex);

		if (ratio != buf_pool.LRU_old_ratio) {
			buf_pool.LRU_old_ratio = ratio;

			if (UT_LIST_GET_LEN(buf_pool.LRU)
			    >= BUF_LRU_OLD_MIN_LEN) {
				buf_LRU_old_adjust_len();
			}
		}

		mutex_exit(&buf_pool.mutex);
	} else {
		buf_pool.LRU_old_ratio = ratio;
	}
	/* the reverse of
	ratio = old_pct * BUF_LRU_OLD_RATIO_DIV / 100 */
	return((uint) (ratio * 100 / (double) BUF_LRU_OLD_RATIO_DIV + 0.5));
}

/********************************************************************//**
Update the historical stats that we are collecting for LRU eviction
policy at the end of each interval. */
void
buf_LRU_stat_update()
{
	buf_LRU_stat_t*	item;
	buf_LRU_stat_t	cur_stat;

	if (!buf_pool.freed_page_clock) {
		goto func_exit;
	}

	/* Update the index. */
	item = &buf_LRU_stat_arr[buf_LRU_stat_arr_ind];
	buf_LRU_stat_arr_ind++;
	buf_LRU_stat_arr_ind %= BUF_LRU_STAT_N_INTERVAL;

	/* Add the current value and subtract the obsolete entry.
	Since buf_LRU_stat_cur is not protected by any mutex,
	it can be changing between adding to buf_LRU_stat_sum
	and copying to item. Assign it to local variables to make
	sure the same value assign to the buf_LRU_stat_sum
	and item */
	cur_stat = buf_LRU_stat_cur;

	buf_LRU_stat_sum.io += cur_stat.io - item->io;
	buf_LRU_stat_sum.unzip += cur_stat.unzip - item->unzip;

	/* Put current entry in the array. */
	memcpy(item, &cur_stat, sizeof *item);

func_exit:
	/* Clear the current entry. */
	memset(&buf_LRU_stat_cur, 0, sizeof buf_LRU_stat_cur);
}

#ifdef UNIV_DEBUG
/** Validate the LRU list. */
void buf_LRU_validate()
{
	ulint	old_len;
	ulint	new_len;

	mutex_enter(&buf_pool.mutex);

	if (UT_LIST_GET_LEN(buf_pool.LRU) >= BUF_LRU_OLD_MIN_LEN) {

		ut_a(buf_pool.LRU_old);
		old_len = buf_pool.LRU_old_len;

		new_len = ut_min(UT_LIST_GET_LEN(buf_pool.LRU)
				 * buf_pool.LRU_old_ratio
				 / BUF_LRU_OLD_RATIO_DIV,
				 UT_LIST_GET_LEN(buf_pool.LRU)
				 - (BUF_LRU_OLD_TOLERANCE
				    + BUF_LRU_NON_OLD_MIN_LEN));

		ut_a(old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
		ut_a(old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
	}

	CheckInLRUList::validate();

	old_len = 0;

	for (buf_page_t* bpage = UT_LIST_GET_FIRST(buf_pool.LRU);
	     bpage != NULL;
             bpage = UT_LIST_GET_NEXT(LRU, bpage)) {

		switch (bpage->state()) {
		case BUF_BLOCK_NOT_USED:
		case BUF_BLOCK_MEMORY:
		case BUF_BLOCK_REMOVE_HASH:
			ut_error;
			break;
		case BUF_BLOCK_FILE_PAGE:
			ut_ad(reinterpret_cast<buf_block_t*>(bpage)
			      ->in_unzip_LRU_list
			      == bpage->belongs_to_unzip_LRU());
		case BUF_BLOCK_ZIP_PAGE:
			break;
		}

		if (bpage->is_old()) {
			const buf_page_t*	prev
				= UT_LIST_GET_PREV(LRU, bpage);
			const buf_page_t*	next
				= UT_LIST_GET_NEXT(LRU, bpage);

			if (!old_len++) {
				ut_a(buf_pool.LRU_old == bpage);
			} else {
				ut_a(!prev || prev->is_old());
			}

			ut_a(!next || next->is_old());
		}
	}

	ut_a(buf_pool.LRU_old_len == old_len);

	CheckInFreeList::validate();

	for (buf_page_t* bpage = UT_LIST_GET_FIRST(buf_pool.free);
	     bpage != NULL;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_a(bpage->state() == BUF_BLOCK_NOT_USED);
	}

	CheckUnzipLRUAndLRUList::validate();

	for (buf_block_t* block = UT_LIST_GET_FIRST(buf_pool.unzip_LRU);
	     block != NULL;
	     block = UT_LIST_GET_NEXT(unzip_LRU, block)) {

		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);
		ut_a(block->page.belongs_to_unzip_LRU());
	}

	mutex_exit(&buf_pool.mutex);
}
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG
/** Dump the LRU list to stderr. */
void buf_LRU_print()
{
	mutex_enter(&buf_pool.mutex);

	for (buf_page_t* bpage = UT_LIST_GET_FIRST(buf_pool.LRU);
	     bpage != NULL;
	     bpage = UT_LIST_GET_NEXT(LRU, bpage)) {
		const page_id_t id(bpage->id());

		fprintf(stderr, "BLOCK space %u page %u ",
			id.space(), id.page_no());

		if (bpage->is_old()) {
			fputs("old ", stderr);
		}

		if (const uint32_t buf_fix_count = bpage->buf_fix_count()) {
			fprintf(stderr, "buffix count %u ", buf_fix_count);
		}

		if (const auto io_fix = bpage->io_fix()) {
			fprintf(stderr, "io_fix %d ", io_fix);
		}

		if (bpage->oldest_modification()) {
			fputs("modif. ", stderr);
		}

		switch (const auto state = bpage->state()) {
			const byte*	frame;
		case BUF_BLOCK_FILE_PAGE:
			frame = buf_block_get_frame((buf_block_t*) bpage);
			fprintf(stderr, "\ntype %u index id " IB_ID_FMT "\n",
				fil_page_get_type(frame),
				btr_page_get_index_id(frame));
			break;
		case BUF_BLOCK_ZIP_PAGE:
			frame = bpage->zip.data;
			fprintf(stderr, "\ntype %u size " ULINTPF
				" index id " IB_ID_FMT "\n",
				fil_page_get_type(frame),
				bpage->zip_size(),
				btr_page_get_index_id(frame));
			break;

		default:
			fprintf(stderr, "\n!state %d!\n", state);
			break;
		}
	}

	mutex_exit(&buf_pool.mutex);
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG */
