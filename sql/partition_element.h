#ifndef PARTITION_ELEMENT_INCLUDED
#define PARTITION_ELEMENT_INCLUDED

/* Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

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

class Vers_field_stats : public Sql_alloc
{
  static const uint buf_size= 4 + (TIME_SECOND_PART_DIGITS + 1) / 2;
  uchar min_buf[buf_size];
  uchar max_buf[buf_size];
  Field_timestampf min_value;
  Field_timestampf max_value;
  mysql_rwlock_t lock;

public:
  Vers_field_stats(const char *field_name, TABLE_SHARE *share) :
    min_value(min_buf, NULL, 0, Field::NONE, field_name, share, 6),
    max_value(max_buf, NULL, 0, Field::NONE, field_name, share, 6)
  {
    min_value.set_max();
    memset(max_buf, 0, buf_size);
    mysql_rwlock_init(key_rwlock_LOCK_vers_stats, &lock);
  }
  ~Vers_field_stats()
  {
    mysql_rwlock_destroy(&lock);
  }
  bool update_unguarded(Field *from)
  {
    return
      from->update_min(&min_value, false) +
      from->update_max(&max_value, false);
  }
  bool update(Field *from)
  {
    mysql_rwlock_wrlock(&lock);
    bool res= update_unguarded(from);
    mysql_rwlock_unlock(&lock);
    return res;
  }
  my_time_t min_time()
  {
    mysql_rwlock_rdlock(&lock);
    my_time_t res= min_value.get_timestamp();
    mysql_rwlock_unlock(&lock);
    return res;
  }
  my_time_t max_time()
  {
    mysql_rwlock_rdlock(&lock);
    my_time_t res= max_value.get_timestamp();
    mysql_rwlock_unlock(&lock);
    return res;
  }
};

enum stat_trx_field
{
  STAT_TRX_END= 0
};

class partition_element :public Sql_alloc
{
public:
  List<partition_element> subpartitions;
  List<part_elem_value> list_val_list;
  ha_rows part_max_rows;
  ha_rows part_min_rows;
  longlong range_value;
  char *partition_name;
  char *tablespace_name;
  struct st_ddl_log_memory_entry *log_entry;
  char* part_comment;
  char* data_file_name;
  char* index_file_name;
  handlerton *engine_type;
  LEX_STRING connect_string;
  enum partition_state part_state;
  uint16 nodegroup_id;
  bool has_null_value;
  bool signed_flag;                          // Range value signed
  bool max_value;                            // MAXVALUE range
  uint32 id;
  bool empty;

  enum elem_type
  {
    CONVENTIONAL= 0,
    AS_OF_NOW,
    VERSIONING
  };

  elem_type type;

  partition_element()
  : part_max_rows(0), part_min_rows(0), range_value(0),
    partition_name(NULL), tablespace_name(NULL),
    log_entry(NULL), part_comment(NULL),
    data_file_name(NULL), index_file_name(NULL),
    engine_type(NULL), connect_string(null_lex_str), part_state(PART_NORMAL),
    nodegroup_id(UNDEF_NODEGROUP), has_null_value(FALSE),
    signed_flag(FALSE), max_value(FALSE),
    id(UINT32_MAX),
    empty(true),
    type(CONVENTIONAL)
  {}
  partition_element(partition_element *part_elem)
  : part_max_rows(part_elem->part_max_rows),
    part_min_rows(part_elem->part_min_rows),
    range_value(0), partition_name(NULL),
    tablespace_name(part_elem->tablespace_name),
    part_comment(part_elem->part_comment),
    data_file_name(part_elem->data_file_name),
    index_file_name(part_elem->index_file_name),
    engine_type(part_elem->engine_type),
    connect_string(null_lex_str),
    part_state(part_elem->part_state),
    nodegroup_id(part_elem->nodegroup_id),
    has_null_value(FALSE),
    id(part_elem->id),
    empty(part_elem->empty),
    type(part_elem->type)
  {}
  ~partition_element() {}

  part_column_list_val& get_col_val(uint idx)
  {
    DBUG_ASSERT(type != CONVENTIONAL);
    DBUG_ASSERT(list_val_list.elements == 1);
    part_elem_value *ev= static_cast<part_elem_value*>(list_val_list.first_node()->info);
    DBUG_ASSERT(ev && ev->col_val_array);
    return ev->col_val_array[idx];
  }
};

#endif /* PARTITION_ELEMENT_INCLUDED */
