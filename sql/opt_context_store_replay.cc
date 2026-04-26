/*
   Copyright (c) 2025, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

#include "sql_plugin.h"
#include "opt_context_store_replay.h"
#include "sql_show.h"
#include "my_json_writer.h"
#include "mysql.h"
#include "hash.h"

#include "sql_select.h"
#include "sql_explain.h"
#include "mysqld_error.h"
#include "sql_statistics.h"
#include "sql_json_lib.h"
#include "opt_histogram_json.h"

using namespace json_reader;

/**
  @file

  @brief
    This file provides mechanism to: -
    1. Record the range stats while the query is running
    2. Store/dump the tables/views context including index stats, range stats,
    and the cost of reading indexes, and ranges into the
    "optimizer_context" Information Schema table
    3. During replay, parse the context which is in JSON format, and
    build an in memory representation of the read stats
    4. Infuse the read stats into the optimzer.

  @detail
    1. range_stats are gathered in memory using the class Range_list_recorder
    2. Stores the tables, and views context (i.e. ddls, and basic stats)
    that are used in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer_context IS table. All these table contexts are stored in
    one place as a JSON array object with name "list_contexts".
    The high level json structure looks like: -
    {
      "list_contexts": [
        {
          "name": "table_name",
          "file_stat_records" : n
          "file_stat_records": n,
          "read_cost_io": n,
          "read_cost_cpu": n,
          "indexes": [ //optional
            {
              ...
            }, ...,
          ],
          "list_ranges": [ //optional
            {
              ...
            }, ...
          ],
          "list_index_read_costs": [ //optional
            {
              ...
            }, ...,
          ]
        }, ...
      ]
    }
    See mysql-test/include/opt_context_schema.inc for its JSON Schema.
    The function "store_optimizer_context()" is used to dump the
    all the tables stats into IS table.
    3. Later, when this JSON structure is given as input to the variable
    "optimizer_replay_context" in the form of an user defined variable,
    it is parsed and an in-memory representation of the same structure is built
    using the class Optimizer_context_replay.
    4. To infuse the stats into the optimizer, the same class
    Optimizer_context_replay is used.

    In addition to storing the table contexts in JSON structure;
    system variables, and EITS stats are also stored. However,
    these are not included in the JSON structure. Instead, they
    are stored as "SET sys_var = current_session_value;" statements.
    Constant tables used in the queries, and EITS stats are recorded
    as "REPLACE INTO table_name VALUES(...);" statements.
    The table_name would either be the table referred in the query or
    one of MYSQL.TABLE_STATS, MYSQL.COLUMN_STATS, MYSQL.INDEX_STATS.
    functions store_system_variables(), and dump_eits_stats() are used
    for achieving this purpose. All these statements are also recorded in
    the "optimizer_context" Information Schema table.
*/

/*
   A record of one multi_range_read_info_const() call:
*/
class Multi_range_read_const_call_record : public Sql_alloc
{
public:
  char *idx_name;
  ha_rows rows;
  List<char> range_list;
  Cost_estimate cost;
  ha_rows max_index_blocks;
  ha_rows max_row_blocks;
};

/*
   A record to hold one cost_for_index_read() call:
*/
class cost_index_read_call_record : public Sql_alloc
{
public:
  uint key;
  ha_rows records;
  bool eq_ref;
  ALL_READ_COST cost;
};

/*
   A record to hold one records_in_range() call:
*/
class records_in_range_call_record : public Sql_alloc
{
public:
  uint keynr;
  char *min_key;
  char *max_key;
  ha_rows records;
};

/*
   structure to store all the index range records,
   and the cost for reading indexes, pertaining to a table
*/
class table_context_for_store : public Sql_alloc
{
public:
  /* full name of the table or view i.e db_name.{table|view}_name */
  char *name;
  size_t name_len;
  List<Multi_range_read_const_call_record> mrr_list;
  List<cost_index_read_call_record> irc_list;
  List<records_in_range_call_record> rir_list;
  List<char> const_tbl_ins_stmt_list;
};

namespace Show
{

ST_FIELD_INFO optimizer_context_capture_info[]= {
    Column("QUERY", Longtext(65535), NOT_NULL),
    Column("CONTEXT", Longtext(65535), NOT_NULL), CEnd()};
} // namespace Show

static void append_full_table_name(const TABLE_LIST *tbl, String *buf);
static int parse_check_obj_start_in_array(json_engine_t *je, String *err_buf,
                                          const char *err_msg);
static int parse_table_context(THD *thd, json_engine_t *je, String *err_buf,
                               table_context_for_replay *table_ctx);
static int parse_index_context(THD *thd, json_engine_t *je, String *err_buf,
                               index_context_for_replay *index_ctx);
static int parse_range_context(THD *thd, json_engine_t *je, String *err_buf,
                               Multi_range_read_const_call_record *range_ctx);
static int parse_index_read_cost_context(THD *thd, json_engine_t *je,
                                         String *err_buf,
                                         cost_index_read_call_record *out);
static bool parse_range_cost_estimate(THD *thd, json_engine_t *je,
                                      String *err_buf, Cost_estimate *cost);
static int parse_records_in_range_context(THD *thd, json_engine_t *je,
                                          String *err_buf,
                                          records_in_range_call_record *rir_ctx);

static char *strdup_root(MEM_ROOT *root, const String *str)
{
  return strmake_root(root, str->ptr(), str->length());
}

struct TABLE_NAME_KEY
{
  char *name; // full name of the table or view
  size_t name_len;
};

struct DB_NAME_KEY
{
  const char *name; // database name
  size_t name_len;
};

/*
  helper function to know the key portion of the record
  that is stored in hash.
*/
static const uchar *get_table_name_key(const void *entry_, size_t *length,
                                       my_bool flags)
{
  auto entry= static_cast<const TABLE_NAME_KEY *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

/*
  helper function to know the key portion of the record
  that is stored in hash.
*/
static const uchar *get_db_name_key(const void *entry_, size_t *length,
                                    my_bool flags)
{
  auto entry= static_cast<const DB_NAME_KEY *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

/*
  @brief
    Check whether a table is a regular base table (for which we should
    dump the ddl) or not.

  @detail
    Besides base tables, the query may have:
     - Table functions (Currently it's only JSON_TABLE)
     - INFORMATION_SCHEMA tables
     - Tables in PERFORMANCE_SCHEMA and mysql database
     - Internal temporary ("work") tables
*/
static bool is_base_table(const TABLE_LIST *tbl)
{
  return (tbl->table && tbl->table->s && !tbl->table_function &&
          !tbl->schema_table &&
          get_table_category(tbl->get_db_name(), tbl->get_table_name()) ==
              TABLE_CATEGORY_USER &&
          tbl->table->s->tmp_table != INTERNAL_TMP_TABLE &&
          tbl->table->s->tmp_table != SYSTEM_TMP_TABLE);
}

static void dump_range_stats(THD *thd, table_context_for_store *context,
                             Json_writer *ctx_writer)
{
  Json_writer_array list_ranges_wrapper(ctx_writer, "list_ranges");
  List_iterator irc_li(context->mrr_list);
  while (Multi_range_read_const_call_record *irc= irc_li++)
  {
    Json_writer_object irc_wrapper(ctx_writer);
    irc_wrapper.add("index_name", irc->idx_name);
    List_iterator rc_li(irc->range_list);
    Json_writer_array ranges_wrapper(ctx_writer, "ranges");
    while (const char *range_str= rc_li++)
    {
      ranges_wrapper.add(range_str, strlen(range_str));
    }
    ranges_wrapper.end();

    irc_wrapper.add("num_rows", irc->rows);
    {
      Json_writer_object cost_wrapper(ctx_writer, "cost");
      cost_wrapper.add("avg_io_cost", irc->cost.avg_io_cost);
      cost_wrapper.add("cpu_cost", irc->cost.cpu_cost);
      cost_wrapper.add("comp_cost", irc->cost.comp_cost);
      cost_wrapper.add("copy_cost", irc->cost.copy_cost);
      cost_wrapper.add("limit_cost", irc->cost.limit_cost);
      cost_wrapper.add("setup_cost", irc->cost.setup_cost);
      cost_wrapper.add("index_cost_io", irc->cost.index_cost.io);
      cost_wrapper.add("index_cost_cpu", irc->cost.index_cost.cpu);
      cost_wrapper.add("row_cost_io", irc->cost.row_cost.io);
      cost_wrapper.add("row_cost_cpu", irc->cost.row_cost.cpu);
    }

    irc_wrapper.add("max_index_blocks", irc->max_index_blocks);
    irc_wrapper.add("max_row_blocks", irc->max_row_blocks);
  }
}

static void dump_index_read_cost(THD *thd, table_context_for_store *context,
                                 Json_writer *ctx_writer)
{
  Json_writer_array list_irc_wrapper(ctx_writer, "list_index_read_costs");
  List_iterator irc_li(context->irc_list);

  while (cost_index_read_call_record *irc= irc_li++)
  {
    Json_writer_object irc_wrapper(ctx_writer);
    irc_wrapper.add("key_number", irc->key);
    irc_wrapper.add("num_records", irc->records);
    irc_wrapper.add("eq_ref", irc->eq_ref ? 1 : 0);
    irc_wrapper.add("index_cost_io", irc->cost.index_cost.io);
    irc_wrapper.add("index_cost_cpu", irc->cost.index_cost.cpu);
    irc_wrapper.add("row_cost_io", irc->cost.row_cost.io);
    irc_wrapper.add("row_cost_cpu", irc->cost.row_cost.cpu);
    irc_wrapper.add("max_index_blocks", irc->cost.max_index_blocks);
    irc_wrapper.add("max_row_blocks", irc->cost.max_row_blocks);
    irc_wrapper.add("copy_cost", irc->cost.copy_cost);
  }
}

static void dump_records_in_range(THD *thd, table_context_for_store *context,
                                  Json_writer *ctx_writer)
{
  Json_writer_array list_irc_wrapper(ctx_writer, "list_records_in_range");
  List_iterator rir_li(context->rir_list);

  while (records_in_range_call_record *rir= rir_li++)
  {
    Json_writer_object rir_wrapper(ctx_writer);
    rir_wrapper.add("key_number", rir->keynr);
    rir_wrapper.add("min_key", rir->min_key);
    rir_wrapper.add("max_key", rir->max_key);
    rir_wrapper.add("num_records", rir->records);
  }
}

static void dump_index_stats(THD *thd, uchar *tbl_name, size_t tbl_name_len,
                             Json_writer *ctx_writer)
{
  table_context_for_store *table_context=
      thd->opt_ctx_recorder->search(tbl_name, tbl_name_len);

  if (!table_context)
    return;

  dump_range_stats(thd, table_context, ctx_writer);
  dump_index_read_cost(thd, table_context, ctx_writer);
  dump_records_in_range(thd, table_context, ctx_writer);
}

/*
  dump the following table stats to optimizer_context IS table: -
  1. total number of records in the table
  2. if there any indexes for the table then
      their names, and the num of records per key
  3. range stats on the indexes
  4. cost of reading indexes
*/
static void dump_table_stats(THD *thd, TABLE_LIST *tbl, uchar *tbl_name,
                             size_t tbl_name_len,
                             Json_writer_object &ctx_wrapper,
                             Json_writer *ctx_writer)
{
  TABLE *table= tbl->table;
  ha_rows records= table->stat_records();
  IO_AND_CPU_COST cost= table->file->ha_scan_time(records);
  ctx_wrapper.add("name", (char *) tbl_name, tbl_name_len);
  ctx_wrapper.add("file_stat_records", table->file->stats.records);
  ctx_wrapper.add("read_cost_io", cost.io);
  ctx_wrapper.add("read_cost_cpu", cost.cpu);
  if (!table->key_info)
    return;

  Json_writer_array indexes_wrapper(ctx_writer, "indexes");
  for (uint idx= 0; idx < table->s->keys; idx++)
  {
    KEY *key= &table->key_info[idx];
    uint num_key_parts= key->user_defined_key_parts;
    Json_writer_object index_wrapper(ctx_writer);
    index_wrapper.add("index_name", key->name);
    Json_writer_array rpk_wrapper(ctx_writer, "rec_per_key");
    for (uint i= 0; i < num_key_parts; i++)
    {
      rpk_wrapper.add(key->actual_rec_per_key(i));
    }
    rpk_wrapper.end();
  }
  indexes_wrapper.end();
  dump_index_stats(thd, tbl_name, tbl_name_len, ctx_writer);
}

static void create_view_def(THD *thd, TABLE_LIST *table, String *name,
                            String *buf)
{
  buf->append(STRING_WITH_LEN("CREATE "));
  view_store_options(thd, table, buf);
  buf->append(STRING_WITH_LEN("VIEW "));
  buf->append(*name);
  buf->append(STRING_WITH_LEN(" AS "));
  buf->append(table->select_stmt.str, table->select_stmt.length);
}

/*
  System variables that are related to the optimizer but
  do not have "optimizer" in their name.
*/
static const char *opt_related_sys_vars[]= {"join_cache_level",
                                            "join_buffer_size", NULL};

static const char *excluded_sys_vars[]= {"optimizer_replay_context",
                                         "optimizer_record_context", NULL};

static bool is_optimizer_related_var(const char **sys_vars,
                                     const char *var_name)
{
  for (uint i= 0; sys_vars[i] != NULL; i++)
  {
    if (!strcmp(sys_vars[i], var_name))
      return true;
  }
  return false;
}

/*
  @brief
    Save current values of optimizer variables: append to sql_script
    a set of "SET variable=value" statements.
*/
static void store_system_variables(THD *thd, String &sql_script)
{
  CHARSET_INFO *charset_info= system_charset_info;
  // hold the lock until the end of this method.
  // follows the same pattern as in sql_show.cc#fill_variables()
  mysql_prlock_rdlock(&LOCK_system_variables_hash);
  if (!thd->variables.dynamic_variables_ptr ||
      global_system_variables.dynamic_variables_head >
          thd->variables.dynamic_variables_head)
    sync_dynamic_session_variables(thd, true);
  SHOW_VAR *all_session_vars= enumerate_sys_vars(thd, true, SHOW_OPT_SESSION);
  size_t len;
  StringBuffer<1024> buf;
  const char *pos;
  for (SHOW_VAR *show_var= all_session_vars; show_var->name != NULL;
       show_var++)
  {
    if (is_optimizer_related_var(excluded_sys_vars, show_var->name))
      continue;
    if (strstr(show_var->name, "optimizer") != NULL ||
        is_optimizer_related_var(opt_related_sys_vars, show_var->name))
    {
      sys_var *var= (sys_var *) show_var->value;
      mysql_mutex_lock(&LOCK_global_system_variables);
      pos= get_one_variable(thd, show_var, SHOW_OPT_SESSION, show_var->type,
                            NULL, &charset_info, buf.c_ptr_safe(), &len);
      mysql_mutex_unlock(&LOCK_global_system_variables);
      sql_script.append(STRING_WITH_LEN("SET "));

      if (var->check_type(SHOW_OPT_SESSION))
        sql_script.append(STRING_WITH_LEN("GLOBAL "));

      sql_script.append(show_var->name, strlen(show_var->name));
      sql_script.append(STRING_WITH_LEN("="));
      switch (var->show_type())
      {
      case SHOW_DOUBLE:
      case SHOW_BOOL:
      case SHOW_UINT:
      case SHOW_ULONG:
      case SHOW_ULONGLONG:
      case SHOW_SINT:
      case SHOW_SLONG:
      case SHOW_SLONGLONG:
      case SHOW_SIZE_T:
        sql_script.append(pos, len);
        break;
      default:
        sql_script.append(STRING_WITH_LEN("'"));
        sql_script.append(pos, len);
        sql_script.append(STRING_WITH_LEN("'"));
        break;
      }
      sql_script.append(STRING_WITH_LEN(";\n\n"));
    }
  }
  mysql_prlock_unlock(&LOCK_system_variables_hash);
}

/*
  @brief
    Append the "create database db_name" DDL statement to sql_script, if it is
    not already present in the db_name_hash.
    Also, append "use database db_name" if the flag req_use_db_stmt is set.

  @return
    0  OK
    1  OOM error
*/
static bool store_db_ddl(THD *thd, HASH *db_name_hash, String &sql_script,
                         const char *db_name, bool req_use_db_stmt)
{
  size_t db_name_len= strlen(db_name);

  if (db_name_len &&
      !my_hash_search(db_name_hash, (uchar *) db_name, db_name_len))
  {
    DB_NAME_KEY *db_name_key;
    if (!(db_name_key= (DB_NAME_KEY *) thd->alloc(sizeof(DB_NAME_KEY))))
      return true; // OOM

    db_name_key->name= db_name;
    db_name_key->name_len= db_name_len;

    if (my_hash_insert(db_name_hash, (uchar *) db_name_key))
      return true; // OOM

    sql_script.append(STRING_WITH_LEN("CREATE DATABASE IF NOT EXISTS "));
    sql_script.append(db_name, db_name_len);
    sql_script.append(STRING_WITH_LEN(";\n\n"));

    if (req_use_db_stmt)
    {
      sql_script.append(STRING_WITH_LEN("USE "));
      sql_script.append(db_name, db_name_len);
      sql_script.append(STRING_WITH_LEN(";\n\n"));
    }
  }

  return false;
}

/*
  @brief
    Dump definitions, basic stats of all tables and views used by the
    statement into the optimizer_context IS table.
    The goal is to eventually save everything that is needed to
    reproduce the query execution

  @detail
    Stores the ddls, stats of the tables, and views that are used
    in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer_context IS table.
    Global query_tables are read in reverse order from the thd->lex,
    and a record with table_name, and ddl of the table are created.
    Hash is used to store the records, where in no duplicates
    are stored. db_name.table_name is used as a key to discard any
    duplicates. If a new record that is created is not in the hash,
    then that is dumped into the IS table.

  @return
    false when no error occurred during the computation
*/
bool store_optimizer_context(THD *thd)
{
  LEX *lex= thd->lex;

  if (thd->spcont)
  {
    /* This is a sub-statement inside SP. Don't do anything */
    return false;
  }

  if (!thd->opt_ctx_recorder || lex->query_tables == *(lex->query_tables_last))
  {
    return false;
  }
  String sql_script;
  sql_script.set_charset(system_charset_info);
  Json_writer ctx_writer;
  Json_writer_object context(&ctx_writer);
  Json_writer_array context_list(&ctx_writer, "list_contexts");
  sql_script.append(STRING_WITH_LEN("SET NAMES utf8mb4;\n\n "));
  store_system_variables(thd, sql_script);
  HASH table_name_hash;
  HASH db_name_hash;
  List<TABLE_LIST> tables_list;

  /*
    lex->query_tables lists the VIEWs before their underlying tables.
    Create a list in the reverse order.
  */
  for (TABLE_LIST *tbl= lex->query_tables; tbl; tbl= tbl->next_global)
  {
    if (!tbl->is_view() && !is_base_table(tbl))
      continue;
    if (tables_list.push_front(tbl))
      return true;
  }

  if (tables_list.is_empty())
    return false;

  List_iterator li(tables_list);
  clean_captured_ctx(thd);

  if (my_hash_init(key_memory_trace_ddl_info, &table_name_hash,
                   system_charset_info, 16, 0, 0, get_table_name_key, NULL,
                   HASH_UNIQUE) ||
      my_hash_init(key_memory_trace_ddl_info, &db_name_hash,
                   system_charset_info, 16, 0, 0, get_db_name_key, NULL,
                   HASH_UNIQUE) ||
      store_db_ddl(thd, &db_name_hash, sql_script, thd->get_db(), true))
  {
    return true;
  }

  bool res= false;
  List<TABLE_LIST> uniq_tables_list;
  for (TABLE_LIST *tbl= li++; tbl; tbl= li++)
  {
    String ddl;
    String full_tbl_name;
    TABLE_NAME_KEY *tbl_name_key;
    append_full_table_name(tbl, &full_tbl_name);

    /*
      A query can use the same table multiple times. Do not dump the
      DDL multiple times.
    */
    if (my_hash_search(&table_name_hash, (uchar *) full_tbl_name.c_ptr_safe(),
                       full_tbl_name.length()))
      continue;

    if (store_db_ddl(thd, &db_name_hash, sql_script, tbl->get_db_name().str,
                     false))
    {
      res= true;
      break;
    }

    if (tbl->is_view())
    {
      StringBuffer<64> drop;
      drop.append(STRING_WITH_LEN("DROP VIEW IF EXISTS "));
      drop.append(full_tbl_name);
      drop.append(STRING_WITH_LEN(";\n"));
      sql_script.append(drop);
      
      create_view_def(thd, tbl, &full_tbl_name, &ddl);
    }
    else
    {
      StringBuffer<64> drop;
      drop.append(STRING_WITH_LEN("DROP TABLE IF EXISTS "));
      drop.append(full_tbl_name);
      drop.append(STRING_WITH_LEN(";\n"));
      sql_script.append(drop);

      if (show_create_table(thd, tbl, &ddl, NULL, WITH_DB_NAME))
      {
        res= true;
        break;
      }
    }

    if (!(tbl_name_key=
              (TABLE_NAME_KEY *) thd->alloc(sizeof(TABLE_NAME_KEY))) ||
        !(tbl_name_key->name= strdup_root(thd->mem_root, &full_tbl_name)))
    {
      res= true;
      break;
    }

    tbl_name_key->name_len= strlen(tbl_name_key->name);

    if (my_hash_insert(&table_name_hash, (uchar *) tbl_name_key))
    {
      res= true; // OOM
      break;
    }

    sql_script.append(ddl);
    sql_script.append(STRING_WITH_LEN(";\n\n"));

    if (!tbl->is_view())
    {
      Json_writer_object ctx_wrapper(&ctx_writer);
      table_context_for_store *table_context= thd->opt_ctx_recorder->search(
          (uchar *) tbl_name_key->name, tbl_name_key->name_len);
      if (table_context)
      {
        List_iterator inserts_li(table_context->const_tbl_ins_stmt_list);
        while (char *stmt= inserts_li++)
        {
          sql_script.append(stmt, strlen(stmt));
          sql_script.append(STRING_WITH_LEN(";\n\n"));
        }
      }
      dump_table_stats(thd, tbl, (uchar *) tbl_name_key->name,
                       tbl_name_key->name_len, ctx_wrapper, &ctx_writer);
    }
    uniq_tables_list.push_front(tbl);
  }
  context_list.end();
  context.end();

  if (!res)
    res= dump_eits_stats(thd, &uniq_tables_list, sql_script);

  if (!res)
  {
    const char *SET_OPT_CONTEXT_VAR= "set @opt_context=\'\n";
    const char *SET_REPLAY_CONTEXT_VAR=
        "set optimizer_replay_context=\'opt_context\'";
    String *s= const_cast<String *>(ctx_writer.output.get_string());
    sql_script.append(SET_OPT_CONTEXT_VAR, strlen(SET_OPT_CONTEXT_VAR));
    sql_script.append(*s);
    sql_script.append(STRING_WITH_LEN("\n\';#opt_context_ends\n\n"));
    sql_script.append(SET_REPLAY_CONTEXT_VAR, strlen(SET_REPLAY_CONTEXT_VAR));
    sql_script.append(STRING_WITH_LEN(";\n\n"));
    sql_script.append(thd->query(), thd->query_length());
    sql_script.append(STRING_WITH_LEN(";\n\n"));
    sql_script.append(STRING_WITH_LEN("set optimizer_replay_context='';\n\n"));
    thd->captured_opt_ctx= new Optimizer_context_capture(thd, sql_script);
    if (!thd->captured_opt_ctx)
      return true; // OOM
  }
  my_hash_free(&table_name_hash);
  my_hash_free(&db_name_hash);
  return res;
}

/*
  Create a new table context if it is not already present in the
  hash.
  The table context is also persisted in the hash which is to be
  used later for dumping all the context infomation into the
  optimizer_context IS table.
*/
table_context_for_store *
Optimizer_context_recorder::get_table_context(const TABLE_LIST *tbl)
{
  String tbl_name;
  append_full_table_name(tbl, &tbl_name);
  table_context_for_store *table_ctx=
      search((uchar *) tbl_name.c_ptr_safe(), tbl_name.length());

  if (!table_ctx)
  {
    if (!(table_ctx= new (mem_root) table_context_for_store))
      return nullptr; // OOM

    if (!(table_ctx->name= strdup_root(mem_root, &tbl_name)))
      return nullptr; // OOM

    table_ctx->name_len= tbl_name.length();

    if (my_hash_insert(&tbl_ctx_hash, (uchar *) table_ctx))
      return nullptr; // OOM
  }

  return table_ctx;
}

Optimizer_context_recorder::Optimizer_context_recorder(MEM_ROOT *mem_root_arg) :
  mem_root(mem_root_arg)
{
  my_hash_init(key_memory_trace_ddl_info, &tbl_ctx_hash, system_charset_info,
               16, 0, 0, &Optimizer_context_recorder::get_tbl_ctx_key, 0,
               HASH_UNIQUE);
}

Optimizer_context_recorder::~Optimizer_context_recorder()
{
  my_hash_free(&tbl_ctx_hash);
}

bool Optimizer_context_recorder::has_records()
{
  return tbl_ctx_hash.records > 0;
}

table_context_for_store *
Optimizer_context_recorder::search(uchar *tbl_name, size_t tbl_name_len)
{
  return (table_context_for_store *) my_hash_search(&tbl_ctx_hash, tbl_name,
                                                    tbl_name_len);
}

void Optimizer_context_recorder::record_multi_range_read_info_const(
    const TABLE_LIST *tbl,
    uint keynr,
    Range_print_enumerator *ranges,
    ha_rows rows,
    const Cost_estimate *cost,
    ha_rows max_index_blocks,
    ha_rows max_row_blocks)
{
  /* Do not record calls made by "Range checked for each record" */
  if (current_thd->lex->explain->is_query_plan_ready())
    return;

  auto *range_ctx= new (mem_root) Multi_range_read_const_call_record;

  if (unlikely(!range_ctx))
    return; // OOM

  const char *index_name= tbl->table->key_info[keynr].name.str;
  if (!(range_ctx->idx_name= strdup_root(mem_root, index_name)))
    return; // OOM

  range_ctx->rows= rows;
  range_ctx->cost= *cost;
  range_ctx->max_index_blocks= max_index_blocks;
  range_ctx->max_row_blocks= max_row_blocks;

  while (!ranges->next())
  {
    const String &str= ranges->get_interval_str();
    const char *range_str;

    if (!(range_str= strdup_root(mem_root, &str)))
      return;
    range_ctx->range_list.push_back(range_str, mem_root);
  }

  /*
    Store the ranges of every index of the table into the
    table context.
  */
  table_context_for_store *table_ctx= get_table_context(tbl);

  if (unlikely(!table_ctx))
    return; // OOM

  table_ctx->mrr_list.push_back(range_ctx, mem_root);
}

/*
  record cost of reading an index, and add it to the index read cost list
  of the table context.
*/
void Optimizer_context_recorder::record_cost_index_read(
    const TABLE_LIST *tbl, uint key, ha_rows records,
    bool eq_ref, const ALL_READ_COST *cost)
{
  cost_index_read_call_record *idx_read_rec=
      new (mem_root) cost_index_read_call_record;

  if (unlikely(!idx_read_rec))
    return; // OOM

  idx_read_rec->key= key;
  idx_read_rec->records= records;
  idx_read_rec->eq_ref= eq_ref;
  idx_read_rec->cost= *cost;

  table_context_for_store *table_ctx= get_table_context(tbl);

  if (unlikely(!table_ctx))
    return; // OOM

  table_ctx->irc_list.push_back(idx_read_rec, mem_root);
}

/*
  helper function to know the key portion of the
  table context that is stored in hash.
*/
const uchar *Optimizer_context_recorder::get_tbl_ctx_key(const void *entry_,
                                                         size_t *length,
                                                         my_bool flags)
{
  auto entry= static_cast<const table_context_for_store *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

void Optimizer_context_recorder::record_records_in_range(
    const TABLE *tbl, const KEY_PART_INFO *key_part,
    uint keynr, const key_range *min_range, const key_range *max_range,
    ha_rows records)
{
  records_in_range_call_record *rec_in_range_ctx=
      new (mem_root) records_in_range_call_record;

  if (unlikely(!rec_in_range_ctx))
    return; // OOM

  rec_in_range_ctx->keynr= keynr;
  String min_key;
  String max_key;
  print_key_value(&min_key, key_part, min_range->key, min_range->length);
  print_key_value(&max_key, key_part, min_range->key, min_range->length);

  if (!(rec_in_range_ctx->min_key= strdup_root(mem_root, &min_key)))
    return; // OOM

  if (!(rec_in_range_ctx->max_key= strdup_root(mem_root, &max_key)))
    return; // OOM

  rec_in_range_ctx->records= records;

  table_context_for_store *table_ctx=
      get_table_context(tbl->pos_in_table_list);

  if (unlikely(!table_ctx))
    return; // OOM

  table_ctx->rir_list.push_back(rec_in_range_ctx, mem_root);
}

void Optimizer_context_recorder::record_table_row(TABLE *tbl, int row_index)
{
  StringBuffer<512> output(&my_charset_utf8mb4_bin);

  /*
    The table could have fields that do not have a default value
    but are not in the table->read_set.
    The record doesn't have values for those.
    Use a relaxed sql_mode setting so that REPLACE INTO doesn't fail.
  */
  output.append(
    STRING_WITH_LEN("SET STATEMENT sql_mode="
                    "REPLACE(REPLACE(@@sql_mode,'STRICT_ALL_TABLES',''),"
                    "'STRICT_TRANS_TABLES','') FOR\n"));
  output.append(STRING_WITH_LEN("REPLACE INTO "));
  append_full_table_name(tbl->pos_in_table_list, &output);
  format_and_store_row(tbl, tbl->record[row_index], true, " VALUES ", false, output);
  table_context_for_store *table_ctx=
      get_table_context(tbl->pos_in_table_list);

  if (unlikely(!table_ctx))
    return; // OOM

  char *ins_stmt= strdup_root(mem_root, &output);

  if (unlikely(!ins_stmt))
    return; // OOM

  table_ctx->const_tbl_ins_stmt_list.push_back(ins_stmt, mem_root);
}

/*
  Given a table *tbl, store its "db_name.table_name" into buf.
*/
static void append_full_table_name(const TABLE_LIST *tbl, String *buf)
{
  // TODO : eventually we'll need quoting.
  buf->append(tbl->get_db_name().str, tbl->get_db_name().length);
  buf->append(STRING_WITH_LEN("."));
  buf->append(tbl->get_table_name().str, tbl->get_table_name().length);
}


/*
  This class is used to store the in-memory representation of
  one index context i.e. read from json
*/
class index_context_for_replay : public Sql_alloc
{
public:
  char *idx_name;
  List<ha_rows> list_rec_per_key;
};

/*
  This class is used to store the in-memory representation of
  a table context i.e. read from json.
  A list of index contexts, and range contexts are stored separately.
*/
class table_context_for_replay : public Sql_alloc
{
public:
  /*
     full name of the table or view
     i.e db_name.[table/view]_name
  */
  char *name;
  ha_rows total_rows;
  ha_rows file_stat_records;
  double read_cost_io;
  double read_cost_cpu;
  List<index_context_for_replay> index_list;
  List<Multi_range_read_const_call_record> ranges_list;
  List<cost_index_read_call_record> irc_list;
  List<records_in_range_call_record> rir_list;
};

/*
  This class structure is used to temporarily store the old index stats
  that are in the optimizer, before they are updated by the stats
  from replay json.
  They are restored once the query that used replay json stats is done
  execution.
*/
class Saved_Index_stats : public Sql_alloc
{
public:
  KEY *key_info;
  bool original_is_statistics_from_stat_tables;
  Index_statistics *original_read_stats;
};

/*
  This class structure is used to temporarily store the old table stats
  that are in the optimizer, before they are updated by the stats
  from replay json.
  They are restored once the query that used replay json stats is done
  execution.
*/
class Saved_Table_stats : public Sql_alloc
{
public:
  TABLE *table;
  /*
    We do not restore table->file->stats.records, they are read from the
    storage engine for every query anyway.
  */
  List<Saved_Index_stats> saved_indexstats_list;
};

/*
  Extends the Read_value interface to read a container of values,
  for eg: array of numbers or strings, an object with several fields, etc...
*/
class Read_container_value : public Read_value
{
private:
  int before_read(json_engine_t *je, const char *value_name, String *err_buf)
  {
    if (json_scan_next(je) || je->state != JST_ARRAY_START)
    {
      err_buf->append(STRING_WITH_LEN("error reading "));
      err_buf->append(value_name, strlen(value_name));
      err_buf->append(STRING_WITH_LEN(" value"));
      return 1;
    }
    return 0;
  }

  int after_read(int rc) { return rc > 0; }

public:
  bool read_value(json_engine_t *je, const char *value_name,
                  String *err_buf) override
  {
    int rc= before_read(je, value_name, err_buf);
    if (rc <= 0)
    {
      rc= read_container(je, err_buf);
    }
    return after_read(rc);
  }
  virtual int read_container(json_engine_t *je, String *err_buf)= 0;
};

class Read_range_cost_estimate : public Read_value
{
  THD *thd;
  Cost_estimate *ptr;

public:
  Read_range_cost_estimate(THD *thd_arg, Cost_estimate *ptr_arg)
      : thd(thd_arg), ptr(ptr_arg)
  {
  }
  bool read_value(json_engine_t *je, const char *value_name,
                  String *err_buf) override
  {
    return parse_range_cost_estimate(thd, je, err_buf, ptr);
  }
};

class Read_list_of_ha_rows : public Read_container_value
{
  THD *thd;
  List<ha_rows> *list_values;

public:
  Read_list_of_ha_rows(THD *thd_arg, List<ha_rows> *list_values_arg)
      : thd(thd_arg), list_values(list_values_arg)
  {
  }
  int read_container(json_engine_t *je, String *err_buf) override
  {
    while (je->state != JST_ARRAY_END)
    {
      using json_reader::read_ha_rows_and_check_limit;
      ha_rows temp_value;
      if (read_ha_rows_and_check_limit(je, "rec_per_key", err_buf, temp_value,
                                       ULONGLONG_MAX, "unsigned longlong",
                                       true))
      {
        return 1;
      }

      ha_rows *records_ptr= (ha_rows *) thd->alloc(sizeof(ha_rows));

      if (unlikely(!records_ptr))
        return 1; // OOM

      *records_ptr= temp_value;

      if (list_values->push_back(records_ptr) || json_scan_next(je))
        return 1;
    }
    return 0;
  }
};

template <typename T> class Read_list_of_context : public Read_container_value
{
  THD *thd;
  List<T> *list_ctx;
  int (*parse_context_fn)(THD *, json_engine_t *, String *, T *);

public:
  Read_list_of_context(THD *thd_arg, List<T> *list_ctx_arg,
                       int (*parse_context_fn_arg)(THD *, json_engine_t *,
                                                   String *, T *))
      : thd(thd_arg), list_ctx(list_ctx_arg),
        parse_context_fn(parse_context_fn_arg)
  {
  }
  int read_container(json_engine_t *je, String *err_buf) override
  {
    int rc;

    while (1)
    {
      T *ctx= new T();

      if (unlikely(!ctx))
        return 1; // OOM

      rc= parse_context_fn(thd, je, err_buf, ctx);

      if (rc == 0)
      {
        if (list_ctx->push_back(ctx))
          return 1; // OOM
      }
      else
        break;
    }

    return rc;
  }
};

class Read_list_of_ranges : public Read_container_value
{
  THD *thd;
  List<char> *list_ranges;

public:
  Read_list_of_ranges(THD *thd_arg, List<char> *list_ranges_arg)
      : thd(thd_arg), list_ranges(list_ranges_arg)
  {
  }
  int read_container(json_engine_t *je, String *err_buf) override
  {
    if (json_scan_next(je))
      return 1;

    while (je->state != JST_ARRAY_END)
    {
      char *value;
      if (read_string(thd, je, "ranges", err_buf, value))
        return 1;

      list_ranges->push_back(value);
      if (json_scan_next(je))
        return 1;
    }

    return 0;
  }
};

/*
  check if the next element being parsed is an object within an array.
  fill the err_buf with err_msg if the parsing check fails.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_check_obj_start_in_array(json_engine_t *je, String *err_buf,
                                          const char *err_msg)
{
  if (json_scan_next(je))
    return 1;

  if (je->state != JST_VALUE)
  {
    if (je->state == JST_ARRAY_END)
      return -1; // EOF
    else
      return 1; // An error
  }

  if (json_scan_next(je) || je->state != JST_OBJ_START)
  {
    err_buf->append(err_msg, strlen(err_msg));
    return 1;
  }

  return 0;
}

/*
  parse a single context object from a json array of contexts.
  The conext object should contain the elements that are
  defined in the argument array

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_context_obj_from_json_array(json_engine_t *je,
                                             String *err_buf,
                                             const char *err_msg,
                                             Read_named_member *array)
{
  if (int rc= parse_check_obj_start_in_array(je, err_buf, err_msg))
    return rc;

  return json_read_object(je, array, err_buf);
}

/*
  Parses the table context of the JSON structure
  of the optimizer context.
  A single array element of list_contexts is parsed
  in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_table_context(THD *thd, json_engine_t *je, String *err_buf,
                               table_context_for_replay *table_ctx)
{
  const char *err_msg= "Expected an object in the list_contexts array";

  Read_named_member array[]= {
      {"name", Read_string(thd, &table_ctx->name), false},
      {"file_stat_records",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&table_ctx->file_stat_records),
       false},
      {"read_cost_io", Read_double(&table_ctx->read_cost_io), false},
      {"read_cost_cpu", Read_double(&table_ctx->read_cost_cpu), false},
      {"indexes",
       Read_list_of_context<index_context_for_replay>(
           thd, &table_ctx->index_list, parse_index_context),
       true},
      {"list_ranges",
       Read_list_of_context<Multi_range_read_const_call_record>(
           thd, &table_ctx->ranges_list, parse_range_context),
       true},
      {"list_index_read_costs",
       Read_list_of_context<cost_index_read_call_record>(
           thd, &table_ctx->irc_list, parse_index_read_cost_context),
       true},
      {"list_records_in_range",
       Read_list_of_context<records_in_range_call_record>(
           thd, &table_ctx->rir_list, parse_records_in_range_context),
       true},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the index context of the JSON structure
  of the optimizer context.
  To be specific, single array element of indexes
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_index_context(THD *thd, json_engine_t *je, String *err_buf,
                               index_context_for_replay *index_ctx)
{
  const char *err_msg= "Expected an object in the indexes array";

  Read_named_member array[]= {
      {"index_name", Read_string(thd, &index_ctx->idx_name), false},
      {"rec_per_key", Read_list_of_ha_rows(thd, &index_ctx->list_rec_per_key),
       false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the range context of the JSON structure
  of the optimizer context.
  To be specific, a single array element of list_ranges
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_range_context(THD *thd, json_engine_t *je, String *err_buf,
                               Multi_range_read_const_call_record *out)
{
  const char *err_msg= "Expected an object in the list_ranges array";

  Read_named_member array[]= {
      {"index_name", Read_string(thd, &out->idx_name), false},
      {"ranges", Read_list_of_ranges(thd, &out->range_list), false},
      {"num_rows",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&out->rows),
       false},
      {"cost", Read_range_cost_estimate(thd, &out->cost), false},
      {"max_index_blocks",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&out->max_index_blocks),
       false},
      {"max_row_blocks",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&out->max_row_blocks),
       false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the cost information present in the
  range context of the JSON structure.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static bool parse_range_cost_estimate(THD *thd, json_engine_t *je,
                                      String *err_buf, Cost_estimate *cost)
{
  if (json_scan_next(je) || je->state != JST_OBJ_START)
  {
    err_buf->append(
        STRING_WITH_LEN("Expected an object while reading range cost"));
    return 1;
  }

  Read_named_member array[]= {
      {"avg_io_cost", Read_double(&cost->avg_io_cost), false},
      {"cpu_cost", Read_double(&cost->cpu_cost), false},
      {"comp_cost", Read_double(&cost->comp_cost), false},
      {"copy_cost", Read_double(&cost->copy_cost), false},
      {"limit_cost", Read_double(&cost->limit_cost), false},
      {"setup_cost", Read_double(&cost->setup_cost), false},
      {"index_cost_io", Read_double(&cost->index_cost.io), false},
      {"index_cost_cpu", Read_double(&cost->index_cost.cpu), false},
      {"row_cost_io", Read_double(&cost->row_cost.io), false},
      {"row_cost_cpu", Read_double(&cost->row_cost.cpu), false},
      {NULL, Read_double(NULL), true}};

  return json_read_object(je, array, err_buf);
}

/*
  Parses the cost information for reading an index using
  ref access of the JSON structure of the optimizer context.
  To be specific, single array element of list_index_read_costs
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_index_read_cost_context(THD *thd, json_engine_t *je,
                                         String *err_buf,
                                         cost_index_read_call_record *out)
{
  const char *err_msg= "Expected an object in the index_read_costs array";

  Read_named_member array[]= {
      {"key_number", Read_non_neg_integer<uint, UINT_MAX>(&out->key),
       false},
      {"num_records",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&out->records), false},
      {"eq_ref", Read_non_neg_integer<bool, 1>(&out->eq_ref), false},
      {"index_cost_io", Read_double(&out->cost.index_cost.io), false},
      {"index_cost_cpu", Read_double(&out->cost.index_cost.cpu), false},
      {"row_cost_io", Read_double(&out->cost.row_cost.io), false},
      {"row_cost_cpu", Read_double(&out->cost.row_cost.cpu), false},
      {"max_index_blocks",
       Read_non_neg_integer<longlong, LONGLONG_MAX>(
           &out->cost.max_index_blocks),
       false},
      {"max_row_blocks",
       Read_non_neg_integer<longlong, LONGLONG_MAX>(
           &out->cost.max_row_blocks),
       false},
      {"copy_cost", Read_double(&out->cost.copy_cost), false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

/*
  Parses the cost information for reading records_in_range
  JSON structure of the optimizer context.
  To be specific, single array element of list_records_in_range
  is parsed in this method.
  Refer to the file opt_context_schema.inc, and
  the description at the start of this file.

  @return
    0  OK
    1  Parse Error
   -1  EOF
*/
static int parse_records_in_range_context(THD *thd, json_engine_t *je,
                                          String *err_buf,
                                          records_in_range_call_record *out)
{
  const char *err_msg= "Expected an object in the records_in_range array";

  Read_named_member array[]= {
      {"key_number", Read_non_neg_integer<uint, UINT_MAX>(&out->keynr),
       false},
      {"min_key", Read_string(thd, &out->min_key), false},
      {"max_key", Read_string(thd, &out->max_key), false},
      {"num_records",
       Read_non_neg_integer<ha_rows, ULONGLONG_MAX>(&out->records), false},
      {NULL, Read_double(NULL), true}};

  return parse_context_obj_from_json_array(je, err_buf, err_msg, array);
}

Optimizer_context_replay::Optimizer_context_replay(THD *thd_arg) : thd(thd_arg)
{
  parse(); // TODO: error handling?
}

static bool sql_command_uses_opt_context(enum_sql_command sql_command)
{
  return (sql_command == SQLCOM_SELECT ||
          sql_command == SQLCOM_INSERT_SELECT ||
          sql_command == SQLCOM_DELETE ||
          sql_command == SQLCOM_UPDATE ||
          sql_command == SQLCOM_DELETE_MULTI ||
          sql_command == SQLCOM_UPDATE_MULTI);
}

void init_optimizer_context_replay_if_needed(THD *thd)
{
  /* If @@optimizer_replay_context is not empty, start the replay  */
  if (thd->variables.optimizer_replay_context &&
      strlen(thd->variables.optimizer_replay_context) > 0 &&
      sql_command_uses_opt_context(thd->lex->sql_command))
  {
    thd->opt_ctx_replay= new Optimizer_context_replay(thd);
  }
}

void init_optimizer_context_recorder_if_needed(THD *thd)
{
  if (thd->spcont)
  {
    /* This is a sub-statement inside SP. Don't do anything */
    return;
  }
  if (!thd->variables.optimizer_record_context)
  {
    clean_captured_ctx(thd);
    return ;
  }
  // Recorder is cleaned up after query in THD::cleanup_after_query()
  DBUG_ASSERT(!thd->opt_ctx_recorder);

  LEX *lex= thd->lex;
  if (sql_command_uses_opt_context(lex->sql_command))
  {
    thd->opt_ctx_recorder= new Optimizer_context_recorder(thd->mem_root);
  }
  else if (lex->sql_command != SQLCOM_SET_OPTION)
  {
    clean_captured_ctx(thd);
  }
}

/*
  search the in memory representation of the parsed contents
  of replay json context, and set read_cost for the given table.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_read_cost(const TABLE *tbl,
                                                IO_AND_CPU_COST *cost)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String tbl_name;
  append_full_table_name(tbl->pos_in_table_list, &tbl_name);

  if (table_context_for_replay *tbl_ctx=
          find_table_context(tbl_name.c_ptr_safe()))
  {
    cost->io= tbl_ctx->read_cost_io;
    cost->cpu= tbl_ctx->read_cost_cpu;
    return false;
  }

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      tbl_name.c_ptr_safe(), "list of table contexts");
  return true;
}

/*
  search the list of range stats from the in memory representation of the
  parsed replay json context, for the given table_name, and index_name.
  If they are found, then compare the ranges one by one until all of them
  match. If so, load the num_records, and the computation cost associated
  with it into the arguments passed.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_range_stats(
    TABLE *table, uint keynr, RANGE_SEQ_IF *seq_if, SEL_ARG_RANGE_SEQ *seq,
    Cost_estimate *cost, ha_rows *rows, ha_rows *max_index_blocks,
    ha_rows *max_row_blocks)
{
  if (!has_records() || !is_base_table(table->pos_in_table_list))
    return true;

  KEY *keyinfo= table->key_info + keynr;
  const char *idx_name= keyinfo->name.str;
  const KEY_PART_INFO *key_part= keyinfo->key_part;
  uint n_key_parts= table->actual_n_key_parts(keyinfo);
  KEY_MULTI_RANGE multi_range;
  range_seq_t seq_it;
  List<Multi_range_read_const_call_record> mrr_const_calls;
  store_range_contexts(table, idx_name, &mrr_const_calls);
  String act_ranges;
  seq_it= seq_if->init((void *) seq, 0, 0);
  act_ranges.append(STRING_WITH_LEN("["));

  while (!seq_if->next(seq_it, &multi_range))
  {
    StringBuffer<128> range_info(system_charset_info);
    print_range(&range_info, key_part, &multi_range, n_key_parts);
    char *r1= range_info.c_ptr_safe();
    act_ranges.append(r1, strlen(r1));
    act_ranges.append(STRING_WITH_LEN(", "));
  }

  act_ranges.append(STRING_WITH_LEN("]"));

  if (!mrr_const_calls.is_empty())
  {
    List_iterator<Multi_range_read_const_call_record> range_ctx_itr(mrr_const_calls);
    while (Multi_range_read_const_call_record *range_ctx= range_ctx_itr++)
    {
      List_iterator<char> range_itr(range_ctx->range_list);
      seq_it= seq_if->init((void *) seq, 0, 0);
      bool matched= true;

      while (!seq_if->next(seq_it, &multi_range))
      {
        StringBuffer<128> range_info(system_charset_info);
        print_range(&range_info, key_part, &multi_range, n_key_parts);
        char *r1= range_info.c_ptr_safe();
        char *r2= range_itr++;
        if (r2 == NULL || strcmp(r1, r2) != 0)
        {
          matched= false;
          break;
        }
      }
      if (matched)
      {
        *cost= range_ctx->cost;
        *rows= range_ctx->rows;
        *max_index_blocks= range_ctx->max_index_blocks;
        *max_row_blocks= range_ctx->max_row_blocks;
        return false;
      }
    }

    String arg1;
    String arg2;
    String tbl_name;
    append_full_table_name(table->pos_in_table_list, &tbl_name);
    arg1.append(STRING_WITH_LEN("the given list of ranges i.e. "));
    arg1.append(act_ranges);
    arg2.append(STRING_WITH_LEN("the list of ranges for table_name "));
    arg2.append(tbl_name);
    arg2.append(STRING_WITH_LEN(" and index_name "));
    arg2.append(idx_name, strlen(idx_name));
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
        ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
        arg1.c_ptr_safe(), arg2.c_ptr_safe());
  }
  return true;
}

/*
  search the index read cost info from the in memory representation of the
  parsed replay json context, for the given table, keynr, records, and eq_ref,
  and set it into the cost if found.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_index_read_cost(const TABLE *tbl,
                                                      uint keynr,
                                                      ha_rows records,
                                                      bool eq_ref,
                                                      ALL_READ_COST *cost)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String tbl_name;
  append_full_table_name(tbl->pos_in_table_list, &tbl_name);

  if (table_context_for_replay *tbl_ctx=
          find_table_context(tbl_name.c_ptr_safe()))
  {
    List_iterator<cost_index_read_call_record> irc_itr(tbl_ctx->irc_list);
    while (cost_index_read_call_record *rec= irc_itr++)
    {
      if (rec->key == keynr && rec->records == records &&
          rec->eq_ref == eq_ref)
      {
        *cost= rec->cost;
        return false;
      }
    }
  }

  String warn_msg;
  warn_msg.append(tbl_name);
  warn_msg.append(STRING_WITH_LEN(" with key_number:"));
  warn_msg.append(keynr);
  warn_msg.append(STRING_WITH_LEN(", records:"));
  warn_msg.q_append_int64(records);
  warn_msg.append(STRING_WITH_LEN(", eq_ref:"));
  warn_msg.append(eq_ref);
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      warn_msg.c_ptr_safe(), "list_index_read_costs");
  return true;
}


/*
  @brief
    Infuse saved table statistics for a given table.
    Current table statistics are saved away to be restored later.
    #records is not handled by this function, see infuse_table_rows().
*/
void Optimizer_context_replay::infuse_table_stats(TABLE *table)
{
  if (!has_records() || !is_base_table(table->pos_in_table_list))
    return;

  Saved_Table_stats *saved_ts= new Saved_Table_stats();

  if (unlikely(!saved_ts))
    return; // OOM

  saved_ts->table= table;

  if (saved_table_stats.push_back(saved_ts))
    return;

  KEY *key_info, *key_info_end;
  for (key_info= table->key_info, key_info_end= key_info + table->s->keys;
       key_info < key_info_end; key_info++)
  {
    List<ha_rows> *index_freq_list=
        get_index_rec_per_key_list(table, key_info->name.str);

    if (!index_freq_list || index_freq_list->is_empty())
      continue;

    Saved_Index_stats *saved_is= new Saved_Index_stats();

    if (unlikely(!saved_is))
      return; // OOM

    uint i= 0;
    uint num_key_parts= key_info->user_defined_key_parts;
    Index_statistics *original_read_stats= key_info->read_stats;
    bool original_is_statistics_from_stat_tables=
        key_info->is_statistics_from_stat_tables;
    Index_statistics *new_read_stats= new Index_statistics();

    if (unlikely(!new_read_stats))
      return; // OOM

    ulonglong *frequencies=
        (ulonglong *) thd->alloc(sizeof(ulonglong) * num_key_parts);

    if (unlikely(!frequencies))
      return; // OOM

    new_read_stats->init_avg_frequency(frequencies);
    List_iterator li(*index_freq_list);
    ha_rows *freq= li++;
    key_info->read_stats= new_read_stats;

    while (freq && i < num_key_parts)
    {
      // Apparently this can be=0 for prefix indexes.
      //DBUG_ASSERT(*freq > 0);
      key_info->read_stats->set_avg_frequency(i, (double) *freq);
      freq= li++;
      i++;
    }

    key_info->is_statistics_from_stat_tables= true;
    saved_is->key_info= key_info;
    saved_is->original_is_statistics_from_stat_tables=
        original_is_statistics_from_stat_tables;
    saved_is->original_read_stats= original_read_stats;
    saved_ts->saved_indexstats_list.push_back(saved_is);
  }
}

bool Optimizer_context_replay::infuse_records_in_range(
    const TABLE *tbl, const KEY_PART_INFO *key_part, uint keynr,
    const key_range *min_range, const key_range *max_range, ha_rows *records)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String min_key;
  String max_key;
  String tbl_name;
  print_key_value(&min_key, key_part, min_range->key, min_range->length);
  print_key_value(&max_key, key_part, min_range->key, min_range->length);
  append_full_table_name(tbl->pos_in_table_list, &tbl_name);

  if (table_context_for_replay *tbl_ctx=
          find_table_context(tbl_name.c_ptr_safe()))
  {
    List_iterator<records_in_range_call_record> iter(tbl_ctx->rir_list);
    while (records_in_range_call_record *rec= iter++)
    {
      if (rec->keynr == keynr &&
          !strcmp(rec->min_key, min_key.c_ptr_safe()) &&
          !strcmp(rec->max_key, max_key.c_ptr_safe()))
      {
        *records= rec->records;
        return false;
      }
    }
  }

  String warn_msg;
  warn_msg.append(tbl_name);
  warn_msg.append(STRING_WITH_LEN(" with key_number:"));
  warn_msg.append(keynr);
  warn_msg.append(STRING_WITH_LEN(" with min_key:"));
  warn_msg.append(min_key);
  warn_msg.append(STRING_WITH_LEN(" with max_key:"));
  warn_msg.append(max_key);
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      warn_msg.c_ptr_safe(), "list_records_in_range");
  return true;
}

/*
  @brief
    restore the saved stats for the tables, and indexes that were
    earlier recorded using infuse_table_stats()
*/
void Optimizer_context_replay::restore_modified_table_stats()
{
  List_iterator<Saved_Table_stats> table_li(saved_table_stats);
  while (Saved_Table_stats *saved_ts= table_li++)
  {
    List_iterator<Saved_Index_stats> index_li(saved_ts->saved_indexstats_list);
    while (Saved_Index_stats *saved_is= index_li++)
    {
      KEY *key= saved_is->key_info;
      key->is_statistics_from_stat_tables=
          saved_is->original_is_statistics_from_stat_tables;
      key->read_stats= saved_is->original_read_stats;
    }
  }
}

/*
  Returns if the in memory representation of the
  parsed replay json context contain any records
*/
bool Optimizer_context_replay::has_records() { return !ctx_list.is_empty(); }

/*
  parse the replay json context that abides to the structure defined in
  opt_context_schema.inc

  @return
    FALSE  OK
    TRUE  Parse Error
*/
bool Optimizer_context_replay::parse()
{
  json_engine_t je;
  char *context= NULL;
  String str;
  String *value;
  String err_buf;
  user_var_entry *var;
  char *var_name= thd->variables.optimizer_replay_context;
  LEX_CSTRING varname= {var_name, strlen(var_name)};

  Read_named_member array[]= {{"list_contexts",
                               Read_list_of_context<table_context_for_replay>(
                                   thd, &ctx_list, parse_table_context),
                               false},
                              {NULL, Read_double(NULL), true}};

  DBUG_ENTER("Optimizer_context_replay::parse");

  if ((var= get_variable(&thd->user_vars, &varname, FALSE)))
  {
    bool null_value;
    value= var->val_str(&null_value, &str, 0);
    if (null_value || !value->length())
      goto err;
  }
  else
  {
    goto err;
  }

  context= value->c_ptr_safe();
  mem_root_dynamic_array_init(thd->mem_root, PSI_INSTRUMENT_MEM, &je.stack,
                              sizeof(int), NULL, JSON_DEPTH_DEFAULT,
                              JSON_DEPTH_INC, MYF(0));

  json_scan_start(&je, system_charset_info, (const uchar *) context,
                  (const uchar *) context + strlen(context));

  if (json_scan_next(&je))
  {
    err_buf.append(STRING_WITH_LEN("JSON parse error"));
    goto err;
  }

  if (je.state != JST_OBJ_START)
  {
    err_buf.append(STRING_WITH_LEN("Root JSON element must be a JSON object"));
    goto err;
  }

  if (json_read_object(&je, array, &err_buf))
    goto err;

#ifndef DBUG_OFF
  dbug_print_read_stats();
#endif
  DBUG_RETURN(false); // Ok
err:
  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_PARSE_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_PARSE_FAILED),
      err_buf.c_ptr_safe(), (je.s.c_str - (const uchar *) context));
  DBUG_RETURN(true);
}

#ifndef DBUG_OFF
/*
  Print the contents of the stats that are read from the replay json context
*/
void Optimizer_context_replay::dbug_print_read_stats()
{
  DBUG_ENTER("Optimizer_context_replay::print()");
  DBUG_PRINT("info", ("----------Printing Stored Context-------------"));
  List_iterator<table_context_for_replay> table_itr(ctx_list);

  while (table_context_for_replay *tbl_ctx= table_itr++)
  {
    DBUG_PRINT("info", ("New Table Context"));
    DBUG_PRINT("info", ("-----------------"));
    DBUG_PRINT("info", ("name: %s", tbl_ctx->name));
    DBUG_PRINT("info",
               ("file_stat_records: %llx", tbl_ctx->file_stat_records));

    List_iterator<index_context_for_replay> index_itr(tbl_ctx->index_list);

    while (index_context_for_replay *idx_ctx= index_itr++)
    {
      DBUG_PRINT("info", ("...........New Index Context........."));
      DBUG_PRINT("info", ("index_name: %s", idx_ctx->idx_name));
      DBUG_PRINT("info", ("list_rec_per_key: [ "));
      List_iterator<ha_rows> rec_itr(idx_ctx->list_rec_per_key);
      while (true)
      {
        ha_rows *num_rec= rec_itr++;
        if (!num_rec)
          break;
        DBUG_PRINT("info", ("%llx, ", *num_rec));
      }
      DBUG_PRINT("info", ("]"));
    }

    List_iterator<Multi_range_read_const_call_record> range_itr(tbl_ctx->ranges_list);

    while (Multi_range_read_const_call_record *call_rec= range_itr++)
    {
      DBUG_PRINT("info", ("...........New Range Context........."));
      DBUG_PRINT("info", ("index_name: %s", call_rec->idx_name));
      DBUG_PRINT("info", ("ranges: [ "));

      List_iterator<char> range_itr(call_rec->range_list);

      while (true)
      {
        char *range= range_itr++;
        if (!range)
          break;
        DBUG_PRINT("info", ("%s, ", range));
      }

      DBUG_PRINT("info", ("]"));
      DBUG_PRINT("info", ("num_rows: %llx", call_rec->rows));
      {
        DBUG_PRINT("info", ("avg_io_cost: %f", call_rec->cost.avg_io_cost));
        DBUG_PRINT("info", ("cpu_cost: %f", call_rec->cost.cpu_cost));
        DBUG_PRINT("info", ("comp_cost: %f", call_rec->cost.comp_cost));
        DBUG_PRINT("info", ("copy_cost: %f", call_rec->cost.copy_cost));
        DBUG_PRINT("info", ("limit_cost: %f", call_rec->cost.limit_cost));
        DBUG_PRINT("info", ("setup_cost: %f", call_rec->cost.setup_cost));
        DBUG_PRINT("info",
                   ("index_cost.io: %f", call_rec->cost.index_cost.io));
        DBUG_PRINT("info",
                   ("index_cost.cpu: %f", call_rec->cost.index_cost.cpu));
        DBUG_PRINT("info", ("row_cost.io: %f", call_rec->cost.row_cost.io));
        DBUG_PRINT("info", ("row_cost.cpu: %f", call_rec->cost.row_cost.cpu));
      }
      DBUG_PRINT("info",
                 ("max_index_blocks: %llx", call_rec->max_index_blocks));
      DBUG_PRINT("info", ("max_row_blocks: %llx", call_rec->max_row_blocks));
    }

    List_iterator<cost_index_read_call_record> irc_itr(tbl_ctx->irc_list);

    for (cost_index_read_call_record *rec= irc_itr++; rec; rec= irc_itr++)
    {
      DBUG_PRINT("info", ("...........New Index Read Cost Context........."));
      DBUG_PRINT("info", ("key_number: %u", rec->key));
      DBUG_PRINT("info", ("num_records: %llx", rec->records));
      DBUG_PRINT("info", ("eq_ref: %d", rec->eq_ref));
      {
        DBUG_PRINT("info", ("index_cost_io: %f", rec->cost.index_cost.io));
        DBUG_PRINT("info", ("index_cost_cpu: %f", rec->cost.index_cost.cpu));
        DBUG_PRINT("info", ("row_cost_io: %f", rec->cost.row_cost.io));
        DBUG_PRINT("info", ("row_cost_cpu: %f", rec->cost.row_cost.cpu));
        DBUG_PRINT("info",
                   ("max_index_blocks: %llx", rec->cost.max_index_blocks));
        DBUG_PRINT("info", ("max_row_blocks: %llx", rec->cost.max_row_blocks));
        DBUG_PRINT("info", ("copy_cost: %f", rec->cost.copy_cost));
      }
    }
  }
  DBUG_VOID_RETURN;
}
#endif

/*
  store the extracted contents from the in memory representation of the
  parsed replay json context, into the variable rows.

  @return
    false  OK
    true  Error
*/
bool Optimizer_context_replay::infuse_table_rows(TABLE *tbl)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return true;

  String tbl_name;
  append_full_table_name(tbl->pos_in_table_list, &tbl_name);

  if (table_context_for_replay *tbl_ctx=
          find_table_context(tbl_name.c_ptr_safe()))
  {
    // Only infuse this one. table->used_stat_records are set by te SQL layer.
    tbl->file->stats.records= tbl_ctx->file_stat_records;
    return false;
  }

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      tbl_name.c_ptr_safe(), "list of table contexts");
  return true;
}

/*
  check the extracted contents from from the in memory representation of the
  parsed replay json context, and return the List of number of records per key
  for the given table and index name
*/
List<ha_rows> *
Optimizer_context_replay::get_index_rec_per_key_list(const TABLE *tbl,
                                                     const char *idx_name)
{
  if (!has_records() || !is_base_table(tbl->pos_in_table_list))
    return NULL;

  String tbl_name;
  append_full_table_name(tbl->pos_in_table_list, &tbl_name);

  if (table_context_for_replay *tbl_ctx=
          find_table_context(tbl_name.c_ptr_safe()))
  {
    List_iterator<index_context_for_replay> index_itr(tbl_ctx->index_list);
    while (index_context_for_replay *idx_ctx= index_itr++)
    {
      if (strcmp(idx_name, idx_ctx->idx_name) == 0)
      {
        return &idx_ctx->list_rec_per_key;
      }
    }
  }

  String name;
  name.append(tbl_name);
  name.append(STRING_WITH_LEN("."));
  name.append(idx_name, strlen(idx_name));

  push_warning_printf(
      thd, Sql_condition::WARN_LEVEL_WARN,
      ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
      ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
      name.c_ptr_safe(), "list of index contexts");
  return NULL;
}

/*
  check the extracted contents from the in memory representation of the
  parsed replay json context, and add the range contexts for the given table,
  and index to the list
*/
void Optimizer_context_replay::store_range_contexts(
    const TABLE *tbl, const char *idx_name,
    List<Multi_range_read_const_call_record> *out)
{
  if (!has_records() || !out)
    return;

  String tbl_name;
  append_full_table_name(tbl->pos_in_table_list, &tbl_name);

  if (table_context_for_replay *tbl_ctx=
          find_table_context(tbl_name.c_ptr_safe()))
  {
    List_iterator<Multi_range_read_const_call_record> range_ctx_itr(
        tbl_ctx->ranges_list);
    while (Multi_range_read_const_call_record *range_ctx= range_ctx_itr++)
    {
      if (!strcmp(idx_name, range_ctx->idx_name))
        out->push_back(range_ctx);
    }
  }

  if (out->is_empty())
  {
    String name;
    name.append(tbl_name);
    name.append(STRING_WITH_LEN("."));
    name.append(idx_name, strlen(idx_name));
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED,
        ER_THD(thd, ER_JSON_OPTIMIZER_REPLAY_CONTEXT_MATCH_FAILED),
        name.c_ptr_safe(), "list of range contexts");
  }
}

table_context_for_replay *
Optimizer_context_replay::find_table_context(const char *name)
{
  List_iterator<table_context_for_replay> table_itr(ctx_list);

  while (table_context_for_replay *tbl_ctx= table_itr++)
  {
    if (!strcmp(name, tbl_ctx->name))
      return tbl_ctx;
  }
  return nullptr;
}

Optimizer_context_capture::Optimizer_context_capture(THD *thd, String &ctx_arg)
{
  query.copy(thd->query(), thd->query_length(), thd->query_charset());
  ctx.copy(ctx_arg);
}


/*
  @brief
    Put the SQL script from thd->captured_opt_ctx into I_S.OPTIMIZER_CONTEXT
    pseudo-table.
*/

int fill_optimizer_context_capture_info(THD *thd, TABLE_LIST *tables, Item *)
{
  TABLE *table= tables->table;

  Optimizer_context_capture *captured_ctx= thd->captured_opt_ctx;

  if (captured_ctx)
  {
    table->field[0]->store(captured_ctx->query.c_ptr_safe(),
                           static_cast<uint>(captured_ctx->query.length()),
                           captured_ctx->query.charset());
    table->field[1]->store(captured_ctx->ctx.c_ptr_safe(),
                           static_cast<uint>(captured_ctx->ctx.length()),
                           system_charset_info);
    //  Store in IS
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}

void clean_captured_ctx(THD *thd)
{
  delete thd->captured_opt_ctx;
  thd->captured_opt_ctx= nullptr;
}
