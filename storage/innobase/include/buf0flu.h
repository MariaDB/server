/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2020, MariaDB Corporation.

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

#include "ut0byte.h"
#include "log0log.h"
#include "buf0types.h"

/** Number of pages flushed through non flush_list flushes. */
extern ulint buf_lru_flush_page_count;

/** Flag indicating if the page_cleaner is in active state. */
extern bool buf_page_cleaner_is_active;

#ifdef UNIV_DEBUG

/** Value of MySQL global variable used to disable page cleaner. */
extern my_bool		innodb_page_cleaner_disabled_debug;

#endif /* UNIV_DEBUG */

/** Event to synchronise with the flushing. */
extern os_event_t	buf_flush_event;

class ut_stage_alter_t;

/** Handled page counters for a single flush */
struct flush_counters_t {
	ulint	flushed;	/*!< number of dirty pages flushed */
	ulint	evicted;	/*!< number of clean pages evicted */
	ulint	unzip_LRU_evicted;/*!< number of uncompressed page images
				evicted */
};

/** Remove a block from the flush list of modified blocks.
@param[in]	bpage	block to be removed from the flush list */
void buf_flush_remove(buf_page_t* bpage);

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage has already been
copied to dpage. */
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage);	/*!< in/out: destination block */
/** Update the flush system data structures when a write is completed.
@param[in,out]	bpage	flushed page
@param[in]	dblwr	whether the doublewrite buffer was used */
void buf_flush_write_complete(buf_page_t* bpage, bool dblwr);

/** Assign the full crc32 checksum for non-compressed page.
@param[in,out]	page	page to be updated */
void buf_flush_assign_full_crc32_checksum(byte* page);

/** Initialize a page for writing to the tablespace.
@param[in]	block			buffer block; NULL if bypassing the buffer pool
@param[in,out]	page			page frame
@param[in,out]	page_zip_		compressed page, or NULL if uncompressed
@param[in]	use_full_checksum	whether tablespace uses full checksum */
void
buf_flush_init_for_writing(
	const buf_block_t*	block,
	byte*			page,
	void*			page_zip_,
	bool			use_full_checksum);

# if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/** Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: block and LRU list mutexes must be held upon entering this function, and
they will be released by this function after flushing. This is loosely based on
buf_flush_batch() and buf_flush_page().
@param[in,out]	block		buffer control block
@return whether the page was flushed and the mutex released */
bool buf_flush_page_try(buf_block_t* block)
	MY_ATTRIBUTE((warn_unused_result));
# endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
/** Do flushing batch of a given type.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in]	type		flush type
@param[in]	min_n		wished minimum mumber of blocks flushed
(it is not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n		the number of pages which were processed is
passed back to caller. Ignored if NULL
@retval true	if a batch was queued successfully.
@retval false	if another batch of same type was already running. */
bool
buf_flush_do_batch(
	buf_flush_t		type,
	ulint			min_n,
	lsn_t			lsn_limit,
	flush_counters_t*	n);

/** This utility flushes dirty blocks from the end of the flush list.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL.
@retval true if a batch was queued successfully
@retval false if another batch of same type was already running */
bool buf_flush_lists(ulint min_n, lsn_t lsn_limit, ulint *n_processed);

/******************************************************************//**
This function picks up a single page from the tail of the LRU
list, flushes it (if it is dirty), removes it from page_hash and LRU
list and puts it on the free list. It is called from user threads when
they are unable to find a replaceable page at the tail of the LRU
list i.e.: when the background LRU flushing in the page_cleaner thread
is not fast enough to keep pace with the workload.
@return true if success. */
bool buf_flush_single_page_from_LRU();

/** Wait until a flush batch ends.
@param[in]	type	BUF_FLUSH_LRU or BUF_FLUSH_LIST */
void buf_flush_wait_batch_end(buf_flush_t type);
/** Wait until a flush batch of the given lsn ends
@param[in]	new_oldest	target oldest_modified_lsn to wait for */
void buf_flush_wait_flushed(lsn_t new_oldest);
/********************************************************************//**
This function should be called at a mini-transaction commit, if a page was
modified in it. Puts the block to the list of modified blocks, if it not
already in it. */
UNIV_INLINE
void
buf_flush_note_modification(
/*========================*/
	buf_block_t*	block,		/*!< in: block which is modified */
	lsn_t		start_lsn,	/*!< in: start lsn of the first mtr in a
					set of mtr's */
	lsn_t		end_lsn);	/*!< in: end lsn of the last mtr in the
					set of mtr's */
/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., transition FILE_PAGE => NOT_USED allowed.
@return TRUE if can replace immediately */
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage);	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */

/** Initialize page_cleaner. */
void buf_flush_page_cleaner_init();

/** Wait for any possible LRU flushes to complete. */
void buf_flush_wait_LRU_batch_end();

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validate the flush list. */
void buf_flush_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
void
buf_flush_init_flush_rbt(void);
/*==========================*/

/********************************************************************//**
Frees up the red-black tree. */
void
buf_flush_free_flush_rbt(void);
/*==========================*/

/** Write a flushable page asynchronously from the buffer pool to a file.
NOTE: 1. in simulated aio we must call os_aio_simulated_wake_handler_threads
after we have posted a batch of writes! 2. buf_page_get_mutex(bpage) must be
held upon entering this function. The LRU list mutex must be held if flush_type
== BUF_FLUSH_SINGLE_PAGE. Both mutexes will be released by this function if it
returns true.
@param[in]	bpage		buffer control block
@param[in]	flush_type	type of flush
@param[in]	sync		true if sync IO request
@return whether the page was flushed */
bool buf_flush_page(buf_page_t* bpage, buf_flush_t flush_type, bool sync);

/** Check if the block is modified and ready for flushing.
@param[in]	bpage		buffer control block, must be buf_page_in_file()
@param[in]	flush_type	type of flush
@return true if can flush immediately */
bool
buf_flush_ready_for_flush(
/*======================*/
	buf_page_t*	bpage,	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) */
	buf_flush_t	flush_type)/*!< in: type of flush */
	MY_ATTRIBUTE((warn_unused_result));

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync();

/** Request IO burst and wake page_cleaner up.
@param[in]	lsn_limit	upper limit of LSN to be flushed */
void buf_flush_request_force(lsn_t lsn_limit);

#include "buf0flu.ic"

#endif
