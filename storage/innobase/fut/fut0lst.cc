/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, MariaDB Corporation.

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
@file fut/fut0lst.cc
File-based list utilities

Created 11/28/1995 Heikki Tuuri
***********************************************************************/

#include "fut0lst.h"
#include "buf0buf.h"
#include "page0page.h"

/** Add a node to an empty list. */
static void flst_add_to_empty(buf_block_t *base, uint16_t boffset,
                              buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, add->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  fil_addr_t addr= { add->page.id.page_no(), aoffset };

  /* Update first and last fields of base node */
  flst_write_addr(*base, base->frame + boffset + FLST_FIRST, addr, mtr);
  /* MDEV-12353 TODO: use MEMMOVE record */
  flst_write_addr(*base, base->frame + boffset + FLST_LAST, addr, mtr);

  /* Set prev and next fields of node to add */
  flst_zero_addr(*add, add->frame + aoffset + FLST_PREV, mtr);
  flst_zero_addr(*add, add->frame + aoffset + FLST_NEXT, mtr);

  /* Update len of base node */
  ut_ad(!mach_read_from_4(base->frame + boffset + FLST_LEN));
  mtr->write<1>(*base, base->frame + boffset + (FLST_LEN + 3), 1U);
}

/** Insert a node after another one.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     insert position block
@param[in]      coffset byte offset of the insert position
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the block to be added
@param[in,outr] mtr     mini-transaction */
static void flst_insert_after(buf_block_t *base, uint16_t boffset,
                              buf_block_t *cur, uint16_t coffset,
                              buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
{
  ut_ad(base != cur || boffset != coffset);
  ut_ad(base != add || boffset != aoffset);
  ut_ad(cur != add || coffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(coffset < cur->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, cur->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, add->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));

  fil_addr_t cur_addr= { cur->page.id.page_no(), coffset };
  fil_addr_t add_addr= { add->page.id.page_no(), aoffset };
  fil_addr_t next_addr= flst_get_next_addr(cur->frame + coffset);

  flst_write_addr(*add, add->frame + aoffset + FLST_PREV, cur_addr, mtr);
  flst_write_addr(*add, add->frame + aoffset + FLST_NEXT, next_addr, mtr);

  if (fil_addr_is_null(next_addr))
    flst_write_addr(*base, base->frame + boffset + FLST_LAST, add_addr, mtr);
  else
  {
    buf_block_t *block;
    flst_node_t *next= fut_get_ptr(add->page.id.space(), add->zip_size(),
                                   next_addr, RW_SX_LATCH, mtr, &block);
    flst_write_addr(*block, next + FLST_PREV, add_addr, mtr);
  }

  flst_write_addr(*cur, cur->frame + coffset + FLST_NEXT, add_addr, mtr);

  byte *len= &base->frame[boffset + FLST_LEN];
  mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
}

/** Insert a node before another one.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     insert position block
@param[in]      coffset byte offset of the insert position
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the block to be added
@param[in,outr] mtr     mini-transaction */
static void flst_insert_before(buf_block_t *base, uint16_t boffset,
                               buf_block_t *cur, uint16_t coffset,
                               buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
{
  ut_ad(base != cur || boffset != coffset);
  ut_ad(base != add || boffset != aoffset);
  ut_ad(cur != add || coffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(coffset < cur->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, cur->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, add->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));

  fil_addr_t cur_addr= { cur->page.id.page_no(), coffset };
  fil_addr_t add_addr= { add->page.id.page_no(), aoffset };
  fil_addr_t prev_addr= flst_get_prev_addr(cur->frame + coffset);

  flst_write_addr(*add, add->frame + aoffset + FLST_PREV, prev_addr, mtr);
  flst_write_addr(*add, add->frame + aoffset + FLST_NEXT, cur_addr, mtr);

  if (fil_addr_is_null(prev_addr))
    flst_write_addr(*base, base->frame + boffset + FLST_FIRST, add_addr, mtr);
  else
  {
    buf_block_t *block;
    flst_node_t *prev= fut_get_ptr(add->page.id.space(), add->zip_size(),
                                   prev_addr, RW_SX_LATCH, mtr, &block);
    flst_write_addr(*block, prev + FLST_NEXT, add_addr, mtr);
  }

  flst_write_addr(*cur, cur->frame + coffset + FLST_PREV, add_addr, mtr);

  byte *len= &base->frame[boffset + FLST_LEN];
  mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
}

/** Append a file list node to a list.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the node to be added
@param[in,outr] mtr     mini-transaction */
void flst_add_last(buf_block_t *base, uint16_t boffset,
                   buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, add->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));

  if (!flst_get_len(base->frame + boffset))
    flst_add_to_empty(base, boffset, add, aoffset, mtr);
  else
  {
    fil_addr_t addr= flst_get_last(base->frame + boffset);
    buf_block_t *cur= add;
    const flst_node_t *c= addr.page == add->page.id.page_no()
      ? add->frame + addr.boffset
      : fut_get_ptr(add->page.id.space(), add->zip_size(), addr,
                    RW_SX_LATCH, mtr, &cur);
    flst_insert_after(base, boffset, cur,
                      static_cast<uint16_t>(c - cur->frame),
                      add, aoffset, mtr);
  }
}

/** Prepend a file list node to a list.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  add     block to be added
@param[in]      aoffset byte offset of the node to be added
@param[in,outr] mtr     mini-transaction */
void flst_add_first(buf_block_t *base, uint16_t boffset,
                    buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, add->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));

  if (!flst_get_len(base->frame + boffset))
    flst_add_to_empty(base, boffset, add, aoffset, mtr);
  else
  {
    fil_addr_t addr= flst_get_first(base->frame + boffset);
    buf_block_t *cur= add;
    const flst_node_t *c= addr.page == add->page.id.page_no()
      ? add->frame + addr.boffset
      : fut_get_ptr(add->page.id.space(), add->zip_size(), addr,
                    RW_SX_LATCH, mtr, &cur);
    flst_insert_before(base, boffset, cur,
                       static_cast<uint16_t>(c - cur->frame),
                       add, aoffset, mtr);
  }
}

/** Remove a file list node.
@param[in,out]  base    base node block
@param[in]      boffset byte offset of the base node
@param[in,out]  cur     block to be removed
@param[in]      coffset byte offset of the current record to be removed
@param[in,outr] mtr     mini-transaction */
void flst_remove(buf_block_t *base, uint16_t boffset,
                 buf_block_t *cur, uint16_t coffset, mtr_t *mtr)
{
  ut_ad(boffset < base->physical_size());
  ut_ad(coffset < cur->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr_memo_contains_page_flagged(mtr, cur->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));

  const fil_addr_t prev_addr= flst_get_prev_addr(cur->frame + coffset);
  const fil_addr_t next_addr= flst_get_next_addr(cur->frame + coffset);

  if (fil_addr_is_null(prev_addr))
    flst_write_addr(*base, base->frame + boffset + FLST_FIRST, next_addr, mtr);
  else
  {
    buf_block_t *block= cur;
    flst_node_t *prev= prev_addr.page == cur->page.id.page_no()
      ? cur->frame + prev_addr.boffset
      : fut_get_ptr(cur->page.id.space(), cur->zip_size(), prev_addr,
                    RW_SX_LATCH, mtr, &block);
    flst_write_addr(*block, prev + FLST_NEXT, next_addr, mtr);
  }

  if (fil_addr_is_null(next_addr))
    flst_write_addr(*base, base->frame + boffset + FLST_LAST, prev_addr, mtr);
  else
  {
    buf_block_t *block= cur;
    flst_node_t *next= next_addr.page == cur->page.id.page_no()
      ? cur->frame + next_addr.boffset
      : fut_get_ptr(cur->page.id.space(), cur->zip_size(), next_addr,
                    RW_SX_LATCH, mtr, &block);
    flst_write_addr(*block, next + FLST_PREV, prev_addr, mtr);
  }

  byte *len= &base->frame[boffset + FLST_LEN];
  ut_ad(mach_read_from_4(len) > 0);
  mtr->write<4>(*base, len, mach_read_from_4(len) - 1);
}

#ifdef UNIV_DEBUG
/** Validate a file-based list. */
void flst_validate(const buf_block_t *base, uint16_t boffset, mtr_t *mtr)
{
  ut_ad(boffset < base->physical_size());
  ut_ad(mtr_memo_contains_page_flagged(mtr, base->frame,
                                       MTR_MEMO_PAGE_X_FIX |
                                       MTR_MEMO_PAGE_SX_FIX));

  /* We use two mini-transaction handles: the first is used to lock
  the base node, and prevent other threads from modifying the list.
  The second is used to traverse the list. We cannot run the second
  mtr without committing it at times, because if the list is long,
  the x-locked pages could fill the buffer, resulting in a deadlock. */
  mtr_t mtr2;

  const uint32_t len= flst_get_len(base->frame + boffset);
  fil_addr_t addr= flst_get_first(base->frame + boffset);

  for (uint32_t i= len; i--; )
  {
    mtr2.start();
    const flst_node_t *node= fut_get_ptr(base->page.id.space(),
                                         base->zip_size(), addr,
                                         RW_SX_LATCH, &mtr2);
    addr= flst_get_next_addr(node);
    mtr2.commit();
  }

  ut_ad(fil_addr_is_null(addr));

  addr= flst_get_last(base->frame + boffset);

  for (uint32_t i= len; i--; )
  {
    mtr2.start();
    const flst_node_t *node= fut_get_ptr(base->page.id.space(),
                                         base->zip_size(), addr,
                                         RW_SX_LATCH, &mtr2);
    addr= flst_get_prev_addr(node);
    mtr2.commit();
  }

  ut_ad(fil_addr_is_null(addr));
}
#endif
