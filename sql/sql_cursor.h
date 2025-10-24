/* Copyright (c) 2005, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef _sql_cursor_h_
#define _sql_cursor_h_

#include "sql_class.h"                          /* Query_arena */
#include "sql_select.h"

class JOIN;

/**
  @file

  Declarations for implementation of server side cursors. Only
  read-only non-scrollable cursors are currently implemented.
*/

/**
  Server_side_cursor -- an interface for materialized
  implementation of cursors. All cursors are self-contained
  (created in their own memory root).  For that reason they must
  be deleted only using a pointer to Server_side_cursor, not to
  its base class.
*/

class Server_side_cursor: protected Query_arena
{
protected:
  /** Row destination used for fetch */
  select_result *result;
public:
  Server_side_cursor(MEM_ROOT *mem_root_arg, select_result *result_arg)
    :Query_arena(mem_root_arg, STMT_INITIALIZED), result(result_arg)
  {}

  virtual bool is_open() const= 0;

  virtual int open(JOIN *top_level_join)= 0;
  virtual void fetch(ulong num_rows)= 0;
  virtual void close()= 0;
  virtual bool export_structure(THD *thd, Row_definition_list *defs)
  {
    DBUG_ASSERT(0);
    return true;
  }
  virtual bool check_assignability_to(const Virtual_tmp_table *table,
                                      const char *spvar_name,
                                      const char *op) const
  {
    DBUG_ASSERT(0);
    return false;
  }
  virtual ~Server_side_cursor();

  static void *operator new(size_t size, MEM_ROOT *mem_root)
  { return alloc_root(mem_root, size); }
  static void operator delete(void *ptr, size_t size);
  static void operator delete(void *, MEM_ROOT *){}
};


/**
  Materialized_cursor -- an insensitive materialized server-side
  cursor. The result set of this cursor is saved in a temporary
  table at open. The cursor itself is simply an interface for the
  handler of the temporary table.
*/

class Materialized_cursor: public Server_side_cursor
{
  MEM_ROOT main_mem_root;
  /* A fake unit to supply to select_send when fetching */
  SELECT_LEX_UNIT fake_unit;
  TABLE *table;
  List<Item> item_list;
  ulong fetch_limit;
  ulong fetch_count;
  bool is_rnd_inited;
public:
  Materialized_cursor(select_result *result, TABLE *table);

  int send_result_set_metadata(THD *thd, List<Item> &send_result_set_metadata);
  bool is_open() const override { return table != 0; }
  int open(JOIN *join __attribute__((unused))) override;
  void fetch(ulong num_rows) override;
  void close() override;
  bool export_structure(THD *thd, Row_definition_list *defs) override
  {
    return table->export_structure(thd, defs);
  }
  bool check_assignability_to(const Virtual_tmp_table *table,
                              const char *spvar_name,
                              const char *op) const override
  {
    return table->check_assignability_from(item_list, spvar_name, op);
  }
  ~Materialized_cursor() override;

  void on_table_fill_finished();
};


/**
  Select_materialize -- a mediator between a cursor query and the
  protocol. In case we were not able to open a non-materialzed
  cursor, it creates an internal temporary HEAP table, and insert
  all rows into it. When the table reaches max_heap_table_size,
  it's converted to a MyISAM table. Later this table is used to
  create a Materialized_cursor.
*/

class Select_materialize: public select_unit
{
  select_result *result; /**< the result object of the caller (PS or SP) */
  const Lex_ident_column &m_cursor_name;
  const Virtual_tmp_table *m_expected_assignable_structure;
public:
  Materialized_cursor *materialized_cursor;
  Select_materialize(THD *thd_arg, select_result *result_arg,
                     const Lex_ident_column &cursor_name,
                     const Virtual_tmp_table *expected_assignable_structure):
    select_unit(thd_arg), result(result_arg),
    m_cursor_name(cursor_name),
    m_expected_assignable_structure(expected_assignable_structure),
    materialized_cursor(0) {}
  int prepare(List<Item> &list, SELECT_LEX_UNIT *u) override;
  bool send_result_set_metadata(List<Item> &list, uint flags) override;
  bool send_eof() override { return false; }
  bool view_structure_only() const override
  {
    return result->view_structure_only();
  }
};


int mysql_open_cursor(THD *thd, select_result *result,
                      Server_side_cursor **res,
                      const Lex_ident_column &cursor_name,
                      const Virtual_tmp_table *expected_assignable_structure);

#endif /* _sql_cusor_h_ */
