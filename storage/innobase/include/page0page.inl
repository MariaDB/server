/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2022, MariaDB Corporation.

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
@file include/page0page.ic
Index page routines

Created 2/2/1994 Heikki Tuuri
*******************************************************/

#ifndef UNIV_INNOCHECKSUM
#include "rem0cmp.h"
#include "mtr0log.h"
#include "page0zip.h"

/*************************************************************//**
Sets the max trx id field value if trx_id is bigger than the previous
value. */
UNIV_INLINE
void
page_update_max_trx_id(
/*===================*/
	buf_block_t*	block,	/*!< in/out: page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page whose
				uncompressed part will be updated, or NULL */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(block);
	ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
	ut_ad(trx_id);
	page_t* page = block->page.frame;
	ut_ad(page_is_leaf(page));

	if (page_get_max_trx_id(page) < trx_id) {
		page_set_max_trx_id(block, page, page_zip, trx_id, mtr);
	}
}

/*************************************************************//**
Returns the RTREE SPLIT SEQUENCE NUMBER (FIL_RTREE_SPLIT_SEQ_NUM).
@return	SPLIT SEQUENCE NUMBER */
UNIV_INLINE
node_seq_t
page_get_ssn_id(
/*============*/
	const page_t*	page)	/*!< in: page */
{
	ut_ad(page);

	return(static_cast<node_seq_t>(
		mach_read_from_8(page + FIL_RTREE_SPLIT_SEQ_NUM)));
}

/*************************************************************//**
Sets the RTREE SPLIT SEQUENCE NUMBER field value */
UNIV_INLINE
void
page_set_ssn_id(
/*============*/
	buf_block_t*	block,	/*!< in/out: page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page whose
				uncompressed part will be updated, or NULL */
	node_seq_t	ssn_id,	/*!< in: transaction id */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_SX_FIX |
                                   MTR_MEMO_PAGE_X_FIX));
  ut_ad(!page_zip || page_zip == &block->page.zip);
  constexpr uint16_t field= FIL_RTREE_SPLIT_SEQ_NUM;
  byte *b= my_assume_aligned<2>(&block->page.frame[field]);
  if (mtr->write<8,mtr_t::MAYBE_NOP>(*block, b, ssn_id) &&
      UNIV_LIKELY_NULL(page_zip))
    memcpy_aligned<2>(&page_zip->data[field], b, 8);
}

#endif /* !UNIV_INNOCHECKSUM */

#ifndef UNIV_INNOCHECKSUM
/*************************************************************//**
Returns the offset stored in the given header field.
@return offset from the start of the page, or 0 */
UNIV_INLINE
uint16_t
page_header_get_offs(
/*=================*/
	const page_t*	page,	/*!< in: page */
	ulint		field)	/*!< in: PAGE_FREE, ... */
{
	ut_ad((field == PAGE_FREE)
	      || (field == PAGE_LAST_INSERT)
	      || (field == PAGE_HEAP_TOP));

	uint16_t offs = page_header_get_field(page, field);

	ut_ad((field != PAGE_HEAP_TOP) || offs);

	return(offs);
}


/**
Reset PAGE_LAST_INSERT.
@param[in,out]  block    file page
@param[in,out]  page     file page frame
@param[in,out]  mtr      mini-transaction */
inline void page_header_reset_last_insert(buf_block_t *block, page_t *page,
                                          mtr_t *mtr)
{
  constexpr uint16_t field= PAGE_HEADER + PAGE_LAST_INSERT;
  byte *b= my_assume_aligned<2>(&page[field]);
  if (mtr->write<2,mtr_t::MAYBE_NOP>(*block, b, 0U) &&
      UNIV_LIKELY_NULL(block->page.zip.data))
    memset_aligned<2>(&block->page.zip.data[field], 0, 2);
}

/************************************************************//**
Returns the middle record of the records on the page. If there is an
even number of records in the list, returns the first record of the
upper half-list.
@return middle record */
UNIV_INLINE
rec_t*
page_get_middle_rec(
/*================*/
	page_t*	page)	/*!< in: page */
{
	ulint	middle = (ulint(page_get_n_recs(page))
			  + PAGE_HEAP_NO_USER_LOW) / 2;

	return(page_rec_get_nth(page, middle));
}

#endif /* !UNIV_INNOCHECKSUM */

/*************************************************************//**
Gets the page number.
@return page number */
UNIV_INLINE
uint32_t
page_get_page_no(
/*=============*/
	const page_t*	page)	/*!< in: page */
{
  ut_ad(page == page_align((page_t*) page));
  return mach_read_from_4(my_assume_aligned<4>(page + FIL_PAGE_OFFSET));
}

#ifndef UNIV_INNOCHECKSUM
/*************************************************************//**
Gets the tablespace identifier.
@return space id */
UNIV_INLINE
uint32_t
page_get_space_id(
/*==============*/
	const page_t*	page)	/*!< in: page */
{
  ut_ad(page == page_align((page_t*) page));
  return mach_read_from_4(my_assume_aligned<2>
                          (page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID));
}

#endif /* !UNIV_INNOCHECKSUM */

/*************************************************************//**
Gets the number of user records on page (infimum and supremum records
are not user records).
@return number of user records */
UNIV_INLINE
uint16_t
page_get_n_recs(
/*============*/
	const page_t*	page)	/*!< in: index page */
{
	return(page_header_get_field(page, PAGE_N_RECS));
}

#ifndef UNIV_INNOCHECKSUM
/*************************************************************//**
Gets the number of dir slots in directory.
@return number of slots */
UNIV_INLINE
uint16_t
page_dir_get_n_slots(
/*=================*/
	const page_t*	page)	/*!< in: index page */
{
	return(page_header_get_field(page, PAGE_N_DIR_SLOTS));
}

/*************************************************************//**
Gets the number of records in the heap.
@return number of user records */
UNIV_INLINE
uint16_t
page_dir_get_n_heap(
/*================*/
	const page_t*	page)	/*!< in: index page */
{
	return(page_header_get_field(page, PAGE_N_HEAP) & 0x7fff);
}

/************************************************************//**
Calculates the space reserved for directory slots of a given number of
records. The exact value is a fraction number n * PAGE_DIR_SLOT_SIZE /
PAGE_DIR_SLOT_MIN_N_OWNED, and it is rounded upwards to an integer. */
UNIV_INLINE
ulint
page_dir_calc_reserved_space(
/*=========================*/
	ulint	n_recs)		/*!< in: number of records */
{
	return((PAGE_DIR_SLOT_SIZE * n_recs + PAGE_DIR_SLOT_MIN_N_OWNED - 1)
	       / PAGE_DIR_SLOT_MIN_N_OWNED);
}

#endif /* UNIV_INNOCHECKSUM */

/************************************************************//**
Returns the sum of the sizes of the records in the record list, excluding
the infimum and supremum records.
@return data in bytes */
UNIV_INLINE
uint16_t
page_get_data_size(
/*===============*/
	const page_t*	page)	/*!< in: index page */
{
	unsigned ret = page_header_get_field(page, PAGE_HEAP_TOP)
		- (page_is_comp(page)
		   ? PAGE_NEW_SUPREMUM_END
		   : PAGE_OLD_SUPREMUM_END)
		- page_header_get_field(page, PAGE_GARBAGE);
	ut_ad(ret < srv_page_size);
	return static_cast<uint16_t>(ret);
}

#ifndef UNIV_INNOCHECKSUM
/*************************************************************//**
Calculates free space if a page is emptied.
@return free space */
UNIV_INLINE
ulint
page_get_free_space_of_empty(
/*=========================*/
	ulint	comp)		/*!< in: nonzero=compact page layout */
{
	if (comp) {
		return((ulint)(srv_page_size
			       - PAGE_NEW_SUPREMUM_END
			       - PAGE_DIR
			       - 2 * PAGE_DIR_SLOT_SIZE));
	}

	return((ulint)(srv_page_size
		       - PAGE_OLD_SUPREMUM_END
		       - PAGE_DIR
		       - 2 * PAGE_DIR_SLOT_SIZE));
}

/************************************************************//**
Each user record on a page, and also the deleted user records in the heap
takes its size plus the fraction of the dir cell size /
PAGE_DIR_SLOT_MIN_N_OWNED bytes for it. If the sum of these exceeds the
value of page_get_free_space_of_empty, the insert is impossible, otherwise
it is allowed. This function returns the maximum combined size of records
which can be inserted on top of the record heap.
@return maximum combined size for inserted records */
UNIV_INLINE
ulint
page_get_max_insert_size(
/*=====================*/
	const page_t*	page,	/*!< in: index page */
	ulint		n_recs)	/*!< in: number of records */
{
	ulint	occupied;
	ulint	free_space;

	if (page_is_comp(page)) {
		occupied = page_header_get_field(page, PAGE_HEAP_TOP)
			- PAGE_NEW_SUPREMUM_END
			+ page_dir_calc_reserved_space(
				n_recs + page_dir_get_n_heap(page) - 2);

		free_space = page_get_free_space_of_empty(TRUE);
	} else {
		occupied = page_header_get_field(page, PAGE_HEAP_TOP)
			- PAGE_OLD_SUPREMUM_END
			+ page_dir_calc_reserved_space(
				n_recs + page_dir_get_n_heap(page) - 2);

		free_space = page_get_free_space_of_empty(FALSE);
	}

	/* Above the 'n_recs +' part reserves directory space for the new
	inserted records; the '- 2' excludes page infimum and supremum
	records */

	if (occupied > free_space) {

		return(0);
	}

	return(free_space - occupied);
}

/************************************************************//**
Returns the maximum combined size of records which can be inserted on top
of the record heap if a page is first reorganized.
@return maximum combined size for inserted records */
UNIV_INLINE
ulint
page_get_max_insert_size_after_reorganize(
/*======================================*/
	const page_t*	page,	/*!< in: index page */
	ulint		n_recs)	/*!< in: number of records */
{
	ulint	occupied;
	ulint	free_space;

	occupied = page_get_data_size(page)
		+ page_dir_calc_reserved_space(n_recs + page_get_n_recs(page));

	free_space = page_get_free_space_of_empty(page_is_comp(page));

	if (occupied > free_space) {

		return(0);
	}

	return(free_space - occupied);
}

/** Read the PAGE_DIRECTION field from a byte.
@param[in]	ptr	pointer to PAGE_DIRECTION_B
@return	the value of the PAGE_DIRECTION field */
inline
byte
page_ptr_get_direction(const byte* ptr)
{
	ut_ad(page_offset(ptr) == PAGE_HEADER + PAGE_DIRECTION_B);
	return *ptr & ((1U << 3) - 1);
}

/** Read the PAGE_INSTANT field.
@param[in]	page	index page
@return the value of the PAGE_INSTANT field */
inline
uint16_t
page_get_instant(const page_t* page)
{
	uint16_t i = page_header_get_field(page, PAGE_INSTANT);
#ifdef UNIV_DEBUG
	switch (fil_page_get_type(page)) {
	case FIL_PAGE_TYPE_INSTANT:
		ut_ad(page_get_direction(page) <= PAGE_NO_DIRECTION);
		ut_ad(i >> 3);
		break;
	case FIL_PAGE_INDEX:
		ut_ad(i <= PAGE_NO_DIRECTION || !page_is_comp(page));
		break;
	case FIL_PAGE_RTREE:
		ut_ad(i <= PAGE_NO_DIRECTION);
		break;
	default:
		ut_ad("invalid page type" == 0);
		break;
	}
#endif /* UNIV_DEBUG */
	return static_cast<uint16_t>(i >> 3);  /* i / 8 */
}
#endif /* !UNIV_INNOCHECKSUM */
