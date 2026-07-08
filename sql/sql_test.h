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

#ifndef SQL_TEST_INCLUDED
#define SQL_TEST_INCLUDED

#include "mysqld.h"
#include "opt_trace_context.h"

class JOIN;
struct TABLE_LIST;
typedef class Item COND;
typedef class st_select_lex SELECT_LEX;
typedef class st_select_lex_unit SELECT_LEX_UNIT;
struct SORT_FIELD;
class SEL_ARG;


#ifndef DBUG_OFF
/*
  Functions intended for manual use in debugger. NOT thread-safe.
*/

/* Print various data structures */
const char *dbug_print_item(Item *item);
const char *dbug_print_select(SELECT_LEX *sl);
const char *dbug_print_unit(SELECT_LEX_UNIT *un);
const char *dbug_print_sel_arg(SEL_ARG *sel_arg);

/* A single overloaded function (not inline so debugger sees them): */
const char *dbug_print(Item *x);
const char *dbug_print(SELECT_LEX *x);
const char *dbug_print(SELECT_LEX_UNIT *x);

/* Print current table row */
const char* dbug_print_table_row(TABLE *table);
const char *dbug_print_row(TABLE *table, const uchar *rec);

/* Check which MEM_ROOT the data is on */
bool dbug_is_mem_on_mem_root(const MEM_ROOT *mem_root, void *ptr);
const char *dbug_which_mem_root(THD *thd, void *ptr);

#else
// A dummy implementation is used in release builds
inline const char *dbug_print_item(Item *item) { return NULL; }
#endif

#ifndef DBUG_OFF
/* Functions that print into DBUG_FILE (/tmp/mariadb.trace by default) */
void print_where(COND *cond,const char *info, enum_query_type query_type);
void TEST_filesort(SORT_FIELD *sortorder,uint s_length);
void TEST_join(JOIN *join);
void print_plan(JOIN* join,uint idx, double record_count, double read_time,
                double current_read_time, const char *info);
void print_keyuse_array(DYNAMIC_ARRAY *keyuse_array);
void print_sjm(SJ_MATERIALIZATION_INFO *sjm);
void dump_TABLE_LIST_graph(SELECT_LEX *select_lex, TABLE_LIST* tl);
#endif
void print_keyuse_array_for_trace(THD *thd, DYNAMIC_ARRAY *keyuse_array);
void mysql_print_status();

#endif /* SQL_TEST_INCLUDED */
