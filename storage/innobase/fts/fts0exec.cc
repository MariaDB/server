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
@file fts/fts0exec.cc

Created 2025/11/05
*******************************************************/

#include "fts0exec.h"
#include "row0query.h"
#include "fts0fts.h"
#include "fts0types.h"
#include "fts0vlc.h"
#include "fts0priv.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "dict0dict.h"
#include "row0ins.h"
#include "row0upd.h"
#include "row0sel.h"
#include "eval0eval.h"
#include "que0que.h"
#include "trx0trx.h"
#include "lock0lock.h"
#include "rem0cmp.h"
#include "page0cur.h"
#include "ha_prototypes.h"


FTSQueryExecutor::FTSQueryExecutor(
  trx_t *trx, const dict_table_t *fts_table)
  : m_executor(trx), m_table(fts_table)
{}

FTSQueryExecutor::~FTSQueryExecutor()
{
  for (uint8_t i= 0; i < FTS_NUM_AUX_INDEX; i++)
    if (m_aux_tables[i]) m_aux_tables[i]->release();

  for (uint8_t i= 0; i < NUM_DELETION_TABLES; i++)
    if (m_common_tables[i]) m_common_tables[i]->release();

  if (m_config_table) m_config_table->release();
}

dberr_t FTSQueryExecutor::open_aux_table(uint8_t aux_index) noexcept
{
  if (m_aux_tables[aux_index]) return DB_SUCCESS;

  char table_name[MAX_FULL_NAME_LEN];
  construct_table_name(table_name, fts_get_suffix(aux_index), false);

  m_aux_tables[aux_index]= dict_table_open_on_name(
    table_name, false, DICT_ERR_IGNORE_TABLESPACE);
  return m_aux_tables[aux_index] ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::open_all_aux_tables(dict_index_t *fts_index) noexcept
{
  for (uint8_t idx= 0; idx < FTS_NUM_AUX_INDEX; idx++)
  {
    dict_table_t *table= m_aux_tables[idx];
    if (table)
    {
      table->release();
      m_aux_tables[idx]= nullptr;
    }
  }
  m_index= fts_index;
  for (uint8_t idx= 0; idx < FTS_NUM_AUX_INDEX; idx++)
  {
    dberr_t err= open_aux_table(idx);
    if (err) return err;
  }
  return DB_SUCCESS;
}

const char* FTSQueryExecutor::get_deletion_table_name(
  FTSDeletionTable table_type) noexcept
{
  switch (table_type)
  {
    case FTSDeletionTable::DELETED: return "DELETED";
    case FTSDeletionTable::DELETED_CACHE: return "DELETED_CACHE";
    case FTSDeletionTable::BEING_DELETED: return "BEING_DELETED";
    case FTSDeletionTable::BEING_DELETED_CACHE: return "BEING_DELETED_CACHE";
    default: return nullptr;
  }
}

/** Helper to convert table name to deletion table enum */
static FTSDeletionTable get_deletion_table_type(const char *tbl_name) noexcept
{
  if (!strcmp(tbl_name, "DELETED")) return FTSDeletionTable::DELETED;
  if (!strcmp(tbl_name, "DELETED_CACHE")) return FTSDeletionTable::DELETED_CACHE;
  if (!strcmp(tbl_name, "BEING_DELETED")) return FTSDeletionTable::BEING_DELETED;
  if (!strcmp(tbl_name, "BEING_DELETED_CACHE")) return FTSDeletionTable::BEING_DELETED_CACHE;
  return FTSDeletionTable::MAX_DELETION_TABLES;
}

dberr_t FTSQueryExecutor::open_deletion_table(FTSDeletionTable table_type) noexcept
{
  uint8_t index= to_index(table_type);
  if (index >= NUM_DELETION_TABLES)
    return DB_ERROR;
  
  if (m_common_tables[index]) return DB_SUCCESS;
  
  const char *suffix_name= get_deletion_table_name(table_type);
  if (!suffix_name) return DB_ERROR;
 
  char table_name[MAX_FULL_NAME_LEN];
  construct_table_name(table_name, suffix_name, true);

  m_common_tables[index]= dict_table_open_on_name(
    table_name, false, DICT_ERR_IGNORE_TABLESPACE);
  return m_common_tables[index] ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::open_config_table() noexcept
{
  if (m_config_table) return DB_SUCCESS;
  char table_name[MAX_FULL_NAME_LEN];
  construct_table_name(table_name, "CONFIG", true);

  m_config_table= dict_table_open_on_name(
    table_name, false, DICT_ERR_IGNORE_TABLESPACE);
  return m_config_table ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::open_all_deletion_tables() noexcept
{
  for (uint8_t i= 0; i < NUM_DELETION_TABLES; i++)
  {
    FTSDeletionTable table_type= static_cast<FTSDeletionTable>(i);
    dberr_t err= open_deletion_table(table_type);
    if (err) return err;
  }
  return DB_SUCCESS;
}

dberr_t FTSQueryExecutor::lock_aux_tables(uint8_t aux_index,
                                          lock_mode mode) noexcept
{
  dict_table_t *table= m_aux_tables[aux_index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  return m_executor.lock_table(table, mode);
}

dberr_t FTSQueryExecutor::lock_common_tables(uint8_t index,
                                             lock_mode mode) noexcept
{
  dict_table_t *table= m_common_tables[index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  return m_executor.lock_table(table, mode);
}

/* SQL equivalent:
     INSERT INTO $FTS_<table_id>_INDEX_<aux_index>
       (word, first_doc_id, last_doc_id, doc_count, ilist)
       VALUES (?, ?, ?, ?, ?);
Build a dtuple_t in stack memory with the 7 physical fields
(word, first_doc_id, db_trx_id, db_roll_ptr, last_doc_id, doc_count,
ilist) and call row_ins_clust_index_entry() directly.
No dict_sys.latch is taken on this path -*/
dberr_t FTSQueryExecutor::insert_aux_record(
  uint8_t aux_index, const fts_aux_data_t *aux_data) noexcept
{
  ut_ad(!dict_sys.locked());
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;

  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;
  ut_ad(m_aux_tables[aux_index]);
  err= lock_aux_tables(aux_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t *table= m_aux_tables[aux_index];
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 7 || index->n_uniq != 2)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t fields[7];
  doc_id_t first_doc_id, last_doc_id;

  dtuple_t tuple{0, 7, 2, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 7);
  /* Field 0: word (VARCHAR) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, aux_data->word, aux_data->word_len);

  /* Field 1: first_doc_id (INT) */
  field= dtuple_get_nth_field(&tuple, 1);
  fts_write_doc_id(&first_doc_id, aux_data->first_doc_id);
  dfield_set_data(field, &first_doc_id, sizeof(doc_id_t));

  /* Field 2: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&tuple, 2);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 3: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&tuple, 3);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  /* Field 4: last_doc_id (UNSIGNED INT) */
  field= dtuple_get_nth_field(&tuple, 4);
  fts_write_doc_id(&last_doc_id, aux_data->last_doc_id);
  dfield_set_data(field, &last_doc_id, sizeof(doc_id_t));

  /* Field 5: doc_count (UINT32_T) */
  byte doc_count[4];
  mach_write_to_4(doc_count, aux_data->doc_count);
  ut_ad(sizeof(aux_data->doc_count) == 4);
  field= dtuple_get_nth_field(&tuple, 5);
  dfield_set_data(field, doc_count, sizeof(doc_count));

  /* Field 6: ilist (VARBINARY) */
  field= dtuple_get_nth_field(&tuple, 6);
  dfield_set_data(field, aux_data->ilist, aux_data->ilist_len);

  return m_executor.insert_record(table, &tuple);
}

/* SQL equivalent:
     INSERT INTO $FTS_<table_id>_<tbl_name> (doc_id) VALUES (?);
where <tbl_name> is one of DELETED, DELETED_CACHE, BEING_DELETED,
BEING_DELETED_CACHE. Build a 3-field dtuple (doc_id + the two system
columns) and call row_ins_clust_index_entry(). */
dberr_t FTSQueryExecutor::insert_common_record(
  const char *tbl_name, doc_id_t doc_id) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(tbl_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES) return DB_ERROR;

  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= to_index(table_type);
  ut_ad(m_common_tables[index_no]); /* open succeeded -> pointer set */
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 3 || index->n_uniq != 1)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t fields[3];

  dtuple_t tuple{0, 3, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 3);
  /* Field 0: doc_id (INT) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  doc_id_t write_doc_id;
  fts_write_doc_id(&write_doc_id, doc_id);
  dfield_set_data(field, &write_doc_id, sizeof(doc_id_t));

  /* Field 1: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&tuple, 1);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 2: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&tuple, 2);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  return m_executor.insert_record(table, &tuple);
}

/* SQL equivalent:
     INSERT INTO $FTS_<table_id>_CONFIG (key, value) VALUES (?, ?);
The CONFIG table stores small <key,value> persistent state for the
FTS index (synced_doc_id, last_optimized_word, stopword settings, ..)
Build a 4-field dtuple (key, db_trx_id, db_roll_ptr, value) and
insert directly. */
dberr_t FTSQueryExecutor::insert_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;
  ut_ad(m_config_table); /* open succeeded -> pointer set */

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_config_table;
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 4 || index->n_uniq != 1)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t fields[4];

  dtuple_t tuple{0, 4, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 4);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  /* Field 1: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&tuple, 1);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 2: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&tuple, 2);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  /* Field 3: value (CHAR(200)) */
  field= dtuple_get_nth_field(&tuple, 3);
  dfield_set_data(field, value, strlen(value));

  return m_executor.insert_record(table, &tuple);
}

/* SQL equivalent:
     INSERT INTO $FTS_<table_id>_CONFIG (key, value) VALUES (?, ?)
       ON DUPLICATE KEY UPDATE value = ?;
Upsert on the CONFIG table.  Maps onto QueryExecutor::replace_record
which itself runs SELECT FOR UPDATE + UPDATE-or-INSERT. */
dberr_t FTSQueryExecutor::update_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;
  ut_ad(m_config_table);

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_config_table;
  dict_index_t* index= dict_table_get_first_index(table);

  if (index->n_fields != 4 || index->n_uniq != 1)
    return DB_ERROR;

  byte sys_buf[DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN]= {0};
  dfield_t search_fields[1];
  dfield_t insert_fields[4];

  dtuple_t search_tuple{0, 1, 1, 0, search_fields, nullptr
#ifdef UNIV_DEBUG
                        , DATA_TUPLE_MAGIC_N
#endif
                        };
  dict_index_copy_types(&search_tuple, index, 1);
  dfield_t *field= dtuple_get_nth_field(&search_tuple, 0);
  dfield_set_data(field, key, strlen(key));

  dtuple_t insert_tuple{0, 4, 1, 0, insert_fields, nullptr
#ifdef UNIV_DEBUG
                        , DATA_TUPLE_MAGIC_N
#endif
                        };
  dict_index_copy_types(&insert_tuple, index, 4);

  /* Field 0: key (CHAR(50)) */
  field= dtuple_get_nth_field(&insert_tuple, 0);
  dfield_set_data(field, key, strlen(key));

  /* Field 1: trx_id (DB_TRX_ID) */
  field= dtuple_get_nth_field(&insert_tuple, 1);
  dfield_set_data(field, sys_buf, DATA_TRX_ID_LEN);

  /* Field 2: roll_ptr (DB_ROLL_PTR) */
  field= dtuple_get_nth_field(&insert_tuple, 2);
  dfield_set_data(field, sys_buf + DATA_TRX_ID_LEN, DATA_ROLL_PTR_LEN);

  /* Field 3: value (CHAR(200)) */
  field= dtuple_get_nth_field(&insert_tuple, 3);
  dfield_set_data(field, value, strlen(value));

  upd_field_t upd_field;
  upd_field.field_no= 3;
  upd_field.orig_len= 0;
  upd_field.exp= nullptr;
  dfield_set_data(&upd_field.new_val, value, strlen(value));
  dict_col_copy_type(dict_index_get_nth_col(index, 3),
                     dfield_get_type(&upd_field.new_val));

  upd_t update;
  update.heap= nullptr;
  update.info_bits= 0;
  update.old_vrow= nullptr;
  update.n_fields= 1;
  update.fields= &upd_field;

  return m_executor.replace_record(table, &search_tuple, &update,
                                   &insert_tuple);
}

/* SQL equivalent:
     DELETE FROM $FTS_<table_id>_INDEX_<aux_index> WHERE word = ?;
Multi-row delete from one auxiliary index.  A single word can span
several rows in INDEX_<n> because the unique key is
(word, first_doc_id) and the ilist gets split at FTS_ILIST_MAX_SIZE
boundaries.  QueryExecutor::delete_record() is single-row, so we
loop until DB_RECORD_NOT_FOUND to match the old parser semantics of
"DELETE WHERE word = ?". */
dberr_t FTSQueryExecutor::delete_aux_record(
  uint8_t aux_index, const fts_aux_data_t *aux_data) noexcept
{
  ut_ad(!dict_sys.locked());
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;

  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;
  ut_ad(m_aux_tables[aux_index]);
  err= lock_aux_tables(aux_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t *table= m_aux_tables[aux_index];
  dict_index_t *index= dict_table_get_first_index(table);

  if (dict_table_get_next_index(index) != nullptr)
    return DB_ERROR;

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: word (VARCHAR) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, aux_data->word, aux_data->word_len);

  for (;;)
  {
    err= m_executor.delete_record(table, &tuple);
    if (err == DB_RECORD_NOT_FOUND)
    {
      err= DB_SUCCESS;
      break;
    }
    if (err != DB_SUCCESS)
      break;
  }
  return err;
}

/* SQL equivalent:
     DELETE FROM $FTS_<table_id>_<table_name> WHERE doc_id = ?;
Single-doc-id delete from a deletion table (DELETED,
DELETED_CACHE, BEING_DELETED, BEING_DELETED_CACHE).  */
dberr_t FTSQueryExecutor::delete_common_record(
  const char *table_name, doc_id_t doc_id) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(table_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES)
    return DB_ERROR;

  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= to_index(table_type);
  ut_ad(m_common_tables[cached_index]);
  err= lock_common_tables(cached_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[cached_index];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: doc_id */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  doc_id_t write_doc_id;
  fts_write_doc_id(&write_doc_id, doc_id);
  dfield_set_data(field, &write_doc_id, sizeof(doc_id_t));

  return m_executor.delete_record(table, &tuple);
}

/* SQL equivalent:
     DELETE FROM $FTS_<table_id>_<table_name>;
Truncate-style drain.  Used to roll the CACHE deletion tables
into the persistent ones during optimize. */
dberr_t FTSQueryExecutor::delete_all_common_records(
  const char *table_name) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(table_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES) return DB_ERROR;

  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= to_index(table_type);
  ut_ad(m_common_tables[cached_index]);
  err= lock_common_tables(cached_index, LOCK_X);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[cached_index];
  return m_executor.delete_all(table);
}

/* SQL equivalent:
     DELETE FROM $FTS_<table_id>_CONFIG WHERE key = ?;
Removes one persistent config entry.  Old path used the cached
config-DELETE graph. */
dberr_t FTSQueryExecutor::delete_config_record(const char *key) noexcept
{
  ut_ad(!dict_sys.locked());
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;
  ut_ad(m_config_table);

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_config_table;
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];

  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  return m_executor.delete_record(table, &tuple);
}

/* SQL equivalent:
     SELECT value FROM $FTS_<table_id>_CONFIG WHERE key = ? FOR UPDATE;
Returns the value through the callback's process_record (which
reads field 3 of the matched row). */
dberr_t FTSQueryExecutor::read_config_with_lock(const char *key,
                                               RecordCallback &callback) noexcept
{
  ut_ad(!dict_sys.locked());
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;
  ut_ad(m_config_table);

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_config_table;
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: key (CHAR(50)) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, key, strlen(key));

  err= m_executor.select_for_update(table, &tuple, &callback);
  return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
}

dtuple_t* FTSQueryExecutor::create_word_search_tuple(
  dict_table_t *table, const fts_string_t *word,
  mem_heap_t *heap) noexcept
{
  if (!word || word->f_len == 0)
    return nullptr;

  dtuple_t* tuple= dtuple_create(heap, 1);
  dict_table_copy_types(tuple, table);
  dfield_t* field= dtuple_get_nth_field(tuple, 0);
  dfield_set_data(field, word->f_str, word->f_len);
  dtuple_set_n_fields_cmp(tuple, 1);

  return tuple;
}

/* SQL equivalent:
     SELECT doc_id FROM $FTS_<table_id>_<tbl_name>;
Full-table scan of a deletion table, every visible doc_id fed
through the callback.  */
dberr_t FTSQueryExecutor::read_all_common(const char *tbl_name,
                                          RecordCallback &callback) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(tbl_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES) return DB_ERROR;

  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= to_index(table_type);
  ut_ad(m_common_tables[index_no]);

  dict_table_t *table= m_common_tables[index_no];
  dict_index_t *index= dict_table_get_first_index(table);
  err= m_executor.read_by_index(table, index, nullptr, PAGE_CUR_G,
                                callback, true);
  return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
}

bool CommonTableReader::extract_common_fields(
  const rec_t *rec, const dict_index_t *index, doc_id_t *doc_id)
{
  ulint len= 0;
  const byte *doc_id_data= nullptr;
  if (!dict_table_is_comp(index->table))
    doc_id_data = rec_get_nth_field_old(rec, 0, &len);
  else
  {
    mem_heap_t *heap= nullptr;
    rec_offs offsets_[REC_OFFS_NORMAL_SIZE];
    rec_offs* offsets= rec_get_offsets(rec, index, offsets_,
                                       index->n_core_fields, 1, &heap);
    doc_id_data = rec_get_nth_field(rec, offsets, 0, &len);
    if (heap)
      mem_heap_free(heap);
  }

  if (!doc_id_data || len != sizeof(doc_id_t))
    return false;

  *doc_id= mach_read_from_8(doc_id_data);
  return true;
}

/* Both constructors share the same callbacks: extract each
doc_id and append it to m_doc_ids; the WHERE predicate accepts
every row.  They differ only in whether m_doc_ids is owned
or borrowed from the caller */
CommonTableReader::CommonTableReader()
  : RecordCallback(
      [this](const rec_t *rec, const dict_index_t *index,
             const rec_offs *offsets) -> dberr_t
      {
        doc_id_t doc_id;
        if (!extract_common_fields(rec, index, &doc_id))
          return DB_CORRUPTION;
        *static_cast<doc_id_t*>(ib_vector_push(m_doc_ids, nullptr))= doc_id;
        return DB_SUCCESS;
      },
      [](const dtuple_t *search_tuple, const rec_t *rec,
         const dict_index_t *index, const rec_offs *offsets)
           -> RecordCompareAction
      { return RecordCompareAction::PROCESS; }),
    m_heap(mem_heap_create(512)),
    m_doc_ids(ib_vector_create(ib_heap_allocator_create(m_heap),
                               sizeof(doc_id_t), 32)) {}

CommonTableReader::CommonTableReader(ib_vector_t *dest)
  : RecordCallback(
      [this](const rec_t *rec, const dict_index_t *index,
             const rec_offs *offsets) -> dberr_t
      {
        doc_id_t doc_id;
        if (!extract_common_fields(rec, index, &doc_id))
          return DB_CORRUPTION;
        *static_cast<doc_id_t*>(ib_vector_push(m_doc_ids, nullptr))= doc_id;
        return DB_SUCCESS;
      },
      [](const dtuple_t *search_tuple, const rec_t *rec,
         const dict_index_t *index, const rec_offs *offsets)
           -> RecordCompareAction
      { return RecordCompareAction::PROCESS; }),
    m_heap(nullptr), m_doc_ids(dest) {}

CommonTableReader::~CommonTableReader()
{
  if (m_heap)
    mem_heap_free(m_heap);
}


ConfigReader::ConfigReader() : RecordCallback(
  [this](const rec_t *rec, const dict_index_t *index,
         const rec_offs *offsets) -> dberr_t
  {
    ulint value_len;
    const byte *value_data= rec_get_nth_field(rec, offsets, 3, &value_len);

    if (value_data && value_len != UNIV_SQL_NULL && value_len > 0)
      value_span= std::string_view(reinterpret_cast<const char*>(value_data),
                                   value_len);

    return DB_SUCCESS;
  },
  [](const dtuple_t *search_tuple, const rec_t *rec,
     const dict_index_t *index, const rec_offs *offsets) -> RecordCompareAction
  {
    return compare_config_key(search_tuple, rec, index, offsets);
  }) {}

/** Initial size of nodes in fts_word_t. */
static const ulint FTS_WORD_NODES_INIT_SIZE= 64;

/** Initialize fts_word_t structure. */
static void init_fts_word(fts_word_t *word, const byte *utf8,
                          ulint len, mem_heap_t *word_heap)
{
  mem_heap_t* heap= word_heap ? word_heap : mem_heap_create(sizeof(fts_node_t));
  memset(word, 0, sizeof(*word));
  word->text.f_len= len;
  word->text.f_str= static_cast<byte*>(mem_heap_alloc(heap, len + 1));
  memcpy(word->text.f_str, utf8, len);
  word->text.f_str[len]= 0;
  word->heap_alloc= ib_heap_allocator_create(heap);
  word->nodes= ib_vector_create(word->heap_alloc, sizeof(fts_node_t),
                                FTS_WORD_NODES_INIT_SIZE);
}

/** AuxRecordReader default word processor implementation */
dberr_t AuxRecordReader::default_word_processor(
  const rec_t *rec, const dict_index_t *index,
  const rec_offs *offsets, void *user_arg)
{
  ib_vector_t *words= static_cast<ib_vector_t*>(user_arg);

  /* Extract fields using rec_get_nth_field() */
  ulint word_len;
  const byte* word_data= rec_get_nth_field(rec, offsets, 0, &word_len);
  if (!word_data || word_len == UNIV_SQL_NULL || word_len > FTS_MAX_WORD_LEN)
    return DB_SUCCESS;

  ulint first_doc_id_len;
  const byte* first_doc_id_data= rec_get_nth_field(rec, offsets, 1, &first_doc_id_len);
  doc_id_t first_doc_id= fts_read_doc_id(first_doc_id_data);

  ulint last_doc_id_len;
  const byte* last_doc_id_data= rec_get_nth_field(rec, offsets, 4, &last_doc_id_len);
  doc_id_t last_doc_id= fts_read_doc_id(last_doc_id_data);

  ulint doc_count_len;
  const byte* doc_count_data= rec_get_nth_field(rec, offsets, 5, &doc_count_len);
  uint32_t doc_count= mach_read_from_4(doc_count_data);

  ulint ilist_len;
  const byte* ilist_data= rec_get_nth_field(rec, offsets, 6, &ilist_len);

  fts_word_t *word;
  bool is_word_init= false;

  ut_ad(word_len <= FTS_MAX_WORD_LEN);

  if (ib_vector_size(words) == 0)
  {
    /* First word - push and initialize */
    word= static_cast<fts_word_t*>(ib_vector_push(words, nullptr));
    init_fts_word(word, word_data, word_len, m_words_heap);
    is_word_init= true;
  }
  else
  {
    /* Check if this word is different from the last word */
    word= static_cast<fts_word_t*>(ib_vector_last(words));
    if (word_len != word->text.f_len ||
        memcmp(word->text.f_str, word_data, word_len))
    {
      /* Different word - push new word and initialize */
      word= static_cast<fts_word_t*>(ib_vector_push(words, nullptr));
      init_fts_word(word, word_data, word_len, m_words_heap);
      is_word_init= true;
    }
  }
  fts_node_t *node= static_cast<fts_node_t*>(
    ib_vector_push(word->nodes, nullptr));

  /* Use extracted field values */
  node->first_doc_id= first_doc_id;
  node->last_doc_id= last_doc_id;
  node->doc_count= doc_count;

  node->ilist_size_alloc= node->ilist_size= 0;
  node->ilist= nullptr;

  if (ilist_data && ilist_len != UNIV_SQL_NULL && ilist_len > 0)
  {
    node->ilist_size_alloc= node->ilist_size= ilist_len;
    if (ilist_len)
    {
      node->ilist= static_cast<byte*>(ut_malloc_nokey(ilist_len));
      memcpy(node->ilist, ilist_data, ilist_len);
    }
    if (ilist_len == 0) return DB_SUCCESS_LOCKED_REC;
  }

  if (this->total_memory)
  {
    if (is_word_init)
    {
      *this->total_memory+=
         sizeof(fts_word_t) + sizeof(ib_alloc_t) +
         sizeof(ib_vector_t) + word_len +
         sizeof(fts_node_t) * FTS_WORD_NODES_INIT_SIZE;
    }
    *this->total_memory+= node->ilist_size;
    if (*this->total_memory >= fts_result_cache_limit)
      return DB_FTS_EXCEED_RESULT_CACHE_LIMIT;
  }
  return DB_SUCCESS;
}

/** AuxRecordReader comparison logic implementation */
RecordCompareAction AuxRecordReader::compare_record(
  const dtuple_t *search_tuple, const rec_t *rec,
  const dict_index_t *index, const rec_offs *offsets) noexcept
{
  if (!search_tuple) return RecordCompareAction::PROCESS;
  int cmp_result;
  switch (compare_mode)
  {
    case AuxCompareMode::GREATER_EQUAL:
    case AuxCompareMode::GREATER:
    {
      cmp_result= cmp_dtuple_rec(search_tuple, rec, index, offsets);
      if (compare_mode == AuxCompareMode::GREATER_EQUAL)
        return (cmp_result <= 0) ? RecordCompareAction::PROCESS
                                 : RecordCompareAction::SKIP;
      else
        return (cmp_result < 0) ? RecordCompareAction::PROCESS
                                : RecordCompareAction::SKIP;
    }
    case AuxCompareMode::LIKE:
    case AuxCompareMode::EQUAL:
    {
      const dfield_t* search_field= dtuple_get_nth_field(search_tuple, 0);
      const void* search_data= dfield_get_data(search_field);
      ulint search_len= dfield_get_len(search_field);

      if (!search_data || search_len == UNIV_SQL_NULL)
        return RecordCompareAction::PROCESS;

      ulint word_len;
      const byte* word_data= rec_get_nth_field(rec, offsets, 0, &word_len);
      if (!word_data || word_len == UNIV_SQL_NULL)
        return RecordCompareAction::SKIP;

      const dtype_t* type= dfield_get_type(search_field);
      cmp_result= cmp_data(type->mtype, type->prtype, false,
                           static_cast<const byte*>(search_data),
                           search_len, word_data, word_len);
      if (compare_mode == AuxCompareMode::EQUAL)
        return cmp_result == 0
               ? RecordCompareAction::PROCESS
               : RecordCompareAction::STOP;
      else /* AuxCompareMode::LIKE */
      {
        int prefix_cmp= cmp_data(type->mtype, type->prtype, false,
                                 static_cast<const byte*>(search_data),
                                 search_len, word_data,
                                 search_len <= word_len ? search_len : word_len);

        if (prefix_cmp != 0) return RecordCompareAction::STOP;
        return (search_len <= word_len) ? RecordCompareAction::PROCESS
                                        : RecordCompareAction::SKIP;
      }
    }
  }
  return RecordCompareAction::PROCESS;
}

/** Direct config key comparison implementation */
RecordCompareAction ConfigReader::compare_config_key(
  const dtuple_t *search_tuple, const rec_t *rec,
  const dict_index_t *index, const rec_offs *offsets)
{
  if (!search_tuple) return RecordCompareAction::PROCESS;
  const dfield_t *search_field= dtuple_get_nth_field(search_tuple, 0);
  const void *search_data= dfield_get_data(search_field);
  ulint search_len= dfield_get_len(search_field);
  if (!search_data || search_len == UNIV_SQL_NULL)
    return RecordCompareAction::PROCESS;

  ulint rec_key_len;
  const byte *rec_key_data= rec_get_nth_field(rec, offsets, 0, &rec_key_len);

  if (!rec_key_data || rec_key_len == UNIV_SQL_NULL)
    return RecordCompareAction::SKIP;

  const dtype_t *type= dfield_get_type(search_field);
  int cmp_result= cmp_data(type->mtype, type->prtype, false,
                           static_cast<const byte*>(search_data),
                           search_len, rec_key_data, rec_key_len);

  return (cmp_result == 0) ? RecordCompareAction::PROCESS
                           : RecordCompareAction::SKIP;
}

void FTSQueryExecutor::construct_table_name(
  char *table_name, const char *suffix, bool common_table) noexcept
{
  ut_ad(m_table);
  ut_ad(common_table || m_index);
  const size_t dbname_len= m_table->name.dblen() + 1;
  ut_ad(dbname_len > 1);
  memcpy(table_name, m_table->name.m_name, dbname_len);
  memcpy(table_name+= dbname_len, "FTS_", 4);
  table_name+= 4;

  int len= fts_write_object_id(m_table->id, table_name,
                               FTS_AUX_MIN_TABLE_ID_LENGTH);
  if (!common_table)
  {
    table_name[len]= '_';
    ++len;
    len+= fts_write_object_id(m_index->id, table_name + len,
                              FTS_AUX_MIN_TABLE_ID_LENGTH);
  }
  ut_a(len >= 16);
  ut_a(len < FTS_AUX_MIN_TABLE_ID_LENGTH);
  table_name+= len;
  *table_name++= '_';
  strcpy(table_name, suffix);
}

/* SQL equivalent:
     SELECT word, first_doc_id, last_doc_id, doc_count, ilist
       FROM $FTS_<table_id>_INDEX_<aux_index>
       WHERE word >= <start_word>  when start_word != NULL
the callback's compare_record refines further;
see AuxCompareMode for EQUAL / GREATER / GREATER_EQUAL / LIKE)
Range scan that drives the AuxRecordReader callback for FTS query
evaluation and INFORMATION_SCHEMA reads.  */
dberr_t FTSQueryExecutor::read_aux(uint8_t aux_index,
                                   const fts_string_t *start_word,
                                   RecordCallback &callback) noexcept
{
  ut_ad(!dict_sys.locked());

  if (aux_index >= FTS_NUM_AUX_INDEX)
    return DB_ERROR;

  dberr_t error= open_aux_table(aux_index);
  if (error != DB_SUCCESS)
    return error;
  ut_ad(m_aux_tables[aux_index]);

  dict_table_t *table= m_aux_tables[aux_index];
  dict_index_t *index= dict_table_get_first_index(table);

  mem_heap_t* heap= nullptr;
  dtuple_t* search_tuple= nullptr;
  page_cur_mode_t mode= PAGE_CUR_G;

  if (start_word && start_word->f_len > 0)
  {
    if (heap == nullptr)
      heap= mem_heap_create(256);
    search_tuple= create_word_search_tuple(table, start_word, heap);
    mode= PAGE_CUR_GE;
  }
  error= m_executor.read_by_index(table, index, search_tuple,
                                  mode, callback, true);
  if (heap)
    mem_heap_free(heap);
  return error;
}
