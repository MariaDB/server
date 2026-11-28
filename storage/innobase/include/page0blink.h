/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#ifndef page0blink_h
#define page0blink_h

#include "btr0btr.h"
#include "dict0mem.h"
#include "mach0data.h"
#include "page0page.h"
#include "rem0rec.h"

/** Non-root index pages do not use the root segment headers. */
constexpr uint16_t PAGE_BLINK_META= PAGE_BTR_SEG_LEAF;
constexpr byte PAGE_BLINK_MAGIC= 0xb1;
constexpr byte PAGE_BLINK_FLAG_ENABLED= 1;
constexpr byte PAGE_BLINK_FLAG_INCOMPLETE_SPLIT= 2;

inline byte page_get_blink_flags(const page_t *page) noexcept
{
  return page[PAGE_HEADER + PAGE_BLINK_META] == PAGE_BLINK_MAGIC
    ? page[PAGE_HEADER + PAGE_BLINK_META + 1] : 0;
}

inline bool page_is_blink(const page_t *page) noexcept
{
  return page_get_blink_flags(page) & PAGE_BLINK_FLAG_ENABLED;
}

inline bool page_has_incomplete_split(const page_t *page) noexcept
{
  return page_get_blink_flags(page) & PAGE_BLINK_FLAG_INCOMPLETE_SPLIT;
}

void page_set_blink_flags(buf_block_t *block, byte flags, mtr_t *mtr);

inline bool rec_is_high_key(const page_t *page, const rec_t *rec) noexcept
{
  return page_is_blink(page) && btr_page_get_next(page) != FIL_NULL &&
    !page_rec_is_infimum(rec) &&
    page_rec_get_next_const(rec) == page_get_supremum_rec(page);
}

inline const rec_t *page_rec_get_next_user(const page_t *page,
                                            const rec_t *rec) noexcept
{
  const rec_t *next= page_rec_get_next_const(rec);
  return next && rec_is_high_key(page, next) ? page_get_supremum_rec(page) : next;
}

inline const rec_t *page_rec_get_prev_user(const page_t *page,
                                            const rec_t *rec) noexcept
{
  const rec_t *prev= page_rec_get_prev_const(rec);
  return prev && rec_is_high_key(page, prev) ? page_rec_get_prev_const(prev) : prev;
}

inline ulint page_get_n_user_recs(const page_t *page) noexcept
{
  const ulint n= page_get_n_recs(page);
  return n - ulint{page_is_blink(page) && btr_page_get_next(page) != FIL_NULL};
}

dtuple_t *blink_read_high_key(const page_t *page, const dict_index_t *index,
                               mem_heap_t *heap);
bool blink_write_high_key(buf_block_t *block, const dtuple_t *key,
                          dict_index_t *index, mtr_t *mtr);
void blink_delete_high_key(buf_block_t *block, dict_index_t *index, mtr_t *mtr);

#endif /* page0blink_h */
