/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file include/buf0lru.h
The database buffer pool LRU replacement algorithm

Created 11/5/1995 Heikki Tuuri
*******************************************************/

#pragma once

#include "buf0types.h"
#include "hash0hash.h"

// Forward declaration
struct trx_t;
struct fil_space_t;

/** Flush this many pages in buf_LRU_get_free_block() */
extern size_t innodb_lru_flush_size;

/*#######################################################################
These are low-level functions
#########################################################################*/

/** Minimum LRU list length for which the LRU_old pointer is defined */
#define BUF_LRU_OLD_MIN_LEN	512	/* 8 megabytes of 16k pages */

/** Try to free a block. If bpage is a descriptor of a compressed-only
ROW_FORMAT=COMPRESSED page, the buf_page_t object will be freed as well.
The caller must hold buf_pool.mutex.
@param bpage      block to be freed
@param zip        whether to remove both copies of a ROW_FORMAT=COMPRESSED page
@retval true if freed and buf_pool.mutex may have been temporarily released
@retval false if the page was not freed */
bool buf_LRU_free_page(buf_page_t *bpage, bool zip)
  MY_ATTRIBUTE((nonnull));

/** Try to free a replaceable block.
@param limit  maximum number of blocks to scan
@return true if found and freed */
bool buf_LRU_scan_and_free_block(ulint limit= ULINT_UNDEFINED);

/** @return a buffer block from the buf_pool.free list
@retval	NULL	if the free list is empty */
buf_block_t* buf_LRU_get_free_only();

/** Get a block from the buf_pool.free list.
If the list is empty, blocks will be moved from the end of buf_pool.LRU
to buf_pool.free.

This function is called from a user thread when it needs a clean
block to read in a page. Note that we only ever get a block from
the free list. Even when we flush a page or find a page in LRU scan
we put it to free list to be used.
* iteration 0:
  * get a block from the buf_pool.free list, success:done
  * if buf_pool.try_LRU_scan is set
    * scan LRU up to 100 pages to free a clean block
    * success:retry the free list
  * flush up to innodb_lru_flush_size LRU blocks to data files
    (until UT_LIST_GET_GEN(buf_pool.free) < innodb_lru_scan_depth)
    * on buf_page_write_complete() the blocks will put on buf_pool.free list
    * success: retry the free list
* subsequent iterations: same as iteration 0 except:
  * scan whole LRU list
  * scan LRU list even if buf_pool.try_LRU_scan is not set

@param have_mutex  whether buf_pool.mutex is already being held
@return the free control block, in state BUF_BLOCK_MEMORY */
buf_block_t* buf_LRU_get_free_block(bool have_mutex)
	MY_ATTRIBUTE((malloc,warn_unused_result));

/** @return whether the unzip_LRU list should be used for evicting a victim
instead of the general LRU list */
bool buf_LRU_evict_from_unzip_LRU();

/** Puts a block back to the free list.
@param[in]	block	block; not containing a file page */
void
buf_LRU_block_free_non_file_page(buf_block_t* block);
/******************************************************************//**
Adds a block to the LRU list. Please make sure that the page_size is
already set when invoking the function, so that we can get correct
page_size from the buffer page when adding a block into LRU */
void
buf_LRU_add_block(
/*==============*/
	buf_page_t*	bpage,	/*!< in: control block */
	bool		old);	/*!< in: true if should be put to the old
				blocks in the LRU list, else put to the
				start; if the LRU list is very short, added to
				the start regardless of this parameter */
/******************************************************************//**
Adds a block to the LRU list of decompressed zip pages. */
void
buf_unzip_LRU_add_block(
/*====================*/
	buf_block_t*	block,	/*!< in: control block */
	ibool		old);	/*!< in: TRUE if should be put to the end
				of the list, else put to the start */

/** Update buf_pool.LRU_old_ratio.
@param[in]	old_pct		Reserve this percentage of
				the buffer pool for "old" blocks
@param[in]	adjust		true=adjust the LRU list;
				false=just assign buf_pool.LRU_old_ratio
				during the initialization of InnoDB
@return updated old_pct */
uint buf_LRU_old_ratio_update(uint old_pct, bool adjust);
/********************************************************************//**
Update the historical stats that we are collecting for LRU eviction
policy at the end of each interval. */
void
buf_LRU_stat_update();

#ifdef UNIV_DEBUG
/** Validate the LRU list. */
void buf_LRU_validate();
#endif /* UNIV_DEBUG */
#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG
/** Dump the LRU list to stderr. */
void buf_LRU_print();
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG */

/** @name Heuristics for detecting index scan @{ */
/** The denominator of buf_pool.LRU_old_ratio. */
#define BUF_LRU_OLD_RATIO_DIV	1024
/** Maximum value of buf_pool.LRU_old_ratio.
@see buf_LRU_old_adjust_len
@see buf_pool.LRU_old_ratio_update */
#define BUF_LRU_OLD_RATIO_MAX	BUF_LRU_OLD_RATIO_DIV
/** Minimum value of buf_pool.LRU_old_ratio.
@see buf_LRU_old_adjust_len
@see buf_pool.LRU_old_ratio_update
The minimum must exceed
(BUF_LRU_OLD_TOLERANCE + 5) * BUF_LRU_OLD_RATIO_DIV / BUF_LRU_OLD_MIN_LEN. */
#define BUF_LRU_OLD_RATIO_MIN	51

#if BUF_LRU_OLD_RATIO_MIN >= BUF_LRU_OLD_RATIO_MAX
# error "BUF_LRU_OLD_RATIO_MIN >= BUF_LRU_OLD_RATIO_MAX"
#endif
#if BUF_LRU_OLD_RATIO_MAX > BUF_LRU_OLD_RATIO_DIV
# error "BUF_LRU_OLD_RATIO_MAX > BUF_LRU_OLD_RATIO_DIV"
#endif

/** Move blocks to "new" LRU list only if the first access was at
least this many milliseconds ago.  Not protected by any mutex or latch. */
extern uint	buf_LRU_old_threshold_ms;
/* @} */

/** @brief Statistics for selecting the LRU list for eviction.

These statistics are not 'of' LRU but 'for' LRU.  We keep count of I/O
and page_zip_decompress() operations.  Based on the statistics we decide
if we want to evict from buf_pool.unzip_LRU or buf_pool.LRU. */
struct buf_LRU_stat_t
{
	ulint	io;	/**< Counter of buffer pool I/O operations. */
	ulint	unzip;	/**< Counter of page_zip_decompress operations. */
};

/** Current operation counters.  Not protected by any mutex.
Cleared by buf_LRU_stat_update(). */
extern buf_LRU_stat_t	buf_LRU_stat_cur;

/** Running sum of past values of buf_LRU_stat_cur.
Updated by buf_LRU_stat_update().  Protected by buf_pool.mutex. */
extern buf_LRU_stat_t	buf_LRU_stat_sum;

/********************************************************************//**
Increments the I/O counter in buf_LRU_stat_cur. */
#define buf_LRU_stat_inc_io() buf_LRU_stat_cur.io++
/********************************************************************//**
Increments the page_zip_decompress() counter in buf_LRU_stat_cur. */
#define buf_LRU_stat_inc_unzip() buf_LRU_stat_cur.unzip++
