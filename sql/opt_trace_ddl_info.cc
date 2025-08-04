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

#include "opt_trace_ddl_info.h"
#include "sql_show.h"
#include "my_json_writer.h"
#include "sql_table.h"
#include "mysql.h"
#include "hash.h"

/**
  @file

  @brief
    Stores the ddls of the tables, and views that are used
    in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer trace. All the ddls are stored together
    at one place as a JSON array object with name "list_ddls"
*/

static void store_full_table_name(THD *thd, TABLE_LIST *tbl, String *buf);

struct DDL_Key
{
  char *name; //full name of the table or view
  size_t name_len;
};

/*
  helper function to know the key portion of the record
  that is stored in hash.
*/
static const uchar *get_rec_key(const void *entry_, size_t *length,
                                my_bool flags)
{
  auto entry= static_cast<const DDL_Key *>(entry_);
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
static bool is_base_table(TABLE_LIST *tbl)
{
  return
    (tbl->table &&
     tbl->table->s &&
     !tbl->table_function &&
     !tbl->schema_table &&
     get_table_category(tbl->get_db_name(), tbl->get_table_name()) ==
       TABLE_CATEGORY_USER &&
     tbl->table->s->tmp_table != INTERNAL_TMP_TABLE &&
     tbl->table->s->tmp_table != SYSTEM_TMP_TABLE);
}

static bool dump_name_ddl_to_trace(THD *thd, DDL_Key *ddl_key, String *stmt,
                                   Json_writer_object &ctx_wrapper)
{
  ctx_wrapper.add("name", ddl_key->name);
  size_t non_esc_stmt_len= stmt->length();
  /*
    making escape_stmt size to be 4 times the non_esc_stmt
    4 is chosen as a worst case although 3 should suffice.
    "'" would be escaped to \"\'\"
  */
  size_t len_multiplier= sizeof(uint32_t);
  size_t escape_stmt_len= len_multiplier * non_esc_stmt_len;
  char *escaped_stmt= (char *) thd->alloc(escape_stmt_len + 1);

  if (!escaped_stmt)
    return true;

  int act_escape_stmt_len=
      json_escape_string(stmt->c_ptr(), stmt->c_ptr() + non_esc_stmt_len,
                         escaped_stmt, escaped_stmt + escape_stmt_len);

  if (act_escape_stmt_len < 0)
    return true;

  escaped_stmt[act_escape_stmt_len]= 0;
  ctx_wrapper.add("ddl", escaped_stmt);
  return false;
}

static void dump_index_range_stats_to_trace(THD *thd, uchar *tbl_name,
                                            size_t tbl_name_len)
{
  if (!thd->stats_ctx_recorder || !thd->stats_ctx_recorder->has_records())
    return;

  trace_table_index_range_context *context=
      thd->stats_ctx_recorder->search(tbl_name, tbl_name_len);

  if (!context)
    return;

  Json_writer_array list_ranges_wrapper(thd, "list_ranges");
  List_iterator irc_li(context->index);
  while (trace_index_range_context *irc= irc_li++)
  {
    Json_writer_object irc_wrapper(thd);
    irc_wrapper.add("index_name", irc->idx_name);
    List_iterator rc_li(*irc->list_range_context);
    Json_writer_array ranges_wrapper(thd, "ranges");
    while (trace_range_context *rc= rc_li++)
    {
      ranges_wrapper.add(rc->range, strlen(rc->range));
    }
    ranges_wrapper.end();
    irc_wrapper.add("num_rows", irc->num_records);
  }
}

/*
  dump the following table stats to trace: -
  1. total number of records in the table
  2. if there any indexes for the table then
      their names, and the num of records per key
  3. range stats on the indexes
*/
static void dump_table_stats_to_trace(THD *thd, TABLE_LIST *tbl,
                                      uchar *tbl_name,
                                      size_t tbl_name_len,
                                      Json_writer_object &ctx_wrapper)
{
  TABLE *table= tbl->table;
  ctx_wrapper.add("num_of_records", tbl->table->stat_records());

  if (!table->key_info)
    return;

  Json_writer_array indexes_wrapper(thd, "indexes");
  for (uint idx= 0; idx < table->s->keys; idx++)
  {
    KEY key= table->key_info[idx];
    uint num_key_parts= key.user_defined_key_parts;
    Json_writer_object index_wrapper(thd);
    index_wrapper.add("index_name", key.name);
    Json_writer_array rpk_wrapper(thd, "rec_per_key");
    for (uint i= 0; i < num_key_parts; i++)
    {
      rpk_wrapper.add(key.actual_rec_per_key(i));
    }
  }
  indexes_wrapper.end();
  dump_index_range_stats_to_trace(thd, tbl_name, tbl_name_len);
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
  @brief
    Dump definitions, basic stats of all tables and views used by the
    statement into the optimizer trace.
    The goal is to eventually save everything that is needed to
    reproduce the query execution

  @detail
    Stores the ddls, stats of the tables, and views that are used
    in either SELECT, INSERT, DELETE, and UPDATE queries,
    into the optimizer trace.
    Global query_tables are read in reverse order from the thd->lex,
    and a record with table_name, and ddl of the table are created.
    Hash is used to store the records, where in no duplicates
    are stored. db_name.table_name is used as a key to discard any
    duplicates. If a new record that is created is not in the hash,
    then that is dumped into the trace.

  @return
    false when no error occurred during the computation
*/
bool store_tables_context_in_trace(THD *thd)
{
  LEX *lex= thd->lex;
  if (!(thd->variables.optimizer_trace &&
        thd->variables.optimizer_record_context &&
        (lex->sql_command == SQLCOM_SELECT ||
         lex->sql_command == SQLCOM_INSERT_SELECT ||
         lex->sql_command == SQLCOM_DELETE ||
         lex->sql_command == SQLCOM_UPDATE ||
         lex->sql_command == SQLCOM_DELETE_MULTI ||
         lex->sql_command == SQLCOM_UPDATE_MULTI)))
    return false;

  if (lex->query_tables == *(lex->query_tables_last))
    return false;

  Json_writer_object main_wrapper(thd);
  main_wrapper.add("current_database", thd->get_db());
  Json_writer_array context_list(thd, "list_contexts");  
  HASH hash;
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
  my_hash_init(key_memory_trace_ddl_info, &hash, system_charset_info, 16, 0, 0,
               get_rec_key, NULL, HASH_UNIQUE);
  bool res= false;
  for (TABLE_LIST *tbl= li++; tbl; li.remove(), tbl= li++)
  {
    String ddl;
    String name;
    DDL_Key *ddl_key;
    store_full_table_name(thd, tbl, &name);

    /*
      A query can use the same table multiple times. Do not dump the DDL
      multiple times.
    */
    if (my_hash_search(&hash, (uchar *) name.c_ptr(), name.length()))
      continue;

    if (!(ddl_key= (DDL_Key *) thd->alloc(sizeof(DDL_Key))))
    {
      res= true;
      break;
    }

    if (tbl->is_view())
      create_view_def(thd, tbl, &name, &ddl);
    else
    {
      if (show_create_table(thd, tbl, &ddl, NULL, WITH_DB_NAME))
      {
        res= true;
        break;
      }
    }

    ddl_key->name= create_new_copy(thd, name.c_ptr());
    ddl_key->name_len= name.length();
    my_hash_insert(&hash, (uchar *) ddl_key);
    Json_writer_object ctx_wrapper(thd);

    if (dump_name_ddl_to_trace(thd, ddl_key, &ddl, ctx_wrapper))
    {
      res= true;
      break;
    }

    if (!tbl->is_view())
    {
      dump_table_stats_to_trace(thd, tbl, (uchar *) ddl_key->name,
                                ddl_key->name_len, ctx_wrapper);
    }
  }
  my_hash_free(&hash);
  return res;
}


Optimizer_Stats_Context_Recorder::Optimizer_Stats_Context_Recorder(THD *thd)
{
  tbl_trace_ctx_hash= (HASH *) thd->calloc(sizeof(HASH));
  my_hash_init(key_memory_trace_ddl_info, tbl_trace_ctx_hash,
               system_charset_info, 16, 0, 0, get_tbl_trace_ctx_key, 0,
               HASH_UNIQUE);
}

Optimizer_Stats_Context_Recorder::~Optimizer_Stats_Context_Recorder()
{
  clear();
}

void Optimizer_Stats_Context_Recorder::clear()
{
  if (tbl_trace_ctx_hash != NULL)
    my_hash_free(tbl_trace_ctx_hash);
  tbl_trace_ctx_hash= NULL;
}

bool Optimizer_Stats_Context_Recorder::has_records()
{
  return this->tbl_trace_ctx_hash && this->tbl_trace_ctx_hash->records > 0;
}

trace_table_index_range_context *
Optimizer_Stats_Context_Recorder::search(uchar *tbl_name, size_t tbl_name_len)
{
  return (trace_table_index_range_context *) my_hash_search(
      this->tbl_trace_ctx_hash, tbl_name, tbl_name_len);
}

void Optimizer_Stats_Context_Recorder::record_ranges_for_tbl(
    THD *thd, TABLE_LIST *tbl, size_t found_records, const char *index_name,
    List<char> *range_list)
{
  String tbl_name;

  List<trace_range_context> *list_trc=
      (List<trace_range_context> *) thd->calloc(
          sizeof(List<trace_range_context>));
  list_trc->empty();
  List_iterator<char> li(*range_list);
  char *range;
  while ((range=li++))
  {
    trace_range_context *trc=
        (trace_range_context *) thd->calloc(sizeof(trace_range_context));
    trc->range= range;
    list_trc->push_back(trc);
  }
  /*
    Create a new table context if it is not already present in the
    hash.
    Store the ranges of every index of the table into the
    table context.
    The table context is also persisted in the hash which is to be
    used later for dumping all the context infomation into the trace.
  */
  store_full_table_name(thd, tbl, &tbl_name);
  trace_table_index_range_context *table_ctx= thd->stats_ctx_recorder->search(
      (uchar *) tbl_name.c_ptr(), tbl_name.length());

  if (!table_ctx)
  {
    table_ctx= (trace_table_index_range_context *) thd->calloc(
        sizeof(trace_table_index_range_context));
    table_ctx->name= create_new_copy(thd, tbl_name.c_ptr());
    table_ctx->name_len= tbl_name.length();
    table_ctx->index.empty();
    my_hash_insert(this->tbl_trace_ctx_hash, (uchar *) table_ctx);
  }

  trace_index_range_context *index_ctx=
      (trace_index_range_context *) thd->calloc(
          sizeof(trace_index_range_context));
  index_ctx->idx_name= create_new_copy(thd, index_name);
  index_ctx->list_range_context= list_trc;
  index_ctx->num_records= found_records;
  table_ctx->index.push_back(index_ctx);
}

/*
  helper function to know the key portion of the
  trace table context that is stored in hash.
*/
const uchar *get_tbl_trace_ctx_key(const void *entry_, size_t *length,
                                   my_bool flags)
{
  auto entry= static_cast<const trace_table_index_range_context *>(entry_);
  *length= entry->name_len;
  return reinterpret_cast<const uchar *>(entry->name);
}

/*
  store full table name i.e. "db_name.table_name",
  into the supplied variable buf
*/
static void store_full_table_name(THD *thd, TABLE_LIST *tbl, String *buf)
{
  buf->append(tbl->get_db_name().str, tbl->get_db_name().length);
  buf->append(STRING_WITH_LEN("."));
  buf->append(tbl->get_table_name().str, tbl->get_table_name().length);
}

/*
  return a new c style string by allocating memory
  from the heap and copying the contents of buf into it.
  uses thd's mem_root for allocating the memory
*/
char *create_new_copy(THD *thd, const char *buf)
{
  char *name_copy= (char *) thd->calloc(strlen(buf) + 1);
  strcpy(name_copy, buf);
  return name_copy;
}
