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

/*
   One index range, aka interval.

   Currently we store the interval's text representation, like
   "1 < (key1) < 2"
*/
struct Range_record : public Sql_alloc
{
   char *range;
};

class Range_list_recorder : public Sql_alloc
{
 public:
  void add_range(MEM_ROOT *mem_root, const char *range);
};

/*
   A record of one multi_range_read_info_const() call:
   - index name
   - number of records
   - list of ranges.
*/
class Multi_range_read_const_call_record : public Range_list_recorder
{
public:
  char *idx_name;
  size_t num_records;
  List<Range_record> range_list;
};


class trace_table_index_range_context : public Sql_alloc
{
public:
  /*
     full name of the table or view
     i.e db_name.[table/view]_name
  */
  char *name;
  size_t name_len;
  List<Multi_range_read_const_call_record> index_list;
};

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

  Range_list_recorder*
  start_range_list_record(THD *thd, MEM_ROOT *mem_root,
                          TABLE_LIST *tbl, size_t found_records,
                          const char *index_name);

  static const uchar *get_tbl_trace_ctx_key(const void *entry_, size_t *length,
                                            my_bool flags);
};

/* Optionally create and get the statistics context recorder for this query */
Optimizer_Stats_Context_Recorder * get_current_stats_recorder(THD *thd);

/* Get the range list recorder if we need one. */
inline Range_list_recorder*
get_range_list_recorder(THD *thd, MEM_ROOT *mem_root,
                        TABLE_LIST *tbl, const char* index_name,
                        ha_rows records)
{
  Optimizer_Stats_Context_Recorder *ctx= get_current_stats_recorder(thd);
  if (ctx)
    return ctx->start_range_list_record(thd, mem_root, tbl, records, index_name);
  return nullptr;
}



#endif
