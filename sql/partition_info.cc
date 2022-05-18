/* Copyright (c) 2006, 2015, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB Corporation.

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

/* Some general useful functions */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation
#endif

#include "mariadb.h"
#include <my_global.h>
#include <tztime.h>
#include "sql_priv.h"
// Required to get server definitions for mysql/plugin.h right
#include "sql_plugin.h"
#include "sql_partition.h"                 // partition_info.h: LIST_PART_ENTRY
                                           // NOT_A_PARTITION_ID
#include "partition_info.h"
#include "sql_parse.h"
#include "sql_base.h"                         // fill_record
#include "lock.h"
#include "table.h"
#include "sql_class.h"
#include "vers_string.h"

#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#include "sql_table.h"
#include "transaction.h"


partition_info *partition_info::get_clone(THD *thd, bool empty_data_and_index_file)
{
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("partition_info::get_clone");

  List_iterator<partition_element> part_it(partitions);
  partition_element *part;
  partition_info *clone= new (mem_root) partition_info(*this);
  if (unlikely(!clone))
    DBUG_RETURN(NULL);

  memset(&(clone->read_partitions), 0, sizeof(clone->read_partitions));
  memset(&(clone->lock_partitions), 0, sizeof(clone->lock_partitions));
  clone->bitmaps_are_initialized= FALSE;
  clone->partitions.empty();

  while ((part= (part_it++)))
  {
    List_iterator<partition_element> subpart_it(part->subpartitions);
    partition_element *subpart;
    partition_element *part_clone= new (mem_root) partition_element(*part);
    if (!part_clone)
      DBUG_RETURN(NULL);
    part_clone->subpartitions.empty();
    while ((subpart= (subpart_it++)))
    {
      partition_element *subpart_clone= new (mem_root) partition_element(*subpart);
      if (!subpart_clone)
        DBUG_RETURN(NULL);
      if (empty_data_and_index_file)
        subpart_clone->data_file_name= subpart_clone->index_file_name= NULL;
      part_clone->subpartitions.push_back(subpart_clone, mem_root);
    }

    if (empty_data_and_index_file)
      part_clone->data_file_name= part_clone->index_file_name= NULL;
    clone->partitions.push_back(part_clone, mem_root);
    part_clone->list_val_list.empty();
    List_iterator<part_elem_value> list_val_it(part->list_val_list);
    part_elem_value *new_val_arr=
      (part_elem_value *)alloc_root(mem_root, sizeof(part_elem_value) *
                                    part->list_val_list.elements);
    if (!new_val_arr)
      DBUG_RETURN(NULL);

    p_column_list_val *new_colval_arr=
      (p_column_list_val*)alloc_root(mem_root, sizeof(p_column_list_val) *
                                     num_columns *
                                     part->list_val_list.elements);
    if (!new_colval_arr)
      DBUG_RETURN(NULL);

    part_elem_value *val;
    while ((val= list_val_it++))
    {
      part_elem_value *new_val= new_val_arr++;
      memcpy(new_val, val, sizeof(part_elem_value));
      if (!val->null_value)
      {
        p_column_list_val *new_colval= new_colval_arr;
        new_colval_arr+= num_columns;
        memcpy(new_colval, val->col_val_array,
               sizeof(p_column_list_val) * num_columns);
        new_val->col_val_array= new_colval;
      }
      part_clone->list_val_list.push_back(new_val, mem_root);
    }
  }
  if (part_type == VERSIONING_PARTITION && vers_info)
  {
    // clone Vers_part_info; set now_part, hist_part
    clone->vers_info= new (mem_root) Vers_part_info(*vers_info);
    List_iterator<partition_element> it(clone->partitions);
    while ((part= it++))
    {
      if (vers_info->now_part && part->id == vers_info->now_part->id)
        clone->vers_info->now_part= part;
      else if (vers_info->hist_part && part->id == vers_info->hist_part->id)
        clone->vers_info->hist_part= part;
    } // while ((part= it++))
  } // if (part_type == VERSIONING_PARTITION ...
  DBUG_RETURN(clone);
}

/**
  Mark named [sub]partition to be used/locked.

  @param part_name  Partition name to match.  Must be \0 terminated!
  @param length     Partition name length.

  @return Success if partition found
    @retval true  Partition found
    @retval false Partition not found
*/

bool partition_info::add_named_partition(const char *part_name, size_t length)
{
  HASH *part_name_hash;
  PART_NAME_DEF *part_def;
  Partition_share *part_share;
  DBUG_ENTER("partition_info::add_named_partition");
  DBUG_ASSERT(part_name[length] == 0);
  DBUG_ASSERT(table);
  DBUG_ASSERT(table->s);
  DBUG_ASSERT(table->s->ha_share);
  part_share= static_cast<Partition_share*>((table->s->ha_share));
  DBUG_ASSERT(part_share->partition_name_hash_initialized);
  part_name_hash= &part_share->partition_name_hash;
  DBUG_ASSERT(part_name_hash->records);

  part_def= (PART_NAME_DEF*) my_hash_search(part_name_hash,
                                            (const uchar*) part_name,
                                            length);
  if (!part_def)
  {
    my_error(ER_UNKNOWN_PARTITION, MYF(0), part_name, table->alias.c_ptr());
    DBUG_RETURN(true);
  }

  if (part_def->is_subpart)
  {
    bitmap_set_bit(&read_partitions, part_def->part_id);
  }
  else
  {
    if (is_sub_partitioned())
    {
      /* Mark all subpartitions in the partition */
      uint j, start= part_def->part_id;
      uint end= start + num_subparts;
      for (j= start; j < end; j++)
        bitmap_set_bit(&read_partitions, j);
    }
    else
      bitmap_set_bit(&read_partitions, part_def->part_id);
  }
  DBUG_PRINT("info", ("Found partition %u is_subpart %d for name %.*s",
                      part_def->part_id, part_def->is_subpart,
                      length, part_name));
  DBUG_RETURN(false);
}


/**
  Mark named [sub]partition to be used/locked.

  @param part_elem  Partition element that matched.
*/

bool partition_info::set_named_partition_bitmap(const char *part_name, size_t length)
{
  DBUG_ENTER("partition_info::set_named_partition_bitmap");
  bitmap_clear_all(&read_partitions);
  if (add_named_partition(part_name, length))
    DBUG_RETURN(true);
  bitmap_copy(&lock_partitions, &read_partitions);
  DBUG_RETURN(false);
}


/**
  Prune away partitions not mentioned in the PARTITION () clause,
  if used.

    @param partition_names  list of names of partitions.

  @return Operation status
    @retval true  Failure
    @retval false Success
*/
bool partition_info::prune_partition_bitmaps(List<String> *partition_names)
{
  List_iterator<String> partition_names_it(*(partition_names));
  uint num_names= partition_names->elements;
  uint i= 0;
  DBUG_ENTER("partition_info::prune_partition_bitmaps");

  if (num_names < 1)
    DBUG_RETURN(true);

  /*
    TODO: When adding support for FK in partitioned tables, the referenced
    table must probably lock all partitions for read, and also write depending
    of ON DELETE/UPDATE.
  */
  bitmap_clear_all(&read_partitions);

  /* No check for duplicate names or overlapping partitions/subpartitions. */

  DBUG_PRINT("info", ("Searching through partition_name_hash"));
  do
  {
    String *part_name_str= partition_names_it++;
    if (add_named_partition(part_name_str->c_ptr(), part_name_str->length()))
      DBUG_RETURN(true);
  } while (++i < num_names);
  DBUG_RETURN(false);
}


/**
  Set read/lock_partitions bitmap over non pruned partitions

  @param partition_names   list of partition names to query

  @return Operation status
    @retval FALSE  OK
    @retval TRUE   Failed to allocate memory for bitmap or list of partitions
                   did not match

  @note OK to call multiple times without the need for free_bitmaps.
*/

bool partition_info::set_partition_bitmaps(List<String> *partition_names)
{
  DBUG_ENTER("partition_info::set_partition_bitmaps");

  DBUG_ASSERT(bitmaps_are_initialized);
  DBUG_ASSERT(table);
  if (!bitmaps_are_initialized)
    DBUG_RETURN(TRUE);

  if (partition_names &&
      partition_names->elements)
  {
    if (table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION)
    {
        my_error(ER_PARTITION_CLAUSE_ON_NONPARTITIONED, MYF(0));
        DBUG_RETURN(true);
    }
    if (prune_partition_bitmaps(partition_names))
      DBUG_RETURN(TRUE);
  }
  else
  {
    bitmap_set_all(&read_partitions);
    DBUG_PRINT("info", ("Set all partitions"));
  }
  bitmap_copy(&lock_partitions, &read_partitions);
  DBUG_ASSERT(bitmap_get_first_set(&lock_partitions) != MY_BIT_NONE);
  DBUG_RETURN(FALSE);
}


/**
  Set read/lock_partitions bitmap over non pruned partitions

  @param table_list   Possible TABLE_LIST which can contain
                      list of partition names to query

  @return Operation status
    @retval FALSE  OK
    @retval TRUE   Failed to allocate memory for bitmap or list of partitions
                   did not match

  @note OK to call multiple times without the need for free_bitmaps.
*/
bool partition_info::set_partition_bitmaps_from_table(TABLE_LIST *table_list)
{
  List<String> *partition_names= table_list ?
                                   NULL : table_list->partition_names;
  return set_partition_bitmaps(partition_names);
}


/*
  Create a memory area where default partition names are stored and fill it
  up with the names.

  SYNOPSIS
    create_default_partition_names()
    part_no                         Partition number for subparts
    num_parts                       Number of partitions
    start_no                        Starting partition number
    subpart                         Is it subpartitions

  RETURN VALUE
    A pointer to the memory area of the default partition names

  DESCRIPTION
    A support routine for the partition code where default values are
    generated.
    The external routine needing this code is check_partition_info
*/

char *partition_info::create_default_partition_names(THD *thd, uint part_no,
                                                     uint num_parts_arg,
                                                     uint start_no)
{
  char *ptr= (char*) thd->calloc(num_parts_arg * MAX_PART_NAME_SIZE + 1);
  char *move_ptr= ptr;
  uint i= 0;
  DBUG_ENTER("create_default_partition_names");

  if (likely(ptr != 0))
  {
    do
    {
      if (make_partition_name(move_ptr, (start_no + i)))
        DBUG_RETURN(NULL);
      move_ptr+= MAX_PART_NAME_SIZE;
    } while (++i < num_parts_arg);
  }
  DBUG_RETURN(ptr);
}


/*
  Create a unique name for the subpartition as part_name'sp''subpart_no'

  SYNOPSIS
    create_default_subpartition_name()
    subpart_no                  Number of subpartition
    part_name                   Name of partition
  RETURN VALUES
    >0                          A reference to the created name string
    0                           Memory allocation error
*/

char *partition_info::create_default_subpartition_name(THD *thd, uint subpart_no,
                                               const char *part_name)
{
  size_t size_alloc= strlen(part_name) + MAX_PART_NAME_SIZE;
  char *ptr= (char*) thd->calloc(size_alloc);
  DBUG_ENTER("create_default_subpartition_name");

  if (likely(ptr != NULL))
    my_snprintf(ptr, size_alloc, "%ssp%u", part_name, subpart_no);

  DBUG_RETURN(ptr);
}


/*
  Set up all the default partitions not set-up by the user in the SQL
  statement. Also perform a number of checks that the user hasn't tried
  to use default values where no defaults exists.

  SYNOPSIS
    set_up_default_partitions()
    file                A reference to a handler of the table
    info                Create info
    start_no            Starting partition number

  RETURN VALUE
    TRUE                Error, attempted default values not possible
    FALSE               Ok, default partitions set-up

  DESCRIPTION
    The routine uses the underlying handler of the partitioning to define
    the default number of partitions. For some handlers this requires
    knowledge of the maximum number of rows to be stored in the table.
    This routine only accepts HASH and KEY partitioning and thus there is
    no subpartitioning if this routine is successful.
    The external routine needing this code is check_partition_info
*/

bool partition_info::set_up_default_partitions(THD *thd, handler *file,
                                               HA_CREATE_INFO *info,
                                               uint start_no)
{
  uint i;
  char *default_name;
  bool result= TRUE;
  DBUG_ENTER("partition_info::set_up_default_partitions");

  if (part_type == VERSIONING_PARTITION)
  {
    if (start_no == 0 && use_default_num_partitions)
      num_parts= 2;
    use_default_num_partitions= false;
  }
  else if (part_type != HASH_PARTITION)
  {
    const char *error_string;
    if (part_type == RANGE_PARTITION)
      error_string= "RANGE";
    else
      error_string= "LIST";
    my_error(ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0), error_string);
    goto end;
  }

  if ((num_parts == 0) &&
      ((num_parts= file->get_default_no_partitions(info)) == 0))
  {
    my_error(ER_PARTITION_NOT_DEFINED_ERROR, MYF(0), "partitions");
    goto end;
  }

  if (unlikely(num_parts > MAX_PARTITIONS))
  {
    my_error(ER_TOO_MANY_PARTITIONS_ERROR, MYF(0));
    goto end;
  }
  if (unlikely((!(default_name= create_default_partition_names(thd, 0,
                                                               num_parts,
                                                               start_no)))))
    goto end;
  i= 0;
  do
  {
    partition_element *part_elem= new partition_element();
    if (likely(part_elem != 0 &&
               (!partitions.push_back(part_elem))))
    {
      part_elem->engine_type= default_engine_type;
      part_elem->partition_name= default_name;
      part_elem->id= i;
      default_name+=MAX_PART_NAME_SIZE;
      if (part_type == VERSIONING_PARTITION)
      {
        if (start_no > 0 || i < num_parts - 1) {
          part_elem->type= partition_element::HISTORY;
        } else {
          part_elem->type= partition_element::CURRENT;
          part_elem->partition_name= "pn";
        }
      }
    }
    else
      goto end;
  } while (++i < num_parts);
  result= FALSE;
end:
  DBUG_RETURN(result);
}


/*
  Set up all the default subpartitions not set-up by the user in the SQL
  statement. Also perform a number of checks that the default partitioning
  becomes an allowed partitioning scheme.

  SYNOPSIS
    set_up_default_subpartitions()
    file                A reference to a handler of the table
    info                Create info

  RETURN VALUE
    TRUE                Error, attempted default values not possible
    FALSE               Ok, default partitions set-up

  DESCRIPTION
    The routine uses the underlying handler of the partitioning to define
    the default number of partitions. For some handlers this requires
    knowledge of the maximum number of rows to be stored in the table.
    This routine is only called for RANGE or LIST partitioning and those
    need to be specified so only subpartitions are specified.
    The external routine needing this code is check_partition_info
*/

bool partition_info::set_up_default_subpartitions(THD *thd, handler *file,
                                                  HA_CREATE_INFO *info)
{
  uint i, j;
  bool result= TRUE;
  partition_element *part_elem;
  List_iterator<partition_element> part_it(partitions);
  DBUG_ENTER("partition_info::set_up_default_subpartitions");

  if (num_subparts == 0)
    num_subparts= file->get_default_no_partitions(info);
  if (unlikely((num_parts * num_subparts) > MAX_PARTITIONS))
  {
    my_error(ER_TOO_MANY_PARTITIONS_ERROR, MYF(0));
    goto end;
  }
  i= 0;
  do
  {
    part_elem= part_it++;
    j= 0;
    do
    {
      partition_element *subpart_elem= new partition_element(part_elem);
      if (likely(subpart_elem != 0 &&
          (!part_elem->subpartitions.push_back(subpart_elem))))
      {
        char *ptr= create_default_subpartition_name(thd, j,
                                                    part_elem->partition_name);
        if (!ptr)
          goto end;
        subpart_elem->engine_type= default_engine_type;
        subpart_elem->partition_name= ptr;
      }
      else
        goto end;
    } while (++j < num_subparts);
  } while (++i < num_parts);
  result= FALSE;
end:
  DBUG_RETURN(result);
}


/*
  Support routine for check_partition_info

  SYNOPSIS
    set_up_defaults_for_partitioning()
    file                A reference to a handler of the table
    info                Create info
    start_no            Starting partition number

  RETURN VALUE
    TRUE                Error, attempted default values not possible
    FALSE               Ok, default partitions set-up

  DESCRIPTION
    Set up defaults for partition or subpartition (cannot set-up for both,
    this will return an error.
*/

bool partition_info::set_up_defaults_for_partitioning(THD *thd, handler *file,
                                                      HA_CREATE_INFO *info, 
                                                      uint start_no)
{
  DBUG_ENTER("partition_info::set_up_defaults_for_partitioning");

  if (!default_partitions_setup)
  {
    default_partitions_setup= TRUE;
    if (use_default_partitions &&
        set_up_default_partitions(thd, file, info, start_no))
      DBUG_RETURN(TRUE);
    if (is_sub_partitioned() && 
        use_default_subpartitions)
      DBUG_RETURN(set_up_default_subpartitions(thd, file, info));
  }
  DBUG_RETURN(FALSE);
}


/*
  Support routine for check_partition_info

  SYNOPSIS
    find_duplicate_field
    no parameters

  RETURN VALUE
    Erroneus field name  Error, there are two fields with same name
    NULL                 Ok, no field defined twice

  DESCRIPTION
    Check that the user haven't defined the same field twice in
    key or column list partitioning.
*/

const char* partition_info::find_duplicate_field()
{
  const char *field_name_outer, *field_name_inner;
  List_iterator<const char> it_outer(part_field_list);
  uint num_fields= part_field_list.elements;
  uint i,j;
  DBUG_ENTER("partition_info::find_duplicate_field");

  for (i= 0; i < num_fields; i++)
  {
    field_name_outer= it_outer++;
    List_iterator<const char> it_inner(part_field_list);
    for (j= 0; j < num_fields; j++)
    {
      field_name_inner= it_inner++;
      if (i >= j)
        continue;
      if (!(my_strcasecmp(system_charset_info,
                          field_name_outer,
                          field_name_inner)))
      {
        DBUG_RETURN(field_name_outer);
      }
    }
  }
  DBUG_RETURN(NULL);
}


/**
  @brief Get part_elem and part_id from partition name

  @param partition_name Name of partition to search for.
  @param file_name[out] Partition file name (part after table name,
                        #P#<part>[#SP#<subpart>]), skipped if NULL.
  @param part_id[out]   Id of found partition or NOT_A_PARTITION_ID.

  @retval Pointer to part_elem of [sub]partition, if not found NULL

  @note Since names of partitions AND subpartitions must be unique,
  this function searches both partitions and subpartitions and if name of
  a partition is given for a subpartitioned table, part_elem will be
  the partition, but part_id will be NOT_A_PARTITION_ID and file_name not set.
*/
partition_element *partition_info::get_part_elem(const char *partition_name,
                                                 char *file_name,
                                                 size_t file_name_size,
                                                 uint32 *part_id)
{
  List_iterator<partition_element> part_it(partitions);
  uint i= 0;
  DBUG_ENTER("partition_info::get_part_elem");
  DBUG_ASSERT(part_id);
  *part_id= NOT_A_PARTITION_ID;
  do
  {
    partition_element *part_elem= part_it++;
    if (is_sub_partitioned())
    {
      List_iterator<partition_element> sub_part_it(part_elem->subpartitions);
      uint j= 0;
      do
      {
        partition_element *sub_part_elem= sub_part_it++;
        if (!my_strcasecmp(system_charset_info,
                           sub_part_elem->partition_name, partition_name))
        {
          if (file_name)
            if (create_subpartition_name(file_name, file_name_size, "",
                                         part_elem->partition_name,
                                         partition_name, NORMAL_PART_NAME))
              DBUG_RETURN(NULL);
          *part_id= j + (i * num_subparts);
          DBUG_RETURN(sub_part_elem);
        }
      } while (++j < num_subparts);

      /* Naming a partition (first level) on a subpartitioned table. */
      if (!my_strcasecmp(system_charset_info,
                            part_elem->partition_name, partition_name))
        DBUG_RETURN(part_elem);
    }
    else if (!my_strcasecmp(system_charset_info,
                            part_elem->partition_name, partition_name))
    {
      if (file_name)
        if (create_partition_name(file_name, file_name_size, "",
                                  partition_name, NORMAL_PART_NAME, TRUE))
          DBUG_RETURN(NULL);
      *part_id= i;
      DBUG_RETURN(part_elem);
    }
  } while (++i < num_parts);
  DBUG_RETURN(NULL);
}


/**
  Helper function to find_duplicate_name.
*/

static const char *get_part_name_from_elem(const char *name, size_t *length,
                                      my_bool not_used __attribute__((unused)))
{
  *length= strlen(name);
  return name;
}

/*
  A support function to check partition names for duplication in a
  partitioned table

  SYNOPSIS
    find_duplicate_name()

  RETURN VALUES
    NULL               Has unique part and subpart names
    !NULL              Pointer to duplicated name

  DESCRIPTION
    Checks that the list of names in the partitions doesn't contain any
    duplicated names.
*/

char *partition_info::find_duplicate_name()
{
  HASH partition_names;
  uint max_names;
  const uchar *curr_name= NULL;
  List_iterator<partition_element> parts_it(partitions);
  partition_element *p_elem;

  DBUG_ENTER("partition_info::find_duplicate_name");

  /*
    TODO: If table->s->ha_part_data->partition_name_hash.elements is > 0,
    then we could just return NULL, but that has not been verified.
    And this only happens when in ALTER TABLE with full table copy.
  */

  max_names= num_parts;
  if (is_sub_partitioned())
    max_names+= num_parts * num_subparts;
  if (my_hash_init(PSI_INSTRUMENT_ME, &partition_names, system_charset_info, max_names, 0, 0,
                   (my_hash_get_key) get_part_name_from_elem, 0, HASH_UNIQUE))
  {
    DBUG_ASSERT(0);
    curr_name= (const uchar*) "Internal failure";
    goto error;
  }
  while ((p_elem= (parts_it++)))
  {
    curr_name= (const uchar*) p_elem->partition_name;
    if (my_hash_insert(&partition_names, curr_name))
      goto error;

    if (!p_elem->subpartitions.is_empty())
    {
      List_iterator<partition_element> subparts_it(p_elem->subpartitions);
      partition_element *subp_elem;
      while ((subp_elem= (subparts_it++)))
      {
        curr_name= (const uchar*) subp_elem->partition_name;
        if (my_hash_insert(&partition_names, curr_name))
          goto error;
      }
    }
  }
  my_hash_free(&partition_names);
  DBUG_RETURN(NULL);
error:
  my_hash_free(&partition_names);
  DBUG_RETURN((char*) curr_name);
}


/*
  A support function to check if a partition element's name is unique
  
  SYNOPSIS
    has_unique_name()
    partition_element  element to check

  RETURN VALUES
    TRUE               Has unique name
    FALSE              Doesn't
*/

bool partition_info::has_unique_name(partition_element *element)
{
  DBUG_ENTER("partition_info::has_unique_name");
  
  const char *name_to_check= element->partition_name;
  List_iterator<partition_element> parts_it(partitions);
  
  partition_element *el;
  while ((el= (parts_it++)))
  {
    if (!(my_strcasecmp(system_charset_info, el->partition_name, 
                        name_to_check)) && el != element)
        DBUG_RETURN(FALSE);

    if (!el->subpartitions.is_empty()) 
    {
      partition_element *sub_el;    
      List_iterator<partition_element> subparts_it(el->subpartitions);
      while ((sub_el= (subparts_it++)))
      {
        if (!(my_strcasecmp(system_charset_info, sub_el->partition_name, 
                            name_to_check)) && sub_el != element)
            DBUG_RETURN(FALSE);
      }
    }
  } 
  DBUG_RETURN(TRUE);
}


/**
  @brief Switch history partition according limit or interval

  @note
    vers_info->limit      Limit by number of partition records
    vers_info->interval   Limit by fixed time interval
    vers_info->hist_part  (out) Working history partition
*/
bool partition_info::vers_set_hist_part(THD *thd, uint *create_count)
{
  DBUG_ASSERT(!thd->lex->last_table() ||
              !thd->lex->last_table()->vers_conditions.delete_history);

  const bool auto_hist= create_count && vers_info->auto_hist;

  if (vers_info->limit)
  {
    DBUG_ASSERT(!vers_info->interval.is_set());
    ha_partition *hp= (ha_partition*)(table->file);
    partition_element *next;
    List_iterator<partition_element> it(partitions);
    ha_rows records= 0;
    vers_info->hist_part= partitions.head();
    while ((next= it++) != vers_info->now_part)
    {
      DBUG_ASSERT(bitmap_is_set(&read_partitions, next->id));
      ha_rows next_records= hp->part_records(next);
      if (next_records == 0)
        break;
      vers_info->hist_part= next;
      records= next_records;
    }
    if (records >= vers_info->limit)
    {
      if (next == vers_info->now_part)
      {
        if (auto_hist)
          *create_count= 1;
      }
      else
        vers_info->hist_part= next;
    }
    return 0;
  }
  else if (vers_info->interval.is_set() &&
           vers_info->hist_part->range_value <= thd->query_start())
  {
    partition_element *next= NULL;
    bool error= true;
    List_iterator<partition_element> it(partitions);
    while (next != vers_info->hist_part)
      next= it++;

    while ((next= it++) != vers_info->now_part)
    {
      vers_info->hist_part= next;
      if (next->range_value > thd->query_start())
      {
        error= false;
        break;
      }
    }
    if (error)
    {
      if (auto_hist)
      {
        *create_count= 0;
        const my_time_t hist_end= (my_time_t) vers_info->hist_part->range_value;
        DBUG_ASSERT(thd->query_start() >= hist_end);
        MYSQL_TIME h0, q0;
        my_tz_OFFSET0->gmt_sec_to_TIME(&h0, hist_end);
        my_tz_OFFSET0->gmt_sec_to_TIME(&q0, thd->query_start());
        longlong q= pack_time(&q0);
        longlong h= pack_time(&h0);
        while (h <= q)
        {
          if (date_add_interval(thd, &h0, vers_info->interval.type,
                                vers_info->interval.step))
            return true;
          h= pack_time(&h0);
          ++*create_count;
          if (*create_count == MAX_PARTITIONS - 2)
          {
            my_error(ER_TOO_MANY_PARTITIONS_ERROR, MYF(ME_WARNING));
            my_error(ER_VERS_HIST_PART_FAILED, MYF(0),
                     table->s->db.str, table->s->table_name.str);
            return true;
          }
        }
      }
      else
      {
        my_error(WARN_VERS_PART_FULL, MYF(ME_WARNING|ME_ERROR_LOG),
                table->s->db.str, table->s->table_name.str,
                vers_info->hist_part->partition_name, "INTERVAL");
      }
    }
  }

  return false;
}


/**
  @brief Run fast_alter_partition_table() to add new history partitions
         for tables requiring them.

  @param num_parts  Number of partitions to create
*/
bool vers_create_partitions(THD *thd, TABLE_LIST* tl, uint num_parts)
{
  bool result= true;
  HA_CREATE_INFO create_info;
  Alter_info alter_info;
  partition_info *save_part_info= thd->work_part_info;
  Query_tables_list save_query_tables;
  Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
  bool save_no_write_to_binlog= thd->lex->no_write_to_binlog;
  thd->m_reprepare_observer= NULL;
  thd->lex->reset_n_backup_query_tables_list(&save_query_tables);
  thd->lex->no_write_to_binlog= true;
  TABLE *table= tl->table;

  DBUG_ASSERT(!thd->is_error());
  DBUG_ASSERT(num_parts);

  {
    DBUG_ASSERT(table->s->get_table_ref_type() == TABLE_REF_BASE_TABLE);
    DBUG_ASSERT(table->versioned());
    DBUG_ASSERT(table->part_info);
    DBUG_ASSERT(table->part_info->vers_info);
    alter_info.reset();
    alter_info.partition_flags= ALTER_PARTITION_ADD|ALTER_PARTITION_AUTO_HIST;
    create_info.init();
    create_info.alter_info= &alter_info;
    Alter_table_ctx alter_ctx(thd, tl, 1, &table->s->db, &table->s->table_name);

    MDL_REQUEST_INIT(&tl->mdl_request, MDL_key::TABLE, tl->db.str,
                    tl->table_name.str, MDL_SHARED_NO_WRITE, MDL_TRANSACTION);
    if (thd->mdl_context.acquire_lock(&tl->mdl_request,
                                      thd->variables.lock_wait_timeout))
      goto exit;
    table->mdl_ticket= tl->mdl_request.ticket;

    create_info.db_type= table->s->db_type();
    create_info.options|= HA_VERSIONED_TABLE;
    DBUG_ASSERT(create_info.db_type);

    create_info.vers_info.set_start(table->s->vers_start_field()->field_name);
    create_info.vers_info.set_end(table->s->vers_end_field()->field_name);

    partition_info *part_info= new partition_info();
    if (unlikely(!part_info))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto exit;
    }
    part_info->use_default_num_partitions= false;
    part_info->use_default_num_subpartitions= false;
    part_info->num_parts= num_parts;
    part_info->num_subparts= table->part_info->num_subparts;
    part_info->subpart_type= table->part_info->subpart_type;
    if (unlikely(part_info->vers_init_info(thd)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto exit;
    }

    thd->work_part_info= part_info;
    if (part_info->set_up_defaults_for_partitioning(thd, table->file, NULL,
                                    table->part_info->next_part_no(num_parts)))
    {
      my_error(ER_VERS_HIST_PART_FAILED, MYF(ME_WARNING),
               tl->db.str, tl->table_name.str);
      goto exit;
    }
    bool partition_changed= false;
    bool fast_alter_partition= false;
    if (prep_alter_part_table(thd, table, &alter_info, &create_info,
                              &partition_changed, &fast_alter_partition))
    {
      my_error(ER_VERS_HIST_PART_FAILED, MYF(ME_WARNING),
               tl->db.str, tl->table_name.str);
      goto exit;
    }
    if (!fast_alter_partition)
    {
      my_error(ER_VERS_HIST_PART_FAILED, MYF(ME_WARNING),
               tl->db.str, tl->table_name.str);
      goto exit;
    }
    DBUG_ASSERT(partition_changed);
    if (mysql_prepare_alter_table(thd, table, &create_info, &alter_info,
                                  &alter_ctx))
    {
      my_error(ER_VERS_HIST_PART_FAILED, MYF(ME_WARNING),
               tl->db.str, tl->table_name.str);
      goto exit;
    }

    if (fast_alter_partition_table(thd, table, &alter_info, &alter_ctx,
                                   &create_info, tl))
    {
      my_error(ER_VERS_HIST_PART_FAILED, MYF(ME_WARNING),
               tl->db.str, tl->table_name.str);
      goto exit;
    }
  }

  result= false;
  // NOTE: we have to return DA_EMPTY for new command
  DBUG_ASSERT(thd->get_stmt_da()->is_ok());
  thd->get_stmt_da()->reset_diagnostics_area();
  thd->variables.option_bits|= OPTION_BINLOG_THIS;

exit:
  thd->work_part_info= save_part_info;
  thd->m_reprepare_observer= save_reprepare_observer;
  thd->lex->restore_backup_query_tables_list(&save_query_tables);
  thd->lex->no_write_to_binlog= save_no_write_to_binlog;
  return result;
}


/**
  Warn at the end of DML command if the last history partition is out of LIMIT.
*/
void partition_info::vers_check_limit(THD *thd)
{
  if (vers_info->auto_hist || !vers_info->limit ||
      vers_info->hist_part->id + 1 < vers_info->now_part->id)
    return;

  /*
    NOTE: at this point read_partitions bitmap is already pruned by DML code,
    we have to set read bits for working history partition. We could use
    bitmap_set_all(), but this is not optimal since there can be quite a number
    of partitions.
  */
  const uint32 sub_factor= num_subparts ? num_subparts : 1;
  uint32 part_id= vers_info->hist_part->id * sub_factor;
  const uint32 part_id_end= part_id + sub_factor;
  DBUG_ASSERT(part_id_end <= num_parts * sub_factor);

  ha_partition *hp= (ha_partition*)(table->file);
  ha_rows hist_rows= hp->part_records(vers_info->hist_part);
  if (hist_rows >= vers_info->limit)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        WARN_VERS_PART_FULL,
                        ER_THD(thd, WARN_VERS_PART_FULL),
                        table->s->db.str, table->s->table_name.str,
                        vers_info->hist_part->partition_name, "LIMIT");

    sql_print_warning(ER_THD(thd, WARN_VERS_PART_FULL),
                      table->s->db.str, table->s->table_name.str,
                      vers_info->hist_part->partition_name, "LIMIT");
  }
}


/*
  Check that the partition/subpartition is setup to use the correct
  storage engine
  SYNOPSIS
    check_engine_condition()
    p_elem                   Partition element
    table_engine_set         Have user specified engine on table level
    inout::engine_type       Current engine used
    inout::first             Is it first partition
  RETURN VALUE
    TRUE                     Failed check
    FALSE                    Ok
  DESCRIPTION
    Specified engine for table and partitions p0 and pn
    Must be correct both on CREATE and ALTER commands
    table p0 pn res (0 - OK, 1 - FAIL)
        -  -  - 0
        -  -  x 1
        -  x  - 1
        -  x  x 0
        x  -  - 0
        x  -  x 0
        x  x  - 0
        x  x  x 0
    i.e:
    - All subpartitions must use the same engine
      AND it must be the same as the partition.
    - All partitions must use the same engine
      AND it must be the same as the table.
    - if one does NOT specify an engine on the table level
      then one must either NOT specify any engine on any
      partition/subpartition OR for ALL partitions/subpartitions
    Note:
    When ALTER a table, the engines are already set for all levels
    (table, all partitions and subpartitions). So if one want to
    change the storage engine, one must specify it on the table level

*/

static bool check_engine_condition(partition_element *p_elem,
                                   bool table_engine_set,
                                   handlerton **engine_type,
                                   bool *first)
{
  DBUG_ENTER("check_engine_condition");

  DBUG_PRINT("enter", ("p_eng %s t_eng %s t_eng_set %u first %u state %u",
                       ha_resolve_storage_engine_name(p_elem->engine_type),
                       ha_resolve_storage_engine_name(*engine_type),
                       table_engine_set, *first, p_elem->part_state));
  if (*first && !table_engine_set)
  {
    *engine_type= p_elem->engine_type;
    DBUG_PRINT("info", ("setting table_engine = %s",
                         ha_resolve_storage_engine_name(*engine_type)));
  }
  *first= FALSE;
  if ((table_engine_set &&
      (p_elem->engine_type != (*engine_type) &&
       p_elem->engine_type)) ||
      (!table_engine_set &&
       p_elem->engine_type != (*engine_type)))
  {
    DBUG_RETURN(TRUE);
  }

  DBUG_RETURN(FALSE);
}


/*
  Check engine mix that it is correct
  Current limitation is that all partitions and subpartitions
  must use the same storage engine.
  SYNOPSIS
    check_engine_mix()
    inout::engine_type       Current engine used
    table_engine_set         Have user specified engine on table level
  RETURN VALUE
    TRUE                     Error, mixed engines
    FALSE                    Ok, no mixed engines
  DESCRIPTION
    Current check verifies only that all handlers are the same.
    Later this check will be more sophisticated.
    (specified partition handler ) specified table handler
    (MYISAM, MYISAM) -       OK
    (MYISAM, -)      -       NOT OK
    (MYISAM, -)    MYISAM    OK
    (- , MYISAM)   -         NOT OK
    (- , -)        MYISAM    OK
    (-,-)          -         OK
*/

bool partition_info::check_engine_mix(handlerton *engine_type,
                                      bool table_engine_set)
{
  handlerton *old_engine_type= engine_type;
  bool first= TRUE;
  uint n_parts= partitions.elements;
  DBUG_ENTER("partition_info::check_engine_mix");
  DBUG_PRINT("info", ("in: engine_type = %s, table_engine_set = %u",
                       ha_resolve_storage_engine_name(engine_type),
                       table_engine_set));
  if (n_parts)
  {
    List_iterator<partition_element> part_it(partitions);
    uint i= 0;
    do
    {
      partition_element *part_elem= part_it++;
      DBUG_PRINT("info", ("part = %d engine = %s table_engine_set %u",
                 i, ha_resolve_storage_engine_name(part_elem->engine_type),
                 table_engine_set));
      if (is_sub_partitioned() &&
          part_elem->subpartitions.elements)
      {
        uint n_subparts= part_elem->subpartitions.elements;
        uint j= 0;
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        do
        {
          partition_element *sub_elem= sub_it++;
          DBUG_PRINT("info", ("sub = %d engine = %s table_engie_set %u",
                     j, ha_resolve_storage_engine_name(sub_elem->engine_type),
                     table_engine_set));
          if (check_engine_condition(sub_elem, table_engine_set,
                                     &engine_type, &first))
            goto error;
        } while (++j < n_subparts);
        /* ensure that the partition also has correct engine */
        if (check_engine_condition(part_elem, table_engine_set,
                                   &engine_type, &first))
          goto error;
      }
      else if (check_engine_condition(part_elem, table_engine_set,
                                      &engine_type, &first))
        goto error;
    } while (++i < n_parts);
  }
  DBUG_PRINT("info", ("engine_type = %s",
                       ha_resolve_storage_engine_name(engine_type)));
  if (!engine_type)
    engine_type= old_engine_type;
  if (engine_type->flags & HTON_NO_PARTITION)
  {
    my_error(ER_PARTITION_MERGE_ERROR, MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_PRINT("info", ("out: engine_type = %s",
                       ha_resolve_storage_engine_name(engine_type)));
  DBUG_ASSERT(engine_type != partition_hton);
  DBUG_RETURN(FALSE);
error:
  /*
    Mixed engines not yet supported but when supported it will need
    the partition handler
  */
  DBUG_RETURN(TRUE);
}


/**
  Check if we allow DATA/INDEX DIRECTORY, if not warn and set them to NULL.

  @param thd  THD also containing sql_mode (looks from MODE_NO_DIR_IN_CREATE).
  @param part_elem partition_element to check.
*/
static void warn_if_dir_in_part_elem(THD *thd, partition_element *part_elem)
{
  if (thd->variables.sql_mode & MODE_NO_DIR_IN_CREATE)
  {
    if (part_elem->data_file_name)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          WARN_OPTION_IGNORED,
                          ER_THD(thd, WARN_OPTION_IGNORED),
                          "DATA DIRECTORY");
    if (part_elem->index_file_name)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          WARN_OPTION_IGNORED,
                          ER_THD(thd, WARN_OPTION_IGNORED),
                          "INDEX DIRECTORY");
    part_elem->data_file_name= part_elem->index_file_name= NULL;
  }
}


/*
  This code is used early in the CREATE TABLE and ALTER TABLE process.

  SYNOPSIS
    check_partition_info()
    thd                 Thread object
    eng_type            Return value for used engine in partitions
    file                A reference to a handler of the table
    info                Create info
    add_or_reorg_part   Is it ALTER TABLE ADD/REORGANIZE command

  RETURN VALUE
    TRUE                 Error, something went wrong
    FALSE                Ok, full partition data structures are now generated

  DESCRIPTION
    We will check that the partition info requested is possible to set-up in
    this version. This routine is an extension of the parser one could say.
    If defaults were used we will generate default data structures for all
    partitions.

*/

bool partition_info::check_partition_info(THD *thd, handlerton **eng_type,
                                          handler *file, HA_CREATE_INFO *info,
                                          partition_info *add_or_reorg_part)
{
  handlerton *table_engine= default_engine_type;
  uint i, tot_partitions;
  bool result= TRUE, table_engine_set;
  const char *same_name;
  uint32 hist_parts= 0;
  uint32 now_parts= 0;
  DBUG_ENTER("partition_info::check_partition_info");
  DBUG_ASSERT(default_engine_type != partition_hton);

  DBUG_PRINT("info", ("default table_engine = %s",
                      ha_resolve_storage_engine_name(table_engine)));
  if (!add_or_reorg_part)
  {
    int err= 0;

    /* Check for partition expression. */
    if (!list_of_part_fields)
    {
      DBUG_ASSERT(part_expr);
      err= part_expr->walk(&Item::check_partition_func_processor, 0, NULL);
    }

    /* Check for sub partition expression. */
    if (!err && is_sub_partitioned() && !list_of_subpart_fields)
    {
      DBUG_ASSERT(subpart_expr);
      err= subpart_expr->walk(&Item::check_partition_func_processor, 0,
                              NULL);
    }

    if (err)
    {
      my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
      goto end;
    }
    if (thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
        fix_parser_data(thd))
      goto end;
  }
  if (unlikely(!is_sub_partitioned() && 
               !(use_default_subpartitions && use_default_num_subpartitions)))
  {
    my_error(ER_SUBPARTITION_ERROR, MYF(0));
    goto end;
  }
  if (unlikely(is_sub_partitioned() &&
              (!(part_type == RANGE_PARTITION || 
                 part_type == LIST_PARTITION ||
                 part_type == VERSIONING_PARTITION))))
  {
    /* Only RANGE, LIST and SYSTEM_TIME partitioning can be subpartitioned */
    my_error(ER_SUBPARTITION_ERROR, MYF(0));
    goto end;
  }
  if (unlikely(set_up_defaults_for_partitioning(thd, file, info, (uint)0)))
    goto end;
  if (!(tot_partitions= get_tot_partitions()))
  {
    my_error(ER_PARTITION_NOT_DEFINED_ERROR, MYF(0), "partitions");
    goto end;
  }
  if (unlikely(tot_partitions > MAX_PARTITIONS))
  {
    my_error(ER_TOO_MANY_PARTITIONS_ERROR, MYF(0));
    goto end;
  }
  /*
    if NOT specified ENGINE = <engine>:
      If Create, always use create_info->db_type
      else, use previous tables db_type 
      either ALL or NONE partition should be set to
      default_engine_type when not table_engine_set
      Note: after a table is created its storage engines for
      the table and all partitions/subpartitions are set.
      So when ALTER it is already set on table level
  */
  if (info && info->used_fields & HA_CREATE_USED_ENGINE)
  {
    table_engine_set= TRUE;
    table_engine= info->db_type;
    /* if partition_hton, use thd->lex->create_info */
    if (table_engine == partition_hton)
      table_engine= thd->lex->create_info.db_type;
    DBUG_ASSERT(table_engine != partition_hton);
    DBUG_PRINT("info", ("Using table_engine = %s",
                        ha_resolve_storage_engine_name(table_engine)));
  }
  else
  {
    table_engine_set= FALSE;
    if (thd->lex->sql_command != SQLCOM_CREATE_TABLE)
    {
      table_engine_set= TRUE;
      DBUG_PRINT("info", ("No create, table_engine = %s",
                          ha_resolve_storage_engine_name(table_engine)));
      DBUG_ASSERT(table_engine && table_engine != partition_hton);
    }
  }

  if (part_field_list.elements > 0 &&
      (same_name= find_duplicate_field()))
  {
    my_error(ER_SAME_NAME_PARTITION_FIELD, MYF(0), same_name);
    goto end;
  }
  if ((same_name= find_duplicate_name()))
  {
    my_error(ER_SAME_NAME_PARTITION, MYF(0), same_name);
    goto end;
  }

  if (part_type == VERSIONING_PARTITION)
  {
    DBUG_ASSERT(vers_info);
    if (num_parts < 2 || !(use_default_partitions || vers_info->now_part))
    {
      DBUG_ASSERT(info);
      DBUG_ASSERT(info->alias.str);
      my_error(ER_VERS_WRONG_PARTS, MYF(0), info->alias.str);
      goto end;
    }
    DBUG_ASSERT(num_parts == partitions.elements);
  }
  i= 0;
  {
    List_iterator<partition_element> part_it(partitions);
    uint num_parts_not_set= 0;
    uint prev_num_subparts_not_set= num_subparts + 1;
    do
    {
      partition_element *part_elem= part_it++;
      warn_if_dir_in_part_elem(thd, part_elem);
      if (!is_sub_partitioned())
      {
        if (part_elem->engine_type == NULL)
        {
          num_parts_not_set++;
          part_elem->engine_type= default_engine_type;
        }
        if (check_table_name(part_elem->partition_name,
                             strlen(part_elem->partition_name), FALSE))
        {
          my_error(ER_WRONG_PARTITION_NAME, MYF(0));
          goto end;
        }
        DBUG_PRINT("info", ("part = %d engine = %s",
                   i, ha_resolve_storage_engine_name(part_elem->engine_type)));
      }
      else
      {
        uint j= 0;
        uint num_subparts_not_set= 0;
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        partition_element *sub_elem;
        do
        {
          sub_elem= sub_it++;
          warn_if_dir_in_part_elem(thd, sub_elem);
          if (check_table_name(sub_elem->partition_name,
                               strlen(sub_elem->partition_name), FALSE))
          {
            my_error(ER_WRONG_PARTITION_NAME, MYF(0));
            goto end;
          }
          if (sub_elem->engine_type == NULL)
          {
            if (part_elem->engine_type != NULL)
              sub_elem->engine_type= part_elem->engine_type;
            else
            {
              sub_elem->engine_type= default_engine_type;
              num_subparts_not_set++;
            }
          }
          DBUG_PRINT("info", ("part = %d sub = %d engine = %s", i, j,
                     ha_resolve_storage_engine_name(sub_elem->engine_type)));
        } while (++j < num_subparts);

        if (prev_num_subparts_not_set == (num_subparts + 1) &&
            (num_subparts_not_set == 0 ||
             num_subparts_not_set == num_subparts))
          prev_num_subparts_not_set= num_subparts_not_set;

        if (!table_engine_set &&
            prev_num_subparts_not_set != num_subparts_not_set)
        {
          DBUG_PRINT("info", ("num_subparts_not_set = %u num_subparts = %u",
                     num_subparts_not_set, num_subparts));
          my_error(ER_MIX_HANDLER_ERROR, MYF(0));
          goto end;
        }

        if (part_elem->engine_type == NULL)
        {
          if (num_subparts_not_set == 0)
            part_elem->engine_type= sub_elem->engine_type;
          else
          {
            num_parts_not_set++;
            part_elem->engine_type= default_engine_type;
          }
        }
      }
      if (part_type == VERSIONING_PARTITION)
      {
        if (part_elem->type == partition_element::HISTORY)
        {
          hist_parts++;
        }
        else
        {
          DBUG_ASSERT(part_elem->type == partition_element::CURRENT);
          now_parts++;
        }
      }
    } while (++i < num_parts);
    if (!table_engine_set &&
        num_parts_not_set != 0 &&
        num_parts_not_set != num_parts)
    {
      DBUG_PRINT("info", ("num_parts_not_set = %u num_parts = %u",
                 num_parts_not_set, num_subparts));
      my_error(ER_MIX_HANDLER_ERROR, MYF(0));
      goto end;
    }
  }
  if (unlikely(check_engine_mix(table_engine, table_engine_set)))
  {
    my_error(ER_MIX_HANDLER_ERROR, MYF(0));
    goto end;
  }

  if (hist_parts > 1)
  {
    if (vers_info->limit == 0 && !vers_info->interval.is_set())
    {
      push_warning_printf(thd,
        Sql_condition::WARN_LEVEL_WARN,
        WARN_VERS_PARAMETERS,
        ER_THD(thd, WARN_VERS_PARAMETERS),
        "no rotation condition for multiple HISTORY partitions.");
    }
  }
  if (unlikely(now_parts > 1))
  {
    my_error(ER_VERS_WRONG_PARTS, MYF(0), info->alias.str);
    goto end;
  }


  DBUG_ASSERT(table_engine != partition_hton &&
              default_engine_type == table_engine);
  if (eng_type)
    *eng_type= table_engine;


  /*
    We need to check all constant expressions that they are of the correct
    type and that they are increasing for ranges and not overlapping for
    list constants.
  */

  if (add_or_reorg_part)
  {
    if (part_type == VERSIONING_PARTITION && add_or_reorg_part->partitions.elements)
      vers_update_el_ids();
    if (check_constants(thd, this))
      goto end;
  }

  result= FALSE;
end:
  DBUG_RETURN(result);
}


/*
  Print error for no partition found

  SYNOPSIS
    print_no_partition_found()
    table                        Table object

  RETURN VALUES
*/

void partition_info::print_no_partition_found(TABLE *table_arg, myf errflag)
{
  char buf[100];
  char *buf_ptr= (char*)&buf;
  TABLE_LIST table_list;
  THD *thd= current_thd;

  table_list.reset();
  table_list.db= table_arg->s->db;
  table_list.table_name= table_arg->s->table_name;

  if (check_single_table_access(thd, SELECT_ACL, &table_list, TRUE))
  {
    my_message(ER_NO_PARTITION_FOR_GIVEN_VALUE,
               ER_THD(thd, ER_NO_PARTITION_FOR_GIVEN_VALUE_SILENT), errflag);
  }
  else
  {
    if (column_list)
      buf_ptr= (char*)"from column_list";
    else
    {
      MY_BITMAP *old_map= dbug_tmp_use_all_columns(table_arg, &table_arg->read_set);
      if (part_expr->null_value)
        buf_ptr= (char*)"NULL";
      else
        longlong10_to_str(err_value, buf,
                     part_expr->unsigned_flag ? 10 : -10);
      dbug_tmp_restore_column_map(&table_arg->read_set, old_map);
    }
    my_error(ER_NO_PARTITION_FOR_GIVEN_VALUE, errflag, buf_ptr);
  }
}


/*
  Set fields related to partition expression
  SYNOPSIS
    set_part_expr()
    start_token               Start of partition function string
    item_ptr                  Pointer to item tree
    end_token                 End of partition function string
    is_subpart                Subpartition indicator
  RETURN VALUES
    TRUE                      Memory allocation error
    FALSE                     Success
*/

bool partition_info::set_part_expr(THD *thd, Item *item_ptr, bool is_subpart)
{
  if (is_subpart)
  {
    list_of_subpart_fields= FALSE;
    subpart_expr= item_ptr;
  }
  else
  {
    list_of_part_fields= FALSE;
    part_expr= item_ptr;
  }
  return FALSE;
}


/*
  Check that partition fields and subpartition fields are not too long

  SYNOPSIS
    check_partition_field_length()

  RETURN VALUES
    TRUE                             Total length was too big
    FALSE                            Length is ok
*/

bool partition_info::check_partition_field_length()
{
  uint store_length= 0;
  uint i;
  DBUG_ENTER("partition_info::check_partition_field_length");

  for (i= 0; i < num_part_fields; i++)
    store_length+= get_partition_field_store_length(part_field_array[i]);
  if (store_length > MAX_DATA_LENGTH_FOR_KEY)
    DBUG_RETURN(TRUE);
  store_length= 0;
  for (i= 0; i < num_subpart_fields; i++)
    store_length+= get_partition_field_store_length(subpart_field_array[i]);
  if (store_length > MAX_DATA_LENGTH_FOR_KEY)
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}


/*
  Set up buffers and arrays for fields requiring preparation
  SYNOPSIS
    set_up_charset_field_preps()

  RETURN VALUES
    TRUE                             Memory Allocation error
    FALSE                            Success

  DESCRIPTION
    Set up arrays and buffers for fields that require special care for
    calculation of partition id. This is used for string fields with
    variable length or string fields with fixed length that isn't using
    the binary collation.
*/

bool partition_info::set_up_charset_field_preps(THD *thd)
{
  Field *field, **ptr;
  uchar **char_ptrs;
  unsigned i;
  size_t size;
  uint tot_fields= 0;
  uint tot_part_fields= 0;
  uint tot_subpart_fields= 0;
  DBUG_ENTER("set_up_charset_field_preps");

  if (!(part_type == HASH_PARTITION &&
        list_of_part_fields) &&
        check_part_func_fields(part_field_array, FALSE))
  {
    ptr= part_field_array;
    /* Set up arrays and buffers for those fields */
    while ((field= *(ptr++)))
    {
      if (field_is_partition_charset(field))
      {
        tot_part_fields++;
        tot_fields++;
      }
    }
    size= tot_part_fields * sizeof(char*);
    if (!(char_ptrs= (uchar**)thd->calloc(size)))
      goto error;
    part_field_buffers= char_ptrs;
    if (!(char_ptrs= (uchar**)thd->calloc(size)))
      goto error;
    restore_part_field_ptrs= char_ptrs;
    size= (tot_part_fields + 1) * sizeof(Field*);
    if (!(char_ptrs= (uchar**)thd->alloc(size)))
      goto error;
    part_charset_field_array= (Field**)char_ptrs;
    ptr= part_field_array;
    i= 0;
    while ((field= *(ptr++)))
    {
      if (field_is_partition_charset(field))
      {
        uchar *field_buf;
        size= field->pack_length();
        if (!(field_buf= (uchar*) thd->calloc(size)))
          goto error;
        part_charset_field_array[i]= field;
        part_field_buffers[i++]= field_buf;
      }
    }
    part_charset_field_array[i]= NULL;
  }
  if (is_sub_partitioned() && !list_of_subpart_fields &&
      check_part_func_fields(subpart_field_array, FALSE))
  {
    /* Set up arrays and buffers for those fields */
    ptr= subpart_field_array;
    while ((field= *(ptr++)))
    {
      if (field_is_partition_charset(field))
      {
        tot_subpart_fields++;
        tot_fields++;
      }
    }
    size= tot_subpart_fields * sizeof(char*);
    if (!(char_ptrs= (uchar**) thd->calloc(size)))
      goto error;
    subpart_field_buffers= char_ptrs;
    if (!(char_ptrs= (uchar**) thd->calloc(size)))
      goto error;
    restore_subpart_field_ptrs= char_ptrs;
    size= (tot_subpart_fields + 1) * sizeof(Field*);
    if (!(char_ptrs= (uchar**) thd->alloc(size)))
      goto error;
    subpart_charset_field_array= (Field**)char_ptrs;
    ptr= subpart_field_array;
    i= 0;
    while ((field= *(ptr++)))
    {
      uchar *UNINIT_VAR(field_buf);

      if (!field_is_partition_charset(field))
        continue;
      size= field->pack_length();
      if (!(field_buf= (uchar*) thd->calloc(size)))
        goto error;
      subpart_charset_field_array[i]= field;
      subpart_field_buffers[i++]= field_buf;
    }
    subpart_charset_field_array[i]= NULL;
  }
  DBUG_RETURN(FALSE);
error:
  DBUG_RETURN(TRUE);
}


/*
  Check if path does not contain mysql data home directory
  for partition elements with data directory and index directory

  SYNOPSIS
    check_partition_dirs()
    part_info               partition_info struct 

  RETURN VALUES
    0	ok
    1	error  
*/

bool check_partition_dirs(partition_info *part_info)
{
  if (!part_info)
    return 0;

  partition_element *part_elem;
  List_iterator<partition_element> part_it(part_info->partitions);
  while ((part_elem= part_it++))
  {
    if (part_elem->subpartitions.elements)
    {
      List_iterator<partition_element> sub_it(part_elem->subpartitions);
      partition_element *subpart_elem;
      while ((subpart_elem= sub_it++))
      {
        if (unlikely(error_if_data_home_dir(subpart_elem->data_file_name,
                                            "DATA DIRECTORY")) ||
            unlikely(error_if_data_home_dir(subpart_elem->index_file_name,
                                            "INDEX DIRECTORY")))
        return 1;
      }
    }
    else
    {
      if (unlikely(error_if_data_home_dir(part_elem->data_file_name,
                                          "DATA DIRECTORY")) ||
          unlikely(error_if_data_home_dir(part_elem->index_file_name,
                                          "INDEX DIRECTORY")))
        return 1;
    }
  }
  return 0;
}


/**
  Check what kind of error to report

  @param use_subpart_expr Use the subpart_expr instead of part_expr
  @param part_str         Name of partition to report error (or NULL)
*/
void partition_info::report_part_expr_error(bool use_subpart_expr)
{
  Item *expr= part_expr;
  DBUG_ENTER("partition_info::report_part_expr_error");
  if (use_subpart_expr)
    expr= subpart_expr;

  if (expr->type() == Item::FIELD_ITEM)
  {
    partition_type type= part_type;
    bool list_of_fields= list_of_part_fields;
    Item_field *item_field= (Item_field*) expr;
    /*
      The expression consists of a single field.
      It must be of integer type unless KEY or COLUMNS partitioning.
    */
    if (use_subpart_expr)
    {
      type= subpart_type;
      list_of_fields= list_of_subpart_fields;
    }
    if (!column_list &&
        item_field->field &&
        item_field->field->result_type() != INT_RESULT &&
        !(type == HASH_PARTITION && list_of_fields))
    {
      my_error(ER_FIELD_TYPE_NOT_ALLOWED_AS_PARTITION_FIELD, MYF(0),
               item_field->name.str);
      DBUG_VOID_RETURN;
    }
  }
  if (use_subpart_expr)
    my_error(ER_PARTITION_FUNC_NOT_ALLOWED_ERROR, MYF(0), "SUBPARTITION");
  else
    my_error(ER_PARTITION_FUNC_NOT_ALLOWED_ERROR, MYF(0), "PARTITION");
  DBUG_VOID_RETURN;
}
 

/*
  Create a new column value in current list with maxvalue
  Called from parser

  SYNOPSIS
    add_max_value()
  RETURN
    TRUE               Error
    FALSE              Success
*/

int partition_info::add_max_value(THD *thd)
{
  DBUG_ENTER("partition_info::add_max_value");

  part_column_list_val *col_val;
  /*
    Makes for LIST COLUMNS 'num_columns' DEFAULT tuples, 1 tuple for RANGEs
  */
  uint max_val= (num_columns && part_type == LIST_PARTITION) ?
                 num_columns : 1;
  for (uint i= 0; i < max_val; i++)
  {
    if (!(col_val= add_column_value(thd)))
    {
      DBUG_RETURN(TRUE);
    }
    col_val->max_value= TRUE;
  }
  DBUG_RETURN(FALSE);
}

/*
  Create a new column value in current list
  Called from parser

  SYNOPSIS
    add_column_value()
  RETURN
    >0                 A part_column_list_val object which have been
                       inserted into its list
    0                  Memory allocation failure
*/

part_column_list_val *partition_info::add_column_value(THD *thd)
{
  uint max_val= num_columns ? num_columns : MAX_REF_PARTS;
  DBUG_ENTER("add_column_value");
  DBUG_PRINT("enter", ("num_columns = %u, curr_list_object %u, max_val = %u",
                        num_columns, curr_list_object, max_val));
  if (curr_list_object < max_val)
  {
    curr_list_val->added_items++;
    DBUG_RETURN(&curr_list_val->col_val_array[curr_list_object++]);
  }
  if (!num_columns && part_type == LIST_PARTITION)
  {
    /*
      We're trying to add more than MAX_REF_PARTS, this can happen
      in ALTER TABLE using List partitions where the first partition
      uses VALUES IN (1,2,3...,17) where the number of fields in
      the list is more than MAX_REF_PARTS, in this case we know
      that the number of columns must be 1 and we thus reorganize
      into the structure used for 1 column. After this we call
      ourselves recursively which should always succeed.
    */
    num_columns= curr_list_object;
    if (!reorganize_into_single_field_col_val(thd))
    {
      if (!init_column_part(thd))
        DBUG_RETURN(add_column_value(thd));
    }
    DBUG_RETURN(NULL);
  }
  if (column_list)
  {
    my_error(ER_PARTITION_COLUMN_LIST_ERROR, MYF(0));
  }
  else
  {
    if (part_type == RANGE_PARTITION)
      my_error(ER_TOO_MANY_VALUES_ERROR, MYF(0), "RANGE");
    else
      my_error(ER_TOO_MANY_VALUES_ERROR, MYF(0), "LIST");
  }
  DBUG_RETURN(NULL);
}


/*
  Initialise part_elem_value object at setting of a new object
  (Helper functions to functions called by parser)

  SYNOPSIS
    init_col_val
    col_val                  Column value object to be initialised
    item                     Item object representing column value

  RETURN VALUES
    TRUE                     Failure
    FALSE                    Success
*/
void partition_info::init_col_val(part_column_list_val *col_val, Item *item)
{
  DBUG_ENTER("partition_info::init_col_val");

  col_val->item_expression= item;
  col_val->null_value= item->null_value;
  if (item->result_type() == INT_RESULT)
  {
    /*
      This could be both column_list partitioning and function
      partitioning, but it doesn't hurt to set the function
      partitioning flags about unsignedness.
    */
    curr_list_val->value= item->val_int();
    curr_list_val->unsigned_flag= TRUE;
    if (!item->unsigned_flag &&
        curr_list_val->value < 0)
      curr_list_val->unsigned_flag= FALSE;
    if (!curr_list_val->unsigned_flag)
      curr_part_elem->signed_flag= TRUE;
  }
  col_val->part_info= NULL;
  DBUG_VOID_RETURN;
}
/*
  Add a column value in VALUES LESS THAN or VALUES IN
  (Called from parser)

  SYNOPSIS
    add_column_list_value()
    lex                      Parser's lex object
    thd                      Thread object
    item                     Item object representing column value

  RETURN VALUES
    TRUE                     Failure
    FALSE                    Success
*/
bool partition_info::add_column_list_value(THD *thd, Item *item)
{
  part_column_list_val *col_val;
  Name_resolution_context *context= &thd->lex->current_select->context;
  TABLE_LIST *save_list= context->table_list;
  const char *save_where= thd->where;
  DBUG_ENTER("partition_info::add_column_list_value");

  if (part_type == LIST_PARTITION &&
      num_columns == 1U)
  {
    if (init_column_part(thd))
    {
      DBUG_RETURN(TRUE);
    }
  }

  context->table_list= 0;
  if (column_list)
    thd->where= "field list";
  else
    thd->where= "partition function";

  if (item->walk(&Item::check_partition_func_processor, 0, NULL))
  {
    my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (item->fix_fields(thd, (Item**)0) ||
      ((context->table_list= save_list), FALSE) ||
      (!item->const_item()))
  {
    context->table_list= save_list;
    thd->where= save_where;
    my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
    DBUG_RETURN(TRUE);
  }
  thd->where= save_where;

  if (!(col_val= add_column_value(thd)))
  {
    DBUG_RETURN(TRUE);
  }
  init_col_val(col_val, item);
  DBUG_RETURN(FALSE);
}

/*
  Initialise part_info object for receiving a set of column values
  for a partition, called when parser reaches VALUES LESS THAN or
  VALUES IN.

  SYNOPSIS
    init_column_part()
    lex                    Parser's lex object

  RETURN VALUES
    TRUE                     Failure
    FALSE                    Success
*/
bool partition_info::init_column_part(THD *thd)
{
  partition_element *p_elem= curr_part_elem;
  part_column_list_val *col_val_array;
  part_elem_value *list_val;
  uint loc_num_columns;
  DBUG_ENTER("partition_info::init_column_part");

  if (!(list_val=
      (part_elem_value*) thd->calloc(sizeof(part_elem_value))) ||
      p_elem->list_val_list.push_back(list_val, thd->mem_root))
    DBUG_RETURN(TRUE);

  if (num_columns)
    loc_num_columns= num_columns;
  else
    loc_num_columns= MAX_REF_PARTS;
  if (!(col_val_array=
        (part_column_list_val*) thd->calloc(loc_num_columns *
                                            sizeof(part_column_list_val))))
    DBUG_RETURN(TRUE);

  list_val->col_val_array= col_val_array;
  list_val->added_items= 0;
  curr_list_val= list_val;
  curr_list_object= 0;
  DBUG_RETURN(FALSE);
}

/*
  In the case of ALTER TABLE ADD/REORGANIZE PARTITION for LIST
  partitions we can specify list values as:
  VALUES IN (v1, v2,,,, v17) if we're using the first partitioning
  variant with a function or a column list partitioned table with
  one partition field. In this case the parser knows not the
  number of columns start with and allocates MAX_REF_PARTS in the
  array. If we try to allocate something beyond MAX_REF_PARTS we
  will call this function to reorganize into a structure with
  num_columns = 1. Also when the parser knows that we used LIST
  partitioning and we used a VALUES IN like above where number of
  values was smaller than MAX_REF_PARTS or equal, then we will
  reorganize after discovering this in the parser.

  SYNOPSIS
    reorganize_into_single_field_col_val()

  RETURN VALUES
    TRUE                     Failure
    FALSE                    Success
*/

int partition_info::reorganize_into_single_field_col_val(THD *thd)
{
  part_column_list_val *col_val, *new_col_val;
  part_elem_value *val= curr_list_val;
  uint loc_num_columns= num_columns;
  uint i;
  DBUG_ENTER("partition_info::reorganize_into_single_field_col_val");

  num_columns= 1;
  val->added_items= 1U;
  col_val= &val->col_val_array[0];
  init_col_val(col_val, col_val->item_expression);
  for (i= 1; i < loc_num_columns; i++)
  {
    col_val= &val->col_val_array[i];
    DBUG_ASSERT(part_type == LIST_PARTITION);
    if (init_column_part(thd))
    {
      DBUG_RETURN(TRUE);
    }
    if (!(new_col_val= add_column_value(thd)))
    {
      DBUG_RETURN(TRUE);
    }
    memcpy(new_col_val, col_val, sizeof(*col_val));
    init_col_val(new_col_val, col_val->item_expression);
  }
  curr_list_val= val;
  DBUG_RETURN(FALSE);
}

/*
  This function handles the case of function-based partitioning.
  It fixes some data structures created in the parser and puts
  them in the format required by the rest of the partitioning
  code.

  SYNOPSIS
  fix_partition_values()
  thd                             Thread object
  col_val                         Array of one value
  part_elem                       The partition instance
  part_id                         Id of partition instance

  RETURN VALUES
    TRUE                     Failure
    FALSE                    Success
*/
int partition_info::fix_partition_values(THD *thd,
                                         part_elem_value *val,
                                         partition_element *part_elem)
{
  part_column_list_val *col_val= val->col_val_array;
  DBUG_ENTER("partition_info::fix_partition_values");

  if (col_val->fixed)
  {
    DBUG_RETURN(FALSE);
  }

  Item *item_expr= col_val->item_expression;
  if ((val->null_value= item_expr->null_value))
  {
    if (part_elem->has_null_value)
    {
      my_error(ER_MULTIPLE_DEF_CONST_IN_LIST_PART_ERROR, MYF(0));
      DBUG_RETURN(TRUE);
    }
    part_elem->has_null_value= TRUE;
  }
  else if (item_expr->result_type() != INT_RESULT)
  {
    my_error(ER_VALUES_IS_NOT_INT_TYPE_ERROR, MYF(0),
             part_elem->partition_name);
    DBUG_RETURN(TRUE);
  }
  if (part_type == RANGE_PARTITION)
  {
    if (part_elem->has_null_value)
    {
      my_error(ER_NULL_IN_VALUES_LESS_THAN, MYF(0));
      DBUG_RETURN(TRUE);
    }
    part_elem->range_value= val->value;
  }
  col_val->fixed= 2;
  DBUG_RETURN(FALSE);
}

/*
  Get column item with a proper character set according to the field

  SYNOPSIS
    get_column_item()
    item                     Item object to start with
    field                    Field for which the item will be compared to

  RETURN VALUES
    NULL                     Error
    item                     Returned item
*/

Item* partition_info::get_column_item(Item *item, Field *field)
{
  if (field->result_type() == STRING_RESULT &&
      item->collation.collation != field->charset())
  {
    if (!(item= convert_charset_partition_constant(item,
                                                   field->charset())))
    {
      my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
      return NULL;
    }
  }
  return item;
}


/*
  Evaluate VALUES functions for column list values
  SYNOPSIS
    fix_column_value_functions()
    thd                              Thread object
    col_val                          List of column values
    part_id                          Partition id we are fixing

  RETURN VALUES
    TRUE                             Error
    FALSE                            Success
  DESCRIPTION
    Fix column VALUES and store in memory array adapted to the data type
*/

bool partition_info::fix_column_value_functions(THD *thd,
                                                part_elem_value *val,
                                                uint part_id)
{
  uint n_columns= part_field_list.elements;
  bool result= FALSE;
  uint i;
  part_column_list_val *col_val= val->col_val_array;
  DBUG_ENTER("partition_info::fix_column_value_functions");

  if (col_val->fixed > 1)
  {
    DBUG_RETURN(FALSE);
  }
  for (i= 0; i < n_columns; col_val++, i++)
  {
    Item *column_item= col_val->item_expression;
    Field *field= part_field_array[i];
    col_val->part_info= this;
    col_val->partition_id= part_id;
    if (col_val->max_value)
      col_val->column_value= NULL;
    else
    {
      col_val->column_value= NULL;
      if (!col_val->null_value)
      {
        uchar *val_ptr;
        uint len= field->pack_length();
        bool save_got_warning;

        if (!(column_item= get_column_item(column_item, field)))
        {
          result= TRUE;
          goto end;
        }
        Sql_mode_instant_set sms(thd, 0);
        save_got_warning= thd->got_warning;
        thd->got_warning= 0;
        if (column_item->save_in_field(field, TRUE) ||
            thd->got_warning)
        {
          my_error(ER_WRONG_TYPE_COLUMN_VALUE_ERROR, MYF(0));
          result= TRUE;
          goto end;
        }
        thd->got_warning= save_got_warning;
        if (!(val_ptr= (uchar*) thd->memdup(field->ptr, len)))
        {
          result= TRUE;
          goto end;
        }
        col_val->column_value= val_ptr;
      }
    }
    col_val->fixed= 2;
  }
end:
  DBUG_RETURN(result);
}


/**
  Fix partition data from parser.

  @details The parser generates generic data structures, we need to set them
  up as the rest of the code expects to find them. This is in reality part
  of the syntax check of the parser code.

  It is necessary to call this function in the case of a CREATE TABLE
  statement, in this case we do it early in the check_partition_info
  function.

  It is necessary to call this function for ALTER TABLE where we
  assign a completely new partition structure, in this case we do it
  in prep_alter_part_table after discovering that the partition
  structure is entirely redefined.

  It's necessary to call this method also for ALTER TABLE ADD/REORGANIZE
  of partitions, in this we call it in prep_alter_part_table after
  making some initial checks but before going deep to check the partition
  info, we also assign the column_list variable before calling this function
  here.

  Finally we also call it immediately after returning from parsing the
  partitioning text found in the frm file.

  This function mainly fixes the VALUES parts, these are handled differently
  whether or not we use column list partitioning. Since the parser doesn't
  know which we are using we need to set-up the old data structures after
  the parser is complete when we know if what type of partitioning the
  base table is using.

  For column lists we will handle this in the fix_column_value_function.
  For column lists it is sufficient to verify that the number of columns
  and number of elements are in synch with each other. So only partitioning
  using functions need to be set-up to their data structures.

  @param thd  Thread object

  @return Operation status
    @retval TRUE   Failure
    @retval FALSE  Success
*/

bool partition_info::fix_parser_data(THD *thd)
{
  List_iterator<partition_element> it(partitions);
  partition_element *part_elem;
  uint num_elements;
  uint i= 0, j, k;
  DBUG_ENTER("partition_info::fix_parser_data");

  if (!(part_type == RANGE_PARTITION ||
        part_type == LIST_PARTITION))
  {
    if (part_type == HASH_PARTITION && list_of_part_fields)
    {
      /* KEY partitioning, check ALGORITHM = N. Should not pass the parser! */
      if (key_algorithm > KEY_ALGORITHM_55)
      {
        my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
        DBUG_RETURN(true);
      }
      /* If not set, use DEFAULT = 2 for CREATE and ALTER! */
      if ((thd_sql_command(thd) == SQLCOM_CREATE_TABLE ||
           thd_sql_command(thd) == SQLCOM_ALTER_TABLE) &&
          key_algorithm == KEY_ALGORITHM_NONE)
        key_algorithm= KEY_ALGORITHM_55;
    }
    DBUG_RETURN(FALSE);
  }
  if (is_sub_partitioned() && list_of_subpart_fields)
  {
    /* KEY subpartitioning, check ALGORITHM = N. Should not pass the parser! */
    if (key_algorithm > KEY_ALGORITHM_55)
    {
      my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
      DBUG_RETURN(true);
    }
    /* If not set, use DEFAULT = 2 for CREATE and ALTER! */
    if ((thd_sql_command(thd) == SQLCOM_CREATE_TABLE ||
         thd_sql_command(thd) == SQLCOM_ALTER_TABLE) &&
        key_algorithm == KEY_ALGORITHM_NONE)
      key_algorithm= KEY_ALGORITHM_55;
  }
  defined_max_value= FALSE; // in case it already set (CREATE TABLE LIKE)
  do
  {
    part_elem= it++;
    List_iterator<part_elem_value> list_val_it(part_elem->list_val_list);
    num_elements= part_elem->list_val_list.elements;
    if (unlikely(!num_elements && error_if_requires_values()))
      DBUG_RETURN(true);
    DBUG_ASSERT(part_type == RANGE_PARTITION ?
                num_elements == 1U : TRUE);

    for (j= 0; j < num_elements; j++)
    {
      part_elem_value *val= list_val_it++;

      if (val->added_items != (column_list ? num_columns : 1))
      {
        my_error(ER_PARTITION_COLUMN_LIST_ERROR, MYF(0));
        DBUG_RETURN(TRUE);
      }

      /*
        Check the last MAX_VALUE for range partitions and DEFAULT value
        for LIST partitions.
        Both values are marked with defined_max_value and
        default_partition_id.

        This is a max_value/default is max_value is set and this is 
        a normal RANGE (no column list) or if it's a LIST partition:

        PARTITION p3 VALUES LESS THAN MAXVALUE
        or
        PARTITION p3 VALUES DEFAULT
      */
      if (val->added_items && val->col_val_array[0].max_value &&
          (!column_list || part_type == LIST_PARTITION))
      {
        DBUG_ASSERT(part_type == RANGE_PARTITION ||
                    part_type == LIST_PARTITION);
        if (defined_max_value)
        {
          my_error((part_type == RANGE_PARTITION) ?
                   ER_PARTITION_MAXVALUE_ERROR :
                   ER_PARTITION_DEFAULT_ERROR, MYF(0));
          DBUG_RETURN(TRUE);
        }

        /* For RANGE PARTITION MAX_VALUE must be last */
        if (i != (num_parts - 1) &&
            part_type != LIST_PARTITION)
        {
          my_error(ER_PARTITION_MAXVALUE_ERROR, MYF(0));
          DBUG_RETURN(TRUE);
        }

        defined_max_value= TRUE;
        default_partition_id= i;
        part_elem->max_value= TRUE;
        part_elem->range_value= LONGLONG_MAX;
        continue;
      }

      if (column_list)
      {
        for (k= 0; k < num_columns; k++)
        {
          part_column_list_val *col_val= &val->col_val_array[k];
          if (col_val->null_value && part_type == RANGE_PARTITION)
          {
            my_error(ER_NULL_IN_VALUES_LESS_THAN, MYF(0));
            DBUG_RETURN(TRUE);
          }
        }
      }
      else
      {
        if (fix_partition_values(thd, val, part_elem))
          DBUG_RETURN(TRUE);
        if (val->null_value)
        {
          /*
            Null values aren't required in the value part, they are kept per
            partition instance, only LIST partitions have NULL values.
          */
          list_val_it.remove();
        }
      }
    }
  } while (++i < num_parts);
  DBUG_RETURN(FALSE);
}


/**
  helper function to compare strings that can also be
  a NULL pointer.

  @param a  char pointer (can be NULL).
  @param b  char pointer (can be NULL).

  @return false if equal
    @retval true  strings differs
    @retval false strings is equal
*/

static bool strcmp_null(const char *a, const char *b)
{
  if (!a && !b)
    return false;
  if (a && b && !strcmp(a, b))
    return false;
  return true;
}


/**
  Check if the new part_info has the same partitioning.

  @param new_part_info  New partition definition to compare with.

  @return True if not considered to have changed the partitioning.
    @retval true  Allowed change (only .frm change, compatible distribution).
    @retval false Different partitioning, will need redistribution of rows.

  @note Currently only used to allow changing from non-set key_algorithm
  to a specified key_algorithm, to avoid rebuild when upgrading from 5.1 of
  such partitioned tables using numeric colums in the partitioning expression.
  For more info see bug#14521864.
  Does not check if columns etc has changed, i.e. only for
  alter_info->partition_flags == ALTER_PARTITION_INFO.
*/

bool partition_info::has_same_partitioning(partition_info *new_part_info)
{
  DBUG_ENTER("partition_info::has_same_partitioning");

  DBUG_ASSERT(part_field_array && part_field_array[0]);

  /*
    Only consider pre 5.5.3 .frm's to have same partitioning as
    a new one with KEY ALGORITHM = 1 ().
  */

  if (part_field_array[0]->table->s->mysql_version >= 50503)
    DBUG_RETURN(false);

  if (!new_part_info ||
      part_type != new_part_info->part_type ||
      num_parts != new_part_info->num_parts ||
      use_default_partitions != new_part_info->use_default_partitions ||
      new_part_info->is_sub_partitioned() != is_sub_partitioned())
    DBUG_RETURN(false);

  if (part_type != HASH_PARTITION)
  {
    /*
      RANGE or LIST partitioning, check if KEY subpartitioned.
      Also COLUMNS partitioning was added in 5.5, so treat that as different.
    */
    if (!is_sub_partitioned() ||
        !new_part_info->is_sub_partitioned() ||
        column_list ||
        new_part_info->column_list ||
        !list_of_subpart_fields ||
        !new_part_info->list_of_subpart_fields ||
        new_part_info->num_subparts != num_subparts ||
        new_part_info->subpart_field_list.elements !=
          subpart_field_list.elements ||
        new_part_info->use_default_subpartitions !=
          use_default_subpartitions)
      DBUG_RETURN(false);
  }
  else
  {
    /* Check if KEY partitioned. */
    if (!new_part_info->list_of_part_fields ||
        !list_of_part_fields ||
        new_part_info->part_field_list.elements != part_field_list.elements)
      DBUG_RETURN(false);
  }

  /* Check that it will use the same fields in KEY (fields) list. */
  List_iterator<const char> old_field_name_it(part_field_list);
  List_iterator<const char> new_field_name_it(new_part_info->part_field_list);
  const char *old_name, *new_name;
  while ((old_name= old_field_name_it++))
  {
    new_name= new_field_name_it++;
    if (!new_name || my_strcasecmp(system_charset_info,
                                   new_name,
                                   old_name))
      DBUG_RETURN(false);
  }

  if (is_sub_partitioned())
  {
    /* Check that it will use the same fields in KEY subpart fields list. */
    List_iterator<const char> old_field_name_it(subpart_field_list);
    List_iterator<const char> new_field_name_it(new_part_info->subpart_field_list);
    const char *old_name, *new_name;
    while ((old_name= old_field_name_it++))
    {
      new_name= new_field_name_it++;
      if (!new_name || my_strcasecmp(system_charset_info,
                                     new_name,
                                     old_name))
        DBUG_RETURN(false);
    }
  }

  if (!use_default_partitions)
  {
    /*
      Loop over partitions/subpartition to verify that they are
      the same, including state and name.
    */
    List_iterator<partition_element> part_it(partitions);
    List_iterator<partition_element> new_part_it(new_part_info->partitions);
    uint i= 0;
    do
    {
      partition_element *part_elem= part_it++;
      partition_element *new_part_elem= new_part_it++;
      /*
        The following must match:
        partition_name, data_file_name, index_file_name,
        engine_type, part_max_rows, part_min_rows, nodegroup_id.
        (max_value, signed_flag, has_null_value only on partition level,
        RANGE/LIST)
        The following can differ:
          - part_comment
        part_state must be PART_NORMAL!
      */
      if (!part_elem || !new_part_elem ||
          strcmp(part_elem->partition_name,
                 new_part_elem->partition_name) ||
          part_elem->part_state != PART_NORMAL ||
          new_part_elem->part_state != PART_NORMAL ||
          part_elem->max_value != new_part_elem->max_value ||
          part_elem->signed_flag != new_part_elem->signed_flag ||
          part_elem->has_null_value != new_part_elem->has_null_value)
        DBUG_RETURN(false);

      /* new_part_elem may not have engine_type set! */
      if (new_part_elem->engine_type &&
          part_elem->engine_type != new_part_elem->engine_type)
        DBUG_RETURN(false);

      if (is_sub_partitioned())
      {
        /*
          Check that both old and new partition has the same definition
          (VALUES IN/VALUES LESS THAN) (No COLUMNS partitioning, see above)
        */
        if (part_type == LIST_PARTITION)
        {
          List_iterator<part_elem_value> list_vals(part_elem->list_val_list);
          List_iterator<part_elem_value>
            new_list_vals(new_part_elem->list_val_list);
          part_elem_value *val;
          part_elem_value *new_val;
          while ((val= list_vals++))
          {
            new_val= new_list_vals++;
            if (!new_val)
              DBUG_RETURN(false);
            if ((!val->null_value && !new_val->null_value) &&
                val->value != new_val->value)
              DBUG_RETURN(false);
          }
          if (new_list_vals++)
            DBUG_RETURN(false);
        }
        else
        {
          DBUG_ASSERT(part_type == RANGE_PARTITION);
          if (new_part_elem->range_value != part_elem->range_value)
            DBUG_RETURN(false);
        }

        if (!use_default_subpartitions)
        {
          List_iterator<partition_element>
            sub_part_it(part_elem->subpartitions);
          List_iterator<partition_element>
            new_sub_part_it(new_part_elem->subpartitions);
          uint j= 0;
          do
          {
            partition_element *sub_part_elem= sub_part_it++;
            partition_element *new_sub_part_elem= new_sub_part_it++;
            /* new_part_elem may not have engine_type set! */
            if (new_sub_part_elem->engine_type &&
                sub_part_elem->engine_type != new_sub_part_elem->engine_type)
              DBUG_RETURN(false);

            if (strcmp(sub_part_elem->partition_name,
                       new_sub_part_elem->partition_name) ||
                sub_part_elem->part_state != PART_NORMAL ||
                new_sub_part_elem->part_state != PART_NORMAL ||
                sub_part_elem->part_min_rows !=
                  new_sub_part_elem->part_min_rows ||
                sub_part_elem->part_max_rows !=
                  new_sub_part_elem->part_max_rows ||
                sub_part_elem->nodegroup_id !=
                  new_sub_part_elem->nodegroup_id)
              DBUG_RETURN(false);
  
            if (strcmp_null(sub_part_elem->data_file_name,
                            new_sub_part_elem->data_file_name) ||
                strcmp_null(sub_part_elem->index_file_name,
                            new_sub_part_elem->index_file_name))
              DBUG_RETURN(false);

          } while (++j < num_subparts);
        }
      }
      else
      {
        if (part_elem->part_min_rows != new_part_elem->part_min_rows ||
            part_elem->part_max_rows != new_part_elem->part_max_rows ||
            part_elem->nodegroup_id != new_part_elem->nodegroup_id)
          DBUG_RETURN(false);

        if (strcmp_null(part_elem->data_file_name,
                        new_part_elem->data_file_name) ||
            strcmp_null(part_elem->index_file_name,
                        new_part_elem->index_file_name))
          DBUG_RETURN(false);
      }
    } while (++i < num_parts);
  }

  /*
    Only if key_algorithm was not specified before and it is now set,
    consider this as nothing was changed, and allow change without rebuild!
  */
  if (key_algorithm != partition_info::KEY_ALGORITHM_NONE ||
      new_part_info->key_algorithm == partition_info::KEY_ALGORITHM_NONE)
    DBUG_RETURN(false);

  DBUG_RETURN(true);
}


void partition_info::print_debug(const char *str, uint *value)
{
  DBUG_ENTER("print_debug");
  if (value)
    DBUG_PRINT("info", ("parser: %s, val = %u", str, *value));
  else
    DBUG_PRINT("info", ("parser: %s", str));
  DBUG_VOID_RETURN;
}

bool partition_info::field_in_partition_expr(Field *field) const
{
  uint i;
  for (i= 0; i < num_part_fields; i++)
  {
    if (field->eq(part_field_array[i]))
      return TRUE;
  }
  for (i= 0; i < num_subpart_fields; i++)
  {
    if (field->eq(subpart_field_array[i]))
      return TRUE;
  }
  return FALSE;
}

#else /* WITH_PARTITION_STORAGE_ENGINE */
 /*
   For builds without partitioning we need to define these functions
   since we they are called from the parser. The parser cannot
   remove code parts using ifdef, but the code parts cannot be called
   so we simply need to add empty functions to make the linker happy.
 */
part_column_list_val *partition_info::add_column_value(THD *thd)
{
  return NULL;
}

bool partition_info::set_part_expr(THD *thd, Item *item_ptr, bool is_subpart)
{
  (void)item_ptr;
  (void)is_subpart;
  return FALSE;
}

int partition_info::reorganize_into_single_field_col_val(THD *thd)
{
  return 0;
}

bool partition_info::init_column_part(THD *thd)
{
  return FALSE;
}

bool partition_info::add_column_list_value(THD *thd, Item *item)
{
  return FALSE;
}
int partition_info::add_max_value(THD *thd)
{
  return 0;
}

void partition_info::print_debug(const char *str, uint *value)
{
}

bool check_partition_dirs(partition_info *part_info)
{
  return 0;
}

#endif /* WITH_PARTITION_STORAGE_ENGINE */

bool partition_info::vers_init_info(THD * thd)
{
  part_type= VERSIONING_PARTITION;
  list_of_part_fields= true;
  column_list= false;
  vers_info= new (thd->mem_root) Vers_part_info;
  if (unlikely(!vers_info))
    return true;

  return false;
}


/**
  Assign INTERVAL and STARTS for SYSTEM_TIME partitions.

  @return true on error
*/

bool partition_info::vers_set_interval(THD* thd, Item* interval,
                                       interval_type int_type, Item* starts,
                                       bool auto_hist, const char *table_name)
{
  DBUG_ASSERT(part_type == VERSIONING_PARTITION);

  MYSQL_TIME ltime;
  uint err;
  vers_info->interval.type= int_type;
  vers_info->auto_hist= auto_hist;

  /* 1. assign INTERVAL to interval.step */
  if (interval->fix_fields_if_needed_for_scalar(thd, &interval))
    return true;
  bool error= get_interval_value(thd, interval, int_type, &vers_info->interval.step) ||
          vers_info->interval.step.neg || vers_info->interval.step.second_part ||
        !(vers_info->interval.step.year || vers_info->interval.step.month ||
          vers_info->interval.step.day || vers_info->interval.step.hour ||
          vers_info->interval.step.minute || vers_info->interval.step.second);
  if (error)
  {
    my_error(ER_PART_WRONG_VALUE, MYF(0), table_name, "INTERVAL");
    return true;
  }

  /* 2. assign STARTS to interval.start */
  if (starts)
  {
    if (starts->fix_fields_if_needed_for_scalar(thd, &starts))
      return true;
    switch (starts->result_type())
    {
      case INT_RESULT:
      case DECIMAL_RESULT:
      case REAL_RESULT:
        /* When table member is defined, we are inside mysql_unpack_partition(). */
        if (!table || starts->val_int() > TIMESTAMP_MAX_VALUE)
          goto interval_starts_error;
        vers_info->interval.start= (my_time_t) starts->val_int();
        break;
      case STRING_RESULT:
      case TIME_RESULT:
      {
        Datetime::Options opt(TIME_NO_ZERO_DATE | TIME_NO_ZERO_IN_DATE, thd);
        starts->get_date(thd, &ltime, opt);
        vers_info->interval.start= TIME_to_timestamp(thd, &ltime, &err);
        if (err)
          goto interval_starts_error;
        break;
      }
      case ROW_RESULT:
      default:
        goto interval_starts_error;
    }
    if (!table)
    {
      if (thd->query_start() < vers_info->interval.start) {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_PART_STARTS_BEYOND_INTERVAL,
                            ER_THD(thd, ER_PART_STARTS_BEYOND_INTERVAL),
                            table_name);
      }
    }
  }
  else // calculate default STARTS depending on INTERVAL
  {
    thd->variables.time_zone->gmt_sec_to_TIME(&ltime, thd->query_start());
    if (vers_info->interval.step.second)
      goto interval_set_starts;
    ltime.second= 0;
    if (vers_info->interval.step.minute)
      goto interval_set_starts;
    ltime.minute= 0;
    if (vers_info->interval.step.hour)
      goto interval_set_starts;
    ltime.hour= 0;

interval_set_starts:
    vers_info->interval.start= TIME_to_timestamp(thd, &ltime, &err);
    if (err)
      goto interval_starts_error;
  }

  return false;

interval_starts_error:
  my_error(ER_PART_WRONG_VALUE, MYF(0), table_name, "STARTS");
  return true;
}


bool partition_info::vers_set_limit(ulonglong limit, bool auto_hist,
                                    const char *table_name)
{
  DBUG_ASSERT(part_type == VERSIONING_PARTITION);

  if (limit < 1)
  {
    my_error(ER_PART_WRONG_VALUE, MYF(0), table_name, "LIMIT");
    return true;
  }

  vers_info->limit= limit;
  vers_info->auto_hist= auto_hist;
  return !limit;
}


bool partition_info::error_if_requires_values() const
{
  switch (part_type) {
  case NOT_A_PARTITION:
  case HASH_PARTITION:
  case VERSIONING_PARTITION:
    break;
  case RANGE_PARTITION:
    my_error(ER_PARTITION_REQUIRES_VALUES_ERROR, MYF(0), "RANGE", "LESS THAN");
    return true;
  case LIST_PARTITION:
    my_error(ER_PARTITION_REQUIRES_VALUES_ERROR, MYF(0), "LIST", "IN");
    return true;
  }
  return false;
}
