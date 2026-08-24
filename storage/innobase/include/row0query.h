/*****************************************************************************

Copyright (c) 2025, MariaDB PLC.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/row0query.h
General Query Executor

Created 2025/10/30
*******************************************************/

#pragma once

#include "btr0pcur.h"
#include <functional>
#include "dict0types.h"
#include "data0types.h"
#include "db0err.h"
#include "lock0types.h"
#include "rem0rec.h"

struct row_prebuilt_t;

/** Comparator action for deciding how to treat a record */
enum class RecordCompareAction
{
  /** Process this record via process_record */
  PROCESS,
  /** Do not process this record, continue traversal */
  SKIP,
  /** Stop traversal immediately */
  STOP
};

using RecordProcessor= std::function<dberr_t(
  const rec_t*, const dict_index_t*, const rec_offs*)>;

using RecordComparator= std::function<
  RecordCompareAction(const dtuple_t*, const rec_t*,
                      const dict_index_t*, const rec_offs*)>;

/** Record processing callback interface using std::function.
Can be used by FTS, stats infrastructure, and other components
that need to process database records with custom logic.
What this replaces, in SQL terms:
The old internal-parser path expressed each scan as
SELECT <columns> FROM <table> WHERE <comparator predicate>
and ran a graph-node FETCH that invoked a row-consumer callback for
every visible row. RecordCallback collapses both pieces into one
C++ object:
- compare_record() is the WHERE predicate, returning
PROCESS / SKIP / STOP for the current candidate.
- process_record() is the row consumer.
The B-tree traversal that the old graph runner drove is now driven
by row_search_mvcc_callback() directly, with no SQL string parsing
and no graph allocation per call. */
class RecordCallback
{
public:
  /** Constructor with processor function and optional comparator
  @param processor  Function to process each record
  @param comparator Optional function to filter records */
  RecordCallback(
    RecordProcessor processor,
    RecordComparator comparator= nullptr)
    : process_record(processor), compare_record(comparator) {}

  virtual ~RecordCallback()= default;

  /** Called for each matching record */
  const RecordProcessor process_record;

  /** Comparison function for custom filtering */
  const RecordComparator compare_record;
};

/** General-purpose MVCC-aware record traversal and basic
DML executor. Provides a thin abstraction over B-tree cursors for
reading and mutating records with consistent-read (MVCC) handling,
and callback API.
- Open and iterate clustered/secondary indexes with page cursors.
- Build consistent-read versions when needed via transaction
read views.
- Filter and process records using RecordCallback:
  - compare_record: decide SKIP/PROCESS/STOP for each record
  - process_record: handle visible records; return DB_SUCCESS to continue, DB_SUCCESS_LOCKED_REC to stop
- Basic DML helpers (insert/delete/replace) and table locking.

m_mtr ownership rule:
  - Positioning operations (select_for_update, update_record,
    delete_record, delete_all) own m_mtr.
  - On any error return, the callee commits m_mtr so it never
    leaks across a failure boundary.
  - On DB_SUCCESS from select_for_update(), the caller must
    follow up with update_record()+commit_mtr() or commit_mtr().
  - Non-positioning DML (insert_record) must NOT be interleaved
    with an open m_mtr.

DB_LOCK_WAIT contract:
  No public QueryExecutor method ever returns DB_LOCK_WAIT.
  Internally there are two flavours of leaf:
    - Self-draining leaves (row_search_mvcc - via
      row_mysql_handle_errors which calls lock_wait()) surface
      either success or the post-wait failure code
      (DB_LOCK_WAIT_TIMEOUT, DB_DEADLOCK, DB_INTERRUPTED).
      Wrappers built on these (select_for_update, read*,
      delete_record, replace_record) just restart on a
      DB_LOCK_WAIT echo.
    - Raw leaves (::lock_table, lock_clust_rec_read_check_and_lock,
      row_ins_clust_index_entry) return DB_LOCK_WAIT with
      trx->lock.wait_lock still set, and require an explicit
      handle_wait() drain before retrying.  Used by lock_table(),
      delete_all() and insert_record().
      (row_ins_clust_index_entry self-drains only when an FK check
      triggers do_possible_lock_wait, which is not on the FTS aux
      path.)
  Either way, callers see only DB_SUCCESS or a terminal error. */
class QueryExecutor
{
private:
  que_thr_t *m_thr;
  btr_pcur_t m_pcur;
  btr_pcur_t *m_clust_pcur;
  trx_t *m_trx;
  mtr_t *m_mtr;
  mem_heap_t *m_heap;

  /* Prebuilt for row_search_mvcc call */
  row_prebuilt_t *m_prebuilt;

  /* Setup prebuilt structure for row_search_mvcc usage
  @param table table to search
  @param index index to use
  @param tuple search tuple
  @param mode  search mode */
  void setup_prebuilt(dict_table_t *table, dict_index_t *index,
                      const dtuple_t *tuple, page_cur_mode_t mode) noexcept;

public:
  QueryExecutor(trx_t *trx);
  ~QueryExecutor();

  /** Insert a record in clustered index of the table
  @param table table to be inserted
  @param tuple tuple to be inserted
  @return DB_SUCCESS on success, error code on failure */
  dberr_t insert_record(dict_table_t *table, dtuple_t *tuple) noexcept;

  /** Delete a record from the clustered index of the table
  @param table table to be inserted
  @param tuple tuple to be inserted
  @return DB_SUCCESS on success, error code on failure */
  dberr_t delete_record(dict_table_t *table, dtuple_t *tuple) noexcept;

  /** Delete all records from the clustered index of the table
  @param table table be be deleted
  @return DB_SUCCESS on success, error code on failure */
  dberr_t delete_all(dict_table_t *table) noexcept;

  /** Acquire and lock a single clustered record for update
  Performs a keyed lookup on the clustered index, validates MVCC visibility,
  and acquires an X lock on the matching record.
  @param[in]  table         Table containing the record
  @param[in]  search_tuple  Exact key for clustered index lookup
  @param[in]  callback      Optional record callback
  @return DB_SUCCESS on successful lock
  DB_RECORD_NOT_FOUND if no visible matching record
  DB_LOCK_WAIT if waiting was required
  error code on failure */
  dberr_t select_for_update(dict_table_t *table, dtuple_t *search_tuple,
                            RecordCallback *callback= nullptr) noexcept;

  /** Update the currently selected clustered record within an active mtr.
  Attempts in-place update; falls back to optimistic/pessimistic update if needed,
  including external field storage when required.
  select_for_update() has positioned and locked m_pcur on the target row.

  The caller is responsible for committing or rolling back m_mtr after this call
  @param[in] table   target table
  @param[in] update  update descriptor (fields, new values)
  @return DB_SUCCESS on success
  DB_OVERFLOW/DB_UNDERFLOW during size-changing paths
  error_code on failures */
  dberr_t update_record(dict_table_t *table, const upd_t *update) noexcept;


  /** Try to update a record by key or insert if not found.
  Performs a SELECT ... FOR UPDATE using search_tuple;
  if found, updates the row; otherwise inserts a new record.
  Note:
    On update path, commits or rolls back the active mtr as needed.
    On insert path, no active mtr remains upon return
  @param[in] table         target table
  @param[in] search_tuple  key identifying the target row
  @param[in] update        update descriptor (applied when found)
  @param[in] insert_tuple  tuple to insert when not found
  @return DB_SUCCESS on successful update or insert
  @retval DB_LOCK_WAIT to be retried,
  @return error code on failure */
  dberr_t replace_record(dict_table_t *table, dtuple_t *search_tuple,
                         const upd_t *update, dtuple_t *insert_tuple) noexcept;

  /** Iterate clustered index records and process via callback.
  Handles full table scan and index scan for range/select queries
  Calls callback.compare_record() to decide SKIP/PROCESS/STOP for
  each matching record. On PROCESS, invokes
  callback.process_record() on an MVCC-visible version.
  @param      table    table to read
  @param      tuple    optional search key (range/point). nullptr => full scan
  @param      mode     B-tree search mode (e.g., PAGE_CUR_GE)
  @param      callback  record comparator/processor
  @return DB_SUCCESS if at least one record was processed
  @retval DB_RECORD_NOT_FOUND if no record matched
  @return error code on failure */
  dberr_t read(dict_table_t *table, const dtuple_t *tuple,
               page_cur_mode_t mode,
               RecordCallback& callback) noexcept;

  /** Read records via a secondary index and process corresponding
  clustered rows. Performs a range or point scan on the given secondary index,
  filters secondary records with callback.compare_record(), then looks up
  the matching clustered record and invokes callback.process_record()
  on a MVCC-visible version.

  Loop termination when read_all=true is intentionally
  per-return-code:
  @retval DB_SUCCESS continue scanning the next record
  @retval DB_END_OF_INDEX normalized to DB_SUCCESS; scan done
  @retval DB_SUCCESS_LOCKED_REC callback's STOP signal; normalized
                                to DB_SUCCESS; scan done
  @retval DB_FTS_EXCEED_RESULT_CACHE_LIMIT propagated as-is so the
  INFORMATION_SCHEMA pagination path (i_s_fts_read_aux_index_words)
  can flush its accumulated batch and resume from the last word

  @param    table         Table to read
  @param    sec_index     Secondary index used for traversal
  @param    search_tuple  search key or nullptr for full scan
  @param    mode          Cursor search mode
  @param    callback      RecordCallback with comparator+processor
  @param    read_all      true to keep iterating until end/STOP/error
  @return DB_SUCCESS on success
  DB_RECORD_NOT_FOUND if no matching record was processed
  DB_FTS_EXCEED_RESULT_CACHE_LIMIT for i_s pagination
  error code on failure */
  dberr_t read_by_index(dict_table_t *table, dict_index_t *sec_index,
                        const dtuple_t *search_tuple,
                        page_cur_mode_t mode,
                        RecordCallback &callback,
			bool all_read) noexcept;

  /** Acquire a table lock in the given mode for transaction.
  @param table  table to lock
  @param mode   lock mode
  @return DB_SUCCESS, DB_LOCK_WAIT or error code */
  dberr_t lock_table(dict_table_t *table, lock_mode mode) noexcept;

  /** Handle a lock wait for the current transaction and thread context.
  @param  err         the lock-related error to handle (e.g., DB_LOCK_WAIT)
  @param  table_lock  true if the wait originated from table lock, else row lock
  @return DB_SUCCESS if the wait completed successfully and lock was granted
  @retval DB_LOCK_WAIT_TIMEOUT if timed out */
  dberr_t handle_wait(dberr_t err, bool table_lock) noexcept;
  mem_heap_t *get_heap() const { return m_heap; }
  trx_t *get_trx() const { return m_trx; }
  void commit_mtr() noexcept
  {
    if (m_mtr)
      m_mtr->commit();
  }
};
