/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2014, 2022, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
@file include/buf0buf.ic
The database buffer buf_pool

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#include "mtr0mtr.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "fsp0types.h"

/** Determine if a block is still close enough to the MRU end of the LRU list
meaning that it is not in danger of getting evicted and also implying
that it has been accessed recently.
The page must be either buffer-fixed, or its page hash must be locked.
@param[in]	bpage		buffer pool page
@return whether bpage is close to MRU end of LRU */
inline bool buf_page_peek_if_young(const buf_page_t *bpage)
{
	/* FIXME: bpage->freed_page_clock is 31 bits */
	return((buf_pool.freed_page_clock & ((1UL << 31) - 1))
	       < (bpage->freed_page_clock
		  + (buf_pool.curr_size
		     * (BUF_LRU_OLD_RATIO_DIV - buf_pool.LRU_old_ratio)
		     / (BUF_LRU_OLD_RATIO_DIV * 4))));
}

/** Determine if a block should be moved to the start of the LRU list if
there is danger of dropping from the buffer pool.
@param[in]	bpage		buffer pool page
@return true if bpage should be made younger */
inline bool buf_page_peek_if_too_old(const buf_page_t *bpage)
{
	if (buf_pool.freed_page_clock == 0) {
		/* If eviction has not started yet, do not update the
		statistics or move blocks in the LRU list.  This is
		either the warm-up phase or an in-memory workload. */
		return(FALSE);
	} else if (buf_LRU_old_threshold_ms && bpage->old) {
		uint32_t access_time = bpage->is_accessed();

		/* It is possible that the below comparison returns an
		unexpected result. 2^32 milliseconds pass in about 50 days,
		so if the difference between ut_time_ms() and access_time
		is e.g. 50 days + 15 ms, then the below will behave as if
		it is 15 ms. This is known and fixing it would require to
		increase buf_page_t::access_time from 32 to 64 bits. */
		if (access_time
		    && ((ib_uint32_t) (ut_time_ms() - access_time))
		    >= buf_LRU_old_threshold_ms) {
			return(TRUE);
		}

		buf_pool.stat.n_pages_not_made_young++;
		return false;
	} else {
		return !buf_page_peek_if_young(bpage);
	}
}

/** Allocate a buffer block.
@return own: the allocated block, in state BUF_BLOCK_MEMORY */
inline buf_block_t *buf_block_alloc()
{
  return buf_LRU_get_free_block(false);
}

/********************************************************************//**
Frees a buffer block which does not contain a file page. */
UNIV_INLINE
void
buf_block_free(
/*===========*/
	buf_block_t*	block)	/*!< in, own: block to be freed */
{
	mysql_mutex_lock(&buf_pool.mutex);
	buf_LRU_block_free_non_file_page(block);
	mysql_mutex_unlock(&buf_pool.mutex);
}

/********************************************************************//**
Increments the modify clock of a frame by 1. The caller must (1) own the
buf_pool mutex and block bufferfix count has to be zero, (2) or own an x-lock
on the block. */
UNIV_INLINE
void
buf_block_modify_clock_inc(
/*=======================*/
	buf_block_t*	block)	/*!< in: block */
{
#ifdef SAFE_MUTEX
	ut_ad((mysql_mutex_is_owner(&buf_pool.mutex)
	       && !block->page.buf_fix_count())
	      || block->page.lock.have_u_or_x());
#else /* SAFE_MUTEX */
	ut_ad(!block->page.buf_fix_count() || block->page.lock.have_u_or_x());
#endif /* SAFE_MUTEX */
	assert_block_ahi_valid(block);

	block->modify_clock++;
}

/********************************************************************//**
Returns the value of the modify clock. The caller must have an s-lock
or x-lock on the block.
@return value */
UNIV_INLINE
ib_uint64_t
buf_block_get_modify_clock(
/*=======================*/
	buf_block_t*	block)	/*!< in: block */
{
	ut_ad(block->page.lock.have_any());
	return(block->modify_clock);
}
