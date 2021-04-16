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

/** Number of pages flushed. Protected by buf_pool.mutex. */
extern ulint buf_flush_page_count;
/** Number of pages flushed via LRU. Protected by buf_pool.mutex.
Also included in buf_flush_page_count. */
extern ulint buf_lru_flush_page_count;

/** Flag indicating if the page_cleaner is in active state. */
extern bool buf_page_cleaner_is_active;

#ifdef UNIV_DEBUG

/** Value of MySQL global variable used to disable page cleaner. */
extern my_bool		innodb_page_cleaner_disabled_debug;

#endif /* UNIV_DEBUG */

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param id    tablespace identifier */
void buf_flush_remove_pages(ulint id);

/** Try to flush all the dirty pages that belong to a given tablespace.
@param id    tablespace identifier
@return number dirty pages that there were for this tablespace */
ulint buf_flush_dirty_pages(ulint id)
  MY_ATTRIBUTE((warn_unused_result));

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage has already been
copied to dpage. */
ATTRIBUTE_COLD
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage);	/*!< in/out: destination block */

/** Complete write of a file page from buf_pool.
@param request write request */
void buf_page_write_complete(const IORequest &request);

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

/** Write out dirty blocks from buf_pool.flush_list.
@param max_n    wished maximum mumber of blocks flushed
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target (0=LRU flush)
@return the number of processed pages
@retval 0 if a batch of the same type (lsn==0 or lsn!=0) is already running */
ulint buf_flush_lists(ulint max_n, lsn_t lsn);

/** Wait until a flush batch ends.
@param lru    true=buf_pool.LRU; false=buf_pool.flush_list */
void buf_flush_wait_batch_end(bool lru);
/** Wait until all persistent pages are flushed up to a limit.
@param sync_lsn   buf_pool.get_oldest_modification(LSN_MAX) to wait for */
ATTRIBUTE_COLD void buf_flush_wait_flushed(lsn_t sync_lsn);
/** If innodb_flush_sync=ON, initiate a furious flush.
@param lsn buf_pool.get_oldest_modification(LSN_MAX) target */
void buf_flush_ahead(lsn_t lsn);

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
ATTRIBUTE_COLD void buf_flush_page_cleaner_init();

/** Wait for pending flushes to complete. */
void buf_flush_wait_batch_end_acquiring_mutex(bool lru);

/** Flush the buffer pool on shutdown. */
ATTRIBUTE_COLD void buf_flush_buffer_pool();

#ifdef UNIV_DEBUG
/** Validate the flush list. */
void buf_flush_validate();
#endif /* UNIV_DEBUG */

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync();

#include "buf0flu.ic"

#endif
