/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2017, 2021, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

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
#include "page0page.h"
#include "page0cur.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "ha0ha.h"
#include "srv0mon.h"
#include "sync0sync.h"

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

/** padding to prevent other memory update
hotspots from residing on the same memory
cache line as btr_search_latches */
UNIV_INTERN byte		btr_sea_pad1[CACHE_LINE_SIZE];

/** The latches protecting the adaptive search system: this latches protects the
(1) positions of records on those pages where a hash index has been built.
NOTE: It does not protect values of non-ordering fields within a record from
being updated in-place! We can use fact (1) to perform unique searches to
indexes. We will allocate the latches from dynamic memory to get it to the
same DRAM page as other hotspot semaphores */
rw_lock_t**	btr_search_latches;

/** padding to prevent other memory update hotspots from residing on
the same memory cache line */
UNIV_INTERN byte		btr_sea_pad2[CACHE_LINE_SIZE];

/** The adaptive hash index */
btr_search_sys_t*	btr_search_sys;

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
static
void
btr_search_check_free_space_in_heap(const dict_index_t* index)
{
	/* Note that we peek the value of heap->free_block without reserving
	the latch: this is ok, because we will not guarantee that there will
	be enough free space in the hash table. */

	buf_block_t*	block = buf_block_alloc(NULL);
	rw_lock_t*	latch = btr_get_search_latch(index);
	hash_table_t*	table;
	mem_heap_t*	heap;

	rw_lock_x_lock(latch);

	if (!btr_search_enabled) {
		goto func_exit;
	}

	table = btr_get_search_table(index);
	heap = table->heap;

	if (heap->free_block == NULL) {
		heap->free_block = block;
	} else {
func_exit:
		buf_block_free(block);
	}

	rw_lock_x_unlock(latch);
}

/** Creates and initializes the adaptive search system at a database start.
@param[in]	hash_size	hash table size. */
void btr_search_sys_create(ulint hash_size)
{
	/* Search System is divided into n parts.
	Each part controls access to distinct set of hash buckets from
	hash table through its own latch. */

	/* Step-1: Allocate latches (1 per part). */
	btr_search_latches = reinterpret_cast<rw_lock_t**>(
		ut_malloc(sizeof(rw_lock_t*) * btr_ahi_parts, mem_key_ahi));

	for (ulint i = 0; i < btr_ahi_parts; ++i) {

		btr_search_latches[i] = reinterpret_cast<rw_lock_t*>(
			ut_malloc(sizeof(rw_lock_t), mem_key_ahi));

		rw_lock_create(btr_search_latch_key,
			       btr_search_latches[i], SYNC_SEARCH_SYS);
	}

	/* Step-2: Allocate hash tablees. */
	btr_search_sys = reinterpret_cast<btr_search_sys_t*>(
		ut_malloc(sizeof(btr_search_sys_t), mem_key_ahi));

	btr_search_sys->hash_tables = NULL;

	if (btr_search_enabled) {
		btr_search_enable();
	}
}

/** Frees the adaptive search system at a database shutdown. */
void btr_search_sys_free()
{
  if (!btr_search_sys)
  {
    ut_ad(!btr_search_latches);
    return;
  }

  ut_ad(btr_search_sys);
  ut_ad(btr_search_latches);

  if (btr_search_sys->hash_tables)
  {
    for (ulint i= 0; i < btr_ahi_parts; ++i)
    {
      mem_heap_free(btr_search_sys->hash_tables[i]->heap);
      hash_table_free(btr_search_sys->hash_tables[i]);
    }
    ut_free(btr_search_sys->hash_tables);
  }

  ut_free(btr_search_sys);
  btr_search_sys= NULL;

  /* Free all latches. */
  for (ulint i= 0; i < btr_ahi_parts; ++i)
  {
    rw_lock_free(btr_search_latches[i]);
    ut_free(btr_search_latches[i]);
  }

  ut_free(btr_search_latches);
  btr_search_latches= NULL;
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
ATTRIBUTE_COLD static void btr_search_lazy_free(dict_index_t *index)
{
  ut_ad(index->freed());
  dict_table_t *table= index->table;
  /* Perform the skipped steps of dict_index_remove_from_cache_low(). */
  UT_LIST_REMOVE(table->freed_indexes, index);
  rw_lock_free(&index->lock);
  dict_mem_index_free(index);

  if (!UT_LIST_GET_LEN(table->freed_indexes) &&
      !UT_LIST_GET_LEN(table->indexes))
  {
    ut_ad(table->id == 0);
    dict_mem_table_free(table);
  }
}

/** Clear the adaptive hash index on all pages in the buffer pool. */
static void buf_pool_clear_hash_index()
{
  ut_ad(btr_search_own_all(RW_LOCK_X));
  ut_ad(!btr_search_enabled);

  std::set<dict_index_t*> garbage;

  for (ulong p = 0; p < srv_buf_pool_instances; p++)
  {
    buf_pool_t *buf_pool= buf_pool_from_array(p);
    buf_chunk_t *chunks= buf_pool->chunks;
    buf_chunk_t *chunk= chunks + buf_pool->n_chunks;

    while (--chunk >= chunks)
    {
      buf_block_t *block= chunk->blocks;
      for (ulint i= chunk->size; i--; block++)
      {
        dict_index_t *index= block->index;
        assert_block_ahi_valid(block);

        /* We can clear block->index and block->n_pointers when
        btr_search_own_all(RW_LOCK_X); see the comments in buf0buf.h */

        if (!index)
        {
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
          ut_a(!block->n_pointers);
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
          continue;
        }

        ut_d(buf_page_state state= buf_block_get_state(block));
        /* Another thread may have set the state to
        BUF_BLOCK_REMOVE_HASH in buf_LRU_block_remove_hashed().

        The state change in buf_page_realloc() is not observable here,
        because in that case we would have !block->index.

        In the end, the entire adaptive hash index will be removed. */
        ut_ad(state == BUF_BLOCK_FILE_PAGE || state == BUF_BLOCK_REMOVE_HASH);
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
        block->n_pointers= 0;
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
        if (index->freed())
          garbage.insert(index);
        block->index= NULL;
      }
    }
  }

  for (std::set<dict_index_t*>::iterator i= garbage.begin();
       i != garbage.end(); i++)
    btr_search_lazy_free(*i);
}

/** Disable the adaptive hash search system and empty the index. */
void btr_search_disable()
{
	dict_table_t*	table;

	mutex_enter(&dict_sys.mutex);

	btr_search_x_lock_all();

	if (!btr_search_enabled) {
		mutex_exit(&dict_sys.mutex);
		btr_search_x_unlock_all();
		return;
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

	mutex_exit(&dict_sys.mutex);

	/* Set all block->index = NULL. */
	buf_pool_clear_hash_index();

	/* Clear the adaptive hash index. */
	for (ulint i = 0; i < btr_ahi_parts; ++i) {
		mem_heap_free(btr_search_sys->hash_tables[i]->heap);
		hash_table_free(btr_search_sys->hash_tables[i]);
	}
	ut_free(btr_search_sys->hash_tables);
	btr_search_sys->hash_tables = NULL;

	btr_search_x_unlock_all();
}

/** Enable the adaptive hash search system.
@param resize whether buf_pool_resize() is the caller */
void btr_search_enable(bool resize)
{
	if (!resize) {
		buf_pool_mutex_enter_all();
		if (srv_buf_pool_old_size != srv_buf_pool_size) {
			buf_pool_mutex_exit_all();
			return;
		}
		buf_pool_mutex_exit_all();
	}

	ulint hash_size = buf_pool_get_curr_size() / sizeof(void *) / 64;
	btr_search_x_lock_all();

	if (btr_search_sys->hash_tables) {
		ut_ad(btr_search_enabled);
		btr_search_x_unlock_all();
		return;
	}

	btr_search_sys->hash_tables = reinterpret_cast<hash_table_t**>(
		ut_malloc(sizeof(hash_table_t*) * btr_ahi_parts, mem_key_ahi));
	for (ulint i = 0; i < btr_ahi_parts; ++i) {
		btr_search_sys->hash_tables[i] =
			ib_create((hash_size / btr_ahi_parts),
				  LATCH_ID_HASH_TABLE_MUTEX,
				  0, MEM_HEAP_FOR_BTR_SEARCH);
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
                btr_search_sys->hash_tables[i]->adaptive = TRUE;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
	}

	btr_search_enabled = true;
	btr_search_x_unlock_all();
}

/** Updates the search info of an index about hash successes. NOTE that info
is NOT protected by any semaphore, to save CPU time! Do not assume its fields
are consistent.
@param[in,out]	info	search info
@param[in]	cursor	cursor which was just positioned */
static
void
btr_search_info_update_hash(
	btr_search_t*	info,
	btr_cur_t*	cursor)
{
	dict_index_t*	index = cursor->index;
	ulint		n_unique;
	int		cmp;

	ut_ad(!btr_search_own_any(RW_LOCK_S));
	ut_ad(!btr_search_own_any(RW_LOCK_X));

	if (dict_index_is_ibuf(index)) {
		/* So many deletes are performed on an insert buffer tree
		that we do not consider a hash index useful on it: */

		return;
	}

	n_unique = dict_index_get_n_unique_in_tree(index);

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
	if (cmp == 0) {
		info->n_hash_potential = 0;

		/* For extra safety, we set some sensible values here */

		info->n_fields = 1;
		info->n_bytes = 0;

		info->left_side = TRUE;

	} else if (cmp > 0) {
		info->n_hash_potential = 1;

		if (cursor->up_match >= n_unique) {

			info->n_fields = n_unique;
			info->n_bytes = 0;

		} else if (cursor->low_match < cursor->up_match) {

			info->n_fields = cursor->low_match + 1;
			info->n_bytes = 0;
		} else {
			info->n_fields = cursor->low_match;
			info->n_bytes = cursor->low_bytes + 1;
		}

		info->left_side = TRUE;
	} else {
		info->n_hash_potential = 1;

		if (cursor->low_match >= n_unique) {

			info->n_fields = n_unique;
			info->n_bytes = 0;
		} else if (cursor->low_match > cursor->up_match) {

			info->n_fields = cursor->up_match + 1;
			info->n_bytes = 0;
		} else {
			info->n_fields = cursor->up_match;
			info->n_bytes = cursor->up_bytes + 1;
		}

		info->left_side = FALSE;
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
	ut_ad(!btr_search_own_any());
	ut_ad(rw_lock_own_flagged(&block->lock,
				  RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	info->last_hash_succ = FALSE;

	ut_a(buf_block_state_valid(block));
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

	if ((block->n_hash_helps > page_get_n_recs(block->frame)
	     / BTR_SEARCH_PAGE_BUILD_LIMIT)
	    && (info->n_hash_potential >= BTR_SEARCH_BUILD_LIMIT)) {

		if ((!block->index)
		    || (block->n_hash_helps
			> 2U * page_get_n_recs(block->frame))
		    || (block->n_fields != block->curr_n_fields)
		    || (block->n_bytes != block->curr_n_bytes)
		    || (block->left_side != block->curr_left_side)) {

			/* Build a new hash index on the page */

			return(true);
		}
	}

	return(false);
}

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

	ut_ad(rw_lock_own_flagged(&block->lock,
				  RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));
	ut_ad(page_align(btr_cur_get_rec(cursor)) == block->frame);
	ut_ad(page_is_leaf(block->frame));
	assert_block_ahi_valid(block);

	dict_index_t* index = block->index;

	if (!index || !info->n_hash_potential) {
		return;
	}

	if (cursor->index != index) {
		ut_ad(cursor->index->id == index->id);
		btr_search_drop_page_hash_index(block);
		return;
	}

	ut_ad(block->page.id.space() == index->table->space_id);
	ut_ad(index == cursor->index);
	ut_ad(!dict_index_is_ibuf(index));
	rw_lock_t* const latch = btr_get_search_latch(index);
	rw_lock_x_lock(latch);
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

		ha_insert_for_fold(btr_get_search_table(index), fold,
				   block, rec);

		MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
	}

func_exit:
	rw_lock_x_unlock(latch);
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
	ibool		success		= FALSE;
	rec_offs_init(offsets_);

	n_unique = dict_index_get_n_unique_in_tree(cursor->index);

	rec = btr_cur_get_rec(cursor);

	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(page_rec_is_leaf(rec));

	match = 0;

	offsets = rec_get_offsets(rec, cursor->index, offsets,
				  cursor->index->n_core_fields,
				  n_unique, &heap);
	cmp = cmp_dtuple_rec_with_match(tuple, rec, offsets, &match);

	if (mode == PAGE_CUR_GE) {
		if (cmp > 0) {
			goto exit_func;
		}

		cursor->up_match = match;

		if (match >= n_unique) {
			success = TRUE;
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
		ut_ad(!page_rec_is_infimum(rec));

		const rec_t* prev_rec = page_rec_get_prev(rec);

		if (page_rec_is_infimum(prev_rec)) {
			success = !page_has_prev(page_align(prev_rec));
			goto exit_func;
		}

		offsets = rec_get_offsets(prev_rec, cursor->index, offsets,
					  cursor->index->n_core_fields,
					  n_unique, &heap);
		cmp = cmp_dtuple_rec_with_match(
			tuple, prev_rec, offsets, &match);
		if (mode == PAGE_CUR_GE) {
			success = cmp > 0;
		} else {
			success = cmp >= 0;
		}
	} else {
		ut_ad(!page_rec_is_supremum(rec));

		const rec_t* next_rec = page_rec_get_next(rec);

		if (page_rec_is_supremum(next_rec)) {
			if (!page_has_next(page_align(next_rec))) {
				cursor->up_match = 0;
				success = TRUE;
			}

			goto exit_func;
		}

		offsets = rec_get_offsets(next_rec, cursor->index, offsets,
					  cursor->index->n_core_fields,
					  n_unique, &heap);
		cmp = cmp_dtuple_rec_with_match(
			tuple, next_rec, offsets, &match);
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
@param[in]	latch_mode	BTR_SEARCH_LEAF, ...;
				NOTE that only if has_search_latch is 0, we will
				have a latch set on the cursor page, otherwise
				we assume the caller uses his search latch
				to protect the record!
@param[out]	cursor		tree cursor
@param[in]	ahi_latch	the adaptive hash index latch being held,
				or NULL
@param[in]	mtr		mini transaction
@return whether the search succeeded */
bool
btr_search_guess_on_hash(
	dict_index_t*	index,
	btr_search_t*	info,
	const dtuple_t*	tuple,
	ulint		mode,
	ulint		latch_mode,
	btr_cur_t*	cursor,
	rw_lock_t*	ahi_latch,
	mtr_t*		mtr)
{
	ulint		fold;
	index_id_t	index_id;
#ifdef notdefined
	btr_cur_t	cursor2;
	btr_pcur_t	pcur;
#endif
	ut_ad(!ahi_latch || rw_lock_own_flagged(
		      ahi_latch, RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	if (!btr_search_enabled) {
		return false;
	}

	ut_ad(index && info && tuple && cursor && mtr);
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(!ahi_latch || ahi_latch == btr_get_search_latch(index));
	ut_ad((latch_mode == BTR_SEARCH_LEAF)
	      || (latch_mode == BTR_MODIFY_LEAF));

	/* Not supported for spatial index */
	ut_ad(!dict_index_is_spatial(index));

	/* Note that, for efficiency, the struct info may not be protected by
	any latch here! */

	if (info->n_hash_potential == 0) {
		return false;
	}

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

	rw_lock_t* use_latch = ahi_latch ? NULL : btr_get_search_latch(index);
	const rec_t* rec;

	if (use_latch) {
		rw_lock_s_lock(use_latch);

		if (!btr_search_enabled) {
			goto fail;
		}
	} else {
		ut_ad(btr_search_enabled);
		ut_ad(rw_lock_own(ahi_latch, RW_LOCK_S));
	}

	rec = static_cast<const rec_t*>(
		ha_search_and_get_data(btr_get_search_table(index), fold));

	if (!rec) {
		if (use_latch) {
fail:
			rw_lock_s_unlock(use_latch);
		}

		btr_search_failure(info, cursor);
		return false;
	}

	buf_block_t*	block = buf_block_from_ahi(rec);

	if (use_latch) {
		if (!buf_page_get_known_nowait(
			latch_mode, block, BUF_MAKE_YOUNG,
			__FILE__, __LINE__, mtr)) {
			goto fail;
		}

		const bool fail = index != block->index
			&& index_id == block->index->id;
		ut_a(!fail || block->index->freed());
		rw_lock_s_unlock(use_latch);

		buf_block_dbg_add_level(block, SYNC_TREE_NODE_FROM_HASH);
		if (UNIV_UNLIKELY(fail)) {
			goto fail_and_release_page;
		}
	} else if (UNIV_UNLIKELY(index != block->index
				 && index_id == block->index->id)) {
		ut_a(block->index->freed());
		goto fail_and_release_page;
	}

	if (buf_block_get_state(block) != BUF_BLOCK_FILE_PAGE) {

		ut_ad(buf_block_get_state(block) == BUF_BLOCK_REMOVE_HASH);

fail_and_release_page:
		if (!ahi_latch) {
			btr_leaf_page_release(block, latch_mode, mtr);
		}

		btr_search_failure(info, cursor);
		return false;
	}

	ut_ad(page_rec_is_user_rec(rec));

	btr_cur_position(index, (rec_t*) rec, block, cursor);

	/* Check the validity of the guess within the page */

	/* If we only have the latch on search system, not on the
	page, it only protects the columns of the record the cursor
	is positioned on. We cannot look at the next of the previous
	record to determine if our guess for the cursor position is
	right. */
	if (index_id != btr_page_get_index_id(block->frame)
	    || !btr_search_check_guess(cursor, !!ahi_latch, tuple, mode)) {
		goto fail_and_release_page;
	}

	if (info->n_hash_potential < BTR_SEARCH_BUILD_LIMIT + 5) {

		info->n_hash_potential++;
	}

#ifdef notdefined
	/* These lines of code can be used in a debug version to check
	the correctness of the searched cursor position: */

	info->last_hash_succ = FALSE;

	/* Currently, does not work if the following fails: */
	ut_ad(!ahi_latch);

	btr_leaf_page_release(block, latch_mode, mtr);

	btr_cur_search_to_nth_level(
		index, 0, tuple, mode, latch_mode, &cursor2, 0, mtr);

	if (mode == PAGE_CUR_GE
	    && page_rec_is_supremum(btr_cur_get_rec(&cursor2))) {

		/* If mode is PAGE_CUR_GE, then the binary search
		in the index tree may actually take us to the supremum
		of the previous page */

		info->last_hash_succ = FALSE;

		btr_pcur_open_on_user_rec(
			index, tuple, mode, latch_mode, &pcur, mtr);

		ut_ad(btr_pcur_get_rec(&pcur) == btr_cur_get_rec(cursor));
	} else {
		ut_ad(btr_cur_get_rec(&cursor2) == btr_cur_get_rec(cursor));
	}

	/* NOTE that it is theoretically possible that the above assertions
	fail if the page of the cursor gets removed from the buffer pool
	meanwhile! Thus it might not be a bug. */
#endif
	info->last_hash_succ = TRUE;

#ifdef UNIV_SEARCH_PERF_STAT
	btr_search_n_succ++;
#endif
	if (!ahi_latch && buf_page_peek_if_too_old(&block->page)) {

		buf_page_make_young(&block->page);
	}

	/* Increment the page get statistics though we did not really
	fix the page: for user info only */
	{
		buf_pool_t*	buf_pool = buf_pool_from_bpage(&block->page);

		++buf_pool->stat.n_page_gets;
	}

	return true;
}

/** Drop any adaptive hash index entries that point to an index page.
@param[in,out]	block	block containing index page, s- or x-latched, or an
			index page for which we know that
			block->buf_fix_count == 0 or it is an index page which
			has already been removed from the buf_pool->page_hash
			i.e.: it is in state BUF_BLOCK_REMOVE_HASH */
void btr_search_drop_page_hash_index(buf_block_t* block)
{
	ulint			n_fields;
	ulint			n_bytes;
	const page_t*		page;
	const rec_t*		rec;
	ulint			fold;
	ulint			prev_fold;
	ulint			n_cached;
	ulint			n_recs;
	ulint*			folds;
	ulint			i;
	mem_heap_t*		heap;
	rec_offs*		offsets;
	rw_lock_t*		latch;

retry:
	/* This debug check uses a dirty read that could theoretically cause
	false positives while buf_pool_clear_hash_index() is executing. */
	assert_block_ahi_valid(block);
	ut_ad(!btr_search_own_any(RW_LOCK_S));
	ut_ad(!btr_search_own_any(RW_LOCK_X));

	if (!block->index) {
		return;
	}

	ut_ad(block->page.buf_fix_count == 0
	      || buf_block_get_state(block) == BUF_BLOCK_REMOVE_HASH
	      || rw_lock_own_flagged(&block->lock,
				     RW_LOCK_FLAG_X | RW_LOCK_FLAG_S
				     | RW_LOCK_FLAG_SX));
	ut_ad(page_is_leaf(block->frame));

	/* We must not dereference block->index here, because it could be freed
	if (index->table->n_ref_count == 0 && !mutex_own(&dict_sys.mutex)).
	Determine the ahi_slot based on the block contents. */

	const index_id_t	index_id
		= btr_page_get_index_id(block->frame);
	const ulint		ahi_slot
		= ut_fold_ulint_pair(static_cast<ulint>(index_id),
				     static_cast<ulint>(block->page.id.space()))
		% btr_ahi_parts;
	latch = btr_search_latches[ahi_slot];

	dict_index_t* index = block->index;

	bool is_freed = index && index->freed();
	if (is_freed) {
		rw_lock_x_lock(latch);
	} else {
		rw_lock_s_lock(latch);
	}

	assert_block_ahi_valid(block);

	if (!index || !btr_search_enabled) {
		if (is_freed) {
			rw_lock_x_unlock(latch);
		} else {
			rw_lock_s_unlock(latch);
		}
		return;
	}

#ifdef MYSQL_INDEX_DISABLE_AHI
	ut_ad(!index->disable_ahi);
#endif
	ut_ad(btr_search_enabled);

	ut_ad(block->page.id.space() == index->table->space_id);
	ut_a(index_id == index->id);
	ut_ad(!dict_index_is_ibuf(index));

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;

	/* NOTE: The AHI fields of block must not be accessed after
	releasing search latch, as the index page might only be s-latched! */

	if (!is_freed) {
		rw_lock_s_unlock(latch);
	}

	ut_a(n_fields > 0 || n_bytes > 0);

	page = block->frame;
	n_recs = page_get_n_recs(page);

	/* Calculate and cache fold values into an array for fast deletion
	from the hash index */

	folds = (ulint*) ut_malloc_nokey(n_recs * sizeof(ulint));

	n_cached = 0;

	rec = page_get_infimum_rec(page);
	rec = page_rec_get_next_low(rec, page_is_comp(page));
	if (rec_is_metadata(rec, *index)) {
		rec = page_rec_get_next_low(rec, page_is_comp(page));
	}

	prev_fold = 0;

	heap = NULL;
	offsets = NULL;

	while (!page_rec_is_supremum(rec)) {
		offsets = rec_get_offsets(
			rec, index, offsets, index->n_core_fields,
			btr_search_get_n_fields(n_fields, n_bytes),
			&heap);
		fold = rec_fold(rec, offsets, n_fields, n_bytes, index_id);

		if (fold == prev_fold && prev_fold != 0) {

			goto next_rec;
		}

		/* Remove all hash nodes pointing to this page from the
		hash chain */

		folds[n_cached] = fold;
		n_cached++;
next_rec:
		rec = page_rec_get_next_low(rec, page_rec_is_comp(rec));
		prev_fold = fold;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	if (!is_freed) {
		rw_lock_x_lock(latch);

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

		rw_lock_x_unlock(latch);

		ut_free(folds);
		goto retry;
	}

	for (i = 0; i < n_cached; i++) {

		ha_remove_all_nodes_to_page(
			btr_search_sys->hash_tables[ahi_slot],
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

	block->index = NULL;

	MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_REMOVED);
	MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_REMOVED, n_cached);

cleanup:
	assert_block_ahi_valid(block);
	rw_lock_x_unlock(latch);

	ut_free(folds);
}

/** Drop possible adaptive hash index entries when a page is evicted
from the buffer pool or freed in a file, or the index is being dropped.
@param[in]	page_id		page id */
void btr_search_drop_page_hash_when_freed(const page_id_t page_id)
{
	buf_block_t*	block;
	mtr_t		mtr;
	dberr_t		err = DB_SUCCESS;

	mtr_start(&mtr);

	/* If the caller has a latch on the page, then the caller must
	have a x-latch on the page and it must have already dropped
	the hash index for the page. Because of the x-latch that we
	are possibly holding, we cannot s-latch the page, but must
	(recursively) x-latch it, even though we are only reading. */

	block = buf_page_get_gen(page_id, 0, RW_X_LATCH, NULL,
				 BUF_PEEK_IF_IN_POOL, __FILE__, __LINE__,
				 &mtr, &err);

	if (block) {

		/* If AHI is still valid, page can't be in free state.
		AHI is dropped when page is freed. */
		ut_ad(!block->page.file_page_was_freed);

		buf_block_dbg_add_level(block, SYNC_TREE_NODE_FROM_HASH);

		dict_index_t*	index = block->index;
		if (index != NULL) {
			/* In all our callers, the table handle should
			be open, or we should be in the process of
			dropping the table (preventing eviction). */
			ut_ad(index->table->get_ref_count() > 0
			      || mutex_own(&dict_sys.mutex));
			btr_search_drop_page_hash_index(block);
		}
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
	rw_lock_t*	ahi_latch,
	ulint		n_fields,
	ulint		n_bytes,
	ibool		left_side)
{
	const rec_t*	rec;
	const rec_t*	next_rec;
	ulint		fold;
	ulint		next_fold;
	ulint		n_cached;
	ulint		n_recs;
	ulint*		folds;
	const rec_t**	recs;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;

#ifdef MYSQL_INDEX_DISABLE_AHI
	if (index->disable_ahi) return;
#endif
	if (!btr_search_enabled) {
		return;
	}

	rec_offs_init(offsets_);
	ut_ad(ahi_latch == btr_get_search_latch(index));
	ut_ad(index);
	ut_ad(block->page.id.space() == index->table->space_id);
	ut_ad(!dict_index_is_ibuf(index));
	ut_ad(page_is_leaf(block->frame));

	ut_ad(rw_lock_own_flagged(&block->lock,
				  RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));
	ut_ad(block->page.id.page_no() >= 3);

	rw_lock_s_lock(ahi_latch);

	const bool enabled = btr_search_enabled;
	const bool rebuild = enabled && block->index
		&& (block->curr_n_fields != n_fields
		    || block->curr_n_bytes != n_bytes
		    || block->curr_left_side != left_side);

	rw_lock_s_unlock(ahi_latch);

	if (!enabled) {
		return;
	}

	if (rebuild) {
		btr_search_drop_page_hash_index(block);
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

	if (rec_is_metadata(rec, *index)) {
		rec = page_rec_get_next_const(rec);
		if (!--n_recs) return;
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
	      || n_fields + (n_bytes > 0) == rec_offs_n_fields(offsets));

	fold = rec_fold(rec, offsets, n_fields, n_bytes, index->id);

	if (left_side) {

		folds[n_cached] = fold;
		recs[n_cached] = rec;
		n_cached++;
	}

	for (;;) {
		next_rec = page_rec_get_next_const(rec);

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

	rw_lock_x_lock(ahi_latch);

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

	block->curr_n_fields = unsigned(n_fields);
	block->curr_n_bytes = unsigned(n_bytes);
	block->curr_left_side = unsigned(left_side);
	block->index = index;

	{
		hash_table_t*	table = btr_get_search_table(index);
		for (ulint i = 0; i < n_cached; i++) {
			ha_insert_for_fold(table, folds[i], block, recs[i]);
		}
	}

	MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_ADDED);
	MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_ADDED, n_cached);
exit_func:
	assert_block_ahi_valid(block);
	rw_lock_x_unlock(ahi_latch);

	ut_free(folds);
	ut_free(recs);
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

/** Updates the search info.
@param[in,out]	info	search info
@param[in,out]	cursor	cursor which was just positioned */
void
btr_search_info_update_slow(btr_search_t* info, btr_cur_t* cursor)
{
	rw_lock_t*	ahi_latch = btr_get_search_latch(cursor->index);

	ut_ad(!rw_lock_own_flagged(ahi_latch,
				   RW_LOCK_FLAG_X | RW_LOCK_FLAG_S));

	buf_block_t*	block = btr_cur_get_block(cursor);

	/* NOTE that the following two function calls do NOT protect
	info or block->n_fields etc. with any semaphore, to save CPU time!
	We cannot assume the fields are consistent when we return from
	those functions! */

	btr_search_info_update_hash(info, cursor);

	bool build_index = btr_search_update_block_hash_info(info, block);

	if (build_index || (cursor->flag == BTR_CUR_HASH_FAIL)) {

		btr_search_check_free_space_in_heap(cursor->index);
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
		btr_search_build_page_hash_index(cursor->index, block,
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
	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_X));
	ut_ad(rw_lock_own(&(new_block->lock), RW_LOCK_X));

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

	rw_lock_t* ahi_latch = index ? btr_get_search_latch(index) : NULL;

	if (new_block->index) {
drop_exit:
		btr_search_drop_page_hash_index(block);
		return;
	}

	if (!index) {
		return;
	}

	rw_lock_s_lock(ahi_latch);

	if (block->index) {

		if (block->index != index) {
			rw_lock_s_unlock(ahi_latch);
			goto drop_exit;
		}

		ulint	n_fields = block->curr_n_fields;
		ulint	n_bytes = block->curr_n_bytes;
		ibool	left_side = block->curr_left_side;

		new_block->n_fields = block->curr_n_fields;
		new_block->n_bytes = block->curr_n_bytes;
		new_block->left_side = left_side;

		rw_lock_s_unlock(ahi_latch);

		ut_a(n_fields > 0 || n_bytes > 0);

		btr_search_build_page_hash_index(
			index, new_block, ahi_latch,
			n_fields, n_bytes, left_side);
		ut_ad(n_fields == block->curr_n_fields);
		ut_ad(n_bytes == block->curr_n_bytes);
		ut_ad(left_side == block->curr_left_side);
		return;
	}
	rw_lock_s_unlock(ahi_latch);
}

/** Updates the page hash index when a single record is deleted from a page.
@param[in]	cursor	cursor which was positioned on the record to delete
			using btr_cur_search_, the record is not yet deleted.*/
void btr_search_update_hash_on_delete(btr_cur_t* cursor)
{
	buf_block_t*	block;
	const rec_t*	rec;
	ulint		fold;
	dict_index_t*	index;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	mem_heap_t*	heap		= NULL;
	rec_offs_init(offsets_);

	ut_ad(page_is_leaf(btr_cur_get_page(cursor)));
#ifdef MYSQL_INDEX_DISABLE_AHI
	if (cursor->index->disable_ahi) return;
#endif

	if (!btr_search_enabled) {
		return;
	}

	block = btr_cur_get_block(cursor);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_X));

	assert_block_ahi_valid(block);
	index = block->index;

	if (!index) {

		return;
	}

	if (index != cursor->index) {
		ut_ad(index->id == cursor->index->id);
		btr_search_drop_page_hash_index(block);
		return;
	}

	ut_ad(block->page.id.space() == index->table->space_id);
	ut_a(index == cursor->index);
	ut_a(block->curr_n_fields > 0 || block->curr_n_bytes > 0);
	ut_ad(!dict_index_is_ibuf(index));

	rec = btr_cur_get_rec(cursor);

	fold = rec_fold(rec, rec_get_offsets(rec, index, offsets_,
					     index->n_core_fields,
					     ULINT_UNDEFINED, &heap),
			block->curr_n_fields, block->curr_n_bytes, index->id);
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	rw_lock_t* ahi_latch = btr_get_search_latch(index);

	rw_lock_x_lock(ahi_latch);
	assert_block_ahi_valid(block);

	if (btr_search_enabled) {
		hash_table_t* table = btr_get_search_table(index);
		if (block->index) {
			ut_a(block->index == index);

			if (ha_search_and_delete_if_found(table, fold, rec)) {
				MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVED);
			} else {
				MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVE_NOT_FOUND);
			}

			assert_block_ahi_valid(block);
		}
	}

	rw_lock_x_unlock(ahi_latch);
}

/** Updates the page hash index when a single record is inserted on a page.
@param[in]	cursor	cursor which was positioned to the place to insert
			using btr_cur_search_, and the new record has been
			inserted next to the cursor.
@param[in]	ahi_latch	the adaptive hash index latch */
void
btr_search_update_hash_node_on_insert(btr_cur_t* cursor, rw_lock_t* ahi_latch)
{
	hash_table_t*	table;
	buf_block_t*	block;
	dict_index_t*	index;
	rec_t*		rec;

	ut_ad(ahi_latch == btr_get_search_latch(cursor->index));
	ut_ad(!btr_search_own_any(RW_LOCK_S));
	ut_ad(!btr_search_own_any(RW_LOCK_X));
#ifdef MYSQL_INDEX_DISABLE_AHI
	if (cursor->index->disable_ahi) return;
#endif
	if (!btr_search_enabled) {
		return;
	}

	rec = btr_cur_get_rec(cursor);

	block = btr_cur_get_block(cursor);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_X));

	index = block->index;

	if (!index) {

		return;
	}

	if (cursor->index != index) {
		ut_ad(cursor->index->id == index->id);
		btr_search_drop_page_hash_index(block);
		return;
	}

	ut_a(cursor->index == index);
	ut_ad(!dict_index_is_ibuf(index));
	rw_lock_x_lock(ahi_latch);

	if (!block->index || !btr_search_enabled) {

		goto func_exit;
	}

	ut_a(block->index == index);

	if ((cursor->flag == BTR_CUR_HASH)
	    && (cursor->n_fields == block->curr_n_fields)
	    && (cursor->n_bytes == block->curr_n_bytes)
	    && !block->curr_left_side) {

		table = btr_get_search_table(index);

		if (ha_search_and_update_if_found(
			table, cursor->fold, rec, block,
			page_rec_get_next(rec))) {
			MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_UPDATED);
		}

func_exit:
		assert_block_ahi_valid(block);
		rw_lock_x_unlock(ahi_latch);
	} else {
		rw_lock_x_unlock(ahi_latch);

		btr_search_update_hash_on_insert(cursor, ahi_latch);
	}
}

/** Updates the page hash index when a single record is inserted on a page.
@param[in,out]	cursor		cursor which was positioned to the
				place to insert using btr_cur_search_...,
				and the new record has been inserted next
				to the cursor
@param[in]	ahi_latch	the adaptive hash index latch */
void
btr_search_update_hash_on_insert(btr_cur_t* cursor, rw_lock_t* ahi_latch)
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

	ut_ad(ahi_latch == btr_get_search_latch(cursor->index));
	ut_ad(page_is_leaf(btr_cur_get_page(cursor)));
	ut_ad(!btr_search_own_any(RW_LOCK_S));
	ut_ad(!btr_search_own_any(RW_LOCK_X));
#ifdef MYSQL_INDEX_DISABLE_AHI
	if (cursor->index->disable_ahi) return;
#endif
	if (!btr_search_enabled) {
		return;
	}

	block = btr_cur_get_block(cursor);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_X));
	assert_block_ahi_valid(block);

	index = block->index;

	if (!index) {

		return;
	}

	ut_ad(block->page.id.space() == index->table->space_id);
	btr_search_check_free_space_in_heap(index);

	rec = btr_cur_get_rec(cursor);

#ifdef MYSQL_INDEX_DISABLE_AHI
	ut_a(!index->disable_ahi);
#endif
	if (index != cursor->index) {
		ut_ad(index->id == cursor->index->id);
		btr_search_drop_page_hash_index(block);
		return;
	}

	ut_a(index == cursor->index);
	ut_ad(!dict_index_is_ibuf(index));

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;
	const bool left_side = block->curr_left_side;

	ins_rec = page_rec_get_next_const(rec);
	next_rec = page_rec_get_next_const(ins_rec);

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

	/* We must not look up "table" before acquiring ahi_latch. */
	hash_table_t* table = NULL;
	bool locked = false;

	if (!page_rec_is_infimum(rec) && !rec_is_metadata(rec, *index)) {
		offsets = rec_get_offsets(
			rec, index, offsets, index->n_core_fields,
			btr_search_get_n_fields(n_fields, n_bytes), &heap);
		fold = rec_fold(rec, offsets, n_fields, n_bytes, index->id);
	} else {
		if (left_side) {
			locked = true;
			rw_lock_x_lock(ahi_latch);

			if (!btr_search_enabled || !block->index) {
				goto function_exit;
			}

			table = btr_get_search_table(index);
			ha_insert_for_fold(table, ins_fold, block, ins_rec);
		}

		goto check_next_rec;
	}

	if (fold != ins_fold) {

		if (!locked) {
			locked = true;
			rw_lock_x_lock(ahi_latch);

			if (!btr_search_enabled || !block->index) {
				goto function_exit;
			}

			table = btr_get_search_table(index);
		}

		if (!left_side) {
			ha_insert_for_fold(table, fold, block, rec);
		} else {
			ha_insert_for_fold(table, ins_fold, block, ins_rec);
		}
	}

check_next_rec:
	if (page_rec_is_supremum(next_rec)) {

		if (!left_side) {
			if (!locked) {
				locked = true;
				rw_lock_x_lock(ahi_latch);

				if (!btr_search_enabled || !block->index) {
					goto function_exit;
				}

				table = btr_get_search_table(index);
			}

			ha_insert_for_fold(table, ins_fold, block, ins_rec);
		}

		goto function_exit;
	}

	if (ins_fold != next_fold) {
		if (!locked) {
			locked = true;
			rw_lock_x_lock(ahi_latch);

			if (!btr_search_enabled || !block->index) {
				goto function_exit;
			}

			table = btr_get_search_table(index);
		}

		if (!left_side) {
			ha_insert_for_fold(table, ins_fold, block, ins_rec);
		} else {
			ha_insert_for_fold(table, next_fold, block, next_rec);
		}
	}

function_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	if (locked) {
		rw_lock_x_unlock(ahi_latch);
	}
	ut_ad(!rw_lock_own(ahi_latch, RW_LOCK_X));
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG

/** Validates the search system for given hash table.
@param[in]	hash_table_id	hash table to validate
@return TRUE if ok */
static
ibool
btr_search_hash_table_validate(ulint hash_table_id)
{
	ha_node_t*	node;
	ibool		ok		= TRUE;
	ulint		i;
	ulint		cell_count;
	mem_heap_t*	heap		= NULL;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;

	btr_search_x_lock_all();
	if (!btr_search_enabled) {
		btr_search_x_unlock_all();
		return(TRUE);
	}

	/* How many cells to check before temporarily releasing
	search latches. */
	ulint		chunk_size = 10000;

	rec_offs_init(offsets_);

	buf_pool_mutex_enter_all();

	cell_count = hash_get_n_cells(
			btr_search_sys->hash_tables[hash_table_id]);

	for (i = 0; i < cell_count; i++) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if ((i != 0) && ((i % chunk_size) == 0)) {

			buf_pool_mutex_exit_all();
			btr_search_x_unlock_all();

			os_thread_yield();

			btr_search_x_lock_all();

			if (!btr_search_enabled) {
				ok = true;
				goto func_exit;
			}

			buf_pool_mutex_enter_all();

			ulint	curr_cell_count = hash_get_n_cells(
				btr_search_sys->hash_tables[hash_table_id]);

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		node = (ha_node_t*) hash_get_nth_cell(
			btr_search_sys->hash_tables[hash_table_id], i)->node;

		for (; node != NULL; node = node->next) {
			const buf_block_t*	block
				= buf_block_from_ahi((byte*) node->data);
			const buf_block_t*	hash_block;
			buf_pool_t*		buf_pool;
			index_id_t		page_index_id;

			buf_pool = buf_pool_from_bpage((buf_page_t*) block);

			if (UNIV_LIKELY(buf_block_get_state(block)
					== BUF_BLOCK_FILE_PAGE)) {

				/* The space and offset are only valid
				for file blocks.  It is possible that
				the block is being freed
				(BUF_BLOCK_REMOVE_HASH, see the
				assertion and the comment below) */
				hash_block = buf_block_hash_get(
					buf_pool,
					block->page.id);
			} else {
				hash_block = NULL;
			}

			if (hash_block) {
				ut_a(hash_block == block);
			} else {
				/* When a block is being freed,
				buf_LRU_search_and_free_block() first
				removes the block from
				buf_pool->page_hash by calling
				buf_LRU_block_remove_hashed_page().
				After that, it invokes
				btr_search_drop_page_hash_index() to
				remove the block from
				btr_search_sys->hash_tables[i]. */

				ut_a(buf_block_get_state(block)
				     == BUF_BLOCK_REMOVE_HASH);
			}

			ut_ad(!dict_index_is_ibuf(block->index));
			ut_ad(block->page.id.space()
			      == block->index->table->space_id);

			page_index_id = btr_page_get_index_id(block->frame);

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
				const page_t*	page = block->frame;

				ok = FALSE;

				ib::error() << "Error in an adaptive hash"
					<< " index pointer to page "
					<< page_id_t(page_get_space_id(page),
						     page_get_page_no(page))
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

			buf_pool_mutex_exit_all();
			btr_search_x_unlock_all();

			os_thread_yield();

			btr_search_x_lock_all();

			if (!btr_search_enabled) {
				ok = true;
				goto func_exit;
			}

			buf_pool_mutex_enter_all();

			ulint	curr_cell_count = hash_get_n_cells(
				btr_search_sys->hash_tables[hash_table_id]);

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		ulint end_index = ut_min(i + chunk_size - 1, cell_count - 1);

		if (!ha_validate(btr_search_sys->hash_tables[hash_table_id],
				 i, end_index)) {
			ok = FALSE;
		}
	}

	buf_pool_mutex_exit_all();
func_exit:
	btr_search_x_unlock_all();

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return(ok);
}

/** Validate the search system.
@return true if ok. */
bool
btr_search_validate()
{
	for (ulint i = 0; i < btr_ahi_parts; ++i) {
		if (!btr_search_hash_table_validate(i)) {
			return(false);
		}
	}

	return(true);
}

#endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
