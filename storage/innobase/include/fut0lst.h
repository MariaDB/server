/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

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
@file include/fut0lst.h
File-based list utilities

Created 11/28/1995 Heikki Tuuri
***********************************************************************/

#pragma once

/* The physical size of a list base node in bytes */
#define	FLST_BASE_NODE_SIZE	(4 + 2 * FIL_ADDR_SIZE)
/* The physical size of a list node in bytes */
#define	FLST_NODE_SIZE		(2 * FIL_ADDR_SIZE)

#ifdef UNIV_INNOCHECKSUM
# include "fil0fil.h"
#else
# include "mtr0log.h"

typedef	byte	flst_base_node_t;
typedef	byte	flst_node_t;

/* We define the field offsets of a node for the list */
#define FLST_PREV	0	/* 6-byte address of the previous list element;
				the page part of address is FIL_NULL, if no
				previous element */
#define FLST_NEXT	FIL_ADDR_SIZE	/* 6-byte address of the next
				list element; the page part of address
				is FIL_NULL, if no next element */

/* We define the field offsets of a base node for the list */
#define FLST_LEN	0	/* 32-bit list length field */
#define	FLST_FIRST	4	/* 6-byte address of the first element
				of the list; undefined if empty list */
#define	FLST_LAST	(4 + FIL_ADDR_SIZE) /* 6-byte address of the
				last element of the list; undefined
				if empty list */

/** Initialize a zero-initialized list base node.
@param[in,out]	block	file page
@param[in]	ofs	byte offset of the list base node
@param[in,out]	mtr	mini-transaction */
inline void flst_init(const buf_block_t* block, uint16_t ofs, mtr_t* mtr)
{
  ut_d(const page_t *page= block->page.frame);
  ut_ad(!mach_read_from_2(FLST_LEN + ofs + page));
  ut_ad(!mach_read_from_2(FLST_FIRST + FIL_ADDR_BYTE + ofs + page));
  ut_ad(!mach_read_from_2(FLST_LAST + FIL_ADDR_BYTE + ofs + page));
  compile_time_assert(FIL_NULL == 0xffU * 0x1010101U);
  mtr->memset(block, FLST_FIRST + FIL_ADDR_PAGE + ofs, 4, 0xff);
  mtr->memset(block, FLST_LAST + FIL_ADDR_PAGE + ofs, 4, 0xff);
}

/** Initialize a list base node.
@param[in]      block   file page
@param[in,out]  base    base node
@param[in,out]  mtr     mini-transaction */
void flst_init(const buf_block_t &block, byte *base, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));

/** Append a file list node to a list.
@param base    base node block
@param boffset byte offset of the base node
@param add     block to be added
@param aoffset byte offset of the node to be added
@param limit   fil_space_t::free_limit
@param mtr     mini-transaction
@return error code */
dberr_t flst_add_last(buf_block_t *base, uint16_t boffset,
                      buf_block_t *add, uint16_t aoffset,
                      uint32_t limit, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Prepend a file list node to a list.
@param base    base node block
@param boffset byte offset of the base node
@param add     block to be added
@param aoffset byte offset of the node to be added
@param limit   fil_space_t::free_limit
@param mtr     mini-transaction
@return error code */
dberr_t flst_add_first(buf_block_t *base, uint16_t boffset,
                       buf_block_t *add, uint16_t aoffset,
                       uint32_t limit, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull, warn_unused_result));
/** Remove a file list node.
@param base    base node block
@param boffset byte offset of the base node
@param cur     block to be removed
@param coffset byte offset of the current record to be removed
@param limit   fil_space_t::free_limit
@param mtr     mini-transaction
@return error code */
dberr_t flst_remove(buf_block_t *base, uint16_t boffset,
                    buf_block_t *cur, uint16_t coffset,
                    uint32_t limit, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull, warn_unused_result));

/** @return the length of a list */
inline uint32_t flst_get_len(const flst_base_node_t *base)
{
  return mach_read_from_4(base + FLST_LEN);
}

/** @return a file address */
inline fil_addr_t flst_read_addr(const byte *faddr)
{
  ut_ad(ut_align_offset(faddr, srv_page_size) >= FIL_PAGE_DATA);
  return fil_addr_t{mach_read_from_4(faddr + FIL_ADDR_PAGE),
                    mach_read_from_2(faddr + FIL_ADDR_BYTE)};
}

/** @return list first node address */
inline fil_addr_t flst_get_first(const flst_base_node_t *base)
{
  return flst_read_addr(base + FLST_FIRST);
}

/** @return list last node address */
inline fil_addr_t flst_get_last(const flst_base_node_t *base)
{
  return flst_read_addr(base + FLST_LAST);
}

/** @return list next node address */
inline fil_addr_t flst_get_next_addr(const flst_node_t* node)
{
  return flst_read_addr(node + FLST_NEXT);
}

/** @return list prev node address */
inline fil_addr_t flst_get_prev_addr(const flst_node_t *node)
{
  return flst_read_addr(node + FLST_PREV);
}

/** Write a file address.
@param[in]      block   file page
@param[in,out]  faddr   file address location
@param[in]      page    page number
@param[in]      boffset byte offset
@param[in,out]  mtr     mini-transaction */
void flst_write_addr(const buf_block_t &block, byte *faddr,
                     uint32_t page, uint16_t boffset, mtr_t *mtr);

/** Validate a file-based list. */
dberr_t flst_validate(const buf_block_t *base, uint16_t boffset,
                      mtr_t *mtr) noexcept;

#endif /* !UNIV_INNOCHECKSUM */
