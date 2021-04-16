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

/* Instead of including sql_lex.h we add this typedef here */
typedef List<Item> List_item;
typedef struct st_copy_info COPY_INFO;

int mysql_prepare_insert(THD *thd, TABLE_LIST *table_list,
                         List<Item> &fields, List_item *values,
                         List<Item> &update_fields,
                         List<Item> &update_values, enum_duplicates duplic,
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

#endif /* SQL_INSERT_INCLUDED */
