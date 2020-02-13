/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2018, 2020, MariaDB Corporation.

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
@file page/page0cur.cc
The page cursor

Created 10/4/1994 Heikki Tuuri
*************************************************************************/

#include "page0cur.h"
#include "page0zip.h"
#include "btr0btr.h"
#include "mtr0log.h"
#include "log0recv.h"
#include "rem0cmp.h"
#include "gis0rtree.h"

#include <algorithm>

#ifdef BTR_CUR_HASH_ADAPT
# ifdef UNIV_SEARCH_PERF_STAT
static ulint	page_cur_short_succ;
# endif /* UNIV_SEARCH_PERF_STAT */

/** Try a search shortcut based on the last insert.
@param[in]	block			index page
@param[in]	index			index tree
@param[in]	tuple			search key
@param[in,out]	iup_matched_fields	already matched fields in the
upper limit record
@param[in,out]	ilow_matched_fields	already matched fields in the
lower limit record
@param[out]	cursor			page cursor
@return true on success */
UNIV_INLINE
bool
page_cur_try_search_shortcut(
	const buf_block_t*	block,
	const dict_index_t*	index,
	const dtuple_t*		tuple,
	ulint*			iup_matched_fields,
	ulint*			ilow_matched_fields,
	page_cur_t*		cursor)
{
	const rec_t*	rec;
	const rec_t*	next_rec;
	ulint		low_match;
	ulint		up_match;
	ibool		success		= FALSE;
	const page_t*	page		= buf_block_get_frame(block);
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(dtuple_check_typed(tuple));
	ut_ad(page_is_leaf(page));

	rec = page_header_get_ptr(page, PAGE_LAST_INSERT);
	offsets = rec_get_offsets(rec, index, offsets, true,
				  dtuple_get_n_fields(tuple), &heap);

	ut_ad(rec);
	ut_ad(page_rec_is_user_rec(rec));

	low_match = up_match = std::min(*ilow_matched_fields,
					*iup_matched_fields);

	if (cmp_dtuple_rec_with_match(tuple, rec, offsets, &low_match) < 0) {
		goto exit_func;
	}

	next_rec = page_rec_get_next_const(rec);
	if (!page_rec_is_supremum(next_rec)) {
		offsets = rec_get_offsets(next_rec, index, offsets, true,
					  dtuple_get_n_fields(tuple), &heap);

		if (cmp_dtuple_rec_with_match(tuple, next_rec, offsets,
					      &up_match) >= 0) {
			goto exit_func;
		}

		*iup_matched_fields = up_match;
	}

	page_cur_position(rec, block, cursor);

	*ilow_matched_fields = low_match;

#ifdef UNIV_SEARCH_PERF_STAT
	page_cur_short_succ++;
#endif
	success = TRUE;
exit_func:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(success);
}

/** Try a search shortcut based on the last insert.
@param[in]	block			index page
@param[in]	index			index tree
@param[in]	tuple			search key
@param[in,out]	iup_matched_fields	already matched fields in the
upper limit record
@param[in,out]	iup_matched_bytes	already matched bytes in the
first partially matched field in the upper limit record
@param[in,out]	ilow_matched_fields	already matched fields in the
lower limit record
@param[in,out]	ilow_matched_bytes	already matched bytes in the
first partially matched field in the lower limit record
@param[out]	cursor			page cursor
@return true on success */
UNIV_INLINE
bool
page_cur_try_search_shortcut_bytes(
	const buf_block_t*	block,
	const dict_index_t*	index,
	const dtuple_t*		tuple,
	ulint*			iup_matched_fields,
	ulint*			iup_matched_bytes,
	ulint*			ilow_matched_fields,
	ulint*			ilow_matched_bytes,
	page_cur_t*		cursor)
{
	const rec_t*	rec;
	const rec_t*	next_rec;
	ulint		low_match;
	ulint		low_bytes;
	ulint		up_match;
	ulint		up_bytes;
	ibool		success		= FALSE;
	const page_t*	page		= buf_block_get_frame(block);
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(dtuple_check_typed(tuple));
	ut_ad(page_is_leaf(page));

	rec = page_header_get_ptr(page, PAGE_LAST_INSERT);
	offsets = rec_get_offsets(rec, index, offsets, true,
				  dtuple_get_n_fields(tuple), &heap);

	ut_ad(rec);
	ut_ad(page_rec_is_user_rec(rec));
	if (ut_pair_cmp(*ilow_matched_fields, *ilow_matched_bytes,
			*iup_matched_fields, *iup_matched_bytes) < 0) {
		up_match = low_match = *ilow_matched_fields;
		up_bytes = low_bytes = *ilow_matched_bytes;
	} else {
		up_match = low_match = *iup_matched_fields;
		up_bytes = low_bytes = *iup_matched_bytes;
	}

	if (cmp_dtuple_rec_with_match_bytes(
		    tuple, rec, index, offsets, &low_match, &low_bytes) < 0) {
		goto exit_func;
	}

	next_rec = page_rec_get_next_const(rec);
	if (!page_rec_is_supremum(next_rec)) {
		offsets = rec_get_offsets(next_rec, index, offsets, true,
					  dtuple_get_n_fields(tuple), &heap);

		if (cmp_dtuple_rec_with_match_bytes(
			    tuple, next_rec, index, offsets,
			    &up_match, &up_bytes)
		    >= 0) {
			goto exit_func;
		}

		*iup_matched_fields = up_match;
		*iup_matched_bytes = up_bytes;
	}

	page_cur_position(rec, block, cursor);

	*ilow_matched_fields = low_match;
	*ilow_matched_bytes = low_bytes;

#ifdef UNIV_SEARCH_PERF_STAT
	page_cur_short_succ++;
#endif
	success = TRUE;
exit_func:
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
	return(success);
}
#endif /* BTR_CUR_HASH_ADAPT */

#ifdef PAGE_CUR_LE_OR_EXTENDS
/****************************************************************//**
Checks if the nth field in a record is a character type field which extends
the nth field in tuple, i.e., the field is longer or equal in length and has
common first characters.
@return TRUE if rec field extends tuple field */
static
ibool
page_cur_rec_field_extends(
/*=======================*/
	const dtuple_t*	tuple,	/*!< in: data tuple */
	const rec_t*	rec,	/*!< in: record */
	const offset_t*	offsets,/*!< in: array returned by rec_get_offsets() */
	ulint		n)	/*!< in: compare nth field */
{
	const dtype_t*	type;
	const dfield_t*	dfield;
	const byte*	rec_f;
	ulint		rec_f_len;

	ut_ad(rec_offs_validate(rec, NULL, offsets));
	dfield = dtuple_get_nth_field(tuple, n);

	type = dfield_get_type(dfield);

	rec_f = rec_get_nth_field(rec, offsets, n, &rec_f_len);

	if (type->mtype == DATA_VARCHAR
	    || type->mtype == DATA_CHAR
	    || type->mtype == DATA_FIXBINARY
	    || type->mtype == DATA_BINARY
	    || type->mtype == DATA_BLOB
	    || DATA_GEOMETRY_MTYPE(type->mtype)
	    || type->mtype == DATA_VARMYSQL
	    || type->mtype == DATA_MYSQL) {

		if (dfield_get_len(dfield) != UNIV_SQL_NULL
		    && rec_f_len != UNIV_SQL_NULL
		    && rec_f_len >= dfield_get_len(dfield)
		    && !cmp_data_data(type->mtype, type->prtype,
				      dfield_get_data(dfield),
				      dfield_get_len(dfield),
				      rec_f, dfield_get_len(dfield))) {

			return(TRUE);
		}
	}

	return(FALSE);
}
#endif /* PAGE_CUR_LE_OR_EXTENDS */

/****************************************************************//**
Searches the right position for a page cursor. */
void
page_cur_search_with_match(
/*=======================*/
	const buf_block_t*	block,	/*!< in: buffer block */
	const dict_index_t*	index,	/*!< in/out: record descriptor */
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
	rtr_info_t*		rtr_info)/*!< in/out: rtree search stack */
{
	ulint		up;
	ulint		low;
	ulint		mid;
	const page_t*	page;
	const page_dir_slot_t* slot;
	const rec_t*	up_rec;
	const rec_t*	low_rec;
	const rec_t*	mid_rec;
	ulint		up_matched_fields;
	ulint		low_matched_fields;
	ulint		cur_matched_fields;
	int		cmp;
#ifdef UNIV_ZIP_DEBUG
	const page_zip_des_t*	page_zip = buf_block_get_page_zip(block);
#endif /* UNIV_ZIP_DEBUG */
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(dtuple_validate(tuple));
#ifdef UNIV_DEBUG
# ifdef PAGE_CUR_DBG
	if (mode != PAGE_CUR_DBG)
# endif /* PAGE_CUR_DBG */
# ifdef PAGE_CUR_LE_OR_EXTENDS
		if (mode != PAGE_CUR_LE_OR_EXTENDS)
# endif /* PAGE_CUR_LE_OR_EXTENDS */
			ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE
			      || mode == PAGE_CUR_G || mode == PAGE_CUR_GE
			      || dict_index_is_spatial(index));
#endif /* UNIV_DEBUG */
	page = buf_block_get_frame(block);
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	ut_d(page_check_dir(page));
	const bool is_leaf = page_is_leaf(page);

#ifdef BTR_CUR_HASH_ADAPT
	if (is_leaf
	    && page_get_direction(page) == PAGE_RIGHT
	    && page_header_get_offs(page, PAGE_LAST_INSERT)
	    && mode == PAGE_CUR_LE
	    && !dict_index_is_spatial(index)
	    && page_header_get_field(page, PAGE_N_DIRECTION) > 3
	    && page_cur_try_search_shortcut(
		    block, index, tuple,
		    iup_matched_fields, ilow_matched_fields, cursor)) {
		return;
	}
# ifdef PAGE_CUR_DBG
	if (mode == PAGE_CUR_DBG) {
		mode = PAGE_CUR_LE;
	}
# endif
#endif /* BTR_CUR_HASH_ADAPT */

	/* If the mode is for R-tree indexes, use the special MBR
	related compare functions */
	if (dict_index_is_spatial(index) && mode > PAGE_CUR_LE) {
		/* For leaf level insert, we still use the traditional
		compare function for now */
		if (mode == PAGE_CUR_RTREE_INSERT && is_leaf) {
			mode = PAGE_CUR_LE;
		} else {
			rtr_cur_search_with_match(
				block, (dict_index_t*)index, tuple, mode,
				cursor, rtr_info);
			return;
		}
	}

	/* The following flag does not work for non-latin1 char sets because
	cmp_full_field does not tell how many bytes matched */
#ifdef PAGE_CUR_LE_OR_EXTENDS
	ut_a(mode != PAGE_CUR_LE_OR_EXTENDS);
#endif /* PAGE_CUR_LE_OR_EXTENDS */

	/* If mode PAGE_CUR_G is specified, we are trying to position the
	cursor to answer a query of the form "tuple < X", where tuple is
	the input parameter, and X denotes an arbitrary physical record on
	the page. We want to position the cursor on the first X which
	satisfies the condition. */

	up_matched_fields  = *iup_matched_fields;
	low_matched_fields = *ilow_matched_fields;

	/* Perform binary search. First the search is done through the page
	directory, after that as a linear search in the list of records
	owned by the upper limit directory slot. */

	low = 0;
	up = ulint(page_dir_get_n_slots(page)) - 1;

	/* Perform binary search until the lower and upper limit directory
	slots come to the distance 1 of each other */

	while (up - low > 1) {
		mid = (low + up) / 2;
		slot = page_dir_get_nth_slot(page, mid);
		mid_rec = page_dir_slot_get_rec(slot);

		cur_matched_fields = std::min(low_matched_fields,
					      up_matched_fields);

		offsets = offsets_;
		offsets = rec_get_offsets(
			mid_rec, index, offsets, is_leaf,
			dtuple_get_n_fields_cmp(tuple), &heap);

		cmp = cmp_dtuple_rec_with_match(
			tuple, mid_rec, offsets, &cur_matched_fields);

		if (cmp > 0) {
low_slot_match:
			low = mid;
			low_matched_fields = cur_matched_fields;

		} else if (cmp) {
#ifdef PAGE_CUR_LE_OR_EXTENDS
			if (mode == PAGE_CUR_LE_OR_EXTENDS
			    && page_cur_rec_field_extends(
				    tuple, mid_rec, offsets,
				    cur_matched_fields)) {

				goto low_slot_match;
			}
#endif /* PAGE_CUR_LE_OR_EXTENDS */
up_slot_match:
			up = mid;
			up_matched_fields = cur_matched_fields;

		} else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE
#ifdef PAGE_CUR_LE_OR_EXTENDS
			   || mode == PAGE_CUR_LE_OR_EXTENDS
#endif /* PAGE_CUR_LE_OR_EXTENDS */
			   ) {
			goto low_slot_match;
		} else {

			goto up_slot_match;
		}
	}

	slot = page_dir_get_nth_slot(page, low);
	low_rec = page_dir_slot_get_rec(slot);
	slot = page_dir_get_nth_slot(page, up);
	up_rec = page_dir_slot_get_rec(slot);

	/* Perform linear search until the upper and lower records come to
	distance 1 of each other. */

	while (page_rec_get_next_const(low_rec) != up_rec) {

		mid_rec = page_rec_get_next_const(low_rec);

		cur_matched_fields = std::min(low_matched_fields,
					      up_matched_fields);

		offsets = offsets_;
		offsets = rec_get_offsets(
			mid_rec, index, offsets, is_leaf,
			dtuple_get_n_fields_cmp(tuple), &heap);

		cmp = cmp_dtuple_rec_with_match(
			tuple, mid_rec, offsets, &cur_matched_fields);

		if (cmp > 0) {
low_rec_match:
			low_rec = mid_rec;
			low_matched_fields = cur_matched_fields;

		} else if (cmp) {
#ifdef PAGE_CUR_LE_OR_EXTENDS
			if (mode == PAGE_CUR_LE_OR_EXTENDS
			    && page_cur_rec_field_extends(
				    tuple, mid_rec, offsets,
				    cur_matched_fields)) {

				goto low_rec_match;
			}
#endif /* PAGE_CUR_LE_OR_EXTENDS */
up_rec_match:
			up_rec = mid_rec;
			up_matched_fields = cur_matched_fields;
		} else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE
#ifdef PAGE_CUR_LE_OR_EXTENDS
			   || mode == PAGE_CUR_LE_OR_EXTENDS
#endif /* PAGE_CUR_LE_OR_EXTENDS */
			   ) {
			if (!cmp && !cur_matched_fields) {
#ifdef UNIV_DEBUG
				mtr_t	mtr;
				mtr_start(&mtr);

				/* We got a match, but cur_matched_fields is
				0, it must have REC_INFO_MIN_REC_FLAG */
				ulint   rec_info = rec_get_info_bits(mid_rec,
                                                     rec_offs_comp(offsets));
				ut_ad(rec_info & REC_INFO_MIN_REC_FLAG);
				ut_ad(!page_has_prev(page));
				mtr_commit(&mtr);
#endif

				cur_matched_fields = dtuple_get_n_fields_cmp(tuple);
			}

			goto low_rec_match;
		} else {

			goto up_rec_match;
		}
	}

	if (mode <= PAGE_CUR_GE) {
		page_cur_position(up_rec, block, cursor);
	} else {
		page_cur_position(low_rec, block, cursor);
	}

	*iup_matched_fields  = up_matched_fields;
	*ilow_matched_fields = low_matched_fields;
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

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
	page_cur_t*		cursor)
{
	ulint		up;
	ulint		low;
	ulint		mid;
	const page_t*	page;
	const page_dir_slot_t* slot;
	const rec_t*	up_rec;
	const rec_t*	low_rec;
	const rec_t*	mid_rec;
	ulint		up_matched_fields;
	ulint		up_matched_bytes;
	ulint		low_matched_fields;
	ulint		low_matched_bytes;
	ulint		cur_matched_fields;
	ulint		cur_matched_bytes;
	int		cmp;
#ifdef UNIV_ZIP_DEBUG
	const page_zip_des_t*	page_zip = buf_block_get_page_zip(block);
#endif /* UNIV_ZIP_DEBUG */
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(dtuple_validate(tuple));
	ut_ad(!(tuple->info_bits & REC_INFO_MIN_REC_FLAG));
#ifdef UNIV_DEBUG
# ifdef PAGE_CUR_DBG
	if (mode != PAGE_CUR_DBG)
# endif /* PAGE_CUR_DBG */
# ifdef PAGE_CUR_LE_OR_EXTENDS
		if (mode != PAGE_CUR_LE_OR_EXTENDS)
# endif /* PAGE_CUR_LE_OR_EXTENDS */
			ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE
			      || mode == PAGE_CUR_G || mode == PAGE_CUR_GE);
#endif /* UNIV_DEBUG */
	page = buf_block_get_frame(block);
#ifdef UNIV_ZIP_DEBUG
	ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	ut_d(page_check_dir(page));

#ifdef BTR_CUR_HASH_ADAPT
	if (page_is_leaf(page)
	    && page_get_direction(page) == PAGE_RIGHT
	    && page_header_get_offs(page, PAGE_LAST_INSERT)
	    && mode == PAGE_CUR_LE
	    && page_header_get_field(page, PAGE_N_DIRECTION) > 3
	    && page_cur_try_search_shortcut_bytes(
		    block, index, tuple,
		    iup_matched_fields, iup_matched_bytes,
		    ilow_matched_fields, ilow_matched_bytes,
		    cursor)) {
		return;
	}
# ifdef PAGE_CUR_DBG
	if (mode == PAGE_CUR_DBG) {
		mode = PAGE_CUR_LE;
	}
# endif
#endif /* BTR_CUR_HASH_ADAPT */

	/* The following flag does not work for non-latin1 char sets because
	cmp_full_field does not tell how many bytes matched */
#ifdef PAGE_CUR_LE_OR_EXTENDS
	ut_a(mode != PAGE_CUR_LE_OR_EXTENDS);
#endif /* PAGE_CUR_LE_OR_EXTENDS */

	/* If mode PAGE_CUR_G is specified, we are trying to position the
	cursor to answer a query of the form "tuple < X", where tuple is
	the input parameter, and X denotes an arbitrary physical record on
	the page. We want to position the cursor on the first X which
	satisfies the condition. */

	up_matched_fields  = *iup_matched_fields;
	up_matched_bytes  = *iup_matched_bytes;
	low_matched_fields = *ilow_matched_fields;
	low_matched_bytes  = *ilow_matched_bytes;

	/* Perform binary search. First the search is done through the page
	directory, after that as a linear search in the list of records
	owned by the upper limit directory slot. */

	low = 0;
	up = ulint(page_dir_get_n_slots(page)) - 1;

	/* Perform binary search until the lower and upper limit directory
	slots come to the distance 1 of each other */
	const bool is_leaf = page_is_leaf(page);

	while (up - low > 1) {
		mid = (low + up) / 2;
		slot = page_dir_get_nth_slot(page, mid);
		mid_rec = page_dir_slot_get_rec(slot);

		ut_pair_min(&cur_matched_fields, &cur_matched_bytes,
			    low_matched_fields, low_matched_bytes,
			    up_matched_fields, up_matched_bytes);

		offsets = rec_get_offsets(
			mid_rec, index, offsets_, is_leaf,
			dtuple_get_n_fields_cmp(tuple), &heap);

		cmp = cmp_dtuple_rec_with_match_bytes(
			tuple, mid_rec, index, offsets,
			&cur_matched_fields, &cur_matched_bytes);

		if (cmp > 0) {
low_slot_match:
			low = mid;
			low_matched_fields = cur_matched_fields;
			low_matched_bytes = cur_matched_bytes;

		} else if (cmp) {
#ifdef PAGE_CUR_LE_OR_EXTENDS
			if (mode == PAGE_CUR_LE_OR_EXTENDS
			    && page_cur_rec_field_extends(
				    tuple, mid_rec, offsets,
				    cur_matched_fields)) {

				goto low_slot_match;
			}
#endif /* PAGE_CUR_LE_OR_EXTENDS */
up_slot_match:
			up = mid;
			up_matched_fields = cur_matched_fields;
			up_matched_bytes = cur_matched_bytes;

		} else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE
#ifdef PAGE_CUR_LE_OR_EXTENDS
			   || mode == PAGE_CUR_LE_OR_EXTENDS
#endif /* PAGE_CUR_LE_OR_EXTENDS */
			   ) {
			goto low_slot_match;
		} else {

			goto up_slot_match;
		}
	}

	slot = page_dir_get_nth_slot(page, low);
	low_rec = page_dir_slot_get_rec(slot);
	slot = page_dir_get_nth_slot(page, up);
	up_rec = page_dir_slot_get_rec(slot);

	/* Perform linear search until the upper and lower records come to
	distance 1 of each other. */

	while (page_rec_get_next_const(low_rec) != up_rec) {

		mid_rec = page_rec_get_next_const(low_rec);

		ut_pair_min(&cur_matched_fields, &cur_matched_bytes,
			    low_matched_fields, low_matched_bytes,
			    up_matched_fields, up_matched_bytes);

		if (UNIV_UNLIKELY(rec_get_info_bits(
					  mid_rec,
					  dict_table_is_comp(index->table))
				  & REC_INFO_MIN_REC_FLAG)) {
			ut_ad(!page_has_prev(page_align(mid_rec)));
			ut_ad(!page_rec_is_leaf(mid_rec)
			      || rec_is_metadata(mid_rec, *index));
			cmp = 1;
			goto low_rec_match;
		}

		offsets = rec_get_offsets(
			mid_rec, index, offsets_, is_leaf,
			dtuple_get_n_fields_cmp(tuple), &heap);

		cmp = cmp_dtuple_rec_with_match_bytes(
			tuple, mid_rec, index, offsets,
			&cur_matched_fields, &cur_matched_bytes);

		if (cmp > 0) {
low_rec_match:
			low_rec = mid_rec;
			low_matched_fields = cur_matched_fields;
			low_matched_bytes = cur_matched_bytes;

		} else if (cmp) {
#ifdef PAGE_CUR_LE_OR_EXTENDS
			if (mode == PAGE_CUR_LE_OR_EXTENDS
			    && page_cur_rec_field_extends(
				    tuple, mid_rec, offsets,
				    cur_matched_fields)) {

				goto low_rec_match;
			}
#endif /* PAGE_CUR_LE_OR_EXTENDS */
up_rec_match:
			up_rec = mid_rec;
			up_matched_fields = cur_matched_fields;
			up_matched_bytes = cur_matched_bytes;
		} else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE
#ifdef PAGE_CUR_LE_OR_EXTENDS
			   || mode == PAGE_CUR_LE_OR_EXTENDS
#endif /* PAGE_CUR_LE_OR_EXTENDS */
			   ) {
			goto low_rec_match;
		} else {

			goto up_rec_match;
		}
	}

	if (mode <= PAGE_CUR_GE) {
		page_cur_position(up_rec, block, cursor);
	} else {
		page_cur_position(low_rec, block, cursor);
	}

	*iup_matched_fields  = up_matched_fields;
	*iup_matched_bytes   = up_matched_bytes;
	*ilow_matched_fields = low_matched_fields;
	*ilow_matched_bytes  = low_matched_bytes;
	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}
#endif /* BTR_CUR_HASH_ADAPT */

/***********************************************************//**
Positions a page cursor on a randomly chosen user record on a page. If there
are no user records, sets the cursor on the infimum record. */
void
page_cur_open_on_rnd_user_rec(
/*==========================*/
	buf_block_t*	block,	/*!< in: page */
	page_cur_t*	cursor)	/*!< out: page cursor */
{
	const ulint	n_recs = page_get_n_recs(block->frame);

	page_cur_set_before_first(block, cursor);

	if (UNIV_UNLIKELY(n_recs == 0)) {

		return;
	}

	cursor->rec = page_rec_get_nth(block->frame,
				       ut_rnd_interval(n_recs) + 1);
}

static void rec_set_heap_no(rec_t *rec, ulint heap_no, bool compact)
{
  rec_set_bit_field_2(rec, heap_no,
                      compact ? REC_NEW_HEAP_NO : REC_OLD_HEAP_NO,
                      REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
}

static rec_t*
page_cur_parse_insert_rec_zip(
	page_cur_t*	cursor,	/*!< in/out: page cursor */
	dict_index_t*	index,	/*!< in: record descriptor */
	const rec_t*	rec,	/*!< in: pointer to a physical record */
	offset_t*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr);	/*!< in/out: mini-transaction */
/***********************************************************//**
Parses a log record of a record insert on a page.
@return end of log record or NULL */
ATTRIBUTE_COLD /* only used when crash-upgrading */
const byte*
page_cur_parse_insert_rec(
/*======================*/
	bool		is_short,/*!< in: true if short inserts */
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	buf_block_t*	block,	/*!< in: page or NULL */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr or NULL */
{
	ulint	origin_offset		= 0; /* remove warning */
	ulint	end_seg_len;
	ulint	mismatch_index		= 0; /* remove warning */
	page_t*	page;
	rec_t*	cursor_rec;
	byte	buf1[1024];
	byte*	buf;
	const byte*	ptr2		= ptr;
	ulint		info_and_status_bits = 0; /* remove warning */
	page_cur_t	cursor;
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	page = block ? buf_block_get_frame(block) : NULL;

	if (is_short) {
		cursor_rec = page_rec_get_prev(page_get_supremum_rec(page));
	} else {
		ulint	offset;

		/* Read the cursor rec offset as a 2-byte ulint */

		if (UNIV_UNLIKELY(end_ptr < ptr + 2)) {

			return(NULL);
		}

		offset = mach_read_from_2(ptr);
		ptr += 2;

		cursor_rec = page + offset;

		if (offset >= srv_page_size) {

			recv_sys.found_corrupt_log = TRUE;

			return(NULL);
		}
	}

	end_seg_len = mach_parse_compressed(&ptr, end_ptr);

	if (ptr == NULL) {

		return(NULL);
	}

	if (end_seg_len >= srv_page_size << 1) {
		recv_sys.found_corrupt_log = TRUE;

		return(NULL);
	}

	if (end_seg_len & 0x1UL) {
		/* Read the info bits */

		if (end_ptr < ptr + 1) {

			return(NULL);
		}

		info_and_status_bits = mach_read_from_1(ptr);
		ptr++;

		origin_offset = mach_parse_compressed(&ptr, end_ptr);

		if (ptr == NULL) {

			return(NULL);
		}

		ut_a(origin_offset < srv_page_size);

		mismatch_index = mach_parse_compressed(&ptr, end_ptr);

		if (ptr == NULL) {

			return(NULL);
		}

		ut_a(mismatch_index < srv_page_size);
	}

	if (end_ptr < ptr + (end_seg_len >> 1)) {

		return(NULL);
	}

	if (!block) {

		return(const_cast<byte*>(ptr + (end_seg_len >> 1)));
	}

	ut_ad(!!page_is_comp(page) == dict_table_is_comp(index->table));
	ut_ad(!buf_block_get_page_zip(block) || page_is_comp(page));

	/* Read from the log the inserted index record end segment which
	differs from the cursor record */

	const bool is_leaf = page_is_leaf(page);

	offsets = rec_get_offsets(cursor_rec, index, offsets, is_leaf,
				  ULINT_UNDEFINED, &heap);

	if (!(end_seg_len & 0x1UL)) {
		info_and_status_bits = rec_get_info_and_status_bits(
			cursor_rec, page_is_comp(page));
		origin_offset = rec_offs_extra_size(offsets);
		mismatch_index = rec_offs_size(offsets) - (end_seg_len >> 1);
	}

	end_seg_len >>= 1;

	if (mismatch_index + end_seg_len < sizeof buf1) {
		buf = buf1;
	} else {
		buf = static_cast<byte*>(
			ut_malloc_nokey(mismatch_index + end_seg_len));
	}

	/* Build the inserted record to buf */

        if (UNIV_UNLIKELY(mismatch_index >= srv_page_size)) {

		ib::fatal() << "is_short " << is_short << ", "
			<< "info_and_status_bits " << info_and_status_bits
			<< ", offset " << page_offset(cursor_rec) << ","
			" o_offset " << origin_offset << ", mismatch index "
			<< mismatch_index << ", end_seg_len " << end_seg_len
			<< " parsed len " << (ptr - ptr2);
	}

	memcpy(buf, rec_get_start(cursor_rec, offsets), mismatch_index);
	memcpy(buf + mismatch_index, ptr, end_seg_len);
	rec_set_heap_no(buf + origin_offset, PAGE_HEAP_NO_USER_LOW,
			page_is_comp(page));

	if (page_is_comp(page)) {
		rec_set_info_and_status_bits(buf + origin_offset,
					     info_and_status_bits);
	} else {
		rec_set_bit_field_1(buf + origin_offset, info_and_status_bits,
				    REC_OLD_INFO_BITS,
				    REC_INFO_BITS_MASK, REC_INFO_BITS_SHIFT);
	}

	page_cur_position(cursor_rec, block, &cursor);

	offsets = rec_get_offsets(buf + origin_offset, index, offsets,
				  is_leaf, ULINT_UNDEFINED, &heap);
	/* The redo log record should only have been written
	after the write was successful. */
	if (block->page.zip.data) {
		if (!page_cur_parse_insert_rec_zip(&cursor, index,
						   buf + origin_offset,
						   offsets, mtr)) {
			ut_error;
		}
	} else if (!page_cur_insert_rec_low(&cursor, index,
					    buf + origin_offset,
					    offsets, mtr)) {
		ut_error;
	}

	if (buf != buf1) {

		ut_free(buf);
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	return(const_cast<byte*>(ptr + end_seg_len));
}

/**
Set the owned records field of the record pointed to by a directory slot.
@tparam compressed  whether to update any ROW_FORMAT=COMPRESSED page as well
@param[in,out]  block    file page
@param[in]      slot     sparse directory slot
@param[in,out]  n        number of records owned by the directory slot
@param[in,out]  mtr      mini-transaction */
template<bool compressed>
static void page_dir_slot_set_n_owned(buf_block_t *block,
                                      const page_dir_slot_t *slot,
                                      ulint n, mtr_t *mtr)
{
  rec_t *rec= const_cast<rec_t*>(page_dir_slot_get_rec(slot));
  page_rec_set_n_owned<compressed>(block, rec, n, page_rec_is_comp(rec), mtr);
}

/**
Split a directory slot which owns too many records.
@tparam compressed  whether to update the ROW_FORMAT=COMPRESSED page as well
@param[in,out]  block   index page
@param[in]      s       the slot that needs to be split
@param[in,out]  mtr     mini-transaction */
template<bool compressed>
static void page_dir_split_slot(buf_block_t *block, ulint s, mtr_t* mtr)
{
  ut_ad(!block->page.zip.data || page_is_comp(block->frame));
  ut_ad(!compressed || block->page.zip.data);
  ut_ad(s);

  page_dir_slot_t *slot= page_dir_get_nth_slot(block->frame, s);
  const ulint n_owned= PAGE_DIR_SLOT_MAX_N_OWNED + 1;

  ut_ad(page_dir_slot_get_n_owned(slot) == n_owned);
  compile_time_assert((PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2
                      >= PAGE_DIR_SLOT_MIN_N_OWNED);

  /* 1. We loop to find a record approximately in the middle of the
  records owned by the slot. */

  const rec_t *rec= page_dir_slot_get_rec(slot + PAGE_DIR_SLOT_SIZE);

  for (ulint i= n_owned / 2; i--; )
    rec= page_rec_get_next_const(rec);

  /* Add a directory slot immediately below this one. */
  constexpr uint16_t n_slots_f= PAGE_N_DIR_SLOTS + PAGE_HEADER;
  byte *n_slots_p= my_assume_aligned<2>(n_slots_f + block->frame);
  const uint16_t n_slots= mach_read_from_2(n_slots_p);

  page_dir_slot_t *last_slot= static_cast<page_dir_slot_t*>
          (block->frame + srv_page_size - (PAGE_DIR + PAGE_DIR_SLOT_SIZE) -
           n_slots * PAGE_DIR_SLOT_SIZE);
  memmove_aligned<2>(last_slot, last_slot + PAGE_DIR_SLOT_SIZE,
                     slot - last_slot);

  const ulint half_owned= n_owned / 2;

  mtr->write<2>(*block, n_slots_p, 1U + n_slots);

  if (compressed)
  {
    /* Log changes to the compressed page header and the dense page
    directory. */
    memcpy_aligned<2>(&block->page.zip.data[n_slots_f], n_slots_p, 2);
    mach_write_to_2(slot, page_offset(rec));
    page_rec_set_n_owned<true>(block, page_dir_slot_get_rec(slot), half_owned,
                               true, mtr);
    page_rec_set_n_owned<true>(block,
                               page_dir_slot_get_rec(slot -
                                                     PAGE_DIR_SLOT_SIZE),
                               n_owned - half_owned, true, mtr);
  }
  else
  {
    mtr->memmove(*block, page_offset(last_slot),
                 page_offset(last_slot) + PAGE_DIR_SLOT_SIZE,
                 slot - last_slot);
    mtr->write<2>(*block, slot, page_offset(rec));
    const bool comp= page_is_comp(block->frame) != 0;
    page_rec_set_n_owned<false>(block, page_dir_slot_get_rec(slot), half_owned,
                                comp, mtr);
    page_rec_set_n_owned<false>(block,
                                page_dir_slot_get_rec(slot -
                                                      PAGE_DIR_SLOT_SIZE),
                                n_owned - half_owned, comp, mtr);
  }
}

/**
Try to balance an underfilled directory slot with an adjacent one,
so that there are at least the minimum number of records owned by the slot;
this may result in merging the two slots.
@param[in,out]	block		index page
@param[in]	s		the slot to be balanced
@param[in,out]	mtr		mini-transaction */
static void page_dir_balance_slot(buf_block_t *block, ulint s, mtr_t *mtr)
{
	ut_ad(!block->page.zip.data || page_is_comp(block->frame));
	ut_ad(s > 0);

	const ulint n_slots = page_dir_get_n_slots(block->frame);

	if (UNIV_UNLIKELY(s + 1 == n_slots)) {
		/* The last directory slot cannot be balanced. */
		return;
	}

	ut_ad(s < n_slots);

	page_dir_slot_t* slot = page_dir_get_nth_slot(block->frame, s);
	page_dir_slot_t* up_slot = slot - PAGE_DIR_SLOT_SIZE;
	const ulint up_n_owned = page_dir_slot_get_n_owned(up_slot);

	ut_ad(page_dir_slot_get_n_owned(slot)
	      == PAGE_DIR_SLOT_MIN_N_OWNED - 1);

	if (up_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED) {
		compile_time_assert(2 * PAGE_DIR_SLOT_MIN_N_OWNED - 1
				    <= PAGE_DIR_SLOT_MAX_N_OWNED);
		/* Merge the slots. */
		ulint n_owned = page_dir_slot_get_n_owned(slot);
		page_dir_slot_set_n_owned<true>(block, slot, 0, mtr);
		page_dir_slot_set_n_owned<true>(block, up_slot, n_owned
						+ page_dir_slot_get_n_owned(
							up_slot), mtr);
		/* Shift the slots */
		page_dir_slot_t* last_slot = page_dir_get_nth_slot(
			block->frame, n_slots - 1);
		memmove_aligned<2>(last_slot + PAGE_DIR_SLOT_SIZE, last_slot,
				   slot - last_slot);
		constexpr uint16_t n_slots_f = PAGE_N_DIR_SLOTS + PAGE_HEADER;
		byte *n_slots_p= my_assume_aligned<2>
			(n_slots_f + block->frame);
		mtr->write<2>(*block, n_slots_p, n_slots - 1);

		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			memset_aligned<2>(last_slot, 0, 2);
			memcpy_aligned<2>(n_slots_f + block->page.zip.data,
					  n_slots_p, 2);
		} else {
			mtr->memmove(*block, PAGE_DIR_SLOT_SIZE
				     + page_offset(last_slot),
				     page_offset(last_slot), slot - last_slot);
			mtr->write<2>(*block, last_slot, 0U);
		}

		return;
	}

	/* Transfer one record to the underfilled slot */
	rec_t* old_rec = const_cast<rec_t*>(page_dir_slot_get_rec(slot));
	rec_t* new_rec;

	if (page_is_comp(block->frame)) {
		new_rec = rec_get_next_ptr(old_rec, TRUE);

		page_rec_set_n_owned<true>(block, old_rec, 0, true, mtr);
		page_rec_set_n_owned<true>(block, new_rec,
					   PAGE_DIR_SLOT_MIN_N_OWNED,
					   true, mtr);
		if (UNIV_LIKELY_NULL(block->page.zip.data)) {
			mach_write_to_2(slot, page_offset(new_rec));
			goto func_exit;
		}
	} else {
		new_rec = rec_get_next_ptr(old_rec, FALSE);

		page_rec_set_n_owned<false>(block, old_rec, 0, false, mtr);
		page_rec_set_n_owned<false>(block, new_rec,
					    PAGE_DIR_SLOT_MIN_N_OWNED,
					    false, mtr);
	}

	mtr->write<2>(*block, slot, page_offset(new_rec));
func_exit:
	page_dir_slot_set_n_owned<true>(block, up_slot, up_n_owned - 1, mtr);
}

/** Allocate space for inserting an index record.
@tparam compressed  whether to update the ROW_FORMAT=COMPRESSED page as well
@param[in,out]	block		index page
@param[in]	need		number of bytes needed
@param[out]	heap_no		record heap number
@param[in,out]	mtr		mini-transaction
@return	pointer to the start of the allocated buffer
@retval	NULL	if allocation fails */
template<bool compressed=false>
static byte* page_mem_alloc_heap(buf_block_t *block, ulint need,
                                 ulint *heap_no, mtr_t *mtr)
{
  ut_ad(!compressed || block->page.zip.data);

  byte *heap_top= my_assume_aligned<2>(PAGE_HEAP_TOP + PAGE_HEADER +
                                       block->frame);

  const uint16_t top= mach_read_from_2(heap_top);

  if (need > page_get_max_insert_size(block->frame, 1))
    return NULL;

  byte *n_heap= my_assume_aligned<2>(PAGE_N_HEAP + PAGE_HEADER + block->frame);

  const uint16_t h= mach_read_from_2(n_heap);
  *heap_no= h & 0x7fff;
  ut_ad(*heap_no < srv_page_size / REC_N_NEW_EXTRA_BYTES);
  compile_time_assert(UNIV_PAGE_SIZE_MAX / REC_N_NEW_EXTRA_BYTES < 0x3fff);

  mach_write_to_2(heap_top, top + need);
  mach_write_to_2(n_heap, h + 1);
  mtr->memcpy(*block, PAGE_HEAP_TOP + PAGE_HEADER, 4);

  if (compressed)
  {
    ut_ad(h & 0x8000);
    memcpy_aligned<4>(&block->page.zip.data[PAGE_HEAP_TOP + PAGE_HEADER],
                      heap_top, 4);
  }

  compile_time_assert(PAGE_N_HEAP == PAGE_HEAP_TOP + 2);
  return &block->frame[top];
}

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
	offset_t*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  buf_block_t* block = cur->block;

  ut_ad(rec_offs_validate(rec, index, offsets));

  ut_ad(index->table->not_redundant() == !!page_is_comp(block->frame));
  ut_ad(!!page_is_comp(block->frame) == !!rec_offs_comp(offsets));
  ut_ad(fil_page_index_page_check(block->frame));
  ut_ad(mach_read_from_8(PAGE_HEADER + PAGE_INDEX_ID + block->frame) ==
        index->id ||
        index->is_dummy ||
        mtr->is_inside_ibuf());

  ut_ad(!page_rec_is_supremum(cur->rec));

  /* We should not write log for ROW_FORMAT=COMPRESSED pages here. */
  ut_ad(mtr->get_log_mode() != MTR_LOG_ALL ||
        !(index->table->flags & DICT_TF_MASK_ZIP_SSIZE));

  /* 1. Get the size of the physical record in the page */
  const ulint rec_size= rec_offs_size(offsets);

#ifdef UNIV_DEBUG_VALGRIND
  {
    const void *rec_start= rec - rec_offs_extra_size(offsets);
    ulint extra_size= rec_offs_extra_size(offsets) -
      (page_is_comp(block->frame)
       ? REC_N_NEW_EXTRA_BYTES
       : REC_N_OLD_EXTRA_BYTES);
    /* All data bytes of the record must be valid. */
    UNIV_MEM_ASSERT_RW(rec, rec_offs_data_size(offsets));
    /* The variable-length header must be valid. */
    UNIV_MEM_ASSERT_RW(rec_start, extra_size);
  }
#endif /* UNIV_DEBUG_VALGRIND */

  /* 2. Try to find suitable space from page memory management */
  ulint heap_no;
  byte *insert_buf;
  alignas(2) byte hdr[8];

  if (rec_t* free_rec = page_header_get_ptr(block->frame, PAGE_FREE))
  {
    /* Try to reuse the head of PAGE_FREE. */
    offset_t foffsets_[REC_OFFS_NORMAL_SIZE];
    mem_heap_t *heap= nullptr;

    rec_offs_init(foffsets_);

    offset_t *foffsets= rec_get_offsets(free_rec, index, foffsets_,
                                        page_is_leaf(block->frame),
                                        ULINT_UNDEFINED, &heap);
    insert_buf= free_rec - rec_offs_extra_size(foffsets);
    const bool too_small= rec_offs_size(foffsets) < rec_size;
    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);

    if (too_small)
      goto use_heap;

    byte *page_free= my_assume_aligned<2>(PAGE_FREE + PAGE_HEADER +
                                          block->frame);
    if (page_is_comp(block->frame))
    {
      heap_no= rec_get_heap_no_new(free_rec);
      const rec_t *next= rec_get_next_ptr(free_rec, true);
      mach_write_to_2(hdr, next ? page_offset(next) : 0);
    }
    else
    {
      heap_no= rec_get_heap_no_old(free_rec);
      memcpy(hdr, free_rec - REC_NEXT, 2);
    }

    static_assert(PAGE_GARBAGE == PAGE_FREE + 2, "compatibility");
    byte *page_garbage = my_assume_aligned<2>(page_free + 2);
    ut_ad(mach_read_from_2(page_garbage) >= rec_size);
    mach_write_to_2(my_assume_aligned<2>(hdr + 2),
                    mach_read_from_2(page_garbage) - rec_size);
    mtr->memcpy(*block, page_free, hdr, 4);
  }
  else
  {
use_heap:
    insert_buf= page_mem_alloc_heap(block, rec_size, &heap_no, mtr);

    if (UNIV_UNLIKELY(!insert_buf))
      return nullptr;
  }

  const ulint extra_size= rec_offs_extra_size(offsets);
  ut_ad(cur->rec != insert_buf + extra_size);

  const rec_t *next_rec= page_rec_get_next_low(cur->rec,
                                               page_is_comp(block->frame));

  /* Update page header fields */
  rec_t *last_insert= page_header_get_ptr(block->frame, PAGE_LAST_INSERT);
  ut_ad(!last_insert || !page_is_comp(block->frame) ||
        rec_get_node_ptr_flag(last_insert) == rec_get_node_ptr_flag(rec));

  static_assert(PAGE_N_RECS - PAGE_LAST_INSERT + 2 == sizeof hdr,
                "compatibility");

  /* Write PAGE_LAST_INSERT */
  mach_write_to_2(hdr, page_offset(insert_buf + extra_size));
  static_assert(PAGE_INSTANT - PAGE_LAST_INSERT == 2, "compatibility");
  static_assert(PAGE_DIRECTION_B - PAGE_INSTANT == 1, "compatibility");
  static_assert(PAGE_N_DIRECTION - PAGE_DIRECTION_B == 1, "compat.");
  static_assert(PAGE_N_RECS - PAGE_N_DIRECTION == 2, "compatibility");

  /* Update PAGE_DIRECTION_B, PAGE_N_DIRECTION if needed */
  memcpy_aligned<2>(hdr + 2, PAGE_HEADER + PAGE_INSTANT + block->frame,
                    PAGE_N_RECS - PAGE_INSTANT + 2);

  if (!index->is_spatial())
  {
    byte *dir= &hdr[PAGE_DIRECTION_B - PAGE_LAST_INSERT];
    byte *n= my_assume_aligned<2>(&hdr[PAGE_N_DIRECTION - PAGE_LAST_INSERT]);
    if (UNIV_UNLIKELY(!last_insert))
    {
no_direction:
      *dir= (*dir & ~((1U << 3) - 1)) | PAGE_NO_DIRECTION;
      memset(n, 0, 2);
    }
    else if (last_insert == cur->rec && (*dir & ((1U << 3) - 1)) != PAGE_LEFT)
    {
      *dir= (*dir & ~((1U << 3) - 1)) | PAGE_RIGHT;
inc_dir:
      mach_write_to_2(n, mach_read_from_2(n) + 1);
    }
    else if (next_rec == last_insert && (*dir & ((1U << 3) - 1)) != PAGE_RIGHT)
    {
      *dir= (*dir & ~((1U << 3) - 1)) | PAGE_LEFT;
      goto inc_dir;
    }
    else
      goto no_direction;
  }

  /* Update PAGE_N_RECS. */
  mach_write_to_2(hdr + PAGE_N_RECS - PAGE_LAST_INSERT,
                  mach_read_from_2(hdr + PAGE_N_RECS - PAGE_LAST_INSERT) + 1);
  /* Write the header fields in one record. */
  mtr->memcpy(*block, PAGE_LAST_INSERT + PAGE_HEADER + block->frame,
              hdr, PAGE_N_RECS - PAGE_LAST_INSERT + 2);

  /* Update the preceding record header, the 'owner' record and
  prepare the record to insert. */
  ulint n_owned;
  static_assert(sizeof hdr >= REC_N_NEW_EXTRA_BYTES, "compatibility");
  static_assert(sizeof hdr >= REC_N_OLD_EXTRA_BYTES, "compatibility");
  ulint fixed_hdr;

  if (page_is_comp(block->frame))
  {
#ifdef UNIV_DEBUG
    switch (rec_get_status(cur->rec)) {
    case REC_STATUS_ORDINARY:
    case REC_STATUS_NODE_PTR:
    case REC_STATUS_INSTANT:
    case REC_STATUS_INFIMUM:
      break;
    case REC_STATUS_SUPREMUM:
      ut_ad(!"wrong status on cur->rec");
    }
    switch (rec_get_status(rec)) {
    case REC_STATUS_ORDINARY:
    case REC_STATUS_NODE_PTR:
    case REC_STATUS_INSTANT:
      break;
    case REC_STATUS_INFIMUM:
    case REC_STATUS_SUPREMUM:
      ut_ad(!"wrong status on rec");
    }
    ut_ad(rec_get_status(next_rec) != REC_STATUS_INFIMUM);
#endif
    memcpy(hdr, rec - REC_N_NEW_EXTRA_BYTES, REC_N_NEW_EXTRA_BYTES);
    rec_set_bit_field_1(hdr + REC_N_NEW_EXTRA_BYTES, 0, REC_NEW_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    rec_set_bit_field_2(hdr + REC_N_NEW_EXTRA_BYTES, heap_no,
                        REC_NEW_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    const rec_t *insert_rec= insert_buf + extra_size;
    mach_write_to_2(REC_N_NEW_EXTRA_BYTES - REC_NEXT + hdr,
                    static_cast<uint16_t>(next_rec - insert_rec));
    mtr->write<2>(*block, cur->rec - REC_NEXT,
                  static_cast<uint16_t>(insert_rec - cur->rec));
    while (!(n_owned = rec_get_n_owned_new(next_rec)))
      next_rec= page_rec_get_next_low(next_rec, true);
    page_rec_set_n_owned<false>(block, const_cast<rec_t*>(next_rec),
                                n_owned + 1, true, mtr);
    fixed_hdr= REC_N_NEW_EXTRA_BYTES;
  }
  else
  {
    memcpy(hdr, rec - REC_N_OLD_EXTRA_BYTES, REC_N_OLD_EXTRA_BYTES);
    rec_set_bit_field_1(hdr + REC_N_OLD_EXTRA_BYTES, 0, REC_OLD_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    rec_set_bit_field_2(hdr + REC_N_OLD_EXTRA_BYTES, heap_no,
                        REC_OLD_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    memcpy(hdr + REC_N_OLD_EXTRA_BYTES - REC_NEXT, cur->rec - REC_NEXT, 2);
    mtr->write<2>(*block, cur->rec - REC_NEXT,
                  page_offset(insert_buf + extra_size));
    while (!(n_owned = rec_get_n_owned_old(next_rec)))
      next_rec= page_rec_get_next_low(next_rec, false);
    page_rec_set_n_owned<false>(block, const_cast<rec_t*>(next_rec),
                                n_owned + 1, false, mtr);
    fixed_hdr= REC_N_OLD_EXTRA_BYTES;
  }

  ut_ad(fixed_hdr <= extra_size);
  /* Insert the record, possibly copying from the preceding record. */
  const ulint data_size = rec_offs_data_size(offsets);
  ut_ad(mtr->has_modifications());

  if (mtr->get_log_mode() == MTR_LOG_ALL)
  {
    /* Temporarily write everything to rec, to simplify the code below. */
    byte rec_hdr[REC_N_OLD_EXTRA_BYTES];
    memcpy(rec_hdr, rec - fixed_hdr, fixed_hdr);
    memcpy(const_cast<rec_t*>(rec - fixed_hdr), hdr, fixed_hdr);

    byte *b= insert_buf;
    const byte *r= rec - extra_size;

    /* Skip any unchanged prefix of the record header. */
    for (;; b++, r++)
      if (UNIV_UNLIKELY(b == insert_buf + rec_size))
        goto rec_done;
      else if (*b != *r)
        break;

    {
      const byte *c= cur->rec - (rec - r);
      const byte *c_end= std::min(cur->rec + data_size,
                                  block->frame + srv_page_size);
      if (c <= insert_buf && c_end > insert_buf)
        c_end= insert_buf;

      /* Try to copy any bytes of the preceding record. */
      if (UNIV_LIKELY(c >= block->frame && c < c_end))
      {
        const byte *cm= c;
        const byte *rm= r;
        while (*rm++ == *cm++)
          if (cm == c_end)
            break;
        rm--, cm--;
        ut_ad(rm - r + b <= insert_buf + rec_size);
        size_t len= static_cast<size_t>(rm - r);
        ut_ad(!memcmp(r, c, len));
        if (len > 2)
        {
          memcpy(b, c, len);
          mtr->memmove(*block, page_offset(b), page_offset(c), len);
          c= cm;
          b+= rm - r;
          r= rm;
        }
      }

      if (c < cur->rec)
      {
        if (!data_size)
        {
no_data:
          mtr->memcpy<mtr_t::FORCED>(*block, b, r, cur->rec - c);
          goto rec_done;
        }
        /* Some header bytes differ. Compare the data separately. */
        byte *bd= insert_buf + extra_size;
        const byte *rd= rec;
        /* Skip any unchanged prefix of the record payload. */
        for (;; bd++, rd++)
          if (bd == insert_buf + rec_size)
            goto no_data;
          else if (*bd != *rd)
            break;

        /* Try to copy any data bytes of the preceding record. */
        const byte * const cd= cur->rec + (rd - rec);
        const byte *cdm= cd;
        const byte *rdm= rd;
        while (*rdm++ == *cdm++)
          if (cdm == c_end)
            break;
        cdm--, rdm--;
        ut_ad(rdm - rd + bd <= insert_buf + rec_size);
        size_t len= static_cast<size_t>(rdm - rd);
        ut_ad(!memcmp(rd, cd, len));
        if (len > 2)
        {
          mtr->memcpy<mtr_t::FORCED>(*block, b, r, cur->rec - c);
          memcpy(bd, cd, len);
          mtr->memmove(*block, page_offset(bd), page_offset(cd), len);
          c= cdm;
          b= rdm - rd + bd;
          r= rdm;
        }
      }
    }

    if (size_t len= static_cast<size_t>(insert_buf + rec_size - b))
      mtr->memcpy<mtr_t::FORCED>(*block, b, r, len);
rec_done:
    ut_ad(!memcmp(insert_buf, rec - extra_size, rec_size));

    /* Restore the record header. */
    memcpy(const_cast<rec_t*>(rec - fixed_hdr), rec_hdr, fixed_hdr);
  }
  else
  {
    memcpy(insert_buf, rec - extra_size, extra_size - fixed_hdr);
    memcpy(insert_buf + extra_size - fixed_hdr, hdr, fixed_hdr);
    memcpy(insert_buf + extra_size, rec, data_size);
  }

  /* We have incremented the n_owned field of the owner record.
  If the number exceeds PAGE_DIR_SLOT_MAX_N_OWNED, we have to split the
  corresponding directory slot in two. */

  if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED))
    page_dir_split_slot<false>(block, page_dir_find_owner_slot(next_rec), mtr);

  rec_offs_make_valid(insert_buf + extra_size, index,
                      page_is_leaf(block->frame), offsets);
  return insert_buf + extra_size;
}

/** Add a slot to the dense page directory.
@param[in,out]  block   ROW_FORMAT=COMPRESSED page
@param[in]      index   the index that the page belongs to
@param[in,out]  mtr     mini-transaction */
static inline void page_zip_dir_add_slot(buf_block_t *block,
                                         const dict_index_t *index, mtr_t *mtr)
{
  page_zip_des_t *page_zip= &block->page.zip;

  ut_ad(page_is_comp(page_zip->data));
  UNIV_MEM_ASSERT_RW(page_zip->data, page_zip_get_size(page_zip));

  /* Read the old n_dense (n_heap has already been incremented). */
  ulint n_dense= page_dir_get_n_heap(page_zip->data) - (PAGE_HEAP_NO_USER_LOW +
                                                        1U);

  byte *dir= page_zip->data + page_zip_get_size(page_zip) -
    PAGE_ZIP_DIR_SLOT_SIZE * n_dense;
  byte *stored= dir;

  if (!page_is_leaf(page_zip->data))
  {
    ut_ad(!page_zip->n_blobs);
    stored-= n_dense * REC_NODE_PTR_SIZE;
  }
  else if (index->is_clust())
  {
    /* Move the BLOB pointer array backwards to make space for the
    columns DB_TRX_ID,DB_ROLL_PTR and the dense directory slot. */

    stored-= n_dense * (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
    byte *externs= stored - page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
    byte *dst= externs - PAGE_ZIP_CLUST_LEAF_SLOT_SIZE;
    ut_ad(!memcmp(dst, field_ref_zero, PAGE_ZIP_CLUST_LEAF_SLOT_SIZE));
    if (const ulint len = ulint(stored - externs))
    {
      memmove(dst, externs, len);
      mtr->memmove(*block, dst - page_zip->data, externs - page_zip->data,
                   len);
    }
  }
  else
  {
    stored-= page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
    ut_ad(!memcmp(stored - PAGE_ZIP_DIR_SLOT_SIZE, field_ref_zero,
                  PAGE_ZIP_DIR_SLOT_SIZE));
  }

  /* Move the uncompressed area backwards to make space
  for one directory slot. */
  if (const ulint len = ulint(dir - stored))
  {
    byte* dst = stored - PAGE_ZIP_DIR_SLOT_SIZE;
    memmove(dst, stored, len);
    mtr->memmove(*block, dst - page_zip->data, stored - page_zip->data, len);
  }
}

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
	offset_t*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  page_zip_des_t * const page_zip= page_cur_get_page_zip(cursor);
  ut_ad(page_zip);
  ut_ad(rec_offs_validate(rec, index, offsets));

  ut_ad(index->table->not_redundant());
  ut_ad(page_is_comp(cursor->block->frame));
  ut_ad(rec_offs_comp(offsets));
  ut_ad(fil_page_get_type(cursor->block->frame) == FIL_PAGE_INDEX ||
        fil_page_get_type(cursor->block->frame) == FIL_PAGE_RTREE);
  ut_ad(mach_read_from_8(PAGE_HEADER + PAGE_INDEX_ID + cursor->block->frame) ==
        index->id ||
        index->is_dummy ||
        mtr->is_inside_ibuf());
  ut_ad(!page_get_instant(cursor->block->frame));
  ut_ad(!page_cur_is_after_last(cursor));
#ifdef UNIV_ZIP_DEBUG
  ut_a(page_zip_validate(page_zip, cursor->block->frame, index));
#endif /* UNIV_ZIP_DEBUG */

  /* 1. Get the size of the physical record in the page */
  const ulint rec_size= rec_offs_size(offsets);

#ifdef UNIV_DEBUG_VALGRIND
  {
    const void *rec_start= rec - rec_offs_extra_size(offsets);
    ulint extra_size= rec_offs_extra_size(offsets) - REC_N_NEW_EXTRA_BYTES;
    /* All data bytes of the record must be valid. */
    UNIV_MEM_ASSERT_RW(rec, rec_offs_data_size(offsets));
    /* The variable-length header must be valid. */
    UNIV_MEM_ASSERT_RW(rec_start, extra_size);
  }
#endif /* UNIV_DEBUG_VALGRIND */
  const bool reorg_before_insert= page_has_garbage(cursor->block->frame) &&
    rec_size > page_get_max_insert_size(cursor->block->frame, 1) &&
    rec_size <= page_get_max_insert_size_after_reorganize(cursor->block->frame,
                                                          1);
  constexpr uint16_t page_free_f= PAGE_FREE + PAGE_HEADER;
  byte* const page_free = my_assume_aligned<4>(page_free_f +
                                               cursor->block->frame);
  uint16_t free_rec= 0;

  /* 2. Try to find suitable space from page memory management */
  ulint heap_no;
  byte *insert_buf;

  if (reorg_before_insert ||
      !page_zip_available(page_zip, index->is_clust(), rec_size, 1))
  {
    /* SET GLOBAL might be executed concurrently. Sample the value once. */
    ulint level= page_zip_level;
#ifdef UNIV_DEBUG
    const rec_t * const cursor_rec= page_cur_get_rec(cursor);
#endif /* UNIV_DEBUG */

    if (page_is_empty(cursor->block->frame))
    {
      ut_ad(page_cur_is_before_first(cursor));

      /* This is an empty page. Recreate to remove the modification log. */
      page_create_zip(cursor->block, index,
                      page_header_get_field(cursor->block->frame, PAGE_LEVEL),
                      0, mtr);
      ut_ad(!page_header_get_ptr(cursor->block->frame, PAGE_FREE));

      if (page_zip_available(page_zip, index->is_clust(), rec_size, 1))
        goto use_heap;

      /* The cursor should remain on the page infimum. */
      return nullptr;
    }

    if (page_zip->m_nonempty || page_has_garbage(cursor->block->frame))
    {
      ulint pos= page_rec_get_n_recs_before(cursor->rec);

      if (!page_zip_reorganize(cursor->block, index, level, mtr, true))
      {
        ut_ad(cursor->rec == cursor_rec);
        return nullptr;
      }

      if (pos)
        cursor->rec= page_rec_get_nth(cursor->block->frame, pos);
      else
        ut_ad(cursor->rec == page_get_infimum_rec(cursor->block->frame));

      ut_ad(!page_header_get_ptr(cursor->block->frame, PAGE_FREE));

      if (page_zip_available(page_zip, index->is_clust(), rec_size, 1))
        goto use_heap;
    }

    /* Try compressing the whole page afterwards. */
    const mtr_log_t log_mode= mtr->set_log_mode(MTR_LOG_NONE);
    rec_t *insert_rec= page_cur_insert_rec_low(cursor, index, rec, offsets,
                                               mtr);
    mtr->set_log_mode(log_mode);

    if (insert_rec)
    {
      ulint pos= page_rec_get_n_recs_before(insert_rec);
      ut_ad(pos > 0);

      /* We are writing entire page images to the log.  Reduce the redo
      log volume by reorganizing the page at the same time. */
      if (page_zip_reorganize(cursor->block, index, level, mtr))
      {
        /* The page was reorganized: Seek to pos. */
        cursor->rec= pos > 1
          ? page_rec_get_nth(cursor->block->frame, pos - 1)
          : cursor->block->frame + PAGE_NEW_INFIMUM;
        insert_rec= cursor->block->frame + rec_get_next_offs(cursor->rec, 1);
        rec_offs_make_valid(insert_rec, index,
                            page_is_leaf(cursor->block->frame), offsets);
        return insert_rec;
      }

      /* Theoretically, we could try one last resort of
      page_zip_reorganize() followed by page_zip_available(), but that
      would be very unlikely to succeed. (If the full reorganized page
      failed to compress, why would it succeed to compress the page,
      plus log the insert of this record?) */

      /* Out of space: restore the page */
      if (!page_zip_decompress(page_zip, cursor->block->frame, false))
        ut_error; /* Memory corrupted? */
      ut_ad(page_validate(cursor->block->frame, index));
      insert_rec= nullptr;
    }
    return insert_rec;
  }

  free_rec= mach_read_from_2(page_free);
  if (free_rec)
  {
    /* Try to allocate from the head of the free list. */
    offset_t foffsets_[REC_OFFS_NORMAL_SIZE];
    mem_heap_t *heap= nullptr;

    rec_offs_init(foffsets_);

    offset_t *foffsets= rec_get_offsets(cursor->block->frame + free_rec, index,
                                        foffsets_,
                                        page_is_leaf(cursor->block->frame),
                                        ULINT_UNDEFINED, &heap);
    insert_buf= cursor->block->frame + free_rec -
      rec_offs_extra_size(foffsets);

    if (rec_offs_size(foffsets) < rec_size)
    {
too_small:
      if (UNIV_LIKELY_NULL(heap))
        mem_heap_free(heap);
      free_rec= 0;
      goto use_heap;
    }

    /* On compressed pages, do not relocate records from
    the free list. If extra_size would grow, use the heap. */
    const ssize_t extra_size_diff= lint(rec_offs_extra_size(offsets) -
                                        rec_offs_extra_size(foffsets));

    if (UNIV_UNLIKELY(extra_size_diff < 0))
    {
      /* Add an offset to the extra_size. */
      if (rec_offs_size(foffsets) < rec_size - ssize_t(extra_size_diff))
        goto too_small;

      insert_buf-= extra_size_diff;
    }
    else if (UNIV_UNLIKELY(extra_size_diff))
      /* Do not allow extra_size to grow */
      goto too_small;

    byte *const free_rec_ptr= cursor->block->frame + free_rec;
    heap_no= rec_get_heap_no_new(free_rec_ptr);
    int16_t next_rec= mach_read_from_2(free_rec_ptr - REC_NEXT);
    /* With innodb_page_size=64k, int16_t would be unsafe to use here,
    but that cannot be used with ROW_FORMAT=COMPRESSED. */
    static_assert(UNIV_ZIP_SIZE_SHIFT_MAX == 14, "compatibility");
    if (next_rec)
    {
      next_rec+= free_rec;
      ut_ad(int{PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES} <= next_rec);
      ut_ad(static_cast<uint16_t>(next_rec) < srv_page_size);
    }

    byte *hdr= my_assume_aligned<4>(&page_zip->data[page_free_f]);
    mach_write_to_2(hdr, static_cast<uint16_t>(next_rec));
    const byte *const garbage= my_assume_aligned<2>(page_free + 2);
    ut_ad(mach_read_from_2(garbage) >= rec_size);
    mach_write_to_2(my_assume_aligned<2>(hdr + 2),
                    mach_read_from_2(garbage) - rec_size);
    static_assert(PAGE_GARBAGE == PAGE_FREE + 2, "compatibility");
    mtr->memcpy(*cursor->block, page_free, hdr, 4);

    if (!page_is_leaf(cursor->block->frame))
    {
      /* Zero out the node pointer of free_rec, in case it will not be
      overwritten by insert_rec. */
      ut_ad(rec_size > REC_NODE_PTR_SIZE);

      if (rec_offs_size(foffsets) > rec_size)
        memset(rec_get_end(free_rec_ptr, foffsets) -
               REC_NODE_PTR_SIZE, 0, REC_NODE_PTR_SIZE);
    }
    else if (index->is_clust())
    {
      /* Zero out DB_TRX_ID,DB_ROLL_PTR in free_rec, in case they will
      not be overwritten by insert_rec. */

      ulint len;
      ulint trx_id_offs= rec_get_nth_field_offs(foffsets, index->db_trx_id(),
                                                &len);
      ut_ad(len == DATA_TRX_ID_LEN);

      if (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN + trx_id_offs +
          rec_offs_extra_size(foffsets) > rec_size)
        memset(free_rec_ptr + trx_id_offs, 0,
               DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

      ut_ad(free_rec_ptr + trx_id_offs + DATA_TRX_ID_LEN ==
            rec_get_nth_field(free_rec_ptr, foffsets, index->db_roll_ptr(),
                              &len));
      ut_ad(len == DATA_ROLL_PTR_LEN);
    }

    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);
  }
  else
  {
use_heap:
    ut_ad(!free_rec);
    insert_buf = page_mem_alloc_heap<true>(cursor->block, rec_size, &heap_no,
                                           mtr);

    if (UNIV_UNLIKELY(!insert_buf))
      return insert_buf;

    page_zip_dir_add_slot(cursor->block, index, mtr);
  }

  /* 3. Create the record */
  byte *insert_rec= rec_copy(insert_buf, rec, offsets);
  rec_offs_make_valid(insert_rec, index, page_is_leaf(cursor->block->frame),
                      offsets);

  /* 4. Insert the record in the linked list of records */
  ut_ad(cursor->rec != insert_rec);

  /* next record after current before the insertion */
  const rec_t* next_rec = page_rec_get_next_low(cursor->rec, TRUE);
  ut_ad(rec_get_status(cursor->rec) <= REC_STATUS_INFIMUM);
  ut_ad(rec_get_status(insert_rec) < REC_STATUS_INFIMUM);
  ut_ad(rec_get_status(next_rec) != REC_STATUS_INFIMUM);

  mach_write_to_2(insert_rec - REC_NEXT, static_cast<uint16_t>
                  (next_rec - insert_rec));
  mach_write_to_2(cursor->rec - REC_NEXT, static_cast<uint16_t>
                  (insert_rec - cursor->rec));
  byte *n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER +
                                     cursor->block->frame);
  mtr->write<2>(*cursor->block, n_recs, 1U + mach_read_from_2(n_recs));
  memcpy_aligned<2>(&page_zip->data[PAGE_N_RECS + PAGE_HEADER], n_recs, 2);

  /* 5. Set the n_owned field in the inserted record to zero,
  and set the heap_no field */
  rec_set_bit_field_1(insert_rec, 0, REC_NEW_N_OWNED,
                      REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
  rec_set_bit_field_2(insert_rec, heap_no, REC_NEW_HEAP_NO,
                      REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);

  UNIV_MEM_ASSERT_RW(rec_get_start(insert_rec, offsets),
                     rec_offs_size(offsets));

  /* 6. Update the last insertion info in page header */
  byte *last_insert= my_assume_aligned<4>(PAGE_LAST_INSERT + PAGE_HEADER +
                                          page_zip->data);
  const uint16_t last_insert_rec= mach_read_from_2(last_insert);
  ut_ad(!last_insert_rec ||
        rec_get_node_ptr_flag(cursor->block->frame + last_insert_rec) ==
        rec_get_node_ptr_flag(insert_rec));
  mach_write_to_2(last_insert, page_offset(insert_rec));

  if (!index->is_spatial())
  {
    byte *dir= &page_zip->data[PAGE_HEADER + PAGE_DIRECTION_B];
    ut_ad(!(*dir & ~((1U << 3) - 1)));
    byte *n= my_assume_aligned<2>
      (&page_zip->data[PAGE_HEADER + PAGE_N_DIRECTION]);
    if (UNIV_UNLIKELY(!last_insert_rec))
    {
no_direction:
      *dir= PAGE_NO_DIRECTION;
      memset(n, 0, 2);
    }
    else if (*dir != PAGE_LEFT &&
             cursor->block->frame + last_insert_rec == cursor->rec)
    {
      *dir= PAGE_RIGHT;
inc_dir:
      mach_write_to_2(n, mach_read_from_2(n) + 1);
    }
    else if (*dir != PAGE_RIGHT && page_rec_get_next(insert_rec) ==
             cursor->block->frame + last_insert_rec)
    {
      *dir= PAGE_LEFT;
      goto inc_dir;
    }
    else
      goto no_direction;
  }

  /* Write the header fields in one record. */
  mtr->memcpy(*cursor->block,
              my_assume_aligned<8>(PAGE_LAST_INSERT + PAGE_HEADER +
                                   cursor->block->frame),
              my_assume_aligned<8>(PAGE_LAST_INSERT + PAGE_HEADER +
                                   page_zip->data),
	      PAGE_N_RECS - PAGE_LAST_INSERT + 2);

  /* 7. It remains to update the owner record. */
  ulint n_owned;

  while (!(n_owned = rec_get_n_owned_new(next_rec)))
    next_rec = page_rec_get_next_low(next_rec, true);

  rec_set_bit_field_1(const_cast<rec_t*>(next_rec), n_owned + 1,
                      REC_NEW_N_OWNED, REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);

  page_zip_dir_insert(cursor, free_rec, insert_rec, mtr);

  /* 8. Now we have incremented the n_owned field of the owner
  record. If the number exceeds PAGE_DIR_SLOT_MAX_N_OWNED,
  we have to split the corresponding directory slot in two. */
  if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED))
    page_dir_split_slot<true>(cursor->block,
                              page_dir_find_owner_slot(next_rec), mtr);

  page_zip_write_rec(cursor->block, insert_rec, index, offsets, 1, mtr);
  return insert_rec;
}

/** Increment PAGE_N_DIRECTION.
@param[in,out]	block		ROW_FORMAT=COMPRESSED index page
@param[in,out]	ptr		the PAGE_DIRECTION_B field
@param[in]	dir		PAGE_RIGHT or PAGE_LEFT */
static inline void page_direction_increment(buf_block_t *block, byte *ptr,
                                            uint dir)
{
  ut_ad(ptr == PAGE_HEADER + PAGE_DIRECTION_B + block->frame);
  ut_ad(dir == PAGE_RIGHT || dir == PAGE_LEFT);
  block->page.zip.data[PAGE_HEADER + PAGE_DIRECTION_B]= *ptr= dir;
  mach_write_to_2(PAGE_HEADER + PAGE_N_DIRECTION + block->frame,
                  1U + page_header_get_field(block->frame, PAGE_N_DIRECTION));
  memcpy_aligned<2>(PAGE_HEADER + PAGE_N_DIRECTION + block->frame,
                    PAGE_HEADER + PAGE_N_DIRECTION + block->page.zip.data, 2);
}

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
static rec_t*
page_cur_parse_insert_rec_zip(
	page_cur_t*	cursor,	/*!< in/out: page cursor */
	dict_index_t*	index,	/*!< in: record descriptor */
	const rec_t*	rec,	/*!< in: pointer to a physical record */
	offset_t*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	byte*		insert_buf;
	ulint		rec_size;
	page_t*		page;		/*!< the relevant page */
	rec_t*		insert_rec;	/*!< inserted record */
	ulint		heap_no;	/*!< heap number of the inserted
					record */
	page_zip_des_t*	page_zip;

	ut_ad(!log_sys.is_physical());

	page_zip = page_cur_get_page_zip(cursor);
	ut_ad(page_zip);
	ut_ad(rec_offs_validate(rec, index, offsets));

	page = page_cur_get_page(cursor);
	ut_ad(dict_table_is_comp(index->table));
	ut_ad(page_is_comp(page));
	ut_ad(fil_page_index_page_check(page));
	ut_ad(mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID) == index->id
	      || index->is_dummy
	      || mtr->is_inside_ibuf());
	ut_ad(!page_get_instant(page));
	ut_ad(!page_cur_is_after_last(cursor));
#ifdef UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

	/* 1. Get the size of the physical record in the page */
	rec_size = rec_offs_size(offsets);

#ifdef UNIV_DEBUG_VALGRIND
	{
		const void*	rec_start
			= rec - rec_offs_extra_size(offsets);
		ulint		extra_size
			= rec_offs_extra_size(offsets)
			- (rec_offs_comp(offsets)
			   ? REC_N_NEW_EXTRA_BYTES
			   : REC_N_OLD_EXTRA_BYTES);

		/* All data bytes of the record must be valid. */
		UNIV_MEM_ASSERT_RW(rec, rec_offs_data_size(offsets));
		/* The variable-length header must be valid. */
		UNIV_MEM_ASSERT_RW(rec_start, extra_size);
	}
#endif /* UNIV_DEBUG_VALGRIND */

	const bool reorg_before_insert = page_has_garbage(page)
		&& rec_size > page_get_max_insert_size(page, 1)
		&& rec_size <= page_get_max_insert_size_after_reorganize(
			page, 1);
	constexpr uint16_t page_free_f = PAGE_FREE + PAGE_HEADER;
	byte* const page_free = my_assume_aligned<4>(page_free_f + page);
	uint16_t free_rec;

	/* 2. Try to find suitable space from page memory management */
	if (!page_zip_available(page_zip, dict_index_is_clust(index),
				rec_size, 1)
	    || reorg_before_insert) {
		/* The values can change dynamically. */
		ulint	level		= page_zip_level;
#ifdef UNIV_DEBUG
		rec_t*	cursor_rec	= page_cur_get_rec(cursor);
#endif /* UNIV_DEBUG */

		/* If we are not writing compressed page images, we
		must reorganize the page before attempting the
		insert. */
		if (recv_recovery_is_on() && !log_sys.is_physical()) {
			/* Insert into the uncompressed page only.
			The page reorganization or creation that we
			would attempt outside crash recovery would
			have been covered by a previous redo log record. */
		} else if (page_is_empty(page)) {
			ut_ad(page_cur_is_before_first(cursor));

			/* This is an empty page. Recreate it to
			get rid of the modification log. */
			page_create_zip(page_cur_get_block(cursor), index,
					page_header_get_field(page, PAGE_LEVEL),
					0, mtr);
			ut_ad(!page_header_get_ptr(page, PAGE_FREE));

			if (page_zip_available(
				    page_zip, dict_index_is_clust(index),
				    rec_size, 1)) {
				free_rec = 0;
				goto use_heap;
			}

			/* The cursor should remain on the page infimum. */
			return(NULL);
		} else if (!page_zip->m_nonempty && !page_has_garbage(page)) {
			/* The page has been freshly compressed, so
			reorganizing it will not help. */
		} else {
			ulint pos = page_rec_get_n_recs_before(cursor->rec);

			if (!page_zip_reorganize(page_cur_get_block(cursor),
						 index, level, mtr, true)) {
				ut_ad(cursor->rec == cursor_rec);
				return NULL;
			}

			if (pos) {
				cursor->rec = page_rec_get_nth(page, pos);
			} else {
				ut_ad(cursor->rec == page_get_infimum_rec(
					      page));
			}

			ut_ad(!page_header_get_ptr(page, PAGE_FREE));

			if (page_zip_available(
				    page_zip, dict_index_is_clust(index),
				    rec_size, 1)) {
				/* After reorganizing, there is space
				available. */
				free_rec = 0;
				goto use_heap;
			}
		}

		/* Try compressing the whole page afterwards. */
		const mtr_log_t log_mode = mtr->set_log_mode(MTR_LOG_NONE);
		insert_rec = page_cur_insert_rec_low(
			cursor, index, rec, offsets, mtr);
		mtr->set_log_mode(log_mode);

		/* If recovery is on, this implies that the compression
		of the page was successful during runtime. Had that not
		been the case or had the redo logging of compressed
		pages been enabled during runtime then we'd have seen
		a MLOG_ZIP_PAGE_COMPRESS redo record. Therefore, we
		know that we don't need to reorganize the page. We,
		however, do need to recompress the page. That will
		happen when the next redo record is read which must
		be of type MLOG_ZIP_PAGE_COMPRESS_NO_DATA and it must
		contain a valid compression level value.
		This implies that during recovery from this point till
		the next redo is applied the uncompressed and
		compressed versions are not identical and
		page_zip_validate will fail but that is OK because
		we call page_zip_validate only after processing
		all changes to a page under a single mtr during
		recovery. */
		if (insert_rec == NULL) {
			/* Out of space.
			This should never occur during crash recovery,
			because the MLOG_COMP_REC_INSERT should only
			be logged after a successful operation. */
			ut_ad(!recv_recovery_is_on());
			ut_ad(!index->is_dummy);
		} else if (recv_recovery_is_on() && !log_sys.is_physical()) {
			/* This should be followed by
			MLOG_ZIP_PAGE_COMPRESS_NO_DATA,
			which should succeed. */
			rec_offs_make_valid(insert_rec, index,
					    page_is_leaf(page), offsets);
		} else {
			ulint	pos = page_rec_get_n_recs_before(insert_rec);
			ut_ad(pos > 0);

			/* We are writing entire page images to the
			log.  Reduce the redo log volume by
			reorganizing the page at the same time. */
			if (page_zip_reorganize(cursor->block, index,
						level, mtr)) {
				/* The page was reorganized: Seek to pos. */
				if (pos > 1) {
					cursor->rec = page_rec_get_nth(
						page, pos - 1);
				} else {
					cursor->rec = page + PAGE_NEW_INFIMUM;
				}

				insert_rec = page + rec_get_next_offs(
					cursor->rec, TRUE);
				rec_offs_make_valid(
					insert_rec, index,
					page_is_leaf(page), offsets);
				return insert_rec;
			}

			/* Theoretically, we could try one last resort
			of btr_page_reorganize_low() followed by
			page_zip_available(), but that would be very
			unlikely to succeed. (If the full reorganized
			page failed to compress, why would it succeed
			to compress the page, plus log the insert of
			this record?) */

			/* Out of space: restore the page */
			if (!page_zip_decompress(page_zip, page, FALSE)) {
				ut_error; /* Memory corrupted? */
			}
			ut_ad(page_validate(page, index));
			insert_rec = NULL;
		}

		return(insert_rec);
	}

	free_rec = mach_read_from_2(page_free);
	if (free_rec) {
		/* Try to allocate from the head of the free list. */
		lint	extra_size_diff;
		offset_t	foffsets_[REC_OFFS_NORMAL_SIZE];
		offset_t*	foffsets	= foffsets_;
		mem_heap_t*	heap		= NULL;

		rec_offs_init(foffsets_);

		foffsets = rec_get_offsets(page + free_rec, index, foffsets,
					   page_is_leaf(page),
					   ULINT_UNDEFINED, &heap);
		if (rec_offs_size(foffsets) < rec_size) {
too_small:
			if (UNIV_LIKELY_NULL(heap)) {
				mem_heap_free(heap);
			}

			free_rec = 0;
			goto use_heap;
		}

		insert_buf = page + free_rec - rec_offs_extra_size(foffsets);

		/* On compressed pages, do not relocate records from
		the free list.  If extra_size would grow, use the heap. */
		extra_size_diff = lint(rec_offs_extra_size(offsets)
				       - rec_offs_extra_size(foffsets));

		if (UNIV_UNLIKELY(extra_size_diff < 0)) {
			/* Add an offset to the extra_size. */
			if (rec_offs_size(foffsets)
			    < rec_size - ulint(extra_size_diff)) {

				goto too_small;
			}

			insert_buf -= extra_size_diff;
		} else if (UNIV_UNLIKELY(extra_size_diff)) {
			/* Do not allow extra_size to grow */

			goto too_small;
		}

		heap_no = rec_get_heap_no_new(page + free_rec);
		int16_t next_rec = mach_read_from_2(page + free_rec - REC_NEXT);
		/* We assume that int16_t is safe to use here.
		With innodb_page_size=64k it would be unsafe,
		but that cannot be used with ROW_FORMAT=COMPRESSED. */
		static_assert(UNIV_ZIP_SIZE_SHIFT_MAX == 14, "compatibility");
		if (next_rec) {
			next_rec += free_rec;
			ut_ad(int{PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES}
			      <= next_rec);
			ut_ad(static_cast<uint16_t>(next_rec) < srv_page_size);
		}
		mtr->write<2>(*cursor->block, page_free,
			      static_cast<uint16_t>(next_rec));
		byte* garbage = my_assume_aligned<2>(page_free + 2);
		ut_ad(mach_read_from_2(garbage) >= rec_size);
		mtr->write<2>(*cursor->block, garbage,
			      mach_read_from_2(garbage) - rec_size);
		compile_time_assert(PAGE_GARBAGE == PAGE_FREE + 2);
		compile_time_assert(!((PAGE_HEADER + PAGE_FREE) % 4));
		memcpy_aligned<4>(&page_zip->data[page_free_f], page_free, 4);
		/* TODO: group with PAGE_LAST_INSERT */

		if (!page_is_leaf(page)) {
			/* Zero out the node pointer of free_rec,
			in case it will not be overwritten by
			insert_rec. */

			ut_ad(rec_size > REC_NODE_PTR_SIZE);

			if (rec_offs_extra_size(foffsets)
			    + rec_offs_data_size(foffsets) > rec_size) {

				memset(rec_get_end(page + free_rec, foffsets)
				       - REC_NODE_PTR_SIZE, 0,
				       REC_NODE_PTR_SIZE);
			}
		} else if (dict_index_is_clust(index)) {
			/* Zero out the DB_TRX_ID and DB_ROLL_PTR
			columns of free_rec, in case it will not be
			overwritten by insert_rec. */

			ulint	trx_id_offs;
			ulint	len;

			trx_id_offs = rec_get_nth_field_offs(
				foffsets, index->db_trx_id(), &len);
			ut_ad(len == DATA_TRX_ID_LEN);

			if (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN + trx_id_offs
			    + rec_offs_extra_size(foffsets) > rec_size) {
				/* We will have to zero out the
				DB_TRX_ID and DB_ROLL_PTR, because
				they will not be fully overwritten by
				insert_rec. */

				memset(page + free_rec + trx_id_offs, 0,
				       DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			}

			ut_ad(free_rec + trx_id_offs + DATA_TRX_ID_LEN
			      == rec_get_nth_field(free_rec, foffsets,
						   index->db_roll_ptr(), &len));
			ut_ad(len == DATA_ROLL_PTR_LEN);
		}

		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
	} else {
use_heap:
		ut_ad(!free_rec);
		insert_buf = page_mem_alloc_heap<true>(cursor->block, rec_size,
						       &heap_no, mtr);

		if (UNIV_UNLIKELY(insert_buf == NULL)) {
			return(NULL);
		}

		page_zip_dir_add_slot(cursor->block, index, mtr);
	}

	/* 3. Create the record */
	insert_rec = rec_copy(insert_buf, rec, offsets);
	rec_offs_make_valid(insert_rec, index, page_is_leaf(page), offsets);

	/* 4. Insert the record in the linked list of records */
	ut_ad(cursor->rec != insert_rec);

	/* next record after current before the insertion */
	const rec_t* next_rec = page_rec_get_next_low(cursor->rec, TRUE);
	ut_ad(rec_get_status(cursor->rec) <= REC_STATUS_INFIMUM);
	ut_ad(rec_get_status(insert_rec) < REC_STATUS_INFIMUM);
	ut_ad(rec_get_status(next_rec) != REC_STATUS_INFIMUM);

	mach_write_to_2(insert_rec - REC_NEXT, static_cast<uint16_t>
			(next_rec - insert_rec));
	mach_write_to_2(cursor->rec - REC_NEXT, static_cast<uint16_t>
			(insert_rec - cursor->rec));
	byte* n_recs = my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER + page);
	mtr->write<2>(*cursor->block, n_recs, 1U + mach_read_from_2(n_recs));
	memcpy_aligned<2>(&page_zip->data[PAGE_N_RECS + PAGE_HEADER], n_recs,
			  2);

	/* 5. Set the n_owned field in the inserted record to zero,
	and set the heap_no field */
	rec_set_bit_field_1(insert_rec, 0, REC_NEW_N_OWNED,
			    REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
	rec_set_bit_field_2(insert_rec, heap_no, REC_NEW_HEAP_NO,
			    REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);

	UNIV_MEM_ASSERT_RW(rec_get_start(insert_rec, offsets),
			   rec_offs_size(offsets));

	page_zip_dir_insert(cursor, free_rec, insert_rec, mtr);

	/* 6. Update the last insertion info in page header */
	byte* last_insert = my_assume_aligned<4>(PAGE_LAST_INSERT + PAGE_HEADER
						 + page);
	const uint16_t last_insert_rec = mach_read_from_2(last_insert);
	ut_ad(!last_insert_rec
	      || rec_get_node_ptr_flag(page + last_insert_rec)
	      == rec_get_node_ptr_flag(insert_rec));
	/* FIXME: combine with PAGE_DIRECTION changes */
	mtr->write<2>(*cursor->block, last_insert, page_offset(insert_rec));
	memcpy_aligned<4>(&page_zip->data[PAGE_LAST_INSERT + PAGE_HEADER],
			  last_insert, 2);

	if (!index->is_spatial()) {
		byte* ptr = PAGE_HEADER + PAGE_DIRECTION_B + page;
		if (UNIV_UNLIKELY(!last_insert_rec)) {
no_direction:
			page_zip->data[PAGE_HEADER + PAGE_DIRECTION_B] = *ptr
				= PAGE_NO_DIRECTION;
			memset_aligned<2>(PAGE_HEADER + PAGE_N_DIRECTION + page,
					  0, 2);
			memset_aligned<2>(PAGE_HEADER + PAGE_N_DIRECTION
					  + page_zip->data, 0, 2);
		} else if (page + last_insert_rec == cursor->rec
			   && page_ptr_get_direction(ptr) != PAGE_LEFT) {
			page_direction_increment(cursor->block, ptr,
						 PAGE_RIGHT);
		} else if (page_ptr_get_direction(ptr) != PAGE_RIGHT
			   && page_rec_get_next(insert_rec)
			   == page + last_insert_rec) {
			page_direction_increment(cursor->block, ptr,
						 PAGE_LEFT);
		} else {
			goto no_direction;
		}
	}

	/* 7. It remains to update the owner record. */
	ulint	n_owned;

	while (!(n_owned = rec_get_n_owned_new(next_rec))) {
		next_rec = page_rec_get_next_low(next_rec, true);
	}

	rec_set_bit_field_1(const_cast<rec_t*>(next_rec), n_owned + 1,
			    REC_NEW_N_OWNED,
			    REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);

	/* 8. Now we have incremented the n_owned field of the owner
	record. If the number exceeds PAGE_DIR_SLOT_MAX_N_OWNED,
	we have to split the corresponding directory slot in two. */
	if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED)) {
		page_dir_split_slot<true>(page_cur_get_block(cursor),
					  page_dir_find_owner_slot(next_rec),
					  mtr);
	}

	page_zip_write_rec(cursor->block, insert_rec, index, offsets, 1, mtr);
	return insert_rec;
}

/**********************************************************//**
Parses a log record of copying a record list end to a new created page.
@return end of log record or NULL */
ATTRIBUTE_COLD /* only used when crash-upgrading */
const byte*
page_parse_copy_rec_list_to_created_page(
/*=====================================*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	buf_block_t*	block,	/*!< in: page or NULL */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr or NULL */
{
	ulint		log_data_len;

	ut_ad(index->is_dummy);

	if (ptr + 4 > end_ptr) {

		return(NULL);
	}

	log_data_len = mach_read_from_4(ptr);
	ptr += 4;

	const byte* rec_end = ptr + log_data_len;

	if (rec_end > end_ptr) {

		return(NULL);
	}

	if (!block) {

		return(rec_end);
	}

	ut_ad(fil_page_index_page_check(block->frame));
	/* This function is never invoked on the clustered index root page,
	except in the redo log apply of
	page_copy_rec_list_end_to_created_page().
	For other pages, this field must be zero-initialized. */
	ut_ad(!page_get_instant(block->frame)
	      || !page_has_siblings(block->frame));

	while (ptr < rec_end) {
		ptr = page_cur_parse_insert_rec(true, ptr, end_ptr,
						block, index, mtr);
	}

	ut_a(ptr == rec_end);

	memset_aligned<2>(PAGE_HEADER + PAGE_LAST_INSERT + block->frame, 0, 2);
	if (block->page.zip.data) {
		memset_aligned<2>(PAGE_HEADER + PAGE_LAST_INSERT
				  + block->page.zip.data, 0, 2);
	}

	if (index->is_spatial()) {
		return rec_end;
	}

	block->frame[PAGE_HEADER + PAGE_DIRECTION_B] &= ~((1U << 3) - 1);
	block->frame[PAGE_HEADER + PAGE_DIRECTION_B] |= PAGE_NO_DIRECTION;
	if (block->page.zip.data) {
		block->page.zip.data[PAGE_HEADER + PAGE_DIRECTION_B]
			= PAGE_NO_DIRECTION;
	}

	return(rec_end);
}

/*************************************************************//**
Copies records from page to a newly created page, from a given record onward,
including that record. Infimum and supremum records are not copied.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if this is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit(). */
ATTRIBUTE_COLD /* only used when crash-upgrading */
void
page_copy_rec_list_end_to_created_page(
/*===================================*/
	buf_block_t*	block,		/*!< in/out: index page to copy to */
	rec_t*		rec,		/*!< in: first record to copy */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
{
	page_dir_slot_t* slot = 0; /* remove warning */
	page_t*	new_page = block->frame;
	byte*	heap_top;
	rec_t*	insert_rec = 0; /* remove warning */
	rec_t*	prev_rec;
	ulint	count;
	ulint	n_recs;
	ulint	slot_index;
	ulint	rec_size;
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	/* The record was never emitted for ROW_FORMAT=COMPRESSED pages. */
	ut_ad(!block->page.zip.data);
	ut_ad(page_dir_get_n_heap(new_page) == PAGE_HEAP_NO_USER_LOW);
	ut_ad(page_align(rec) != new_page);
	ut_ad(page_rec_is_comp(rec) == page_is_comp(new_page));
	ut_ad(fil_page_index_page_check(new_page));
	/* This function is never invoked on the clustered index root page,
	except in btr_lift_page_up(). */
	ut_ad(!page_get_instant(new_page) || !page_has_siblings(new_page));

	if (page_rec_is_infimum(rec)) {

		rec = page_rec_get_next(rec);
	}

	if (page_rec_is_supremum(rec)) {

		return;
	}

#ifdef UNIV_DEBUG
	/* To pass the debug tests we have to set these dummy values
	in the debug version */
	mach_write_to_2(PAGE_HEADER + PAGE_N_DIR_SLOTS + new_page,
			srv_page_size / 2);
	mach_write_to_2(PAGE_HEADER + PAGE_HEAP_TOP + new_page,
			srv_page_size - 1);
#endif
	prev_rec = page_get_infimum_rec(new_page);
	if (page_is_comp(new_page)) {
		heap_top = new_page + PAGE_NEW_SUPREMUM_END;
	} else {
		heap_top = new_page + PAGE_OLD_SUPREMUM_END;
	}
	count = 0;
	slot_index = 0;
	n_recs = 0;

	const bool is_leaf = page_is_leaf(new_page);

	do {
		offsets = rec_get_offsets(rec, index, offsets, is_leaf,
					  ULINT_UNDEFINED, &heap);
		insert_rec = rec_copy(heap_top, rec, offsets);

		const bool comp = page_is_comp(new_page) != 0;

		if (comp) {
			rec_set_next_offs_new(prev_rec,
					      page_offset(insert_rec));
		} else {
			rec_set_next_offs_old(prev_rec,
					      page_offset(insert_rec));
		}

		page_rec_set_n_owned<false>(block, insert_rec, 0, comp, mtr);

		rec_set_heap_no(insert_rec, PAGE_HEAP_NO_USER_LOW + n_recs,
				page_is_comp(new_page));

		count++;
		n_recs++;

		if (UNIV_UNLIKELY
		    (count == (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2)) {

			slot_index++;

			slot = page_dir_get_nth_slot(new_page, slot_index);
			mach_write_to_2(slot, page_offset(insert_rec));
			page_dir_slot_set_n_owned<false>(block, slot, count,
							 mtr);

			count = 0;
		}

		rec_size = rec_offs_size(offsets);

		ut_ad(heap_top < new_page + srv_page_size);

		heap_top += rec_size;

		rec_offs_make_valid(insert_rec, index, is_leaf, offsets);
		prev_rec = insert_rec;
		rec = page_rec_get_next(rec);
	} while (!page_rec_is_supremum(rec));

	ut_ad(n_recs);

	if ((slot_index > 0) && (count + 1
				 + (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2
				 <= PAGE_DIR_SLOT_MAX_N_OWNED)) {
		/* We can merge the two last dir slots. This operation is
		here to make this function imitate exactly the equivalent
		task made using page_cur_insert_rec, which we use in database
		recovery to reproduce the task performed by this function.
		To be able to check the correctness of recovery, it is good
		that it imitates exactly. */

		count += (PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2;

		page_dir_slot_set_n_owned<false>(block, slot, 0, mtr);

		slot_index--;
	}

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}

	slot = page_dir_get_nth_slot(new_page, 1 + slot_index);

	if (page_is_comp(new_page)) {
		rec_set_next_offs_new(insert_rec, PAGE_NEW_SUPREMUM);
		mach_write_to_2(slot, PAGE_NEW_SUPREMUM);
		rec_set_bit_field_1(new_page + PAGE_NEW_SUPREMUM, count + 1,
				    REC_NEW_N_OWNED,
				    REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
	} else {
		rec_set_next_offs_old(insert_rec, PAGE_OLD_SUPREMUM);
		mach_write_to_2(slot, PAGE_OLD_SUPREMUM);
		rec_set_bit_field_1(new_page + PAGE_OLD_SUPREMUM, count + 1,
				    REC_OLD_N_OWNED,
				    REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
	}

	mach_write_to_2(PAGE_HEADER + PAGE_N_DIR_SLOTS + new_page,
			2 + slot_index);
	mach_write_to_2(PAGE_HEADER + PAGE_HEAP_TOP + new_page,
			page_offset(heap_top));
	mach_write_to_2(PAGE_HEADER + PAGE_N_HEAP + new_page,
			PAGE_HEAP_NO_USER_LOW + n_recs);
	mach_write_to_2(PAGE_HEADER + PAGE_N_RECS + new_page, n_recs);

	memset_aligned<2>(PAGE_HEADER + PAGE_LAST_INSERT + new_page, 0, 2);
	mach_write_to_1(PAGE_HEADER + PAGE_DIRECTION_B + new_page,
			(mach_read_from_1(PAGE_HEADER + PAGE_DIRECTION_B
					  + new_page) & ~((1U << 3) - 1))
			| PAGE_NO_DIRECTION);
	memset_aligned<2>(PAGE_HEADER + PAGE_N_DIRECTION + new_page, 0, 2);
}

/***********************************************************//**
Parses log record of a record delete on a page.
@return pointer to record end or NULL */
ATTRIBUTE_COLD /* only used when crash-upgrading */
const byte*
page_cur_parse_delete_rec(
/*======================*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	buf_block_t*	block,	/*!< in: page or NULL */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction,
				or NULL if block=NULL */
{
	ulint		offset;
	page_cur_t	cursor;

	ut_ad(!block == !mtr);

	if (end_ptr < ptr + 2) {

		return(NULL);
	}

	/* Read the cursor rec offset as a 2-byte ulint */
	offset = mach_read_from_2(ptr);
	ptr += 2;

	if (UNIV_UNLIKELY(offset >= srv_page_size)) {
		recv_sys.found_corrupt_log = true;
		return NULL;
	}

	if (block) {
		page_t*		page		= buf_block_get_frame(block);
		mem_heap_t*	heap		= NULL;
		offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
		rec_t*		rec		= page + offset;
		rec_offs_init(offsets_);

		page_cur_position(rec, block, &cursor);
		ut_ad(!buf_block_get_page_zip(block) || page_is_comp(page));

		page_cur_delete_rec(&cursor, index,
				    rec_get_offsets(rec, index, offsets_,
						    page_rec_is_leaf(rec),
						    ULINT_UNDEFINED, &heap),
				    mtr);
		if (UNIV_LIKELY_NULL(heap)) {
			mem_heap_free(heap);
		}
	}

	return(ptr);
}

/** Prepend a record to the PAGE_FREE list.
@param[in,out]  block   index page
@param[in,out]  rec     record being deleted
@param[in]      index   the index that the page belongs to
@param[in]      offsets rec_get_offsets(rec, index)
@param[in,out]  mtr     mini-transaction */
static void page_mem_free(buf_block_t *block, rec_t *rec,
                          const dict_index_t *index, const offset_t *offsets,
                          mtr_t *mtr)
{
  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(page_align(rec) == block->frame);
  const rec_t *free= page_header_get_ptr(block->frame, PAGE_FREE);

  if (UNIV_LIKELY_NULL(block->page.zip.data))
    page_zip_dir_delete(block, rec, index, offsets, free, mtr);
  else
  {
    if (srv_immediate_scrub_data_uncompressed)
      mtr->memset(block, page_offset(rec), rec_offs_data_size(offsets), 0);

    uint16_t next= free
      ? (page_is_comp(block->frame)
         ? static_cast<uint16_t>(free - rec)
         : static_cast<uint16_t>(page_offset(free)))
      : 0;
    mtr->write<2>(*block, rec - REC_NEXT, next);
    mtr->write<2>(*block, PAGE_FREE + PAGE_HEADER + block->frame,
                  page_offset(rec));
    mtr->write<2>(*block, PAGE_GARBAGE + PAGE_HEADER + block->frame,
                  rec_offs_size(offsets)
                  + page_header_get_field(block->frame, PAGE_GARBAGE));
    mtr->write<2>(*block, PAGE_N_RECS + PAGE_HEADER + block->frame,
                  ulint(page_get_n_recs(block->frame)) - 1);
  }
}

/***********************************************************//**
Deletes a record at the page cursor. The cursor is moved to the next
record after the deleted one. */
void
page_cur_delete_rec(
/*================*/
	page_cur_t*		cursor,	/*!< in/out: a page cursor */
	const dict_index_t*	index,	/*!< in: record descriptor */
	const offset_t*		offsets,/*!< in: rec_get_offsets(
					cursor->rec, index) */
	mtr_t*			mtr)	/*!< in/out: mini-transaction */
{
	page_dir_slot_t* cur_dir_slot;
	page_dir_slot_t* prev_slot;
	rec_t*		current_rec;
	rec_t*		prev_rec	= NULL;
	rec_t*		next_rec;
	ulint		cur_slot_no;
	ulint		cur_n_owned;
	rec_t*		rec;

	/* page_zip_validate() will fail here when
	btr_cur_pessimistic_delete() invokes btr_set_min_rec_mark().
	Then, both "page_zip" and "block->frame" would have the min-rec-mark
	set on the smallest user record, but "block->frame" would additionally
	have it set on the smallest-but-one record.  Because sloppy
	page_zip_validate_low() only ignores min-rec-flag differences
	in the smallest user record, it cannot be used here either. */

	current_rec = cursor->rec;
	buf_block_t* const block = cursor->block;
	ut_ad(rec_offs_validate(current_rec, index, offsets));
	ut_ad(!!page_is_comp(block->frame) == index->table->not_redundant());
	ut_ad(fil_page_index_page_check(block->frame));
	ut_ad(mach_read_from_8(PAGE_HEADER + PAGE_INDEX_ID + block->frame)
	      == index->id
	      || index->is_dummy
	      || mtr->is_inside_ibuf());
	ut_ad(mtr->is_named_space(index->table->space));

	/* The record must not be the supremum or infimum record. */
	ut_ad(page_rec_is_user_rec(current_rec));

	if (page_get_n_recs(block->frame) == 1
	    /* Empty the page, unless we are applying the redo log
	    during crash recovery. During normal operation, the
	    page_create_empty() gets logged as one of MLOG_PAGE_CREATE,
	    MLOG_COMP_PAGE_CREATE, MLOG_ZIP_PAGE_COMPRESS. */
	    && !recv_recovery_is_on() && !log_sys.is_physical()
	    && !rec_is_alter_metadata(current_rec, *index)) {
		/* Empty the page. */
		ut_ad(page_is_leaf(block->frame));
		/* Usually, this should be the root page,
		and the whole index tree should become empty.
		However, this could also be a call in
		btr_cur_pessimistic_update() to delete the only
		record in the page and to insert another one. */
		page_cur_move_to_next(cursor);
		ut_ad(page_cur_is_after_last(cursor));
		page_create_empty(page_cur_get_block(cursor),
				  const_cast<dict_index_t*>(index), mtr);
		return;
	}

	/* Save to local variables some data associated with current_rec */
	cur_slot_no = page_dir_find_owner_slot(current_rec);
	ut_ad(cur_slot_no > 0);
	cur_dir_slot = page_dir_get_nth_slot(block->frame, cur_slot_no);
	cur_n_owned = page_dir_slot_get_n_owned(cur_dir_slot);

	/* 1. Reset the last insert info in the page header and increment
	the modify clock for the frame */
	page_header_reset_last_insert(block, mtr);

	/* The page gets invalid for btr_pcur_restore_pos().
	We avoid invoking buf_block_modify_clock_inc(block) because its
	consistency checks would fail for the dummy block that is being
	used during IMPORT TABLESPACE. */
	block->modify_clock++;

	/* 2. Find the next and the previous record. Note that the cursor is
	left at the next record. */

	ut_ad(cur_slot_no > 0);
	prev_slot = page_dir_get_nth_slot(block->frame, cur_slot_no - 1);

	rec = const_cast<rec_t*>(page_dir_slot_get_rec(prev_slot));

	/* rec now points to the record of the previous directory slot. Look
	for the immediate predecessor of current_rec in a loop. */

	while (current_rec != rec) {
		prev_rec = rec;
		rec = page_rec_get_next(rec);
	}

	page_cur_move_to_next(cursor);
	next_rec = cursor->rec;

	/* 3. Remove the record from the linked list of records */
	/* 4. If the deleted record is pointed to by a dir slot, update the
	record pointer in slot. In the following if-clause we assume that
	prev_rec is owned by the same slot, i.e., PAGE_DIR_SLOT_MIN_N_OWNED
	>= 2. */
	/* 5. Update the number of owned records of the slot */

	compile_time_assert(PAGE_DIR_SLOT_MIN_N_OWNED >= 2);
	ut_ad(cur_n_owned > 1);

	rec_t* slot_rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(cur_dir_slot));

	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		ut_ad(page_is_comp(block->frame));
		if (current_rec == slot_rec) {
			page_zip_rec_set_owned(block, prev_rec, 1, mtr);
			page_zip_rec_set_owned(block, slot_rec, 0, mtr);
			slot_rec = prev_rec;
			mach_write_to_2(cur_dir_slot, page_offset(slot_rec));
		} else if (cur_n_owned == 1
			   && !page_rec_is_supremum(slot_rec)) {
			page_zip_rec_set_owned(block, slot_rec, 0, mtr);
		}

		mach_write_to_2(prev_rec - REC_NEXT, static_cast<uint16_t>
				(next_rec - prev_rec));
		mach_write_to_1(slot_rec - REC_NEW_N_OWNED,
				(slot_rec[-REC_NEW_N_OWNED]
				 & ~REC_N_OWNED_MASK)
				| (cur_n_owned - 1) << REC_N_OWNED_SHIFT);
	} else {
		if (current_rec == slot_rec) {
			slot_rec = prev_rec;
			mtr->write<2>(*block, cur_dir_slot,
				      page_offset(slot_rec));
		}

		if (page_is_comp(block->frame)) {
			mtr->write<2>(*block, prev_rec - REC_NEXT,
				      static_cast<uint16_t>
				      (next_rec - prev_rec));
			mtr->write<1>(*block, slot_rec - REC_NEW_N_OWNED,
				      (slot_rec[-REC_NEW_N_OWNED]
				       & ~REC_N_OWNED_MASK)
				      | (cur_n_owned - 1)
				      << REC_N_OWNED_SHIFT);
		} else {
			mtr->write<2>(*block, prev_rec - REC_NEXT,
				      page_offset(next_rec));
			mtr->write<1>(*block, slot_rec - REC_OLD_N_OWNED,
				      (slot_rec[-REC_OLD_N_OWNED]
				       & ~REC_N_OWNED_MASK)
				      | (cur_n_owned - 1)
				      << REC_N_OWNED_SHIFT);
		}
	}

	/* 6. Free the memory occupied by the record */
	page_mem_free(block, current_rec, index, offsets, mtr);

	/* 7. Now we have decremented the number of owned records of the slot.
	If the number drops below PAGE_DIR_SLOT_MIN_N_OWNED, we balance the
	slots. */

	if (cur_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED) {
		page_dir_balance_slot(block, cur_slot_no, mtr);
	}
}

#ifdef UNIV_COMPILE_TEST_FUNCS

/*******************************************************************//**
Print the first n numbers, generated by ut_rnd_gen() to make sure
(visually) that it works properly. */
void
test_ut_rnd_gen(
	int	n)	/*!< in: print first n numbers */
{
	int			i;
	unsigned long long	rnd;

	for (i = 0; i < n; i++) {
		rnd = ut_rnd_gen();
		printf("%llu\t%%2=%llu %%3=%llu %%5=%llu %%7=%llu %%11=%llu\n",
		       rnd,
		       rnd % 2,
		       rnd % 3,
		       rnd % 5,
		       rnd % 7,
		       rnd % 11);
	}
}

#endif /* UNIV_COMPILE_TEST_FUNCS */
