/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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

/********************************************************************//**
@file btr/btr0sea.cc
The index tree adaptive search

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#include "btr0sea.h"
#ifdef BTR_CUR_HASH_ADAPT
#include "buf0buf.h"
#include "buf0lru.h"
#include "page0page.h"
#include "page0cur.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "srv0mon.h"

/** Is search system enabled.
Search system is protected by array of latches. */
char		btr_search_enabled;

/** Number of adaptive hash index partition. */
ulong		btr_ahi_parts;

#ifdef UNIV_SEARCH_PERF_STAT
/** Number of successful adaptive hash index lookups */
ulint		btr_search_n_succ	= 0;
/** Number of failed adaptive hash index lookups */
ulint		btr_search_n_hash_fail	= 0;
#endif /* UNIV_SEARCH_PERF_STAT */

#ifdef UNIV_PFS_RWLOCK
mysql_pfs_key_t	btr_search_latch_key;
#endif /* UNIV_PFS_RWLOCK */

/** The adaptive hash index */
btr_search_sys_t btr_search_sys;

/** If the number of records on the page divided by this parameter
would have been successfully accessed using a hash index, the index
is then built on the page, assuming the global limit has been reached */
#define BTR_SEARCH_PAGE_BUILD_LIMIT	16U

/** The global limit for consecutive potentially successful hash searches,
before hash index building is started */
#define BTR_SEARCH_BUILD_LIMIT		100U

/** Compute a hash value of a record in a page.
@param[in]	rec		index record
@param[in]	offsets		return value of rec_get_offsets()
@param[in]	n_fields	number of complete fields to fold
@param[in]	n_bytes		number of bytes to fold in the last field
@param[in]	index_id	index tree ID
@return the hash value */
static inline
ulint
rec_fold(
	const rec_t*	rec,
	const rec_offs*	offsets,
	ulint		n_fields,
	ulint		n_bytes,
	index_id_t	tree_id)
{
	ulint		i;
	const byte*	data;
	ulint		len;
	ulint		fold;
	ulint		n_fields_rec;

	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(rec_validate(rec, offsets));
	ut_ad(page_rec_is_leaf(rec));
	ut_ad(!page_rec_is_metadata(rec));
	ut_ad(n_fields > 0 || n_bytes > 0);

	n_fields_rec = rec_offs_n_fields(offsets);
	ut_ad(n_fields <= n_fields_rec);
	ut_ad(n_fields < n_fields_rec || n_bytes == 0);

	if (n_fields > n_fields_rec) {
		n_fields = n_fields_rec;
	}

	if (n_fields == n_fields_rec) {
		n_bytes = 0;
	}

	fold = ut_fold_ull(tree_id);

	for (i = 0; i < n_fields; i++) {
		data = rec_get_nth_field(rec, offsets, i, &len);

		if (len != UNIV_SQL_NULL) {
			fold = ut_fold_ulint_pair(fold,
						  ut_fold_binary(data, len));
		}
	}

	if (n_bytes > 0) {
		data = rec_get_nth_field(rec, offsets, i, &len);

		if (len != UNIV_SQL_NULL) {
			if (len > n_bytes) {
				len = n_bytes;
			}

			fold = ut_fold_ulint_pair(fold,
						  ut_fold_binary(data, len));
		}
	}

	return(fold);
}

/** Determine the number of accessed key fields.
@param[in]	n_fields	number of complete fields
@param[in]	n_bytes		number of bytes in an incomplete last field
@return	number of complete or incomplete fields */
inline MY_ATTRIBUTE((warn_unused_result))
ulint
btr_search_get_n_fields(
	ulint	n_fields,
	ulint	n_bytes)
{
	return(n_fields + (n_bytes > 0 ? 1 : 0));
}

/** Determine the number of accessed key fields.
@param[in]	cursor		b-tree cursor
@return	number of complete or incomplete fields */
inline MY_ATTRIBUTE((warn_unused_result))
ulint
btr_search_get_n_fields(
	const btr_cur_t*	cursor)
{
	return(btr_search_get_n_fields(cursor->n_fields, cursor->n_bytes));
}

/** This function should be called before reserving any btr search mutex, if
the intended operation might add nodes to the search system hash table.
Because of the latching order, once we have reserved the btr search system
latch, we cannot allocate a free frame from the buffer pool. Checks that
there is a free buffer frame allocated for hash table heap in the btr search
system. If not, allocates a free frames for the heap. This check makes it
probable that, when have reserved the btr search system latch and we need to
allocate a new node to the hash table, it will succeed. However, the check
will not guarantee success.
@param[in]	index	index handler */
static void btr_search_check_free_space_in_heap(const dict_index_t *index)
{
  /* Note that we peek the value of heap->free_block without reserving
  the latch: this is ok, because we will not guarantee that there will
  be enough free space in the hash table. */

  buf_block_t *block= buf_block_alloc();
  auto part= btr_search_sys.get_part(*index);

  part->latch.wr_lock(SRW_LOCK_CALL_ false);

  if (!btr_search_enabled || part->heap->free_block)
    buf_block_free(block);
  else
    part->heap->free_block= block;

  part->latch.wr_unlock();
}

/** Set index->ref_count = 0 on all indexes of a table.
@param[in,out]	table	table handler */
static void btr_search_disable_ref_count(dict_table_t *table)
{
  for (dict_index_t *index= dict_table_get_first_index(table); index;
       index= dict_table_get_next_index(index))
    index->search_info->ref_count= 0;
}

/** Lazily free detached metadata when removing the last reference. */
ATTRIBUTE_COLD void btr_search_lazy_free(dict_index_t *index)
{
  ut_ad(index->freed());
  dict_table_t *table= index->table;
  table->autoinc_mutex.wr_lock(false);

  /* Perform the skipped steps of dict_index_remove_from_cache_low(). */
  UT_LIST_REMOVE(table->freed_indexes, index);
  index->lock.free();
  dict_mem_index_free(index);

  if (!UT_LIST_GET_LEN(table->freed_indexes) &&
      !UT_LIST_GET_LEN(table->indexes))
  {
    ut_ad(!table->id);
    table->autoinc_mutex.wr_unlock();
    table->autoinc_mutex.destroy();
    dict_mem_table_free(table);
    return;
  }

  table->autoinc_mutex.wr_unlock();
}

ATTRIBUTE_COLD bool btr_search_disable()
{
	dict_table_t*	table;

	dict_sys.freeze(SRW_LOCK_CALL);

	btr_search_x_lock_all();

	if (!btr_search_enabled) {
		dict_sys.unfreeze();
		btr_search_x_unlock_all();
		return false;
	}

	btr_search_enabled = false;

	/* Clear the index->search_info->ref_count of every index in
	the data dictionary cache. */
	for (table = UT_LIST_GET_FIRST(dict_sys.table_LRU); table;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		btr_search_disable_ref_count(table);
	}

	for (table = UT_LIST_GET_FIRST(dict_sys.table_non_LRU); table;
	     table = UT_LIST_GET_NEXT(table_LRU, table)) {

		btr_search_disable_ref_count(table);
	}

	dict_sys.unfreeze();

	/* Set all block->index = NULL. */
	buf_pool.clear_hash_index();

	/* Clear the adaptive hash index. */
	btr_search_sys.clear();

	btr_search_x_unlock_all();

	return true;
}

/** Enable the adaptive hash search system.
@param resize whether buf_pool_t::resize() is the caller */
ATTRIBUTE_COLD void btr_search_enable(bool resize)
{
	if (!resize) {
		mysql_mutex_lock(&buf_pool.mutex);
		const auto is_shrinking = buf_pool.is_shrinking();
		mysql_mutex_unlock(&buf_pool.mutex);
		if (is_shrinking) {
			return;
		}
	}

	btr_search_x_lock_all();
	ulint hash_size = buf_pool.curr_pool_size() / sizeof(void *) / 64;

	if (btr_search_sys.parts[0].heap) {
		ut_ad(btr_search_enabled);
		btr_search_x_unlock_all();
		return;
	}

	btr_search_sys.alloc(hash_size);

	btr_search_enabled = true;
	btr_search_x_unlock_all();
}

/** Updates the search info of an index about hash successes. NOTE that info
is NOT protected by any semaphore, to save CPU time! Do not assume its fields
are consistent.
@param[in,out]	info	search info
@param[in]	cursor	cursor which was just positioned */
static void btr_search_info_update_hash(btr_search_t *info, btr_cur_t *cursor)
{
	dict_index_t*	index = cursor->index();
	int		cmp;

	uint16_t n_unique = dict_index_get_n_unique_in_tree(index);

	if (info->n_hash_potential == 0) {

		goto set_new_recomm;
	}

	/* Test if the search would have succeeded using the recommended
	hash prefix */

	if (info->n_fields >= n_unique && cursor->up_match >= n_unique) {
increment_potential:
		info->n_hash_potential++;

		return;
	}

	cmp = ut_pair_cmp(info->n_fields, info->n_bytes,
			  cursor->low_match, cursor->low_bytes);

	if (info->left_side ? cmp <= 0 : cmp > 0) {

		goto set_new_recomm;
	}

	cmp = ut_pair_cmp(info->n_fields, info->n_bytes,
			  cursor->up_match, cursor->up_bytes);

	if (info->left_side ? cmp <= 0 : cmp > 0) {

		goto increment_potential;
	}

set_new_recomm:
	/* We have to set a new recommendation; skip the hash analysis
	for a while to avoid unnecessary CPU time usage when there is no
	chance for success */

	info->hash_analysis = 0;

	cmp = ut_pair_cmp(cursor->up_match, cursor->up_bytes,
			  cursor->low_match, cursor->low_bytes);
	info->left_side = cmp >= 0;
	info->n_hash_potential = cmp != 0;

	if (cmp == 0) {
		/* For extra safety, we set some sensible values here */
		info->n_fields = 1;
		info->n_bytes = 0;
	} else if (cmp > 0) {
		info->n_hash_potential = 1;

		if (cursor->up_match >= n_unique) {

			info->n_fields = n_unique;
			info->n_bytes = 0;

		} else if (cursor->low_match < cursor->up_match) {

			info->n_fields = static_cast<uint16_t>(
				cursor->low_match + 1);
			info->n_bytes = 0;
		} else {
			info->n_fields = static_cast<uint16_t>(
				cursor->low_match);
			info->n_bytes = static_cast<uint16_t>(
				cursor->low_bytes + 1);
		}
	} else {
		if (cursor->low_match >= n_unique) {

			info->n_fields = n_unique;
			info->n_bytes = 0;
		} else if (cursor->low_match > cursor->up_match) {

			info->n_fields = static_cast<uint16_t>(
				cursor->up_match + 1);
			info->n_bytes = 0;
		} else {
			info->n_fields = static_cast<uint16_t>(
				cursor->up_match);
			info->n_bytes = static_cast<uint16_t>(
				cursor->up_bytes + 1);
		}
	}
}

/** Update the block search info on hash successes. NOTE that info and
block->n_hash_helps, n_fields, n_bytes, left_side are NOT protected by any
semaphore, to save CPU time! Do not assume the fields are consistent.
@return TRUE if building a (new) hash index on the block is recommended
@param[in,out]	info	search info
@param[in,out]	block	buffer block */
static
bool
btr_search_update_block_hash_info(btr_search_t* info, buf_block_t* block)
{
	ut_ad(block->page.lock.have_x() || block->page.lock.have_s());

	info->last_hash_succ = FALSE;
	ut_ad(block->page.frame);
	ut_ad(info->magic_n == BTR_SEARCH_MAGIC_N);

	if ((block->n_hash_helps > 0)
	    && (info->n_hash_potential > 0)
	    && (block->n_fields == info->n_fields)
	    && (block->n_bytes == info->n_bytes)
	    && (block->left_side == info->left_side)) {

		if ((block->index)
		    && (block->curr_n_fields == info->n_fields)
		    && (block->curr_n_bytes == info->n_bytes)
		    && (block->curr_left_side == info->left_side)) {

			/* The search would presumably have succeeded using
			the hash index */

			info->last_hash_succ = TRUE;
		}

		block->n_hash_helps++;
	} else {
		block->n_hash_helps = 1;
		block->n_fields = info->n_fields;
		block->n_bytes = info->n_bytes;
		block->left_side = info->left_side;
	}

	if ((block->n_hash_helps > page_get_n_recs(block->page.frame)
	     / BTR_SEARCH_PAGE_BUILD_LIMIT)
	    && (info->n_hash_potential >= BTR_SEARCH_BUILD_LIMIT)) {

		if ((!block->index)
		    || (block->n_hash_helps
			> 2U * page_get_n_recs(block->page.frame))
		    || (block->n_fields != block->curr_n_fields)
		    || (block->n_bytes != block->curr_n_bytes)
		    || (block->left_side != block->curr_left_side)) {

			/* Build a new hash index on the page */

			return(true);
		}
	}

	return(false);
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
/** Maximum number of records in a page */
constexpr ulint MAX_N_POINTERS = UNIV_PAGE_SIZE_MAX / REC_N_NEW_EXTRA_BYTES;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

__attribute__((nonnull))
/**
Insert an entry into the hash table. If an entry with the same fold number
is found, its node is updated to point to the new data, and no new node
is inserted.
@param table hash table
@param heap  memory heap
@param fold  folded value of the record
@param block buffer block containing the record
@param data  the record
@retval true on success
@retval false if no more memory could be allocated */
static bool ha_insert_for_fold(hash_table_t *table, mem_heap_t* heap,
                               ulint fold,
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
                               buf_block_t *block, /*!< buffer block of data */
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
                               const rec_t *data)
{
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block->page.frame == page_align(data));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  ut_ad(btr_search_enabled);

  hash_cell_t *cell= &table->array[table->calc_hash(fold)];

  for (ha_node_t *prev= static_cast<ha_node_t*>(cell->node); prev;
       prev= prev->next)
  {
    if (prev->fold == fold)
    {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
      buf_block_t *prev_block= prev->block;
      ut_a(prev_block->page.frame == page_align(prev->data));
      ut_a(prev_block->n_pointers-- < MAX_N_POINTERS);
      ut_a(block->n_pointers++ < MAX_N_POINTERS);

      prev->block= block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
      prev->data= data;
      return true;
    }
  }

  /* We have to allocate a new chain node */
  ha_node_t *node= static_cast<ha_node_t*>(mem_heap_alloc(heap, sizeof *node));

  if (!node)
    return false;

  ha_node_set_data(node, block, data);

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block->n_pointers++ < MAX_N_POINTERS);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

  node->fold= fold;
  node->next= nullptr;

  ha_node_t *prev= static_cast<ha_node_t*>(cell->node);
  if (!prev)
    cell->node= node;
  else
  {
    while (prev->next)
      prev= prev->next;
    prev->next= node;
  }
  return true;
}

__attribute__((nonnull))
/** Delete a record.
@param table     hash table
@param heap      memory heap
@param del_node  record to be deleted */
static void ha_delete_hash_node(hash_table_t *table, mem_heap_t *heap,
                                ha_node_t *del_node)
{
  ut_ad(btr_search_enabled);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(del_node->block->page.frame == page_align(del_node->data));
  ut_a(del_node->block->n_pointers-- < MAX_N_POINTERS);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

  table->cell_get(del_node->fold)->remove(*del_node, &ha_node_t::next);

  ha_node_t *top= static_cast<ha_node_t*>(mem_heap_get_top(heap, sizeof *top));

  if (del_node != top)
  {
    /* Compact the heap of nodes by moving the top in the place of del_node. */
    *del_node= *top;
    hash_cell_t *cell= &table->array[table->calc_hash(top->fold)];

    /* Look for the pointer to the top node, to update it */
    if (cell->node == top)
      /* The top node is the first in the chain */
      cell->node= del_node;
    else
    {
      /* We have to look for the predecessor */
      ha_node_t *node= static_cast<ha_node_t*>(cell->node);

      while (top != node->next) node= node->next;

      /* Now we have the predecessor node */
      node->next= del_node;
    }
  }

  /* Free the occupied space */
  mem_heap_free_top(heap, sizeof *top);
}

__attribute__((nonnull))
/** Delete all pointers to a page.
@param table     hash table
@param heap      memory heap
@param page      record to be deleted */
static void ha_remove_all_nodes_to_page(hash_table_t *table, mem_heap_t *heap,
                                        ulint fold, const page_t *page)
{
  for (ha_node_t *node= ha_chain_get_first(table, fold); node; )
  {
    if (page_align(ha_node_get_data(node)) == page)
    {
      ha_delete_hash_node(table, heap, node);
      /* The deletion may compact the heap of nodes and move other nodes! */
      node= ha_chain_get_first(table, fold);
    }
    else
      node= ha_chain_get_next(node);
  }
#ifdef UNIV_DEBUG
  /* Check that all nodes really got deleted */
  for (ha_node_t *node= ha_chain_get_first(table, fold); node;
       node= ha_chain_get_next(node))
    ut_ad(page_align(ha_node_get_data(node)) != page);
#endif /* UNIV_DEBUG */
}

/** Delete a record if found.
@param table     hash table
@param heap      memory heap for the hash bucket chain
@param fold      folded value of the searched data
@param data      pointer to the record
@return whether the record was found */
static bool ha_search_and_delete_if_found(hash_table_t *table,
                                          mem_heap_t *heap,
                                          ulint fold, const rec_t *data)
{
  if (ha_node_t *node= ha_search_with_data(table, fold, data))
  {
    ha_delete_hash_node(table, heap, node);
    return true;
  }

  return false;
}

__attribute__((nonnull))
/** Looks for an element when we know the pointer to the data and
updates the pointer to data if found.
@param table     hash table
@param fold      folded value of the searched data
@param data      pointer to the data
@param new_data  new pointer to the data
@return whether the element was found */
static bool ha_search_and_update_if_found(hash_table_t *table, ulint fold,
                                          const rec_t *data,
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
                                          /** block containing new_data */
                                          buf_block_t *new_block,
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
                                          const rec_t *new_data)
{
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(new_block->page.frame == page_align(new_data));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

  if (!btr_search_enabled)
    return false;

  if (ha_node_t *node= ha_search_with_data(table, fold, data))
  {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
    ut_a(node->block->n_pointers-- < MAX_N_POINTERS);
    ut_a(new_block->n_pointers++ < MAX_N_POINTERS);
    node->block= new_block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
    node->data= new_data;

    return true;
  }

  return false;
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
#else
# define ha_insert_for_fold(t,h,f,b,d) ha_insert_for_fold(t,h,f,d)
# define ha_search_and_update_if_found(table,fold,data,new_block,new_data) \
	ha_search_and_update_if_found(table,fold,data,new_data)
#endif

/** Updates a hash node reference when it has been unsuccessfully used in a
search which could have succeeded with the used hash parameters. This can
happen because when building a hash index for a page, we do not check
what happens at page boundaries, and therefore there can be misleading
hash nodes. Also, collisions in the fold value can lead to misleading
references. This function lazily fixes these imperfections in the hash
index.
@param[in]	info	search info
@param[in]	block	buffer block where cursor positioned
@param[in]	cursor	cursor */
static
void
btr_search_update_hash_ref(
	const btr_search_t*	info,
	buf_block_t*		block,
	const btr_cur_t*	cursor)
{
	ut_ad(cursor->flag == BTR_CUR_HASH_FAIL);

	ut_ad(block->page.lock.have_x() || block->page.lock.have_s());
	ut_ad(btr_cur_get_page(cursor) == block->page.frame);
	ut_ad(page_is_leaf(block->page.frame));
	assert_block_ahi_valid(block);

	dict_index_t* index = block->index;

	if (!index || !info->n_hash_potential) {
		return;
	}

	if (index != cursor->index()) {
		ut_ad(index->id == cursor->index()->id);
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_ad(block->page.id().space() == index->table->space_id);
	ut_ad(index == cursor->index());
	auto part = btr_search_sys.get_part(*index);
	part->latch.wr_lock(SRW_LOCK_CALL_ false);
	ut_ad(!block->index || block->index == index);

	if (block->index
	    && (block->curr_n_fields == info->n_fields)
	    && (block->curr_n_bytes == info->n_bytes)
	    && (block->curr_left_side == info->left_side)
	    && btr_search_enabled) {
		mem_heap_t*	heap		= NULL;
		rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
		rec_offs_init(offsets_);

		const rec_t* rec = btr_cur_get_rec(cursor);

		if (!page_rec_is_user_rec(rec)) {
			goto func_exit;
		}

		ulint fold = rec_fold(
			rec,
			rec_get_offsets(rec, index, offsets_,
					index->n_core_fields,
					ULINT_UNDEFINED, &heap),
			block->curr_n_fields,
			block->curr_n_bytes, index->id);
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}

		ha_insert_for_fold(&part->table, part->heap, fold, block, rec);

		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

func_exit:
	part->latch.wr_unlock();
}

/** Checks if a guessed position for a tree cursor is right. Note that if
mode is PAGE_CUR_LE, which is used in inserts, and the function returns
TRUE, then cursor->up_match and cursor->low_match both have sensible values.
@param[in,out]	cursor		guess cursor position
@param[in]	can_only_compare_to_cursor_rec
				if we do not have a latch on the page of cursor,
				but a latch corresponding search system, then
				ONLY the columns of the record UNDER the cursor
				are protected, not the next or previous record
				in the chain: we cannot look at the next or
				previous record to check our guess!
@param[in]	tuple		data tuple
@param[in]	mode		PAGE_CUR_L, PAGE_CUR_LE, PAGE_CUR_G, PAGE_CUR_GE
@return	whether a match was found */
static
bool
btr_search_check_guess(
	btr_cur_t*	cursor,
	bool		can_only_compare_to_cursor_rec,
	const dtuple_t*	tuple,
	ulint		mode)
{
	rec_t*		rec;
	ulint		n_unique;
	ulint		match;
	int		cmp;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	bool		success		= false;
	rec_offs_init(offsets_);

	n_unique = dict_index_get_n_unique_in_tree(cursor->index());

	rec = btr_cur_get_rec(cursor);

	if (UNIV_UNLIKELY(!page_rec_is_user_rec(rec)
			  || !page_rec_is_leaf(rec))) {
		ut_ad("corrupted index" == 0);
		return false;
	} else if (cursor->index()->table->not_redundant()) {
		switch (rec_get_status(rec)) {
		case REC_STATUS_INSTANT:
		case REC_STATUS_ORDINARY:
			break;
		default:
			ut_ad("corrupted index" == 0);
			return false;
		}
	}

	match = 0;

	offsets = rec_get_offsets(rec, cursor->index(), offsets,
				  cursor->index()->n_core_fields,
				  n_unique, &heap);
	cmp = cmp_dtuple_rec_with_match(tuple, rec, cursor->index(), offsets,
					&match);

	if (mode == PAGE_CUR_GE) {
		if (cmp > 0) {
			goto exit_func;
		}

		cursor->up_match = match;

		if (match >= n_unique) {
			success = true;
			goto exit_func;
		}
	} else if (mode == PAGE_CUR_LE) {
		if (cmp < 0) {
			goto exit_func;
		}

		cursor->low_match = match;

	} else if (mode == PAGE_CUR_G) {
		if (cmp >= 0) {
			goto exit_func;
		}
	} else if (mode == PAGE_CUR_L) {
		if (cmp <= 0) {
			goto exit_func;
		}
	}

	if (can_only_compare_to_cursor_rec) {
		/* Since we could not determine if our guess is right just by
		looking at the record under the cursor, return FALSE */
		goto exit_func;
	}

	match = 0;

	if ((mode == PAGE_CUR_G) || (mode == PAGE_CUR_GE)) {
		const rec_t* prev_rec = page_rec_get_prev(rec);

		if (UNIV_UNLIKELY(!prev_rec)) {
			ut_ad("corrupted index" == 0);
			goto exit_func;
		}

		if (page_rec_is_infimum(prev_rec)) {
			success = !page_has_prev(page_align(prev_rec));
			goto exit_func;
		}

		if (cursor->index()->table->not_redundant()) {
			switch (rec_get_status(prev_rec)) {
			case REC_STATUS_INSTANT:
			case REC_STATUS_ORDINARY:
				break;
			default:
				ut_ad("corrupted index" == 0);
				goto exit_func;
			}
		}

		offsets = rec_get_offsets(prev_rec, cursor->index(), offsets,
					  cursor->index()->n_core_fields,
					  n_unique, &heap);
		cmp = cmp_dtuple_rec_with_match(tuple, prev_rec,
						cursor->index(), offsets,
						&match);
		if (mode == PAGE_CUR_GE) {
			success = cmp > 0;
		} else {
			success = cmp >= 0;
		}
	} else {
		ut_ad(!page_rec_is_supremum(rec));

		const rec_t* next_rec = page_rec_get_next(rec);

		if (UNIV_UNLIKELY(!next_rec)) {
			ut_ad("corrupted index" == 0);
			goto exit_func;
		}

		if (page_rec_is_supremum(next_rec)) {
			if (!page_has_next(page_align(next_rec))) {
				cursor->up_match = 0;
				success = true;
			}

			goto exit_func;
		}

		if (cursor->index()->table->not_redundant()) {
			switch (rec_get_status(next_rec)) {
			case REC_STATUS_INSTANT:
			case REC_STATUS_ORDINARY:
				break;
			default:
				ut_ad("corrupted index" == 0);
				goto exit_func;
			}
		}

		offsets = rec_get_offsets(next_rec, cursor->index(), offsets,
					  cursor->index()->n_core_fields,
					  n_unique, &heap);
		cmp = cmp_dtuple_rec_with_match(
			tuple, next_rec, cursor->index(), offsets, &match);
		if (mode == PAGE_CUR_LE) {
			success = cmp < 0;
			cursor->up_match = match;
		} else {
			success = cmp <= 0;
		}
	}
exit_func:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(success);
}

static
void
btr_search_failure(btr_search_t* info, btr_cur_t* cursor)
{
	cursor->flag = BTR_CUR_HASH_FAIL;

#ifdef UNIV_SEARCH_PERF_STAT
	++info->n_hash_fail;

	if (info->n_hash_succ > 0) {
		--info->n_hash_succ;
	}
#endif /* UNIV_SEARCH_PERF_STAT */

	info->last_hash_succ = FALSE;
}

/** Tries to guess the right search position based on the hash search info
of the index. Note that if mode is PAGE_CUR_LE, which is used in inserts,
and the function returns TRUE, then cursor->up_match and cursor->low_match
both have sensible values.
@param[in,out]	index		index
@param[in,out]	info		index search info
@param[in]	tuple		logical record
@param[in]	mode		PAGE_CUR_L, ....
@param[in]	latch_mode	BTR_SEARCH_LEAF, ...
@param[out]	cursor		tree cursor
@param[in]	mtr		mini-transaction
@return whether the search succeeded */
TRANSACTIONAL_TARGET
bool
btr_search_guess_on_hash(
	dict_index_t*	index,
	btr_search_t*	info,
	const dtuple_t*	tuple,
	ulint		mode,
	ulint		latch_mode,
	btr_cur_t*	cursor,
	mtr_t*		mtr)
{
	ulint		fold;
	index_id_t	index_id;

	ut_ad(mtr->is_active());
	ut_ad(index->is_btree());

	/* Note that, for efficiency, the struct info may not be protected by
	any latch here! */

	if (latch_mode > BTR_MODIFY_LEAF
	    || !info->last_hash_succ || !info->n_hash_potential
	    || (tuple->info_bits & REC_INFO_MIN_REC_FLAG)) {
		return false;
	}

	ut_ad(index->is_btree());
        ut_ad(!index->table->is_temporary());

	ut_ad(latch_mode == BTR_SEARCH_LEAF || latch_mode == BTR_MODIFY_LEAF);
	compile_time_assert(ulint{BTR_SEARCH_LEAF} == ulint{RW_S_LATCH});
	compile_time_assert(ulint{BTR_MODIFY_LEAF} == ulint{RW_X_LATCH});

	cursor->n_fields = info->n_fields;
	cursor->n_bytes = info->n_bytes;

	if (dtuple_get_n_fields(tuple) < btr_search_get_n_fields(cursor)) {
		return false;
	}

	index_id = index->id;

#ifdef UNIV_SEARCH_PERF_STAT
	info->n_hash_succ++;
#endif
	fold = dtuple_fold(tuple, cursor->n_fields, cursor->n_bytes, index_id);

	cursor->fold = fold;
	cursor->flag = BTR_CUR_HASH;

	auto part = btr_search_sys.get_part(*index);
	const rec_t* rec;

	part->latch.rd_lock(SRW_LOCK_CALL_ false);

	if (!btr_search_enabled) {
		goto ahi_release_and_fail;
	}

	rec = static_cast<const rec_t*>(
		ha_search_and_get_data(&part->table, fold));

	if (!rec) {
ahi_release_and_fail:
		part->latch.rd_unlock();
fail:
		btr_search_failure(info, cursor);
		return false;
	}

	buf_block_t* block = buf_pool.block_from(rec);
	ut_ad(block->page.frame == page_align(rec));

	buf_pool_t::hash_chain& chain = buf_pool.page_hash.cell_get(
		block->page.id().fold());
	bool got_latch;
	{
		transactional_shared_lock_guard<page_hash_latch> g{
			buf_pool.page_hash.lock_get(chain)};
		got_latch = (latch_mode == BTR_SEARCH_LEAF)
			? block->page.lock.s_lock_try()
			: block->page.lock.x_lock_try();
	}

	if (!got_latch) {
		goto ahi_release_and_fail;
	}

	const auto state = block->page.state();
	if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED)) {
		ut_ad(state == buf_page_t::REMOVE_HASH);
block_and_ahi_release_and_fail:
		if (latch_mode == BTR_SEARCH_LEAF) {
			block->page.lock.s_unlock();
		} else {
			block->page.lock.x_unlock();
		}
		goto ahi_release_and_fail;
	}

	ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);
	ut_ad(state < buf_page_t::READ_FIX || latch_mode == BTR_SEARCH_LEAF);

	if (index != block->index && index_id == block->index->id) {
		ut_a(block->index->freed());
		goto block_and_ahi_release_and_fail;
	}

	block->page.fix();
	buf_page_make_young_if_needed(&block->page);
	static_assert(ulint{MTR_MEMO_PAGE_S_FIX} == ulint{BTR_SEARCH_LEAF},
		      "");
	static_assert(ulint{MTR_MEMO_PAGE_X_FIX} == ulint{BTR_MODIFY_LEAF},
		      "");

	part->latch.rd_unlock();

	++buf_pool.stat.n_page_gets;

	mtr->memo_push(block, mtr_memo_type_t(latch_mode));

	ut_ad(page_rec_is_user_rec(rec));

	btr_cur_position(index, (rec_t*) rec, block, cursor);

	/* Check the validity of the guess within the page */

	/* If we only have the latch on search system, not on the
	page, it only protects the columns of the record the cursor
	is positioned on. We cannot look at the next of the previous
	record to determine if our guess for the cursor position is
	right. */
	if (index_id != btr_page_get_index_id(block->page.frame)
	    || !btr_search_check_guess(cursor, false, tuple, mode)) {
		mtr->release_last_page();
		goto fail;
	}

	if (info->n_hash_potential < BTR_SEARCH_BUILD_LIMIT + 5) {

		info->n_hash_potential++;
	}

	info->last_hash_succ = TRUE;

#ifdef UNIV_SEARCH_PERF_STAT
	btr_search_n_succ++;
#endif
	return true;
}

/** Drop any adaptive hash index entries that point to an index page.
@param[in,out]	block	block containing index page, s- or x-latched, or an
			index page for which we know that
			block->buf_fix_count == 0 or it is an index page which
			has already been removed from the buf_pool.page_hash
			i.e.: it is in state BUF_BLOCK_REMOVE_HASH
@param[in]	garbage_collect	drop ahi only if the index is marked
				as freed */
void btr_search_drop_page_hash_index(buf_block_t* block,
				     bool garbage_collect)
{
	ulint			n_fields;
	ulint			n_bytes;
	const rec_t*		rec;
	mem_heap_t*		heap;
	rec_offs*		offsets;

retry:
	if (!block->index) {
		return;
	}

	ut_d(const auto state = block->page.state());
	ut_ad(state == buf_page_t::REMOVE_HASH
	      || state >= buf_page_t::UNFIXED);
	ut_ad(state == buf_page_t::REMOVE_HASH
	      || !(~buf_page_t::LRU_MASK & state)
	      || block->page.lock.have_any());
	ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);
	ut_ad(page_is_leaf(block->page.frame));

	/* We must not dereference block->index here, because it could be freed
	if (!index->table->get_ref_count() && !dict_sys.frozen()).
	Determine the ahi_slot based on the block contents. */

	const index_id_t	index_id
		= btr_page_get_index_id(block->page.frame);

	auto part = btr_search_sys.get_part(index_id,
					    block->page.id().space());

	part->latch.rd_lock(SRW_LOCK_CALL_ false);

	dict_index_t* index = block->index;
	bool is_freed = index && index->freed();

	if (is_freed) {
		part->latch.rd_unlock();
		part->latch.wr_lock(SRW_LOCK_CALL_ false);
		if (index != block->index) {
			part->latch.wr_unlock();
			goto retry;
		}
	} else if (garbage_collect) {
		part->latch.rd_unlock();
		return;
	}

	assert_block_ahi_valid(block);

	if (!index || !btr_search_enabled) {
		if (is_freed) {
			part->latch.wr_unlock();
		} else {
			part->latch.rd_unlock();
		}
		return;
	}

	ut_ad(!index->table->is_temporary());
	ut_ad(btr_search_enabled);

	ut_ad(block->page.id().space() == index->table->space_id);
	ut_a(index_id == index->id);

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;

	/* NOTE: The AHI fields of block must not be accessed after
	releasing search latch, as the index page might only be s-latched! */

	if (!is_freed) {
		part->latch.rd_unlock();
	}

	ut_a(n_fields > 0 || n_bytes > 0);

	const page_t* const page = block->page.frame;
	ulint n_recs = page_get_n_recs(page);
	if (!n_recs) {
		ut_ad("corrupted adaptive hash index" == 0);
		return;
	}

	/* Calculate and cache fold values into an array for fast deletion
	from the hash index */

	const auto comp = page_is_comp(page);
	ulint* folds;
	ulint n_cached = 0;
	ulint prev_fold = 0;

	if (UNIV_LIKELY(comp != 0)) {
		rec = page_rec_next_get<true>(page, page + PAGE_NEW_INFIMUM);
		if (rec && rec_is_metadata(rec, TRUE)) {
			rec = page_rec_next_get<true>(page, rec);
skipped_metadata:
			if (!--n_recs) {
				/* The page only contains the hidden
				metadata record for instant ALTER
				TABLE that the adaptive hash index
				never points to. */
				folds = nullptr;
				goto all_deleted;
			}
		}
	} else {
		rec = page_rec_next_get<false>(page, page + PAGE_OLD_INFIMUM);
		if (rec && rec_is_metadata(rec, FALSE)) {
			rec = page_rec_next_get<false>(page, rec);
			goto skipped_metadata;
		}
	}

	folds = (ulint*) ut_malloc_nokey(n_recs * sizeof(ulint));
	heap = nullptr;
	offsets = nullptr;

	while (rec) {
		if (n_cached >= n_recs) {
			ut_ad(page_rec_is_supremum(rec));
			break;
		}
		ut_ad(page_rec_is_user_rec(rec));
		offsets = rec_get_offsets(
			rec, index, offsets, index->n_core_fields,
			btr_search_get_n_fields(n_fields, n_bytes),
			&heap);
		const ulint fold = rec_fold(rec, offsets, n_fields, n_bytes,
					    index_id);

		if (fold == prev_fold && prev_fold != 0) {

			goto next_rec;
		}

		/* Remove all hash nodes pointing to this page from the
		hash chain */
		folds[n_cached++] = fold;

next_rec:
		if (comp) {
			rec = page_rec_next_get<true>(page, rec);
			if (!rec || rec == page + PAGE_NEW_SUPREMUM) {
				break;
			}
		} else {
			rec = page_rec_next_get<false>(page, rec);
			if (!rec || rec == page + PAGE_OLD_SUPREMUM) {
				break;
			}
		}
		prev_fold = fold;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

all_deleted:
	if (!is_freed) {
		part->latch.wr_lock(SRW_LOCK_CALL_ false);

		if (UNIV_UNLIKELY(!block->index)) {
			/* Someone else has meanwhile dropped the
			hash index */
			goto cleanup;
		}

		ut_a(block->index == index);
	}

	if (block->curr_n_fields != n_fields
	    || block->curr_n_bytes != n_bytes) {

		/* Someone else has meanwhile built a new hash index on the
		page, with different parameters */

		part->latch.wr_unlock();

		ut_free(folds);
		goto retry;
	}

	for (ulint i = 0; i < n_cached; i++) {
		ha_remove_all_nodes_to_page(&part->table, part->heap,
					    folds[i], page);
	}

	switch (index->search_info->ref_count--) {
	case 0:
		ut_error;
	case 1:
		if (index->freed()) {
			btr_search_lazy_free(index);
		}
	}

	block->index = nullptr;

	MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_REMOVED);
	MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_REMOVED, n_cached);

cleanup:
	assert_block_ahi_valid(block);
	part->latch.wr_unlock();

	ut_free(folds);
}

/** Drop possible adaptive hash index entries when a page is evicted
from the buffer pool or freed in a file, or the index is being dropped.
@param[in]	page_id		page id */
void btr_search_drop_page_hash_when_freed(const page_id_t page_id)
{
	buf_block_t*	block;
	mtr_t		mtr;

	mtr_start(&mtr);

	/* If the caller has a latch on the page, then the caller must
	have a x-latch on the page and it must have already dropped
	the hash index for the page. Because of the x-latch that we
	are possibly holding, we cannot s-latch the page, but must
	(recursively) x-latch it, even though we are only reading. */

	block = buf_page_get_gen(page_id, 0, RW_X_LATCH, NULL,
				 BUF_PEEK_IF_IN_POOL, &mtr);

	if (block && block->index) {
		/* In all our callers, the table handle should
		be open, or we should be in the process of
		dropping the table (preventing eviction). */
		DBUG_ASSERT(block->index->table->get_ref_count()
			    || dict_sys.locked());
		btr_search_drop_page_hash_index(block, false);
	}

	mtr_commit(&mtr);
}

/** Build a hash index on a page with the given parameters. If the page already
has a hash index with different parameters, the old hash index is removed.
If index is non-NULL, this function checks if n_fields and n_bytes are
sensible, and does not build a hash index if not.
@param[in,out]	index		index for which to build.
@param[in,out]	block		index page, s-/x- latched.
@param[in,out]	ahi_latch	the adaptive search latch
@param[in]	n_fields	hash this many full fields
@param[in]	n_bytes		hash this many bytes of the next field
@param[in]	left_side	hash for searches from left side */
static
void
btr_search_build_page_hash_index(
	dict_index_t*	index,
	buf_block_t*	block,
	srw_spin_lock*	ahi_latch,
	uint16_t	n_fields,
	uint16_t	n_bytes,
	bool		left_side)
{
	const rec_t*	rec;
	ulint		fold;
	ulint		next_fold;
	ulint		n_cached;
	ulint		n_recs;
	ulint*		folds;
	const rec_t**	recs;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;

	ut_ad(!index->table->is_temporary());

	if (!btr_search_enabled) {
		return;
	}

	rec_offs_init(offsets_);
	ut_ad(ahi_latch == &btr_search_sys.get_part(*index)->latch);
	ut_ad(index);
	ut_ad(block->page.id().space() == index->table->space_id);
	ut_ad(page_is_leaf(block->page.frame));

	ut_ad(block->page.lock.have_x() || block->page.lock.have_s());
	ut_ad(block->page.id().page_no() >= 3);

	ahi_latch->rd_lock(SRW_LOCK_CALL_ false);

	const bool enabled = btr_search_enabled;
	const bool rebuild = enabled && block->index
		&& (block->curr_n_fields != n_fields
		    || block->curr_n_bytes != n_bytes
		    || block->curr_left_side != left_side);

	ahi_latch->rd_unlock();

	if (!enabled) {
		return;
	}

	if (rebuild) {
		btr_search_drop_page_hash_index(block, false);
	}

	/* Check that the values for hash index build are sensible */

	if (n_fields == 0 && n_bytes == 0) {

		return;
	}

	if (dict_index_get_n_unique_in_tree(index)
	    < btr_search_get_n_fields(n_fields, n_bytes)) {
		return;
	}

	page_t*		page	= buf_block_get_frame(block);
	n_recs = page_get_n_recs(page);

	if (n_recs == 0) {

		return;
	}

	rec = page_rec_get_next_const(page_get_infimum_rec(page));
        if (!rec) return;

	if (rec_is_metadata(rec, *index)) {
		rec = page_rec_get_next_const(rec);
		if (!rec || !--n_recs) return;
	}

	/* Calculate and cache fold values and corresponding records into
	an array for fast insertion to the hash index */

	folds = static_cast<ulint*>(ut_malloc_nokey(n_recs * sizeof *folds));
	recs = static_cast<const rec_t**>(
		ut_malloc_nokey(n_recs * sizeof *recs));

	n_cached = 0;

	ut_a(index->id == btr_page_get_index_id(page));

	offsets = rec_get_offsets(
		rec, index, offsets, index->n_core_fields,
		btr_search_get_n_fields(n_fields, n_bytes),
		&heap);
	ut_ad(page_rec_is_supremum(rec)
	      || n_fields == rec_offs_n_fields(offsets) - (n_bytes > 0));

	fold = rec_fold(rec, offsets, n_fields, n_bytes, index->id);

	if (left_side) {

		folds[n_cached] = fold;
		recs[n_cached] = rec;
		n_cached++;
	}

	while (const rec_t* next_rec = page_rec_get_next_const(rec)) {
		if (page_rec_is_supremum(next_rec)) {

			if (!left_side) {

				folds[n_cached] = fold;
				recs[n_cached] = rec;
				n_cached++;
			}

			break;
		}

		offsets = rec_get_offsets(
			next_rec, index, offsets, index->n_core_fields,
			btr_search_get_n_fields(n_fields, n_bytes), &heap);
		next_fold = rec_fold(next_rec, offsets, n_fields,
				     n_bytes, index->id);

		if (fold != next_fold) {
			/* Insert an entry into the hash index */

			if (left_side) {

				folds[n_cached] = next_fold;
				recs[n_cached] = next_rec;
				n_cached++;
			} else {
				folds[n_cached] = fold;
				recs[n_cached] = rec;
				n_cached++;
			}
		}

		rec = next_rec;
		fold = next_fold;
	}

	btr_search_check_free_space_in_heap(index);

	ahi_latch->wr_lock(SRW_LOCK_CALL_ false);

	if (!btr_search_enabled) {
		goto exit_func;
	}

	/* This counter is decremented every time we drop page
	hash index entries and is incremented here. Since we can
	rebuild hash index for a page that is already hashed, we
	have to take care not to increment the counter in that
	case. */
	if (!block->index) {
		assert_block_ahi_empty(block);
		index->search_info->ref_count++;
	} else if (block->curr_n_fields != n_fields
		   || block->curr_n_bytes != n_bytes
		   || block->curr_left_side != left_side) {
		goto exit_func;
	}

	block->n_hash_helps = 0;

	block->curr_n_fields = n_fields & dict_index_t::MAX_N_FIELDS;
	block->curr_n_bytes = n_bytes & ((1U << 15) - 1);
	block->curr_left_side = left_side;
	block->index = index;

	{
		auto part = btr_search_sys.get_part(*index);
		for (ulint i = 0; i < n_cached; i++) {
			ha_insert_for_fold(&part->table, part->heap,
					   folds[i], block, recs[i]);
		}
	}

	MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_ADDED);
	MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_ADDED, n_cached);
exit_func:
	assert_block_ahi_valid(block);
	ahi_latch->wr_unlock();

	ut_free(folds);
	ut_free(recs);
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

/** Updates the search info.
@param[in,out]	info	search info
@param[in,out]	cursor	cursor which was just positioned */
void btr_search_info_update_slow(btr_search_t *info, btr_cur_t *cursor)
{
	srw_spin_lock*	ahi_latch = &btr_search_sys.get_part(*cursor->index())
		->latch;
	buf_block_t*	block = btr_cur_get_block(cursor);

	/* NOTE that the following two function calls do NOT protect
	info or block->n_fields etc. with any semaphore, to save CPU time!
	We cannot assume the fields are consistent when we return from
	those functions! */

	btr_search_info_update_hash(info, cursor);

	bool build_index = btr_search_update_block_hash_info(info, block);

	if (build_index || (cursor->flag == BTR_CUR_HASH_FAIL)) {

		btr_search_check_free_space_in_heap(cursor->index());
	}

	if (cursor->flag == BTR_CUR_HASH_FAIL) {
		/* Update the hash node reference, if appropriate */

#ifdef UNIV_SEARCH_PERF_STAT
		btr_search_n_hash_fail++;
#endif /* UNIV_SEARCH_PERF_STAT */

		btr_search_update_hash_ref(info, block, cursor);
	}

	if (build_index) {
		/* Note that since we did not protect block->n_fields etc.
		with any semaphore, the values can be inconsistent. We have
		to check inside the function call that they make sense. */
		btr_search_build_page_hash_index(cursor->index(), block,
						 ahi_latch,
						 block->n_fields,
						 block->n_bytes,
						 block->left_side);
	}
}

/** Move or delete hash entries for moved records, usually in a page split.
If new_block is already hashed, then any hash index for block is dropped.
If new_block is not hashed, and block is hashed, then a new hash index is
built to new_block with the same parameters as block.
@param[in,out]	new_block	destination page
@param[in,out]	block		source page (subject to deletion later) */
void
btr_search_move_or_delete_hash_entries(
	buf_block_t*	new_block,
	buf_block_t*	block)
{
	ut_ad(block->page.lock.have_x());
	ut_ad(new_block->page.lock.have_x());

	if (!btr_search_enabled) {
		return;
	}

	dict_index_t* index = block->index;
	if (!index) {
		index = new_block->index;
	} else {
		ut_ad(!new_block->index || index == new_block->index);
	}
	assert_block_ahi_valid(block);
	assert_block_ahi_valid(new_block);

	srw_spin_lock* ahi_latch = index
		? &btr_search_sys.get_part(*index)->latch
		: nullptr;

	if (new_block->index) {
drop_exit:
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	if (!index) {
		return;
	}

	ahi_latch->rd_lock(SRW_LOCK_CALL_ false);

	if (index->freed()) {
		ahi_latch->rd_unlock();
		goto drop_exit;
	}

	if (block->index) {
		uint16_t n_fields = block->curr_n_fields;
		uint16_t n_bytes = block->curr_n_bytes;
		bool left_side = block->curr_left_side;

		new_block->n_fields = block->curr_n_fields;
		new_block->n_bytes = block->curr_n_bytes;
		new_block->left_side = left_side;

		ahi_latch->rd_unlock();

		ut_a(n_fields > 0 || n_bytes > 0);

		btr_search_build_page_hash_index(
			index, new_block, ahi_latch,
			n_fields, n_bytes, left_side);
		ut_ad(n_fields == block->curr_n_fields);
		ut_ad(n_bytes == block->curr_n_bytes);
		ut_ad(left_side == block->curr_left_side);
		return;
	}

	ahi_latch->rd_unlock();
}

/** Updates the page hash index when a single record is deleted from a page.
@param[in]	cursor	cursor which was positioned on the record to delete
			using btr_cur_search_, the record is not yet deleted.*/
void btr_search_update_hash_on_delete(btr_cur_t *cursor)
{
	buf_block_t*	block;
	const rec_t*	rec;
	ulint		fold;
	dict_index_t*	index;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	mem_heap_t*	heap		= NULL;
	rec_offs_init(offsets_);

	ut_ad(page_is_leaf(btr_cur_get_page(cursor)));

	if (!btr_search_enabled) {
		return;
	}

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.lock.have_x());

	assert_block_ahi_valid(block);
	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(!cursor->index()->table->is_temporary());

	if (index != cursor->index()) {
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_ad(block->page.id().space() == index->table->space_id);
	ut_a(index == cursor->index());
	ut_a(block->curr_n_fields > 0 || block->curr_n_bytes > 0);

	rec = btr_cur_get_rec(cursor);

	fold = rec_fold(rec, rec_get_offsets(rec, index, offsets_,
					     index->n_core_fields,
					     ULINT_UNDEFINED, &heap),
			block->curr_n_fields, block->curr_n_bytes, index->id);
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	auto part = btr_search_sys.get_part(*index);

	part->latch.wr_lock(SRW_LOCK_CALL_ false);
	assert_block_ahi_valid(block);

	if (block->index && btr_search_enabled) {
		ut_a(block->index == index);

		if (ha_search_and_delete_if_found(&part->table, part->heap,
						  fold, rec)) {
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVED);
		} else {
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVE_NOT_FOUND);
		}

		assert_block_ahi_valid(block);
	}

	part->latch.wr_unlock();
}

/** Updates the page hash index when a single record is inserted on a page.
@param[in]	cursor	cursor which was positioned to the place to insert
			using btr_cur_search_, and the new record has been
			inserted next to the cursor.
@param[in]	ahi_latch	the adaptive hash index latch */
void btr_search_update_hash_node_on_insert(btr_cur_t *cursor,
                                           srw_spin_lock *ahi_latch)
{
	buf_block_t*	block;
	dict_index_t*	index;
	rec_t*		rec;

	ut_ad(ahi_latch == &btr_search_sys.get_part(*cursor->index())->latch);

	if (!btr_search_enabled) {
		return;
	}

	rec = btr_cur_get_rec(cursor);

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.lock.have_x());

	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(!cursor->index()->table->is_temporary());

	if (index != cursor->index()) {
		ut_ad(index->id == cursor->index()->id);
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_a(cursor->index() == index);
	ahi_latch->wr_lock(SRW_LOCK_CALL_ false);

	if (!block->index || !btr_search_enabled) {

		goto func_exit;
	}

	ut_a(block->index == index);

	if ((cursor->flag == BTR_CUR_HASH)
	    && (cursor->n_fields == block->curr_n_fields)
	    && (cursor->n_bytes == block->curr_n_bytes)
	    && !block->curr_left_side) {
		if (const rec_t *new_rec = page_rec_get_next_const(rec)) {
			if (ha_search_and_update_if_found(
				&btr_search_sys.get_part(*cursor->index())
				->table,
				cursor->fold, rec, block, new_rec)) {
				MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_UPDATED);
			}
		} else {
			ut_ad("corrupted page" == 0);
		}

func_exit:
		assert_block_ahi_valid(block);
		ahi_latch->wr_unlock();
	} else {
		ahi_latch->wr_unlock();

		btr_search_update_hash_on_insert(cursor, ahi_latch);
	}
}

/** Updates the page hash index when a single record is inserted on a page.
@param[in,out]	cursor		cursor which was positioned to the
				place to insert using btr_cur_search_...,
				and the new record has been inserted next
				to the cursor
@param[in]	ahi_latch	the adaptive hash index latch */
void btr_search_update_hash_on_insert(btr_cur_t *cursor,
                                      srw_spin_lock *ahi_latch)
{
	buf_block_t*	block;
	dict_index_t*	index;
	const rec_t*	rec;
	const rec_t*	ins_rec;
	const rec_t*	next_rec;
	ulint		fold;
	ulint		ins_fold;
	ulint		next_fold = 0; /* remove warning (??? bug ???) */
	ulint		n_fields;
	ulint		n_bytes;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(ahi_latch == &btr_search_sys.get_part(*cursor->index())->latch);
	ut_ad(page_is_leaf(btr_cur_get_page(cursor)));

	if (!btr_search_enabled) {
		return;
	}

	block = btr_cur_get_block(cursor);

	ut_ad(block->page.lock.have_x());
	assert_block_ahi_valid(block);

	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(block->page.id().space() == index->table->space_id);
	btr_search_check_free_space_in_heap(index);

	rec = btr_cur_get_rec(cursor);

	ut_ad(!cursor->index()->table->is_temporary());

	if (index != cursor->index()) {
		ut_ad(index->id == cursor->index()->id);
drop:
		btr_search_drop_page_hash_index(block, false);
		return;
	}

	ut_a(index == cursor->index());

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;
	const bool left_side = block->curr_left_side;

	ins_rec = page_rec_get_next_const(rec);
	if (UNIV_UNLIKELY(!ins_rec)) goto drop;
	next_rec = page_rec_get_next_const(ins_rec);
	if (UNIV_UNLIKELY(!next_rec)) goto drop;

	offsets = rec_get_offsets(ins_rec, index, offsets,
				  index->n_core_fields,
				  ULINT_UNDEFINED, &heap);
	ins_fold = rec_fold(ins_rec, offsets, n_fields, n_bytes, index->id);

	if (!page_rec_is_supremum(next_rec)) {
		offsets = rec_get_offsets(
			next_rec, index, offsets, index->n_core_fields,
			btr_search_get_n_fields(n_fields, n_bytes), &heap);
		next_fold = rec_fold(next_rec, offsets, n_fields,
				     n_bytes, index->id);
	}

	/* We must not look up "part" before acquiring ahi_latch. */
	btr_search_sys_t::partition* part= nullptr;
	bool locked = false;

	if (!page_rec_is_infimum(rec) && !rec_is_metadata(rec, *index)) {
		offsets = rec_get_offsets(
			rec, index, offsets, index->n_core_fields,
			btr_search_get_n_fields(n_fields, n_bytes), &heap);
		fold = rec_fold(rec, offsets, n_fields, n_bytes, index->id);
	} else {
		if (left_side) {
			locked = true;
			ahi_latch->wr_lock(SRW_LOCK_CALL_ false);

			if (!btr_search_enabled || !block->index) {
				goto function_exit;
			}

			part = btr_search_sys.get_part(*index);
			ha_insert_for_fold(&part->table, part->heap,
					   ins_fold, block, ins_rec);
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
		}

		goto check_next_rec;
	}

	if (fold != ins_fold) {

		if (!locked) {
			locked = true;
			ahi_latch->wr_lock(SRW_LOCK_CALL_ false);

			if (!btr_search_enabled || !block->index) {
				goto function_exit;
			}

			part = btr_search_sys.get_part(*index);
		}

		if (!left_side) {
			ha_insert_for_fold(&part->table, part->heap,
					   fold, block, rec);
		} else {
			ha_insert_for_fold(&part->table, part->heap,
					   ins_fold, block, ins_rec);
		}
		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

check_next_rec:
	if (page_rec_is_supremum(next_rec)) {

		if (!left_side) {
			if (!locked) {
				locked = true;
				ahi_latch->wr_lock(SRW_LOCK_CALL_ false);

				if (!btr_search_enabled || !block->index) {
					goto function_exit;
				}

				part = btr_search_sys.get_part(*index);
			}

			ha_insert_for_fold(&part->table, part->heap,
					   ins_fold, block, ins_rec);
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
		}

		goto function_exit;
	}

	if (ins_fold != next_fold) {
		if (!locked) {
			locked = true;
			ahi_latch->wr_lock(SRW_LOCK_CALL_ false);

			if (!btr_search_enabled || !block->index) {
				goto function_exit;
			}

			part = btr_search_sys.get_part(*index);
		}

		if (!left_side) {
			ha_insert_for_fold(&part->table, part->heap,
					   ins_fold, block, ins_rec);
		} else {
			ha_insert_for_fold(&part->table, part->heap,
					   next_fold, block, next_rec);
		}
		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

function_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	if (locked) {
		ahi_latch->wr_unlock();
	}
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
__attribute__((nonnull))
/** @return whether a range of the cells is valid */
static bool ha_validate(const hash_table_t *table,
                        ulint start_index, ulint end_index)
{
  ut_a(start_index <= end_index);
  ut_a(end_index < table->n_cells);

  bool ok= true;

  for (ulint i= start_index; i <= end_index; i++)
  {
    for (auto node= static_cast<const ha_node_t*>(table->array[i].node); node;
         node= node->next)
    {
      if (table->calc_hash(node->fold) != i) {
        ib::error() << "Hash table node fold value " << node->fold
		    << " does not match the cell number " << i;
	ok= false;
      }
    }
  }

  return ok;
}

/** Validates the search system for given hash table.
@param thd            connection, for checking if CHECK TABLE has been killed
@param hash_table_id  hash table to validate
@return true if ok */
static bool btr_search_hash_table_validate(THD *thd, ulint hash_table_id)
{
	ha_node_t*	node;
	bool		ok		= true;
	ulint		i;
	ulint		cell_count;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;

	btr_search_x_lock_all();
	if (!btr_search_enabled || (thd && thd_kill_level(thd))) {
func_exit:
		btr_search_x_unlock_all();

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}

		return ok;
	}

	/* How many cells to check before temporarily releasing
	search latches. */
	ulint		chunk_size = 10000;

	rec_offs_init(offsets_);

	mysql_mutex_lock(&buf_pool.mutex);

	auto &part = btr_search_sys.parts[hash_table_id];

	cell_count = part.table.n_cells;

	for (i = 0; i < cell_count; i++) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if ((i != 0) && ((i % chunk_size) == 0)) {

			mysql_mutex_unlock(&buf_pool.mutex);
			btr_search_x_unlock_all();

			std::this_thread::yield();

			btr_search_x_lock_all();

			if (!btr_search_enabled
			    || (thd && thd_kill_level(thd))) {
				goto func_exit;
			}

			mysql_mutex_lock(&buf_pool.mutex);

			ulint curr_cell_count = part.table.n_cells;

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		node = static_cast<ha_node_t*>(part.table.array[i].node);

		for (; node != NULL; node = node->next) {
			const buf_block_t*	block
				= buf_pool.block_from(node->data);
			index_id_t		page_index_id;

			if (UNIV_LIKELY(block->page.in_file())) {
				/* The space and offset are only valid
				for file blocks.  It is possible that
				the block is being freed
				(BUF_BLOCK_REMOVE_HASH, see the
				assertion and the comment below) */
				const page_id_t id(block->page.id());
				if (const buf_page_t* hash_page
				    = buf_pool.page_hash.get(
					    id, buf_pool.page_hash.cell_get(
						    id.fold()))) {
					ut_ad(hash_page == &block->page);
					goto state_ok;
				}
			}

			/* When a block is being freed,
			buf_LRU_search_and_free_block() first removes
			the block from buf_pool.page_hash by calling
			buf_LRU_block_remove_hashed_page(). Then it
			invokes btr_search_drop_page_hash_index(). */
			ut_a(block->page.state() == buf_page_t::REMOVE_HASH);
state_ok:
			ut_ad(block->page.id().space()
			      == block->index->table->space_id);

			const page_t* page = block->page.frame;

			page_index_id = btr_page_get_index_id(page);

			offsets = rec_get_offsets(
				node->data, block->index, offsets,
				block->index->n_core_fields,
				btr_search_get_n_fields(block->curr_n_fields,
							block->curr_n_bytes),
				&heap);

			const ulint	fold = rec_fold(
				node->data, offsets,
				block->curr_n_fields,
				block->curr_n_bytes,
				page_index_id);

			if (node->fold != fold) {
				ok = FALSE;

				ib::error() << "Error in an adaptive hash"
					<< " index pointer to page "
					<< block->page.id()
					<< ", ptr mem address "
					<< reinterpret_cast<const void*>(
						node->data)
					<< ", index id " << page_index_id
					<< ", node fold " << node->fold
					<< ", rec fold " << fold;

				fputs("InnoDB: Record ", stderr);
				rec_print_new(stderr, node->data, offsets);
				fprintf(stderr, "\nInnoDB: on that page."
					" Page mem address %p, is hashed %p,"
					" n fields %lu\n"
					"InnoDB: side %lu\n",
					(void*) page, (void*) block->index,
					(ulong) block->curr_n_fields,
					(ulong) block->curr_left_side);
				ut_ad(0);
			}
		}
	}

	for (i = 0; i < cell_count; i += chunk_size) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if (i != 0) {
			mysql_mutex_unlock(&buf_pool.mutex);
			btr_search_x_unlock_all();

			std::this_thread::yield();

			btr_search_x_lock_all();

			if (!btr_search_enabled
			    || (thd && thd_kill_level(thd))) {
				goto func_exit;
			}

			mysql_mutex_lock(&buf_pool.mutex);

			ulint curr_cell_count = part.table.n_cells;

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		ulint end_index = ut_min(i + chunk_size - 1, cell_count - 1);

		if (!ha_validate(&part.table, i, end_index)) {
			ok = false;
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);
	goto func_exit;
}

/** Validates the search system.
@param thd   connection, for checking if CHECK TABLE has been killed
@return true if ok */
bool btr_search_validate(THD *thd)
{
  for (ulint i= 0; i < btr_ahi_parts; ++i)
    if (!btr_search_hash_table_validate(thd, i))
      return(false);
  return true;
}

#ifdef UNIV_DEBUG
bool btr_search_check_marked_free_index(const buf_block_t *block)
{
  const index_id_t index_id= btr_page_get_index_id(block->page.frame);
  auto part= btr_search_sys.get_part(index_id, block->page.id().space());

  part->latch.rd_lock(SRW_LOCK_CALL_ false);

  bool is_freed= block->index && block->index->freed();

  part->latch.rd_unlock();

  return is_freed;
}
#endif /* UNIV_DEBUG */
#endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
