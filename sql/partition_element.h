#ifndef PARTITION_ELEMENT_INCLUDED
#define PARTITION_ELEMENT_INCLUDED

/* Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2021, MariaDB Corporation.

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

#include "my_base.h"                            /* ha_rows */
#include "handler.h"                            /* UNDEF_NODEGROUP */

/**
 * An enum and a struct to handle partitioning and subpartitioning.
 */
enum partition_type {
  NOT_A_PARTITION= 0,
  RANGE_PARTITION,
  HASH_PARTITION,
  LIST_PARTITION,
  VERSIONING_PARTITION
};

enum partition_state {
  PART_NORMAL= 0,
  PART_IS_DROPPED= 1,
  PART_TO_BE_DROPPED= 2,
  PART_TO_BE_ADDED= 3,
  PART_TO_BE_REORGED= 4,
  PART_REORGED_DROPPED= 5,
  PART_CHANGED= 6,
  PART_IS_CHANGED= 7,
  PART_IS_ADDED= 8,
  PART_ADMIN= 9
};

/*
  This struct is used to keep track of column expressions as part
  of the COLUMNS concept in conjunction with RANGE and LIST partitioning.
  The value can be either of MINVALUE, MAXVALUE and an expression that
  must be constant and evaluate to the same type as the column it
  represents.

  The data in this fixed in two steps. The parser will only fill in whether
  it is a max_value or provide an expression. Filling in
  column_value, part_info, partition_id, null_value is done by the
  function fix_column_value_function. However the item tree needs
  fixed also before writing it into the frm file (in add_column_list_values).
  To distinguish between those two variants, fixed= 1 after the
  fixing in add_column_list_values and fixed= 2 otherwise. This is
  since the fixing in add_column_list_values isn't a complete fixing.
*/

typedef struct p_column_list_val
{
  void* column_value;
  Item* item_expression;
  partition_info *part_info;
  uint partition_id;
  bool max_value; // MAXVALUE for RANGE type or DEFAULT value for LIST type
  bool null_value;
  char fixed;
} part_column_list_val;


/*
  This struct is used to contain the value of an element
  in the VALUES IN struct. It needs to keep knowledge of
  whether it is a signed/unsigned value and whether it is
  NULL or not.
*/

typedef struct p_elem_val
{
  longlong value;
  uint added_items;
  bool null_value;
  bool unsigned_flag;
  part_column_list_val *col_val_array;
} part_elem_value;

struct st_ddl_log_memory_entry;

enum stat_trx_field
{
  STAT_TRX_END= 0
};

class partition_element :public Sql_alloc
{
public:
  enum elem_type_enum
  {
    CONVENTIONAL= 0,
    CURRENT,
    HISTORY
  };

  List<partition_element> subpartitions;
  List<part_elem_value> list_val_list;
  ha_rows part_max_rows;
  ha_rows part_min_rows;
  longlong range_value;
  const char *partition_name;
  struct st_ddl_log_memory_entry *log_entry;
  const char* part_comment;
  const char* data_file_name;
  const char* index_file_name;
  handlerton *engine_type;
  LEX_CSTRING connect_string;
  enum partition_state part_state;
  uint16 nodegroup_id;
  bool has_null_value;
  bool signed_flag;                          // Range value signed
  bool max_value;                            // MAXVALUE range
  uint32 id;
  bool empty;
  elem_type_enum type;

  engine_option_value *option_list;      // create options for partition
  ha_table_option_struct *option_struct; // structure with parsed options

  partition_element()
  : part_max_rows(0), part_min_rows(0), range_value(0),
    partition_name(NULL),
    log_entry(NULL), part_comment(NULL),
    data_file_name(NULL), index_file_name(NULL),
    engine_type(NULL), connect_string(null_clex_str), part_state(PART_NORMAL),
    nodegroup_id(UNDEF_NODEGROUP), has_null_value(FALSE),
    signed_flag(FALSE), max_value(FALSE),
    id(UINT_MAX32),
    empty(true),
    type(CONVENTIONAL),
    option_list(NULL), option_struct(NULL)
  {}
  partition_element(partition_element *part_elem)
  : part_max_rows(part_elem->part_max_rows),
    part_min_rows(part_elem->part_min_rows),
    range_value(0), partition_name(NULL),
    log_entry(NULL),
    part_comment(part_elem->part_comment),
    data_file_name(part_elem->data_file_name),
    index_file_name(part_elem->index_file_name),
    engine_type(part_elem->engine_type),
    connect_string(null_clex_str),
    part_state(part_elem->part_state),
    nodegroup_id(part_elem->nodegroup_id),
    has_null_value(FALSE),
    signed_flag(part_elem->signed_flag),
    max_value(part_elem->max_value),
    id(part_elem->id),
    empty(part_elem->empty),
    type(CONVENTIONAL),
    option_list(part_elem->option_list),
    option_struct(part_elem->option_struct)
  {}
  ~partition_element() {}

  part_column_list_val& get_col_val(uint idx)
  {
    part_elem_value *ev= list_val_list.head();
    DBUG_ASSERT(ev);
    DBUG_ASSERT(ev->col_val_array);
    return ev->col_val_array[idx];
  }
};

#endif /* PARTITION_ELEMENT_INCLUDED */
