/*****************************************************************************

Copyright (c) 1994, 2014, Oracle and/or its affiliates. All Rights Reserved.
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

/********************************************************************//**
@file include/page0cur.ic
The page cursor

Created 10/4/1994 Heikki Tuuri
*************************************************************************/

#ifdef UNIV_DEBUG
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

/* Gets the record where the cursor is positioned.
@param cur page cursor
@return record */
UNIV_INLINE
rec_t *page_cur_get_rec(const page_cur_t *cur)
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

/***********************************************************//**
Inserts a record next to page cursor. Returns pointer to inserted record if
succeed, i.e., enough space available, NULL otherwise. The cursor stays at
the same logical position, but the physical position may change if it is
pointing to a compressed page that was reorganized.

@return pointer to record if succeed, NULL otherwise */
UNIV_INLINE
rec_t*
page_cur_tuple_insert(
/*==================*/
	page_cur_t*	cursor,	/*!< in/out: a page cursor */
	const dtuple_t*	tuple,	/*!< in: pointer to a data tuple */
	rec_offs**	offsets,/*!< out: offsets on *rec */
	mem_heap_t**	heap,	/*!< in/out: pointer to memory heap, or NULL */
	ulint		n_ext,	/*!< in: number of externally stored columns */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint size = rec_get_converted_size(cursor->index, tuple, n_ext);

	if (!*heap) {
		*heap = mem_heap_create(size
					+ (4 + REC_OFFS_HEADER_SIZE
					   + dtuple_get_n_fields(tuple))
					* sizeof **offsets);
	}

	rec_t* rec = rec_convert_dtuple_to_rec(
		static_cast<byte*>(mem_heap_alloc(*heap, size)),
		cursor->index, tuple, n_ext);

	*offsets = rec_get_offsets(rec, cursor->index, *offsets,
				   page_is_leaf(cursor->block->page.frame)
				   ? cursor->index->n_core_fields : 0,
				   ULINT_UNDEFINED, heap);
	ut_ad(size == rec_offs_size(*offsets));

	if (is_buf_block_get_page_zip(cursor->block)) {
		rec = page_cur_insert_rec_zip(cursor, rec, *offsets, mtr);
	} else {
		rec = page_cur_insert_rec_low(cursor, rec, *offsets, mtr);
	}

	ut_ad(!rec || !cmp_dtuple_rec(tuple, rec, cursor->index, *offsets));
	return(rec);
}

