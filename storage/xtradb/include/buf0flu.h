/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/buf0flu.h
The database buffer pool flush algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#ifndef buf0flu_h
#define buf0flu_h

#include "univ.i"
#include "ut0byte.h"
#include "log0log.h"
#ifndef UNIV_HOTBACKUP
#include "mtr0types.h"
#include "buf0types.h"

/** Flag indicating if the page_cleaner is in active state. */
extern bool buf_page_cleaner_is_active;

/** Flag indicating if the lru_manager is in active state. */
extern bool buf_lru_manager_is_active;

/** Handled page counters for a single flush */
struct flush_counters_t {
	ulint	flushed;	/*!< number of dirty pages flushed */
	ulint	evicted;	/*!< number of clean pages evicted */
	ulint	unzip_LRU_evicted;/*!< number of uncompressed page images
				evicted */
};

/********************************************************************//**
Remove a block from the flush list of modified blocks.  */
UNIV_INTERN
void
buf_flush_remove(
/*=============*/
	buf_page_t*	bpage);	/*!< in: pointer to the block in question */
/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage has already been
copied to dpage. */
UNIV_INTERN
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage);	/*!< in/out: destination block */
/********************************************************************//**
Updates the flush system data structures when a write is completed. */
UNIV_INTERN
void
buf_flush_write_complete(
/*=====================*/
	buf_page_t*	bpage);	/*!< in: pointer to the block in question */
#endif /* !UNIV_HOTBACKUP */
/********************************************************************//**
Initializes a page for writing to the tablespace. */
UNIV_INTERN
void
buf_flush_init_for_writing(
/*=======================*/
	byte*	page,		/*!< in/out: page */
	void*	page_zip_,	/*!< in/out: compressed page, or NULL */
	lsn_t	newest_lsn);	/*!< in: newest modification lsn
				to the page */
#ifndef UNIV_HOTBACKUP
# if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: block and LRU list mutexes must be held upon entering this function, and
they will be released by this function after flushing. This is loosely based on
buf_flush_batch() and buf_flush_page().
@return TRUE if the page was flushed and the mutexes released */
UNIV_INTERN
ibool
buf_flush_page_try(
/*===============*/
	buf_pool_t*	buf_pool,	/*!< in/out: buffer pool instance */
	buf_block_t*	block)		/*!< in/out: buffer control block */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
# endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
/*******************************************************************//**
This utility flushes dirty blocks from the end of the flush list of
all buffer pool instances.
NOTE: The calling thread is not allowed to own any latches on pages!
@return true if a batch was queued successfully for each buffer pool
instance. false if another batch of same type was already running in
at least one of the buffer pool instance */
UNIV_INTERN
bool
buf_flush_list(
/*===========*/
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	lsn_t		lsn_limit,	/*!< in the case BUF_FLUSH_LIST all
					blocks whose oldest_modification is
					smaller than this should be flushed
					(if their number does not exceed
					min_n), otherwise ignored */
	ulint*		n_processed);	/*!< out: the number of pages
					which were processed is passed
					back to caller. Ignored if NULL */
/******************************************************************//**
This function picks up a single dirty page from the tail of the LRU
list, flushes it, removes it from page_hash and LRU list and puts
it on the free list. It is called from user threads when they are
unable to find a replacable page at the tail of the LRU list i.e.:
when the background LRU flushing in the page_cleaner thread is not
fast enough to keep pace with the workload.
@return TRUE if success. */
UNIV_INTERN
ibool
buf_flush_single_page_from_LRU(
/*===========================*/
	buf_pool_t*	buf_pool);	/*!< in/out: buffer pool instance */
/******************************************************************//**
Waits until a flush batch of the given type ends */
UNIV_INTERN
void
buf_flush_wait_batch_end(
/*=====================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_flush_t	type);		/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
/********************************************************************//**
This function should be called at a mini-transaction commit, if a page was
modified in it. Puts the block to the list of modified blocks, if it not
already in it. */
UNIV_INLINE
void
buf_flush_note_modification(
/*========================*/
	buf_block_t*	block,	/*!< in: block which is modified */
	mtr_t*		mtr);	/*!< in: mtr */
/********************************************************************//**
This function should be called when recovery has modified a buffer page. */
UNIV_INLINE
void
buf_flush_recv_note_modification(
/*=============================*/
	buf_block_t*	block,		/*!< in: block which is modified */
	lsn_t		start_lsn,	/*!< in: start lsn of the first mtr in a
					set of mtr's */
	lsn_t		end_lsn);	/*!< in: end lsn of the last mtr in the
					set of mtr's */
/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., transition FILE_PAGE => NOT_USED allowed.
@return	TRUE if can replace immediately */
UNIV_INTERN
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage);	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */
/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pool flush lists. As of now we'll have only one instance of this thread.
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(buf_flush_page_cleaner_thread)(
/*==========================================*/
	void*	arg);		/*!< in: a dummy parameter required by
				os_thread_create */
/******************************************************************//**
lru_manager thread tasked with performing LRU flushes and evictions to refill
the buffer pool free lists.  As of now we'll have only one instance of this
thread.
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(buf_flush_lru_manager_thread)(
/*=========================================*/
	void*	arg);		/*!< in: a dummy parameter required by
				os_thread_create */
/*********************************************************************//**
Clears up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return number of pages flushed */
UNIV_INTERN
ulint
buf_flush_LRU_tail(void);
/*====================*/
/*********************************************************************//**
Wait for any possible LRU flushes that are in progress to end. */
UNIV_INTERN
void
buf_flush_wait_LRU_batch_end(void);
/*==============================*/

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
UNIV_INTERN
ibool
buf_flush_validate(
/*===============*/
	buf_pool_t*	buf_pool);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
UNIV_INTERN
void
buf_flush_init_flush_rbt(void);
/*==========================*/

/********************************************************************//**
Frees up the red-black tree. */
UNIV_INTERN
void
buf_flush_free_flush_rbt(void);
/*==========================*/

/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: in simulated aio we must call
os_aio_simulated_wake_handler_threads after we have posted a batch of
writes! NOTE: buf_page_get_mutex(bpage) must be held upon entering this
function, and they will be released by this function if it returns true.
LRU_list_mutex must be held iff performing a single page flush and will be
released by the function if it returns true.
@return TRUE if the page was flushed */
UNIV_INTERN
bool
buf_flush_page(
/*===========*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_page_t*	bpage,		/*!< in: buffer control block */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync);		/*!< in: true if sync IO request */
/********************************************************************//**
Returns true if the block is modified and ready for flushing.
@return	true if can flush immediately */
UNIV_INTERN
bool
buf_flush_ready_for_flush(
/*======================*/
	buf_page_t*	bpage,	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) */
	buf_flush_t	flush_type)/*!< in: type of flush */
	MY_ATTRIBUTE((warn_unused_result));

#ifdef UNIV_DEBUG
/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush
list in a particular buffer pool.
@return	number of dirty pages present in a single buffer pool */
UNIV_INTERN
ulint
buf_pool_get_dirty_pages_count(
/*===========================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool */
	ulint		id);		/*!< in: space id to check */
/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush list.
@return	count of dirty pages present in all the buffer pools */
UNIV_INTERN
ulint
buf_flush_get_dirty_pages_count(
/*============================*/
	ulint		id);		/*!< in: space id to check */
#endif /* UNIV_DEBUG */

#endif /* !UNIV_HOTBACKUP */

/******************************************************************//**
Check if a flush list flush is in progress for any buffer pool instance, or if
all the instances are clean, for heuristic purposes.
@return true if flush list flush is in progress or buffer pool is clean */
UNIV_INLINE
bool
buf_flush_flush_list_in_progress(void)
/*==================================*/
	MY_ATTRIBUTE((warn_unused_result));

/** If LRU list of a buf_pool is less than this size then LRU eviction
should not happen. This is because when we do LRU flushing we also put
the blocks on free list. If LRU list is very small then we can end up
in thrashing. */
#define BUF_LRU_MIN_LEN		256

/******************************************************************//**
Start a buffer flush batch for LRU or flush list */
ibool
buf_flush_start(
/*============*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	flush_type);	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */

/******************************************************************//**
End a buffer flush batch for LRU or flush list */
void
buf_flush_end(
/*==========*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	flush_type);	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list or flush_list.
NOTE 1: in the case of an LRU flush the calling thread may own latches to
pages: to avoid deadlocks, this function must be written so that it cannot
end up waiting for these latches! NOTE 2: in the case of a flush list flush,
the calling thread is not allowed to own any latches on pages!
@return number of blocks for which the write request was queued */
__attribute__((nonnull))
void
buf_flush_batch(
/*============*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_flush_t	flush_type,	/*!< in: BUF_FLUSH_LRU or
					BUF_FLUSH_LIST; if BUF_FLUSH_LIST,
					then the caller must not own any
					latches on pages */
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	lsn_t		lsn_limit,	/*!< in: in the case of BUF_FLUSH_LIST
					all blocks whose oldest_modification is
					smaller than this should be flushed
					(if their number does not exceed
					min_n), otherwise ignored */
	bool		limited_lru_scan,/*!< in: for LRU flushes, if true,
					allow to scan only up to
					srv_LRU_scan_depth pages in total */
	flush_counters_t*	n);	/*!< out: flushed/evicted page
					counts  */


/******************************************************************//**
Gather the aggregated stats for both flush list and LRU list flushing */
void
buf_flush_common(
/*=============*/
	buf_flush_t	flush_type,	/*!< in: type of flush */
	ulint		page_count);	/*!< in: number of pages flushed */

#ifndef UNIV_NONINL
#include "buf0flu.ic"
#endif

#endif
