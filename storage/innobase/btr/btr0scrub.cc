// Copyright (c) 2014, Google Inc.
// Copyright (c) 2017, 2021, MariaDB Corporation.

/**************************************************//**
@file btr/btr0scrub.cc
Scrubbing of btree pages

*******************************************************/

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0scrub.h"
#include "ibuf0ibuf.h"
#include "fsp0fsp.h"
#include "dict0dict.h"
#include "mtr0mtr.h"

/* used when trying to acquire dict-lock */
UNIV_INTERN bool fil_crypt_is_closing(ulint space);

/**
* scrub data at delete time (e.g purge thread)
*/
my_bool srv_immediate_scrub_data_uncompressed = false;

/**
* background scrub uncompressed data
*
* if srv_immediate_scrub_data_uncompressed is enabled
* this is only needed to handle "old" data
*/
my_bool srv_background_scrub_data_uncompressed = false;

/**
* backgrounds scrub compressed data
*
* reorganize compressed page for scrubbing
* (only way to scrub compressed data)
*/
my_bool srv_background_scrub_data_compressed = false;

/* check spaces once per hour */
UNIV_INTERN uint srv_background_scrub_data_check_interval = (60 * 60);

/* default to scrub spaces that hasn't been scrubbed in a week */
UNIV_INTERN uint srv_background_scrub_data_interval = (7 * 24 * 60 * 60);

/**
* statistics for scrubbing by background threads
*/
static btr_scrub_stat_t scrub_stat;
static ib_mutex_t scrub_stat_mutex;
#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t scrub_stat_mutex_key;
#endif

#ifdef UNIV_DEBUG
/**
* srv_scrub_force_testing
*
* - force scrubbing using background threads even for uncompressed tables
* - force pessimistic scrubbing (page split) even if not needed
*   (see test_pessimistic_scrub_pct)
*/
my_bool srv_scrub_force_testing = true;

/**
* Force pessimistic scrubbing in 50% of the cases (UNIV_DEBUG only)
*/
static int test_pessimistic_scrub_pct = 50;

#endif
static uint scrub_compression_level = page_zip_level;

/**************************************************************//**
Log a scrubbing failure */
static
void
log_scrub_failure(
/*===============*/
	dict_index_t* index,     /*!< in: index */
	btr_scrub_t* scrub_data, /*!< in: data to store statistics on */
	buf_block_t* block,	 /*!< in: block */
	dberr_t err)             /*!< in: error */
{
	const char* reason = "unknown";
	switch(err) {
	case DB_UNDERFLOW:
		reason = "too few records on page";
		scrub_data->scrub_stat.page_split_failures_underflow++;
		break;
	case DB_INDEX_CORRUPT:
		reason = "unable to find index!";
		scrub_data->scrub_stat.page_split_failures_missing_index++;
		break;
	case DB_OUT_OF_FILE_SPACE:
		reason = "out of filespace";
		scrub_data->scrub_stat.page_split_failures_out_of_filespace++;
		break;
	default:
		ut_ad(0);
		reason = "unknown";
		scrub_data->scrub_stat.page_split_failures_unknown++;
	}

	ib::warn() << "Failed to scrub index " << index->name
		   << " of table " << index->table->name
		   << " page " << block->page.id << ": " << reason;
}

/****************************************************************
Lock dict mutexes */
static
bool
btr_scrub_lock_dict_func(ulint space_id, bool lock_to_close_table,
			 const char * file, uint line)
{
	time_t start = time(0);
	time_t last = start;

	/* FIXME: this is not the proper way of doing things. The
	dict_sys->mutex should not be held by any thread for longer
	than a few microseconds. It must not be held during I/O,
	for example. So, what is the purpose for this busy-waiting?
	This function should be rewritten as part of MDEV-8139:
	Fix scrubbing tests. */

	while (mutex_enter_nowait(&(dict_sys->mutex))) {
		/* if we lock to close a table, we wait forever
		* if we don't lock to close a table, we check if space
		* is closing, and then instead give up
		*/
		if (lock_to_close_table) {
		} else if (fil_space_t* space = fil_space_acquire(space_id)) {
			bool stopping = space->is_stopping();
			fil_space_release(space);
			if (stopping) {
				return false;
			}
		} else {
			return false;
		}

		os_thread_sleep(250000);

		time_t now = time(0);

		if (now >= last + 30) {
			fprintf(stderr,
				"WARNING: %s:%u waited %ld seconds for"
				" dict_sys lock, space: " ULINTPF
				" lock_to_close_table: %d\n",
				file, line, long(now - start), space_id,
				lock_to_close_table);

			last = now;
		}
	}

	ut_ad(mutex_own(&dict_sys->mutex));
	return true;
}

#define btr_scrub_lock_dict(space, lock_to_close_table)			\
	btr_scrub_lock_dict_func(space, lock_to_close_table, __FILE__, __LINE__)

/****************************************************************
Unlock dict mutexes */
static
void
btr_scrub_unlock_dict()
{
	dict_mutex_exit_for_mysql();
}

/****************************************************************
Release reference to table
*/
static
void
btr_scrub_table_close(
/*==================*/
	dict_table_t* table)  /*!< in: table */
{
	bool dict_locked = true;
	bool try_drop = false;
	table->stats_bg_flag &= ~BG_SCRUB_IN_PROGRESS;
	dict_table_close(table, dict_locked, try_drop);
}

/****************************************************************
Release reference to table
*/
static
void
btr_scrub_table_close_for_thread(
	btr_scrub_t *scrub_data)
{
	if (scrub_data->current_table == NULL) {
		return;
	}

	if (fil_space_t* space = fil_space_acquire(scrub_data->space)) {
		/* If tablespace is not marked as stopping perform
		the actual close. */
		if (!space->is_stopping()) {
			mutex_enter(&dict_sys->mutex);
			/* perform the actual closing */
			btr_scrub_table_close(scrub_data->current_table);
			mutex_exit(&dict_sys->mutex);
		}
		fil_space_release(space);
	}

	scrub_data->current_table = NULL;
	scrub_data->current_index = NULL;
}

/**************************************************************//**
Check if scrubbing is turned ON or OFF */
static
bool
check_scrub_setting(
/*=====================*/
	btr_scrub_t*	scrub_data) /*!< in: scrub data  */
{
	if (scrub_data->compressed)
		return srv_background_scrub_data_compressed;
	else
		return srv_background_scrub_data_uncompressed;
}

#define IBUF_INDEX_ID (DICT_IBUF_ID_MIN + IBUF_SPACE_ID)

/**************************************************************//**
Check if a page needs scrubbing */
UNIV_INTERN
int
btr_page_needs_scrubbing(
/*=====================*/
	btr_scrub_t*	scrub_data, /*!< in: scrub data  */
	buf_block_t*	block,	    /*!< in: block to check, latched */
	btr_scrub_page_allocation_status_t allocated)  /*!< in: is block known
						       to be allocated */
{
	/**
	* Check if scrubbing has been turned OFF.
	*
	* at start of space, we check if scrubbing is ON or OFF
	* here we only check if scrubbing is turned OFF.
	*
	* Motivation is that it's only valueable to have a full table (space)
	* scrubbed.
	*/
	if (!check_scrub_setting(scrub_data)) {
		bool before_value = scrub_data->scrubbing;
		scrub_data->scrubbing = false;

		if (before_value == true) {
			/* we toggle scrubbing from on to off */
			return BTR_SCRUB_TURNED_OFF;
		}
	}

	if (scrub_data->scrubbing == false) {
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	const page_t*	page = buf_block_get_frame(block);

	if (allocated == BTR_SCRUB_PAGE_ALLOCATED) {
		if (fil_page_get_type(page) != FIL_PAGE_INDEX) {
			/* this function is called from fil-crypt-threads.
			* these threads iterate all pages of all tablespaces
			* and don't know about fil_page_type.
			* But scrubbing is only needed for index-pages. */

			/**
			* NOTE: scrubbing is also needed for UNDO pages,
			* but they are scrubbed at purge-time, since they are
			* uncompressed
			*/

			/* if encountering page type not needing scrubbing
			release reference to table object */
			return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
		}

		if (!page_has_garbage(page)) {
			/* no garbage (from deleted/shrunken records) */
			return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
		}

	} else if (allocated == BTR_SCRUB_PAGE_FREE ||
		   allocated == BTR_SCRUB_PAGE_ALLOCATION_UNKNOWN) {

		switch (fil_page_get_type(page)) {
		case FIL_PAGE_INDEX:
		case FIL_PAGE_TYPE_ZBLOB:
		case FIL_PAGE_TYPE_ZBLOB2:
			break;
		default:
			/**
			* If this is a dropped page, we also need to scrub
			* BLOB pages
			*/

			/* if encountering page type not needing scrubbing
			release reference to table object */
			return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
		}
	}

	if (block->page.id.space() == TRX_SYS_SPACE
	    && btr_page_get_index_id(page) == IBUF_INDEX_ID) {
		/* skip ibuf */
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	return BTR_SCRUB_PAGE;
}

/****************************************************************
Handle a skipped page
*/
UNIV_INTERN
void
btr_scrub_skip_page(
/*==================*/
	btr_scrub_t* scrub_data, /*!< in: data with scrub state */
	int needs_scrubbing)     /*!< in: return code from
				 btr_page_needs_scrubbing */
{
	switch(needs_scrubbing) {
	case BTR_SCRUB_SKIP_PAGE:
		/* nothing todo */
		return;
	case BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE:
		btr_scrub_table_close_for_thread(scrub_data);
		return;
	case BTR_SCRUB_TURNED_OFF:
	case BTR_SCRUB_SKIP_PAGE_AND_COMPLETE_SPACE:
		btr_scrub_complete_space(scrub_data);
		return;
	}

	/* unknown value. should not happen */
	ut_a(0);
}

/****************************************************************
Try to scrub a page using btr_page_reorganize_low
return DB_SUCCESS on success or DB_OVERFLOW on failure */
static
dberr_t
btr_optimistic_scrub(
/*==================*/
	btr_scrub_t* scrub_data, /*!< in: data with scrub state */
	buf_block_t* block,      /*!< in: block to scrub */
	dict_index_t* index,     /*!< in: index */
	mtr_t* mtr)              /*!< in: mtr */
{
#ifdef UNIV_DEBUG
	if (srv_scrub_force_testing &&
	    page_get_n_recs(buf_block_get_frame(block)) > 2 &&
	    (rand() % 100) < test_pessimistic_scrub_pct) {

		log_scrub_failure(index, scrub_data, block, DB_OVERFLOW);
		return DB_OVERFLOW;
	}
#endif

	page_cur_t cur;
	page_cur_set_before_first(block, &cur);
	bool recovery = false;
	if (!btr_page_reorganize_low(recovery, scrub_compression_level,
				     &cur, index, mtr)) {
		return DB_OVERFLOW;
	}

	/* We play safe and reset the free bits */
	if (!dict_index_is_clust(index) &&
	    block != NULL) {
		buf_frame_t* frame = buf_block_get_frame(block);
		if (frame &&
		    page_is_leaf(frame)) {

			ibuf_reset_free_bits(block);
		}
	}

	scrub_data->scrub_stat.page_reorganizations++;

	return DB_SUCCESS;
}

/****************************************************************
Try to scrub a page by splitting it
return DB_SUCCESS on success
DB_UNDERFLOW if page has too few records
DB_OUT_OF_FILE_SPACE if we can't find space for split */
static
dberr_t
btr_pessimistic_scrub(
/*==================*/
	btr_scrub_t* scrub_data, /*!< in: data with scrub state */
	buf_block_t* block,      /*!< in: block to scrub */
	dict_index_t* index,     /*!< in: index */
	mtr_t* mtr)              /*!< in: mtr */
{
	page_t*	page = buf_block_get_frame(block);

	if (page_get_n_recs(page) < 2) {
		/**
		* There is no way we can split a page with < 2 records
		*/
		log_scrub_failure(index, scrub_data, block, DB_UNDERFLOW);
		return DB_UNDERFLOW;
	}

	/**
	* Splitting page needs new space, allocate it here
	* so that splitting won't fail due to this */
	ulint n_extents = 3;
	ulint n_reserved = 0;
	if (!fsp_reserve_free_extents(&n_reserved, index->space,
				      n_extents, FSP_NORMAL, mtr)) {
		log_scrub_failure(index, scrub_data, block,
				  DB_OUT_OF_FILE_SPACE);
		return DB_OUT_OF_FILE_SPACE;
	}

	/* read block variables */
	const ulint page_no =  mach_read_from_4(page + FIL_PAGE_OFFSET);
	const page_id_t page_id(dict_index_get_space(index), page_no);
	const uint32_t left_page_no = btr_page_get_prev(page);
	const uint32_t right_page_no = btr_page_get_next(page);
	const page_id_t lpage_id(dict_index_get_space(index), left_page_no);
	const page_id_t rpage_id(dict_index_get_space(index), right_page_no);
	const page_size_t page_size(dict_table_page_size(index->table));

	/**
	* When splitting page, we need X-latches on left/right brothers
	* see e.g btr_cur_latch_leaves
	*/

	if (left_page_no != FIL_NULL) {
		/**
		* pages needs to be locked left-to-right, release block
		* and re-lock. We still have x-lock on index
		* so this should be safe
		*/
		mtr->release_block_at_savepoint(scrub_data->savepoint, block);

		buf_block_t* get_block __attribute__((unused)) = btr_block_get(
			lpage_id, page_size,
			RW_X_LATCH, index, mtr);

		/**
		* Refetch block and re-initialize page
		*/
		block = btr_block_get(
			page_id, page_size,
			RW_X_LATCH, index, mtr);

		page = buf_block_get_frame(block);

		/**
		* structure should be unchanged
		*/
		ut_a(left_page_no == btr_page_get_prev(page));
		ut_a(right_page_no == btr_page_get_next(page));
	}

	if (right_page_no != FIL_NULL) {
		buf_block_t* get_block __attribute__((unused))= btr_block_get(
			rpage_id, page_size,
			RW_X_LATCH, index, mtr);
	}

	/* arguments to btr_page_split_and_insert */
	mem_heap_t* heap = NULL;
	dtuple_t* entry = NULL;
	rec_offs* offsets = NULL;
	ulint n_ext = 0;
	ulint flags = BTR_MODIFY_TREE;

	/**
	* position a cursor on first record on page
	*/
	rec_t* rec = page_rec_get_next(page_get_infimum_rec(page));
	btr_cur_t cursor;
	btr_cur_position(index, rec, block, &cursor);

	/**
	* call split page with NULL as argument for entry to insert
	*/
	if (dict_index_get_page(index) == page_no) {
		/* The page is the root page
		* NOTE: ibuf_reset_free_bits is called inside
		* btr_root_raise_and_insert */
		rec = btr_root_raise_and_insert(
			flags, &cursor, &offsets, &heap, entry, n_ext, mtr);
	} else {
		/* We play safe and reset the free bits
		* NOTE: need to call this prior to btr_page_split_and_insert */
		if (!dict_index_is_clust(index) &&
		    block != NULL) {
			buf_frame_t* frame = buf_block_get_frame(block);
			if (frame &&
			    page_is_leaf(frame)) {

				ibuf_reset_free_bits(block);
			}
		}

		rec = btr_page_split_and_insert(
			flags, &cursor, &offsets, &heap, entry, n_ext, mtr);
	}

	if (heap) {
		mem_heap_free(heap);
	}

	if (n_reserved > 0) {
		fil_space_release_free_extents(index->space, n_reserved);
	}

	scrub_data->scrub_stat.page_splits++;
	return DB_SUCCESS;
}

/****************************************************************
Location index by id for a table
return index or NULL */
static
dict_index_t*
find_index(
/*========*/
	dict_table_t* table, /*!< in: table */
	index_id_t index_id) /*!< in: index id */
{
	if (table != NULL) {
		dict_index_t* index = dict_table_get_first_index(table);
		while (index != NULL) {
			if (index->id == index_id)
				return index;
			index = dict_table_get_next_index(index);
		}
	}

	return NULL;
}

/****************************************************************
Check if table should be scrubbed
*/
static
bool
btr_scrub_table_needs_scrubbing(
/*============================*/
	dict_table_t* table) /*!< in: table */
{
	if (table == NULL)
		return false;

	if (table->stats_bg_flag & BG_STAT_SHOULD_QUIT) {
		return false;
	}

	if (table->to_be_dropped) {
		return false;
	}

	if (!table->is_readable()) {
		return false;
	}

	return true;
}

/****************************************************************
Check if index should be scrubbed
*/
static
bool
btr_scrub_index_needs_scrubbing(
/*============================*/
	dict_index_t* index) /*!< in: index */
{
	if (index == NULL)
		return false;

	if (dict_index_is_ibuf(index)) {
		return false;
	}

	if (dict_index_is_online_ddl(index)) {
		return false;
	}

	return true;
}

/****************************************************************
Get table and index and store it on scrub_data
*/
static
void
btr_scrub_get_table_and_index(
/*=========================*/
	btr_scrub_t* scrub_data, /*!< in/out: scrub data */
	index_id_t index_id)     /*!< in: index id */
{
	/* first check if it's an index to current table */
	scrub_data->current_index = find_index(scrub_data->current_table,
					       index_id);

	if (scrub_data->current_index != NULL) {
		/* yes it was */
		return;
	}

	if (!btr_scrub_lock_dict(scrub_data->space, false)) {
		btr_scrub_complete_space(scrub_data);
		return;
	}

	/* close current table (if any) */
	if (scrub_data->current_table != NULL) {
		btr_scrub_table_close(scrub_data->current_table);
		scrub_data->current_table = NULL;
	}

	/* open table based on index_id */
	dict_table_t* table = dict_table_open_on_index_id(index_id);

	if (table != NULL) {
		/* mark table as being scrubbed */
		table->stats_bg_flag |= BG_SCRUB_IN_PROGRESS;

		if (!btr_scrub_table_needs_scrubbing(table)) {
			btr_scrub_table_close(table);
			btr_scrub_unlock_dict();
			return;
		}
	}

	btr_scrub_unlock_dict();
	scrub_data->current_table = table;
	scrub_data->current_index = find_index(table, index_id);
}

/****************************************************************
Handle free page */
UNIV_INTERN
int
btr_scrub_free_page(
/*====================*/
	btr_scrub_t* scrub_data,  /*!< in/out: scrub data */
	buf_block_t* block,       /*!< in: block to scrub */
	mtr_t* mtr)               /*!< in: mtr */
{
	// TODO(jonaso): scrub only what is actually needed

	{
		/* note: perform both the memset and setting of FIL_PAGE_TYPE
		* wo/ logging. so that if we crash before page is flushed
		* it will be found by scrubbing thread again
		*/
		memset(buf_block_get_frame(block) + PAGE_HEADER, 0,
		       UNIV_PAGE_SIZE - PAGE_HEADER);

		mach_write_to_2(buf_block_get_frame(block) + FIL_PAGE_TYPE,
				FIL_PAGE_TYPE_ALLOCATED);
	}

	page_create(block, mtr,
		    dict_table_is_comp(scrub_data->current_table),
		    dict_index_is_spatial(scrub_data->current_index));

	mtr_commit(mtr);

	/* page doesn't need further processing => SKIP
	* and close table/index so that we don't keep references too long */
	return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
}

/****************************************************************
Recheck if a page needs scrubbing, and if it does load appropriate
table and index */
UNIV_INTERN
int
btr_scrub_recheck_page(
/*====================*/
	btr_scrub_t* scrub_data,  /*!< inut: scrub data */
	buf_block_t* block,       /*!< in: block */
	btr_scrub_page_allocation_status_t allocated, /*!< in: is block
						      allocated or free */
	mtr_t* mtr)               /*!< in: mtr */
{
	/* recheck if page needs scrubbing (knowing allocation status) */
	int needs_scrubbing = btr_page_needs_scrubbing(
		scrub_data, block, allocated);

	if (needs_scrubbing != BTR_SCRUB_PAGE) {
		mtr_commit(mtr);
		return needs_scrubbing;
	}

	if (allocated == BTR_SCRUB_PAGE_FREE) {
		/** we don't need to load table/index for free pages
		* so scrub directly here */
		/* mtr is committed inside btr_scrub_page_free */
		return btr_scrub_free_page(scrub_data,
					   block,
					   mtr);
	}

	page_t*	page = buf_block_get_frame(block);
	index_id_t index_id = btr_page_get_index_id(page);

	if (scrub_data->current_index == NULL ||
	    scrub_data->current_index->id != index_id) {

		/**
		* commit mtr (i.e release locks on block)
		* and try to get table&index potentially loading it
		* from disk
		*/
		mtr_commit(mtr);
		btr_scrub_get_table_and_index(scrub_data, index_id);
	} else {
		/* we already have correct index
		* commit mtr so that we can lock index before fetching page
		*/
		mtr_commit(mtr);
	}

	/* check if table is about to be dropped */
	if (!btr_scrub_table_needs_scrubbing(scrub_data->current_table)) {
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	/* check if index is scrubbable */
	if (!btr_scrub_index_needs_scrubbing(scrub_data->current_index)) {
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	mtr_start(mtr);
	mtr_x_lock(dict_index_get_lock(scrub_data->current_index), mtr);
	/** set savepoint for X-latch of block */
	scrub_data->savepoint = mtr_set_savepoint(mtr);
	return BTR_SCRUB_PAGE;
}

/****************************************************************
Perform actual scrubbing of page */
UNIV_INTERN
int
btr_scrub_page(
/*============*/
	btr_scrub_t* scrub_data,  /*!< in/out: scrub data */
	buf_block_t* block,       /*!< in: block */
	btr_scrub_page_allocation_status_t allocated, /*!< in: is block
						      allocated or free */
	mtr_t* mtr)               /*!< in: mtr */
{
	/* recheck if page needs scrubbing (knowing allocation status) */
	int needs_scrubbing = BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;

	if (block) {
		btr_page_needs_scrubbing(scrub_data, block, allocated);
	}

	if (!block || needs_scrubbing != BTR_SCRUB_PAGE) {
		mtr_commit(mtr);
		return needs_scrubbing;
	}

	if (allocated == BTR_SCRUB_PAGE_FREE) {
		/* mtr is committed inside btr_scrub_page_free */
		return btr_scrub_free_page(scrub_data,
					   block,
					   mtr);
	}

	/* check that table/index still match now that they are loaded */

	if (scrub_data->current_table->space != scrub_data->space) {
		/* this is truncate table */
		mtr_commit(mtr);
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	if (scrub_data->current_index->space != scrub_data->space) {
		/* this is truncate table */
		mtr_commit(mtr);
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	if (scrub_data->current_index->page == FIL_NULL) {
		/* this is truncate table */
		mtr_commit(mtr);
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	buf_frame_t* frame = buf_block_get_frame(block);

	if (!frame || btr_page_get_index_id(frame) !=
	    scrub_data->current_index->id) {
		/* page has been reallocated to new index */
		mtr_commit(mtr);
		return BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE;
	}

	/* check if I can scrub (reorganize) page wo/ overflow */
	if (btr_optimistic_scrub(scrub_data,
				 block,
				 scrub_data->current_index,
				 mtr) != DB_SUCCESS) {

		/**
		* Can't reorganize page...need to split it
		*/
		btr_pessimistic_scrub(scrub_data,
				      block,
				      scrub_data->current_index,
				      mtr);
	}
	mtr_commit(mtr);

	return BTR_SCRUB_SKIP_PAGE; // no further action needed
}

/**************************************************************//**
Start iterating a space */
bool btr_scrub_start_space(const fil_space_t &space, btr_scrub_t *scrub_data)
{
	scrub_data->space = space.id;
	scrub_data->current_table = NULL;
	scrub_data->current_index = NULL;
	scrub_data->compressed = FSP_FLAGS_GET_ZIP_SSIZE(space.flags) != 0;
	scrub_data->scrubbing = check_scrub_setting(scrub_data);
	return scrub_data->scrubbing;
}

/***********************************************************************
Update global statistics with thread statistics */
static
void
btr_scrub_update_total_stat(btr_scrub_t *scrub_data)
{
	mutex_enter(&scrub_stat_mutex);
	scrub_stat.page_reorganizations +=
		scrub_data->scrub_stat.page_reorganizations;
	scrub_stat.page_splits +=
		scrub_data->scrub_stat.page_splits;
	scrub_stat.page_split_failures_underflow +=
		scrub_data->scrub_stat.page_split_failures_underflow;
	scrub_stat.page_split_failures_out_of_filespace +=
		scrub_data->scrub_stat.page_split_failures_out_of_filespace;
	scrub_stat.page_split_failures_missing_index +=
		scrub_data->scrub_stat.page_split_failures_missing_index;
	scrub_stat.page_split_failures_unknown +=
		scrub_data->scrub_stat.page_split_failures_unknown;
	mutex_exit(&scrub_stat_mutex);

	// clear stat
	memset(&scrub_data->scrub_stat, 0, sizeof(scrub_data->scrub_stat));
}

/** Complete iterating a space.
@param[in,out]	scrub_data	 scrub data */
UNIV_INTERN
void
btr_scrub_complete_space(btr_scrub_t* scrub_data)
{
	ut_ad(scrub_data->scrubbing);
	btr_scrub_table_close_for_thread(scrub_data);
	btr_scrub_update_total_stat(scrub_data);
}

/*********************************************************************
Return scrub statistics */
void
btr_scrub_total_stat(btr_scrub_stat_t *stat)
{
	mutex_enter(&scrub_stat_mutex);
	*stat = scrub_stat;
	mutex_exit(&scrub_stat_mutex);
}

/*********************************************************************
Init global variables */
UNIV_INTERN
void
btr_scrub_init()
{
	mutex_create(LATCH_ID_SCRUB_STAT_MUTEX, &scrub_stat_mutex);

	memset(&scrub_stat, 0, sizeof(scrub_stat));
}

/*********************************************************************
Cleanup globals */
UNIV_INTERN
void
btr_scrub_cleanup()
{
	mutex_free(&scrub_stat_mutex);
}

