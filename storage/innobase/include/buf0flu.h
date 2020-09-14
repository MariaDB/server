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

class ut_stage_alter_t;

/** Handled page counters for a single flush */
struct flush_counters_t {
	ulint	flushed;	/*!< number of dirty pages flushed */
	ulint	evicted;	/*!< number of clean pages evicted */
	ulint	unzip_LRU_evicted;/*!< number of uncompressed page images
				evicted */
};

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param id    tablespace identifier */
void buf_flush_remove_pages(ulint id);

/** Flush all the dirty pages that belong to a given tablespace.
@param id    tablespace identifier */
void buf_flush_dirty_pages(ulint id);

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage has already been
copied to dpage. */
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage);	/*!< in/out: destination block */

/** Complete write of a file page from buf_pool.
@param bpage   written page
@param request write request
@param dblwr   whether the doublewrite buffer was used
@param evict   whether or not to evict the page from LRU list */
void buf_page_write_complete(buf_page_t *bpage, const IORequest &request,
                             bool dblwr, bool evict);

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

/** Initiate a flushing batch.
@param max_n  wished minimum mumber of blocks flushed
@param lsn    0 for buf_pool.LRU flushing; otherwise,
              stop the buf_pool.flush_list batch on oldest_modification>=lsn
@param n      the number of processed pages
@return whether a batch was queued successfully (not running already) */
bool buf_flush_do_batch(ulint max_n, lsn_t lsn, flush_counters_t *n);

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

/** Wait until a flush batch ends.
@param[in]	lru	true=buf_pool.LRU; false=buf_pool.flush_list */
void buf_flush_wait_batch_end(bool lru);
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

/** Initialize page_cleaner. */
void buf_flush_page_cleaner_init();

/** Wait for pending flushes to complete. */
void buf_flush_wait_batch_end_acquiring_mutex(bool lru);

#ifdef UNIV_DEBUG
/** Validate the flush list. */
void buf_flush_validate();
#endif /* UNIV_DEBUG */

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync();

#include "buf0flu.ic"

#endif
