/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2014, 2020, MariaDB Corporation.

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

/*********************************************************************//**
Gets the current size of buffer buf_pool in bytes.
@return size in bytes */
UNIV_INLINE
ulint
buf_pool_get_curr_size(void)
/*========================*/
{
	return(srv_buf_pool_curr_size);
}

/********************************************************************//**
Reads the freed_page_clock of a buffer block.
@return freed_page_clock */
UNIV_INLINE
unsigned
buf_page_get_freed_page_clock(
/*==========================*/
	const buf_page_t*	bpage)	/*!< in: block */
{
	/* This is sometimes read without holding buf_pool.mutex. */
	return(bpage->freed_page_clock);
}

/********************************************************************//**
Reads the freed_page_clock of a buffer block.
@return freed_page_clock */
UNIV_INLINE
unsigned
buf_block_get_freed_page_clock(
/*===========================*/
	const buf_block_t*	block)	/*!< in: block */
{
	return(buf_page_get_freed_page_clock(&block->page));
}

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

#ifdef UNIV_DEBUG
/*********************************************************************//**
Gets a pointer to the memory frame of a block.
@return pointer to the frame */
UNIV_INLINE
buf_frame_t*
buf_block_get_frame(
/*================*/
	const buf_block_t*	block)	/*!< in: pointer to the control block */
{
	if (!block) {
		return NULL;
	}

	switch (block->page.state()) {
	case BUF_BLOCK_ZIP_PAGE:
	case BUF_BLOCK_NOT_USED:
		ut_error;
		break;
	case BUF_BLOCK_FILE_PAGE:
		ut_a(block->page.buf_fix_count());
		/* fall through */
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		goto ok;
	}
	ut_error;
ok:
	return((buf_frame_t*) block->frame);
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
Allocates a buf_page_t descriptor. This function must succeed. In case
of failure we assert in this function.
@return: the allocated descriptor. */
UNIV_INLINE
buf_page_t*
buf_page_alloc_descriptor(void)
/*===========================*/
{
	buf_page_t*	bpage;

	bpage = (buf_page_t*) ut_zalloc_nokey(sizeof *bpage);
	ut_ad(bpage);
	MEM_UNDEFINED(bpage, sizeof *bpage);

	return(bpage);
}

/********************************************************************//**
Free a buf_page_t descriptor. */
UNIV_INLINE
void
buf_page_free_descriptor(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: bpage descriptor to free. */
{
	ut_free(bpage);
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
	/* No latch is acquired for the shared temporary tablespace. */
	ut_ad(fsp_is_system_temporary(block->page.id().space())
	      || (mysql_mutex_is_owner(&buf_pool.mutex)
		  && !block->page.buf_fix_count())
	      || rw_lock_own_flagged(&block->lock,
				     RW_LOCK_FLAG_X | RW_LOCK_FLAG_SX));
#else /* SAFE_MUTEX */
	/* No latch is acquired for the shared temporary tablespace. */
	ut_ad(fsp_is_system_temporary(block->page.id().space())
	      || !block->page.buf_fix_count()
	      || rw_lock_own_flagged(&block->lock,
				     RW_LOCK_FLAG_X | RW_LOCK_FLAG_SX));
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
#ifdef UNIV_DEBUG
	/* No latch is acquired for the shared temporary tablespace. */
	if (!fsp_is_system_temporary(block->page.id().space())) {
		ut_ad(rw_lock_own(&(block->lock), RW_LOCK_S)
		      || rw_lock_own(&(block->lock), RW_LOCK_X)
		      || rw_lock_own(&(block->lock), RW_LOCK_SX));
	}
#endif /* UNIV_DEBUG */

	return(block->modify_clock);
}

/*******************************************************************//**
Increments the bufferfix count. */
UNIV_INLINE
void
buf_block_buf_fix_inc_func(
/*=======================*/
#ifdef UNIV_DEBUG
	const char*	file,	/*!< in: file name */
	unsigned	line,	/*!< in: line */
#endif /* UNIV_DEBUG */
	buf_block_t*	block)	/*!< in/out: block to bufferfix */
{
#ifdef UNIV_DEBUG
	/* No debug latch is acquired if block belongs to system temporary.
	Debug latch is not of much help if access to block is single
	threaded. */
	if (!fsp_is_system_temporary(block->page.id().space())) {
		ibool   ret;
		ret = rw_lock_s_lock_nowait(block->debug_latch, file, line);
		ut_a(ret);
	}
#endif /* UNIV_DEBUG */

	block->fix();
}

/*******************************************************************//**
Decrements the bufferfix count. */
UNIV_INLINE
void
buf_block_buf_fix_dec(
/*==================*/
	buf_block_t*	block)	/*!< in/out: block to bufferunfix */
{
#ifdef UNIV_DEBUG
	/* No debug latch is acquired if block belongs to system temporary.
	Debug latch is not of much help if access to block is single
	threaded. */
	if (!fsp_is_system_temporary(block->page.id().space())) {
		rw_lock_s_unlock(block->debug_latch);
	}
#endif /* UNIV_DEBUG */

	block->unfix();
}

/********************************************************************//**
Releases a compressed-only page acquired with buf_page_get_zip(). */
UNIV_INLINE
void
buf_page_release_zip(
/*=================*/
	buf_page_t*	bpage)		/*!< in: buffer block */
{
	ut_ad(bpage);
	ut_a(bpage->buf_fix_count());

	switch (bpage->state()) {
	case BUF_BLOCK_FILE_PAGE:
#ifdef UNIV_DEBUG
	{
		/* No debug latch is acquired if block belongs to system
		temporary. Debug latch is not of much help if access to block
		is single threaded. */
		buf_block_t*	block = reinterpret_cast<buf_block_t*>(bpage);
		if (!fsp_is_system_temporary(block->page.id().space())) {
			rw_lock_s_unlock(block->debug_latch);
		}
	}
#endif /* UNIV_DEBUG */
		/* Fall through */
	case BUF_BLOCK_ZIP_PAGE:
		reinterpret_cast<buf_block_t*>(bpage)->unfix();
		return;

	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		break;
	}

	ut_error;
}

/********************************************************************//**
Releases a latch, if specified. */
UNIV_INLINE
void
buf_page_release_latch(
/*===================*/
	buf_block_t*	block,		/*!< in: buffer block */
	ulint		rw_latch)	/*!< in: RW_S_LATCH, RW_X_LATCH,
					RW_NO_LATCH */
{
#ifdef UNIV_DEBUG
	/* No debug latch is acquired if block belongs to system
	temporary. Debug latch is not of much help if access to block
	is single threaded. */
	if (!fsp_is_system_temporary(block->page.id().space())) {
		rw_lock_s_unlock(block->debug_latch);
	}
#endif /* UNIV_DEBUG */

	if (rw_latch == RW_S_LATCH) {
		rw_lock_s_unlock(&block->lock);
	} else if (rw_latch == RW_SX_LATCH) {
		rw_lock_sx_unlock(&block->lock);
	} else if (rw_latch == RW_X_LATCH) {
		rw_lock_x_unlock(&block->lock);
	}
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Adds latch level info for the rw-lock protecting the buffer frame. This
should be called in the debug version after a successful latching of a
page if we know the latching order level of the acquired latch. */
UNIV_INLINE
void
buf_block_dbg_add_level(
/*====================*/
	buf_block_t*	block,	/*!< in: buffer page
				where we have acquired latch */
	latch_level_t	level)	/*!< in: latching order level */
{
	sync_check_lock(&block->lock, level);
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
Get buf frame. */
UNIV_INLINE
void *
buf_page_get_frame(
/*===============*/
	const buf_page_t*	bpage) /*!< in: buffer pool page */
{
	/* In encryption/compression buffer pool page may contain extra
	buffer where result is stored. */
	if (bpage->slot && bpage->slot->out_buf) {
		return bpage->slot->out_buf;
	} else if (bpage->zip.data) {
		return bpage->zip.data;
	} else {
		return ((buf_block_t*) bpage)->frame;
	}
}
