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

#ifndef OPT_CONTEXT_STORE_REPLAY
#define OPT_CONTEXT_STORE_REPLAY

#include "my_global.h"
#include "sql_list.h"
#include "table.h"
//#include "json_lib.h"

class Item_subselect;
/***************************************************************************
 * Part 1: APIs for recording Optimizer Context.
 ***************************************************************************/

class SEL_ARG_RANGE_SEQ;
class Range_print_enumerator;

class table_context_for_store;
class Multi_range_read_const_call_record;

void init_optimizer_context_recorder_if_needed(THD *thd,
                                               const TABLE_LIST *query_tables);

/*
  Recorder is used to capture the environment during query optimization run.
  When the optimization is finished, one can save the captured context
  somewhere (currently, we write it into the OptimizerContext IS table)
*/
class Optimizer_context_recorder
{
public:
  Optimizer_context_recorder(MEM_ROOT *mem_root_arg);

  ~Optimizer_context_recorder();

  void record_multi_range_read_info_const(const TABLE *table, uint keynr,
                                          Range_print_enumerator *ranges,
                                          ha_rows rows,
                                          const Cost_estimate *cost,
                                          uint mrr_flags,
                                          const ha_rows *max_index_blocks,
                                          const ha_rows *max_row_blocks);

  void record_cost_for_index_read(const TABLE *table,
                                  uint key, ha_rows records, bool eq_ref,
                                  const ALL_READ_COST *cost);
  void record_records_in_range(const TABLE *tbl,
                               const KEY_PART_INFO *key_part, uint keynr,
                               const key_range *min_range,
                               const key_range *max_range, ha_rows records);
  void record_const_table_row(TABLE *tbl)
  {
    /* use table->record[1] */
    record_table_row(tbl, 1);
  }
  void record_current_table_row(TABLE *tbl)
  {
    /* use table->record[0] */
    record_table_row(tbl, 0);
  }

  void record_subquery_exec(Item_subselect *subq);

  bool has_records();
  table_context_for_store *search(uchar *tbl_name, size_t tbl_name_len);

  List<int> subquery_runs;
private:
  void record_table_row(TABLE *tbl, int row_index);
  MEM_ROOT *mem_root;
  /*
    Hash table mapping "dbname.table_name" -> pointer to
    table_context_for_store. Contains records for all tables for which we have
    captured data.
  */
  HASH tbl_ctx_hash;

  table_context_for_store *get_table_context(const TABLE *tbl);
  static const uchar *get_tbl_ctx_key(const void *entry_, size_t *length,
                                      my_bool flags);
  /*
    counter that tracks record_multi_range_read_info_const() calls
  */
  ulong mrr_counter= 0;
};

/* Save the collected context into optimizer_context IS table */
bool store_optimizer_context(THD *thd);

/***************************************************************************
 * Part 2: APIs for loading previously saved Optimizer Context and replaying
 *  it: making the optimizer work as if the environment was like it has been
 *  at the time the context was recorded.
 ***************************************************************************/
class table_context_for_replay;
class index_context_for_replay;

class Saved_table_stats;

void init_optimizer_context_replay_if_needed(THD *thd);

/*
  Optimizer context that's loaded and can be used for replay.

  - When this object is created, it will parse the context JSON document
    from a user variable pointed by @@optimizer_replay_context.

  - The optimizer checks thd->opt_ctx_replay, if it is present, it will call

       thd->opt_ctx_replay->infuse_XXX()

    to get "infuse" the statistics records from the context.
*/

class Optimizer_context_replay
{
public:
  Optimizer_context_replay(THD *thd);

  bool infuse_table_rows(TABLE *tbl);
  /* Save table's statistics and replace it with data from the context.  */
  void infuse_table_stats(TABLE *table);
  /* Restore the saved statistics back (to be done at query end) */
  void restore_modified_table_stats();

  /*
    "Infusion" functions.
    When the optimizer needs some data, for example to call index_read_cost(),
    it will call infuse_index_read_cost() and get the value from the context.
  */
  bool infuse_ha_scan_time(const TABLE *tbl, IO_AND_CPU_COST *cost);

  /*
    TODO: why doesn't this use Range_print_enumerator like record function does?
  */
  bool infuse_multi_range_read_info_const(TABLE *tbl, uint keynr,
                                          RANGE_SEQ_IF *seq_if,
                                          SEL_ARG_RANGE_SEQ *seq,
                                          Cost_estimate *cost,
                                          ha_rows *rows, uint *mrr_flags,
                                          ha_rows *max_index_blocks,
                                          ha_rows *max_row_blocks);
  bool infuse_cost_for_index_read(const TABLE *tbl, uint keynr, ha_rows records,
                                  bool eq_ref, ALL_READ_COST *cost);
  bool infuse_records_in_range(const TABLE *tbl, const KEY_PART_INFO *key_part,
                               uint keynr, const key_range *min_range,
                               const key_range *max_range, ha_rows *records);

private:
  THD *thd;
  /*
    counter that tracks infuse_range_stats() calls
  */
  ulong mrr_counter= 0;
  /*
    Statistics that tables had before we've replaced them with values from
    the saved context. To be used to restore the original values.
  */
  List<Saved_table_stats> saved_table_stats;

  List<table_context_for_replay> ctx_list;
  bool parse();
  bool has_records();
  List<double> *get_index_rec_per_key_list(const TABLE *tbl,
                                           const char *idx_name);
  void store_range_contexts(const TABLE *tbl, const char *idx_name,
                            List<Multi_range_read_const_call_record> *list);
  table_context_for_replay *find_table_context(const char *name);
#ifndef DBUG_OFF
  void dbug_print_read_stats();
#endif
};

/*
  Optimizer context that is captured and serialized into an SQL script.

  This is the source data for INFORMATION_SCHEMA.OPTIMIZER_CONTEXT.
*/
class Optimizer_context_capture
{
public:
  String query;
  /* Optimizer context in the form of SQL script */
  String ctx;
  Optimizer_context_capture(THD *thd, String &ctx_arg);
};

int fill_optimizer_context_capture_info(THD *thd, TABLE_LIST *tables, Item *);

void clean_captured_ctx(THD *thd);
#endif
