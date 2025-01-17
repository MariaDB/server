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

#include <memory>
#include <cctype>
#include <cstdlib>
#include <thread>

#include "fsp0fsp.h"
#include "buf0flu.h"
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
#include <unordered_set>
#include "trx0undo.h"
#include "trx0trx.h"
#include "ut0compr_int.h"
#include "rpl_gtid_base.h"

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
ulint
xdes_get_state(
/*===========*/
	const xdes_t*	descr)	/*!< in: descriptor */
{
	ulint	state;

	ut_ad(descr);
	state = mach_read_from_4(descr + XDES_STATE);
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
    ut_ad(mtr.is_named_space(id) ||
          id == SRV_SPACE_ID_BINLOG0 || id == SRV_SPACE_ID_BINLOG1);
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
                                    mtr_t *mtr)
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
static ulint fseg_get_n_frag_pages(const fseg_inode_t *inode)
{
	ulint	i;
	ulint	count	= 0;

	for (i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i++) {
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
ulint
fseg_n_reserved_pages_low(
/*======================*/
	const fseg_inode_t*	inode,	/*!< in: segment inode */
	ulint*		used)	/*!< out: number of pages used (not
				more than reserved) */
{
	*used = mach_read_from_4(inode + FSEG_NOT_FULL_N_USED)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FULL)
		+ fseg_get_n_frag_pages(inode);

	return fseg_get_n_frag_pages(inode)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FREE)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_NOT_FULL)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FULL);
}

/** Calculate the number of pages reserved by a segment,
and how many pages are currently used.
@param[in]      block   buffer block containing the file segment header
@param[in]      header  file segment header
@param[out]     used    number of pages that are used (not more than reserved)
@param[in,out]  mtr     mini-transaction
@return number of reserved pages */
ulint fseg_n_reserved_pages(const buf_block_t &block,
                            const fseg_header_t *header, ulint *used,
                            mtr_t *mtr)
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
  ulint	used;

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
	ulint		used;
	ulint		reserved;
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

/** Get the latched extent descriptor page or
acquire the extent descriptor page.
@param page_id  page identifier to be acquired
@param mtr      mini-transaction
@param err      error code
@return block descriptor */
static
buf_block_t *fsp_get_latched_xdes_page(
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
  ulint n_pages()
  {
    uint32_t count=0;
    for (uint32_t i= 0; i < m_old_xdes_pages.size(); i++)
      if (m_old_xdes_pages[i]) count++;
    return count;
  }

  __attribute__((warn_unused_result))
  dberr_t insert(uint32_t page_no, mtr_t *mtr)
  {
    uint32_t m_index= page_no >> srv_page_size_shift;
    if (m_old_xdes_pages.size() > m_index &&
        m_old_xdes_pages[m_index] != nullptr)
      return DB_SUCCESS;

    DBUG_EXECUTE_IF("shrink_buffer_pool_full",
                    return DB_OUT_OF_MEMORY;);
    dberr_t err= DB_SUCCESS;
    buf_block_t *block= fsp_get_latched_xdes_page(
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

  buf_block_t *search(uint32_t page_no)
  {
    uint32_t m_index= page_no >> srv_page_size_shift;
    if (m_index > m_old_xdes_pages.size())
      return nullptr;
    return m_old_xdes_pages[m_index];
  }

  void restore(mtr_t *mtr)
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
  uint32_t skip_len, mtr_t *mtr)
{
  dberr_t err= DB_SUCCESS;
  uint32_t space_id= header->page.id().space();
  buf_block_t *cur= fsp_get_latched_xdes_page(
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
      prev= fsp_get_latched_xdes_page(
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
  mtr_t *mtr)
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

    buf_block_t *cur_block= fsp_get_latched_xdes_page(
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
                        uint32_t threshold, mtr_t *mtr)
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
      descr_block= fsp_get_latched_xdes_page(
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
  buf_block_t *block= fsp_get_latched_xdes_page(
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
      block= fsp_get_latched_xdes_page(
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

#ifdef UNIV_DEBUG
/** Validate the system tablespace list */
__attribute__((warn_unused_result))
dberr_t fsp_tablespace_validate(fil_space_t *space)
{
  /* Validate all FSP list in system tablespace */
  mtr_t local_mtr;
  dberr_t err= DB_SUCCESS;
  local_mtr.start();
  if (buf_block_t *header= fsp_get_header(
        space, &local_mtr, &err))
  {
    flst_validate(header, FSP_FREE + FSP_HEADER_OFFSET, &local_mtr);
    flst_validate(header, FSP_FREE_FRAG + FSP_HEADER_OFFSET,
                  &local_mtr);
    flst_validate(header, FSP_HEADER_OFFSET + FSP_FULL_FRAG,
                  &local_mtr);
    flst_validate(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FULL,
                  &local_mtr);
    flst_validate(header, FSP_HEADER_OFFSET + FSP_SEG_INODES_FREE,
                  &local_mtr);
  }
  local_mtr.commit();
  return err;
}
#endif /* UNIV_DEBUG */

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
  ut_ad(!fsp_tablespace_validate(space));

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

    header= fsp_get_latched_xdes_page(
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
      ut_ad(!fsp_tablespace_validate(space));
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

  buf_block_t *header= fsp_get_latched_xdes_page(
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


/*
  Binlog implementation in InnoDB.
  ToDo: Move this somewhere reasonable, its own file(s) etc.
*/

enum fsp_binlog_chunk_types {
  /* Zero means no data, effectively EOF. */
  FSP_BINLOG_TYPE_EMPTY= 0,
  /* A binlogged committed event group. */
  FSP_BINLOG_TYPE_COMMIT= 1,
  /* A binlog GTID state record. */
  FSP_BINLOG_TYPE_GTID_STATE= 2,
  /* Out-of-band event group data. */
  FSP_BINLOG_TYPE_OOB_DATA= 3,

  /* Padding data at end of page. */
  FSP_BINLOG_TYPE_FILLER= 0xff
};

/*
  Bit set on the chunk type for a continuation chunk, when data needs to be
  split across pages.
*/
static constexpr uint32_t FSP_BINLOG_FLAG_BIT_CONT= 7;
static constexpr uint32_t FSP_BINLOG_FLAG_CONT= (1 << FSP_BINLOG_FLAG_BIT_CONT);
/*
  Bit set on the chunk type for the last chunk (no continuation chunks
  follow)
*/
static constexpr uint32_t FSP_BINLOG_FLAG_BIT_LAST= 6;
static constexpr uint32_t FSP_BINLOG_FLAG_LAST= (1 << FSP_BINLOG_FLAG_BIT_LAST);
static constexpr uint32_t FSP_BINLOG_TYPE_MASK=
  ~(FSP_BINLOG_FLAG_CONT | FSP_BINLOG_FLAG_LAST);


static uint32_t binlog_size_in_pages;
buf_block_t *binlog_cur_block;
uint32_t binlog_cur_page_no;
uint32_t binlog_cur_page_offset;
/*
  How often (in terms of bytes written) to dump a (differential) binlog state
  at the start of the page, to speed up finding the initial GTID position for
  a connecting slave.

  This value must be used over the setting innodb_binlog_state_interval,
  because after a restart the latest binlog file will be using the value of the
  setting prior to the restart; the new value of the setting (if different)
  will be used for newly created binlog files.
*/
uint64_t current_binlog_state_interval;
/* The corresponding server setting, read-only. */
ulonglong innodb_binlog_state_interval;
/*
  Mutex protecting active_binlog_file_no and active_binlog_space.
*/
mysql_mutex_t active_binlog_mutex;
pthread_cond_t active_binlog_cond;
static std::thread binlog_prealloc_thr_obj;
static bool prealloc_thread_end= false;
/* The currently being written binlog tablespace. */
std::atomic<uint64_t> active_binlog_file_no;
fil_space_t* active_binlog_space;
/*
  The first binlog tablespace that is still open.
  This can be equal to active_binlog_file_no, if the tablespace prior to the
  active one has been fully flushed out to disk and closed.
  Or it can be one less, if the prior tablespace is still being written out and
  closed.
*/
static uint64_t first_open_binlog_file_no;
/*
  The most recent created and open tablespace.
  This can be equal to active_binlog_file_no+1, if the next tablespace to be
  used has already been pre-allocated and opened.
  Or it can be the same as active_binlog_file_no, if the pre-allocation of the
  next tablespace is still pending.
*/
uint64_t last_created_binlog_file_no;
fil_space_t *last_created_binlog_space;

/*
  Differential binlog state in the currently active binlog tablespace, relative
  to the state at the start.
*/
static rpl_binlog_state_base binlog_diff_state;

/*
  Point at which it is guaranteed that all data has been written out to the
  binlog file (on the OS level; not necessarily fsync()'ed yet).

  Stores the most recent two values, each corresponding to active_binlog_file_no&1.
*/
/* ToDo: maintain this offset value as up to where data has been written out to the OS. Needs to be binary-searched in current binlog file at server restart; which is also a reason why it might not be a multiple of the page size. */
std::atomic<uint64_t> binlog_cur_written_offset[2];
/*
  Offset of last valid byte of data in most recent 2 binlog files.
  A value of ~0 means that file is not opened as a tablespace (and data is
  valid until the end of the file).
*/
std::atomic<uint64_t> binlog_cur_end_offset[2];


/*
  The struct chunk_data_base is a simple encapsulation of data for a chunk that
  is to be written to the binlog. Used to separate the generic code that
  handles binlog writing with page format and so on, from the details of the
  data being written, avoiding an intermediary buffer holding consecutive data.

  Currently used for:
   - chunk_data_cache: A binlog trx cache to be binlogged as a commit record.
   - chunk_data_oob: An out-of-band piece of event group data.
*/
struct chunk_data_base {
  /*
    Copy at most max_len bytes to address p.
    Returns a pair with amount copied, and a bool if this is the last data.
    Should return the maximum amount of data available (up to max_len). Thus
    the size returned should only be less than max_len if the last-data flag
    is returned as true.
  */
  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) = 0;
  virtual ~chunk_data_base() {};
};


/* Structure holding context for out-of-band chunks of binlogged event group. */
struct binlog_oob_context {
  /*
    Structure used to encapsulate the data to be binlogged in an out-of-band
    chunk, for use by fsp_binlog_write_chunk().
  */
  struct chunk_data_oob : public chunk_data_base {
    /*
      Need room for 5 numbers:
        node index
        left child file_no
        left child offset
        right child file_no
        right child offset
    */
    static constexpr uint32_t max_buffer= 5*COMPR_INT_MAX64;
    uint64_t sofar;
    uint64_t main_len;
    byte *main_data;
    uint32_t header_len;
    byte header_buf[max_buffer];

    chunk_data_oob(uint64_t idx,
                   uint64_t left_file_no, uint64_t left_offset,
                   uint64_t right_file_no, uint64_t right_offset,
                   byte *data, size_t data_len);
    virtual ~chunk_data_oob() {};
    virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final;
  };

  bool binlog_node(uint32_t node, uint64_t new_idx,
                   uint32_t left_node, uint32_t right_node,
                   chunk_data_oob *oob_data);

  uint64_t first_node_file_no;
  uint64_t first_node_offset;
  uint32_t node_list_len;
  uint32_t node_list_alloc_len;
  /*
    The node_list contains the root of each tree in the forest of perfect
    binary trees.
  */
#ifdef _MSC_VER
/* Flexible array member is not standard C++, disable compiler warning. */
#pragma warning(disable : 4200)
#endif
  struct node_info {
    uint64_t file_no;
    uint64_t offset;
    uint64_t node_index;
    uint32_t height;
  } node_list [];
};


class ha_innodb_binlog_reader : public handler_binlog_reader {
  /* Buffer to hold a page read directly from the binlog file. */
  uchar *page_buf;
  /* Length of the currently open file (if cur_file != -1). */
  uint64_t cur_file_length;
  /* Used to keep track of partial chunk returned to reader. */
  uint32_t chunk_pos;
  uint32_t chunk_remain;
  /*
    Flag used to skip the rest of any partial chunk we might be starting in
    the middle of.
  */
  bool skipping_partial;
private:
  bool ensure_file_open();
  void next_file();
  int read_from_buffer_pool_page(buf_block_t *block, uint64_t end_offset,
                                 uchar *buf, uint32_t len);
  int read_from_file(uint64_t end_offset, uchar *buf, uint32_t len);
  int read_from_page(uchar *page_ptr, uint64_t end_offset,
                     uchar *buf, uint32_t len);

public:
  ha_innodb_binlog_reader(uint64_t file_no= 0, uint64_t offset= 0);
  ~ha_innodb_binlog_reader();
  virtual int read_binlog_data(uchar *buf, uint32_t len) final;
  virtual bool data_available() final;
  virtual int init_gtid_pos(slave_connection_state *pos,
                            rpl_binlog_state_base *state) final;
};


static void fsp_binlog_prealloc_thread();
static int fsp_binlog_discover();
static bool binlog_state_recover();

#define BINLOG_NAME_BASE "binlog-"
#define BINLOG_NAME_EXT ".ibb"
/* '.' + '/' + "binlog-" + (<=20 digits) + '.' + "ibb" + '\0'. */
#define BINLOG_NAME_LEN 1 + 1 + 7 + 20 + 1 + 3 + 1
static inline void
binlog_name_make(char name_buf[BINLOG_NAME_LEN], uint64_t file_no)
{
  sprintf(name_buf, "./" BINLOG_NAME_BASE "%06" PRIu64 BINLOG_NAME_EXT,
          file_no);
}


/*
  Check if this is an InnoDB binlog file name.
  Return the index/file_no if so.
*/
static bool
is_binlog_name(const char *name, uint64_t *out_idx)
{
  const size_t base_len= sizeof(BINLOG_NAME_BASE) - 1;  // Length without '\0' terminator
  const size_t ext_len= sizeof(BINLOG_NAME_EXT) - 1;

  if (0 != strncmp(name, BINLOG_NAME_BASE, base_len))
    return false;
  size_t name_len= strlen(name);
  if (name_len < base_len + 1 + ext_len)
    return false;
  const char *ext_start= name + (name_len - ext_len);
  if (0 != strcmp(ext_start, BINLOG_NAME_EXT))
    return false;
  if (!std::isdigit((unsigned char)(name[base_len])))
    return false;
  char *conv_end= nullptr;
  unsigned long long idx= std::strtoull(name + base_len, &conv_end, 10);
  if (idx == ULLONG_MAX || conv_end != ext_start)
    return false;

  *out_idx= (uint64_t)idx;
  return true;
}


/** Write out all pages, flush, and close/detach a binlog tablespace.
@param[in] file_no	 Index of the binlog tablespace
@return DB_SUCCESS or error code */
static dberr_t
fsp_binlog_tablespace_close(uint64_t file_no)
{
  mtr_t mtr;
  dberr_t res;

  uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);
  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(space_id);
  mysql_mutex_unlock(&fil_system.mutex);
  if (!space) {
    res= DB_ERROR;
    goto end;
  }

  /*
    Write out any remaining pages in the buffer pool to the binlog tablespace.
    Then flush the file to disk, and close the old tablespace.

    ToDo: Will this turn into a busy-wait if some of the pages are still latched
    in this tablespace, maybe because even though the tablespace has been
    written full, the mtr that's ending in the next tablespace may still be
    active? This will need fixing, no busy-wait should be done here.
  */

  /*
    Take and release an exclusive latch on the last page in the tablespace to
    be closed. We might be signalled that the tablespace is done while the mtr
    completing the tablespace write is still active; the exclusive latch will
    ensure we wait for any last mtr to commit before we close the tablespace.
  */
  mtr.start();
  buf_page_get_gen(page_id_t{space_id, space->size - 1}, 0, RW_X_LATCH, nullptr,
                   BUF_GET, &mtr, &res);
  mtr.commit();

  while (buf_flush_list_space(space))
    ;
  // ToDo: Also, buf_flush_list_space() seems to use io_capacity, but that's not appropriate here perhaps
  os_aio_wait_until_no_pending_writes(false);
  space->flush<false>();
  fil_space_free(space_id, false);
  res= DB_SUCCESS;
end:
  return res;
}


/*
  Initialize the InnoDB implementation of binlog.
  Note that we do not create or open any binlog tablespaces here.
  This is only done if InnoDB binlog is enabled on the server level.
*/
void
fsp_binlog_init()
{
  mysql_mutex_init(fsp_active_binlog_mutex_key, &active_binlog_mutex, nullptr);
  pthread_cond_init(&active_binlog_cond, nullptr);
  binlog_diff_state.init();
}


/*
  Open the InnoDB binlog implementation.
  This is called from server binlog layer if the user configured the binlog to
  use the innodb implementation (with --binlog-storage-engine=innodb).
*/
bool
innodb_binlog_init(size_t binlog_size)
{
  uint64_t pages= binlog_size >> srv_page_size_shift;
  if (UNIV_LIKELY(pages > (uint64_t)UINT32_MAX)) {
    pages= UINT32_MAX;
    ib::warn() << "Requested max_binlog_size is larger than the maximum " <<
      "InnoDB tablespace size, truncated to " <<
      (pages << srv_page_size_shift) << ".";
  } else if (pages < 2) {  /* Minimum one data page and one index page. */
    pages= 2;
    ib::warn() << "Requested max_binlog_size is smaller than the minimum " <<
      "size supported by InnoDB, truncated to " <<
      (pages << srv_page_size_shift) << ".";
  }
  binlog_size_in_pages= (uint32_t)pages;

  first_open_binlog_file_no= ~(uint64_t)0;
  binlog_cur_end_offset[0].store(~(uint64_t)0, std::memory_order_relaxed);
  binlog_cur_end_offset[1].store(~(uint64_t)0, std::memory_order_relaxed);
  last_created_binlog_file_no= ~(uint64_t)0;
  active_binlog_file_no.store(~(uint64_t)0, std::memory_order_release);
  active_binlog_space= nullptr;
  binlog_cur_page_no= 0;
  binlog_cur_page_offset= FIL_PAGE_DATA;
  current_binlog_state_interval= innodb_binlog_state_interval;
  /* Find any existing binlog files and continue writing in them. */
  int res= fsp_binlog_discover();
  if (res < 0)
  {
    /* Need to think more on the error handling if the binlog cannot be opened. We may need to abort starting the server, at least for some errors? And/or in some cases maybe force ignore any existing unusable files and continue with a new binlog (but then maybe fsp_binlog_discover() should return 0 and print warnings in the error log?). */
    return true;
  }
  if (res > 0)
  {
    /* We are continuing from existing binlogs. Recover the binlog state. */
    if (binlog_state_recover())
      return true;
  }

  /* Start pre-allocating new binlog files. */
  binlog_prealloc_thr_obj= std::thread{fsp_binlog_prealloc_thread};

  mysql_mutex_lock(&active_binlog_mutex);
  while (last_created_binlog_file_no == ~(uint64_t)0) {
    /* Wait for the first binlog file to be available. */
    my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);
  }
  mysql_mutex_unlock(&active_binlog_mutex);

  return false;
}


struct found_binlogs {
  uint64_t last_file_no, prev_file_no;
  size_t last_size, prev_size;
  int found_binlogs;
};


/* Compute the (so far) last and last-but-one binlog files found. */
static void
process_binlog_name(found_binlogs *bls, uint64_t idx, size_t size)
{
  if (bls->found_binlogs == 0 ||
      idx > bls->last_file_no) {
    if (bls->found_binlogs >= 1 && idx == bls->last_file_no + 1) {
      bls->prev_file_no= bls->last_file_no;
      bls->prev_size= bls->last_size;
      bls->found_binlogs= 2;
    } else {
      bls->found_binlogs= 1;
    }
    bls->last_file_no= idx;
    bls->last_size= size;
  } else if (bls->found_binlogs == 1 && idx + 1 == bls->last_file_no) {
    bls->found_binlogs= 2;
    bls->prev_file_no= idx;
    bls->prev_size= size;
  }
}


/*
  Open an existing tablespace. The filehandle fh is taken over by the tablespace
  (or closed in case of error).
*/
static fil_space_t *
fsp_binlog_open(const char *file_name, pfs_os_file_t fh,
                uint64_t file_no, size_t file_size, bool open_empty)
{
  const uint32_t page_size= (uint32_t)srv_page_size;
  const uint32_t page_size_shift= srv_page_size_shift;

  os_offset_t binlog_size= max_binlog_size;
  if (open_empty && file_size < binlog_size) {
    /*
      A crash may have left a partially pre-allocated file. If so, extend it
      to the required size.
      Note that this may also extend a previously pre-allocated file to the new
      binlog configured size, if the configuration changed during server
      restart.
    */
    if (!os_file_set_size(file_name, fh, binlog_size, false)) {
      ib::warn() << "Failed to change the size of InnoDB binlog file " <<
        file_name << " from " << file_size << " to " << binlog_size <<
        " bytes (error code: " << errno << ").";
    } else {
      file_size= binlog_size;
    }
  }
  if (file_size < 2*page_size)
  {
    ib::warn() << "InnoDB binlog file number " << file_no << " is too short"
      " (" << file_size << " bytes), should be at least " << 2*page_size <<
      " bytes.";
    os_file_close(fh);
    return nullptr;
  }

  uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);

  if (!open_empty) {
    page_t *page_buf= static_cast<byte*>(aligned_malloc(page_size, page_size));
    if (!page_buf) {
      os_file_close(fh);
      return nullptr;
    }

    dberr_t err= os_file_read(IORequestRead, fh, page_buf, 0, page_size, nullptr);
    if (err != DB_SUCCESS) {
      ib::warn() << "Unable to read first page of file " << file_name;
      aligned_free(page_buf);
      os_file_close(fh);
      return nullptr;
    }

    /* ToDo: Maybe use leaner page format for binlog tablespace? */
    uint32_t id1= mach_read_from_4(FIL_PAGE_SPACE_ID + page_buf);
    if (id1 != space_id) {
      ib::warn() << "Binlog file " << file_name <<
        " has inconsistent tablespace id " << id1 <<
        " (expected " << space_id << ")";
      aligned_free(page_buf);
      os_file_close(fh);
      return nullptr;
    }
    // ToDo: should we here check buf_page_is_corrupted() ?

    aligned_free(page_buf);
  }

  uint32_t fsp_flags=
    FSP_FLAGS_FCRC32_MASK_MARKER | FSP_FLAGS_FCRC32_PAGE_SSIZE();
  /* ToDo: Enryption. */
  fil_encryption_t mode= FIL_ENCRYPTION_OFF;
  fil_space_crypt_t* crypt_data= nullptr;
  fil_space_t *space;

  mysql_mutex_lock(&fil_system.mutex);
  if (!(space= fil_space_t::create(space_id, fsp_flags,
                                   FIL_TYPE_TABLESPACE, crypt_data,
                                   mode, true))) {
    mysql_mutex_unlock(&fil_system.mutex);
    os_file_close(fh);
    return nullptr;
  }

  space->add(file_name, fh, (uint32_t)(file_size >> page_size_shift),
             false, true);

  first_open_binlog_file_no= file_no;
  if (last_created_binlog_file_no == ~(uint64_t)0 ||
      file_no > last_created_binlog_file_no) {
    last_created_binlog_file_no= file_no;
    last_created_binlog_space= space;
  }

  mysql_mutex_unlock(&fil_system.mutex);
  return space;
}


static bool
binlog_page_empty(const byte *page)
{
  /* ToDo: Here we also need to see if there is a full state record at the start of the file. If not, we have to delete the file and ignore it, it is an incomplete file. Or can we rely on the innodb crash recovery to make file creation atomic and we will never see a partially pre-allocated file? Also if the gtid state is larger than mtr max size (if there is such max?), or if we crash in the middle of pre-allocation? */
  return page[FIL_PAGE_DATA] == 0;
}


/*
  Find the last written position in the binlog file.
  Do a binary search through the pages to find the last non-empty page, then
  scan the page to find the place to start writing new binlog data.

  Returns:
     1 position found, output in *out_space, *out_page_no and *out_pos_in_page.
     0 binlog file is empty.
    -1 error.
*/

static int
find_pos_in_binlog(uint64_t file_no, size_t file_size, byte *page_buf,
                   fil_space_t **out_space,
                   uint32_t *out_page_no, uint32_t *out_pos_in_page)
{
  const uint32_t page_size= (uint32_t)srv_page_size;
  const uint32_t page_size_shift= (uint32_t)srv_page_size_shift;
  const uint32_t idx= file_no & 1;
  char file_name[BINLOG_NAME_LEN];
  uint32_t p_0, p_1, p_2, last_nonempty;
  dberr_t err;
  byte *p, *page_end;
  bool ret;

  *out_page_no= 0;
  *out_pos_in_page= FIL_PAGE_DATA;

  binlog_name_make(file_name, file_no);
  pfs_os_file_t fh= os_file_create(innodb_data_file_key, file_name,
                                   OS_FILE_OPEN, OS_DATA_FILE,
                                   srv_read_only_mode, &ret);
  if (!ret) {
    ib::warn() << "Unable to open file " << file_name;
    return -1;
  }

  err= os_file_read(IORequestRead, fh, page_buf, 0, page_size, nullptr);
  if (err != DB_SUCCESS) {
    os_file_close(fh);
    return -1;
  }
  if (binlog_page_empty(page_buf)) {
    *out_space= fsp_binlog_open(file_name, fh, file_no, file_size, true);
    binlog_cur_written_offset[idx].store(0, std::memory_order_relaxed);
    binlog_cur_end_offset[idx].store(0, std::memory_order_relaxed);
    return (*out_space ? 0 : -1);
  }
  last_nonempty= 0;

  /*
    During the binary search, p_0-1 is the largest page number that is know to
    be non-empty. And p_2 is the first page that is known to be empty.
  */
  p_0= 1;
  p_2= (uint32_t)(file_size / page_size);
  for (;;) {
    if (p_0 == p_2)
      break;
    ut_ad(p_0 < p_2);
    p_1= (p_0 + p_2) / 2;
    err= os_file_read(IORequestRead, fh, page_buf, p_1 << page_size_shift,
                      page_size, nullptr);
    if (err != DB_SUCCESS) {
      os_file_close(fh);
      return -1;
    }
    if (binlog_page_empty(page_buf)) {
      p_2= p_1;
    } else {
      p_0= p_1 + 1;
      last_nonempty= p_1;
    }
  }
  /* At this point, p_0 == p_2 is the first empty page. */
  ut_ad(p_0 >= 1);

  /*
    This sometimes does an extra read, but as this is only during startup it
    does not matter.
  */
  err= os_file_read(IORequestRead, fh, page_buf,
                    last_nonempty << page_size_shift, page_size, nullptr);
  if (err != DB_SUCCESS) {
    os_file_close(fh);
    return -1;
  }

  /* Now scan the last page to find the position in it to continue. */
  p= &page_buf[FIL_PAGE_DATA];
  page_end= &page_buf[page_size - FIL_PAGE_DATA_END];
  while (*p && p < page_end) {
    if (*p == FSP_BINLOG_TYPE_FILLER) {
      p= page_end;
      break;
    }
    p += 3 + (((uint32_t)p[2] << 8) | ((uint32_t)p[1] & 0xff));
    // ToDo: How to handle page corruption?
    ut_a(p <= page_end);
  }

  *out_page_no= p_0 - 1;
  *out_pos_in_page= (uint32_t)(p - page_buf);

  *out_space= fsp_binlog_open(file_name, fh, file_no, file_size, false);
  uint64_t pos= (*out_page_no << page_size_shift) | *out_pos_in_page;
  binlog_cur_written_offset[idx].store(pos, std::memory_order_relaxed);
  binlog_cur_end_offset[idx].store(pos, std::memory_order_relaxed);
  return (*out_space ? 1 : -1);
}


/*
  Returns:
    -1     error
     0     No binlogs found
     1     Just one binlog file found
     2     Found two (or more) existing binlog files
*/
static int
fsp_binlog_discover()
{
  uint64_t file_no;
  const uint32_t page_size= (uint32_t)srv_page_size;
  const uint32_t page_size_shift= (uint32_t)srv_page_size_shift;
  MY_DIR *dir= my_dir(".", MYF(MY_WME|MY_WANT_STAT));  // ToDo: configurable binlog directory, and don't ask my_dir to stat every file found
  if (!dir)
    return -1;

  struct found_binlogs binlog_files;
  binlog_files.found_binlogs= 0;
  size_t num_entries= dir->number_of_files;
  fileinfo *entries= dir-> dir_entry;
  for (size_t i= 0; i < num_entries; ++i) {
    const char *name= entries[i].name;
    uint64_t idx;
    if (!is_binlog_name(name, &idx))
      continue;
    process_binlog_name(&binlog_files, idx, entries[i].mystat->st_size);
  }
  my_dirend(dir);

  /*
    Now, if we found any binlog files, locate the point in one of them where
    binlogging stopped, and where we should continue writing new binlog data.
  */
  fil_space_t *space, *prev_space;
  uint32_t page_no, prev_page_no, pos_in_page, prev_pos_in_page;
  // ToDo: Do we need aligned_malloc() for page_buf, to be able to read a page into it (like IO_DIRECT maybe) ?
  std::unique_ptr<byte[]> page_buf(new byte[page_size]);
  if (!page_buf)
    return -1;
  if (binlog_files.found_binlogs >= 1) {
    int res= find_pos_in_binlog(binlog_files.last_file_no,
                                binlog_files.last_size,
                                page_buf.get(),
                                &space, &page_no, &pos_in_page);
    if (res < 0) {
      file_no= binlog_files.last_file_no;
      active_binlog_file_no.store(file_no, std::memory_order_release);
      ib::warn() << "Binlog number " << binlog_files.last_file_no <<
        " could no be opened. Starting a new binlog file from number " <<
        (file_no + 1) << ".";
      return 0;
    }

    if (res > 0) {
      /* Found start position in the last binlog file. */
      file_no= binlog_files.last_file_no;
      active_binlog_file_no.store(file_no, std::memory_order_release);
      active_binlog_space= space;
      binlog_cur_page_no= page_no;
      binlog_cur_page_offset= pos_in_page;
      ib::info() << "Continuing binlog number " << file_no << " from position "
                 << (((uint64_t)page_no << page_size_shift) | pos_in_page)
                 << ".";
      return binlog_files.found_binlogs;
    }

    /* res == 0, the last binlog is empty. */
    if (binlog_files.found_binlogs >= 2) {
      /* The last binlog is empty, try the previous one. */
      res= find_pos_in_binlog(binlog_files.prev_file_no,
                              binlog_files.prev_size,
                              page_buf.get(),
                              &prev_space, &prev_page_no, &prev_pos_in_page);
      if (res < 0) {
        file_no= binlog_files.last_file_no;
        active_binlog_file_no.store(file_no, std::memory_order_release);
        active_binlog_space= space;
        binlog_cur_page_no= page_no;
        binlog_cur_page_offset= pos_in_page;
        ib::warn() << "Binlog number " << binlog_files.prev_file_no
                   << " could not be opened, starting from binlog number "
                   << file_no << " instead." ;
        return 1;
      }
      file_no= binlog_files.prev_file_no;
      active_binlog_file_no.store(file_no, std::memory_order_release);
      active_binlog_space= prev_space;
      binlog_cur_page_no= prev_page_no;
      binlog_cur_page_offset= prev_pos_in_page;
      ib::info() << "Continuing binlog number " << file_no << " from position "
                 << (((uint64_t)prev_page_no << page_size_shift) |
                     prev_pos_in_page)
                 << ".";
      return binlog_files.found_binlogs;
    }

    /* Just one empty binlog file found. */
    file_no= binlog_files.last_file_no;
    active_binlog_file_no.store(file_no, std::memory_order_release);
    active_binlog_space= space;
    binlog_cur_page_no= page_no;
    binlog_cur_page_offset= pos_in_page;
    ib::info() << "Continuing binlog number " << file_no << " from position "
               << FIL_PAGE_DATA << ".";
    return binlog_files.found_binlogs;
  }

  /* No binlog files found, start from scratch. */
  file_no= 0;
  ib::info() << "Starting a new binlog from file number " << file_no << ".";
  return 0;
}


void fsp_binlog_close()
{
  if (binlog_prealloc_thr_obj.joinable()) {
    mysql_mutex_lock(&active_binlog_mutex);
    prealloc_thread_end= true;
    pthread_cond_signal(&active_binlog_cond);
    mysql_mutex_unlock(&active_binlog_mutex);
    binlog_prealloc_thr_obj.join();
  }

  uint64_t file_no= first_open_binlog_file_no;
  if (file_no != ~(uint64_t)0) {
    if (file_no <= last_created_binlog_file_no) {
      fsp_binlog_tablespace_close(file_no);
      if (file_no + 1 <= last_created_binlog_file_no) {
        fsp_binlog_tablespace_close(file_no + 1);
      }
    }
  }
  /*
    ToDo: This doesn't seem to free all memory. I'm still getting leaks in eg. --valgrind. Find out why and fix. Example:
==3464576==    at 0x48407B4: malloc (vg_replace_malloc.c:381)
==3464576==    by 0x15318CD: mem_strdup(char const*) (mem0mem.inl:452)
==3464576==    by 0x15321DF: fil_space_t::add(char const*, pfs_os_file_t, unsigned int, bool, bool, unsigned int) (fil0fil.cc:306)
==3464576==    by 0x1558445: fsp_binlog_tablespace_create(unsigned long) (fsp0fsp.cc:3900)
==3464576==    by 0x1558C70: fsp_binlog_write_cache(st_io_cache*, unsigned long, mtr_t*) (fsp0fsp.cc:4013)
  */
  binlog_diff_state.free();
  pthread_cond_destroy(&active_binlog_cond);
  mysql_mutex_destroy(&active_binlog_mutex);
}


/** Create a binlog tablespace file
@param[in]  file_no	 Index of the binlog tablespace
@param[out] new_space	 The newly created tablespace
@return DB_SUCCESS or error code */
dberr_t fsp_binlog_tablespace_create(uint64_t file_no, fil_space_t **new_space)
{
	pfs_os_file_t	fh;
	bool		ret;

        *new_space= nullptr;
	uint32_t size= binlog_size_in_pages;
	if(srv_read_only_mode)
		return DB_ERROR;

        char name[BINLOG_NAME_LEN];
        binlog_name_make(name, file_no);

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
        /* ToDo: Need to ensure file (N-2) is no longer active before creating (N). */
	uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);
	if (!(*new_space= fil_space_t::create(space_id,
                                                ( FSP_FLAGS_FCRC32_MASK_MARKER |
						  FSP_FLAGS_FCRC32_PAGE_SSIZE()),
						false, crypt_data,
						mode, true))) {
		mysql_mutex_unlock(&fil_system.mutex);
		os_file_close(fh);
		os_file_delete(innodb_data_file_key, name);
		return DB_ERROR;
	}

	fil_node_t* node = (*new_space)->add(name, fh, size, false, true);
	node->find_metadata();
	mysql_mutex_unlock(&fil_system.mutex);

	return DB_SUCCESS;
}


/*
  Background thread to close old binlog tablespaces and pre-allocate new ones.
*/
static void
fsp_binlog_prealloc_thread()
{

  mysql_mutex_lock(&active_binlog_mutex);
  while (1)
  {
    uint64_t active= active_binlog_file_no.load(std::memory_order_relaxed);
    uint64_t first_open= first_open_binlog_file_no;

    /* Pre-allocate the next tablespace (if not done already). */
    uint64_t last_created= last_created_binlog_file_no;
    if (last_created <= active && last_created <= first_open) {
      fil_space_t *new_space;
      ut_ad(last_created == active);
      ut_ad(last_created == first_open || first_open == ~(uint64_t)0);
      /*
        Note: `last_created` is initialized to ~0, so incrementing it here
        makes us start from binlog file 0.
      */
      ++last_created;
      mysql_mutex_unlock(&active_binlog_mutex);
      dberr_t res2= fsp_binlog_tablespace_create(last_created, &new_space);
      mysql_mutex_lock(&active_binlog_mutex);
      ut_a(res2 == DB_SUCCESS /* ToDo: Error handling. */);
      ut_a(new_space);
      last_created_binlog_file_no= last_created;
      last_created_binlog_space= new_space;

      /* If we created the initial tablespace file, make it the active one. */
      ut_ad(active < ~(uint64_t)0 || last_created == 0);
      if (active == ~(uint64_t)0) {
        active_binlog_file_no.store(last_created, std::memory_order_relaxed);
        active_binlog_space= last_created_binlog_space;
      }
      if (first_open == ~(uint64_t)0)
        first_open_binlog_file_no= first_open= last_created;

      pthread_cond_signal(&active_binlog_cond);
      continue;  /* Re-start loop after releasing/reacquiring mutex. */
    }

    /*
      Flush out to disk and close any binlog tablespace that has been
      completely written.
    */
    if (first_open < active) {
      ut_ad(first_open == active - 1);
      mysql_mutex_unlock(&active_binlog_mutex);
      fsp_binlog_tablespace_close(active - 1);
      mysql_mutex_lock(&active_binlog_mutex);
      first_open_binlog_file_no= first_open + 1;
      binlog_cur_end_offset[first_open & 1].store(~(uint64_t)0,
                                                  std::memory_order_relaxed);
      continue;  /* Re-start loop after releasing/reacquiring mutex. */
    }

    /* Exit thread at server shutdown. */
    if (prealloc_thread_end)
      break;
    my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);

  }
  mysql_mutex_unlock(&active_binlog_mutex);
}


__attribute__((noinline))
static ssize_t
serialize_gtid_state(rpl_binlog_state_base *state, byte *buf, size_t buf_size,
                     bool is_first_page)
{
  unsigned char *p= (unsigned char *)buf;
  /*
    1 uint64_t for the innodb_binlog_state_interval.
    1 uint64_t for the number of entries in the state stored.
    2 uint32_t + 1 uint64_t for at least one GTID.
  */
  ut_ad(buf_size >= 2*COMPR_INT_MAX32 + 3*COMPR_INT_MAX64);
  if (is_first_page) {
    /*
      In the first page where we put the full state, include the value of the
      setting for the interval at which differential states are binlogged, so
      we know how to search them independent of how the setting changes.
    */
    /* ToDo: Check that this current_binlog_state_interval is the correct value! */
    p= compr_int_write(p, current_binlog_state_interval);
  }
  p= compr_int_write(p, state->count_nolock());
  unsigned char * const pmax=
    p + (buf_size - (2*COMPR_INT_MAX32 + COMPR_INT_MAX64));

  if (state->iterate(
    [buf, buf_size, pmax, &p] (const rpl_gtid *gtid) {
      if (UNIV_UNLIKELY(p > pmax))
        return true;
      p= compr_int_write(p, gtid->domain_id);
      p= compr_int_write(p, gtid->server_id);
      p= compr_int_write(p, gtid->seq_no);
      return false;
    }))
    return -1;
  else
    return p - (unsigned char *)buf;
}


static bool
binlog_gtid_state(rpl_binlog_state_base *state, mtr_t *mtr,
                  buf_block_t * &block, uint32_t &page_no,
                  uint32_t &page_offset, fil_space_t *space)
{
  /*
    Use a small, efficient stack-allocated buffer by default, falling back to
    malloc() if needed for large GTID state.
  */
  byte small_buf[192];
  byte *buf, *alloced_buf;

  ssize_t used_bytes= serialize_gtid_state(state, small_buf, sizeof(small_buf),
                                           page_no==0);
  if (used_bytes >= 0)
  {
    buf= small_buf;
    alloced_buf= nullptr;
  }
  else
  {
    size_t buf_size=
      state->count_nolock() * (2*COMPR_INT_MAX32 + COMPR_INT_MAX64);
    /* ToDo: InnoDB alloc function? */
    alloced_buf= (byte *)my_malloc(PSI_INSTRUMENT_ME, buf_size, MYF(MY_WME));
    if (UNIV_UNLIKELY(!alloced_buf))
      return true;
    buf= alloced_buf;
    used_bytes= serialize_gtid_state(state, buf, buf_size, page_no==0);
    if (UNIV_UNLIKELY(used_bytes < 0))
    {
      ut_ad(0 /* Shouldn't happen, as we allocated maximum needed size. */);
      my_free(alloced_buf);
      return true;
    }
  }

  const uint32_t page_size= (uint32_t)srv_page_size;
  const uint32_t page_room= page_size - (FIL_PAGE_DATA + FIL_PAGE_DATA_END);
  uint32_t needed_pages= (uint32_t)((used_bytes + page_room - 1) / page_room);

  /* For now, GTID state always at the start of a page. */
  ut_ad(page_offset == FIL_PAGE_DATA);

  /*
    Only write the GTID state record if there is room for actual event data
    afterwards. There is no point in using space to allow fast search to a
    point if there is no data to search for after that point.
  */
  if (page_no + needed_pages < space->size)
  {
    byte cont_flag= 0;
    while (used_bytes > 0)
    {
      ut_ad(page_no < space->size);
      block= fsp_page_create(space, page_no, mtr);
      ut_a(block /* ToDo: error handling? */);
      page_offset= FIL_PAGE_DATA;
      byte *ptr= page_offset + block->page.frame;
      ssize_t chunk= used_bytes;
      byte last_flag= FSP_BINLOG_FLAG_LAST;
      if (chunk > page_room - 3) {
        last_flag= 0;
        chunk= page_room - 3;
        ++page_no;
      }
      ptr[0]= FSP_BINLOG_TYPE_GTID_STATE | cont_flag | last_flag;
      ptr[1] = (byte)chunk & 0xff;
      ptr[2] = (byte)(chunk >> 8);
      ut_ad(chunk <= 0xffff);
      memcpy(ptr+3, buf, chunk);
      mtr->memcpy(*block, page_offset, chunk+3);
      page_offset+= (uint32_t)(chunk+3);
      buf+= chunk;
      used_bytes-= chunk;
      cont_flag= FSP_BINLOG_FLAG_CONT;
    }

    if (page_offset == FIL_PAGE_DATA_END) {
      block= nullptr;
      page_offset= FIL_PAGE_DATA;
      ++page_no;
    }
  }
  my_free(alloced_buf);

  /* Make sure we return a page for caller to write the main event data into. */
  if (UNIV_UNLIKELY(!block)) {
    block= fsp_page_create(space, page_no, mtr);
    ut_a(block /* ToDo: error handling? */);
  }

  return false;  // No error
}


/*
  Read a binlog state record from a page in a buffer. The passed in STATE
  object is updated with the state read.

  Returns:
    1  State record found
    0  No state record found
    -1 Error
*/
static int
read_gtid_state_from_page(rpl_binlog_state_base *state, const byte *page,
                          uint32_t page_no, uint64_t *out_diff_state_interval)
{
  const byte *p= page + FIL_PAGE_DATA;
  byte t= *p;
  if (UNIV_UNLIKELY((t & FSP_BINLOG_TYPE_MASK) != FSP_BINLOG_TYPE_GTID_STATE))
    return 0;
  /* ToDo: Handle reading a state that spans multiple pages. For now, we assume the state fits in a single page. */
  ut_a(t & FSP_BINLOG_FLAG_LAST);

  uint32_t len= ((uint32_t)p[2] << 8) | p[1];
  const byte *p_end= p + 3 + len;
  if (UNIV_UNLIKELY(p + 3 >= p_end))
    return -1;
  std::pair<uint64_t, const unsigned char *> v_and_p= compr_int_read(p + 3);
  p= v_and_p.second;
  if (page_no == 0)
  {
    /*
      The state in the first page has an extra word, the offset between
      differential binlog states logged regularly in the binlog tablespace.
    */
    *out_diff_state_interval= v_and_p.first;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    p= v_and_p.second;
  }
  else
    *out_diff_state_interval= 0;

  if (UNIV_UNLIKELY(p > p_end))
    return -1;

  for (uint64_t count= v_and_p.first; count > 0; --count)
  {
    rpl_gtid gtid;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    if (UNIV_UNLIKELY(v_and_p.first > UINT32_MAX))
      return -1;
    gtid.domain_id= (uint32_t)v_and_p.first;
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    if (UNIV_UNLIKELY(v_and_p.first > UINT32_MAX))
      return -1;
    gtid.server_id= (uint32_t)v_and_p.first;
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p >= p_end))
      return -1;
    v_and_p= compr_int_read(p);
    gtid.seq_no= v_and_p.first;
    p= v_and_p.second;
    if (UNIV_UNLIKELY(p > p_end))
      return -1;
    if (state->update_nolock(&gtid))
      return -1;
  }

  /*
    For now, we expect no more data.
    Later it could be extended, as we store (and read) the count of GTIDs.
  */
  ut_ad(p == p_end);

  return 1;
}


/*
  Read a binlog state record from a specific page in a file. The passed in
  STATE object is updated with the state read.

  Returns:
    1  State record found
    0  No state record found
    -1 Error
*/
static int
read_gtid_state(rpl_binlog_state_base *state, File file, uint32_t page_no,
                uint64_t *out_diff_state_interval)
{
  std::unique_ptr<byte [], void (*)(void *)> page_buf
    ((byte *)my_malloc(PSI_NOT_INSTRUMENTED, srv_page_size, MYF(MY_WME)),
     &my_free);
  if (UNIV_UNLIKELY(!page_buf))
    return -1;

  /* ToDo: Verify checksum, and handle encryption. */
  size_t res= my_pread(file, page_buf.get(), srv_page_size,
                       (uint64_t)page_no << srv_page_size_shift, MYF(MY_WME));
  if (UNIV_UNLIKELY(res == (size_t)-1))
    return -1;

  return read_gtid_state_from_page(state, page_buf.get(), page_no,
                                   out_diff_state_interval);
}


/*
  Recover the GTID binlog state at startup.
  Read the full binlog state at the start of the current binlog file, as well
  as the last differential binlog state on top, if any. Then scan from there to
  the end to obtain the exact current GTID binlog state.

  Return false if ok, true if error.
*/
static bool
binlog_state_recover()
{
  rpl_binlog_state_base state;
  state.init();
  uint64_t diff_state_interval= 0;
  uint32_t page_no= 0;
  char filename[BINLOG_NAME_LEN];

  binlog_name_make(filename,
                   active_binlog_file_no.load(std::memory_order_relaxed));
  File file= my_open(filename, O_RDONLY | O_BINARY, MYF(MY_WME));
  if (UNIV_UNLIKELY(file < (File)0))
    return true;

  int res= read_gtid_state(&state, file, page_no, &diff_state_interval);
  if (res < 0)
  {
    my_close(file, MYF(0));
    return true;
  }
  if (diff_state_interval == 0 || diff_state_interval % srv_page_size != 0)
  {
    ib::warn() << "Invalid differential binlog state interval " <<
      diff_state_interval << " found in binlog file, ignoring";
    current_binlog_state_interval= 0;  /* Disable in this binlog file */
  }
  else
  {
    current_binlog_state_interval= diff_state_interval;
    diff_state_interval>>= srv_page_size_shift;
    page_no= (uint32_t)(binlog_cur_page_no -
                        (binlog_cur_page_no % diff_state_interval));
    while (page_no > 0)
    {
      uint64_t dummy_interval;
      res= read_gtid_state(&state, file, page_no, &dummy_interval);
      if (res > 0)
        break;
      page_no-= (uint32_t)diff_state_interval;
    }
  }
  my_close(file, MYF(0));

  ha_innodb_binlog_reader reader(active_binlog_file_no.load
                                   (std::memory_order_relaxed),
                                 page_no << srv_page_size_shift);
  return binlog_recover_gtid_state(&state, &reader);
}


std::pair<uint64_t, uint64_t>
fsp_binlog_write_chunk(chunk_data_base *chunk_data, mtr_t *mtr, byte chunk_type)
{
  uint32_t page_size= (uint32_t)srv_page_size;
  uint32_t page_size_shift= srv_page_size_shift;
  fil_space_t *space= active_binlog_space;
  const uint32_t page_end= page_size - FIL_PAGE_DATA_END;
  uint32_t page_no= binlog_cur_page_no;
  uint32_t page_offset= binlog_cur_page_offset;
  /* ToDo: What is the lifetime of what's pointed to by binlog_cur_block, is there some locking needed around it or something? */
  buf_block_t *block= binlog_cur_block;
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);
  uint64_t pending_prev_end_offset= 0;
  uint64_t start_file_no= 0;
  uint64_t start_offset= 0;

  /*
    Write out the event data in chunks of whatever size will fit in the current
    page, until all data has been written.
  */
  byte cont_flag= 0;
  for (;;) {
    if (page_offset == FIL_PAGE_DATA) {
      if (UNIV_UNLIKELY(page_no >= space->size)) {
        /*
          Signal to the pre-allocation thread that this tablespace has been
          written full, so that it can be closed and a new one pre-allocated
          in its place. Then wait for a new tablespace to be pre-allocated that
          we can use.

          The normal case is that the next tablespace is already pre-allocated
          and available; binlog tablespace N is active while (N+1) is being
          pre-allocated. Only under extreme I/O pressure should be need to
          stall here.

          ToDo: Handle recovery. Idea: write the current LSN at the start of
          the binlog tablespace when we create it. At recovery, we should open
          the (at most) 2 most recent binlog tablespaces. Whenever we have a
          redo record, skip it if its LSN is smaller than the one stored in the
          tablespace corresponding to its space_id. This way, it should be safe
          to re-use tablespace ids between just two, SRV_SPACE_ID_BINLOG0 and
          SRV_SPACE_ID_BINLOG1.
        */
        pending_prev_end_offset= page_no << page_size_shift;
        mysql_mutex_lock(&active_binlog_mutex);
        /* ToDo: Make this wait killable?. */
        /* ToDo2: Handle not stalling infinitely if the new tablespace cannot be created due to eg. I/O error. Or should we in this case loop and repeatedly retry the create? */
        while (last_created_binlog_file_no <= file_no) {
          my_cond_wait(&active_binlog_cond, &active_binlog_mutex.m_mutex);
        }

        // ToDo: assert that a single write doesn't span more than two binlog files.
        ++file_no;
        binlog_cur_written_offset[file_no & 1].store(0, std::memory_order_relaxed);
        binlog_cur_end_offset[file_no & 1].store(0, std::memory_order_relaxed);
        active_binlog_file_no.store(file_no, std::memory_order_release);
        active_binlog_space= space= last_created_binlog_space;
        pthread_cond_signal(&active_binlog_cond);
        mysql_mutex_unlock(&active_binlog_mutex);
        binlog_cur_page_no= page_no= 0;
        /* ToDo: Here we must use the value from the file, if this file was pre-allocated before a server restart where the value of innodb_binlog_state_interval changed. Maybe just make innodb_binlog_state_interval dynamic and make the prealloc thread (and discover code at startup) supply the correct value to use for each file. */
        current_binlog_state_interval= innodb_binlog_state_interval;
      }

      /* Must be a power of two and larger than page size. */
      ut_ad(current_binlog_state_interval == 0 ||
            current_binlog_state_interval > page_size);
      ut_ad(current_binlog_state_interval == 0 ||
            current_binlog_state_interval ==
            (uint64_t)1 << (63 - nlz(current_binlog_state_interval)));

      if (0 == (page_no &
                ((current_binlog_state_interval >> page_size_shift) - 1))) {
        if (page_no == 0) {
          rpl_binlog_state_base full_state;
          bool err;
          full_state.init();
          err= load_global_binlog_state(&full_state);
          ut_a(!err /* ToDo error handling */);
          if (UNIV_UNLIKELY(file_no == 0 && page_no == 0) &&
              (full_state.count_nolock() == 1))
          {
            /*
              The gtid state written here includes the GTID for the event group
              currently being written. This is precise when the event group
              data begins before this point. If the event group happens to
              start exactly on a binlog file boundary, it just means we will
              have to read slightly more binlog data to find the starting point
              of that GTID.

              But there is an annoying case if this is the very first binlog
              file created (no migration from legacy binlog). If we start the
              binlog with some GTID 0-1-1 and write the state "0-1-1" at the
              start of the first file, then we will be unable to start
              replicating from the GTID position "0-1-1", corresponding to the
              *second* event group in the binlog. Because there will be no
              slightly earlier point to start reading from!

              So we put a slightly awkward special case here to handle that: If
              at the start of the first file we have a singleton gtid state
              with seq_no=1, D-S-1, then it must be the very first GTID in the
              entire binlog, so we write an *empty* gtid state that will always
              allow to start replicating from the very start of the binlog.

              (If the user would explicitly set the seq_no of the very first
              GTID in the binlog greater than 1, then starting from that GTID
              position will still not be possible).
            */
            rpl_gtid singleton_gtid;
            full_state.get_gtid_list_nolock(&singleton_gtid, 1);
            if (singleton_gtid.seq_no == 1)
            {
              full_state.reset_nolock();
            }
          }
          err= binlog_gtid_state(&full_state, mtr, block, page_no,
                                 page_offset, space);
          ut_a(!err /* ToDo error handling */);
          ut_ad(block);
          full_state.free();
          binlog_diff_state.reset_nolock();
        } else {
          bool err= binlog_gtid_state(&binlog_diff_state, mtr, block, page_no,
                                      page_offset, space);
          ut_a(!err /* ToDo error handling */);
        }
      } else
        block= fsp_page_create(space, page_no, mtr);
    } else {
      dberr_t err;
      /* ToDo: Is RW_SX_LATCH appropriate here? */
      block= buf_page_get_gen(page_id_t{space->id, page_no},
                              0, RW_SX_LATCH, block,
                              BUF_GET, mtr, &err);
      ut_a(err == DB_SUCCESS);
    }

    ut_ad(page_offset < page_end);
    uint32_t page_remain= page_end - page_offset;
    byte *ptr= page_offset + block->page.frame;
    /* ToDo: Do this check at the end instead, to save one buf_page_get_gen()? */
    if (page_remain < 4) {
      /* Pad the remaining few bytes, and move to next page. */
      mtr->memset(block, page_offset, page_remain, FSP_BINLOG_TYPE_FILLER);
      block= nullptr;
      ++page_no;
      page_offset= FIL_PAGE_DATA;
      continue;
    }
    if (start_offset == 0)
    {
      start_file_no= file_no;
      start_offset= (page_no << page_size_shift) + page_offset;
    }
    page_remain-= 3;    /* Type byte and 2-byte length. */
    std::pair<uint32_t, bool> size_last=
      chunk_data->copy_data(ptr+3, page_remain);
    uint32_t size= size_last.first;
    ut_ad(size_last.second || size == page_remain);
    ut_ad(size <= page_remain);
    page_remain-= size;
    byte last_flag= size_last.second ? FSP_BINLOG_FLAG_LAST : 0;
    ptr[0]= chunk_type | cont_flag | last_flag;
    ptr[1]= size & 0xff;
    ptr[2]= (byte)(size >> 8);
    ut_ad(size <= 0xffff);

    mtr->memcpy(*block, page_offset, size+3);
    cont_flag= FSP_BINLOG_FLAG_CONT;
    if (page_remain == 0) {
      block= nullptr;
      page_offset= FIL_PAGE_DATA;
      ++page_no;
    } else {
      page_offset+= size+3;
    }
    if (size_last.second)
      break;
  }
  binlog_cur_block= block;
  binlog_cur_page_no= page_no;
  binlog_cur_page_offset= page_offset;
  if (UNIV_UNLIKELY(pending_prev_end_offset))
    binlog_cur_end_offset[(file_no-1) & 1].store(pending_prev_end_offset,
                                                 std::memory_order_relaxed);
  binlog_cur_end_offset[file_no & 1].store((page_no << page_size_shift) + page_offset,
                                           std::memory_order_relaxed);
  return {start_file_no, start_offset};
}


struct chunk_data_cache : public chunk_data_base {
  IO_CACHE *cache;
  size_t main_remain;
  size_t gtid_remain;
  uint32_t header_remain;
  uint32_t header_sofar;
  byte header_buf[5*COMPR_INT_MAX64];

  chunk_data_cache(IO_CACHE *cache_arg,
                   handler_binlog_event_group_info *binlog_info)
  : cache(cache_arg),
    main_remain(binlog_info->gtid_offset - binlog_info->out_of_band_offset),
    header_sofar(0)
  {
    size_t end_offset= my_b_tell(cache);
    size_t remain= end_offset - binlog_info->out_of_band_offset;
    ut_ad(remain > 0);
    ut_ad(binlog_info->gtid_offset >= binlog_info->out_of_band_offset);
    ut_ad(end_offset >= binlog_info->gtid_offset);
    gtid_remain= end_offset - binlog_info->gtid_offset;

    binlog_oob_context *c= (binlog_oob_context *)binlog_info->engine_ptr;
    unsigned char *p;
    if (c && c->node_list_len)
    {
      /*
        Link to the out-of-band data. First store the number of nodes; then
        store 2 x 2 numbers of file_no/offset for the first and last node.
      */
      uint32_t last= c->node_list_len-1;
      uint64_t num_nodes= c->node_list[last].node_index + 1;
      p= compr_int_write(header_buf, num_nodes);
      p= compr_int_write(p, c->first_node_file_no);
      p= compr_int_write(p, c->first_node_offset);
      p= compr_int_write(p, c->node_list[last].file_no);
      p= compr_int_write(p, c->node_list[last].offset);
    }
    else
    {
      /*
        No out-of-band data, marked with a single 0 count for nodes and no
        first/last links.
      */
      p= compr_int_write(header_buf, 0);
    }
    header_remain= (uint32_t)(p - header_buf);
    ut_ad((size_t)(p - header_buf) <= sizeof(header_buf));

    if (cache->pos_in_file > binlog_info->out_of_band_offset) {
      /*
        ToDo: A limitation in mysys IO_CACHE. If I change (reinit_io_cache())
        the cache from WRITE_CACHE to READ_CACHE without seeking out of the
        current buffer, then the cache will not be flushed to disk (which is
        good for small cache that fits completely in buffer). But then if I
        later my_b_seek() or reinit_io_cache() it again and seek out of the
        current buffer, the buffered data will not be flushed to the file
        because the cache is now a READ_CACHE! The result is that the end of the
        cache will be lost if the cache doesn't fit in memory.

        So for now, have to do this somewhat in-elegant conditional flush
        myself.
      */
      flush_io_cache(cache);
    }

    /* Start with the GTID event, which is put at the end of the IO_CACHE. */
    my_bool res= reinit_io_cache(cache, READ_CACHE, binlog_info->gtid_offset, 0, 0);
    ut_a(!res /* ToDo: Error handling. */);
  }
  ~chunk_data_cache() { }

  virtual std::pair<uint32_t, bool> copy_data(byte *p, uint32_t max_len) final
  {
    uint32_t size= 0;
    /* Write header data, if any still available. */
    if (header_remain > 0)
    {
      size= header_remain > max_len ? max_len : (uint32_t)header_remain;
      memcpy(p, header_buf + header_sofar, size);
      header_remain-= size;
      header_sofar+= size;
      max_len-= size;
      if (UNIV_UNLIKELY(max_len == 0))
      {
        ut_ad(gtid_remain + main_remain > 0);
        return {size, false};
      }
    }

    /* Write GTID data, if any still available. */
    ut_ad(header_remain == 0);
    if (gtid_remain > 0)
    {
      uint32_t size2= gtid_remain > max_len ? max_len : (uint32_t)gtid_remain;
      int res2= my_b_read(cache, p, size2);
      ut_a(!res2 /* ToDo: Error handling */);
      gtid_remain-= size2;
      if (gtid_remain == 0)
        my_b_seek(cache, 0);    /* Move to read the rest of the events. */
      max_len-= size2;
      size+= size2;
      if (max_len == 0)
        return {size, gtid_remain + main_remain == 0};
    }

    /* Write remaining data. */
    ut_ad(gtid_remain == 0);
    if (main_remain == 0)
    {
      /*
        This means that only GTID data is present, eg. when the main data was
        already binlogged out-of-band.
      */
      ut_ad(size > 0);
      return {size, true};
    }
    uint32_t size2= main_remain > max_len ? max_len : (uint32_t)main_remain;
    int res2= my_b_read(cache, p + size, size2);
    ut_a(!res2 /* ToDo: Error handling */);
    ut_ad(main_remain >= size2);
    main_remain-= size2;
    return {size + size2, main_remain == 0};
  }
};


static void
fsp_binlog_write_cache(IO_CACHE *cache,
                       handler_binlog_event_group_info *binlog_info, mtr_t *mtr)
{
  chunk_data_cache chunk_data(cache, binlog_info);
  fsp_binlog_write_chunk(&chunk_data, mtr, FSP_BINLOG_TYPE_COMMIT);
}


/* Allocate a context for out-of-band binlogging. */
static binlog_oob_context *
alloc_oob_context(uint32 list_length)
{
  size_t needed= sizeof(binlog_oob_context) +
    list_length * sizeof(binlog_oob_context::node_info);
  binlog_oob_context *c=
    (binlog_oob_context *) ut_malloc(needed, mem_key_binlog);
  if (c)
  {
    c->node_list_alloc_len= list_length;
    c->node_list_len= 0;
  }
  else
    my_error(ER_OUTOFMEMORY, MYF(0), needed);

  return c;
}


static inline void
free_oob_context(binlog_oob_context *c)
{
  ut_free(c);
}


static binlog_oob_context *
ensure_oob_context(void **engine_data, uint32_t needed_len)
{
  binlog_oob_context *c= (binlog_oob_context *)*engine_data;
  if (c->node_list_alloc_len >= needed_len)
    return c;
  if (needed_len < c->node_list_alloc_len + 10)
    needed_len= c->node_list_alloc_len + 10;
  binlog_oob_context *new_c= alloc_oob_context(needed_len);
  if (UNIV_UNLIKELY(!new_c))
    return nullptr;
  memcpy(new_c, c, sizeof(binlog_oob_context) +
         needed_len*sizeof(binlog_oob_context::node_info));
  new_c->node_list_alloc_len= needed_len;
  *engine_data= new_c;
  free_oob_context(c);
  return new_c;
}


/*
  Binlog an out-of-band piece of event group data.

  For large transactions, we binlog the data in pieces spread out over the
  binlog file(s), to avoid a large stall to write large amounts of data during
  transaction commit, and to avoid having to keep all of the transaction in
  memory or spill it to temporary file.

  The chunks of data are written out in a binary tree structure, to allow
  efficiently reading the transaction back in order from start to end. Note
  that the binlog is written append-only, so we cannot simply link each chunk
  to the following chunk, as the following chunk is unknown when binlogging the
  prior chunk. With a binary tree structure, the reader can do a post-order
  traversal and only need to keep log_2(N) node pointers in-memory at any time.

  A perfect binary tree of height h has 2**h - 1 nodes. At any time during a
  transaction, the out-of-band data in the binary log for that transaction
  consists of a forest (eg. a list) of perfect binary trees of strictly
  decreasing height, except that the last two trees may have the same height.
  For example, here is how it looks for a transaction where 13 nodes (0-12)
  have been binlogged out-of-band so far:

          6
       _ / \_
      2      5      9     12
     / \    / \    / \    / \
    0   1  3   4  7   8 10  11

  In addition to the shown binary tree parent->child pointers, each leaf has a
  (single) link to the root node of the prior (at the time the leaf was added)
  tree. In the example this means the following links:
    11->10, 10->9, 8->7, 7->6, 4->3, 3->2, 1->0
  This allows to fully traverse the forest of perfect binary trees starting
  from the last node (12 in the example). In the example, only 10->9 and 7->6
  will be needed, but the other links would be needed if the tree had been
  completed at earlier stages.

  As a new node is added, there are two different cases on how to maintain
  the binary tree forest structure:

    1. If the last two trees in the forest have the same height h, then those
       two trees are replaced by a single tree of height (h+1) with the new
       node as root and the two trees as left and right child. The number of
       trees in the forest thus decrease by one.

    2. Otherwise the new node is added at the end of the forest as a tree of
       height 1; in this case the forest increases by one tree.

  In both cases, we maintain the invariants that the forest consist of a list
  of perfect binary trees, and that the heights of the trees are strictly
  decreasing except that the last two trees can have the same height.

  When a transaction is committed, the commit record contains a pointer to
  the root node of the last tree in the forest. If the transaction is never
  committed (explicitly rolled back or lost due to disconnect or server
  restart or crash), then the out-of-band data is simply left in place; it
  will be ignored by readers and eventually discarded as the old binlog files
  are purged.
*/
bool
fsp_binlog_oob(THD *thd, const unsigned char *data, size_t data_len,
               void **engine_data)
{
  binlog_oob_context *c= (binlog_oob_context *)*engine_data;
  if (!c)
    *engine_data= c= alloc_oob_context(10);
  if (UNIV_UNLIKELY(!c))
    return true;

  uint32_t i= c->node_list_len;
  uint64_t new_idx= i==0 ? 0 : c->node_list[i-1].node_index + 1;
  if (i >= 2 && c->node_list[i-2].height == c->node_list[i-1].height)
  {
    /* Case 1: Replace two trees with a tree rooted in a new node. */
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx,
       c->node_list[i-2].file_no, c->node_list[i-2].offset,
       c->node_list[i-1].file_no, c->node_list[i-1].offset,
       (byte *)data, data_len);
    if (c->binlog_node(i-2, new_idx, i-2, i-1, &oob_data))
      return true;
    c->node_list_len= i - 1;
  }
  else if (i > 0)
  {
    /* Case 2: Add the new node as a singleton tree. */
    c= ensure_oob_context(engine_data, i+1);
    if (!c)
      return true;
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx,
       0, 0, /* NULL left child signifies a leaf */
       c->node_list[i-1].file_no, c->node_list[i-1].offset,
       (byte *)data, data_len);
    if (c->binlog_node(i, new_idx, i-1, i-1, &oob_data))
      return true;
    c->node_list_len= i + 1;
  }
  else
  {
    /* Special case i==0, like case 2 but no prior node to link to. */
    binlog_oob_context::chunk_data_oob oob_data
      (new_idx, 0, 0, 0, 0, (byte *)data, data_len);
    if (c->binlog_node(i, new_idx, ~(uint32_t)0, ~(uint32_t)0, &oob_data))
      return true;
    c->first_node_file_no= c->node_list[i].file_no;
    c->first_node_offset= c->node_list[i].offset;
    c->node_list_len= 1;
  }

  return false;
}


/*
  Binlog a new out-of-band tree node and put it at position `node` in the list
  of trees. A leaf node is denoted by left and right child being identical (and
  in this case they point to the root of the prior tree).
*/
bool
binlog_oob_context::binlog_node(uint32_t node, uint64_t new_idx,
                                uint32_t left_node, uint32_t right_node,
                                chunk_data_oob *oob_data)
{
  uint32_t new_height=
    left_node == right_node ? 1 : 1 + node_list[left_node].height;
  mtr_t mtr;
  mtr.start();
  std::pair<uint64_t, uint64_t> new_file_no_offset=
    fsp_binlog_write_chunk(oob_data, &mtr, FSP_BINLOG_TYPE_OOB_DATA);
  mtr.commit();
  node_list[node].file_no= new_file_no_offset.first;
  node_list[node].offset= new_file_no_offset.second;
  node_list[node].node_index= new_idx;
  node_list[node].height= new_height;
  return false;  // ToDo: Error handling?
}


binlog_oob_context::chunk_data_oob::chunk_data_oob(uint64_t idx,
        uint64_t left_file_no, uint64_t left_offset,
        uint64_t right_file_no, uint64_t right_offset,
        byte *data, size_t data_len)
  : sofar(0), main_len(data_len), main_data(data)
{
  ut_ad(data_len > 0);
  byte *p= &header_buf[0];
  p= compr_int_write(p, idx);
  p= compr_int_write(p, left_file_no);
  p= compr_int_write(p, left_offset);
  p= compr_int_write(p, right_file_no);
  p= compr_int_write(p, right_offset);
  ut_ad(p - &header_buf[0] <= max_buffer);
  header_len= (uint32_t)(p - &header_buf[0]);
}


std::pair<uint32_t, bool>
binlog_oob_context::chunk_data_oob::copy_data(byte *p, uint32_t max_len)
{
  uint32_t size= 0;
  /* First write header data, if any left. */
  if (sofar < header_len)
  {
    size= std::min(header_len - (uint32_t)sofar, max_len);
    memcpy(p, header_buf + sofar, size);
    p+= size;
    sofar+= size;
    if (UNIV_UNLIKELY(max_len == size))
      return {size, sofar == header_len + main_len};
    max_len-= size;
  }

  /* Then write the main chunk data. */
  ut_ad(sofar >= header_len);
  ut_ad(main_len > 0);
  uint32_t size2=
    (uint32_t)std::min(header_len + main_len - sofar, (uint64_t)max_len);
  memcpy(p, main_data + (sofar - header_len), size2);
  sofar+= size2;
  return {size + size2, sofar == header_len + main_len};
}


void
fsp_free_oob(THD *thd, void *engine_data)
{
  free_oob_context((binlog_oob_context *)engine_data);
}


extern "C" void binlog_get_cache(THD *, IO_CACHE **,
                                 handler_binlog_event_group_info **,
                                 const rpl_gtid **);

void
fsp_binlog_trx(trx_t *trx, mtr_t *mtr)
{
  IO_CACHE *cache;
  handler_binlog_event_group_info *binlog_info;
  const rpl_gtid *gtid;

  if (!trx->mysql_thd)
    return;
  binlog_get_cache(trx->mysql_thd, &cache, &binlog_info, &gtid);
  if (UNIV_LIKELY(binlog_info != nullptr) &&
      UNIV_LIKELY(binlog_info->gtid_offset > 0)) {
    binlog_diff_state.update_nolock(gtid);
    fsp_binlog_write_cache(cache, binlog_info, mtr);
  }
}


ha_innodb_binlog_reader::ha_innodb_binlog_reader(uint64_t file_no,
                                                 uint64_t offset)
  : chunk_pos(0), chunk_remain(0), skipping_partial(true)
{
  page_buf= (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, srv_page_size, MYF(0)); /* ToDo: InnoDB alloc function? */
  // ToDo: Need some mechanism to find where to start reading. This is just "start from 0" for early testing.
  cur_file_no= file_no;
  cur_file_offset= offset;
}


ha_innodb_binlog_reader::~ha_innodb_binlog_reader()
{
  if (cur_file != (File)-1)
    my_close(cur_file, MYF(0));
  my_free(page_buf); /* ToDo: InnoDB alloc function? */
}


bool
ha_innodb_binlog_reader::ensure_file_open()
{
  if (cur_file != (File)-1)
    return false;
  char filename[BINLOG_NAME_LEN];
  binlog_name_make(filename, cur_file_no);
  cur_file= my_open(filename, O_RDONLY | O_BINARY, MYF(MY_WME));
  if (UNIV_UNLIKELY(cur_file < (File)0)) {
    cur_file= (File)-1;
    return true;
  }
  MY_STAT stat_buf;
  if (my_fstat(cur_file, &stat_buf, MYF(0))) {
    my_error(ER_CANT_GET_STAT, MYF(0), filename, errno);
    my_close(cur_file, MYF(0));
    cur_file= (File)-1;
    return true;
  }
  cur_file_length= stat_buf.st_size;
  return false;
}


void
ha_innodb_binlog_reader::next_file()
{
  if (cur_file != (File)-1) {
    my_close(cur_file, MYF(0));
    cur_file= (File)-1;
  }
  ++cur_file_no;
  cur_file_offset= 0;
}


/*
  Read data from current position in binlog.

  If the data is written to disk (visible at the OS level, even if not
  necessarily fsync()'ed to disk), we can read directly from the file.
  Otherwise, the data must still be available in the buffer pool and
  we can read it from there.

  First try a dirty read of current state; if this says the data is available
  to read from the file, this is safe to do (data cannot become un-written).

  If not, then check if the page is in the buffer pool; if not, then likewise
  we know it's safe to read from the file directly.

  Finally, do another check of the current state. This will catch the case
  where we looked for a page in binlog file N, but its tablespace id has been
  recycled, so we got a page from (N+2) instead. In this case also, we can
  then read from the real file.
*/
int ha_innodb_binlog_reader::read_binlog_data(uchar *buf, uint32_t len)
{
  int res;

  /*
    Loop repeatedly trying to read some data from a page.
    The usual case is that just one iteration of the loop is necessary. But
    occasionally more may be needed, for example when moving to the next
    binlog file or when a page has no replication event data to read.
  */
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  for (;;) {
    buf_block_t *block= nullptr;
    mtr_t mtr;
    bool mtr_started= false;
    uint64_t active= active2;
    uint64_t end_offset=
      binlog_cur_end_offset[cur_file_no&1].load(std::memory_order_acquire);
    ut_ad(cur_file_no <= active);

    if (cur_file_no + 1 >= active) {
      /* Check if we should read from the buffer pool or from the file. */
      if (end_offset != ~(uint64_t)0 && cur_file_offset < end_offset) {
        mtr.start();
        mtr_started= true;
        /*
          ToDo: Should we keep track of the last block read and use it as a
          hint? Will be mainly useful when reading the partially written active
          page at the current end of the active binlog, which might be a common
          case.
        */
        buf_block_t *hint_block= nullptr;
        uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (cur_file_no & 1);
        uint32_t page_no= (uint32_t)(cur_file_offset >> srv_page_size_shift);
        dberr_t err= DB_SUCCESS;
        block= buf_page_get_gen(page_id_t{space_id, page_no}, 0,
                                RW_S_LATCH, hint_block, BUF_GET_IF_IN_POOL,
                                &mtr, &err);
        if (err != DB_SUCCESS) {
          mtr.commit();
          res= -1;
          break;
        }
      }
      active2= active_binlog_file_no.load(std::memory_order_acquire);
      if (UNIV_UNLIKELY(active2 != active)) {
        /*
          The active binlog file changed while we were processing; we might
          have gotten invalid end_offset or a buffer pool page from a wrong
          tablespace. So just try again.
        */
        if (mtr_started)
          mtr.commit();
        continue;
      }
      if (cur_file_offset >= end_offset) {
        ut_ad(!mtr_started);
        if (cur_file_no == active) {
          /* Reached end of the currently active binlog file -> EOF. */
          res= 0;
          break;
        }
        /* End of file reached, move to next file. */
        /*
          ToDo: Should not read data and send to slaves that has not yet been
          durably synced to disk, at least optionally, lest a crash leaves the
          slaves with transactions that do not (any longer) exist on master
          and breaks replication.
        */
        /*
          ToDo: Should also obey binlog_cur_written_offset[], once we start
          actually maintaining that, to save unnecessary buffer pool
          lookup.
        */
        next_file();
        continue;
      }
      if (block) {
        res= read_from_buffer_pool_page(block, end_offset, buf, len);
        ut_ad(mtr_started);
        if (mtr_started)
          mtr.commit();
      } else {
        /* Not in buffer pool, just read it from the file. */
        if (mtr_started)
          mtr.commit();
        if (ensure_file_open()) {
          res= -1;
          break;
        }
        ut_ad(cur_file_offset < end_offset);
        if (cur_file_offset >= cur_file_length) {
          /*
            This happens when we reach the end of (active-1) and the tablespace
            has been closed.
          */
          ut_ad(end_offset == ~(uint64_t)0);
          ut_ad(!mtr_started);
          next_file();
          continue;
        }
        res= read_from_file(end_offset, buf, len);
      }
    } else {
      /* Tablespace is not open, just read from the file. */
      if (ensure_file_open()) {
        res= -1;
        break;
      }
      if (cur_file_offset >= cur_file_length) {
        /* End of this file, move to the next one. */
        next_file();
        continue;
      }
      res= read_from_file(cur_file_length, buf, len);
    }

    /* If nothing read, but not eof/error, then loop to try the next page. */
    if (res != 0)
      break;
  }

  return res;
}


bool
ha_innodb_binlog_reader::data_available()
{
  uint64_t active= active_binlog_file_no.load(std::memory_order_acquire);
  if (active != cur_file_no)
  {
    ut_ad(active > cur_file_no);
    return true;
  }
  uint64_t end_offset=
    binlog_cur_end_offset[cur_file_no&1].load(std::memory_order_acquire);
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  if (active2 != active || end_offset > cur_file_offset)
    return true;
  ut_ad(cur_file_no == active2);
  ut_ad(cur_file_offset == end_offset);
  return false;
}


int
ha_innodb_binlog_reader::read_from_buffer_pool_page(buf_block_t *block,
                                                    uint64_t end_offset,
                                                    uchar *buf, uint32_t len)
{
  return read_from_page(block->page.frame, end_offset, buf, len);
}


int
ha_innodb_binlog_reader::read_from_file(uint64_t end_offset,
                                        uchar *buf, uint32_t len)
{
  uint64_t mask= ((uint64_t)1 << srv_page_size_shift) - 1;
  uint64_t offset= cur_file_offset;
  uint64_t page_start_offset;

  ut_ad(cur_file != (File)-1);
  ut_ad(cur_file_offset < cur_file_length);

  page_start_offset= offset & ~mask;
  size_t res= my_pread(cur_file, page_buf, srv_page_size, page_start_offset,
                       MYF(MY_WME));
  if (res == (size_t)-1)
    return -1;

  return read_from_page(page_buf, end_offset, buf, len);
}


/*
  Read out max `len` bytes from the chunks stored in a page.

  page_ptr   Points to start of data for current page matching cur_file_offset
  end_offset Current end of binlog file, no reads past this point
  buf        Destination buffer to read into
  len        Maximum number of bytes to read

  Returns number of bytes actually read.
*/
int
ha_innodb_binlog_reader::read_from_page(uchar *page_ptr, uint64_t end_offset,
                                        uchar *buf, uint32_t len)
{
  uint32_t page_size= (uint32_t)srv_page_size;
  uint64_t mask= ((uint64_t)1 << srv_page_size_shift) - 1;
  uint64_t offset= cur_file_offset;
  uint64_t page_start_offset= offset & ~mask;
  uint32_t page_end=
    end_offset > page_start_offset + (page_size - FIL_PAGE_DATA_END) ?
      (page_size - FIL_PAGE_DATA_END) :
      (uint32_t)(end_offset & mask);
  uint32_t in_page_offset= (uint32_t)(offset & mask);
  uint32_t sofar= 0;

  ut_ad(in_page_offset < page_size - FIL_PAGE_DATA_END);
  if (in_page_offset < FIL_PAGE_DATA)
    in_page_offset= FIL_PAGE_DATA;

  /* First return data from any partially-read chunk. */
  if ((sofar= chunk_remain)) {
    if (sofar <= len) {
      memcpy(buf, page_ptr + in_page_offset + chunk_pos, sofar);
      chunk_pos= 0;
      chunk_remain= 0;
      in_page_offset+= sofar;
    } else {
      memcpy(buf, page_ptr + in_page_offset + chunk_pos, len);
      chunk_pos+= len;
      chunk_remain= sofar - len;
      cur_file_offset= offset + len;
      return len;
    }
  }

  while (sofar < len && in_page_offset < page_end)
  {
    uchar type= page_ptr[in_page_offset];
    if (type == 0x00)
      break;  /* No more data on the page yet */
    if (type == FSP_BINLOG_TYPE_FILLER) {
      in_page_offset= page_size;  /* Point to start of next page */
      break;  /* No more data on page */
    }
    uint32_t size= page_ptr[in_page_offset + 1] +
      (uint32_t)(page_ptr[in_page_offset + 2] << 8);
    if ((type & FSP_BINLOG_TYPE_MASK) != FSP_BINLOG_TYPE_COMMIT ||
        (UNIV_UNLIKELY(skipping_partial) && (type & FSP_BINLOG_FLAG_CONT)))
    {
      /* Skip non-binlog-event record, or initial partial record. */
      in_page_offset += 3 + size;
      continue;
    }
    skipping_partial= false;

    /* Now grab the data in the chunk, or however much the caller requested. */
    uint32_t rest = len - sofar;
    if (size > rest) {
      /*
        Chunk contains more data than reader requested.
        Return what was requested, and remember the remaining partial data
        for the next read.
      */
      memcpy(buf + sofar, page_ptr + (in_page_offset + 3), rest);
      chunk_pos= rest;
      chunk_remain= size - rest;
      sofar+= rest;
      break;
    }

    memcpy(buf + sofar, page_ptr + (in_page_offset + 3), size);
    in_page_offset= in_page_offset + 3 + size;
    sofar+= size;
  }

  if (in_page_offset >= page_size - FIL_PAGE_DATA_END)
    cur_file_offset= page_start_offset + page_size; // To start of next page
  else
    cur_file_offset= page_start_offset | in_page_offset;
  return sofar;
}


handler_binlog_reader *
innodb_get_binlog_reader()
{
  return new ha_innodb_binlog_reader();
}


class gtid_search {
  /*
    Note that this enum is set up to be compatible with int results -1/0/1 for
    error/not found/fount from read_gtid_state_from_page().
  */
  enum Read_Result {
    READ_ENOENT= -2,
    READ_ERROR= -1,
    READ_NOT_FOUND= 0,
    READ_FOUND= 1
  };
public:
  gtid_search();
  ~gtid_search();
  enum Read_Result read_gtid_state_file_no(rpl_binlog_state_base *state,
                                           uint64_t file_no, uint32_t page_no,
                                           uint64_t *out_file_end,
                                           uint64_t *out_diff_state_interval);
  int find_gtid_pos(slave_connection_state *pos,
                    rpl_binlog_state_base *out_state, uint64_t *out_file_no,
                    uint64_t *out_offset);
private:
  uint64_t cur_open_file_no;
  uint64_t cur_open_file_length;
  File cur_open_file;
};


gtid_search::gtid_search()
  : cur_open_file_no(~(uint64_t)0), cur_open_file_length(0),
    cur_open_file((File)-1)
{
  /* Nothing else. */
}


gtid_search::~gtid_search()
{
  if (cur_open_file >= (File)0)
    my_close(cur_open_file, MYF(0));
}


/*
  Read a GTID state record from file_no and page_no.

  Returns:
    READ_ERROR      Error reading the file or corrupt data
    READ_ENOENT     File not found
    READ_NOT_FOUND  No GTID state record found on the page
    READ_FOUND      Record found
*/
enum gtid_search::Read_Result
gtid_search::read_gtid_state_file_no(rpl_binlog_state_base *state,
                                     uint64_t file_no, uint32_t page_no,
                                     uint64_t *out_file_end,
                                     uint64_t *out_diff_state_interval)
{
  buf_block_t *block;

  *out_file_end= 0;
  uint64_t active2= active_binlog_file_no.load(std::memory_order_acquire);
  if (file_no > active2)
    return READ_ENOENT;

  for (;;)
  {
    mtr_t mtr;
    bool mtr_started= false;
    uint64_t active= active2;
    uint64_t end_offset=
      binlog_cur_end_offset[file_no&1].load(std::memory_order_acquire);
    if (file_no + 1 >= active &&
        end_offset != ~(uint64_t)0 &&
        page_no <= (end_offset >> srv_page_size_shift))
    {
      /*
        See if the page is available in the buffer pool.
        Since we only use the low bit of file_no to determine the tablespace
        id, the buffer pool page will only be valid if the active file_no did
        not change while getting the page (otherwise it might belong to a
        later tablespace file).
      */
      mtr.start();
      mtr_started= true;
      uint32_t space_id= SRV_SPACE_ID_BINLOG0 + (file_no & 1);
      dberr_t err= DB_SUCCESS;
      block= buf_page_get_gen(page_id_t{space_id, page_no}, 0, RW_S_LATCH,
                              nullptr, BUF_GET_IF_IN_POOL, &mtr, &err);
      if (err != DB_SUCCESS) {
        mtr.commit();
        return READ_ERROR;
      }
    }
    else
      block= nullptr;
    active2= active_binlog_file_no.load(std::memory_order_acquire);
    if (UNIV_UNLIKELY(active2 != active))
    {
      /* Active moved ahead while we were reading, try again. */
      if (mtr_started)
        mtr.commit();
      continue;
    }
    if (file_no + 1 >= active)
    {
      *out_file_end= end_offset;
      /*
        Note: if end_offset is ~0, it means that the tablespace has been closed
        and needs to be read as a plain file. Then this condition will be false
        and we fall through to the file-reading code below, no need for an
        extra conditional jump here.
      */
      if (page_no > (end_offset >> srv_page_size_shift))
      {
        ut_ad(!mtr_started);
        return READ_NOT_FOUND;
      }
    }

    if (block)
    {
      ut_ad(end_offset != ~(uint64_t)0);
      int res= read_gtid_state_from_page(state, block->page.frame, page_no,
                                         out_diff_state_interval);
      ut_ad(mtr_started);
      if (mtr_started)
        mtr.commit();
      return (Read_Result)res;
    }
    else
    {
      if (mtr_started)
        mtr.commit();
      if (cur_open_file_no != file_no)
      {
        if (cur_open_file >= (File)0)
        {
          my_close(cur_open_file, MYF(0));
          cur_open_file= (File)-1;
          cur_open_file_length= 0;
        }
      }
      if (cur_open_file < (File)0)
      {
        char filename[BINLOG_NAME_LEN];
        binlog_name_make(filename, file_no);
        cur_open_file= my_open(filename, O_RDONLY | O_BINARY, MYF(0));
        if (cur_open_file < (File)0)
        {
          if (errno == ENOENT)
            return READ_ENOENT;
          my_error(ER_CANT_OPEN_FILE, MYF(0), filename, errno);
          return READ_ERROR;
        }
        MY_STAT stat_buf;
        if (my_fstat(cur_open_file, &stat_buf, MYF(0))) {
          my_error(ER_CANT_GET_STAT, MYF(0), filename, errno);
          my_close(cur_open_file, MYF(0));
          cur_open_file= (File)-1;
          return READ_ERROR;
        }
        cur_open_file_length= stat_buf.st_size;
        cur_open_file_no= file_no;
      }
      if (!*out_file_end)
        *out_file_end= cur_open_file_length;
      return (Read_Result)read_gtid_state(state, cur_open_file, page_no,
                                          out_diff_state_interval);
    }
  }
}


/*
  Search for a GTID position in the binlog.
  Find a binlog file_no and an offset into the file that is guaranteed to
  be before the target position. It can be a bit earlier, that only means a
  bit more of the binlog needs to be scanned to find the real position.

  Returns:
    -1 error
     0 Position not found (has been purged)
     1 Position found
*/

int
gtid_search::find_gtid_pos(slave_connection_state *pos,
                           rpl_binlog_state_base *out_state,
                           uint64_t *out_file_no, uint64_t *out_offset)
{
  /*
    Dirty read, but getting a slightly stale value is no problem, we will just
    be starting to scan the binlog file at a slightly earlier position than
    necessary.
  */
  uint64_t file_no= active_binlog_file_no.load(std::memory_order_relaxed);

  /* First search backwards for the right file to start from. */
  uint64_t file_end= 0;
  uint64_t diff_state_interval= 0;
  rpl_binlog_state_base base_state, diff_state;
  base_state.init();
  for (;;)
  {
    enum Read_Result res=
      read_gtid_state_file_no(&base_state, file_no, 0, &file_end,
                              &diff_state_interval);
    if (res == READ_ENOENT)
      return 0;
    if (res == READ_ERROR)
      return -1;
    if (res == READ_NOT_FOUND)
    {
      if (file_no == 0)
      {
        /* Handle the special case of a completely empty binlog file. */
        out_state->reset_nolock();
        *out_file_no= file_no;
        *out_offset= 0;
        return 1;
      }
      ut_ad(0 /* Not expected to find no state, should always be written. */);
      return -1;
    }
    if (base_state.is_before_pos(pos))
      break;
    base_state.reset_nolock();
    if (file_no == 0)
      return 0;
    --file_no;
  }

  /*
    Then binary search for the last differential state record that is still
    before the searched position.

    The invariant is that page2 is known to be after the target page, and page0
    is known to be a valid position to start (but possibly earlier than needed).
  */
  uint32_t diff_state_page_interval=
    (uint32_t)(diff_state_interval >> srv_page_size_shift);
  ut_ad(diff_state_interval % srv_page_size == 0);
  if (diff_state_interval % srv_page_size != 0)
    return -1;  // Corrupt tablespace
  uint32_t page0= 0;
  uint32_t page2= (uint32_t)
    ((file_end + diff_state_interval - 1) >> srv_page_size_shift);
  /* Round to the next diff_state_interval after file_end. */
  page2-= page2 % diff_state_page_interval;
  uint32_t page1= (page0 + page2) / 2;
  diff_state.init();
  diff_state.load_nolock(&base_state);
  while (page1 >= page0 + diff_state_interval)
  {
    ut_ad((page1 - page0) % diff_state_interval == 0);
    diff_state.reset_nolock();
    diff_state.load_nolock(&base_state);
    enum Read_Result res=
      read_gtid_state_file_no(&diff_state, file_no, 0, &file_end,
                              &diff_state_interval);
    if (res == READ_ENOENT)
      return 0;  /* File purged while we are reading from it? */
    if (res == READ_ERROR)
      return -1;
    if (res == READ_NOT_FOUND)
    {
      /*
        If the diff state record was not written here for some reason, just
        try the one just before. It will be safe, even if not always optimal,
        and this is an abnormal situation anyway.
      */
      page1= page1 - diff_state_page_interval;
      continue;
    }
    if (diff_state.is_before_pos(pos))
      page0= page1;
    else
      page2= page1;
    page1= (page0 + page2) / 2;
  }
  ut_ad(page1 >= page0);
  out_state->load_nolock(&diff_state);
  *out_file_no= file_no;
  *out_offset= (uint64_t)page0 << srv_page_size_shift;
  return 1;
}


int
ha_innodb_binlog_reader::init_gtid_pos(slave_connection_state *pos,
                                       rpl_binlog_state_base *state)
{
  gtid_search search_obj;
  uint64_t file_no;
  uint64_t offset;
  int res= search_obj.find_gtid_pos(pos, state, &file_no, &offset);
  if (res < 0)
    return -1;
  if (res > 0)
  {
    cur_file_no= file_no;
    cur_file_offset= offset;
  }
  return res;
}


bool
innobase_binlog_write_direct(IO_CACHE *cache,
                             handler_binlog_event_group_info *binlog_info,
                             const rpl_gtid *gtid)
{
  mtr_t mtr;
  if (gtid)
    binlog_diff_state.update_nolock(gtid);
  mtr.start();
  fsp_binlog_write_cache(cache, binlog_info, &mtr);
  mtr.commit();
  /* ToDo: Should we sync the log here? Maybe depending on an extra bool parameter? */
  /* ToDo: Presumably fsp_binlog_write_cache() should be able to fail in some cases? Then return any such error to the caller. */
  return false;
}
