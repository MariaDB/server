/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_global.h>
#include "sql_partition.h"
#include "partition_info.h"
#include "tztime.h"
#include "sql_base.h"
#include "rpl_rli.h"

#ifdef WITH_PARTITION_STORAGE_ENGINE
/*
  @brief
    Determine how many range interval partitions we will need to create to
    cover the values up to the current date and time.

  @param create_count OUT Number of partitions to create.

  @detail
    This function was inspired by partition_info::vers_set_hist_part()
*/
bool partition_info::range_interval_set_count(THD* thd, uint *create_count)
{
  partition_element *el= partitions.elem(partitions.elements - 1);
  Item *item;
  /* At least one range partition is defined */
  if (!el || !(item= el->get_col_val(0).item_expression))
  {
    DBUG_ASSERT(0);
    my_error(ER_INTERNAL_ERROR, MYF(0), "no range partition specified or "
                                        "invalid partition range expression");
    return true;
  }
  /*
    Compute the transition partition range time and the query time.
    They serve as the starting and the end points for new partitions
  */
  MYSQL_TIME cur_time, end_time;
  longlong cur, end;
  cur= item->val_datetime_packed(thd);
  unpack_time(cur, &cur_time, MYSQL_TIMESTAMP_DATETIME);
  thd->variables.time_zone->gmt_sec_to_TIME(&end_time, thd->query_start());
  end= pack_time(&end_time);
  *create_count= 0;
  while (cur <= end)
  {
    if (date_add_interval(thd, &cur_time, int_type, interval))
    {
      my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "partition range", "INTERVAL");
      return true;
    }
    cur= pack_time(&cur_time);
    ++*create_count;
    if (partitions.elements + *create_count > MAX_PARTITIONS)
    {
      my_error(ER_TOO_MANY_PARTITIONS_ERROR, MYF(0));
      return true;
    }
  }
  return false;
}


/*
  @brief
    Similar to vers_switch_partition, find how many partitions to create
    and call oc_ctx->request_backoff_action to record actions to be taken
    after returning failure from open_and_process_table().

  @retval true  Error or partition creation was requested.
  @retval false No error
*/

bool TABLE::range_interval_check_partition(THD *thd, TABLE_LIST *table_list,
                                           Open_table_context *ot_ctx)
{
  rpl_group_info *rgi= thd->rgi_slave ? thd->rgi_slave : thd->rgi_fake;
  if (!part_info || !part_info->is_range_interval() ||
      table_list->lock_type < TL_WRITE_ALLOW_WRITE ||
      table_list->mdl_request.type == MDL_EXCLUSIVE ||
      ot_ctx->range_interval_create_count > 0)
    return false;
  if (table_list->prelocking_placeholder != TABLE_LIST::PRELOCK_ROUTINE)
  {
    switch (thd->lex->sql_command)
    {
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_INSERT:
    case SQLCOM_LOAD:
    case SQLCOM_UPDATE:
    case SQLCOM_REPLACE:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_UPDATE_MULTI:
      break;
    case SQLCOM_END:
      if (!rgi || !rgi->current_event)
        return false;
      else
      {
        switch (rgi->current_event->get_type_code())
        {
        case UPDATE_ROWS_EVENT:
        case UPDATE_ROWS_EVENT_V1:
        case WRITE_ROWS_EVENT:
        case WRITE_ROWS_EVENT_V1:
          break;
        default:
          return false;
        }
      }
      break;
    default:
      return false;
    }
  }
  if (part_info->range_interval_set_count(
        thd, &ot_ctx->range_interval_create_count))
    return true;
  if (ot_ctx->range_interval_create_count == 0)
    return false;
  ot_ctx->request_backoff_action(
    Open_table_context::OT_ADD_RANGE_INTERVAL_PARTITION, table_list);
  return true;
}

/*
  @brief
    Determine the range of the new RANGE COLUMNS partitions by interval
*/
bool check_range_interval_constants(THD *thd, partition_info *part_info)
{
  List_iterator<partition_element> part_it(part_info->partitions);
  partition_element *el, *transition_el;
  Item *last_el_item;
  uint error, part_id= 0;
  int warn;
  /* No partition auto-creation in DDL. */
  if (thd_sql_command(thd) == SQLCOM_CREATE_TABLE ||
      thd_sql_command(thd) == SQLCOM_ALTER_TABLE)
    return FALSE;
  /* Range interval is only supported in RANGE COLUMNS with one column */
  DBUG_ASSERT(part_info->column_list);
  DBUG_ASSERT(part_info->part_field_list.elements == 1);
  /* Find the first partition to auto-add */
  while ((el= part_it++) && el->part_state == PART_NORMAL)
    part_id++;
  /* No partition to auto-add */
  if (!el)
    return FALSE;
  /*
    We are in a DML where partitions need to be created. There
    should already be some existing partitions
  */
  transition_el= part_info->partitions.elem(part_id - 1);
  if (!transition_el ||
      !(last_el_item = transition_el->get_col_val(0).item_expression))
  {
    DBUG_ASSERT(0);
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "no existing range partition, or that the transition partition"
               " has an invalid range");
    return true;
  }
  longlong packed= last_el_item->val_datetime_packed(thd);
  MYSQL_TIME ltime;
  unpack_time(packed, &ltime, MYSQL_TIMESTAMP_DATETIME);
  enum_field_types ftype= part_info->part_field_array[0]->type();
  decimal_digits_t dec= last_el_item->datetime_precision(thd);
  do
  {
    if (date_add_interval(thd, &ltime,
                          part_info->int_type, part_info->interval))
    {
      /* Same error as in check_vers_constants */
      my_error(ER_DATA_OUT_OF_RANGE, MYF(0),
               part_info->part_field_array[0]->type_handler()->name().ptr(),
               "INTERVAL");
      return TRUE;
    }
    Datetime dt(thd, &warn, &ltime, Datetime::Options(thd), dec);
    Date d(static_cast<const Temporal_with_date *>(&dt));
    Timestamp_or_zero_datetime ts(thd, &ltime, &error);
    Item *column_item;
    switch (ftype)
    {
    case MYSQL_TYPE_DATE:
      column_item= new (thd->mem_root) Item_date_literal(thd, &d);
      break;
    case MYSQL_TYPE_DATETIME:
      column_item= new (thd->mem_root) Item_datetime_literal(thd, &dt, dec);
      break;
    case MYSQL_TYPE_TIMESTAMP:
      /* Likely Year 2038/2106 Problem */
      if (error)
      {
        my_error(ER_DATA_OUT_OF_RANGE, MYF(0),
                 part_info->part_field_array[0]->type_handler()->name().ptr(),
                 "INTERVAL");
        return TRUE;
      }
      column_item= new (thd->mem_root) Item_timestamp_literal(thd, ts, dec);
      break;
    default:
      /* Only DATE, DATETIME and TIMESTAMP are allowed */
      DBUG_ASSERT(0);
      return TRUE;
    }
    part_elem_value *range_val= thd->calloc<part_elem_value>(1);
    part_column_list_val *col_val= thd->calloc<part_column_list_val>(1);
    if (unlikely(!column_item || !range_val || !col_val ||
                 el->list_val_list.push_back(range_val)))
      return TRUE;              /* OOM */
    range_val->col_val_array= col_val;
    col_val->item_expression= column_item;
    col_val->max_value= false;
    col_val->null_value= false;

    /*
      Similar to partition_info::fix_column_value_functions, but with a
      hack on field->table->write_set to pass the assertion of
      marked_for_write_or_computed() in
      Field_date_common::store_TIME_with_warning
    */
    Field *field= part_info->part_field_array[0];
    col_val->part_info= part_info;
    col_val->partition_id= part_id;
    uchar *val_ptr;
    uint len= field->pack_length();

    Sql_mode_instant_set sms(thd, 0);
    /* Needed to pass assertion on Field::marked_for_write_or_computed() */
    MY_BITMAP *save_write_set= field->table->write_set;
    field->table->write_set= &field->table->s->all_set;
    bool save_got_warning= thd->got_warning;
    thd->got_warning= FALSE;
    if (column_item->save_in_field(field, TRUE) || thd->got_warning)
    {
      field->table->write_set= save_write_set;
      my_error(ER_WRONG_TYPE_COLUMN_VALUE_ERROR, MYF(0));
      return TRUE;
    }
    thd->got_warning= save_got_warning;
    field->table->write_set= save_write_set;
    if (!(val_ptr= (uchar*) thd->memdup(field->ptr, len)))
    {
      return TRUE;
    }
    col_val->column_value= val_ptr;
    col_val->fixed= TRUE;
    part_id++;
  } while ((el= part_it++));
  return FALSE;
}

static int compare_int(const void *a, const void *b)
{
  if (*(int *) a < *(int *) b)
    return -1;
  if (*(int *) b < *(int *) a)
    return 1;
  return 0;
}

/*
  Find the first index for naming range interval auto created
  partitions. Find the first gap large enough to fit in all the new
  partitions.
*/
uint range_interval_next_part_no(uint new_parts,
                                 List<partition_element>& partitions)
{
  int *cur, *start, *end;
  List_iterator_fast<partition_element> it(partitions);
  partition_element *el;
  const char *name;
  uint right= new_parts;
  DBUG_ASSERT(new_parts > 0);
  if(!(start= (int *) my_alloca(sizeof(int) * partitions.elements)))
  {
    /* Out of memory */
    return 0;
  }
  cur= start;

  /* For each partition named pNUMBER, put the NUMBER into an array */
  while ((el= it++))
  {
    name= el->partition_name.str;
    if (name && name[0] == 'p')
    {
      *cur= atoi(name + 1);
      /* Ignore 0 which could be a failed conversion */
      if (*cur > 0)
        cur++;
    }
  }
  end= cur;

  /* Ok, got the numbers from pN partition names. Sort them. */
  my_qsort(start, end - start, sizeof(int), compare_int);

  /* Look for the gap that's large enough */
  for (cur= start; cur < end && right >= (uint) *cur; cur++)
    right= (uint) *cur + new_parts;
  my_afree(start);
  return right - new_parts + 1;
}
/*
  Similar to vers_create_partitions, create range interval partitions
*/
bool range_interval_create_partitions(THD* thd, TABLE_LIST* tl, uint num_parts)
{
  bool result= true;
  Table_specification_st create_info;
  Alter_info alter_info;
  TABLE *table= tl->table;
  partition_info *save_part_info= thd->work_part_info;
  Query_tables_list save_query_tables;
  Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
  bool save_no_write_to_binlog= thd->lex->no_write_to_binlog;
  thd->m_reprepare_observer= NULL;
  thd->lex->reset_n_backup_query_tables_list(&save_query_tables);
  /*
    Do not write the partition creation triggered by a DML into the
    binlog
  */
  thd->lex->no_write_to_binlog= true;

  DBUG_ASSERT(!thd->is_error());
  DBUG_ASSERT(num_parts);

  {
    alter_info.reset();
    alter_info.partition_flags= ALTER_PARTITION_ADD;
    create_info.init();
    create_info.alter_info= &alter_info;
    Alter_table_ctx alter_ctx(thd, tl, 1, &table->s->db, &table->s->table_name);

    MDL_REQUEST_INIT(&tl->mdl_request, MDL_key::TABLE, tl->db.str,
                    tl->table_name.str, MDL_SHARED_NO_WRITE, MDL_TRANSACTION);
    if (thd->mdl_context.acquire_lock(&tl->mdl_request,
                                      thd->variables.lock_wait_timeout))
      goto exit;
    DEBUG_SYNC(thd, "range_interval_create_partitions_lock_acquired");
    table->mdl_ticket= tl->mdl_request.ticket;

    create_info.db_type= table->s->db_type();
    DBUG_ASSERT(create_info.db_type);

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
    part_info->num_columns= table->part_info->num_columns;
    part_info->part_type= RANGE_PARTITION;
    /* for partition_info::fix_parser_data to exit early */
    part_info->int_type= table->part_info->int_type;

    thd->work_part_info= part_info;
    if (alter_add_partitions(thd, tl, alter_info, alter_ctx, create_info,
                            ER_RANGE_INTERVAL_PART_FAILED))
      goto exit;
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
#endif // WITH_PARTITION_STORAGE_ENGINE

/* Check and set interval value for auto interval partitioning */
bool partition_info::set_range_interval(THD* thd, Item* ival,
                                        interval_type type,
                                        const char *table_name)
{
  if (ival->fix_fields_if_needed_for_scalar(thd, &ival))
    return true;
  bool error= get_interval_value(thd, ival, type, &interval) ||
    interval.neg || interval.second_part ||
    !(interval.year || interval.month || interval.day || interval.hour ||
      interval.minute || interval.second);
  if (error)
  {
    my_error(ER_PART_WRONG_VALUE, MYF(0), table_name, "INTERVAL");
    return true;
  }
  int_type= type;
  return false;
}

/*
  Check and set interval value for auto interval partitioning, from
  the ORACLE NUMTODSINTERVAL (is_ds == true) or NUMTOYMINTERVAL (is_ds
  == false) format.

  @return
    false - Ok, this->interval and this->int_type are set to describe the
            interval.
    true  - Invalid interval specified.
*/
bool partition_info::set_range_interval(int num, LEX_CSTRING &type,
                                        bool is_ds,
                                        const char *table_name)
{
  if (num <= 0)
    goto end;
  if (is_ds)
  {
    if (type.length == 3 && !strncasecmp(type.str, "DAY", 3))
    {
      interval.day= num;
      int_type= INTERVAL_DAY;
      return false;
    }
    else if (type.length == 4 && !strncasecmp(type.str, "HOUR", 4))
    {
      interval.hour= num;
      int_type= INTERVAL_HOUR;
      return false;
    }
    else if (type.length == 6)
    {
      if (!strncasecmp(type.str, "MINUTE", 6))
      {
        interval.minute= num;
        int_type= INTERVAL_MINUTE;
        return false;
      }
      else if (!strncasecmp(type.str, "SECOND", 6))
      {
        interval.second= num;
        int_type= INTERVAL_SECOND;
        return false;
      }
    }
  }
  else
  {
    if (type.length == 4 && !strncasecmp(type.str, "YEAR", 4))
    {
      interval.year= num;
      int_type= INTERVAL_YEAR;
      return false;
    }
    else if (type.length == 5 && !strncasecmp(type.str, "MONTH", 5))
    {
      interval.month= num;
      int_type= INTERVAL_MONTH;
      return false;
    }
  }
end:
  my_error(ER_PART_WRONG_VALUE, MYF(0), table_name, "INTERVAL");
  return true;
}
