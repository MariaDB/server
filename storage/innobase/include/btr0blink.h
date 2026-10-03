/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#ifndef btr0blink_h
#define btr0blink_h

#include "dict0boot.h"
#include "dict0mem.h"
#include "btr0types.h"
#include "db0err.h"
#include "fsp0fsp.h"
#include "page0types.h"
#include "univ.i"

struct big_rec_t;
struct dtuple_t;
struct mem_block_info_t;
typedef mem_block_info_t mem_heap_t;
struct que_thr_t;
struct mtr_t;
struct trx_t;

extern Atomic_counter<uint64_t> blink_searches;
extern Atomic_counter<uint64_t> blink_right_moves;
extern Atomic_counter<uint64_t> blink_optimistic_inserts;
extern Atomic_counter<uint64_t> blink_leaf_splits;
extern Atomic_counter<uint64_t> blink_parent_installs;
extern Atomic_counter<uint64_t> blink_pool_empty;

/** Whether a table shape is supported by the B-link proof of concept. */
inline bool blink_table_shape_ok(const dict_table_t *table) noexcept
{
  return table->not_redundant() && table->space && !table->space->zip_size() &&
    !table->is_temporary() && !table->persistent_autoinc &&
    !dict_is_sys_table(table->id) && !(table->flags2 & DICT_TF2_FTS);
}

/** Whether an index shape is supported by the B-link proof of concept. */
inline bool blink_index_shape_ok(const dict_index_t *index) noexcept
{
  return blink_table_shape_ok(index->table) &&
    !(index->type & (DICT_FTS | DICT_SPATIAL)) && index->is_committed();
}

/** Whether operations on an index must use the B-link proof-of-concept path. */
inline bool use_blink_path(const dict_index_t *index) noexcept
{
  return (index->type & DICT_BLINK) && blink_index_shape_ok(index);
}

/** Descend without page-latch coupling while holding S(index), following
right links whenever @p tuple is at or beyond a page high key. The returned
cursor page remains latched in @p mtr.
@param index clustered, uncompressed B-link index
@param tuple search tuple with n_fields_cmp set
@param mode page cursor search mode
@param leaf_latch RW_S_LATCH or RW_X_LATCH
@param cursor positioned result
@param mtr active mini-transaction
@return DB_SUCCESS or an error code */
dberr_t blink_search_leaf(dict_index_t *index, const dtuple_t *tuple,
                          page_cur_mode_t mode, rw_lock_type_t leaf_latch,
                          btr_cur_t *cursor, mtr_t *mtr);

/** Optimistic insert wrapper for a leaf returned by blink_search_leaf(). */
dberr_t blink_optimistic_insert(ulint flags, btr_cur_t *cursor,
                                rec_offs **offsets, mem_heap_t **heap,
                                dtuple_t *entry, rec_t **rec,
                                big_rec_t **big_rec, ulint n_ext,
                                que_thr_t *thr, mtr_t *mtr);

/** Split a non-root leaf to a preallocated empty right page and insert entry.
The old leaf sibling pointer, high key, and incomplete-split flag are published
in the same mini-transaction. The caller must commit before installing the
parent separator.
@param flags insertion flags
@param cursor X-latched non-root leaf cursor
@param right X-latched, initialized empty leaf page
@param entry clustered record to insert
@param offsets inserted record offsets
@param heap offsets heap
@param rec inserted record
@param big_rec externally stored fields, if any
@param n_ext externally stored field count
@param thr query thread
@param mtr active mini-transaction holding S(index)
@return DB_SUCCESS, DB_FAIL if the shape cannot use this split, or error */
dberr_t blink_split_leaf_and_insert(ulint flags, btr_cur_t *cursor,
                                    buf_block_t *right, dtuple_t *entry,
                                    rec_offs **offsets, mem_heap_t **heap,
                                    rec_t **rec, big_rec_t **big_rec,
                                    ulint n_ext, que_thr_t *thr, mtr_t *mtr);

/** Re-descend under X(index), install the separator for an incomplete leaf
split, recursively using the existing internal split/root-raise machinery,
and clear the incomplete flag.
@param index clustered B-link index
@param left_page left page carrying the incomplete flag
@param right_page new right sibling
@param trx transaction associated with the mini-transaction, or nullptr
@return DB_SUCCESS or error */
dberr_t blink_install_parent(dict_index_t *index, uint32_t left_page,
                             uint32_t right_page, trx_t *trx);

#endif /* btr0blink_h */
