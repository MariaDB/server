/*****************************************************************************

Copyright (c) 2018, 2025, Oracle and/or its affiliates.
Copyright (c) 2026, MariaDB

 OLEGS: adjust the text

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

/** @file row/row0pread.cc
Parallel read implementation

Created 2018-01-27 by Sunny Bains */

#include <array>

#include "btr0pcur.h"
#include "dict0dict.h"
//#include "os0thread-create.h"
#include "row0mysql.h"
#include "row0pread.h"
#include "row0row.h"
#include "row0vers.h"
#include "ut0new.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t parallel_read_thread_key;
#endif /* UNIV_PFS_THREAD */

std::atomic_size_t Parallel_reader::s_active_threads{};

/** Tree depth at which we decide to split blocks further. */
static constexpr size_t SPLIT_THRESHOLD{3};

/** No. of pages to scan, in the case of large tables, before the check for
trx interrupted is made as the call is expensive. */
static constexpr size_t TRX_IS_INTERRUPTED_PROBE{50000};

/* Local helper for parallel-reader ports: MariaDB removed the
page_cur_search() wrapper, but this file's MySQL-port code hits the
same setup pattern in 4 places. None of the call sites use the
matched-fields count; corruption must still be propagated. */
// OLEGS: add [[nodiscard]] and handle errors
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

std::string Parallel_reader::Scan_range::to_string() const {
  std::ostringstream os;

  os << "m_start: ";
  if (m_start != nullptr)
  {
    m_start->print(os);
  } else {
    os << "null";
  }
  os << ", m_end: ";
  if (m_end != nullptr)
  {
    m_end->print(os);
  } else
  {
    os << "null";
  }
  return (os.str());
}

Parallel_reader::Scan_ctx::Iter::~Iter()
{
  if (m_heap == nullptr) {
    return;
  }

  if (m_pcur != nullptr) {
    ut_free(m_pcur->old_rec_buf);
    /* Created with placement new on the heap, call destructor */
    m_pcur->~btr_pcur_t();
  }

  mem_heap_free(m_heap);
  m_heap = nullptr;
}

size_t Parallel_reader::available_threads(size_t n_required,
                                          bool use_reserved)
{
  auto max_threads = MAX_THREADS;
  constexpr auto SEQ_CST = std::memory_order_seq_cst;
  auto active = s_active_threads.fetch_add(n_required, SEQ_CST);

  if (use_reserved) {
    max_threads += MAX_RESERVED_THREADS;
  }

  if (active < max_threads) {
    const auto available = max_threads - active;

    if (n_required <= available) {
      return n_required;
    } else {
      const auto release = n_required - available;
      const auto o = s_active_threads.fetch_sub(release, SEQ_CST);
      ut_a(o >= release);
      return available;
    }
  }

  const auto o = s_active_threads.fetch_sub(n_required, SEQ_CST);
  ut_a(o >= n_required);

  return 0;
}

void Parallel_reader::Scan_ctx::index_s_lock()
{
  if (m_s_locks.fetch_add(1, std::memory_order_acquire) == 0) {
    // OLEGS: MySQL states that another thread may release this lock,
    // however in MariaDB that will trigger a debug assertion.
    // The same thread must release the lock
    m_config.m_index->lock.s_lock(SRW_LOCK_CALL);
  }
}

void Parallel_reader::Scan_ctx::index_s_unlock()
{
  if (m_s_locks.fetch_sub(1, std::memory_order_acquire) == 1) {
    m_config.m_index->lock.s_unlock();
  }
}

dberr_t Parallel_reader::Ctx::split()
{
  ut_ad(m_range.first->m_tuple == nullptr ||
        dtuple_validate(m_range.first->m_tuple));
  ut_ad(m_range.second->m_tuple == nullptr ||
        dtuple_validate(m_range.second->m_tuple));

  /* Setup the sub-range. */
  Scan_range scan_range(m_range.first->m_tuple, m_range.second->m_tuple);

  /* S lock so that the tree structure doesn't change while we are
  figuring out the sub-trees to scan. */
  m_scan_ctx->index_s_lock();

  Parallel_reader::Scan_ctx::Ranges ranges{};
  m_scan_ctx->partition(scan_range, ranges, 1);

  if (!ranges.empty())
    ranges.back().second = m_range.second;

  dberr_t err{DB_SUCCESS};

  /* Create the partitioned scan execution contexts. */
  for (auto &range : ranges)
  {
    err = m_scan_ctx->create_context(range, false);

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

Parallel_reader::Scan_ctx::Scan_ctx(Parallel_reader *reader, size_t id,
                                    trx_t *trx,
                                    const Parallel_reader::Config &config,
                                    F &&f)
    : m_id(id), m_config(config), m_trx(trx), m_f(f), m_reader(reader) {}

/** Persistent cursor wrapper around btr_pcur_t */
class PCursor
{
 public:
  /** Constructor.
  @param[in,out] pcur           Persistent cursor in use.
  @param[in] mtr                Mini-transaction used by the persistent cursor.
  @param[in] read_level         Read level where the block should be present. */
  PCursor(btr_pcur_t *pcur, mtr_t *mtr, size_t read_level)
      : m_mtr(mtr), m_pcur(pcur), m_read_level(read_level) {}

  /** Store the position of the user record that is added to the key buffer and,
  commit the mini-transaction. The cursor must not be positioned at infimum
  or supremum.
  @note This method must be paired with
  restore_to_last_processed_user_record() to restore the record position */
  void save_current_user_record_as_last_processed() noexcept;

  /** Restore the cursor to the saved record. If the saved record was physically
  removed then restore to the largest record which is smaller than the saved
  record. If all records were removed then restore the cursor to the infimum;
  this must be fine if the next step of the caller is to advance the cursor.
  @note This method must be paired with
  save_current_user_record_as_last_processed() to save the record position */
  void restore_to_last_processed_user_record() noexcept;

  /** This method must be called after all records on a page are processed and
  cursor is positioned at supremum. Under this assumption, it stores the
  position BTR_PCUR_AFTER the last user record on the page.
  This method must be paired with restore_to_first_unprocessed() to restore to
  a record which comes right after the value of the stored last processed
  record @see restore_to_first_unprocessed for details. */
  void save_previous_user_record_as_last_processed() noexcept;

  /** Restore the cursor to the record which comes next after the saved
  position by the paired method save_previous_user_record_as_last_processed().
  If there is no such record, it will restore to supremum on the last leaf
  page.
  @note This method must be paired with
  save_previous_user_record_as_last_processed() to save the record position */
  void restore_to_first_unprocessed() noexcept;

  /** Move to the next block.
  @param[in]  index             Index being traversed.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t move_to_next_block(dict_index_t *index);

  /** Restore the cursor to the saved record. If the saved record
  was physically removed, then restore to the largest record which
  is smaller than the saved record.
  @return true if restored to the same record, false otherwise */
  bool restore_to_largest_le_position_saved() noexcept;

  /** @return the current page cursor. */
  [[nodiscard]] page_cur_t *get_page_cursor() noexcept
  {
    return btr_pcur_get_page_cur(m_pcur);
  }

  /** @return true if cursor is after last on page. */
  [[nodiscard]] bool is_after_last_on_page() const noexcept
  {
    return btr_pcur_is_after_last_on_page(m_pcur);
  }

  /** @return Level where the cursor is intended. */
  size_t read_level() const noexcept { return m_read_level; }

 private:
  /** This method must be called when cursor is positioned at supremum of
  the page at the level one of the index. It saves the min-key value of
  the last leaf page referenced on the page.
  This method must be paired with restore_level_one_progress() to restore
  cursor to a record on the non-leaf page which points to the next leaf
  page to process.
  @see restore_level_one_progress for details. */
  void save_level_one_progress() noexcept;

  /** This method must be called to restore to the record on the non-leaf page
  (level one) of the index. It must be paired with save_level_one_progress().
  If the cursor was saved to the record <min-key value, child_page_no> position,
  then the restored cursor is positioned to the
  <min-key value+, child_page_no^>. The min-key value+ is the value that comes
  next after the saved value min-key value.
  As PAGE_CUR_G mode is not supported for level 1, this function searches for
  such a record in two steps:
  1. using PAGE_CUR_LE it finds a record which is lower-or-equal min_key value
  2. adjusts the position by moving one record forward
  The later step might require moving to the next page, and this function
  tries to latch the next page without any other thread waiting to latch it.
  If the latch could not be obtained on the page, then the method returns the
  DB_LOCK_NOWAIT error code. The caller must implement retry logic to handle
  this error code.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t restore_level_one_progress() noexcept;

  /** Move to the next block at the leaf level of the index. If other
  thread(s) are waiting to acquire the latches on the index then first
  save the last processed user record, pause, give chance to the other
  threads to execute, then restore the cursor to next unprocessed record,
  which typically is the first record on next page, if the tree was not
  modified meanwhile.
  @param[in]  index   Index being traversed.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t move_to_next_block_at_leaf_level(dict_index_t *index);

  /** Move to the next block at the level one of the index. If other
  thread(s) are waiting to acquire the latches on the index then first
  pause, give chance the other threads to execute, then restore the
  cursor before positioning it to user record on next page. The latch
  on next page is acquired without waiting in a retry loop, otherwise
  waiting might cause deadlock with other tree modifying operations.
  @param[in]  index   Index being traversed.
  @return DB_SUCCESS or error code. */

  // OLEGS: used only for histogram sampling, can be removed
  //[[nodiscard]] dberr_t move_to_next_block_at_level_one(dict_index_t *index);

  /** Move to the first user record on the next page. Release the latches
  on the current page and obtain the S-latch on next page. If it is non-leaf
  level then S-latch is obtained in the optimistic/non-blocking way, which
  can fail with DB_LOCK_WAIT. Caller must add the retry logic to handle
  this error code.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t move_to_user_rec_on_next_page() noexcept;

  /** Mini-transaction. */
  mtr_t *m_mtr{};

public: // OLEGS debugging, remove this line
  /** Persistent cursor. */
  btr_pcur_t *m_pcur{};

  /** Level where the cursor is positioned or need to be positioned in case of
  restore. */
  size_t m_read_level{};
};

buf_block_t *Parallel_reader::Scan_ctx::block_get_s_latched(
    const page_id_t &page_id, mtr_t *mtr, size_t line) const
{
  /* We never scan undo tablespaces. */
  ut_a(!srv_is_undo_tablespace(page_id.space()));

  auto block =
  // OLEGS: no straight equivalent to MySQL Page_fetch::SCAN, replaced with BUF_GET
  // which can cause some LRU churn and random read-ahead
      // buf_page_get_gen(page_id, m_config.m_page_size, RW_S_LATCH, nullptr,
      //                  Page_fetch::SCAN, {__FILE__, line}, mtr);
      buf_page_get_gen(page_id, m_config.m_zip_size, RW_S_LATCH, nullptr,
                       BUF_GET, mtr);

  return (block);
}

void PCursor::save_level_one_progress() noexcept
{
  ut_ad(m_read_level == 1);
  ut_ad(btr_pcur_is_after_last_on_page(m_pcur));

  /* Store the cursor position on the previous user record on the page.

  @note: If btr_pcur_t::store_position() detects that cursor is positioned to
  after the last record then it moves the position to previous user record too.
  The reason we have to move to previous user record explicitly is that,
  btr_pcur_t::store_position() changes the relative position to BTR_PCUR_AFTER
  in that case btr_pcur_t::restore_position() restores the next user record
  which is not allowed at non-leaf level. We use modified search modes on the
  non-leaf levels, @see btr_cur_search_to_nth_level for details. Therefore, we
  explicitly move to cursor position to previous record */
  [[maybe_unused]] rec_t *rec= btr_pcur_move_to_prev_on_page(m_pcur);
  // OLEGS: handle rec==nullptr

  btr_pcur_store_position(m_pcur, m_mtr);

  ut_ad(btr_pcur_get_rel_pos(m_pcur) == BTR_PCUR_ON);

  m_mtr->commit();
}

dberr_t PCursor::restore_level_one_progress() noexcept
{
  ut_ad(m_read_level == 1);
  ut_ad(btr_pcur_get_rel_pos(m_pcur) == BTR_PCUR_ON);
  m_mtr->start();
  m_mtr->set_log_mode(MTR_LOG_NO_REDO);

  restore_to_largest_le_position_saved();

  /* Move to the next record on the page */
  // OLEGS: check for nullptr (corrupted page)
  [[maybe_unused]] rec_t *next= page_cur_move_to_next(get_page_cursor());

  if (btr_pcur_is_on_user_rec(m_pcur))
  {
    /* Cursor is still on a user record so we can claim success */
    return DB_SUCCESS;
  }

  return move_to_user_rec_on_next_page();
}

void PCursor::save_current_user_record_as_last_processed() noexcept
{
  ut_a(btr_pcur_is_on_user_rec(m_pcur));
  ut_ad(m_read_level == 0);
  btr_pcur_store_position(m_pcur, m_mtr);
  m_mtr->commit();
}

void PCursor::restore_to_last_processed_user_record() noexcept
{
  ut_a(btr_pcur_get_rel_pos(m_pcur) == BTR_PCUR_ON);
  ut_ad(m_read_level == 0);
  m_mtr->start();
  m_mtr->set_log_mode(MTR_LOG_NO_REDO);

  [[maybe_unused]] const btr_pcur_t::restore_status status =
      m_pcur->restore_position(BTR_SEARCH_LEAF, m_mtr);

  /* Restored cursor is positioned on the page at the level intended before */
  ut_ad(m_read_level == btr_page_get_level(btr_pcur_get_page(m_pcur)));

  /* Since m_rel_pos was BTR_PCUR_ON, the state can't be
  BTR_PCUR_IS_POSITIONED_OPTIMISTIC */
  ut_a(BTR_PCUR_IS_POSITIONED_OPTIMISTIC != m_pcur->pos_state);

  /* If the cursor was pointing to the user record then 'PAGE_CUR_LE' mode
  used by the btr_pcur_t::restore_position() restores the cursor to a record
  lower than the original one if it was removed, and in particular, if all
  smaller ones were removed, cur could be on infimum now. This would be fine,
  as our next step will be to advance the cursor, anyway.
  However, right now the only usage of this method is in the bulk inserter
  for a B-Tree. which only calls it when the current row was visible to the
  scan, and thus can't be purged. So, we know it can't happen, unless someone
  reuses this function in a new context, in which case they should probably
  repeat the proof of correctness. The purpose of following debug assert is
  to show the limited the scope of this method, and thus how risky it
  is to use it in new context: */

  // OLEGS: no check for CORRUPTED, consider adding
  ut_ad(status == btr_pcur_t::SAME_ALL ||
        status == btr_pcur_t::SAME_UNIQ);
}

void PCursor::save_previous_user_record_as_last_processed() noexcept
{
  ut_a(btr_pcur_is_after_last_on_page(m_pcur));
  ut_ad(m_read_level == 0);
  btr_pcur_store_position(m_pcur, m_mtr);
  ut_a(btr_pcur_get_rel_pos(m_pcur) == BTR_PCUR_AFTER);
  m_mtr->commit();
}

void PCursor::restore_to_first_unprocessed() noexcept
{
  ut_a(btr_pcur_get_rel_pos(m_pcur) == BTR_PCUR_AFTER);
  ut_ad(m_read_level == 0);
  m_mtr->start();
  m_mtr->set_log_mode(MTR_LOG_NO_REDO);
  auto status= m_pcur->restore_position(BTR_SEARCH_LEAF, m_mtr);
  if (status == btr_pcur_t::CORRUPTED)
  {
    // OLEGS:
    // propagate corruption — set scan error state, abort
    ut_a(false);   // assertion for now
  }

  /* Restored cursor is positioned on the page at the level intended before */
  ut_ad(m_read_level == btr_page_get_level(btr_pcur_get_page(m_pcur)));

  /* The BTR_PCUR_IS_POSITIONED_OPTIMISTIC only happens in case of a successful
  optimistic restoration in which the cursor points to a user record after
  restoration. But, in save_previous_user_record_as_last_processed() the cursor
  pointed to SUPREMUM before calling m_ptr->store_position(mtr), so it would
  also point there if optimistic restoration succeeded, which is not a user
  record. */
  ut_ad(m_pcur->pos_state != BTR_PCUR_IS_POSITIONED_OPTIMISTIC);
}

bool PCursor::restore_to_largest_le_position_saved() noexcept
{
  ut_ad(btr_pcur_get_rel_pos(m_pcur) == BTR_PCUR_ON);
  ut_ad(m_mtr->is_active());

  const btr_pcur_t::restore_status status =
      m_pcur->restore_position(BTR_SEARCH_LEAF, m_mtr);

  ut_ad(!btr_pcur_is_after_last_on_page(m_pcur));

  // OLEGS: no check for CORRUPTED, consider adding
  return status == btr_pcur_t::SAME_ALL ||
         status == btr_pcur_t::SAME_UNIQ;
}

dberr_t PCursor::move_to_user_rec_on_next_page() noexcept
{
  auto cur = btr_pcur_get_page_cur(m_pcur);
  const auto next_page_no = btr_page_get_next(page_cur_get_page(cur));

  if (next_page_no == FIL_NULL)
  {
    m_mtr->commit();
    return DB_END_OF_INDEX;
  }

  buf_block_t *current_block = page_cur_get_block(cur);
  const auto space_id = current_block->page.id().space();
  /// OLEGS: we don't have such, not clear how to port
  //const auto page_size = current_block->page.size;
  const auto zip_size = current_block->page.zip_size();

  DEBUG_SYNC_C("parallel_reader_next_block");

  /* We never scan undo tablespaces. */
  ut_a(!srv_is_undo_tablespace(space_id));
  ut_a(m_read_level == 0);
  
  dberr_t bp_err = DB_SUCCESS;
  buf_block_t *next_block = buf_page_get_gen(
      page_id_t(space_id, next_page_no), zip_size,
      RW_S_LATCH, nullptr, BUF_GET, m_mtr, &bp_err);
  if (!next_block || bp_err != DB_SUCCESS)
  {
    m_mtr->commit();
    return DB_CORRUPTION;
  }

  m_mtr->release(*current_block);

  page_cur_set_before_first(next_block, cur);
  // OLEGS: check for nullptr (corrupted page)
  rec_t *next= page_cur_move_to_next(cur);  // Skip the infimum record
  if (!next) {
    m_mtr->commit();
    return DB_CORRUPTION;
  }

  /* Page can't be empty unless it is a root page. */
  ut_ad(!page_cur_is_after_last(cur));

  return DB_SUCCESS;
}

void Parallel_reader::Thread_ctx::
    save_current_user_record_as_last_processed() noexcept
{
  m_pcursor->save_current_user_record_as_last_processed();
}

void Parallel_reader::Thread_ctx::
    restore_to_last_processed_user_record() noexcept
{
  m_pcursor->restore_to_last_processed_user_record();
}

void Parallel_reader::Thread_ctx::
    save_previous_user_record_as_last_processed() noexcept
{
  m_pcursor->save_previous_user_record_as_last_processed();
}

void Parallel_reader::Thread_ctx::restore_to_first_unprocessed() noexcept
{
  m_pcursor->restore_to_first_unprocessed();
}

dberr_t PCursor::move_to_next_block_at_leaf_level(dict_index_t *index)
{
  ut_ad(m_read_level == 0);
  /// OLEGS: temporarily disabled
  //if (DBUG_EVALUATE_IF("pcursor_move_to_next_block_release_latches", true,
  //                     rw_lock_get_waiters(dict_index_get_lock(index))))
  if (false)
  {
    save_previous_user_record_as_last_processed();
    DEBUG_SYNC_C("pcursor_move_to_next_block_latches_released");
    std::this_thread::yield();
    restore_to_first_unprocessed();
    if (btr_pcur_is_after_last_on_page(m_pcur)) {
      /* There's nothing wrong here, as restore_to_first_unprocessed() is not
      obliged to position the cursor on a *user* record - it can be a
      supremum. This can happen if
      a) the block in BP wasn't modified and thus we've restored the cursor
      exactly where it was, i.e. on supremum
      b) there is no more records in the table
      These options aren't mutually exclusive. Both are well handled by: */
      return move_to_user_rec_on_next_page();
    }
    /* This happens if we were descending the B-tree, in which case we try
    to navigate in such a way to arrive at a user record. */
    ut_a(btr_pcur_is_on_user_rec(m_pcur));
    return DB_SUCCESS;
  }
  return move_to_user_rec_on_next_page();
}

// dberr_t PCursor::move_to_next_block_at_level_one(dict_index_t *index) {
//   ut_ad(m_read_level != 0);
//   dberr_t err = DB_SUCCESS;

//   if (rw_lock_get_waiters(dict_index_get_lock(index))) {
//     /* There are waiters on the index tree lock. Store and restore
//     the cursor position, and yield so that scanning a large table
//     will not starve other threads. */

//     /* We should always yield on a block boundary. */
//     ut_ad(btr_pcur_is_after_last_on_page(m_pcur));

//     save_level_one_progress();

//     // OLEGS: fix this
//     /* Yield so that another thread can proceed. */
//     std::this_thread::yield();

//     err = restore_level_one_progress();
//   } else {
//     err = move_to_user_rec_on_next_page();
//   }

//   int n_retries [[maybe_unused]] = 0;
//   while (err == DB_LOCK_NOWAIT) {
//     /* We should restore the cursor from index root page,
//     to avoid deadlock opportunity. */

//     save_level_one_progress();

//     /* Forces to restore from index root. */
//     m_pcur->m_block_when_stored.clear();

//     err = restore_level_one_progress();

//     n_retries++;
//     ut_ad(n_retries < 10);
//   }

//   return err;
// }

dberr_t PCursor::move_to_next_block(dict_index_t *index)
{
  ut_ad(btr_pcur_is_after_last_on_page(m_pcur));
  ut_ad(m_read_level == 0);
  return move_to_next_block_at_leaf_level(index);

  // OLEGS:
  //ut_ad(m_read_level == 1);
  //return move_to_next_block_at_level_one(index);
}

bool Parallel_reader::Scan_ctx::check_visibility(const rec_t *&rec,
                                                 rec_offs *&offsets,
                                                 mem_heap_t *&heap,
                                                 mtr_t *mtr)
{
  // OLEGS: todo

  // const auto table_name = m_config.m_index->table->name;

  // ut_ad(!m_trx || m_trx->read_view == nullptr ||
  //       MVCC::is_view_active(m_trx->read_view));

  // if (!m_trx) {
  //   /* Do nothing */
  // } else if (m_trx->read_view != nullptr) {
  //   auto view = m_trx->read_view;

  //   if (m_config.m_index->is_clustered()) {
  //     trx_id_t rec_trx_id;

  //     if (m_config.m_index->trx_id_offset > 0) {
  //       rec_trx_id = trx_read_trx_id(rec + m_config.m_index->trx_id_offset);
  //     } else {
  //       rec_trx_id = row_get_rec_trx_id(rec, m_config.m_index, offsets);
  //     }

  //     if (m_trx->isolation_level > TRX_ISO_READ_UNCOMMITTED &&
  //         !view->changes_visible(rec_trx_id, table_name)) {
  //       rec_t *old_vers;

  //       row_vers_build_for_consistent_read(rec, mtr, m_config.m_index, &offsets,
  //                                          view, &heap, heap, &old_vers,
  //                                          nullptr, nullptr);

  //       rec = old_vers;

  //       if (rec == nullptr) {
  //         return (false);
  //       }
  //     }
  //   } else {
  //     /* Secondary index scan not supported yet. */
  //     ut_error;
  //   }
  // }

  // if (rec_get_deleted_flag(rec, m_config.m_is_compact)) {
  //   /* This record was deleted in the latest committed version, or it was
  //   deleted and then reinserted-by-update before purge kicked in. Skip it. */
  //   return (false);
  // }

  // ut_ad(!m_trx || m_trx->isolation_level == TRX_ISO_READ_UNCOMMITTED ||
  //       !rec_offs_any_null_extern(m_config.m_index, rec, offsets));

  return (true);
}

void Parallel_reader::Scan_ctx::copy_row(const rec_t *rec, Iter *iter) const
{
  // OLEGS: old (MySQL)  version:

  // auto index= m_config.m_index;
  // iter->m_offsets =
  //     rec_get_offsets(rec, index, iter->m_offsets, index->n_core_fields,
  //                     ULINT_UNDEFINED, &iter->m_heap);

  // /* Copy the row from the page to the scan iterator. The copy should use
  // memory from the iterator heap because the scan iterator owns the copy. */
  // auto rec_len = rec_offs_size(iter->m_offsets);

  // auto copy_rec = static_cast<rec_t *>(mem_heap_alloc(iter->m_heap, rec_len));

  // memcpy(copy_rec, rec, rec_len);

  // iter->m_rec = copy_rec;

  // auto tuple = row_rec_to_index_entry_low(iter->m_rec, m_config.m_index,
  //                                         iter->m_offsets, iter->m_heap);

  // ut_ad(dtuple_validate(tuple));

  // /* We have copied the entire record but we only need to compare the
  // key columns when we check for boundary conditions. */
  // const auto n_compare = dict_index_get_n_unique_in_tree(m_config.m_index);

  // dtuple_set_n_fields_cmp(tuple, n_compare);

  // iter->m_tuple = tuple;

  // OLEGS: new version, copies only key fields rather than full tuple

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

std::shared_ptr<Parallel_reader::Scan_ctx::Iter>
Parallel_reader::Scan_ctx::create_persistent_cursor(
    const page_cur_t &page_cursor, mtr_t *mtr) const
{
  ut_a(m_config.m_read_level == 0);
  ut_ad(index_s_own());

  std::shared_ptr<Iter> iter = std::make_shared<Iter>();

  iter->m_heap = mem_heap_create(sizeof(btr_pcur_t) + (srv_page_size / 16));

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

  void *ptr = mem_heap_alloc(iter->m_heap, sizeof(btr_pcur_t));

  ::new (ptr) btr_pcur_t();

  iter->m_pcur = reinterpret_cast<btr_pcur_t *>(ptr);

  //iter->m_pcur->init(m_config.m_read_level);
  btr_pcur_init(iter->m_pcur);

  /* Make a copy of the rec. */
  copy_row(rec, iter.get());

  // iter->m_pcur->open_on_user_rec(page_cursor, PAGE_CUR_GE,
  //                                BTR_ALREADY_S_LATCHED | BTR_SEARCH_LEAF);
  btr_pcur_open_on_user_rec(iter->m_pcur, page_cursor, PAGE_CUR_GE,
                            BTR_SEARCH_LEAF_ALREADY_S_LATCHED);

  ut_ad(btr_page_get_level(buf_block_get_frame(btr_pcur_get_block(iter->m_pcur))) ==
        m_config.m_read_level);

  btr_pcur_store_position(iter->m_pcur, mtr);

  return (iter);
}

bool Parallel_reader::Ctx::move_to_next_node(PCursor *pcursor)
{
  // ut_d(auto cur = m_range.first->m_pcur->get_page_cur());
  ut_d(auto cur = btr_pcur_get_page_cur(m_range.first->m_pcur));

  auto err = pcursor->move_to_next_block(const_cast<dict_index_t *>(index()));

  if (err != DB_SUCCESS)
  {
    ut_a(err == DB_END_OF_INDEX);
    return false;
  }
  else
  {
    /* Page can't be empty unless it is a root page. */
    ut_ad(!page_cur_is_after_last(cur));
    ut_ad(!page_cur_is_before_first(cur));
    return true;
  }
}

dberr_t Parallel_reader::Ctx::traverse()
{
  /* Take index lock if the requested read level is on a non-leaf level as the
  index lock is required to access non-leaf page.  */
  
  // OLEGS: looks like m_read_level != 0 is only valid for histogram sampling,
  // can be removed
  ut_a(m_scan_ctx->m_config.m_read_level == 0);

  mtr_t mtr(m_scan_ctx->m_trx);
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  auto &from = m_range.first;

  PCursor pcursor(from->m_pcur, &mtr, m_scan_ctx->m_config.m_read_level);
  if (!pcursor.restore_to_largest_le_position_saved()) {
    /* Move to the next record on the page */
    [[maybe_unused]] rec_t *next= page_cur_move_to_next(pcursor.get_page_cursor());
    // OLEGS: check next == nullptr
  }

  dberr_t err{DB_SUCCESS};

  m_thread_ctx->m_pcursor = &pcursor;

  err = traverse_recs(&pcursor, &mtr);

  if (mtr.is_active()) {
    mtr.commit();
  }

  m_thread_ctx->m_pcursor = nullptr;

  if (m_scan_ctx->m_config.m_read_level != 0) {
    m_scan_ctx->index_s_unlock();
  }

  return (err);
}

dberr_t Parallel_reader::Ctx::traverse_recs(PCursor *pcursor, mtr_t *mtr)
{
  const auto &end_tuple = m_range.second->m_tuple;
  auto heap = mem_heap_create(srv_page_size / 4);
  auto index = m_scan_ctx->m_config.m_index;

  m_start = true;

  dberr_t err{DB_SUCCESS};

  if (m_scan_ctx->m_reader->m_start_callback)
  {
    /* Page start. */
    m_thread_ctx->m_state = State::PAGE;
    err = m_scan_ctx->m_reader->m_start_callback(m_thread_ctx);
  }

  bool call_end_page{true};
  auto cur = pcursor->get_page_cursor();

  while (err == DB_SUCCESS)
  {
    if (page_cur_is_after_last(cur))
    {
      call_end_page = false;

      if (m_scan_ctx->m_reader->m_finish_callback) {
        /* End of page. */
        m_thread_ctx->m_state = State::PAGE;
        err = m_scan_ctx->m_reader->m_finish_callback(m_thread_ctx);
        if (err != DB_SUCCESS) {
          break;
        }
      }

      mem_heap_empty(heap);

      if (!(m_n_pages % TRX_IS_INTERRUPTED_PROBE) &&
          trx_is_interrupted(trx())) {
        err = DB_INTERRUPTED;
        break;
      }

      if (is_error_set()) {
        break;
      }

      /* Note: The page end callback (above) can save and restore the cursor.
      The restore can end up in the middle of a page. */
      if (pcursor->is_after_last_on_page() && !move_to_next_node(pcursor)) {
        break;
      }

      ++m_n_pages;
      m_first_rec = true;

      call_end_page = true;

      if (m_scan_ctx->m_reader->m_start_callback) {
        /* Page start. */
        m_thread_ctx->m_state = State::PAGE;
        err = m_scan_ctx->m_reader->m_start_callback(m_thread_ctx);
        if (err != DB_SUCCESS) {
          break;
        }
      }
    }

    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs *offsets = offsets_;

    rec_offs_init(offsets_);

    const rec_t *rec = page_cur_get_rec(cur);
    offsets = rec_get_offsets(rec, index, offsets, index->n_core_fields,
                              ULINT_UNDEFINED, &heap);

    if (end_tuple != nullptr)
    {
      ut_ad(rec != nullptr);

      /* Key value of a record can change only if the record is deleted or if
      it's updated. An update is essentially a delete + insert. So in both
      the cases we just delete mark the record and the original key value is
      preserved on the page.

      Since the range creation is based on the key values and the key value do
      not ever change the, latest (non-MVCC) version of the record should always
      tell us correctly whether we're within the range or outside of it. */
      auto ret = cmp_dtuple_rec(end_tuple, rec, index, offsets);

      /* Note: The range creation doesn't use MVCC. Therefore it's possible
      that the range boundary entry could have been deleted. */
      if (ret <= 0)
      {
        break;
      }
    }

    bool skip = !m_scan_ctx->check_visibility(rec, offsets, heap, mtr);
    
    if (!skip) {
      m_rec = rec;
      m_offsets = offsets;
      m_block = cur->block;

      err = m_scan_ctx->m_f(this);

      if (err != DB_SUCCESS) {
        break;
      }

      m_start = false;
    }

    m_first_rec = false;

    // OLEGS: check for error (nullptr - DB_CORRUPTION)
    [[maybe_unused]] rec_t *next= page_cur_move_to_next(cur);
  }

  if (err != DB_SUCCESS) {
    m_scan_ctx->set_error_state(err);
  }

  mem_heap_free(heap);

  if (call_end_page && m_scan_ctx->m_reader->m_finish_callback) {
    /* Page finished. */
    m_thread_ctx->m_state = State::PAGE;
    auto cb_err = m_scan_ctx->m_reader->m_finish_callback(m_thread_ctx);

    if (cb_err != DB_SUCCESS && !m_scan_ctx->is_error_set()) {
      err = cb_err;
    }
  }

  return (err);
}

void Parallel_reader::enqueue(std::shared_ptr<Ctx> ctx)
{
  mysql_mutex_lock(&m_mutex);
  m_ctxs.push_back(ctx);
  mysql_mutex_unlock(&m_mutex);
}

std::shared_ptr<Parallel_reader::Ctx> Parallel_reader::dequeue()
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

bool Parallel_reader::is_queue_empty() const
{
  mysql_mutex_lock(&m_mutex);
  auto empty = m_ctxs.empty();
  mysql_mutex_unlock(&m_mutex);
  return (empty);
}

void Parallel_reader::worker(Parallel_reader::Thread_ctx *thread_ctx)
{
  dberr_t err{DB_SUCCESS};
  dberr_t cb_err{DB_SUCCESS};

  if (m_start_callback) {
    /* Thread start. */
    thread_ctx->m_state = State::THREAD;
    cb_err = m_start_callback(thread_ctx);

    if (cb_err != DB_SUCCESS) {
      err = cb_err;
      set_error_state(cb_err);
    }
  }

  /* Wait for all the threads to be spawned as it's possible that we could
  abort the operation if there are not enough resources to spawn all the
  threads. */

  // OLEGS: todo
  // if (!m_sync) {
  //   os_event_wait_time_low(m_event, std::chrono::microseconds::max(),
  //                          m_sig_count);
  // }

  for (;;) {
    size_t n_completed{};
    // OLEGS: todo
    // int64_t sig_count = os_event_reset(m_event);

    while (err == DB_SUCCESS && cb_err == DB_SUCCESS && !is_error_set()) {
      auto ctx = dequeue();

      if (ctx == nullptr) {
        break;
      }

      auto scan_ctx = ctx->m_scan_ctx;

      if (scan_ctx->is_error_set()) {
        break;
      }

      ctx->m_thread_ctx = thread_ctx;

      if (ctx->m_split)
      {
        err = ctx->split();
        /* Tell the other threads that there is work to do. */

        // OLEGS: todo
        //os_event_set(m_event);
      }
      else
      {
        if (m_start_callback) {
          /* Context start. */
          thread_ctx->m_state = State::CTX;
          cb_err = m_start_callback(thread_ctx);
        }

        if (cb_err == DB_SUCCESS && err == DB_SUCCESS) {
          err = ctx->traverse();
        }

        if (m_finish_callback) {
          /* Context finished. */
          thread_ctx->m_state = State::CTX;
          cb_err = m_finish_callback(thread_ctx);
        }
      }

      /* Check for trx interrupted (useful in the case of small tables). */
      if (err == DB_SUCCESS && trx_is_interrupted(ctx->trx())) {
        err = DB_INTERRUPTED;
        scan_ctx->set_error_state(err);
        break;
      }

      ut_ad(err == DB_SUCCESS || scan_ctx->is_error_set());

      ++n_completed;
    }

    if (cb_err != DB_SUCCESS || err != DB_SUCCESS || is_error_set()) {
      break;
    }

    m_n_completed.fetch_add(n_completed, std::memory_order_relaxed);

    if (m_n_completed == m_ctx_id) {
      /* Wakeup other worker threads before exiting */

      // OLEGS: todo
      //os_event_set(m_event);
      break;
    }

    if (!m_sync) {
      // OLEGS: todo
      // os_event_wait_time_low(m_event, std::chrono::microseconds::max(),
      //                       sig_count);
    }
  }

  if (err != DB_SUCCESS && !is_error_set()) {
    /* Set the "global" error state. */
    set_error_state(err);
  }

  if (is_error_set()) {
    /* Wake up any sleeping threads. */

    // OLEGS: todo
    // os_event_set(m_event);
  }

  if (m_finish_callback) {
    /* Thread finished. */
    thread_ctx->m_state = State::THREAD;
    cb_err = m_finish_callback(thread_ctx);

    /* Keep the err status from previous failed operations */
    if (cb_err != DB_SUCCESS) {
      err = cb_err;
      set_error_state(cb_err);
    }
  }

  ut_a(is_error_set() || (m_n_completed == m_ctx_id && is_queue_empty()));
}

dberr_t Parallel_reader::init_worker(Worker_ctx *wctx)
{
  // OLEGS: todo
  return DB_SUCCESS;
}

std::shared_ptr<Parallel_reader::Ctx> Parallel_reader::get_job_for_worker(Worker_ctx *worker_ctx)
{
  std::shared_ptr<Parallel_reader::Ctx> ctx = dequeue();
  return ctx;
}

page_no_t Parallel_reader::Scan_ctx::search(buf_block_t *block,
                                            const dtuple_t *key) const
{
  ut_ad(index_s_own());

  page_cur_t page_cursor;
  const auto index = m_config.m_index;

  if (key != nullptr)
  {
    auto err = pread_page_cur_search(block, index, key, PAGE_CUR_LE, &page_cursor);
    if (err != DB_SUCCESS)
    {
      // OLEGS: propagate error
    }
  }
  else
  {
    page_cur_set_before_first(block, &page_cursor);
  }

  if (page_rec_is_infimum(page_cur_get_rec(&page_cursor)))
  {
    // OLEGS: check for error (nullptr - DB_CORRUPTION)
    [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
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

page_cur_t Parallel_reader::Scan_ctx::start_range(
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

    if (block == nullptr) {
      /* Page fetch failed — typically tablespace corruption. */
      *err = DB_CORRUPTION;
      return page_cur_t{};
    }

    height = btr_page_get_level(buf_block_get_frame(block));

    savepoints.push_back({savepoint, block});

    if (height != 0 && height != m_config.m_read_level)
    {
      page_id.set_page_no(search(block, key));
      continue;
    }

    page_cur_t page_cursor;

    if (key != nullptr) {
      auto err = pread_page_cur_search(block, index, key, PAGE_CUR_GE, &page_cursor);
      if (err != DB_SUCCESS)
      {
        // OLEGS: propagate error
      }
    }
    else
    {
      page_cur_set_before_first(block, &page_cursor);
    }

    if (page_rec_is_infimum(page_cur_get_rec(&page_cursor)))
    {
      // OLEGS: check for error (nullptr - DB_CORRUPTION)
      [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
    }

    return (page_cursor);
  }
}

void Parallel_reader::Scan_ctx::create_range(Ranges &ranges,
                                             page_cur_t &leaf_page_cursor,
                                             mtr_t *mtr) const {
  leaf_page_cursor.index = m_config.m_index;

  auto iter = create_persistent_cursor(leaf_page_cursor, mtr);

  /* Setup the previous range (next) to point to the current range. */
  if (!ranges.empty()) {
    ut_a(ranges.back().second->m_heap == nullptr);
    ranges.back().second = iter;
  }

  ranges.push_back(Range(iter, std::make_shared<Iter>()));
}

dberr_t Parallel_reader::Scan_ctx::create_ranges(const Scan_range &scan_range,
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

  if (block == nullptr) {
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
    auto err = pread_page_cur_search(block, index, start,
                                     PAGE_CUR_GE, &page_cursor);
    if (err != DB_SUCCESS)
    {
      return err;
    }

    if (page_cur_is_after_last(&page_cursor))
    {
      return (DB_SUCCESS);
    }
    else if (page_cur_is_before_first((&page_cursor)))
    {
      // OLEGS: check for error (nullptr - DB_CORRUPTION)
      [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
    }
  }
  else
  {
    page_cur_set_before_first(block, &page_cursor);
    /* Skip the infimum record. */

    // OLEGS: check for error (nullptr - DB_CORRUPTION)
    [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
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

    if (end != nullptr && cmp_dtuple_rec(end, rec, index, offsets) <= 0)
      break;

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

        // OLEGS: check for error (nullptr - DB_CORRUPTION)
        [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
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
        // OLEGS: check for return status
        pread_page_cur_search(block, index, start, PAGE_CUR_GE, &page_cursor);
        ut_a(!page_rec_is_infimum(page_cur_get_rec(&page_cursor)));
      }
      else
      {
        page_cur_set_before_first(block, &page_cursor);

        /* Skip the infimum record. */
        // OLEGS: check for error (nullptr - DB_CORRUPTION)
        [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
        ut_a(!page_cur_is_after_last(&page_cursor));
      }

      /* Since we are already at the requested level use the current page
       cursor. */
      memcpy(&level_page_cursor, &page_cursor, sizeof(level_page_cursor));
    }

    if (!page_rec_is_supremum(page_cur_get_rec(&level_page_cursor)))
    {
      create_range(ranges, level_page_cursor, mtr);
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

    // OLEGS: check for nullptr (corrupted page)
    [[maybe_unused]] rec_t *next= page_cur_move_to_next(&page_cursor);
  }

  savepoints.push_back(savepoint);

  for (auto &savepoint : savepoints)
    mtr->release(*savepoint.second);

  if (heap != nullptr) {
    mem_heap_free(heap);
  }

  return (DB_SUCCESS);
}

dberr_t Parallel_reader::Scan_ctx::partition(
    const Scan_range &scan_range, Parallel_reader::Scan_ctx::Ranges &ranges,
    size_t split_level)
{
  ut_ad(index_s_own());

  mtr_t mtr(m_trx);
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  dberr_t err{DB_SUCCESS};

  err = create_ranges(scan_range, m_config.m_index->page, 0, split_level,
                      ranges, &mtr);

  if (err == DB_SUCCESS && scan_range.m_end != nullptr && !ranges.empty()) {
    auto &iter = ranges.back().second;

    ut_a(iter->m_heap == nullptr);

    iter->m_heap = mem_heap_create(sizeof(btr_pcur_t) + (srv_page_size / 16));

    iter->m_tuple = dtuple_copy(scan_range.m_end, iter->m_heap);

    /* Do a deep copy. */
    for (size_t i = 0; i < dtuple_get_n_fields(iter->m_tuple); ++i) {
      dfield_dup(&iter->m_tuple->fields[i], iter->m_heap);
    }
  }

  mtr.commit();
  return (err);
}

dberr_t Parallel_reader::Scan_ctx::create_context(const Range &range,
                                                  bool split)
{
  auto ctx_id = m_reader->m_ctx_id.fetch_add(1, std::memory_order_relaxed);
  
  auto ctx = std::shared_ptr<Ctx>(
    UT_NEW_NOKEY(Ctx(ctx_id, this, range)),
    [](Ctx *ctx) { UT_DELETE(ctx); });

  dberr_t err{DB_SUCCESS};

  if (ctx.get() == nullptr)
  {
    m_reader->m_ctx_id.fetch_sub(1, std::memory_order_relaxed);
    return (DB_OUT_OF_MEMORY);
  }
  else
  {
    ctx->m_split = split;
    m_reader->enqueue(ctx);
  }

  return (err);
}

dberr_t Parallel_reader::Scan_ctx::create_contexts(const Ranges &ranges)
{
  size_t split_point{};

  {
    const auto n = std::max(max_threads(), size_t{1});

    ut_a(n <= Parallel_reader::MAX_TOTAL_THREADS);

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
    auto err = create_context(range, i >= split_point);

    if (err != DB_SUCCESS) {
      return (err);
    }

    ++i;
  }

  return DB_SUCCESS;
}

void Parallel_reader::parallel_read() {
  if (m_ctxs.empty())
  {
    return;
  }

  if (m_sync)
  {
    Thread_ctx *ptr = UT_NEW_NOKEY(Thread_ctx(0));

    if (ptr == nullptr)
    {
      set_error_state(DB_OUT_OF_MEMORY);
      return;
    }

    m_thread_ctxs.push_back(ptr);

    /* Set event to indicate to ::worker() that no threads will be spawned. */
    // OLEGS: os_event_set(m_event);

    worker(m_thread_ctxs[0]);

    return;
  }

  ut_a(m_n_threads > 0);

  m_thread_ctxs.reserve(m_n_threads);

  dberr_t err{DB_SUCCESS};

  for (size_t i = 0; i < m_n_threads; ++i) {
    try {
      // OLEGS: moved to Parallel_reader::initialize
      // Thread_ctx *ptr = UT_NEW_NOKEY(Thread_ctx(i));
      // if (ptr == nullptr)
      // {
      //   set_error_state(DB_OUT_OF_MEMORY);
      //   return;
      // }
      // m_thread_ctxs.emplace_back(ptr);

      // OLEGS:
      // m_parallel_read_threads.emplace_back(
      //     os_thread_create(parallel_read_thread_key, i + 1,
      //                      &Parallel_reader::worker, this, m_thread_ctxs[i]));
      // m_parallel_read_threads.back().start();
    }
    catch (...)
    {
      err = DB_OUT_OF_MEMORY;
      /* Set the global error state to tell the worker threads to exit. */
      set_error_state(err);
      break;
    }
  }

  DEBUG_SYNC_C("parallel_read_wait_for_kill_query");

  // OLEGS:
  // DBUG_EXECUTE_IF(
  //     "innodb_pread_thread_OOR", if (!m_sync) {
  //       err = DB_OUT_OF_RESOURCES;
  //       set_error_state(err);
  //     });

  // OLEGS: os_event_set(m_event);

  // DBUG_EXECUTE_IF("bug28079850", set_error_state(DB_INTERRUPTED););
}

dberr_t Parallel_reader::spawn(size_t n_threads) noexcept {
  /* In case this is a retry after a DB_OUT_OF_RESOURCES error. */
  m_err = DB_SUCCESS;

  m_n_threads = n_threads;

  // OLEGS:
  // if (max_threads() > m_n_threads) {
  //   release_unused_threads(max_threads() - m_n_threads);
  // }

  parallel_read();

  return DB_SUCCESS;
}

dberr_t Parallel_reader::run(size_t n_threads) {
  /* In case this is a retry after a DB_OUT_OF_RESOURCES error. */
  m_err = DB_SUCCESS;

  ut_a(max_threads() >= n_threads);

  if (n_threads == 0) {
    m_sync = true;
  }

  const auto err = spawn(n_threads);

  /* Don't wait for the threads to finish if the read is not synchronous or
  if there's no parallel read. */
  if (m_sync)
  {
    if (err != DB_SUCCESS) {
      return err;
    }
    ut_a(m_n_threads == 0);
    return is_error_set() ? m_err.load() : DB_SUCCESS;
  }
  else
  {
    // OLEGS:
    //join();

    if (err != DB_SUCCESS) {
      return err;
    } else if (is_error_set()) {
      return m_err;
    }

    for (auto &scan_ctx : m_scan_ctxs) {
      if (scan_ctx->is_error_set()) {
        /* Return the state of the first Scan context that is in state ERROR. */
        return scan_ctx->m_err;
      }
    }
  }

  return DB_SUCCESS;
}

dberr_t Parallel_reader::add_scan(trx_t *trx,
                                  const Parallel_reader::Config &config,
                                  Parallel_reader::F &&f) {
  auto scan_ctx = std::shared_ptr<Scan_ctx>(
    UT_NEW_NOKEY(Scan_ctx(this, m_scan_ctx_id, trx, config, std::move(f))),
    [](Scan_ctx *scan_ctx) { UT_DELETE(scan_ctx); });

  if (scan_ctx.get() == nullptr)
  {
    ib::error() << "Out of memory";
    return (DB_OUT_OF_MEMORY);
  }

  m_scan_ctxs.push_back(scan_ctx);

  ++m_scan_ctx_id;

  scan_ctx->index_s_lock();

  Parallel_reader::Scan_ctx::Ranges ranges{};
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

int Parallel_reader::initialize(size_t n_threads)
{
  DBUG_ASSERT(!is_initialized);
  m_max_threads= m_n_threads= n_threads;
  m_n_completed= 0;

  int err= mysql_mutex_init(PSI_NOT_INSTRUMENTED, &m_mutex, nullptr);
  if (err != 0)
    return err;

  m_worker_ctxs.reserve(n_threads);
  m_thread_ctxs.reserve(n_threads);
  for (size_t i= 0; i < n_threads; ++i)
  {
    // OLEGS: is it possible to combine contexts?
    Worker_ctx *wctx= UT_NEW_NOKEY(Worker_ctx(i, this));
    Thread_ctx *thr = UT_NEW_NOKEY(Thread_ctx(i)); // OLEGS: remove those if not used
    if (wctx == nullptr || thr == nullptr)
    {
      for (auto *p : m_worker_ctxs) UT_DELETE(p);
      m_worker_ctxs.clear();
      for (auto *p : m_thread_ctxs) UT_DELETE(p);
      m_thread_ctxs.clear();
      mysql_mutex_destroy(&m_mutex);
      return HA_ERR_OUT_OF_MEM;
    }
    m_worker_ctxs.push_back(wctx);
    m_thread_ctxs.push_back(thr);
  }

  is_initialized= true;
  return 0;
}

Parallel_reader::Worker_ctx *
Parallel_reader::get_worker_ctx(size_t worker_idx) const
{
  ut_a(worker_idx < m_worker_ctxs.size());
  return m_worker_ctxs[worker_idx];
}

  void Parallel_reader::cleanup()
  {
    if (!is_initialized) return;

    for (auto *p : m_worker_ctxs) UT_DELETE(p);
    m_worker_ctxs.clear();

    m_ctxs.clear();
    m_scan_ctxs.clear();
    m_scan_ctx_id = 0;
    m_ctx_id.store(0, std::memory_order_relaxed);
    m_n_completed.store(0, std::memory_order_relaxed);
    m_n_threads = 0;
    m_max_threads = 0;
    m_sig_count = 0;

    m_err.store(DB_SUCCESS, std::memory_order_relaxed);

    m_start_callback = Start{};
    m_finish_callback = Finish{};
    mysql_mutex_destroy(&m_mutex);

    is_initialized= false;
  }