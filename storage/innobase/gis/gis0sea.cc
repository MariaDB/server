/*****************************************************************************

Copyright (c) 2016, 2018, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************//**
@file gis/gis0sea.cc
InnoDB R-tree search interfaces

Created 2014/01/16 Jimmy Yang
***********************************************************************/

#include "fsp0fsp.h"
#include "page0page.h"
#include "page0cur.h"
#include "page0zip.h"
#include "gis0rtree.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "ibuf0ibuf.h"
#include "trx0trx.h"
#include "srv0mon.h"
#include "que0que.h"
#include "gis0geo.h"

/** Restore the stored position of a persistent cursor bufferfixing the page */
static
bool
rtr_cur_restore_position(
	btr_cur_t*	cursor,		/*!< in: detached persistent cursor */
	ulint		level,		/*!< in: index level */
	mtr_t*		mtr);		/*!< in: mtr */

/*************************************************************//**
Pop out used parent path entry, until we find the parent with matching
page number */
static
void
rtr_adjust_parent_path(
/*===================*/
	rtr_info_t*	rtr_info,	/* R-Tree info struct */
	ulint		page_no)	/* page number to look for */
{
	while (!rtr_info->parent_path->empty()) {
		if (rtr_info->parent_path->back().child_no == page_no) {
			break;
		} else {
			if (rtr_info->parent_path->back().cursor) {
				btr_pcur_close(
					rtr_info->parent_path->back().cursor);
				ut_free(rtr_info->parent_path->back().cursor);
			}

			rtr_info->parent_path->pop_back();
		}
	}
}

/** Latches the leaf page or pages requested.
@param[in]	block_savepoint	leaf page where the search converged
@param[in]	latch_mode	BTR_SEARCH_LEAF, ...
@param[in]	cursor		cursor
@param[in]	mtr		mini-transaction */
static void
rtr_latch_leaves(
	ulint			block_savepoint,
	btr_latch_mode		latch_mode,
	btr_cur_t*		cursor,
	mtr_t*			mtr)
{
	compile_time_assert(int(MTR_MEMO_PAGE_S_FIX) == int(RW_S_LATCH));
	compile_time_assert(int(MTR_MEMO_PAGE_X_FIX) == int(RW_X_LATCH));
	compile_time_assert(int(MTR_MEMO_PAGE_SX_FIX) == int(RW_SX_LATCH));

	buf_block_t* block = mtr->at_savepoint(block_savepoint);

	ut_ad(block->page.id().space() == cursor->index()->table->space->id);
	ut_ad(block->page.in_file());
	ut_ad(mtr->memo_contains_flagged(&cursor->index()->lock,
					 MTR_MEMO_S_LOCK
					 | MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));

	switch (latch_mode) {
		uint32_t	left_page_no;
		uint32_t	right_page_no;
	default:
		ut_ad(latch_mode == BTR_CONT_MODIFY_TREE);
		break;
	case BTR_MODIFY_TREE:
		/* It is exclusive for other operations which calls
		btr_page_set_prev() */
		ut_ad(mtr->memo_contains_flagged(&cursor->index()->lock,
						 MTR_MEMO_X_LOCK
						 | MTR_MEMO_SX_LOCK));
		/* x-latch also siblings from left to right */
		left_page_no = btr_page_get_prev(block->page.frame);

		if (left_page_no != FIL_NULL) {
			btr_block_get(*cursor->index(), left_page_no, RW_X_LATCH,
				      true, mtr);
		}

		mtr->upgrade_buffer_fix(block_savepoint, RW_X_LATCH);

		right_page_no = btr_page_get_next(block->page.frame);

		if (right_page_no != FIL_NULL) {
			btr_block_get(*cursor->index(), right_page_no,
				      RW_X_LATCH, true, mtr);
		}
		break;
	case BTR_SEARCH_LEAF:
	case BTR_MODIFY_LEAF:
		rw_lock_type_t mode =
			rw_lock_type_t(latch_mode & (RW_X_LATCH | RW_S_LATCH));
		static_assert(int{RW_S_LATCH} == int{BTR_SEARCH_LEAF}, "");
		static_assert(int{RW_X_LATCH} == int{BTR_MODIFY_LEAF}, "");
		mtr->upgrade_buffer_fix(block_savepoint, mode);
	}
}

/*************************************************************//**
Find the next matching record. This function is used by search
or record locating during index delete/update.
@return true if there is suitable record found, otherwise false */
TRANSACTIONAL_TARGET
static
bool
rtr_pcur_getnext_from_path(
/*=======================*/
	const dtuple_t* tuple,	/*!< in: data tuple */
	page_cur_mode_t	mode,	/*!< in: cursor search mode */
	btr_cur_t*	btr_cur,/*!< in: persistent cursor; NOTE that the
				function may release the page latch */
	ulint		target_level,
				/*!< in: target level */
	ulint		latch_mode,
				/*!< in: latch_mode */
	bool		index_locked,
				/*!< in: index tree locked */
	mtr_t*		mtr)	/*!< in: mtr */
{
	dict_index_t*	index = btr_cur->index();
	bool		found = false;
	page_cur_t*	page_cursor;
	ulint		level = 0;
	node_visit_t	next_rec;
	rtr_info_t*	rtr_info = btr_cur->rtr_info;
	node_seq_t	page_ssn;
	ulint		skip_parent = false;
	bool		new_split = false;
	bool		for_delete = false;
	bool		for_undo_ins = false;

	/* exhausted all the pages to be searched */
	if (rtr_info->path->empty()) {
		return(false);
	}

	ut_ad(dtuple_get_n_fields_cmp(tuple));

	const auto my_latch_mode = BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);

	for_delete = latch_mode & BTR_RTREE_DELETE_MARK;
	for_undo_ins = latch_mode & BTR_RTREE_UNDO_INS;

	/* There should be no insert coming to this function. Only
	mode with BTR_MODIFY_* should be delete */
	ut_ad(mode != PAGE_CUR_RTREE_INSERT);
	ut_ad(my_latch_mode == BTR_SEARCH_LEAF
	      || my_latch_mode == BTR_MODIFY_LEAF
	      || my_latch_mode == BTR_MODIFY_TREE
	      || my_latch_mode == BTR_CONT_MODIFY_TREE);

	/* Whether need to track parent information. Only need so
	when we do tree altering operations (such as index page merge) */
	static_assert(BTR_CONT_MODIFY_TREE == (4 | BTR_MODIFY_TREE), "");

	const bool need_parent = mode == PAGE_CUR_RTREE_LOCATE
		&& (my_latch_mode | 4) == BTR_CONT_MODIFY_TREE;

	if (!index_locked) {
		ut_ad(mtr->is_empty());
		mtr_s_lock_index(index, mtr);
	} else {
		ut_ad(mtr->memo_contains_flagged(&index->lock,
						 MTR_MEMO_SX_LOCK
						 | MTR_MEMO_S_LOCK
						 | MTR_MEMO_X_LOCK));
	}

	const ulint zip_size = index->table->space->zip_size();

	/* Pop each node/page to be searched from "path" structure
	and do a search on it. Please note, any pages that are in
	the "path" structure are protected by "page" lock, so tey
	cannot be shrunk away */
	do {
		buf_block_t*	block;
		node_seq_t	path_ssn;
		const page_t*	page;
		rw_lock_type_t	rw_latch;

		mysql_mutex_lock(&rtr_info->rtr_path_mutex);
		next_rec = rtr_info->path->back();
		rtr_info->path->pop_back();
		level = next_rec.level;
		path_ssn = next_rec.seq_no;

		/* Maintain the parent path info as well, if needed */
		if (need_parent && !skip_parent && !new_split) {
			ulint		old_level;
			ulint		new_level;

			ut_ad(!rtr_info->parent_path->empty());

			/* Cleanup unused parent info */
			if (rtr_info->parent_path->back().cursor) {
				btr_pcur_close(
					rtr_info->parent_path->back().cursor);
				ut_free(rtr_info->parent_path->back().cursor);
			}

			old_level = rtr_info->parent_path->back().level;

			rtr_info->parent_path->pop_back();

			ut_ad(!rtr_info->parent_path->empty());

			/* check whether there is a level change. If so,
			the current parent path needs to pop enough
			nodes to adjust to the new search page */
			new_level = rtr_info->parent_path->back().level;

			if (old_level < new_level) {
				rtr_adjust_parent_path(
					rtr_info, next_rec.page_no);
			}

			ut_ad(!rtr_info->parent_path->empty());

			ut_ad(next_rec.page_no
			      == rtr_info->parent_path->back().child_no);
		}

		mysql_mutex_unlock(&rtr_info->rtr_path_mutex);

		skip_parent = false;
		new_split = false;

		/* Once we have pages in "path", these pages are
		predicate page locked, so they can't be shrunk away.
		They also have SSN (split sequence number) to detect
		splits, so we can directly latch single page while
		getting them. They can be unlatched if not qualified.
		One reason for pre-latch is that we might need to position
		some parent position (requires latch) during search */
		if (level == 0) {
			static_assert(ulint{BTR_SEARCH_LEAF} ==
				      ulint{RW_S_LATCH}, "");
			static_assert(ulint{BTR_MODIFY_LEAF} ==
				      ulint{RW_X_LATCH}, "");
			rw_latch = (my_latch_mode | 4) == BTR_CONT_MODIFY_TREE
				? RW_NO_LATCH
				: rw_lock_type_t(my_latch_mode);
		} else {
			rw_latch = RW_X_LATCH;
		}

		if (my_latch_mode == BTR_MODIFY_LEAF) {
			mtr->rollback_to_savepoint(1);
		}

		const auto block_savepoint = mtr->get_savepoint();
		block = buf_page_get_gen(
			page_id_t(index->table->space_id,
				  next_rec.page_no), zip_size,
			rw_latch, NULL, BUF_GET, mtr);

		if (!block) {
			found = false;
			break;
		}

		buf_page_make_young_if_needed(&block->page);

		page = buf_block_get_frame(block);
		page_ssn = page_get_ssn_id(page);

		/* If there are splits, push the splitted page.
		Note that we have SX lock on index->lock, there
		should not be any split/shrink happening here */
		if (page_ssn > path_ssn) {
			uint32_t next_page_no = btr_page_get_next(page);
			rtr_non_leaf_stack_push(
				rtr_info->path, next_page_no, path_ssn,
				level, 0, NULL, 0);

			if (!srv_read_only_mode
			    && mode != PAGE_CUR_RTREE_INSERT
			    && mode != PAGE_CUR_RTREE_LOCATE) {
				ut_ad(rtr_info->thr);
				lock_place_prdt_page_lock(
					page_id_t(block->page.id().space(),
						  next_page_no),
					index,
					rtr_info->thr);
			}
			new_split = true;
#if defined(UNIV_GIS_DEBUG)
			fprintf(stderr,
				"GIS_DIAG: Splitted page found: %d, %ld\n",
				static_cast<int>(need_parent), next_page_no);
#endif
		}

		page_cursor = btr_cur_get_page_cur(btr_cur);
		page_cursor->rec = NULL;
		page_cursor->block = block;

		if (mode == PAGE_CUR_RTREE_LOCATE) {
			if (target_level == 0 && level == 0) {
				ulint	low_match = 0, up_match = 0;

				found = false;

				if (!page_cur_search_with_match(
					tuple, PAGE_CUR_LE,
					&up_match, &low_match,
					btr_cur_get_page_cur(btr_cur), nullptr)
				    && low_match
				    == dtuple_get_n_fields_cmp(tuple)) {
					rec_t*	rec = btr_cur_get_rec(btr_cur);

					if (!rec_get_deleted_flag(rec,
					    dict_table_is_comp(index->table))
					    || (!for_delete && !for_undo_ins)) {
						found = true;
						btr_cur->low_match = low_match;
					} else {
						/* mark we found deleted row */
						btr_cur->rtr_info->fd_del
							= true;
					}
				}
			} else {
				page_cur_mode_t	page_mode = mode;

				if (level == target_level
				    && target_level != 0) {
					page_mode = PAGE_CUR_RTREE_GET_FATHER;
				}
				found = rtr_cur_search_with_match(
					block, index, tuple, page_mode,
					page_cursor, btr_cur->rtr_info);

				/* Save the position of parent if needed */
				if (found && need_parent) {
					btr_pcur_t*     r_cursor =
						rtr_get_parent_cursor(
							btr_cur, level, false);

					rec_t*	rec = page_cur_get_rec(
						page_cursor);
					page_cur_position(
						rec, block,
						btr_pcur_get_page_cur(r_cursor));
					r_cursor->pos_state =
						 BTR_PCUR_IS_POSITIONED;
					r_cursor->latch_mode = my_latch_mode;
					btr_pcur_store_position(r_cursor, mtr);
					ut_d(ulint num_stored =)
					rtr_store_parent_path(
						block, btr_cur,
						btr_latch_mode(rw_latch),
						level, mtr);
					ut_ad(num_stored > 0);
				}
			}
		} else {
			found = rtr_cur_search_with_match(
				block, index, tuple, mode, page_cursor,
				btr_cur->rtr_info);
		}

		/* Attach predicate lock if needed, no matter whether
		there are matched records */
		if (mode != PAGE_CUR_RTREE_INSERT
		    && mode != PAGE_CUR_RTREE_LOCATE
		    && mode >= PAGE_CUR_CONTAIN
		    && btr_cur->rtr_info->need_prdt_lock) {
			lock_prdt_t	prdt;

			trx_t*		trx = thr_get_trx(
						btr_cur->rtr_info->thr);
			{
				TMLockTrxGuard g{TMLockTrxArgs(*trx)};
				lock_init_prdt_from_mbr(
					&prdt, &btr_cur->rtr_info->mbr,
					mode, trx->lock.lock_heap);
			}

			if (rw_latch == RW_NO_LATCH) {
				block->page.lock.s_lock();
			}

			lock_prdt_lock(block, &prdt, index, LOCK_S,
				       LOCK_PREDICATE, btr_cur->rtr_info->thr);

			if (rw_latch == RW_NO_LATCH) {
				block->page.lock.s_unlock();
			}
		}

		if (found) {
			if (level == target_level) {
				ut_ad(block
				      == mtr->at_savepoint(block_savepoint));

				if (my_latch_mode == BTR_MODIFY_TREE
				    && level == 0) {
					ut_ad(rw_latch == RW_NO_LATCH);

					rtr_latch_leaves(
						block_savepoint,
						BTR_MODIFY_TREE,
						btr_cur, mtr);
				}

				page_cur_position(
					page_cur_get_rec(page_cursor),
					page_cur_get_block(page_cursor),
					btr_cur_get_page_cur(btr_cur));

				btr_cur->low_match = level != 0 ?
					DICT_INDEX_SPATIAL_NODEPTR_SIZE + 1
					: btr_cur->low_match;
				break;
			}

			/* Keep the parent path node, which points to
			last node just located */
			skip_parent = true;
		} else {
			mtr->release_last_page();
		}

	} while (!rtr_info->path->empty());

	const rec_t* rec = btr_cur_get_rec(btr_cur);

	if (!page_rec_is_user_rec(rec)) {
		mtr->commit();
		mtr->start();
	} else if (!index_locked) {
		mtr->release(index->lock);
	}

	return(found);
}

/*************************************************************//**
Find the next matching record. This function will first exhaust
the copied record listed in the rtr_info->matches vector before
moving to the next page
@return true if there is suitable record found, otherwise false */
bool
rtr_pcur_move_to_next(
/*==================*/
	const dtuple_t*	tuple,	/*!< in: data tuple; NOTE: n_fields_cmp in
				tuple must be set so that it cannot get
				compared to the node ptr page number field! */
	page_cur_mode_t	mode,	/*!< in: cursor search mode */
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; NOTE that the
				function may release the page latch */
	ulint		level,	/*!< in: target level */
	mtr_t*		mtr)	/*!< in: mtr */
{
	rtr_info_t*	rtr_info = cursor->btr_cur.rtr_info;

	ut_a(cursor->pos_state == BTR_PCUR_IS_POSITIONED);

	mysql_mutex_lock(&rtr_info->matches->rtr_match_mutex);
	/* First retrieve the next record on the current page */
	if (!rtr_info->matches->matched_recs->empty()) {
		rtr_rec_t	rec;
		rec = rtr_info->matches->matched_recs->back();
		rtr_info->matches->matched_recs->pop_back();
		cursor->btr_cur.page_cur.block = rtr_info->matches->block;
		mysql_mutex_unlock(&rtr_info->matches->rtr_match_mutex);

		cursor->btr_cur.page_cur.rec = rec.r_rec;

		DEBUG_SYNC_C("rtr_pcur_move_to_next_return");
		return(true);
	}

	mysql_mutex_unlock(&rtr_info->matches->rtr_match_mutex);

	/* Fetch the next page */
	return(rtr_pcur_getnext_from_path(tuple, mode, &cursor->btr_cur,
					 level, cursor->latch_mode,
					 false, mtr));
}

#ifdef UNIV_DEBUG
/*************************************************************//**
Check if the cursor holds record pointing to the specified child page
@return	true if it is (pointing to the child page) false otherwise */
static void rtr_compare_cursor_rec(const rec_t *rec, dict_index_t *index,
                                   ulint page_no)
{
  if (!rec)
    return;
  mem_heap_t *heap= nullptr;
  rec_offs *offsets= rec_get_offsets(rec, index, nullptr, 0,
                                     ULINT_UNDEFINED, &heap);
  ut_ad(btr_node_ptr_get_child_page_no(rec, offsets) == page_no);
  mem_heap_free(heap);
}
#endif

TRANSACTIONAL_TARGET
dberr_t rtr_search_to_nth_level(ulint level, const dtuple_t *tuple,
                                page_cur_mode_t mode,
                                btr_latch_mode latch_mode,
                                btr_cur_t *cur, mtr_t *mtr)
{
  page_cur_mode_t page_mode;
  page_cur_mode_t search_mode= PAGE_CUR_UNSUPP;

  bool mbr_adj= false;
  bool found= false;
  dict_index_t *const index= cur->index();

  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs *offsets= offsets_;
  rec_offs_init(offsets_);
  ut_ad(level == 0 || mode == PAGE_CUR_LE || RTREE_SEARCH_MODE(mode));
  ut_ad(dict_index_check_search_tuple(index, tuple));
  ut_ad(dtuple_check_typed(tuple));
  ut_ad(index->is_spatial());
  ut_ad(index->page != FIL_NULL);

  MEM_UNDEFINED(&cur->up_match, sizeof cur->up_match);
  MEM_UNDEFINED(&cur->up_bytes, sizeof cur->up_bytes);
  MEM_UNDEFINED(&cur->low_match, sizeof cur->low_match);
  MEM_UNDEFINED(&cur->low_bytes, sizeof cur->low_bytes);
  ut_d(cur->up_match= ULINT_UNDEFINED);
  ut_d(cur->low_match= ULINT_UNDEFINED);

  const bool latch_by_caller= latch_mode & BTR_ALREADY_S_LATCHED;

  ut_ad(!latch_by_caller
        || mtr->memo_contains_flagged(&index->lock, MTR_MEMO_S_LOCK
                                      | MTR_MEMO_SX_LOCK));
  latch_mode= BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);

  ut_ad(!latch_by_caller || latch_mode == BTR_SEARCH_LEAF ||
        latch_mode == BTR_MODIFY_LEAF);

  cur->flag= BTR_CUR_BINARY;

#ifndef BTR_CUR_ADAPT
  buf_block_t *guess= nullptr;
#else
  btr_search_t *const info= btr_search_get_info(index);
  buf_block_t *guess= info->root_guess;
#endif

  /* Store the position of the tree latch we push to mtr so that we
     know how to release it when we have latched leaf node(s) */

  const ulint savepoint= mtr->get_savepoint();

  rw_lock_type_t upper_rw_latch, root_leaf_rw_latch= RW_NO_LATCH;

  switch (latch_mode) {
  case BTR_MODIFY_TREE:
    mtr_x_lock_index(index, mtr);
    upper_rw_latch= root_leaf_rw_latch= RW_X_LATCH;
    break;
  case BTR_CONT_MODIFY_TREE:
    ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK |
                                     MTR_MEMO_SX_LOCK));
    upper_rw_latch= RW_X_LATCH;
    break;
  default:
    ut_ad(latch_mode != BTR_MODIFY_PREV);
    ut_ad(latch_mode != BTR_SEARCH_PREV);
    if (!latch_by_caller)
      mtr_s_lock_index(index, mtr);
    upper_rw_latch= root_leaf_rw_latch= RW_S_LATCH;
    if (latch_mode == BTR_MODIFY_LEAF)
      root_leaf_rw_latch= RW_X_LATCH;
  }

  auto root_savepoint= mtr->get_savepoint();
  const ulint zip_size= index->table->space->zip_size();

  /* Start with the root page. */
  page_id_t page_id(index->table->space_id, index->page);

  ulint up_match= 0, up_bytes= 0, low_match= 0, low_bytes= 0;
  ulint height= ULINT_UNDEFINED;

  /* We use these modified search modes on non-leaf levels of the
     B-tree. These let us end up in the right B-tree leaf. In that leaf
     we use the original search mode. */

  switch (mode) {
  case PAGE_CUR_GE:
    page_mode= PAGE_CUR_L;
    break;
  case PAGE_CUR_G:
    page_mode= PAGE_CUR_LE;
    break;
  default:
#ifdef PAGE_CUR_LE_OR_EXTENDS
    ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE
          || RTREE_SEARCH_MODE(mode)
          || mode == PAGE_CUR_LE_OR_EXTENDS);
#else /* PAGE_CUR_LE_OR_EXTENDS */
    ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE
          || RTREE_SEARCH_MODE(mode));
#endif /* PAGE_CUR_LE_OR_EXTENDS */
    page_mode= mode;
    break;
  }

 search_loop:
  auto buf_mode= BUF_GET;
  rw_lock_type_t rw_latch= RW_NO_LATCH;

  if (height)
  {
    /* We are about to fetch the root or a non-leaf page. */
    if (latch_mode != BTR_MODIFY_TREE || height == level)
      /* If doesn't have SX or X latch of index,
         each page should be latched before reading. */
      rw_latch= upper_rw_latch;
  }
  else if (latch_mode <= BTR_MODIFY_LEAF)
    rw_latch= rw_lock_type_t(latch_mode);

  dberr_t err;
  auto block_savepoint= mtr->get_savepoint();
  buf_block_t *block= buf_page_get_gen(page_id, zip_size, rw_latch, guess,
                                       buf_mode, mtr, &err, false);
  if (!block)
  {
    if (err)
    {
    err_exit:
      btr_read_failed(err, *index);
      mtr->rollback_to_savepoint(savepoint);
    }
  func_exit:
    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);

    if (mbr_adj)
      /* remember that we will need to adjust parent MBR */
      cur->rtr_info->mbr_adj= true;

    return err;
  }

  buf_page_make_young_if_needed(&block->page);

  const page_t *page= buf_block_get_frame(block);
#ifdef UNIV_ZIP_DEBUG
  if (rw_latch != RW_NO_LATCH) {
    const page_zip_des_t *page_zip= buf_block_get_page_zip(block);
    ut_a(!page_zip || page_zip_validate(page_zip, page, index));
  }
#endif /* UNIV_ZIP_DEBUG */

  ut_ad(fil_page_index_page_check(page));
  ut_ad(index->id == btr_page_get_index_id(page));

  if (height != ULINT_UNDEFINED);
  else if (page_is_leaf(page) &&
           rw_latch != RW_NO_LATCH && rw_latch != root_leaf_rw_latch)
  {
    /* The root page is also a leaf page (root_leaf).
    We should reacquire the page, because the root page
    is latched differently from leaf pages. */
    ut_ad(root_leaf_rw_latch != RW_NO_LATCH);
    ut_ad(rw_latch == RW_S_LATCH || rw_latch == RW_SX_LATCH);

    ut_ad(block == mtr->at_savepoint(block_savepoint));
    mtr->rollback_to_savepoint(block_savepoint);

    upper_rw_latch= root_leaf_rw_latch;
    goto search_loop;
  }
  else
  {
    /* We are in the root node */

    height= btr_page_get_level(page);
    cur->tree_height= height + 1;

    ut_ad(cur->rtr_info);

    /* If SSN in memory is not initialized, fetch it from root page */
    if (!rtr_get_current_ssn_id(index))
      /* FIXME: do this in dict_load_table_one() */
      index->set_ssn(page_get_ssn_id(page) + 1);

    /* Save the MBR */
    cur->rtr_info->thr= cur->thr;
    rtr_get_mbr_from_tuple(tuple, &cur->rtr_info->mbr);

#ifdef BTR_CUR_ADAPT
    info->root_guess= block;
#endif
  }

  if (height == 0)
  {
    if (rw_latch == RW_NO_LATCH)
    {
      ut_ad(block == mtr->at_savepoint(block_savepoint));
      rtr_latch_leaves(block_savepoint, latch_mode, cur, mtr);
    }

    switch (latch_mode) {
    case BTR_MODIFY_TREE:
    case BTR_CONT_MODIFY_TREE:
      break;
    default:
      if (!latch_by_caller)
      {
        /* Release the tree s-latch */
        mtr->rollback_to_savepoint(savepoint,
                                   savepoint + 1);
        block_savepoint--;
        root_savepoint--;
      }
      /* release upper blocks */
      if (savepoint < block_savepoint)
        mtr->rollback_to_savepoint(savepoint, block_savepoint);
    }

    page_mode= mode;
  }

  /* Remember the page search mode */
  search_mode= page_mode;

  /* Some adjustment on search mode, when the page search mode is
  PAGE_CUR_RTREE_LOCATE or PAGE_CUR_RTREE_INSERT, as we are searching
  with MBRs. When it is not the target level, we should search all
  sub-trees that "CONTAIN" the search range/MBR. When it is at the
  target level, the search becomes PAGE_CUR_LE */

  if (page_mode == PAGE_CUR_RTREE_INSERT)
  {
    page_mode= (level == height)
      ? PAGE_CUR_LE
      : PAGE_CUR_RTREE_INSERT;

    ut_ad(!page_is_leaf(page) || page_mode == PAGE_CUR_LE);
  }
  else if (page_mode == PAGE_CUR_RTREE_LOCATE && level == height)
    page_mode= level == 0 ? PAGE_CUR_LE : PAGE_CUR_RTREE_GET_FATHER;

  up_match= 0;
  low_match= 0;

  if (latch_mode == BTR_MODIFY_TREE || latch_mode == BTR_CONT_MODIFY_TREE)
    /* Tree are locked, no need for Page Lock to protect the "path" */
    cur->rtr_info->need_page_lock= false;

  cur->page_cur.block= block;

  if (page_mode >= PAGE_CUR_CONTAIN)
  {
    found= rtr_cur_search_with_match(block, index, tuple, page_mode,
                                     &cur->page_cur, cur->rtr_info);

    /* Need to use BTR_MODIFY_TREE to do the MBR adjustment */
    if (search_mode == PAGE_CUR_RTREE_INSERT && cur->rtr_info->mbr_adj) {
      static_assert(BTR_MODIFY_TREE == (8 | BTR_MODIFY_LEAF), "");

      if (!(latch_mode & 8))
        /* Parent MBR needs updated, should retry with BTR_MODIFY_TREE */
        goto func_exit;

      cur->rtr_info->mbr_adj= false;
      mbr_adj= true;
    }

    if (found && page_mode == PAGE_CUR_RTREE_GET_FATHER)
      cur->low_match= DICT_INDEX_SPATIAL_NODEPTR_SIZE + 1;
  }
  else
  {
    /* Search for complete index fields. */
    up_bytes= low_bytes= 0;
    if (page_cur_search_with_match(tuple, page_mode, &up_match,
                                   &low_match, &cur->page_cur, nullptr)) {
      err= DB_CORRUPTION;
      goto err_exit;
    }
  }

  /* If this is the desired level, leave the loop */

  ut_ad(height == btr_page_get_level(btr_cur_get_page(cur)));

  /* Add Predicate lock if it is serializable isolation
     and only if it is in the search case */
  if (mode >= PAGE_CUR_CONTAIN && mode != PAGE_CUR_RTREE_INSERT &&
      mode != PAGE_CUR_RTREE_LOCATE && cur->rtr_info->need_prdt_lock)
  {
    lock_prdt_t prdt;

    {
      trx_t* trx= thr_get_trx(cur->thr);
      TMLockTrxGuard g{TMLockTrxArgs(*trx)};
      lock_init_prdt_from_mbr(&prdt, &cur->rtr_info->mbr, mode,
                              trx->lock.lock_heap);
    }

    if (rw_latch == RW_NO_LATCH && height != 0)
      block->page.lock.s_lock();

    lock_prdt_lock(block, &prdt, index, LOCK_S, LOCK_PREDICATE, cur->thr);

    if (rw_latch == RW_NO_LATCH && height != 0)
      block->page.lock.s_unlock();
  }

  if (level != height)
  {
    ut_ad(height > 0);

    height--;
    guess= nullptr;

    const rec_t *node_ptr= btr_cur_get_rec(cur);

    offsets= rec_get_offsets(node_ptr, index, offsets, 0,
                             ULINT_UNDEFINED, &heap);

    if (page_rec_is_supremum(node_ptr))
    {
      cur->low_match= 0;
      cur->up_match= 0;
      goto func_exit;
    }

    /* If we are doing insertion or record locating,
       remember the tree nodes we visited */
    if (page_mode == PAGE_CUR_RTREE_INSERT ||
        (search_mode == PAGE_CUR_RTREE_LOCATE &&
         latch_mode != BTR_MODIFY_LEAF))
    {
      const bool add_latch= latch_mode == BTR_MODIFY_TREE &&
        rw_latch == RW_NO_LATCH;

      if (add_latch)
      {
        ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK |
                                         MTR_MEMO_SX_LOCK));
        block->page.lock.s_lock();
      }

      /* Store the parent cursor location */
      ut_d(auto num_stored=)
      rtr_store_parent_path(block, cur, latch_mode, height + 1, mtr);

      if (page_mode == PAGE_CUR_RTREE_INSERT)
      {
        btr_pcur_t *r_cursor= rtr_get_parent_cursor(cur, height + 1, true);
        /* If it is insertion, there should be only one parent for
        each level traverse */
        ut_ad(num_stored == 1);
        node_ptr= btr_pcur_get_rec(r_cursor);
      }

      if (add_latch)
        block->page.lock.s_unlock();

      ut_ad(!page_rec_is_supremum(node_ptr));
    }

    ut_ad(page_mode == search_mode ||
          (page_mode == PAGE_CUR_WITHIN &&
           search_mode == PAGE_CUR_RTREE_LOCATE));
    page_mode= search_mode;

    if (height == level && latch_mode == BTR_MODIFY_TREE)
    {
      ut_ad(upper_rw_latch == RW_X_LATCH);
      for (auto i= root_savepoint, n= mtr->get_savepoint(); i < n; i++)
        mtr->upgrade_buffer_fix(i, RW_X_LATCH);
    }

    /* Go to the child node */
    page_id.set_page_no(btr_node_ptr_get_child_page_no(node_ptr, offsets));

    if (page_mode >= PAGE_CUR_CONTAIN && page_mode != PAGE_CUR_RTREE_INSERT)
    {
      rtr_node_path_t *path= cur->rtr_info->path;

      if (found && !path->empty())
      {
        ut_ad(path->back().page_no == page_id.page_no());
        path->pop_back();
#ifdef UNIV_DEBUG
        if (page_mode == PAGE_CUR_RTREE_LOCATE &&
            latch_mode != BTR_MODIFY_LEAF)
        {
          btr_pcur_t* pcur= cur->rtr_info->parent_path->back().cursor;
          rec_t *my_node_ptr= btr_pcur_get_rec(pcur);

          offsets= rec_get_offsets(my_node_ptr, index, offsets,
                                   0, ULINT_UNDEFINED, &heap);

          ut_ad(page_id.page_no() ==
                btr_node_ptr_get_child_page_no(my_node_ptr, offsets));
        }
#endif
      }
    }

    goto search_loop;
  }

  if (level)
  {
    if (upper_rw_latch == RW_NO_LATCH)
    {
      ut_ad(latch_mode == BTR_CONT_MODIFY_TREE);
      btr_block_get(*index, page_id.page_no(), RW_X_LATCH, false, mtr, &err);
    }
    else
    {
      ut_ad(mtr->memo_contains_flagged(block, upper_rw_latch));
      ut_ad(!latch_by_caller);
    }

    if (page_mode <= PAGE_CUR_LE)
    {
      cur->low_match= low_match;
      cur->up_match= up_match;
    }
  }
  else
  {
    cur->low_match= low_match;
    cur->low_bytes= low_bytes;
    cur->up_match= up_match;
    cur->up_bytes= up_bytes;

    ut_ad(up_match != ULINT_UNDEFINED || mode != PAGE_CUR_GE);
    ut_ad(up_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
    ut_ad(low_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
  }

  goto func_exit;
}

dberr_t rtr_search_leaf(btr_cur_t *cur, const dtuple_t *tuple,
                        btr_latch_mode latch_mode,
                        mtr_t *mtr, page_cur_mode_t mode)
{
  return rtr_search_to_nth_level(0, tuple, mode, latch_mode, cur, mtr);
}

/** Search for a spatial index leaf page record.
@param pcur         cursor
@param tuple       search tuple
@param mode        search mode
@param mtr         mini-transaction */
dberr_t rtr_search_leaf(btr_pcur_t *pcur, const dtuple_t *tuple,
                        page_cur_mode_t mode, mtr_t *mtr)
{
#ifdef UNIV_DEBUG
  switch (mode) {
  case PAGE_CUR_CONTAIN:
  case PAGE_CUR_INTERSECT:
  case PAGE_CUR_WITHIN:
  case PAGE_CUR_DISJOINT:
  case PAGE_CUR_MBR_EQUAL:
    break;
  default:
    ut_ad("invalid mode" == 0);
  }
#endif
  pcur->latch_mode= BTR_SEARCH_LEAF;
  pcur->search_mode= mode;
  pcur->pos_state= BTR_PCUR_IS_POSITIONED;
  pcur->trx_if_known= nullptr;
  return rtr_search_leaf(&pcur->btr_cur, tuple, BTR_SEARCH_LEAF, mtr, mode);
}

/**************************************************************//**
Initializes and opens a persistent cursor to an index tree. It should be
closed with btr_pcur_close. */
bool rtr_search(
	const dtuple_t*	tuple,	/*!< in: tuple on which search done */
	btr_latch_mode	latch_mode,/*!< in: BTR_MODIFY_LEAF, ... */
	btr_pcur_t*	cursor, /*!< in: memory buffer for persistent cursor */
	mtr_t*		mtr)	/*!< in: mtr */
{
	static_assert(BTR_MODIFY_TREE == (8 | BTR_MODIFY_LEAF), "");
	ut_ad(latch_mode & BTR_MODIFY_LEAF);
	ut_ad(!(latch_mode & BTR_ALREADY_S_LATCHED));
	ut_ad(mtr->is_empty());

	/* Initialize the cursor */

	btr_pcur_init(cursor);

	cursor->latch_mode = BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);
	cursor->search_mode = PAGE_CUR_RTREE_LOCATE;
	cursor->trx_if_known = nullptr;

	if (latch_mode & 8) {
		mtr_x_lock_index(cursor->index(), mtr);
	} else {
		latch_mode
			= btr_latch_mode(latch_mode | BTR_ALREADY_S_LATCHED);
		mtr_sx_lock_index(cursor->index(), mtr);
	}

	/* Search with the tree cursor */

	btr_cur_t* btr_cursor = btr_pcur_get_btr_cur(cursor);

	btr_cursor->rtr_info
		= rtr_create_rtr_info(false, false,
				      btr_cursor, cursor->index());

	if (btr_cursor->thr) {
		btr_cursor->rtr_info->need_page_lock = true;
		btr_cursor->rtr_info->thr = btr_cursor->thr;
	}

	if (rtr_search_leaf(btr_cursor, tuple, latch_mode, mtr)
	    != DB_SUCCESS) {
		return true;
	}

	cursor->pos_state = BTR_PCUR_IS_POSITIONED;

	const rec_t* rec = btr_pcur_get_rec(cursor);

	const bool d= rec_get_deleted_flag(
		rec, cursor->index()->table->not_redundant());

	if (page_rec_is_infimum(rec)
	    || btr_pcur_get_low_match(cursor) != dtuple_get_n_fields(tuple)
	    || (d && latch_mode
		& (BTR_RTREE_DELETE_MARK | BTR_RTREE_UNDO_INS))) {

		if (d && latch_mode & BTR_RTREE_DELETE_MARK) {
			btr_cursor->rtr_info->fd_del = true;
			btr_cursor->low_match = 0;
		}

		mtr->rollback_to_savepoint(1);

		if (!rtr_pcur_getnext_from_path(tuple, PAGE_CUR_RTREE_LOCATE,
						btr_cursor, 0, latch_mode,
						true, mtr)) {
			return true;
		}

		ut_ad(btr_pcur_get_low_match(cursor)
		      == dtuple_get_n_fields(tuple));
	}

	if (!(latch_mode & 8)) {
		mtr->rollback_to_savepoint(0, 1);
	}

	return false;
}

/* Get the rtree page father.
@param[in,out]	mtr		mtr
@param[in]	sea_cur		search cursor, contains information
				about parent nodes in search
@param[out]	cursor		cursor on node pointer record,
				its page x-latched
@return whether the cursor was successfully positioned */
bool rtr_page_get_father(mtr_t *mtr, btr_cur_t *sea_cur, btr_cur_t *cursor)
{
  mem_heap_t *heap = mem_heap_create(100);
  rec_offs *offsets= rtr_page_get_father_block(nullptr, heap,
                                               mtr, sea_cur, cursor);
  mem_heap_free(heap);
  return offsets != nullptr;
}

MY_ATTRIBUTE((warn_unused_result))
/********************************************************************//**
Returns the upper level node pointer to a R-Tree page. It is assumed
that mtr holds an x-latch on the tree. */
static const rec_t* rtr_get_father_node(
	ulint		level,	/*!< in: the tree level of search */
	const dtuple_t*	tuple,	/*!< in: data tuple; NOTE: n_fields_cmp in
				tuple must be set so that it cannot get
				compared to the node ptr page number field! */
	btr_cur_t*	sea_cur,/*!< in: search cursor */
	btr_cur_t*	btr_cur,/*!< in/out: tree cursor; the cursor page is
				s- or x-latched, but see also above! */
	ulint		page_no,/*!< Current page no */
	mtr_t*		mtr)	/*!< in: mtr */
{
	const rec_t* rec = nullptr;
	auto had_rtr = btr_cur->rtr_info;
	dict_index_t* const index = btr_cur->index();

	/* Try to optimally locate the parent node. Level should always
	less than sea_cur->tree_height unless the root is splitting */
	if (sea_cur && sea_cur->tree_height > level) {
		ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
						 | MTR_MEMO_SX_LOCK));
		if (rtr_cur_restore_position(sea_cur, level, mtr)) {
			btr_pcur_t*	r_cursor = rtr_get_parent_cursor(
				sea_cur, level, false);

			rec = btr_pcur_get_rec(r_cursor);

			ut_ad(r_cursor->rel_pos == BTR_PCUR_ON);
			page_cur_position(rec,
					  btr_pcur_get_block(r_cursor),
					  btr_cur_get_page_cur(btr_cur));
			had_rtr = btr_cur->rtr_info = sea_cur->rtr_info;
			btr_cur->tree_height = sea_cur->tree_height;
		}
		goto func_exit;
	}

	/* We arrive here in one of two scenario
	1) check table and btr_valide
	2) index root page being raised */

	if (btr_cur->rtr_info) {
		rtr_clean_rtr_info(btr_cur->rtr_info, true);
	}

	btr_cur->rtr_info = rtr_create_rtr_info(false, false, btr_cur, index);

	if (rtr_search_to_nth_level(level, tuple, PAGE_CUR_RTREE_LOCATE,
				    BTR_CONT_MODIFY_TREE, btr_cur, mtr)
	    != DB_SUCCESS) {
	} else if (sea_cur && sea_cur->tree_height == level) {
		rec = btr_cur_get_rec(btr_cur);
	} else {
		/* btr_validate */
		ut_ad(level >= 1);
		ut_ad(!sea_cur);

		rec = btr_cur_get_rec(btr_cur);
		const ulint n_fields = dtuple_get_n_fields_cmp(tuple);

		if (page_rec_is_infimum(rec)
		    || (btr_cur->low_match != n_fields)) {
			if (!rtr_pcur_getnext_from_path(
				    tuple, PAGE_CUR_RTREE_LOCATE, btr_cur,
				    level, BTR_CONT_MODIFY_TREE, true, mtr)) {
				rec = nullptr;
			} else {
				ut_ad(btr_cur->low_match == n_fields);
				rec = btr_cur_get_rec(btr_cur);
			}
		}
	}

func_exit:
	ut_d(rtr_compare_cursor_rec(rec, index, page_no));

	if (!had_rtr && btr_cur->rtr_info) {
		rtr_clean_rtr_info(btr_cur->rtr_info, true);
		btr_cur->rtr_info = NULL;
	}

	return rec;
}

/** Returns the upper level node pointer to a R-Tree page. It is assumed
that mtr holds an SX-latch or X-latch on the tree.
@return	rec_get_offsets() of the node pointer record */
static
rec_offs*
rtr_page_get_father_node_ptr(
	rec_offs*	offsets,/*!< in: work area for the return value */
	mem_heap_t*	heap,	/*!< in: memory heap to use */
	btr_cur_t*	sea_cur,/*!< in: search cursor */
	btr_cur_t*	cursor,	/*!< in: cursor pointing to user record,
				out: cursor on node pointer record,
				its page x-latched */
	mtr_t*		mtr)	/*!< in: mtr */
{
	dtuple_t*	tuple;
	ulint		level;
	ulint		page_no;
	dict_index_t*	index;
	rtr_mbr_t	mbr;

	page_no = btr_cur_get_block(cursor)->page.id().page_no();
	index = btr_cur_get_index(cursor);

	ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
					 | MTR_MEMO_SX_LOCK));

	ut_ad(dict_index_get_page(index) != page_no);

	level = btr_page_get_level(btr_cur_get_page(cursor));

	const rec_t* user_rec = btr_cur_get_rec(cursor);
	ut_a(page_rec_is_user_rec(user_rec));

	offsets = rec_get_offsets(user_rec, index, offsets,
				  level ? 0 : index->n_fields,
				  ULINT_UNDEFINED, &heap);
	rtr_get_mbr_from_rec(user_rec, offsets, &mbr);

	tuple = rtr_index_build_node_ptr(
		index, &mbr, user_rec, page_no, heap);

	if (sea_cur && !sea_cur->rtr_info) {
		sea_cur = NULL;
	}

	const rec_t* node_ptr = rtr_get_father_node(level + 1, tuple,
						    sea_cur, cursor,
						    page_no, mtr);
	if (!node_ptr) {
		return nullptr;
	}

	ut_ad(!page_rec_is_comp(node_ptr)
	      || rec_get_status(node_ptr) == REC_STATUS_NODE_PTR);
	offsets = rec_get_offsets(node_ptr, index, offsets, 0,
				  ULINT_UNDEFINED, &heap);

	if (btr_node_ptr_get_child_page_no(node_ptr, offsets) != page_no) {
		offsets = nullptr;
	}

	return(offsets);
}

/************************************************************//**
Returns the father block to a page. It is assumed that mtr holds
an X or SX latch on the tree.
@return rec_get_offsets() of the node pointer record */
rec_offs*
rtr_page_get_father_block(
/*======================*/
	rec_offs*	offsets,/*!< in: work area for the return value */
	mem_heap_t*	heap,	/*!< in: memory heap to use */
	mtr_t*		mtr,	/*!< in: mtr */
	btr_cur_t*	sea_cur,/*!< in: search cursor, contains information
				about parent nodes in search */
	btr_cur_t*	cursor)	/*!< out: cursor on node pointer record,
				its page x-latched */
{
  const page_t *const page= cursor->block()->page.frame;
  const rec_t *rec= page_is_comp(page)
    ? page_rec_next_get<true>(page, page + PAGE_NEW_INFIMUM)
    : page_rec_next_get<false>(page, page + PAGE_OLD_INFIMUM);
  if (!rec)
    return nullptr;
  cursor->page_cur.rec= const_cast<rec_t*>(rec);
  return rtr_page_get_father_node_ptr(offsets, heap, sea_cur, cursor, mtr);
}

/*******************************************************************//**
Create a RTree search info structure */
rtr_info_t*
rtr_create_rtr_info(
/******************/
	bool		need_prdt,	/*!< in: Whether predicate lock
					is needed */
	bool		init_matches,	/*!< in: Whether to initiate the
					"matches" structure for collecting
					matched leaf records */
	btr_cur_t*	cursor,		/*!< in: tree search cursor */
	dict_index_t*	index)		/*!< in: index struct */
{
	rtr_info_t*	rtr_info;

	index = index ? index : cursor->index();
	ut_ad(index);

	rtr_info = static_cast<rtr_info_t*>(ut_zalloc_nokey(sizeof(*rtr_info)));

	rtr_info->allocated = true;
	rtr_info->cursor = cursor;
	rtr_info->index = index;

	if (init_matches) {
		rtr_info->matches = static_cast<matched_rec_t*>(
			ut_zalloc_nokey(sizeof *rtr_info->matches));

		rtr_info->matches->matched_recs
			= UT_NEW_NOKEY(rtr_rec_vector());

		mysql_mutex_init(rtr_match_mutex_key,
				 &rtr_info->matches->rtr_match_mutex,
				 nullptr);
	}

	rtr_info->path = UT_NEW_NOKEY(rtr_node_path_t());
	rtr_info->parent_path = UT_NEW_NOKEY(rtr_node_path_t());
	rtr_info->need_prdt_lock = need_prdt;
	mysql_mutex_init(rtr_path_mutex_key, &rtr_info->rtr_path_mutex,
			 nullptr);

	mysql_mutex_lock(&index->rtr_track->rtr_active_mutex);
	index->rtr_track->rtr_active.push_front(rtr_info);
	mysql_mutex_unlock(&index->rtr_track->rtr_active_mutex);
	return(rtr_info);
}

/*******************************************************************//**
Update a btr_cur_t with rtr_info */
void
rtr_info_update_btr(
/******************/
	btr_cur_t*	cursor,		/*!< in/out: tree cursor */
	rtr_info_t*	rtr_info)	/*!< in: rtr_info to set to the
					cursor */
{
	ut_ad(rtr_info);

	cursor->rtr_info = rtr_info;
}

/*******************************************************************//**
Initialize a R-Tree Search structure */
void
rtr_init_rtr_info(
/****************/
	rtr_info_t*	rtr_info,	/*!< in: rtr_info to set to the
					cursor */
	bool		need_prdt,	/*!< in: Whether predicate lock is
					needed */
	btr_cur_t*	cursor,		/*!< in: tree search cursor */
	dict_index_t*	index,		/*!< in: index structure */
	bool		reinit)		/*!< in: Whether this is a reinit */
{
	ut_ad(rtr_info);

	if (!reinit) {
		/* Reset all members. */
		memset(rtr_info, 0, sizeof *rtr_info);
		static_assert(PAGE_CUR_UNSUPP == 0, "compatibility");
		mysql_mutex_init(rtr_path_mutex_key, &rtr_info->rtr_path_mutex,
				 nullptr);
	}

	ut_ad(!rtr_info->matches || rtr_info->matches->matched_recs->empty());

	rtr_info->path = UT_NEW_NOKEY(rtr_node_path_t());
	rtr_info->parent_path = UT_NEW_NOKEY(rtr_node_path_t());
	rtr_info->need_prdt_lock = need_prdt;
	rtr_info->cursor = cursor;
	rtr_info->index = index;

	mysql_mutex_lock(&index->rtr_track->rtr_active_mutex);
	index->rtr_track->rtr_active.push_front(rtr_info);
	mysql_mutex_unlock(&index->rtr_track->rtr_active_mutex);
}

/**************************************************************//**
Clean up R-Tree search structure */
void
rtr_clean_rtr_info(
/*===============*/
	rtr_info_t*	rtr_info,	/*!< in: RTree search info */
	bool		free_all)	/*!< in: need to free rtr_info itself */
{
	dict_index_t*	index;
	bool		initialized = false;

	if (!rtr_info) {
		return;
	}

	index = rtr_info->index;

	if (index) {
		mysql_mutex_lock(&index->rtr_track->rtr_active_mutex);
	}

	while (rtr_info->parent_path && !rtr_info->parent_path->empty()) {
		btr_pcur_t*	cur = rtr_info->parent_path->back().cursor;
		rtr_info->parent_path->pop_back();

		if (cur) {
			btr_pcur_close(cur);
			ut_free(cur);
		}
	}

	UT_DELETE(rtr_info->parent_path);
	rtr_info->parent_path = NULL;

	if (rtr_info->path != NULL) {
		UT_DELETE(rtr_info->path);
		rtr_info->path = NULL;
		initialized = true;
	}

	if (rtr_info->matches) {
		rtr_info->matches->used = false;
		rtr_info->matches->locked = false;
		rtr_info->matches->valid = false;
		rtr_info->matches->matched_recs->clear();
	}

	if (index) {
		index->rtr_track->rtr_active.remove(rtr_info);
		mysql_mutex_unlock(&index->rtr_track->rtr_active_mutex);
	}

	if (free_all) {
		if (rtr_info->matches) {
			if (rtr_info->matches->block) {
				buf_block_free(rtr_info->matches->block);
				rtr_info->matches->block = nullptr;
			}

			UT_DELETE(rtr_info->matches->matched_recs);

			mysql_mutex_destroy(
				&rtr_info->matches->rtr_match_mutex);
			ut_free(rtr_info->matches);
		}

		if (initialized) {
			mysql_mutex_destroy(&rtr_info->rtr_path_mutex);
		}

		if (rtr_info->allocated) {
			ut_free(rtr_info);
		}
	}
}

/**************************************************************//**
Rebuilt the "path" to exclude the removing page no */
static
void
rtr_rebuild_path(
/*=============*/
	rtr_info_t*	rtr_info,	/*!< in: RTree search info */
	ulint		page_no)	/*!< in: need to free rtr_info itself */
{
	rtr_node_path_t*		new_path
		= UT_NEW_NOKEY(rtr_node_path_t());

	rtr_node_path_t::iterator	rit;
#ifdef UNIV_DEBUG
	ulint	before_size = rtr_info->path->size();
#endif /* UNIV_DEBUG */

	for (rit = rtr_info->path->begin();
	     rit != rtr_info->path->end(); ++rit) {
		node_visit_t	next_rec = *rit;

		if (next_rec.page_no == page_no) {
			continue;
		}

		new_path->push_back(next_rec);
#ifdef UNIV_DEBUG
		node_visit_t	rec = new_path->back();
		ut_ad(rec.level < rtr_info->cursor->tree_height
		      && rec.page_no > 0);
#endif /* UNIV_DEBUG */
	}

	UT_DELETE(rtr_info->path);

	ut_ad(new_path->size() == before_size - 1);

	rtr_info->path = new_path;

	if (!rtr_info->parent_path->empty()) {
		rtr_node_path_t*	new_parent_path = UT_NEW_NOKEY(
			rtr_node_path_t());

		for (rit = rtr_info->parent_path->begin();
		     rit != rtr_info->parent_path->end(); ++rit) {
			node_visit_t	next_rec = *rit;

			if (next_rec.child_no == page_no) {
				btr_pcur_t*	cur = next_rec.cursor;

				if (cur) {
					btr_pcur_close(cur);
					ut_free(cur);
				}

				continue;
			}

			new_parent_path->push_back(next_rec);
		}
		UT_DELETE(rtr_info->parent_path);
		rtr_info->parent_path = new_parent_path;
	}

}

/**************************************************************//**
Check whether a discarding page is in anyone's search path */
void
rtr_check_discard_page(
/*===================*/
	dict_index_t*	index,	/*!< in: index */
	btr_cur_t*	cursor, /*!< in: cursor on the page to discard: not on
				the root page */
	buf_block_t*	block)	/*!< in: block of page to be discarded */
{
	const page_id_t id{block->page.id()};

	mysql_mutex_lock(&index->rtr_track->rtr_active_mutex);

	for (const auto& rtr_info : index->rtr_track->rtr_active) {
		if (cursor && rtr_info == cursor->rtr_info) {
			continue;
		}

		mysql_mutex_lock(&rtr_info->rtr_path_mutex);
		for (const node_visit_t& node : *rtr_info->path) {
			if (node.page_no == id.page_no()) {
				rtr_rebuild_path(rtr_info, node.page_no);
				break;
			}
		}
		mysql_mutex_unlock(&rtr_info->rtr_path_mutex);

		if (auto matches = rtr_info->matches) {
			mysql_mutex_lock(&matches->rtr_match_mutex);

			/* matches->block could be nullptr when cursor
			encounters empty table */
			if (rtr_info->matches->block
			    && matches->block->page.id() == id) {
				matches->matched_recs->clear();
				matches->valid = false;
			}

			mysql_mutex_unlock(&matches->rtr_match_mutex);
		}
	}

	mysql_mutex_unlock(&index->rtr_track->rtr_active_mutex);

	lock_sys.prdt_page_free_from_discard(id, true);
}

/** Restore the stored position of a persistent cursor bufferfixing the page */
static
bool
rtr_cur_restore_position(
	btr_cur_t*	btr_cur,	/*!< in: detached persistent cursor */
	ulint		level,		/*!< in: index level */
	mtr_t*		mtr)		/*!< in: mtr */
{
	dict_index_t*	index;
	mem_heap_t*	heap;
	btr_pcur_t*	r_cursor = rtr_get_parent_cursor(btr_cur, level, false);
	dtuple_t*	tuple;
	bool		ret = false;

	ut_ad(mtr);
	ut_ad(r_cursor);
	ut_ad(mtr->is_active());

	index = btr_cur_get_index(btr_cur);
	ut_ad(r_cursor->index() == btr_cur->index());

	if (r_cursor->rel_pos == BTR_PCUR_AFTER_LAST_IN_TREE
	    || r_cursor->rel_pos == BTR_PCUR_BEFORE_FIRST_IN_TREE) {
		return(false);
	}

	DBUG_EXECUTE_IF(
		"rtr_pessimistic_position",
		r_cursor->modify_clock = 100;
	);

	if (buf_page_optimistic_fix(r_cursor->btr_cur.page_cur.block,
				    r_cursor->old_page_id)
	    && buf_page_optimistic_get(r_cursor->btr_cur.page_cur.block,
				       RW_X_LATCH, r_cursor->modify_clock,
				       mtr)) {
		ut_ad(r_cursor->pos_state == BTR_PCUR_IS_POSITIONED);

		ut_ad(r_cursor->rel_pos == BTR_PCUR_ON);
#ifdef UNIV_DEBUG
		do {
			const rec_t*	rec;
			const rec_offs*	offsets1;
			const rec_offs*	offsets2;
			ulint		comp;

			rec = btr_pcur_get_rec(r_cursor);

			heap = mem_heap_create(256);
			offsets1 = rec_get_offsets(
				r_cursor->old_rec, index, NULL,
				level ? 0 : r_cursor->old_n_fields,
				r_cursor->old_n_fields, &heap);
			offsets2 = rec_get_offsets(
				rec, index, NULL,
				level ? 0 : r_cursor->old_n_fields,
				r_cursor->old_n_fields, &heap);

			comp = rec_offs_comp(offsets1);

			if (rec_get_info_bits(r_cursor->old_rec, comp)
			    & REC_INFO_MIN_REC_FLAG) {
				ut_ad(rec_get_info_bits(rec, comp)
					& REC_INFO_MIN_REC_FLAG);
			} else {

				ut_ad(!cmp_rec_rec(r_cursor->old_rec,
						   rec, offsets1, offsets2,
						   index));
			}

			mem_heap_free(heap);
		} while (0);
#endif /* UNIV_DEBUG */

		return(true);
	}

	/* Page has changed, for R-Tree, the page cannot be shrunk away,
	so we search the page and its right siblings */
	node_seq_t	page_ssn;
	const page_t*	page;
	page_cur_t*	page_cursor;
	node_visit_t*	node = rtr_get_parent_node(btr_cur, level, false);
	node_seq_t	path_ssn = node->seq_no;
	const unsigned	zip_size = index->table->space->zip_size();
	uint32_t	page_no = node->page_no;

	heap = mem_heap_create(256);

	tuple = dict_index_build_data_tuple(r_cursor->old_rec, index, !level,
					    r_cursor->old_n_fields, heap);

	page_cursor = btr_pcur_get_page_cur(r_cursor);
	ut_ad(r_cursor == node->cursor);

search_again:
	ulint up_match = 0, low_match = 0;

	page_cursor->block = buf_page_get_gen(
		page_id_t(index->table->space_id, page_no),
		zip_size, RW_X_LATCH, NULL, BUF_GET, mtr);

	if (!page_cursor->block) {
corrupted:
		ret = false;
		goto func_exit;
	}

	buf_page_make_young_if_needed(&page_cursor->block->page);

	/* Get the page SSN */
	page = buf_block_get_frame(page_cursor->block);
	page_ssn = page_get_ssn_id(page);

	if (page_cur_search_with_match(tuple, PAGE_CUR_LE,
				       &up_match, &low_match, page_cursor,
				       nullptr)) {
		goto corrupted;
	}

	if (low_match == r_cursor->old_n_fields) {
		const rec_t*	rec;
		const rec_offs*	offsets1;
		const rec_offs*	offsets2;
		ulint		comp;

		rec = btr_pcur_get_rec(r_cursor);

		offsets1 = rec_get_offsets(r_cursor->old_rec, index, NULL,
					   level ? 0 : r_cursor->old_n_fields,
					   r_cursor->old_n_fields, &heap);
		offsets2 = rec_get_offsets(rec, index, NULL,
					   level ? 0 : r_cursor->old_n_fields,
					   r_cursor->old_n_fields, &heap);

		comp = rec_offs_comp(offsets1);

		if ((rec_get_info_bits(r_cursor->old_rec, comp)
		     & REC_INFO_MIN_REC_FLAG)
		    && (rec_get_info_bits(rec, comp) & REC_INFO_MIN_REC_FLAG)) {
			r_cursor->pos_state = BTR_PCUR_IS_POSITIONED;
			ret = true;
		} else if (!cmp_rec_rec(r_cursor->old_rec, rec, offsets1, offsets2,
				 index)) {
			r_cursor->pos_state = BTR_PCUR_IS_POSITIONED;
			ret = true;
		}
	}

	/* Check the page SSN to see if it has been splitted, if so, search
	the right page */
	if (!ret && page_ssn > path_ssn) {
		page_no = btr_page_get_next(page);
		goto search_again;
	}

func_exit:
	mem_heap_free(heap);

	return(ret);
}

/****************************************************************//**
Copy the leaf level R-tree record, and push it to matched_rec in rtr_info */
static
void
rtr_leaf_push_match_rec(
/*====================*/
	const rec_t*	rec,		/*!< in: record to copy */
	rtr_info_t*	rtr_info,	/*!< in/out: search stack */
	rec_offs*	offsets,	/*!< in: offsets */
	bool		is_comp)	/*!< in: is compact format */
{
	byte*		buf;
	matched_rec_t*	match_rec = rtr_info->matches;
	rec_t*		copy;
	ulint		data_len;
	rtr_rec_t	rtr_rec;

	buf = match_rec->block->page.frame + match_rec->used;
	ut_ad(page_rec_is_leaf(rec));

	copy = rec_copy(buf, rec, offsets);

	if (is_comp) {
		rec_set_next_offs_new(copy, PAGE_NEW_SUPREMUM);
	} else {
		rec_set_next_offs_old(copy, PAGE_OLD_SUPREMUM);
	}

	rtr_rec.r_rec = copy;
	rtr_rec.locked = false;

	match_rec->matched_recs->push_back(rtr_rec);
	match_rec->valid = true;

	data_len = rec_offs_data_size(offsets) + rec_offs_extra_size(offsets);
	match_rec->used += data_len;

	ut_ad(match_rec->used < srv_page_size);
}

/**************************************************************//**
Store the parent path cursor
@return number of cursor stored */
ulint
rtr_store_parent_path(
/*==================*/
	const buf_block_t*	block,	/*!< in: block of the page */
	btr_cur_t*		btr_cur,/*!< in/out: persistent cursor */
	btr_latch_mode		latch_mode,
					/*!< in: latch_mode */
	ulint			level,	/*!< in: index level */
	mtr_t*			mtr)	/*!< in: mtr */
{
	ulint	num = btr_cur->rtr_info->parent_path->size();
	ulint	num_stored = 0;

	while (num >= 1) {
		node_visit_t*	node = &(*btr_cur->rtr_info->parent_path)[
					num - 1];
		btr_pcur_t*	r_cursor = node->cursor;
		buf_block_t*	cur_block;

		if (node->level > level) {
			break;
		}

		r_cursor->pos_state = BTR_PCUR_IS_POSITIONED;
		r_cursor->latch_mode = latch_mode;

		cur_block = btr_pcur_get_block(r_cursor);

		if (cur_block == block) {
			btr_pcur_store_position(r_cursor, mtr);
			num_stored++;
		} else {
			break;
		}

		num--;
	}

	return(num_stored);
}
/**************************************************************//**
push a nonleaf index node to the search path for insertion */
static
void
rtr_non_leaf_insert_stack_push(
/*===========================*/
	dict_index_t*		index,	/*!< in: index descriptor */
	rtr_node_path_t*	path,	/*!< in/out: search path */
	ulint			level,	/*!< in: index page level */
	uint32_t		child_no,/*!< in: child page no */
	const buf_block_t*	block,	/*!< in: block of the page */
	const rec_t*		rec,	/*!< in: positioned record */
	double			mbr_inc)/*!< in: MBR needs to be enlarged */
{
	node_seq_t	new_seq;
	btr_pcur_t*	my_cursor;

	my_cursor = static_cast<btr_pcur_t*>(
		ut_malloc_nokey(sizeof(*my_cursor)));

	btr_pcur_init(my_cursor);

	page_cur_position(rec, block, btr_pcur_get_page_cur(my_cursor));

	btr_pcur_get_page_cur(my_cursor)->index = index;

	new_seq = rtr_get_current_ssn_id(index);
	rtr_non_leaf_stack_push(path, block->page.id().page_no(),
				new_seq, level, child_no, my_cursor, mbr_inc);
}

/****************************************************************//**
Generate a shadow copy of the page block header to save the
matched records */
static
void
rtr_init_match(
/*===========*/
	matched_rec_t*		matches,/*!< in/out: match to initialize */
	const buf_block_t*	block,	/*!< in: buffer block */
	const page_t*		page)	/*!< in: buffer page */
{
	ut_ad(matches->matched_recs->empty());
	matches->locked = false;
	matches->valid = false;
	if (!matches->block) {
		matches->block = buf_block_alloc();
	}

	matches->block->page.init(buf_page_t::MEMORY, block->page.id());
	/* We have to copy PAGE_*_SUPREMUM_END bytes so that we can
	use infimum/supremum of this page as normal btr page for search. */
	matches->used = page_is_comp(page)
				? PAGE_NEW_SUPREMUM_END
				: PAGE_OLD_SUPREMUM_END;
	memcpy(matches->block->page.frame, page, matches->used);
#ifdef RTR_SEARCH_DIAGNOSTIC
	ulint pageno = page_get_page_no(page);
	fprintf(stderr, "INNODB_RTR: Searching leaf page %d\n",
		static_cast<int>(pageno));
#endif /* RTR_SEARCH_DIAGNOSTIC */
}

/****************************************************************//**
Get the bounding box content from an index record */
void
rtr_get_mbr_from_rec(
/*=================*/
	const rec_t*	rec,	/*!< in: data tuple */
	const rec_offs*	offsets,/*!< in: offsets array */
	rtr_mbr_t*	mbr)	/*!< out MBR */
{
	ulint		rec_f_len;
	const byte*	data;

	data = rec_get_nth_field(rec, offsets, 0, &rec_f_len);

	rtr_read_mbr(data, mbr);
}

/****************************************************************//**
Get the bounding box content from a MBR data record */
void
rtr_get_mbr_from_tuple(
/*===================*/
	const dtuple_t* dtuple, /*!< in: data tuple */
	rtr_mbr*	mbr)	/*!< out: mbr to fill */
{
	const dfield_t* dtuple_field;
        ulint           dtuple_f_len;

	dtuple_field = dtuple_get_nth_field(dtuple, 0);
	dtuple_f_len = dfield_get_len(dtuple_field);
	ut_a(dtuple_f_len >= 4 * sizeof(double));

	rtr_read_mbr(static_cast<const byte*>(dfield_get_data(dtuple_field)),
		     mbr);
}

/** Compare minimum bounding rectangles.
@return	1, 0, -1, if mode == PAGE_CUR_MBR_EQUAL. And return
1, 0 for rest compare modes, depends on a and b qualifies the
relationship (CONTAINS, WITHIN etc.) */
static int cmp_gis_field(page_cur_mode_t mode, const void *a, const void *b)
{
  return mode == PAGE_CUR_MBR_EQUAL
    ? cmp_geometry_field(a, b)
    : rtree_key_cmp(mode, a, b);
}

/** Compare a GIS data tuple to a physical record in rtree non-leaf node.
We need to check the page number field, since we don't store pk field in
rtree non-leaf node.
@param[in]	dtuple		data tuple
@param[in]	rec		R-tree record
@return whether dtuple is less than rec */
static bool
cmp_dtuple_rec_with_gis_internal(const dtuple_t* dtuple, const rec_t* rec)
{
  const dfield_t *dtuple_field= dtuple_get_nth_field(dtuple, 0);
  ut_ad(dfield_get_len(dtuple_field) == DATA_MBR_LEN);

  if (cmp_gis_field(PAGE_CUR_WITHIN, dfield_get_data(dtuple_field), rec))
    return true;

  dtuple_field= dtuple_get_nth_field(dtuple, 1);
  ut_ad(dfield_get_len(dtuple_field) == 4); /* child page number */
  ut_ad(dtuple_field->type.mtype == DATA_SYS_CHILD);
  ut_ad(!(dtuple_field->type.prtype & ~DATA_NOT_NULL));

  return memcmp(dtuple_field->data, rec + DATA_MBR_LEN, 4) != 0;
}

#ifndef UNIV_DEBUG
static
#endif
/** Compare a GIS data tuple to a physical record.
@param[in] dtuple data tuple
@param[in] rec R-tree record
@param[in] mode compare mode
@retval negative if dtuple is less than rec */
int cmp_dtuple_rec_with_gis(const dtuple_t *dtuple, const rec_t *rec,
                            page_cur_mode_t mode)
{
  const dfield_t *dtuple_field= dtuple_get_nth_field(dtuple, 0);
  /* FIXME: TABLE_SHARE::init_from_binary_frm_image() is adding
  field->key_part_length_bytes() to the key length */
  ut_ad(dfield_get_len(dtuple_field) == DATA_MBR_LEN ||
        dfield_get_len(dtuple_field) == DATA_MBR_LEN + 2);

  return cmp_gis_field(mode, dfield_get_data(dtuple_field), rec);
}

/****************************************************************//**
Searches the right position in rtree for a page cursor. */
bool
rtr_cur_search_with_match(
/*======================*/
	const buf_block_t*	block,	/*!< in: buffer block */
	dict_index_t*		index,	/*!< in: index descriptor */
	const dtuple_t*		tuple,	/*!< in: data tuple */
	page_cur_mode_t		mode,	/*!< in: PAGE_CUR_RTREE_INSERT,
					PAGE_CUR_RTREE_LOCATE etc. */
	page_cur_t*		cursor,	/*!< in/out: page cursor */
	rtr_info_t*		rtr_info)/*!< in/out: search stack */
{
	bool		found = false;
	const page_t*	page;
	const rec_t*	rec;
	const rec_t*	last_rec;
	rec_offs	offsets_[REC_OFFS_NORMAL_SIZE];
	rec_offs*	offsets		= offsets_;
	mem_heap_t*	heap = NULL;
	int		cmp = 1;
	double		least_inc = DBL_MAX;
	const rec_t*	best_rec;
	const rec_t*	last_match_rec = NULL;
	bool		match_init = false;
	page_cur_mode_t	orig_mode = mode;
	const rec_t*	first_rec = NULL;

	rec_offs_init(offsets_);

	ut_ad(RTREE_SEARCH_MODE(mode));

	ut_ad(dict_index_is_spatial(index));

	page = buf_block_get_frame(block);

	const ulint level = btr_page_get_level(page);
	const ulint n_core = level ? 0 : index->n_fields;

	if (mode == PAGE_CUR_RTREE_LOCATE) {
		ut_ad(level != 0);
		mode = PAGE_CUR_WITHIN;
	}

	rec = page_dir_slot_get_rec_validate(page_dir_get_nth_slot(page, 0));

	if (UNIV_UNLIKELY(!rec)) {
		return false;
	}

	last_rec = rec;
	best_rec = rec;

	if (page_rec_is_infimum(rec)) {
		rec = page_rec_get_next_const(rec);
		if (UNIV_UNLIKELY(!rec)) {
			return false;
		}
	}

	/* Check insert tuple size is larger than first rec, and try to
	avoid it if possible */
	if (mode == PAGE_CUR_RTREE_INSERT && !page_rec_is_supremum(rec)) {

		ulint	new_rec_size = rec_get_converted_size(index, tuple, 0);

		offsets = rec_get_offsets(rec, index, offsets, n_core,
					  dtuple_get_n_fields_cmp(tuple),
					  &heap);

		if (rec_offs_size(offsets) < new_rec_size) {
			first_rec = rec;
		}

		/* If this is the left-most page of this index level
		and the table is a compressed table, try to avoid
		first page as much as possible, as there will be problem
		when update MIN_REC rec in compress table */
		if (is_buf_block_get_page_zip(block)
		    && !page_has_prev(page)
		    && page_get_n_recs(page) >= 2) {

			rec = page_rec_get_next_const(rec);
		}
	}

	while (!page_rec_is_supremum(rec)) {
		if (!n_core) {
			switch (mode) {
			case PAGE_CUR_CONTAIN:
			case PAGE_CUR_INTERSECT:
			case PAGE_CUR_MBR_EQUAL:
				/* At non-leaf level, we will need to check
				both CONTAIN and INTERSECT for either of
				the search mode */
				cmp = cmp_dtuple_rec_with_gis(
					tuple, rec, PAGE_CUR_CONTAIN);

				if (cmp != 0) {
					cmp = cmp_dtuple_rec_with_gis(
						tuple, rec,
						PAGE_CUR_INTERSECT);
				}
				break;
			case PAGE_CUR_DISJOINT:
				cmp = cmp_dtuple_rec_with_gis(
					tuple, rec, mode);

				if (cmp != 0) {
					cmp = cmp_dtuple_rec_with_gis(
						tuple, rec,
						PAGE_CUR_INTERSECT);
				}
				break;
			case PAGE_CUR_RTREE_INSERT:
				double	increase;
				double	area;

				cmp = cmp_dtuple_rec_with_gis(
					tuple, rec, PAGE_CUR_WITHIN);

				if (cmp != 0) {
					increase = rtr_rec_cal_increase(
						tuple, rec, &area);
					/* Once it goes beyond DBL_MAX,
					it would not make sense to record
					such value, just make it
					DBL_MAX / 2  */
					if (increase >= DBL_MAX) {
						increase = DBL_MAX / 2;
					}

					if (increase < least_inc) {
						least_inc = increase;
						best_rec = rec;
					} else if (best_rec
						   && best_rec == first_rec) {
						/* if first_rec is set,
						we will try to avoid it */
						least_inc = increase;
						best_rec = rec;
					}
				}
				break;
			case PAGE_CUR_RTREE_GET_FATHER:
				cmp = cmp_dtuple_rec_with_gis_internal(
					tuple, rec);
				break;
			default:
				/* WITHIN etc. */
				cmp = cmp_dtuple_rec_with_gis(
					tuple, rec, mode);
			}
		} else {
			/* At leaf level, INSERT should translate to LE */
			ut_ad(mode != PAGE_CUR_RTREE_INSERT);

			cmp = cmp_dtuple_rec_with_gis(
				tuple, rec, mode);
		}

		if (cmp == 0) {
			found = true;

			/* If located, the matching node/rec will be pushed
			to rtr_info->path for non-leaf nodes, or
			rtr_info->matches for leaf nodes */
			if (rtr_info && mode != PAGE_CUR_RTREE_INSERT) {
				if (!n_core) {
					uint32_t	page_no;
					node_seq_t	new_seq;
					bool		is_loc;

					is_loc = (orig_mode
						  == PAGE_CUR_RTREE_LOCATE
						  || orig_mode
						  == PAGE_CUR_RTREE_GET_FATHER);

					offsets = rec_get_offsets(
						rec, index, offsets, 0,
						ULINT_UNDEFINED, &heap);

					page_no = btr_node_ptr_get_child_page_no(
						rec, offsets);

					ut_ad(level >= 1);

					/* Get current SSN, before we insert
					it into the path stack */
					new_seq = rtr_get_current_ssn_id(index);

					rtr_non_leaf_stack_push(
						rtr_info->path,
						page_no,
						new_seq, level - 1, 0,
						NULL, 0);

					if (is_loc) {
						rtr_non_leaf_insert_stack_push(
							index,
							rtr_info->parent_path,
							level, page_no, block,
							rec, 0);
					}

					if (!srv_read_only_mode
					    && (rtr_info->need_page_lock
						|| !is_loc)) {

						/* Lock the page, preventing it
						from being shrunk */
						lock_place_prdt_page_lock(
							page_id_t(block->page
								  .id()
								  .space(),
								  page_no),
							index,
							rtr_info->thr);
					}
				} else {
					ut_ad(orig_mode
					      != PAGE_CUR_RTREE_LOCATE);

					/* Collect matched records on page */
					offsets = rec_get_offsets(
						rec, index, offsets,
						index->n_fields,
						ULINT_UNDEFINED, &heap);

					mysql_mutex_lock(
					  &rtr_info->matches->rtr_match_mutex);

					if (!match_init) {
						rtr_init_match(
							rtr_info->matches,
							block, page);
						match_init = true;
					}

					rtr_leaf_push_match_rec(
						rec, rtr_info, offsets,
						page_is_comp(page));

					mysql_mutex_unlock(
					  &rtr_info->matches->rtr_match_mutex);
				}

				last_match_rec = rec;
			} else {
				/* This is the insertion case, it will break
				once it finds the first MBR that can accomodate
				the inserting rec */
				break;
			}
		}

		last_rec = rec;

		rec = page_rec_get_next_const(rec);
	}

	/* All records on page are searched */
	if (rec && page_rec_is_supremum(rec)) {
		if (!n_core) {
			if (!found) {
				/* No match case, if it is for insertion,
				then we select the record that result in
				least increased area */
				if (mode == PAGE_CUR_RTREE_INSERT) {
					ut_ad(least_inc < DBL_MAX);
					offsets = rec_get_offsets(
						best_rec, index, offsets,
						0, ULINT_UNDEFINED, &heap);
					uint32_t child_no =
					btr_node_ptr_get_child_page_no(
						best_rec, offsets);

					rtr_non_leaf_insert_stack_push(
						index, rtr_info->parent_path,
						level, child_no, block,
						best_rec, least_inc);

					page_cur_position(best_rec, block,
							  cursor);
					rtr_info->mbr_adj = true;
				} else {
					/* Position at the last rec of the
					page, if it is not the leaf page */
					page_cur_position(last_rec, block,
							  cursor);
				}
			} else {
				/* There are matching records, position
				in the last matching records */
				if (rtr_info) {
					rec = last_match_rec;
					page_cur_position(
						rec, block, cursor);
				}
			}
		} else if (rtr_info) {
			/* Leaf level, no match, position at the
			last (supremum) rec */
			if (!last_match_rec) {
				page_cur_position(rec, block, cursor);
				goto func_exit;
			}

			/* There are matched records */
			matched_rec_t*	match_rec = rtr_info->matches;

			rtr_rec_t	test_rec;

			test_rec = match_rec->matched_recs->back();
#ifdef UNIV_DEBUG
			rec_offs	offsets_2[REC_OFFS_NORMAL_SIZE];
			rec_offs*	offsets2	= offsets_2;
			rec_offs_init(offsets_2);

			ut_ad(found);

			/* Verify the record to be positioned is the same
			as the last record in matched_rec vector */
			offsets2 = rec_get_offsets(test_rec.r_rec, index,
						   offsets2, index->n_fields,
						   ULINT_UNDEFINED, &heap);

			offsets = rec_get_offsets(last_match_rec, index,
						  offsets, index->n_fields,
						  ULINT_UNDEFINED, &heap);

			ut_ad(cmp_rec_rec(test_rec.r_rec, last_match_rec,
					  offsets2, offsets, index) == 0);
#endif /* UNIV_DEBUG */
			/* Pop the last match record and position on it */
			match_rec->matched_recs->pop_back();
			page_cur_position(test_rec.r_rec, match_rec->block,
					  cursor);
		}
	} else {

		if (mode == PAGE_CUR_RTREE_INSERT) {
			ut_ad(!last_match_rec);
			rtr_non_leaf_insert_stack_push(
				index, rtr_info->parent_path, level,
				mach_read_from_4(rec + DATA_MBR_LEN),
				block, rec, 0);

		} else if (rtr_info && found && !n_core) {
			rec = last_match_rec;
		}

		page_cur_position(rec, block, cursor);
	}

#ifdef UNIV_DEBUG
	/* Verify that we are positioned at the same child page as pushed in
	the path stack */
	if (!n_core && (!page_rec_is_supremum(rec) || found)
	    && mode != PAGE_CUR_RTREE_INSERT) {
		ulint		page_no;

		offsets = rec_get_offsets(rec, index, offsets, 0,
					  ULINT_UNDEFINED, &heap);
		page_no = btr_node_ptr_get_child_page_no(rec, offsets);

		if (rtr_info && found) {
			rtr_node_path_t*	path = rtr_info->path;
			node_visit_t		last_visit = path->back();

			ut_ad(last_visit.page_no == page_no);
		}
	}
#endif /* UNIV_DEBUG */

func_exit:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return(found);
}
