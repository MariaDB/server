/*****************************************************************************

Copyright (c) 1994, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2021, MariaDB Corporation.

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
@file include/page0cur.ic
The page cursor

Created 10/4/1994 Heikki Tuuri
*************************************************************************/

#ifdef UNIV_DEBUG
/*********************************************************//**
Gets pointer to the page frame where the cursor is positioned.
@return page */
UNIV_INLINE
page_t*
page_cur_get_page(
/*==============*/
	page_cur_t*	cur)	/*!< in: page cursor */
{
  return page_align(page_cur_get_rec(cur));
}

/*********************************************************//**
Gets pointer to the buffer block where the cursor is positioned.
@return page */
UNIV_INLINE
buf_block_t*
page_cur_get_block(
/*===============*/
	page_cur_t*	cur)	/*!< in: page cursor */
{
  ut_ad(cur);
  ut_ad(!cur->rec || page_align(cur->rec) == cur->block->page.frame);
  return cur->block;
}

/*********************************************************//**
Gets pointer to the page frame where the cursor is positioned.
@return page */
UNIV_INLINE
page_zip_des_t*
page_cur_get_page_zip(
/*==================*/
	page_cur_t*	cur)	/*!< in: page cursor */
{
	return(buf_block_get_page_zip(page_cur_get_block(cur)));
}

/*********************************************************//**
Gets the record where the cursor is positioned.
@return record */
UNIV_INLINE
rec_t*
page_cur_get_rec(
/*=============*/
	page_cur_t*	cur)	/*!< in: page cursor */
{
  ut_ad(cur);
  ut_ad(!cur->rec || page_align(cur->rec) == cur->block->page.frame);
  return cur->rec;
}
#endif /* UNIV_DEBUG */

/*********************************************************//**
Sets the cursor object to point before the first user record
on the page. */
UNIV_INLINE
void
page_cur_set_before_first(
/*======================*/
	const buf_block_t*	block,	/*!< in: index page */
	page_cur_t*		cur)	/*!< in: cursor */
{
	cur->block = const_cast<buf_block_t*>(block);
	cur->rec = page_get_infimum_rec(buf_block_get_frame(cur->block));
}

/*********************************************************//**
Sets the cursor object to point after the last user record on
the page. */
UNIV_INLINE
void
page_cur_set_after_last(
/*====================*/
	const buf_block_t*	block,	/*!< in: index page */
	page_cur_t*		cur)	/*!< in: cursor */
{
	cur->block = const_cast<buf_block_t*>(block);
	cur->rec = page_get_supremum_rec(buf_block_get_frame(cur->block));
}

/*********************************************************//**
Returns TRUE if the cursor is before first user record on page.
@return TRUE if at start */
UNIV_INLINE
ibool
page_cur_is_before_first(
/*=====================*/
	const page_cur_t*	cur)	/*!< in: cursor */
{
	ut_ad(cur);
	ut_ad(page_align(cur->rec) == cur->block->page.frame);
	return(page_rec_is_infimum(cur->rec));
}

/*********************************************************//**
Returns TRUE if the cursor is after last user record.
@return TRUE if at end */
UNIV_INLINE
ibool
page_cur_is_after_last(
/*===================*/
	const page_cur_t*	cur)	/*!< in: cursor */
{
	ut_ad(cur);
	ut_ad(page_align(cur->rec) == cur->block->page.frame);
	return(page_rec_is_supremum(cur->rec));
}

/**********************************************************//**
Positions the cursor on the given record. */
UNIV_INLINE
void
page_cur_position(
/*==============*/
	const rec_t*		rec,	/*!< in: record on a page */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	page_cur_t*		cur)	/*!< out: page cursor */
{
	ut_ad(rec && block && cur);
	ut_ad(page_align(rec) == block->page.frame);

	cur->rec = (rec_t*) rec;
	cur->block = (buf_block_t*) block;
}

/**********************************************************//**
Moves the cursor to the next record on page. */
UNIV_INLINE
void
page_cur_move_to_next(
/*==================*/
	page_cur_t*	cur)	/*!< in/out: cursor; must not be after last */
{
	ut_ad(!page_cur_is_after_last(cur));

	cur->rec = page_rec_get_next(cur->rec);
}

/**********************************************************//**
Moves the cursor to the previous record on page. */
UNIV_INLINE
void
page_cur_move_to_prev(
/*==================*/
	page_cur_t*	cur)	/*!< in/out: page cursor, not before first */
{
	ut_ad(!page_cur_is_before_first(cur));

	cur->rec = page_rec_get_prev(cur->rec);
}

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
	page_cur_t*		cursor)
{
	ulint		low_match = 0;
	ulint		up_match = 0;

	ut_ad(dtuple_check_typed(tuple));

	page_cur_search_with_match(block, index, tuple, mode,
				   &up_match, &low_match, cursor, NULL);
	return(low_match);
}

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
	page_cur_t*		cursor)
{
	return(page_cur_search(block, index, tuple, PAGE_CUR_LE, cursor));
}

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
{
	rec_t*		rec;
	ulint		size = rec_get_converted_size(index, tuple, n_ext);

	if (!*heap) {
		*heap = mem_heap_create(size
					+ (4 + REC_OFFS_HEADER_SIZE
					   + dtuple_get_n_fields(tuple))
					* sizeof **offsets);
	}

	rec = rec_convert_dtuple_to_rec((byte*) mem_heap_alloc(*heap, size),
					index, tuple, n_ext);

	*offsets = rec_get_offsets(rec, index, *offsets,
				   page_is_leaf(cursor->block->page.frame)
				   ? index->n_core_fields : 0,
				   ULINT_UNDEFINED, heap);
	ut_ad(size == rec_offs_size(*offsets));

	if (is_buf_block_get_page_zip(cursor->block)) {
		rec = page_cur_insert_rec_zip(
			cursor, index, rec, *offsets, mtr);
	} else {
		rec = page_cur_insert_rec_low(cursor,
					      index, rec, *offsets, mtr);
	}

	ut_ad(!rec || !cmp_dtuple_rec(tuple, rec, index, *offsets));
	return(rec);
}

