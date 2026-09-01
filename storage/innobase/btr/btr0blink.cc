#include "btr0blink.h"

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "buf0buf.h"
#include "dict0dict.h"
#include "mem0mem.h"
#include "mtr0mtr.h"
#include "page0blink.h"
#include "page0cur.h"
#include "rem0cmp.h"
#include "rem0rec.h"

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
