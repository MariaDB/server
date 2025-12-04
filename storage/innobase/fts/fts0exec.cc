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
#include "ha_prototypes.h"

/** Defined in fts0fts.cc */
extern const char* fts_common_tables[];

/** Find common table index by name */
uint8_t find_common_table(const char* tbl_name)
{
  for (uint8_t i= 0; fts_common_tables[i]; i++)
    if (!strcmp(tbl_name, fts_common_tables[i])) return i;
  return UINT8_MAX;
}

FTSQueryExecutor::FTSQueryExecutor(
  trx_t *trx, const dict_index_t *fts_index, const dict_table_t *fts_table,
  bool dict_locked) : m_executor(new QueryExecutor(trx)),
                      m_dict_locked(dict_locked), m_fts_index(fts_index),
                      m_fts_table(fts_table)
{
  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX; i++)
    m_aux_tables[i] = nullptr;

  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX - 1; i++)
    m_common_tables[i] = nullptr;
}

FTSQueryExecutor::~FTSQueryExecutor()
{
  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX; i++)
    if (m_aux_tables[i]) m_aux_tables[i]->release();

  for (uint8_t i = 0; i < FTS_NUM_AUX_INDEX - 1; i++)
    if (m_common_tables[i]) m_common_tables[i]->release();
  delete m_executor;
}

dberr_t FTSQueryExecutor::open_aux_table(uint8_t aux_index) noexcept
{
  if (m_aux_tables[aux_index]) return DB_SUCCESS;
  fts_table_t fts_table;
  FTS_INIT_INDEX_TABLE(&fts_table, nullptr, FTS_INDEX_TABLE, m_fts_index);
  fts_table.suffix= fts_get_suffix(aux_index);

  char table_name[MAX_FULL_NAME_LEN];
  fts_get_table_name(&fts_table, table_name, m_dict_locked);

  m_aux_tables[aux_index]= dict_table_open_on_name(
    table_name, m_dict_locked, DICT_ERR_IGNORE_TABLESPACE);
  return m_aux_tables[aux_index] ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::open_common_table(const char *tbl_name) noexcept
{
  uint8_t index= find_common_table(tbl_name);
  if (index == UINT8_MAX) return DB_ERROR;
  if (m_common_tables[index]) return DB_SUCCESS;
  fts_table_t fts_table;
  FTS_INIT_FTS_TABLE(&fts_table, nullptr, FTS_COMMON_TABLE, m_fts_table);
  fts_table.suffix= tbl_name;
  char table_name[MAX_FULL_NAME_LEN];
  fts_get_table_name(&fts_table, table_name, m_dict_locked);

  m_common_tables[index]= dict_table_open_on_name(
    table_name, m_dict_locked, DICT_ERR_IGNORE_TABLESPACE);
  return m_common_tables[index] ? DB_SUCCESS : DB_TABLE_NOT_FOUND;
}

dberr_t FTSQueryExecutor::lock_aux_tables(uint8_t aux_index,
                                          lock_mode mode) noexcept
{
  dict_table_t *table= m_aux_tables[aux_index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  dberr_t err= m_executor->lock_table(table, mode);
  if (err == DB_LOCK_WAIT) err= m_executor->handle_wait(err, true);
  return err;
}

dberr_t FTSQueryExecutor::lock_common_tables(uint8_t index,
                                             lock_mode mode) noexcept
{
  dict_table_t *table= m_common_tables[index];
  if (table == nullptr) return DB_TABLE_NOT_FOUND;
  dberr_t err = m_executor->lock_table(table, mode);
  if (err == DB_LOCK_WAIT) err= m_executor->handle_wait(err, true);
  return err;
}

dberr_t FTSQueryExecutor::insert_aux_record(
  uint8_t aux_index, const fts_aux_data_t* aux_data) noexcept
{
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

  return m_executor->insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::insert_common_record(
  const char *tbl_name, doc_id_t doc_id) noexcept
{
  dberr_t err= open_common_table(tbl_name);
  if (err != DB_SUCCESS) return err;
  uint8_t index_no= find_common_table(tbl_name);
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

  return m_executor->insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::insert_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
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

  return m_executor->insert_record(table, &tuple);
}

dberr_t FTSQueryExecutor::update_config_record(
  const char *key, const char *value) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
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
  upd_field.field_no = 3;
  upd_field.orig_len = 0;
  upd_field.exp = nullptr;
  dfield_set_data(&upd_field.new_val, value, strlen(value));
  dict_col_copy_type(dict_index_get_nth_col(index, 3),
                     dfield_get_type(&upd_field.new_val));

  upd_t update;
  update.heap = nullptr;
  update.info_bits = 0;
  update.old_vrow = nullptr;
  update.n_fields = 1;
  update.fields = &upd_field;

  return m_executor->replace_record(table, &search_tuple, &update,
                                    &insert_tuple);
}

dberr_t FTSQueryExecutor::delete_aux_record(
  uint8_t aux_index, const fts_aux_data_t* aux_data) noexcept
{
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

  return m_executor->delete_record(table, &tuple);
}

dberr_t FTSQueryExecutor::delete_common_record(
  const char *table_name, doc_id_t doc_id) noexcept
{
  dberr_t err= open_common_table(table_name);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= find_common_table(table_name);
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

  return m_executor->delete_record(table, &tuple);
}

dberr_t FTSQueryExecutor::delete_all_common_records(
  const char *table_name) noexcept
{
  dberr_t err= open_common_table(table_name);
  if (err != DB_SUCCESS) return err;

  uint8_t cached_index= find_common_table(table_name);
  err= lock_common_tables(cached_index, LOCK_X);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[cached_index];
  return m_executor->delete_all(table);
}

dberr_t FTSQueryExecutor::delete_config_record(
  const char *key) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");
  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
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

  return m_executor->delete_record(table, &tuple);
}

dberr_t FTSQueryExecutor::read_config_with_lock(const char *key,
                                               RecordCallback& callback) noexcept
{
  dberr_t err= open_common_table("CONFIG");
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table("CONFIG");

  err= lock_common_tables(index_no, LOCK_IX);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
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

  return m_executor->select_for_update(table, &tuple, &callback);
}

dberr_t FTSQueryExecutor::read_aux(uint8_t aux_index,
                                   const char *word,
                                   page_cur_mode_t mode,
                                   RecordCallback& callback) noexcept
{
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

  return m_executor->read(table, &tuple, mode, callback);
}

dberr_t FTSQueryExecutor::read_aux_all(uint8_t aux_index, RecordCallback& callback) noexcept
{
  if (aux_index >= FTS_NUM_AUX_INDEX) return DB_ERROR;
  dberr_t err= open_aux_table(aux_index);
  if (err != DB_SUCCESS) return err;

  err= lock_aux_tables(aux_index, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_aux_tables[aux_index];
  return m_executor->read(table, nullptr, PAGE_CUR_GE, callback);
}

dberr_t FTSQueryExecutor::read_all_common(const char *tbl_name,
                                          RecordCallback& callback) noexcept
{
  dberr_t err= open_common_table(tbl_name);
  if (err != DB_SUCCESS) return err;

  uint8_t index_no= find_common_table(tbl_name);
  err= lock_common_tables(index_no, LOCK_IS);
  if (err != DB_SUCCESS) return err;

  dict_table_t* table= m_common_tables[index_no];
  return m_executor->read(table, nullptr, PAGE_CUR_GE, callback);
}

CommonTableReader::CommonTableReader() : RecordCallback(
  [this](const rec_t* rec, const dict_index_t* index,
         const rec_offs* offsets) -> bool
  {
    ulint len;
    const byte* id_data= rec_get_nth_field(rec, offsets, 0, &len);
    if (id_data && len != UNIV_SQL_NULL && len == 8)
    {
      doc_id_t doc_id= mach_read_from_8(id_data);
      doc_ids.push_back(doc_id);
    }
    return true;
  },
  [](const dtuple_t* search_tuple, const rec_t* rec,
     const dict_index_t* index, const rec_offs* offsets) -> RecordCompareAction
  { return RecordCompareAction::PROCESS; }) {}


ConfigReader::ConfigReader() : RecordCallback(
  [this](const rec_t* rec, const dict_index_t* index,
         const rec_offs* offsets) -> bool
  {
    ulint value_len;
    const byte *value_data= rec_get_nth_field(rec, offsets, 3, &value_len);

    if (value_data && value_len != UNIV_SQL_NULL && value_len > 0)
      value_span= span<const char>(
        reinterpret_cast<const char*>(value_data), value_len);
    return false;
  },
  [](const dtuple_t* search_tuple, const rec_t* rec,
     const dict_index_t* index, const rec_offs* offsets) -> RecordCompareAction
  {
    if (!search_tuple) return RecordCompareAction::PROCESS;
    uint16_t matched_fields= 0;
    int cmp_result= cmp_dtuple_rec_with_match(search_tuple, rec, index,
                                              offsets, &matched_fields);
    return (cmp_result == 0) ? RecordCompareAction::PROCESS
                             : RecordCompareAction::STOP;
  }) {}

/** Initial size of nodes in fts_word_t. */
static const ulint FTS_WORD_NODES_INIT_SIZE = 64;

/** Initialize fts_word_t structure */
static void init_fts_word(fts_word_t* word, const byte* utf8, ulint len)
{
  mem_heap_t* heap = mem_heap_create(sizeof(fts_node_t));
  memset(word, 0, sizeof(*word));
  word->text.f_len = len;
  word->text.f_str = static_cast<byte*>(mem_heap_alloc(heap, len + 1));
  memcpy(word->text.f_str, utf8, len);
  word->text.f_str[len] = 0;
  word->heap_alloc = ib_heap_allocator_create(heap);
  word->nodes = ib_vector_create(word->heap_alloc, sizeof(fts_node_t),
                                 FTS_WORD_NODES_INIT_SIZE);
}

/** AuxRecordReader default word processor implementation */
bool AuxRecordReader::default_word_processor(
  const rec_t* rec, const dict_index_t* index,
  const rec_offs* offsets, void* user_arg)
{
  ib_vector_t *words= static_cast<ib_vector_t*>(user_arg);
  ulint word_len;
  const byte *word_data= rec_get_nth_field(rec, offsets, 0, &word_len);
  fts_word_t *word;
  bool is_word_init = false;
  if (!word_data || word_len == UNIV_SQL_NULL || word_len > FTS_MAX_WORD_LEN)
    return true;

  ut_ad(word_len <= FTS_MAX_WORD_LEN);

  if (ib_vector_size(words) == 0)
  {
    /* First word - push and initialize */
    word = static_cast<fts_word_t*>(ib_vector_push(words, nullptr));
    init_fts_word(word, word_data, word_len);
    is_word_init = true;
  }
  else
  {
    /* Check if this word is different from the last word */
    word = static_cast<fts_word_t*>(ib_vector_last(words));
    if (word_len != word->text.f_len ||
        memcmp(word->text.f_str, word_data, word_len))
    {
      /* Different word - push new word and initialize */
      word = static_cast<fts_word_t*>(ib_vector_push(words, nullptr));
      init_fts_word(word, word_data, word_len);
      is_word_init = true;
    }
  }
  fts_node_t *node= static_cast<fts_node_t*>(
    ib_vector_push(word->nodes, nullptr));

  ulint doc_id_len;
  const byte *doc_id_data= rec_get_nth_field(rec, offsets, 1, &doc_id_len);
  if (doc_id_data && doc_id_len == 8)
    node->first_doc_id= fts_read_doc_id(doc_id_data);
  else node->first_doc_id= 0;

  /* Read last_doc_id (field 4) */
  doc_id_data= rec_get_nth_field(rec, offsets, 4, &doc_id_len);
  if (doc_id_data && doc_id_len == 8)
    node->last_doc_id= fts_read_doc_id(doc_id_data);
  else node->last_doc_id= 0;

  /* Read doc_count (field 5) */
  ulint doc_count_len;
  const byte *doc_count_data= rec_get_nth_field(rec, offsets, 5,
                                                &doc_count_len);
  if (doc_count_data && doc_count_len == 4)
    node->doc_count= mach_read_from_4(doc_count_data);
  else node->doc_count= 0;

  /* Read ilist (field 6) with external BLOB support */
  ulint ilist_len= 0;
  const byte *ilist_data= rec_get_nth_field(rec, offsets, 6, &ilist_len);
  byte *external_data= nullptr;
  mem_heap_t *temp_heap= nullptr;

  node->ilist_size_alloc= node->ilist_size= 0;
  node->ilist= nullptr;

  if (ilist_data && ilist_len != UNIV_SQL_NULL && ilist_len > 0)
  {
    if (rec_offs_nth_extern(offsets, 6))
    {
      temp_heap= mem_heap_create(ilist_len);
      ulint external_len;
      external_data= btr_copy_externally_stored_field(
        &external_len, ilist_data, index->table->space->zip_size(),
        ilist_len, temp_heap);
      if (external_data)
      {
        ilist_data= external_data;
	ilist_len= external_len;
      }
    }
    node->ilist_size_alloc= node->ilist_size= ilist_len;
    if (ilist_len)
    {
      node->ilist= static_cast<byte*>(ut_malloc_nokey(ilist_len));
      memcpy(node->ilist, ilist_data, ilist_len);
    }
    if (temp_heap) mem_heap_free(temp_heap);
    if (ilist_len == 0) return false;
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
    *this->total_memory += node->ilist_size;
    if (*this->total_memory >= fts_result_cache_limit)
      return false;
  }
  return true;
}

/** AuxRecordReader comparison logic implementation */
RecordCompareAction AuxRecordReader::compare_record(
  const dtuple_t* search_tuple, const rec_t* rec,
  const dict_index_t* index, const rec_offs* offsets) noexcept
{
  if (!search_tuple) return RecordCompareAction::PROCESS;
  const dfield_t* search_field= dtuple_get_nth_field(search_tuple, 0);
  const void* search_data= dfield_get_data(search_field);
  ulint search_len= dfield_get_len(search_field);

  ulint rec_len;
  const byte* rec_data= rec_get_nth_field(rec, offsets, 0, &rec_len);

  if (!rec_data || rec_len == UNIV_SQL_NULL)
    return RecordCompareAction::SKIP;
  if (!search_data || search_len == UNIV_SQL_NULL)
    return RecordCompareAction::PROCESS;
  int cmp_result;
  switch (compare_mode)
  {
    case AuxCompareMode::GREATER_EQUAL:
    case AuxCompareMode::GREATER:
    {
      uint16_t matched_fields= 0;
      cmp_result= cmp_dtuple_rec_with_match(search_tuple, rec, index,
                                            offsets, &matched_fields);
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
      /* Use charset-aware comparison for LIKE and EQUAL modes */
      const dtype_t* type= dfield_get_type(search_field);
      cmp_result= cmp_data(type->mtype, type->prtype, false,
                           static_cast<const byte*>(search_data),
                           search_len, rec_data, rec_len);

      if (compare_mode == AuxCompareMode::EQUAL)
        return cmp_result == 0
               ? RecordCompareAction::PROCESS
               : RecordCompareAction::STOP;
      else /* AuxCompareMode::LIKE */
      {
        /* For LIKE mode, compare only the prefix (search_len bytes) */
        int prefix_cmp = cmp_data(type->mtype, type->prtype, false,
                                  static_cast<const byte*>(search_data),
                                  search_len, rec_data,
                                  search_len <= rec_len ? search_len : rec_len);

        if (prefix_cmp != 0) return RecordCompareAction::STOP;
        return (search_len <= rec_len) ? RecordCompareAction::PROCESS
                                       : RecordCompareAction::SKIP;
      }
    }
  }
  return RecordCompareAction::PROCESS;
}
