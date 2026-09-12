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

/** @file btr/btr0pscan.cc
Parallel scan partitioner implementation

Based on MySQL commit dbfc59ffaf80 created 2018-01-27 by Sunny Bains. */

#include <array>

#include "btr0pcur.h"
#include "dict0dict.h"
#include "row0mysql.h"
#include "btr0pscan.h"
#include "row0row.h"
#include "row0vers.h"
#include "ut0new.h"

/** Tree depth at which we decide to split blocks further. */
static constexpr size_t SPLIT_THRESHOLD{2};

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

Parallel_scan_partitioner::Scan_ctx::Boundary::~Boundary()
{
  if (m_heap == nullptr) {
    return;
  }

  mem_heap_free(m_heap);
  m_heap = nullptr;
}

void Parallel_scan_partitioner::Scan_ctx::index_s_lock()
{
  m_config.m_index->lock.s_lock(SRW_LOCK_CALL);
  m_s_locks.fetch_add(1, std::memory_order_release);
}

void Parallel_scan_partitioner::Scan_ctx::index_s_unlock()
{
  m_s_locks.fetch_sub(1, std::memory_order_release);
  m_config.m_index->lock.s_unlock();
}

dberr_t Parallel_scan_partitioner::Chunk::split()
{
  /*
    Twice the worker count, not once: the pieces this split produces are never
    re-split again (create_chunk() below flags none of them), so if the rows
    inside this chunk are themselves skewed, the only slack for evening that
    out is having more pieces than workers. Twice keeps the piece count -- and
    with it the per-piece descent from the root -- small, while leaving a
    second helping for whoever finishes early.
  */
  const size_t target= std::max(2 * m_scan_ctx->num_workers(), size_t{2});

  ut_ad(m_bounds.first->m_tuple == nullptr ||
        dtuple_validate(m_bounds.first->m_tuple));
  ut_ad(m_bounds.second->m_tuple == nullptr ||
        dtuple_validate(m_bounds.second->m_tuple));

  /* Setup the sub-range. Carry the inclusivity of this chunk's end so that
  create_bounds() keeps the sub-tree holding the boundary key. */
  Scan_range scan_range(m_bounds.first->m_tuple, m_bounds.second->m_tuple,
                        m_end_inclusive);

  /* S lock so that the tree structure doesn't change while we are
  figuring out the sub-trees to scan. */
  m_scan_ctx->index_s_lock();

  Parallel_scan_partitioner::Scan_ctx::Bounds_list bounds_list{};
  m_scan_ctx->partition(scan_range, bounds_list, 1);

  if (!bounds_list.empty())
    bounds_list.back().second = m_bounds.second;

  /* partition() at level 1 cuts this sub-tree at every child of its root, and
  for a tree of any depth that is one range per page at the level below --
  hundreds or thousands of them. That is far more than the shortfall this
  re-split exists to make up: a chunk is re-split because some worker had
  nothing to do, so a handful of pieces is enough, and every piece beyond that
  costs a descent from the root when a worker picks it up, plus the queue
  traffic to hand it over.

  Keep the boundaries the partitioning found and use only every m'th one.

  Adjacent chunks are contiguous intervals that share an endpoint, so
  merging a run of them is just taking the first one's start and the last
  one's end. */

  if (bounds_list.size() > target)
  {
    /*
      Floor, not ceiling: rounding m up makes ceil(size/m) groups, which for a
      size just over the target collapses to half of it -- 16 boundaries at
      target 15 would merge into 8 pieces. Rounding m down overshoots instead,
      bounded by twice the target, and too many small pieces costs less than
      too few large ones: the whole point of this split is idle workers.
    */
    const size_t m= std::max(bounds_list.size() / target, size_t{1});
    Parallel_scan_partitioner::Scan_ctx::Bounds_list merged{};

    for (size_t i= 0; i < bounds_list.size(); i+= m)
    {
      const size_t last= std::min(i + m, bounds_list.size()) - 1;
      merged.push_back(Parallel_scan_partitioner::Scan_ctx::Bounds(
          bounds_list[i].first, bounds_list[last].second));
    }
    bounds_list.swap(merged);
  }

  dberr_t err{DB_SUCCESS};

  /* Create the chunks of the partitioned scan. Only the last sub-chunk
  ends where this chunk ended, so only it inherits the inclusivity. */
  size_t i{};

  for (auto &bounds : bounds_list)
  {
    const bool last = (++i == bounds_list.size());

    err = m_scan_ctx->create_chunk(bounds, false,
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

Parallel_scan_partitioner::Scan_ctx::Scan_ctx(
    Parallel_scan_partitioner *partitioner, size_t id, trx_t *trx,
    const Parallel_scan_partitioner::Config &config)
    : m_id(id), m_config(config), m_trx(trx), m_partitioner(partitioner)
{}

buf_block_t *Parallel_scan_partitioner::Scan_ctx::block_get_s_latched(
    const page_id_t &page_id, mtr_t *mtr, size_t line) const
{
  /* We never scan undo tablespaces. */
  ut_a(!srv_is_undo_tablespace(page_id.space()));

  auto block =
      buf_page_get_gen(page_id, m_config.m_zip_size, RW_S_LATCH, nullptr,
                       BUF_GET, mtr);

  return (block);
}


void Parallel_scan_partitioner::Scan_ctx::copy_row(const rec_t *rec,
                                                   Boundary *boundary) const
{
  const ulint n_core = page_rec_is_leaf(rec) ? m_config.m_index->n_core_fields : 0;
  boundary->m_offsets = rec_get_offsets(rec, m_config.m_index, nullptr, n_core,
                                        ULINT_UNDEFINED, &boundary->m_heap);

  // Copy the raw record bytes into the boundary's heap.
  ulint rec_len = rec_offs_size(boundary->m_offsets);
  rec_t *copy_rec =
      static_cast<rec_t *>(mem_heap_alloc(boundary->m_heap, rec_len));
  memcpy(copy_rec, rec, rec_len);
  boundary->m_rec = copy_rec;

  // Build a key-only dtuple (just the unique-in-tree fields).
  const ulint n_unique = dict_index_get_n_unique_in_tree(m_config.m_index);

  dtuple_t *tuple = dtuple_create(boundary->m_heap, n_unique);
  dict_index_copy_types(tuple, m_config.m_index, n_unique);

  for (ulint i = 0; i < n_unique; ++i) {
    ulint len;
    const byte *data =
        rec_get_nth_field(boundary->m_rec, boundary->m_offsets, i, &len);
    dfield_t *dfield = dtuple_get_nth_field(tuple, i);
    dfield_set_data(dfield, data, len);
  }
  dtuple_set_n_fields_cmp(tuple, n_unique);

  boundary->m_tuple = tuple;
}

std::shared_ptr<Parallel_scan_partitioner::Scan_ctx::Boundary>
Parallel_scan_partitioner::Scan_ctx::snapshot_boundary(
    const page_cur_t &page_cursor) const
{
  ut_a(m_config.m_read_level == 0);
  ut_ad(index_s_own());

  std::shared_ptr<Boundary> boundary = std::make_shared<Boundary>();

  boundary->m_heap = mem_heap_create(srv_page_size / 16);

  auto rec = page_cursor.rec;

  const bool is_infimum = page_rec_is_infimum(rec);

  if (is_infimum) {
    rec = page_rec_get_next(rec);
  }

  if (page_rec_is_supremum(rec)) {
    /* Empty page, only root page can be empty. */
    ut_a(!is_infimum ||
         page_cursor.block->page.id().page_no() == m_config.m_index->page);
    return (boundary);
  }

  /* Make a copy of the rec. The tuple built from it points into that copy,
  so the boundary stays valid once create_bounds() releases the block
  latches. */
  copy_row(rec, boundary.get());

  return (boundary);
}

void Parallel_scan_partitioner::enqueue(std::shared_ptr<Chunk> chunk)
{
  mysql_mutex_lock(&m_mutex);
  m_chunk_queue.push_back(chunk);
  /* Every chunk that becomes work passes through here, the ones a re-split
  produced along with the ones the first partitioning did. */
  ++m_chunks_created;
  mysql_cond_signal(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}

std::shared_ptr<Parallel_scan_partitioner::Chunk>
Parallel_scan_partitioner::dequeue()
{
  mysql_mutex_lock(&m_mutex);

  /* An empty queue does not always mean the scan is finished: a chunk
  flagged m_to_be_resplit may not have yet enqueued its sub-chunks,
  so wait until the re-split is finished. */
  while (m_chunk_queue.empty() && m_n_resplitting > 0 && !is_error_set())
    mysql_cond_wait(&m_cond, &m_mutex);

  if (m_chunk_queue.empty() || is_error_set()) {
    mysql_mutex_unlock(&m_mutex);
    return (nullptr);
  }

  auto chunk = m_chunk_queue.front();
  m_chunk_queue.pop_front();

  if (chunk->m_to_be_resplit)
    ++m_n_resplitting;

  mysql_mutex_unlock(&m_mutex);

  return (chunk);
}

dberr_t
Parallel_scan_partitioner::get_next_chunk(std::shared_ptr<Chunk> *chunk_out)
{
  /* Pull the next scannable chunk. A chunk flagged m_to_be_resplit is not
  itself scanned: split() re-partitions its sub-tree one level deeper and
  enqueues the finer sub-chunks (for this worker and others), then we
  pull again. This is the pull-based equivalent of the m_to_be_resplit
  handling in Parallel_scan_partitioner::worker() (which we don't use here) —
  it is what turns the few coarse root-subtree ranges into one-leaf-page
  chunks on deep trees. Without it, a deep B-tree with a narrow root yields
  only a handful of huge chunks and almost no worker parallelism. */
  *chunk_out = nullptr;

  for (;;)
  {
    std::shared_ptr<Parallel_scan_partitioner::Chunk> chunk = dequeue();

    if (chunk == nullptr)
    {
      /* Either the scan is over or it has failed; m_err says which. */
      return get_error_state();
    }

    if (chunk->is_error_set())
      return chunk->get_error_state();

    if (chunk->m_to_be_resplit)
    {
      const dberr_t err = chunk->split();

      if (err != DB_SUCCESS)
      {
        chunk->m_scan_ctx->set_error_state(err);
        set_error_state(err);
      }

      mysql_mutex_lock(&m_mutex);
      ut_ad(m_n_resplitting > 0);
      --m_n_resplitting;
      /* This chunk was divided instead of being scanned; its pieces counted
      themselves in as they were enqueued. */
      ++m_chunks_resplit;
      mysql_cond_broadcast(&m_cond);
      mysql_mutex_unlock(&m_mutex);

      if (err != DB_SUCCESS)
        return err;

      /* The sub-chunks are now queued; pull one for this worker. */
      continue;
    }

    *chunk_out = chunk;
    return DB_SUCCESS;
  }
}

dberr_t Parallel_scan_partitioner::Scan_ctx::search(buf_block_t *block,
                                                    const dtuple_t *key,
                                                    page_no_t *page_no) const
{
  ut_ad(index_s_own());
  *page_no = FIL_NULL;

  page_cur_t page_cursor;
  const auto index = m_config.m_index;

  if (key != nullptr)
  {
    if (dberr_t ps_err = pread_page_cur_search(block, index, key,
                                               PAGE_CUR_L, &page_cursor))
      return ps_err;
  }
  else
  {
    page_cur_set_before_first(block, &page_cursor);
  }

  if (page_rec_is_infimum(page_cur_get_rec(&page_cursor)))
  {
    rec_t *next = page_cur_move_to_next(&page_cursor);
    if (!next)
      return DB_CORRUPTION;
  }

  const auto rec = page_cur_get_rec(&page_cursor);

  mem_heap_t *heap = nullptr;

  rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
  auto offsets = offsets_;

  rec_offs_init(offsets_);

  const ulint n_core = page_rec_is_leaf(rec) ? index->n_core_fields : 0;
  offsets = rec_get_offsets(rec, index, offsets, n_core,
                            ULINT_UNDEFINED, &heap);

  *page_no = btr_node_ptr_get_child_page_no(rec, offsets);

  if (heap != nullptr)
  {
    mem_heap_free(heap);
  }

  return DB_SUCCESS;
}

dberr_t Parallel_scan_partitioner::Scan_ctx::start_range(
    page_no_t page_no, mtr_t *mtr, const dtuple_t *key,
    Savepoints &savepoints, page_cur_t *cursor) const {
  ut_ad(index_s_own());

  auto index = m_config.m_index;
  page_id_t page_id(index->table->space_id, page_no);
  ulint height{};

  /* Follow the left most pointer down on each page. */
  for (;;) {
    auto savepoint = mtr->get_savepoint();

    auto block = block_get_s_latched(page_id, mtr, __LINE__);

    if (!block) {
      /* Page fetch failed — typically tablespace corruption. */
      return DB_CORRUPTION;
    }

    height = btr_page_get_level(buf_block_get_frame(block));

    savepoints.push_back({savepoint, block});

    if (height != 0 && height != m_config.m_read_level)
    {
      page_no_t child_no;
      if (dberr_t s_err = search(block, key, &child_no))
        return s_err;

      page_id.set_page_no(child_no);
      continue;
    }

    page_cur_t page_cursor;
    page_cursor.index = index;

    if (key != nullptr)
    {
      if (dberr_t ps_err = pread_page_cur_search(block, index, key,
                                                 PAGE_CUR_GE, &page_cursor))
        return ps_err;
    }
    else
    {
      page_cur_set_before_first(block, &page_cursor);
    }

    if (page_rec_is_infimum(page_cur_get_rec(&page_cursor)))
    {
      rec_t *next = page_cur_move_to_next(&page_cursor);
      if (!next)
        return DB_CORRUPTION;
    }

    /* search() descends with the strict PAGE_CUR_L, which can stop one
    sub-tree short of `key`: it lands on the last leaf whose lowest key is
    below `key`, and every record on that leaf may still be below `key`. The
    record we want is then the first one on the following page - the leaf after
    it starts at or above `key` by construction. */
    while (page_cur_is_after_last(&page_cursor))
    {
      const auto next_no = btr_page_get_next(buf_block_get_frame(block));

      /* End of the level: nothing at or after `key`. The caller sees the
      supremum and creates no range. */
      if (next_no == FIL_NULL)
        break;

      /* Drop the page we are leaving: the record we want is further right */
      ut_ad(!savepoints.empty() && savepoints.back().second == block);
      mtr->release(*block);
      savepoints.pop_back();

      savepoint = mtr->get_savepoint();
      page_id.set_page_no(next_no);

      block = block_get_s_latched(page_id, mtr, __LINE__);

      if (!block)
        return DB_CORRUPTION;

      savepoints.push_back({savepoint, block});

      page_cur_set_before_first(block, &page_cursor);

      if (!page_cur_move_to_next(&page_cursor))
        return DB_CORRUPTION;
    }

    *cursor = page_cursor;
    return DB_SUCCESS;
  }
}

void Parallel_scan_partitioner::Scan_ctx::add_boundary(
  Bounds_list &bounds_list, page_cur_t &leaf_page_cursor) const {
  leaf_page_cursor.index = m_config.m_index;

  auto boundary = snapshot_boundary(leaf_page_cursor);

  /* This boundary ends the range that is still open. */
  if (!bounds_list.empty()) {
    ut_a(bounds_list.back().second->m_heap == nullptr);
    bounds_list.back().second = boundary;
  }

  /* Open the next range here; its end is filled in by the next boundary. */
  bounds_list.push_back(Bounds(boundary, std::make_shared<Boundary>()));
}

dberr_t Parallel_scan_partitioner::Scan_ctx::create_bounds(
    const Scan_range &scan_range, page_no_t page_no, size_t depth,
    const size_t split_level, Bounds_list &bounds_list, mtr_t *mtr) {
  ut_ad(index_s_own());
  ut_a(page_no != FIL_NULL);

  /* Do a breadth first traversal of the B+Tree using recursion. We want to
  set up the chunk boundaries in one pass. This guarantees that the tree
  structure cannot change while we are creating the sub-ranges.

  Once we have the boundaries of a sub-tree we can release
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
    /* On a leaf, `start` is where the scan begins, so we want the first record
    >= start: PAGE_CUR_GE. One level up the same bound asks something else - a
    node pointer key is the *lowest* key of its sub-tree, so we need the child
    at or before `start`, and PAGE_CUR_L rather than LE because `start` may be
    a prefix (see Scan_ctx::search()). */
    const page_cur_mode_t mode= page_is_leaf(buf_block_get_frame(block))
      ? PAGE_CUR_GE : PAGE_CUR_L;

    auto err = pread_page_cur_search(block, index, start,
                                     mode, &page_cursor);
    if (err != DB_SUCCESS)
      return err;

    if (page_cur_is_after_last(&page_cursor))
    {
      /* Only the leaf mode (GE) can land here; L stops at the infimum at
      worst. `start` is past every record on this leaf, so this sub-tree
      contributes no range */
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
        create_bounds(scan_range, page_no, depth + 1, split_level,
                      bounds_list, mtr);

        rec_t *next = page_cur_move_to_next(&page_cursor);
        if (!next)
          return DB_CORRUPTION;

        continue;
      }

      /* Find the range start in the leaf node. */
      if (dberr_t sr_err = start_range(page_no, mtr, start, savepoints,
                                       &level_page_cursor))
        return sr_err;
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
      add_boundary(bounds_list, level_page_cursor);
    }

    /* We've created the range, safe to release S latches on
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

dberr_t Parallel_scan_partitioner::Scan_ctx::partition(
    const Scan_range &scan_range,
    Parallel_scan_partitioner::Scan_ctx::Bounds_list &bounds_list,
    size_t split_level)
{
  ut_ad(index_s_own());

  mtr_t mtr(m_trx);
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  dberr_t err{DB_SUCCESS};

  err = create_bounds(scan_range, m_config.m_index->page, 0, split_level,
                      bounds_list, &mtr);

  /* Stamp the scan's own upper bound onto the last chunk. Whether the bound
  itself belongs to the scan travels separately, on the Chunk built from
  this range, and reaches the clamp via pscan_chunk_clamp.reset_to(). */
  if (err == DB_SUCCESS && scan_range.m_end != nullptr &&
      !bounds_list.empty()) {
    auto &boundary = bounds_list.back().second;

    ut_a(boundary->m_heap == nullptr);

    boundary->m_heap = mem_heap_create(srv_page_size / 16);

    boundary->m_tuple = dtuple_copy(scan_range.m_end, boundary->m_heap);

    /* Do a deep copy. */
    for (size_t i = 0; i < dtuple_get_n_fields(boundary->m_tuple); ++i) {
      dfield_dup(&boundary->m_tuple->fields[i], boundary->m_heap);
    }
  }

  mtr.commit();
  return (err);
}

dberr_t Parallel_scan_partitioner::Scan_ctx::create_chunk(const Bounds &bounds,
                                                          bool resplit,
                                                          bool end_inclusive)
{
  auto chunk = std::shared_ptr<Parallel_scan_partitioner::Chunk>(
    UT_NEW_NOKEY(Parallel_scan_partitioner::Chunk(this, bounds)),
    [](Parallel_scan_partitioner::Chunk *chunk) { UT_DELETE(chunk); });

  dberr_t err{DB_SUCCESS};

  if (chunk.get() == nullptr)
  {
    return (DB_OUT_OF_MEMORY);
  }
  else
  {
    chunk->m_to_be_resplit = resplit;
    chunk->m_end_inclusive = end_inclusive;
    m_partitioner->enqueue(chunk);
  }

  return (err);
}

dberr_t Parallel_scan_partitioner::Scan_ctx::create_chunks(
    const Bounds_list &bounds_list)
{
  size_t split_point{};

  {
    const auto n = std::max(num_workers(), size_t{1});

    if (bounds_list.size() > n) {
      split_point = (bounds_list.size() / n) * n;
    } else if (m_depth < SPLIT_THRESHOLD) {
      /* If the tree is not very deep then don't split. For smaller tables
      it is more expensive to split because we end up traversing more blocks*/
      split_point = n;
    }
  }

  size_t i{};

  for (auto bounds : bounds_list) {
    /* Only the last chunk ends at the caller's upper bound, so only it can
    be inclusive. */
    const bool last = (i + 1 == bounds_list.size());

    auto err = create_chunk(bounds, i >= split_point,
                            last && m_config.m_scan_range.m_end_inclusive);

    if (err != DB_SUCCESS) {
      return (err);
    }

    ++i;
  }

  return DB_SUCCESS;
}

dberr_t Parallel_scan_partitioner::add_scan(
    trx_t *trx, const Parallel_scan_partitioner::Config &config)
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

  Parallel_scan_partitioner::Scan_ctx::Bounds_list bounds_list{};
  dberr_t err{DB_SUCCESS};

  /* Split at the root node (level == 0). */
  err = scan_ctx->partition(config.m_scan_range, bounds_list, 0);

  if (bounds_list.empty() || err != DB_SUCCESS)
  {
    /* Table is empty. */
    scan_ctx->index_s_unlock();
    return (err);
  }

  err = scan_ctx->create_chunks(bounds_list);

  scan_ctx->index_s_unlock();

  return (err);
}

int Parallel_scan_partitioner::initialize(size_t n_workers)
{
  DBUG_ASSERT(!m_is_initialized);
  m_n_workers= n_workers;

  int err= mysql_mutex_init(PSI_NOT_INSTRUMENTED, &m_mutex, nullptr);
  if (err != 0)
    return err;

  mysql_cond_init(PSI_NOT_INSTRUMENTED, &m_cond, nullptr);
  m_n_resplitting= 0;
  m_chunks_created= 0;
  m_chunks_resplit= 0;

  m_is_initialized= true;
  return 0;
}

void Parallel_scan_partitioner::cleanup()
{
  if (!m_is_initialized) return;

  m_chunk_queue.clear();
  m_scan_ctxs.clear();
  m_scan_ctx_id = 0;
  m_n_workers = 0;
  m_n_resplitting = 0;

  m_err.store(DB_SUCCESS, std::memory_order_relaxed);

  mysql_cond_destroy(&m_cond);
  mysql_mutex_destroy(&m_mutex);

  m_is_initialized = false;
}
