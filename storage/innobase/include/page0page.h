/*****************************************************************************
Copyright (c) 1994, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2021, MariaDB Corporation.

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
@file include/page0page.h
Index page routines

Created 2/2/1994 Heikki Tuuri
*******************************************************/

#ifndef page0page_h
#define page0page_h

#include "page0types.h"
#include "fsp0fsp.h"
#include "fil0fil.h"
#include "buf0buf.h"
#include "rem0rec.h"
#include "mach0data.h"
#ifndef UNIV_INNOCHECKSUM
#include "dict0dict.h"
#include "data0data.h"
#include "mtr0mtr.h"

/*			PAGE HEADER
			===========

Index page header starts at the first offset left free by the FIL-module */

typedef	byte		page_header_t;
#endif /* !UNIV_INNOCHECKSUM */

#define	PAGE_HEADER	FSEG_PAGE_DATA	/* index page header starts at this
				offset */
/*-----------------------------*/
#define PAGE_N_DIR_SLOTS 0	/* number of slots in page directory */
#define	PAGE_HEAP_TOP	 2	/* pointer to record heap top */
#define	PAGE_N_HEAP	 4	/* number of records in the heap,
				bit 15=flag: new-style compact page format */
#define	PAGE_FREE	 6	/* pointer to start of page free record list */
#define	PAGE_GARBAGE	 8	/* number of bytes in deleted records */
#define	PAGE_LAST_INSERT 10	/* pointer to the last inserted record, or
				0 if this info has been reset by a delete,
				for example */

/** This 10-bit field is usually 0. In B-tree index pages of
ROW_FORMAT=REDUNDANT tables, this byte can contain garbage if the .ibd
file was created in MySQL 4.1.0 or if the table resides in the system
tablespace and was created before MySQL 4.1.1 or MySQL 4.0.14.
In this case, the FIL_PAGE_TYPE would be FIL_PAGE_INDEX.

In ROW_FORMAT=COMPRESSED tables, this field is always 0, because
instant ADD COLUMN is not supported.

In ROW_FORMAT=COMPACT and ROW_FORMAT=DYNAMIC tables, this field is
always 0, except in the root page of the clustered index after instant
ADD COLUMN.

Instant ADD COLUMN will change FIL_PAGE_TYPE to FIL_PAGE_TYPE_INSTANT
and initialize the PAGE_INSTANT field to the original number of
fields in the clustered index (dict_index_t::n_core_fields).  The most
significant bits are in the first byte, and the least significant 5
bits are stored in the most significant 5 bits of PAGE_DIRECTION_B.

These FIL_PAGE_TYPE_INSTANT and PAGE_INSTANT may be assigned even if
instant ADD COLUMN was not committed. Changes to these page header fields
are not undo-logged, but changes to the hidden metadata record are.
If the server is killed and restarted, the page header fields could
remain set even though no metadata record is present.

When the table becomes empty, the PAGE_INSTANT field and the
FIL_PAGE_TYPE can be reset and any metadata record be removed. */
#define PAGE_INSTANT	12

/** last insert direction: PAGE_LEFT, ....
In ROW_FORMAT=REDUNDANT tables created before MySQL 4.1.1 or MySQL 4.0.14,
this byte can be garbage. */
#define	PAGE_DIRECTION_B 13
#define	PAGE_N_DIRECTION 14	/* number of consecutive inserts to the same
				direction */
#define	PAGE_N_RECS	 16	/* number of user records on the page */
/** The largest DB_TRX_ID that may have modified a record on the page;
Defined only in secondary index leaf pages and in change buffer leaf pages.
Otherwise written as 0. @see PAGE_ROOT_AUTO_INC */
#define PAGE_MAX_TRX_ID	 18
/** The AUTO_INCREMENT value (on persistent clustered index root pages). */
#define PAGE_ROOT_AUTO_INC	PAGE_MAX_TRX_ID
#define PAGE_HEADER_PRIV_END 26	/* end of private data structure of the page
				header which are set in a page create */
/*----*/
#define	PAGE_LEVEL	 26	/* level of the node in an index tree; the
				leaf level is the level 0.  This field should
				not be written to after page creation. */
#define	PAGE_INDEX_ID	 28	/* index id where the page belongs.
				This field should not be written to after
				page creation. */

#define PAGE_BTR_SEG_LEAF 36	/* file segment header for the leaf pages in
				a B-tree: defined only on the root page of a
				B-tree, but not in the root of an ibuf tree */
#define PAGE_BTR_IBUF_FREE_LIST	PAGE_BTR_SEG_LEAF
#define PAGE_BTR_IBUF_FREE_LIST_NODE PAGE_BTR_SEG_LEAF
				/* in the place of PAGE_BTR_SEG_LEAF and _TOP
				there is a free list base node if the page is
				the root page of an ibuf tree, and at the same
				place is the free list node if the page is in
				a free list */
#define PAGE_BTR_SEG_TOP (36 + FSEG_HEADER_SIZE)
				/* file segment header for the non-leaf pages
				in a B-tree: defined only on the root page of
				a B-tree, but not in the root of an ibuf
				tree */
/*----*/
#define PAGE_DATA	(PAGE_HEADER + 36 + 2 * FSEG_HEADER_SIZE)
				/* start of data on the page */

#define PAGE_OLD_INFIMUM	(PAGE_DATA + 1 + REC_N_OLD_EXTRA_BYTES)
				/* offset of the page infimum record on an
				old-style page */
#define PAGE_OLD_SUPREMUM	(PAGE_DATA + 2 + 2 * REC_N_OLD_EXTRA_BYTES + 8)
				/* offset of the page supremum record on an
				old-style page */
#define PAGE_OLD_SUPREMUM_END (PAGE_OLD_SUPREMUM + 9)
				/* offset of the page supremum record end on
				an old-style page */
#define PAGE_NEW_INFIMUM	(PAGE_DATA + REC_N_NEW_EXTRA_BYTES)
				/* offset of the page infimum record on a
				new-style compact page */
#define PAGE_NEW_SUPREMUM	(PAGE_DATA + 2 * REC_N_NEW_EXTRA_BYTES + 8)
				/* offset of the page supremum record on a
				new-style compact page */
#define PAGE_NEW_SUPREMUM_END (PAGE_NEW_SUPREMUM + 8)
				/* offset of the page supremum record end on
				a new-style compact page */
/*-----------------------------*/

/* Heap numbers */
#define PAGE_HEAP_NO_INFIMUM	0U	/* page infimum */
#define PAGE_HEAP_NO_SUPREMUM	1U	/* page supremum */
#define PAGE_HEAP_NO_USER_LOW	2U	/* first user record in
					creation (insertion) order,
					not necessarily collation order;
					this record may have been deleted */

/* Directions of cursor movement (stored in PAGE_DIRECTION field) */
constexpr uint16_t PAGE_LEFT= 1;
constexpr uint16_t PAGE_RIGHT= 2;
constexpr uint16_t PAGE_SAME_REC= 3;
constexpr uint16_t PAGE_SAME_PAGE= 4;
constexpr uint16_t PAGE_NO_DIRECTION= 5;

#ifndef UNIV_INNOCHECKSUM

/*			PAGE DIRECTORY
			==============
*/

typedef	byte			page_dir_slot_t;

/* Offset of the directory start down from the page end. We call the
slot with the highest file address directory start, as it points to
the first record in the list of records. */
#define	PAGE_DIR		FIL_PAGE_DATA_END

/* We define a slot in the page directory as two bytes */
constexpr uint16_t PAGE_DIR_SLOT_SIZE= 2;

/* The offset of the physically lower end of the directory, counted from
page end, when the page is empty */
#define PAGE_EMPTY_DIR_START	(PAGE_DIR + 2 * PAGE_DIR_SLOT_SIZE)

/* The maximum and minimum number of records owned by a directory slot. The
number may drop below the minimum in the first and the last slot in the
directory. */
#define PAGE_DIR_SLOT_MAX_N_OWNED	8
#define	PAGE_DIR_SLOT_MIN_N_OWNED	4

extern my_bool srv_immediate_scrub_data_uncompressed;
#endif /* UNIV_INNOCHECKSUM */

/** Get the start of a page frame.
@param[in]	ptr	pointer within a page frame
@return start of the page frame */
MY_ATTRIBUTE((const))
inline page_t* page_align(void *ptr)
{
  return my_assume_aligned<UNIV_PAGE_SIZE_MIN>
    (reinterpret_cast<page_t*>(ut_align_down(ptr, srv_page_size)));
}
inline const page_t *page_align(const void *ptr)
{
  return page_align(const_cast<void*>(ptr));
}

/** Gets the byte offset within a page frame.
@param[in]	ptr	pointer within a page frame
@return offset from the start of the page */
MY_ATTRIBUTE((const))
inline uint16_t page_offset(const void*	ptr)
{
  return static_cast<uint16_t>(ut_align_offset(ptr, srv_page_size));
}

/** Determine whether an index page is not in ROW_FORMAT=REDUNDANT.
@param[in]	page	index page
@return	nonzero	if ROW_FORMAT is one of COMPACT,DYNAMIC,COMPRESSED
@retval	0	if ROW_FORMAT=REDUNDANT */
inline
byte
page_is_comp(const page_t* page)
{
	ut_ad(!ut_align_offset(page, UNIV_ZIP_SIZE_MIN));
	return(page[PAGE_HEADER + PAGE_N_HEAP] & 0x80);
}

/** Determine whether an index page is empty.
@param[in]	page	index page
@return whether the page is empty (PAGE_N_RECS = 0) */
inline
bool
page_is_empty(const page_t* page)
{
	ut_ad(!ut_align_offset(page, UNIV_ZIP_SIZE_MIN));
	return !*reinterpret_cast<const uint16_t*>(PAGE_HEADER + PAGE_N_RECS
						   + page);
}

/** Determine whether an index page contains garbage.
@param[in]	page	index page
@return whether the page contains garbage (PAGE_GARBAGE is not 0) */
inline
bool
page_has_garbage(const page_t* page)
{
	ut_ad(!ut_align_offset(page, UNIV_ZIP_SIZE_MIN));
	return *reinterpret_cast<const uint16_t*>(PAGE_HEADER + PAGE_GARBAGE
						  + page);
}

/** Determine whether an B-tree or R-tree index page is a leaf page.
@param[in]	page	index page
@return true if the page is a leaf (PAGE_LEVEL = 0) */
inline
bool
page_is_leaf(const page_t* page)
{
	ut_ad(!ut_align_offset(page, UNIV_ZIP_SIZE_MIN));
	return !*reinterpret_cast<const uint16_t*>(PAGE_HEADER + PAGE_LEVEL
						   + page);
}

#ifndef UNIV_INNOCHECKSUM
/** Determine whether an index page record is not in ROW_FORMAT=REDUNDANT.
@param[in]	rec	record in an index page frame (not a copy)
@return	nonzero	if ROW_FORMAT is one of COMPACT,DYNAMIC,COMPRESSED
@retval	0	if ROW_FORMAT=REDUNDANT */
inline
byte
page_rec_is_comp(const byte* rec)
{
	return(page_is_comp(page_align(rec)));
}

# ifdef UNIV_DEBUG
/** Determine if the record is the metadata pseudo-record
in the clustered index.
@param[in]	rec	leaf page record on an index page
@return	whether the record is the metadata pseudo-record */
inline bool page_rec_is_metadata(const rec_t* rec)
{
	return rec_get_info_bits(rec, page_rec_is_comp(rec))
		& REC_INFO_MIN_REC_FLAG;
}
# endif /* UNIV_DEBUG */

/** Determine the offset of the infimum record on the page.
@param[in]	page	index page
@return offset of the infimum record in record list, relative from page */
inline
unsigned
page_get_infimum_offset(const page_t* page)
{
	ut_ad(!page_offset(page));
	return page_is_comp(page) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
}

/** Determine the offset of the supremum record on the page.
@param[in]	page	index page
@return offset of the supremum record in record list, relative from page */
inline
unsigned
page_get_supremum_offset(const page_t* page)
{
	ut_ad(!page_offset(page));
	return page_is_comp(page) ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;
}

/** Determine whether an index page record is a user record.
@param[in]	offset	record offset in the page
@retval true if a user record
@retval	false if the infimum or supremum pseudo-record */
inline
bool
page_rec_is_user_rec_low(ulint offset)
{
	compile_time_assert(PAGE_OLD_INFIMUM >= PAGE_NEW_INFIMUM);
	compile_time_assert(PAGE_OLD_SUPREMUM >= PAGE_NEW_SUPREMUM);
	compile_time_assert(PAGE_NEW_INFIMUM < PAGE_OLD_SUPREMUM);
	compile_time_assert(PAGE_OLD_INFIMUM < PAGE_NEW_SUPREMUM);
	compile_time_assert(PAGE_NEW_SUPREMUM < PAGE_OLD_SUPREMUM_END);
	compile_time_assert(PAGE_OLD_SUPREMUM < PAGE_NEW_SUPREMUM_END);
	ut_ad(offset >= PAGE_NEW_INFIMUM);
	ut_ad(offset <= srv_page_size - PAGE_EMPTY_DIR_START);

	return(offset != PAGE_NEW_SUPREMUM
	       && offset != PAGE_NEW_INFIMUM
	       && offset != PAGE_OLD_INFIMUM
	       && offset != PAGE_OLD_SUPREMUM);
}

/** Determine if a record is the supremum record on an index page.
@param[in]	offset	record offset in an index page
@return true if the supremum record */
inline
bool
page_rec_is_supremum_low(ulint offset)
{
	ut_ad(offset >= PAGE_NEW_INFIMUM);
	ut_ad(offset <= srv_page_size - PAGE_EMPTY_DIR_START);
	return(offset == PAGE_NEW_SUPREMUM || offset == PAGE_OLD_SUPREMUM);
}

/** Determine if a record is the infimum record on an index page.
@param[in]	offset	record offset in an index page
@return true if the infimum record */
inline
bool
page_rec_is_infimum_low(ulint offset)
{
	ut_ad(offset >= PAGE_NEW_INFIMUM);
	ut_ad(offset <= srv_page_size - PAGE_EMPTY_DIR_START);
	return(offset == PAGE_NEW_INFIMUM || offset == PAGE_OLD_INFIMUM);
}

/** Determine whether an B-tree or R-tree index record is in a leaf page.
@param[in]	rec	index record in an index page
@return true if the record is in a leaf page */
inline
bool
page_rec_is_leaf(const page_t* rec)
{
	const page_t* page = page_align(rec);
	ut_ad(ulint(rec - page) >= page_get_infimum_offset(page));
	bool leaf = page_is_leaf(page);
	ut_ad(!page_rec_is_comp(rec)
	      || !page_rec_is_user_rec_low(ulint(rec - page))
	      || leaf == !rec_get_node_ptr_flag(rec));
	return leaf;
}

/** Determine whether an index page record is a user record.
@param[in]	rec	record in an index page
@return true if a user record */
inline
bool
page_rec_is_user_rec(const rec_t* rec);

/** Determine whether an index page record is the supremum record.
@param[in]	rec	record in an index page
@return true if the supremum record */
inline
bool
page_rec_is_supremum(const rec_t* rec);

/** Determine whether an index page record is the infimum record.
@param[in]	rec	record in an index page
@return true if the infimum record */
inline
bool
page_rec_is_infimum(const rec_t* rec);

/** Read PAGE_MAX_TRX_ID.
@param[in]      page    index page
@return the value of PAGE_MAX_TRX_ID or PAGE_ROOT_AUTO_INC */
MY_ATTRIBUTE((nonnull, warn_unused_result))
inline trx_id_t page_get_max_trx_id(const page_t *page)
{
  ut_ad(fil_page_index_page_check(page));
  static_assert((PAGE_HEADER + PAGE_MAX_TRX_ID) % 8 == 0, "alignment");
  const auto *p= my_assume_aligned<8>(page + PAGE_HEADER + PAGE_MAX_TRX_ID);
  return mach_read_from_8(p);
}

/**
Set the number of owned records.
@tparam compressed    whether to update any ROW_FORMAT=COMPRESSED page as well
@param[in,out]  block   index page
@param[in,out]  rec     record in block.frame
@param[in]      n_owned number of records skipped in the sparse page directory
@param[in]      comp    whether ROW_FORMAT is one of COMPACT,DYNAMIC,COMPRESSED
@param[in,out]  mtr     mini-transaction */
template<bool compressed>
inline void page_rec_set_n_owned(buf_block_t *block, rec_t *rec, ulint n_owned,
                                 bool comp, mtr_t *mtr)
{
  ut_ad(block->page.frame == page_align(rec));
  ut_ad(comp == (page_is_comp(block->page.frame) != 0));

  if (page_zip_des_t *page_zip= compressed
      ? buf_block_get_page_zip(block) : nullptr)
  {
    ut_ad(comp);
    rec_set_bit_field_1(rec, n_owned, REC_NEW_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    if (rec_get_status(rec) != REC_STATUS_SUPREMUM)
      page_zip_rec_set_owned(block, rec, n_owned, mtr);
  }
  else
  {
    rec-= comp ? REC_NEW_N_OWNED : REC_OLD_N_OWNED;
    mtr->write<1,mtr_t::MAYBE_NOP>(*block, rec, (*rec & ~REC_N_OWNED_MASK) |
                                   (n_owned << REC_N_OWNED_SHIFT));
  }
}

/*************************************************************//**
Sets the max trx id field value. */
void
page_set_max_trx_id(
/*================*/
	buf_block_t*	block,	/*!< in/out: page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page, or NULL */
	trx_id_t	trx_id,	/*!< in: transaction id */
	mtr_t*		mtr);	/*!< in/out: mini-transaction, or NULL */
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
	mtr_t*		mtr);	/*!< in/out: mini-transaction */

/** Persist the AUTO_INCREMENT value on a clustered index root page.
@param[in,out]	block	clustered index root page
@param[in]	autoinc	next available AUTO_INCREMENT value
@param[in,out]	mtr	mini-transaction
@param[in]	reset	whether to reset the AUTO_INCREMENT
			to a possibly smaller value than currently
			exists in the page */
void
page_set_autoinc(
	buf_block_t*		block,
	ib_uint64_t		autoinc,
	mtr_t*			mtr,
	bool			reset)
	MY_ATTRIBUTE((nonnull));

/*************************************************************//**
Returns the RTREE SPLIT SEQUENCE NUMBER (FIL_RTREE_SPLIT_SEQ_NUM).
@return SPLIT SEQUENCE NUMBER */
UNIV_INLINE
node_seq_t
page_get_ssn_id(
/*============*/
	const page_t*	page);	/*!< in: page */
/*************************************************************//**
Sets the RTREE SPLIT SEQUENCE NUMBER field value */
UNIV_INLINE
void
page_set_ssn_id(
/*============*/
	buf_block_t*	block,	/*!< in/out: page */
	page_zip_des_t*	page_zip,/*!< in/out: compressed page whose
				uncompressed part will be updated, or NULL */
	node_seq_t	ssn_id,	/*!< in: split sequence id */
	mtr_t*		mtr);	/*!< in/out: mini-transaction */

#endif /* !UNIV_INNOCHECKSUM */
/** Read a page header field. */
inline uint16_t page_header_get_field(const page_t *page, ulint field)
{
  ut_ad(field <= PAGE_INDEX_ID);
  ut_ad(!(field & 1));
  return mach_read_from_2(my_assume_aligned<2>(PAGE_HEADER + field + page));
}

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
	MY_ATTRIBUTE((warn_unused_result));

/*************************************************************//**
Returns the pointer stored in the given header field, or NULL. */
#define page_header_get_ptr(page, field)			\
	(page_header_get_offs(page, field)			\
	 ? page + page_header_get_offs(page, field) : NULL)

/**
Reset PAGE_LAST_INSERT.
@param[in,out]  block    file page
@param[in,out]  mtr      mini-transaction */
inline void page_header_reset_last_insert(buf_block_t *block, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));
#define page_get_infimum_rec(page) ((page) + page_get_infimum_offset(page))
#define page_get_supremum_rec(page) ((page) + page_get_supremum_offset(page))

/************************************************************//**
Returns the nth record of the record list.
This is the inverse function of page_rec_get_n_recs_before().
@return nth record */
const rec_t*
page_rec_get_nth_const(
/*===================*/
	const page_t*	page,	/*!< in: page */
	ulint		nth)	/*!< in: nth record */
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/************************************************************//**
Returns the nth record of the record list.
This is the inverse function of page_rec_get_n_recs_before().
@return nth record */
UNIV_INLINE
rec_t*
page_rec_get_nth(
/*=============*/
	page_t*	page,	/*< in: page */
	ulint	nth)	/*!< in: nth record */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

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
	MY_ATTRIBUTE((nonnull, warn_unused_result));
/*************************************************************//**
Gets the page number.
@return page number */
UNIV_INLINE
uint32_t
page_get_page_no(
/*=============*/
	const page_t*	page);	/*!< in: page */

/*************************************************************//**
Gets the tablespace identifier.
@return space id */
UNIV_INLINE
uint32_t
page_get_space_id(
/*==============*/
	const page_t*	page);	/*!< in: page */

/*************************************************************//**
Gets the number of user records on page (the infimum and supremum records
are not user records).
@return number of user records */
UNIV_INLINE
uint16_t
page_get_n_recs(
/*============*/
	const page_t*	page);	/*!< in: index page */

/***************************************************************//**
Returns the number of records before the given record in chain.
The number includes infimum and supremum records.
This is the inverse function of page_rec_get_nth().
@return number of records */
ulint
page_rec_get_n_recs_before(
/*=======================*/
	const rec_t*	rec);	/*!< in: the physical record */
/*************************************************************//**
Gets the number of records in the heap.
@return number of user records */
UNIV_INLINE
uint16_t
page_dir_get_n_heap(
/*================*/
	const page_t*	page);	/*!< in: index page */
/*************************************************************//**
Gets the number of dir slots in directory.
@return number of slots */
UNIV_INLINE
uint16_t
page_dir_get_n_slots(
/*=================*/
	const page_t*	page);	/*!< in: index page */
/** Gets the pointer to a directory slot.
@param n  sparse directory slot number
@return pointer to the sparse directory slot */
inline page_dir_slot_t *page_dir_get_nth_slot(page_t *page, ulint n)
{
  ut_ad(page_dir_get_n_slots(page) > n);
  static_assert(PAGE_DIR_SLOT_SIZE == 2, "compatibility");
  return my_assume_aligned<2>(page + srv_page_size - (PAGE_DIR + 2) - n * 2);
}
inline const page_dir_slot_t *page_dir_get_nth_slot(const page_t *page,ulint n)
{
  return page_dir_get_nth_slot(const_cast<page_t*>(page), n);
}
/**************************************************************//**
Used to check the consistency of a record on a page.
@return TRUE if succeed */
UNIV_INLINE
ibool
page_rec_check(
/*===========*/
	const rec_t*	rec);	/*!< in: record */
/** Get the record pointed to by a directory slot.
@param[in] slot   directory slot
@return pointer to record */
inline rec_t *page_dir_slot_get_rec(page_dir_slot_t *slot)
{
  return page_align(slot) + mach_read_from_2(my_assume_aligned<2>(slot));
}
inline const rec_t *page_dir_slot_get_rec(const page_dir_slot_t *slot)
{
  return page_dir_slot_get_rec(const_cast<rec_t*>(slot));
}
/***************************************************************//**
Gets the number of records owned by a directory slot.
@return number of records */
UNIV_INLINE
ulint
page_dir_slot_get_n_owned(
/*======================*/
	const page_dir_slot_t*	slot);	/*!< in: page directory slot */
/************************************************************//**
Calculates the space reserved for directory slots of a given
number of records. The exact value is a fraction number
n * PAGE_DIR_SLOT_SIZE / PAGE_DIR_SLOT_MIN_N_OWNED, and it is
rounded upwards to an integer. */
UNIV_INLINE
ulint
page_dir_calc_reserved_space(
/*=========================*/
	ulint	n_recs);	/*!< in: number of records */
/***************************************************************//**
Looks for the directory slot which owns the given record.
@return the directory slot number */
ulint
page_dir_find_owner_slot(
/*=====================*/
	const rec_t*	rec);	/*!< in: the physical record */

/***************************************************************//**
Returns the heap number of a record.
@return heap number */
UNIV_INLINE
ulint
page_rec_get_heap_no(
/*=================*/
	const rec_t*	rec);	/*!< in: the physical record */
/** Determine whether a page has any siblings.
@param[in]	page	page frame
@return true if the page has any siblings */
inline bool page_has_siblings(const page_t* page)
{
	compile_time_assert(!(FIL_PAGE_PREV % 8));
	compile_time_assert(FIL_PAGE_NEXT == FIL_PAGE_PREV + 4);
	compile_time_assert(FIL_NULL == 0xffffffff);
	return *reinterpret_cast<const uint64_t*>(page + FIL_PAGE_PREV)
		!= ~uint64_t(0);
}

/** Determine whether a page has a predecessor.
@param[in]	page	page frame
@return true if the page has a predecessor */
inline bool page_has_prev(const page_t* page)
{
	return *reinterpret_cast<const uint32_t*>(page + FIL_PAGE_PREV)
		!= FIL_NULL;
}

/** Determine whether a page has a successor.
@param[in]	page	page frame
@return true if the page has a successor */
inline bool page_has_next(const page_t* page)
{
	return *reinterpret_cast<const uint32_t*>(page + FIL_PAGE_NEXT)
		!= FIL_NULL;
}

/** Read the AUTO_INCREMENT value from a clustered index root page.
@param[in]	page	clustered index root page
@return	the persisted AUTO_INCREMENT value */
MY_ATTRIBUTE((nonnull, warn_unused_result))
inline uint64_t page_get_autoinc(const page_t *page)
{
  ut_d(uint16_t page_type= fil_page_get_type(page));
  ut_ad(page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_TYPE_INSTANT);
  ut_ad(!page_has_siblings(page));
  const auto *p= my_assume_aligned<8>(page + PAGE_HEADER + PAGE_ROOT_AUTO_INC);
  return mach_read_from_8(p);
}

/************************************************************//**
Gets the pointer to the next record on the page.
@return pointer to next record */
UNIV_INLINE
const rec_t*
page_rec_get_next_low(
/*==================*/
	const rec_t*	rec,	/*!< in: pointer to record */
	ulint		comp);	/*!< in: nonzero=compact page layout */
/************************************************************//**
Gets the pointer to the next record on the page.
@return pointer to next record */
UNIV_INLINE
rec_t*
page_rec_get_next(
/*==============*/
	rec_t*	rec);	/*!< in: pointer to record */
/************************************************************//**
Gets the pointer to the next record on the page.
@return pointer to next record */
UNIV_INLINE
const rec_t*
page_rec_get_next_const(
/*====================*/
	const rec_t*	rec);	/*!< in: pointer to record */
/************************************************************//**
Gets the pointer to the next non delete-marked record on the page.
If all subsequent records are delete-marked, then this function
will return the supremum record.
@return pointer to next non delete-marked record or pointer to supremum */
UNIV_INLINE
const rec_t*
page_rec_get_next_non_del_marked(
/*=============================*/
	const rec_t*	rec);	/*!< in: pointer to record */
/************************************************************//**
Gets the pointer to the previous record.
@return pointer to previous record */
UNIV_INLINE
const rec_t*
page_rec_get_prev_const(
/*====================*/
	const rec_t*	rec);	/*!< in: pointer to record, must not be page
				infimum */
/************************************************************//**
Gets the pointer to the previous record.
@return pointer to previous record */
UNIV_INLINE
rec_t*
page_rec_get_prev(
/*==============*/
	rec_t*		rec);	/*!< in: pointer to record,
				must not be page infimum */

/************************************************************//**
true if the record is the first user record on a page.
@return true if the first user record */
UNIV_INLINE
bool
page_rec_is_first(
/*==============*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
	MY_ATTRIBUTE((warn_unused_result));

/************************************************************//**
true if the record is the second user record on a page.
@return true if the second user record */
UNIV_INLINE
bool
page_rec_is_second(
/*===============*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
	MY_ATTRIBUTE((warn_unused_result));

/************************************************************//**
true if the record is the last user record on a page.
@return true if the last user record */
UNIV_INLINE
bool
page_rec_is_last(
/*=============*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
	MY_ATTRIBUTE((warn_unused_result));

/************************************************************//**
true if distance between the records (measured in number of times we have to
move to the next record) is at most the specified value
@param[in]	left_rec	lefter record
@param[in]	right_rec	righter record
@param[in]	val		specified value to compare
@return true if the distance is smaller than the value */
UNIV_INLINE
bool
page_rec_distance_is_at_most(
/*=========================*/
	const rec_t*	left_rec,
	const rec_t*	right_rec,
	ulint		val)
	MY_ATTRIBUTE((warn_unused_result));

/************************************************************//**
true if the record is the second last user record on a page.
@return true if the second last user record */
UNIV_INLINE
bool
page_rec_is_second_last(
/*====================*/
	const rec_t*	rec,	/*!< in: record */
	const page_t*	page)	/*!< in: page */
	MY_ATTRIBUTE((warn_unused_result));

/************************************************************//**
Returns the maximum combined size of records which can be inserted on top
of record heap.
@return maximum combined size for inserted records */
UNIV_INLINE
ulint
page_get_max_insert_size(
/*=====================*/
	const page_t*	page,	/*!< in: index page */
	ulint		n_recs);/*!< in: number of records */
/************************************************************//**
Returns the maximum combined size of records which can be inserted on top
of record heap if page is first reorganized.
@return maximum combined size for inserted records */
UNIV_INLINE
ulint
page_get_max_insert_size_after_reorganize(
/*======================================*/
	const page_t*	page,	/*!< in: index page */
	ulint		n_recs);/*!< in: number of records */
/*************************************************************//**
Calculates free space if a page is emptied.
@return free space */
UNIV_INLINE
ulint
page_get_free_space_of_empty(
/*=========================*/
	ulint	comp)	/*!< in: nonzero=compact page format */
		MY_ATTRIBUTE((const));
/************************************************************//**
Returns the sum of the sizes of the records in the record list
excluding the infimum and supremum records.
@return data in bytes */
UNIV_INLINE
uint16_t
page_get_data_size(
/*===============*/
	const page_t*	page);	/*!< in: index page */
/** Read the PAGE_DIRECTION field from a byte.
@param[in]	ptr	pointer to PAGE_DIRECTION_B
@return	the value of the PAGE_DIRECTION field */
inline
byte
page_ptr_get_direction(const byte* ptr);

/** Read the PAGE_DIRECTION field.
@param[in]	page	index page
@return	the value of the PAGE_DIRECTION field */
inline
byte
page_get_direction(const page_t* page)
{
	return page_ptr_get_direction(PAGE_HEADER + PAGE_DIRECTION_B + page);
}

/** Read the PAGE_INSTANT field.
@param[in]	page	index page
@return the value of the PAGE_INSTANT field */
inline
uint16_t
page_get_instant(const page_t* page);

/** Create an uncompressed index page.
@param[in,out]	block	buffer block
@param[in,out]	mtr	mini-transaction
@param[in]	comp	set unless ROW_FORMAT=REDUNDANT */
void page_create(buf_block_t *block, mtr_t *mtr, bool comp);
/**********************************************************//**
Create a compressed B-tree index page. */
void
page_create_zip(
/*============*/
	buf_block_t*		block,		/*!< in/out: a buffer frame
						where the page is created */
	dict_index_t*		index,		/*!< in: the index of the
						page */
	ulint			level,		/*!< in: the B-tree level of
						the page */
	trx_id_t		max_trx_id,	/*!< in: PAGE_MAX_TRX_ID */
	mtr_t*			mtr);		/*!< in/out: mini-transaction
						handle */
/**********************************************************//**
Empty a previously created B-tree index page. */
void
page_create_empty(
/*==============*/
	buf_block_t*	block,	/*!< in/out: B-tree block */
	dict_index_t*	index,	/*!< in: the index of the page */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull(1,2)));
/*************************************************************//**
Differs from page_copy_rec_list_end, because this function does not
touch the lock table and max trx id on page or compress the page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit(). */
void
page_copy_rec_list_end_no_locks(
/*============================*/
	buf_block_t*	new_block,	/*!< in: index page to copy to */
	buf_block_t*	block,		/*!< in: index page of rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr);		/*!< in: mtr */
/*************************************************************//**
Copies records from page to new_page, from the given record onward,
including that record. Infimum and supremum records are not copied.
The records are copied to the start of the record list on new_page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to the original successor of the infimum record on
new_page, or NULL on zip overflow (new_block will be decompressed) */
rec_t*
page_copy_rec_list_end(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: index page to copy to */
	buf_block_t*	block,		/*!< in: index page containing rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));
/*************************************************************//**
Copies records from page to new_page, up to the given record, NOT
including that record. Infimum and supremum records are not copied.
The records are copied to the end of the record list on new_page.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return pointer to the original predecessor of the supremum record on
new_page, or NULL on zip overflow (new_block will be decompressed) */
rec_t*
page_copy_rec_list_start(
/*=====================*/
	buf_block_t*	new_block,	/*!< in/out: index page to copy to */
	buf_block_t*	block,		/*!< in: index page containing rec */
	rec_t*		rec,		/*!< in: record on page */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));
/*************************************************************//**
Deletes records from a page from a given record onward, including that record.
The infimum and supremum records are not deleted. */
void
page_delete_rec_list_end(
/*=====================*/
	rec_t*		rec,	/*!< in: pointer to record on page */
	buf_block_t*	block,	/*!< in: buffer block of the page */
	dict_index_t*	index,	/*!< in: record descriptor */
	ulint		n_recs,	/*!< in: number of records to delete,
				or ULINT_UNDEFINED if not known */
	ulint		size,	/*!< in: the sum of the sizes of the
				records in the end of the chain to
				delete, or ULINT_UNDEFINED if not known */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));
/*************************************************************//**
Deletes records from page, up to the given record, NOT including
that record. Infimum and supremum records are not deleted. */
void
page_delete_rec_list_start(
/*=======================*/
	rec_t*		rec,	/*!< in: record on page */
	buf_block_t*	block,	/*!< in: buffer block of the page */
	dict_index_t*	index,	/*!< in: record descriptor */
	mtr_t*		mtr)	/*!< in: mtr */
	MY_ATTRIBUTE((nonnull));
/*************************************************************//**
Moves record list end to another page. Moved records include
split_rec.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return TRUE on success; FALSE on compression failure (new_block will
be decompressed) */
ibool
page_move_rec_list_end(
/*===================*/
	buf_block_t*	new_block,	/*!< in/out: index page where to move */
	buf_block_t*	block,		/*!< in: index page from where to move */
	rec_t*		split_rec,	/*!< in: first record to move */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
	MY_ATTRIBUTE((nonnull(1, 2, 4, 5)));
/*************************************************************//**
Moves record list start to another page. Moved records do not include
split_rec.

IMPORTANT: The caller will have to update IBUF_BITMAP_FREE
if new_block is a compressed leaf page in a secondary index.
This has to be done either within the same mini-transaction,
or by invoking ibuf_reset_free_bits() before mtr_commit().

@return TRUE on success; FALSE on compression failure */
ibool
page_move_rec_list_start(
/*=====================*/
	buf_block_t*	new_block,	/*!< in/out: index page where to move */
	buf_block_t*	block,		/*!< in/out: page containing split_rec */
	rec_t*		split_rec,	/*!< in: first record not to move */
	dict_index_t*	index,		/*!< in: record descriptor */
	mtr_t*		mtr)		/*!< in: mtr */
	MY_ATTRIBUTE((nonnull(1, 2, 4, 5)));
/** Create an index page.
@param[in,out]	block	buffer block
@param[in]	comp	nonzero=compact page format */
void page_create_low(const buf_block_t* block, bool comp);

/************************************************************//**
Prints record contents including the data relevant only in
the index page context. */
void
page_rec_print(
/*===========*/
	const rec_t*	rec,	/*!< in: physical record */
	const rec_offs*	offsets);/*!< in: record descriptor */
# ifdef UNIV_BTR_PRINT
/***************************************************************//**
This is used to print the contents of the directory for
debugging purposes. */
void
page_dir_print(
/*===========*/
	page_t*	page,	/*!< in: index page */
	ulint	pr_n);	/*!< in: print n first and n last entries */
/***************************************************************//**
This is used to print the contents of the page record list for
debugging purposes. */
void
page_print_list(
/*============*/
	buf_block_t*	block,	/*!< in: index page */
	dict_index_t*	index,	/*!< in: dictionary index of the page */
	ulint		pr_n);	/*!< in: print n first and n last entries */
/***************************************************************//**
Prints the info in a page header. */
void
page_header_print(
/*==============*/
	const page_t*	page);	/*!< in: index page */
/***************************************************************//**
This is used to print the contents of the page for
debugging purposes. */
void
page_print(
/*=======*/
	buf_block_t*	block,	/*!< in: index page */
	dict_index_t*	index,	/*!< in: dictionary index of the page */
	ulint		dn,	/*!< in: print dn first and last entries
				in directory */
	ulint		rn);	/*!< in: print rn first and last records
				in directory */
# endif /* UNIV_BTR_PRINT */
/***************************************************************//**
The following is used to validate a record on a page. This function
differs from rec_validate as it can also check the n_owned field and
the heap_no field.
@return TRUE if ok */
ibool
page_rec_validate(
/*==============*/
	const rec_t*	rec,	/*!< in: physical record */
	const rec_offs*	offsets);/*!< in: array returned by rec_get_offsets() */
#ifdef UNIV_DEBUG
/***************************************************************//**
Checks that the first directory slot points to the infimum record and
the last to the supremum. This function is intended to track if the
bug fixed in 4.0.14 has caused corruption to users' databases. */
void
page_check_dir(
/*===========*/
	const page_t*	page);	/*!< in: index page */
#endif /* UNIV_DEBUG */
/***************************************************************//**
This function checks the consistency of an index page when we do not
know the index. This is also resilient so that this should never crash
even if the page is total garbage.
@return TRUE if ok */
ibool
page_simple_validate_old(
/*=====================*/
	const page_t*	page);	/*!< in: index page in ROW_FORMAT=REDUNDANT */
/***************************************************************//**
This function checks the consistency of an index page when we do not
know the index. This is also resilient so that this should never crash
even if the page is total garbage.
@return TRUE if ok */
ibool
page_simple_validate_new(
/*=====================*/
	const page_t*	page);	/*!< in: index page in ROW_FORMAT!=REDUNDANT */
/** Check the consistency of an index page.
@param[in]	page	index page
@param[in]	index	B-tree or R-tree index
@return	whether the page is valid */
bool page_validate(const page_t* page, const dict_index_t* index)
	MY_ATTRIBUTE((nonnull));
/***************************************************************//**
Looks in the page record list for a record with the given heap number.
@return record, NULL if not found */
const rec_t*
page_find_rec_with_heap_no(
/*=======================*/
	const page_t*	page,	/*!< in: index page */
	ulint		heap_no);/*!< in: heap number */
/** Get the last non-delete-marked record on a page.
@param[in]	page	index tree leaf page
@return the last record, not delete-marked
@retval infimum record if all records are delete-marked */
const rec_t*
page_find_rec_last_not_deleted(
	const page_t*	page);

#endif /* !UNIV_INNOCHECKSUM */

#include "page0page.inl"

#endif
