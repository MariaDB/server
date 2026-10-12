/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#include "page0blink.h"

#include "btr0btr.h"
#include "data0data.h"
#include "dict0dict.h"
#include "mem0mem.h"
#include "page0cur.h"
#include "rem0rec.h"

void page_set_blink_flags(buf_block_t *block, byte flags, mtr_t *mtr)
{
  ut_ad(block);
  ut_ad(mtr);
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
  byte *meta= block->page.frame + PAGE_HEADER + PAGE_BLINK_META;
  mtr->write<2>(*block, meta, static_cast<uint16_t>(
                static_cast<uint16_t>(PAGE_BLINK_MAGIC) << 8 | flags));
}

static dtuple_t *blink_make_high_key(const dict_index_t *index,
                                     const dtuple_t *key, mem_heap_t *heap)
{
  const uint16_t n= dict_index_get_n_unique_in_tree_nonleaf(index);
  ut_ad(dtuple_get_n_fields(key) == n);
  dtuple_t *tuple= dtuple_create(heap, n + 1);
  dtuple_set_n_fields_cmp(tuple, n);
  dict_index_copy_types(tuple, index, n);
  for (uint16_t i= 0; i < n; i++)
    *dtuple_get_nth_field(tuple, i)= *dtuple_get_nth_field(key, i);
  byte *child= static_cast<byte*>(mem_heap_alloc(heap, 4));
  mach_write_to_4(child, FIL_NULL);
  dfield_t *field= dtuple_get_nth_field(tuple, n);
  dfield_set_data(field, child, 4);
  dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);
  dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple) | REC_STATUS_NODE_PTR);
  return tuple;
}

dtuple_t *blink_read_high_key(const page_t *page, const dict_index_t *index,
                               mem_heap_t *heap)
{
  if (!page_is_blink(page) || btr_page_get_next(page) == FIL_NULL)
    return nullptr;
  const rec_t *rec= page_rec_get_prev_const(page_get_supremum_rec(page));
  if (!rec_is_high_key(page, rec) || !rec_get_node_ptr_flag(rec))
    return nullptr;
  const uint16_t n= dict_index_get_n_unique_in_tree_nonleaf(index);
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(rec, index, offsets_, 0, n, &heap);
  dtuple_t *key= dtuple_create(heap, n);
  dtuple_set_n_fields_cmp(key, n);
  dict_index_copy_types(key, index, n);
  for (uint16_t i= 0; i < n; i++) {
    ulint len;
    const byte *data= rec_get_nth_field(rec, offsets, i, &len);
    dfield_set_data(dtuple_get_nth_field(key, i), data, len);
  }
  return key;
}

bool blink_write_high_key(buf_block_t *block, const dtuple_t *key,
                          dict_index_t *index, mtr_t *mtr)
{
  ut_ad(page_is_blink(block->page.frame));
  ut_ad(btr_page_get_next(block->page.frame) != FIL_NULL);
  ut_ad(!buf_block_get_page_zip(block));
  mem_heap_t *heap= mem_heap_create(512);
  rec_t *sup= page_get_supremum_rec(block->page.frame);
  rec_t *prev= page_rec_get_prev(sup);
  page_cur_t cur;
  if (rec_is_high_key(block->page.frame, prev) && rec_get_node_ptr_flag(prev)) {
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(prev, index, offsets_, 0,
                                       ULINT_UNDEFINED, &heap);
    page_cur_position(prev, block, &cur);
    page_cur_delete_rec(&cur, offsets, mtr);
    static_cast<void>(page_cur_move_to_prev(&cur));
  } else
    page_cur_position(prev, block, &cur);
  dtuple_t *tuple= blink_make_high_key(index, key, heap);
  rec_offs *offsets= nullptr;
  rec_t *inserted= page_cur_tuple_insert(&cur, tuple, &offsets, &heap, 0, mtr);
  if (!inserted && btr_page_reorganize(&cur, mtr) == DB_SUCCESS)
    inserted= page_cur_tuple_insert(&cur, tuple, &offsets, &heap, 0, mtr);
  mem_heap_free(heap);
  return inserted != nullptr;
}

void blink_delete_high_key(buf_block_t *block, dict_index_t *index, mtr_t *mtr)
{
  page_t *page= block->page.frame;
  if (!page_is_blink(page) || btr_page_get_next(page) == FIL_NULL)
    return;
  rec_t *rec= page_rec_get_prev(page_get_supremum_rec(page));
  ut_ad(rec_is_high_key(page, rec));
  mem_heap_t *heap= mem_heap_create(256);
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(rec, index, offsets_, 0,
                                     ULINT_UNDEFINED, &heap);
  page_cur_t cur;
  page_cur_position(rec, block, &cur);
  page_cur_delete_rec(&cur, offsets, mtr);
  mem_heap_free(heap);
}
