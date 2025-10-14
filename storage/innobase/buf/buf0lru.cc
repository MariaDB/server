/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
#include "fil0fil.h"
#include "btr0btr.h"
#include "buf0buddy.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "btr0sea.h"
#include "os0file.h"
#include "page0zip.h"
#include "log0recv.h"
#include "srv0srv.h"
#include "srv0mon.h"
#include "my_cpu.h"
#include "log.h"

/** The number of blocks from the LRU_old pointer onward, including
the block pointed to, must be buf_pool.LRU_old_ratio/BUF_LRU_OLD_RATIO_DIV
of the whole LRU list length, except that the tolerance defined below
is allowed. Note that the tolerance must be small enough such that for
even the BUF_LRU_OLD_MIN_LEN long LRU list, the LRU_old pointer is not
allowed to point to either end of the LRU list. */

static constexpr ulint BUF_LRU_OLD_TOLERANCE = 20;

/** The minimum amount of non-old blocks when the LRU_old list exists
(that is, when there are more than BUF_LRU_OLD_MIN_LEN blocks).
@see buf_LRU_old_adjust_len */
#define BUF_LRU_NON_OLD_MIN_LEN	5

/** If we switch on the InnoDB monitor because there are too few available
frames in the buffer pool, we set this to TRUE */
static bool buf_lru_switched_on_innodb_mon = false;

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
Updated at SRV_MONITOR_INTERVAL (the buf_LRU_stat_update() call rate). */
static constexpr ulint BUF_LRU_STAT_N_INTERVAL= 4;

/** Co-efficient with which we multiply I/O operations to equate them
with page_zip_decompress() operations. */
static constexpr ulint BUF_LRU_IO_TO_UNZIP_FACTOR= 50;

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

/** Remove bpage from buf_pool.LRU and buf_pool.page_hash.

If !bpage->frame && bpage->oldest_modification() <= 1,
the object will be freed.

@param bpage      buffer block
@param id         page identifier
@param chain      locked buf_pool.page_hash chain (will be released here)
@param zip        whether bpage->zip of BUF_BLOCK_FILE_PAGE should be freed

If a compressed page is freed other compressed pages may be relocated.
@retval true if bpage with bpage->frame was removed from page_hash. The
caller needs to free the page to the free list
@retval false if block without bpage->frame was removed from page_hash. In
this case the block is already returned to the buddy allocator. */
static bool buf_LRU_block_remove_hashed(buf_page_t *bpage, const page_id_t id,
                                        buf_pool_t::hash_chain &chain,
                                        bool zip);

/** Try to free a replaceable block.
@param limit  maximum number of blocks to scan
@return true if found and freed */
static bool buf_LRU_scan_and_free_block(ulint limit= ULINT_UNDEFINED);

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
	mysql_mutex_assert_owner(&buf_pool.mutex);

	buf_pool.stat.LRU_bytes += bpage->physical_size();

	ut_ad(buf_pool.stat.LRU_bytes <= buf_pool.curr_pool_size());
}

/** @return whether the unzip_LRU list should be used for evicting a victim
instead of the general LRU list */
bool buf_LRU_evict_from_unzip_LRU()
{
	mysql_mutex_assert_owner(&buf_pool.mutex);

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

/** Try to free an uncompressed page of a compressed block from the unzip
LRU list.  The compressed page is preserved, and it need not be clean.
@param limit  maximum number of blocks to scan
@param tm     buf_page_t::is_accessed() threshold for recent access, or 0
@return true if freed */
static bool buf_LRU_free_from_unzip_LRU_list(ulint limit, uint16_t tm)
{
	if (!buf_LRU_evict_from_unzip_LRU()) {
		return(false);
	}

	ulint	scanned = 0;
	bool	freed = false;

	for (buf_block_t* block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
	     block && scanned < limit; ++scanned) {
		buf_block_t* prev_block = UT_LIST_GET_PREV(unzip_LRU, block);

		ut_ad(block->page.in_file());
		ut_ad(block->page.belongs_to_unzip_LRU());
		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);

		if (block->page.zip.was_accessed()) {
			block->page.make_young(tm);
		} else {
			freed = buf_LRU_free_page(&block->page, false);
			if (freed) {
				scanned++;
				break;
			}
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
@param limit  maximum number of blocks to scan
@param tm     buf_page_t::is_accessed() threshold for recent access, or 0
@return whether a page was freed */
static bool buf_LRU_free_from_common_LRU_list(ulint limit, uint16_t tm)
{
	mysql_mutex_assert_owner(&buf_pool.mutex);

	ulint		scanned = 0;
	bool		freed = false;

	for (buf_page_t* bpage = buf_pool.lru_scan_itr.start();
	     bpage && scanned < limit;
	     ++scanned, bpage = buf_pool.lru_scan_itr.get()) {
		buf_page_t*	prev = UT_LIST_GET_PREV(LRU, bpage);
		buf_pool.lru_scan_itr.set(prev);

		if (bpage->zip.was_accessed()) {
			bpage->make_young(tm);
			continue;
		}

		const auto accessed = bpage->is_accessed();

		if (buf_LRU_free_page(bpage, true)) {
			if (!accessed) {
				/* Keep track of pages that are evicted without
				ever being accessed. This gives us a measure of
				the effectiveness of readahead */
				++buf_pool.stat.n_ra_pages_evicted;
			}

			freed = true;
			scanned++;
			break;
		}
	}

	MONITOR_INC_VALUE_CUMULATIVE(
		MONITOR_LRU_SEARCH_SCANNED,
		MONITOR_LRU_SEARCH_SCANNED_NUM_CALL,
		MONITOR_LRU_SEARCH_SCANNED_PER_CALL,
		scanned);

	return(freed);
}

/******************************************************************//**
Checks how much of buf_pool is occupied by non-data objects like
AHI, lock heaps etc. Depending on the size of non-data objects this
function will either assert or issue a warning and switch on the
status monitor. */
static void buf_LRU_check_size_of_non_data_objects() noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);

  if (recv_recovery_is_on())
    return;

  const size_t curr_size{buf_pool.usable_size()};

  auto s= UT_LIST_GET_LEN(buf_pool.free) + UT_LIST_GET_LEN(buf_pool.LRU);

  if (s >= curr_size / 20);
  else if (buf_pool.is_shrinking())
    buf_pool.LRU_warn();
  else
  {
    sql_print_error("[FATAL] InnoDB: Over 95 percent of the buffer pool is"
                    " occupied by lock heaps"
#ifdef BTR_CUR_HASH_ADAPT
                    " or the adaptive hash index"
#endif /* BTR_CUR_HASH_ADAPT */
                    "! Check that your transactions do not set too many"
                    " row locks, or review if innodb_buffer_pool_size=%zuM"
                    " could be bigger",
                    curr_size >> (20 - srv_page_size_shift));
    abort();
  }

  if (s < curr_size / 3)
  {
    if (!buf_lru_switched_on_innodb_mon && srv_monitor_timer)
    {
      /* Over 67 % of the buffer pool is occupied by lock heaps or
      the adaptive hash index. This may be a memory leak! */
      sql_print_warning("InnoDB: Over 67 percent of the buffer pool is"
                        " occupied by lock heaps"
#ifdef BTR_CUR_HASH_ADAPT
                        " or the adaptive hash index"
#endif /* BTR_CUR_HASH_ADAPT */
                        "! Check that your transactions do not set too many"
                        " row locks. innodb_buffer_pool_size=%zuM."
                        " Starting the InnoDB Monitor to print diagnostics.",
                        curr_size >> (20 - srv_page_size_shift));

      buf_lru_switched_on_innodb_mon= true;
      srv_print_innodb_monitor= TRUE;
      srv_monitor_timer_schedule_now();
    }
  }
  else if (buf_lru_switched_on_innodb_mon)
  {
    /* Switch off the InnoDB Monitor; this is a simple way to stop the
    monitor if the situation becomes less urgent, but may also
    surprise users who did SET GLOBAL innodb_status_output=ON earlier! */
    buf_lru_switched_on_innodb_mon= false;
    srv_print_innodb_monitor= FALSE;
  }
}

/** Get a block from the buf_pool.free list.
If the list is empty, blocks will be moved from the end of buf_pool.LRU
to buf_pool.free.

This function is called from a user thread when it needs a clean
block to read in a page. Note that we only ever get a block from
the free list. Even when we flush a page or find a page in LRU scan
we put it to free list to be used.
* iteration 0:
  * get a block from the buf_pool.free list
  * if buf_pool.try_LRU_scan is set
    * scan LRU up to 100 pages to free a clean block
    * success:retry the free list
  * invoke buf_pool.page_cleaner_wakeup(true) and wait its completion
* subsequent iterations: same as iteration 0 except:
  * scan the entire LRU list

@param get  how to allocate the block
@return the free control block, in state BUF_BLOCK_MEMORY
@retval nullptr if get==have_no_mutex_soft and memory was not available */
buf_block_t* buf_LRU_get_free_block(buf_LRU_get get)
{
  bool waited= false;
  MONITOR_INC(MONITOR_LRU_GET_FREE_SEARCH);
  if (UNIV_LIKELY(get != have_mutex))
    mysql_mutex_lock(&buf_pool.mutex);

  buf_LRU_check_size_of_non_data_objects();

  buf_block_t *block;

retry:
  /* If there is a block in the free list, take it */
  block= buf_pool.allocate();
  if (block)
  {
got_block:
    const ulint LRU_size= UT_LIST_GET_LEN(buf_pool.LRU);
    const ulint available= UT_LIST_GET_LEN(buf_pool.free);
    const size_t scan_depth{buf_pool.LRU_scan_depth / 2};
    ut_ad(LRU_size <= BUF_LRU_MIN_LEN || available >= scan_depth ||
          buf_pool.is_shrinking() || buf_pool.need_LRU_eviction());

    ut_d(bool signalled = false);

    if (UNIV_UNLIKELY(available < scan_depth) && LRU_size > BUF_LRU_MIN_LEN)
    {
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      if (!buf_pool.page_cleaner_active())
      {
        buf_pool.page_cleaner_wakeup(true);
        ut_d(signalled = true);
      }
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    }

    if (UNIV_LIKELY(get != have_mutex))
      mysql_mutex_unlock(&buf_pool.mutex);

    DBUG_EXECUTE_IF("ib_free_page_sleep",
    {
      static bool do_sleep = true;
      if (do_sleep && signalled)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        do_sleep = false;
      }
    });

    block->page.zip.clear();
    return block;
  }

  MONITOR_INC(MONITOR_LRU_GET_FREE_LOOPS);
  if (waited || buf_pool.try_LRU_scan)
  {
    /* If no block was in the free list, search from the end of the
    LRU list and try to free a block there.  If we are doing for the
    first time we'll scan only tail of the LRU list otherwise we scan
    the whole LRU list. */
    if (buf_LRU_scan_and_free_block(waited ? ULINT_UNDEFINED : 100))
      goto retry;

    /* Tell other threads that there is no point in scanning the LRU
    list. */
    buf_pool.try_LRU_scan= false;
  }

  if (get == have_no_mutex_soft)
  {
    mysql_mutex_unlock(&buf_pool.mutex);
    return nullptr;
  }

  waited= true;

  while (!(block= buf_pool.allocate()))
  {
    buf_pool.stat.LRU_waits++;

    timespec abstime;
    set_timespec(abstime, 1);

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    if (!buf_pool.page_cleaner_active())
      buf_pool.page_cleaner_wakeup(true);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    if (my_cond_timedwait(&buf_pool.done_free, &buf_pool.mutex.m_mutex,
                          &abstime))
    {
      buf_pool.LRU_warn();
      buf_LRU_check_size_of_non_data_objects();
    }
  }

  goto got_block;
}

/** Move the LRU_old pointer so that the length of the old blocks list
is inside the allowed limits. */
static void buf_LRU_old_adjust_len()
{
	ulint	old_len;
	ulint	new_len;

	ut_a(buf_pool.LRU_old);
	mysql_mutex_assert_owner(&buf_pool.mutex);
	ut_ad(buf_pool.LRU_old_ratio >= BUF_LRU_OLD_RATIO_MIN);
	ut_ad(buf_pool.LRU_old_ratio <= BUF_LRU_OLD_RATIO_MAX);
	compile_time_assert(BUF_LRU_OLD_RATIO_MIN * BUF_LRU_OLD_MIN_LEN
			    > BUF_LRU_OLD_RATIO_DIV
			    * (BUF_LRU_OLD_TOLERANCE + 5));
	compile_time_assert(BUF_LRU_NON_OLD_MIN_LEN < BUF_LRU_OLD_MIN_LEN);

#ifdef UNIV_LRU_DEBUG
	/* buf_pool.LRU_old must be the first item in the LRU list
	whose "old" flag is set. */
	ut_a(buf_pool.LRU_old->zip.old());
	ut_a(!UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)
	     || !UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)->zip.old());
	ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)
	     || UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)->zip.old());
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
		ut_a(LRU_old->zip.old());
#endif /* UNIV_LRU_DEBUG */

		/* Update the LRU_old pointer if necessary */

		if (old_len + BUF_LRU_OLD_TOLERANCE < new_len) {

			buf_pool.LRU_old = LRU_old = UT_LIST_GET_PREV(
				LRU, LRU_old);
#ifdef UNIV_LRU_DEBUG
			ut_a(!LRU_old->zip.old());
#endif /* UNIV_LRU_DEBUG */
			old_len = ++buf_pool.LRU_old_len;
			LRU_old->set_old<true>();

		} else if (old_len > new_len + BUF_LRU_OLD_TOLERANCE) {

			buf_pool.LRU_old = UT_LIST_GET_NEXT(LRU, LRU_old);
			old_len = --buf_pool.LRU_old_len;
			LRU_old->set_old<false>();
		} else {
			return;
		}
	}
}

/** Initialize the old blocks pointer in the LRU list. This function should be
called when the LRU list grows to BUF_LRU_OLD_MIN_LEN length. */
static void buf_LRU_old_init()
{
	mysql_mutex_assert_owner(&buf_pool.mutex);
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
		bpage->zip.set_old<true>();
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
	mysql_mutex_assert_owner(&buf_pool.mutex);

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
		ut_a(!prev_bpage->zip.old());
#endif /* UNIV_LRU_DEBUG */
		buf_pool.LRU_old = prev_bpage;
		prev_bpage->set_old<true>();

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
			bpage->zip.set_old<false>();
		}

		buf_pool.LRU_old = NULL;
		buf_pool.LRU_old_len = 0;

		return;
	}

	ut_ad(buf_pool.LRU_old);

	/* Update the LRU_old_len field if necessary */
	if (bpage->zip.old()) {
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
	mysql_mutex_assert_owner(&buf_pool.mutex);
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
	mysql_mutex_assert_owner(&buf_pool.mutex);
	ut_ad(!bpage->in_LRU_list);

	if (!old || (UT_LIST_GET_LEN(buf_pool.LRU) < BUF_LRU_OLD_MIN_LEN)) {
		UT_LIST_ADD_FIRST(buf_pool.LRU, bpage);
	} else {
#ifdef UNIV_LRU_DEBUG
		/* buf_pool.LRU_old must be the first item in the LRU list
		whose "old" flag is set. */
		ut_a(buf_pool.LRU_old->zip.old());
		ut_a(!UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)
		     || !UT_LIST_GET_PREV(LRU, buf_pool.LRU_old)->zip.old());
		ut_a(!UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)
		     || UT_LIST_GET_NEXT(LRU, buf_pool.LRU_old)->zip.old());
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

		if (old) bpage->set_old<true>(); else bpage->set_old<false>();
		buf_LRU_old_adjust_len();

	} else if (UT_LIST_GET_LEN(buf_pool.LRU) == BUF_LRU_OLD_MIN_LEN) {

		/* The LRU list is now long enough for LRU_old to become
		defined: init it */

		buf_LRU_old_init();
	} else if (buf_pool.LRU_old) {
		bpage->set_old<true>();
	} else {
		bpage->set_old<false>();
	}

	/* If this is a zipped block with decompressed frame as well
	then put it on the unzip_LRU list */
	if (bpage->belongs_to_unzip_LRU()) {
		buf_unzip_LRU_add_block((buf_block_t*) bpage, old);
	}
}

void buf_page_make_young(buf_page_t *bpage)
{
  buf_pool.stat.n_pages_made_young++;
  buf_LRU_remove_block(bpage);
  buf_LRU_add_block(bpage, false);
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
	const page_id_t id{bpage->id()};
	buf_page_t*	b = nullptr;

	mysql_mutex_assert_owner(&buf_pool.mutex);

	/* First, perform a quick check before we acquire hash_lock. */
	if (!bpage->can_relocate()) {
		return false;
	}

	/* We must hold an exclusive hash_lock to prevent
	bpage->can_relocate() from changing due to a concurrent
	execution of buf_page_get_gen(). */
	buf_pool_t::hash_chain& chain= buf_pool.page_hash.cell_get(id.fold());
	page_hash_latch& hash_lock = buf_pool.page_hash.lock_get(chain);
	/* We cannot use transactional_lock_guard here,
	because buf_buddy_relocate() in buf_buddy_free() could get stuck. */
	hash_lock.lock();
	const lsn_t oldest_modification = bpage->oldest_modification_acquire();

	if (UNIV_UNLIKELY(!bpage->can_relocate())) {
		/* Do not free buffer fixed and I/O-fixed blocks. */
		goto func_exit;
	}

	switch (oldest_modification) {
	case 2:
		ut_ad(id.space() == SRV_TMP_SPACE_ID);
		ut_ad(!bpage->zip.data);
		if (!bpage->is_freed()) {
			goto func_exit;
		}
		bpage->clear_oldest_modification();
		break;
	case 1:
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		if (ut_d(const lsn_t om =) bpage->oldest_modification()) {
			ut_ad(om == 1);
			buf_pool.delete_from_flush_list(bpage);
		}
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
		ut_ad(!bpage->oldest_modification());
		/* fall through */
	case 0:
		if (zip || !bpage->zip.data || !bpage->frame) {
			break;
		}
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
relocate_compressed:
		b = static_cast<buf_page_t*>(ut_zalloc_nokey(sizeof *b));
		ut_a(b);
		new (b) buf_page_t(*bpage);
		b->frame = nullptr;
		{
			ut_d(uint32_t s=) b->fix();
			ut_ad(s == buf_page_t::FREED
			      || s == buf_page_t::UNFIXED
			      || s == buf_page_t::REINIT);
		}
		break;
	default:
		if (zip || !bpage->zip.data || !bpage->frame) {
			/* This would completely free the block. */
			/* Do not completely free dirty blocks. */
func_exit:
			hash_lock.unlock();
			return(false);
		}
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		if (bpage->can_relocate()) {
			goto relocate_compressed;
		}
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
		goto func_exit;
	}

	mysql_mutex_assert_owner(&buf_pool.mutex);

	DBUG_PRINT("ib_buf", ("free page %u:%u", id.space(), id.page_no()));

	ut_ad(bpage->can_relocate());

	if (!buf_LRU_block_remove_hashed(bpage, id, chain, zip)) {
		ut_ad(!b);
		mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
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

		ut_ad(!buf_pool.page_hash.get(id, chain));
		ut_ad(b->zip_size());

		/* The field in_LRU_list of
		the to-be-freed block descriptor should have
		been cleared in
		buf_LRU_block_remove_hashed(), which
		invokes buf_LRU_remove_block(). */
		ut_ad(!bpage->in_LRU_list);
		ut_ad(bpage->frame);
		ut_ad(!((buf_block_t*) bpage)->in_unzip_LRU_list);

		/* The fields of bpage were copied to b before
		buf_LRU_block_remove_hashed() was invoked. */
		ut_ad(b->in_LRU_list);
		ut_ad(b->in_page_hash);
		ut_d(b->in_page_hash = false);
		b->hash = nullptr;

		buf_pool.page_hash.append(chain, b);

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
			if (b->is_old()) {
				b->set_old<true>();
			} else {
				b->set_old<false>();
			}
#endif /* UNIV_LRU_DEBUG */
		} else {
			ut_d(b->in_LRU_list = FALSE);
			buf_LRU_add_block(b, b->zip.old());
		}

		buf_flush_relocate_on_flush_list(bpage, b);
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);

		bpage->zip.clear();
		b->lock.x_lock();
		hash_lock.unlock();
	} else if (!zip) {
		hash_lock.unlock();
	}

	buf_block_t* block = reinterpret_cast<buf_block_t*>(bpage);

#ifdef BTR_CUR_HASH_ADAPT
	if (block->index) {
		mysql_mutex_unlock(&buf_pool.mutex);

		/* Remove the adaptive hash index on the page.
		The page was declared uninitialized by
		buf_LRU_block_remove_hashed().  We need to flag
		the contents of the page valid (which it still is) in
		order to avoid bogus Valgrind or MSAN warnings.*/

		MEM_MAKE_DEFINED(block->page.frame, srv_page_size);
		btr_search_drop_page_hash_index(block, nullptr);
		MEM_UNDEFINED(block->page.frame, srv_page_size);
		mysql_mutex_lock(&buf_pool.mutex);
	}
#endif
	if (UNIV_LIKELY_NULL(b)) {
		ut_ad(b->zip_size());
		b->lock.x_unlock();
		b->unfix();
	}

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

	ut_ad(block->page.state() == buf_page_t::MEMORY);
#ifdef BTR_CUR_HASH_ADAPT
	assert_block_ahi_empty(block);
	block->n_hash_helps = 0;
#endif
	ut_ad(!block->page.in_free_list);
	ut_ad(!block->page.oldest_modification());
	ut_ad(!block->page.in_LRU_list);
	ut_ad(!block->page.hash);

	block->page.set_state(buf_page_t::NOT_USED);

	MEM_UNDEFINED(block->page.frame, srv_page_size);
	data = block->page.zip.data;

	if (UNIV_LIKELY_NULL(data)) {
		const auto zip_size = block->zip_size();
		ut_ad(zip_size);
		block->page.zip.clear();
		buf_buddy_free(data, zip_size);
	}

	if (buf_pool.to_withdraw() && buf_pool.withdraw(block->page)) {
	} else {
		UT_LIST_ADD_FIRST(buf_pool.free, &block->page);
		ut_d(block->page.in_free_list = true);
		buf_pool.try_LRU_scan= true;
		pthread_cond_broadcast(&buf_pool.done_free);
	}

	block->page.set_os_unused();
}

/** Release a memory block to the buffer pool. */
ATTRIBUTE_COLD void buf_pool_t::free_block(buf_block_t *block) noexcept
{
  ut_ad(this == &buf_pool);
  mysql_mutex_lock(&mutex);
  buf_LRU_block_free_non_file_page(block);
  mysql_mutex_unlock(&mutex);
}

inline void
buf_pool_t::page_hash_table::remove(buf_pool_t::hash_chain &chain,
                                    buf_page_t *bpage) noexcept
{
  mysql_mutex_assert_owner(&buf_pool.mutex);

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

/** Remove bpage from buf_pool.LRU and buf_pool.page_hash.

If !bpage->frame && !bpage->oldest_modification(), the object will be freed.

@param bpage      buffer block
@param id         page identifier
@param chain      locked buf_pool.page_hash chain (will be released here)
@param zip        whether bpage->zip of BUF_BLOCK_FILE_PAGE should be freed

If a compressed page is freed other compressed pages may be relocated.
@retval true if BUF_BLOCK_FILE_PAGE was removed from page_hash. The
caller needs to free the page to the free list
@retval false if BUF_BLOCK_ZIP_PAGE was removed from page_hash. In
this case the block is already returned to the buddy allocator. */
static bool buf_LRU_block_remove_hashed(buf_page_t *bpage, const page_id_t id,
                                        buf_pool_t::hash_chain &chain,
                                        bool zip)
{
	ut_a(bpage->can_relocate());
	ut_ad(buf_pool.page_hash.lock_get(chain).is_write_locked());

	buf_LRU_remove_block(bpage);

	if (UNIV_LIKELY(!bpage->zip.data)) {
		MEM_CHECK_ADDRESSABLE(bpage, sizeof(buf_block_t));
		MEM_CHECK_ADDRESSABLE(bpage->frame, srv_page_size);
		reinterpret_cast<buf_block_t*>(bpage)->invalidate();
	} else if (const page_t *page = bpage->frame) {
		MEM_CHECK_ADDRESSABLE(bpage, sizeof(buf_block_t));
		MEM_CHECK_ADDRESSABLE(bpage->frame, srv_page_size);
		reinterpret_cast<buf_block_t*>(bpage)->invalidate();

		ut_a(!zip || !bpage->oldest_modification());
		ut_ad(bpage->zip_size());
		/* Skip consistency checks if the page was freed.
		In recovery, we could get a sole FREE_PAGE record
		and nothing else, for a ROW_FORMAT=COMPRESSED page.
		Its contents would be garbage. */
		if (!bpage->is_freed())
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
		case FIL_PAGE_INDEX:
		case FIL_PAGE_RTREE:
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
	} else {
		ut_a(!bpage->oldest_modification());
		MEM_CHECK_ADDRESSABLE(bpage->zip.data, bpage->zip_size());
	}

	buf_pool.page_hash.remove(chain, bpage);
	page_hash_latch& hash_lock = buf_pool.page_hash.lock_get(chain);

	if (UNIV_UNLIKELY(!bpage->frame)) {
		ut_ad(!bpage->in_free_list);
		ut_ad(!bpage->in_LRU_list);
		ut_a(bpage->zip.data);
		ut_a(bpage->zip.ssize());
		ut_ad(!bpage->oldest_modification());

		hash_lock.unlock();
		buf_buddy_free(bpage->zip.data, bpage->zip_size());
		bpage->lock.free();
		ut_free(bpage);
		return false;
	} else {
		static_assert(FIL_NULL == 0xffffffffU, "fill pattern");
		static_assert(FIL_PAGE_OFFSET % 4 == 0, "alignment");
		memset_aligned<4>(bpage->frame + FIL_PAGE_OFFSET, 0xff, 4);
		static_assert(FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID % 4 == 2,
			      "not perfect alignment");
		memset_aligned<2>(bpage->frame
				  + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 0xff, 4);
		MEM_UNDEFINED(bpage->frame, srv_page_size);
		bpage->set_state(buf_page_t::REMOVE_HASH);

		if (!zip) {
			return true;
		}

		hash_lock.unlock();

		if (void* data = bpage->zip.data) {
			/* Free the compressed page. */
			const auto zip_size = bpage->zip_size();

			ut_ad(!bpage->in_free_list);
			ut_ad(!bpage->oldest_modification());
			ut_ad(!bpage->in_LRU_list);
			bpage->zip.clear();
			buf_buddy_free(data, zip_size);
		}

		return true;
	}
}

/** Release and evict a corrupted page.
@param bpage    x-latched page that was found corrupted
@param state    expected current state of the page */
ATTRIBUTE_COLD
void buf_pool_t::corrupted_evict(buf_page_t *bpage, uint32_t state) noexcept
{
  const page_id_t id{bpage->id()};
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);

  mysql_mutex_lock(&mutex);
  hash_lock.lock();

  ut_ad(!bpage->oldest_modification());
  bpage->set_corrupt_id();
  auto unfix= state - buf_page_t::FREED;
  auto s= bpage->zip.fix.fetch_sub(unfix) - unfix;
  bpage->lock.x_unlock(true);

  while (s != buf_page_t::FREED || bpage->lock.is_locked_or_waiting())
  {
    ut_ad(s >= buf_page_t::FREED);
    ut_ad(s < buf_page_t::UNFIXED);
    /* Wait for other threads to release the fix count
    before releasing the bpage from LRU list. */
    (void) LF_BACKOFF();
    s= bpage->state();
  }

  /* remove from LRU and page_hash */
  if (buf_LRU_block_remove_hashed(bpage, id, chain, true))
    buf_LRU_block_free_hashed_page(reinterpret_cast<buf_block_t*>(bpage));

  mysql_mutex_unlock(&mutex);
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
		mysql_mutex_lock(&buf_pool.mutex);

		if (ratio != buf_pool.LRU_old_ratio) {
			buf_pool.LRU_old_ratio = ratio;

			if (UT_LIST_GET_LEN(buf_pool.LRU)
			    >= BUF_LRU_OLD_MIN_LEN) {
				buf_LRU_old_adjust_len();
			}
		}

		mysql_mutex_unlock(&buf_pool.mutex);
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

	/* Clear the current entry. */
	memset(&buf_LRU_stat_cur, 0, sizeof buf_LRU_stat_cur);
}

/** Invalidate all pages in the buffer pool.
All pages must be in a replaceable state (not modified or latched). */
void buf_pool_invalidate() noexcept
{
  /* It is possible that a write batch that has been posted
  earlier is still not complete. For buffer pool invalidation to
  proceed we must ensure there is NO write activity happening. */

  os_aio_wait_until_no_pending_writes(false);
  ut_d(buf_pool.assert_all_freed());
  mysql_mutex_lock(&buf_pool.mutex);

  while (UT_LIST_GET_LEN(buf_pool.LRU))
    buf_LRU_scan_and_free_block();

  ut_ad(UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0);

  buf_pool.LRU_old= nullptr;
  buf_pool.LRU_old_len= 0;
  buf_pool.stat.init();

  buf_refresh_io_stats();
  mysql_mutex_unlock(&buf_pool.mutex);
}

#if defined __aarch64__&&defined __GNUC__&&__GNUC__==4&&!defined __clang__
/* Avoid GCC 4.8.5 internal compiler error "could not split insn".
We would only need this for buf_LRU_scan_and_free_block(),
but GCC 4.8.5 does not support pop_options. */
# pragma GCC optimize ("O0")
#endif
/** Try to free a replaceable block.
@param limit  maximum number of blocks to scan
@return true if found and freed */
static bool buf_LRU_scan_and_free_block(ulint limit)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);

  const uint16_t tm= buf_pool.LRU_old_time_threshold
    ? uint16_t(time(nullptr) - buf_pool.LRU_old_time_threshold / 1000)
    : 0;

  return buf_LRU_free_from_unzip_LRU_list(limit, tm) ||
    buf_LRU_free_from_common_LRU_list(limit, tm);
}

void buf_LRU_truncate_temp(uint32_t threshold)
{
  /* Set the extent descriptor page state as FREED */
  for (uint32_t cur_xdes_page= xdes_calc_descriptor_page(
         0, fil_system.temp_space->free_limit);
       cur_xdes_page >= threshold;)
  {
    mtr_t mtr{nullptr};
    mtr.start();
    if (buf_block_t* block= buf_page_get_gen(
          page_id_t(SRV_TMP_SPACE_ID, cur_xdes_page), 0, RW_X_LATCH,
          nullptr, BUF_PEEK_IF_IN_POOL, &mtr))
    {
      uint32_t state= block->page.state();
      ut_ad(state > buf_page_t::UNFIXED);
      ut_ad(state < buf_page_t::READ_FIX);
      block->page.set_freed(state);
    }
    cur_xdes_page-= uint32_t(srv_page_size);
    mtr.commit();
  }

  const page_id_t limit{SRV_TMP_SPACE_ID, threshold};
  mysql_mutex_lock(&buf_pool.mutex);
  for (buf_page_t* bpage = UT_LIST_GET_FIRST(buf_pool.LRU);
       bpage;)
  {
    buf_page_t* next= UT_LIST_GET_NEXT(LRU, bpage);
    if (bpage->id() >= limit)
    {
    #ifdef UNIV_DEBUG
      if (bpage->lock.u_lock_try(0))
      {
        ut_ad(bpage->state() == buf_page_t::FREED);
        bpage->lock.u_unlock();
      }
    #endif /* UNIV_DEBUG */
      ut_ad(!reinterpret_cast<buf_block_t*>(bpage)->index);
      buf_LRU_free_page(bpage, true);
    }
    bpage= next;
  }
  mysql_mutex_unlock(&buf_pool.mutex);
}

#ifdef UNIV_DEBUG
/** Validate the LRU list. */
void buf_LRU_validate()
{
	ulint	old_len;
	ulint	new_len;

	mysql_mutex_lock(&buf_pool.mutex);

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
		ut_ad(bpage->in_file());
		ut_ad(!bpage->frame
		      || reinterpret_cast<buf_block_t*>(bpage)
		      ->in_unzip_LRU_list
		      == !!bpage->zip.data);

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

		ut_a(bpage->state() == buf_page_t::NOT_USED);
	}

	CheckUnzipLRUAndLRUList::validate();

	for (buf_block_t* block = UT_LIST_GET_FIRST(buf_pool.unzip_LRU);
	     block != NULL;
	     block = UT_LIST_GET_NEXT(unzip_LRU, block)) {

		ut_ad(block->in_unzip_LRU_list);
		ut_ad(block->page.in_LRU_list);
		ut_a(block->page.belongs_to_unzip_LRU());
	}

	mysql_mutex_unlock(&buf_pool.mutex);
}
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG
/** Dump the LRU list to stderr. */
void buf_LRU_print()
{
	mysql_mutex_lock(&buf_pool.mutex);

	for (buf_page_t* bpage = UT_LIST_GET_FIRST(buf_pool.LRU);
	     bpage != NULL;
	     bpage = UT_LIST_GET_NEXT(LRU, bpage)) {
		const page_id_t id(bpage->id());

		fprintf(stderr, "BLOCK space %u page %u ",
			id.space(), id.page_no());

		if (bpage->is_old()) {
			fputs("old ", stderr);
		}

		const unsigned s = bpage->state();
		if (s > buf_page_t::UNFIXED) {
			fprintf(stderr, "fix %u ", s - buf_page_t::UNFIXED);
		} else {
			ut_ad(s == buf_page_t::UNFIXED
			      || s == buf_page_t::REMOVE_HASH);
		}

		if (bpage->oldest_modification()) {
			fputs("modif. ", stderr);
		}

		if (const byte* frame = bpage->zip.data) {
			fprintf(stderr, "\ntype %u size " ULINTPF
				" index id " IB_ID_FMT "\n",
				fil_page_get_type(frame),
				bpage->zip_size(),
				btr_page_get_index_id(frame));
		} else {
			fprintf(stderr, "\ntype %u index id " IB_ID_FMT "\n",
				fil_page_get_type(bpage->frame),
				btr_page_get_index_id(bpage->frame));
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);
}
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG */
