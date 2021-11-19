/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2021, MariaDB Corporation.

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
@file include/btr0btr.ic
The B-tree

Created 6/2/1994 Heikki Tuuri
*******************************************************/

#include "mach0data.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "page0zip.h"

/**************************************************************//**
Gets the index id field of a page.
@return index id */
UNIV_INLINE
index_id_t
btr_page_get_index_id(
/*==================*/
	const page_t*	page)	/*!< in: index page */
{
	return(mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID));
}

/** Set PAGE_LEVEL.
@param[in,out]  block  buffer block
@param[in]      level  page level
@param[in,out]  mtr    mini-transaction */
inline
void btr_page_set_level(buf_block_t *block, ulint level, mtr_t *mtr)
{
  ut_ad(level <= BTR_MAX_NODE_LEVEL);
  constexpr uint16_t field= PAGE_HEADER + PAGE_LEVEL;
  byte *b= my_assume_aligned<2>(&block->page.frame[field]);
  if (mtr->write<2,mtr_t::MAYBE_NOP>(*block, b, level) &&
      UNIV_LIKELY_NULL(block->page.zip.data))
    memcpy_aligned<2>(&block->page.zip.data[field], b, 2);
}

/** Set FIL_PAGE_NEXT.
@param[in,out]  block  buffer block
@param[in]      next   number of successor page
@param[in,out]  mtr    mini-transaction */
inline void btr_page_set_next(buf_block_t *block, ulint next, mtr_t *mtr)
{
  constexpr uint16_t field= FIL_PAGE_NEXT;
  byte *b= my_assume_aligned<4>(&block->page.frame[field]);
  if (mtr->write<4,mtr_t::MAYBE_NOP>(*block, b, next) &&
      UNIV_LIKELY_NULL(block->page.zip.data))
    memcpy_aligned<4>(&block->page.zip.data[field], b, 4);
}

/** Set FIL_PAGE_PREV.
@param[in,out]  block  buffer block
@param[in]      prev   number of predecessor page
@param[in,out]  mtr    mini-transaction */
inline void btr_page_set_prev(buf_block_t *block, ulint prev, mtr_t *mtr)
{
  constexpr uint16_t field= FIL_PAGE_PREV;
  byte *b= my_assume_aligned<4>(&block->page.frame[field]);
  if (mtr->write<4,mtr_t::MAYBE_NOP>(*block, b, prev) &&
      UNIV_LIKELY_NULL(block->page.zip.data))
    memcpy_aligned<4>(&block->page.zip.data[field], b, 4);
}

/**************************************************************//**
Gets the child node file address in a node pointer.
NOTE: the offsets array must contain all offsets for the record since
we read the last field according to offsets and assume that it contains
the child page number. In other words offsets must have been retrieved
with rec_get_offsets(n_fields=ULINT_UNDEFINED).
@return child node address */
UNIV_INLINE
uint32_t
btr_node_ptr_get_child_page_no(
/*===========================*/
	const rec_t*	rec,	/*!< in: node pointer record */
	const rec_offs*	offsets)/*!< in: array returned by rec_get_offsets() */
{
	const byte*	field;
	ulint		len;

	ut_ad(!rec_offs_comp(offsets) || rec_get_node_ptr_flag(rec));

	/* The child address is in the last field */
	field = rec_get_nth_field(rec, offsets,
				  rec_offs_n_fields(offsets) - 1, &len);

	ut_ad(len == 4);

	uint32_t page_no = mach_read_from_4(field);
	ut_ad(page_no > 1);

	return(page_no);
}

/**************************************************************//**
Releases the latches on a leaf page and bufferunfixes it. */
UNIV_INLINE
void
btr_leaf_page_release(
/*==================*/
	buf_block_t*	block,		/*!< in: buffer block */
	ulint		latch_mode,	/*!< in: BTR_SEARCH_LEAF or
					BTR_MODIFY_LEAF */
	mtr_t*		mtr)		/*!< in: mtr */
{
	ut_ad(latch_mode == BTR_SEARCH_LEAF
	      || latch_mode == BTR_MODIFY_LEAF
	      || latch_mode == BTR_NO_LATCHES);

	ut_ad(!mtr->memo_contains_flagged(block, MTR_MEMO_MODIFY));

	mtr_memo_type_t mode;
	switch (latch_mode) {
		case BTR_SEARCH_LEAF:
			mode = MTR_MEMO_PAGE_S_FIX;
			break;
		case BTR_MODIFY_LEAF:
			mode = MTR_MEMO_PAGE_X_FIX;
			break;
		case BTR_NO_LATCHES:
			mode = MTR_MEMO_BUF_FIX;
			break;
		default:
			ut_a(0);
	}

	mtr->memo_release(block, mode);
}
