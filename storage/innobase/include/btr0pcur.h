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

/**************************************************//**
@file include/btr0pcur.h
The index tree persistent cursor

Created 2/23/1996 Heikki Tuuri
*******************************************************/

#pragma once

#include "dict0dict.h"
#include "btr0cur.h"
#include "btr0btr.h"
#include "gis0rtree.h"

/* Relative positions for a stored cursor position */
enum btr_pcur_pos_t {
	BTR_PCUR_ON		= 1,
	BTR_PCUR_BEFORE		= 2,
	BTR_PCUR_AFTER		= 3,
/* Note that if the tree is not empty, btr_pcur_store_position does not
use the following, but only uses the above three alternatives, where the
position is stored relative to a specific record: this makes implementation
of a scroll cursor easier */
	BTR_PCUR_BEFORE_FIRST_IN_TREE	= 4,	/* in an empty tree */
	BTR_PCUR_AFTER_LAST_IN_TREE	= 5	/* in an empty tree */
};

/**************************************************************//**
Resets a persistent cursor object, freeing ::old_rec_buf if it is
allocated and resetting the other members to their initial values. */
void
btr_pcur_reset(
/*===========*/
	btr_pcur_t*	cursor);/*!< in, out: persistent cursor */

/**************************************************************//**
Copies the stored position of a pcur to another pcur. */
void
btr_pcur_copy_stored_position(
/*==========================*/
	btr_pcur_t*	pcur_receive,	/*!< in: pcur which will receive the
					position info */
	btr_pcur_t*	pcur_donate);	/*!< in: pcur from which the info is
					copied */
/**************************************************************//**
Sets the old_rec_buf field to NULL. */
UNIV_INLINE
void
btr_pcur_init(
/*==========*/
	btr_pcur_t*	pcur);	/*!< in: persistent cursor */

/** Opens an persistent cursor to an index tree without initializing the
cursor.
@param tuple      tuple on which search done
@param mode       PAGE_CUR_L, ...; NOTE that if the search is made using a
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
                                   btr_pcur_t *cursor, mtr_t *mtr);

/**************************************************************//**
Gets the up_match value for a pcur after a search.
@return number of matched fields at the cursor or to the right if
search mode was PAGE_CUR_GE, otherwise undefined */
UNIV_INLINE
ulint
btr_pcur_get_up_match(
/*==================*/
	const btr_pcur_t*	cursor); /*!< in: persistent cursor */
/**************************************************************//**
Gets the low_match value for a pcur after a search.
@return number of matched fields at the cursor or to the right if
search mode was PAGE_CUR_LE, otherwise undefined */
UNIV_INLINE
ulint
btr_pcur_get_low_match(
/*===================*/
	const btr_pcur_t*	cursor); /*!< in: persistent cursor */

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
	btr_pcur_t*	cursor);	/*!< in: persistent cursor */
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
	mtr_t*		mtr);	/*!< in: mtr */
/*********************************************************//**
Gets the rel_pos field for a cursor whose position has been stored.
@return BTR_PCUR_ON, ... */
UNIV_INLINE
ulint
btr_pcur_get_rel_pos(
/*=================*/
	const btr_pcur_t*	cursor);/*!< in: persistent cursor */
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
	mtr_t*		mtr);	/*!< in: mtr to commit */

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
	mtr_t*		mtr);

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
	mtr_t*		mtr);	/*!< in: mtr */
/*********************************************************//**
Moves the persistent cursor to the previous record in the tree. If no records
are left, the cursor stays 'before first in tree'.
@return true if the cursor was not before first in tree */
bool
btr_pcur_move_to_prev(
/*==================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; NOTE that the
				function may release the page latch */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
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
	mtr_t*		mtr);	/*!< in: mtr */
/*********************************************************//**
Moves the persistent cursor to the first record on the next page.
Releases the latch on the current page, and bufferunfixes it.
Note that there must not be modifications on the current page,
as then the x-latch can be released only in mtr_commit. */
dberr_t
btr_pcur_move_to_next_page(
/*=======================*/
	btr_pcur_t*	cursor,	/*!< in: persistent cursor; must be on the
				last record of the current page */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

#define btr_pcur_get_btr_cur(cursor) (&(cursor)->btr_cur)
#define btr_pcur_get_page_cur(cursor) (&(cursor)->btr_cur.page_cur)
#define btr_pcur_get_page(cursor) btr_pcur_get_block(cursor)->page.frame

/*********************************************************//**
Checks if the persistent cursor is on a user record. */
UNIV_INLINE
ibool
btr_pcur_is_on_user_rec(
/*====================*/
	const btr_pcur_t*	cursor);/*!< in: persistent cursor */
/*********************************************************//**
Checks if the persistent cursor is after the last user record on
a page. */
UNIV_INLINE
ibool
btr_pcur_is_after_last_on_page(
/*===========================*/
	const btr_pcur_t*	cursor);/*!< in: persistent cursor */
/*********************************************************//**
Checks if the persistent cursor is before the first user record on
a page. */
UNIV_INLINE
ibool
btr_pcur_is_before_first_on_page(
/*=============================*/
	const btr_pcur_t*	cursor);/*!< in: persistent cursor */
/*********************************************************//**
Checks if the persistent cursor is before the first user record in
the index tree. */
static inline bool btr_pcur_is_before_first_in_tree(btr_pcur_t* cursor);
/*********************************************************//**
Checks if the persistent cursor is after the last user record in
the index tree. */
static inline bool btr_pcur_is_after_last_in_tree(btr_pcur_t* cursor);
MY_ATTRIBUTE((nonnull, warn_unused_result))
/*********************************************************//**
Moves the persistent cursor to the next record on the same page. */
UNIV_INLINE
rec_t*
btr_pcur_move_to_next_on_page(
/*==========================*/
	btr_pcur_t*	cursor);/*!< in/out: persistent cursor */
MY_ATTRIBUTE((nonnull, warn_unused_result))
/*********************************************************//**
Moves the persistent cursor to the previous record on the same page. */
UNIV_INLINE
rec_t*
btr_pcur_move_to_prev_on_page(
/*==========================*/
	btr_pcur_t*	cursor);/*!< in/out: persistent cursor */
/*********************************************************//**
Moves the persistent cursor to the infimum record on the same page. */
UNIV_INLINE
void
btr_pcur_move_before_first_on_page(
/*===============================*/
	btr_pcur_t*	cursor); /*!< in/out: persistent cursor */

/** Position state of persistent B-tree cursor. */
enum pcur_pos_t {
	/** The persistent cursor is not positioned. */
	BTR_PCUR_NOT_POSITIONED = 0,
	/** The persistent cursor was previously positioned.
	TODO: currently, the state can be BTR_PCUR_IS_POSITIONED,
	though it really should be BTR_PCUR_WAS_POSITIONED,
	because we have no obligation to commit the cursor with
	mtr; similarly latch_mode may be out of date. This can
	lead to problems if btr_pcur is not used the right way;
	all current code should be ok. */
	BTR_PCUR_WAS_POSITIONED,
	/** The persistent cursor is positioned by optimistic get to the same
	record as it was positioned at. Not used for rel_pos == BTR_PCUR_ON.
	It may need adjustment depending on previous/current search direction
	and rel_pos. */
	BTR_PCUR_IS_POSITIONED_OPTIMISTIC,
	/** The persistent cursor is positioned by index search.
	Or optimistic get for rel_pos == BTR_PCUR_ON. */
	BTR_PCUR_IS_POSITIONED
};

/* The persistent B-tree cursor structure. This is used mainly for SQL
selects, updates, and deletes. */

struct btr_pcur_t
{
  /** Return value of restore_position() */
  enum restore_status {
    /** cursor position on user rec and points on the record with
    the same field values as in the stored record */
    SAME_ALL,
    /** cursor position is on user rec and points on the record with
    the same unique field values as in the stored record */
    SAME_UNIQ,
    /** cursor position is not on user rec or points on the record
    with not the same uniq field values as in the stored record */
    NOT_SAME,
    /** the index tree is corrupted */
    CORRUPTED
  };
  /** a B-tree cursor */
  btr_cur_t btr_cur;
  /** @see BTR_PCUR_WAS_POSITIONED
  BTR_SEARCH_LEAF, BTR_MODIFY_LEAF, BTR_MODIFY_TREE or BTR_NO_LATCHES,
  depending on the latching state of the page and tree where the cursor
  is positioned; BTR_NO_LATCHES means that the cursor is not currently
  positioned:
  we say then that the cursor is detached; it can be restored to
  attached if the old position was stored in old_rec */
  btr_latch_mode latch_mode= BTR_NO_LATCHES;
  /** if cursor position is stored, contains an initial segment of the
  latest record cursor was positioned either on, before or after */
  rec_t *old_rec= nullptr;
  /** btr_cur.index()->n_core_fields when old_rec was copied */
  uint16 old_n_core_fields= 0;
  /** number of fields in old_rec */
  uint16 old_n_fields= 0;
  /** BTR_PCUR_ON, BTR_PCUR_BEFORE, or BTR_PCUR_AFTER, depending on
  whether cursor was on, before, or after the old_rec record */
  btr_pcur_pos_t rel_pos= btr_pcur_pos_t(0);
  /** the page identifier of old_rec */
  page_id_t old_page_id{0,0};
  /** the modify clock value of the buffer block when the cursor position
  was stored */
  ib_uint64_t modify_clock= 0;
  /** btr_pcur_store_position() and restore_position() state. */
  enum pcur_pos_t pos_state= BTR_PCUR_NOT_POSITIONED;
  page_cur_mode_t search_mode= PAGE_CUR_UNSUPP;
  /** the transaction, if we know it; otherwise this field is not defined;
  can ONLY BE USED in error prints in fatal assertion failures! */
  trx_t *trx_if_known= nullptr;
  /** a dynamically allocated buffer for old_rec */
  byte *old_rec_buf= nullptr;
  /** old_rec_buf size if old_rec_buf is not NULL */
  ulint buf_size= 0;

  /** Return the index of this persistent cursor */
  dict_index_t *index() const { return(btr_cur.index()); }
  MY_ATTRIBUTE((nonnull, warn_unused_result))
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
  @param latch_mode  BTR_SEARCH_LEAF, ...
  @param mtr         mini-transaction
  @retval SAME_ALL cursor position on user rec and points on
  the record with the same field values as in the stored record,
  @retval SAME_UNIQ cursor position is on user rec and points on the
  record with the same unique field values as in the stored record,
  @retval NOT_SAME cursor position is not on user rec or points on
  the record with not the same uniq field values as in the stored
  @retval CORRUPTED if the index is corrupted */
  restore_status restore_position(btr_latch_mode latch_mode, mtr_t *mtr)
    noexcept;
private:
  restore_status restore_pessimistic(btr_latch_mode latch_mode, mtr_t *mtr)
    noexcept;
  bool restore_optimistic_prev(rw_lock_type_t mode, mtr_t *mtr,
                               buf_block_t *block) noexcept;
public:
  restore_status restore_optimistic(btr_latch_mode latch_mode, mtr_t *mtr)
    noexcept;
  /** Open the cursor on the first or last record.
  @param first         true=first record, false=last record
  @param index         B-tree
  @param latch_mode    which latches to acquire
  @param mtr           mini-transaction
  @return error code */
  dberr_t open_leaf(bool first, dict_index_t *index, btr_latch_mode latch_mode,
                    mtr_t *mtr)

  {
    this->latch_mode= BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);
    search_mode= first ? PAGE_CUR_G : PAGE_CUR_L;
    pos_state= BTR_PCUR_IS_POSITIONED;
    old_rec= nullptr;

    return btr_cur.open_leaf(first, index, this->latch_mode, mtr);
  }
};

inline buf_block_t *btr_pcur_get_block(btr_pcur_t *cursor)
{
  ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
  return cursor->btr_cur.page_cur.block;
}

inline const buf_block_t *btr_pcur_get_block(const btr_pcur_t *cursor)
{
  ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
  return cursor->btr_cur.page_cur.block;
}

inline rec_t *btr_pcur_get_rec(const btr_pcur_t *cursor)
{
  ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
  ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
  return cursor->btr_cur.page_cur.rec;
}

/**************************************************************//**
Initializes and opens a persistent cursor to an index tree. */
inline
dberr_t
btr_pcur_open(
	const dtuple_t*	tuple,	/*!< in: tuple on which search done */
        page_cur_mode_t	mode,	/*!< in: PAGE_CUR_LE, ... */
	btr_latch_mode	latch_mode,/*!< in: BTR_SEARCH_LEAF, ... */
	btr_pcur_t*	cursor, /*!< in: memory buffer for persistent cursor */
	mtr_t*		mtr)	/*!< in: mtr */
{
  cursor->latch_mode= BTR_LATCH_MODE_WITHOUT_FLAGS(latch_mode);
  cursor->search_mode= mode;
  cursor->pos_state= BTR_PCUR_IS_POSITIONED;
  cursor->trx_if_known= nullptr;
  return cursor->btr_cur.search_leaf(tuple, mode, latch_mode, mtr);
}

/** Open a cursor on the first user record satisfying the search condition;
in case of no match, after the last index record.
@return DB_SUCCESS or error code */
MY_ATTRIBUTE((nonnull, warn_unused_result))
inline
dberr_t
btr_pcur_open_on_user_rec(
	const dtuple_t*	tuple,		/*!< in: tuple on which search done */
	btr_latch_mode	latch_mode,	/*!< in: BTR_SEARCH_LEAF or
					BTR_MODIFY_LEAF */
	btr_pcur_t*	cursor,		/*!< in: memory buffer for persistent
					cursor */
	mtr_t*		mtr)		/*!< in: mtr */
{
  ut_ad(latch_mode == BTR_SEARCH_LEAF || latch_mode == BTR_MODIFY_LEAF);
  if (dberr_t err=
      btr_pcur_open(tuple, PAGE_CUR_GE, latch_mode, cursor, mtr))
    return err;
  if (!btr_pcur_is_after_last_on_page(cursor) ||
      btr_pcur_is_after_last_in_tree(cursor))
    return DB_SUCCESS;
  if (dberr_t err= btr_pcur_move_to_next_page(cursor, mtr))
    return err;
  return btr_pcur_move_to_next_on_page(cursor) ? DB_SUCCESS : DB_CORRUPTION;
}

#include "btr0pcur.inl"
