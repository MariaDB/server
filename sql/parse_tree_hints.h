/* Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
  Parse tree node classes for optimizer hint syntax
*/


#ifndef PARSE_TREE_HINTS_INCLUDED
#define PARSE_TREE_HINTS_INCLUDED

#include "my_global.h"
#include "my_config.h"
#include "sql_alloc.h"
#include "sql_list.h"
#include "mem_root_array.h"

struct LEX;


struct Hint_param_table
{
  LEX_CSTRING table;
  LEX_CSTRING opt_query_block;
};


class PT_hint : public Sql_alloc
{
};


class PT_hint_list : public Sql_alloc
{
  Mem_root_array<PT_hint *, true> hints;

public:
  explicit PT_hint_list(MEM_ROOT *mem_root) : hints(mem_root) {}

  // OLEGS: virtual bool contextualize(Parse_context *pc);

  bool push_back(PT_hint *hint) { return hints.push_back(hint); }
};


class Hint_param_table_list : public Sql_alloc
{
  Mem_root_array<Hint_param_table, true> tables;

public:
  Hint_param_table_list (MEM_ROOT *mem_root)
    : tables(mem_root)
  {}

  bool push_back(Hint_param_table *table) { return tables.push_back(*table); }
};


class Hint_param_index_list : public Sql_alloc
{
  Mem_root_array<LEX_CSTRING, true> indexes;

public:
  Hint_param_index_list (MEM_ROOT *mem_root)
    : indexes(mem_root)
  {}

  bool push_back(LEX_CSTRING *index) { return indexes.push_back(*index); }
};


class PT_hint_max_execution_time : public PT_hint
{
  typedef PT_hint super;
public:
  ulong milliseconds;

  explicit PT_hint_max_execution_time(ulong milliseconds_arg)
  : milliseconds(milliseconds_arg)
  {}

  // OLEGS: virtual bool contextualize(Parse_context *pc);
};


class PT_hint_debug1 : public PT_hint
{
   const LEX_CSTRING opt_qb_name;
   Hint_param_table_list *table_list;

public:
  PT_hint_debug1(const LEX_CSTRING &opt_qb_name_arg,
                 Hint_param_table_list *table_list_arg)
  : opt_qb_name(opt_qb_name_arg),
    table_list(table_list_arg)
  {}
};


class PT_hint_debug2 : public PT_hint
{
  Hint_param_index_list *opt_index_list;

public:
  explicit
  PT_hint_debug2(Hint_param_index_list *opt_index_list_arg)
  : opt_index_list(opt_index_list_arg)
  {}
};



#endif /* PARSE_TREE_HINTS_INCLUDED */
