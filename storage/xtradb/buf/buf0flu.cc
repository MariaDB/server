/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates
Copyright (c) 2013, 2016, MariaDB
Copyright (c) 2013, 2014, Fusion-io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0flu.cc
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "buf0flu.h"

#ifdef UNIV_NONINL
#include "buf0flu.ic"
#endif

#include "buf0buf.h"
#include "buf0mtflu.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#ifndef UNIV_HOTBACKUP
#include "ut0byte.h"
#include "ut0lst.h"
#include "page0page.h"
#include "fil0fil.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "os0file.h"
#include "os0sync.h"
#include "trx0sys.h"
#include "srv0mon.h"
#include "mysql/plugin.h"
#include "mysql/service_thd_wait.h"
#include "fil0pagecompress.h"

/** Number of pages flushed through non flush_list flushes. */
// static ulint buf_lru_flush_page_count = 0;

/** Flag indicating if the page_cleaner is in active state. This flag
is set to TRUE by the page_cleaner thread when it is spawned and is set
back to FALSE at shutdown by the page_cleaner as well. Therefore no
need to protect it by a mutex. It is only ever read by the thread
doing the shutdown */
UNIV_INTERN ibool buf_page_cleaner_is_active = FALSE;

/** Flag indicating if the lru_manager is in active state. */
UNIV_INTERN bool buf_lru_manager_is_active = false;

#ifdef UNIV_PFS_THREAD
UNIV_INTERN mysql_pfs_key_t buf_page_cleaner_thread_key;
UNIV_INTERN mysql_pfs_key_t buf_lru_manager_thread_key;
#endif /* UNIV_PFS_THREAD */

/* @} */

/******************************************************************//**
Increases flush_list size in bytes with zip_size for compressed page,
UNIV_PAGE_SIZE for uncompressed page in inline function */
static inline
void
incr_flush_list_size_in_bytes(
/*==========================*/
	buf_block_t*	block,		/*!< in: control block */
	buf_pool_t*	buf_pool)	/*!< in: buffer pool instance */
{
	ut_ad(buf_flush_list_mutex_own(buf_pool));
	ulint zip_size = page_zip_get_size(&block->page.zip);
	buf_pool->stat.flush_list_bytes += zip_size ? zip_size : UNIV_PAGE_SIZE;
	ut_ad(buf_pool->stat.flush_list_bytes <= buf_pool->curr_pool_size);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
static
ibool
buf_flush_validate_low(
/*===================*/
	buf_pool_t*	buf_pool);	/*!< in: Buffer pool instance */

/******************************************************************//**
Validates the flush list some of the time.
@return	TRUE if ok or the check was skipped */
static
ibool
buf_flush_validate_skip(
/*====================*/
	buf_pool_t*	buf_pool)	/*!< in: Buffer pool instance */
{
/** Try buf_flush_validate_low() every this many times */
# define BUF_FLUSH_VALIDATE_SKIP	23

	/** The buf_flush_validate_low() call skip counter.
	Use a signed type because of the race condition below. */
	static int buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly buf_flush_validate_low()
	check in debug builds. */
	if (--buf_flush_validate_count > 0) {
		return(TRUE);
	}

	buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;
	return(buf_flush_validate_low(buf_pool));
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/*******************************************************************//**
Sets hazard pointer during flush_list iteration. */
UNIV_INLINE
void
buf_flush_set_hp(
/*=============*/
	buf_pool_t*		buf_pool,/*!< in/out: buffer pool instance */
	const buf_page_t*	bpage)	/*!< in: buffer control block */
{
	ut_ad(buf_flush_list_mutex_own(buf_pool));
	ut_ad(buf_pool->flush_list_hp == NULL || bpage == NULL);
	ut_ad(!bpage || buf_page_in_file(bpage)
	      || buf_page_get_state(bpage) == BUF_BLOCK_REMOVE_HASH);
	ut_ad(!bpage || bpage->in_flush_list);
	ut_ad(!bpage || buf_pool_from_bpage(bpage) == buf_pool);

	buf_pool->flush_list_hp = bpage;
}

/*******************************************************************//**
Checks if the given block is a hazard pointer
@return true if bpage is hazard pointer */
UNIV_INLINE
bool
buf_flush_is_hp(
/*============*/
	buf_pool_t*		buf_pool,/*!< in: buffer pool instance */
	const buf_page_t*	bpage)	/*!< in: buffer control block */
{
	ut_ad(buf_flush_list_mutex_own(buf_pool));

	return(buf_pool->flush_list_hp == bpage);
}

/*******************************************************************//**
Whenever we move a block in flush_list (either to remove it or to
relocate it) we check the hazard pointer set by some other thread
doing the flush list scan. If the hazard pointer is the same as the
one we are about going to move then we set it to NULL to force a rescan
in the thread doing the batch. */
UNIV_INLINE
void
buf_flush_update_hp(
/*================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_page_t*	bpage)		/*!< in: buffer control block */
{
	ut_ad(buf_flush_list_mutex_own(buf_pool));

	if (buf_flush_is_hp(buf_pool, bpage)) {
		buf_flush_set_hp(buf_pool, NULL);
		MONITOR_INC(MONITOR_FLUSH_HP_RESCAN);
	}
}

/******************************************************************//**
Insert a block in the flush_rbt and returns a pointer to its
predecessor or NULL if no predecessor. The ordering is maintained
on the basis of the <oldest_modification, space, offset> key.
@return	pointer to the predecessor or NULL if no predecessor. */
static
buf_page_t*
buf_flush_insert_in_flush_rbt(
/*==========================*/
	buf_page_t*	bpage)	/*!< in: bpage to be inserted. */
{
	const ib_rbt_node_t*	c_node;
	const ib_rbt_node_t*	p_node;
	buf_page_t*		prev = NULL;
	buf_pool_t*		buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_flush_list_mutex_own(buf_pool));

	/* Insert this buffer into the rbt. */
	c_node = rbt_insert(buf_pool->flush_rbt, &bpage, &bpage);
	ut_a(c_node != NULL);

	/* Get the predecessor. */
	p_node = rbt_prev(buf_pool->flush_rbt, c_node);

	if (p_node != NULL) {
		buf_page_t**	value;
		value = rbt_value(buf_page_t*, p_node);
		prev = *value;
		ut_a(prev != NULL);
	}

	return(prev);
}

/*********************************************************//**
Delete a bpage from the flush_rbt. */
static
void
buf_flush_delete_from_flush_rbt(
/*============================*/
	buf_page_t*	bpage)	/*!< in: bpage to be removed. */
{
#ifdef UNIV_DEBUG
	ibool		ret = FALSE;
#endif /* UNIV_DEBUG */
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	ut_ad(buf_flush_list_mutex_own(buf_pool));

#ifdef UNIV_DEBUG
	ret =
#endif /* UNIV_DEBUG */
	rbt_delete(buf_pool->flush_rbt, &bpage);

	ut_ad(ret);
}

/*****************************************************************//**
Compare two modified blocks in the buffer pool. The key for comparison
is:
key = <oldest_modification, space, offset>
This comparison is used to maintian ordering of blocks in the
buf_pool->flush_rbt.
Note that for the purpose of flush_rbt, we only need to order blocks
on the oldest_modification. The other two fields are used to uniquely
identify the blocks.
@return	 < 0 if b2 < b1, 0 if b2 == b1, > 0 if b2 > b1 */
static
int
buf_flush_block_cmp(
/*================*/
	const void*	p1,		/*!< in: block1 */
	const void*	p2)		/*!< in: block2 */
{
	int			ret;
	const buf_page_t*	b1 = *(const buf_page_t**) p1;
	const buf_page_t*	b2 = *(const buf_page_t**) p2;
#ifdef UNIV_DEBUG
	buf_pool_t*		buf_pool = buf_pool_from_bpage(b1);
#endif /* UNIV_DEBUG */

	ut_ad(b1 != NULL);
	ut_ad(b2 != NULL);

	ut_ad(buf_flush_list_mutex_own(buf_pool));

	ut_ad(b1->in_flush_list);
	ut_ad(b2->in_flush_list);

	if (b2->oldest_modification > b1->oldest_modification) {
		return(1);
	} else if (b2->oldest_modification < b1->oldest_modification) {
		return(-1);
	}

	/* If oldest_modification is same then decide on the space. */
	ret = (int)(b2->space - b1->space);

	/* Or else decide ordering on the offset field. */
	return(ret ? ret : (int)(b2->offset - b1->offset));
}

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
UNIV_INTERN
void
buf_flush_init_flush_rbt(void)
/*==========================*/
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

		ut_ad(buf_pool->flush_rbt == NULL);

		/* Create red black tree for speedy insertions in flush list. */
		buf_pool->flush_rbt = rbt_create(
			sizeof(buf_page_t*), buf_flush_block_cmp);

		buf_flush_list_mutex_exit(buf_pool);
	}
}

/********************************************************************//**
Frees up the red-black tree. */
UNIV_INTERN
void
buf_flush_free_flush_rbt(void)
/*==========================*/
{
	ulint	i;

	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		buf_flush_list_mutex_enter(buf_pool);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		ut_a(buf_flush_validate_low(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

		rbt_free(buf_pool->flush_rbt);
		buf_pool->flush_rbt = NULL;

		buf_flush_list_mutex_exit(buf_pool);
	}
}

/********************************************************************//**
Inserts a modified block into the flush list. */
UNIV_INTERN
void
buf_flush_insert_into_flush_list(
/*=============================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_block_t*	block,		/*!< in/out: block which is modified */
	lsn_t		lsn)		/*!< in: oldest modification */
{
	ut_ad(log_flush_order_mutex_own());
	ut_ad(mutex_own(&block->mutex));

	buf_flush_list_mutex_enter(buf_pool);

	ut_ad((UT_LIST_GET_FIRST(buf_pool->flush_list) == NULL)
	      || (UT_LIST_GET_FIRST(buf_pool->flush_list)->oldest_modification
		  <= lsn));

	/* If we are in the recovery then we need to update the flush
	red-black tree as well. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		buf_flush_list_mutex_exit(buf_pool);
		buf_flush_insert_sorted_into_flush_list(buf_pool, block, lsn);
		return;
	}

	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(!block->page.in_flush_list);

	ut_d(block->page.in_flush_list = TRUE);
	block->page.oldest_modification = lsn;
	UT_LIST_ADD_FIRST(list, buf_pool->flush_list, &block->page);
	incr_flush_list_size_in_bytes(block, buf_pool);

#ifdef UNIV_DEBUG_VALGRIND
	{
		ulint	zip_size = buf_block_get_zip_size(block);

		if (zip_size) {
			UNIV_MEM_ASSERT_RW(block->page.zip.data, zip_size);
		} else {
			UNIV_MEM_ASSERT_RW(block->frame, UNIV_PAGE_SIZE);
		}
	}
#endif /* UNIV_DEBUG_VALGRIND */
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_skip(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_list_mutex_exit(buf_pool);
}

/********************************************************************//**
Inserts a modified block into the flush list in the right sorted position.
This function is used by recovery, because there the modifications do not
necessarily come in the order of lsn's. */
UNIV_INTERN
void
buf_flush_insert_sorted_into_flush_list(
/*====================================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	buf_block_t*	block,		/*!< in/out: block which is modified */
	lsn_t		lsn)		/*!< in: oldest modification */
{
	buf_page_t*	prev_b;
	buf_page_t*	b;

	ut_ad(log_flush_order_mutex_own());
	ut_ad(mutex_own(&block->mutex));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);

	buf_flush_list_mutex_enter(buf_pool);

	/* The field in_LRU_list is protected by buf_pool->LRU_list_mutex,
	which we are not holding.  However, while a block is in the flush
	list, it is dirty and cannot be discarded, not from the
	page_hash or from the LRU list.  At most, the uncompressed
	page frame of a compressed block may be discarded or created
	(copying the block->page to or from a buf_page_t that is
	dynamically allocated from buf_buddy_alloc()).  Because those
	transitions hold block->mutex and the flush list mutex (via
	buf_flush_relocate_on_flush_list()), there is no possibility
	of a race condition in the assertions below. */
	ut_ad(block->page.in_LRU_list);
	ut_ad(block->page.in_page_hash);
	/* buf_buddy_block_register() will take a block in the
	BUF_BLOCK_MEMORY state, not a file page. */
	ut_ad(!block->page.in_zip_hash);

	ut_ad(!block->page.in_flush_list);
	ut_d(block->page.in_flush_list = TRUE);
	block->page.oldest_modification = lsn;

#ifdef UNIV_DEBUG_VALGRIND
	{
		ulint	zip_size = buf_block_get_zip_size(block);

		if (zip_size) {
			UNIV_MEM_ASSERT_RW(block->page.zip.data, zip_size);
		} else {
			UNIV_MEM_ASSERT_RW(block->frame, UNIV_PAGE_SIZE);
		}
	}
#endif /* UNIV_DEBUG_VALGRIND */

	prev_b = NULL;

	/* For the most part when this function is called the flush_rbt
	should not be NULL. In a very rare boundary case it is possible
	that the flush_rbt has already been freed by the recovery thread
	before the last page was hooked up in the flush_list by the
	io-handler thread. In that case we'll  just do a simple
	linear search in the else block. */
	if (buf_pool->flush_rbt) {

		prev_b = buf_flush_insert_in_flush_rbt(&block->page);

	} else {

		b = UT_LIST_GET_FIRST(buf_pool->flush_list);

		while (b && b->oldest_modification
		       > block->page.oldest_modification) {
			ut_ad(b->in_flush_list);
			prev_b = b;
			b = UT_LIST_GET_NEXT(list, b);
		}
	}

	if (prev_b == NULL) {
		UT_LIST_ADD_FIRST(list, buf_pool->flush_list, &block->page);
	} else {
		UT_LIST_INSERT_AFTER(list, buf_pool->flush_list,
				     prev_b, &block->page);
	}

	incr_flush_list_size_in_bytes(block, buf_pool);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_low(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_list_mutex_exit(buf_pool);
}

/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., the transition FILE_PAGE => NOT_USED allowed.
@return	TRUE if can replace immediately */
UNIV_INTERN
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage)	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */
{
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));
#endif /* UNIV_DEBUG */
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(bpage->in_LRU_list);

	if (buf_page_in_file(bpage)) {

		return(bpage->oldest_modification == 0
		       && bpage->buf_fix_count == 0
		       && buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	}

	ut_print_timestamp(stderr);
	fprintf(stderr,
		"  InnoDB: Error: buffer block state %lu"
		" in the LRU list!\n",
		(ulong) buf_page_get_state(bpage));
	ut_print_buf(stderr, bpage, sizeof(buf_page_t));
	putc('\n', stderr);

	return(FALSE);
}

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
{
	ut_ad(flush_type < BUF_FLUSH_N_TYPES);
	ut_ad(mutex_own(buf_page_get_mutex(bpage))
	      || flush_type == BUF_FLUSH_LIST);
	ut_a(buf_page_in_file(bpage) || buf_page_get_state(bpage) == BUF_BLOCK_REMOVE_HASH);

	if (bpage->oldest_modification == 0
	    || buf_page_get_io_fix_unlocked(bpage) != BUF_IO_NONE) {
		return(false);
	}

	ut_ad(bpage->in_flush_list);

	switch (flush_type) {
	case BUF_FLUSH_LIST:
		return(buf_page_get_state(bpage) != BUF_BLOCK_REMOVE_HASH);
	case BUF_FLUSH_LRU:
	case BUF_FLUSH_SINGLE_PAGE:
		return(true);

	case BUF_FLUSH_N_TYPES:
		break;
	}

	ut_error;
	return(false);
}

/********************************************************************//**
Remove a block from the flush list of modified blocks. */
UNIV_INTERN
void
buf_flush_remove(
/*=============*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ulint		zip_size;

	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_ad(buf_page_get_state(bpage) != BUF_BLOCK_ZIP_DIRTY
	      || mutex_own(&buf_pool->LRU_list_mutex));
#endif
	ut_ad(bpage->in_flush_list);

	buf_flush_list_mutex_enter(buf_pool);

	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_ZIP_PAGE:
		/* Clean compressed pages should not be on the flush list */
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		return;
	case BUF_BLOCK_ZIP_DIRTY:
		buf_page_set_state(bpage, BUF_BLOCK_ZIP_PAGE);
		UT_LIST_REMOVE(list, buf_pool->flush_list, bpage);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		buf_LRU_insert_zip_clean(bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		break;
	case BUF_BLOCK_FILE_PAGE:
		UT_LIST_REMOVE(list, buf_pool->flush_list, bpage);
		break;
	}

	/* If the flush_rbt is active then delete from there as well. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		buf_flush_delete_from_flush_rbt(bpage);
	}

	/* Must be done after we have removed it from the flush_rbt
	because we assert on in_flush_list in comparison function. */
	ut_d(bpage->in_flush_list = FALSE);

	zip_size = page_zip_get_size(&bpage->zip);
	buf_pool->stat.flush_list_bytes -= zip_size ? zip_size : UNIV_PAGE_SIZE;

	bpage->oldest_modification = 0;

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_skip(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_update_hp(buf_pool, bpage);
	buf_flush_list_mutex_exit(buf_pool);
}

/*******************************************************************//**
Relocates a buffer control block on the flush_list.
Note that it is assumed that the contents of bpage have already been
copied to dpage.
IMPORTANT: When this function is called bpage and dpage are not
exact copies of each other. For example, they both will have different
::state. Also the ::list pointers in dpage may be stale. We need to
use the current list node (bpage) to do the list manipulation because
the list pointers could have changed between the time that we copied
the contents of bpage to the dpage and the flush list manipulation
below. */
UNIV_INTERN
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage)	/*!< in/out: destination block */
{
	buf_page_t*	prev;
	buf_page_t*	prev_b = NULL;
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	/* Must reside in the same buffer pool. */
	ut_ad(buf_pool == buf_pool_from_bpage(dpage));

	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	buf_flush_list_mutex_enter(buf_pool);

	ut_ad(bpage->in_flush_list);
	ut_ad(dpage->in_flush_list);

	/* If recovery is active we must swap the control blocks in
	the flush_rbt as well. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		buf_flush_delete_from_flush_rbt(bpage);
		prev_b = buf_flush_insert_in_flush_rbt(dpage);
	}

	/* Must be done after we have removed it from the flush_rbt
	because we assert on in_flush_list in comparison function. */
	ut_d(bpage->in_flush_list = FALSE);

	prev = UT_LIST_GET_PREV(list, bpage);
	UT_LIST_REMOVE(list, buf_pool->flush_list, bpage);

	if (prev) {
		ut_ad(prev->in_flush_list);
		UT_LIST_INSERT_AFTER(
			list,
			buf_pool->flush_list,
			prev, dpage);
	} else {
		UT_LIST_ADD_FIRST(
			list,
			buf_pool->flush_list,
			dpage);
	}

	/* Just an extra check. Previous in flush_list
	should be the same control block as in flush_rbt. */
	ut_a(!buf_pool->flush_rbt || prev_b == prev);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	ut_a(buf_flush_validate_low(buf_pool));
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	buf_flush_update_hp(buf_pool, bpage);
	buf_flush_list_mutex_exit(buf_pool);
}

/********************************************************************//**
Updates the flush system data structures when a write is completed. */
UNIV_INTERN
void
buf_flush_write_complete(
/*=====================*/
	buf_page_t*	bpage)	/*!< in: pointer to the block in question */
{
	buf_flush_t	flush_type = buf_page_get_flush_type(bpage);
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);

	mutex_enter(&buf_pool->flush_state_mutex);

	buf_flush_remove(bpage);

	buf_page_set_io_fix(bpage, BUF_IO_NONE);

	buf_pool->n_flush[flush_type]--;

#ifdef UNIV_MTFLUSH_DEBUG
	fprintf(stderr, "n pending flush %lu\n",
		buf_pool->n_flush[flush_type]);
#endif

	if (buf_pool->n_flush[flush_type] == 0
	    && buf_pool->init_flush[flush_type] == FALSE) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}

	buf_dblwr_update(bpage, flush_type);

	mutex_exit(&buf_pool->flush_state_mutex);
}
#endif /* !UNIV_HOTBACKUP */

/********************************************************************//**
Calculate the checksum of a page from compressed table and update the page. */
UNIV_INTERN
void
buf_flush_update_zip_checksum(
/*==========================*/
	buf_frame_t*	page,		/*!< in/out: Page to update */
	ulint		zip_size,	/*!< in: Compressed page size */
	lsn_t		lsn)		/*!< in: Lsn to stamp on the page */
{
	ut_a(zip_size > 0);

	ib_uint32_t	checksum = static_cast<ib_uint32_t>(
		page_zip_calc_checksum(
			page, zip_size,
			static_cast<srv_checksum_algorithm_t>(
				srv_checksum_algorithm)));

	mach_write_to_8(page + FIL_PAGE_LSN, lsn);
	memset(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
	mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
}

/********************************************************************//**
Initializes a page for writing to the tablespace. */
UNIV_INTERN
void
buf_flush_init_for_writing(
/*=======================*/
	byte*	page,		/*!< in/out: page */
	void*	page_zip_,	/*!< in/out: compressed page, or NULL */
	lsn_t	newest_lsn)	/*!< in: newest modification lsn
				to the page */
{
	ib_uint32_t	checksum = 0 /* silence bogus gcc warning */;

	ut_ad(page);

	if (page_zip_) {
		page_zip_des_t*	page_zip;
		ulint		zip_size;

		page_zip = static_cast<page_zip_des_t*>(page_zip_);
		zip_size = page_zip_get_size(page_zip);

		ut_ad(zip_size);
		ut_ad(ut_is_2pow(zip_size));
		ut_ad(zip_size <= UNIV_ZIP_SIZE_MAX);

		switch (UNIV_EXPECT(fil_page_get_type(page), FIL_PAGE_INDEX)) {
		case FIL_PAGE_TYPE_ALLOCATED:
		case FIL_PAGE_INODE:
		case FIL_PAGE_IBUF_BITMAP:
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			/* These are essentially uncompressed pages. */
			memcpy(page_zip->data, page, zip_size);
			/* fall through */
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
		case FIL_PAGE_INDEX:

			buf_flush_update_zip_checksum(
				page_zip->data, zip_size, newest_lsn);

			return;
		}

		ut_print_timestamp(stderr);
		fputs("  InnoDB: ERROR: The compressed page to be written"
		      " seems corrupt:", stderr);
		ut_print_buf(stderr, page, zip_size);
		fputs("\nInnoDB: Possibly older version of the page:", stderr);
		ut_print_buf(stderr, page_zip->data, zip_size);
		putc('\n', stderr);
		ut_error;
	}

	/* Write the newest modification lsn to the page header and trailer */
	mach_write_to_8(page + FIL_PAGE_LSN, newest_lsn);

	mach_write_to_8(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
			newest_lsn);

	/* Store the new formula checksum */

	switch ((srv_checksum_algorithm_t) srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		checksum = buf_calc_page_crc32(page);
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
		break;
	case SRV_CHECKSUM_ALGORITHM_INNODB:
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		checksum = (ib_uint32_t) buf_calc_page_new_checksum(page);
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
		checksum = (ib_uint32_t) buf_calc_page_old_checksum(page);
		break;
	case SRV_CHECKSUM_ALGORITHM_NONE:
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		checksum = BUF_NO_CHECKSUM_MAGIC;
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
		break;
	/* no default so the compiler will emit a warning if new enum
	is added and not handled here */
	}

	/* With the InnoDB checksum, we overwrite the first 4 bytes of
	the end lsn field to store the old formula checksum. Since it
	depends also on the field FIL_PAGE_SPACE_OR_CHKSUM, it has to
	be calculated after storing the new formula checksum.

	In other cases we write the same value to both fields.
	If CRC32 is used then it is faster to use that checksum
	(calculated above) instead of calculating another one.
	We can afford to store something other than
	buf_calc_page_old_checksum() or BUF_NO_CHECKSUM_MAGIC in
	this field because the file will not be readable by old
	versions of MySQL/InnoDB anyway (older than MySQL 5.6.3) */

	mach_write_to_4(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
			checksum);
}

#ifndef UNIV_HOTBACKUP
/********************************************************************//**
Does an asynchronous write of a buffer page. NOTE: in simulated aio and
also when the doublewrite buffer is used, we must call
buf_dblwr_flush_buffered_writes after we have posted a batch of
writes! */
static
void
buf_flush_write_block_low(
/*======================*/
	buf_page_t*	bpage,		/*!< in: buffer block to write */
	buf_flush_t	flush_type,	/*!< in: type of flush */
	bool		sync)		/*!< in: true if sync IO request */
{
	ulint	zip_size	= buf_page_get_zip_size(bpage);
	page_t*	frame		= NULL;
	ulint space_id          = buf_page_get_space(bpage);
	atomic_writes_t awrites = fil_space_get_atomic_writes(space_id);

#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
#endif

#ifdef UNIV_LOG_DEBUG
	static ibool	univ_log_debug_warned;
#endif /* UNIV_LOG_DEBUG */

	ut_ad(buf_page_in_file(bpage));

	/* We are not holding block_mutex here.
	Nevertheless, it is safe to access bpage, because it is
	io_fixed and oldest_modification != 0.  Thus, it cannot be
	relocated in the buffer pool or removed from flush_list or
	LRU_list. */
	ut_ad(!buf_flush_list_mutex_own(buf_pool));
	ut_ad(!mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(buf_page_get_io_fix_unlocked(bpage) == BUF_IO_WRITE);
	ut_ad(bpage->oldest_modification != 0);

#ifdef UNIV_IBUF_COUNT_DEBUG
	ut_a(ibuf_count_get(bpage->space, bpage->offset) == 0);
#endif
	ut_ad(bpage->newest_modification != 0);

#ifdef UNIV_LOG_DEBUG
	if (!univ_log_debug_warned) {
		univ_log_debug_warned = TRUE;
		fputs("Warning: cannot force log to disk if"
		      " UNIV_LOG_DEBUG is defined!\n"
		      "Crash recovery will not work!\n",
		      stderr);
	}
#else
	/* Force the log to the disk before writing the modified block */
	log_write_up_to(bpage->newest_modification, LOG_WAIT_ALL_GROUPS, TRUE);
#endif
	switch (buf_page_get_state(bpage)) {
	case BUF_BLOCK_POOL_WATCH:
	case BUF_BLOCK_ZIP_PAGE: /* The page should be dirty. */
	case BUF_BLOCK_NOT_USED:
	case BUF_BLOCK_READY_FOR_USE:
	case BUF_BLOCK_MEMORY:
	case BUF_BLOCK_REMOVE_HASH:
		ut_error;
		break;
	case BUF_BLOCK_ZIP_DIRTY:
		frame = bpage->zip.data;
		mach_write_to_8(frame + FIL_PAGE_LSN,
				bpage->newest_modification);

		ut_a(page_zip_verify_checksum(frame, zip_size));

		memset(frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
		break;
	case BUF_BLOCK_FILE_PAGE:
		frame = bpage->zip.data;
		if (!frame) {
			frame = ((buf_block_t*) bpage)->frame;
		}

		buf_flush_init_for_writing(((buf_block_t*) bpage)->frame,
					   bpage->zip.data
					   ? &bpage->zip : NULL,
					   bpage->newest_modification);
		break;
	}

	frame = buf_page_encrypt_before_write(bpage, frame, space_id);

	if (!srv_use_doublewrite_buf || !buf_dblwr) {
		fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER,
			sync,
			buf_page_get_space(bpage),
			zip_size,
			buf_page_get_page_no(bpage),
			0,
			zip_size ? zip_size : bpage->real_size,
			frame,
			bpage,
			&bpage->write_size);
	} else {
		/* InnoDB uses doublewrite buffer and doublewrite buffer
		is initialized. User can define do we use atomic writes
		on a file space (table) or not. If atomic writes are
		not used we should use doublewrite buffer and if
		atomic writes should be used, no doublewrite buffer
		is used. */

		if (awrites == ATOMIC_WRITES_ON) {
			fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER,
				FALSE,
				buf_page_get_space(bpage),
				zip_size,
				buf_page_get_page_no(bpage),
				0,
				zip_size ? zip_size : bpage->real_size,
				frame,
				bpage,
				&bpage->write_size);
		} else if (flush_type == BUF_FLUSH_SINGLE_PAGE) {
			buf_dblwr_write_single_page(bpage, sync);
		} else {
			buf_dblwr_add_to_batch(bpage);
		}
	}

	/* When doing single page flushing the IO is done synchronously
	and we flush the changes to disk only for the tablespace we
	are working on. */
	if (sync) {
		ut_ad(flush_type == BUF_FLUSH_SINGLE_PAGE);
		fil_flush(buf_page_get_space(bpage));
		buf_page_io_complete(bpage);
	}

	/* Increment the counter of I/O operations used
	for selecting LRU policy. */
	buf_LRU_stat_inc_io();
}

/********************************************************************//**
Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: in simulated aio we must call
os_aio_simulated_wake_handler_threads after we have posted a batch of
writes! NOTE: buf_page_get_mutex(bpage) must be held upon entering this
function, and it will be released by this function if it returns true.
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
	bool		sync)		/*!< in: true if sync IO request */
{
	ut_ad(flush_type < BUF_FLUSH_N_TYPES);
	/* Hold the LRU list mutex iff called for a single page LRU
	flush. A single page LRU flush is already non-performant, and holding
	the LRU list mutex allows us to avoid having to store the previous LRU
	list page or to restart the LRU scan in
	buf_flush_single_page_from_LRU(). */
	ut_ad(flush_type == BUF_FLUSH_SINGLE_PAGE ||
	      !mutex_own(&buf_pool->LRU_list_mutex));
	ut_ad(flush_type != BUF_FLUSH_SINGLE_PAGE ||
	      mutex_own(&buf_pool->LRU_list_mutex));
	ut_ad(buf_page_in_file(bpage));
	ut_ad(!sync || flush_type == BUF_FLUSH_SINGLE_PAGE);

	ib_mutex_t*	block_mutex = buf_page_get_mutex(bpage);

	ut_ad(mutex_own(block_mutex));

	ut_ad(buf_flush_ready_for_flush(bpage, flush_type));

        bool            is_uncompressed;

        is_uncompressed = (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
        ut_ad(is_uncompressed == (block_mutex != &buf_pool->zip_mutex));

        ibool           flush;
        rw_lock_t*	rw_lock;
        bool            no_fix_count = bpage->buf_fix_count == 0;

        if (!is_uncompressed) {
                flush = TRUE;
		rw_lock = NULL;

	} else if (!(no_fix_count || flush_type == BUF_FLUSH_LIST)) {
		/* This is a heuristic, to avoid expensive S attempts. */
		flush = FALSE;
	} else {

		rw_lock = &reinterpret_cast<buf_block_t*>(bpage)->lock;

		if (flush_type != BUF_FLUSH_LIST) {
			flush = rw_lock_s_lock_gen_nowait(
				rw_lock, BUF_IO_WRITE);
		} else {
			/* Will S lock later */
			flush = TRUE;
		}
	}

        if (flush) {

		/* We are committed to flushing by the time we get here */

		mutex_enter(&buf_pool->flush_state_mutex);

		buf_page_set_io_fix(bpage, BUF_IO_WRITE);

		buf_page_set_flush_type(bpage, flush_type);

		if (buf_pool->n_flush[flush_type] == 0) {

			os_event_reset(buf_pool->no_flush[flush_type]);
		}

		++buf_pool->n_flush[flush_type];

		mutex_exit(&buf_pool->flush_state_mutex);

		mutex_exit(block_mutex);

		if (flush_type == BUF_FLUSH_SINGLE_PAGE)
			mutex_exit(&buf_pool->LRU_list_mutex);

		if (flush_type == BUF_FLUSH_LIST
		    && is_uncompressed
		    && !rw_lock_s_lock_gen_nowait(rw_lock, BUF_IO_WRITE)) {
			/* avoiding deadlock possibility involves doublewrite
			buffer, should flush it, because it might hold the
			another block->lock. */
			buf_dblwr_flush_buffered_writes();

			rw_lock_s_lock_gen(rw_lock, BUF_IO_WRITE);
                }

                /* Even though bpage is not protected by any mutex at this
                point, it is safe to access bpage, because it is io_fixed and
                oldest_modification != 0.  Thus, it cannot be relocated in the
                buffer pool or removed from flush_list or LRU_list. */

                buf_flush_write_block_low(bpage, flush_type, sync);
        }

	return(flush);
}

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
{
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(mutex_own(&block->mutex));
	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	if (!buf_flush_ready_for_flush(&block->page, BUF_FLUSH_SINGLE_PAGE)) {
		return(FALSE);
	}

	/* The following call will release the LRU list and
	block mutex if successful. */
	return(buf_flush_page(
			buf_pool, &block->page, BUF_FLUSH_SINGLE_PAGE, true));
}
# endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
/***********************************************************//**
Check the page is in buffer pool and can be flushed.
@return	true if the page can be flushed. */
static
bool
buf_flush_check_neighbor(
/*=====================*/
	ulint		space,		/*!< in: space id */
	ulint		offset,		/*!< in: page offset */
	buf_flush_t	flush_type)	/*!< in: BUF_FLUSH_LRU or
					BUF_FLUSH_LIST */
{
	buf_page_t*	bpage;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);
	bool		ret;
	prio_rw_lock_t*	hash_lock;
	ib_mutex_t*	block_mutex;

	ut_ad(flush_type == BUF_FLUSH_LRU
	      || flush_type == BUF_FLUSH_LIST);

	/* We only want to flush pages from this buffer pool. */
	bpage = buf_page_hash_get_s_locked(buf_pool, space, offset,
					   &hash_lock);

	if (!bpage) {

		return(false);
	}

	block_mutex = buf_page_get_mutex(bpage);

	mutex_enter(block_mutex);

	rw_lock_s_unlock(hash_lock);

	ut_a(buf_page_in_file(bpage));

	/* We avoid flushing 'non-old' blocks in an LRU flush,
	because the flushed blocks are soon freed */

	ret = false;
	if (flush_type != BUF_FLUSH_LRU || buf_page_is_old(bpage)) {

		if (buf_flush_ready_for_flush(bpage, flush_type)) {
			ret = true;
		}
	}

	mutex_exit(block_mutex);

	return(ret);
}

/***********************************************************//**
Flushes to disk all flushable pages within the flush area.
@return	number of pages flushed */
static
ulint
buf_flush_try_neighbors(
/*====================*/
	ulint		space,		/*!< in: space id */
	ulint		offset,		/*!< in: page offset */
	buf_flush_t	flush_type,	/*!< in: BUF_FLUSH_LRU or
					BUF_FLUSH_LIST */
	ulint		n_flushed,	/*!< in: number of pages
					flushed so far in this batch */
	ulint		n_to_flush)	/*!< in: maximum number of pages
					we are allowed to flush */
{
	ulint		i;
	ulint		low;
	ulint		high;
	buf_pool_t*	buf_pool = buf_pool_get(space, offset);

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	ut_ad(!buf_flush_list_mutex_own(buf_pool));

	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN
	    || srv_flush_neighbors == 0) {
		/* If there is little space or neighbor flushing is
		not enabled then just flush the victim. */
		low = offset;
		high = offset + 1;
	} else {
		/* When flushed, dirty blocks are searched in
		neighborhoods of this size, and flushed along with the
		original page. */

		ulint	buf_flush_area;

		buf_flush_area	= ut_min(
			BUF_READ_AHEAD_AREA(buf_pool),
			buf_pool->curr_size / 16);

		low = (offset / buf_flush_area) * buf_flush_area;
		high = (offset / buf_flush_area + 1) * buf_flush_area;

		if (srv_flush_neighbors == 1) {
			/* adjust 'low' and 'high' to limit
			   for contiguous dirty area */
			if (offset > low) {
				for (i = offset - 1;
				     i >= low
				     && buf_flush_check_neighbor(
						space, i, flush_type);
				     i--) {
					/* do nothing */
				}
				low = i + 1;
			}

			for (i = offset + 1;
			     i < high
			     && buf_flush_check_neighbor(
						space, i, flush_type);
			     i++) {
				/* do nothing */
			}
			high = i;
		}
	}

	/* fprintf(stderr, "Flush area: low %lu high %lu\n", low, high); */

	if (high > fil_space_get_size(space)) {
		high = fil_space_get_size(space);
	}

	ulint	count = 0;

	for (i = low; i < high; i++) {

		prio_rw_lock_t*	hash_lock;
		ib_mutex_t*	block_mutex;

		if ((count + n_flushed) >= n_to_flush) {

			/* We have already flushed enough pages and
			should call it a day. There is, however, one
			exception. If the page whose neighbors we
			are flushing has not been flushed yet then
			we'll try to flush the victim that we
			selected originally. */
			if (i <= offset) {
				i = offset;
			} else {
				break;
			}
		}

		buf_pool = buf_pool_get(space, i);

		/* We only want to flush pages from this buffer pool. */
		buf_page_t*	bpage = buf_page_hash_get_s_locked(buf_pool,
						   space, i, &hash_lock);

		if (bpage == NULL) {

			continue;
		}

		block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		rw_lock_s_unlock(hash_lock);

		ut_a(buf_page_in_file(bpage));

		/* We avoid flushing 'non-old' blocks in an LRU flush,
		because the flushed blocks are soon freed */

		if (flush_type != BUF_FLUSH_LRU
		    || i == offset
		    || buf_page_is_old(bpage)) {

			if (buf_flush_ready_for_flush(bpage, flush_type)
			    && (i == offset || bpage->buf_fix_count == 0)
			    && buf_flush_page(
					buf_pool, bpage, flush_type, false)) {

				++count;

				continue;
			}
		}

		mutex_exit(block_mutex);
	}

	if (count > 0) {
		MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_NEIGHBOR_TOTAL_PAGE,
					MONITOR_FLUSH_NEIGHBOR_COUNT,
					MONITOR_FLUSH_NEIGHBOR_PAGES,
					(count - 1));
	}

	return(count);
}

/********************************************************************//**
Check if the block is modified and ready for flushing. If the the block
is ready to flush then flush the page and try o flush its neighbors.

@return	TRUE if, depending on the flush type, either LRU or flush list
mutex was released during this function.  This does not guarantee that some
pages were written as well.
Number of pages written are incremented to the count. */
static
ibool
buf_flush_page_and_try_neighbors(
/*=============================*/
	buf_page_t*	bpage,		/*!< in: buffer control block,
					must be
					buf_page_in_file(bpage) */
	buf_flush_t	flush_type,	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
	ulint		n_to_flush,	/*!< in: number of pages to
					flush */
	ulint*		count)		/*!< in/out: number of pages
					flushed */
{
	ibool		flushed;
	ib_mutex_t*	block_mutex = NULL;
#ifdef UNIV_DEBUG
	buf_pool_t*	buf_pool = buf_pool_from_bpage(bpage);
#endif /* UNIV_DEBUG */

	ut_ad((flush_type == BUF_FLUSH_LRU
	       && mutex_own(&buf_pool->LRU_list_mutex))
	      || (flush_type == BUF_FLUSH_LIST
		  && buf_flush_list_mutex_own(buf_pool)));

	if (flush_type == BUF_FLUSH_LRU) {
		block_mutex = buf_page_get_mutex(bpage);
		mutex_enter(block_mutex);
	}

	ut_a(buf_page_in_file(bpage)
	     || (buf_page_get_state(bpage) == BUF_BLOCK_REMOVE_HASH
		 ));

	if (buf_flush_ready_for_flush(bpage, flush_type)) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_bpage(bpage);

		if (flush_type == BUF_FLUSH_LRU) {
			mutex_exit(&buf_pool->LRU_list_mutex);
		}

		/* These fields are protected by the buf_page_get_mutex()
		mutex. */
		/* Read the fields directly in order to avoid asserting on
		BUF_BLOCK_REMOVE_HASH pages. */
		ulint	space = bpage->space;
		ulint	offset = bpage->offset;

		if (flush_type == BUF_FLUSH_LRU) {
			mutex_exit(block_mutex);
		} else {
			buf_flush_list_mutex_exit(buf_pool);
		}

		/* Try to flush also all the neighbors */
		*count += buf_flush_try_neighbors(
			space, offset, flush_type, *count, n_to_flush);

		if (flush_type == BUF_FLUSH_LRU) {
			mutex_enter(&buf_pool->LRU_list_mutex);
		} else {
			buf_flush_list_mutex_enter(buf_pool);
		}
		flushed = TRUE;

	} else if (flush_type == BUF_FLUSH_LRU) {
		mutex_exit(block_mutex);
		flushed = FALSE;
	} else {
		flushed = FALSE;
	}

	ut_ad((flush_type == BUF_FLUSH_LRU
	       && mutex_own(&buf_pool->LRU_list_mutex))
	      || (flush_type == BUF_FLUSH_LIST
		  && buf_flush_list_mutex_own(buf_pool)));

	return(flushed);
}

/*******************************************************************//**
This utility moves the uncompressed frames of pages to the free list.
Note that this function does not actually flush any data to disk. It
just detaches the uncompressed frames from the compressed pages at the
tail of the unzip_LRU and puts those freed frames in the free list.
Note that it is a best effort attempt and it is not guaranteed that
after a call to this function there will be 'max' blocks in the free
list.
@return number of blocks moved to the free list. */
static
ulint
buf_free_from_unzip_LRU_list_batch(
/*===============================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		max)		/*!< in: desired number of
					blocks in the free_list */
{
	buf_block_t*	block;
	ulint		scanned = 0;
	ulint		count = 0;
	ulint		free_len = UT_LIST_GET_LEN(buf_pool->free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);

	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);
	while (block != NULL && count < max
	       && free_len < srv_LRU_scan_depth
	       && lru_len > UT_LIST_GET_LEN(buf_pool->LRU) / 10) {

		ib_mutex_t*	block_mutex = buf_page_get_mutex(&block->page);

		++scanned;

		mutex_enter(block_mutex);

		if (buf_LRU_free_page(&block->page, false)) {

			mutex_exit(block_mutex);
			/* Block was freed. LRU list mutex potentially
			released and reacquired */
			++count;
			mutex_enter(&buf_pool->LRU_list_mutex);
			block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);

		} else {

			mutex_exit(block_mutex);
			block = UT_LIST_GET_PREV(unzip_LRU, block);
		}

		free_len = UT_LIST_GET_LEN(buf_pool->free);
		lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);
	}

	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	return(count);
}

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list.
The calling thread is not allowed to own any latches on pages!
It attempts to make 'max' blocks available in the free list. Note that
it is a best effort attempt and it is not guaranteed that after a call
to this function there will be 'max' blocks in the free list.
@return number of blocks for which the write request was queued. */
MY_ATTRIBUTE((nonnull))
static
void
buf_flush_LRU_list_batch(
/*=====================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		max,		/*!< in: desired number of
					blocks in the free_list */
	bool		limited_scan,	/*!< in: if true, allow to scan only up
					to srv_LRU_scan_depth pages in total */
	flush_counters_t*	n)	/*!< out: flushed/evicted page
					counts */
{
	buf_page_t*	bpage;
	ulint		scanned = 0;
	ulint		lru_position = 0;
	ulint		max_lru_position;
	ulint		max_scanned_pages;
	ulint		free_len = UT_LIST_GET_LEN(buf_pool->free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool->LRU);

	n->flushed = 0;
	n->evicted = 0;
	n->unzip_LRU_evicted = 0;

	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	max_scanned_pages = limited_scan ? srv_LRU_scan_depth : lru_len * max;
	max_lru_position = ut_min(srv_LRU_scan_depth, lru_len);

	bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	while (bpage != NULL
	       && (srv_cleaner_eviction_factor ? n->evicted : n->flushed) < max
	       && free_len < srv_LRU_scan_depth
	       && lru_len > BUF_LRU_MIN_LEN
	       && lru_position < max_lru_position
	       && scanned < max_scanned_pages) {

		ib_mutex_t* block_mutex = buf_page_get_mutex(bpage);
		ibool	 evict;
		ulint	failed_acquire;

		++scanned;
		++lru_position;

		failed_acquire = mutex_enter_nowait(block_mutex);

		evict = UNIV_LIKELY(!failed_acquire)
			&& buf_flush_ready_for_replace(bpage);

		if (UNIV_LIKELY(!failed_acquire) && !evict) {

			mutex_exit(block_mutex);
		}

		/* If the block is ready to be replaced we try to
		free it i.e.: put it on the free list.
		Otherwise we try to flush the block and its
		neighbors. In this case we'll put it on the
		free list in the next pass. We do this extra work
		of putting blocks to the free list instead of
		just flushing them because after every flush
		we have to restart the scan from the tail of
		the LRU list and if we don't clear the tail
		of the flushed pages then the scan becomes
		O(n*n). */
		if (evict) {

			if (buf_LRU_free_page(bpage, true)) {

				mutex_exit(block_mutex);
				n->evicted++;
				lru_position = 0;
				mutex_enter(&buf_pool->LRU_list_mutex);
				bpage = UT_LIST_GET_LAST(buf_pool->LRU);
			} else {

				bpage = UT_LIST_GET_PREV(LRU, bpage);
				mutex_exit(block_mutex);
			}
		} else if (UNIV_LIKELY(!failed_acquire)) {

			ulint		space;
			ulint		offset;
			buf_page_t*	prev_bpage;

			prev_bpage = UT_LIST_GET_PREV(LRU, bpage);

			/* Save the previous bpage */

			if (prev_bpage != NULL) {
				space = prev_bpage->space;
				offset = prev_bpage->offset;
			} else {
				space = ULINT_UNDEFINED;
				offset = ULINT_UNDEFINED;
			}

			if (buf_flush_page_and_try_neighbors(
				bpage,
				BUF_FLUSH_LRU, max, &n->flushed)) {

				/* LRU list mutex was released.
				reposition the iterator. Note: the
				prev block could have been repositioned
				too but that should be rare. */

				if (prev_bpage != NULL) {

					ut_ad(space != ULINT_UNDEFINED);
					ut_ad(offset != ULINT_UNDEFINED);

					prev_bpage = buf_page_hash_get(
						buf_pool, space, offset);
				}
			}

			bpage = prev_bpage;
		}

		free_len = UT_LIST_GET_LEN(buf_pool->free);
		lru_len = UT_LIST_GET_LEN(buf_pool->LRU);
	}

	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	/* We keep track of all flushes happening as part of LRU
	flush. When estimating the desired rate at which flush_list
	should be flushed, we factor in this value. */
	buf_pool->stat.buf_lru_flush_page_count += n->flushed;

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}
}

/*******************************************************************//**
Flush and move pages from LRU or unzip_LRU list to the free list.
Whether LRU or unzip_LRU is used depends on the state of the system.
@return number of blocks for which either the write request was queued
or in case of unzip_LRU the number of blocks actually moved to the
free list */
MY_ATTRIBUTE((nonnull))
static
void
buf_do_LRU_batch(
/*=============*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		max,		/*!< in: desired number of
					blocks in the free_list */
	bool		limited_scan,	/*!< in: if true, allow to scan only up
					to srv_LRU_scan_depth pages in total */
	flush_counters_t*	n)	/*!< out: flushed/evicted page
					counts */
{
	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));

	if (buf_LRU_evict_from_unzip_LRU(buf_pool)) {
		n->unzip_LRU_evicted
			= buf_free_from_unzip_LRU_list_batch(buf_pool, max);
	} else {
		n->unzip_LRU_evicted = 0;
	}

	if (max > n->unzip_LRU_evicted) {
		buf_flush_LRU_list_batch(buf_pool, max - n->unzip_LRU_evicted,
					 limited_scan, n);
	} else {
		n->evicted = 0;
		n->flushed = 0;
	}

	n->evicted += n->unzip_LRU_evicted;
}

/*******************************************************************//**
This utility flushes dirty blocks from the end of the flush_list.
the calling thread is not allowed to own any latches on pages!
@return number of blocks for which the write request was queued;
ULINT_UNDEFINED if there was a flush of the same type already
running */
static
ulint
buf_do_flush_list_batch(
/*====================*/
	buf_pool_t*	buf_pool,	/*!< in: buffer pool instance */
	ulint		min_n,		/*!< in: wished minimum mumber
					of blocks flushed (it is not
					guaranteed that the actual
					number is that big, though) */
	lsn_t		lsn_limit)	/*!< all blocks whose
					oldest_modification is smaller
					than this should be flushed (if
					their number does not exceed
					min_n) */
{
	ulint		count = 0;
	ulint		scanned = 0;

	/* Start from the end of the list looking for a suitable
	block to be flushed. */
	buf_flush_list_mutex_enter(buf_pool);
	ulint len = UT_LIST_GET_LEN(buf_pool->flush_list);

	/* In order not to degenerate this scan to O(n*n) we attempt
	to preserve pointer of previous block in the flush list. To do
	so we declare it a hazard pointer. Any thread working on the
	flush list must check the hazard pointer and if it is removing
	the same block then it must reset it. */
	for (buf_page_t* bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
	     count < min_n && bpage != NULL && len > 0
	     && bpage->oldest_modification < lsn_limit;
	     ++scanned) {

		buf_page_t*	prev;

		ut_a(bpage->oldest_modification > 0);
		ut_ad(bpage->in_flush_list);

		prev = UT_LIST_GET_PREV(list, bpage);
		buf_flush_set_hp(buf_pool, prev);

#ifdef UNIV_DEBUG
		bool flushed =
#endif /* UNIV_DEBUG */
		buf_flush_page_and_try_neighbors(
			bpage, BUF_FLUSH_LIST, min_n, &count);

		ut_ad(flushed || buf_flush_is_hp(buf_pool, prev));

		if (!buf_flush_is_hp(buf_pool, prev)) {
			/* The hazard pointer was reset by some other
			thread. Restart the scan. */
			ut_ad(buf_flush_is_hp(buf_pool, NULL));
			bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
			len = UT_LIST_GET_LEN(buf_pool->flush_list);
		} else {
			bpage = prev;
			--len;
			buf_flush_set_hp(buf_pool, NULL);
		}

		ut_ad(!bpage || bpage->in_flush_list);
	}

	buf_flush_list_mutex_exit(buf_pool);

	MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_SCANNED,
				     MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
				     MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
				     scanned);

	return(count);
}

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list or flush_list.
NOTE 1: in the case of an LRU flush the calling thread may own latches to
pages: to avoid deadlocks, this function must be written so that it cannot
end up waiting for these latches! NOTE 2: in the case of a flush list flush,
the calling thread is not allowed to own any latches on pages!
@return number of blocks for which the write request was queued */
MY_ATTRIBUTE((nonnull))
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
	flush_counters_t*	n)	/*!< out: flushed/evicted page
					counts  */
{
	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
#ifdef UNIV_SYNC_DEBUG
	ut_ad((flush_type != BUF_FLUSH_LIST)
	      || sync_thread_levels_empty_except_dict());
#endif /* UNIV_SYNC_DEBUG */

	/* Note: The buffer pool mutexes are released and reacquired within
	the flush functions. */
	switch (flush_type) {
	case BUF_FLUSH_LRU:
		mutex_enter(&buf_pool->LRU_list_mutex);
		buf_do_LRU_batch(buf_pool, min_n, limited_lru_scan, n);
		mutex_exit(&buf_pool->LRU_list_mutex);
		break;
	case BUF_FLUSH_LIST:
		ut_ad(!limited_lru_scan);
		n->flushed = buf_do_flush_list_batch(buf_pool, min_n,
						     lsn_limit);
		n->evicted = 0;
		break;
	default:
		ut_error;
	}

#ifdef UNIV_DEBUG
	if (buf_debug_prints && n->flushed > 0) {
		fprintf(stderr, flush_type == BUF_FLUSH_LRU
			? "Flushed %lu pages in LRU flush\n"
			: "Flushed %lu pages in flush list flush\n",
			(ulong) n->flushed);
	}
#endif /* UNIV_DEBUG */
}

/******************************************************************//**
Gather the aggregated stats for both flush list and LRU list flushing */
void
buf_flush_common(
/*=============*/
	buf_flush_t	flush_type,	/*!< in: type of flush */
	ulint		page_count)	/*!< in: number of pages flushed */
{
	if (page_count) {
		buf_dblwr_flush_buffered_writes();
	}

	ut_a(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

#ifdef UNIV_DEBUG
	if (buf_debug_prints && page_count > 0) {
		fprintf(stderr, flush_type == BUF_FLUSH_LRU
			? "Flushed %lu pages in LRU flush\n"
			: "Flushed %lu pages in flush list flush\n",
			(ulong) page_count);
	}
#endif /* UNIV_DEBUG */

	srv_stats.buf_pool_flushed.add(page_count);
}

/******************************************************************//**
Start a buffer flush batch for LRU or flush list */
ibool
buf_flush_start(
/*============*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	flush_type)	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	mutex_enter(&buf_pool->flush_state_mutex);

	if (buf_pool->n_flush[flush_type] > 0
	    || buf_pool->init_flush[flush_type] == TRUE) {

		/* There is already a flush batch of the same type running */

#ifdef UNIV_PAGECOMPRESS_DEBUG
		fprintf(stderr, "Error: flush_type %d n_flush %lu init_flush %lu\n",
			flush_type, buf_pool->n_flush[flush_type], buf_pool->init_flush[flush_type]);
#endif

		mutex_exit(&buf_pool->flush_state_mutex);

		return(FALSE);
	}

	buf_pool->init_flush[flush_type] = TRUE;

	mutex_exit(&buf_pool->flush_state_mutex);

	return(TRUE);
}

/******************************************************************//**
End a buffer flush batch for LRU or flush list */
void
buf_flush_end(
/*==========*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	flush_type)	/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	mutex_enter(&buf_pool->flush_state_mutex);

	buf_pool->init_flush[flush_type] = FALSE;

	buf_pool->try_LRU_scan = TRUE;

	if (buf_pool->n_flush[flush_type] == 0) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}

	mutex_exit(&buf_pool->flush_state_mutex);
}

/******************************************************************//**
Waits until a flush batch of the given type ends */
UNIV_INTERN
void
buf_flush_wait_batch_end(
/*=====================*/
	buf_pool_t*	buf_pool,	/*!< buffer pool instance */
	buf_flush_t	type)		/*!< in: BUF_FLUSH_LRU
					or BUF_FLUSH_LIST */
{
	ut_ad(type == BUF_FLUSH_LRU || type == BUF_FLUSH_LIST);

	if (buf_pool == NULL) {
		ulint	i;

		for (i = 0; i < srv_buf_pool_instances; ++i) {
			buf_pool_t*	buf_pool;

			buf_pool = buf_pool_from_array(i);

			thd_wait_begin(NULL, THD_WAIT_DISKIO);
			os_event_wait(buf_pool->no_flush[type]);
			thd_wait_end(NULL);
		}
	} else {
		thd_wait_begin(NULL, THD_WAIT_DISKIO);
		os_event_wait(buf_pool->no_flush[type]);
		thd_wait_end(NULL);
	}
}

/* JAN: TODO: */

void buf_pool_enter_LRU_mutex(
	buf_pool_t*    buf_pool)
{
	ut_ad(!mutex_own(&buf_pool->LRU_list_mutex));
	mutex_enter(&buf_pool->LRU_list_mutex);
}

void buf_pool_exit_LRU_mutex(
	buf_pool_t*    buf_pool)
{
	ut_ad(mutex_own(&buf_pool->LRU_list_mutex));
	mutex_exit(&buf_pool->LRU_list_mutex);
}

/* JAN: TODO: END: */

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list and also
puts replaceable clean pages from the end of the LRU list to the free
list.
NOTE: The calling thread is not allowed to own any latches on pages!
@return true if a batch was queued successfully. false if another batch
of same type was already running. */
MY_ATTRIBUTE((nonnull))
static
bool
buf_flush_LRU(
/*==========*/
	buf_pool_t*	buf_pool,	/*!< in/out: buffer pool instance */
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	bool			limited_scan,	/*!< in: if true, allow to scan
						only up to srv_LRU_scan_depth
						pages in total */
	flush_counters_t	*n)	/*!< out: flushed/evicted page
					counts */
{
	if (!buf_flush_start(buf_pool, BUF_FLUSH_LRU)) {
		n->flushed = 0;
		n->evicted = 0;
		n->unzip_LRU_evicted = 0;
		return(false);
	}

	buf_flush_batch(buf_pool, BUF_FLUSH_LRU, min_n, 0, limited_scan, n);

	buf_flush_end(buf_pool, BUF_FLUSH_LRU);

	buf_flush_common(BUF_FLUSH_LRU, n->flushed);

	return(true);
}

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
	ulint*		n_processed)	/*!< out: the number of pages
					which were processed is passed
					back to caller. Ignored if NULL */

{
	ulint		i;

	ulint		requested_pages[MAX_BUFFER_POOLS];
	bool		active_instance[MAX_BUFFER_POOLS];
	ulint		remaining_instances = srv_buf_pool_instances;
	bool		timeout = false;
	ulint		flush_start_time = 0;

	if (buf_mtflu_init_done()) {
		return(buf_mtflu_flush_list(min_n, lsn_limit, n_processed));
	}

	for (i = 0; i < srv_buf_pool_instances; i++) {
		requested_pages[i] = 0;
		active_instance[i] = true;
	}

	if (n_processed) {
		*n_processed = 0;
	}

	if (min_n != ULINT_MAX) {
		/* Ensure that flushing is spread evenly amongst the
		buffer pool instances. When min_n is ULINT_MAX
		we need to flush everything up to the lsn limit
		so no limit here. */
		min_n = (min_n + srv_buf_pool_instances - 1)
			 / srv_buf_pool_instances;
		if (lsn_limit != LSN_MAX) {
			flush_start_time = ut_time_ms();
		}
	}

	/* Flush to lsn_limit in all buffer pool instances */
	while (remaining_instances && !timeout) {

		ulint flush_common_batch = 0;

		for (i = 0; i < srv_buf_pool_instances; i++) {

			if (flush_start_time
			    && (ut_time_ms() - flush_start_time
				>= srv_cleaner_max_flush_time)) {

				timeout = true;
				break;
			}

			if (active_instance[i]) {

				buf_pool_t*	buf_pool;
				ulint		chunk_size;
				flush_counters_t n;

				chunk_size = ut_min(
					srv_cleaner_flush_chunk_size,
					min_n - requested_pages[i]);

				buf_pool = buf_pool_from_array(i);

				if (!buf_flush_start(buf_pool,
						     BUF_FLUSH_LIST)) {

					continue;
				}

				buf_flush_batch(buf_pool, BUF_FLUSH_LIST,
						chunk_size, lsn_limit, false,
						&n);

				buf_flush_end(buf_pool, BUF_FLUSH_LIST);

				flush_common_batch += n.flushed;

				if (n_processed) {
					*n_processed += n.flushed;
				}

				requested_pages[i] += chunk_size;

				if (requested_pages[i] >= min_n
				    || !n.flushed) {

					active_instance[i] = false;
					remaining_instances--;
				}

				if (n.flushed) {
					MONITOR_INC_VALUE_CUMULATIVE(
						MONITOR_FLUSH_BATCH_TOTAL_PAGE,
						MONITOR_FLUSH_BATCH_COUNT,
						MONITOR_FLUSH_BATCH_PAGES,
						n.flushed);
				}
			}
		}

		buf_flush_common(BUF_FLUSH_LIST, flush_common_batch);
	}

	/* If we haven't flushed all the instances due to timeout or a repeat
	failure to start a flush, return failure */
	for (i = 0; i < srv_buf_pool_instances; i++) {
		if (active_instance[i]) {
			return(false);
		}
	}

	return(true);
}

/******************************************************************//**
This function picks up a single dirty page from the tail of the LRU
list, flushes it, removes it from page_hash and LRU list and puts
it on the free list. It is called from user threads when they are
unable to find a replaceable page at the tail of the LRU list i.e.:
when the background LRU flushing in the page_cleaner thread is not
fast enough to keep pace with the workload.
@return TRUE if success. */
UNIV_INTERN
ibool
buf_flush_single_page_from_LRU(
/*===========================*/
	buf_pool_t*	buf_pool)	/*!< in/out: buffer pool instance */
{
	ulint		scanned;
	buf_page_t*	bpage;
	ibool		flushed = FALSE;

	mutex_enter(&buf_pool->LRU_list_mutex);

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU), scanned = 1;
	     bpage != NULL;
	     bpage = UT_LIST_GET_PREV(LRU, bpage), ++scanned) {

		ib_mutex_t*	block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		if (buf_flush_ready_for_flush(bpage, BUF_FLUSH_SINGLE_PAGE)) {

			/* The following call will release the LRU list
			and block mutex. */

			flushed = buf_flush_page(buf_pool, bpage,
						 BUF_FLUSH_SINGLE_PAGE, true);

			if (flushed) {
				/* buf_flush_page() will release the
				block mutex */
				break;
			}
		}

		mutex_exit(block_mutex);
	}

	if (!flushed)
		mutex_exit(&buf_pool->LRU_list_mutex);

	MONITOR_INC_VALUE_CUMULATIVE(
		MONITOR_LRU_SINGLE_FLUSH_SCANNED,
		MONITOR_LRU_SINGLE_FLUSH_SCANNED_NUM_CALL,
		MONITOR_LRU_SINGLE_FLUSH_SCANNED_PER_CALL,
		scanned);

	if (bpage == NULL) {
		/* Can't find a single flushable page. */
		return(FALSE);
	}


	ibool	freed = FALSE;

	/* At this point the page has been written to the disk.
	As we are not holding LRU list or buf_page_get_mutex() mutex therefore
	we cannot use the bpage safely. It may have been plucked out
	of the LRU list by some other thread or it may even have
	relocated in case of a compressed page. We need to start
	the scan of LRU list again to remove the block from the LRU
	list and put it on the free list. */
	mutex_enter(&buf_pool->LRU_list_mutex);

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     bpage != NULL;
	     bpage = UT_LIST_GET_PREV(LRU, bpage)) {

		ib_mutex_t*	block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		ibool	ready = buf_flush_ready_for_replace(bpage);

		if (ready) {
			bool	evict_zip;

			evict_zip = !buf_LRU_evict_from_unzip_LRU(buf_pool);

			freed = buf_LRU_free_page(bpage, evict_zip);

			mutex_exit(block_mutex);

			break;
		}

		mutex_exit(block_mutex);

	}

	if (!freed)
		mutex_exit(&buf_pool->LRU_list_mutex);

	return(freed);
}

/*********************************************************************//**
Clears up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return number of pages flushed */
UNIV_INTERN
ulint
buf_flush_LRU_tail(void)
/*====================*/
{
	ulint	total_flushed = 0;
	ulint	start_time = ut_time_ms();
	ulint	scan_depth[MAX_BUFFER_POOLS];
	ulint	requested_pages[MAX_BUFFER_POOLS];
	bool	active_instance[MAX_BUFFER_POOLS];
	bool	limited_scan[MAX_BUFFER_POOLS];
	ulint	previous_evicted[MAX_BUFFER_POOLS];
	ulint	remaining_instances = srv_buf_pool_instances;
	ulint	lru_chunk_size = srv_cleaner_lru_chunk_size;
	ulint	free_list_lwm = srv_LRU_scan_depth / 100
		* srv_cleaner_free_list_lwm;

	if(buf_mtflu_init_done())
	{
		return(buf_mtflu_flush_LRU_tail());
	}

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {

		const buf_pool_t* buf_pool = buf_pool_from_array(i);

		scan_depth[i] = ut_min(srv_LRU_scan_depth,
				       UT_LIST_GET_LEN(buf_pool->LRU));
		requested_pages[i] = 0;
		active_instance[i] = true;
		limited_scan[i] = true;
		previous_evicted[i] = 0;
	}

	while (remaining_instances) {

		if (ut_time_ms() - start_time >= srv_cleaner_max_lru_time) {

			break;
		}

		for (ulint i = 0; i < srv_buf_pool_instances; i++) {

			if (!active_instance[i]) {
				continue;
			}

			ulint free_len = free_list_lwm;
			buf_pool_t* buf_pool = buf_pool_from_array(i);

			do {
				flush_counters_t	n;

				ut_ad(requested_pages[i] <= scan_depth[i]);

				/* Currently page_cleaner is the only thread
				that can trigger an LRU flush. It is possible
				that a batch triggered during last iteration is
				still running, */
				if (buf_flush_LRU(buf_pool, lru_chunk_size,
						  limited_scan[i], &n)) {

					/* Allowed only one batch per
					buffer pool instance. */
					buf_flush_wait_batch_end(
						buf_pool, BUF_FLUSH_LRU);
				}

				total_flushed += n.flushed;

				/* When we evict less pages than we did	on a
				previous try we relax the LRU scan limit in
				order to attempt to evict more */
				limited_scan[i]
					= (previous_evicted[i] > n.evicted);
				previous_evicted[i] = n.evicted;

				requested_pages[i] += lru_chunk_size;

				/* If we failed to flush or evict this
				instance, do not bother anymore. But take into
			        account that we might have zero flushed pages
				because the flushing request was fully
				satisfied by unzip_LRU evictions. */
				if (requested_pages[i] >= scan_depth[i]
				    || !(srv_cleaner_eviction_factor
					? n.evicted
					: (n.flushed + n.unzip_LRU_evicted))) {

					active_instance[i] = false;
					remaining_instances--;
				} else {

					free_len = UT_LIST_GET_LEN(
						buf_pool->free);
				}
				if (n.flushed) {
					MONITOR_INC_VALUE_CUMULATIVE(
						MONITOR_LRU_BATCH_FLUSH_TOTAL_PAGE,
						MONITOR_LRU_BATCH_FLUSH_COUNT,
						MONITOR_LRU_BATCH_FLUSH_PAGES,
						n.flushed);
				}

				if (n.evicted) {
					MONITOR_INC_VALUE_CUMULATIVE(
						MONITOR_LRU_BATCH_EVICT_TOTAL_PAGE,
						MONITOR_LRU_BATCH_EVICT_COUNT,
						MONITOR_LRU_BATCH_EVICT_PAGES,
						n.evicted);
				}
			} while (active_instance[i]
				 && free_len <= free_list_lwm);
		}
	}

	return(total_flushed);
}

/*********************************************************************//**
Wait for any possible LRU flushes that are in progress to end. */
UNIV_INTERN
void
buf_flush_wait_LRU_batch_end(void)
/*==============================*/
{
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		mutex_enter(&buf_pool->flush_state_mutex);

		if (buf_pool->n_flush[BUF_FLUSH_LRU] > 0
		   || buf_pool->init_flush[BUF_FLUSH_LRU]) {

			mutex_exit(&buf_pool->flush_state_mutex);
			buf_flush_wait_batch_end(buf_pool, BUF_FLUSH_LRU);
		} else {
			mutex_exit(&buf_pool->flush_state_mutex);
		}
	}
}

/*********************************************************************//**
Flush a batch of dirty pages from the flush list
@return number of pages flushed, 0 if no page is flushed or if another
flush_list type batch is running */
static
ulint
page_cleaner_do_flush_batch(
/*========================*/
	ulint		n_to_flush,	/*!< in: number of pages that
					we should attempt to flush. */
	lsn_t		lsn_limit)	/*!< in: LSN up to which flushing
					must happen */
{
	ulint n_flushed;

	buf_flush_list(n_to_flush, lsn_limit, &n_flushed);

	return(n_flushed);
}

/*********************************************************************//**
Calculates if flushing is required based on number of dirty pages in
the buffer pool.
@return percent of io_capacity to flush to manage dirty page ratio */
static
ulint
af_get_pct_for_dirty()
/*==================*/
{
	ulint dirty_pct = (ulint) buf_get_modified_ratio_pct();

	if (dirty_pct > 0 && srv_max_buf_pool_modified_pct == 0) {
		return(100);
	}

	ut_a(srv_max_dirty_pages_pct_lwm
	     <= srv_max_buf_pool_modified_pct);

	if (srv_max_dirty_pages_pct_lwm == 0) {
		/* The user has not set the option to preflush dirty
		pages as we approach the high water mark. */
		if (dirty_pct > srv_max_buf_pool_modified_pct) {
			/* We have crossed the high water mark of dirty
			pages In this case we start flushing at 100% of
			innodb_io_capacity. */
			return(100);
		}
	} else if (dirty_pct > srv_max_dirty_pages_pct_lwm) {
		/* We should start flushing pages gradually. */
		return (ulint) ((dirty_pct * 100)
		       / (srv_max_buf_pool_modified_pct + 1));
	}

	return(0);
}

/*********************************************************************//**
Calculates if flushing is required based on redo generation rate.
@return percent of io_capacity to flush to manage redo space */
static
ulint
af_get_pct_for_lsn(
/*===============*/
	lsn_t	age)	/*!< in: current age of LSN. */
{
	lsn_t	max_async_age;
	lsn_t	lsn_age_factor;
	lsn_t	af_lwm = (lsn_t) ((srv_adaptive_flushing_lwm
			* log_get_capacity()) / 100);

	if (age < af_lwm) {
		/* No adaptive flushing. */
		return(0);
	}

	max_async_age = log_get_max_modified_age_async();

	if (age < max_async_age && !srv_adaptive_flushing) {
		/* We have still not reached the max_async point and
		the user has disabled adaptive flushing. */
		return(0);
	}

	/* If we are here then we know that either:
	1) User has enabled adaptive flushing
	2) User may have disabled adaptive flushing but we have reached
	max_async_age. */
	lsn_age_factor = (age * 100) / max_async_age;

	ut_ad(srv_max_io_capacity >= srv_io_capacity);
	switch ((srv_cleaner_lsn_age_factor_t)srv_cleaner_lsn_age_factor) {
	case SRV_CLEANER_LSN_AGE_FACTOR_LEGACY:
		return(static_cast<ulint>(
			       ((srv_max_io_capacity / srv_io_capacity)
				* (lsn_age_factor
				   * sqrt((double)lsn_age_factor)))
			       / 7.5));
	case SRV_CLEANER_LSN_AGE_FACTOR_HIGH_CHECKPOINT:
		return(static_cast<ulint>(
			       ((srv_max_io_capacity / srv_io_capacity)
				* (lsn_age_factor * lsn_age_factor
				   * sqrt((double)lsn_age_factor)))
			       / 700.5));
	default:
		ut_error;
	}
}

/*********************************************************************//**
This function is called approximately once every second by the
page_cleaner thread. Based on various factors it decides if there is a
need to do flushing. If flushing is needed it is performed and the
number of pages flushed is returned.
@return number of pages flushed */
static
ulint
page_cleaner_flush_pages_if_needed(void)
/*====================================*/
{
	static	lsn_t		lsn_avg_rate = 0;
	static	lsn_t		prev_lsn = 0;
	static	lsn_t		last_lsn = 0;
	static	ulint		sum_pages = 0;
	static	ulint		last_pages = 0;
	static	ulint		prev_pages = 0;
	static	ulint		avg_page_rate = 0;
	static	ulint		n_iterations = 0;
	lsn_t			oldest_lsn;
	lsn_t			cur_lsn;
	lsn_t			age;
	lsn_t			lsn_rate;
	ulint			n_pages = 0;
	ulint			pct_for_dirty = 0;
	ulint			pct_for_lsn = 0;
	ulint			pct_total = 0;
	int			age_factor = 0;

	cur_lsn = log_get_lsn_nowait();

	/* log_get_lsn_nowait tries to get log_sys->mutex with
	mutex_enter_nowait, if this does not succeed function
	returns 0, do not use that value to update stats. */
	if (cur_lsn == 0) {
		return(0);
	}

	if (prev_lsn == 0) {
		/* First time around. */
		prev_lsn = cur_lsn;
		return(0);
	}

	if (prev_lsn == cur_lsn) {
		return(0);
	}

	/* We update our variables every srv_flushing_avg_loops
	iterations to smooth out transition in workload. */
	if (++n_iterations >= srv_flushing_avg_loops) {

		avg_page_rate = ((sum_pages / srv_flushing_avg_loops)
				 + avg_page_rate) / 2;

		/* How much LSN we have generated since last call. */
		lsn_rate = (cur_lsn - prev_lsn) / srv_flushing_avg_loops;

		lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;

		prev_lsn = cur_lsn;

		n_iterations = 0;

		sum_pages = 0;
	}

	oldest_lsn = buf_pool_get_oldest_modification();

	ut_ad(oldest_lsn <= log_get_lsn());

	age = cur_lsn > oldest_lsn ? cur_lsn - oldest_lsn : 0;

	pct_for_dirty = af_get_pct_for_dirty();
	pct_for_lsn = af_get_pct_for_lsn(age);

	pct_total = ut_max(pct_for_dirty, pct_for_lsn);

	/* Cap the maximum IO capacity that we are going to use by
	max_io_capacity. */
	n_pages = PCT_IO(pct_total);
	if (age < log_get_max_modified_age_async())
		n_pages = (n_pages + avg_page_rate) / 2;

	if (n_pages > srv_max_io_capacity) {
		n_pages = srv_max_io_capacity;
	}

	if (last_pages && cur_lsn - last_lsn > lsn_avg_rate / 2) {
		age_factor = static_cast<int>(prev_pages / last_pages);
	}

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_REQUESTED, n_pages);

	prev_pages = n_pages;
	n_pages = page_cleaner_do_flush_batch(
		n_pages, oldest_lsn + lsn_avg_rate * (age_factor + 1));

	last_lsn= cur_lsn;
	last_pages= n_pages + 1;

	MONITOR_SET(MONITOR_FLUSH_AVG_PAGE_RATE, avg_page_rate);
	MONITOR_SET(MONITOR_FLUSH_LSN_AVG_RATE, lsn_avg_rate);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_DIRTY, pct_for_dirty);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_LSN, pct_for_lsn);

	if (n_pages) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_ADAPTIVE_TOTAL_PAGE,
			MONITOR_FLUSH_ADAPTIVE_COUNT,
			MONITOR_FLUSH_ADAPTIVE_PAGES,
			n_pages);

		sum_pages += n_pages;
	}

	return(n_pages);
}

/*********************************************************************//**
Puts the page_cleaner thread to sleep if it has finished work in less
than a second */
static
void
page_cleaner_sleep_if_needed(
/*=========================*/
	ulint	next_loop_time)	/*!< in: time when next loop iteration
				should start */
{
	/* No sleep if we are cleaning the buffer pool during the shutdown
	with everything else finished */
	if (srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE)
		return;

	ulint	cur_time = ut_time_ms();

	if (next_loop_time > cur_time) {
		/* Get sleep interval in micro seconds. We use
		ut_min() to avoid long sleep in case of
		wrap around. */
		os_thread_sleep(ut_min(1000000,
				(next_loop_time - cur_time)
				 * 1000));
	}
}

/*********************************************************************//**
Returns the aggregate free list length over all buffer pool instances.
@return total free list length. */
MY_ATTRIBUTE((warn_unused_result))
static
ulint
buf_get_total_free_list_length(void)
/*================================*/
{
	ulint result = 0;

	for (ulint i = 0; i < srv_buf_pool_instances; i++) {

		result += UT_LIST_GET_LEN(buf_pool_from_array(i)->free);
	}

	return result;
}

/*********************************************************************//**
Adjust the desired page cleaner thread sleep time for LRU flushes.  */
MY_ATTRIBUTE((nonnull))
static
void
page_cleaner_adapt_lru_sleep_time(
/*==============================*/
	ulint*	lru_sleep_time,	/*!< in/out: desired page cleaner thread sleep
				time for LRU flushes  */
	ulint	lru_n_flushed) /*!< in: number of flushed in previous batch */

{
	ulint free_len = buf_get_total_free_list_length();
	ulint max_free_len = srv_LRU_scan_depth * srv_buf_pool_instances;

	if (free_len < max_free_len / 100 && lru_n_flushed) {

		/* Free lists filled less than 1%
		and iteration was able to flush, no sleep */
		*lru_sleep_time = 0;
	} else if (free_len > max_free_len / 5
		   || (free_len < max_free_len / 100 && lru_n_flushed == 0)) {

		/* Free lists filled more than 20%
		or no pages flushed in previous batch, sleep a bit more */
		*lru_sleep_time += 50;
		if (*lru_sleep_time > srv_cleaner_max_lru_time)
			*lru_sleep_time = srv_cleaner_max_lru_time;
	} else if (free_len < max_free_len / 20 && *lru_sleep_time >= 50) {

		/* Free lists filled less than 5%, sleep a bit less */
		*lru_sleep_time -= 50;
	} else {

		/* Free lists filled between 5% and 20%, no change */
	}
}

/*********************************************************************//**
Get the desired page cleaner thread sleep time for flush list flushes.
@return desired sleep time */
MY_ATTRIBUTE((warn_unused_result))
static
ulint
page_cleaner_adapt_flush_sleep_time(void)
/*=====================================*/
{
	lsn_t	age = log_get_lsn() - log_sys->last_checkpoint_lsn;

	if (age > log_sys->max_modified_age_sync) {

		/* No sleep if in sync preflush zone */
		return(0);
	}

	/* In all other cases flush list factors do not influence the page
	cleaner sleep time */
	return(srv_cleaner_max_flush_time);
}

/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pool flush lists. As of now we'll have only one instance of this thread.
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(buf_flush_page_cleaner_thread)(
/*==========================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint	next_loop_time = ut_time_ms() + 1000;
	ulint	n_flushed = 0;
	ulint	last_activity = srv_get_activity_count();
	ulint	last_activity_time = ut_time_ms();

	ut_ad(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(buf_page_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */

	srv_cleaner_tid = os_thread_get_tid();

	os_thread_set_priority(srv_cleaner_tid, srv_sched_priority_cleaner);

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "InnoDB: page_cleaner thread running, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	buf_page_cleaner_is_active = TRUE;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		ulint	page_cleaner_sleep_time;
		ibool	server_active;

		srv_current_thread_priority = srv_cleaner_thread_priority;

		page_cleaner_sleep_if_needed(next_loop_time);

		page_cleaner_sleep_time
			= page_cleaner_adapt_flush_sleep_time();

		next_loop_time = ut_time_ms() + page_cleaner_sleep_time;

		server_active = srv_check_activity(last_activity);

		if (server_active
		    || ut_time_ms() - last_activity_time < 1000) {

			if (server_active) {

				last_activity = srv_get_activity_count();
				last_activity_time = ut_time_ms();
			}

			/* Flush pages from flush_list if required */
			page_cleaner_flush_pages_if_needed();
		} else if (srv_idle_flush_pct) {
			n_flushed = page_cleaner_do_flush_batch(
							PCT_IO(100),
							LSN_MAX);

			if (n_flushed) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_BACKGROUND_TOTAL_PAGE,
					MONITOR_FLUSH_BACKGROUND_COUNT,
					MONITOR_FLUSH_BACKGROUND_PAGES,
					n_flushed);
			}
		}

		/* Flush pages from end of LRU if required */
		buf_flush_LRU_tail();
	}

	ut_ad(srv_shutdown_state > 0);
	if (srv_fast_shutdown == 2) {
		/* In very fast shutdown we simulate a crash of
		buffer pool. We are not required to do any flushing */
		goto thread_exit;
	}

	/* In case of normal and slow shutdown the page_cleaner thread
	must wait for all other activity in the server to die down.
	Note that we can start flushing the buffer pool as soon as the
	server enters shutdown phase but we must stay alive long enough
	to ensure that any work done by the master or purge threads is
	also flushed.
	During shutdown we pass through two stages. In the first stage,
	when SRV_SHUTDOWN_CLEANUP is set other threads like the master
	and the purge threads may be working as well. We start flushing
	the buffer pool but can't be sure that no new pages are being
	dirtied until we enter SRV_SHUTDOWN_FLUSH_PHASE phase. */

	do {
		n_flushed = page_cleaner_do_flush_batch(PCT_IO(100), LSN_MAX);

		/* We sleep only if there are no pages to flush */
		if (n_flushed == 0) {
			os_thread_sleep(100000);
		}
	} while (srv_shutdown_state == SRV_SHUTDOWN_CLEANUP);

	/* At this point all threads including the master and the purge
	thread must have been suspended. */
	ut_a(srv_get_active_thread_type() == SRV_NONE);
	ut_a(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);

	/* We can now make a final sweep on flushing the buffer pool
	and exit after we have cleaned the whole buffer pool.
	It is important that we wait for any running batch that has
	been triggered by us to finish. Otherwise we can end up
	considering end of that batch as a finish of our final
	sweep and we'll come out of the loop leaving behind dirty pages
	in the flush_list */
	buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
	buf_flush_wait_LRU_batch_end();

	bool	success;

	do {

		success = buf_flush_list(PCT_IO(100), LSN_MAX, &n_flushed);
		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);

	} while (!success || n_flushed > 0);

	/* Some sanity checks */
	ut_a(srv_get_active_thread_type() == SRV_NONE);
	ut_a(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);
	for (ulint i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t* buf_pool = buf_pool_from_array(i);
		ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == 0);
	}

	/* We have lived our life. Time to die. */

thread_exit:
	buf_page_cleaner_is_active = FALSE;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/******************************************************************//**
lru_manager thread tasked with performing LRU flushes and evictions to refill
the buffer pool free lists.  As of now we'll have only one instance of this
thread.
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(buf_flush_lru_manager_thread)(
/*==========================================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ulint	next_loop_time = ut_time_ms() + 1000;
	ulint	lru_sleep_time = srv_cleaner_max_lru_time;
	ulint	lru_n_flushed = 1;

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(buf_lru_manager_thread_key);
#endif /* UNIV_PFS_THREAD */

	srv_lru_manager_tid = os_thread_get_tid();

	os_thread_set_priority(srv_lru_manager_tid,
			       srv_sched_priority_cleaner);

#ifdef UNIV_DEBUG_THREAD_CREATION
	fprintf(stderr, "InnoDB: lru_manager thread running, id %lu\n",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	buf_lru_manager_is_active = true;

	/* On server shutdown, the LRU manager thread runs through cleanup
	phase to provide free pages for the master and purge threads.  */
	while (srv_shutdown_state == SRV_SHUTDOWN_NONE
	       || srv_shutdown_state == SRV_SHUTDOWN_CLEANUP) {

		srv_current_thread_priority = srv_cleaner_thread_priority;

		page_cleaner_sleep_if_needed(next_loop_time);

		page_cleaner_adapt_lru_sleep_time(&lru_sleep_time, lru_n_flushed);

		next_loop_time = ut_time_ms() + lru_sleep_time;

		lru_n_flushed = buf_flush_LRU_tail();
	}

	buf_lru_manager_is_active = false;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG

/** Functor to validate the flush list. */
struct	Check {
	void	operator()(const buf_page_t* elem)
	{
		ut_a(elem->in_flush_list);
	}
};

/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
static
ibool
buf_flush_validate_low(
/*===================*/
	buf_pool_t*	buf_pool)		/*!< in: Buffer pool instance */
{
	buf_page_t*		bpage;
	const ib_rbt_node_t*	rnode = NULL;

	ut_ad(buf_flush_list_mutex_own(buf_pool));

	UT_LIST_VALIDATE(list, buf_page_t, buf_pool->flush_list, Check());

	bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);

	/* If we are in recovery mode i.e.: flush_rbt != NULL
	then each block in the flush_list must also be present
	in the flush_rbt. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		rnode = rbt_first(buf_pool->flush_rbt);
	}

	while (bpage != NULL) {
		const lsn_t	om = bpage->oldest_modification;

		ut_ad(buf_pool_from_bpage(bpage) == buf_pool);

		ut_ad(bpage->in_flush_list);

		/* A page in buf_pool->flush_list can be in
		BUF_BLOCK_REMOVE_HASH state. This happens when a page
		is in the middle of being relocated. In that case the
		original descriptor can have this state and still be
		in the flush list waiting to acquire the
		buf_pool->flush_list_mutex to complete the relocation. */
		ut_a(buf_page_in_file(bpage)
		     || buf_page_get_state(bpage) == BUF_BLOCK_REMOVE_HASH);
		ut_a(om > 0);

		if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
			buf_page_t** prpage;

			ut_a(rnode);
			prpage = rbt_value(buf_page_t*, rnode);

			ut_a(*prpage);
			ut_a(*prpage == bpage);
			rnode = rbt_next(buf_pool->flush_rbt, rnode);
		}

		bpage = UT_LIST_GET_NEXT(list, bpage);

		ut_a(!bpage || om >= bpage->oldest_modification);
	}

	/* By this time we must have exhausted the traversal of
	flush_rbt (if active) as well. */
	ut_a(rnode == NULL);

	return(TRUE);
}

/******************************************************************//**
Validates the flush list.
@return	TRUE if ok */
UNIV_INTERN
ibool
buf_flush_validate(
/*===============*/
	buf_pool_t*	buf_pool)	/*!< buffer pool instance */
{
	ibool	ret;

	buf_flush_list_mutex_enter(buf_pool);

	ret = buf_flush_validate_low(buf_pool);

	buf_flush_list_mutex_exit(buf_pool);

	return(ret);
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
#endif /* !UNIV_HOTBACKUP */

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
	ulint		id)		/*!< in: space id to check */

{
	ulint		count = 0;

	buf_flush_list_mutex_enter(buf_pool);

	buf_page_t*	bpage;

	for (bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);
	     bpage != 0;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_ad(buf_page_in_file(bpage)
		      || buf_page_get_state(bpage) == BUF_BLOCK_REMOVE_HASH);
		ut_ad(bpage->in_flush_list);
		ut_ad(bpage->oldest_modification > 0);

		if (bpage->space == id) {
			++count;
		}
	}

	buf_flush_list_mutex_exit(buf_pool);

	return(count);
}

/******************************************************************//**
Check if there are any dirty pages that belong to a space id in the flush list.
@return	number of dirty pages present in all the buffer pools */
UNIV_INTERN
ulint
buf_flush_get_dirty_pages_count(
/*============================*/
	ulint		id)		/*!< in: space id to check */

{
	ulint		count = 0;

	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool;

		buf_pool = buf_pool_from_array(i);

		count += buf_pool_get_dirty_pages_count(buf_pool, id);
	}

	return(count);
}
#endif /* UNIV_DEBUG */
