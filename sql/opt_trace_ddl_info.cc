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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

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

struct DDL_Info_Record
{
  char *name;
  char *stmt;
};

/*
  helper function to know the key portion of the record
  that is stored in hash, and record list.
*/
static const uchar *get_rec_key(const void *entry_, size_t *length, my_bool flags)
{
  auto entry= static_cast<const DDL_Info_Record *>(entry_);
  *length= strlen(entry->name);
  return reinterpret_cast<const uchar *>(entry->name);
}

/*
  helper function to free record from the hash
*/
static void free_rec(void *entry_) {
  auto entry= static_cast<DDL_Info_Record *>(entry_);
  my_free(entry->name);
  my_free(entry->stmt);
  my_free(entry);
}

/*
  Determines whether the tbl is a USER Defined table or not.
  returns true if tbl is of type json, or schema or system table,
  or any other type other than belonging to the user category.
*/
static bool is_json_schema_nonuser_table(TABLE_LIST *tbl)
{
  if (tbl->table_function)
    return true;
  else if (tbl->schema_table)
    return true;
  else if (get_table_category(tbl->get_db_name(), tbl->get_table_name()) !=
               TABLE_CATEGORY_USER ||
           strcmp(tbl->get_db_name().str, "sys") == 0)
    return true;
  return false;
}

/*
  The records from the list are read one by one,
  and added to the json array object "list_ddls". 
  The record has full table_name,
  and ddl of the table/view.
*/
static void dump_table_definitions(THD *thd, List<DDL_Info_Record> &list)
{
  if (list.is_empty() || list.elements == 0) {
    return;
  }
  List_iterator<DDL_Info_Record> li(list);
  Json_writer_object ddls_wrapper(thd);
  Json_writer_array ddl_list(thd, "list_ddls");
  while (DDL_Info_Record *rec=li++)
  {
    Json_writer_object ddl_wrapper(thd);
    ddl_wrapper.add("name", rec->name);
    size_t non_esc_stmt_len= strlen(rec->stmt);
    size_t escape_stmt_len= 4 * non_esc_stmt_len;
    char *escaped_stmt=
        (char *) my_malloc(key_memory_trace_ddl_info, escape_stmt_len + 1, 0);
    int act_escape_stmt_len=
        json_escape_string(rec->stmt, rec->stmt + non_esc_stmt_len,
                           escaped_stmt, escaped_stmt + escape_stmt_len);
    escaped_stmt[act_escape_stmt_len]= 0;
    ddl_wrapper.add("ddl", escaped_stmt);
    my_free(escaped_stmt);
    li.remove();
  }
}

static void create_view_def(THD *thd, TABLE_LIST *table, String *name, String *stmt, String *view_def)
{
  view_def->append(STRING_WITH_LEN("CREATE "));
  view_store_options(thd, table, view_def);
  view_def->append(STRING_WITH_LEN("VIEW "));
  view_def->append(*name);
  view_def->append(STRING_WITH_LEN(" AS "));
  view_def->append(*stmt);
}

/*
  Save table/view definitions that are referenced in queries
  into hash, and record_list.
  Global query_tables are read in reverse order from the thd->lex,
  and a record with table_name, and ddl of the table are created.
  Hash and a list is used to store the records, where in no duplicates
  are stored. dbName_plus_tableName is used as a key to discard any
  dupplicates.
*/
static void save_table_definitions(THD *thd, HASH *hash,
                                   List<DDL_Info_Record> &record_list)
{
  List<TABLE_LIST> tables_list;

  for (TABLE_LIST *tbl= thd->lex->query_tables; tbl; tbl= tbl->next_global)
    tables_list.push_front(tbl);

  if (tables_list.is_empty() || tables_list.elements == 0)
    return;

  List_iterator li(tables_list);
  my_hash_init(key_memory_trace_ddl_info, hash, system_charset_info, 16, 0, 0,
               get_rec_key, free_rec, HASH_UNIQUE);
  while (TABLE_LIST *tbl= li++)
  {
    if (tbl->is_view() ||
        (!is_json_schema_nonuser_table(tbl) ||
         (tbl->table && tbl->table->s &&
          tbl->table->s->tmp_table == TRANSACTIONAL_TMP_TABLE)))
    {
      size_t name_len=
          strlen(tbl->table_name.str) + strlen(tbl->get_db_name().str) + 1;
      char *full_name=
          (char *) my_malloc(key_memory_trace_ddl_info, name_len, 0);
      strcpy(full_name, tbl->get_db_name().str);
      strcat(full_name, ".");
      strcat(full_name, tbl->table_name.str);

      if (my_hash_search(hash, (uchar *) full_name, name_len))
        my_free(full_name);
      else
      {
        String ddl(2048);
        DDL_Info_Record *rec= (DDL_Info_Record *) my_malloc(
            key_memory_trace_ddl_info, sizeof(DDL_Info_Record), 0);
        char *buf;
        size_t buf_len;
        char *buf_copy;

        if (tbl->is_view())
        {
          String name(full_name, name_len, system_charset_info);
          String stmt(tbl->select_stmt.str, strlen(tbl->select_stmt.str),
                      system_charset_info);
          create_view_def(thd, tbl, &name, &stmt, &ddl);
        }
        else
          show_create_table(thd, tbl, &ddl, NULL, WITH_DB_NAME);

        buf= ddl.c_ptr();
        buf_len= strlen(buf) + 1;
        buf_copy= (char *) my_malloc(key_memory_trace_ddl_info, buf_len, 0);
        strcpy(buf_copy, buf);
        rec->name= full_name;
        rec->stmt= buf_copy;
        my_hash_insert(hash, (uchar *) rec);
        record_list.push_back(rec);
      }
    }
    li.remove();
  }
}

/*
  Stores the ddls of the tables, and views that are used 
  in either SELECT, INSERT, DELETE, and UPDATE queries,
  into the optimizer trace.
*/
void store_table_definitions_in_trace(THD *thd)
{
  LEX *lex = thd->lex;
  if (thd->variables.optimizer_trace &&
      thd->variables.store_ddls_in_optimizer_trace &&
      (lex->sql_command == SQLCOM_SELECT ||
       lex->sql_command == SQLCOM_INSERT ||
       lex->sql_command == SQLCOM_INSERT_SELECT ||
       lex->sql_command == SQLCOM_DELETE ||
       lex->sql_command == SQLCOM_UPDATE ||
       lex->sql_command == SQLCOM_DELETE_MULTI ||
       lex->sql_command == SQLCOM_UPDATE_MULTI))
  {
    HASH hash;
    List<DDL_Info_Record> record_list;
    save_table_definitions(thd, &hash, record_list);
    dump_table_definitions(thd, record_list);
    my_hash_free(&hash);
    record_list.empty();
  }
}