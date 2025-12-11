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
@file include/fts0exec.h
FTS Query Builder - Abstraction layer for FTS operations

Created 2025/10/30
*******************************************************/

#pragma once

#include "row0query.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "fts0opt.h"
#include "fts0ast.h"
#include <functional>

/** Structure to represent FTS auxiliary table data for insertion */
struct fts_aux_data_t
{
  /**
  CREATE TABLE $FTS_PREFIX_INDEX_[1-6](
        word            VARCHAR(FTS_MAX_WORD_LEN),
        first_doc_id    INT NOT NULL,
        last_doc_id     UNSIGNED NOT NULL,
        doc_count       UNSIGNED INT NOT NULL,
        ilist           VARBINARY NOT NULL,
        UNIQUE CLUSTERED INDEX ON (word, first_doc_id));
  */
  const char *word;
  ulint      word_len;
  doc_id_t   first_doc_id;
  doc_id_t   last_doc_id;
  uint32_t   doc_count;
  const byte *ilist;
  ulint      ilist_len;

  fts_aux_data_t(const char *w, ulint w_len)
    : word(w), word_len(w_len)
  {
    first_doc_id= last_doc_id= doc_count= 0;
    ilist= nullptr;
    ilist_len= 0;
  }

  fts_aux_data_t(const char* w, ulint w_len, doc_id_t first_id,
                 doc_id_t last_id, uint32_t d_count, const byte* il,
                 ulint il_len)
    : word(w), word_len(w_len), first_doc_id(first_id),
      last_doc_id(last_id), doc_count(d_count), ilist(il),
      ilist_len(il_len) {}
};

/** Abstraction over QueryExecutor for FTS auxiliary/common tables.
Handles table open/lock and provides typed helpers to insert,
delete and read records in FTS INDEX_1..INDEX_6
and common tables (DELETED, CONFIG, ...) */
class FTSQueryExecutor
{
private:
  QueryExecutor *m_executor;
  bool m_dict_locked;
  const dict_index_t *m_fts_index;
  const dict_table_t *m_fts_table;
  dict_table_t *m_aux_tables[FTS_NUM_AUX_INDEX];
  dict_table_t *m_common_tables[FTS_NUM_AUX_INDEX - 1];

  /** Table preparation methods */

  /** Open FTS INDEX_[1..6] table for the given auxiliary index.
  @return DB_SUCCESS or error code */
  dberr_t open_aux_table(uint8_t aux_index) noexcept;

  /** Open a common FTS table (DELETED, CONFIG, ...).
  @return DB_SUCCESS or error code */
  dberr_t open_common_table(const char *tbl_name) noexcept;

  /** Table lock operation */

  /** Acquire a lock on an opened INDEX_[1..6] table.
  Retries lock wait once via QueryExecutor.
  @return DB_SUCCESS or error code */
  dberr_t lock_aux_tables(uint8_t aux_index, lock_mode mode) noexcept;

  /** Acquire a lock on an opened common FTS table.
  Retries lock wait once via QueryExecutor.
  @return DB_SUCCESS or error code */
  dberr_t lock_common_tables(uint8_t index, lock_mode mode) noexcept;

public:
  /** Create executor bound to trx and FTS table/index.
  @param trx transaction (not owned)
  @param fts_index FTS index (for INDEX_[1..6] tables) or nullptr
  @param fts_table FTS table (for common tables) or nullptr
  @param dict_locked true if dict is already locked */
  FTSQueryExecutor(trx_t *trx, const dict_index_t *fts_index,
                   const dict_table_t *fts_table, bool dict_locked= false);

  /** Release any opened table handles and executor resources. */
  ~FTSQueryExecutor();

  /** High level DML operation on FTS TABLE */

  /** Insert a row into auxiliary INDEX_[1..6] table.
  Expects (word, first_doc_id, trx_id, roll_ptr, last_doc_id,
           doc_count, ilist).
  @param aux_index auxiliary table index
  @param aux_data  data to be inserted
  @return DB_SUCCESS or error code */
  dberr_t insert_aux_record(uint8_t aux_index,
                            const fts_aux_data_t *aux_data) noexcept;

  /** Insert a single doc_id into a common table (e.g. DELETED, ...)
  @param tbl_name  common table name
  @param doc_id    document id to be inserted
  @return DB_SUCCESS or error code */
  dberr_t insert_common_record(const char *tbl_name, doc_id_t doc_id) noexcept;

  /** Insert a key/value into CONFIG table.
  @param key key for the config table
  @param value value for the key
  @return DB_SUCCESS or error code */
  dberr_t insert_config_record(const char *key, const char *value) noexcept;

  /** Delete one word row from INDEX_[1..6] by (word).
  @param aux_index auxiliary table index
  @param aux_data  auxiliary table record
  @return DB_SUCCESS or error code */
  dberr_t delete_aux_record(uint8_t aux_index,
                            const fts_aux_data_t *aux_data) noexcept;

  /** Delete a single doc_id row from a common table by (doc_id).
  @param tbl_name common table name
  @param doc_id document id to be deleted
  @return DB_SUCCESS or error code */
  dberr_t delete_common_record(const char *tbl_name, doc_id_t doc_id) noexcept;

  /** Delete all rows from a common table.
  @return DB_SUCCESS or error code */
  dberr_t delete_all_common_records(const char *tbl_name) noexcept;

  /** Delete a key from CONFIG table by (key).
  @return DB_SUCCESS or error code */
  dberr_t delete_config_record(const char *key) noexcept;

  /** Upsert a key/value in CONFIG table.
  Replaces 'value' if key exists, inserts otherwise.
  @return DB_SUCCESS or error code */
  dberr_t update_config_record(const char *key, const char *value) noexcept;

  /** Select-for-update CONFIG row by 'key'
  @return DB_SUCCESS or error code */
  dberr_t read_config_with_lock(
    const char *key, RecordCallback& callback) noexcept;

  /** Read Auxiliary INDEX_[1..6] table rows at (or) after
  'word' with given cursor mode. Callback is invoked for each
  row for comparing it with word and process it if there is a match
  @return DB_SUCCESS or error code */
  dberr_t read_aux(uint8_t aux_index, const char *word,
		   page_cur_mode_t mode, RecordCallback& callback) noexcept;

  /** Read all INDEX_[1..6] rows
  Callback is invoked for each row for comparing it with word
  and process it if it is matching
  @return DB_SUCCESS or error code */
  dberr_t read_aux_all(uint8_t aux_index, RecordCallback& callback) noexcept;

  /** Read all rows from given COMMON table
  Callback is invoked for processing the record */
  dberr_t read_all_common(const char *tbl_name,
                          RecordCallback& callback) noexcept;
  mem_heap_t* get_heap() const noexcept
  { return m_executor->get_heap(); }

  void release_lock() { m_executor->commit_mtr(); }
};

/** Callback class for reading common table records
(DELETED, BEING_DELETED, DELETED_CACHE, BEING_DELETED_CACHE) */
class CommonTableReader : public RecordCallback
{
private:
  std::vector<doc_id_t> doc_ids;

public:
  CommonTableReader();

  const std::vector<doc_id_t>& get_doc_ids() const { return doc_ids; }
  void clear() { doc_ids.clear(); }
};

/** Callback class for reading FTS config table records */
class ConfigReader : public RecordCallback
{
public:
  span<const char> value_span;
  ConfigReader();
};

/** Type alias for FTS record processor function */
using FTSRecordProcessor= std::function<
  bool(const rec_t*, const dict_index_t*, const rec_offs*, void*)>;

/** Comparison modes for AuxRecordReader */
enum class AuxCompareMode
{
  /** >= comparison (range scan from word) */
  GREATER_EQUAL,
  /** > comparison (exclude exact match) */
  GREATER,
  /** LIKE pattern matching (prefix match) */
  LIKE,
  /** = comparison (exact match) */
  EQUAL
};

/** Callback class for reading FTS auxiliary index table records */
class AuxRecordReader : public RecordCallback
{
private:
  void *user_arg;
  ulint *total_memory;
  AuxCompareMode compare_mode;

private:
  /** FTS-specific record comparison logic */
  RecordCompareAction compare_record(
    const dtuple_t *search_tuple, const rec_t *rec,
    const dict_index_t *index, const rec_offs *offsets) noexcept;

public:
  /** Default word processor for FTS auxiliary table records */
  bool default_word_processor(const rec_t *rec, const dict_index_t *index,
                              const rec_offs *offsets, void *user_arg);

  /* Constructor with custom processor */
  template<typename ProcessorFunc>
  AuxRecordReader(void* user_data,
                  ProcessorFunc proc_func,
                  AuxCompareMode mode= AuxCompareMode::GREATER_EQUAL)
    : RecordCallback(
        [this, proc_func](const rec_t* rec, const dict_index_t* index,
                          const rec_offs* offsets) -> bool
	{
          return proc_func(rec, index, offsets, this->user_arg);
        },
        [this](const dtuple_t* search_tuple, const rec_t* rec,
               const dict_index_t* index, const rec_offs* offsets)
        -> RecordCompareAction
        {
          return this->compare_record(search_tuple, rec, index, offsets);
        }
      ),
      user_arg(user_data), total_memory(nullptr),
      compare_mode(mode) {}

  /* Different constructor with default word processing */
  AuxRecordReader(void *user_data, ulint *memory_counter,
                  AuxCompareMode mode= AuxCompareMode::GREATER_EQUAL)
    : RecordCallback(
        [this](const rec_t *rec, const dict_index_t *index,
               const rec_offs *offsets) -> bool
        {
          return this->default_word_processor(rec, index, offsets,
                                              this->user_arg);
        },
        [this](const dtuple_t *search_tuple, const rec_t *rec,
               const dict_index_t *index, const rec_offs *offsets)
        -> RecordCompareAction
	{
          return this->compare_record(search_tuple, rec, index, offsets);
        }
      ),
      user_arg(user_data), total_memory(memory_counter),
      compare_mode(mode) {}
};
