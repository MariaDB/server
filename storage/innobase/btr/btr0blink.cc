#include "btr0blink.h"

#include "btr0blink_alloc.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "buf0buf.h"
#include "dict0dict.h"
#include "lock0lock.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "my_cpu.h"
#include "page0blink.h"
#include "page0cur.h"
#include "que0que.h"
#include "rem0cmp.h"
#include "rem0rec.h"
#include "trx0trx.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

Atomic_counter<uint64_t> blink_searches;
Atomic_counter<uint64_t> blink_right_moves;
Atomic_counter<uint64_t> blink_optimistic_inserts;
Atomic_counter<uint64_t> blink_leaf_splits;
Atomic_counter<uint64_t> blink_internal_splits;
Atomic_counter<uint64_t> blink_root_raises;
Atomic_counter<uint64_t> blink_parent_installs;
Atomic_counter<uint64_t> blink_cascade_levels;
Atomic_counter<uint64_t> blink_incomplete_retries;
Atomic_counter<uint64_t> blink_pool_empty_retries;
Atomic_counter<uint64_t> blink_pool_refills;
Atomic_counter<uint64_t> blink_normal_x_index;

bool blink_stamp_empty_tree(dict_index_t *index)
{
  ut_ad(dict_sys.locked());
  if (!blink_index_shape_ok(index) || index->type & DICT_BLINK ||
      !index->is_committed())
    return false;

  mtr_t mtr{nullptr};
  mtr.start();
  mtr_x_lock_index(index, &mtr);
  dberr_t err= DB_SUCCESS;
  buf_block_t *root= btr_root_block_get(index, RW_X_LATCH, &mtr, &err);
  if (!root || btr_page_get_level(root->page.frame)) {
    mtr.commit();
    return false;
  }

#ifdef BTR_CUR_HASH_ADAPT
  btr_search_drop_page_hash_index(root, nullptr);
  index->search_info.set_enabled_fixed_mask(
    dict_index_t::ahi::AHI_INDEX_FORCE_DISABLED,
    false, false, false, 0, 0, false);
#endif
  index->type|= DICT_BLINK;
  const bool success= dict_index_persist_type(index);
  if (!success)
    index->type&= ~DICT_BLINK;
  mtr.commit();
  if (success)
    blink_page_pool_register(index);
  return success;
}

static bool blink_page_matches(const page_t *page, const dict_index_t *index,
                               uint16_t level)
{
  if (!fil_page_index_page_check(page) ||
      btr_page_get_index_id(page) != index->id ||
      btr_page_get_level(page) != level ||
      !!page_is_comp(page) != index->table->not_redundant())
    return false;
  if (level && !page_get_n_user_recs(page, index))
    return false;
  if (btr_page_get_next(page) == FIL_NULL)
    return true;
  return rec_is_high_key_structural(
    page, page_rec_get_prev_const(page_get_supremum_rec(page)), index);
}

static buf_block_t *blink_move_right(buf_block_t *block,
                                     const dtuple_t *tuple,
                                     page_cur_mode_t mode,
                                     rw_lock_type_t latch,
                                     dict_index_t *index, mtr_t *mtr,
                                     dberr_t *err)
{
  const uint16_t level= static_cast<uint16_t>(
    btr_page_get_level(block->page.frame));
  for (;;) {
    page_t *page= block->page.frame;
    const uint32_t next= btr_page_get_next(page);
    if (next == FIL_NULL)
      return block;
    const rec_t *high_key=
      page_rec_get_prev_const(page_get_supremum_rec(page));
    if (!rec_is_high_key_structural(page, high_key, index)) {
      *err= DB_CORRUPTION;
      mtr->release(*block);
      return nullptr;
    }
    mem_heap_t *heap= nullptr;
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(high_key, index, offsets_, 0,
                                       ULINT_UNDEFINED, &heap);
    const int comparison= cmp_dtuple_rec(tuple, high_key, index, offsets);
    if (heap)
      mem_heap_free(heap);
    if (comparison < 0 || (comparison == 0 && mode == PAGE_CUR_L))
      return block;
    buf_block_t *right= btr_block_get(*index, next, latch, mtr, err);
    if (!right || !blink_page_matches(right->page.frame, index, level)) {
      if (right) {
        *err= DB_CORRUPTION;
        mtr->release(*right);
      }
      mtr->release(*block);
      return nullptr;
    }
    mtr->release(*block);
    block= right;
    ++blink_right_moves;
  }
}

static page_cur_mode_t blink_internal_mode(page_cur_mode_t mode)
{
  if (mode == PAGE_CUR_GE)
    return PAGE_CUR_L;
  if (mode == PAGE_CUR_G)
    return PAGE_CUR_LE;
  return mode;
}

dberr_t blink_search_to_level(dict_index_t *index, uint16_t target_level,
                              const dtuple_t *tuple, page_cur_mode_t mode,
                              rw_lock_type_t target_latch, bool index_latched,
                              btr_cur_t *cursor, mtr_t *mtr)
{
  ut_ad(use_blink_path(index));
  ut_ad(target_latch == RW_S_LATCH || target_latch == RW_X_LATCH);
  ut_ad(dict_index_check_search_tuple(index, tuple));
  ut_ad(dtuple_check_typed(tuple));
  ut_ad(index->page != FIL_NULL);
  ut_ad(!index_latched || mtr->memo_contains_flagged(
    &index->lock, MTR_MEMO_S_LOCK | MTR_MEMO_SX_LOCK | MTR_MEMO_X_LOCK));

  if (!index_latched)
    mtr_s_lock_index(index, mtr);
  ++blink_searches;

  cursor->page_cur.index= index;
  cursor->up_match= 0;
  cursor->low_match= 0;
#ifdef BTR_CUR_HASH_ADAPT
  cursor->flag= BTR_CUR_BINARY;
#endif

  dberr_t err= DB_SUCCESS;
  uint16_t current_level= UINT16_MAX;

restart:
  page_id_t page_id(index->table->space_id, index->page);
  current_level= UINT16_MAX;

  for (;;) {
    rw_lock_type_t latch= current_level != UINT16_MAX &&
      current_level == target_level ? target_latch : RW_S_LATCH;
    buf_block_t *block;
    if (current_level == UINT16_MAX)
      block= btr_root_block_get(index, RW_S_LATCH, mtr, &err);
    else
      block= btr_block_get(*index, page_id.page_no(), latch, mtr, &err);
    if (!block)
      return err;

    page_t *page= block->page.frame;
    if (current_level == UINT16_MAX) {
      if (!fil_page_index_page_check(page) ||
          btr_page_get_index_id(page) != index->id) {
        mtr->release(*block);
        return DB_CORRUPTION;
      }
      current_level= static_cast<uint16_t>(btr_page_get_level(page));
      if (!blink_page_matches(page, index, current_level)) {
        mtr->release(*block);
        return DB_CORRUPTION;
      }
      cursor->tree_height= current_level + 1;
      if (current_level < target_level) {
        mtr->release(*block);
        return DB_CORRUPTION;
      }
      if (current_level == target_level && target_latch == RW_X_LATCH) {
        mtr->release(*block);
        block= btr_root_block_get(index, RW_X_LATCH, mtr, &err);
        if (!block)
          return err;
        page= block->page.frame;
        if (!fil_page_index_page_check(page) ||
            btr_page_get_index_id(page) != index->id) {
          mtr->release(*block);
          return DB_CORRUPTION;
        }
        if (btr_page_get_level(page) != current_level) {
          mtr->release(*block);
          goto restart;
        }
        if (!blink_page_matches(page, index, current_level)) {
          mtr->release(*block);
          return DB_CORRUPTION;
        }
        latch= RW_X_LATCH;
      }
    } else if (!blink_page_matches(page, index, current_level)) {
      mtr->release(*block);
      return DB_CORRUPTION;
    }

    block= blink_move_right(block, tuple, mode, latch, index, mtr, &err);
    if (!block)
      return err;
    page= block->page.frame;
    cursor->page_cur.block= block;
    const page_cur_mode_t search_mode= current_level == target_level
      ? mode : blink_internal_mode(mode);
    if (page_cur_search_with_match(tuple, search_mode, &cursor->up_match,
                                   &cursor->low_match, &cursor->page_cur,
                                   nullptr)) {
      mtr->release(*block);
      return DB_CORRUPTION;
    }
    if (current_level == target_level)
      return DB_SUCCESS;

    rec_t *node_ptr= cursor->page_cur.rec;
    if (rec_is_high_key(page, node_ptr, index))
      node_ptr= page_rec_get_prev(node_ptr);
    else if (page_rec_is_infimum(node_ptr))
      node_ptr= page_rec_get_next(node_ptr);
    if (page_rec_is_infimum(node_ptr) || page_rec_is_supremum(node_ptr) ||
        rec_is_high_key(page, node_ptr, index)) {
      mtr->release(*block);
      return DB_CORRUPTION;
    }

    mem_heap_t *heap= nullptr;
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(node_ptr, index, offsets_, 0,
                                       ULINT_UNDEFINED, &heap);
    const uint32_t child= btr_node_ptr_get_child_page_no(node_ptr, offsets);
    if (heap)
      mem_heap_free(heap);
    if (child == FIL_NULL) {
      mtr->release(*block);
      return DB_CORRUPTION;
    }
    mtr->release(*block);
    page_id.set_page_no(child);
    --current_level;
  }
}

static ulint blink_max_high_key_record_size(const dict_index_t *index)
{
  ulint size= 4 + REC_N_NEW_EXTRA_BYTES + PAGE_DIR_SLOT_SIZE;
  ulint variable= 0;
  ulint nullable= 0;
  const uint16_t fields= dict_index_get_n_unique_in_tree_nonleaf(index);
  for (uint16_t i= 0; i < fields; ++i) {
    const dict_field_t *field= dict_index_get_nth_field(index, i);
    if (field->fixed_len)
      size+= field->fixed_len;
    else {
      size+= field->prefix_len ? field->prefix_len : field->col->len;
      ++variable;
    }
    if (!(field->col->prtype & DATA_NOT_NULL))
      ++nullable;
  }
  return size + 2 * variable + UT_BITS_IN_BYTES(nullable);
}

bool blink_split_choose_and_check_fit(btr_cur_t *cursor, const dtuple_t *tuple,
                                      ulint n_ext, rec_t **split_rec,
                                      bool *insert_left, mem_heap_t **heap)
{
  page_t *page= btr_cur_get_page(cursor);
  dict_index_t *index= cursor->index();
  std::vector<rec_t*> records;
  std::vector<ulint> sizes;
  ulint total_size= 0;
  for (rec_t *record= page_rec_get_next(page_get_infimum_rec(page));
       !page_rec_is_supremum(record);
       record= page_rec_get_next(record)) {
    if (rec_is_high_key(page, record, index))
      break;
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(
      record, index, offsets_, page_is_leaf(page) ? index->n_core_fields : 0,
      ULINT_UNDEFINED, heap);
    records.push_back(record);
    sizes.push_back(rec_offs_size(offsets));
    total_size+= sizes.back();
  }
  if (records.empty())
    return false;

  const ulint tuple_size= rec_get_converted_size(index, tuple, n_ext);
  const ulint high_key_size= blink_max_high_key_record_size(index);
  const ulint free_space= page_get_free_space_of_empty(page_is_comp(page));
  const bool right_has_high_key= btr_page_get_next(page) != FIL_NULL;
  ulint left_data= 0;
  ulint best_score= std::numeric_limits<ulint>::max();
  size_t best= records.size() + 1;
  bool best_insert_left= false;

  for (size_t boundary= 0; boundary <= records.size(); ++boundary) {
    bool tuple_left= false;
    if (boundary < records.size()) {
      rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
      rec_offs_init(offsets_);
      rec_offs *offsets= rec_get_offsets(
        records[boundary], index, offsets_,
        page_is_leaf(page) ? index->n_core_fields : 0,
        ULINT_UNDEFINED, heap);
      tuple_left= cmp_dtuple_rec(tuple, records[boundary], index, offsets) < 0;
    }
    const ulint left_count= boundary + ulint{tuple_left};
    const ulint right_count= records.size() - boundary + ulint{!tuple_left};
    if (!left_count || !right_count)
      goto next_boundary;
    {
      const ulint left_bytes= left_data + (tuple_left ? tuple_size : 0) +
        high_key_size + page_dir_calc_reserved_space(left_count + 1);
      const ulint right_bytes= total_size - left_data +
        (!tuple_left ? tuple_size : 0) +
        (right_has_high_key ? high_key_size : 0) +
        page_dir_calc_reserved_space(right_count + right_has_high_key);
      if (left_bytes <= free_space && right_bytes <= free_space) {
        const ulint score= std::max(left_bytes, right_bytes);
        if (score < best_score) {
          best_score= score;
          best= boundary;
          best_insert_left= tuple_left;
        }
      }
    }
next_boundary:
    if (boundary < records.size())
      left_data+= sizes[boundary];
  }

  if (best > records.size())
    return false;
  *split_rec= best == records.size() ? nullptr : records[best];
  *insert_left= best_insert_left;
  return true;
}

static dtuple_t *blink_copy_key(const rec_t *record, dict_index_t *index,
                                mem_heap_t *heap)
{
  const uint16_t fields= dict_index_get_n_unique_in_tree_nonleaf(index);
  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  rec_offs_init(offsets_);
  rec_offs *offsets= rec_get_offsets(
    record, index, offsets_, page_rec_is_leaf(record) ? index->n_core_fields : 0,
    ULINT_UNDEFINED, &heap);
  dtuple_t *key= dtuple_create(heap, fields);
  dtuple_set_n_fields_cmp(key, fields);
  dict_index_copy_types(key, index, fields);
  for (uint16_t i= 0; i < fields; ++i) {
    ulint len;
    const byte *data= rec_get_nth_field(record, offsets, i, &len);
    dfield_t *field= dtuple_get_nth_field(key, i);
    dfield_set_data(field, data, len);
    dfield_dup(field, heap);
  }
  return key;
}

rec_t *blink_split_page_and_insert(ulint flags, btr_cur_t *cursor,
                                   rec_offs **offsets, mem_heap_t **heap,
                                   dtuple_t *tuple,
                                   ulint n_ext, buf_block_t *new_block,
                                   buf_block_t *old_right, mtr_t *mtr)
{
  dict_index_t *index= cursor->index();
  buf_block_t *left= cursor->block();
  page_t *left_page= left->page.frame;
  const uint32_t old_next= btr_page_get_next(left_page);
  if (!use_blink_path(index) || left->zip_size() || new_block->zip_size() ||
      left->page.id().page_no() == index->page || page_is_empty(left_page) ||
      page_get_n_recs(new_block->page.frame) || new_block == left ||
      (old_next == FIL_NULL) != (old_right == nullptr))
    return nullptr;
  if (old_right &&
      (old_right->page.id().page_no() != old_next ||
       btr_page_get_index_id(old_right->page.frame) != index->id ||
       btr_page_get_level(old_right->page.frame) !=
         btr_page_get_level(left_page)))
    return nullptr;
  ut_ad(mtr->memo_contains_flagged(
    &index->lock, MTR_MEMO_S_LOCK | MTR_MEMO_SX_LOCK | MTR_MEMO_X_LOCK));
  ut_ad(mtr->memo_contains_flagged(left, MTR_MEMO_PAGE_X_FIX));
  ut_ad(mtr->memo_contains_flagged(new_block, MTR_MEMO_PAGE_X_FIX));
  ut_ad(!old_right ||
        mtr->memo_contains_flagged(old_right, MTR_MEMO_PAGE_X_FIX));

  if (!*heap)
    *heap= mem_heap_create(1024);
  if (old_next != FIL_NULL) {
    rec_t *high_key= page_rec_get_prev(page_get_supremum_rec(left_page));
    if (!rec_is_high_key_structural(left_page, high_key, index))
      return nullptr;
    rec_offs high_offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(high_offsets_);
    rec_offs *high_offsets= rec_get_offsets(high_key, index, high_offsets_, 0,
                                             ULINT_UNDEFINED, heap);
    if (cmp_dtuple_rec(tuple, high_key, index, high_offsets) >= 0)
      return nullptr;
  }
  rec_t *split_rec= nullptr;
  bool insert_left= false;
  if (!blink_split_choose_and_check_fit(cursor, tuple, n_ext, &split_rec,
                                        &insert_left, heap))
    return nullptr;

  dtuple_t *old_high_key= old_next == FIL_NULL ? nullptr :
    blink_read_high_key_record(left_page, index, *heap);
  if (old_next != FIL_NULL && !old_high_key)
    return nullptr;
  dtuple_t *lower_high_key= split_rec
    ? blink_copy_key(split_rec, index, *heap) : nullptr;
  if (!lower_high_key) {
    const uint16_t fields= dict_index_get_n_unique_in_tree_nonleaf(index);
    lower_high_key= dtuple_create(*heap, fields);
    dtuple_set_n_fields_cmp(lower_high_key, fields);
    dict_index_copy_types(lower_high_key, index, fields);
    for (uint16_t i= 0; i < fields; ++i) {
      *dtuple_get_nth_field(lower_high_key, i)=
        *dtuple_get_nth_field(tuple, i);
      dfield_dup(dtuple_get_nth_field(lower_high_key, i), *heap);
    }
  }

  const ulint level= btr_page_get_level(left_page);
  if (level)
    ++blink_internal_splits;
  else
    ++blink_leaf_splits;
  btr_page_create(new_block, nullptr, index, level, mtr);
  if (level == 0 && !index->is_primary() && !index->table->is_temporary()) {
    const trx_id_t max_trx_id= page_get_max_trx_id(left_page);
    if (max_trx_id)
      page_update_max_trx_id(new_block, nullptr, max_trx_id, mtr);
  }
  if (old_high_key)
    blink_delete_high_key_record(left, index, mtr);

  if (split_rec) {
    dberr_t err= DB_SUCCESS;
    ut_a(page_copy_rec_list_end(new_block, left, split_rec, index, mtr, &err));
    ut_a(err == DB_SUCCESS);
    err= page_delete_rec_list_end(split_rec, left, index,
                                  ULINT_UNDEFINED, ULINT_UNDEFINED, mtr);
    ut_a(err == DB_SUCCESS);
  }

  if (!(flags & BTR_NO_LOCKING_FLAG) && index->has_locking())
    lock_update_split_right(new_block, left);
#ifdef BTR_CUR_HASH_ADAPT
  btr_search_move_or_delete_hash_entries(new_block, left, *mtr);
#endif
  btr_page_set_prev(new_block, left->page.id().page_no(), mtr);
  btr_page_set_next(new_block, old_next, mtr);
  btr_page_set_next(left, new_block->page.id().page_no(), mtr);
  if (old_right)
    btr_page_set_prev(old_right, new_block->page.id().page_no(), mtr);
  ut_a(blink_write_high_key_record(left, lower_high_key, index, mtr));
  if (old_high_key)
    ut_a(blink_write_high_key_record(new_block, old_high_key, index, mtr));

  buf_block_t *insert_block= insert_left ? left : new_block;
  cursor->page_cur.block= insert_block;
  cursor->page_cur.index= index;
  cursor->up_match= cursor->low_match= 0;
  ut_a(!page_cur_search_with_match(tuple, PAGE_CUR_LE, &cursor->up_match,
                                   &cursor->low_match, &cursor->page_cur,
                                   nullptr));
  rec_t *inserted= page_cur_tuple_insert(&cursor->page_cur, tuple, offsets,
                                         heap, n_ext, mtr);
  if (!inserted) {
    ut_a(btr_page_reorganize(&cursor->page_cur, mtr) == DB_SUCCESS);
    inserted= page_cur_tuple_insert(&cursor->page_cur, tuple, offsets,
                                    heap, n_ext, mtr);
    ut_a(inserted);
  }
  page_set_incomplete_split(left, mtr);
  ut_ad(rec_is_high_key_structural(
    left->page.frame,
    page_rec_get_prev_const(page_get_supremum_rec(left->page.frame)), index));
  ut_ad(btr_page_get_next(new_block->page.frame) == FIL_NULL ||
        rec_is_high_key_structural(
          new_block->page.frame,
          page_rec_get_prev_const(page_get_supremum_rec(new_block->page.frame)),
          index));
  return inserted;
}

static dtuple_t *blink_build_node_ptr(dict_index_t *index, buf_block_t *block,
                                      mem_heap_t *heap)
{
  const rec_t *first= page_rec_get_next_user(
    block->page.frame, page_get_infimum_rec(block->page.frame), index);
  if (page_rec_is_supremum(first))
    return nullptr;
  dtuple_t *node_ptr= dict_index_build_node_ptr(
    index, first, block->page.id().page_no(), heap,
    btr_page_get_level(block->page.frame));
  for (ulint i= 0; i < dtuple_get_n_fields(node_ptr); ++i)
    dfield_dup(dtuple_get_nth_field(node_ptr, i), heap);
  return node_ptr;
}

buf_block_t *blink_root_raise_low(ulint flags, dict_index_t *index,
                                  buf_block_t *root, buf_block_t *old_root,
                                  mtr_t *mtr)
{
  if (root->page.id().page_no() != index->page || root->zip_size() ||
      old_root->zip_size() || old_root == root ||
      page_get_n_recs(old_root->page.frame) ||
      page_has_siblings(root->page.frame) ||
      page_has_incomplete_split(root->page.frame))
    return nullptr;
  ut_ad(mtr->memo_contains_flagged(
    &index->lock, MTR_MEMO_S_LOCK | MTR_MEMO_SX_LOCK | MTR_MEMO_X_LOCK));
  ut_ad(mtr->memo_contains_flagged(root, MTR_MEMO_PAGE_X_FIX));
  ut_ad(mtr->memo_contains_flagged(old_root, MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_DEBUG
  byte leaf_segment[10];
  byte top_segment[10];
  memcpy(leaf_segment,
         root->page.frame + PAGE_HEADER + PAGE_BTR_SEG_LEAF, 10);
  memcpy(top_segment,
         root->page.frame + PAGE_HEADER + PAGE_BTR_SEG_TOP, 10);
#endif
  ut_d(const uint32_t root_page_no= root->page.id().page_no());
  ++blink_root_raises;
  const ulint level= btr_page_get_level(root->page.frame);
  btr_page_create(old_root, nullptr, index, level, mtr);
  if (level == 0 && !index->is_primary() && !index->table->is_temporary()) {
    const trx_id_t max_trx_id= page_get_max_trx_id(root->page.frame);
    if (max_trx_id)
      page_update_max_trx_id(old_root, nullptr, max_trx_id, mtr);
  }
  btr_page_set_prev(old_root, FIL_NULL, mtr);
  btr_page_set_next(old_root, FIL_NULL, mtr);
  dberr_t err= DB_SUCCESS;
  ut_a(page_copy_rec_list_end(old_root, root,
                              page_get_infimum_rec(root->page.frame),
                              index, mtr, &err));
  ut_a(err == DB_SUCCESS);
  if (!(flags & BTR_NO_LOCKING_FLAG) && index->has_locking())
    lock_update_root_raise(*old_root, root->page.id());
  mem_heap_t *heap= mem_heap_create(512);
  dtuple_t *node_ptr= blink_build_node_ptr(index, old_root, heap);
  ut_a(node_ptr);
  dtuple_set_info_bits(node_ptr, dtuple_get_info_bits(node_ptr) |
                       REC_INFO_MIN_REC_FLAG);
  btr_page_empty(root, nullptr, index, level + 1, mtr);
  if (index->is_instant())
    btr_set_instant(root, *index, mtr);
  page_cur_t page_cursor;
  page_cur_position(page_get_infimum_rec(root->page.frame), root, &page_cursor);
  page_cursor.index= index;
  rec_offs *offsets= nullptr;
  ut_a(page_cur_tuple_insert(&page_cursor, node_ptr, &offsets, &heap, 0, mtr));
  ut_ad(root->page.id().page_no() == root_page_no);
  ut_ad(btr_page_get_level(root->page.frame) == level + 1);
  ut_ad(btr_page_get_level(old_root->page.frame) == level);
#ifdef UNIV_DEBUG
  ut_ad(!memcmp(leaf_segment,
                root->page.frame + PAGE_HEADER + PAGE_BTR_SEG_LEAF, 10));
  ut_ad(!memcmp(top_segment,
                root->page.frame + PAGE_HEADER + PAGE_BTR_SEG_TOP, 10));
#endif
  mem_heap_free(heap);
  return old_root;
}

rec_t *blink_root_raise_and_insert(ulint flags, btr_cur_t *cursor,
                                   rec_offs **offsets, mem_heap_t **heap,
                                   dtuple_t *tuple,
                                   ulint n_ext, buf_block_t *old_root,
                                   buf_block_t *sibling, mtr_t *mtr)
{
  dict_index_t *index= cursor->index();
  buf_block_t *root= cursor->block();
  if (root == old_root || root == sibling || old_root == sibling ||
      page_get_n_recs(old_root->page.frame) ||
      page_get_n_recs(sibling->page.frame))
    return nullptr;
  rec_t *preflight_split= nullptr;
  bool preflight_left= false;
  if (!blink_split_choose_and_check_fit(cursor, tuple, n_ext,
                                        &preflight_split, &preflight_left,
                                        heap))
    return nullptr;
  if (!blink_root_raise_low(flags, index, root, old_root, mtr))
    return nullptr;
  cursor->page_cur.block= old_root;
  cursor->page_cur.index= index;
  cursor->up_match= cursor->low_match= 0;
  ut_a(!page_cur_search_with_match(tuple, PAGE_CUR_LE, &cursor->up_match,
                                   &cursor->low_match, &cursor->page_cur,
                                   nullptr));
  rec_t *inserted= blink_split_page_and_insert(
    flags, cursor, offsets, heap, tuple, n_ext, sibling, nullptr, mtr);
  ut_a(inserted);

  dtuple_t *second_ptr= blink_build_node_ptr(index, sibling, *heap);
  ut_a(second_ptr);
  page_cur_t root_cursor;
  page_cur_position(page_get_infimum_rec(root->page.frame), root, &root_cursor);
  root_cursor.index= index;
  rec_offs *root_offsets= nullptr;
  uint16_t up_match= 0;
  uint16_t low_match= 0;
  ut_a(!page_cur_search_with_match(second_ptr, PAGE_CUR_LE,
                                   &up_match, &low_match,
                                   &root_cursor, nullptr));
  ut_a(page_cur_tuple_insert(&root_cursor, second_ptr, &root_offsets,
                             heap, 0, mtr));
  page_clear_incomplete_split(old_root, mtr);
  return inserted;
}

static constexpr ulint BLINK_STASH_SLACK= 4;

struct blink_nonleaf_stash_t
{
  uint32_t pages[BTR_MAX_LEVELS + BLINK_STASH_SLACK];
  ulint size{};

  uint32_t pop() noexcept
  {
    return size ? pages[--size] : FIL_NULL;
  }
};

static void blink_stash_return(dict_index_t *index,
                               blink_nonleaf_stash_t *stash) noexcept
{
  while (stash->size)
    blink_page_pool_push(index, blink_page_kind::INTERNAL,
                         stash->pages[--stash->size]);
}

static bool blink_parent_has_child(const page_t *page, dict_index_t *index,
                                   uint32_t child, mem_heap_t **heap)
{
  for (const rec_t *record= page_rec_get_next_const(page_get_infimum_rec(page));
       !page_rec_is_supremum(record);
       record= page_rec_get_next_const(record)) {
    if (rec_is_high_key(page, record, index))
      break;
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs_init(offsets_);
    rec_offs *offsets= rec_get_offsets(record, index, offsets_, 0,
                                       ULINT_UNDEFINED, heap);
    if (btr_node_ptr_get_child_page_no(record, offsets) == child)
      return true;
  }
  return false;
}

static dberr_t blink_clear_child_split(dict_index_t *index,
                                       uint32_t left_page, mtr_t *mtr)
{
  dberr_t err= DB_SUCCESS;
  buf_block_t *left= btr_block_get(*index, left_page, RW_X_LATCH, mtr, &err);
  if (!left)
    return err;
  if (!page_has_incomplete_split(left->page.frame))
    return DB_SUCCESS;
  const uint32_t right_page= btr_page_get_next(left->page.frame);
  if (right_page == FIL_NULL)
    return DB_CORRUPTION;
  buf_block_t *right= btr_block_get(*index, right_page, RW_S_LATCH, mtr, &err);
  if (!right || btr_page_get_prev(right->page.frame) != left_page ||
      btr_page_get_level(right->page.frame) !=
        btr_page_get_level(left->page.frame))
    return DB_CORRUPTION;
  page_clear_incomplete_split(left, mtr);
  return DB_SUCCESS;
}

static dberr_t blink_insert_into_level(ulint flags, dtuple_t *node_ptr,
                                       uint16_t level, uint32_t previous_child,
                                       mtr_t *holder, dict_index_t *index,
                                       blink_nonleaf_stash_t *stash,
                                       que_thr_t *thr)
{
  ++blink_cascade_levels;
  mtr_t parked{holder->trx};
  ulint attempts= 0;
  for (;;) {
    mtr_t mtr{holder->trx};
    mtr.start();
    holder->transfer_to(&mtr, &index->lock, MTR_MEMO_S_LOCK);
    holder->commit();

    btr_cur_t parent;
    parent.page_cur.index= index;
    dberr_t err= blink_search_to_level(index, level, node_ptr, PAGE_CUR_LE,
                                       RW_X_LATCH, true, &parent, &mtr);
    if (err != DB_SUCCESS) {
      mtr.commit();
      return err;
    }

    buf_block_t *parent_block= parent.block();
    if (page_has_incomplete_split(parent_block->page.frame)) {
      ++blink_incomplete_retries;
      if (!thr) {
        const uint32_t parent_page= parent_block->page.id().page_no();
        mtr.commit();
        blink_pending_split_enqueue(index, parent_page);
        blink_pending_split_enqueue(index, previous_child);
        return DB_BLINK_RETRY;
      }
      if (trx_is_interrupted(thr_get_trx(thr))) {
        mtr.commit();
        blink_pending_split_enqueue(index, previous_child);
        return DB_INTERRUPTED;
      }
      parked.start();
      mtr.transfer_to(&parked, &index->lock, MTR_MEMO_S_LOCK);
      mtr.commit();
      if (attempts < 4)
        MY_RELAX_CPU();
      else if (attempts < 16)
        std::this_thread::yield();
      else
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      ++attempts;
      holder= &parked;
      continue;
    }

    mem_heap_t *parent_heap= mem_heap_create(1024);
    const uint16_t child_field=
      dict_index_get_n_unique_in_tree_nonleaf(index);
    ulint child_len;
    const byte *child_data= static_cast<const byte*>(dfield_get_data(
      dtuple_get_nth_field(node_ptr, child_field)));
    child_len= dfield_get_len(dtuple_get_nth_field(node_ptr, child_field));
    ut_a(child_len == 4);
    const uint32_t child= mach_read_from_4(child_data);

    if (blink_parent_has_child(parent_block->page.frame, index, child,
                               &parent_heap)) {
      err= blink_clear_child_split(index, previous_child, &mtr);
      mem_heap_free(parent_heap);
      mtr.commit();
      return err;
    }

    rec_offs *parent_offsets= nullptr;
    rec_t *installed= page_cur_tuple_insert(
      &parent.page_cur, node_ptr, &parent_offsets, &parent_heap, 0, &mtr);
    if (!installed &&
        btr_page_reorganize(&parent.page_cur, &mtr) == DB_SUCCESS)
      installed= page_cur_tuple_insert(
        &parent.page_cur, node_ptr, &parent_offsets, &parent_heap, 0, &mtr);
    if (installed) {
      err= blink_clear_child_split(index, previous_child, &mtr);
      if (err == DB_SUCCESS)
        ++blink_parent_installs;
      mem_heap_free(parent_heap);
      mtr.commit();
      return err;
    }

    uint32_t new_page_no= stash->pop();
    if (new_page_no == FIL_NULL &&
        !blink_page_pool_try_pop(index, blink_page_kind::INTERNAL,
                                 &new_page_no))
      ut_error;
    buf_block_t *new_page= btr_block_get(
      *index, new_page_no, RW_X_LATCH, &mtr, &err);
    ut_a(new_page);
    const bool root= parent_block->page.id().page_no() == index->page;
    uint32_t root_sibling_no= FIL_NULL;
    buf_block_t *root_sibling= nullptr;
    buf_block_t *old_right= nullptr;
    if (root) {
      root_sibling_no= stash->pop();
      if (root_sibling_no == FIL_NULL &&
          !blink_page_pool_try_pop(index, blink_page_kind::INTERNAL,
                                   &root_sibling_no))
        ut_error;
      root_sibling= btr_block_get(
        *index, root_sibling_no, RW_X_LATCH, &mtr, &err);
      ut_a(root_sibling);
    } else {
      const uint32_t right_no= btr_page_get_next(parent_block->page.frame);
      if (right_no != FIL_NULL) {
        old_right= btr_block_get(*index, right_no, RW_X_LATCH, &mtr, &err);
        ut_a(old_right);
      }
    }

    rec_t *placed= root
      ? blink_root_raise_and_insert(flags, &parent, &parent_offsets,
                                    &parent_heap, node_ptr, 0, new_page,
                                    root_sibling, &mtr)
      : blink_split_page_and_insert(flags, &parent, &parent_offsets,
                                    &parent_heap, node_ptr, 0, new_page,
                                    old_right, &mtr);
    ut_a(placed);
    err= blink_clear_child_split(index, previous_child, &mtr);
    ut_a(err == DB_SUCCESS);
    ++blink_parent_installs;
    if (root) {
      mem_heap_free(parent_heap);
      mtr.commit();
      return DB_SUCCESS;
    }

    mem_heap_t *cascade_heap= mem_heap_create(1024);
    dtuple_t *next_node_ptr= blink_build_node_ptr(index, new_page, cascade_heap);
    ut_a(next_node_ptr);
    const uint32_t next_previous= parent_block->page.id().page_no();
    mtr_t next_holder{holder->trx};
    next_holder.start();
    mtr.transfer_to(&next_holder, &index->lock, MTR_MEMO_S_LOCK);
    mtr.commit();
    mem_heap_free(parent_heap);
    err= blink_insert_into_level(flags, next_node_ptr, level + 1,
                                 next_previous, &next_holder, index, stash,
                                 thr);
    mem_heap_free(cascade_heap);
    return err;
  }
}

static void blink_return_preallocated(dict_index_t *index, uint32_t leaf,
                                      uint32_t root_sibling,
                                      blink_nonleaf_stash_t *stash) noexcept
{
  if (leaf != FIL_NULL)
    blink_page_pool_push(index, blink_page_kind::LEAF, leaf);
  if (root_sibling != FIL_NULL)
    blink_page_pool_push(index, blink_page_kind::LEAF, root_sibling);
  blink_stash_return(index, stash);
}

dberr_t blink_pessimistic_insert(ulint flags, btr_cur_t *cursor,
                                  rec_offs **offsets, mem_heap_t **heap,
                                  dtuple_t *entry, rec_t **insert_rec,
                                  big_rec_t **big_rec, ulint n_ext,
                                  que_thr_t *thr, trx_id_t trx_id, mtr_t *mtr)
{
  dict_index_t *index= cursor->index();
  buf_block_t *left= cursor->block();
  *insert_rec= nullptr;
  *big_rec= nullptr;
  if (page_has_incomplete_split(left->page.frame)) {
    ++blink_incomplete_retries;
    return DB_BLINK_RETRY;
  }

  uint32_t new_page_no= FIL_NULL;
  if (!blink_page_pool_try_pop(index, blink_page_kind::LEAF, &new_page_no)) {
    ++blink_pool_empty_retries;
    return DB_BLINK_RETRY_POOL_EMPTY;
  }
  const bool root= left->page.id().page_no() == index->page;
  uint32_t root_sibling_no= FIL_NULL;
  blink_nonleaf_stash_t stash;
  if (root) {
    if (!blink_page_pool_try_pop(index, blink_page_kind::LEAF,
                                 &root_sibling_no)) {
      blink_return_preallocated(index, new_page_no, FIL_NULL, &stash);
      ++blink_pool_empty_retries;
      return DB_BLINK_RETRY_POOL_EMPTY;
    }
  } else {
    const ulint budget= cursor->tree_height + BLINK_STASH_SLACK;
    ut_a(budget <= UT_ARR_SIZE(stash.pages));
    for (ulint i= 0; i < budget; ++i) {
      uint32_t page;
      if (!blink_page_pool_try_pop(index, blink_page_kind::INTERNAL, &page)) {
        blink_return_preallocated(index, new_page_no, FIL_NULL, &stash);
        ++blink_pool_empty_retries;
        return DB_BLINK_RETRY_POOL_EMPTY;
      }
      stash.pages[stash.size++]= page;
    }
  }

  bool inherit= false;
  dberr_t err= btr_cur_ins_lock_and_undo(
    flags, cursor, entry, thr, mtr, &inherit);
  if (err != DB_SUCCESS) {
    blink_return_preallocated(index, new_page_no, root_sibling_no, &stash);
    return err;
  }

  big_rec_t *big_rec_vec= nullptr;
  if (page_zip_rec_needs_ext(rec_get_converted_size(index, entry, n_ext),
                             index->table->not_redundant(),
                             dtuple_get_n_fields(entry), left->zip_size())) {
    big_rec_vec= dtuple_convert_big_rec(index, 0, entry, &n_ext);
    if (!big_rec_vec) {
      blink_return_preallocated(index, new_page_no, root_sibling_no, &stash);
      return DB_TOO_BIG_RECORD;
    }
  }

  buf_block_t *new_page= btr_block_get(
    *index, new_page_no, RW_X_LATCH, mtr, &err);
  ut_a(new_page);
  buf_block_t *root_sibling= nullptr;
  buf_block_t *old_right= nullptr;
  if (root) {
    root_sibling= btr_block_get(
      *index, root_sibling_no, RW_X_LATCH, mtr, &err);
    ut_a(root_sibling);
  } else {
    const uint32_t old_right_no= btr_page_get_next(left->page.frame);
    if (old_right_no != FIL_NULL) {
      old_right= btr_block_get(
        *index, old_right_no, RW_X_LATCH, mtr, &err);
      ut_a(old_right);
    }
  }

  rec_t *inserted= root
    ? blink_root_raise_and_insert(flags, cursor, offsets, heap, entry, n_ext,
                                  new_page, root_sibling, mtr)
    : blink_split_page_and_insert(flags, cursor, offsets, heap, entry, n_ext,
                                  new_page, old_right, mtr);
  if (!inserted) {
    blink_return_preallocated(index, new_page_no, root_sibling_no, &stash);
    if (big_rec_vec)
      dtuple_convert_back_big_rec(index, entry, big_rec_vec);
    return DB_OUT_OF_FILE_SPACE;
  }
  *insert_rec= inserted;
  if (inherit && !(flags & BTR_NO_LOCKING_FLAG))
    lock_update_insert(cursor->block(), inserted, index);
  if (trx_id)
    page_update_max_trx_id(cursor->block(),
                           buf_block_get_page_zip(cursor->block()),
                           trx_id, mtr);
  if (root) {
    *big_rec= big_rec_vec;
    return DB_SUCCESS;
  }

  mem_heap_t *cascade_heap= mem_heap_create(1024);
  dtuple_t *node_ptr= blink_build_node_ptr(index, new_page, cascade_heap);
  ut_a(node_ptr);
  const uint32_t previous_child= left->page.id().page_no();
  mtr_t holder{mtr->trx};
  holder.start();
  mtr->transfer_to(&holder, &index->lock, MTR_MEMO_S_LOCK);
  mtr->commit();
  err= blink_insert_into_level(flags, node_ptr, 1, previous_child,
                               &holder, index, &stash, thr);
  mem_heap_free(cascade_heap);
  blink_stash_return(index, &stash);
  mtr->start();
  if (err != DB_SUCCESS) {
    if (big_rec_vec)
      dtuple_convert_back_big_rec(index, entry, big_rec_vec);
    return err;
  }
  *big_rec= big_rec_vec;
  return DB_SUCCESS;
}

struct blink_pending_split_t
{
  dict_index_t *index;
  uint32_t left_page;
  uint16_t attempts;
};

static std::mutex blink_pending_mutex;
static std::deque<blink_pending_split_t> blink_pending_splits;

void blink_pending_split_enqueue(dict_index_t *index,
                                 uint32_t left_page) noexcept
{
  std::lock_guard<std::mutex> guard(blink_pending_mutex);
  for (const blink_pending_split_t &task : blink_pending_splits)
    if (task.index == index && task.left_page == left_page)
      return;
  blink_pending_splits.push_back({index, left_page, 0});
}

void blink_pending_splits_remove(dict_index_t *index) noexcept
{
  std::lock_guard<std::mutex> guard(blink_pending_mutex);
  for (auto it= blink_pending_splits.begin(); it != blink_pending_splits.end(); )
    if (it->index == index)
      it= blink_pending_splits.erase(it);
    else
      ++it;
}

static void blink_pending_split_requeue(dict_index_t *index,
                                        uint32_t left_page,
                                        uint16_t attempts) noexcept
{
  if (attempts >= 100)
    return;
  std::lock_guard<std::mutex> guard(blink_pending_mutex);
  blink_pool_entry_t *pin= blink_page_pool_pin(index);
  if (!pin)
    return;
  bool found= false;
  for (const blink_pending_split_t &task : blink_pending_splits)
    if (task.index == index && task.left_page == left_page) {
      found= true;
      break;
    }
  if (!found)
    blink_pending_splits.push_back({index, left_page, attempts});
  blink_page_pool_unpin(pin);
}

dberr_t blink_finish_incomplete_split(dict_index_t *index,
                                       uint32_t left_page, trx_t *trx)
{
  if (!use_blink_path(index))
    return DB_UNSUPPORTED;
  blink_nonleaf_stash_t stash;
  mtr_t mtr{trx};
  mtr.start();
  if (!index->lock.x_lock_try()) {
    mtr.commit();
    return DB_BLINK_RETRY;
  }
  mtr.memo_push(&index->lock, MTR_MEMO_X_LOCK);
  if (index->page == FIL_NULL || !index->table->space) {
    mtr.commit();
    return DB_TABLESPACE_DELETED;
  }
  dberr_t err= DB_SUCCESS;
  buf_block_t *root= btr_root_block_get(index, RW_S_LATCH, &mtr, &err);
  if (!root) {
    mtr.commit();
    return err;
  }
  const ulint budget= btr_page_get_level(root->page.frame) +
    BLINK_STASH_SLACK;
  mtr.release(*root);
  for (ulint i= 0; i < budget; ++i) {
    uint32_t page;
    if (!blink_page_pool_try_pop(index, blink_page_kind::INTERNAL, &page)) {
      blink_stash_return(index, &stash);
      mtr.commit();
      ++blink_pool_empty_retries;
      return DB_BLINK_RETRY_POOL_EMPTY;
    }
    stash.pages[stash.size++]= page;
  }

  buf_block_t *left= btr_block_get(
    *index, left_page, RW_S_LATCH, &mtr, &err);
  if (!left) {
    blink_stash_return(index, &stash);
    mtr.commit();
    return err;
  }
  if (!page_has_incomplete_split(left->page.frame)) {
    blink_stash_return(index, &stash);
    mtr.commit();
    return DB_SUCCESS;
  }
  const uint32_t right_page= btr_page_get_next(left->page.frame);
  if (right_page == FIL_NULL) {
    blink_stash_return(index, &stash);
    mtr.commit();
    return DB_CORRUPTION;
  }
  buf_block_t *right= btr_block_get(
    *index, right_page, RW_S_LATCH, &mtr, &err);
  if (!right || btr_page_get_prev(right->page.frame) != left_page ||
      btr_page_get_level(right->page.frame) !=
        btr_page_get_level(left->page.frame)) {
    blink_stash_return(index, &stash);
    mtr.commit();
    return DB_CORRUPTION;
  }

  mem_heap_t *heap= mem_heap_create(1024);
  dtuple_t *node_ptr= blink_build_node_ptr(index, right, heap);
  if (!node_ptr) {
    const rec_t *high_key= page_rec_get_prev_const(
      page_get_supremum_rec(left->page.frame));
    if (!rec_is_high_key_structural(left->page.frame, high_key, index)) {
      mem_heap_free(heap);
      blink_stash_return(index, &stash);
      mtr.commit();
      return DB_CORRUPTION;
    }
    node_ptr= dict_index_build_node_ptr(
      index, high_key, right_page, heap,
      btr_page_get_level(left->page.frame));
    for (ulint i= 0; i < dtuple_get_n_fields(node_ptr); ++i)
      dfield_dup(dtuple_get_nth_field(node_ptr, i), heap);
  }
  const uint16_t parent_level= static_cast<uint16_t>(
    btr_page_get_level(left->page.frame) + 1);
  mtr.commit();
  mtr_t holder{trx};
  holder.start();
  mtr_s_lock_index(index, &holder);
  err= blink_insert_into_level(BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG,
                               node_ptr, parent_level, left_page, &holder,
                               index, &stash, nullptr);
  mem_heap_free(heap);
  blink_stash_return(index, &stash);
  return err;
}

static dberr_t blink_scan_incomplete_splits(dict_index_t *index)
{
  if (!index->is_committed())
    return DB_BLINK_RETRY;
  std::vector<uint32_t> candidates;
  mtr_t mtr{nullptr};
  mtr.start();
  if (!index->lock.x_lock_try()) {
    mtr.commit();
    return DB_BLINK_RETRY;
  }
  mtr.memo_push(&index->lock, MTR_MEMO_X_LOCK);
  if (index->page == FIL_NULL || !index->table->space) {
    mtr.commit();
    return DB_TABLESPACE_DELETED;
  }
  dberr_t err= DB_SUCCESS;
  buf_block_t *root= btr_root_block_get(index, RW_S_LATCH, &mtr, &err);
  if (!root) {
    mtr.commit();
    return err;
  }
  const uint16_t root_level= static_cast<uint16_t>(
    btr_page_get_level(root->page.frame));
  mtr.release(*root);

  for (uint16_t target= 0; target <= root_level; ++target) {
    uint32_t page_no= index->page;
    uint16_t level= root_level;
    buf_block_t *block= nullptr;
    while (level > target) {
      block= btr_block_get(*index, page_no, RW_S_LATCH, &mtr, &err);
      if (!block) {
        mtr.commit();
        return err;
      }
      const rec_t *record= page_rec_get_next_user(
        block->page.frame, page_get_infimum_rec(block->page.frame), index);
      if (page_rec_is_supremum(record)) {
        mtr.commit();
        return DB_CORRUPTION;
      }
      mem_heap_t *heap= nullptr;
      rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
      rec_offs_init(offsets_);
      rec_offs *offsets= rec_get_offsets(record, index, offsets_, 0,
                                         ULINT_UNDEFINED, &heap);
      page_no= btr_node_ptr_get_child_page_no(record, offsets);
      if (heap)
        mem_heap_free(heap);
      mtr.release(*block);
      --level;
    }
    do {
      block= btr_block_get(*index, page_no, RW_S_LATCH, &mtr, &err);
      if (!block) {
        mtr.commit();
        return err;
      }
      if (page_has_incomplete_split(block->page.frame))
        candidates.push_back(page_no);
      page_no= btr_page_get_next(block->page.frame);
      mtr.release(*block);
    } while (page_no != FIL_NULL);
  }
  mtr.commit();
  for (uint32_t page : candidates)
    blink_pending_split_enqueue(index, page);
  return DB_SUCCESS;
}

void blink_pending_splits_process() noexcept
{
  blink_pending_split_t task{};
  blink_pool_entry_t *pin= nullptr;
  {
    std::lock_guard<std::mutex> guard(blink_pending_mutex);
    if (blink_pending_splits.empty())
      return;
    task= blink_pending_splits.front();
    blink_pending_splits.pop_front();
    pin= blink_page_pool_pin(task.index);
  }
  if (!pin)
    return;
  const dberr_t err= task.left_page == FIL_NULL
    ? blink_scan_incomplete_splits(task.index)
    : blink_finish_incomplete_split(task.index, task.left_page, nullptr);
  if (err == DB_BLINK_RETRY || err == DB_BLINK_RETRY_POOL_EMPTY)
    blink_pending_split_requeue(task.index, task.left_page,
                                task.attempts + 1);
  blink_page_pool_unpin(pin);
}
