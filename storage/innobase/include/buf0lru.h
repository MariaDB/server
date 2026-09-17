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

/** How to acquire a block */
enum buf_LRU_get {
  /** The caller is not holding buf_pool.mutex */
  have_no_mutex= 0,
  /** The caller is holding buf_pool.mutex */
  have_mutex,
  /** The caller is not holding buf_pool.mutex and is OK if a block
  cannot be allocated. */
  have_no_mutex_soft
};

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
	MY_ATTRIBUTE((malloc,warn_unused_result));

#define buf_block_alloc() buf_LRU_get_free_block(have_no_mutex)

/** @return whether the unzip_LRU list should be used for evicting a victim
instead of the general LRU list */
bool buf_LRU_evict_from_unzip_LRU();

/** Free a buffer block which does not contain a file page,
while holding buf_pool.mutex.
@param block   block to be put to buf_pool.free */
void buf_LRU_block_free_non_file_page(buf_block_t *block);

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

/** Move a block to the "recently used" end of buf_pool.LRU. */
void buf_page_make_young(buf_page_t *bpage);

/******************************************************************//**
Adds a block to the LRU list of decompressed zip pages. */
void
buf_unzip_LRU_add_block(
/*====================*/
	buf_block_t*	block,	/*!< in: control block */
	ibool		old);	/*!< in: TRUE if should be put to the end
				of the list, else put to the start */

/** Evict the temporary tablespace pages above the given threshold
@param threshold  Above this page to be removed from LRU list */
void buf_LRU_truncate_temp(uint32_t threshold);

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
