#include "page0blink.h"

#include "btr0btr.h"
#include "buf0buf.h"
#include "data0data.h"
#include "dict0dict.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "page0cur.h"
#include "rem0rec.h"

static void page_set_split_direction(buf_block_t *block, byte direction,
                                     mtr_t *mtr)
{
  ut_ad(block);
  ut_ad(mtr);
  ut_ad(mtr->memo_contains_flagged(block, MTR_MEMO_PAGE_X_FIX));
  byte *field= block->page.frame + PAGE_HEADER + PAGE_DIRECTION_B;
  const byte value= static_cast<byte>((*field & 0xf8U) | direction);
  mtr->write<1,mtr_t::MAYBE_NOP>(*block, field, value);
  mtr->write<2,mtr_t::MAYBE_NOP>(
    *block, block->page.frame + PAGE_HEADER + PAGE_N_DIRECTION, 0U);
  if (UNIV_LIKELY_NULL(block->page.zip.data)) {
    block->page.zip.data[PAGE_HEADER + PAGE_DIRECTION_B]= value;
    mach_write_to_2(block->page.zip.data + PAGE_HEADER + PAGE_N_DIRECTION, 0);
  }
}

void page_set_incomplete_split(buf_block_t *block, mtr_t *mtr)
{
  page_set_split_direction(block, PAGE_INCOMPLETE_SPLIT, mtr);
}

void page_clear_incomplete_split(buf_block_t *block, mtr_t *mtr)
{
  page_set_split_direction(block, PAGE_NO_DIRECTION, mtr);
}

bool rec_is_high_key_structural(const page_t *page, const rec_t *rec,
                                const dict_index_t *index)
{
  if (!(index->type & DICT_BLINK) || !rec ||
      btr_page_get_next(page) == FIL_NULL || page_rec_is_infimum(rec) ||
      page_rec_get_next_const(rec) != page_get_supremum_rec(page) ||
      !rec_get_node_ptr_flag(rec))
    return false;

  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(rec, index, offsets_, 0,
                                     ULINT_UNDEFINED, &heap);
  ulint len;
  const byte *child= rec_get_nth_field(
    rec, offsets, rec_offs_n_fields(offsets) - 1, &len);
  const bool high_key= len == 4 && mach_read_from_4(child) == FIL_NULL;
  if (heap)
    mem_heap_free(heap);
  return high_key;
}

static dtuple_t *blink_make_high_key_record(const dict_index_t *index,
                                            const dtuple_t *key,
                                            mem_heap_t *heap)
{
  const uint16_t n= dict_index_get_n_unique_in_tree_nonleaf(index);
  ut_ad(dtuple_get_n_fields(key) == n);
  dtuple_t *tuple= dtuple_create(heap, n + 1);
  dtuple_set_n_fields_cmp(tuple, n);
  dict_index_copy_types(tuple, index, n);
  for (uint16_t i= 0; i < n; ++i)
    *dtuple_get_nth_field(tuple, i)= *dtuple_get_nth_field(key, i);
  byte *child= static_cast<byte*>(mem_heap_alloc(heap, 4));
  mach_write_to_4(child, FIL_NULL);
  dfield_t *field= dtuple_get_nth_field(tuple, n);
  dfield_set_data(field, child, 4);
  dtype_set(dfield_get_type(field), DATA_SYS_CHILD, DATA_NOT_NULL, 4);
  dtuple_set_info_bits(tuple, dtuple_get_info_bits(tuple) | REC_STATUS_NODE_PTR);
  return tuple;
}

dtuple_t *blink_read_high_key_record(const page_t *page,
                                      const dict_index_t *index,
                                      mem_heap_t *heap)
{
  if (btr_page_get_next(page) == FIL_NULL)
    return nullptr;
  const rec_t *record= page_rec_get_prev_const(page_get_supremum_rec(page));
  if (!rec_is_high_key_structural(page, record, index))
    return nullptr;

  const uint16_t n= dict_index_get_n_unique_in_tree_nonleaf(index);
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(record, index, offsets_, 0, n, &heap);
  dtuple_t *key= dtuple_create(heap, n);
  dtuple_set_n_fields_cmp(key, n);
  dict_index_copy_types(key, index, n);
  for (uint16_t i= 0; i < n; ++i) {
    ulint len;
    const byte *data= rec_get_nth_field(record, offsets, i, &len);
    dfield_t *field= dtuple_get_nth_field(key, i);
    dfield_set_data(field, data, len);
    dfield_dup(field, heap);
  }
  return key;
}

bool blink_write_high_key_record(buf_block_t *block, const dtuple_t *key,
                                 dict_index_t *index, mtr_t *mtr)
{
  ut_ad(index->type & DICT_BLINK);
  ut_ad(btr_page_get_next(block->page.frame) != FIL_NULL);
  ut_ad(!block->zip_size());
  mem_heap_t *heap= mem_heap_create(512);
  rec_t *supremum= page_get_supremum_rec(block->page.frame);
  rec_t *previous= page_rec_get_prev(supremum);
  page_cur_t cursor;

  if (rec_is_high_key_structural(block->page.frame, previous, index)) {
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(previous, index, offsets_, 0,
                                       ULINT_UNDEFINED, &heap);
    page_cur_position(previous, block, &cursor);
    page_cur_delete_rec(&cursor, offsets, mtr);
    static_cast<void>(page_cur_move_to_prev(&cursor));
  } else
    page_cur_position(previous, block, &cursor);

  cursor.index= index;
  dtuple_t *tuple= blink_make_high_key_record(index, key, heap);
  mtr->write<2>(*block,
                block->page.frame + PAGE_HEADER + PAGE_LAST_INSERT, 0U);
  rec_offs *offsets= nullptr;
  rec_t *inserted= page_cur_tuple_insert(
    &cursor, tuple, &offsets, &heap, 0, mtr);
  if (!inserted && btr_page_reorganize(&cursor, mtr) == DB_SUCCESS) {
    mtr->write<2>(*block,
                  block->page.frame + PAGE_HEADER + PAGE_LAST_INSERT, 0U);
    inserted= page_cur_tuple_insert(&cursor, tuple, &offsets, &heap, 0, mtr);
  }
  if (inserted)
    mtr->write<2>(*block,
                  block->page.frame + PAGE_HEADER + PAGE_LAST_INSERT, 0U);
  const bool success= inserted && rec_is_high_key_structural(
    block->page.frame, inserted, index);
  mem_heap_free(heap);
  return success;
}

void blink_delete_high_key_record(buf_block_t *block, dict_index_t *index,
                                  mtr_t *mtr)
{
  rec_t *record= page_rec_get_prev(page_get_supremum_rec(block->page.frame));
  ut_ad(rec_is_high_key_structural(block->page.frame, record, index));
  mem_heap_t *heap= nullptr;
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(record, index, offsets_, 0,
                                     ULINT_UNDEFINED, &heap);
  page_cur_t cursor;
  page_cur_position(record, block, &cursor);
  cursor.index= index;
  page_cur_delete_rec(&cursor, offsets, mtr);
  if (heap)
    mem_heap_free(heap);
}
