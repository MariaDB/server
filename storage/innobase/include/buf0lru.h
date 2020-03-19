/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

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

#ifndef buf0lru_h
#define buf0lru_h

#include "ut0byte.h"
#include "buf0types.h"

// Forward declaration
struct trx_t;
struct fil_space_t;

/** @return whether less than 1/4 of the buffer pool is available */
bool
buf_LRU_buf_pool_running_out();

/*#######################################################################
These are low-level functions
#########################################################################*/

/** Minimum LRU list length for which the LRU_old pointer is defined */
#define BUF_LRU_OLD_MIN_LEN	512	/* 8 megabytes of 16k pages */

#ifdef BTR_CUR_HASH_ADAPT
struct dict_table_t;
/** Try to drop the adaptive hash index for a tablespace.
@param[in,out]	table	table
@return	whether anything was dropped */
bool buf_LRU_drop_page_hash_for_tablespace(dict_table_t* table)
	MY_ATTRIBUTE((warn_unused_result,nonnull));
#else
# define buf_LRU_drop_page_hash_for_tablespace(table)
#endif /* BTR_CUR_HASH_ADAPT */

/** Empty the flush list for all pages belonging to a tablespace.
@param[in]	id		tablespace identifier
@param[in]	flush		whether to write the pages to files
@param[in]	first		first page to be flushed or evicted */
void buf_LRU_flush_or_remove_pages(ulint id, bool flush, ulint first = 0);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Insert a compressed block into buf_pool.zip_clean in the LRU order.
@param[in]	bpage	pointer to the block in question */
void
buf_LRU_insert_zip_clean(
	buf_page_t*	bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/** Try to free a block.  If bpage is a descriptor of a compressed-only
page, the descriptor object will be freed as well.
NOTE: this function may temporarily release and relock the
buf_page_get_get_mutex(). Furthermore, the page frame will no longer be
accessible via bpage. If this function returns true, it will also release
the LRU list mutex.
The caller must hold the LRU list and buf_page_get_mutex() mutexes.
@param[in]	bpage	block to be freed
@param[in]	zip	true if should remove also the compressed page of
			an uncompressed page
@return true if freed, false otherwise. */
bool
buf_LRU_free_page(
	buf_page_t*	bpage,
	bool		zip)

	MY_ATTRIBUTE((nonnull));

/** Try to free a replaceable block.
@param[in]	scan_all	true=scan the whole LRU list,
				false=use BUF_LRU_SEARCH_SCAN_THRESHOLD
@return true if found and freed */
bool buf_LRU_scan_and_free_block(bool scan_all);

/** @return a buffer block from the buf_pool.free list
@retval	NULL	if the free list is empty */
buf_block_t* buf_LRU_get_free_only();

/** Get a free block from the buf_pool. The block is taken off the
@param[in]	buf_pool	buffer pool instance
free list. If free list is empty, blocks are moved from the end of the
LRU list to the free list.
This function is called from a user thread when it needs a clean
block to read in a page. Note that we only ever get a block from
the free list. Even when we flush a page or find a page in LRU scan
we put it to free list to be used.
* iteration 0:
  * get a block from free list, success:done
  * if buf_pool.try_LRU_scan is set
    * scan LRU up to srv_LRU_scan_depth to find a clean block
    * the above will put the block on free list
    * success:retry the free list
  * flush one dirty page from tail of LRU to disk
    * the above will put the block on free list
    * success: retry the free list
* iteration 1:
  * same as iteration 0 except:
    * scan whole LRU list
    * scan LRU list even if buf_pool.try_LRU_scan is not set
* iteration > 1:
  * same as iteration 1 but sleep 10ms
@param[in,out]	buf_pool	buffer pool instance
@return the free control block, in state BUF_BLOCK_READY_FOR_USE */
buf_block_t* buf_LRU_get_free_block()
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
	ibool		old);	/*!< in: TRUE if should be put to the old
				blocks in the LRU list, else put to the
				start; if the LRU list is very short, added to
				the start regardless of this parameter */

/** Adds a block to the LRU list of decompressed zip pages.
@param[in]	block	control block
@param[in]	old	TRUE if should be put to the end of the list,
			else put to the start */
void
buf_unzip_LRU_add_block(
	buf_block_t*	block,
	ibool		old);

/** Moves a block to the start of the LRU list.
@param[in]	bpage	control block */
void
buf_LRU_make_block_young(buf_page_t* bpage);

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

/** Remove one page from LRU list and put it to free list.
@param[in,out]	bpage		block, must contain a file page and be in
				a freeable state; there may or may not be a
				hash index to the page
@param[in]	old_page_id	page number before bpage->id was invalidated */
void buf_LRU_free_one_page(buf_page_t* bpage, page_id_t old_page_id)
	MY_ATTRIBUTE((nonnull));


/** Adjust LRU hazard pointers if needed.
@param[in]	bpage	buffer page descriptor */
void buf_LRU_adjust_hp(const buf_page_t* bpage);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validate the LRU list. */
void buf_LRU_validate();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#if defined UNIV_DEBUG_PRINT || defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Dump the LRU list to stderr. */
void buf_LRU_print();
#endif /* UNIV_DEBUG_PRINT || UNIV_DEBUG || UNIV_BUF_DEBUG */

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
Updated by buf_LRU_stat_update(). Accesses protected by memory barriers. */
extern buf_LRU_stat_t	buf_LRU_stat_sum;

/********************************************************************//**
Increments the I/O counter in buf_LRU_stat_cur. */
#define buf_LRU_stat_inc_io() buf_LRU_stat_cur.io++
/********************************************************************//**
Increments the page_zip_decompress() counter in buf_LRU_stat_cur. */
#define buf_LRU_stat_inc_unzip() buf_LRU_stat_cur.unzip++

#endif
