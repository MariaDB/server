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

struct replace_execution_result
{ ; };

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


class Write_record
{
  THD *thd;
  TABLE *table;
  COPY_INFO *info;
  select_result *sink;

  MY_BITMAP *save_read_set, *save_write_set;
  ulonglong prev_insert_id;
  ulonglong insert_id_for_cur_row= 0;
  uchar *key;
  ushort key_nr;

  ushort last_unique_key;
  bool use_triggers;
  bool versioned;
  bool has_delete_triggers;
  bool referenced_by_fk;

  ushort get_last_unique_key() const;
  // FINALIZATION
  void notify_non_trans_table_modified();
  void after_insert();
  void after_update();
  void after_trg_and_copied_inc();
  void send_data();
  int on_error(int error)
  {
    info->last_errno= error;
    table->file->print_error(error,MYF(0));
    return restore_on_error();
  }
  int restore_on_error()
  {
    table->file->restore_auto_increment(prev_insert_id);
    table->column_bitmaps_set(save_read_set, save_write_set);
    return 1;
  }
  int after_trg_error= 0;

  bool is_fatal_error(int error);
  int prepare_handle_duplicate(int error);
  int locate_dup_record();


  template<bool can_optimize>
  int replace_row();
  int update_duplicate();

  int single_insert();
public:

  /**
    @param thd  thread context
    @param info COPY_INFO structure describing handling of duplicates
            and which is used for counting number of records inserted
            and deleted.
    @param sink result sink for the RETURNING clause
    @param table
    @param versioned
    @param use_triggers
  */
  Write_record(THD *thd, TABLE *table, COPY_INFO *info,
               bool versioned, bool use_triggers, select_result *sink):
          thd(thd), table(table), info(info), sink(sink),
          insert_id_for_cur_row(0), key(NULL),
          last_unique_key(get_last_unique_key()),
          use_triggers(use_triggers), versioned(versioned),
          has_delete_triggers(use_triggers &&
                              table->triggers->has_delete_triggers()),
          referenced_by_fk(table->file->referenced_by_foreign_key())
  {
    switch(info->handle_duplicates)
    {
    case DUP_ERROR:
      write_record= &Write_record::single_insert;
      break;
    case DUP_UPDATE:
      write_record= &Write_record::update_duplicate;
      break;
    case DUP_REPLACE:
      write_record=
              !referenced_by_fk && has_delete_triggers ?
              &Write_record::replace_row<true>:
              &Write_record::replace_row<false>;
    }
  }
  Write_record(THD *thd, TABLE *table, COPY_INFO *info,
               select_result *sink= NULL)
       : Write_record(thd, table, info, table->versioned(VERS_TIMESTAMP),
                      table->triggers && table->triggers->has_delete_triggers(),
                      sink) {}
  Write_record() = default; // dummy, to allow later (lazy) initializations

  /**
    Write a record to table with optional deleting of conflicting records,
    invoke proper triggers if needed.

    @note
      Once this record will be written to table after insert trigger will
      be invoked. If instead of inserting new record we will update old one
      then both on update triggers will work instead. Similarly both on
      delete triggers will be invoked if we will delete conflicting records.

      Sets thd->transaction.stmt.modified_non_trans_table to TRUE if table which
      is updated didn't have transactions.

    @return
      0     - success
      non-0 - error
  */
  int (Write_record::*write_record)();
};
#ifdef EMBEDDED_LIBRARY
inline void kill_delayed_threads(void) {}
#endif

#endif /* SQL_INSERT_INCLUDED */
