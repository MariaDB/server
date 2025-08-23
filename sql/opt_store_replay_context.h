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

class SEL_ARG_RANGE_SEQ;

class Range_list_recorder : public Sql_alloc
{
public:
  void add_range(MEM_ROOT *mem_root, const char *range);
};

class trace_table_context;

bool store_tables_context_in_trace(THD *thd);

/*
   This class is used to buffer the range stats for indexes,
   while the range optimizer is in use,
   so that entire tables/views contexts can be stored
   at a single place in the trace.
*/
class Optimizer_context_recorder
{
private:
  /*
     Hash of table contexts used for storing
     all the ranges of indexes that are used
     in the current query, into the trace.
     full name of the table/view is used as the key.
  */
  HASH tbl_trace_ctx_hash;
  trace_table_context *get_table_context(MEM_ROOT *mem_root,
                                         const TABLE_LIST *tbl);

public:
  Optimizer_context_recorder();

  ~Optimizer_context_recorder();

  bool has_records();

  trace_table_context *search(uchar *tbl_name, size_t tbl_name_len);

  Range_list_recorder *
  start_range_list_record(MEM_ROOT *mem_root, TABLE_LIST *tbl,
                          size_t found_records, const char *index_name,
                          const Cost_estimate *cost, ha_rows max_index_blocks,
                          ha_rows max_row_blocks);

  void record_cost_index_read(MEM_ROOT *mem_root, const TABLE_LIST *tbl,
                              uint key, ha_rows records, bool eq_ref,
                              const ALL_READ_COST *cost);
  static const uchar *get_tbl_trace_ctx_key(const void *entry_, size_t *length,
                                            my_bool flags);
};

/* Optionally create and get the statistics context recorder for this query */
Optimizer_context_recorder *get_opt_context_recorder(THD *thd);

/* Get the range list recorder if we need one. */
Range_list_recorder *
get_range_list_recorder(THD *thd, MEM_ROOT *mem_root, TABLE_LIST *tbl,
                        const char *index_name, ha_rows records,
                        Cost_estimate *cost, ha_rows max_index_blocks,
                        ha_rows max_row_blocks);

class trace_table_context_read;
class trace_index_context_read;
class trace_range_context_read;
class trace_irc_context_read;
class Saved_Table_stats;

/*
  This class stores the parsed optimizer context information
  and then infuses read stats into the optimizer
*/
class Optimizer_context_replay
{
private:
  THD *thd;
  List<Saved_Table_stats> saved_tablestats_list;
  char *db_name;
  List<trace_table_context_read> ctx_list;
  bool has_records();
  bool parse();
#ifndef DBUG_OFF
  void dbug_print_read_stats();
#endif
  List<ha_rows> *get_index_rec_per_key_list(const TABLE *tbl,
                                            const char *idx_name);
  void store_range_contexts(const TABLE *tbl, const char *idx_name,
                            List<trace_range_context_read> *list);
  bool infuse_table_rows(const TABLE *tbl, ha_rows *rows);

public:
  Optimizer_context_replay(THD *thd);
  bool infuse_read_cost(const TABLE *tbl, IO_AND_CPU_COST *cost);
  bool infuse_range_stats(TABLE *tbl, uint keynr, RANGE_SEQ_IF *seq_if,
                          SEL_ARG_RANGE_SEQ *seq, Cost_estimate *cost,
                          ha_rows *rows, ha_rows *max_index_blocks,
                          ha_rows *max_row_blocks);
  bool infuse_index_read_cost(const TABLE *tbl, uint keynr, ha_rows records,
                              bool eq_ref, ALL_READ_COST *cost);
  void infuse_table_stats(TABLE *table);
  void restore_modified_table_stats();
};

#endif
