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

/** Comparator action for deciding how to treat a record */
enum class RecordCompareAction
{
  /** Do not process this record, continue traversal */
  SKIP,
  /** Process this record via process_record */
  PROCESS,
  /** Stop traversal immediately */
  STOP
};

using RecordProcessor= std::function<bool(
  const rec_t*, const dict_index_t*, const rec_offs*)>;

using RecordComparator= std::function<
  RecordCompareAction(const dtuple_t*, const rec_t*,
                      const dict_index_t*, const rec_offs*)>;

/** Record processing callback interface using std::function.
Can be used by FTS, stats infrastructure, and other components
that need to process database records with custom logic. */
class RecordCallback
{
public:
  /** Constructor with processor function and optional comparator
  @param[in] processor    Function to process each record
  @param[in] comparator   Optional function to filter records (default: accept all) */
  RecordCallback(
    RecordProcessor processor,
    RecordComparator comparator= [](const dtuple_t*, const rec_t*,
                                    const dict_index_t*, const rec_offs*)
      { return RecordCompareAction::PROCESS; })
    : process_record(std::move(processor)),
      compare_record(std::move(comparator)) {}

  virtual ~RecordCallback() = default;

  /** Called for each matching record */
  RecordProcessor process_record;

  /** Comparison function for custom filtering */
  RecordComparator compare_record;
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
  - process_record: handle visible records; return whether to continue
- Basic DML helpers (insert/delete/replace) and table locking. */
class QueryExecutor
{
private:
  que_thr_t *m_thr;
  btr_pcur_t m_pcur;
  mtr_t m_mtr;
  mem_heap_t *m_heap;

  /** Lookup clustered index record from secondary index record
  @param     table              table containing the indexes
  @param     sec_index          secondary index
  @param     clust_index        clustered index
  @param     sec_rec            secondary index record
  @param     callback           callback to process the clustered record
  @param     match_count        counter for processed records
  @param     continue_processing whether to continue tree traversal
  @return error code from processing */
  dberr_t lookup_clustered_record(dict_table_t *table,
                                  dict_index_t *sec_index,
                                  dict_index_t *clust_index,
                                  const rec_t *sec_rec,
                                  RecordCallback &callback,
                                  ulint &match_count,
                                  bool &continue_processing) noexcept;

  /** Process a record with MVCC visibility checking and
  version building
  @param     table              table containing the record
  @param     index              index of the record
  @param     rec                record to process
  @param     offsets            record offsets
  @param     callback           callback to process the record
  @param     continue_processing whether to continue tree traversal
  @return error code from processing */
  dberr_t process_record_with_mvcc(dict_table_t *table,
                                   dict_index_t *index,
                                   const rec_t *rec,
                                   rec_offs *offsets,
                                   RecordCallback &callback,
                                   bool &continue_processing) noexcept;
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

  @param    table         Table to read
  @param    sec_index     Secondary index used for traversal
  @param    search_tuple  search key or nullptr for full scan
  @param    mode          Cursor search mode
  @param    callback      RecordCallback with comparator+processor
  @return DB_SUCCESS on success
  DB_RECORD_NOT_FOUND if no matching record was processed
  error code on failure */
  __attribute__((nonnull))
  dberr_t read_by_index(dict_table_t *table, dict_index_t *sec_index,
                        const dtuple_t *search_tuple,
                        page_cur_mode_t mode,
                        RecordCallback &callback) noexcept;

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
  trx_t *get_trx() const { return m_mtr.trx; }
  void commit_mtr() noexcept { m_mtr.commit(); }
};
