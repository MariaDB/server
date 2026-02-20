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

#ifndef OPT_STORE_REPLAY_CONTEXT
#define OPT_STORE_REPLAY_CONTEXT

#include "my_global.h"
#include "sql_list.h"
#include "table.h"
#include "json_lib.h"

/***************************************************************************
 * Part 1: APIs for recording Optimizer Context.
 ***************************************************************************/

class SEL_ARG_RANGE_SEQ;
class Range_list_recorder;
class table_context_for_store;

/*
  Recorder is used to capture the environment during query optimization run.
  When the optimization is finished, one can save the captured context
  somewhere (currently, we write it into the OptimizerContext IS table)
*/
class Optimizer_context_recorder
{
public:
  Optimizer_context_recorder();

  ~Optimizer_context_recorder();

  Range_list_recorder *
  start_range_list_record(MEM_ROOT *mem_root, TABLE_LIST *tbl,
                          size_t found_records, const char *index_name,
                          const Cost_estimate *cost, ha_rows max_index_blocks,
                          ha_rows max_row_blocks);

  void record_cost_index_read(MEM_ROOT *mem_root, const TABLE_LIST *tbl,
                              uint key, ha_rows records, bool eq_ref,
                              const ALL_READ_COST *cost);
  void record_records_in_range(MEM_ROOT *mem_root, const TABLE *tbl,
                               const KEY_PART_INFO *key_part, uint keynr,
                               const key_range *min_range,
                               const key_range *max_range, ha_rows records);
  void record_const_table_row(MEM_ROOT *mem_root, TABLE *tbl);

  bool has_records();
  table_context_for_store *search(uchar *tbl_name, size_t tbl_name_len);

private:
  /*
    Hash table mapping "dbname.table_name" -> pointer to
    table_context_for_store. Contains records for all tables for which we have
    captured data.
  */
  HASH tbl_ctx_hash;

  table_context_for_store *get_table_context(MEM_ROOT *mem_root,
                                             const TABLE_LIST *tbl);
  static const uchar *get_tbl_ctx_key(const void *entry_, size_t *length,
                                      my_bool flags);
};

/* Interface to record range lists */
class Range_list_recorder : public Sql_alloc
{
public:
  void add_range(MEM_ROOT *mem_root, const char *range);
};

/* Optionally create and get the statistics context recorder for this query */
Optimizer_context_recorder *get_opt_context_recorder(THD *thd);

/* Get the range list recorder if we need one. */
Range_list_recorder *
get_range_list_recorder(THD *thd, MEM_ROOT *mem_root, TABLE_LIST *tbl,
                        const char *index_name, ha_rows records,
                        Cost_estimate *cost, ha_rows max_index_blocks,
                        ha_rows max_row_blocks);

/* Save the collected context into optimizer_context IS table */
bool store_optimizer_context(THD *thd);

/***************************************************************************
 * Part 2: APIs for loading previously saved Optimizer Context and replaying
 *  it: making the optimizer work as if the environment was like it has been
 *  at the time the context was recorded.
 ***************************************************************************/
class table_context_for_replay;
class index_context_for_replay;
class range_context_for_replay;
class irc_context_for_replay;
class rir_context_for_replay;

class Saved_Table_stats;

/*
  This class stores the parsed optimizer context information
  and then infuses read stats into the optimizer

  Optimizer Context information that we've read from a JSON document.

  The optimizer can use infuse_XXX() methods to get the saved values.
*/
class Optimizer_context_replay
{
public:
  Optimizer_context_replay(THD *thd);

  /* Save table's statistics and replace it with data from the context.  */
  void infuse_table_stats(TABLE *table);
  /* Restore the saved statistics back (to be done at query end) */
  void restore_modified_table_stats();

  /*
    "Infusion" functions.
    When the optimizer needs some data, for example to call index_read_cost(),
    it will call infuse_index_read_cost() and get the value from the context.
  */
  bool infuse_read_cost(const TABLE *tbl, IO_AND_CPU_COST *cost);
  bool infuse_range_stats(TABLE *tbl, uint keynr, RANGE_SEQ_IF *seq_if,
                          SEL_ARG_RANGE_SEQ *seq, Cost_estimate *cost,
                          ha_rows *rows, ha_rows *max_index_blocks,
                          ha_rows *max_row_blocks);
  bool infuse_index_read_cost(const TABLE *tbl, uint keynr, ha_rows records,
                              bool eq_ref, ALL_READ_COST *cost);
  bool infuse_records_in_range(const TABLE *tbl, const KEY_PART_INFO *key_part,
                               uint keynr, const key_range *min_range,
                               const key_range *max_range, ha_rows *records);

private:
  THD *thd;
  /*
    Statistics that tables had before we've replaced them with values from
    the saved context. To be used to restore the original values.
  */
  List<Saved_Table_stats> saved_table_stats;

  /* Current database recorded in the saved Optimizer Context */
  char *db_name;

  List<table_context_for_replay> ctx_list;
  bool parse();
  bool has_records();
#ifndef DBUG_OFF
  void dbug_print_read_stats();
#endif
  List<ha_rows> *get_index_rec_per_key_list(const TABLE *tbl,
                                            const char *idx_name);
  void store_range_contexts(const TABLE *tbl, const char *idx_name,
                            List<range_context_for_replay> *list);
  bool infuse_table_rows(const TABLE *tbl, ha_rows *rows);
  table_context_for_replay *find_table_context(const char *name);
};

class Optimizer_context_capture
{
public:
  String query;
  String ctx;
  Optimizer_context_capture(THD *thd, String &ctx_arg);
};

int fill_optimizer_context_capture_info(THD *thd, TABLE_LIST *tables, Item *);

void clean_captured_ctx(THD *thd);
#endif
