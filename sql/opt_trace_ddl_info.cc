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
  helper function to free record from the hash
*/
static void free_rec(void *entry_)
{
  auto entry= static_cast<DDL_Key *>(entry_);
  my_free(entry->name);
  my_free(entry);
}

/*
  Determines whether the tbl is a USER Defined table or not.
  returns true if tbl is of type json, or schema or system table,
  or any other type other than belonging to the user category.
*/
static bool is_json_schema_nonuser_table(TABLE_LIST *tbl)
{
  if (tbl->table_function || tbl->schema_table)
    return true;

  return get_table_category(tbl->get_db_name(), tbl->get_table_name()) !=
             TABLE_CATEGORY_USER ||
         tbl->table->s->tmp_table == INTERNAL_TMP_TABLE ||
         tbl->table->s->tmp_table == SYSTEM_TMP_TABLE;
}

static bool is_dumpable_tmp_table(TABLE_LIST *tbl)
{
  return tbl->table->s->tmp_table == TRANSACTIONAL_TMP_TABLE ||
         tbl->table->s->tmp_table == NON_TRANSACTIONAL_TMP_TABLE;
}

static void dump_record_to_trace(THD *thd, DDL_Key *ddl_key, String *stmt)
{
  Json_writer_object ddl_wrapper(thd);
  ddl_wrapper.add("name", ddl_key->name);
  size_t non_esc_stmt_len= stmt->length();
  // making escape_stmt size to be 4 times the non_esc_stmt
  // 4 is chosen as a worst case although 3 should suffice.
  //"'" can be escaped to \"\'\"
  size_t len_multiplier= sizeof(uint32_t);
  size_t escape_stmt_len= len_multiplier * non_esc_stmt_len;
  char *escaped_stmt=
      (char *) my_malloc(key_memory_trace_ddl_info, escape_stmt_len + 1, 0);
  int act_escape_stmt_len=
      json_escape_string(stmt->c_ptr(), stmt->c_ptr() + non_esc_stmt_len,
                         escaped_stmt, escaped_stmt + escape_stmt_len);
  escaped_stmt[act_escape_stmt_len]= 0;
  ddl_wrapper.add("ddl", escaped_stmt);
  my_free(escaped_stmt);
}

static void create_view_def(THD *thd, TABLE_LIST *table, String *name,
                            String *view_def)
{
  view_def->append(STRING_WITH_LEN("CREATE "));
  view_store_options(thd, table, view_def);
  view_def->append(STRING_WITH_LEN("VIEW "));
  view_def->append(*name);
  view_def->append(STRING_WITH_LEN(" AS "));
  view_def->append(table->select_stmt.str, table->select_stmt.length);
}

/*
  Stores the ddls of the tables, and views that are used
  in either SELECT, INSERT, DELETE, and UPDATE queries,
  into the optimizer trace.
  Global query_tables are read in reverse order from the thd->lex,
  and a record with table_name, and ddl of the table are created.
  Hash is used to store the records, where in no duplicates
  are stored. dbName_plus_tableName is used as a key to discard any
  duplicates. If a new record that is created is not in the hash,
  then that is dumped into the trace.
*/
void store_table_definitions_in_trace(THD *thd)
{
  LEX *lex= thd->lex;
  if (thd->variables.optimizer_trace &&
      thd->variables.store_ddls_in_optimizer_trace &&
      (lex->sql_command == SQLCOM_SELECT ||
       lex->sql_command == SQLCOM_INSERT_SELECT ||
       lex->sql_command == SQLCOM_DELETE ||
       lex->sql_command == SQLCOM_UPDATE ||
       lex->sql_command == SQLCOM_DELETE_MULTI ||
       lex->sql_command == SQLCOM_UPDATE_MULTI))
  {
    Json_writer_object ddls_wrapper(thd);
    ddls_wrapper.add("database_used", thd->get_db());
    Json_writer_array ddl_list(thd, "list_ddls");
    HASH hash;
    List<TABLE_LIST> tables_list;

    for (TABLE_LIST *tbl= thd->lex->query_tables; tbl; tbl= tbl->next_global)
      tables_list.push_front(tbl);

    if (tables_list.is_empty() || tables_list.elements == 0)
      return;

    List_iterator li(tables_list);
    my_hash_init(key_memory_trace_ddl_info, &hash, system_charset_info, 16, 0,
                 0, get_rec_key, free_rec, HASH_UNIQUE);
    for (TABLE_LIST *tbl= li++; tbl; li.remove(), tbl= li++)
    {
      String ddl;
      String name;
      DDL_Key *ddl_key;
      char *name_copy;

      if (!(tbl->is_view() || (tbl->table && tbl->table->s &&
                               (!is_json_schema_nonuser_table(tbl) ||
                                is_dumpable_tmp_table(tbl)))))
        continue;

      name.append(tbl->get_db_name().str, tbl->get_db_name().length);
      name.append(STRING_WITH_LEN("."));
      name.append(tbl->get_table_name().str, tbl->get_table_name().length);

      if (my_hash_search(&hash, (uchar *) name.c_ptr(), name.length()))
        continue;

      ddl_key=
          (DDL_Key *) my_malloc(key_memory_trace_ddl_info, sizeof(DDL_Key), 0);

      if (tbl->is_view())
        create_view_def(thd, tbl, &name, &ddl);
      else
        show_create_table(thd, tbl, &ddl, NULL, WITH_DB_NAME);

      name_copy=
          (char *) my_malloc(key_memory_trace_ddl_info, name.length() + 1, 0);
      strcpy(name_copy, name.c_ptr());
      ddl_key->name= name_copy;
      ddl_key->name_len= name.length();
      my_hash_insert(&hash, (uchar *) ddl_key);
      dump_record_to_trace(thd, ddl_key, &ddl);
    }
    my_hash_free(&hash);
  }
}
