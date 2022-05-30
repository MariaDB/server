/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
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

/******************************************************************//**
@file include/fut0lst.h
File-based list utilities

Created 11/28/1995 Heikki Tuuri
***********************************************************************/

#ifndef fut0lst_h
#define fut0lst_h

#ifdef UNIV_INNOCHECKSUM
# include "fil0fil.h"
#else
#include "fut0fut.h"
#include "mtr0log.h"

/* The C 'types' of base node and list node: these should be used to
write self-documenting code. Of course, the sizeof macro cannot be
applied to these types! */

typedef	byte	flst_base_node_t;
typedef	byte	flst_node_t;

#endif /* !UNIV_INNOCHECKSUM */

/* The physical size of a list base node in bytes */
#define	FLST_BASE_NODE_SIZE	(4 + 2 * FIL_ADDR_SIZE)
/* The physical size of a list node in bytes */
#define	FLST_NODE_SIZE		(2 * FIL_ADDR_SIZE)

#ifndef UNIV_INNOCHECKSUM
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
void flst_init(const buf_block_t& block, byte *base, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));

/** Append a file list node to a list.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the node to be added
@param[in,outr] mtr     mini-transaction */
void flst_add_last(buf_block_t *base, uint16_t boffset,
                   buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));
/** Prepend a file list node to a list.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the node to be added
@param[in,outr] mtr     mini-transaction */
void flst_add_first(buf_block_t *base, uint16_t boffset,
                    buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));
/** Remove a file list node.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     block to be removed
@param[in]      coffset byte offset of the current record to be removed
@param[in,outr] mtr     mini-transaction */
void flst_remove(buf_block_t *base, uint16_t boffset,
                 buf_block_t *cur, uint16_t coffset, mtr_t *mtr)
  MY_ATTRIBUTE((nonnull));

/** @return the length of a list */
inline uint32_t flst_get_len(const flst_base_node_t *base)
{
  return mach_read_from_4(base + FLST_LEN);
}

/** @return a file address */
inline fil_addr_t flst_read_addr(const byte *faddr)
{
  fil_addr_t addr= { mach_read_from_4(faddr + FIL_ADDR_PAGE),
		     mach_read_from_2(faddr + FIL_ADDR_BYTE) };
  ut_a(addr.page == FIL_NULL || addr.boffset >= FIL_PAGE_DATA);
  ut_a(ut_align_offset(faddr, srv_page_size) >= FIL_PAGE_DATA);
  return addr;
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

#ifdef UNIV_DEBUG
/** Validate a file-based list. */
void flst_validate(const buf_block_t *base, uint16_t boffset, mtr_t *mtr);
#endif

#endif /* !UNIV_INNOCHECKSUM */

#endif
