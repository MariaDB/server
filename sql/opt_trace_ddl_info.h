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

struct trace_range_context{
   char *range;
};

struct trace_index_range_context {
   char *idx_name;
   size_t num_records;
   List<trace_range_context> *list_range_context;
};

struct trace_table_index_range_context
{
   /*
      full name of the table or view
      i.e db_name.[table/view]_name
   */
   char *name;
   size_t name_len;
   List<trace_index_range_context> *list_index_range_context;
};

bool store_tables_context_in_trace(THD *thd);

const uchar *get_tbl_trace_ctx_key(const void *entry_, size_t *length,
                                   my_bool flags);

char *create_new_copy(THD *thd, const char *buf);

class Optimizer_Stats_Context_Recorder
{
private:
  /*
    Hash of table contexts used for storing
    all the ranges of indexes that are used
    in the current query, into the trace
  */
  HASH *tbl_trace_ctx_hash;

public:
  Optimizer_Stats_Context_Recorder(THD *thd);

  ~Optimizer_Stats_Context_Recorder();

  void clear();

  bool has_records();

  trace_table_index_range_context *search(uchar *tbl_name,
                                          size_t tbl_name_len);

  void record_ranges_for_tbl(THD *thd, TABLE_LIST *tbl, size_t found_records,
                             const char *index_name, List<char> *range_list);
};
#endif
