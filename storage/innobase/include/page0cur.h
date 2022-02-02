/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2021, MariaDB Corporation.

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
@file include/page0cur.h
The page cursor

Created 10/4/1994 Heikki Tuuri
*************************************************************************/

#ifndef page0cur_h
#define page0cur_h

#include "page0page.h"

#ifdef UNIV_DEBUG
/*********************************************************//**
Gets pointer to the page frame where the cursor is positioned.
@return page */
UNIV_INLINE
page_t*
page_cur_get_page(
/*==============*/
	page_cur_t*	cur);	/*!< in: page cursor */
/*********************************************************//**
Gets pointer to the buffer block where the cursor is positioned.
@return page */
UNIV_INLINE
buf_block_t*
page_cur_get_block(
/*===============*/
	page_cur_t*	cur);	/*!< in: page cursor */
/*********************************************************//**
Gets pointer to the page frame where the cursor is positioned.
@return page */
UNIV_INLINE
page_zip_des_t*
page_cur_get_page_zip(
/*==================*/
	page_cur_t*	cur);	/*!< in: page cursor */
/*********************************************************//**
Gets the record where the cursor is positioned.
@return record */
UNIV_INLINE
rec_t*
page_cur_get_rec(
/*=============*/
	page_cur_t*	cur);	/*!< in: page cursor */
#else /* UNIV_DEBUG */
# define page_cur_get_page(cur)		page_align((cur)->rec)
# define page_cur_get_block(cur)	(cur)->block
# define page_cur_get_page_zip(cur)	buf_block_get_page_zip((cur)->block)
# define page_cur_get_rec(cur)		(cur)->rec
#endif /* UNIV_DEBUG */
# define is_page_cur_get_page_zip(cur)	is_buf_block_get_page_zip((cur)->block)
/*********************************************************//**
Sets the cursor object to point before the first user record
on the page. */
UNIV_INLINE
void
page_cur_set_before_first(
/*======================*/
	const buf_block_t*	block,	/*!< in: index page */
	page_cur_t*		cur);	/*!< in: cursor */
/*********************************************************//**
Sets the cursor object to point after the last user record on
the page. */
UNIV_INLINE
void
page_cur_set_after_last(
/*====================*/
	const buf_block_t*	block,	/*!< in: index page */
	page_cur_t*		cur);	/*!< in: cursor */
/*********************************************************//**
Returns TRUE if the cursor is before first user record on page.
@return TRUE if at start */
UNIV_INLINE
ibool
page_cur_is_before_first(
/*=====================*/
	const page_cur_t*	cur);	/*!< in: cursor */
/*********************************************************//**
Returns TRUE if the cursor is after last user record.
@return TRUE if at end */
UNIV_INLINE
ibool
page_cur_is_after_last(
/*===================*/
	const page_cur_t*	cur);	/*!< in: cursor */
/**********************************************************//**
Positions the cursor on the given record. */
UNIV_INLINE
void
page_cur_position(
/*==============*/
	const rec_t*		rec,	/*!< in: record on a page */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	page_cur_t*		cur);	/*!< out: page cursor */
/**********************************************************//**
Moves the cursor to the next record on page. */
UNIV_INLINE
void
page_cur_move_to_next(
/*==================*/
	page_cur_t*	cur);	/*!< in/out: cursor; must not be after last */
/**********************************************************//**
Moves the cursor to the previous record on page. */
UNIV_INLINE
void
page_cur_move_to_prev(
/*==================*/
	page_cur_t*	cur);	/*!< in/out: cursor; not before first */

/***********************************************************//**
Inserts a record next to page cursor. Returns pointer to inserted record if
succeed, i.e., enough space available, NULL otherwise. The cursor stays at
the same logical position, but the physical position may change if it is
pointing to a compressed page that was reorganized.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to record if succeed, NULL otherwise */
UNIV_INLINE
rec_t*
page_cur_tuple_insert(
/*==================*/
	page_cur_t*	cursor,	/*!< in/out: a page cursor */
	const dtuple_t*	tuple,	/*!< in: pointer to a data tuple */
	dict_index_t*	index,	/*!< in: record descriptor */
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************//**
Inserts a record next to page cursor on an uncompressed page.
Returns pointer to inserted record if succeed, i.e., enough
space available, NULL otherwise. The cursor stays at the same position.
@return pointer to record if succeed, NULL otherwise */
rec_t*
page_cur_insert_rec_low(
/*====================*/
	const page_cur_t*cur,	/*!< in: page cursor */
	dict_index_t*	index,	/*!< in: record descriptor */
	const rec_t*	rec,	/*!< in: record to insert after cur */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/***********************************************************//**
Inserts a record next to page cursor on a compressed and uncompressed
page. Returns pointer to inserted record if succeed, i.e.,
enough space available, NULL otherwise.
The cursor stays at the same position.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to record if succeed, NULL otherwise */
rec_t*
page_cur_insert_rec_zip(
/*====================*/
	page_cur_t*	cursor,	/*!< in/out: page cursor */
	dict_index_t*	index,	/*!< in: record descriptor */
	const rec_t*	rec,	/*!< in: pointer to a physical record */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/***********************************************************//**
Deletes a record at the page cursor. The cursor is moved to the
next record after the deleted one. */
void
page_cur_delete_rec(
/*================*/
	page_cur_t*		cursor,	/*!< in/out: a page cursor */
	const dict_index_t*	index,	/*!< in: record descriptor */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(
					cursor->rec, index) */
	mtr_t*			mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull));

/** Apply a INSERT_HEAP_REDUNDANT or INSERT_REUSE_REDUNDANT record that was
written by page_cur_insert_rec_low() for a ROW_FORMAT=REDUNDANT page.
@param block      B-tree or R-tree page in ROW_FORMAT=COMPACT or DYNAMIC
@param reuse      false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
@param prev       byte offset of the predecessor, relative to PAGE_OLD_INFIMUM
@param enc_hdr    encoded fixed-size header bits
@param hdr_c      number of common record header bytes with prev
@param data_c     number of common data bytes with prev
@param data       literal header and data bytes
@param data_len   length of the literal data, in bytes
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_insert_redundant(const buf_block_t &block, bool reuse,
                                 ulint prev, ulint enc_hdr,
                                 size_t hdr_c, size_t data_c,
                                 const void *data, size_t data_len);

/** Apply a INSERT_HEAP_DYNAMIC or INSERT_REUSE_DYNAMIC record that was
written by page_cur_insert_rec_low() for a ROW_FORMAT=COMPACT or DYNAMIC page.
@param block      B-tree or R-tree page in ROW_FORMAT=COMPACT or DYNAMIC
@param reuse      false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
@param prev       byte offset of the predecessor, relative to PAGE_NEW_INFIMUM
@param shift      unless !reuse: number of bytes the PAGE_FREE is moving
@param enc_hdr_l  number of copied record header bytes, plus record type bits
@param hdr_c      number of common record header bytes with prev
@param data_c     number of common data bytes with prev
@param data       literal header and data bytes
@param data_len   length of the literal data, in bytes
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_insert_dynamic(const buf_block_t &block, bool reuse,
                               ulint prev, ulint shift, ulint enc_hdr_l,
                               size_t hdr_c, size_t data_c,
                               const void *data, size_t data_len);

/** Apply a DELETE_ROW_FORMAT_REDUNDANT record that was written by
page_cur_delete_rec() for a ROW_FORMAT=REDUNDANT page.
@param block    B-tree or R-tree page in ROW_FORMAT=REDUNDANT
@param prev     byte offset of the predecessor, relative to PAGE_OLD_INFIMUM
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_delete_redundant(const buf_block_t &block, ulint prev);

/** Apply a DELETE_ROW_FORMAT_DYNAMIC record that was written by
page_cur_delete_rec() for a ROW_FORMAT=COMPACT or DYNAMIC page.
@param block      B-tree or R-tree page in ROW_FORMAT=COMPACT or DYNAMIC
@param prev       byte offset of the predecessor, relative to PAGE_NEW_INFIMUM
@param hdr_size   record header size, excluding REC_N_NEW_EXTRA_BYTES
@param data_size  data payload size, in bytes
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_delete_dynamic(const buf_block_t &block, ulint prev,
                               size_t hdr_size, size_t data_size);

/** Search the right position for a page cursor.
@param[in] block buffer block
@param[in] index index tree
@param[in] tuple data tuple
@param[in] mode PAGE_CUR_L, PAGE_CUR_LE, PAGE_CUR_G, or PAGE_CUR_GE
@param[out] cursor page cursor
@return number of matched fields on the left */
UNIV_INLINE
ulint
page_cur_search(
	const buf_block_t*	block,
	const dict_index_t*	index,
	const dtuple_t*		tuple,
	page_cur_mode_t		mode,
	page_cur_t*		cursor);

/** Search the right position for a page cursor.
@param[in] block buffer block
@param[in] index index tree
@param[in] tuple data tuple
@param[out] cursor page cursor
@return number of matched fields on the left */
UNIV_INLINE
ulint
page_cur_search(
	const buf_block_t*	block,
	const dict_index_t*	index,
	const dtuple_t*		tuple,
	page_cur_t*		cursor);

/****************************************************************//**
Searches the right position for a page cursor. */
void
page_cur_search_with_match(
/*=======================*/
	const buf_block_t*	block,	/*!< in: buffer block */
	const dict_index_t*	index,	/*!< in: record descriptor */
	const dtuple_t*		tuple,	/*!< in: data tuple */
	page_cur_mode_t		mode,	/*!< in: PAGE_CUR_L,
					PAGE_CUR_LE, PAGE_CUR_G, or
					PAGE_CUR_GE */
	ulint*			iup_matched_fields,
					/*!< in/out: already matched
					fields in upper limit record */
	ulint*			ilow_matched_fields,
					/*!< in/out: already matched
					fields in lower limit record */
	page_cur_t*		cursor,	/*!< out: page cursor */
	rtr_info_t*		rtr_info);/*!< in/out: rtree search stack */
#ifdef BTR_CUR_HASH_ADAPT
/** Search the right position for a page cursor.
@param[in]	block			buffer block
@param[in]	index			index tree
@param[in]	tuple			key to be searched for
@param[in]	mode			search mode
@param[in,out]	iup_matched_fields	already matched fields in the
upper limit record
@param[in,out]	iup_matched_bytes	already matched bytes in the
first partially matched field in the upper limit record
@param[in,out]	ilow_matched_fields	already matched fields in the
lower limit record
@param[in,out]	ilow_matched_bytes	already matched bytes in the
first partially matched field in the lower limit record
@param[out]	cursor			page cursor */
void
page_cur_search_with_match_bytes(
	const buf_block_t*	block,
	const dict_index_t*	index,
	const dtuple_t*		tuple,
	page_cur_mode_t		mode,
	ulint*			iup_matched_fields,
	ulint*			iup_matched_bytes,
	ulint*			ilow_matched_fields,
	ulint*			ilow_matched_bytes,
	page_cur_t*		cursor);
#endif /* BTR_CUR_HASH_ADAPT */
/***********************************************************//**
Positions a page cursor on a randomly chosen user record on a page. If there
are no user records, sets the cursor on the infimum record. */
void
page_cur_open_on_rnd_user_rec(
/*==========================*/
	buf_block_t*	block,	/*!< in: page */
	page_cur_t*	cursor);/*!< out: page cursor */

/** Index page cursor */

struct page_cur_t{
	const dict_index_t*	index;
	rec_t*		rec;	/*!< pointer to a record on page */
	rec_offs*	offsets;
	buf_block_t*	block;	/*!< pointer to the block containing rec */
};

#include "page0cur.inl"

#endif
