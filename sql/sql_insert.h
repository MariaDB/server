/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_INSERT_INCLUDED
#define SQL_INSERT_INCLUDED

#include "sql_class.h"                          /* enum_duplicates */
#include "sql_list.h"
#include "sql_base.h"

/* Instead of including sql_lex.h we add this typedef here */
typedef List<Item> List_item;
typedef struct st_copy_info COPY_INFO;

int mysql_prepare_insert(THD *thd, TABLE_LIST *table_list,
                         List<Item> &fields, List_item *values,
                         List<Item> &update_fields,
                         List<Item> &update_values, enum_duplicates duplic,
                         bool ignore,
                         COND **where, bool select_insert);
bool mysql_insert(THD *thd,TABLE_LIST *table,List<Item> &fields,
                  List<List_item> &values, List<Item> &update_fields,
                  List<Item> &update_values, enum_duplicates flag,
                  bool ignore, select_result* result);
void upgrade_lock_type_for_insert(THD *thd, thr_lock_type *lock_type,
                                  enum_duplicates duplic,
                                  bool is_multi_insert);
int check_that_all_fields_are_given_values(THD *thd, TABLE *entry,
                                           TABLE_LIST *table_list);
int vers_insert_history_row(TABLE *table);
int check_duplic_insert_without_overlaps(THD *thd, TABLE *table,
                                         enum_duplicates duplic);
int write_record(THD *thd, TABLE *table, COPY_INFO *info,
                 select_result *returning= NULL);
void kill_delayed_threads(void);
bool binlog_create_table(THD *thd, TABLE *table, bool replace);
bool binlog_drop_table(THD *thd, TABLE *table);

#ifdef EMBEDDED_LIBRARY
inline void kill_delayed_threads(void) {}
#endif


/**
  Base class for all INSERT and REPLACE statements. Abstract class that
  is inherited by Sql_cmd_insert_values and Sql_cmd_insert_select.
*/

class Sql_cmd_insert_base : public Sql_cmd_dml
{
protected:
  virtual bool precheck(THD *thd) override;

  virtual bool prepare_inner(THD *thd) override;

private:
  bool resolve_update_expressions(THD *thd);
  bool prepare_values_table(THD *thd);
  bool resolve_values_table_columns(THD *thd);

  /**
    Field list to insert/replace

    One of two things:
    1. For the INSERT/REPLACE ... (col1, ... colN) VALUES ... syntax
       this is a list of col1, ..., colN fields.
    2. For the INSERT/REPLACE ... SET col1=x1, ... colM=xM syntax extension
       this is a list of col1, ... colM fields as well.
  */
  List<Item> insert_field_list;

public:
  /*
    field_list was created for view and should be removed before PS/SP
    rexecuton
  */
  bool empty_field_list_on_rset;

  DML_prelocking_strategy *get_dml_prelocking_strategy()
  {
    return &dml_prelocking_strategy;
  }

protected:
  const bool is_replace;

  /**
    Row data to insert/replace

    One of two things:
    1. For the INSERT/REPLACE ... VALUES (row1), (row2), ... (rowN) syntax
       the list contains N List_item lists: one List_item per row.
    2. For the INSERT/REPLACE ... SET col1=x1, ... colM=xM syntax extension
       this list contains only 1 List_item of M data values: this way we
       emulate this syntax:
         INSERT/REPLACE ... (col1, ... colM) VALUE (x1, ..., xM);
  */
  List<List_item> insert_many_values;

  /* 
    Number of values per row in insert_many_values, available after resolving
  */ 
  uint value_count;

  /* ON DUPLICATE KEY UPDATE field list */
  List<Item> update_field_list;

  const enum_duplicates duplicates;

  Protocol *save_protocol; /**< needed for ANALYZE .. INSERT .. RETURNING */

  explicit Sql_cmd_insert_base(bool is_replace_arg,
                               enum_duplicates duplicates_arg)
    : empty_field_list_on_rset(false),
     is_replace(is_replace_arg),
     value_count(0),
     duplicates(duplicates_arg),
     save_protocol(NULL)
  {}

#if 0
  virtual void cleanup(THD *thd) override
  {
    if (empty_field_list_on_rset)
    {
      empty_field_list_on_rset = false;
      insert_field_list.empty();
    }
  }
#endif

private:

  /* The prelocking strategy used when opening the used tables */
  DML_prelocking_strategy dml_prelocking_strategy;

};


/**
  Class that implements INSERT ... VALUES and REPLACE ... VALUES statements.
*/

class Sql_cmd_insert_values final : public Sql_cmd_insert_base
{
public:
  explicit Sql_cmd_insert_values(bool is_replace_arg,
                                 enum_duplicates duplicates_arg)
   : Sql_cmd_insert_base(is_replace_arg, duplicates_arg) {}

  enum_sql_command sql_command_code() const
  {
    return is_replace ? SQLCOM_REPLACE : SQLCOM_INSERT;
  }

};


/**
  Class that implements INSERT ... SELECT and REPLACE ... SELECT statements.
*/

class Sql_cmd_insert_select final : public Sql_cmd_insert_base
{
public:
  explicit Sql_cmd_insert_select(bool is_replace_arg,
                                 enum_duplicates duplicates_arg)
    : Sql_cmd_insert_base(is_replace_arg, duplicates_arg) {}

  enum_sql_command sql_command_code() const
  {
    return is_replace ? SQLCOM_REPLACE_SELECT : SQLCOM_INSERT_SELECT;
  }
};



#endif /* SQL_INSERT_INCLUDED */
