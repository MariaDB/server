/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2021, MariaDB Corporation.

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
@file btr/btr0pcur.cc
The index tree persistent cursor

Created 2/23/1996 Heikki Tuuri
*******************************************************/

#include "btr0pcur.h"
#include "ut0byte.h"
#include "rem0cmp.h"
#include "trx0trx.h"

/**************************************************************//**
Allocates memory for a persistent cursor object and initializes the cursor.
@return own: persistent cursor */
btr_pcur_t*
btr_pcur_create_for_mysql(void)
/*============================*/
{
	btr_pcur_t*	pcur;
	DBUG_ENTER("btr_pcur_create_for_mysql");

	pcur = (btr_pcur_t*) ut_malloc_nokey(sizeof(btr_pcur_t));

	pcur->btr_cur.index = NULL;
	btr_pcur_init(pcur);

	DBUG_PRINT("btr_pcur_create_for_mysql", ("pcur: %p", pcur));
	DBUG_RETURN(pcur);
}

/**************************************************************//**
Resets a persistent cursor object, freeing ::old_rec_buf if it is
allocated and resetting the other members to their initial values. */
void
btr_pcur_reset(
/*===========*/
	btr_pcur_t*	cursor)	/*!< in, out: persistent cursor */
{
	btr_pcur_free(cursor);
	cursor->old_rec_buf = NULL;
	cursor->btr_cur.index = NULL;
	cursor->btr_cur.page_cur.rec = NULL;
	cursor->old_rec = NULL;
	cursor->old_n_core_fields = 0;
	cursor->old_n_fields = 0;
	cursor->old_stored = false;

	cursor->latch_mode = BTR_NO_LATCHES;
	cursor->pos_state = BTR_PCUR_NOT_POSITIONED;
}

/**************************************************************//**
Frees the memory for a persistent cursor object. */
void
btr_pcur_free_for_mysql(
/*====================*/
	btr_pcur_t*	cursor)	/*!< in, own: persistent cursor */
{
	DBUG_ENTER("btr_pcur_free_for_mysql");
	DBUG_PRINT("btr_pcur_free_for_mysql", ("pcur: %p", cursor));

	btr_pcur_free(cursor);
	ut_free(cursor);
	DBUG_VOID_RETURN;
}

/**************************************************************//**
The position of the cursor is stored by taking an initial segment of the
record the cursor is positioned on, before, or after, and copying it to the
cursor data structure, or just setting a flag if the cursor id before the
first in an EMPTY tree, or after the last in an EMPTY tree. NOTE that the
page where the cursor is positioned must not be empty if the index tree is
not totally empty! */
void
btr_pcur_store_position(
/*====================*/
	btr_pcur_t*	cursor, /*!< in: persistent cursor */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_cur_t*	page_cursor;
	buf_block_t*	block;
	rec_t*		rec;
	dict_index_t*	index;
	ulint		offs;

	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	block = btr_pcur_get_block(cursor);
	index = btr_cur_get_index(btr_pcur_get_btr_cur(cursor));

	page_cursor = btr_pcur_get_page_cur(cursor);

	rec = page_cur_get_rec(page_cursor);
	offs = rec - block->page.frame;
	ut_ad(block->page.id().page_no()
	      == page_get_page_no(block->page.frame));
	ut_ad(block->page.buf_fix_count());
	/* For spatial index, when we do positioning on parent
	buffer if necessary, it might not hold latches, but the
	tree must be locked to prevent change on the page */
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_S_FIX
					 | MTR_MEMO_PAGE_X_FIX)
	      || (index->is_spatial()
		  && mtr->memo_contains_flagged(&index->lock, MTR_MEMO_X_LOCK
						| MTR_MEMO_SX_LOCK)));

	cursor->old_stored = true;

	if (page_is_empty(block->page.frame)) {
		/* It must be an empty index tree; NOTE that in this case
		we do not store the modify_clock, but always do a search
		if we restore the cursor position */

		ut_a(!page_has_siblings(block->page.frame));
		ut_ad(page_is_leaf(block->page.frame));
		ut_ad(block->page.id().page_no() == index->page);

		if (page_rec_is_supremum_low(offs)) {
			cursor->rel_pos = BTR_PCUR_AFTER_LAST_IN_TREE;
		} else {
before_first:
			cursor->rel_pos = BTR_PCUR_BEFORE_FIRST_IN_TREE;
		}

		return;
	}

	if (page_rec_is_supremum_low(offs)) {
		rec = page_rec_get_prev(rec);

		ut_ad(!page_rec_is_infimum(rec));
		if (UNIV_UNLIKELY(rec_is_metadata(rec, *index))) {
#if 0 /* MDEV-22867 had to relax this */
			/* If the table is emptied during an ALGORITHM=NOCOPY
			DROP COLUMN ... that is not ALGORITHM=INSTANT,
			then we must preserve any instant ADD metadata. */
			ut_ad(index->table->instant
			      || block->page.id().page_no() != index->page);
#endif
			ut_ad(index->is_instant()
			      || block->page.id().page_no() != index->page);
			ut_ad(page_get_n_recs(block->page.frame) == 1);
			ut_ad(page_is_leaf(block->page.frame));
			ut_ad(!page_has_prev(block->page.frame));
			cursor->rel_pos = BTR_PCUR_AFTER_LAST_IN_TREE;
			return;
		}

		cursor->rel_pos = BTR_PCUR_AFTER;
	} else if (page_rec_is_infimum_low(offs)) {
		rec = page_rec_get_next(rec);

		if (rec_is_metadata(rec, *index)) {
			ut_ad(!page_has_prev(block->page.frame));
			rec = page_rec_get_next(rec);
			if (page_rec_is_supremum(rec)) {
				goto before_first;
			}
		}

		cursor->rel_pos = BTR_PCUR_BEFORE;
	} else {
		cursor->rel_pos = BTR_PCUR_ON;
	}

	if (index->is_ibuf()) {
		ut_ad(!index->table->not_redundant());
		cursor->old_n_fields = uint16_t(rec_get_n_fields_old(rec));
	} else {
		cursor->old_n_fields = static_cast<uint16>(
			dict_index_get_n_unique_in_tree(index));
		if (index->is_spatial() && !page_rec_is_leaf(rec)) {
			ut_ad(dict_index_get_n_unique_in_tree_nonleaf(index)
			      == DICT_INDEX_SPATIAL_NODEPTR_SIZE);
			/* For R-tree, we have to compare
			the child page numbers as well. */
			cursor->old_n_fields
				= DICT_INDEX_SPATIAL_NODEPTR_SIZE + 1;
		}
	}

	cursor->old_n_core_fields = index->n_core_fields;
	cursor->old_rec = rec_copy_prefix_to_buf(rec, index,
						 cursor->old_n_fields,
						 &cursor->old_rec_buf,
						 &cursor->buf_size);
	cursor->block_when_stored.store(block);

	/* Function try to check if block is S/X latch. */
	cursor->modify_clock = buf_block_get_modify_clock(block);
}

/**************************************************************//**
Copies the stored position of a pcur to another pcur. */
void
btr_pcur_copy_stored_position(
/*==========================*/
	btr_pcur_t*	pcur_receive,	/*!< in: pcur which will receive the
					position info */
	btr_pcur_t*	pcur_donate)	/*!< in: pcur from which the info is
					copied */
{
	ut_free(pcur_receive->old_rec_buf);
	memcpy(pcur_receive, pcur_donate, sizeof(btr_pcur_t));

	if (pcur_donate->old_rec_buf) {

		pcur_receive->old_rec_buf = (byte*)
			ut_malloc_nokey(pcur_donate->buf_size);

		memcpy(pcur_receive->old_rec_buf, pcur_donate->old_rec_buf,
		       pcur_donate->buf_size);
		pcur_receive->old_rec = pcur_receive->old_rec_buf
			+ (pcur_donate->old_rec - pcur_donate->old_rec_buf);
	}

	pcur_receive->old_n_core_fields = pcur_donate->old_n_core_fields;
	pcur_receive->old_n_fields = pcur_donate->old_n_fields;
}

/** Structure acts as functor to do the latching of leaf pages.
It returns true if latching of leaf pages succeeded and false
otherwise. */
struct optimistic_latch_leaves
{
  btr_pcur_t *const cursor;
  ulint *latch_mode;
  mtr_t *const mtr;

  optimistic_latch_leaves(btr_pcur_t *cursor, ulint *latch_mode, mtr_t *mtr)
  :cursor(cursor), latch_mode(latch_mode), mtr(mtr) {}

  bool operator() (buf_block_t *hint) const
  {
    return hint && btr_cur_optimistic_latch_leaves(
             hint, cursor->modify_clock, latch_mode,
             btr_pcur_get_btr_cur(cursor), mtr);
  }
};

/** Restores the stored position of a persistent cursor bufferfixing
the page and obtaining the specified latches. If the cursor position
was saved when the
(1) cursor was positioned on a user record: this function restores the
position to the last record LESS OR EQUAL to the stored record;
(2) cursor was positioned on a page infimum record: restores the
position to the last record LESS than the user record which was the
successor of the page infimum;
(3) cursor was positioned on the page supremum: restores to the first
record GREATER than the user record which was the predecessor of the
supremum.
(4) cursor was positioned before the first or after the last in an
empty tree: restores to before first or after the last in the tree.
@param restore_latch_mode BTR_SEARCH_LEAF, ...
@param mtr mtr
@return btr_pcur_t::SAME_ALL cursor position on user rec and points on
the record with the same field values as in the stored record,
btr_pcur_t::SAME_UNIQ cursor position is on user rec and points on the
record with the same unique field values as in the stored record,
btr_pcur_t::NOT_SAME cursor position is not on user rec or points on
the record with not the samebuniq field values as in the stored */
btr_pcur_t::restore_status
btr_pcur_t::restore_position(ulint restore_latch_mode, mtr_t *mtr)
{
	dict_index_t*	index;
	dtuple_t*	tuple;
	page_cur_mode_t	mode;
	page_cur_mode_t	old_mode;
	mem_heap_t*	heap;

	ut_ad(mtr->is_active());
	//ut_ad(cursor->old_stored);
	ut_ad(pos_state == BTR_PCUR_WAS_POSITIONED
	      || pos_state == BTR_PCUR_IS_POSITIONED);

	index = btr_cur_get_index(&btr_cur);

	if (UNIV_UNLIKELY
	    (rel_pos == BTR_PCUR_AFTER_LAST_IN_TREE
	     || rel_pos == BTR_PCUR_BEFORE_FIRST_IN_TREE)) {
		dberr_t err = DB_SUCCESS;

		/* In these cases we do not try an optimistic restoration,
		but always do a search */

		err = btr_cur_open_at_index_side(
			rel_pos == BTR_PCUR_BEFORE_FIRST_IN_TREE,
			index, restore_latch_mode,
			&btr_cur, 0, mtr);

		if (err != DB_SUCCESS) {
			ib::warn() << " Error code: " << err
				   << " btr_pcur_t::restore_position "
				   << " table: " << index->table->name
				   << " index: " << index->name;
		}

		latch_mode =
			BTR_LATCH_MODE_WITHOUT_INTENTION(restore_latch_mode);
		pos_state = BTR_PCUR_IS_POSITIONED;
		block_when_stored.clear();

		return restore_status::NOT_SAME;
	}

	ut_a(old_rec);
	ut_a(old_n_core_fields);
	ut_a(old_n_core_fields <= index->n_core_fields);
	ut_a(old_n_fields);

	switch (restore_latch_mode) {
	case BTR_SEARCH_LEAF:
	case BTR_MODIFY_LEAF:
	case BTR_SEARCH_PREV:
	case BTR_MODIFY_PREV:
		/* Try optimistic restoration. */

		if (block_when_stored.run_with_hint(
			optimistic_latch_leaves(this, &restore_latch_mode,
						mtr))) {
			pos_state = BTR_PCUR_IS_POSITIONED;
			latch_mode = restore_latch_mode;

			if (rel_pos == BTR_PCUR_ON) {
#ifdef UNIV_DEBUG
				const rec_t*	rec;
				rec_offs	offsets1_[REC_OFFS_NORMAL_SIZE];
				rec_offs	offsets2_[REC_OFFS_NORMAL_SIZE];
				rec_offs*	offsets1 = offsets1_;
				rec_offs*	offsets2 = offsets2_;
				rec = btr_pcur_get_rec(this);

				rec_offs_init(offsets1_);
				rec_offs_init(offsets2_);

				heap = mem_heap_create(256);
				ut_ad(old_n_core_fields
				      == index->n_core_fields);

				offsets1 = rec_get_offsets(
					old_rec, index, offsets1,
					old_n_core_fields,
					old_n_fields, &heap);
				offsets2 = rec_get_offsets(
					rec, index, offsets2,
					index->n_core_fields,
					old_n_fields, &heap);

				ut_ad(!cmp_rec_rec(old_rec,
						   rec, offsets1, offsets2,
						   index));
				mem_heap_free(heap);
#endif /* UNIV_DEBUG */
				return restore_status::SAME_ALL;
			}
			/* This is the same record as stored,
			may need to be adjusted for BTR_PCUR_BEFORE/AFTER,
			depending on search mode and direction. */
			if (btr_pcur_is_on_user_rec(this)) {
				pos_state
					= BTR_PCUR_IS_POSITIONED_OPTIMISTIC;
			}
			return restore_status::NOT_SAME;
		}
	}

	/* If optimistic restoration did not succeed, open the cursor anew */

	heap = mem_heap_create(256);

	tuple = dtuple_create(heap, old_n_fields);

	dict_index_copy_types(tuple, index, old_n_fields);

	rec_copy_prefix_to_dtuple(tuple, old_rec, index,
				  old_n_core_fields,
				  old_n_fields, heap);
	ut_ad(dtuple_check_typed(tuple));

	/* Save the old search mode of the cursor */
	old_mode = search_mode;

	switch (rel_pos) {
	case BTR_PCUR_ON:
		mode = PAGE_CUR_LE;
		break;
	case BTR_PCUR_AFTER:
		mode = PAGE_CUR_G;
		break;
	case BTR_PCUR_BEFORE:
		mode = PAGE_CUR_L;
		break;
	default:
		ut_error;
		mode = PAGE_CUR_UNSUPP;
	}

	btr_pcur_open_with_no_init_func(index, tuple, mode, restore_latch_mode,
					this,
#ifdef BTR_CUR_HASH_ADAPT
					NULL,
#endif /* BTR_CUR_HASH_ADAPT */
					mtr);

	/* Restore the old search mode */
	search_mode = old_mode;

	ut_ad(rel_pos == BTR_PCUR_ON
	      || rel_pos == BTR_PCUR_BEFORE
	      || rel_pos == BTR_PCUR_AFTER);
	rec_offs offsets[REC_OFFS_NORMAL_SIZE];
	rec_offs_init(offsets);
	restore_status ret_val= restore_status::NOT_SAME;
	if (rel_pos == BTR_PCUR_ON && btr_pcur_is_on_user_rec(this)) {
		ulint n_matched_fields= 0;
		if (!cmp_dtuple_rec_with_match(
		      tuple, btr_pcur_get_rec(this), index,
		      rec_get_offsets(btr_pcur_get_rec(this), index, offsets,
			index->n_core_fields, ULINT_UNDEFINED, &heap),
		      &n_matched_fields)) {

			/* We have to store the NEW value for the modify clock,
			since the cursor can now be on a different page!
			But we can retain the value of old_rec */

			block_when_stored.store(btr_pcur_get_block(this));
			modify_clock= buf_block_get_modify_clock(
			    block_when_stored.block());
			old_stored= true;

			mem_heap_free(heap);

			return restore_status::SAME_ALL;
		}
		if (n_matched_fields >= index->n_uniq)
			ret_val= restore_status::SAME_UNIQ;
	}

	mem_heap_free(heap);

	/* We have to store new position information, modify_clock etc.,
	to the cursor because it can now be on a different page, the record
	under it may have been removed, etc. */

	btr_pcur_store_position(this, mtr);

	return ret_val;
}

/*********************************************************//**
Moves the persistent cursor to the first record on the next page. Releases the
latch on the current page, and bufferunfixes it. Note that there must not be
modifications on the current page, as then the x-latch can be released only in
mtr_commit. */
void
btr_pcur_move_to_next_page(
/*=======================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; must be on the
				last record of the current page */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
	ut_ad(btr_pcur_is_after_last_on_page(cursor));

	cursor->old_stored = false;

	const page_t* page = btr_pcur_get_page(cursor);

	if (UNIV_UNLIKELY(!page)) {
		return;
	}

	const uint32_t next_page_no = btr_page_get_next(page);

	ut_ad(next_page_no != FIL_NULL);

	ulint mode = cursor->latch_mode;
	switch (mode) {
	case BTR_SEARCH_TREE:
		mode = BTR_SEARCH_LEAF;
		break;
	case BTR_MODIFY_TREE:
		mode = BTR_MODIFY_LEAF;
	}

	buf_block_t* next_block = btr_block_get(
		*btr_pcur_get_btr_cur(cursor)->index, next_page_no, mode,
		page_is_leaf(page), mtr);

	if (UNIV_UNLIKELY(!next_block)) {
		return;
	}

	const page_t* next_page = buf_block_get_frame(next_block);
#ifdef UNIV_BTR_DEBUG
	ut_a(page_is_comp(next_page) == page_is_comp(page));
	ut_a(btr_page_get_prev(next_page)
	     == btr_pcur_get_block(cursor)->page.id().page_no());
#endif /* UNIV_BTR_DEBUG */

	btr_leaf_page_release(btr_pcur_get_block(cursor), mode, mtr);

	page_cur_set_before_first(next_block, btr_pcur_get_page_cur(cursor));

	ut_d(page_check_dir(next_page));
}

/*********************************************************//**
Moves the persistent cursor backward if it is on the first record of the page.
Commits mtr. Note that to prevent a possible deadlock, the operation
first stores the position of the cursor, commits mtr, acquires the necessary
latches and restores the cursor position again before returning. The
alphabetical position of the cursor is guaranteed to be sensible on
return, but it may happen that the cursor is not positioned on the last
record of any page, because the structure of the tree may have changed
during the time when the cursor had no latches. */
static
void
btr_pcur_move_backward_from_page(
/*=============================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor, must be on the first
				record of the current page */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint		prev_page_no;
	page_t*		page;
	buf_block_t*	prev_block;
	ulint		latch_mode;
	ulint		latch_mode2;

	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
	ut_ad(btr_pcur_is_before_first_on_page(cursor));
	ut_ad(!btr_pcur_is_before_first_in_tree(cursor));

	latch_mode = cursor->latch_mode;

	if (latch_mode == BTR_SEARCH_LEAF) {

		latch_mode2 = BTR_SEARCH_PREV;

	} else if (latch_mode == BTR_MODIFY_LEAF) {

		latch_mode2 = BTR_MODIFY_PREV;
	} else {
		latch_mode2 = 0; /* To eliminate compiler warning */
		ut_error;
	}

	btr_pcur_store_position(cursor, mtr);

	mtr_commit(mtr);

	mtr_start(mtr);

	cursor->restore_position(latch_mode2, mtr);

	page = btr_pcur_get_page(cursor);

	prev_page_no = btr_page_get_prev(page);

	if (prev_page_no == FIL_NULL) {
	} else if (btr_pcur_is_before_first_on_page(cursor)) {

		prev_block = btr_pcur_get_btr_cur(cursor)->left_block;

		btr_leaf_page_release(btr_pcur_get_block(cursor),
				      latch_mode, mtr);

		page_cur_set_after_last(prev_block,
					btr_pcur_get_page_cur(cursor));
	} else {

		/* The repositioned cursor did not end on an infimum
		record on a page. Cursor repositioning acquired a latch
		also on the previous page, but we do not need the latch:
		release it. */

		prev_block = btr_pcur_get_btr_cur(cursor)->left_block;

		btr_leaf_page_release(prev_block, latch_mode, mtr);
	}

	cursor->latch_mode = latch_mode;
	cursor->old_stored = false;
}

/*********************************************************//**
Moves the persistent cursor to the previous record in the tree. If no records
are left, the cursor stays 'before first in tree'.
@return TRUE if the cursor was not before first in tree */
ibool
btr_pcur_move_to_prev(
/*==================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; NOTE that the
				function may release the page latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	cursor->old_stored = false;

	if (btr_pcur_is_before_first_on_page(cursor)) {

		if (btr_pcur_is_before_first_in_tree(cursor)) {

			return(FALSE);
		}

		btr_pcur_move_backward_from_page(cursor, mtr);

		return(TRUE);
	}

	btr_pcur_move_to_prev_on_page(cursor);

	return(TRUE);
}

/**************************************************************//**
If mode is PAGE_CUR_G or PAGE_CUR_GE, opens a persistent cursor on the first
user record satisfying the search condition, in the case PAGE_CUR_L or
PAGE_CUR_LE, on the last user record. If no such user record exists, then
in the first case sets the cursor after last in tree, and in the latter case
before first in tree. The latching mode must be BTR_SEARCH_LEAF or
BTR_MODIFY_LEAF. */
void
btr_pcur_open_on_user_rec(
	dict_index_t*	index,		/*!< in: index */
	const dtuple_t*	tuple,		/*!< in: tuple on which search done */
	page_cur_mode_t	mode,		/*!< in: PAGE_CUR_L, ... */
	ulint		latch_mode,	/*!< in: BTR_SEARCH_LEAF or
					BTR_MODIFY_LEAF */
	btr_pcur_t*	cursor,		/*!< in: memory buffer for persistent
					cursor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	btr_pcur_open_low(index, 0, tuple, mode, latch_mode, cursor, 0, mtr);

	if ((mode == PAGE_CUR_GE) || (mode == PAGE_CUR_G)) {

		if (btr_pcur_is_after_last_on_page(cursor)) {

			btr_pcur_move_to_next_user_rec(cursor, mtr);
		}
	} else {
		ut_ad((mode == PAGE_CUR_LE) || (mode == PAGE_CUR_L));

		/* Not implemented yet */

		ut_error;
	}
}
