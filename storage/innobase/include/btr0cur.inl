/*****************************************************************************

Copyright (c) 1994, 2015, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************//**
@file include/btr0cur.ic
The index tree cursor

Created 10/16/1994 Heikki Tuuri
*******************************************************/

#include "btr0btr.h"

#ifdef UNIV_DEBUG
# define LIMIT_OPTIMISTIC_INSERT_DEBUG(NREC, CODE)\
if (btr_cur_limit_optimistic_insert_debug > 1\
    && (NREC) >= btr_cur_limit_optimistic_insert_debug) {\
        CODE;\
}
#else
# define LIMIT_OPTIMISTIC_INSERT_DEBUG(NREC, CODE)
#endif /* UNIV_DEBUG */

/*********************************************************//**
Returns the compressed page on which the tree cursor is positioned.
@return pointer to compressed page, or NULL if the page is not compressed */
UNIV_INLINE
page_zip_des_t*
btr_cur_get_page_zip(
/*=================*/
	btr_cur_t*	cursor)	/*!< in: tree cursor */
{
	return(buf_block_get_page_zip(btr_cur_get_block(cursor)));
}

/*********************************************************//**
Returns the page of a tree cursor.
@return pointer to page */
UNIV_INLINE
page_t*
btr_cur_get_page(
/*=============*/
	btr_cur_t*	cursor)	/*!< in: tree cursor */
{
	return(page_align(page_cur_get_rec(&(cursor->page_cur))));
}

/*********************************************************//**
Positions a tree cursor at a given record. */
UNIV_INLINE
void
btr_cur_position(
/*=============*/
	dict_index_t*	index,	/*!< in: index */
	rec_t*		rec,	/*!< in: record in tree */
	buf_block_t*	block,	/*!< in: buffer block of rec */
	btr_cur_t*	cursor)	/*!< out: cursor */
{
	page_cur_position(rec, block, btr_cur_get_page_cur(cursor));
	cursor->index = index;
}

/*********************************************************************//**
Checks if compressing an index page where a btr cursor is placed makes
sense.
@return TRUE if compression is recommended */
UNIV_INLINE
ibool
btr_cur_compress_recommendation(
/*============================*/
	btr_cur_t*	cursor,	/*!< in: btr cursor */
	mtr_t*		mtr)	/*!< in: mtr */
{
	const page_t*	page;

	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(cursor),
					 MTR_MEMO_PAGE_X_FIX));

	page = btr_cur_get_page(cursor);

	LIMIT_OPTIMISTIC_INSERT_DEBUG(page_get_n_recs(page) * 2U,
				      return(FALSE));

	if (!page_has_siblings(page)
	    || page_get_data_size(page)
	    < BTR_CUR_PAGE_COMPRESS_LIMIT(cursor->index)) {

		/* The page fillfactor has dropped below a predefined
		minimum value OR the level in the B-tree contains just
		one page: we recommend compression if this is not the
		root page. */

		return cursor->index->page
			!= btr_cur_get_block(cursor)->page.id().page_no();
	}

	return(FALSE);
}

/*********************************************************************//**
Checks if the record on which the cursor is placed can be deleted without
making tree compression necessary (or, recommended).
@return TRUE if can be deleted without recommended compression */
UNIV_INLINE
ibool
btr_cur_can_delete_without_compress(
/*================================*/
	btr_cur_t*	cursor,	/*!< in: btr cursor */
	ulint		rec_size,/*!< in: rec_get_size(btr_cur_get_rec(cursor))*/
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_t*		page;

	ut_ad(mtr->memo_contains_flagged(btr_cur_get_block(cursor),
					 MTR_MEMO_PAGE_X_FIX));

	page = btr_cur_get_page(cursor);

	if (!page_has_siblings(page) || page_get_n_recs(page) < 2
	    || page_get_data_size(page) - rec_size
	    < BTR_CUR_PAGE_COMPRESS_LIMIT(cursor->index)) {

		/* The page fillfactor will drop below a predefined
		minimum value, OR the level in the B-tree contains just
		one page, OR the page will become empty: we recommend
		compression if this is not the root page. */

		return cursor->index->page
			== btr_cur_get_block(cursor)->page.id().page_no();
	}

	return(TRUE);
}

/*******************************************************************//**
Determine if an operation on off-page columns is an update.
@return TRUE if op != BTR_STORE_INSERT */
UNIV_INLINE
ibool
btr_blob_op_is_update(
/*==================*/
	enum blob_op	op)	/*!< in: operation */
{
	switch (op) {
	case BTR_STORE_INSERT:
	case BTR_STORE_INSERT_BULK:
		return(FALSE);
	case BTR_STORE_INSERT_UPDATE:
	case BTR_STORE_UPDATE:
		return(TRUE);
	}

	ut_ad(0);
	return(FALSE);
}
