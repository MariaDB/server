/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2023, MariaDB Corporation.

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
@file include/btr0pcur.ic
The index tree persistent cursor

Created 2/23/1996 Heikki Tuuri
*******************************************************/


/*********************************************************//**
Gets the rel_pos field for a cursor whose position has been stored.
@return BTR_PCUR_ON, ... */
UNIV_INLINE
ulint
btr_pcur_get_rel_pos(
/*=================*/
	const btr_pcur_t*	cursor)	/*!< in: persistent cursor */
{
	ut_ad(cursor);
	ut_ad(cursor->old_rec);
	ut_ad(cursor->pos_state == BTR_PCUR_WAS_POSITIONED
	      || cursor->pos_state == BTR_PCUR_IS_POSITIONED);

	return(cursor->rel_pos);
}

/**************************************************************//**
Gets the up_match value for a pcur after a search.
@return number of matched fields at the cursor or to the right if
search mode was PAGE_CUR_GE, otherwise undefined */
UNIV_INLINE
ulint
btr_pcur_get_up_match(
/*==================*/
	const btr_pcur_t*	cursor) /*!< in: persistent cursor */
{
	const btr_cur_t*	btr_cursor;

	ut_ad((cursor->pos_state == BTR_PCUR_WAS_POSITIONED)
	      || (cursor->pos_state == BTR_PCUR_IS_POSITIONED));

	btr_cursor = btr_pcur_get_btr_cur(cursor);

	ut_ad(btr_cursor->up_match != uint16_t(~0U));

	return(btr_cursor->up_match);
}

/**************************************************************//**
Gets the low_match value for a pcur after a search.
@return number of matched fields at the cursor or to the right if
search mode was PAGE_CUR_LE, otherwise undefined */
UNIV_INLINE
ulint
btr_pcur_get_low_match(
/*===================*/
	const btr_pcur_t*	cursor) /*!< in: persistent cursor */
{
	const btr_cur_t*	btr_cursor;

	ut_ad((cursor->pos_state == BTR_PCUR_WAS_POSITIONED)
	      || (cursor->pos_state == BTR_PCUR_IS_POSITIONED));

	btr_cursor = btr_pcur_get_btr_cur(cursor);
	ut_ad(btr_cursor->low_match != uint16_t(~0U));

	return(btr_cursor->low_match);
}

/*********************************************************//**
Checks if the persistent cursor is after the last user record on
a page. */
UNIV_INLINE
ibool
btr_pcur_is_after_last_on_page(
/*===========================*/
	const btr_pcur_t*	cursor)	/*!< in: persistent cursor */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	return(page_cur_is_after_last(btr_pcur_get_page_cur(cursor)));
}

/*********************************************************//**
Checks if the persistent cursor is before the first user record on
a page. */
UNIV_INLINE
ibool
btr_pcur_is_before_first_on_page(
/*=============================*/
	const btr_pcur_t*	cursor)	/*!< in: persistent cursor */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	return(page_cur_is_before_first(btr_pcur_get_page_cur(cursor)));
}

/*********************************************************//**
Checks if the persistent cursor is on a user record. */
UNIV_INLINE
ibool
btr_pcur_is_on_user_rec(
/*====================*/
	const btr_pcur_t*	cursor)	/*!< in: persistent cursor */
{
  return !btr_pcur_is_before_first_on_page(cursor) &&
    !btr_pcur_is_after_last_on_page(cursor);
}

/*********************************************************//**
Checks if the persistent cursor is before the first user record in
the index tree. */
static inline bool btr_pcur_is_before_first_in_tree(btr_pcur_t* cursor)
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	return !page_has_prev(btr_pcur_get_page(cursor))
		&& page_cur_is_before_first(btr_pcur_get_page_cur(cursor));
}

/*********************************************************//**
Checks if the persistent cursor is after the last user record in
the index tree. */
static inline bool btr_pcur_is_after_last_in_tree(btr_pcur_t* cursor)
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	return !page_has_next(btr_pcur_get_page(cursor))
		&& page_cur_is_after_last(btr_pcur_get_page_cur(cursor));
}

/*********************************************************//**
Moves the persistent cursor to the next record on the same page. */
UNIV_INLINE
rec_t*
btr_pcur_move_to_next_on_page(
/*==========================*/
	btr_pcur_t*	cursor)	/*!< in/out: persistent cursor */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	cursor->old_rec = nullptr;
	return page_cur_move_to_next(btr_pcur_get_page_cur(cursor));
}

/*********************************************************//**
Moves the persistent cursor to the previous record on the same page. */
UNIV_INLINE
rec_t*
btr_pcur_move_to_prev_on_page(
/*==========================*/
	btr_pcur_t*	cursor)	/*!< in/out: persistent cursor */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
	cursor->old_rec = nullptr;

	return page_cur_move_to_prev(btr_pcur_get_page_cur(cursor));
}

/*********************************************************//**
Moves the persistent cursor to the next user record in the tree. If no user
records are left, the cursor ends up 'after last in tree'.
@return TRUE if the cursor moved forward, ending on a user record */
UNIV_INLINE
ibool
btr_pcur_move_to_next_user_rec(
/*===========================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; NOTE that the
				function may release the page latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
	cursor->old_rec = nullptr;
loop:
	if (btr_pcur_is_after_last_on_page(cursor)) {
		if (btr_pcur_is_after_last_in_tree(cursor)
		    || btr_pcur_move_to_next_page(cursor, mtr) != DB_SUCCESS) {
			return(FALSE);
		}
	} else if (UNIV_UNLIKELY(!btr_pcur_move_to_next_on_page(cursor))) {
		return false;
	}

	if (btr_pcur_is_on_user_rec(cursor)) {

		return(TRUE);
	}

	goto loop;
}

/*********************************************************//**
Moves the persistent cursor to the next record in the tree. If no records are
left, the cursor stays 'after last in tree'.
@return TRUE if the cursor was not after last in tree */
UNIV_INLINE
ibool
btr_pcur_move_to_next(
/*==================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; NOTE that the
				function may release the page latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
  ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
  ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

  cursor->old_rec= nullptr;

  if (btr_pcur_is_after_last_on_page(cursor))
    return !btr_pcur_is_after_last_in_tree(cursor) &&
      btr_pcur_move_to_next_page(cursor, mtr) == DB_SUCCESS;
  else
    return !!btr_pcur_move_to_next_on_page(cursor);
}

/**************************************************************//**
Commits the mtr and sets the pcur latch mode to BTR_NO_LATCHES,
that is, the cursor becomes detached.
Function btr_pcur_store_position should be used before calling this,
if restoration of cursor is wanted later. */
UNIV_INLINE
void
btr_pcur_commit_specify_mtr(
/*========================*/
	btr_pcur_t*	pcur,	/*!< in: persistent cursor */
	mtr_t*		mtr)	/*!< in: mtr to commit */
{
	ut_ad(pcur->pos_state == BTR_PCUR_IS_POSITIONED);

	pcur->latch_mode = BTR_NO_LATCHES;

	mtr_commit(mtr);

	pcur->pos_state = BTR_PCUR_WAS_POSITIONED;
}

/** Commits the mtr and sets the clustered index pcur and secondary index
pcur latch mode to BTR_NO_LATCHES, that is, the cursor becomes detached.
Function btr_pcur_store_position should be used for both cursor before
calling this, if restoration of cursor is wanted later.
@param[in]	pcur		persistent cursor
@param[in]	sec_pcur	secondary index persistent cursor
@param[in]	mtr		mtr to commit */
UNIV_INLINE
void
btr_pcurs_commit_specify_mtr(
	btr_pcur_t*	pcur,
	btr_pcur_t*	sec_pcur,
	mtr_t*		mtr)
{
	ut_ad(pcur->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(sec_pcur->pos_state == BTR_PCUR_IS_POSITIONED);

	pcur->latch_mode = BTR_NO_LATCHES;
	sec_pcur->latch_mode = BTR_NO_LATCHES;

	mtr_commit(mtr);

	pcur->pos_state = BTR_PCUR_WAS_POSITIONED;
	sec_pcur->pos_state = BTR_PCUR_WAS_POSITIONED;
}

/**************************************************************//**
Sets the old_rec_buf field to NULL. */
UNIV_INLINE
void
btr_pcur_init(
/*==========*/
	btr_pcur_t*	pcur)	/*!< in: persistent cursor */
{
	pcur->old_rec_buf = NULL;
	pcur->old_rec = NULL;

	pcur->btr_cur.rtr_info = NULL;
}

/** Opens an persistent cursor to an index tree without initializing the
cursor.
@param tuple      tuple on which search done
@param mode       search mode; NOTE that if the search is made using a
                  unique prefix of a record, mode should be PAGE_CUR_LE, not
                  PAGE_CUR_GE, as the latter may end up on the previous page of
                  the record!
@param latch_mode BTR_SEARCH_LEAF, ...
@param cursor     memory buffer for persistent cursor
@param mtr        mini-transaction
@return DB_SUCCESS on success or error code otherwise. */
inline
dberr_t btr_pcur_open_with_no_init(const dtuple_t *tuple, page_cur_mode_t mode,
                                   btr_latch_mode latch_mode,
                                   btr_pcur_t *cursor, mtr_t *mtr)
{
  cursor->latch_mode= BTR_LATCH_MODE_WITHOUT_INTENTION(latch_mode);
  cursor->search_mode= mode;
  cursor->pos_state= BTR_PCUR_IS_POSITIONED;
  cursor->trx_if_known= nullptr;
  return cursor->btr_cur.search_leaf(tuple, mode, latch_mode, mtr);
}

/**************************************************************//**
Frees the possible memory heap of a persistent cursor and sets the latch
mode of the persistent cursor to BTR_NO_LATCHES.
WARNING: this function does not release the latch on the page where the
cursor is currently positioned. The latch is acquired by the
"move to next/previous" family of functions. Since recursive shared locks
are not allowed, you must take care (if using the cursor in S-mode) to
manually release the latch by either calling
btr_leaf_page_release(btr_pcur_get_block(&pcur), pcur.latch_mode, mtr)
or by mtr_t::commit(). */
UNIV_INLINE
void
btr_pcur_close(
/*===========*/
	btr_pcur_t*	cursor)	/*!< in: persistent cursor */
{
  ut_free(cursor->old_rec_buf);

  if (cursor->btr_cur.rtr_info)
    rtr_clean_rtr_info(cursor->btr_cur.rtr_info, true);

  cursor->btr_cur.rtr_info= nullptr;
  cursor->old_rec = nullptr;
  cursor->old_rec_buf = nullptr;
  cursor->btr_cur.page_cur.rec = nullptr;
  cursor->btr_cur.page_cur.block = nullptr;

  cursor->latch_mode = BTR_NO_LATCHES;
  cursor->pos_state = BTR_PCUR_NOT_POSITIONED;

  cursor->trx_if_known = nullptr;
}

/*********************************************************//**
Moves the persistent cursor to the infimum record on the same page. */
UNIV_INLINE
void
btr_pcur_move_before_first_on_page(
/*===============================*/
	btr_pcur_t*	cursor) /*!< in/out: persistent cursor */
{
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	page_cur_set_before_first(btr_pcur_get_block(cursor),
		btr_pcur_get_page_cur(cursor));

	cursor->old_rec = nullptr;
}
