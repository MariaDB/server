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

#include <list>

using std::list;
using std::min;

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

/* Work queue for defragmentation. */
typedef std::list<btr_defragment_item_t*>	btr_defragment_wq_t;
static btr_defragment_wq_t	btr_defragment_wq;

/* Mutex protecting the defragmentation work queue.*/
ib_mutex_t		btr_defragment_mutex;
#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t	btr_defragment_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/* Number of compression failures caused by defragmentation since server
start. */
ulint btr_defragment_compression_failures = 0;
/* Number of btr_defragment_n_pages calls that altered page but didn't
manage to release any page. */
ulint btr_defragment_failures = 0;
/* Total number of btr_defragment_n_pages calls that altered page.
The difference between btr_defragment_count and btr_defragment_failures shows
the amount of effort wasted. */
ulint btr_defragment_count = 0;

/******************************************************************//**
Constructor for btr_defragment_item_t. */
btr_defragment_item_t::btr_defragment_item_t(
	btr_pcur_t* pcur,
	os_event_t event)
{
	this->pcur = pcur;
	this->event = event;
	this->removed = false;
	this->last_processed = 0;
}

/******************************************************************//**
Destructor for btr_defragment_item_t. */
btr_defragment_item_t::~btr_defragment_item_t() {
	if (this->pcur) {
		btr_pcur_free_for_mysql(this->pcur);
	}
	if (this->event) {
		os_event_set(this->event);
	}
}

/******************************************************************//**
Initialize defragmentation. */
void
btr_defragment_init()
{
	srv_defragment_interval = 1000000000ULL / srv_defragment_frequency;
	mutex_create(LATCH_ID_BTR_DEFRAGMENT_MUTEX, &btr_defragment_mutex);
}

/******************************************************************//**
Shutdown defragmentation. Release all resources. */
void
btr_defragment_shutdown()
{
	mutex_enter(&btr_defragment_mutex);
	std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	while(iter != btr_defragment_wq.end()) {
		btr_defragment_item_t* item = *iter;
		iter = btr_defragment_wq.erase(iter);
		delete item;
	}
	mutex_exit(&btr_defragment_mutex);
	mutex_free(&btr_defragment_mutex);
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
	mutex_enter(&btr_defragment_mutex);
	for (std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	     iter != btr_defragment_wq.end();
	     ++iter) {
		btr_defragment_item_t* item = *iter;
		btr_pcur_t* pcur = item->pcur;
		btr_cur_t* cursor = btr_pcur_get_btr_cur(pcur);
		dict_index_t* idx = btr_cur_get_index(cursor);
		if (index->id == idx->id) {
			mutex_exit(&btr_defragment_mutex);
			return true;
		}
	}
	mutex_exit(&btr_defragment_mutex);
	return false;
}

/******************************************************************//**
Query thread uses this function to add an index to btr_defragment_wq.
Return a pointer to os_event for the query thread to wait on if this is a
synchronized defragmentation. */
os_event_t
btr_defragment_add_index(
	dict_index_t*	index,	/*!< index to be added  */
	dberr_t*	err)	/*!< out: error code */
{
	mtr_t mtr;
	ulint page_no = dict_index_get_page(index);
	*err = DB_SUCCESS;

	mtr_start(&mtr);
	// Load index rood page.
	const page_id_t page_id(dict_index_get_space(index), page_no);
	const page_size_t page_size(dict_table_page_size(index->table));
	buf_block_t* block = btr_block_get(page_id, page_size, RW_NO_LATCH, index, &mtr);
	page_t* page = NULL;

	if (block) {
		page = buf_block_get_frame(block);
	}

	if (page == NULL && !index->is_readable()) {
		mtr_commit(&mtr);
		*err = DB_DECRYPTION_FAILED;
		return NULL;
	}

	ut_ad(fil_page_index_page_check(page));
	ut_ad(!page_has_siblings(page));

	if (page_is_leaf(page)) {
		// Index root is a leaf page, no need to defragment.
		mtr_commit(&mtr);
		return NULL;
	}
	btr_pcur_t* pcur = btr_pcur_create_for_mysql();
	os_event_t event = os_event_create(0);
	btr_pcur_open_at_index_side(true, index, BTR_SEARCH_LEAF, pcur,
				    true, 0, &mtr);
	btr_pcur_move_to_next(pcur, &mtr);
	btr_pcur_store_position(pcur, &mtr);
	mtr_commit(&mtr);
	dict_stats_empty_defrag_summary(index);
	btr_defragment_item_t*	item = new btr_defragment_item_t(pcur, event);
	mutex_enter(&btr_defragment_mutex);
	btr_defragment_wq.push_back(item);
	mutex_exit(&btr_defragment_mutex);
	return event;
}

/******************************************************************//**
When table is dropped, this function is called to mark a table as removed in
btr_efragment_wq. The difference between this function and the remove_index
function is this will not NULL the event. */
void
btr_defragment_remove_table(
	dict_table_t*	table)	/*!< Index to be removed. */
{
	mutex_enter(&btr_defragment_mutex);
	for (std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	     iter != btr_defragment_wq.end();
	     ++iter) {
		btr_defragment_item_t* item = *iter;
		btr_pcur_t* pcur = item->pcur;
		btr_cur_t* cursor = btr_pcur_get_btr_cur(pcur);
		dict_index_t* idx = btr_cur_get_index(cursor);
		if (table->id == idx->table->id) {
			item->removed = true;
		}
	}
	mutex_exit(&btr_defragment_mutex);
}

/******************************************************************//**
Query thread uses this function to mark an index as removed in
btr_efragment_wq. */
void
btr_defragment_remove_index(
	dict_index_t*	index)	/*!< Index to be removed. */
{
	mutex_enter(&btr_defragment_mutex);
	for (std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	     iter != btr_defragment_wq.end();
	     ++iter) {
		btr_defragment_item_t* item = *iter;
		btr_pcur_t* pcur = item->pcur;
		btr_cur_t* cursor = btr_pcur_get_btr_cur(pcur);
		dict_index_t* idx = btr_cur_get_index(cursor);
		if (index->id == idx->id) {
			item->removed = true;
			item->event = NULL;
			break;
		}
	}
	mutex_exit(&btr_defragment_mutex);
}

/******************************************************************//**
Functions used by defragmentation thread: btr_defragment_xxx_item.
Defragmentation thread operates on the work *item*. It gets/removes
item from the work queue. */
/******************************************************************//**
Defragment thread uses this to remove an item from btr_defragment_wq.
When an item is removed from the work queue, all resources associated with it
are free as well. */
void
btr_defragment_remove_item(
	btr_defragment_item_t*	item) /*!< Item to be removed. */
{
	mutex_enter(&btr_defragment_mutex);
	for (std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	     iter != btr_defragment_wq.end();
	     ++iter) {
		if (item == *iter) {
			btr_defragment_wq.erase(iter);
			delete item;
			break;
		}
	}
	mutex_exit(&btr_defragment_mutex);
}

/******************************************************************//**
Defragment thread uses this to get an item from btr_defragment_wq to work on.
The item is not removed from the work queue so query threads can still access
this item. We keep it this way so query threads can find and kill a
defragmentation even if that index is being worked on. Be aware that while you
work on this item you have no lock protection on it whatsoever. This is OK as
long as the query threads and defragment thread won't modify the same fields
without lock protection.
*/
btr_defragment_item_t*
btr_defragment_get_item()
{
	if (btr_defragment_wq.empty()) {
		return NULL;
		//return nullptr;
	}
	mutex_enter(&btr_defragment_mutex);
	std::list< btr_defragment_item_t* >::iterator iter = btr_defragment_wq.begin();
	if (iter == btr_defragment_wq.end()) {
		iter = btr_defragment_wq.begin();
	}
	btr_defragment_item_t* item = *iter;
	iter++;
	mutex_exit(&btr_defragment_mutex);
	return item;
}

/*********************************************************************//**
Check whether we should save defragmentation statistics to persistent storage.
Currently we save the stats to persistent storage every 100 updates. */
UNIV_INTERN
void
btr_defragment_save_defrag_stats_if_needed(
	dict_index_t*	index)	/*!< in: index */
{
	if (srv_defragment_stats_accuracy != 0 // stats tracking disabled
	    && dict_index_get_space(index) != 0 // do not track system tables
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
UNIV_INTERN
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

	page_cur_set_before_first(block, &cur);
	page_cur_move_to_next(&cur);
	while (page_cur_get_rec(&cur) != page_get_supremum_rec(page)) {
		rec_t* cur_rec = page_cur_get_rec(&cur);
		offsets = rec_get_offsets(cur_rec, index, offsets,
					  page_is_leaf(page),
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
	return n_recs;
}

/*************************************************************//**
Merge as many records from the from_block to the to_block. Delete
the from_block if all records are successfully merged to to_block.
@return the to_block to target for next merge operation. */
UNIV_INTERN
buf_block_t*
btr_defragment_merge_pages(
	dict_index_t*	index,		/*!< in: index tree */
	buf_block_t*	from_block,	/*!< in: origin of merge */
	buf_block_t*	to_block,	/*!< in: destination of merge */
	const page_size_t	page_size,	/*!< in: page size of the block */
	ulint		reserved_space,	/*!< in: space reserved for future
					insert to avoid immediate page split */
	ulint*		max_data_size,	/*!< in/out: max data size to
					fit in a single compressed page. */
	mem_heap_t*	heap,		/*!< in/out: pointer to memory heap */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	page_t* from_page = buf_block_get_frame(from_block);
	page_t* to_page = buf_block_get_frame(to_block);
	ulint space = dict_index_get_space(index);
	ulint level = btr_page_get_level(from_page, mtr);
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
	if (page_size.is_compressed()) {
		ulint page_diff = UNIV_PAGE_SIZE - *max_data_size;
		max_ins_size_to_use = (max_ins_size_to_use > page_diff)
			       ? max_ins_size_to_use - page_diff : 0;
	}
	n_recs_to_move = btr_defragment_calc_n_recs_for_size(
		from_block, index, max_ins_size_to_use, &move_size);

	// If max_ins_size >= move_size, we can move the records without
	// reorganizing the page, otherwise we need to reorganize the page
	// first to release more space.
	if (move_size > max_ins_size) {
		if (!btr_page_reorganize_block(false, page_zip_level,
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
		my_atomic_addlint(
			&btr_defragment_compression_failures, 1);
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
		if (page_size.is_compressed()) {
			ibuf_reset_free_bits(to_block);
		} else {
			ibuf_update_free_bits_if_full(
				to_block,
				UNIV_PAGE_SIZE,
				ULINT_UNDEFINED);
		}
	}
	btr_cur_t parent;
	if (n_recs_to_move == n_recs) {
		/* The whole page is merged with the previous page,
		free it. */
		lock_update_merge_left(to_block, orig_pred,
				       from_block);
		btr_search_drop_page_hash_index(from_block);
		ut_a(DB_SUCCESS == btr_level_list_remove(space, page_size,
					   (page_t*)from_page, index, mtr));
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
UNIV_INTERN
buf_block_t*
btr_defragment_n_pages(
	buf_block_t*	block,	/*!< in: starting block for defragmentation */
	dict_index_t*	index,	/*!< in: index tree */
	uint		n_pages,/*!< in: number of pages to defragment */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint		space;
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
	ulint		level;
	ulint		max_data_size = 0;
	uint		n_defragmented = 0;
	uint		n_new_slots;
	mem_heap_t*	heap;
	ibool		end_of_index = FALSE;

	/* It doesn't make sense to call this function with n_pages = 1. */
	ut_ad(n_pages > 1);

	space = dict_index_get_space(index);
	if (space == 0) {
		/* Ignore space 0. */
		return NULL;
	}

	if (n_pages > BTR_DEFRAGMENT_MAX_N_PAGES) {
		n_pages = BTR_DEFRAGMENT_MAX_N_PAGES;
	}

	first_page = buf_block_get_frame(block);
	level = btr_page_get_level(first_page, mtr);
	const page_size_t page_size(dict_table_page_size(index->table));

	if (level != 0) {
		return NULL;
	}

	/* 1. Load the pages and calculate the total data size. */
	blocks[0] = block;
	for (uint i = 1; i <= n_pages; i++) {
		page_t* page = buf_block_get_frame(blocks[i-1]);
		ulint page_no = btr_page_get_next(page);
		total_data_size += page_get_data_size(page);
		total_n_recs += page_get_n_recs(page);
		if (page_no == FIL_NULL) {
			n_pages = i;
			end_of_index = TRUE;
			break;
		}

		const page_id_t page_id(dict_index_get_space(index), page_no);

		blocks[i] = btr_block_get(page_id, page_size,
					  RW_X_LATCH, index, mtr);
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
	if (page_size.is_compressed()) {
		ulint size = 0;
		int i = 0;
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
			size = size / i;
			optimal_page_size = ut_min(optimal_page_size, size);
		}
		max_data_size = optimal_page_size;
	}

	reserved_space = ut_min((ulint)(optimal_page_size
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
			index, blocks[i], current_block, page_size,
			reserved_space, &max_data_size, heap, mtr);
		if (new_block != current_block) {
			n_defragmented ++;
			current_block = new_block;
		}
	}
	mem_heap_free(heap);
	n_defragmented ++;
	my_atomic_addlint(
		&btr_defragment_count, 1);
	if (n_pages == n_defragmented) {
		my_atomic_addlint(
			&btr_defragment_failures, 1);
	} else {
		index->stat_defrag_n_pages_freed += (n_pages - n_defragmented);
	}
	if (end_of_index)
		return NULL;
	return current_block;
}

/** Whether btr_defragment_thread is active */
bool btr_defragment_thread_active;

/** Merge consecutive b-tree pages into fewer pages to defragment indexes */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(btr_defragment_thread)(void*)
{
	btr_pcur_t*	pcur;
	btr_cur_t*	cursor;
	dict_index_t*	index;
	mtr_t		mtr;
	buf_block_t*	first_block;
	buf_block_t*	last_block;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {
		ut_ad(btr_defragment_thread_active);

		/* If defragmentation is disabled, sleep before
		checking whether it's enabled. */
		if (!srv_defragment) {
			os_thread_sleep(BTR_DEFRAGMENT_SLEEP_IN_USECS);
			continue;
		}
		/* The following call won't remove the item from work queue.
		We only get a pointer to it to work on. This will make sure
		when user issue a kill command, all indices are in the work
		queue to be searched. This also means that the user thread
		cannot directly remove the item from queue (since we might be
		using it). So user thread only marks index as removed. */
		btr_defragment_item_t* item = btr_defragment_get_item();
		/* If work queue is empty, sleep and check later. */
		if (!item) {
			os_thread_sleep(BTR_DEFRAGMENT_SLEEP_IN_USECS);
			continue;
		}
		/* If an index is marked as removed, we remove it from the work
		queue. No other thread could be using this item at this point so
		it's safe to remove now. */
		if (item->removed) {
			btr_defragment_remove_item(item);
			continue;
		}

		pcur = item->pcur;
		ulonglong now = my_interval_timer();
		ulonglong elapsed = now - item->last_processed;

		if (elapsed < srv_defragment_interval) {
			/* If we see an index again before the interval
			determined by the configured frequency is reached,
			we just sleep until the interval pass. Since
			defragmentation of all indices queue up on a single
			thread, it's likely other indices that follow this one
			don't need to sleep again. */
			os_thread_sleep(static_cast<ulint>
					((srv_defragment_interval - elapsed)
					 / 1000));
		}

		now = my_interval_timer();
		mtr_start(&mtr);
		cursor = btr_pcur_get_btr_cur(pcur);
		index = btr_cur_get_index(cursor);
		mtr.set_named_space(index->space);
		/* To follow the latching order defined in WL#6326, acquire index->lock X-latch.
		This entitles us to acquire page latches in any order for the index. */
		mtr_x_lock(&index->lock, &mtr);
		/* This will acquire index->lock SX-latch, which per WL#6363 is allowed
		when we are already holding the X-latch. */
		btr_pcur_restore_position(BTR_MODIFY_TREE, pcur, &mtr);
		first_block = btr_cur_get_block(cursor);

		last_block = btr_defragment_n_pages(first_block, index,
						    srv_defragment_n_pages,
						    &mtr);
		if (last_block) {
			/* If we haven't reached the end of the index,
			place the cursor on the last record of last page,
			store the cursor position, and put back in queue. */
			page_t* last_page = buf_block_get_frame(last_block);
			rec_t* rec = page_rec_get_prev(
				page_get_supremum_rec(last_page));
			ut_a(page_rec_is_user_rec(rec));
			page_cur_position(rec, last_block,
					  btr_cur_get_page_cur(cursor));
			btr_pcur_store_position(pcur, &mtr);
			mtr_commit(&mtr);
			/* Update the last_processed time of this index. */
			item->last_processed = now;
		} else {
			dberr_t err = DB_SUCCESS;
			mtr_commit(&mtr);
			/* Reaching the end of the index. */
			dict_stats_empty_defrag_stats(index);
			err = dict_stats_save_defrag_stats(index);
			if (err != DB_SUCCESS) {
				ib::error() << "Saving defragmentation stats for table "
					    << index->table->name
					    << " index " << index->name()
					    << " failed with error " << err;
			} else {
				err = dict_stats_save_defrag_summary(index);

				if (err != DB_SUCCESS) {
					ib::error() << "Saving defragmentation summary for table "
					    << index->table->name
					    << " index " << index->name()
					    << " failed with error " << err;
				}
			}

			btr_defragment_remove_item(item);
		}
	}

	btr_defragment_thread_active = false;
	os_thread_exit();
	OS_THREAD_DUMMY_RETURN;
}
