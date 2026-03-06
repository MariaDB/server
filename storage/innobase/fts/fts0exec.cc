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

const char* FTSQueryExecutor::get_deletion_table_name(FTSDeletionTable table_type) noexcept
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
static FTSDeletionTable get_deletion_table_type(const char* tbl_name)
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
  
  const char* suffix_name= get_deletion_table_name(table_type);
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
  dberr_t err= m_executor.lock_table(table, mode);
  if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
  return err;
}

dberr_t FTSQueryExecutor::lock_all_aux(lock_mode mode) noexcept
{
  for (uint8_t aux_index= 0; aux_index < FTS_NUM_AUX_INDEX;
       aux_index++)
  {
    dict_table_t *table= m_aux_tables[aux_index];
    if (table == nullptr) return DB_TABLE_NOT_FOUND;
    dberr_t err= m_executor.lock_table(table, mode);
    if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
    if (err) return err;
  }
  return DB_SUCCESS;
}

dberr_t FTSQueryExecutor::lock_common_tables(uint8_t index,
                                             lock_mode mode) noexcept
{
  dict_table_t *table= m_common_tables[index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  dberr_t err= m_executor.lock_table(table, mode);
  if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
  return err;
}

dberr_t FTSQueryExecutor::lock_all_common(lock_mode mode) noexcept
{
  for (uint8_t i= 0; i < NUM_DELETION_TABLES; i++)
  {
    dict_table_t *table= m_common_tables[i];
    if (table == nullptr) return DB_TABLE_NOT_FOUND;
    dberr_t err= m_executor.lock_table(table, mode);
    if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
    if (err) return err;
  }
  return DB_SUCCESS;
}

dberr_t FTSQueryExecutor::insert_aux_record(
  uint8_t aux_index, const fts_aux_data_t* aux_data) noexcept
{
  ut_ad(!dict_sys.locked());
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;

  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;
  err= lock_aux_tables(aux_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
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
  field= dtuple_get_nth_field(&tuple, 5);
  dfield_set_data(field, doc_count, sizeof(doc_count));

  /* Field 6: ilist (VARBINARY) */
  field= dtuple_get_nth_field(&tuple, 6);
  dfield_set_data(field, aux_data->ilist, aux_data->ilist_len);

  return m_executor.insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::insert_common_record(
  const char *tbl_name, doc_id_t doc_id) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(tbl_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES) return DB_ERROR;
  
  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;
  
  uint8_t index_no= to_index(table_type);
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

dberr_t FTSQueryExecutor::insert_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
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

dberr_t FTSQueryExecutor::update_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
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

dberr_t FTSQueryExecutor::delete_aux_record(
  uint8_t aux_index, const fts_aux_data_t* aux_data) noexcept
{
  ut_ad(!dict_sys.locked());
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;

  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;
  err= lock_aux_tables(aux_index, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  dict_index_t* index= dict_table_get_first_index(table);

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

  return m_executor.delete_record(table, &tuple);
}

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

dberr_t FTSQueryExecutor::delete_all_common_records(
  const char *table_name) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(table_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES) return DB_ERROR;
  
  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= to_index(table_type);
  err= lock_common_tables(cached_index, LOCK_X);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[cached_index];
  return m_executor.delete_all(table);
}

dberr_t FTSQueryExecutor::delete_config_record(
  const char *key) noexcept
{
  ut_ad(!dict_sys.locked());
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
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

dberr_t FTSQueryExecutor::read_config_with_lock(const char *key,
                                               RecordCallback& callback) noexcept
{
  ut_ad(!dict_sys.locked());
  dberr_t err= open_config_table();
  if (err != DB_SUCCESS) return err;

  err= m_executor.lock_table(m_config_table, LOCK_IX);
  if (err == DB_LOCK_WAIT) err= m_executor.handle_wait(err, true);
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

dberr_t FTSQueryExecutor::read_aux(uint8_t aux_index,
                                   const char *word,
                                   page_cur_mode_t mode,
                                   RecordCallback& callback) noexcept
{
  ut_ad(!dict_sys.locked());
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;
  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;

  err= lock_aux_tables(aux_index, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  dict_index_t* index= dict_table_get_first_index(table);

  dfield_t fields[1];
  dtuple_t tuple{0, 1, 1, 0, fields, nullptr
#ifdef UNIV_DEBUG
                 , DATA_TUPLE_MAGIC_N
#endif
                 };
  dict_index_copy_types(&tuple, index, 1);
  /* Field 0: word (VARCHAR) */
  dfield_t *field= dtuple_get_nth_field(&tuple, 0);
  dfield_set_data(field, word, strlen(word));

  err= m_executor.read(table, &tuple, mode, callback);
  return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
}

dberr_t FTSQueryExecutor::read_aux_all(uint8_t aux_index, RecordCallback& callback) noexcept
{
  ut_ad(!dict_sys.locked());
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;
  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;

  err= lock_aux_tables(aux_index, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  err= m_executor.read(table, nullptr, PAGE_CUR_GE, callback);
  return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
}

dberr_t FTSQueryExecutor::read_all_common(const char *tbl_name,
                                          RecordCallback& callback) noexcept
{
  ut_ad(!dict_sys.locked());
  FTSDeletionTable table_type= get_deletion_table_type(tbl_name);
  if (table_type == FTSDeletionTable::MAX_DELETION_TABLES) return DB_ERROR;
  
  dberr_t err= open_deletion_table(table_type);
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= to_index(table_type);
  err= lock_common_tables(index_no, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  err= m_executor.read(table, nullptr, PAGE_CUR_GE, callback);
  return (err == DB_SUCCESS_LOCKED_REC) ? DB_SUCCESS : err;
}

CommonTableReader::CommonTableReader() : RecordCallback(
  [this](const rec_t* rec, const dict_index_t* index,
         const rec_offs* offsets) -> dberr_t
  {
    doc_id_t doc_id;
    if (extract_common_fields(rec, index, &doc_id))
      doc_ids.push_back(doc_id);
    return DB_SUCCESS;
  },
  [](const dtuple_t* search_tuple, const rec_t* rec,
     const dict_index_t* index) -> RecordCompareAction
  { return RecordCompareAction::PROCESS; }) {}


ConfigReader::ConfigReader() : RecordCallback(
  [this](const rec_t* rec, const dict_index_t* index,
         const rec_offs* offsets) -> dberr_t
  {
    const byte *value_data, *key_data;
    ulint value_len, key_len;
    if (extract_config_fields(rec, index, &key_data, &key_len,
                             &value_data, &value_len))
    {
      if (value_data && value_len != UNIV_SQL_NULL && value_len > 0)
        value_span= span<const char>(
          reinterpret_cast<const char*>(value_data), value_len);
    }
    return DB_SUCCESS;
  },
  [](const dtuple_t* search_tuple, const rec_t* rec,
     const dict_index_t* index) -> RecordCompareAction
  {
    return compare_config_key(search_tuple, rec, index);
  }) {}

/** Initial size of nodes in fts_word_t. */
static const ulint FTS_WORD_NODES_INIT_SIZE= 64;

/** Initialize fts_word_t structure */
static void init_fts_word(fts_word_t* word, const byte* utf8, ulint len)
{
  mem_heap_t* heap= mem_heap_create(sizeof(fts_node_t));
  memset(word, 0, sizeof(*word));
  word->text.f_len= len;
  word->text.f_str= static_cast<byte*>(mem_heap_alloc(heap, len + 1));
  memcpy(word->text.f_str, utf8, len);
  word->text.f_str[len]= 0;
  word->heap_alloc= ib_heap_allocator_create(heap);
  word->nodes= ib_vector_create(word->heap_alloc, sizeof(fts_node_t),
                                FTS_WORD_NODES_INIT_SIZE);
}

bool AuxRecordReader::extract_aux_fields(
  const rec_t* rec, const dict_index_t* index,
  AuxRecordFields& fields, bool word_only)
{
  const byte *ilist_data= nullptr;
  bool ilist_external= false;
  if (dict_table_is_comp(index->table))
  {
    const byte* lens=
      rec - REC_N_NEW_EXTRA_BYTES - 1 - index->n_core_null_bytes;
    ulint wlen= *lens--;
    if (UNIV_UNLIKELY(wlen & 0x80) && DATA_BIG_COL(index->fields[0].col))
    {
      wlen<<= 8;
      wlen|= *lens--;
      fields.word_len= (wlen & 0x3fff);
    }
    else
      fields.word_len= wlen;
    fields.word_data= rec;

    if (fields.word_len == UNIV_SQL_NULL || fields.word_len > FTS_MAX_WORD_LEN)
      return false;

    /* If only word field is needed, return early */
    if (word_only)
      return true;

    const byte* fixed_fields_start= rec + fields.word_len;
    fields.first_doc_id= mach_read_from_8(fixed_fields_start);
    /* DOC_ID (8) + DATA_TRX_ID(6) + DATA_ROLL_PTR(7) */
    fields.last_doc_id= mach_read_from_8(fixed_fields_start + 21);
    fields.doc_count= mach_read_from_4(fixed_fields_start + 29);

    /* Extract ilist length from current lens position */
    ulint ilen= *lens--;
    if (UNIV_UNLIKELY(ilen & 0x80) && DATA_BIG_COL(index->fields[6].col))
    {
      ilen <<= 8;
      ilen |= *lens--;
      ilist_external= (ilen & REC_OFFS_EXTERNAL);
      fields.ilist_len= (ilen & 0x3fff);
    }
    else
    {
      fields.ilist_len= ilen;
      ilist_external= false;
    }

    ilist_data= fixed_fields_start + 33;
  }
  else
  {
    fields.word_data= rec_get_nth_field_old(rec, 0, &fields.word_len);
    if (!fields.word_data || fields.word_len == UNIV_SQL_NULL ||
        fields.word_len > FTS_MAX_WORD_LEN)
      return false;

    if (word_only)
      return true;

    ulint len;
    const byte* data= rec_get_nth_field_old(rec, 1, &len);
    fields.first_doc_id= fts_read_doc_id(data);
    data= rec_get_nth_field_old(rec, 4, &len);
    fields.last_doc_id= fts_read_doc_id(data);
    data= rec_get_nth_field_old(rec, 5, &len);
    fields.doc_count= mach_read_from_4(data);

    ilist_data= data + 4;
    ulint offs= rec_get_nth_field_offs_old(rec, 6, &fields.ilist_len);
    ilist_external= (offs & REC_OFFS_EXTERNAL);
  }

  if (ilist_external)
  {
    fields.ilist_heap= mem_heap_create(fields.ilist_len + 1000);
    ulint external_len;
    byte* external_data= btr_copy_externally_stored_field(
      &external_len, ilist_data, index->table->space->zip_size(),
      fields.ilist_len, fields.ilist_heap);
    if (external_data)
    {
      fields.ilist_data= external_data;
      fields.ilist_len= external_len;
    }
    else return false;
  }
  else fields.ilist_data= const_cast<byte*>(ilist_data);
  return true;
}

/** AuxRecordReader default word processor implementation */
dberr_t AuxRecordReader::default_word_processor(
  const rec_t* rec, const dict_index_t* index,
  const rec_offs* offsets, void* user_arg)
{
  ib_vector_t *words= static_cast<ib_vector_t*>(user_arg);
  AuxRecordFields fields;
  /* Use optimized field extraction with external BLOB handling */
  if (!extract_aux_fields(rec, index, fields))
    return DB_SUCCESS;
  fts_word_t *word;
  bool is_word_init = false;

  ut_ad(fields.word_len <= FTS_MAX_WORD_LEN);

  if (ib_vector_size(words) == 0)
  {
    /* First word - push and initialize */
    word = static_cast<fts_word_t*>(ib_vector_push(words, nullptr));
    init_fts_word(word, fields.word_data, fields.word_len);
    is_word_init = true;
  }
  else
  {
    /* Check if this word is different from the last word */
    word = static_cast<fts_word_t*>(ib_vector_last(words));
    if (fields.word_len != word->text.f_len ||
        memcmp(word->text.f_str, fields.word_data, fields.word_len))
    {
      /* Different word - push new word and initialize */
      word = static_cast<fts_word_t*>(ib_vector_push(words, nullptr));
      init_fts_word(word, fields.word_data, fields.word_len);
      is_word_init = true;
    }
  }
  fts_node_t *node= static_cast<fts_node_t*>(
    ib_vector_push(word->nodes, nullptr));

  /* Use extracted field values */
  node->first_doc_id= fields.first_doc_id;
  node->last_doc_id= fields.last_doc_id;
  node->doc_count= fields.doc_count;

  node->ilist_size_alloc= node->ilist_size= 0;
  node->ilist= nullptr;

  if (fields.ilist_data && fields.ilist_len != UNIV_SQL_NULL && fields.ilist_len > 0)
  {
    node->ilist_size_alloc= node->ilist_size= fields.ilist_len;
    if (fields.ilist_len)
    {
      node->ilist= static_cast<byte*>(ut_malloc_nokey(fields.ilist_len));
      memcpy(node->ilist, fields.ilist_data, fields.ilist_len);
    }
    if (fields.ilist_len == 0) return DB_SUCCESS_LOCKED_REC;
  }

  if (this->total_memory)
  {
    if (is_word_init)
    {
      *this->total_memory+=
         sizeof(fts_word_t) + sizeof(ib_alloc_t) +
         sizeof(ib_vector_t) + fields.word_len +
         sizeof(fts_node_t) * FTS_WORD_NODES_INIT_SIZE;
    }
    *this->total_memory += node->ilist_size;
    if (*this->total_memory >= fts_result_cache_limit)
      return DB_FTS_EXCEED_RESULT_CACHE_LIMIT;
  }
  return DB_SUCCESS;
}

/** AuxRecordReader comparison logic implementation */
RecordCompareAction AuxRecordReader::compare_record(
  const dtuple_t* search_tuple, const rec_t* rec,
  const dict_index_t* index) noexcept
{
  if (!search_tuple) return RecordCompareAction::PROCESS;
  int cmp_result;
  switch (compare_mode)
  {
    case AuxCompareMode::GREATER_EQUAL:
    case AuxCompareMode::GREATER:
    {
      int match= 0;
      cmp_result= cmp_dtuple_rec_bytes(rec, *index, *search_tuple, &match,
                                       index->table->not_redundant());
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
      AuxRecordFields fields;
      if (!extract_aux_fields(rec, index, fields, true))
        return RecordCompareAction::SKIP;

      const dfield_t* search_field= dtuple_get_nth_field(search_tuple, 0);
      const void* search_data= dfield_get_data(search_field);
      ulint search_len= dfield_get_len(search_field);
      if (!search_data || search_len == UNIV_SQL_NULL)
        return RecordCompareAction::PROCESS;
      if (!fields.word_data || fields.word_len == UNIV_SQL_NULL)
        return RecordCompareAction::SKIP;

      const dtype_t* type= dfield_get_type(search_field);
      cmp_result= cmp_data(type->mtype, type->prtype, false,
                           static_cast<const byte*>(search_data),
                           search_len, fields.word_data, fields.word_len);

      if (compare_mode == AuxCompareMode::EQUAL)
        return cmp_result == 0
               ? RecordCompareAction::PROCESS
               : RecordCompareAction::STOP;
      else /* AuxCompareMode::LIKE */
      {
        int prefix_cmp= cmp_data(type->mtype, type->prtype, false,
                                 static_cast<const byte*>(search_data),
                                 search_len, fields.word_data,
                                 search_len <= fields.word_len ? search_len : fields.word_len);

        if (prefix_cmp != 0) return RecordCompareAction::STOP;
        return (search_len <= fields.word_len) ? RecordCompareAction::PROCESS
                                               : RecordCompareAction::SKIP;
      }
    }
  }
  return RecordCompareAction::PROCESS;
}

bool ConfigReader::extract_config_fields(
  const rec_t* rec, const dict_index_t* index,
  const byte** key_data, ulint* key_len,
  const byte** value_data, ulint* value_len)
{
  bool comp= dict_table_is_comp(index->table);
  if (comp)
  {
    const byte* lens=
      rec - REC_N_NEW_EXTRA_BYTES - 1 - index->n_core_null_bytes;
    if (key_data && key_len)
    {
      *key_len = *lens;
      if (*key_len & 0x80)
        *key_len = ((*key_len & 0x3f) << 8) | *(lens - 1);
      *key_data = rec;
    }
    if (value_data && value_len)
    {
      ulint key_field_len = *lens;
      if (key_field_len & 0x80)
        key_field_len = ((key_field_len & 0x3f) << 8) | *(lens - 1);

      const byte* value_start = rec + key_field_len + DATA_TRX_ID_LEN +
                                DATA_ROLL_PTR_LEN;
      const byte* value_lens = lens;
      if (key_field_len & 0x80) value_lens--; /* Skip extra key length byte */
      value_lens--; /* Skip to field 3 length */
      *value_len = *value_lens;
      if (*value_len & 0x80)
        *value_len = ((*value_len & 0x3f) << 8) | *(value_lens - 1);
      *value_data = value_start;
    }
    return true;
  }
  else
  {
    if (key_data && key_len)
    {
      *key_data= rec_get_nth_field_old(rec, 0, key_len);
      if (!*key_data || *key_len == UNIV_SQL_NULL) return false;
    }  
    if (value_data && value_len)
    {
      *value_data= rec_get_nth_field_old(rec, 3, value_len);
      if (!*value_data || *value_len == UNIV_SQL_NULL) return false;
    }
    return true;
  }
}

bool CommonTableReader::extract_common_fields(
  const rec_t* rec, const dict_index_t* index,
  doc_id_t* doc_id)
{

  if (!dict_table_is_comp(index->table))
  {
    ulint doc_id_len;
    ulint offset= rec_get_nth_field_offs_old(rec, 0, &doc_id_len);
    if (offset != 0 || doc_id_len == UNIV_SQL_NULL || doc_id_len != 8)
      return false;
  }

  *doc_id= mach_read_from_8(rec);
  return true;
}

/** Direct config key comparison implementation */
RecordCompareAction ConfigReader::compare_config_key(
  const dtuple_t *search_tuple, const rec_t *rec,
  const dict_index_t *index)
{
  if (!search_tuple) return RecordCompareAction::PROCESS;
  const dfield_t *search_field = dtuple_get_nth_field(search_tuple, 0);
  const void *search_data = dfield_get_data(search_field);
  ulint search_len = dfield_get_len(search_field);
  if (!search_data || search_len == UNIV_SQL_NULL)
    return RecordCompareAction::PROCESS;

  const byte *rec_key_data;
  ulint rec_key_len;
  if (!extract_config_fields(rec, index, &rec_key_data, &rec_key_len))
    return RecordCompareAction::SKIP;

  const dtype_t *type = dfield_get_type(search_field);
  int cmp_result = cmp_data(type->mtype, type->prtype, false,
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
  memcpy(table_name += dbname_len, "FTS_", 4);
  table_name+= 4;

  int len= fts_write_object_id(m_table->id, table_name);
  if (!common_table)
  {
    table_name[len]= '_';
    ++len;
    len+= fts_write_object_id(m_index->id, table_name + len);
  }
  ut_a(len >= 16);
  ut_a(len < FTS_AUX_MIN_TABLE_ID_LENGTH);
  table_name+= len;
  *table_name++= '_';
  strcpy(table_name, suffix);
}
