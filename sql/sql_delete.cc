/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates.
   Copyright (c) 2010, 2015, MariaDB

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
  Delete of records tables.

  Multi-table deletes were introduced by Monty and Sinisa
*/

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "sql_delete.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"                           // open_temprary_table
#include "lock.h"                              // unlock_table_name
#include "sql_view.h"             // check_key_in_view, mysql_frm_type
#include "sql_parse.h"            // mysql_init_select
#include "sql_acl.h"              // *_ACL
#include "filesort.h"             // filesort
#include "sql_handler.h"          // mysql_ha_rm_tables
#include "sql_select.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_statistics.h"
#include "transaction.h"
#include "records.h"                            // init_read_record,
#include "filesort.h"
#include "uniques.h"
#include "sql_derived.h"                        // mysql_handle_list_of_derived
                                                // end_read_record
#include "sql_partition.h"       // make_used_partitions_str

/*
  @brief
    Print query plan of a single-table DELETE command
  
  @detail
    This function is used by EXPLAIN DELETE and by SHOW EXPLAIN when it is
    invoked on a running DELETE statement.
*/

Explain_delete* Delete_plan::save_explain_delete_data(MEM_ROOT *mem_root, THD *thd)
{
  Explain_query *query= thd->lex->explain;
  Explain_delete *explain= 
     new (mem_root) Explain_delete(mem_root, thd->lex->analyze_stmt);

  if (deleting_all_rows)
  {
    explain->deleting_all_rows= true;
    explain->select_type= "SIMPLE";
    explain->rows= scanned_rows;
  }
  else
  {
    explain->deleting_all_rows= false;
    Update_plan::save_explain_data_intern(mem_root, explain, 
                                          thd->lex->analyze_stmt);
  }
 
  query->add_upd_del_plan(explain);
  return explain;
}


Explain_update* 
Update_plan::save_explain_update_data(MEM_ROOT *mem_root, THD *thd)
{
  Explain_query *query= thd->lex->explain;
  Explain_update* explain= 
    new (mem_root) Explain_update(mem_root, thd->lex->analyze_stmt);
  save_explain_data_intern(mem_root, explain, thd->lex->analyze_stmt);
  query->add_upd_del_plan(explain);
  return explain;
}


void Update_plan::save_explain_data_intern(MEM_ROOT *mem_root,
                                           Explain_update *explain,
                                           bool is_analyze)
{
  explain->select_type= "SIMPLE";
  explain->table_name.append(table->pos_in_table_list->alias);
  
  explain->impossible_where= false;
  explain->no_partitions= false;

  if (impossible_where)
  {
    explain->impossible_where= true;
    return;
  }

  if (no_partitions)
  {
    explain->no_partitions= true;
    return;
  }
  
  if (is_analyze)
    table->file->set_time_tracker(&explain->table_tracker);

  select_lex->set_explain_type(TRUE);
  explain->select_type= select_lex->type;
  /* Partitions */
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    partition_info *part_info;
    if ((part_info= table->part_info))
    {          
      make_used_partitions_str(mem_root, part_info, &explain->used_partitions,
                               explain->used_partitions_list);
      explain->used_partitions_set= true;
    }
    else
      explain->used_partitions_set= false;
#else
    /* just produce empty column if partitioning is not compiled in */
    explain->used_partitions_set= false;
#endif
  }


  /* Set jtype */
  if (select && select->quick)
  {
    int quick_type= select->quick->get_type();
    if ((quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_INTERSECT) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_UNION))
      explain->jtype= JT_INDEX_MERGE;
    else
      explain->jtype= JT_RANGE;
  }
  else
  {
    if (index == MAX_KEY)
      explain->jtype= JT_ALL;
    else
      explain->jtype= JT_NEXT;
  }

  explain->using_where= MY_TEST(select && select->cond);
  explain->where_cond= select? select->cond: NULL;

  if (using_filesort)
    explain->filesort_tracker= new (mem_root) Filesort_tracker(is_analyze);
  explain->using_io_buffer= using_io_buffer;

  append_possible_keys(mem_root, explain->possible_keys, table, 
                       possible_keys);

  explain->quick_info= NULL;

  /* Calculate key_len */
  if (select && select->quick)
  {
    explain->quick_info= select->quick->get_explain(mem_root);
  }
  else
  {
    if (index != MAX_KEY)
    {
      explain->key.set(mem_root, &table->key_info[index],
                       table->key_info[index].key_length);
    }
  }
  explain->rows= scanned_rows;

  if (select && select->quick && 
      select->quick->get_type() == QUICK_SELECT_I::QS_TYPE_RANGE)
  {
    explain_append_mrr_info((QUICK_RANGE_SELECT*)select->quick, 
                            &explain->mrr_type);
  }

  bool skip= updating_a_view;

  /* Save subquery children */
  for (SELECT_LEX_UNIT *unit= select_lex->first_inner_unit();
       unit;
       unit= unit->next_unit())
  {
    if (skip)
    {
      skip= false;
      continue;
    }
    /* 
      Display subqueries only if they are not parts of eliminated WHERE/ON
      clauses.
    */
    if (!(unit->item && unit->item->eliminated))
      explain->add_child(unit->first_select()->select_number);
  }
}


/**
  Implement DELETE SQL word.

  @note Like implementations of other DDL/DML in MySQL, this function
  relies on the caller to close the thread tables. This is done in the
  end of dispatch_command().
*/

bool mysql_delete(THD *thd, TABLE_LIST *table_list, COND *conds,
                  SQL_I_List<ORDER> *order_list, ha_rows limit,
                  ulonglong options, select_result *result)
{
  bool          will_batch;
  int		error, loc_error;
  TABLE		*table;
  SQL_SELECT	*select=0;
  SORT_INFO	*file_sort= 0;
  READ_RECORD	info;
  bool          using_limit=limit != HA_POS_ERROR;
  bool		transactional_table, safe_update, const_cond;
  bool          const_cond_result;
  bool		return_error= 0;
  ha_rows	deleted= 0;
  bool          reverse= FALSE;
  ORDER *order= (ORDER *) ((order_list && order_list->elements) ?
                           order_list->first : NULL);
  SELECT_LEX   *select_lex= &thd->lex->select_lex;
  killed_state killed_status= NOT_KILLED;
  THD::enum_binlog_query_type query_type= THD::ROW_QUERY_TYPE;
  bool with_select= !select_lex->item_list.is_empty();
  Explain_delete *explain;
  Delete_plan query_plan(thd->mem_root);
  query_plan.index= MAX_KEY;
  query_plan.using_filesort= FALSE;
  DBUG_ENTER("mysql_delete");

  create_explain_query(thd->lex, thd->mem_root);
  if (open_and_lock_tables(thd, table_list, TRUE, 0))
    DBUG_RETURN(TRUE);

  if (mysql_handle_list_of_derived(thd->lex, table_list, DT_MERGE_FOR_INSERT))
    DBUG_RETURN(TRUE);
  if (mysql_handle_list_of_derived(thd->lex, table_list, DT_PREPARE))
    DBUG_RETURN(TRUE);

  if (!table_list->single_table_updatable())
  {
     my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias, "DELETE");
     DBUG_RETURN(TRUE);
  }
  if (!(table= table_list->table) || !table->is_created())
  {
      my_error(ER_VIEW_DELETE_MERGE_VIEW, MYF(0),
	       table_list->view_db.str, table_list->view_name.str);
    DBUG_RETURN(TRUE);
  }
  THD_STAGE_INFO(thd, stage_init);
  table->map=1;
  query_plan.select_lex= &thd->lex->select_lex;
  query_plan.table= table;
  query_plan.updating_a_view= MY_TEST(table_list->view);

  if (mysql_prepare_delete(thd, table_list, select_lex->with_wild,
                                            select_lex->item_list, &conds))
    DBUG_RETURN(TRUE);
  
  if (with_select)
    (void) result->prepare(select_lex->item_list, NULL);

  if (thd->lex->current_select->first_cond_optimization)
  {
    thd->lex->current_select->save_leaf_tables(thd);
    thd->lex->current_select->first_cond_optimization= 0;
  }
  /* check ORDER BY even if it can be ignored */
  if (order)
  {
    TABLE_LIST   tables;
    List<Item>   fields;
    List<Item>   all_fields;

    bzero((char*) &tables,sizeof(tables));
    tables.table = table;
    tables.alias = table_list->alias;

      if (select_lex->setup_ref_array(thd, order_list->elements) ||
	  setup_order(thd, select_lex->ref_pointer_array, &tables,
                    fields, all_fields, order))
    {
      free_underlaid_joins(thd, &thd->lex->select_lex);
      DBUG_RETURN(TRUE);
    }
  }

  /* Apply the IN=>EXISTS transformation to all subqueries and optimize them. */
  if (select_lex->optimize_unflattened_subqueries(false))
    DBUG_RETURN(TRUE);

  const_cond= (!conds || conds->const_item());
  safe_update= MY_TEST(thd->variables.option_bits & OPTION_SAFE_UPDATES);
  if (safe_update && const_cond)
  {
    my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
               ER_THD(thd, ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
    DBUG_RETURN(TRUE);
  }

  const_cond_result= const_cond && (!conds || conds->val_int());
  if (thd->is_error())
  {
    /* Error evaluating val_int(). */
    DBUG_RETURN(TRUE);
  }

  /*
    Test if the user wants to delete all rows and deletion doesn't have
    any side-effects (because of triggers), so we can use optimized
    handler::delete_all_rows() method.

    We can use delete_all_rows() if and only if:
    - We allow new functions (not using option --skip-new), and are
      not in safe mode (not using option --safe-mode)
    - There is no limit clause
    - The condition is constant
    - If there is a condition, then it it produces a non-zero value
    - If the current command is DELETE FROM with no where clause, then:
      - We should not be binlogging this statement in row-based, and
      - there should be no delete triggers associated with the table.
  */
  if (!with_select && !using_limit && const_cond_result &&
      (!thd->is_current_stmt_binlog_format_row() &&
       !(table->triggers && table->triggers->has_delete_triggers())))
  {
    /* Update the table->file->stats.records number */
    table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
    ha_rows const maybe_deleted= table->file->stats.records;
    DBUG_PRINT("debug", ("Trying to use delete_all_rows()"));

    query_plan.set_delete_all_rows(maybe_deleted);
    if (thd->lex->describe)
      goto produce_explain_and_leave;

    if (!(error=table->file->ha_delete_all_rows()))
    {
      /*
        If delete_all_rows() is used, it is not possible to log the
        query in row format, so we have to log it in statement format.
      */
      query_type= THD::STMT_QUERY_TYPE;
      error= -1;
      deleted= maybe_deleted;
      query_plan.save_explain_delete_data(thd->mem_root, thd);
      goto cleanup;
    }
    if (error != HA_ERR_WRONG_COMMAND)
    {
      table->file->print_error(error,MYF(0));
      error=0;
      goto cleanup;
    }
    /* Handler didn't support fast delete; Delete rows one by one */
    query_plan.cancel_delete_all_rows();
  }
  if (conds)
  {
    Item::cond_result result;
    conds= conds->remove_eq_conds(thd, &result, true);
    if (result == Item::COND_FALSE)             // Impossible where
    {
      limit= 0;
      query_plan.set_impossible_where();
      if (thd->lex->describe || thd->lex->analyze_stmt)
        goto produce_explain_and_leave;
    }
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (prune_partitions(thd, table, conds))
  {
    free_underlaid_joins(thd, select_lex);

    query_plan.set_no_partitions();
    if (thd->lex->describe || thd->lex->analyze_stmt)
      goto produce_explain_and_leave;

    my_ok(thd, 0);
    DBUG_RETURN(0);
  }
#endif
  /* Update the table->file->stats.records number */
  table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
  set_statistics_for_table(thd, table);

  table->covering_keys.clear_all();
  table->quick_keys.clear_all();		// Can't use 'only index'

  select=make_select(table, 0, 0, conds, (SORT_INFO*) 0, 0, &error);
  if (error)
    DBUG_RETURN(TRUE);
  if ((select && select->check_quick(thd, safe_update, limit)) || !limit)
  {
    query_plan.set_impossible_where();
    if (thd->lex->describe || thd->lex->analyze_stmt)
      goto produce_explain_and_leave;

    delete select;
    free_underlaid_joins(thd, select_lex);
    /* 
      Error was already created by quick select evaluation (check_quick()).
      TODO: Add error code output parameter to Item::val_xxx() methods.
      Currently they rely on the user checking DA for
      errors when unwinding the stack after calling Item::val_xxx().
    */
    if (thd->is_error())
      DBUG_RETURN(TRUE);
    my_ok(thd, 0);
    DBUG_RETURN(0);				// Nothing to delete
  }

  /* If running in safe sql mode, don't allow updates without keys */
  if (table->quick_keys.is_clear_all())
  {
    thd->set_status_no_index_used();
    if (safe_update && !using_limit)
    {
      delete select;
      free_underlaid_joins(thd, select_lex);
      my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
                 ER_THD(thd, ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
      DBUG_RETURN(TRUE);
    }
  }
  if (options & OPTION_QUICK)
    (void) table->file->extra(HA_EXTRA_QUICK);

  query_plan.scanned_rows= select? select->records: table->file->stats.records;
  if (order)
  {
    table->update_const_key_parts(conds);
    order= simple_remove_const(order, conds);

    if (select && select->quick && select->quick->unique_key_range())
    { // Single row select (always "ordered")
      query_plan.using_filesort= FALSE;
      query_plan.index= MAX_KEY;
    }
    else
    {
      ha_rows scanned_limit= query_plan.scanned_rows;
      query_plan.index= get_index_for_order(order, table, select, limit,
                                            &scanned_limit,
                                            &query_plan.using_filesort, 
                                            &reverse);
      if (!query_plan.using_filesort)
        query_plan.scanned_rows= scanned_limit;
    }
  }

  query_plan.select= select;
  query_plan.possible_keys= select? select->possible_keys: key_map(0);
  
  /*
    Ok, we have generated a query plan for the DELETE.
     - if we're running EXPLAIN DELETE, goto produce explain output 
     - otherwise, execute the query plan
  */
  if (thd->lex->describe)
    goto produce_explain_and_leave;
  
  explain= query_plan.save_explain_delete_data(thd->mem_root, thd);
  ANALYZE_START_TRACKING(&explain->command_tracker);

  DBUG_EXECUTE_IF("show_explain_probe_delete_exec_start", 
                  dbug_serve_apcs(thd, 1););

  if (!(select && select->quick))
    status_var_increment(thd->status_var.delete_scan_count);

  if (query_plan.using_filesort)
  {

    {
      Filesort fsort(order, HA_POS_ERROR, true, select);
      DBUG_ASSERT(query_plan.index == MAX_KEY);

      Filesort_tracker *fs_tracker= 
        thd->lex->explain->get_upd_del_plan()->filesort_tracker;

      if (!(file_sort= filesort(thd, table, &fsort, fs_tracker)))
        goto got_error;

      thd->inc_examined_row_count(file_sort->examined_rows);
      /*
        Filesort has already found and selected the rows we want to delete,
        so we don't need the where clause
      */
      delete select;

      /*
        If we are not in DELETE ... RETURNING, we can free subqueries. (in
        DELETE ... RETURNING we can't, because the RETURNING part may have
        a subquery in it)
      */
      if (!with_select)
        free_underlaid_joins(thd, select_lex);
      select= 0;
    }
  }

  /* If quick select is used, initialize it before retrieving rows. */
  if (select && select->quick && select->quick->reset())
    goto got_error;

  if (query_plan.index == MAX_KEY || (select && select->quick))
    error= init_read_record(&info, thd, table, select, file_sort, 1, 1, FALSE);
  else
    error= init_read_record_idx(&info, thd, table, 1, query_plan.index,
                                reverse);
  if (error)
    goto got_error;
  
  init_ftfuncs(thd, select_lex, 1);
  THD_STAGE_INFO(thd, stage_updating);

  if (table->prepare_triggers_for_delete_stmt_or_event())
  {
    will_batch= FALSE;
  }
  else
    will_batch= !table->file->start_bulk_delete();

  table->mark_columns_needed_for_delete();

  if (with_select)
  {
    if (result->send_result_set_metadata(select_lex->item_list,
                                         Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
      goto cleanup;
  }

  explain= (Explain_delete*)thd->lex->explain->get_upd_del_plan();
  explain->tracker.on_scan_init();

  while (!(error=info.read_record(&info)) && !thd->killed &&
	 ! thd->is_error())
  {
    explain->tracker.on_record_read();
    thd->inc_examined_row_count(1);
    if (table->vfield)
      (void) table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_DELETE);
    if (!select || select->skip_record(thd) > 0)
    {
      explain->tracker.on_record_after_where();
      if (table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                            TRG_ACTION_BEFORE, FALSE))
      {
        error= 1;
        break;
      }

      if (with_select && result->send_data(select_lex->item_list) < 0)
      {
        error=1;
        break;
      }

      if (!(error= table->file->ha_delete_row(table->record[0])))
      {
	deleted++;
        if (table->triggers &&
            table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                              TRG_ACTION_AFTER, FALSE))
        {
          error= 1;
          break;
        }
	if (!--limit && using_limit)
	{
	  error= -1;
	  break;
	}
      }
      else
      {
	table->file->print_error(error,
                                 MYF(thd->lex->ignore ? ME_JUST_WARNING : 0));
        if (thd->is_error())
        {
          error= 1;
          break;
        }
      }
    }
    /*
      Don't try unlocking the row if skip_record reported an error since in
      this case the transaction might have been rolled back already.
    */
    else if (!thd->is_error())
      table->file->unlock_row();  // Row failed selection, release lock on it
    else
      break;
  }
  killed_status= thd->killed;
  if (killed_status != NOT_KILLED || thd->is_error())
    error= 1;					// Aborted
  if (will_batch && (loc_error= table->file->end_bulk_delete()))
  {
    if (error != 1)
      table->file->print_error(loc_error,MYF(0));
    error=1;
  }
  THD_STAGE_INFO(thd, stage_end);
  end_read_record(&info);
  if (options & OPTION_QUICK)
    (void) table->file->extra(HA_EXTRA_NORMAL);
  ANALYZE_STOP_TRACKING(&explain->command_tracker);

cleanup:
  /*
    Invalidate the table in the query cache if something changed. This must
    be before binlog writing and ha_autocommit_...
  */
  if (deleted)
  {
    query_cache_invalidate3(thd, table_list, 1);
  }

  if (thd->lex->current_select->first_cond_optimization)
  {
    thd->lex->current_select->save_leaf_tables(thd);
    thd->lex->current_select->first_cond_optimization= 0;
  }

  delete select;
  select= NULL;
  transactional_table= table->file->has_transactions();

  if (!transactional_table && deleted > 0)
    thd->transaction.stmt.modified_non_trans_table=
      thd->transaction.all.modified_non_trans_table= TRUE;

  /* See similar binlogging code in sql_update.cc, for comments */
  if ((error < 0) || thd->transaction.stmt.modified_non_trans_table)
  {
    if (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      int errcode= 0;
      if (error < 0)
        thd->clear_error();
      else
        errcode= query_error_code(thd, killed_status == NOT_KILLED);

      /*
        [binlog]: If 'handler::delete_all_rows()' was called and the
        storage engine does not inject the rows itself, we replicate
        statement-based; otherwise, 'ha_delete_row()' was used to
        delete specific rows which we might log row-based.
      */
      int log_result= thd->binlog_query(query_type,
                                        thd->query(), thd->query_length(),
                                        transactional_table, FALSE, FALSE,
                                        errcode);

      if (log_result)
      {
	error=1;
      }
    }
  }
  DBUG_ASSERT(transactional_table || !deleted || thd->transaction.stmt.modified_non_trans_table);
  
  if (error < 0 || 
      (thd->lex->ignore && !thd->is_error() && !thd->is_fatal_error))
  {
    if (thd->lex->analyze_stmt)
      goto send_nothing_and_leave;

    if (with_select)
      result->send_eof();
    else
      my_ok(thd, deleted);
    DBUG_PRINT("info",("%ld records deleted",(long) deleted));
  }
  delete file_sort;
  free_underlaid_joins(thd, select_lex);
  DBUG_RETURN(error >= 0 || thd->is_error());
  
  /* Special exits */
produce_explain_and_leave:
  /* 
    We come here for various "degenerate" query plans: impossible WHERE,
    no-partitions-used, impossible-range, etc.
  */
  query_plan.save_explain_delete_data(thd->mem_root, thd);

send_nothing_and_leave:
  /* 
    ANALYZE DELETE jumps here. We can't send explain right here, because
    we might be using ANALYZE DELETE ...RETURNING, in which case we have 
    Protocol_discard active.
  */

  delete select;
  delete file_sort;
  free_underlaid_joins(thd, select_lex);
  //table->set_keyread(false);

  DBUG_ASSERT(!return_error || thd->is_error() || thd->killed);
  DBUG_RETURN((return_error || thd->is_error() || thd->killed) ? 1 : 0);

got_error:
  return_error= 1;
  goto send_nothing_and_leave;
}


/*
  Prepare items in DELETE statement

  SYNOPSIS
    mysql_prepare_delete()
    thd			- thread handler
    table_list		- global/local table list
    wild_num            - number of wildcards used in optional SELECT clause 
    field_list          - list of items in optional SELECT clause
    conds		- conditions
l
  RETURN VALUE
    FALSE OK
    TRUE  error
*/
  int mysql_prepare_delete(THD *thd, TABLE_LIST *table_list,
                           uint wild_num, List<Item> &field_list, Item **conds)
{
  Item *fake_conds= 0;
  SELECT_LEX *select_lex= &thd->lex->select_lex;
  DBUG_ENTER("mysql_prepare_delete");
  List<Item> all_fields;

  thd->lex->allow_sum_func= 0;
  if (setup_tables_and_check_access(thd, &thd->lex->select_lex.context,
                                    &thd->lex->select_lex.top_join_list,
                                    table_list, 
                                    select_lex->leaf_tables, FALSE, 
                                    DELETE_ACL, SELECT_ACL, TRUE))
    DBUG_RETURN(TRUE);
  if ((wild_num && setup_wild(thd, table_list, field_list, NULL, wild_num)) ||
      setup_fields(thd, Ref_ptr_array(),
                   field_list, MARK_COLUMNS_READ, NULL, 0) ||
      setup_conds(thd, table_list, select_lex->leaf_tables, conds) ||
      setup_ftfuncs(select_lex))
    DBUG_RETURN(TRUE);
  if (!table_list->single_table_updatable() ||
      check_key_in_view(thd, table_list))
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias, "DELETE");
    DBUG_RETURN(TRUE);
  }
  {
    TABLE_LIST *duplicate;
    if ((duplicate= unique_table(thd, table_list, table_list->next_global, 0)))
    {
      update_non_unique_table_error(table_list, "DELETE", duplicate);
      DBUG_RETURN(TRUE);
    }
  }

  if (select_lex->inner_refs_list.elements &&
    fix_inner_refs(thd, all_fields, select_lex, select_lex->ref_pointer_array))
    DBUG_RETURN(TRUE);

  select_lex->fix_prepare_information(thd, conds, &fake_conds); 
  DBUG_RETURN(FALSE);
}


/***************************************************************************
  Delete multiple tables from join 
***************************************************************************/

#define MEM_STRIP_BUF_SIZE current_thd->variables.sortbuff_size

extern "C" int refpos_order_cmp(void* arg, const void *a,const void *b)
{
  handler *file= (handler*)arg;
  return file->cmp_ref((const uchar*)a, (const uchar*)b);
}

/*
  make delete specific preparation and checks after opening tables

  SYNOPSIS
    mysql_multi_delete_prepare()
    thd         thread handler

  RETURN
    FALSE OK
    TRUE  Error
*/

int mysql_multi_delete_prepare(THD *thd)
{
  LEX *lex= thd->lex;
  TABLE_LIST *aux_tables= lex->auxiliary_table_list.first;
  TABLE_LIST *target_tbl;
  DBUG_ENTER("mysql_multi_delete_prepare");

  if (mysql_handle_derived(lex, DT_INIT))
    DBUG_RETURN(TRUE);
  if (mysql_handle_derived(lex, DT_MERGE_FOR_INSERT))
    DBUG_RETURN(TRUE);
  if (mysql_handle_derived(lex, DT_PREPARE))
    DBUG_RETURN(TRUE);
  /*
    setup_tables() need for VIEWs. JOIN::prepare() will not do it second
    time.

    lex->query_tables also point on local list of DELETE SELECT_LEX
  */
  if (setup_tables_and_check_access(thd, &thd->lex->select_lex.context,
                                    &thd->lex->select_lex.top_join_list,
                                    lex->query_tables,
                                    lex->select_lex.leaf_tables, FALSE, 
                                    DELETE_ACL, SELECT_ACL, FALSE))
    DBUG_RETURN(TRUE);

  if (lex->select_lex.handle_derived(thd->lex, DT_MERGE))  
    DBUG_RETURN(TRUE);

  /*
    Multi-delete can't be constructed over-union => we always have
    single SELECT on top and have to check underlying SELECTs of it
  */
  lex->select_lex.exclude_from_table_unique_test= TRUE;
  /* Fix tables-to-be-deleted-from list to point at opened tables */
  for (target_tbl= (TABLE_LIST*) aux_tables;
       target_tbl;
       target_tbl= target_tbl->next_local)
  {

    target_tbl->table= target_tbl->correspondent_table->table;
    if (target_tbl->correspondent_table->is_multitable())
    {
       my_error(ER_VIEW_DELETE_MERGE_VIEW, MYF(0),
                target_tbl->correspondent_table->view_db.str,
                target_tbl->correspondent_table->view_name.str);
       DBUG_RETURN(TRUE);
    }

    if (!target_tbl->correspondent_table->single_table_updatable() ||
        check_key_in_view(thd, target_tbl->correspondent_table))
    {
      my_error(ER_NON_UPDATABLE_TABLE, MYF(0),
               target_tbl->table_name, "DELETE");
      DBUG_RETURN(TRUE);
    }
    /*
      Check that table from which we delete is not used somewhere
      inside subqueries/view.
    */
    {
      TABLE_LIST *duplicate;
      if ((duplicate= unique_table(thd, target_tbl->correspondent_table,
                                   lex->query_tables, 0)))
      {
        update_non_unique_table_error(target_tbl->correspondent_table,
                                      "DELETE", duplicate);
        DBUG_RETURN(TRUE);
      }
    }
  }
  /*
    Reset the exclude flag to false so it doesn't interfare
    with further calls to unique_table
  */
  lex->select_lex.exclude_from_table_unique_test= FALSE;
  
  if (lex->save_prep_leaf_tables())
    DBUG_RETURN(TRUE);
  
  DBUG_RETURN(FALSE);
}


multi_delete::multi_delete(THD *thd_arg, TABLE_LIST *dt, uint num_of_tables_arg):
    select_result_interceptor(thd_arg), delete_tables(dt), deleted(0), found(0),
    num_of_tables(num_of_tables_arg), error(0),
    do_delete(0), transactional_tables(0), normal_tables(0), error_handled(0)
{
  tempfiles= (Unique **) thd_arg->calloc(sizeof(Unique *) * num_of_tables);
}


int
multi_delete::prepare(List<Item> &values, SELECT_LEX_UNIT *u)
{
  DBUG_ENTER("multi_delete::prepare");
  unit= u;
  do_delete= 1;
  THD_STAGE_INFO(thd, stage_deleting_from_main_table);
  SELECT_LEX *select_lex= u->first_select();
  if (select_lex->first_cond_optimization)
  {
    if (select_lex->handle_derived(thd->lex, DT_MERGE))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(0);
}

void multi_delete::prepare_to_read_rows()
{
  /* see multi_update::prepare_to_read_rows() */
  for (TABLE_LIST *walk= delete_tables; walk; walk= walk->next_local)
  {
    TABLE_LIST *tbl= walk->correspondent_table->find_table_for_update();
    tbl->table->mark_columns_needed_for_delete();
  }
}

bool
multi_delete::initialize_tables(JOIN *join)
{
  TABLE_LIST *walk;
  Unique **tempfiles_ptr;
  DBUG_ENTER("initialize_tables");

  if ((thd->variables.option_bits & OPTION_SAFE_UPDATES) && error_if_full_join(join))
    DBUG_RETURN(1);

  table_map tables_to_delete_from=0;
  delete_while_scanning= 1;
  for (walk= delete_tables; walk; walk= walk->next_local)
  {
    TABLE_LIST *tbl= walk->correspondent_table->find_table_for_update();
    tables_to_delete_from|= tbl->table->map;
    if (delete_while_scanning &&
        unique_table(thd, tbl, join->tables_list, false))
    {
      /*
        If the table we are going to delete from appears
        in join, we need to defer delete. So the delete
        doesn't interfers with the scaning of results.
      */
      delete_while_scanning= 0;
    }
  }

  walk= delete_tables;

  for (JOIN_TAB *tab= first_linear_tab(join, WITHOUT_BUSH_ROOTS,
                                       WITH_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITHOUT_BUSH_ROOTS))
  {
    if (!tab->bush_children && tab->table->map & tables_to_delete_from)
    {
      /* We are going to delete from this table */
      TABLE *tbl=walk->table=tab->table;
      walk= walk->next_local;
      /* Don't use KEYREAD optimization on this table */
      tbl->no_keyread=1;
      /* Don't use record cache */
      tbl->no_cache= 1;
      tbl->covering_keys.clear_all();
      if (tbl->file->has_transactions())
	transactional_tables= 1;
      else
	normal_tables= 1;
      tbl->prepare_triggers_for_delete_stmt_or_event();
      tbl->prepare_for_position();
    }
    else if ((tab->type != JT_SYSTEM && tab->type != JT_CONST) &&
             walk == delete_tables)
    {
      /*
        We are not deleting from the table we are scanning. In this
        case send_data() shouldn't delete any rows a we may touch
        the rows in the deleted table many times
      */
      delete_while_scanning= 0;
    }
  }
  walk= delete_tables;
  tempfiles_ptr= tempfiles;
  if (delete_while_scanning)
  {
    table_being_deleted= delete_tables;
    walk= walk->next_local;
  }
  for (;walk ;walk= walk->next_local)
  {
    TABLE *table=walk->table;
    *tempfiles_ptr++= new Unique (refpos_order_cmp,
				  (void *) table->file,
				  table->file->ref_length,
				  MEM_STRIP_BUF_SIZE);
  }
  init_ftfuncs(thd, thd->lex->current_select, 1);
  DBUG_RETURN(thd->is_fatal_error != 0);
}


multi_delete::~multi_delete()
{
  for (table_being_deleted= delete_tables;
       table_being_deleted;
       table_being_deleted= table_being_deleted->next_local)
  {
    TABLE *table= table_being_deleted->table;
    table->no_keyread=0;
  }

  for (uint counter= 0; counter < num_of_tables; counter++)
  {
    if (tempfiles[counter])
      delete tempfiles[counter];
  }
}


int multi_delete::send_data(List<Item> &values)
{
  int secure_counter= delete_while_scanning ? -1 : 0;
  TABLE_LIST *del_table;
  DBUG_ENTER("multi_delete::send_data");

  bool ignore= thd->lex->ignore;

  for (del_table= delete_tables;
       del_table;
       del_table= del_table->next_local, secure_counter++)
  {
    TABLE *table= del_table->table;

    /* Check if we are using outer join and we didn't find the row */
    if (table->status & (STATUS_NULL_ROW | STATUS_DELETED))
      continue;

    table->file->position(table->record[0]);
    found++;

    if (secure_counter < 0)
    {
      /* We are scanning the current table */
      DBUG_ASSERT(del_table == table_being_deleted);
      if (table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                            TRG_ACTION_BEFORE, FALSE))
        DBUG_RETURN(1);
      table->status|= STATUS_DELETED;
      if (!(error=table->file->ha_delete_row(table->record[0])))
      {
        deleted++;
        if (!table->file->has_transactions())
          thd->transaction.stmt.modified_non_trans_table= TRUE;
        if (table->triggers &&
            table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                              TRG_ACTION_AFTER, FALSE))
          DBUG_RETURN(1);
      }
      else if (!ignore)
      {
        /*
          If the IGNORE option is used errors caused by ha_delete_row don't
          have to stop the iteration.
        */
        table->file->print_error(error,MYF(0));
        DBUG_RETURN(1);
      }
    }
    else
    {
      error=tempfiles[secure_counter]->unique_add((char*) table->file->ref);
      if (error)
      {
	error= 1;                               // Fatal error
	DBUG_RETURN(1);
      }
    }
  }
  DBUG_RETURN(0);
}


void multi_delete::abort_result_set()
{
  DBUG_ENTER("multi_delete::abort_result_set");

  /* the error was handled or nothing deleted and no side effects return */
  if (error_handled ||
      (!thd->transaction.stmt.modified_non_trans_table && !deleted))
    DBUG_VOID_RETURN;

  /* Something already deleted so we have to invalidate cache */
  if (deleted)
    query_cache_invalidate3(thd, delete_tables, 1);

  if (thd->transaction.stmt.modified_non_trans_table)
    thd->transaction.all.modified_non_trans_table= TRUE;
  thd->transaction.all.m_unsafe_rollback_flags|=
    (thd->transaction.stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);

  /*
    If rows from the first table only has been deleted and it is
    transactional, just do rollback.
    The same if all tables are transactional, regardless of where we are.
    In all other cases do attempt deletes ...
  */
  if (do_delete && normal_tables &&
      (table_being_deleted != delete_tables ||
       !table_being_deleted->table->file->has_transactions()))
  {
    /*
      We have to execute the recorded do_deletes() and write info into the
      error log
    */
    error= 1;
    send_eof();
    DBUG_ASSERT(error_handled);
    DBUG_VOID_RETURN;
  }

  if (thd->transaction.stmt.modified_non_trans_table)
  {
    /*
       there is only side effects; to binlog with the error
    */
    if (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      int errcode= query_error_code(thd, thd->killed == NOT_KILLED);
      /* possible error of writing binary log is ignored deliberately */
      (void) thd->binlog_query(THD::ROW_QUERY_TYPE,
                               thd->query(), thd->query_length(),
                               transactional_tables, FALSE, FALSE, errcode);
    }
  }
  DBUG_VOID_RETURN;
}



/**
  Do delete from other tables.

  @retval 0 ok
  @retval 1 error

  @todo Is there any reason not use the normal nested-loops join? If not, and
  there is no documentation supporting it, this method and callee should be
  removed and there should be hooks within normal execution.
*/

int multi_delete::do_deletes()
{
  DBUG_ENTER("do_deletes");
  DBUG_ASSERT(do_delete);

  do_delete= 0;                                 // Mark called
  if (!found)
    DBUG_RETURN(0);

  table_being_deleted= (delete_while_scanning ? delete_tables->next_local :
                        delete_tables);
 
  for (uint counter= 0; table_being_deleted;
       table_being_deleted= table_being_deleted->next_local, counter++)
  { 
    TABLE *table = table_being_deleted->table;
    int local_error; 
    if (tempfiles[counter]->get(table))
      DBUG_RETURN(1);

    local_error= do_table_deletes(table, &tempfiles[counter]->sort,
                                  thd->lex->ignore);

    if (thd->killed && !local_error)
      DBUG_RETURN(1);

    if (local_error == -1)				// End of file
      local_error = 0;

    if (local_error)
      DBUG_RETURN(local_error);
  }
  DBUG_RETURN(0);
}


/**
   Implements the inner loop of nested-loops join within multi-DELETE
   execution.

   @param table The table from which to delete.

   @param ignore If used, all non fatal errors will be translated
   to warnings and we should not break the row-by-row iteration.

   @return Status code

   @retval  0 All ok.
   @retval  1 Triggers or handler reported error.
   @retval -1 End of file from handler.
*/
int multi_delete::do_table_deletes(TABLE *table, SORT_INFO *sort_info,
                                   bool ignore)
{
  int local_error= 0;
  READ_RECORD info;
  ha_rows last_deleted= deleted;
  DBUG_ENTER("do_deletes_for_table");

  if (init_read_record(&info, thd, table, NULL, sort_info, 0, 1, FALSE))
    DBUG_RETURN(1);

  /*
    Ignore any rows not found in reference tables as they may already have
    been deleted by foreign key handling
  */
  info.ignore_not_found_rows= 1;
  bool will_batch= !table->file->start_bulk_delete();
  while (!(local_error= info.read_record(&info)) && !thd->killed)
  {
    if (table->triggers &&
        table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                          TRG_ACTION_BEFORE, FALSE))
    {
      local_error= 1;
      break;
    }
      
    local_error= table->file->ha_delete_row(table->record[0]);
    if (local_error && !ignore)
    {
      table->file->print_error(local_error, MYF(0));
      break;
    }
      
    /*
      Increase the reported number of deleted rows only if no error occurred
      during ha_delete_row.
      Also, don't execute the AFTER trigger if the row operation failed.
    */
    if (!local_error)
    {
      deleted++;
      if (table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                            TRG_ACTION_AFTER, FALSE))
      {
        local_error= 1;
        break;
      }
    }
  }
  if (will_batch)
  {
    int tmp_error= table->file->end_bulk_delete();
    if (tmp_error && !local_error)
    {
      local_error= tmp_error;
      table->file->print_error(local_error, MYF(0));
    }
  }
  if (last_deleted != deleted && !table->file->has_transactions())
    thd->transaction.stmt.modified_non_trans_table= TRUE;

  end_read_record(&info);

  DBUG_RETURN(local_error);
}

/*
  Send ok to the client

  return:  0 sucess
	   1 error
*/

bool multi_delete::send_eof()
{
  killed_state killed_status= NOT_KILLED;
  THD_STAGE_INFO(thd, stage_deleting_from_reference_tables);

  /* Does deletes for the last n - 1 tables, returns 0 if ok */
  int local_error= do_deletes();		// returns 0 if success

  /* compute a total error to know if something failed */
  local_error= local_error || error;
  killed_status= (local_error == 0)? NOT_KILLED : thd->killed;
  /* reset used flags */
  THD_STAGE_INFO(thd, stage_end);

  if (thd->transaction.stmt.modified_non_trans_table)
    thd->transaction.all.modified_non_trans_table= TRUE;
  thd->transaction.all.m_unsafe_rollback_flags|=
    (thd->transaction.stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);

  /*
    We must invalidate the query cache before binlog writing and
    ha_autocommit_...
  */
  if (deleted)
  {
    query_cache_invalidate3(thd, delete_tables, 1);
  }
  if ((local_error == 0) || thd->transaction.stmt.modified_non_trans_table)
  {
    if(WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      int errcode= 0;
      if (local_error == 0)
        thd->clear_error();
      else
        errcode= query_error_code(thd, killed_status == NOT_KILLED);
      if (thd->binlog_query(THD::ROW_QUERY_TYPE,
                            thd->query(), thd->query_length(),
                            transactional_tables, FALSE, FALSE, errcode) &&
          !normal_tables)
      {
	local_error=1;  // Log write failed: roll back the SQL statement
      }
    }
  }
  if (local_error != 0)
    error_handled= TRUE; // to force early leave from ::abort_result_set()

  if (!local_error && !thd->lex->analyze_stmt)
  {
    ::my_ok(thd, deleted);
  }
  return 0;
}
