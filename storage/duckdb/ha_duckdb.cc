/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#include "ha_duckdb.h"

#include <algorithm>
#include <cctype>

#include <my_global.h>
#include <mysql/plugin.h>
#include <mysql/plugin_function.h>
#include "sql_class.h"
#include "sql_table.h"

#include "duckdb_manager.h"
#include "duckdb_context.h"
#include "duckdb_query.h"
#include "duckdb_config.h"
#include "duckdb_timezone.h"
#include "duckdb_types.h"
#include "duckdb_select.h"
#include "ddl_convertor.h"
#include "dml_convertor.h"
#include "delta_appender.h"
#include "row_helpers.h"
#include "ha_duckdb_pushdown.h"
#include "duckdb_log.h"

/* Global status counters */
struct duckdb_status_t
{
  ulonglong duckdb_rows_insert;
  ulonglong duckdb_rows_update;
  ulonglong duckdb_rows_delete;
  ulonglong duckdb_rows_insert_in_batch;
  ulonglong duckdb_rows_update_in_batch;
  ulonglong duckdb_rows_delete_in_batch;
  ulonglong duckdb_commit;
  ulonglong duckdb_rollback;
};

static duckdb_status_t srv_duckdb_status;

/* Plugin variables */
static my_bool copy_ddl_in_batch= TRUE;
static my_bool dml_in_batch= TRUE;
static my_bool update_modified_column_only= TRUE;

static handler *duckdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root);

handlerton *duckdb_hton;

/* ----- Per-thread context via thd_get_ha_data / thd_set_ha_data ----- */

static myduck::DuckdbThdContext *get_duckdb_context(THD *thd)
{
  auto *ctx= static_cast<myduck::DuckdbThdContext *>(
      thd_get_ha_data(thd, duckdb_hton));
  if (!ctx)
  {
    ctx= new myduck::DuckdbThdContext();
    thd_set_ha_data(thd, duckdb_hton, ctx);
  }
  return ctx;
}

/* ----- Transaction callbacks ----- */

#if MYSQL_VERSION_ID >= 110800
static int duckdb_prepare(THD *thd, bool all)
#else
static int duckdb_prepare(handlerton *hton, THD *thd, bool all)
#endif
{
  if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
  {
    std::string error_msg;
    auto *ctx= get_duckdb_context(thd);
    if (ctx->flush_appenders(error_msg))
    {
      my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, error_msg.c_str(), "DuckDB");
      return 1;
    }
  }
  return 0;
}

static void push_duckdb_query_error(const std::string &err)
{
  if (err.find("Parser Error") != std::string::npos ||
      err.find("syntax error") != std::string::npos)
  {
    my_error(ER_PARSE_ERROR, MYF(0), err.c_str());
    return;
  }

  my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_GENERIC, err.c_str(), "DuckDB");
}

#if MYSQL_VERSION_ID >= 110800
static int duckdb_commit(THD *thd, bool commit_trx)
#else
static int duckdb_commit(handlerton *hton, THD *thd, bool commit_trx)
#endif
{
  if (commit_trx ||
      (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)))
  {
    srv_duckdb_status.duckdb_commit++;

    std::string error_msg;
    auto *ctx= get_duckdb_context(thd);

    /* Safety net: flush if prepare() was not called (no 2PC).
       This is a no-op when appenders were already flushed. */
    if (ctx->flush_appenders(error_msg))
    {
      my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, error_msg.c_str(), "DuckDB");
      ctx->duckdb_trans_rollback(error_msg);
      return 1;
    }

    if (ctx->duckdb_trans_commit(error_msg))
    {
      my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_GENERIC, error_msg.c_str(), "DuckDB");
      ctx->duckdb_trans_rollback(error_msg);
      return 1;
    }
  }
  return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int duckdb_rollback(THD *thd, bool rollback_trx)
#else
static int duckdb_rollback(handlerton *hton, THD *thd, bool rollback_trx)
#endif
{
  if (rollback_trx ||
      !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
  {
    srv_duckdb_status.duckdb_rollback++;

    std::string error_msg;
    auto *ctx= get_duckdb_context(thd);
    if (ctx->duckdb_trans_rollback(error_msg))
    {
      my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_GENERIC, error_msg.c_str(), "DuckDB");
      return 1;
    }
  }
  return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int duckdb_close_connection(THD *thd)
#else
static int duckdb_close_connection(handlerton *hton, THD *thd)
#endif
{
  auto *ctx= static_cast<myduck::DuckdbThdContext *>(
      thd_get_ha_data(thd, duckdb_hton));
  if (ctx)
  {
    delete ctx;
    thd_set_ha_data(thd, duckdb_hton, nullptr);
  }
  return 0;
}

static int duckdb_register_trx(THD *thd)
{
  trans_register_ha(thd, false, duckdb_hton, 0);

  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    trans_register_ha(thd, true, duckdb_hton, 0);

  auto *ctx= get_duckdb_context(thd);
  if (!ctx->has_transaction())
    ctx->duckdb_trans_begin();
  return 0;
}

static void duckdb_drop_database(handlerton *hton, char *path)
{
  THD *thd= current_thd;
  DBUG_ENTER("duckdb_drop_database");

  Databasename db(path);

  std::string query= "DROP SCHEMA IF EXISTS \"";
  query.append(db.name);
  query.append("\"");

  duckdb_register_trx(thd);
  auto *ctx= get_duckdb_context(thd);
  auto query_result= myduck::duckdb_query(ctx->get_connection(), query);
  DBUG_VOID_RETURN;
}

/* ----- Handlerton init ----- */

static int duckdb_init_func(void *p)
{
  DBUG_ENTER("duckdb_init_func");

  duckdb_hton= (handlerton *) p;
  duckdb_hton->db_type= DB_TYPE_AUTOASSIGN;
  duckdb_hton->create= duckdb_create_handler;
  duckdb_hton->flags= HTON_NO_FLAGS;
  duckdb_hton->prepare= duckdb_prepare;
  duckdb_hton->commit= duckdb_commit;
  duckdb_hton->rollback= duckdb_rollback;
  duckdb_hton->close_connection= duckdb_close_connection;
  duckdb_hton->drop_database= duckdb_drop_database;

  duckdb_hton->create_select= create_duckdb_select_handler;
  duckdb_hton->create_unit= create_duckdb_unit_handler;

  myduck::TimeZoneOffsetHelper::init_timezone();

  if (myduck::DuckdbManager::CreateInstance())
  {
    sql_print_error("DuckDB: failed to create DuckdbManager instance");
    DBUG_RETURN(1);
  }

  sql_print_information("DuckDB storage engine initialized");
  DBUG_RETURN(0);
}

static int duckdb_deinit_func(void *p)
{
  DBUG_ENTER("duckdb_deinit_func");
  myduck::DuckdbManager::Cleanup();
  DBUG_RETURN(0);
}

/* ----- Share management ----- */

Duckdb_share::Duckdb_share() { thr_lock_init(&lock); }

Duckdb_share *ha_duckdb::get_share()
{
  Duckdb_share *tmp_share;
  DBUG_ENTER("ha_duckdb::get_share()");

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<Duckdb_share *>(get_ha_share_ptr())))
  {
    tmp_share= new Duckdb_share;
    if (!tmp_share)
      goto err;
    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}

/* ----- Handler creation ----- */

static handler *duckdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      MEM_ROOT *mem_root)
{
  return new (mem_root) ha_duckdb(hton, table);
}

ha_duckdb::ha_duckdb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg)
{
  my_bitmap_init(&m_blob_map, nullptr, MAX_FIELDS);
}

ha_duckdb::~ha_duckdb() { my_bitmap_free(&m_blob_map); }

/* ----- Basic handler methods ----- */

int ha_duckdb::open(const char *, int, uint)
{
  DBUG_ENTER("ha_duckdb::open");

  if (!(share= get_share()))
    DBUG_RETURN(1);
  thr_lock_data_init(&share->lock, &lock, nullptr);

  DBUG_RETURN(0);
}

int ha_duckdb::close(void)
{
  DBUG_ENTER("ha_duckdb::close");
  DBUG_RETURN(0);
}

/* ----- DML helpers ----- */

static int execute_dml(THD *thd, DMLConvertor *convertor)
{
  if (convertor->check())
    return HA_DUCKDB_DML_ERROR;

  auto query= convertor->translate();
  auto *ctx= get_duckdb_context(thd);
  auto query_result= myduck::duckdb_query(ctx->get_connection(), query);

  if (query_result->HasError())
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_DML_ERROR, query_result->GetError().c_str(), "DuckDB");
    return HA_DUCKDB_DML_ERROR;
  }

  return 0;
}

/* check whether field is modified */
static bool calc_field_difference(const uchar *old_row, const uchar *new_row,
                                  TABLE *table, Field *field)
{
  ulong o_len;
  ulong n_len;
  const uchar *o_ptr;
  const uchar *n_ptr;

  o_ptr= (const uchar *) old_row + field->offset(table->record[0]);
  n_ptr= (const uchar *) new_row + field->offset(table->record[0]);

  o_len= n_len= field->pack_length();

  switch (field->type())
  {
  case MYSQL_TYPE_VARCHAR:
    o_ptr= row_mysql_read_true_varchar(
        &o_len, o_ptr, (ulong) ((Field_varstring *) field)->length_bytes);
    n_ptr= row_mysql_read_true_varchar(
        &n_len, n_ptr, (ulong) ((Field_varstring *) field)->length_bytes);
    break;
  case MYSQL_TYPE_GEOMETRY:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    o_ptr= row_mysql_read_blob_ref(&o_len, o_ptr, o_len);
    n_ptr= row_mysql_read_blob_ref(&n_len, n_ptr, n_len);
    break;
  default:;
  }

  if (field->real_maybe_null())
  {
    if (field->is_null_in_record(old_row))
      o_len= ~0U;
    if (field->is_null_in_record(new_row))
      n_len= ~0U;
  }

  return o_len != n_len ||
         (o_len != ~0U && o_len != 0 && 0 != memcmp(o_ptr, n_ptr, o_len));
}

/* calculate row difference, set bit for modified columns in table->tmp_set */
static bool calc_row_difference(const uchar *old_row, const uchar *new_row,
                                TABLE *table)
{
  bool res= false;
  bitmap_clear_all(&table->tmp_set);

  for (uint i= 0; i < table->s->fields; i++)
  {
    Field *field= table->field[i];
    if (calc_field_difference(old_row, new_row, table, field))
    {
      bitmap_set_bit(&table->tmp_set, field->field_index);
      res= true;
    }
  }
  return res;
}

/* check whether PK is modified */
static bool calc_pk_difference(const uchar *old_row, const uchar *new_row,
                               TABLE *table) __attribute__((unused));
static bool calc_pk_difference(const uchar *old_row, const uchar *new_row,
                               TABLE *table)
{
  KEY *key_info= table->key_info;
  if (!key_info)
    return false;

  KEY_PART_INFO *key_part= table->key_info->key_part;
  for (uint j= 0; j < key_info->user_defined_key_parts; j++, key_part++)
  {
    if (calc_field_difference(old_row, new_row, table, key_part->field))
      return true;
  }
  return false;
}

static myduck::BatchState get_batch_state(THD *thd)
{
  auto *ctx= get_duckdb_context(thd);
  myduck::BatchState batch_state= ctx->get_batch_state();

  if (batch_state == myduck::BatchState::UNDEFINED)
  {
    if (dml_in_batch)
      batch_state= myduck::BatchState::IN_INSERT_ONLY_BATCH;
    else
      batch_state= myduck::BatchState::NOT_IN_BATCH;
    ctx->set_batch_state(batch_state);
  }
  return batch_state;
}

/* Build duckdb type map of blob type */
static void build_duckdb_blob_map(Field **field_list, MY_BITMAP *map)
{
  for (Field **f_ptr= field_list; *f_ptr != nullptr; f_ptr++)
  {
    Field *field= *f_ptr;
    enum_field_types type= field->real_type();

    if (type == MYSQL_TYPE_SET || type == MYSQL_TYPE_ENUM ||
        type == MYSQL_TYPE_BIT || type == MYSQL_TYPE_GEOMETRY ||
        type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_STRING ||
        type == MYSQL_TYPE_TINY_BLOB || type == MYSQL_TYPE_BLOB ||
        type == MYSQL_TYPE_MEDIUM_BLOB || type == MYSQL_TYPE_LONG_BLOB)
    {
      if (FieldConvertor::convert_type(field) == "BLOB")
        bitmap_set_bit(map, field->field_index);
    }
  }
}

/* ----- DML operations ----- */

int ha_duckdb::write_row(const uchar *)
{
  DBUG_ENTER("ha_duckdb::write_row");
  int ret= 0;
  THD *thd= ha_thd();

  DBUG_ASSERT(table_share != nullptr && table != nullptr);
  MY_BITMAP *org_bitmap= dbug_tmp_use_all_columns(table, &table->read_set);

  ret= duckdb_register_trx(thd);
  if (ret)
  {
    dbug_tmp_restore_column_map(&table->read_set, org_bitmap);
    DBUG_RETURN(ret);
  }

  myduck::BatchState batch_state= get_batch_state(thd);

  if (batch_state == myduck::BatchState::NOT_IN_BATCH)
  {
    InsertConvertor convertor(table, false);
    ret= execute_dml(thd, &convertor);
    if (ret == 0)
      srv_duckdb_status.duckdb_rows_insert++;
  }
  else
  {
    if (m_first_write)
    {
      build_duckdb_blob_map(table->field, &m_blob_map);
      m_first_write= false;
    }
    auto *ctx= get_duckdb_context(thd);
    ret= ctx->append_row_insert(table, &m_blob_map);
    if (ret == 0)
      srv_duckdb_status.duckdb_rows_insert_in_batch++;
    else
    {
      /* Appender failed (e.g. table not yet created during ALTER TABLE).
         Fall back to non-batch SQL insert. */
      ctx->set_batch_state(myduck::BatchState::NOT_IN_BATCH);
      InsertConvertor convertor(table, false);
      ret= execute_dml(thd, &convertor);
      if (ret == 0)
        srv_duckdb_status.duckdb_rows_insert++;
    }
  }

  dbug_tmp_restore_column_map(&table->read_set, org_bitmap);
  DBUG_RETURN(ret);
}

int ha_duckdb::update_row(const uchar *old_row, const uchar *new_row)
{
  DBUG_ENTER("ha_duckdb::update_row");
  int ret= 0;
  THD *thd= ha_thd();

  ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  myduck::BatchState batch_state= get_batch_state(thd);

  if (batch_state == myduck::BatchState::NOT_IN_BATCH)
  {
    if (update_modified_column_only &&
        calc_row_difference(old_row, new_row, table))
    {
      bitmap_copy(table->write_set, &table->tmp_set);
    }
    bitmap_clear_all(&table->tmp_set);

    UpdateConvertor update_convertor(table, old_row);
    ret= execute_dml(thd, &update_convertor);
    if (ret == 0)
      srv_duckdb_status.duckdb_rows_update++;
  }
  else
  {
    auto *ctx= get_duckdb_context(thd);
    ret= ctx->append_row_update(table, old_row);
    if (ret == 0)
      srv_duckdb_status.duckdb_rows_update_in_batch++;
    else
    {
      ctx->set_batch_state(myduck::BatchState::NOT_IN_BATCH);
      UpdateConvertor update_convertor(table, old_row);
      ret= execute_dml(thd, &update_convertor);
      if (ret == 0)
        srv_duckdb_status.duckdb_rows_update++;
    }
  }

  DBUG_RETURN(ret);
}

int ha_duckdb::delete_row(const uchar *)
{
  DBUG_ENTER("ha_duckdb::delete_row");
  int ret= 0;
  THD *thd= ha_thd();

  ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  myduck::BatchState batch_state= get_batch_state(thd);

  if (batch_state == myduck::BatchState::NOT_IN_BATCH)
  {
    DeleteConvertor convertor(table);
    ret= execute_dml(thd, &convertor);
    if (ret == 0)
      srv_duckdb_status.duckdb_rows_delete++;
  }
  else
  {
    auto *ctx= get_duckdb_context(thd);
    ret= ctx->append_row_delete(table);
    if (ret == 0)
      srv_duckdb_status.duckdb_rows_delete_in_batch++;
    else
    {
      ctx->set_batch_state(myduck::BatchState::NOT_IN_BATCH);
      DeleteConvertor convertor(table);
      ret= execute_dml(thd, &convertor);
      if (ret == 0)
        srv_duckdb_status.duckdb_rows_delete++;
    }
  }

  DBUG_RETURN(ret);
}

/* ----- Index stubs (not supported) ----- */

int ha_duckdb::index_read_map(uchar *, const uchar *, key_part_map,
                              enum ha_rkey_function)
{
  DBUG_ENTER("ha_duckdb::index_read_map");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::index_next(uchar *)
{
  DBUG_ENTER("ha_duckdb::index_next");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::index_prev(uchar *)
{
  DBUG_ENTER("ha_duckdb::index_prev");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::index_first(uchar *)
{
  DBUG_ENTER("ha_duckdb::index_first");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::index_last(uchar *)
{
  DBUG_ENTER("ha_duckdb::index_last");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

/* ----- Table scan (rnd_*) ----- */

int ha_duckdb::rnd_init(bool)
{
  DBUG_ENTER("ha_duckdb::rnd_init");
  THD *thd= ha_thd();
  std::string schema_name;
  std::string table_name;

  if (table && table->s)
  {
    schema_name.assign(table->s->db.str, table->s->db.length);
    table_name.assign(table->s->table_name.str, table->s->table_name.length);
  }
  else
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  std::string query=
      "SELECT * FROM \"" + schema_name + "\".\"" + table_name + "\"";

  auto *ctx= get_duckdb_context(thd);
  query_result= myduck::duckdb_query(ctx->get_connection(), query);
  if (query_result->HasError())
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR, query_result->GetError().c_str(), "DuckDB");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  for (auto *field= table->field; *field; ++field)
    bitmap_set_bit(table->write_set, (*field)->field_index);

  DBUG_RETURN(0);
}

int ha_duckdb::rnd_end()
{
  DBUG_ENTER("ha_duckdb::rnd_end");
  query_result.reset();
  current_chunk.reset();
  DBUG_RETURN(0);
}

int ha_duckdb::rnd_next(uchar *buf)
{
  DBUG_ENTER("ha_duckdb::rnd_next");
  THD *thd= ha_thd();

  if (!query_result)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  memset(buf, 0, table->s->reclength);

  /* fetch new chunk when current chunk is empty */
  if (!current_chunk || current_row_index >= current_chunk->size())
  {
    current_chunk.reset();
    current_chunk= query_result->Fetch();

    if (!current_chunk)
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    current_row_index= 0;
  }

  /* store the fields of a tuple */
  for (size_t col_idx= 0; col_idx < current_chunk->ColumnCount(); ++col_idx)
  {
    duckdb::Value value= current_chunk->GetValue(col_idx, current_row_index);
    Field *field= table->field[col_idx];
    store_duckdb_field_in_mysql_format(field, value, thd);
  }

  /* update NULL field tag */
  if (table->s->null_bytes > 0)
  {
    if (table->null_flags)
      memcpy(buf, table->null_flags, table->s->null_bytes);
    else
      memset(buf, 0, table->s->null_bytes);
  }

  current_row_index++;
  DBUG_RETURN(0);
}

void ha_duckdb::position(const uchar *)
{
  DBUG_ENTER("ha_duckdb::position");
  DBUG_VOID_RETURN;
}

int ha_duckdb::rnd_pos(uchar *, uchar *)
{
  DBUG_ENTER("ha_duckdb::rnd_pos");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int ha_duckdb::info(uint flag)
{
  DBUG_ENTER("ha_duckdb::info");
  if (flag & HA_STATUS_VARIABLE)
  {
    /* Retrieve variable info, such as row counts and file lengths */
    stats.records= records();
    stats.deleted= 0;
    // stats.data_file_length =
    // stats.index_file_length =
    // stats.delete_length =
    stats.check_time= 0;
    // stats.mrr_length_per_rec =

    // stats.data_file_length may be unset for TIAMAT; avoid division by
    // garbage.
    if (stats.records == 0 || stats.data_file_length == 0)
      stats.mean_rec_length= 0;
    else
      stats.mean_rec_length= (ulong) (stats.data_file_length / stats.records);
  }

  DBUG_RETURN(0);
}

ha_rows ha_duckdb::records()
{
  DBUG_ENTER("ha_tiamat::records");
  // Optimizer may call records()/info() in contexts where ha_share isn't
  // initialized for this handler instance. Return a conservative estimate.
  if (stats.records)
    DBUG_RETURN(stats.records);
  DBUG_RETURN(10);
}

int ha_duckdb::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_duckdb::extra");
  THD *thd= ha_thd();
  auto *ctx= get_duckdb_context(thd);

  switch (operation)
  {
  case HA_EXTRA_BEGIN_COPY:
    ctx->set_in_copy_ddl(true);
    break;
  case HA_EXTRA_END_COPY:
  case HA_EXTRA_ABORT_COPY:
    ctx->set_in_copy_ddl(false);
    break;
  default:
    break;
  }
  DBUG_RETURN(0);
}

int ha_duckdb::delete_all_rows()
{
  DBUG_ENTER("ha_duckdb::delete_all_rows");
  int ret= 0;
  THD *thd= ha_thd();

  ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  auto *ctx= get_duckdb_context(thd);

  /* Discard any pending batch rows for this table */
  DatabaseTableNames dt(table->s->normalized_path.str);
  ctx->delete_appender(dt.db_name, dt.table_name);

  /* Execute DELETE FROM "schema"."table" */
  std::string query=
      "DELETE FROM \"" + dt.db_name + "\".\"" + dt.table_name + "\"";

  auto query_result= myduck::duckdb_query(ctx->get_connection(), query);
  if (query_result->HasError())
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_DML_ERROR, query_result->GetError().c_str(), "DuckDB");
    DBUG_RETURN(HA_DUCKDB_DML_ERROR);
  }

  DBUG_RETURN(0);
}

const COND *ha_duckdb::cond_push(const COND *cond)
{
  DBUG_ENTER("ha_duckdb::cond_push");
  /*
    Accept all conditions — DuckDB will evaluate the WHERE clause
    from the original SQL query in direct_delete_rows().
  */
  DBUG_RETURN(NULL);
}

int ha_duckdb::direct_delete_rows_init()
{
  DBUG_ENTER("ha_duckdb::direct_delete_rows_init");
  DBUG_RETURN(0);
}

int ha_duckdb::direct_delete_rows(ha_rows *delete_rows)
{
  DBUG_ENTER("ha_duckdb::direct_delete_rows");
  int ret= 0;
  THD *thd= ha_thd();

  ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  auto *ctx= get_duckdb_context(thd);

  /* Flush any pending batch rows so DuckDB sees consistent data */
  std::string error_msg;
  if (ctx->flush_appenders(error_msg))
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, error_msg.c_str(), "DuckDB");
    DBUG_RETURN(HA_DUCKDB_DML_ERROR);
  }

  /* Execute the original DELETE statement in DuckDB */
  LEX_STRING *qs= thd_query_string(thd);
  std::string query(qs->str, qs->length);
  auto result= myduck::duckdb_query(thd, query, true);
  if (result->HasError())
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_DML_ERROR, result->GetError().c_str(), "DuckDB");
    DBUG_RETURN(HA_DUCKDB_DML_ERROR);
  }

  /* DuckDB returns a single row with the count of affected rows */
  auto chunk= result->Fetch();
  if (chunk && chunk->size() > 0)
    *delete_rows= chunk->GetValue(0, 0).GetValue<int64_t>();
  else
    *delete_rows= 0;

  srv_duckdb_status.duckdb_rows_delete+= *delete_rows;

  DBUG_RETURN(0);
}

int ha_duckdb::direct_update_rows_init(List<Item> *update_fields
                                       __attribute__((unused)))
{
  DBUG_ENTER("ha_duckdb::direct_update_rows_init");
  DBUG_RETURN(0);
}

int ha_duckdb::direct_update_rows(ha_rows *update_rows, ha_rows *found_rows)
{
  DBUG_ENTER("ha_duckdb::direct_update_rows");
  int ret= 0;
  THD *thd= ha_thd();

  ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  auto *ctx= get_duckdb_context(thd);

  /* Flush any pending batch rows so DuckDB sees consistent data */
  std::string error_msg;
  if (ctx->flush_appenders(error_msg))
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_APPEND_ERROR, error_msg.c_str(), "DuckDB");
    DBUG_RETURN(HA_DUCKDB_DML_ERROR);
  }

  /* Execute the original UPDATE statement in DuckDB */
  LEX_STRING *qs= thd_query_string(thd);
  std::string query(qs->str, qs->length);
  auto result= myduck::duckdb_query(thd, query, true);
  if (result->HasError())
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_DML_ERROR, result->GetError().c_str(), "DuckDB");
    DBUG_RETURN(HA_DUCKDB_DML_ERROR);
  }

  /* DuckDB returns a single row with the count of affected rows */
  auto chunk= result->Fetch();
  ha_rows affected= 0;
  if (chunk && chunk->size() > 0)
    affected= chunk->GetValue(0, 0).GetValue<int64_t>();

  *update_rows= affected;
  *found_rows= affected;

  srv_duckdb_status.duckdb_rows_update+= affected;

  DBUG_RETURN(0);
}

int ha_duckdb::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_duckdb::external_lock");
  if (lock_type != F_UNLCK)
  {
    /* DuckDB does not support XA transactions. Reject DML early. */
    if (myduck::reject_xa_if_active(thd))
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);

    int ret= duckdb_register_trx(thd);
    if (ret)
      DBUG_RETURN(ret);
  }
  DBUG_RETURN(0);
}

uint ha_duckdb::lock_count(void) const { return 0; }

THR_LOCK_DATA **ha_duckdb::store_lock(THD *, THR_LOCK_DATA **to,
                                      enum thr_lock_type)
{
  return to;
}

ha_rows ha_duckdb::records_in_range(uint, const key_range *, const key_range *,
                                    page_range *)
{
  DBUG_ENTER("ha_duckdb::records_in_range");
  DBUG_RETURN(10);
}

/* ----- DDL operations ----- */

int ha_duckdb::create(const char *name, TABLE *form,
                      HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_duckdb::create");
  THD *thd= ha_thd();

  int ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  DatabaseTableNames dt(name);
  CreateTableConvertor convertor(thd, form, create_info, dt.db_name,
                                 dt.table_name);

  if (convertor.check())
    DBUG_RETURN(HA_DUCKDB_CREATE_ERROR);

  std::string query= convertor.translate();

  auto *ctx= get_duckdb_context(thd);
  auto query_result= myduck::duckdb_query(ctx->get_connection(), query);

  if (query_result->HasError())
  {
    push_duckdb_query_error(query_result->GetError());
    DBUG_RETURN(HA_DUCKDB_CREATE_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_duckdb::delete_table(const char *name)
{
  DBUG_ENTER("ha_duckdb::delete_table");
  THD *thd= ha_thd();

  int ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  DatabaseTableNames dt(name);

  std::string query=
      "DROP TABLE IF EXISTS \"" + dt.db_name + "\".\"" + dt.table_name + "\"";

  auto *ctx= get_duckdb_context(thd);
  auto query_result= myduck::duckdb_query(ctx->get_connection(), query);

  if (query_result == nullptr || query_result->HasError())
    DBUG_RETURN(HA_DUCKDB_DROP_TABLE_ERROR);

  DBUG_RETURN(0);
}

int ha_duckdb::rename_table(const char *from, const char *to)
{
  DBUG_ENTER("ha_duckdb::rename_table");
  THD *thd= ha_thd();

  int ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  DatabaseTableNames old_t(from);
  DatabaseTableNames new_t(to);

  auto convertor= std::make_unique<RenameTableConvertor>(
      old_t.db_name, old_t.table_name, new_t.db_name, new_t.table_name);

  if (convertor->check())
    DBUG_RETURN(HA_DUCKDB_RENAME_ERROR);

  std::string query= convertor->translate();

  auto *ctx= get_duckdb_context(thd);
  std::string error_msg;
  ret= ctx->flush_appenders(error_msg);
  if (ret)
    DBUG_RETURN(ret);

  auto query_result= myduck::duckdb_query(ctx->get_connection(), query);

  if (query_result->HasError())
    DBUG_RETURN(HA_DUCKDB_RENAME_ERROR);

  DBUG_RETURN(0);
}

int ha_duckdb::truncate()
{
  DBUG_ENTER("ha_duckdb::truncate");
  THD *thd= ha_thd();

  int ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(ret);

  std::string schema_name(table->s->db.str, table->s->db.length);
  std::string table_name(table->s->table_name.str,
                         table->s->table_name.length);

  std::ostringstream query;
  query << "USE \"" << schema_name << "\";";
  query << "TRUNCATE TABLE \"" << table_name << "\";";

  auto *ctx= get_duckdb_context(thd);
  auto query_result= myduck::duckdb_query(ctx->get_connection(), query.str());

  if (query_result->HasError())
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_DUCKDB_TRUNCATE_TABLE_ERROR, query_result->GetError().c_str(), "DuckDB");
    DBUG_RETURN(HA_DUCKDB_TRUNCATE_TABLE_ERROR);
  }

  DBUG_RETURN(0);
}

/* ----- ALTER TABLE (inplace) ----- */

static inline bool database_changed(const char *old_schema,
                                    const char *new_schema)
{
  return strcasecmp(old_schema, new_schema) != 0;
}

enum_alter_inplace_result
ha_duckdb::check_if_supported_inplace_alter(TABLE *altered_table,
                                            Alter_inplace_info *ha_alter_info)
{
  DBUG_ENTER("ha_duckdb::check_if_supported_inplace_alter");

  if (database_changed(table->s->db.str, altered_table->s->db.str))
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  if (ha_alter_info->alter_info->flags & ALTER_COLUMN_ORDER)
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);

  /* Reject ALTER on tables without PK when require_primary_key is ON */
  if (myduck::require_primary_key && table->s->primary_key == MAX_KEY)
  {
    my_error(ER_REQUIRES_PRIMARY_KEY, MYF(0));
    DBUG_RETURN(HA_ALTER_ERROR);
  }

  DBUG_RETURN(HA_ALTER_INPLACE_NO_LOCK);
}

bool ha_duckdb::commit_inplace_alter_table(TABLE *altered_table,
                                           Alter_inplace_info *ha_alter_info,
                                           bool commit)
{
  DBUG_ENTER("ha_duckdb::commit_inplace_alter_table");

  if (!commit)
    DBUG_RETURN(false);

  THD *thd= ha_thd();
  int ret= duckdb_register_trx(thd);
  if (ret)
    DBUG_RETURN(true);

  ulonglong handler_flags= ha_alter_info->handler_flags;
  ulonglong sql_flags= ha_alter_info->alter_info->flags;

  using DDL_convertor= std::unique_ptr<AlterTableConvertor>;
  using DDL_convertors= std::vector<DDL_convertor>;
  DDL_convertor convertor;
  DDL_convertors convertors;

  std::string schema_name(table->s->db.str, table->s->db.length);
  std::string table_name(table->s->table_name.str,
                         table->s->table_name.length);

  if (handler_flags & ALTER_ADD_COLUMN)
  {
    convertor= std::make_unique<AddColumnConvertor>(
        schema_name, table_name, altered_table, ha_alter_info->alter_info);
    convertors.push_back(std::move(convertor));
  }

  if (handler_flags & ALTER_DROP_COLUMN)
  {
    convertor= std::make_unique<DropColumnConvertor>(
        schema_name, table_name, table, altered_table,
        ha_alter_info->alter_info);
    convertors.push_back(std::move(convertor));
  }

  if ((sql_flags & ALTER_CHANGE_COLUMN) || (handler_flags & ALTER_COLUMN_NAME))
  {
    convertor= std::make_unique<ChangeColumnConvertor>(
        schema_name, table_name, altered_table, ha_alter_info->alter_info);
    convertors.push_back(std::move(convertor));
  }

  if (sql_flags & ALTER_CHANGE_COLUMN_DEFAULT)
  {
    convertor= std::make_unique<ChangeColumnDefaultConvertor>(
        schema_name, table_name, table, altered_table);
    convertors.push_back(std::move(convertor));
  }

  /*
    When adding a primary key, set NOT NULL on the corresponding columns
    in DuckDB (DuckDB doesn't have indexes, but needs the constraint).
  */
  if (sql_flags & ALTER_ADD_INDEX)
  {
    convertor= std::make_unique<ChangeColumnForPrimaryKeyConvertor>(
        schema_name, table_name, altered_table);
    convertors.push_back(std::move(convertor));
  }

  if (convertors.empty())
    DBUG_RETURN(false);

  /* Execute each ALTER operation in its own auto-commit context.
     DuckDB v1.5+ does not allow compound DDL that mixes structural
     changes (ADD COLUMN) with constraint updates (SET DEFAULT)
     within the same transaction. */
  auto con= myduck::DuckdbManager::CreateConnection();

  for (auto &conv : convertors)
  {
    if (!conv || conv->check())
      DBUG_RETURN(true);

    std::string sql= conv->translate();
    if (sql.empty())
      continue;

    auto query_result= myduck::duckdb_query(*con, sql);
    if (query_result->HasError())
    {
      my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_GENERIC, query_result->GetError().c_str(), "DuckDB");
      DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}

/* ----- Plugin declaration ----- */

/* ---- AliSQL-specific global variables (no DuckDB push) ---- */

static MYSQL_SYSVAR_BOOL(copy_ddl_in_batch, copy_ddl_in_batch,
                         PLUGIN_VAR_RQCMDARG,
                         "Use batch insert to speed up copy ddl", NULL, NULL,
                         TRUE);

static MYSQL_SYSVAR_BOOL(dml_in_batch, dml_in_batch, PLUGIN_VAR_RQCMDARG,
                         "Use batch to speed up INSERT/UPDATE/DELETE", NULL,
                         NULL, TRUE);

static MYSQL_SYSVAR_BOOL(update_modified_column_only,
                         update_modified_column_only, PLUGIN_VAR_RQCMDARG,
                         "Whether to only update modified columns", NULL, NULL,
                         TRUE);

/* ---- Global proxy variables (pushed into DuckDB) ---- */

static MYSQL_SYSVAR_ULONGLONG(memory_limit, myduck::global_memory_limit,
                              PLUGIN_VAR_RQCMDARG,
                              "DuckDB memory limit in bytes (0 = default)",
                              NULL, myduck::update_memory_limit_cb, 0, 0,
                              ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_STR(temp_directory, myduck::global_duckdb_temp_directory,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
                        "Directory for DuckDB temporary files", NULL, NULL,
                        NULL);

static MYSQL_SYSVAR_ULONGLONG(max_temp_directory_size,
                              myduck::global_max_temp_directory_size,
                              PLUGIN_VAR_RQCMDARG,
                              "Max disk space for DuckDB temp directory "
                              "(0 = 90%% of available)",
                              NULL, myduck::update_max_temp_directory_size_cb,
                              0, 0, ULONGLONG_MAX, 1024);

static MYSQL_SYSVAR_ULONGLONG(max_threads, myduck::global_max_threads,
                              PLUGIN_VAR_RQCMDARG,
                              "DuckDB max threads (0 = default)", NULL,
                              myduck::update_threads_cb, 0, 0, 1048576, 0);

static MYSQL_SYSVAR_BOOL(use_direct_io, myduck::global_use_dio,
                         PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
                         "Use Direct I/O for DuckDB data files", NULL, NULL,
                         FALSE);

static MYSQL_SYSVAR_BOOL(scheduler_process_partial,
                         myduck::global_scheduler_process_partial,
                         PLUGIN_VAR_RQCMDARG,
                         "Partially process tasks before rescheduling", NULL,
                         myduck::update_scheduler_process_partial_cb, TRUE);

static MYSQL_SYSVAR_ULONGLONG(checkpoint_threshold,
                              myduck::checkpoint_threshold,
                              PLUGIN_VAR_RQCMDARG,
                              "DuckDB WAL checkpoint threshold in bytes", NULL,
                              myduck::update_checkpoint_threshold_cb,
                              268435456, 0, ULONGLONG_MAX, 1024);

static MYSQL_SYSVAR_BOOL(use_double_for_decimal,
                         myduck::use_double_for_decimal, PLUGIN_VAR_RQCMDARG,
                         "Use DOUBLE for DECIMAL precision > 38", NULL, NULL,
                         TRUE);

static MYSQL_SYSVAR_BOOL(require_primary_key, myduck::require_primary_key,
                         PLUGIN_VAR_RQCMDARG,
                         "Require primary key for DuckDB tables", NULL, NULL,
                         TRUE);

static MYSQL_SYSVAR_ULONGLONG(appender_allocator_flush_threshold,
                              myduck::appender_allocator_flush_threshold,
                              PLUGIN_VAR_RQCMDARG,
                              "Flush appender allocator when batch memory "
                              "reaches this threshold",
                              NULL, myduck::update_appender_flush_threshold_cb,
                              67108864, 0, ULONGLONG_MAX, 1024);

static MYSQL_SYSVAR_SET(log_options, myduck::duckdb_log_options,
                        PLUGIN_VAR_RQCMDARG, "DuckDB operation types to log",
                        NULL, NULL, 0, &myduck::log_options_typelib);

/* ---- Session variables (propagated per-connection) ---- */

static MYSQL_THDVAR_ULONGLONG(merge_join_threshold, PLUGIN_VAR_RQCMDARG,
                              "Row count threshold to prefer merge join", NULL,
                              NULL, 4611686018427387904ULL, 0,
                              4611686018427387904ULL, 0);

static MYSQL_THDVAR_BOOL(force_no_collation, PLUGIN_VAR_RQCMDARG,
                         "Disable collation pushdown, use binary comparison",
                         NULL, NULL, FALSE);

static MYSQL_THDVAR_ENUM(explain_output, PLUGIN_VAR_RQCMDARG,
                         "DuckDB EXPLAIN output format", NULL, NULL,
                         myduck::EXPLAIN_PHYSICAL_ONLY,
                         &myduck::explain_output_typelib);

static MYSQL_THDVAR_SET(disabled_optimizers, PLUGIN_VAR_RQCMDARG,
                        "Disable specific DuckDB optimizer rules", NULL, NULL,
                        0, &myduck::disabled_optimizers_typelib);

/* ---- THDVAR accessor functions (used from duckdb_context.cc) ---- */

namespace myduck
{

ulonglong get_thd_merge_join_threshold(THD *thd)
{
  return THDVAR(thd, merge_join_threshold);
}

my_bool get_thd_force_no_collation(THD *thd)
{
  return THDVAR(thd, force_no_collation);
}

ulong get_thd_explain_output(THD *thd) { return THDVAR(thd, explain_output); }

ulonglong get_thd_disabled_optimizers(THD *thd)
{
  return THDVAR(thd, disabled_optimizers);
}

} // namespace myduck

static struct st_mysql_sys_var *duckdb_system_variables[]= {
    MYSQL_SYSVAR(copy_ddl_in_batch), MYSQL_SYSVAR(dml_in_batch),
    MYSQL_SYSVAR(update_modified_column_only),
    /* Global proxy */
    MYSQL_SYSVAR(memory_limit), MYSQL_SYSVAR(temp_directory),
    MYSQL_SYSVAR(max_temp_directory_size), MYSQL_SYSVAR(max_threads),
    MYSQL_SYSVAR(use_direct_io), MYSQL_SYSVAR(scheduler_process_partial),
    MYSQL_SYSVAR(checkpoint_threshold), MYSQL_SYSVAR(use_double_for_decimal),
    MYSQL_SYSVAR(require_primary_key),
    MYSQL_SYSVAR(appender_allocator_flush_threshold),
    MYSQL_SYSVAR(log_options),
    /* Session proxy */
    MYSQL_SYSVAR(merge_join_threshold), MYSQL_SYSVAR(force_no_collation),
    MYSQL_SYSVAR(explain_output), MYSQL_SYSVAR(disabled_optimizers), NULL};

static struct st_mysql_show_var duckdb_status_variables[]= {
    {"Duckdb_rows_insert", (char *) &srv_duckdb_status.duckdb_rows_insert,
     SHOW_LONGLONG},
    {"Duckdb_rows_update", (char *) &srv_duckdb_status.duckdb_rows_update,
     SHOW_LONGLONG},
    {"Duckdb_rows_delete", (char *) &srv_duckdb_status.duckdb_rows_delete,
     SHOW_LONGLONG},
    {"Duckdb_rows_insert_in_batch",
     (char *) &srv_duckdb_status.duckdb_rows_insert_in_batch, SHOW_LONGLONG},
    {"Duckdb_rows_update_in_batch",
     (char *) &srv_duckdb_status.duckdb_rows_update_in_batch, SHOW_LONGLONG},
    {"Duckdb_rows_delete_in_batch",
     (char *) &srv_duckdb_status.duckdb_rows_delete_in_batch, SHOW_LONGLONG},
    {"Duckdb_commit", (char *) &srv_duckdb_status.duckdb_commit,
     SHOW_LONGLONG},
    {"Duckdb_rollback", (char *) &srv_duckdb_status.duckdb_rollback,
     SHOW_LONGLONG},
    {NullS, NullS, SHOW_LONG}};

static struct st_mysql_storage_engine duckdb_storage_engine= {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

extern Plugin_function plugin_descriptor_function_run_in_duckdb;

maria_declare_plugin(duckdb)
{
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &duckdb_storage_engine,
    "DUCKDB",
    "drrtuy,lfedorov",
    "DuckDB storage engine",
    PLUGIN_LICENSE_GPL,
    duckdb_init_func,              /* Plugin Init */
    duckdb_deinit_func,            /* Plugin Deinit */
    0x0100,                        /* version number (1.0) */
    duckdb_status_variables,       /* status variables */
    duckdb_system_variables,       /* system variables */
    "1.0",                         /* string version */
    MariaDB_PLUGIN_MATURITY_ALPHA  /* maturity */
},
{
    MariaDB_FUNCTION_PLUGIN,
    &plugin_descriptor_function_run_in_duckdb,
    "RUN_IN_DUCKDB",
    "drrtuy,lfedorov",
    "Execute a SQL query in DuckDB and return the result as a string",
    PLUGIN_LICENSE_GPL,
    NULL,                          /* Plugin Init */
    NULL,                          /* Plugin Deinit */
    0x0100,                        /* version number (1.0) */
    NULL,                          /* status variables */
    NULL,                          /* system variables */
    "1.0",                         /* string version */
    MariaDB_PLUGIN_MATURITY_ALPHA  /* maturity */
}
maria_declare_plugin_end;
