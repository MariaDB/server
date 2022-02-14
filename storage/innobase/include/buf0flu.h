/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2022, MariaDB Corporation.

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

#pragma once

#include "ut0byte.h"
#include "log0log.h"
#include "buf0buf.h"

/** Number of pages flushed. Protected by buf_pool.mutex. */
extern ulint buf_flush_page_count;
/** Number of pages flushed via LRU. Protected by buf_pool.mutex.
Also included in buf_flush_page_count. */
extern ulint buf_lru_flush_page_count;
/** Number of pages freed without flushing. Protected by buf_pool.mutex. */
extern ulint buf_lru_freed_page_count;

/** Flag indicating if the page_cleaner is in active state. */
extern Atomic_relaxed<bool> buf_page_cleaner_is_active;

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param id    tablespace identifier */
void buf_flush_remove_pages(uint32_t id);

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

/** Try to flush dirty pages that belong to a given tablespace.
@param space       tablespace
@param n_flushed   number of pages written
@return whether the flush for some pages might not have been initiated */
bool buf_flush_list_space(fil_space_t *space, ulint *n_flushed= nullptr)
  MY_ATTRIBUTE((warn_unused_result));

/** Write out dirty blocks from buf_pool.LRU.
@param max_n    wished maximum mumber of blocks flushed
@return the number of processed pages
@retval 0 if a buf_pool.LRU batch is already running */
ulint buf_flush_LRU(ulint max_n);

/** Wait until a flush batch ends.
@param lru    true=buf_pool.LRU; false=buf_pool.flush_list */
void buf_flush_wait_batch_end(bool lru);
/** Wait until all persistent pages are flushed up to a limit.
@param sync_lsn   buf_pool.get_oldest_modification(LSN_MAX) to wait for */
ATTRIBUTE_COLD void buf_flush_wait_flushed(lsn_t sync_lsn);
/** Initiate more eager page flushing if the log checkpoint age is too old.
@param lsn      buf_pool.get_oldest_modification(LSN_MAX) target
@param furious  true=furious flushing, false=limit to innodb_io_capacity */
ATTRIBUTE_COLD void buf_flush_ahead(lsn_t lsn, bool furious);

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

/** Synchronously flush dirty blocks during recv_sys_t::apply().
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync_batch(lsn_t lsn);

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync();
