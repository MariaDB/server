/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2020, MariaDB Corporation.
Copyright (c) 2013, 2014, Fusion-io

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
@file buf/buf0flu.cc
The database buffer buf_pool flush algorithm

Created 11/11/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <mysql/service_thd_wait.h>
#include <sql_class.h>

#include "buf0flu.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "buf0dblwr.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "ut0byte.h"
#include "page0page.h"
#include "fil0fil.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "log0crypt.h"
#include "os0file.h"
#include "trx0sys.h"
#include "srv0mon.h"
#include "ut0stage.h"
#include "fil0pagecompress.h"
#ifdef UNIV_LINUX
/* include defs for CPU time priority settings */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
static const int buf_flush_page_cleaner_priority = -20;
#endif /* UNIV_LINUX */
#ifdef HAVE_LZO
#include "lzo/lzo1x.h"
#endif

#ifdef HAVE_SNAPPY
#include "snappy-c.h"
#endif

/** Sleep time in microseconds for loop waiting for the oldest
modification lsn */
static const ulint buf_flush_wait_flushed_sleep_time = 10000;

#include <my_service_manager.h>

/** Number of pages flushed through non flush_list flushes. */
ulint buf_lru_flush_page_count;

/** Flag indicating if the page_cleaner is in active state. This flag
is set to TRUE by the page_cleaner thread when it is spawned and is set
back to FALSE at shutdown by the page_cleaner as well. Therefore no
need to protect it by a mutex. It is only ever read by the thread
doing the shutdown */
bool buf_page_cleaner_is_active;

/** Factor for scan length to determine n_pages for intended oldest LSN
progress */
static ulint buf_flush_lsn_scan_factor = 3;

/** Average redo generation rate */
static lsn_t lsn_avg_rate = 0;

/** Target oldest LSN for the requested flush_sync */
static lsn_t buf_flush_sync_lsn = 0;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t page_cleaner_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Event to synchronise with the flushing. */
os_event_t	buf_flush_event;

static void pc_flush_slot_func(void *);
static tpool::task_group page_cleaner_task_group(1);
static tpool::waitable_task pc_flush_slot_task(
	pc_flush_slot_func, 0, &page_cleaner_task_group);

/** State for page cleaner array slot */
enum page_cleaner_state_t {
	/** Not requested any yet. Moved from FINISHED. */
	PAGE_CLEANER_STATE_NONE = 0,
	/** Requested but not started flushing. Moved from NONE. */
	PAGE_CLEANER_STATE_REQUESTED,
	/** Flushing is on going. Moved from REQUESTED. */
	PAGE_CLEANER_STATE_FLUSHING,
	/** Flushing was finished. Moved from FLUSHING. */
	PAGE_CLEANER_STATE_FINISHED
};

/** Page cleaner request state for buf_pool */
struct page_cleaner_slot_t {
	page_cleaner_state_t	state;	/*!< state of the request.
					protected by page_cleaner_t::mutex
					if the worker thread got the slot and
					set to PAGE_CLEANER_STATE_FLUSHING,
					n_flushed_lru and n_flushed_list can be
					updated only by the worker thread */
	/* This value is set during state==PAGE_CLEANER_STATE_NONE */
	ulint			n_pages_requested;
					/*!< number of requested pages
					for the slot */
	/* These values are updated during state==PAGE_CLEANER_STATE_FLUSHING,
	and commited with state==PAGE_CLEANER_STATE_FINISHED.
	The consistency is protected by the 'state' */
	ulint			n_flushed_lru;
					/*!< number of flushed pages
					by LRU scan flushing */
	ulint			n_flushed_list;
					/*!< number of flushed pages
					by flush_list flushing */
	bool			succeeded_list;
					/*!< true if flush_list flushing
					succeeded. */
	ulint			flush_lru_time;
					/*!< elapsed time for LRU flushing */
	ulint			flush_list_time;
					/*!< elapsed time for flush_list
					flushing */
	ulint			flush_lru_pass;
					/*!< count to attempt LRU flushing */
	ulint			flush_list_pass;
					/*!< count to attempt flush_list
					flushing */
};

/** Page cleaner structure */
struct page_cleaner_t {
	/* FIXME: do we need mutex? use atomics? */
	ib_mutex_t		mutex;		/*!< mutex to protect whole of
						page_cleaner_t struct and
						page_cleaner_slot_t slots. */
	os_event_t		is_finished;	/*!< event to signal that all
						slots were finished. */
	bool			requested;	/*!< true if requested pages
						to flush */
	lsn_t			lsn_limit;	/*!< upper limit of LSN to be
						flushed */
#if 1 /* FIXME: use bool for these, or remove some of these */
	ulint			n_slots_requested;
						/*!< number of slots
						in the state
						PAGE_CLEANER_STATE_REQUESTED */
	ulint			n_slots_flushing;
						/*!< number of slots
						in the state
						PAGE_CLEANER_STATE_FLUSHING */
	ulint			n_slots_finished;
						/*!< number of slots
						in the state
						PAGE_CLEANER_STATE_FINISHED */
#endif
	ulint			flush_time;	/*!< elapsed time to flush
						requests for all slots */
	ulint			flush_pass;	/*!< count to finish to flush
						requests for all slots */
	page_cleaner_slot_t	slot;
	bool			is_running;	/*!< false if attempt
						to shutdown */
};

static page_cleaner_t	page_cleaner;

#ifdef UNIV_DEBUG
my_bool innodb_page_cleaner_disabled_debug;
#endif /* UNIV_DEBUG */

/** If LRU list of a buf_pool is less than this size then LRU eviction
should not happen. This is because when we do LRU flushing we also put
the blocks on free list. If LRU list is very small then we can end up
in thrashing. */
#define BUF_LRU_MIN_LEN		256

/* @} */

/** Increases flush_list size in bytes with the page size */
static inline void incr_flush_list_size_in_bytes(const buf_block_t* block)
{
	/* FIXME: use std::atomic! */
	ut_ad(mutex_own(&buf_pool->flush_list_mutex));
	buf_pool->stat.flush_list_bytes += block->physical_size();
	ut_ad(buf_pool->stat.flush_list_bytes <= buf_pool->curr_pool_size);
}

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
/** Validate the flush list. */
static void buf_flush_validate_low();

/** Validates the flush list some of the time. */
static void buf_flush_validate_skip()
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
		return;
	}

	buf_flush_validate_count = BUF_FLUSH_VALIDATE_SKIP;
	buf_flush_validate_low();
}
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/******************************************************************//**
Insert a block in the flush_rbt and returns a pointer to its
predecessor or NULL if no predecessor. The ordering is maintained
on the basis of the <oldest_modification, space, offset> key.
@return pointer to the predecessor or NULL if no predecessor. */
static
buf_page_t*
buf_flush_insert_in_flush_rbt(
/*==========================*/
	buf_page_t*	bpage)	/*!< in: bpage to be inserted. */
{
	const ib_rbt_node_t*	c_node;
	const ib_rbt_node_t*	p_node;
	buf_page_t*		prev = NULL;

	ut_ad(srv_shutdown_state != SRV_SHUTDOWN_FLUSH_PHASE);
	ut_ad(mutex_own(&buf_pool->flush_list_mutex));

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
	ut_ad(mutex_own(&buf_pool->flush_list_mutex));

#ifdef UNIV_DEBUG
	ibool ret =
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
@return < 0 if b2 < b1, 0 if b2 == b1, > 0 if b2 > b1 */
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

	ut_ad(b1 != NULL);
	ut_ad(b2 != NULL);

	ut_ad(mutex_own(&buf_pool->flush_list_mutex));

	ut_ad(b1->in_flush_list);
	ut_ad(b2->in_flush_list);

	if (b2->oldest_modification > b1->oldest_modification) {
		return(1);
	} else if (b2->oldest_modification < b1->oldest_modification) {
		return(-1);
	}

	/* If oldest_modification is same then decide on the space. */
	ret = (int)(b2->id.space() - b1->id.space());

	/* Or else decide ordering on the page number. */
	return(ret ? ret : (int) (b2->id.page_no() - b1->id.page_no()));
}

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
void
buf_flush_init_flush_rbt(void)
/*==========================*/
{
	mutex_enter(&buf_pool->flush_list_mutex);
	ut_ad(buf_pool->flush_rbt == NULL);
	/* Create red black tree for speedy insertions in flush list. */
	buf_pool->flush_rbt = rbt_create(
		sizeof(buf_page_t*), buf_flush_block_cmp);
	mutex_exit(&buf_pool->flush_list_mutex);
}

/********************************************************************//**
Frees up the red-black tree. */
void
buf_flush_free_flush_rbt(void)
/*==========================*/
{
	mutex_enter(&buf_pool->flush_list_mutex);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	buf_flush_validate_low();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
	rbt_free(buf_pool->flush_rbt);
	buf_pool->flush_rbt = NULL;
	mutex_exit(&buf_pool->flush_list_mutex);
}

/** Insert a modified block into the flush list.
@param[in,out]	block	modified block
@param[in]	lsn	oldest modification */
void buf_flush_insert_into_flush_list(buf_block_t* block, lsn_t lsn)
{
	ut_ad(!mutex_own(&buf_pool->mutex));
	ut_ad(log_flush_order_mutex_own());
	ut_ad(buf_page_mutex_own(block));
	ut_ad(lsn);

	mutex_enter(&buf_pool->flush_list_mutex);
	ut_ad(!block->page.in_flush_list);
	ut_d(block->page.in_flush_list = TRUE);
	ut_ad(!block->page.oldest_modification);
	block->page.oldest_modification = lsn;
	UNIV_MEM_ASSERT_RW(block->page.zip.data
			   ? block->page.zip.data : block->frame,
			   block->physical_size());
	incr_flush_list_size_in_bytes(block);

	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		ut_ad(srv_shutdown_state != SRV_SHUTDOWN_FLUSH_PHASE);
		/* The field in_LRU_list is protected by buf_pool->mutex, which
		we are not holding.  However, while a block is in the flush
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

		if (buf_page_t* prev_b =
		    buf_flush_insert_in_flush_rbt(&block->page)) {
			UT_LIST_INSERT_AFTER(buf_pool->flush_list, prev_b, &block->page);
			goto func_exit;
		}
	}

	UT_LIST_ADD_FIRST(buf_pool->flush_list, &block->page);
func_exit:
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	buf_flush_validate_skip();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	mutex_exit(&buf_pool->flush_list_mutex);
}

/********************************************************************//**
Returns TRUE if the file page block is immediately suitable for replacement,
i.e., the transition FILE_PAGE => NOT_USED allowed.
@return TRUE if can replace immediately */
ibool
buf_flush_ready_for_replace(
/*========================*/
	buf_page_t*	bpage)	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) and in the LRU list */
{
	ut_ad(mutex_own(&buf_pool->mutex));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(bpage->in_LRU_list);

	if (buf_page_in_file(bpage)) {

		return(bpage->oldest_modification == 0
		       && bpage->buf_fix_count == 0
		       && buf_page_get_io_fix(bpage) == BUF_IO_NONE);
	}

	ib::fatal() << "Buffer block " << bpage << " state " <<  bpage->state
		<< " in the LRU list!";

	return(FALSE);
}

/********************************************************************//**
Returns true if the block is modified and ready for flushing.
@return true if can flush immediately */
bool
buf_flush_ready_for_flush(
/*======================*/
	buf_page_t*	bpage,	/*!< in: buffer control block, must be
				buf_page_in_file(bpage) */
	buf_flush_t	flush_type)/*!< in: type of flush */
{
	ut_ad(mutex_own(&buf_pool->mutex));
	ut_a(buf_page_in_file(bpage));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(flush_type < BUF_FLUSH_N_TYPES);

	if (bpage->oldest_modification == 0
	    || buf_page_get_io_fix(bpage) != BUF_IO_NONE) {
		return(false);
	}

	ut_ad(bpage->in_flush_list);

	switch (flush_type) {
	case BUF_FLUSH_LIST:
	case BUF_FLUSH_LRU:
	case BUF_FLUSH_SINGLE_PAGE:
		return(true);

	case BUF_FLUSH_N_TYPES:
		break;
	}

	ut_error;
	return(false);
}

/** Remove a block from the flush list of modified blocks.
@param[in]	bpage	block to be removed from the flush list */
void buf_flush_remove(buf_page_t* bpage)
{
#if 0 // FIXME: Rate-limit the output. Move this to the page cleaner?
	if (UNIV_UNLIKELY(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE)) {
		service_manager_extend_timeout(
			INNODB_EXTEND_TIMEOUT_INTERVAL,
			"Flush and remove page with tablespace id %u"
			", flush list length " ULINTPF,
			bpage->space, UT_LIST_GET_LEN(buf_pool->flush_list));
	}
#endif
	ut_ad(mutex_own(&buf_pool->mutex));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));
	ut_ad(bpage->in_flush_list);

	mutex_enter(&buf_pool->flush_list_mutex);

	/* Important that we adjust the hazard pointer before removing
	the bpage from flush list. */
	buf_pool->flush_hp.adjust(bpage);

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
		UT_LIST_REMOVE(buf_pool->flush_list, bpage);
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
		buf_LRU_insert_zip_clean(bpage);
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */
		break;
	case BUF_BLOCK_FILE_PAGE:
		UT_LIST_REMOVE(buf_pool->flush_list, bpage);
		break;
	}

	/* If the flush_rbt is active then delete from there as well. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		buf_flush_delete_from_flush_rbt(bpage);
	}

	/* Must be done after we have removed it from the flush_rbt
	because we assert on in_flush_list in comparison function. */
	ut_d(bpage->in_flush_list = FALSE);

	buf_pool->stat.flush_list_bytes -= bpage->physical_size();

	bpage->oldest_modification = 0;

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	buf_flush_validate_skip();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	mutex_exit(&buf_pool->flush_list_mutex);
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
void
buf_flush_relocate_on_flush_list(
/*=============================*/
	buf_page_t*	bpage,	/*!< in/out: control block being moved */
	buf_page_t*	dpage)	/*!< in/out: destination block */
{
	buf_page_t*	prev;
	buf_page_t*	prev_b = NULL;

	ut_ad(mutex_own(&buf_pool->mutex));
	ut_ad(mutex_own(buf_page_get_mutex(bpage)));

	mutex_enter(&buf_pool->flush_list_mutex);

	/* FIXME: At this point we have both buf_pool and flush_list
	mutexes. Theoretically removal of a block from flush list is
	only covered by flush_list mutex but currently we do
	have buf_pool mutex in buf_flush_remove() therefore this block
	is guaranteed to be in the flush list. We need to check if
	this will work without the assumption of block removing code
	having the buf_pool mutex. */
	ut_ad(bpage->in_flush_list);
	ut_ad(dpage->in_flush_list);

	/* If recovery is active we must swap the control blocks in
	the flush_rbt as well. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		buf_flush_delete_from_flush_rbt(bpage);
		prev_b = buf_flush_insert_in_flush_rbt(dpage);
	}

	/* Important that we adjust the hazard pointer before removing
	the bpage from the flush list. */
	buf_pool->flush_hp.adjust(bpage);

	/* Must be done after we have removed it from the flush_rbt
	because we assert on in_flush_list in comparison function. */
	ut_d(bpage->in_flush_list = FALSE);

	prev = UT_LIST_GET_PREV(list, bpage);
	UT_LIST_REMOVE(buf_pool->flush_list, bpage);

	if (prev) {
		ut_ad(prev->in_flush_list);
		UT_LIST_INSERT_AFTER( buf_pool->flush_list, prev, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool->flush_list, dpage);
	}

	/* Just an extra check. Previous in flush_list
	should be the same control block as in flush_rbt. */
	ut_a(buf_pool->flush_rbt == NULL || prev_b == prev);

#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG
	buf_flush_validate_low();
#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

	mutex_exit(&buf_pool->flush_list_mutex);
}

/** Update the flush system data structures when a write is completed.
@param[in,out]	bpage	flushed page
@param[in]	dblwr	whether the doublewrite buffer was used */
void buf_flush_write_complete(buf_page_t* bpage, bool dblwr)
{
	ut_ad(bpage);

	buf_flush_remove(bpage);

	const buf_flush_t flush_type = buf_page_get_flush_type(bpage);
	buf_pool->n_flush[flush_type]--;
	ut_ad(buf_pool->n_flush[flush_type] != ULINT_MAX);

	ut_ad(mutex_own(&buf_pool->mutex));

	if (buf_pool->n_flush[flush_type] == 0
	    && buf_pool->init_flush[flush_type] == FALSE) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}

	if (dblwr) {
		buf_dblwr_update(bpage, flush_type);
	}
}

/** Calculate a ROW_FORMAT=COMPRESSED page checksum and update the page.
@param[in,out]	page		page to update
@param[in]	size		compressed page size */
void buf_flush_update_zip_checksum(buf_frame_t *page, ulint size)
{
  ut_ad(size > 0);
  mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
                  page_zip_calc_checksum(page, size,
                                         static_cast<srv_checksum_algorithm_t>
                                         (srv_checksum_algorithm)));
}

/** Assign the full crc32 checksum for non-compressed page.
@param[in,out]	page	page to be updated */
void buf_flush_assign_full_crc32_checksum(byte* page)
{
	ut_d(bool compressed = false);
	ut_d(bool corrupted = false);
	ut_d(const uint size = buf_page_full_crc32_size(page, &compressed,
							&corrupted));
	ut_ad(!compressed);
	ut_ad(!corrupted);
	ut_ad(size == uint(srv_page_size));
	const ulint payload = srv_page_size - FIL_PAGE_FCRC32_CHECKSUM;
	mach_write_to_4(page + payload, ut_crc32(page, payload));
}

/** Initialize a page for writing to the tablespace.
@param[in]	block			buffer block; NULL if bypassing
					the buffer pool
@param[in,out]	page			page frame
@param[in,out]	page_zip_		compressed page, or NULL if
					uncompressed
@param[in]	use_full_checksum	whether tablespace uses full checksum */
void
buf_flush_init_for_writing(
	const buf_block_t*	block,
	byte*			page,
	void*			page_zip_,
	bool			use_full_checksum)
{
	if (block != NULL && block->frame != page) {
		/* If page is encrypted in full crc32 format then
		checksum stored already as a part of fil_encrypt_buf() */
		ut_ad(use_full_checksum);
		return;
	}

	ut_ad(block == NULL || block->frame == page);
	ut_ad(block == NULL || page_zip_ == NULL
	      || &block->page.zip == page_zip_);
	ut_ad(page);

	if (page_zip_) {
		page_zip_des_t*	page_zip;
		ulint		size;

		page_zip = static_cast<page_zip_des_t*>(page_zip_);
		size = page_zip_get_size(page_zip);

		ut_ad(size);
		ut_ad(ut_is_2pow(size));
		ut_ad(size <= UNIV_ZIP_SIZE_MAX);

		switch (fil_page_get_type(page)) {
		case FIL_PAGE_TYPE_ALLOCATED:
		case FIL_PAGE_INODE:
		case FIL_PAGE_IBUF_BITMAP:
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			/* These are essentially uncompressed pages. */
			memcpy(page_zip->data, page, size);
			/* fall through */
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
		case FIL_PAGE_INDEX:
		case FIL_PAGE_RTREE:
			buf_flush_update_zip_checksum(page_zip->data, size);
			return;
		}

		ib::error() << "The compressed page to be written"
			" seems corrupt:";
		ut_print_buf(stderr, page, size);
		fputs("\nInnoDB: Possibly older version of the page:", stderr);
		ut_print_buf(stderr, page_zip->data, size);
		putc('\n', stderr);
		ut_error;
	}

	if (use_full_checksum) {
		static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "aligned");
		static_assert(FIL_PAGE_LSN % 4 == 0, "aligned");
		memcpy_aligned<4>(page + srv_page_size
				  - FIL_PAGE_FCRC32_END_LSN,
				  FIL_PAGE_LSN + 4 + page, 4);
		return buf_flush_assign_full_crc32_checksum(page);
	}

	static_assert(FIL_PAGE_END_LSN_OLD_CHKSUM % 8 == 0, "aligned");
	static_assert(FIL_PAGE_LSN % 8 == 0, "aligned");
	memcpy_aligned<8>(page + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM,
			  FIL_PAGE_LSN + page, 8);

	if (block && srv_page_size == 16384) {
		/* The page type could be garbage in old files
		created before MySQL 5.5. Such files always
		had a page size of 16 kilobytes. */
		ulint	page_type = fil_page_get_type(page);
		ulint	reset_type = page_type;

		switch (block->page.id.page_no() % 16384) {
		case 0:
			reset_type = block->page.id.page_no() == 0
				? FIL_PAGE_TYPE_FSP_HDR
				: FIL_PAGE_TYPE_XDES;
			break;
		case 1:
			reset_type = FIL_PAGE_IBUF_BITMAP;
			break;
		case FSP_TRX_SYS_PAGE_NO:
			if (block->page.id.page_no()
			    == TRX_SYS_PAGE_NO
			    && block->page.id.space()
			    == TRX_SYS_SPACE) {
				reset_type = FIL_PAGE_TYPE_TRX_SYS;
				break;
			}
			/* fall through */
		default:
			switch (page_type) {
			case FIL_PAGE_INDEX:
			case FIL_PAGE_TYPE_INSTANT:
			case FIL_PAGE_RTREE:
			case FIL_PAGE_UNDO_LOG:
			case FIL_PAGE_INODE:
			case FIL_PAGE_IBUF_FREE_LIST:
			case FIL_PAGE_TYPE_ALLOCATED:
			case FIL_PAGE_TYPE_SYS:
			case FIL_PAGE_TYPE_TRX_SYS:
			case FIL_PAGE_TYPE_BLOB:
			case FIL_PAGE_TYPE_ZBLOB:
			case FIL_PAGE_TYPE_ZBLOB2:
				break;
			case FIL_PAGE_TYPE_FSP_HDR:
			case FIL_PAGE_TYPE_XDES:
			case FIL_PAGE_IBUF_BITMAP:
				/* These pages should have
				predetermined page numbers
				(see above). */
			default:
				reset_type = FIL_PAGE_TYPE_UNKNOWN;
				break;
			}
		}

		if (UNIV_UNLIKELY(page_type != reset_type)) {
			ib::info()
				<< "Resetting invalid page "
				<< block->page.id << " type "
				<< page_type << " to "
				<< reset_type << " when flushing.";
			fil_page_set_type(page, reset_type);
		}
	}

	uint32_t checksum = BUF_NO_CHECKSUM_MAGIC;

	switch (srv_checksum_algorithm_t(srv_checksum_algorithm)) {
	case SRV_CHECKSUM_ALGORITHM_INNODB:
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		checksum = buf_calc_page_new_checksum(page);
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
				checksum);
		/* With the InnoDB checksum, we overwrite the first 4 bytes of
		the end lsn field to store the old formula checksum. Since it
		depends also on the field FIL_PAGE_SPACE_OR_CHKSUM, it has to
		be calculated after storing the new formula checksum. */
		checksum = buf_calc_page_old_checksum(page);
		break;
	case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		/* In other cases we write the same checksum to both fields. */
		checksum = buf_calc_page_crc32(page);
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
				checksum);
		break;
	case SRV_CHECKSUM_ALGORITHM_NONE:
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM,
				checksum);
		break;
		/* no default so the compiler will emit a warning if
		new enum is added and not handled here */
	}

	mach_write_to_4(page + srv_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM,
			checksum);
}

/** Reserve a buffer for compression.
@param[in,out]  slot    reserved slot */
static void buf_tmp_reserve_compression_buf(buf_tmp_buffer_t* slot)
{
  if (slot->comp_buf)
    return;
  /* Both Snappy and LZO compression methods require that the output
  buffer be bigger than input buffer. Adjust the allocated size. */
  ulint size= srv_page_size;
#ifdef HAVE_LZO
  size+= LZO1X_1_15_MEM_COMPRESS;
#elif defined HAVE_SNAPPY
  size= snappy_max_compressed_length(size);
#endif
  slot->comp_buf= static_cast<byte*>(aligned_malloc(size, srv_page_size));
}

/** Encrypt a buffer of temporary tablespace
@param[in]      offset  Page offset
@param[in]      s       Page to encrypt
@param[in,out]  d       Output buffer
@return encrypted buffer or NULL */
static byte* buf_tmp_page_encrypt(ulint offset, const byte* s, byte* d)
{
  /* Calculate the start offset in a page */
  uint srclen= srv_page_size - (FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION +
                                FIL_PAGE_FCRC32_CHECKSUM);
  const byte* src= s + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;
  byte* dst= d + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION;

  memcpy(d, s, FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

  if (!log_tmp_block_encrypt(src, srclen, dst, (offset * srv_page_size), true))
    return NULL;

  const ulint payload= srv_page_size - FIL_PAGE_FCRC32_CHECKSUM;
  mach_write_to_4(d + payload, ut_crc32(d, payload));

  srv_stats.pages_encrypted.inc();
  srv_stats.n_temp_blocks_encrypted.inc();
  return d;
}

/** Encryption and page_compression hook that is called just before
a page is written to disk.
@param[in,out]  space   tablespace
@param[in,out]  bpage   buffer page
@param[in]      s       physical page frame that is being encrypted
@return page frame to be written to file
(may be src_frame or an encrypted/compressed copy of it) */
static byte* buf_page_encrypt(fil_space_t* space, buf_page_t* bpage, byte* s)
{
  ut_ad(space->id == bpage->id.space());
  bpage->real_size = srv_page_size;

  ut_d(fil_page_type_validate(space, s));

  switch (bpage->id.page_no()) {
  case TRX_SYS_PAGE_NO:
    if (bpage->id.space() != TRX_SYS_SPACE)
      break;
    /* The TRX_SYS page is neither encrypted nor compressed, because
    it contains the address of the doublewrite buffer. */
    /* fall through */
  case 0:
    /* Page 0 of a tablespace is not encrypted/compressed */
    return s;
  }

  fil_space_crypt_t *crypt_data= space->crypt_data;
  bool encrypted, page_compressed;
  if (space->purpose == FIL_TYPE_TEMPORARY)
  {
    ut_ad(!crypt_data);
    encrypted= innodb_encrypt_temporary_tables;
    page_compressed= false;
  }
  else
  {
    encrypted= crypt_data && !crypt_data->not_encrypted() &&
      crypt_data->type != CRYPT_SCHEME_UNENCRYPTED &&
      (!crypt_data->is_default_encryption() || srv_encrypt_tables);
    page_compressed= space->is_compressed();
  }

  const bool full_crc32= space->full_crc32();

  if (!encrypted && !page_compressed)
  {
    /* No need to encrypt or compress. Clear key-version & crypt-checksum. */
    static_assert(FIL_PAGE_FCRC32_KEY_VERSION % 4 == 0, "alignment");
    static_assert(FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION % 4 == 2,
                  "not perfect alignment");
    if (full_crc32)
      memset_aligned<4>(s + FIL_PAGE_FCRC32_KEY_VERSION, 0, 4);
    else
      memset_aligned<2>(s + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
    return s;
  }

  static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");
  static_assert(FIL_PAGE_LSN % 8 == 0, "alignment");
  if (full_crc32)
    memcpy_aligned<4>(s + srv_page_size - FIL_PAGE_FCRC32_END_LSN,
                      FIL_PAGE_LSN + 4 + s, 4);

  ut_ad(!bpage->zip_size() || !page_compressed);
  /* Find free slot from temporary memory array */
  buf_tmp_buffer_t *slot= buf_pool->io_buf.reserve();
  ut_a(slot);
  slot->allocate();
  slot->out_buf= NULL;
  bpage->slot= slot;

  byte *d= slot->crypt_buf;

  if (!page_compressed)
  {
not_compressed:
    byte *tmp= space->purpose == FIL_TYPE_TEMPORARY
      ? buf_tmp_page_encrypt(bpage->id.page_no(), s, d)
      : fil_space_encrypt(space, bpage->id.page_no(), s, d);

    slot->out_buf= d= tmp;

    ut_d(fil_page_type_validate(space, tmp));
  }
  else
  {
    ut_ad(space->purpose != FIL_TYPE_TEMPORARY);
    /* First we compress the page content */
    buf_tmp_reserve_compression_buf(slot);
    byte *tmp= slot->comp_buf;
    ulint len= fil_page_compress(s, tmp, space->flags,
                                 fil_space_get_block_size(space,
                                                          bpage->id.page_no()),
                                 encrypted);

    if (!len)
      goto not_compressed;

    bpage->real_size= len;

    if (full_crc32)
    {
      ut_d(bool compressed = false);
      len= buf_page_full_crc32_size(tmp,
#ifdef UNIV_DEBUG
                                    &compressed,
#else
                                    NULL,
#endif
                                    NULL);
      ut_ad(compressed);
    }

    /* Workaround for MDEV-15527. */
    memset(tmp + len, 0 , srv_page_size - len);
    ut_d(fil_page_type_validate(space, tmp));

    if (encrypted)
      tmp = fil_space_encrypt(space, bpage->id.page_no(), tmp, d);

    if (full_crc32)
    {
      static_assert(FIL_PAGE_FCRC32_CHECKSUM == 4, "alignment");
      mach_write_to_4(tmp + len - 4, ut_crc32(tmp, len - 4));
      ut_ad(!buf_page_is_corrupted(true, tmp, space->flags));
    }

    slot->out_buf= d= tmp;
  }

  ut_d(fil_page_type_validate(space, d));
  return d;
}

/********************************************************************//**
Does an asynchronous write of a buffer page. NOTE: when the
doublewrite buffer is used, we must call
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
	fil_space_t* space = fil_space_acquire_for_io(bpage->id.space());
	if (!space) {
		return;
	}
	ut_ad(space->purpose == FIL_TYPE_TEMPORARY
	      || space->purpose == FIL_TYPE_IMPORT
	      || space->purpose == FIL_TYPE_TABLESPACE);
	ut_ad((space->purpose == FIL_TYPE_TEMPORARY)
	      == (space == fil_system.temp_space));

	page_t*	frame = NULL;
	const bool full_crc32 = space->full_crc32();

	DBUG_PRINT("ib_buf", ("flush %s %u page %u:%u",
			      sync ? "sync" : "async", (unsigned) flush_type,
			      bpage->id.space(), bpage->id.page_no()));

	ut_ad(buf_page_in_file(bpage));

	/* We are not holding buf_pool->mutex or block_mutex here.
	Nevertheless, it is safe to access bpage, because it is
	io_fixed and oldest_modification != 0.  Thus, it cannot be
	relocated in the buffer pool or removed from flush_list or
	LRU_list. */
	ut_ad(!mutex_own(&buf_pool->mutex));
	ut_ad(!mutex_own(&buf_pool->flush_list_mutex));
	ut_ad(!buf_page_get_mutex(bpage)->is_owned());
	ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_WRITE);
	ut_ad(bpage->oldest_modification != 0);

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
		ut_a(page_zip_verify_checksum(frame, bpage->zip_size()));
		break;
	case BUF_BLOCK_FILE_PAGE:
		frame = bpage->zip.data;
		if (!frame) {
			frame = ((buf_block_t*) bpage)->frame;
		}

		byte* page = reinterpret_cast<const buf_block_t*>(bpage)->frame;

		if (full_crc32) {
			page = buf_page_encrypt(space, bpage, page);
			frame = page;
		}

		buf_flush_init_for_writing(
			reinterpret_cast<const buf_block_t*>(bpage), page,
			bpage->zip.data ? &bpage->zip : NULL, full_crc32);
		break;
	}

	if (!full_crc32) {
		frame = buf_page_encrypt(space, bpage, frame);
	}

	if (UNIV_LIKELY(space->purpose == FIL_TYPE_TABLESPACE)) {
		const lsn_t lsn = mach_read_from_8(frame + FIL_PAGE_LSN);
		ut_ad(lsn);
		ut_ad(lsn >= bpage->oldest_modification);
		ut_ad(!srv_read_only_mode);
		log_write_up_to(lsn, true);
	} else {
		ut_ad(space->atomic_write_supported);
	}

	const bool use_doublewrite = !bpage->init_on_flush
		&& space->use_doublewrite();

	if (!use_doublewrite) {
		ulint	type = IORequest::WRITE;

		IORequest	request(type, bpage);

		/* TODO: pass the tablespace to fil_io() */
		fil_io(request,
		       sync, bpage->id, bpage->zip_size(), 0,
		       bpage->physical_size(),
		       frame, bpage);
	} else {
		ut_ad(!srv_read_only_mode);

		if (flush_type == BUF_FLUSH_SINGLE_PAGE) {
			buf_dblwr_write_single_page(bpage, sync);
		} else {
			ut_ad(!sync);
			buf_dblwr_add_to_batch(bpage);
		}
	}

	/* When doing single page flushing the IO is done synchronously
	and we flush the changes to disk only for the tablespace we
	are working on. */
	if (sync) {
		ut_ad(flush_type == BUF_FLUSH_SINGLE_PAGE);
		if (space->purpose != FIL_TYPE_TEMPORARY) {
			fil_flush(space);
		}

		/* The tablespace could already have been dropped,
		because fil_io(request, sync) would already have
		decremented the node->n_pending. However,
		buf_page_io_complete() only needs to look up the
		tablespace during read requests, not during writes. */
		ut_ad(buf_page_get_io_fix(bpage) == BUF_IO_WRITE);
#ifdef UNIV_DEBUG
		dberr_t err =
#endif
		/* true means we want to evict this page from the
		LRU list as well. */
		buf_page_io_complete(bpage, use_doublewrite, true);

		ut_ad(err == DB_SUCCESS);
	}

	space->release_for_io();

	/* Increment the counter of I/O operations used
	for selecting LRU policy. */
	buf_LRU_stat_inc_io();
}

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
bool buf_flush_page(buf_page_t* bpage, buf_flush_t flush_type, bool sync)
{
	BPageMutex*	block_mutex;

	ut_ad(flush_type < BUF_FLUSH_N_TYPES);
	ut_ad(mutex_own(&buf_pool->mutex));
	ut_ad(buf_page_in_file(bpage));
	ut_ad(!sync || flush_type == BUF_FLUSH_SINGLE_PAGE);

	block_mutex = buf_page_get_mutex(bpage);
	ut_ad(mutex_own(block_mutex));

	ut_ad(buf_flush_ready_for_flush(bpage, flush_type));

	bool	is_uncompressed;

	is_uncompressed = (buf_page_get_state(bpage) == BUF_BLOCK_FILE_PAGE);
	ut_ad(is_uncompressed == (block_mutex != &buf_pool->zip_mutex));

	ibool		flush;
	rw_lock_t*	rw_lock;
	bool		no_fix_count = bpage->buf_fix_count == 0;

	if (!is_uncompressed) {
		flush = TRUE;
		rw_lock = NULL;
	} else if (!(no_fix_count || flush_type == BUF_FLUSH_LIST)
		   || (!no_fix_count
		       && srv_shutdown_state <= SRV_SHUTDOWN_CLEANUP
		       && fsp_is_system_temporary(bpage->id.space()))) {
		/* This is a heuristic, to avoid expensive SX attempts. */
		/* For table residing in temporary tablespace sync is done
		using IO_FIX and so before scheduling for flush ensure that
		page is not fixed. */
		flush = FALSE;
	} else {
		rw_lock = &reinterpret_cast<buf_block_t*>(bpage)->lock;
		if (flush_type != BUF_FLUSH_LIST) {
			flush = rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE);
		} else {
			/* Will SX lock later */
			flush = TRUE;
		}
	}

	if (flush) {

		/* We are committed to flushing by the time we get here */

		buf_page_set_io_fix(bpage, BUF_IO_WRITE);

		buf_page_set_flush_type(bpage, flush_type);

		if (buf_pool->n_flush[flush_type] == 0) {
			os_event_reset(buf_pool->no_flush[flush_type]);
		}

		++buf_pool->n_flush[flush_type];
		ut_ad(buf_pool->n_flush[flush_type] != 0);

		mutex_exit(block_mutex);

		mutex_exit(&buf_pool->mutex);

		if (flush_type == BUF_FLUSH_LIST
		    && is_uncompressed
		    && !rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE)) {

			if (!fsp_is_system_temporary(bpage->id.space())) {
				/* avoiding deadlock possibility involves
				doublewrite buffer, should flush it, because
				it might hold the another block->lock. */
				buf_dblwr_flush_buffered_writes();
			} else {
				buf_dblwr_sync_datafiles();
			}

			rw_lock_sx_lock_gen(rw_lock, BUF_IO_WRITE);
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
/** Writes a flushable page asynchronously from the buffer pool to a file.
NOTE: block and LRU list mutexes must be held upon entering this function, and
they will be released by this function after flushing. This is loosely based on
buf_flush_batch() and buf_flush_page().
@param[in,out]	block		buffer control block
@return whether the page was flushed and the mutex released */
bool buf_flush_page_try(buf_block_t* block)
{
	ut_ad(mutex_own(&buf_pool->mutex));
	ut_ad(buf_block_get_state(block) == BUF_BLOCK_FILE_PAGE);
	ut_ad(buf_page_mutex_own(block));

	if (!buf_flush_ready_for_flush(&block->page, BUF_FLUSH_SINGLE_PAGE)) {
		return false;
	}

	/* The following call will release the buf_pool and block mutex. */
	return buf_flush_page(&block->page, BUF_FLUSH_SINGLE_PAGE, true);
}
# endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/** Check the page is in buffer pool and can be flushed.
@param[in]	page_id		page id
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@return true if the page can be flushed. */
static
bool
buf_flush_check_neighbor(
	const page_id_t		page_id,
	buf_flush_t		flush_type)
{
	buf_page_t*	bpage;
	bool		ret;

	ut_ad(flush_type == BUF_FLUSH_LRU
	      || flush_type == BUF_FLUSH_LIST);

	mutex_enter(&buf_pool->mutex);

	bpage = buf_page_hash_get(page_id);

	if (!bpage) {

		mutex_exit(&buf_pool->mutex);
		return(false);
	}

	ut_a(buf_page_in_file(bpage));

	/* We avoid flushing 'non-old' blocks in an LRU flush,
	because the flushed blocks are soon freed */

	ret = false;
	if (flush_type != BUF_FLUSH_LRU || buf_page_is_old(bpage)) {
		BPageMutex* block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);
		if (buf_flush_ready_for_flush(bpage, flush_type)) {
			ret = true;
		}
		mutex_exit(block_mutex);
	}
	mutex_exit(&buf_pool->mutex);

	return(ret);
}

/** Flushes to disk all flushable pages within the flush area.
@param[in]	page_id		page id
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@param[in]	n_flushed	number of pages flushed so far in this batch
@param[in]	n_to_flush	maximum number of pages we are allowed to flush
@return number of pages flushed */
static
ulint
buf_flush_try_neighbors(
	const page_id_t		page_id,
	buf_flush_t		flush_type,
	ulint			n_flushed,
	ulint			n_to_flush)
{
	ulint		i;
	ulint		low;
	ulint		high;
	ulint		count = 0;

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
	fil_space_t* space = fil_space_acquire_for_io(page_id.space());
	if (!space) {
		return 0;
	}

	if (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN
	    || !srv_flush_neighbors || !space->is_rotational()) {
		/* If there is little space or neighbor flushing is
		not enabled then just flush the victim. */
		low = page_id.page_no();
		high = page_id.page_no() + 1;
	} else {
		/* When flushed, dirty blocks are searched in
		neighborhoods of this size, and flushed along with the
		original page. */

		ulint	buf_flush_area;

		buf_flush_area	= ut_min(
			buf_pool->read_ahead_area,
			buf_pool->curr_size / 16);

		low = (page_id.page_no() / buf_flush_area) * buf_flush_area;
		high = (page_id.page_no() / buf_flush_area + 1) * buf_flush_area;

		if (srv_flush_neighbors == 1) {
			/* adjust 'low' and 'high' to limit
			   for contiguous dirty area */
			if (page_id.page_no() > low) {
				for (i = page_id.page_no() - 1; i >= low; i--) {
					if (!buf_flush_check_neighbor(
						page_id_t(page_id.space(), i),
						flush_type)) {

						break;
					}

					if (i == low) {
						/* Avoid overwrap when low == 0
						and calling
						buf_flush_check_neighbor() with
						i == (ulint) -1 */
						i--;
						break;
					}
				}
				low = i + 1;
			}

			for (i = page_id.page_no() + 1;
			     i < high
			     && buf_flush_check_neighbor(
				     page_id_t(page_id.space(), i),
				     flush_type);
			     i++) {
				/* do nothing */
			}
			high = i;
		}
	}

	if (high > space->size) {
		high = space->size;
	}

	DBUG_PRINT("ib_buf", ("flush %u:%u..%u",
			      page_id.space(),
			      (unsigned) low, (unsigned) high));

	for (ulint i = low; i < high; i++) {
		buf_page_t*	bpage;

		if ((count + n_flushed) >= n_to_flush) {

			/* We have already flushed enough pages and
			should call it a day. There is, however, one
			exception. If the page whose neighbors we
			are flushing has not been flushed yet then
			we'll try to flush the victim that we
			selected originally. */
			if (i <= page_id.page_no()) {
				i = page_id.page_no();
			} else {
				break;
			}
		}

		const page_id_t	cur_page_id(page_id.space(), i);

		mutex_enter(&buf_pool->mutex);

		bpage = buf_page_hash_get(cur_page_id);

		if (bpage == NULL) {
			mutex_exit(&buf_pool->mutex);
			continue;
		}

		ut_a(buf_page_in_file(bpage));

		/* We avoid flushing 'non-old' blocks in an LRU flush,
		because the flushed blocks are soon freed */

		if (flush_type != BUF_FLUSH_LRU
		    || i == page_id.page_no()
		    || buf_page_is_old(bpage)) {

			BPageMutex* block_mutex = buf_page_get_mutex(bpage);

			mutex_enter(block_mutex);

			if (buf_flush_ready_for_flush(bpage, flush_type)
			    && (i == page_id.page_no()
				|| bpage->buf_fix_count == 0)) {

				/* We also try to flush those
				neighbors != offset */

				if (buf_flush_page(bpage, flush_type, false)) {
					++count;
				} else {
					mutex_exit(block_mutex);
					mutex_exit(&buf_pool->mutex);
				}

				continue;
			} else {
				mutex_exit(block_mutex);
			}
		}
		mutex_exit(&buf_pool->mutex);
	}

	space->release_for_io();

	if (count > 1) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_NEIGHBOR_TOTAL_PAGE,
			MONITOR_FLUSH_NEIGHBOR_COUNT,
			MONITOR_FLUSH_NEIGHBOR_PAGES,
			(count - 1));
	}

	return(count);
}

/** Check if the block is modified and ready for flushing.
If the the block is ready to flush then flush the page and try o flush
its neighbors.
@param[in]	bpage		buffer control block,
must be buf_page_in_file(bpage)
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@param[in]	n_to_flush	number of pages to flush
@param[in,out]	count		number of pages flushed
@return TRUE if buf_pool mutex was released during this function.
This does not guarantee that some pages were written as well.
Number of pages written are incremented to the count. */
static
bool
buf_flush_page_and_try_neighbors(
	buf_page_t*		bpage,
	buf_flush_t		flush_type,
	ulint			n_to_flush,
	ulint*			count)
{
	ut_ad(mutex_own(&buf_pool->mutex));

	bool		flushed;
	BPageMutex*	block_mutex = buf_page_get_mutex(bpage);

	mutex_enter(block_mutex);

	ut_a(buf_page_in_file(bpage));

	if (buf_flush_ready_for_flush(bpage, flush_type)) {
		const page_id_t	page_id = bpage->id;

		mutex_exit(block_mutex);
		mutex_exit(&buf_pool->mutex);

		/* Try to flush also all the neighbors */
		*count += buf_flush_try_neighbors(
			page_id, flush_type, *count, n_to_flush);

		mutex_enter(&buf_pool->mutex);
		flushed = true;
	} else {
		mutex_exit(block_mutex);
		flushed = false;
	}

	ut_ad(mutex_own(&buf_pool->mutex));

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
@param[in]	max		desired number of blocks in the free_list
@return number of blocks moved to the free list. */
static ulint buf_free_from_unzip_LRU_list_batch(ulint max)
{
	ulint		scanned = 0;
	ulint		count = 0;
	ulint		free_len = UT_LIST_GET_LEN(buf_pool->free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);

	ut_ad(mutex_own(&buf_pool->mutex));

	buf_block_t*	block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);

	while (block != NULL
	       && count < max
	       && free_len < srv_LRU_scan_depth
	       && lru_len > UT_LIST_GET_LEN(buf_pool->LRU) / 10) {

		++scanned;
		if (buf_LRU_free_page(&block->page, false)) {
			/* Block was freed. buf_pool->mutex potentially
			released and reacquired */
			++count;
			block = UT_LIST_GET_LAST(buf_pool->unzip_LRU);

		} else {

			block = UT_LIST_GET_PREV(unzip_LRU, block);
		}

		free_len = UT_LIST_GET_LEN(buf_pool->free);
		lru_len = UT_LIST_GET_LEN(buf_pool->unzip_LRU);
	}

	ut_ad(mutex_own(&buf_pool->mutex));

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	return(count);
}

/** Flush dirty blocks from the end of the LRU list.
The calling thread is not allowed to own any latches on pages!

@param[in]	max	desired number of blocks to make available
			in the free list (best effort; not guaranteed)
@param[out]	n	counts of flushed and evicted pages */
static void buf_flush_LRU_list_batch(ulint max, flush_counters_t* n)
{
	buf_page_t*	bpage;
	ulint		scanned = 0;
	ulint		free_len = UT_LIST_GET_LEN(buf_pool->free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool->LRU);
	ulint		withdraw_depth = 0;

	n->flushed = 0;
	n->evicted = 0;
	n->unzip_LRU_evicted = 0;
	ut_ad(mutex_own(&buf_pool->mutex));
	if (buf_pool->curr_size < buf_pool->old_size
	    && buf_pool->withdraw_target > 0) {
		withdraw_depth = buf_pool->withdraw_target
			- UT_LIST_GET_LEN(buf_pool->withdraw);
	}

	for (bpage = UT_LIST_GET_LAST(buf_pool->LRU);
	     bpage != NULL && n->flushed + n->evicted < max
	     && free_len < srv_LRU_scan_depth + withdraw_depth
	     && lru_len > BUF_LRU_MIN_LEN;
	     ++scanned,
	     bpage = buf_pool->lru_hp.get()) {

		buf_page_t* prev = UT_LIST_GET_PREV(LRU, bpage);
		buf_pool->lru_hp.set(prev);

		BPageMutex*	block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		if (buf_flush_ready_for_replace(bpage)) {
			/* block is ready for eviction i.e., it is
			clean and is not IO-fixed or buffer fixed. */
			mutex_exit(block_mutex);
			if (buf_LRU_free_page(bpage, true)) {
				++n->evicted;
			}
		} else if (buf_flush_ready_for_flush(bpage, BUF_FLUSH_LRU)) {
			/* Block is ready for flush. Dispatch an IO
			request. The IO helper thread will put it on
			free list in IO completion routine. */
			mutex_exit(block_mutex);
			buf_flush_page_and_try_neighbors(
				bpage, BUF_FLUSH_LRU, max, &n->flushed);
		} else {
			/* Can't evict or dispatch this block. Go to
			previous. */
			ut_ad(buf_pool->lru_hp.is_hp(prev));
			mutex_exit(block_mutex);
		}

		ut_ad(!mutex_own(block_mutex));
		ut_ad(mutex_own(&buf_pool->mutex));

		free_len = UT_LIST_GET_LEN(buf_pool->free);
		lru_len = UT_LIST_GET_LEN(buf_pool->LRU);
	}

	buf_pool->lru_hp.set(NULL);

	/* We keep track of all flushes happening as part of LRU
	flush. When estimating the desired rate at which flush_list
	should be flushed, we factor in this value. */
	buf_lru_flush_page_count += n->flushed;

	ut_ad(mutex_own(&buf_pool->mutex));

	if (n->evicted) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_EVICT_TOTAL_PAGE,
			MONITOR_LRU_BATCH_EVICT_COUNT,
			MONITOR_LRU_BATCH_EVICT_PAGES,
			n->evicted);
	}

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_BATCH_SCANNED,
			MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
			MONITOR_LRU_BATCH_SCANNED_PER_CALL,
			scanned);
	}
}

/** Flush and move pages from LRU or unzip_LRU list to the free list.
Whether LRU or unzip_LRU is used depends on the state of the system.
@param[in]	max	desired number of blocks to make available
			in the free list (best effort; not guaranteed)
@param[out]	n	counts of flushed and evicted pages */
static void buf_do_LRU_batch(ulint max, flush_counters_t* n)
{
	n->unzip_LRU_evicted = buf_LRU_evict_from_unzip_LRU()
		? buf_free_from_unzip_LRU_list_batch(max) : 0;

	if (max > n->unzip_LRU_evicted) {
		buf_flush_LRU_list_batch(max - n->unzip_LRU_evicted, n);
	} else {
		n->evicted = 0;
		n->flushed = 0;
	}

	/* Add evicted pages from unzip_LRU to the evicted pages from
	the simple LRU. */
	n->evicted += n->unzip_LRU_evicted;
}

/** This utility flushes dirty blocks from the end of the flush_list.
The calling thread is not allowed to own any latches on pages!
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	all blocks whose oldest_modification is smaller
than this should be flushed (if their number does not exceed min_n)
@return number of blocks for which the write request was queued;
ULINT_UNDEFINED if there was a flush of the same type already
running */
static ulint buf_do_flush_list_batch(ulint min_n, lsn_t lsn_limit)
{
	ulint		count = 0;
	ulint		scanned = 0;

	ut_ad(mutex_own(&buf_pool->mutex));

	/* Start from the end of the list looking for a suitable
	block to be flushed. */
	mutex_enter(&buf_pool->flush_list_mutex);
	ulint len = UT_LIST_GET_LEN(buf_pool->flush_list);

	/* In order not to degenerate this scan to O(n*n) we attempt
	to preserve pointer of previous block in the flush list. To do
	so we declare it a hazard pointer. Any thread working on the
	flush list must check the hazard pointer and if it is removing
	the same block then it must reset it. */
	for (buf_page_t* bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
	     count < min_n && bpage != NULL && len > 0
	     && bpage->oldest_modification < lsn_limit;
	     bpage = buf_pool->flush_hp.get(),
	     ++scanned) {

		buf_page_t*	prev;

		ut_a(bpage->oldest_modification > 0);
		ut_ad(bpage->in_flush_list);

		prev = UT_LIST_GET_PREV(list, bpage);
		buf_pool->flush_hp.set(prev);
		mutex_exit(&buf_pool->flush_list_mutex);

#ifdef UNIV_DEBUG
		bool flushed =
#endif /* UNIV_DEBUG */
		buf_flush_page_and_try_neighbors(
			bpage, BUF_FLUSH_LIST, min_n, &count);

		mutex_enter(&buf_pool->flush_list_mutex);

		ut_ad(flushed || buf_pool->flush_hp.is_hp(prev));

		--len;
	}

	buf_pool->flush_hp.set(NULL);
	mutex_exit(&buf_pool->flush_list_mutex);

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_BATCH_SCANNED,
			MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
			MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
			scanned);
	}

	if (count) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_BATCH_TOTAL_PAGE,
			MONITOR_FLUSH_BATCH_COUNT,
			MONITOR_FLUSH_BATCH_PAGES,
			count);
	}

	ut_ad(mutex_own(&buf_pool->mutex));

	return(count);
}

/** This utility flushes dirty blocks from the end of the LRU list or
flush_list.
NOTE 1: in the case of an LRU flush the calling thread may own latches to
pages: to avoid deadlocks, this function must be written so that it cannot
end up waiting for these latches! NOTE 2: in the case of a flush list flush,
the calling thread is not allowed to own any latches on pages!
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST; if
BUF_FLUSH_LIST, then the caller must not own any latches on pages
@param[in]	min_n		wished minimum mumber of blocks flushed (it is
not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case of BUF_FLUSH_LIST all blocks whose
@param[out]	n		counts of flushed and evicted pages
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored */
static
void
buf_flush_batch(
	buf_flush_t		flush_type,
	ulint			min_n,
	lsn_t			lsn_limit,
	flush_counters_t*	n)
{
	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);
	ut_ad(flush_type == BUF_FLUSH_LRU
	      || !sync_check_iterate(dict_sync_check()));

	mutex_enter(&buf_pool->mutex);

	/* Note: The buffer pool mutex is released and reacquired within
	the flush functions. */
	switch (flush_type) {
	case BUF_FLUSH_LRU:
		buf_do_LRU_batch(min_n, n);
		break;
	case BUF_FLUSH_LIST:
		n->flushed = buf_do_flush_list_batch(min_n, lsn_limit);
		n->evicted = 0;
		break;
	default:
		ut_error;
	}

	mutex_exit(&buf_pool->mutex);

	DBUG_LOG("ib_buf", "flush " << flush_type << " completed");
}

/******************************************************************//**
Gather the aggregated stats for both flush list and LRU list flushing.
@param page_count_flush	number of pages flushed from the end of the flush_list
@param page_count_LRU	number of pages flushed from the end of the LRU list
*/
static
void
buf_flush_stats(
/*============*/
	ulint		page_count_flush,
	ulint		page_count_LRU)
{
	DBUG_PRINT("ib_buf", ("flush completed, from flush_list %u pages, "
			      "from LRU_list %u pages",
			      unsigned(page_count_flush),
			      unsigned(page_count_LRU)));

	srv_stats.buf_pool_flushed.add(page_count_flush + page_count_LRU);
}

/** Start a buffer flush batch for LRU or flush list
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST
@return	whether the flush batch was started (was not already running) */
bool buf_flush_start(buf_flush_t flush_type)
{
	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

	mutex_enter(&buf_pool->mutex);

	if (buf_pool->n_flush[flush_type] > 0
	   || buf_pool->init_flush[flush_type] == TRUE) {

		/* There is already a flush batch of the same type running */

		mutex_exit(&buf_pool->mutex);

		return(false);
	}

	buf_pool->init_flush[flush_type] = TRUE;

	os_event_reset(buf_pool->no_flush[flush_type]);

	mutex_exit(&buf_pool->mutex);

	return(true);
}

/** End a buffer flush batch.
@param[in]	flush_type	BUF_FLUSH_LRU or BUF_FLUSH_LIST */
void buf_flush_end(buf_flush_t flush_type)
{
	mutex_enter(&buf_pool->mutex);

	buf_pool->init_flush[flush_type] = FALSE;

	buf_pool->try_LRU_scan = TRUE;

	if (buf_pool->n_flush[flush_type] == 0) {

		/* The running flush batch has ended */

		os_event_set(buf_pool->no_flush[flush_type]);
	}

	mutex_exit(&buf_pool->mutex);

	if (!srv_read_only_mode) {
		buf_dblwr_flush_buffered_writes();
	}
}

/** Wait until a flush batch ends.
@param[in]	type	BUF_FLUSH_LRU or BUF_FLUSH_LIST */
void buf_flush_wait_batch_end(buf_flush_t type)
{
	ut_ad(type == BUF_FLUSH_LRU || type == BUF_FLUSH_LIST);
	thd_wait_begin(NULL, THD_WAIT_DISKIO);
	os_event_wait(buf_pool->no_flush[type]);
	thd_wait_end(NULL);
}

/** Do flushing batch of a given type.
NOTE: The calling thread is not allowed to own any latches on pages!
@param[in]	type		flush type
@param[in]	min_n		wished minimum mumber of blocks flushed
(it is not guaranteed that the actual number is that big, though)
@param[in]	lsn_limit	in the case BUF_FLUSH_LIST all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL
@retval true	if a batch was queued successfully.
@retval false	if another batch of same type was already running. */
bool
buf_flush_do_batch(
	buf_flush_t		type,
	ulint			min_n,
	lsn_t			lsn_limit,
	flush_counters_t*	n)
{
	ut_ad(type == BUF_FLUSH_LRU || type == BUF_FLUSH_LIST);

	if (n != NULL) {
		n->flushed = 0;
	}

	if (!buf_flush_start(type)) {
		return(false);
	}

	buf_flush_batch(type, min_n, lsn_limit, n);
	buf_flush_end(type);

	return(true);
}
/** Wait until a flush batch of the given lsn ends
@param[in]	new_oldest	target oldest_modified_lsn to wait for */
void buf_flush_wait_flushed(lsn_t new_oldest)
{
	for (;;) {
		/* We don't need to wait for fsync of the flushed
		blocks, because anyway we need fsync to make chekpoint.
		So, we don't need to wait for the batch end here. */

		mutex_enter(&buf_pool->flush_list_mutex);

		buf_page_t*	bpage;

		/* FIXME: Keep temporary tablespace pages in a separate flush
		list. We would only need to write out temporary pages if the
		page is about to be evicted from the buffer pool, and the page
		contents is still needed (the page has not been freed). */
		for (bpage = UT_LIST_GET_LAST(buf_pool->flush_list);
		     bpage && fsp_is_system_temporary(bpage->id.space());
		     bpage = UT_LIST_GET_PREV(list, bpage)) {
			ut_ad(bpage->in_flush_list);
		}

		lsn_t oldest = bpage ? bpage->oldest_modification : 0;

		mutex_exit(&buf_pool->flush_list_mutex);

		if (oldest == 0 || oldest >= new_oldest) {
			break;
		}

		/* sleep and retry */
		os_thread_sleep(buf_flush_wait_flushed_sleep_time);

		MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
	}
}

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
bool buf_flush_lists(ulint min_n, lsn_t lsn_limit, ulint *n_processed)
{
	flush_counters_t	n;

	bool success = buf_flush_do_batch(
		BUF_FLUSH_LIST, min_n, lsn_limit, &n);

	if (n.flushed) {
		buf_flush_stats(n.flushed, 0);
	}

	if (n_processed) {
		*n_processed = n.flushed;
	}

	return success;
}

/******************************************************************//**
This function picks up a single page from the tail of the LRU
list, flushes it (if it is dirty), removes it from page_hash and LRU
list and puts it on the free list. It is called from user threads when
they are unable to find a replaceable page at the tail of the LRU
list i.e.: when the background LRU flushing in the page_cleaner thread
is not fast enough to keep pace with the workload.
@return true if success. */
bool buf_flush_single_page_from_LRU()
{
	ulint		scanned;
	buf_page_t*	bpage;
	ibool		freed;

	mutex_enter(&buf_pool->mutex);

	for (bpage = buf_pool->single_scan_itr.start(), scanned = 0,
	     freed = false;
	     bpage != NULL;
	     ++scanned, bpage = buf_pool->single_scan_itr.get()) {

		ut_ad(mutex_own(&buf_pool->mutex));

		buf_page_t*	prev = UT_LIST_GET_PREV(LRU, bpage);
		buf_pool->single_scan_itr.set(prev);
		BPageMutex*	block_mutex;

		block_mutex = buf_page_get_mutex(bpage);

		mutex_enter(block_mutex);

		if (buf_flush_ready_for_replace(bpage)) {
			/* block is ready for eviction i.e., it is
			clean and is not IO-fixed or buffer fixed. */
			mutex_exit(block_mutex);

			if (buf_LRU_free_page(bpage, true)) {
				mutex_exit(&buf_pool->mutex);
				freed = true;
				break;
			}

		} else if (buf_flush_ready_for_flush(
				   bpage, BUF_FLUSH_SINGLE_PAGE)) {

			/* Block is ready for flush. Try and dispatch an IO
			request. We'll put it on free list in IO completion
			routine if it is not buffer fixed. The following call
			will release the buffer pool and block mutex.

			Note: There is no guarantee that this page has actually
			been freed, only that it has been flushed to disk */

			freed = buf_flush_page(bpage, BUF_FLUSH_SINGLE_PAGE,
					       true);

			if (freed) {
				break;
			}

			mutex_exit(block_mutex);
		} else {
			mutex_exit(block_mutex);
		}
		ut_ad(!mutex_own(block_mutex));
	}
	if (!freed) {
		/* Can't find a single flushable page. */
		ut_ad(!bpage);
		mutex_exit(&buf_pool->mutex);
	}

	if (scanned) {
		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_LRU_SINGLE_FLUSH_SCANNED,
			MONITOR_LRU_SINGLE_FLUSH_SCANNED_NUM_CALL,
			MONITOR_LRU_SINGLE_FLUSH_SCANNED_PER_CALL,
			scanned);
	}

	ut_ad(!mutex_own(&buf_pool->mutex));
	return(freed);
}

/**
Clear up the tail of the LRU list.
Put replaceable pages at the tail of LRU to the free list.
Flush dirty pages at the tail of LRU to the disk.
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return total pages flushed */
static ulint buf_flush_LRU_list()
{
	ulint	scan_depth, withdraw_depth;
	flush_counters_t	n;

	memset(&n, 0, sizeof(flush_counters_t));

	/* srv_LRU_scan_depth can be arbitrarily large value.
	We cap it with current LRU size. */
	mutex_enter(&buf_pool->mutex);
	scan_depth = UT_LIST_GET_LEN(buf_pool->LRU);
	if (buf_pool->curr_size < buf_pool->old_size
	    && buf_pool->withdraw_target > 0) {
		withdraw_depth = buf_pool->withdraw_target
			- UT_LIST_GET_LEN(buf_pool->withdraw);
	} else {
		withdraw_depth = 0;
	}
	mutex_exit(&buf_pool->mutex);
	if (withdraw_depth > srv_LRU_scan_depth) {
		scan_depth = ut_min(withdraw_depth, scan_depth);
	} else {
		scan_depth = ut_min(static_cast<ulint>(srv_LRU_scan_depth),
				    scan_depth);
	}
	/* Currently one of page_cleaners is the only thread
	that can trigger an LRU flush at the same time.
	So, it is not possible that a batch triggered during
	last iteration is still running, */
	buf_flush_do_batch(BUF_FLUSH_LRU, scan_depth, 0, &n);

	return(n.flushed);
}

/** Wait for any possible LRU flushes to complete. */
void buf_flush_wait_LRU_batch_end()
{
	mutex_enter(&buf_pool->mutex);
	bool wait = buf_pool->n_flush[BUF_FLUSH_LRU]
		|| buf_pool->init_flush[BUF_FLUSH_LRU];
	mutex_exit(&buf_pool->mutex);
	if (wait) {
		buf_flush_wait_batch_end(BUF_FLUSH_LRU);
	}
}

/*********************************************************************//**
Calculates if flushing is required based on number of dirty pages in
the buffer pool.
@return percent of io_capacity to flush to manage dirty page ratio */
static
ulint
af_get_pct_for_dirty()
{
	const ulint dirty = UT_LIST_GET_LEN(buf_pool->flush_list);
	if (!dirty) {
		/* No pages modified */
		return 0;
	}

	/* 1 + is there to avoid division by zero (in case the buffer
	pool (including the flush_list) was emptied while we are
	looking at it) */
	double	dirty_pct = double(100 * dirty)
		/ (1 + UT_LIST_GET_LEN(buf_pool->LRU)
		   + UT_LIST_GET_LEN(buf_pool->free));

	ut_a(srv_max_dirty_pages_pct_lwm
	     <= srv_max_buf_pool_modified_pct);

	if (srv_max_dirty_pages_pct_lwm == 0) {
		/* The user has not set the option to preflush dirty
		pages as we approach the high water mark. */
		if (dirty_pct >= srv_max_buf_pool_modified_pct) {
			/* We have crossed the high water mark of dirty
			pages In this case we start flushing at 100% of
			innodb_io_capacity. */
			return(100);
		}
	} else if (dirty_pct >= srv_max_dirty_pages_pct_lwm) {
		/* We should start flushing pages gradually. */
		return(static_cast<ulint>((dirty_pct * 100)
		       / (srv_max_buf_pool_modified_pct + 1)));
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
	return(static_cast<ulint>(
		((srv_max_io_capacity / srv_io_capacity)
		* (lsn_age_factor * sqrt((double)lsn_age_factor)))
		/ 7.5));
}

/*********************************************************************//**
This function is called approximately once every second by the
page_cleaner thread. Based on various factors it decides if there is a
need to do flushing.
@return number of pages recommended to be flushed
@param last_pages_in	the number of pages flushed by the last flush_list
			flushing. */
static
ulint
page_cleaner_flush_pages_recommendation(ulint last_pages_in)
{
	static	lsn_t		prev_lsn = 0;
	static	ulint		sum_pages = 0;
	static	ulint		avg_page_rate = 0;
	static	ulint		n_iterations = 0;
	static	time_t		prev_time;
	lsn_t			oldest_lsn;
	lsn_t			cur_lsn;
	lsn_t			age;
	lsn_t			lsn_rate;
	ulint			n_pages = 0;
	ulint			pct_for_dirty = 0;
	ulint			pct_for_lsn = 0;
	ulint			pct_total = 0;

	cur_lsn = log_sys.get_lsn();

	if (prev_lsn == 0) {
		/* First time around. */
		prev_lsn = cur_lsn;
		prev_time = time(NULL);
		return(0);
	}

	if (prev_lsn == cur_lsn) {
		return(0);
	}

	sum_pages += last_pages_in;

	time_t	curr_time = time(NULL);
	double	time_elapsed = difftime(curr_time, prev_time);

	/* We update our variables every srv_flushing_avg_loops
	iterations to smooth out transition in workload. */
	if (++n_iterations >= srv_flushing_avg_loops
	    || time_elapsed >= srv_flushing_avg_loops) {

		if (time_elapsed < 1) {
			time_elapsed = 1;
		}

		avg_page_rate = static_cast<ulint>(
			((static_cast<double>(sum_pages)
			  / time_elapsed)
			 + avg_page_rate) / 2);

		/* How much LSN we have generated since last call. */
		lsn_rate = static_cast<lsn_t>(
			static_cast<double>(cur_lsn - prev_lsn)
			/ time_elapsed);

		lsn_avg_rate = (lsn_avg_rate + lsn_rate) / 2;

		/* aggregate stats of all slots */
		mutex_enter(&page_cleaner.mutex);

		ulint	flush_tm = page_cleaner.flush_time;
		ulint	flush_pass = page_cleaner.flush_pass;

		page_cleaner.flush_time = 0;
		page_cleaner.flush_pass = 0;

		ulint	lru_tm = page_cleaner.slot.flush_lru_time;
		ulint	list_tm = page_cleaner.slot.flush_list_time;
		ulint	lru_pass = page_cleaner.slot.flush_lru_pass;
		ulint	list_pass = page_cleaner.slot.flush_list_pass;
		page_cleaner.slot.flush_lru_time  = 0;
		page_cleaner.slot.flush_lru_pass  = 0;
		page_cleaner.slot.flush_list_time = 0;
		page_cleaner.slot.flush_list_pass = 0;
		mutex_exit(&page_cleaner.mutex);

		/* minimum values are 1, to avoid dividing by zero. */
		if (lru_tm < 1) {
			lru_tm = 1;
		}
		if (list_tm < 1) {
			list_tm = 1;
		}
		if (flush_tm < 1) {
			flush_tm = 1;
		}

		if (lru_pass < 1) {
			lru_pass = 1;
		}
		if (list_pass < 1) {
			list_pass = 1;
		}
		if (flush_pass < 1) {
			flush_pass = 1;
		}

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME_SLOT,
			    list_tm / list_pass);
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_TIME_SLOT,
			    lru_tm  / lru_pass);

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME_THREAD,
			    list_tm / flush_pass);
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_TIME_THREAD,
			    lru_tm / flush_pass);
		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_TIME_EST,
			    flush_tm * list_tm / flush_pass
			    / (list_tm + lru_tm));
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_TIME_EST,
			    flush_tm * lru_tm / flush_pass
			    / (list_tm + lru_tm));
		MONITOR_SET(MONITOR_FLUSH_AVG_TIME, flush_tm / flush_pass);

		MONITOR_SET(MONITOR_FLUSH_ADAPTIVE_AVG_PASS, list_pass);
		MONITOR_SET(MONITOR_LRU_BATCH_FLUSH_AVG_PASS, lru_pass);
		MONITOR_SET(MONITOR_FLUSH_AVG_PASS, flush_pass);

		prev_lsn = cur_lsn;
		prev_time = curr_time;

		n_iterations = 0;

		sum_pages = 0;
	}

	oldest_lsn = buf_pool_get_oldest_modification();

	ut_ad(oldest_lsn <= log_get_lsn());

	age = cur_lsn > oldest_lsn ? cur_lsn - oldest_lsn : 0;

	pct_for_dirty = af_get_pct_for_dirty();
	pct_for_lsn = af_get_pct_for_lsn(age);

	pct_total = ut_max(pct_for_dirty, pct_for_lsn);

	/* Estimate pages to be flushed for the lsn progress */
	lsn_t	target_lsn = oldest_lsn
		+ lsn_avg_rate * buf_flush_lsn_scan_factor;
	ulint	pages_for_lsn = 0;

	mutex_enter(&buf_pool->flush_list_mutex);
	for (buf_page_t* b = UT_LIST_GET_LAST(buf_pool->flush_list);
	     b != NULL;
	     b = UT_LIST_GET_PREV(list, b)) {
		if (b->oldest_modification > target_lsn) {
			break;
		}
		++pages_for_lsn;
	}
	mutex_exit(&buf_pool->flush_list_mutex);

	mutex_enter(&page_cleaner.mutex);
	ut_ad(page_cleaner.slot.state == PAGE_CLEANER_STATE_NONE);
	page_cleaner.slot.n_pages_requested
		= pages_for_lsn / buf_flush_lsn_scan_factor + 1;
	mutex_exit(&page_cleaner.mutex);

	pages_for_lsn /= buf_flush_lsn_scan_factor;
	if (pages_for_lsn < 1) {
		pages_for_lsn = 1;
	}

	/* Cap the maximum IO capacity that we are going to use by
	max_io_capacity. Limit the value to avoid too quick increase */
	pages_for_lsn = std::min<ulint>(
		pages_for_lsn, srv_max_io_capacity * 2);

	n_pages = (PCT_IO(pct_total) + avg_page_rate + pages_for_lsn) / 3;

	if (n_pages > srv_max_io_capacity) {
		n_pages = srv_max_io_capacity;
	}

	mutex_enter(&page_cleaner.mutex);
	ut_ad(page_cleaner.n_slots_requested == 0);
	ut_ad(page_cleaner.n_slots_flushing == 0);
	ut_ad(page_cleaner.n_slots_finished == 0);

	/* if REDO has enough of free space,
	don't care about age distribution of pages */
	if (pct_for_lsn > 30) {
		page_cleaner.slot.n_pages_requested *= n_pages
			/ pages_for_lsn + 1;
	} else {
		page_cleaner.slot.n_pages_requested = n_pages;
	}
	mutex_exit(&page_cleaner.mutex);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_REQUESTED, n_pages);

	MONITOR_SET(MONITOR_FLUSH_N_TO_FLUSH_BY_AGE, pages_for_lsn);

	MONITOR_SET(MONITOR_FLUSH_AVG_PAGE_RATE, avg_page_rate);
	MONITOR_SET(MONITOR_FLUSH_LSN_AVG_RATE, lsn_avg_rate);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_DIRTY, pct_for_dirty);
	MONITOR_SET(MONITOR_FLUSH_PCT_FOR_LSN, pct_for_lsn);

	return(n_pages);
}

/*********************************************************************//**
Puts the page_cleaner thread to sleep if it has finished work in less
than a second
@retval 0 wake up by event set,
@retval OS_SYNC_TIME_EXCEEDED if timeout was exceeded
@param next_loop_time	time when next loop iteration should start
@param sig_count	zero or the value returned by previous call of
			os_event_reset()
@param cur_time		current time as in ut_time_ms() */
static
ulint
pc_sleep_if_needed(
/*===============*/
	ulint		next_loop_time,
	int64_t		sig_count,
	ulint		cur_time)
{
	/* No sleep if we are cleaning the buffer pool during the shutdown
	with everything else finished */
	if (srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE)
		return OS_SYNC_TIME_EXCEEDED;

	if (next_loop_time > cur_time) {
		/* Get sleep interval in micro seconds. We use
		ut_min() to avoid long sleep in case of wrap around. */
		ulint	sleep_us;

		sleep_us = ut_min(static_cast<ulint>(1000000),
				  (next_loop_time - cur_time) * 1000);

		return(os_event_wait_time_low(buf_flush_event,
					      sleep_us, sig_count));
	}

	return(OS_SYNC_TIME_EXCEEDED);
}

/**
Requests for all slots to flush.
@param min_n	wished minimum mumber of blocks flushed
		(it is not guaranteed that the actual number is that big)
@param lsn_limit in the case BUF_FLUSH_LIST all blocks whose
		oldest_modification is smaller than this should be flushed
		(if their number does not exceed min_n), otherwise ignored
*/
static void pc_request(ulint min_n, lsn_t lsn_limit)
{
	mutex_enter(&page_cleaner.mutex);

	ut_ad(page_cleaner.n_slots_requested == 0);
	ut_ad(page_cleaner.n_slots_flushing == 0);
	ut_ad(page_cleaner.n_slots_finished == 0);

	page_cleaner.requested = (min_n > 0);
	page_cleaner.lsn_limit = lsn_limit;

	ut_ad(page_cleaner.slot.state == PAGE_CLEANER_STATE_NONE);

	if (min_n == 0 || min_n == ULINT_MAX) {
		page_cleaner.slot.n_pages_requested = min_n;
	}

	/* page_cleaner.slot.n_pages_requested was already set by
	page_cleaner_flush_pages_recommendation() */

	page_cleaner.slot.state = PAGE_CLEANER_STATE_REQUESTED;

	page_cleaner.n_slots_requested = 1;
	page_cleaner.n_slots_flushing = 0;
	page_cleaner.n_slots_finished = 0;

	mutex_exit(&page_cleaner.mutex);
}

/**
Do flush for one slot.
@return	the number of the slots which has not been treated yet. */
static ulint pc_flush_slot()
{
	ulint	lru_tm = 0;
	ulint	list_tm = 0;
	ulint	lru_pass = 0;
	ulint	list_pass = 0;

	mutex_enter(&page_cleaner.mutex);

	if (page_cleaner.n_slots_requested) {
		ut_ad(page_cleaner.slot.state == PAGE_CLEANER_STATE_REQUESTED);
		page_cleaner.n_slots_requested--;
		page_cleaner.n_slots_flushing++;
		page_cleaner.slot.state = PAGE_CLEANER_STATE_FLUSHING;

		if (UNIV_UNLIKELY(!page_cleaner.is_running)) {
			page_cleaner.slot.n_flushed_lru = 0;
			page_cleaner.slot.n_flushed_list = 0;
			goto finish_mutex;
		}

		mutex_exit(&page_cleaner.mutex);

		lru_tm = ut_time_ms();

		/* Flush pages from end of LRU if required */
		page_cleaner.slot.n_flushed_lru = buf_flush_LRU_list();

		lru_tm = ut_time_ms() - lru_tm;
		lru_pass++;

		if (UNIV_UNLIKELY(!page_cleaner.is_running)) {
			page_cleaner.slot.n_flushed_list = 0;
			goto finish;
		}

		/* Flush pages from flush_list if required */
		if (page_cleaner.requested) {
			flush_counters_t n;
			memset(&n, 0, sizeof(flush_counters_t));
			list_tm = ut_time_ms();

			page_cleaner.slot.succeeded_list = buf_flush_do_batch(
				BUF_FLUSH_LIST,
				page_cleaner.slot.n_pages_requested,
				page_cleaner.lsn_limit,
				&n);

			page_cleaner.slot.n_flushed_list = n.flushed;

			list_tm = ut_time_ms() - list_tm;
			list_pass++;
		} else {
			page_cleaner.slot.n_flushed_list = 0;
			page_cleaner.slot.succeeded_list = true;
		}
finish:
		mutex_enter(&page_cleaner.mutex);
finish_mutex:
		page_cleaner.n_slots_flushing--;
		page_cleaner.n_slots_finished++;
		page_cleaner.slot.state = PAGE_CLEANER_STATE_FINISHED;

		page_cleaner.slot.flush_lru_time += lru_tm;
		page_cleaner.slot.flush_list_time += list_tm;
		page_cleaner.slot.flush_lru_pass += lru_pass;
		page_cleaner.slot.flush_list_pass += list_pass;

		if (page_cleaner.n_slots_requested == 0
		    && page_cleaner.n_slots_flushing == 0) {
			os_event_set(page_cleaner.is_finished);
		}
	}

	ulint	ret = page_cleaner.n_slots_requested;

	mutex_exit(&page_cleaner.mutex);

	return(ret);
}

/**
Wait until all flush requests are finished.
@param n_flushed_lru	number of pages flushed from the end of the LRU list.
@param n_flushed_list	number of pages flushed from the end of the
			flush_list.
@return			true if all flush_list flushing batch were success. */
static
bool
pc_wait_finished(
	ulint*	n_flushed_lru,
	ulint*	n_flushed_list)
{
	bool	all_succeeded = true;

	*n_flushed_lru = 0;
	*n_flushed_list = 0;

	os_event_wait(page_cleaner.is_finished);

	mutex_enter(&page_cleaner.mutex);

	ut_ad(page_cleaner.n_slots_requested == 0);
	ut_ad(page_cleaner.n_slots_flushing == 0);
	ut_ad(page_cleaner.n_slots_finished == 1);

	ut_ad(page_cleaner.slot.state == PAGE_CLEANER_STATE_FINISHED);
	page_cleaner.slot.state = PAGE_CLEANER_STATE_NONE;
	*n_flushed_lru = page_cleaner.slot.n_flushed_lru;
	*n_flushed_list = page_cleaner.slot.n_flushed_list;
	all_succeeded = page_cleaner.slot.succeeded_list;
	page_cleaner.slot.n_pages_requested = 0;

	page_cleaner.n_slots_finished = 0;

	os_event_reset(page_cleaner.is_finished);

	mutex_exit(&page_cleaner.mutex);

	return(all_succeeded);
}

#ifdef UNIV_LINUX
/**
Set priority for page_cleaner threads.
@param[in]	priority	priority intended to set
@return	true if set as intended */
static
bool
buf_flush_page_cleaner_set_priority(
	int	priority)
{
	setpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid),
		    priority);
	return(getpriority(PRIO_PROCESS, (pid_t)syscall(SYS_gettid))
	       == priority);
}
#endif /* UNIV_LINUX */

#ifdef UNIV_DEBUG
/** Loop used to disable the page cleaner thread. */
static void buf_flush_page_cleaner_disabled_loop()
{
	while (innodb_page_cleaner_disabled_debug
	       && srv_shutdown_state == SRV_SHUTDOWN_NONE
	       && page_cleaner.is_running) {
		os_thread_sleep(100000);
	}
}
#endif /* UNIV_DEBUG */

/******************************************************************//**
page_cleaner thread tasked with flushing dirty pages from the buffer
pools. As of now we'll have only one coordinator.
@return a dummy parameter */
static os_thread_ret_t DECLARE_THREAD(buf_flush_page_cleaner)(void*)
{
	my_thread_init();
#ifdef UNIV_PFS_THREAD
	pfs_register_thread(page_cleaner_thread_key);
#endif /* UNIV_PFS_THREAD */
	ut_ad(!srv_read_only_mode);

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "page_cleaner thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */
#ifdef UNIV_LINUX
	/* linux might be able to set different setting for each thread.
	worth to try to set high priority for page cleaner threads */
	if (buf_flush_page_cleaner_set_priority(
		buf_flush_page_cleaner_priority)) {

		ib::info() << "page_cleaner coordinator priority: "
			<< buf_flush_page_cleaner_priority;
	} else {
		ib::info() << "If the mysqld execution user is authorized,"
		" page cleaner thread priority can be changed."
		" See the man page of setpriority().";
	}
	/* Signal that setpriority() has been attempted. */
	os_event_set(recv_sys.flush_end);
#endif /* UNIV_LINUX */

	do {
		/* treat flushing requests during recovery. */
		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;

		os_event_wait(recv_sys.flush_start);

		if (!recv_writer_thread_active) {
			break;
		}

		switch (recv_sys.flush_type) {
		case BUF_FLUSH_LRU:
			/* Flush pages from end of LRU if required */
			pc_request(0, LSN_MAX);
			while (pc_flush_slot() > 0) {}
			pc_wait_finished(&n_flushed_lru, &n_flushed_list);
			break;

		case BUF_FLUSH_LIST:
			/* Flush all pages */
			do {
				pc_request(ULINT_MAX, LSN_MAX);
				while (pc_flush_slot() > 0) {}
			} while (!pc_wait_finished(&n_flushed_lru,
						   &n_flushed_list));
			break;

		default:
			ut_ad(0);
		}

		os_event_reset(recv_sys.flush_start);
		os_event_set(recv_sys.flush_end);
	} while (recv_writer_thread_active);

	os_event_wait(buf_flush_event);

	ulint	ret_sleep = 0;
	ulint	n_evicted = 0;
	ulint	n_flushed_last = 0;
	ulint	warn_interval = 1;
	ulint	warn_count = 0;
	int64_t	sig_count = os_event_reset(buf_flush_event);
	ulint	next_loop_time = ut_time_ms() + 1000;
	ulint	n_flushed = 0;
	ulint	last_activity = srv_get_activity_count();
	ulint	last_pages = 0;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		ulint	curr_time = ut_time_ms();

		/* The page_cleaner skips sleep if the server is
		idle and there are no pending IOs in the buffer pool
		and there is work to do. */
		if (!n_flushed || !buf_pool->n_pend_reads
		    || srv_check_activity(last_activity)) {

			ret_sleep = pc_sleep_if_needed(
				next_loop_time, sig_count, curr_time);
		} else if (curr_time > next_loop_time) {
			ret_sleep = OS_SYNC_TIME_EXCEEDED;
		} else {
			ret_sleep = 0;
		}

		if (srv_shutdown_state != SRV_SHUTDOWN_NONE) {
			break;
		}

		sig_count = os_event_reset(buf_flush_event);

		if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
			if (global_system_variables.log_warnings > 2
			    && curr_time > next_loop_time + 3000
			    && !(test_flags & TEST_SIGINT)) {
				if (warn_count == 0) {
					ib::info() << "page_cleaner: 1000ms"
						" intended loop took "
						<< 1000 + curr_time
						   - next_loop_time
						<< "ms. The settings might not"
						" be optimal. (flushed="
						<< n_flushed_last
						<< " and evicted="
						<< n_evicted
						<< ", during the time.)";
					if (warn_interval > 300) {
						warn_interval = 600;
					} else {
						warn_interval *= 2;
					}

					warn_count = warn_interval;
				} else {
					--warn_count;
				}
			} else {
				/* reset counter */
				warn_interval = 1;
				warn_count = 0;
			}

			next_loop_time = curr_time + 1000;
			n_flushed_last = n_evicted = 0;
		}

		if (ret_sleep != OS_SYNC_TIME_EXCEEDED
		    && srv_flush_sync
		    && buf_flush_sync_lsn > 0) {
			/* woke up for flush_sync */
			mutex_enter(&page_cleaner.mutex);
			lsn_t	lsn_limit = buf_flush_sync_lsn;
			buf_flush_sync_lsn = 0;
			mutex_exit(&page_cleaner.mutex);

			/* Request flushing for threads */
			pc_request(ULINT_MAX, lsn_limit);

			ulint tm = ut_time_ms();

			/* Coordinator also treats requests */
			while (pc_flush_slot() > 0) {}

			/* only coordinator is using these counters,
			so no need to protect by lock. */
			page_cleaner.flush_time += ut_time_ms() - tm;
			page_cleaner.flush_pass++;

			/* Wait for all slots to be finished */
			ulint	n_flushed_lru = 0;
			ulint	n_flushed_list = 0;
			pc_wait_finished(&n_flushed_lru, &n_flushed_list);

			if (n_flushed_list > 0 || n_flushed_lru > 0) {
				buf_flush_stats(n_flushed_list, n_flushed_lru);

				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_SYNC_TOTAL_PAGE,
					MONITOR_FLUSH_SYNC_COUNT,
					MONITOR_FLUSH_SYNC_PAGES,
					n_flushed_lru + n_flushed_list);
			}

			n_flushed = n_flushed_lru + n_flushed_list;

		} else if (srv_check_activity(last_activity)) {
			ulint	n_to_flush;
			lsn_t	lsn_limit;

			/* Estimate pages from flush_list to be flushed */
			if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
				last_activity = srv_get_activity_count();
				n_to_flush =
					page_cleaner_flush_pages_recommendation(
						last_pages);
				lsn_limit = LSN_MAX;
			} else {
				n_to_flush = 0;
				lsn_limit = 0;
			}

			/* Request flushing for threads */
			pc_request(n_to_flush, lsn_limit);

			ulint tm = ut_time_ms();

			/* Coordinator also treats requests */
			while (pc_flush_slot() > 0) {
				/* No op */
			}

			/* only coordinator is using these counters,
			so no need to protect by lock. */
			page_cleaner.flush_time += ut_time_ms() - tm;
			page_cleaner.flush_pass++ ;

			/* Wait for all slots to be finished */
			ulint	n_flushed_lru = 0;
			ulint	n_flushed_list = 0;

			pc_wait_finished(&n_flushed_lru, &n_flushed_list);

			if (n_flushed_list > 0 || n_flushed_lru > 0) {
				buf_flush_stats(n_flushed_list, n_flushed_lru);
			}

			if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
				last_pages = n_flushed_list;
			}

			n_evicted += n_flushed_lru;
			n_flushed_last += n_flushed_list;

			n_flushed = n_flushed_lru + n_flushed_list;

			if (n_flushed_lru) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_LRU_BATCH_FLUSH_TOTAL_PAGE,
					MONITOR_LRU_BATCH_FLUSH_COUNT,
					MONITOR_LRU_BATCH_FLUSH_PAGES,
					n_flushed_lru);
			}

			if (n_flushed_list) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_ADAPTIVE_TOTAL_PAGE,
					MONITOR_FLUSH_ADAPTIVE_COUNT,
					MONITOR_FLUSH_ADAPTIVE_PAGES,
					n_flushed_list);
			}

		} else if (ret_sleep == OS_SYNC_TIME_EXCEEDED) {
			/* no activity, slept enough */
			buf_flush_lists(PCT_IO(100), LSN_MAX, &n_flushed);

			n_flushed_last += n_flushed;

			if (n_flushed) {
				MONITOR_INC_VALUE_CUMULATIVE(
					MONITOR_FLUSH_BACKGROUND_TOTAL_PAGE,
					MONITOR_FLUSH_BACKGROUND_COUNT,
					MONITOR_FLUSH_BACKGROUND_PAGES,
					n_flushed);

			}

		} else {
			/* no activity, but woken up by event */
			n_flushed = 0;
		}

		ut_d(buf_flush_page_cleaner_disabled_loop());
	}

	ut_ad(srv_shutdown_state > 0);
	if (srv_fast_shutdown == 2
	    || srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS) {
		/* In very fast shutdown or when innodb failed to start, we
		simulate a crash of the buffer pool. We are not required to do
		any flushing. */
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
		pc_request(ULINT_MAX, LSN_MAX);

		while (pc_flush_slot() > 0) {}

		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;
		pc_wait_finished(&n_flushed_lru, &n_flushed_list);

		n_flushed = n_flushed_lru + n_flushed_list;

		/* We sleep only if there are no pages to flush */
		if (n_flushed == 0) {
			os_thread_sleep(100000);
		}
	} while (srv_shutdown_state == SRV_SHUTDOWN_CLEANUP);

	/* At this point all threads including the master and the purge
	thread must have been suspended. */
	ut_ad(!srv_any_background_activity());
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);

	/* We can now make a final sweep on flushing the buffer pool
	and exit after we have cleaned the whole buffer pool.
	It is important that we wait for any running batch that has
	been triggered by us to finish. Otherwise we can end up
	considering end of that batch as a finish of our final
	sweep and we'll come out of the loop leaving behind dirty pages
	in the flush_list */
	buf_flush_wait_batch_end(BUF_FLUSH_LIST);
	buf_flush_wait_LRU_batch_end();

	bool	success;

	do {
		pc_request(ULINT_MAX, LSN_MAX);

		while (pc_flush_slot() > 0) {}

		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;
		success = pc_wait_finished(&n_flushed_lru, &n_flushed_list);

		n_flushed = n_flushed_lru + n_flushed_list;

		buf_flush_wait_batch_end(BUF_FLUSH_LIST);
		buf_flush_wait_LRU_batch_end();

	} while (!success || n_flushed > 0);

	/* Some sanity checks */
	ut_ad(!srv_any_background_activity());
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);
	ut_a(UT_LIST_GET_LEN(buf_pool->flush_list) == 0);

	/* We have lived our life. Time to die. */

thread_exit:
	page_cleaner.is_running = false;
	mutex_destroy(&page_cleaner.mutex);

	os_event_destroy(page_cleaner.is_finished);

	buf_page_cleaner_is_active = false;

	my_thread_end();
	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

static void pc_flush_slot_func(void*)
{
	while (pc_flush_slot() > 0) {};
}


/** Initialize page_cleaner. */
void buf_flush_page_cleaner_init()
{
	ut_ad(!page_cleaner.is_running);

	mutex_create(LATCH_ID_PAGE_CLEANER, &page_cleaner.mutex);

	page_cleaner.is_finished = os_event_create("pc_is_finished");

	page_cleaner.is_running = true;

	buf_page_cleaner_is_active = true;
	os_thread_create(buf_flush_page_cleaner, NULL, NULL);
}

/** Synchronously flush dirty blocks.
NOTE: The calling thread is not allowed to hold any buffer page latches! */
void buf_flush_sync()
{
	bool success;
	do {
		success = buf_flush_lists(ULINT_MAX, LSN_MAX, NULL);
		buf_flush_wait_batch_end(BUF_FLUSH_LIST);
	} while (!success);
}

/** Request IO burst and wake page_cleaner up.
@param[in]	lsn_limit	upper limit of LSN to be flushed */
void buf_flush_request_force(lsn_t lsn_limit)
{
	/* adjust based on lsn_avg_rate not to get old */
	lsn_t	lsn_target = lsn_limit + lsn_avg_rate * 3;

	mutex_enter(&page_cleaner.mutex);
	if (lsn_target > buf_flush_sync_lsn) {
		buf_flush_sync_lsn = lsn_target;
	}
	mutex_exit(&page_cleaner.mutex);

	os_event_set(buf_flush_event);
}
#if defined UNIV_DEBUG || defined UNIV_BUF_DEBUG

/** Functor to validate the flush list. */
struct	Check {
	void operator()(const buf_page_t* elem) const
	{
		ut_a(elem->in_flush_list);
	}
};

/** Validate the flush list. */
static void buf_flush_validate_low()
{
	buf_page_t*		bpage;
	const ib_rbt_node_t*	rnode = NULL;

	ut_ad(mutex_own(&buf_pool->flush_list_mutex));

	ut_list_validate(buf_pool->flush_list, Check());

	bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);

	/* If we are in recovery mode i.e.: flush_rbt != NULL
	then each block in the flush_list must also be present
	in the flush_rbt. */
	if (UNIV_LIKELY_NULL(buf_pool->flush_rbt)) {
		rnode = rbt_first(buf_pool->flush_rbt);
	}

	while (bpage != NULL) {
		const lsn_t	om = bpage->oldest_modification;
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
			buf_page_t**	prpage;

			ut_a(rnode != NULL);
			prpage = rbt_value(buf_page_t*, rnode);

			ut_a(*prpage != NULL);
			ut_a(*prpage == bpage);
			rnode = rbt_next(buf_pool->flush_rbt, rnode);
		}

		bpage = UT_LIST_GET_NEXT(list, bpage);

		ut_a(bpage == NULL || om >= bpage->oldest_modification);
	}

	/* By this time we must have exhausted the traversal of
	flush_rbt (if active) as well. */
	ut_a(rnode == NULL);
}

/** Validate the flush list. */
void buf_flush_validate()
{
	mutex_enter(&buf_pool->flush_list_mutex);
	buf_flush_validate_low();
	mutex_exit(&buf_pool->flush_list_mutex);
}

#endif /* UNIV_DEBUG || UNIV_BUF_DEBUG */

/** Determine the number of dirty pages in a tablespace.
@param[in]	id		tablespace identifier
@return number of dirty pages */
ulint buf_pool_get_dirty_pages_count(ulint id)
{
	ulint		count = 0;

	mutex_enter(&buf_pool->mutex);
	mutex_enter(&buf_pool->flush_list_mutex);

	buf_page_t*	bpage;

	for (bpage = UT_LIST_GET_FIRST(buf_pool->flush_list);
	     bpage != 0;
	     bpage = UT_LIST_GET_NEXT(list, bpage)) {

		ut_ad(buf_page_in_file(bpage));
		ut_ad(bpage->in_flush_list);
		ut_ad(bpage->oldest_modification > 0);

		if (id == bpage->id.space()) {
			++count;
		}
	}

	mutex_exit(&buf_pool->flush_list_mutex);
	mutex_exit(&buf_pool->mutex);

	return(count);
}
