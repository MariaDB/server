/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#include "btr0blink.h"

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "buf0buf.h"
#include "data0data.h"
#include "dict0dict.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "page0blink.h"
#include "page0cur.h"
#include "page0page.h"
#include "rem0cmp.h"
#include "rem0rec.h"

Atomic_counter<uint64_t> blink_searches{0};
Atomic_counter<uint64_t> blink_right_moves{0};
Atomic_counter<uint64_t> blink_optimistic_inserts{0};
Atomic_counter<uint64_t> blink_leaf_splits{0};
Atomic_counter<uint64_t> blink_parent_installs{0};
Atomic_counter<uint64_t> blink_pool_empty{0};

namespace {

bool blink_page_valid(const page_t *page, const dict_index_t *index,
                      uint16_t expected_level) noexcept
{
  return fil_page_index_page_check(page) &&
    fil_page_get_type(page) != FIL_PAGE_RTREE &&
    page_is_comp(page) == index->table->not_redundant() &&
    btr_page_get_index_id(page) == index->id &&
    btr_page_get_level(page) == expected_level;
}

/** Return whether key belongs at or to the right of the page high key. */
bool blink_key_reaches_high_key(const dtuple_t *key, const page_t *page,
                                const dict_index_t *index, mem_heap_t **heap)
{
  if (!page_is_blink(page) || btr_page_get_next(page) == FIL_NULL)
    return false;

  const rec_t *high= page_rec_get_prev_const(page_get_supremum_rec(page));
  if (!rec_is_high_key(page, high) || !rec_get_node_ptr_flag(high))
    return false;

  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(high, index, offsets_, 0,
                                     ULINT_UNDEFINED, heap);
  uint16_t matched= 0;
  return cmp_dtuple_rec_with_match_low(
           key, high, index, offsets,
           dict_index_get_n_unique_in_tree_nonleaf(index), &matched) >= 0;
}

/** Copy the non-leaf unique prefix of a record into stable heap storage. */
dtuple_t *blink_copy_separator(const rec_t *rec, const dict_index_t *index,
                               mem_heap_t *heap)
{
  const uint16_t n= dict_index_get_n_unique_in_tree_nonleaf(index);
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(rec, index, offsets_, 0, n, &heap);
  dtuple_t *tuple= dtuple_create(heap, n);
  dtuple_set_n_fields_cmp(tuple, n);
  dict_index_copy_types(tuple, index, n);

  for (uint16_t i= 0; i < n; ++i) {
    ulint len;
    const byte *data= rec_get_nth_field(rec, offsets, i, &len);
    dfield_t *field= dtuple_get_nth_field(tuple, i);
    dfield_set_data(field, data, len);
    dfield_dup(field, heap);
  }

  return tuple;
}

} /* namespace */

dberr_t blink_search_leaf(dict_index_t *index, const dtuple_t *tuple,
                          page_cur_mode_t mode, rw_lock_type_t leaf_latch,
                          btr_cur_t *cursor, mtr_t *mtr)
{
  if (!use_blink_path(index) || !index->is_primary() ||
      (leaf_latch != RW_S_LATCH && leaf_latch != RW_X_LATCH))
    return DB_UNSUPPORTED;

  ++blink_searches;
  ut_ad(mtr);
  ut_ad(index->page != FIL_NULL);
  ut_ad(dict_index_check_search_tuple(index, tuple));
  ut_ad(dtuple_check_typed(tuple));

  if (!mtr->memo_contains_flagged(&index->lock, MTR_MEMO_S_LOCK |
                                  MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK))
    mtr_s_lock_index(index, mtr);

  mem_heap_t *heap= nullptr;
  uint32_t page_no= index->page;
  uint16_t expected_level= UINT16_MAX;
  dberr_t err= DB_SUCCESS;

  for (;;) {
    const rw_lock_type_t latch= expected_level == 0 ? leaf_latch : RW_S_LATCH;
    const ulint savepoint= mtr->get_savepoint();
    buf_block_t *block= btr_block_get(*index, page_no, latch, mtr, &err);
    if (!block)
      break;

    page_t *page= block->page.frame;
    const uint16_t level= btr_page_get_level(page);
    if (expected_level == UINT16_MAX) {
      expected_level= level;
      cursor->tree_height= ulint{level} + 1;
      if (!blink_page_valid(page, index, expected_level)) {
        err= DB_CORRUPTION;
        break;
      }
      if (!level && leaf_latch == RW_X_LATCH) {
        mtr->rollback_to_savepoint(savepoint);
        block= btr_block_get(*index, page_no, RW_X_LATCH, mtr, &err);
        if (!block)
          break;
        page= block->page.frame;
        if (!blink_page_valid(page, index, 0)) {
          err= DB_CORRUPTION;
          break;
        }
      }
    } else if (!blink_page_valid(page, index, expected_level)) {
      err= DB_CORRUPTION;
      break;
    }

    while (blink_key_reaches_high_key(tuple, page, index, &heap)) {
      ++blink_right_moves;
      const uint32_t next= btr_page_get_next(page);
      mtr->rollback_to_savepoint(savepoint);
      block= btr_block_get(*index, next, latch, mtr, &err);
      if (!block)
        goto func_exit;
      page= block->page.frame;
      if (!blink_page_valid(page, index, expected_level)) {
        err= DB_CORRUPTION;
        goto func_exit;
      }
    }

    cursor->page_cur.block= block;
    cursor->page_cur.index= index;
    cursor->up_match= cursor->low_match= 0;

    if (level == 0) {
      if (page_cur_search_with_match(tuple, mode, &cursor->up_match,
                                     &cursor->low_match, &cursor->page_cur,
                                     nullptr))
        err= DB_CORRUPTION;
      break;
    }

    if (page_cur_search_with_match(tuple, PAGE_CUR_LE, &cursor->up_match,
                                   &cursor->low_match, &cursor->page_cur,
                                   nullptr)) {
      err= DB_CORRUPTION;
      break;
    }

    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(cursor->page_cur.rec, index, offsets_,
                                       0, ULINT_UNDEFINED, &heap);
    const uint32_t child=
      btr_node_ptr_get_child_page_no(cursor->page_cur.rec, offsets);

    /* Deliberately release the parent before acquiring the child. The index
    S-latch keeps root height stable; high keys repair stale child choices. */
    mtr->rollback_to_savepoint(savepoint);
    page_no= child;
    --expected_level;
  }

func_exit:
  if (heap)
    mem_heap_free(heap);
  return err;
}

dberr_t blink_optimistic_insert(ulint flags, btr_cur_t *cursor,
                                rec_offs **offsets, mem_heap_t **heap,
                                dtuple_t *entry, rec_t **rec,
                                big_rec_t **big_rec, ulint n_ext,
                                que_thr_t *thr, mtr_t *mtr)
{
  if (!use_blink_path(cursor->index()) || !cursor->index()->is_primary() ||
      !page_is_leaf(cursor->block()->page.frame) || cursor->block()->zip_size())
    return DB_UNSUPPORTED;

  ++blink_optimistic_inserts;
  return btr_cur_optimistic_insert(flags, cursor, offsets, heap, entry, rec,
                                   big_rec, n_ext, thr, mtr);
}

dberr_t blink_split_leaf_and_insert(ulint flags, btr_cur_t *cursor,
                                    buf_block_t *right, dtuple_t *entry,
                                    rec_offs **offsets, mem_heap_t **heap,
                                    rec_t **rec, big_rec_t **big_rec,
                                    ulint n_ext, que_thr_t *thr, mtr_t *mtr)
{
  dict_index_t *index= cursor->index();
  buf_block_t *left= cursor->block();
  page_t *left_page= left->page.frame;

  if (!use_blink_path(index) || !index->is_primary() || n_ext ||
      left->zip_size() || right->zip_size() || !page_is_leaf(left_page) ||
      !page_is_leaf(right->page.frame) ||
      left->page.id().page_no() == index->page ||
      page_get_n_user_recs(left_page) < 2 ||
      page_get_n_recs(right->page.frame))
    return DB_FAIL;

  ++blink_leaf_splits;
  ut_ad(mtr->memo_contains_flagged(&index->lock, MTR_MEMO_S_LOCK |
                                   MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK));
  ut_ad(mtr->memo_contains_flagged(left, MTR_MEMO_PAGE_X_FIX));
  ut_ad(mtr->memo_contains_flagged(right, MTR_MEMO_PAGE_X_FIX));

  const ulint n_user= page_get_n_user_recs(left_page);
  rec_t *split_rec= page_rec_get_nth(left_page, 1 + n_user / 2);
  if (!split_rec || page_rec_is_supremum(split_rec) ||
      rec_is_high_key(left_page, split_rec))
    return DB_CORRUPTION;

  mem_heap_t *separator_heap= mem_heap_create(512);
  dtuple_t *separator= blink_copy_separator(split_rec, index, separator_heap);
  rec_offs split_offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(split_offsets_);
  rec_offs *split_offsets= rec_get_offsets(split_rec, index, split_offsets_,
                                            index->n_core_fields,
                                            ULINT_UNDEFINED, &separator_heap);
  const bool insert_right= cmp_dtuple_rec(entry, split_rec, index,
                                          split_offsets) >= 0;
  const uint32_t old_next= btr_page_get_next(left_page);
  if (old_next != FIL_NULL && !page_is_blink(left_page)) {
    mem_heap_free(separator_heap);
    return DB_FAIL;
  }
  dberr_t err= DB_SUCCESS;

  if (!page_copy_rec_list_end(right, left, split_rec, index, mtr, &err)) {
    mem_heap_free(separator_heap);
    return err;
  }

  err= page_delete_rec_list_end(split_rec, left, index, ULINT_UNDEFINED,
                                ULINT_UNDEFINED, mtr);
  if (err != DB_SUCCESS) {
    mem_heap_free(separator_heap);
    return err;
  }

  btr_page_set_prev(right, left->page.id().page_no(), mtr);
  btr_page_set_next(right, old_next, mtr);
  btr_page_set_next(left, right->page.id().page_no(), mtr);
  page_set_blink_flags(right, PAGE_BLINK_FLAG_ENABLED, mtr);
  page_set_blink_flags(left, PAGE_BLINK_FLAG_ENABLED |
                       PAGE_BLINK_FLAG_INCOMPLETE_SPLIT, mtr);

  if (!blink_write_high_key(left, separator, index, mtr)) {
    mem_heap_free(separator_heap);
    return DB_CORRUPTION;
  }

  btr_search_move_or_delete_hash_entries(right, left, *mtr);
  cursor->page_cur.block= insert_right ? right : left;
  cursor->page_cur.index= index;
  cursor->up_match= cursor->low_match= 0;
  if (page_cur_search_with_match(entry, PAGE_CUR_LE, &cursor->up_match,
                                 &cursor->low_match, &cursor->page_cur,
                                 nullptr))
    err= DB_CORRUPTION;
  else
    err= blink_optimistic_insert(flags, cursor, offsets, heap, entry, rec,
                                 big_rec, n_ext, thr, mtr);

  mem_heap_free(separator_heap);
  return err;
}

dberr_t blink_install_parent(dict_index_t *index, uint32_t left_page,
                             uint32_t right_page, trx_t *trx)
{
  if (!use_blink_path(index) || !index->is_primary() ||
      left_page == index->page)
    return DB_UNSUPPORTED;

  mtr_t mtr(trx);
  mtr.start();
  mtr.set_named_space(index->table->space);
  mtr_x_lock_index(index, &mtr);

  dberr_t err= DB_SUCCESS;
  mem_heap_t *heap= mem_heap_create(512);
  const ulint savepoint= mtr.get_savepoint();
  buf_block_t *right= btr_block_get(*index, right_page, RW_S_LATCH, &mtr, &err);
  dtuple_t *node_ptr= nullptr;

  if (!right || !page_is_leaf(right->page.frame) ||
      btr_page_get_prev(right->page.frame) != left_page) {
    err= right ? DB_CORRUPTION : err;
  } else {
    const rec_t *first= page_rec_get_next_const(
      page_get_infimum_rec(right->page.frame));
    if (!first || page_rec_is_supremum(first) ||
        rec_is_high_key(right->page.frame, first))
      err= DB_CORRUPTION;
    else {
      node_ptr= dict_index_build_node_ptr(index, first, right_page, heap, 0);
      for (ulint i= 0; i < dtuple_get_n_fields(node_ptr); ++i)
        dfield_dup(dtuple_get_nth_field(node_ptr, i), heap);
    }
  }

  mtr.rollback_to_savepoint(savepoint);
  if (err == DB_SUCCESS)
    err= btr_insert_on_non_leaf_level(BTR_NO_LOCKING_FLAG |
                                     BTR_NO_UNDO_LOG_FLAG |
                                     BTR_KEEP_SYS_FLAG,
                                     index, 1, node_ptr, &mtr);

  if (err == DB_SUCCESS) {
    ++blink_parent_installs;
    buf_block_t *left= btr_block_get(*index, left_page, RW_X_LATCH, &mtr, &err);
    if (left && page_has_incomplete_split(left->page.frame))
      page_set_blink_flags(left, PAGE_BLINK_FLAG_ENABLED, &mtr);
    else if (left)
      err= DB_CORRUPTION;
  }

  mem_heap_free(heap);
  mtr.commit();
  return err;
}
