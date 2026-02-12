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

/** FTS deletion table types for m_common_tables array indexing */
enum class FTSDeletionTable : uint8_t
{
  DELETED = 0,
  DELETED_CACHE = 1,
  BEING_DELETED = 2,
  BEING_DELETED_CACHE = 3,
  MAX_DELETION_TABLES = 4
};

/** Helper to convert FTSDeletionTable to array index */
constexpr uint8_t to_index(FTSDeletionTable table_type) noexcept
{
  return static_cast<uint8_t>(table_type);
}

/** Number of deletion tables */
constexpr uint8_t NUM_DELETION_TABLES = to_index(FTSDeletionTable::MAX_DELETION_TABLES);

/** Abstraction over QueryExecutor for FTS auxiliary/common tables.
Handles table open/lock and provides typed helpers to insert,
delete and read records in FTS INDEX_1..INDEX_6
and deletion tables (DELETED, BEING_DELETED, etc.) */
class FTSQueryExecutor
{
private:
  QueryExecutor m_executor;
  const dict_index_t *m_index;
  const dict_table_t *const m_table;
  /* FTS Auxiliary table pointers */
  dict_table_t *m_aux_tables[6]={nullptr};
  /* FTS deletion table pointers (DELETED, BEING_DELETED, etc.) */
  dict_table_t *m_common_tables[NUM_DELETION_TABLES]={nullptr};
  /* FTS CONFIG table pointer */
  dict_table_t *m_config_table={nullptr};

  /** Table preparation methods */

  /** Open FTS INDEX_[1..6] table for the given auxiliary index.
  @return DB_SUCCESS or error code */
  dberr_t open_aux_table(uint8_t aux_index) noexcept;

  /** Open a deletion table (DELETED, BEING_DELETED, etc.).
  @param table_type deletion table type
  @return DB_SUCCESS or error code */
  dberr_t open_deletion_table(FTSDeletionTable table_type) noexcept;

  /** Helper to convert deletion table enum to string name */
  static const char* get_deletion_table_name(
                       FTSDeletionTable table_type) noexcept;

  /** Table lock operation */

  /** Acquire a lock on an opened INDEX_[1..6] table.
  Retries lock wait once via QueryExecutor.
  @return DB_SUCCESS or error code */
  dberr_t lock_aux_tables(uint8_t aux_index, lock_mode mode) noexcept;

  /** Lock all auxiliary tables */
  dberr_t lock_all_aux(lock_mode mode) noexcept;

  /** Acquire a lock on an opened common FTS table.
  Retries lock wait once via QueryExecutor.
  @return DB_SUCCESS or error code */
  dberr_t lock_common_tables(uint8_t index, lock_mode mode) noexcept;

  /** Acquire lock on opened all common FTS table
  @return DB_SUCCESS or error code */
  dberr_t lock_all_common(lock_mode mode) noexcept;

public:
  /** Open all auxiliary tables
  @return DB_SUCCESS or error code */
  dberr_t open_all_aux_tables(dict_index_t *fts_index) noexcept;

  /** Open all deletion tables (DELETED, BEING_DELETED, etc.)
  @return DB_SUCCESS or error code */
  dberr_t open_all_deletion_tables() noexcept;

  /** Open FTS CONFIG table for configuration operations.
  @return DB_SUCCESS or error code */
  dberr_t open_config_table() noexcept;

  /** Set CONFIG table directly (for cases where table is already opened)
  @param config_table CONFIG table pointer */
  void set_config_table(dict_table_t *config_table) noexcept
  { 
    m_config_table = config_table;
    m_config_table->acquire();
  }

  /** Create executor bound to trx and FTS table/index.
  @param trx transaction
  @param fts_table FTS table */
  FTSQueryExecutor(trx_t *trx, const dict_table_t *fts_table);

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
  { return m_executor.get_heap(); }

  trx_t* trx() const noexcept
  { return m_executor.get_trx(); }

  const dict_index_t* index() const noexcept
  { return m_index; }

  void release_lock() { m_executor.commit_mtr(); }

  /** Read records from table using underlying QueryExecutor
  @param table table to read
  @param tuple search tuple
  @param mode cursor mode
  @param callback record callback
  @return DB_SUCCESS or error code */
  dberr_t read(dict_table_t *table, const dtuple_t *tuple,
               page_cur_mode_t mode, RecordCallback& callback) noexcept
  { 
    dberr_t err= m_executor.read(table, tuple, mode, callback);
    return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
  }

  /** Read records by index using underlying QueryExecutor
  @param table table to read
  @param sec_index secondary index
  @param search_tuple search tuple
  @param mode cursor mode
  @param callback record callback
  @return DB_SUCCESS or error code */
  dberr_t read_by_index(dict_table_t *table, dict_index_t *sec_index,
                        const dtuple_t *search_tuple, page_cur_mode_t mode,
                        RecordCallback& callback) noexcept
  { 
    dberr_t err= m_executor.read_by_index(table, sec_index, search_tuple, mode, callback);
    return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
  }

  /** Construct FTS auxiliary table name
  @param table_name output buffer for table name
  @param suffix table suffix (e.g., "CONFIG", "INDEX_1")
  @param common_table true for common tables, false for index tables */
  void construct_table_name(char *table_name, const char *suffix,
                            bool common_table) noexcept;
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

  /** Fast common table field extraction for known table format.
  Structure: (doc_id BIGINT UNSIGNED) - always known schema
  @param rec          record to extract from
  @param index        common table index
  @param doc_id       output doc_id value
  @return true if extraction successful */
  static bool extract_common_fields(
    const rec_t *rec, const dict_index_t *index,
    doc_id_t *doc_id);
};

/** Callback class for reading FTS config table records */
class ConfigReader : public RecordCallback
{
public:
  span<const char> value_span;
  ConfigReader();

  /** Extract the config table record.
  Structure: (key VARCHAR, db_trx_id, db_roll_ptr, value TEXT)
  @param rec          record to extract from
  @param index        config table index
  @param key_data     output pointer to key data
  @param key_len      output key length
  @param value_data   output pointer to value data (optional)
  @param value_len    output value length (optional)
  @return true if extraction successful */
  static bool extract_config_fields(
    const rec_t *rec, const dict_index_t *index,
    const byte **key_data, ulint *key_len,
    const byte **value_data = nullptr, ulint *value_len = nullptr);

  /** Direct config key comparison - compares first field with tuple value.
  @param search_tuple   search tuple containing target key
  @param rec           record to compare
  @param index         config table index
  @return comparison action */
  static RecordCompareAction compare_config_key(
    const dtuple_t *search_tuple, const rec_t *rec,
    const dict_index_t *index);
};

/** Type alias for FTS record processor function */
using FTSRecordProcessor= std::function<
  dberr_t(const rec_t*, const dict_index_t*, const rec_offs*, void*)>;

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
    const dict_index_t *index) noexcept;

public:
  /** Structure to hold extracted auxiliary table fields */
  struct AuxRecordFields
  {
    const byte* word_data;
    ulint word_len;
    doc_id_t first_doc_id;
    doc_id_t last_doc_id;
    ulint doc_count;
    byte* ilist_data;
    ulint ilist_len;
    bool ilist_is_external;
    mem_heap_t* ilist_heap;

    AuxRecordFields() : word_data(nullptr), word_len(0), first_doc_id(0),
                       last_doc_id(0), doc_count(0), ilist_data(nullptr),
                       ilist_len(0), ilist_is_external(false), ilist_heap(nullptr) {}

    ~AuxRecordFields() { if (ilist_heap) mem_heap_free(ilist_heap); }

    AuxRecordFields(const AuxRecordFields&) = delete;
    AuxRecordFields& operator=(const AuxRecordFields&) = delete;
  };

  /** Fast auxiliary table field extraction for known table format.
  Structure: (word VARCHAR, first_doc_id BIGINT, ..., last_doc_id BIGINT, doc_count INT, ilist BLOB)
  @param rec          record to extract from
  @param index        auxiliary table index
  @param fields       output structure with extracted fields
  @param word_only    if true, extract only word field
  @return true if extraction successful */
  static bool extract_aux_fields(
    const rec_t *rec, const dict_index_t *index,
    AuxRecordFields &fields, bool word_only = false);

  /** Default word processor for FTS auxiliary table records */
  dberr_t default_word_processor(const rec_t *rec, const dict_index_t *index,
                                const rec_offs *offsets, void *user_arg);

  /* Constructor with custom processor */
  template<typename ProcessorFunc>
  AuxRecordReader(void* user_data,
                  ProcessorFunc proc_func,
                  AuxCompareMode mode= AuxCompareMode::GREATER_EQUAL)
    : RecordCallback(
        [this, proc_func](const rec_t* rec, const dict_index_t* index,
                          const rec_offs* offsets) -> dberr_t
	{
          return proc_func(rec, index, offsets, this->user_arg);
        },
        [this](const dtuple_t* search_tuple, const rec_t* rec,
               const dict_index_t* index)
        -> RecordCompareAction
        {
          return this->compare_record(search_tuple, rec, index);
        }
      ),
      user_arg(user_data), total_memory(nullptr),
      compare_mode(mode) {}

  /* Different constructor with default word processing */
  AuxRecordReader(void *user_data, ulint *memory_counter,
                  AuxCompareMode mode= AuxCompareMode::GREATER_EQUAL)
    : RecordCallback(
        [this](const rec_t *rec, const dict_index_t *index,
               const rec_offs *offsets) -> dberr_t
        {
          return this->default_word_processor(rec, index, offsets,
                                              this->user_arg);
        },
        [this](const dtuple_t *search_tuple, const rec_t *rec,
               const dict_index_t *index)
        -> RecordCompareAction
	{
          return this->compare_record(search_tuple, rec, index);
        }
      ),
      user_arg(user_data), total_memory(memory_counter),
      compare_mode(mode) {}

  /** Reset total memory counter */
  void reset_total_memory() { if (total_memory) *total_memory = 0; }
};
