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

bool store_tables_context_in_trace(THD *thd);

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

const uchar *get_tbl_trace_ctx_key(const void *entry_, size_t *length,
                                   my_bool flags);

void store_full_table_name(THD *thd, TABLE_LIST *tbl, String *buf);
char *create_new_copy(THD *thd, const char *buf);
#endif
