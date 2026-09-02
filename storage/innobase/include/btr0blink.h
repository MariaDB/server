#ifndef btr0blink_h
#define btr0blink_h

#include "btr0types.h"
#include "buf0types.h"
#include "dict0boot.h"
#include "dict0mem.h"
#include "page0types.h"

struct big_rec_t;
struct que_thr_t;
struct trx_t;

extern Atomic_counter<uint64_t> blink_searches;
extern Atomic_counter<uint64_t> blink_right_moves;
extern Atomic_counter<uint64_t> blink_optimistic_inserts;
extern Atomic_counter<uint64_t> blink_leaf_splits;
extern Atomic_counter<uint64_t> blink_internal_splits;
extern Atomic_counter<uint64_t> blink_root_raises;
extern Atomic_counter<uint64_t> blink_parent_installs;
extern Atomic_counter<uint64_t> blink_cascade_levels;
extern Atomic_counter<uint64_t> blink_incomplete_retries;
extern Atomic_counter<uint64_t> blink_pool_empty_retries;
extern Atomic_counter<uint64_t> blink_pool_refills;
extern Atomic_counter<uint64_t> blink_normal_x_index;

inline bool blink_table_shape_ok(const dict_table_t *table) noexcept
{
  return table->not_redundant() && !DICT_TF_GET_ZIP_SSIZE(table->flags) &&
    !table->is_temporary() && !dict_is_sys_table(table->id) &&
    !(table->flags2 & DICT_TF2_FTS);
}

inline bool blink_index_shape_ok(const dict_index_t *index) noexcept
{
  return blink_table_shape_ok(index->table) &&
    !(index->type & (DICT_FTS | DICT_SPATIAL | DICT_CORRUPT));
}

inline bool use_blink_path(const dict_index_t *index) noexcept
{
  return (index->type & DICT_BLINK) && blink_index_shape_ok(index);
}

inline bool blink_table_has_index(const dict_table_t *table) noexcept
{
  for (const dict_index_t *index= UT_LIST_GET_FIRST(table->indexes); index;
       index= UT_LIST_GET_NEXT(indexes, index))
    if (index->type & DICT_BLINK)
      return true;
  return false;
}

bool blink_stamp_empty_tree(dict_index_t *index);
dberr_t blink_search_to_level(dict_index_t *index, uint16_t target_level,
                              const dtuple_t *tuple, page_cur_mode_t mode,
                              rw_lock_type_t target_latch, bool index_latched,
                              btr_cur_t *cursor, mtr_t *mtr);
inline dberr_t blink_search_leaf(dict_index_t *index, const dtuple_t *tuple,
                                 page_cur_mode_t mode,
                                 rw_lock_type_t target_latch,
                                 bool index_latched, btr_cur_t *cursor,
                                 mtr_t *mtr)
{
  return blink_search_to_level(index, 0, tuple, mode, target_latch,
                               index_latched, cursor, mtr);
}
bool blink_split_choose_and_check_fit(btr_cur_t *cursor,
                                      const dtuple_t *tuple, ulint n_ext,
                                      rec_t **split_rec, bool *insert_left,
                                      mem_heap_t **heap);
rec_t *blink_split_page_and_insert(ulint flags, btr_cur_t *cursor,
                                   rec_offs **offsets, mem_heap_t **heap,
                                   dtuple_t *tuple, ulint n_ext,
                                   buf_block_t *new_block,
                                   buf_block_t *old_right, mtr_t *mtr);
buf_block_t *blink_root_raise_low(ulint flags, dict_index_t *index,
                                  buf_block_t *root, buf_block_t *old_root,
                                  mtr_t *mtr);
rec_t *blink_root_raise_and_insert(ulint flags, btr_cur_t *cursor,
                                   rec_offs **offsets, mem_heap_t **heap,
                                   dtuple_t *tuple, ulint n_ext,
                                   buf_block_t *old_root,
                                   buf_block_t *sibling, mtr_t *mtr);
dberr_t blink_pessimistic_insert(ulint flags, btr_cur_t *cursor,
                                  rec_offs **offsets, mem_heap_t **heap,
                                  dtuple_t *entry, rec_t **insert_rec,
                                  big_rec_t **big_rec, ulint n_ext,
                                  que_thr_t *thr, trx_id_t trx_id, mtr_t *mtr);
dberr_t blink_finish_incomplete_split(dict_index_t *index,
                                       uint32_t left_page, trx_t *trx);
void blink_pending_split_enqueue(dict_index_t *index,
                                 uint32_t left_page) noexcept;
void blink_pending_splits_process() noexcept;
void blink_pending_splits_remove(dict_index_t *index) noexcept;

#endif
