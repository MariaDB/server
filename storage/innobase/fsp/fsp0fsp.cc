/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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

/******************************************************************//**
@file fsp/fsp0fsp.cc
File space management

Created 11/29/1995 Heikki Tuuri
***********************************************************************/

#include "fsp0fsp.h"
#include "buf0buf.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "mtr0log.h"
#include "ut0byte.h"
#include "page0page.h"
#include "fut0fut.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "ibuf0ibuf.h"
#include "btr0btr.h"
#include "btr0sea.h"
#include "dict0boot.h"
#include "log0log.h"
#include "dict0mem.h"
#include "fsp0types.h"

// JAN: MySQL 5.7 Encryption
// #include <my_aes.h>

typedef ulint page_no_t;

/** Return an extent to the free list of a space.
@param[in,out]	space		tablespace
@param[in]	offset		page number in the extent
@param[in,out]	mtr		mini-transaction */
MY_ATTRIBUTE((nonnull))
static
void
fsp_free_extent(
	fil_space_t*		space,
	page_no_t		offset,
	mtr_t*			mtr);

/********************************************************************//**
Marks a page used. The page must reside within the extents of the given
segment. */
static MY_ATTRIBUTE((nonnull))
void
fseg_mark_page_used(
/*================*/
	fseg_inode_t*	seg_inode,/*!< in: segment inode */
	page_no_t	page,	/*!< in: page offset */
	xdes_t*		descr,  /*!< in: extent descriptor */
	mtr_t*		mtr);	/*!< in/out: mini-transaction */

/** Returns the first extent descriptor for a segment.
We think of the extent lists of the segment catenated in the order
FSEG_FULL -> FSEG_NOT_FULL -> FSEG_FREE.
@param[in]	inode		segment inode
@param[in]	space		tablespace
@param[in,out]	mtr		mini-transaction
@return the first extent descriptor, or NULL if none */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
xdes_t*
fseg_get_first_extent(
	fseg_inode_t*		inode,
	const fil_space_t*	space,
	mtr_t*			mtr);

/** Put new extents to the free list if there are free extents above the free
limit. If an extent happens to contain an extent descriptor page, the extent
is put to the FSP_FREE_FRAG list with the page marked as used.
@param[in]	init_space	true if this is a single-table tablespace
and we are only initializing the first extent and the first bitmap pages;
then we will not allocate more extents
@param[in,out]	space		tablespace
@param[in,out]	header		tablespace header
@param[in,out]	mtr		mini-transaction */
static ATTRIBUTE_COLD
void
fsp_fill_free_list(
	bool		init_space,
	fil_space_t*	space,
	fsp_header_t*	header,
	mtr_t*		mtr);

/** Allocates a single free page from a segment.
This function implements the intelligent allocation strategy which tries
to minimize file space fragmentation.
@param[in,out]	space			tablespace
@param[in,out]	seg_inode		segment inode
@param[in]	hint			hint of which page would be desirable
@param[in]	direction		if the new page is needed because of
an index page split, and records are inserted there in order, into which
direction they go alphabetically: FSP_DOWN, FSP_UP, FSP_NO_DIR
@param[in,out]	mtr			mini-transaction
@param[in,out]	init_mtr		mtr or another mini-transaction in
which the page should be initialized. If init_mtr != mtr, but the page is
already latched in mtr, do not initialize the page
@param[in]	has_done_reservation	TRUE if the space has already been
reserved, in this case we will never return NULL
@retval NULL	if no page could be allocated
@retval block	rw_lock_x_lock_count(&block->lock) == 1 if allocation succeeded
(init_mtr == mtr, or the page was not previously freed in mtr)
@retval block	(not allocated or initialized) otherwise */
static
buf_block_t*
fseg_alloc_free_page_low(
	fil_space_t*		space,
	fseg_inode_t*		seg_inode,
	ulint			hint,
	byte			direction,
	mtr_t*			mtr,
	mtr_t*			init_mtr
#ifdef UNIV_DEBUG
	, ibool			has_done_reservation
#endif /* UNIV_DEBUG */
)
	MY_ATTRIBUTE((warn_unused_result));

/** Gets a pointer to the space header and x-locks its page.
@param[in]	space		tablespace
@param[in,out]	mtr		mini-transaction
@return pointer to the space header, page x-locked */
inline fsp_header_t* fsp_get_space_header(const fil_space_t* space, mtr_t* mtr)
{
	buf_block_t*	block;
	fsp_header_t*	header;

	ut_ad(space->purpose != FIL_TYPE_LOG);

	block = buf_page_get(page_id_t(space->id, 0), space->zip_size(),
			     RW_SX_LATCH, mtr);
	header = FSP_HEADER_OFFSET + buf_block_get_frame(block);
	buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

	ut_ad(space->id == mach_read_from_4(FSP_SPACE_ID + header));
	return(header);
}

/**********************************************************************//**
Gets a descriptor bit of a page.
@return TRUE if free */
UNIV_INLINE
ibool
xdes_mtr_get_bit(
/*=============*/
	const xdes_t*	descr,	/*!< in: descriptor */
	ulint		bit,	/*!< in: XDES_FREE_BIT or XDES_CLEAN_BIT */
	ulint		offset,	/*!< in: page offset within extent:
				0 ... FSP_EXTENT_SIZE - 1 */
	mtr_t*		mtr)	/*!< in: mini-transaction */
{
	ut_ad(mtr->is_active());
	ut_ad(mtr_memo_contains_page(mtr, descr, MTR_MEMO_PAGE_SX_FIX));

	return(xdes_get_bit(descr, bit, offset));
}

/**********************************************************************//**
Sets a descriptor bit of a page. */
UNIV_INLINE
void
xdes_set_bit(
/*=========*/
	xdes_t*	descr,	/*!< in: descriptor */
	ulint	bit,	/*!< in: XDES_FREE_BIT or XDES_CLEAN_BIT */
	ulint	offset,	/*!< in: page offset within extent:
			0 ... FSP_EXTENT_SIZE - 1 */
	ibool	val,	/*!< in: bit value */
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ulint	index;
	ulint	byte_index;
	ulint	bit_index;
	ulint	descr_byte;

	ut_ad(mtr_memo_contains_page(
			mtr, descr,
			MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX));
	ut_ad((bit == XDES_FREE_BIT) || (bit == XDES_CLEAN_BIT));
	ut_ad(offset < FSP_EXTENT_SIZE);

	index = bit + XDES_BITS_PER_PAGE * offset;

	byte_index = index / 8;
	bit_index = index % 8;

	descr_byte = mach_read_from_1(descr + XDES_BITMAP + byte_index);
	descr_byte = ut_bit_set_nth(descr_byte, bit_index, val);

	mlog_write_ulint(descr + XDES_BITMAP + byte_index, descr_byte,
			 MLOG_1BYTE, mtr);
}

/**********************************************************************//**
Looks for a descriptor bit having the desired value. Starts from hint
and scans upward; at the end of the extent the search is wrapped to
the start of the extent.
@return bit index of the bit, ULINT_UNDEFINED if not found */
UNIV_INLINE
ulint
xdes_find_bit(
/*==========*/
	xdes_t*	descr,	/*!< in: descriptor */
	ulint	bit,	/*!< in: XDES_FREE_BIT or XDES_CLEAN_BIT */
	ibool	val,	/*!< in: desired bit value */
	ulint	hint,	/*!< in: hint of which bit position would
			be desirable */
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ulint	i;

	ut_ad(descr && mtr);
	ut_ad(val <= TRUE);
	ut_ad(hint < FSP_EXTENT_SIZE);
	ut_ad(mtr_memo_contains_page(mtr, descr, MTR_MEMO_PAGE_SX_FIX));
	for (i = hint; i < FSP_EXTENT_SIZE; i++) {
		if (val == xdes_mtr_get_bit(descr, bit, i, mtr)) {

			return(i);
		}
	}

	for (i = 0; i < hint; i++) {
		if (val == xdes_mtr_get_bit(descr, bit, i, mtr)) {

			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/**********************************************************************//**
Returns the number of used pages in a descriptor.
@return number of pages used */
UNIV_INLINE
ulint
xdes_get_n_used(
/*============*/
	const xdes_t*	descr,	/*!< in: descriptor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	count	= 0;

	ut_ad(descr && mtr);
	ut_ad(mtr_memo_contains_page(mtr, descr, MTR_MEMO_PAGE_SX_FIX));
	for (ulint i = 0; i < FSP_EXTENT_SIZE; ++i) {
		if (FALSE == xdes_mtr_get_bit(descr, XDES_FREE_BIT, i, mtr)) {
			count++;
		}
	}

	return(count);
}

/**********************************************************************//**
Returns true if extent contains no used pages.
@return TRUE if totally free */
UNIV_INLINE
ibool
xdes_is_free(
/*=========*/
	const xdes_t*	descr,	/*!< in: descriptor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	if (0 == xdes_get_n_used(descr, mtr)) {

		return(TRUE);
	}

	return(FALSE);
}

/**********************************************************************//**
Returns true if extent contains no free pages.
@return TRUE if full */
UNIV_INLINE
ibool
xdes_is_full(
/*=========*/
	const xdes_t*	descr,	/*!< in: descriptor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	if (FSP_EXTENT_SIZE == xdes_get_n_used(descr, mtr)) {

		return(TRUE);
	}

	return(FALSE);
}

/**********************************************************************//**
Sets the state of an xdes. */
UNIV_INLINE
void
xdes_set_state(
/*===========*/
	xdes_t*	descr,	/*!< in/out: descriptor */
	ulint	state,	/*!< in: state to set */
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(descr && mtr);
	ut_ad(state >= XDES_FREE);
	ut_ad(state <= XDES_FSEG);
	ut_ad(mtr_memo_contains_page(
			mtr, descr,
			MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX));

	mlog_write_ulint(descr + XDES_STATE, state, MLOG_4BYTES, mtr);
}

/**********************************************************************//**
Gets the state of an xdes.
@return state */
UNIV_INLINE
ulint
xdes_get_state(
/*===========*/
	const xdes_t*	descr,	/*!< in: descriptor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	state;

	ut_ad(descr && mtr);
	ut_ad(mtr_memo_contains_page(mtr, descr, MTR_MEMO_PAGE_SX_FIX));

	state = mach_read_from_4(descr + XDES_STATE);
	ut_ad(state - 1 < XDES_FSEG);
	return(state);
}

/**********************************************************************//**
Inits an extent descriptor to the free and clean state. */
UNIV_INLINE
void
xdes_init(
/*======*/
	xdes_t*	descr,	/*!< in: descriptor */
	mtr_t*	mtr)	/*!< in/out: mini-transaction */
{
  ut_ad(mtr_memo_contains_page(mtr, descr, MTR_MEMO_PAGE_SX_FIX |
                               MTR_MEMO_PAGE_X_FIX));
  mlog_memset(descr + XDES_BITMAP, XDES_SIZE - XDES_BITMAP, 0xff, mtr);
  xdes_set_state(descr, XDES_FREE, mtr);
}

/** Get pointer to a the extent descriptor of a page.
@param[in,out]	sp_header	tablespace header page, x-latched
@param[in]	space		tablespace
@param[in]	offset		page offset
@param[in,out]	mtr		mini-transaction
@param[in]	init_space	whether the tablespace is being initialized
@param[out]	desc_block	descriptor block, or NULL if it is
the same as the tablespace header
@return pointer to the extent descriptor, NULL if the page does not
exist in the space or if the offset exceeds free limit */
UNIV_INLINE MY_ATTRIBUTE((warn_unused_result))
xdes_t*
xdes_get_descriptor_with_space_hdr(
	fsp_header_t*		sp_header,
	const fil_space_t*	space,
	page_no_t		offset,
	mtr_t*			mtr,
	bool			init_space = false,
	buf_block_t**		desc_block = NULL)
{
	ulint	limit;
	ulint	size;
	ulint	descr_page_no;
	page_t*	descr_page;
	ut_ad(mtr_memo_contains(mtr, space, MTR_MEMO_SPACE_X_LOCK));
	ut_ad(mtr_memo_contains_page(mtr, sp_header, MTR_MEMO_PAGE_SX_FIX)
	      || mtr_memo_contains_page(mtr, sp_header, MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_offset(sp_header) == FSP_HEADER_OFFSET);
	/* Read free limit and space size */
	limit = mach_read_from_4(sp_header + FSP_FREE_LIMIT);
	size  = mach_read_from_4(sp_header + FSP_SIZE);
	ut_ad(limit == space->free_limit
	      || (space->free_limit == 0
		  && (init_space
		      || space->purpose == FIL_TYPE_TEMPORARY
		      || (srv_startup_is_before_trx_rollback_phase
			  && (space->id == TRX_SYS_SPACE
			      || srv_is_undo_tablespace(space->id))))));
	ut_ad(size == space->size_in_header);

	if ((offset >= size) || (offset >= limit)) {
		return(NULL);
	}

	const ulint zip_size = space->zip_size();

	descr_page_no = xdes_calc_descriptor_page(zip_size, offset);

	buf_block_t*		block;

	if (descr_page_no == 0) {
		/* It is on the space header page */

		descr_page = page_align(sp_header);
		block = NULL;
	} else {
		block = buf_page_get(
			page_id_t(space->id, descr_page_no), zip_size,
			RW_SX_LATCH, mtr);

		buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

		descr_page = buf_block_get_frame(block);
	}

	if (desc_block != NULL) {
		*desc_block = block;
	}

	return(descr_page + XDES_ARR_OFFSET
	       + XDES_SIZE * xdes_calc_descriptor_index(zip_size, offset));
}

/** Get the extent descriptor of a page.
The page where the extent descriptor resides is x-locked. If the page
offset is equal to the free limit of the space, we will add new
extents from above the free limit to the space free list, if not free
limit == space size. This adding is necessary to make the descriptor
defined, as they are uninitialized above the free limit.
@param[in]	space		tablespace
@param[in]	offset		page offset; if equal to the free limit, we
try to add new extents to the space free list
@param[in,out]	mtr		mini-transaction
@return the extent descriptor */
MY_ATTRIBUTE((warn_unused_result))
static
xdes_t*
xdes_get_descriptor(const fil_space_t* space, page_no_t offset, mtr_t* mtr)
{
	buf_block_t*	block;
	fsp_header_t*	sp_header;

	block = buf_page_get(page_id_t(space->id, 0), space->zip_size(),
			     RW_SX_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

	sp_header = FSP_HEADER_OFFSET + buf_block_get_frame(block);
	return(xdes_get_descriptor_with_space_hdr(
		       sp_header, space, offset, mtr));
}

/** Get the extent descriptor of a page.
The page where the extent descriptor resides is x-locked. If the page
offset is equal to the free limit of the space, we will add new
extents from above the free limit to the space free list, if not free
limit == space size. This adding is necessary to make the descriptor
defined, as they are uninitialized above the free limit.
@param[in]	space		tablespace
@param[in]	page		descriptor page offset
@param[in]	offset		page offset
@param[in,out]	mtr		mini-transaction
@return	the extent descriptor
@retval	NULL	if the descriptor is not available */
MY_ATTRIBUTE((warn_unused_result))
static
const xdes_t*
xdes_get_descriptor_const(
	const fil_space_t*	space,
	page_no_t		page,
	page_no_t		offset,
	mtr_t*			mtr)
{
	ut_ad(mtr_memo_contains(mtr, &space->latch, MTR_MEMO_S_LOCK));
	ut_ad(offset < space->free_limit);
	ut_ad(offset < space->size_in_header);

	const ulint zip_size = space->zip_size();

	if (buf_block_t* block = buf_page_get(page_id_t(space->id, page),
					      zip_size, RW_S_LATCH, mtr)) {
		buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

		ut_ad(page != 0 || space->free_limit == mach_read_from_4(
			      FSP_FREE_LIMIT + FSP_HEADER_OFFSET
			      + block->frame));
		ut_ad(page != 0 || space->size_in_header == mach_read_from_4(
			      FSP_SIZE + FSP_HEADER_OFFSET
			      + block->frame));

		return(block->frame + XDES_ARR_OFFSET + XDES_SIZE
		       * xdes_calc_descriptor_index(zip_size, offset));
	}

	return(NULL);
}

/** Get a pointer to the extent descriptor. The page where the
extent descriptor resides is x-locked.
@param[in]	space		tablespace
@param[in]	lst_node	file address of the list node
				contained in the descriptor
@param[in,out]	mtr		mini-transaction
@return pointer to the extent descriptor */
MY_ATTRIBUTE((nonnull, warn_unused_result))
UNIV_INLINE
xdes_t*
xdes_lst_get_descriptor(
	const fil_space_t*	space,
	fil_addr_t		lst_node,
	mtr_t*			mtr)
{
	ut_ad(mtr_memo_contains(mtr, space, MTR_MEMO_SPACE_X_LOCK));
	return fut_get_ptr(space->id, space->zip_size(),
			   lst_node, RW_SX_LATCH, mtr)
		- XDES_FLST_NODE;
}

/********************************************************************//**
Returns page offset of the first page in extent described by a descriptor.
@return offset of the first page in extent */
UNIV_INLINE
ulint
xdes_get_offset(
/*============*/
	const xdes_t*	descr)	/*!< in: extent descriptor */
{
	ut_ad(descr);

	return(page_get_page_no(page_align(descr))
	       + ((page_offset(descr) - XDES_ARR_OFFSET) / XDES_SIZE)
	       * FSP_EXTENT_SIZE);
}

/** Initialize a file page whose prior contents should be ignored.
@param[in,out]	block	buffer pool block */
void fsp_apply_init_file_page(buf_block_t* block)
{
	page_t*		page	= buf_block_get_frame(block);

	memset(page, 0, srv_page_size);

	mach_write_to_4(page + FIL_PAGE_OFFSET, block->page.id.page_no());
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
			block->page.id.space());

	if (page_zip_des_t* page_zip= buf_block_get_page_zip(block)) {
		memset(page_zip->data, 0, page_zip_get_size(page_zip));
		memcpy(page_zip->data + FIL_PAGE_OFFSET,
		       page + FIL_PAGE_OFFSET, 4);
		memcpy(page_zip->data + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
		       page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 4);
	}
}

#ifdef UNIV_DEBUG
/** Assert that the mini-transaction is compatible with
updating an allocation bitmap page.
@param[in]	mtr	mini-transaction */
void fil_space_t::modify_check(const mtr_t& mtr) const
{
	switch (mtr.get_log_mode()) {
	case MTR_LOG_SHORT_INSERTS:
	case MTR_LOG_NONE:
		/* These modes are only allowed within a non-bitmap page
		when there is a higher-level redo log record written. */
		ut_ad(purpose == FIL_TYPE_TABLESPACE
		      || purpose == FIL_TYPE_TEMPORARY);
		break;
	case MTR_LOG_NO_REDO:
		ut_ad(purpose == FIL_TYPE_TEMPORARY
		      || purpose == FIL_TYPE_IMPORT
		      || redo_skipped_count);
		return;
	case MTR_LOG_ALL:
		/* We may only write redo log for a persistent
		tablespace. */
		ut_ad(purpose == FIL_TYPE_TABLESPACE);
		ut_ad(mtr.is_named_space(id));
		return;
	}

	ut_ad(!"invalid log mode");
}
#endif

/**********************************************************************//**
Writes the space id and flags to a tablespace header.  The flags contain
row type, physical/compressed page size, and logical/uncompressed page
size of the tablespace. */
void
fsp_header_init_fields(
/*===================*/
	page_t*	page,		/*!< in/out: first page in the space */
	ulint	space_id,	/*!< in: space id */
	ulint	flags)		/*!< in: tablespace flags (FSP_SPACE_FLAGS) */
{
	flags &= ~FSP_FLAGS_MEM_MASK;
	ut_a(fil_space_t::is_valid_flags(flags, space_id));

	mach_write_to_4(FSP_HEADER_OFFSET + FSP_SPACE_ID + page,
			space_id);
	mach_write_to_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page,
			flags);
}

/** Initialize a tablespace header.
@param[in,out]	space	tablespace
@param[in]	size	current size in blocks
@param[in,out]	mtr	mini-transaction */
void fsp_header_init(fil_space_t* space, ulint size, mtr_t* mtr)
{
	const page_id_t page_id(space->id, 0);
	const ulint zip_size = space->zip_size();

	mtr_x_lock_space(space, mtr);
	buf_block_t* block = buf_page_create(page_id, zip_size, mtr);
	buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

	space->size_in_header = size;
	space->free_len = 0;
	space->free_limit = 0;

	/* The prior contents of the file page should be ignored */

	fsp_init_file_page(space, block, mtr);

	mlog_write_ulint(block->frame + FIL_PAGE_TYPE, FIL_PAGE_TYPE_FSP_HDR,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(FSP_HEADER_OFFSET + FSP_SPACE_ID + block->frame,
			 space->id, MLOG_4BYTES, mtr);
	ut_ad(0 == mach_read_from_4(FSP_HEADER_OFFSET + FSP_NOT_USED
				    + block->frame));
	mlog_write_ulint(FSP_HEADER_OFFSET + FSP_SIZE + block->frame, size,
			 MLOG_4BYTES, mtr);
	ut_ad(0 == mach_read_from_4(FSP_HEADER_OFFSET + FSP_FREE_LIMIT
				    + block->frame));
	mlog_write_ulint(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + block->frame,
			 space->flags & ~FSP_FLAGS_MEM_MASK,
			 MLOG_4BYTES, mtr);
	ut_ad(0 == mach_read_from_4(FSP_HEADER_OFFSET + FSP_FRAG_N_USED
				    + block->frame));

	flst_init(block, FSP_HEADER_OFFSET + FSP_FREE, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_FREE_FRAG, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_FULL_FRAG, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE, mtr);

	mlog_write_ull(FSP_HEADER_OFFSET + FSP_SEG_ID + block->frame, 1, mtr);

	fsp_fill_free_list(!is_system_tablespace(space->id),
			   space, FSP_HEADER_OFFSET + block->frame, mtr);

	/* Write encryption metadata to page 0 if tablespace is
	encrypted or encryption is disabled by table option. */
	if (space->crypt_data &&
	    (space->crypt_data->should_encrypt() ||
	     space->crypt_data->not_encrypted())) {
		space->crypt_data->write_page0(space, block->frame, mtr);
	}
}

/**********************************************************************//**
Reads the space id from the first page of a tablespace.
@return space id, ULINT UNDEFINED if error */
ulint
fsp_header_get_space_id(
/*====================*/
	const page_t*	page)	/*!< in: first page of a tablespace */
{
	ulint	fsp_id;
	ulint	id;

	fsp_id = mach_read_from_4(FSP_HEADER_OFFSET + page + FSP_SPACE_ID);

	id = mach_read_from_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

	DBUG_EXECUTE_IF("fsp_header_get_space_id_failure",
			id = ULINT_UNDEFINED;);

	if (id != fsp_id) {
		ib::error() << "Space ID in fsp header is " << fsp_id
			<< ", but in the page header it is " << id << ".";
		return(ULINT_UNDEFINED);
	}

	return(id);
}

/** Try to extend a single-table tablespace so that a page would fit in the
data file.
@param[in,out]	space	tablespace
@param[in]	page_no	page number
@param[in,out]	header	tablespace header
@param[in,out]	mtr	mini-transaction
@return true if success */
static ATTRIBUTE_COLD __attribute__((warn_unused_result))
bool
fsp_try_extend_data_file_with_pages(
	fil_space_t*	space,
	ulint		page_no,
	fsp_header_t*	header,
	mtr_t*		mtr)
{
	bool	success;
	ulint	size;

	ut_a(!is_system_tablespace(space->id));
	ut_d(space->modify_check(*mtr));

	size = mach_read_from_4(header + FSP_SIZE);
	ut_ad(size == space->size_in_header);

	ut_a(page_no >= size);

	success = fil_space_extend(space, page_no + 1);
	/* The size may be less than we wanted if we ran out of disk space. */
	mlog_write_ulint(header + FSP_SIZE, space->size, MLOG_4BYTES, mtr);
	space->size_in_header = space->size;

	return(success);
}

/** Calculate the number of physical pages in an extent for this file.
@param[in]	physical_size	page_size of the datafile
@return number of pages in an extent for this file */
inline ulint fsp_get_extent_size_in_pages(ulint physical_size)
{
	return (FSP_EXTENT_SIZE << srv_page_size_shift) / physical_size;
}


/** Calculate the number of pages to extend a datafile.
We extend single-table tablespaces first one extent at a time,
but 4 at a time for bigger tablespaces. It is not enough to extend always
by one extent, because we need to add at least one extent to FSP_FREE.
A single extent descriptor page will track many extents. And the extent
that uses its extent descriptor page is put onto the FSP_FREE_FRAG list.
Extents that do not use their extent descriptor page are added to FSP_FREE.
The physical page size is used to determine how many extents are tracked
on one extent descriptor page. See xdes_calc_descriptor_page().
@param[in]	physical_size	page size in data file
@param[in]	size		current number of pages in the datafile
@return number of pages to extend the file. */
static ulint fsp_get_pages_to_extend_ibd(ulint physical_size, ulint size)
{
	ulint extent_size = fsp_get_extent_size_in_pages(physical_size);
	/* The threshold is set at 32MiB except when the physical page
	size is small enough that it must be done sooner. */
	ulint threshold = std::min(32 * extent_size, physical_size);

	if (size >= threshold) {
		/* Below in fsp_fill_free_list() we assume
		that we add at most FSP_FREE_ADD extents at
		a time */
		extent_size *= FSP_FREE_ADD;
	}

	return extent_size;
}

/** Try to extend the last data file of a tablespace if it is auto-extending.
@param[in,out]	space	tablespace
@param[in,out]	header	tablespace header
@param[in,out]	mtr	mini-transaction
@return	number of pages added
@retval	0 if the tablespace was not extended */
ATTRIBUTE_COLD __attribute__((nonnull))
static
ulint
fsp_try_extend_data_file(fil_space_t* space, fsp_header_t* header, mtr_t* mtr)
{
	ulint	size;		/* current number of pages in the datafile */
	ulint	size_increase;	/* number of pages to extend this file */
	const char* OUT_OF_SPACE_MSG =
		"ran out of space. Please add another file or use"
		" 'autoextend' for the last file in setting";

	ut_d(space->modify_check(*mtr));

	if (space->id == TRX_SYS_SPACE
	    && !srv_sys_space.can_auto_extend_last_file()) {

		/* We print the error message only once to avoid
		spamming the error log. Note that we don't need
		to reset the flag to false as dealing with this
		error requires server restart. */
		if (!srv_sys_space.get_tablespace_full_status()) {
			ib::error() << "The InnoDB system tablespace "
				<< OUT_OF_SPACE_MSG
				<< " innodb_data_file_path.";
			srv_sys_space.set_tablespace_full_status(true);
		}
		return(0);
	} else if (space->id == SRV_TMP_SPACE_ID
		   && !srv_tmp_space.can_auto_extend_last_file()) {

		/* We print the error message only once to avoid
		spamming the error log. Note that we don't need
		to reset the flag to false as dealing with this
		error requires server restart. */
		if (!srv_tmp_space.get_tablespace_full_status()) {
			ib::error() << "The InnoDB temporary tablespace "
				<< OUT_OF_SPACE_MSG
				<< " innodb_temp_data_file_path.";
			srv_tmp_space.set_tablespace_full_status(true);
		}
		return(0);
	}

	size = mach_read_from_4(header + FSP_SIZE);
	ut_ad(size == space->size_in_header);

	const ulint ps = space->physical_size();

	switch (space->id) {
	case TRX_SYS_SPACE:
		size_increase = srv_sys_space.get_increment();
		break;
	case SRV_TMP_SPACE_ID:
		size_increase = srv_tmp_space.get_increment();
		break;
	default:
		ulint extent_pages = fsp_get_extent_size_in_pages(ps);
		if (size < extent_pages) {
			/* Let us first extend the file to extent_size */
			if (!fsp_try_extend_data_file_with_pages(
				    space, extent_pages - 1, header, mtr)) {
				return(0);
			}

			size = extent_pages;
		}

		size_increase = fsp_get_pages_to_extend_ibd(ps, size);
	}

	if (size_increase == 0) {
		return(0);
	}

	if (!fil_space_extend(space, size + size_increase)) {
		return(0);
	}

	/* For the system tablespace, we ignore any fragments of a
	full megabyte when storing the size to the space header */

	space->size_in_header = space->id
		? space->size
		: ut_2pow_round(space->size, (1024 * 1024) / ps);

	mlog_write_ulint(
		header + FSP_SIZE, space->size_in_header, MLOG_4BYTES, mtr);

	return(size_increase);
}

/** Reset the page type.
Data files created before MySQL 5.1.48 may contain garbage in FIL_PAGE_TYPE.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in]	block	block with invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
ATTRIBUTE_COLD
void fil_block_reset_type(const buf_block_t& block, ulint type, mtr_t* mtr)
{
	ib::info()
		<< "Resetting invalid page " << block.page.id << " type "
		<< fil_page_get_type(block.frame) << " to " << type << ".";
	mlog_write_ulint(block.frame + FIL_PAGE_TYPE, type, MLOG_2BYTES, mtr);
}

/** Put new extents to the free list if there are free extents above the free
limit. If an extent happens to contain an extent descriptor page, the extent
is put to the FSP_FREE_FRAG list with the page marked as used.
@param[in]	init_space	true if this is a single-table tablespace
and we are only initializing the first extent and the first bitmap pages;
then we will not allocate more extents
@param[in,out]	space		tablespace
@param[in,out]	header		tablespace header
@param[in,out]	mtr		mini-transaction */
static
void
fsp_fill_free_list(
	bool		init_space,
	fil_space_t*	space,
	fsp_header_t*	header,
	mtr_t*		mtr)
{
	ulint	limit;
	ulint	size;
	xdes_t*	descr;
	ulint	count		= 0;
	ulint	frag_n_used;
	ulint	i;

	ut_ad(page_offset(header) == FSP_HEADER_OFFSET);
	ut_d(space->modify_check(*mtr));

	/* Check if we can fill free list from above the free list limit */
	size = mach_read_from_4(header + FSP_SIZE);
	limit = mach_read_from_4(header + FSP_FREE_LIMIT);

	ut_ad(size == space->size_in_header);
	ut_ad(limit == space->free_limit);

	const ulint zip_size = space->zip_size();

	if (size < limit + FSP_EXTENT_SIZE * FSP_FREE_ADD) {
		bool	skip_resize	= init_space;
		switch (space->id) {
		case TRX_SYS_SPACE:
			skip_resize = !srv_sys_space.can_auto_extend_last_file();
			break;
		case SRV_TMP_SPACE_ID:
			skip_resize = !srv_tmp_space.can_auto_extend_last_file();
			break;
		}

		if (!skip_resize) {
			fsp_try_extend_data_file(space, header, mtr);
			size = space->size_in_header;
		}
	}

	i = limit;

	while ((init_space && i < 1)
	       || ((i + FSP_EXTENT_SIZE <= size) && (count < FSP_FREE_ADD))) {

		const bool init_xdes = 0
			== ut_2pow_remainder(i, ulint(space->physical_size()));

		space->free_limit = i + FSP_EXTENT_SIZE;
		mlog_write_ulint(header + FSP_FREE_LIMIT, i + FSP_EXTENT_SIZE,
				 MLOG_4BYTES, mtr);

		if (init_xdes) {

			buf_block_t*	block;

			/* We are going to initialize a new descriptor page
			and a new ibuf bitmap page: the prior contents of the
			pages should be ignored. */

			if (i > 0) {
				const page_id_t	page_id(space->id, i);

				block = buf_page_create(
					page_id, zip_size, mtr);

				buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

				fsp_init_file_page(space, block, mtr);
				mlog_write_ulint(buf_block_get_frame(block)
						 + FIL_PAGE_TYPE,
						 FIL_PAGE_TYPE_XDES,
						 MLOG_2BYTES, mtr);
			}

			if (space->purpose != FIL_TYPE_TEMPORARY) {
				const page_id_t	page_id(
					space->id,
					i + FSP_IBUF_BITMAP_OFFSET);

				block = buf_page_create(
					page_id, zip_size, mtr);

				buf_block_dbg_add_level(block, SYNC_FSP_PAGE);

				fsp_init_file_page(space, block, mtr);
				mlog_write_ulint(block->frame + FIL_PAGE_TYPE,
						 FIL_PAGE_IBUF_BITMAP,
						 MLOG_2BYTES, mtr);
			}
		}

		buf_block_t*	desc_block = NULL;
		descr = xdes_get_descriptor_with_space_hdr(
			header, space, i, mtr, init_space, &desc_block);
		if (desc_block && !space->full_crc32()) {
			fil_block_check_type(
				*desc_block, FIL_PAGE_TYPE_XDES, mtr);
		}
		xdes_init(descr, mtr);

		if (UNIV_UNLIKELY(init_xdes)) {

			/* The first page in the extent is a descriptor page
			and the second is an ibuf bitmap page: mark them
			used */

			xdes_set_bit(descr, XDES_FREE_BIT, 0, FALSE, mtr);
			xdes_set_bit(descr, XDES_FREE_BIT,
				     FSP_IBUF_BITMAP_OFFSET, FALSE, mtr);
			xdes_set_state(descr, XDES_FREE_FRAG, mtr);

			flst_add_last(header + FSP_FREE_FRAG,
				      descr + XDES_FLST_NODE, mtr);
			frag_n_used = mach_read_from_4(
				header + FSP_FRAG_N_USED);
			mlog_write_ulint(header + FSP_FRAG_N_USED,
					 frag_n_used + 2, MLOG_4BYTES, mtr);
		} else {
			flst_add_last(header + FSP_FREE,
				      descr + XDES_FLST_NODE, mtr);
			count++;
		}

		i += FSP_EXTENT_SIZE;
	}

	space->free_len += count;
}

/** Allocates a new free extent.
@param[in,out]	space		tablespace
@param[in]	hint		hint of which extent would be desirable: any
page offset in the extent goes; the hint must not be > FSP_FREE_LIMIT
@param[in,out]	mtr		mini-transaction
@return extent descriptor, NULL if cannot be allocated */
static
xdes_t*
fsp_alloc_free_extent(
	fil_space_t*		space,
	ulint			hint,
	mtr_t*			mtr)
{
	fsp_header_t*	header;
	fil_addr_t	first;
	xdes_t*		descr;
	buf_block_t*	desc_block = NULL;

	header = fsp_get_space_header(space, mtr);

	descr = xdes_get_descriptor_with_space_hdr(
		header, space, hint, mtr, false, &desc_block);

	if (desc_block && !space->full_crc32()) {
		fil_block_check_type(*desc_block, FIL_PAGE_TYPE_XDES, mtr);
	}

	if (descr && (xdes_get_state(descr, mtr) == XDES_FREE)) {
		/* Ok, we can take this extent */
	} else {
		/* Take the first extent in the free list */
		first = flst_get_first(header + FSP_FREE, mtr);

		if (fil_addr_is_null(first)) {
			fsp_fill_free_list(false, space, header, mtr);

			first = flst_get_first(header + FSP_FREE, mtr);
		}

		if (fil_addr_is_null(first)) {

			return(NULL);	/* No free extents left */
		}

		descr = xdes_lst_get_descriptor(space, first, mtr);
	}

	flst_remove(header + FSP_FREE, descr + XDES_FLST_NODE, mtr);
	space->free_len--;

	return(descr);
}

/**********************************************************************//**
Allocates a single free page from a space. */
static MY_ATTRIBUTE((nonnull))
void
fsp_alloc_from_free_frag(
/*=====================*/
	fsp_header_t*	header,	/*!< in/out: tablespace header */
	xdes_t*		descr,	/*!< in/out: extent descriptor */
	ulint		bit,	/*!< in: slot to allocate in the extent */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint		frag_n_used;

	ut_ad(xdes_get_state(descr, mtr) == XDES_FREE_FRAG);
	ut_a(xdes_mtr_get_bit(descr, XDES_FREE_BIT, bit, mtr));
	xdes_set_bit(descr, XDES_FREE_BIT, bit, FALSE, mtr);

	/* Update the FRAG_N_USED field */
	frag_n_used = mach_read_from_4(header + FSP_FRAG_N_USED);
	frag_n_used++;
	mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used, MLOG_4BYTES,
			 mtr);
	if (xdes_is_full(descr, mtr)) {
		/* The fragment is full: move it to another list */
		flst_remove(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE,
			    mtr);
		xdes_set_state(descr, XDES_FULL_FRAG, mtr);

		flst_add_last(header + FSP_FULL_FRAG, descr + XDES_FLST_NODE,
			      mtr);
		mlog_write_ulint(header + FSP_FRAG_N_USED,
				 frag_n_used - FSP_EXTENT_SIZE, MLOG_4BYTES,
				 mtr);
	}
}

/** Gets a buffer block for an allocated page.
NOTE: If init_mtr != mtr, the block will only be initialized if it was
not previously x-latched. It is assumed that the block has been
x-latched only by mtr, and freed in mtr in that case.
@param[in,out]	space		tablespace
@param[in]	offset		page number of the allocated page
@param[in,out]	mtr		mini-transaction of the allocation
@param[in,out]	init_mtr	mini-transaction for initializing the page
@return block, initialized if init_mtr==mtr
or rw_lock_x_lock_count(&block->lock) == 1 */
static
buf_block_t*
fsp_page_create(
	fil_space_t*		space,
	page_no_t		offset,
	mtr_t*			mtr,
	mtr_t*			init_mtr)
{
	buf_block_t*	block = buf_page_create(page_id_t(space->id, offset),
						space->zip_size(), init_mtr);

	if (init_mtr == mtr
	    || rw_lock_get_x_lock_count(&block->lock) == 1) {
		/* Initialize the page, unless it was already
		latched in mtr. (In this case, we would want to
		allocate another page that has not been freed in mtr.) */
		fsp_init_file_page(space, block, init_mtr);
	}

	return(block);
}

/** Allocates a single free page from a space.
The page is marked as used.
@param[in,out]	space		tablespace
@param[in]	hint		hint of which page would be desirable
@param[in,out]	mtr		mini-transaction
@param[in,out]	init_mtr	mini-transaction in which the page should be
initialized (may be the same as mtr)
@retval NULL	if no page could be allocated
@retval block	rw_lock_x_lock_count(&block->lock) == 1 if allocation succeeded
(init_mtr == mtr, or the page was not previously freed in mtr)
@retval block	(not allocated or initialized) otherwise */
static MY_ATTRIBUTE((warn_unused_result, nonnull))
buf_block_t*
fsp_alloc_free_page(
	fil_space_t*		space,
	ulint			hint,
	mtr_t*			mtr,
	mtr_t*			init_mtr)
{
	fsp_header_t*	header;
	fil_addr_t	first;
	xdes_t*		descr;
	ulint		free;
	const ulint	space_id = space->id;

	ut_d(space->modify_check(*mtr));
	header = fsp_get_space_header(space, mtr);

	/* Get the hinted descriptor */
	descr = xdes_get_descriptor_with_space_hdr(header, space, hint, mtr);

	if (descr && (xdes_get_state(descr, mtr) == XDES_FREE_FRAG)) {
		/* Ok, we can take this extent */
	} else {
		/* Else take the first extent in free_frag list */
		first = flst_get_first(header + FSP_FREE_FRAG, mtr);

		if (fil_addr_is_null(first)) {
			/* There are no partially full fragments: allocate
			a free extent and add it to the FREE_FRAG list. NOTE
			that the allocation may have as a side-effect that an
			extent containing a descriptor page is added to the
			FREE_FRAG list. But we will allocate our page from the
			the free extent anyway. */

			descr = fsp_alloc_free_extent(space, hint, mtr);

			if (descr == NULL) {
				/* No free space left */

				return(NULL);
			}

			xdes_set_state(descr, XDES_FREE_FRAG, mtr);
			flst_add_last(header + FSP_FREE_FRAG,
				      descr + XDES_FLST_NODE, mtr);
		} else {
			descr = xdes_lst_get_descriptor(space, first, mtr);
		}

		/* Reset the hint */
		hint = 0;
	}

	/* Now we have in descr an extent with at least one free page. Look
	for a free page in the extent. */

	free = xdes_find_bit(descr, XDES_FREE_BIT, TRUE,
			     hint % FSP_EXTENT_SIZE, mtr);
	if (free == ULINT_UNDEFINED) {

		ut_print_buf(stderr, ((byte*) descr) - 500, 1000);
		putc('\n', stderr);

		ut_error;
	}

	page_no_t page_no = xdes_get_offset(descr) + free;

	page_no_t space_size = mach_read_from_4(header + FSP_SIZE);
	ut_ad(space_size == space->size_in_header
	      || (space_id == TRX_SYS_SPACE
		  && srv_startup_is_before_trx_rollback_phase));

	if (space_size <= page_no) {
		/* It must be that we are extending a single-table tablespace
		whose size is still < 64 pages */

		ut_a(!is_predefined_tablespace(space_id));
		if (page_no >= FSP_EXTENT_SIZE) {
			ib::error() << "Trying to extend a single-table"
				" tablespace " << space->name << " , by single"
				" page(s) though the space size " << space_size
				<< ". Page no " << page_no << ".";
			return(NULL);
		}

		if (!fsp_try_extend_data_file_with_pages(space, page_no,
							 header, mtr)) {
			/* No disk space left */
			return(NULL);
		}
	}

	fsp_alloc_from_free_frag(header, descr, free, mtr);
	return fsp_page_create(space, page_no, mtr, init_mtr);
}

/** Frees a single page of a space.
The page is marked as free and clean.
@param[in,out]	space		tablespace
@param[in]	offset		page number
@param[in]	log		whether to write MLOG_INIT_FREE_PAGE record
@param[in,out]	mtr		mini-transaction */
static void fsp_free_page(fil_space_t* space, page_no_t offset,
			  bool log, mtr_t* mtr)
{
	fsp_header_t*	header;
	xdes_t*		descr;
	ulint		state;
	ulint		frag_n_used;

	ut_ad(mtr);
	ut_d(space->modify_check(*mtr));

	/* fprintf(stderr, "Freeing page %lu in space %lu\n", page, space); */

	header = fsp_get_space_header(space, mtr);

	descr = xdes_get_descriptor_with_space_hdr(
		header, space, offset, mtr);

	state = xdes_get_state(descr, mtr);

	if (UNIV_UNLIKELY(state != XDES_FREE_FRAG
			  && state != XDES_FULL_FRAG)) {
		ib::error() << "File space extent descriptor of page "
			<< page_id_t(space->id, offset)
			<< " has state " << state;
		/* Crash in debug version, so that we get a core dump
		of this corruption. */
		ut_ad(0);

		if (state == XDES_FREE) {
			/* We put here some fault tolerance: if the page
			is already free, return without doing anything! */

			return;
		}

		ut_error;
	}

	if (xdes_mtr_get_bit(descr, XDES_FREE_BIT,
			     offset % FSP_EXTENT_SIZE, mtr)) {

		ib::error() << "File space extent descriptor of page "
			<< page_id_t(space->id, offset)
			<< " says it is free.";
		/* Crash in debug version, so that we get a core dump
		of this corruption. */
		ut_ad(0);

		/* We put here some fault tolerance: if the page
		is already free, return without doing anything! */

		return;
	}

	if (UNIV_UNLIKELY(!log)) {
		/* The last page freed in BtrBulk::finish() must be
		written with redo logging disabled for the page
		itself. The modifications of the allocation data
		structures are covered by redo log. */
	} else if (byte* log_ptr = mlog_open(mtr, 11)) {
		log_ptr = mlog_write_initial_log_record_low(
			MLOG_INIT_FREE_PAGE, space->id, offset, log_ptr, mtr);
		mlog_close(mtr, log_ptr);
	}

	const ulint	bit = offset % FSP_EXTENT_SIZE;

	xdes_set_bit(descr, XDES_FREE_BIT, bit, TRUE, mtr);
	/* xdes_init() should have set all XDES_CLEAN_BIT */
	ut_ad(xdes_get_bit(descr, XDES_CLEAN_BIT, bit));

	frag_n_used = mach_read_from_4(header + FSP_FRAG_N_USED);
	if (state == XDES_FULL_FRAG) {
		/* The fragment was full: move it to another list */
		flst_remove(header + FSP_FULL_FRAG, descr + XDES_FLST_NODE,
			    mtr);
		xdes_set_state(descr, XDES_FREE_FRAG, mtr);
		flst_add_last(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE,
			      mtr);
		mlog_write_ulint(header + FSP_FRAG_N_USED,
				 frag_n_used + FSP_EXTENT_SIZE - 1,
				 MLOG_4BYTES, mtr);
	} else {
		ut_a(frag_n_used > 0);
		mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used - 1,
				 MLOG_4BYTES, mtr);
	}

	if (xdes_is_free(descr, mtr)) {
		/* The extent has become free: move it to another list */
		flst_remove(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE,
			    mtr);
		fsp_free_extent(space, offset, mtr);
	}
}

/** Return an extent to the free list of a space.
@param[in,out]	space		tablespace
@param[in]	offset		page number in the extent
@param[in,out]	mtr		mini-transaction */
static void fsp_free_extent(fil_space_t* space, page_no_t offset, mtr_t* mtr)
{
	fsp_header_t*	header;
	xdes_t*		descr;

	ut_ad(mtr_memo_contains(mtr, space, MTR_MEMO_SPACE_X_LOCK));

	header = fsp_get_space_header(space, mtr);

	descr = xdes_get_descriptor_with_space_hdr(
		header, space, offset, mtr);

	ut_a(xdes_get_state(descr, mtr) != XDES_FREE);

	xdes_init(descr, mtr);

	flst_add_last(header + FSP_FREE, descr + XDES_FLST_NODE, mtr);
	space->free_len++;
}

/** @return Number of segment inodes which fit on a single page */
inline ulint FSP_SEG_INODES_PER_PAGE(ulint physical_size)
{
	return (physical_size - FSEG_ARR_OFFSET - 10) / FSEG_INODE_SIZE;
}

/** Returns the nth inode slot on an inode page.
@param[in]	page		segment inode page
@param[in]	i		inode index on page
@param[in]	physical_size	page size
@param[in,out]	mtr		mini-transaction
@return segment inode */
UNIV_INLINE
fseg_inode_t*
fsp_seg_inode_page_get_nth_inode(
	page_t*			page,
	ulint			i,
	ulint			physical_size,
	mtr_t*			mtr)
{
	ut_ad(i < FSP_SEG_INODES_PER_PAGE(physical_size));
	ut_ad(mtr_memo_contains_page(mtr, page, MTR_MEMO_PAGE_SX_FIX));

	return(page + FSEG_ARR_OFFSET + FSEG_INODE_SIZE * i);
}

/** Looks for a used segment inode on a segment inode page.
@param[in]	page		segment inode page
@param[in]	physical_size	page size
@param[in,out]	mtr		mini-transaction
@return segment inode index, or ULINT_UNDEFINED if not found */
static
ulint
fsp_seg_inode_page_find_used(
	page_t*			page,
	ulint			physical_size,
	mtr_t*			mtr)
{
	ulint		i;
	fseg_inode_t*	inode;

	for (i = 0; i < FSP_SEG_INODES_PER_PAGE(physical_size); i++) {

		inode = fsp_seg_inode_page_get_nth_inode(
			page, i, physical_size, mtr);

		if (mach_read_from_8(inode + FSEG_ID)) {
			/* This is used */

			ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N)
			      == FSEG_MAGIC_N_VALUE);
			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/** Looks for an unused segment inode on a segment inode page.
@param[in]	page		segment inode page
@param[in]	i		search forward starting from this index
@param[in]	physical_size	page size
@param[in,out]	mtr		mini-transaction
@return segment inode index, or ULINT_UNDEFINED if not found */
static
ulint
fsp_seg_inode_page_find_free(
	page_t*			page,
	ulint			i,
	ulint			physical_size,
	mtr_t*			mtr)
{
	for (; i < FSP_SEG_INODES_PER_PAGE(physical_size); i++) {

		fseg_inode_t*	inode;

		inode = fsp_seg_inode_page_get_nth_inode(
			page, i, physical_size, mtr);

		if (!mach_read_from_8(inode + FSEG_ID)) {
			/* This is unused */
			return(i);
		}

		ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N)
		      == FSEG_MAGIC_N_VALUE);
	}

	return(ULINT_UNDEFINED);
}

/** Allocate a file segment inode page.
@param[in,out]	space		tablespace
@param[in,out]	space_header	tablespace header
@param[in,out]	mtr		mini-transaction
@return whether the allocation succeeded */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
bool
fsp_alloc_seg_inode_page(
	fil_space_t*	space,
	fsp_header_t*	space_header,
	mtr_t*		mtr)
{
	buf_block_t*	block;

	ut_ad(page_offset(space_header) == FSP_HEADER_OFFSET);
	ut_ad(page_get_space_id(page_align(space_header)) == space->id);

	block = fsp_alloc_free_page(space, 0, mtr, mtr);

	if (!block) {
		return(false);
	}

	buf_block_dbg_add_level(block, SYNC_FSP_PAGE);
	ut_ad(rw_lock_get_x_lock_count(&block->lock) == 1);

	mlog_write_ulint(block->frame + FIL_PAGE_TYPE, FIL_PAGE_INODE,
			 MLOG_2BYTES, mtr);

#ifdef UNIV_DEBUG
	const byte* inode = FSEG_ID + FSEG_ARR_OFFSET + block->frame;
	for (ulint i = FSP_SEG_INODES_PER_PAGE(space->physical_size()); i--;
	     inode += FSEG_INODE_SIZE) {
		ut_ad(!mach_read_from_8(inode));
	}
#endif

	flst_add_last(
		space_header + FSP_SEG_INODES_FREE,
		block->frame + FSEG_INODE_PAGE_NODE, mtr);

	return(true);
}

/** Allocate a file segment inode.
@param[in,out]	space		tablespace
@param[in,out]	space_header	tablespace header
@param[in,out]	mtr		mini-transaction
@return segment inode
@retval NULL if not enough space */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
fseg_inode_t*
fsp_alloc_seg_inode(
	fil_space_t*	space,
	fsp_header_t*	space_header,
	mtr_t*		mtr)
{
	buf_block_t*	block;
	page_t*		page;
	fseg_inode_t*	inode;
	ulint		n;

	ut_ad(page_offset(space_header) == FSP_HEADER_OFFSET);

	/* Allocate a new segment inode page if needed. */
	if (flst_get_len(space_header + FSP_SEG_INODES_FREE) == 0
	    && !fsp_alloc_seg_inode_page(space, space_header, mtr)) {
		return(NULL);
	}
	const page_id_t		page_id(
		space->id,
		flst_get_first(space_header + FSP_SEG_INODES_FREE, mtr).page);

	block = buf_page_get(page_id, space->zip_size(), RW_SX_LATCH, mtr);
	buf_block_dbg_add_level(block, SYNC_FSP_PAGE);
	if (!space->full_crc32()) {
		fil_block_check_type(*block, FIL_PAGE_INODE, mtr);
	}

	page = buf_block_get_frame(block);

	const ulint physical_size = space->physical_size();

	n = fsp_seg_inode_page_find_free(page, 0, physical_size, mtr);

	ut_a(n != ULINT_UNDEFINED);

	inode = fsp_seg_inode_page_get_nth_inode(page, n, physical_size, mtr);

	if (ULINT_UNDEFINED == fsp_seg_inode_page_find_free(page, n + 1,
							    physical_size,
							    mtr)) {
		/* There are no other unused headers left on the page: move it
		to another list */

		flst_remove(space_header + FSP_SEG_INODES_FREE,
			    page + FSEG_INODE_PAGE_NODE, mtr);

		flst_add_last(space_header + FSP_SEG_INODES_FULL,
			      page + FSEG_INODE_PAGE_NODE, mtr);
	}

	ut_ad(!mach_read_from_8(inode + FSEG_ID)
	      || mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);
	return(inode);
}

/** Frees a file segment inode.
@param[in,out]	space		tablespace
@param[in,out]	inode		segment inode
@param[in,out]	mtr		mini-transaction */
static void fsp_free_seg_inode(
	fil_space_t*		space,
	fseg_inode_t*		inode,
	mtr_t*			mtr)
{
	page_t*		page;
	fsp_header_t*	space_header;

	ut_d(space->modify_check(*mtr));

	page = page_align(inode);

	space_header = fsp_get_space_header(space, mtr);

	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);

	const ulint physical_size = space->physical_size();

	if (ULINT_UNDEFINED
	    == fsp_seg_inode_page_find_free(page, 0, physical_size, mtr)) {

		/* Move the page to another list */

		flst_remove(space_header + FSP_SEG_INODES_FULL,
			    page + FSEG_INODE_PAGE_NODE, mtr);

		flst_add_last(space_header + FSP_SEG_INODES_FREE,
			      page + FSEG_INODE_PAGE_NODE, mtr);
	}

	mlog_write_ull(inode + FSEG_ID, 0, mtr);
	mlog_write_ulint(inode + FSEG_MAGIC_N, 0xfa051ce3, MLOG_4BYTES, mtr);

	if (ULINT_UNDEFINED
	    == fsp_seg_inode_page_find_used(page, physical_size, mtr)) {

		/* There are no other used headers left on the page: free it */

		flst_remove(space_header + FSP_SEG_INODES_FREE,
			    page + FSEG_INODE_PAGE_NODE, mtr);

		fsp_free_page(space, page_get_page_no(page), true, mtr);
	}
}

/** Returns the file segment inode, page x-latched.
@param[in]	header		segment header
@param[in]	space		space id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@param[out]	block		inode block, or NULL to ignore
@return segment inode, page x-latched; NULL if the inode is free */
static
fseg_inode_t*
fseg_inode_try_get(
	fseg_header_t*		header,
	ulint			space,
	ulint			zip_size,
	mtr_t*			mtr,
	buf_block_t**		block)
{
	fil_addr_t	inode_addr;
	fseg_inode_t*	inode;

	inode_addr.page = mach_read_from_4(header + FSEG_HDR_PAGE_NO);
	inode_addr.boffset = mach_read_from_2(header + FSEG_HDR_OFFSET);
	ut_ad(space == mach_read_from_4(header + FSEG_HDR_SPACE));

	inode = fut_get_ptr(space, zip_size, inode_addr, RW_SX_LATCH, mtr,
			    block);

	if (UNIV_UNLIKELY(!mach_read_from_8(inode + FSEG_ID))) {

		inode = NULL;
	} else {
		ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N)
		      == FSEG_MAGIC_N_VALUE);
	}

	return(inode);
}

/** Returns the file segment inode, page x-latched.
@param[in]	header		segment header
@param[in]	space		space id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@param[out]	block		inode block
@return segment inode, page x-latched */
static
fseg_inode_t*
fseg_inode_get(
	fseg_header_t*		header,
	ulint			space,
	ulint			zip_size,
	mtr_t*			mtr,
	buf_block_t**		block = NULL)
{
	fseg_inode_t*	inode
		= fseg_inode_try_get(header, space, zip_size, mtr, block);
	ut_a(inode);
	return(inode);
}

/**********************************************************************//**
Gets the page number from the nth fragment page slot.
@return page number, FIL_NULL if not in use */
UNIV_INLINE
ulint
fseg_get_nth_frag_page_no(
/*======================*/
	fseg_inode_t*	inode,	/*!< in: segment inode */
	ulint		n,	/*!< in: slot index */
	mtr_t*		mtr MY_ATTRIBUTE((unused)))
				/*!< in/out: mini-transaction */
{
	ut_ad(inode && mtr);
	ut_ad(n < FSEG_FRAG_ARR_N_SLOTS);
	ut_ad(mtr_memo_contains_page(mtr, inode, MTR_MEMO_PAGE_SX_FIX));
	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);
	return(mach_read_from_4(inode + FSEG_FRAG_ARR
				+ n * FSEG_FRAG_SLOT_SIZE));
}

/**********************************************************************//**
Sets the page number in the nth fragment page slot. */
UNIV_INLINE
void
fseg_set_nth_frag_page_no(
/*======================*/
	fseg_inode_t*	inode,	/*!< in: segment inode */
	ulint		n,	/*!< in: slot index */
	ulint		page_no,/*!< in: page number to set */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ut_ad(inode && mtr);
	ut_ad(n < FSEG_FRAG_ARR_N_SLOTS);
	ut_ad(mtr_memo_contains_page(mtr, inode, MTR_MEMO_PAGE_SX_FIX));
	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);

	mlog_write_ulint(inode + FSEG_FRAG_ARR + n * FSEG_FRAG_SLOT_SIZE,
			 page_no, MLOG_4BYTES, mtr);
}

/**********************************************************************//**
Finds a fragment page slot which is free.
@return slot index; ULINT_UNDEFINED if none found */
static
ulint
fseg_find_free_frag_page_slot(
/*==========================*/
	fseg_inode_t*	inode,	/*!< in: segment inode */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	i;
	ulint	page_no;

	ut_ad(inode && mtr);

	for (i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
		page_no = fseg_get_nth_frag_page_no(inode, i, mtr);

		if (page_no == FIL_NULL) {

			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/**********************************************************************//**
Finds a fragment page slot which is used and last in the array.
@return slot index; ULINT_UNDEFINED if none found */
static
ulint
fseg_find_last_used_frag_page_slot(
/*===============================*/
	fseg_inode_t*	inode,	/*!< in: segment inode */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	i;
	ulint	page_no;

	ut_ad(inode && mtr);

	for (i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
		page_no = fseg_get_nth_frag_page_no(
			inode, FSEG_FRAG_ARR_N_SLOTS - i - 1, mtr);

		if (page_no != FIL_NULL) {

			return(FSEG_FRAG_ARR_N_SLOTS - i - 1);
		}
	}

	return(ULINT_UNDEFINED);
}

/**********************************************************************//**
Calculates reserved fragment page slots.
@return number of fragment pages */
static
ulint
fseg_get_n_frag_pages(
/*==================*/
	fseg_inode_t*	inode,	/*!< in: segment inode */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	i;
	ulint	count	= 0;

	ut_ad(inode && mtr);

	for (i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
		if (FIL_NULL != fseg_get_nth_frag_page_no(inode, i, mtr)) {
			count++;
		}
	}

	return(count);
}

/** Create a new segment.
@param space                tablespace
@param byte_offset          byte offset of the created segment header
@param mtr                  mini-transaction
@param has_done_reservation whether fsp_reserve_free_extents() was invoked
@param block                block where segment header is placed,
                            or NULL to allocate an additional page for that
@return the block where the segment header is placed, x-latched
@retval NULL if could not create segment because of lack of space */
buf_block_t*
fseg_create(fil_space_t *space, ulint byte_offset, mtr_t *mtr,
            bool has_done_reservation, buf_block_t *block)
{
	fsp_header_t*	space_header;
	fseg_inode_t*	inode;
	ib_id_t		seg_id;
	fseg_header_t*	header	= 0; /* remove warning */
	ulint		n_reserved;

	DBUG_ENTER("fseg_create");

	ut_ad(mtr);
	ut_ad(byte_offset >= FIL_PAGE_DATA);
	ut_ad(byte_offset + FSEG_HEADER_SIZE
	      <= srv_page_size - FIL_PAGE_DATA_END);

	mtr_x_lock_space(space, mtr);
	ut_d(space->modify_check(*mtr));

	if (block) {
		header = byte_offset + buf_block_get_frame(block);

		ut_ad(block->page.id.space() == space->id);

		if (!space->full_crc32()) {
			fil_block_check_type(*block, block->page.id
					     == page_id_t(TRX_SYS_SPACE,
							  TRX_SYS_PAGE_NO)
					     ? FIL_PAGE_TYPE_TRX_SYS
					     : FIL_PAGE_TYPE_SYS,
					     mtr);
		}
	}

	if (!has_done_reservation
	    && !fsp_reserve_free_extents(&n_reserved, space, 2,
					 FSP_NORMAL, mtr)) {
		DBUG_RETURN(NULL);
	}

	space_header = fsp_get_space_header(space, mtr);

	inode = fsp_alloc_seg_inode(space, space_header, mtr);

	if (inode == NULL) {
		goto funct_exit;
	}

	/* Read the next segment id from space header and increment the
	value in space header */

	seg_id = mach_read_from_8(space_header + FSP_SEG_ID);

	mlog_write_ull(space_header + FSP_SEG_ID, seg_id + 1, mtr);
	mlog_write_ull(inode + FSEG_ID, seg_id, mtr);
	ut_ad(!mach_read_from_4(inode + FSEG_NOT_FULL_N_USED));

	flst_init(inode + FSEG_FREE, mtr);
	flst_init(inode + FSEG_NOT_FULL, mtr);
	flst_init(inode + FSEG_FULL, mtr);

	mlog_write_ulint(inode + FSEG_MAGIC_N, FSEG_MAGIC_N_VALUE,
			 MLOG_4BYTES, mtr);
	compile_time_assert(FSEG_FRAG_SLOT_SIZE == 4);
	compile_time_assert(FIL_NULL == 0xffffffff);
	mlog_memset(inode + FSEG_FRAG_ARR,
		    FSEG_FRAG_SLOT_SIZE * FSEG_FRAG_ARR_N_SLOTS, 0xff, mtr);

	if (!block) {
		block = fseg_alloc_free_page_low(space, inode, 0, FSP_UP,
						 mtr, mtr
#ifdef UNIV_DEBUG
						 , has_done_reservation
#endif /* UNIV_DEBUG */
						 );

		/* The allocation cannot fail if we have already reserved a
		space for the page. */
		ut_ad(!has_done_reservation || block != NULL);

		if (block == NULL) {
			fsp_free_seg_inode(space, inode, mtr);
			goto funct_exit;
		}

		ut_ad(rw_lock_get_x_lock_count(&block->lock) == 1);

		header = byte_offset + buf_block_get_frame(block);
		mlog_write_ulint(buf_block_get_frame(block) + FIL_PAGE_TYPE,
				 FIL_PAGE_TYPE_SYS, MLOG_2BYTES, mtr);
	}

	mlog_write_ulint(header + FSEG_HDR_OFFSET,
			 page_offset(inode), MLOG_2BYTES, mtr);

	mlog_write_ulint(header + FSEG_HDR_PAGE_NO,
			 page_get_page_no(page_align(inode)),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(header + FSEG_HDR_SPACE, space->id, MLOG_4BYTES, mtr);

funct_exit:
	if (!has_done_reservation) {
		space->release_free_extents(n_reserved);
	}

	DBUG_RETURN(block);
}

/**********************************************************************//**
Calculates the number of pages reserved by a segment, and how many pages are
currently used.
@return number of reserved pages */
static
ulint
fseg_n_reserved_pages_low(
/*======================*/
	fseg_inode_t*	inode,	/*!< in: segment inode */
	ulint*		used,	/*!< out: number of pages used (not
				more than reserved) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	ret;

	ut_ad(inode && used && mtr);
	ut_ad(mtr_memo_contains_page(mtr, inode, MTR_MEMO_PAGE_SX_FIX));

	*used = mach_read_from_4(inode + FSEG_NOT_FULL_N_USED)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FULL)
		+ fseg_get_n_frag_pages(inode, mtr);

	ret = fseg_get_n_frag_pages(inode, mtr)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FREE)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_NOT_FULL)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FULL);

	return(ret);
}

/**********************************************************************//**
Calculates the number of pages reserved by a segment, and how many pages are
currently used.
@return number of reserved pages */
ulint
fseg_n_reserved_pages(
/*==================*/
	fseg_header_t*	header,	/*!< in: segment header */
	ulint*		used,	/*!< out: number of pages used (<= reserved) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint		ret;
	fseg_inode_t*	inode;
	ulint		space_id;
	fil_space_t*	space;

	space_id = page_get_space_id(page_align(header));
	space = mtr_x_lock_space(space_id, mtr);

	inode = fseg_inode_get(header, space_id, space->zip_size(), mtr);

	ret = fseg_n_reserved_pages_low(inode, used, mtr);

	return(ret);
}

/** Tries to fill the free list of a segment with consecutive free extents.
This happens if the segment is big enough to allow extents in the free list,
the free list is empty, and the extents can be allocated consecutively from
the hint onward.
@param[in]	inode		segment inode
@param[in]	space		tablespace
@param[in]	hint		hint which extent would be good as the first
extent
@param[in,out]	mtr		mini-transaction */
static
void
fseg_fill_free_list(
	fseg_inode_t*		inode,
	fil_space_t*		space,
	ulint			hint,
	mtr_t*			mtr)
{
	xdes_t*	descr;
	ulint	i;
	ib_id_t	seg_id;
	ulint	reserved;
	ulint	used;

	ut_ad(inode && mtr);
	ut_ad(!((page_offset(inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	ut_d(space->modify_check(*mtr));

	reserved = fseg_n_reserved_pages_low(inode, &used, mtr);

	if (reserved < FSEG_FREE_LIST_LIMIT * FSP_EXTENT_SIZE) {

		/* The segment is too small to allow extents in free list */

		return;
	}

	if (flst_get_len(inode + FSEG_FREE) > 0) {
		/* Free list is not empty */

		return;
	}

	for (i = 0; i < FSEG_FREE_LIST_MAX_LEN; i++) {
		descr = xdes_get_descriptor(space, hint, mtr);

		if ((descr == NULL)
		    || (XDES_FREE != xdes_get_state(descr, mtr))) {

			/* We cannot allocate the desired extent: stop */

			return;
		}

		descr = fsp_alloc_free_extent(space, hint, mtr);

		xdes_set_state(descr, XDES_FSEG, mtr);

		seg_id = mach_read_from_8(inode + FSEG_ID);
		ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N)
		      == FSEG_MAGIC_N_VALUE);
		mlog_write_ull(descr + XDES_ID, seg_id, mtr);

		flst_add_last(inode + FSEG_FREE, descr + XDES_FLST_NODE, mtr);
		hint += FSP_EXTENT_SIZE;
	}
}

/** Allocates a free extent for the segment: looks first in the free list of
the segment, then tries to allocate from the space free list.
NOTE that the extent returned still resides in the segment free list, it is
not yet taken off it!
@param[in]	inode		segment inode
@param[in,out]	space		tablespace
@param[in,out]	mtr		mini-transaction
@retval NULL	if no page could be allocated
@retval block	rw_lock_x_lock_count(&block->lock) == 1 if allocation succeeded
(init_mtr == mtr, or the page was not previously freed in mtr)
@retval block	(not allocated or initialized) otherwise */
static
xdes_t*
fseg_alloc_free_extent(
	fseg_inode_t*		inode,
	fil_space_t*		space,
	mtr_t*			mtr)
{
	xdes_t*		descr;
	ib_id_t		seg_id;
	fil_addr_t	first;

	ut_ad(!((page_offset(inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);
	ut_d(space->modify_check(*mtr));

	if (flst_get_len(inode + FSEG_FREE) > 0) {
		/* Segment free list is not empty, allocate from it */

		first = flst_get_first(inode + FSEG_FREE, mtr);

		descr = xdes_lst_get_descriptor(space, first, mtr);
	} else {
		/* Segment free list was empty, allocate from space */
		descr = fsp_alloc_free_extent(space, 0, mtr);

		if (descr == NULL) {

			return(NULL);
		}

		seg_id = mach_read_from_8(inode + FSEG_ID);

		xdes_set_state(descr, XDES_FSEG, mtr);
		mlog_write_ull(descr + XDES_ID, seg_id, mtr);
		flst_add_last(inode + FSEG_FREE, descr + XDES_FLST_NODE, mtr);

		/* Try to fill the segment free list */
		fseg_fill_free_list(inode, space,
				    xdes_get_offset(descr) + FSP_EXTENT_SIZE,
				    mtr);
	}

	return(descr);
}

/** Allocates a single free page from a segment.
This function implements the intelligent allocation strategy which tries to
minimize file space fragmentation.
@param[in,out]	space			tablespace
@param[in,out]	seg_inode		segment inode
@param[in]	hint			hint of which page would be desirable
@param[in]	direction		if the new page is needed because of
an index page split, and records are inserted there in order, into which
direction they go alphabetically: FSP_DOWN, FSP_UP, FSP_NO_DIR
@param[in,out]	mtr			mini-transaction
@param[in,out]	init_mtr		mtr or another mini-transaction in
which the page should be initialized. If init_mtr != mtr, but the page is
already latched in mtr, do not initialize the page
@param[in]	has_done_reservation	TRUE if the space has already been
reserved, in this case we will never return NULL
@retval NULL	if no page could be allocated
@retval block	rw_lock_x_lock_count(&block->lock) == 1 if allocation succeeded
(init_mtr == mtr, or the page was not previously freed in mtr)
@retval block	(not allocated or initialized) otherwise */
static
buf_block_t*
fseg_alloc_free_page_low(
	fil_space_t*		space,
	fseg_inode_t*		seg_inode,
	ulint			hint,
	byte			direction,
	mtr_t*			mtr,
	mtr_t*			init_mtr
#ifdef UNIV_DEBUG
	, ibool			has_done_reservation
#endif /* UNIV_DEBUG */
)
{
	fsp_header_t*	space_header;
	ib_id_t		seg_id;
	ulint		used;
	ulint		reserved;
	xdes_t*		descr;		/*!< extent of the hinted page */
	ulint		ret_page;	/*!< the allocated page offset, FIL_NULL
					if could not be allocated */
	xdes_t*		ret_descr;	/*!< the extent of the allocated page */
	ulint		n;
	const ulint	space_id	= space->id;

	ut_ad((direction >= FSP_UP) && (direction <= FSP_NO_DIR));
	ut_ad(mach_read_from_4(seg_inode + FSEG_MAGIC_N)
	      == FSEG_MAGIC_N_VALUE);
	ut_ad(!((page_offset(seg_inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	seg_id = mach_read_from_8(seg_inode + FSEG_ID);

	ut_ad(seg_id);
	ut_d(space->modify_check(*mtr));
	ut_ad(fil_page_get_type(page_align(seg_inode)) == FIL_PAGE_INODE);

	reserved = fseg_n_reserved_pages_low(seg_inode, &used, mtr);

	space_header = fsp_get_space_header(space, mtr);

	descr = xdes_get_descriptor_with_space_hdr(space_header, space,
						   hint, mtr);
	if (descr == NULL) {
		/* Hint outside space or too high above free limit: reset
		hint */
		/* The file space header page is always allocated. */
		hint = 0;
		descr = xdes_get_descriptor(space, hint, mtr);
	}

	/* In the big if-else below we look for ret_page and ret_descr */
	/*-------------------------------------------------------------*/
	if ((xdes_get_state(descr, mtr) == XDES_FSEG)
	    && mach_read_from_8(descr + XDES_ID) == seg_id
	    && (xdes_mtr_get_bit(descr, XDES_FREE_BIT,
				 hint % FSP_EXTENT_SIZE, mtr) == TRUE)) {
take_hinted_page:
		/* 1. We can take the hinted page
		=================================*/
		ret_descr = descr;
		ret_page = hint;
		/* Skip the check for extending the tablespace. If the
		page hint were not within the size of the tablespace,
		we would have got (descr == NULL) above and reset the hint. */
		goto got_hinted_page;
		/*-----------------------------------------------------------*/
	} else if (xdes_get_state(descr, mtr) == XDES_FREE
		   && reserved - used < reserved / FSEG_FILLFACTOR
		   && used >= FSEG_FRAG_LIMIT) {

		/* 2. We allocate the free extent from space and can take
		=========================================================
		the hinted page
		===============*/
		ret_descr = fsp_alloc_free_extent(space, hint, mtr);

		ut_a(ret_descr == descr);

		xdes_set_state(ret_descr, XDES_FSEG, mtr);
		mlog_write_ull(ret_descr + XDES_ID, seg_id, mtr);
		flst_add_last(seg_inode + FSEG_FREE,
			      ret_descr + XDES_FLST_NODE, mtr);

		/* Try to fill the segment free list */
		fseg_fill_free_list(seg_inode, space,
				    hint + FSP_EXTENT_SIZE, mtr);
		goto take_hinted_page;
		/*-----------------------------------------------------------*/
	} else if ((direction != FSP_NO_DIR)
		   && ((reserved - used) < reserved / FSEG_FILLFACTOR)
		   && (used >= FSEG_FRAG_LIMIT)
		   && (!!(ret_descr
			  = fseg_alloc_free_extent(seg_inode, space, mtr)))) {

		/* 3. We take any free extent (which was already assigned above
		===============================================================
		in the if-condition to ret_descr) and take the lowest or
		========================================================
		highest page in it, depending on the direction
		==============================================*/
		ret_page = xdes_get_offset(ret_descr);

		if (direction == FSP_DOWN) {
			ret_page += FSP_EXTENT_SIZE - 1;
		}
		ut_ad(!has_done_reservation || ret_page != FIL_NULL);
		/*-----------------------------------------------------------*/
	} else if ((xdes_get_state(descr, mtr) == XDES_FSEG)
		   && mach_read_from_8(descr + XDES_ID) == seg_id
		   && (!xdes_is_full(descr, mtr))) {

		/* 4. We can take the page from the same extent as the
		======================================================
		hinted page (and the extent already belongs to the
		==================================================
		segment)
		========*/
		ret_descr = descr;
		ret_page = xdes_get_offset(ret_descr)
			+ xdes_find_bit(ret_descr, XDES_FREE_BIT, TRUE,
					hint % FSP_EXTENT_SIZE, mtr);
		ut_ad(!has_done_reservation || ret_page != FIL_NULL);
		/*-----------------------------------------------------------*/
	} else if (reserved - used > 0) {
		/* 5. We take any unused page from the segment
		==============================================*/
		fil_addr_t	first;

		if (flst_get_len(seg_inode + FSEG_NOT_FULL) > 0) {
			first = flst_get_first(seg_inode + FSEG_NOT_FULL,
					       mtr);
		} else if (flst_get_len(seg_inode + FSEG_FREE) > 0) {
			first = flst_get_first(seg_inode + FSEG_FREE, mtr);
		} else {
			ut_ad(!has_done_reservation);
			return(NULL);
		}

		ret_descr = xdes_lst_get_descriptor(space, first, mtr);
		ret_page = xdes_get_offset(ret_descr)
			+ xdes_find_bit(ret_descr, XDES_FREE_BIT, TRUE,
					0, mtr);
		ut_ad(!has_done_reservation || ret_page != FIL_NULL);
		/*-----------------------------------------------------------*/
	} else if (used < FSEG_FRAG_LIMIT) {
		/* 6. We allocate an individual page from the space
		===================================================*/
		buf_block_t* block = fsp_alloc_free_page(
			space, hint, mtr, init_mtr);

		ut_ad(!has_done_reservation || block);

		if (block) {
			/* Put the page in the fragment page array of the
			segment */
			n = fseg_find_free_frag_page_slot(seg_inode, mtr);
			ut_a(n != ULINT_UNDEFINED);

			fseg_set_nth_frag_page_no(
				seg_inode, n, block->page.id.page_no(),
				mtr);
		}

		/* fsp_alloc_free_page() invoked fsp_init_file_page()
		already. */
		return(block);
		/*-----------------------------------------------------------*/
	} else {
		/* 7. We allocate a new extent and take its first page
		======================================================*/
		ret_descr = fseg_alloc_free_extent(seg_inode, space, mtr);

		if (ret_descr == NULL) {
			ret_page = FIL_NULL;
			ut_ad(!has_done_reservation);
		} else {
			ret_page = xdes_get_offset(ret_descr);
			ut_ad(!has_done_reservation || ret_page != FIL_NULL);
		}
	}

	if (ret_page == FIL_NULL) {
		/* Page could not be allocated */

		ut_ad(!has_done_reservation);
		return(NULL);
	}

	if (space->size <= ret_page && !is_predefined_tablespace(space_id)) {
		/* It must be that we are extending a single-table
		tablespace whose size is still < 64 pages */

		if (ret_page >= FSP_EXTENT_SIZE) {
			ib::error() << "Trying to extend '"
			<< space->chain.start->name
			<< "' by single page(s) though the"
			<< " space size " << space->size
			<< ". Page no " << ret_page << ".";
			ut_ad(!has_done_reservation);
			return(NULL);
		}

		if (!fsp_try_extend_data_file_with_pages(
			    space, ret_page, space_header, mtr)) {
			/* No disk space left */
			ut_ad(!has_done_reservation);
			return(NULL);
		}
	}

got_hinted_page:
	/* ret_descr == NULL if the block was allocated from free_frag
	(XDES_FREE_FRAG) */
	if (ret_descr != NULL) {
		/* At this point we know the extent and the page offset.
		The extent is still in the appropriate list (FSEG_NOT_FULL
		or FSEG_FREE), and the page is not yet marked as used. */

		ut_ad(xdes_get_descriptor(space, ret_page, mtr) == ret_descr);

		ut_ad(xdes_mtr_get_bit(
				ret_descr, XDES_FREE_BIT,
				ret_page % FSP_EXTENT_SIZE, mtr));

		fseg_mark_page_used(seg_inode, ret_page, ret_descr, mtr);
	}

	return fsp_page_create(space, ret_page, mtr, init_mtr);
}

/**********************************************************************//**
Allocates a single free page from a segment. This function implements
the intelligent allocation strategy which tries to minimize file space
fragmentation.
@retval NULL if no page could be allocated
@retval block, rw_lock_x_lock_count(&block->lock) == 1 if allocation succeeded
(init_mtr == mtr, or the page was not previously freed in mtr)
@retval block (not allocated or initialized) otherwise */
buf_block_t*
fseg_alloc_free_page_general(
/*=========================*/
	fseg_header_t*	seg_header,/*!< in/out: segment header */
	ulint		hint,	/*!< in: hint of which page would be
				desirable */
	byte		direction,/*!< in: if the new page is needed because
				of an index page split, and records are
				inserted there in order, into which
				direction they go alphabetically: FSP_DOWN,
				FSP_UP, FSP_NO_DIR */
	ibool		has_done_reservation, /*!< in: TRUE if the caller has
				already done the reservation for the page
				with fsp_reserve_free_extents, then there
				is no need to do the check for this individual
				page */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	mtr_t*		init_mtr)/*!< in/out: mtr or another mini-transaction
				in which the page should be initialized.
				If init_mtr!=mtr, but the page is already
				latched in mtr, do not initialize the page. */
{
	fseg_inode_t*	inode;
	ulint		space_id;
	fil_space_t*	space;
	buf_block_t*	iblock;
	buf_block_t*	block;
	ulint		n_reserved;

	space_id = page_get_space_id(page_align(seg_header));
	space = mtr_x_lock_space(space_id, mtr);
	inode = fseg_inode_get(seg_header, space_id, space->zip_size(),
			       mtr, &iblock);
	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}

	if (!has_done_reservation
	    && !fsp_reserve_free_extents(&n_reserved, space, 2,
					 FSP_NORMAL, mtr)) {
		return(NULL);
	}

	block = fseg_alloc_free_page_low(space,
					 inode, hint, direction,
					 mtr, init_mtr
#ifdef UNIV_DEBUG
					 , has_done_reservation
#endif /* UNIV_DEBUG */
					 );

	/* The allocation cannot fail if we have already reserved a
	space for the page. */
	ut_ad(!has_done_reservation || block != NULL);

	if (!has_done_reservation) {
		space->release_free_extents(n_reserved);
	}

	return(block);
}

/** Check that we have at least n_pages frag pages free in the first extent
of a single-table tablespace, and they are also physically initialized to
the data file. That is we have already extended the data file so that those
pages are inside the data file. If not, this function extends the tablespace
with pages.
@param[in,out]	space		tablespace
@param[in,out]	space_header	tablespace header, x-latched
@param[in]	size		size of the tablespace in pages,
must be less than FSP_EXTENT_SIZE
@param[in,out]	mtr		mini-transaction
@param[in]	n_pages		number of pages to reserve
@return true if there were at least n_pages free pages, or we were able
to extend */
static
bool
fsp_reserve_free_pages(
	fil_space_t*	space,
	fsp_header_t*	space_header,
	ulint		size,
	mtr_t*		mtr,
	ulint		n_pages)
{
	xdes_t*	descr;
	ulint	n_used;

	ut_a(!is_system_tablespace(space->id));
	ut_a(size < FSP_EXTENT_SIZE);

	descr = xdes_get_descriptor_with_space_hdr(
		space_header, space, 0, mtr);
	n_used = xdes_get_n_used(descr, mtr);

	ut_a(n_used <= size);

	return(size >= n_used + n_pages
	       || fsp_try_extend_data_file_with_pages(
		       space, n_used + n_pages - 1, space_header, mtr));
}

/** Reserves free pages from a tablespace. All mini-transactions which may
use several pages from the tablespace should call this function beforehand
and reserve enough free extents so that they certainly will be able
to do their operation, like a B-tree page split, fully. Reservations
must be released with function fil_space_t::release_free_extents()!

The alloc_type below has the following meaning: FSP_NORMAL means an
operation which will probably result in more space usage, like an
insert in a B-tree; FSP_UNDO means allocation to undo logs: if we are
deleting rows, then this allocation will in the long run result in
less space usage (after a purge); FSP_CLEANING means allocation done
in a physical record delete (like in a purge) or other cleaning operation
which will result in less space usage in the long run. We prefer the latter
two types of allocation: when space is scarce, FSP_NORMAL allocations
will not succeed, but the latter two allocations will succeed, if possible.
The purpose is to avoid dead end where the database is full but the
user cannot free any space because these freeing operations temporarily
reserve some space.

Single-table tablespaces whose size is < FSP_EXTENT_SIZE pages are a special
case. In this function we would liberally reserve several extents for
every page split or merge in a B-tree. But we do not want to waste disk space
if the table only occupies < FSP_EXTENT_SIZE pages. That is why we apply
different rules in that special case, just ensuring that there are n_pages
free pages available.

@param[out]	n_reserved	number of extents actually reserved; if we
				return true and the tablespace size is <
				FSP_EXTENT_SIZE pages, then this can be 0,
				otherwise it is n_ext
@param[in,out]	space		tablespace
@param[in]	n_ext		number of extents to reserve
@param[in]	alloc_type	page reservation type (FSP_BLOB, etc)
@param[in,out]	mtr		the mini transaction
@param[in]	n_pages		for small tablespaces (tablespace size is
				less than FSP_EXTENT_SIZE), number of free
				pages to reserve.
@return true if we were able to make the reservation */
bool
fsp_reserve_free_extents(
	ulint*		n_reserved,
	fil_space_t*	space,
	ulint		n_ext,
	fsp_reserve_t	alloc_type,
	mtr_t*		mtr,
	ulint		n_pages)
{
	fsp_header_t*	space_header;
	ulint		n_free_list_ext;
	ulint		free_limit;
	ulint		size;
	ulint		n_free;
	ulint		n_free_up;
	ulint		reserve;

	ut_ad(mtr);
	*n_reserved = n_ext;

	mtr_x_lock_space(space, mtr);
	const ulint physical_size = space->physical_size();

	space_header = fsp_get_space_header(space, mtr);
try_again:
	size = mach_read_from_4(space_header + FSP_SIZE);
	ut_ad(size == space->size_in_header);

	if (size < FSP_EXTENT_SIZE && n_pages < FSP_EXTENT_SIZE / 2) {
		/* Use different rules for small single-table tablespaces */
		*n_reserved = 0;
		return(fsp_reserve_free_pages(space, space_header, size,
					      mtr, n_pages));
	}

	n_free_list_ext = flst_get_len(space_header + FSP_FREE);
	ut_ad(space->free_len == n_free_list_ext);

	free_limit = mach_read_from_4(space_header + FSP_FREE_LIMIT);
	ut_ad(space->free_limit == free_limit);

	/* Below we play safe when counting free extents above the free limit:
	some of them will contain extent descriptor pages, and therefore
	will not be free extents */

	if (size >= free_limit) {
		n_free_up = (size - free_limit) / FSP_EXTENT_SIZE;
	} else {
		ut_ad(alloc_type == FSP_BLOB);
		n_free_up = 0;
	}

	if (n_free_up > 0) {
		n_free_up--;
		n_free_up -= n_free_up / (physical_size / FSP_EXTENT_SIZE);
	}

	n_free = n_free_list_ext + n_free_up;

	switch (alloc_type) {
	case FSP_NORMAL:
		/* We reserve 1 extent + 0.5 % of the space size to undo logs
		and 1 extent + 0.5 % to cleaning operations; NOTE: this source
		code is duplicated in the function below! */

		reserve = 2 + ((size / FSP_EXTENT_SIZE) * 2) / 200;

		if (n_free <= reserve + n_ext) {

			goto try_to_extend;
		}
		break;
	case FSP_UNDO:
		/* We reserve 0.5 % of the space size to cleaning operations */

		reserve = 1 + ((size / FSP_EXTENT_SIZE) * 1) / 200;

		if (n_free <= reserve + n_ext) {

			goto try_to_extend;
		}
		break;
	case FSP_CLEANING:
	case FSP_BLOB:
		reserve = 0;
		break;
	default:
		ut_error;
	}

	if (space->reserve_free_extents(n_free, n_ext)) {
		return(true);
	}
try_to_extend:
	if (fsp_try_extend_data_file(space, space_header, mtr)) {
		goto try_again;
	}

	return(false);
}

/********************************************************************//**
Marks a page used. The page must reside within the extents of the given
segment. */
static MY_ATTRIBUTE((nonnull))
void
fseg_mark_page_used(
/*================*/
	fseg_inode_t*	seg_inode,/*!< in: segment inode */
	ulint		page,	/*!< in: page offset */
	xdes_t*		descr,  /*!< in: extent descriptor */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	not_full_n_used;

	ut_ad(fil_page_get_type(page_align(seg_inode)) == FIL_PAGE_INODE);
	ut_ad(!((page_offset(seg_inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	ut_ad(mach_read_from_4(seg_inode + FSEG_MAGIC_N)
	      == FSEG_MAGIC_N_VALUE);
	ut_ad(!memcmp(seg_inode + FSEG_ID, descr + XDES_ID, 4));

	if (xdes_is_free(descr, mtr)) {
		/* We move the extent from the free list to the
		NOT_FULL list */
		flst_remove(seg_inode + FSEG_FREE, descr + XDES_FLST_NODE,
			    mtr);
		flst_add_last(seg_inode + FSEG_NOT_FULL,
			      descr + XDES_FLST_NODE, mtr);
	}

	ut_ad(xdes_mtr_get_bit(
			descr, XDES_FREE_BIT, page % FSP_EXTENT_SIZE, mtr));

	/* We mark the page as used */
	xdes_set_bit(descr, XDES_FREE_BIT, page % FSP_EXTENT_SIZE, FALSE, mtr);

	not_full_n_used = mach_read_from_4(seg_inode + FSEG_NOT_FULL_N_USED);
	not_full_n_used++;
	mlog_write_ulint(seg_inode + FSEG_NOT_FULL_N_USED, not_full_n_used,
			 MLOG_4BYTES, mtr);
	if (xdes_is_full(descr, mtr)) {
		/* We move the extent from the NOT_FULL list to the
		FULL list */
		flst_remove(seg_inode + FSEG_NOT_FULL,
			    descr + XDES_FLST_NODE, mtr);
		flst_add_last(seg_inode + FSEG_FULL,
			      descr + XDES_FLST_NODE, mtr);

		mlog_write_ulint(seg_inode + FSEG_NOT_FULL_N_USED,
				 not_full_n_used - FSP_EXTENT_SIZE,
				 MLOG_4BYTES, mtr);
	}
}

/** Frees a single page of a segment.
@param[in]	seg_inode	segment inode
@param[in,out]	space		tablespace
@param[in]	offset		page number
@param[in]	log		whether to write MLOG_INIT_FREE_PAGE record
@param[in,out]	mtr		mini-transaction */
static
void
fseg_free_page_low(
	fseg_inode_t*		seg_inode,
	fil_space_t*		space,
	page_no_t		offset,
	bool			log,
	mtr_t*			mtr)
{
	xdes_t*	descr;
	ulint	not_full_n_used;
	ulint	state;
	ib_id_t	descr_id;
	ib_id_t	seg_id;

	ut_ad(seg_inode != NULL);
	ut_ad(mtr != NULL);
	ut_ad(mach_read_from_4(seg_inode + FSEG_MAGIC_N)
	      == FSEG_MAGIC_N_VALUE);
	ut_ad(!((page_offset(seg_inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	ut_d(space->modify_check(*mtr));

	descr = xdes_get_descriptor(space, offset, mtr);

	if (xdes_mtr_get_bit(descr, XDES_FREE_BIT,
			     offset % FSP_EXTENT_SIZE, mtr)) {
		ib::fatal() << "InnoDB is trying to free page "
			<< page_id_t(space->id, offset)
			<< " though it is already marked as free in the"
			" tablespace! The tablespace free space info is"
			" corrupt. You may need to dump your tables and"
			" recreate the whole database!"
			<< FORCE_RECOVERY_MSG;
	}

	state = xdes_get_state(descr, mtr);

	if (state != XDES_FSEG) {
		/* The page is in the fragment pages of the segment */
		for (ulint i = 0;; i++) {
			if (fseg_get_nth_frag_page_no(seg_inode, i, mtr)
			    != offset) {
				continue;
			}

			compile_time_assert(FIL_NULL == 0xffffffff);
			mlog_memset(seg_inode + FSEG_FRAG_ARR
				    + i * FSEG_FRAG_SLOT_SIZE, 4, 0xff, mtr);
			break;
		}

		fsp_free_page(space, offset, log, mtr);
		return;
	}

	/* If we get here, the page is in some extent of the segment */

	descr_id = mach_read_from_8(descr + XDES_ID);
	seg_id = mach_read_from_8(seg_inode + FSEG_ID);

	if (UNIV_UNLIKELY(descr_id != seg_id)) {
		fputs("InnoDB: Dump of the tablespace extent descriptor: ",
		      stderr);
		ut_print_buf(stderr, descr, 40);
		fputs("\nInnoDB: Dump of the segment inode: ", stderr);
		ut_print_buf(stderr, seg_inode, 40);
		putc('\n', stderr);

		ib::fatal() << "InnoDB is trying to free page "
			<< page_id_t(space->id, offset)
			<< ", which does not belong to segment " << descr_id
			<< " but belongs to segment " << seg_id << "."
			<< FORCE_RECOVERY_MSG;
	}

	not_full_n_used = mach_read_from_4(seg_inode + FSEG_NOT_FULL_N_USED);
	if (xdes_is_full(descr, mtr)) {
		/* The fragment is full: move it to another list */
		flst_remove(seg_inode + FSEG_FULL,
			    descr + XDES_FLST_NODE, mtr);
		flst_add_last(seg_inode + FSEG_NOT_FULL,
			      descr + XDES_FLST_NODE, mtr);
		mlog_write_ulint(seg_inode + FSEG_NOT_FULL_N_USED,
				 not_full_n_used + FSP_EXTENT_SIZE - 1,
				 MLOG_4BYTES, mtr);
	} else {
		ut_a(not_full_n_used > 0);
		mlog_write_ulint(seg_inode + FSEG_NOT_FULL_N_USED,
				 not_full_n_used - 1, MLOG_4BYTES, mtr);
	}

	const ulint	bit = offset % FSP_EXTENT_SIZE;

	xdes_set_bit(descr, XDES_FREE_BIT, bit, TRUE, mtr);
	/* xdes_init() should have set all XDES_CLEAN_BIT */
	ut_ad(xdes_get_bit(descr, XDES_CLEAN_BIT, bit));

	if (xdes_is_free(descr, mtr)) {
		/* The extent has become free: free it to space */
		flst_remove(seg_inode + FSEG_NOT_FULL,
			    descr + XDES_FLST_NODE, mtr);
		fsp_free_extent(space, offset, mtr);
	}
}

/** Free a page in a file segment.
@param[in,out]	seg_header	file segment header
@param[in,out]	space		tablespace
@param[in]	offset		page number
@param[in]	log		whether to write MLOG_INIT_FREE_PAGE record
@param[in,out]	mtr		mini-transaction */
void
fseg_free_page(
	fseg_header_t*	seg_header,
	fil_space_t*	space,
	ulint		offset,
	bool		log,
	mtr_t*		mtr)
{
	DBUG_ENTER("fseg_free_page");
	fseg_inode_t*		seg_inode;
	buf_block_t*		iblock;
	mtr_x_lock_space(space, mtr);

	DBUG_LOG("fseg_free_page", "space_id: " << space->id
		 << ", page_no: " << offset);

	seg_inode = fseg_inode_get(seg_header, space->id, space->zip_size(),
				   mtr,
				   &iblock);
	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}

	fseg_free_page_low(seg_inode, space, offset, log, mtr);

	ut_d(buf_page_set_file_page_was_freed(page_id_t(space->id, offset)));

	DBUG_VOID_RETURN;
}

/** Determine whether a page is free.
@param[in,out]	space	tablespace
@param[in]	page	page number
@return whether the page is marked as free */
bool
fseg_page_is_free(fil_space_t* space, unsigned page)
{
	bool		is_free;
	mtr_t		mtr;
	page_no_t	dpage = xdes_calc_descriptor_page(space->zip_size(),
							  page);

	mtr.start();
	mtr_s_lock_space(space, &mtr);

	if (page >= space->free_limit || page >= space->size_in_header) {
		is_free = true;
	} else if (const xdes_t* descr = xdes_get_descriptor_const(
			   space, dpage, page, &mtr)) {
		is_free = xdes_get_bit(descr, XDES_FREE_BIT,
				       page % FSP_EXTENT_SIZE);
	} else {
		is_free = true;
	}
	mtr.commit();

	return(is_free);
}

/** Free an extent of a segment to the space free list.
@param[in,out]	seg_inode	segment inode
@param[in,out]	space		tablespace
@param[in]	page		page number in the extent
@param[in,out]	mtr		mini-transaction */
MY_ATTRIBUTE((nonnull))
static
void
fseg_free_extent(
	fseg_inode_t*		seg_inode,
	fil_space_t*		space,
	ulint			page,
	mtr_t*			mtr)
{
	xdes_t*	descr;
	ulint	not_full_n_used;
	ulint	descr_n_used;

	ut_ad(mtr != NULL);

	descr = xdes_get_descriptor(space, page, mtr);

	ut_a(xdes_get_state(descr, mtr) == XDES_FSEG);
	ut_a(!memcmp(descr + XDES_ID, seg_inode + FSEG_ID, 8));
	ut_ad(mach_read_from_4(seg_inode + FSEG_MAGIC_N)
	      == FSEG_MAGIC_N_VALUE);
	ut_d(space->modify_check(*mtr));
	ut_d(ulint first_page_in_extent = page - (page % FSP_EXTENT_SIZE));

	if (xdes_is_full(descr, mtr)) {
		flst_remove(seg_inode + FSEG_FULL,
			    descr + XDES_FLST_NODE, mtr);
	} else if (xdes_is_free(descr, mtr)) {
		flst_remove(seg_inode + FSEG_FREE,
			    descr + XDES_FLST_NODE, mtr);
	} else {
		flst_remove(seg_inode + FSEG_NOT_FULL,
			    descr + XDES_FLST_NODE, mtr);

		not_full_n_used = mach_read_from_4(FSEG_NOT_FULL_N_USED
						   + seg_inode);
		descr_n_used = xdes_get_n_used(descr, mtr);
		ut_a(not_full_n_used >= descr_n_used);
		mlog_write_ulint(seg_inode + FSEG_NOT_FULL_N_USED,
				 not_full_n_used - descr_n_used,
				 MLOG_4BYTES, mtr);
	}

	fsp_free_extent(space, page, mtr);

#ifdef UNIV_DEBUG
	for (ulint i = 0; i < FSP_EXTENT_SIZE; i++) {

		buf_page_set_file_page_was_freed(
			page_id_t(space->id, first_page_in_extent + i));
	}
#endif /* UNIV_DEBUG */
}

/**********************************************************************//**
Frees part of a segment. This function can be used to free a segment by
repeatedly calling this function in different mini-transactions. Doing
the freeing in a single mini-transaction might result in too big a
mini-transaction.
@return whether the freeing was completed */
bool
fseg_free_step(
	fseg_header_t*	header,	/*!< in, own: segment header; NOTE: if the header
				resides on the first page of the frag list
				of the segment, this pointer becomes obsolete
				after the last freeing step */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint		n;
	ulint		page;
	xdes_t*		descr;
	fseg_inode_t*	inode;
	ulint		space_id;
	ulint		header_page;

	DBUG_ENTER("fseg_free_step");

	space_id = page_get_space_id(page_align(header));
	header_page = page_get_page_no(page_align(header));

	fil_space_t*		space = mtr_x_lock_space(space_id, mtr);

	descr = xdes_get_descriptor(space, header_page, mtr);

	/* Check that the header resides on a page which has not been
	freed yet */

	ut_a(xdes_mtr_get_bit(descr, XDES_FREE_BIT,
			      header_page % FSP_EXTENT_SIZE, mtr) == FALSE);
	buf_block_t*		iblock;
	const ulint zip_size = space->zip_size();
	inode = fseg_inode_try_get(header, space_id, zip_size, mtr, &iblock);

	if (inode == NULL) {
		ib::info() << "Double free of inode from "
			<< page_id_t(space_id, header_page);
		DBUG_RETURN(true);
	}

	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}
	descr = fseg_get_first_extent(inode, space, mtr);

	if (descr != NULL) {
		/* Free the extent held by the segment */
		page = xdes_get_offset(descr);
		fseg_free_extent(inode, space, page, mtr);
		DBUG_RETURN(false);
	}

	/* Free a frag page */
	n = fseg_find_last_used_frag_page_slot(inode, mtr);

	if (n == ULINT_UNDEFINED) {
		/* Freeing completed: free the segment inode */
		fsp_free_seg_inode(space, inode, mtr);

		DBUG_RETURN(true);
	}

	fseg_free_page_low(
		inode, space,
		fseg_get_nth_frag_page_no(inode, n, mtr),
		true, mtr);

	n = fseg_find_last_used_frag_page_slot(inode, mtr);

	if (n == ULINT_UNDEFINED) {
		/* Freeing completed: free the segment inode */
		fsp_free_seg_inode(space, inode, mtr);

		DBUG_RETURN(true);
	}

	DBUG_RETURN(false);
}

/**********************************************************************//**
Frees part of a segment. Differs from fseg_free_step because this function
leaves the header page unfreed.
@return whether the freeing was completed, except for the header page */
bool
fseg_free_step_not_header(
	fseg_header_t*	header,	/*!< in: segment header which must reside on
				the first fragment page of the segment */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint		n;
	ulint		page;
	xdes_t*		descr;
	fseg_inode_t*	inode;
	ulint		space_id;
	ulint		page_no;

	space_id = page_get_space_id(page_align(header));
	ut_ad(mtr->is_named_space(space_id));

	fil_space_t*		space = mtr_x_lock_space(space_id, mtr);
	buf_block_t*		iblock;

	inode = fseg_inode_get(header, space_id, space->zip_size(), mtr,
			       &iblock);
	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}

	descr = fseg_get_first_extent(inode, space, mtr);

	if (descr != NULL) {
		/* Free the extent held by the segment */
		page = xdes_get_offset(descr);
		fseg_free_extent(inode, space, page, mtr);
		return false;
	}

	/* Free a frag page */

	n = fseg_find_last_used_frag_page_slot(inode, mtr);

	ut_a(n != ULINT_UNDEFINED);

	page_no = fseg_get_nth_frag_page_no(inode, n, mtr);

	if (page_no == page_get_page_no(page_align(header))) {
		return true;
	}

	fseg_free_page_low(inode, space, page_no, true, mtr);
	return false;
}

/** Returns the first extent descriptor for a segment.
We think of the extent lists of the segment catenated in the order
FSEG_FULL -> FSEG_NOT_FULL -> FSEG_FREE.
@param[in]	inode		segment inode
@param[in]	space		tablespace
@param[in,out]	mtr		mini-transaction
@return the first extent descriptor, or NULL if none */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
xdes_t*
fseg_get_first_extent(
	fseg_inode_t*		inode,
	const fil_space_t*	space,
	mtr_t*			mtr)
{
	fil_addr_t	first;

	ut_ad(space->id == page_get_space_id(page_align(inode)));
	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);

	if (flst_get_len(inode + FSEG_FULL) > 0) {

		first = flst_get_first(inode + FSEG_FULL, mtr);

	} else if (flst_get_len(inode + FSEG_NOT_FULL) > 0) {

		first = flst_get_first(inode + FSEG_NOT_FULL, mtr);

	} else if (flst_get_len(inode + FSEG_FREE) > 0) {

		first = flst_get_first(inode + FSEG_FREE, mtr);
	} else {
		return(NULL);
	}

	ut_ad(first.page != FIL_NULL);

	return(first.page == FIL_NULL ? NULL
	       : xdes_lst_get_descriptor(space, first, mtr));
}

#ifdef UNIV_BTR_PRINT
/*******************************************************************//**
Writes info of a segment. */
static
void
fseg_print_low(
/*===========*/
	fseg_inode_t*	inode, /*!< in: segment inode */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	ulint	space;
	ulint	n_used;
	ulint	n_frag;
	ulint	n_free;
	ulint	n_not_full;
	ulint	n_full;
	ulint	reserved;
	ulint	used;
	ulint	page_no;
	ib_id_t	seg_id;

	ut_ad(mtr_memo_contains_page(mtr, inode, MTR_MEMO_PAGE_X_FIX));
	space = page_get_space_id(page_align(inode));
	page_no = page_get_page_no(page_align(inode));

	reserved = fseg_n_reserved_pages_low(inode, &used, mtr);

	seg_id = mach_read_from_8(inode + FSEG_ID);
	n_used = mach_read_from_4(inode + FSEG_NOT_FULL_N_USED);
	n_frag = fseg_get_n_frag_pages(inode, mtr);
	n_free = flst_get_len(inode + FSEG_FREE);
	n_not_full = flst_get_len(inode + FSEG_NOT_FULL);
	n_full = flst_get_len(inode + FSEG_FULL);

	ib::info() << "SEGMENT id " << seg_id
		<< " space " << space << ";"
		<< " page " << page_no << ";"
		<< " res " << reserved << " used " << used << ";"
		<< " full ext " << n_full << ";"
		<< " fragm pages " << n_frag << ";"
		<< " free extents " << n_free << ";"
		<< " not full extents " << n_not_full << ": pages " << n_used;

	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);
}

/*******************************************************************//**
Writes info of a segment. */
void
fseg_print(
/*=======*/
	fseg_header_t*	header, /*!< in: segment header */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	fseg_inode_t*	inode;
	ulint		space_id;

	space_id = page_get_space_id(page_align(header));
	const fil_space_t*	space = mtr_x_lock_space(space_id, mtr);

	inode = fseg_inode_get(header, space_id, space->zip_size(), mtr);

	fseg_print_low(inode, mtr);
}
#endif /* UNIV_BTR_PRINT */

#ifdef UNIV_DEBUG
std::ostream &fseg_header::to_stream(std::ostream &out) const
{
  out << "[fseg_header_t: space="
      << mach_read_from_4(m_header + FSEG_HDR_SPACE)
      << ", page=" << mach_read_from_4(m_header + FSEG_HDR_PAGE_NO)
      << ", offset=" << mach_read_from_2(m_header + FSEG_HDR_OFFSET) << "]";
  return out;
}
#endif /* UNIV_DEBUG */
