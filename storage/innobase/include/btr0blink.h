#ifndef btr0blink_h
#define btr0blink_h

#include "btr0types.h"
#include "buf0types.h"
#include "dict0boot.h"
#include "dict0mem.h"
#include "page0types.h"

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

#endif
