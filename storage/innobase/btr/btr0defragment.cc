/*****************************************************************************

Copyright (C) 2012, 2014 Facebook, Inc. All Rights Reserved.
Copyright (C) 2014, 2021, MariaDB Corporation.

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
@file btr/btr0defragment.cc
Index defragmentation.

Created  05/29/2014 Rongrong Zhong
Modified 16/07/2014 Sunguck Lee
Modified 30/07/2014 Jan Lindström jan.lindstrom@mariadb.com
*******************************************************/

#include "btr0defragment.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "dict0defrag_bg.h"
#include "ibuf0ibuf.h"
#include "lock0lock.h"
#include "srv0start.h"
#include "mysqld.h"

#include <list>

/* When there's no work, either because defragment is disabled, or because no
query is submitted, thread checks state every BTR_DEFRAGMENT_SLEEP_IN_USECS.*/
#define BTR_DEFRAGMENT_SLEEP_IN_USECS		1000000
/* Reduce the target page size by this amount when compression failure happens
during defragmentaiton. 512 is chosen because it's a power of 2 and it is about
3% of the page size. When there are compression failures in defragmentation,
our goal is to get a decent defrag ratio with as few compression failure as
possible. From experimentation it seems that reduce the target size by 512 every
time will make sure the page is compressible within a couple of iterations. */
#define BTR_DEFRAGMENT_PAGE_REDUCTION_STEP_SIZE	512

/** Item in the work queue for btr_degrament_thread. */
struct btr_defragment_item_t
{
  /** persistent cursor where btr_defragment_n_pages should start */
  btr_pcur_t * const pcur;
  /** completion signal */
  pthread_cond_t *cond;
  /** timestamp of last time this index is processed by defragment thread */
  ulonglong last_processed= 0;

  btr_defragment_item_t(btr_pcur_t *pcur, pthread_cond_t *cond)
    : pcur(pcur), cond(cond) {}
};

/* Work queue for defragmentation. */
typedef std::list<btr_defragment_item_t*>	btr_defragment_wq_t;
static btr_defragment_wq_t	btr_defragment_wq;

/* Mutex protecting the defragmentation work queue.*/
static mysql_mutex_t btr_defragment_mutex;
#ifdef UNIV_PFS_MUTEX
mysql_pfs_key_t btr_defragment_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/* Number of compression failures caused by defragmentation since server
start. */
Atomic_counter<ulint> btr_defragment_compression_failures;
/* Number of btr_defragment_n_pages calls that altered page but didn't
manage to release any page. */
Atomic_counter<ulint> btr_defragment_failures;
/* Total number of btr_defragment_n_pages calls that altered page.
The difference between btr_defragment_count and btr_defragment_failures shows
the amount of effort wasted. */
Atomic_counter<ulint> btr_defragment_count;

bool btr_defragment_active;
static void btr_defragment_chunk(void*);

static tpool::timer* btr_defragment_timer;
static tpool::task_group task_group(1);
static tpool::task btr_defragment_task(btr_defragment_chunk, 0, &task_group);
static void btr_defragment_start();

static void submit_defragment_task(void*arg=0)
{
	srv_thread_pool->submit_task(&btr_defragment_task);
}

/******************************************************************//**
Initialize defragmentation. */
void
btr_defragment_init()
{
	srv_defragment_interval = 1000000000ULL / srv_defragment_frequency;
	mysql_mutex_init(btr_defragment_mutex_key, &btr_defragment_mutex,
			 nullptr);
	btr_defragment_timer = srv_thread_pool->create_timer(submit_defragment_task);
	btr_defragment_active = true;
}

/******************************************************************//**
Shutdown defragmentation. Release all resources. */
void
btr_defragment_shutdown()
{
	if (!btr_defragment_timer)
		return;
	delete btr_defragment_timer;
	btr_defragment_timer = 0;
	task_group.cancel_pending(&btr_defragment_task);
	mysql_mutex_lock(&btr_defragment_mutex);
	std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	while(iter != btr_defragment_wq.end()) {
		btr_defragment_item_t* item = *iter;
		iter = btr_defragment_wq.erase(iter);
		if (item->cond) {
			pthread_cond_signal(item->cond);
		}
	}
	mysql_mutex_unlock(&btr_defragment_mutex);
	mysql_mutex_destroy(&btr_defragment_mutex);
	btr_defragment_active = false;
}


/******************************************************************//**
Functions used by the query threads: btr_defragment_xxx_index
Query threads find/add/remove index. */
/******************************************************************//**
Check whether the given index is in btr_defragment_wq. We use index->id
to identify indices. */
bool
btr_defragment_find_index(
	dict_index_t*	index)	/*!< Index to find. */
{
	mysql_mutex_lock(&btr_defragment_mutex);
	for (std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	     iter != btr_defragment_wq.end();
	     ++iter) {
		btr_defragment_item_t* item = *iter;
		btr_pcur_t* pcur = item->pcur;
		btr_cur_t* cursor = btr_pcur_get_btr_cur(pcur);
		dict_index_t* idx = btr_cur_get_index(cursor);
		if (index->id == idx->id) {
			mysql_mutex_unlock(&btr_defragment_mutex);
			return true;
		}
	}
	mysql_mutex_unlock(&btr_defragment_mutex);
	return false;
}

/** Defragment an index.
@param pcur      persistent cursor
@param thd       current session, for checking thd_killed()
@return whether the operation was interrupted */
bool btr_defragment_add_index(btr_pcur_t *pcur, THD *thd)
{
  dict_stats_empty_defrag_summary(pcur->btr_cur.index);
  pthread_cond_t cond;
  pthread_cond_init(&cond, nullptr);
  btr_defragment_item_t item(pcur, &cond);
  mysql_mutex_lock(&btr_defragment_mutex);
  btr_defragment_wq.push_back(&item);
  if (btr_defragment_wq.size() == 1)
    /* Kick off defragmentation work */
    btr_defragment_start();
  bool interrupted= false;
  for (;;)
  {
    timespec abstime;
    set_timespec(abstime, 1);
    if (!my_cond_timedwait(&cond, &btr_defragment_mutex.m_mutex, &abstime))
      break;
    if (thd_killed(thd))
    {
      item.cond= nullptr;
      interrupted= true;
      break;
    }
  }

  pthread_cond_destroy(&cond);
  mysql_mutex_unlock(&btr_defragment_mutex);
  return interrupted;
}

/******************************************************************//**
When table is dropped, this function is called to mark a table as removed in
btr_efragment_wq. The difference between this function and the remove_index
function is this will not NULL the event. */
void
btr_defragment_remove_table(
	dict_table_t*	table)	/*!< Index to be removed. */
{
  mysql_mutex_lock(&btr_defragment_mutex);
  for (auto item : btr_defragment_wq)
  {
    if (item->cond && table == item->pcur->btr_cur.index->table)
    {
      pthread_cond_signal(item->cond);
      item->cond= nullptr;
    }
  }
  mysql_mutex_unlock(&btr_defragment_mutex);
}

/*********************************************************************//**
Check whether we should save defragmentation statistics to persistent storage.
Currently we save the stats to persistent storage every 100 updates. */
void btr_defragment_save_defrag_stats_if_needed(dict_index_t *index)
{
	if (srv_defragment_stats_accuracy != 0 // stats tracking disabled
	    && index->table->space_id != 0 // do not track system tables
	    && !index->table->is_temporary()
	    && index->stat_defrag_modified_counter
	       >= srv_defragment_stats_accuracy) {
		dict_stats_defrag_pool_add(index);
		index->stat_defrag_modified_counter = 0;
	}
}

/*********************************************************************//**
Main defragment functionalities used by defragment thread.*/
/*************************************************************//**
Calculate number of records from beginning of block that can
fit into size_limit
@return number of records */
static
ulint
btr_defragment_calc_n_recs_for_size(
	buf_block_t* block,	/*!< in: B-tree page */
	dict_index_t* index,	/*!< in: index of the page */
	ulint size_limit,	/*!< in: size limit to fit records in */
	ulint* n_recs_size)	/*!< out: actual size of the records that fit
				in size_limit. */
{
	page_t* page = buf_block_get_frame(block);
	ulint n_recs = 0;
	rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs* offsets = offsets_;
	rec_offs_init(offsets_);
	mem_heap_t* heap = NULL;
	ulint size = 0;
	page_cur_t cur;

	const ulint n_core = page_is_leaf(page) ? index->n_core_fields : 0;
	page_cur_set_before_first(block, &cur);
	page_cur_move_to_next(&cur);
	while (page_cur_get_rec(&cur) != page_get_supremum_rec(page)) {
		rec_t* cur_rec = page_cur_get_rec(&cur);
		offsets = rec_get_offsets(cur_rec, index, offsets, n_core,
					  ULINT_UNDEFINED, &heap);
		ulint rec_size = rec_offs_size(offsets);
		size += rec_size;
		if (size > size_limit) {
			size = size - rec_size;
			break;
		}
		n_recs ++;
		page_cur_move_to_next(&cur);
	}
	*n_recs_size = size;
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return n_recs;
}

/*************************************************************//**
Merge as many records from the from_block to the to_block. Delete
the from_block if all records are successfully merged to to_block.
@return the to_block to target for next merge operation. */
static
buf_block_t*
btr_defragment_merge_pages(
	dict_index_t*	index,		/*!< in: index tree */
	buf_block_t*	from_block,	/*!< in: origin of merge */
	buf_block_t*	to_block,	/*!< in: destination of merge */
	ulint		zip_size,	/*!< in: ROW_FORMAT=COMPRESSED size */
	ulint		reserved_space,	/*!< in: space reserved for future
					insert to avoid immediate page split */
	ulint*		max_data_size,	/*!< in/out: max data size to
					fit in a single compressed page. */
	mem_heap_t*	heap,		/*!< in/out: pointer to memory heap */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	page_t* from_page = buf_block_get_frame(from_block);
	page_t* to_page = buf_block_get_frame(to_block);
	ulint level = btr_page_get_level(from_page);
	ulint n_recs = page_get_n_recs(from_page);
	ulint new_data_size = page_get_data_size(to_page);
	ulint max_ins_size =
		page_get_max_insert_size(to_page, n_recs);
	ulint max_ins_size_reorg =
		page_get_max_insert_size_after_reorganize(
			to_page, n_recs);
	ulint max_ins_size_to_use = max_ins_size_reorg > reserved_space
				    ? max_ins_size_reorg - reserved_space : 0;
	ulint move_size = 0;
	ulint n_recs_to_move = 0;
	rec_t* rec = NULL;
	ulint target_n_recs = 0;
	rec_t* orig_pred;

	// Estimate how many records can be moved from the from_page to
	// the to_page.
	if (zip_size) {
		ulint page_diff = srv_page_size - *max_data_size;
		max_ins_size_to_use = (max_ins_size_to_use > page_diff)
			       ? max_ins_size_to_use - page_diff : 0;
	}
	n_recs_to_move = btr_defragment_calc_n_recs_for_size(
		from_block, index, max_ins_size_to_use, &move_size);

	// If max_ins_size >= move_size, we can move the records without
	// reorganizing the page, otherwise we need to reorganize the page
	// first to release more space.
	if (move_size > max_ins_size) {
		if (!btr_page_reorganize_block(page_zip_level,
					       to_block, index,
					       mtr)) {
			if (!dict_index_is_clust(index)
			    && page_is_leaf(to_page)) {
				ibuf_reset_free_bits(to_block);
			}
			// If reorganization fails, that means page is
			// not compressable. There's no point to try
			// merging into this page. Continue to the
			// next page.
			return from_block;
		}
		ut_ad(page_validate(to_page, index));
		max_ins_size = page_get_max_insert_size(to_page, n_recs);
		ut_a(max_ins_size >= move_size);
	}

	// Move records to pack to_page more full.
	orig_pred = NULL;
	target_n_recs = n_recs_to_move;
	while (n_recs_to_move > 0) {
		rec = page_rec_get_nth(from_page,
					n_recs_to_move + 1);
		orig_pred = page_copy_rec_list_start(
			to_block, from_block, rec, index, mtr);
		if (orig_pred)
			break;
		// If we reach here, that means compression failed after packing
		// n_recs_to_move number of records to to_page. We try to reduce
		// the targeted data size on the to_page by
		// BTR_DEFRAGMENT_PAGE_REDUCTION_STEP_SIZE and try again.
		btr_defragment_compression_failures++;
		max_ins_size_to_use =
			move_size > BTR_DEFRAGMENT_PAGE_REDUCTION_STEP_SIZE
			? move_size - BTR_DEFRAGMENT_PAGE_REDUCTION_STEP_SIZE
			: 0;
		if (max_ins_size_to_use == 0) {
			n_recs_to_move = 0;
			move_size = 0;
			break;
		}
		n_recs_to_move = btr_defragment_calc_n_recs_for_size(
			from_block, index, max_ins_size_to_use, &move_size);
	}
	// If less than target_n_recs are moved, it means there are
	// compression failures during page_copy_rec_list_start. Adjust
	// the max_data_size estimation to reduce compression failures
	// in the following runs.
	if (target_n_recs > n_recs_to_move
	    && *max_data_size > new_data_size + move_size) {
		*max_data_size = new_data_size + move_size;
	}
	// Set ibuf free bits if necessary.
	if (!dict_index_is_clust(index)
	    && page_is_leaf(to_page)) {
		if (zip_size) {
			ibuf_reset_free_bits(to_block);
		} else {
			ibuf_update_free_bits_if_full(
				to_block,
				srv_page_size,
				ULINT_UNDEFINED);
		}
	}
	btr_cur_t parent;
	if (n_recs_to_move == n_recs) {
		/* The whole page is merged with the previous page,
		free it. */
		const page_id_t from{from_block->page.id()};
		lock_update_merge_left(*to_block, orig_pred, from);
		btr_search_drop_page_hash_index(from_block);
		ut_a(DB_SUCCESS == btr_level_list_remove(*from_block, *index,
							 mtr));
		btr_page_get_father(index, from_block, mtr, &parent);
		btr_cur_node_ptr_delete(&parent, mtr);
		/* btr_blob_dbg_remove(from_page, index,
		"btr_defragment_n_pages"); */
		btr_page_free(index, from_block, mtr);
	} else {
		// There are still records left on the page, so
		// increment n_defragmented. Node pointer will be changed
		// so remove the old node pointer.
		if (n_recs_to_move > 0) {
			// Part of the page is merged to left, remove
			// the merged records, update record locks and
			// node pointer.
			dtuple_t* node_ptr;
			page_delete_rec_list_start(rec, from_block,
						   index, mtr);
			lock_update_split_and_merge(to_block,
						    orig_pred,
						    from_block);
			// FIXME: reuse the node_ptr!
			btr_page_get_father(index, from_block, mtr, &parent);
			btr_cur_node_ptr_delete(&parent, mtr);
			rec = page_rec_get_next(
				page_get_infimum_rec(from_page));
			node_ptr = dict_index_build_node_ptr(
				index, rec, page_get_page_no(from_page),
				heap, level);
			btr_insert_on_non_leaf_level(0, index, level+1,
						     node_ptr, mtr);
		}
		to_block = from_block;
	}
	return to_block;
}

/*************************************************************//**
Tries to merge N consecutive pages, starting from the page pointed by the
cursor. Skip space 0. Only consider leaf pages.
This function first loads all N pages into memory, then for each of
the pages other than the first page, it tries to move as many records
as possible to the left sibling to keep the left sibling full. During
the process, if any page becomes empty, that page will be removed from
the level list. Record locks, hash, and node pointers are updated after
page reorganization.
@return pointer to the last block processed, or NULL if reaching end of index */
static
buf_block_t*
btr_defragment_n_pages(
	buf_block_t*	block,	/*!< in: starting block for defragmentation */
	dict_index_t*	index,	/*!< in: index tree */
	uint		n_pages,/*!< in: number of pages to defragment */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	/* We will need to load the n+1 block because if the last page is freed
	and we need to modify the prev_page_no of that block. */
	buf_block_t*	blocks[BTR_DEFRAGMENT_MAX_N_PAGES + 1];
	page_t*		first_page;
	buf_block_t*	current_block;
	ulint		total_data_size = 0;
	ulint		total_n_recs = 0;
	ulint		data_size_per_rec;
	ulint		optimal_page_size;
	ulint		reserved_space;
	ulint		max_data_size = 0;
	uint		n_defragmented = 0;
	uint		n_new_slots;
	mem_heap_t*	heap;
	ibool		end_of_index = FALSE;

	/* It doesn't make sense to call this function with n_pages = 1. */
	ut_ad(n_pages > 1);

	if (!page_is_leaf(block->page.frame)) {
		return NULL;
	}

	if (!index->table->space || !index->table->space_id) {
		/* Ignore space 0. */
		return NULL;
	}

	if (n_pages > BTR_DEFRAGMENT_MAX_N_PAGES) {
		n_pages = BTR_DEFRAGMENT_MAX_N_PAGES;
	}

	first_page = buf_block_get_frame(block);
	const ulint zip_size = index->table->space->zip_size();

	/* 1. Load the pages and calculate the total data size. */
	blocks[0] = block;
	for (uint i = 1; i <= n_pages; i++) {
		page_t* page = buf_block_get_frame(blocks[i-1]);
		uint32_t page_no = btr_page_get_next(page);
		total_data_size += page_get_data_size(page);
		total_n_recs += page_get_n_recs(page);
		if (page_no == FIL_NULL) {
			n_pages = i;
			end_of_index = TRUE;
			break;
		}

		blocks[i] = btr_block_get(*index, page_no, RW_X_LATCH, true,
					  mtr);
	}

	if (n_pages == 1) {
		if (!page_has_prev(first_page)) {
			/* last page in the index */
			if (dict_index_get_page(index)
			    == page_get_page_no(first_page))
				return NULL;
			/* given page is the last page.
			Lift the records to father. */
			btr_lift_page_up(index, block, mtr);
		}
		return NULL;
	}

	/* 2. Calculate how many pages data can fit in. If not compressable,
	return early. */
	ut_a(total_n_recs != 0);
	data_size_per_rec = total_data_size / total_n_recs;
	// For uncompressed pages, the optimal data size if the free space of a
	// empty page.
	optimal_page_size = page_get_free_space_of_empty(
		page_is_comp(first_page));
	// For compressed pages, we take compression failures into account.
	if (zip_size) {
		ulint size = 0;
		uint i = 0;
		// We estimate the optimal data size of the index use samples of
		// data size. These samples are taken when pages failed to
		// compress due to insertion on the page. We use the average
		// of all samples we have as the estimation. Different pages of
		// the same index vary in compressibility. Average gives a good
		// enough estimation.
		for (;i < STAT_DEFRAG_DATA_SIZE_N_SAMPLE; i++) {
			if (index->stat_defrag_data_size_sample[i] == 0) {
				break;
			}
			size += index->stat_defrag_data_size_sample[i];
		}
		if (i != 0) {
			size /= i;
			optimal_page_size = ut_min(optimal_page_size, size);
		}
		max_data_size = optimal_page_size;
	}

	reserved_space = ut_min(static_cast<ulint>(
					static_cast<double>(optimal_page_size)
					* (1 - srv_defragment_fill_factor)),
			     (data_size_per_rec
			      * srv_defragment_fill_factor_n_recs));
	optimal_page_size -= reserved_space;
	n_new_slots = uint((total_data_size + optimal_page_size - 1)
			   / optimal_page_size);
	if (n_new_slots >= n_pages) {
		/* Can't defragment. */
		if (end_of_index)
			return NULL;
		return blocks[n_pages-1];
	}

	/* 3. Defragment pages. */
	heap = mem_heap_create(256);
	// First defragmented page will be the first page.
	current_block = blocks[0];
	// Start from the second page.
	for (uint i = 1; i < n_pages; i ++) {
		buf_block_t* new_block = btr_defragment_merge_pages(
			index, blocks[i], current_block, zip_size,
			reserved_space, &max_data_size, heap, mtr);
		if (new_block != current_block) {
			n_defragmented ++;
			current_block = new_block;
		}
	}
	mem_heap_free(heap);
	n_defragmented ++;
	btr_defragment_count++;
	if (n_pages == n_defragmented) {
		btr_defragment_failures++;
	} else {
		index->stat_defrag_n_pages_freed += (n_pages - n_defragmented);
	}
	if (end_of_index)
		return NULL;
	return current_block;
}



void btr_defragment_start() {
	if (!srv_defragment)
		return;
	ut_ad(!btr_defragment_wq.empty());
	submit_defragment_task();
}


/**
Callback used by defragment timer

Throttling "sleep", is implemented via rescheduling the
threadpool timer, which, when fired, will resume the work again,
where it is left.

The state (current item) is stored in function parameter.
*/
static void btr_defragment_chunk(void*)
{
	THD *thd = innobase_create_background_thd("InnoDB defragment");
	set_current_thd(thd);

	btr_defragment_item_t* item = nullptr;
	mtr_t		mtr;

	mysql_mutex_lock(&btr_defragment_mutex);

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		if (!item) {
			if (btr_defragment_wq.empty()) {
release_and_exit:
				mysql_mutex_unlock(&btr_defragment_mutex);
func_exit:
				set_current_thd(nullptr);
				innobase_destroy_background_thd(thd);
				return;
			}
			item = *btr_defragment_wq.begin();
			ut_ad(item);
		}

		if (!item->cond) {
processed:
			btr_defragment_wq.remove(item);
			item = nullptr;
			continue;
		}

		mysql_mutex_unlock(&btr_defragment_mutex);

		ulonglong now = my_interval_timer();
		ulonglong elapsed = now - item->last_processed;

		if (elapsed < srv_defragment_interval) {
			/* If we see an index again before the interval
			determined by the configured frequency is reached,
			we just sleep until the interval pass. Since
			defragmentation of all indices queue up on a single
			thread, it's likely other indices that follow this one
			don't need to sleep again. */
			int sleep_ms = (int)((srv_defragment_interval - elapsed) / 1000 / 1000);
			if (sleep_ms) {
				btr_defragment_timer->set_time(sleep_ms, 0);
				goto func_exit;
			}
		}
		log_free_check();
		mtr_start(&mtr);
		dict_index_t *index = item->pcur->btr_cur.index;
		index->set_modified(mtr);
		/* To follow the latching order defined in WL#6326, acquire index->lock X-latch.
		This entitles us to acquire page latches in any order for the index. */
		mtr_x_lock_index(index, &mtr);
		/* This will acquire index->lock SX-latch, which per WL#6363 is allowed
		when we are already holding the X-latch. */
		item->pcur->restore_position(BTR_MODIFY_TREE, &mtr);
		buf_block_t* first_block = btr_pcur_get_block(item->pcur);
		if (buf_block_t *last_block =
		    btr_defragment_n_pages(first_block, index,
					   srv_defragment_n_pages,
					   &mtr)) {
			/* If we haven't reached the end of the index,
			place the cursor on the last record of last page,
			store the cursor position, and put back in queue. */
			page_t* last_page = buf_block_get_frame(last_block);
			rec_t* rec = page_rec_get_prev(
				page_get_supremum_rec(last_page));
			ut_a(page_rec_is_user_rec(rec));
			page_cur_position(rec, last_block,
					  btr_pcur_get_page_cur(item->pcur));
			btr_pcur_store_position(item->pcur, &mtr);
			mtr_commit(&mtr);
			/* Update the last_processed time of this index. */
			item->last_processed = now;
			mysql_mutex_lock(&btr_defragment_mutex);
		} else {
			mtr_commit(&mtr);
			/* Reaching the end of the index. */
			dict_stats_empty_defrag_stats(index);
			if (dberr_t err= dict_stats_save_defrag_stats(index)) {
				ib::error() << "Saving defragmentation stats for table "
					    << index->table->name
					    << " index " << index->name()
					    << " failed with error " << err;
			} else {
				err = dict_stats_save_defrag_summary(index,
								     thd);

				if (err != DB_SUCCESS) {
					ib::error() << "Saving defragmentation summary for table "
					    << index->table->name
					    << " index " << index->name()
					    << " failed with error " << err;
				}
			}

			mysql_mutex_lock(&btr_defragment_mutex);
			if (item->cond) {
				pthread_cond_signal(item->cond);
			}
			goto processed;
		}
	}

	goto release_and_exit;
}
