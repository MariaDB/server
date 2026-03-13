/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2019, 2022, MariaDB Corporation.

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


/** Write a file address.
@param[in]      block   file page
@param[in,out]  faddr   file address location
@param[in]      page    page number
@param[in]      boffset byte offset
@param[in,out]  mtr     mini-transaction */
void flst_write_addr(const buf_block_t &block, byte *faddr,
                     uint32_t page, uint16_t boffset, mtr_t *mtr)
{
  ut_ad(mtr->memo_contains_page_flagged(faddr, MTR_MEMO_PAGE_X_FIX |
                                        MTR_MEMO_PAGE_SX_FIX));
  ut_a(page == FIL_NULL || boffset >= FIL_PAGE_DATA);
  ut_a(ut_align_offset(faddr, srv_page_size) >= FIL_PAGE_DATA);

  static_assert(FIL_ADDR_PAGE == 0, "compatibility");
  static_assert(FIL_ADDR_BYTE == 4, "compatibility");
  static_assert(FIL_ADDR_SIZE == 6, "compatibility");

  if (!mtr->is_logged())
  {
    mach_write_to_4(faddr + FIL_ADDR_PAGE, page);
    mach_write_to_2(faddr + FIL_ADDR_BYTE, boffset);
    mtr->set_modified(block);
    return;
  }

  const bool same_page= mach_read_from_4(faddr + FIL_ADDR_PAGE) == page;
  const bool same_offset= mach_read_from_2(faddr + FIL_ADDR_BYTE) == boffset;
  if (same_page)
  {
    if (!same_offset)
      mtr->write<2>(block, faddr + FIL_ADDR_BYTE, boffset);
    return;
  }
  if (same_offset)
    mtr->write<4>(block, faddr + FIL_ADDR_PAGE, page);
  else
  {
    alignas(4) byte fil_addr[6];
    mach_write_to_4(fil_addr + FIL_ADDR_PAGE, page);
    mach_write_to_2(fil_addr + FIL_ADDR_BYTE, boffset);
    mtr->memcpy(block, faddr + FIL_ADDR_PAGE, fil_addr, 6);
  }
}

/** Write 2 null file addresses.
@param[in]      b       file page
@param[in,out]  addr    file address to be zeroed out
@param[in,out]  mtr     mini-transaction */
static void flst_zero_both(const buf_block_t& b, byte *addr, mtr_t *mtr)
{
  const ulint boffset= ulint(addr - b.page.frame);
  if (mach_read_from_4(addr + FIL_ADDR_PAGE) != FIL_NULL)
    mtr->memset(&b, boffset + FIL_ADDR_PAGE, 4, 0xff);
  mtr->write<2,mtr_t::MAYBE_NOP>(b, addr + FIL_ADDR_BYTE, 0U);
  /* Initialize the other address by (MEMMOVE|0x80,offset,FIL_ADDR_SIZE,source)
  which is 4 bytes, or less than FIL_ADDR_SIZE. */
  memcpy(addr + FIL_ADDR_SIZE, addr, FIL_ADDR_SIZE);
  mtr->memmove(b, boffset + FIL_ADDR_SIZE, boffset, FIL_ADDR_SIZE);
}

/** Add a node to an empty list. */
static void flst_add_to_empty(buf_block_t *base, uint16_t boffset,
                              buf_block_t *add, uint16_t aoffset, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  ut_ad(!mach_read_from_4(base->page.frame + boffset + FLST_LEN));
  mtr->write<1>(*base, base->page.frame + boffset + (FLST_LEN + 3), 1U);
  /* Update first and last fields of base node */
  flst_write_addr(*base, base->page.frame + boffset + FLST_FIRST,
                  add->page.id().page_no(), aoffset, mtr);
  memcpy(base->page.frame + boffset + FLST_LAST,
         base->page.frame + boffset + FLST_FIRST,
         FIL_ADDR_SIZE);
  /* Initialize FLST_LAST by (MEMMOVE|0x80,offset,FIL_ADDR_SIZE,source)
  which is 4 bytes, or less than FIL_ADDR_SIZE. */
  mtr->memmove(*base, boffset + FLST_LAST, boffset + FLST_FIRST,
               FIL_ADDR_SIZE);

  /* Set prev and next fields of node to add */
  static_assert(FLST_NEXT == FLST_PREV + FIL_ADDR_SIZE, "compatibility");
  flst_zero_both(*add, add->page.frame + aoffset + FLST_PREV, mtr);
}

/** Insert a node after another one.
@param base    base node block
@param boffset byte offset of the base node
@param cur     insert position block
@param coffset byte offset of the insert position
@param add     block to be added
@param aoffset byte offset of the block to be added
@param limit   fil_space_t::free_limit
@param mtr     mini-transaction */
static dberr_t flst_insert_after(buf_block_t *base, uint16_t boffset,
                                 buf_block_t *cur, uint16_t coffset,
                                 buf_block_t *add, uint16_t aoffset,
                                 uint32_t limit, mtr_t *mtr)
{
  ut_ad(base != cur || boffset != coffset);
  ut_ad(base != add || boffset != aoffset);
  ut_ad(cur != add || coffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(coffset < cur->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(cur, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  fil_addr_t next_addr= flst_get_next_addr(cur->page.frame + coffset);
  if (next_addr.page >= limit)
  {
    if (UNIV_UNLIKELY(next_addr.page != FIL_NULL))
      return DB_CORRUPTION;
  }
  else if (UNIV_UNLIKELY(next_addr.boffset < FIL_PAGE_DATA ||
                         next_addr.boffset >= base->physical_size() -
                         FIL_PAGE_DATA_END))
    return DB_CORRUPTION;

  flst_write_addr(*add, add->page.frame + aoffset + FLST_PREV,
                  cur->page.id().page_no(), coffset, mtr);
  flst_write_addr(*add, add->page.frame + aoffset + FLST_NEXT,
                  next_addr.page, next_addr.boffset, mtr);

  dberr_t err= DB_SUCCESS;

  if (next_addr.page == FIL_NULL)
    flst_write_addr(*base, base->page.frame + boffset + FLST_LAST,
                    add->page.id().page_no(), aoffset, mtr);
  else if (buf_block_t *block=
           buf_page_get_gen(page_id_t{add->page.id().space(), next_addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err))
    flst_write_addr(*block, block->page.frame +
                    next_addr.boffset + FLST_PREV,
                    add->page.id().page_no(), aoffset, mtr);

  flst_write_addr(*cur, cur->page.frame + coffset + FLST_NEXT,
                  add->page.id().page_no(), aoffset, mtr);

  byte *len= &base->page.frame[boffset + FLST_LEN];
  mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
  return err;
}

/** Insert a node before another one.
@param base    base node block
@param boffset byte offset of the base node
@param cur     insert position block
@param coffset byte offset of the insert position
@param add     block to be added
@param aoffset byte offset of the block to be added
@param limit   fil_space_t::free_limit
@param mtr     mini-transaction
@return error code */
static dberr_t flst_insert_before(buf_block_t *base, uint16_t boffset,
                                  buf_block_t *cur, uint16_t coffset,
                                  buf_block_t *add, uint16_t aoffset,
                                  uint32_t limit, mtr_t *mtr)
{
  ut_ad(base != cur || boffset != coffset);
  ut_ad(base != add || boffset != aoffset);
  ut_ad(cur != add || coffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(coffset < cur->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(cur, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  fil_addr_t prev_addr= flst_get_prev_addr(cur->page.frame + coffset);
  if (prev_addr.page >= limit)
  {
    if (UNIV_UNLIKELY(prev_addr.page != FIL_NULL))
      return DB_CORRUPTION;
  }
  else if (UNIV_UNLIKELY(prev_addr.boffset < FIL_PAGE_DATA ||
                         prev_addr.boffset >= base->physical_size() -
                         FIL_PAGE_DATA_END))
    return DB_CORRUPTION;

  flst_write_addr(*add, add->page.frame + aoffset + FLST_PREV,
                  prev_addr.page, prev_addr.boffset, mtr);
  flst_write_addr(*add, add->page.frame + aoffset + FLST_NEXT,
                  cur->page.id().page_no(), coffset, mtr);

  dberr_t err= DB_SUCCESS;

  if (prev_addr.page == FIL_NULL)
    flst_write_addr(*base, base->page.frame + boffset + FLST_FIRST,
                    add->page.id().page_no(), aoffset, mtr);
  else if (buf_block_t *block=
           buf_page_get_gen(page_id_t{add->page.id().space(), prev_addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err))
    flst_write_addr(*block, block->page.frame +
                    prev_addr.boffset + FLST_NEXT,
                    add->page.id().page_no(), aoffset, mtr);

  flst_write_addr(*cur, cur->page.frame + coffset + FLST_PREV,
                    add->page.id().page_no(), aoffset, mtr);

  byte *len= &base->page.frame[boffset + FLST_LEN];
  mtr->write<4>(*base, len, mach_read_from_4(len) + 1);
  return err;
}

/** Initialize a list base node.
@param[in]      block   file page
@param[in,out]  base    base node
@param[in,out]  mtr     mini-transaction */
void flst_init(const buf_block_t& block, byte *base, mtr_t *mtr)
{
  ut_ad(mtr->memo_contains_page_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                        MTR_MEMO_PAGE_SX_FIX));
  mtr->write<4,mtr_t::MAYBE_NOP>(block, base + FLST_LEN, 0U);
  static_assert(FLST_LAST == FLST_FIRST + FIL_ADDR_SIZE, "compatibility");
  flst_zero_both(block, base + FLST_FIRST, mtr);
}

dberr_t flst_add_last(buf_block_t *base, uint16_t boffset,
                      buf_block_t *add, uint16_t aoffset,
                      uint32_t limit, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  if (!flst_get_len(base->page.frame + boffset))
  {
    flst_add_to_empty(base, boffset, add, aoffset, mtr);
    return DB_SUCCESS;
  }
  else
  {
    fil_addr_t addr= flst_get_last(base->page.frame + boffset);
    if (UNIV_UNLIKELY(addr.page >= limit))
      return DB_CORRUPTION;
    else if (UNIV_UNLIKELY(addr.boffset < FIL_PAGE_DATA ||
                           addr.boffset >= base->physical_size() -
                           FIL_PAGE_DATA_END))
      return DB_CORRUPTION;

    buf_block_t *cur= add;
    if (addr.page != add->page.id().page_no())
    {
      dberr_t err;
      cur= buf_page_get_gen(page_id_t{add->page.id().space(), addr.page},
                            add->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err);
      if (!cur)
        return err;
    }
    return flst_insert_after(base, boffset, cur, addr.boffset,
                             add, aoffset, limit, mtr);
  }
}

dberr_t flst_add_first(buf_block_t *base, uint16_t boffset,
                       buf_block_t *add, uint16_t aoffset,
                       uint32_t limit, mtr_t *mtr)
{
  ut_ad(base != add || boffset != aoffset);
  ut_ad(boffset < base->physical_size());
  ut_ad(aoffset < add->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(add, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  if (!flst_get_len(base->page.frame + boffset))
  {
    flst_add_to_empty(base, boffset, add, aoffset, mtr);
    return DB_SUCCESS;
  }
  else
  {
    fil_addr_t addr= flst_get_first(base->page.frame + boffset);
    if (UNIV_UNLIKELY(addr.page >= limit))
      return DB_CORRUPTION;
    else if (UNIV_UNLIKELY(addr.boffset < FIL_PAGE_DATA ||
                           addr.boffset >= base->physical_size() -
                           FIL_PAGE_DATA_END))
      return DB_CORRUPTION;
    buf_block_t *cur= add;
    dberr_t err;
    if (addr.page != add->page.id().page_no() &&
        !(cur= buf_page_get_gen(page_id_t{add->page.id().space(), addr.page},
                                add->zip_size(), RW_SX_LATCH, nullptr,
                                BUF_GET_POSSIBLY_FREED, mtr, &err)))
      return err;
    return flst_insert_before(base, boffset, cur, addr.boffset,
                              add, aoffset, limit, mtr);
  }
}

dberr_t flst_remove(buf_block_t *base, uint16_t boffset,
                    buf_block_t *cur, uint16_t coffset,
                    uint32_t limit, mtr_t *mtr)
{
  ut_ad(boffset < base->physical_size());
  ut_ad(coffset < cur->physical_size());
  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));
  ut_ad(mtr->memo_contains_flagged(cur, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  const fil_addr_t prev_addr= flst_get_prev_addr(cur->page.frame + coffset);
  const fil_addr_t next_addr= flst_get_next_addr(cur->page.frame + coffset);
  dberr_t err= DB_SUCCESS;

  if (next_addr.page >= limit)
  {
    if (next_addr.page != FIL_NULL)
      return DB_CORRUPTION;
  }
  else if (UNIV_UNLIKELY(next_addr.boffset < FIL_PAGE_DATA ||
                         next_addr.boffset >= base->physical_size() -
                         FIL_PAGE_DATA_END))
    return DB_CORRUPTION;

  if (prev_addr.page >= limit)
  {
    if (prev_addr.page != FIL_NULL)
      return DB_CORRUPTION;
    flst_write_addr(*base, base->page.frame + boffset + FLST_FIRST,
                    next_addr.page, next_addr.boffset, mtr);
  }
  else if (UNIV_UNLIKELY(prev_addr.boffset < FIL_PAGE_DATA ||
                         prev_addr.boffset >= base->physical_size() -
                         FIL_PAGE_DATA_END))
    return DB_CORRUPTION;
  else
  {
    buf_block_t *b= cur;
    if (prev_addr.page == b->page.id().page_no() ||
        (b= buf_page_get_gen(page_id_t(b->page.id().space(), prev_addr.page),
                             b->zip_size(), RW_SX_LATCH, nullptr,
                             BUF_GET_POSSIBLY_FREED, mtr, &err)))
      flst_write_addr(*b, b->page.frame + prev_addr.boffset + FLST_NEXT,
                      next_addr.page, next_addr.boffset, mtr);
  }

  if (next_addr.page == FIL_NULL)
    flst_write_addr(*base, base->page.frame + boffset + FLST_LAST,
                    prev_addr.page, prev_addr.boffset, mtr);
  else
  {
    if (next_addr.page == cur->page.id().page_no())
    write_addr:
      flst_write_addr(*cur, cur->page.frame + next_addr.boffset + FLST_PREV,
                      prev_addr.page, prev_addr.boffset, mtr);
    else
    {
      dberr_t err2;
      cur= buf_page_get_gen(page_id_t(cur->page.id().space(), next_addr.page),
                            cur->zip_size(), RW_SX_LATCH, nullptr,
                            BUF_GET_POSSIBLY_FREED, mtr, &err2);
      if (cur)
        goto write_addr;
      if (err == DB_SUCCESS)
        err= err2;
    }
  }

  byte *len= &base->page.frame[boffset + FLST_LEN];
  if (UNIV_UNLIKELY(!mach_read_from_4(len)))
    return DB_CORRUPTION;
  mtr->write<4>(*base, len, mach_read_from_4(len) - 1);
  return err;
}

/** Validate a file-based list. */
dberr_t flst_validate(const buf_block_t *base, uint16_t boffset,
                      mtr_t *mtr) noexcept
{
  if (boffset >= base->physical_size())
    return DB_CORRUPTION;

  ut_ad(mtr->memo_contains_flagged(base, MTR_MEMO_PAGE_X_FIX |
                                   MTR_MEMO_PAGE_SX_FIX));

  const uint32_t len= flst_get_len(base->page.frame + boffset);
  fil_addr_t addr= flst_get_first(base->page.frame + boffset);
  dberr_t err= DB_SUCCESS;

  for (uint32_t i= len; i--; )
  {
    if (addr.boffset < FIL_PAGE_DATA ||
        addr.boffset >= base->physical_size() - FIL_PAGE_DATA_END)
      return DB_CORRUPTION;
    const buf_block_t *b=
      buf_page_get_gen(page_id_t(base->page.id().space(), addr.page),
                       base->zip_size(), RW_SX_LATCH, nullptr, BUF_GET, mtr,
                       &err);
    if (!b)
      return err;
    addr= flst_get_next_addr(b->page.frame + addr.boffset);
    mtr->release_last_page();
  }

  if (addr.page != FIL_NULL)
    return DB_CORRUPTION;

  addr= flst_get_last(base->page.frame + boffset);

  for (uint32_t i= len; i--; )
  {
    if (addr.boffset < FIL_PAGE_DATA ||
        addr.boffset >= base->physical_size() - FIL_PAGE_DATA_END)
      return DB_CORRUPTION;
    const buf_block_t *b=
      buf_page_get_gen(page_id_t(base->page.id().space(), addr.page),
                       base->zip_size(), RW_SX_LATCH, nullptr, BUF_GET, mtr,
                       &err);
    if (!b)
      return err;
    addr= flst_get_prev_addr(b->page.frame + addr.boffset);
    mtr->release_last_page();
  }

  if (addr.page != FIL_NULL)
    return DB_CORRUPTION;
  return err;
}
