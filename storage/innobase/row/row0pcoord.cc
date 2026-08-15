/*****************************************************************************

Copyright (c) 2018, 2025, Oracle and/or its affiliates.
Copyright (c) 2026, MariaDB

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file row/row0pcoord.cc
Parallel coordinator implementation

Based on MySQL commit dbfc59ffaf80 created 2018-01-27 by Sunny Bains. */

#include <array>

#include "btr0pcur.h"
#include "dict0dict.h"
#include "row0mysql.h"
#include "row0pcoord.h"
#include "row0row.h"
#include "row0vers.h"
#include "ut0new.h"

/** Tree depth at which we decide to split blocks further. */
static constexpr size_t SPLIT_THRESHOLD{3};

[[nodiscard]]
static dberr_t pread_page_cur_search(buf_block_t *block,
                                     dict_index_t *index,
                                     const dtuple_t *tuple,
                                     page_cur_mode_t mode,
                                     page_cur_t *cursor) noexcept
{
  uint16_t up_match = 0, low_match = 0;
  cursor->index = index;
  cursor->block = block;
  return page_cur_search_with_match(tuple, mode, &up_match, &low_match,
                                    cursor, nullptr)
           ? DB_CORRUPTION
           : DB_SUCCESS;
}

Parallel_coordinator::Scan_ctx::Iter::~Iter()
{
  if (m_heap == nullptr) {
    return;
  }

  mem_heap_free(m_heap);
  m_heap = nullptr;
}

void Parallel_coordinator::Scan_ctx::index_s_lock()
{
  m_config.m_index->lock.s_lock(SRW_LOCK_CALL);
  m_s_locks.fetch_add(1, std::memory_order_release);
}

void Parallel_coordinator::Scan_ctx::index_s_unlock()
{
  m_s_locks.fetch_sub(1, std::memory_order_release);
  m_config.m_index->lock.s_unlock();
}

dberr_t Parallel_coordinator::Exec_ctx::split()
{
  ut_ad(m_range.first->m_tuple == nullptr ||
        dtuple_validate(m_range.first->m_tuple));
  ut_ad(m_range.second->m_tuple == nullptr ||
        dtuple_validate(m_range.second->m_tuple));

  /* Setup the sub-range. Carry the inclusivity of this chunk's end so that
  create_ranges() keeps the sub-tree holding the boundary key. */
  Scan_range scan_range(m_range.first->m_tuple, m_range.second->m_tuple,
                        m_end_inclusive);

  /* S lock so that the tree structure doesn't change while we are
  figuring out the sub-trees to scan. */
  m_scan_ctx->index_s_lock();

  Parallel_coordinator::Scan_ctx::Ranges ranges{};
  m_scan_ctx->partition(scan_range, ranges, 1);

  if (!ranges.empty())
    ranges.back().second = m_range.second;

  dberr_t err{DB_SUCCESS};

  /* Create the partitioned scan execution contexts. Only the last sub-chunk
  ends where this chunk ended, so only it inherits the inclusivity. */
  size_t i{};

  for (auto &range : ranges)
  {
    const bool last = (++i == ranges.size());

    err = m_scan_ctx->create_context(range, false,
                                     last && m_end_inclusive);

    if (err != DB_SUCCESS) {
      break;
    }
  }

  if (err != DB_SUCCESS)
  {
    m_scan_ctx->set_error_state(err);
  }

  m_scan_ctx->index_s_unlock();

  return err;
}

Parallel_coordinator::Scan_ctx::Scan_ctx(Parallel_coordinator *coordinator, size_t id,
                                    trx_t *trx,
                                    const Parallel_coordinator::Config &config)
    : m_id(id), m_config(config), m_trx(trx), m_coordinator(coordinator) {}

buf_block_t *Parallel_coordinator::Scan_ctx::block_get_s_latched(
    const page_id_t &page_id, mtr_t *mtr, size_t line) const
{
  /* We never scan undo tablespaces. */
  ut_a(!srv_is_undo_tablespace(page_id.space()));

  auto block =
      buf_page_get_gen(page_id, m_config.m_zip_size, RW_S_LATCH, nullptr,
                       BUF_GET, mtr);

  return (block);
}


void Parallel_coordinator::Scan_ctx::copy_row(const rec_t *rec, Iter *iter) const
{
  const ulint n_core = page_rec_is_leaf(rec) ? m_config.m_index->n_core_fields : 0;
  iter->m_offsets = rec_get_offsets(rec, m_config.m_index, nullptr,
                                    n_core, ULINT_UNDEFINED, &iter->m_heap);

  // Copy the raw record bytes into the iter's heap.
  ulint rec_len = rec_offs_size(iter->m_offsets);
  rec_t *copy_rec = static_cast<rec_t *>(mem_heap_alloc(iter->m_heap, rec_len));
  memcpy(copy_rec, rec, rec_len);
  iter->m_rec = copy_rec;

  // Build a key-only dtuple (just the unique-in-tree fields).
  const ulint n_unique = dict_index_get_n_unique_in_tree(m_config.m_index);

  dtuple_t *tuple = dtuple_create(iter->m_heap, n_unique);
  dict_index_copy_types(tuple, m_config.m_index, n_unique);

  for (ulint i = 0; i < n_unique; ++i) {
    ulint len;
    const byte *data = rec_get_nth_field(iter->m_rec, iter->m_offsets, i, &len);
    dfield_t *dfield = dtuple_get_nth_field(tuple, i);
    dfield_set_data(dfield, data, len);
  }
  dtuple_set_n_fields_cmp(tuple, n_unique);

  iter->m_tuple = tuple;
}

std::shared_ptr<Parallel_coordinator::Scan_ctx::Iter>
Parallel_coordinator::Scan_ctx::create_iter(
    const page_cur_t &page_cursor) const
{
  ut_a(m_config.m_read_level == 0);
  ut_ad(index_s_own());

  std::shared_ptr<Iter> iter = std::make_shared<Iter>();

  iter->m_heap = mem_heap_create(srv_page_size / 16);

  auto rec = page_cursor.rec;

  const bool is_infimum = page_rec_is_infimum(rec);

  if (is_infimum) {
    rec = page_rec_get_next(rec);
  }

  if (page_rec_is_supremum(rec)) {
    /* Empty page, only root page can be empty. */
    ut_a(!is_infimum ||
         page_cursor.block->page.id().page_no() == m_config.m_index->page);
    return (iter);
  }

  /* Make a copy of the rec. The tuple built from it points into that copy,
  so the boundary stays valid once create_ranges() releases the block
  latches. */
  copy_row(rec, iter.get());

  return (iter);
}

void Parallel_coordinator::enqueue(std::shared_ptr<Exec_ctx> ctx)
{
  mysql_mutex_lock(&m_mutex);
  m_ctxs.push_back(ctx);
  mysql_mutex_unlock(&m_mutex);
}

std::shared_ptr<Parallel_coordinator::Exec_ctx> Parallel_coordinator::dequeue()
{
  mysql_mutex_lock(&m_mutex);

  if (m_ctxs.empty()) {
    mysql_mutex_unlock(&m_mutex);
    return (nullptr);
  }

  auto ctx = m_ctxs.front();
  m_ctxs.pop_front();

  mysql_mutex_unlock(&m_mutex);

  return (ctx);
}

std::shared_ptr<Parallel_coordinator::Exec_ctx>
Parallel_coordinator::get_job_for_worker(Worker_ctx *worker_ctx)
{
  /* Pull the next scannable context. A context flagged m_split is not
  itself scanned: split() re-partitions its sub-tree one level deeper and
  enqueues the finer sub-contexts (for this worker and others), then we
  pull again. This is the pull-based equivalent of the m_split handling in
  Parallel_coordinator::worker() (which we don't use here) — it is what turns
  the few coarse root-subtree ranges into one-leaf-page chunks on deep
  trees. Without it, a deep B-tree with a narrow root yields only a handful
  of huge chunks and almost no worker parallelism. */
  for (;;)
  {
    std::shared_ptr<Parallel_coordinator::Exec_ctx> ctx = dequeue();

    if (ctx == nullptr)
      return nullptr;

    if (ctx->m_scan_ctx->is_error_set())
      return nullptr;

    if (ctx->m_split)
    {
      dberr_t err = ctx->split();
      if (err != DB_SUCCESS)
      {
        ctx->m_scan_ctx->set_error_state(err);
        return nullptr;
      }
      /* The sub-contexts are now queued; pull one for this worker. */
      continue;
    }

    return ctx;
  }
}

page_no_t Parallel_coordinator::Scan_ctx::search(buf_block_t *block,
                                                 const dtuple_t *key,
                                                 dberr_t *err) const
{
  ut_ad(index_s_own());
  *err = DB_SUCCESS;

  page_cur_t page_cursor;
  const auto index = m_config.m_index;

  if (key != nullptr)
  {
    auto ps_err = pread_page_cur_search(block, index, key,
                                        PAGE_CUR_LE, &page_cursor);
    if (ps_err != DB_SUCCESS)
    {
      *err = ps_err;
      return page_no_t{FIL_NULL};
    }
  }
  else
  {
    page_cur_set_before_first(block, &page_cursor);
  }

  if (page_rec_is_infimum(page_cur_get_rec(&page_cursor)))
  {
    rec_t *next = page_cur_move_to_next(&page_cursor);
    if (!next)
    {
      *err = DB_CORRUPTION;
      return page_no_t{FIL_NULL};
    }
  }

  const auto rec = page_cur_get_rec(&page_cursor);

  mem_heap_t *heap = nullptr;

  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  auto offsets = offsets_;

  rec_offs_init(offsets_);

  const ulint n_core = page_rec_is_leaf(rec) ? index->n_core_fields : 0;
  offsets = rec_get_offsets(rec, index, offsets, n_core,
                            ULINT_UNDEFINED, &heap);

  auto page_no = btr_node_ptr_get_child_page_no(rec, offsets);

  if (heap != nullptr)
  {
    mem_heap_free(heap);
  }

  return (page_no);
}

page_cur_t Parallel_coordinator::Scan_ctx::start_range(
    page_no_t page_no, mtr_t *mtr, const dtuple_t *key,
    Savepoints &savepoints, dberr_t *err) const {
  ut_ad(index_s_own());
  *err = DB_SUCCESS;

  auto index = m_config.m_index;
  page_id_t page_id(index->table->space_id, page_no);
  ulint height{};

  /* Follow the left most pointer down on each page. */
  for (;;) {
    auto savepoint = mtr->get_savepoint();

    auto block = block_get_s_latched(page_id, mtr, __LINE__);

    if (!block) {
      /* Page fetch failed — typically tablespace corruption. */
      *err = DB_CORRUPTION;
      return page_cur_t{};
    }

    height = btr_page_get_level(buf_block_get_frame(block));

    savepoints.push_back({savepoint, block});

    if (height != 0 && height != m_config.m_read_level)
    {
      page_id.set_page_no(search(block, key, err));
      if (*err != DB_SUCCESS) {
        return page_cur_t{};
      }
      continue;
    }

    page_cur_t page_cursor;

    if (key != nullptr) {
      auto ps_err = pread_page_cur_search(block, index, key, PAGE_CUR_GE, &page_cursor);
      if (ps_err != DB_SUCCESS)
      {
        *err = ps_err;
        return page_cur_t{};
      }
    }
    else
    {
      page_cur_set_before_first(block, &page_cursor);
    }

    if (page_rec_is_infimum(page_cur_get_rec(&page_cursor)))
    {
      rec_t *next = page_cur_move_to_next(&page_cursor);
      if (!next)
      {
        *err = DB_CORRUPTION;
        return page_cur_t{};
      }
    }

    return (page_cursor);
  }
}

void Parallel_coordinator::Scan_ctx::create_range(
  Ranges &ranges, page_cur_t &leaf_page_cursor) const {
  leaf_page_cursor.index = m_config.m_index;

  auto iter = create_iter(leaf_page_cursor);

  /* Setup the previous range (next) to point to the current range. */
  if (!ranges.empty()) {
    ut_a(ranges.back().second->m_heap == nullptr);
    ranges.back().second = iter;
  }

  ranges.push_back(Range(iter, std::make_shared<Iter>()));
}

dberr_t
Parallel_coordinator::Scan_ctx::create_ranges(const Scan_range &scan_range,
                                              page_no_t page_no,
                                              size_t depth,
                                              const size_t split_level,
                                              Ranges &ranges, mtr_t *mtr) {
  ut_ad(index_s_own());
  ut_a(page_no != FIL_NULL);

  /* Do a breadth first traversal of the B+Tree using recursion. We want to
  set up the scan ranges in one pass. This guarantees that the tree structure
  cannot change while we are creating the scan sub-ranges.

  Once we create the persistent cursor (Range) for a sub-tree we can release
  the latches on all blocks traversed for that sub-tree. */

  const auto index = m_config.m_index;

  page_id_t page_id(index->table->space_id, page_no);

  Savepoint savepoint({mtr->get_savepoint(), nullptr});

  auto block = block_get_s_latched(page_id, mtr, __LINE__);

  if (!block) {
    /* Page fetch failed — typically tablespace corruption. The I/O
    subsystem has already logged a [ERROR]; surface DB_CORRUPTION so the
    SQL layer renders ER_GET_ERRNO (or equivalent) instead of crashing. */
    return DB_CORRUPTION;
  }

  /* read_level requested should be less than the tree height. */
  ut_ad(m_config.m_read_level <
        btr_page_get_level(buf_block_get_frame(block)) + 1);

  savepoint.second = block;

  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  auto offsets = offsets_;

  rec_offs_init(offsets_);

  page_cur_t page_cursor;

  page_cursor.index = index;

  auto start = scan_range.m_start;

  if (start != nullptr)
  {
    /* A node pointer key is the *lowest* key of its child sub-tree, so the
    child that may hold `start` is the last node pointer <= start, not the
    first one >= start. PAGE_CUR_GE would descend into the child after it
    and lose every row from `start` up to that child's first key. The
    leftmost node pointer additionally carries REC_INFO_MIN_REC_FLAG and
    compares less than any tuple (see cmp_dtuple_rec_with_match_low()), so
    GE always skips the leftmost sub-tree. Use the same mode Scan_ctx::
    search() uses to descend, and keep PAGE_CUR_GE once we are on a leaf,
    where we do want the first record >= start.

    Only Exec_ctx::split() reaches this with a non-NULL start - it
    re-partitions an existing chunk beginning at that chunk's first key -
    which is why this only shows up on trees deep enough to be re-split. */
    const page_cur_mode_t mode= page_is_leaf(buf_block_get_frame(block))
      ? PAGE_CUR_GE : PAGE_CUR_LE;

    auto err = pread_page_cur_search(block, index, start,
                                     mode, &page_cursor);
    if (err != DB_SUCCESS)
      return err;

    if (page_cur_is_after_last(&page_cursor))
    {
      return (DB_SUCCESS);
    }
    else if (page_cur_is_before_first((&page_cursor)))
    {
      rec_t *next = page_cur_move_to_next(&page_cursor);
      if (!next)
        return DB_CORRUPTION;
    }
  }
  else
  {
    page_cur_set_before_first(block, &page_cursor);
    /* Skip the infimum record. */

    rec_t *next = page_cur_move_to_next(&page_cursor);
    if (!next)
      return DB_CORRUPTION;
  }

  mem_heap_t *heap{};

  const bool at_leaf = page_is_leaf(buf_block_get_frame(block));
  const uint16_t at_level = btr_page_get_level(buf_block_get_frame(block));

  Savepoints savepoints{};

  while (!page_cur_is_after_last(&page_cursor))
  {
    const rec_t *rec = page_cur_get_rec(&page_cursor);

    /* rec_get_node_ptr_flag() reads the new-style status byte and asserts
    bits <= REC_STATUS_INSTANT — only valid for COMPACT/DYNAMIC tables.
    Check the format first so we never invoke it on REDUNDANT records. */
    ut_a(at_leaf || !dict_table_is_comp(index->table) ||
         rec_get_node_ptr_flag(rec));

    if (heap == nullptr) {
      heap = mem_heap_create(srv_page_size / 4);
    }

    const ulint n_core = page_rec_is_leaf(rec) ? index->n_core_fields : 0;
    offsets = rec_get_offsets(rec, index, offsets, n_core,
                            ULINT_UNDEFINED, &heap);

    const auto end = scan_range.m_end;

    if (end != nullptr)
    {
      const int cmp= cmp_dtuple_rec(end, rec, index, offsets);
      /* rec is the lowest key of the sub-tree it points at, so cmp == 0 means
      that sub-tree starts exactly at the bound. An inclusive bound needs it -
      that is where the boundary rows live - so stop one record later. */
      if (scan_range.m_end_inclusive ? cmp < 0 : cmp <= 0)
        break;
    }

    page_cur_t level_page_cursor;

    /* Split the tree one level below the root if read_level requested is below
    the root level. */
    if (at_level > m_config.m_read_level)
    {
      auto page_no = btr_node_ptr_get_child_page_no(rec, offsets);

      if (depth < split_level)
      {
        /* Need to create a range starting at a lower level in the tree. */
        create_ranges(scan_range, page_no, depth + 1, split_level, ranges, mtr);

        rec_t *next = page_cur_move_to_next(&page_cursor);
        if (!next)
          return DB_CORRUPTION;

        continue;
      }

      /* Find the range start in the leaf node. */
      dberr_t sr_err = DB_SUCCESS;
      level_page_cursor = start_range(page_no, mtr, start, savepoints, &sr_err);
      if (sr_err != DB_SUCCESS) {
        return sr_err;
      }
    }
    else
    {
      /* In case of root node being the leaf node or in case we've been asked to
      read the root node (via read_level) place the cursor on the root node and
      proceed. */

      if (start != nullptr)
      {
        dberr_t err = pread_page_cur_search(block, index, start, PAGE_CUR_GE, &page_cursor);
        if (err != DB_SUCCESS)
          return err;
        ut_a(!page_rec_is_infimum(page_cur_get_rec(&page_cursor)));
      }
      else
      {
        page_cur_set_before_first(block, &page_cursor);

        /* Skip the infimum record. */
        rec_t *next = page_cur_move_to_next(&page_cursor);
        if (!next)
          return DB_CORRUPTION;
        ut_a(!page_cur_is_after_last(&page_cursor));
      }

      /* Since we are already at the requested level use the current page
       cursor. */
      memcpy(&level_page_cursor, &page_cursor, sizeof(level_page_cursor));
    }

    if (!page_rec_is_supremum(page_cur_get_rec(&level_page_cursor)))
    {
      create_range(ranges, level_page_cursor);
    }

    /* We've created the persistent cursor, safe to release S latches on
    the blocks that are in this range (sub-tree). */
    for (auto &savepoint : savepoints)
      mtr->release(*savepoint.second);

    if (m_depth == 0 && depth == 0)
      m_depth = savepoints.size();

    savepoints.clear();

    if (at_level == m_config.m_read_level)
      break;

    start = nullptr;

    rec_t *next = page_cur_move_to_next(&page_cursor);
    if (!next)
      return DB_CORRUPTION;
  }

  savepoints.push_back(savepoint);

  for (auto &savepoint : savepoints)
    mtr->release(*savepoint.second);

  if (heap != nullptr) {
    mem_heap_free(heap);
  }

  return (DB_SUCCESS);
}

dberr_t Parallel_coordinator::Scan_ctx::partition(
    const Scan_range &scan_range,
    Parallel_coordinator::Scan_ctx::Ranges &ranges,
    size_t split_level)
{
  ut_ad(index_s_own());

  mtr_t mtr(m_trx);
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  dberr_t err{DB_SUCCESS};

  err = create_ranges(scan_range, m_config.m_index->page, 0, split_level,
                      ranges, &mtr);

  /* Stamp the scan's own upper bound onto the last chunk. Whether the bound
  itself belongs to the scan travels separately, on the Exec_ctx built from
  this range, and reaches the clamp via set_pscan_end_tuple(). */
  if (err == DB_SUCCESS && scan_range.m_end != nullptr && !ranges.empty()) {
    auto &iter = ranges.back().second;

    ut_a(iter->m_heap == nullptr);

    iter->m_heap = mem_heap_create(srv_page_size / 16);

    iter->m_tuple = dtuple_copy(scan_range.m_end, iter->m_heap);

    /* Do a deep copy. */
    for (size_t i = 0; i < dtuple_get_n_fields(iter->m_tuple); ++i) {
      dfield_dup(&iter->m_tuple->fields[i], iter->m_heap);
    }
  }

  mtr.commit();
  return (err);
}

dberr_t Parallel_coordinator::Scan_ctx::create_context(const Range &range,
                                                       bool split,
                                                       bool end_inclusive)
{
  auto ctx_id =
    m_coordinator->m_ctx_id.fetch_add(1, std::memory_order_relaxed);

  auto ctx = std::shared_ptr<Parallel_coordinator::Exec_ctx>(
    UT_NEW_NOKEY(Parallel_coordinator::Exec_ctx(ctx_id, this, range)),
    [](Parallel_coordinator::Exec_ctx *ctx) { UT_DELETE(ctx); });

  dberr_t err{DB_SUCCESS};

  if (ctx.get() == nullptr)
  {
    m_coordinator->m_ctx_id.fetch_sub(1, std::memory_order_relaxed);
    return (DB_OUT_OF_MEMORY);
  }
  else
  {
    ctx->m_split = split;
    ctx->m_end_inclusive = end_inclusive;
    m_coordinator->enqueue(ctx);
  }

  return (err);
}

dberr_t Parallel_coordinator::Scan_ctx::create_contexts(const Ranges &ranges)
{
  size_t split_point{};

  {
    const auto n = std::max(num_workers(), size_t{1});

    if (ranges.size() > n) {
      split_point = (ranges.size() / n) * n;
    } else if (m_depth < SPLIT_THRESHOLD) {
      /* If the tree is not very deep then don't split. For smaller tables
      it is more expensive to split because we end up traversing more blocks*/
      split_point = n;
    }
  }

  size_t i{};

  for (auto range : ranges) {
    /* Only the last chunk ends at the caller's upper bound, so only it can
    be inclusive. */
    const bool last = (i + 1 == ranges.size());

    auto err = create_context(range, i >= split_point,
                              last && m_config.m_scan_range.m_end_inclusive);

    if (err != DB_SUCCESS) {
      return (err);
    }

    ++i;
  }

  return DB_SUCCESS;
}

dberr_t
Parallel_coordinator::add_scan(trx_t *trx,
                               const Parallel_coordinator::Config &config)
{
  auto scan_ctx = std::shared_ptr<Scan_ctx>(
    UT_NEW_NOKEY(Scan_ctx(this, m_scan_ctx_id, trx, config)),
    [](Scan_ctx *scan_ctx) { UT_DELETE(scan_ctx); });

  if (scan_ctx.get() == nullptr)
  {
    ib::error() << "Out of memory";
    return (DB_OUT_OF_MEMORY);
  }

  m_scan_ctxs.push_back(scan_ctx);

  ++m_scan_ctx_id;

  scan_ctx->index_s_lock();

  Parallel_coordinator::Scan_ctx::Ranges ranges{};
  dberr_t err{DB_SUCCESS};

  /* Split at the root node (level == 0). */
  err = scan_ctx->partition(config.m_scan_range, ranges, 0);

  if (ranges.empty() || err != DB_SUCCESS)
  {
    /* Table is empty. */
    scan_ctx->index_s_unlock();
    return (err);
  }

  err = scan_ctx->create_contexts(ranges);

  scan_ctx->index_s_unlock();

  return (err);
}

int Parallel_coordinator::initialize(size_t n_workers)
{
  DBUG_ASSERT(!m_is_initialized);
  m_n_workers= n_workers;

  int err= mysql_mutex_init(PSI_NOT_INSTRUMENTED, &m_mutex, nullptr);
  if (err != 0)
    return err;

  m_worker_ctxs.reserve(n_workers);
  for (size_t i = 0; i < n_workers; ++i)
  {
    Worker_ctx *wctx = UT_NEW_NOKEY(Worker_ctx(i, this));
    if (wctx == nullptr)
    {
      for (auto *p : m_worker_ctxs) UT_DELETE(p);
      m_worker_ctxs.clear();
      mysql_mutex_destroy(&m_mutex);
      return HA_ERR_OUT_OF_MEM;
    }
    m_worker_ctxs.push_back(wctx);
  }

  m_is_initialized= true;
  return 0;
}

Parallel_coordinator::Worker_ctx*
Parallel_coordinator::get_worker_ctx(size_t worker_idx) const
{
  ut_a(worker_idx < m_worker_ctxs.size());
  return m_worker_ctxs[worker_idx];
}

  void Parallel_coordinator::cleanup()
  {
    if (!m_is_initialized) return;

    for (auto *p : m_worker_ctxs) UT_DELETE(p);
    m_worker_ctxs.clear();

    m_ctxs.clear();
    m_scan_ctxs.clear();
    m_scan_ctx_id = 0;
    m_ctx_id.store(0, std::memory_order_relaxed);
    m_n_workers = 0;

    m_err.store(DB_SUCCESS, std::memory_order_relaxed);

    mysql_mutex_destroy(&m_mutex);

    m_is_initialized = false;
  }