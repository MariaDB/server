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

#ifndef OPT_TRACE_DDL_INFO
#define OPT_TRACE_DDL_INFO

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

class trace_table_index_range_context;

bool can_rw_trace_context(THD *thd);
bool store_tables_context_in_trace(THD *thd);

/*
   This class is used to buffer the range stats for indexes,
   while the range optimizer is in use,
   so that entire tables/views contexts can be stored
   at a single place in the trace.
*/
class Optimizer_Stats_Context_Recorder
{
private:
  /*
     Hash of table contexts used for storing
     all the ranges of indexes that are used
     in the current query, into the trace.
     full name of the table/view is used as the key.
  */
  HASH tbl_trace_ctx_hash;

public:
  Optimizer_Stats_Context_Recorder();

  ~Optimizer_Stats_Context_Recorder();

  bool has_records();

  trace_table_index_range_context *search(uchar *tbl_name,
                                          size_t tbl_name_len);

  Range_list_recorder *start_range_list_record(THD *thd, MEM_ROOT *mem_root,
                                               TABLE_LIST *tbl,
                                               size_t found_records,
                                               const char *index_name,
                                               double comp_cost);

  static const uchar *get_tbl_trace_ctx_key(const void *entry_, size_t *length,
                                            my_bool flags);
};

/* Optionally create and get the statistics context recorder for this query */
Optimizer_Stats_Context_Recorder *get_current_stats_recorder(THD *thd);

/* Get the range list recorder if we need one. */
inline Range_list_recorder *
get_range_list_recorder(THD *thd, MEM_ROOT *mem_root, TABLE_LIST *tbl,
                        const char *index_name, ha_rows records,
                        double comp_cost)
{
  Optimizer_Stats_Context_Recorder *ctx= get_current_stats_recorder(thd);
  if (ctx)
    return ctx->start_range_list_record(thd, mem_root, tbl, records,
                                        index_name, comp_cost);
  return nullptr;
}

class trace_table_context_read;
class trace_index_context_read;
class trace_range_context_read;
class Saved_Table_stats;

/*
  This class is used to parse the json structure
  and also infuse read stats into the optimizer
*/
class Optimizer_Trace_Stored_Context_Extractor
{
private:
  List<Saved_Table_stats> saved_tablestats_list;
  char *db_name;
  List<trace_table_context_read> ctx_list;
  bool has_records();
  bool parse(THD *thd);
  int parse_table_context(THD *thd, json_engine_t *je, const char **err,
                          trace_table_context_read *table_ctx);
  int parse_index_context(THD *thd, json_engine_t *je, const char **err,
                          trace_index_context_read *index_ctx);
  int parse_range_context(THD *thd, json_engine_t *je, const char **err,
                          trace_range_context_read *range_ctx);
  void dbug_print_read_stats();
  ha_rows get_table_rows(THD *thd, const TABLE *tbl);
  List<ha_rows> *get_index_rec_per_key_list(THD *thd, const TABLE *tbl,
                                            const char *idx_name);
  trace_range_context_read *get_range_context(THD *thd, const TABLE *tbl,
                                              const char *idx_name);
public:
  Optimizer_Trace_Stored_Context_Extractor(THD *thd);
  void load_range_stats_into_client(THD *thd, TABLE *tbl, uint keynr,
                                    RANGE_SEQ_IF *seq_if,
                                    SEL_ARG_RANGE_SEQ *seq,
                                    Cost_estimate *cost, ha_rows *rows);
  void save_old_and_set_new_table_stats(THD *thd, TABLE *table);
  void restore_saved_stats();
};

#endif
