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
                         bool ignore,
                         COND **where, bool select_insert, bool * const cache_results);
bool mysql_insert(THD *thd,TABLE_LIST *table,List<Item> &fields,
                  List<List_item> &values, List<Item> &update_fields,
                  List<Item> &update_values, enum_duplicates flag,
                  bool ignore, select_result* result);
int check_that_all_fields_are_given_values(THD *thd, TABLE *entry,
                                           TABLE_LIST *table_list);
int vers_insert_history_row(TABLE *table);
int check_duplic_insert_without_overlaps(THD *thd, TABLE *table,
                                         enum_duplicates duplic);
void kill_delayed_threads(void);
bool binlog_create_table(THD *thd, TABLE *table, bool replace);
bool binlog_drop_table(THD *thd, TABLE *table);
int prepare_for_replace(TABLE *table, enum_duplicates handle_duplicates,
                        bool ignore);
int finalize_replace(TABLE *table, enum_duplicates handle_duplicates,
                     bool ignore);
static inline void restore_default_record_for_insert(TABLE *t)
{
  restore_record(t,s->default_values);
  if (t->triggers)
    t->triggers->default_extra_null_bitmap();
}


class Write_record
{
  THD *thd;
  TABLE *table;
  COPY_INFO *info;

  ulonglong prev_insert_id;
  ulonglong insert_id_for_cur_row= 0;
  uchar *key;
  ushort key_nr;

  ushort last_unique_key;
  bool use_triggers;
  bool versioned;
  bool can_optimize;
  bool ignored_error;

  int (*incomplete_records_cb)(void *arg1, void *arg2);
  void *arg1, *arg2;
  select_result *sink;

  ushort get_last_unique_key() const;
  // FINALIZATION
  void notify_non_trans_table_modified();
  int after_insert(ha_rows *inserted);
  int after_ins_trg();
  int send_data();

  int on_ha_error(int error);
  int restore_on_error();

  bool is_fatal_error(int error);
  int prepare_handle_duplicate(int error);
  int locate_dup_record();

  int replace_row(ha_rows *inserted, ha_rows *deleted);
  int insert_on_duplicate_update(ha_rows *inserted, ha_rows *updated);
  int single_insert(ha_rows *inserted);
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
               bool versioned, bool use_triggers, select_result *sink,
               int (*incomplete_records_cb)(void *, void *),
               void *arg1, void* arg2):
          thd(thd), table(table), info(info), insert_id_for_cur_row(0),
          key(NULL), last_unique_key(get_last_unique_key()),
          use_triggers(use_triggers), versioned(versioned),
          incomplete_records_cb(incomplete_records_cb), arg1(arg1), arg2(arg2),
          sink(sink)
  {
    if (info->handle_duplicates == DUP_REPLACE)
    {
      const bool has_delete_triggers= use_triggers &&
                                table->triggers->has_delete_triggers();
      const bool referenced_by_fk= table->file->referenced_by_foreign_key();
      can_optimize= !referenced_by_fk && !has_delete_triggers &&
                    !table->versioned(VERS_TRX_ID);
    }
  }
  Write_record(THD *thd, TABLE *table, COPY_INFO *info,
               select_result *sink= NULL):
        Write_record(thd, table, info, table->versioned(VERS_TIMESTAMP),
                     table->triggers, sink, NULL, NULL, NULL)
  {}
  Write_record() = default; // dummy, to allow later (lazy) initializations

  /* Main entry point, see docs in sql_insert.cc */
  int write_record();

  int last_errno() { return info->last_errno; }
};
#ifdef EMBEDDED_LIBRARY
inline void kill_delayed_threads(void) {}
#endif

#endif /* SQL_INSERT_INCLUDED */
