/*
   Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2010, 2022, MariaDB

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
  Delete of records tables.

  Multi-table deletes were introduced by Monty and Sinisa
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_delete.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"                           // open_temprary_table
#include "lock.h"                              // unlock_table_name
#include "sql_view.h"             // check_key_in_view, mysql_frm_type
#include "sql_parse.h"            // mysql_init_select
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
#include "sql_derived.h"                        // mysql_handle_derived
#include "key.h"
                                                // end_read_record
#include "sql_insert.h"          // fix_rownum_pointers
#include "sql_partition.h"       // make_used_partitions_str
#ifdef WITH_WSREP
#include "wsrep_mysqld.h"
#endif

#define MEM_STRIP_BUF_SIZE ((size_t) thd->variables.sortbuff_size)

/*
  @brief
    Print query plan of a single-table DELETE command
  
  @detail
    This function is used by EXPLAIN DELETE and by SHOW EXPLAIN when it is
    invoked on a running DELETE statement.
*/

Explain_delete* Delete_plan::save_explain_delete_data(THD *thd, MEM_ROOT *mem_root)
{
  Explain_query *query= thd->lex->explain;
  Explain_delete *explain= 
     new (mem_root) Explain_delete(mem_root, thd->lex->analyze_stmt);
  if (!explain)
    return 0;

  if (deleting_all_rows)
  {
    explain->deleting_all_rows= true;
    explain->select_type= "SIMPLE";
    explain->rows= scanned_rows;
  }
  else
  {
    explain->deleting_all_rows= false;
    if (Update_plan::save_explain_data_intern(thd, mem_root, explain,
                                              thd->lex->analyze_stmt))
      return 0;
  }
 
  query->add_upd_del_plan(explain);
  return explain;
}


Explain_update* 
Update_plan::save_explain_update_data(THD *thd, MEM_ROOT *mem_root)
{
  Explain_query *query= thd->lex->explain;
  Explain_update* explain= 
    new (mem_root) Explain_update(mem_root, thd->lex->analyze_stmt);
  if (!explain)
    return 0;
  if (save_explain_data_intern(thd, mem_root, explain, thd->lex->analyze_stmt))
    return 0;
  query->add_upd_del_plan(explain);
  return explain;
}


bool Update_plan::save_explain_data_intern(THD *thd,
                                           MEM_ROOT *mem_root,
                                           Explain_update *explain,
                                           bool is_analyze)
{
  explain->select_type= "SIMPLE";
  explain->table_name.append(table->alias);
  
  explain->impossible_where= false;
  explain->no_partitions= false;

  if (impossible_where)
  {
    explain->impossible_where= true;
    return 0;
  }

  if (no_partitions)
  {
    explain->no_partitions= true;
    return 0;
  }
  
  if (is_analyze ||
      (thd->variables.log_slow_verbosity &
       LOG_SLOW_VERBOSITY_ENGINE))
  {
    explain->table_tracker.set_gap_tracker(&explain->extra_time_tracker);
    table->file->set_time_tracker(&explain->table_tracker);

    if (table->file->handler_stats && table->s->tmp_table != INTERNAL_TMP_TABLE)
      explain->handler_for_stats= table->file;
  }

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
    if (!(explain->filesort_tracker= new (mem_root) Filesort_tracker(is_analyze)))
      return 1;
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

  /* Save subquery children */
  for (SELECT_LEX_UNIT *unit= select_lex->first_inner_unit();
       unit;
       unit= unit->next_unit())
  {
    if (unit->explainable())
      explain->add_child(unit->first_select()->select_number);
  }
  return 0;
}


static bool record_should_be_deleted(THD *thd, TABLE *table, SQL_SELECT *sel,
                                     Explain_delete *explain, bool truncate_history)
{
  explain->tracker.on_record_read();
  thd->inc_examined_row_count();
  if (table->vfield)
    (void) table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_DELETE);
  if (!sel || sel->skip_record(thd) > 0)
  {
    explain->tracker.on_record_after_where();
    return true;
  }
  return false;
}

static
int update_portion_of_time(THD *thd, TABLE *table,
                           const vers_select_conds_t &period_conds,
                           bool *inside_period)
{
  bool lcond= period_conds.field_start->val_datetime_packed(thd)
              < period_conds.start.item->val_datetime_packed(thd);
  bool rcond= period_conds.field_end->val_datetime_packed(thd)
              > period_conds.end.item->val_datetime_packed(thd);

  *inside_period= !lcond && !rcond;
  if (*inside_period)
    return 0;

  DBUG_ASSERT(!table->triggers
              || !table->triggers->has_triggers(TRG_EVENT_INSERT,
                                                TRG_ACTION_BEFORE));

  int res= 0;
  Item *src= lcond ? period_conds.start.item : period_conds.end.item;
  uint dst_fieldno= lcond ? table->s->period.end_fieldno
                          : table->s->period.start_fieldno;

  ulonglong prev_insert_id= table->file->next_insert_id;
  store_record(table, record[1]);
  if (likely(!res))
    res= src->save_in_field(table->field[dst_fieldno], true);

  if (likely(!res))
  {
    table->period_prepare_autoinc();
    res= table->update_generated_fields();
  }

  if(likely(!res))
    res= table->file->ha_update_row(table->record[1], table->record[0]);

  if (likely(!res) && table->triggers)
    res= table->triggers->process_triggers(thd, TRG_EVENT_INSERT,
                                           TRG_ACTION_AFTER, true, nullptr);
  restore_record(table, record[1]);
  if (res)
    table->file->restore_auto_increment(prev_insert_id);

  if (likely(!res) && lcond && rcond)
    res= table->period_make_insert(period_conds.end.item,
                                   table->field[table->s->period.start_fieldno]);

  return res;
}

/**
  Delete a record stored in:
  replace= true: record[0]
  replace= false: record[1]

  with regard to the treat_versioned flag, which can be false for a versioned
  table in case of versioned->versioned replication.

 For a versioned case, we detect a few conditions, under which we should delete
 a row instead of updating it to a history row.
 This includes:
 * History deletion by user;
 * History collision, in case of REPLACE or very fast sequence of dmls
   so that timestamp doesn't change;
 * History collision in the parent table

 A normal delete is processed here as well.
*/
template <bool replace>
int TABLE::delete_row(bool treat_versioned)
{
  int err= 0;
  bool remembered_pos= false;
  uchar *del_buf= record[replace ? 1 : 0];
  bool delete_row= !treat_versioned
                   || in_use->lex->vers_conditions.delete_history
                   || versioned(VERS_TRX_ID)
                   || !vers_end_field()->is_max(
                           vers_end_field()->ptr_in_record(del_buf));

  if (!delete_row)
  {
    if ((err= file->extra(HA_EXTRA_REMEMBER_POS)))
      return err;
    remembered_pos= true;

    if (replace)
    {
      store_record(this, record[2]);
      restore_record(this, record[1]);
    }
    else
    {
      store_record(this, record[1]);
    }
    vers_update_end();

    Field *row_start= vers_start_field();
    Field *row_end= vers_end_field();
    // Don't make history row with negative lifetime
    delete_row= row_start->cmp(row_start->ptr, row_end->ptr) > 0;

    if (likely(!delete_row))
      err= file->ha_update_row(record[1], record[0]);
    if (unlikely(err))
    {
      /*
        MDEV-23644: we get HA_ERR_FOREIGN_DUPLICATE_KEY iff we already got
        history row with same trx_id which is the result of foreign key
        action, so we don't need one more history row.

        Additionally, delete the row if versioned record already exists.
        This happens on replace, a very fast sequence of inserts and deletes,
        or if timestamp is frozen.
      */
      delete_row= err == HA_ERR_FOUND_DUPP_KEY
                  || err == HA_ERR_FOUND_DUPP_UNIQUE
                  || err == HA_ERR_FOREIGN_DUPLICATE_KEY;
      if (!delete_row)
        return err;
    }

    if (delete_row)
      del_buf= record[1];

    if (replace)
      restore_record(this, record[2]);
  }

  if (delete_row)
    err= file->ha_delete_row(del_buf);

  if (remembered_pos)
    (void) file->extra(HA_EXTRA_RESTORE_POS);

  return err;
}

template int TABLE::delete_row<true>(bool treat_versioned);
template int TABLE::delete_row<false>(bool treat_versioned);

/**
  @brief Special handling of single-table deletes after prepare phase

  @param thd  global context the processed statement
  @returns false on success, true on error
*/

bool Sql_cmd_delete::delete_from_single_table(THD *thd)
{
  int error;
  int loc_error;
  bool transactional_table;
  bool const_cond;
  bool safe_update;
  bool const_cond_result;
  bool return_error= 0;
  bool binlogged= 0;
  TABLE	*table;
  SQL_SELECT *select= 0;
  SORT_INFO *file_sort= 0;
  READ_RECORD info;
  bool reverse= FALSE;
  bool binlog_is_row;
  killed_state killed_status= NOT_KILLED;
  THD::enum_binlog_query_type query_type= THD::ROW_QUERY_TYPE;
  bool will_batch= FALSE;

  bool has_triggers= false;
  SELECT_LEX_UNIT *unit = &lex->unit;
  SELECT_LEX *select_lex= unit->first_select();
  SELECT_LEX *returning= thd->lex->has_returning() ? thd->lex->returning() : 0;
  TABLE_LIST *const table_list = select_lex->get_table_list();
  ulonglong options= select_lex->options;
  ORDER *order= select_lex->order_list.first;
  COND *conds= select_lex->join->conds;
  ha_rows limit= unit->lim.get_select_limit();
  bool using_limit= limit != HA_POS_ERROR;

  Delete_plan query_plan(thd->mem_root);
  Explain_delete *explain;
  Unique * deltempfile= NULL;
  bool delete_record= false;
  bool delete_while_scanning= table_list->delete_while_scanning;
  bool portion_of_time_through_update;
  /*
    TRUE if we are after the call to
    select_lex->optimize_unflattened_subqueries(true) and before the
    call to select_lex->optimize_unflattened_subqueries(false), to
    ensure a call to
    select_lex->optimize_unflattened_subqueries(false) happens which
    avoid 2nd ps mem leaks when e.g. the first execution produces
    empty result and the second execution produces a non-empty set
  */
  bool optimize_subqueries= FALSE;

  DBUG_ENTER("Sql_cmd_delete::delete_single_table");

  query_plan.index= MAX_KEY;
  query_plan.using_filesort= FALSE;

  THD_STAGE_INFO(thd, stage_init_update);

  const bool delete_history= table_list->vers_conditions.delete_history;
  DBUG_ASSERT(!(delete_history && table_list->period_conditions.is_set()));

  if (table_list->handle_derived(thd->lex, DT_MERGE_FOR_INSERT))
    DBUG_RETURN(1);
  if (table_list->handle_derived(thd->lex, DT_PREPARE))
    DBUG_RETURN(1);

  table= table_list->table;

  if (!table_list->single_table_updatable())
  {
     my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "DELETE");
     DBUG_RETURN(TRUE);
  }

  if (!table || !table->is_created())
  {
      my_error(ER_VIEW_DELETE_MERGE_VIEW, MYF(0),
	       table_list->view_db.str, table_list->view_name.str);
    DBUG_RETURN(TRUE);
  }

  query_plan.select_lex= thd->lex->first_select_lex();
  query_plan.table= table;

  thd->lex->promote_select_describe_flag_if_needed();

  /*
    Apply the IN=>EXISTS transformation to all constant subqueries
    and optimize them.

    It is too early to choose subquery optimization strategies without
    an estimate of how many times the subquery will be executed so we
    call optimize_unflattened_subqueries() with const_only= true, and
    choose between materialization and in-to-exists later.
  */
  if (select_lex->optimize_unflattened_subqueries(true))
    DBUG_RETURN(TRUE);
  optimize_subqueries= TRUE;

  const_cond= (!conds || conds->const_item());
  safe_update= (thd->variables.option_bits & OPTION_SAFE_UPDATES) &&
               !thd->lex->describe;
  if (safe_update && const_cond)
  {
    my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
               ER_THD(thd, ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
    DBUG_RETURN(TRUE);
  }

  const_cond_result= const_cond && (!conds || conds->val_bool());
  if (unlikely(thd->is_error()))
  {
    /* Error evaluating val_bool(). */
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
    - If there is a condition, then it produces a non-zero value
    - If the current command is DELETE FROM with no where clause, then:
      - We should not be binlogging this statement in row-based, and
      - there should be no delete triggers associated with the table.
  */

  has_triggers= table->triggers && table->triggers->has_delete_triggers();
  transactional_table= table->file->has_transactions_and_rollback();
  deleted= 0;

  if (!returning && !using_limit && const_cond_result &&
      !thd->is_current_stmt_binlog_format_row() && !has_triggers &&
      !table->versioned(VERS_TIMESTAMP) && !table_list->has_period())
  {
    /* Update the table->file->stats.records number */
    table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
    ha_rows const maybe_deleted= table->file->stats.records;
    DBUG_PRINT("debug", ("Trying to use delete_all_rows()"));

    query_plan.set_delete_all_rows(maybe_deleted);
    if (thd->lex->describe)
      goto produce_explain_and_leave;

    table->file->prepare_for_modify(false, false);
    if (likely(!(error=table->file->ha_delete_all_rows())))
    {
      /*
        If delete_all_rows() is used, it is not possible to log the
        query in row format, so we have to log it in statement format.
      */
      query_type= THD::STMT_QUERY_TYPE;
      error= -1;
      deleted= maybe_deleted;
      if (!query_plan.save_explain_delete_data(thd, thd->mem_root))
        error= 1;
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
  if (conds && thd->lex->are_date_funcs_used())
  {
    /* Rewrite datetime comparison conditions into sargable */
    conds= conds->top_level_transform(thd, &Item::date_conds_transformer,
                                      (uchar *) 0);
  }

  if (conds && optimizer_flag(thd, OPTIMIZER_SWITCH_SARGABLE_CASEFOLD))
  {
    conds= conds->top_level_transform(thd, &Item::varchar_upper_cmp_transformer,
                                          (uchar *) 0);
  }

  if ((conds || order) && substitute_indexed_vcols_for_table(table, conds,
                                                             order, select_lex))
   DBUG_RETURN(1); // Fatal error

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (prune_partitions(thd, table, conds))
  {
    if (optimize_subqueries && select_lex->optimize_unflattened_subqueries(false))
      DBUG_RETURN(TRUE);
    optimize_subqueries= FALSE;
    free_underlaid_joins(thd, select_lex);

    query_plan.set_no_partitions();
    if (thd->lex->describe || thd->lex->analyze_stmt)
      goto produce_explain_and_leave;

    if (thd->binlog_for_noop_dml(transactional_table))
      DBUG_RETURN(1);

    if (!thd->lex->current_select->leaf_tables_saved)
    {
      thd->lex->current_select->save_leaf_tables(thd);
      thd->lex->current_select->leaf_tables_saved= true;
      thd->lex->current_select->first_cond_optimization= 0;
    }

    my_ok(thd, 0);
    DBUG_RETURN(0);
  }
#endif
  /* Update the table->file->stats.records number */
  table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
  set_statistics_for_table(thd, table);

  table->covering_keys.clear_all();
  table->opt_range_keys.clear_all();

  select=make_select(table, 0, 0, conds, (SORT_INFO*) 0, 0, &error);
  if (unlikely(error))
    DBUG_RETURN(TRUE);
  if ((select && select->check_quick(thd, safe_update, limit,
                                     Item_func::BITMAP_ALL)) || !limit ||
      table->stat_records() == 0)
  {
    query_plan.set_impossible_where();
    if (thd->lex->describe || thd->lex->analyze_stmt)
      goto produce_explain_and_leave;

    delete select;
    if (select_lex->optimize_unflattened_subqueries(false))
      DBUG_RETURN(TRUE);
    optimize_subqueries= FALSE;
    free_underlaid_joins(thd, select_lex);
    /* 
      Error was already created by quick select evaluation (check_quick()).
      TODO: Add error code output parameter to Item::val_xxx() methods.
      Currently they rely on the user checking DA for
      errors when unwinding the stack after calling Item::val_xxx().
    */
    if (unlikely(thd->is_error()))
      DBUG_RETURN(TRUE);

    if (thd->binlog_for_noop_dml(transactional_table))
      DBUG_RETURN(1);

    if (!thd->lex->current_select->leaf_tables_saved)
    {
      thd->lex->current_select->save_leaf_tables(thd);
      thd->lex->current_select->leaf_tables_saved= true;
      thd->lex->current_select->first_cond_optimization= 0;
    }

    my_ok(thd, 0);
    DBUG_RETURN(0);				// Nothing to delete
  }

  /* If running in safe sql mode, don't allow updates without keys */
  if (!select || !select->quick)
  {
    thd->set_status_no_index_used();
    if (safe_update && !using_limit)
    {
      delete select;
      if (optimize_subqueries && select_lex->optimize_unflattened_subqueries(false))
        DBUG_RETURN(TRUE);
      optimize_subqueries= FALSE;
      free_underlaid_joins(thd, select_lex);
      my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
                 ER_THD(thd, ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
      DBUG_RETURN(TRUE);
    }
  }
  if (options & OPTION_QUICK)
    (void) table->file->extra(HA_EXTRA_QUICK);

  /*
    Estimate the number of scanned rows and have it accessible in
    JOIN::choose_subquery_plan() from the outer join through
    JOIN::sql_cmd_dml
  */
  scanned_rows= query_plan.scanned_rows= select ?
    select->records : table->file->stats.records;
  select_lex->join->sql_cmd_dml= this;
  DBUG_ASSERT(optimize_subqueries);
  if (select_lex->optimize_unflattened_subqueries(false))
    DBUG_RETURN(TRUE);
  optimize_subqueries= FALSE;
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
      table->no_keyread= 1;
      query_plan.index= get_index_for_order(order, table, select, limit,
                                            &scanned_limit,
                                            &query_plan.using_filesort, 
                                            &reverse);
      table->no_keyread= 0;
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
  
  if (!(explain= query_plan.save_explain_delete_data(thd, thd->mem_root)))
    goto got_error;
  ANALYZE_START_TRACKING(thd, &explain->command_tracker);

  DBUG_EXECUTE_IF("show_explain_probe_delete_exec_start", 
                  dbug_serve_apcs(thd, 1););

  if (!(select && select->quick))
    status_var_increment(thd->status_var.delete_scan_count);

  binlog_is_row= thd->is_current_stmt_binlog_format_row();
  DBUG_PRINT("info", ("binlog_is_row: %s", binlog_is_row ? "TRUE" : "FALSE"));

  /*
    We can use direct delete (delete that is done silently in the handler)
    if none of the following conditions are true:
    - There are triggers
    - There is binary logging
    - There is a virtual not stored column in the WHERE clause
    - ORDER BY or LIMIT
      - As this requires the rows to be deleted in a specific order
      - Note that Spider can handle ORDER BY and LIMIT in a cluster with
        one data node.  These conditions are therefore checked in
        direct_delete_rows_init().

    Direct delete does not require a WHERE clause

    Later we also ensure that we are only using one table (no sub queries)
  */

  if ((table->file->ha_table_flags() & HA_CAN_DIRECT_UPDATE_AND_DELETE) &&
      !has_triggers && !binlog_is_row && !returning &&
      !table_list->has_period())
  {
    table->mark_columns_needed_for_delete();
    if (!table->check_virtual_columns_marked_for_read())
    {
      DBUG_PRINT("info", ("Trying direct delete"));
      bool use_direct_delete= !select || !select->cond;
      if (!use_direct_delete &&
          (select->cond->used_tables() & ~RAND_TABLE_BIT) == table->map)
      {
        DBUG_ASSERT(!table->file->pushed_cond);
        if (!table->file->cond_push(select->cond))
        {
          use_direct_delete= TRUE;
          table->file->pushed_cond= select->cond;
        }
      }
      if (use_direct_delete && !table->file->direct_delete_rows_init())
      {
        /* Direct deleting is supported */
        DBUG_PRINT("info", ("Using direct delete"));
        THD_STAGE_INFO(thd, stage_updating);
        if (!(error= table->file->ha_direct_delete_rows(&deleted)))
          error= -1;
        goto terminate_delete;
      }
    }
  }

  if (query_plan.using_filesort)
  {
    {
      Filesort fsort(order, HA_POS_ERROR, true, select);
      DBUG_ASSERT(query_plan.index == MAX_KEY);

      Filesort_tracker *fs_tracker=
        thd->lex->explain->get_upd_del_plan()->filesort_tracker;

      if (!(file_sort= filesort(thd, table, &fsort, fs_tracker)))
        goto got_error;

      thd->ps_report_examined_row_count();
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
      if (!returning)
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
  if (unlikely(error))
    goto got_error;
  
  if (unlikely(init_ftfuncs(thd, select_lex, 1)))
    goto got_error;

  if (table_list->has_period())
  {
    table->use_all_columns();
    table->rpl_write_set= table->write_set;
    // Initialize autoinc.
    // We don't set next_number_field here, as it is handled manually.
    if (table->found_next_number_field)
      table->file->info(HA_STATUS_AUTO);
  }
  else
  {
    table->mark_columns_needed_for_delete();
  }

  if (!table->prepare_triggers_for_delete_stmt_or_event() &&
      table->file->ha_table_flags() & HA_CAN_FORCE_BULK_DELETE)
    will_batch= !table->file->start_bulk_delete();

  /*
    thd->get_stmt_da()->is_set() means first iteration of prepared statement
    with array binding operation execution (non optimized so it is not
    INSERT)
  */
  if (returning && !thd->get_stmt_da()->is_set())
  {
    if (result->send_result_set_metadata(returning->item_list,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
      goto cleanup;
  }

  explain= (Explain_delete*)thd->lex->explain->get_upd_del_plan();
  explain->tracker.on_scan_init();

  thd->get_stmt_da()->reset_current_row_for_warning(1);

  if (!delete_while_scanning)
  {
    /*
      The table we are going to delete appears in subqueries in the where
      clause.  Instead of deleting the rows, first mark them deleted.
    */
    ha_rows tmplimit=limit;
    deltempfile= new (thd->mem_root) Unique (refpos_order_cmp, table->file,
                                             table->file->ref_length,
                                             MEM_STRIP_BUF_SIZE);

    THD_STAGE_INFO(thd, stage_searching_rows_for_update);
    while (!(error=info.read_record()) && !thd->killed && !thd->is_error())
    {
      if (record_should_be_deleted(thd, table, select, explain, delete_history))
      {
        table->file->position(table->record[0]);
        if ((error= deltempfile->unique_add((char*) table->file->ref)))
          break;
        if (!--tmplimit && using_limit)
          break;
      }
    }
    end_read_record(&info);
    if (table->file->ha_index_or_rnd_end() || error > 0 ||
        deltempfile->get(table) ||
        init_read_record(&info, thd, table, 0, &deltempfile->sort, 0, 1, 0))
    {
      error= 1;
      goto terminate_delete;
    }
    delete_record= true;
  }

  /*
    From SQL2016, Part 2, 15.7 <Effect of deleting rows from base table>,
    General Rules, 8), we can conclude that DELETE FOR PORTION OF time performs
    0-2 INSERTS + DELETE. We can substitute INSERT+DELETE with one UPDATE, with
    a condition of no side effects. The side effect is possible if there is a
    BEFORE INSERT trigger, since it is the only one splitting DELETE and INSERT
    operations.
    Another possible side effect is related to tables of non-transactional
    engines, since UPDATE is anyway atomic, and DELETE+INSERT is not.

    This optimization is not possible for system-versioned table.
  */
  portion_of_time_through_update=
          !(table->triggers && table->triggers->has_triggers(TRG_EVENT_INSERT,
                                                             TRG_ACTION_BEFORE))
          && !table->versioned()
          && table->file->has_transactions();

  table->file->prepare_for_modify(table->versioned(VERS_TIMESTAMP) ||
                                  table_list->has_period(), true);
  DBUG_ASSERT(table->file->inited != handler::NONE);

  THD_STAGE_INFO(thd, stage_updating);
  fix_rownum_pointers(thd, thd->lex->current_select, &deleted);

  thd->get_stmt_da()->reset_current_row_for_warning(0);
  while (likely(!(error=info.read_record())) && likely(!thd->killed) &&
         likely(!thd->is_error()))
  {
    thd->get_stmt_da()->inc_current_row_for_warning();
    if (delete_while_scanning)
      delete_record= record_should_be_deleted(thd, table, select, explain,
                                              delete_history);
    if (delete_record)
    {
      bool trg_skip_row= false;

      if (!delete_history && table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                            TRG_ACTION_BEFORE, FALSE,
                                            &trg_skip_row))
      {
        error= 1;
        break;
      }

      if (trg_skip_row)
        continue;

      // no LIMIT / OFFSET
      if (returning && result->send_data(returning->item_list) < 0)
      {
        error=1;
        break;
      }

      if (table_list->has_period() && portion_of_time_through_update)
      {
        bool need_delete= true;
        error= update_portion_of_time(thd, table, table_list->period_conditions,
                                      &need_delete);
        if (likely(!error) && need_delete)
          error= table->delete_row();
      }
      else
      {
        error= table->delete_row();

        ha_rows rows_inserted;
        if (likely(!error) && table_list->has_period()
            && !portion_of_time_through_update)
          error= table->insert_portion_of_time(thd, table_list->period_conditions,
                                               &rows_inserted);
      }

      if (likely(!error))
      {
        deleted++;
        if (!delete_history && table->triggers &&
            table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                              TRG_ACTION_AFTER, false,
                                              nullptr))
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
                                 MYF(thd->lex->ignore ? ME_WARNING : 0));
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
    else if (likely(!thd->is_error()))
      table->file->unlock_row();  // Row failed selection, release lock on it
    else
      break;
  }
  thd->get_stmt_da()->reset_current_row_for_warning(1);

terminate_delete:
  killed_status= thd->killed;
  if (unlikely(killed_status != NOT_KILLED || thd->is_error()))
    error= 1;					// Aborted
  if (will_batch && unlikely((loc_error= table->file->end_bulk_delete())))
  {
    if (error != 1)
      table->file->print_error(loc_error,MYF(0));
    error=1;
  }
  THD_STAGE_INFO(thd, stage_end);
  end_read_record(&info);
  if (table_list->has_period())
    table->file->ha_release_auto_increment();
  if (options & OPTION_QUICK)
    (void) table->file->extra(HA_EXTRA_NORMAL);
  ANALYZE_STOP_TRACKING(thd, &explain->command_tracker);

cleanup:
  /*
    Invalidate the table in the query cache if something changed. This must
    be before binlog writing and ha_autocommit_...
  */
  if (deleted)
  {
    query_cache_invalidate3(thd, table_list, 1);
  }

  if (!thd->lex->current_select->leaf_tables_saved)
  {
    thd->lex->current_select->save_leaf_tables(thd);
    thd->lex->current_select->leaf_tables_saved= true;
    thd->lex->current_select->first_cond_optimization= false;
  }

  delete deltempfile;
  deltempfile=NULL;
  delete select;
  select= NULL;

  if (!transactional_table && deleted > 0)
    thd->transaction->stmt.modified_non_trans_table=
      thd->transaction->all.modified_non_trans_table= TRUE;

  /* See similar binlogging code in sql_update.cc, for comments */
  if (likely((error < 0) || thd->transaction->stmt.modified_non_trans_table ||
             thd->log_current_statement()))
  {
    if ((WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()) &&
        table->s->using_binlog())
    {
      int errcode= 0;
      if (error < 0)
        thd->clear_error();
      else
        errcode= query_error_code(thd, killed_status == NOT_KILLED);

      StatementBinlog stmt_binlog(thd, table->versioned(VERS_TRX_ID) ||
                                       thd->binlog_need_stmt_format(transactional_table));
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

      if (log_result > 0)
      {
        error=1;
      }
      else
        binlogged= 1;
    }
  }
  if (!binlogged)
    table->mark_as_not_binlogged();

  DBUG_ASSERT(transactional_table || !deleted || thd->transaction->stmt.modified_non_trans_table);
  
  if (likely(error < 0) ||
      (thd->lex->ignore && !thd->is_error() && !thd->is_fatal_error))
  {
    if (thd->lex->analyze_stmt)
      goto send_nothing_and_leave;

    thd->collect_unit_results(0, deleted);

    if (returning)
      result->send_eof();
    else
      my_ok(thd, deleted);
    DBUG_PRINT("info", ("%ld records deleted", (long) deleted));
  }
  delete file_sort;
  if (optimize_subqueries && select_lex->optimize_unflattened_subqueries(false))
    DBUG_RETURN(TRUE);
  free_underlaid_joins(thd, select_lex);
  if (table->file->pushed_cond)
    table->file->cond_pop();
  DBUG_RETURN(error >= 0 || thd->is_error());
  
  /* Special exits */
produce_explain_and_leave:
  /* 
    We come here for various "degenerate" query plans: impossible WHERE,
    no-partitions-used, impossible-range, etc.
  */
  if (!(query_plan.save_explain_delete_data(thd, thd->mem_root)))
    goto got_error;

send_nothing_and_leave:
  /* 
    ANALYZE DELETE jumps here. We can't send explain right here, because
    we might be using ANALYZE DELETE ...RETURNING, in which case we have 
    Protocol_discard active.
  */

  delete select;
  delete file_sort;
  if (!thd->is_error() && optimize_subqueries &&
      select_lex->optimize_unflattened_subqueries(false))
    DBUG_RETURN(TRUE);
  free_underlaid_joins(thd, select_lex);
  if (table->file->pushed_cond)
    table->file->cond_pop();

  DBUG_ASSERT(!return_error || thd->is_error() || thd->killed);
  DBUG_RETURN((return_error || thd->is_error() || thd->killed) ? 1 : 0);

got_error:
  return_error= 1;
  goto send_nothing_and_leave;
}


/***************************************************************************
  Delete multiple tables from join 
***************************************************************************/


extern "C" int refpos_order_cmp(void *arg, const void *a, const void *b)
{
  auto file= static_cast<handler *>(arg);
  return file->cmp_ref(static_cast<const uchar *>(a),
                       static_cast<const uchar *>(b));
}


multi_delete::multi_delete(THD *thd_arg,
                           TABLE_LIST *dt,
                           uint num_of_tables_arg)
  : select_result_interceptor(thd_arg),
    delete_tables(dt),
    deleted(0),
    found(0),
    table_count(num_of_tables_arg),
    error(0),
    do_delete(0),
    transactional_tables(0),
    normal_tables(0),
    error_handled(0)
{
  tmp_tables = thd->calloc<TABLE*>(table_count);
  tmp_table_param = thd->calloc<TMP_TABLE_PARAM>(table_count);
}


int
multi_delete::prepare(List<Item> &values, SELECT_LEX_UNIT *u)
{
  DBUG_ENTER("multi_delete::prepare");
  unit= u;
  do_delete= 1;
  THD_STAGE_INFO(thd, stage_deleting_from_main_table);
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
  multi_delete stores a rowid and new field values for every updated row in a
  temporary table (one temporary table per updated table).  These rowids are
  obtained via Item_temptable_rowid's by calling handler::position().  But if
  the join is resolved via a temp table, rowids cannot be obtained from
  handler::position() in the multi_update::send_data().  So, they're stored in
  the join's temp table (JOIN::add_fields_for_current_rowid()) and here we
  replace Item_temptable_rowid's (that would've done handler::position()) with
  Item_field's (that will simply take the corresponding field value from the
  temp table).
*/
int multi_delete::prepare2(JOIN *join)
{
  if (!join->need_tmp || !join->tmp_table_keep_current_rowid)
    return 0;
  delete_while_scanning= false;
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
  DBUG_ENTER("initialize_tables");

  if (unlikely((thd->variables.option_bits & OPTION_SAFE_UPDATES) &&
               error_if_full_join(join)))
    DBUG_RETURN(1);
  main_table=join->join_tab->table;

  table_map tables_to_delete_from= 0;
  delete_while_scanning= true;
  for (walk= delete_tables; walk; walk= walk->next_local)
  {
    TABLE_LIST *tbl= walk->correspondent_table->find_table_for_update();
    tables_to_delete_from|= tbl->table->map;

    /*
      Ensure that filesort re-reads the row from the engine before
      delete is called.
    */
    join->map2table[tbl->table->tablenr]->keep_current_rowid= true;

    if (delete_while_scanning &&
        unique_table(thd, tbl, join->tables_list, 0))
    {
      /*
        If the table we are going to delete from appears
        in join, we need to defer delete. So the delete
        doesn't interfere with the scanning of results.
      */
      delete_while_scanning= false;
    }
  }

  walk= delete_tables;
  uint index= 0;
  for (JOIN_TAB *tab= first_linear_tab(join, WITHOUT_BUSH_ROOTS,
                                       WITH_CONST_TABLES);
       tab;
       tab= next_linear_tab(join, tab, WITHOUT_BUSH_ROOTS))
  {
    if (!tab->bush_children && tab->table->map & tables_to_delete_from)
    {
      /* We are going to delete from this table */
      TABLE *tbl=walk->table=tab->table;
      TABLE_LIST *prior= walk;
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
      tbl->file->prepare_for_modify(tbl->versioned(VERS_TIMESTAMP), true);

      List<Item> temp_fields;
      tbl->prepare_for_position();
      join->map2table[tbl->tablenr]->keep_current_rowid= true;
      Item_temptable_rowid *item=
        new (thd->mem_root) Item_temptable_rowid(tbl);
      if (!item)
         DBUG_RETURN(1);
      item->fix_fields(thd, 0);
      if (temp_fields.push_back(item, thd->mem_root))
        DBUG_RETURN(1);
      /* Make an unique key over the first field to avoid duplicated updates */
      ORDER group;
      bzero((char*) &group, sizeof(group));
      group.direction= ORDER::ORDER_ASC;
      group.item= (Item**) temp_fields.head_ref();
      TMP_TABLE_PARAM *tmp_param;
      prior->shared = index;
      tmp_param= &tmp_table_param[prior->shared];
      tmp_param->init();
      tmp_param->tmp_name="update";
      tmp_param->field_count= temp_fields.elements;
      tmp_param->func_count= temp_fields.elements;
      calc_group_buffer(tmp_param, &group);
      tmp_tables[index]=create_tmp_table(thd, tmp_param, temp_fields,
                                       (ORDER*) &group, 0, 0,
                                       TMP_TABLE_ALL_COLUMNS, HA_POS_ERROR, &empty_clex_str);
      if (!tmp_tables[index])
        DBUG_RETURN(1);
      tmp_tables[index]->file->extra(HA_EXTRA_WRITE_CACHE);
      ++index;
    }
    else if ((tab->type != JT_SYSTEM && tab->type != JT_CONST) &&
             walk == delete_tables)
    {
      /*
        We are not deleting from the table we are scanning. In this
        case send_data() shouldn't delete any rows a we may touch
        the rows in the deleted table many times
      */
      delete_while_scanning= false;
    }
  }
  if (delete_while_scanning)
    table_being_deleted= delete_tables;
  if (init_ftfuncs(thd, thd->lex->current_select, 1))
    DBUG_RETURN(true);

  join->tmp_table_keep_current_rowid= TRUE;
  DBUG_RETURN(thd->is_fatal_error);
}


multi_delete::~multi_delete()
{
  for (TABLE_LIST *walk= delete_tables; walk; walk= walk->next_local)
  {
    TABLE *table= walk->table;
    if (!table)
      continue;
    table->no_keyread=0;
    table->no_cache= 0;
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
}


int multi_delete::send_data(List<Item> &values)
{
  int secure_counter= delete_while_scanning ? -1 : 0;
  TABLE_LIST *del_table;
  DBUG_ENTER("multi_delete::send_data");

  bool ignore= thd->lex->ignore;

  for (del_table= delete_tables;
       del_table;
       del_table= del_table->next_local, ++secure_counter)
  {
    TABLE *table= del_table->table;
    // DELETE and TRUNCATE don't affect SEQUENCE, so bail early
    if (table->file->ht->db_type == DB_TYPE_SEQUENCE)
      continue;

    /* Check if we are using outer join and we didn't find the row */
    if (table->status & (STATUS_NULL_ROW | STATUS_DELETED))
      continue;

    table->file->position(table->record[0]);
    ++found;

    if (secure_counter < 0)
    {
      bool trg_skip_row= false;

      /* We are scanning the current table */
      DBUG_ASSERT(del_table == table_being_deleted);
      if (table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                            TRG_ACTION_BEFORE, false,
                                            &trg_skip_row))
        DBUG_RETURN(1);

      if (trg_skip_row)
        continue;

      table->status|= STATUS_DELETED;

      error= table->delete_row();
      if (likely(!error))
      {
        deleted++;
        if (!table->file->has_transactions())
          thd->transaction->stmt.modified_non_trans_table= TRUE;
        if (table->triggers &&
            table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                              TRG_ACTION_AFTER, false,
                                              nullptr))
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
      const uint offset= del_table->shared;
      TABLE *tmp_table= tmp_tables[offset];
      if (copy_funcs(tmp_table_param[offset].items_to_copy, thd))
        DBUG_RETURN(1);
      /* rowid field is NULL if join tmp table has null row from outer join */
      if (tmp_table->field[0]->is_null())
        continue;
      error= tmp_table->file->ha_write_tmp_row(tmp_table->record[0]);
      if (error)
      {
          --found;
          if (error != HA_ERR_FOUND_DUPP_KEY &&
              error != HA_ERR_FOUND_DUPP_UNIQUE)
          {
              if (create_internal_tmp_table_from_heap(thd, tmp_table,
                                                      tmp_table_param[offset].start_recinfo,
                                                      &tmp_table_param[offset].recinfo,
                                                      error, 1, NULL))
              {
                  do_delete= 0;
                  DBUG_RETURN(1);			// Not a table_is_full error
              }
              found++;
          }
          error= 0;
      }
    }
  }
  DBUG_RETURN(0);
}


void multi_delete::abort_result_set()
{
  TABLE_LIST *cur_table;
  DBUG_ENTER("multi_delete::abort_result_set");

  /****************************************************************************

    NOTE: if you change here be aware that almost the same code is in
     multi_delete::send_eof().

  ***************************************************************************/

  /* the error was handled or nothing deleted and no side effects return */
  if (error_handled ||
      (!thd->transaction->stmt.modified_non_trans_table && !deleted))
    DBUG_VOID_RETURN;

  /* Something already deleted so we have to invalidate cache */
  if (deleted)
    query_cache_invalidate3(thd, delete_tables, 1);

  if (thd->transaction->stmt.modified_non_trans_table)
    thd->transaction->all.modified_non_trans_table= TRUE;
  thd->transaction->all.m_unsafe_rollback_flags|=
    (thd->transaction->stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);

  /*
    If rows from the first table only has been deleted and it is
    transactional, just do rollback.
    The same if all tables are transactional, regardless of where we are.
    In all other cases do attempt deletes ...
  */
  if (do_delete && normal_tables &&
      (table_being_deleted != delete_tables ||
       !table_being_deleted->table->file->has_transactions_and_rollback()))
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

  if (thd->transaction->stmt.modified_non_trans_table ||
      thd->log_current_statement())
  {
    /*
       there is only side effects; to binlog with the error
    */
    if (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      StatementBinlog stmt_binlog(thd, thd->binlog_need_stmt_format(transactional_tables));
      int errcode= query_error_code(thd, thd->killed == NOT_KILLED);
      /* possible error of writing binary log is ignored deliberately */
      (void) thd->binlog_query(THD::ROW_QUERY_TYPE,
                               thd->query(), thd->query_length(),
                               transactional_tables, FALSE, FALSE, errcode);
    }
  }
  /*
    Mark all temporay tables as not completely binlogged
    All future usage of these tables will enforce row level logging, which
    ensures that all future usage of them enforces row level logging.
  */
  for (cur_table= delete_tables; cur_table; cur_table= cur_table->next_local)
    cur_table->table->mark_as_not_binlogged();
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

  for (; table_being_deleted;
       table_being_deleted= table_being_deleted->next_local)
  {
    TABLE *table = table_being_deleted->table;
    // DELETE and TRUNCATE don't affect SEQUENCE, so bail early
    if (table->file->ht->db_type == DB_TYPE_SEQUENCE)
      continue;

    int local_error= rowid_table_deletes(table, thd->lex->ignore);

    if (unlikely(thd->killed) && likely(!local_error))
      DBUG_RETURN(1);

    if (unlikely(local_error == -1))            // End of file
      local_error= 0;

    if (unlikely(local_error))
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
int multi_delete::rowid_table_deletes(TABLE *table, bool ignore)
{
  int local_error= 0;
  ha_rows last_deleted= deleted;
  DBUG_ENTER("rowid_table_deletes");
  TABLE *err_table= nullptr;

  bool will_batch= !table->file->start_bulk_delete();
  TABLE *tmp_table= tmp_tables[table_being_deleted->shared];
  tmp_table->file->extra(HA_EXTRA_CACHE);  // Change to read cache
  if (unlikely((local_error= table->file->ha_rnd_init(0))))
  {
    err_table= table;
    goto err;
  }
  table->file->extra(HA_EXTRA_NO_CACHE);
  if (unlikely((local_error= tmp_table->file->ha_rnd_init(1))))
  {
    err_table= tmp_table;
    goto err;
  }

  while (!thd->killed)
  {
    if (unlikely((local_error=
                  tmp_table->file->ha_rnd_next(tmp_table->record[0]))))
    {
      if (local_error == HA_ERR_END_OF_FILE)
      {
        local_error= 0;
        break;
      }
      err_table= tmp_table;
      goto err;
    }

    DBUG_ASSERT(!tmp_table->field[0]->is_null());
    String rowid;
    tmp_table->field[0]->val_str(&rowid);
    if (unlikely((local_error= table->file->ha_rnd_pos(table->record[0],
                                                       (uchar*)rowid.ptr()))))
    {
      // Table aliased to itself had key deleted already
      continue;
    }

    bool trg_skip_row= false;

    if (table->triggers &&
        unlikely(table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                                   TRG_ACTION_BEFORE, FALSE,
                                                   &trg_skip_row)))
    {
      err_table= table;
      local_error= 1;
      break;
    }

    if (trg_skip_row)
      continue;

    local_error= table->delete_row();
    if (unlikely(local_error) && !ignore)
    {
      table->file->print_error(local_error, MYF(0));
      break;
    }

    /*
      Increase the reported number of deleted rows only if no error occurred
      during ha_delete_row.
      Also, don't execute the AFTER trigger if the row operation failed.
    */
    if (likely(!local_error))
    {
      deleted++;
      if (table->triggers &&
          table->triggers->process_triggers(thd, TRG_EVENT_DELETE,
                                            TRG_ACTION_AFTER, FALSE,
                                            nullptr))
      {
        err_table= table;
        local_error= 1;
        break;
      }
    }
  }
  if (will_batch)
  {
    int tmp_error= table->file->end_bulk_delete();
    if (unlikely(tmp_error) && !local_error)
    {
      local_error= tmp_error;
      table->file->print_error(local_error, MYF(0));
    }
  }
  if (last_deleted != deleted && !table->file->has_transactions_and_rollback())
    thd->transaction->stmt.modified_non_trans_table= TRUE;
err:
  if (err_table)
    err_table->file->print_error(local_error,MYF(ME_FATAL));
  if (tmp_table->file->inited == handler::init_stat::RND)
    tmp_table->file->ha_rnd_end();
  if (table->file->inited == handler::init_stat::RND)
    table->file->ha_rnd_end();
  DBUG_RETURN(local_error);
}


/*
  Send ok to the client

  return:  0 success
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

  /****************************************************************************

    NOTE: if you change here be aware that almost the same code is in
     multi_delete::abort_result_set().

  ***************************************************************************/

  if (thd->transaction->stmt.modified_non_trans_table)
    thd->transaction->all.modified_non_trans_table= TRUE;
  thd->transaction->all.m_unsafe_rollback_flags|=
    (thd->transaction->stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);

  /*
    We must invalidate the query cache before binlog writing and
    ha_autocommit_...
  */
  if (deleted)
  {
    query_cache_invalidate3(thd, delete_tables, 1);
  }
  if (likely((local_error == 0) ||
             thd->transaction->stmt.modified_non_trans_table) ||
      thd->log_current_statement())
  {
    if(WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open())
    {
      int errcode= 0;
      if (likely(local_error == 0))
        thd->clear_error();
      else
        errcode= query_error_code(thd, killed_status == NOT_KILLED);
      thd->used|= THD::THREAD_SPECIFIC_USED;
      StatementBinlog stmt_binlog(thd, thd->binlog_need_stmt_format(transactional_tables));
      if (unlikely(thd->binlog_query(THD::ROW_QUERY_TYPE,
                                     thd->query(), thd->query_length(),
                                     transactional_tables, FALSE, FALSE,
                                     errcode) > 0) &&
          !normal_tables)
      {
	local_error=1;  // Log write failed: roll back the SQL statement
      }
    }
  }
  if (unlikely(local_error != 0))
  {
    error_handled= TRUE; // to force early leave from ::abort_result_set()
    if (thd->killed == NOT_KILLED && !thd->get_stmt_da()->is_set())
    {
      /*
        No error message was sent and query was not killed (in which case
        mysql_execute_command() will send the error mesage).
      */
      ::my_ok(thd, deleted);  // Ends the DELETE statement
    }
    return 1;
  }

  if (likely(!thd->lex->analyze_stmt))
  {
    ::my_ok(thd, deleted);
  }
  return 0;
}


/**
  @brief Remove ORDER BY from DELETE if it's used without limit clause
*/

void  Sql_cmd_delete::remove_order_by_without_limit(THD *thd)
{
  SELECT_LEX *const select_lex = thd->lex->first_select_lex();
  if (select_lex->order_list.elements &&
      !select_lex->limit_params.select_limit)
    select_lex->order_list.empty();
}


/**
  @brief Check whether processing to multi-table delete is prohibited

  @param thd  global context the processed statement
  @returns true if processing as multitable is prohibited, false otherwise

  @todo
  Introduce handler level flag for storage engines that would prohibit
  such conversion for any single-table delete.
*/

bool Sql_cmd_delete::processing_as_multitable_delete_prohibited(THD *thd)
{
  return
    (thd->lex->has_returning());
}


/**
  @brief Perform precheck of table privileges for delete statements

  @param thd  global context the processed statement
  @returns false on success, true on error
*/

bool Sql_cmd_delete::precheck(THD *thd)
{
  if (!multitable)
  {
    if (delete_precheck(thd, lex->query_tables))
      return true;
  }
  else
  {
    if (multi_delete_precheck(thd, lex->query_tables))
      return true;

    SELECT_LEX *select_lex= lex->first_select_lex();
    /* condition will be TRUE on SP re-excuting */
    if (select_lex->item_list.elements != 0)
      select_lex->item_list.empty();
    if (add_item_to_list(thd, new (thd->mem_root) Item_null(thd)))
      return true;
  }

#ifdef WITH_WSREP
  WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_UPDATE_DELETE);
#endif

  return false;

#ifdef WITH_WSREP
wsrep_error_label:
#endif
  return true;
}


/**
  @brief Perform context analysis for delete statements

  @param thd  global context the processed statement
  @returns false on success, true on error

  @note
  The main bulk of the context analysis actions for a delete statement
  is performed by a call of JOIN::prepare().
*/

bool Sql_cmd_delete::prepare_inner(THD *thd)
{
  int err= 0;
  TABLE_LIST *target_tbl;
  JOIN *join;
  SELECT_LEX *const select_lex = thd->lex->first_select_lex();
  TABLE_LIST *const table_list = select_lex->get_table_list();
  TABLE_LIST *aux_tables= thd->lex->auxiliary_table_list.first;
  ulonglong select_options= select_lex->options;
  bool free_join= 1;
  SELECT_LEX *returning= thd->lex->has_returning() ? thd->lex->returning() : 0;
  const bool delete_history= table_list->vers_conditions.delete_history;
  DBUG_ASSERT(!(delete_history && table_list->period_conditions.is_set()));

  DBUG_ENTER("Sql_cmd_delete::prepare_inner");

  (void) read_statistics_for_tables_if_needed(thd, table_list);

  THD_STAGE_INFO(thd, stage_init_update);

  {
    if (mysql_handle_derived(lex, DT_INIT))
      DBUG_RETURN(TRUE);
    if (mysql_handle_derived(lex, DT_MERGE_FOR_INSERT))
      DBUG_RETURN(TRUE);
    if (mysql_handle_derived(lex, DT_PREPARE))
      DBUG_RETURN(TRUE);
  }

  if (!(result= new (thd->mem_root) multi_delete(thd, aux_tables,
                                                 lex->table_count_update)))
  {
    DBUG_RETURN(TRUE);
  }

  table_list->delete_while_scanning= true;

  if (!multitable && !table_list->single_table_updatable())
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "DELETE");
    DBUG_RETURN(TRUE);
  }

  if (!multitable && (!table_list->table || !table_list->table->is_created()))
  {
      my_error(ER_VIEW_DELETE_MERGE_VIEW, MYF(0),
	       table_list->view_db.str, table_list->view_name.str);
    DBUG_RETURN(TRUE);
  }

  if (setup_tables_and_check_access(thd, &select_lex->context,
                                    &select_lex->top_join_list,
                                    table_list, select_lex->leaf_tables,
                                    false, DELETE_ACL, SELECT_ACL, true))
    DBUG_RETURN(TRUE);

  if (setup_tables(thd, &select_lex->context, &select_lex->top_join_list,
                   table_list, select_lex->leaf_tables, false, false))
    DBUG_RETURN(TRUE);

  if (!multitable)
  {
    if (select_lex->index_hints || table_list->index_hints)
    {
      if (!processing_as_multitable_delete_prohibited(thd))
      {
        multitable= true;
      }
      else
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            WARN_INDEX_HINTS_IGNORED,
                            "%s", ER_THD(thd, WARN_INDEX_HINTS_IGNORED));
      }
    }
    if (table_list->vers_conditions.is_set() && table_list->is_view_or_derived())
    {
      my_error(ER_IT_IS_A_VIEW, MYF(0), table_list->table_name.str);
      DBUG_RETURN(true);
    }

    if (!multitable)
    {
      TABLE_LIST *update_source_table= 0;
      if (((update_source_table=unique_table(thd, table_list,
                                             table_list->next_global, 0)) ||
          table_list->is_multitable()))
      {
        DBUG_ASSERT(update_source_table || table_list->view != 0);
        if (!table_list->is_multitable() &&
            !processing_as_multitable_delete_prohibited(thd))
	{
          multitable= true;
          remove_order_by_without_limit(thd);
        }
      }
    }

    if (table_list->has_period())
    {
      if (table_list->is_view_or_derived())
      {
        my_error(ER_IT_IS_A_VIEW, MYF(0), table_list->table_name.str);
        DBUG_RETURN(true);
      }

      if (select_lex->period_setup_conds(thd, table_list))
        DBUG_RETURN(true);
    }

    if (select_lex->vers_setup_conds(thd, table_list))
      DBUG_RETURN(TRUE);
    /*
        Application-time periods: if FOR PORTION OF ... syntax used, DELETE
        statement could issue delete_row's mixed with write_row's. This causes
        problems for myisam and corrupts table, if deleting while scanning.
     */
    if (table_list->has_period()
        || unique_table(thd, table_list, table_list->next_global, 0))
    table_list->delete_while_scanning= false;
  }

  {
    if (thd->lex->describe)
      select_options|= SELECT_DESCRIBE;

    /*
      When in EXPLAIN, delay deleting the joins so that they are still
      available when we're producing EXPLAIN EXTENDED warning text.
    */
    if (select_options & SELECT_DESCRIBE)
      free_join= 0;
    select_options|=
      SELECT_NO_JOIN_CACHE | SELECT_NO_UNLOCK | OPTION_SETUP_TABLES_DONE;

    if (!(join= new (thd->mem_root) JOIN(thd, empty_list,
                                         select_options, result)))
	DBUG_RETURN(TRUE);
    THD_STAGE_INFO(thd, stage_init);
    select_lex->join= join;
    thd->lex->used_tables=0;
    if ((err= join->prepare(table_list, select_lex->where,
                            select_lex->order_list.elements,
                            select_lex->order_list.first,
                            false, NULL, NULL, NULL,
                            select_lex, &lex->unit)))

    {
      goto err;
    }

    if (!multitable &&
        select_lex->sj_subselects.elements)
      multitable= true;
  }

  if (multitable)
  {
    /*
      Multi-delete can't be constructed over-union => we always have
      single SELECT on top and have to check underlying SELECTs of it
    */
    lex->first_select_lex()->set_unique_exclude();
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
                 target_tbl->table_name.str, "DELETE");
        DBUG_RETURN(TRUE);
      }
    }

    /*
      Reset the exclude flag to false so it doesn't interfere
      with further calls to unique_table
    */
    lex->first_select_lex()->exclude_from_table_unique_test= FALSE;
  }

  if (!multitable && table_list->has_period())
  {
    if (!table_list->period_conditions.start.item->const_item()
        || !table_list->period_conditions.end.item->const_item())
    {
      my_error(ER_NOT_CONSTANT_EXPRESSION, MYF(0), "FOR PORTION OF");
      DBUG_RETURN(true);
    }
  }

  if (delete_history)
    table_list->table->vers_write= false;

  if (setup_returning_fields(thd, table_list) ||
      setup_ftfuncs(select_lex))
    goto err;

  free_join= false;

  if (returning)
    (void) result->prepare(returning->item_list, NULL);

err:

  if (free_join)
  {
    THD_STAGE_INFO(thd, stage_end);
    err|= (int)(select_lex->cleanup());
    DBUG_RETURN(err || thd->is_error());
  }
  DBUG_RETURN(err);

}


/**
  @brief Perform optimization and execution actions needed for deletes

  @param thd  global context the processed statement
  @returns false on success, true on error
*/

bool Sql_cmd_delete::execute_inner(THD *thd)
{
  Running_stmt_guard guard(thd, active_dml_stmt::DELETING_STMT);

  if (!multitable)
  {
    if (lex->has_returning())
    {
      select_result *sel_result= NULL;
      delete result;
      /* This is DELETE ... RETURNING.  It will return output to the client */
      if (thd->lex->analyze_stmt)
      {
        /*
          Actually, it is ANALYZE .. DELETE .. RETURNING. We need to produce
          output and then discard it.
        */
        sel_result= new (thd->mem_root) select_send_analyze(thd);
        save_protocol= thd->protocol;
        thd->protocol= new Protocol_discard(thd);
      }
      else
      {
        if (!lex->result && !(sel_result= new (thd->mem_root) select_send(thd)))
          return true;
      }
      result= lex->result ? lex->result : sel_result;
    }
  }

  bool res= multitable ? Sql_cmd_dml::execute_inner(thd)
                         : delete_from_single_table(thd);

  res|= thd->is_error();

  if (save_protocol)
  {
    delete thd->protocol;
    thd->protocol= save_protocol;
  }
  {
    if (unlikely(res))
    {
      if (multitable)
        result->abort_result_set();
    }
    else
    {
      if (thd->lex->describe || thd->lex->analyze_stmt)
      {
        bool extended= thd->lex->describe & DESCRIBE_EXTENDED;
        res= thd->lex->explain->send_explain(thd, extended);
      }
    }
  }

  if (result)
  {
    /* In single table case, this->deleted set by delete_from_single_table */
    if (res && multitable)
      deleted= ((multi_delete*)get_result())->num_deleted();
    res= false;
    delete result;
  }

  status_var_add(thd->status_var.rows_sent, thd->get_sent_row_count());
  return res;
}
