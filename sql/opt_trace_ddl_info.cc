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
#include "sql_list.h"
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
  ctx_wrapper.add("name", ddl_key->name);
  return false;
}

static void dump_stats_to_trace(THD *thd, TABLE_LIST *tbl,
                                Json_writer_object &ctx_wrapper)
{
  ctx_wrapper.add("num_of_records", tbl->table->stat_records());
  TABLE *table= tbl->table;
  if (table->key_info)
  {
    Json_writer_array indexes(thd, "indexes");
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
  }
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
    char *name_copy;

    /*
      A query can use the same table multiple times. Do not dump the DDL
      multiple times.
    */
    name.append(tbl->get_db_name().str, tbl->get_db_name().length);
    name.append(STRING_WITH_LEN("."));
    name.append(tbl->get_table_name().str, tbl->get_table_name().length);

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

    name_copy= (char *) thd->alloc(name.length() + 1);
    strcpy(name_copy, name.c_ptr());
    ddl_key->name= name_copy;
    ddl_key->name_len= name.length();
    my_hash_insert(&hash, (uchar *) ddl_key);
    Json_writer_object ctx_wrapper(thd);

    if (dump_name_ddl_to_trace(thd, ddl_key, &ddl, ctx_wrapper))
    {
      res= true;
      break;
    }

    if (!tbl->is_view())
      dump_stats_to_trace(thd, tbl, ctx_wrapper);
  }
  my_hash_free(&hash);
  return res;
}
