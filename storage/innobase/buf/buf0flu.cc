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
static constexpr ulint buf_flush_wait_flushed_sleep_time = 10000;

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

#ifdef UNIV_DEBUG
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
#endif /* UNIV_DEBUG */

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
	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

	/* Insert this buffer into the rbt. */
	c_node = rbt_insert(buf_pool.flush_rbt, &bpage, &bpage);
	ut_a(c_node != NULL);

	/* Get the predecessor. */
	p_node = rbt_prev(buf_pool.flush_rbt, c_node);

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
	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

#ifdef UNIV_DEBUG
	ibool ret =
#endif /* UNIV_DEBUG */
	rbt_delete(buf_pool.flush_rbt, &bpage);

	ut_ad(ret);
}

/*****************************************************************//**
Compare two modified blocks in the buffer pool. The key for comparison
is:
key = <oldest_modification, space, offset>
This comparison is used to maintian ordering of blocks in the
buf_pool.flush_rbt.
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
	const buf_page_t* b1 = *static_cast<const buf_page_t*const*>(p1);
	const buf_page_t* b2 = *static_cast<const buf_page_t*const*>(p2);

	ut_ad(b1 != NULL);
	ut_ad(b2 != NULL);

	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

	const lsn_t m1 = b1->oldest_modification(),
		m2 = b2->oldest_modification();

	ut_ad(m1);
	ut_ad(m2);

	if (m2 > m1) {
		return(1);
	} else if (m2 < m1) {
		return(-1);
	}

	if (b2->id() > b1->id()) {
		return 1;
	}
	if (b2->id() < b1->id()) {
		return -1;
	}
	return 0;
}

/********************************************************************//**
Initialize the red-black tree to speed up insertions into the flush_list
during recovery process. Should be called at the start of recovery
process before any page has been read/written. */
void
buf_flush_init_flush_rbt(void)
/*==========================*/
{
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	ut_ad(buf_pool.flush_rbt == NULL);
	/* Create red black tree for speedy insertions in flush list. */
	buf_pool.flush_rbt = rbt_create(
		sizeof(buf_page_t*), buf_flush_block_cmp);
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/********************************************************************//**
Frees up the red-black tree. */
void
buf_flush_free_flush_rbt(void)
/*==========================*/
{
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	ut_d(buf_flush_validate_low());
	rbt_free(buf_pool.flush_rbt);
	buf_pool.flush_rbt = NULL;
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Insert a modified block into the flush list.
@param[in,out]	block	modified block
@param[in]	lsn	oldest modification */
void buf_flush_insert_into_flush_list(buf_block_t* block, lsn_t lsn)
{
	mysql_mutex_assert_not_owner(&buf_pool.mutex);
	ut_ad(log_flush_order_mutex_own());
	ut_ad(lsn);

	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	block->page.set_oldest_modification(lsn);
	MEM_CHECK_DEFINED(block->page.zip.data
			  ? block->page.zip.data : block->frame,
			  block->physical_size());
	buf_pool.stat.flush_list_bytes += block->physical_size();
	ut_ad(buf_pool.stat.flush_list_bytes <= buf_pool.curr_pool_size);

	if (UNIV_LIKELY_NULL(buf_pool.flush_rbt)) {
		ut_ad(srv_shutdown_state != SRV_SHUTDOWN_FLUSH_PHASE);
		/* The field in_LRU_list is protected by buf_pool.mutex, which
		we are not holding.  However, while a block is in the flush
		list, it is dirty and cannot be discarded, not from the
		page_hash or from the LRU list.  At most, the uncompressed
		page frame of a compressed block may be discarded or created
		(copying the block->page to or from a buf_page_t that is
		dynamically allocated from buf_buddy_alloc()).  Because those
		transitions hold buf_pool.flush_list_mutex (via
		buf_flush_relocate_on_flush_list()), there is no possibility
		of a race condition in the assertions below. */
		ut_ad(block->page.in_LRU_list);
		/* buf_buddy_block_register() will take a block in the
		BUF_BLOCK_MEMORY state, not a file page. */
		ut_ad(!block->page.in_zip_hash);

		if (buf_page_t* prev_b =
		    buf_flush_insert_in_flush_rbt(&block->page)) {
			UT_LIST_INSERT_AFTER(buf_pool.flush_list, prev_b, &block->page);
			goto func_exit;
		}
	}

	UT_LIST_ADD_FIRST(buf_pool.flush_list, &block->page);
func_exit:
	ut_d(buf_flush_validate_skip());
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Remove a block from the flush list of modified blocks.
@param[in,out]	bpage	block to be removed from the flush list */
static void buf_flush_remove(buf_page_t *bpage)
{
#if 0 // FIXME: Rate-limit the output. Move this to the page cleaner?
	if (UNIV_UNLIKELY(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE)) {
		service_manager_extend_timeout(
			INNODB_EXTEND_TIMEOUT_INTERVAL,
			"Flush and remove page with tablespace id %u"
			", flush list length " ULINTPF,
			bpage->space, UT_LIST_GET_LEN(buf_pool.flush_list));
	}
#endif
	mysql_mutex_assert_owner(&buf_pool.mutex);
	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

	/* Important that we adjust the hazard pointer before removing
	the bpage from flush list. */
	buf_pool.flush_hp.adjust(bpage);
	UT_LIST_REMOVE(buf_pool.flush_list, bpage);

	/* If the flush_rbt is active then delete from there as well. */
	if (UNIV_LIKELY_NULL(buf_pool.flush_rbt)) {
		buf_flush_delete_from_flush_rbt(bpage);
	}

	/* Must be done after we have removed it from the flush_rbt
	because we assert on it in buf_flush_block_cmp(). */
	bpage->clear_oldest_modification();

	buf_pool.stat.flush_list_bytes -= bpage->physical_size();

#ifdef UNIV_DEBUG
	buf_flush_validate_skip();
#endif /* UNIV_DEBUG */
}

/** Remove all dirty pages belonging to a given tablespace when we are
deleting the data file of that tablespace.
The pages still remain a part of LRU and are evicted from
the list as they age towards the tail of the LRU.
@param id    tablespace identifier */
void buf_flush_remove_pages(ulint id)
{
  const page_id_t first(id, 0), end(id + 1, 0);
  ut_ad(id);
  mysql_mutex_lock(&buf_pool.mutex);

  for (;;)
  {
    bool deferred= false;

    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list); bpage; )
    {
      ut_a(bpage->in_file());
      buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);

      const page_id_t bpage_id(bpage->id());

      if (bpage_id < first || bpage_id >= end);
      else if (bpage->io_fix() != BUF_IO_NONE)
        deferred= true;
      else
        buf_flush_remove(bpage);

      bpage= prev;
    }

    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    if (!deferred)
      break;

    mysql_mutex_unlock(&buf_pool.mutex);
    os_thread_yield();
    mysql_mutex_lock(&buf_pool.mutex);
    buf_flush_wait_batch_end(false);
  }

  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Flush all the dirty pages that belong to a given tablespace.
The pages will remain in the LRU list and will be evicted from the LRU list
as they age and move towards the tail of the LRU list.
@param[in]	id		tablespace identifier */
void buf_flush_dirty_pages(ulint id)
{
  ut_ad(!sync_check_iterate(dict_sync_check()));

  for (;;)
  {
    ulint n= 0;

    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    for (buf_page_t *bpage= UT_LIST_GET_FIRST(buf_pool.flush_list); bpage;
         bpage= UT_LIST_GET_NEXT(list, bpage))
    {
      ut_ad(bpage->in_file());
      ut_ad(bpage->oldest_modification());
      if (id == bpage->id().space())
        n++;
    }
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    if (!n)
      return;

    buf_flush_lists(ULINT_MAX, LSN_MAX, nullptr);
  }
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

	mysql_mutex_assert_owner(&buf_pool.mutex);
	mysql_mutex_lock(&buf_pool.flush_list_mutex);

	/* FIXME: At this point we have both buf_pool and flush_list
	mutexes. Theoretically removal of a block from flush list is
	only covered by flush_list mutex but currently we do
	have buf_pool mutex in buf_flush_remove() therefore this block
	is guaranteed to be in the flush list. We need to check if
	this will work without the assumption of block removing code
	having the buf_pool mutex. */
	ut_ad(dpage->oldest_modification());

	/* If recovery is active we must swap the control blocks in
	the flush_rbt as well. */
	if (UNIV_LIKELY_NULL(buf_pool.flush_rbt)) {
		buf_flush_delete_from_flush_rbt(bpage);
		prev_b = buf_flush_insert_in_flush_rbt(dpage);
	}

	/* Important that we adjust the hazard pointer before removing
	the bpage from the flush list. */
	buf_pool.flush_hp.adjust(bpage);

	/* Must be done after we have removed it from the flush_rbt
	because we assert on it in buf_flush_block_cmp(). */
	bpage->clear_oldest_modification();

	prev = UT_LIST_GET_PREV(list, bpage);
	UT_LIST_REMOVE(buf_pool.flush_list, bpage);

	if (prev) {
		ut_ad(prev->oldest_modification());
		UT_LIST_INSERT_AFTER( buf_pool.flush_list, prev, dpage);
	} else {
		UT_LIST_ADD_FIRST(buf_pool.flush_list, dpage);
	}

	/* Just an extra check. Previous in flush_list
	should be the same control block as in flush_rbt. */
	ut_a(!buf_pool.flush_rbt || prev_b == prev);
	ut_d(buf_flush_validate_low());
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Complete write of a file page from buf_pool.
@param bpage   written page
@param request write request
@param dblwr   whether the doublewrite buffer was used
@param evict   whether or not to evict the page from LRU list */
void buf_page_write_complete(buf_page_t *bpage, const IORequest &request,
                             bool dblwr, bool evict)
{
  ut_ad(request.is_write());
  ut_ad(bpage->in_file());
  ut_ad(bpage->io_fix() == BUF_IO_WRITE);
  ut_ad(bpage->id().space() != TRX_SYS_SPACE ||
        !buf_dblwr_page_inside(bpage->id().page_no()));

  /* We do not need protect io_fix here by mutex to read it because
  this and buf_page_write_complete() are the only functions where we can
  change the value from BUF_IO_READ or BUF_IO_WRITE to some other
  value, and our code ensures that this is the only thread that handles
  the i/o for this block. */
  if (bpage->slot)
  {
    bpage->slot->release();
    bpage->slot= nullptr;
  }

  if (UNIV_UNLIKELY(MONITOR_IS_ON(MONITOR_MODULE_BUF_PAGE)))
    buf_page_monitor(bpage, BUF_IO_WRITE);
  DBUG_PRINT("ib_buf", ("write page %u:%u",
                        bpage->id().space(), bpage->id().page_no()));
  ut_ad(request.is_LRU() ? buf_pool.n_flush_LRU : buf_pool.n_flush_list);

  mysql_mutex_lock(&buf_pool.mutex);
  bpage->set_io_fix(BUF_IO_NONE);
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_remove(bpage);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (request.is_LRU())
  {
    if (!--buf_pool.n_flush_LRU)
      mysql_cond_signal(&buf_pool.no_flush_LRU);
  }
  else
  {
    if (!--buf_pool.n_flush_list)
      mysql_cond_signal(&buf_pool.no_flush_list);
  }

  if (dblwr)
    buf_dblwr_update(*bpage);

  /* Because this thread which does the unlocking might not be the same that
  did the locking, we use a pass value != 0 in unlock, which simply
  removes the newest lock debug record, without checking the thread id. */
  if (bpage->state() == BUF_BLOCK_FILE_PAGE)
    rw_lock_sx_unlock_gen(&((buf_block_t*) bpage)->lock, BUF_IO_WRITE);

  buf_pool.stat.n_pages_written++;

  if (evict)
    buf_LRU_free_page(bpage, true);

  mysql_mutex_unlock(&buf_pool.mutex);
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

		switch (block->page.id().page_no() % 16384) {
		case 0:
			reset_type = block->page.id().page_no() == 0
				? FIL_PAGE_TYPE_FSP_HDR
				: FIL_PAGE_TYPE_XDES;
			break;
		case 1:
			reset_type = FIL_PAGE_IBUF_BITMAP;
			break;
		case FSP_TRX_SYS_PAGE_NO:
			if (block->page.id()
			    == page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO)) {
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
				<< block->page.id() << " type "
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
  uint srclen= static_cast<uint>(srv_page_size) -
    (FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION +
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
@param[in,out]  size    payload size in bytes
@return page frame to be written to file
(may be src_frame or an encrypted/compressed copy of it) */
static byte *buf_page_encrypt(fil_space_t* space, buf_page_t* bpage, byte* s,
                              size_t *size)
{
  ut_ad(bpage->status != buf_page_t::FREED);
  ut_ad(space->id == bpage->id().space());

  ut_d(fil_page_type_validate(space, s));
  const uint32_t page_no= bpage->id().page_no();

  switch (page_no) {
  case TRX_SYS_PAGE_NO:
    if (bpage->id().space() != TRX_SYS_SPACE)
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
  buf_tmp_buffer_t *slot= buf_pool.io_buf_reserve();
  ut_a(slot);
  slot->allocate();
  slot->out_buf= NULL;
  bpage->slot= slot;

  byte *d= slot->crypt_buf;

  if (!page_compressed)
  {
not_compressed:
    byte *tmp= space->purpose == FIL_TYPE_TEMPORARY
      ? buf_tmp_page_encrypt(page_no, s, d)
      : fil_space_encrypt(space, page_no, s, d);

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
                                 fil_space_get_block_size(space, page_no),
                                 encrypted);

    if (!len)
      goto not_compressed;

    *size= len;

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
      tmp = fil_space_encrypt(space, page_no, tmp, d);

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

/** The following function deals with freed page during flushing.
     i)  Writing zeros to the file asynchronously if scrubbing is enabled
     ii) Punch the hole to the file synchoronously if page_compressed is
         enabled for the tablespace
This function also resets the IO_FIX to IO_NONE and making the
page status as NORMAL. It initiates the write to the file only after
releasing the page from flush list and its associated mutex.
@param[in,out]	bpage	freed buffer page */
static void buf_release_freed_page(buf_page_t *bpage)
{
  ut_ad(bpage->in_file());
  const bool uncompressed= bpage->state() == BUF_BLOCK_FILE_PAGE;
  mysql_mutex_lock(&buf_pool.mutex);
  bpage->set_io_fix(BUF_IO_NONE);
  bpage->status= buf_page_t::NORMAL;
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_remove(bpage);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (uncompressed)
    rw_lock_sx_unlock_gen(&reinterpret_cast<buf_block_t*>(bpage)->lock,
                          BUF_IO_WRITE);

  buf_LRU_free_page(bpage, true);
  mysql_mutex_unlock(&buf_pool.mutex);
}

/** Write a flushable page from buf_pool to a file.
buf_pool.mutex must be held.
@param bpage       buffer control block
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@param space       tablespace
@return whether the page was flushed and buf_pool.mutex was released */
static bool buf_flush_page(buf_page_t *bpage, bool lru, fil_space_t *space)
{
  ut_ad(bpage->in_file());
  ut_ad(bpage->ready_for_flush());
  mysql_mutex_assert_owner(&buf_pool.mutex);

  rw_lock_t *rw_lock;

  if (bpage->state() != BUF_BLOCK_FILE_PAGE)
    rw_lock= nullptr;
  else
  {
    rw_lock= &reinterpret_cast<buf_block_t*>(bpage)->lock;
    if (!rw_lock_sx_lock_nowait(rw_lock, BUF_IO_WRITE))
      return false;
  }

  bpage->set_io_fix(BUF_IO_WRITE);
  mysql_mutex_unlock(&buf_pool.mutex);

  /* We are holding rw_lock = buf_block_t::lock in SX mode except if
  this is a ROW_FORMAT=COMPRESSED page whose uncompressed page frame
  has been evicted from the buffer pool.

  Apart from possible rw_lock protection, bpage is also protected by
  io_fix and oldest_modification()!=0. Thus, it cannot be relocated in
  the buffer pool or removed from flush_list or LRU_list. */

  ut_ad((space->purpose == FIL_TYPE_TEMPORARY) ==
        (space == fil_system.temp_space));
  ut_ad(space->purpose == FIL_TYPE_TABLESPACE ||
        space->atomic_write_supported);

  const bool full_crc32= space->full_crc32();

  DBUG_PRINT("ib_buf", ("%s %u page %u:%u",
                        lru ? "LRU" : "flush_list",
                        bpage->id().space(), bpage->id().page_no()));
  mysql_mutex_assert_not_owner(&buf_pool.mutex);
  mysql_mutex_assert_not_owner(&buf_pool.flush_list_mutex);
  ut_ad(bpage->io_fix() == BUF_IO_WRITE);
  ut_ad(bpage->oldest_modification());
  ut_ad(bpage->state() ==
        (rw_lock ? BUF_BLOCK_FILE_PAGE : BUF_BLOCK_ZIP_PAGE));
  ut_ad(ULINT_UNDEFINED >
        (lru ? buf_pool.n_flush_LRU : buf_pool.n_flush_list));

  /* Because bpage->status can only be changed while buf_block_t
  exists, it cannot be modified for ROW_FORMAT=COMPRESSED pages
  without first allocating the uncompressed page frame. Such
  allocation cannot be completed due to our io_fix. So, bpage->status
  is protected even if !rw_lock. */
  const auto status= bpage->status;

  if (status == buf_page_t::FREED);
  else if (lru)
    buf_pool.n_flush_LRU++;
  else
    buf_pool.n_flush_list++;

  page_t *frame= bpage->zip.data;
  size_t size, orig_size;

  if (UNIV_UNLIKELY(!rw_lock)) /* ROW_FORMAT=COMPRESSED */
  {
    ut_ad(!space->full_crc32());
    ut_ad(!space->is_compressed()); /* not page_compressed */
    orig_size= size= bpage->zip_size();
    if (status != buf_page_t::FREED)
    {
      buf_flush_update_zip_checksum(frame, orig_size);
      frame= buf_page_encrypt(space, bpage, frame, &size);
    }
    ut_ad(size == bpage->zip_size());
  }
  else
  {
    buf_block_t *block= reinterpret_cast<buf_block_t*>(bpage);
    byte *page= block->frame;
    orig_size= size= block->physical_size();

    if (status != buf_page_t::FREED)
    {
      if (full_crc32)
      {
        /* innodb_checksum_algorithm=full_crc32 is not implemented for
        ROW_FORMAT=COMPRESSED pages. */
        ut_ad(!frame);
        page= buf_page_encrypt(space, bpage, page, &size);
      }

      buf_flush_init_for_writing(block, page, frame ? &bpage->zip : nullptr,
                                 full_crc32);

      if (!full_crc32)
        page= buf_page_encrypt(space, bpage, frame ? frame : page, &size);
    }

    frame= page;
  }

  if (UNIV_LIKELY(space->purpose == FIL_TYPE_TABLESPACE))
  {
    const lsn_t lsn= mach_read_from_8(frame + FIL_PAGE_LSN);
    ut_ad(lsn);
    ut_ad(lsn >= bpage->oldest_modification());
    ut_ad(!srv_read_only_mode);
    log_write_up_to(lsn, true);
  }

  bool use_doublewrite;
  IORequest request(IORequest::WRITE, bpage, lru);

  ut_ad(status == bpage->status);

  switch (status) {
  default:
    ut_ad(status == buf_page_t::FREED);
    buf_release_freed_page(bpage);
    break;
  case buf_page_t::NORMAL:
    use_doublewrite= space->use_doublewrite();

    if (use_doublewrite)
    {
      ut_ad(!srv_read_only_mode);
      buf_dblwr->add_to_batch(bpage, lru, size);
      break;
    }
    /* fall through */
  case buf_page_t::INIT_ON_FLUSH:
    use_doublewrite= false;
    if (size != orig_size)
      request.set_punch_hole();
    /* FIXME: pass space to fil_io() */
    fil_io(request, false, bpage->id(), bpage->zip_size(), 0,
           bpage->physical_size(), frame, bpage);
  }

  /* Increment the I/O operation count used for selecting LRU policy. */
  buf_LRU_stat_inc_io();
  return true;
}

/** Check whether a page can be flushed from the buf_pool.
@param id          page identifier
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@return whether the page can be flushed */
static bool buf_flush_check_neighbor(const page_id_t id, bool lru)
{
  mysql_mutex_assert_owner(&buf_pool.mutex);

  buf_page_t *bpage= buf_pool.page_hash_get_low(id, id.fold());

  if (!bpage || buf_pool.watch_is_sentinel(*bpage))
    return false;

  /* We avoid flushing 'non-old' blocks in an LRU flush, because the
  flushed blocks are soon freed */

  return (!lru || bpage->is_old()) && bpage->ready_for_flush();
}

/** Check which neighbors of a page can be flushed from the buf_pool.
@param space       tablespace
@param id          page identifier of a dirty page
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@return last page number that can be flushed */
static page_id_t buf_flush_check_neighbors(const fil_space_t &space,
                                           page_id_t &id, bool lru)
{
  ut_ad(id.page_no() < space.size);
  /* When flushed, dirty blocks are searched in neighborhoods of this
  size, and flushed along with the original page. */
  const ulint s= buf_pool.curr_size / 16;
  const uint32_t read_ahead= buf_pool.read_ahead_area;
  const uint32_t buf_flush_area= read_ahead > s
    ? static_cast<uint32_t>(s) : read_ahead;
  page_id_t low= id - (id.page_no() % buf_flush_area);
  page_id_t high= low + buf_flush_area;
  high.set_page_no(std::min(high.page_no(),
                            static_cast<uint32_t>(space.committed_size - 1)));

  /* Determine the contiguous dirty area around id. */
  const ulint id_fold= id.fold();

  mysql_mutex_lock(&buf_pool.mutex);

  if (id > low)
  {
    ulint fold= id_fold;
    for (page_id_t i= id - 1;; --i)
    {
      fold--;
      ut_ad(i.fold() == fold);
      if (!buf_flush_check_neighbor(i, lru))
      {
        low= i + 1;
        break;
      }
      if (i == low)
        break;
    }
  }

  page_id_t i= id;
  id= low;
  ulint fold= id_fold;
  while (++i < high)
  {
    ++fold;
    ut_ad(i.fold() == fold);
    if (!buf_flush_check_neighbor(i, lru))
      break;
  }

  mysql_mutex_unlock(&buf_pool.mutex);
  return i;
}

/** Write punch-hole or zeroes of the freed ranges when
innodb_immediate_scrub_data_uncompressed from the freed ranges.
@param[in]	space		tablespace which contains freed ranges
@param[in]	freed_ranges	freed ranges of the page to be flushed */
static void buf_flush_freed_pages(fil_space_t *space)
{
  ut_ad(space != NULL);
  const bool punch_hole= space->punch_hole;
  if (!srv_immediate_scrub_data_uncompressed && !punch_hole)
    return;
  lsn_t flush_to_disk_lsn= log_sys.get_flushed_lsn();

  std::unique_lock<std::mutex> freed_lock(space->freed_range_mutex);
  if (space->freed_ranges.empty()
      || flush_to_disk_lsn < space->get_last_freed_lsn())
  {
    freed_lock.unlock();
    return;
  }

  range_set freed_ranges= std::move(space->freed_ranges);
  freed_lock.unlock();

  for (const auto &range : freed_ranges)
  {
    ulint page_size= space->zip_size();
    if (!page_size)
      page_size= srv_page_size;

    if (punch_hole)
    {
      const auto len= (range.last - range.first + 1) * page_size;
      const page_id_t page_id(space->id, range.first);
      fil_io_t fio= fil_io(IORequestWrite, true, page_id, space->zip_size(),
                           0, len, nullptr, nullptr, false, true);
      if (fio.node)
        fio.node->space->release_for_io();
    }
    else if (srv_immediate_scrub_data_uncompressed)
    {
      for (auto i= range.first; i <= range.last; i++)
      {
        const page_id_t page_id(space->id, i);
        fil_io(IORequestWrite, false, page_id, space->zip_size(), 0,
               space->zip_size() ? space->zip_size() : srv_page_size,
               const_cast<byte*>(field_ref_zero), nullptr, false, false);
      }
    }
    buf_pool.stat.n_pages_written+= (range.last - range.first + 1);
  }
}

/** Flushes to disk all flushable pages within the flush area
and also write zeroes or punch the hole for the freed ranges of pages.
@param page_id     page identifier
@param lru         true=buf_pool.LRU; false=buf_pool.flush_list
@param n_flushed   number of pages flushed so far in this batch
@param n_to_flush  maximum number of pages we are allowed to flush
@return number of pages flushed */
static ulint buf_flush_try_neighbors(const page_id_t page_id, bool lru,
                                     ulint n_flushed, ulint n_to_flush)
{
	ulint		count = 0;

	fil_space_t* space = fil_space_acquire_for_io(page_id.space());
	if (!space) {
		return 0;
	}

        /* Flush the freed ranges while flushing the neighbors */
        buf_flush_freed_pages(space);

	page_id_t id = page_id;
	page_id_t high = (srv_flush_neighbors != 1
			  || UT_LIST_GET_LEN(buf_pool.LRU)
			  < BUF_LRU_OLD_MIN_LEN
			  || !space->is_rotational())
		? id + 1 /* Flush the minimum. */
		: buf_flush_check_neighbors(*space, id, lru);

	for (; id < high; ++id) {
		buf_page_t*	bpage;

		if ((count + n_flushed) >= n_to_flush) {

			/* We have already flushed enough pages and
			should call it a day. There is, however, one
			exception. If the page whose neighbors we
			are flushing has not been flushed yet then
			we'll try to flush the victim that we
			selected originally. */
			if (id <= page_id) {
				id = page_id;
			} else {
				break;
			}
		}

		const ulint fold = id.fold();

		mysql_mutex_lock(&buf_pool.mutex);

		bpage = buf_pool.page_hash_get_low(id, fold);

		if (bpage == NULL) {
			mysql_mutex_unlock(&buf_pool.mutex);
			continue;
		}

		ut_a(bpage->in_file());

		/* We avoid flushing 'non-old' blocks in an LRU flush,
		because the flushed blocks are soon freed */

		if (!lru || id == page_id || bpage->is_old()) {
			if (bpage->ready_for_flush()
			    && (id == page_id || bpage->buf_fix_count() == 0)
			    && buf_flush_page(bpage, lru, space)) {
				    ++count;
				    continue;
			}
		}
		mysql_mutex_unlock(&buf_pool.mutex);
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
	ulint		free_len = UT_LIST_GET_LEN(buf_pool.free);
	ulint		lru_len = UT_LIST_GET_LEN(buf_pool.unzip_LRU);

	mysql_mutex_assert_owner(&buf_pool.mutex);

	buf_block_t*	block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);

	while (block != NULL
	       && count < max
	       && free_len < srv_LRU_scan_depth
	       && lru_len > UT_LIST_GET_LEN(buf_pool.LRU) / 10) {

		++scanned;
		if (buf_LRU_free_page(&block->page, false)) {
			/* Block was freed. buf_pool.mutex potentially
			released and reacquired */
			++count;
			block = UT_LIST_GET_LAST(buf_pool.unzip_LRU);
			free_len = UT_LIST_GET_LEN(buf_pool.free);
			lru_len = UT_LIST_GET_LEN(buf_pool.unzip_LRU);
		} else {
			block = UT_LIST_GET_PREV(unzip_LRU, block);
		}
	}

	mysql_mutex_assert_owner(&buf_pool.mutex);

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
static void buf_flush_LRU_list_batch(ulint max, flush_counters_t *n)
{
  ulint scanned= 0;
  ulint free_limit= srv_LRU_scan_depth;
  n->flushed = 0;
  n->evicted = 0;
  n->unzip_LRU_evicted = 0;
  mysql_mutex_assert_owner(&buf_pool.mutex);
  if (buf_pool.withdraw_target && buf_pool.curr_size < buf_pool.old_size)
    free_limit+= buf_pool.withdraw_target - UT_LIST_GET_LEN(buf_pool.withdraw);

  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.LRU);
       bpage && n->flushed + n->evicted < max &&
       UT_LIST_GET_LEN(buf_pool.LRU) > BUF_LRU_MIN_LEN &&
       UT_LIST_GET_LEN(buf_pool.free) < free_limit;
       ++scanned, bpage= buf_pool.lru_hp.get())
  {
    buf_page_t *prev= UT_LIST_GET_PREV(LRU, bpage);
    buf_pool.lru_hp.set(prev);

    if (bpage->ready_for_replace())
    {
      /* block is ready for eviction i.e., it is clean and is not
      IO-fixed or buffer fixed. */
      if (buf_LRU_free_page(bpage, true))
        ++n->evicted;
    }
    else if (bpage->ready_for_flush())
    {
      /* Block is ready for flush. Dispatch an IO request. The IO
      helper thread will put it on free list in IO completion routine. */
      const page_id_t page_id(bpage->id());
      mysql_mutex_unlock(&buf_pool.mutex);
      n->flushed+= buf_flush_try_neighbors(page_id, true, n->flushed, max);
      mysql_mutex_lock(&buf_pool.mutex);
    }
    else
      /* Can't evict or dispatch this block. Go to previous. */
      ut_ad(buf_pool.lru_hp.is_hp(prev));
  }

  buf_pool.lru_hp.set(nullptr);

  /* We keep track of all flushes happening as part of LRU flush. When
  estimating the desired rate at which flush_list should be flushed,
  we factor in this value. */
  buf_lru_flush_page_count+= n->flushed;

  mysql_mutex_assert_owner(&buf_pool.mutex);

  if (n->evicted)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_BATCH_EVICT_TOTAL_PAGE,
                                 MONITOR_LRU_BATCH_EVICT_COUNT,
                                 MONITOR_LRU_BATCH_EVICT_PAGES,
                                 n->evicted);
  if (scanned)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_LRU_BATCH_SCANNED,
                                 MONITOR_LRU_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_LRU_BATCH_SCANNED_PER_CALL,
                                 scanned);
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
@param max_n    maximum mumber of blocks to flush
@param lsn      once an oldest_modification>=lsn is found, terminate the batch
@return number of blocks for which the write request was queued */
static ulint buf_do_flush_list_batch(ulint max_n, lsn_t lsn)
{
  ulint count= 0;
  ulint scanned= 0;

  mysql_mutex_assert_owner(&buf_pool.mutex);

  /* Start from the end of the list looking for a suitable block to be
  flushed. */
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  ulint len = UT_LIST_GET_LEN(buf_pool.flush_list);

  /* In order not to degenerate this scan to O(n*n) we attempt to
  preserve pointer of previous block in the flush list. To do so we
  declare it a hazard pointer. Any thread working on the flush list
  must check the hazard pointer and if it is removing the same block
  then it must reset it. */
  for (buf_page_t *bpage= UT_LIST_GET_LAST(buf_pool.flush_list);
       bpage && len && count < max_n;
       bpage= buf_pool.flush_hp.get(), ++scanned, len--)
  {
    const lsn_t oldest_modification= bpage->oldest_modification();
    if (oldest_modification >= lsn)
      break;
    ut_a(oldest_modification);

    buf_page_t *prev= UT_LIST_GET_PREV(list, bpage);
    buf_pool.flush_hp.set(prev);
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    ut_ad(bpage->in_file());
    const bool flushed= bpage->ready_for_flush();

    if (flushed)
    {
      const page_id_t page_id(bpage->id());
      mysql_mutex_unlock(&buf_pool.mutex);
      count+= buf_flush_try_neighbors(page_id, false, count, max_n);
      mysql_mutex_lock(&buf_pool.mutex);
    }

    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    ut_ad(flushed || buf_pool.flush_hp.is_hp(prev));
  }

  buf_pool.flush_hp.set(nullptr);
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (scanned)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_SCANNED,
                                 MONITOR_FLUSH_BATCH_SCANNED_NUM_CALL,
                                 MONITOR_FLUSH_BATCH_SCANNED_PER_CALL,
                                 scanned);
  if (count)
    MONITOR_INC_VALUE_CUMULATIVE(MONITOR_FLUSH_BATCH_TOTAL_PAGE,
                                 MONITOR_FLUSH_BATCH_COUNT,
                                 MONITOR_FLUSH_BATCH_PAGES,
                                 count);
  mysql_mutex_assert_owner(&buf_pool.mutex);
  return count;
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

/** Wait until a flush batch ends.
@param[in]	lru	true=buf_pool.LRU; false=buf_pool.flush_list */
void buf_flush_wait_batch_end(bool lru)
{
  thd_wait_begin(nullptr, THD_WAIT_DISKIO);
  if (lru)
    while (buf_pool.n_flush_LRU)
      mysql_cond_wait(&buf_pool.no_flush_LRU, &buf_pool.mutex);
  else
    while (buf_pool.n_flush_list)
      mysql_cond_wait(&buf_pool.no_flush_list, &buf_pool.mutex);
  thd_wait_end(nullptr);
}

/** Initiate a flushing batch.
@param max_n  wished minimum mumber of blocks flushed
@param lsn    0 for buf_pool.LRU flushing; otherwise,
              stop the buf_pool.flush_list batch on oldest_modification>=lsn
@param n      the number of processed pages
@return whether a batch was queued successfully (not running already) */
bool buf_flush_do_batch(ulint max_n, lsn_t lsn, flush_counters_t *n)
{
  n->flushed= 0;

  auto &n_flush= lsn ? buf_pool.n_flush_list : buf_pool.n_flush_LRU;

  if (n_flush)
    return false;

  mysql_mutex_lock(&buf_pool.mutex);
  const bool running= n_flush != 0;
  if (running || !UT_LIST_GET_LEN(buf_pool.flush_list))
  {
    mysql_mutex_unlock(&buf_pool.mutex);
    return !running;
  }
  n_flush++;

  if (!lsn)
  {
    buf_do_LRU_batch(max_n, n);
    if (!--n_flush)
      mysql_cond_signal(&buf_pool.no_flush_LRU);
  }
  else
  {
    n->flushed= buf_do_flush_list_batch(max_n, lsn);
    n->evicted= 0;
    if (!--n_flush)
      mysql_cond_signal(&buf_pool.no_flush_list);
  }

  buf_pool.try_LRU_scan= true;
  mysql_mutex_unlock(&buf_pool.mutex);

  if (!srv_read_only_mode)
    buf_dblwr_flush_buffered_writes();

  DBUG_PRINT("ib_buf", (lsn ? "flush_list completed" : "LRU flush completed"));
  return true;
}

/** Wait until a flush batch of the given lsn ends
@param[in]	new_oldest	target oldest_modified_lsn to wait for */
void buf_flush_wait_flushed(lsn_t new_oldest)
{
	for (;;) {
		/* We don't need to wait for fsync of the flushed
		blocks, because anyway we need fsync to make chekpoint.
		So, we don't need to wait for the batch end here. */

		mysql_mutex_lock(&buf_pool.flush_list_mutex);

		buf_page_t*	bpage;
		/* FIXME: Keep temporary tablespace pages in a separate flush
		list. We would only need to write out temporary pages if the
		page is about to be evicted from the buffer pool, and the page
		contents is still needed (the page has not been freed). */
		for (bpage = UT_LIST_GET_LAST(buf_pool.flush_list);
		     bpage && fsp_is_system_temporary(bpage->id().space());
		     bpage = UT_LIST_GET_PREV(list, bpage)) {
			ut_ad(bpage->oldest_modification());
		}

		lsn_t oldest = bpage ? bpage->oldest_modification() : 0;

		mysql_mutex_unlock(&buf_pool.flush_list_mutex);

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
@param[in]	lsn_limit	all blocks whose
oldest_modification is smaller than this should be flushed (if their number
does not exceed min_n), otherwise ignored
@param[out]	n_processed	the number of pages which were processed is
passed back to caller. Ignored if NULL.
@retval true if a batch was queued successfully
@retval false if another batch of same type was already running */
bool buf_flush_lists(ulint min_n, lsn_t lsn_limit, ulint *n_processed)
{
	ut_ad(lsn_limit);

	flush_counters_t	n;

	bool success = buf_flush_do_batch(min_n, lsn_limit, &n);

	if (n.flushed) {
		buf_flush_stats(n.flushed, 0);
	}

	if (n_processed) {
		*n_processed = n.flushed;
	}

	return success;
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
	mysql_mutex_lock(&buf_pool.mutex);
	scan_depth = UT_LIST_GET_LEN(buf_pool.LRU);
	if (buf_pool.curr_size < buf_pool.old_size
	    && buf_pool.withdraw_target > 0) {
		withdraw_depth = buf_pool.withdraw_target
			- UT_LIST_GET_LEN(buf_pool.withdraw);
	} else {
		withdraw_depth = 0;
	}
	mysql_mutex_unlock(&buf_pool.mutex);
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
	buf_flush_do_batch(scan_depth, 0, &n);

	return(n.flushed);
}

/** Wait for pending flushes to complete. */
void buf_flush_wait_batch_end_acquiring_mutex(bool lru)
{
  if (lru ? buf_pool.n_flush_LRU : buf_pool.n_flush_list)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    buf_flush_wait_batch_end(lru);
    mysql_mutex_unlock(&buf_pool.mutex);
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
	const ulint dirty = UT_LIST_GET_LEN(buf_pool.flush_list);
	if (!dirty) {
		/* No pages modified */
		return 0;
	}

	/* 1 + is there to avoid division by zero (in case the buffer
	pool (including the flush_list) was emptied while we are
	looking at it) */
	double	dirty_pct = 100 * static_cast<double>(dirty)
		/ static_cast<double>(1 + UT_LIST_GET_LEN(buf_pool.LRU)
				      + UT_LIST_GET_LEN(buf_pool.free));

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
	lsn_t	af_lwm = static_cast<lsn_t>(
		srv_adaptive_flushing_lwm
		* static_cast<double>(log_get_capacity()) / 100);

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
	return static_cast<ulint>(
		(static_cast<double>(srv_max_io_capacity / srv_io_capacity
				     * lsn_age_factor)
		 * sqrt(static_cast<double>(lsn_age_factor))
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
	    || time_elapsed >= static_cast<double>(srv_flushing_avg_loops)) {

		if (time_elapsed < 1) {
			time_elapsed = 1;
		}

		avg_page_rate = static_cast<ulint>(
			((static_cast<double>(sum_pages)
			  / time_elapsed)
			 + static_cast<double>(avg_page_rate)) / 2);

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

	oldest_lsn = buf_pool.get_oldest_modification();

	ut_ad(oldest_lsn <= log_get_lsn());

	age = cur_lsn > oldest_lsn ? cur_lsn - oldest_lsn : 0;

	pct_for_dirty = af_get_pct_for_dirty();
	pct_for_lsn = af_get_pct_for_lsn(age);

	pct_total = ut_max(pct_for_dirty, pct_for_lsn);

	/* Estimate pages to be flushed for the lsn progress */
	lsn_t	target_lsn = oldest_lsn
		+ lsn_avg_rate * buf_flush_lsn_scan_factor;
	ulint	pages_for_lsn = 0;

	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	for (buf_page_t* b = UT_LIST_GET_LAST(buf_pool.flush_list);
	     b != NULL;
	     b = UT_LIST_GET_PREV(list, b)) {
		if (b->oldest_modification() > target_lsn) {
			break;
		}
		++pages_for_lsn;
	}
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

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

	n_pages = (ulint(double(srv_io_capacity) * double(pct_total) / 100.0)
		   + avg_page_rate + pages_for_lsn) / 3;

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
@param lsn_limit in the case of buf_pool.flush_list all blocks whose
		oldest_modification is smaller than this should be flushed
		(if their number does not exceed min_n), otherwise ignored
*/
static void pc_request(ulint min_n, lsn_t lsn_limit)
{
	ut_ad(lsn_limit);

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
			ut_ad(page_cleaner.lsn_limit);

			page_cleaner.slot.succeeded_list = buf_flush_do_batch(
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

		if (recv_sys.flush_lru) {
			/* Flush pages from end of LRU if required */
			pc_request(0, LSN_MAX);
			while (pc_flush_slot() > 0) {}
			pc_wait_finished(&n_flushed_lru, &n_flushed_list);
		} else {
			/* Flush all pages */
			do {
				pc_request(ULINT_MAX, LSN_MAX);
				while (pc_flush_slot() > 0) {}
			} while (!pc_wait_finished(&n_flushed_lru,
						   &n_flushed_list));
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

	while (srv_shutdown_state <= SRV_SHUTDOWN_INITIATED) {
		ulint	curr_time = ut_time_ms();

		/* The page_cleaner skips sleep if the server is
		idle and there are no pending IOs in the buffer pool
		and there is work to do. */
		if (!n_flushed || !buf_pool.n_pend_reads
		    || srv_check_activity(&last_activity)) {

			ret_sleep = pc_sleep_if_needed(
				next_loop_time, sig_count, curr_time);
		} else if (curr_time > next_loop_time) {
			ret_sleep = OS_SYNC_TIME_EXCEEDED;
		} else {
			ret_sleep = 0;
		}

		if (srv_shutdown_state > SRV_SHUTDOWN_INITIATED) {
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

		} else if (srv_check_activity(&last_activity)) {
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
				lsn_limit = 1;
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
			buf_flush_lists(srv_io_capacity, LSN_MAX, &n_flushed);

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

	ut_ad(srv_shutdown_state > SRV_SHUTDOWN_INITIATED);
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
	buf_flush_wait_batch_end_acquiring_mutex(false);
	buf_flush_wait_batch_end_acquiring_mutex(true);

	bool	success;

	do {
		pc_request(ULINT_MAX, LSN_MAX);

		while (pc_flush_slot() > 0) {}

		ulint	n_flushed_lru = 0;
		ulint	n_flushed_list = 0;
		success = pc_wait_finished(&n_flushed_lru, &n_flushed_list);

		n_flushed = n_flushed_lru + n_flushed_list;

		buf_flush_wait_batch_end_acquiring_mutex(false);
		buf_flush_wait_batch_end_acquiring_mutex(true);
	} while (!success || n_flushed > 0);

	/* Some sanity checks */
	ut_ad(!srv_any_background_activity());
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_FLUSH_PHASE);
	ut_a(UT_LIST_GET_LEN(buf_pool.flush_list) == 0);

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
		buf_flush_wait_batch_end_acquiring_mutex(false);
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

#ifdef UNIV_DEBUG
/** Functor to validate the flush list. */
struct	Check {
	void operator()(const buf_page_t* elem) const
	{
		ut_a(elem->oldest_modification());
	}
};

/** Validate the flush list. */
static void buf_flush_validate_low()
{
	buf_page_t*		bpage;
	const ib_rbt_node_t*	rnode = NULL;

	mysql_mutex_assert_owner(&buf_pool.flush_list_mutex);

	ut_list_validate(buf_pool.flush_list, Check());

	bpage = UT_LIST_GET_FIRST(buf_pool.flush_list);

	/* If we are in recovery mode i.e.: flush_rbt != NULL
	then each block in the flush_list must also be present
	in the flush_rbt. */
	if (UNIV_LIKELY_NULL(buf_pool.flush_rbt)) {
		rnode = rbt_first(buf_pool.flush_rbt);
	}

	while (bpage != NULL) {
		const lsn_t	om = bpage->oldest_modification();
		/* A page in buf_pool.flush_list can be in
		BUF_BLOCK_REMOVE_HASH state. This happens when a page
		is in the middle of being relocated. In that case the
		original descriptor can have this state and still be
		in the flush list waiting to acquire the
		buf_pool.flush_list_mutex to complete the relocation. */
		ut_a(bpage->in_file()
		     || bpage->state() == BUF_BLOCK_REMOVE_HASH);
		ut_a(om > 0);

		if (UNIV_LIKELY_NULL(buf_pool.flush_rbt)) {
			buf_page_t**	prpage;

			ut_a(rnode != NULL);
			prpage = rbt_value(buf_page_t*, rnode);

			ut_a(*prpage != NULL);
			ut_a(*prpage == bpage);
			rnode = rbt_next(buf_pool.flush_rbt, rnode);
		}

		bpage = UT_LIST_GET_NEXT(list, bpage);

		ut_a(!bpage || om >= bpage->oldest_modification());
	}

	/* By this time we must have exhausted the traversal of
	flush_rbt (if active) as well. */
	ut_a(rnode == NULL);
}

/** Validate the flush list. */
void buf_flush_validate()
{
  mysql_mutex_lock(&buf_pool.flush_list_mutex);
  buf_flush_validate_low();
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}
#endif /* UNIV_DEBUG */
