/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2011, 2021, MariaDB

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


/*
  Single table and multi table updates of tables.
  Multi-table updates were introduced by Sinisa & Monty
*/

#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "sql_update.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"                       // close_tables_for_reopen
#include "sql_parse.h"                          // cleanup_items
#include "sql_partition.h"                   // partition_key_modified
#include "sql_select.h"
#include "sql_view.h"                           // check_key_in_view
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_statistics.h"
#include "probes_mysql.h"
#include "debug_sync.h"
#include "key.h"                                // is_key_used
#include "records.h"                            // init_read_record,
                                                // end_read_record
#include "filesort.h"                           // filesort
#include "sql_derived.h" // mysql_derived_prepare,
                         // mysql_handle_derived,
                         // mysql_derived_filling


#include "sql_insert.h"  // For vers_insert_history_row() that may be
                         //   needed for System Versioning.

/**
   True if the table's input and output record buffers are comparable using
   compare_record(TABLE*).
 */
bool records_are_comparable(const TABLE *table) {
  return !table->versioned(VERS_TRX_ID) &&
          (((table->file->ha_table_flags() & HA_PARTIAL_COLUMN_READ) == 0) ||
           bitmap_is_subset(table->write_set, table->read_set));
}


/**
   Compares the input and outbut record buffers of the table to see if a row
   has changed.

   @return true if row has changed.
   @return false otherwise.
*/

bool compare_record(const TABLE *table)
{
  DBUG_ASSERT(records_are_comparable(table));

  if (table->file->ha_table_flags() & HA_PARTIAL_COLUMN_READ ||
      table->s->has_update_default_function)
  {
    /*
      Storage engine may not have read all columns of the record.  Fields
      (including NULL bits) not in the write_set may not have been read and
      can therefore not be compared.
      Or ON UPDATE DEFAULT NOW() could've changed field values, including
      NULL bits.
    */ 
    for (Field **ptr= table->field ; *ptr != NULL; ptr++)
    {
      Field *field= *ptr;
      if (field->has_explicit_value() && !field->vcol_info)
      {
        if (field->real_maybe_null())
        {
          uchar null_byte_index= (uchar)(field->null_ptr - table->record[0]);
          
          if (((table->record[0][null_byte_index]) & field->null_bit) !=
              ((table->record[1][null_byte_index]) & field->null_bit))
            return TRUE;
        }
        if (field->cmp_binary_offset(table->s->rec_buff_length))
          return TRUE;
      }
    }
    return FALSE;
  }
  
  /* 
     The storage engine has read all columns, so it's safe to compare all bits
     including those not in the write_set. This is cheaper than the
     field-by-field comparison done above.
  */ 
  if (table->s->can_cmp_whole_record)
    return cmp_record(table,record[1]);
  /* Compare null bits */
  if (memcmp(table->null_flags,
	     table->null_flags+table->s->rec_buff_length,
	     table->s->null_bytes_for_compare))
    return TRUE;				// Diff in NULL value
  /* Compare updated fields */
  for (Field **ptr= table->field ; *ptr ; ptr++)
  {
    Field *field= *ptr;
    if (field->has_explicit_value() && !field->vcol_info &&
	field->cmp_binary_offset(table->s->rec_buff_length))
      return TRUE;
  }
  return FALSE;
}


/*
  check that all fields are real fields

  SYNOPSIS
    check_fields()
    thd             thread handler
    items           Items for check

  RETURN
    TRUE  Items can't be used in UPDATE
    FALSE Items are OK
*/

static bool check_fields(THD *thd, TABLE_LIST *table, List<Item> &items,
                         bool update_view)
{
  Item *item;
  if (update_view)
  {
    List_iterator<Item> it(items);
    Item_field *field;
    while ((item= it++))
    {
      if (!(field= item->field_for_view_update()))
      {
        /* item has name, because it comes from VIEW SELECT list */
        my_error(ER_NONUPDATEABLE_COLUMN, MYF(0), item->name.str);
        return TRUE;
      }
      /*
        we make temporary copy of Item_field, to avoid influence of changing
        result_field on Item_ref which refer on this field
      */
      thd->change_item_tree(it.ref(),
                            new (thd->mem_root) Item_field(thd, field));
    }
  }

  if (thd->variables.sql_mode & MODE_SIMULTANEOUS_ASSIGNMENT)
  {
    // Make sure that a column is updated only once
    List_iterator_fast<Item> it(items);
    while ((item= it++))
    {
      item->field_for_view_update()->field->clear_has_explicit_value();
    }
    it.rewind();
    while ((item= it++))
    {
      Field *f= item->field_for_view_update()->field;
      if (f->has_explicit_value())
      {
        my_error(ER_UPDATED_COLUMN_ONLY_ONCE, MYF(0),
                 *(f->table_name), f->field_name.str);
        return TRUE;
      }
      f->set_has_explicit_value();
    }
  }

  if (table->has_period())
  {
    if (table->is_view_or_derived())
    {
      my_error(ER_IT_IS_A_VIEW, MYF(0), table->table_name.str);
      return TRUE;
    }
    if (thd->lex->sql_command == SQLCOM_UPDATE_MULTI)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "updating and querying the same temporal periods table");

      return true;
    }
    DBUG_ASSERT(thd->lex->sql_command == SQLCOM_UPDATE);
    for (List_iterator_fast<Item> it(items); (item=it++);)
    {
      Field *f= item->field_for_view_update()->field;
      vers_select_conds_t &period= table->period_conditions;
      if (period.field_start->field == f || period.field_end->field == f)
      {
        my_error(ER_PERIOD_COLUMNS_UPDATED, MYF(0),
                 item->name.str, period.name.str);
        return true;
      }
    }
  }
  return FALSE;
}

bool TABLE::vers_check_update(List<Item> &items)
{
  List_iterator<Item> it(items);
  if (!versioned_write())
    return false;

  while (Item *item= it++)
  {
    if (Item_field *item_field= item->field_for_view_update())
    {
      Field *field= item_field->field;
      if (field->table == this && !field->vers_update_unversioned())
      {
        no_cache= true;
        return true;
      }
    }
  }
  return false;
}

/**
  Re-read record if more columns are needed for error message.

  If we got a duplicate key error, we want to write an error
  message containing the value of the duplicate key. If we do not have
  all fields of the key value in record[0], we need to re-read the
  record with a proper read_set.

  @param[in] error   error number
  @param[in] table   table
*/

static void prepare_record_for_error_message(int error, TABLE *table)
{
  Field **field_p;
  Field *field;
  uint keynr;
  MY_BITMAP unique_map; /* Fields in offended unique. */
  my_bitmap_map unique_map_buf[bitmap_buffer_size(MAX_FIELDS)];
  DBUG_ENTER("prepare_record_for_error_message");

  /*
    Only duplicate key errors print the key value.
    If storage engine does always read all columns, we have the value alraedy.
  */
  if ((error != HA_ERR_FOUND_DUPP_KEY) ||
      !(table->file->ha_table_flags() & HA_PARTIAL_COLUMN_READ))
    DBUG_VOID_RETURN;

  /*
    Get the number of the offended index.
    We will see MAX_KEY if the engine cannot determine the affected index.
  */
  if (unlikely((keynr= table->file->get_dup_key(error)) >= MAX_KEY))
    DBUG_VOID_RETURN;

  /* Create unique_map with all fields used by that index. */
  my_bitmap_init(&unique_map, unique_map_buf, table->s->fields);
  table->mark_index_columns(keynr, &unique_map);

  /* Subtract read_set and write_set. */
  bitmap_subtract(&unique_map, table->read_set);
  bitmap_subtract(&unique_map, table->write_set);

  /*
    If the unique index uses columns that are neither in read_set
    nor in write_set, we must re-read the record.
    Otherwise no need to do anything.
  */
  if (bitmap_is_clear_all(&unique_map))
    DBUG_VOID_RETURN;

  /* Get identifier of last read record into table->file->ref. */
  table->file->position(table->record[0]);
  /* Add all fields used by unique index to read_set. */
  bitmap_union(table->read_set, &unique_map);
  /* Tell the engine about the new set. */
  table->file->column_bitmaps_signal();

  if ((error= table->file->ha_index_or_rnd_end()) ||
      (error= table->file->ha_rnd_init(0)))
  {
    table->file->print_error(error, MYF(0));
    DBUG_VOID_RETURN;
  }

  /* Read record that is identified by table->file->ref. */
  (void) table->file->ha_rnd_pos(table->record[1], table->file->ref);
  /* Copy the newly read columns into the new record. */
  for (field_p= table->field; (field= *field_p); field_p++)
    if (bitmap_is_set(&unique_map, field->field_index))
      field->copy_from_tmp(table->s->rec_buff_length);

  DBUG_VOID_RETURN;
}


static
int cut_fields_for_portion_of_time(THD *thd, TABLE *table,
                                   const vers_select_conds_t &period_conds)
{
  bool lcond= period_conds.field_start->val_datetime_packed(thd)
              < period_conds.start.item->val_datetime_packed(thd);
  bool rcond= period_conds.field_end->val_datetime_packed(thd)
              > period_conds.end.item->val_datetime_packed(thd);

  Field *start_field= table->field[table->s->period.start_fieldno];
  Field *end_field= table->field[table->s->period.end_fieldno];

  int res= 0;
  if (lcond)
  {
    res= period_conds.start.item->save_in_field(start_field, true);
    start_field->set_has_explicit_value();
  }

  if (likely(!res) && rcond)
  {
    res= period_conds.end.item->save_in_field(end_field, true);
    end_field->set_has_explicit_value();
  }

  return res;
}

/*
  Process usual UPDATE

  SYNOPSIS
    mysql_update()
    thd			thread handler
    fields		fields for update
    values		values of fields for update
    conds		WHERE clause expression
    order_num		number of elemen in ORDER BY clause
    order		ORDER BY clause list
    limit		limit clause

  RETURN
    0  - OK
    2  - privilege check and openning table passed, but we need to convert to
         multi-update because of view substitution
    1  - error
*/

int mysql_update(THD *thd,
                 TABLE_LIST *table_list,
                 List<Item> &fields,
		 List<Item> &values,
                 COND *conds,
                 uint order_num, ORDER *order,
                 ha_rows limit,
                 bool ignore,
                 ha_rows *found_return, ha_rows *updated_return)
{
  bool		using_limit= limit != HA_POS_ERROR;
  bool          safe_update= thd->variables.option_bits & OPTION_SAFE_UPDATES;
  bool          used_key_is_modified= FALSE, transactional_table;
  bool          will_batch= FALSE;
  bool		can_compare_record;
  int           res;
  int		error, loc_error;
  ha_rows       dup_key_found;
  bool          need_sort= TRUE;
  bool          reverse= FALSE;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  privilege_t   want_privilege(NO_ACL);
#endif
  uint          table_count= 0;
  ha_rows	updated, updated_or_same, found;
  key_map	old_covering_keys;
  TABLE		*table;
  SQL_SELECT	*select= NULL;
  SORT_INFO     *file_sort= 0;
  READ_RECORD	info;
  SELECT_LEX    *select_lex= thd->lex->first_select_lex();
  ulonglong     id;
  List<Item> all_fields;
  killed_state killed_status= NOT_KILLED;
  bool has_triggers, binlog_is_row, do_direct_update= FALSE;
  Update_plan query_plan(thd->mem_root);
  Explain_update *explain;
  TABLE_LIST *update_source_table;
  query_plan.index= MAX_KEY;
  query_plan.using_filesort= FALSE;

  // For System Versioning (may need to insert new fields to a table).
  ha_rows rows_inserted= 0;

  DBUG_ENTER("mysql_update");

  create_explain_query(thd->lex, thd->mem_root);
  if (open_tables(thd, &table_list, &table_count, 0))
    DBUG_RETURN(1);

  /* Prepare views so they are handled correctly */
  if (mysql_handle_derived(thd->lex, DT_INIT))
    DBUG_RETURN(1);

  if (table_list->has_period() && table_list->is_view_or_derived())
  {
    my_error(ER_IT_IS_A_VIEW, MYF(0), table_list->table_name.str);
    DBUG_RETURN(TRUE);
  }

  if (((update_source_table=unique_table(thd, table_list,
                                        table_list->next_global, 0)) ||
        table_list->is_multitable()))
  {
    DBUG_ASSERT(update_source_table || table_list->view != 0);
    DBUG_PRINT("info", ("Switch to multi-update"));
    /* pass counter value */
    thd->lex->table_count= table_count;
    if (thd->lex->period_conditions.is_set())
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "updating and querying the same temporal periods table");

      DBUG_RETURN(1);
    }

    /* convert to multiupdate */
    DBUG_RETURN(2);
  }
  if (lock_tables(thd, table_list, table_count, 0))
    DBUG_RETURN(1);

  (void) read_statistics_for_tables_if_needed(thd, table_list);

  THD_STAGE_INFO(thd, stage_init_update);
  if (table_list->handle_derived(thd->lex, DT_MERGE_FOR_INSERT))
    DBUG_RETURN(1);
  if (table_list->handle_derived(thd->lex, DT_PREPARE))
    DBUG_RETURN(1);

  table= table_list->table;

  if (!table_list->single_table_updatable())
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "UPDATE");
    DBUG_RETURN(1);
  }
  
  /* Calculate "table->covering_keys" based on the WHERE */
  table->covering_keys= table->s->keys_in_use;
  table->opt_range_keys.clear_all();

  query_plan.select_lex= thd->lex->first_select_lex();
  query_plan.table= table;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /* Force privilege re-checking for views after they have been opened. */
  want_privilege= (table_list->view ? UPDATE_ACL :
                   table_list->grant.want_privilege);
#endif
  thd->lex->promote_select_describe_flag_if_needed();

  if (mysql_prepare_update(thd, table_list, &conds, order_num, order))
    DBUG_RETURN(1);

  if (table_list->has_period())
  {
    if (!table_list->period_conditions.start.item->const_item()
        || !table_list->period_conditions.end.item->const_item())
    {
      my_error(ER_NOT_CONSTANT_EXPRESSION, MYF(0), "FOR PORTION OF");
      DBUG_RETURN(true);
    }
    table->no_cache= true;
  }

  old_covering_keys= table->covering_keys;		// Keys used in WHERE
  /* Check the fields we are going to modify */
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  table_list->grant.want_privilege= table->grant.want_privilege= want_privilege;
  table_list->register_want_access(want_privilege);
#endif
  /* 'Unfix' fields to allow correct marking by the setup_fields function. */
  if (table_list->is_view())
    unfix_fields(fields);

  if (setup_fields_with_no_wrap(thd, Ref_ptr_array(),
                                fields, MARK_COLUMNS_WRITE, 0, 0))
    DBUG_RETURN(1);                     /* purecov: inspected */
  if (check_fields(thd, table_list, fields, table_list->view))
  {
    DBUG_RETURN(1);
  }
  bool has_vers_fields= table->vers_check_update(fields);
  if (check_key_in_view(thd, table_list))
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "UPDATE");
    DBUG_RETURN(1);
  }

  if (table->default_field)
    table->mark_default_fields_for_write(false);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /* Check values */
  table_list->grant.want_privilege= table->grant.want_privilege=
    (SELECT_ACL & ~table->grant.privilege);
#endif
  if (setup_fields(thd, Ref_ptr_array(), values, MARK_COLUMNS_READ, 0, NULL, 0))
  {
    free_underlaid_joins(thd, select_lex);
    DBUG_RETURN(1);				/* purecov: inspected */
  }

  if (check_unique_table(thd, table_list))
    DBUG_RETURN(TRUE);

  switch_to_nullable_trigger_fields(fields, table);
  switch_to_nullable_trigger_fields(values, table);

  /* Apply the IN=>EXISTS transformation to all subqueries and optimize them */
  if (select_lex->optimize_unflattened_subqueries(false))
    DBUG_RETURN(TRUE);

  if (select_lex->inner_refs_list.elements &&
    fix_inner_refs(thd, all_fields, select_lex, select_lex->ref_pointer_array))
    DBUG_RETURN(1);

  if (conds)
  {
    Item::cond_result cond_value;
    conds= conds->remove_eq_conds(thd, &cond_value, true);
    if (cond_value == Item::COND_FALSE)
    {
      limit= 0;                                   // Impossible WHERE
      query_plan.set_impossible_where();
      if (thd->lex->describe || thd->lex->analyze_stmt)
        goto produce_explain_and_leave;
    }
  }

  // Don't count on usage of 'only index' when calculating which key to use
  table->covering_keys.clear_all();

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (prune_partitions(thd, table, conds))
  {
    free_underlaid_joins(thd, select_lex);

    query_plan.set_no_partitions();
    if (thd->lex->describe || thd->lex->analyze_stmt)
      goto produce_explain_and_leave;
    if (thd->is_error())
      DBUG_RETURN(1);

    my_ok(thd);				// No matching records
    DBUG_RETURN(0);
  }
#endif
  /* Update the table->file->stats.records number */
  table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
  set_statistics_for_table(thd, table);

  select= make_select(table, 0, 0, conds, (SORT_INFO*) 0, 0, &error);
  if (unlikely(error || !limit || thd->is_error() ||
               (select && select->check_quick(thd, safe_update, limit))))
  {
    query_plan.set_impossible_where();
    if (thd->lex->describe || thd->lex->analyze_stmt)
      goto produce_explain_and_leave;

    delete select;
    free_underlaid_joins(thd, select_lex);
    /*
      There was an error or the error was already sent by
      the quick select evaluation.
      TODO: Add error code output parameter to Item::val_xxx() methods.
      Currently they rely on the user checking DA for
      errors when unwinding the stack after calling Item::val_xxx().
    */
    if (error || thd->is_error())
    {
      DBUG_RETURN(1);				// Error in where
    }
    my_ok(thd);				// No matching records
    DBUG_RETURN(0);
  }

  /* If running in safe sql mode, don't allow updates without keys */
  if (table->opt_range_keys.is_clear_all())
  {
    thd->set_status_no_index_used();
    if (safe_update && !using_limit)
    {
      my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
		 ER_THD(thd, ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
      goto err;
    }
  }
  if (unlikely(init_ftfuncs(thd, select_lex, 1)))
    goto err;

  if (table_list->has_period())
  {
    table->use_all_columns();
    table->rpl_write_set= table->write_set;
  }
  else
  {
    table->mark_columns_needed_for_update();
  }

  table->update_const_key_parts(conds);
  order= simple_remove_const(order, conds);
  query_plan.scanned_rows= select? select->records: table->file->stats.records;
        
  if (select && select->quick && select->quick->unique_key_range())
  {
    /* Single row select (always "ordered"): Ok to use with key field UPDATE */
    need_sort= FALSE;
    query_plan.index= MAX_KEY;
    used_key_is_modified= FALSE;
  }
  else
  {
    ha_rows scanned_limit= query_plan.scanned_rows;
    table->no_keyread= 1;
    query_plan.index= get_index_for_order(order, table, select, limit,
                                          &scanned_limit, &need_sort,
                                          &reverse);
    table->no_keyread= 0;
    if (!need_sort)
      query_plan.scanned_rows= scanned_limit;

    if (select && select->quick)
    {
      DBUG_ASSERT(need_sort || query_plan.index == select->quick->index);
      used_key_is_modified= (!select->quick->unique_key_range() &&
                             select->quick->is_keys_used(table->write_set));
    }
    else
    {
      if (need_sort)
      {
        /* Assign table scan index to check below for modified key fields: */
        query_plan.index= table->file->key_used_on_scan;
      }
      if (query_plan.index != MAX_KEY)
      {
        /* Check if we are modifying a key that we are used to search with: */
        used_key_is_modified= is_key_used(table, query_plan.index,
                                          table->write_set);
      }
    }
  }
  
  /* 
    Query optimization is finished at this point.
     - Save the decisions in the query plan
     - if we're running EXPLAIN UPDATE, get out
  */
  query_plan.select= select;
  query_plan.possible_keys= select? select->possible_keys: key_map(0);
  
  if (used_key_is_modified || order ||
      partition_key_modified(table, table->write_set))
  {
    if (order && need_sort)
      query_plan.using_filesort= true;
    else
      query_plan.using_io_buffer= true;
  }

  /*
    Ok, we have generated a query plan for the UPDATE.
     - if we're running EXPLAIN UPDATE, goto produce explain output 
     - otherwise, execute the query plan
  */
  if (thd->lex->describe)
    goto produce_explain_and_leave;
  if (!(explain= query_plan.save_explain_update_data(query_plan.mem_root, thd)))
    goto err;

  ANALYZE_START_TRACKING(thd, &explain->command_tracker);

  DBUG_EXECUTE_IF("show_explain_probe_update_exec_start", 
                  dbug_serve_apcs(thd, 1););

  has_triggers= (table->triggers &&
                 (table->triggers->has_triggers(TRG_EVENT_UPDATE,
                                                TRG_ACTION_BEFORE) ||
                 table->triggers->has_triggers(TRG_EVENT_UPDATE,
                                               TRG_ACTION_AFTER)));

  if (table_list->has_period())
    has_triggers= table->triggers &&
                  (table->triggers->has_triggers(TRG_EVENT_INSERT,
                                                 TRG_ACTION_BEFORE)
                   || table->triggers->has_triggers(TRG_EVENT_INSERT,
                                                    TRG_ACTION_AFTER)
                   || has_triggers);
  DBUG_PRINT("info", ("has_triggers: %s", has_triggers ? "TRUE" : "FALSE"));
  binlog_is_row= thd->is_current_stmt_binlog_format_row();
  DBUG_PRINT("info", ("binlog_is_row: %s", binlog_is_row ? "TRUE" : "FALSE"));

  if (!(select && select->quick))
    status_var_increment(thd->status_var.update_scan_count);

  /*
    We can use direct update (update that is done silently in the handler)
    if none of the following conditions are true:
    - There are triggers
    - There is binary logging
    - using_io_buffer
      - This means that the partition changed or the key we want
        to use for scanning the table is changed
    - ignore is set
      - Direct updates don't return the number of ignored rows
    - There is a virtual not stored column in the WHERE clause
    - Changing a field used by a stored virtual column, which
      would require the column to be recalculated.
    - ORDER BY or LIMIT
      - As this requires the rows to be updated in a specific order
      - Note that Spider can handle ORDER BY and LIMIT in a cluster with
        one data node.  These conditions are therefore checked in
        direct_update_rows_init().
    - Update fields include a unique timestamp field
      - The storage engine may not be able to avoid false duplicate key
        errors.  This condition is checked in direct_update_rows_init().

    Direct update does not require a WHERE clause

    Later we also ensure that we are only using one table (no sub queries)
  */
  DBUG_PRINT("info", ("HA_CAN_DIRECT_UPDATE_AND_DELETE: %s", (table->file->ha_table_flags() & HA_CAN_DIRECT_UPDATE_AND_DELETE) ? "TRUE" : "FALSE"));
  DBUG_PRINT("info", ("using_io_buffer: %s", query_plan.using_io_buffer ? "TRUE" : "FALSE"));
  DBUG_PRINT("info", ("ignore: %s", ignore ? "TRUE" : "FALSE"));
  DBUG_PRINT("info", ("virtual_columns_marked_for_read: %s", table->check_virtual_columns_marked_for_read() ? "TRUE" : "FALSE"));
  DBUG_PRINT("info", ("virtual_columns_marked_for_write: %s", table->check_virtual_columns_marked_for_write() ? "TRUE" : "FALSE"));
  if ((table->file->ha_table_flags() & HA_CAN_DIRECT_UPDATE_AND_DELETE) &&
      !has_triggers && !binlog_is_row &&
      !query_plan.using_io_buffer && !ignore &&
      !table->check_virtual_columns_marked_for_read() &&
      !table->check_virtual_columns_marked_for_write())
  {
    DBUG_PRINT("info", ("Trying direct update"));
    bool use_direct_update= !select || !select->cond;
    if (!use_direct_update &&
        (select->cond->used_tables() & ~RAND_TABLE_BIT) == table->map)
    {
      DBUG_ASSERT(!table->file->pushed_cond);
      if (!table->file->cond_push(select->cond))
      {
        use_direct_update= TRUE;
        table->file->pushed_cond= select->cond;
      }
    }

    if (use_direct_update &&
        !table->file->info_push(INFO_KIND_UPDATE_FIELDS, &fields) &&
        !table->file->info_push(INFO_KIND_UPDATE_VALUES, &values) &&
        !table->file->direct_update_rows_init(&fields))
    {
      do_direct_update= TRUE;

      /* Direct update is not using_filesort and is not using_io_buffer */
      goto update_begin;
    }
  }

  if (query_plan.using_filesort || query_plan.using_io_buffer)
  {
    /*
      We can't update table directly;  We must first search after all
      matching rows before updating the table!

      note: We avoid sorting if we sort on the used index
    */
    if (query_plan.using_filesort)
    {
      /*
	Doing an ORDER BY;  Let filesort find and sort the rows we are going
	to update
        NOTE: filesort will call table->prepare_for_position()
      */
      Filesort fsort(order, limit, true, select);

      Filesort_tracker *fs_tracker= 
        thd->lex->explain->get_upd_del_plan()->filesort_tracker;

      if (!(file_sort= filesort(thd, table, &fsort, fs_tracker)))
	goto err;
      thd->inc_examined_row_count(file_sort->examined_rows);

      /*
	Filesort has already found and selected the rows we want to update,
	so we don't need the where clause
      */
      delete select;
      select= 0;
    }
    else
    {
      MY_BITMAP *save_read_set= table->read_set;
      MY_BITMAP *save_write_set= table->write_set;

      if (query_plan.index < MAX_KEY && old_covering_keys.is_set(query_plan.index))
        table->prepare_for_keyread(query_plan.index);
      else
        table->use_all_columns();

      /*
	We are doing a search on a key that is updated. In this case
	we go trough the matching rows, save a pointer to them and
	update these in a separate loop based on the pointer.
      */
      explain->buf_tracker.on_scan_init();
      IO_CACHE tempfile;
      if (open_cached_file(&tempfile, mysql_tmpdir,TEMP_PREFIX,
			   DISK_BUFFER_SIZE, MYF(MY_WME)))
	goto err;

      /* If quick select is used, initialize it before retrieving rows. */
      if (select && select->quick && select->quick->reset())
      {
        close_cached_file(&tempfile);
        goto err;
      }

      table->file->try_semi_consistent_read(1);

      /*
        When we get here, we have one of the following options:
        A. query_plan.index == MAX_KEY
           This means we should use full table scan, and start it with
           init_read_record call
        B. query_plan.index != MAX_KEY
           B.1 quick select is used, start the scan with init_read_record
           B.2 quick select is not used, this is full index scan (with LIMIT)
           Full index scan must be started with init_read_record_idx
      */

      if (query_plan.index == MAX_KEY || (select && select->quick))
        error= init_read_record(&info, thd, table, select, NULL, 0, 1, FALSE);
      else
        error= init_read_record_idx(&info, thd, table, 1, query_plan.index,
                                    reverse);

      if (unlikely(error))
      {
        close_cached_file(&tempfile);
        goto err;
      }

      THD_STAGE_INFO(thd, stage_searching_rows_for_update);
      ha_rows tmp_limit= limit;

      while (likely(!(error=info.read_record())) && likely(!thd->killed))
      {
        explain->buf_tracker.on_record_read();
        thd->inc_examined_row_count(1);
	if (!select || (error= select->skip_record(thd)) > 0)
	{
          if (table->file->ha_was_semi_consistent_read())
	    continue;  /* repeat the read of the same row if it still exists */

          explain->buf_tracker.on_record_after_where();
	  table->file->position(table->record[0]);
	  if (unlikely(my_b_write(&tempfile,table->file->ref,
                                  table->file->ref_length)))
	  {
	    error=1; /* purecov: inspected */
	    break; /* purecov: inspected */
	  }
	  if (!--limit && using_limit)
	  {
	    error= -1;
	    break;
	  }
	}
	else
        {
          /*
            Don't try unlocking the row if skip_record reported an
            error since in this case the transaction might have been
            rolled back already.
          */
          if (unlikely(error < 0))
          {
            /* Fatal error from select->skip_record() */
            error= 1;
            break;
          }
          else
            table->file->unlock_row();
        }
      }
      if (unlikely(thd->killed) && !error)
	error= 1;				// Aborted
      limit= tmp_limit;
      table->file->try_semi_consistent_read(0);
      end_read_record(&info);
     
      /* Change select to use tempfile */
      if (select)
      {
	delete select->quick;
	if (select->free_cond)
	  delete select->cond;
	select->quick=0;
	select->cond=0;
      }
      else
      {
	if (!(select= new SQL_SELECT))
          goto err;
	select->head=table;
      }

      if (unlikely(reinit_io_cache(&tempfile,READ_CACHE,0L,0,0)))
	error= 1; /* purecov: inspected */
      select->file= tempfile;			// Read row ptrs from this file
       if (unlikely(error >= 0))
	goto err;

      table->file->ha_end_keyread();
      table->column_bitmaps_set(save_read_set, save_write_set);
    }
  }

update_begin:
  if (ignore)
    table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  
  if (select && select->quick && select->quick->reset())
    goto err;
  table->file->try_semi_consistent_read(1);
  if (init_read_record(&info, thd, table, select, file_sort, 0, 1, FALSE))
    goto err;

  updated= updated_or_same= found= 0;
  /*
    Generate an error (in TRADITIONAL mode) or warning
    when trying to set a NOT NULL field to NULL.
  */
  thd->count_cuted_fields= CHECK_FIELD_WARN;
  thd->cuted_fields=0L;

  transactional_table= table->file->has_transactions_and_rollback();
  thd->abort_on_warning= !ignore && thd->is_strict_mode();

  if (do_direct_update)
  {
    /* Direct updating is supported */
    ha_rows update_rows= 0, found_rows= 0;
    DBUG_PRINT("info", ("Using direct update"));
    table->reset_default_fields();
    if (unlikely(!(error= table->file->ha_direct_update_rows(&update_rows,
                                                             &found_rows))))
      error= -1;
    updated= update_rows;
    found= found_rows;
    if (found < updated)
      found= updated;
    goto update_end;
  }

  if ((table->file->ha_table_flags() & HA_CAN_FORCE_BULK_UPDATE) &&
      !table->prepare_triggers_for_update_stmt_or_event() &&
      !thd->lex->with_rownum)
    will_batch= !table->file->start_bulk_update();

  /*
    Assure that we can use position()
    if we need to create an error message.
  */
  if (table->file->ha_table_flags() & HA_PARTIAL_COLUMN_READ)
    table->prepare_for_position();

  table->reset_default_fields();

  /*
    We can use compare_record() to optimize away updates if
    the table handler is returning all columns OR if
    if all updated columns are read
  */
  can_compare_record= records_are_comparable(table);
  explain->tracker.on_scan_init();

  table->file->prepare_for_insert(1);
  DBUG_ASSERT(table->file->inited != handler::NONE);

  THD_STAGE_INFO(thd, stage_updating);
  fix_rownum_pointers(thd, thd->lex->current_select, &updated_or_same);
  thd->get_stmt_da()->reset_current_row_for_warning(1);
  while (!(error=info.read_record()) && !thd->killed)
  {
    explain->tracker.on_record_read();
    thd->inc_examined_row_count(1);
    if (!select || select->skip_record(thd) > 0)
    {
      if (table->file->ha_was_semi_consistent_read())
        continue;  /* repeat the read of the same row if it still exists */

      explain->tracker.on_record_after_where();
      store_record(table,record[1]);

      if (table_list->has_period())
        cut_fields_for_portion_of_time(thd, table,
                                       table_list->period_conditions);

      if (fill_record_n_invoke_before_triggers(thd, table, fields, values, 0,
                                               TRG_EVENT_UPDATE))
        break; /* purecov: inspected */

      found++;

      bool record_was_same= false;
      bool need_update= !can_compare_record || compare_record(table);

      if (need_update)
      {
        if (table->versioned(VERS_TIMESTAMP) &&
            thd->lex->sql_command == SQLCOM_DELETE)
          table->vers_update_end();

        if ((res= table_list->view_check_option(thd, ignore)) !=
            VIEW_CHECK_OK)
        {
          found--;
          if (res == VIEW_CHECK_SKIP)
            continue;
          else if (res == VIEW_CHECK_ERROR)
          {
            error= 1;
            break;
          }
        }
        if (will_batch)
        {
          /*
            Typically a batched handler can execute the batched jobs when:
            1) When specifically told to do so
            2) When it is not a good idea to batch anymore
            3) When it is necessary to send batch for other reasons
               (One such reason is when READ's must be performed)

            1) is covered by exec_bulk_update calls.
            2) and 3) is handled by the bulk_update_row method.
            
            bulk_update_row can execute the updates including the one
            defined in the bulk_update_row or not including the row
            in the call. This is up to the handler implementation and can
            vary from call to call.

            The dup_key_found reports the number of duplicate keys found
            in those updates actually executed. It only reports those if
            the extra call with HA_EXTRA_IGNORE_DUP_KEY have been issued.
            If this hasn't been issued it returns an error code and can
            ignore this number. Thus any handler that implements batching
            for UPDATE IGNORE must also handle this extra call properly.

            If a duplicate key is found on the record included in this
            call then it should be included in the count of dup_key_found
            and error should be set to 0 (only if these errors are ignored).
          */
          DBUG_PRINT("info", ("Batched update"));
          error= table->file->ha_bulk_update_row(table->record[1],
                                                 table->record[0],
                                                 &dup_key_found);
          limit+= dup_key_found;
          updated-= dup_key_found;
        }
        else
        {
          /* Non-batched update */
          error= table->file->ha_update_row(table->record[1],
                                            table->record[0]);
        }

        record_was_same= error == HA_ERR_RECORD_IS_THE_SAME;
        if (unlikely(record_was_same))
        {
          error= 0;
          updated_or_same++;
        }
        else if (likely(!error))
        {
          if (has_vers_fields && table->versioned(VERS_TRX_ID))
            rows_inserted++;
          updated++;
          updated_or_same++;
        }

        if (likely(!error) && !record_was_same && table_list->has_period())
        {
          store_record(table, record[2]);
          restore_record(table, record[1]);
          error= table->insert_portion_of_time(thd,
                                               table_list->period_conditions,
                                               &rows_inserted);
          restore_record(table, record[2]);
        }

        if (unlikely(error) &&
            (!ignore || table->file->is_fatal_error(error, HA_CHECK_ALL)))
        {
          goto error;
        }
      }
      else
        updated_or_same++;

      if (likely(!error) && has_vers_fields && table->versioned(VERS_TIMESTAMP))
      {
        store_record(table, record[2]);
        table->mark_columns_per_binlog_row_image();
        error= vers_insert_history_row(table);
        restore_record(table, record[2]);
        if (unlikely(error))
        {
error:
          /*
            If (ignore && error is ignorable) we don't have to
            do anything; otherwise...
          */
          myf flags= 0;

          if (table->file->is_fatal_error(error, HA_CHECK_ALL))
            flags|= ME_FATAL; /* Other handler errors are fatal */

          prepare_record_for_error_message(error, table);
          table->file->print_error(error,MYF(flags));
          error= 1;
          break;
        }
        rows_inserted++;
      }

      if (table->triggers &&
          unlikely(table->triggers->process_triggers(thd, TRG_EVENT_UPDATE,
                                                     TRG_ACTION_AFTER, TRUE)))
      {
        error= 1;
        break;
      }

      if (!--limit && using_limit)
      {
        /*
          We have reached end-of-file in most common situations where no
          batching has occurred and if batching was supposed to occur but
          no updates were made and finally when the batch execution was
          performed without error and without finding any duplicate keys.
          If the batched updates were performed with errors we need to
          check and if no error but duplicate key's found we need to
          continue since those are not counted for in limit.
        */
        if (will_batch &&
            ((error= table->file->exec_bulk_update(&dup_key_found)) ||
             dup_key_found))
        {
 	  if (error)
          {
            /* purecov: begin inspected */
            /*
              The handler should not report error of duplicate keys if they
              are ignored. This is a requirement on batching handlers.
            */
            prepare_record_for_error_message(error, table);
            table->file->print_error(error,MYF(0));
            error= 1;
            break;
            /* purecov: end */
          }
          /*
            Either an error was found and we are ignoring errors or there
            were duplicate keys found. In both cases we need to correct
            the counters and continue the loop.
          */
          limit= dup_key_found; //limit is 0 when we get here so need to +
          updated-= dup_key_found;
        }
        else
        {
	  error= -1;				// Simulate end of file
	  break;
        }
      }
    }
    /*
      Don't try unlocking the row if skip_record reported an error since in
      this case the transaction might have been rolled back already.
    */
    else if (likely(!thd->is_error()))
      table->file->unlock_row();
    else
    {
      error= 1;
      break;
    }
    thd->get_stmt_da()->inc_current_row_for_warning();
    if (unlikely(thd->is_error()))
    {
      error= 1;
      break;
    }
  }
  ANALYZE_STOP_TRACKING(thd, &explain->command_tracker);
  table->auto_increment_field_not_null= FALSE;
  dup_key_found= 0;
  /*
    Caching the killed status to pass as the arg to query event constuctor;
    The cached value can not change whereas the killed status can
    (externally) since this point and change of the latter won't affect
    binlogging.
    It's assumed that if an error was set in combination with an effective 
    killed status then the error is due to killing.
  */
  killed_status= thd->killed; // get the status of the volatile 
  // simulated killing after the loop must be ineffective for binlogging
  DBUG_EXECUTE_IF("simulate_kill_bug27571",
                  {
                    thd->set_killed(KILL_QUERY);
                  };);
  error= (killed_status == NOT_KILLED)?  error : 1;
  
  if (likely(error) &&
      will_batch &&
      (loc_error= table->file->exec_bulk_update(&dup_key_found)))
    /*
      An error has occurred when a batched update was performed and returned
      an error indication. It cannot be an allowed duplicate key error since
      we require the batching handler to treat this as a normal behavior.

      Otherwise we simply remove the number of duplicate keys records found
      in the batched update.
    */
  {
    /* purecov: begin inspected */
    prepare_record_for_error_message(loc_error, table);
    table->file->print_error(loc_error,MYF(ME_FATAL));
    error= 1;
    /* purecov: end */
  }
  else
    updated-= dup_key_found;
  if (will_batch)
    table->file->end_bulk_update();

update_end:
  table->file->try_semi_consistent_read(0);

  if (!transactional_table && updated > 0)
    thd->transaction->stmt.modified_non_trans_table= TRUE;

  end_read_record(&info);
  delete select;
  select= NULL;
  THD_STAGE_INFO(thd, stage_end);
  if (table_list->has_period())
    table->file->ha_release_auto_increment();
  (void) table->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);

  /*
    Invalidate the table in the query cache if something changed.
    This must be before binlog writing and ha_autocommit_...
  */
  if (updated)
  {
    query_cache_invalidate3(thd, table_list, 1);
  }
  
  if (thd->transaction->stmt.modified_non_trans_table)
      thd->transaction->all.modified_non_trans_table= TRUE;
  thd->transaction->all.m_unsafe_rollback_flags|=
    (thd->transaction->stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);

  /*
    error < 0 means really no error at all: we processed all rows until the
    last one without error. error > 0 means an error (e.g. unique key
    violation and no IGNORE or REPLACE). error == 0 is also an error (if
    preparing the record or invoking before triggers fails). See
    ha_autocommit_or_rollback(error>=0) and DBUG_RETURN(error>=0) below.
    Sometimes we want to binlog even if we updated no rows, in case user used
    it to be sure master and slave are in same state.
  */
  if (likely(error < 0) || thd->transaction->stmt.modified_non_trans_table)
  {
    if (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      int errcode= 0;
      if (likely(error < 0))
        thd->clear_error();
      else
        errcode= query_error_code(thd, killed_status == NOT_KILLED);

      ScopedStatementReplication scoped_stmt_rpl(
          table->versioned(VERS_TRX_ID) ? thd : NULL);

      if (thd->binlog_query(THD::ROW_QUERY_TYPE,
                            thd->query(), thd->query_length(),
                            transactional_table, FALSE, FALSE, errcode) > 0)
      {
        error=1;				// Rollback update
      }
    }
  }
  DBUG_ASSERT(transactional_table || !updated || thd->transaction->stmt.modified_non_trans_table);
  free_underlaid_joins(thd, select_lex);
  delete file_sort;
  if (table->file->pushed_cond)
  {
    table->file->pushed_cond= 0;
    table->file->cond_pop();
  }

  /* If LAST_INSERT_ID(X) was used, report X */
  id= thd->arg_of_last_insert_id_function ?
    thd->first_successful_insert_id_in_prev_stmt : 0;

  if (likely(error < 0) && likely(!thd->lex->analyze_stmt))
  {
    char buff[MYSQL_ERRMSG_SIZE];
    if (!table->versioned(VERS_TIMESTAMP) && !table_list->has_period())
      my_snprintf(buff, sizeof(buff), ER_THD(thd, ER_UPDATE_INFO), (ulong) found,
                  (ulong) updated,
                  (ulong) thd->get_stmt_da()->current_statement_warn_count());
    else
      my_snprintf(buff, sizeof(buff),
                  ER_THD(thd, ER_UPDATE_INFO_WITH_SYSTEM_VERSIONING),
                  (ulong) found, (ulong) updated, (ulong) rows_inserted,
                  (ulong) thd->get_stmt_da()->current_statement_warn_count());
    my_ok(thd, (thd->client_capabilities & CLIENT_FOUND_ROWS) ? found : updated,
          id, buff);
    DBUG_PRINT("info",("%ld records updated", (long) updated));
  }
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;		/* calc cuted fields */
  thd->abort_on_warning= 0;
  if (thd->lex->current_select->first_cond_optimization)
  {
    thd->lex->current_select->save_leaf_tables(thd);
    thd->lex->current_select->first_cond_optimization= 0;
  }
  *found_return= found;
  *updated_return= updated;
  
  if (unlikely(thd->lex->analyze_stmt))
    goto emit_explain_and_leave;

  DBUG_RETURN((error >= 0 || thd->is_error()) ? 1 : 0);

err:
  delete select;
  delete file_sort;
  free_underlaid_joins(thd, select_lex);
  table->file->ha_end_keyread();
  if (table->file->pushed_cond)
    table->file->cond_pop();
  thd->abort_on_warning= 0;
  DBUG_RETURN(1);

produce_explain_and_leave:
  /* 
    We come here for various "degenerate" query plans: impossible WHERE,
    no-partitions-used, impossible-range, etc.
  */
  if (unlikely(!query_plan.save_explain_update_data(query_plan.mem_root, thd)))
    goto err;

emit_explain_and_leave:
  int err2= thd->lex->explain->send_explain(thd);

  delete select;
  free_underlaid_joins(thd, select_lex);
  DBUG_RETURN((err2 || thd->is_error()) ? 1 : 0);
}

/*
  Prepare items in UPDATE statement

  SYNOPSIS
    mysql_prepare_update()
    thd			- thread handler
    table_list		- global/local table list
    conds		- conditions
    order_num		- number of ORDER BY list entries
    order		- ORDER BY clause list

  RETURN VALUE
    FALSE OK
    TRUE  error
*/
bool mysql_prepare_update(THD *thd, TABLE_LIST *table_list,
			 Item **conds, uint order_num, ORDER *order)
{
  Item *fake_conds= 0;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  TABLE *table= table_list->table;
#endif
  List<Item> all_fields;
  SELECT_LEX *select_lex= thd->lex->first_select_lex();
  DBUG_ENTER("mysql_prepare_update");

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  table_list->grant.want_privilege= table->grant.want_privilege= 
    (SELECT_ACL & ~table->grant.privilege);
  table_list->register_want_access(SELECT_ACL);
#endif

  thd->lex->allow_sum_func.clear_all();

  if (table_list->has_period() &&
      select_lex->period_setup_conds(thd, table_list))
      DBUG_RETURN(true);

  DBUG_ASSERT(table_list->table);
  // conds could be cached from previous SP call
  DBUG_ASSERT(!table_list->vers_conditions.need_setup() ||
              !*conds || thd->stmt_arena->is_stmt_execute());
  if (select_lex->vers_setup_conds(thd, table_list))
    DBUG_RETURN(TRUE);

  *conds= select_lex->where;

  /*
    We do not call DT_MERGE_FOR_INSERT because it has no sense for simple
    (not multi-) update
  */
  if (mysql_handle_derived(thd->lex, DT_PREPARE))
    DBUG_RETURN(TRUE);

  if (setup_tables_and_check_access(thd, &select_lex->context, 
                                    &select_lex->top_join_list, table_list,
                                    select_lex->leaf_tables,
                                    FALSE, UPDATE_ACL, SELECT_ACL, TRUE) ||
      setup_conds(thd, table_list, select_lex->leaf_tables, conds) ||
      select_lex->setup_ref_array(thd, order_num) ||
      setup_order(thd, select_lex->ref_pointer_array,
		  table_list, all_fields, all_fields, order) ||
      setup_ftfuncs(select_lex))
    DBUG_RETURN(TRUE);


  select_lex->fix_prepare_information(thd, conds, &fake_conds);
  DBUG_RETURN(FALSE);
}

/**
  Check that we are not using table that we are updating in a sub select

  @param thd             Thread handle
  @param table_list      List of table with first to check

  @retval TRUE  Error
  @retval FALSE OK
*/
bool check_unique_table(THD *thd, TABLE_LIST *table_list)
{
  TABLE_LIST *duplicate;
  DBUG_ENTER("check_unique_table");
  if ((duplicate= unique_table(thd, table_list, table_list->next_global, 0)))
  {
    update_non_unique_table_error(table_list, "UPDATE", duplicate);
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

/***************************************************************************
  Update multiple tables from join 
***************************************************************************/

/*
  Get table map for list of Item_field
*/

static table_map get_table_map(List<Item> *items)
{
  List_iterator_fast<Item> item_it(*items);
  Item_field *item;
  table_map map= 0;

  while ((item= (Item_field *) item_it++))
    map|= item->all_used_tables();
  DBUG_PRINT("info", ("table_map: 0x%08lx", (long) map));
  return map;
}

/**
  If one row is updated through two different aliases and the first
  update physically moves the row, the second update will error
  because the row is no longer located where expected. This function
  checks if the multiple-table update is about to do that and if so
  returns with an error.

  The following update operations physically moves rows:
    1) Update of a column in a clustered primary key
    2) Update of a column used to calculate which partition the row belongs to

  This function returns with an error if both of the following are
  true:

    a) A table in the multiple-table update statement is updated
       through multiple aliases (including views)
    b) At least one of the updates on the table from a) may physically
       moves the row. Note: Updating a column used to calculate which
       partition a row belongs to does not necessarily mean that the
       row is moved. The new value may or may not belong to the same
       partition.

  @param leaves               First leaf table
  @param tables_for_update    Map of tables that are updated

  @return
    true   if the update is unsafe, in which case an error message is also set,
    false  otherwise.
*/
static
bool unsafe_key_update(List<TABLE_LIST> leaves, table_map tables_for_update)
{
  List_iterator_fast<TABLE_LIST> it(leaves), it2(leaves);
  TABLE_LIST *tl, *tl2;

  while ((tl= it++))
  {
    if (!tl->is_jtbm() && (tl->table->map & tables_for_update))
    {
      TABLE *table1= tl->table;
      bool primkey_clustered= (table1->file->
                               pk_is_clustering_key(table1->s->primary_key));

      bool table_partitioned= false;
#ifdef WITH_PARTITION_STORAGE_ENGINE
      table_partitioned= (table1->part_info != NULL);
#endif

      if (!table_partitioned && !primkey_clustered)
        continue;

      it2.rewind();
      while ((tl2= it2++))
      {
        if (tl2->is_jtbm())
          continue;
        /*
          Look at "next" tables only since all previous tables have
          already been checked
        */
        TABLE *table2= tl2->table;
        if (tl2 != tl &&
            table2->map & tables_for_update && table1->s == table2->s)
        {
          // A table is updated through two aliases
          if (table_partitioned &&
              (partition_key_modified(table1, table1->write_set) ||
               partition_key_modified(table2, table2->write_set)))
          {
            // Partitioned key is updated
            my_error(ER_MULTI_UPDATE_KEY_CONFLICT, MYF(0),
                     tl->top_table()->alias.str,
                     tl2->top_table()->alias.str);
            return true;
          }

          if (primkey_clustered)
          {
            // The primary key can cover multiple columns
            KEY key_info= table1->key_info[table1->s->primary_key];
            KEY_PART_INFO *key_part= key_info.key_part;
            KEY_PART_INFO *key_part_end= key_part + key_info.user_defined_key_parts;

            for (;key_part != key_part_end; ++key_part)
            {
              if (bitmap_is_set(table1->write_set, key_part->fieldnr-1) ||
                  bitmap_is_set(table2->write_set, key_part->fieldnr-1))
              {
                // Clustered primary key is updated
                my_error(ER_MULTI_UPDATE_KEY_CONFLICT, MYF(0),
                         tl->top_table()->alias.str,
                         tl2->top_table()->alias.str);
                return true;
              }
            }
          }
        }
      }
    }
  }
  return false;
}

/**
  Check if there is enough privilege on specific table used by the
  main select list of multi-update directly or indirectly (through
  a view).

  @param[in]      thd                Thread context.
  @param[in]      table              Table list element for the table.
  @param[in]      tables_for_update  Bitmap with tables being updated.
  @param[in/out]  updated_arg        Set to true if table in question is
                                     updated, also set to true if it is
                                     a view and one of its underlying
                                     tables is updated. Should be
                                     initialized to false by the caller
                                     before a sequence of calls to this
                                     function.

  @note To determine which tables/views are updated we have to go from
        leaves to root since tables_for_update contains map of leaf
        tables being updated and doesn't include non-leaf tables
        (fields are already resolved to leaf tables).

  @retval false - Success, all necessary privileges on all tables are
                  present or might be present on column-level.
  @retval true  - Failure, some necessary privilege on some table is
                  missing.
*/

static bool multi_update_check_table_access(THD *thd, TABLE_LIST *table,
                                            table_map tables_for_update,
                                            bool *updated_arg)
{
  if (table->view)
  {
    bool updated= false;
    /*
      If it is a mergeable view then we need to check privileges on its
      underlying tables being merged (including views). We also need to
      check if any of them is updated in order to find if this view is
      updated.
      If it is a non-mergeable view then it can't be updated.
    */
    DBUG_ASSERT(table->merge_underlying_list ||
                (!table->updatable &&
                 !(table->table->map & tables_for_update)));

    for (TABLE_LIST *tbl= table->merge_underlying_list; tbl;
         tbl= tbl->next_local)
    {
      if (multi_update_check_table_access(thd, tbl, tables_for_update,
                                          &updated))
      {
        tbl->hide_view_error(thd);
        return true;
      }
    }
    if (check_table_access(thd, updated ? UPDATE_ACL: SELECT_ACL, table,
                           FALSE, 1, FALSE))
      return true;
    *updated_arg|= updated;
    /* We only need SELECT privilege for columns in the values list. */
    table->grant.want_privilege= SELECT_ACL & ~table->grant.privilege;
  }
  else
  {
    /* Must be a base or derived table. */
    const bool updated= table->table->map & tables_for_update;
    if (check_table_access(thd, updated ? UPDATE_ACL : SELECT_ACL, table,
                           FALSE, 1, FALSE))
      return true;
    *updated_arg|= updated;
    /* We only need SELECT privilege for columns in the values list. */
    if (!table->derived)
    {
      table->grant.want_privilege= SELECT_ACL & ~table->grant.privilege;
      table->table->grant.want_privilege= (SELECT_ACL &
                                           ~table->table->grant.privilege);
    }
  }
  return false;
}


class Multiupdate_prelocking_strategy : public DML_prelocking_strategy
{
  bool done;
  bool has_prelocking_list;
public:
  void reset(THD *thd);
  bool handle_end(THD *thd);
};

void Multiupdate_prelocking_strategy::reset(THD *thd)
{
  done= false;
  has_prelocking_list= thd->lex->requires_prelocking();
}

/**
  Determine what tables could be updated in the multi-update

  For these tables we'll need to open triggers and continue prelocking
  until all is open.
*/
bool Multiupdate_prelocking_strategy::handle_end(THD *thd)
{
  DBUG_ENTER("Multiupdate_prelocking_strategy::handle_end");
  if (done)
    DBUG_RETURN(0);

  LEX *lex= thd->lex;
  SELECT_LEX *select_lex= lex->first_select_lex();
  TABLE_LIST *table_list= lex->query_tables, *tl;

  done= true;

  if (mysql_handle_derived(lex, DT_INIT) ||
      mysql_handle_derived(lex, DT_MERGE_FOR_INSERT) ||
      mysql_handle_derived(lex, DT_PREPARE))
    DBUG_RETURN(1);

  /*
    setup_tables() need for VIEWs. JOIN::prepare() will call setup_tables()
    second time, but this call will do nothing (there are check for second
    call in setup_tables()).
  */

  if (setup_tables_and_check_access(thd, &select_lex->context,
      &select_lex->top_join_list, table_list, select_lex->leaf_tables,
      FALSE, UPDATE_ACL, SELECT_ACL, TRUE))
    DBUG_RETURN(1);

  List<Item> *fields= &lex->first_select_lex()->item_list;
  if (setup_fields_with_no_wrap(thd, Ref_ptr_array(),
                                *fields, MARK_COLUMNS_WRITE, 0, 0))
    DBUG_RETURN(1);

  // Check if we have a view in the list ...
  for (tl= table_list; tl ; tl= tl->next_local)
    if (tl->view)
      break;
  // ... and pass this knowlage in check_fields call
  if (check_fields(thd, table_list, *fields, tl != NULL ))
    DBUG_RETURN(1);

  table_map tables_for_update= thd->table_map_for_update= get_table_map(fields);

  if (unsafe_key_update(select_lex->leaf_tables, tables_for_update))
    DBUG_RETURN(1);

  /*
    Setup timestamp handling and locking mode
  */
  List_iterator<TABLE_LIST> ti(lex->first_select_lex()->leaf_tables);
  const bool using_lock_tables= thd->locked_tables_mode != LTM_NONE;
  while ((tl= ti++))
  {
    TABLE *table= tl->table;

    if (tl->is_jtbm())
      continue;

    /* if table will be updated then check that it is unique */
    if (table->map & tables_for_update)
    {
      if (!tl->single_table_updatable() || check_key_in_view(thd, tl))
      {
        my_error(ER_NON_UPDATABLE_TABLE, MYF(0),
                 tl->top_table()->alias.str, "UPDATE");
        DBUG_RETURN(1);
      }

      DBUG_PRINT("info",("setting table `%s` for update",
                         tl->top_table()->alias.str));
      /*
        If table will be updated we should not downgrade lock for it and
        leave it as is.
      */
      tl->updating= 1;
      if (tl->belong_to_view)
        tl->belong_to_view->updating= 1;
      if (extend_table_list(thd, tl, this, has_prelocking_list))
        DBUG_RETURN(1);
    }
    else
    {
      DBUG_PRINT("info",("setting table `%s` for read-only", tl->alias.str));
      /*
        If we are using the binary log, we need TL_READ_NO_INSERT to get
        correct order of statements. Otherwise, we use a TL_READ lock to
        improve performance.
        We don't downgrade metadata lock from SW to SR in this case as
        there is no guarantee that the same ticket is not used by
        another table instance used by this statement which is going to
        be write-locked (for example, trigger to be invoked might try
        to update this table).
        Last argument routine_modifies_data for read_lock_type_for_table()
        is ignored, as prelocking placeholder will never be set here.
      */
      DBUG_ASSERT(tl->prelocking_placeholder == false);
      thr_lock_type lock_type= read_lock_type_for_table(thd, lex, tl, true);
      if (using_lock_tables)
        tl->lock_type= lock_type;
      else
        tl->set_lock_type(thd, lock_type);
    }
  }

  /*
    Check access privileges for tables being updated or read.
    Note that unlike in the above loop we need to iterate here not only
    through all leaf tables but also through all view hierarchy.
  */

  for (tl= table_list; tl; tl= tl->next_local)
  {
    bool not_used= false;
    if (tl->is_jtbm())
      continue;
    if (multi_update_check_table_access(thd, tl, tables_for_update, &not_used))
      DBUG_RETURN(TRUE);
  }

  /* check single table update for view compound from several tables */
  for (tl= table_list; tl; tl= tl->next_local)
  {
    TABLE_LIST *for_update= 0;
    if (tl->is_jtbm())
      continue;
    if (tl->is_merged_derived() &&
        tl->check_single_table(&for_update, tables_for_update, tl))
    {
      my_error(ER_VIEW_MULTIUPDATE, MYF(0), tl->view_db.str, tl->view_name.str);
      DBUG_RETURN(1);
    }
  }

  DBUG_RETURN(0);
}

/*
  make update specific preparation and checks after opening tables

  SYNOPSIS
    mysql_multi_update_prepare()
    thd         thread handler

  RETURN
    FALSE OK
    TRUE  Error
*/

int mysql_multi_update_prepare(THD *thd)
{
  LEX *lex= thd->lex;
  TABLE_LIST *table_list= lex->query_tables;
  TABLE_LIST *tl;
  Multiupdate_prelocking_strategy prelocking_strategy;
  uint table_count= lex->table_count;
  DBUG_ENTER("mysql_multi_update_prepare");

  /*
    Open tables and create derived ones, but do not lock and fill them yet.

    During prepare phase acquire only S metadata locks instead of SW locks to
    keep prepare of multi-UPDATE compatible with concurrent LOCK TABLES WRITE
    and global read lock.

    Don't evaluate any subqueries even if constant, because
    tables aren't locked yet.
  */
  lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_DERIVED;
  if (thd->lex->sql_command == SQLCOM_UPDATE_MULTI)
  {
    if (open_tables(thd, &table_list, &table_count,
        thd->stmt_arena->is_stmt_prepare() ? MYSQL_OPEN_FORCE_SHARED_MDL : 0,
        &prelocking_strategy))
      DBUG_RETURN(TRUE);
  }
  else
  {
    /* following need for prepared statements, to run next time multi-update */
    thd->lex->sql_command= SQLCOM_UPDATE_MULTI;
    prelocking_strategy.reset(thd);
    if (prelocking_strategy.handle_end(thd))
      DBUG_RETURN(TRUE);
  }

  /* now lock and fill tables */
  if (!thd->stmt_arena->is_stmt_prepare() &&
      lock_tables(thd, table_list, table_count, 0))
    DBUG_RETURN(TRUE);

  lex->context_analysis_only&= ~CONTEXT_ANALYSIS_ONLY_DERIVED;

  (void) read_statistics_for_tables_if_needed(thd, table_list);
  /* @todo: downgrade the metadata locks here. */

  /*
    Check that we are not using table that we are updating, but we should
    skip all tables of UPDATE SELECT itself
  */
  lex->first_select_lex()->exclude_from_table_unique_test= TRUE;
  /* We only need SELECT privilege for columns in the values list */
  List_iterator<TABLE_LIST> ti(lex->first_select_lex()->leaf_tables);
  while ((tl= ti++))
  {
    if (tl->is_jtbm())
      continue;
    TABLE *table= tl->table;
    TABLE_LIST *tlist;
    if (!(tlist= tl->top_table())->derived)
    {
      tlist->grant.want_privilege=
        (SELECT_ACL & ~tlist->grant.privilege);
      table->grant.want_privilege= (SELECT_ACL & ~table->grant.privilege);
    }
    DBUG_PRINT("info", ("table: %s  want_privilege: %llx", tl->alias.str,
                        (longlong) table->grant.want_privilege));
  }
  /*
    Set exclude_from_table_unique_test value back to FALSE. It is needed for
    further check in multi_update::prepare whether to use record cache.
  */
  lex->first_select_lex()->exclude_from_table_unique_test= FALSE;

  if (lex->save_prep_leaf_tables())
    DBUG_RETURN(TRUE);
 
  DBUG_RETURN (FALSE);
}


/*
  Setup multi-update handling and call SELECT to do the join
*/

bool mysql_multi_update(THD *thd, TABLE_LIST *table_list, List<Item> *fields,
                        List<Item> *values, COND *conds, ulonglong options,
                        enum enum_duplicates handle_duplicates,
                        bool ignore, SELECT_LEX_UNIT *unit,
                        SELECT_LEX *select_lex, multi_update **result)
{
  bool res;
  DBUG_ENTER("mysql_multi_update");

  if (!(*result= new (thd->mem_root) multi_update(thd, table_list,
                                 &thd->lex->first_select_lex()->leaf_tables,
                                 fields, values, handle_duplicates, ignore)))
  {
    DBUG_RETURN(TRUE);
  }

  if ((*result)->init(thd))
    DBUG_RETURN(1);

  thd->abort_on_warning= !ignore && thd->is_strict_mode();
  List<Item> total_list;

  if (setup_tables(thd, &select_lex->context, &select_lex->top_join_list,
                   table_list, select_lex->leaf_tables, FALSE, FALSE))
    DBUG_RETURN(1);

  if (select_lex->vers_setup_conds(thd, table_list))
    DBUG_RETURN(1);

  res= mysql_select(thd,
                    table_list, total_list, conds,
                    select_lex->order_list.elements,
                    select_lex->order_list.first, NULL, NULL, NULL,
                    options | SELECT_NO_JOIN_CACHE | SELECT_NO_UNLOCK |
                    OPTION_SETUP_TABLES_DONE,
                    *result, unit, select_lex);

  DBUG_PRINT("info",("res: %d  report_error: %d", res, (int) thd->is_error()));
  res|= thd->is_error();
  if (unlikely(res))
    (*result)->abort_result_set();
  else
  {
    if (thd->lex->describe || thd->lex->analyze_stmt)
      res= thd->lex->explain->send_explain(thd);
  }
  thd->abort_on_warning= 0;
  DBUG_RETURN(res);
}


multi_update::multi_update(THD *thd_arg, TABLE_LIST *table_list,
                           List<TABLE_LIST> *leaves_list,
			   List<Item> *field_list, List<Item> *value_list,
			   enum enum_duplicates handle_duplicates_arg,
                           bool ignore_arg):
   select_result_interceptor(thd_arg),
   all_tables(table_list), leaves(leaves_list), update_tables(0),
   tmp_tables(0), updated(0), found(0), fields(field_list),
   values(value_list), table_count(0), copy_field(0),
   handle_duplicates(handle_duplicates_arg), do_update(1), trans_safe(1),
   transactional_tables(0), ignore(ignore_arg), error_handled(0), prepared(0),
   updated_sys_ver(0)
{
}


bool multi_update::init(THD *thd)
{
  table_map tables_to_update= get_table_map(fields);
  List_iterator_fast<TABLE_LIST> li(*leaves);
  TABLE_LIST *tbl;
  while ((tbl =li++))
  {
    if (tbl->is_jtbm())
      continue;
    if (!(tbl->table->map & tables_to_update))
      continue;
    if (updated_leaves.push_back(tbl, thd->mem_root))
      return true;
  }
  return false;
}


/*
  Connect fields with tables and create list of tables that are updated
*/

int multi_update::prepare(List<Item> &not_used_values,
			  SELECT_LEX_UNIT *lex_unit)

{
  TABLE_LIST *table_ref;
  SQL_I_List<TABLE_LIST> update;
  table_map tables_to_update;
  Item_field *item;
  List_iterator_fast<Item> field_it(*fields);
  List_iterator_fast<Item> value_it(*values);
  uint i, max_fields;
  uint leaf_table_count= 0;
  List_iterator<TABLE_LIST> ti(updated_leaves);
  DBUG_ENTER("multi_update::prepare");

  if (prepared)
    DBUG_RETURN(0);
  prepared= true;

  thd->count_cuted_fields= CHECK_FIELD_WARN;
  thd->cuted_fields=0L;
  THD_STAGE_INFO(thd, stage_updating_main_table);

  tables_to_update= get_table_map(fields);

  if (!tables_to_update)
  {
    my_message(ER_NO_TABLES_USED, ER_THD(thd, ER_NO_TABLES_USED), MYF(0));
    DBUG_RETURN(1);
  }

  /*
    We gather the set of columns read during evaluation of SET expression in
    TABLE::tmp_set by pointing TABLE::read_set to it and then restore it after
    setup_fields().
  */
  while ((table_ref= ti++))
  {
    if (table_ref->is_jtbm())
      continue;

    TABLE *table= table_ref->table;
    if (tables_to_update & table->map)
    {
      DBUG_ASSERT(table->read_set == &table->def_read_set);
      table->read_set= &table->tmp_set;
      bitmap_clear_all(table->read_set);
    }
  }

  /*
    We have to check values after setup_tables to get covering_keys right in
    reference tables
  */

  int error= setup_fields(thd, Ref_ptr_array(),
                          *values, MARK_COLUMNS_READ, 0, NULL, 0);

  ti.rewind();
  while ((table_ref= ti++))
  {
    if (table_ref->is_jtbm())
      continue;

    TABLE *table= table_ref->table;
    if (tables_to_update & table->map)
    {
      table->read_set= &table->def_read_set;
      bitmap_union(table->read_set, &table->tmp_set);
      table->file->prepare_for_insert(1);
    }
  }
  if (unlikely(error))
    DBUG_RETURN(1);    

  /*
    Save tables being updated in update_tables
    update_table->shared is position for table
    Don't use key read on tables that are updated
  */

  update.empty();
  ti.rewind();
  while ((table_ref= ti++))
  {
    /* TODO: add support of view of join support */
    if (table_ref->is_jtbm())
      continue;
    TABLE *table=table_ref->table;
    leaf_table_count++;
    if (tables_to_update & table->map)
    {
      TABLE_LIST *tl= (TABLE_LIST*) thd->memdup(table_ref,
						sizeof(*tl));
      if (!tl)
	DBUG_RETURN(1);
      update.link_in_list(tl, &tl->next_local);
      tl->shared= table_count++;
      table->no_keyread=1;
      table->covering_keys.clear_all();
      table->pos_in_table_list= tl;
      table->prepare_triggers_for_update_stmt_or_event();
      table->reset_default_fields();
    }
  }

  table_count=  update.elements;
  update_tables= update.first;

  tmp_tables = (TABLE**) thd->calloc(sizeof(TABLE *) * table_count);
  tmp_table_param = (TMP_TABLE_PARAM*) thd->calloc(sizeof(TMP_TABLE_PARAM) *
						   table_count);
  fields_for_table= (List_item **) thd->alloc(sizeof(List_item *) *
					      table_count);
  values_for_table= (List_item **) thd->alloc(sizeof(List_item *) *
					      table_count);
  if (unlikely(thd->is_fatal_error))
    DBUG_RETURN(1);
  for (i=0 ; i < table_count ; i++)
  {
    fields_for_table[i]= new List_item;
    values_for_table[i]= new List_item;
  }
  if (unlikely(thd->is_fatal_error))
    DBUG_RETURN(1);

  /* Split fields into fields_for_table[] and values_by_table[] */

  while ((item= (Item_field *) field_it++))
  {
    Item *value= value_it++;
    uint offset= item->field->table->pos_in_table_list->shared;
    fields_for_table[offset]->push_back(item, thd->mem_root);
    values_for_table[offset]->push_back(value, thd->mem_root);
  }
  if (unlikely(thd->is_fatal_error))
    DBUG_RETURN(1);

  /* Allocate copy fields */
  max_fields=0;
  for (i=0 ; i < table_count ; i++)
  {
    set_if_bigger(max_fields, fields_for_table[i]->elements + leaf_table_count);
    if (fields_for_table[i]->elements)
    {
      TABLE *table= ((Item_field*)(fields_for_table[i]->head()))->field->table;
      switch_to_nullable_trigger_fields(*fields_for_table[i], table);
      switch_to_nullable_trigger_fields(*values_for_table[i], table);
    }
  }
  copy_field= new (thd->mem_root) Copy_field[max_fields];
  DBUG_RETURN(thd->is_fatal_error != 0);
}

void multi_update::update_used_tables()
{
  Item *item;
  List_iterator_fast<Item> it(*values);
  while ((item= it++))
  {
    item->update_used_tables();
  }
}

void multi_update::prepare_to_read_rows()
{
  /*
    update column maps now. it cannot be done in ::prepare() before the
    optimizer, because the optimize might reset them (in
    SELECT_LEX::update_used_tables()), it cannot be done in
    ::initialize_tables() after the optimizer, because the optimizer
    might read rows from const tables
  */

  for (TABLE_LIST *tl= update_tables; tl; tl= tl->next_local)
    tl->table->mark_columns_needed_for_update();
}


/*
  Check if table is safe to update on fly

  SYNOPSIS
    safe_update_on_fly()
    thd                 Thread handler
    join_tab            How table is used in join
    all_tables          List of tables

  NOTES
    We can update the first table in join on the fly if we know that
    a row in this table will never be read twice. This is true under
    the following conditions:

    - No column is both written to and read in SET expressions.

    - We are doing a table scan and the data is in a separate file (MyISAM) or
      if we don't update a clustered key.

    - We are doing a range scan and we don't update the scan key or
      the primary key for a clustered table handler.

    - Table is not joined to itself.

    This function gets information about fields to be updated from
    the TABLE::write_set bitmap.

  WARNING
    This code is a bit dependent of how make_join_readinfo() works.

    The field table->tmp_set is used for keeping track of which fields are
    read during evaluation of the SET expression. See multi_update::prepare.

  RETURN
    0		Not safe to update
    1		Safe to update
*/

static bool safe_update_on_fly(THD *thd, JOIN_TAB *join_tab,
                               TABLE_LIST *table_ref, TABLE_LIST *all_tables)
{
  TABLE *table= join_tab->table;
  if (unique_table(thd, table_ref, all_tables, 0))
    return 0;
  if (join_tab->join->order) // FIXME this is probably too strong
    return 0;
  switch (join_tab->type) {
  case JT_SYSTEM:
  case JT_CONST:
  case JT_EQ_REF:
    return TRUE;				// At most one matching row
  case JT_REF:
  case JT_REF_OR_NULL:
    return !is_key_used(table, join_tab->ref.key, table->write_set);
  case JT_ALL:
    if (bitmap_is_overlapping(&table->tmp_set, table->write_set))
      return FALSE;
    /* If range search on index */
    if (join_tab->quick)
      return !join_tab->quick->is_keys_used(table->write_set);
    /* If scanning in clustered key */
    if ((table->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX) &&
	table->s->primary_key < MAX_KEY)
      return !is_key_used(table, table->s->primary_key, table->write_set);
    return TRUE;
  default:
    break;					// Avoid compiler warning
  }
  return FALSE;

}


/*
  Initialize table for multi table

  IMPLEMENTATION
    - Update first table in join on the fly, if possible
    - Create temporary tables to store changed values for all other tables
      that are updated (and main_table if the above doesn't hold).
*/

bool
multi_update::initialize_tables(JOIN *join)
{
  TABLE_LIST *table_ref;
  DBUG_ENTER("initialize_tables");

  if (unlikely((thd->variables.option_bits & OPTION_SAFE_UPDATES) &&
               error_if_full_join(join)))
    DBUG_RETURN(1);
  main_table=join->join_tab->table;
  table_to_update= 0;

  /* Any update has at least one pair (field, value) */
  DBUG_ASSERT(fields->elements);
  /*
   Only one table may be modified by UPDATE of an updatable view.
   For an updatable view first_table_for_update indicates this
   table.
   For a regular multi-update it refers to some updated table.
  */ 
  TABLE *first_table_for_update= ((Item_field *) fields->head())->field->table;

  /* Create a temporary table for keys to all tables, except main table */
  for (table_ref= update_tables; table_ref; table_ref= table_ref->next_local)
  {
    TABLE *table=table_ref->table;
    uint cnt= table_ref->shared;
    List<Item> temp_fields;
    ORDER     group;
    TMP_TABLE_PARAM *tmp_param;

    if (ignore)
      table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
    if (table == main_table)			// First table in join
    {
      if (safe_update_on_fly(thd, join->join_tab, table_ref, all_tables))
      {
	table_to_update= table;			// Update table on the fly
        has_vers_fields= table->vers_check_update(*fields);
	continue;
      }
    }
    table->prepare_for_position();
    join->map2table[table->tablenr]->keep_current_rowid= true;

    /*
      enable uncacheable flag if we update a view with check option
      and check option has a subselect, otherwise, the check option
      can be evaluated after the subselect was freed as independent
      (See full_local in JOIN::join_free()).
    */
    if (table_ref->check_option && !join->select_lex->uncacheable)
    {
      SELECT_LEX_UNIT *tmp_unit;
      SELECT_LEX *sl;
      for (tmp_unit= join->select_lex->first_inner_unit();
           tmp_unit;
           tmp_unit= tmp_unit->next_unit())
      {
        for (sl= tmp_unit->first_select(); sl; sl= sl->next_select())
        {
          if (sl->master_unit()->item)
          {
            join->select_lex->uncacheable|= UNCACHEABLE_CHECKOPTION;
            goto loop_end;
          }
        }
      }
    }
loop_end:

    if (table == first_table_for_update && table_ref->check_option)
    {
      table_map unupdated_tables= table_ref->check_option->used_tables() &
                                  ~first_table_for_update->map;
      List_iterator<TABLE_LIST> ti(*leaves);
      TABLE_LIST *tbl_ref;
      while ((tbl_ref= ti++) && unupdated_tables)
      {
        if (unupdated_tables & tbl_ref->table->map)
          unupdated_tables&= ~tbl_ref->table->map;
        else
          continue;
        if (unupdated_check_opt_tables.push_back(tbl_ref->table))
          DBUG_RETURN(1);
      }
    }

    tmp_param= tmp_table_param+cnt;

    /*
      Create a temporary table to store all fields that are changed for this
      table. The first field in the temporary table is a pointer to the
      original row so that we can find and update it. For the updatable
      VIEW a few following fields are rowids of tables used in the CHECK
      OPTION condition.
    */

    List_iterator_fast<TABLE> tbl_it(unupdated_check_opt_tables);
    TABLE *tbl= table;
    do
    {
      LEX_CSTRING field_name;
      field_name.str= tbl->alias.c_ptr();
      field_name.length= strlen(field_name.str);
      /*
        Signal each table (including tables referenced by WITH CHECK OPTION
        clause) for which we will store row position in the temporary table
        that we need a position to be read first.
      */
      tbl->prepare_for_position();
      join->map2table[tbl->tablenr]->keep_current_rowid= true;

      Item_temptable_rowid *item=
        new (thd->mem_root) Item_temptable_rowid(tbl);
      if (!item)
         DBUG_RETURN(1);
      item->fix_fields(thd, 0);
      if (temp_fields.push_back(item, thd->mem_root))
        DBUG_RETURN(1);
    } while ((tbl= tbl_it++));

    temp_fields.append(fields_for_table[cnt]);

    /* Make an unique key over the first field to avoid duplicated updates */
    bzero((char*) &group, sizeof(group));
    group.direction= ORDER::ORDER_ASC;
    group.item= (Item**) temp_fields.head_ref();

    tmp_param->quick_group= 1;
    tmp_param->field_count= temp_fields.elements;
    tmp_param->func_count=  temp_fields.elements - 1;
    calc_group_buffer(tmp_param, &group);
    /* small table, ignore @@big_tables */
    my_bool save_big_tables= thd->variables.big_tables; 
    thd->variables.big_tables= FALSE;
    tmp_tables[cnt]=create_tmp_table(thd, tmp_param, temp_fields,
                                     (ORDER*) &group, 0, 0,
                                     TMP_TABLE_ALL_COLUMNS, HA_POS_ERROR, &empty_clex_str);
    thd->variables.big_tables= save_big_tables;
    if (!tmp_tables[cnt])
      DBUG_RETURN(1);
    tmp_tables[cnt]->file->extra(HA_EXTRA_WRITE_CACHE);
  }
  join->tmp_table_keep_current_rowid= TRUE;
  DBUG_RETURN(0);
}


static TABLE *item_rowid_table(Item *item)
{
  if (item->type() != Item::FUNC_ITEM)
    return NULL;
  Item_func *func= (Item_func *)item;
  if (func->functype() != Item_func::TEMPTABLE_ROWID)
    return NULL;
  Item_temptable_rowid *itr= (Item_temptable_rowid *)func;
  return itr->table;
}


/*
  multi_update stores a rowid and new field values for every updated row in a
  temporary table (one temporary table per updated table).  These rowids are
  obtained via Item_temptable_rowid's by calling handler::position().  But if
  the join is resolved via a temp table, rowids cannot be obtained from
  handler::position() in the multi_update::send_data().  So, they're stored in
  the join's temp table (JOIN::add_fields_for_current_rowid()) and here we
  replace Item_temptable_rowid's (that would've done handler::position()) with
  Item_field's (that will simply take the corresponding field value from the
  temp table).
*/
int multi_update::prepare2(JOIN *join)
{
  if (!join->need_tmp || !join->tmp_table_keep_current_rowid)
    return 0;

  // there cannot be many tmp tables in multi-update
  JOIN_TAB *tmptab= join->join_tab + join->exec_join_tab_cnt();

  for (Item **it= tmptab->tmp_table_param->items_to_copy; *it ; it++)
  {
    TABLE *tbl= item_rowid_table(*it);
    if (!tbl)
      continue;
    for (uint i= 0; i < table_count; i++)
    {
      for (Item **it2= tmp_table_param[i].items_to_copy; *it2; it2++)
      {
        if (item_rowid_table(*it2) != tbl)
          continue;
        Item_field *fld= new (thd->mem_root)
                             Item_field(thd, (*it)->get_tmp_table_field());
        if (!fld)
          return 1;
        fld->result_field= (*it2)->get_tmp_table_field();
        *it2= fld;
      }
    }
  }
  return 0;
}


multi_update::~multi_update()
{
  TABLE_LIST *table;
  for (table= update_tables ; table; table= table->next_local)
  {
    table->table->no_keyread= 0;
    if (ignore)
      table->table->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);
  }

  if (tmp_tables)
  {
    for (uint cnt = 0; cnt < table_count; cnt++)
    {
      if (tmp_tables[cnt])
      {
	free_tmp_table(thd, tmp_tables[cnt]);
	tmp_table_param[cnt].cleanup();
      }
    }
  }
  if (copy_field)
    delete [] copy_field;
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;		// Restore this setting
  DBUG_ASSERT(trans_safe || !updated || 
              thd->transaction->all.modified_non_trans_table);
}


int multi_update::send_data(List<Item> &not_used_values)
{
  TABLE_LIST *cur_table;
  DBUG_ENTER("multi_update::send_data");

  for (cur_table= update_tables; cur_table; cur_table= cur_table->next_local)
  {
    int error= 0;
    TABLE *table= cur_table->table;
    uint offset= cur_table->shared;
    /*
      Check if we are using outer join and we didn't find the row
      or if we have already updated this row in the previous call to this
      function.

      The same row may be presented here several times in a join of type
      UPDATE t1 FROM t1,t2 SET t1.a=t2.a

      In this case we will do the update for the first found row combination.
      The join algorithm guarantees that we will not find the a row in
      t1 several times.
    */
    if (table->status & (STATUS_NULL_ROW | STATUS_UPDATED))
      continue;

    if (table == table_to_update)
    {
      /*
        We can use compare_record() to optimize away updates if
        the table handler is returning all columns OR if
        if all updated columns are read
      */
      bool can_compare_record;
      can_compare_record= records_are_comparable(table);

      table->status|= STATUS_UPDATED;
      store_record(table,record[1]);

      if (fill_record_n_invoke_before_triggers(thd, table,
                                               *fields_for_table[offset],
                                               *values_for_table[offset], 0,
                                               TRG_EVENT_UPDATE))
	DBUG_RETURN(1);
      /*
        Reset the table->auto_increment_field_not_null as it is valid for
        only one row.
      */
      table->auto_increment_field_not_null= FALSE;
      found++;
      if (!can_compare_record || compare_record(table))
      {

        if ((error= cur_table->view_check_option(thd, ignore)) !=
            VIEW_CHECK_OK)
        {
          found--;
          if (error == VIEW_CHECK_SKIP)
            continue;
          else if (unlikely(error == VIEW_CHECK_ERROR))
            DBUG_RETURN(1);
        }
        if (unlikely(!updated++))
        {
          /*
            Inform the main table that we are going to update the table even
            while we may be scanning it.  This will flush the read cache
            if it's used.
          */
          main_table->file->extra(HA_EXTRA_PREPARE_FOR_UPDATE);
        }
        if (unlikely((error=table->file->ha_update_row(table->record[1],
                                                       table->record[0]))) &&
            error != HA_ERR_RECORD_IS_THE_SAME)
        {
          updated--;
          if (!ignore ||
              table->file->is_fatal_error(error, HA_CHECK_ALL))
            goto error;
        }
        else
        {
          if (unlikely(error == HA_ERR_RECORD_IS_THE_SAME))
          {
            error= 0;
            updated--;
          }
          else if (has_vers_fields && table->versioned(VERS_TRX_ID))
          {
            updated_sys_ver++;
          }
          /* non-transactional or transactional table got modified   */
          /* either multi_update class' flag is raised in its branch */
          if (table->file->has_transactions_and_rollback())
            transactional_tables= TRUE;
          else
          {
            trans_safe= FALSE;
            thd->transaction->stmt.modified_non_trans_table= TRUE;
          }
        }
      }
      if (has_vers_fields && table->versioned(VERS_TIMESTAMP))
      {
        store_record(table, record[2]);
        if (unlikely(error= vers_insert_history_row(table)))
        {
          restore_record(table, record[2]);
          goto error;
        }
        restore_record(table, record[2]);
        updated_sys_ver++;
      }
      if (table->triggers &&
          unlikely(table->triggers->process_triggers(thd, TRG_EVENT_UPDATE,
                                                     TRG_ACTION_AFTER, TRUE)))
        DBUG_RETURN(1);
    }
    else
    {
      TABLE *tmp_table= tmp_tables[offset];
      if (copy_funcs(tmp_table_param[offset].items_to_copy, thd))
        DBUG_RETURN(1);
      /* rowid field is NULL if join tmp table has null row from outer join */
      if (tmp_table->field[0]->is_null())
        continue;
      /* Store regular updated fields in the row. */
      DBUG_ASSERT(1 + unupdated_check_opt_tables.elements ==
                  tmp_table_param[offset].func_count);
      fill_record(thd, tmp_table,
                  tmp_table->field + 1 + unupdated_check_opt_tables.elements,
                  *values_for_table[offset], TRUE, FALSE);

      /* Write row, ignoring duplicated updates to a row */
      error= tmp_table->file->ha_write_tmp_row(tmp_table->record[0]);
      found++;
      if (unlikely(error))
      {
        found--;
        if (error != HA_ERR_FOUND_DUPP_KEY &&
            error != HA_ERR_FOUND_DUPP_UNIQUE)
        {
          if (create_internal_tmp_table_from_heap(thd, tmp_table,
                                                  tmp_table_param[offset].start_recinfo,
                                                  &tmp_table_param[offset].recinfo,
                                                  error, 1, NULL))
          {
            do_update= 0;
            DBUG_RETURN(1);			// Not a table_is_full error
          }
          found++;
        }
      }
    }
    continue;
error:
    DBUG_ASSERT(error > 0);
    /*
      If (ignore && error == is ignorable) we don't have to
      do anything; otherwise...
    */
    myf flags= 0;

    if (table->file->is_fatal_error(error, HA_CHECK_ALL))
      flags|= ME_FATAL; /* Other handler errors are fatal */

    prepare_record_for_error_message(error, table);
    table->file->print_error(error,MYF(flags));
    DBUG_RETURN(1);
  } // for (cur_table)
  DBUG_RETURN(0);
}


void multi_update::abort_result_set()
{
  /* the error was handled or nothing deleted and no side effects return */
  if (unlikely(error_handled ||
               (!thd->transaction->stmt.modified_non_trans_table && !updated)))
    return;

  /* Something already updated so we have to invalidate cache */
  if (updated)
    query_cache_invalidate3(thd, update_tables, 1);
  /*
    If all tables that has been updated are trans safe then just do rollback.
    If not attempt to do remaining updates.
  */

  if (! trans_safe)
  {
    DBUG_ASSERT(thd->transaction->stmt.modified_non_trans_table);
    if (do_update && table_count > 1)
    {
      /* Add warning here */
      (void) do_updates();
    }
  }
  if (thd->transaction->stmt.modified_non_trans_table)
  {
    /*
      The query has to binlog because there's a modified non-transactional table
      either from the query's list or via a stored routine: bug#13270,23333
    */
    if (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      /*
        THD::killed status might not have been set ON at time of an error
        got caught and if happens later the killed error is written
        into repl event.
      */
      int errcode= query_error_code(thd, thd->killed == NOT_KILLED);
      /* the error of binary logging is ignored */
      (void)thd->binlog_query(THD::ROW_QUERY_TYPE,
                        thd->query(), thd->query_length(),
                        transactional_tables, FALSE, FALSE, errcode);
    }
    thd->transaction->all.modified_non_trans_table= TRUE;
  }
  thd->transaction->all.m_unsafe_rollback_flags|=
    (thd->transaction->stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);
  DBUG_ASSERT(trans_safe || !updated || thd->transaction->stmt.modified_non_trans_table);
}


int multi_update::do_updates()
{
  TABLE_LIST *cur_table;
  int local_error= 0;
  ha_rows org_updated;
  TABLE *table, *tmp_table, *err_table;
  List_iterator_fast<TABLE> check_opt_it(unupdated_check_opt_tables);
  DBUG_ENTER("multi_update::do_updates");

  do_update= 0;					// Don't retry this function
  if (!found)
    DBUG_RETURN(0);

  /*
    Update read_set to include all fields that virtual columns may depend on.
    Usually they're already in the read_set, but if the previous access
    method was keyread, only the virtual column itself will be in read_set,
    not its dependencies
  */
  while(TABLE *tbl= check_opt_it++)
    if (Field **vf= tbl->vfield)
      for (; *vf; vf++)
        if (bitmap_is_set(tbl->read_set, (*vf)->field_index))
          (*vf)->vcol_info->expr->walk(&Item::register_field_in_read_map, 1, 0);

  for (cur_table= update_tables; cur_table; cur_table= cur_table->next_local)
  {
    bool can_compare_record;
    uint offset= cur_table->shared;

    table = cur_table->table;
    if (table == table_to_update)
      continue;					// Already updated
    org_updated= updated;
    tmp_table= tmp_tables[cur_table->shared];
    tmp_table->file->extra(HA_EXTRA_CACHE);	// Change to read cache
    if (unlikely((local_error= table->file->ha_rnd_init(0))))
    {
      err_table= table;
      goto err;
    }
    table->file->extra(HA_EXTRA_NO_CACHE);
    /*
      We have to clear the base record, if we have virtual indexed
      blob fields, as some storage engines will access the blob fields
      to calculate the keys to see if they have changed. Without
      clearing the blob pointers will contain random values which can
      cause a crash.
      This is a workaround for engines that access columns not present in
      either read or write set.
    */
    if (table->vfield)
      empty_record(table);

    has_vers_fields= table->vers_check_update(*fields);

    check_opt_it.rewind();
    while(TABLE *tbl= check_opt_it++)
    {
      if (unlikely((local_error= tbl->file->ha_rnd_init(0))))
      {
        err_table= tbl;
        goto err;
      }
      tbl->file->extra(HA_EXTRA_CACHE);
    }

    /*
      Setup copy functions to copy fields from temporary table
    */
    List_iterator_fast<Item> field_it(*fields_for_table[offset]);
    Field **field;
    Copy_field *copy_field_ptr= copy_field, *copy_field_end;

    /* Skip row pointers */
    field= tmp_table->field + 1 + unupdated_check_opt_tables.elements;
    for ( ; *field ; field++)
    {
      Item_field *item= (Item_field* ) field_it++;
      (copy_field_ptr++)->set(item->field, *field, 0);
    }
    copy_field_end=copy_field_ptr;

    if (unlikely((local_error= tmp_table->file->ha_rnd_init(1))))
    {
      err_table= tmp_table;
      goto err;
    }

    can_compare_record= records_are_comparable(table);

    for (;;)
    {
      if (thd->killed && trans_safe)
      {
        thd->fatal_error();
	goto err2;
      }
      if (unlikely((local_error=
                    tmp_table->file->ha_rnd_next(tmp_table->record[0]))))
      {
	if (local_error == HA_ERR_END_OF_FILE)
	  break;
        err_table= tmp_table;
	goto err;
      }

      /* call rnd_pos() using rowids from temporary table */
      check_opt_it.rewind();
      TABLE *tbl= table;
      uint field_num= 0;
      do
      {
        DBUG_ASSERT(!tmp_table->field[field_num]->is_null());
        String rowid;
        tmp_table->field[field_num]->val_str(&rowid);
        if (unlikely((local_error= tbl->file->ha_rnd_pos(tbl->record[0],
                                                         (uchar*)rowid.ptr()))))
        {
          err_table= tbl;
          goto err;
        }
        field_num++;
      } while ((tbl= check_opt_it++));

      if (table->vfield &&
          unlikely(table->update_virtual_fields(table->file,
                                                VCOL_UPDATE_INDEXED_FOR_UPDATE)))
        goto err2;

      table->status|= STATUS_UPDATED;
      store_record(table,record[1]);

      /* Copy data from temporary table to current table */
      for (copy_field_ptr=copy_field;
	   copy_field_ptr != copy_field_end;
	   copy_field_ptr++)
      {
	(*copy_field_ptr->do_copy)(copy_field_ptr);
        copy_field_ptr->to_field->set_has_explicit_value();
      }

      table->evaluate_update_default_function();
      if (table->vfield &&
          table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE))
        goto err2;
      if (table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_UPDATE,
                                            TRG_ACTION_BEFORE, TRUE))
        goto err2;

      if (!can_compare_record || compare_record(table))
      {
        int error;
        if ((error= cur_table->view_check_option(thd, ignore)) !=
            VIEW_CHECK_OK)
        {
          if (error == VIEW_CHECK_SKIP)
            continue;
          else if (unlikely(error == VIEW_CHECK_ERROR))
          {
            thd->fatal_error();
            goto err2;
          }
        }
        if (has_vers_fields && table->versioned())
          table->vers_update_fields();

        if (unlikely((local_error=
                      table->file->ha_update_row(table->record[1],
                                                 table->record[0]))) &&
            local_error != HA_ERR_RECORD_IS_THE_SAME)
	{
	  if (!ignore ||
              table->file->is_fatal_error(local_error, HA_CHECK_ALL))
          {
            err_table= table;
            goto err;
          }
        }
        if (local_error != HA_ERR_RECORD_IS_THE_SAME)
        {
          updated++;

          if (has_vers_fields && table->versioned())
          {
            if (table->versioned(VERS_TIMESTAMP))
            {
              store_record(table, record[2]);
              if ((local_error= vers_insert_history_row(table)))
              {
                restore_record(table, record[2]);
                err_table = table;
                goto err;
              }
              restore_record(table, record[2]);
            }
            updated_sys_ver++;
          }
        }
        else
        {
          local_error= 0;
        }
      }

      if (table->triggers &&
          unlikely(table->triggers->process_triggers(thd, TRG_EVENT_UPDATE,
                                                     TRG_ACTION_AFTER, TRUE)))
        goto err2;
    }

    if (updated != org_updated)
    {
      if (table->file->has_transactions_and_rollback())
        transactional_tables= TRUE;
      else
      {
        trans_safe= FALSE;				// Can't do safe rollback
        thd->transaction->stmt.modified_non_trans_table= TRUE;
      }
    }
    (void) table->file->ha_rnd_end();
    (void) tmp_table->file->ha_rnd_end();
    check_opt_it.rewind();
    while (TABLE *tbl= check_opt_it++)
        tbl->file->ha_rnd_end();

  }
  DBUG_RETURN(0);

err:
  {
    prepare_record_for_error_message(local_error, err_table);
    err_table->file->print_error(local_error,MYF(ME_FATAL));
  }

err2:
  if (table->file->inited)
    (void) table->file->ha_rnd_end();
  if (tmp_table->file->inited)
    (void) tmp_table->file->ha_rnd_end();
  check_opt_it.rewind();
  while (TABLE *tbl= check_opt_it++)
  {
    if (tbl->file->inited)
      (void) tbl->file->ha_rnd_end();
  }

  if (updated != org_updated)
  {
    if (table->file->has_transactions_and_rollback())
      transactional_tables= TRUE;
    else
    {
      trans_safe= FALSE;
      thd->transaction->stmt.modified_non_trans_table= TRUE;
    }
  }
  DBUG_RETURN(1);
}


/* out: 1 if error, 0 if success */

bool multi_update::send_eof()
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  ulonglong id;
  killed_state killed_status= NOT_KILLED;
  DBUG_ENTER("multi_update::send_eof");
  THD_STAGE_INFO(thd, stage_updating_reference_tables);

  /* 
     Does updates for the last n - 1 tables, returns 0 if ok;
     error takes into account killed status gained in do_updates()
  */
  int local_error= thd->is_error();
  if (likely(!local_error))
    local_error = (table_count) ? do_updates() : 0;
  /*
    if local_error is not set ON until after do_updates() then
    later carried out killing should not affect binlogging.
  */
  killed_status= (local_error == 0) ? NOT_KILLED : thd->killed;
  THD_STAGE_INFO(thd, stage_end);

  /* We must invalidate the query cache before binlog writing and
  ha_autocommit_... */

  if (updated)
  {
    query_cache_invalidate3(thd, update_tables, 1);
  }
  /*
    Write the SQL statement to the binlog if we updated
    rows and we succeeded or if we updated some non
    transactional tables.
    
    The query has to binlog because there's a modified non-transactional table
    either from the query's list or via a stored routine: bug#13270,23333
  */

  if (thd->transaction->stmt.modified_non_trans_table)
    thd->transaction->all.modified_non_trans_table= TRUE;
  thd->transaction->all.m_unsafe_rollback_flags|=
    (thd->transaction->stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);

  if (likely(local_error == 0 ||
             thd->transaction->stmt.modified_non_trans_table))
  {
    if (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      int errcode= 0;
      if (likely(local_error == 0))
        thd->clear_error();
      else
        errcode= query_error_code(thd, killed_status == NOT_KILLED);

      bool force_stmt= false;
      for (TABLE *table= all_tables->table; table; table= table->next)
      {
        if (table->versioned(VERS_TRX_ID))
        {
          force_stmt= true;
          break;
        }
      }
      enum_binlog_format save_binlog_format;
      save_binlog_format= thd->get_current_stmt_binlog_format();
      if (force_stmt)
        thd->set_current_stmt_binlog_format_stmt();

      if (thd->binlog_query(THD::ROW_QUERY_TYPE, thd->query(),
                            thd->query_length(), transactional_tables, FALSE,
                            FALSE, errcode) > 0)
	local_error= 1;				// Rollback update
      thd->set_current_stmt_binlog_format(save_binlog_format);
    }
  }
  DBUG_ASSERT(trans_safe || !updated ||
              thd->transaction->stmt.modified_non_trans_table);

  if (unlikely(local_error))
  {
    error_handled= TRUE; // to force early leave from ::abort_result_set()
    if (thd->killed == NOT_KILLED && !thd->get_stmt_da()->is_set())
    {
      /*
        No error message was sent and query was not killed (in which case
        mysql_execute_command() will send the error mesage).
      */
      my_message(ER_UNKNOWN_ERROR, "An error occurred in multi-table update",
                 MYF(0));
    }
    DBUG_RETURN(TRUE);
  }

  if (!thd->lex->analyze_stmt)
  {
    id= thd->arg_of_last_insert_id_function ?
    thd->first_successful_insert_id_in_prev_stmt : 0;
    my_snprintf(buff, sizeof(buff), ER_THD(thd, ER_UPDATE_INFO),
                (ulong) found, (ulong) updated, (ulong) thd->cuted_fields);
    ::my_ok(thd, (thd->client_capabilities & CLIENT_FOUND_ROWS) ? found : updated,
            id, buff);
  }
  DBUG_RETURN(FALSE);
}
