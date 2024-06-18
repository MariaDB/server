/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/******************************************************************//**
@file fsp/fsp0fsp.cc
File space management

Created 11/29/1995 Heikki Tuuri
***********************************************************************/

#include "fsp0fsp.h"
#include "fil0crypt.h"
#include "mtr0log.h"
#include "page0page.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "btr0btr.h"
#include "btr0sea.h"
#include "dict0boot.h"
#include "log0log.h"
#include "dict0load.h"
#include "dict0mem.h"
#include "btr0pcur.h"
#include "trx0sys.h"
#include "log.h"
#ifndef DBUG_OFF
# include "trx0purge.h"
#endif
#include <unordered_map>
#include <unordered_set>
#include "trx0undo.h"
#include "trx0trx.h"

/** Returns the first extent descriptor for a segment.
We think of the extent lists of the segment catenated in the order
FSEG_FULL -> FSEG_NOT_FULL -> FSEG_FREE.
@param[in]	inode		segment inode
@param[in]	space		tablespace
@param[in,out]	mtr		mini-transaction
@param[out]	err		error code
@return the first extent descriptor, or NULL if none */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
xdes_t*
fseg_get_first_extent(
	fseg_inode_t*		inode,
	const fil_space_t*	space,
	mtr_t*			mtr,
	dberr_t*		err);

ATTRIBUTE_COLD MY_ATTRIBUTE((nonnull, warn_unused_result))
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
dberr_t
fsp_fill_free_list(
	bool		init_space,
	fil_space_t*	space,
	buf_block_t*	header,
	mtr_t*		mtr);

/** Allocates a single free page from a segment.
This function implements the intelligent allocation strategy which tries to
minimize file space fragmentation.
@param[in,out]	space			tablespace
@param[in,out]	seg_inode		segment inode
@param[in,out]	iblock			segment inode page
@param[in]	hint			hint of which page would be desirable
@param[in]	direction		if the new page is needed because of
an index page split, and records are inserted there in order, into which
direction they go alphabetically: FSP_DOWN, FSP_UP, FSP_NO_DIR
@param[in,out]	mtr			mini-transaction
@param[in,out]	init_mtr		mtr or another mini-transaction in
which the page should be initialized.
@param[out]	err			error code
@return the allocated page
@retval nullptr	if no page could be allocated */
static
buf_block_t*
fseg_alloc_free_page_low(
	fil_space_t*		space,
	fseg_inode_t*		seg_inode,
	buf_block_t*		iblock,
	uint32_t		hint,
	byte			direction,
#ifdef UNIV_DEBUG
	bool			has_done_reservation,
	/*!< whether the space has already been reserved */
#endif /* UNIV_DEBUG */
	mtr_t*			mtr,
	mtr_t*			init_mtr,
	dberr_t*		err)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Get the tablespace header block, SX-latched
@param[in]      space           tablespace
@param[in,out]  mtr             mini-transaction
@param[out]     err             error code
@return pointer to the space header, page x-locked
@retval nullptr if the page cannot be retrieved or is corrupted */
static buf_block_t *fsp_get_header(const fil_space_t *space, mtr_t *mtr,
                                   dberr_t *err)
{
  const page_id_t id{space->id, 0};
  buf_block_t *block= mtr->get_already_latched(id, MTR_MEMO_PAGE_SX_FIX);
  if (block)
    *err= DB_SUCCESS;
  else
  {
    block= buf_page_get_gen(id, space->zip_size(), RW_SX_LATCH,
                            nullptr, BUF_GET_POSSIBLY_FREED,
                            mtr, err);
    if (block &&
        space->id != mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_ID +
                                      block->page.frame))
    {
      *err= DB_CORRUPTION;
      block= nullptr;
    }
  }
  return block;
}

/** Set the XDES_FREE_BIT of a page.
@tparam         free    desired value of XDES_FREE_BIT
@param[in]      block   extent descriptor block
@param[in,out]  descr   extent descriptor
@param[in]      offset  page offset within the extent
@param[in,out]  mtr     mini-transaction */
template<bool free>
inline void xdes_set_free(const buf_block_t &block, xdes_t *descr,
                          ulint offset, mtr_t *mtr)
{
  ut_ad(mtr->memo_contains_flagged(&block, MTR_MEMO_PAGE_SX_FIX |
                                   MTR_MEMO_PAGE_X_FIX));
  ut_ad(offset < FSP_EXTENT_SIZE);
  ut_ad(page_align(descr) == block.page.frame);
  compile_time_assert(XDES_BITS_PER_PAGE == 2);
  compile_time_assert(XDES_FREE_BIT == 0);
  compile_time_assert(XDES_CLEAN_BIT == 1);

  ulint index= XDES_BITS_PER_PAGE * offset;
  byte *b= &descr[XDES_BITMAP + (index >> 3)];
  /* xdes_init() should have set all XDES_CLEAN_BIT. */
  ut_ad(!(~*b & 0xaa));
  /* Clear or set XDES_FREE_BIT. */
  byte val= free
    ? static_cast<byte>(*b | 1 << (index & 7))
    : static_cast<byte>(*b & ~(1 << (index & 7)));
  mtr->write<1>(block, b, val);
}

/**
Find a free page.
@param descr   extent descriptor
@param hint    page offset to start searching from (towards larger pages)
@return free page offset
@retval FIL_NULL if no page is free */
inline uint32_t xdes_find_free(const xdes_t *descr, uint32_t hint= 0)
{
  const uint32_t extent_size= FSP_EXTENT_SIZE;
  ut_ad(hint < extent_size);
  for (uint32_t i= hint; i < extent_size; i++)
    if (xdes_is_free(descr, i))
      return i;
  for (uint32_t i= 0; i < hint; i++)
    if (xdes_is_free(descr, i))
      return i;
  return FIL_NULL;
}

/**
Determine the number of used pages in a descriptor.
@param descr  file descriptor
@return number of pages used */
inline uint32_t xdes_get_n_used(const xdes_t *descr)
{
  uint32_t count= 0;

  for (uint32_t i= FSP_EXTENT_SIZE; i--; )
    if (!xdes_is_free(descr, i))
      count++;

  return count;
}

/**
Determine whether a file extent is full.
@param descr  file descriptor
@return whether all pages have been allocated */
inline bool xdes_is_full(const xdes_t *descr)
{
  return FSP_EXTENT_SIZE == xdes_get_n_used(descr);
}

/** Set the state of an extent descriptor.
@param[in]      block   extent descriptor block
@param[in,out]  descr   extent descriptor
@param[in]      state   the state
@param[in,out]  mtr     mini-transaction */
inline void xdes_set_state(const buf_block_t &block, xdes_t *descr,
                           byte state, mtr_t *mtr)
{
  ut_ad(descr && mtr);
  ut_ad(state >= XDES_FREE);
  ut_ad(state <= XDES_FSEG);
  ut_ad(mtr->memo_contains_flagged(&block, MTR_MEMO_PAGE_SX_FIX |
                                   MTR_MEMO_PAGE_X_FIX));
  ut_ad(page_align(descr) == block.page.frame);
  ut_ad(mach_read_from_4(descr + XDES_STATE) <= XDES_FSEG);
  mtr->write<1>(block, XDES_STATE + 3 + descr, state);
}

/**********************************************************************//**
Gets the state of an xdes.
@return state */
UNIV_INLINE
uint32_t
xdes_get_state(
/*===========*/
	const xdes_t*	descr)	/*!< in: descriptor */
{
	ut_ad(descr);
	uint32_t state = mach_read_from_4(descr + XDES_STATE);
	ut_ad(state - 1 < XDES_FSEG);
	return(state);
}

/**********************************************************************//**
Inits an extent descriptor to the free and clean state. */
inline void xdes_init(const buf_block_t &block, xdes_t *descr, mtr_t *mtr)
{
  ut_ad(mtr->memo_contains_flagged(&block, MTR_MEMO_PAGE_SX_FIX |
                                   MTR_MEMO_PAGE_X_FIX));
  mtr->memset(&block, uint16_t(descr - block.page.frame) + XDES_BITMAP,
              XDES_SIZE - XDES_BITMAP, 0xff);
  xdes_set_state(block, descr, XDES_FREE, mtr);
}

/** Mark a page used in an extent descriptor.
@param[in]      space           tablespace
@param[in,out]  seg_inode       segment inode
@param[in,out]  iblock          segment inode page
@param[in]      page            page number
@param[in,out]  descr           extent descriptor
@param[in,out]  xdes            extent descriptor page
@param[in,out]  mtr             mini-transaction
@return error code */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
fseg_mark_page_used(const fil_space_t *space,
                    fseg_inode_t *seg_inode, buf_block_t *iblock,
                    uint32_t page, xdes_t *descr, buf_block_t *xdes, mtr_t *mtr)
{
  ut_ad(fil_page_get_type(iblock->page.frame) == FIL_PAGE_INODE);
  ut_ad(!((page_offset(seg_inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
  ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + seg_inode, 4));
  ut_ad(!memcmp(seg_inode + FSEG_ID, descr + XDES_ID, 4));

  const uint16_t xoffset= uint16_t(descr - xdes->page.frame + XDES_FLST_NODE);
  const uint16_t ioffset= uint16_t(seg_inode - iblock->page.frame);
  const uint32_t limit= space->free_limit;

  if (!xdes_get_n_used(descr))
  {
    /* We move the extent from the free list to the NOT_FULL list */
    if (dberr_t err= flst_remove(iblock, uint16_t(FSEG_FREE + ioffset),
                                 xdes, xoffset, limit, mtr))
      return err;
    if (dberr_t err= flst_add_last(iblock, uint16_t(FSEG_NOT_FULL + ioffset),
                                   xdes, xoffset, limit, mtr))
      return err;
  }

  if (UNIV_UNLIKELY(!xdes_is_free(descr, page % FSP_EXTENT_SIZE)))
    return DB_CORRUPTION;

  /* We mark the page as used */
  xdes_set_free<false>(*xdes, descr, page % FSP_EXTENT_SIZE, mtr);

  byte* p_not_full= seg_inode + FSEG_NOT_FULL_N_USED;
  const uint32_t not_full_n_used= mach_read_from_4(p_not_full) + 1;
  mtr->write<4>(*iblock, p_not_full, not_full_n_used);
  if (xdes_is_full(descr))
  {
    /* We move the extent from the NOT_FULL list to the FULL list */
    if (dberr_t err= flst_remove(iblock, uint16_t(FSEG_NOT_FULL + ioffset),
                                 xdes, xoffset, limit, mtr))
      return err;
    if (dberr_t err= flst_add_last(iblock, uint16_t(FSEG_FULL + ioffset),
                                   xdes, xoffset, limit, mtr))
      return err;
    mtr->write<4>(*iblock, seg_inode + FSEG_NOT_FULL_N_USED,
                  not_full_n_used - FSP_EXTENT_SIZE);
  }

  return DB_SUCCESS;
}

/** Get pointer to a the extent descriptor of a page.
@param[in,out]	sp_header	tablespace header page, x-latched
@param[in]	space		tablespace
@param[in]	offset		page offset
@param[in,out]	mtr		mini-transaction
@param[out]	err		error code
@param[out]	desc_block	descriptor block
@param[in]	init_space	whether the tablespace is being initialized
@return pointer to the extent descriptor, NULL if the page does not
exist in the space or if the offset exceeds free limit */
UNIV_INLINE MY_ATTRIBUTE((warn_unused_result))
xdes_t*
xdes_get_descriptor_with_space_hdr(
	buf_block_t*		header,
	const fil_space_t*	space,
	uint32_t		offset,
	mtr_t*			mtr,
	dberr_t*		err = nullptr,
	buf_block_t**		desc_block = nullptr,
	bool			init_space = false)
{
	ut_ad(space->is_owner());
	ut_ad(mtr->memo_contains_flagged(header, MTR_MEMO_PAGE_SX_FIX
					 | MTR_MEMO_PAGE_X_FIX));
	/* Read free limit and space size */
	uint32_t limit = mach_read_from_4(FSP_HEADER_OFFSET + FSP_FREE_LIMIT
					  + header->page.frame);
	uint32_t size  = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE
					  + header->page.frame);
	ut_ad(limit == space->free_limit
	      || (space->free_limit == 0
		  && (init_space
		      || space->is_temporary()
		      || (srv_startup_is_before_trx_rollback_phase
			  && (space->id == TRX_SYS_SPACE
			      || srv_is_undo_tablespace(space->id))))));
	ut_ad(size == space->size_in_header);

	if (offset >= size || offset >= limit) {
		return nullptr;
	}

	const unsigned zip_size = space->zip_size();

	uint32_t descr_page_no = xdes_calc_descriptor_page(zip_size, offset);

	buf_block_t* block = header;

	if (descr_page_no) {
		block = buf_page_get_gen(page_id_t(space->id, descr_page_no),
					 zip_size, RW_SX_LATCH, nullptr,
					 BUF_GET_POSSIBLY_FREED, mtr, err);
	}

	if (desc_block) {
		*desc_block = block;
	}

	return block
		? XDES_ARR_OFFSET + XDES_SIZE
		* xdes_calc_descriptor_index(zip_size, offset)
		+ block->page.frame
		: nullptr;
}

MY_ATTRIBUTE((nonnull(1,3), warn_unused_result))
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
@param[out]	err		error code
@param[out]	xdes		extent descriptor page
@return the extent descriptor */
static xdes_t *xdes_get_descriptor(const fil_space_t *space, uint32_t offset,
                                   mtr_t *mtr, dberr_t *err= nullptr,
                                   buf_block_t **xdes= nullptr)
{
  if (buf_block_t *block=
      buf_page_get_gen(page_id_t(space->id, 0), space->zip_size(), RW_SX_LATCH,
                       nullptr, BUF_GET_POSSIBLY_FREED, mtr, err))
    return xdes_get_descriptor_with_space_hdr(block, space, offset, mtr,
                                              err, xdes);
  return nullptr;
}

MY_ATTRIBUTE((nonnull(3), warn_unused_result))
/** Get a pointer to the extent descriptor. The page where the
extent descriptor resides is x-locked.
@param space    tablespace
@param lst_node file address of the list node contained in the descriptor
@param mtr      mini-transaction
@param err      error code
@param block    extent descriptor block
@return pointer to the extent descriptor */
static inline
xdes_t *xdes_lst_get_descriptor(const fil_space_t &space, fil_addr_t lst_node,
                                mtr_t *mtr, buf_block_t **block= nullptr,
                                dberr_t *err= nullptr)
{
  ut_ad(mtr->memo_contains(space));
  ut_ad(lst_node.boffset < space.physical_size());
  buf_block_t *b;
  if (!block)
    block= &b;
  *block= buf_page_get_gen(page_id_t{space.id, lst_node.page},
                           space.zip_size(), RW_SX_LATCH,
                           nullptr, BUF_GET_POSSIBLY_FREED, mtr, err);
  if (*block)
    return (*block)->page.frame + lst_node.boffset - XDES_FLST_NODE;

  space.set_corrupted();
  return nullptr;
}

/********************************************************************//**
Returns page offset of the first page in extent described by a descriptor.
@return offset of the first page in extent */
static uint32_t xdes_get_offset(const xdes_t *descr)
{
  ut_ad(descr);
  const page_t *page= page_align(descr);
  return page_get_page_no(page) +
    uint32_t(((descr - page - XDES_ARR_OFFSET) / XDES_SIZE) *
             FSP_EXTENT_SIZE);
}

/** Initialize a file page whose prior contents should be ignored.
@param[in,out]	block	buffer pool block */
void fsp_apply_init_file_page(buf_block_t *block)
{
  memset_aligned<UNIV_PAGE_SIZE_MIN>(block->page.frame, 0, srv_page_size);
  const page_id_t id(block->page.id());

  mach_write_to_4(block->page.frame + FIL_PAGE_OFFSET, id.page_no());
  memset_aligned<8>(block->page.frame + FIL_PAGE_PREV, 0xff, 8);
  mach_write_to_4(block->page.frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
                  id.space());
  if (page_zip_des_t* page_zip= buf_block_get_page_zip(block))
  {
    memset_aligned<UNIV_ZIP_SIZE_MIN>(page_zip->data, 0,
                                      page_zip_get_size(page_zip));
    static_assert(FIL_PAGE_OFFSET == 4, "compatibility");
    memcpy_aligned<4>(page_zip->data + FIL_PAGE_OFFSET,
                      block->page.frame + FIL_PAGE_OFFSET, 4);
    memset_aligned<8>(page_zip->data + FIL_PAGE_PREV, 0xff, 8);
    static_assert(FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID % 4 == 2,
                  "not perfect alignment");
    memcpy_aligned<2>(page_zip->data + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
                      block->page.frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, 4);
  }
}

#ifdef UNIV_DEBUG
/** Assert that the mini-transaction is compatible with
updating an allocation bitmap page.
@param[in]	mtr	mini-transaction */
void fil_space_t::modify_check(const mtr_t& mtr) const
{
  switch (mtr.get_log_mode()) {
  case MTR_LOG_NONE:
    /* These modes are only allowed within a non-bitmap page
       when there is a higher-level redo log record written. */
    ut_ad(!is_being_imported());
    break;
  case MTR_LOG_NO_REDO:
    ut_ad(is_temporary() || is_being_imported());
    break;
  default:
    /* We may only write redo log for a persistent tablespace. */
    ut_ad(!is_temporary());
    ut_ad(!is_being_imported());
    ut_ad(mtr.is_named_space(id));
  }
}
#endif

/** Initialize a tablespace header.
@param[in,out]	space	tablespace
@param[in]	size	current size in blocks
@param[in,out]	mtr	mini-transaction
@return error code */
dberr_t fsp_header_init(fil_space_t *space, uint32_t size, mtr_t *mtr)
{
	const page_id_t page_id(space->id, 0);
	const ulint zip_size = space->zip_size();

	buf_block_t *free_block = buf_LRU_get_free_block(have_no_mutex);

	mtr->x_lock_space(space);

	buf_block_t* block = buf_page_create(space, 0, zip_size, mtr,
					     free_block);
	if (UNIV_UNLIKELY(block != free_block)) {
		buf_pool.free_block(free_block);
	}

	space->size_in_header = size;
	space->free_len = 0;
	space->free_limit = 0;

	/* The prior contents of the file page should be ignored */

	fsp_init_file_page(space, block, mtr);

	mtr->write<2>(*block, block->page.frame + FIL_PAGE_TYPE,
		      FIL_PAGE_TYPE_FSP_HDR);

	mtr->write<4,mtr_t::MAYBE_NOP>(*block, FSP_HEADER_OFFSET + FSP_SPACE_ID
				       + block->page.frame, space->id);
	ut_ad(0 == mach_read_from_4(FSP_HEADER_OFFSET + FSP_NOT_USED
				    + block->page.frame));
	/* recv_sys_t::parse() expects to find a WRITE record that
	covers all 4 bytes. Therefore, we must specify mtr_t::FORCED
	in order to avoid optimizing away any unchanged most
	significant bytes of FSP_SIZE. */
	mtr->write<4,mtr_t::FORCED>(*block, FSP_HEADER_OFFSET + FSP_SIZE
				    + block->page.frame, size);
	ut_ad(0 == mach_read_from_4(FSP_HEADER_OFFSET + FSP_FREE_LIMIT
				    + block->page.frame));
	if (auto f = space->flags & ~FSP_FLAGS_MEM_MASK) {
		mtr->write<4,mtr_t::FORCED>(*block,
					    FSP_HEADER_OFFSET + FSP_SPACE_FLAGS
					    + block->page.frame, f);
	}
	ut_ad(0 == mach_read_from_4(FSP_HEADER_OFFSET + FSP_FRAG_N_USED
				    + block->page.frame));

	flst_init(block, FSP_HEADER_OFFSET + FSP_FREE, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_FREE_FRAG, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_FULL_FRAG, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL, mtr);
	flst_init(block, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE, mtr);

	mtr->write<8>(*block, FSP_HEADER_OFFSET + FSP_SEG_ID
		      + block->page.frame,
		      1U);

	if (dberr_t err = fsp_fill_free_list(!is_system_tablespace(space->id),
					     space, block, mtr)) {
		return err;
	}

	/* Write encryption metadata to page 0 if tablespace is
	encrypted or encryption is disabled by table option. */
	if (space->crypt_data &&
	    (space->crypt_data->should_encrypt() ||
	     space->crypt_data->not_encrypted())) {
		space->crypt_data->write_page0(block, mtr);
	}

	return DB_SUCCESS;
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
	uint32_t	page_no,
	buf_block_t*	header,
	mtr_t*		mtr)
{
	bool	success;
	ulint	size;

	ut_ad(!is_system_tablespace(space->id));
	ut_d(space->modify_check(*mtr));

	size = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE
				+ header->page.frame);
	ut_ad(size == space->size_in_header);

	ut_a(page_no >= size);

	success = fil_space_extend(space, page_no + 1);
	/* The size may be less than we wanted if we ran out of disk space. */
	/* recv_sys_t::parse() expects to find a WRITE record that
	covers all 4 bytes. Therefore, we must specify mtr_t::FORCED
	in order to avoid optimizing away any unchanged most
	significant bytes of FSP_SIZE. */
	mtr->write<4,mtr_t::FORCED>(*header, FSP_HEADER_OFFSET + FSP_SIZE
				    + header->page.frame, space->size);
	space->size_in_header = space->size;

	return(success);
}

/** Calculate the number of physical pages in an extent for this file.
@param[in]	physical_size	page_size of the datafile
@return number of pages in an extent for this file */
inline uint32_t fsp_get_extent_size_in_pages(ulint physical_size)
{
  return uint32_t((FSP_EXTENT_SIZE << srv_page_size_shift) / physical_size);
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
static uint32_t fsp_get_pages_to_extend_ibd(unsigned physical_size,
					    uint32_t size)
{
	uint32_t extent_size = fsp_get_extent_size_in_pages(physical_size);
	/* The threshold is set at 32MiB except when the physical page
	size is small enough that it must be done sooner. */
	uint32_t threshold = std::min(32 * extent_size, physical_size);

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
fsp_try_extend_data_file(fil_space_t *space, buf_block_t *header, mtr_t *mtr)
{
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
			sql_print_error("InnoDB: The InnoDB system tablespace "
                                        "%s" " innodb_data_file_path.",
                                        OUT_OF_SPACE_MSG);
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
			sql_print_error("InnoDB: The InnoDB temporary"
                                        " tablespace %s"
                                        " innodb_temp_data_file_path.",
                                        OUT_OF_SPACE_MSG);
			srv_tmp_space.set_tablespace_full_status(true);
		}
		return(0);
	}

	uint32_t size = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE
					 + header->page.frame);
	ut_ad(size == space->size_in_header);
	uint32_t size_increase;

	const unsigned ps = space->physical_size();

	switch (space->id) {
	case TRX_SYS_SPACE:
		size_increase = srv_sys_space.get_increment();
		break;
	case SRV_TMP_SPACE_ID:
		size_increase = srv_tmp_space.get_increment();
		break;
	default:
		uint32_t extent_pages = fsp_get_extent_size_in_pages(ps);
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

	/* recv_sys_t::parse() expects to find a WRITE record that
	covers all 4 bytes. Therefore, we must specify mtr_t::FORCED
	in order to avoid optimizing away any unchanged most
	significant bytes of FSP_SIZE. */
	mtr->write<4,mtr_t::FORCED>(*header, FSP_HEADER_OFFSET + FSP_SIZE
				    + header->page.frame,
				    space->size_in_header);

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
  ib::info() << "Resetting invalid page " << block.page.id() << " type "
             << fil_page_get_type(block.page.frame) << " to " << type << ".";
  mtr->write<2>(block, block.page.frame + FIL_PAGE_TYPE, type);
}

/** Put new extents to the free list if there are free extents above the free
limit. If an extent happens to contain an extent descriptor page, the extent
is put to the FSP_FREE_FRAG list with the page marked as used.
@param[in]	init_space	true if this is a single-table tablespace
and we are only initializing the first extent and the first bitmap pages;
then we will not allocate more extents
@param[in,out]	space		tablespace
@param[in,out]	header		tablespace header
@param[in,out]	mtr		mini-transaction
@return error code */
static
dberr_t
fsp_fill_free_list(
	bool		init_space,
	fil_space_t*	space,
	buf_block_t*	header,
	mtr_t*		mtr)
{
  ut_d(space->modify_check(*mtr));

  /* Check if we can fill free list from above the free list limit */
  uint32_t size=
    mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE + header->page.frame);
  uint32_t limit=
    mach_read_from_4(FSP_HEADER_OFFSET + FSP_FREE_LIMIT + header->page.frame);

  ut_ad(size == space->size_in_header);
  ut_ad(limit == space->free_limit);

  const auto zip_size= space->zip_size();

  if (size < limit + FSP_EXTENT_SIZE * FSP_FREE_ADD)
  {
    bool skip_resize= init_space;
    switch (space->id) {
    case TRX_SYS_SPACE:
      skip_resize= !srv_sys_space.can_auto_extend_last_file();
      break;
    case SRV_TMP_SPACE_ID:
      skip_resize= !srv_tmp_space.can_auto_extend_last_file();
      break;
    }

    if (!skip_resize)
    {
      fsp_try_extend_data_file(space, header, mtr);
      size= space->size_in_header;
    }
  }

  uint32_t count= 0;
  for (uint32_t i= limit, extent_size= FSP_EXTENT_SIZE,
         physical_size= space->physical_size();
       (init_space && i < 1) ||
         (i + extent_size <= size && count < FSP_FREE_ADD);
       i += extent_size)
  {
    const bool init_xdes= !ut_2pow_remainder(i, physical_size);
    space->free_limit= i + extent_size;
    mtr->write<4>(*header, FSP_HEADER_OFFSET + FSP_FREE_LIMIT +
                  header->page.frame, i + extent_size);

    if (init_xdes)
    {
      /* We are going to initialize a new descriptor page
      and a new ibuf bitmap page: the prior contents of the
      pages should be ignored. */

      if (i)
      {
        buf_block_t *f= buf_LRU_get_free_block(have_no_mutex);
        buf_block_t *block= buf_page_create(space, i, zip_size, mtr, f);
        if (UNIV_UNLIKELY(block != f))
          buf_pool.free_block(f);
        fsp_init_file_page(space, block, mtr);
        mtr->write<2>(*block, FIL_PAGE_TYPE + block->page.frame,
                      FIL_PAGE_TYPE_XDES);
      }

      if (!space->is_temporary())
      {
        buf_block_t *f= buf_LRU_get_free_block(have_no_mutex);
        buf_block_t *block=
          buf_page_create(space, i + 1, zip_size, mtr, f);
        if (UNIV_UNLIKELY(block != f))
          buf_pool.free_block(f);
        /* The zero-initialization will reset the change buffer bitmap bits
        to safe values for possible import to an earlier version that
        supports change buffering:

        IBUF_BITMAP_FREE     = 0 (no space left for buffering inserts)
        IBUF_BITMAP_BUFFERED = 0 (no changes have been buffered)
        IBUF_BITMAP_IBUF     = 0 (not part of the change buffer) */
        fsp_init_file_page(space, block, mtr);
        mtr->write<2>(*block, FIL_PAGE_TYPE + block->page.frame,
                      FIL_PAGE_IBUF_BITMAP);
      }
    }

    buf_block_t *xdes= nullptr;
    xdes_t *descr;
    {
      dberr_t err= DB_SUCCESS;
      descr= xdes_get_descriptor_with_space_hdr(header, space, i, mtr,
                                                &err, &xdes, init_space);
      if (!descr)
        return err;
    }

    if (xdes != header && !space->full_crc32())
      fil_block_check_type(*xdes, FIL_PAGE_TYPE_XDES, mtr);
    xdes_init(*xdes, descr, mtr);
    const uint16_t xoffset=
      static_cast<uint16_t>(descr - xdes->page.frame + XDES_FLST_NODE);
    if (UNIV_UNLIKELY(init_xdes))
    {
      /* The first page in the extent is a descriptor page and the
      second was reserved for change buffer bitmap: mark them used */
      xdes_set_free<false>(*xdes, descr, 0, mtr);
      xdes_set_free<false>(*xdes, descr, 1, mtr);
      xdes_set_state(*xdes, descr, XDES_FREE_FRAG, mtr);
      if (dberr_t err= flst_add_last(header, FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                                     xdes, xoffset, space->free_limit, mtr))
        return err;
      byte *n_used= FSP_HEADER_OFFSET + FSP_FRAG_N_USED + header->page.frame;
      mtr->write<4>(*header, n_used, 2U + mach_read_from_4(n_used));
    }
    else
    {
      if (dberr_t err=
          flst_add_last(header, FSP_HEADER_OFFSET + FSP_FREE,
                        xdes, xoffset, space->free_limit, mtr))
        return err;
      count++;
    }
  }

  space->free_len+= count;
  return DB_SUCCESS;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Allocates a new free extent.
@param[in,out]	space		tablespace
@param[in]	hint		hint of which extent would be desirable: any
page offset in the extent goes; the hint must not be > FSP_FREE_LIMIT
@param[out]	xdes		extent descriptor page
@param[in,out]	mtr		mini-transaction
@return extent descriptor
@retval nullptr if cannot be allocated */
static xdes_t *fsp_alloc_free_extent(fil_space_t *space, uint32_t hint,
                                     buf_block_t **xdes, mtr_t *mtr,
                                     dberr_t *err)
{
	fil_addr_t	first;
	xdes_t*		descr;
	buf_block_t*	desc_block;

	buf_block_t* header = fsp_get_header(space, mtr, err);
	if (!header) {
corrupted:
		space->set_corrupted();
		return nullptr;
	}

	descr = xdes_get_descriptor_with_space_hdr(
		header, space, hint, mtr, err, &desc_block);
	if (!descr) {
		goto corrupted;
	}

	if (desc_block != header && !space->full_crc32()) {
		fil_block_check_type(*desc_block, FIL_PAGE_TYPE_XDES, mtr);
	}

	if (xdes_get_state(descr) == XDES_FREE) {
		/* Ok, we can take this extent */
	} else {
		/* Take the first extent in the free list */
		first = flst_get_first(FSP_HEADER_OFFSET + FSP_FREE
				       + header->page.frame);

		if (first.page >= space->free_limit) {
			if (first.page != FIL_NULL) {
				goto flst_corrupted;
			}

			*err = fsp_fill_free_list(false, space, header, mtr);
			if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
				goto corrupted;
			}

			first = flst_get_first(FSP_HEADER_OFFSET + FSP_FREE
					       + header->page.frame);
			if (first.page == FIL_NULL) {
				*err = DB_OUT_OF_FILE_SPACE;
				return nullptr;	/* No free extents left */
			}
			if (first.page >= space->free_limit) {
				goto flst_corrupted;
			}
		}

		if (first.boffset < FSP_HEADER_OFFSET + FSP_HEADER_SIZE
		    || first.boffset >= space->physical_size()
		    - (XDES_SIZE + FIL_PAGE_DATA_END)) {
		flst_corrupted:
			*err = DB_CORRUPTION;
			goto corrupted;
		}

		descr = xdes_lst_get_descriptor(*space, first, mtr,
						&desc_block, err);
		if (!descr) {
			return descr;
		}
	}

	*err = flst_remove(header, FSP_HEADER_OFFSET + FSP_FREE, desc_block,
			   static_cast<uint16_t>(descr - desc_block->page.frame
						 + XDES_FLST_NODE),
			   space->free_limit, mtr);
	if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
		return nullptr;
	}

	space->free_len--;
	*xdes = desc_block;

	return(descr);
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Allocate a single free page.
@param[in,out]	header	tablespace header
@param[in,out]	xdes	extent descriptor page
@param[in,out]	descr	extent descriptor
@param[in]	bit	slot to allocate in the extent
@param[in]	space	tablespace
@param[in,out]	mtr	mini-transaction
@return error code */
static dberr_t
fsp_alloc_from_free_frag(buf_block_t *header, buf_block_t *xdes, xdes_t *descr,
			 uint32_t bit, fil_space_t *space, mtr_t *mtr)
{
  if (UNIV_UNLIKELY(xdes_get_state(descr) != XDES_FREE_FRAG ||
                    !xdes_is_free(descr, bit)))
    return DB_CORRUPTION;
  xdes_set_free<false>(*xdes, descr, bit, mtr);

  /* Update the FRAG_N_USED field */
  byte *n_used_p= FSP_HEADER_OFFSET + FSP_FRAG_N_USED + header->page.frame;
  uint32_t n_used = mach_read_from_4(n_used_p) + 1;

  if (xdes_is_full(descr))
  {
    const uint32_t limit= space->free_limit;
    /* The fragment is full: move it to another list */
    const uint16_t xoffset=
      static_cast<uint16_t>(descr - xdes->page.frame + XDES_FLST_NODE);
    if (dberr_t err= flst_remove(header, FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                                 xdes, xoffset, limit, mtr))
      return err;
    if (dberr_t err= flst_add_last(header, FSP_HEADER_OFFSET + FSP_FULL_FRAG,
                                   xdes, xoffset, limit, mtr))
      return err;
    xdes_set_state(*xdes, descr, XDES_FULL_FRAG, mtr);
    n_used-= FSP_EXTENT_SIZE;
  }

  mtr->write<4>(*header, n_used_p, n_used);
  return DB_SUCCESS;
}

/** Gets a buffer block for an allocated page.
@param[in,out]	space		tablespace
@param[in]	offset		page number of the allocated page
@param[in,out]	mtr		mini-transaction
@return block, initialized */
static buf_block_t* fsp_page_create(fil_space_t *space, uint32_t offset,
                                    mtr_t *mtr) noexcept
{
  buf_block_t *free_block= buf_LRU_get_free_block(have_no_mutex),
    *block= buf_page_create(space, offset, space->zip_size(), mtr, free_block);
  if (UNIV_UNLIKELY(block != free_block))
    buf_pool.free_block(free_block);
  fsp_init_file_page(space, block, mtr);
  return block;
}

/** Allocates a single free page from a space.
The page is marked as used.
@param[in,out]	space		tablespace
@param[in]	hint		hint of which page would be desirable
@param[in,out]	mtr		mini-transaction
@param[in,out]	init_mtr	mini-transaction in which the page should be
initialized (may be the same as mtr)
@param[out]	err		error code
@return allocated block
@retval nullptr	if no page could be allocated */
static MY_ATTRIBUTE((warn_unused_result, nonnull))
buf_block_t *fsp_alloc_free_page(fil_space_t *space, uint32_t hint,
                                 mtr_t *mtr, mtr_t *init_mtr, dberr_t *err)
{
  ut_d(space->modify_check(*mtr));
  buf_block_t *block= fsp_get_header(space, mtr, err);
  if (!block)
    return block;

  buf_block_t *xdes;
  /* Get the hinted descriptor */
  xdes_t *descr= xdes_get_descriptor_with_space_hdr(block, space, hint, mtr,
                                                    err, &xdes);
  if (descr && xdes_get_state(descr) == XDES_FREE_FRAG)
    /* Ok, we can take this extent */;
  else if (*err != DB_SUCCESS)
  {
  err_exit:
    space->set_corrupted();
    return nullptr;
  }
  else
  {
    /* Else take the first extent in free_frag list */
    fil_addr_t first = flst_get_first(FSP_HEADER_OFFSET + FSP_FREE_FRAG +
                                      block->page.frame);
    if (first.page >= space->free_limit)
    {
      if (first.page != FIL_NULL)
        goto flst_corrupted;

      /* There are no partially full fragments: allocate a free extent
      and add it to the FREE_FRAG list. NOTE that the allocation may
      have as a side-effect that an extent containing a descriptor
      page is added to the FREE_FRAG list. But we will allocate our
      page from the the free extent anyway. */
      descr= fsp_alloc_free_extent(space, hint, &xdes, mtr, err);
      if (!descr)
        return nullptr;
      *err= flst_add_last(block, FSP_HEADER_OFFSET + FSP_FREE_FRAG, xdes,
                          static_cast<uint16_t>(descr - xdes->page.frame +
                                                XDES_FLST_NODE),
                          space->free_limit, mtr);
      if (UNIV_UNLIKELY(*err != DB_SUCCESS))
        return nullptr;
      xdes_set_state(*xdes, descr, XDES_FREE_FRAG, mtr);
    }
    else
    {
      if (first.boffset < FSP_HEADER_OFFSET + FSP_HEADER_SIZE ||
          first.boffset >= space->physical_size() -
          (XDES_SIZE + FIL_PAGE_DATA_END))
      {
      flst_corrupted:
        *err= DB_CORRUPTION;
        goto err_exit;
      }

      descr= xdes_lst_get_descriptor(*space, first, mtr, &xdes, err);
      if (!descr)
        return nullptr;
      /* Reset the hint */
      hint= 0;
    }
  }

  /* Now we have in descr an extent with at least one free page. Look
  for a free page in the extent. */
  uint32_t free= xdes_find_free(descr, hint % FSP_EXTENT_SIZE);
  if (free == FIL_NULL)
  {
  corrupted:
    *err= DB_CORRUPTION;
    goto err_exit;
  }

  uint32_t page_no= xdes_get_offset(descr) + free;
  uint32_t space_size = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE +
                                         block->page.frame);
  ut_ad(space_size == space->size_in_header ||
        (space->id == TRX_SYS_SPACE &&
         srv_startup_is_before_trx_rollback_phase));

  if (space_size <= page_no)
  {
    /* It must be that we are extending a single-table tablespace
    whose size is still < 64 pages */
    ut_ad(!is_system_tablespace(space->id));
    if (page_no >= FSP_EXTENT_SIZE)
    {
      sql_print_error("InnoDB: Trying to extend %s"
                      " by single page(s) though the size is " UINT32PF "."
                      " Page no " UINT32PF ".",
                      space->chain.start->name, space_size, page_no);
      goto corrupted;
    }

    if (!fsp_try_extend_data_file_with_pages(space, page_no, block, mtr))
    {
      *err= DB_OUT_OF_FILE_SPACE;
      return nullptr;
    }
  }

  *err= fsp_alloc_from_free_frag(block, xdes, descr, free, space, mtr);
  if (UNIV_UNLIKELY(*err != DB_SUCCESS))
    goto corrupted;
  return fsp_page_create(space, page_no, init_mtr);
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Return an extent to the free list of a space.
@param[in,out]  space   tablespace
@param[in]      offset  page number in the extent
@param[in,out]  mtr     mini-transaction
@return error code */
static dberr_t fsp_free_extent(fil_space_t* space, uint32_t offset,
                               mtr_t* mtr)
{
  ut_ad(space->is_owner());
  dberr_t err;
  buf_block_t *block= fsp_get_header(space, mtr, &err);
  if (!block)
    return err;
  buf_block_t *xdes;
  xdes_t *descr= xdes_get_descriptor_with_space_hdr(block, space, offset, mtr,
                                                    &err, &xdes);
  if (!descr)
  {
    ut_ad(err || space->is_stopping());
    return err;
  }

  if (UNIV_UNLIKELY(xdes_get_state(descr) == XDES_FREE))
  {
    space->set_corrupted();
    return DB_CORRUPTION;
  }

  xdes_init(*xdes, descr, mtr);
  space->free_len++;
  return flst_add_last(block, FSP_HEADER_OFFSET + FSP_FREE,
                       xdes, static_cast<uint16_t>(descr - xdes->page.frame +
                                                   XDES_FLST_NODE),
                       space->free_limit, mtr);
}

MY_ATTRIBUTE((nonnull))
/** Frees a single page of a space.
The page is marked as free and clean.
@param[in,out]	space		tablespace
@param[in]	offset		page number
@param[in,out]	mtr		mini-transaction
@return error code */
static dberr_t fsp_free_page(fil_space_t *space, uint32_t offset, mtr_t *mtr)
{
	xdes_t*		descr;
	ulint		frag_n_used;

	ut_ad(mtr);
	ut_d(space->modify_check(*mtr));

	/* fprintf(stderr, "Freeing page %lu in space %lu\n", page, space); */

	dberr_t err;
	buf_block_t* header = fsp_get_header(space, mtr, &err);
	if (!header) {
		ut_ad(space->is_stopping());
		return err;
	}
	buf_block_t* xdes;

	descr = xdes_get_descriptor_with_space_hdr(header, space, offset, mtr,
						   &err, &xdes);
	if (!descr) {
		ut_ad(err || space->is_stopping());
		return err;
	}

	const auto state = xdes_get_state(descr);

	switch (state) {
	case XDES_FREE_FRAG:
	case XDES_FULL_FRAG:
		if (!xdes_is_free(descr, offset % FSP_EXTENT_SIZE)) {
			break;
		}
		/* fall through */
	default:
		space->set_corrupted();
		return DB_CORRUPTION;
	}

	frag_n_used = mach_read_from_4(FSP_HEADER_OFFSET + FSP_FRAG_N_USED
				       + header->page.frame);

	const uint16_t xoffset= static_cast<uint16_t>(descr - xdes->page.frame
						      + XDES_FLST_NODE);
	const uint32_t limit = space->free_limit;

	if (state == XDES_FULL_FRAG) {
		/* The fragment was full: move it to another list */
		err = flst_remove(header, FSP_HEADER_OFFSET + FSP_FULL_FRAG,
				  xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
		err = flst_add_last(header, FSP_HEADER_OFFSET + FSP_FREE_FRAG,
				    xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
		xdes_set_state(*xdes, descr, XDES_FREE_FRAG, mtr);
		mtr->write<4>(*header, FSP_HEADER_OFFSET + FSP_FRAG_N_USED
			      + header->page.frame,
			      frag_n_used + FSP_EXTENT_SIZE - 1);
	} else if (UNIV_UNLIKELY(!frag_n_used)) {
		return DB_CORRUPTION;
	} else {
		mtr->write<4>(*header, FSP_HEADER_OFFSET + FSP_FRAG_N_USED
			      + header->page.frame, frag_n_used - 1);
	}

	mtr->free(*space, static_cast<uint32_t>(offset));
	xdes_set_free<true>(*xdes, descr, offset % FSP_EXTENT_SIZE, mtr);
	ut_ad(err == DB_SUCCESS);

	if (!xdes_get_n_used(descr)) {
		/* The extent has become free: move it to another list */
		err = flst_remove(header, FSP_HEADER_OFFSET + FSP_FREE_FRAG,
				  xdes, xoffset, limit, mtr);
		if (err == DB_SUCCESS) {
			err = fsp_free_extent(space, offset, mtr);
		}
	}

	return err;
}

/** @return Number of segment inodes which fit on a single page */
inline ulint FSP_SEG_INODES_PER_PAGE(ulint physical_size)
{
	return (physical_size - FSEG_ARR_OFFSET - 10) / FSEG_INODE_SIZE;
}

/** Returns the nth inode slot on an inode page.
@param[in]	page		segment inode page
@param[in]	i		inode index on page
@return segment inode */
#define fsp_seg_inode_page_get_nth_inode(page, i)	\
	FSEG_ARR_OFFSET + FSEG_INODE_SIZE * i + page

/** Looks for a used segment inode on a segment inode page.
@param page             segment inode page
@param physical_size    page size
@return segment inode index
@retval ULINT_UNDEFINED if not found */
static
ulint
fsp_seg_inode_page_find_used(const page_t *page, ulint physical_size)
{
  for (ulint i= 0; i < FSP_SEG_INODES_PER_PAGE(physical_size); i++)
  {
    const byte *inode= fsp_seg_inode_page_get_nth_inode(page, i);
    if (mach_read_from_8(FSEG_ID + inode))
    {
      ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));
      return i;
    }
  }

  return ULINT_UNDEFINED;
}

/** Looks for an unused segment inode on a segment inode page.
@param[in]	page		segment inode page
@param[in]	i		search forward starting from this index
@param[in]	physical_size	page size
@return segment inode index
@retval ULINT_UNDEFINED if not found */
static
ulint
fsp_seg_inode_page_find_free(const page_t *page, ulint i, ulint physical_size)
{
  for (; i < FSP_SEG_INODES_PER_PAGE(physical_size); i++)
  {
    const byte *inode= fsp_seg_inode_page_get_nth_inode(page, i);
    if (mach_read_from_8(FSEG_ID + inode))
      ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));
    else
      /* This is unused */
      return i;
  }
  return ULINT_UNDEFINED;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Allocate a file segment inode page.
@param[in,out]  space   tablespace
@param[in,out]  header  tablespace header
@param[in,out]  mtr     mini-transaction
@return error code */
static dberr_t fsp_alloc_seg_inode_page(fil_space_t *space,
                                        buf_block_t *header, mtr_t *mtr)
{
  ut_ad(header->page.id().space() == space->id);
  dberr_t err;
  buf_block_t *block= fsp_alloc_free_page(space, 0, mtr, mtr, &err);

  if (!block)
    return err;

  ut_ad(block->page.lock.not_recursive());

  mtr->write<2>(*block, block->page.frame + FIL_PAGE_TYPE, FIL_PAGE_INODE);

#ifdef UNIV_DEBUG
  const byte *inode= FSEG_ID + FSEG_ARR_OFFSET + block->page.frame;
  for (ulint i= FSP_SEG_INODES_PER_PAGE(space->physical_size()); i--;
       inode += FSEG_INODE_SIZE)
    ut_ad(!mach_read_from_8(inode));
#endif

  return flst_add_last(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                       block, FSEG_INODE_PAGE_NODE, space->free_limit, mtr);
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Allocate a file segment inode.
@param[in,out]  space   tablespace
@param[in,out]  header  tablespace header
@param[out]     iblock  segment inode page
@param[in,out]  mtr     mini-transaction
@param[out]     err     error code
@return segment inode
@retval nullptr on failure */
static fseg_inode_t*
fsp_alloc_seg_inode(fil_space_t *space, buf_block_t *header,
                    buf_block_t **iblock, mtr_t *mtr, dberr_t *err)
{
  /* Allocate a new segment inode page if needed. */
  if (!flst_get_len(FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE +
                    header->page.frame))
  {
    *err= fsp_alloc_seg_inode_page(space, header, mtr);
    if (*err != DB_SUCCESS)
      return nullptr;
  }

  const page_id_t page_id
  {
    space->id,
    mach_read_from_4(FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE + FLST_FIRST +
                     FIL_ADDR_PAGE + header->page.frame)
  };

  buf_block_t *block=
    buf_page_get_gen(page_id, space->zip_size(), RW_SX_LATCH,
                     nullptr, BUF_GET_POSSIBLY_FREED, mtr, err);
  if (!block)
    return nullptr;

  if (!space->full_crc32())
    fil_block_check_type(*block, FIL_PAGE_INODE, mtr);

  const ulint physical_size= space->physical_size();
  ulint n= fsp_seg_inode_page_find_free(block->page.frame, 0, physical_size);

  if (UNIV_UNLIKELY(n >= FSP_SEG_INODES_PER_PAGE(physical_size)))
  {
    *err= DB_CORRUPTION;
    return nullptr;
  }
  fseg_inode_t *inode= fsp_seg_inode_page_get_nth_inode(block->page.frame, n);

  if (ULINT_UNDEFINED == fsp_seg_inode_page_find_free(block->page.frame, n + 1,
                                                      physical_size))
  {
    /* There are no other unused headers left on the page: move it
    to another list */
    const uint32_t limit= space->free_limit;
    *err= flst_remove(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                      block, FSEG_INODE_PAGE_NODE, limit, mtr);
    if (UNIV_UNLIKELY(*err != DB_SUCCESS))
      return nullptr;
    *err= flst_add_last(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL,
                        block, FSEG_INODE_PAGE_NODE, limit, mtr);
    if (UNIV_UNLIKELY(*err != DB_SUCCESS))
      return nullptr;
  }

  ut_ad(!mach_read_from_8(inode + FSEG_ID) ||
        !memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));
  *iblock= block;
  return inode;
}

MY_ATTRIBUTE((nonnull))
/** Frees a file segment inode.
@param[in,out]	space		tablespace
@param[in,out]	inode		segment inode
@param[in,out]	iblock		segment inode page
@param[in,out]	mtr		mini-transaction */
static dberr_t fsp_free_seg_inode(fil_space_t *space, fseg_inode_t *inode,
                                  buf_block_t *iblock, mtr_t *mtr)
{
  ut_d(space->modify_check(*mtr));

  dberr_t err;
  buf_block_t *header= fsp_get_header(space, mtr, &err);
  if (!header)
    return err;
  if (UNIV_UNLIKELY(memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4)))
  {
    space->set_corrupted();
    return DB_CORRUPTION;
  }

  const ulint physical_size= space->physical_size();
  const uint32_t limit= space->free_limit;

  if (ULINT_UNDEFINED == fsp_seg_inode_page_find_free(iblock->page.frame, 0,
                                                      physical_size))
  {
    /* Move the page to another list */
    err= flst_remove(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL,
                     iblock, FSEG_INODE_PAGE_NODE, limit, mtr);
    if (err == DB_SUCCESS)
      err= flst_add_last(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                         iblock, FSEG_INODE_PAGE_NODE, limit, mtr);
    if (err)
      return err;
  }

  mtr->memset(iblock, inode - iblock->page.frame + FSEG_ID,
              FSEG_INODE_SIZE, 0);

  if (ULINT_UNDEFINED != fsp_seg_inode_page_find_used(iblock->page.frame,
                                                      physical_size))
    return DB_SUCCESS;

  /* There are no other used headers left on the page: free it */
  err= flst_remove(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                   iblock, FSEG_INODE_PAGE_NODE, limit, mtr);
  if (err != DB_SUCCESS)
    return err;
  return fsp_free_page(space, iblock->page.id().page_no(), mtr);
}

MY_ATTRIBUTE((nonnull(1,4,5), warn_unused_result))
/** Returns the file segment inode, page x-latched.
@param[in]	header		segment header
@param[in]	space		space id
@param[in]	zip_size	ROW_FORMAT=COMPRESSED page size, or 0
@param[in,out]	mtr		mini-transaction
@param[out]	block		inode block
@param[out]	err		error code
@return segment inode, page x-latched
@retrval nullptr if the inode is free or corruption was noticed */
static
fseg_inode_t*
fseg_inode_try_get(
	const fseg_header_t*	header,
	uint32_t		space,
	ulint			zip_size,
	mtr_t*			mtr,
	buf_block_t**		block,
        dberr_t*		err = nullptr)
{
  if (UNIV_UNLIKELY(space != mach_read_from_4(header + FSEG_HDR_SPACE)))
  {
  corrupted:
    if (err)
      *err= DB_CORRUPTION;
    return nullptr;
  }

  *block=
    buf_page_get_gen(page_id_t(space,
                               mach_read_from_4(header + FSEG_HDR_PAGE_NO)),
                     zip_size, RW_SX_LATCH, nullptr, BUF_GET_POSSIBLY_FREED,
                     mtr, err);
  if (!*block)
    return nullptr;

  const uint16_t offset= mach_read_from_2(header + FSEG_HDR_OFFSET);
  if (UNIV_UNLIKELY(offset >= (*block)->physical_size()))
    goto corrupted;

  fseg_inode_t *inode= (*block)->page.frame + offset;
  if (UNIV_UNLIKELY(!mach_read_from_8(inode + FSEG_ID) ||
                    memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4)))
    goto corrupted;

  return inode;
}

/** Get the page number from the nth fragment page slot.
@param inode  file segment findex
@param n      slot index
@return page number
@retval FIL_NULL if not in use */
static uint32_t fseg_get_nth_frag_page_no(const fseg_inode_t *inode, ulint n)
{
	ut_ad(inode);
	ut_ad(n < FSEG_FRAG_ARR_N_SLOTS);
	ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));
	return(mach_read_from_4(inode + FSEG_FRAG_ARR
				+ n * FSEG_FRAG_SLOT_SIZE));
}

/** Set the page number in the nth fragment page slot.
@param[in,out]  inode   segment inode
@param[in,out]  iblock  segment inode page
@param[in]      n       slot index
@param[in]      page_no page number to set
@param[in,out]  mtr     mini-transaction */
inline void fseg_set_nth_frag_page_no(fseg_inode_t *inode, buf_block_t *iblock,
                                      ulint n, ulint page_no, mtr_t *mtr)
{
  ut_ad(n < FSEG_FRAG_ARR_N_SLOTS);
  ut_ad(mtr->memo_contains_flagged(iblock, MTR_MEMO_PAGE_SX_FIX));
  ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));

  mtr->write<4>(*iblock, inode + FSEG_FRAG_ARR + n * FSEG_FRAG_SLOT_SIZE,
                page_no);
}

/**********************************************************************//**
Finds a fragment page slot which is free.
@return slot index; ULINT_UNDEFINED if none found */
static
ulint
fseg_find_free_frag_page_slot(
/*==========================*/
	fseg_inode_t*	inode)	/*!< in: segment inode */
{
	ulint	i;
	ulint	page_no;

	for (i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
		page_no = fseg_get_nth_frag_page_no(inode, i);

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
	fseg_inode_t*	inode)	/*!< in: segment inode */
{
	ulint	i;
	ulint	page_no;

	for (i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
		page_no = fseg_get_nth_frag_page_no(
			inode, FSEG_FRAG_ARR_N_SLOTS - i - 1);

		if (page_no != FIL_NULL) {

			return(FSEG_FRAG_ARR_N_SLOTS - i - 1);
		}
	}

	return(ULINT_UNDEFINED);
}

/** Calculate reserved fragment page slots.
@param inode  file segment index
@return number of fragment pages */
static uint32_t fseg_get_n_frag_pages(const fseg_inode_t *inode) noexcept
{
	uint32_t count = 0;

	for (ulint i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
		if (FIL_NULL != fseg_get_nth_frag_page_no(inode, i)) {
			count++;
		}
	}

	return(count);
}

/** Create a new segment.
@param space                tablespace
@param byte_offset          byte offset of the created segment header
@param mtr                  mini-transaction
@param err                  error code
@param has_done_reservation whether fsp_reserve_free_extents() was invoked
@param block                block where segment header is placed,
                            or NULL to allocate an additional page for that
@return the block where the segment header is placed, x-latched
@retval nullptr if could not create segment */
buf_block_t*
fseg_create(fil_space_t *space, ulint byte_offset, mtr_t *mtr, dberr_t *err,
            bool has_done_reservation, buf_block_t *block)
{
	fseg_inode_t*	inode;
	ib_id_t		seg_id;
	uint32_t	n_reserved;
	bool		reserved_extent = false;

	DBUG_ENTER("fseg_create");

	ut_ad(mtr);
	ut_ad(byte_offset >= FIL_PAGE_DATA);
	ut_ad(byte_offset + FSEG_HEADER_SIZE
	      <= srv_page_size - FIL_PAGE_DATA_END);
	buf_block_t* iblock= 0;

	mtr->x_lock_space(space);
	ut_d(space->modify_check(*mtr));

	ut_ad(!block || block->page.id().space() == space->id);

	buf_block_t* header = fsp_get_header(space, mtr, err);
	if (!header) {
		block = nullptr;
		goto funct_exit;
	}

inode_alloc:
	inode = fsp_alloc_seg_inode(space, header, &iblock, mtr, err);

	if (!inode) {
		block = nullptr;
reserve_extent:
		if (!has_done_reservation && !reserved_extent) {
			*err = fsp_reserve_free_extents(&n_reserved, space, 2,
							FSP_NORMAL, mtr);
			if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
				DBUG_RETURN(nullptr);
			}

			/* Extents reserved successfully. So
			try allocating the page or inode */
			reserved_extent = true;
			if (inode) {
				goto page_alloc;
			}

			goto inode_alloc;
		}

		if (inode) {
			fsp_free_seg_inode(space, inode, iblock, mtr);
		}
		goto funct_exit;
	}

	/* Read the next segment id from space header and increment the
	value in space header */

	seg_id = mach_read_from_8(FSP_HEADER_OFFSET + FSP_SEG_ID
				  + header->page.frame);

	mtr->write<8>(*header,
		      FSP_HEADER_OFFSET + FSP_SEG_ID + header->page.frame,
		      seg_id + 1);
	mtr->write<8>(*iblock, inode + FSEG_ID, seg_id);
	ut_ad(!mach_read_from_4(inode + FSEG_NOT_FULL_N_USED));

	flst_init(*iblock, inode + FSEG_FREE, mtr);
	flst_init(*iblock, inode + FSEG_NOT_FULL, mtr);
	flst_init(*iblock, inode + FSEG_FULL, mtr);

	mtr->memcpy(*iblock, inode + FSEG_MAGIC_N, FSEG_MAGIC_N_BYTES, 4);
	compile_time_assert(FSEG_FRAG_SLOT_SIZE == 4);
	compile_time_assert(FIL_NULL == 0xffffffff);
	mtr->memset(iblock,
		    uint16_t(inode - iblock->page.frame) + FSEG_FRAG_ARR,
		    FSEG_FRAG_SLOT_SIZE * FSEG_FRAG_ARR_N_SLOTS, 0xff);

	if (!block) {
page_alloc:
		block = fseg_alloc_free_page_low(space,
						 inode, iblock, 0, FSP_UP,
#ifdef UNIV_DEBUG
						 has_done_reservation,
#endif /* UNIV_DEBUG */
						 mtr, mtr, err);

		if (!block) {
			ut_ad(!has_done_reservation);
			goto reserve_extent;
		}

		ut_d(const auto x = block->page.lock.x_lock_count());
		ut_ad(x || block->page.lock.not_recursive());
		ut_ad(x <= 2);
		ut_ad(!fil_page_get_type(block->page.frame));
		mtr->write<1>(*block, FIL_PAGE_TYPE + 1 + block->page.frame,
			      FIL_PAGE_TYPE_SYS);
	}

	mtr->write<2>(*block, byte_offset + FSEG_HDR_OFFSET
		      + block->page.frame,
		      uintptr_t(inode - iblock->page.frame));

	mtr->write<4>(*block, byte_offset + FSEG_HDR_PAGE_NO
		      + block->page.frame, iblock->page.id().page_no());

	mtr->write<4,mtr_t::MAYBE_NOP>(*block, byte_offset + FSEG_HDR_SPACE
				       + block->page.frame, space->id);

funct_exit:
	if (!has_done_reservation && reserved_extent) {
		space->release_free_extents(n_reserved);
	}

	DBUG_RETURN(block);
}

/**********************************************************************//**
Calculates the number of pages reserved by a segment, and how many pages are
currently used.
@return number of reserved pages */
static
uint32_t
fseg_n_reserved_pages_low(
/*======================*/
	const fseg_inode_t*	inode,	/*!< in: segment inode */
	uint32_t*		used)	/*!< out: number of pages used (not
				more than reserved) */
	noexcept
{
	const uint32_t extent_size = FSP_EXTENT_SIZE;

	*used = mach_read_from_4(inode + FSEG_NOT_FULL_N_USED)
		+ extent_size * flst_get_len(inode + FSEG_FULL)
		+ fseg_get_n_frag_pages(inode);

	return fseg_get_n_frag_pages(inode)
		+ extent_size * flst_get_len(inode + FSEG_FREE)
		+ extent_size * flst_get_len(inode + FSEG_NOT_FULL)
		+ extent_size * flst_get_len(inode + FSEG_FULL);
}

/** Calculate the number of pages reserved by a segment,
and how many pages are currently used.
@param[in]      block   buffer block containing the file segment header
@param[in]      header  file segment header
@param[out]     used    number of pages that are used (not more than reserved)
@param[in,out]  mtr     mini-transaction
@return number of reserved pages */
uint32_t fseg_n_reserved_pages(const buf_block_t &block,
                               const fseg_header_t *header, uint32_t *used,
                               mtr_t *mtr) noexcept
{
  ut_ad(page_align(header) == block.page.frame);
  buf_block_t *iblock;
  if (fseg_inode_t *inode=
      fseg_inode_try_get(header, block.page.id().space(), block.zip_size(),
                         mtr, &iblock))
    return fseg_n_reserved_pages_low(inode, used);
  return *used= 0;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Tries to fill the free list of a segment with consecutive free extents.
This happens if the segment is big enough to allow extents in the free list,
the free list is empty, and the extents can be allocated consecutively from
the hint onward.
@param[in]	inode	segment inode
@param[in,out]	iblock	segment inode page
@param[in]	space	tablespace
@param[in]	hint	hint which extent would be good as the first extent
@param[in,out]	mtr	mini-transaction */
static dberr_t fseg_fill_free_list(const fseg_inode_t *inode,
                                   buf_block_t *iblock, fil_space_t *space,
                                   uint32_t hint, mtr_t *mtr)
{
  uint32_t used;

  ut_ad(!((page_offset(inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
  ut_d(space->modify_check(*mtr));

  if (fseg_n_reserved_pages_low(inode, &used) <
      FSEG_FREE_LIST_LIMIT * FSP_EXTENT_SIZE)
    /* The segment is too small to allow extents in free list */
    return DB_SUCCESS;

  if (UNIV_UNLIKELY(memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4)))
  {
    space->set_corrupted();
    return DB_CORRUPTION;
  }

  if (flst_get_len(inode + FSEG_FREE) > 0)
    /* Free list is not empty */
    return DB_SUCCESS;

  for (ulint i= 0; i < FSEG_FREE_LIST_MAX_LEN; i++, hint += FSP_EXTENT_SIZE)
  {
    buf_block_t *xdes;
    dberr_t err;
    xdes_t *descr= xdes_get_descriptor(space, hint, mtr, &err, &xdes);
    if (!descr || XDES_FREE != xdes_get_state(descr))
      /* We cannot allocate the desired extent: stop */
      return err;

    descr= fsp_alloc_free_extent(space, hint, &xdes, mtr, &err);
    if (UNIV_UNLIKELY(!descr))
      return err;

    if (dberr_t err=
        flst_add_last(iblock,
                      static_cast<uint16_t>(inode - iblock->page.frame +
                                            FSEG_FREE), xdes,
                      static_cast<uint16_t>(descr - xdes->page.frame +
                                            XDES_FLST_NODE),
                      space->free_limit, mtr))
      return err;
    xdes_set_state(*xdes, descr, XDES_FSEG, mtr);
    mtr->memcpy(*xdes, descr + XDES_ID, inode + FSEG_ID, 8);
  }

  return DB_SUCCESS;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Allocates a free extent for the segment: looks first in the free list of
the segment, then tries to allocate from the space free list.
NOTE that the extent returned still resides in the segment free list, it is
not yet taken off it!
@param[in]	inode		segment inode
@param[in,out]	iblock		segment inode page
@param[out]	xdes		extent descriptor page
@param[in,out]	space		tablespace
@param[in,out]	mtr		mini-transaction
@param[out]	err		error code
@retval nullptr	if no page could be allocated */
static
xdes_t*
fseg_alloc_free_extent(
	const fseg_inode_t*	inode,
	buf_block_t*		iblock,
	buf_block_t**		xdes,
	fil_space_t*		space,
	mtr_t*			mtr,
	dberr_t*		err)
{
  ut_ad(iblock->page.frame == page_align(inode));
  ut_ad(!((inode - iblock->page.frame - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
  ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));
  ut_d(space->modify_check(*mtr));

  if (UNIV_UNLIKELY(uintptr_t(inode - iblock->page.frame) < FSEG_ARR_OFFSET))
  {
  corrupted:
    *err= DB_CORRUPTION;
    space->set_corrupted();
    return nullptr;
  }

  if (flst_get_len(inode + FSEG_FREE))
  {
    const fil_addr_t first= flst_get_first(inode + FSEG_FREE);
    if (first.page >= space->free_limit ||
        first.boffset < FSP_HEADER_OFFSET + FSP_HEADER_SIZE ||
        first.boffset >= space->physical_size() -
        (XDES_SIZE + FIL_PAGE_DATA_END))
      goto corrupted;

    /* Segment free list is not empty, allocate from it */
    return xdes_lst_get_descriptor(*space, first, mtr, xdes, err);
  }

  xdes_t* descr= fsp_alloc_free_extent(space, 0, xdes, mtr, err);
  if (UNIV_UNLIKELY(!descr))
    return descr;
  xdes_set_state(**xdes, descr, XDES_FSEG, mtr);
  mtr->memcpy<mtr_t::MAYBE_NOP>(**xdes, descr + XDES_ID, inode + FSEG_ID, 8);
  *err= flst_add_last(iblock,
                      static_cast<uint16_t>(inode - iblock->page.frame +
                                            FSEG_FREE), *xdes,
                      static_cast<uint16_t>(descr - (*xdes)->page.frame +
                                            XDES_FLST_NODE),
                      space->free_limit, mtr);
  if (UNIV_LIKELY(*err != DB_SUCCESS))
    return nullptr;
  /* Try to fill the segment free list */
  *err= fseg_fill_free_list(inode, iblock, space,
                            xdes_get_offset(descr) + FSP_EXTENT_SIZE, mtr);
  if (UNIV_UNLIKELY(*err != DB_SUCCESS))
    return nullptr;

  return descr;
}

/** Allocates a single free page from a segment.
This function implements the intelligent allocation strategy which tries to
minimize file space fragmentation.
@param[in,out]	space			tablespace
@param[in,out]	seg_inode		segment inode
@param[in,out]	iblock			segment inode page
@param[in]	hint			hint of which page would be desirable
@param[in]	direction		if the new page is needed because of
an index page split, and records are inserted there in order, into which
direction they go alphabetically: FSP_DOWN, FSP_UP, FSP_NO_DIR
@param[in,out]	mtr			mini-transaction
@param[in,out]	init_mtr		mtr or another mini-transaction in
which the page should be initialized.
@param[out]	err			error code
@return the allocated page
@retval nullptr	if no page could be allocated */
static
buf_block_t*
fseg_alloc_free_page_low(
	fil_space_t*		space,
	fseg_inode_t*		seg_inode,
	buf_block_t*		iblock,
	uint32_t		hint,
	byte			direction,
#ifdef UNIV_DEBUG
	bool			has_done_reservation,
	/*!< whether the space has already been reserved */
#endif /* UNIV_DEBUG */
	mtr_t*			mtr,
	mtr_t*			init_mtr,
	dberr_t*		err)
{
	ib_id_t		seg_id;
	uint32_t	used, reserved;
	xdes_t*		descr;		/*!< extent of the hinted page */
	uint32_t	ret_page;	/*!< the allocated page offset, FIL_NULL
					if could not be allocated */
	xdes_t*		ret_descr;	/*!< the extent of the allocated page */
	buf_block_t*	xdes;
	ulint		n;

	ut_ad((direction >= FSP_UP) && (direction <= FSP_NO_DIR));
	ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + seg_inode, 4));
	ut_ad(!((page_offset(seg_inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	seg_id = mach_read_from_8(seg_inode + FSEG_ID);

	ut_ad(seg_id);
	ut_d(space->modify_check(*mtr));
	ut_ad(fil_page_get_type(page_align(seg_inode)) == FIL_PAGE_INODE);

	reserved = fseg_n_reserved_pages_low(seg_inode, &used);

	buf_block_t* header = fsp_get_header(space, mtr, err);
	if (!header) {
		return header;
	}

	descr = xdes_get_descriptor_with_space_hdr(header, space, hint, mtr,
						   err, &xdes);
	if (!descr) {
		if (*err != DB_SUCCESS) {
			return nullptr;
		}
		/* Hint outside space or too high above free limit: reset
		hint */
		/* The file space header page is always allocated. */
		hint = 0;
		descr = xdes_get_descriptor(space, hint, mtr, err, &xdes);
		if (!descr) {
			return nullptr;
		}
	}

	const uint32_t extent_size = FSP_EXTENT_SIZE;
	ret_descr = descr;
	/* Try to get the page from extent which belongs to segment */
	if (xdes_get_state(descr) == XDES_FSEG
	    && mach_read_from_8(descr + XDES_ID) == seg_id) {
		/* Get the page from the segment extent */
		if (xdes_is_free(descr, hint % extent_size)) {
take_hinted_page:
			ret_page = hint;
			goto got_hinted_page;
		} else if (!xdes_is_full(descr)) {
			/* Take the page from the same extent as the
			hinted page (and the extent already belongs to
			the segment) */
			ret_page = xdes_find_free(descr, hint % extent_size);
			if (ret_page == FIL_NULL) {
				ut_ad(!has_done_reservation);
				return nullptr;
			}
			ret_page += xdes_get_offset(ret_descr);
			goto alloc_done;
		}
	}

	/** If the number of unused but reserved pages in a segment is
	esser than minimum value of 1/8 of reserved pages or
	4 * FSP_EXTENT_SIZE and there are at least half of extent size
	used pages, then we allow a new empty extent to be added to
	the segment in fseg_alloc_free_page_general(). Otherwise, we use
	unused pages of the segment. */
	if (used < extent_size / 2 ||
            reserved - used >= reserved / 8 ||
            reserved - used >= extent_size * 4) {
	} else if (xdes_get_state(descr) == XDES_FREE) {
		/* Allocate the free extent from space and can
		take the hinted page */
		ret_descr = fsp_alloc_free_extent(space, hint, &xdes,
						  mtr, err);

		if (UNIV_UNLIKELY(ret_descr != descr)) {
			if (*err != DB_SUCCESS) {
				*err = DB_CORRUPTION;
			}
			return nullptr;
		}

		xdes_set_state(*xdes, ret_descr, XDES_FSEG, mtr);
		mtr->write<8,mtr_t::MAYBE_NOP>(*xdes, ret_descr + XDES_ID,
					       seg_id);
		*err = flst_add_last(
			iblock,
			static_cast<uint16_t>(seg_inode - iblock->page.frame
					      + FSEG_FREE), xdes,
			static_cast<uint16_t>(ret_descr
					      - xdes->page.frame
					      + XDES_FLST_NODE),
			space->free_limit, mtr);
		if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
			return nullptr;
		}

		/* Try to fill the segment free list */
		*err = fseg_fill_free_list(seg_inode, iblock, space,
					   hint + extent_size, mtr);
		if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
			return nullptr;
		}
		goto take_hinted_page;
	} else if (direction != FSP_NO_DIR) {

		ret_descr = fseg_alloc_free_extent(seg_inode, iblock,
						   &xdes, space, mtr, err);

		if (!ret_descr) {
			ut_ad(*err != DB_SUCCESS);
			return nullptr;
		}
		/* Take any free extent (which was already assigned
		above in the if-condition to ret_descr) and take the
		lowest or highest page in it, depending on the direction */
		ret_page = xdes_get_offset(ret_descr);

		if (direction == FSP_DOWN) {
			ret_page += extent_size - 1;
		}
		goto alloc_done;
	}

	/* Try to take individual page from the segment	or tablespace */
	if (reserved - used > 0) {
		/* Take any unused page from the segment */
		fil_addr_t	first;

		if (flst_get_len(seg_inode + FSEG_NOT_FULL) > 0) {
			first = flst_get_first(seg_inode + FSEG_NOT_FULL);
		} else if (flst_get_len(seg_inode + FSEG_FREE) > 0) {
			first = flst_get_first(seg_inode + FSEG_FREE);
		} else {
			ut_ad(!has_done_reservation);
			return nullptr;
		}

		if (first.page >= space->free_limit
		    || first.boffset < FSP_HEADER_OFFSET + FSP_HEADER_SIZE
		    || first.boffset >= space->physical_size()
		    - (XDES_SIZE + FIL_PAGE_DATA_END)) {
			*err= DB_CORRUPTION;
			return nullptr;
		}

		ret_descr = xdes_lst_get_descriptor(*space, first, mtr, &xdes);
		if (!ret_descr) {
			return nullptr;
		}

		ret_page = xdes_find_free(ret_descr);
		if (ret_page == FIL_NULL) {
			ut_ad(!has_done_reservation);
		} else {
			ret_page += xdes_get_offset(ret_descr);
		}

	} else if (used < extent_size / 2) {
		/* Allocate an individual page from the space */
		buf_block_t* block = fsp_alloc_free_page(
			space, hint, mtr, init_mtr, err);

		ut_ad(block || !has_done_reservation || *err);

		if (block) {
			/* Put the page in the fragment page array of the
			segment */
			n = fseg_find_free_frag_page_slot(seg_inode);
			if (UNIV_UNLIKELY(n == ULINT_UNDEFINED)) {
				*err = DB_CORRUPTION;
				return nullptr;
			}

			fseg_set_nth_frag_page_no(
				seg_inode, iblock, n,
				block->page.id().page_no(), mtr);
		}

		/* fsp_alloc_free_page() invoked fsp_init_file_page()
		already. */
		return(block);
	} else {
		/* In worst case, try to allocate a new extent
		and take its first page */
		ret_descr = fseg_alloc_free_extent(seg_inode, iblock, &xdes,
						   space, mtr, err);
		if (!ret_descr) {
			ut_ad(!has_done_reservation || *err);
			return nullptr;
		} else {
			ret_page = xdes_get_offset(ret_descr);
		}
	}

	if (ret_page == FIL_NULL) {
		/* Page could not be allocated */

		ut_ad(!has_done_reservation);
		return nullptr;
	}
alloc_done:
	if (space->size <= ret_page && !is_predefined_tablespace(space->id)) {
		/* It must be that we are extending a single-table
		tablespace whose size is still < 64 pages */
		if (ret_page >= extent_size) {
			sql_print_error("InnoDB: Trying to extend '%s'"
					" by single page(s) though the"
					" space size " UINT32PF "."
					" Page no " UINT32PF ".",
					space->chain.start->name, space->size,
					ret_page);
			ut_ad(!has_done_reservation);
			return nullptr;
		}

		if (!fsp_try_extend_data_file_with_pages(
			    space, ret_page, header, mtr)) {
			/* No disk space left */
			ut_ad(!has_done_reservation);
			return nullptr;
		}
	}

	/* Skip the check for extending the tablespace.
	If the page hint were not within the size of the tablespace,
	descr set to nullptr above and reset the hint and the block
	was allocated from free_frag (XDES_FREE_FRAG) */
	if (ret_descr != NULL) {
got_hinted_page:
		/* At this point we know the extent and the page offset.
		The extent is still in the appropriate list (FSEG_NOT_FULL
		or FSEG_FREE), and the page is not yet marked as used. */
		ut_d(buf_block_t* xxdes);
		ut_ad(xdes_get_descriptor(space, ret_page, mtr, err, &xxdes)
		      == ret_descr);
		ut_ad(xdes == xxdes);
		ut_ad(xdes_is_free(ret_descr, ret_page % extent_size));

		*err = fseg_mark_page_used(space, seg_inode, iblock, ret_page,
					   ret_descr, xdes, mtr);
		if (UNIV_UNLIKELY(*err != DB_SUCCESS)) {
			return nullptr;
		}
	}

	return fsp_page_create(space, ret_page, init_mtr);
}

/**********************************************************************//**
Allocates a single free page from a segment. This function implements
the intelligent allocation strategy which tries to minimize file space
fragmentation.
@retval NULL if no page could be allocated */
buf_block_t*
fseg_alloc_free_page_general(
/*=========================*/
	fseg_header_t*	seg_header,/*!< in/out: segment header */
	uint32_t	hint,	/*!< in: hint of which page would be
				desirable */
	byte		direction,/*!< in: if the new page is needed because
				of an index page split, and records are
				inserted there in order, into which
				direction they go alphabetically: FSP_DOWN,
				FSP_UP, FSP_NO_DIR */
	bool		has_done_reservation, /*!< in: true if the caller has
				already done the reservation for the page
				with fsp_reserve_free_extents, then there
				is no need to do the check for this individual
				page */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	mtr_t*		init_mtr,/*!< in/out: mtr or another mini-transaction
				in which the page should be initialized. */
	dberr_t*	err)	/*!< out: error code */
{
	fseg_inode_t*	inode;
	fil_space_t*	space;
	buf_block_t*	iblock;
	buf_block_t*	block;
	uint32_t	n_reserved;

	const uint32_t space_id = page_get_space_id(page_align(seg_header));
	space = mtr->x_lock_space(space_id);
	inode = fseg_inode_try_get(seg_header, space_id, space->zip_size(),
				   mtr, &iblock, err);
	if (!inode) {
		return nullptr;
	}
	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}

	if (!has_done_reservation) {
		*err = fsp_reserve_free_extents(&n_reserved, space, 2,
						FSP_NORMAL, mtr);
		if (*err != DB_SUCCESS) {
			return nullptr;
		}
	}

	block = fseg_alloc_free_page_low(space,
					 inode, iblock, hint, direction,
#ifdef UNIV_DEBUG
					 has_done_reservation,
#endif /* UNIV_DEBUG */
					 mtr, init_mtr, err);

	/* The allocation cannot fail if we have already reserved a
	space for the page. */
	ut_ad(block || !has_done_reservation || *err);

	if (!has_done_reservation) {
		space->release_free_extents(n_reserved);
	}

	return(block);
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Check that we have at least n_pages frag pages free in the first extent
of a single-table tablespace, and they are also physically initialized to
the data file. That is we have already extended the data file so that those
pages are inside the data file. If not, this function extends the tablespace
with pages.
@param[in,out]	space	tablespace
@param[in,out]	header	tablespace header, x-latched
@param[in]	size	tablespace size in pages, less than FSP_EXTENT_SIZE
@param[in,out]	mtr	mini-transaction
@param[in]	n_pages	number of pages to reserve
@return error code */
static
dberr_t
fsp_reserve_free_pages(
	fil_space_t*	space,
	buf_block_t*	header,
	ulint		size,
	mtr_t*		mtr,
	uint32_t	n_pages)
{
  ut_ad(space != fil_system.sys_space && space != fil_system.temp_space);
  ut_ad(size < FSP_EXTENT_SIZE);

  dberr_t err= DB_OUT_OF_FILE_SPACE;
  const xdes_t *descr=
    xdes_get_descriptor_with_space_hdr(header, space, 0, mtr, &err);
  if (!descr)
    return err;
  const uint32_t n_used= xdes_get_n_used(descr);
  if (size >= n_used + n_pages)
    return DB_SUCCESS;
  if (n_used > size)
    return DB_CORRUPTION;
  return fsp_try_extend_data_file_with_pages(space, n_used + n_pages - 1,
                                             header, mtr)
    ? DB_SUCCESS
    : DB_OUT_OF_FILE_SPACE;
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
@return error code
@retval DB_SUCCESS if we were able to make the reservation */
dberr_t
fsp_reserve_free_extents(
	uint32_t*	n_reserved,
	fil_space_t*	space,
	uint32_t	n_ext,
	fsp_reserve_t	alloc_type,
	mtr_t*		mtr,
	uint32_t	n_pages)
{
	ulint		reserve;

	ut_ad(mtr);
	*n_reserved = n_ext;

	const uint32_t extent_size = FSP_EXTENT_SIZE;

	mtr->x_lock_space(space);
	const unsigned physical_size = space->physical_size();

	dberr_t err;
	buf_block_t* header = fsp_get_header(space, mtr, &err);
	if (!header) {
		return err;
	}
try_again:
	uint32_t size = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE
					 + header->page.frame);
	ut_ad(size == space->size_in_header);

	if (size < extent_size && n_pages < extent_size / 2) {
		/* Use different rules for small single-table tablespaces */
		*n_reserved = 0;
		return fsp_reserve_free_pages(space, header, size,
					      mtr, n_pages);
	}

	uint32_t n_free_list_ext = flst_get_len(FSP_HEADER_OFFSET + FSP_FREE
						+ header->page.frame);
	ut_ad(space->free_len == n_free_list_ext);

	uint32_t free_limit = mach_read_from_4(FSP_HEADER_OFFSET
					       + FSP_FREE_LIMIT
					       + header->page.frame);
	ut_ad(space->free_limit == free_limit);

	/* Below we play safe when counting free extents above the free limit:
	some of them will contain extent descriptor pages, and therefore
	will not be free extents */

	uint32_t n_free_up;

	if (size >= free_limit) {
		n_free_up = (size - free_limit) / extent_size;
		if (n_free_up) {
			n_free_up--;
			n_free_up -= n_free_up / (physical_size / extent_size);
		}
	} else {
		ut_ad(alloc_type == FSP_BLOB);
		n_free_up = 0;
	}

	uint32_t n_free = n_free_list_ext + n_free_up;

	switch (alloc_type) {
	case FSP_NORMAL:
		/* We reserve 1 extent + 0.5 % of the space size to undo logs
		and 1 extent + 0.5 % to cleaning operations; NOTE: this source
		code is duplicated in the function below! */

		reserve = 2 + ((size / extent_size) * 2) / 200;

		if (n_free <= reserve + n_ext) {

			goto try_to_extend;
		}
		break;
	case FSP_UNDO:
		/* We reserve 0.5 % of the space size to cleaning operations */

		reserve = 1 + ((size / extent_size) * 1) / 200;

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
		return DB_SUCCESS;
	}
try_to_extend:
	if (fsp_try_extend_data_file(space, header, mtr)) {
		goto try_again;
	}

	return DB_OUT_OF_FILE_SPACE;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Frees a single page of a segment.
@param[in,out]	space		tablespace
@param[in]	seg_inode	segment inode
@param[in,out]	iblock		block where segment inode are kept
@param[in,out]	mtr		mini-transaction
@param[in]	offset		page number
@param[in]	ahi		Drop adaptive hash index
@return error code */
static
dberr_t
fseg_free_page_low(
	fil_space_t*		space,
	fseg_inode_t*		seg_inode,
	buf_block_t*		iblock,
	mtr_t*			mtr,
	uint32_t		offset
#ifdef BTR_CUR_HASH_ADAPT
	,bool			ahi=false
#endif /* BTR_CUR_HASH_ADAPT */
	)
{
	ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + seg_inode, 4));
	ut_ad(!((page_offset(seg_inode) - FSEG_ARR_OFFSET) % FSEG_INODE_SIZE));
	ut_ad(iblock->page.frame == page_align(seg_inode));
	ut_d(space->modify_check(*mtr));

#ifdef BTR_CUR_HASH_ADAPT
	if (ahi) {
		btr_search_drop_page_hash_when_freed(
			page_id_t(space->id, offset));
	}
#endif /* BTR_CUR_HASH_ADAPT */

	const uint32_t extent_size = FSP_EXTENT_SIZE;
	ut_ad(ut_is_2pow(extent_size));
	buf_block_t* xdes;
	dberr_t err;
	xdes_t* descr = xdes_get_descriptor(space, offset, mtr, &err, &xdes);

	if (!descr) {
		return err;
	}
	if (UNIV_UNLIKELY(xdes_is_free(descr, offset & (extent_size - 1)))) {
corrupted:
		space->set_corrupted();
		return DB_CORRUPTION;
	}

	if (xdes_get_state(descr) != XDES_FSEG) {
		/* The page is in the fragment pages of the segment */
		for (ulint i = 0;; i++) {
			if (fseg_get_nth_frag_page_no(seg_inode, i)
			    != offset) {
				continue;
			}

			compile_time_assert(FIL_NULL == 0xffffffff);
			mtr->memset(iblock, uint16_t(seg_inode
						     - iblock->page.frame)
				    + FSEG_FRAG_ARR
				    + i * FSEG_FRAG_SLOT_SIZE, 4, 0xff);
			break;
		}

		return fsp_free_page(space, offset, mtr);
	}

	/* If we get here, the page is in some extent of the segment */

	if (UNIV_UNLIKELY(memcmp(descr + XDES_ID, seg_inode + FSEG_ID, 8))) {
		goto corrupted;
	}

	byte* p_not_full = seg_inode + FSEG_NOT_FULL_N_USED;
	uint32_t not_full_n_used = mach_read_from_4(p_not_full);
	const uint16_t xoffset= uint16_t(descr - xdes->page.frame
					 + XDES_FLST_NODE);
	const uint16_t ioffset= uint16_t(seg_inode - iblock->page.frame);
	const uint32_t limit = space->free_limit;

	if (xdes_is_full(descr)) {
		/* The fragment is full: move it to another list */
		err = flst_remove(iblock,
				  static_cast<uint16_t>(FSEG_FULL + ioffset),
				  xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
		err = flst_add_last(iblock, static_cast<uint16_t>(FSEG_NOT_FULL
								  + ioffset),
				    xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
		not_full_n_used += extent_size - 1;
	} else {
		if (!not_full_n_used) {
			goto corrupted;
		}
		not_full_n_used--;
	}

	mtr->write<4>(*iblock, p_not_full, not_full_n_used);
	xdes_set_free<true>(*xdes, descr, offset & (extent_size - 1), mtr);

	if (!xdes_get_n_used(descr)) {
		err = flst_remove(iblock, static_cast<uint16_t>(FSEG_NOT_FULL
								+ ioffset),
				  xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
		err = fsp_free_extent(space, offset, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
	}

	mtr->free(*space, static_cast<uint32_t>(offset));
	return DB_SUCCESS;
}

/** Free a page in a file segment.
@param[in,out]	seg_header	file segment header
@param[in,out]	space		tablespace
@param[in]	offset		page number
@param[in,out]	mtr		mini-transaction
@param[in]	have_latch	whether space->x_lock() was already called
@return error code */
dberr_t fseg_free_page(fseg_header_t *seg_header, fil_space_t *space,
                       uint32_t offset, mtr_t *mtr, bool have_latch)
{
  buf_block_t *iblock;
  if (have_latch)
    ut_ad(space->is_owner());
  else
    mtr->x_lock_space(space);

  DBUG_PRINT("fseg_free_page",
             ("space_id: %" PRIu32 ", page_no: %" PRIu32, space->id, offset));

  dberr_t err;
  if (fseg_inode_t *seg_inode= fseg_inode_try_get(seg_header,
                                                  space->id, space->zip_size(),
                                                  mtr, &iblock, &err))
  {
    if (!space->full_crc32())
      fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
    return fseg_free_page_low(space, seg_inode, iblock, mtr, offset);
  }

  return err;
}

/** Determine whether a page is allocated.
@param space   tablespace
@param page    page number
@return error code
@retval DB_SUCCESS             if the page is marked as free
@retval DB_SUCCESS_LOCKED_REC  if the page is marked as allocated */
dberr_t fseg_page_is_allocated(fil_space_t *space, unsigned page)
{
  mtr_t mtr;
  uint32_t dpage= xdes_calc_descriptor_page(space->zip_size(), page);
  const unsigned zip_size= space->zip_size();
  dberr_t err= DB_SUCCESS;

  mtr.start();
  if (!space->is_owner())
    mtr.x_lock_space(space);

  if (page >= space->free_limit || page >= space->size_in_header);
  else if (const buf_block_t *b=
           buf_page_get_gen(page_id_t(space->id, dpage), space->zip_size(),
                            RW_S_LATCH, nullptr, BUF_GET_POSSIBLY_FREED,
                            &mtr, &err))
  {
    if (!dpage &&
        (space->free_limit !=
         mach_read_from_4(FSP_FREE_LIMIT + FSP_HEADER_OFFSET +
                          b->page.frame) ||
         space->size_in_header !=
         mach_read_from_4(FSP_SIZE + FSP_HEADER_OFFSET + b->page.frame)))
      err= DB_CORRUPTION;
    else
      err= xdes_is_free(b->page.frame + XDES_ARR_OFFSET + XDES_SIZE
                        * xdes_calc_descriptor_index(zip_size, page),
                        page & (FSP_EXTENT_SIZE - 1))
        ? DB_SUCCESS
        : DB_SUCCESS_LOCKED_REC;
  }

  mtr.commit();
  return err;
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Free an extent of a segment to the space free list.
@param[in,out]	seg_inode	segment inode
@param[in,out]	space		tablespace
@param[in]	page		page number in the extent
@param[in,out]	mtr		mini-transaction
@return error code */
static
dberr_t
fseg_free_extent(
	fseg_inode_t*		seg_inode,
	buf_block_t*		iblock,
	fil_space_t*		space,
	uint32_t		page,
	mtr_t*			mtr
#ifdef BTR_CUR_HASH_ADAPT
	,bool			ahi=false
#endif /* BTR_CUR_HASH_ADAPT */
	)
{
	buf_block_t* xdes;
	dberr_t err;
	xdes_t*	descr = xdes_get_descriptor(space, page, mtr, &err, &xdes);

	if (!descr) {
		return err;
	}

	if (UNIV_UNLIKELY(xdes_get_state(descr) != XDES_FSEG
			  || memcmp(descr + XDES_ID, seg_inode + FSEG_ID, 8)
			  || memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N
				    + seg_inode, 4))) {
		return DB_CORRUPTION;
	}
	ut_d(space->modify_check(*mtr));
	const uint32_t first_page_in_extent = page - (page % FSP_EXTENT_SIZE);

	const uint16_t xoffset= uint16_t(descr - xdes->page.frame
					 + XDES_FLST_NODE);
	const uint16_t ioffset= uint16_t(seg_inode - iblock->page.frame);

#ifdef BTR_CUR_HASH_ADAPT
	if (ahi) {
		for (uint32_t i = 0; i < FSP_EXTENT_SIZE; i++) {
			if (!xdes_is_free(descr, i)) {
				/* Drop search system page hash index
				if the page is found in the pool and
				is hashed */
				btr_search_drop_page_hash_when_freed(
					page_id_t(space->id,
						 first_page_in_extent + i));
			}
		}
	}
#endif /* BTR_CUR_HASH_ADAPT */

	uint16_t lst;
	uint32_t limit = space->free_limit;

	if (xdes_is_full(descr)) {
		lst = static_cast<uint16_t>(FSEG_FULL + ioffset);
remove:
		err = flst_remove(iblock, lst, xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
	} else if (!xdes_get_n_used(descr)) {
		lst = static_cast<uint16_t>(FSEG_FREE + ioffset);
                goto remove;
	} else {
		err = flst_remove(
			iblock, static_cast<uint16_t>(FSEG_NOT_FULL + ioffset),
			xdes, xoffset, limit, mtr);
		if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
			return err;
		}
		uint32_t not_full_n_used = mach_read_from_4(
			FSEG_NOT_FULL_N_USED + seg_inode);
		uint32_t descr_n_used = xdes_get_n_used(descr);
		if (not_full_n_used < descr_n_used) {
			return DB_CORRUPTION;
		}
		mtr->write<4>(*iblock, seg_inode + FSEG_NOT_FULL_N_USED,
			      not_full_n_used - descr_n_used);
	}

	std::vector<uint8_t> going_to_free;
	static_assert(FSP_EXTENT_SIZE_MIN == 256, "compatibility");
	static_assert(FSP_EXTENT_SIZE_MAX == 64, "compatibility");

	for (uint32_t i = 0; i < FSP_EXTENT_SIZE; i++) {
		if (!xdes_is_free(descr, i)) {
			going_to_free.emplace_back(uint8_t(i));
		}
	}

	if (dberr_t err = fsp_free_extent(space, page, mtr)) {
		return err;
	}

	for (uint32_t i : going_to_free) {
		mtr->free(*space, first_page_in_extent + i);
		buf_page_free(space, first_page_in_extent + i, mtr);
	}

	return DB_SUCCESS;
}

/** Free the extent and fragment page associated with
the segment.
@param space       tablespace where segment resides
@param inode       index node information
@param iblock      page where segment header are placed
@param mtr         mini-transaction
@param hdr_page    segment header page
@param ahi         adaptive hash index
@return DB_SUCCESS_LOCKED_REC when freeing wasn't completed
@return DB_SUCCESS or other error code  when freeing was completed */
static
dberr_t fseg_free_step_low(fil_space_t *space, fseg_inode_t *inode,
                           buf_block_t *iblock, mtr_t *mtr,
                           const page_t *hdr_page
#ifdef BTR_CUR_HASH_ADAPT
                           , bool ahi=false
#endif /* BTR_CUR_HASH_ADAPT */
                           )
{
  dberr_t err= DB_SUCCESS;
  if (xdes_t *descr= fseg_get_first_extent(inode, space, mtr, &err))
  {
    err= fseg_free_extent(inode, iblock, space,
                          xdes_get_offset(descr), mtr
#ifdef BTR_CUR_HASH_ADAPT
                          , ahi
#endif /* BTR_CUR_HASH_ADAPT */
                        );
    return err == DB_SUCCESS ? DB_SUCCESS_LOCKED_REC : err;
  }

  if (err != DB_SUCCESS)
    return err;

  /* Free a fragment page. If there are no fragment pages
  exist in the array then free the file segment inode */
  ulint n = fseg_find_last_used_frag_page_slot(inode);
  if (UNIV_UNLIKELY(n == ULINT_UNDEFINED))
    return hdr_page
      ? DB_SUCCESS
      : fsp_free_seg_inode(space, inode, iblock, mtr);

  if (hdr_page &&
      !memcmp_aligned<2>(hdr_page + FIL_PAGE_OFFSET,
                         inode + FSEG_FRAG_ARR + n * FSEG_FRAG_SLOT_SIZE, 4))
    /* hdr_page is only passed by fseg_free_step_not_header().
    In that case, the header page must be preserved, to be freed
    when we're finally called by fseg_free_step(). */
    return DB_SUCCESS;

  uint32_t page_no= fseg_get_nth_frag_page_no(inode, n);
  err= fseg_free_page_low(space, inode, iblock, mtr, page_no
#ifdef BTR_CUR_HASH_ADAPT
                          , ahi
#endif /* BTR_CUR_HASH_ADAPT */
                          );
  if (err != DB_SUCCESS)
    return err;
  buf_page_free(space, page_no, mtr);
  if (!hdr_page &&
      fseg_find_last_used_frag_page_slot(inode) == ULINT_UNDEFINED)
    return fsp_free_seg_inode(space, inode, iblock, mtr);
  return DB_SUCCESS_LOCKED_REC;
}

bool fseg_free_step(buf_block_t *block, size_t header, mtr_t *mtr
#ifdef BTR_CUR_HASH_ADAPT
                    , bool ahi
#endif /* BTR_CUR_HASH_ADAPT */
                    ) noexcept
{
	fseg_inode_t*	inode;

	const page_id_t header_id{block->page.id()};
	fil_space_t* space = mtr->x_lock_space(header_id.space());
	xdes_t* descr = xdes_get_descriptor(space, header_id.page_no(), mtr);

	if (!descr) {
		return true;
	}

	/* Check that the header resides on a page which has not been
	freed yet */

	if (UNIV_UNLIKELY(xdes_is_free(descr,
				       header_id.page_no()
				       & (FSP_EXTENT_SIZE - 1)))) {
		/* Some corruption was detected: stop the freeing
		in order to prevent a crash. */
		return true;
	}
	buf_block_t* iblock;
	const ulint zip_size = space->zip_size();
	inode = fseg_inode_try_get(block->page.frame + header,
				   header_id.space(), zip_size,
				   mtr, &iblock);
	if (!inode || space->is_stopping()) {
		return true;
	}

	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}

	return fseg_free_step_low(space, inode, iblock, mtr, nullptr
#ifdef BTR_CUR_HASH_ADAPT
				  , ahi
#endif /* BTR_CUR_HASH_ADAPT */
				  ) != DB_SUCCESS_LOCKED_REC;
}

bool fseg_free_step_not_header(buf_block_t *block, size_t header, mtr_t *mtr
#ifdef BTR_CUR_HASH_ADAPT
                               , bool ahi
#endif /* BTR_CUR_HASH_ADAPT */
                               ) noexcept
{
	fseg_inode_t* inode;
	const page_id_t header_id{block->page.id()};
	ut_ad(mtr->is_named_space(header_id.space()));

	fil_space_t* space = mtr->x_lock_space(header_id.space());
	buf_block_t* iblock;

	inode = fseg_inode_try_get(block->page.frame + header,
				   header_id.space(), space->zip_size(),
				   mtr, &iblock);
	if (space->is_stopping()) {
		return true;
	}

	if (UNIV_UNLIKELY(!inode)) {
		sql_print_warning("InnoDB: Double free of page " UINT32PF
				  " in file %s",
				  header_id.page_no(),
				  space->chain.start->name);
		return true;
	}

	if (!space->full_crc32()) {
		fil_block_check_type(*iblock, FIL_PAGE_INODE, mtr);
	}

	return fseg_free_step_low(space, inode, iblock, mtr, block->page.frame
#ifdef BTR_CUR_HASH_ADAPT
				  , ahi
#endif /* BTR_CUR_HASH_ADAPT */
				  ) != DB_SUCCESS_LOCKED_REC;
}

/** Returns the first extent descriptor for a segment.
We think of the extent lists of the segment catenated in the order
FSEG_FULL -> FSEG_NOT_FULL -> FSEG_FREE.
@param[in]	inode		segment inode
@param[in]	space		tablespace
@param[in,out]	mtr		mini-transaction
@return the first extent descriptor
@retval nullptr if none, or on corruption */
MY_ATTRIBUTE((nonnull, warn_unused_result))
static
xdes_t*
fseg_get_first_extent(
	fseg_inode_t*		inode,
	const fil_space_t*	space,
	mtr_t*			mtr,
	dberr_t*		err)
{
  if (UNIV_UNLIKELY(space->id != page_get_space_id(page_align(inode)) ||
                    memcmp(inode + FSEG_MAGIC_N, FSEG_MAGIC_N_BYTES, 4)))
  {
  corrupted:
    *err= DB_CORRUPTION;
    return nullptr;
  }

  fil_addr_t first;

  if (flst_get_len(inode + FSEG_FULL))
    first= flst_get_first(inode + FSEG_FULL);
  else if (flst_get_len(inode + FSEG_NOT_FULL))
    first= flst_get_first(inode + FSEG_NOT_FULL);
  else if (flst_get_len(inode + FSEG_FREE))
    first= flst_get_first(inode + FSEG_FREE);
  else
  {
    *err= DB_SUCCESS;
    return nullptr;
  }

  if (first.page >= space->free_limit ||
      first.boffset < FSP_HEADER_OFFSET + FSP_HEADER_SIZE ||
      first.boffset >= space->physical_size() -
      (XDES_SIZE + FIL_PAGE_DATA_END))
    goto corrupted;

  return xdes_lst_get_descriptor(*space, first, mtr, nullptr, err);
}

#ifdef UNIV_BTR_PRINT
/*******************************************************************//**
Writes info of a segment. */
static void fseg_print_low(const fseg_inode_t *inode)
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

	const page_t* inode_page = page_align(inode);
	space = page_get_space_id(inode_page);
	page_no = page_get_page_no(inode_page);

	reserved = fseg_n_reserved_pages_low(inode, &used);

	seg_id = mach_read_from_8(inode + FSEG_ID);
	n_used = mach_read_from_4(inode + FSEG_NOT_FULL_N_USED);
	n_frag = fseg_get_n_frag_pages(inode);
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

	ut_ad(!memcmp(FSEG_MAGIC_N_BYTES, FSEG_MAGIC_N + inode, 4));
}

/*******************************************************************//**
Writes info of a segment. */
void
fseg_print(
/*=======*/
	fseg_header_t*	header, /*!< in: segment header */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  const fil_space_t *space=
    mtr->x_lock_space(page_get_space_id(page_align(header)));
  buf_block_t *block;
  if (fseg_inode_t *inode=
      fseg_inode_try_get(header, space->id, space->zip_size(), mtr, &block))
    fseg_print_low(inode);
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

/** Get the latched page page or acquire the page.
@param page_id  page identifier to be acquired
@param mtr      mini-transaction
@param err      error code
@return block descriptor */
static
buf_block_t *fsp_get_latched_page(
  page_id_t page_id, mtr_t *mtr, dberr_t *err)
{
  buf_block_t *block= nullptr;
  block= mtr->get_already_latched(
    page_id, MTR_MEMO_PAGE_SX_FIX);
  if (block)
    return block;
  return buf_page_get_gen(
    page_id, 0, RW_SX_LATCH, nullptr,
    BUF_GET_POSSIBLY_FREED, mtr, err);
}

/** Used during system tablespace truncation. Stores
the "to be modified" extent descriptor page and its
old page state */
class fsp_xdes_old_page
{
  std::vector<buf_block_t*> m_old_xdes_pages;
  const uint32_t m_space;
public:
  fsp_xdes_old_page(uint32_t space):m_space(space) {}
  uint32_t n_pages() noexcept
  {
    uint32_t count=0;
    for (uint32_t i= 0; i < m_old_xdes_pages.size(); i++)
      if (m_old_xdes_pages[i]) count++;
    return count;
  }

  __attribute__((warn_unused_result))
  dberr_t insert(uint32_t page_no, mtr_t *mtr) noexcept
  {
    uint32_t m_index= page_no >> srv_page_size_shift;
    if (m_old_xdes_pages.size() > m_index &&
        m_old_xdes_pages[m_index] != nullptr)
      return DB_SUCCESS;

    DBUG_EXECUTE_IF("shrink_buffer_pool_full",
                    return DB_OUT_OF_MEMORY;);
    dberr_t err= DB_SUCCESS;
    buf_block_t *block= fsp_get_latched_page(
                          page_id_t(m_space, page_no), mtr, &err);
    if (block)
    {
      buf_block_t *old= buf_LRU_get_free_block(have_no_mutex_soft);
      if (!old) return DB_OUT_OF_MEMORY;

      memcpy_aligned<UNIV_PAGE_SIZE_MIN>(
        old->page.frame, block->page.frame, srv_page_size);

      if (m_index >= m_old_xdes_pages.size())
        m_old_xdes_pages.resize(m_index + 1);
      m_old_xdes_pages[m_index] = old;
    }
    return err;
  }

  buf_block_t *search(uint32_t page_no) noexcept
  {
    uint32_t m_index= page_no >> srv_page_size_shift;
    if (m_index > m_old_xdes_pages.size())
      return nullptr;
    return m_old_xdes_pages[m_index];
  }

  void restore(mtr_t *mtr) noexcept
  {
    for (uint32_t i= 0; i < m_old_xdes_pages.size(); i++)
    {
      if (m_old_xdes_pages[i] == nullptr) continue;
      buf_block_t *block= mtr->get_already_latched(
        page_id_t{m_space, i << srv_page_size_shift},
        MTR_MEMO_PAGE_SX_FIX);
      ut_ad(block);
      memcpy_aligned<UNIV_PAGE_SIZE_MIN>(
        block->page.frame, m_old_xdes_pages[i]->page.frame, srv_page_size);
    }
  }

  ~fsp_xdes_old_page()
  {
    for (uint32_t i= 0; i < m_old_xdes_pages.size(); i++)
      if (m_old_xdes_pages[i])
        buf_block_free(m_old_xdes_pages[i]);
  }
};

/** Update the current descriptor entry with last valid
descriptor entry with skipped descriptor pages
@param header          File segment header
@param hdr_offset      FSP_FREE or FSP_FREE_FRAG
@param cur_addr        current descriptor
@param last_valid_addr last valid descriptor
@param skip_len        number of truncated extent descriptor entry
@param mtr             mini-transaction
@return error code or DB_SUCCESS */
__attribute__((warn_unused_result))
static
dberr_t fsp_lst_update_skip(
  buf_block_t *header, uint16_t hdr_offset,
  fil_addr_t cur_addr, fil_addr_t last_valid_addr,
  uint32_t skip_len, mtr_t *mtr) noexcept
{
  dberr_t err= DB_SUCCESS;
  uint32_t space_id= header->page.id().space();
  buf_block_t *cur= fsp_get_latched_page(
    page_id_t(space_id, cur_addr.page), mtr, &err);

  if (!cur) return err;
  if (last_valid_addr.page == FIL_NULL)
  {
    /* First node, so update the FIRST pointer of base
    with current extent descriptor and update
    the PREV pointer of last valid descriptor with
    FIL_NULL */
    flst_write_addr(
      *header,
      header->page.frame + hdr_offset + FLST_FIRST,
      cur_addr.page, cur_addr.boffset, mtr);

    flst_write_addr(
      *cur,
      cur->page.frame + cur_addr.boffset + FLST_PREV,
      last_valid_addr.page, last_valid_addr.boffset, mtr);
  }
  else
  {
    buf_block_t *prev= nullptr;
    if (cur->page.id().page_no() == last_valid_addr.page)
      prev= cur;
    else
    {
      prev= fsp_get_latched_page(
              page_id_t(space_id, last_valid_addr.page),
              mtr, &err);
      if (!prev) return err;
    }

    /* Update the NEXT pointer of last valid extent
    descriptor entry with current extent descriptor */
    flst_write_addr(
      *prev,
      prev->page.frame + last_valid_addr.boffset + FLST_NEXT,
      cur_addr.page, cur_addr.boffset, mtr);

    /* Update the PREV pointer of current extent
    descriptor entry with last valid extent descriptor */
    flst_write_addr(
      *cur,
      cur->page.frame + cur_addr.boffset + FLST_PREV,
      last_valid_addr.page, last_valid_addr.boffset, mtr);
  }

  byte *len_bytes= &header->page.frame[hdr_offset + FLST_LEN];
  uint32_t len= mach_read_from_4(len_bytes);
  ut_ad(len > skip_len);
  mtr->write<4>(*header, len_bytes, len - skip_len);
  return DB_SUCCESS;
}

/** Write the FLST_NEXT pointer of the last valid node with FIL_NULL
@param header          File segment header
@param hdr_offset      FSP_HEADER_OFFSET + FSP_FREE or FSP_FREE_FRAG
@param cur_addr        current descriptor
@param skip_len        number of truncated extent descriptor entry
@param orig_len        original length of the list
@param mtr             mini-transaction
@return error code or DB_SUCCESS */
__attribute__((warn_unused_result))
dberr_t
fsp_lst_write_end(
  buf_block_t *header, uint16_t hdr_offset,
  fil_addr_t cur_addr, uint32_t skip_len, uint32_t orig_len,
  mtr_t *mtr) noexcept
{
  dberr_t err= DB_SUCCESS;
  byte *len_bytes= &header->page.frame[hdr_offset + FLST_LEN];
  uint32_t len= mach_read_from_4(len_bytes);
  if (skip_len == 0)
  {
func_exit:
    if (hdr_offset == FSP_FREE_FRAG + FSP_HEADER_OFFSET)
    {
      byte *frag_used_byte= &header->page.frame[
        FSP_HEADER_OFFSET + FSP_FRAG_N_USED];
      uint32_t n_used_frag= mach_read_from_4(frag_used_byte);
      /* Update the FSP_FRAG_N_USED value after removing
      the truncated pages from FSP_FREE_FRAG list */
      if (len != orig_len)
        mtr->write<4>(*header, frag_used_byte,
                      n_used_frag - ((orig_len - len) * 2));
    }
    return DB_SUCCESS;
  }

  if (cur_addr.page == FIL_NULL)
  {
    /* There is no list, so reset base node */
    mtr->memset(
      header,
      FLST_FIRST + FIL_ADDR_PAGE + hdr_offset, 4, 0xff);
    mtr->memset(
      header,
      FLST_LAST + FIL_ADDR_PAGE + hdr_offset, 4, 0xff);
  }
  else
  {
    /* Update the FLST_LAST pointer in base node with current
    valid extent descriptor and mark the FIL_NULL as next in
    current extent descriptr */
    flst_write_addr(
      *header,
      header->page.frame + hdr_offset + FLST_LAST,
      cur_addr.page, cur_addr.boffset, mtr);

    buf_block_t *cur_block= fsp_get_latched_page(
      page_id_t(header->page.id().space(), cur_addr.page),
      mtr, &err);

    if (!cur_block) return err;

    flst_write_addr(
      *cur_block,
      cur_block->page.frame + cur_addr.boffset + FLST_NEXT,
      FIL_NULL, 0, mtr);
  }

  ut_ad(len >= skip_len);
  len-= skip_len;
  mtr->write<4>(*header, len_bytes, len);
  goto func_exit;
}

/** Remove the truncated extents from the FSP_FREE list
@param header     tablespace header
@param hdr_offset FSP_FREE or FSP_FREE_FRAG
@param threshold  Remove the pages from the list which is
                  greater than threshold
@param mtr        mini-transaction to remove the extents
@return DB_SUCCESS on success or error code */
__attribute__((warn_unused_result))
static
dberr_t fsp_shrink_list(buf_block_t *header, uint16_t hdr_offset,
                        uint32_t threshold, mtr_t *mtr) noexcept
{
  ut_ad(mach_read_from_4(header->page.frame + FIL_PAGE_OFFSET) == 0);
  const uint32_t len= flst_get_len(hdr_offset + header->page.frame);
  if (len == 0)
    return DB_SUCCESS;

  buf_block_t *descr_block= nullptr;
  dberr_t err= DB_SUCCESS;
  uint32_t skip_len= 0;
  fil_addr_t last_valid_addr {FIL_NULL, 0}, next_addr{FIL_NULL, 0};
  fil_addr_t addr= flst_get_first(header->page.frame + hdr_offset);

  for (uint32_t i= len; i > 0; i--)
  {
    ut_d(fil_space_t *space= header->page.id().space() == 0
                             ? fil_system.sys_space
                             : fil_system.temp_space);
    ut_ad(addr.page < space->size);
    ut_ad(!(addr.page & (srv_page_size - 1)));
    if (!descr_block || descr_block->page.id().page_no() != addr.page)
    {
      descr_block= fsp_get_latched_page(
        page_id_t(header->page.id().space(), addr.page), mtr, &err);
      if (!descr_block)
        return err;
    }

    if (addr.page < threshold)
    {
      /* Update only if only non-truncated page */
      if (skip_len)
      {
        err= fsp_lst_update_skip(
          header, hdr_offset, addr, last_valid_addr, skip_len, mtr);
        if (err) return err;
        skip_len= 0;
      }

      if (threshold <= xdes_get_offset(
            descr_block->page.frame + addr.boffset - XDES_FLST_NODE))
        skip_len++;
      else last_valid_addr= addr;
    }
    else skip_len++;

    next_addr= flst_get_next_addr(
      descr_block->page.frame + addr.boffset);
    if (next_addr.page != addr.page && addr.page >= threshold)
    {
      mtr->release_last_page();
      descr_block= nullptr;
    }

    if (next_addr.page == FIL_NULL)
    {
      err= fsp_lst_write_end(header, hdr_offset, last_valid_addr,
                             skip_len, len, mtr);
      break;
    }
    addr= next_addr;
  }
  ut_d(if (err == DB_SUCCESS) flst_validate(header, hdr_offset, mtr););
  return err;
}

/** Reset the XDES_BITMAP for the truncated extents
@param  space      tablespace to be truncated
@param  threshold  truncated size
@param  mtr        mini-transaction to reset XDES_BITMAP
@return DB_SUCCESS or error code on failure */
__attribute__((warn_unused_result))
static
dberr_t fsp_xdes_reset(uint32_t space_id, uint32_t threshold, mtr_t *mtr)
{
  if (!(threshold & (srv_page_size - 1)))
    return DB_SUCCESS;

  uint32_t cur_descr_page= xdes_calc_descriptor_page(0, threshold);
  ulint descr_offset= XDES_ARR_OFFSET + XDES_SIZE
          * xdes_calc_descriptor_index(0, threshold);
  ulint last_descr_offset= XDES_ARR_OFFSET + XDES_SIZE
          * xdes_calc_descriptor_index(
               0, (cur_descr_page + srv_page_size - 1));
  last_descr_offset+= XDES_SIZE;
  dberr_t err= DB_SUCCESS;
  buf_block_t *block= fsp_get_latched_page(
    page_id_t(space_id, cur_descr_page), mtr, &err);
  if (!block)
    return err;
  mtr->memset(
    block, descr_offset, (last_descr_offset - descr_offset), 0);
  return err;
}

/** This function does 2 things by traversing all the used
extents in the system tablespace
1) Find the last used extent
2) Store the old page frame of the "to be modified" extent
descriptor pages.
@param space             system tablespace
@param last_used_extent  value is 0 in case of finding the last used
                         extent; else it could be last used extent
@param old_xdes_entry    nullptr or object to store the
                         old page content of "to be modified"
                         extent descriptor pages
@return DB_SUCCESS or error code */
__attribute__((warn_unused_result))
dberr_t fsp_traverse_extents(
  fil_space_t *space, uint32_t *last_used_extent, mtr_t *mtr,
  fsp_xdes_old_page *old_xdes_entry= nullptr)
{
  dberr_t err= DB_SUCCESS;
  bool find_last_used_extent= (old_xdes_entry == nullptr);
  uint32_t threshold= *last_used_extent;
  uint32_t last_descr_page_no= xdes_calc_descriptor_page(
    0, space->free_limit - 1);

  if (find_last_used_extent)
    *last_used_extent= space->free_limit;
  else
  {
    err= old_xdes_entry->insert(0, mtr);
    if (err == DB_SUCCESS && threshold & (srv_page_size - 1))
      err= old_xdes_entry->insert(
        xdes_calc_descriptor_page(0, threshold), mtr);
    if (err) return err;
  }

  buf_block_t *block= nullptr;
  std::vector<uint32_t> modified_xdes;

  for (uint32_t cur_extent=
       ((space->free_limit - 1)/ FSP_EXTENT_SIZE) * FSP_EXTENT_SIZE;
       cur_extent >= threshold;)
  {
    if (!block)
    {
      block= fsp_get_latched_page(
               page_id_t(space->id, last_descr_page_no),
               mtr, &err);
      if (!block) return err;
    }

    xdes_t *descr= XDES_ARR_OFFSET + XDES_SIZE
      * xdes_calc_descriptor_index(0, cur_extent)
      + block->page.frame;

    if (find_last_used_extent)
    {
      ulint state= xdes_get_state(descr);
      if (state == XDES_FREE)
        *last_used_extent= cur_extent;
      else if (state == XDES_FREE_FRAG &&
               !(cur_extent & (srv_page_size - 1)) &&
               xdes_get_n_used(descr) == 2)
        /* Extent Descriptor Page */
        *last_used_extent= cur_extent;
      else return DB_SUCCESS;
    }
    else
    {
      fil_addr_t prev_addr= flst_get_prev_addr(
                              descr + XDES_FLST_NODE);
      ut_ad(prev_addr.page < space->size ||
            prev_addr.page == FIL_NULL);
      ut_ad(prev_addr.page == FIL_NULL ||
            !(prev_addr.page & (srv_page_size - 1)));

      fil_addr_t next_addr= flst_get_next_addr(
                              descr + XDES_FLST_NODE);
      ut_ad(next_addr.page < space->size ||
            next_addr.page == FIL_NULL);
      ut_ad(next_addr.page == FIL_NULL ||
            !(next_addr.page & (srv_page_size - 1)));

      if (prev_addr.page < threshold)
        modified_xdes.push_back(prev_addr.page);

      if (next_addr.page < threshold)
        modified_xdes.push_back(next_addr.page);
    }

    cur_extent-= FSP_EXTENT_SIZE;
    uint32_t cur_descr_page= xdes_calc_descriptor_page(0, cur_extent);
    if (last_descr_page_no != cur_descr_page)
    {
      if (last_descr_page_no >= threshold)
        mtr->release_last_page();
      last_descr_page_no= cur_descr_page;
      block= nullptr;
    }
  }

  if (!find_last_used_extent)
  {
    for (auto it : modified_xdes)
    {
      err= old_xdes_entry->insert(it, mtr);
      if (err) return err;
    }
    modified_xdes.clear();
  }
  return err;
}

/** Validate the system tablespace list */
__attribute__((warn_unused_result))
static dberr_t fsp_tablespace_validate(fil_space_t *space,
                                       mtr_t *mtr) noexcept
{
  /* Validate all FSP list in system tablespace */
  dberr_t err= DB_SUCCESS;
  if (buf_block_t *header= fsp_get_header(space, mtr, &err))
  {
    err= flst_validate(header, FSP_FREE + FSP_HEADER_OFFSET, mtr);
    if (err == DB_SUCCESS)
      err= flst_validate(header, FSP_FREE_FRAG + FSP_HEADER_OFFSET,
                         mtr);
    if (err == DB_SUCCESS)
      err= flst_validate(header, FSP_HEADER_OFFSET + FSP_FULL_FRAG,
                         mtr);
    if (err == DB_SUCCESS)
      err= flst_validate(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL,
                         mtr);
    if (err == DB_SUCCESS)
      err= flst_validate(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                         mtr);
  }
  return err;
}

/** Store the inode information which basically stores
the page and offset */
struct inode_info : private std::unordered_set<uint64_t>
{
public:
  /** Register an inode
  @param page index node page
  @param offset index node offset within the page
  @retval true in case of successful registeration
  @retval false in case of invalid entry or already inserted inode */
  __attribute__((warn_unused_result))
  bool insert_inode(uint32_t page, uint16_t offset)
  {
    return page < fil_system.sys_space->free_limit &&
      offset >= FIL_PAGE_DATA && offset < srv_page_size - FIL_PAGE_DATA_END &&
      emplace(uint64_t{page} << 32 | offset).second;
  }

  /** Register an inode
  @param inode index node information
  @retval true in case of successful registeration
  @retval false in case of invalid entry or already inserted inode */
  __attribute__((warn_unused_result))
  bool insert_seg(const byte *inode)
  {
    return insert_inode(mach_read_from_4(inode + 4),
                        mach_read_from_2(inode + 8));
  }

  __attribute__((warn_unused_result))
  bool find(uint32_t page, uint16_t offset) const
  {
    return std::unordered_set<uint64_t>::find(uint64_t{page} << 32 |
                                              offset) != end();
  }

  /** Get the unused inode segment header from the list of index
  node pages.
  @param boffset offset for the FSP_SEG_INODES_FULL
                 or FSP_SEG_INODES_FREE list in fsp header page
  @param unused  store the unused information
  @return error code */
  dberr_t get_unused(uint16_t boffset, inode_info *unused) const
  {
    dberr_t err= DB_SUCCESS;
    buf_block_t *block= buf_pool.page_fix(page_id_t{0, 0}, &err,
                                          buf_pool_t::FIX_WAIT_READ);
    if (!block)
      return err;
    buf_block_t *header= block;
    const uint32_t len= flst_get_len(block->page.frame + boffset);
    fil_addr_t addr= flst_get_first(block->page.frame + boffset);
    ulint n_inode_per_page=
      FSP_SEG_INODES_PER_PAGE(fil_system.sys_space->physical_size());
    for (uint32_t i= len; i--; )
    {
      if (addr.boffset < FIL_PAGE_DATA ||
          addr.boffset >= block->physical_size() - FIL_PAGE_DATA_END)
      {
        err= DB_CORRUPTION;
        break;
      }

      block= buf_pool.page_fix(page_id_t{0, addr.page}, &err,
                               buf_pool_t::FIX_WAIT_READ);
      if (!block)
        break;

      fil_addr_t next_addr= flst_get_next_addr(block->page.frame +
                                               addr.boffset);
      for (uint32_t i= 0; i < n_inode_per_page; i++)
      {
        const fseg_inode_t *inode=
          fsp_seg_inode_page_get_nth_inode(block->page.frame, i);
        /* Consider TRX_SYS_FSEG_HEADER as used segment.
        While reinitializing the undo tablespace, InnoDB
        fail to reset the value of TRX_SYS_FSEG_HEADER
        in TRX_SYS page. so InnoDB shouldn't consider
        this segment as unused one */
        switch (mach_read_from_8(FSEG_ID + inode)) {
        case 0: case 2:
          continue;
        }
	uint16_t offset= uint16_t(inode - block->page.frame);
        if (offset < FIL_PAGE_DATA ||
            offset >= block->physical_size() - FIL_PAGE_DATA_END)
        {
          err= DB_CORRUPTION;
          break;
        }

        if (!find(addr.page, offset) &&
            !unused->insert_inode(addr.page, offset))
        {
          err= DB_DUPLICATE_KEY;
          break;
        }
      }
      addr= next_addr;
      block->page.unfix();
      if (err)
        break;
    }
    ut_ad(addr.page == FIL_NULL || err != DB_SUCCESS);
    header->page.unfix();
    return err;
  }

  /** Free the segment information present in the set
  @return error code */
  dberr_t free_segs();
};

/** Get the file segments from root page
@param inodes store the index nodes information
@param root   root page
@return error code */
static dberr_t fsp_table_inodes_root(inode_info *inodes, uint32_t root)
{
  if (root == FIL_NULL)
    return DB_SUCCESS;

  dberr_t err= DB_SUCCESS;
  buf_block_t *block= buf_pool.page_fix(page_id_t{0, root}, &err,
                                        buf_pool_t::FIX_WAIT_READ);
  if (!block)
    return err;

  if (!inodes->insert_seg(block->page.frame + PAGE_HEADER + PAGE_BTR_SEG_TOP))
    err= DB_CORRUPTION;

  if (!inodes->insert_seg(block->page.frame + PAGE_HEADER + PAGE_BTR_SEG_LEAF))
    err= DB_CORRUPTION;

  block->page.unfix();
  return err;
}

/** Add the file segment of all root pages in table
@param inodes store the index nodes information
@param table  table to be read
@return error code */
static dberr_t add_index_root_pages(inode_info *inodes, dict_table_t *table)
{
  dberr_t err= DB_SUCCESS;
  for (auto i= UT_LIST_GET_FIRST(table->indexes);
       i != nullptr && err == DB_SUCCESS; i= UT_LIST_GET_NEXT(indexes, i))
    err= fsp_table_inodes_root(inodes, i->page);
  return err;
}

/** Determine the inodes used by tables in the system tablespace.
@param inodes store the index nodes information
@param mtr    mini-transaction
@return error code */
static dberr_t fsp_table_inodes(inode_info *inodes, mtr_t *mtr)
{
  btr_pcur_t pcur;
  ulint len;
  const auto savepoint= mtr->get_savepoint();
  dberr_t err= DB_SUCCESS;
  dict_sys.freeze(SRW_LOCK_CALL);
  for (const rec_t *rec= dict_startscan_system(&pcur, mtr,
                                               dict_sys.sys_indexes);
       rec; rec= dict_getnext_system_low(&pcur, mtr))
  {
    const byte *field=
      rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__SPACE, &len);
    if (len != 4)
    {
      err= DB_CORRUPTION;
      break;
    }
    uint32_t space= mach_read_from_4(field);
    if (space > 0) continue;

    field= rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);
    if (len != 4)
    {
      err= DB_CORRUPTION;
      break;
    }
    err= fsp_table_inodes_root(inodes, mach_read_from_4(field));
    if (err)
      break;
  }
  mtr->rollback_to_savepoint(savepoint);
  dict_sys.unfreeze();

  if (err == DB_SUCCESS)
    err= add_index_root_pages(inodes, dict_sys.sys_tables);
  if (err == DB_SUCCESS)
    err= add_index_root_pages(inodes, dict_sys.sys_indexes);
  if (err == DB_SUCCESS)
    err= add_index_root_pages(inodes, dict_sys.sys_columns);
  if (err == DB_SUCCESS)
    err= add_index_root_pages(inodes, dict_sys.sys_fields);
  return err;
}

/* Get the used inode from the system tablespace
@param inodes inode information used found in system tablespace
@param mtr    mini-transaction
@return error code */
static dberr_t fsp_get_sys_used_segment(inode_info *inodes, mtr_t *mtr)
{
  dberr_t err= DB_SUCCESS;
  buf_block_t *block= nullptr;
  /* Get TRX_SYS_FSEG_HEADER, TRX_SYS_DOUBLEWRITE_FSEG from
  TRX_SYS_PAGE */
  block= buf_pool.page_fix(page_id_t{0, TRX_SYS_PAGE_NO}, &err,
                           buf_pool_t::FIX_WAIT_READ);
  if (!block)
    return err;

  fil_addr_t sys_fseg_addr= flst_read_addr(block->page.frame +
                                           TRX_SYS + TRX_SYS_FSEG_HEADER + 4);
  if (sys_fseg_addr.page == 0 && sys_fseg_addr.boffset == 0)
  {
    /* While reinitializing the undo tablespace, InnoDB fail
    to reset the TRX_SYS_FSEG_HEADER offset in TRX_SYS page */
  }
  else if (!inodes->insert_inode(sys_fseg_addr.page, sys_fseg_addr.boffset))
    err= DB_CORRUPTION;

  if (!inodes->insert_seg(block->page.frame + TRX_SYS_DOUBLEWRITE +
                          TRX_SYS_DOUBLEWRITE_FSEG))
    err= DB_CORRUPTION;

  block->page.unfix();

  if (err)
    return err;

  block= buf_pool.page_fix(page_id_t{0, DICT_HDR_PAGE_NO}, &err,
                           buf_pool_t::FIX_WAIT_READ);
  if (!block)
    return err;

  if (!inodes->insert_seg(block->page.frame + DICT_HDR + DICT_HDR_FSEG_HEADER))
    err= DB_CORRUPTION;

  block->page.unfix();

  if (err)
    return err;

  block= buf_pool.page_fix(page_id_t{0, FSP_IBUF_HEADER_PAGE_NO},
                           &err, buf_pool_t::FIX_WAIT_READ);
  if (!block)
    return err;
  if (!inodes->insert_seg(block->page.frame + PAGE_DATA))
    err= DB_CORRUPTION;

  block->page.unfix();

  /* Get rollback segment header page */
  for (ulint rseg_id= 0; rseg_id < TRX_SYS_N_RSEGS && err == DB_SUCCESS;
       rseg_id++)
  {
    trx_rseg_t *rseg= &trx_sys.rseg_array[rseg_id];
    if (rseg->space->id == 0)
    {
      block= buf_pool.page_fix(rseg->page_id(), &err,
                               buf_pool_t::FIX_WAIT_READ);
      if (!block)
        break;
      if (!inodes->insert_seg(block->page.frame + TRX_RSEG +
                              TRX_RSEG_FSEG_HEADER))
        err= DB_CORRUPTION;
      block->page.unfix();
    }
  }

  if (err == DB_SUCCESS)
    err= fsp_table_inodes(inodes, mtr);
  return err;
}

/** Free the extents, fragment page from the given inode
@param page_no index node page number
@param offset  index node offset within page
@return error code */
static dberr_t fseg_inode_free(uint32_t page_no, uint16_t offset)
{
  fil_space_t *space= fil_system.sys_space;
  dberr_t err= DB_SUCCESS;
  mtr_t mtr;
  mtr.start();
  mtr.x_lock_space(space);
  buf_block_t *iblock= buf_page_get_gen(page_id_t{0, page_no}, 0,
                                        RW_X_LATCH, nullptr, BUF_GET,
                                        &mtr, &err);

  fseg_inode_t *inode= nullptr;
  DBUG_EXECUTE_IF("unused_undo_free_fail_4",
                  iblock= nullptr; err= DB_CORRUPTION;);
  if (!iblock)
    goto func_exit;

  inode= iblock->page.frame + offset;
  while ((err= fseg_free_step_low(space, inode, iblock,
                                  &mtr, nullptr)) == DB_SUCCESS_LOCKED_REC)
  {
    DBUG_EXECUTE_IF("unused_undo_free_fail_5",
                    err= DB_CORRUPTION;
                    goto func_exit;);
    iblock->fix();
    mtr.commit();

    mtr.start();
    mtr.x_lock_space(space);
    iblock->page.lock.x_lock();
    mtr.memo_push(iblock, MTR_MEMO_PAGE_X_FIX);
  }
  /* These are all leaked undo log segments. That means there is no
  way to access these undo log segments other than traversing
  the index node page. Above fseg_free_step_low() clears
  the undo segment header page as well. */
func_exit:
  mtr.commit();
  return err;
}

/** Free the unused segment
@return error code */
dberr_t inode_info::free_segs()
{
  for (auto i : *this)
  {
    uint32_t page= uint32_t(i >> 32);
    uint16_t offset= uint16_t(i);
    if (dberr_t err= fseg_inode_free(page, offset))
    {
      sql_print_error("InnoDB: :autoshrink failed to free the "
                      "segment %u in page " UINT32PF, unsigned{offset},
                      page);
      return err;
    }
    sql_print_information("InnoDB: :autoshrink freed the segment "
                          "%u in page " UINT32PF, unsigned{offset}, page);
  }
  return DB_SUCCESS;
}

bool trx_sys_t::is_xa_exist() noexcept
{
  for (const trx_rseg_t &rseg : trx_sys.rseg_array)
  {
    if (rseg.page_no == FIL_NULL)
      continue;
    const trx_undo_t *undo= UT_LIST_GET_FIRST(rseg.undo_list);
    while (undo)
    {
      if (undo->state == TRX_UNDO_PREPARED)
        return true;
      undo= UT_LIST_GET_NEXT(undo_list, undo);
    }
  }
  return false;
}

/** Remove the unused segment in tablespace. This function
used only during shrinking of system tablespace
@param shutdown called during slow shutdown
@return error code */
dberr_t fil_space_t::garbage_collect(bool shutdown)
{
  if ((shutdown && trx_sys_t::is_xa_exist()) ||
      (!shutdown && !trx_sys.is_undo_empty()))
  {
    sql_print_warning("InnoDB: Cannot free the unused segments"
                      " in system tablespace because a previous"
                      " shutdown was not with innodb_fast_shutdown=0"
                      " or XA PREPARE transactions exist");
    return DB_SUCCESS;
  }

  ut_a(id == 0);
  /* Collect all the used segment inode entries */
  mtr_t mtr;
  mtr.start();
  inode_info used_inodes, unused_inodes;
  dberr_t err= fsp_get_sys_used_segment(&used_inodes, &mtr);
  DBUG_EXECUTE_IF("unused_undo_free_fail_1", err= DB_CORRUPTION;);
  if (err)
  {
    sql_print_error("InnoDB: :autoshrink failed to read the "
                    "used segment due to %s", ut_strerr(err));
    mtr.commit();
    return err;
  }

  const char *ctx= "in FSP_SEG_INODES_FULL list";
  err= used_inodes.get_unused(FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL,
                              &unused_inodes);
  DBUG_EXECUTE_IF("unused_undo_free_fail_2", err= DB_CORRUPTION;);

  if (err == DB_SUCCESS)
  {
    ctx= "in FSP_SEG_INODES_FREE list";
    err= used_inodes.get_unused(FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                                &unused_inodes);
    DBUG_EXECUTE_IF("unused_undo_free_fail_3", err= DB_CORRUPTION;);
  }

  mtr.commit();
  if (err)
  {
    sql_print_error("InnoDB: :autoshrink failed due to "
                    "%s %s ", ut_strerr(err), ctx);
    return err;
  }

  /* Reset the undo log segments slots in the rollback segment header
  which exist in system tablespace. Undo cached segment will be
  treated as unused file segment. These segments will be freed as a
  part of inode_info::free_segs  */
  mtr.start();
  mtr.x_lock_space(fil_system.sys_space);
  for (trx_rseg_t &rseg : trx_sys.rseg_array)
  {
    if (rseg.space == fil_system.sys_space &&
        UT_LIST_GET_LEN(rseg.undo_cached))
    {
      buf_block_t *block=
        buf_page_get_gen(page_id_t{0, rseg.page_no}, 0,
                         RW_X_LATCH, nullptr, BUF_GET, &mtr, &err);
      if (!block)
      {
        mtr.commit();
        return err;
      }

      mtr.memset(block, TRX_RSEG_UNDO_SLOTS + TRX_RSEG,
                 TRX_RSEG_N_SLOTS * TRX_RSEG_SLOT_SIZE, 0xff);
      rseg.reinit(rseg.page_no);
    }
  }
  mtr.commit();

  return unused_inodes.free_segs();
}

class SpaceDefragmenter;

namespace flst
{
  /** Validate the file list node for the system tablespace.
  @param addr file space address
  @return true if validation successful or false */
  static bool node_valid(const fil_addr_t *addr) noexcept
  {
    return addr->boffset >= FIL_PAGE_DATA &&
           addr->boffset < (srv_page_size - FIL_PAGE_DATA_END);
  }

  /** Prepare the steps for removing the file list node
  @param descr_block descriptor block
  @param xoffset     descriptor offset within the block
  @param free_limit maximum free limit in the tablespace
  @param mtr        mini-transaction
  @param prev_block previous block in the list
  @param next_block next block in the list
  @return error code */
  static dberr_t remove_prepare(const buf_block_t &descr_block,
                                uint32_t xoffset, uint32_t free_limit,
                                mtr_t *mtr, buf_block_t **prev_block,
                                buf_block_t **next_block) noexcept
  {
    const xdes_t *descr= descr_block.page.frame + xoffset;
    fil_addr_t prev_addr= flst_get_prev_addr(descr);
    fil_addr_t next_addr= flst_get_next_addr(descr);
    dberr_t err= DB_SUCCESS;

    if (prev_addr.page != FIL_NULL)
    {
      if (!node_valid(&prev_addr))
        return DB_CORRUPTION;

      *prev_block= fsp_get_latched_page(page_id_t{0, prev_addr.page},
                                        mtr, &err);
      ut_ad(!*prev_block == (err != DB_SUCCESS));

      if (!*prev_block)
        return err;

      fil_addr_t cur_addr=
        flst_get_next_addr((*prev_block)->page.frame
                             + prev_addr.boffset);
      if (cur_addr.page != descr_block.page.id().page_no() ||
          cur_addr.boffset != xoffset)
        return DB_CORRUPTION;
    }

    if (next_addr.page != FIL_NULL)
    {
      if (!node_valid(&next_addr))
        return DB_CORRUPTION;

      *next_block= fsp_get_latched_page(page_id_t{0, next_addr.page},
                                        mtr, &err);
      ut_ad(!*next_block == (err != DB_SUCCESS));
      if (!*next_block)
        return err;

      fil_addr_t cur_addr=
        flst_get_prev_addr((*next_block)->page.frame + next_addr.boffset);
      if (cur_addr.page != descr_block.page.id().page_no() ||
          cur_addr.boffset != xoffset)
        return DB_CORRUPTION;
    }

    return err;
  }

  /** Complete the steps for removing the file list node
  @param base       base block where free list starts
  @param boffset    offset where list starts
  @param descr      descriptor to be removed
  @param mtr        mini-transaction */
  static void remove_complete(buf_block_t *base, uint16_t boffset,
                              xdes_t *descr, mtr_t *mtr) noexcept
  {
    fil_addr_t prev_addr= flst_get_prev_addr(descr + XDES_FLST_NODE);
    fil_addr_t next_addr= flst_get_next_addr(descr + XDES_FLST_NODE);
    /* remove_prepare() checked these already */
    ut_ad(next_addr.page == FIL_NULL || node_valid(&next_addr));
    ut_ad(prev_addr.page == FIL_NULL || node_valid(&prev_addr));
    byte *list= base->page.frame + boffset;

    buf_block_t *prev_block= nullptr;
    buf_block_t *next_block= nullptr;

    if (prev_addr.page != FIL_NULL)
    {
      prev_block=
        mtr->get_already_latched(page_id_t{0, prev_addr.page},
                                 MTR_MEMO_PAGE_SX_FIX);
      ut_ad(prev_block);

      flst_write_addr(*prev_block, prev_block->page.frame +
                      prev_addr.boffset + FLST_NEXT,
                      next_addr.page, next_addr.boffset, mtr);
    }
    else
      flst_write_addr(*base, list + FLST_FIRST,
                      next_addr.page, next_addr.boffset, mtr);

    if (next_addr.page != FIL_NULL)
    {
      next_block=
        mtr->get_already_latched(page_id_t{0, next_addr.page},
                                 MTR_MEMO_PAGE_SX_FIX);
      ut_ad(next_block);

      flst_write_addr(*next_block, next_block->page.frame +
                      next_addr.boffset + FLST_PREV,
                      prev_addr.page, prev_addr.boffset, mtr);
    }
    else
      flst_write_addr(*base, list + FLST_LAST,
                      prev_addr.page, prev_addr.boffset, mtr);

    /* All callers of remove_prepare() does check the FLST_LEN of
    the list */
    byte *len= list + FLST_LEN;
    mtr->write<4>(*base, len, mach_read_from_4(len) - 1);
  }

  /** Prepare the steps for adding the block into last of the list
  @param base             block where list starts
  @param boffset          offset to find the list
  @param free_limit       maximum free limit in the tablespace
  @param mtr              mini-transaction
  @param last_block_list  last block in the list
  @return error code */
  static dberr_t append_prepare(const buf_block_t &base, uint16_t boffset,
                                uint32_t free_limit, mtr_t *mtr,
                                buf_block_t **last_block_list) noexcept
  {
    ut_ad(!*last_block_list);
    if (!flst_get_len(base.page.frame + boffset))
      return DB_SUCCESS;

    fil_addr_t addr= flst_get_last(base.page.frame + boffset);

    if (addr.page >= free_limit)
      return DB_CORRUPTION;

    if (!node_valid(&addr))
      return DB_CORRUPTION;

    dberr_t err= DB_SUCCESS;
    *last_block_list= fsp_get_latched_page(page_id_t{0, addr.page},
                                           mtr, &err);
    return err;
  }

  /** Complete the steps for adding the block into last of the list
  @param base       base block where free list starts
  @param boffset    offset where list starts
  @param curr       extent descriptor block
  @param coffset    offset to point the descriptor
  @param mtr        mini-transaction */
  static void append_complete(buf_block_t *base, uint16_t boffset,
                              buf_block_t *curr, uint16_t coffset,
                              mtr_t *mtr) noexcept
  {
    fil_addr_t last_addr= flst_get_last(base->page.frame + boffset);
    ut_ad(last_addr.page == FIL_NULL || node_valid(&last_addr));
    buf_block_t *last_block_list= nullptr;
    if (last_addr.page != FIL_NULL)
    {
      last_block_list=
        mtr->get_already_latched(page_id_t{0, last_addr.page},
                                 MTR_MEMO_PAGE_SX_FIX);
      ut_ad(last_block_list);

      fil_addr_t addr= flst_get_last(base->page.frame + boffset);

      flst_write_addr(*last_block_list,
                      last_block_list->page.frame + addr.boffset +
                      FLST_NEXT,
                      curr->page.id().page_no(), coffset, mtr);
      flst_write_addr(*curr,
                      curr->page.frame + coffset + FLST_PREV,
                      addr.page, addr.boffset, mtr);
      flst_write_addr(*base, base->page.frame + boffset + FLST_LAST,
                      curr->page.id().page_no(), coffset, mtr);
    }
    else
    {
      /* Encountered empty list. So add current block as FIRST
      and LAST block in the list */
      flst_write_addr(*curr,
                      curr->page.frame + coffset + FLST_PREV,
                      FIL_NULL, 0, mtr);
      flst_write_addr(*base, base->page.frame + boffset + FLST_FIRST,
                      curr->page.id().page_no(), coffset, mtr);
      memcpy(base->page.frame + boffset + FLST_LAST,
             base->page.frame + boffset + FLST_FIRST, FIL_ADDR_SIZE);
      mtr->memmove(*base, boffset + FLST_LAST,
                   boffset + FLST_FIRST, FIL_ADDR_SIZE);
    }

    flst_write_addr(*curr,
                    curr->page.frame + coffset + FLST_NEXT,
                    FIL_NULL, 0, mtr);

    byte *len= base->page.frame + boffset + FLST_LEN;
    mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
  }
} /* namespace flst */

static dberr_t fseg_validate_low(fil_space_t *space, dict_index_t *index,
                                 mtr_t *mtr) noexcept
{
  dberr_t err= DB_SUCCESS;
  buf_block_t *root= btr_root_block_get(index, RW_SX_LATCH, mtr, &err);
  if (UNIV_UNLIKELY(!root))
    return err;

  fseg_header_t *seg_header=
    root->page.frame + PAGE_HEADER + PAGE_BTR_SEG_TOP;
  buf_block_t *iblock;
  fseg_inode_t *inode= fseg_inode_try_get(seg_header, 0, 0, mtr,
                                          &iblock, &err);
  if (!inode)
    return err;

  uint16_t i_offset= uint16_t(inode - iblock->page.frame);

  err= flst_validate(iblock, uint16_t(i_offset + FSEG_FREE), mtr);
  if (err == DB_SUCCESS)
    err= flst_validate(iblock, uint16_t(i_offset + FSEG_NOT_FULL), mtr);
  if (err == DB_SUCCESS)
    err= flst_validate(iblock, uint16_t(i_offset + FSEG_FULL), mtr);

  if (err) return err;

  seg_header= root->page.frame + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
  inode= fseg_inode_try_get(seg_header, 0, 0, mtr, &iblock, &err);
  if (!inode)
    return err;

  i_offset= uint16_t(inode - iblock->page.frame);

  err= flst_validate(iblock, uint16_t(i_offset + FSEG_FREE), mtr);
  if (err == DB_SUCCESS)
    err= flst_validate(iblock, uint16_t(i_offset + FSEG_NOT_FULL), mtr);
  if (err == DB_SUCCESS)
    err= flst_validate(iblock, uint16_t(i_offset + FSEG_FULL), mtr);
  return err;
}

/** Validate the system tablespace list */
__attribute__((warn_unused_result))
static dberr_t fseg_validate(fil_space_t *space,
                             dict_index_t *index) noexcept
{
  /* Validate all FSP list in system tablespace */
  mtr_t mtr;
  mtr.start();
  dberr_t err= fseg_validate_low(space, index, &mtr);
  mtr.commit();
  return err;
}

/** Prepare the associate pages of the current block and modify
the associated pages */
class AssociatedPages final
{
  buf_block_t *m_left_block= nullptr;
  buf_block_t *m_right_block= nullptr;
  buf_block_t *m_parent_block= nullptr;
  buf_block_t *const m_cur_block;
  mtr_t *const m_mtr;

public:
  AssociatedPages(buf_block_t *cur_block, mtr_t *mtr)
    : m_cur_block(cur_block), m_mtr(mtr) {}

  /** Fetch the left, right and parent page for the respective
  current block and make sure that there is no issue exist */
  dberr_t prepare(uint32_t parent_page) noexcept
  {
    uint32_t left_page_no= btr_page_get_prev(m_cur_block->page.frame);
    dberr_t err= DB_SUCCESS;
    if (left_page_no != FIL_NULL)
    {
      m_left_block= fsp_get_latched_page(page_id_t{0, left_page_no},
                                         m_mtr, &err);
      ut_ad(!m_left_block == (err != DB_SUCCESS));
      if (!m_left_block)
        return err;
    }

    uint32_t right_page_no= btr_page_get_next(m_cur_block->page.frame);
    if (right_page_no != FIL_NULL)
    {
      m_right_block= fsp_get_latched_page(page_id_t{0, right_page_no},
                                          m_mtr, &err);
      ut_ad(!m_right_block == (err != DB_SUCCESS));
      if (!m_right_block)
        return err;
    }

    m_parent_block= fsp_get_latched_page(page_id_t{0, parent_page},
                                         m_mtr, &err);
    return err;
  }

  /** Modify the FIL_PAGE_NEXT, FIL_PAGE_PREV, CHILD_PAGE of
  respective left, right and parent block to new page number */
  void complete(uint32_t new_page_no, uint32_t parent_offset) noexcept
  {
    if (m_left_block)
      m_mtr->write<4>(*m_left_block,
                      m_left_block->page.frame + FIL_PAGE_NEXT,
                      new_page_no);

    if (m_right_block)
      m_mtr->write<4>(*m_right_block,
                      m_right_block->page.frame + FIL_PAGE_PREV,
                      new_page_no);

    m_mtr->write<4>(*m_parent_block,
                    m_parent_block->page.frame + parent_offset,
                    new_page_no);
  }
};

/** page operation for the system tablespace does the 2 things:
1) Page Allocation
2) Page removal

Steps for page allocation depends on new extent state.

(1) If the xdes_get_state(new_descr) == XDES_FREE then
remove the new extent from FSP_FREE list

    (1.1) If the page has to be allocated for segment then
          add the newly allocated extent descriptor to
          FSEG_NOT_FULL list and make the xdes_set_state(new_descr)
          as XDES_FSEG

    (1.2) If the page has to be non-segment page then add the
          newly allocated extent descriptor to FSP_FREE_FRAG list
          and make the xdes_set_state(new_descr) as XDES_FREE_FRAG

    (1.3) Allocate a page from the new extent

(2) If the xdes_get_state(new_descr) == XDES_FREE_FRAG then

     (2.1) Allocate a page from the new extent

     (2.2) xdes_get_n_used(new_descr) is FSP_EXTENT_SIZE then
           - Remove the new extent descriptor from FSP_FREE_FRAG list
           - Add the new extent descriptor to FSP_FULL_FRAG list
             and make xdes_set_state(new_descr) as XDES_FULL_FRAG

(3) If the xdes_get_state(new_descr) == XDES_FSEG then

     (3.1) Allocate a page from extent

     (3.2) xdes_get_n_used(new_descr) is FSP_EXTENT_SIZE then
            - Remove the new extent descriptor from FSEG_NOT_FULL list
            - Add the new extent descriptor to FSEG_FULL list


Steps for removing the page from extent:

  (1) To remove the page from extent and number of used
      pages in extent descriptor is FSP_EXTENT_SIZE

     (1a) If the xdes_get_state(m_old_descr) is XDES_FSEG then
          move the extent descriptor from FSEG_FULL to FSEG_NOT_FULL

     (1b) If the xdes_get_stats(m_old_descr) is XDES_FREE_FRAG/XDES_FULL_FRAG
          then move the extent descriptor from FSP_FULL_FRAG to
          FSP_FREE_FRAG list

  (2) If the number of used pages in extent descriptor is 0 then
      move the extent descriptor to FSP_FREE

  (3) Free the page and mark the XDES_FREE_BIT of the respective
      page in current extent descriptor

Above all scenario done by 2 steps to make sure that there
will be no error scenario once the modification of the pages
has started.
1) prepare - Basically validates the necessary condition
and make sure that pages are being latched
2) Complete - Completes the action by using the latched
pages in prepare step */
class PageOperator final
{
  /** Header block for the tablespace */
  buf_block_t *const m_header_block= nullptr;
  /** Index node block */
  buf_block_t *const m_iblock= nullptr;
  /** Index node */
  fseg_inode_t *const m_inode= nullptr;
  /** offset of index node within index node page*/
  uint16_t m_ioffset= 0;
  /** Maximum free limit of the tablespace */
  uint32_t m_free_limit= 0;
  /** Segment id */
  uint64_t m_seg_id= 0;
  /** Extent size */
  uint32_t m_extent_size= 0;

  /** New block to be allocated */
  buf_block_t *m_new_block= nullptr;
  /** New block extent descriptor */
  buf_block_t *m_new_xdes= nullptr;
  /** New block descriptor */
  xdes_t *m_new_descr= nullptr;
  /** New block descriptor offset within xdes page */
  uint16_t m_xoffset= 0;
  /** New extent descriptor state */
  uint32_t m_new_state= 0;
  /** Need segment allocation */
  bool m_need_segment= false;
  /** Old pages during allocation to be saved */
  buf_block_t *m_old_pages[8]= {nullptr};
  /** Page to be removed */
  byte m_old_page_no[4]= {0};
  /** Old block extent descriptor page */
  buf_block_t *m_old_xdes= nullptr;
  /** Old block descriptor */
  xdes_t *m_old_descr= nullptr;
  /** Old block descriptor offset with descriptor page */
  uint16_t m_old_xoffset= 0;
  /** Old descriptor state */
  uint32_t m_old_state= 0;
  /** Mini-transaction to allocate & free a page */
  mtr_t *const m_mtr;

  /** Save the old page state of the block before
  allocating a page
  @param block   block to be stored
  @return error code */
  dberr_t save_old_page(buf_block_t *block) noexcept
  {
    if (!block) return DB_SUCCESS;
    size_t first_free;
    for (first_free= 0; first_free < array_elements(m_old_pages); first_free++)
    {
      const buf_block_t *b= m_old_pages[first_free];
      if (!b)
        goto found;
      if (b->page.hash == &block->page)
        return DB_SUCCESS;
    }
    return DB_CORRUPTION;
found:
    buf_block_t *old= buf_LRU_get_free_block(have_no_mutex_soft);
    if (!old) return DB_OUT_OF_MEMORY;
    memcpy_aligned<UNIV_PAGE_SIZE_MIN>(
      old->page.frame, block->page.frame, srv_page_size);
    m_old_pages[first_free]= old;
    old->page.hash= &block->page;
    return DB_SUCCESS;
  }

  /** Prepare the steps for free extent allocation by validating
  FLST_PREV, FLST_NEXT of choosen extent descriptor
  and their FLST_LEN of FSP_FREE list in FSP_HEADER_PAGE.
  @return error code or DB_SUCCESS */
  dberr_t free_extent_prepare() noexcept
  {
    /* At least there should be 1 element in FSP_FREE list */
    byte *len=
      &m_header_block->page.frame[FSP_HEADER_OFFSET + FSP_FREE +
                                  FLST_LEN];
    if (mach_read_from_4(len) == 0)
      return DB_CORRUPTION;

    buf_block_t *fsp_free_prev= nullptr;
    buf_block_t *fsp_free_next= nullptr;

    dberr_t err= flst::remove_prepare(*m_new_xdes, m_xoffset,
                                      m_free_limit, m_mtr,
                                      &fsp_free_prev, &fsp_free_next);
    if (err == DB_SUCCESS)
    {
      err= save_old_page(fsp_free_prev);
      if (err == DB_SUCCESS)
        err= save_old_page(fsp_free_next);
    }
    return err;
  }

  /** Complete the free extent allocation */
  void free_extent_complete() noexcept
  {
    flst::remove_complete(m_header_block, FSP_HEADER_OFFSET + FSP_FREE,
                          m_new_descr, m_mtr);
    fil_system.sys_space->free_len--;
  }

  /** Prepare the steps to do the following
  1) free extent allocation
  2) Add the extent to FSEG_NOT_FULL list by validating the
  last extent descriptor in FSEG_NOT_FULL list of segment inode
  @return error code */
  dberr_t initialize_segment_prepare() noexcept
  {
    dberr_t err= free_extent_prepare();
    if (err) return err;

    buf_block_t *fseg_not_full_last= nullptr;
    err= flst::append_prepare(*m_iblock,
                              uint16_t(m_ioffset + FSEG_NOT_FULL),
                              m_free_limit, m_mtr, &fseg_not_full_last);
    if (err == DB_SUCCESS)
      err= save_old_page(fseg_not_full_last);
    return err;
  }

  /** This function does the following
  1) Allocating the free extent
  2) Appending the extent to FSEG_NOT_FULL list in segment inode
  3) Mark the extent state as XDES_FSEG */
  void initialize_segment_complete() noexcept
  {
    free_extent_complete();
    flst::append_complete(m_iblock,
                          uint16_t(m_ioffset + FSEG_NOT_FULL),
                          m_new_xdes, m_xoffset, m_mtr);

    /* Update the FSEG_NOT_FULL_N_USED in inode */
    byte *p_not_full= m_inode + FSEG_NOT_FULL_N_USED;
    m_mtr->write<4>(*m_iblock, p_not_full,
                    mach_read_from_4(p_not_full) + 1);
    xdes_set_state(*m_new_xdes, m_new_descr, XDES_FSEG, m_mtr);
    m_mtr->write<8,mtr_t::MAYBE_NOP>(*m_new_xdes,
                                     m_new_descr + XDES_ID,
                                     m_seg_id);
    xdes_set_free<false>(*m_new_xdes, m_new_descr,
                         m_new_block->page.id().page_no() % m_extent_size,
                         m_mtr);
  }

  /** Prepare the steps for
  1) Allocating the free extent
  2) Adding the extent to FSP_FREE_FRAG list by validating
  the last extent descriptor in FSP_FREE_FRAG list of FSP_HEADER page
  @return error code */
  dberr_t initialize_free_frag_prepare() noexcept
  {
    dberr_t err= free_extent_prepare();
    if (err) return err;

    buf_block_t *fsp_free_frag_last= nullptr;
    err= flst::append_prepare(*m_header_block,
                              FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                              m_free_limit, m_mtr, &fsp_free_frag_last);

    if (err == DB_SUCCESS)
      err= save_old_page(fsp_free_frag_last);
    return err;
  }

  /** This function does the following
  1) Allocating the free extent
  2) Appending the extent to FSP_FREE_FRAG list in FSP_HEADER page
  3) Mark the extent state as XDES_FREE_FRAG */
  void initialize_free_frag_complete() noexcept
  {
    free_extent_complete();
    flst::append_complete(m_header_block,
                          FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                          m_new_xdes, m_xoffset, m_mtr);

    byte *n_frag_used= m_header_block->page.frame
                         + FSP_HEADER_OFFSET + FSP_FRAG_N_USED;
    m_mtr->write<4>(*m_header_block, n_frag_used,
                    mach_read_from_4(n_frag_used) + 1);

    /* Allocate the extent state to FREE_FRAG & update FSP_FRAG_N_USED */
    xdes_set_state(*m_new_xdes, m_new_descr, XDES_FREE_FRAG, m_mtr);
    xdes_set_free<false>(*m_new_xdes, m_new_descr,
                         m_new_block->page.id().page_no() % m_extent_size,
                         m_mtr);
  }

  /** Prepare the steps to
  1) Allocate a page from XDES_FSEG extent
  2) If the extent size is FSP_EXTENT_SIZE then
  prepare the extent to move from FSEG_NOT_FULL to FSEG_FULL
  list in segment inode by validating the last extent descriptor in
  FSEG_FULL list and previous and next extent in FSEG_NOT_FULL list.
  @return error code */
  dberr_t alloc_from_fseg_prepare() noexcept
  {
    uint32_t n_used= xdes_get_n_used(m_new_descr);
    if (n_used < 1 || n_used >= m_extent_size)
      return DB_CORRUPTION;

    if (n_used < m_extent_size)
      return DB_SUCCESS;

    byte *lst= m_iblock->page.frame + uint16_t(m_ioffset + FSEG_NOT_FULL);
    if (!mach_read_from_4(lst + FLST_LEN))
      return DB_CORRUPTION;

    buf_block_t *fseg_not_full_prev= nullptr;
    buf_block_t *fseg_not_full_next= nullptr;
    dberr_t err= flst::remove_prepare(*m_new_xdes, m_xoffset,
                                      m_free_limit, m_mtr,
                                      &fseg_not_full_prev,
                                      &fseg_not_full_next);
    if (err) return err;

    buf_block_t *fseg_full_last= nullptr;
    err= flst::append_prepare(*m_iblock,
                              uint16_t(m_ioffset + FSEG_FULL),
                              m_free_limit, m_mtr, &fseg_full_last);
    if (err == DB_SUCCESS)
    {
      err= save_old_page(fseg_not_full_prev);
      if (err == DB_SUCCESS)
        err= save_old_page(fseg_not_full_next);
      if (err == DB_SUCCESS)
        err= save_old_page(fseg_full_last);
    }
    return err;
  }

  /** Does the following
  1) Complete the page allocation from file segment.
  2) If the extent size is FSP_EXTENT_SIZE then
      i) Remove the extent from FSEG_NOT_FULL list
      ii) Add the extent to FSEG_FULL */
  void alloc_from_fseg_complete() noexcept
  {
    xdes_set_free<false>(*m_new_xdes, m_new_descr,
                         m_new_block->page.id().page_no() % m_extent_size,
                         m_mtr);

    byte *p_not_full= m_inode + FSEG_NOT_FULL_N_USED;
    uint32_t n_used_val= mach_read_from_4(p_not_full) + 1;

    if (xdes_get_n_used(m_new_descr) == m_extent_size)
    {
      n_used_val-= FSP_EXTENT_SIZE;
      m_mtr->write<4>(*m_iblock, p_not_full, n_used_val);
      flst::remove_complete(m_iblock,
                            uint16_t(m_ioffset + FSEG_NOT_FULL),
                            m_new_descr, m_mtr);
      flst::append_complete(m_iblock,
                            uint16_t(m_ioffset + FSEG_FULL),
                            m_new_xdes, m_xoffset, m_mtr);
    }
    else
      m_mtr->write<4>(*m_iblock, p_not_full, n_used_val);
  }

  /** Prepare the steps to
  1) Allocate the page from free fragment extent.
  2) If the extent size is FSP_EXTENT_SIZE then prepare the
  steps to move the extent from FSP_FREE_FRAG to FSP_FULL_FRAG
  list by validating the next, previous extent descriptor of
  current extent descriptor in FSP_FREE_FRAG list and
  last extent descriptor in FSP_FULL_FRAG list
  @return error code */
  dberr_t alloc_from_free_frag_prepare() noexcept
  {
    uint32_t n_used= xdes_get_n_used(m_new_descr);
    if (n_used < 1 || n_used >= m_extent_size)
      return DB_CORRUPTION;

    if (n_used < m_extent_size)
      return DB_SUCCESS;

    byte *lst= m_header_block->page.frame + FSP_HEADER_OFFSET + FSP_FREE_FRAG;
    if (!mach_read_from_4(lst + FLST_LEN))
      return DB_CORRUPTION;

    buf_block_t *fsp_free_frag_prev= nullptr;
    buf_block_t *fsp_free_frag_next= nullptr;
    dberr_t err= flst::remove_prepare(*m_new_xdes, m_xoffset,
                                      m_free_limit, m_mtr,
                                      &fsp_free_frag_prev,
                                      &fsp_free_frag_next);
    if (err) return err;

    buf_block_t *fsp_full_frag_last= nullptr;
    err= flst::append_prepare(*m_header_block,
                              FSP_HEADER_OFFSET + FSP_FULL_FRAG,
                              m_free_limit, m_mtr,
                              &fsp_full_frag_last);

    if (err == DB_SUCCESS)
    {
      err= save_old_page(fsp_free_frag_prev);
      if (err == DB_SUCCESS)
        err= save_old_page(fsp_free_frag_next);
      if (err == DB_SUCCESS)
        err= save_old_page(fsp_full_frag_last);
    }
    return err;
  }

  /** Does the following
  1) Allocate the page from fragment extent
  2) If the extent size is FSP_EXTENT_SIZE then
     i) remove the extent descriptor from FSP_FREE_FRAG list
     ii) Add the extent descriptor in FSP_FULL_FRAG list */
  void alloc_from_free_frag_complete() noexcept
  {
    xdes_set_free<false>(*m_new_xdes, m_new_descr,
                         m_new_block->page.id().page_no() % m_extent_size,
                         m_mtr);

    byte *frag_n_used= m_header_block->page.frame + FSP_HEADER_OFFSET
      + FSP_FRAG_N_USED;
    uint32_t n_used_frag= mach_read_from_4(frag_n_used) + 1;

    if (xdes_get_n_used(m_new_descr) == m_extent_size)
    {
      n_used_frag-= FSP_EXTENT_SIZE;
      m_mtr->write<4>(*m_header_block, frag_n_used, n_used_frag);
      flst::remove_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                            m_new_descr, m_mtr);

      flst::append_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FULL_FRAG,
                            m_new_xdes, m_xoffset, m_mtr);
    }
    else
      m_mtr->write<4>(*m_header_block, frag_n_used, n_used_frag);
  }

  /** Prepare the steps to free the page from fragment pages.
  1) Check the page exist in segment fragment array
  2) If the extent descriptor is in XDES_FULL_FRAG then
  prepare the steps to move the extent descriptor
  from FSP_FULL_FRAG to FSP_FREE_FRAG list by validating
  the FLST_PREV, FLST_NEXT of current extent descriptor
  and FLST_LAST in FSP_FREE_FRAG list
  3) If the extent is about to empty then prepare the steps
  to move the extent descriptor from FSP_FREE_FRAG to FSP_FREE list
  by validating the FLST_PREV, FLST_NEXT of current extent
  descriptor and FLST_LAST in FSP_FREE list
  @return error code */
  dberr_t free_from_frag_prepare() noexcept
  {
    uint32_t n_arr_slots= m_extent_size / 2;
    bool page_exist= false;
    for (ulint i= 0; i < n_arr_slots; i++)
    {
      if (!memcmp(m_inode + FSEG_FRAG_ARR + i * FSEG_FRAG_SLOT_SIZE,
                  m_old_page_no, 4))
      {
        page_exist= true;
        break;
      }
    }

    if (!page_exist) return DB_CORRUPTION;

    buf_block_t *fsp_full_frag_prev= nullptr;
    buf_block_t *fsp_full_frag_next= nullptr;
    buf_block_t *fsp_free_frag_last= nullptr;
    dberr_t err= DB_SUCCESS;
    uint32_t n_used= xdes_get_n_used(m_old_descr);

    if (m_old_state == XDES_FULL_FRAG)
    {
      if (n_used != m_extent_size)
        return DB_CORRUPTION;

      byte *lst= m_header_block->page.frame + FSP_HEADER_OFFSET + FSP_FULL_FRAG;
      if (!mach_read_from_4(lst + FLST_LEN))
        return DB_CORRUPTION;

      err= flst::remove_prepare(*m_old_xdes, m_old_xoffset, m_free_limit,
                                m_mtr, &fsp_full_frag_prev,
                                &fsp_full_frag_next);

      if (err) return err;

      return flst::append_prepare(*m_header_block,
                                  FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                                  m_free_limit, m_mtr,
                                  &fsp_free_frag_last);
    }

    if (n_used >= m_extent_size || n_used == 0)
      return DB_CORRUPTION;

    buf_block_t *fsp_free_frag_prev= nullptr;
    buf_block_t *fsp_free_frag_next= nullptr;
    buf_block_t *fsp_free_last= nullptr;

    if (n_used == 1)
    {
      byte *lst= m_header_block->page.frame + FSP_HEADER_OFFSET + FSP_FREE_FRAG;
      if (!mach_read_from_4(lst + FLST_LEN))
        return DB_CORRUPTION;

      err= flst::remove_prepare(*m_old_xdes, m_old_xoffset, m_free_limit,
                                m_mtr, &fsp_free_frag_prev,
                                &fsp_free_frag_next);
      if (err) return err;

      return flst::append_prepare(*m_header_block,
                                  FSP_HEADER_OFFSET + FSP_FREE,
                                  m_free_limit, m_mtr,
                                  &fsp_free_last);
    }
    return err;
  }

  /** Complete the removal of page from XDES_FREE_FRAG
  (or) XDES_FULL_FRAG list.
  1) If the extent is from FSP_FULL_FRAG then move the
  extent descriptor from FSP_FULL_FRAG to FSP_FREE_FRAG
  2) If the extent is from FSP_FREE_FRAG and no pages
  has been used in that descr then move the extent
  from FSP_FREE_FRAG to FSP_FREE */
  void free_from_frag_complete() noexcept
  {
    uint32_t old_page_no= mach_read_from_4(m_old_page_no);
    m_mtr->free(*fil_system.sys_space, old_page_no);
    xdes_set_free<true>(*m_old_xdes, m_old_descr,
                        old_page_no % m_extent_size, m_mtr);
    uint32_t n_used= xdes_get_n_used(m_old_descr);
    byte *frag_n_used= m_header_block->page.frame + FSP_HEADER_OFFSET
                         + FSP_FRAG_N_USED;
    uint32_t n_frag_used= mach_read_from_4(frag_n_used) - 1;

    for (size_t i= 0, frag= m_ioffset + FSEG_FRAG_ARR;
         i < m_extent_size / 2; i++, frag += FSEG_FRAG_SLOT_SIZE)
    {
      if (!memcmp(m_iblock->page.frame + frag, m_old_page_no, 4))
      {
        m_mtr->memset(m_iblock, frag, 4, 0xff);
        break;
      }
    }

    if (n_used == m_extent_size - 1)
    {
      flst::remove_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FULL_FRAG,
                            m_old_descr, m_mtr);

      xdes_set_state(*m_old_xdes, m_old_descr, XDES_FREE_FRAG, m_mtr);

      flst::append_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                            m_old_xdes, m_old_xoffset, m_mtr);

      n_frag_used += m_extent_size;
    }
    else if (n_used == 0)
    {
      flst::remove_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FREE_FRAG,
                            m_old_descr, m_mtr);

      xdes_set_state(*m_old_xdes, m_old_descr, XDES_FREE, m_mtr);

      flst::append_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FREE,
                            m_old_xdes, m_old_xoffset, m_mtr);
    }
    m_mtr->write<4>(*m_header_block, frag_n_used, n_frag_used);
  }

  /** Prepare the removal of page from file segment
  1) If the number of used pages in extent descriptor is
     FSP_EXTENT_SIZE then move the extent descriptor from
     FSEG_FULL to FSEG_NOT_FULL list by validating the
     FLST_PREV, FLST_NEXT of current extent descriptor
     and last extent descriptor in FSEG_NOT_FULL list
  2) If the number of used pages in extent descriptor is 0
     then move the extent descriptor from FSEG_NOT_FULL to
     FSP_FREE list by validating the FLST_PREV, FLST_NEXT
     of current extent descriptor and last extent descriptor
     in FSP_FREE list
  @return error code */
  dberr_t free_from_fseg_prepare() noexcept
  {
    if (memcmp(m_old_descr, m_inode + FSEG_ID, 8))
      return DB_CORRUPTION;

    uint32_t n_used= xdes_get_n_used(m_old_descr);
    if (n_used == 0 || n_used > m_extent_size)
      return DB_CORRUPTION;

    buf_block_t *fseg_full_prev= nullptr;
    buf_block_t *fseg_full_next= nullptr;
    buf_block_t *fseg_not_full_last= nullptr;
    buf_block_t *fseg_not_full_prev= nullptr;
    buf_block_t *fseg_not_full_next= nullptr;
    buf_block_t *fsp_free_last= nullptr;

    dberr_t err= DB_SUCCESS;

    if (n_used == m_extent_size)
    {
      byte *lst= m_iblock->page.frame + uint16_t(m_ioffset + FSEG_FULL);
      if (!mach_read_from_4(lst + FLST_LEN))
        return DB_CORRUPTION;

      err= flst::remove_prepare(*m_old_xdes, m_old_xoffset, m_free_limit,
                                m_mtr, &fseg_full_prev,
                                &fseg_full_next);
      if (err) return err;

      err= flst::append_prepare(*m_iblock,
                                uint16_t(FSEG_NOT_FULL + m_ioffset),
                                m_free_limit, m_mtr,
                                &fseg_not_full_last);
      if (err) return err;
    }
    else
    {
      uint32_t not_full_n_used=
        mach_read_from_4(m_inode + FSEG_NOT_FULL_N_USED);
      if (!not_full_n_used) return DB_CORRUPTION;
    }

    if (n_used == 1)
    {
      byte *lst= m_iblock->page.frame + uint16_t(m_ioffset + FSEG_NOT_FULL);
      if (!mach_read_from_4(lst + FLST_LEN))
        return DB_CORRUPTION;

      err= flst::remove_prepare(*m_old_xdes, m_old_xoffset, m_free_limit,
                                m_mtr, &fseg_not_full_prev,
                                &fseg_not_full_next);
      if (err) return err;

      err= flst::append_prepare(*m_header_block,
                                FSP_FREE + FSP_HEADER_OFFSET,
                                m_free_limit, m_mtr, &fsp_free_last);
    }
    return err;
  }

  /** Complete the removal of page from file segment
  1) If the extent is from FSEG_FULL then move the
  extent descriptor from FSEG_FULL to FSEG_NOT_FULL
  2) If the extent is from FSEG_NOT_FULL then move the
  extent descriptor to FSP_FREE */
  void free_from_fseg_complete() noexcept
  {
    uint32_t n_used= xdes_get_n_used(m_old_descr);
    uint32_t old_page_no= mach_read_from_4(m_old_page_no);
    m_mtr->free(*fil_system.sys_space, old_page_no);
    xdes_set_free<true>(*m_old_xdes, m_old_descr,
                        old_page_no % m_extent_size, m_mtr);

    byte* p_not_full = m_inode + FSEG_NOT_FULL_N_USED;
    uint32_t not_full_n_used = mach_read_from_4(p_not_full) - 1;
    if (n_used == m_extent_size)
    {
      flst::remove_complete(m_iblock, uint16_t(m_ioffset + FSEG_FULL),
                            m_old_descr, m_mtr);
      flst::append_complete(m_iblock,
                            uint16_t(m_ioffset + FSEG_NOT_FULL),
                            m_old_xdes, m_old_xoffset, m_mtr);
      not_full_n_used += m_extent_size;
    }
    m_mtr->write<4>(*m_iblock, p_not_full, not_full_n_used);

    if (n_used == 1)
    {
      flst::remove_complete(m_iblock,
                            uint16_t(m_ioffset + FSEG_NOT_FULL),
                            m_old_descr, m_mtr);

      xdes_set_state(*m_old_xdes, m_old_descr, XDES_FREE, m_mtr);
      flst::append_complete(m_header_block,
                            FSP_HEADER_OFFSET + FSP_FREE,
                            m_old_xdes, m_old_xoffset, m_mtr);
      fil_system.sys_space->free_len++;
    }
  }
public:
  PageOperator(buf_block_t *header_block, buf_block_t *iblock,
               fseg_inode_t *inode,
               uint32_t extent_size, byte* old_page_no,
               mtr_t *mtr) :
  m_header_block(header_block),
  m_iblock(iblock), m_inode(inode), m_extent_size(extent_size),
  m_mtr(mtr)
  {
    if (old_page_no)
      memcpy(m_old_page_no, old_page_no, 4);
    m_free_limit= mach_read_from_4(FSP_HEADER_OFFSET + FSP_FREE_LIMIT +
                                   m_header_block->page.frame);
    m_seg_id= mach_read_from_8(m_inode + FSEG_ID);
  }

  ~PageOperator()
  {
    for (buf_block_t *old : m_old_pages)
      if (old)
      {
        old->page.hash= nullptr;
        buf_block_free(old);
      }
  }


  /** Get allocated new block */
  buf_block_t* get_new_block() const noexcept { return m_new_block; }

  /** Prepare the new page allocation from the new given extent
  @param new_extent starting page of new extent
  @param segment    segment allocation
  @return error code */
  dberr_t prepare_new_page(uint32_t new_extent, bool segment) noexcept
  {
    dberr_t err= DB_SUCCESS;
    uint32_t size= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE
                                    + m_header_block->page.frame);
    if (new_extent >= size || new_extent >= m_free_limit)
      return DB_CORRUPTION;

    uint32_t new_descr_page_no= xdes_calc_descriptor_page(0, new_extent);
    m_new_xdes= fsp_get_latched_page(page_id_t{0, new_descr_page_no},
                                     m_mtr, &err);
    if (!m_new_xdes)
      return err;

    ut_ad(!m_new_block);
    m_ioffset= uint16_t(m_inode - m_iblock->page.frame);
    m_need_segment= segment;
    m_xoffset= uint16_t(xdes_calc_descriptor_index(0, new_extent) * XDES_SIZE
                          + XDES_ARR_OFFSET + XDES_FLST_NODE);
    m_new_descr= m_new_xdes->page.frame + m_xoffset - XDES_FLST_NODE;
    m_new_state= uint32_t(xdes_get_state(m_new_descr));
    uint32_t new_page= 0;

    /* Allocate the new extent and initialize the extent state
    with XDES_FSEG/XDES_FREE_FRAG */
    if (m_new_state == XDES_FREE)
    {
      if (segment) err= initialize_segment_prepare();
      else err= initialize_free_frag_prepare();

      if (err) return err;
new_page:
      new_page= xdes_find_free(m_new_descr);
      if (new_page == FIL_NULL)
        return DB_CORRUPTION;

      new_page+= new_extent;
      m_new_block= fsp_page_create(fil_system.sys_space, new_page, m_mtr);
      err= save_old_page(m_header_block);
      if (err == DB_SUCCESS)
        err= save_old_page(m_iblock);
      if (err == DB_SUCCESS)
        err= save_old_page(m_new_xdes);
      if (err == DB_SUCCESS)
        err= save_old_page(m_new_block);
      return err;
    }

    uint32_t  n_used= xdes_get_n_used(m_new_descr);
    if (n_used == 0 || n_used >= m_extent_size)
      return DB_CORRUPTION;

    /* Allocate the page from file segment */
    if (m_seg_id != FIL_NULL && m_new_state == XDES_FSEG &&
        mach_read_from_8(m_new_descr + XDES_ID) == m_seg_id)
      err= alloc_from_fseg_prepare();
    /* Allocate the page from free frag */
    else if (m_new_state == XDES_FREE_FRAG || m_new_state == XDES_FULL_FRAG)
      err= alloc_from_free_frag_prepare();
    else return DB_CORRUPTION;

    if (err) return err;
    goto new_page;
  }

  /** Complete the page allocation from FREE extent descriptor
  or XDES_FSEG/XDES_FREE_FRAG extent list */
  void complete_new_page() noexcept
  {
    if (m_new_state == XDES_FREE)
    {
      if (m_need_segment)
        return initialize_segment_complete();
      return initialize_free_frag_complete();
    }
    if (m_new_state == XDES_FSEG)
      return alloc_from_fseg_complete();
    return alloc_from_free_frag_complete();
  }

  /** Assign the fragment slot of the index node.
  This step should be done after removing the old page
  because there is a possiblity that FRAGMENT ARRAY
  could be full. */
  void assign_frag_slot() noexcept
  {
    if ((!m_need_segment && m_new_state == XDES_FREE) ||
        m_new_state == XDES_FULL_FRAG ||
        m_new_state == XDES_FREE_FRAG)
      fseg_set_nth_frag_page_no(m_inode, m_iblock,
                                fseg_find_free_frag_page_slot(m_inode),
                                m_new_block->page.id().page_no(), m_mtr);
  }

  /** Restore the page modified during page allocation */
  void restore_old_pages() noexcept
  {
    for (buf_block_t *old : m_old_pages)
      if (old)
        memcpy_aligned<UNIV_PAGE_SIZE_MIN>(
          old->page.hash->frame, old->page.frame, srv_page_size);
  }

  /** Prepare the steps to remove the page from file segment
  (or) fragment extent.
  @return error code */
  dberr_t prepare_old_page() noexcept
  {
    uint32_t old_page_no= mach_read_from_4(m_old_page_no);
    uint32_t old_descr_page_no=
      xdes_calc_descriptor_page(0, old_page_no);
    dberr_t err= DB_SUCCESS;
    m_old_xdes= fsp_get_latched_page(page_id_t{0, old_descr_page_no},
                                     m_mtr, &err);
    if (!m_old_xdes)
      return err;

    m_old_xoffset=
      uint16_t(xdes_calc_descriptor_index(0, old_page_no) * XDES_SIZE
                 + XDES_ARR_OFFSET + XDES_FLST_NODE);

    m_old_descr= m_old_xdes->page.frame + m_old_xoffset - XDES_FLST_NODE;
    m_old_state= uint32_t(xdes_get_state(m_old_descr));
    if (m_old_state == XDES_FREE)
      return DB_CORRUPTION;

    if (xdes_is_free(m_old_descr, old_page_no & (m_extent_size -1)))
      return DB_CORRUPTION;

    m_ioffset= uint16_t(m_inode - m_iblock->page.frame);
    return m_old_state == XDES_FSEG
      ? free_from_fseg_prepare()
      : free_from_frag_prepare();
  }

  /** Complete the removal of page operation */
  void complete_free_old_page() noexcept
  {
    return m_old_state == XDES_FSEG
      ? free_from_fseg_complete()
      : free_from_frag_complete();
  }
};


class IndexDefragmenter final
{
  /** Parent block and its associate offset where
  we store the child page number. This is stored
  in the form of <child_page_no, parent_page_no + parent_offset> */
  std::unordered_map<uint32_t, uint64_t> m_parent_pages;

  dict_index_t &m_index;

  buf_block_t *m_root;
  /** Iterate through the page and map the child_page_no
  with the parent page and their associate offset
  in m_parent_pages
  @param block  block to be traversed */
  dberr_t get_child_pages(buf_block_t *block) noexcept;

  /** Get the first block for the given level
  @param level       level
  @param mtr         mini-transaction
  @param cur_page_no first page number for the given level
  @return error code or DB_SUCCESS */
  dberr_t get_level_block(uint16_t level, mtr_t *mtr,
                          uint32_t *cur_page_no) noexcept;

  /** Defragment the level of the index
  @param level          level to be defragmented
  @param mtr            mini-transaction
  @param space_defrag   space defragmenter information
                        and also responsible for allocating new
                        segment or page from tablespace
  @return error code or DB_SUCCESS */
  dberr_t defragment_level(uint16_t level, mtr_t *mtr,
                           SpaceDefragmenter *space_defrag) noexcept;

public:
  IndexDefragmenter(dict_index_t &index): m_index(index) {}

  /** Defragment the index with the help of space defragmenter.
  1) Iterate through each level of the index
  2) Find out what are the pages/segment
  to be modified for the index.
  3) Allocate the page from the new segment/extent
  4) Copy the to be changed page content to new page
  5) Change the associative pages in the tree with
  new page(left, right, parent block)
  6) Do step (4), (5) within single mini-transaction
  and commit the mini-transaction
  @return error code or DB_SUCCESS */
  dberr_t defragment(SpaceDefragmenter *space_defrag) noexcept;
};

class SpaceDefragmenter final
{
  /** Extent is already allocated for defragmentation */
  static constexpr uint32_t XDES_USED= ~0U;
  /** Store the extent information in the tablespace <extent, state>*/
  std::map<uint32_t, uint32_t> m_extent_info;
  /** Map of last used extent with early unused extent within
  the tablespace */
  std::map<uint32_t, uint32_t> m_extent_map;

  /** Collect the extent information from tablespace */
  dberr_t extract_extent_state() noexcept
  {
    mtr_t mtr;
    dberr_t err= DB_SUCCESS;
    uint32_t last_descr_page_no= 0;
    fil_space_t *space= fil_system.sys_space;
    mtr.start();
    mtr.x_lock_space(space);
    buf_block_t *last_descr= buf_page_get_gen(page_id_t{space->id, 0}, 0,
                                              RW_S_LATCH, nullptr,
                                              BUF_GET_POSSIBLY_FREED, &mtr,
                                              &err);
    if (!last_descr)
    {
func_exit:
      mtr.commit();
      return err;
    }

    for (uint32_t xdes_n= 0; xdes_n < space->free_limit;
         xdes_n+= m_extent_size)
    {
      /* Ignore doublewrite buffer extent */
      if (buf_dblwr.is_inside(xdes_n))
        continue;
      uint32_t descr_page_no=
        xdes_calc_descriptor_page(space->id, xdes_n);
      if (descr_page_no != last_descr_page_no)
      {
        last_descr= buf_page_get_gen(page_id_t{space->id, xdes_n},
                                     0, RW_S_LATCH, nullptr,
                                     BUF_GET_POSSIBLY_FREED, &mtr,
                                     &err);
        if (!last_descr)
          goto func_exit;
      }
      xdes_t *descr= XDES_ARR_OFFSET + XDES_SIZE *
        xdes_calc_descriptor_index(0, xdes_n) + last_descr->page.frame;
      last_descr_page_no= descr_page_no;
      /* Ignore the extent descriptor extent */
      if (xdes_n % srv_page_size == 0 && xdes_get_n_used(descr) == 2)
        continue;
      m_extent_info[xdes_n]= xdes_get_state(descr);
    }
    goto func_exit;
  }

  /** Find the earlier free extent for the given used extent
  @param max_limit Find the extent below max limit extent
  @return value
  @retval FIL_NULL if there is no extent */
  uint32_t find_free_extent(uint32_t max_limit) noexcept
  {
    for (auto &extent_info : m_extent_info)
    {
      if (max_limit <= extent_info.first)
        return FIL_NULL;

      if (extent_info.second == XDES_FREE)
      {
        /* Mark the extent as used one */
        extent_info.second = XDES_USED;
        return extent_info.first;
      }
    }
    return FIL_NULL;
  }

  /** Defragment the indexes */
  dberr_t defragment_index(dict_index_t &index) noexcept
  {
    IndexDefragmenter index_defrag(index);
    return index_defrag.defragment(this);
  }

  /** Defragment the table */
  dberr_t defragment_table(const dict_table_t *table) noexcept
  {
    for (dict_index_t *index= dict_table_get_first_index(table);
         index; index= dict_table_get_next_index(index))
    {
      dberr_t err= fseg_validate(fil_system.sys_space, index);
      if (err == DB_SUCCESS)
        err= defragment_index(*index);

      if (err)
      {
        sql_print_error("InnoDB: Defragmentation of %s in %s failed: %s",
                        index->name, table->name.m_name, ut_strerr(err));
        return err;
      }
    }
    return DB_SUCCESS;
  }
public:
  const uint32_t m_extent_size;

  SpaceDefragmenter() noexcept : m_extent_size(FSP_EXTENT_SIZE) {}

  /** Find the new extent for the existing last used extent
  Iterate the tablespace from last and find out the free
  extent in the beginning of the tablespace */
  dberr_t find_new_extents() noexcept
  {
    dberr_t err= extract_extent_state();
    if (err) return err;

    uint32_t free_limit= fil_system.sys_space->free_limit;
    uint32_t fixed_size= srv_sys_space.get_min_size();
    while (free_limit > fixed_size)
    {
      uint32_t state= m_extent_info[free_limit];

      switch (state) {
      case XDES_USED:
        goto func_exit;
      case XDES_FREE:
        goto prev_extent;
      case XDES_FSEG:
      case XDES_FULL_FRAG:
      case XDES_FREE_FRAG:
        uint32_t dest= find_free_extent(free_limit);
        if (dest == FIL_NULL)
          goto func_exit;
        m_extent_map[free_limit]= dest;
        break;
      }
prev_extent:
      free_limit-= FSP_EXTENT_SIZE;
    }
func_exit:
    if (m_extent_map.empty())
      return DB_SUCCESS_LOCKED_REC;

    sql_print_information("InnoDB: System tablespace defragmentation "
                          "process starts");
    sql_print_information("InnoDB: Moving the data from extents %"
                          PRIu32 " through %" PRIu32,
                          m_extent_map.begin()->first,
                          m_extent_map.rbegin()->first);
    return DB_SUCCESS;
  }

  /** Defragment the system tables */
  dberr_t defragment_system_tables() noexcept
  {
    dberr_t err= defragment_table(dict_sys.sys_tables);
    if (err == DB_SUCCESS)
      err= defragment_table(dict_sys.sys_columns);
    if (err == DB_SUCCESS)
      err= defragment_table(dict_sys.sys_indexes);
    if (err == DB_SUCCESS)
      err= defragment_table(dict_sys.sys_fields);
    if (err == DB_SUCCESS)
      err= defragment_table(dict_sys.sys_foreign);
    if (err == DB_SUCCESS)
      err= defragment_table(dict_sys.sys_foreign_cols);
    if (err == DB_SUCCESS)
      err= defragment_table(dict_sys.sys_virtual);

    if (err == DB_SUCCESS)
      sql_print_information("InnoDB: Defragmentation of system "
                            "tablespace is successful");
    return err;
  }

  /** @return extent which replaces the later extent
  or same extent if there is no replacement exist */
  uint32_t get_new_extent(uint32_t old_extent) const noexcept
  {
    auto it= m_extent_map.find(old_extent);
    if (it != m_extent_map.end())
      return it->second;
    return old_extent;
  }

  /** @return state for the given extent */
  uint32_t get_state(uint32_t extent) noexcept
  {
    return m_extent_info[extent];
  }
};

dberr_t IndexDefragmenter::get_child_pages(buf_block_t *block) noexcept
{
  const byte *page= block->page.frame;
  const rec_t *rec= page_rec_get_next_low(page + PAGE_OLD_INFIMUM, false);
  while (rec != page + PAGE_OLD_SUPREMUM)
  {
    ulint len;
    ulint offset= rec_get_nth_field_offs_old(rec,
                                             rec_get_n_fields_old(rec) - 1,
                                             &len);
    if (len != 4)
      return DB_CORRUPTION;

    if (offset >= srv_page_size)
      return DB_CORRUPTION;

    const byte *field= rec + offset;
    /* m_parent_pages[child_page_no] =
       1st 32 bit to indicate offset in parent page
       2nd 32 bit to indicate parent page number */
    m_parent_pages[mach_read_from_4(field)]=
      uint64_t(page_offset(field)) << 32 | block->page.id().page_no();
    rec= page_rec_get_next_low(rec, false);
  }
  return DB_SUCCESS;
}

dberr_t IndexDefragmenter::get_level_block(uint16_t level, mtr_t *mtr,
                                           uint32_t *cur_page_no) noexcept
{
  uint32_t child_page_no= m_index.page;
  dberr_t err= DB_SUCCESS;
  uint16_t prev_level= UINT16_MAX;
  while (1)
  {
    buf_block_t *block= fsp_get_latched_page(page_id_t{0, child_page_no},
                                             mtr, &err);
    if (!block)
      return err;

    page_t *page= buf_block_get_frame(block);
    uint16_t cur_level= btr_page_get_level(page);
    if (cur_level == level)
      break;

    if (prev_level == UINT16_MAX)
      prev_level= cur_level;
    else if (prev_level != cur_level + 1)
      return DB_CORRUPTION;

    const rec_t *rec= page_rec_get_next_low(page + PAGE_OLD_INFIMUM, false);
    if (rec && rec != page + PAGE_OLD_SUPREMUM)
    {
      ulint len;
      rec+= rec_get_nth_field_offs_old(rec, rec_get_n_fields_old(rec) - 1,
                                       &len);
      if (len != 4 || rec + len - page > page_header_get_field(page,
                                                               PAGE_HEAP_TOP))
        return DB_CORRUPTION;
      child_page_no= mach_read_from_4(rec);
    }
    else
      return DB_CORRUPTION;
    if (cur_level == level + 1)
      break;
    prev_level= cur_level;
  }
  *cur_page_no= child_page_no;
  return err;
}

dberr_t IndexDefragmenter::defragment_level(
  uint16_t level,
  mtr_t *mtr,
  SpaceDefragmenter *space_defrag) noexcept
{
  uint32_t cur_page_no= FIL_NULL;
  dberr_t err= get_level_block(level, mtr, &cur_page_no);
  if (err)
    return err;

  fil_space_t *const space= fil_system.sys_space;
  uint32_t extent_size= space_defrag->m_extent_size;

  buf_block_t *block= fsp_get_latched_page(page_id_t{0, cur_page_no},
                                           mtr, &err);
  if (!block)
    return err;

  for (;;)
  {
    page_t *page= buf_block_get_frame(block);
    uint32_t next_page_no= btr_page_get_next(page);
    uint32_t cur_extent= (cur_page_no / extent_size) * extent_size;
    uint32_t old_state= space_defrag->get_state(cur_extent);

    if (old_state == XDES_FREE)
    {
fetch_next_page:
      if (next_page_no == FIL_NULL)
        break;
      mtr->commit();
      cur_page_no= next_page_no;

      mtr->start();
      mtr->x_lock_space(space);
      block= fsp_get_latched_page(page_id_t{0, cur_page_no},
                                  mtr, &err);
      if (!block)
        return err;
      continue;
    }

    uint32_t new_extent= space_defrag->get_new_extent(cur_extent);
    /* There is no need for extent to be changed */
    if (new_extent == cur_extent)
    {
      if (level)
      {
        /* Store the child page number and their offset
        exist in the parent block records */
        err= get_child_pages(block);
        if (err) return err;
      }
      goto fetch_next_page;
    }

    buf_block_t *header_block=
      fsp_get_latched_page(page_id_t{0, 0}, mtr, &err);
    if (!header_block)
      return err;

    const fseg_header_t *seg_header= m_root->page.frame +
      (level ? PAGE_HEADER + PAGE_BTR_SEG_TOP
             : PAGE_HEADER + PAGE_BTR_SEG_LEAF);

    buf_block_t *iblock;
    fseg_inode_t *inode= fseg_inode_try_get(seg_header, 0, 0, mtr,
                                            &iblock, &err);
    if (!inode)
      return err;

    auto parent_it= m_parent_pages.find(cur_page_no);
    if (parent_it == m_parent_pages.end())
    {
      err= DB_CORRUPTION;
      return err;
    }

    uint32_t parent_page_no= uint32_t(parent_it->second);

    uint32_t parent_offset= uint32_t(parent_it->second >> 32);

    if (parent_offset >= srv_page_size - FIL_PAGE_DATA_END)
    {
      err= DB_CORRUPTION;
      return err;
    }

    PageOperator operation(header_block, iblock, inode, extent_size,
                           page + FIL_PAGE_OFFSET, mtr);

    AssociatedPages related_pages(block, mtr);

    err= operation.prepare_new_page(new_extent, old_state == XDES_FSEG);

    DBUG_EXECUTE_IF("allocation_prepare_fail", err= DB_CORRUPTION;);
    if (err)
    {
err_exit:
      operation.restore_old_pages();
      mtr->discard_modifications();
      return err;
    }

    err= related_pages.prepare(parent_page_no);
    DBUG_EXECUTE_IF("relation_page_prepare_fail", err= DB_CORRUPTION;);

    if (err) goto err_exit;

    operation.complete_new_page();

    /* After allocating the new page, try to prepare the steps
    of page removal function. Because there is a possiblity that
    last block in FSEG_NOT_FULL/FSP_FREE_FRAG/FSP_FREE last block
    could've changed while allocating the new block. */
    err= operation.prepare_old_page();

    DBUG_EXECUTE_IF("remover_prepare_fail", err= DB_CORRUPTION;);
    if (err) goto err_exit;

    /* Copy the data from old block to new block */
    buf_block_t *new_block= operation.get_new_block();
    uint32_t new_page_no= new_block->page.id().page_no();
    /* Copy FIL_PAGE_PREV, FIL_PAGE_NEXT */
    mtr->memcpy<mtr_t::MAYBE_NOP>(*new_block,
                                  new_block->page.frame + FIL_PAGE_PREV,
                                  block->page.frame + FIL_PAGE_PREV,
                                  page_has_next(block->page.frame) ? 8 : 4);
    mtr->memcpy(*new_block, new_block->page.frame + FIL_PAGE_TYPE,
                block->page.frame + FIL_PAGE_TYPE,
                srv_page_size - FIL_PAGE_TYPE - 8);

    /* Assign the new block page number in left, right
    and parent block */
    related_pages.complete(new_page_no, parent_offset);

    /* Complete the page free operation */
    operation.complete_free_old_page();
    /* Add the new page in inode fragment array */
    operation.assign_frag_slot();

    if (level)
    {
      err= get_child_pages(new_block);
      if (err) return err;
    }
    goto fetch_next_page;
  }

  ut_a(!fsp_tablespace_validate(space, mtr));
  ut_a(!fseg_validate_low(space, &m_index, mtr));
  if (level > 1)
  {
    mtr->commit();
    mtr->start();
    mtr->x_lock_space(space);
  }
  return DB_SUCCESS;
}

dberr_t IndexDefragmenter::defragment(SpaceDefragmenter *space_defrag) noexcept
{
  mtr_t mtr;
  mtr.start();
  dberr_t err= DB_SUCCESS;
  m_index.lock.x_lock(SRW_LOCK_CALL);
  fil_space_t *const space= fil_system.sys_space;
  mtr.x_lock_space(space);
  m_root= btr_root_block_get(&m_index, RW_S_LATCH, &mtr, &err);
  if (!m_root)
  {
    mtr.commit();
    m_index.lock.x_unlock();
    return err;
  }

  m_root->page.fix();
  mtr.release_last_page();
  uint16_t level= btr_page_get_level(m_root->page.frame);
  while (1)
  {
    err= defragment_level(level, &mtr, space_defrag);
    DBUG_EXECUTE_IF("fail_after_level_defragment",
                    if (m_index.table->id == 2 && level == 1)
                      err= DB_CORRUPTION;);
    if (err || !level)
      break;
    level--;
  }
  ut_ad(err == DB_SUCCESS || !mtr.has_modifications());
  mtr.commit();
  m_index.lock.x_unlock();
  m_root->page.unfix();
  return err;
}

/** check whether any user table exist in system tablespace
@retval DB_SUCCESS_LOCKED_REC if user table exist
@retval DB_SUCCESS if no user table exist
@retval DB_CORRUPTION if any error encountered */
static dberr_t user_tables_exists() noexcept
{
  mtr_t mtr;
  btr_pcur_t pcur;
  dberr_t err= DB_SUCCESS;
  mtr.start();
  for (const rec_t *rec= dict_startscan_system(&pcur, &mtr,
                                               dict_sys.sys_tables);
       rec; rec= dict_getnext_system(&pcur, &mtr))
  {
    const byte *field= nullptr;
    ulint len= 0;
    if (rec_get_deleted_flag(rec, 0))
    {
corrupt:
      sql_print_error("InnoDB: Encountered corrupted record in SYS_TABLES");
      err= DB_CORRUPTION;
      goto func_exit;
    }
    field= rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__SPACE, &len);
    if (len != 4)
      goto corrupt;
    if (mach_read_from_4(field) != 0)
      continue;
    field= rec_get_nth_field_old(rec, DICT_FLD__SYS_TABLES__ID, &len);
    if (len != 8)
      goto corrupt;
    if (!dict_sys.is_sys_table(mach_read_from_8(field)))
    {
      err= DB_SUCCESS_LOCKED_REC;
      btr_pcur_close(&pcur);
      goto func_exit;
    }
  }
func_exit:
  mtr.commit();
  return err;
}

dberr_t fil_space_t::defragment() noexcept
{
  ut_ad(this == fil_system.sys_space);
  dberr_t err= user_tables_exists();
  if (err == DB_SUCCESS_LOCKED_REC)
  {
    sql_print_information(
      "InnoDB: User table exists in the system tablespace."
      "Please try to move the data from system tablespace "
      "to separate tablespace before defragment the "
      "system tablespace.");
    return DB_SUCCESS;
  } else if (err) { return err; }

  SpaceDefragmenter defragmenter;
  err= defragmenter.find_new_extents();
  /* There is no free extent exist */
  if (err == DB_SUCCESS_LOCKED_REC)
    return DB_SUCCESS;

  if (err == DB_SUCCESS)
    err= defragmenter.defragment_system_tables();
  return err;
}

void fsp_system_tablespace_truncate(bool shutdown)
{
  ut_ad(!purge_sys.enabled());
  ut_ad(!srv_undo_sources);
  uint32_t last_used_extent= 0;
  fil_space_t *space= fil_system.sys_space;
  dberr_t err= space->garbage_collect(shutdown);
  if (err)
  {
    srv_sys_space.set_shrink_fail();
    return;
  }

  if (!shutdown)
  {
    err= space->defragment();
    if (err)
    {
      srv_sys_space.set_shrink_fail();
      return;
    }
  }

  mtr_t mtr;
  mtr.start();
  mtr.x_lock_space(space);
  err= fsp_traverse_extents(space, &last_used_extent, &mtr);
  DBUG_EXECUTE_IF("traversal_extent_fail", err= DB_CORRUPTION;);
  if (err != DB_SUCCESS)
  {
err_exit:
    mtr.commit();
    sql_print_warning("InnoDB: Cannot shrink the system tablespace "
                      "due to %s", ut_strerr(err));
    srv_sys_space.set_shrink_fail();
    return;
  }
  uint32_t fixed_size= srv_sys_space.get_min_size(),
           header_size= space->size_in_header;
  mtr.commit();

  if (last_used_extent >= header_size || fixed_size >= header_size)
    /* Tablespace is being used within fixed size */
    return;

  /* Set fixed size as threshold to truncate */
  if (fixed_size > last_used_extent)
    last_used_extent= fixed_size;

  bool old_dblwr_buf= buf_dblwr.in_use();
  /* Flush all pages in buffer pool, so that it doesn't have to
  use doublewrite buffer and disable dblwr and there should
  be enough space in redo log */
  log_make_checkpoint();
  fil_system.set_use_doublewrite(false);

  buf_block_t *header= nullptr;
#ifdef UNIV_DEBUG
  mtr.start();
  ut_ad(!fsp_tablespace_validate(space, &mtr));
  mtr.commit();
#endif /* UNIV_DEBUG */

  mtr.start();
  mtr.x_lock_space(space);

  {
    /* Take the rough estimation of modified extent
    descriptor page and store their old state */
    fsp_xdes_old_page old_xdes_list(space->id);
    err= fsp_traverse_extents(space, &last_used_extent, &mtr, &old_xdes_list);

    if (err == DB_OUT_OF_MEMORY)
    {
      mtr.commit();
      sql_print_warning("InnoDB: Cannot shrink the system "
                        "tablespace from " UINT32PF" to "
                        UINT32PF " pages due to insufficient "
                        "innodb_buffer_pool_size", space->size,
                        last_used_extent);
      return;
    }

    sql_print_information("InnoDB: Truncating system tablespace from "
                          UINT32PF " to " UINT32PF " pages", space->size,
                          last_used_extent);

    header= fsp_get_latched_page(
              page_id_t(space->id, 0), &mtr, &err);
    if (!header)
      goto err_exit;

    mtr.write<4, mtr_t::FORCED>(
      *header, FSP_HEADER_OFFSET + FSP_SIZE + header->page.frame,
      last_used_extent);

    if (space->free_limit > last_used_extent)
      mtr.write<4,mtr_t::MAYBE_NOP>(*header, FSP_HEADER_OFFSET
                                    + FSP_FREE_LIMIT + header->page.frame,
                                    last_used_extent);
    err= fsp_shrink_list(
      header, FSP_HEADER_OFFSET + FSP_FREE, last_used_extent, &mtr);
    if (err != DB_SUCCESS)
      goto err_exit;

    err= fsp_shrink_list(
      header, FSP_HEADER_OFFSET + FSP_FREE_FRAG, last_used_extent, &mtr);
    if (err != DB_SUCCESS)
      goto err_exit;

    err= fsp_xdes_reset(space->id, last_used_extent, &mtr);
    if (err != DB_SUCCESS)
      goto err_exit;

    mtr.trim_pages(page_id_t(0, last_used_extent));
    size_t shrink_redo_size= mtr.get_log_size();

    DBUG_EXECUTE_IF("mtr_log_max_size", goto mtr_max;);
    if (shrink_redo_size >
          (2 << 20) - 8 /* encryption nonce */ - 5 /* EOF, checksum */)
    {
#ifndef DBUG_OFF
mtr_max:
#endif
      /* Replace the modified copy from buffer pool with
      original copy of the pages. */
      old_xdes_list.restore(&mtr);
      mtr.discard_modifications();
      mtr.commit();
#ifdef UNIV_DEBUG
      mtr.start();
      ut_ad(!fsp_tablespace_validate(space, &mtr));
      mtr.commit();
#endif /* UNIV_DEBUG */
      sql_print_error(
        "InnoDB: Cannot shrink the system tablespace "
        "because the mini-transaction log size (%zu bytes) "
        "exceeds 2 MiB", shrink_redo_size + 8 + 5);
      return;
    }
  }

  if (space->free_limit > last_used_extent)
    space->free_limit= last_used_extent;
  space->free_len= flst_get_len(FSP_HEADER_OFFSET + FSP_FREE +
                                header->page.frame);

  mtr.commit_shrink(*space, last_used_extent);
  sql_print_information("InnoDB: System tablespace truncated successfully");
  fil_system.set_use_doublewrite(old_dblwr_buf);
}

inline void fil_space_t::clear_freed_ranges(uint32_t threshold)
{
  ut_ad(id == SRV_TMP_SPACE_ID);
  std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
  range_set current_ranges;
  for (const auto &range : freed_ranges)
  {
    if (range.first >= threshold)
      continue;
    else if (range.last >= threshold)
    {
      range_t new_range{range.first, threshold - 1};
      current_ranges.add_range(new_range);
      continue;
    }
    current_ranges.add_range(range);
  }
  freed_ranges= std::move(current_ranges);
}

void fsp_shrink_temp_space()
{
  uint32_t last_used_extent= 0;
  fil_space_t *space= fil_system.temp_space;
  mtr_t mtr;
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);
  mtr.x_lock_space(space);
  dberr_t err= fsp_traverse_extents(space, &last_used_extent, &mtr);
  if (err != DB_SUCCESS)
  {
func_exit:
    sql_print_warning("InnoDB: Cannot shrink the temporary tablespace "
                      "due to %s", ut_strerr(err));
    mtr.commit();
    return;
  }
  uint32_t fixed_size= srv_tmp_space.get_min_size(),
           header_size= space->size_in_header;

  if (last_used_extent >= header_size || fixed_size >= header_size)
  {
    /* Tablespace is being used within fixed size */
    mtr.commit();
    return;
  }

  /* Set fixed size as threshold to truncate */
  if (fixed_size > last_used_extent)
    last_used_extent= fixed_size;

  sql_print_information("InnoDB: Truncating temporary tablespace from "
                        UINT32PF " to " UINT32PF " pages", space->size,
                        last_used_extent);

  buf_block_t *header= fsp_get_latched_page(
      page_id_t(space->id, 0), &mtr, &err);
  if (!header)
    goto func_exit;

  mach_write_to_4(
    FSP_HEADER_OFFSET + FSP_SIZE + header->page.frame,
    last_used_extent);

  if (space->free_limit > last_used_extent)
    mach_write_to_4(
      FSP_HEADER_OFFSET + FSP_FREE_LIMIT + header->page.frame,
      last_used_extent);

  mtr.set_modified(*header);

  err= fsp_shrink_list(header, FSP_HEADER_OFFSET + FSP_FREE,
                       last_used_extent, &mtr);

  if (err != DB_SUCCESS)
    goto func_exit;

  err= fsp_shrink_list(
         header, FSP_HEADER_OFFSET + FSP_FREE_FRAG,
         last_used_extent, &mtr);
  DBUG_EXECUTE_IF("fail_temp_truncate", err= DB_ERROR;);
  if (err != DB_SUCCESS)
    goto func_exit;

  err= fsp_xdes_reset(space->id, last_used_extent, &mtr);
  if (err != DB_SUCCESS)
    goto func_exit;

  space->clear_freed_ranges(last_used_extent);
  buf_LRU_truncate_temp(last_used_extent);
  mysql_mutex_lock(&fil_system.mutex);

  space->size= last_used_extent;
  if (space->free_limit > last_used_extent)
    space->free_limit= space->size;

  space->free_len= flst_get_len(
    FSP_HEADER_OFFSET + FSP_FREE+ header->page.frame);

  /* Last file new size after truncation */
  uint32_t new_last_file_size=
    last_used_extent -
    (fixed_size - srv_tmp_space.m_files.at(
     srv_tmp_space.m_files.size() - 1).param_size());

  space->size_in_header= space->size;
  space->chain.end->size= new_last_file_size;
  srv_tmp_space.set_last_file_size(new_last_file_size);
  mysql_mutex_unlock(&fil_system.mutex);
  os_file_truncate(
    space->chain.end->name, space->chain.end->handle,
    os_offset_t{space->chain.end->size} << srv_page_size_shift, true);
  mtr.commit();
  sql_print_information("InnoDB: Temporary tablespace truncated successfully");
}



fil_space_t* binlog_space;
buf_block_t *binlog_cur_block;
uint32_t binlog_cur_page_no;
uint32_t binlog_cur_page_offset;

/** Create a binlog tablespace file
@param[in] name	 file name
@return DB_SUCCESS or error code */
dberr_t fsp_binlog_tablespace_create(const char* name)
{
	pfs_os_file_t	fh;
	bool		ret;

	uint32_t size= (1<<20) >> srv_page_size_shift /* ToDo --max-binlog-size */;
	if(srv_read_only_mode)
		return DB_ERROR;

	os_file_create_subdirs_if_needed(name);

	/* ToDo: Do we need here an mtr.log_file_op(FILE_CREATE) like in fil_ibd_create(()? */
	fh = os_file_create(
		innodb_data_file_key, name,
		OS_FILE_CREATE, OS_DATA_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		os_file_close(fh);
		return DB_ERROR;
	}

	/* ToDo: Enryption? */
	fil_encryption_t mode= FIL_ENCRYPTION_OFF;
	fil_space_crypt_t* crypt_data= nullptr;

	/* We created the binlog file and now write it full of zeros */
	if (!os_file_set_size(name, fh,
			      os_offset_t{size} << srv_page_size_shift)) {
		ib::error() << "Unable to allocate " << name;
		os_file_close(fh);
		os_file_delete(innodb_data_file_key, name);
		return DB_ERROR;
	}

	mysql_mutex_lock(&fil_system.mutex);
	uint32_t space_id= SRV_SPACE_ID_BINLOG0;
	if (!(binlog_space= fil_space_t::create(space_id,
                                                ( FSP_FLAGS_FCRC32_MASK_MARKER |
						  FSP_FLAGS_FCRC32_PAGE_SSIZE()),
						false, crypt_data,
						mode, true))) {
		mysql_mutex_unlock(&fil_system.mutex);
		return DB_ERROR;
	}

	fil_node_t* node = binlog_space->add(name, fh, size, false, true);
	node->find_metadata();
	mysql_mutex_unlock(&fil_system.mutex);

        binlog_cur_page_no= 0;
        binlog_cur_page_offset= FIL_PAGE_DATA;
	return DB_SUCCESS;
}

void fsp_binlog_write_start(uint32_t page_no,
                            const uchar *data, uint32_t len, mtr_t *mtr)
{
	buf_block_t *block= fsp_page_create(binlog_space, page_no, mtr);
	mtr->memcpy<mtr_t::MAYBE_NOP>(*block, FIL_PAGE_DATA + block->page.frame,
				      data, len);
	binlog_cur_block= block;
}

void fsp_binlog_write_offset(uint32_t page_no, uint32_t offset,
                             const uchar *data, uint32_t len, mtr_t *mtr)
{
	dberr_t err;
        /* ToDo: Is RW_SX_LATCH appropriate here? */
	buf_block_t *block= buf_page_get_gen(page_id_t{binlog_space->id, page_no},
					     0, RW_SX_LATCH, binlog_cur_block,
					     BUF_GET, mtr, &err);
	ut_a(err == DB_SUCCESS);
	mtr->memcpy<mtr_t::MAYBE_NOP>(*block,
                                      offset + block->page.frame,
                                      data, len);
}

void fsp_binlog_append(const uchar *data, uint32_t len, mtr_t *mtr)
{
  ut_ad(binlog_cur_page_offset <= srv_page_size - FIL_PAGE_DATA_END);
  uint32_t remain= ((uint32_t)srv_page_size - FIL_PAGE_DATA_END) -
    binlog_cur_page_offset;
  // ToDo: Some kind of mutex to protect binlog access.
  while (len > 0) {
    if (remain < 4) {
      binlog_cur_page_offset= FIL_PAGE_DATA;
      remain= ((uint32_t)srv_page_size - FIL_PAGE_DATA_END) -
        binlog_cur_page_offset;
      ++binlog_cur_page_no;
    }
    uint32_t this_len= std::min<uint32_t>(len, remain);
    if (binlog_cur_page_offset == FIL_PAGE_DATA)
      fsp_binlog_write_start(binlog_cur_page_no, data, this_len, mtr);
    else
      fsp_binlog_write_offset(binlog_cur_page_no, binlog_cur_page_offset,
                              data, this_len, mtr);
    len-= this_len;
    data+= this_len;
    binlog_cur_page_offset+= this_len;
  }
}

void fsp_binlog_write_cache(IO_CACHE *cache, size_t main_size, mtr_t *mtr)
{
  fil_space_t *space= binlog_space;
  if (!space) {
    fsp_binlog_tablespace_create("./binlog-000000.ibb");
    space= binlog_space;
  }
  /* ToDo: Is this set_named_space() needed / appropriate? */
  mtr->set_named_space(space);
  const uint32_t page_end= (uint32_t)srv_page_size - FIL_PAGE_DATA_END;
  uint32_t page_no= binlog_cur_page_no;
  uint32_t page_offset= binlog_cur_page_offset;
  /* ToDo: What is the lifetime of what's pointed to by binlog_cur_block, is there some locking needed around it or something? */
  buf_block_t *block= binlog_cur_block;

  /*
    Write out the event data in chunks of whatever size will fit in the current
    page, until all data has been written.
  */
  size_t remain= my_b_tell(cache);
  ut_ad(remain > main_size);
  /* Start with the GTID event, which is put at the end of the IO_CACHE. */
  my_bool res= reinit_io_cache(cache, READ_CACHE, main_size, 0, 0);
  ut_a(!res /* ToDo: Error handling. */);
  size_t gtid_remain= remain - main_size;
  while (remain > 0) {
    if (page_offset == FIL_PAGE_DATA) {
      block= fsp_page_create(space, page_no, mtr);
    } else {
      dberr_t err;
      /* ToDo: Is RW_SX_LATCH appropriate here? */
      block= buf_page_get_gen(page_id_t{space->id, page_no},
                              0, RW_SX_LATCH, binlog_cur_block,
                              BUF_GET, mtr, &err);
      ut_a(err == DB_SUCCESS);
    }

    ut_ad(page_offset < page_end);
    uint32_t page_remain= page_end - page_offset;
    byte *ptr= page_offset + block->page.frame;
    /* ToDo: Do this check at the end instead, to save one buf_page_get_gen()? */
    if (page_remain < 4) {
      /* Pad the remaining few bytes, and move to next page. */
      mtr->memset(block, page_offset, page_remain, 0xff);
      block= nullptr;
      ++page_no;
      page_offset= FIL_PAGE_DATA;
      continue;
    }
    page_remain-= 3;    /* Type byte and 2-byte length. */
    uint32_t size= 0;
    /* Write GTID data, if any still available. */
    if (gtid_remain > 0)
    {
      size= gtid_remain > page_remain ? page_remain : (uint32_t)gtid_remain;
      int res2= my_b_read(cache, ptr+3, size);
      ut_a(!res2 /* ToDo: Error handling */);
      gtid_remain-= size;
      page_remain-= size;
      if (gtid_remain == 0)
        my_b_seek(cache, 0);    /* Move to read the rest of the events. */
    }
    /* Write remaining data, if any available _and_ more room on page. */
    ut_ad(remain >= size);
    size_t remain2= remain - size;
    if (remain2 + page_remain > 0) {
      uint32_t size2= remain2 > page_remain ? page_remain : (uint32_t)remain2;
      int res2= my_b_read(cache, ptr+3+size, size2);
      ut_a(!res2 /* ToDo: Error handling */);
      size+= size2;
      page_remain-= size2;
    }
    ptr[0]= 0x01 /* ToDo: FSP_BINLOG_TYPE_COMMIT */ | ((size < remain) << 7);
    ptr[1]= size & 0xff;
    ptr[2]= (byte)(size >> 8);
    ut_ad(size <= 0xffff);

    mtr->memcpy(*block, page_offset, size+3);
    remain-= size;
    if (page_remain == 0) {
      block= nullptr;
      page_offset= FIL_PAGE_DATA;
      ++page_no;
    } else {
      page_offset+= size+3;
    }
  }
  binlog_cur_block= block;
  binlog_cur_page_no= page_no;
  binlog_cur_page_offset= page_offset;
}


extern "C" void binlog_get_cache(THD *, IO_CACHE **, size_t *);

void
fsp_binlog_trx(trx_t *trx, mtr_t *mtr)
{
  IO_CACHE *cache;
  size_t main_size;

  if (!trx->mysql_thd)
    return;
  binlog_get_cache(trx->mysql_thd, &cache, &main_size);
  if (main_size)
    fsp_binlog_write_cache(cache, main_size, mtr);
}


void fsp_binlog_test(const uchar *data, uint32_t len)
{
  mtr_t mtr;
  mtr.start();
  if (!binlog_space)
    fsp_binlog_tablespace_create("./binlog-000000.ibb");
  mtr.set_named_space(binlog_space);
  fsp_binlog_append(data, len, &mtr);
  mtr.commit();
}
