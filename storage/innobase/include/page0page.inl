/*****************************************************************************

Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
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
@file include/page0page.ic
Index page routines

Created 2/2/1994 Heikki Tuuri
*******************************************************/

#ifndef page0page_ic
#define page0page_ic

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
	ut_ad(page_is_leaf(buf_block_get_frame(block)));

	if (page_get_max_trx_id(buf_block_get_frame(block)) < trx_id) {

		page_set_max_trx_id(block, page_zip, trx_id, mtr);
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
@param[in,out]  mtr      mini-transaction */
inline void page_header_reset_last_insert(buf_block_t *block, mtr_t *mtr)
{
  constexpr uint16_t field= PAGE_HEADER + PAGE_LAST_INSERT;
  byte *b= my_assume_aligned<2>(&block->page.frame[field]);
  if (mtr->write<2,mtr_t::MAYBE_NOP>(*block, b, 0U) &&
      UNIV_LIKELY_NULL(block->page.zip.data))
    memset_aligned<2>(&block->page.zip.data[field], 0, 2);
}

/***************************************************************//**
Returns the heap number of a record.
@return heap number */
UNIV_INLINE
ulint
page_rec_get_heap_no(
/*=================*/
	const rec_t*	rec)	/*!< in: the physical record */
{
	if (page_rec_is_comp(rec)) {
		return(rec_get_heap_no_new(rec));
	} else {
		return(rec_get_heap_no_old(rec));
	}
}

/** Determine whether an index page record is a user record.
@param[in]	rec	record in an index page
@return true if a user record */
inline
bool
page_rec_is_user_rec(const rec_t* rec)
{
	ut_ad(page_rec_check(rec));
	return(page_rec_is_user_rec_low(page_offset(rec)));
}

/** Determine whether an index page record is the supremum record.
@param[in]	rec	record in an index page
@return true if the supremum record */
inline
bool
page_rec_is_supremum(const rec_t* rec)
{
	ut_ad(page_rec_check(rec));
	return(page_rec_is_supremum_low(page_offset(rec)));
}

/** Determine whether an index page record is the infimum record.
@param[in]	rec	record in an index page
@return true if the infimum record */
inline
bool
page_rec_is_infimum(const rec_t* rec)
{
	ut_ad(page_rec_check(rec));
	return(page_rec_is_infimum_low(page_offset(rec)));
}

/************************************************************//**
true if the record is the first user record on a page.
@return true if the first user record */
UNIV_INLINE
bool
page_rec_is_first(
/*==============*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
{
	ut_ad(page_get_n_recs(page) > 0);

	return(page_rec_get_next_const(page_get_infimum_rec(page)) == rec);
}

/************************************************************//**
true if the record is the second user record on a page.
@return true if the second user record */
UNIV_INLINE
bool
page_rec_is_second(
/*===============*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
{
	ut_ad(page_get_n_recs(page) > 1);

	return(page_rec_get_next_const(
		page_rec_get_next_const(page_get_infimum_rec(page))) == rec);
}

/************************************************************//**
true if the record is the last user record on a page.
@return true if the last user record */
UNIV_INLINE
bool
page_rec_is_last(
/*=============*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
{
	ut_ad(page_get_n_recs(page) > 0);

	return(page_rec_get_next_const(rec) == page_get_supremum_rec(page));
}

/************************************************************//**
true if distance between the records (measured in number of times we have to
move to the next record) is at most the specified value */
UNIV_INLINE
bool
page_rec_distance_is_at_most(
/*=========================*/
	const rec_t*	left_rec,
	const rec_t*	right_rec,
	ulint		val)
{
	for (ulint i = 0; i <= val; i++) {
		if (left_rec == right_rec) {
			return (true);
		}
		left_rec = page_rec_get_next_const(left_rec);
	}
	return (false);
}

/************************************************************//**
true if the record is the second last user record on a page.
@return true if the second last user record */
UNIV_INLINE
bool
page_rec_is_second_last(
/*====================*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
{
	ut_ad(page_get_n_recs(page) > 1);
	ut_ad(!page_rec_is_last(rec, page));

	return(page_rec_get_next_const(
		page_rec_get_next_const(rec)) == page_get_supremum_rec(page));
}

/************************************************************//**
Returns the nth record of the record list.
This is the inverse function of page_rec_get_n_recs_before().
@return nth record */
UNIV_INLINE
rec_t*
page_rec_get_nth(
/*=============*/
	page_t*	page,	/*!< in: page */
	ulint	nth)	/*!< in: nth record */
{
	return((rec_t*) page_rec_get_nth_const(page, nth));
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

/**************************************************************//**
Used to check the consistency of a record on a page.
@return TRUE if succeed */
UNIV_INLINE
ibool
page_rec_check(
/*===========*/
	const rec_t*	rec)	/*!< in: record */
{
	const page_t*	page = page_align(rec);

	ut_a(rec);

	ut_a(page_offset(rec) <= page_header_get_field(page, PAGE_HEAP_TOP));
	ut_a(page_offset(rec) >= PAGE_DATA);

	return(TRUE);
}

/***************************************************************//**
Gets the number of records owned by a directory slot.
@return number of records */
UNIV_INLINE
ulint
page_dir_slot_get_n_owned(
/*======================*/
	const page_dir_slot_t*	slot)	/*!< in: page directory slot */
{
	const rec_t*	rec	= page_dir_slot_get_rec(slot);
	if (page_rec_is_comp(slot)) {
		return(rec_get_n_owned_new(rec));
	} else {
		return(rec_get_n_owned_old(rec));
	}
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

/************************************************************//**
Gets the pointer to the next record on the page.
@return pointer to next record */
UNIV_INLINE
const rec_t*
page_rec_get_next_low(
/*==================*/
	const rec_t*	rec,	/*!< in: pointer to record */
	ulint		comp)	/*!< in: nonzero=compact page layout */
{
	ulint		offs;
	const page_t*	page;

	ut_ad(page_rec_check(rec));

	page = page_align(rec);

	offs = rec_get_next_offs(rec, comp);

	if (offs >= srv_page_size) {
		fprintf(stderr,
			"InnoDB: Next record offset is nonsensical %lu"
			" in record at offset %lu\n"
			"InnoDB: rec address %p, space id %lu, page %lu\n",
			(ulong) offs, (ulong) page_offset(rec),
			(void*) rec,
			(ulong) page_get_space_id(page),
			(ulong) page_get_page_no(page));
		ut_error;
	} else if (offs == 0) {

		return(NULL);
	}

	ut_ad(page_rec_is_infimum(rec)
	      || (!page_is_leaf(page) && !page_has_prev(page))
	      || !(rec_get_info_bits(page + offs, comp)
		   & REC_INFO_MIN_REC_FLAG));

	return(page + offs);
}

/************************************************************//**
Gets the pointer to the next record on the page.
@return pointer to next record */
UNIV_INLINE
rec_t*
page_rec_get_next(
/*==============*/
	rec_t*	rec)	/*!< in: pointer to record */
{
	return((rec_t*) page_rec_get_next_low(rec, page_rec_is_comp(rec)));
}

/************************************************************//**
Gets the pointer to the next record on the page.
@return pointer to next record */
UNIV_INLINE
const rec_t*
page_rec_get_next_const(
/*====================*/
	const rec_t*	rec)	/*!< in: pointer to record */
{
	return(page_rec_get_next_low(rec, page_rec_is_comp(rec)));
}

/************************************************************//**
Gets the pointer to the next non delete-marked record on the page.
If all subsequent records are delete-marked, then this function
will return the supremum record.
@return pointer to next non delete-marked record or pointer to supremum */
UNIV_INLINE
const rec_t*
page_rec_get_next_non_del_marked(
/*=============================*/
	const rec_t*	rec)	/*!< in: pointer to record */
{
	const rec_t*	r;
	ulint		page_is_compact = page_rec_is_comp(rec);

	for (r = page_rec_get_next_const(rec);
	     !page_rec_is_supremum(r)
	     && rec_get_deleted_flag(r, page_is_compact);
	     r = page_rec_get_next_const(r)) {
		/* noop */
	}

	return(r);
}

/************************************************************//**
Gets the pointer to the previous record.
@return pointer to previous record */
UNIV_INLINE
const rec_t*
page_rec_get_prev_const(
/*====================*/
	const rec_t*	rec)	/*!< in: pointer to record, must not be page
				infimum */
{
	const page_dir_slot_t*	slot;
	ulint			slot_no;
	const rec_t*		rec2;
	const rec_t*		prev_rec = NULL;
	const page_t*		page;

	ut_ad(page_rec_check(rec));

	page = page_align(rec);

	ut_ad(!page_rec_is_infimum(rec));

	slot_no = page_dir_find_owner_slot(rec);

	ut_a(slot_no != 0);

	slot = page_dir_get_nth_slot(page, slot_no - 1);

	rec2 = page_dir_slot_get_rec(slot);

	if (page_is_comp(page)) {
		while (rec != rec2) {
			prev_rec = rec2;
			rec2 = page_rec_get_next_low(rec2, TRUE);
		}
	} else {
		while (rec != rec2) {
			prev_rec = rec2;
			rec2 = page_rec_get_next_low(rec2, FALSE);
		}
	}

	ut_a(prev_rec);

	return(prev_rec);
}

/************************************************************//**
Gets the pointer to the previous record.
@return pointer to previous record */
UNIV_INLINE
rec_t*
page_rec_get_prev(
/*==============*/
	rec_t*	rec)	/*!< in: pointer to record, must not be page
			infimum */
{
	return((rec_t*) page_rec_get_prev_const(rec));
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

#endif
