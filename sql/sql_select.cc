/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB Corporation.

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

/**
  @file

  @brief
  mysql_select and join optimization


  @defgroup Query_Optimizer  Query Optimizer
  @{
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_select.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_table.h"                          // primary_key_name
#include "probes_mysql.h"
#include "key.h"                 // key_copy, key_cmp, key_cmp_if_same
#include "lock.h"                // mysql_unlock_some_tables,
                                 // mysql_unlock_read_tables
#include "sql_show.h"            // append_identifier
#include "sql_base.h"            // setup_wild, setup_fields, fill_record
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_partition.h"       // make_used_partitions_str
#include "sql_acl.h"             // *_ACL
#include "sql_test.h"            // print_where, print_keyuse_array,
                                 // print_sjm, print_plan, TEST_join
#include "records.h"             // init_read_record, end_read_record
#include "filesort.h"            // filesort_free_buffers
#include "sql_union.h"           // mysql_union
#include "opt_subselect.h"
#include "sql_derived.h"
#include "sql_statistics.h"
#include "sql_cte.h"
#include "sql_window.h"
#include "tztime.h"

#include "debug_sync.h"          // DEBUG_SYNC
#include <m_ctype.h>
#include <my_bit.h>
#include <hash.h>
#include <ft_global.h>
#include "sys_vars_shared.h"
#include "sp_head.h"
#include "sp_rcontext.h"

/*
  A key part number that means we're using a fulltext scan.

  In order not to confuse it with regular equalities, we need to pick
  a number that's greater than MAX_REF_PARTS.

  Hash Join code stores field->field_index in KEYUSE::keypart, so the 
  number needs to be bigger than MAX_FIELDS, also.

  CAUTION: sql_test.cc has its own definition of FT_KEYPART.
*/
#define FT_KEYPART   (MAX_FIELDS+10)

const char *join_type_str[]={ "UNKNOWN","system","const","eq_ref","ref",
			      "MAYBE_REF","ALL","range","index","fulltext",
			      "ref_or_null","unique_subquery","index_subquery",
                              "index_merge", "hash_ALL", "hash_range",
                              "hash_index", "hash_index_merge" };

LEX_CSTRING group_key= {STRING_WITH_LEN("group_key")};
LEX_CSTRING distinct_key= {STRING_WITH_LEN("distinct_key")};

struct st_sargable_param;

static bool make_join_statistics(JOIN *join, List<TABLE_LIST> &leaves, 
                                 DYNAMIC_ARRAY *keyuse);
static bool update_ref_and_keys(THD *thd, DYNAMIC_ARRAY *keyuse,
                                JOIN_TAB *join_tab,
                                uint tables, COND *conds,
                                table_map table_map, SELECT_LEX *select_lex,
                                SARGABLE_PARAM **sargables);
static int sort_keyuse(KEYUSE *a,KEYUSE *b);
static bool are_tables_local(JOIN_TAB *jtab, table_map used_tables);
static bool create_ref_for_key(JOIN *join, JOIN_TAB *j, KEYUSE *org_keyuse,
			       bool allow_full_scan, table_map used_tables);
static void optimize_straight_join(JOIN *join, table_map join_tables);
static bool greedy_search(JOIN *join, table_map remaining_tables,
                          uint depth, uint prune_level,
                          uint use_cond_selectivity);
static bool best_extension_by_limited_search(JOIN *join,
                                             table_map remaining_tables,
                                             uint idx, double record_count,
                                             double read_time, uint depth,
                                             uint prune_level,
                                             uint use_cond_selectivity);
static uint determine_search_depth(JOIN* join);
C_MODE_START
static int join_tab_cmp(const void *dummy, const void* ptr1, const void* ptr2);
static int join_tab_cmp_straight(const void *dummy, const void* ptr1, const void* ptr2);
static int join_tab_cmp_embedded_first(const void *emb, const void* ptr1, const void *ptr2);
C_MODE_END
static uint cache_record_length(JOIN *join,uint index);
static store_key *get_store_key(THD *thd,
				KEYUSE *keyuse, table_map used_tables,
				KEY_PART_INFO *key_part, uchar *key_buff,
				uint maybe_null);
static bool make_outerjoin_info(JOIN *join);
static Item*
make_cond_after_sjm(THD *thd, Item *root_cond, Item *cond, table_map tables,
                    table_map sjm_tables, bool inside_or_clause);
static bool make_join_select(JOIN *join,SQL_SELECT *select,COND *item);
static void revise_cache_usage(JOIN_TAB *join_tab);
static bool make_join_readinfo(JOIN *join, ulonglong options, uint no_jbuf_after);
static bool only_eq_ref_tables(JOIN *join, ORDER *order, table_map tables);
static void update_depend_map(JOIN *join);
static void update_depend_map_for_order(JOIN *join, ORDER *order);
static ORDER *remove_const(JOIN *join,ORDER *first_order,COND *cond,
			   bool change_list, bool *simple_order);
static int return_zero_rows(JOIN *join, select_result *res, 
                            List<TABLE_LIST> &tables,
                            List<Item> &fields, bool send_row,
                            ulonglong select_options, const char *info,
                            Item *having, List<Item> &all_fields);
static COND *build_equal_items(JOIN *join, COND *cond,
                               COND_EQUAL *inherited,
                               List<TABLE_LIST> *join_list,
                               bool ignore_on_conds,
                               COND_EQUAL **cond_equal_ref,
                               bool link_equal_fields= FALSE);
static COND* substitute_for_best_equal_field(THD *thd, JOIN_TAB *context_tab,
                                             COND *cond,
                                             COND_EQUAL *cond_equal,
                                             void *table_join_idx);
static COND *simplify_joins(JOIN *join, List<TABLE_LIST> *join_list,
                            COND *conds, bool top, bool in_sj);
static bool check_interleaving_with_nj(JOIN_TAB *next);
static void restore_prev_nj_state(JOIN_TAB *last);
static uint reset_nj_counters(JOIN *join, List<TABLE_LIST> *join_list);
static uint build_bitmap_for_nested_joins(List<TABLE_LIST> *join_list,
                                          uint first_unused);

static COND *optimize_cond(JOIN *join, COND *conds,
                           List<TABLE_LIST> *join_list,
                           bool ignore_on_conds,
                           Item::cond_result *cond_value, 
                           COND_EQUAL **cond_equal,
                           int flags= 0);
bool const_expression_in_where(COND *conds,Item *item, Item **comp_item);
static int do_select(JOIN *join, Procedure *procedure);

static enum_nested_loop_state evaluate_join_record(JOIN *, JOIN_TAB *, int);
static enum_nested_loop_state
evaluate_null_complemented_join_record(JOIN *join, JOIN_TAB *join_tab);
static enum_nested_loop_state
end_send(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
static enum_nested_loop_state
end_write(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
static enum_nested_loop_state
end_update(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
static enum_nested_loop_state
end_unique_update(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);

static int join_read_const_table(THD *thd, JOIN_TAB *tab, POSITION *pos);
static int join_read_system(JOIN_TAB *tab);
static int join_read_const(JOIN_TAB *tab);
static int join_read_key(JOIN_TAB *tab);
static void join_read_key_unlock_row(st_join_table *tab);
static int join_read_always_key(JOIN_TAB *tab);
static int join_read_last_key(JOIN_TAB *tab);
static int join_no_more_records(READ_RECORD *info);
static int join_read_next(READ_RECORD *info);
static int join_init_quick_read_record(JOIN_TAB *tab);
static int test_if_quick_select(JOIN_TAB *tab);
static bool test_if_use_dynamic_range_scan(JOIN_TAB *join_tab);
static int join_read_first(JOIN_TAB *tab);
static int join_read_next(READ_RECORD *info);
static int join_read_next_same(READ_RECORD *info);
static int join_read_last(JOIN_TAB *tab);
static int join_read_prev_same(READ_RECORD *info);
static int join_read_prev(READ_RECORD *info);
static int join_ft_read_first(JOIN_TAB *tab);
static int join_ft_read_next(READ_RECORD *info);
int join_read_always_key_or_null(JOIN_TAB *tab);
int join_read_next_same_or_null(READ_RECORD *info);
static COND *make_cond_for_table(THD *thd, Item *cond,table_map table,
                                 table_map used_table,
                                 int join_tab_idx_arg,
                                 bool exclude_expensive_cond,
                                 bool retain_ref_cond);
static COND *make_cond_for_table_from_pred(THD *thd, Item *root_cond,
                                           Item *cond,
                                           table_map tables,
                                           table_map used_table,
                                           int join_tab_idx_arg,
                                           bool exclude_expensive_cond,
                                           bool retain_ref_cond,
                                           bool is_top_and_level);

static Item* part_of_refkey(TABLE *form,Field *field);
uint find_shortest_key(TABLE *table, const key_map *usable_keys);
static bool test_if_cheaper_ordering(const JOIN_TAB *tab,
                                     ORDER *order, TABLE *table,
                                     key_map usable_keys, int key,
                                     ha_rows select_limit,
                                     int *new_key, int *new_key_direction,
                                     ha_rows *new_select_limit,
                                     uint *new_used_key_parts= NULL,
                                     uint *saved_best_key_parts= NULL);
static int test_if_order_by_key(JOIN *join,
                                ORDER *order, TABLE *table, uint idx,
				uint *used_key_parts= NULL);
static bool test_if_skip_sort_order(JOIN_TAB *tab,ORDER *order,
				    ha_rows select_limit, bool no_changes,
                                    const key_map *map);
static bool list_contains_unique_index(TABLE *table,
                          bool (*find_func) (Field *, void *), void *data);
static bool find_field_in_item_list (Field *field, void *data);
static bool find_field_in_order_list (Field *field, void *data);
int create_sort_index(THD *thd, JOIN *join, JOIN_TAB *tab, Filesort *fsort);
static int remove_dup_with_compare(THD *thd, TABLE *entry, Field **field,
				   Item *having);
static int remove_dup_with_hash_index(THD *thd,TABLE *table,
				      uint field_count, Field **first_field,
				      ulong key_length,Item *having);
static bool cmp_buffer_with_ref(THD *thd, TABLE *table, TABLE_REF *tab_ref);
static bool setup_new_fields(THD *thd, List<Item> &fields,
			     List<Item> &all_fields, ORDER *new_order);
static ORDER *create_distinct_group(THD *thd, Ref_ptr_array ref_pointer_array,
                                    ORDER *order, List<Item> &fields,
                                    List<Item> &all_fields,
				    bool *all_order_by_fields_used);
static bool test_if_subpart(ORDER *a,ORDER *b);
static TABLE *get_sort_by_table(ORDER *a,ORDER *b,List<TABLE_LIST> &tables, 
                                table_map const_tables);
static void calc_group_buffer(JOIN *join,ORDER *group);
static bool make_group_fields(JOIN *main_join, JOIN *curr_join);
static bool alloc_group_fields(JOIN *join,ORDER *group);
// Create list for using with tempory table
static bool change_to_use_tmp_fields(THD *thd, Ref_ptr_array ref_pointer_array,
				     List<Item> &new_list1,
				     List<Item> &new_list2,
				     uint elements, List<Item> &items);
// Create list for using with tempory table
static bool change_refs_to_tmp_fields(THD *thd, Ref_ptr_array ref_pointer_array,
				      List<Item> &new_list1,
				      List<Item> &new_list2,
				      uint elements, List<Item> &items);
static void init_tmptable_sum_functions(Item_sum **func);
static void update_tmptable_sum_func(Item_sum **func,TABLE *tmp_table);
static void copy_sum_funcs(Item_sum **func_ptr, Item_sum **end);
static bool add_ref_to_table_cond(THD *thd, JOIN_TAB *join_tab);
static bool setup_sum_funcs(THD *thd, Item_sum **func_ptr);
static bool prepare_sum_aggregators(Item_sum **func_ptr, bool need_distinct);
static bool init_sum_functions(Item_sum **func, Item_sum **end);
static bool update_sum_func(Item_sum **func);
static void select_describe(JOIN *join, bool need_tmp_table,bool need_order,
			    bool distinct, const char *message=NullS);
static void add_group_and_distinct_keys(JOIN *join, JOIN_TAB *join_tab);
static uint make_join_orderinfo(JOIN *join);
static bool generate_derived_keys(DYNAMIC_ARRAY *keyuse_array);

Item_equal *find_item_equal(COND_EQUAL *cond_equal, Field *field,
                            bool *inherited_fl);
JOIN_TAB *first_depth_first_tab(JOIN* join);
JOIN_TAB *next_depth_first_tab(JOIN* join, JOIN_TAB* tab);

static JOIN_TAB *next_breadth_first_tab(JOIN_TAB *first_top_tab,
                                        uint n_top_tabs_count, JOIN_TAB *tab);
static bool find_order_in_list(THD *, Ref_ptr_array, TABLE_LIST *, ORDER *,
                               List<Item> &, List<Item> &, bool, bool, bool);

static double table_cond_selectivity(JOIN *join, uint idx, JOIN_TAB *s,
                                     table_map rem_tables);
void set_postjoin_aggr_write_func(JOIN_TAB *tab);

static Item **get_sargable_cond(JOIN *join, TABLE *table);

bool is_eq_cond_injected_for_split_opt(Item_func_eq *eq_item);

#ifndef DBUG_OFF

/*
  SHOW EXPLAIN testing: wait for, and serve n_calls APC requests.
*/
void dbug_serve_apcs(THD *thd, int n_calls)
{
  const char *save_proc_info= thd->proc_info;
  
  /* Busy-wait for n_calls APC requests to arrive and be processed */
  int n_apcs= thd->apc_target.n_calls_processed + n_calls;
  while (thd->apc_target.n_calls_processed < n_apcs)
  {
    /* This is so that mysqltest knows we're ready to serve requests: */
    thd_proc_info(thd, "show_explain_trap");
    my_sleep(30000);
    thd_proc_info(thd, save_proc_info);
    if (unlikely(thd->check_killed(1)))
      break;
  }
}


/*
  Debugging: check if @name=value, comparing as integer

  Intended usage:
  
  DBUG_EXECUTE_IF("show_explain_probe_2", 
                     if (dbug_user_var_equals_int(thd, "select_id", select_id)) 
                        dbug_serve_apcs(thd, 1);
                 );

*/

bool dbug_user_var_equals_int(THD *thd, const char *name, int value)
{
  user_var_entry *var;
  LEX_CSTRING varname= { name, strlen(name)};
  if ((var= get_variable(&thd->user_vars, &varname, FALSE)))
  {
    bool null_value;
    longlong var_value= var->val_int(&null_value);
    if (!null_value && var_value == value)
      return TRUE;
  }
  return FALSE;
}
#endif


/**
  This handles SELECT with and without UNION.
*/

bool handle_select(THD *thd, LEX *lex, select_result *result,
                   ulong setup_tables_done_option)
{
  bool res;
  SELECT_LEX *select_lex = &lex->select_lex;
  DBUG_ENTER("handle_select");
  MYSQL_SELECT_START(thd->query());

  if (select_lex->master_unit()->is_unit_op() ||
      select_lex->master_unit()->fake_select_lex)
    res= mysql_union(thd, lex, result, &lex->unit, setup_tables_done_option);
  else
  {
    SELECT_LEX_UNIT *unit= &lex->unit;
    unit->set_limit(unit->global_parameters());
    /*
      'options' of mysql_select will be set in JOIN, as far as JOIN for
      every PS/SP execution new, we will not need reset this flag if 
      setup_tables_done_option changed for next rexecution
    */
    res= mysql_select(thd,
		      select_lex->table_list.first,
		      select_lex->with_wild, select_lex->item_list,
		      select_lex->where,
		      select_lex->order_list.elements +
		      select_lex->group_list.elements,
		      select_lex->order_list.first,
		      select_lex->group_list.first,
		      select_lex->having,
		      lex->proc_list.first,
		      select_lex->options | thd->variables.option_bits |
                      setup_tables_done_option,
		      result, unit, select_lex);
  }
  DBUG_PRINT("info",("res: %d  report_error: %d", res,
		     thd->is_error()));
  res|= thd->is_error();
  if (unlikely(res))
    result->abort_result_set();
  if (unlikely(thd->killed == ABORT_QUERY && !thd->no_errors))
  {
    /*
      If LIMIT ROWS EXAMINED interrupted query execution, issue a warning,
      continue with normal processing and produce an incomplete query result.
    */
    bool saved_abort_on_warning= thd->abort_on_warning;
    thd->abort_on_warning= false;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_QUERY_EXCEEDED_ROWS_EXAMINED_LIMIT,
                        ER_THD(thd, ER_QUERY_EXCEEDED_ROWS_EXAMINED_LIMIT),
                        thd->accessed_rows_and_keys,
                        thd->lex->limit_rows_examined->val_uint());
    thd->abort_on_warning= saved_abort_on_warning;
    thd->reset_killed();
  }
  /* Disable LIMIT ROWS EXAMINED after query execution. */
  thd->lex->limit_rows_examined_cnt= ULONGLONG_MAX;

  MYSQL_SELECT_DONE((int) res, (ulong) thd->limit_found_rows);
  DBUG_RETURN(res);
}


/**
  Fix fields referenced from inner selects.

  @param thd               Thread handle
  @param all_fields        List of all fields used in select
  @param select            Current select
  @param ref_pointer_array Array of references to Items used in current select
  @param group_list        GROUP BY list (is NULL by default)

  @details
    The function serves 3 purposes

    - adds fields referenced from inner query blocks to the current select list

    - Decides which class to use to reference the items (Item_ref or
      Item_direct_ref)

    - fixes references (Item_ref objects) to these fields.

    If a field isn't already on the select list and the ref_pointer_array
    is provided then it is added to the all_fields list and the pointer to
    it is saved in the ref_pointer_array.

    The class to access the outer field is determined by the following rules:

    -#. If the outer field isn't used under an aggregate function then the
        Item_ref class should be used.

    -#. If the outer field is used under an aggregate function and this
        function is, in turn, aggregated in the query block where the outer
        field was resolved or some query nested therein, then the
        Item_direct_ref class should be used. Also it should be used if we are
        grouping by a subquery that references this outer field.

    The resolution is done here and not at the fix_fields() stage as
    it can be done only after aggregate functions are fixed and pulled up to
    selects where they are to be aggregated.

    When the class is chosen it substitutes the original field in the
    Item_outer_ref object.

    After this we proceed with fixing references (Item_outer_ref objects) to
    this field from inner subqueries.

  @return Status
  @retval true An error occurred.
  @retval false OK.
 */

bool
fix_inner_refs(THD *thd, List<Item> &all_fields, SELECT_LEX *select,
               Ref_ptr_array ref_pointer_array)
{
  Item_outer_ref *ref;

  /*
    Mark the references from  the inner_refs_list that are occurred in
    the group by expressions. Those references will contain direct
    references to the referred fields. The markers are set in 
    the found_in_group_by field of the references from the list.
  */
  List_iterator_fast <Item_outer_ref> ref_it(select->inner_refs_list);
  for (ORDER *group= select->join->group_list; group;  group= group->next)
  {
    (*group->item)->walk(&Item::check_inner_refs_processor, TRUE, &ref_it);
  } 
    
  while ((ref= ref_it++))
  {
    bool direct_ref= false;
    Item *item= ref->outer_ref;
    Item **item_ref= ref->ref;
    Item_ref *new_ref;
    /*
      TODO: this field item already might be present in the select list.
      In this case instead of adding new field item we could use an
      existing one. The change will lead to less operations for copying fields,
      smaller temporary tables and less data passed through filesort.
    */
    if (!ref_pointer_array.is_null() && !ref->found_in_select_list)
    {
      int el= all_fields.elements;
      ref_pointer_array[el]= item;
      /* Add the field item to the select list of the current select. */
      all_fields.push_front(item, thd->mem_root);
      /*
        If it's needed reset each Item_ref item that refers this field with
        a new reference taken from ref_pointer_array.
      */
      item_ref= &ref_pointer_array[el];
    }

    if (ref->in_sum_func)
    {
      Item_sum *sum_func;
      if (ref->in_sum_func->nest_level > select->nest_level)
        direct_ref= TRUE;
      else
      {
        for (sum_func= ref->in_sum_func; sum_func &&
             sum_func->aggr_level >= select->nest_level;
             sum_func= sum_func->in_sum_func)
        {
          if (sum_func->aggr_level == select->nest_level)
          {
            direct_ref= TRUE;
            break;
          }
        }
      }
    }
    else if (ref->found_in_group_by)
      direct_ref= TRUE;

    new_ref= direct_ref ?
              new (thd->mem_root) Item_direct_ref(thd, ref->context, item_ref, ref->table_name,
                          &ref->field_name, ref->alias_name_used) :
              new (thd->mem_root) Item_ref(thd, ref->context, item_ref, ref->table_name,
                          &ref->field_name, ref->alias_name_used);
    if (!new_ref)
      return TRUE;
    ref->outer_ref= new_ref;
    ref->ref= &ref->outer_ref;

    if (ref->fix_fields_if_needed(thd, 0))
      return TRUE;
    thd->lex->used_tables|= item->used_tables();
    thd->lex->current_select->select_list_tables|= item->used_tables();
  }
  return false;
}

/**
   The following clauses are redundant for subqueries:

   DISTINCT
   GROUP BY   if there are no aggregate functions and no HAVING
              clause

   Because redundant clauses are removed both from JOIN and
   select_lex, the removal is permanent. Thus, it only makes sense to
   call this function for normal queries and on first execution of
   SP/PS

   @param subq_select_lex   select_lex that is part of a subquery 
                            predicate. This object and the associated 
                            join is modified.
*/

static
void remove_redundant_subquery_clauses(st_select_lex *subq_select_lex)
{
  DBUG_ENTER("remove_redundant_subquery_clauses");
  Item_subselect *subq_predicate= subq_select_lex->master_unit()->item;
  /*
    The removal should happen for IN, ALL, ANY and EXISTS subqueries,
    which means all but single row subqueries. Example single row
    subqueries: 
       a) SELECT * FROM t1 WHERE t1.a = (<single row subquery>) 
       b) SELECT a, (<single row subquery) FROM t1
   */
  if (subq_predicate->substype() == Item_subselect::SINGLEROW_SUBS)
    DBUG_VOID_RETURN;

  /* A subquery that is not single row should be one of IN/ALL/ANY/EXISTS. */
  DBUG_ASSERT (subq_predicate->substype() == Item_subselect::EXISTS_SUBS ||
               subq_predicate->is_in_predicate());

  if (subq_select_lex->options & SELECT_DISTINCT)
  {
    subq_select_lex->join->select_distinct= false;
    subq_select_lex->options&= ~SELECT_DISTINCT;
    DBUG_PRINT("info", ("DISTINCT removed"));
  }

  /*
    Remove GROUP BY if there are no aggregate functions and no HAVING
    clause
  */
  if (subq_select_lex->group_list.elements &&
      !subq_select_lex->with_sum_func && !subq_select_lex->join->having)
  {
    for (ORDER *ord= subq_select_lex->group_list.first; ord; ord= ord->next)
    {
      /*
        Do not remove the item if it is used in select list and then referred
        from GROUP BY clause by its name or number. Example:

          select (select ... ) as SUBQ ...  group by SUBQ

        Here SUBQ cannot be removed.
      */
      if (!ord->in_field_list)
        (*ord->item)->walk(&Item::eliminate_subselect_processor, FALSE, NULL);
    }
    subq_select_lex->join->group_list= NULL;
    subq_select_lex->group_list.empty();
    DBUG_PRINT("info", ("GROUP BY removed"));
  }

  /*
    TODO: This would prevent processing quries with ORDER BY ... LIMIT
    therefore we disable this optimization for now.
    Remove GROUP BY if there are no aggregate functions and no HAVING
    clause
  if (subq_select_lex->group_list.elements &&
      !subq_select_lex->with_sum_func && !subq_select_lex->join->having)
  {
    subq_select_lex->join->group_list= NULL;
    subq_select_lex->group_list.empty();
  }
  */
  DBUG_VOID_RETURN;
}


/**
  Function to setup clauses without sum functions.
*/
static inline int
setup_without_group(THD *thd, Ref_ptr_array ref_pointer_array,
                              TABLE_LIST *tables,
                              List<TABLE_LIST> &leaves,
                              List<Item> &fields,
                              List<Item> &all_fields,
                              COND **conds,
                              ORDER *order,
                              ORDER *group,
                              List<Window_spec> &win_specs,
		              List<Item_window_func> &win_funcs,
                              bool *hidden_group_fields,
                              uint *reserved)
{
  int res;
  enum_parsing_place save_place;
  st_select_lex *const select= thd->lex->current_select;
  nesting_map save_allow_sum_func= thd->lex->allow_sum_func;
  /* 
    Need to stave the value, so we can turn off only any new non_agg_field_used
    additions coming from the WHERE
  */
  const bool saved_non_agg_field_used= select->non_agg_field_used();
  DBUG_ENTER("setup_without_group");

  thd->lex->allow_sum_func.clear_bit(select->nest_level);
  res= setup_conds(thd, tables, leaves, conds);
  if (thd->lex->current_select->first_cond_optimization)
  {
    if (!res && *conds && ! thd->lex->current_select->merged_into)
      (*reserved)= (*conds)->exists2in_reserved_items();
    else
      (*reserved)= 0;
  }

  /* it's not wrong to have non-aggregated columns in a WHERE */
  select->set_non_agg_field_used(saved_non_agg_field_used);

  thd->lex->allow_sum_func.set_bit(select->nest_level);
  
  save_place= thd->lex->current_select->context_analysis_place;
  thd->lex->current_select->context_analysis_place= IN_ORDER_BY;
  res= res || setup_order(thd, ref_pointer_array, tables, fields, all_fields,
                          order);
  thd->lex->allow_sum_func.clear_bit(select->nest_level);
  thd->lex->current_select->context_analysis_place= IN_GROUP_BY;
  res= res || setup_group(thd, ref_pointer_array, tables, fields, all_fields,
                          group, hidden_group_fields);
  thd->lex->current_select->context_analysis_place= save_place;
  thd->lex->allow_sum_func.set_bit(select->nest_level);
  res= res || setup_windows(thd, ref_pointer_array, tables, fields, all_fields,
                            win_specs, win_funcs);
  thd->lex->allow_sum_func= save_allow_sum_func;
  DBUG_RETURN(res);
}

bool vers_select_conds_t::init_from_sysvar(THD *thd)
{
  vers_asof_timestamp_t &in= thd->variables.vers_asof_timestamp;
  type= (vers_system_time_t) in.type;
  delete_history= false;
  start.unit= VERS_TIMESTAMP;
  if (type != SYSTEM_TIME_UNSPECIFIED && type != SYSTEM_TIME_ALL)
  {
    DBUG_ASSERT(type == SYSTEM_TIME_AS_OF);
    MYSQL_TIME ltime;
    thd->variables.time_zone->gmt_sec_to_TIME(&ltime, in.unix_time);
    ltime.second_part = in.second_part;

    start.item= new (thd->mem_root)
        Item_datetime_literal(thd, &ltime, TIME_SECOND_PART_DIGITS);
    if (!start.item)
      return true;
  }
  else
    start.item= NULL;
  end.empty();
  return false;
}

void vers_select_conds_t::print(String *str, enum_query_type query_type) const
{
  switch (orig_type) {
  case SYSTEM_TIME_UNSPECIFIED:
    break;
  case SYSTEM_TIME_AS_OF:
    start.print(str, query_type, STRING_WITH_LEN(" FOR SYSTEM_TIME AS OF "));
    break;
  case SYSTEM_TIME_FROM_TO:
    start.print(str, query_type, STRING_WITH_LEN(" FOR SYSTEM_TIME FROM "));
    end.print(str, query_type, STRING_WITH_LEN(" TO "));
    break;
  case SYSTEM_TIME_BETWEEN:
    start.print(str, query_type, STRING_WITH_LEN(" FOR SYSTEM_TIME BETWEEN "));
    end.print(str, query_type, STRING_WITH_LEN(" AND "));
    break;
  case SYSTEM_TIME_BEFORE:
  case SYSTEM_TIME_HISTORY:
    DBUG_ASSERT(0);
    break;
  case SYSTEM_TIME_ALL:
    str->append(" FOR SYSTEM_TIME ALL");
    break;
  }
}

static
bool skip_setup_conds(THD *thd)
{
  return (!thd->stmt_arena->is_conventional()
          && !thd->stmt_arena->is_stmt_prepare_or_first_sp_execute())
         || thd->lex->is_view_context_analysis();
}

int SELECT_LEX::vers_setup_conds(THD *thd, TABLE_LIST *tables)
{
  DBUG_ENTER("SELECT_LEX::vers_setup_cond");
#define newx new (thd->mem_root)

  const bool update_conds= !skip_setup_conds(thd);
  TABLE_LIST *table;

  if (!versioned_tables)
  {
    for (table= tables; table; table= table->next_local)
    {
      if (table->table && table->table->versioned())
        versioned_tables++;
      else if (table->vers_conditions.is_set() &&
              (table->is_non_derived() || !table->vers_conditions.used))
      {
        my_error(ER_VERS_NOT_VERSIONED, MYF(0), table->alias.str);
        DBUG_RETURN(-1);
      }
    }
  }

  if (versioned_tables == 0)
    DBUG_RETURN(0);

  /* For prepared statements we create items on statement arena,
     because they must outlive execution phase for multiple executions. */
  Query_arena_stmt on_stmt_arena(thd);

  // find outer system_time
  SELECT_LEX *outer_slex= outer_select();
  TABLE_LIST* outer_table= NULL;

  if (outer_slex)
  {
    TABLE_LIST* derived= master_unit()->derived;
    // inner SELECT may not be a derived table (derived == NULL)
    while (derived && outer_slex && !derived->vers_conditions.is_set())
    {
      derived= outer_slex->master_unit()->derived;
      outer_slex= outer_slex->outer_select();
    }
    if (derived && outer_slex)
    {
      DBUG_ASSERT(derived->vers_conditions.is_set());
      outer_table= derived;
    }
  }

  bool is_select= false;
  bool use_sysvar= false;
  switch (thd->lex->sql_command)
  {
  case SQLCOM_SELECT:
    use_sysvar= true;
    /* fall through */
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_UPDATE_MULTI:
    is_select= true;
  default:
    break;
  }

  for (table= tables; table; table= table->next_local)
  {
    if (!table->table || table->is_view() || !table->table->versioned())
      continue;

    vers_select_conds_t &vers_conditions= table->vers_conditions;

#ifdef WITH_PARTITION_STORAGE_ENGINE
      /*
        if the history is stored in partitions, then partitions
        themselves are not versioned
      */
      if (table->partition_names && table->table->part_info->vers_info)
      {
        /* If the history is stored in partitions, then partitions
            themselves are not versioned. */
        if (vers_conditions.was_set())
        {
          my_error(ER_VERS_QUERY_IN_PARTITION, MYF(0), table->alias.str);
          DBUG_RETURN(-1);
        }
        else if (!vers_conditions.is_set())
          vers_conditions.type= SYSTEM_TIME_ALL;
      }
#endif

    if (outer_table && !vers_conditions.is_set())
    {
      // propagate system_time from nearest outer SELECT_LEX
      vers_conditions= outer_table->vers_conditions;
      outer_table->vers_conditions.used= true;
    }

    // propagate system_time from sysvar
    if (!vers_conditions.is_set() && use_sysvar)
    {
      if (vers_conditions.init_from_sysvar(thd))
        DBUG_RETURN(-1);
    }

    if (vers_conditions.is_set())
    {
      if (vers_conditions.was_set() &&
          table->lock_type > TL_READ_NO_INSERT &&
          !vers_conditions.delete_history)
      {
        my_error(ER_TABLE_NOT_LOCKED_FOR_WRITE, MYF(0), table->alias.str);
        DBUG_RETURN(-1);
      }

      if (vers_conditions.type == SYSTEM_TIME_ALL)
        continue;
    }

    const LEX_CSTRING *fstart=
        thd->make_clex_string(table->table->vers_start_field()->field_name);
    const LEX_CSTRING *fend=
        thd->make_clex_string(table->table->vers_end_field()->field_name);

    Item *row_start=
        newx Item_field(thd, &this->context, table->db.str, table->alias.str, fstart);
    Item *row_end=
        newx Item_field(thd, &this->context, table->db.str, table->alias.str, fend);

    bool timestamps_only= table->table->versioned(VERS_TIMESTAMP);

    if (vers_conditions.is_set() && vers_conditions.type != SYSTEM_TIME_HISTORY)
    {
      thd->where= "FOR SYSTEM_TIME";
      /* TODO: do resolve fix_length_and_dec(), fix_fields(). This requires
        storing vers_conditions as Item and make some magic related to
        vers_system_time_t/VERS_TRX_ID at stage of fix_fields()
        (this is large refactoring). */
      if (vers_conditions.resolve_units(thd))
        DBUG_RETURN(-1);
      if (timestamps_only && (vers_conditions.start.unit == VERS_TRX_ID ||
        vers_conditions.end.unit == VERS_TRX_ID))
      {
        my_error(ER_VERS_ENGINE_UNSUPPORTED, MYF(0), table->table_name.str);
        DBUG_RETURN(-1);
      }
    }

    if (!update_conds)
      continue;

    Item *cond1= NULL, *cond2= NULL, *cond3= NULL, *curr= NULL;
    Item *point_in_time1= vers_conditions.start.item;
    Item *point_in_time2= vers_conditions.end.item;
    TABLE *t= table->table;
    if (t->versioned(VERS_TIMESTAMP))
    {
      MYSQL_TIME max_time;
      switch (vers_conditions.type)
      {
      case SYSTEM_TIME_UNSPECIFIED:
      case SYSTEM_TIME_HISTORY:
        thd->variables.time_zone->gmt_sec_to_TIME(&max_time, TIMESTAMP_MAX_VALUE);
        max_time.second_part= TIME_MAX_SECOND_PART;
        curr= newx Item_datetime_literal(thd, &max_time, TIME_SECOND_PART_DIGITS);
        if (vers_conditions.type == SYSTEM_TIME_UNSPECIFIED)
          cond1= newx Item_func_eq(thd, row_end, curr);
        else
          cond1= newx Item_func_lt(thd, row_end, curr);
        break;
      case SYSTEM_TIME_AS_OF:
        cond1= newx Item_func_le(thd, row_start, point_in_time1);
        cond2= newx Item_func_gt(thd, row_end, point_in_time1);
        break;
      case SYSTEM_TIME_FROM_TO:
        cond1= newx Item_func_lt(thd, row_start, point_in_time2);
        cond2= newx Item_func_gt(thd, row_end, point_in_time1);
        cond3= newx Item_func_lt(thd, point_in_time1, point_in_time2);
        break;
      case SYSTEM_TIME_BETWEEN:
        cond1= newx Item_func_le(thd, row_start, point_in_time2);
        cond2= newx Item_func_gt(thd, row_end, point_in_time1);
        cond3= newx Item_func_le(thd, point_in_time1, point_in_time2);
        break;
      case SYSTEM_TIME_BEFORE:
        cond1= newx Item_func_history(thd, row_end);
        cond2= newx Item_func_lt(thd, row_end, point_in_time1);
        break;
      default:
        DBUG_ASSERT(0);
      }
    }
    else
    {
      DBUG_ASSERT(table->table->s && table->table->s->db_plugin);

      Item *trx_id0, *trx_id1;

      switch (vers_conditions.type)
      {
      case SYSTEM_TIME_UNSPECIFIED:
      case SYSTEM_TIME_HISTORY:
        curr= newx Item_int(thd, ULONGLONG_MAX);
        if (vers_conditions.type == SYSTEM_TIME_UNSPECIFIED)
          cond1= newx Item_func_eq(thd, row_end, curr);
        else
          cond1= newx Item_func_lt(thd, row_end, curr);
        break;
      case SYSTEM_TIME_AS_OF:
        trx_id0= vers_conditions.start.unit == VERS_TIMESTAMP
          ? newx Item_func_trt_id(thd, point_in_time1, TR_table::FLD_TRX_ID)
          : point_in_time1;
        cond1= newx Item_func_trt_trx_sees_eq(thd, trx_id0, row_start);
        cond2= newx Item_func_trt_trx_sees(thd, row_end, trx_id0);
        break;
      case SYSTEM_TIME_FROM_TO:
	cond3= newx Item_func_lt(thd, point_in_time1, point_in_time2);
        /* fall through */
      case SYSTEM_TIME_BETWEEN:
        trx_id0= vers_conditions.start.unit == VERS_TIMESTAMP
          ? newx Item_func_trt_id(thd, point_in_time1, TR_table::FLD_TRX_ID, true)
          : point_in_time1;
        trx_id1= vers_conditions.end.unit == VERS_TIMESTAMP
          ? newx Item_func_trt_id(thd, point_in_time2, TR_table::FLD_TRX_ID, false)
          : point_in_time2;
        cond1= vers_conditions.type == SYSTEM_TIME_FROM_TO
          ? newx Item_func_trt_trx_sees(thd, trx_id1, row_start)
          : newx Item_func_trt_trx_sees_eq(thd, trx_id1, row_start);
        cond2= newx Item_func_trt_trx_sees_eq(thd, row_end, trx_id0);
	if (!cond3)
	  cond3= newx Item_func_le(thd, point_in_time1, point_in_time2);
        break;
      case SYSTEM_TIME_BEFORE:
        trx_id0= vers_conditions.start.unit == VERS_TIMESTAMP
          ? newx Item_func_trt_id(thd, point_in_time1, TR_table::FLD_TRX_ID, true)
          : point_in_time1;
        cond1= newx Item_func_history(thd, row_end);
        cond2= newx Item_func_trt_trx_sees(thd, trx_id0, row_end);
        break;
      default:
        DBUG_ASSERT(0);
      }
    }

    if (cond1)
    {
      cond1= and_items(thd, cond2, cond1);
      cond1= and_items(thd, cond3, cond1);
      if (is_select)
        table->on_expr= and_items(thd, table->on_expr, cond1);
      else
      {
        if (join)
        {
          where= and_items(thd, join->conds, cond1);
          join->conds= where;
        }
        else
          where= and_items(thd, where, cond1);
        table->where= and_items(thd, table->where, cond1);
      }
    }

    table->vers_conditions.type= SYSTEM_TIME_ALL;
  } // for (table= tables; ...)

  DBUG_RETURN(0);
#undef newx
}

/*****************************************************************************
  Check fields, find best join, do the select and output fields.
  mysql_select assumes that all tables are already opened
*****************************************************************************/


/**
  Prepare of whole select (including sub queries in future).

  @todo
    Add check of calculation of GROUP functions and fields:
    SELECT COUNT(*)+table.col1 from table1;

  @retval
    -1   on error
  @retval
    0   on success
*/
int
JOIN::prepare(TABLE_LIST *tables_init,
	      uint wild_num, COND *conds_init, uint og_num,
	      ORDER *order_init, bool skip_order_by,
              ORDER *group_init, Item *having_init,
	      ORDER *proc_param_init, SELECT_LEX *select_lex_arg,
	      SELECT_LEX_UNIT *unit_arg)
{
  DBUG_ENTER("JOIN::prepare");

  // to prevent double initialization on EXPLAIN
  if (optimization_state != JOIN::NOT_OPTIMIZED)
    DBUG_RETURN(0);

  conds= conds_init;
  order= order_init;
  group_list= group_init;
  having= having_init;
  proc_param= proc_param_init;
  tables_list= tables_init;
  select_lex= select_lex_arg;
  select_lex->join= this;
  join_list= &select_lex->top_join_list;
  union_part= unit_arg->is_unit_op();

  // simple check that we got usable conds
  dbug_print_item(conds);

  if (select_lex->handle_derived(thd->lex, DT_PREPARE))
    DBUG_RETURN(-1);

  thd->lex->current_select->context_analysis_place= NO_MATTER;
  thd->lex->current_select->is_item_list_lookup= 1;
  /*
    If we have already executed SELECT, then it have not sense to prevent
    its table from update (see unique_table())
    Affects only materialized derived tables.
  */
  /* Check that all tables, fields, conds and order are ok */
  if (!(select_options & OPTION_SETUP_TABLES_DONE) &&
      setup_tables_and_check_access(thd, &select_lex->context, join_list,
                                    tables_list, select_lex->leaf_tables,
                                    FALSE, SELECT_ACL, SELECT_ACL, FALSE))
      DBUG_RETURN(-1);

  /* System Versioning: handle FOR SYSTEM_TIME clause. */
  if (select_lex->vers_setup_conds(thd, tables_list) < 0)
    DBUG_RETURN(-1);

  /*
    TRUE if the SELECT list mixes elements with and without grouping,
    and there is no GROUP BY clause. Mixing non-aggregated fields with
    aggregate functions in the SELECT list is a MySQL extenstion that
    is allowed only if the ONLY_FULL_GROUP_BY sql mode is not set.
  */
  mixed_implicit_grouping= false;
  if ((~thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY) &&
      select_lex->with_sum_func && !group_list)
  {
    List_iterator_fast <Item> select_it(fields_list);
    Item *select_el; /* Element of the SELECT clause, can be an expression. */
    bool found_field_elem= false;
    bool found_sum_func_elem= false;

    while ((select_el= select_it++))
    {
      if (select_el->with_sum_func)
        found_sum_func_elem= true;
      if (select_el->with_field)
        found_field_elem= true;
      if (found_sum_func_elem && found_field_elem)
      {
        mixed_implicit_grouping= true;
        break;
      }
    }
  }

  table_count= select_lex->leaf_tables.elements;

  TABLE_LIST *tbl;
  List_iterator_fast<TABLE_LIST> li(select_lex->leaf_tables);
  while ((tbl= li++))
  {
    /*
      If the query uses implicit grouping where the select list contains both
      aggregate functions and non-aggregate fields, any non-aggregated field
      may produce a NULL value. Set all fields of each table as nullable before
      semantic analysis to take into account this change of nullability.

      Note: this loop doesn't touch tables inside merged semi-joins, because
      subquery-to-semijoin conversion has not been done yet. This is intended.
    */
    if (mixed_implicit_grouping && tbl->table)
      tbl->table->maybe_null= 1;
  }
 
  uint real_og_num= og_num;
  if (skip_order_by && 
      select_lex != select_lex->master_unit()->global_parameters())
    real_og_num+= select_lex->order_list.elements;

  DBUG_ASSERT(select_lex->hidden_bit_fields == 0);
  if (setup_wild(thd, tables_list, fields_list, &all_fields, wild_num,
                 &select_lex->hidden_bit_fields))
    DBUG_RETURN(-1);
  if (select_lex->setup_ref_array(thd, real_og_num))
    DBUG_RETURN(-1);

  ref_ptrs= ref_ptr_array_slice(0);
  
  enum_parsing_place save_place=
                     thd->lex->current_select->context_analysis_place;
  thd->lex->current_select->context_analysis_place= SELECT_LIST;
  if (setup_fields(thd, ref_ptrs, fields_list, MARK_COLUMNS_READ,
                   &all_fields, &select_lex->pre_fix, 1))
    DBUG_RETURN(-1);
  thd->lex->current_select->context_analysis_place= save_place;

  if (setup_without_group(thd, ref_ptrs, tables_list,
                          select_lex->leaf_tables, fields_list,
                          all_fields, &conds, order, group_list,
                          select_lex->window_specs,
                          select_lex->window_funcs,
                          &hidden_group_fields,
                          &select_lex->select_n_reserved))
    DBUG_RETURN(-1);

  /*
    Permanently remove redundant parts from the query if
      1) This is a subquery
      2) This is the first time this query is optimized (since the
         transformation is permanent
      3) Not normalizing a view. Removal should take place when a
         query involving a view is optimized, not when the view
         is created
  */
  if (select_lex->master_unit()->item &&                               // 1)
      select_lex->first_cond_optimization &&                           // 2)
      !thd->lex->is_view_context_analysis())                           // 3)
  {
    remove_redundant_subquery_clauses(select_lex);
  }

  /* Resolve the ORDER BY that was skipped, then remove it. */
  if (skip_order_by && select_lex !=
                       select_lex->master_unit()->global_parameters())
  {
    nesting_map save_allow_sum_func= thd->lex->allow_sum_func;
    thd->lex->allow_sum_func.set_bit(select_lex->nest_level);
    thd->where= "order clause";
    for (ORDER *order= select_lex->order_list.first; order; order= order->next)
    {
      /* Don't add the order items to all fields. Just resolve them to ensure
         the query is valid, we'll drop them immediately after. */
      if (find_order_in_list(thd, ref_ptrs, tables_list, order,
                             fields_list, all_fields, false, false, false))
        DBUG_RETURN(-1);
    }
    thd->lex->allow_sum_func= save_allow_sum_func;
    select_lex->order_list.empty();
  }

  if (having)
  {
    nesting_map save_allow_sum_func= thd->lex->allow_sum_func;
    thd->where="having clause";
    thd->lex->allow_sum_func.set_bit(select_lex_arg->nest_level);
    select_lex->having_fix_field= 1;
    /*
      Wrap alone field in HAVING clause in case it will be outer field
      of subquery which need persistent pointer on it, but having
      could be changed by optimizer
    */
    if (having->type() == Item::REF_ITEM &&
        ((Item_ref *)having)->ref_type() == Item_ref::REF)
      wrap_ident(thd, &having);
    bool having_fix_rc= having->fix_fields_if_needed_for_bool(thd, &having);
    select_lex->having_fix_field= 0;

    if (unlikely(having_fix_rc || thd->is_error()))
      DBUG_RETURN(-1);				/* purecov: inspected */
    thd->lex->allow_sum_func= save_allow_sum_func;

    if (having->with_window_func)
    {
      my_error(ER_WRONG_PLACEMENT_OF_WINDOW_FUNCTION, MYF(0));
      DBUG_RETURN(-1); 
    }
  }

  /*
     After setting up window functions, we may have discovered additional
     used tables from the PARTITION BY and ORDER BY list. Update all items
     that contain window functions.
  */
  if (select_lex->have_window_funcs())
  {
    List_iterator_fast<Item> it(select_lex->item_list);
    Item *item;
    while ((item= it++))
    {
      if (item->with_window_func)
        item->update_used_tables();
    }
  }

  With_clause *with_clause=select_lex->get_with_clause();
  if (with_clause && with_clause->prepare_unreferenced_elements(thd))
    DBUG_RETURN(1);

  With_element *with_elem= select_lex->get_with_element();
  if (with_elem &&
      select_lex->check_unrestricted_recursive(
                      thd->variables.only_standard_compliant_cte))
    DBUG_RETURN(-1);
  if (!(select_lex->changed_elements & TOUCHED_SEL_COND))
    select_lex->check_subqueries_with_recursive_references();
  
  int res= check_and_do_in_subquery_rewrites(this);

  select_lex->fix_prepare_information(thd, &conds, &having);
  
  if (res)
    DBUG_RETURN(res);

  if (order)
  {
    bool real_order= FALSE;
    ORDER *ord;
    for (ord= order; ord; ord= ord->next)
    {
      Item *item= *ord->item;
      /*
        Disregard sort order if there's only 
        zero length NOT NULL fields (e.g. {VAR}CHAR(0) NOT NULL") or
        zero length NOT NULL string functions there.
        Such tuples don't contain any data to sort.
      */
      if (!real_order &&
           /* Not a zero length NOT NULL field */
          ((item->type() != Item::FIELD_ITEM ||
            ((Item_field *) item)->field->maybe_null() ||
            ((Item_field *) item)->field->sort_length()) &&
           /* AND not a zero length NOT NULL string function. */
           (item->type() != Item::FUNC_ITEM ||
            item->maybe_null ||
            item->result_type() != STRING_RESULT ||
            item->max_length)))
        real_order= TRUE;

      if ((item->with_sum_func && item->type() != Item::SUM_FUNC_ITEM) ||
          item->with_window_func)
        item->split_sum_func(thd, ref_ptrs, all_fields, SPLIT_SUM_SELECT);
    }
    if (!real_order)
      order= NULL;
  }

  if (having && having->with_sum_func)
    having->split_sum_func2(thd, ref_ptrs, all_fields,
                            &having, SPLIT_SUM_SKIP_REGISTERED);
  if (select_lex->inner_sum_func_list)
  {
    Item_sum *end=select_lex->inner_sum_func_list;
    Item_sum *item_sum= end;  
    do
    { 
      item_sum= item_sum->next;
      item_sum->split_sum_func2(thd, ref_ptrs,
                                all_fields, item_sum->ref_by, 0);
    } while (item_sum != end);
  }

  if (select_lex->inner_refs_list.elements &&
      fix_inner_refs(thd, all_fields, select_lex, ref_ptrs))
    DBUG_RETURN(-1);

  if (group_list)
  {
    /*
      Because HEAP tables can't index BIT fields we need to use an
      additional hidden field for grouping because later it will be
      converted to a LONG field. Original field will remain of the
      BIT type and will be returned to a client.
    */
    for (ORDER *ord= group_list; ord; ord= ord->next)
    {
      if ((*ord->item)->type() == Item::FIELD_ITEM &&
          (*ord->item)->field_type() == MYSQL_TYPE_BIT)
      {
        Item_field *field= new (thd->mem_root) Item_field(thd, *(Item_field**)ord->item);
        if (!field)
          DBUG_RETURN(-1);
        int el= all_fields.elements;
        ref_ptrs[el]= field;
        all_fields.push_front(field, thd->mem_root);
        ord->item= &ref_ptrs[el];
      }
    }
  }

  /*
    Check if there are references to un-aggregated columns when computing 
    aggregate functions with implicit grouping (there is no GROUP BY).
  */
  if (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY && !group_list &&
      !(select_lex->master_unit()->item &&
        select_lex->master_unit()->item->is_in_predicate() &&
        ((Item_in_subselect*)select_lex->master_unit()->item)->
        test_set_strategy(SUBS_MAXMIN_INJECTED)) &&
      select_lex->non_agg_field_used() &&
      select_lex->agg_func_used())
  {
    my_message(ER_MIX_OF_GROUP_FUNC_AND_FIELDS,
               ER_THD(thd, ER_MIX_OF_GROUP_FUNC_AND_FIELDS), MYF(0));
    DBUG_RETURN(-1);
  }
  {
    /* Caclulate the number of groups */
    send_group_parts= 0;
    for (ORDER *group_tmp= group_list ; group_tmp ; group_tmp= group_tmp->next)
      send_group_parts++;
  }
  
  procedure= setup_procedure(thd, proc_param, result, fields_list, &error);
  if (unlikely(error))
    goto err;					/* purecov: inspected */
  if (procedure)
  {
    if (setup_new_fields(thd, fields_list, all_fields,
			 procedure->param_fields))
	goto err;				/* purecov: inspected */
    if (procedure->group)
    {
      if (!test_if_subpart(procedure->group,group_list))
      {						/* purecov: inspected */
	my_message(ER_DIFF_GROUPS_PROC, ER_THD(thd, ER_DIFF_GROUPS_PROC),
                   MYF(0));                     /* purecov: inspected */
	goto err;				/* purecov: inspected */
      }
    }
    if (order && (procedure->flags & PROC_NO_SORT))
    {						/* purecov: inspected */
      my_message(ER_ORDER_WITH_PROC, ER_THD(thd, ER_ORDER_WITH_PROC),
                 MYF(0));                       /* purecov: inspected */
      goto err;					/* purecov: inspected */
    }
    if (thd->lex->derived_tables)
    {
      /*
        Queries with derived tables and PROCEDURE are not allowed.
        Many of such queries are disallowed grammatically, but there
        are still some complex cases:
          SELECT 1 FROM (SELECT 1) a PROCEDURE ANALYSE()
      */
      my_error(ER_WRONG_USAGE, MYF(0), "PROCEDURE", 
               thd->lex->derived_tables & DERIVED_VIEW ?
               "view" : "subquery"); 
      goto err;
    }
    if (thd->lex->sql_command != SQLCOM_SELECT)
    {
      // EXPLAIN SELECT * FROM t1 PROCEDURE ANALYSE()
      my_error(ER_WRONG_USAGE, MYF(0), "PROCEDURE", "non-SELECT");
      goto err;
    }
  }

  if (!procedure && result && result->prepare(fields_list, unit_arg))
    goto err;					/* purecov: inspected */

  unit= unit_arg;
  if (prepare_stage2())
    goto err;

  DBUG_RETURN(0); // All OK

err:
  delete procedure;                /* purecov: inspected */
  procedure= 0;
  DBUG_RETURN(-1);                /* purecov: inspected */
}


/**
  Second phase of prepare where we collect some statistic.

  @details
  We made this part separate to be able recalculate some statistic after
  transforming subquery on optimization phase.
*/

bool JOIN::prepare_stage2()
{
  bool res= TRUE;
  DBUG_ENTER("JOIN::prepare_stage2");

  /* Init join struct */
  count_field_types(select_lex, &tmp_table_param, all_fields, 0);
  this->group= group_list != 0;

  if (tmp_table_param.sum_func_count && !group_list)
  {
    implicit_grouping= TRUE;
    // Result will contain zero or one row - ordering is meaningless
    order= NULL;
  }

#ifdef RESTRICTED_GROUP
  if (implicit_grouping)
  {
    my_message(ER_WRONG_SUM_SELECT,ER_THD(thd, ER_WRONG_SUM_SELECT),MYF(0));
    goto err;
  }
#endif
  if (select_lex->olap == ROLLUP_TYPE && rollup_init())
    goto err;
  if (alloc_func_list())
    goto err;

  res= FALSE;
err:
  DBUG_RETURN(res);				/* purecov: inspected */
}


bool JOIN::build_explain()
{
  have_query_plan= QEP_AVAILABLE;

  /*
    explain data must be created on the Explain_query::mem_root. Because it's
    just a memroot, not an arena, explain data must not contain any Items
  */
  MEM_ROOT *old_mem_root= thd->mem_root;
  Item *old_free_list __attribute__((unused))= thd->free_list;
  thd->mem_root= thd->lex->explain->mem_root;
  bool res= save_explain_data(thd->lex->explain, false /* can overwrite */,
                        need_tmp,
                        !skip_sort_order && !no_order && (order || group_list),
                        select_distinct);
  thd->mem_root= old_mem_root;
  DBUG_ASSERT(thd->free_list == old_free_list); // no Items were created
  if (res)
    return 1;

  uint select_nr= select_lex->select_number;
  JOIN_TAB *curr_tab= join_tab + exec_join_tab_cnt();
  for (uint i= 0; i < aggr_tables; i++, curr_tab++)
  {
    if (select_nr == INT_MAX)
    {
      /* this is a fake_select_lex of a union */
      select_nr= select_lex->master_unit()->first_select()->select_number;
      curr_tab->tracker= thd->lex->explain->get_union(select_nr)->
                         get_tmptable_read_tracker();
    }
    else
    {
      curr_tab->tracker= thd->lex->explain->get_select(select_nr)->
                         get_using_temporary_read_tracker();
    }
  }
  return 0;
}


int JOIN::optimize()
{
  int res= 0;
  create_explain_query_if_not_exists(thd->lex, thd->mem_root);
  join_optimization_state init_state= optimization_state;
  if (optimization_state == JOIN::OPTIMIZATION_PHASE_1_DONE)
    res= optimize_stage2();
  else
  {
    // to prevent double initialization on EXPLAIN
    if (optimization_state != JOIN::NOT_OPTIMIZED)
      return FALSE;
    optimization_state= JOIN::OPTIMIZATION_IN_PROGRESS;
    res= optimize_inner();
  }
  if (!with_two_phase_optimization ||
      init_state == JOIN::OPTIMIZATION_PHASE_1_DONE)
  {
    if (!res && have_query_plan != QEP_DELETED)
      res= build_explain();
    optimization_state= JOIN::OPTIMIZATION_DONE;
  }
  return res;
}


int JOIN::init_join_caches()
{
  JOIN_TAB *tab;

  for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab;
       tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
  {
    TABLE *table= tab->table;
    if (table->file->keyread_enabled())
    {
      if (!(table->file->index_flags(table->file->keyread, 0, 1) & HA_CLUSTERED_INDEX))
        table->mark_index_columns(table->file->keyread, table->read_set);
    }
    else if ((tab->read_first_record == join_read_first ||
              tab->read_first_record == join_read_last) &&
             !tab->filesort && table->covering_keys.is_set(tab->index) &&
             !table->no_keyread)
    {
      table->prepare_for_keyread(tab->index, table->read_set);
    }
    if (tab->cache && tab->cache->init(select_options & SELECT_DESCRIBE))
      revise_cache_usage(tab);
    else
      tab->remove_redundant_bnl_scan_conds();
  }
  return 0;
}


/**
  global select optimisation.

  @note
    error code saved in field 'error'

  @retval
    0   success
  @retval
    1   error
*/

int
JOIN::optimize_inner()
{
  DBUG_ENTER("JOIN::optimize_inner");
  subq_exit_fl= false;
  do_send_rows = (unit->select_limit_cnt) ? 1 : 0;

  DEBUG_SYNC(thd, "before_join_optimize");

  THD_STAGE_INFO(thd, stage_optimizing);

  set_allowed_join_cache_types();
  need_distinct= TRUE;

  /*
    Needed in case optimizer short-cuts,
    set properly in make_aggr_tables_info()
  */
  fields= &select_lex->item_list;

  if (select_lex->first_cond_optimization)
  {
    //Do it only for the first execution
    /* Merge all mergeable derived tables/views in this SELECT. */
    if (select_lex->handle_derived(thd->lex, DT_MERGE))
      DBUG_RETURN(TRUE);  
    table_count= select_lex->leaf_tables.elements;
  }

  if (select_lex->first_cond_optimization &&
      transform_in_predicates_into_in_subq(thd))
    DBUG_RETURN(1);

  // Update used tables after all handling derived table procedures
  select_lex->update_used_tables();

  /*
    In fact we transform underlying subqueries after their 'prepare' phase and
    before 'optimize' from upper query 'optimize' to allow semijoin
    conversion happened (which done in the same way.
  */
  if (select_lex->first_cond_optimization &&
      conds && conds->walk(&Item::exists2in_processor, 0, thd))
    DBUG_RETURN(1);
  /*
    TODO
    make view to decide if it is possible to write to WHERE directly or make Semi-Joins able to process ON condition if it is possible
  for (TABLE_LIST *tbl= tables_list; tbl; tbl= tbl->next_local)
  {
    if (tbl->on_expr &&
        tbl->on_expr->walk(&Item::exists2in_processor, 0, thd))
      DBUG_RETURN(1);
  }
  */

  if (transform_max_min_subquery())
    DBUG_RETURN(1); /* purecov: inspected */

  if (select_lex->first_cond_optimization)
  {
    /* dump_TABLE_LIST_graph(select_lex, select_lex->leaf_tables); */
    if (convert_join_subqueries_to_semijoins(this))
      DBUG_RETURN(1); /* purecov: inspected */
    /* dump_TABLE_LIST_graph(select_lex, select_lex->leaf_tables); */
    select_lex->update_used_tables();
  }
  
  eval_select_list_used_tables();

  table_count= select_lex->leaf_tables.elements;

  if (select_lex->options & OPTION_SCHEMA_TABLE &&
      optimize_schema_tables_memory_usage(select_lex->leaf_tables))
    DBUG_RETURN(1);

  if (setup_ftfuncs(select_lex)) /* should be after having->fix_fields */
    DBUG_RETURN(-1);

  row_limit= ((select_distinct || order || group_list) ? HA_POS_ERROR :
	      unit->select_limit_cnt);
  /* select_limit is used to decide if we are likely to scan the whole table */
  select_limit= unit->select_limit_cnt;
  if (having || (select_options & OPTION_FOUND_ROWS))
    select_limit= HA_POS_ERROR;
#ifdef HAVE_REF_TO_FIELDS			// Not done yet
  /* Add HAVING to WHERE if possible */
  if (having && !group_list && !sum_func_count)
  {
    if (!conds)
    {
      conds= having;
      having= 0;
    }
    else if ((conds=new (thd->mem_root) Item_cond_and(conds,having)))
    {
      /*
        Item_cond_and can't be fixed after creation, so we do not check
        conds->fixed
      */
      conds->fix_fields(thd, &conds);
      conds->change_ref_to_fields(thd, tables_list);
      conds->top_level_item();
      having= 0;
    }
  }
#endif

  SELECT_LEX *sel= select_lex;
  if (sel->first_cond_optimization)
  {
    /*
      The following code will allocate the new items in a permanent
      MEMROOT for prepared statements and stored procedures.

      But first we need to ensure that thd->lex->explain is allocated
      in the execution arena
    */
    create_explain_query_if_not_exists(thd->lex, thd->mem_root);

    Query_arena *arena, backup;
    arena= thd->activate_stmt_arena_if_needed(&backup);

    sel->first_cond_optimization= 0;

    /* Convert all outer joins to inner joins if possible */
    conds= simplify_joins(this, join_list, conds, TRUE, FALSE);
    if (thd->is_error() || select_lex->save_leaf_tables(thd))
    {
      if (arena)
        thd->restore_active_arena(arena, &backup);
      DBUG_RETURN(1);
    }
    build_bitmap_for_nested_joins(join_list, 0);

    sel->prep_where= conds ? conds->copy_andor_structure(thd) : 0;

    sel->where= conds;

    select_lex->update_used_tables();

    if (arena)
      thd->restore_active_arena(arena, &backup);
  }
  
  if (optimize_constant_subqueries())
    DBUG_RETURN(1);

  if (conds && conds->with_subquery())
    (void) conds->walk(&Item::cleanup_is_expensive_cache_processor,
                       0, (void *) 0);
  if (having && having->with_subquery())
    (void) having->walk(&Item::cleanup_is_expensive_cache_processor,
			0, (void *) 0);

  if (setup_jtbm_semi_joins(this, join_list, &conds))
    DBUG_RETURN(1);

  if (select_lex->cond_pushed_into_where)
  {
    conds= and_conds(thd, conds, select_lex->cond_pushed_into_where);
    if (conds && conds->fix_fields(thd, &conds))
      DBUG_RETURN(1);
  }
  if (select_lex->cond_pushed_into_having)
  {
    having= and_conds(thd, having, select_lex->cond_pushed_into_having);
    if (having)
    {
      select_lex->having_fix_field= 1;
      select_lex->having_fix_field_for_pushed_cond= 1;
      if (having->fix_fields(thd, &having))
        DBUG_RETURN(1);
      select_lex->having_fix_field= 0;
      select_lex->having_fix_field_for_pushed_cond= 0;
    }
  }
  
  bool ignore_on_expr= false;
  /*
    PS/SP note: on_expr of versioned table can not be reallocated
    (see build_equal_items() below) because it can be not rebuilt
    at second invocation.
  */
  if (!thd->stmt_arena->is_conventional() && thd->mem_root != thd->stmt_arena->mem_root)
    for (TABLE_LIST *tbl= tables_list; tbl; tbl= tbl->next_local)
      if (tbl->table && tbl->on_expr && tbl->table->versioned())
      {
        ignore_on_expr= true;
        break;
      }
  conds= optimize_cond(this, conds, join_list, ignore_on_expr,
                       &cond_value, &cond_equal, OPT_LINK_EQUAL_FIELDS);
  
  if (thd->is_error())
  {
    error= 1;
    DBUG_PRINT("error",("Error from optimize_cond"));
    DBUG_RETURN(1);
  }

  if (optimizer_flag(thd, OPTIMIZER_SWITCH_COND_PUSHDOWN_FOR_DERIVED))
  {
    TABLE_LIST *tbl;
    List_iterator_fast<TABLE_LIST> li(select_lex->leaf_tables);
    while ((tbl= li++))
    {
      /* 
        Do not push conditions from where into materialized inner tables
        of outer joins: this is not valid.
      */
      if (tbl->is_materialized_derived())
      {
        JOIN *join= tbl->get_unit()->first_select()->join;
        if (join &&
            join->optimization_state == JOIN::OPTIMIZATION_PHASE_1_DONE &&
            join->with_two_phase_optimization)
          continue;
        /*
          Do not push conditions from where into materialized inner tables
          of outer joins: this is not valid.
        */
        if (!tbl->is_inner_table_of_outer_join())
	{
          if (pushdown_cond_for_derived(thd, conds, tbl))
	    DBUG_RETURN(1);
        }
	if (mysql_handle_single_derived(thd->lex, tbl, DT_OPTIMIZE))
	  DBUG_RETURN(1);
      }
    }
  }
  else
  {
    /* Run optimize phase for all derived tables/views used in this SELECT. */
    if (select_lex->handle_derived(thd->lex, DT_OPTIMIZE))
      DBUG_RETURN(1);
  }

  {
    having= optimize_cond(this, having, join_list, TRUE,
                          &having_value, &having_equal);

    if (unlikely(thd->is_error()))
    {
      error= 1;
      DBUG_PRINT("error",("Error from optimize_cond"));
      DBUG_RETURN(1);
    }
    if (select_lex->where)
    {
      select_lex->cond_value= cond_value;
      if (sel->where != conds && cond_value == Item::COND_OK)
        thd->change_item_tree(&sel->where, conds);
    }  
    if (select_lex->having)
    {
      select_lex->having_value= having_value;
      if (sel->having != having && having_value == Item::COND_OK)
        thd->change_item_tree(&sel->having, having);    
    }
    if (cond_value == Item::COND_FALSE || having_value == Item::COND_FALSE || 
        (!unit->select_limit_cnt && !(select_options & OPTION_FOUND_ROWS)))
    {						/* Impossible cond */
      if (unit->select_limit_cnt)
      {
        DBUG_PRINT("info", (having_value == Item::COND_FALSE ?
                              "Impossible HAVING" : "Impossible WHERE"));
        zero_result_cause=  having_value == Item::COND_FALSE ?
                             "Impossible HAVING" : "Impossible WHERE";
      }
      else
      {
        DBUG_PRINT("info", ("Zero limit"));
        zero_result_cause= "Zero limit";
      }
      table_count= top_join_tab_count= 0;
      handle_implicit_grouping_with_window_funcs();
      error= 0;
      subq_exit_fl= true;
      goto setup_subq_exit;
    }
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  {
    TABLE_LIST *tbl;
    List_iterator_fast<TABLE_LIST> li(select_lex->leaf_tables);
    while ((tbl= li++))
    {
      Item **prune_cond= get_sargable_cond(this, tbl->table);
      tbl->table->all_partitions_pruned_away=
        prune_partitions(thd, tbl->table, *prune_cond);
    }
  }
#endif

  /* 
     Try to optimize count(*), MY_MIN() and MY_MAX() to const fields if
     there is implicit grouping (aggregate functions but no
     group_list). In this case, the result set shall only contain one
     row. 
  */
  if (tables_list && implicit_grouping)
  {
    int res;
    /*
      opt_sum_query() returns HA_ERR_KEY_NOT_FOUND if no rows match
      to the WHERE conditions,
      or 1 if all items were resolved (optimized away),
      or 0, or an error number HA_ERR_...

      If all items were resolved by opt_sum_query, there is no need to
      open any tables.
    */
    if ((res=opt_sum_query(thd, select_lex->leaf_tables, all_fields, conds)))
    {
      DBUG_ASSERT(res >= 0);
      if (res == HA_ERR_KEY_NOT_FOUND)
      {
        DBUG_PRINT("info",("No matching min/max row"));
	zero_result_cause= "No matching min/max row";
        table_count= top_join_tab_count= 0;
	error=0;
        subq_exit_fl= true;
        handle_implicit_grouping_with_window_funcs();
        goto setup_subq_exit;
      }
      if (res > 1)
      {
        error= res;
        DBUG_PRINT("error",("Error from opt_sum_query"));
        DBUG_RETURN(1);
      }

      DBUG_PRINT("info",("Select tables optimized away"));
      if (!select_lex->have_window_funcs())
        zero_result_cause= "Select tables optimized away";
      tables_list= 0;				// All tables resolved
      select_lex->min_max_opt_list.empty();
      const_tables= top_join_tab_count= table_count;
      handle_implicit_grouping_with_window_funcs();
      /*
        Extract all table-independent conditions and replace the WHERE
        clause with them. All other conditions were computed by opt_sum_query
        and the MIN/MAX/COUNT function(s) have been replaced by constants,
        so there is no need to compute the whole WHERE clause again.
        Notice that make_cond_for_table() will always succeed to remove all
        computed conditions, because opt_sum_query() is applicable only to
        conjunctions.
        Preserve conditions for EXPLAIN.
      */
      if (conds && !(thd->lex->describe & DESCRIBE_EXTENDED))
      {
        COND *table_independent_conds=
          make_cond_for_table(thd, conds, PSEUDO_TABLE_BITS, 0, -1,
                              FALSE, FALSE);
        DBUG_EXECUTE("where",
                     print_where(table_independent_conds,
                                 "where after opt_sum_query()",
                                 QT_ORDINARY););
        conds= table_independent_conds;
      }
    }
  }
  if (!tables_list)
  {
    DBUG_PRINT("info",("No tables"));
    error= 0;
    subq_exit_fl= true;
    goto setup_subq_exit;
  }
  error= -1;					// Error is sent to client
  /* get_sort_by_table() call used to be here: */
  MEM_UNDEFINED(&sort_by_table, sizeof(sort_by_table));

  /*
    We have to remove constants and duplicates from group_list before
    calling make_join_statistics() as this may call get_best_group_min_max()
    which needs a simplfied group_list.
  */
  if (group_list && table_count == 1)
  {
    group_list= remove_const(this, group_list, conds,
                             rollup.state == ROLLUP::STATE_NONE,
                             &simple_group);
    if (unlikely(thd->is_error()))
    {
      error= 1;
      DBUG_RETURN(1);
    }
    if (!group_list)
    {
      /* The output has only one row */
      order=0;
      simple_order=1;
      group_optimized_away= 1;
      select_distinct=0;
    }
  }
  
  /* Calculate how to do the join */
  THD_STAGE_INFO(thd, stage_statistics);
  result->prepare_to_read_rows();
  if (unlikely(make_join_statistics(this, select_lex->leaf_tables,
                                    &keyuse)) ||
      unlikely(thd->is_fatal_error))
  {
    DBUG_PRINT("error",("Error: make_join_statistics() failed"));
    DBUG_RETURN(1);
  }

  /*
    If a splittable materialized derived/view dt_i is embedded into
    into another splittable materialized derived/view dt_o then
    splitting plans for dt_i and dt_o are evaluated independently.
    First the optimizer looks for the best splitting plan sp_i for dt_i.
    It happens when non-splitting plans for dt_o are evaluated.
    The cost of sp_i is considered as the cost of materialization of dt_i
    when evaluating any splitting plan for dt_o.
  */
  if (fix_all_splittings_in_plan())
    DBUG_RETURN(1);

setup_subq_exit:
  with_two_phase_optimization= check_two_phase_optimization(thd);
  if (with_two_phase_optimization)
    optimization_state= JOIN::OPTIMIZATION_PHASE_1_DONE;
  else
  {
    if (optimize_stage2())
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


int JOIN::optimize_stage2()
{
  ulonglong select_opts_for_readinfo;
  uint no_jbuf_after;
  JOIN_TAB *tab;
  DBUG_ENTER("JOIN::optimize_stage2");

  if (subq_exit_fl)
    goto setup_subq_exit;

  if (unlikely(thd->check_killed()))
    DBUG_RETURN(1);

  /* Generate an execution plan from the found optimal join order. */
  if (get_best_combination())
    DBUG_RETURN(1);

  if (select_lex->handle_derived(thd->lex, DT_OPTIMIZE))
    DBUG_RETURN(1);

  if (optimizer_flag(thd, OPTIMIZER_SWITCH_DERIVED_WITH_KEYS))
    drop_unused_derived_keys();

  if (rollup.state != ROLLUP::STATE_NONE)
  {
    if (rollup_process_const_fields())
    {
      DBUG_PRINT("error", ("Error: rollup_process_fields() failed"));
      DBUG_RETURN(1);
    }
  }
  else
  {
    /* Remove distinct if only const tables */
    select_distinct= select_distinct && (const_tables != table_count);
  }

  THD_STAGE_INFO(thd, stage_preparing);
  if (result->initialize_tables(this))
  {
    DBUG_PRINT("error",("Error: initialize_tables() failed"));
    DBUG_RETURN(1);				// error == -1
  }
  if (const_table_map != found_const_table_map &&
      !(select_options & SELECT_DESCRIBE))
  {
    // There is at least one empty const table
    zero_result_cause= "no matching row in const table";
    DBUG_PRINT("error",("Error: %s", zero_result_cause));
    error= 0;
    handle_implicit_grouping_with_window_funcs();
    goto setup_subq_exit;
  }
  if (!(thd->variables.option_bits & OPTION_BIG_SELECTS) &&
      best_read > (double) thd->variables.max_join_size &&
      !(select_options & SELECT_DESCRIBE))
  {						/* purecov: inspected */
    my_message(ER_TOO_BIG_SELECT, ER_THD(thd, ER_TOO_BIG_SELECT), MYF(0));
    error= -1;
    DBUG_RETURN(1);
  }
  if (const_tables && !thd->locked_tables_mode &&
      !(select_options & SELECT_NO_UNLOCK))
  {
    /*
      Unlock all tables, except sequences, as accessing these may still
      require table updates
    */
    mysql_unlock_some_tables(thd, table, const_tables,
                             GET_LOCK_SKIP_SEQUENCES);
  }
  if (!conds && outer_join)
  {
    /* Handle the case where we have an OUTER JOIN without a WHERE */
    conds= new (thd->mem_root) Item_int(thd, (longlong) 1,1); // Always true
  }

  if (impossible_where)
  {
    zero_result_cause=
      "Impossible WHERE noticed after reading const tables";
    select_lex->mark_const_derived(zero_result_cause);
    handle_implicit_grouping_with_window_funcs();
    goto setup_subq_exit;
  }

  select= make_select(*table, const_table_map,
                      const_table_map, conds, (SORT_INFO*) 0, 1, &error);
  if (unlikely(error))
  {						/* purecov: inspected */
    error= -1;					/* purecov: inspected */
    DBUG_PRINT("error",("Error: make_select() failed"));
    DBUG_RETURN(1);
  }
  
  reset_nj_counters(this, join_list);
  if (make_outerjoin_info(this))
  {
    DBUG_RETURN(1);
  }

  /*
    Among the equal fields belonging to the same multiple equality
    choose the one that is to be retrieved first and substitute
    all references to these in where condition for a reference for
    the selected field.
  */
  if (conds)
  {
    conds= substitute_for_best_equal_field(thd, NO_PARTICULAR_TAB, conds,
                                           cond_equal, map2table);
    if (unlikely(thd->is_error()))
    {
      error= 1;
      DBUG_PRINT("error",("Error from substitute_for_best_equal"));
      DBUG_RETURN(1);
    }
    conds->update_used_tables();
    DBUG_EXECUTE("where",
                 print_where(conds,
                             "after substitute_best_equal",
                             QT_ORDINARY););
  }

  /*
    Perform the optimization on fields evaluation mentioned above
    for all on expressions.
  */
  for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES); tab;
       tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
  {
    if (*tab->on_expr_ref)
    {
      *tab->on_expr_ref= substitute_for_best_equal_field(thd, NO_PARTICULAR_TAB,
                                                         *tab->on_expr_ref,
                                                         tab->cond_equal,
                                                         map2table);
      if (unlikely(thd->is_error()))
      {
        error= 1;
        DBUG_PRINT("error",("Error from substitute_for_best_equal"));
        DBUG_RETURN(1);
      }
      (*tab->on_expr_ref)->update_used_tables();
    }
  }

  /*
    Perform the optimization on fields evaliation mentioned above
    for all used ref items.
  */
  for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES); tab;
       tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
  {
    uint key_copy_index=0;
    for (uint i=0; i < tab->ref.key_parts; i++)
    {
      Item **ref_item_ptr= tab->ref.items+i;
      Item *ref_item= *ref_item_ptr;
      if (!ref_item->used_tables() && !(select_options & SELECT_DESCRIBE))
        continue;
      COND_EQUAL *equals= cond_equal;
      JOIN_TAB *first_inner= tab->first_inner;
      while (equals)
      {
        ref_item= substitute_for_best_equal_field(thd, tab, ref_item,
                                                  equals, map2table);
        if (unlikely(thd->is_fatal_error))
          DBUG_RETURN(1);

        if (first_inner)
	{
          equals= first_inner->cond_equal;
          first_inner= first_inner->first_upper;
        }
        else
          equals= 0;
      }  
      ref_item->update_used_tables();
      if (*ref_item_ptr != ref_item)
      {
        *ref_item_ptr= ref_item;
        Item *item= ref_item->real_item();
        store_key *key_copy= tab->ref.key_copy[key_copy_index];
        if (key_copy->type() == store_key::FIELD_STORE_KEY)
        {
          if (item->basic_const_item())
          {
            /* It is constant propagated here */
            tab->ref.key_copy[key_copy_index]=
              new store_key_const_item(*tab->ref.key_copy[key_copy_index],
                                       item);
          }
          else if (item->const_item())
	  {
            tab->ref.key_copy[key_copy_index]=
              new store_key_item(*tab->ref.key_copy[key_copy_index],
                                 item, TRUE);
          }            
          else
          {
            store_key_field *field_copy= ((store_key_field *)key_copy);
            DBUG_ASSERT(item->type() == Item::FIELD_ITEM);
            field_copy->change_source_field((Item_field *) item);
          }
        }
      }
      key_copy_index++;
    }
  }

  if (conds && const_table_map != found_const_table_map &&
      (select_options & SELECT_DESCRIBE))
  {
    conds=new (thd->mem_root) Item_int(thd, (longlong) 0, 1); // Always false
  }

  /* Cache constant expressions in WHERE, HAVING, ON clauses. */
  cache_const_exprs();

  if (setup_semijoin_loosescan(this))
    DBUG_RETURN(1);

  if (make_join_select(this, select, conds))
  {
    zero_result_cause=
      "Impossible WHERE noticed after reading const tables";
    select_lex->mark_const_derived(zero_result_cause);
    handle_implicit_grouping_with_window_funcs();
    goto setup_subq_exit;
  }

  error= -1;					/* if goto err */

  /* Optimize distinct away if possible */
  {
    ORDER *org_order= order;
    order=remove_const(this, order,conds,1, &simple_order);
    if (unlikely(thd->is_error()))
    {
      error= 1;
      DBUG_RETURN(1);
    }

    /*
      If we are using ORDER BY NULL or ORDER BY const_expression,
      return result in any order (even if we are using a GROUP BY)
    */
    if (!order && org_order)
      skip_sort_order= 1;
  }
  /*
     Check if we can optimize away GROUP BY/DISTINCT.
     We can do that if there are no aggregate functions, the
     fields in DISTINCT clause (if present) and/or columns in GROUP BY
     (if present) contain direct references to all key parts of
     an unique index (in whatever order) and if the key parts of the
     unique index cannot contain NULLs.
     Note that the unique keys for DISTINCT and GROUP BY should not
     be the same (as long as they are unique).

     The FROM clause must contain a single non-constant table.
  */
  if (table_count - const_tables == 1 && (group || select_distinct) &&
      !tmp_table_param.sum_func_count &&
      (!join_tab[const_tables].select ||
       !join_tab[const_tables].select->quick ||
       join_tab[const_tables].select->quick->get_type() != 
       QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX) &&
      !select_lex->have_window_funcs())
  {
    if (group && rollup.state == ROLLUP::STATE_NONE &&
       list_contains_unique_index(join_tab[const_tables].table,
                                 find_field_in_order_list,
                                 (void *) group_list))
    {
      /*
        We have found that grouping can be removed since groups correspond to
        only one row anyway, but we still have to guarantee correct result
        order. The line below effectively rewrites the query from GROUP BY
        <fields> to ORDER BY <fields>. There are three exceptions:
        - if skip_sort_order is set (see above), then we can simply skip
          GROUP BY;
        - if we are in a subquery, we don't have to maintain order unless there
	  is a limit clause in the subquery.
        - we can only rewrite ORDER BY if the ORDER BY fields are 'compatible'
          with the GROUP BY ones, i.e. either one is a prefix of another.
          We only check if the ORDER BY is a prefix of GROUP BY. In this case
          test_if_subpart() copies the ASC/DESC attributes from the original
          ORDER BY fields.
          If GROUP BY is a prefix of ORDER BY, then it is safe to leave
          'order' as is.
       */
      if (!order || test_if_subpart(group_list, order))
      {
        if (skip_sort_order ||
            (select_lex->master_unit()->item && select_limit == HA_POS_ERROR)) // This is a subquery
          order= NULL;
        else
          order= group_list;
      }
      /*
        If we have an IGNORE INDEX FOR GROUP BY(fields) clause, this must be 
        rewritten to IGNORE INDEX FOR ORDER BY(fields).
      */
      join_tab->table->keys_in_use_for_order_by=
        join_tab->table->keys_in_use_for_group_by;
      group_list= 0;
      group= 0;
    }
    if (select_distinct &&
       list_contains_unique_index(join_tab[const_tables].table,
                                 find_field_in_item_list,
                                 (void *) &fields_list))
    {
      select_distinct= 0;
    }
  }
  if (group || tmp_table_param.sum_func_count)
  {
    if (! hidden_group_fields && rollup.state == ROLLUP::STATE_NONE
        && !select_lex->have_window_funcs())
      select_distinct=0;
  }
  else if (select_distinct && table_count - const_tables == 1 &&
           rollup.state == ROLLUP::STATE_NONE &&
           !select_lex->have_window_funcs())
  {
    /*
      We are only using one table. In this case we change DISTINCT to a
      GROUP BY query if:
      - The GROUP BY can be done through indexes (no sort) and the ORDER
        BY only uses selected fields.
	(In this case we can later optimize away GROUP BY and ORDER BY)
      - We are scanning the whole table without LIMIT
        This can happen if:
        - We are using CALC_FOUND_ROWS
        - We are using an ORDER BY that can't be optimized away.

      We don't want to use this optimization when we are using LIMIT
      because in this case we can just create a temporary table that
      holds LIMIT rows and stop when this table is full.
    */
    bool all_order_fields_used;

    tab= &join_tab[const_tables];
    if (order)
    {
      skip_sort_order=
        test_if_skip_sort_order(tab, order, select_limit,
                                true,           // no_changes
                                &tab->table->keys_in_use_for_order_by);
    }
    if ((group_list=create_distinct_group(thd, select_lex->ref_pointer_array,
                                          order, fields_list, all_fields,
				          &all_order_fields_used)))
    {
      const bool skip_group=
        skip_sort_order &&
        test_if_skip_sort_order(tab, group_list, select_limit,
                                  true,         // no_changes
                                  &tab->table->keys_in_use_for_group_by);
      count_field_types(select_lex, &tmp_table_param, all_fields, 0);
      if ((skip_group && all_order_fields_used) ||
	  select_limit == HA_POS_ERROR ||
	  (order && !skip_sort_order))
      {
	/*  Change DISTINCT to GROUP BY */
	select_distinct= 0;
	no_order= !order;
	if (all_order_fields_used)
	{
	  if (order && skip_sort_order)
	  {
	    /*
	      Force MySQL to read the table in sorted order to get result in
	      ORDER BY order.
	    */
	    tmp_table_param.quick_group=0;
	  }
	  order=0;
        }
	group=1;				// For end_write_group
      }
      else
	group_list= 0;
    }
    else if (thd->is_fatal_error)			// End of memory
      DBUG_RETURN(1);
  }
  simple_group= rollup.state == ROLLUP::STATE_NONE;
  if (group)
  {
    /*
      Update simple_group and group_list as we now have more information, like
      which tables or columns are constant.
    */
    group_list= remove_const(this, group_list, conds,
                             rollup.state == ROLLUP::STATE_NONE,
                             &simple_group);
    if (unlikely(thd->is_error()))
    {
      error= 1;
      DBUG_RETURN(1);
    }
    if (!group_list)
    {
      /* The output has only one row */
      order=0;
      simple_order=1;
      select_distinct= 0;
      group_optimized_away= 1;
    }
  }

  calc_group_buffer(this, group_list);
  send_group_parts= tmp_table_param.group_parts; /* Save org parts */
  if (procedure && procedure->group)
  {
    group_list= procedure->group= remove_const(this, procedure->group, conds,
					       1, &simple_group);
    if (unlikely(thd->is_error()))
    {
      error= 1;
      DBUG_RETURN(1);
    }   
    calc_group_buffer(this, group_list);
  }

  if (test_if_subpart(group_list, order) ||
      (!group_list && tmp_table_param.sum_func_count))
  {
    order=0;
    if (is_indexed_agg_distinct(this, NULL))
      sort_and_group= 0;
  }

  // Can't use sort on head table if using join buffering
  if (full_join || hash_join)
  {
    TABLE *stable= (sort_by_table == (TABLE *) 1 ? 
      join_tab[const_tables].table : sort_by_table);
    /* 
      FORCE INDEX FOR ORDER BY can be used to prevent join buffering when
      sorting on the first table.
    */
    if (!stable || (!stable->force_index_order &&
                    !map2table[stable->tablenr]->keep_current_rowid))
    {
      if (group_list)
        simple_group= 0;
      if (order)
        simple_order= 0;
    }
  }

  need_tmp= test_if_need_tmp_table();

  /*
    If window functions are present then we can't have simple_order set to
    TRUE as the window function needs a temp table for computation.
    ORDER BY is computed after the window function computation is done, so
    the sort will be done on the temp table.
  */
  if (select_lex->have_window_funcs())
    simple_order= FALSE;


  /*
    If the hint FORCE INDEX FOR ORDER BY/GROUP BY is used for the table
    whose columns are required to be returned in a sorted order, then
    the proper value for no_jbuf_after should be yielded by a call to
    the make_join_orderinfo function.
    Yet the current implementation of FORCE INDEX hints does not
    allow us to do it in a clean manner.
  */
  no_jbuf_after= 1 ? table_count : make_join_orderinfo(this);

  // Don't use join buffering when we use MATCH
  select_opts_for_readinfo=
    (select_options & (SELECT_DESCRIBE | SELECT_NO_JOIN_CACHE)) |
    (select_lex->ftfunc_list->elements ?  SELECT_NO_JOIN_CACHE : 0);

  if (select_lex->options & OPTION_SCHEMA_TABLE &&
       optimize_schema_tables_reads(this))
    DBUG_RETURN(1);

  if (make_join_readinfo(this, select_opts_for_readinfo, no_jbuf_after))
    DBUG_RETURN(1);

  /* Perform FULLTEXT search before all regular searches */
  if (!(select_options & SELECT_DESCRIBE))
    if (init_ftfuncs(thd, select_lex, MY_TEST(order)))
      DBUG_RETURN(1);

  /*
    It's necessary to check const part of HAVING cond as
    there is a chance that some cond parts may become
    const items after make_join_statistics(for example
    when Item is a reference to cost table field from
    outer join).
    This check is performed only for those conditions
    which do not use aggregate functions. In such case
    temporary table may not be used and const condition
    elements may be lost during further having
    condition transformation in JOIN::exec.
  */
  if (having && const_table_map && !having->with_sum_func)
  {
    having->update_used_tables();
    having= having->remove_eq_conds(thd, &select_lex->having_value, true);
    if (select_lex->having_value == Item::COND_FALSE)
    {
      having= new (thd->mem_root) Item_int(thd, (longlong) 0,1);
      zero_result_cause= "Impossible HAVING noticed after reading const tables";
      error= 0;
      select_lex->mark_const_derived(zero_result_cause);
      goto setup_subq_exit;
    }
  }

  if (optimize_unflattened_subqueries())
    DBUG_RETURN(1);
  
  int res;
  if ((res= rewrite_to_index_subquery_engine(this)) != -1)
    DBUG_RETURN(res);
  if (setup_subquery_caches())
    DBUG_RETURN(-1);

  /*
    Need to tell handlers that to play it safe, it should fetch all
    columns of the primary key of the tables: this is because MySQL may
    build row pointers for the rows, and for all columns of the primary key
    the read set has not necessarily been set by the server code.
  */
  if (need_tmp || select_distinct || group_list || order)
  {
    for (uint i= 0; i < table_count; i++)
    {
      if (!(table[i]->map & const_table_map))
        table[i]->prepare_for_position();
    }
  }

  DBUG_EXECUTE("info",TEST_join(this););

  if (!only_const_tables())
  {
     JOIN_TAB *tab= &join_tab[const_tables];

    if (order)
    {
      /*
        Force using of tmp table if sorting by a SP or UDF function due to
        their expensive and probably non-deterministic nature.
      */
      for (ORDER *tmp_order= order; tmp_order ; tmp_order=tmp_order->next)
      {
        Item *item= *tmp_order->item;
        if (item->is_expensive())
        {
          /* Force tmp table without sort */
          need_tmp=1; simple_order=simple_group=0;
          break;
        }
      }
    }

    /*
      Because filesort always does a full table scan or a quick range scan
      we must add the removed reference to the select for the table.
      We only need to do this when we have a simple_order or simple_group
      as in other cases the join is done before the sort.
    */
    if ((order || group_list) &&
        tab->type != JT_ALL &&
        tab->type != JT_FT &&
        tab->type != JT_REF_OR_NULL &&
        ((order && simple_order) || (group_list && simple_group)))
    {
      if (add_ref_to_table_cond(thd,tab)) {
        DBUG_RETURN(1);
      }
    }
    /*
      Investigate whether we may use an ordered index as part of either
      DISTINCT, GROUP BY or ORDER BY execution. An ordered index may be
      used for only the first of any of these terms to be executed. This
      is reflected in the order which we check for test_if_skip_sort_order()
      below. However we do not check for DISTINCT here, as it would have
      been transformed to a GROUP BY at this stage if it is a candidate for 
      ordered index optimization.
      If a decision was made to use an ordered index, the availability
      of such an access path is stored in 'ordered_index_usage' for later
      use by 'execute' or 'explain'
    */
    DBUG_ASSERT(ordered_index_usage == ordered_index_void);

    if (group_list)   // GROUP BY honoured first
                      // (DISTINCT was rewritten to GROUP BY if skippable)
    {
      /*
        When there is SQL_BIG_RESULT do not sort using index for GROUP BY,
        and thus force sorting on disk unless a group min-max optimization
        is going to be used as it is applied now only for one table queries
        with covering indexes.
      */
      if (!(select_options & SELECT_BIG_RESULT) ||
            (tab->select &&
             tab->select->quick &&
             tab->select->quick->get_type() ==
             QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX))
      {
        if (simple_group &&              // GROUP BY is possibly skippable
            !select_distinct)            // .. if not preceded by a DISTINCT
        {
          /*
            Calculate a possible 'limit' of table rows for 'GROUP BY':
            A specified 'LIMIT' is relative to the final resultset.
            'need_tmp' implies that there will be more postprocessing 
            so the specified 'limit' should not be enforced yet.
           */
          const ha_rows limit = need_tmp ? HA_POS_ERROR : select_limit;
          if (test_if_skip_sort_order(tab, group_list, limit, false, 
                                      &tab->table->keys_in_use_for_group_by))
          {
            ordered_index_usage= ordered_index_group_by;
          }
        }

	/*
	  If we are going to use semi-join LooseScan, it will depend
	  on the selected index scan to be used.  If index is not used
	  for the GROUP BY, we risk that sorting is put on the LooseScan
	  table.  In order to avoid this, force use of temporary table.
	  TODO: Explain the quick_group part of the test below.
	 */
        if ((ordered_index_usage != ordered_index_group_by) &&
            ((tmp_table_param.quick_group && !procedure) || 
	     (tab->emb_sj_nest && 
	      best_positions[const_tables].sj_strategy == SJ_OPT_LOOSE_SCAN)))
        {
          need_tmp=1;
          simple_order= simple_group= false; // Force tmp table without sort
        }
      }
    }
    else if (order &&                      // ORDER BY wo/ preceding GROUP BY
             (simple_order || skip_sort_order)) // which is possibly skippable
    {
      if (test_if_skip_sort_order(tab, order, select_limit, false, 
                                  &tab->table->keys_in_use_for_order_by))
      {
        ordered_index_usage= ordered_index_order_by;
      }
    }
  }

  if (having)
    having_is_correlated= MY_TEST(having->used_tables() & OUTER_REF_TABLE_BIT);
  tmp_having= having;

  if (unlikely(thd->is_error()))
    DBUG_RETURN(TRUE);

  /*
    The loose index scan access method guarantees that all grouping or
    duplicate row elimination (for distinct) is already performed
    during data retrieval, and that all MIN/MAX functions are already
    computed for each group. Thus all MIN/MAX functions should be
    treated as regular functions, and there is no need to perform
    grouping in the main execution loop.
    Notice that currently loose index scan is applicable only for
    single table queries, thus it is sufficient to test only the first
    join_tab element of the plan for its access method.
  */
  if (join_tab->is_using_loose_index_scan())
  {
    tmp_table_param.precomputed_group_by= TRUE;
    if (join_tab->is_using_agg_loose_index_scan())
    {
      need_distinct= FALSE;
      tmp_table_param.precomputed_group_by= FALSE;
    }
  }

  if (make_aggr_tables_info())
    DBUG_RETURN(1);

  if (init_join_caches())
    DBUG_RETURN(1);

  error= 0;

  if (select_options & SELECT_DESCRIBE)
    goto derived_exit;

  DBUG_RETURN(0);

setup_subq_exit:
  /* Choose an execution strategy for this JOIN. */
  if (!tables_list || !table_count)
  {
    choose_tableless_subquery_plan();

    /* The output has atmost one row */
    if (group_list)
    {
      group_list= NULL;
      group_optimized_away= 1;
      rollup.state= ROLLUP::STATE_NONE;
    }
    order= NULL;
    simple_order= TRUE;
    select_distinct= FALSE;

    if (select_lex->have_window_funcs())
    {
      if (!(join_tab= (JOIN_TAB*) thd->alloc(sizeof(JOIN_TAB))))
        DBUG_RETURN(1);
      need_tmp= 1;
    }
    if (make_aggr_tables_info())
      DBUG_RETURN(1);
  }
  /*
    Even with zero matching rows, subqueries in the HAVING clause may
    need to be evaluated if there are aggregate functions in the query.
  */
  if (optimize_unflattened_subqueries())
    DBUG_RETURN(1);
  error= 0;

derived_exit:

  select_lex->mark_const_derived(zero_result_cause);
  DBUG_RETURN(0);
}

/**
  Add having condition as a where clause condition of the given temp table.

  @param    tab   Table to which having condition is added.

  @returns  false if success, true if error.
*/

bool JOIN::add_having_as_table_cond(JOIN_TAB *tab)
{
  tmp_having->update_used_tables();
  table_map used_tables= tab->table->map | OUTER_REF_TABLE_BIT;

  /* If tmp table is not used then consider conditions of const table also */
  if (!need_tmp)
    used_tables|= const_table_map;

  DBUG_ENTER("JOIN::add_having_as_table_cond");

  Item* sort_table_cond= make_cond_for_table(thd, tmp_having, used_tables,
                                             (table_map) 0, false,
                                             false, false);
  if (sort_table_cond)
  {
    if (!tab->select)
    {
      if (!(tab->select= new SQL_SELECT))
        DBUG_RETURN(true);
      tab->select->head= tab->table;
    }
    if (!tab->select->cond)
      tab->select->cond= sort_table_cond;
    else
    {
      if (!(tab->select->cond=
	      new (thd->mem_root) Item_cond_and(thd,
                                                tab->select->cond,
                                                sort_table_cond)))
        DBUG_RETURN(true);
    }
    if (tab->pre_idx_push_select_cond)
    {
      if (sort_table_cond->type() == Item::COND_ITEM)
        sort_table_cond= sort_table_cond->copy_andor_structure(thd);
      if (!(tab->pre_idx_push_select_cond=
              new (thd->mem_root) Item_cond_and(thd,
                                                tab->pre_idx_push_select_cond,
                                                sort_table_cond)))
        DBUG_RETURN(true);
    }
    if (tab->select->cond)
      tab->select->cond->fix_fields_if_needed(thd, 0);
    if (tab->pre_idx_push_select_cond)
      tab->pre_idx_push_select_cond->fix_fields_if_needed(thd, 0);
    tab->select->pre_idx_push_select_cond= tab->pre_idx_push_select_cond;
    tab->set_select_cond(tab->select->cond, __LINE__);
    tab->select_cond->top_level_item();
    DBUG_EXECUTE("where",print_where(tab->select->cond,
				     "select and having",
                                     QT_ORDINARY););

    having= make_cond_for_table(thd, tmp_having, ~ (table_map) 0,
                                ~used_tables, false, false, false);
    DBUG_EXECUTE("where",
                 print_where(having, "having after sort", QT_ORDINARY););
  }

  DBUG_RETURN(false);
}


bool JOIN::add_fields_for_current_rowid(JOIN_TAB *cur, List<Item> *table_fields)
{
  /*
    this will not walk into semi-join materialization nests but this is ok
    because we will never need to save current rowids for those.
  */
  for (JOIN_TAB *tab=join_tab; tab < cur; tab++)
  {
    if (!tab->keep_current_rowid)
      continue;
    Item *item= new (thd->mem_root) Item_temptable_rowid(tab->table);
    item->fix_fields(thd, 0);
    table_fields->push_back(item, thd->mem_root);
    cur->tmp_table_param->func_count++;
  }
  return 0;
}


/**
  Set info for aggregation tables

  @details
  This function finalizes execution plan by taking following actions:
    .) aggregation temporary tables are created, but not instantiated 
       (this is done during execution).
       JOIN_TABs for aggregation tables are set appropriately
       (see JOIN::create_postjoin_aggr_table).
    .) prepare fields lists (fields, all_fields, ref_pointer_array slices) for
       each required stage of execution. These fields lists are set for
       working tables' tabs and for the tab of last table in the join.
    .) info for sorting/grouping/dups removal is prepared and saved in
       appropriate tabs. Here is an example:

  @returns
  false - Ok
  true  - Error
*/

bool JOIN::make_aggr_tables_info()
{
  List<Item> *curr_all_fields= &all_fields;
  List<Item> *curr_fields_list= &fields_list;
  JOIN_TAB *curr_tab= join_tab + const_tables;
  TABLE *exec_tmp_table= NULL;
  bool distinct= false;
  bool keep_row_order= false;
  bool is_having_added_as_table_cond= false;
  DBUG_ENTER("JOIN::make_aggr_tables_info");

  const bool has_group_by= this->group;
  
  sort_and_group_aggr_tab= NULL;

  if (group_optimized_away)
    implicit_grouping= true;

  bool implicit_grouping_with_window_funcs= implicit_grouping &&
                                            select_lex->have_window_funcs();
  bool implicit_grouping_without_tables= implicit_grouping &&
                                         !tables_list;

  /*
    Setup last table to provide fields and all_fields lists to the next
    node in the plan.
  */
  if (join_tab && top_join_tab_count && tables_list)
  {
    join_tab[top_join_tab_count - 1].fields= &fields_list;
    join_tab[top_join_tab_count - 1].all_fields= &all_fields;
  }

  /*
    All optimization is done. Check if we can use the storage engines
    group by handler to evaluate the group by.
    Some storage engines, like spider can also do joins, group by and
    distinct in the engine, so we do this for all queries, not only
    GROUP BY queries.
  */
  if (tables_list && top_join_tab_count && !procedure)
  {
    /*
      At the moment we only support push down for queries where
      all tables are in the same storage engine
    */
    TABLE_LIST *tbl= tables_list;
    handlerton *ht= tbl && tbl->table ? tbl->table->file->partition_ht() : 0;
    for (tbl= tbl->next_local; ht && tbl; tbl= tbl->next_local)
    {
      if (!tbl->table || tbl->table->file->partition_ht() != ht)
        ht= 0;
    }

    if (ht && ht->create_group_by)
    {
      /* Check if the storage engine can intercept the query */
      Query query= {&all_fields, select_distinct, tables_list, conds,
                    group_list, order ? order : group_list, having};
      group_by_handler *gbh= ht->create_group_by(thd, &query);

      if (gbh)
      {
        if (!(pushdown_query= new (thd->mem_root) Pushdown_query(select_lex, gbh)))
          DBUG_RETURN(1);
        /*
          We must store rows in the tmp table if we need to do an ORDER BY
          or DISTINCT and the storage handler can't handle it.
        */
        need_tmp= query.order_by || query.group_by || query.distinct;
        distinct= query.distinct;
        keep_row_order= query.order_by || query.group_by;
        
        order= query.order_by;

        aggr_tables++;
        curr_tab= join_tab + exec_join_tab_cnt();
        bzero((void*)curr_tab, sizeof(JOIN_TAB));
        curr_tab->ref.key= -1;
        curr_tab->join= this;

        if (!(curr_tab->tmp_table_param= new TMP_TABLE_PARAM(tmp_table_param)))
          DBUG_RETURN(1);
        TABLE* table= create_tmp_table(thd, curr_tab->tmp_table_param,
                                       all_fields,
                                       NULL, query.distinct,
                                       TRUE, select_options, HA_POS_ERROR,
                                       &empty_clex_str, !need_tmp,
                                       query.order_by || query.group_by);
        if (!table)
          DBUG_RETURN(1);

        if (!(curr_tab->aggr= new (thd->mem_root) AGGR_OP(curr_tab)))
          DBUG_RETURN(1);
        curr_tab->aggr->set_write_func(::end_send);
        curr_tab->table= table;
        /*
          Setup reference fields, used by summary functions and group by fields,
          to point to the temporary table.
          The actual switching to the temporary tables fields for HAVING
          and ORDER BY is done in do_select() by calling
          set_items_ref_array(items1).
        */
        init_items_ref_array();
        items1= ref_ptr_array_slice(2);
        //items1= items0 + all_fields.elements;
        if (change_to_use_tmp_fields(thd, items1,
                                     tmp_fields_list1, tmp_all_fields1,
                                     fields_list.elements, all_fields))
          DBUG_RETURN(1);

        /* Give storage engine access to temporary table */
        gbh->table= table;
        pushdown_query->store_data_in_temp_table= need_tmp;
        pushdown_query->having= having;

        /*
          Group by and having is calculated by the group_by handler.
          Reset the group by and having
        */
        DBUG_ASSERT(query.group_by == NULL);
        group= 0; group_list= 0;
        having= tmp_having= 0;
        /*
          Select distinct is handled by handler or by creating an unique index
          over all fields in the temporary table
        */
        select_distinct= 0;
        order= query.order_by;
        tmp_table_param.field_count+= tmp_table_param.sum_func_count;
        tmp_table_param.sum_func_count= 0;

        fields= curr_fields_list;

        //todo: new:
        curr_tab->ref_array= &items1;
        curr_tab->all_fields= &tmp_all_fields1;
        curr_tab->fields= &tmp_fields_list1;

        DBUG_RETURN(thd->is_fatal_error);
      }
    }
  }


  /*
    The loose index scan access method guarantees that all grouping or
    duplicate row elimination (for distinct) is already performed
    during data retrieval, and that all MIN/MAX functions are already
    computed for each group. Thus all MIN/MAX functions should be
    treated as regular functions, and there is no need to perform
    grouping in the main execution loop.
    Notice that currently loose index scan is applicable only for
    single table queries, thus it is sufficient to test only the first
    join_tab element of the plan for its access method.
  */
  if (join_tab && top_join_tab_count && tables_list &&
      join_tab->is_using_loose_index_scan())
    tmp_table_param.precomputed_group_by=
      !join_tab->is_using_agg_loose_index_scan();

  group_list_for_estimates= group_list;
  /* Create a tmp table if distinct or if the sort is too complicated */
  if (need_tmp)
  {
    aggr_tables++;
    curr_tab= join_tab + exec_join_tab_cnt();
    bzero((void*)curr_tab, sizeof(JOIN_TAB));
    curr_tab->ref.key= -1;
    if (only_const_tables())
      first_select= sub_select_postjoin_aggr;

    /*
      Create temporary table on first execution of this join.
      (Will be reused if this is a subquery that is executed several times.)
    */
    init_items_ref_array();

    ORDER *tmp_group= (ORDER *) 0;
    if (!simple_group && !procedure && !(test_flags & TEST_NO_KEY_GROUP))
      tmp_group= group_list;

    tmp_table_param.hidden_field_count= 
      all_fields.elements - fields_list.elements;

    distinct= select_distinct && !group_list && 
              !select_lex->have_window_funcs();
    keep_row_order= false;
    bool save_sum_fields= (group_list && simple_group) ||
                           implicit_grouping_with_window_funcs;
    if (create_postjoin_aggr_table(curr_tab,
                                   &all_fields, tmp_group,
                                   save_sum_fields,
                                   distinct, keep_row_order))
      DBUG_RETURN(true);
    exec_tmp_table= curr_tab->table;

    if (exec_tmp_table->distinct)
      optimize_distinct();

   /* Change sum_fields reference to calculated fields in tmp_table */
    items1= ref_ptr_array_slice(2);
    if ((sort_and_group || curr_tab->table->group ||
         tmp_table_param.precomputed_group_by) && 
         !implicit_grouping_without_tables)
    {
      if (change_to_use_tmp_fields(thd, items1,
                                   tmp_fields_list1, tmp_all_fields1,
                                   fields_list.elements, all_fields))
        DBUG_RETURN(true);
    }
    else
    {
      if (change_refs_to_tmp_fields(thd, items1,
                                    tmp_fields_list1, tmp_all_fields1,
                                    fields_list.elements, all_fields))
        DBUG_RETURN(true);
    }
    curr_all_fields= &tmp_all_fields1;
    curr_fields_list= &tmp_fields_list1;
    // Need to set them now for correct group_fields setup, reset at the end.
    set_items_ref_array(items1);
    curr_tab->ref_array= &items1;
    curr_tab->all_fields= &tmp_all_fields1;
    curr_tab->fields= &tmp_fields_list1;
    set_postjoin_aggr_write_func(curr_tab);

    /*
      If having is not handled here, it will be checked before the row is sent
      to the client.
    */
    if (tmp_having &&
        (sort_and_group || (exec_tmp_table->distinct && !group_list) ||
	 select_lex->have_window_funcs()))
    {
      /*
        If there is no select distinct and there are no window functions
        then move the having to table conds of tmp table.
        NOTE : We cannot apply having after distinct or window functions
               If columns of having are not part of select distinct,
               then distinct may remove rows which can satisfy having.
               In the case of window functions we *must* make sure to not
               store any rows which don't match HAVING within the temp table,
               as rows will end up being used during their computation.
      */
      if (!select_distinct && !select_lex->have_window_funcs() &&
          add_having_as_table_cond(curr_tab))
        DBUG_RETURN(true);
      is_having_added_as_table_cond= tmp_having != having;

      /*
        Having condition which we are not able to add as tmp table conds are
        kept as before. And, this will be applied before storing the rows in
        tmp table.
      */
      curr_tab->having= having;
      having= NULL; // Already done
    }

    tmp_table_param.func_count= 0;
    tmp_table_param.field_count+= tmp_table_param.func_count;
    if (sort_and_group || curr_tab->table->group)
    {
      tmp_table_param.field_count+= tmp_table_param.sum_func_count;
      tmp_table_param.sum_func_count= 0;
    }

    if (exec_tmp_table->group)
    {						// Already grouped
      if (!order && !no_order && !skip_sort_order)
        order= group_list;  /* order by group */
      group_list= NULL;
    }

    /*
      If we have different sort & group then we must sort the data by group
      and copy it to another tmp table
      This code is also used if we are using distinct something
      we haven't been able to store in the temporary table yet
      like SEC_TO_TIME(SUM(...)).
    */
    if ((group_list &&
         (!test_if_subpart(group_list, order) || select_distinct)) ||
        (select_distinct && tmp_table_param.using_outer_summary_function))
    {					/* Must copy to another table */
      DBUG_PRINT("info",("Creating group table"));

      calc_group_buffer(this, group_list);
      count_field_types(select_lex, &tmp_table_param, tmp_all_fields1,
                        select_distinct && !group_list);
      tmp_table_param.hidden_field_count=
        tmp_all_fields1.elements - tmp_fields_list1.elements;

      curr_tab++;
      aggr_tables++;
      bzero((void*)curr_tab, sizeof(JOIN_TAB));
      curr_tab->ref.key= -1;

      /* group data to new table */
      /*
        If the access method is loose index scan then all MIN/MAX
        functions are precomputed, and should be treated as regular
        functions. See extended comment above.
      */
      if (join_tab->is_using_loose_index_scan())
        tmp_table_param.precomputed_group_by= TRUE;

      tmp_table_param.hidden_field_count=
        curr_all_fields->elements - curr_fields_list->elements;
      ORDER *dummy= NULL; //TODO can use table->group here also

      if (create_postjoin_aggr_table(curr_tab, curr_all_fields, dummy, true,
                                     distinct, keep_row_order))
	DBUG_RETURN(true);

      if (group_list)
      {
        if (!only_const_tables())        // No need to sort a single row
        {
          if (add_sorting_to_table(curr_tab - 1, group_list))
            DBUG_RETURN(true);
        }

        if (make_group_fields(this, this))
          DBUG_RETURN(true);
      }

      // Setup sum funcs only when necessary, otherwise we might break info
      // for the first table
      if (group_list || tmp_table_param.sum_func_count)
      {
        if (make_sum_func_list(*curr_all_fields, *curr_fields_list, true, true))
          DBUG_RETURN(true);
        if (prepare_sum_aggregators(sum_funcs,
                                    !join_tab->is_using_agg_loose_index_scan()))
          DBUG_RETURN(true);
        group_list= NULL;
        if (setup_sum_funcs(thd, sum_funcs))
          DBUG_RETURN(true);
      }
      // No sum funcs anymore
      DBUG_ASSERT(items2.is_null());

      items2= ref_ptr_array_slice(3);
      if (change_to_use_tmp_fields(thd, items2,
                                   tmp_fields_list2, tmp_all_fields2, 
                                   fields_list.elements, tmp_all_fields1))
        DBUG_RETURN(true);

      curr_fields_list= &tmp_fields_list2;
      curr_all_fields= &tmp_all_fields2;
      set_items_ref_array(items2);
      curr_tab->ref_array= &items2;
      curr_tab->all_fields= &tmp_all_fields2;
      curr_tab->fields= &tmp_fields_list2;
      set_postjoin_aggr_write_func(curr_tab);

      tmp_table_param.field_count+= tmp_table_param.sum_func_count;
      tmp_table_param.sum_func_count= 0;
    }
    if (curr_tab->table->distinct)
      select_distinct= false;               /* Each row is unique */

    if (select_distinct && !group_list)
    {
      if (having)
      {
        curr_tab->having= having;
        having->update_used_tables();
      }
      /*
        We only need DISTINCT operation if the join is not degenerate.
        If it is, we must not request DISTINCT processing, because
        remove_duplicates() assumes there is a preceding computation step (and
        in the degenerate join, there's none)
      */
      if (top_join_tab_count && tables_list)
        curr_tab->distinct= true;

      having= NULL;
      select_distinct= false;
    }
    /* Clean tmp_table_param for the next tmp table. */
    tmp_table_param.field_count= tmp_table_param.sum_func_count=
      tmp_table_param.func_count= 0;

    tmp_table_param.copy_field= tmp_table_param.copy_field_end=0;
    first_record= sort_and_group=0;

    if (!group_optimized_away || implicit_grouping_with_window_funcs)
    {
      group= false;
    }
    else
    {
      /*
        If grouping has been optimized away, a temporary table is
        normally not needed unless we're explicitly requested to create
        one (e.g. due to a SQL_BUFFER_RESULT hint or INSERT ... SELECT).

        In this case (grouping was optimized away), temp_table was
        created without a grouping expression and JOIN::exec() will not
        perform the necessary grouping (by the use of end_send_group()
        or end_write_group()) if JOIN::group is set to false.
      */
      // the temporary table was explicitly requested
      DBUG_ASSERT(MY_TEST(select_options & OPTION_BUFFER_RESULT));
      // the temporary table does not have a grouping expression
      DBUG_ASSERT(!curr_tab->table->group); 
    }
    calc_group_buffer(this, group_list);
    count_field_types(select_lex, &tmp_table_param, *curr_all_fields, false);
  }

  if (group ||
      (implicit_grouping  && !implicit_grouping_with_window_funcs) ||
      tmp_table_param.sum_func_count)
  {
    if (make_group_fields(this, this))
      DBUG_RETURN(true);

    DBUG_ASSERT(items3.is_null());

    if (items0.is_null())
      init_items_ref_array();
    items3= ref_ptr_array_slice(4);
    setup_copy_fields(thd, &tmp_table_param,
                      items3, tmp_fields_list3, tmp_all_fields3,
                      curr_fields_list->elements, *curr_all_fields);

    curr_fields_list= &tmp_fields_list3;
    curr_all_fields= &tmp_all_fields3;
    set_items_ref_array(items3);
    if (join_tab)
    {
      JOIN_TAB *last_tab= join_tab + top_join_tab_count + aggr_tables - 1;
      // Set grouped fields on the last table
      last_tab->ref_array= &items3;
      last_tab->all_fields= &tmp_all_fields3;
      last_tab->fields= &tmp_fields_list3;
    }
    if (make_sum_func_list(*curr_all_fields, *curr_fields_list, true, true))
      DBUG_RETURN(true);
    if (prepare_sum_aggregators(sum_funcs,
                                !join_tab ||
                                !join_tab-> is_using_agg_loose_index_scan()))
      DBUG_RETURN(true);
    if (unlikely(setup_sum_funcs(thd, sum_funcs) || thd->is_fatal_error))
      DBUG_RETURN(true);
  }
  if (group_list || order)
  {
    DBUG_PRINT("info",("Sorting for send_result_set_metadata"));
    THD_STAGE_INFO(thd, stage_sorting_result);
    /* If we have already done the group, add HAVING to sorted table */
    if (tmp_having && !is_having_added_as_table_cond &&
        !group_list && !sort_and_group)
    {
      if (add_having_as_table_cond(curr_tab))
        DBUG_RETURN(true);
    }

    if (group)
      select_limit= HA_POS_ERROR;
    else if (!need_tmp)
    {
      /*
        We can abort sorting after thd->select_limit rows if there are no
        filter conditions for any tables after the sorted one.
        Filter conditions come in several forms:
         1. as a condition item attached to the join_tab, or
         2. as a keyuse attached to the join_tab (ref access).
      */
      for (uint i= const_tables + 1; i < top_join_tab_count; i++)
      {
        JOIN_TAB *const tab= join_tab + i;
        if (tab->select_cond ||                                // 1
            (tab->keyuse && !tab->first_inner))                // 2
        {
          /* We have to sort all rows */
          select_limit= HA_POS_ERROR;
          break;
        }
      }
    }
    /*
      Here we add sorting stage for ORDER BY/GROUP BY clause, if the
      optimiser chose FILESORT to be faster than INDEX SCAN or there is
      no suitable index present.
      OPTION_FOUND_ROWS supersedes LIMIT and is taken into account.
    */
    DBUG_PRINT("info",("Sorting for order by/group by"));
    ORDER *order_arg= group_list ?  group_list : order;
    if (top_join_tab_count + aggr_tables > const_tables &&
        ordered_index_usage !=
        (group_list ? ordered_index_group_by : ordered_index_order_by) &&
        curr_tab->type != JT_CONST &&
        curr_tab->type != JT_EQ_REF) // Don't sort 1 row
    {
      // Sort either first non-const table or the last tmp table
      JOIN_TAB *sort_tab= curr_tab;

      if (add_sorting_to_table(sort_tab, order_arg))
        DBUG_RETURN(true);
      /*
        filesort_limit:	 Return only this many rows from filesort().
        We can use select_limit_cnt only if we have no group_by and 1 table.
        This allows us to use Bounded_queue for queries like:
          "select SQL_CALC_FOUND_ROWS * from t1 order by b desc limit 1;"
        m_select_limit == HA_POS_ERROR (we need a full table scan)
        unit->select_limit_cnt == 1 (we only need one row in the result set)
      */
      sort_tab->filesort->limit=
        (has_group_by || (join_tab + table_count > curr_tab + 1)) ?
         select_limit : unit->select_limit_cnt;
    }
    if (!only_const_tables() &&
        !join_tab[const_tables].filesort &&
        !(select_options & SELECT_DESCRIBE))
    {
      /*
        If no IO cache exists for the first table then we are using an
        INDEX SCAN and no filesort. Thus we should not remove the sorted
        attribute on the INDEX SCAN.
      */
      skip_sort_order= true;
    }
  }

  /*
    Window functions computation step should be attached to the last join_tab
    that's doing aggregation.
    The last join_tab reads the data from the temp. table.  It also may do
    - sorting
    - duplicate value removal
    Both of these operations are done after window function computation step.
  */
  curr_tab= join_tab + total_join_tab_cnt();
  if (select_lex->window_funcs.elements)
  {
    if (!(curr_tab->window_funcs_step= new Window_funcs_computation))
      DBUG_RETURN(true);
    if (curr_tab->window_funcs_step->setup(thd, &select_lex->window_funcs,
                                           curr_tab))
      DBUG_RETURN(true);
    /* Count that we're using window functions. */
    status_var_increment(thd->status_var.feature_window_functions);
  }
  if (select_lex->custom_agg_func_used())
    status_var_increment(thd->status_var.feature_custom_aggregate_functions);

  fields= curr_fields_list;
  // Reset before execution
  set_items_ref_array(items0);
  if (join_tab)
    join_tab[exec_join_tab_cnt() + aggr_tables - 1].next_select=
      setup_end_select_func(this, NULL);
  group= has_group_by;

  DBUG_RETURN(false);
}



bool
JOIN::create_postjoin_aggr_table(JOIN_TAB *tab, List<Item> *table_fields,
                                 ORDER *table_group,
                                 bool save_sum_fields,
                                 bool distinct,
                                 bool keep_row_order)
{
  DBUG_ENTER("JOIN::create_postjoin_aggr_table");
  THD_STAGE_INFO(thd, stage_creating_tmp_table);

  /*
    Pushing LIMIT to the post-join temporary table creation is not applicable
    when there is ORDER BY or GROUP BY or there is no GROUP BY, but
    there are aggregate functions, because in all these cases we need
    all result rows.
  */
  ha_rows table_rows_limit= ((order == NULL || skip_sort_order) &&
                              !table_group &&
                              !select_lex->with_sum_func) ? select_limit
                                                          : HA_POS_ERROR;

  if (!(tab->tmp_table_param= new TMP_TABLE_PARAM(tmp_table_param)))
    DBUG_RETURN(true);
  if (tmp_table_keep_current_rowid)
    add_fields_for_current_rowid(tab, table_fields);
  tab->tmp_table_param->skip_create_table= true;
  TABLE* table= create_tmp_table(thd, tab->tmp_table_param, *table_fields,
                                 table_group, distinct,
                                 save_sum_fields, select_options, table_rows_limit, 
                                 &empty_clex_str, true, keep_row_order);
  if (!table)
    DBUG_RETURN(true);
  tmp_table_param.using_outer_summary_function=
    tab->tmp_table_param->using_outer_summary_function;
  tab->join= this;
  DBUG_ASSERT(tab > tab->join->join_tab || !top_join_tab_count || !tables_list);
  if (tab > join_tab)
    (tab - 1)->next_select= sub_select_postjoin_aggr;
  if (!(tab->aggr= new (thd->mem_root) AGGR_OP(tab)))
    goto err;
  tab->table= table;
  table->reginfo.join_tab= tab;

  /* if group or order on first table, sort first */
  if ((group_list && simple_group) ||
      (implicit_grouping && select_lex->have_window_funcs()))
  {
    DBUG_PRINT("info",("Sorting for group"));
    THD_STAGE_INFO(thd, stage_sorting_for_group);

    if (ordered_index_usage != ordered_index_group_by &&
        !only_const_tables() &&
        (join_tab + const_tables)->type != JT_CONST && // Don't sort 1 row
        !implicit_grouping &&
        add_sorting_to_table(join_tab + const_tables, group_list))
      goto err;

    if (alloc_group_fields(this, group_list))
      goto err;
    if (make_sum_func_list(all_fields, fields_list, true))
      goto err;
    if (prepare_sum_aggregators(sum_funcs,
                                !(tables_list && 
                                  join_tab->is_using_agg_loose_index_scan())))
      goto err;
    if (setup_sum_funcs(thd, sum_funcs))
      goto err;
    group_list= NULL;
  }
  else
  {
    if (make_sum_func_list(all_fields, fields_list, false))
      goto err;
    if (prepare_sum_aggregators(sum_funcs,
                                !join_tab->is_using_agg_loose_index_scan()))
      goto err;
    if (setup_sum_funcs(thd, sum_funcs))
      goto err;

    if (!group_list && !table->distinct && order && simple_order &&
        tab == join_tab + const_tables)
    {
      DBUG_PRINT("info",("Sorting for order"));
      THD_STAGE_INFO(thd, stage_sorting_for_order);

      if (ordered_index_usage != ordered_index_order_by &&
          !only_const_tables() &&
          add_sorting_to_table(join_tab + const_tables, order))
        goto err;
      order= NULL;
    }
  }

  DBUG_RETURN(false);

err:
  if (table != NULL)
    free_tmp_table(thd, table);
  DBUG_RETURN(true);
}


void
JOIN::optimize_distinct()
{
  for (JOIN_TAB *last_join_tab= join_tab + top_join_tab_count - 1; ;)
  {
    if (select_lex->select_list_tables & last_join_tab->table->map ||
        last_join_tab->use_join_cache)
      break;
    last_join_tab->shortcut_for_distinct= true;
    if (last_join_tab == join_tab)
      break;
    --last_join_tab;
  }

  /* Optimize "select distinct b from t1 order by key_part_1 limit #" */
  if (order && skip_sort_order)
  {
    /* Should already have been optimized away */
    DBUG_ASSERT(ordered_index_usage == ordered_index_order_by);
    if (ordered_index_usage == ordered_index_order_by)
    {
      order= NULL;
    }
  }
}


/**
  @brief Add Filesort object to the given table to sort if with filesort

  @param tab   the JOIN_TAB object to attach created Filesort object to
  @param order List of expressions to sort the table by

  @note This function moves tab->select, if any, to filesort->select

  @return false on success, true on OOM
*/

bool
JOIN::add_sorting_to_table(JOIN_TAB *tab, ORDER *order)
{
  tab->filesort= 
    new (thd->mem_root) Filesort(order, HA_POS_ERROR, tab->keep_current_rowid,
                                 tab->select);
  if (!tab->filesort)
    return true;
  /*
    Select was moved to filesort->select to force join_init_read_record to use
    sorted result instead of reading table through select.
  */
  if (tab->select)
  {
    tab->select= NULL;
    tab->set_select_cond(NULL, __LINE__);
  }
  tab->read_first_record= join_init_read_record;
  return false;
}




/**
  Setup expression caches for subqueries that need them

  @details
  The function wraps correlated subquery expressions that return one value
  into objects of the class Item_cache_wrapper setting up an expression
  cache for each of them. The result values of the subqueries are to be
  cached together with the corresponding sets of the parameters - outer
  references of the subqueries.

  @retval FALSE OK
  @retval TRUE  Error
*/

bool JOIN::setup_subquery_caches()
{
  DBUG_ENTER("JOIN::setup_subquery_caches");

  /*
    We have to check all this condition together because items created in
    one of this clauses can be moved to another one by optimizer
  */
  if (select_lex->expr_cache_may_be_used[IN_WHERE] ||
      select_lex->expr_cache_may_be_used[IN_HAVING] ||
      select_lex->expr_cache_may_be_used[IN_ON] ||
      select_lex->expr_cache_may_be_used[NO_MATTER])
  {
    JOIN_TAB *tab;
    if (conds &&
        !(conds= conds->transform(thd, &Item::expr_cache_insert_transformer,
                                  NULL)))
      DBUG_RETURN(TRUE);
    for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
         tab; tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
    {
      if (tab->select_cond &&
          !(tab->select_cond=
            tab->select_cond->transform(thd,
                                        &Item::expr_cache_insert_transformer,
                                        NULL)))
	DBUG_RETURN(TRUE);
      if (tab->cache_select && tab->cache_select->cond)
        if (!(tab->cache_select->cond=
              tab->cache_select->
              cond->transform(thd, &Item::expr_cache_insert_transformer,
                              NULL)))
          DBUG_RETURN(TRUE);
    }

    if (having &&
        !(having= having->transform(thd,
                                    &Item::expr_cache_insert_transformer,
                                    NULL)))
      DBUG_RETURN(TRUE);

    if (tmp_having)
    {
      DBUG_ASSERT(having == NULL);
      if (!(tmp_having=
            tmp_having->transform(thd,
                                  &Item::expr_cache_insert_transformer,
                                  NULL)))
	DBUG_RETURN(TRUE);
    }
  }
  if (select_lex->expr_cache_may_be_used[SELECT_LIST] ||
      select_lex->expr_cache_may_be_used[IN_GROUP_BY] ||
      select_lex->expr_cache_may_be_used[NO_MATTER])
  {
    List_iterator<Item> li(all_fields);
    Item *item;
    while ((item= li++))
    {
      Item *new_item;
      if (!(new_item=
            item->transform(thd, &Item::expr_cache_insert_transformer,
                            NULL)))
        DBUG_RETURN(TRUE);
      if (new_item != item)
      {
        thd->change_item_tree(li.ref(), new_item);
      }
    }
    for (ORDER *tmp_group= group_list; tmp_group ; tmp_group= tmp_group->next)
    {
      if (!(*tmp_group->item=
            (*tmp_group->item)->transform(thd,
                                          &Item::expr_cache_insert_transformer,
                                          NULL)))
        DBUG_RETURN(TRUE);
    }
  }
  if (select_lex->expr_cache_may_be_used[NO_MATTER])
  {
    for (ORDER *ord= order; ord; ord= ord->next)
    {
      if (!(*ord->item=
            (*ord->item)->transform(thd,
                                    &Item::expr_cache_insert_transformer,
                                    NULL)))
	DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  Shrink join buffers used for preceding tables to reduce the occupied space

  SYNOPSIS
    shrink_join_buffers()
      jt           table up to which the buffers are to be shrunk
      curr_space   the size of the space used by the buffers for tables 1..jt
      needed_space the size of the space that has to be used by these buffers

  DESCRIPTION
    The function makes an attempt to shrink all join buffers used for the
    tables starting from the first up to jt to reduce the total size of the
    space occupied by the buffers used for tables 1,...,jt  from curr_space
    to needed_space.
    The function assumes that the buffer for the table jt has not been
    allocated yet.

  RETURN
    FALSE     if all buffer have been successfully shrunk
    TRUE      otherwise
*/
  
bool JOIN::shrink_join_buffers(JOIN_TAB *jt, 
                               ulonglong curr_space,
                               ulonglong needed_space)
{
  JOIN_TAB *tab;
  JOIN_CACHE *cache;
  for (tab= first_linear_tab(this, WITHOUT_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab != jt;
       tab= next_linear_tab(this, tab, WITHOUT_BUSH_ROOTS))
  {
    cache= tab->cache;
    if (cache)
    { 
      size_t buff_size;
      if (needed_space < cache->get_min_join_buffer_size())
        return TRUE;
      if (cache->shrink_join_buffer_in_ratio(curr_space, needed_space))
      { 
        revise_cache_usage(tab);
        return TRUE;
      }
      buff_size= cache->get_join_buffer_size();
      curr_space-= buff_size;
      needed_space-= buff_size;
    }
  }

  cache= jt->cache;
  DBUG_ASSERT(cache);
  if (needed_space < cache->get_min_join_buffer_size())
    return TRUE;
  cache->set_join_buffer_size((size_t)needed_space);
  
  return FALSE;
}


int
JOIN::reinit()
{
  DBUG_ENTER("JOIN::reinit");

  unit->offset_limit_cnt= (ha_rows)(select_lex->offset_limit ?
                                    select_lex->offset_limit->val_uint() : 0);

  first_record= false;
  group_sent= false;
  cleaned= false;

  if (aggr_tables)
  {
    JOIN_TAB *curr_tab= join_tab + exec_join_tab_cnt();
    JOIN_TAB *end_tab= curr_tab + aggr_tables;
    for ( ; curr_tab < end_tab; curr_tab++)
    {
      TABLE *tmp_table= curr_tab->table;
      if (!tmp_table->is_created())
        continue;
      tmp_table->file->extra(HA_EXTRA_RESET_STATE);
      tmp_table->file->ha_delete_all_rows();
    }
  }
  clear_sj_tmp_tables(this);
  if (current_ref_ptrs != items0)
  {
    set_items_ref_array(items0);
    set_group_rpa= false;
  }

  /* need to reset ref access state (see join_read_key) */
  if (join_tab)
  {
    JOIN_TAB *tab;
    for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITH_CONST_TABLES); tab;
         tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
    {
      tab->ref.key_err= TRUE;
    }
  }

  /* Reset of sum functions */
  if (sum_funcs)
  {
    Item_sum *func, **func_ptr= sum_funcs;
    while ((func= *(func_ptr++)))
      func->clear();
  }

  if (no_rows_in_result_called)
  {
    /* Reset effect of possible no_rows_in_result() */
    List_iterator_fast<Item> it(fields_list);
    Item *item;
    no_rows_in_result_called= 0;
    while ((item= it++))
      item->restore_to_before_no_rows_in_result();
  }

  if (!(select_options & SELECT_DESCRIBE))
    if (init_ftfuncs(thd, select_lex, MY_TEST(order)))
      DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/**
  Prepare join result.

  @details Prepare join result prior to join execution or describing.
  Instantiate derived tables and get schema tables result if necessary.

  @return
    TRUE  An error during derived or schema tables instantiation.
    FALSE Ok
*/

bool JOIN::prepare_result(List<Item> **columns_list)
{
  DBUG_ENTER("JOIN::prepare_result");

  error= 0;
  /* Create result tables for materialized views. */
  if (!zero_result_cause &&
      select_lex->handle_derived(thd->lex, DT_CREATE))
    goto err;

  if (result->prepare2(this))
    goto err;

  if ((select_lex->options & OPTION_SCHEMA_TABLE) &&
      get_schema_tables_result(this, PROCESSED_BY_JOIN_EXEC))
    goto err;

  DBUG_RETURN(FALSE);

err:
  error= 1;
  DBUG_RETURN(TRUE);
}


/**
   @retval
   0 ok
   1 error
*/


bool JOIN::save_explain_data(Explain_query *output, bool can_overwrite,
                             bool need_tmp_table, bool need_order, 
                             bool distinct)
{
  /*
    If there is SELECT in this statement with the same number it must be the
    same SELECT
  */
  DBUG_SLOW_ASSERT(select_lex->select_number == UINT_MAX ||
              select_lex->select_number == INT_MAX ||
              !output ||
              !output->get_select(select_lex->select_number) ||
              output->get_select(select_lex->select_number)->select_lex ==
                select_lex);

  if (select_lex->select_number != UINT_MAX && 
      select_lex->select_number != INT_MAX /* this is not a UNION's "fake select */ && 
      have_query_plan != JOIN::QEP_NOT_PRESENT_YET && 
      have_query_plan != JOIN::QEP_DELETED &&  // this happens when there was 
                                               // no QEP ever, but then
                                               //cleanup() is called multiple times
      output && // for "SET" command in SPs.
      (can_overwrite? true: !output->get_select(select_lex->select_number)))
  {
    const char *message= NULL;
    if (!table_count || !tables_list || zero_result_cause)
    {
      /* It's a degenerate join */
      message= zero_result_cause ? zero_result_cause : "No tables used";
    }
    return save_explain_data_intern(thd->lex->explain, need_tmp_table, need_order,
                                    distinct, message);
  }
  
  /*
    Can have join_tab==NULL for degenerate cases (e.g. SELECT .. UNION ... SELECT LIMIT 0)
  */
  if (select_lex == select_lex->master_unit()->fake_select_lex && join_tab)
  {
    /* 
      This is fake_select_lex. It has no query plan, but we need to set up a
      tracker for ANALYZE 
    */
    uint nr= select_lex->master_unit()->first_select()->select_number;
    Explain_union *eu= output->get_union(nr);
    explain= &eu->fake_select_lex_explain;
    join_tab[0].tracker= eu->get_fake_select_lex_tracker();
    for (uint i=0 ; i < exec_join_tab_cnt() + aggr_tables; i++)
    {
      if (join_tab[i].filesort)
      {
        if (!(join_tab[i].filesort->tracker=
              new Filesort_tracker(thd->lex->analyze_stmt)))
          return 1;
      }
    }
  }
  return 0;
}


void JOIN::exec()
{
  DBUG_EXECUTE_IF("show_explain_probe_join_exec_start", 
                  if (dbug_user_var_equals_int(thd, 
                                               "show_explain_probe_select_id", 
                                               select_lex->select_number))
                        dbug_serve_apcs(thd, 1);
                 );
  ANALYZE_START_TRACKING(&explain->time_tracker);
  exec_inner();
  ANALYZE_STOP_TRACKING(&explain->time_tracker);

  DBUG_EXECUTE_IF("show_explain_probe_join_exec_end", 
                  if (dbug_user_var_equals_int(thd, 
                                               "show_explain_probe_select_id", 
                                               select_lex->select_number))
                        dbug_serve_apcs(thd, 1);
                 );
}


void JOIN::exec_inner()
{
  List<Item> *columns_list= &fields_list;
  DBUG_ENTER("JOIN::exec_inner");
  DBUG_ASSERT(optimization_state == JOIN::OPTIMIZATION_DONE);

  THD_STAGE_INFO(thd, stage_executing);

  /*
    Enable LIMIT ROWS EXAMINED during query execution if:
    (1) This JOIN is the outermost query (not a subquery or derived table)
        This ensures that the limit is enabled when actual execution begins, and
        not if a subquery is evaluated during optimization of the outer query.
    (2) This JOIN is not the result of a UNION. In this case do not apply the
        limit in order to produce the partial query result stored in the
        UNION temp table.
  */
  if (!select_lex->outer_select() &&                            // (1)
      select_lex != select_lex->master_unit()->fake_select_lex) // (2)
    thd->lex->set_limit_rows_examined();

  if (procedure)
  {
    procedure_fields_list= fields_list;
    if (procedure->change_columns(thd, procedure_fields_list) ||
	result->prepare(procedure_fields_list, unit))
    {
      thd->set_examined_row_count(0);
      thd->limit_found_rows= 0;
      DBUG_VOID_RETURN;
    }
    columns_list= &procedure_fields_list;
  }
  if (result->prepare2(this))
    DBUG_VOID_RETURN;

  if (!tables_list && (table_count || !select_lex->with_sum_func) &&
      !select_lex->have_window_funcs())
  {                                           // Only test of functions
    if (select_options & SELECT_DESCRIBE)
      select_describe(this, FALSE, FALSE, FALSE,
		      (zero_result_cause?zero_result_cause:"No tables used"));

    else
    {
      if (result->send_result_set_metadata(*columns_list,
                                           Protocol::SEND_NUM_ROWS |
                                           Protocol::SEND_EOF))
      {
        DBUG_VOID_RETURN;
      }

      /*
        We have to test for 'conds' here as the WHERE may not be constant
        even if we don't have any tables for prepared statements or if
        conds uses something like 'rand()'.
        If the HAVING clause is either impossible or always true, then
        JOIN::having is set to NULL by optimize_cond.
        In this case JOIN::exec must check for JOIN::having_value, in the
        same way it checks for JOIN::cond_value.
      */
      DBUG_ASSERT(error == 0);
      if (cond_value != Item::COND_FALSE &&
          having_value != Item::COND_FALSE &&
          (!conds || conds->val_int()) &&
          (!having || having->val_int()))
      {
	if (do_send_rows &&
            (procedure ? (procedure->send_row(procedure_fields_list) ||
             procedure->end_of_records()) : result->send_data(fields_list)> 0))
	  error= 1;
	else
	  send_records= ((select_options & OPTION_FOUND_ROWS) ? 1 :
                         thd->get_sent_row_count());
      }
      else
        send_records= 0;
      if (likely(!error))
      {
        join_free();                      // Unlock all cursors
        error= (int) result->send_eof();
      }
    }
    /* Single select (without union) always returns 0 or 1 row */
    thd->limit_found_rows= send_records;
    thd->set_examined_row_count(0);
    DBUG_VOID_RETURN;
  }

  /*
    Evaluate expensive constant conditions that were not evaluated during
    optimization. Do not evaluate them for EXPLAIN statements as these
    condtions may be arbitrarily costly, and because the optimize phase
    might not have produced a complete executable plan for EXPLAINs.
  */
  if (!zero_result_cause &&
      exec_const_cond && !(select_options & SELECT_DESCRIBE) &&
      !exec_const_cond->val_int())
    zero_result_cause= "Impossible WHERE noticed after reading const tables";

  /* 
    We've called exec_const_cond->val_int(). This may have caused an error.
  */
  if (unlikely(thd->is_error()))
  {
    error= thd->is_error();
    DBUG_VOID_RETURN;
  }

  if (zero_result_cause)
  {
    if (select_lex->have_window_funcs() && send_row_on_empty_set())
    {
      /*
        The query produces just one row but it has window functions.

        The only way to compute the value of window function(s) is to
        run the entire window function computation step (there is no shortcut).
      */
      const_tables= table_count;
      first_select= sub_select_postjoin_aggr;
    }
    else
    {
      (void) return_zero_rows(this, result, select_lex->leaf_tables,
                              *columns_list,
			      send_row_on_empty_set(),
			      select_options,
			      zero_result_cause,
			      having ? having : tmp_having, all_fields);
      DBUG_VOID_RETURN;
    }
  }
  
  /*
    Evaluate all constant expressions with subqueries in the
    ORDER/GROUP clauses to make sure that all subqueries return a
    single row. The evaluation itself will trigger an error if that is
    not the case.
  */
  if (exec_const_order_group_cond.elements &&
      !(select_options & SELECT_DESCRIBE))
  {
    List_iterator_fast<Item> const_item_it(exec_const_order_group_cond);
    Item *cur_const_item;
    while ((cur_const_item= const_item_it++))
    {
      cur_const_item->val_str(); // This caches val_str() to Item::str_value
      if (unlikely(thd->is_error()))
      {
        error= thd->is_error();
        DBUG_VOID_RETURN;
      }
    }
  }

  if ((this->select_lex->options & OPTION_SCHEMA_TABLE) &&
      get_schema_tables_result(this, PROCESSED_BY_JOIN_EXEC))
    DBUG_VOID_RETURN;

  if (select_options & SELECT_DESCRIBE)
  {
    select_describe(this, need_tmp,
		    order != 0 && !skip_sort_order,
		    select_distinct,
                    !table_count ? "No tables used" : NullS);
    DBUG_VOID_RETURN;
  }
  else
  {
    /* it's a const select, materialize it. */
    select_lex->mark_const_derived(zero_result_cause);
  }

  /*
    Initialize examined rows here because the values from all join parts
    must be accumulated in examined_row_count. Hence every join
    iteration must count from zero.
  */
  join_examined_rows= 0;

  /* XXX: When can we have here thd->is_error() not zero? */
  if (unlikely(thd->is_error()))
  {
    error= thd->is_error();
    DBUG_VOID_RETURN;
  }

  THD_STAGE_INFO(thd, stage_sending_data);
  DBUG_PRINT("info", ("%s", thd->proc_info));
  result->send_result_set_metadata(
                 procedure ? procedure_fields_list : *fields,
                 Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF);

  error= result->view_structure_only() ? false : do_select(this, procedure);
  /* Accumulate the counts from all join iterations of all join parts. */
  thd->inc_examined_row_count(join_examined_rows);
  DBUG_PRINT("counts", ("thd->examined_row_count: %lu",
                        (ulong) thd->get_examined_row_count()));

  DBUG_VOID_RETURN;
}


/**
  Clean up join.

  @return
    Return error that hold JOIN.
*/

int
JOIN::destroy()
{
  DBUG_ENTER("JOIN::destroy");
  select_lex->join= 0;

  cond_equal= 0;
  having_equal= 0;

  cleanup(1);

  if (join_tab)
  {
    for (JOIN_TAB *tab= first_linear_tab(this, WITH_BUSH_ROOTS,
                                         WITH_CONST_TABLES);
         tab; tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
    {
      if (tab->aggr)
      {
        free_tmp_table(thd, tab->table);
        delete tab->tmp_table_param;
        tab->tmp_table_param= NULL;
        tab->aggr= NULL;
      }
      tab->table= NULL;
    }
  }

  /* Cleanup items referencing temporary table columns */
  cleanup_item_list(tmp_all_fields1);
  cleanup_item_list(tmp_all_fields3);
  destroy_sj_tmp_tables(this);
  delete_dynamic(&keyuse);
  if (save_qep)
    delete(save_qep);
  if (ext_keyuses_for_splitting)
    delete(ext_keyuses_for_splitting);
  delete procedure;
  DBUG_RETURN(error);
}


void JOIN::cleanup_item_list(List<Item> &items) const
{
  DBUG_ENTER("JOIN::cleanup_item_list");
  if (!items.is_empty())
  {
    List_iterator_fast<Item> it(items);
    Item *item;
    while ((item= it++))
      item->cleanup();
  }
  DBUG_VOID_RETURN;
}


/**
  An entry point to single-unit select (a select without UNION).

  @param thd                  thread handler
  @param rref_pointer_array   a reference to ref_pointer_array of
                              the top-level select_lex for this query
  @param tables               list of all tables used in this query.
                              The tables have been pre-opened.
  @param wild_num             number of wildcards used in the top level 
                              select of this query.
                              For example statement
                              SELECT *, t1.*, catalog.t2.* FROM t0, t1, t2;
                              has 3 wildcards.
  @param fields               list of items in SELECT list of the top-level
                              select
                              e.g. SELECT a, b, c FROM t1 will have Item_field
                              for a, b and c in this list.
  @param conds                top level item of an expression representing
                              WHERE clause of the top level select
  @param og_num               total number of ORDER BY and GROUP BY clauses
                              arguments
  @param order                linked list of ORDER BY agruments
  @param group                linked list of GROUP BY arguments
  @param having               top level item of HAVING expression
  @param proc_param           list of PROCEDUREs
  @param select_options       select options (BIG_RESULT, etc)
  @param result               an instance of result set handling class.
                              This object is responsible for send result
                              set rows to the client or inserting them
                              into a table.
  @param select_lex           the only SELECT_LEX of this query
  @param unit                 top-level UNIT of this query
                              UNIT is an artificial object created by the
                              parser for every SELECT clause.
                              e.g.
                              SELECT * FROM t1 WHERE a1 IN (SELECT * FROM t2)
                              has 2 unions.

  @retval
    FALSE  success
  @retval
    TRUE   an error
*/

bool
mysql_select(THD *thd,
	     TABLE_LIST *tables, uint wild_num, List<Item> &fields,
	     COND *conds, uint og_num,  ORDER *order, ORDER *group,
	     Item *having, ORDER *proc_param, ulonglong select_options,
	     select_result *result, SELECT_LEX_UNIT *unit,
	     SELECT_LEX *select_lex)
{
  int err= 0;
  bool free_join= 1;
  DBUG_ENTER("mysql_select");

  if (!fields.is_empty())
    select_lex->context.resolve_in_select_list= true;
  JOIN *join;
  if (select_lex->join != 0)
  {
    join= select_lex->join;
    /*
      is it single SELECT in derived table, called in derived table
      creation
    */
    if (select_lex->linkage != DERIVED_TABLE_TYPE ||
	(select_options & SELECT_DESCRIBE))
    {
      if (select_lex->linkage != GLOBAL_OPTIONS_TYPE)
      {
        /*
          Original join tabs might be overwritten at first
          subselect execution. So we need to restore them.
        */
        Item_subselect *subselect= select_lex->master_unit()->item;
        if (subselect && subselect->is_uncacheable() && join->reinit())
          DBUG_RETURN(TRUE);
      }
      else
      {
        if ((err= join->prepare( tables, wild_num,
                                conds, og_num, order, false, group, having,
                                proc_param, select_lex, unit)))
	{
	  goto err;
	}
      }
    }
    free_join= 0;
    join->select_options= select_options;
  }
  else
  {
    if (thd->lex->describe)
      select_options|= SELECT_DESCRIBE;

    /*
      When in EXPLAIN, delay deleting the joins so that they are still
      available when we're producing EXPLAIN EXTENDED warning text.
    */
    if (select_options & SELECT_DESCRIBE)
      free_join= 0;

    if (!(join= new (thd->mem_root) JOIN(thd, fields, select_options, result)))
	DBUG_RETURN(TRUE);
    THD_STAGE_INFO(thd, stage_init);
    thd->lex->used_tables=0;
    if ((err= join->prepare(tables, wild_num,
                            conds, og_num, order, false, group, having, proc_param,
                            select_lex, unit)))
    {
      goto err;
    }
  }

  if ((err= join->optimize()))
  {
    goto err;					// 1
  }

  if (thd->lex->describe & DESCRIBE_EXTENDED)
  {
    join->conds_history= join->conds;
    join->having_history= (join->having?join->having:join->tmp_having);
  }

  if (unlikely(thd->is_error()))
    goto err;

  join->exec();

  if (thd->lex->describe & DESCRIBE_EXTENDED)
  {
    select_lex->where= join->conds_history;
    select_lex->having= join->having_history;
  }

err:
  if (free_join)
  {
    THD_STAGE_INFO(thd, stage_end);
    err|= (int)(select_lex->cleanup());
    DBUG_RETURN(err || thd->is_error());
  }
  DBUG_RETURN(join->error ? join->error: err);
}


/*****************************************************************************
  Create JOIN_TABS, make a guess about the table types,
  Approximate how many records will be used in each table
*****************************************************************************/

static ha_rows get_quick_record_count(THD *thd, SQL_SELECT *select,
				      TABLE *table,
				      const key_map *keys,ha_rows limit)
{
  int error;
  DBUG_ENTER("get_quick_record_count");
  uchar buff[STACK_BUFF_ALLOC];
  if (unlikely(check_stack_overrun(thd, STACK_MIN_SIZE, buff)))
    DBUG_RETURN(0);                           // Fatal error flag is set
  if (select)
  {
    select->head=table;
    table->reginfo.impossible_range=0;
    if (likely((error=
                select->test_quick_select(thd, *(key_map *)keys,
                                          (table_map) 0,
                                          limit, 0, FALSE,
                                          TRUE /* remove_where_parts*/)) ==
               1))
      DBUG_RETURN(select->quick->records);
    if (unlikely(error == -1))
    {
      table->reginfo.impossible_range=1;
      DBUG_RETURN(0);
    }
    DBUG_PRINT("warning",("Couldn't use record count on const keypart"));
  }
  DBUG_RETURN(HA_POS_ERROR);			/* This shouldn't happend */
}

/*
   This structure is used to collect info on potentially sargable
   predicates in order to check whether they become sargable after
   reading const tables.
   We form a bitmap of indexes that can be used for sargable predicates.
   Only such indexes are involved in range analysis.
*/
struct SARGABLE_PARAM
{
  Field *field;              /* field against which to check sargability */
  Item **arg_value;          /* values of potential keys for lookups     */
  uint num_values;           /* number of values in the above array      */
};


/*
  Mark all tables inside a join nest as constant.

  @detail  This is called when there is a local "Impossible WHERE" inside
           a multi-table LEFT JOIN.
*/

void mark_join_nest_as_const(JOIN *join,
                             TABLE_LIST *join_nest,
                             table_map *found_const_table_map,
                             uint *const_count)
{
  List_iterator<TABLE_LIST> it(join_nest->nested_join->join_list);
  TABLE_LIST *tbl;
  while ((tbl= it++))
  {
    if (tbl->nested_join)
    {
      mark_join_nest_as_const(join, tbl, found_const_table_map, const_count);
      continue;
    }
    JOIN_TAB *tab= tbl->table->reginfo.join_tab;

    if (!(join->const_table_map & tab->table->map))
    {
      tab->type= JT_CONST;
      tab->info= ET_IMPOSSIBLE_ON_CONDITION;
      tab->table->const_table= 1;

      join->const_table_map|= tab->table->map;
      *found_const_table_map|= tab->table->map;
      set_position(join,(*const_count)++,tab,(KEYUSE*) 0);
      mark_as_null_row(tab->table);		// All fields are NULL
    }
  }
}


/*
  @brief Get the condition that can be used to do range analysis/partition
    pruning/etc

  @detail
    Figure out which condition we can use:
    - For INNER JOIN, we use the WHERE,
    - "t1 LEFT JOIN t2 ON ..." uses t2's ON expression
    - "t1 LEFT JOIN (...) ON ..." uses the join nest's ON expression.
*/

static Item **get_sargable_cond(JOIN *join, TABLE *table)
{
  Item **retval;
  if (table->pos_in_table_list->on_expr)
  {
    /*
      This is an inner table from a single-table LEFT JOIN, "t1 LEFT JOIN
      t2 ON cond". Use the condition cond.
    */
    retval= &table->pos_in_table_list->on_expr;
  }
  else if (table->pos_in_table_list->embedding &&
           !table->pos_in_table_list->embedding->sj_on_expr)
  {
    /*
      This is the inner side of a multi-table outer join. Use the
      appropriate ON expression.
    */
    retval= &(table->pos_in_table_list->embedding->on_expr);
  }
  else
  {
    /* The table is not inner wrt some LEFT JOIN. Use the WHERE clause */
    retval= &join->conds;
  }
  return retval;
}


/**
  Calculate the best possible join and initialize the join structure.

  @retval
    0	ok
  @retval
    1	Fatal error
*/

static bool
make_join_statistics(JOIN *join, List<TABLE_LIST> &tables_list,
                     DYNAMIC_ARRAY *keyuse_array)
{
  int error= 0;
  TABLE *UNINIT_VAR(table); /* inited in all loops */
  uint i,table_count,const_count,key;
  table_map found_const_table_map, all_table_map;
  key_map const_ref, eq_part;
  bool has_expensive_keyparts;
  TABLE **table_vector;
  JOIN_TAB *stat,*stat_end,*s,**stat_ref, **stat_vector;
  KEYUSE *keyuse,*start_keyuse;
  table_map outer_join=0;
  table_map no_rows_const_tables= 0;
  SARGABLE_PARAM *sargables= 0;
  List_iterator<TABLE_LIST> ti(tables_list);
  TABLE_LIST *tables;
  DBUG_ENTER("make_join_statistics");

  table_count=join->table_count;

  /*
    best_positions is ok to allocate with alloc() as we copy things to it with
    memcpy()
  */

  if (!multi_alloc_root(join->thd->mem_root,
                        &stat, sizeof(JOIN_TAB)*(table_count),
                        &stat_ref, sizeof(JOIN_TAB*)* MAX_TABLES,
                        &stat_vector, sizeof(JOIN_TAB*)* (table_count +1),
                        &table_vector, sizeof(TABLE*)*(table_count*2),
                        &join->positions, sizeof(POSITION)*(table_count + 1),
                        &join->best_positions,
                        sizeof(POSITION)*(table_count + 1),
                        NullS))
    DBUG_RETURN(1);

  /* The following should be optimized to only clear critical things */
  bzero((void*)stat, sizeof(JOIN_TAB)* table_count);
  /* Initialize POSITION objects */
  for (i=0 ; i <= table_count ; i++)
    (void) new ((char*) (join->positions + i)) POSITION;

  join->best_ref= stat_vector;

  stat_end=stat+table_count;
  found_const_table_map= all_table_map=0;
  const_count=0;

  for (s= stat, i= 0; (tables= ti++); s++, i++)
  {
    TABLE_LIST *embedding= tables->embedding;
    stat_vector[i]=s;
    s->keys.init();
    s->const_keys.init();
    s->checked_keys.init();
    s->needed_reg.init();
    table_vector[i]=s->table=table=tables->table;
    s->tab_list= tables;
    table->pos_in_table_list= tables;
    error= tables->fetch_number_of_rows();
    set_statistics_for_table(join->thd, table);
    bitmap_clear_all(&table->cond_set);

#ifdef WITH_PARTITION_STORAGE_ENGINE
    const bool all_partitions_pruned_away= table->all_partitions_pruned_away;
#else
    const bool all_partitions_pruned_away= FALSE;
#endif

    DBUG_EXECUTE_IF("bug11747970_raise_error",
                    { join->thd->set_killed(KILL_QUERY_HARD); });
    if (unlikely(error))
    {
      table->file->print_error(error, MYF(0));
      goto error;
    }
    table->quick_keys.clear_all();
    table->intersect_keys.clear_all();
    table->reginfo.join_tab=s;
    table->reginfo.not_exists_optimize=0;
    bzero((char*) table->const_key_parts, sizeof(key_part_map)*table->s->keys);
    all_table_map|= table->map;
    s->preread_init_done= FALSE;
    s->join=join;

    s->dependent= tables->dep_tables;
    if (tables->schema_table)
      table->file->stats.records= table->used_stat_records= 2;
    table->quick_condition_rows= table->stat_records();

    s->on_expr_ref= &tables->on_expr;
    if (*s->on_expr_ref)
    {
      /* s is the only inner table of an outer join */
      if (!table->is_filled_at_execution() &&
          ((!table->file->stats.records &&
            (table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT)) ||
           all_partitions_pruned_away) && !embedding)
      {						// Empty table
        s->dependent= 0;                        // Ignore LEFT JOIN depend.
        no_rows_const_tables |= table->map;
	set_position(join,const_count++,s,(KEYUSE*) 0);
	continue;
      }
      outer_join|= table->map;
      s->embedding_map= 0;
      for (;embedding; embedding= embedding->embedding)
        s->embedding_map|= embedding->nested_join->nj_map;
      continue;
    }
    if (embedding)
    {
      /* s belongs to a nested join, maybe to several embedded joins */
      s->embedding_map= 0;
      bool inside_an_outer_join= FALSE;
      do
      {
        /* 
          If this is a semi-join nest, skip it, and proceed upwards. Maybe
          we're in some outer join nest
        */
        if (embedding->sj_on_expr)
        {
          embedding= embedding->embedding;
          continue;
        }
        inside_an_outer_join= TRUE;
        NESTED_JOIN *nested_join= embedding->nested_join;
        s->embedding_map|=nested_join->nj_map;
        s->dependent|= embedding->dep_tables;
        embedding= embedding->embedding;
        outer_join|= nested_join->used_tables;
      }
      while (embedding);
      if (inside_an_outer_join)
        continue;
    }
    if (!table->is_filled_at_execution() &&
        (table->s->system ||
         (table->file->stats.records <= 1 &&
          (table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT)) ||
         all_partitions_pruned_away) &&
	!s->dependent &&
        !table->fulltext_searched && !join->no_const_tables)
    {
      set_position(join,const_count++,s,(KEYUSE*) 0);
      no_rows_const_tables |= table->map;
    }
    
    /* SJ-Materialization handling: */
    if (table->pos_in_table_list->jtbm_subselect &&
        table->pos_in_table_list->jtbm_subselect->is_jtbm_const_tab)
    {
      set_position(join,const_count++,s,(KEYUSE*) 0);
      no_rows_const_tables |= table->map;
    }
  }

  stat_vector[i]=0;
  join->outer_join=outer_join;

  if (join->outer_join)
  {
    /* 
       Build transitive closure for relation 'to be dependent on'.
       This will speed up the plan search for many cases with outer joins,
       as well as allow us to catch illegal cross references/
       Warshall's algorithm is used to build the transitive closure.
       As we use bitmaps to represent the relation the complexity
       of the algorithm is O((number of tables)^2).

       The classic form of the Warshall's algorithm would look like: 
       for (i= 0; i < table_count; i++)
       {
         for (j= 0; j < table_count; j++)
         {
           for (k= 0; k < table_count; k++)
           {
             if (bitmap_is_set(stat[j].dependent, i) &&
                 bitmap_is_set(stat[i].dependent, k))
               bitmap_set_bit(stat[j].dependent, k);
           }
         }
       }  
    */
    
    for (s= stat ; s < stat_end ; s++)
    {
      table= s->table;
      for (JOIN_TAB *t= stat ; t < stat_end ; t++)
      {
        if (t->dependent & table->map)
          t->dependent |= table->reginfo.join_tab->dependent;
      }
      if (outer_join & s->table->map)
        s->table->maybe_null= 1;
    }
    /* Catch illegal cross references for outer joins */
    for (i= 0, s= stat ; i < table_count ; i++, s++)
    {
      if (s->dependent & s->table->map)
      {
        join->table_count=0;			// Don't use join->table
        my_message(ER_WRONG_OUTER_JOIN,
                   ER_THD(join->thd, ER_WRONG_OUTER_JOIN), MYF(0));
        goto error;
      }
      s->key_dependent= s->dependent;
    }
  }

  if (join->conds || outer_join)
  {
    if (update_ref_and_keys(join->thd, keyuse_array, stat, join->table_count,
                            join->conds, ~outer_join, join->select_lex, &sargables))
      goto error;
    /*
      Keyparts without prefixes may be useful if this JOIN is a subquery, and
      if the subquery may be executed via the IN-EXISTS strategy.
    */
    bool skip_unprefixed_keyparts=
      !(join->is_in_subquery() &&
        ((Item_in_subselect*)join->unit->item)->test_strategy(SUBS_IN_TO_EXISTS));

    if (keyuse_array->elements &&
        sort_and_filter_keyuse(join->thd, keyuse_array,
                               skip_unprefixed_keyparts))
      goto error;
    DBUG_EXECUTE("opt", print_keyuse_array(keyuse_array););
  }

  join->const_table_map= no_rows_const_tables;
  join->const_tables= const_count;
  eliminate_tables(join);
  join->const_table_map &= ~no_rows_const_tables;
  const_count= join->const_tables;
  found_const_table_map= join->const_table_map;

  /* Read tables with 0 or 1 rows (system tables) */
  for (POSITION *p_pos=join->positions, *p_end=p_pos+const_count;
       p_pos < p_end ;
       p_pos++)
  {
    s= p_pos->table;
    if (! (s->table->map & join->eliminated_tables))
    {
      int tmp;
      s->type=JT_SYSTEM;
      join->const_table_map|=s->table->map;
      if ((tmp=join_read_const_table(join->thd, s, p_pos)))
      {
        if (tmp > 0)
          goto error;		// Fatal error
      }
      else
      {
        found_const_table_map|= s->table->map;
        s->table->pos_in_table_list->optimized_away= TRUE;
      }
    }
  }

  /* loop until no more const tables are found */
  int ref_changed;
  do
  {
    ref_changed = 0;
  more_const_tables_found:

    /*
      We only have to loop from stat_vector + const_count as
      set_position() will move all const_tables first in stat_vector
    */

    for (JOIN_TAB **pos=stat_vector+const_count ; (s= *pos) ; pos++)
    {
      table=s->table;

      if (table->is_filled_at_execution())
        continue;

      /* 
        If equi-join condition by a key is null rejecting and after a
        substitution of a const table the key value happens to be null
        then we can state that there are no matches for this equi-join.
      */  
      if ((keyuse= s->keyuse) && *s->on_expr_ref && !s->embedding_map &&
         !(table->map & join->eliminated_tables))
      {
        /* 
          When performing an outer join operation if there are no matching rows
          for the single row of the outer table all the inner tables are to be
          null complemented and thus considered as constant tables.
          Here we apply this consideration to the case of outer join operations 
          with a single inner table only because the case with nested tables
          would require a more thorough analysis.
          TODO. Apply single row substitution to null complemented inner tables
          for nested outer join operations. 
	*/              
        while (keyuse->table == table)
        {
          if (!keyuse->is_for_hash_join() && 
              !(keyuse->val->used_tables() & ~join->const_table_map) &&
              keyuse->val->is_null() && keyuse->null_rejecting)
          {
            s->type= JT_CONST;
            s->table->const_table= 1;
            mark_as_null_row(table);
            found_const_table_map|= table->map;
	    join->const_table_map|= table->map;
	    set_position(join,const_count++,s,(KEYUSE*) 0);
            goto more_const_tables_found;
           }
	  keyuse++;
        }
      }

      if (s->dependent)				// If dependent on some table
      {
	// All dep. must be constants
	if (s->dependent & ~(found_const_table_map))
	  continue;
	if (table->file->stats.records <= 1L &&
	    (table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT) &&
            !table->pos_in_table_list->embedding &&
	      !((outer_join & table->map) && 
		(*s->on_expr_ref)->is_expensive()))
	{					// system table
	  int tmp= 0;
	  s->type=JT_SYSTEM;
	  join->const_table_map|=table->map;
	  set_position(join,const_count++,s,(KEYUSE*) 0);
	  if ((tmp= join_read_const_table(join->thd, s, join->positions+const_count-1)))
	  {
	    if (tmp > 0)
	      goto error;			// Fatal error
	  }
	  else
	    found_const_table_map|= table->map;
	  continue;
	}
      }
      /* check if table can be read by key or table only uses const refs */
      if ((keyuse=s->keyuse))
      {
	s->type= JT_REF;
	while (keyuse->table == table)
	{
          if (keyuse->is_for_hash_join())
	  {
            keyuse++;
            continue;
          }
	  start_keyuse=keyuse;
	  key=keyuse->key;
	  s->keys.set_bit(key);               // TODO: remove this ?

          const_ref.clear_all();
	  eq_part.clear_all();
          has_expensive_keyparts= false;
	  do
	  {
            if (keyuse->val->type() != Item::NULL_ITEM &&
                !keyuse->optimize &&
                keyuse->keypart != FT_KEYPART)
	    {
	      if (!((~found_const_table_map) & keyuse->used_tables))
              {
		const_ref.set_bit(keyuse->keypart);
                if (keyuse->val->is_expensive())
                  has_expensive_keyparts= true;
              }
	      eq_part.set_bit(keyuse->keypart);
	    }
	    keyuse++;
	  } while (keyuse->table == table && keyuse->key == key);

          TABLE_LIST *embedding= table->pos_in_table_list->embedding;
          /*
            TODO (low priority): currently we ignore the const tables that
            are within a semi-join nest which is within an outer join nest.
            The effect of this is that we don't do const substitution for
            such tables.
          */
          KEY *keyinfo= table->key_info + key;
          uint  key_parts= table->actual_n_key_parts(keyinfo);
          if (eq_part.is_prefix(key_parts) &&
              !table->fulltext_searched && 
              (!embedding || (embedding->sj_on_expr && !embedding->embedding)))
	  {
            key_map base_part, base_const_ref, base_eq_part;
            base_part.set_prefix(keyinfo->user_defined_key_parts); 
            base_const_ref= const_ref;
            base_const_ref.intersect(base_part);
            base_eq_part= eq_part;
            base_eq_part.intersect(base_part);
            if (table->actual_key_flags(keyinfo) & HA_NOSAME)
            {
              
	      if (base_const_ref == base_eq_part &&
                  !has_expensive_keyparts &&
                  !((outer_join & table->map) &&
                    (*s->on_expr_ref)->is_expensive()))
	      {					// Found everything for ref.
	        int tmp;
	        ref_changed = 1;
	        s->type= JT_CONST;
	        join->const_table_map|=table->map;
	        set_position(join,const_count++,s,start_keyuse);
                /* create_ref_for_key will set s->table->const_table */
	        if (create_ref_for_key(join, s, start_keyuse, FALSE,
				       found_const_table_map))
                  goto error;
	        if ((tmp=join_read_const_table(join->thd, s,
                                               join->positions+const_count-1)))
	        {
		  if (tmp > 0)
		    goto error;			// Fatal error
	        }
	        else
		  found_const_table_map|= table->map;
	        break;
	      }
	    }
            else if (base_const_ref == base_eq_part)
              s->const_keys.set_bit(key);
          }
	}
      }
    }
  } while (ref_changed);
 
  join->sort_by_table= get_sort_by_table(join->order, join->group_list,
                                         join->select_lex->leaf_tables,
                                         join->const_table_map);
  /* 
    Update info on indexes that can be used for search lookups as
    reading const tables may has added new sargable predicates. 
  */
  if (const_count && sargables)
  {
    for( ; sargables->field ; sargables++)
    {
      Field *field= sargables->field;
      JOIN_TAB *join_tab= field->table->reginfo.join_tab;
      key_map possible_keys= field->key_start;
      possible_keys.intersect(field->table->keys_in_use_for_query);
      bool is_const= 1;
      for (uint j=0; j < sargables->num_values; j++)
        is_const&= sargables->arg_value[j]->const_item();
      if (is_const)
        join_tab[0].const_keys.merge(possible_keys);
    }
  }

  join->impossible_where= false;
  if (join->conds && const_count)
  {
    Item* &conds= join->conds;
    COND_EQUAL *orig_cond_equal = join->cond_equal;

    conds->update_used_tables();
    conds= conds->remove_eq_conds(join->thd, &join->cond_value, true);
    if (conds && conds->type() == Item::COND_ITEM &&
        ((Item_cond*) conds)->functype() == Item_func::COND_AND_FUNC)
      join->cond_equal= &((Item_cond_and*) conds)->m_cond_equal;
    join->select_lex->where= conds;
    if (join->cond_value == Item::COND_FALSE)
    {
      join->impossible_where= true;
      conds= new (join->thd->mem_root) Item_int(join->thd, (longlong) 0, 1);
    }

    join->cond_equal= NULL;
    if (conds) 
    { 
      if (conds->type() == Item::COND_ITEM && 
	  ((Item_cond*) conds)->functype() == Item_func::COND_AND_FUNC)
        join->cond_equal= (&((Item_cond_and *) conds)->m_cond_equal);
      else if (conds->type() == Item::FUNC_ITEM &&
	       ((Item_func*) conds)->functype() == Item_func::MULT_EQUAL_FUNC)
      {
        if (!join->cond_equal)
          join->cond_equal= new COND_EQUAL;
        join->cond_equal->current_level.empty();
        join->cond_equal->current_level.push_back((Item_equal*) conds,
                                                  join->thd->mem_root);
      }
    }

    if (orig_cond_equal != join->cond_equal)
    {
      /*
        If join->cond_equal has changed all references to it from COND_EQUAL
        objects associated with ON expressions must be updated.
      */
      for (JOIN_TAB **pos=stat_vector+const_count ; (s= *pos) ; pos++) 
      {
        if (*s->on_expr_ref && s->cond_equal &&
	    s->cond_equal->upper_levels == orig_cond_equal)
          s->cond_equal->upper_levels= join->cond_equal;
      }
    }
  }

  /* Calc how many (possible) matched records in each table */

  for (s=stat ; s < stat_end ; s++)
  {
    s->startup_cost= 0;
    if (s->type == JT_SYSTEM || s->type == JT_CONST)
    {
      /* Only one matching row */
      s->found_records= s->records= 1;
      s->read_time=1.0; 
      s->worst_seeks=1.0;
      continue;
    }
    /* Approximate found rows and time to read them */
    if (s->table->is_filled_at_execution())
    {
      get_delayed_table_estimates(s->table, &s->records, &s->read_time,
                                  &s->startup_cost);
      s->found_records= s->records;
      table->quick_condition_rows=s->records;
    }
    else
    {
       s->scan_time();
    }

    if (s->table->is_splittable())
      s->add_keyuses_for_splitting();

    /*
      Set a max range of how many seeks we can expect when using keys
      This is can't be to high as otherwise we are likely to use
      table scan.
    */
    s->worst_seeks= MY_MIN((double) s->found_records / 10,
			(double) s->read_time*3);
    if (s->worst_seeks < 2.0)			// Fix for small tables
      s->worst_seeks=2.0;

    /*
      Add to stat->const_keys those indexes for which all group fields or
      all select distinct fields participate in one index.
    */
    add_group_and_distinct_keys(join, s);

    s->table->cond_selectivity= 1.0;
    
    /*
      Perform range analysis if there are keys it could use (1). 
      Don't do range analysis for materialized subqueries (2).
      Don't do range analysis for materialized derived tables (3)
    */
    if ((!s->const_keys.is_clear_all() ||
	 !bitmap_is_clear_all(&s->table->cond_set)) &&              // (1)
        !s->table->is_filled_at_execution() &&                      // (2)
        !(s->table->pos_in_table_list->derived &&                   // (3)
          s->table->pos_in_table_list->is_materialized_derived()))  // (3)
    {
      bool impossible_range= FALSE;
      ha_rows records= HA_POS_ERROR;
      SQL_SELECT *select= 0;
      Item **sargable_cond= NULL;
      if (!s->const_keys.is_clear_all())
      {
        sargable_cond= get_sargable_cond(join, s->table);

        select= make_select(s->table, found_const_table_map,
			    found_const_table_map,
                            *sargable_cond,
                            (SORT_INFO*) 0,
			    1, &error);
        if (!select)
          goto error;
        records= get_quick_record_count(join->thd, select, s->table,
				        &s->const_keys, join->row_limit);

        /*
          Range analyzer might have modified the condition. Put it the new
          condition to where we got it from.
        */
        *sargable_cond= select->cond;

        s->quick=select->quick;
        s->needed_reg=select->needed_reg;
        select->quick=0;
        impossible_range= records == 0 && s->table->reginfo.impossible_range;
      }
      if (!impossible_range)
      {
        if (!sargable_cond)
          sargable_cond= get_sargable_cond(join, s->table);
        if (join->thd->variables.optimizer_use_condition_selectivity > 1)
          calculate_cond_selectivity_for_table(join->thd, s->table, 
                                               sargable_cond);
        if (s->table->reginfo.impossible_range)
	{
          impossible_range= TRUE;
          records= 0;
        }
      }
      if (impossible_range)
      {
        /*
          Impossible WHERE or ON expression
          In case of ON, we mark that the we match one empty NULL row.
          In case of WHERE, don't set found_const_table_map to get the
          caller to abort with a zero row result.
        */
        TABLE_LIST *emb= s->table->pos_in_table_list->embedding;
        if (emb && !emb->sj_on_expr)
        {
          /* Mark all tables in a multi-table join nest as const */
          mark_join_nest_as_const(join, emb, &found_const_table_map,
                                &const_count);
        }
        else
        {
          join->const_table_map|= s->table->map;
          set_position(join,const_count++,s,(KEYUSE*) 0);
          s->type= JT_CONST;
          s->table->const_table= 1;
          if (*s->on_expr_ref)
          {
            /* Generate empty row */
            s->info= ET_IMPOSSIBLE_ON_CONDITION;
            found_const_table_map|= s->table->map;
            mark_as_null_row(s->table);		// All fields are NULL
          }
        }
      }
      if (records != HA_POS_ERROR)
      {
	s->found_records=records;
	s->read_time= s->quick ? s->quick->read_time : 0.0;
      }
      if (select)
        delete select;
    }

  }

  if (pull_out_semijoin_tables(join))
    DBUG_RETURN(TRUE);

  join->join_tab=stat;
  join->top_join_tab_count= table_count;
  join->map2table=stat_ref;
  join->table= table_vector;
  join->const_tables=const_count;
  join->found_const_table_map=found_const_table_map;

  if (join->const_tables != join->table_count)
    optimize_keyuse(join, keyuse_array);
   
  DBUG_ASSERT(!join->conds || !join->cond_equal ||
              !join->cond_equal->current_level.elements ||
              (join->conds->type() == Item::COND_ITEM &&
	       ((Item_cond*) (join->conds))->functype() ==
               Item_func::COND_AND_FUNC && 
               join->cond_equal ==
	       &((Item_cond_and *) (join->conds))->m_cond_equal) ||
              (join->conds->type() == Item::FUNC_ITEM &&
	       ((Item_func*) (join->conds))->functype() ==
               Item_func::MULT_EQUAL_FUNC &&
	       join->cond_equal->current_level.elements == 1 &&
               join->cond_equal->current_level.head() == join->conds));

  if (optimize_semijoin_nests(join, all_table_map))
    DBUG_RETURN(TRUE); /* purecov: inspected */

  {
    double records= 1;
    SELECT_LEX_UNIT *unit= join->select_lex->master_unit();

    /* Find an optimal join order of the non-constant tables. */
    if (join->const_tables != join->table_count)
    {
      if (choose_plan(join, all_table_map & ~join->const_table_map))
        goto error;

#ifdef HAVE_valgrind
      // JOIN::positions holds the current query plan. We've already
      // made the plan choice, so we should only use JOIN::best_positions
      for (uint k=join->const_tables; k < join->table_count; k++)
        MEM_UNDEFINED(&join->positions[k], sizeof(join->positions[k]));
#endif
    }
    else
    {
      memcpy((uchar*) join->best_positions,(uchar*) join->positions,
	     sizeof(POSITION)*join->const_tables);
      join->join_record_count= 1.0;
      join->best_read=1.0;
    }
  
    if (!(join->select_options & SELECT_DESCRIBE) &&
        unit->derived && unit->derived->is_materialized_derived())
    {
      /*
        Calculate estimated number of rows for materialized derived
        table/view.
      */
      for (i= 0; i < join->table_count ; i++)
        if (double rr= join->best_positions[i].records_read)
          records= COST_MULT(records, rr);
      ha_rows rows= records > (double) HA_ROWS_MAX ? HA_ROWS_MAX : (ha_rows) records;
      set_if_smaller(rows, unit->select_limit_cnt);
      join->select_lex->increase_derived_records(rows);
    }
  }

  if (join->choose_subquery_plan(all_table_map & ~join->const_table_map))
    goto error;

  DEBUG_SYNC(join->thd, "inside_make_join_statistics");

  DBUG_RETURN(0);

error:
  /*
    Need to clean up join_tab from TABLEs in case of error.
    They won't get cleaned up by JOIN::cleanup() because JOIN::join_tab
    may not be assigned yet by this function (which is building join_tab).
    Dangling TABLE::reginfo.join_tab may cause part_of_refkey to choke. 
  */
  {    
    TABLE_LIST *tmp_table;
    List_iterator<TABLE_LIST> ti2(tables_list);
    while ((tmp_table= ti2++))
      tmp_table->table->reginfo.join_tab= NULL;
  }
  DBUG_RETURN (1);
}


/*****************************************************************************
  Check with keys are used and with tables references with tables
  Updates in stat:
	  keys	     Bitmap of all used keys
	  const_keys Bitmap of all keys with may be used with quick_select
	  keyuse     Pointer to possible keys
*****************************************************************************/


/**
  Merge new key definitions to old ones, remove those not used in both.

  This is called for OR between different levels.

  That is, the function operates on an array of KEY_FIELD elements which has
  two parts:

                      $LEFT_PART             $RIGHT_PART
             +-----------------------+-----------------------+
            start                new_fields                 end
         
  $LEFT_PART and $RIGHT_PART are arrays that have KEY_FIELD elements for two
  parts of the OR condition. Our task is to produce an array of KEY_FIELD 
  elements that would correspond to "$LEFT_PART OR $RIGHT_PART". 
  
  The rules for combining elements are as follows:

    (keyfieldA1 AND keyfieldA2 AND ...) OR (keyfieldB1 AND keyfieldB2 AND ...)=
     
     = AND_ij (keyfieldA_i OR keyfieldB_j)
  
  We discard all (keyfieldA_i OR keyfieldB_j) that refer to different
  fields. For those referring to the same field, the logic is as follows:
    
    t.keycol=expr1 OR t.keycol=expr2 -> (since expr1 and expr2 are different 
                                         we can't produce a single equality,
                                         so produce nothing)

    t.keycol=expr1 OR t.keycol=expr1 -> t.keycol=expr1

    t.keycol=expr1 OR t.keycol IS NULL -> t.keycol=expr1, and also set
                                          KEY_OPTIMIZE_REF_OR_NULL flag

  The last one is for ref_or_null access. We have handling for this special
  because it's needed for evaluating IN subqueries that are internally
  transformed into 

  @code
    EXISTS(SELECT * FROM t1 WHERE t1.key=outer_ref_field or t1.key IS NULL)
  @endcode

  See add_key_fields() for discussion of what is and_level.

  KEY_FIELD::null_rejecting is processed as follows: @n
  result has null_rejecting=true if it is set for both ORed references.
  for example:
  -   (t2.key = t1.field OR t2.key  =  t1.field) -> null_rejecting=true
  -   (t2.key = t1.field OR t2.key <=> t1.field) -> null_rejecting=false

  @todo
    The result of this is that we're missing some 'ref' accesses.
    OptimizerTeam: Fix this
*/

static KEY_FIELD *
merge_key_fields(KEY_FIELD *start,KEY_FIELD *new_fields,KEY_FIELD *end,
		 uint and_level)
{
  if (start == new_fields)
    return start;				// Impossible or
  if (new_fields == end)
    return start;				// No new fields, skip all

  KEY_FIELD *first_free=new_fields;

  /* Mark all found fields in old array */
  for (; new_fields != end ; new_fields++)
  {
    for (KEY_FIELD *old=start ; old != first_free ; old++)
    {
      if (old->field == new_fields->field)
      {
        /*
          NOTE: below const_item() call really works as "!used_tables()", i.e.
          it can return FALSE where it is feasible to make it return TRUE.
          
          The cause is as follows: Some of the tables are already known to be
          const tables (the detection code is in make_join_statistics(),
          above the update_ref_and_keys() call), but we didn't propagate 
          information about this: TABLE::const_table is not set to TRUE, and
          Item::update_used_tables() hasn't been called for each item.
          The result of this is that we're missing some 'ref' accesses.
          TODO: OptimizerTeam: Fix this
        */
	if (!new_fields->val->const_item())
	{
	  /*
	    If the value matches, we can use the key reference.
	    If not, we keep it until we have examined all new values
	  */
	  if (old->val->eq(new_fields->val, old->field->binary()))
	  {
	    old->level= and_level;
	    old->optimize= ((old->optimize & new_fields->optimize &
			     KEY_OPTIMIZE_EXISTS) |
			    ((old->optimize | new_fields->optimize) &
			     KEY_OPTIMIZE_REF_OR_NULL));
            old->null_rejecting= (old->null_rejecting &&
                                  new_fields->null_rejecting);
	  }
	}
	else if (old->eq_func && new_fields->eq_func &&
                 old->val->eq_by_collation(new_fields->val, 
                                           old->field->binary(),
                                           old->field->charset()))

	{
	  old->level= and_level;
	  old->optimize= ((old->optimize & new_fields->optimize &
			   KEY_OPTIMIZE_EXISTS) |
			  ((old->optimize | new_fields->optimize) &
			   KEY_OPTIMIZE_REF_OR_NULL));
          old->null_rejecting= (old->null_rejecting &&
                                new_fields->null_rejecting);
	}
	else if (old->eq_func && new_fields->eq_func &&
		 ((old->val->const_item() && !old->val->is_expensive() &&
                   old->val->is_null()) ||
                  (!new_fields->val->is_expensive() &&
                   new_fields->val->is_null())))
	{
	  /* field = expression OR field IS NULL */
	  old->level= and_level;
          if (old->field->maybe_null())
	  {
	    old->optimize= KEY_OPTIMIZE_REF_OR_NULL;
            /* The referred expression can be NULL: */ 
            old->null_rejecting= 0;
	  }
	  /*
            Remember the NOT NULL value unless the value does not depend
            on other tables.
          */
	  if (!old->val->used_tables() && !old->val->is_expensive() &&
              old->val->is_null())
	    old->val= new_fields->val;
	}
	else
	{
	  /*
	    We are comparing two different const.  In this case we can't
	    use a key-lookup on this so it's better to remove the value
	    and let the range optimzier handle it
	  */
	  if (old == --first_free)		// If last item
	    break;
	  *old= *first_free;			// Remove old value
	  old--;				// Retry this value
	}
      }
    }
  }
  /* Remove all not used items */
  for (KEY_FIELD *old=start ; old != first_free ;)
  {
    if (old->level != and_level)
    {						// Not used in all levels
      if (old == --first_free)
	break;
      *old= *first_free;			// Remove old value
      continue;
    }
    old++;
  }
  return first_free;
}


/*
  Given a field, return its index in semi-join's select list, or UINT_MAX

  DESCRIPTION
    Given a field, we find its table; then see if the table is within a
    semi-join nest and if the field was in select list of the subselect.
    If it was, we return field's index in the select list. The value is used
    by LooseScan strategy.
*/

static uint get_semi_join_select_list_index(Field *field)
{
  uint res= UINT_MAX;
  TABLE_LIST *emb_sj_nest;
  if ((emb_sj_nest= field->table->pos_in_table_list->embedding) &&
      emb_sj_nest->sj_on_expr)
  {
    Item_in_subselect *subq_pred= emb_sj_nest->sj_subq_pred;
    st_select_lex *subq_lex= subq_pred->unit->first_select();
    if (subq_pred->left_expr->cols() == 1)
    {
      Item *sel_item= subq_lex->ref_pointer_array[0];
      if (sel_item->type() == Item::FIELD_ITEM &&
          ((Item_field*)sel_item)->field->eq(field))
      {
        res= 0;
      }
    }
    else
    {
      for (uint i= 0; i < subq_pred->left_expr->cols(); i++)
      {
        Item *sel_item= subq_lex->ref_pointer_array[i];
        if (sel_item->type() == Item::FIELD_ITEM &&
            ((Item_field*)sel_item)->field->eq(field))
        {
          res= i;
          break;
        }
      }
    }
  }
  return res;
}


/**
  Add a possible key to array of possible keys if it's usable as a key

    @param key_fields      Pointer to add key, if usable
    @param and_level       And level, to be stored in KEY_FIELD
    @param cond            Condition predicate
    @param field           Field used in comparision
    @param eq_func         True if we used =, <=> or IS NULL
    @param value           Value used for comparison with field
    @param num_values      Number of values[] that we are comparing against
    @param usable_tables   Tables which can be used for key optimization
    @param sargables       IN/OUT Array of found sargable candidates
    @param row_col_no      if = n that > 0 then field is compared only
                           against the n-th component of row values

  @note
    If we are doing a NOT NULL comparison on a NOT NULL field in a outer join
    table, we store this to be able to do not exists optimization later.

  @returns
    *key_fields is incremented if we stored a key in the array
*/

static void
add_key_field(JOIN *join,
              KEY_FIELD **key_fields,uint and_level, Item_bool_func *cond,
              Field *field, bool eq_func, Item **value, uint num_values,
              table_map usable_tables, SARGABLE_PARAM **sargables,
              uint row_col_no= 0)
{
  uint optimize= 0;  
  if (eq_func &&
      ((join->is_allowed_hash_join_access() &&
        field->hash_join_is_possible() && 
        !(field->table->pos_in_table_list->is_materialized_derived() &&
          field->table->is_created())) ||
       (field->table->pos_in_table_list->is_materialized_derived() &&
        !field->table->is_created() && !(field->flags & BLOB_FLAG))))
  {
    optimize= KEY_OPTIMIZE_EQ;
  }   
  else if (!(field->flags & PART_KEY_FLAG))
  {
    // Don't remove column IS NULL on a LEFT JOIN table
    if (eq_func && (*value)->type() == Item::NULL_ITEM &&
        field->table->maybe_null && !field->null_ptr)
    {
      optimize= KEY_OPTIMIZE_EXISTS;
      DBUG_ASSERT(num_values == 1);
    }
  }
  if (optimize != KEY_OPTIMIZE_EXISTS)
  {
    table_map used_tables=0;
    bool optimizable=0;
    for (uint i=0; i<num_values; i++)
    {
      Item *curr_val; 
      if (row_col_no && value[i]->real_item()->type() == Item::ROW_ITEM)
      {
        Item_row *value_tuple= (Item_row *) (value[i]->real_item());
        curr_val= value_tuple->element_index(row_col_no - 1);
      }
      else
        curr_val= value[i];
      table_map value_used_tables= curr_val->used_tables();
      used_tables|= value_used_tables;
      if (!(value_used_tables & (field->table->map | RAND_TABLE_BIT)))
        optimizable=1;
    }
    if (!optimizable)
      return;
    if (!(usable_tables & field->table->map))
    {
      if (!eq_func || (*value)->type() != Item::NULL_ITEM ||
          !field->table->maybe_null || field->null_ptr)
	return;					// Can't use left join optimize
      optimize= KEY_OPTIMIZE_EXISTS;
    }
    else
    {
      JOIN_TAB *stat=field->table->reginfo.join_tab;
      key_map possible_keys=field->get_possible_keys();
      possible_keys.intersect(field->table->keys_in_use_for_query);
      stat[0].keys.merge(possible_keys);             // Add possible keys

      /*
	Save the following cases:
	Field op constant
	Field LIKE constant where constant doesn't start with a wildcard
	Field = field2 where field2 is in a different table
	Field op formula
	Field IS NULL
	Field IS NOT NULL
         Field BETWEEN ...
         Field IN ...
      */
      if (field->flags & PART_KEY_FLAG)
        stat[0].key_dependent|=used_tables;

      bool is_const=1;
      for (uint i=0; i<num_values; i++)
      {
        Item *curr_val;
        if (row_col_no && value[i]->real_item()->type() == Item::ROW_ITEM)
	{
          Item_row *value_tuple= (Item_row *) (value[i]->real_item());
          curr_val= value_tuple->element_index(row_col_no - 1);
        }
        else
          curr_val= value[i];
        if (!(is_const&= curr_val->const_item()))
          break;
      }
      if (is_const)
      {
        stat[0].const_keys.merge(possible_keys);
        bitmap_set_bit(&field->table->cond_set, field->field_index);
      }
      else if (!eq_func)
      {
        /* 
          Save info to be able check whether this predicate can be 
          considered as sargable for range analisis after reading const tables.
          We do not save info about equalities as update_const_equal_items
          will take care of updating info on keys from sargable equalities. 
        */
        (*sargables)--;
        (*sargables)->field= field;
        (*sargables)->arg_value= value;
        (*sargables)->num_values= num_values;
      }
      if (!eq_func) // eq_func is NEVER true when num_values > 1
        return;
    }
  }
  /*
    For the moment eq_func is always true. This slot is reserved for future
    extensions where we want to remembers other things than just eq comparisons
  */
  DBUG_ASSERT(eq_func);
  /* Store possible eq field */
  (*key_fields)->field=		field;
  (*key_fields)->eq_func=	eq_func;
  (*key_fields)->val=		*value;
  (*key_fields)->cond=          cond;
  (*key_fields)->level=         and_level;
  (*key_fields)->optimize=      optimize;
  /*
    If the condition we are analyzing is NULL-rejecting and at least
    one side of the equalities is NULLable, mark the KEY_FIELD object as
    null-rejecting. This property is used by:
    - add_not_null_conds() to add "column IS NOT NULL" conditions
    - best_access_path() to produce better estimates for NULL-able unique keys.
  */
  {
    if ((cond->functype() == Item_func::EQ_FUNC ||
         cond->functype() == Item_func::MULT_EQUAL_FUNC) &&
        ((*value)->maybe_null || field->real_maybe_null()))
      (*key_fields)->null_rejecting= true;
    else
      (*key_fields)->null_rejecting= false;
  }
  (*key_fields)->cond_guard= NULL;

  (*key_fields)->sj_pred_no= get_semi_join_select_list_index(field);
  (*key_fields)++;
}

/**
  Add possible keys to array of possible keys originated from a simple
  predicate.

    @param  key_fields     Pointer to add key, if usable
    @param  and_level      And level, to be stored in KEY_FIELD
    @param  cond           Condition predicate
    @param  field_item     Field item used for comparison
    @param  eq_func        True if we used =, <=> or IS NULL
    @param  value          Value used for comparison with field_item
    @param   num_values    Number of values[] that we are comparing against 
    @param  usable_tables  Tables which can be used for key optimization
    @param  sargables      IN/OUT Array of found sargable candidates
    @param row_col_no      if = n that > 0 then field is compared only
                           against the n-th component of row values    

  @note
    If field items f1 and f2 belong to the same multiple equality and
    a key is added for f1, the the same key is added for f2.

  @returns
    *key_fields is incremented if we stored a key in the array
*/

static void
add_key_equal_fields(JOIN *join, KEY_FIELD **key_fields, uint and_level,
                     Item_bool_func *cond, Item *field_item,
                     bool eq_func, Item **val,
                     uint num_values, table_map usable_tables,
                     SARGABLE_PARAM **sargables, uint row_col_no= 0)
{
  Field *field= ((Item_field *) (field_item->real_item()))->field;
  add_key_field(join, key_fields, and_level, cond, field,
                eq_func, val, num_values, usable_tables, sargables,
                row_col_no);
  Item_equal *item_equal= field_item->get_item_equal();
  if (item_equal)
  { 
    /*
      Add to the set of possible key values every substitution of
      the field for an equal field included into item_equal
    */
    Item_equal_fields_iterator it(*item_equal);
    while (it++)
    {
      Field *equal_field= it.get_curr_field();
      if (!field->eq(equal_field))
      {
        add_key_field(join, key_fields, and_level, cond, equal_field,
                      eq_func, val, num_values, usable_tables,
                      sargables, row_col_no);
      }
    }
  }
}


/**
  Check if an expression is a non-outer field.

  Checks if an expression is a field and belongs to the current select.

  @param   field  Item expression to check

  @return boolean
     @retval TRUE   the expression is a local field
     @retval FALSE  it's something else
*/

static bool
is_local_field (Item *field)
{
  return field->real_item()->type() == Item::FIELD_ITEM
     && !(field->used_tables() & OUTER_REF_TABLE_BIT)
    && !((Item_field *)field->real_item())->get_depended_from();
}


/*
  In this and other functions, and_level is a number that is ever-growing
  and is different for the contents of every AND or OR clause. For example,
  when processing clause

     (a AND b AND c) OR (x AND y)
  
  we'll have
   * KEY_FIELD elements for (a AND b AND c) are assigned and_level=1
   * KEY_FIELD elements for (x AND y) are assigned and_level=2
   * OR operation is performed, and whatever elements are left after it are
     assigned and_level=3.

  The primary reason for having and_level attribute is the OR operation which 
  uses and_level to mark KEY_FIELDs that should get into the result of the OR
  operation
*/


void
Item_cond_and::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                              uint *and_level, table_map usable_tables,
                              SARGABLE_PARAM **sargables)
{
  List_iterator_fast<Item> li(*argument_list());
  KEY_FIELD *org_key_fields= *key_fields;

  Item *item;
  while ((item=li++))
    item->add_key_fields(join, key_fields, and_level, usable_tables,
                         sargables);
  for (; org_key_fields != *key_fields ; org_key_fields++)
    org_key_fields->level= *and_level;
}


void
Item_cond::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                          uint *and_level, table_map usable_tables,
                          SARGABLE_PARAM **sargables)
{
  List_iterator_fast<Item> li(*argument_list());
  KEY_FIELD *org_key_fields= *key_fields;

  (*and_level)++;
  (li++)->add_key_fields(join, key_fields, and_level, usable_tables,
                         sargables);
  Item *item;
  while ((item=li++))
  {
    KEY_FIELD *start_key_fields= *key_fields;
    (*and_level)++;
    item->add_key_fields(join, key_fields, and_level, usable_tables,
                         sargables);
    *key_fields= merge_key_fields(org_key_fields,start_key_fields,
                                  *key_fields, ++(*and_level));
  }
}


void
Item_func_trig_cond::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                                    uint *and_level, table_map usable_tables,
                                    SARGABLE_PARAM **sargables)
{
  /* 
    Subquery optimization: Conditions that are pushed down into subqueries
    are wrapped into Item_func_trig_cond. We process the wrapped condition
    but need to set cond_guard for KEYUSE elements generated from it.
  */
  if (!join->group_list && !join->order &&
      join->unit->item && 
      join->unit->item->substype() == Item_subselect::IN_SUBS &&
      !join->unit->is_unit_op())
  {
    KEY_FIELD *save= *key_fields;
    args[0]->add_key_fields(join, key_fields, and_level, usable_tables,
                            sargables);
    // Indicate that this ref access candidate is for subquery lookup:
    for (; save != *key_fields; save++)
      save->cond_guard= get_trig_var();
  }
}


void
Item_func_between::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                                  uint *and_level, table_map usable_tables,
                                  SARGABLE_PARAM **sargables)
{
  /*
    Build list of possible keys for 'a BETWEEN low AND high'.
    It is handled similar to the equivalent condition 
    'a >= low AND a <= high':
  */
  Item_field *field_item;
  bool equal_func= false;
  uint num_values= 2;

  bool binary_cmp= (args[0]->real_item()->type() == Item::FIELD_ITEM)
        ? ((Item_field*) args[0]->real_item())->field->binary()
        : true;
  /*
    Additional optimization: If 'low = high':
    Handle as if the condition was "t.key = low".
  */
  if (!negated && args[1]->eq(args[2], binary_cmp))
  {
    equal_func= true;
    num_values= 1;
  }

  /*
    Append keys for 'field <cmp> value[]' if the
    condition is of the form::
    '<field> BETWEEN value[1] AND value[2]'
  */
  if (is_local_field(args[0]))
  {
    field_item= (Item_field *) (args[0]->real_item());
    add_key_equal_fields(join, key_fields, *and_level, this,
                         field_item, equal_func, &args[1],
                         num_values, usable_tables, sargables);
  }
  /*
    Append keys for 'value[0] <cmp> field' if the
    condition is of the form:
    'value[0] BETWEEN field1 AND field2'
  */
  for (uint i= 1; i <= num_values; i++)
  {
    if (is_local_field(args[i]))
    {
      field_item= (Item_field *) (args[i]->real_item());
      add_key_equal_fields(join, key_fields, *and_level, this,
                           field_item, equal_func, args,
                           1, usable_tables, sargables);
    }
  }
}


void
Item_func_in::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                             uint *and_level, table_map usable_tables,
                             SARGABLE_PARAM **sargables)
{
  if (is_local_field(args[0]) && !(used_tables() & OUTER_REF_TABLE_BIT))
  {
    DBUG_ASSERT(arg_count != 2);
    add_key_equal_fields(join, key_fields, *and_level, this,
                         (Item_field*) (args[0]->real_item()), false,
                         args + 1, arg_count - 1, usable_tables, sargables);
  }
  else if (key_item()->type() == Item::ROW_ITEM &&
           !(used_tables() & OUTER_REF_TABLE_BIT))
  {
    Item_row *key_row= (Item_row *) key_item();
    Item **key_col= key_row->addr(0);
    uint row_cols= key_row->cols();
    for (uint i= 0; i < row_cols; i++, key_col++)
    {
      if (is_local_field(*key_col))
      {
        Item_field *field_item= (Item_field *)((*key_col)->real_item());
        add_key_equal_fields(join, key_fields, *and_level, this,
                             field_item, false, args + 1, arg_count - 1,
                             usable_tables, sargables, i + 1);
      } 
    }
  }
  
}


void
Item_func_ne::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                             uint *and_level, table_map usable_tables,
                             SARGABLE_PARAM **sargables)
{
  if (!(used_tables() & OUTER_REF_TABLE_BIT))
  {
    /*
      QQ: perhaps test for !is_local_field(args[1]) is not really needed here.
      Other comparison functions, e.g. Item_func_le, Item_func_gt, etc,
      do not have this test. See Item_bool_func2::add_key_fieldoptimize_op().
      Check with the optimizer team.
    */
    if (is_local_field(args[0]) && !is_local_field(args[1]))
      add_key_equal_fields(join, key_fields, *and_level, this,
                           (Item_field*) (args[0]->real_item()), false,
                           &args[1], 1, usable_tables, sargables);
    /*
      QQ: perhaps test for !is_local_field(args[0]) is not really needed here.
    */
    if (is_local_field(args[1]) && !is_local_field(args[0]))
      add_key_equal_fields(join, key_fields, *and_level, this,
                           (Item_field*) (args[1]->real_item()), false,
                           &args[0], 1, usable_tables, sargables);
  }
}


void
Item_func_like::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                               uint *and_level, table_map usable_tables,
                               SARGABLE_PARAM **sargables)
{
  if (is_local_field(args[0]) && with_sargable_pattern())
  {
    /*
      SELECT * FROM t1 WHERE field LIKE const_pattern
      const_pattern starts with a non-wildcard character
    */
    add_key_equal_fields(join, key_fields, *and_level, this,
                         (Item_field*) args[0]->real_item(), false,
                         args + 1, 1, usable_tables, sargables);
  }
}


void
Item_bool_func2::add_key_fields_optimize_op(JOIN *join, KEY_FIELD **key_fields,
                                            uint *and_level,
                                            table_map usable_tables,
                                            SARGABLE_PARAM **sargables,
                                            bool equal_func)
{
  /* If item is of type 'field op field/constant' add it to key_fields */
  if (is_local_field(args[0]))
  {
    add_key_equal_fields(join, key_fields, *and_level, this,
                         (Item_field*) args[0]->real_item(), equal_func,
                         args + 1, 1, usable_tables, sargables);
  }
  if (is_local_field(args[1]))
  {
    add_key_equal_fields(join, key_fields, *and_level, this, 
                         (Item_field*) args[1]->real_item(), equal_func,
                         args, 1, usable_tables, sargables);
  }
}


void
Item_func_null_predicate::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                                         uint *and_level,
                                         table_map usable_tables,
                                         SARGABLE_PARAM **sargables)
{
  /* column_name IS [NOT] NULL */
  if (is_local_field(args[0]) && !(used_tables() & OUTER_REF_TABLE_BIT))
  {
    Item *tmp= new (join->thd->mem_root) Item_null(join->thd);
    if (unlikely(!tmp))                       // Should never be true
      return;
    add_key_equal_fields(join, key_fields, *and_level, this,
                         (Item_field*) args[0]->real_item(),
                         functype() == Item_func::ISNULL_FUNC,
                         &tmp, 1, usable_tables, sargables);
  }
}


void
Item_equal::add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                           uint *and_level, table_map usable_tables,
                           SARGABLE_PARAM **sargables)
{
  Item *const_item2= get_const();
  Item_equal_fields_iterator it(*this);
  if (const_item2)
  {
    
    /*
      For each field field1 from item_equal consider the equality 
      field1=const_item as a condition allowing an index access of the table
      with field1 by the keys value of field1.
    */   
    while (it++)
    {
      Field *equal_field= it.get_curr_field();
      add_key_field(join, key_fields, *and_level, this, equal_field,
                    TRUE, &const_item2, 1, usable_tables, sargables);
    }
  }
  else 
  {
    /*
      Consider all pairs of different fields included into item_equal.
      For each of them (field1, field1) consider the equality 
      field1=field2 as a condition allowing an index access of the table
      with field1 by the keys value of field2.
    */   
    Item_equal_fields_iterator fi(*this);
    while (fi++)
    {
      Field *field= fi.get_curr_field();
      Item *item;
      while ((item= it++))
      {
        Field *equal_field= it.get_curr_field();
        if (!field->eq(equal_field))
        {
          add_key_field(join, key_fields, *and_level, this, field,
                        TRUE, &item, 1, usable_tables,
                        sargables);
        }
      }
      it.rewind();
    }
  }
}


static uint
max_part_bit(key_part_map bits)
{
  uint found;
  for (found=0; bits & 1 ; found++,bits>>=1) ;
  return found;
}


/**
  Add a new keuse to the specified array of KEYUSE objects

  @param[in,out]  keyuse_array  array of keyuses to be extended 
  @param[in]      key_field     info on the key use occurrence
  @param[in]      key           key number for the keyse to be added
  @param[in]      part          key part for the keyuse to be added

  @note
  The function builds a new KEYUSE object for a key use utilizing the info
  on the left and right parts of the given key use  extracted from the 
  structure key_field, the key number and key part for this key use. 
  The built object is added to the dynamic array keyuse_array.

  @retval         0             the built object is succesfully added 
  @retval         1             otherwise
*/

static bool
add_keyuse(DYNAMIC_ARRAY *keyuse_array, KEY_FIELD *key_field,
          uint key, uint part)
{
  KEYUSE keyuse;
  Field *field= key_field->field;

  keyuse.table= field->table;
  keyuse.val= key_field->val;
  keyuse.key= key;
  if (!is_hash_join_key_no(key))
  {
    keyuse.keypart=part;
    keyuse.keypart_map= (key_part_map) 1 << part;
  }
  else
  {
    keyuse.keypart= field->field_index;
    keyuse.keypart_map= (key_part_map) 0;
  }
  keyuse.used_tables= key_field->val->used_tables();
  keyuse.optimize= key_field->optimize & KEY_OPTIMIZE_REF_OR_NULL;
  keyuse.ref_table_rows= 0;
  keyuse.null_rejecting= key_field->null_rejecting;
  keyuse.cond_guard= key_field->cond_guard;
  keyuse.sj_pred_no= key_field->sj_pred_no;
  keyuse.validity_ref= 0;
  return (insert_dynamic(keyuse_array,(uchar*) &keyuse));
}


/*
  Add all keys with uses 'field' for some keypart
  If field->and_level != and_level then only mark key_part as const_part

  RETURN 
   0 - OK
   1 - Out of memory.
*/

static bool
add_key_part(DYNAMIC_ARRAY *keyuse_array, KEY_FIELD *key_field)
{
  Field *field=key_field->field;
  TABLE *form= field->table;

  if (key_field->eq_func && !(key_field->optimize & KEY_OPTIMIZE_EXISTS))
  {
    for (uint key=0 ; key < form->s->keys ; key++)
    {
      if (!(form->keys_in_use_for_query.is_set(key)))
	continue;
      if (form->key_info[key].flags & (HA_FULLTEXT | HA_SPATIAL))
	continue;    // ToDo: ft-keys in non-ft queries.   SerG

      KEY *keyinfo= form->key_info+key;
      uint key_parts= form->actual_n_key_parts(keyinfo);
      for (uint part=0 ; part <  key_parts ; part++)
      {
        if (field->eq(form->key_info[key].key_part[part].field) &&
            field->can_optimize_keypart_ref(key_field->cond, key_field->val))
	{
          if (add_keyuse(keyuse_array, key_field, key, part))
            return TRUE;
	}
      }
    }
    if (field->hash_join_is_possible() &&
        (key_field->optimize & KEY_OPTIMIZE_EQ) &&
        key_field->val->used_tables())
    {
      if (!field->can_optimize_hash_join(key_field->cond, key_field->val))
        return false;
      if (form->is_splittable())
        form->add_splitting_info_for_key_field(key_field);
      /* 
        If a key use is extracted from an equi-join predicate then it is
        added not only as a key use for every index whose component can
        be evalusted utilizing this key use, but also as a key use for
        hash join. Such key uses are marked with a special key number. 
      */    
      if (add_keyuse(keyuse_array, key_field, get_hash_join_key_no(), 0))
        return TRUE;
    }
  }
  return FALSE;
}

static bool
add_ft_keys(DYNAMIC_ARRAY *keyuse_array,
            JOIN_TAB *stat,COND *cond,table_map usable_tables)
{
  Item_func_match *cond_func=NULL;

  if (!cond)
    return FALSE;

  if (cond->type() == Item::FUNC_ITEM)
  {
    Item_func *func=(Item_func *)cond;
    Item_func::Functype functype=  func->functype();
    if (functype == Item_func::FT_FUNC)
      cond_func=(Item_func_match *)cond;
    else if (func->argument_count() == 2)
    {
      Item *arg0=(Item *)(func->arguments()[0]),
           *arg1=(Item *)(func->arguments()[1]);
      if (arg1->const_item() && arg1->cols() == 1 &&
           arg0->type() == Item::FUNC_ITEM &&
           ((Item_func *) arg0)->functype() == Item_func::FT_FUNC &&
          ((functype == Item_func::GE_FUNC && arg1->val_real() > 0) ||
           (functype == Item_func::GT_FUNC && arg1->val_real() >=0)))
        cond_func= (Item_func_match *) arg0;
      else if (arg0->const_item() && arg0->cols() == 1 &&
                arg1->type() == Item::FUNC_ITEM &&
                ((Item_func *) arg1)->functype() == Item_func::FT_FUNC &&
               ((functype == Item_func::LE_FUNC && arg0->val_real() > 0) ||
                (functype == Item_func::LT_FUNC && arg0->val_real() >=0)))
        cond_func= (Item_func_match *) arg1;
    }
  }
  else if (cond->type() == Item::COND_ITEM)
  {
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());

    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      Item *item;
      while ((item=li++))
      {
        if (add_ft_keys(keyuse_array,stat,item,usable_tables))
          return TRUE;
      }
    }
  }

  if (!cond_func || cond_func->key == NO_SUCH_KEY ||
      !(usable_tables & cond_func->table->map))
    return FALSE;

  KEYUSE keyuse;
  keyuse.table= cond_func->table;
  keyuse.val =  cond_func;
  keyuse.key =  cond_func->key;
  keyuse.keypart= FT_KEYPART;
  keyuse.used_tables=cond_func->key_item()->used_tables();
  keyuse.optimize= 0;
  keyuse.ref_table_rows= 0;
  keyuse.keypart_map= 0;
  keyuse.sj_pred_no= UINT_MAX;
  keyuse.validity_ref= 0;
  keyuse.null_rejecting= FALSE;
  return insert_dynamic(keyuse_array,(uchar*) &keyuse);
}


static int
sort_keyuse(KEYUSE *a,KEYUSE *b)
{
  int res;
  if (a->table->tablenr != b->table->tablenr)
    return (int) (a->table->tablenr - b->table->tablenr);
  if (a->key != b->key)
    return (int) (a->key - b->key);
  if (a->key == MAX_KEY && b->key == MAX_KEY && 
      a->used_tables != b->used_tables)
    return (int) ((ulong) a->used_tables - (ulong) b->used_tables);
  if (a->keypart != b->keypart)
    return (int) (a->keypart - b->keypart);
  // Place const values before other ones
  if ((res= MY_TEST((a->used_tables & ~OUTER_REF_TABLE_BIT)) -
       MY_TEST((b->used_tables & ~OUTER_REF_TABLE_BIT))))
    return res;
  /* Place rows that are not 'OPTIMIZE_REF_OR_NULL' first */
  return (int) ((a->optimize & KEY_OPTIMIZE_REF_OR_NULL) -
		(b->optimize & KEY_OPTIMIZE_REF_OR_NULL));
}


/*
  Add to KEY_FIELD array all 'ref' access candidates within nested join.

    This function populates KEY_FIELD array with entries generated from the 
    ON condition of the given nested join, and does the same for nested joins 
    contained within this nested join.

  @param[in]      nested_join_table   Nested join pseudo-table to process
  @param[in,out]  end                 End of the key field array
  @param[in,out]  and_level           And-level
  @param[in,out]  sargables           Array of found sargable candidates


  @note
    We can add accesses to the tables that are direct children of this nested 
    join (1), and are not inner tables w.r.t their neighbours (2).
    
    Example for #1 (outer brackets pair denotes nested join this function is 
    invoked for):
    @code
     ... LEFT JOIN (t1 LEFT JOIN (t2 ... ) ) ON cond
    @endcode
    Example for #2:
    @code
     ... LEFT JOIN (t1 LEFT JOIN t2 ) ON cond
    @endcode
    In examples 1-2 for condition cond, we can add 'ref' access candidates to 
    t1 only.
    Example #3:
    @code
     ... LEFT JOIN (t1, t2 LEFT JOIN t3 ON inner_cond) ON cond
    @endcode
    Here we can add 'ref' access candidates for t1 and t2, but not for t3.
*/

static void add_key_fields_for_nj(JOIN *join, TABLE_LIST *nested_join_table,
                                  KEY_FIELD **end, uint *and_level,
                                  SARGABLE_PARAM **sargables)
{
  List_iterator<TABLE_LIST> li(nested_join_table->nested_join->join_list);
  List_iterator<TABLE_LIST> li2(nested_join_table->nested_join->join_list);
  bool have_another = FALSE;
  table_map tables= 0;
  TABLE_LIST *table;
  DBUG_ASSERT(nested_join_table->nested_join);

  while ((table= li++) || (have_another && (li=li2, have_another=FALSE,
                                            (table= li++))))
  {
    if (table->nested_join)
    {
      if (!table->on_expr)
      {
        /* It's a semi-join nest. Walk into it as if it wasn't a nest */
        have_another= TRUE;
        li2= li;
        li= List_iterator<TABLE_LIST>(table->nested_join->join_list); 
      }
      else
        add_key_fields_for_nj(join, table, end, and_level, sargables);
    }
    else
      if (!table->on_expr)
        tables |= table->table->map;
  }
  if (nested_join_table->on_expr)
    nested_join_table->on_expr->add_key_fields(join, end, and_level, tables,
                                               sargables);
}


void count_cond_for_nj(SELECT_LEX *sel, TABLE_LIST *nested_join_table)
{
  List_iterator<TABLE_LIST> li(nested_join_table->nested_join->join_list);
  List_iterator<TABLE_LIST> li2(nested_join_table->nested_join->join_list);
  bool have_another = FALSE;
  TABLE_LIST *table;

  while ((table= li++) || (have_another && (li=li2, have_another=FALSE,
                                            (table= li++))))
  if (table->nested_join)
  {
    if (!table->on_expr)
    {
      /* It's a semi-join nest. Walk into it as if it wasn't a nest */
      have_another= TRUE;
      li2= li;
      li= List_iterator<TABLE_LIST>(table->nested_join->join_list); 
    }
    else
      count_cond_for_nj(sel, table); 
  }
  if (nested_join_table->on_expr)
    nested_join_table->on_expr->walk(&Item::count_sargable_conds, 0, sel);
    
}

/**
  Update keyuse array with all possible keys we can use to fetch rows.
  
  @param       thd 
  @param[out]  keyuse         Put here ordered array of KEYUSE structures
  @param       join_tab       Array in tablenr_order
  @param       tables         Number of tables in join
  @param       cond           WHERE condition (note that the function analyzes
                              join_tab[i]->on_expr too)
  @param       normal_tables  Tables not inner w.r.t some outer join (ones
                              for which we can make ref access based the WHERE
                              clause)
  @param       select_lex     current SELECT
  @param[out]  sargables      Array of found sargable candidates
      
   @retval
     0  OK
   @retval
     1  Out of memory.
*/

static bool
update_ref_and_keys(THD *thd, DYNAMIC_ARRAY *keyuse,JOIN_TAB *join_tab,
                    uint tables, COND *cond, table_map normal_tables,
                    SELECT_LEX *select_lex, SARGABLE_PARAM **sargables)
{
  uint	and_level,i;
  KEY_FIELD *key_fields, *end, *field;
  uint sz;
  uint m= MY_MAX(select_lex->max_equal_elems,1);
  DBUG_ENTER("update_ref_and_keys");
  DBUG_PRINT("enter", ("normal_tables: %llx", normal_tables));

  SELECT_LEX *sel=thd->lex->current_select; 
  sel->cond_count= 0;
  sel->between_count= 0; 
  if (cond)
    cond->walk(&Item::count_sargable_conds, 0, sel);
  for (i=0 ; i < tables ; i++)
  {
    if (*join_tab[i].on_expr_ref)
      (*join_tab[i].on_expr_ref)->walk(&Item::count_sargable_conds, 0, sel);
  }
  {
    List_iterator<TABLE_LIST> li(*join_tab->join->join_list);
    TABLE_LIST *table;
    while ((table= li++))
    {
      if (table->nested_join)
        count_cond_for_nj(sel, table);
    }
  }
  
  /* 
    We use the same piece of memory to store both  KEY_FIELD 
    and SARGABLE_PARAM structure.
    KEY_FIELD values are placed at the beginning this memory
    while  SARGABLE_PARAM values are put at the end.
    All predicates that are used to fill arrays of KEY_FIELD
    and SARGABLE_PARAM structures have at most 2 arguments
    except BETWEEN predicates that have 3 arguments and 
    IN predicates.
    This any predicate if it's not BETWEEN/IN can be used 
    directly to fill at most 2 array elements, either of KEY_FIELD
    or SARGABLE_PARAM type. For a BETWEEN predicate 3 elements
    can be filled as this predicate is considered as
    saragable with respect to each of its argument.
    An IN predicate can require at most 1 element as currently
    it is considered as sargable only for its first argument.
    Multiple equality can add  elements that are filled after
    substitution of field arguments by equal fields. There
    can be not more than select_lex->max_equal_elems such 
    substitutions.
  */ 
  sz= MY_MAX(sizeof(KEY_FIELD),sizeof(SARGABLE_PARAM))*
    ((sel->cond_count*2 + sel->between_count)*m+1);
  if (!(key_fields=(KEY_FIELD*)	thd->alloc(sz)))
    DBUG_RETURN(TRUE); /* purecov: inspected */
  and_level= 0;
  field= end= key_fields;
  *sargables= (SARGABLE_PARAM *) key_fields + 
                (sz - sizeof((*sargables)[0].field))/sizeof(SARGABLE_PARAM);
  /* set a barrier for the array of SARGABLE_PARAM */
  (*sargables)[0].field= 0; 

  if (my_init_dynamic_array2(keyuse, sizeof(KEYUSE),
                             thd->alloc(sizeof(KEYUSE) * 20), 20, 64,
                             MYF(MY_THREAD_SPECIFIC)))
    DBUG_RETURN(TRUE);

  if (cond)
  {
    KEY_FIELD *saved_field= field;
    cond->add_key_fields(join_tab->join, &end, &and_level, normal_tables,
                         sargables);
    for (; field != end ; field++)
    {

      /* Mark that we can optimize LEFT JOIN */
      if (field->val->type() == Item::NULL_ITEM &&
	  !field->field->real_maybe_null())
	field->field->table->reginfo.not_exists_optimize=1;
    }
    field= saved_field;
  }
  for (i=0 ; i < tables ; i++)
  {
    /*
      Block the creation of keys for inner tables of outer joins.
      Here only the outer joins that can not be converted to
      inner joins are left and all nests that can be eliminated
      are flattened.
      In the future when we introduce conditional accesses
      for inner tables in outer joins these keys will be taken
      into account as well.
    */ 
    if (*join_tab[i].on_expr_ref)
      (*join_tab[i].on_expr_ref)->add_key_fields(join_tab->join, &end,
                                                 &and_level, 
                                                 join_tab[i].table->map,
                                                 sargables);
  }

  /* Process ON conditions for the nested joins */
  {
    List_iterator<TABLE_LIST> li(*join_tab->join->join_list);
    TABLE_LIST *table;
    while ((table= li++))
    {
      if (table->nested_join)
        add_key_fields_for_nj(join_tab->join, table, &end, &and_level, 
                              sargables);
    }
  }

  /* fill keyuse with found key parts */
  for ( ; field != end ; field++)
  {
    if (add_key_part(keyuse,field))
      DBUG_RETURN(TRUE);
  }

  if (select_lex->ftfunc_list->elements)
  {
    if (add_ft_keys(keyuse,join_tab,cond,normal_tables))
      DBUG_RETURN(TRUE);
  }

  DBUG_RETURN(FALSE);
}


/**
  Sort the array of possible keys and remove the following key parts:
  - ref if there is a keypart which is a ref and a const.
    (e.g. if there is a key(a,b) and the clause is a=3 and b=7 and b=t2.d,
    then we skip the key part corresponding to b=t2.d)
  - keyparts without previous keyparts
    (e.g. if there is a key(a,b,c) but only b < 5 (or a=2 and c < 3) is
    used in the query, we drop the partial key parts from consideration).
  Special treatment for ft-keys.
*/

bool sort_and_filter_keyuse(THD *thd, DYNAMIC_ARRAY *keyuse,
                            bool skip_unprefixed_keyparts)
{
  KEYUSE key_end, *prev, *save_pos, *use;
  uint found_eq_constant, i;

  DBUG_ASSERT(keyuse->elements);

  my_qsort(keyuse->buffer, keyuse->elements, sizeof(KEYUSE),
           (qsort_cmp) sort_keyuse);

  bzero((char*) &key_end, sizeof(key_end));    /* Add for easy testing */
  if (insert_dynamic(keyuse, (uchar*) &key_end))
    return TRUE;

  if (optimizer_flag(thd, OPTIMIZER_SWITCH_DERIVED_WITH_KEYS))
    generate_derived_keys(keyuse);

  use= save_pos= dynamic_element(keyuse,0,KEYUSE*);
  prev= &key_end;
  found_eq_constant= 0;
  for (i=0 ; i < keyuse->elements-1 ; i++,use++)
  {
    if (!use->is_for_hash_join())
    {
      if (!(use->used_tables & ~OUTER_REF_TABLE_BIT) && 
          use->optimize != KEY_OPTIMIZE_REF_OR_NULL)
        use->table->const_key_parts[use->key]|= use->keypart_map;
      if (use->keypart != FT_KEYPART)
      {
        if (use->key == prev->key && use->table == prev->table)
        {
          if ((prev->keypart+1 < use->keypart && skip_unprefixed_keyparts) ||
              (prev->keypart == use->keypart && found_eq_constant))
            continue;				/* remove */
        }
        else if (use->keypart != 0 && skip_unprefixed_keyparts)
          continue; /* remove - first found must be 0 */
      }

      prev= use;
      found_eq_constant= !use->used_tables;
      use->table->reginfo.join_tab->checked_keys.set_bit(use->key);
    }
    /*
      Old gcc used a memcpy(), which is undefined if save_pos==use:
      http://gcc.gnu.org/bugzilla/show_bug.cgi?id=19410
      http://gcc.gnu.org/bugzilla/show_bug.cgi?id=39480
      This also disables a valgrind warning, so better to have the test.
    */
    if (save_pos != use)
      *save_pos= *use;
    /* Save ptr to first use */
    if (!use->table->reginfo.join_tab->keyuse)
      use->table->reginfo.join_tab->keyuse= save_pos;
    save_pos++;
  }
  i= (uint) (save_pos-(KEYUSE*) keyuse->buffer);
  (void) set_dynamic(keyuse,(uchar*) &key_end,i);
  keyuse->elements= i;

  return FALSE;
}


/**
  Update some values in keyuse for faster choose_plan() loop.
*/

void optimize_keyuse(JOIN *join, DYNAMIC_ARRAY *keyuse_array)
{
  KEYUSE *end,*keyuse= dynamic_element(keyuse_array, 0, KEYUSE*);

  for (end= keyuse+ keyuse_array->elements ; keyuse < end ; keyuse++)
  {
    table_map map;
    /*
      If we find a ref, assume this table matches a proportional
      part of this table.
      For example 100 records matching a table with 5000 records
      gives 5000/100 = 50 records per key
      Constant tables are ignored.
      To avoid bad matches, we don't make ref_table_rows less than 100.
    */
    keyuse->ref_table_rows= ~(ha_rows) 0;	// If no ref
    if (keyuse->used_tables &
	(map= (keyuse->used_tables & ~join->const_table_map &
	       ~OUTER_REF_TABLE_BIT)))
    {
      uint n_tables= my_count_bits(map);
      if (n_tables == 1)			// Only one table
      {
        DBUG_ASSERT(!(map & PSEUDO_TABLE_BITS)); // Must be a real table
        Table_map_iterator it(map);
        int tablenr= it.next_bit();
        DBUG_ASSERT(tablenr != Table_map_iterator::BITMAP_END);
	TABLE *tmp_table=join->table[tablenr];
        if (tmp_table) // already created
          keyuse->ref_table_rows= MY_MAX(tmp_table->file->stats.records, 100);
      }
    }
    /*
      Outer reference (external field) is constant for single executing
      of subquery
    */
    if (keyuse->used_tables == OUTER_REF_TABLE_BIT)
      keyuse->ref_table_rows= 1;
  }
}


/**
  Check for the presence of AGGFN(DISTINCT a) queries that may be subject
  to loose index scan.


  Check if the query is a subject to AGGFN(DISTINCT) using loose index scan 
  (QUICK_GROUP_MIN_MAX_SELECT).
  Optionally (if out_args is supplied) will push the arguments of 
  AGGFN(DISTINCT) to the list

  Check for every COUNT(DISTINCT), AVG(DISTINCT) or
  SUM(DISTINCT). These can be resolved by Loose Index Scan as long
  as all the aggregate distinct functions refer to the same
  fields. Thus:

  SELECT AGGFN(DISTINCT a, b), AGGFN(DISTINCT b, a)... => can use LIS
  SELECT AGGFN(DISTINCT a),    AGGFN(DISTINCT a)   ... => can use LIS
  SELECT AGGFN(DISTINCT a, b), AGGFN(DISTINCT a)   ... => cannot use LIS
  SELECT AGGFN(DISTINCT a),    AGGFN(DISTINCT b)   ... => cannot use LIS
  etc.

  @param      join       the join to check
  @param[out] out_args   Collect the arguments of the aggregate functions
                         to a list. We don't worry about duplicates as
                         these will be sorted out later in
                         get_best_group_min_max.

  @return                does the query qualify for indexed AGGFN(DISTINCT)
    @retval   true       it does
    @retval   false      AGGFN(DISTINCT) must apply distinct in it.
*/

bool
is_indexed_agg_distinct(JOIN *join, List<Item_field> *out_args)
{
  Item_sum **sum_item_ptr;
  bool result= false;
  Field_map first_aggdistinct_fields;

  if (join->table_count != 1 ||                    /* reference more than 1 table */
      join->select_distinct ||                /* or a DISTINCT */
      join->select_lex->olap == ROLLUP_TYPE)  /* Check (B3) for ROLLUP */
    return false;

  if (join->make_sum_func_list(join->all_fields, join->fields_list, true))
    return false;

  for (sum_item_ptr= join->sum_funcs; *sum_item_ptr; sum_item_ptr++)
  {
    Item_sum *sum_item= *sum_item_ptr;
    Field_map cur_aggdistinct_fields;
    Item *expr;
    /* aggregate is not AGGFN(DISTINCT) or more than 1 argument to it */
    switch (sum_item->sum_func())
    {
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        continue;
      case Item_sum::COUNT_DISTINCT_FUNC: 
        break;
      case Item_sum::AVG_DISTINCT_FUNC:
      case Item_sum::SUM_DISTINCT_FUNC:
        if (sum_item->get_arg_count() == 1) 
          break;
        /* fall through */
      default: return false;
    }
    /*
      We arrive here for every COUNT(DISTINCT),AVG(DISTINCT) or SUM(DISTINCT).
      Collect the arguments of the aggregate functions to a list.
      We don't worry about duplicates as these will be sorted out later in 
      get_best_group_min_max 
    */
    for (uint i= 0; i < sum_item->get_arg_count(); i++)
    {
      expr= sum_item->get_arg(i);
      /* The AGGFN(DISTINCT) arg is not an attribute? */
      if (expr->real_item()->type() != Item::FIELD_ITEM)
        return false;

      Item_field* item= static_cast<Item_field*>(expr->real_item());
      if (out_args)
        out_args->push_back(item, join->thd->mem_root);

      cur_aggdistinct_fields.set_bit(item->field->field_index);
      result= true;
    }
    /*
      If there are multiple aggregate functions, make sure that they all
      refer to exactly the same set of columns.
    */
    if (first_aggdistinct_fields.is_clear_all())
      first_aggdistinct_fields.merge(cur_aggdistinct_fields);
    else if (first_aggdistinct_fields != cur_aggdistinct_fields)
      return false;
  }

  return result;
}


/**
  Discover the indexes that can be used for GROUP BY or DISTINCT queries.

  If the query has a GROUP BY clause, find all indexes that contain all
  GROUP BY fields, and add those indexes to join->const_keys.

  If the query has a DISTINCT clause, find all indexes that contain all
  SELECT fields, and add those indexes to join->const_keys.
  This allows later on such queries to be processed by a
  QUICK_GROUP_MIN_MAX_SELECT.

  @param join
  @param join_tab

  @return
    None
*/

static void
add_group_and_distinct_keys(JOIN *join, JOIN_TAB *join_tab)
{
  List<Item_field> indexed_fields;
  List_iterator<Item_field> indexed_fields_it(indexed_fields);
  ORDER      *cur_group;
  Item_field *cur_item;
  key_map possible_keys(0);

  if (join->group_list)
  { /* Collect all query fields referenced in the GROUP clause. */
    for (cur_group= join->group_list; cur_group; cur_group= cur_group->next)
      (*cur_group->item)->walk(&Item::collect_item_field_processor, 0,
                               &indexed_fields);
  }
  else if (join->select_distinct)
  { /* Collect all query fields referenced in the SELECT clause. */
    List<Item> &select_items= join->fields_list;
    List_iterator<Item> select_items_it(select_items);
    Item *item;
    while ((item= select_items_it++))
      item->walk(&Item::collect_item_field_processor, 0, &indexed_fields);
  }
  else if (join->tmp_table_param.sum_func_count &&
           is_indexed_agg_distinct(join, &indexed_fields))
  {
    join->sort_and_group= 1;
  }
  else
    return;

  if (indexed_fields.elements == 0)
    return;

  /* Intersect the keys of all group fields. */
  cur_item= indexed_fields_it++;
  possible_keys.merge(cur_item->field->part_of_key);
  while ((cur_item= indexed_fields_it++))
  {
    possible_keys.intersect(cur_item->field->part_of_key);
  }

  if (!possible_keys.is_clear_all())
    join_tab->const_keys.merge(possible_keys);
}


/*****************************************************************************
  Go through all combinations of not marked tables and find the one
  which uses least records
*****************************************************************************/

/** Save const tables first as used tables. */

void set_position(JOIN *join,uint idx,JOIN_TAB *table,KEYUSE *key)
{
  join->positions[idx].table= table;
  join->positions[idx].key=key;
  join->positions[idx].records_read=1.0;	/* This is a const table */
  join->positions[idx].cond_selectivity= 1.0;
  join->positions[idx].ref_depend_map= 0;

//  join->positions[idx].loosescan_key= MAX_KEY; /* Not a LooseScan */
  join->positions[idx].sj_strategy= SJ_OPT_NONE;
  join->positions[idx].use_join_buffer= FALSE;

  /* Move the const table as down as possible in best_ref */
  JOIN_TAB **pos=join->best_ref+idx+1;
  JOIN_TAB *next=join->best_ref[idx];
  for (;next != table ; pos++)
  {
    JOIN_TAB *tmp=pos[0];
    pos[0]=next;
    next=tmp;
  }
  join->best_ref[idx]=table;
  join->positions[idx].spl_plan= 0;
}


/*
  Estimate how many records we will get if we read just this table and apply
  a part of WHERE that can be checked for it.

  @detail
  Estimate how many records we will get if we
   - read the given table with its "independent" access method (either quick 
     select or full table/index scan),
   - apply the part of WHERE that refers only to this table.

  @seealso
    table_cond_selectivity() produces selectivity of condition that is checked
    after joining rows from this table to rows from preceding tables.
*/

inline
double matching_candidates_in_table(JOIN_TAB *s, bool with_found_constraint,
                                     uint use_cond_selectivity)
{
  ha_rows records;
  double dbl_records;

  if (use_cond_selectivity > 1)
  {
    TABLE *table= s->table;
    double sel= table->cond_selectivity;
    double table_records= (double)table->stat_records();
    dbl_records= table_records * sel;
    return dbl_records;
  }

  records = s->found_records;

  /*
    If there is a filtering condition on the table (i.e. ref analyzer found
    at least one "table.keyXpartY= exprZ", where exprZ refers only to tables
    preceding this table in the join order we're now considering), then 
    assume that 25% of the rows will be filtered out by this condition.

    This heuristic is supposed to force tables used in exprZ to be before
    this table in join order.
  */
  if (with_found_constraint)
    records-= records/4;

    /*
      If applicable, get a more accurate estimate. Don't use the two
      heuristics at once.
    */
  if (s->table->quick_condition_rows != s->found_records)
    records= s->table->quick_condition_rows;

  dbl_records= (double)records;
  return dbl_records;
}


/**
  Find the best access path for an extension of a partial execution
  plan and add this path to the plan.

  The function finds the best access path to table 's' from the passed
  partial plan where an access path is the general term for any means to
  access the data in 's'. An access path may use either an index or a scan,
  whichever is cheaper. The input partial plan is passed via the array
  'join->positions' of length 'idx'. The chosen access method for 's' and its
  cost are stored in 'join->positions[idx]'.

  @param join             pointer to the structure providing all context info
                          for the query
  @param s                the table to be joined by the function
  @param thd              thread for the connection that submitted the query
  @param remaining_tables set of tables not included into the partial plan yet
  @param idx              the length of the partial plan
  @param disable_jbuf     TRUE<=> Don't use join buffering
  @param record_count     estimate for the number of records returned by the
                          partial plan
  @param pos              OUT Table access plan
  @param loose_scan_pos   OUT Table plan that uses loosescan, or set cost to 
                              DBL_MAX if not possible.

  @return
    None
*/

void
best_access_path(JOIN      *join,
                 JOIN_TAB  *s,
                 table_map remaining_tables,
                 const POSITION *join_positions,
                 uint      idx,
                 bool      disable_jbuf,
                 double    record_count,
                 POSITION *pos,
                 POSITION *loose_scan_pos)
{
  THD *thd= join->thd;
  uint use_cond_selectivity= thd->variables.optimizer_use_condition_selectivity;
  KEYUSE *best_key=         0;
  uint best_max_key_part=   0;
  my_bool found_constraint= 0;
  double best=              DBL_MAX;
  double best_time=         DBL_MAX;
  double records=           DBL_MAX;
  table_map best_ref_depends_map= 0;
  double tmp;
  ha_rows rec;
  bool best_uses_jbuf= FALSE;
  MY_BITMAP *eq_join_set= &s->table->eq_join_set;
  KEYUSE *hj_start_key= 0;
  SplM_plan_info *spl_plan= 0;

  disable_jbuf= disable_jbuf || idx == join->const_tables;  

  Loose_scan_opt loose_scan_opt;
  DBUG_ENTER("best_access_path");
  
  bitmap_clear_all(eq_join_set);

  loose_scan_opt.init(join, s, remaining_tables);

  if (s->table->is_splittable())
    spl_plan= s->choose_best_splitting(record_count, remaining_tables);

  if (s->keyuse)
  {                                            /* Use key if possible */
    KEYUSE *keyuse;
    KEYUSE *start_key=0;
    TABLE *table= s->table;
    double best_records= DBL_MAX;
    uint max_key_part=0;

    /* Test how we can use keys */
    rec= s->records/MATCHING_ROWS_IN_OTHER_TABLE;  // Assumed records/key
    for (keyuse=s->keyuse ; keyuse->table == table ;)
    {
      KEY *keyinfo;
      ulong key_flags;
      uint key_parts;
      key_part_map found_part= 0;
      key_part_map notnull_part=0; // key parts which won't have NULL in lookup tuple.
      table_map found_ref= 0;
      uint key= keyuse->key;
      bool ft_key=  (keyuse->keypart == FT_KEYPART);
      /* Bitmap of keyparts where the ref access is over 'keypart=const': */
      key_part_map const_part= 0;
      /* The or-null keypart in ref-or-null access: */
      key_part_map ref_or_null_part= 0;
      if (is_hash_join_key_no(key))
      {
        /* 
          Hash join as any join employing join buffer can be used to join
          only those tables that are joined after the first non const table
	*/  
        if (!(remaining_tables & keyuse->used_tables) &&
            idx > join->const_tables)
        {
          if (!hj_start_key)
            hj_start_key= keyuse;
          bitmap_set_bit(eq_join_set, keyuse->keypart);
        }
        keyuse++;
        continue;
      }

      keyinfo= table->key_info+key;
      key_parts= table->actual_n_key_parts(keyinfo);
      key_flags= table->actual_key_flags(keyinfo);

      /* Calculate how many key segments of the current key we can use */
      start_key= keyuse;

      loose_scan_opt.next_ref_key();
      DBUG_PRINT("info", ("Considering ref access on key %s",
                          keyuse->table->key_info[keyuse->key].name.str));

      do /* For each keypart */
      {
        uint keypart= keyuse->keypart;
        table_map best_part_found_ref= 0;
        double best_prev_record_reads= DBL_MAX;
        
        do /* For each way to access the keypart */
        {
          /*
            if 1. expression doesn't refer to forward tables
               2. we won't get two ref-or-null's
          */
          if (!(remaining_tables & keyuse->used_tables) &&
              (!keyuse->validity_ref || *keyuse->validity_ref) &&
              s->access_from_tables_is_allowed(keyuse->used_tables,
                                               join->sjm_lookup_tables) &&
              !(ref_or_null_part && (keyuse->optimize &
                                     KEY_OPTIMIZE_REF_OR_NULL)))
          {
            found_part|= keyuse->keypart_map;
            if (!(keyuse->used_tables & ~join->const_table_map))
              const_part|= keyuse->keypart_map;

            if (!keyuse->val->maybe_null || keyuse->null_rejecting)
              notnull_part|=keyuse->keypart_map;

            double tmp2= prev_record_reads(join_positions, idx,
                                           (found_ref | keyuse->used_tables));
            if (tmp2 < best_prev_record_reads)
            {
              best_part_found_ref= keyuse->used_tables & ~join->const_table_map;
              best_prev_record_reads= tmp2;
            }
            if (rec > keyuse->ref_table_rows)
              rec= keyuse->ref_table_rows;
	    /*
	      If there is one 'key_column IS NULL' expression, we can
	      use this ref_or_null optimisation of this field
	    */
            if (keyuse->optimize & KEY_OPTIMIZE_REF_OR_NULL)
              ref_or_null_part |= keyuse->keypart_map;
          }
          loose_scan_opt.add_keyuse(remaining_tables, keyuse);
          keyuse++;
        } while (keyuse->table == table && keyuse->key == key &&
                 keyuse->keypart == keypart);
	found_ref|= best_part_found_ref;
      } while (keyuse->table == table && keyuse->key == key);

      /*
        Assume that that each key matches a proportional part of table.
      */
      if (!found_part && !ft_key && !loose_scan_opt.have_a_case())
        continue;                               // Nothing usable found

      if (rec < MATCHING_ROWS_IN_OTHER_TABLE)
        rec= MATCHING_ROWS_IN_OTHER_TABLE;      // Fix for small tables

      /*
        ft-keys require special treatment
      */
      if (ft_key)
      {
        /*
          Really, there should be records=0.0 (yes!)
          but 1.0 would be probably safer
        */
        tmp= prev_record_reads(join_positions, idx, found_ref);
        records= 1.0;
      }
      else
      {
        found_constraint= MY_TEST(found_part);
        loose_scan_opt.check_ref_access_part1(s, key, start_key, found_part);

        /* Check if we found full key */
        const key_part_map all_key_parts= PREV_BITS(uint, key_parts);
        if (found_part == all_key_parts && !ref_or_null_part)
        {                                         /* use eq key */
          max_key_part= (uint) ~0;
          /*
            If the index is a unique index (1), and
            - all its columns are not null (2), or
            - equalities we are using reject NULLs (3)
            then the estimate is rows=1.
          */
          if ((key_flags & (HA_NOSAME | HA_EXT_NOSAME)) &&   // (1)
              (!(key_flags & HA_NULL_PART_KEY) ||            //  (2)
               all_key_parts == notnull_part))               //  (3)
          {
            tmp = prev_record_reads(join_positions, idx, found_ref);
            records=1.0;
          }
          else
          {
            if (!found_ref)
            {                                     /* We found a const key */
              /*
                ReuseRangeEstimateForRef-1:
                We get here if we've found a ref(const) (c_i are constants):
                  "(keypart1=c1) AND ... AND (keypartN=cN)"   [ref_const_cond]
                
                If range optimizer was able to construct a "range" 
                access on this index, then its condition "quick_cond" was
                eqivalent to ref_const_cond (*), and we can re-use E(#rows)
                from the range optimizer.
                
                Proof of (*): By properties of range and ref optimizers 
                quick_cond will be equal or tighther than ref_const_cond. 
                ref_const_cond already covers "smallest" possible interval - 
                a singlepoint interval over all keyparts. Therefore, 
                quick_cond is equivalent to ref_const_cond (if it was an 
                empty interval we wouldn't have got here).
              */
              if (table->quick_keys.is_set(key))
                records= (double) table->quick_rows[key];
              else
              {
                /* quick_range couldn't use key! */
                records= (double) s->records/rec;
              }
            }
            else
            {
              if (!(records= keyinfo->actual_rec_per_key(key_parts-1)))
              {                                   /* Prefer longer keys */
                records=
                  ((double) s->records / (double) rec *
                   (1.0 +
                    ((double) (table->s->max_key_length-keyinfo->key_length) /
                     (double) table->s->max_key_length)));
                if (records < 2.0)
                  records=2.0;               /* Can't be as good as a unique */
              }
              /*
                ReuseRangeEstimateForRef-2:  We get here if we could not reuse
                E(#rows) from range optimizer. Make another try:
                
                If range optimizer produced E(#rows) for a prefix of the ref
                access we're considering, and that E(#rows) is lower then our
                current estimate, make an adjustment. The criteria of when we
                can make an adjustment is a special case of the criteria used
                in ReuseRangeEstimateForRef-3.
              */
              if (table->quick_keys.is_set(key) &&
                  (const_part &
                    (((key_part_map)1 << table->quick_key_parts[key])-1)) ==
                  (((key_part_map)1 << table->quick_key_parts[key])-1) &&
                  table->quick_n_ranges[key] == 1 &&
                  records > (double) table->quick_rows[key])
              {
                records= (double) table->quick_rows[key];
              }
            }
            /* Limit the number of matched rows */
            tmp= records;
            set_if_smaller(tmp, (double) thd->variables.max_seeks_for_key);
            if (table->covering_keys.is_set(key))
              tmp= table->file->keyread_time(key, 1, (ha_rows) tmp);
            else
              tmp= table->file->read_time(key, 1,
                                          (ha_rows) MY_MIN(tmp,s->worst_seeks));
            tmp= COST_MULT(tmp, record_count);
          }
        }
        else
        {
          /*
            Use as much key-parts as possible and a uniq key is better
            than a not unique key
            Set tmp to (previous record count) * (records / combination)
          */
          if ((found_part & 1) &&
              (!(table->file->index_flags(key, 0, 0) & HA_ONLY_WHOLE_INDEX) ||
               found_part == PREV_BITS(uint,keyinfo->user_defined_key_parts)))
          {
            max_key_part= max_part_bit(found_part);
            /*
              ReuseRangeEstimateForRef-3:
              We're now considering a ref[or_null] access via
              (t.keypart1=e1 AND ... AND t.keypartK=eK) [ OR  
              (same-as-above but with one cond replaced 
               with "t.keypart_i IS NULL")]  (**)
              
              Try re-using E(#rows) from "range" optimizer:
              We can do so if "range" optimizer used the same intervals as
              in (**). The intervals used by range optimizer may be not 
              available at this point (as "range" access might have choosen to
              create quick select over another index), so we can't compare
              them to (**). We'll make indirect judgements instead.
              The sufficient conditions for re-use are:
              (C1) All e_i in (**) are constants, i.e. found_ref==FALSE. (if
                   this is not satisfied we have no way to know which ranges
                   will be actually scanned by 'ref' until we execute the 
                   join)
              (C2) max #key parts in 'range' access == K == max_key_part (this
                   is apparently a necessary requirement)

              We also have a property that "range optimizer produces equal or 
              tighter set of scan intervals than ref(const) optimizer". Each
              of the intervals in (**) are "tightest possible" intervals when 
              one limits itself to using keyparts 1..K (which we do in #2).              
              From here it follows that range access used either one, or
              both of the (I1) and (I2) intervals:
              
               (t.keypart1=c1 AND ... AND t.keypartK=eK)  (I1) 
               (same-as-above but with one cond replaced  
                with "t.keypart_i IS NULL")               (I2)

              The remaining part is to exclude the situation where range
              optimizer used one interval while we're considering
              ref-or-null and looking for estimate for two intervals. This
              is done by last limitation:

              (C3) "range optimizer used (have ref_or_null?2:1) intervals"
            */
            if (table->quick_keys.is_set(key) && !found_ref &&          //(C1)
                table->quick_key_parts[key] == max_key_part &&          //(C2)
                table->quick_n_ranges[key] == 1 + MY_TEST(ref_or_null_part)) //(C3)
            {
              tmp= records= (double) table->quick_rows[key];
            }
            else
            {
              /* Check if we have statistic about the distribution */
              if ((records= keyinfo->actual_rec_per_key(max_key_part-1)))
              {
                /* 
                  Fix for the case where the index statistics is too
                  optimistic: If 
                  (1) We're considering ref(const) and there is quick select
                      on the same index, 
                  (2) and that quick select uses more keyparts (i.e. it will
                      scan equal/smaller interval then this ref(const))
                  (3) and E(#rows) for quick select is higher then our
                      estimate,
                  Then 
                    We'll use E(#rows) from quick select.

                  Q: Why do we choose to use 'ref'? Won't quick select be
                  cheaper in some cases ?
                  TODO: figure this out and adjust the plan choice if needed.
                */
                if (!found_ref && table->quick_keys.is_set(key) &&    // (1)
                    table->quick_key_parts[key] > max_key_part &&     // (2)
                    records < (double)table->quick_rows[key])         // (3)
                  records= (double)table->quick_rows[key];

                tmp= records;
              }
              else
              {
                /*
                  Assume that the first key part matches 1% of the file
                  and that the whole key matches 10 (duplicates) or 1
                  (unique) records.
                  Assume also that more key matches proportionally more
                  records
                  This gives the formula:
                  records = (x * (b-a) + a*c-b)/(c-1)

                  b = records matched by whole key
                  a = records matched by first key part (1% of all records?)
                  c = number of key parts in key
                  x = used key parts (1 <= x <= c)
                */
                double rec_per_key;
                if (!(rec_per_key=(double)
                      keyinfo->rec_per_key[keyinfo->user_defined_key_parts-1]))
                  rec_per_key=(double) s->records/rec+1;

                if (!s->records)
                  tmp = 0;
                else if (rec_per_key/(double) s->records >= 0.01)
                  tmp = rec_per_key;
                else
                {
                  double a=s->records*0.01;
                  if (keyinfo->user_defined_key_parts > 1)
                    tmp= (max_key_part * (rec_per_key - a) +
                          a*keyinfo->user_defined_key_parts - rec_per_key)/
                         (keyinfo->user_defined_key_parts-1);
                  else
                    tmp= a;
                  set_if_bigger(tmp,1.0);
                }
                records = (ulong) tmp;
              }

              if (ref_or_null_part)
              {
                /* We need to do two key searches to find key */
                tmp *= 2.0;
                records *= 2.0;
              }

              /*
                ReuseRangeEstimateForRef-4:  We get here if we could not reuse
                E(#rows) from range optimizer. Make another try:
                
                If range optimizer produced E(#rows) for a prefix of the ref 
                access we're considering, and that E(#rows) is lower then our
                current estimate, make the adjustment.

                The decision whether we can re-use the estimate from the range
                optimizer is the same as in ReuseRangeEstimateForRef-3,
                applied to first table->quick_key_parts[key] key parts.
              */
              if (table->quick_keys.is_set(key) &&
                  table->quick_key_parts[key] <= max_key_part &&
                  const_part &
                    ((key_part_map)1 << table->quick_key_parts[key]) &&
                  table->quick_n_ranges[key] == 1 + MY_TEST(ref_or_null_part &
                                                            const_part) &&
                  records > (double) table->quick_rows[key])
              {
                tmp= records= (double) table->quick_rows[key];
              }
            }

            /* Limit the number of matched rows */
            set_if_smaller(tmp, (double) thd->variables.max_seeks_for_key);
            if (table->covering_keys.is_set(key))
              tmp= table->file->keyread_time(key, 1, (ha_rows) tmp);
            else
              tmp= table->file->read_time(key, 1,
                                          (ha_rows) MY_MIN(tmp,s->worst_seeks));
            tmp= COST_MULT(tmp, record_count);
          }
          else
            tmp= best_time;                     // Do nothing
        }

        tmp= COST_ADD(tmp, s->startup_cost);
        loose_scan_opt.check_ref_access_part2(key, start_key, records, tmp,
                                              found_ref);
      } /* not ft_key */

      if (tmp + 0.0001 < best_time - records/(double) TIME_FOR_COMPARE)
      {
        best_time= COST_ADD(tmp, records/(double) TIME_FOR_COMPARE);
        best= tmp;
        best_records= records;
        best_key= start_key;
        best_max_key_part= max_key_part;
        best_ref_depends_map= found_ref;
      }
    } /* for each key */
    records= best_records;
  }

  /* 
    If there is no key to access the table, but there is an equi-join
    predicate connecting the table with the privious tables then we
    consider the possibility of using hash join.
    We need also to check that:
    (1) s is inner table of semi-join -> join cache is allowed for semijoins
    (2) s is inner table of outer join -> join cache is allowed for outer joins
  */  
  if (idx > join->const_tables && best_key == 0 &&
      (join->allowed_join_cache_types & JOIN_CACHE_HASHED_BIT) &&
      join->max_allowed_join_cache_level > 2 &&
     !bitmap_is_clear_all(eq_join_set) &&  !disable_jbuf &&
      (!s->emb_sj_nest ||                     
       join->allowed_semijoin_with_cache) &&    // (1)
      (!(s->table->map & join->outer_join) ||
       join->allowed_outer_join_with_cache))    // (2)
  {
    double join_sel= 0.1;
    /* Estimate the cost of  the hash join access to the table */
    double rnd_records= matching_candidates_in_table(s, found_constraint,
                                                     use_cond_selectivity);

    tmp= s->quick ? s->quick->read_time : s->scan_time();
    double cmp_time= (s->records - rnd_records)/(double) TIME_FOR_COMPARE;
    tmp= COST_ADD(tmp, cmp_time);

    /* We read the table as many times as join buffer becomes full. */

    double refills= (1.0 + floor((double) cache_record_length(join,idx) *
                           record_count /
			   (double) thd->variables.join_buff_size));
    tmp= COST_MULT(tmp, refills);
    best_time= COST_ADD(tmp,
                        COST_MULT((record_count*join_sel) / TIME_FOR_COMPARE,
                                  rnd_records));
    best= tmp;
    records= rnd_records;
    best_key= hj_start_key;
    best_ref_depends_map= 0;
    best_uses_jbuf= TRUE;
   }

  /*
    Don't test table scan if it can't be better.
    Prefer key lookup if we would use the same key for scanning.

    Don't do a table scan on InnoDB tables, if we can read the used
    parts of the row from any of the used index.
    This is because table scans uses index and we would not win
    anything by using a table scan.

    A word for word translation of the below if-statement in sergefp's
    understanding: we check if we should use table scan if:
    (1) The found 'ref' access produces more records than a table scan
        (or index scan, or quick select), or 'ref' is more expensive than
        any of them.
    (2) This doesn't hold: the best way to perform table scan is to to perform
        'range' access using index IDX, and the best way to perform 'ref' 
        access is to use the same index IDX, with the same or more key parts.
        (note: it is not clear how this rule is/should be extended to 
        index_merge quick selects). Also if we have a hash join we prefer that
        over a table scan
    (3) See above note about InnoDB.
    (4) NOT ("FORCE INDEX(...)" is used for table and there is 'ref' access
             path, but there is no quick select)
        If the condition in the above brackets holds, then the only possible
        "table scan" access method is ALL/index (there is no quick select).
        Since we have a 'ref' access path, and FORCE INDEX instructs us to
        choose it over ALL/index, there is no need to consider a full table
        scan.
    (5) Non-flattenable semi-joins: don't consider doing a scan of temporary
        table if we had an option to make lookups into it. In real-world cases,
        lookups are cheaper than full scans, but when the table is small, they
        can be [considered to be] more expensive, which causes lookups not to 
        be used for cases with small datasets, which is annoying.
  */
  if ((records >= s->found_records || best > s->read_time) &&            // (1)
      !(best_key && best_key->key == MAX_KEY) &&                         // (2)
      !(s->quick && best_key && s->quick->index == best_key->key &&      // (2)
        best_max_key_part >= s->table->quick_key_parts[best_key->key]) &&// (2)
      !((s->table->file->ha_table_flags() & HA_TABLE_SCAN_ON_INDEX) &&   // (3)
        ! s->table->covering_keys.is_clear_all() && best_key && !s->quick) &&// (3)
      !(s->table->force_index && best_key && !s->quick) &&               // (4)
      !(best_key && s->table->pos_in_table_list->jtbm_subselect))        // (5)
  {                                             // Check full join
    double rnd_records= matching_candidates_in_table(s, found_constraint,
                                                      use_cond_selectivity);

    /*
      Range optimizer never proposes a RANGE if it isn't better
      than FULL: so if RANGE is present, it's always preferred to FULL.
      Here we estimate its cost.
    */

    if (s->quick)
    {
      /*
        For each record we:
        - read record range through 'quick'
        - skip rows which does not satisfy WHERE constraints
        TODO: 
        We take into account possible use of join cache for ALL/index
        access (see first else-branch below), but we don't take it into 
        account here for range/index_merge access. Find out why this is so.
      */
      double cmp_time= (s->found_records - rnd_records)/(double) TIME_FOR_COMPARE;
      tmp= COST_MULT(record_count,
                     COST_ADD(s->quick->read_time, cmp_time));

      loose_scan_opt.check_range_access(join, idx, s->quick);
    }
    else
    {
      /* Estimate cost of reading table. */
      if (s->table->force_index && !best_key) // index scan
        tmp= s->table->file->read_time(s->ref.key, 1, s->records);
      else // table scan
        tmp= s->scan_time();

      if ((s->table->map & join->outer_join) || disable_jbuf)     // Can't use join cache
      {
        /*
          For each record we have to:
          - read the whole table record 
          - skip rows which does not satisfy join condition
        */
        double cmp_time= (s->records - rnd_records)/(double) TIME_FOR_COMPARE;
        tmp= COST_MULT(record_count, COST_ADD(tmp,cmp_time));
      }
      else
      {
        double refills= (1.0 + floor((double) cache_record_length(join,idx) *
                        (record_count /
                         (double) thd->variables.join_buff_size)));
        tmp= COST_MULT(tmp, refills);
        /* 
            We don't make full cartesian product between rows in the scanned
           table and existing records because we skip all rows from the
           scanned table, which does not satisfy join condition when 
           we read the table (see flush_cached_records for details). Here we
           take into account cost to read and skip these records.
        */
        double cmp_time= (s->records - rnd_records)/(double) TIME_FOR_COMPARE;
        tmp= COST_ADD(tmp, cmp_time);
      }
    }

    /* Splitting technique cannot be used with join cache */
    if (s->table->is_splittable())
      tmp+= s->table->get_materialization_cost();
    else
      tmp+= s->startup_cost;
    /*
      We estimate the cost of evaluating WHERE clause for found records
      as record_count * rnd_records / TIME_FOR_COMPARE. This cost plus
      tmp give us total cost of using TABLE SCAN
    */
    if (best == DBL_MAX ||
        COST_ADD(tmp, record_count/(double) TIME_FOR_COMPARE*rnd_records) <
         (best_key->is_for_hash_join() ? best_time :
          COST_ADD(best, record_count/(double) TIME_FOR_COMPARE*records)))
    {
      /*
        If the table has a range (s->quick is set) make_join_select()
        will ensure that this will be used
      */
      best= tmp;
      records= rnd_records;
      best_key= 0;
      /* range/index_merge/ALL/index access method are "independent", so: */
      best_ref_depends_map= 0;
      best_uses_jbuf= MY_TEST(!disable_jbuf && !((s->table->map &
                                                  join->outer_join)));
      spl_plan= 0;
    }
  }

  /* Update the cost information for the current partial plan */
  pos->records_read= records;
  pos->read_time=    best;
  pos->key=          best_key;
  pos->table=        s;
  pos->ref_depend_map= best_ref_depends_map;
  pos->loosescan_picker.loosescan_key= MAX_KEY;
  pos->use_join_buffer= best_uses_jbuf;
  pos->spl_plan= spl_plan;

  loose_scan_opt.save_to_position(s, loose_scan_pos);

  if (!best_key &&
      idx == join->const_tables &&
      s->table == join->sort_by_table &&
      join->unit->select_limit_cnt >= records)
    join->sort_by_table= (TABLE*) 1;  // Must use temporary table

  DBUG_VOID_RETURN;
}


/*
  Find JOIN_TAB's embedding (i.e, parent) subquery.
  - For merged semi-joins, tables inside the semi-join nest have their
    semi-join nest as parent.  We intentionally ignore results of table 
    pullout action here.
  - For non-merged semi-joins (JTBM tabs), the embedding subquery is the 
    JTBM join tab itself.
*/

static TABLE_LIST* get_emb_subq(JOIN_TAB *tab)
{
  TABLE_LIST *tlist= tab->table->pos_in_table_list;
  if (tlist->jtbm_subselect)
    return tlist;
  TABLE_LIST *embedding= tlist->embedding;
  if (!embedding || !embedding->sj_subq_pred)
    return NULL;
  return embedding;
}


/*
  Choose initial table order that "helps" semi-join optimizations.

  The idea is that we should start with the order that is the same as the one
  we would have had if we had semijoin=off:
  - Top-level tables go first
  - subquery tables are grouped together by the subquery they are in,
  - subquery tables are attached where the subquery predicate would have been
    attached if we had semi-join off.
  
  This function relies on join_tab_cmp()/join_tab_cmp_straight() to produce
  certain pre-liminary ordering, see compare_embedding_subqueries() for its
  description.
*/

static void choose_initial_table_order(JOIN *join)
{
  TABLE_LIST *emb_subq;
  JOIN_TAB **tab= join->best_ref + join->const_tables;
  JOIN_TAB **tabs_end= tab + join->table_count - join->const_tables;
  DBUG_ENTER("choose_initial_table_order");
  /* Find where the top-level JOIN_TABs end and subquery JOIN_TABs start */
  for (; tab != tabs_end; tab++)
  {
    if ((emb_subq= get_emb_subq(*tab)))
      break;
  }
  uint n_subquery_tabs= (uint)(tabs_end - tab);

  if (!n_subquery_tabs)
    DBUG_VOID_RETURN;

  /* Copy the subquery JOIN_TABs to a separate array */
  JOIN_TAB *subquery_tabs[MAX_TABLES];
  memcpy(subquery_tabs, tab, sizeof(JOIN_TAB*) * n_subquery_tabs);
  
  JOIN_TAB **last_top_level_tab= tab;
  JOIN_TAB **subq_tab= subquery_tabs;
  JOIN_TAB **subq_tabs_end= subquery_tabs + n_subquery_tabs;
  TABLE_LIST *cur_subq_nest= NULL;
  for (; subq_tab < subq_tabs_end; subq_tab++)
  {
    if (get_emb_subq(*subq_tab)!= cur_subq_nest)
    {
      /*
        Reached the part of subquery_tabs that covers tables in some subquery.
      */
      cur_subq_nest= get_emb_subq(*subq_tab);

      /* Determine how many tables the subquery has */
      JOIN_TAB **last_tab_for_subq;
      for (last_tab_for_subq= subq_tab;
           last_tab_for_subq < subq_tabs_end && 
           get_emb_subq(*last_tab_for_subq) == cur_subq_nest;
           last_tab_for_subq++) {}
      uint n_subquery_tables= (uint)(last_tab_for_subq - subq_tab);

      /* 
        Walk the original array and find where this subquery would have been
        attached to
      */
      table_map need_tables= cur_subq_nest->original_subq_pred_used_tables;
      need_tables &= ~(join->const_table_map | PSEUDO_TABLE_BITS);
      for (JOIN_TAB **top_level_tab= join->best_ref + join->const_tables;
           top_level_tab < last_top_level_tab;
           //top_level_tab < join->best_ref + join->table_count;
           top_level_tab++)
      {
        need_tables &= ~(*top_level_tab)->table->map;
        /* Check if this is the place where subquery should be attached */
        if (!need_tables)
        {
          /* Move away the top-level tables that are after top_level_tab */
          size_t top_tail_len= last_top_level_tab - top_level_tab - 1;
          memmove(top_level_tab + 1 + n_subquery_tables, top_level_tab + 1,
                  sizeof(JOIN_TAB*)*top_tail_len);
          last_top_level_tab += n_subquery_tables;
          memcpy(top_level_tab + 1, subq_tab, sizeof(JOIN_TAB*)*n_subquery_tables);
          break;
        }
      }
      DBUG_ASSERT(!need_tables);
      subq_tab += n_subquery_tables - 1;
    }
  }
  DBUG_VOID_RETURN;
}


/**
  Selects and invokes a search strategy for an optimal query plan.

  The function checks user-configurable parameters that control the search
  strategy for an optimal plan, selects the search method and then invokes
  it. Each specific optimization procedure stores the final optimal plan in
  the array 'join->best_positions', and the cost of the plan in
  'join->best_read'.

  @param join         pointer to the structure providing all context info for
                      the query
  @param join_tables  set of the tables in the query

  @retval
    FALSE       ok
  @retval
    TRUE        Fatal error
*/

bool
choose_plan(JOIN *join, table_map join_tables)
{
  uint search_depth= join->thd->variables.optimizer_search_depth;
  uint prune_level=  join->thd->variables.optimizer_prune_level;
  uint use_cond_selectivity= 
         join->thd->variables.optimizer_use_condition_selectivity;
  bool straight_join= MY_TEST(join->select_options & SELECT_STRAIGHT_JOIN);
  DBUG_ENTER("choose_plan");

  join->cur_embedding_map= 0;
  reset_nj_counters(join, join->join_list);
  qsort2_cmp jtab_sort_func;

  if (join->emb_sjm_nest)
  {
    /* We're optimizing semi-join materialization nest, so put the 
       tables from this semi-join as first
    */
    jtab_sort_func= join_tab_cmp_embedded_first;
  }
  else
  {
    /*
      if (SELECT_STRAIGHT_JOIN option is set)
        reorder tables so dependent tables come after tables they depend 
        on, otherwise keep tables in the order they were specified in the query 
      else
        Apply heuristic: pre-sort all access plans with respect to the number of
        records accessed.
    */
    jtab_sort_func= straight_join ? join_tab_cmp_straight : join_tab_cmp;
  }

  /*
    psergey-todo: if we're not optimizing an SJM nest, 
     - sort that outer tables are first, and each sjm nest follows
     - then, put each [sjm_table1, ... sjm_tableN] sub-array right where 
       WHERE clause pushdown would have put it.
  */
  my_qsort2(join->best_ref + join->const_tables,
            join->table_count - join->const_tables, sizeof(JOIN_TAB*),
            jtab_sort_func, (void*)join->emb_sjm_nest);

  if (!join->emb_sjm_nest)
  {
    choose_initial_table_order(join);
  }
  join->cur_sj_inner_tables= 0;

  if (straight_join)
  {
    optimize_straight_join(join, join_tables);
  }
  else
  {
    DBUG_ASSERT(search_depth <= MAX_TABLES + 1);
    if (search_depth == 0)
      /* Automatically determine a reasonable value for 'search_depth' */
      search_depth= determine_search_depth(join);
    if (greedy_search(join, join_tables, search_depth, prune_level,
                      use_cond_selectivity))
      DBUG_RETURN(TRUE);
  }

  /* 
    Store the cost of this query into a user variable
    Don't update last_query_cost for statements that are not "flat joins" :
    i.e. they have subqueries, unions or call stored procedures.
    TODO: calculate a correct cost for a query with subqueries and UNIONs.
  */
  if (join->thd->lex->is_single_level_stmt())
    join->thd->status_var.last_query_cost= join->best_read;
  DBUG_RETURN(FALSE);
}


/*
  Compare two join tabs based on the subqueries they are from.
   - top-level join tabs go first
   - then subqueries are ordered by their select_id (we're using this 
     criteria because we need a cross-platform, deterministic ordering)

  @return 
     0   -  equal
     -1  -  jt1 < jt2
     1   -  jt1 > jt2
*/

static int compare_embedding_subqueries(JOIN_TAB *jt1, JOIN_TAB *jt2)
{
  /* Determine if the first table is originally from a subquery */
  TABLE_LIST *tbl1= jt1->table->pos_in_table_list;
  uint tbl1_select_no;
  if (tbl1->jtbm_subselect)
  {
    tbl1_select_no= 
      tbl1->jtbm_subselect->unit->first_select()->select_number;
  }
  else if (tbl1->embedding && tbl1->embedding->sj_subq_pred)
  {
    tbl1_select_no= 
      tbl1->embedding->sj_subq_pred->unit->first_select()->select_number;
  }
  else
    tbl1_select_no= 1; /* Top-level */

  /* Same for the second table */
  TABLE_LIST *tbl2= jt2->table->pos_in_table_list;
  uint tbl2_select_no;
  if (tbl2->jtbm_subselect)
  {
    tbl2_select_no= 
      tbl2->jtbm_subselect->unit->first_select()->select_number;
  }
  else if (tbl2->embedding && tbl2->embedding->sj_subq_pred)
  {
    tbl2_select_no= 
      tbl2->embedding->sj_subq_pred->unit->first_select()->select_number;
  }
  else
    tbl2_select_no= 1; /* Top-level */

  /* 
    Put top-level tables in front. Tables from within subqueries must follow,
    grouped by their owner subquery. We don't care about the order that
    subquery groups are in, because choose_initial_table_order() will re-order
    the groups.
  */
  if (tbl1_select_no != tbl2_select_no)
    return tbl1_select_no > tbl2_select_no ? 1 : -1;
  return 0;
}


/**
  Compare two JOIN_TAB objects based on the number of accessed records.

  @param ptr1 pointer to first JOIN_TAB object
  @param ptr2 pointer to second JOIN_TAB object

  NOTES
    The order relation implemented by join_tab_cmp() is not transitive,
    i.e. it is possible to choose such a, b and c that (a < b) && (b < c)
    but (c < a). This implies that result of a sort using the relation
    implemented by join_tab_cmp() depends on the order in which
    elements are compared, i.e. the result is implementation-specific.
    Example:
      a: dependent = 0x0 table->map = 0x1 found_records = 3 ptr = 0x907e6b0
      b: dependent = 0x0 table->map = 0x2 found_records = 3 ptr = 0x907e838
      c: dependent = 0x6 table->map = 0x10 found_records = 2 ptr = 0x907ecd0

   As for subqueries, this function must produce order that can be fed to
   choose_initial_table_order().
     
  @retval
    1  if first is bigger
  @retval
    -1  if second is bigger
  @retval
    0  if equal
*/

static int
join_tab_cmp(const void *dummy, const void* ptr1, const void* ptr2)
{
  JOIN_TAB *jt1= *(JOIN_TAB**) ptr1;
  JOIN_TAB *jt2= *(JOIN_TAB**) ptr2;
  int cmp;

  if ((cmp= compare_embedding_subqueries(jt1, jt2)) != 0)
    return cmp;
  /*
    After that,
    take care about ordering imposed by LEFT JOIN constraints,
    possible [eq]ref accesses, and numbers of matching records in the table.
  */
  if (jt1->dependent & jt2->table->map)
    return 1;
  if (jt2->dependent & jt1->table->map)
    return -1;  
  if (jt1->found_records > jt2->found_records)
    return 1;
  if (jt1->found_records < jt2->found_records)
    return -1; 
  return jt1 > jt2 ? 1 : (jt1 < jt2 ? -1 : 0);
}


/**
  Same as join_tab_cmp, but for use with SELECT_STRAIGHT_JOIN.
*/

static int
join_tab_cmp_straight(const void *dummy, const void* ptr1, const void* ptr2)
{
  JOIN_TAB *jt1= *(JOIN_TAB**) ptr1;
  JOIN_TAB *jt2= *(JOIN_TAB**) ptr2;

  /*
    We don't do subquery flattening if the parent or child select has
    STRAIGHT_JOIN modifier. It is complicated to implement and the semantics
    is hardly useful.
  */
  DBUG_ASSERT(!jt1->emb_sj_nest);
  DBUG_ASSERT(!jt2->emb_sj_nest);

  int cmp;
  if ((cmp= compare_embedding_subqueries(jt1, jt2)) != 0)
    return cmp;

  if (jt1->dependent & jt2->table->map)
    return 1;
  if (jt2->dependent & jt1->table->map)
    return -1;
  return jt1 > jt2 ? 1 : (jt1 < jt2 ? -1 : 0);
}


/*
  Same as join_tab_cmp but tables from within the given semi-join nest go 
  first. Used when the optimizing semi-join materialization nests.
*/

static int
join_tab_cmp_embedded_first(const void *emb,  const void* ptr1, const void* ptr2)
{
  const TABLE_LIST *emb_nest= (TABLE_LIST*) emb;
  JOIN_TAB *jt1= *(JOIN_TAB**) ptr1;
  JOIN_TAB *jt2= *(JOIN_TAB**) ptr2;

  if (jt1->emb_sj_nest == emb_nest && jt2->emb_sj_nest != emb_nest)
    return -1;
  if (jt1->emb_sj_nest != emb_nest && jt2->emb_sj_nest == emb_nest)
    return 1;

  if (jt1->dependent & jt2->table->map)
    return 1;
  if (jt2->dependent & jt1->table->map)
    return -1;

  if (jt1->found_records > jt2->found_records)
    return 1;
  if (jt1->found_records < jt2->found_records)
    return -1; 
  
  return jt1 > jt2 ? 1 : (jt1 < jt2 ? -1 : 0);
}


/**
  Heuristic procedure to automatically guess a reasonable degree of
  exhaustiveness for the greedy search procedure.

  The procedure estimates the optimization time and selects a search depth
  big enough to result in a near-optimal QEP, that doesn't take too long to
  find. If the number of tables in the query exceeds some constant, then
  search_depth is set to this constant.

  @param join   pointer to the structure providing all context info for
                the query

  @note
    This is an extremely simplistic implementation that serves as a stub for a
    more advanced analysis of the join. Ideally the search depth should be
    determined by learning from previous query optimizations, because it will
    depend on the CPU power (and other factors).

  @todo
    this value should be determined dynamically, based on statistics:
    uint max_tables_for_exhaustive_opt= 7;

  @todo
    this value could be determined by some mapping of the form:
    depth : table_count -> [max_tables_for_exhaustive_opt..MAX_EXHAUSTIVE]

  @return
    A positive integer that specifies the search depth (and thus the
    exhaustiveness) of the depth-first search algorithm used by
    'greedy_search'.
*/

static uint
determine_search_depth(JOIN *join)
{
  uint table_count=  join->table_count - join->const_tables;
  uint search_depth;
  /* TODO: this value should be determined dynamically, based on statistics: */
  uint max_tables_for_exhaustive_opt= 7;

  if (table_count <= max_tables_for_exhaustive_opt)
    search_depth= table_count+1; // use exhaustive for small number of tables
  else
    /*
      TODO: this value could be determined by some mapping of the form:
      depth : table_count -> [max_tables_for_exhaustive_opt..MAX_EXHAUSTIVE]
    */
    search_depth= max_tables_for_exhaustive_opt; // use greedy search

  return search_depth;
}


/**
  Select the best ways to access the tables in a query without reordering them.

    Find the best access paths for each query table and compute their costs
    according to their order in the array 'join->best_ref' (thus without
    reordering the join tables). The function calls sequentially
    'best_access_path' for each table in the query to select the best table
    access method. The final optimal plan is stored in the array
    'join->best_positions', and the corresponding cost in 'join->best_read'.

  @param join          pointer to the structure providing all context info for
                       the query
  @param join_tables   set of the tables in the query

  @note
    This function can be applied to:
    - queries with STRAIGHT_JOIN
    - internally to compute the cost of an arbitrary QEP
  @par
    Thus 'optimize_straight_join' can be used at any stage of the query
    optimization process to finalize a QEP as it is.
*/

static void
optimize_straight_join(JOIN *join, table_map join_tables)
{
  JOIN_TAB *s;
  uint idx= join->const_tables;
  bool disable_jbuf= join->thd->variables.join_cache_level == 0;
  double    record_count= 1.0;
  double    read_time=    0.0;
  uint use_cond_selectivity= 
         join->thd->variables.optimizer_use_condition_selectivity;
  POSITION  loose_scan_pos;

  for (JOIN_TAB **pos= join->best_ref + idx ; (s= *pos) ; pos++)
  {
    /* Find the best access method from 's' to the current partial plan */
    best_access_path(join, s, join_tables, join->positions, idx,
                     disable_jbuf, record_count,
                     join->positions + idx, &loose_scan_pos);

    /* compute the cost of the new plan extended with 's' */
    record_count= COST_MULT(record_count, join->positions[idx].records_read);
    read_time= COST_ADD(read_time,
                        COST_ADD(join->positions[idx].read_time,
                                 record_count / (double) TIME_FOR_COMPARE));
    advance_sj_state(join, join_tables, idx, &record_count, &read_time,
                     &loose_scan_pos);

    join_tables&= ~(s->table->map);
    double pushdown_cond_selectivity= 1.0;
    if (use_cond_selectivity > 1)
      pushdown_cond_selectivity= table_cond_selectivity(join, idx, s,
                                                        join_tables);
    join->positions[idx].cond_selectivity= pushdown_cond_selectivity;
    ++idx;
  }

  if (join->sort_by_table &&
      join->sort_by_table != join->positions[join->const_tables].table->table)
    read_time+= record_count;  // We have to make a temp table
  memcpy((uchar*) join->best_positions, (uchar*) join->positions,
         sizeof(POSITION)*idx);
  join->join_record_count= record_count;
  join->best_read= read_time - 0.001;
}


/**
  Find a good, possibly optimal, query execution plan (QEP) by a greedy search.

    The search procedure uses a hybrid greedy/exhaustive search with controlled
    exhaustiveness. The search is performed in N = card(remaining_tables)
    steps. Each step evaluates how promising is each of the unoptimized tables,
    selects the most promising table, and extends the current partial QEP with
    that table.  Currenly the most 'promising' table is the one with least
    expensive extension.\

    There are two extreme cases:
    -# When (card(remaining_tables) < search_depth), the estimate finds the
    best complete continuation of the partial QEP. This continuation can be
    used directly as a result of the search.
    -# When (search_depth == 1) the 'best_extension_by_limited_search'
    consideres the extension of the current QEP with each of the remaining
    unoptimized tables.

    All other cases are in-between these two extremes. Thus the parameter
    'search_depth' controlls the exhaustiveness of the search. The higher the
    value, the longer the optimization time and possibly the better the
    resulting plan. The lower the value, the fewer alternative plans are
    estimated, but the more likely to get a bad QEP.

    All intermediate and final results of the procedure are stored in 'join':
    - join->positions     : modified for every partial QEP that is explored
    - join->best_positions: modified for the current best complete QEP
    - join->best_read     : modified for the current best complete QEP
    - join->best_ref      : might be partially reordered

    The final optimal plan is stored in 'join->best_positions', and its
    corresponding cost in 'join->best_read'.

  @note
    The following pseudocode describes the algorithm of 'greedy_search':

    @code
    procedure greedy_search
    input: remaining_tables
    output: pplan;
    {
      pplan = <>;
      do {
        (t, a) = best_extension(pplan, remaining_tables);
        pplan = concat(pplan, (t, a));
        remaining_tables = remaining_tables - t;
      } while (remaining_tables != {})
      return pplan;
    }

  @endcode
    where 'best_extension' is a placeholder for a procedure that selects the
    most "promising" of all tables in 'remaining_tables'.
    Currently this estimate is performed by calling
    'best_extension_by_limited_search' to evaluate all extensions of the
    current QEP of size 'search_depth', thus the complexity of 'greedy_search'
    mainly depends on that of 'best_extension_by_limited_search'.

  @par
    If 'best_extension()' == 'best_extension_by_limited_search()', then the
    worst-case complexity of this algorithm is <=
    O(N*N^search_depth/search_depth). When serch_depth >= N, then the
    complexity of greedy_search is O(N!).

  @par
    In the future, 'greedy_search' might be extended to support other
    implementations of 'best_extension', e.g. some simpler quadratic procedure.

  @param join             pointer to the structure providing all context info
                          for the query
  @param remaining_tables set of tables not included into the partial plan yet
  @param search_depth     controlls the exhaustiveness of the search
  @param prune_level      the pruning heuristics that should be applied during
                          search
  @param use_cond_selectivity  specifies how the selectivity of the conditions
                          pushed to a table should be taken into account

  @retval
    FALSE       ok
  @retval
    TRUE        Fatal error
*/

static bool
greedy_search(JOIN      *join,
              table_map remaining_tables,
              uint      search_depth,
              uint      prune_level,
              uint      use_cond_selectivity)
{
  double    record_count= 1.0;
  double    read_time=    0.0;
  uint      idx= join->const_tables; // index into 'join->best_ref'
  uint      best_idx;
  uint      size_remain;    // cardinality of remaining_tables
  POSITION  best_pos;
  JOIN_TAB  *best_table; // the next plan node to be added to the curr QEP
  // ==join->tables or # tables in the sj-mat nest we're optimizing
  uint      n_tables __attribute__((unused));
  DBUG_ENTER("greedy_search");

  /* number of tables that remain to be optimized */
  n_tables= size_remain= my_count_bits(remaining_tables &
                                       (join->emb_sjm_nest? 
                                         (join->emb_sjm_nest->sj_inner_tables &
                                          ~join->const_table_map)
                                         :
                                         ~(table_map)0));

  do {
    /* Find the extension of the current QEP with the lowest cost */
    join->best_read= DBL_MAX;
    if (best_extension_by_limited_search(join, remaining_tables, idx, record_count,
                                         read_time, search_depth, prune_level,
                                         use_cond_selectivity))
      DBUG_RETURN(TRUE);
    /*
      'best_read < DBL_MAX' means that optimizer managed to find
      some plan and updated 'best_positions' array accordingly.
    */
    DBUG_ASSERT(join->best_read < DBL_MAX);

    if (size_remain <= search_depth)
    {
      /*
        'join->best_positions' contains a complete optimal extension of the
        current partial QEP.
      */
      DBUG_EXECUTE("opt", print_plan(join, n_tables,
                                     record_count, read_time, read_time,
                                     "optimal"););
      DBUG_RETURN(FALSE);
    }

    /* select the first table in the optimal extension as most promising */
    best_pos= join->best_positions[idx];
    best_table= best_pos.table;
    /*
      Each subsequent loop of 'best_extension_by_limited_search' uses
      'join->positions' for cost estimates, therefore we have to update its
      value.
    */
    join->positions[idx]= best_pos;

    /*
      Update the interleaving state after extending the current partial plan
      with a new table.
      We are doing this here because best_extension_by_limited_search reverts
      the interleaving state to the one of the non-extended partial plan 
      on exit.
    */
    bool is_interleave_error __attribute__((unused))= 
      check_interleaving_with_nj (best_table);
    /* This has been already checked by best_extension_by_limited_search */
    DBUG_ASSERT(!is_interleave_error);


    /* find the position of 'best_table' in 'join->best_ref' */
    best_idx= idx;
    JOIN_TAB *pos= join->best_ref[best_idx];
    while (pos && best_table != pos)
      pos= join->best_ref[++best_idx];
    DBUG_ASSERT((pos != NULL)); // should always find 'best_table'
    /* move 'best_table' at the first free position in the array of joins */
    swap_variables(JOIN_TAB*, join->best_ref[idx], join->best_ref[best_idx]);

    /* compute the cost of the new plan extended with 'best_table' */
    record_count= COST_MULT(record_count, join->positions[idx].records_read);
    read_time= COST_ADD(read_time,
                         COST_ADD(join->positions[idx].read_time,
                                  record_count / (double) TIME_FOR_COMPARE));

    remaining_tables&= ~(best_table->table->map);
    --size_remain;
    ++idx;

    DBUG_EXECUTE("opt", print_plan(join, idx,
                                   record_count, read_time, read_time,
                                   "extended"););
  } while (TRUE);
}


/**
  Get cost of execution and fanout produced by selected tables in the join
  prefix (where prefix is defined as prefix in depth-first traversal)
 
  @param end_tab_idx               The number of last tab to be taken into
                                   account (in depth-first traversal prefix)
  @param filter_map                Bitmap of tables whose cost/fanout are to 
                                   be taken into account.
  @param read_time_arg     [out]   store read time here 
  @param record_count_arg  [out]   store record count here

  @note

  @returns
    read_time_arg and record_count_arg contain the computed cost and fanout
*/

void JOIN::get_partial_cost_and_fanout(int end_tab_idx,
                                       table_map filter_map,
                                       double *read_time_arg, 
                                       double *record_count_arg)
{
  double record_count= 1;
  double read_time= 0.0;
  double sj_inner_fanout= 1.0;
  JOIN_TAB *end_tab= NULL;
  JOIN_TAB *tab;
  int i;
  int last_sj_table= MAX_TABLES;

  /* 
    Handle a special case where the join is degenerate, and produces no
    records
  */
  if (table_count == const_tables)
  {
    *read_time_arg= 0.0;
    /*
      We return 1, because 
       - it is the pessimistic estimate (there might be grouping)
       - it's safer, as we're less likely to hit the edge cases in
         calculations.
    */
    *record_count_arg=1.0;
    return;
  }

  for (tab= first_depth_first_tab(this), i= const_tables;
       tab;
       tab= next_depth_first_tab(this, tab), i++)
  {
    end_tab= tab;
    if (i == end_tab_idx)
      break;
  }

  for (tab= first_depth_first_tab(this), i= const_tables;
       ;
       tab= next_depth_first_tab(this, tab), i++)
  {
    if (end_tab->bush_root_tab && end_tab->bush_root_tab == tab)
    {
      /* 
        We've entered the SJM nest that contains the end_tab. The caller is
        - interested in fanout inside the nest (because that's how many times 
          we'll invoke the attached WHERE conditions)
        - not interested in cost
      */
      record_count= 1.0;
      read_time= 0.0;
    }
    
    /* 
      Ignore fanout (but not cost) from sj-inner tables, as long as 
      the range that processes them finishes before the end_tab
    */
    if (tab->sj_strategy != SJ_OPT_NONE)
    {
      sj_inner_fanout= 1.0;
      last_sj_table= i + tab->n_sj_tables;
    }
    
    table_map cur_table_map;
    if (tab->table)
      cur_table_map= tab->table->map;
    else
    {
      /* This is a SJ-Materialization nest. Check all of its tables */
      TABLE *first_child= tab->bush_children->start->table;
      TABLE_LIST *sjm_nest= first_child->pos_in_table_list->embedding;
      cur_table_map= sjm_nest->nested_join->used_tables;
    }
    if (tab->records_read && (cur_table_map & filter_map))
    {
      record_count= COST_MULT(record_count, tab->records_read);
      read_time= COST_ADD(read_time,
                          COST_ADD(tab->read_time,
                                   record_count / (double) TIME_FOR_COMPARE));
      if (tab->emb_sj_nest)
        sj_inner_fanout= COST_MULT(sj_inner_fanout, tab->records_read);
				     }

    if (i == last_sj_table)
    {
      record_count /= sj_inner_fanout;
      sj_inner_fanout= 1.0;
      last_sj_table= MAX_TABLES;
    }

    if (tab == end_tab)
      break;
  }
  *read_time_arg= read_time;// + record_count / TIME_FOR_COMPARE;
  *record_count_arg= record_count;
}


/*
  Get prefix cost and fanout. This function is different from
  get_partial_cost_and_fanout:
   - it operates on a JOIN that haven't yet finished its optimization phase (in
     particular, fix_semijoin_strategies_for_picked_join_order() and
     get_best_combination() haven't been called)
   - it assumes the the join prefix doesn't have any semi-join plans

  These assumptions are met by the caller of the function.
*/

void JOIN::get_prefix_cost_and_fanout(uint n_tables, 
                                      double *read_time_arg,
                                      double *record_count_arg)
{
  double record_count= 1;
  double read_time= 0.0;
  for (uint i= const_tables; i < n_tables + const_tables ; i++)
  {
    if (best_positions[i].records_read)
    {
      record_count= COST_MULT(record_count, best_positions[i].records_read);
      read_time= COST_ADD(read_time, best_positions[i].read_time);
    }
    /* TODO: Take into account condition selectivities here */
  }
  *read_time_arg= read_time;// + record_count / TIME_FOR_COMPARE;
  *record_count_arg= record_count;
}


/**
  Estimate the number of rows that query execution will read.

  @todo This is a very pessimistic upper bound. Use join selectivity
  when available to produce a more realistic number.
*/

double JOIN::get_examined_rows()
{
  double examined_rows;
  double prev_fanout= 1;
  double records;
  JOIN_TAB *tab= first_breadth_first_tab();
  JOIN_TAB *prev_tab= tab;

  records= (double)tab->get_examined_rows();

  while ((tab= next_breadth_first_tab(first_breadth_first_tab(),
                                      top_join_tab_count, tab)))
  {
    prev_fanout= COST_MULT(prev_fanout, prev_tab->records_read);
    records=
      COST_ADD(records,
               COST_MULT((double) (tab->get_examined_rows()), prev_fanout));
    prev_tab= tab;
  }
  examined_rows= (double)
    (records > (double) HA_ROWS_MAX ? HA_ROWS_MAX : (ha_rows) records);
  return examined_rows;
}


/**
  @brief
  Get the selectivity of equalities between columns when joining a table

  @param join       The optimized join
  @param idx        The number of tables in the evaluated partual join
  @param s          The table to be joined for evaluation
  @param rem_tables The bitmap of tables to be joined later
  @param keyparts   The number of key parts to used when joining s
  @param ref_keyuse_steps Array of references to keyuses employed to join s 
*/

static 
double table_multi_eq_cond_selectivity(JOIN *join, uint idx, JOIN_TAB *s,
                                       table_map rem_tables, uint keyparts,
                                       uint16 *ref_keyuse_steps)
{
  double sel= 1.0;
  COND_EQUAL *cond_equal= join->cond_equal;

  if (!cond_equal || !cond_equal->current_level.elements)
    return sel;

   if (!s->keyuse)
    return sel;

  Item_equal *item_equal;
  List_iterator_fast<Item_equal> it(cond_equal->current_level);
  TABLE *table= s->table;
  table_map table_bit= table->map;
  POSITION *pos= &join->positions[idx];
  
  while ((item_equal= it++))
  { 
    /* 
      Check whether we need to take into account the selectivity of
      multiple equality item_equal. If this is the case multiply
      the current value of sel by this selectivity
    */
    table_map used_tables= item_equal->used_tables();
    if (!(used_tables & table_bit))
      continue;
    if (item_equal->get_const())
      continue;

    bool adjust_sel= FALSE;
    Item_equal_fields_iterator fi(*item_equal);
    while((fi++) && !adjust_sel)
    {
      Field *fld= fi.get_curr_field();
      if (fld->table->map != table_bit)
        continue;
      if (pos->key == 0)
        adjust_sel= TRUE;
      else
      {
        uint i;
        KEYUSE *keyuse= pos->key;
        uint key= keyuse->key;
        for (i= 0; i < keyparts; i++)
	{
          if (i > 0)
            keyuse+= ref_keyuse_steps[i-1];
          uint fldno;
          if (is_hash_join_key_no(key))
	    fldno= keyuse->keypart;
          else
            fldno= table->key_info[key].key_part[i].fieldnr - 1;        
          if (fld->field_index == fldno)
            break;
        }
        keyuse= pos->key;

        if (i == keyparts)
	{
          /* 
            Field fld is included in multiple equality item_equal
            and is not a part of the ref key.
            The selectivity of the multiple equality must be taken
            into account unless one of the ref arguments is
            equal to fld.  
	  */
          adjust_sel= TRUE;
          for (uint j= 0; j < keyparts && adjust_sel; j++)
	  {
            if (j > 0)
              keyuse+= ref_keyuse_steps[j-1];  
            Item *ref_item= keyuse->val;
	    if (ref_item->real_item()->type() == Item::FIELD_ITEM)
	    {
              Item_field *field_item= (Item_field *) (ref_item->real_item());
              if (item_equal->contains(field_item->field))
                adjust_sel= FALSE;              
	    }
          }
        }          
      }
    }
    if (adjust_sel)
    {
      /* 
        If ref == 0 and there are no fields in the multiple equality
        item_equal that belong to the tables joined prior to s
        then the selectivity of multiple equality will be set to 1.0.
      */
      double eq_fld_sel= 1.0;
      fi.rewind();
      while ((fi++))
      {
        double curr_eq_fld_sel;
        Field *fld= fi.get_curr_field();
        if (!(fld->table->map & ~(table_bit | rem_tables)))
          continue;
        curr_eq_fld_sel= get_column_avg_frequency(fld) /
                         fld->table->stat_records();
        if (curr_eq_fld_sel < 1.0)
          set_if_bigger(eq_fld_sel, curr_eq_fld_sel);
      }
      sel*= eq_fld_sel;
    }
  } 
  return sel;
}


/**
  @brief
    Get the selectivity of conditions when joining a table

  @param join       The optimized join
  @param s          The table to be joined for evaluation
  @param rem_tables The bitmap of tables to be joined later

  @detail
    Get selectivity of conditions that can be applied when joining this table
    with previous tables.

    For quick selects and full table scans, selectivity of COND(this_table)
    is accounted for in matching_candidates_in_table(). Here, we only count
    selectivity of COND(this_table, previous_tables). 

    For other access methods, we need to calculate selectivity of the whole
    condition, "COND(this_table) AND COND(this_table, previous_tables)".

  @retval
    selectivity of the conditions imposed on the rows of s
*/

static
double table_cond_selectivity(JOIN *join, uint idx, JOIN_TAB *s,
                              table_map rem_tables)
{
  uint16 ref_keyuse_steps_buf[MAX_REF_PARTS];
  uint   ref_keyuse_size= MAX_REF_PARTS;
  uint16 *ref_keyuse_steps= ref_keyuse_steps_buf;
  Field *field;
  TABLE *table= s->table;
  MY_BITMAP *read_set= table->read_set;
  double sel= s->table->cond_selectivity;
  POSITION *pos= &join->positions[idx];
  uint keyparts= 0;
  uint found_part_ref_or_null= 0;

  if (pos->key != 0)
  {
    /* 
      A ref access or hash join is used for this table. ref access is created
      from

        tbl.keypart1=expr1 AND tbl.keypart2=expr2 AND ...
      
      and it will only return rows for which this condition is satisified.
      Suppose, certain expr{i} is a constant. Since ref access only returns
      rows that satisfy
        
         tbl.keypart{i}=const       (*)

      then selectivity of this equality should not be counted in return value 
      of this function. This function uses the value of 
       
         table->cond_selectivity=selectivity(COND(tbl)) (**)
      
      as a starting point. This value includes selectivity of equality (*). We
      should somehow discount it. 
      
      Looking at calculate_cond_selectivity_for_table(), one can see that that
      the value is not necessarily a direct multiplicand in 
      table->cond_selectivity

      There are three possible ways to discount
      1. There is a potential range access on t.keypart{i}=const. 
         (an important special case: the used ref access has a const prefix for
          which a range estimate is available)
      
      2. The field has a histogram. field[x]->cond_selectivity has the data.
      
      3. Use index stats on this index:
         rec_per_key[key_part+1]/rec_per_key[key_part]

      (TODO: more details about the "t.key=othertable.col" case)
    */
    KEYUSE *keyuse= pos->key;
    KEYUSE *prev_ref_keyuse= keyuse;
    uint key= keyuse->key;
    bool used_range_selectivity= false;
    
    /*
      Check if we have a prefix of key=const that matches a quick select.
    */
    if (!is_hash_join_key_no(key) && table->quick_keys.is_set(key))
    {
      key_part_map quick_key_map= (key_part_map(1) << table->quick_key_parts[key]) - 1;
      if (table->quick_rows[key] && 
          !(quick_key_map & ~table->const_key_parts[key]))
      {
        /* 
          Ok, there is an equality for each of the key parts used by the
          quick select. This means, quick select's estimate can be reused to
          discount the selectivity of a prefix of a ref access.
        */
        for (; quick_key_map & 1 ; quick_key_map>>= 1)
        {
          while (keyuse->table == table && keyuse->key == key && 
                 keyuse->keypart == keyparts)
          {
            keyuse++;
          }
          keyparts++;
        }
        /*
          Here we discount selectivity of the constant range CR. To calculate
          this selectivity we use elements from the quick_rows[] array.
          If we have indexes i1,...,ik with the same prefix compatible
          with CR any of the estimate quick_rows[i1], ... quick_rows[ik] could
          be used for this calculation but here we don't know which one was
          actually used. So sel could be greater than 1 and we have to cap it.
          However if sel becomes greater than 2 then with high probability
          something went wrong.
	*/
        sel /= (double)table->quick_rows[key] / (double) table->stat_records();
        set_if_smaller(sel, 1.0);
        used_range_selectivity= true;
      }
    }
    
    /*
      Go through the "keypart{N}=..." equalities and find those that were
      already taken into account in table->cond_selectivity.
    */
    keyuse= pos->key;
    keyparts=0;
    while (keyuse->table == table && keyuse->key == key)
    {
      if (!(keyuse->used_tables & (rem_tables | table->map)))
      {
        if (are_tables_local(s, keyuse->val->used_tables()))
	{
          if (is_hash_join_key_no(key))
	  {
            if (keyparts == keyuse->keypart)
              keyparts++;
          }
          else
	  {
            if (keyparts == keyuse->keypart &&
                !((keyuse->val->used_tables()) & ~pos->ref_depend_map) &&
                !(found_part_ref_or_null & keyuse->optimize))
	    {
              /* Found a KEYUSE object that will be used by ref access */
              keyparts++;
              found_part_ref_or_null|= keyuse->optimize & ~KEY_OPTIMIZE_EQ;
            }
          }

          if (keyparts > keyuse->keypart)
	  {
            /* Ok this is the keyuse that will be used for ref access */
            if (!used_range_selectivity && keyuse->val->const_item())
            { 
              uint fldno;
              if (is_hash_join_key_no(key))
                fldno= keyuse->keypart;
              else
                fldno= table->key_info[key].key_part[keyparts-1].fieldnr - 1;

              if (table->field[fldno]->cond_selectivity > 0)
	      {            
                sel /= table->field[fldno]->cond_selectivity;
                set_if_smaller(sel, 1.0);
              }
              /* 
               TODO: we could do better here:
                 1. cond_selectivity might be =1 (the default) because quick 
                    select on some index prevented us from analyzing 
                    histogram for this column.
                 2. we could get an estimate through this?
                     rec_per_key[key_part-1] / rec_per_key[key_part]
              */
            }
            if (keyparts > 1)
	    {
              /*
                Prepare to set ref_keyuse_steps[keyparts-2]: resize the array
                if it is not large enough
              */
              if (keyparts - 2 >= ref_keyuse_size)
              {
                uint new_size= MY_MAX(ref_keyuse_size*2, keyparts);
                void *new_buf;
                if (!(new_buf= my_malloc(sizeof(*ref_keyuse_steps)*new_size,
                                         MYF(0))))
                {
                  sel= 1.0; // As if no selectivity was computed
                  goto exit;
                }
                memcpy(new_buf, ref_keyuse_steps,
                       sizeof(*ref_keyuse_steps)*ref_keyuse_size);
                if (ref_keyuse_steps != ref_keyuse_steps_buf)
                  my_free(ref_keyuse_steps);

                ref_keyuse_steps= (uint16*)new_buf;
                ref_keyuse_size= new_size;
              }

              ref_keyuse_steps[keyparts-2]= (uint16)(keyuse - prev_ref_keyuse);
              prev_ref_keyuse= keyuse;
            }
          }
	}
      }
      keyuse++;
    }
  }
  else
  {
    /*
      The table is accessed with full table scan, or quick select.
      Selectivity of COND(table) is already accounted for in 
      matching_candidates_in_table().
    */
    sel= 1;
  }

  /* 
    If the field f from the table is equal to a field from one the
    earlier joined tables then the selectivity of the range conditions
    over the field f must be discounted.

    We need to discount selectivity only if we're using ref-based 
    access method (and have sel!=1).
    If we use ALL/range/index_merge, then sel==1, and no need to discount.
  */
  if (pos->key != NULL)
  {
    for (Field **f_ptr=table->field ; (field= *f_ptr) ; f_ptr++)
    {
      if (!bitmap_is_set(read_set, field->field_index) ||
          !field->next_equal_field)
        continue; 
      for (Field *next_field= field->next_equal_field; 
           next_field != field; 
           next_field= next_field->next_equal_field)
      {
        if (!(next_field->table->map & rem_tables) && next_field->table != table)
        { 
          if (field->cond_selectivity > 0)
	  {
            sel/= field->cond_selectivity;
            set_if_smaller(sel, 1.0);
          }
          break;
        }
      }
    }
  }

  sel*= table_multi_eq_cond_selectivity(join, idx, s, rem_tables,
                                        keyparts, ref_keyuse_steps);
exit:
  if (ref_keyuse_steps != ref_keyuse_steps_buf)
    my_free(ref_keyuse_steps);
  return sel;
}


/**
  Find a good, possibly optimal, query execution plan (QEP) by a possibly
  exhaustive search.

    The procedure searches for the optimal ordering of the query tables in set
    'remaining_tables' of size N, and the corresponding optimal access paths to
    each table. The choice of a table order and an access path for each table
    constitutes a query execution plan (QEP) that fully specifies how to
    execute the query.
   
    The maximal size of the found plan is controlled by the parameter
    'search_depth'. When search_depth == N, the resulting plan is complete and
    can be used directly as a QEP. If search_depth < N, the found plan consists
    of only some of the query tables. Such "partial" optimal plans are useful
    only as input to query optimization procedures, and cannot be used directly
    to execute a query.

    The algorithm begins with an empty partial plan stored in 'join->positions'
    and a set of N tables - 'remaining_tables'. Each step of the algorithm
    evaluates the cost of the partial plan extended by all access plans for
    each of the relations in 'remaining_tables', expands the current partial
    plan with the access plan that results in lowest cost of the expanded
    partial plan, and removes the corresponding relation from
    'remaining_tables'. The algorithm continues until it either constructs a
    complete optimal plan, or constructs an optimal plartial plan with size =
    search_depth.

    The final optimal plan is stored in 'join->best_positions'. The
    corresponding cost of the optimal plan is in 'join->best_read'.

  @note
    The procedure uses a recursive depth-first search where the depth of the
    recursion (and thus the exhaustiveness of the search) is controlled by the
    parameter 'search_depth'.

  @note
    The pseudocode below describes the algorithm of
    'best_extension_by_limited_search'. The worst-case complexity of this
    algorithm is O(N*N^search_depth/search_depth). When serch_depth >= N, then
    the complexity of greedy_search is O(N!).

    @code
    procedure best_extension_by_limited_search(
      pplan in,             // in, partial plan of tables-joined-so-far
      pplan_cost,           // in, cost of pplan
      remaining_tables,     // in, set of tables not referenced in pplan
      best_plan_so_far,     // in/out, best plan found so far
      best_plan_so_far_cost,// in/out, cost of best_plan_so_far
      search_depth)         // in, maximum size of the plans being considered
    {
      for each table T from remaining_tables
      {
        // Calculate the cost of using table T as above
        cost = complex-series-of-calculations;

        // Add the cost to the cost so far.
        pplan_cost+= cost;

        if (pplan_cost >= best_plan_so_far_cost)
          // pplan_cost already too great, stop search
          continue;

        pplan= expand pplan by best_access_method;
        remaining_tables= remaining_tables - table T;
        if (remaining_tables is not an empty set
            and
            search_depth > 1)
        {
          best_extension_by_limited_search(pplan, pplan_cost,
                                           remaining_tables,
                                           best_plan_so_far,
                                           best_plan_so_far_cost,
                                           search_depth - 1);
        }
        else
        {
          best_plan_so_far_cost= pplan_cost;
          best_plan_so_far= pplan;
        }
      }
    }
    @endcode

  @note
    When 'best_extension_by_limited_search' is called for the first time,
    'join->best_read' must be set to the largest possible value (e.g. DBL_MAX).
    The actual implementation provides a way to optionally use pruning
    heuristic (controlled by the parameter 'prune_level') to reduce the search
    space by skipping some partial plans.

  @note
    The parameter 'search_depth' provides control over the recursion
    depth, and thus the size of the resulting optimal plan.

  @param join             pointer to the structure providing all context info
                          for the query
  @param remaining_tables set of tables not included into the partial plan yet
  @param idx              length of the partial QEP in 'join->positions';
                          since a depth-first search is used, also corresponds
                          to the current depth of the search tree;
                          also an index in the array 'join->best_ref';
  @param record_count     estimate for the number of records returned by the
                          best partial plan
  @param read_time        the cost of the best partial plan
  @param search_depth     maximum depth of the recursion and thus size of the
                          found optimal plan
                          (0 < search_depth <= join->tables+1).
  @param prune_level      pruning heuristics that should be applied during
                          optimization
                          (values: 0 = EXHAUSTIVE, 1 = PRUNE_BY_TIME_OR_ROWS)
  @param use_cond_selectivity  specifies how the selectivity of the conditions
                          pushed to a table should be taken into account

  @retval
    FALSE       ok
  @retval
    TRUE        Fatal error
*/

static bool
best_extension_by_limited_search(JOIN      *join,
                                 table_map remaining_tables,
                                 uint      idx,
                                 double    record_count,
                                 double    read_time,
                                 uint      search_depth,
                                 uint      prune_level,
                                 uint      use_cond_selectivity)
{
  DBUG_ENTER("best_extension_by_limited_search");

  THD *thd= join->thd;

  DBUG_EXECUTE_IF("show_explain_probe_best_ext_lim_search", 
                  if (dbug_user_var_equals_int(thd, 
                                               "show_explain_probe_select_id", 
                                               join->select_lex->select_number))
                        dbug_serve_apcs(thd, 1);
                 );

  if (unlikely(thd->check_killed()))  // Abort
    DBUG_RETURN(TRUE);

  DBUG_EXECUTE("opt", print_plan(join, idx, read_time, record_count, idx,
                                 "SOFAR:"););

  /* 
     'join' is a partial plan with lower cost than the best plan so far,
     so continue expanding it further with the tables in 'remaining_tables'.
  */
  JOIN_TAB *s;
  double best_record_count= DBL_MAX;
  double best_read_time=    DBL_MAX;
  bool disable_jbuf= join->thd->variables.join_cache_level == 0;

  DBUG_EXECUTE("opt", print_plan(join, idx, record_count, read_time, read_time,
                                "part_plan"););

  /* 
    If we are searching for the execution plan of a materialized semi-join nest
    then allowed_tables contains bits only for the tables from this nest.
  */
  table_map allowed_tables= ~(table_map)0;
  if (join->emb_sjm_nest)
    allowed_tables= join->emb_sjm_nest->sj_inner_tables & ~join->const_table_map;

  for (JOIN_TAB **pos= join->best_ref + idx ; (s= *pos) ; pos++)
  {
    table_map real_table_bit= s->table->map;
    if ((remaining_tables & real_table_bit) && 
        (allowed_tables & real_table_bit) &&
        !(remaining_tables & s->dependent) && 
        (!idx || !check_interleaving_with_nj(s)))
    {
      double current_record_count, current_read_time;
      POSITION *position= join->positions + idx;

      /* Find the best access method from 's' to the current partial plan */
      POSITION loose_scan_pos;
      best_access_path(join, s, remaining_tables, join->positions, idx,
                       disable_jbuf, record_count, position, &loose_scan_pos);

      /* Compute the cost of extending the plan with 's' */
      current_record_count= COST_MULT(record_count, position->records_read);
      current_read_time=COST_ADD(read_time,
                                 COST_ADD(position->read_time,
                                          current_record_count /
                                          (double) TIME_FOR_COMPARE));

      advance_sj_state(join, remaining_tables, idx, &current_record_count,
                       &current_read_time, &loose_scan_pos);

      /* Expand only partial plans with lower cost than the best QEP so far */
      if (current_read_time >= join->best_read)
      {
        DBUG_EXECUTE("opt", print_plan(join, idx+1,
                                       current_record_count,
                                       read_time,
                                       current_read_time,
                                       "prune_by_cost"););
        restore_prev_nj_state(s);
        restore_prev_sj_state(remaining_tables, s, idx);
        continue;
      }

      /*
        Prune some less promising partial plans. This heuristic may miss
        the optimal QEPs, thus it results in a non-exhaustive search.
      */
      if (prune_level == 1)
      {
        if (best_record_count > current_record_count ||
            best_read_time > current_read_time ||
            (idx == join->const_tables &&  // 's' is the first table in the QEP
            s->table == join->sort_by_table))
        {
          if (best_record_count >= current_record_count &&
              best_read_time >= current_read_time &&
              /* TODO: What is the reasoning behind this condition? */
              (!(s->key_dependent & allowed_tables & remaining_tables) ||
               join->positions[idx].records_read < 2.0))
          {
            best_record_count= current_record_count;
            best_read_time=    current_read_time;
          }
        }
        else
        {
          DBUG_EXECUTE("opt", print_plan(join, idx+1,
                                         current_record_count,
                                         read_time,
                                         current_read_time,
                                         "pruned_by_heuristic"););
          restore_prev_nj_state(s);
          restore_prev_sj_state(remaining_tables, s, idx);
          continue;
        }
      }

      double pushdown_cond_selectivity= 1.0;
      if (use_cond_selectivity > 1)
        pushdown_cond_selectivity= table_cond_selectivity(join, idx, s,
				                          remaining_tables &
                                                          ~real_table_bit);
      join->positions[idx].cond_selectivity= pushdown_cond_selectivity;
      double partial_join_cardinality= current_record_count *
                                        pushdown_cond_selectivity;
      if ( (search_depth > 1) && (remaining_tables & ~real_table_bit) & allowed_tables )
      { /* Recursively expand the current partial plan */
        swap_variables(JOIN_TAB*, join->best_ref[idx], *pos);
        if (best_extension_by_limited_search(join,
                                             remaining_tables & ~real_table_bit,
                                             idx + 1,
                                             partial_join_cardinality,
                                             current_read_time,
                                             search_depth - 1,
                                             prune_level,
                                             use_cond_selectivity))
          DBUG_RETURN(TRUE);
        swap_variables(JOIN_TAB*, join->best_ref[idx], *pos);
      }
      else
      { /*
          'join' is either the best partial QEP with 'search_depth' relations,
          or the best complete QEP so far, whichever is smaller.
        */
        if (join->sort_by_table &&
            join->sort_by_table !=
            join->positions[join->const_tables].table->table)
          /*
             We may have to make a temp table, note that this is only a
             heuristic since we cannot know for sure at this point.
             Hence it may be wrong.
          */
          current_read_time= COST_ADD(current_read_time, current_record_count);
        if (current_read_time < join->best_read)
        {
          memcpy((uchar*) join->best_positions, (uchar*) join->positions,
                 sizeof(POSITION) * (idx + 1));
          join->join_record_count= partial_join_cardinality;
          join->best_read= current_read_time - 0.001;
        }
        DBUG_EXECUTE("opt", print_plan(join, idx+1,
                                       current_record_count,
                                       read_time,
                                       current_read_time,
                                       "full_plan"););
      }
      restore_prev_nj_state(s);
      restore_prev_sj_state(remaining_tables, s, idx);
    }
  }
  DBUG_RETURN(FALSE);
}


/**
  Find how much space the prevous read not const tables takes in cache.
*/

void JOIN_TAB::calc_used_field_length(bool max_fl)
{
  uint null_fields,blobs,fields;
  ulong rec_length;
  Field **f_ptr,*field;
  uint uneven_bit_fields;
  MY_BITMAP *read_set= table->read_set;

  uneven_bit_fields= null_fields= blobs= fields= rec_length=0;
  for (f_ptr=table->field ; (field= *f_ptr) ; f_ptr++)
  {
    if (bitmap_is_set(read_set, field->field_index))
    {
      uint flags=field->flags;
      fields++;
      rec_length+=field->pack_length();
      if (flags & BLOB_FLAG)
	blobs++;
      if (!(flags & NOT_NULL_FLAG))
	null_fields++;
      if (field->type() == MYSQL_TYPE_BIT &&
          ((Field_bit*)field)->bit_len)
        uneven_bit_fields++;
    }
  }
  if (null_fields || uneven_bit_fields)
    rec_length+=(table->s->null_fields+7)/8;
  if (table->maybe_null)
    rec_length+=sizeof(my_bool);

  /* Take into account that DuplicateElimination may need to store rowid */
  uint rowid_add_size= 0;
  if (keep_current_rowid)
  {
    rowid_add_size= table->file->ref_length; 
    rec_length += rowid_add_size;
    fields++;
  }

  if (max_fl)
  {
    // TODO: to improve this estimate for max expected length 
    if (blobs)
    {
      ulong blob_length= table->file->stats.mean_rec_length;
      if (ULONG_MAX - rec_length > blob_length)
        rec_length+=  blob_length;
      else
        rec_length= ULONG_MAX;
    }
    max_used_fieldlength= rec_length;
  } 
  else if (table->file->stats.mean_rec_length)
    set_if_smaller(rec_length, table->file->stats.mean_rec_length + rowid_add_size);
      
  used_fields=fields;
  used_fieldlength=rec_length;
  used_blobs=blobs;
  used_null_fields= null_fields;
  used_uneven_bit_fields= uneven_bit_fields;
}


/* 
  @brief
  Extract pushdown conditions for a table scan

  @details
  This functions extracts pushdown conditions usable when this table is scanned.
  The conditions are extracted either from WHERE or from ON expressions.
  The conditions are attached to the field cache_select of this table.

  @note 
  Currently the extracted conditions are used only by BNL and BNLH join.
  algorithms.
 
  @retval  0   on success
           1   otherwise
*/ 

int JOIN_TAB::make_scan_filter()
{
  COND *tmp;
  DBUG_ENTER("make_scan_filter");

  Item *cond= is_inner_table_of_outer_join() ?
                *get_first_inner_table()->on_expr_ref : join->conds;
  
  if (cond &&
      (tmp= make_cond_for_table(join->thd, cond,
                               join->const_table_map | table->map,
			       table->map, -1, FALSE, TRUE)))
  {
     DBUG_EXECUTE("where",print_where(tmp,"cache", QT_ORDINARY););
     if (!(cache_select=
          (SQL_SELECT*) join->thd->memdup((uchar*) select, sizeof(SQL_SELECT))))
	DBUG_RETURN(1);
     cache_select->cond= tmp;
     cache_select->read_tables=join->const_table_map;
  }
  DBUG_RETURN(0);
}


/**
  @brief
  Check whether hash join algorithm can be used to join this table   

  @details
  This function finds out whether the ref items that have been chosen
  by the planner to access this table can be used for hash join algorithms.
  The answer depends on a certain property of the the fields of the
  joined tables on which the hash join key is built.
  
  @note
  At present the function is supposed to be called only after the function
  get_best_combination has been called.

  @retval TRUE    it's possible to use hash join to join this table
  @retval FALSE   otherwise
*/

bool JOIN_TAB::hash_join_is_possible()
{
  if (type != JT_REF && type != JT_EQ_REF)
    return FALSE;
  if (!is_ref_for_hash_join())
  {
    KEY *keyinfo= table->key_info + ref.key;
    return keyinfo->key_part[0].field->hash_join_is_possible();
  }
  return TRUE;
}


/**
  @brief
  Check whether a KEYUSE can be really used for access this join table 

  @param join    Join structure with the best join order 
                 for which the check is performed
  @param keyuse  Evaluated KEYUSE structure    

  @details
  This function is supposed to be used after the best execution plan have been
  already chosen and the JOIN_TAB array for the best join order been already set.
  For a given KEYUSE to access this JOIN_TAB in the best execution plan the
  function checks whether it really can be used. The function first performs
  the check with access_from_tables_is_allowed(). If it succeeds it checks
  whether the keyuse->val does not use some fields of a materialized semijoin
  nest that cannot be used to build keys to access outer tables.
  Such KEYUSEs exists for the query like this:
    select * from ot 
    where ot.c in (select it1.c from it1, it2 where it1.c=f(it2.c))
  Here we have two KEYUSEs to access table ot: with val=it1.c and val=f(it2.c).
  However if the subquery was materialized the second KEYUSE cannot be employed
  to access ot.

  @retval true  the given keyuse can be used for ref access of this JOIN_TAB 
  @retval false otherwise
*/

bool JOIN_TAB::keyuse_is_valid_for_access_in_chosen_plan(JOIN *join,
                                                         KEYUSE *keyuse)
{
  if (!access_from_tables_is_allowed(keyuse->used_tables, 
                                     join->sjm_lookup_tables))
    return false;
  if (join->sjm_scan_tables & table->map)
    return true;
  table_map keyuse_sjm_scan_tables= keyuse->used_tables &
                                    join->sjm_scan_tables;
  if (!keyuse_sjm_scan_tables)
    return true;
  uint sjm_tab_nr= 0;
  while (!(keyuse_sjm_scan_tables & table_map(1) << sjm_tab_nr))
    sjm_tab_nr++;
  JOIN_TAB *sjm_tab= join->map2table[sjm_tab_nr];
  TABLE_LIST *emb_sj_nest= sjm_tab->emb_sj_nest;    
  if (!(emb_sj_nest->sj_mat_info && emb_sj_nest->sj_mat_info->is_used &&
        emb_sj_nest->sj_mat_info->is_sj_scan))
    return true;
  st_select_lex *sjm_sel= emb_sj_nest->sj_subq_pred->unit->first_select(); 
  for (uint i= 0; i < sjm_sel->item_list.elements; i++)
  {
    DBUG_ASSERT(sjm_sel->ref_pointer_array[i]->real_item()->type() == Item::FIELD_ITEM);
    if (keyuse->val->real_item()->type() == Item::FIELD_ITEM)
    {
      Field *field = ((Item_field*)sjm_sel->ref_pointer_array[i]->real_item())->field;
      if (field->eq(((Item_field*)keyuse->val->real_item())->field))
        return true;
    }
  }
  return false; 
}


static uint
cache_record_length(JOIN *join,uint idx)
{
  uint length=0;
  JOIN_TAB **pos,**end;

  for (pos=join->best_ref+join->const_tables,end=join->best_ref+idx ;
       pos != end ;
       pos++)
  {
    JOIN_TAB *join_tab= *pos;
    length+= join_tab->get_used_fieldlength();
  }
  return length;
}


/*
  Get the number of different row combinations for subset of partial join

  SYNOPSIS
    prev_record_reads()
      join       The join structure
      idx        Number of tables in the partial join order (i.e. the
                 partial join order is in join->positions[0..idx-1])
      found_ref  Bitmap of tables for which we need to find # of distinct
                 row combinations.

  DESCRIPTION
    Given a partial join order (in join->positions[0..idx-1]) and a subset of
    tables within that join order (specified in found_ref), find out how many
    distinct row combinations of subset tables will be in the result of the
    partial join order.
     
    This is used as follows: Suppose we have a table accessed with a ref-based
    method. The ref access depends on current rows of tables in found_ref.
    We want to count # of different ref accesses. We assume two ref accesses
    will be different if at least one of access parameters is different.
    Example: consider a query

    SELECT * FROM t1, t2, t3 WHERE t1.key=c1 AND t2.key=c2 AND t3.key=t1.field

    and a join order:
      t1,  ref access on t1.key=c1
      t2,  ref access on t2.key=c2       
      t3,  ref access on t3.key=t1.field 
    
    For t1: n_ref_scans = 1, n_distinct_ref_scans = 1
    For t2: n_ref_scans = records_read(t1), n_distinct_ref_scans=1
    For t3: n_ref_scans = records_read(t1)*records_read(t2)
            n_distinct_ref_scans = #records_read(t1)
    
    The reason for having this function (at least the latest version of it)
    is that we need to account for buffering in join execution. 
    
    An edge-case example: if we have a non-first table in join accessed via
    ref(const) or ref(param) where there is a small number of different
    values of param, then the access will likely hit the disk cache and will
    not require any disk seeks.
    
    The proper solution would be to assume an LRU disk cache of some size,
    calculate probability of cache hits, etc. For now we just count
    identical ref accesses as one.

  RETURN 
    Expected number of row combinations
*/

double
prev_record_reads(const POSITION *positions, uint idx, table_map found_ref)
{
  double found=1.0;
  const POSITION *pos_end= positions - 1;
  for (const POSITION *pos= positions + idx - 1; pos != pos_end; pos--)
  {
    if (pos->table->table->map & found_ref)
    {
      found_ref|= pos->ref_depend_map;
      /* 
        For the case of "t1 LEFT JOIN t2 ON ..." where t2 is a const table 
        with no matching row we will get position[t2].records_read==0. 
        Actually the size of output is one null-complemented row, therefore 
        we will use value of 1 whenever we get records_read==0.

        Note
        - the above case can't occur if inner part of outer join has more 
          than one table: table with no matches will not be marked as const.

        - Ideally we should add 1 to records_read for every possible null-
          complemented row. We're not doing it because: 1. it will require
          non-trivial code and add overhead. 2. The value of records_read
          is an inprecise estimate and adding 1 (or, in the worst case,
          #max_nested_outer_joins=64-1) will not make it any more precise.
      */
      if (pos->records_read)
      {
        found= COST_MULT(found, pos->records_read);
        found*= pos->cond_selectivity;
      }
     }
  }
  return found;
}


/*
  Enumerate join tabs in breadth-first fashion, including const tables.
*/

static JOIN_TAB *next_breadth_first_tab(JOIN_TAB *first_top_tab,
                                        uint n_top_tabs_count, JOIN_TAB *tab)
{
  n_top_tabs_count += tab->join->aggr_tables;
  if (!tab->bush_root_tab)
  {
    /* We're at top level. Get the next top-level tab */
    tab++;
    if (tab < first_top_tab + n_top_tabs_count)
      return tab;

    /* No more top-level tabs. Switch to enumerating SJM nest children */
    tab= first_top_tab;
  }
  else
  {
    /* We're inside of an SJM nest */
    if (!tab->last_leaf_in_bush)
    {
      /* There's one more table in the nest, return it. */
      return ++tab;
    }
    else
    {
      /* 
        There are no more tables in this nest. Get out of it and then we'll
        proceed to the next nest.
      */
      tab= tab->bush_root_tab + 1;
    }
  }
   
  /* 
    Ok, "tab" points to a top-level table, and we need to find the next SJM
    nest and enter it.
  */
  for (; tab < first_top_tab + n_top_tabs_count; tab++)
  {
    if (tab->bush_children)
      return tab->bush_children->start;
  }
  return NULL;
}


/* 
  Enumerate JOIN_TABs in "EXPLAIN order". This order
   - const tabs are included
   - we enumerate "optimization tabs".
   - 
*/

JOIN_TAB *first_explain_order_tab(JOIN* join)
{
  JOIN_TAB* tab;
  tab= join->join_tab;
  if (!tab)
    return NULL; /* Can happen when when the tables were optimized away */
  return (tab->bush_children) ? tab->bush_children->start : tab;
}


JOIN_TAB *next_explain_order_tab(JOIN* join, JOIN_TAB* tab)
{
  /* If we're inside SJM nest and have reached its end, get out */
  if (tab->last_leaf_in_bush)
    return tab->bush_root_tab;
  
  /* Move to next tab in the array we're traversing */
  tab++;
  
  if (tab == join->join_tab + join->top_join_tab_count)
    return NULL; /* Outside SJM nest and reached EOF */

  if (tab->bush_children)
    return tab->bush_children->start;

  return tab;
}



JOIN_TAB *first_top_level_tab(JOIN *join, enum enum_with_const_tables const_tbls)
{
  JOIN_TAB *tab= join->join_tab;
  if (const_tbls == WITHOUT_CONST_TABLES)
  {
    if (join->const_tables == join->table_count || !tab)
      return NULL;
    tab += join->const_tables;
  }
  return tab;
}


JOIN_TAB *next_top_level_tab(JOIN *join, JOIN_TAB *tab)
{
  tab= next_breadth_first_tab(join->first_breadth_first_tab(),
                              join->top_join_tab_count, tab);
  if (tab && tab->bush_root_tab)
    tab= NULL;
  return tab;
}


JOIN_TAB *first_linear_tab(JOIN *join,
                           enum enum_with_bush_roots include_bush_roots,
                           enum enum_with_const_tables const_tbls)
{
  JOIN_TAB *first= join->join_tab;

  if (!first)
    return NULL;

  if (const_tbls == WITHOUT_CONST_TABLES)
    first+= join->const_tables;

  if (first >= join->join_tab + join->top_join_tab_count)
    return NULL; /* All are const tables */

  if (first->bush_children && include_bush_roots == WITHOUT_BUSH_ROOTS)
  {
    /* This JOIN_TAB is a SJM nest; Start from first table in nest */
    return first->bush_children->start;
  }

  return first;
}


/*
  A helper function to loop over all join's join_tab in sequential fashion

  DESCRIPTION
    Depending on include_bush_roots parameter, JOIN_TABs that represent
    SJM-scan/lookups are either returned or omitted.

    SJM-Bush children are returned right after (or in place of) their container
    join tab (TODO: does anybody depend on this? A: make_join_readinfo() seems
    to)

    For example, if we have this structure:
      
       ot1--ot2--sjm1----------------ot3-...
                  |
                  +--it1--it2--it3

    calls to next_linear_tab( include_bush_roots=TRUE) will return:
      
      ot1 ot2 sjm1 it1 it2 it3 ot3 ...
   
   while calls to next_linear_tab( include_bush_roots=FALSE) will return:

      ot1 ot2 it1 it2 it3 ot3 ...

   (note that sjm1 won't be returned).
*/

JOIN_TAB *next_linear_tab(JOIN* join, JOIN_TAB* tab, 
                          enum enum_with_bush_roots include_bush_roots)
{
  if (include_bush_roots == WITH_BUSH_ROOTS && tab->bush_children)
  {
    /* This JOIN_TAB is a SJM nest; Start from first table in nest */
    return tab->bush_children->start;
  }

  DBUG_ASSERT(!tab->last_leaf_in_bush || tab->bush_root_tab);

  if (tab->bush_root_tab)       /* Are we inside an SJM nest */
  {
    /* Inside SJM nest */
    if (!tab->last_leaf_in_bush)
      return tab+1;              /* Return next in nest */
    /* Continue from the sjm on the top level */
    tab= tab->bush_root_tab;
  }

  /* If no more JOIN_TAB's on the top level */
  if (++tab >= join->join_tab + join->exec_join_tab_cnt() + join->aggr_tables)
    return NULL;

  if (include_bush_roots == WITHOUT_BUSH_ROOTS && tab->bush_children)
  {
    /* This JOIN_TAB is a SJM nest; Start from first table in nest */
    tab= tab->bush_children->start;
  }
  return tab;
}


/*
  Start to iterate over all join tables in bush-children-first order, excluding 
  the const tables (see next_depth_first_tab() comment for details)
*/

JOIN_TAB *first_depth_first_tab(JOIN* join)
{
  JOIN_TAB* tab;
  /* This means we're starting the enumeration */
  if (join->const_tables == join->top_join_tab_count || !join->join_tab)
    return NULL;

  tab= join->join_tab + join->const_tables;

  return (tab->bush_children) ? tab->bush_children->start : tab;
}


/*
  A helper function to iterate over all join tables in bush-children-first order

  DESCRIPTION
   
  For example, for this join plan

    ot1--ot2--sjm1------------ot3-...
               |
               |
              it1--it2--it3 
  
  call to first_depth_first_tab() will return ot1, and subsequent calls to
  next_depth_first_tab() will return:

     ot2 it1 it2 it3 sjm ot3 ...
*/

JOIN_TAB *next_depth_first_tab(JOIN* join, JOIN_TAB* tab)
{
  /* If we're inside SJM nest and have reached its end, get out */
  if (tab->last_leaf_in_bush)
    return tab->bush_root_tab;
  
  /* Move to next tab in the array we're traversing */
  tab++;
  
  if (tab == join->join_tab +join->top_join_tab_count)
    return NULL; /* Outside SJM nest and reached EOF */

  if (tab->bush_children)
    return tab->bush_children->start;

  return tab;
}


bool JOIN::check_two_phase_optimization(THD *thd)
{
  if (check_for_splittable_materialized())
    return true;
  return false;
}


bool JOIN::inject_cond_into_where(Item *injected_cond)
{
  Item *where_item= injected_cond;
  List<Item> *and_args= NULL;
  if (conds && conds->type() == Item::COND_ITEM &&
      ((Item_cond*) conds)->functype() == Item_func::COND_AND_FUNC)
  {
    and_args= ((Item_cond*) conds)->argument_list();
    if (cond_equal)
      and_args->disjoin((List<Item> *) &cond_equal->current_level);
  }

  where_item= and_items(thd, conds, where_item);
  if (where_item->fix_fields_if_needed(thd, 0))
    return true;
  thd->change_item_tree(&select_lex->where, where_item);
  select_lex->where->top_level_item();
  conds= select_lex->where;

  if (and_args && cond_equal)
  {
    and_args= ((Item_cond*) conds)->argument_list();
    List_iterator<Item_equal> li(cond_equal->current_level);
    Item_equal *elem;
    while ((elem= li++))
    {
      and_args->push_back(elem, thd->mem_root);
    }
  }

  return false;

}


static Item * const null_ptr= NULL;

/*
  Set up join struct according to the picked join order in
  
  SYNOPSIS
    get_best_combination()
      join  The join to process (the picked join order is mainly in
            join->best_positions)

  DESCRIPTION
    Setup join structures according the picked join order
    - finalize semi-join strategy choices (see
        fix_semijoin_strategies_for_picked_join_order)
    - create join->join_tab array and put there the JOIN_TABs in the join order
    - create data structures describing ref access methods.

  NOTE
    In this function we switch from pre-join-optimization JOIN_TABs to
    post-join-optimization JOIN_TABs. This is achieved by copying the entire
    JOIN_TAB objects.
 
  RETURN 
    FALSE  OK
    TRUE   Out of memory
*/

bool JOIN::get_best_combination()
{
  uint tablenr;
  table_map used_tables;
  JOIN_TAB *j;
  KEYUSE *keyuse;
  DBUG_ENTER("get_best_combination");

   /*
    Additional plan nodes for postjoin tmp tables:
      1? + // For GROUP BY
      1? + // For DISTINCT
      1? + // For aggregation functions aggregated in outer query
           // when used with distinct
      1? + // For ORDER BY
      1?   // buffer result
    Up to 2 tmp tables are actually used, but it's hard to tell exact number
    at this stage.
  */ 
  uint aggr_tables= (group_list ? 1 : 0) +
                    (select_distinct ?
                     (tmp_table_param.using_outer_summary_function ? 2 : 1) : 0) +
                    (order ? 1 : 0) +
       (select_options & (SELECT_BIG_RESULT | OPTION_BUFFER_RESULT) ? 1 : 0) ;
  
  if (aggr_tables == 0)
    aggr_tables= 1; /* For group by pushdown */

  if (select_lex->window_specs.elements)
    aggr_tables++;

  if (aggr_tables > 2)
    aggr_tables= 2;
  if (!(join_tab= (JOIN_TAB*) thd->alloc(sizeof(JOIN_TAB)*
                                        (top_join_tab_count + aggr_tables))))
    DBUG_RETURN(TRUE);

  full_join=0;
  hash_join= FALSE;

  fix_semijoin_strategies_for_picked_join_order(this);
   
  JOIN_TAB_RANGE *root_range;
  if (!(root_range= new (thd->mem_root) JOIN_TAB_RANGE))
    DBUG_RETURN(TRUE);
   root_range->start= join_tab;
  /* root_range->end will be set later */
  join_tab_ranges.empty();

  if (join_tab_ranges.push_back(root_range, thd->mem_root))
    DBUG_RETURN(TRUE);

  JOIN_TAB *sjm_nest_end= NULL;
  JOIN_TAB *sjm_nest_root= NULL;

  for (j=join_tab, tablenr=0 ; tablenr < table_count ; tablenr++,j++)
  {
    TABLE *form;
    POSITION *cur_pos= &best_positions[tablenr];
    if (cur_pos->sj_strategy == SJ_OPT_MATERIALIZE || 
        cur_pos->sj_strategy == SJ_OPT_MATERIALIZE_SCAN)
    {
      /*
        Ok, we've entered an SJ-Materialization semi-join (note that this can't
        be done recursively, semi-joins are not allowed to be nested).
        1. Put into main join order a JOIN_TAB that represents a lookup or scan
           in the temptable.
      */
      bzero((void*)j, sizeof(JOIN_TAB));
      j->join= this;
      j->table= NULL; //temporary way to tell SJM tables from others.
      j->ref.key = -1;
      j->on_expr_ref= (Item**) &null_ptr;
      j->keys= key_map(1); /* The unique index is always in 'possible keys' in EXPLAIN */

      /*
        2. Proceed with processing SJM nest's join tabs, putting them into the
           sub-order
      */
      SJ_MATERIALIZATION_INFO *sjm= cur_pos->table->emb_sj_nest->sj_mat_info;
      j->records_read= (sjm->is_sj_scan? sjm->rows : 1);
      j->records= (ha_rows) j->records_read;
      j->cond_selectivity= 1.0;
      JOIN_TAB *jt;
      JOIN_TAB_RANGE *jt_range;
      if (!(jt= (JOIN_TAB*) thd->alloc(sizeof(JOIN_TAB)*sjm->tables)) ||
          !(jt_range= new JOIN_TAB_RANGE))
        DBUG_RETURN(TRUE);
      jt_range->start= jt;
      jt_range->end= jt + sjm->tables;
      join_tab_ranges.push_back(jt_range, thd->mem_root);
      j->bush_children= jt_range;
      sjm_nest_end= jt + sjm->tables;
      sjm_nest_root= j;

      j= jt;
    }
    
    *j= *best_positions[tablenr].table;

    j->bush_root_tab= sjm_nest_root;

    form= table[tablenr]= j->table;
    form->reginfo.join_tab=j;
    DBUG_PRINT("info",("type: %d", j->type));
    if (j->type == JT_CONST)
      goto loop_end;					// Handled in make_join_stat..

    j->loosescan_match_tab= NULL;  //non-nulls will be set later
    j->inside_loosescan_range= FALSE;
    j->ref.key = -1;
    j->ref.key_parts=0;

    if (j->type == JT_SYSTEM)
      goto loop_end;
    if ( !(keyuse= best_positions[tablenr].key))
    {
      j->type=JT_ALL;
      if (best_positions[tablenr].use_join_buffer &&
          tablenr != const_tables)
	full_join= 1;
    }

    /*if (best_positions[tablenr].sj_strategy == SJ_OPT_LOOSE_SCAN)
    {
      DBUG_ASSERT(!keyuse || keyuse->key ==
                             best_positions[tablenr].loosescan_picker.loosescan_key);
      j->index= best_positions[tablenr].loosescan_picker.loosescan_key;
    }*/

    if ((j->type == JT_REF || j->type == JT_EQ_REF) &&
        is_hash_join_key_no(j->ref.key))
      hash_join= TRUE; 

  loop_end:
    /* 
      Save records_read in JOIN_TAB so that select_describe()/etc don't have
      to access join->best_positions[]. 
    */
    j->records_read= best_positions[tablenr].records_read;
    j->cond_selectivity= best_positions[tablenr].cond_selectivity;
    map2table[j->table->tablenr]= j;

    /* If we've reached the end of sjm nest, switch back to main sequence */
    if (j + 1 == sjm_nest_end)
    {
      j->last_leaf_in_bush= TRUE;
      j= sjm_nest_root;
      sjm_nest_root= NULL;
      sjm_nest_end= NULL;
    }
  }
  root_range->end= j;

  used_tables= OUTER_REF_TABLE_BIT;		// Outer row is already read
  for (j=join_tab, tablenr=0 ; tablenr < table_count ; tablenr++,j++)
  {
    if (j->bush_children)
      j= j->bush_children->start;

    used_tables|= j->table->map;
    if (j->type != JT_CONST && j->type != JT_SYSTEM)
    {
      if ((keyuse= best_positions[tablenr].key) &&
          create_ref_for_key(this, j, keyuse, TRUE, used_tables))
        DBUG_RETURN(TRUE);              // Something went wrong
    }
    if (j->last_leaf_in_bush)
      j= j->bush_root_tab;
  }
 
  top_join_tab_count= (uint)(join_tab_ranges.head()->end - 
                      join_tab_ranges.head()->start);

  update_depend_map(this);
  DBUG_RETURN(0);
}

/**
  Create a descriptor of hash join key to access a given join table  

  @param   join         join which the join table belongs to
  @param   join_tab     the join table to access
  @param   org_keyuse   beginning of the key uses to join this table
  @param   used_tables  bitmap of the previous tables

  @details
  This function first finds key uses that can be utilized by the hash join
  algorithm to join join_tab to the previous tables marked in the bitmap 
  used_tables.  The tested key uses are taken from the array of all key uses
  for 'join' starting from the position org_keyuse. After all interesting key
  uses have been found the function builds a descriptor of the corresponding
  key that is used by the hash join algorithm would it be chosen to join
  the table join_tab.

  @retval  FALSE  the descriptor for a hash join key is successfully created
  @retval  TRUE   otherwise
*/

static bool create_hj_key_for_table(JOIN *join, JOIN_TAB *join_tab,
                                    KEYUSE *org_keyuse, table_map used_tables)
{
  KEY *keyinfo;
  KEY_PART_INFO *key_part_info;
  KEYUSE *keyuse= org_keyuse;
  uint key_parts= 0;
  THD  *thd= join->thd;
  TABLE *table= join_tab->table;
  bool first_keyuse= TRUE;
  DBUG_ENTER("create_hj_key_for_table");

  do
  {
    if (!(~used_tables & keyuse->used_tables) &&
        join_tab->keyuse_is_valid_for_access_in_chosen_plan(join, keyuse) &&
        are_tables_local(join_tab, keyuse->used_tables))    
    {
      if (first_keyuse)
      {
        key_parts++;
      }
      else
      {
        KEYUSE *curr= org_keyuse;
        for( ; curr < keyuse; curr++)
        {
          if (curr->keypart == keyuse->keypart &&
              !(~used_tables & curr->used_tables) &&
              join_tab->keyuse_is_valid_for_access_in_chosen_plan(join,
                                                                  curr) &&
              are_tables_local(join_tab, curr->used_tables))
            break;
        }
        if (curr == keyuse)
           key_parts++;
      }
    }
    first_keyuse= FALSE;
    keyuse++;
  } while (keyuse->table == table && keyuse->is_for_hash_join());
  if (!key_parts)
    DBUG_RETURN(TRUE);
  /* This memory is allocated only once for the joined table join_tab */
  if (!(keyinfo= (KEY *) thd->alloc(sizeof(KEY))) ||
      !(key_part_info = (KEY_PART_INFO *) thd->alloc(sizeof(KEY_PART_INFO)*
                                                     key_parts)))
    DBUG_RETURN(TRUE);
  keyinfo->usable_key_parts= keyinfo->user_defined_key_parts = key_parts;
  keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
  keyinfo->key_part= key_part_info;
  keyinfo->key_length=0;
  keyinfo->algorithm= HA_KEY_ALG_UNDEF;
  keyinfo->flags= HA_GENERATED_KEY;
  keyinfo->is_statistics_from_stat_tables= FALSE;
  keyinfo->name.str= "$hj";
  keyinfo->name.length= 3;
  keyinfo->rec_per_key= (ulong*) thd->calloc(sizeof(ulong)*key_parts);
  if (!keyinfo->rec_per_key)
    DBUG_RETURN(TRUE);
  keyinfo->key_part= key_part_info;

  first_keyuse= TRUE;
  keyuse= org_keyuse;
  do
  {
    if (!(~used_tables & keyuse->used_tables) &&
        join_tab->keyuse_is_valid_for_access_in_chosen_plan(join, keyuse) &&
        are_tables_local(join_tab, keyuse->used_tables))
    { 
      bool add_key_part= TRUE;
      if (!first_keyuse)
      {
        for(KEYUSE *curr= org_keyuse; curr < keyuse; curr++)
        {
          if (curr->keypart == keyuse->keypart &&
              !(~used_tables & curr->used_tables) &&
              join_tab->keyuse_is_valid_for_access_in_chosen_plan(join,
                                                                  curr) &&
              are_tables_local(join_tab, curr->used_tables))
	  {
            keyuse->keypart= NO_KEYPART;
            add_key_part= FALSE;
            break;
          }
        }
      }
      if (add_key_part)
      {
        Field *field= table->field[keyuse->keypart];
        uint fieldnr= keyuse->keypart+1;
        table->create_key_part_by_field(key_part_info, field, fieldnr);
        keyinfo->key_length += key_part_info->store_length;
        key_part_info++;
      }
    }
    first_keyuse= FALSE;
    keyuse++;
  } while (keyuse->table == table && keyuse->is_for_hash_join());

  keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
  keyinfo->ext_key_flags= keyinfo->flags;
  keyinfo->ext_key_part_map= 0;

  join_tab->hj_key= keyinfo;

  DBUG_RETURN(FALSE);
}

/* 
  Check if a set of tables specified by used_tables can be accessed when
  we're doing scan on join_tab jtab.
*/
static bool are_tables_local(JOIN_TAB *jtab, table_map used_tables)
{
  if (jtab->bush_root_tab)
  {
    /*
      jtab is inside execution join nest. We may not refer to outside tables,
      except the const tables.
    */
    table_map local_tables= jtab->emb_sj_nest->nested_join->used_tables |
                            jtab->join->const_table_map |
                            OUTER_REF_TABLE_BIT;
    return !MY_TEST(used_tables & ~local_tables);
  }

  /* 
    If we got here then jtab is at top level. 
     - all other tables at top level are accessible,
     - tables in join nests are accessible too, because all their columns that 
       are needed at top level will be unpacked when scanning the
       materialization table.
  */
  return TRUE;
}

static bool create_ref_for_key(JOIN *join, JOIN_TAB *j,
                               KEYUSE *org_keyuse, bool allow_full_scan, 
                               table_map used_tables)
{
  uint keyparts, length, key;
  TABLE *table;
  KEY *keyinfo;
  KEYUSE *keyuse= org_keyuse;
  bool ftkey= (keyuse->keypart == FT_KEYPART);
  THD *thd= join->thd;
  DBUG_ENTER("create_ref_for_key");

  /*  Use best key from find_best */
  table= j->table;
  key= keyuse->key;
  if (!is_hash_join_key_no(key))
    keyinfo= table->key_info+key;
  else
  {
    if (create_hj_key_for_table(join, j, org_keyuse, used_tables))
      DBUG_RETURN(TRUE);
    keyinfo= j->hj_key;
  }

  if (ftkey)
  {
    Item_func_match *ifm=(Item_func_match *)keyuse->val;

    length=0;
    keyparts=1;
    ifm->join_key=1;
  }
  else
  {
    keyparts=length=0;
    uint found_part_ref_or_null= 0;
    /*
      Calculate length for the used key
      Stop if there is a missing key part or when we find second key_part
      with KEY_OPTIMIZE_REF_OR_NULL
    */
    do
    {
      if (!(~used_tables & keyuse->used_tables) &&
          (!keyuse->validity_ref || *keyuse->validity_ref) &&
	  j->keyuse_is_valid_for_access_in_chosen_plan(join, keyuse))
      {
        if  (are_tables_local(j, keyuse->val->used_tables()))
        {
          if ((is_hash_join_key_no(key) && keyuse->keypart != NO_KEYPART) ||
              (!is_hash_join_key_no(key) && keyparts == keyuse->keypart &&
               !(found_part_ref_or_null & keyuse->optimize)))
          {
             length+= keyinfo->key_part[keyparts].store_length;
             keyparts++;
             found_part_ref_or_null|= keyuse->optimize & ~KEY_OPTIMIZE_EQ;
          }
        }
      }
      keyuse++;
    } while (keyuse->table == table && keyuse->key == key);

    if (!keyparts && allow_full_scan)
    {
      /* It's a LooseIndexScan strategy scanning whole index */
      j->type= JT_ALL;
      j->index= key;
      DBUG_RETURN(FALSE);
    }

    DBUG_ASSERT(length > 0);
    DBUG_ASSERT(keyparts != 0);
  } /* not ftkey */
  
  /* set up fieldref */
  j->ref.key_parts= keyparts;
  j->ref.key_length= length;
  j->ref.key= (int) key;
  if (!(j->ref.key_buff= (uchar*) thd->calloc(ALIGN_SIZE(length)*2)) ||
      !(j->ref.key_copy= (store_key**) thd->alloc((sizeof(store_key*) *
						          (keyparts+1)))) ||
      !(j->ref.items=(Item**) thd->alloc(sizeof(Item*)*keyparts)) ||
      !(j->ref.cond_guards= (bool**) thd->alloc(sizeof(uint*)*keyparts)))
  {
    DBUG_RETURN(TRUE);
  }
  j->ref.key_buff2=j->ref.key_buff+ALIGN_SIZE(length);
  j->ref.key_err=1;
  j->ref.has_record= FALSE;
  j->ref.null_rejecting= 0;
  j->ref.disable_cache= FALSE;
  j->ref.null_ref_part= NO_REF_PART;
  j->ref.const_ref_part_map= 0;
  j->ref.uses_splitting= FALSE;
  keyuse=org_keyuse;

  store_key **ref_key= j->ref.key_copy;
  uchar *key_buff=j->ref.key_buff, *null_ref_key= 0;
  uint null_ref_part= NO_REF_PART;
  bool keyuse_uses_no_tables= TRUE;
  uint not_null_keyparts= 0;
  if (ftkey)
  {
    j->ref.items[0]=((Item_func*)(keyuse->val))->key_item();
    /* Predicates pushed down into subquery can't be used FT access */
    j->ref.cond_guards[0]= NULL;
    if (keyuse->used_tables)
      DBUG_RETURN(TRUE);                        // not supported yet. SerG

    j->type=JT_FT;
  }
  else
  {
    uint i;
    for (i=0 ; i < keyparts ; keyuse++,i++)
    {
      while (((~used_tables) & keyuse->used_tables) ||
             (keyuse->validity_ref && !(*keyuse->validity_ref)) ||
	     !j->keyuse_is_valid_for_access_in_chosen_plan(join, keyuse) ||
             keyuse->keypart == NO_KEYPART ||
	     (keyuse->keypart != 
              (is_hash_join_key_no(key) ?
                 keyinfo->key_part[i].field->field_index : i)) || 
             !are_tables_local(j, keyuse->val->used_tables())) 
	 keyuse++;                              	/* Skip other parts */ 

      uint maybe_null= MY_TEST(keyinfo->key_part[i].null_bit);
      j->ref.items[i]=keyuse->val;		// Save for cond removal
      j->ref.cond_guards[i]= keyuse->cond_guard;

      if (!keyuse->val->maybe_null || keyuse->null_rejecting)
        not_null_keyparts++;
      /*
        Set ref.null_rejecting to true only if we are going to inject a
        "keyuse->val IS NOT NULL" predicate.
      */
      Item *real= (keyuse->val)->real_item();
      if (keyuse->null_rejecting && (real->type() == Item::FIELD_ITEM) &&
          ((Item_field*)real)->field->maybe_null())
        j->ref.null_rejecting|= (key_part_map)1 << i;

      keyuse_uses_no_tables= keyuse_uses_no_tables && !keyuse->used_tables;
      j->ref.uses_splitting |= (keyuse->validity_ref != NULL);
      /*
        We don't want to compute heavy expressions in EXPLAIN, an example would
        select * from t1 where t1.key=(select thats very heavy);

        (select thats very heavy) => is a constant here
        eg: (select avg(order_cost) from orders) => constant but expensive
      */
      if (!keyuse->val->used_tables() && !thd->lex->describe)
      {					// Compare against constant
        store_key_item tmp(thd,
                           keyinfo->key_part[i].field,
                           key_buff + maybe_null,
                           maybe_null ?  key_buff : 0,
                           keyinfo->key_part[i].length,
                           keyuse->val,
                           FALSE);
        if (unlikely(thd->is_fatal_error))
          DBUG_RETURN(TRUE);
        tmp.copy();
        j->ref.const_ref_part_map |= key_part_map(1) << i ;
      }
      else
      {
        *ref_key++= get_store_key(thd,
                                  keyuse,join->const_table_map,
                                  &keyinfo->key_part[i],
                                  key_buff, maybe_null);
        if (!keyuse->val->used_tables())
          j->ref.const_ref_part_map |= key_part_map(1) << i ;
      }
      /*
	Remember if we are going to use REF_OR_NULL
	But only if field _really_ can be null i.e. we force JT_REF
	instead of JT_REF_OR_NULL in case if field can't be null
      */
      if ((keyuse->optimize & KEY_OPTIMIZE_REF_OR_NULL) && maybe_null)
      {
	null_ref_key= key_buff;
        null_ref_part= i;
      }
      key_buff+= keyinfo->key_part[i].store_length;
    }
  } /* not ftkey */
  *ref_key=0;				// end_marker
  if (j->type == JT_FT)
    DBUG_RETURN(0);
  ulong key_flags= j->table->actual_key_flags(keyinfo);
  if (j->type == JT_CONST)
    j->table->const_table= 1;
  else if (!((keyparts == keyinfo->user_defined_key_parts &&
              (
                (key_flags & (HA_NOSAME | HA_NULL_PART_KEY)) == HA_NOSAME ||
                /* Unique key and all keyparts are NULL rejecting */
                ((key_flags & HA_NOSAME) && keyparts == not_null_keyparts)
              )) ||
              /* true only for extended keys */
              (keyparts > keyinfo->user_defined_key_parts &&
               MY_TEST(key_flags & HA_EXT_NOSAME) &&
               keyparts == keyinfo->ext_key_parts)
            ) ||
            null_ref_key)
  {
    /* Must read with repeat */
    j->type= null_ref_key ? JT_REF_OR_NULL : JT_REF;
    j->ref.null_ref_key= null_ref_key;
    j->ref.null_ref_part= null_ref_part;
  }
  else if (keyuse_uses_no_tables)
  {
    /*
      This happen if we are using a constant expression in the ON part
      of an LEFT JOIN.
      SELECT * FROM a LEFT JOIN b ON b.key=30
      Here we should not mark the table as a 'const' as a field may
      have a 'normal' value or a NULL value.
    */
    j->type=JT_CONST;
  }
  else
    j->type=JT_EQ_REF;

  j->read_record.unlock_row= (j->type == JT_EQ_REF)? 
                             join_read_key_unlock_row : rr_unlock_row; 
  DBUG_RETURN(0);
}



static store_key *
get_store_key(THD *thd, KEYUSE *keyuse, table_map used_tables,
	      KEY_PART_INFO *key_part, uchar *key_buff, uint maybe_null)
{
  if (!((~used_tables) & keyuse->used_tables))		// if const item
  {
    return new store_key_const_item(thd,
				    key_part->field,
				    key_buff + maybe_null,
				    maybe_null ? key_buff : 0,
				    key_part->length,
				    keyuse->val);
  }
  else if (keyuse->val->type() == Item::FIELD_ITEM ||
           (keyuse->val->type() == Item::REF_ITEM &&
	    ((((Item_ref*)keyuse->val)->ref_type() == Item_ref::OUTER_REF &&
              (*(Item_ref**)((Item_ref*)keyuse->val)->ref)->ref_type() ==
              Item_ref::DIRECT_REF) || 
             ((Item_ref*)keyuse->val)->ref_type() == Item_ref::VIEW_REF) &&
            keyuse->val->real_item()->type() == Item::FIELD_ITEM))
    return new store_key_field(thd,
			       key_part->field,
			       key_buff + maybe_null,
			       maybe_null ? key_buff : 0,
			       key_part->length,
			       ((Item_field*) keyuse->val->real_item())->field,
			       keyuse->val->real_item()->full_name());

  return new store_key_item(thd,
			    key_part->field,
			    key_buff + maybe_null,
			    maybe_null ? key_buff : 0,
			    key_part->length,
			    keyuse->val, FALSE);
}


inline void add_cond_and_fix(THD *thd, Item **e1, Item *e2)
{
  if (*e1)
  {
    if (!e2)
      return;
    Item *res;
    if ((res= new (thd->mem_root) Item_cond_and(thd, *e1, e2)))
    {
      res->fix_fields(thd, 0);
      res->update_used_tables();
      *e1= res;
    }
  }
  else
    *e1= e2;
}


/**
  Add to join_tab->select_cond[i] "table.field IS NOT NULL" conditions
  we've inferred from ref/eq_ref access performed.

    This function is a part of "Early NULL-values filtering for ref access"
    optimization.

    Example of this optimization:
    For query SELECT * FROM t1,t2 WHERE t2.key=t1.field @n
    and plan " any-access(t1), ref(t2.key=t1.field) " @n
    add "t1.field IS NOT NULL" to t1's table condition. @n

    Description of the optimization:
    
      We look through equalities choosen to perform ref/eq_ref access,
      pick equalities that have form "tbl.part_of_key = othertbl.field"
      (where othertbl is a non-const table and othertbl.field may be NULL)
      and add them to conditions on correspoding tables (othertbl in this
      example).

      Exception from that is the case when referred_tab->join != join.
      I.e. don't add NOT NULL constraints from any embedded subquery.
      Consider this query:
      @code
      SELECT A.f2 FROM t1 LEFT JOIN t2 A ON A.f2 = f1
      WHERE A.f3=(SELECT MIN(f3) FROM  t2 C WHERE A.f4 = C.f4) OR A.f3 IS NULL;
      @endocde
      Here condition A.f3 IS NOT NULL is going to be added to the WHERE
      condition of the embedding query.
      Another example:
      SELECT * FROM t10, t11 WHERE (t10.a < 10 OR t10.a IS NULL)
      AND t11.b <=> t10.b AND (t11.a = (SELECT MAX(a) FROM t12
      WHERE t12.b = t10.a ));
      Here condition t10.a IS NOT NULL is going to be added.
      In both cases addition of NOT NULL condition will erroneously reject
      some rows of the result set.
      referred_tab->join != join constraint would disallow such additions.

      This optimization doesn't affect the choices that ref, range, or join
      optimizer make. This was intentional because this was added after 4.1
      was GA.
      
    Implementation overview
      1. update_ref_and_keys() accumulates info about null-rejecting
         predicates in in KEY_FIELD::null_rejecting
      1.1 add_key_part saves these to KEYUSE.
      2. create_ref_for_key copies them to TABLE_REF.
      3. add_not_null_conds adds "x IS NOT NULL" to join_tab->select_cond of
         appropiate JOIN_TAB members.
*/

static void add_not_null_conds(JOIN *join)
{
  JOIN_TAB *tab;
  DBUG_ENTER("add_not_null_conds");
  
  for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    if (tab->type == JT_REF || tab->type == JT_EQ_REF || 
        tab->type == JT_REF_OR_NULL)
    {
      for (uint keypart= 0; keypart < tab->ref.key_parts; keypart++)
      {
        if (tab->ref.null_rejecting & ((key_part_map)1 << keypart))
        {
          Item *item= tab->ref.items[keypart];
          Item *notnull;
          Item *real= item->real_item();
	  if (real->const_item() && real->type() != Item::FIELD_ITEM && 
              !real->is_expensive())
          {
            /*
              It could be constant instead of field after constant
              propagation.
            */
            continue;
          }
          DBUG_ASSERT(real->type() == Item::FIELD_ITEM);
          Item_field *not_null_item= (Item_field*)real;
          JOIN_TAB *referred_tab= not_null_item->field->table->reginfo.join_tab;
          /*
            For UPDATE queries such as:
            UPDATE t1 SET t1.f2=(SELECT MAX(t2.f4) FROM t2 WHERE t2.f3=t1.f1);
            not_null_item is the t1.f1, but it's referred_tab is 0.
          */
          if (!(notnull= new (join->thd->mem_root)
                Item_func_isnotnull(join->thd, item)))
            DBUG_VOID_RETURN;
          /*
            We need to do full fix_fields() call here in order to have correct
            notnull->const_item(). This is needed e.g. by test_quick_select 
            when it is called from make_join_select after this function is 
            called.
          */
          if (notnull->fix_fields(join->thd, &notnull))
            DBUG_VOID_RETURN;

          DBUG_EXECUTE("where",print_where(notnull,
                                            (referred_tab ?
                                            referred_tab->table->alias.c_ptr() :
                                            "outer_ref_cond"),
                                            QT_ORDINARY););
          if (!tab->first_inner)
          {
            COND *new_cond= (referred_tab && referred_tab->join == join) ?
                              referred_tab->select_cond :
                              join->outer_ref_cond;
            add_cond_and_fix(join->thd, &new_cond, notnull);
            if (referred_tab && referred_tab->join == join)
              referred_tab->set_select_cond(new_cond, __LINE__);
            else 
              join->outer_ref_cond= new_cond;
          }
          else
            add_cond_and_fix(join->thd, tab->first_inner->on_expr_ref, notnull);
        }
      }
    }
  }
  DBUG_VOID_RETURN;
}

/**
  Build a predicate guarded by match variables for embedding outer joins.
  The function recursively adds guards for predicate cond
  assending from tab to the first inner table  next embedding
  nested outer join and so on until it reaches root_tab
  (root_tab can be 0).

  In other words:
  add_found_match_trig_cond(tab->first_inner_tab, y, 0) is the way one should 
  wrap parts of WHERE.  The idea is that the part of WHERE should be only
  evaluated after we've finished figuring out whether outer joins.
  ^^^ is the above correct?

  @param tab       the first inner table for most nested outer join
  @param cond      the predicate to be guarded (must be set)
  @param root_tab  the first inner table to stop

  @return
    -  pointer to the guarded predicate, if success
    -  0, otherwise
*/

static COND*
add_found_match_trig_cond(THD *thd, JOIN_TAB *tab, COND *cond,
                          JOIN_TAB *root_tab)
{
  COND *tmp;
  DBUG_ASSERT(cond != 0);
  if (tab == root_tab)
    return cond;
  if ((tmp= add_found_match_trig_cond(thd, tab->first_upper, cond, root_tab)))
    tmp= new (thd->mem_root) Item_func_trig_cond(thd, tmp, &tab->found);
  if (tmp)
  {
    tmp->quick_fix_field();
    tmp->update_used_tables();
  }
  return tmp;
}


bool TABLE_LIST::is_active_sjm()
{ 
  return sj_mat_info && sj_mat_info->is_used;
}


/**
  Fill in outer join related info for the execution plan structure.

    For each outer join operation left after simplification of the
    original query the function set up the following pointers in the linear
    structure join->join_tab representing the selected execution plan.
    The first inner table t0 for the operation is set to refer to the last
    inner table tk through the field t0->last_inner.
    Any inner table ti for the operation are set to refer to the first
    inner table ti->first_inner.
    The first inner table t0 for the operation is set to refer to the
    first inner table of the embedding outer join operation, if there is any,
    through the field t0->first_upper.
    The on expression for the outer join operation is attached to the
    corresponding first inner table through the field t0->on_expr_ref.
    Here ti are structures of the JOIN_TAB type.

    In other words, for each join tab, set
     - first_inner
     - last_inner
     - first_upper
     - on_expr_ref, cond_equal

  EXAMPLE. For the query: 
  @code
        SELECT * FROM t1
                      LEFT JOIN
                      (t2, t3 LEFT JOIN t4 ON t3.a=t4.a)
                      ON (t1.a=t2.a AND t1.b=t3.b)
          WHERE t1.c > 5,
  @endcode

    given the execution plan with the table order t1,t2,t3,t4
    is selected, the following references will be set;
    t4->last_inner=[t4], t4->first_inner=[t4], t4->first_upper=[t2]
    t2->last_inner=[t4], t2->first_inner=t3->first_inner=[t2],
    on expression (t1.a=t2.a AND t1.b=t3.b) will be attached to 
    *t2->on_expr_ref, while t3.a=t4.a will be attached to *t4->on_expr_ref.

  @param join   reference to the info fully describing the query

  @note
    The function assumes that the simplification procedure has been
    already applied to the join query (see simplify_joins).
    This function can be called only after the execution plan
    has been chosen.
*/

static bool
make_outerjoin_info(JOIN *join)
{
  DBUG_ENTER("make_outerjoin_info");
  
  /*
    Create temp. tables for merged SJ-Materialization nests. We need to do
    this now, because further code relies on tab->table and
    tab->table->pos_in_table_list being set.
  */
  JOIN_TAB *tab;
  for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    if (tab->bush_children)
    {
      if (setup_sj_materialization_part1(tab))
        DBUG_RETURN(TRUE);
      tab->table->reginfo.join_tab= tab;
    }
  }

  for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    TABLE *table= tab->table;
    TABLE_LIST *tbl= table->pos_in_table_list;
    TABLE_LIST *embedding= tbl->embedding;

    if (tbl->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT))
    {
      /* 
        Table tab is the only one inner table for outer join.
        (Like table t4 for the table reference t3 LEFT JOIN t4 ON t3.a=t4.a
        is in the query above.)
      */
      tab->last_inner= tab->first_inner= tab;
      tab->on_expr_ref= &tbl->on_expr;
      tab->cond_equal= tbl->cond_equal;
      if (embedding && !embedding->is_active_sjm())
        tab->first_upper= embedding->nested_join->first_nested;
    }
    else if (!embedding)
      tab->table->reginfo.not_exists_optimize= 0;
          
    for ( ; embedding ; embedding= embedding->embedding)
    {
      if (embedding->is_active_sjm())
      {
        /* We're trying to walk out of an SJ-Materialization nest. Don't do this.  */
        break;
      }
      /* Ignore sj-nests: */
      if (!(embedding->on_expr && embedding->outer_join))
      {
        tab->table->reginfo.not_exists_optimize= 0;
        continue;
      }
      NESTED_JOIN *nested_join= embedding->nested_join;
      if (!nested_join->counter)
      {
        /* 
          Table tab is the first inner table for nested_join.
          Save reference to it in the nested join structure.
        */ 
        nested_join->first_nested= tab;
        tab->on_expr_ref= &embedding->on_expr;
        tab->cond_equal= tbl->cond_equal;
        if (embedding->embedding)
          tab->first_upper= embedding->embedding->nested_join->first_nested;
      }
      if (!tab->first_inner)  
        tab->first_inner= nested_join->first_nested;
      if (++nested_join->counter < nested_join->n_tables)
        break;
      /* Table tab is the last inner table for nested join. */
      nested_join->first_nested->last_inner= tab;
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  @brief
    Build a temporary join prefix condition for JOIN_TABs up to the last tab

  @param  ret  OUT  the condition is returned here

  @return
     false  OK
     true   Out of memory

  @detail
    Walk through the join prefix (from the first table to the last_tab) and
    build a condition:

    join_tab_1_cond AND join_tab_2_cond AND ... AND last_tab_conds

    The condition is only intended to be used by the range optimizer, so:
    - it is not normalized (can have Item_cond_and inside another
      Item_cond_and)
    - it does not include join->exec_const_cond and other similar conditions.
*/

bool build_tmp_join_prefix_cond(JOIN *join, JOIN_TAB *last_tab, Item **ret)
{
  THD *const thd= join->thd;
  Item_cond_and *all_conds= NULL;

  Item *res= NULL;

  // Pick the ON-expression. Use the same logic as in get_sargable_cond():
  if (last_tab->on_expr_ref)
    res= *last_tab->on_expr_ref;
  else if (last_tab->table->pos_in_table_list &&
           last_tab->table->pos_in_table_list->embedding &&
           !last_tab->table->pos_in_table_list->embedding->sj_on_expr)
  {
    res= last_tab->table->pos_in_table_list->embedding->on_expr;
  }

  for (JOIN_TAB *tab= first_depth_first_tab(join);
       tab;
       tab= next_depth_first_tab(join, tab))
  {
    if (tab->select_cond)
    {
      if (!res)
        res= tab->select_cond;
      else
      {
        if (!all_conds)
        {
          if (!(all_conds= new (thd->mem_root)Item_cond_and(thd, res,
                                                            tab->select_cond)))
            return true;
          res= all_conds;
        }
        else
          all_conds->add(tab->select_cond, thd->mem_root);
      }
    }
    if (tab == last_tab)
      break;
  }
  *ret= all_conds? all_conds: res;
  return false;
}


static bool
make_join_select(JOIN *join,SQL_SELECT *select,COND *cond)
{
  THD *thd= join->thd;
  DBUG_ENTER("make_join_select");
  if (select)
  {
    add_not_null_conds(join);
    table_map used_tables;
    /*
      Step #1: Extract constant condition
       - Extract and check the constant part of the WHERE 
       - Extract constant parts of ON expressions from outer 
         joins and attach them appropriately.
    */
    if (cond)                /* Because of QUICK_GROUP_MIN_MAX_SELECT */
    {                        /* there may be a select without a cond. */    
      if (join->table_count > 1)
        cond->update_used_tables();		// Tablenr may have changed

      /*
        Extract expressions that depend on constant tables
        1. Const part of the join's WHERE clause can be checked immediately
           and if it is not satisfied then the join has empty result
        2. Constant parts of outer joins' ON expressions must be attached 
           there inside the triggers.
      */
      {						// Check const tables
        join->exec_const_cond=
	  make_cond_for_table(thd, cond,
                              join->const_table_map,
                              (table_map) 0, -1, FALSE, FALSE);
        /* Add conditions added by add_not_null_conds(). */
        for (uint i= 0 ; i < join->const_tables ; i++)
          add_cond_and_fix(thd, &join->exec_const_cond,
                           join->join_tab[i].select_cond);

        DBUG_EXECUTE("where",print_where(join->exec_const_cond,"constants",
					 QT_ORDINARY););
        if (join->exec_const_cond && !join->exec_const_cond->is_expensive() &&
            !join->exec_const_cond->val_int())
        {
          DBUG_PRINT("info",("Found impossible WHERE condition"));
          join->exec_const_cond= NULL;
          DBUG_RETURN(1);	 // Impossible const condition
        }

        if (join->table_count != join->const_tables)
        {
          COND *outer_ref_cond= make_cond_for_table(thd, cond,
                                                    join->const_table_map |
                                                    OUTER_REF_TABLE_BIT,
                                                    OUTER_REF_TABLE_BIT,
                                                    -1, FALSE, FALSE);
          if (outer_ref_cond)
          {
            add_cond_and_fix(thd, &outer_ref_cond, join->outer_ref_cond);
            join->outer_ref_cond= outer_ref_cond;
          }
        }
        else
        {
          COND *pseudo_bits_cond=
            make_cond_for_table(thd, cond,
                                join->const_table_map |
                                PSEUDO_TABLE_BITS,
                                PSEUDO_TABLE_BITS,
                                -1, FALSE, FALSE);
          if (pseudo_bits_cond)
          {
            add_cond_and_fix(thd, &pseudo_bits_cond,
                             join->pseudo_bits_cond);
            join->pseudo_bits_cond= pseudo_bits_cond;
          }
        }
      }
    }

    /*
      Step #2: Extract WHERE/ON parts
    */
    uint i;
    for (i= join->top_join_tab_count - 1; i >= join->const_tables; i--)
    {
      if (!join->join_tab[i].bush_children)
        break;
    }
    uint last_top_base_tab_idx= i;

    table_map save_used_tables= 0;
    used_tables=((select->const_tables=join->const_table_map) |
		 OUTER_REF_TABLE_BIT | RAND_TABLE_BIT);
    JOIN_TAB *tab;
    table_map current_map;
    i= join->const_tables;
    for (tab= first_depth_first_tab(join); tab;
         tab= next_depth_first_tab(join, tab))
    {
      bool is_hj;

      /*
        first_inner is the X in queries like:
        SELECT * FROM t1 LEFT OUTER JOIN (t2 JOIN t3) ON X
      */
      JOIN_TAB *first_inner_tab= tab->first_inner;

      if (!tab->bush_children)
        current_map= tab->table->map;
      else
        current_map= tab->bush_children->start->emb_sj_nest->sj_inner_tables;

      bool use_quick_range=0;
      COND *tmp;

      /* 
        Tables that are within SJ-Materialization nests cannot have their
        conditions referring to preceding non-const tables.
         - If we're looking at the first SJM table, reset used_tables
           to refer to only allowed tables
      */
      if (tab->emb_sj_nest && tab->emb_sj_nest->sj_mat_info && 
          tab->emb_sj_nest->sj_mat_info->is_used &&
          !(used_tables & tab->emb_sj_nest->sj_inner_tables))
      {
        save_used_tables= used_tables;
        used_tables= join->const_table_map | OUTER_REF_TABLE_BIT | 
                     RAND_TABLE_BIT;
      }

      used_tables|=current_map;

      if (tab->type == JT_REF && tab->quick &&
	  (((uint) tab->ref.key == tab->quick->index &&
	    tab->ref.key_length < tab->quick->max_used_key_length) ||
           (!is_hash_join_key_no(tab->ref.key) &&
            tab->table->intersect_keys.is_set(tab->ref.key))))
      {
	/* Range uses longer key;  Use this instead of ref on key */
	tab->type=JT_ALL;
	use_quick_range=1;
	tab->use_quick=1;
        tab->ref.key= -1;
	tab->ref.key_parts=0;		// Don't use ref key.
	join->best_positions[i].records_read= rows2double(tab->quick->records);
        /* 
          We will use join cache here : prevent sorting of the first
          table only and sort at the end.
        */
        if (i != join->const_tables &&
            join->table_count > join->const_tables + 1 &&
            join->best_positions[i].use_join_buffer)
          join->full_join= 1;
      }

      tmp= NULL;

      if (cond)
      {
        if (tab->bush_children)
        {
          // Reached the materialization tab
          tmp= make_cond_after_sjm(thd, cond, cond, save_used_tables,
                                   used_tables, /*inside_or_clause=*/FALSE);
          used_tables= save_used_tables | used_tables;
          save_used_tables= 0;
        }
        else
        {
          tmp= make_cond_for_table(thd, cond, used_tables, current_map, i,
                                   FALSE, FALSE);
          if (tab == join->join_tab + last_top_base_tab_idx)
          {
            /*
              This pushes conjunctive conditions of WHERE condition such that:
              - their used_tables() contain RAND_TABLE_BIT
              - the conditions does not refer to any fields
              (such like rand() > 0.5)
            */
            table_map rand_table_bit= (table_map) RAND_TABLE_BIT;
            COND *rand_cond= make_cond_for_table(thd, cond, used_tables,
                                                 rand_table_bit, -1,
                                                 FALSE, FALSE);
            add_cond_and_fix(thd, &tmp, rand_cond);
          }
        }
        /* Add conditions added by add_not_null_conds(). */
        if (tab->select_cond)
          add_cond_and_fix(thd, &tmp, tab->select_cond);
      }

      is_hj= (tab->type == JT_REF || tab->type == JT_EQ_REF) &&
             (join->allowed_join_cache_types & JOIN_CACHE_HASHED_BIT) &&
	     ((join->max_allowed_join_cache_level+1)/2 == 2 ||
              ((join->max_allowed_join_cache_level+1)/2 > 2 &&
	       is_hash_join_key_no(tab->ref.key))) &&
              (!tab->emb_sj_nest ||                     
               join->allowed_semijoin_with_cache) && 
              (!(tab->table->map & join->outer_join) ||
               join->allowed_outer_join_with_cache);

      if (cond && !tmp && tab->quick)
      {						// Outer join
        if (tab->type != JT_ALL && !is_hj)
        {
          /*
            Don't use the quick method
            We come here in the case where we have 'key=constant' and
            the test is removed by make_cond_for_table()
          */
          delete tab->quick;
          tab->quick= 0;
        }
        else
        {
          /*
            Hack to handle the case where we only refer to a table
            in the ON part of an OUTER JOIN. In this case we want the code
            below to check if we should use 'quick' instead.
          */
          DBUG_PRINT("info", ("Item_int"));
          tmp= new (thd->mem_root) Item_int(thd, (longlong) 1, 1); // Always true
        }

      }
      if (tmp || !cond || tab->type == JT_REF || tab->type == JT_REF_OR_NULL ||
          tab->type == JT_EQ_REF || first_inner_tab)
      {
        DBUG_EXECUTE("where",print_where(tmp, 
                                         tab->table? tab->table->alias.c_ptr() :"sjm-nest",
                                         QT_ORDINARY););
	SQL_SELECT *sel= tab->select= ((SQL_SELECT*)
                                       thd->memdup((uchar*) select,
                                                   sizeof(*select)));
	if (!sel)
	  DBUG_RETURN(1);			// End of memory
        /*
          If tab is an inner table of an outer join operation,
          add a match guard to the pushed down predicate.
          The guard will turn the predicate on only after
          the first match for outer tables is encountered.
	*/        
        if (cond && tmp)
        {
          /*
            Because of QUICK_GROUP_MIN_MAX_SELECT there may be a select without
            a cond, so neutralize the hack above.
          */
          COND *tmp_cond;
          if (!(tmp_cond= add_found_match_trig_cond(thd, first_inner_tab, tmp,
                                                    0)))
            DBUG_RETURN(1);
          sel->cond= tmp_cond;
          tab->set_select_cond(tmp_cond, __LINE__);
          /* Push condition to storage engine if this is enabled
             and the condition is not guarded */
          if (tab->table)
          {
            tab->table->file->pushed_cond= NULL;
            if ((tab->table->file->ha_table_flags() &
                  HA_CAN_TABLE_CONDITION_PUSHDOWN) &&
                !first_inner_tab)
            {
              COND *push_cond= 
              make_cond_for_table(thd, tmp_cond, current_map, current_map,
                                  -1, FALSE, FALSE);
              if (push_cond)
              {
                /* Push condition to handler */
                if (!tab->table->file->cond_push(push_cond))
                  tab->table->file->pushed_cond= push_cond;
              }
            }
          }
        }
        else
        {
          sel->cond= NULL;
          tab->set_select_cond(NULL, __LINE__);
        }

	sel->head=tab->table;
        DBUG_EXECUTE("where",
                     print_where(tmp, 
                                 tab->table ? tab->table->alias.c_ptr() :
                                   "(sjm-nest)",
                                 QT_ORDINARY););
	if (tab->quick)
	{
	  /* Use quick key read if it's a constant and it's not used
	     with key reading */
          if ((tab->needed_reg.is_clear_all() && tab->type != JT_EQ_REF &&
              tab->type != JT_FT &&
              ((tab->type != JT_CONST && tab->type != JT_REF) ||
               (uint) tab->ref.key == tab->quick->index)) || is_hj)
          {
            DBUG_ASSERT(tab->quick->is_valid());
	    sel->quick=tab->quick;		// Use value from get_quick_...
	    sel->quick_keys.clear_all();
	    sel->needed_reg.clear_all();
	  }
	  else
	  {
	    delete tab->quick;
	  }
	  tab->quick=0;
	}
	uint ref_key= sel->head? (uint) sel->head->reginfo.join_tab->ref.key+1 : 0;
	if (i == join->const_tables && ref_key)
	{
	  if (!tab->const_keys.is_clear_all() &&
              tab->table->reginfo.impossible_range)
	    DBUG_RETURN(1);
	}
	else if (tab->type == JT_ALL && ! use_quick_range)
	{
	  if (!tab->const_keys.is_clear_all() &&
	      tab->table->reginfo.impossible_range)
	    DBUG_RETURN(1);				// Impossible range
	  /*
	    We plan to scan all rows.
	    Check again if we should use an index.

            There are two cases:
            1) There could be an index usage the refers to a previous
               table that we didn't consider before, but could be consider
               now as a "last resort". For example
               SELECT * from t1,t2 where t1.a between t2.a and t2.b;
            2) If the current table is the first non const table
               and there is a limit it still possibly beneficial
               to use the index even if the index range is big as
               we can stop when we've found limit rows.

            (1) - Don't switch the used index if we are using semi-join
                  LooseScan on this table. Using different index will not
                  produce the desired ordering and de-duplication.
	  */

	  if (!tab->table->is_filled_at_execution() &&
              !tab->loosescan_match_tab &&              // (1)
              ((cond && (!tab->keys.is_subset(tab->const_keys) && i > 0)) ||
               (!tab->const_keys.is_clear_all() && i == join->const_tables &&
                join->unit->select_limit_cnt <
                join->best_positions[i].records_read &&
                !(join->select_options & OPTION_FOUND_ROWS))))
	  {
	    /* Join with outer join condition */
	    COND *orig_cond=sel->cond;

            if (build_tmp_join_prefix_cond(join, tab, &sel->cond))
              return true;

	    /*
              We can't call sel->cond->fix_fields,
              as it will break tab->on_expr if it's AND condition
              (fix_fields currently removes extra AND/OR levels).
              Yet attributes of the just built condition are not needed.
              Thus we call sel->cond->quick_fix_field for safety.
	    */
	    if (sel->cond && !sel->cond->fixed)
	      sel->cond->quick_fix_field();

	    if (sel->test_quick_select(thd, tab->keys,
				       ((used_tables & ~ current_map) |
                                        OUTER_REF_TABLE_BIT),
				       (join->select_options &
					OPTION_FOUND_ROWS ?
					HA_POS_ERROR :
					join->unit->select_limit_cnt), 0,
                                        FALSE, FALSE) < 0)
            {
	      /*
		Before reporting "Impossible WHERE" for the whole query
		we have to check isn't it only "impossible ON" instead
	      */
              sel->cond=orig_cond;
              if (!*tab->on_expr_ref ||
                  sel->test_quick_select(thd, tab->keys,
                                         used_tables & ~ current_map,
                                         (join->select_options &
                                          OPTION_FOUND_ROWS ?
                                          HA_POS_ERROR :
                                          join->unit->select_limit_cnt),0,
                                          FALSE, FALSE) < 0)
		DBUG_RETURN(1);			// Impossible WHERE
            }
            else
	      sel->cond=orig_cond;

	    /* Fix for EXPLAIN */
	    if (sel->quick)
	      join->best_positions[i].records_read= (double)sel->quick->records;
	  }
	  else
	  {
	    sel->needed_reg=tab->needed_reg;
	  }
	  sel->quick_keys= tab->table->quick_keys;
	  if (!sel->quick_keys.is_subset(tab->checked_keys) ||
              !sel->needed_reg.is_subset(tab->checked_keys))
	  {
            /*
              "Range checked for each record" is a "last resort" access method
              that should only be used when the other option is a cross-product
              join.

              We use the following condition (it's approximate):
              1. There are potential keys for (sel->needed_reg)
              2. There were no possible ways to construct a quick select, or
                 the quick select would be more expensive than the full table
                 scan.
            */
	    tab->use_quick= (!sel->needed_reg.is_clear_all() &&
			     (sel->quick_keys.is_clear_all() ||
                              (sel->quick && 
                               sel->quick->read_time > 
                               tab->table->file->scan_time() + 
                               tab->table->file->stats.records/TIME_FOR_COMPARE
                               ))) ?
	      2 : 1;
	    sel->read_tables= used_tables & ~current_map;
            sel->quick_keys.clear_all();
	  }
	  if (i != join->const_tables && tab->use_quick != 2 &&
              !tab->first_inner)
	  {					/* Read with cache */
            /*
              TODO: the execution also gets here when we will not be using
              join buffer. Review these cases and perhaps, remove this call.
              (The final decision whether to use join buffer is made in
              check_join_cache_usage, so we should only call make_scan_filter()
              there, too).
            */
            if (tab->make_scan_filter())
              DBUG_RETURN(1);
          }
	}
      }
      
      /* 
        Push down conditions from all ON expressions.
        Each of these conditions are guarded by a variable
        that turns if off just before null complemented row for
        outer joins is formed. Thus, the condition from an
        'on expression' are guaranteed not to be checked for
        the null complemented row.
      */ 

      /* 
        First push down constant conditions from ON expressions. 
         - Each pushed-down condition is wrapped into trigger which is 
           enabled only for non-NULL-complemented record
         - The condition is attached to the first_inner_table.
        
        With regards to join nests:
         - if we start at top level, don't walk into nests
         - if we start inside a nest, stay within that nest.
      */
      JOIN_TAB *start_from= tab->bush_root_tab? 
                               tab->bush_root_tab->bush_children->start : 
                               join->join_tab + join->const_tables;
      JOIN_TAB *end_with= tab->bush_root_tab? 
                               tab->bush_root_tab->bush_children->end : 
                               join->join_tab + join->top_join_tab_count;
      for (JOIN_TAB *join_tab= start_from;
           join_tab != end_with;
           join_tab++)
      {
        if (*join_tab->on_expr_ref)
        {
          JOIN_TAB *cond_tab= join_tab->first_inner;
          COND *tmp_cond= make_cond_for_table(thd, *join_tab->on_expr_ref,
                                              join->const_table_map,
                                              (table_map) 0, -1, FALSE, FALSE);
          if (!tmp_cond)
            continue;
          tmp_cond= new (thd->mem_root) Item_func_trig_cond(thd, tmp_cond,
                                            &cond_tab->not_null_compl);
          if (!tmp_cond)
            DBUG_RETURN(1);
          tmp_cond->quick_fix_field();
          cond_tab->select_cond= !cond_tab->select_cond ? tmp_cond :
                                 new (thd->mem_root) Item_cond_and(thd, cond_tab->select_cond,
                                                   tmp_cond);
          if (!cond_tab->select_cond)
	    DBUG_RETURN(1);
          cond_tab->select_cond->quick_fix_field();
          cond_tab->select_cond->update_used_tables();
          if (cond_tab->select)
            cond_tab->select->cond= cond_tab->select_cond; 
        }       
      }


      /* Push down non-constant conditions from ON expressions */
      JOIN_TAB *last_tab= tab;

      /*
        while we're inside of an outer join and last_tab is 
        the last of its tables ... 
      */
      while (first_inner_tab && first_inner_tab->last_inner == last_tab)
      { 
        /* 
          Table tab is the last inner table of an outer join.
          An on expression is always attached to it.
	*/     
        COND *on_expr= *first_inner_tab->on_expr_ref;

        table_map used_tables2= (join->const_table_map |
                                 OUTER_REF_TABLE_BIT | RAND_TABLE_BIT);

        start_from= tab->bush_root_tab? 
                      tab->bush_root_tab->bush_children->start : 
                      join->join_tab + join->const_tables;
        for (JOIN_TAB *inner_tab= start_from;
             inner_tab <= last_tab;
             inner_tab++)
        {
          DBUG_ASSERT(inner_tab->table);
          current_map= inner_tab->table->map;
          used_tables2|= current_map;
          /*
            psergey: have put the -1 below. It's bad, will need to fix it.
          */
          COND *tmp_cond= make_cond_for_table(thd, on_expr, used_tables2,
                                              current_map,
                                              /*(inner_tab - first_tab)*/ -1,
					      FALSE, FALSE);
          if (tab == last_tab)
          {
            /*
              This pushes conjunctive conditions of ON expression of an outer
              join such that:
              - their used_tables() contain RAND_TABLE_BIT
              - the conditions does not refer to any fields
              (such like rand() > 0.5)
            */
            table_map rand_table_bit= (table_map) RAND_TABLE_BIT;
            COND *rand_cond= make_cond_for_table(thd, on_expr, used_tables2,
                                                 rand_table_bit, -1,
                                                 FALSE, FALSE);
            add_cond_and_fix(thd, &tmp_cond, rand_cond);
          }
          bool is_sjm_lookup_tab= FALSE;
          if (inner_tab->bush_children)
          {
            /*
              'inner_tab' is an SJ-Materialization tab, i.e. we have a join
              order like this:

                ot1 sjm_tab LEFT JOIN ot2 ot3
                         ^          ^
                   'tab'-+          +--- left join we're adding triggers for

              LEFT JOIN's ON expression may not have references to subquery
              columns.  The subquery was in the WHERE clause, so IN-equality 
              is in the WHERE clause, also.
              However, equality propagation code may have propagated the
              IN-equality into ON expression, and we may get things like

                subquery_inner_table=const

              in the ON expression. We must not check such conditions during
              SJM-lookup, because 1) subquery_inner_table has no valid current
              row (materialization temp.table has it instead), and 2) they
              would be true anyway.
            */
            SJ_MATERIALIZATION_INFO *sjm=
              inner_tab->bush_children->start->emb_sj_nest->sj_mat_info;
            if (sjm->is_used && !sjm->is_sj_scan)
              is_sjm_lookup_tab= TRUE;
          }

          if (inner_tab == first_inner_tab && inner_tab->on_precond &&
              !is_sjm_lookup_tab)
            add_cond_and_fix(thd, &tmp_cond, inner_tab->on_precond);
          if (tmp_cond && !is_sjm_lookup_tab)
          {
            JOIN_TAB *cond_tab=  (inner_tab < first_inner_tab ?
                                  first_inner_tab : inner_tab);
            Item **sel_cond_ref= (inner_tab < first_inner_tab ?
                                  &first_inner_tab->on_precond :
                                  &inner_tab->select_cond);
            /*
              First add the guards for match variables of
              all embedding outer join operations.
	    */
            if (!(tmp_cond= add_found_match_trig_cond(thd,
                                                     cond_tab->first_inner,
                                                     tmp_cond,
                                                     first_inner_tab)))
              DBUG_RETURN(1);
            /* 
              Now add the guard turning the predicate off for 
              the null complemented row.
	    */ 
            DBUG_PRINT("info", ("Item_func_trig_cond"));
            tmp_cond= new (thd->mem_root) Item_func_trig_cond(thd, tmp_cond,
                                              &first_inner_tab->
                                              not_null_compl);
            DBUG_PRINT("info", ("Item_func_trig_cond %p",
                                tmp_cond));
            if (tmp_cond)
              tmp_cond->quick_fix_field();
	    /* Add the predicate to other pushed down predicates */
            DBUG_PRINT("info", ("Item_cond_and"));
            *sel_cond_ref= !(*sel_cond_ref) ? 
                             tmp_cond :
                             new (thd->mem_root) Item_cond_and(thd, *sel_cond_ref, tmp_cond);
            DBUG_PRINT("info", ("Item_cond_and %p",
                                (*sel_cond_ref)));
            if (!(*sel_cond_ref))
              DBUG_RETURN(1);
            (*sel_cond_ref)->quick_fix_field();
            (*sel_cond_ref)->update_used_tables();
            if (cond_tab->select)
              cond_tab->select->cond= cond_tab->select_cond;
          }
        }
        first_inner_tab= first_inner_tab->first_upper;       
      }
      if (!tab->bush_children)
        i++;
    }
  }
  DBUG_RETURN(0);
}


static
uint get_next_field_for_derived_key(uchar *arg)
{
  KEYUSE *keyuse= *(KEYUSE **) arg;
  if (!keyuse)
    return (uint) (-1);
  TABLE *table= keyuse->table;
  uint key= keyuse->key;
  uint fldno= keyuse->keypart; 
  uint keypart= keyuse->keypart_map == (key_part_map) 1 ?
                                         0 : (keyuse-1)->keypart+1;
  for ( ; 
        keyuse->table == table && keyuse->key == key && keyuse->keypart == fldno;
        keyuse++)
    keyuse->keypart= keypart;
  if (keyuse->key != key)
    keyuse= 0;
  *((KEYUSE **) arg)= keyuse;
  return fldno;
}


static
uint get_next_field_for_derived_key_simple(uchar *arg)
{
  KEYUSE *keyuse= *(KEYUSE **) arg;
  if (!keyuse)
    return (uint) (-1);
  TABLE *table= keyuse->table;
  uint key= keyuse->key;
  uint fldno= keyuse->keypart;
  for ( ;
        keyuse->table == table && keyuse->key == key && keyuse->keypart == fldno;
        keyuse++)
    ;
  if (keyuse->key != key)
    keyuse= 0;
  *((KEYUSE **) arg)= keyuse;
  return fldno;
}

static 
bool generate_derived_keys_for_table(KEYUSE *keyuse, uint count, uint keys)
{
  TABLE *table= keyuse->table;
  if (table->alloc_keys(keys))
    return TRUE;
  uint key_count= 0;
  KEYUSE *first_keyuse= keyuse;
  uint prev_part= keyuse->keypart;
  uint parts= 0;
  uint i= 0;

  for ( ; i < count && key_count < keys; )
  {
    do
    {
      keyuse->key= table->s->keys;
      keyuse->keypart_map= (key_part_map) (1 << parts);     
      keyuse++;
      i++;
    } 
    while (i < count && keyuse->used_tables == first_keyuse->used_tables &&
           keyuse->keypart == prev_part);
    parts++;
    if (i < count && keyuse->used_tables == first_keyuse->used_tables)
    {
      prev_part= keyuse->keypart;
    }
    else
    {
      KEYUSE *save_first_keyuse= first_keyuse;
      if (table->check_tmp_key(table->s->keys, parts,
                               get_next_field_for_derived_key_simple,
                               (uchar *) &first_keyuse))

      {
        first_keyuse= save_first_keyuse;
        if (table->add_tmp_key(table->s->keys, parts, 
                               get_next_field_for_derived_key, 
                               (uchar *) &first_keyuse,
                               FALSE))
          return TRUE;
        table->reginfo.join_tab->keys.set_bit(table->s->keys);
      }
      else
      {
        /* Mark keyuses for this key to be excluded */
        for (KEYUSE *curr=save_first_keyuse; curr < keyuse; curr++)
	{
          curr->key= MAX_KEY;
        }
      }
      first_keyuse= keyuse;
      key_count++;
      parts= 0;
      prev_part= keyuse->keypart;
    }
  }             

  return FALSE;
}
   

static
bool generate_derived_keys(DYNAMIC_ARRAY *keyuse_array)
{
  KEYUSE *keyuse= dynamic_element(keyuse_array, 0, KEYUSE*);
  uint elements= keyuse_array->elements;
  TABLE *prev_table= 0;
  for (uint i= 0; i < elements; i++, keyuse++)
  {
    if (!keyuse->table)
      break;
    KEYUSE *first_table_keyuse= NULL;
    table_map last_used_tables= 0;
    uint count= 0;
    uint keys= 0;
    TABLE_LIST *derived= NULL;
    if (keyuse->table != prev_table)
      derived= keyuse->table->pos_in_table_list;
    while (derived && derived->is_materialized_derived())
    {
      if (keyuse->table != prev_table)
      {
        prev_table= keyuse->table;
        while (keyuse->table == prev_table && keyuse->key != MAX_KEY)
	{
          keyuse++;
          i++;
        }
        if (keyuse->table != prev_table)
	{
          keyuse--;
          i--;
          derived= NULL;
          continue;
        }
        first_table_keyuse= keyuse;
        last_used_tables= keyuse->used_tables;
        count= 0;
        keys= 0;
      }
      else if (keyuse->used_tables != last_used_tables)
      {
        keys++;
        last_used_tables= keyuse->used_tables;
      }
      count++;
      keyuse++;
      i++;
      if (keyuse->table != prev_table)
      {
        if (generate_derived_keys_for_table(first_table_keyuse, count, ++keys))
          return TRUE;
        keyuse--;
        i--;
	derived= NULL;
      }
    }
  }
  return FALSE;
}


/*
  @brief
  Drops unused keys for each materialized derived table/view

  @details
  For materialized derived tables only ref access can be used, it employs
  only one index, thus we don't need the rest. For each materialized derived
  table/view call TABLE::use_index to save one index chosen by the optimizer
  and free others. No key is chosen then all keys will be dropped.
*/

void JOIN::drop_unused_derived_keys()
{
  JOIN_TAB *tab;
  for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
  {
    
    TABLE *tmp_tbl= tab->table;
    if (!tmp_tbl)
      continue;
    if (!tmp_tbl->pos_in_table_list->is_materialized_derived())
      continue;
    if (tmp_tbl->max_keys > 1 && !tab->is_ref_for_hash_join())
      tmp_tbl->use_index(tab->ref.key);
    if (tmp_tbl->s->keys)
    {
      if (tab->ref.key >= 0 && tab->ref.key < MAX_KEY)
        tab->ref.key= 0;
      else
        tmp_tbl->s->keys= 0;
    }
    tab->keys= (key_map) (tmp_tbl->s->keys ? 1 : 0);
  }
}


/*
  Evaluate the bitmap of used tables for items from the select list
*/

inline void JOIN::eval_select_list_used_tables()
{
  select_list_used_tables= 0;
  Item *item;
  List_iterator_fast<Item> it(fields_list);
  while ((item= it++))
  {
    select_list_used_tables|= item->used_tables();
  }
  Item_outer_ref *ref;
  List_iterator_fast<Item_outer_ref> ref_it(select_lex->inner_refs_list);
  while ((ref= ref_it++))
  {
    item= ref->outer_ref;
    select_list_used_tables|= item->used_tables();
  }
}


/*
  Determine {after which table we'll produce ordered set} 

  SYNOPSIS
    make_join_orderinfo()
     join

   
  DESCRIPTION 
    Determine if the set is already ordered for ORDER BY, so it can 
    disable join cache because it will change the ordering of the results.
    Code handles sort table that is at any location (not only first after 
    the const tables) despite the fact that it's currently prohibited.
    We must disable join cache if the first non-const table alone is
    ordered. If there is a temp table the ordering is done as a last
    operation and doesn't prevent join cache usage.

  RETURN
    Number of table after which the set will be ordered
    join->tables if we don't need an ordered set 
*/

static uint make_join_orderinfo(JOIN *join)
{
  /*
    This function needs to be fixed to take into account that we now have SJM
    nests.
  */
  DBUG_ASSERT(0);

  JOIN_TAB *tab;
  if (join->need_tmp)
    return join->table_count;
  tab= join->get_sort_by_join_tab();
  return tab ? (uint)(tab-join->join_tab) : join->table_count;
}

/*
  Deny usage of join buffer for the specified table

  SYNOPSIS
    set_join_cache_denial()
      tab    join table for which join buffer usage is to be denied  
     
  DESCRIPTION
    The function denies usage of join buffer when joining the table 'tab'.
    The table is marked as not employing any join buffer. If a join cache
    object has been already allocated for the table this object is destroyed.

  RETURN
    none    
*/

static
void set_join_cache_denial(JOIN_TAB *join_tab)
{
  if (join_tab->cache)
  {
    /* 
      If there is a previous cache linked to this cache through the
      next_cache pointer: remove the link. 
    */
    if (join_tab->cache->prev_cache)
      join_tab->cache->prev_cache->next_cache= 0;
    /*
      Same for the next_cache
    */
    if (join_tab->cache->next_cache)
      join_tab->cache->next_cache->prev_cache= 0;

    join_tab->cache->free();
    join_tab->cache= 0;
  }
  if (join_tab->use_join_cache)
  {
    join_tab->use_join_cache= FALSE;
    join_tab->used_join_cache_level= 0;
    /*
      It could be only sub_select(). It could not be sub_seject_sjm because we
      don't do join buffering for the first table in sjm nest. 
    */
    join_tab[-1].next_select= sub_select;
    if (join_tab->type == JT_REF && join_tab->is_ref_for_hash_join())
    {
      join_tab->type= JT_ALL;
      join_tab->ref.key_parts= 0;
    }
    join_tab->join->return_tab= join_tab;
  }
}


/**
  The default implementation of unlock-row method of READ_RECORD,
  used in all access methods.
*/

void rr_unlock_row(st_join_table *tab)
{
  READ_RECORD *info= &tab->read_record;
  info->table->file->unlock_row();
}


/**
  Pick the appropriate access method functions

  Sets the functions for the selected table access method

  @param      tab               Table reference to put access method
*/

static void
pick_table_access_method(JOIN_TAB *tab)
{
  switch (tab->type) 
  {
  case JT_REF:
    tab->read_first_record= join_read_always_key;
    tab->read_record.read_record_func= join_read_next_same;
    break;

  case JT_REF_OR_NULL:
    tab->read_first_record= join_read_always_key_or_null;
    tab->read_record.read_record_func= join_read_next_same_or_null;
    break;

  case JT_CONST:
    tab->read_first_record= join_read_const;
    tab->read_record.read_record_func= join_no_more_records;
    break;

  case JT_EQ_REF:
    tab->read_first_record= join_read_key;
    tab->read_record.read_record_func= join_no_more_records;
    break;

  case JT_FT:
    tab->read_first_record= join_ft_read_first;
    tab->read_record.read_record_func= join_ft_read_next;
    break;

  case JT_SYSTEM:
    tab->read_first_record= join_read_system;
    tab->read_record.read_record_func= join_no_more_records;
    break;

  /* keep gcc happy */  
  default:
    break;  
  }
}


/* 
  Revise usage of join buffer for the specified table and the whole nest   

  SYNOPSIS
    revise_cache_usage()
      tab    join table for which join buffer usage is to be revised  

  DESCRIPTION
    The function revise the decision to use a join buffer for the table 'tab'.
    If this table happened to be among the inner tables of a nested outer join/
    semi-join the functions denies usage of join buffers for all of them

  RETURN
    none    
*/

static
void revise_cache_usage(JOIN_TAB *join_tab)
{
  JOIN_TAB *tab;
  JOIN_TAB *first_inner;

  if (join_tab->first_inner)
  {
    JOIN_TAB *end_tab= join_tab;
    for (first_inner= join_tab->first_inner; 
         first_inner;
         first_inner= first_inner->first_upper)           
    {
      for (tab= end_tab; tab >= first_inner; tab--)
        set_join_cache_denial(tab);
      end_tab= first_inner;
    }
  }
  else if (join_tab->first_sj_inner_tab)
  {
    first_inner= join_tab->first_sj_inner_tab;
    for (tab= join_tab; tab >= first_inner; tab--)
    {
      set_join_cache_denial(tab);
    }
  }
  else set_join_cache_denial(join_tab);
}


/*
  end_select-compatible function that writes the record into a sjm temptable
  
  SYNOPSIS
    end_sj_materialize()
      join            The join 
      join_tab        Points to right after the last join_tab in materialization bush
      end_of_records  FALSE <=> This call is made to pass another record 
                                combination
                      TRUE  <=> EOF (no action)

  DESCRIPTION
    This function is used by semi-join materialization to capture suquery's
    resultset and write it into the temptable (that is, materialize it).

  NOTE
    This function is used only for semi-join materialization. Non-semijoin
    materialization uses different mechanism.

  RETURN 
    NESTED_LOOP_OK
    NESTED_LOOP_ERROR
*/

enum_nested_loop_state 
end_sj_materialize(JOIN *join, JOIN_TAB *join_tab, bool end_of_records)
{
  int error;
  THD *thd= join->thd;
  SJ_MATERIALIZATION_INFO *sjm= join_tab[-1].emb_sj_nest->sj_mat_info;
  DBUG_ENTER("end_sj_materialize");
  if (!end_of_records)
  {
    TABLE *table= sjm->table;

    List_iterator<Item> it(sjm->sjm_table_cols);
    Item *item;
    while ((item= it++))
    {
      if (item->is_null())
        DBUG_RETURN(NESTED_LOOP_OK);
    }
    fill_record(thd, table, table->field, sjm->sjm_table_cols, TRUE, FALSE);
    if (unlikely(thd->is_error()))
      DBUG_RETURN(NESTED_LOOP_ERROR); /* purecov: inspected */
    if (unlikely((error= table->file->ha_write_tmp_row(table->record[0]))))
    {
      /* create_myisam_from_heap will generate error if needed */
      if (table->file->is_fatal_error(error, HA_CHECK_DUP) &&
          create_internal_tmp_table_from_heap(thd, table,
                                              sjm->sjm_table_param.start_recinfo, 
                                              &sjm->sjm_table_param.recinfo, error, 1, NULL))
        DBUG_RETURN(NESTED_LOOP_ERROR); /* purecov: inspected */
    }
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/* 
  Check whether a join buffer can be used to join the specified table   

  SYNOPSIS
    check_join_cache_usage()
      tab                 joined table to check join buffer usage for
      options             options of the join
      no_jbuf_after       don't use join buffering after table with this number
      prev_tab            previous join table

  DESCRIPTION
    The function finds out whether the table 'tab' can be joined using a join
    buffer. This check is performed after the best execution plan for 'join'
    has been chosen. If the function decides that a join buffer can be employed
    then it selects the most appropriate join cache object that contains this
    join buffer.
    The result of the check and the type of the the join buffer to be used
    depend on:
      - the access method to access rows of the joined table
      - whether the join table is an inner table of an outer join or semi-join
      - whether the optimizer switches
          outer_join_with_cache, semijoin_with_cache, join_cache_incremental,
          join_cache_hashed, join_cache_bka,
        are set on or off
      - the join cache level set for the query
      - the join 'options'.

    In any case join buffer is not used if the number of the joined table is
    greater than 'no_jbuf_after'. It's also never used if the value of
    join_cache_level is equal to 0.
    If the optimizer switch outer_join_with_cache is off no join buffer is
    used for outer join operations.
    If the optimizer switch semijoin_with_cache is off no join buffer is used
    for semi-join operations.
    If the optimizer switch join_cache_incremental is off no incremental join
    buffers are used.
    If the optimizer switch join_cache_hashed is off then the optimizer uses
    neither BNLH algorithm, nor BKAH algorithm to perform join operations.

    If the optimizer switch join_cache_bka is off then the optimizer uses
    neither BKA algorithm, nor BKAH algorithm to perform join operation.
    The valid settings for join_cache_level lay in the interval 0..8.
    If it set to 0 no join buffers are used to perform join operations.
    Currently we differentiate between join caches of 8 levels:
      1 : non-incremental join cache used for BNL join algorithm
      2 : incremental join cache used for BNL join algorithm
      3 : non-incremental join cache used for BNLH join algorithm
      4 : incremental join cache used for BNLH join algorithm
      5 : non-incremental join cache used for BKA join algorithm
      6 : incremental join cache used for BKA join algorithm 
      7 : non-incremental join cache used for BKAH join algorithm 
      8 : incremental join cache used for BKAH join algorithm
    If the value of join_cache_level is set to n then no join caches of
    levels higher than n can be employed.

    If the optimizer switches outer_join_with_cache, semijoin_with_cache,
    join_cache_incremental, join_cache_hashed, join_cache_bka are all on
    the following rules are applied.
    If join_cache_level==1|2 then join buffer is used for inner joins, outer
    joins and semi-joins with 'JT_ALL' access method. In this case a
    JOIN_CACHE_BNL object is employed.
    If join_cache_level==3|4 and then join buffer is used for a join operation
    (inner join, outer join, semi-join) with 'JT_REF'/'JT_EQREF' access method
    then a JOIN_CACHE_BNLH object is employed. 
    If an index is used to access rows of the joined table and the value of
    join_cache_level==5|6 then a JOIN_CACHE_BKA object is employed. 
    If an index is used to access rows of the joined table and the value of
    join_cache_level==7|8 then a JOIN_CACHE_BKAH object is employed. 
    If the value of join_cache_level is odd then creation of a non-linked 
    join cache is forced.

    Currently for any join operation a join cache of the  level of the
    highest allowed and applicable level is used.
    For example, if join_cache_level is set to 6 and the optimizer switch
    join_cache_bka is off, while the optimizer switch join_cache_hashed is
    on then for any inner join operation with JT_REF/JT_EQREF access method
    to the joined table the BNLH join algorithm will be used, while for
    the table accessed by the JT_ALL methods the BNL algorithm will be used.

    If the function decides that a join buffer can be used to join the table
    'tab' then it sets the value of tab->use_join_buffer to TRUE and assigns
    the selected join cache object to the field 'cache' of the previous
    join table. 
    If the function creates a join cache object it tries to initialize it. The
    failure to do this results in an invocation of the function that destructs
    the created object.
    If the function decides that but some reasons no join buffer can be used
    for a table it calls the function revise_cache_usage that checks
    whether join cache should be denied for some previous tables. In this case
    a pointer to the first table for which join cache usage has been denied
    is passed in join->return_val (see the function set_join_cache_denial).
    
    The functions changes the value the fields tab->icp_other_tables_ok and
    tab->idx_cond_fact_out to FALSE if the chosen join cache algorithm 
    requires it.
 
  NOTES
    An inner table of a nested outer join or a nested semi-join can be currently
    joined only when a linked cache object is employed. In these cases setting
    join_cache_incremental to 'off' results in denial of usage of any join
    buffer when joining the table.
    For a nested outer join/semi-join, currently, we either use join buffers for
    all inner tables or for none of them. 
    Some engines (e.g. Falcon) currently allow to use only a join cache
    of the type JOIN_CACHE_BKAH when the joined table is accessed through
    an index. For these engines setting the value of join_cache_level to 5 or 6
    results in that no join buffer is used to join the table. 
  
  RETURN VALUE
    cache level if cache is used, otherwise returns 0

  TODO
    Support BKA inside SJ-Materialization nests. When doing this, we'll need
    to only store sj-inner tables in the join buffer.
#if 0
        JOIN_TAB *first_tab= join->join_tab+join->const_tables;
        uint n_tables= i-join->const_tables;
        / *
          We normally put all preceding tables into the join buffer, except
          for the constant tables.
          If we're inside a semi-join materialization nest, e.g.

             outer_tbl1  outer_tbl2  ( inner_tbl1, inner_tbl2 ) ...
                                                       ^-- we're here

          then we need to put into the join buffer only the tables from
          within the nest.
        * /
        if (i >= first_sjm_table && i < last_sjm_table)
        {
          n_tables= i - first_sjm_table; // will be >0 if we got here
          first_tab= join->join_tab + first_sjm_table;
        }
#endif
*/

static
uint check_join_cache_usage(JOIN_TAB *tab,
                            ulonglong options,
                            uint no_jbuf_after,
                            uint table_index,
                            JOIN_TAB *prev_tab)
{
  Cost_estimate cost;
  uint flags= 0;
  ha_rows rows= 0;
  uint bufsz= 4096;
  JOIN_CACHE *prev_cache=0;
  JOIN *join= tab->join;
  MEM_ROOT *root= join->thd->mem_root;
  uint cache_level= tab->used_join_cache_level;
  bool force_unlinked_cache=
         !(join->allowed_join_cache_types & JOIN_CACHE_INCREMENTAL_BIT);
  bool no_hashed_cache=
         !(join->allowed_join_cache_types & JOIN_CACHE_HASHED_BIT);
  bool no_bka_cache= 
         !(join->allowed_join_cache_types & JOIN_CACHE_BKA_BIT);

  join->return_tab= 0;

  /*
    Don't use join cache if @@join_cache_level==0 or this table is the first
    one join suborder (either at top level or inside a bush)
  */
  if (cache_level == 0 || !prev_tab)
    return 0;

  if (force_unlinked_cache && (cache_level%2 == 0))
    cache_level--;

  if (options & SELECT_NO_JOIN_CACHE)
    goto no_join_cache;

  if (tab->use_quick == 2)
    goto no_join_cache;

  if (tab->table->map & join->complex_firstmatch_tables)
    goto no_join_cache;
  
  /*
    Don't use join cache if we're inside a join tab range covered by LooseScan
    strategy (TODO: LooseScan is very similar to FirstMatch so theoretically it 
    should be possible to use join buffering in the same way we're using it for
    multi-table firstmatch ranges).
  */
  if (tab->inside_loosescan_range)
    goto no_join_cache;

  if (tab->is_inner_table_of_semijoin() &&
      !join->allowed_semijoin_with_cache)
    goto no_join_cache;
  if (tab->is_inner_table_of_outer_join() &&
      !join->allowed_outer_join_with_cache)
    goto no_join_cache;

  /*
    Non-linked join buffers can't guarantee one match
  */
  if (tab->is_nested_inner())
  {
    if (force_unlinked_cache || cache_level == 1)
      goto no_join_cache;
    if (cache_level & 1)
      cache_level--;
  }
    
  /*
    Don't use BKA for materialized tables. We could actually have a
    meaningful use of BKA when linked join buffers are used.

    The problem is, the temp.table is not filled (actually not even opened
    properly) yet, and this doesn't let us call
    handler->multi_range_read_info(). It is possible to come up with
    estimates, etc. without acessing the table, but it seems not to worth the
    effort now.
  */
  if (tab->table->pos_in_table_list->is_materialized_derived())
  {
    no_bka_cache= true;
    /*
      Don't use hash join algorithm if the temporary table for the rows
      of the derived table will be created with an equi-join key.
    */
    if (tab->table->s->keys)
      no_hashed_cache= true;
  }

  /*
    Don't use join buffering if we're dictated not to by no_jbuf_after
    (This is not meaningfully used currently)
  */
  if (table_index > no_jbuf_after)
    goto no_join_cache;
  
  /*
    TODO: BNL join buffer should be perfectly ok with tab->bush_children.
  */
  if (tab->loosescan_match_tab || tab->bush_children)
    goto no_join_cache;

  for (JOIN_TAB *first_inner= tab->first_inner; first_inner;
       first_inner= first_inner->first_upper)
  {
    if (first_inner != tab && 
        (!first_inner->use_join_cache || !(tab-1)->use_join_cache))
      goto no_join_cache;
  }
  if (tab->first_sj_inner_tab && tab->first_sj_inner_tab != tab &&
      (!tab->first_sj_inner_tab->use_join_cache || !(tab-1)->use_join_cache))
    goto no_join_cache;
  if (!prev_tab->use_join_cache)
  {
    /* 
      Check whether table tab and the previous one belong to the same nest of
      inner tables and if so do not use join buffer when joining table tab. 
    */
    if (tab->first_inner && tab != tab->first_inner)
    {
      for (JOIN_TAB *first_inner= tab[-1].first_inner;
           first_inner;
           first_inner= first_inner->first_upper)
      {
        if (first_inner == tab->first_inner)
          goto no_join_cache;
      }
    }
    else if (tab->first_sj_inner_tab && tab != tab->first_sj_inner_tab &&
             tab->first_sj_inner_tab == tab[-1].first_sj_inner_tab)
      goto no_join_cache; 
  }       

  prev_cache= prev_tab->cache;

  switch (tab->type) {
  case JT_ALL:
    if (cache_level == 1)
      prev_cache= 0;
    if ((tab->cache= new (root) JOIN_CACHE_BNL(join, tab, prev_cache)))
    {
      tab->icp_other_tables_ok= FALSE;
      /* If make_join_select() hasn't called make_scan_filter(), do it now */
      if (!tab->cache_select && tab->make_scan_filter())
        goto no_join_cache;
      return (2 - MY_TEST(!prev_cache));
    }
    goto no_join_cache;
  case JT_SYSTEM:
  case JT_CONST:
  case JT_REF:
  case JT_EQ_REF:
    if (cache_level <=2 || (no_hashed_cache && no_bka_cache))
      goto no_join_cache;
    if (tab->ref.is_access_triggered())
      goto no_join_cache;

    if (!tab->is_ref_for_hash_join() && !no_bka_cache)
    {
      flags= HA_MRR_NO_NULL_ENDPOINTS | HA_MRR_SINGLE_POINT;
      if (tab->table->covering_keys.is_set(tab->ref.key))
        flags|= HA_MRR_INDEX_ONLY;
      rows= tab->table->file->multi_range_read_info(tab->ref.key, 10, 20,
                                                    tab->ref.key_parts,
                                                    &bufsz, &flags, &cost);
    }

    if ((cache_level <=4 && !no_hashed_cache) || no_bka_cache ||
        tab->is_ref_for_hash_join() ||
	((flags & HA_MRR_NO_ASSOCIATION) && cache_level <=6))
    {
      if (!tab->hash_join_is_possible() ||
          tab->make_scan_filter())
        goto no_join_cache;
      if (cache_level == 3)
        prev_cache= 0;
      if ((tab->cache= new (root) JOIN_CACHE_BNLH(join, tab, prev_cache)))
      {
        tab->icp_other_tables_ok= FALSE;        
        return (4 - MY_TEST(!prev_cache));
      }
      goto no_join_cache;
    }
    if (cache_level > 4 && no_bka_cache)
      goto no_join_cache;
    
    if ((flags & HA_MRR_NO_ASSOCIATION) &&
	(cache_level <= 6 || no_hashed_cache))
      goto no_join_cache;

    if ((rows != HA_POS_ERROR) && !(flags & HA_MRR_USE_DEFAULT_IMPL))
    {
      if (cache_level <= 6 || no_hashed_cache)
      {
        if (cache_level == 5)
          prev_cache= 0;
        if ((tab->cache= new (root) JOIN_CACHE_BKA(join, tab, flags, prev_cache)))
          return (6 - MY_TEST(!prev_cache));
        goto no_join_cache;
      }
      else
      {
        if (cache_level == 7)
          prev_cache= 0;
        if ((tab->cache= new (root) JOIN_CACHE_BKAH(join, tab, flags, prev_cache)))
	{
          tab->idx_cond_fact_out= FALSE;
          return (8 - MY_TEST(!prev_cache));
        }
        goto no_join_cache;
      }
    }
    goto no_join_cache;
  default : ;
  }

no_join_cache:
  if (tab->type != JT_ALL && tab->is_ref_for_hash_join())
  {
    tab->type= JT_ALL;
    tab->ref.key_parts= 0;
  }
  revise_cache_usage(tab); 
  return 0;
}


/* 
  Check whether join buffers can be used to join tables of a join   

  SYNOPSIS
    check_join_cache_usage()
      join                join whose tables are to be checked             
      options             options of the join
      no_jbuf_after       don't use join buffering after table with this number
                          (The tables are assumed to be numbered in
                          first_linear_tab(join, WITHOUT_CONST_TABLES),
                          next_linear_tab(join, WITH_CONST_TABLES) order).

  DESCRIPTION
    For each table after the first non-constant table the function checks
    whether the table can be joined using a join buffer. If the function decides
    that a join buffer can be employed then it selects the most appropriate join
    cache object that contains this join buffer whose level is not greater
    than join_cache_level set for the join. To make this check the function
    calls the function check_join_cache_usage for every non-constant table.

  NOTES
    In some situations (e.g. for nested outer joins, for nested semi-joins) only
    incremental buffers can be used. If it turns out that for some inner table
    no join buffer can be used then any inner table of an outer/semi-join nest
    cannot use join buffer. In the case when already chosen buffer must be
    denied for a table the function recalls check_join_cache_usage()
    starting from this table. The pointer to the table from which the check
    has to be restarted is returned in join->return_val (see the description
    of check_join_cache_usage).
*/

void check_join_cache_usage_for_tables(JOIN *join, ulonglong options,
                                       uint no_jbuf_after)
{
  JOIN_TAB *tab;
  JOIN_TAB *prev_tab;

  for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    tab->used_join_cache_level= join->max_allowed_join_cache_level;  
  }

  uint idx= join->const_tables;
  for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
restart:
    tab->icp_other_tables_ok= TRUE;
    tab->idx_cond_fact_out= TRUE;
    
    /* 
      Check if we have a preceding join_tab, as something that will feed us
      records that we could buffer. We don't have it, if 
       - this is the first non-const table in the join order,
       - this is the first table inside an SJM nest.
    */
    prev_tab= tab - 1;
    if (tab == join->join_tab + join->const_tables ||
        (tab->bush_root_tab && tab->bush_root_tab->bush_children->start == tab))
      prev_tab= NULL;

    switch (tab->type) {
    case JT_SYSTEM:
    case JT_CONST:
    case JT_EQ_REF:
    case JT_REF:
    case JT_REF_OR_NULL:
    case JT_ALL:
      tab->used_join_cache_level= check_join_cache_usage(tab, options,
                                                         no_jbuf_after,
                                                         idx,
                                                         prev_tab);
      tab->use_join_cache= MY_TEST(tab->used_join_cache_level);
      /*
        psergey-merge: todo: raise the question that this is really stupid that
        we can first allocate a join buffer, then decide not to use it and free
        it.
      */
      if (join->return_tab)
      {
        tab= join->return_tab;
        goto restart;
      }
      break; 
    default:
      tab->used_join_cache_level= 0;
    }
    if (!tab->bush_children)
      idx++;
  }
}

/**
  Remove pushdown conditions that are already checked by the scan phase
  of BNL/BNLH joins.

  @note
  If the single-table condition for this table will be used by a
  blocked join to pre-filter this table's rows, there is no need
  to re-check the same single-table condition for each joined record.

  This method removes from JOIN_TAB::select_cond and JOIN_TAB::select::cond
  all top-level conjuncts that also appear in in JOIN_TAB::cache_select::cond.
*/

void JOIN_TAB::remove_redundant_bnl_scan_conds()
{
  if (!(select_cond && cache_select && cache &&
        (cache->get_join_alg() == JOIN_CACHE::BNL_JOIN_ALG ||
         cache->get_join_alg() == JOIN_CACHE::BNLH_JOIN_ALG)))
    return;

  /*
    select->cond is not processed separately. This method assumes it is always
    the same as select_cond.
  */
  if (select && select->cond != select_cond)
    return;

  if (is_cond_and(select_cond))
  {
    List_iterator<Item> pushed_cond_li(*((Item_cond*) select_cond)->argument_list());
    Item *pushed_item;
    Item_cond_and *reduced_select_cond= new (join->thd->mem_root)
      Item_cond_and(join->thd);

    if (is_cond_and(cache_select->cond))
    {
      List_iterator<Item> scan_cond_li(*((Item_cond*) cache_select->cond)->argument_list());
      Item *scan_item;
      while ((pushed_item= pushed_cond_li++))
      {
        bool found_cond= false;
        scan_cond_li.rewind();
        while ((scan_item= scan_cond_li++))
        {
          if (pushed_item->eq(scan_item, 0))
          {
            found_cond= true;
            break;
          }
        }
        if (!found_cond)
          reduced_select_cond->add(pushed_item, join->thd->mem_root);
      }
    }
    else
    {
      while ((pushed_item= pushed_cond_li++))
      {
        if (!pushed_item->eq(cache_select->cond, 0))
          reduced_select_cond->add(pushed_item, join->thd->mem_root);
      }
    }

    /*
      JOIN_CACHE::check_match uses JOIN_TAB::select->cond instead of
      JOIN_TAB::select_cond. set_cond() sets both pointers.
    */
    if (reduced_select_cond->argument_list()->is_empty())
      set_cond(NULL);
    else if (reduced_select_cond->argument_list()->elements == 1)
      set_cond(reduced_select_cond->argument_list()->head());
    else
    {
      reduced_select_cond->quick_fix_field();
      set_cond(reduced_select_cond);
    }
  }
  else if (select_cond->eq(cache_select->cond, 0))
    set_cond(NULL);
}


/*
  Plan refinement stage: do various setup things for the executor

  SYNOPSIS
    make_join_readinfo()
      join           Join being processed
      options        Join's options (checking for SELECT_DESCRIBE, 
                     SELECT_NO_JOIN_CACHE)
      no_jbuf_after  Don't use join buffering after table with this number.

  DESCRIPTION
    Plan refinement stage: do various set ups for the executioner
      - set up use of join buffering
      - push index conditions
      - increment relevant counters
      - etc

  RETURN 
    FALSE - OK
    TRUE  - Out of memory
*/

static bool
make_join_readinfo(JOIN *join, ulonglong options, uint no_jbuf_after)
{
  JOIN_TAB *tab;
  uint i;
  DBUG_ENTER("make_join_readinfo");

  bool statistics= MY_TEST(!(join->select_options & SELECT_DESCRIBE));
  bool sorted= 1;

  join->complex_firstmatch_tables= table_map(0);

  if (!join->select_lex->sj_nests.is_empty() &&
      setup_semijoin_dups_elimination(join, options, no_jbuf_after))
    DBUG_RETURN(TRUE); /* purecov: inspected */
  
  /* For const tables, set partial_join_cardinality to 1. */
  for (tab= join->join_tab; tab != join->join_tab + join->const_tables; tab++)
    tab->partial_join_cardinality= 1; 

  JOIN_TAB *prev_tab= NULL;
  i= join->const_tables;
  for (tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       prev_tab=tab, tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    /*
      The approximation below for partial join cardinality is not good because
        - it does not take into account some pushdown predicates
        - it does not differentiate between inner joins, outer joins and
        semi-joins.
      Later it should be improved.
    */

    if (tab->bush_root_tab && tab->bush_root_tab->bush_children->start == tab)
      prev_tab= NULL;
    DBUG_ASSERT(tab->bush_children || tab->table == join->best_positions[i].table->table);

    tab->partial_join_cardinality= join->best_positions[i].records_read *
                                   (prev_tab? prev_tab->partial_join_cardinality : 1);
    if (!tab->bush_children)
      i++;
  }
 
  check_join_cache_usage_for_tables(join, options, no_jbuf_after);
  
  JOIN_TAB *first_tab;
  for (tab= first_tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITHOUT_CONST_TABLES);
       tab; 
       tab= next_linear_tab(join, tab, WITH_BUSH_ROOTS))
  {
    if (tab->bush_children)
    {
      if (setup_sj_materialization_part2(tab))
        return TRUE;
    }

    TABLE *table=tab->table;
    uint jcl= tab->used_join_cache_level;
    tab->read_record.table= table;
    tab->read_record.unlock_row= rr_unlock_row;
    tab->sorted= sorted;
    sorted= 0;                                  // only first must be sorted
    

    /*
      We should not set tab->next_select for the last table in the
      SMJ-nest, as setup_sj_materialization() has already set it to
      end_sj_materialize.
    */
    if (!(tab->bush_root_tab && 
          tab->bush_root_tab->bush_children->end == tab + 1))
    {
      tab->next_select=sub_select;		/* normal select */
    }


    if (tab->loosescan_match_tab)
    {
      if (!(tab->loosescan_buf= (uchar*)join->thd->alloc(tab->
                                                         loosescan_key_len)))
        return TRUE; /* purecov: inspected */
      tab->sorted= TRUE;
    }
    table->status=STATUS_NO_RECORD;
    pick_table_access_method (tab);

    if (jcl)
       tab[-1].next_select=sub_select_cache;

    if (tab->cache && tab->cache->get_join_alg() == JOIN_CACHE::BNLH_JOIN_ALG)
      tab->type= JT_HASH;
      
    switch (tab->type) {
    case JT_SYSTEM:				// Only happens with left join 
    case JT_CONST:				// Only happens with left join
      /* Only happens with outer joins */
      tab->read_first_record= tab->type == JT_SYSTEM ? join_read_system
                                                     : join_read_const;
      if (table->covering_keys.is_set(tab->ref.key) && !table->no_keyread)
        table->file->ha_start_keyread(tab->ref.key);
      else if ((!jcl || jcl > 4) && !tab->ref.is_access_triggered())
        push_index_cond(tab, tab->ref.key);
      break;
    case JT_EQ_REF:
      tab->read_record.unlock_row= join_read_key_unlock_row;
      /* fall through */
      if (table->covering_keys.is_set(tab->ref.key) && !table->no_keyread)
        table->file->ha_start_keyread(tab->ref.key);
      else if ((!jcl || jcl > 4) && !tab->ref.is_access_triggered())
        push_index_cond(tab, tab->ref.key);
      break;
    case JT_REF_OR_NULL:
    case JT_REF:
      if (tab->select)
      {
	delete tab->select->quick;
	tab->select->quick=0;
      }
      delete tab->quick;
      tab->quick=0;
      if (table->covering_keys.is_set(tab->ref.key) && !table->no_keyread)
        table->file->ha_start_keyread(tab->ref.key);
      else if ((!jcl || jcl > 4) && !tab->ref.is_access_triggered())
        push_index_cond(tab, tab->ref.key);
      break;
    case JT_ALL:
    case JT_HASH:
      /*
	If previous table use cache
        If the incoming data set is already sorted don't use cache.
        Also don't use cache if this is the first table in semi-join
          materialization nest.
      */
      /* These init changes read_record */
      if (tab->use_quick == 2)
      {
        join->thd->set_status_no_good_index_used();
	tab->read_first_record= join_init_quick_read_record;
	if (statistics)
	  join->thd->inc_status_select_range_check();
      }
      else
      {
        if (!tab->bush_children)
          tab->read_first_record= join_init_read_record;
	if (tab == first_tab)
	{
	  if (tab->select && tab->select->quick)
	  {
	    if (statistics)
	      join->thd->inc_status_select_range();
	  }
	  else
	  {
            join->thd->set_status_no_index_used();
	    if (statistics)
	    {
              join->thd->inc_status_select_scan();
	      join->thd->query_plan_flags|= QPLAN_FULL_SCAN;
	    }
	  }
	}
	else
	{
	  if (tab->select && tab->select->quick)
	  {
	    if (statistics)
              join->thd->inc_status_select_full_range_join();
	  }
	  else
	  {
            join->thd->set_status_no_index_used();
	    if (statistics)
	    {
              join->thd->inc_status_select_full_join();
	      join->thd->query_plan_flags|= QPLAN_FULL_JOIN;
	    }
	  }
	}
	if (!table->no_keyread)
	{
	  if (tab->select && tab->select->quick &&
              tab->select->quick->index != MAX_KEY && //not index_merge
	      table->covering_keys.is_set(tab->select->quick->index))
            table->file->ha_start_keyread(tab->select->quick->index);
	  else if (!table->covering_keys.is_clear_all() &&
		   !(tab->select && tab->select->quick))
	  {					// Only read index tree
            if (tab->loosescan_match_tab)
              tab->index= tab->loosescan_key;
            else 
            {
#ifdef BAD_OPTIMIZATION
              /*
                It has turned out that the below change, while speeding things
                up for disk-bound loads, slows them down for cases when the data
                is in disk cache (see BUG#35850):
                See bug #26447: "Using the clustered index for a table scan
                is always faster than using a secondary index".
              */
              if (table->s->primary_key != MAX_KEY &&
                  table->file->primary_key_is_clustered())
                tab->index= table->s->primary_key;
              else
#endif
                tab->index=find_shortest_key(table, & table->covering_keys);
            }
	    tab->read_first_record= join_read_first;
            /* Read with index_first / index_next */
	    tab->type= tab->type == JT_ALL ? JT_NEXT : JT_HASH_NEXT;		
	  }
	}
        if (tab->select && tab->select->quick &&
            tab->select->quick->index != MAX_KEY &&
            !tab->table->file->keyread_enabled())
          push_index_cond(tab, tab->select->quick->index);
      }
      break;
    case JT_FT:
      break;
      /* purecov: begin deadcode */
    default:
      DBUG_PRINT("error",("Table type %d found",tab->type));
      break;
    case JT_UNKNOWN:
    case JT_MAYBE_REF:
      abort();
      /* purecov: end */
    }

    DBUG_EXECUTE("where",
                 char buff[256];
                 String str(buff,sizeof(buff),system_charset_info);
                 str.length(0);
                 str.append(tab->table? tab->table->alias.c_ptr() :"<no_table_name>");
                 str.append(" final_pushdown_cond");
                 print_where(tab->select_cond, str.c_ptr_safe(), QT_ORDINARY););
  }
  uint n_top_tables= (uint)(join->join_tab_ranges.head()->end -  
                     join->join_tab_ranges.head()->start);

  join->join_tab[n_top_tables - 1].next_select=0;  /* Set by do_select */
  
  /*
    If a join buffer is used to join a table the ordering by an index
    for the first non-constant table cannot be employed anymore.
  */
  for (tab= join->join_tab + join->const_tables ; 
       tab != join->join_tab + n_top_tables ; tab++)
  {
    if (tab->use_join_cache)
    {
       JOIN_TAB *sort_by_tab= join->group && join->simple_group &&
                              join->group_list ?
			       join->join_tab+join->const_tables :
                               join->get_sort_by_join_tab();
      /*
        It could be that sort_by_tab==NULL, and the plan is to use filesort()
        on the first table.
      */
      if (join->order)
      {
        join->simple_order= 0;
        join->need_tmp= 1;
      }

      if (join->group && !join->group_optimized_away)
      {
        join->need_tmp= 1;
        join->simple_group= 0;
      }
      
      if (sort_by_tab)
      {
        join->need_tmp= 1;
        join->simple_order= join->simple_group= 0;
        if (sort_by_tab->type == JT_NEXT && 
            !sort_by_tab->table->covering_keys.is_set(sort_by_tab->index))
        {
          sort_by_tab->type= JT_ALL;
          sort_by_tab->read_first_record= join_init_read_record;
        }
        else if (sort_by_tab->type == JT_HASH_NEXT &&
                 !sort_by_tab->table->covering_keys.is_set(sort_by_tab->index))
        {
          sort_by_tab->type= JT_HASH;
          sort_by_tab->read_first_record= join_init_read_record;
        }
      }
      break;
    }
  }

  DBUG_RETURN(FALSE);
}


/**
  Give error if we some tables are done with a full join.

  This is used by multi_table_update and multi_table_delete when running
  in safe mode.

  @param join		Join condition

  @retval
    0	ok
  @retval
    1	Error (full join used)
*/

bool error_if_full_join(JOIN *join)
{
  for (JOIN_TAB *tab=first_top_level_tab(join, WITH_CONST_TABLES); tab;
       tab= next_top_level_tab(join, tab))
  {
    if (tab->type == JT_ALL && (!tab->select || !tab->select->quick))
    {
      my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
                 ER_THD(join->thd,
                        ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
      return(1);
    }
  }
  return(0);
}


/**
  cleanup JOIN_TAB.

  DESCRIPTION 
    This is invoked when we've finished all join executions.
*/

void JOIN_TAB::cleanup()
{
  DBUG_ENTER("JOIN_TAB::cleanup");
  
  DBUG_PRINT("enter", ("tab: %p  table %s.%s",
                       this,
                       (table ? table->s->db.str : "?"),
                       (table ? table->s->table_name.str : "?")));
  delete select;
  select= 0;
  delete quick;
  quick= 0;
  if (cache)
  {
    cache->free();
    cache= 0;
  }
  limit= 0;
  // Free select that was created for filesort outside of create_sort_index
  if (filesort && filesort->select && !filesort->own_select)
    delete filesort->select;
  delete filesort;
  filesort= NULL;
  /* Skip non-existing derived tables/views result tables */
  if (table &&
      (table->s->tmp_table != INTERNAL_TMP_TABLE || table->is_created()))
  {
    table->file->ha_end_keyread();
    table->file->ha_index_or_rnd_end();
  }
  if (table)
  {
    table->file->ha_end_keyread();
    if (type == JT_FT)
      table->file->ha_ft_end();
    else
      table->file->ha_index_or_rnd_end();
    preread_init_done= FALSE;
    if (table->pos_in_table_list && 
        table->pos_in_table_list->jtbm_subselect)
    {
      if (table->pos_in_table_list->jtbm_subselect->is_jtbm_const_tab)
      {
        /*
          Set this to NULL so that cleanup_empty_jtbm_semi_joins() doesn't
          attempt to make another free_tmp_table call.
        */
        table->pos_in_table_list->table= NULL;
        free_tmp_table(join->thd, table);
        table= NULL;
      }
      else
      {
        TABLE_LIST *tmp= table->pos_in_table_list;
        end_read_record(&read_record);
        tmp->jtbm_subselect->cleanup();
        /* 
          The above call freed the materializedd temptable. Set it to NULL so
          that we don't attempt to touch it if JOIN_TAB::cleanup() is invoked
          multiple times (it may be)
        */
        tmp->table= NULL;
        table= NULL;
      }
      DBUG_VOID_RETURN;
    }
    /*
      We need to reset this for next select
      (Tested in part_of_refkey)
    */
    table->reginfo.join_tab= 0;
  }
  end_read_record(&read_record);
  explain_plan= NULL;
  DBUG_VOID_RETURN;
}


/**
  Estimate the time to get rows of the joined table
*/

double JOIN_TAB::scan_time()
{
  double res;
  if (table->is_created())
  {
    if (table->is_filled_at_execution())
    {
      get_delayed_table_estimates(table, &records, &read_time,
                                    &startup_cost);
      found_records= records;
      table->quick_condition_rows= records;
    }
    else
    {
      found_records= records= table->stat_records();
      read_time= table->file->scan_time();
      /*
        table->quick_condition_rows has already been set to
        table->file->stats.records
      */
    }
    res= read_time;
  }
  else
  {
    found_records= records=table->stat_records();
    read_time= found_records ? (double)found_records: 10.0;// TODO:fix this stub
    res= read_time;
  }
  return res;
}


/**
  Estimate the number of rows that a an access method will read from a table.

  @todo: why not use JOIN_TAB::found_records
*/

ha_rows JOIN_TAB::get_examined_rows()
{
  double examined_rows;
  SQL_SELECT *sel= filesort? filesort->select : this->select;

  if (sel && sel->quick && use_quick != 2)
    examined_rows= (double)sel->quick->records;
  else if (type == JT_NEXT || type == JT_ALL ||
           type == JT_HASH || type ==JT_HASH_NEXT)
  {
    if (limit)
    {
      /*
        @todo This estimate is wrong, a LIMIT query may examine much more rows
        than the LIMIT itself.
      */
      examined_rows= (double)limit;
    }
    else
    {
      if (table->is_filled_at_execution())
        examined_rows= (double)records;
      else
      {
        /*
          handler->info(HA_STATUS_VARIABLE) has been called in
          make_join_statistics()
        */
        examined_rows= (double)table->stat_records();
      }
    }
  }
  else
    examined_rows= records_read;

  if (examined_rows >= (double) HA_ROWS_MAX)
    return HA_ROWS_MAX;
  return (ha_rows) examined_rows;
}


/**
  Initialize the join_tab before reading.
  Currently only derived table/view materialization is done here.

  TODO: consider moving this together with join_tab_execution_startup
*/
bool JOIN_TAB::preread_init()
{
  TABLE_LIST *derived= table->pos_in_table_list;
  DBUG_ENTER("JOIN_TAB::preread_init");

  if (!derived || !derived->is_materialized_derived())
  {
    preread_init_done= TRUE;
    DBUG_RETURN(FALSE);
  }

  /* Materialize derived table/view. */
  if ((!derived->get_unit()->executed  ||
       derived->is_recursive_with_table() ||
       derived->get_unit()->uncacheable) &&
      mysql_handle_single_derived(join->thd->lex,
                                    derived, DT_CREATE | DT_FILL))
    DBUG_RETURN(TRUE);

  if (!(derived->get_unit()->uncacheable & UNCACHEABLE_DEPENDENT) ||
      derived->is_nonrecursive_derived_with_rec_ref())
    preread_init_done= TRUE;
  if (select && select->quick)
    select->quick->replace_handler(table->file);

  DBUG_EXECUTE_IF("show_explain_probe_join_tab_preread", 
                  if (dbug_user_var_equals_int(join->thd, 
                                               "show_explain_probe_select_id", 
                                               join->select_lex->select_number))
                        dbug_serve_apcs(join->thd, 1);
                 );

  /* init ftfuns for just initialized derived table */
  if (table->fulltext_searched)
    if (init_ftfuncs(join->thd, join->select_lex, MY_TEST(join->order)))
      DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


/**
  Build a TABLE_REF structure for index lookup in the temporary table

  @param thd             Thread handle
  @param tmp_key         The temporary table key
  @param it              The iterator of items for lookup in the key
  @param skip            Number of fields from the beginning to skip

  @details
  Build TABLE_REF object for lookup in the key 'tmp_key' using items
  accessible via item iterator 'it'.

  @retval TRUE  Error
  @retval FALSE OK
*/

bool TABLE_REF::tmp_table_index_lookup_init(THD *thd,
                                            KEY *tmp_key,
                                            Item_iterator &it,
                                            bool value,
                                            uint skip)
{
  uint tmp_key_parts= tmp_key->user_defined_key_parts;
  uint i;
  DBUG_ENTER("TABLE_REF::tmp_table_index_lookup_init");

  key= 0; /* The only temp table index. */
  key_length= tmp_key->key_length;
  if (!(key_buff=
        (uchar*) thd->calloc(ALIGN_SIZE(tmp_key->key_length) * 2)) ||
      !(key_copy=
        (store_key**) thd->alloc((sizeof(store_key*) *
                                  (tmp_key_parts + 1)))) ||
      !(items=
        (Item**) thd->alloc(sizeof(Item*) * tmp_key_parts)))
    DBUG_RETURN(TRUE);

  key_buff2= key_buff + ALIGN_SIZE(tmp_key->key_length);

  KEY_PART_INFO *cur_key_part= tmp_key->key_part;
  store_key **ref_key= key_copy;
  uchar *cur_ref_buff= key_buff;

  it.open();
  for (i= 0; i < skip; i++) it.next();
  for (i= 0; i < tmp_key_parts; i++, cur_key_part++, ref_key++)
  {
    Item *item= it.next();
    DBUG_ASSERT(item);
    items[i]= item;
    int null_count= MY_TEST(cur_key_part->field->real_maybe_null());
    *ref_key= new store_key_item(thd, cur_key_part->field,
                                 /* TIMOUR:
                                    the NULL byte is taken into account in
                                    cur_key_part->store_length, so instead of
                                    cur_ref_buff + MY_TEST(maybe_null), we could
                                    use that information instead.
                                 */
                                 cur_ref_buff + null_count,
                                 null_count ? cur_ref_buff : 0,
                                 cur_key_part->length, items[i], value);
    cur_ref_buff+= cur_key_part->store_length;
  }
  *ref_key= NULL; /* End marker. */
  key_err= 1;
  key_parts= tmp_key_parts;
  DBUG_RETURN(FALSE);
}


/*
  Check if ref access uses "Full scan on NULL key" (i.e. it actually alternates
  between ref access and full table scan)
*/

bool TABLE_REF::is_access_triggered()
{
  for (uint i = 0; i < key_parts; i++)
  {
    if (cond_guards[i])
      return TRUE;
  }
  return FALSE;
}


/**
  Partially cleanup JOIN after it has executed: close index or rnd read
  (table cursors), free quick selects.

    This function is called in the end of execution of a JOIN, before the used
    tables are unlocked and closed.

    For a join that is resolved using a temporary table, the first sweep is
    performed against actual tables and an intermediate result is inserted
    into the temprorary table.
    The last sweep is performed against the temporary table. Therefore,
    the base tables and associated buffers used to fill the temporary table
    are no longer needed, and this function is called to free them.

    For a join that is performed without a temporary table, this function
    is called after all rows are sent, but before EOF packet is sent.

    For a simple SELECT with no subqueries this function performs a full
    cleanup of the JOIN and calls mysql_unlock_read_tables to free used base
    tables.

    If a JOIN is executed for a subquery or if it has a subquery, we can't
    do the full cleanup and need to do a partial cleanup only.
    - If a JOIN is not the top level join, we must not unlock the tables
    because the outer select may not have been evaluated yet, and we
    can't unlock only selected tables of a query.
    - Additionally, if this JOIN corresponds to a correlated subquery, we
    should not free quick selects and join buffers because they will be
    needed for the next execution of the correlated subquery.
    - However, if this is a JOIN for a [sub]select, which is not
    a correlated subquery itself, but has subqueries, we can free it
    fully and also free JOINs of all its subqueries. The exception
    is a subquery in SELECT list, e.g: @n
    SELECT a, (select MY_MAX(b) from t1) group by c @n
    This subquery will not be evaluated at first sweep and its value will
    not be inserted into the temporary table. Instead, it's evaluated
    when selecting from the temporary table. Therefore, it can't be freed
    here even though it's not correlated.

  @todo
    Unlock tables even if the join isn't top level select in the tree
*/

void JOIN::join_free()
{
  SELECT_LEX_UNIT *tmp_unit;
  SELECT_LEX *sl;
  /*
    Optimization: if not EXPLAIN and we are done with the JOIN,
    free all tables.
  */
  bool full= !(select_lex->uncacheable) &&  !(thd->lex->describe);
  bool can_unlock= full;
  DBUG_ENTER("JOIN::join_free");

  cleanup(full);

  for (tmp_unit= select_lex->first_inner_unit();
       tmp_unit;
       tmp_unit= tmp_unit->next_unit())
  {
    if (tmp_unit->with_element && tmp_unit->with_element->is_recursive)
      continue;
    for (sl= tmp_unit->first_select(); sl; sl= sl->next_select())
    {
      Item_subselect *subselect= sl->master_unit()->item;
      bool full_local= full && (!subselect || subselect->is_evaluated());
      /*
        If this join is evaluated, we can fully clean it up and clean up all
        its underlying joins even if they are correlated -- they will not be
        used any more anyway.
        If this join is not yet evaluated, we still must clean it up to
        close its table cursors -- it may never get evaluated, as in case of
        ... HAVING FALSE OR a IN (SELECT ...))
        but all table cursors must be closed before the unlock.
      */
      sl->cleanup_all_joins(full_local);
      /* Can't unlock if at least one JOIN is still needed */
      can_unlock= can_unlock && full_local;
    }
  }
  /*
    We are not using tables anymore
    Unlock all tables. We may be in an INSERT .... SELECT statement.
  */
  if (can_unlock && lock && thd->lock && ! thd->locked_tables_mode &&
      !(select_options & SELECT_NO_UNLOCK) &&
      !select_lex->subquery_in_having &&
      (select_lex == (thd->lex->unit.fake_select_lex ?
                      thd->lex->unit.fake_select_lex : &thd->lex->select_lex)))
  {
    /*
      TODO: unlock tables even if the join isn't top level select in the
      tree.
    */
    mysql_unlock_read_tables(thd, lock);           // Don't free join->lock
    lock= 0;
  }

  DBUG_VOID_RETURN;
}


/**
  Free resources of given join.

  @param full   true if we should free all resources, call with full==1
                should be last, before it this function can be called with
                full==0

  @note
    With subquery this function definitely will be called several times,
    but even for simple query it can be called several times.
*/

void JOIN::cleanup(bool full)
{
  DBUG_ENTER("JOIN::cleanup");
  DBUG_PRINT("enter", ("full %u", (uint) full));
  
  if (full)
    have_query_plan= QEP_DELETED;

  if (original_join_tab)
  {
    /* Free the original optimized join created for the group_by_handler */
    join_tab= original_join_tab;
    original_join_tab= 0;
    table_count= original_table_count;
  }

  if (join_tab)
  {
    JOIN_TAB *tab;

    if (full)
    {
      /*
        Call cleanup() on join tabs used by the join optimization
        (join->join_tab may now be pointing to result of make_simple_join
         reading from the temporary table)

        We also need to check table_count to handle various degenerate joins
        w/o tables: they don't have some members initialized and
        WALK_OPTIMIZATION_TABS may not work correctly for them.
      */
      if (top_join_tab_count && tables_list)
      {
        for (tab= first_breadth_first_tab(); tab;
             tab= next_breadth_first_tab(first_breadth_first_tab(),
                                         top_join_tab_count, tab))
        {
          tab->cleanup();
          delete tab->filesort_result;
          tab->filesort_result= NULL;
        }
      }
      cleaned= true;
      //psergey2: added (Q: why not in the above loop?)
      {
        JOIN_TAB *curr_tab= join_tab + exec_join_tab_cnt();
        for (uint i= 0; i < aggr_tables; i++, curr_tab++)
        {
          if (curr_tab->aggr)
          {
            free_tmp_table(thd, curr_tab->table);
            delete curr_tab->tmp_table_param;
            curr_tab->tmp_table_param= NULL;
            curr_tab->aggr= NULL;

            delete curr_tab->filesort_result;
            curr_tab->filesort_result= NULL;
          }
        }
        aggr_tables= 0; // psergey3
      }
    }
    else
    {
      for (tab= first_linear_tab(this, WITH_BUSH_ROOTS, WITH_CONST_TABLES); tab;
           tab= next_linear_tab(this, tab, WITH_BUSH_ROOTS))
      {
        tab->partial_cleanup();
      }
    }
  }
  if (full)
  {
    cleanup_empty_jtbm_semi_joins(this, join_list);

    // Run Cached_item DTORs!
    group_fields.delete_elements();

    /*
      We can't call delete_elements() on copy_funcs as this will cause
      problems in free_elements() as some of the elements are then deleted.
    */
    tmp_table_param.copy_funcs.empty();
    /*
      If we have tmp_join and 'this' JOIN is not tmp_join and
      tmp_table_param.copy_field's  of them are equal then we have to remove
      pointer to  tmp_table_param.copy_field from tmp_join, because it will
      be removed in tmp_table_param.cleanup().
    */
    tmp_table_param.cleanup();

    delete pushdown_query;
    pushdown_query= 0;

    if (!join_tab)
    {
      List_iterator<TABLE_LIST> li(*join_list);
      TABLE_LIST *table_ref;
      while ((table_ref= li++))
      {
        if (table_ref->table &&
            table_ref->jtbm_subselect &&
            table_ref->jtbm_subselect->is_jtbm_const_tab)
        {
          free_tmp_table(thd, table_ref->table);
          table_ref->table= NULL;
        }
      }
    }
  }
  /* Restore ref array to original state */
  if (current_ref_ptrs != items0)
  {
    set_items_ref_array(items0);
    set_group_rpa= false;
  }
  DBUG_VOID_RETURN;
}


/**
  Remove the following expressions from ORDER BY and GROUP BY:
  Constant expressions @n
  Expression that only uses tables that are of type EQ_REF and the reference
  is in the ORDER list or if all refereed tables are of the above type.

  In the following, the X field can be removed:
  @code
  SELECT * FROM t1,t2 WHERE t1.a=t2.a ORDER BY t1.a,t2.X
  SELECT * FROM t1,t2,t3 WHERE t1.a=t2.a AND t2.b=t3.b ORDER BY t1.a,t3.X
  @endcode

  These can't be optimized:
  @code
  SELECT * FROM t1,t2 WHERE t1.a=t2.a ORDER BY t2.X,t1.a
  SELECT * FROM t1,t2 WHERE t1.a=t2.a AND t1.b=t2.b ORDER BY t1.a,t2.c
  SELECT * FROM t1,t2 WHERE t1.a=t2.a ORDER BY t2.b,t1.a
  @endcode

  TODO: this function checks ORDER::used, which can only have a value of 0.
*/

static bool
eq_ref_table(JOIN *join, ORDER *start_order, JOIN_TAB *tab)
{
  if (tab->cached_eq_ref_table)			// If cached
    return tab->eq_ref_table;
  tab->cached_eq_ref_table=1;
  /* We can skip const tables only if not an outer table */
  if (tab->type == JT_CONST && !tab->first_inner)
    return (tab->eq_ref_table=1);		/* purecov: inspected */
  if (tab->type != JT_EQ_REF || tab->table->maybe_null)
    return (tab->eq_ref_table=0);		// We must use this
  Item **ref_item=tab->ref.items;
  Item **end=ref_item+tab->ref.key_parts;
  uint found=0;
  table_map map=tab->table->map;

  for (; ref_item != end ; ref_item++)
  {
    if (! (*ref_item)->const_item())
    {						// Not a const ref
      ORDER *order;
      for (order=start_order ; order ; order=order->next)
      {
	if ((*ref_item)->eq(order->item[0],0))
	  break;
      }
      if (order)
      {
        if (!(order->used & map))
        {
          found++;
          order->used|= map;
        }
	continue;				// Used in ORDER BY
      }
      if (!only_eq_ref_tables(join,start_order, (*ref_item)->used_tables()))
	return (tab->eq_ref_table=0);
    }
  }
  /* Check that there was no reference to table before sort order */
  for (; found && start_order ; start_order=start_order->next)
  {
    if (start_order->used & map)
    {
      found--;
      continue;
    }
    if (start_order->depend_map & map)
      return (tab->eq_ref_table=0);
  }
  return tab->eq_ref_table=1;
}


static bool
only_eq_ref_tables(JOIN *join,ORDER *order,table_map tables)
{
  tables&= ~PSEUDO_TABLE_BITS;
  for (JOIN_TAB **tab=join->map2table ; tables ; tab++, tables>>=1)
  {
    if (tables & 1 && !eq_ref_table(join, order, *tab))
      return 0;
  }
  return 1;
}


/** Update the dependency map for the tables. */

static void update_depend_map(JOIN *join)
{
  JOIN_TAB *join_tab;
  for (join_tab= first_linear_tab(join, WITH_BUSH_ROOTS, WITH_CONST_TABLES); 
       join_tab;
       join_tab= next_linear_tab(join, join_tab, WITH_BUSH_ROOTS))
  {
    TABLE_REF *ref= &join_tab->ref;
    table_map depend_map=0;
    Item **item=ref->items;
    uint i;
    for (i=0 ; i < ref->key_parts ; i++,item++)
      depend_map|=(*item)->used_tables();
    depend_map&= ~OUTER_REF_TABLE_BIT;
    ref->depend_map= depend_map;
    for (JOIN_TAB **tab=join->map2table;
         depend_map ;
         tab++,depend_map>>=1 )
    {
      if (depend_map & 1)
        ref->depend_map|=(*tab)->ref.depend_map;
    }
  }
}


/** Update the dependency map for the sort order. */

static void update_depend_map_for_order(JOIN *join, ORDER *order)
{
  for (; order ; order=order->next)
  {
    table_map depend_map;
    order->item[0]->update_used_tables();
    order->depend_map=depend_map=order->item[0]->used_tables();
    order->used= 0;
    // Not item_sum(), RAND() and no reference to table outside of sub select
    if (!(order->depend_map & (OUTER_REF_TABLE_BIT | RAND_TABLE_BIT))
        && !order->item[0]->with_sum_func &&
        join->join_tab)
    {
      for (JOIN_TAB **tab=join->map2table;
	   depend_map ;
	   tab++, depend_map>>=1)
      {
	if (depend_map & 1)
	  order->depend_map|=(*tab)->ref.depend_map;
      }
    }
  }
}


/**
  Remove all constants and check if ORDER only contains simple
  expressions.

  We also remove all duplicate expressions, keeping only the first one.

  simple_order is set to 1 if sort_order only uses fields from head table
  and the head table is not a LEFT JOIN table.

  @param join			Join handler
  @param first_order		List of SORT or GROUP order
  @param cond			WHERE statement
  @param change_list		Set to 1 if we should remove things from list.
                                If this is not set, then only simple_order is
                                calculated. This is not set when we
                                are using ROLLUP
  @param simple_order		Set to 1 if we are only using simple
				expressions.

  @return
    Returns new sort order
*/

static ORDER *
remove_const(JOIN *join,ORDER *first_order, COND *cond,
             bool change_list, bool *simple_order)
{
  *simple_order= join->rollup.state == ROLLUP::STATE_NONE;
  if (join->only_const_tables())
    return change_list ? 0 : first_order;		// No need to sort

  ORDER *order,**prev_ptr, *tmp_order;
  table_map UNINIT_VAR(first_table); /* protected by first_is_base_table */
  table_map not_const_tables= ~join->const_table_map;
  table_map ref;
  bool first_is_base_table= FALSE;
  DBUG_ENTER("remove_const");
  
  /*
    Join tab is set after make_join_statistics() has been called.
    In case of one table with GROUP BY this function is called before
    join_tab is set for the GROUP_BY expression
  */
  if (join->join_tab)
  {
    if (join->join_tab[join->const_tables].table)
    {
      first_table= join->join_tab[join->const_tables].table->map;
      first_is_base_table= TRUE;
    }
  
    /*
      Cleanup to avoid interference of calls of this function for
      ORDER BY and GROUP BY
    */
    for (JOIN_TAB *tab= join->join_tab + join->const_tables;
         tab < join->join_tab + join->table_count;
         tab++)
      tab->cached_eq_ref_table= FALSE;

    *simple_order= *join->join_tab[join->const_tables].on_expr_ref ? 0 : 1;
  }
  else
  {
    first_is_base_table= FALSE;
    first_table= 0;                     // Not used, for gcc
  }

  prev_ptr= &first_order;

  /* NOTE: A variable of not_const_tables ^ first_table; breaks gcc 2.7 */

  update_depend_map_for_order(join, first_order);
  for (order=first_order; order ; order=order->next)
  {
    table_map order_tables=order->item[0]->used_tables();
    if (order->item[0]->with_sum_func ||
        order->item[0]->with_window_func ||
        /*
          If the outer table of an outer join is const (either by itself or
          after applying WHERE condition), grouping on a field from such a
          table will be optimized away and filesort without temporary table
          will be used unless we prevent that now. Filesort is not fit to
          handle joins and the join condition is not applied. We can't detect
          the case without an expensive test, however, so we force temporary
          table for all queries containing more than one table, ROLLUP, and an
          outer join.
         */
        (join->table_count > 1 && join->rollup.state == ROLLUP::STATE_INITED &&
        join->outer_join))
      *simple_order=0;				// Must do a temp table to sort
    else if (!(order_tables & not_const_tables))
    {
      if (order->item[0]->with_subquery())
      {
        /*
          Delay the evaluation of constant ORDER and/or GROUP expressions that
          contain subqueries until the execution phase.
        */
        join->exec_const_order_group_cond.push_back(order->item[0],
                                                    join->thd->mem_root);
      }
      DBUG_PRINT("info",("removing: %s", order->item[0]->full_name()));
      continue;
    }
    else
    {
      if (order_tables & (RAND_TABLE_BIT | OUTER_REF_TABLE_BIT))
	*simple_order=0;
      else
      {
	if (cond && const_expression_in_where(cond,order->item[0]))
	{
	  DBUG_PRINT("info",("removing: %s", order->item[0]->full_name()));
	  continue;
	}
	if (first_is_base_table &&
            (ref=order_tables & (not_const_tables ^ first_table)))
	{
	  if (!(order_tables & first_table) &&
              only_eq_ref_tables(join,first_order, ref))
	  {
	    DBUG_PRINT("info",("removing: %s", order->item[0]->full_name()));
	    continue;
	  }
          /*
            UseMultipleEqualitiesToRemoveTempTable:
            Can use multiple-equalities here to check that ORDER BY columns
            can be used without tmp. table.
          */
          bool can_subst_to_first_table= false;
          bool first_is_in_sjm_nest= false;
          if (first_is_base_table)
          {
            TABLE_LIST *tbl_for_first=
              join->join_tab[join->const_tables].table->pos_in_table_list;
            first_is_in_sjm_nest= tbl_for_first->sj_mat_info &&
                                  tbl_for_first->sj_mat_info->is_used;
          }
          /*
            Currently we do not employ the optimization that uses multiple
            equalities for ORDER BY to remove tmp table in the case when
            the first table happens to be the result of materialization of
            a semi-join nest ( <=> first_is_in_sjm_nest == true).

            When a semi-join nest is materialized and scanned to look for
            possible matches in the remaining tables for every its row
            the fields from the result of materialization are copied
            into the record buffers of tables from the semi-join nest.
            So these copies are used to access the remaining tables rather
            than the fields from the result of materialization.

            Unfortunately now this so-called 'copy back' technique is
            supported only if the rows  are scanned with the rr_sequential
            function, but not with other rr_* functions that are employed
            when the result of materialization is required to be sorted.

            TODO: either to support 'copy back' technique for the above case,
                  or to get rid of this technique altogether.
          */
          if (optimizer_flag(join->thd, OPTIMIZER_SWITCH_ORDERBY_EQ_PROP) &&
              first_is_base_table && !first_is_in_sjm_nest &&
              order->item[0]->real_item()->type() == Item::FIELD_ITEM &&
              join->cond_equal)
          {
            table_map first_table_bit=
              join->join_tab[join->const_tables].table->map;

            Item *item= order->item[0];

            /*
              TODO: equality substitution in the context of ORDER BY is 
              sometimes allowed when it is not allowed in the general case.
              
              We make the below call for its side effect: it will locate the
              multiple equality the item belongs to and set item->item_equal
              accordingly.
            */
            Item *res= item->propagate_equal_fields(join->thd,
                                                    Value_source::
                                                    Context_identity(),
                                                    join->cond_equal);
            Item_equal *item_eq;
            if ((item_eq= res->get_item_equal()))
            {
              Item *first= item_eq->get_first(NO_PARTICULAR_TAB, NULL);
              if (first->const_item() || first->used_tables() ==
                                         first_table_bit)
              {
                can_subst_to_first_table= true;
              }
            }
          }

          if (!can_subst_to_first_table)
          {
            *simple_order=0;			// Must do a temp table to sort
          }
	}
      }
    }
    /* Remove ORDER BY entries that we have seen before */
    for (tmp_order= first_order;
         tmp_order != order;
         tmp_order= tmp_order->next)
    {
      if (tmp_order->item[0]->eq(order->item[0],1))
        break;
    }
    if (tmp_order != order)
      continue;                                // Duplicate order by. Remove
    
    if (change_list)
      *prev_ptr= order;				// use this entry
    prev_ptr= &order->next;
  }
  if (change_list)
    *prev_ptr=0;
  if (prev_ptr == &first_order)			// Nothing to sort/group
    *simple_order=1;
#ifndef DBUG_OFF
  if (unlikely(join->thd->is_error()))
    DBUG_PRINT("error",("Error from remove_const"));
#endif
  DBUG_PRINT("exit",("simple_order: %d",(int) *simple_order));
  DBUG_RETURN(first_order);
}


/**
  Filter out ORDER items those are equal to constants in WHERE

  This function is a limited version of remove_const() for use
  with non-JOIN statements (i.e. single-table UPDATE and DELETE).


  @param order            Linked list of ORDER BY arguments
  @param cond             WHERE expression

  @return pointer to new filtered ORDER list or NULL if whole list eliminated

  @note
    This function overwrites input order list.
*/

ORDER *simple_remove_const(ORDER *order, COND *where)
{
  if (!order || !where)
    return order;

  ORDER *first= NULL, *prev= NULL;
  for (; order; order= order->next)
  {
    DBUG_ASSERT(!order->item[0]->with_sum_func); // should never happen
    if (!const_expression_in_where(where, order->item[0]))
    {
      if (!first)
        first= order;
      if (prev)
        prev->next= order;
      prev= order;
    }
  }
  if (prev)
    prev->next= NULL;
  return first;
}


static int
return_zero_rows(JOIN *join, select_result *result, List<TABLE_LIST> &tables,
		 List<Item> &fields, bool send_row, ulonglong select_options,
		 const char *info, Item *having, List<Item> &all_fields)
{
  DBUG_ENTER("return_zero_rows");

  if (select_options & SELECT_DESCRIBE)
  {
    select_describe(join, FALSE, FALSE, FALSE, info);
    DBUG_RETURN(0);
  }

  join->join_free();

  if (send_row)
  {
    /*
      Set all tables to have NULL row. This is needed as we will be evaluating
      HAVING condition.
    */
    List_iterator<TABLE_LIST> ti(tables);
    TABLE_LIST *table;
    while ((table= ti++))
    {
      /*
        Don't touch semi-join materialization tables, as the above join_free()
        call has freed them (and HAVING clause can't have references to them 
        anyway).
      */
      if (!table->is_jtbm())
        mark_as_null_row(table->table);		// All fields are NULL
    }
    List_iterator_fast<Item> it(all_fields);
    Item *item;
    /*
      Inform all items (especially aggregating) to calculate HAVING correctly,
      also we will need it for sending results.
    */
    while ((item= it++))
      item->no_rows_in_result();
    if (having && having->val_int() == 0)
      send_row=0;
  }

  /* Update results for FOUND_ROWS */
  if (!join->send_row_on_empty_set())
  {
    join->thd->set_examined_row_count(0);
    join->thd->limit_found_rows= 0;
  }

  if (!(result->send_result_set_metadata(fields,
                              Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF)))
  {
    bool send_error= FALSE;
    if (send_row)
      send_error= result->send_data(fields) > 0;
    if (likely(!send_error))
      result->send_eof();				// Should be safe
  }
  DBUG_RETURN(0);
}

/**
  used only in JOIN::clear (always) and in do_select()
  (if there where no matching rows)

  @param join            JOIN
  @param cleared_tables  If not null, clear also const tables and mark all
                         cleared tables in the map. cleared_tables is only
                         set when called from do_select() when there is a
                         group function and there where no matching rows.
*/

static void clear_tables(JOIN *join, table_map *cleared_tables)
{
  /* 
    must clear only the non-const tables as const tables are not re-calculated.
  */
  for (uint i= 0 ; i < join->table_count ; i++)
  {
    TABLE *table= join->table[i];

    if (table->null_row)
      continue;                                 // Nothing more to do
    if (!(table->map & join->const_table_map) || cleared_tables)
    {
      if (cleared_tables)
      {
        (*cleared_tables)|= (((table_map) 1) << i);
        if (table->s->null_bytes)
        {
          /*
            Remember null bits for the record so that we can restore the
            original const record in unclear_tables()
          */
          memcpy(table->record[1], table->null_flags, table->s->null_bytes);
        }
      }
      mark_as_null_row(table);                  // All fields are NULL
    }
  }
}


/**
   Reverse null marking for tables and restore null bits.

   We have to do this because the tables may be re-used in a sub query
   and the subquery will assume that the const tables contains the original
   data before clear_tables().
*/

static void unclear_tables(JOIN *join, table_map *cleared_tables)
{
  for (uint i= 0 ; i < join->table_count ; i++)
  {
    if ((*cleared_tables) & (((table_map) 1) << i))
    {
      TABLE *table= join->table[i];
      if (table->s->null_bytes)
        memcpy(table->null_flags, table->record[1], table->s->null_bytes);
      unmark_as_null_row(table);
    }
  }
}


/*****************************************************************************
  Make som simple condition optimization:
  If there is a test 'field = const' change all refs to 'field' to 'const'
  Remove all dummy tests 'item = item', 'const op const'.
  Remove all 'item is NULL', when item can never be null!
  item->marker should be 0 for all items on entry
  Return in cond_value FALSE if condition is impossible (1 = 2)
*****************************************************************************/

class COND_CMP :public ilink {
public:
  static void *operator new(size_t size, MEM_ROOT *mem_root)
  {
    return alloc_root(mem_root, size);
  }
  static void operator delete(void *ptr __attribute__((unused)),
                              size_t size __attribute__((unused)))
  { TRASH_FREE(ptr, size); }

  static void operator delete(void *, MEM_ROOT*) {}

  Item *and_level;
  Item_bool_func2 *cmp_func;
  COND_CMP(Item *a,Item_bool_func2 *b) :and_level(a),cmp_func(b) {}
};

/**
  Find the multiple equality predicate containing a field.

  The function retrieves the multiple equalities accessed through
  the con_equal structure from current level and up looking for
  an equality containing field. It stops retrieval as soon as the equality
  is found and set up inherited_fl to TRUE if it's found on upper levels.

  @param cond_equal          multiple equalities to search in
  @param field               field to look for
  @param[out] inherited_fl   set up to TRUE if multiple equality is found
                             on upper levels (not on current level of
                             cond_equal)

  @return
    - Item_equal for the found multiple equality predicate if a success;
    - NULL otherwise.
*/

Item_equal *find_item_equal(COND_EQUAL *cond_equal, Field *field,
                            bool *inherited_fl)
{
  Item_equal *item= 0;
  bool in_upper_level= FALSE;
  while (cond_equal)
  {
    List_iterator_fast<Item_equal> li(cond_equal->current_level);
    while ((item= li++))
    {
      if (item->contains(field))
        goto finish;
    }
    in_upper_level= TRUE;
    cond_equal= cond_equal->upper_levels;
  }
  in_upper_level= FALSE;
finish:
  *inherited_fl= in_upper_level;
  return item;
}

  
/**
  Check whether an equality can be used to build multiple equalities.

    This function first checks whether the equality (left_item=right_item)
    is a simple equality i.e. the one that equates a field with another field
    or a constant (field=field_item or field=const_item).
    If this is the case the function looks for a multiple equality
    in the lists referenced directly or indirectly by cond_equal inferring
    the given simple equality. If it doesn't find any, it builds a multiple
    equality that covers the predicate, i.e. the predicate can be inferred
    from this multiple equality.
    The built multiple equality could be obtained in such a way:
    create a binary  multiple equality equivalent to the predicate, then
    merge it, if possible, with one of old multiple equalities.
    This guarantees that the set of multiple equalities covering equality
    predicates will be minimal.

  EXAMPLE:
    For the where condition
    @code
      WHERE a=b AND b=c AND
            (b=2 OR f=e)
    @endcode
    the check_equality will be called for the following equality
    predicates a=b, b=c, b=2 and f=e.
    - For a=b it will be called with *cond_equal=(0,[]) and will transform
      *cond_equal into (0,[Item_equal(a,b)]). 
    - For b=c it will be called with *cond_equal=(0,[Item_equal(a,b)])
      and will transform *cond_equal into CE=(0,[Item_equal(a,b,c)]).
    - For b=2 it will be called with *cond_equal=(ptr(CE),[])
      and will transform *cond_equal into (ptr(CE),[Item_equal(2,a,b,c)]).
    - For f=e it will be called with *cond_equal=(ptr(CE), [])
      and will transform *cond_equal into (ptr(CE),[Item_equal(f,e)]).

  @note
    Now only fields that have the same type definitions (verified by
    the Field::eq_def method) are placed to the same multiple equalities.
    Because of this some equality predicates are not eliminated and
    can be used in the constant propagation procedure.
    We could weeken the equlity test as soon as at least one of the 
    equal fields is to be equal to a constant. It would require a 
    more complicated implementation: we would have to store, in
    general case, its own constant for each fields from the multiple
    equality. But at the same time it would allow us to get rid
    of constant propagation completely: it would be done by the call
    to cond->build_equal_items().


    The implementation does not follow exactly the above rules to
    build a new multiple equality for the equality predicate.
    If it processes the equality of the form field1=field2, it
    looks for multiple equalities me1 containig field1 and me2 containing
    field2. If only one of them is found the fuction expands it with
    the lacking field. If multiple equalities for both fields are
    found they are merged. If both searches fail a new multiple equality
    containing just field1 and field2 is added to the existing
    multiple equalities.
    If the function processes the predicate of the form field1=const,
    it looks for a multiple equality containing field1. If found, the 
    function checks the constant of the multiple equality. If the value
    is unknown, it is setup to const. Otherwise the value is compared with
    const and the evaluation of the equality predicate is performed.
    When expanding/merging equality predicates from the upper levels
    the function first copies them for the current level. It looks
    acceptable, as this happens rarely. The implementation without
    copying would be much more complicated.

    For description of how equality propagation works with SJM nests, grep 
    for EqualityPropagationAndSjmNests.

  @param left_item   left term of the quality to be checked
  @param right_item  right term of the equality to be checked
  @param item        equality item if the equality originates from a condition
                     predicate, 0 if the equality is the result of row
                     elimination
  @param cond_equal  multiple equalities that must hold together with the
                     equality

  @retval
    TRUE    if the predicate is a simple equality predicate to be used
    for building multiple equalities
  @retval
    FALSE   otherwise
*/

static bool check_simple_equality(THD *thd, const Item::Context &ctx,
                                  Item *left_item, Item *right_item,
                                  COND_EQUAL *cond_equal)
{
  Item *orig_left_item= left_item;
  Item *orig_right_item= right_item;
  if (left_item->type() == Item::REF_ITEM &&
      ((Item_ref*)left_item)->ref_type() == Item_ref::VIEW_REF)
  {
    if (((Item_ref*)left_item)->get_depended_from())
      return FALSE;
    if (((Item_direct_view_ref*)left_item)->get_null_ref_table() !=
        NO_NULL_TABLE && !left_item->real_item()->used_tables())
      return FALSE;
    left_item= left_item->real_item();
  }
  if (right_item->type() == Item::REF_ITEM &&
      ((Item_ref*)right_item)->ref_type() == Item_ref::VIEW_REF)
  {
    if (((Item_ref*)right_item)->get_depended_from())
      return FALSE;
    if (((Item_direct_view_ref*)right_item)->get_null_ref_table() !=
        NO_NULL_TABLE && !right_item->real_item()->used_tables())
      return FALSE;
    right_item= right_item->real_item();
  }
  if (left_item->type() == Item::FIELD_ITEM &&
      right_item->type() == Item::FIELD_ITEM &&
      !((Item_field*)left_item)->get_depended_from() &&
      !((Item_field*)right_item)->get_depended_from())
  {
    /* The predicate the form field1=field2 is processed */

    Field *left_field= ((Item_field*) left_item)->field;
    Field *right_field= ((Item_field*) right_item)->field;

    if (!left_field->eq_def(right_field))
      return FALSE;

    /* Search for multiple equalities containing field1 and/or field2 */
    bool left_copyfl, right_copyfl;
    Item_equal *left_item_equal=
               find_item_equal(cond_equal, left_field, &left_copyfl);
    Item_equal *right_item_equal= 
               find_item_equal(cond_equal, right_field, &right_copyfl);

    /* As (NULL=NULL) != TRUE we can't just remove the predicate f=f */
    if (left_field->eq(right_field)) /* f = f */
      return (!(left_field->maybe_null() && !left_item_equal)); 

    if (left_item_equal && left_item_equal == right_item_equal)
    {
      /* 
        The equality predicate is inference of one of the existing
        multiple equalities, i.e the condition is already covered
        by upper level equalities
      */
       return TRUE;
    }
      
    /* Copy the found multiple equalities at the current level if needed */
    if (left_copyfl)
    {
      /* left_item_equal of an upper level contains left_item */
      left_item_equal= new (thd->mem_root) Item_equal(thd, left_item_equal);
      left_item_equal->set_context_field(((Item_field*) left_item));
      cond_equal->current_level.push_back(left_item_equal, thd->mem_root);
    }
    if (right_copyfl)
    {
      /* right_item_equal of an upper level contains right_item */
      right_item_equal= new (thd->mem_root) Item_equal(thd, right_item_equal);
      right_item_equal->set_context_field(((Item_field*) right_item));
      cond_equal->current_level.push_back(right_item_equal, thd->mem_root);
    }

    if (left_item_equal)
    { 
      /* left item was found in the current or one of the upper levels */
      if (! right_item_equal)
        left_item_equal->add(orig_right_item, thd->mem_root);
      else
      {
        /* Merge two multiple equalities forming a new one */
        left_item_equal->merge(thd, right_item_equal);
        /* Remove the merged multiple equality from the list */
        List_iterator<Item_equal> li(cond_equal->current_level);
        while ((li++) != right_item_equal) ;
        li.remove();
      }
    }
    else
    { 
      /* left item was not found neither the current nor in upper levels  */
      if (right_item_equal)
        right_item_equal->add(orig_left_item, thd->mem_root);
      else 
      {
        /* None of the fields was found in multiple equalities */
        Type_handler_hybrid_field_type
          tmp(orig_left_item->type_handler_for_comparison());
        if (tmp.aggregate_for_comparison(orig_right_item->
                                         type_handler_for_comparison()))
          return false;
        Item_equal *item_equal=
          new (thd->mem_root) Item_equal(thd, tmp.type_handler(),
                                         orig_left_item, orig_right_item,
                                         false);
        item_equal->set_context_field((Item_field*)left_item);
        cond_equal->current_level.push_back(item_equal, thd->mem_root);
      }
    }
    return TRUE;
  }

  {
    /* The predicate of the form field=const/const=field is processed */
    Item *const_item= 0;
    Item_field *field_item= 0;
    Item *orig_field_item= 0;
    if (left_item->type() == Item::FIELD_ITEM &&
        !((Item_field*)left_item)->get_depended_from() &&
        right_item->const_item() && !right_item->is_expensive())
    {
      orig_field_item= orig_left_item;
      field_item= (Item_field *) left_item;
      const_item= right_item;
    }
    else if (right_item->type() == Item::FIELD_ITEM &&
             !((Item_field*)right_item)->get_depended_from() &&
             left_item->const_item() && !left_item->is_expensive())
    {
      orig_field_item= orig_right_item;
      field_item= (Item_field *) right_item;
      const_item= left_item;
    }

    if (const_item &&
        field_item->field->test_if_equality_guarantees_uniqueness(const_item))
    {
      /*
        field_item and const_item are arguments of a scalar or a row
        comparison function:
          WHERE column=constant
          WHERE (column, ...) = (constant, ...)

        The owner comparison function has previously called fix_fields(),
        so field_item and const_item should be directly comparable items,
        field_item->cmp_context and const_item->cmp_context should be set.
        In case of string comparison, charsets and collations of
        field_item and const_item should have already be aggregated
        for comparison, all necessary character set converters installed
        and fixed.

        In case of string comparison, const_item can be either:
        - a weaker constant that does not need to be converted to field_item:
            WHERE latin1_field = 'latin1_const'
            WHERE varbinary_field = 'latin1_const'
            WHERE latin1_bin_field = 'latin1_general_ci_const'
        - a stronger constant that does not need to be converted to field_item:
            WHERE latin1_field = binary 0xDF
            WHERE latin1_field = 'a' COLLATE latin1_bin
        - a result of conversion (e.g. from the session character set)
          to the character set of field_item:
            WHERE latin1_field = 'utf8_string_with_latin1_repertoire'
      */
      bool copyfl;

      Item_equal *item_equal = find_item_equal(cond_equal,
                                               field_item->field, &copyfl);
      if (copyfl)
      {
        item_equal= new (thd->mem_root) Item_equal(thd, item_equal);
        cond_equal->current_level.push_back(item_equal, thd->mem_root);
        item_equal->set_context_field(field_item);
      }
      Item *const_item2= field_item->field->get_equal_const_item(thd, ctx,
                                                                 const_item);
      if (!const_item2)
        return false;

      if (item_equal)
      {
        /* 
          The flag cond_false will be set to 1 after this, if item_equal
          already contains a constant and its value is  not equal to
          the value of const_item.
        */
        item_equal->add_const(thd, const_item2);
      }
      else
      {
        Type_handler_hybrid_field_type
          tmp(orig_left_item->type_handler_for_comparison());
        if (tmp.aggregate_for_comparison(orig_right_item->
                                         type_handler_for_comparison()))
          return false;
        item_equal= new (thd->mem_root) Item_equal(thd, tmp.type_handler(),
                                                   const_item2,
                                                   orig_field_item, true);
        item_equal->set_context_field(field_item);
        cond_equal->current_level.push_back(item_equal, thd->mem_root);
      }
      return TRUE;
    }
  }
  return FALSE;
}


/**
  Convert row equalities into a conjunction of regular equalities.

    The function converts a row equality of the form (E1,...,En)=(E'1,...,E'n)
    into a list of equalities E1=E'1,...,En=E'n. For each of these equalities
    Ei=E'i the function checks whether it is a simple equality or a row
    equality. If it is a simple equality it is used to expand multiple
    equalities of cond_equal. If it is a row equality it converted to a
    sequence of equalities between row elements. If Ei=E'i is neither a
    simple equality nor a row equality the item for this predicate is added
    to eq_list.

  @param thd        thread handle
  @param left_row   left term of the row equality to be processed
  @param right_row  right term of the row equality to be processed
  @param cond_equal multiple equalities that must hold together with the
                    predicate
  @param eq_list    results of conversions of row equalities that are not
                    simple enough to form multiple equalities

  @retval
    TRUE    if conversion has succeeded (no fatal error)
  @retval
    FALSE   otherwise
*/
 
static bool check_row_equality(THD *thd, const Arg_comparator *comparators,
                               Item *left_row, Item_row *right_row,
                               COND_EQUAL *cond_equal, List<Item>* eq_list)
{ 
  uint n= left_row->cols();
  for (uint i= 0 ; i < n; i++)
  {
    bool is_converted;
    Item *left_item= left_row->element_index(i);
    Item *right_item= right_row->element_index(i);
    if (left_item->type() == Item::ROW_ITEM &&
        right_item->type() == Item::ROW_ITEM)
    {
      /*
        Item_splocal for ROW SP variables return Item::ROW_ITEM.
        Here we know that left_item and right_item are not Item_splocal,
        because ROW SP variables with nested ROWs are not supported yet.
        It's safe to cast left_item and right_item to Item_row.
      */
      DBUG_ASSERT(!left_item->get_item_splocal());
      DBUG_ASSERT(!right_item->get_item_splocal());
      is_converted= check_row_equality(thd,
                                       comparators[i].subcomparators(),
                                       (Item_row *) left_item,
                                       (Item_row *) right_item,
			               cond_equal, eq_list);
    }
    else
    { 
      const Arg_comparator *tmp= &comparators[i];
      is_converted= check_simple_equality(thd,
                                          Item::Context(Item::ANY_SUBST,
                                                  tmp->compare_type_handler(),
                                                  tmp->compare_collation()),
                                          left_item, right_item,
                                          cond_equal);
    }  
 
    if (!is_converted)
    {
      Item_func_eq *eq_item;
      if (!(eq_item= new (thd->mem_root) Item_func_eq(thd, left_item, right_item)) ||
          eq_item->set_cmp_func())
        return FALSE;
      eq_item->quick_fix_field();
      eq_list->push_back(eq_item, thd->mem_root);
    }
  }
  return TRUE;
}


/**
  Eliminate row equalities and form multiple equalities predicates.

    This function checks whether the item is a simple equality
    i.e. the one that equates a field with another field or a constant
    (field=field_item or field=constant_item), or, a row equality.
    For a simple equality the function looks for a multiple equality
    in the lists referenced directly or indirectly by cond_equal inferring
    the given simple equality. If it doesn't find any, it builds/expands
    multiple equality that covers the predicate.
    Row equalities are eliminated substituted for conjunctive regular
    equalities which are treated in the same way as original equality
    predicates.

  @param thd        thread handle
  @param item       predicate to process
  @param cond_equal multiple equalities that must hold together with the
                    predicate
  @param eq_list    results of conversions of row equalities that are not
                    simple enough to form multiple equalities

  @retval
    TRUE   if re-writing rules have been applied
  @retval
    FALSE  otherwise, i.e.
           if the predicate is not an equality,
           or, if the equality is neither a simple one nor a row equality,
           or, if the procedure fails by a fatal error.
*/

bool Item_func_eq::check_equality(THD *thd, COND_EQUAL *cond_equal,
                                  List<Item> *eq_list)
{
  Item *left_item= arguments()[0];
  Item *right_item= arguments()[1];

  if (left_item->type() == Item::ROW_ITEM &&
      right_item->type() == Item::ROW_ITEM)
  {
    /*
      Item_splocal::type() for ROW variables returns Item::ROW_ITEM.
      Distinguish ROW-type Item_splocal from Item_row.
      Example query:
        SELECT 1 FROM DUAL WHERE row_sp_variable=ROW(100,200);
    */
    if (left_item->get_item_splocal() ||
        right_item->get_item_splocal())
      return false;
    return check_row_equality(thd,
                              cmp.subcomparators(),
                              (Item_row *) left_item,
                              (Item_row *) right_item,
                              cond_equal, eq_list);
  }
  return check_simple_equality(thd,
                               Context(ANY_SUBST,
                                       compare_type_handler(),
                                       compare_collation()),
                               left_item, right_item, cond_equal);
}

                          
/**
  Item_xxx::build_equal_items()
  
  Replace all equality predicates in a condition referenced by "this"
  by multiple equality items.

    At each 'and' level the function detects items for equality predicates
    and replaced them by a set of multiple equality items of class Item_equal,
    taking into account inherited equalities from upper levels. 
    If an equality predicate is used not in a conjunction it's just
    replaced by a multiple equality predicate.
    For each 'and' level the function set a pointer to the inherited
    multiple equalities in the cond_equal field of the associated
    object of the type Item_cond_and.   
    The function also traverses the cond tree and and for each field reference
    sets a pointer to the multiple equality item containing the field, if there
    is any. If this multiple equality equates fields to a constant the
    function replaces the field reference by the constant in the cases 
    when the field is not of a string type or when the field reference is
    just an argument of a comparison predicate.
    The function also determines the maximum number of members in 
    equality lists of each Item_cond_and object assigning it to
    thd->lex->current_select->max_equal_elems.

  @note
    Multiple equality predicate =(f1,..fn) is equivalent to the conjuction of
    f1=f2, .., fn-1=fn. It substitutes any inference from these
    equality predicates that is equivalent to the conjunction.
    Thus, =(a1,a2,a3) can substitute for ((a1=a3) AND (a2=a3) AND (a2=a1)) as
    it is equivalent to ((a1=a2) AND (a2=a3)).
    The function always makes a substitution of all equality predicates occurred
    in a conjuction for a minimal set of multiple equality predicates.
    This set can be considered as a canonical representation of the
    sub-conjunction of the equality predicates.
    E.g. (t1.a=t2.b AND t2.b>5 AND t1.a=t3.c) is replaced by 
    (=(t1.a,t2.b,t3.c) AND t2.b>5), not by
    (=(t1.a,t2.b) AND =(t1.a,t3.c) AND t2.b>5);
    while (t1.a=t2.b AND t2.b>5 AND t3.c=t4.d) is replaced by
    (=(t1.a,t2.b) AND =(t3.c=t4.d) AND t2.b>5),
    but if additionally =(t4.d,t2.b) is inherited, it
    will be replaced by (=(t1.a,t2.b,t3.c,t4.d) AND t2.b>5)

    The function performs the substitution in a recursive descent by
    the condtion tree, passing to the next AND level a chain of multiple
    equality predicates which have been built at the upper levels.
    The Item_equal items built at the level are attached to other 
    non-equality conjucts as a sublist. The pointer to the inherited
    multiple equalities is saved in the and condition object (Item_cond_and).
    This chain allows us for any field reference occurence easyly to find a 
    multiple equality that must be held for this occurence.
    For each AND level we do the following:
    - scan it for all equality predicate (=) items
    - join them into disjoint Item_equal() groups
    - process the included OR conditions recursively to do the same for 
      lower AND levels. 

    We need to do things in this order as lower AND levels need to know about
    all possible Item_equal objects in upper levels.

  @param thd        thread handle
  @param inherited  path to all inherited multiple equality items

  @return
    pointer to the transformed condition,
    whose Used_tables_and_const_cache is up to date,
    so no additional update_used_tables() is needed on the result.
*/

COND *Item_cond_and::build_equal_items(THD *thd,
                                       COND_EQUAL *inherited,
                                       bool link_item_fields,
                                       COND_EQUAL **cond_equal_ref)
{
  Item_equal *item_equal;
  COND_EQUAL cond_equal;
  cond_equal.upper_levels= inherited;

  if (check_stack_overrun(thd, STACK_MIN_SIZE, NULL))
    return this;                          // Fatal error flag is set!

  List<Item> eq_list;
  List<Item> *cond_args= argument_list();

  List_iterator<Item> li(*cond_args);
  Item *item;

  DBUG_ASSERT(!cond_equal_ref || !cond_equal_ref[0]);
  /*
     Retrieve all conjuncts of this level detecting the equality
     that are subject to substitution by multiple equality items and
     removing each such predicate from the conjunction after having 
     found/created a multiple equality whose inference the predicate is.
 */
  while ((item= li++))
  {
    /*
      PS/SP note: we can safely remove a node from AND-OR
      structure here because it's restored before each
      re-execution of any prepared statement/stored procedure.
    */
    if (item->check_equality(thd, &cond_equal, &eq_list))
      li.remove();
  }

  /*
    Check if we eliminated all the predicates of the level, e.g.
    (a=a AND b=b AND a=a).
  */
  if (!cond_args->elements && 
      !cond_equal.current_level.elements && 
      !eq_list.elements)
    return new (thd->mem_root) Item_int(thd, (longlong) 1, 1);

  List_iterator_fast<Item_equal> it(cond_equal.current_level);
  while ((item_equal= it++))
  {
    item_equal->set_link_equal_fields(link_item_fields);
    item_equal->fix_fields(thd, NULL);
    item_equal->update_used_tables();
    set_if_bigger(thd->lex->current_select->max_equal_elems,
                  item_equal->n_field_items());  
  }

  m_cond_equal.copy(cond_equal);
  cond_equal.current_level= m_cond_equal.current_level;
  inherited= &m_cond_equal;

  /*
     Make replacement of equality predicates for lower levels
     of the condition expression.
  */
  li.rewind();
  while ((item= li++))
  { 
    Item *new_item;
    if ((new_item= item->build_equal_items(thd, inherited, false, NULL))
        != item)
    {
      /* This replacement happens only for standalone equalities */
      /*
        This is ok with PS/SP as the replacement is done for
        cond_args of an AND/OR item, which are restored for each
        execution of PS/SP.
      */
      li.replace(new_item);
    }
  }
  cond_args->append(&eq_list);
  cond_args->append((List<Item> *)&cond_equal.current_level);
  update_used_tables();
  if (cond_equal_ref)
    *cond_equal_ref= &m_cond_equal;
  return this;
}


COND *Item_cond::build_equal_items(THD *thd,
                                   COND_EQUAL *inherited,
                                   bool link_item_fields,
                                   COND_EQUAL **cond_equal_ref)
{
  List<Item> *cond_args= argument_list();
  
  List_iterator<Item> li(*cond_args);
  Item *item;

  DBUG_ASSERT(!cond_equal_ref || !cond_equal_ref[0]);
  /*
     Make replacement of equality predicates for lower levels
     of the condition expression.
     Update used_tables_cache and const_item_cache on the way.
  */
  used_tables_and_const_cache_init();
  while ((item= li++))
  { 
    Item *new_item;
    if ((new_item= item->build_equal_items(thd, inherited, false, NULL))
        != item)
    {
      /* This replacement happens only for standalone equalities */
      /*
        This is ok with PS/SP as the replacement is done for
        arguments of an AND/OR item, which are restored for each
        execution of PS/SP.
      */
      li.replace(new_item);
    }
    used_tables_and_const_cache_join(new_item);
  }
  return this;
}


COND *Item_func_eq::build_equal_items(THD *thd,
                                      COND_EQUAL *inherited,
                                      bool link_item_fields,
                                      COND_EQUAL **cond_equal_ref)
{
  COND_EQUAL cond_equal;
  cond_equal.upper_levels= inherited;
  List<Item> eq_list;

  DBUG_ASSERT(!cond_equal_ref || !cond_equal_ref[0]);
  /*
    If an equality predicate forms the whole and level,
    we call it standalone equality and it's processed here.
    E.g. in the following where condition
    WHERE a=5 AND (b=5 or a=c)
    (b=5) and (a=c) are standalone equalities.
    In general we can't leave alone standalone eqalities:
    for WHERE a=b AND c=d AND (b=c OR d=5)
    b=c is replaced by =(a,b,c,d).  
   */
  if (Item_func_eq::check_equality(thd, &cond_equal, &eq_list))
  {
    Item_equal *item_equal;
    int n= cond_equal.current_level.elements + eq_list.elements;
    if (n == 0)
      return new (thd->mem_root) Item_int(thd, (longlong) 1, 1);
    else if (n == 1)
    {
      if ((item_equal= cond_equal.current_level.pop()))
      {
        item_equal->fix_fields(thd, NULL);
        item_equal->update_used_tables();
        set_if_bigger(thd->lex->current_select->max_equal_elems,
                      item_equal->n_field_items());  
        item_equal->upper_levels= inherited;
        if (cond_equal_ref)
          *cond_equal_ref= new (thd->mem_root) COND_EQUAL(item_equal,
                                                          thd->mem_root);
        return item_equal;
      }
      Item *res= eq_list.pop();
      res->update_used_tables();
      DBUG_ASSERT(res->type() == FUNC_ITEM);
      return res;
    }
    else
    {
      /* 
        Here a new AND level must be created. It can happen only
        when a row equality is processed as a standalone predicate.
      */
      Item_cond_and *and_cond= new (thd->mem_root) Item_cond_and(thd, eq_list);
      and_cond->quick_fix_field();
      List<Item> *cond_args= and_cond->argument_list();
      List_iterator_fast<Item_equal> it(cond_equal.current_level);
      while ((item_equal= it++))
      {
        if (item_equal->fix_length_and_dec())
          return NULL;
        item_equal->update_used_tables();
        set_if_bigger(thd->lex->current_select->max_equal_elems,
                      item_equal->n_field_items());  
      }
      and_cond->m_cond_equal.copy(cond_equal);
      cond_equal.current_level= and_cond->m_cond_equal.current_level;
      cond_args->append((List<Item> *)&cond_equal.current_level);
      and_cond->update_used_tables();
      if (cond_equal_ref)
        *cond_equal_ref= &and_cond->m_cond_equal;
      return and_cond;
    }
  }
  return Item_func::build_equal_items(thd, inherited, link_item_fields,
                                      cond_equal_ref);
}


COND *Item_func::build_equal_items(THD *thd, COND_EQUAL *inherited,
                                   bool link_item_fields,
                                   COND_EQUAL **cond_equal_ref)
{
  /* 
    For each field reference in cond, not from equal item predicates,
    set a pointer to the multiple equality it belongs to (if there is any)
    as soon the field is not of a string type or the field reference is
    an argument of a comparison predicate. 
  */ 
  COND *cond= propagate_equal_fields(thd, Context_boolean(), inherited);
  cond->update_used_tables();
  DBUG_ASSERT(cond == this);
  DBUG_ASSERT(!cond_equal_ref || !cond_equal_ref[0]);
  return cond;
}


COND *Item_equal::build_equal_items(THD *thd, COND_EQUAL *inherited,
                                    bool link_item_fields,
                                    COND_EQUAL **cond_equal_ref)
{
  COND *cond= Item_func::build_equal_items(thd, inherited, link_item_fields,
                                           cond_equal_ref);
  if (cond_equal_ref)
    *cond_equal_ref= new (thd->mem_root) COND_EQUAL(this, thd->mem_root);
  return cond;
}


/**
  Build multiple equalities for a condition and all on expressions that
  inherit these multiple equalities.

    The function first applies the cond->build_equal_items() method
    to build all multiple equalities for condition cond utilizing equalities
    referred through the parameter inherited. The extended set of
    equalities is returned in the structure referred by the cond_equal_ref
    parameter. After this the function calls itself recursively for
    all on expressions whose direct references can be found in join_list
    and who inherit directly the multiple equalities just having built.

  @note
    The on expression used in an outer join operation inherits all equalities
    from the on expression of the embedding join, if there is any, or
    otherwise - from the where condition.
    This fact is not obvious, but presumably can be proved.
    Consider the following query:
    @code
      SELECT * FROM (t1,t2) LEFT JOIN (t3,t4) ON t1.a=t3.a AND t2.a=t4.a
        WHERE t1.a=t2.a;
    @endcode
    If the on expression in the query inherits =(t1.a,t2.a), then we
    can build the multiple equality =(t1.a,t2.a,t3.a,t4.a) that infers
    the equality t3.a=t4.a. Although the on expression
    t1.a=t3.a AND t2.a=t4.a AND t3.a=t4.a is not equivalent to the one
    in the query the latter can be replaced by the former: the new query
    will return the same result set as the original one.

    Interesting that multiple equality =(t1.a,t2.a,t3.a,t4.a) allows us
    to use t1.a=t3.a AND t3.a=t4.a under the on condition:
    @code
      SELECT * FROM (t1,t2) LEFT JOIN (t3,t4) ON t1.a=t3.a AND t3.a=t4.a
        WHERE t1.a=t2.a
    @endcode
    This query equivalent to:
    @code
      SELECT * FROM (t1 LEFT JOIN (t3,t4) ON t1.a=t3.a AND t3.a=t4.a),t2
        WHERE t1.a=t2.a
    @endcode
    Similarly the original query can be rewritten to the query:
    @code
      SELECT * FROM (t1,t2) LEFT JOIN (t3,t4) ON t2.a=t4.a AND t3.a=t4.a
        WHERE t1.a=t2.a
    @endcode
    that is equivalent to:   
    @code
      SELECT * FROM (t2 LEFT JOIN (t3,t4)ON t2.a=t4.a AND t3.a=t4.a), t1
        WHERE t1.a=t2.a
    @endcode
    Thus, applying equalities from the where condition we basically
    can get more freedom in performing join operations.
    Although we don't use this property now, it probably makes sense to use 
    it in the future.    
  @param thd		     Thread handler
  @param cond                condition to build the multiple equalities for
  @param inherited           path to all inherited multiple equality items
  @param join_list           list of join tables to which the condition
                             refers to
  @ignore_on_conds           TRUE <-> do not build multiple equalities
                             for on expressions
  @param[out] cond_equal_ref pointer to the structure to place built
                             equalities in
  @param link_equal_items    equal fields are to be linked

  @return
    pointer to the transformed condition containing multiple equalities
*/
   
static COND *build_equal_items(JOIN *join, COND *cond,
                               COND_EQUAL *inherited,
                               List<TABLE_LIST> *join_list,
                               bool ignore_on_conds,
                               COND_EQUAL **cond_equal_ref,
                               bool link_equal_fields)
{
  THD *thd= join->thd;

  *cond_equal_ref= NULL;

  if (cond) 
  {
    cond= cond->build_equal_items(thd, inherited, link_equal_fields,
                                  cond_equal_ref);
    if (*cond_equal_ref)
    {
      (*cond_equal_ref)->upper_levels= inherited;
      inherited= *cond_equal_ref;
    }
  }

  if (join_list && !ignore_on_conds)
  {
    TABLE_LIST *table;
    List_iterator<TABLE_LIST> li(*join_list);

    while ((table= li++))
    {
      if (table->on_expr)
      {
        List<TABLE_LIST> *nested_join_list= table->nested_join ?
          &table->nested_join->join_list : NULL;
        /*
          We can modify table->on_expr because its old value will
          be restored before re-execution of PS/SP.
        */
        table->on_expr= build_equal_items(join, table->on_expr, inherited,
                                          nested_join_list, ignore_on_conds,
                                          &table->cond_equal);
      }
    }
  }

  return cond;
}    


/**
  Compare field items by table order in the execution plan.

    If field1 and field2 belong to different tables then
    field1 considered as better than field2 if the table containing
    field1 is accessed earlier than the table containing field2.   
    The function finds out what of two fields is better according
    this criteria.
    If field1 and field2 belong to the same table then the result
    of comparison depends on whether the fields are parts of
    the key that are used to access this table.  

  @param field1          first field item to compare
  @param field2          second field item to compare
  @param table_join_idx  index to tables determining table order

  @retval
    1  if field1 is better than field2
  @retval
    -1  if field2 is better than field1
  @retval
    0  otherwise
*/

static int compare_fields_by_table_order(Item *field1,
                                         Item *field2,
                                         void *table_join_idx)
{
  int cmp= 0;
  bool outer_ref= 0;
  Item *field1_real= field1->real_item();
  Item *field2_real= field2->real_item();

  if (field1->const_item() || field1_real->const_item())
    return -1;
  if (field2->const_item() || field2_real->const_item())
    return 1;
  Item_field *f1= (Item_field *) field1_real;
  Item_field *f2= (Item_field *) field2_real;
  if (f1->used_tables() & OUTER_REF_TABLE_BIT)
  {
    outer_ref= 1;
    cmp= -1;
  }
  if (f2->used_tables() & OUTER_REF_TABLE_BIT)
  {  
    outer_ref= 1;
    cmp++;
  }
  if (outer_ref)
    return cmp;
  JOIN_TAB **idx= (JOIN_TAB **) table_join_idx;
  
  JOIN_TAB *tab1= idx[f1->field->table->tablenr];
  JOIN_TAB *tab2= idx[f2->field->table->tablenr];
  
  /* 
    if one of the table is inside a merged SJM nest and another one isn't,
    compare SJM bush roots of the tables.
  */
  if (tab1->bush_root_tab != tab2->bush_root_tab)
  {
    if (tab1->bush_root_tab)
      tab1= tab1->bush_root_tab;

    if (tab2->bush_root_tab)
      tab2= tab2->bush_root_tab;
  }
  
  cmp= (int)(tab1 - tab2);

  if (!cmp)
  {
    /* Fields f1, f2 belong to the same table */

    JOIN_TAB *tab= idx[f1->field->table->tablenr];
    uint keyno= MAX_KEY;
    if (tab->ref.key_parts)
      keyno= tab->ref.key;
    else if (tab->select && tab->select->quick)
       keyno = tab->select->quick->index;
    if (keyno != MAX_KEY)
    {
      if (f1->field->part_of_key.is_set(keyno))
        cmp= -1;
      if (f2->field->part_of_key.is_set(keyno))
        cmp++;
      /*
        Here:
        if both f1, f2 are components of the key tab->ref.key then cmp==0,
        if only f1 is a component of the key then cmp==-1 (f1 is better),
        if only f2 is a component of the key then cmp==1, (f2 is better),
        if none of f1,f1 is component of the key cmp==0.
      */  
      if (!cmp)
      {
        KEY *key_info= tab->table->key_info + keyno;
        for (uint i= 0; i < key_info->user_defined_key_parts; i++)
	{
          Field *fld= key_info->key_part[i].field;
          if (fld->eq(f1->field))
	  {
	    cmp= -1; // f1 is better
            break;
          }
          if (fld->eq(f2->field))
	  {
	    cmp= 1;  // f2 is better
            break;
          }
        }
      }              
    }              
    if (!cmp)   
      cmp= f1->field->field_index-f2->field->field_index;
  }
  return cmp < 0 ? -1 : (cmp ? 1 : 0);
}


static TABLE_LIST* embedding_sjm(Item *item)
{
  Item_field *item_field= (Item_field *) (item->real_item());
  TABLE_LIST *nest= item_field->field->table->pos_in_table_list->embedding;
  if (nest && nest->sj_mat_info && nest->sj_mat_info->is_used)
    return nest;
  else
    return NULL;
}

/**
  Generate minimal set of simple equalities equivalent to a multiple equality.

    The function retrieves the fields of the multiple equality item
    item_equal and  for each field f:
    - if item_equal contains const it generates the equality f=const_item;
    - otherwise, if f is not the first field, generates the equality
      f=item_equal->get_first().
    All generated equality are added to the cond conjunction.

  @param cond            condition to add the generated equality to
  @param upper_levels    structure to access multiple equality of upper levels
  @param item_equal      multiple equality to generate simple equality from

  @note
    Before generating an equality function checks that it has not
    been generated for multiple equalities of the upper levels.
    E.g. for the following where condition
    WHERE a=5 AND ((a=b AND b=c) OR  c>4)
    the upper level AND condition will contain =(5,a),
    while the lower level AND condition will contain =(5,a,b,c).
    When splitting =(5,a,b,c) into a separate equality predicates
    we should omit 5=a, as we have it already in the upper level.
    The following where condition gives us a more complicated case:
    WHERE t1.a=t2.b AND t3.c=t4.d AND (t2.b=t3.c OR t4.e>5 ...) AND ...
    Given the tables are accessed in the order t1->t2->t3->t4 for
    the selected query execution plan the lower level multiple
    equality =(t1.a,t2.b,t3.c,t4.d) formally  should be converted to
    t1.a=t2.b AND t1.a=t3.c AND t1.a=t4.d. But t1.a=t2.a will be
    generated for the upper level. Also t3.c=t4.d will be generated there.
    So only t1.a=t3.c should be left in the lower level.
    If cond is equal to 0, then not more then one equality is generated
    and a pointer to it is returned as the result of the function.
    
    Equality substutution and semi-join materialization nests:

       In case join order looks like this:

          outer_tbl1 outer_tbl2 SJM (inner_tbl1 inner_tbl2) outer_tbl3 

        We must not construct equalities like 

           outer_tbl1.col = inner_tbl1.col 

        because they would get attached to inner_tbl1 and will get evaluated
        during materialization phase, when we don't have current value of
        outer_tbl1.col.

        Item_equal::get_first() also takes similar measures for dealing with
        equality substitution in presense of SJM nests.

    Grep for EqualityPropagationAndSjmNests for a more verbose description.

  @return
    - The condition with generated simple equalities or
    a pointer to the simple generated equality, if success.
    - 0, otherwise.
*/

Item *eliminate_item_equal(THD *thd, COND *cond, COND_EQUAL *upper_levels,
                           Item_equal *item_equal)
{
  List<Item> eq_list;
  Item_func_eq *eq_item= 0;
  if (((Item *) item_equal)->const_item() && !item_equal->val_int())
    return new (thd->mem_root) Item_int(thd, (longlong) 0, 1);
  Item *item_const= item_equal->get_const();
  Item_equal_fields_iterator it(*item_equal);
  Item *head;
  TABLE_LIST *current_sjm= NULL;
  Item *current_sjm_head= NULL;

  DBUG_ASSERT(!cond ||
              cond->type() == Item::INT_ITEM ||
              (cond->type() == Item::FUNC_ITEM &&
               ((Item_func *) cond)->functype() == Item_func::EQ_FUNC) ||  
              (cond->type() == Item::COND_ITEM  && 
               ((Item_func *) cond)->functype() == Item_func::COND_AND_FUNC));
       
  /* 
    Pick the "head" item: the constant one or the first in the join order
    (if the first in the join order happends to be inside an SJM nest, that's
    ok, because this is where the value will be unpacked after
    materialization).
  */
  if (item_const)
    head= item_const;
  else
  {
    TABLE_LIST *emb_nest;
    head= item_equal->get_first(NO_PARTICULAR_TAB, NULL);
    it++;
    if ((emb_nest= embedding_sjm(head)))
    {
      current_sjm= emb_nest;
      current_sjm_head= head;
    }
  }

  Item *field_item;
  /*
    For each other item, generate "item=head" equality (except the tables that 
    are within SJ-Materialization nests, for those "head" is defined
    differently)
  */
  while ((field_item= it++))
  {
    Item_equal *upper= field_item->find_item_equal(upper_levels);
    Item *item= field_item;
    TABLE_LIST *field_sjm= embedding_sjm(field_item);
    if (!field_sjm)
    { 
      current_sjm= NULL;
      current_sjm_head= NULL;
    }      

    /* 
      Check if "field_item=head" equality is already guaranteed to be true 
      on upper AND-levels.
    */
    if (upper)
    {
      TABLE_LIST *native_sjm= embedding_sjm(item_equal->context_field);
      Item *upper_const= upper->get_const();
      if (item_const && upper_const)
      {
        /* 
          Upper item also has "field_item=const".
          Don't produce equality if const is equal to item_const.
        */
        Item_func_eq *func= new (thd->mem_root) Item_func_eq(thd, item_const, upper_const);
        func->set_cmp_func();
        func->quick_fix_field();
        if (func->val_int())
          item= 0;
      }
      else
      {
        Item_equal_fields_iterator li(*item_equal);
        while ((item= li++) != field_item)
        {
          if (embedding_sjm(item) == field_sjm && 
              item->find_item_equal(upper_levels) == upper)
            break;
        }
      }
      if (embedding_sjm(field_item) != native_sjm)
        item= NULL; /* Don't produce equality */
    }
    
    bool produce_equality= MY_TEST(item == field_item);
    if (!item_const && field_sjm && field_sjm != current_sjm)
    {
      /* Entering an SJM nest */
      current_sjm_head= field_item;
      if (!field_sjm->sj_mat_info->is_sj_scan)
        produce_equality= FALSE;
    }

    if (produce_equality)
    {
      if (eq_item && eq_list.push_back(eq_item, thd->mem_root))
        return 0;
      
      /*
        If we're inside an SJM-nest (current_sjm!=NULL), and the multi-equality
        doesn't include a constant, we should produce equality with the first
        of the equal items in this SJM (except for the first element inside the
        SJM. For that, we produce the equality with the "head" item).

        In other cases, get the "head" item, which is either first of the
        equals on top level, or the constant.
      */
      Item *head_item= (!item_const && current_sjm && 
                        current_sjm_head != field_item) ? current_sjm_head: head;
      Item *head_real_item=  head_item->real_item();
      if (head_real_item->type() == Item::FIELD_ITEM)
        head_item= head_real_item;
      
      eq_item= new (thd->mem_root) Item_func_eq(thd, field_item->real_item(), head_item);

      if (!eq_item || eq_item->set_cmp_func())
        return 0;
      eq_item->quick_fix_field();
    }
    current_sjm= field_sjm;
  }

  /*
    We have produced zero, one, or more pair-wise equalities eq_i. We want to
    return an expression in form:

      cond AND eq_1 AND eq_2 AND eq_3 AND ...
    
    'cond' is a parameter for this function, which may be NULL, an Item_int(1),
    or an Item_func_eq or an Item_cond_and.

    We want to return a well-formed condition: no nested Item_cond_and objects,
    or Item_cond_and with a single child:
    - if 'cond' is an Item_cond_and, we add eq_i as its tail
    - if 'cond' is Item_int(1), we return eq_i
    - otherwise, we create our own Item_cond_and and put 'cond' at the front of
      it.
    - if we have only one condition to return, we don't create an Item_cond_and
  */

  if (eq_item && eq_list.push_back(eq_item, thd->mem_root))
    return 0;
  COND *res= 0;
  switch (eq_list.elements)
  {
  case 0:
    res= cond ? cond : new (thd->mem_root) Item_int(thd, (longlong) 1, 1);
    break;
  case 1:
    if (!cond || cond->type() ==  Item::INT_ITEM)
      res= eq_item;
    break;
  default:
    break;
  }
  if (!res) 
  {
    if (cond)
    {
      if (cond->type() == Item::COND_ITEM)
      {
        res= cond;
        ((Item_cond *) res)->add_at_end(&eq_list);
      }
      else if (eq_list.push_front(cond, thd->mem_root))
        return 0;
    }
  }  
  if (!res)
    res= new (thd->mem_root) Item_cond_and(thd, eq_list);
  if (res)
  {
    res->quick_fix_field();
    res->update_used_tables();
  }

  return res;
}


/**
  Substitute every field reference in a condition by the best equal field
  and eliminate all multiple equality predicates.

    The function retrieves the cond condition and for each encountered
    multiple equality predicate it sorts the field references in it
    according to the order of tables specified by the table_join_idx
    parameter. Then it eliminates the multiple equality predicate it
    replacing it by the conjunction of simple equality predicates 
    equating every field from the multiple equality to the first
    field in it, or to the constant, if there is any.
    After this the function retrieves all other conjuncted
    predicates substitute every field reference by the field reference
    to the first equal field or equal constant if there are any.

  @param context_tab     Join tab that 'cond' will be attached to, or 
                         NO_PARTICULAR_TAB. See notes above.
  @param cond            condition to process
  @param cond_equal      multiple equalities to take into consideration
  @param table_join_idx  index to tables determining field preference

  @note
    At the first glance full sort of fields in multiple equality
    seems to be an overkill. Yet it's not the case due to possible
    new fields in multiple equality item of lower levels. We want
    the order in them to comply with the order of upper levels.

    context_tab may be used to specify which join tab `cond` will be
    attached to. There are two possible cases:

    1. context_tab != NO_PARTICULAR_TAB
       We're doing substitution for an Item which will be evaluated in the 
       context of a particular item. For example, if the optimizer does a 
       ref access on "tbl1.key= expr" then
        = equality substitution will be perfomed on 'expr'
        = it is known in advance that 'expr' will be evaluated when 
          table t1 is accessed.
       Note that in this kind of substution we never have to replace Item_equal
       objects. For example, for

        t.key= func(col1=col2 AND col2=const)
       
       we will not build Item_equal or do equality substution (if we decide to,
       this function will need to be fixed to handle it)

    2. context_tab == NO_PARTICULAR_TAB
       We're doing substitution in WHERE/ON condition, which is not yet 
       attached to any particular join_tab. We will use information about the
       chosen join order to make "optimal" substitions, i.e. those that allow
       to apply filtering as soon as possible. See eliminate_item_equal() and 
       Item_equal::get_first() for details.

  @return
    The transformed condition, or NULL in case of error
*/

static COND* substitute_for_best_equal_field(THD *thd, JOIN_TAB *context_tab,
                                             COND *cond,
                                             COND_EQUAL *cond_equal,
                                             void *table_join_idx)
{
  Item_equal *item_equal;
  COND *org_cond= cond;                 // Return this in case of fatal error

  if (cond->type() == Item::COND_ITEM)
  {
    List<Item> *cond_list= ((Item_cond*) cond)->argument_list();

    bool and_level= ((Item_cond*) cond)->functype() ==
                      Item_func::COND_AND_FUNC;
    if (and_level)
    {
      cond_equal= &((Item_cond_and *) cond)->m_cond_equal;
      cond_list->disjoin((List<Item> *) &cond_equal->current_level);/* remove Item_equal objects from the AND. */

      List_iterator_fast<Item_equal> it(cond_equal->current_level);      
      while ((item_equal= it++))
      {
        item_equal->sort(&compare_fields_by_table_order, table_join_idx);
      }
    }
    
    List_iterator<Item> li(*cond_list);
    Item *item;
    while ((item= li++))
    {
      Item *new_item= substitute_for_best_equal_field(thd, context_tab,
                                                      item, cond_equal,
                                                      table_join_idx);
      /*
        This works OK with PS/SP re-execution as changes are made to
        the arguments of AND/OR items only
      */
      if (new_item && new_item != item)
        li.replace(new_item);
    }

    if (and_level)
    {
      COND *eq_cond= 0;
      List_iterator_fast<Item_equal> it(cond_equal->current_level);
      bool false_eq_cond= FALSE;
      while ((item_equal= it++))
      {
        eq_cond= eliminate_item_equal(thd, eq_cond, cond_equal->upper_levels,
                                      item_equal);
        if (!eq_cond)
	{
          eq_cond= 0;
          break;
        }
        else if (eq_cond->type() == Item::INT_ITEM && !eq_cond->val_bool()) 
	{
          /*
            This occurs when eliminate_item_equal() founds that cond is
            always false and substitutes it with Item_int 0.
            Due to this, value of item_equal will be 0, so just return it.
	  */
          cond= eq_cond;
          false_eq_cond= TRUE;
          break;
        }
      }
      if (eq_cond && !false_eq_cond)
      {
        /* Insert the generated equalities before all other conditions */
        if (eq_cond->type() == Item::COND_ITEM)
          ((Item_cond *) cond)->add_at_head(
                                  ((Item_cond *) eq_cond)->argument_list());
        else
	{
          if (cond_list->is_empty())
            cond= eq_cond;
          else
	  {
             /* Do not add an equality condition if it's always true */ 
             if (eq_cond->type() != Item::INT_ITEM &&
                 cond_list->push_front(eq_cond, thd->mem_root))
               eq_cond= 0;
          }
	}
      }
      if (!eq_cond)
      {
        /* 
          We are out of memory doing the transformation.
          This is a fatal error now. However we bail out by returning the
          original condition that we had before we started the transformation. 
	*/
	cond_list->append((List<Item> *) &cond_equal->current_level);
      }
    }	 
  }
  else if (cond->type() == Item::FUNC_ITEM && 
           ((Item_func*) cond)->functype() == Item_func::MULT_EQUAL_FUNC)
  {
    item_equal= (Item_equal *) cond;
    item_equal->sort(&compare_fields_by_table_order, table_join_idx);
    cond_equal= item_equal->upper_levels;
    if (cond_equal && cond_equal->current_level.head() == item_equal)
      cond_equal= cond_equal->upper_levels;
    cond= eliminate_item_equal(thd, 0, cond_equal, item_equal);
    return cond ? cond : org_cond;
  }
  else 
  {
    while (cond_equal)
    {
      List_iterator_fast<Item_equal> it(cond_equal->current_level);
      while((item_equal= it++))
      {
        REPLACE_EQUAL_FIELD_ARG arg= {item_equal, context_tab};
        if (!(cond= cond->transform(thd, &Item::replace_equal_field,
                                    (uchar *) &arg)))
          return 0;
      }
      cond_equal= cond_equal->upper_levels;
    }
  }
  return cond;
}


/**
  Check appearance of new constant items in multiple equalities
  of a condition after reading a constant table.

    The function retrieves the cond condition and for each encountered
    multiple equality checks whether new constants have appeared after
    reading the constant (single row) table tab. If so it adjusts
    the multiple equality appropriately.

  @param cond       condition whose multiple equalities are to be checked
  @param table      constant table that has been read
  @param const_key  mark key parts as constant
*/

static void update_const_equal_items(THD *thd, COND *cond, JOIN_TAB *tab,
                                     bool const_key)
{
  if (!(cond->used_tables() & tab->table->map))
    return;

  if (cond->type() == Item::COND_ITEM)
  {
    List<Item> *cond_list= ((Item_cond*) cond)->argument_list(); 
    List_iterator_fast<Item> li(*cond_list);
    Item *item;
    while ((item= li++))
      update_const_equal_items(thd, item, tab,
                               (((Item_cond*) cond)->top_level() &&
                                ((Item_cond*) cond)->functype() ==
                                Item_func::COND_AND_FUNC));
  }
  else if (cond->type() == Item::FUNC_ITEM && 
           ((Item_cond*) cond)->functype() == Item_func::MULT_EQUAL_FUNC)
  {
    Item_equal *item_equal= (Item_equal *) cond;
    bool contained_const= item_equal->get_const() != NULL;
    item_equal->update_const(thd);
    if (!contained_const && item_equal->get_const())
    {
      /* Update keys for range analysis */
      Item_equal_fields_iterator it(*item_equal);
      while (it++)
      {
        Field *field= it.get_curr_field();
        JOIN_TAB *stat= field->table->reginfo.join_tab;
        key_map possible_keys= field->key_start;
        possible_keys.intersect(field->table->keys_in_use_for_query);
        stat[0].const_keys.merge(possible_keys);

        /*
          For each field in the multiple equality (for which we know that it 
          is a constant) we have to find its corresponding key part, and set 
          that key part in const_key_parts.
        */  
        if (!possible_keys.is_clear_all())
        {
          TABLE *field_tab= field->table;
          KEYUSE *use;
          for (use= stat->keyuse; use && use->table == field_tab; use++)
            if (const_key &&
                !use->is_for_hash_join() && possible_keys.is_set(use->key) && 
                field_tab->key_info[use->key].key_part[use->keypart].field ==
                field)
              field_tab->const_key_parts[use->key]|= use->keypart_map;
        }
      }
    }
  }
}


/**
  Check if
    WHERE expr=value AND expr=const
  can be rewritten as:
    WHERE const=value AND expr=const

  @param target       - the target operator whose "expr" argument will be
                        replaced to "const".
  @param target_expr  - the target's "expr" which will be replaced to "const".
  @param target_value - the target's second argument, it will remain unchanged.
  @param source       - the equality expression ("=" or "<=>") that
                        can be used to rewrite the "target" part
                        (under certain conditions, see the code).
  @param source_expr  - the source's "expr". It should be exactly equal to 
                        the target's "expr" to make condition rewrite possible.
  @param source_const - the source's "const" argument, it will be inserted
                        into "target" instead of "expr".
*/
static bool
can_change_cond_ref_to_const(Item_bool_func2 *target,
                             Item *target_expr, Item *target_value,
                             Item_bool_func2 *source,
                             Item *source_expr, Item *source_const)
{
  return target_expr->eq(source_expr,0) &&
         target_value != source_const &&
         target->compare_type_handler()->
           can_change_cond_ref_to_const(target, target_expr, target_value,
                                        source, source_expr, source_const);
}


/*
  change field = field to field = const for each found field = const in the
  and_level
*/

static void
change_cond_ref_to_const(THD *thd, I_List<COND_CMP> *save_list,
                         Item *and_father, Item *cond,
                         Item_bool_func2 *field_value_owner,
                         Item *field, Item *value)
{
  if (cond->type() == Item::COND_ITEM)
  {
    bool and_level= ((Item_cond*) cond)->functype() ==
      Item_func::COND_AND_FUNC;
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
      change_cond_ref_to_const(thd, save_list,and_level ? cond : item, item,
			       field_value_owner, field, value);
    return;
  }
  if (cond->eq_cmp_result() == Item::COND_OK)
    return;					// Not a boolean function

  Item_bool_func2 *func=  (Item_bool_func2*) cond;
  Item **args= func->arguments();
  Item *left_item=  args[0];
  Item *right_item= args[1];
  Item_func::Functype functype=  func->functype();

  if (can_change_cond_ref_to_const(func, right_item, left_item,
                                   field_value_owner, field, value))
  {
    Item *tmp=value->clone_item(thd);
    if (tmp)
    {
      tmp->collation.set(right_item->collation);
      thd->change_item_tree(args + 1, tmp);
      func->update_used_tables();
      if ((functype == Item_func::EQ_FUNC || functype == Item_func::EQUAL_FUNC)
	  && and_father != cond && !left_item->const_item())
      {
	cond->marker=1;
	COND_CMP *tmp2;
        /* Will work, even if malloc would fail */
        if ((tmp2= new (thd->mem_root) COND_CMP(and_father, func)))
	  save_list->push_back(tmp2);
      }
      /*
        LIKE can be optimized for BINARY/VARBINARY/BLOB columns, e.g.:

        from: WHERE CONCAT(c1)='const1' AND CONCAT(c1) LIKE 'const2'
          to: WHERE CONCAT(c1)='const1' AND 'const1' LIKE 'const2'

        So make sure to use set_cmp_func() only for non-LIKE operators.
      */
      if (functype != Item_func::LIKE_FUNC)
        ((Item_bool_rowready_func2*) func)->set_cmp_func();
    }
  }
  else if (can_change_cond_ref_to_const(func, left_item, right_item,
                                        field_value_owner, field, value))
  {
    Item *tmp= value->clone_item(thd);
    if (tmp)
    {
      tmp->collation.set(left_item->collation);
      thd->change_item_tree(args, tmp);
      value= tmp;
      func->update_used_tables();
      if ((functype == Item_func::EQ_FUNC || functype == Item_func::EQUAL_FUNC)
	  && and_father != cond && !right_item->const_item())
      {
        args[0]= args[1];                       // For easy check
        thd->change_item_tree(args + 1, value);
	cond->marker=1;
	COND_CMP *tmp2;
        /* Will work, even if malloc would fail */
        if ((tmp2=new (thd->mem_root) COND_CMP(and_father, func)))
	  save_list->push_back(tmp2);
      }
      if (functype != Item_func::LIKE_FUNC)
        ((Item_bool_rowready_func2*) func)->set_cmp_func();
    }
  }
}


static void
propagate_cond_constants(THD *thd, I_List<COND_CMP> *save_list,
                         COND *and_father, COND *cond)
{
  if (cond->type() == Item::COND_ITEM)
  {
    bool and_level= ((Item_cond*) cond)->functype() ==
      Item_func::COND_AND_FUNC;
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    I_List<COND_CMP> save;
    while ((item=li++))
    {
      propagate_cond_constants(thd, &save,and_level ? cond : item, item);
    }
    if (and_level)
    {						// Handle other found items
      I_List_iterator<COND_CMP> cond_itr(save);
      COND_CMP *cond_cmp;
      while ((cond_cmp=cond_itr++))
      {
        Item **args= cond_cmp->cmp_func->arguments();
        if (!args[0]->const_item())
          change_cond_ref_to_const(thd, &save,cond_cmp->and_level,
                                   cond_cmp->and_level,
                                   cond_cmp->cmp_func, args[0], args[1]);
      }
    }
  }
  else if (and_father != cond && !cond->marker)		// In a AND group
  {
    if (cond->type() == Item::FUNC_ITEM &&
	(((Item_func*) cond)->functype() == Item_func::EQ_FUNC ||
	 ((Item_func*) cond)->functype() == Item_func::EQUAL_FUNC))
    {
      Item_func_eq *func=(Item_func_eq*) cond;
      Item **args= func->arguments();
      bool left_const= args[0]->const_item() && !args[0]->is_expensive();
      bool right_const= args[1]->const_item() && !args[1]->is_expensive();
      if (!(left_const && right_const) &&
          args[0]->cmp_type() == args[1]->cmp_type())
      {
	if (right_const)
	{
          resolve_const_item(thd, &args[1], args[0]);
	  func->update_used_tables();
          change_cond_ref_to_const(thd, save_list, and_father, and_father,
                                   func, args[0], args[1]);
	}
	else if (left_const)
	{
          resolve_const_item(thd, &args[0], args[1]);
	  func->update_used_tables();
          change_cond_ref_to_const(thd, save_list, and_father, and_father,
                                   func, args[1], args[0]);
	}
      }
    }
  }
}

/**
  Simplify joins replacing outer joins by inner joins whenever it's
  possible.

    The function, during a retrieval of join_list,  eliminates those
    outer joins that can be converted into inner join, possibly nested.
    It also moves the on expressions for the converted outer joins
    and from inner joins to conds.
    The function also calculates some attributes for nested joins:
    - used_tables    
    - not_null_tables
    - dep_tables.
    - on_expr_dep_tables
    The first two attributes are used to test whether an outer join can
    be substituted for an inner join. The third attribute represents the
    relation 'to be dependent on' for tables. If table t2 is dependent
    on table t1, then in any evaluated execution plan table access to
    table t2 must precede access to table t2. This relation is used also
    to check whether the query contains  invalid cross-references.
    The forth attribute is an auxiliary one and is used to calculate
    dep_tables.
    As the attribute dep_tables qualifies possibles orders of tables in the
    execution plan, the dependencies required by the straight join
    modifiers are reflected in this attribute as well.
    The function also removes all braces that can be removed from the join
    expression without changing its meaning.

  @note
    An outer join can be replaced by an inner join if the where condition
    or the on expression for an embedding nested join contains a conjunctive
    predicate rejecting null values for some attribute of the inner tables.

    E.g. in the query:    
    @code
      SELECT * FROM t1 LEFT JOIN t2 ON t2.a=t1.a WHERE t2.b < 5
    @endcode
    the predicate t2.b < 5 rejects nulls.
    The query is converted first to:
    @code
      SELECT * FROM t1 INNER JOIN t2 ON t2.a=t1.a WHERE t2.b < 5
    @endcode
    then to the equivalent form:
    @code
      SELECT * FROM t1, t2 ON t2.a=t1.a WHERE t2.b < 5 AND t2.a=t1.a
    @endcode


    Similarly the following query:
    @code
      SELECT * from t1 LEFT JOIN (t2, t3) ON t2.a=t1.a t3.b=t1.b
        WHERE t2.c < 5  
    @endcode
    is converted to:
    @code
      SELECT * FROM t1, (t2, t3) WHERE t2.c < 5 AND t2.a=t1.a t3.b=t1.b 

    @endcode

    One conversion might trigger another:
    @code
      SELECT * FROM t1 LEFT JOIN t2 ON t2.a=t1.a
                       LEFT JOIN t3 ON t3.b=t2.b
        WHERE t3 IS NOT NULL =>
      SELECT * FROM t1 LEFT JOIN t2 ON t2.a=t1.a, t3
        WHERE t3 IS NOT NULL AND t3.b=t2.b => 
      SELECT * FROM t1, t2, t3
        WHERE t3 IS NOT NULL AND t3.b=t2.b AND t2.a=t1.a
  @endcode

    The function removes all unnecessary braces from the expression
    produced by the conversions.
    E.g.
    @code
      SELECT * FROM t1, (t2, t3) WHERE t2.c < 5 AND t2.a=t1.a AND t3.b=t1.b
    @endcode
    finally is converted to: 
    @code
      SELECT * FROM t1, t2, t3 WHERE t2.c < 5 AND t2.a=t1.a AND t3.b=t1.b

    @endcode


    It also will remove braces from the following queries:
    @code
      SELECT * from (t1 LEFT JOIN t2 ON t2.a=t1.a) LEFT JOIN t3 ON t3.b=t2.b
      SELECT * from (t1, (t2,t3)) WHERE t1.a=t2.a AND t2.b=t3.b.
    @endcode

    The benefit of this simplification procedure is that it might return 
    a query for which the optimizer can evaluate execution plan with more
    join orders. With a left join operation the optimizer does not
    consider any plan where one of the inner tables is before some of outer
    tables.

  IMPLEMENTATION
    The function is implemented by a recursive procedure.  On the recursive
    ascent all attributes are calculated, all outer joins that can be
    converted are replaced and then all unnecessary braces are removed.
    As join list contains join tables in the reverse order sequential
    elimination of outer joins does not require extra recursive calls.

  SEMI-JOIN NOTES
    Remove all semi-joins that have are within another semi-join (i.e. have
    an "ancestor" semi-join nest)

  EXAMPLES
    Here is an example of a join query with invalid cross references:
    @code
      SELECT * FROM t1 LEFT JOIN t2 ON t2.a=t3.a LEFT JOIN t3 ON t3.b=t1.b 
    @endcode

  @param join        reference to the query info
  @param join_list   list representation of the join to be converted
  @param conds       conditions to add on expressions for converted joins
  @param top         true <=> conds is the where condition
  @param in_sj       TRUE <=> processing semi-join nest's children
  @return
    - The new condition, if success
    - 0, otherwise
*/

static COND *
simplify_joins(JOIN *join, List<TABLE_LIST> *join_list, COND *conds, bool top,
               bool in_sj)
{
  TABLE_LIST *table;
  NESTED_JOIN *nested_join;
  TABLE_LIST *prev_table= 0;
  List_iterator<TABLE_LIST> li(*join_list);
  bool straight_join= MY_TEST(join->select_options & SELECT_STRAIGHT_JOIN);
  DBUG_ENTER("simplify_joins");

  /* 
    Try to simplify join operations from join_list.
    The most outer join operation is checked for conversion first. 
  */
  while ((table= li++))
  {
    table_map used_tables;
    table_map not_null_tables= (table_map) 0;

    if ((nested_join= table->nested_join))
    {
      /* 
         If the element of join_list is a nested join apply
         the procedure to its nested join list first.
      */
      if (table->on_expr)
      {
        Item *expr= table->on_expr;
        /* 
           If an on expression E is attached to the table, 
           check all null rejected predicates in this expression.
           If such a predicate over an attribute belonging to
           an inner table of an embedded outer join is found,
           the outer join is converted to an inner join and
           the corresponding on expression is added to E. 
	*/ 
        expr= simplify_joins(join, &nested_join->join_list,
                             expr, FALSE, in_sj || table->sj_on_expr);

        if (!table->prep_on_expr || expr != table->on_expr)
        {
          DBUG_ASSERT(expr);

          table->on_expr= expr;
          table->prep_on_expr= expr->copy_andor_structure(join->thd);
        }
      }
      nested_join->used_tables= (table_map) 0;
      nested_join->not_null_tables=(table_map) 0;
      conds= simplify_joins(join, &nested_join->join_list, conds, top, 
                            in_sj || table->sj_on_expr);
      used_tables= nested_join->used_tables;
      not_null_tables= nested_join->not_null_tables;  
      /* The following two might become unequal after table elimination: */
      nested_join->n_tables= nested_join->join_list.elements;
    }
    else
    {
      if (!table->prep_on_expr)
        table->prep_on_expr= table->on_expr;
      used_tables= table->get_map();
      if (conds)
        not_null_tables= conds->not_null_tables();
    }
      
    if (table->embedding)
    {
      table->embedding->nested_join->used_tables|= used_tables;
      table->embedding->nested_join->not_null_tables|= not_null_tables;
    }

    if (!(table->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT)) ||
        (used_tables & not_null_tables))
    {
      /* 
        For some of the inner tables there are conjunctive predicates
        that reject nulls => the outer join can be replaced by an inner join.
      */
      if (table->outer_join && !table->embedding && table->table)
        table->table->maybe_null= FALSE;
      table->outer_join= 0;
      if (!(straight_join || table->straight))
      {
        table->dep_tables= 0;
        TABLE_LIST *embedding= table->embedding;
        while (embedding)
        {
          if (embedding->nested_join->join_list.head()->outer_join)
          {
            if (!embedding->sj_subq_pred)
              table->dep_tables= embedding->dep_tables;
            break;
          }
          embedding= embedding->embedding;
        }
      }
      if (table->on_expr)
      {
        /* Add ON expression to the WHERE or upper-level ON condition. */
        if (conds)
        {
          conds= and_conds(join->thd, conds, table->on_expr);
          conds->top_level_item();
          /* conds is always a new item as both cond and on_expr existed */
          DBUG_ASSERT(!conds->fixed);
          conds->fix_fields(join->thd, &conds);
        }
        else
          conds= table->on_expr; 
        table->prep_on_expr= table->on_expr= 0;
      }
    }

    /* 
      Only inner tables of non-convertible outer joins
      remain with on_expr.
    */ 
    if (table->on_expr)
    {
      table_map table_on_expr_used_tables= table->on_expr->used_tables();
      table->dep_tables|= table_on_expr_used_tables;
      if (table->embedding)
      {
        table->dep_tables&= ~table->embedding->nested_join->used_tables;   
        /*
           Embedding table depends on tables used
           in embedded on expressions. 
        */
        table->embedding->on_expr_dep_tables|= table_on_expr_used_tables;
      }
      else
        table->dep_tables&= ~table->get_map();
    }

    if (prev_table)
    {
      /* The order of tables is reverse: prev_table follows table */
      if (prev_table->straight || straight_join)
        prev_table->dep_tables|= used_tables;
      if (prev_table->on_expr)
      {
        prev_table->dep_tables|= table->on_expr_dep_tables;
        table_map prev_used_tables= prev_table->nested_join ?
	                            prev_table->nested_join->used_tables :
	                            prev_table->get_map();
        /* 
          If on expression contains only references to inner tables
          we still make the inner tables dependent on the outer tables.
          It would be enough to set dependency only on one outer table
          for them. Yet this is really a rare case.
          Note:
          RAND_TABLE_BIT mask should not be counted as it
          prevents update of inner table dependences.
          For example it might happen if RAND() function
          is used in JOIN ON clause.
	*/  
        if (!((prev_table->on_expr->used_tables() &
               ~(OUTER_REF_TABLE_BIT | RAND_TABLE_BIT)) &
              ~prev_used_tables))
          prev_table->dep_tables|= used_tables;
      }
    }
    prev_table= table;
  }
    
  /* 
    Flatten nested joins that can be flattened.
    no ON expression and not a semi-join => can be flattened.
  */
  li.rewind();
  while ((table= li++))
  {
    nested_join= table->nested_join;
    if (table->sj_on_expr && !in_sj)
    {
      /*
        If this is a semi-join that is not contained within another semi-join
        leave it intact (otherwise it is flattened)
      */
      /*
        Make sure that any semi-join appear in
        the join->select_lex->sj_nests list only once
      */
      List_iterator_fast<TABLE_LIST> sj_it(join->select_lex->sj_nests);
      TABLE_LIST *sj_nest;
      while ((sj_nest= sj_it++))
      {
        if (table == sj_nest)
          break;
      }
      if (sj_nest)
        continue;
      join->select_lex->sj_nests.push_back(table, join->thd->mem_root);

      /* 
        Also, walk through semi-join children and mark those that are now
        top-level
      */
      TABLE_LIST *tbl;
      List_iterator<TABLE_LIST> it(nested_join->join_list);
      while ((tbl= it++))
      {
        if (!tbl->on_expr && tbl->table)
          tbl->table->maybe_null= FALSE;
      }
    }
    else if (nested_join && !table->on_expr)
    {
      TABLE_LIST *tbl;
      List_iterator<TABLE_LIST> it(nested_join->join_list);
      List<TABLE_LIST> repl_list;  
      while ((tbl= it++))
      {
        tbl->embedding= table->embedding;
        if (!tbl->embedding && !tbl->on_expr && tbl->table)
          tbl->table->maybe_null= FALSE;
        tbl->join_list= table->join_list;
        repl_list.push_back(tbl, join->thd->mem_root);
        tbl->dep_tables|= table->dep_tables;
      }
      li.replace(repl_list);
    }
  }
  DBUG_RETURN(conds); 
}


/**
  Assign each nested join structure a bit in nested_join_map.

    Assign each nested join structure (except ones that embed only one element
    and so are redundant) a bit in nested_join_map.

  @param join          Join being processed
  @param join_list     List of tables
  @param first_unused  Number of first unused bit in nested_join_map before the
                       call

  @note
    This function is called after simplify_joins(), when there are no
    redundant nested joins, #non_redundant_nested_joins <= #tables_in_join so
    we will not run out of bits in nested_join_map.

  @return
    First unused bit in nested_join_map after the call.
*/

static uint build_bitmap_for_nested_joins(List<TABLE_LIST> *join_list, 
                                          uint first_unused)
{
  List_iterator<TABLE_LIST> li(*join_list);
  TABLE_LIST *table;
  DBUG_ENTER("build_bitmap_for_nested_joins");
  while ((table= li++))
  {
    NESTED_JOIN *nested_join;
    if ((nested_join= table->nested_join))
    {
      /*
        It is guaranteed by simplify_joins() function that a nested join
        that has only one child represents a single table VIEW (and the child
        is an underlying table). We don't assign bits to such nested join
        structures because 
        1. it is redundant (a "sequence" of one table cannot be interleaved 
            with anything)
        2. we could run out bits in nested_join_map otherwise.
      */
      if (nested_join->n_tables != 1)
      {
        /* Don't assign bits to sj-nests */
        if (table->on_expr)
          nested_join->nj_map= (nested_join_map) 1 << first_unused++;
        first_unused= build_bitmap_for_nested_joins(&nested_join->join_list,
                                                    first_unused);
      }
    }
  }
  DBUG_RETURN(first_unused);
}


/**
  Set NESTED_JOIN::counter and n_tables in all nested joins in passed list.

  For all nested joins contained in the passed join_list (including its
  children), set:
   - nested_join->counter=0
   - nested_join->n_tables= {number of non-degenerate direct children}.

  Non-degenerate means non-const base table or a join nest that has a
  non-degenerate child.

  @param join_list  List of nested joins to process. It may also contain base
                    tables which will be ignored.
*/

static uint reset_nj_counters(JOIN *join, List<TABLE_LIST> *join_list)
{
  List_iterator<TABLE_LIST> li(*join_list);
  TABLE_LIST *table;
  DBUG_ENTER("reset_nj_counters");
  uint n=0;
  while ((table= li++))
  {
    NESTED_JOIN *nested_join;
    bool is_eliminated_nest= FALSE;
    if ((nested_join= table->nested_join))
    {
      nested_join->counter= 0;
      nested_join->n_tables= reset_nj_counters(join, &nested_join->join_list);
      if (!nested_join->n_tables)
        is_eliminated_nest= TRUE;
    }
    const table_map removed_tables= join->eliminated_tables |
                                    join->const_table_map;

    if ((table->nested_join && !is_eliminated_nest) ||
        (!table->nested_join && (table->table->map & ~removed_tables)))
      n++;
  }
  DBUG_RETURN(n);
}


/**
  Check interleaving with an inner tables of an outer join for
  extension table.

    Check if table next_tab can be added to current partial join order, and 
    if yes, record that it has been added.

    The function assumes that both current partial join order and its
    extension with next_tab are valid wrt table dependencies.

  @verbatim
     IMPLEMENTATION 
       LIMITATIONS ON JOIN ORDER
         The nested [outer] joins executioner algorithm imposes these limitations
         on join order:
         1. "Outer tables first" -  any "outer" table must be before any 
             corresponding "inner" table.
         2. "No interleaving" - tables inside a nested join must form a continuous
            sequence in join order (i.e. the sequence must not be interrupted by 
            tables that are outside of this nested join).

         #1 is checked elsewhere, this function checks #2 provided that #1 has
         been already checked.

       WHY NEED NON-INTERLEAVING
         Consider an example: 

           select * from t0 join t1 left join (t2 join t3) on cond1

         The join order "t1 t2 t0 t3" is invalid:

         table t0 is outside of the nested join, so WHERE condition for t0 is
         attached directly to t0 (without triggers, and it may be used to access
         t0). Applying WHERE(t0) to (t2,t0,t3) record is invalid as we may miss
         combinations of (t1, t2, t3) that satisfy condition cond1, and produce a
         null-complemented (t1, t2.NULLs, t3.NULLs) row, which should not have
         been produced.

         If table t0 is not between t2 and t3, the problem doesn't exist:
          If t0 is located after (t2,t3), WHERE(t0) is applied after nested join
           processing has finished.
          If t0 is located before (t2,t3), predicates like WHERE_cond(t0, t2) are
           wrapped into condition triggers, which takes care of correct nested
           join processing.

       HOW IT IS IMPLEMENTED
         The limitations on join order can be rephrased as follows: for valid
         join order one must be able to:
           1. write down the used tables in the join order on one line.
           2. for each nested join, put one '(' and one ')' on the said line        
           3. write "LEFT JOIN" and "ON (...)" where appropriate
           4. get a query equivalent to the query we're trying to execute.

         Calls to check_interleaving_with_nj() are equivalent to writing the
         above described line from left to right. 
         A single check_interleaving_with_nj(A,B) call is equivalent to writing 
         table B and appropriate brackets on condition that table A and
         appropriate brackets is the last what was written. Graphically the
         transition is as follows:

                              +---- current position
                              |
             ... last_tab ))) | ( next_tab )  )..) | ...
                                X          Y   Z   |
                                                   +- need to move to this
                                                      position.

         Notes about the position:
           The caller guarantees that there is no more then one X-bracket by 
           checking "!(remaining_tables & s->dependent)" before calling this 
           function. X-bracket may have a pair in Y-bracket.

         When "writing" we store/update this auxilary info about the current
         position:
          1. join->cur_embedding_map - bitmap of pairs of brackets (aka nested
             joins) we've opened but didn't close.
          2. {each NESTED_JOIN structure not simplified away}->counter - number
             of this nested join's children that have already been added to to
             the partial join order.
  @endverbatim

  @param next_tab   Table we're going to extend the current partial join with

  @retval
    FALSE  Join order extended, nested joins info about current join
    order (see NOTE section) updated.
  @retval
    TRUE   Requested join order extension not allowed.
*/

static bool check_interleaving_with_nj(JOIN_TAB *next_tab)
{
  TABLE_LIST *next_emb= next_tab->table->pos_in_table_list->embedding;
  JOIN *join= next_tab->join;

  if (join->cur_embedding_map & ~next_tab->embedding_map)
  {
    /* 
      next_tab is outside of the "pair of brackets" we're currently in.
      Cannot add it.
    */
    return TRUE;
  }
   
  /*
    Do update counters for "pairs of brackets" that we've left (marked as
    X,Y,Z in the above picture)
  */
  for (;next_emb && next_emb != join->emb_sjm_nest; next_emb= next_emb->embedding)
  {
    if (!next_emb->sj_on_expr)
    {
      next_emb->nested_join->counter++;
      if (next_emb->nested_join->counter == 1)
      {
        /* 
          next_emb is the first table inside a nested join we've "entered". In
          the picture above, we're looking at the 'X' bracket. Don't exit yet as
          X bracket might have Y pair bracket.
        */
        join->cur_embedding_map |= next_emb->nested_join->nj_map;
      }
      
      if (next_emb->nested_join->n_tables !=
          next_emb->nested_join->counter)
        break;

      /*
        We're currently at Y or Z-bracket as depicted in the above picture.
        Mark that we've left it and continue walking up the brackets hierarchy.
      */
      join->cur_embedding_map &= ~next_emb->nested_join->nj_map;
    }
  }
  return FALSE;
}


/**
  Nested joins perspective: Remove the last table from the join order.

  The algorithm is the reciprocal of check_interleaving_with_nj(), hence
  parent join nest nodes are updated only when the last table in its child
  node is removed. The ASCII graphic below will clarify.

  %A table nesting such as <tt> t1 x [ ( t2 x t3 ) x ( t4 x t5 ) ] </tt>is
  represented by the below join nest tree.

  @verbatim
                     NJ1
                  _/ /  \
                _/  /    NJ2
              _/   /     / \ 
             /    /     /   \
   t1 x [ (t2 x t3) x (t4 x t5) ]
  @endverbatim

  At the point in time when check_interleaving_with_nj() adds the table t5 to
  the query execution plan, QEP, it also directs the node named NJ2 to mark
  the table as covered. NJ2 does so by incrementing its @c counter
  member. Since all of NJ2's tables are now covered by the QEP, the algorithm
  proceeds up the tree to NJ1, incrementing its counter as well. All join
  nests are now completely covered by the QEP.

  restore_prev_nj_state() does the above in reverse. As seen above, the node
  NJ1 contains the nodes t2, t3, and NJ2. Its counter being equal to 3 means
  that the plan covers t2, t3, and NJ2, @e and that the sub-plan (t4 x t5)
  completely covers NJ2. The removal of t5 from the partial plan will first
  decrement NJ2's counter to 1. It will then detect that NJ2 went from being
  completely to partially covered, and hence the algorithm must continue
  upwards to NJ1 and decrement its counter to 2. %A subsequent removal of t4
  will however not influence NJ1 since it did not un-cover the last table in
  NJ2.

  SYNOPSIS
    restore_prev_nj_state()
      last  join table to remove, it is assumed to be the last in current 
            partial join order.
     
  DESCRIPTION

    Remove the last table from the partial join order and update the nested
    joins counters and join->cur_embedding_map. It is ok to call this 
    function for the first table in join order (for which 
    check_interleaving_with_nj has not been called)

  @param last  join table to remove, it is assumed to be the last in current
               partial join order.
*/

static void restore_prev_nj_state(JOIN_TAB *last)
{
  TABLE_LIST *last_emb= last->table->pos_in_table_list->embedding;
  JOIN *join= last->join;
  for (;last_emb != NULL && last_emb != join->emb_sjm_nest; 
       last_emb= last_emb->embedding)
  {
    if (!last_emb->sj_on_expr)
    {
      NESTED_JOIN *nest= last_emb->nested_join;
      DBUG_ASSERT(nest->counter > 0);
      
      bool was_fully_covered= nest->is_fully_covered();
      
      join->cur_embedding_map|= nest->nj_map;

      if (--nest->counter == 0)
        join->cur_embedding_map&= ~nest->nj_map;
      
      if (!was_fully_covered)
        break;
    }
  }
}



/*
  Change access methods not to use join buffering and adjust costs accordingly

  SYNOPSIS
    optimize_wo_join_buffering()
      join
      first_tab               The first tab to do re-optimization for
      last_tab                The last tab to do re-optimization for
      last_remaining_tables   Bitmap of tables that are not in the
                              [0...last_tab] join prefix
      first_alt               TRUE <=> Use the LooseScan plan for the first_tab
      no_jbuf_before          Don't allow to use join buffering before this
                              table
      reopt_rec_count     OUT New output record count
      reopt_cost          OUT New join prefix cost

  DESCRIPTION
    Given a join prefix [0; ... first_tab], change the access to the tables
    in the [first_tab; last_tab] not to use join buffering. This is needed
    because some semi-join strategies cannot be used together with the join
    buffering.
    In general case the best table order in [first_tab; last_tab] range with
    join buffering is different from the best order without join buffering but
    we don't try finding a better join order. (TODO ask Igor why did we
    chose not to do this in the end. that's actually the difference from the 
    forking approach)
*/

void optimize_wo_join_buffering(JOIN *join, uint first_tab, uint last_tab, 
                                table_map last_remaining_tables, 
                                bool first_alt, uint no_jbuf_before,
                                double *outer_rec_count, double *reopt_cost)
{
  double cost, rec_count;
  table_map reopt_remaining_tables= last_remaining_tables;
  uint i;

  if (first_tab > join->const_tables)
  {
    cost=      join->positions[first_tab - 1].prefix_cost.total_cost();
    rec_count= join->positions[first_tab - 1].prefix_record_count;
  }
  else
  {
    cost= 0.0;
    rec_count= 1;
  }

  *outer_rec_count= rec_count;
  for (i= first_tab; i <= last_tab; i++)
    reopt_remaining_tables |= join->positions[i].table->table->map;
  
  /*
    best_access_path() optimization depends on the value of 
    join->cur_sj_inner_tables. Our goal in this function is to do a
    re-optimization with disabled join buffering, but no other changes.
    In order to achieve this, cur_sj_inner_tables needs have the same 
    value it had during the original invocations of best_access_path. 

    We know that this function, optimize_wo_join_buffering() is called to
    re-optimize semi-join join order range, which allows to conclude that 
    the "original" value of cur_sj_inner_tables was 0.
  */
  table_map save_cur_sj_inner_tables= join->cur_sj_inner_tables;
  join->cur_sj_inner_tables= 0;

  for (i= first_tab; i <= last_tab; i++)
  {
    JOIN_TAB *rs= join->positions[i].table;
    POSITION pos, loose_scan_pos;
    
    if ((i == first_tab && first_alt) || join->positions[i].use_join_buffer)
    {
      /* Find the best access method that would not use join buffering */
      best_access_path(join, rs, reopt_remaining_tables,
                       join->positions, i,
                       TRUE, rec_count,
                       &pos, &loose_scan_pos);
    }
    else 
      pos= join->positions[i];

    if ((i == first_tab && first_alt))
      pos= loose_scan_pos;

    reopt_remaining_tables &= ~rs->table->map;
    rec_count= COST_MULT(rec_count, pos.records_read);
    cost= COST_ADD(cost, pos.read_time);
    cost= COST_ADD(cost, rec_count / (double) TIME_FOR_COMPARE);
    //TODO: take into account join condition selectivity here
    double pushdown_cond_selectivity= 1.0;
    table_map real_table_bit= rs->table->map;
    if (join->thd->variables.optimizer_use_condition_selectivity > 1)
    {
      pushdown_cond_selectivity= table_cond_selectivity(join, i, rs,
                                                        reopt_remaining_tables &
                                                        ~real_table_bit);
    }
    (*outer_rec_count) *= pushdown_cond_selectivity;
    if (!rs->emb_sj_nest)
      *outer_rec_count= COST_MULT(*outer_rec_count, pos.records_read);

  }
  join->cur_sj_inner_tables= save_cur_sj_inner_tables;

  *reopt_cost= cost;
}


static COND *
optimize_cond(JOIN *join, COND *conds,
              List<TABLE_LIST> *join_list, bool ignore_on_conds,
              Item::cond_result *cond_value, COND_EQUAL **cond_equal,
              int flags)
{
  THD *thd= join->thd;
  DBUG_ENTER("optimize_cond");

  if (!conds)
  {
    *cond_value= Item::COND_TRUE;
    if (!ignore_on_conds)
      build_equal_items(join, NULL, NULL, join_list, ignore_on_conds,
                        cond_equal);
  }  
  else
  {
    /* 
      Build all multiple equality predicates and eliminate equality
      predicates that can be inferred from these multiple equalities.
      For each reference of a field included into a multiple equality
      that occurs in a function set a pointer to the multiple equality
      predicate. Substitute a constant instead of this field if the
      multiple equality contains a constant.
    */ 
    DBUG_EXECUTE("where", print_where(conds, "original", QT_ORDINARY););
    conds= build_equal_items(join, conds, NULL, join_list, 
                             ignore_on_conds, cond_equal,
                             MY_TEST(flags & OPT_LINK_EQUAL_FIELDS));
    DBUG_EXECUTE("where",print_where(conds,"after equal_items", QT_ORDINARY););

    /* change field = field to field = const for each found field = const */
    propagate_cond_constants(thd, (I_List<COND_CMP> *) 0, conds, conds);
    /*
      Remove all instances of item == item
      Remove all and-levels where CONST item != CONST item
    */
    DBUG_EXECUTE("where",print_where(conds,"after const change", QT_ORDINARY););
    conds= conds->remove_eq_conds(thd, cond_value, true);
    if (conds && conds->type() == Item::COND_ITEM &&
        ((Item_cond*) conds)->functype() == Item_func::COND_AND_FUNC)
      *cond_equal= &((Item_cond_and*) conds)->m_cond_equal;
    DBUG_EXECUTE("info",print_where(conds,"after remove", QT_ORDINARY););
  }
  DBUG_RETURN(conds);
}


/**
  @brief
  Propagate multiple equalities to the sub-expressions of a condition

  @param thd             thread handle
  @param cond            the condition where equalities are to be propagated
  @param *new_equalities the multiple equalities to be propagated
  @param inherited        path to all inherited multiple equality items
  @param[out] is_simplifiable_cond   'cond' may be simplified after the
                                      propagation of the equalities
 
  @details
  The function recursively traverses the tree of the condition 'cond' and
  for each its AND sub-level of any depth the function merges the multiple
  equalities from the list 'new_equalities' into the multiple equalities
  attached to the AND item created for this sub-level.
  The function also [re]sets references to the equalities formed by the
  merges of multiple equalities in all field items occurred in 'cond'
  that are encountered in the equalities.
  If the result of any merge of multiple equalities is an impossible
  condition the function returns TRUE in the parameter is_simplifiable_cond.   
*/

void propagate_new_equalities(THD *thd, Item *cond,
                              List<Item_equal> *new_equalities,
                              COND_EQUAL *inherited,
                              bool *is_simplifiable_cond)
{
  if (cond->type() == Item::COND_ITEM)
  {
    bool and_level= ((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC;
    if (and_level)
    {
      Item_cond_and *cond_and= (Item_cond_and *) cond; 
      List<Item_equal> *cond_equalities= &cond_and->m_cond_equal.current_level;
      cond_and->m_cond_equal.upper_levels= inherited;
      if (!cond_equalities->is_empty() && cond_equalities != new_equalities)
      {
        Item_equal *equal_item;
        List_iterator<Item_equal> it(*new_equalities);
	while ((equal_item= it++))
	{
          equal_item->merge_into_list(thd, cond_equalities, true, true);
        }
        List_iterator<Item_equal> ei(*cond_equalities);
        while ((equal_item= ei++))
	{
          if (equal_item->const_item() && !equal_item->val_int())
	  {
            *is_simplifiable_cond= true;
            return;
          }
        }
      }
    }

    Item *item;
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    while ((item= li++))
    {
      COND_EQUAL *new_inherited= and_level && item->type() == Item::COND_ITEM ?
                                   &((Item_cond_and *) cond)->m_cond_equal :
                                   inherited;
      propagate_new_equalities(thd, item, new_equalities, new_inherited,
                               is_simplifiable_cond);
    }
  }
  else if (cond->type() == Item::FUNC_ITEM && 
           ((Item_cond*) cond)->functype() == Item_func::MULT_EQUAL_FUNC)
  {
    Item_equal *equal_item;
    List_iterator<Item_equal> it(*new_equalities);
    Item_equal *equality= (Item_equal *) cond;
    equality->upper_levels= inherited;
    while ((equal_item= it++))
    {
      equality->merge_with_check(thd, equal_item, true);
    }
    if (equality->const_item() && !equality->val_int())
      *is_simplifiable_cond= true;
  }
  else
  {
    cond= cond->propagate_equal_fields(thd,
                                       Item::Context_boolean(), inherited);
    cond->update_used_tables();
  }          
} 

/*
  Check if cond_is_datetime_is_null() is true for the condition cond, or 
  for any of its AND/OR-children
*/
bool cond_has_datetime_is_null(Item *cond)
{
  if (cond_is_datetime_is_null(cond))
    return true;

  if (cond->type() == Item::COND_ITEM)
  {
    List<Item> *cond_arg_list= ((Item_cond*) cond)->argument_list();
    List_iterator<Item> li(*cond_arg_list);
    Item *item;
    while ((item= li++))
    {
      if (cond_has_datetime_is_null(item))
        return true;
    }
  }
  return false;
}

/*
  Check if passed condtition has for of

    not_null_date_col IS NULL

  where not_null_date_col has a datte or datetime type
*/

bool cond_is_datetime_is_null(Item *cond)
{
  if (cond->type() == Item::FUNC_ITEM &&
      ((Item_func*) cond)->functype() == Item_func::ISNULL_FUNC)
  {
    return ((Item_func_isnull*) cond)->arg_is_datetime_notnull_field();
  }
  return false;
}


/**
  @brief
  Evaluate all constant boolean sub-expressions in a condition
 
  @param thd        thread handle
  @param cond       condition where where to evaluate constant sub-expressions
  @param[out] cond_value : the returned value of the condition 
                           (TRUE/FALSE/UNKNOWN:
                           Item::COND_TRUE/Item::COND_FALSE/Item::COND_OK)
  @return
   the item that is the result of the substitution of all inexpensive constant
   boolean sub-expressions into cond, or,
   NULL if the condition is constant and is evaluated to FALSE.

  @details
  This function looks for all inexpensive constant boolean sub-expressions in
  the given condition 'cond' and substitutes them for their values.
  For example, the condition 2 > (5 + 1) or a < (10 / 2)
  will be transformed to the condition a < (10 / 2).
  Note that a constant sub-expression is evaluated only if it is constant and
  inexpensive. A sub-expression with an uncorrelated subquery may be evaluated
  only if the subquery is considered as inexpensive.
  The function does not evaluate a constant sub-expression if it is not on one
  of AND/OR levels of the condition 'cond'. For example, the subquery in the
  condition a > (select max(b) from t1 where b > 5) will never be evaluated
  by this function. 
  If a constant boolean sub-expression is evaluated to TRUE then:
    - when the sub-expression is a conjunct of an AND formula it is simply
      removed from this formula
    - when the sub-expression is a disjunct of an OR formula the whole OR
      formula is converted to TRUE 
  If a constant boolean sub-expression is evaluated to FALSE then:
    - when the sub-expression is a disjunct of an OR formula it is simply
      removed from this formula
    - when the sub-expression is a conjuct of an AND formula the whole AND
      formula is converted to FALSE
  When a disjunct/conjunct is removed from an OR/AND formula it might happen
  that there is only one conjunct/disjunct remaining. In this case this
  remaining disjunct/conjunct must be merged into underlying AND/OR formula,
  because AND/OR levels must alternate in the same way as they alternate
  after fix_fields() is called for the original condition.
  The specifics of merging a formula f into an AND formula A appears
  when A contains multiple equalities and f contains multiple equalities.
  In this case the multiple equalities from f and A have to be merged.
  After this the resulting multiple equalities have to be propagated into
  the all AND/OR levels of the formula A (see propagate_new_equalities()).
  The propagation of multiple equalities might result in forming multiple
  equalities that are always FALSE. This, in its turn, might trigger further
  simplification of the condition.

  @note
  EXAMPLE 1:
  SELECT * FROM t1 WHERE (b = 1 OR a = 1) AND (b = 5 AND a = 5 OR 1 != 1);
  First 1 != 1 will be removed from the second conjunct:
  => SELECT * FROM t1 WHERE (b = 1 OR a = 1) AND (b = 5 AND a = 5);
  Then (b = 5 AND a = 5) will be merged into the top level condition:
  => SELECT * FROM t1 WHERE (b = 1 OR a = 1) AND (b = 5) AND (a = 5);
  Then (b = 5), (a = 5)  will be propagated into the disjuncs of 
  (b = 1 OR a = 1):
  => SELECT * FROM t1 WHERE ((b = 1) AND (b = 5) AND (a = 5) OR
                             (a = 1) AND (b = 5) AND (a = 5)) AND
                            (b = 5) AND (a = 5)
  => SELECT * FROM t1 WHERE ((FALSE AND (a = 5)) OR
                             (FALSE AND (b = 5))) AND
                             (b = 5) AND (a = 5)
  After this an additional call of remove_eq_conds() converts it
  to FALSE

  EXAMPLE 2:  
  SELECT * FROM t1 WHERE (b = 1 OR a = 5) AND (b = 5 AND a = 5 OR 1 != 1);
  => SELECT * FROM t1 WHERE (b = 1 OR a = 5) AND (b = 5 AND a = 5);
  => SELECT * FROM t1 WHERE (b = 1 OR a = 5) AND (b = 5) AND (a = 5);
  => SELECT * FROM t1 WHERE ((b = 1) AND (b = 5) AND (a = 5) OR
                             (a = 5) AND (b = 5) AND (a = 5)) AND
                            (b = 5) AND (a = 5)
  => SELECT * FROM t1 WHERE ((FALSE AND (a = 5)) OR
                             ((b = 5) AND (a = 5))) AND
                             (b = 5) AND (a = 5)
  After this an additional call of  remove_eq_conds() converts it to
 =>  SELECT * FROM t1 WHERE (b = 5) AND (a = 5)                            
*/


COND *
Item_cond::remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                           bool top_level_arg)
{
  bool and_level= functype() == Item_func::COND_AND_FUNC;
  List<Item> *cond_arg_list= argument_list();

  if (and_level)
  {
    /*
      Remove multiple equalities that became always true (e.g. after
      constant row substitution).
      They would be removed later in the function anyway, but the list of
      them cond_equal.current_level also  must be adjusted correspondingly.
      So it's easier  to do it at one pass through the list of the equalities.
    */
     List<Item_equal> *cond_equalities=
      &((Item_cond_and *) this)->m_cond_equal.current_level;
     cond_arg_list->disjoin((List<Item> *) cond_equalities);
     List_iterator<Item_equal> it(*cond_equalities);
     Item_equal *eq_item;
     while ((eq_item= it++))
     {
       if (eq_item->const_item() && eq_item->val_int())
         it.remove();
     }
     cond_arg_list->append((List<Item> *) cond_equalities);
  }

  List<Item_equal> new_equalities;
  List_iterator<Item> li(*cond_arg_list);
  bool should_fix_fields= 0;
  Item::cond_result tmp_cond_value;
  Item *item;

  /*
    If the list cond_arg_list became empty then it consisted only
    of always true multiple equalities.
  */
  *cond_value= cond_arg_list->elements ? Item::COND_UNDEF : Item::COND_TRUE;

  while ((item=li++))
  {
    Item *new_item= item->remove_eq_conds(thd, &tmp_cond_value, false);
    if (!new_item)
    {
      /* This can happen only when item is converted to TRUE or FALSE */
      li.remove();
    }
    else if (item != new_item)
    {
      /*
        This can happen when:
        - item was an OR formula converted to one disjunct
        - item was an AND formula converted to one conjunct
        In these cases the disjunct/conjunct must be merged into the
        argument list of cond.
      */
      if (new_item->type() == Item::COND_ITEM &&
          item->type() == Item::COND_ITEM)
      {
        DBUG_ASSERT(functype() == ((Item_cond *) new_item)->functype());
        List<Item> *new_item_arg_list=
          ((Item_cond *) new_item)->argument_list();
        if (and_level)
        {
          /*
            If new_item is an AND formula then multiple equalities
            of new_item_arg_list must merged into multiple equalities
            of cond_arg_list.
          */
          List<Item_equal> *new_item_equalities=
            &((Item_cond_and *) new_item)->m_cond_equal.current_level;
          if (!new_item_equalities->is_empty())
          {
            /*
              Cut the multiple equalities from the new_item_arg_list and
              append them on the list new_equalities. Later the equalities
              from this list will be merged into the multiple equalities
              of cond_arg_list all together.
            */
            new_item_arg_list->disjoin((List<Item> *) new_item_equalities);
            new_equalities.append(new_item_equalities);
          }
        }
        if (new_item_arg_list->is_empty())
          li.remove();
        else
        {
          uint cnt= new_item_arg_list->elements;
          li.replace(*new_item_arg_list);
          /* Make iterator li ignore new items */
          for (cnt--; cnt; cnt--)
            li++;
          should_fix_fields= 1;
        }
      }
      else if (and_level &&
               new_item->type() == Item::FUNC_ITEM &&
               ((Item_cond*) new_item)->functype() ==
                Item_func::MULT_EQUAL_FUNC)
      {
        li.remove();
        new_equalities.push_back((Item_equal *) new_item, thd->mem_root);
      }
      else
      {
        if (new_item->type() == Item::COND_ITEM &&
            ((Item_cond*) new_item)->functype() ==  functype())
        {
          List<Item> *new_item_arg_list=
            ((Item_cond *) new_item)->argument_list();
          uint cnt= new_item_arg_list->elements;
          li.replace(*new_item_arg_list);
          /* Make iterator li ignore new items */
          for (cnt--; cnt; cnt--)
            li++;
        }
        else
          li.replace(new_item);
        should_fix_fields= 1;
      }
    }
    if (*cond_value == Item::COND_UNDEF)
      *cond_value= tmp_cond_value;
    switch (tmp_cond_value) {
    case Item::COND_OK:                        // Not TRUE or FALSE
      if (and_level || *cond_value == Item::COND_FALSE)
        *cond_value=tmp_cond_value;
      break;
    case Item::COND_FALSE:
      if (and_level)
      {
        *cond_value= tmp_cond_value;
        return (COND*) 0;                        // Always false
      }
      break;
    case Item::COND_TRUE:
      if (!and_level)
      {
        *cond_value= tmp_cond_value;
        return (COND*) 0;                        // Always true
      }
      break;
    case Item::COND_UNDEF:                        // Impossible
      break; /* purecov: deadcode */
    }
  }
  COND *cond= this;
  if (!new_equalities.is_empty())
  {
    DBUG_ASSERT(and_level);
    /*
      Merge multiple equalities that were cut from the results of
      simplification of OR formulas converted into AND formulas.
      These multiple equalities are to be merged into the
      multiple equalities of  cond_arg_list.
    */
    COND_EQUAL *cond_equal= &((Item_cond_and *) this)->m_cond_equal;
    List<Item_equal> *cond_equalities= &cond_equal->current_level;
    cond_arg_list->disjoin((List<Item> *) cond_equalities);
    Item_equal *equality;
    List_iterator_fast<Item_equal> it(new_equalities);
    while ((equality= it++))
    {
      equality->upper_levels= cond_equal->upper_levels;
      equality->merge_into_list(thd, cond_equalities, false, false);
      List_iterator_fast<Item_equal> ei(*cond_equalities);
      while ((equality= ei++))
      {
        if (equality->const_item() && !equality->val_int())
        {
          *cond_value= Item::COND_FALSE;
          return (COND*) 0;
        }
      }
    }
    cond_arg_list->append((List<Item> *) cond_equalities);
    /*
      Propagate the newly formed multiple equalities to
      the all AND/OR levels of cond
    */
    bool is_simplifiable_cond= false;
    propagate_new_equalities(thd, this, cond_equalities,
                             cond_equal->upper_levels,
                             &is_simplifiable_cond);
    /*
      If the above propagation of multiple equalities brings us
      to multiple equalities that are always FALSE then try to
      simplify the condition with remove_eq_cond() again.
    */
    if (is_simplifiable_cond)
    {
      if (!(cond= cond->remove_eq_conds(thd, cond_value, false)))
        return cond;
    }
    should_fix_fields= 1;
  }
  if (should_fix_fields)
    cond->update_used_tables();

  if (!((Item_cond*) cond)->argument_list()->elements ||
      *cond_value != Item::COND_OK)
    return (COND*) 0;
  if (((Item_cond*) cond)->argument_list()->elements == 1)
  {                                                // Remove list
    item= ((Item_cond*) cond)->argument_list()->head();
    ((Item_cond*) cond)->argument_list()->empty();
    return item;
  }
  *cond_value= Item::COND_OK;
  return cond;
}


COND *
Item::remove_eq_conds(THD *thd, Item::cond_result *cond_value, bool top_level_arg)
{
  if (const_item() && !is_expensive())
  {
    *cond_value= eval_const_cond() ? Item::COND_TRUE : Item::COND_FALSE;
    return (COND*) 0;
  }
  *cond_value= Item::COND_OK;
  return this;                                        // Point at next and level
}


COND *
Item_bool_func2::remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                                 bool top_level_arg)
{
  if (const_item() && !is_expensive())
  {
    *cond_value= eval_const_cond() ? Item::COND_TRUE : Item::COND_FALSE;
    return (COND*) 0;
  }
  if ((*cond_value= eq_cmp_result()) != Item::COND_OK)
  {
    if (args[0]->eq(args[1], true))
    {
      if (!args[0]->maybe_null || functype() == Item_func::EQUAL_FUNC)
        return (COND*) 0;                       // Compare of identical items
    }
  }
  *cond_value= Item::COND_OK;
  return this;                                  // Point at next and level
}


/**
  Remove const and eq items. Return new item, or NULL if no condition
  cond_value is set to according:
  COND_OK    query is possible (field = constant)
  COND_TRUE  always true       ( 1 = 1 )
  COND_FALSE always false      ( 1 = 2 )

  SYNPOSIS
    remove_eq_conds()
    thd                         THD environment
    cond                        the condition to handle
    cond_value                  the resulting value of the condition

  NOTES
    calls the inner_remove_eq_conds to check all the tree reqursively

  RETURN
    *COND with the simplified condition
*/

COND *
Item_func_isnull::remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                                  bool top_level_arg)
{
  Item *real_item= args[0]->real_item();
  if (real_item->type() == Item::FIELD_ITEM)
  {
    Field *field= ((Item_field*) real_item)->field;

    if (((field->type() == MYSQL_TYPE_DATE) ||
         (field->type() == MYSQL_TYPE_DATETIME)) &&
         (field->flags & NOT_NULL_FLAG))
    {
      /* fix to replace 'NULL' dates with '0' (shreeve@uci.edu) */
      /*
        See BUG#12594011
        Documentation says that
        SELECT datetime_notnull d FROM t1 WHERE d IS NULL
        shall return rows where d=='0000-00-00'

        Thus, for DATE and DATETIME columns defined as NOT NULL,
        "date_notnull IS NULL" has to be modified to
        "date_notnull IS NULL OR date_notnull == 0" (if outer join)
        "date_notnull == 0"                         (otherwise)

      */

      Item *item0= new(thd->mem_root) Item_int(thd, (longlong) 0, 1);
      Item *eq_cond= new(thd->mem_root) Item_func_eq(thd, args[0], item0);
      if (!eq_cond)
        return this;

      COND *cond= this;
      if (field->table->pos_in_table_list->is_inner_table_of_outer_join())
      {
        // outer join: transform "col IS NULL" to "col IS NULL or col=0"
        Item *or_cond= new(thd->mem_root) Item_cond_or(thd, eq_cond, this);
        if (!or_cond)
          return this;
        cond= or_cond;
      }
      else
      {
        // not outer join: transform "col IS NULL" to "col=0"
        cond= eq_cond;
      }

      cond->fix_fields(thd, &cond);
      /*
        Note: although args[0] is a field, cond can still be a constant
        (in case field is a part of a dependent subquery).

        Note: we call cond->Item::remove_eq_conds() non-virtually (statically)
        for performance purpose.
        A non-qualified call, i.e. just cond->remove_eq_conds(),
        would call Item_bool_func2::remove_eq_conds() instead, which would
        try to do some extra job to detect if args[0] and args[1] are
        equivalent items. We know they are not (we have field=0 here).
      */
      return cond->Item::remove_eq_conds(thd, cond_value, false);
    }

    /*
      Handles this special case for some ODBC applications:
      The are requesting the row that was just updated with a auto_increment
      value with this construct:

      SELECT * from table_name where auto_increment_column IS NULL
      This will be changed to:
      SELECT * from table_name where auto_increment_column = LAST_INSERT_ID

      Note, this substitution is done if the NULL test is the only condition!
      If the NULL test is a part of a more complex condition, it is not
      substituted and is treated normally:
        WHERE auto_increment IS NULL AND something_else
    */

    if (top_level_arg) // "auto_increment_column IS NULL" is the only condition
    {
      if (field->flags & AUTO_INCREMENT_FLAG && !field->table->maybe_null &&
          (thd->variables.option_bits & OPTION_AUTO_IS_NULL) &&
          (thd->first_successful_insert_id_in_prev_stmt > 0 &&
           thd->substitute_null_with_insert_id))
      {
  #ifdef HAVE_QUERY_CACHE
        query_cache_abort(thd, &thd->query_cache_tls);
  #endif
        COND *new_cond, *cond= this;
        /* If this fails, we will catch it later before executing query */
        if ((new_cond= new (thd->mem_root) Item_func_eq(thd, args[0],
                                        new (thd->mem_root) Item_int(thd, "last_insert_id()",
                                                     thd->read_first_successful_insert_id_in_prev_stmt(),
                                                     MY_INT64_NUM_DECIMAL_DIGITS))))
        {
          cond= new_cond;
          /*
            Item_func_eq can't be fixed after creation so we do not check
            cond->fixed, also it do not need tables so we use 0 as second
            argument.
          */
          cond->fix_fields(thd, &cond);
        }
        /*
          IS NULL should be mapped to LAST_INSERT_ID only for first row, so
          clear for next row
        */
        thd->substitute_null_with_insert_id= FALSE;

        *cond_value= Item::COND_OK;
        return cond;
      }
    }
  }
  return Item::remove_eq_conds(thd, cond_value, top_level_arg);
}


/**
  Check if equality can be used in removing components of GROUP BY/DISTINCT
  
  @param    l          the left comparison argument (a field if any)
  @param    r          the right comparison argument (a const of any)
  
  @details
  Checks if an equality predicate can be used to take away 
  DISTINCT/GROUP BY because it is known to be true for exactly one 
  distinct value (e.g. <expr> == <const>).
  Arguments must be compared in the native type of the left argument
  and (for strings) in the native collation of the left argument.
  Otherwise, for example,
  <string_field> = <int_const> may match more than 1 distinct value or
  the <string_field>.

  @note We don't need to aggregate l and r collations here, because r -
  the constant item - has already been converted to a proper collation
  for comparison. We only need to compare this collation with field's collation.

  @retval true    can be used
  @retval false   cannot be used
*/

/*
  psergey-todo: this returns false for int_column='1234' (here '1234' is a
  constant. Need to discuss this with Bar).

  See also Field::test_if_equality_guaranees_uniqueness(const Item *item);
*/
static bool
test_if_equality_guarantees_uniqueness(Item *l, Item *r)
{
  return (r->const_item() || !(r->used_tables() & ~OUTER_REF_TABLE_BIT)) &&
    item_cmp_type(l, r) == l->cmp_type() &&
    (l->cmp_type() != STRING_RESULT ||
     l->collation.collation == r->collation.collation);
}


/*
  Return TRUE if i1 and i2 (if any) are equal items,
  or if i1 is a wrapper item around the f2 field.
*/

static bool equal(Item *i1, Item *i2, Field *f2)
{
  DBUG_ASSERT((i2 == NULL) ^ (f2 == NULL));

  if (i2 != NULL)
    return i1->eq(i2, 1);
  else if (i1->type() == Item::FIELD_ITEM)
    return f2->eq(((Item_field *) i1)->field);
  else
    return FALSE;
}


/**
  Test if a field or an item is equal to a constant value in WHERE

  @param        cond            WHERE clause expression
  @param        comp_item       Item to find in WHERE expression
                                (if comp_field != NULL)
  @param        comp_field      Field to find in WHERE expression
                                (if comp_item != NULL)
  @param[out]   const_item      intermediate arg, set to Item pointer to NULL 

  @return TRUE if the field is a constant value in WHERE

  @note
    comp_item and comp_field parameters are mutually exclusive.
*/
bool
const_expression_in_where(COND *cond, Item *comp_item, Field *comp_field,
                          Item **const_item)
{
  DBUG_ASSERT((comp_item == NULL) ^ (comp_field == NULL));

  Item *intermediate= NULL;
  if (const_item == NULL)
    const_item= &intermediate;

  if (cond->type() == Item::COND_ITEM)
  {
    bool and_level= (((Item_cond*) cond)->functype()
		     == Item_func::COND_AND_FUNC);
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      bool res=const_expression_in_where(item, comp_item, comp_field,
                                         const_item);
      if (res)					// Is a const value
      {
	if (and_level)
	  return 1;
      }
      else if (!and_level)
	return 0;
    }
    return and_level ? 0 : 1;
  }
  else if (cond->eq_cmp_result() != Item::COND_OK)
  {						// boolean compare function
    Item_func* func= (Item_func*) cond;
    if (func->functype() != Item_func::EQUAL_FUNC &&
	func->functype() != Item_func::EQ_FUNC)
      return 0;
    Item *left_item=	((Item_func*) cond)->arguments()[0];
    Item *right_item= ((Item_func*) cond)->arguments()[1];
    if (equal(left_item, comp_item, comp_field))
    {
      if (test_if_equality_guarantees_uniqueness (left_item, right_item))
      {
	if (*const_item)
	  return right_item->eq(*const_item, 1);
	*const_item=right_item;
	return 1;
      }
    }
    else if (equal(right_item, comp_item, comp_field))
    {
      if (test_if_equality_guarantees_uniqueness (right_item, left_item))
      {
	if (*const_item)
	  return left_item->eq(*const_item, 1);
	*const_item=left_item;
	return 1;
      }
    }
  }
  return 0;
}


/****************************************************************************
  Create internal temporary table
****************************************************************************/

/**
  Create field for temporary table from given field.

  @param thd	       Thread handler
  @param org_field    field from which new field will be created
  @param name         New field name
  @param table	       Temporary table
  @param item	       !=NULL if item->result_field should point to new field.
                      This is relevant for how fill_record() is going to work:
                      If item != NULL then fill_record() will update
                      the record in the original table.
                      If item == NULL then fill_record() will update
                      the temporary table

  @retval
    NULL		on error
  @retval
    new_created field
*/

Field *create_tmp_field_from_field(THD *thd, Field *org_field,
                                   LEX_CSTRING *name, TABLE *table,
                                   Item_field *item)
{
  Field *new_field;

  new_field= org_field->make_new_field(thd->mem_root, table,
                                       table == org_field->table);
  if (new_field)
  {
    new_field->init(table);
    new_field->orig_table= org_field->orig_table;
    if (item)
      item->result_field= new_field;
    else
      new_field->field_name= *name;
    new_field->flags|= org_field->flags & NO_DEFAULT_VALUE_FLAG;
    if (org_field->maybe_null() || (item && item->maybe_null))
      new_field->flags&= ~NOT_NULL_FLAG;	// Because of outer join
    if (org_field->type() == MYSQL_TYPE_VAR_STRING ||
        org_field->type() == MYSQL_TYPE_VARCHAR)
      table->s->db_create_options|= HA_OPTION_PACK_RECORD;
    else if (org_field->type() == FIELD_TYPE_DOUBLE)
      ((Field_double *) new_field)->not_fixed= TRUE;
    new_field->vcol_info= 0;
    new_field->cond_selectivity= 1.0;
    new_field->next_equal_field= NULL;
    new_field->option_list= NULL;
    new_field->option_struct= NULL;
  }
  return new_field;
}


Field *Item::create_tmp_field_int(TABLE *table, uint convert_int_length)
{
  const Type_handler *h= &type_handler_long;
  if (max_char_length() > convert_int_length)
    h= &type_handler_longlong;
  return h->make_and_init_table_field(&name, Record_addr(maybe_null),
                                      *this, table);
}


Field *Item_sum::create_tmp_field(bool group, TABLE *table)
{
  Field *UNINIT_VAR(new_field);
  MEM_ROOT *mem_root= table->in_use->mem_root;

  switch (cmp_type()) {
  case REAL_RESULT:
  {
    new_field= new (mem_root)
      Field_double(max_char_length(), maybe_null, &name, decimals, TRUE);
    break;
  }
  case INT_RESULT:
  case TIME_RESULT:
  case DECIMAL_RESULT:
  case STRING_RESULT:
    new_field= tmp_table_field_from_field_type(table);
    break;
  case ROW_RESULT:
    // This case should never be choosen
    DBUG_ASSERT(0);
    new_field= 0;
    break;
  }
  if (new_field)
    new_field->init(table);
  return new_field;
}


static void create_tmp_field_from_item_finalize(THD *thd,
                                                Field *new_field,
                                                Item *item,
                                                Item ***copy_func,
                                                bool modify_item)
{
  if (copy_func &&
      (item->is_result_field() ||
       (item->real_item()->is_result_field())))
    *((*copy_func)++) = item;			// Save for copy_funcs
  if (modify_item)
    item->set_result_field(new_field);
  if (item->type() == Item::NULL_ITEM)
    new_field->is_created_from_null_item= TRUE;
}


/**
  Create field for temporary table using type of given item.

  @param thd                   Thread handler
  @param item                  Item to create a field for
  @param table                 Temporary table
  @param copy_func             If set and item is a function, store copy of
                               item in this array
  @param modify_item           1 if item->result_field should point to new
                               item. This is relevent for how fill_record()
                               is going to work:
                               If modify_item is 1 then fill_record() will
                               update the record in the original table.
                               If modify_item is 0 then fill_record() will
                               update the temporary table

  @retval
    0  on error
  @retval
    new_created field
*/

static Field *create_tmp_field_from_item(THD *thd, Item *item, TABLE *table,
                                         Item ***copy_func, bool modify_item)
{
  DBUG_ASSERT(thd == table->in_use);
  Field* new_field= item->create_tmp_field(false, table);
  if (new_field)
    create_tmp_field_from_item_finalize(thd, new_field, item,
                                        copy_func, modify_item);
  return new_field;
}


/**
  Create field for information schema table.

  @param thd		Thread handler
  @param table		Temporary table
  @param item		Item to create a field for

  @retval
    0			on error
  @retval
    new_created field
*/

Field *Item::create_field_for_schema(THD *thd, TABLE *table)
{
  if (field_type() == MYSQL_TYPE_VARCHAR)
  {
    Field *field;
    if (max_length > MAX_FIELD_VARCHARLENGTH)
      field= new Field_blob(max_length, maybe_null, &name,
                            collation.collation);
    else
      field= new Field_varstring(max_length, maybe_null, &name,
                                 table->s, collation.collation);
    if (field)
      field->init(table);
    return field;
  }
  return tmp_table_field_from_field_type(table);
}


/**
  Create field for temporary table.

  @param thd		Thread handler
  @param table		Temporary table
  @param item		Item to create a field for
  @param type		Type of item (normally item->type)
  @param copy_func	If set and item is a function, store copy of item
                       in this array
  @param from_field    if field will be created using other field as example,
                       pointer example field will be written here
  @param default_field	If field has a default value field, store it here
  @param group		1 if we are going to do a relative group by on result
  @param modify_item	1 if item->result_field should point to new item.
                       This is relevent for how fill_record() is going to
                       work:
                       If modify_item is 1 then fill_record() will update
                       the record in the original table.
                       If modify_item is 0 then fill_record() will update
                       the temporary table
  @param table_cant_handle_bit_fields
                       Set to 1 if the temporary table cannot handle bit
                       fields. Only set for heap tables when the bit field
                       is part of an index.
  @param make_copy_field
                       Set when using with rollup when we want to have
                       an exact copy of the field.
  @retval
    0			on error
  @retval
    new_created field
*/

Field *create_tmp_field(THD *thd, TABLE *table,Item *item, Item::Type type,
                        Item ***copy_func, Field **from_field,
                        Field **default_field,
                        bool group, bool modify_item,
                        bool table_cant_handle_bit_fields,
                        bool make_copy_field)
{
  Field *result;
  Item::Type orig_type= type;
  Item *orig_item= 0;

  DBUG_ASSERT(thd == table->in_use);

  if (type != Item::FIELD_ITEM &&
      item->real_item()->type() == Item::FIELD_ITEM)
  {
    orig_item= item;
    item= item->real_item();
    type= Item::FIELD_ITEM;
  }

  switch (type) {
  case Item::TYPE_HOLDER:
  case Item::SUM_FUNC_ITEM:
  {
    result= item->create_tmp_field(group, table);
    if (!result)
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATALERROR));
    return result;
  }
  case Item::DEFAULT_VALUE_ITEM:
  {
    Field *field= ((Item_default_value*) item)->field;
    if (field->default_value && (field->flags & BLOB_FLAG))
    {
      /*
        We have to use a copy function when using a blob with default value
        as the we have to calcuate the default value before we can use it.
      */
      return create_tmp_field_from_item(thd, item, table,
                                        (make_copy_field ? 0 : copy_func),
                                        modify_item);
    }
  }
  /* Fall through */
  case Item::FIELD_ITEM:
  case Item::CONTEXTUALLY_TYPED_VALUE_ITEM:
  case Item::INSERT_VALUE_ITEM:
  case Item::TRIGGER_FIELD_ITEM:
  {
    Item_field *field= (Item_field*) item;
    bool orig_modify= modify_item;
    if (orig_type == Item::REF_ITEM)
      modify_item= 0;
    /*
      If item have to be able to store NULLs but underlaid field can't do it,
      create_tmp_field_from_field() can't be used for tmp field creation.
    */
    if (((field->maybe_null && field->in_rollup) ||      
	(thd->create_tmp_table_for_derived  &&    /* for mat. view/dt */
	 orig_item && orig_item->maybe_null)) &&         
        !field->field->maybe_null())
    {
      bool save_maybe_null= FALSE;
      /*
        The item the ref points to may have maybe_null flag set while
        the ref doesn't have it. This may happen for outer fields
        when the outer query decided at some point after name resolution phase
        that this field might be null. Take this into account here.
      */
      if (orig_item)
      {
        save_maybe_null= item->maybe_null;
        item->maybe_null= orig_item->maybe_null;
      }
      result= create_tmp_field_from_item(thd, item, table, NULL,
                                         modify_item);
      *from_field= field->field;
      if (result && modify_item)
        field->result_field= result;
      if (orig_item)
      {
        item->maybe_null= save_maybe_null;
        result->field_name= orig_item->name;
      }
    }
    else if (table_cant_handle_bit_fields && field->field->type() ==
             MYSQL_TYPE_BIT)
    {
      const Type_handler *handler= item->type_handler_long_or_longlong();
      *from_field= field->field;
      if ((result=
             handler->make_and_init_table_field(&item->name,
                                                Record_addr(item->maybe_null),
                                                *item, table)))
        create_tmp_field_from_item_finalize(thd, result, item,
                                            copy_func, modify_item);
      if (result && modify_item)
        field->result_field= result;
    }
    else
    {
      LEX_CSTRING *tmp= orig_item ? &orig_item->name : &item->name;
      result= create_tmp_field_from_field(thd, (*from_field= field->field),
                                          tmp, table,
                                          modify_item ? field :
                                          NULL);
    }

    if (orig_type == Item::REF_ITEM && orig_modify)
      ((Item_ref*)orig_item)->set_result_field(result);
    /*
      Fields that are used as arguments to the DEFAULT() function already have
      their data pointers set to the default value during name resolution. See
      Item_default_value::fix_fields.
    */
    if (orig_type != Item::DEFAULT_VALUE_ITEM && field->field->eq_def(result))
      *default_field= field->field;
    return result;
  }
  /* Fall through */
  case Item::FUNC_ITEM:
    if (((Item_func *) item)->functype() == Item_func::FUNC_SP)
    {
      Item_func_sp *item_func_sp= (Item_func_sp *) item;
      Field *sp_result_field= item_func_sp->get_sp_result_field();

      if (make_copy_field)
      {
        DBUG_ASSERT(item_func_sp->result_field);
        *from_field= item_func_sp->result_field;
      }
      else
      {
        *((*copy_func)++)= item;
      }
      Field *result_field=
        create_tmp_field_from_field(thd,
                                    sp_result_field,
                                    &item_func_sp->name,
                                    table,
                                    NULL);

      if (modify_item)
        item->set_result_field(result_field);

      return result_field;
    }

    /* Fall through */
  case Item::COND_ITEM:
  case Item::SUBSELECT_ITEM:
  case Item::REF_ITEM:
  case Item::EXPR_CACHE_ITEM:
    if (make_copy_field)
    {
      DBUG_ASSERT(((Item_result_field*)item)->result_field);
      *from_field= ((Item_result_field*)item)->result_field;
    }
    /* Fall through */
  case Item::FIELD_AVG_ITEM:
  case Item::FIELD_STD_ITEM:
  case Item::PROC_ITEM:
  case Item::INT_ITEM:
  case Item::REAL_ITEM:
  case Item::DECIMAL_ITEM:
  case Item::STRING_ITEM:
  case Item::DATE_ITEM:
  case Item::NULL_ITEM:
  case Item::VARBIN_ITEM:
  case Item::CACHE_ITEM:
  case Item::WINDOW_FUNC_ITEM: // psergey-winfunc:
  case Item::PARAM_ITEM:
    return create_tmp_field_from_item(thd, item, table,
                                      (make_copy_field ? 0 : copy_func),
                                       modify_item);
  default:					// Dosen't have to be stored
    return 0;
  }
}

/*
  Set up column usage bitmaps for a temporary table

  IMPLEMENTATION
    For temporary tables, we need one bitmap with all columns set and
    a tmp_set bitmap to be used by things like filesort.
*/

void
setup_tmp_table_column_bitmaps(TABLE *table, uchar *bitmaps, uint field_count)
{
  uint bitmap_size= bitmap_buffer_size(field_count);

  DBUG_ASSERT(table->s->virtual_fields == 0 && table->def_vcol_set == 0);

  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count,
              FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->tmp_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->eq_join_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->cond_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->has_value_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  /* write_set and all_set are copies of read_set */
  table->def_write_set= table->def_read_set;
  table->s->all_set= table->def_read_set;
  bitmap_set_all(&table->s->all_set);
  table->default_column_bitmaps();
}


void
setup_tmp_table_column_bitmaps(TABLE *table, uchar *bitmaps)
{
  setup_tmp_table_column_bitmaps(table, bitmaps, table->s->fields);
}


/**
  Create a temp table according to a field list.

  Given field pointers are changed to point at tmp_table for
  send_result_set_metadata. The table object is self contained: it's
  allocated in its own memory root, as well as Field objects
  created for table columns.
  This function will replace Item_sum items in 'fields' list with
  corresponding Item_field items, pointing at the fields in the
  temporary table, unless this was prohibited by TRUE
  value of argument save_sum_fields. The Item_field objects
  are created in THD memory root.

  @param thd                  thread handle
  @param param                a description used as input to create the table
  @param fields               list of items that will be used to define
                              column types of the table (also see NOTES)
  @param group                Create an unique key over all group by fields.
                              This is used to retrive the row during
                              end_write_group() and update them.
  @param distinct             should table rows be distinct
  @param save_sum_fields      see NOTES
  @param select_options       Optiions for how the select is run.
                              See sql_priv.h for a list of options.
  @param rows_limit           Maximum number of rows to insert into the
                              temporary table
  @param table_alias          possible name of the temporary table that can
                              be used for name resolving; can be "".
  @param do_not_open          only create the TABLE object, do not
                              open the table in the engine
  @param keep_row_order       rows need to be read in the order they were
                              inserted, the engine should preserve this order
*/

TABLE *
create_tmp_table(THD *thd, TMP_TABLE_PARAM *param, List<Item> &fields,
		 ORDER *group, bool distinct, bool save_sum_fields,
		 ulonglong select_options, ha_rows rows_limit,
                 const LEX_CSTRING *table_alias, bool do_not_open,
                 bool keep_row_order)
{
  MEM_ROOT *mem_root_save, own_root;
  TABLE *table;
  TABLE_SHARE *share;
  uint	i,field_count,null_count,null_pack_length;
  uint  copy_func_count= param->func_count;
  uint  hidden_null_count, hidden_null_pack_length, hidden_field_count;
  uint  blob_count,group_null_items, string_count;
  uint  temp_pool_slot=MY_BIT_NONE;
  uint fieldnr= 0;
  ulong reclength, string_total_length;
  bool  using_unique_constraint= false;
  bool  use_packed_rows= false;
  bool  not_all_columns= !(select_options & TMP_TABLE_ALL_COLUMNS);
  bool  save_abort_on_warning;
  char  *tmpname,path[FN_REFLEN];
  uchar	*pos, *group_buff, *bitmaps;
  uchar *null_flags;
  Field **reg_field, **from_field, **default_field;
  uint *blob_field;
  Copy_field *copy=0;
  KEY *keyinfo;
  KEY_PART_INFO *key_part_info;
  Item **copy_func;
  TMP_ENGINE_COLUMNDEF *recinfo;
  /*
    total_uneven_bit_length is uneven bit length for visible fields
    hidden_uneven_bit_length is uneven bit length for hidden fields
  */
  uint total_uneven_bit_length= 0, hidden_uneven_bit_length= 0;
  bool force_copy_fields= param->force_copy_fields;
  /* Treat sum functions as normal ones when loose index scan is used. */
  save_sum_fields|= param->precomputed_group_by;
  DBUG_ENTER("create_tmp_table");
  DBUG_PRINT("enter",
             ("table_alias: '%s'  distinct: %d  save_sum_fields: %d  "
              "rows_limit: %lu  group: %d", table_alias->str,
              (int) distinct, (int) save_sum_fields,
              (ulong) rows_limit, MY_TEST(group)));

  if (use_temp_pool && !(test_flags & TEST_KEEP_TMP_TABLES))
    temp_pool_slot = bitmap_lock_set_next(&temp_pool);

  if (temp_pool_slot != MY_BIT_NONE) // we got a slot
    sprintf(path, "%s_%lx_%i", tmp_file_prefix,
            current_pid, temp_pool_slot);
  else
  {
    /* if we run out of slots or we are not using tempool */
    sprintf(path, "%s%lx_%lx_%x", tmp_file_prefix,current_pid,
            (ulong) thd->thread_id, thd->tmp_table++);
  }

  /*
    No need to change table name to lower case as we are only creating
    MyISAM, Aria or HEAP tables here
  */
  fn_format(path, path, mysql_tmpdir, "", MY_REPLACE_EXT|MY_UNPACK_FILENAME);

  if (group)
  {
    ORDER **prev= &group;
    if (!param->quick_group)
      group=0;					// Can't use group key
    else for (ORDER *tmp=group ; tmp ; tmp=tmp->next)
    {
      /* Exclude found constant from the list */
      if ((*tmp->item)->const_item())
      {
        *prev= tmp->next;
        param->group_parts--;
        continue;
      }
      else
        prev= &(tmp->next);
      /*
        marker == 4 means two things:
        - store NULLs in the key, and
        - convert BIT fields to 64-bit long, needed because MEMORY tables
          can't index BIT fields.
      */
      (*tmp->item)->marker=4;			// Store null in key
      if ((*tmp->item)->too_big_for_varchar())
	using_unique_constraint= true;
    }
    if (param->group_length >= MAX_BLOB_WIDTH)
      using_unique_constraint= true;
    if (group)
      distinct=0;				// Can't use distinct
  }

  field_count=param->field_count+param->func_count+param->sum_func_count;
  hidden_field_count=param->hidden_field_count;

  /*
    When loose index scan is employed as access method, it already
    computes all groups and the result of all aggregate functions. We
    make space for the items of the aggregate function in the list of
    functions TMP_TABLE_PARAM::items_to_copy, so that the values of
    these items are stored in the temporary table.
  */
  if (param->precomputed_group_by)
    copy_func_count+= param->sum_func_count;
  
  init_sql_alloc(&own_root, "tmp_table", TABLE_ALLOC_BLOCK_SIZE, 0,
                 MYF(MY_THREAD_SPECIFIC));

  if (!multi_alloc_root(&own_root,
                        &table, sizeof(*table),
                        &share, sizeof(*share),
                        &reg_field, sizeof(Field*) * (field_count+1),
                        &default_field, sizeof(Field*) * (field_count),
                        &blob_field, sizeof(uint)*(field_count+1),
                        &from_field, sizeof(Field*)*field_count,
                        &copy_func, sizeof(*copy_func)*(copy_func_count+1),
                        &param->keyinfo, sizeof(*param->keyinfo),
                        &key_part_info,
                        sizeof(*key_part_info)*(param->group_parts+1),
                        &param->start_recinfo,
                        sizeof(*param->recinfo)*(field_count*2+4),
                        &tmpname, (uint) strlen(path)+1,
                        &group_buff, (group && ! using_unique_constraint ?
                                      param->group_length : 0),
                        &bitmaps, bitmap_buffer_size(field_count)*6,
                        NullS))
  {
    if (temp_pool_slot != MY_BIT_NONE)
      bitmap_lock_clear_bit(&temp_pool, temp_pool_slot);
    DBUG_RETURN(NULL);				/* purecov: inspected */
  }
  /* Copy_field belongs to TMP_TABLE_PARAM, allocate it in THD mem_root */
  if (!(param->copy_field= copy= new (thd->mem_root) Copy_field[field_count]))
  {
    if (temp_pool_slot != MY_BIT_NONE)
      bitmap_lock_clear_bit(&temp_pool, temp_pool_slot);
    free_root(&own_root, MYF(0));               /* purecov: inspected */
    DBUG_RETURN(NULL);				/* purecov: inspected */
  }
  param->items_to_copy= copy_func;
  strmov(tmpname, path);
  /* make table according to fields */

  bzero((char*) table,sizeof(*table));
  bzero((char*) reg_field,sizeof(Field*)*(field_count+1));
  bzero((char*) default_field, sizeof(Field*) * (field_count));
  bzero((char*) from_field,sizeof(Field*)*field_count);

  table->mem_root= own_root;
  mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  table->field=reg_field;
  table->alias.set(table_alias->str, table_alias->length, table_alias_charset);

  table->reginfo.lock_type=TL_WRITE;	/* Will be updated */
  table->map=1;
  table->temp_pool_slot = temp_pool_slot;
  table->copy_blobs= 1;
  table->in_use= thd;
  table->quick_keys.init();
  table->covering_keys.init();
  table->intersect_keys.init();
  table->keys_in_use_for_query.init();
  table->no_rows_with_nulls= param->force_not_null_cols;

  table->s= share;
  init_tmp_table_share(thd, share, "", 0, "(temporary)", tmpname);
  share->blob_field= blob_field;
  share->table_charset= param->table_charset;
  share->primary_key= MAX_KEY;               // Indicate no primary key
  share->keys_for_keyread.init();
  share->keys_in_use.init();
  if (param->schema_table)
    share->db= INFORMATION_SCHEMA_NAME;

  /* Calculate which type of fields we will store in the temporary table */

  reclength= string_total_length= 0;
  blob_count= string_count= null_count= hidden_null_count= group_null_items= 0;
  param->using_outer_summary_function= 0;

  List_iterator_fast<Item> li(fields);
  Item *item;
  Field **tmp_from_field=from_field;
  while ((item=li++))
  {
    Item::Type type= item->type();
    if (type == Item::COPY_STR_ITEM)
    {
      item= ((Item_copy *)item)->get_item();
      type= item->type();
    }
    if (not_all_columns)
    {
      if (item->with_sum_func && type != Item::SUM_FUNC_ITEM)
      {
        if (item->used_tables() & OUTER_REF_TABLE_BIT)
          item->update_used_tables();
        if ((item->real_type() == Item::SUBSELECT_ITEM) ||
            (item->used_tables() & ~OUTER_REF_TABLE_BIT))
        {
	  /*
	    Mark that the we have ignored an item that refers to a summary
	    function. We need to know this if someone is going to use
	    DISTINCT on the result.
	  */
	  param->using_outer_summary_function=1;
	  continue;
        }
      }
      if (item->const_item() && (int) hidden_field_count <= 0)
        continue; // We don't have to store this
    }
    if (type == Item::SUM_FUNC_ITEM && !group && !save_sum_fields)
    {						/* Can't calc group yet */
      Item_sum *sum_item= (Item_sum *) item;
      sum_item->result_field=0;
      for (i=0 ; i < sum_item->get_arg_count() ; i++)
      {
	Item *arg= sum_item->get_arg(i);
	if (!arg->const_item())
	{
          Item *tmp_item;
          Field *new_field=
            create_tmp_field(thd, table, arg, arg->type(), &copy_func,
                             tmp_from_field, &default_field[fieldnr],
                             group != 0,not_all_columns,
                             distinct, false);
	  if (!new_field)
	    goto err;					// Should be OOM
          DBUG_ASSERT(!new_field->field_name.str || strlen(new_field->field_name.str) == new_field->field_name.length);
	  tmp_from_field++;
	  reclength+=new_field->pack_length();
	  if (new_field->flags & BLOB_FLAG)
	  {
	    *blob_field++= fieldnr;
	    blob_count++;
	  }
          if (new_field->type() == MYSQL_TYPE_BIT)
            total_uneven_bit_length+= new_field->field_length & 7;
	  *(reg_field++)= new_field;
          if (new_field->real_type() == MYSQL_TYPE_STRING ||
              new_field->real_type() == MYSQL_TYPE_VARCHAR)
          {
            string_count++;
            string_total_length+= new_field->pack_length();
          }
          thd->mem_root= mem_root_save;
          if (!(tmp_item= new (thd->mem_root)
                Item_temptable_field(thd, new_field)))
            goto err;
          arg= sum_item->set_arg(i, thd, tmp_item);
          thd->mem_root= &table->mem_root;
          if (param->force_not_null_cols)
	  {
            new_field->flags|= NOT_NULL_FLAG;
            new_field->null_ptr= NULL;
          }
	  if (!(new_field->flags & NOT_NULL_FLAG))
          {
	    null_count++;
            /*
              new_field->maybe_null() is still false, it will be
              changed below. But we have to setup Item_field correctly
            */
            arg->maybe_null=1;
          }
          new_field->field_index= fieldnr++;
	}
      }
    }
    else
    {
      /*
	The last parameter to create_tmp_field() is a bit tricky:

	We need to set it to 0 in union, to get fill_record() to modify the
	temporary table.
	We need to set it to 1 on multi-table-update and in select to
	write rows to the temporary table.
	We here distinguish between UNION and multi-table-updates by the fact
	that in the later case group is set to the row pointer.

        The test for item->marker == 4 is ensure we don't create a group-by
        key over a bit field as heap tables can't handle that.
      */
      Field *new_field= (param->schema_table) ?
        item->create_field_for_schema(thd, table) :
        create_tmp_field(thd, table, item, type, &copy_func,
                         tmp_from_field, &default_field[fieldnr],
                         group != 0,
                         !force_copy_fields &&
                           (not_all_columns || group !=0),
                         /*
                           If item->marker == 4 then we force create_tmp_field
                           to create a 64-bit longs for BIT fields because HEAP
                           tables can't index BIT fields directly. We do the
                           same for distinct, as we want the distinct index
                           to be usable in this case too.
                         */
                         item->marker == 4  || param->bit_fields_as_long,
                         force_copy_fields);

      if (unlikely(!new_field))
      {
	if (unlikely(thd->is_fatal_error))
	  goto err;				// Got OOM
	continue;				// Some kind of const item
      }
      DBUG_ASSERT(!new_field->field_name.str || strlen(new_field->field_name.str) == new_field->field_name.length);
      if (type == Item::SUM_FUNC_ITEM)
      {
        Item_sum *agg_item= (Item_sum *) item;
        /*
          Update the result field only if it has never been set, or if the
          created temporary table is not to be used for subquery
          materialization.

          The reason is that for subqueries that require
          materialization as part of their plan, we create the
          'external' temporary table needed for IN execution, after
          the 'internal' temporary table needed for grouping.  Since
          both the external and the internal temporary tables are
          created for the same list of SELECT fields of the subquery,
          setting 'result_field' for each invocation of
          create_tmp_table overrides the previous value of
          'result_field'.

          The condition below prevents the creation of the external
          temp table to override the 'result_field' that was set for
          the internal temp table.
        */
        if (!agg_item->result_field || !param->materialized_subquery)
          agg_item->result_field= new_field;
      }
      tmp_from_field++;
      if (param->force_not_null_cols)
      {
        new_field->flags|= NOT_NULL_FLAG;
        new_field->null_ptr= NULL;
      }
      reclength+=new_field->pack_length();
      if (!(new_field->flags & NOT_NULL_FLAG))
	null_count++;
      if (new_field->type() == MYSQL_TYPE_BIT)
        total_uneven_bit_length+= new_field->field_length & 7;
      if (new_field->flags & BLOB_FLAG)
      {
        *blob_field++= fieldnr;
	blob_count++;
      }

      if (new_field->real_type() == MYSQL_TYPE_STRING ||
          new_field->real_type() == MYSQL_TYPE_VARCHAR)
      {
        string_count++;
        string_total_length+= new_field->pack_length();
      }

      if (item->marker == 4 && item->maybe_null)
      {
	group_null_items++;
	new_field->flags|= GROUP_FLAG;
      }
      new_field->field_index= fieldnr++;
      *(reg_field++)= new_field;
    }
    if (!--hidden_field_count)
    {
      /*
        This was the last hidden field; Remember how many hidden fields could
        have null
      */
      hidden_null_count=null_count;
      /*
	We need to update hidden_field_count as we may have stored group
	functions with constant arguments
      */
      param->hidden_field_count= fieldnr;
      null_count= 0;
      /*
        On last hidden field we store uneven bit length in
        hidden_uneven_bit_length and proceed calculation of
        uneven bits for visible fields into
        total_uneven_bit_length variable.
      */
      hidden_uneven_bit_length= total_uneven_bit_length;
      total_uneven_bit_length= 0;
    }
  }
  DBUG_ASSERT(fieldnr == (uint) (reg_field - table->field));
  DBUG_ASSERT(field_count >= (uint) (reg_field - table->field));
  field_count= fieldnr;
  *reg_field= 0;
  *blob_field= 0;				// End marker
  share->fields= field_count;
  share->column_bitmap_size= bitmap_buffer_size(share->fields);

  /* If result table is small; use a heap */
  /* future: storage engine selection can be made dynamic? */
  if (blob_count || using_unique_constraint
      || (thd->variables.big_tables && !(select_options & SELECT_SMALL_RESULT))
      || (select_options & TMP_TABLE_FORCE_MYISAM)
      || thd->variables.tmp_memory_table_size == 0)
  {
    share->db_plugin= ha_lock_engine(0, TMP_ENGINE_HTON);
    table->file= get_new_handler(share, &table->mem_root,
                                 share->db_type());
    if (group &&
	(param->group_parts > table->file->max_key_parts() ||
	 param->group_length > table->file->max_key_length()))
      using_unique_constraint= true;
  }
  else
  {
    share->db_plugin= ha_lock_engine(0, heap_hton);
    table->file= get_new_handler(share, &table->mem_root,
                                 share->db_type());
  }
  if (!table->file)
    goto err;

  if (table->file->set_ha_share_ref(&share->ha_share))
  {
    delete table->file;
    goto err;
  }

  if (!using_unique_constraint)
    reclength+= group_null_items;	// null flag is stored separately

  share->blob_fields= blob_count;
  if (blob_count == 0)
  {
    /* We need to ensure that first byte is not 0 for the delete link */
    if (param->hidden_field_count)
      hidden_null_count++;
    else
      null_count++;
  }
  hidden_null_pack_length= (hidden_null_count + 7 +
                            hidden_uneven_bit_length) / 8;
  null_pack_length= (hidden_null_pack_length +
                     (null_count + total_uneven_bit_length + 7) / 8);
  reclength+=null_pack_length;
  if (!reclength)
    reclength=1;				// Dummy select
  /* Use packed rows if there is blobs or a lot of space to gain */
  if (blob_count ||
      (string_total_length >= STRING_TOTAL_LENGTH_TO_PACK_ROWS &&
       (reclength / string_total_length <= RATIO_TO_PACK_ROWS ||
        string_total_length / string_count >= AVG_STRING_LENGTH_TO_PACK_ROWS)))
    use_packed_rows= 1;

  share->reclength= reclength;
  {
    uint alloc_length=ALIGN_SIZE(reclength+MI_UNIQUE_HASH_LENGTH+1);
    share->rec_buff_length= alloc_length;
    if (!(table->record[0]= (uchar*)
                            alloc_root(&table->mem_root, alloc_length*3)))
      goto err;
    table->record[1]= table->record[0]+alloc_length;
    share->default_values= table->record[1]+alloc_length;
  }
  copy_func[0]=0;				// End marker
  param->func_count= (uint)(copy_func - param->items_to_copy); 

  setup_tmp_table_column_bitmaps(table, bitmaps);

  recinfo=param->start_recinfo;
  null_flags=(uchar*) table->record[0];
  pos=table->record[0]+ null_pack_length;
  if (null_pack_length)
  {
    bzero((uchar*) recinfo,sizeof(*recinfo));
    recinfo->type=FIELD_NORMAL;
    recinfo->length=null_pack_length;
    recinfo++;
    bfill(null_flags,null_pack_length,255);	// Set null fields

    table->null_flags= (uchar*) table->record[0];
    share->null_fields= null_count+ hidden_null_count;
    share->null_bytes= share->null_bytes_for_compare= null_pack_length;
  }
  null_count= (blob_count == 0) ? 1 : 0;
  hidden_field_count=param->hidden_field_count;

  /* Protect against warnings in field_conv() in the next loop*/
  save_abort_on_warning= thd->abort_on_warning;
  thd->abort_on_warning= 0;

  for (i=0,reg_field=table->field; i < field_count; i++,reg_field++,recinfo++)
  {
    Field *field= *reg_field;
    uint length;
    bzero((uchar*) recinfo,sizeof(*recinfo));

    if (!(field->flags & NOT_NULL_FLAG))
    {
      recinfo->null_bit= (uint8)1 << (null_count & 7);
      recinfo->null_pos= null_count/8;
      field->move_field(pos,null_flags+null_count/8,
			(uint8)1 << (null_count & 7));
      null_count++;
    }
    else
      field->move_field(pos,(uchar*) 0,0);
    if (field->type() == MYSQL_TYPE_BIT)
    {
      /* We have to reserve place for extra bits among null bits */
      ((Field_bit*) field)->set_bit_ptr(null_flags + null_count / 8,
                                        null_count & 7);
      null_count+= (field->field_length & 7);
    }
    field->reset();

    /*
      Test if there is a default field value. The test for ->ptr is to skip
      'offset' fields generated by initialize_tables
    */
    if (default_field[i] && default_field[i]->ptr)
    {
      /* 
         default_field[i] is set only in the cases  when 'field' can
         inherit the default value that is defined for the field referred
         by the Item_field object from which 'field' has been created.
      */
      Field *orig_field= default_field[i];
      /* Get the value from default_values */
      if (orig_field->is_null_in_record(orig_field->table->s->default_values))
        field->set_null();
      else
      {
        /*
          Copy default value. We have to use field_conv() for copy, instead of
          memcpy(), because bit_fields may be stored differently
        */
        my_ptrdiff_t ptr_diff= (orig_field->table->s->default_values -
                                orig_field->table->record[0]);
        field->set_notnull();
        orig_field->move_field_offset(ptr_diff);
        field_conv(field, orig_field);
        orig_field->move_field_offset(-ptr_diff);
      }
    }

    if (from_field[i])
    {						/* Not a table Item */
      copy->set(field,from_field[i],save_sum_fields);
      copy++;
    }
    length=field->pack_length_in_rec();
    pos+= length;

    /* Make entry for create table */
    recinfo->length=length;
    if (field->flags & BLOB_FLAG)
      recinfo->type= FIELD_BLOB;
    else if (use_packed_rows &&
             field->real_type() == MYSQL_TYPE_STRING &&
	     length >= MIN_STRING_LENGTH_TO_PACK_ROWS)
      recinfo->type= FIELD_SKIP_ENDSPACE;
    else if (field->real_type() == MYSQL_TYPE_VARCHAR)
      recinfo->type= FIELD_VARCHAR;
    else
      recinfo->type= FIELD_NORMAL;

    if (!--hidden_field_count)
      null_count=(null_count+7) & ~7;		// move to next byte

    // fix table name in field entry
    field->set_table_name(&table->alias);
  }
  /* Handle group_null_items */
  bzero(pos, table->s->reclength - (pos - table->record[0]));
  MEM_CHECK_DEFINED(table->record[0], table->s->reclength);

  thd->abort_on_warning= save_abort_on_warning;
  param->copy_field_end=copy;
  param->recinfo= recinfo;              	// Pointer to after last field
  store_record(table,s->default_values);        // Make empty default record

  if (thd->variables.tmp_memory_table_size == ~ (ulonglong) 0)	// No limit
    share->max_rows= ~(ha_rows) 0;
  else
    share->max_rows= (ha_rows) (((share->db_type() == heap_hton) ?
                                 MY_MIN(thd->variables.tmp_memory_table_size,
                                     thd->variables.max_heap_table_size) :
                                 thd->variables.tmp_memory_table_size) /
                                share->reclength);
  set_if_bigger(share->max_rows,1);		// For dummy start options
  /*
    Push the LIMIT clause to the temporary table creation, so that we
    materialize only up to 'rows_limit' records instead of all result records.
  */
  set_if_smaller(share->max_rows, rows_limit);
  param->end_write_records= rows_limit;

  keyinfo= param->keyinfo;

  if (group)
  {
    DBUG_PRINT("info",("Creating group key in temporary table"));
    table->group=group;				/* Table is grouped by key */
    param->group_buff=group_buff;
    share->keys=1;
    share->uniques= MY_TEST(using_unique_constraint);
    table->key_info= table->s->key_info= keyinfo;
    table->keys_in_use_for_query.set_bit(0);
    share->keys_in_use.set_bit(0);
    keyinfo->key_part=key_part_info;
    keyinfo->flags=HA_NOSAME | HA_BINARY_PACK_KEY | HA_PACK_KEY;
    keyinfo->ext_key_flags= keyinfo->flags;
    keyinfo->usable_key_parts=keyinfo->user_defined_key_parts= param->group_parts;
    keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
    keyinfo->key_length=0;
    keyinfo->rec_per_key=NULL;
    keyinfo->read_stats= NULL;
    keyinfo->collected_stats= NULL;
    keyinfo->algorithm= HA_KEY_ALG_UNDEF;
    keyinfo->is_statistics_from_stat_tables= FALSE;
    keyinfo->name= group_key;
    ORDER *cur_group= group;
    for (; cur_group ; cur_group= cur_group->next, key_part_info++)
    {
      Field *field=(*cur_group->item)->get_tmp_table_field();
      DBUG_ASSERT(field->table == table);
      bool maybe_null=(*cur_group->item)->maybe_null;
      key_part_info->null_bit=0;
      key_part_info->field=  field;
      key_part_info->fieldnr= field->field_index + 1;
      if (cur_group == group)
        field->key_start.set_bit(0);
      key_part_info->offset= field->offset(table->record[0]);
      key_part_info->length= (uint16) field->key_length();
      key_part_info->type=   (uint8) field->key_type();
      key_part_info->key_type =
	((ha_base_keytype) key_part_info->type == HA_KEYTYPE_TEXT ||
	 (ha_base_keytype) key_part_info->type == HA_KEYTYPE_VARTEXT1 ||
	 (ha_base_keytype) key_part_info->type == HA_KEYTYPE_VARTEXT2) ?
	0 : FIELDFLAG_BINARY;
      key_part_info->key_part_flag= 0;
      if (!using_unique_constraint)
      {
	cur_group->buff=(char*) group_buff;

        if (maybe_null && !field->null_bit)
        {
          /*
            This can only happen in the unusual case where an outer join
            table was found to be not-nullable by the optimizer and we
            the item can't really be null.
            We solve this by marking the item as !maybe_null to ensure
            that the key,field and item definition match.
          */
          (*cur_group->item)->maybe_null= maybe_null= 0;
        }

	if (!(cur_group->field= field->new_key_field(thd->mem_root,table,
                                                     group_buff +
                                                     MY_TEST(maybe_null),
                                                     key_part_info->length,
                                                     field->null_ptr,
                                                     field->null_bit)))
	  goto err; /* purecov: inspected */

	if (maybe_null)
	{
	  /*
	    To be able to group on NULL, we reserved place in group_buff
	    for the NULL flag just before the column. (see above).
	    The field data is after this flag.
	    The NULL flag is updated in 'end_update()' and 'end_write()'
	  */
	  keyinfo->flags|= HA_NULL_ARE_EQUAL;	// def. that NULL == NULL
	  key_part_info->null_bit=field->null_bit;
	  key_part_info->null_offset= (uint) (field->null_ptr -
					      (uchar*) table->record[0]);
          cur_group->buff++;                        // Pointer to field data
	  group_buff++;                         // Skipp null flag
	}
	group_buff+= cur_group->field->pack_length();
      }
      keyinfo->key_length+=  key_part_info->length;
    }
    /*
      Ensure we didn't overrun the group buffer. The < is only true when
      some maybe_null fields was changed to be not null fields.
    */
    DBUG_ASSERT(using_unique_constraint ||
                group_buff <= param->group_buff + param->group_length);
  }

  if (distinct && field_count != param->hidden_field_count)
  {
    /*
      Create an unique key or an unique constraint over all columns
      that should be in the result.  In the temporary table, there are
      'param->hidden_field_count' extra columns, whose null bits are stored
      in the first 'hidden_null_pack_length' bytes of the row.
    */
    DBUG_PRINT("info",("hidden_field_count: %d", param->hidden_field_count));

    if (blob_count)
    {
      /*
        Special mode for index creation in MyISAM used to support unique
        indexes on blobs with arbitrary length. Such indexes cannot be
        used for lookups.
      */
      share->uniques= 1;
    }
    null_pack_length-=hidden_null_pack_length;
    keyinfo->user_defined_key_parts=
      ((field_count-param->hidden_field_count)+
       (share->uniques ? MY_TEST(null_pack_length) : 0));
    keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
    keyinfo->usable_key_parts= keyinfo->user_defined_key_parts;
    table->distinct= 1;
    share->keys= 1;
    if (!(key_part_info= (KEY_PART_INFO*)
          alloc_root(&table->mem_root,
                     keyinfo->user_defined_key_parts * sizeof(KEY_PART_INFO))))
      goto err;
    bzero((void*) key_part_info, keyinfo->user_defined_key_parts * sizeof(KEY_PART_INFO));
    table->keys_in_use_for_query.set_bit(0);
    share->keys_in_use.set_bit(0);
    table->key_info= table->s->key_info= keyinfo;
    keyinfo->key_part=key_part_info;
    keyinfo->flags=HA_NOSAME | HA_NULL_ARE_EQUAL | HA_BINARY_PACK_KEY | HA_PACK_KEY;
    keyinfo->ext_key_flags= keyinfo->flags;
    keyinfo->key_length= 0;  // Will compute the sum of the parts below.
    keyinfo->name= distinct_key;
    keyinfo->algorithm= HA_KEY_ALG_UNDEF;
    keyinfo->is_statistics_from_stat_tables= FALSE;
    keyinfo->read_stats= NULL;
    keyinfo->collected_stats= NULL;

    /*
      Needed by non-merged semi-joins: SJ-Materialized table must have a valid 
      rec_per_key array, because it participates in join optimization. Since
      the table has no data, the only statistics we can provide is "unknown",
      i.e. zero values.

      (For table record count, we calculate and set JOIN_TAB::found_records,
       see get_delayed_table_estimates()).
    */
    size_t rpk_size= keyinfo->user_defined_key_parts * sizeof(keyinfo->rec_per_key[0]);
    if (!(keyinfo->rec_per_key= (ulong*) alloc_root(&table->mem_root, 
                                                    rpk_size)))
      goto err;
    bzero(keyinfo->rec_per_key, rpk_size);

    /*
      Create an extra field to hold NULL bits so that unique indexes on
      blobs can distinguish NULL from 0. This extra field is not needed
      when we do not use UNIQUE indexes for blobs.
    */
    if (null_pack_length && share->uniques)
    {
      key_part_info->null_bit=0;
      key_part_info->offset=hidden_null_pack_length;
      key_part_info->length=null_pack_length;
      key_part_info->field= new Field_string(table->record[0],
                                             (uint32) key_part_info->length,
                                             (uchar*) 0,
                                             (uint) 0,
                                             Field::NONE,
                                             &null_clex_str, &my_charset_bin);
      if (!key_part_info->field)
        goto err;
      key_part_info->field->init(table);
      key_part_info->key_type=FIELDFLAG_BINARY;
      key_part_info->type=    HA_KEYTYPE_BINARY;
      key_part_info->fieldnr= key_part_info->field->field_index + 1;
      key_part_info++;
    }
    /* Create a distinct key over the columns we are going to return */
    for (i=param->hidden_field_count, reg_field=table->field + i ;
	 i < field_count;
	 i++, reg_field++, key_part_info++)
    {
      key_part_info->field=    *reg_field;
      (*reg_field)->flags |= PART_KEY_FLAG;
      if (key_part_info == keyinfo->key_part)
        (*reg_field)->key_start.set_bit(0);
      key_part_info->null_bit= (*reg_field)->null_bit;
      key_part_info->null_offset= (uint) ((*reg_field)->null_ptr -
                                          (uchar*) table->record[0]);

      key_part_info->offset=   (*reg_field)->offset(table->record[0]);
      key_part_info->length=   (uint16) (*reg_field)->pack_length();
      key_part_info->fieldnr= (*reg_field)->field_index + 1;
      /* TODO:
        The below method of computing the key format length of the
        key part is a copy/paste from opt_range.cc, and table.cc.
        This should be factored out, e.g. as a method of Field.
        In addition it is not clear if any of the Field::*_length
        methods is supposed to compute the same length. If so, it
        might be reused.
      */
      key_part_info->store_length= key_part_info->length;

      if ((*reg_field)->real_maybe_null())
      {
        key_part_info->store_length+= HA_KEY_NULL_LENGTH;
        key_part_info->key_part_flag |= HA_NULL_PART;
      }
      if ((*reg_field)->type() == MYSQL_TYPE_BLOB ||
          (*reg_field)->real_type() == MYSQL_TYPE_VARCHAR ||
          (*reg_field)->type() == MYSQL_TYPE_GEOMETRY)
      {
        if ((*reg_field)->type() == MYSQL_TYPE_BLOB ||
            (*reg_field)->type() == MYSQL_TYPE_GEOMETRY)
          key_part_info->key_part_flag|= HA_BLOB_PART;
        else
          key_part_info->key_part_flag|= HA_VAR_LENGTH_PART;

        key_part_info->store_length+=HA_KEY_BLOB_LENGTH;
      }

      keyinfo->key_length+= key_part_info->store_length;

      key_part_info->type=     (uint8) (*reg_field)->key_type();
      key_part_info->key_type =
	((ha_base_keytype) key_part_info->type == HA_KEYTYPE_TEXT ||
	 (ha_base_keytype) key_part_info->type == HA_KEYTYPE_VARTEXT1 ||
	 (ha_base_keytype) key_part_info->type == HA_KEYTYPE_VARTEXT2) ?
	0 : FIELDFLAG_BINARY;
    }
  }

  if (unlikely(thd->is_fatal_error))             // If end of memory
    goto err;					 /* purecov: inspected */
  share->db_record_offset= 1;
  table->used_for_duplicate_elimination= (param->sum_func_count == 0 &&
                                          (table->group || table->distinct));
  table->keep_row_order= keep_row_order;

  if (!do_not_open)
  {
    if (instantiate_tmp_table(table, param->keyinfo, param->start_recinfo,
                              &param->recinfo, select_options))
      goto err;
  }

  /* record[0] and share->default_values should now have been set up */
  MEM_CHECK_DEFINED(table->record[0], table->s->reclength);
  MEM_CHECK_DEFINED(share->default_values, table->s->reclength);

  thd->mem_root= mem_root_save;

  DBUG_RETURN(table);

err:
  thd->mem_root= mem_root_save;
  free_tmp_table(thd,table);                    /* purecov: inspected */
  if (temp_pool_slot != MY_BIT_NONE)
    bitmap_lock_clear_bit(&temp_pool, temp_pool_slot);
  DBUG_RETURN(NULL);				/* purecov: inspected */
}


/****************************************************************************/

void *Virtual_tmp_table::operator new(size_t size, THD *thd) throw()
{
  return (Virtual_tmp_table *) alloc_root(thd->mem_root, size);
}


bool Virtual_tmp_table::init(uint field_count)
{
  uint *blob_field;
  uchar *bitmaps;
  DBUG_ENTER("Virtual_tmp_table::init");
  if (!multi_alloc_root(in_use->mem_root,
                        &s, sizeof(*s),
                        &field, (field_count + 1) * sizeof(Field*),
                        &blob_field, (field_count + 1) * sizeof(uint),
                        &bitmaps, bitmap_buffer_size(field_count) * 6,
                        NullS))
    DBUG_RETURN(true);
  s->reset();
  s->blob_field= blob_field;
  setup_tmp_table_column_bitmaps(this, bitmaps, field_count);
  m_alloced_field_count= field_count;
  DBUG_RETURN(false);
};


bool Virtual_tmp_table::add(List<Spvar_definition> &field_list)
{
  /* Create all fields and calculate the total length of record */
  Spvar_definition *cdef;            /* column definition */
  List_iterator_fast<Spvar_definition> it(field_list);
  DBUG_ENTER("Virtual_tmp_table::add");
  while ((cdef= it++))
  {
    Field *tmp;
    if (!(tmp= cdef->make_field(s, in_use->mem_root, 0,
                             (uchar*) (f_maybe_null(cdef->pack_flag) ? "" : 0),
                             f_maybe_null(cdef->pack_flag) ? 1 : 0,
                             &cdef->field_name)))
      DBUG_RETURN(true);
     add(tmp);
  }
  DBUG_RETURN(false);
}


void Virtual_tmp_table::setup_field_pointers()
{
  uchar *null_pos= record[0];
  uchar *field_pos= null_pos + s->null_bytes;
  uint null_bit= 1;

  for (Field **cur_ptr= field; *cur_ptr; ++cur_ptr)
  {
    Field *cur_field= *cur_ptr;
    if ((cur_field->flags & NOT_NULL_FLAG))
      cur_field->move_field(field_pos);
    else
    {
      cur_field->move_field(field_pos, (uchar*) null_pos, null_bit);
      null_bit<<= 1;
      if (null_bit == (uint)1 << 8)
      {
        ++null_pos;
        null_bit= 1;
      }
    }
    if (cur_field->type() == MYSQL_TYPE_BIT &&
        cur_field->key_type() == HA_KEYTYPE_BIT)
    {
      /* This is a Field_bit since key_type is HA_KEYTYPE_BIT */
      static_cast<Field_bit*>(cur_field)->set_bit_ptr(null_pos, null_bit);
      null_bit+= cur_field->field_length & 7;
      if (null_bit > 7)
      {
        null_pos++;
        null_bit-= 8;
      }
    }
    cur_field->reset();
    field_pos+= cur_field->pack_length();
  }
}


bool Virtual_tmp_table::open()
{
  // Make sure that we added all the fields we planned to:
  DBUG_ASSERT(s->fields == m_alloced_field_count);
  field[s->fields]= NULL;            // mark the end of the list
  s->blob_field[s->blob_fields]= 0;  // mark the end of the list

  uint null_pack_length= (s->null_fields + 7) / 8; // NULL-bit array length
  s->reclength+= null_pack_length;
  s->rec_buff_length= ALIGN_SIZE(s->reclength + 1);
  if (!(record[0]= (uchar*) in_use->alloc(s->rec_buff_length)))
    return true;
  if (null_pack_length)
  {
    null_flags= (uchar*) record[0];
    s->null_bytes= s->null_bytes_for_compare= null_pack_length;
  }
  setup_field_pointers();
  return false;
}


bool Virtual_tmp_table::sp_find_field_by_name(uint *idx,
                                              const LEX_CSTRING &name) const
{
  Field *f;
  for (uint i= 0; (f= field[i]); i++)
  {
    // Use the same comparison style with sp_context::find_variable()
    if (!my_strnncoll(system_charset_info,
                      (const uchar *) f->field_name.str,
                      f->field_name.length,
                      (const uchar *) name.str, name.length))
    {
      *idx= i;
      return false;
    }
  }
  return true;
}


bool
Virtual_tmp_table::sp_find_field_by_name_or_error(uint *idx,
                                                  const LEX_CSTRING &var_name,
                                                  const LEX_CSTRING &field_name)
                                                  const
{
  if (sp_find_field_by_name(idx, field_name))
  {
    my_error(ER_ROW_VARIABLE_DOES_NOT_HAVE_FIELD, MYF(0),
             var_name.str, field_name.str);
    return true;
  }
  return false;
}


bool Virtual_tmp_table::sp_set_all_fields_from_item_list(THD *thd,
                                                         List<Item> &items)
{
  DBUG_ASSERT(s->fields == items.elements);
  List_iterator<Item> it(items);
  Item *item;
  for (uint i= 0 ; (item= it++) ; i++)
  {
    if (field[i]->sp_prepare_and_store_item(thd, &item))
      return true;
  }
  return false;
}


bool Virtual_tmp_table::sp_set_all_fields_from_item(THD *thd, Item *value)
{
  DBUG_ASSERT(value->fixed);
  DBUG_ASSERT(value->cols() == s->fields);
  for (uint i= 0; i < value->cols(); i++)
  {
    if (field[i]->sp_prepare_and_store_item(thd, value->addr(i)))
      return true;
  }
  return false;
}


bool open_tmp_table(TABLE *table)
{
  int error;
  if (unlikely((error= table->file->ha_open(table, table->s->path.str, O_RDWR,
                                            HA_OPEN_TMP_TABLE |
                                            HA_OPEN_INTERNAL_TABLE))))
  {
    table->file->print_error(error, MYF(0)); /* purecov: inspected */
    table->db_stat= 0;
    return 1;
  }
  table->db_stat= HA_OPEN_KEYFILE;
  (void) table->file->extra(HA_EXTRA_QUICK); /* Faster */
  if (!table->is_created())
  {
    table->set_created();
    table->in_use->inc_status_created_tmp_tables();
  }

  return 0;
}


#ifdef USE_ARIA_FOR_TMP_TABLES
/*
  Create internal (MyISAM or Maria) temporary table

  SYNOPSIS
    create_internal_tmp_table()
      table           Table object that descrimes the table to be created
      keyinfo         Description of the index (there is always one index)
      start_recinfo   engine's column descriptions
      recinfo INOUT   End of engine's column descriptions
      options         Option bits
   
  DESCRIPTION
    Create an internal emporary table according to passed description. The is
    assumed to have one unique index or constraint.

    The passed array or TMP_ENGINE_COLUMNDEF structures must have this form:

      1. 1-byte column (afaiu for 'deleted' flag) (note maybe not 1-byte
         when there are many nullable columns)
      2. Table columns
      3. One free TMP_ENGINE_COLUMNDEF element (*recinfo points here)
   
    This function may use the free element to create hash column for unique
    constraint.

   RETURN
     FALSE - OK
     TRUE  - Error
*/


bool create_internal_tmp_table(TABLE *table, KEY *keyinfo, 
                               TMP_ENGINE_COLUMNDEF *start_recinfo,
                               TMP_ENGINE_COLUMNDEF **recinfo, 
                               ulonglong options)
{
  int error;
  MARIA_KEYDEF keydef;
  MARIA_UNIQUEDEF uniquedef;
  TABLE_SHARE *share= table->s;
  MARIA_CREATE_INFO create_info;
  DBUG_ENTER("create_internal_tmp_table");

  if (share->keys)
  {						// Get keys for ni_create
    bool using_unique_constraint=0;
    HA_KEYSEG *seg= (HA_KEYSEG*) alloc_root(&table->mem_root,
                                            sizeof(*seg) * keyinfo->user_defined_key_parts);
    if (!seg)
      goto err;

    bzero(seg, sizeof(*seg) * keyinfo->user_defined_key_parts);
    /*
       Note that a similar check is performed during
       subquery_types_allow_materialization. See MDEV-7122 for more details as
       to why. Whenever this changes, it must be updated there as well, for
       all tmp_table engines.
    */
    if (keyinfo->key_length > table->file->max_key_length() ||
	keyinfo->user_defined_key_parts > table->file->max_key_parts() ||
	share->uniques)
    {
      if (!share->uniques && !(keyinfo->flags & HA_NOSAME))
      {
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "Using too big key for internal temp tables");
        DBUG_RETURN(1);
      }

      /* Can't create a key; Make a unique constraint instead of a key */
      share->keys=    0;
      share->uniques= 1;
      using_unique_constraint=1;
      bzero((char*) &uniquedef,sizeof(uniquedef));
      uniquedef.keysegs=keyinfo->user_defined_key_parts;
      uniquedef.seg=seg;
      uniquedef.null_are_equal=1;

      /* Create extra column for hash value */
      bzero((uchar*) *recinfo,sizeof(**recinfo));
      (*recinfo)->type=   FIELD_CHECK;
      (*recinfo)->length= MARIA_UNIQUE_HASH_LENGTH;
      (*recinfo)++;

      /* Avoid warnings from valgrind */
      bzero(table->record[0]+ share->reclength, MARIA_UNIQUE_HASH_LENGTH);
      bzero(share->default_values+ share->reclength, MARIA_UNIQUE_HASH_LENGTH);
      share->reclength+= MARIA_UNIQUE_HASH_LENGTH;
    }
    else
    {
      /* Create a key */
      bzero((char*) &keydef,sizeof(keydef));
      keydef.flag= keyinfo->flags & HA_NOSAME;
      keydef.keysegs=  keyinfo->user_defined_key_parts;
      keydef.seg= seg;
    }
    for (uint i=0; i < keyinfo->user_defined_key_parts ; i++,seg++)
    {
      Field *field=keyinfo->key_part[i].field;
      seg->flag=     0;
      seg->language= field->charset()->number;
      seg->length=   keyinfo->key_part[i].length;
      seg->start=    keyinfo->key_part[i].offset;
      if (field->flags & BLOB_FLAG)
      {
	seg->type=
	((keyinfo->key_part[i].key_type & FIELDFLAG_BINARY) ?
	 HA_KEYTYPE_VARBINARY2 : HA_KEYTYPE_VARTEXT2);
	seg->bit_start= (uint8)(field->pack_length() -
                                portable_sizeof_char_ptr);
	seg->flag= HA_BLOB_PART;
	seg->length=0;			// Whole blob in unique constraint
      }
      else
      {
	seg->type= keyinfo->key_part[i].type;
        /* Tell handler if it can do suffic space compression */
	if (field->real_type() == MYSQL_TYPE_STRING &&
	    keyinfo->key_part[i].length > 32)
	  seg->flag|= HA_SPACE_PACK;
      }
      if (!(field->flags & NOT_NULL_FLAG))
      {
	seg->null_bit= field->null_bit;
	seg->null_pos= (uint) (field->null_ptr - (uchar*) table->record[0]);
	/*
	  We are using a GROUP BY on something that contains NULL
	  In this case we have to tell Aria that two NULL should
	  on INSERT be regarded at the same value
	*/
	if (!using_unique_constraint)
	  keydef.flag|= HA_NULL_ARE_EQUAL;
      }
    }
  }
  bzero((char*) &create_info,sizeof(create_info));
  create_info.data_file_length= table->in_use->variables.tmp_disk_table_size;

  /*
    The logic for choosing the record format:
    The STATIC_RECORD format is the fastest one, because it's so simple,
    so we use this by default for short rows.
    BLOCK_RECORD caches both row and data, so this is generally faster than
    DYNAMIC_RECORD. The one exception is when we write to tmp table and
    want to use keys for duplicate elimination as with BLOCK RECORD
    we first write the row, then check for key conflicts and then we have to
    delete the row.  The cases when this can happen is when there is
    a group by and no sum functions or if distinct is used.
  */
  {
    enum data_file_type file_type= table->no_rows ? NO_RECORD :
        (share->reclength < 64 && !share->blob_fields ? STATIC_RECORD :
         table->used_for_duplicate_elimination ? DYNAMIC_RECORD : BLOCK_RECORD);
    uint create_flags= HA_CREATE_TMP_TABLE | HA_CREATE_INTERNAL_TABLE |
        (table->keep_row_order ? HA_PRESERVE_INSERT_ORDER : 0);

    if (file_type != NO_RECORD && encrypt_tmp_disk_tables)
    {
      /* encryption is only supported for BLOCK_RECORD */
      file_type= BLOCK_RECORD;
      if (table->used_for_duplicate_elimination)
      {
        /*
          sql-layer expect the last column to be stored/restored also
          when it's null.

          This is probably a bug (that sql-layer doesn't annotate
          the column as not-null) but both heap, aria-static, aria-dynamic and
          myisam has this property. aria-block_record does not since it
          does not store null-columns at all.
          Emulate behaviour by making column not-nullable when creating the
          table.
        */
        uint cols= (uint)(*recinfo-start_recinfo);
        start_recinfo[cols-1].null_bit= 0;
      }
    }

    if (unlikely((error= maria_create(share->path.str, file_type, share->keys,
                                      &keydef, (uint) (*recinfo-start_recinfo),
                                      start_recinfo, share->uniques, &uniquedef,
                                      &create_info, create_flags))))
    {
      table->file->print_error(error,MYF(0));	/* purecov: inspected */
      table->db_stat=0;
      goto err;
    }
  }

  table->in_use->inc_status_created_tmp_disk_tables();
  table->in_use->inc_status_created_tmp_tables();
  share->db_record_offset= 1;
  table->set_created();
  DBUG_RETURN(0);
 err:
  DBUG_RETURN(1);
}

#else

/*
  Create internal (MyISAM or Maria) temporary table

  SYNOPSIS
    create_internal_tmp_table()
      table           Table object that descrimes the table to be created
      keyinfo         Description of the index (there is always one index)
      start_recinfo   engine's column descriptions
      recinfo INOUT   End of engine's column descriptions
      options         Option bits
   
  DESCRIPTION
    Create an internal emporary table according to passed description. The is
    assumed to have one unique index or constraint.

    The passed array or TMP_ENGINE_COLUMNDEF structures must have this form:

      1. 1-byte column (afaiu for 'deleted' flag) (note maybe not 1-byte
         when there are many nullable columns)
      2. Table columns
      3. One free TMP_ENGINE_COLUMNDEF element (*recinfo points here)
   
    This function may use the free element to create hash column for unique
    constraint.

   RETURN
     FALSE - OK
     TRUE  - Error
*/

/* Create internal MyISAM temporary table */

bool create_internal_tmp_table(TABLE *table, KEY *keyinfo, 
                               TMP_ENGINE_COLUMNDEF *start_recinfo,
                               TMP_ENGINE_COLUMNDEF **recinfo,
                               ulonglong options)
{
  int error;
  MI_KEYDEF keydef;
  MI_UNIQUEDEF uniquedef;
  TABLE_SHARE *share= table->s;
  DBUG_ENTER("create_internal_tmp_table");

  if (share->keys)
  {						// Get keys for ni_create
    bool using_unique_constraint=0;
    HA_KEYSEG *seg= (HA_KEYSEG*) alloc_root(&table->mem_root,
                                            sizeof(*seg) * keyinfo->user_defined_key_parts);
    if (!seg)
      goto err;

    bzero(seg, sizeof(*seg) * keyinfo->user_defined_key_parts);
    /*
       Note that a similar check is performed during
       subquery_types_allow_materialization. See MDEV-7122 for more details as
       to why. Whenever this changes, it must be updated there as well, for
       all tmp_table engines.
    */
    if (keyinfo->key_length > table->file->max_key_length() ||
	keyinfo->user_defined_key_parts > table->file->max_key_parts() ||
	share->uniques)
    {
      /* Can't create a key; Make a unique constraint instead of a key */
      share->keys=    0;
      share->uniques= 1;
      using_unique_constraint=1;
      bzero((char*) &uniquedef,sizeof(uniquedef));
      uniquedef.keysegs=keyinfo->user_defined_key_parts;
      uniquedef.seg=seg;
      uniquedef.null_are_equal=1;

      /* Create extra column for hash value */
      bzero((uchar*) *recinfo,sizeof(**recinfo));
      (*recinfo)->type= FIELD_CHECK;
      (*recinfo)->length=MI_UNIQUE_HASH_LENGTH;
      (*recinfo)++;
      /* Avoid warnings from valgrind */
      bzero(table->record[0]+ share->reclength, MI_UNIQUE_HASH_LENGTH);
      bzero(share->default_values+ share->reclength, MI_UNIQUE_HASH_LENGTH);
      share->reclength+= MI_UNIQUE_HASH_LENGTH;
    }
    else
    {
      /* Create an unique key */
      bzero((char*) &keydef,sizeof(keydef));
      keydef.flag= ((keyinfo->flags & HA_NOSAME) | HA_BINARY_PACK_KEY |
                    HA_PACK_KEY);
      keydef.keysegs=  keyinfo->user_defined_key_parts;
      keydef.seg= seg;
    }
    for (uint i=0; i < keyinfo->user_defined_key_parts ; i++,seg++)
    {
      Field *field=keyinfo->key_part[i].field;
      seg->flag=     0;
      seg->language= field->charset()->number;
      seg->length=   keyinfo->key_part[i].length;
      seg->start=    keyinfo->key_part[i].offset;
      if (field->flags & BLOB_FLAG)
      {
	seg->type=
	((keyinfo->key_part[i].key_type & FIELDFLAG_BINARY) ?
	 HA_KEYTYPE_VARBINARY2 : HA_KEYTYPE_VARTEXT2);
	seg->bit_start= (uint8)(field->pack_length() - portable_sizeof_char_ptr);
	seg->flag= HA_BLOB_PART;
	seg->length=0;			// Whole blob in unique constraint
      }
      else
      {
	seg->type= keyinfo->key_part[i].type;
        /* Tell handler if it can do suffic space compression */
	if (field->real_type() == MYSQL_TYPE_STRING &&
	    keyinfo->key_part[i].length > 4)
	  seg->flag|= HA_SPACE_PACK;
      }
      if (!(field->flags & NOT_NULL_FLAG))
      {
	seg->null_bit= field->null_bit;
	seg->null_pos= (uint) (field->null_ptr - (uchar*) table->record[0]);
	/*
	  We are using a GROUP BY on something that contains NULL
	  In this case we have to tell MyISAM that two NULL should
	  on INSERT be regarded at the same value
	*/
	if (!using_unique_constraint)
	  keydef.flag|= HA_NULL_ARE_EQUAL;
      }
    }
  }
  MI_CREATE_INFO create_info;
  bzero((char*) &create_info,sizeof(create_info));
  create_info.data_file_length= table->in_use->variables.tmp_disk_table_size;

  if (unlikely((error= mi_create(share->path.str, share->keys, &keydef,
		                 (uint) (*recinfo-start_recinfo),
                                 start_recinfo,
		                 share->uniques, &uniquedef,
                                 &create_info,
		                 HA_CREATE_TMP_TABLE |
                                 HA_CREATE_INTERNAL_TABLE |
                                 ((share->db_create_options &
                                   HA_OPTION_PACK_RECORD) ?
                                  HA_PACK_RECORD : 0)
                                 ))))
  {
    table->file->print_error(error,MYF(0));	/* purecov: inspected */
    table->db_stat=0;
    goto err;
  }
  table->in_use->inc_status_created_tmp_disk_tables();
  table->in_use->inc_status_created_tmp_tables();
  share->db_record_offset= 1;
  table->set_created();
  DBUG_RETURN(0);
 err:
  DBUG_RETURN(1);
}

#endif /* USE_ARIA_FOR_TMP_TABLES */


/*
  If a HEAP table gets full, create a internal table in MyISAM or Maria
  and copy all rows to this
*/


bool
create_internal_tmp_table_from_heap(THD *thd, TABLE *table,
                                    TMP_ENGINE_COLUMNDEF *start_recinfo,
                                    TMP_ENGINE_COLUMNDEF **recinfo, 
                                    int error,
                                    bool ignore_last_dupp_key_error,
                                    bool *is_duplicate)
{
  TABLE new_table;
  TABLE_SHARE share;
  const char *save_proc_info;
  int write_err= 0;
  DBUG_ENTER("create_internal_tmp_table_from_heap");
  if (is_duplicate)
    *is_duplicate= FALSE;

  if (table->s->db_type() != heap_hton || error != HA_ERR_RECORD_FILE_FULL)
  {
    /*
      We don't want this error to be converted to a warning, e.g. in case of
      INSERT IGNORE ... SELECT.
    */
    table->file->print_error(error, MYF(ME_FATALERROR));
    DBUG_RETURN(1);
  }
  new_table= *table;
  share= *table->s;
  new_table.s= &share;
  new_table.s->db_plugin= ha_lock_engine(thd, TMP_ENGINE_HTON);
  if (unlikely(!(new_table.file= get_new_handler(&share, &new_table.mem_root,
                                                 new_table.s->db_type()))))
    DBUG_RETURN(1);				// End of memory

  if (unlikely(new_table.file->set_ha_share_ref(&share.ha_share)))
  {
    delete new_table.file;
    DBUG_RETURN(1);
  }

  save_proc_info=thd->proc_info;
  THD_STAGE_INFO(thd, stage_converting_heap_to_myisam);

  new_table.no_rows= table->no_rows;
  if (create_internal_tmp_table(&new_table, table->key_info, start_recinfo,
                                recinfo,
                                thd->lex->select_lex.options | 
			        thd->variables.option_bits))
    goto err2;
  if (open_tmp_table(&new_table))
    goto err1;
  if (table->file->indexes_are_disabled())
    new_table.file->ha_disable_indexes(HA_KEY_SWITCH_ALL);
  table->file->ha_index_or_rnd_end();
  if (table->file->ha_rnd_init_with_error(1))
    DBUG_RETURN(1);
  if (new_table.no_rows)
    new_table.file->extra(HA_EXTRA_NO_ROWS);
  else
  {
    /* update table->file->stats.records */
    table->file->info(HA_STATUS_VARIABLE);
    new_table.file->ha_start_bulk_insert(table->file->stats.records);
  }

  /*
    copy all old rows from heap table to MyISAM table
    This is the only code that uses record[1] to read/write but this
    is safe as this is a temporary MyISAM table without timestamp/autoincrement
    or partitioning.
  */
  while (!table->file->ha_rnd_next(new_table.record[1]))
  {
    write_err= new_table.file->ha_write_tmp_row(new_table.record[1]);
    DBUG_EXECUTE_IF("raise_error", write_err= HA_ERR_FOUND_DUPP_KEY ;);
    if (write_err)
      goto err;
    if (unlikely(thd->check_killed()))
      goto err_killed;
  }
  if (!new_table.no_rows && new_table.file->ha_end_bulk_insert())
    goto err;
  /* copy row that filled HEAP table */
  if (unlikely((write_err=new_table.file->ha_write_tmp_row(table->record[0]))))
  {
    if (new_table.file->is_fatal_error(write_err, HA_CHECK_DUP) ||
	!ignore_last_dupp_key_error)
      goto err;
    if (is_duplicate)
      *is_duplicate= TRUE;
  }
  else
  {
    if (is_duplicate)
      *is_duplicate= FALSE;
  }

  /* remove heap table and change to use myisam table */
  (void) table->file->ha_rnd_end();
  (void) table->file->ha_close();          // This deletes the table !
  delete table->file;
  table->file=0;
  plugin_unlock(0, table->s->db_plugin);
  share.db_plugin= my_plugin_lock(0, share.db_plugin);
  new_table.s= table->s;                       // Keep old share
  *table= new_table;
  *table->s= share;
  
  table->file->change_table_ptr(table, table->s);
  table->use_all_columns();
  if (save_proc_info)
    thd_proc_info(thd, (!strcmp(save_proc_info,"Copying to tmp table") ?
                  "Copying to tmp table on disk" : save_proc_info));
  DBUG_RETURN(0);

 err:
  DBUG_PRINT("error",("Got error: %d",write_err));
  table->file->print_error(write_err, MYF(0));
err_killed:
  (void) table->file->ha_rnd_end();
  (void) new_table.file->ha_close();
 err1:
  new_table.file->ha_delete_table(new_table.s->path.str);
 err2:
  delete new_table.file;
  thd_proc_info(thd, save_proc_info);
  table->mem_root= new_table.mem_root;
  DBUG_RETURN(1);
}


void
free_tmp_table(THD *thd, TABLE *entry)
{
  MEM_ROOT own_root= entry->mem_root;
  const char *save_proc_info;
  DBUG_ENTER("free_tmp_table");
  DBUG_PRINT("enter",("table: %s  alias: %s",entry->s->table_name.str,
                      entry->alias.c_ptr()));

  save_proc_info=thd->proc_info;
  THD_STAGE_INFO(thd, stage_removing_tmp_table);

  if (entry->file && entry->is_created())
  {
    entry->file->ha_index_or_rnd_end();
    if (entry->db_stat)
    {
      entry->file->info(HA_STATUS_VARIABLE);
      thd->tmp_tables_size+= (entry->file->stats.data_file_length +
                              entry->file->stats.index_file_length);
      entry->file->ha_drop_table(entry->s->path.str);
    }
    else
      entry->file->ha_delete_table(entry->s->path.str);
    delete entry->file;
  }

  /* free blobs */
  for (Field **ptr=entry->field ; *ptr ; ptr++)
    (*ptr)->free();

  if (entry->temp_pool_slot != MY_BIT_NONE)
    bitmap_lock_clear_bit(&temp_pool, entry->temp_pool_slot);

  plugin_unlock(0, entry->s->db_plugin);
  entry->alias.free();

  if (entry->pos_in_table_list && entry->pos_in_table_list->table)
  {
    DBUG_ASSERT(entry->pos_in_table_list->table == entry);
    entry->pos_in_table_list->table= NULL;
  }

  free_root(&own_root, MYF(0)); /* the table is allocated in its own root */
  thd_proc_info(thd, save_proc_info);

  DBUG_VOID_RETURN;
}


/**
  @brief
  Set write_func of AGGR_OP object

  @param join_tab JOIN_TAB of the corresponding tmp table

  @details
  Function sets up write_func according to how AGGR_OP object that
  is attached to the given join_tab will be used in the query.
*/

void set_postjoin_aggr_write_func(JOIN_TAB *tab)
{
  JOIN *join= tab->join;
  TABLE *table= tab->table;
  AGGR_OP *aggr= tab->aggr;
  TMP_TABLE_PARAM *tmp_tbl= tab->tmp_table_param;

  DBUG_ASSERT(table && aggr);

  if (table->group && tmp_tbl->sum_func_count && 
      !tmp_tbl->precomputed_group_by)
  {
    /*
      Note for MyISAM tmp tables: if uniques is true keys won't be
      created.
    */
    if (table->s->keys && !table->s->uniques)
    {
      DBUG_PRINT("info",("Using end_update"));
      aggr->set_write_func(end_update);
    }
    else
    {
      DBUG_PRINT("info",("Using end_unique_update"));
      aggr->set_write_func(end_unique_update);
    }
  }
  else if (join->sort_and_group && !tmp_tbl->precomputed_group_by &&
           !join->sort_and_group_aggr_tab && join->tables_list &&
           join->top_join_tab_count)
  {
    DBUG_PRINT("info",("Using end_write_group"));
    aggr->set_write_func(end_write_group);
    join->sort_and_group_aggr_tab= tab;
  }
  else
  {
    DBUG_PRINT("info",("Using end_write"));
    aggr->set_write_func(end_write);
    if (tmp_tbl->precomputed_group_by)
    {
      /*
        A preceding call to create_tmp_table in the case when loose
        index scan is used guarantees that
        TMP_TABLE_PARAM::items_to_copy has enough space for the group
        by functions. It is OK here to use memcpy since we copy
        Item_sum pointers into an array of Item pointers.
      */
      memcpy(tmp_tbl->items_to_copy + tmp_tbl->func_count,
             join->sum_funcs,
             sizeof(Item*)*tmp_tbl->sum_func_count);
      tmp_tbl->items_to_copy[tmp_tbl->func_count+tmp_tbl->sum_func_count]= 0;
    }
  }
}


/**
  @details
  Rows produced by a join sweep may end up in a temporary table or be sent
  to a client. Set the function of the nested loop join algorithm which
  handles final fully constructed and matched records.

  @param join   join to setup the function for.

  @return
    end_select function to use. This function can't fail.
*/

Next_select_func setup_end_select_func(JOIN *join, JOIN_TAB *tab)
{
  TMP_TABLE_PARAM *tmp_tbl= tab ? tab->tmp_table_param : &join->tmp_table_param;

  /* 
     Choose method for presenting result to user. Use end_send_group
     if the query requires grouping (has a GROUP BY clause and/or one or
     more aggregate functions). Use end_send if the query should not
     be grouped.
   */
  if (join->sort_and_group && !tmp_tbl->precomputed_group_by)
  {
    DBUG_PRINT("info",("Using end_send_group"));
    return end_send_group;
  }
  DBUG_PRINT("info",("Using end_send"));
  return end_send;
}


/**
  Make a join of all tables and write it on socket or to table.

  @retval
    0  if ok
  @retval
    1  if error is sent
  @retval
    -1  if error should be sent
*/

static int
do_select(JOIN *join, Procedure *procedure)
{
  int rc= 0;
  enum_nested_loop_state error= NESTED_LOOP_OK;
  DBUG_ENTER("do_select");

  if (join->pushdown_query)
  {
    /* Select fields are in the temporary table */
    join->fields= &join->tmp_fields_list1;
    /* Setup HAVING to work with fields in temporary table */
    join->set_items_ref_array(join->items1);
    /* The storage engine will take care of the group by query result */
    int res= join->pushdown_query->execute(join);

    if (res)
      DBUG_RETURN(res);

    if (join->pushdown_query->store_data_in_temp_table)
    {
      JOIN_TAB *last_tab= join->join_tab + join->table_count -
                          join->exec_join_tab_cnt();      
      last_tab->next_select= end_send;

      enum_nested_loop_state state= last_tab->aggr->end_send();
      if (state >= NESTED_LOOP_OK)
        state= sub_select(join, last_tab, true);

      if (state < NESTED_LOOP_OK)
        res= 1;

      if (join->result->send_eof())
        res= 1;
    }
    DBUG_RETURN(res);
  }
  
  join->procedure= procedure;
  join->duplicate_rows= join->send_records=0;
  if (join->only_const_tables() && !join->need_tmp)
  {
    Next_select_func end_select= setup_end_select_func(join, NULL);

    /*
      HAVING will be checked after processing aggregate functions,
      But WHERE should checked here (we alredy have read tables).
      Notice that make_join_select() splits all conditions in this case
      into two groups exec_const_cond and outer_ref_cond.
      If join->table_count == join->const_tables then it is
      sufficient to check only the condition pseudo_bits_cond.
    */
    DBUG_ASSERT(join->outer_ref_cond == NULL);
    if (!join->pseudo_bits_cond || join->pseudo_bits_cond->val_int())
    {
      // HAVING will be checked by end_select
      error= (*end_select)(join, 0, 0);
      if (error >= NESTED_LOOP_OK)
	error= (*end_select)(join, 0, 1);

      /*
        If we don't go through evaluate_join_record(), do the counting
        here.  join->send_records is increased on success in end_send(),
        so we don't touch it here.
      */
      join->join_examined_rows++;
      DBUG_ASSERT(join->join_examined_rows <= 1);
    }
    else if (join->send_row_on_empty_set())
    {
      table_map cleared_tables= (table_map) 0;
      if (end_select == end_send_group)
      {
        /*
          Was a grouping query but we did not find any rows. In this case
          we clear all tables to get null in any referenced fields,
          like in case of:
          SELECT MAX(a) AS f1, a AS f2 FROM t1 WHERE VALUE(a) IS NOT NULL
        */
        clear_tables(join, &cleared_tables);
      }
      if (!join->having || join->having->val_int())
      {
        List<Item> *columns_list= (procedure ? &join->procedure_fields_list :
                                   join->fields);
        rc= join->result->send_data(*columns_list) > 0;
      }
      /*
        We have to remove the null markings from the tables as this table
        may be part of a sub query that is re-evaluated
      */
      if (cleared_tables)
        unclear_tables(join, &cleared_tables);
    }
    /*
      An error can happen when evaluating the conds 
      (the join condition and piece of where clause 
      relevant to this join table).
    */
    if (unlikely(join->thd->is_error()))
      error= NESTED_LOOP_ERROR;
  }
  else
  {
    DBUG_EXECUTE_IF("show_explain_probe_do_select", 
                    if (dbug_user_var_equals_int(join->thd, 
                                                 "show_explain_probe_select_id", 
                                                 join->select_lex->select_number))
                          dbug_serve_apcs(join->thd, 1);
                   );

    JOIN_TAB *join_tab= join->join_tab +
                        (join->tables_list ? join->const_tables : 0);
    if (join->outer_ref_cond && !join->outer_ref_cond->val_int())
      error= NESTED_LOOP_NO_MORE_ROWS;
    else
      error= join->first_select(join,join_tab,0);
    if (error >= NESTED_LOOP_OK && likely(join->thd->killed != ABORT_QUERY))
      error= join->first_select(join,join_tab,1);
  }

  join->thd->limit_found_rows= join->send_records - join->duplicate_rows;

  if (error == NESTED_LOOP_NO_MORE_ROWS ||
      unlikely(join->thd->killed == ABORT_QUERY))
    error= NESTED_LOOP_OK;

  /*
    For "order by with limit", we cannot rely on send_records, but need
    to use the rowcount read originally into the join_tab applying the
    filesort. There cannot be any post-filtering conditions, nor any
    following join_tabs in this case, so this rowcount properly represents
    the correct number of qualifying rows.
  */
  if (join->order)
  {
    // Save # of found records prior to cleanup
    JOIN_TAB *sort_tab;
    JOIN_TAB *join_tab= join->join_tab;
    uint const_tables= join->const_tables;

    // Take record count from first non constant table or from last tmp table
    if (join->aggr_tables > 0)
      sort_tab= join_tab + join->top_join_tab_count + join->aggr_tables - 1;
    else
    {
      DBUG_ASSERT(!join->only_const_tables());
      sort_tab= join_tab + const_tables;
    }
    if (sort_tab->filesort &&
        join->select_options & OPTION_FOUND_ROWS &&
        sort_tab->filesort->sortorder &&
        sort_tab->filesort->limit != HA_POS_ERROR)
    {
      join->thd->limit_found_rows= sort_tab->records;
    }
  }

  {
    /*
      The following will unlock all cursors if the command wasn't an
      update command
    */
    join->join_free();			// Unlock all cursors
  }
  if (error == NESTED_LOOP_OK)
  {
    /*
      Sic: this branch works even if rc != 0, e.g. when
      send_data above returns an error.
    */
    if (unlikely(join->result->send_eof()))
      rc= 1;                                  // Don't send error
    DBUG_PRINT("info",("%ld records output", (long) join->send_records));
  }
  else
    rc= -1;
#ifndef DBUG_OFF
  if (rc)
  {
    DBUG_PRINT("error",("Error: do_select() failed"));
  }
#endif
  rc= join->thd->is_error() ? -1 : rc;
  DBUG_RETURN(rc);
}


int rr_sequential_and_unpack(READ_RECORD *info)
{
  int error;
  if (unlikely((error= rr_sequential(info))))
    return error;
  
  for (Copy_field *cp= info->copy_field; cp != info->copy_field_end; cp++)
    (*cp->do_copy)(cp);

  return error;
}


/**
  @brief
  Instantiates temporary table

  @param  table           Table object that describes the table to be
                          instantiated
  @param  keyinfo         Description of the index (there is always one index)
  @param  start_recinfo   Column descriptions
  @param  recinfo INOUT   End of column descriptions
  @param  options         Option bits

  @details
    Creates tmp table and opens it.

  @return
     FALSE - OK
     TRUE  - Error
*/

bool instantiate_tmp_table(TABLE *table, KEY *keyinfo, 
                           TMP_ENGINE_COLUMNDEF *start_recinfo,
                           TMP_ENGINE_COLUMNDEF **recinfo,
                           ulonglong options)
{
  if (table->s->db_type() == TMP_ENGINE_HTON)
  {
    /*
      If it is not heap (in-memory) table then convert index to unique
      constrain.
    */
    MEM_CHECK_DEFINED(table->record[0], table->s->reclength);
    if (create_internal_tmp_table(table, keyinfo, start_recinfo, recinfo,
                                  options))
      return TRUE;
    MEM_CHECK_DEFINED(table->record[0], table->s->reclength);
  }
  if (open_tmp_table(table))
    return TRUE;

  return FALSE;
}


/**
  @brief 
  Accumulate rows of the result of an aggregation operation in a tmp table

  @param join  pointer to the structure providing all context info for the query
  @param join_tab the JOIN_TAB object to which the operation is attached
  @param end_records  TRUE <=> all records were accumulated, send them further

  @details
  This function accumulates records of the aggreagation operation for 
  the node join_tab from the execution plan in a tmp table. To add a new
  record the function calls join_tab->aggr->put_records.
  When there is no more records to save, in this
  case the end_of_records argument == true, function tells the operation to
  send records further by calling aggr->send_records().
  When all records are sent this function passes 'end_of_records' signal
  further by calling sub_select() with end_of_records argument set to
  true. After that aggr->end_send() is called to tell the operation that
  it could end internal buffer scan.

  @note
  This function is not expected to be called when dynamic range scan is
  used to scan join_tab because  range scans aren't used for tmp tables.

  @return
    return one of enum_nested_loop_state.
*/

enum_nested_loop_state
sub_select_postjoin_aggr(JOIN *join, JOIN_TAB *join_tab, bool end_of_records)
{
  enum_nested_loop_state rc;
  AGGR_OP *aggr= join_tab->aggr;

  /* This function cannot be called if join_tab has no associated aggregation */
  DBUG_ASSERT(aggr != NULL);

  DBUG_ENTER("sub_select_aggr_tab");

  if (join->thd->killed)
  {
    /* The user has aborted the execution of the query */
    join->thd->send_kill_message();
    DBUG_RETURN(NESTED_LOOP_KILLED);
  }

  if (end_of_records)
  {
    rc= aggr->end_send();
    if (rc >= NESTED_LOOP_OK)
      rc= sub_select(join, join_tab, end_of_records);
    DBUG_RETURN(rc);
  }

  rc= aggr->put_record();

  DBUG_RETURN(rc);
}


/*
  Fill the join buffer with partial records, retrieve all full matches for
  them

  SYNOPSIS
    sub_select_cache()
      join         pointer to the structure providing all context info for the
                   query
      join_tab     the first next table of the execution plan to be retrieved
      end_records  true when we need to perform final steps of the retrieval

  DESCRIPTION
    For a given table Ti= join_tab from the sequence of tables of the chosen 
    execution plan T1,...,Ti,...,Tn the function just put the partial record
    t1,...,t[i-1] into the join buffer associated with table Ti unless this
    is the last record added into the buffer. In this case,  the function 
    additionally finds all matching full records for all partial
    records accumulated in the buffer, after which it cleans the buffer up.
    If a partial join record t1,...,ti is extended utilizing a dynamic
    range scan then it is not put into the join buffer. Rather all matching
    records are found for it at once by the function sub_select.

  NOTES
    The function implements the algorithmic schema for both Blocked Nested
    Loop Join and Batched Key Access Join. The difference can be seen only at
    the level of of the implementation of the put_record and join_records
    virtual methods for the cache object associated with the join_tab.
    The put_record method accumulates records in the cache, while the 
    join_records method builds all matching join records and send them into
    the output stream.  
      
  RETURN
    return one of enum_nested_loop_state, except NESTED_LOOP_NO_MORE_ROWS.
*/ 

enum_nested_loop_state
sub_select_cache(JOIN *join, JOIN_TAB *join_tab, bool end_of_records)
{
  enum_nested_loop_state rc;
  JOIN_CACHE *cache= join_tab->cache;
  DBUG_ENTER("sub_select_cache");

  /*
    This function cannot be called if join_tab has no associated join
    buffer
  */
  DBUG_ASSERT(cache != NULL);

  join_tab->cache->reset_join(join);

  if (end_of_records)
  {
    rc= cache->join_records(FALSE);
    if (rc == NESTED_LOOP_OK || rc == NESTED_LOOP_NO_MORE_ROWS ||
        rc == NESTED_LOOP_QUERY_LIMIT)
      rc= sub_select(join, join_tab, end_of_records);
    DBUG_RETURN(rc);
  }
  if (unlikely(join->thd->check_killed()))
  {
    /* The user has aborted the execution of the query */
    DBUG_RETURN(NESTED_LOOP_KILLED);
  }
  if (!test_if_use_dynamic_range_scan(join_tab))
  {
    if (!cache->put_record())
      DBUG_RETURN(NESTED_LOOP_OK); 
    /* 
      We has decided that after the record we've just put into the buffer
      won't add any more records. Now try to find all the matching 
      extensions for all records in the buffer.
    */ 
    rc= cache->join_records(FALSE);
    DBUG_RETURN(rc);
  }
  /*
     TODO: Check whether we really need the call below and we can't do
           without it. If it's not the case remove it.
  */ 
  rc= cache->join_records(TRUE);
  if (rc == NESTED_LOOP_OK || rc == NESTED_LOOP_NO_MORE_ROWS ||
      rc == NESTED_LOOP_QUERY_LIMIT)
    rc= sub_select(join, join_tab, end_of_records);
  DBUG_RETURN(rc);
}

/**
  Retrieve records ends with a given beginning from the result of a join.

    For a given partial join record consisting of records from the tables 
    preceding the table join_tab in the execution plan, the function
    retrieves all matching full records from the result set and
    send them to the result set stream. 

  @note
    The function effectively implements the  final (n-k) nested loops
    of nested loops join algorithm, where k is the ordinal number of
    the join_tab table and n is the total number of tables in the join query.
    It performs nested loops joins with all conjunctive predicates from
    the where condition pushed as low to the tables as possible.
    E.g. for the query
    @code
      SELECT * FROM t1,t2,t3
      WHERE t1.a=t2.a AND t2.b=t3.b AND t1.a BETWEEN 5 AND 9
    @endcode
    the predicate (t1.a BETWEEN 5 AND 9) will be pushed to table t1,
    given the selected plan prescribes to nest retrievals of the
    joined tables in the following order: t1,t2,t3.
    A pushed down predicate are attached to the table which it pushed to,
    at the field join_tab->select_cond.
    When executing a nested loop of level k the function runs through
    the rows of 'join_tab' and for each row checks the pushed condition
    attached to the table.
    If it is false the function moves to the next row of the
    table. If the condition is true the function recursively executes (n-k-1)
    remaining embedded nested loops.
    The situation becomes more complicated if outer joins are involved in
    the execution plan. In this case the pushed down predicates can be
    checked only at certain conditions.
    Suppose for the query
    @code
      SELECT * FROM t1 LEFT JOIN (t2,t3) ON t3.a=t1.a
      WHERE t1>2 AND (t2.b>5 OR t2.b IS NULL)
    @endcode
    the optimizer has chosen a plan with the table order t1,t2,t3.
    The predicate P1=t1>2 will be pushed down to the table t1, while the
    predicate P2=(t2.b>5 OR t2.b IS NULL) will be attached to the table
    t2. But the second predicate can not be unconditionally tested right
    after a row from t2 has been read. This can be done only after the
    first row with t3.a=t1.a has been encountered.
    Thus, the second predicate P2 is supplied with a guarded value that are
    stored in the field 'found' of the first inner table for the outer join
    (table t2). When the first row with t3.a=t1.a for the  current row 
    of table t1  appears, the value becomes true. For now on the predicate
    is evaluated immediately after the row of table t2 has been read.
    When the first row with t3.a=t1.a has been encountered all
    conditions attached to the inner tables t2,t3 must be evaluated.
    Only when all of them are true the row is sent to the output stream.
    If not, the function returns to the lowest nest level that has a false
    attached condition.
    The predicates from on expressions are also pushed down. If in the 
    the above example the on expression were (t3.a=t1.a AND t2.a=t1.a),
    then t1.a=t2.a would be pushed down to table t2, and without any
    guard.
    If after the run through all rows of table t2, the first inner table
    for the outer join operation, it turns out that no matches are
    found for the current row of t1, then current row from table t1
    is complemented by nulls  for t2 and t3. Then the pushed down predicates
    are checked for the composed row almost in the same way as it had
    been done for the first row with a match. The only difference is
    the predicates from on expressions are not checked. 

  @par
  @b IMPLEMENTATION
  @par
    The function forms output rows for a current partial join of k
    tables tables recursively.
    For each partial join record ending with a certain row from
    join_tab it calls sub_select that builds all possible matching
    tails from the result set.
    To be able  check predicates conditionally items of the class
    Item_func_trig_cond are employed.
    An object of  this class is constructed from an item of class COND
    and a pointer to a guarding boolean variable.
    When the value of the guard variable is true the value of the object
    is the same as the value of the predicate, otherwise it's just returns
    true. 
    To carry out a return to a nested loop level of join table t the pointer 
    to t is remembered in the field 'return_rtab' of the join structure.
    Consider the following query:
    @code
        SELECT * FROM t1,
                      LEFT JOIN
                      (t2, t3 LEFT JOIN (t4,t5) ON t5.a=t3.a)
                      ON t4.a=t2.a
           WHERE (t2.b=5 OR t2.b IS NULL) AND (t4.b=2 OR t4.b IS NULL)
    @endcode
    Suppose the chosen execution plan dictates the order t1,t2,t3,t4,t5
    and suppose for a given joined rows from tables t1,t2,t3 there are
    no rows in the result set yet.
    When first row from t5 that satisfies the on condition
    t5.a=t3.a is found, the pushed down predicate t4.b=2 OR t4.b IS NULL
    becomes 'activated', as well the predicate t4.a=t2.a. But
    the predicate (t2.b=5 OR t2.b IS NULL) can not be checked until
    t4.a=t2.a becomes true. 
    In order not to re-evaluate the predicates that were already evaluated
    as attached pushed down predicates, a pointer to the the first
    most inner unmatched table is maintained in join_tab->first_unmatched.
    Thus, when the first row from t5 with t5.a=t3.a is found
    this pointer for t5 is changed from t4 to t2.             

    @par
    @b STRUCTURE @b NOTES
    @par
    join_tab->first_unmatched points always backwards to the first inner
    table of the embedding nested join, if any.

  @param join      pointer to the structure providing all context info for
                   the query
  @param join_tab  the first next table of the execution plan to be retrieved
  @param end_records  true when we need to perform final steps of retrival   

  @return
    return one of enum_nested_loop_state, except NESTED_LOOP_NO_MORE_ROWS.
*/

enum_nested_loop_state
sub_select(JOIN *join,JOIN_TAB *join_tab,bool end_of_records)
{
  DBUG_ENTER("sub_select");

  if (join_tab->last_inner)
  {
    JOIN_TAB *last_inner_tab= join_tab->last_inner;
    for (JOIN_TAB  *jt= join_tab; jt <= last_inner_tab; jt++)
      jt->table->null_row= 0;
  }
  else
    join_tab->table->null_row=0;

  if (end_of_records)
  {
    enum_nested_loop_state nls=
      (*join_tab->next_select)(join,join_tab+1,end_of_records);
    DBUG_RETURN(nls);
  }
  join_tab->tracker->r_scans++;

  int error;
  enum_nested_loop_state rc= NESTED_LOOP_OK;
  READ_RECORD *info= &join_tab->read_record;


  for (SJ_TMP_TABLE *flush_dups_table= join_tab->flush_weedout_table;
       flush_dups_table;
       flush_dups_table= flush_dups_table->next_flush_table)
  {
    flush_dups_table->sj_weedout_delete_rows();
  }

  if (!join_tab->preread_init_done && join_tab->preread_init())
    DBUG_RETURN(NESTED_LOOP_ERROR);

  join->return_tab= join_tab;

  if (join_tab->last_inner)
  {
    /* join_tab is the first inner table for an outer join operation. */

    /* Set initial state of guard variables for this table.*/
    join_tab->found=0;
    join_tab->not_null_compl= 1;

    /* Set first_unmatched for the last inner table of this group */
    join_tab->last_inner->first_unmatched= join_tab;
    if (join_tab->on_precond && !join_tab->on_precond->val_int())
      rc= NESTED_LOOP_NO_MORE_ROWS;
  }
  join->thd->get_stmt_da()->reset_current_row_for_warning();

  if (rc != NESTED_LOOP_NO_MORE_ROWS && 
      (rc= join_tab_execution_startup(join_tab)) < 0)
    DBUG_RETURN(rc);
  
  if (join_tab->loosescan_match_tab)
    join_tab->loosescan_match_tab->found_match= FALSE;

  if (rc != NESTED_LOOP_NO_MORE_ROWS)
  {
    error= (*join_tab->read_first_record)(join_tab);
    if (!error && join_tab->keep_current_rowid)
      join_tab->table->file->position(join_tab->table->record[0]);    
    rc= evaluate_join_record(join, join_tab, error);
  }

  /* 
    Note: psergey has added the 2nd part of the following condition; the 
    change should probably be made in 5.1, too.
  */
  bool skip_over= FALSE;
  while (rc == NESTED_LOOP_OK && join->return_tab >= join_tab)
  {
    if (join_tab->loosescan_match_tab && 
        join_tab->loosescan_match_tab->found_match)
    {
      KEY *key= join_tab->table->key_info + join_tab->loosescan_key;
      key_copy(join_tab->loosescan_buf, join_tab->table->record[0], key, 
               join_tab->loosescan_key_len);
      skip_over= TRUE;
    }

    error= info->read_record();

    if (skip_over && likely(!error))
    {
      if (!key_cmp(join_tab->table->key_info[join_tab->loosescan_key].key_part,
                   join_tab->loosescan_buf, join_tab->loosescan_key_len))
      {
        /* 
          This is the LooseScan action: skip over records with the same key
          value if we already had a match for them.
        */
        continue;
      }
      join_tab->loosescan_match_tab->found_match= FALSE;
      skip_over= FALSE;
    }

    if (join_tab->keep_current_rowid && likely(!error))
      join_tab->table->file->position(join_tab->table->record[0]);
    
    rc= evaluate_join_record(join, join_tab, error);
  }

  if (rc == NESTED_LOOP_NO_MORE_ROWS &&
      join_tab->last_inner && !join_tab->found)
    rc= evaluate_null_complemented_join_record(join, join_tab);

  if (rc == NESTED_LOOP_NO_MORE_ROWS)
    rc= NESTED_LOOP_OK;
  DBUG_RETURN(rc);
}

/**
  @brief Process one row of the nested loop join.

  This function will evaluate parts of WHERE/ON clauses that are
  applicable to the partial row on hand and in case of success
  submit this row to the next level of the nested loop.

  @param  join     - The join object
  @param  join_tab - The most inner join_tab being processed
  @param  error > 0: Error, terminate processing
                = 0: (Partial) row is available
                < 0: No more rows available at this level
  @return Nested loop state (Ok, No_more_rows, Error, Killed)
*/

static enum_nested_loop_state
evaluate_join_record(JOIN *join, JOIN_TAB *join_tab,
                     int error)
{
  bool shortcut_for_distinct= join_tab->shortcut_for_distinct;
  ha_rows found_records=join->found_records;
  COND *select_cond= join_tab->select_cond;
  bool select_cond_result= TRUE;

  DBUG_ENTER("evaluate_join_record");
  DBUG_PRINT("enter",
             ("evaluate_join_record join: %p join_tab: %p"
              " cond: %p error: %d  alias %s",
              join, join_tab, select_cond, error,
              join_tab->table->alias.ptr()));

  if (error > 0 || unlikely(join->thd->is_error())) // Fatal error
    DBUG_RETURN(NESTED_LOOP_ERROR);
  if (error < 0)
    DBUG_RETURN(NESTED_LOOP_NO_MORE_ROWS);
  if (unlikely(join->thd->check_killed()))       // Aborted by user
  {
    DBUG_RETURN(NESTED_LOOP_KILLED);            /* purecov: inspected */
  }

  join_tab->tracker->r_rows++;

  if (select_cond)
  {
    select_cond_result= MY_TEST(select_cond->val_int());

    /* check for errors evaluating the condition */
    if (unlikely(join->thd->is_error()))
      DBUG_RETURN(NESTED_LOOP_ERROR);
  }

  if (!select_cond || select_cond_result)
  {
    /*
      There is no select condition or the attached pushed down
      condition is true => a match is found.
    */
    join_tab->tracker->r_rows_after_where++;

    bool found= 1;
    while (join_tab->first_unmatched && found)
    {
      /*
        The while condition is always false if join_tab is not
        the last inner join table of an outer join operation.
      */
      JOIN_TAB *first_unmatched= join_tab->first_unmatched;
      /*
        Mark that a match for current outer table is found.
        This activates push down conditional predicates attached
        to the all inner tables of the outer join.
      */
      first_unmatched->found= 1;
      for (JOIN_TAB *tab= first_unmatched; tab <= join_tab; tab++)
      {
        /*
          Check whether 'not exists' optimization can be used here.
          If  tab->table->reginfo.not_exists_optimize is set to true
          then WHERE contains a conjunctive predicate IS NULL over
          a non-nullable field of tab. When activated this predicate
          will filter out all records with matches for the left part
          of the outer join whose inner tables start from the
          first_unmatched table and include table tab. To safely use
          'not exists' optimization we have to check that the
          IS NULL predicate is really activated, i.e. all guards
          that wrap it are in the 'open' state. 
	*/  
	bool not_exists_opt_is_applicable=
               tab->table->reginfo.not_exists_optimize;
	for (JOIN_TAB *first_upper= first_unmatched->first_upper;
             not_exists_opt_is_applicable && first_upper;
             first_upper= first_upper->first_upper)
        {
          if (!first_upper->found)
            not_exists_opt_is_applicable= false;
        }
        /* Check all predicates that has just been activated. */
        /*
          Actually all predicates non-guarded by first_unmatched->found
          will be re-evaluated again. It could be fixed, but, probably,
          it's not worth doing now.
        */
        if (tab->select_cond && !tab->select_cond->val_int())
        {
          /* The condition attached to table tab is false */
          if (tab == join_tab)
          {
            found= 0;
            if (not_exists_opt_is_applicable)
              DBUG_RETURN(NESTED_LOOP_NO_MORE_ROWS);
          }            
          else
          {
            /*
              Set a return point if rejected predicate is attached
              not to the last table of the current nest level.
            */
            join->return_tab= tab;
            if (not_exists_opt_is_applicable)
              DBUG_RETURN(NESTED_LOOP_NO_MORE_ROWS);
            else
              DBUG_RETURN(NESTED_LOOP_OK);
          }
        }
      }
      /*
        Check whether join_tab is not the last inner table
        for another embedding outer join.
      */
      if ((first_unmatched= first_unmatched->first_upper) &&
          first_unmatched->last_inner != join_tab)
        first_unmatched= 0;
      join_tab->first_unmatched= first_unmatched;
    }

    JOIN_TAB *return_tab= join->return_tab;
    join_tab->found_match= TRUE;

    if (join_tab->check_weed_out_table && found)
    {
      int res= join_tab->check_weed_out_table->sj_weedout_check_row(join->thd);
      DBUG_PRINT("info", ("weedout_check: %d", res));
      if (res == -1)
        DBUG_RETURN(NESTED_LOOP_ERROR);
      else if (res == 1)
        found= FALSE;
    }
    else if (join_tab->do_firstmatch)
    {
      /* 
        We should return to the join_tab->do_firstmatch after we have 
        enumerated all the suffixes for current prefix row combination
      */
      return_tab= join_tab->do_firstmatch;
    }

    /*
      It was not just a return to lower loop level when one
      of the newly activated predicates is evaluated as false
      (See above join->return_tab= tab).
    */
    join->join_examined_rows++;
    DBUG_PRINT("counts", ("join->examined_rows++: %lu  found: %d",
                          (ulong) join->join_examined_rows, (int) found));

    if (found)
    {
      enum enum_nested_loop_state rc;
      /* A match from join_tab is found for the current partial join. */
      rc= (*join_tab->next_select)(join, join_tab+1, 0);
      join->thd->get_stmt_da()->inc_current_row_for_warning();
      if (rc != NESTED_LOOP_OK && rc != NESTED_LOOP_NO_MORE_ROWS)
        DBUG_RETURN(rc);
      if (return_tab < join->return_tab)
        join->return_tab= return_tab;

      /* check for errors evaluating the condition */
      if (unlikely(join->thd->is_error()))
        DBUG_RETURN(NESTED_LOOP_ERROR);

      if (join->return_tab < join_tab)
        DBUG_RETURN(NESTED_LOOP_OK);
      /*
        Test if this was a SELECT DISTINCT query on a table that
        was not in the field list;  In this case we can abort if
        we found a row, as no new rows can be added to the result.
      */
      if (shortcut_for_distinct && found_records != join->found_records)
        DBUG_RETURN(NESTED_LOOP_NO_MORE_ROWS);
    }
    else
    {
      join->thd->get_stmt_da()->inc_current_row_for_warning();
      join_tab->read_record.unlock_row(join_tab);
    }
  }
  else
  {
    /*
      The condition pushed down to the table join_tab rejects all rows
      with the beginning coinciding with the current partial join.
    */
    join->join_examined_rows++;
    join->thd->get_stmt_da()->inc_current_row_for_warning();
    join_tab->read_record.unlock_row(join_tab);
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}

/**

  @details
    Construct a NULL complimented partial join record and feed it to the next
    level of the nested loop. This function is used in case we have
    an OUTER join and no matching record was found.
*/

static enum_nested_loop_state
evaluate_null_complemented_join_record(JOIN *join, JOIN_TAB *join_tab)
{
  /*
    The table join_tab is the first inner table of a outer join operation
    and no matches has been found for the current outer row.
  */
  JOIN_TAB *last_inner_tab= join_tab->last_inner;
  /* Cache variables for faster loop */
  COND *select_cond;
  for ( ; join_tab <= last_inner_tab ; join_tab++)
  {
    /* Change the the values of guard predicate variables. */
    join_tab->found= 1;
    join_tab->not_null_compl= 0;
    /* The outer row is complemented by nulls for each inner tables */
    restore_record(join_tab->table,s->default_values);  // Make empty record
    mark_as_null_row(join_tab->table);       // For group by without error
    select_cond= join_tab->select_cond;
    /* Check all attached conditions for inner table rows. */
    if (select_cond && !select_cond->val_int())
      return NESTED_LOOP_OK;
  }
  join_tab--;
  /*
    The row complemented by nulls might be the first row
    of embedding outer joins.
    If so, perform the same actions as in the code
    for the first regular outer join row above.
  */
  for ( ; ; )
  {
    JOIN_TAB *first_unmatched= join_tab->first_unmatched;
    if ((first_unmatched= first_unmatched->first_upper) &&
        first_unmatched->last_inner != join_tab)
      first_unmatched= 0;
    join_tab->first_unmatched= first_unmatched;
    if (!first_unmatched)
      break;
    first_unmatched->found= 1;
    for (JOIN_TAB *tab= first_unmatched; tab <= join_tab; tab++)
    {
      if (tab->select_cond && !tab->select_cond->val_int())
      {
        join->return_tab= tab;
        return NESTED_LOOP_OK;
      }
    }
  }
  /*
    The row complemented by nulls satisfies all conditions
    attached to inner tables.
  */
  if (join_tab->check_weed_out_table)
  {
    int res= join_tab->check_weed_out_table->sj_weedout_check_row(join->thd);
    if (res == -1)
      return NESTED_LOOP_ERROR;
    else if (res == 1)
      return NESTED_LOOP_OK;
  }
  else if (join_tab->do_firstmatch)
  {
    /* 
      We should return to the join_tab->do_firstmatch after we have 
      enumerated all the suffixes for current prefix row combination
    */
    if (join_tab->do_firstmatch < join->return_tab)
      join->return_tab= join_tab->do_firstmatch;
  }

  /*
    Send the row complemented by nulls to be joined with the
    remaining tables.
  */
  return (*join_tab->next_select)(join, join_tab+1, 0);
}

/*****************************************************************************
  The different ways to read a record
  Returns -1 if row was not found, 0 if row was found and 1 on errors
*****************************************************************************/

/** Help function when we get some an error from the table handler. */

int report_error(TABLE *table, int error)
{
  if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND)
  {
    table->status= STATUS_GARBAGE;
    return -1;					// key not found; ok
  }
  /*
    Locking reads can legally return also these errors, do not
    print them to the .err log
  */
  if (error != HA_ERR_LOCK_DEADLOCK && error != HA_ERR_LOCK_WAIT_TIMEOUT
      && error != HA_ERR_TABLE_DEF_CHANGED && !table->in_use->killed)
    sql_print_error("Got error %d when reading table '%s'",
		    error, table->s->path.str);
  table->file->print_error(error,MYF(0));
  return 1;
}


int safe_index_read(JOIN_TAB *tab)
{
  int error;
  TABLE *table= tab->table;
  if (unlikely((error=
                table->file->ha_index_read_map(table->record[0],
                                               tab->ref.key_buff,
                                               make_prev_keypart_map(tab->ref.key_parts),
                                               HA_READ_KEY_EXACT))))
    return report_error(table, error);
  return 0;
}


/**
  Reads content of constant table

  @param tab  table
  @param pos  position of table in query plan

  @retval 0   ok, one row was found or one NULL-complemented row was created
  @retval -1  ok, no row was found and no NULL-complemented row was created
  @retval 1   error
*/

static int
join_read_const_table(THD *thd, JOIN_TAB *tab, POSITION *pos)
{
  int error;
  TABLE_LIST *tbl;
  DBUG_ENTER("join_read_const_table");
  TABLE *table=tab->table;
  table->const_table=1;
  table->null_row=0;
  table->status=STATUS_NO_RECORD;
  
  if (tab->table->pos_in_table_list->is_materialized_derived() &&
      !tab->table->pos_in_table_list->fill_me)
  {
    //TODO: don't get here at all
    /* Skip materialized derived tables/views. */
    DBUG_RETURN(0);
  }
  else if (tab->table->pos_in_table_list->jtbm_subselect && 
          tab->table->pos_in_table_list->jtbm_subselect->is_jtbm_const_tab)
  {
    /* Row will not be found */
    int res;
    if (tab->table->pos_in_table_list->jtbm_subselect->jtbm_const_row_found)
      res= 0;
    else
      res= -1;
    DBUG_RETURN(res);
  }
  else if (tab->type == JT_SYSTEM)
  {
    if (unlikely((error=join_read_system(tab))))
    {						// Info for DESCRIBE
      tab->info= ET_CONST_ROW_NOT_FOUND;
      /* Mark for EXPLAIN that the row was not found */
      pos->records_read=0.0;
      pos->ref_depend_map= 0;
      if (!table->pos_in_table_list->outer_join || error > 0)
	DBUG_RETURN(error);
    }
    /*
      The optimizer trust the engine that when stats.records is 0, there
      was no found rows
    */
    DBUG_ASSERT(table->file->stats.records > 0 || error);
  }
  else
  {
    if (/*!table->file->key_read && */
        table->covering_keys.is_set(tab->ref.key) && !table->no_keyread &&
        (int) table->reginfo.lock_type <= (int) TL_READ_HIGH_PRIORITY)
    {
      table->file->ha_start_keyread(tab->ref.key);
      tab->index= tab->ref.key;
    }
    error=join_read_const(tab);
    table->file->ha_end_keyread();
    if (unlikely(error))
    {
      tab->info= ET_UNIQUE_ROW_NOT_FOUND;
      /* Mark for EXPLAIN that the row was not found */
      pos->records_read=0.0;
      pos->ref_depend_map= 0;
      if (!table->pos_in_table_list->outer_join || error > 0)
	DBUG_RETURN(error);
    }
  }
  /* 
     Evaluate an on-expression only if it is not considered expensive.
     This mainly prevents executing subqueries in optimization phase.
     This is necessary since proper setup for such execution has not been
     done at this stage.
  */
  if (*tab->on_expr_ref && !table->null_row && 
      !(*tab->on_expr_ref)->is_expensive())
  {
#if !defined(DBUG_OFF) && defined(NOT_USING_ITEM_EQUAL)
    /*
      This test could be very useful to find bugs in the optimizer
      where we would call this function with an expression that can't be
      evaluated yet. We can't have this enabled by default as long as
      have items like Item_equal, that doesn't report they are const but
      they can still be called even if they contain not const items.
    */
    (*tab->on_expr_ref)->update_used_tables();
    DBUG_ASSERT((*tab->on_expr_ref)->const_item());
#endif
    if ((table->null_row= MY_TEST((*tab->on_expr_ref)->val_int() == 0)))
      mark_as_null_row(table);  
  }
  if (!table->null_row && ! tab->join->mixed_implicit_grouping)
    table->maybe_null= 0;

  {
    JOIN *join= tab->join;
    List_iterator<TABLE_LIST> ti(join->select_lex->leaf_tables);
    /* Check appearance of new constant items in Item_equal objects */
    if (join->conds)
      update_const_equal_items(thd, join->conds, tab, TRUE);
    while ((tbl= ti++))
    {
      TABLE_LIST *embedded;
      TABLE_LIST *embedding= tbl;
      do
      {
        embedded= embedding;
        if (embedded->on_expr)
           update_const_equal_items(thd, embedded->on_expr, tab, TRUE);
        embedding= embedded->embedding;
      }
      while (embedding &&
             embedding->nested_join->join_list.head() == embedded);
    }
  }
  DBUG_RETURN(0);
}


/**
  Read a constant table when there is at most one matching row, using a table
  scan.

  @param tab			Table to read

  @retval  0  Row was found
  @retval  -1 Row was not found
  @retval  1  Got an error (other than row not found) during read
*/
static int
join_read_system(JOIN_TAB *tab)
{
  TABLE *table= tab->table;
  int error;
  if (table->status & STATUS_GARBAGE)		// If first read
  {
    if (unlikely((error=
                  table->file->ha_read_first_row(table->record[0],
                                                 table->s->primary_key))))
    {
      if (error != HA_ERR_END_OF_FILE)
	return report_error(table, error);
      table->const_table= 1;
      mark_as_null_row(tab->table);
      empty_record(table);			// Make empty record
      return -1;
    }
    store_record(table,record[1]);
  }
  else if (!table->status)			// Only happens with left join
    restore_record(table,record[1]);			// restore old record
  table->null_row=0;
  return table->status ? -1 : 0;
}


/**
  Read a table when there is at most one matching row.

  @param tab			Table to read

  @retval  0  Row was found
  @retval  -1 Row was not found
  @retval  1  Got an error (other than row not found) during read
*/

static int
join_read_const(JOIN_TAB *tab)
{
  int error;
  TABLE *table= tab->table;
  if (table->status & STATUS_GARBAGE)		// If first read
  {
    table->status= 0;
    if (cp_buffer_from_ref(tab->join->thd, table, &tab->ref))
      error=HA_ERR_KEY_NOT_FOUND;
    else
    {
      error= table->file->ha_index_read_idx_map(table->record[0],tab->ref.key,
                                                (uchar*) tab->ref.key_buff,
                                                make_prev_keypart_map(tab->ref.key_parts),
                                                HA_READ_KEY_EXACT);
    }
    if (unlikely(error))
    {
      table->status= STATUS_NOT_FOUND;
      mark_as_null_row(tab->table);
      empty_record(table);
      if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
	return report_error(table, error);
      return -1;
    }
    store_record(table,record[1]);
  }
  else if (!(table->status & ~STATUS_NULL_ROW))	// Only happens with left join
  {
    table->status=0;
    restore_record(table,record[1]);			// restore old record
  }
  table->null_row=0;
  return table->status ? -1 : 0;
}

/*
  eq_ref access method implementation: "read_first" function

  SYNOPSIS
    join_read_key()
      tab  JOIN_TAB of the accessed table

  DESCRIPTION
    This is "read_fist" function for the eq_ref access method. The difference
    from ref access function is that is that it has a one-element lookup 
    cache (see cmp_buffer_with_ref)

  RETURN
    0  - Ok
   -1  - Row not found 
    1  - Error
*/


static int
join_read_key(JOIN_TAB *tab)
{
  return join_read_key2(tab->join->thd, tab, tab->table, &tab->ref);
}


/*
  eq_ref access handler but generalized a bit to support TABLE and TABLE_REF
  not from the join_tab. See join_read_key for detailed synopsis.
*/
int join_read_key2(THD *thd, JOIN_TAB *tab, TABLE *table, TABLE_REF *table_ref)
{
  int error;
  if (!table->file->inited)
  {
    error= table->file->ha_index_init(table_ref->key, tab ? tab->sorted : TRUE);
    if (unlikely(error))
    {
      (void) report_error(table, error);
      return 1;
    }
  }

  /*
    The following is needed when one makes ref (or eq_ref) access from row
    comparisons: one must call row->bring_value() to get the new values.
  */
  if (tab && tab->bush_children)
  {
    TABLE_LIST *emb_sj_nest= tab->bush_children->start->emb_sj_nest;
    emb_sj_nest->sj_subq_pred->left_expr->bring_value();
  }

  /* TODO: Why don't we do "Late NULLs Filtering" here? */

  if (cmp_buffer_with_ref(thd, table, table_ref) ||
      (table->status & (STATUS_GARBAGE | STATUS_NO_PARENT | STATUS_NULL_ROW)))
  {
    if (table_ref->key_err)
    {
      table->status=STATUS_NOT_FOUND;
      return -1;
    }
    /*
      Moving away from the current record. Unlock the row
      in the handler if it did not match the partial WHERE.
    */
    if (tab && tab->ref.has_record && tab->ref.use_count == 0)
    {
      tab->read_record.table->file->unlock_row();
      table_ref->has_record= FALSE;
    }
    error=table->file->ha_index_read_map(table->record[0],
                                  table_ref->key_buff,
                                  make_prev_keypart_map(table_ref->key_parts),
                                  HA_READ_KEY_EXACT);
    if (unlikely(error) &&
        error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      return report_error(table, error);

    if (likely(!error))
    {
      table_ref->has_record= TRUE;
      table_ref->use_count= 1;
    }
  }
  else if (table->status == 0)
  {
    DBUG_ASSERT(table_ref->has_record);
    table_ref->use_count++;
  }
  table->null_row=0;
  return table->status ? -1 : 0;
}


/**
  Since join_read_key may buffer a record, do not unlock
  it if it was not used in this invocation of join_read_key().
  Only count locks, thus remembering if the record was left unused,
  and unlock already when pruning the current value of
  TABLE_REF buffer.
  @sa join_read_key()
*/

static void
join_read_key_unlock_row(st_join_table *tab)
{
  DBUG_ASSERT(tab->ref.use_count);
  if (tab->ref.use_count)
    tab->ref.use_count--;
}

/*
  ref access method implementation: "read_first" function

  SYNOPSIS
    join_read_always_key()
      tab  JOIN_TAB of the accessed table

  DESCRIPTION
    This is "read_fist" function for the "ref" access method.
   
    The functon must leave the index initialized when it returns.
    ref_or_null access implementation depends on that.

  RETURN
    0  - Ok
   -1  - Row not found 
    1  - Error
*/

static int
join_read_always_key(JOIN_TAB *tab)
{
  int error;
  TABLE *table= tab->table;

  /* Initialize the index first */
  if (!table->file->inited)
  {
    if (unlikely((error= table->file->ha_index_init(tab->ref.key,
                                                    tab->sorted))))
    {
      (void) report_error(table, error);
      return 1;
    }
  }

  if (unlikely(cp_buffer_from_ref(tab->join->thd, table, &tab->ref)))
    return -1;
  if (unlikely((error=
                table->file->prepare_index_key_scan_map(tab->ref.key_buff,
                                                        make_prev_keypart_map(tab->ref.key_parts)))))
  {
    report_error(table,error);
    return -1;
  }
  if ((error= table->file->ha_index_read_map(table->record[0],
                                             tab->ref.key_buff,
                                             make_prev_keypart_map(tab->ref.key_parts),
                                             HA_READ_KEY_EXACT)))
  {
    if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      return report_error(table, error);
    return -1; /* purecov: inspected */
  }
  return 0;
}


/**
  This function is used when optimizing away ORDER BY in 
  SELECT * FROM t1 WHERE a=1 ORDER BY a DESC,b DESC.
*/
  
static int
join_read_last_key(JOIN_TAB *tab)
{
  int error;
  TABLE *table= tab->table;

  if (!table->file->inited &&
      unlikely((error= table->file->ha_index_init(tab->ref.key, tab->sorted))))
  {
    (void) report_error(table, error);
    return 1;
  }

  if (unlikely(cp_buffer_from_ref(tab->join->thd, table, &tab->ref)))
    return -1;
  if (unlikely((error=
                table->file->prepare_index_key_scan_map(tab->ref.key_buff,
                                                        make_prev_keypart_map(tab->ref.key_parts)))) )
  {
    report_error(table,error);
    return -1;
  }
  if (unlikely((error=
                table->file->ha_index_read_map(table->record[0],
                                               tab->ref.key_buff,
                                               make_prev_keypart_map(tab->ref.key_parts),
                                               HA_READ_PREFIX_LAST))))
  {
    if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      return report_error(table, error);
    return -1; /* purecov: inspected */
  }
  return 0;
}


	/* ARGSUSED */
static int
join_no_more_records(READ_RECORD *info __attribute__((unused)))
{
  return -1;
}


static int
join_read_next_same(READ_RECORD *info)
{
  int error;
  TABLE *table= info->table;
  JOIN_TAB *tab=table->reginfo.join_tab;

  if (unlikely((error= table->file->ha_index_next_same(table->record[0],
                                                       tab->ref.key_buff,
                                                       tab->ref.key_length))))
  {
    if (error != HA_ERR_END_OF_FILE)
      return report_error(table, error);
    table->status= STATUS_GARBAGE;
    return -1;
  }
  return 0;
}


static int
join_read_prev_same(READ_RECORD *info)
{
  int error;
  TABLE *table= info->table;
  JOIN_TAB *tab=table->reginfo.join_tab;

  if (unlikely((error= table->file->ha_index_prev(table->record[0]))))
    return report_error(table, error);
  if (key_cmp_if_same(table, tab->ref.key_buff, tab->ref.key,
                      tab->ref.key_length))
  {
    table->status=STATUS_NOT_FOUND;
    error= -1;
  }
  return error;
}


static int
join_init_quick_read_record(JOIN_TAB *tab)
{
  if (test_if_quick_select(tab) == -1)
    return -1;					/* No possible records */
  return join_init_read_record(tab);
}


int read_first_record_seq(JOIN_TAB *tab)
{
  if (unlikely(tab->read_record.table->file->ha_rnd_init_with_error(1)))
    return 1;
  return tab->read_record.read_record();
}

static int
test_if_quick_select(JOIN_TAB *tab)
{
  DBUG_EXECUTE_IF("show_explain_probe_test_if_quick_select", 
                  if (dbug_user_var_equals_int(tab->join->thd, 
                                               "show_explain_probe_select_id", 
                                               tab->join->select_lex->select_number))
                        dbug_serve_apcs(tab->join->thd, 1);
                 );


  delete tab->select->quick;
  tab->select->quick=0;

  if (tab->table->file->inited != handler::NONE)
    tab->table->file->ha_index_or_rnd_end();

  int res= tab->select->test_quick_select(tab->join->thd, tab->keys,
                                          (table_map) 0, HA_POS_ERROR, 0,
                                          FALSE, /*remove where parts*/FALSE);
  if (tab->explain_plan && tab->explain_plan->range_checked_fer)
    tab->explain_plan->range_checked_fer->collect_data(tab->select->quick);

  return res;
}


static 
bool test_if_use_dynamic_range_scan(JOIN_TAB *join_tab)
{
    return (join_tab->use_quick == 2 && test_if_quick_select(join_tab) > 0);
}

int join_init_read_record(JOIN_TAB *tab)
{
  /* 
    Note: the query plan tree for the below operations is constructed in
    save_agg_explain_data.
  */
  if (tab->distinct && tab->remove_duplicates())  // Remove duplicates.
    return 1;
  if (tab->filesort && tab->sort_table())     // Sort table.
    return 1;

  DBUG_EXECUTE_IF("kill_join_init_read_record",
                  tab->join->thd->set_killed(KILL_QUERY););
  if (tab->select && tab->select->quick && tab->select->quick->reset())
  {
    /* Ensures error status is propagated back to client */
    report_error(tab->table,
                 tab->join->thd->killed ? HA_ERR_QUERY_INTERRUPTED : HA_ERR_OUT_OF_MEM);
    return 1;
  }
  /* make sure we won't get ER_QUERY_INTERRUPTED from any code below */
  DBUG_EXECUTE_IF("kill_join_init_read_record",
                  tab->join->thd->reset_killed(););
  if (!tab->preread_init_done  && tab->preread_init())
    return 1;
  if (init_read_record(&tab->read_record, tab->join->thd, tab->table,
                       tab->select, tab->filesort_result, 1,1, FALSE))
    return 1;
  return tab->read_record.read_record();
}

int
join_read_record_no_init(JOIN_TAB *tab)
{
  Copy_field *save_copy, *save_copy_end;
  
  /*
    init_read_record resets all elements of tab->read_record().
    Remember things that we don't want to have reset.
  */
  save_copy=     tab->read_record.copy_field;
  save_copy_end= tab->read_record.copy_field_end;
  
  init_read_record(&tab->read_record, tab->join->thd, tab->table,
		   tab->select, tab->filesort_result, 1, 1, FALSE);

  tab->read_record.copy_field=     save_copy;
  tab->read_record.copy_field_end= save_copy_end;
  tab->read_record.read_record_func= rr_sequential_and_unpack;

  return tab->read_record.read_record();
}


/*
  Helper function for sorting table with filesort.
*/

bool
JOIN_TAB::sort_table()
{
  int rc;
  DBUG_PRINT("info",("Sorting for index"));
  THD_STAGE_INFO(join->thd, stage_creating_sort_index);
  DBUG_ASSERT(join->ordered_index_usage != (filesort->order == join->order ?
                                            JOIN::ordered_index_order_by :
                                            JOIN::ordered_index_group_by));
  rc= create_sort_index(join->thd, join, this, NULL);
  return (rc != 0);
}


static int
join_read_first(JOIN_TAB *tab)
{
  int error= 0;
  TABLE *table=tab->table;
  DBUG_ENTER("join_read_first");

  DBUG_ASSERT(table->no_keyread ||
              !table->covering_keys.is_set(tab->index) ||
              table->file->keyread == tab->index);
  tab->table->status=0;
  tab->read_record.read_record_func= join_read_next;
  tab->read_record.table=table;
  if (!table->file->inited)
    error= table->file->ha_index_init(tab->index, tab->sorted);
  if (likely(!error))
    error= table->file->prepare_index_scan();
  if (unlikely(error) ||
      unlikely(error= tab->table->file->ha_index_first(tab->table->record[0])))
  {
    if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      report_error(table, error);
    DBUG_RETURN(-1);
  }
  DBUG_RETURN(0);
}


static int
join_read_next(READ_RECORD *info)
{
  int error;
  if (unlikely((error= info->table->file->ha_index_next(info->record()))))
    return report_error(info->table, error);

  return 0;
}


static int
join_read_last(JOIN_TAB *tab)
{
  TABLE *table=tab->table;
  int error= 0;
  DBUG_ENTER("join_read_last");

  DBUG_ASSERT(table->no_keyread ||
              !table->covering_keys.is_set(tab->index) ||
              table->file->keyread == tab->index);
  tab->table->status=0;
  tab->read_record.read_record_func= join_read_prev;
  tab->read_record.table=table;
  if (!table->file->inited)
    error= table->file->ha_index_init(tab->index, 1);
  if (likely(!error))
    error= table->file->prepare_index_scan();
  if (unlikely(error) ||
      unlikely(error= tab->table->file->ha_index_last(tab->table->record[0])))
    DBUG_RETURN(report_error(table, error));

  DBUG_RETURN(0);
}


static int
join_read_prev(READ_RECORD *info)
{
  int error;
  if (unlikely((error= info->table->file->ha_index_prev(info->record()))))
    return report_error(info->table, error);
  return 0;
}


static int
join_ft_read_first(JOIN_TAB *tab)
{
  int error;
  TABLE *table= tab->table;

  if (!table->file->inited &&
      (error= table->file->ha_index_init(tab->ref.key, 1)))
  {
    (void) report_error(table, error);
    return 1;
  }

  table->file->ft_init();

  if (unlikely((error= table->file->ha_ft_read(table->record[0]))))
    return report_error(table, error);
  return 0;
}

static int
join_ft_read_next(READ_RECORD *info)
{
  int error;
  if (unlikely((error= info->table->file->ha_ft_read(info->record()))))
    return report_error(info->table, error);
  return 0;
}


/**
  Reading of key with key reference and one part that may be NULL.
*/

int
join_read_always_key_or_null(JOIN_TAB *tab)
{
  int res;

  /* First read according to key which is NOT NULL */
  *tab->ref.null_ref_key= 0;			// Clear null byte
  if ((res= join_read_always_key(tab)) >= 0)
    return res;

  /* Then read key with null value */
  *tab->ref.null_ref_key= 1;			// Set null byte
  return safe_index_read(tab);
}


int
join_read_next_same_or_null(READ_RECORD *info)
{
  int error;
  if (unlikely((error= join_read_next_same(info)) >= 0))
    return error;
  JOIN_TAB *tab= info->table->reginfo.join_tab;

  /* Test if we have already done a read after null key */
  if (*tab->ref.null_ref_key)
    return -1;					// All keys read
  *tab->ref.null_ref_key= 1;			// Set null byte
  return safe_index_read(tab);			// then read null keys
}


/*****************************************************************************
  DESCRIPTION
    Functions that end one nested loop iteration. Different functions
    are used to support GROUP BY clause and to redirect records
    to a table (e.g. in case of SELECT into a temporary table) or to the
    network client.

  RETURN VALUES
    NESTED_LOOP_OK           - the record has been successfully handled
    NESTED_LOOP_ERROR        - a fatal error (like table corruption)
                               was detected
    NESTED_LOOP_KILLED       - thread shutdown was requested while processing
                               the record
    NESTED_LOOP_QUERY_LIMIT  - the record has been successfully handled;
                               additionally, the nested loop produced the
                               number of rows specified in the LIMIT clause
                               for the query
    NESTED_LOOP_CURSOR_LIMIT - the record has been successfully handled;
                               additionally, there is a cursor and the nested
                               loop algorithm produced the number of rows
                               that is specified for current cursor fetch
                               operation.
   All return values except NESTED_LOOP_OK abort the nested loop.
*****************************************************************************/

/* ARGSUSED */
static enum_nested_loop_state
end_send(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
	 bool end_of_records)
{
  DBUG_ENTER("end_send");
  /*
    When all tables are const this function is called with jointab == NULL.
    This function shouldn't be called for the first join_tab as it needs
    to get fields from previous tab.
  */
  DBUG_ASSERT(join_tab == NULL || join_tab != join->join_tab);
  //TODO pass fields via argument
  List<Item> *fields= join_tab ? (join_tab-1)->fields : join->fields;

  if (!end_of_records)
  {
    if (join->table_count &&
        join->join_tab->is_using_loose_index_scan())
    {
      /* Copy non-aggregated fields when loose index scan is used. */
      copy_fields(&join->tmp_table_param);
    }
    if (join->having && join->having->val_int() == 0)
      DBUG_RETURN(NESTED_LOOP_OK);               // Didn't match having
    if (join->procedure)
    {
      if (join->procedure->send_row(join->procedure_fields_list))
        DBUG_RETURN(NESTED_LOOP_ERROR);
      DBUG_RETURN(NESTED_LOOP_OK);
    }
    if (join->do_send_rows)
    {
      int error;
      /* result < 0 if row was not accepted and should not be counted */
      if (unlikely((error= join->result->send_data(*fields))))
      {
        if (error > 0)
          DBUG_RETURN(NESTED_LOOP_ERROR);
        // error < 0 => duplicate row
        join->duplicate_rows++;
      }
    }

    ++join->send_records;
    if (join->send_records >= join->unit->select_limit_cnt &&
        !join->do_send_rows)
    {
      /*
        If we have used Priority Queue for optimizing order by with limit,
        then stop here, there are no more records to consume.
        When this optimization is used, end_send is called on the next
        join_tab.
      */
      if (join->order &&
          join->select_options & OPTION_FOUND_ROWS &&
          join_tab > join->join_tab &&
          (join_tab - 1)->filesort && (join_tab - 1)->filesort->using_pq)
      {
        DBUG_PRINT("info", ("filesort NESTED_LOOP_QUERY_LIMIT"));
        DBUG_RETURN(NESTED_LOOP_QUERY_LIMIT);
      }
    }
    if (join->send_records >= join->unit->select_limit_cnt &&
	join->do_send_rows)
    {
      if (join->select_options & OPTION_FOUND_ROWS)
      {
	JOIN_TAB *jt=join->join_tab;
	if ((join->table_count == 1) && !join->sort_and_group
	    && !join->send_group_parts && !join->having && !jt->select_cond &&
	    !(jt->select && jt->select->quick) &&
	    (jt->table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT) &&
            (jt->ref.key < 0))
	{
	  /* Join over all rows in table;  Return number of found rows */
	  TABLE *table=jt->table;

	  if (jt->filesort_result)                     // If filesort was used
	  {
	    join->send_records= jt->filesort_result->found_rows;
	  }
	  else
	  {
	    table->file->info(HA_STATUS_VARIABLE);
	    join->send_records= table->file->stats.records;
	  }
	}
	else 
	{
	  join->do_send_rows= 0;
	  if (join->unit->fake_select_lex)
	    join->unit->fake_select_lex->select_limit= 0;
	  DBUG_RETURN(NESTED_LOOP_OK);
	}
      }
      DBUG_RETURN(NESTED_LOOP_QUERY_LIMIT);      // Abort nicely
    }
    else if (join->send_records >= join->fetch_limit)
    {
      /*
        There is a server side cursor and all rows for
        this fetch request are sent.
      */
      DBUG_RETURN(NESTED_LOOP_CURSOR_LIMIT);
    }
  }
  else
  {
    if (join->procedure && join->procedure->end_of_records())
      DBUG_RETURN(NESTED_LOOP_ERROR);
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/*
  @brief
    Perform a GROUP BY operation over a stream of rows ordered by their group. The 
    result is sent into join->result.

  @detail
    Also applies HAVING, etc.
*/

enum_nested_loop_state
end_send_group(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
	       bool end_of_records)
{
  int idx= -1;
  enum_nested_loop_state ok_code= NESTED_LOOP_OK;
  List<Item> *fields= join_tab ? (join_tab-1)->fields : join->fields;
  DBUG_ENTER("end_send_group");

  if (!join->items3.is_null() && !join->set_group_rpa)
  {
    join->set_group_rpa= true;
    join->set_items_ref_array(join->items3);
  }

  if (!join->first_record || end_of_records ||
      (idx=test_if_group_changed(join->group_fields)) >= 0)
  {
    if (!join->group_sent &&
        (join->first_record ||
         (end_of_records && !join->group && !join->group_optimized_away)))
    {
      if (join->procedure)
	join->procedure->end_group();
      if (idx < (int) join->send_group_parts)
      {
	int error=0;
	if (join->procedure)
	{
	  if (join->having && join->having->val_int() == 0)
	    error= -1;				// Didn't satisfy having
 	  else
	  {
	    if (join->do_send_rows)
	      error=join->procedure->send_row(*fields) ? 1 : 0;
	    join->send_records++;
	  }
	  if (end_of_records && join->procedure->end_of_records())
	    error= 1;				// Fatal error
	}
	else
	{
	  if (!join->first_record)
	  {
            List_iterator_fast<Item> it(*join->fields);
            Item *item;
            /* No matching rows for group function */
            join->clear();

            while ((item= it++))
              item->no_rows_in_result();
	  }
	  if (join->having && join->having->val_int() == 0)
	    error= -1;				// Didn't satisfy having
	  else
	  {
	    if (join->do_send_rows)
            {
	      error=join->result->send_data(*fields);
              if (unlikely(error < 0))
              {
                /* Duplicate row, don't count */
                join->duplicate_rows++;
                error= 0;
              }
            }
	    join->send_records++;
            join->group_sent= true;
	  }
	  if (unlikely(join->rollup.state != ROLLUP::STATE_NONE && error <= 0))
	  {
	    if (join->rollup_send_data((uint) (idx+1)))
	      error= 1;
	  }
	}
	if (unlikely(error > 0))
          DBUG_RETURN(NESTED_LOOP_ERROR);        /* purecov: inspected */
	if (end_of_records)
	  DBUG_RETURN(NESTED_LOOP_OK);
	if (join->send_records >= join->unit->select_limit_cnt &&
	    join->do_send_rows)
	{
	  if (!(join->select_options & OPTION_FOUND_ROWS))
	    DBUG_RETURN(NESTED_LOOP_QUERY_LIMIT); // Abort nicely
	  join->do_send_rows=0;
	  join->unit->select_limit_cnt = HA_POS_ERROR;
        }
        else if (join->send_records >= join->fetch_limit)
        {
          /*
            There is a server side cursor and all rows
            for this fetch request are sent.
          */
          /*
            Preventing code duplication. When finished with the group reset
            the group functions and copy_fields. We fall through. bug #11904
          */
          ok_code= NESTED_LOOP_CURSOR_LIMIT;
        }
      }
    }
    else
    {
      if (end_of_records)
	DBUG_RETURN(NESTED_LOOP_OK);
      join->first_record=1;
      (void) test_if_group_changed(join->group_fields);
    }
    if (idx < (int) join->send_group_parts)
    {
      /*
        This branch is executed also for cursors which have finished their
        fetch limit - the reason for ok_code.
      */
      copy_fields(&join->tmp_table_param);
      if (init_sum_functions(join->sum_funcs, join->sum_funcs_end[idx+1]))
	DBUG_RETURN(NESTED_LOOP_ERROR);
      if (join->procedure)
	join->procedure->add();
      join->group_sent= false;
      DBUG_RETURN(ok_code);
    }
  }
  if (update_sum_func(join->sum_funcs))
    DBUG_RETURN(NESTED_LOOP_ERROR);
  if (join->procedure)
    join->procedure->add();
  DBUG_RETURN(NESTED_LOOP_OK);
}


	/* ARGSUSED */
static enum_nested_loop_state
end_write(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
	  bool end_of_records)
{
  TABLE *const table= join_tab->table;
  DBUG_ENTER("end_write");

  if (!end_of_records)
  {
    copy_fields(join_tab->tmp_table_param);
    if (copy_funcs(join_tab->tmp_table_param->items_to_copy, join->thd))
      DBUG_RETURN(NESTED_LOOP_ERROR);           /* purecov: inspected */

    if (likely(!join_tab->having || join_tab->having->val_int()))
    {
      int error;
      join->found_records++;
      if ((error= table->file->ha_write_tmp_row(table->record[0])))
      {
        if (likely(!table->file->is_fatal_error(error, HA_CHECK_DUP)))
	  goto end;                             // Ignore duplicate keys
        bool is_duplicate;
	if (create_internal_tmp_table_from_heap(join->thd, table, 
                                                join_tab->tmp_table_param->start_recinfo,
                                                &join_tab->tmp_table_param->recinfo,
                                                error, 1, &is_duplicate))
	  DBUG_RETURN(NESTED_LOOP_ERROR);        // Not a table_is_full error
        if (is_duplicate)
          goto end;
	table->s->uniques=0;			// To ensure rows are the same
      }
      if (++join_tab->send_records >=
            join_tab->tmp_table_param->end_write_records &&
	  join->do_send_rows)
      {
	if (!(join->select_options & OPTION_FOUND_ROWS))
	  DBUG_RETURN(NESTED_LOOP_QUERY_LIMIT);
	join->do_send_rows=0;
	join->unit->select_limit_cnt = HA_POS_ERROR;
      }
    }
  }
end:
  if (unlikely(join->thd->check_killed()))
  {
    DBUG_RETURN(NESTED_LOOP_KILLED);             /* purecov: inspected */
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/*
  @brief
    Perform a GROUP BY operation over rows coming in arbitrary order. 
    
    This is done by looking up the group in a temp.table and updating group
    values.

  @detail
    Also applies HAVING, etc.
*/

static enum_nested_loop_state
end_update(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
	   bool end_of_records)
{
  TABLE *const table= join_tab->table;
  ORDER   *group;
  int	  error;
  DBUG_ENTER("end_update");

  if (end_of_records)
    DBUG_RETURN(NESTED_LOOP_OK);

  join->found_records++;
  copy_fields(join_tab->tmp_table_param);	// Groups are copied twice.
  /* Make a key of group index */
  for (group=table->group ; group ; group=group->next)
  {
    Item *item= *group->item;
    if (group->fast_field_copier_setup != group->field)
    {
      DBUG_PRINT("info", ("new setup %p -> %p",
                          group->fast_field_copier_setup,
                          group->field));
      group->fast_field_copier_setup= group->field;
      group->fast_field_copier_func=
        item->setup_fast_field_copier(group->field);
    }
    item->save_org_in_field(group->field, group->fast_field_copier_func);
    /* Store in the used key if the field was 0 */
    if (item->maybe_null)
      group->buff[-1]= (char) group->field->is_null();
  }
  if (!table->file->ha_index_read_map(table->record[1],
                                      join_tab->tmp_table_param->group_buff,
                                      HA_WHOLE_KEY,
                                      HA_READ_KEY_EXACT))
  {						/* Update old record */
    restore_record(table,record[1]);
    update_tmptable_sum_func(join->sum_funcs,table);
    if (unlikely((error= table->file->ha_update_tmp_row(table->record[1],
                                                        table->record[0]))))
    {
      table->file->print_error(error,MYF(0));	/* purecov: inspected */
      DBUG_RETURN(NESTED_LOOP_ERROR);            /* purecov: inspected */
    }
    goto end;
  }

  init_tmptable_sum_functions(join->sum_funcs);
  if (unlikely(copy_funcs(join_tab->tmp_table_param->items_to_copy,
                          join->thd)))
    DBUG_RETURN(NESTED_LOOP_ERROR);           /* purecov: inspected */
  if (unlikely((error= table->file->ha_write_tmp_row(table->record[0]))))
  {
    if (create_internal_tmp_table_from_heap(join->thd, table,
                                       join_tab->tmp_table_param->start_recinfo,
                                            &join_tab->tmp_table_param->recinfo,
                                            error, 0, NULL))
      DBUG_RETURN(NESTED_LOOP_ERROR);            // Not a table_is_full error
    /* Change method to update rows */
    if (unlikely((error= table->file->ha_index_init(0, 0))))
    {
      table->file->print_error(error, MYF(0));
      DBUG_RETURN(NESTED_LOOP_ERROR);
    }

    join_tab->aggr->set_write_func(end_unique_update);
  }
  join_tab->send_records++;
end:
  if (unlikely(join->thd->check_killed()))
  {
    DBUG_RETURN(NESTED_LOOP_KILLED);             /* purecov: inspected */
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/** Like end_update, but this is done with unique constraints instead of keys.  */

static enum_nested_loop_state
end_unique_update(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
		  bool end_of_records)
{
  TABLE *table= join_tab->table;
  int	  error;
  DBUG_ENTER("end_unique_update");

  if (end_of_records)
    DBUG_RETURN(NESTED_LOOP_OK);

  init_tmptable_sum_functions(join->sum_funcs);
  copy_fields(join_tab->tmp_table_param);		// Groups are copied twice.
  if (copy_funcs(join_tab->tmp_table_param->items_to_copy, join->thd))
    DBUG_RETURN(NESTED_LOOP_ERROR);           /* purecov: inspected */

  if (likely(!(error= table->file->ha_write_tmp_row(table->record[0]))))
    join_tab->send_records++;			// New group
  else
  {
    if (unlikely((int) table->file->get_dup_key(error) < 0))
    {
      table->file->print_error(error,MYF(0));	/* purecov: inspected */
      DBUG_RETURN(NESTED_LOOP_ERROR);            /* purecov: inspected */
    }
    /* Prepare table for random positioning */
    bool rnd_inited= (table->file->inited == handler::RND);
    if (!rnd_inited &&
        ((error= table->file->ha_index_end()) ||
         (error= table->file->ha_rnd_init(0))))
    {
      table->file->print_error(error, MYF(0));
      DBUG_RETURN(NESTED_LOOP_ERROR);
    }
    if (unlikely(table->file->ha_rnd_pos(table->record[1],table->file->dup_ref)))
    {
      table->file->print_error(error,MYF(0));	/* purecov: inspected */
      DBUG_RETURN(NESTED_LOOP_ERROR);            /* purecov: inspected */
    }
    restore_record(table,record[1]);
    update_tmptable_sum_func(join->sum_funcs,table);
    if (unlikely((error= table->file->ha_update_tmp_row(table->record[1],
                                                        table->record[0]))))
    {
      table->file->print_error(error,MYF(0));	/* purecov: inspected */
      DBUG_RETURN(NESTED_LOOP_ERROR);            /* purecov: inspected */
    }
    if (!rnd_inited &&
        ((error= table->file->ha_rnd_end()) ||
         (error= table->file->ha_index_init(0, 0))))
    {
      table->file->print_error(error, MYF(0));
      DBUG_RETURN(NESTED_LOOP_ERROR);
    }
  }
  if (unlikely(join->thd->check_killed()))
  {
    DBUG_RETURN(NESTED_LOOP_KILLED);             /* purecov: inspected */
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/*
  @brief
    Perform a GROUP BY operation over a stream of rows ordered by their group.
    Write the result into a temporary table.

  @detail
    Also applies HAVING, etc.

    The rows are written into temptable so e.g. filesort can read them.
*/

enum_nested_loop_state
end_write_group(JOIN *join, JOIN_TAB *join_tab __attribute__((unused)),
		bool end_of_records)
{
  TABLE *table= join_tab->table;
  int	  idx= -1;
  DBUG_ENTER("end_write_group");

  if (!join->first_record || end_of_records ||
      (idx=test_if_group_changed(join->group_fields)) >= 0)
  {
    if (join->first_record || (end_of_records && !join->group))
    {
      if (join->procedure)
	join->procedure->end_group();
      int send_group_parts= join->send_group_parts;
      if (idx < send_group_parts)
      {
        if (!join->first_record)
        {
          /* No matching rows for group function */
          join->clear();
        }
        copy_sum_funcs(join->sum_funcs,
                       join->sum_funcs_end[send_group_parts]);
	if (!join_tab->having || join_tab->having->val_int())
	{
          int error= table->file->ha_write_tmp_row(table->record[0]);
          if (unlikely(error) &&
              create_internal_tmp_table_from_heap(join->thd, table,
                                          join_tab->tmp_table_param->start_recinfo,
                                          &join_tab->tmp_table_param->recinfo,
                                                   error, 0, NULL))
	    DBUG_RETURN(NESTED_LOOP_ERROR);
        }
        if (unlikely(join->rollup.state != ROLLUP::STATE_NONE))
	{
          if (unlikely(join->rollup_write_data((uint) (idx+1),
                                               join_tab->tmp_table_param,
                                               table)))
          {
	    DBUG_RETURN(NESTED_LOOP_ERROR);
          }
	}
	if (end_of_records)
	  goto end;
      }
    }
    else
    {
      if (end_of_records)
        goto end;
      join->first_record=1;
      (void) test_if_group_changed(join->group_fields);
    }
    if (idx < (int) join->send_group_parts)
    {
      copy_fields(join_tab->tmp_table_param);
      if (unlikely(copy_funcs(join_tab->tmp_table_param->items_to_copy,
                              join->thd)))
	DBUG_RETURN(NESTED_LOOP_ERROR);
      if (unlikely(init_sum_functions(join->sum_funcs,
                                      join->sum_funcs_end[idx+1])))
	DBUG_RETURN(NESTED_LOOP_ERROR);
      if (unlikely(join->procedure))
	join->procedure->add();
      goto end;
    }
  }
  if (unlikely(update_sum_func(join->sum_funcs)))
    DBUG_RETURN(NESTED_LOOP_ERROR);
  if (unlikely(join->procedure))
    join->procedure->add();
end:
  if (unlikely(join->thd->check_killed()))
  {
    DBUG_RETURN(NESTED_LOOP_KILLED);             /* purecov: inspected */
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/*****************************************************************************
  Remove calculation with tables that aren't yet read. Remove also tests
  against fields that are read through key where the table is not a
  outer join table.
  We can't remove tests that are made against columns which are stored
  in sorted order.
*****************************************************************************/

/**
  Check if "left_item=right_item" equality is guaranteed to be true by use of
  [eq]ref access on left_item->field->table.

  SYNOPSIS
    test_if_ref()
      root_cond
      left_item
      right_item

  DESCRIPTION
    Check if the given "left_item = right_item" equality is guaranteed to be
    true by use of [eq_]ref access method.

    We need root_cond as we can't remove ON expressions even if employed ref 
    access guarantees that they are true. This is because  TODO

  RETURN
    TRUE   if right_item is used removable reference key on left_item
    FALSE  Otherwise
    
*/

bool test_if_ref(Item *root_cond, Item_field *left_item,Item *right_item)
{
  Field *field=left_item->field;
  JOIN_TAB *join_tab= field->table->reginfo.join_tab;
  // No need to change const test
  if (!field->table->const_table && join_tab &&
      !join_tab->is_ref_for_hash_join() &&
      (!join_tab->first_inner ||
       *join_tab->first_inner->on_expr_ref == root_cond))
  {
    /*
      If ref access uses "Full scan on NULL key" (i.e. it actually alternates
      between ref access and full table scan), then no equality can be
      guaranteed to be true.
    */
    if (join_tab->ref.is_access_triggered())
      return FALSE;

    Item *ref_item=part_of_refkey(field->table,field);
    if (ref_item && (ref_item->eq(right_item,1) || 
		     ref_item->real_item()->eq(right_item,1)))
    {
      right_item= right_item->real_item();
      if (right_item->type() == Item::FIELD_ITEM)
	return (field->eq_def(((Item_field *) right_item)->field));
      /* remove equalities injected by IN->EXISTS transformation */
      else if (right_item->type() == Item::CACHE_ITEM)
        return ((Item_cache *)right_item)->eq_def (field);
      if (right_item->const_item() && !(right_item->is_null()))
      {
	/*
	  We can remove binary fields and numerical fields except float,
	  as float comparison isn't 100 % safe
	  We have to keep normal strings to be able to check for end spaces
	*/
	if (field->binary() &&
	    field->real_type() != MYSQL_TYPE_STRING &&
	    field->real_type() != MYSQL_TYPE_VARCHAR &&
	    (field->type() != MYSQL_TYPE_FLOAT || field->decimals() == 0))
	{
	  return !right_item->save_in_field_no_warnings(field, 1);
	}
      }
    }
  }
  return 0;					// keep test
}


/**
   Extract a condition that can be checked after reading given table
   @fn make_cond_for_table()

   @param cond       Condition to analyze
   @param tables     Tables for which "current field values" are available
   @param used_table Table that we're extracting the condition for
      tables       Tables for which "current field values" are available (this
                   includes used_table)
                   (may  also include PSEUDO_TABLE_BITS, and may be zero)
   @param join_tab_idx_arg
		     The index of the JOIN_TAB this Item is being extracted
                     for. MAX_TABLES if there is no corresponding JOIN_TAB.
   @param exclude_expensive_cond
		     Do not push expensive conditions
   @param retain_ref_cond
                     Retain ref conditions

   @retval <>NULL Generated condition
   @retval =NULL  Already checked, OR error

   @details
     Extract the condition that can be checked after reading the table
     specified in 'used_table', given that current-field values for tables
     specified in 'tables' bitmap are available.
     If 'used_table' is 0
     - extract conditions for all tables in 'tables'.
     - extract conditions are unrelated to any tables
       in the same query block/level(i.e. conditions
       which have used_tables == 0).

     The function assumes that
     - Constant parts of the condition has already been checked.
     - Condition that could be checked for tables in 'tables' has already
     been checked.

     The function takes into account that some parts of the condition are
     guaranteed to be true by employed 'ref' access methods (the code that
     does this is located at the end, search down for "EQ_FUNC").

   @note
     Make sure to keep the implementations of make_cond_for_table() and
     make_cond_after_sjm() synchronized.
     make_cond_for_info_schema() uses similar algorithm as well.
*/ 

static Item *
make_cond_for_table(THD *thd, Item *cond, table_map tables,
                    table_map used_table,
                    int join_tab_idx_arg,
                    bool exclude_expensive_cond __attribute__((unused)),
		    bool retain_ref_cond)
{
  return make_cond_for_table_from_pred(thd, cond, cond, tables, used_table,
                                       join_tab_idx_arg,
                                       exclude_expensive_cond,
                                       retain_ref_cond, true);
}


static Item *
make_cond_for_table_from_pred(THD *thd, Item *root_cond, Item *cond,
                              table_map tables, table_map used_table,
                              int join_tab_idx_arg,
                              bool exclude_expensive_cond __attribute__
                              ((unused)),
                              bool retain_ref_cond,
                              bool is_top_and_level)

{
  table_map rand_table_bit= (table_map) RAND_TABLE_BIT;

  if (used_table && !(cond->used_tables() & used_table))
    return (COND*) 0;				// Already checked

  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      /* Create new top level AND item */
      Item_cond_and *new_cond=new (thd->mem_root) Item_cond_and(thd);
      if (!new_cond)
	return (COND*) 0;			// OOM /* purecov: inspected */
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
        /*
          Special handling of top level conjuncts with RAND_TABLE_BIT:
          if such a conjunct contains a reference to a field that is not
          an outer field then it is pushed to the corresponding table by
          the same rule as all other conjuncts. Otherwise, if the conjunct
          is used in WHERE is is pushed to the last joined table, if is it
          is used in ON condition of an outer join it is pushed into the
          last inner table of the outer join. Such conjuncts are pushed in
          a call of make_cond_for_table_from_pred() with the
          parameter 'used_table' equal to PSEUDO_TABLE_BITS.
        */
        if (is_top_and_level && used_table == rand_table_bit &&
            (item->used_tables() & ~OUTER_REF_TABLE_BIT) != rand_table_bit)
        {
          /* The conjunct with RAND_TABLE_BIT has been allready pushed */
          continue;
        }
	Item *fix=make_cond_for_table_from_pred(thd, root_cond, item, 
                                                tables, used_table,
                                                join_tab_idx_arg,
                                                exclude_expensive_cond,
                                                retain_ref_cond, false);
	if (fix)
	  new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      switch (new_cond->argument_list()->elements) {
      case 0:
	return (COND*) 0;			// Always true
      case 1:
	return new_cond->argument_list()->head();
      default:
	/*
          Call fix_fields to propagate all properties of the children to
          the new parent Item. This should not be expensive because all
	  children of Item_cond_and should be fixed by now.
	*/
	if (new_cond->fix_fields(thd, 0))
          return (COND*) 0;
	new_cond->used_tables_cache=
	  ((Item_cond_and*) cond)->used_tables_cache &
	  tables;
	return new_cond;
      }
    }
    else
    {						// Or list
      if (is_top_and_level && used_table == rand_table_bit &&
          (cond->used_tables() & ~OUTER_REF_TABLE_BIT) != rand_table_bit)
      {
        /* This top level formula with RAND_TABLE_BIT has been already pushed */
        return (COND*) 0;
      }

      Item_cond_or *new_cond=new (thd->mem_root) Item_cond_or(thd);
      if (!new_cond)
	return (COND*) 0;			// OOM /* purecov: inspected */
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
	Item *fix=make_cond_for_table_from_pred(thd, root_cond, item,
                                                tables, 0L,
                                                join_tab_idx_arg,
                                                exclude_expensive_cond,
                                                retain_ref_cond, false);
	if (!fix)
	  return (COND*) 0;			// Always true
	new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      /*
        Call fix_fields to propagate all properties of the children to
        the new parent Item. This should not be expensive because all
        children of Item_cond_and should be fixed by now.
      */
      new_cond->fix_fields(thd, 0);
      new_cond->used_tables_cache= ((Item_cond_or*) cond)->used_tables_cache;
      new_cond->top_level_item();
      return new_cond;
    }
  }

  if (is_top_and_level && used_table == rand_table_bit &&
      (cond->used_tables() & ~OUTER_REF_TABLE_BIT) != rand_table_bit)
  {
    /* This top level formula with RAND_TABLE_BIT has been already pushed */
    return (COND*) 0;
  }

  /*
    Because the following test takes a while and it can be done
    table_count times, we mark each item that we have examined with the result
    of the test
  */
  if ((cond->marker == 3 && !retain_ref_cond) ||
      (cond->used_tables() & ~tables))
    return (COND*) 0;				// Can't check this yet

  if (cond->marker == 2 || cond->eq_cmp_result() == Item::COND_OK)
  {
    cond->set_join_tab_idx(join_tab_idx_arg);
    return cond;				// Not boolean op
  }

  if (cond->type() == Item::FUNC_ITEM && 
      ((Item_func*) cond)->functype() == Item_func::EQ_FUNC)
  {
    Item *left_item=	((Item_func*) cond)->arguments()[0]->real_item();
    Item *right_item= ((Item_func*) cond)->arguments()[1]->real_item();
    if (left_item->type() == Item::FIELD_ITEM && !retain_ref_cond &&
	test_if_ref(root_cond, (Item_field*) left_item,right_item))
    {
      cond->marker=3;			// Checked when read
      return (COND*) 0;
    }
    if (right_item->type() == Item::FIELD_ITEM && !retain_ref_cond &&
	test_if_ref(root_cond, (Item_field*) right_item,left_item))
    {
      cond->marker=3;			// Checked when read
      return (COND*) 0;
    }
    /*
      If cond is an equality injected for split optimization then
      a. when retain_ref_cond == false : cond is removed unconditionally
         (cond that supports ref access is removed by the preceding code)
      b. when retain_ref_cond == true : cond is removed if it does not
         support ref access
    */
    if (left_item->type() == Item::FIELD_ITEM &&
        is_eq_cond_injected_for_split_opt((Item_func_eq *) cond) &&
        (!retain_ref_cond ||
         !test_if_ref(root_cond, (Item_field*) left_item,right_item)))
    {
      cond->marker=3;
      return (COND*) 0;
    }
  }
  cond->marker=2;
  cond->set_join_tab_idx(join_tab_idx_arg);
  return cond;
}


/*
  The difference of this from make_cond_for_table() is that we're in the
  following state:
    1. conditions referring to 'tables' have been checked
    2. conditions referring to sjm_tables have been checked, too
    3. We need condition that couldn't be checked in #1 or #2 but 
       can be checked when we get both (tables | sjm_tables).

*/
static COND *
make_cond_after_sjm(THD *thd, Item *root_cond, Item *cond, table_map tables,
                    table_map sjm_tables, bool inside_or_clause)
{
  /*
    We assume that conditions that refer to only join prefix tables or 
    sjm_tables have already been checked.
  */
  if (!inside_or_clause)
  {
    table_map cond_used_tables= cond->used_tables();
    if((!(cond_used_tables & ~tables) ||
       !(cond_used_tables & ~sjm_tables)))
      return (COND*) 0;				// Already checked
  }

  /* AND/OR recursive descent */
  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      /* Create new top level AND item */
      Item_cond_and *new_cond= new (thd->mem_root) Item_cond_and(thd);
      if (!new_cond)
	return (COND*) 0;			// OOM /* purecov: inspected */
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
        Item *fix=make_cond_after_sjm(thd, root_cond, item, tables, sjm_tables,
                                      inside_or_clause);
	if (fix)
	  new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      switch (new_cond->argument_list()->elements) {
      case 0:
	return (COND*) 0;			// Always true
      case 1:
	return new_cond->argument_list()->head();
      default:
	/*
	  Item_cond_and do not need fix_fields for execution, its parameters
	  are fixed or do not need fix_fields, too
	*/
	new_cond->quick_fix_field();
	new_cond->used_tables_cache=
	  ((Item_cond_and*) cond)->used_tables_cache &
	  tables;
	return new_cond;
      }
    }
    else
    {						// Or list
      Item_cond_or *new_cond= new (thd->mem_root) Item_cond_or(thd);
      if (!new_cond)
	return (COND*) 0;			// OOM /* purecov: inspected */
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item=li++))
      {
        Item *fix= make_cond_after_sjm(thd, root_cond, item, tables, sjm_tables,
                                       /*inside_or_clause= */TRUE);
	if (!fix)
	  return (COND*) 0;			// Always true
	new_cond->argument_list()->push_back(fix, thd->mem_root);
      }
      /*
	Item_cond_or do not need fix_fields for execution, its parameters
	are fixed or do not need fix_fields, too
      */
      new_cond->quick_fix_field();
      new_cond->used_tables_cache= ((Item_cond_or*) cond)->used_tables_cache;
      new_cond->top_level_item();
      return new_cond;
    }
  }

  /*
    Because the following test takes a while and it can be done
    table_count times, we mark each item that we have examined with the result
    of the test
  */

  if (cond->marker == 3 || (cond->used_tables() & ~(tables | sjm_tables)))
    return (COND*) 0;				// Can't check this yet
  if (cond->marker == 2 || cond->eq_cmp_result() == Item::COND_OK)
    return cond;				// Not boolean op

  /* 
    Remove equalities that are guaranteed to be true by use of 'ref' access
    method
  */
  if (((Item_func*) cond)->functype() == Item_func::EQ_FUNC)
  {
    Item *left_item= ((Item_func*) cond)->arguments()[0]->real_item();
    Item *right_item= ((Item_func*) cond)->arguments()[1]->real_item();
    if (left_item->type() == Item::FIELD_ITEM &&
	test_if_ref(root_cond, (Item_field*) left_item,right_item))
    {
      cond->marker=3;			// Checked when read
      return (COND*) 0;
    }
    if (right_item->type() == Item::FIELD_ITEM &&
	test_if_ref(root_cond, (Item_field*) right_item,left_item))
    {
      cond->marker=3;			// Checked when read
      return (COND*) 0;
    }
  }
  cond->marker=2;
  return cond;
}


/*
  @brief

  Check if
   - @table uses "ref"-like access 
   - it is based on "@field=certain_item" equality
   - the equality will be true for any record returned by the access method
  and return the certain_item if yes.
  
  @detail
  
  Equality won't necessarily hold if:
   - the used index covers only part of the @field. 
     Suppose, we have a CHAR(5) field and INDEX(field(3)). if you make a lookup
     for 'abc', you will get both record with 'abc' and with 'abcde'.
   - The type of access is actually ref_or_null, and so @field can be either 
     a value or NULL.

  @return 
    Item that the field will be equal to
    NULL if no such item 
*/

static Item *
part_of_refkey(TABLE *table,Field *field)
{
  JOIN_TAB *join_tab= table->reginfo.join_tab;
  if (!join_tab)
    return (Item*) 0;             // field from outer non-select (UPDATE,...)

  uint ref_parts= join_tab->ref.key_parts;
  if (ref_parts) /* if it's ref/eq_ref/ref_or_null */
  {
    uint key= join_tab->ref.key;
    KEY *key_info= join_tab->get_keyinfo_by_key_no(key);
    KEY_PART_INFO *key_part= key_info->key_part;

    for (uint part=0 ; part < ref_parts ; part++,key_part++)
    {
      if (field->eq(key_part->field))
      {
        /*
          Found the field in the key. Check that 
           1. ref_or_null doesn't alternate this component between a value and
              a NULL
           2. index fully covers the key
        */
        if (part != join_tab->ref.null_ref_part &&            // (1)
            !(key_part->key_part_flag & HA_PART_KEY_SEG))     // (2)
        {
          return join_tab->ref.items[part];
        }
        break;
      }
    }
  }
  return (Item*) 0;
}


/**
  Test if one can use the key to resolve ORDER BY.

  @param join                  if not NULL, can use the join's top-level
                               multiple-equalities.
  @param order                 Sort order
  @param table                 Table to sort
  @param idx                   Index to check
  @param used_key_parts [out]  NULL by default, otherwise return value for
                               used key parts.


  @note
    used_key_parts is set to correct key parts used if return value != 0
    (On other cases, used_key_part may be changed)
    Note that the value may actually be greater than the number of index 
    key parts. This can happen for storage engines that have the primary 
    key parts as a suffix for every secondary key.

  @retval
    1   key is ok.
  @retval
    0   Key can't be used
  @retval
    -1   Reverse key can be used
*/

static int test_if_order_by_key(JOIN *join,
                                ORDER *order, TABLE *table, uint idx,
				uint *used_key_parts)
{
  KEY_PART_INFO *key_part,*key_part_end;
  key_part=table->key_info[idx].key_part;
  key_part_end=key_part + table->key_info[idx].ext_key_parts;
  key_part_map const_key_parts=table->const_key_parts[idx];
  uint user_defined_kp= table->key_info[idx].user_defined_key_parts;
  int reverse=0;
  uint key_parts;
  bool have_pk_suffix= false;
  uint pk= table->s->primary_key;
  DBUG_ENTER("test_if_order_by_key");
 
  if ((table->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX) && 
      table->key_info[idx].ext_key_part_map &&
      pk != MAX_KEY && pk != idx)
  {
    have_pk_suffix= true;
  }

  for (; order ; order=order->next, const_key_parts>>=1)
  {
    Item_field *item_field= ((Item_field*) (*order->item)->real_item());
    Field *field= item_field->field;
    int flag;

    /*
      Skip key parts that are constants in the WHERE clause.
      These are already skipped in the ORDER BY by const_expression_in_where()
    */
    for (; const_key_parts & 1 ; const_key_parts>>= 1)
      key_part++; 
    
    /*
      This check was in this function historically (although I think it's
      better to check it outside of this function):

      "Test if the primary key parts were all const (i.e. there's one row).
       The sorting doesn't matter"

       So, we're checking that 
       (1) this is an extended key
       (2) we've reached its end
    */
    key_parts= (uint)(key_part - table->key_info[idx].key_part);
    if (have_pk_suffix &&
        reverse == 0 && // all were =const so far
        key_parts == table->key_info[idx].ext_key_parts && 
        table->const_key_parts[pk] == PREV_BITS(uint, 
                                                table->key_info[pk].
                                                user_defined_key_parts))
    {
      key_parts= 0;
      reverse= 1;                           // Key is ok to use
      goto ok;
    }

    if (key_part == key_part_end)
    {
      /*
        There are some items left in ORDER BY that we don't
      */
      DBUG_RETURN(0);
    }

    if (key_part->field != field)
    {
      /*
        Check if there is a multiple equality that allows to infer that field
        and key_part->field are equal 
        (see also: compute_part_of_sort_key_for_equals)
      */
      if (item_field->item_equal && 
          item_field->item_equal->contains(key_part->field))
        field= key_part->field;
    }
    if (key_part->field != field || !field->part_of_sortkey.is_set(idx))
      DBUG_RETURN(0);

    const ORDER::enum_order keypart_order= 
      (key_part->key_part_flag & HA_REVERSE_SORT) ? 
      ORDER::ORDER_DESC : ORDER::ORDER_ASC;
    /* set flag to 1 if we can use read-next on key, else to -1 */
    flag= (order->direction == keypart_order) ? 1 : -1;
    if (reverse && flag != reverse)
      DBUG_RETURN(0);
    reverse=flag;				// Remember if reverse
    if (key_part < key_part_end)
      key_part++;
  }

  key_parts= (uint) (key_part - table->key_info[idx].key_part);

  if (reverse == -1 && 
      !(table->file->index_flags(idx, user_defined_kp-1, 1) & HA_READ_PREV))
    reverse= 0;                               // Index can't be used
  
  if (have_pk_suffix && reverse == -1)
  {
    uint pk_parts= table->key_info[pk].user_defined_key_parts;
    if (!(table->file->index_flags(pk, pk_parts, 1) & HA_READ_PREV))
      reverse= 0;                               // Index can't be used
  }

ok:
  if (used_key_parts != NULL)
    *used_key_parts= key_parts;
  DBUG_RETURN(reverse);
}


/**
  Find shortest key suitable for full table scan.

  @param table                 Table to scan
  @param usable_keys           Allowed keys

  @return
    MAX_KEY     no suitable key found
    key index   otherwise
*/

uint find_shortest_key(TABLE *table, const key_map *usable_keys)
{
  double min_cost= DBL_MAX;
  uint best= MAX_KEY;
  if (!usable_keys->is_clear_all())
  {
    for (uint nr=0; nr < table->s->keys ; nr++)
    {
      if (usable_keys->is_set(nr))
      {
        double cost= table->file->keyread_time(nr, 1, table->file->records());
        if (cost < min_cost)
        {
          min_cost= cost;
          best=nr;
        }
        DBUG_ASSERT(best < MAX_KEY);
      }
    }
  }
  return best;
}

/**
  Test if a second key is the subkey of the first one.

  @param key_part              First key parts
  @param ref_key_part          Second key parts
  @param ref_key_part_end      Last+1 part of the second key

  @note
    Second key MUST be shorter than the first one.

  @retval
    1	is a subkey
  @retval
    0	no sub key
*/

inline bool 
is_subkey(KEY_PART_INFO *key_part, KEY_PART_INFO *ref_key_part,
	  KEY_PART_INFO *ref_key_part_end)
{
  for (; ref_key_part < ref_key_part_end; key_part++, ref_key_part++)
    if (!key_part->field->eq(ref_key_part->field))
      return 0;
  return 1;
}

/**
  Test if we can use one of the 'usable_keys' instead of 'ref' key
  for sorting.

  @param ref			Number of key, used for WHERE clause
  @param usable_keys		Keys for testing

  @return
    - MAX_KEY			If we can't use other key
    - the number of found key	Otherwise
*/

static uint
test_if_subkey(ORDER *order, TABLE *table, uint ref, uint ref_key_parts,
	       const key_map *usable_keys)
{
  uint nr;
  uint min_length= (uint) ~0;
  uint best= MAX_KEY;
  KEY_PART_INFO *ref_key_part= table->key_info[ref].key_part;
  KEY_PART_INFO *ref_key_part_end= ref_key_part + ref_key_parts;
  
  /*
    Find the shortest key that
    - produces the required ordering
    - has key #ref (up to ref_key_parts) as its subkey.
  */
  for (nr= 0 ; nr < table->s->keys ; nr++)
  {
    if (usable_keys->is_set(nr) &&
	table->key_info[nr].key_length < min_length &&
	table->key_info[nr].user_defined_key_parts >= ref_key_parts &&
	is_subkey(table->key_info[nr].key_part, ref_key_part,
		  ref_key_part_end) &&
	test_if_order_by_key(NULL, order, table, nr))
    {
      min_length= table->key_info[nr].key_length;
      best= nr;
    }
  }
  return best;
}


/**
  Check if GROUP BY/DISTINCT can be optimized away because the set is
  already known to be distinct.

  Used in removing the GROUP BY/DISTINCT of the following types of
  statements:
  @code
    SELECT [DISTINCT] <unique_key_cols>... FROM <single_table_ref>
      [GROUP BY <unique_key_cols>,...]
  @endcode

    If (a,b,c is distinct)
    then <any combination of a,b,c>,{whatever} is also distinct

    This function checks if all the key parts of any of the unique keys
    of the table are referenced by a list : either the select list
    through find_field_in_item_list or GROUP BY list through
    find_field_in_order_list.
    If the above holds and the key parts cannot contain NULLs then we 
    can safely remove the GROUP BY/DISTINCT,
    as no result set can be more distinct than an unique key.

  @param table                The table to operate on.
  @param find_func            function to iterate over the list and search
                              for a field

  @retval
    1                    found
  @retval
    0                    not found.
*/

static bool
list_contains_unique_index(TABLE *table,
                          bool (*find_func) (Field *, void *), void *data)
{
  for (uint keynr= 0; keynr < table->s->keys; keynr++)
  {
    if (keynr == table->s->primary_key ||
         (table->key_info[keynr].flags & HA_NOSAME))
    {
      KEY *keyinfo= table->key_info + keynr;
      KEY_PART_INFO *key_part, *key_part_end;

      for (key_part=keyinfo->key_part,
           key_part_end=key_part+ keyinfo->user_defined_key_parts;
           key_part < key_part_end;
           key_part++)
      {
        if (key_part->field->maybe_null() ||
            !find_func(key_part->field, data))
          break;
      }
      if (key_part == key_part_end)
        return 1;
    }
  }
  return 0;
}


/**
  Helper function for list_contains_unique_index.
  Find a field reference in a list of ORDER structures.
  Finds a direct reference of the Field in the list.

  @param field                The field to search for.
  @param data                 ORDER *.The list to search in

  @retval
    1                    found
  @retval
    0                    not found.
*/

static bool
find_field_in_order_list (Field *field, void *data)
{
  ORDER *group= (ORDER *) data;
  bool part_found= 0;
  for (ORDER *tmp_group= group; tmp_group; tmp_group=tmp_group->next)
  {
    Item *item= (*tmp_group->item)->real_item();
    if (item->type() == Item::FIELD_ITEM &&
        ((Item_field*) item)->field->eq(field))
    {
      part_found= 1;
      break;
    }
  }
  return part_found;
}


/**
  Helper function for list_contains_unique_index.
  Find a field reference in a dynamic list of Items.
  Finds a direct reference of the Field in the list.

  @param[in] field             The field to search for.
  @param[in] data              List<Item> *.The list to search in

  @retval
    1                    found
  @retval
    0                    not found.
*/

static bool
find_field_in_item_list (Field *field, void *data)
{
  List<Item> *fields= (List<Item> *) data;
  bool part_found= 0;
  List_iterator<Item> li(*fields);
  Item *item;

  while ((item= li++))
  {
    if (item->real_item()->type() == Item::FIELD_ITEM &&
	((Item_field*) (item->real_item()))->field->eq(field))
    {
      part_found= 1;
      break;
    }
  }
  return part_found;
}


/*
  Fill *col_keys with a union of Field::part_of_sortkey of all fields
  that belong to 'table' and are equal to 'item_field'.
*/

void compute_part_of_sort_key_for_equals(JOIN *join, TABLE *table,
                                         Item_field *item_field,
                                         key_map *col_keys)
{
  col_keys->clear_all();
  col_keys->merge(item_field->field->part_of_sortkey);
  
  if (!optimizer_flag(join->thd, OPTIMIZER_SWITCH_ORDERBY_EQ_PROP))
    return;

  Item_equal *item_eq= NULL;

  if (item_field->item_equal)
  {
    /* 
      The item_field is from ORDER structure, but it already has an item_equal
      pointer set (UseMultipleEqualitiesToRemoveTempTable code have set it)
    */
    item_eq= item_field->item_equal;
  }
  else
  {
    /* 
      Walk through join's muliple equalities and find the one that contains
      item_field.
    */
    if (!join->cond_equal)
      return;
    table_map needed_tbl_map= item_field->used_tables() | table->map;
    List_iterator<Item_equal> li(join->cond_equal->current_level);
    Item_equal *cur_item_eq;
    while ((cur_item_eq= li++))
    {
      if ((cur_item_eq->used_tables() & needed_tbl_map) &&
          cur_item_eq->contains(item_field->field))
      {
        item_eq= cur_item_eq;
        item_field->item_equal= item_eq; // Save the pointer to our Item_equal.
        break;
      }
    }
  }
  
  if (item_eq)
  {
    Item_equal_fields_iterator it(*item_eq);
    Item *item;
    /* Loop through other members that belong to table table */
    while ((item= it++))
    {
      if (item->type() == Item::FIELD_ITEM &&
          ((Item_field*)item)->field->table == table)
      {
        col_keys->merge(((Item_field*)item)->field->part_of_sortkey);
      }
    }
  }
}


/**
  Test if we can skip the ORDER BY by using an index.

  If we can use an index, the JOIN_TAB / tab->select struct
  is changed to use the index.

  The index must cover all fields in <order>, or it will not be considered.

  @param no_changes No changes will be made to the query plan.

  @todo
    - sergeyp: Results of all index merge selects actually are ordered 
    by clustered PK values.

  @retval
    0    We have to use filesort to do the sorting
  @retval
    1    We can use an index.
*/

static bool
test_if_skip_sort_order(JOIN_TAB *tab,ORDER *order,ha_rows select_limit,
			bool no_changes, const key_map *map)
{
  int ref_key;
  uint UNINIT_VAR(ref_key_parts);
  int order_direction= 0;
  uint used_key_parts= 0;
  TABLE *table=tab->table;
  SQL_SELECT *select=tab->select;
  key_map usable_keys;
  QUICK_SELECT_I *save_quick= select ? select->quick : 0;
  Item *orig_cond= 0;
  bool orig_cond_saved= false;
  int best_key= -1;
  bool changed_key= false;
  DBUG_ENTER("test_if_skip_sort_order");

  /* Check that we are always called with first non-const table */
  DBUG_ASSERT(tab == tab->join->join_tab + tab->join->const_tables);

  /*
    Keys disabled by ALTER TABLE ... DISABLE KEYS should have already
    been taken into account.
  */
  usable_keys= *map;
  
  /* Find indexes that cover all ORDER/GROUP BY fields */
  for (ORDER *tmp_order=order; tmp_order ; tmp_order=tmp_order->next)
  {
    Item *item= (*tmp_order->item)->real_item();
    if (item->type() != Item::FIELD_ITEM)
    {
      usable_keys.clear_all();
      DBUG_RETURN(0);
    }

    /*
      Take multiple-equalities into account. Suppose we have
        ORDER BY col1, col10
      and there are
         multiple-equal(col1, col2, col3),
         multiple-equal(col10, col11).

      Then, 
      - when item=col1, we find the set of indexes that cover one of {col1,
        col2, col3}
      - when item=col10, we find the set of indexes that cover one of {col10,
        col11}

      And we compute an intersection of these sets to find set of indexes that
      cover all ORDER BY components.
    */
    key_map col_keys;
    compute_part_of_sort_key_for_equals(tab->join, table, (Item_field*)item,
                                        &col_keys);
    usable_keys.intersect(col_keys);
    if (usable_keys.is_clear_all())
      goto use_filesort;                        // No usable keys
  }

  ref_key= -1;
  /* Test if constant range in WHERE */
  if (tab->ref.key >= 0 && tab->ref.key_parts)
  {
    ref_key=	   tab->ref.key;
    ref_key_parts= tab->ref.key_parts;
    /* 
      todo: why does JT_REF_OR_NULL mean filesort? We could find another index
      that satisfies the ordering. I would just set ref_key=MAX_KEY here...
    */
    if (tab->type == JT_REF_OR_NULL || tab->type == JT_FT ||
        tab->ref.uses_splitting)
      goto use_filesort;
  }
  else if (select && select->quick)		// Range found by opt_range
  {
    int quick_type= select->quick->get_type();
    /* 
      assume results are not ordered when index merge is used 
      TODO: sergeyp: Results of all index merge selects actually are ordered 
      by clustered PK values.
    */
  
    if (quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE ||
        quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_INTERSECT ||
        quick_type == QUICK_SELECT_I::QS_TYPE_ROR_UNION || 
        quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT)
    {
      /*
        we set ref_key=MAX_KEY instead of -1, because test_if_cheaper ordering
        assumes that "ref_key==-1" means doing full index scan. 
        (This is not very straightforward and we got into this situation for 
         historical reasons. Should be fixed at some point).
      */
      ref_key= MAX_KEY;
    }
    else
    {
      ref_key= select->quick->index;
      ref_key_parts= select->quick->used_key_parts;
    }
  }

  if (ref_key >= 0 && ref_key != MAX_KEY)
  {
    /* Current access method uses index ref_key with ref_key_parts parts */
    if (!usable_keys.is_set(ref_key))
    {
      /* However, ref_key doesn't match the needed ordering */
      uint new_ref_key;

      /*
	If using index only read, only consider other possible index only
	keys
      */
      if (table->covering_keys.is_set(ref_key))
	usable_keys.intersect(table->covering_keys);
      if (tab->pre_idx_push_select_cond)
      {
        orig_cond= tab->set_cond(tab->pre_idx_push_select_cond);
        orig_cond_saved= true;
      }

      if ((new_ref_key= test_if_subkey(order, table, ref_key, ref_key_parts,
				       &usable_keys)) < MAX_KEY)
      {
        /*
          Index new_ref_key 
          - produces the required ordering, 
          - also has the same columns as ref_key for #ref_key_parts (this
            means we will read the same number of rows as with ref_key).
        */

        /*
          If new_ref_key allows to construct a quick select which uses more key
          parts than ref(new_ref_key) would, do that.

          Otherwise, construct a ref access (todo: it's not clear what is the
          win in using ref access when we could use quick select also?)
        */
        if ((table->quick_keys.is_set(new_ref_key) && 
             table->quick_key_parts[new_ref_key] > ref_key_parts) ||
             !(tab->ref.key >= 0))
	{
          /*
            The range optimizer constructed QUICK_RANGE for ref_key, and
            we want to use instead new_ref_key as the index. We can't
            just change the index of the quick select, because this may
            result in an inconsistent QUICK_SELECT object. Below we
            create a new QUICK_SELECT from scratch so that all its
            parameters are set correctly by the range optimizer.
           */
          key_map new_ref_key_map;
          COND *save_cond;
          bool res;
          new_ref_key_map.clear_all();  // Force the creation of quick select
          new_ref_key_map.set_bit(new_ref_key); // only for new_ref_key.

          /* Reset quick;  This will be restored in 'use_filesort' if needed */
          select->quick= 0;
          save_cond= select->cond;
          if (select->pre_idx_push_select_cond)
            select->cond= select->pre_idx_push_select_cond;
          res= select->test_quick_select(tab->join->thd, new_ref_key_map, 0,
                                         (tab->join->select_options &
                                          OPTION_FOUND_ROWS) ?
                                         HA_POS_ERROR :
                                         tab->join->unit->select_limit_cnt,TRUE,
                                         TRUE, FALSE) <= 0;
          if (res)
          {
            select->cond= save_cond;
            goto use_filesort;
          }
          DBUG_ASSERT(tab->select->quick);
          tab->type= JT_ALL;
          tab->ref.key= -1;
          tab->ref.key_parts= 0;
          tab->use_quick= 1;
          best_key= new_ref_key;
          /*
            We don't restore select->cond as we want to use the
            original condition as index condition pushdown is not
            active for the new index.
            todo: why not perform index condition pushdown for the new index?
          */
	}
        else
	{
          /*
            We'll use ref access method on key new_ref_key. In general case 
            the index search tuple for new_ref_key will be different (e.g.
            when one index is defined as (part1, part2, ...) and another as
            (part1, part2(N), ...) and the WHERE clause contains 
            "part1 = const1 AND part2=const2". 
            So we build tab->ref from scratch here.
          */
          KEYUSE *keyuse= tab->keyuse;
          while (keyuse->key != new_ref_key && keyuse->table == tab->table)
            keyuse++;
          if (create_ref_for_key(tab->join, tab, keyuse, FALSE,
                                 (tab->join->const_table_map |
                                  OUTER_REF_TABLE_BIT)))
            goto use_filesort;

          pick_table_access_method(tab);
	}

        ref_key= new_ref_key;
        changed_key= true;
     }
    }
    /* Check if we get the rows in requested sorted order by using the key */
    if (usable_keys.is_set(ref_key) &&
        (order_direction= test_if_order_by_key(tab->join, order,table,ref_key,
					       &used_key_parts)))
      goto check_reverse_order;
  }
  {
    uint UNINIT_VAR(best_key_parts);
    uint saved_best_key_parts= 0;
    int best_key_direction= 0;
    JOIN *join= tab->join;
    ha_rows table_records= table->stat_records();

    test_if_cheaper_ordering(tab, order, table, usable_keys,
                             ref_key, select_limit,
                             &best_key, &best_key_direction,
                             &select_limit, &best_key_parts,
                             &saved_best_key_parts);

    /*
      filesort() and join cache are usually faster than reading in 
      index order and not using join cache, except in case that chosen
      index is clustered key.
    */
    if (best_key < 0 ||
        ((select_limit >= table_records) &&
         (tab->type == JT_ALL &&
         tab->join->table_count > tab->join->const_tables + 1) &&
         !(table->file->index_flags(best_key, 0, 1) & HA_CLUSTERED_INDEX)))
      goto use_filesort;

    if (select && // psergey:  why doesn't this use a quick?
        table->quick_keys.is_set(best_key) && best_key != ref_key)
    {
      key_map tmp_map;
      tmp_map.clear_all();       // Force the creation of quick select
      tmp_map.set_bit(best_key); // only best_key.
      select->quick= 0;

      bool cond_saved= false;
      Item *saved_cond;

      /*
        Index Condition Pushdown may have removed parts of the condition for
        this table. Temporarily put them back because we want the whole
        condition for the range analysis.
      */
      if (select->pre_idx_push_select_cond)
      {
        saved_cond= select->cond;
        select->cond= select->pre_idx_push_select_cond;
        cond_saved= true;
      }

      select->test_quick_select(join->thd, tmp_map, 0,
                                join->select_options & OPTION_FOUND_ROWS ?
                                HA_POS_ERROR :
                                join->unit->select_limit_cnt,
                                TRUE, FALSE, FALSE);

      if (cond_saved)
        select->cond= saved_cond;
    }
    order_direction= best_key_direction;
    /*
      saved_best_key_parts is actual number of used keyparts found by the
      test_if_order_by_key function. It could differ from keyinfo->user_defined_key_parts,
      thus we have to restore it in case of desc order as it affects
      QUICK_SELECT_DESC behaviour.
    */
    used_key_parts= (order_direction == -1) ?
      saved_best_key_parts :  best_key_parts;
    changed_key= true;
  }

check_reverse_order:                  
  DBUG_ASSERT(order_direction != 0);

  if (order_direction == -1)		// If ORDER BY ... DESC
  {
    int quick_type;
    if (select && select->quick)
    {
      /*
	Don't reverse the sort order, if it's already done.
        (In some cases test_if_order_by_key() can be called multiple times
      */
      if (select->quick->reverse_sorted())
        goto skipped_filesort;

      quick_type= select->quick->get_type();
      if (quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE ||
          quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_INTERSECT ||
          quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT ||
          quick_type == QUICK_SELECT_I::QS_TYPE_ROR_UNION ||
          quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX)
      {
        tab->limit= 0;
        goto use_filesort;               // Use filesort
      }
    }
  }

  /*
    Update query plan with access pattern for doing ordered access
    according to what we have decided above.
  */
  if (!no_changes) // We are allowed to update QEP
  {
    if (best_key >= 0)
    {
      bool quick_created= 
        (select && select->quick && select->quick!=save_quick);

      /* 
         If ref_key used index tree reading only ('Using index' in EXPLAIN),
         and best_key doesn't, then revert the decision.
      */
      if (table->covering_keys.is_set(best_key))
        table->file->ha_start_keyread(best_key);
      else
        table->file->ha_end_keyread();

      if (!quick_created)
      {
        if (select)                  // Throw any existing quick select
          select->quick= 0;          // Cleanup either reset to save_quick,
                                     // or 'delete save_quick'
        tab->index= best_key;
        tab->read_first_record= order_direction > 0 ?
                                join_read_first:join_read_last;
        tab->type=JT_NEXT;           // Read with index_first(), index_next()

        if (tab->pre_idx_push_select_cond)
        {
          tab->set_cond(tab->pre_idx_push_select_cond);
          /*
            orig_cond is a part of pre_idx_push_cond,
            no need to restore it.
          */
          orig_cond= 0;
          orig_cond_saved= false;
        }

        table->file->ha_index_or_rnd_end();
        if (tab->join->select_options & SELECT_DESCRIBE)
        {
          tab->ref.key= -1;
          tab->ref.key_parts= 0;
          if (select_limit < table->stat_records())
            tab->limit= select_limit;
          table->file->ha_end_keyread();
        }
      }
      else if (tab->type != JT_ALL || tab->select->quick)
      {
        /*
          We're about to use a quick access to the table.
          We need to change the access method so as the quick access
          method is actually used.
        */
        DBUG_ASSERT(tab->select->quick);
        tab->type=JT_ALL;
        tab->use_quick=1;
        tab->ref.key= -1;
        tab->ref.key_parts=0;		// Don't use ref key.
        tab->read_first_record= join_init_read_record;
        if (tab->is_using_loose_index_scan())
          tab->join->tmp_table_param.precomputed_group_by= TRUE;

        /*
          Restore the original condition as changes done by pushdown
          condition are not relevant anymore
        */
        if (tab->select && tab->select->pre_idx_push_select_cond)
	{
          tab->set_cond(tab->select->pre_idx_push_select_cond);
           tab->table->file->cancel_pushed_idx_cond();
        }
        /*
          TODO: update the number of records in join->best_positions[tablenr]
        */
      }
    } // best_key >= 0

    if (order_direction == -1)		// If ORDER BY ... DESC
    {
      if (select && select->quick)
      {
        /* ORDER BY range_key DESC */
        QUICK_SELECT_I *tmp= select->quick->make_reverse(used_key_parts);
        if (!tmp)
        {
          tab->limit= 0;
          goto use_filesort;           // Reverse sort failed -> filesort
        }
        /*
          Cancel Pushed Index Condition, as it doesn't work for reverse scans.
        */
        if (tab->select && tab->select->pre_idx_push_select_cond)
	{
          tab->set_cond(tab->select->pre_idx_push_select_cond);
           tab->table->file->cancel_pushed_idx_cond();
        }
        if (select->quick == save_quick)
          save_quick= 0;                // make_reverse() consumed it
        select->set_quick(tmp);
        /* Cancel "Range checked for each record" */
        if (tab->use_quick == 2)
        {
          tab->use_quick= 1;
          tab->read_first_record= join_init_read_record;
        }
      }
      else if (tab->type != JT_NEXT && tab->type != JT_REF_OR_NULL &&
               tab->ref.key >= 0 && tab->ref.key_parts <= used_key_parts)
      {
        /*
          SELECT * FROM t1 WHERE a=1 ORDER BY a DESC,b DESC

          Use a traversal function that starts by reading the last row
          with key part (A) and then traverse the index backwards.
        */
        tab->read_first_record= join_read_last_key;
        tab->read_record.read_record_func= join_read_prev_same;
        /* Cancel "Range checked for each record" */
        if (tab->use_quick == 2)
        {
          tab->use_quick= 1;
          tab->read_first_record= join_init_read_record;
        }
        /*
          Cancel Pushed Index Condition, as it doesn't work for reverse scans.
        */
        if (tab->select && tab->select->pre_idx_push_select_cond)
	{
          tab->set_cond(tab->select->pre_idx_push_select_cond);
           tab->table->file->cancel_pushed_idx_cond();
        }
      }
    }
    else if (select && select->quick)
      select->quick->need_sorted_output();

    tab->read_record.unlock_row= (tab->type == JT_EQ_REF) ?
                                 join_read_key_unlock_row : rr_unlock_row;

  } // QEP has been modified

  /*
    Cleanup:
    We may have both a 'select->quick' and 'save_quick' (original)
    at this point. Delete the one that we wan't use.
  */

skipped_filesort:
  // Keep current (ordered) select->quick 
  if (select && save_quick != select->quick)
  {
    delete save_quick;
    save_quick= NULL;
  }
  if (orig_cond_saved && !changed_key)
    tab->set_cond(orig_cond);
  if (!no_changes && changed_key && table->file->pushed_idx_cond)
    table->file->cancel_pushed_idx_cond();

  DBUG_RETURN(1);

use_filesort:
  // Restore original save_quick
  if (select && select->quick != save_quick)
  {
    delete select->quick;
    select->quick= save_quick;
  }
  if (orig_cond_saved)
    tab->set_cond(orig_cond);

  DBUG_RETURN(0);
}


/*
  If not selecting by given key, create an index how records should be read

  SYNOPSIS
   create_sort_index()
     thd		Thread handler
     join		Join with table to sort
     join_tab		What table to sort
     fsort              Filesort object.  NULL means "use tab->filesort".
 
  IMPLEMENTATION
   - If there is an index that can be used, the first non-const join_tab in
     'join' is modified to use this index.
   - If no index, create with filesort() an index file that can be used to
     retrieve rows in order (should be done with 'read_record').
     The sorted data is stored in tab->filesort

  RETURN VALUES
    0		ok
    -1		Some fatal error
    1		No records
*/

int
create_sort_index(THD *thd, JOIN *join, JOIN_TAB *tab, Filesort *fsort)
{
  TABLE *table;
  SQL_SELECT *select;
  bool quick_created= FALSE;
  SORT_INFO *file_sort= 0;
  DBUG_ENTER("create_sort_index");

  if (fsort == NULL)
    fsort= tab->filesort;

  table=  tab->table;
  select= fsort->select;
 
  table->status=0;				// May be wrong if quick_select

  if (!tab->preread_init_done && tab->preread_init())
    goto err;

  // If table has a range, move it to select
  if (select && tab->ref.key >= 0)
  {
    if (!select->quick)
    {
      if (tab->quick)
      {
        select->quick= tab->quick;
        tab->quick= NULL;
      /* 
        We can only use 'Only index' if quick key is same as ref_key
        and in index_merge 'Only index' cannot be used
      */
      if (((uint) tab->ref.key != select->quick->index))
        table->file->ha_end_keyread();
      }
      else
      {
        /*
	  We have a ref on a const;  Change this to a range that filesort
	  can use.
	  For impossible ranges (like when doing a lookup on NULL on a NOT NULL
	  field, quick will contain an empty record set.
        */
        if (!(select->quick= (tab->type == JT_FT ?
			      get_ft_select(thd, table, tab->ref.key) :
			      get_quick_select_for_ref(thd, table, &tab->ref, 
                                                       tab->found_records))))
	  goto err;
        quick_created= TRUE;
      }
      fsort->own_select= true;
    }
    else
    {
      DBUG_ASSERT(tab->type == JT_REF || tab->type == JT_EQ_REF);
      // Update ref value
      if (unlikely(cp_buffer_from_ref(thd, table, &tab->ref) &&
                   thd->is_fatal_error))
        goto err;                                   // out of memory
    }
  }

 
  /* Fill schema tables with data before filesort if it's necessary */
  if ((join->select_lex->options & OPTION_SCHEMA_TABLE) &&
      unlikely(get_schema_tables_result(join, PROCESSED_BY_CREATE_SORT_INDEX)))
    goto err;

  if (table->s->tmp_table)
    table->file->info(HA_STATUS_VARIABLE);	// Get record count
  file_sort= filesort(thd, table, fsort, fsort->tracker, join, tab->table->map);
  DBUG_ASSERT(tab->filesort_result == 0);
  tab->filesort_result= file_sort;
  tab->records= 0;
  if (file_sort)
  {
    tab->records= join->select_options & OPTION_FOUND_ROWS ?
      file_sort->found_rows : file_sort->return_rows;
    tab->join->join_examined_rows+= file_sort->examined_rows;
  }

  if (quick_created)
  {
    /* This will delete the quick select. */
    select->cleanup();
  }
 
  table->file->ha_end_keyread();
  if (tab->type == JT_FT)
    table->file->ha_ft_end();
  else
    table->file->ha_index_or_rnd_end();

  DBUG_RETURN(file_sort == 0);
err:
  DBUG_RETURN(-1);
}


/**
  Compare fields from table->record[0] and table->record[1],
  possibly skipping few first fields.

  @param table
  @param ptr                    field to start the comparison from,
                                somewhere in the table->field[] array

  @retval 1     different
  @retval 0     identical
*/
static bool compare_record(TABLE *table, Field **ptr)
{
  for (; *ptr ; ptr++)
  {
    Field *f= *ptr;
    if (f->is_null() != f->is_null(table->s->rec_buff_length) ||
        (!f->is_null() && f->cmp_offset(table->s->rec_buff_length)))
      return 1;
  }
  return 0;
}

static bool copy_blobs(Field **ptr)
{
  for (; *ptr ; ptr++)
  {
    if ((*ptr)->flags & BLOB_FLAG)
      if (((Field_blob *) (*ptr))->copy())
	return 1;				// Error
  }
  return 0;
}

static void free_blobs(Field **ptr)
{
  for (; *ptr ; ptr++)
  {
    if ((*ptr)->flags & BLOB_FLAG)
      ((Field_blob *) (*ptr))->free();
  }
}


/*
  @brief
    Remove duplicates from a temporary table.

  @detail
    Remove duplicate rows from a temporary table. This is used for e.g. queries
    like

      select distinct count(*) as CNT from tbl group by col

    Here, we get a group table with count(*) values. It is not possible to
    prevent duplicates from appearing in the table (as we don't know the values
    before we've done the grouping).  Because of that, we have this function to
    scan the temptable (maybe, multiple times) and remove the duplicate rows

    Rows that do not satisfy 'having' condition are also removed.
*/

bool
JOIN_TAB::remove_duplicates()

{
  bool error;
  ulong keylength= 0;
  uint field_count;
  List<Item> *fields= (this-1)->fields;
  THD *thd= join->thd;

  DBUG_ENTER("remove_duplicates");

  DBUG_ASSERT(join->aggr_tables > 0 && table->s->tmp_table != NO_TMP_TABLE);
  THD_STAGE_INFO(join->thd, stage_removing_duplicates);

  //join->explain->ops_tracker.report_duplicate_removal();

  table->reginfo.lock_type=TL_WRITE;

  /* Calculate how many saved fields there is in list */
  field_count=0;
  List_iterator<Item> it(*fields);
  Item *item;
  while ((item=it++))
  {
    if (item->get_tmp_table_field() && ! item->const_item())
      field_count++;
  }

  if (!field_count && !(join->select_options & OPTION_FOUND_ROWS) && !having) 
  {                    // only const items with no OPTION_FOUND_ROWS
    join->unit->select_limit_cnt= 1;		// Only send first row
    DBUG_RETURN(false);
  }

  Field **first_field=table->field+table->s->fields - field_count;
  for (Field **ptr=first_field; *ptr; ptr++)
    keylength+= (*ptr)->sort_length() + (*ptr)->maybe_null();

  /*
    Disable LIMIT ROWS EXAMINED in order to avoid interrupting prematurely
    duplicate removal, and produce a possibly incomplete query result.
  */
  thd->lex->limit_rows_examined_cnt= ULONGLONG_MAX;
  if (thd->killed == ABORT_QUERY)
    thd->reset_killed();

  table->file->info(HA_STATUS_VARIABLE);
  if (table->s->db_type() == heap_hton ||
      (!table->s->blob_fields &&
       ((ALIGN_SIZE(keylength) + HASH_OVERHEAD) * table->file->stats.records <
	thd->variables.sortbuff_size)))
    error=remove_dup_with_hash_index(join->thd, table, field_count, first_field,
				     keylength, having);
  else
    error=remove_dup_with_compare(join->thd, table, first_field, having);

  if (join->select_lex != join->select_lex->master_unit()->fake_select_lex)
    thd->lex->set_limit_rows_examined();
  free_blobs(first_field);
  DBUG_RETURN(error);
}


static int remove_dup_with_compare(THD *thd, TABLE *table, Field **first_field,
				   Item *having)
{
  handler *file=table->file;
  uchar *record=table->record[0];
  int error;
  DBUG_ENTER("remove_dup_with_compare");

  if (unlikely(file->ha_rnd_init_with_error(1)))
    DBUG_RETURN(1);

  error= file->ha_rnd_next(record);
  for (;;)
  {
    if (unlikely(thd->check_killed()))
    {
      error=0;
      goto err;
    }
    if (unlikely(error))
    {
      if (error == HA_ERR_END_OF_FILE)
	break;
      goto err;
    }
    if (having && !having->val_int())
    {
      if (unlikely((error= file->ha_delete_row(record))))
	goto err;
      error= file->ha_rnd_next(record);
      continue;
    }
    if (unlikely(copy_blobs(first_field)))
    {
      my_message(ER_OUTOFMEMORY, ER_THD(thd,ER_OUTOFMEMORY),
                 MYF(ME_FATALERROR));
      error=0;
      goto err;
    }
    store_record(table,record[1]);

    /* Read through rest of file and mark duplicated rows deleted */
    bool found=0;
    for (;;)
    {
      if (unlikely((error= file->ha_rnd_next(record))))
      {
	if (error == HA_ERR_END_OF_FILE)
	  break;
	goto err;
      }
      if (compare_record(table, first_field) == 0)
      {
	if (unlikely((error= file->ha_delete_row(record))))
	  goto err;
      }
      else if (!found)
      {
	found=1;
        if (unlikely((error= file->remember_rnd_pos())))
          goto err;
      }
    }
    if (!found)
      break;					// End of file
    /* Restart search on saved row */
    if (unlikely((error= file->restart_rnd_next(record))))
      goto err;
  }

  file->extra(HA_EXTRA_NO_CACHE);
  (void) file->ha_rnd_end();
  DBUG_RETURN(0);
err:
  file->extra(HA_EXTRA_NO_CACHE);
  (void) file->ha_rnd_end();
  if (error)
    file->print_error(error,MYF(0));
  DBUG_RETURN(1);
}


/**
  Generate a hash index for each row to quickly find duplicate rows.

  @note
    Note that this will not work on tables with blobs!
*/

static int remove_dup_with_hash_index(THD *thd, TABLE *table,
				      uint field_count,
				      Field **first_field,
				      ulong key_length,
				      Item *having)
{
  uchar *key_buffer, *key_pos, *record=table->record[0];
  int error;
  handler *file= table->file;
  ulong extra_length= ALIGN_SIZE(key_length)-key_length;
  uint *field_lengths, *field_length;
  HASH hash;
  Field **ptr;
  DBUG_ENTER("remove_dup_with_hash_index");

  if (unlikely(!my_multi_malloc(MYF(MY_WME),
                                &key_buffer,
                                (uint) ((key_length + extra_length) *
                                        (long) file->stats.records),
                                &field_lengths,
                                (uint) (field_count*sizeof(*field_lengths)),
                                NullS)))
    DBUG_RETURN(1);

  for (ptr= first_field, field_length=field_lengths ; *ptr ; ptr++)
    (*field_length++)= (*ptr)->sort_length();

  if (unlikely(my_hash_init(&hash, &my_charset_bin,
                            (uint) file->stats.records, 0,
                            key_length, (my_hash_get_key) 0, 0, 0)))
  {
    my_free(key_buffer);
    DBUG_RETURN(1);
  }

  if (unlikely((error= file->ha_rnd_init(1))))
    goto err;

  key_pos=key_buffer;
  for (;;)
  {
    uchar *org_key_pos;
    if (unlikely(thd->check_killed()))
    {
      error=0;
      goto err;
    }
    if (unlikely((error= file->ha_rnd_next(record))))
    {
      if (error == HA_ERR_END_OF_FILE)
	break;
      goto err;
    }
    if (having && !having->val_int())
    {
      if (unlikely((error= file->ha_delete_row(record))))
	goto err;
      continue;
    }

    /* copy fields to key buffer */
    org_key_pos= key_pos;
    field_length=field_lengths;
    for (ptr= first_field ; *ptr ; ptr++)
    {
      (*ptr)->make_sort_key(key_pos, *field_length);
      key_pos+= (*ptr)->maybe_null() + *field_length++;
    }
    /* Check if it exists before */
    if (my_hash_search(&hash, org_key_pos, key_length))
    {
      /* Duplicated found ; Remove the row */
      if (unlikely((error= file->ha_delete_row(record))))
	goto err;
    }
    else
    {
      if (my_hash_insert(&hash, org_key_pos))
        goto err;
    }
    key_pos+=extra_length;
  }
  my_free(key_buffer);
  my_hash_free(&hash);
  file->extra(HA_EXTRA_NO_CACHE);
  (void) file->ha_rnd_end();
  DBUG_RETURN(0);

err:
  my_free(key_buffer);
  my_hash_free(&hash);
  file->extra(HA_EXTRA_NO_CACHE);
  (void) file->ha_rnd_end();
  if (unlikely(error))
    file->print_error(error,MYF(0));
  DBUG_RETURN(1);
}


/*
  eq_ref: Create the lookup key and check if it is the same as saved key

  SYNOPSIS
    cmp_buffer_with_ref()
      tab      Join tab of the accessed table
      table    The table to read.  This is usually tab->table, except for 
               semi-join when we might need to make a lookup in a temptable
               instead.
      tab_ref  The structure with methods to collect index lookup tuple. 
               This is usually table->ref, except for the case of when we're 
               doing lookup into semi-join materialization table.

  DESCRIPTION 
    Used by eq_ref access method: create the index lookup key and check if 
    we've used this key at previous lookup (If yes, we don't need to repeat
    the lookup - the record has been already fetched)

  RETURN 
    TRUE   No cached record for the key, or failed to create the key (due to
           out-of-domain error)
    FALSE  The created key is the same as the previous one (and the record 
           is already in table->record)
*/

static bool
cmp_buffer_with_ref(THD *thd, TABLE *table, TABLE_REF *tab_ref)
{
  bool no_prev_key;
  if (!tab_ref->disable_cache)
  {
    if (!(no_prev_key= tab_ref->key_err))
    {
      /* Previous access found a row. Copy its key */
      memcpy(tab_ref->key_buff2, tab_ref->key_buff, tab_ref->key_length);
    }
  }
  else 
    no_prev_key= TRUE;
  if ((tab_ref->key_err= cp_buffer_from_ref(thd, table, tab_ref)) ||
      no_prev_key)
    return 1;
  return memcmp(tab_ref->key_buff2, tab_ref->key_buff, tab_ref->key_length)
    != 0;
}


bool
cp_buffer_from_ref(THD *thd, TABLE *table, TABLE_REF *ref)
{
  Check_level_instant_set check_level_save(thd, CHECK_FIELD_IGNORE);
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->write_set);
  bool result= 0;

  for (store_key **copy=ref->key_copy ; *copy ; copy++)
  {
    if ((*copy)->copy() & 1)
    {
      result= 1;
      break;
    }
  }
  dbug_tmp_restore_column_map(&table->write_set, old_map);
  return result;
}


/*****************************************************************************
  Group and order functions
*****************************************************************************/

/**
  Resolve an ORDER BY or GROUP BY column reference.

  Given a column reference (represented by 'order') from a GROUP BY or ORDER
  BY clause, find the actual column it represents. If the column being
  resolved is from the GROUP BY clause, the procedure searches the SELECT
  list 'fields' and the columns in the FROM list 'tables'. If 'order' is from
  the ORDER BY clause, only the SELECT list is being searched.

  If 'order' is resolved to an Item, then order->item is set to the found
  Item. If there is no item for the found column (that is, it was resolved
  into a table field), order->item is 'fixed' and is added to all_fields and
  ref_pointer_array.

  ref_pointer_array and all_fields are updated.

  @param[in] thd		    Pointer to current thread structure
  @param[in,out] ref_pointer_array  All select, group and order by fields
  @param[in] tables                 List of tables to search in (usually
    FROM clause)
  @param[in] order                  Column reference to be resolved
  @param[in] fields                 List of fields to search in (usually
    SELECT list)
  @param[in,out] all_fields         All select, group and order by fields
  @param[in] is_group_field         True if order is a GROUP field, false if
                                    ORDER by field
  @param[in] add_to_all_fields      If the item is to be added to all_fields and
                                    ref_pointer_array, this flag can be set to
                                    false to stop the automatic insertion.
  @param[in] from_window_spec       If true then order is from a window spec

  @retval
    FALSE if OK
  @retval
    TRUE  if error occurred
*/

static bool
find_order_in_list(THD *thd, Ref_ptr_array ref_pointer_array,
                   TABLE_LIST *tables,
                   ORDER *order, List<Item> &fields, List<Item> &all_fields,
                   bool is_group_field, bool add_to_all_fields,
                   bool from_window_spec)
{
  Item *order_item= *order->item; /* The item from the GROUP/ORDER caluse. */
  Item::Type order_item_type;
  Item **select_item; /* The corresponding item from the SELECT clause. */
  Field *from_field;  /* The corresponding field from the FROM clause. */
  uint counter;
  enum_resolution_type resolution;

  /*
    Local SP variables may be int but are expressions, not positions.
    (And they can't be used before fix_fields is called for them).
  */
  if (order_item->type() == Item::INT_ITEM && order_item->basic_const_item() &&
      !from_window_spec)
  {						/* Order by position */
    uint count;
    if (order->counter_used)
      count= order->counter; // counter was once resolved
    else
      count= (uint) order_item->val_int();
    if (!count || count > fields.elements)
    {
      my_error(ER_BAD_FIELD_ERROR, MYF(0),
               order_item->full_name(), thd->where);
      return TRUE;
    }
    thd->change_item_tree((Item **)&order->item, (Item *)&ref_pointer_array[count - 1]);
    order->in_field_list= 1;
    order->counter= count;
    order->counter_used= 1;
    return FALSE;
  }
  /* Lookup the current GROUP/ORDER field in the SELECT clause. */
  select_item= find_item_in_list(order_item, fields, &counter,
                                 REPORT_EXCEPT_NOT_FOUND, &resolution);
  if (!select_item)
    return TRUE; /* The item is not unique, or some other error occurred. */


  /* Check whether the resolved field is not ambiguos. */
  if (select_item != not_found_item)
  {
    Item *view_ref= NULL;
    /*
      If we have found field not by its alias in select list but by its
      original field name, we should additionally check if we have conflict
      for this name (in case if we would perform lookup in all tables).
    */
    if (resolution == RESOLVED_BEHIND_ALIAS &&
        order_item->fix_fields_if_needed_for_order_by(thd, order->item))
      return TRUE;

    /* Lookup the current GROUP field in the FROM clause. */
    order_item_type= order_item->type();
    from_field= (Field*) not_found_field;
    if ((is_group_field && order_item_type == Item::FIELD_ITEM) ||
        order_item_type == Item::REF_ITEM)
    {
      from_field= find_field_in_tables(thd, (Item_ident*) order_item, tables,
                                       NULL, &view_ref, IGNORE_ERRORS, FALSE,
                                       FALSE);
      if (!from_field)
        from_field= (Field*) not_found_field;
    }

    if (from_field == not_found_field ||
        (from_field != view_ref_found ?
         /* it is field of base table => check that fields are same */
         ((*select_item)->type() == Item::FIELD_ITEM &&
          ((Item_field*) (*select_item))->field->eq(from_field)) :
         /*
           in is field of view table => check that references on translation
           table are same
         */
         ((*select_item)->type() == Item::REF_ITEM &&
          view_ref->type() == Item::REF_ITEM &&
          ((Item_ref *) (*select_item))->ref ==
          ((Item_ref *) view_ref)->ref)))
    {
      /*
        If there is no such field in the FROM clause, or it is the same field
        as the one found in the SELECT clause, then use the Item created for
        the SELECT field. As a result if there was a derived field that
        'shadowed' a table field with the same name, the table field will be
        chosen over the derived field.
      */
      order->item= &ref_pointer_array[counter];
      order->in_field_list=1;
      return FALSE;
    }
    else
    {
      /*
        There is a field with the same name in the FROM clause. This
        is the field that will be chosen. In this case we issue a
        warning so the user knows that the field from the FROM clause
        overshadows the column reference from the SELECT list.
      */
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_NON_UNIQ_ERROR,
                          ER_THD(thd, ER_NON_UNIQ_ERROR),
                          ((Item_ident*) order_item)->field_name.str,
                          thd->where);
    }
  }
  else if (from_window_spec)
  {
    Item **found_item= find_item_in_list(order_item, all_fields, &counter,
                                         REPORT_EXCEPT_NOT_FOUND, &resolution,
                                         all_fields.elements - fields.elements);
    if (found_item != not_found_item)
    {
      order->item= &ref_pointer_array[all_fields.elements-1-counter];
      order->in_field_list= 0;
      return FALSE;
    }
  }

  order->in_field_list=0;
  /*
    The call to order_item->fix_fields() means that here we resolve
    'order_item' to a column from a table in the list 'tables', or to
    a column in some outer query. Exactly because of the second case
    we come to this point even if (select_item == not_found_item),
    inspite of that fix_fields() calls find_item_in_list() one more
    time.

    We check order_item->fixed because Item_func_group_concat can put
    arguments for which fix_fields already was called.    
  */
  if (order_item->fix_fields_if_needed_for_order_by(thd, order->item) ||
      thd->is_error())
    return TRUE; /* Wrong field. */
  order_item= *order->item; // Item can change during fix_fields()

  if (!add_to_all_fields)
    return FALSE;

  uint el= all_fields.elements;
 /* Add new field to field list. */
  all_fields.push_front(order_item, thd->mem_root);
  ref_pointer_array[el]= order_item;
  /*
     If the order_item is a SUM_FUNC_ITEM, when fix_fields is called
     ref_by is set to order->item which is the address of order_item.
     But this needs to be address of order_item in the all_fields list.
     As a result, when it gets replaced with Item_aggregate_ref
     object in Item::split_sum_func2, we will be able to retrieve the
     newly created object.
  */
  if (order_item->type() == Item::SUM_FUNC_ITEM)
    ((Item_sum *)order_item)->ref_by= all_fields.head_ref();

  order->item= &ref_pointer_array[el];
  return FALSE;
}


/**
  Change order to point at item in select list.

  If item isn't a number and doesn't exits in the select list, add it the
  the field list.
*/

int setup_order(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
                List<Item> &fields, List<Item> &all_fields, ORDER *order,
                bool from_window_spec)
{ 
  SELECT_LEX *select = thd->lex->current_select;
  enum_parsing_place context_analysis_place=
                     thd->lex->current_select->context_analysis_place;
  thd->where="order clause";
  const bool for_union= select->master_unit()->is_unit_op() &&
    select == select->master_unit()->fake_select_lex;
  for (uint number = 1; order; order=order->next, number++)
  {
    if (find_order_in_list(thd, ref_pointer_array, tables, order, fields,
                           all_fields, false, true, from_window_spec))
      return 1;
    if ((*order->item)->with_window_func &&
        context_analysis_place != IN_ORDER_BY)
    {
      my_error(ER_WINDOW_FUNCTION_IN_WINDOW_SPEC, MYF(0));
      return 1;
    }

    /*
      UNION queries cannot be used with an aggregate function in
      an ORDER BY clause
    */

    if (for_union && (*order->item)->with_sum_func)
    {
      my_error(ER_AGGREGATE_ORDER_FOR_UNION, MYF(0), number);
      return 1;
    }

    if (from_window_spec && (*order->item)->with_sum_func &&
        (*order->item)->type() != Item::SUM_FUNC_ITEM)
      (*order->item)->split_sum_func(thd, ref_pointer_array,
                                     all_fields, SPLIT_SUM_SELECT);
  }
  return 0;
}


/**
  Intitialize the GROUP BY list.

  @param thd		       Thread handler
  @param ref_pointer_array     We store references to all fields that was
                               not in 'fields' here.
  @param fields		       All fields in the select part. Any item in
                               'order' that is part of these list is replaced
                               by a pointer to this fields.
  @param all_fields	       Total list of all unique fields used by the
                               select. All items in 'order' that was not part
                               of fields will be added first to this list.
  @param order		       The fields we should do GROUP/PARTITION BY on 
  @param hidden_group_fields   Pointer to flag that is set to 1 if we added
                               any fields to all_fields.
  @param from_window_spec      If true then list is from a window spec

  @todo
    change ER_WRONG_FIELD_WITH_GROUP to more detailed
    ER_NON_GROUPING_FIELD_USED

  @retval
    0  ok
  @retval
    1  error (probably out of memory)
*/

int
setup_group(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
	    List<Item> &fields, List<Item> &all_fields, ORDER *order,
	    bool *hidden_group_fields, bool from_window_spec)
{
  enum_parsing_place context_analysis_place=
                     thd->lex->current_select->context_analysis_place;
  *hidden_group_fields=0;
  ORDER *ord;

  if (!order)
    return 0;				/* Everything is ok */

  uint org_fields=all_fields.elements;

  thd->where="group statement";
  for (ord= order; ord; ord= ord->next)
  {
    if (find_order_in_list(thd, ref_pointer_array, tables, ord, fields,
                           all_fields, true, true, from_window_spec))
      return 1;
    (*ord->item)->marker= UNDEF_POS;		/* Mark found */
    if ((*ord->item)->with_sum_func && context_analysis_place == IN_GROUP_BY)
    {
      my_error(ER_WRONG_GROUP_FIELD, MYF(0), (*ord->item)->full_name());
      return 1;
    }
    if ((*ord->item)->with_window_func)
    {
      if (context_analysis_place == IN_GROUP_BY)
        my_error(ER_WRONG_PLACEMENT_OF_WINDOW_FUNCTION, MYF(0));
      else
        my_error(ER_WINDOW_FUNCTION_IN_WINDOW_SPEC, MYF(0));
      return 1;
    }
    if (from_window_spec && (*ord->item)->with_sum_func &&
        (*ord->item)->type() != Item::SUM_FUNC_ITEM)
      (*ord->item)->split_sum_func(thd, ref_pointer_array,
                                   all_fields, SPLIT_SUM_SELECT);
  }
  if (thd->variables.sql_mode & MODE_ONLY_FULL_GROUP_BY &&
      context_analysis_place == IN_GROUP_BY)
  {
    /*
      Don't allow one to use fields that is not used in GROUP BY
      For each select a list of field references that aren't under an
      aggregate function is created. Each field in this list keeps the
      position of the select list expression which it belongs to.

      First we check an expression from the select list against the GROUP BY
      list. If it's found there then it's ok. It's also ok if this expression
      is a constant or an aggregate function. Otherwise we scan the list
      of non-aggregated fields and if we'll find at least one field reference
      that belongs to this expression and doesn't occur in the GROUP BY list
      we throw an error. If there are no fields in the created list for a
      select list expression this means that all fields in it are used under
      aggregate functions.
    */
    Item *item;
    Item_field *field;
    int cur_pos_in_select_list= 0;
    List_iterator<Item> li(fields);
    List_iterator<Item_field> naf_it(thd->lex->current_select->join->non_agg_fields);

    field= naf_it++;
    while (field && (item=li++))
    {
      if (item->type() != Item::SUM_FUNC_ITEM && item->marker >= 0 &&
          !item->const_item() &&
          !(item->real_item()->type() == Item::FIELD_ITEM &&
            item->used_tables() & OUTER_REF_TABLE_BIT))
      {
        while (field)
        {
          /* Skip fields from previous expressions. */
          if (field->marker < cur_pos_in_select_list)
            goto next_field;
          /* Found a field from the next expression. */
          if (field->marker > cur_pos_in_select_list)
            break;
          /*
            Check whether the field occur in the GROUP BY list.
            Throw the error later if the field isn't found.
          */
          for (ord= order; ord; ord= ord->next)
            if ((*ord->item)->eq((Item*)field, 0))
              goto next_field;
          /*
            TODO: change ER_WRONG_FIELD_WITH_GROUP to more detailed
            ER_NON_GROUPING_FIELD_USED
          */
          my_error(ER_WRONG_FIELD_WITH_GROUP, MYF(0), field->full_name());
          return 1;
next_field:
          field= naf_it++;
        }
      }
      cur_pos_in_select_list++;
    }
  }
  if (org_fields != all_fields.elements)
    *hidden_group_fields=1;			// group fields is not used
  return 0;
}

/**
  Add fields with aren't used at start of field list.

  @return
    FALSE if ok
*/

static bool
setup_new_fields(THD *thd, List<Item> &fields,
		 List<Item> &all_fields, ORDER *new_field)
{
  Item	  **item;
  uint counter;
  enum_resolution_type not_used;
  DBUG_ENTER("setup_new_fields");

  thd->column_usage= MARK_COLUMNS_READ;       // Not really needed, but...
  for (; new_field ; new_field= new_field->next)
  {
    if ((item= find_item_in_list(*new_field->item, fields, &counter,
				 IGNORE_ERRORS, &not_used)))
      new_field->item=item;			/* Change to shared Item */
    else
    {
      thd->where="procedure list";
      if ((*new_field->item)->fix_fields(thd, new_field->item))
	DBUG_RETURN(1); /* purecov: inspected */
      all_fields.push_front(*new_field->item, thd->mem_root);
      new_field->item=all_fields.head_ref();
    }
  }
  DBUG_RETURN(0);
}

/**
  Create a group by that consist of all non const fields.

  Try to use the fields in the order given by 'order' to allow one to
  optimize away 'order by'.

  @retval
    0 OOM error if thd->is_fatal_error is set. Otherwise group was eliminated
    # Pointer to new group
*/

ORDER *
create_distinct_group(THD *thd, Ref_ptr_array ref_pointer_array,
                      ORDER *order_list, List<Item> &fields,
                      List<Item> &all_fields,
		      bool *all_order_by_fields_used)
{
  List_iterator<Item> li(fields);
  Item *item;
  Ref_ptr_array orig_ref_pointer_array= ref_pointer_array;
  ORDER *order,*group,**prev;
  uint idx= 0;

  *all_order_by_fields_used= 1;
  while ((item=li++))
    item->marker=0;			/* Marker that field is not used */

  prev= &group;  group=0;
  for (order=order_list ; order; order=order->next)
  {
    if (order->in_field_list)
    {
      ORDER *ord=(ORDER*) thd->memdup((char*) order,sizeof(ORDER));
      if (!ord)
	return 0;
      *prev=ord;
      prev= &ord->next;
      (*ord->item)->marker=1;
    }
    else
      *all_order_by_fields_used= 0;
  }

  li.rewind();
  while ((item=li++))
  {
    if (!item->const_item() && !item->with_sum_func && !item->marker)
    {
      /* 
        Don't put duplicate columns from the SELECT list into the 
        GROUP BY list.
      */
      ORDER *ord_iter;
      for (ord_iter= group; ord_iter; ord_iter= ord_iter->next)
        if ((*ord_iter->item)->eq(item, 1))
          goto next_item;
      
      ORDER *ord=(ORDER*) thd->calloc(sizeof(ORDER));
      if (!ord)
	return 0;

      if (item->type() == Item::FIELD_ITEM &&
          item->field_type() == MYSQL_TYPE_BIT)
      {
        /*
          Because HEAP tables can't index BIT fields we need to use an
          additional hidden field for grouping because later it will be
          converted to a LONG field. Original field will remain of the
          BIT type and will be returned [el]client.
        */
        Item_field *new_item= new (thd->mem_root) Item_field(thd, (Item_field*)item);
        if (!new_item)
          return 0;
        int el= all_fields.elements;
        orig_ref_pointer_array[el]= new_item;
        all_fields.push_front(new_item, thd->mem_root);
        ord->item=&orig_ref_pointer_array[el]; 
     }
      else
      {
        /*
          We have here only field_list (not all_field_list), so we can use
          simple indexing of ref_pointer_array (order in the array and in the
          list are same)
        */
        ord->item= &ref_pointer_array[idx];
      }
      ord->direction= ORDER::ORDER_ASC;
      *prev=ord;
      prev= &ord->next;
    }
next_item:
    idx++;
  }
  *prev=0;
  return group;
}


/**
  Update join with count of the different type of fields.
*/

void
count_field_types(SELECT_LEX *select_lex, TMP_TABLE_PARAM *param, 
                  List<Item> &fields, bool reset_with_sum_func)
{
  List_iterator<Item> li(fields);
  Item *field;

  param->field_count=param->sum_func_count=param->func_count=
    param->hidden_field_count=0;
  param->quick_group=1;
  while ((field=li++))
  {
    Item::Type real_type= field->real_item()->type();
    if (real_type == Item::FIELD_ITEM)
      param->field_count++;
    else if (real_type == Item::SUM_FUNC_ITEM)
    {
      if (! field->const_item())
      {
	Item_sum *sum_item=(Item_sum*) field->real_item();
        if (!sum_item->depended_from() ||
            sum_item->depended_from() == select_lex)
        {
          if (!sum_item->quick_group)
            param->quick_group=0;			// UDF SUM function
          param->sum_func_count++;

          for (uint i=0 ; i < sum_item->get_arg_count() ; i++)
          {
            if (sum_item->get_arg(i)->real_item()->type() == Item::FIELD_ITEM)
              param->field_count++;
            else
              param->func_count++;
          }
        }
        param->func_count++;
      }
    }
    else
    {
      param->func_count++;
      if (reset_with_sum_func)
	field->with_sum_func=0;
    }
  }
}


/**
  Return 1 if second is a subpart of first argument.

  If first parts has different direction, change it to second part
  (group is sorted like order)
*/

static bool
test_if_subpart(ORDER *a,ORDER *b)
{
  for (; a && b; a=a->next,b=b->next)
  {
    if ((*a->item)->eq(*b->item,1))
      a->direction=b->direction;
    else
      return 0;
  }
  return MY_TEST(!b);
}

/**
  Return table number if there is only one table in sort order
  and group and order is compatible, else return 0.
*/

static TABLE *
get_sort_by_table(ORDER *a,ORDER *b, List<TABLE_LIST> &tables, 
                  table_map const_tables)
{
  TABLE_LIST *table;
  List_iterator<TABLE_LIST> ti(tables);
  table_map map= (table_map) 0;
  DBUG_ENTER("get_sort_by_table");

  if (!a)
    a=b;					// Only one need to be given
  else if (!b)
    b=a;

  for (; a && b; a=a->next,b=b->next)
  {
    /* Skip elements of a that are constant */
    while (!((*a->item)->used_tables() & ~const_tables))
    {
      if (!(a= a->next))
        break;
    }

    /* Skip elements of b that are constant */
    while (!((*b->item)->used_tables() & ~const_tables))
    {
      if (!(b= b->next))
        break;
    }

    if (!a || !b)
      break;

    if (!(*a->item)->eq(*b->item,1))
      DBUG_RETURN(0);
    map|=a->item[0]->used_tables();
  }
  if (!map || (map & (RAND_TABLE_BIT | OUTER_REF_TABLE_BIT)))
    DBUG_RETURN(0);

  map&= ~const_tables;
  while ((table= ti++) && !(map & table->table->map)) ;
  if (map != table->table->map)
    DBUG_RETURN(0);				// More than one table
  DBUG_PRINT("exit",("sort by table: %d",table->table->tablenr));
  DBUG_RETURN(table->table);
}


/**
  calc how big buffer we need for comparing group entries.
*/

void calc_group_buffer(TMP_TABLE_PARAM *param, ORDER *group)
{
  uint key_length=0, parts=0, null_parts=0;

  for (; group ; group=group->next)
  {
    Item *group_item= *group->item;
    Field *field= group_item->get_tmp_table_field();
    if (field)
    {
      enum_field_types type;
      if ((type= field->type()) == MYSQL_TYPE_BLOB)
	key_length+=MAX_BLOB_WIDTH;		// Can't be used as a key
      else if (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_VAR_STRING)
        key_length+= field->field_length + HA_KEY_BLOB_LENGTH;
      else if (type == MYSQL_TYPE_BIT)
      {
        /* Bit is usually stored as a longlong key for group fields */
        key_length+= 8;                         // Big enough
      }
      else
	key_length+= field->pack_length();
    }
    else
    { 
      switch (group_item->cmp_type()) {
      case REAL_RESULT:
        key_length+= sizeof(double);
        break;
      case INT_RESULT:
        key_length+= sizeof(longlong);
        break;
      case DECIMAL_RESULT:
        key_length+= my_decimal_get_binary_size(group_item->max_length - 
                                                (group_item->decimals ? 1 : 0),
                                                group_item->decimals);
        break;
      case TIME_RESULT:
      {
        /*
          As items represented as DATE/TIME fields in the group buffer
          have STRING_RESULT result type, we increase the length 
          by 8 as maximum pack length of such fields.
        */
        key_length+= 8;
        break;
      }
      case STRING_RESULT:
      {
        enum enum_field_types type= group_item->field_type();
        if (type == MYSQL_TYPE_BLOB)
          key_length+= MAX_BLOB_WIDTH;		// Can't be used as a key
        else
        {
          /*
            Group strings are taken as varstrings and require an length field.
            A field is not yet created by create_tmp_field()
            and the sizes should match up.
          */
          key_length+= group_item->max_length + HA_KEY_BLOB_LENGTH;
        }
        break;
      }
      default:
        /* This case should never be choosen */
        DBUG_ASSERT(0);
        my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATALERROR));
      }
    }
    parts++;
    if (group_item->maybe_null)
      null_parts++;
  }
  param->group_length= key_length + null_parts;
  param->group_parts= parts;
  param->group_null_parts= null_parts;
}

static void calc_group_buffer(JOIN *join, ORDER *group)
{
  if (group)
    join->group= 1;
  calc_group_buffer(&join->tmp_table_param, group);
}


/**
  allocate group fields or take prepared (cached).

  @param main_join   join of current select
  @param curr_join   current join (join of current select or temporary copy
                     of it)

  @retval
    0   ok
  @retval
    1   failed
*/

static bool
make_group_fields(JOIN *main_join, JOIN *curr_join)
{
  if (main_join->group_fields_cache.elements)
  {
    curr_join->group_fields= main_join->group_fields_cache;
    curr_join->sort_and_group= 1;
  }
  else
  {
    if (alloc_group_fields(curr_join, curr_join->group_list))
      return (1);
    main_join->group_fields_cache= curr_join->group_fields;
  }
  return (0);
}


/**
  Get a list of buffers for saving last group.

  Groups are saved in reverse order for easier check loop.
*/

static bool
alloc_group_fields(JOIN *join,ORDER *group)
{
  if (group)
  {
    for (; group ; group=group->next)
    {
      Cached_item *tmp=new_Cached_item(join->thd, *group->item, TRUE);
      if (!tmp || join->group_fields.push_front(tmp))
	return TRUE;
    }
  }
  join->sort_and_group=1;			/* Mark for do_select */
  return FALSE;
}



/*
  Test if a single-row cache of items changed, and update the cache.

  @details Test if a list of items that typically represents a result
  row has changed. If the value of some item changed, update the cached
  value for this item.
  
  @param list list of <item, cached_value> pairs stored as Cached_item.

  @return -1 if no item changed
  @return index of the first item that changed
*/

int test_if_item_cache_changed(List<Cached_item> &list)
{
  DBUG_ENTER("test_if_item_cache_changed");
  List_iterator<Cached_item> li(list);
  int idx= -1,i;
  Cached_item *buff;

  for (i=(int) list.elements-1 ; (buff=li++) ; i--)
  {
    if (buff->cmp())
      idx=i;
  }
  DBUG_PRINT("info", ("idx: %d", idx));
  DBUG_RETURN(idx);
}


/*
  @return
    -1         - Group not changed
   value>=0    - Number of the component where the group changed
*/

int
test_if_group_changed(List<Cached_item> &list)
{
  DBUG_ENTER("test_if_group_changed");
  List_iterator<Cached_item> li(list);
  int idx= -1,i;
  Cached_item *buff;

  for (i=(int) list.elements-1 ; (buff=li++) ; i--)
  {
    if (buff->cmp())
      idx=i;
  }
  DBUG_PRINT("info", ("idx: %d", idx));
  DBUG_RETURN(idx);
}


/**
  Setup copy_fields to save fields at start of new group.

  Setup copy_fields to save fields at start of new group

  Only FIELD_ITEM:s and FUNC_ITEM:s needs to be saved between groups.
  Change old item_field to use a new field with points at saved fieldvalue
  This function is only called before use of send_result_set_metadata.

  @param thd                   THD pointer
  @param param                 temporary table parameters
  @param ref_pointer_array     array of pointers to top elements of filed list
  @param res_selected_fields   new list of items of select item list
  @param res_all_fields        new list of all items
  @param elements              number of elements in select item list
  @param all_fields            all fields list

  @todo
    In most cases this result will be sent to the user.
    This should be changed to use copy_int or copy_real depending
    on how the value is to be used: In some cases this may be an
    argument in a group function, like: IF(ISNULL(col),0,COUNT(*))

  @retval
    0     ok
  @retval
    !=0   error
*/

bool
setup_copy_fields(THD *thd, TMP_TABLE_PARAM *param,
		  Ref_ptr_array ref_pointer_array,
		  List<Item> &res_selected_fields, List<Item> &res_all_fields,
		  uint elements, List<Item> &all_fields)
{
  Item *pos;
  List_iterator_fast<Item> li(all_fields);
  Copy_field *copy= NULL;
  Copy_field *copy_start __attribute__((unused));
  res_selected_fields.empty();
  res_all_fields.empty();
  List_iterator_fast<Item> itr(res_all_fields);
  List<Item> extra_funcs;
  uint i, border= all_fields.elements - elements;
  DBUG_ENTER("setup_copy_fields");

  if (param->field_count && 
      !(copy=param->copy_field= new (thd->mem_root) Copy_field[param->field_count]))
    goto err2;

  param->copy_funcs.empty();
  copy_start= copy;
  for (i= 0; (pos= li++); i++)
  {
    Field *field;
    uchar *tmp;
    Item *real_pos= pos->real_item();
    /*
      Aggregate functions can be substituted for fields (by e.g. temp tables).
      We need to filter those substituted fields out.
    */
    if (real_pos->type() == Item::FIELD_ITEM &&
        !(real_pos != pos &&
          ((Item_ref *)pos)->ref_type() == Item_ref::AGGREGATE_REF))
    {
      Item_field *item;
      if (!(item= new (thd->mem_root) Item_field(thd, ((Item_field*) real_pos))))
	goto err;
      if (pos->type() == Item::REF_ITEM)
      {
        /* preserve the names of the ref when dereferncing */
        Item_ref *ref= (Item_ref *) pos;
        item->db_name= ref->db_name;
        item->table_name= ref->table_name;
        item->name= ref->name;
      }
      pos= item;
      if (item->field->flags & BLOB_FLAG)
      {
	if (!(pos= new (thd->mem_root) Item_copy_string(thd, pos)))
	  goto err;
       /*
         Item_copy_string::copy for function can call 
         Item_copy_string::val_int for blob via Item_ref.
         But if Item_copy_string::copy for blob isn't called before,
         it's value will be wrong
         so let's insert Item_copy_string for blobs in the beginning of 
         copy_funcs
         (to see full test case look at having.test, BUG #4358) 
       */
	if (param->copy_funcs.push_front(pos, thd->mem_root))
	  goto err;
      }
      else
      {
	/* 
	   set up save buffer and change result_field to point at 
	   saved value
	*/
	field= item->field;
	item->result_field=field->make_new_field(thd->mem_root,
                                                 field->table, 1);
        /*
          We need to allocate one extra byte for null handling and
          another extra byte to not get warnings from purify in
          Field_string::val_int
        */
	if (!(tmp= (uchar*) thd->alloc(field->pack_length()+2)))
	  goto err;
        if (copy)
        {
          DBUG_ASSERT (param->field_count > (uint) (copy - copy_start));
          copy->set(tmp, item->result_field);
          item->result_field->move_field(copy->to_ptr,copy->to_null_ptr,1);
#ifdef HAVE_valgrind
          copy->to_ptr[copy->from_length]= 0;
#endif
          copy++;
        }
      }
    }
    else if ((real_pos->type() == Item::FUNC_ITEM ||
	      real_pos->real_type() == Item::SUBSELECT_ITEM ||
	      real_pos->type() == Item::CACHE_ITEM ||
	      real_pos->type() == Item::COND_ITEM) &&
	     !real_pos->with_sum_func)
    {						// Save for send fields
      LEX_CSTRING real_name= pos->name;
      pos= real_pos;
      pos->name= real_name;
      /* TODO:
	 In most cases this result will be sent to the user.
	 This should be changed to use copy_int or copy_real depending
	 on how the value is to be used: In some cases this may be an
	 argument in a group function, like: IF(ISNULL(col),0,COUNT(*))
      */
      if (!(pos=new (thd->mem_root) Item_copy_string(thd, pos)))
	goto err;
      if (i < border)                           // HAVING, ORDER and GROUP BY
      {
        if (extra_funcs.push_back(pos, thd->mem_root))
          goto err;
      }
      else if (param->copy_funcs.push_back(pos, thd->mem_root))
	goto err;
    }
    res_all_fields.push_back(pos, thd->mem_root);
    ref_pointer_array[((i < border)? all_fields.elements-i-1 : i-border)]=
      pos;
  }
  param->copy_field_end= copy;

  for (i= 0; i < border; i++)
    itr++;
  itr.sublist(res_selected_fields, elements);
  /*
    Put elements from HAVING, ORDER BY and GROUP BY last to ensure that any
    reference used in these will resolve to a item that is already calculated
  */
  param->copy_funcs.append(&extra_funcs);

  DBUG_RETURN(0);

 err:
  if (copy)
    delete [] param->copy_field;			// This is never 0
  param->copy_field= 0;
err2:
  DBUG_RETURN(TRUE);
}


/**
  Make a copy of all simple SELECT'ed items.

  This is done at the start of a new group so that we can retrieve
  these later when the group changes.
*/

void
copy_fields(TMP_TABLE_PARAM *param)
{
  Copy_field *ptr=param->copy_field;
  Copy_field *end=param->copy_field_end;

  DBUG_ASSERT((ptr != NULL && end >= ptr) || (ptr == NULL && end == NULL));

  for (; ptr != end; ptr++)
    (*ptr->do_copy)(ptr);

  List_iterator_fast<Item> it(param->copy_funcs);
  Item_copy_string *item;
  while ((item = (Item_copy_string*) it++))
    item->copy();
}


/**
  Make an array of pointers to sum_functions to speed up
  sum_func calculation.

  @retval
    0	ok
  @retval
    1	Error
*/

bool JOIN::alloc_func_list()
{
  uint func_count, group_parts;
  DBUG_ENTER("alloc_func_list");

  func_count= tmp_table_param.sum_func_count;
  /*
    If we are using rollup, we need a copy of the summary functions for
    each level
  */
  if (rollup.state != ROLLUP::STATE_NONE)
    func_count*= (send_group_parts+1);

  group_parts= send_group_parts;
  /*
    If distinct, reserve memory for possible
    disctinct->group_by optimization
  */
  if (select_distinct)
  {
    group_parts+= fields_list.elements;
    /*
      If the ORDER clause is specified then it's possible that
      it also will be optimized, so reserve space for it too
    */
    if (order)
    {
      ORDER *ord;
      for (ord= order; ord; ord= ord->next)
        group_parts++;
    }
  }

  /* This must use calloc() as rollup_make_fields depends on this */
  sum_funcs= (Item_sum**) thd->calloc(sizeof(Item_sum**) * (func_count+1) +
				      sizeof(Item_sum***) * (group_parts+1));
  sum_funcs_end= (Item_sum***) (sum_funcs+func_count+1);
  DBUG_RETURN(sum_funcs == 0);
}


/**
  Initialize 'sum_funcs' array with all Item_sum objects.

  @param field_list        All items
  @param send_result_set_metadata       Items in select list
  @param before_group_by   Set to 1 if this is called before GROUP BY handling
  @param recompute         Set to TRUE if sum_funcs must be recomputed

  @retval
    0  ok
  @retval
    1  error
*/

bool JOIN::make_sum_func_list(List<Item> &field_list,
                              List<Item> &send_result_set_metadata,
			      bool before_group_by, bool recompute)
{
  List_iterator_fast<Item> it(field_list);
  Item_sum **func;
  Item *item;
  DBUG_ENTER("make_sum_func_list");

  if (*sum_funcs && !recompute)
    DBUG_RETURN(FALSE); /* We have already initialized sum_funcs. */

  func= sum_funcs;
  while ((item=it++))
  {
    if (item->type() == Item::SUM_FUNC_ITEM && !item->const_item() &&
        (!((Item_sum*) item)->depended_from() ||
         ((Item_sum *)item)->depended_from() == select_lex))
      *func++= (Item_sum*) item;
  }
  if (before_group_by && rollup.state == ROLLUP::STATE_INITED)
  {
    rollup.state= ROLLUP::STATE_READY;
    if (rollup_make_fields(field_list, send_result_set_metadata, &func))
      DBUG_RETURN(TRUE);			// Should never happen
  }
  else if (rollup.state == ROLLUP::STATE_NONE)
  {
    for (uint i=0 ; i <= send_group_parts ;i++)
      sum_funcs_end[i]= func;
  }
  else if (rollup.state == ROLLUP::STATE_READY)
    DBUG_RETURN(FALSE);                         // Don't put end marker
  *func=0;					// End marker
  DBUG_RETURN(FALSE);
}


/**
  Change all funcs and sum_funcs to fields in tmp table, and create
  new list of all items.

  @param thd                   THD pointer
  @param ref_pointer_array     array of pointers to top elements of filed list
  @param res_selected_fields   new list of items of select item list
  @param res_all_fields        new list of all items
  @param elements              number of elements in select item list
  @param all_fields            all fields list

  @retval
    0     ok
  @retval
    !=0   error
*/

static bool
change_to_use_tmp_fields(THD *thd, Ref_ptr_array ref_pointer_array,
			 List<Item> &res_selected_fields,
			 List<Item> &res_all_fields,
			 uint elements, List<Item> &all_fields)
{
  List_iterator_fast<Item> it(all_fields);
  Item *item_field,*item;
  DBUG_ENTER("change_to_use_tmp_fields");

  res_selected_fields.empty();
  res_all_fields.empty();

  uint border= all_fields.elements - elements;
  for (uint i= 0; (item= it++); i++)
  {
    Field *field;
    if ((item->with_sum_func && item->type() != Item::SUM_FUNC_ITEM) ||
       item->with_window_func)
      item_field= item;
    else if (item->type() == Item::FIELD_ITEM)
    {
      if (!(item_field= item->get_tmp_table_item(thd)))
        DBUG_RETURN(true);
    }
    else if (item->type() == Item::FUNC_ITEM &&
             ((Item_func*)item)->functype() == Item_func::SUSERVAR_FUNC)
    {
      field= item->get_tmp_table_field();
      if (field != NULL)
      {
        /*
          Replace "@:=<expression>" with "@:=<tmp table
          column>". Otherwise, we would re-evaluate <expression>, and
          if expression were a subquery, this would access
          already-unlocked tables.
         */
        Item_func_set_user_var* suv=
          new (thd->mem_root) Item_func_set_user_var(thd, (Item_func_set_user_var*) item);
        Item_field *new_field= new (thd->mem_root) Item_temptable_field(thd, field);
        if (!suv || !new_field)
          DBUG_RETURN(true);                  // Fatal error
        List<Item> list;
        list.push_back(new_field, thd->mem_root);
        suv->set_arguments(thd, list);
        item_field= suv;
      }
      else
        item_field= item;
    }
    else if ((field= item->get_tmp_table_field()))
    {
      if (item->type() == Item::SUM_FUNC_ITEM && field->table->group)
        item_field= ((Item_sum*) item)->result_item(thd, field);
      else
        item_field= (Item *) new (thd->mem_root) Item_temptable_field(thd, field);
      if (!item_field)
        DBUG_RETURN(true);                    // Fatal error

      if (item->real_item()->type() != Item::FIELD_ITEM)
        field->orig_table= 0;
      item_field->name= item->name;
      if (item->type() == Item::REF_ITEM)
      {
        Item_field *ifield= (Item_field *) item_field;
        Item_ref *iref= (Item_ref *) item;
        ifield->table_name= iref->table_name;
        ifield->db_name= iref->db_name;
      }
#ifndef DBUG_OFF
      if (!item_field->name.str)
      {
        char buff[256];
        String str(buff,sizeof(buff),&my_charset_bin);
        str.length(0);
        str.extra_allocation(1024);
        item->print(&str, QT_ORDINARY);
        item_field->name.str= thd->strmake(str.ptr(), str.length());
        item_field->name.length= str.length();
      }
#endif
    }
    else
      item_field= item;

    res_all_fields.push_back(item_field, thd->mem_root);
    ref_pointer_array[((i < border)? all_fields.elements-i-1 : i-border)]=
      item_field;
  }

  List_iterator_fast<Item> itr(res_all_fields);
  for (uint i= 0; i < border; i++)
    itr++;
  itr.sublist(res_selected_fields, elements);
  DBUG_RETURN(false);
}


/**
  Change all sum_func refs to fields to point at fields in tmp table.
  Change all funcs to be fields in tmp table.

  @param thd                   THD pointer
  @param ref_pointer_array     array of pointers to top elements of filed list
  @param res_selected_fields   new list of items of select item list
  @param res_all_fields        new list of all items
  @param elements              number of elements in select item list
  @param all_fields            all fields list

  @retval
    0	ok
  @retval
    1	error
*/

static bool
change_refs_to_tmp_fields(THD *thd, Ref_ptr_array ref_pointer_array,
			  List<Item> &res_selected_fields,
			  List<Item> &res_all_fields, uint elements,
			  List<Item> &all_fields)
{
  List_iterator_fast<Item> it(all_fields);
  Item *item, *new_item;
  res_selected_fields.empty();
  res_all_fields.empty();

  uint i, border= all_fields.elements - elements;
  for (i= 0; (item= it++); i++)
  {
    if (item->type() == Item::SUM_FUNC_ITEM && item->const_item())
      new_item= item;
    else
    {
      if (!(new_item= item->get_tmp_table_item(thd)))
        return 1;
    }

    if (res_all_fields.push_back(new_item, thd->mem_root))
      return 1;
    ref_pointer_array[((i < border)? all_fields.elements-i-1 : i-border)]=
      new_item;
  }

  List_iterator_fast<Item> itr(res_all_fields);
  for (i= 0; i < border; i++)
    itr++;
  itr.sublist(res_selected_fields, elements);

  return thd->is_fatal_error;
}



/******************************************************************************
  Code for calculating functions
******************************************************************************/


/**
  Call ::setup for all sum functions.

  @param thd           thread handler
  @param func_ptr      sum function list

  @retval
    FALSE  ok
  @retval
    TRUE   error
*/

static bool setup_sum_funcs(THD *thd, Item_sum **func_ptr)
{
  Item_sum *func;
  DBUG_ENTER("setup_sum_funcs");
  while ((func= *(func_ptr++)))
  {
    if (func->aggregator_setup(thd))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


static bool prepare_sum_aggregators(Item_sum **func_ptr, bool need_distinct)
{
  Item_sum *func;
  DBUG_ENTER("prepare_sum_aggregators");
  while ((func= *(func_ptr++)))
  {
    if (func->set_aggregator(need_distinct && func->has_with_distinct() ?
                             Aggregator::DISTINCT_AGGREGATOR :
                             Aggregator::SIMPLE_AGGREGATOR))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


static void
init_tmptable_sum_functions(Item_sum **func_ptr)
{
  Item_sum *func;
  while ((func= *(func_ptr++)))
    func->reset_field();
}


/** Update record 0 in tmp_table from record 1. */

static void
update_tmptable_sum_func(Item_sum **func_ptr,
			 TABLE *tmp_table __attribute__((unused)))
{
  Item_sum *func;
  while ((func= *(func_ptr++)))
    func->update_field();
}


/** Copy result of sum functions to record in tmp_table. */

static void
copy_sum_funcs(Item_sum **func_ptr, Item_sum **end_ptr)
{
  for (; func_ptr != end_ptr ; func_ptr++)
    (void) (*func_ptr)->save_in_result_field(1);
  return;
}


static bool
init_sum_functions(Item_sum **func_ptr, Item_sum **end_ptr)
{
  for (; func_ptr != end_ptr ;func_ptr++)
  {
    if ((*func_ptr)->reset_and_add())
      return 1;
  }
  /* If rollup, calculate the upper sum levels */
  for ( ; *func_ptr ; func_ptr++)
  {
    if ((*func_ptr)->aggregator_add())
      return 1;
  }
  return 0;
}


static bool
update_sum_func(Item_sum **func_ptr)
{
  Item_sum *func;
  for (; (func= (Item_sum*) *func_ptr) ; func_ptr++)
    if (func->aggregator_add())
      return 1;
  return 0;
}

/** 
  Copy result of functions to record in tmp_table. 

  Uses the thread pointer to check for errors in 
  some of the val_xxx() methods called by the 
  save_in_result_field() function.
  TODO: make the Item::val_xxx() return error code

  @param func_ptr  array of the function Items to copy to the tmp table
  @param thd       pointer to the current thread for error checking
  @retval
    FALSE if OK
  @retval
    TRUE on error  
*/

bool
copy_funcs(Item **func_ptr, const THD *thd)
{
  Item *func;
  for (; (func = *func_ptr) ; func_ptr++)
  {
    if (func->type() == Item::FUNC_ITEM &&
        ((Item_func *) func)->with_window_func)
      continue;
    func->save_in_result_field(1);
    /*
      Need to check the THD error state because Item::val_xxx() don't
      return error code, but can generate errors
      TODO: change it for a real status check when Item::val_xxx()
      are extended to return status code.
    */  
    if (unlikely(thd->is_error()))
      return TRUE;
  }
  return FALSE;
}


/**
  Create a condition for a const reference and add this to the
  currenct select for the table.
*/

static bool add_ref_to_table_cond(THD *thd, JOIN_TAB *join_tab)
{
  DBUG_ENTER("add_ref_to_table_cond");
  if (!join_tab->ref.key_parts)
    DBUG_RETURN(FALSE);

  Item_cond_and *cond= new (thd->mem_root) Item_cond_and(thd);
  TABLE *table=join_tab->table;
  int error= 0;
  if (!cond)
    DBUG_RETURN(TRUE);

  for (uint i=0 ; i < join_tab->ref.key_parts ; i++)
  {
    Field *field=table->field[table->key_info[join_tab->ref.key].key_part[i].
			      fieldnr-1];
    Item *value=join_tab->ref.items[i];
    cond->add(new (thd->mem_root)
              Item_func_equal(thd, new (thd->mem_root) Item_field(thd, field),
                              value),
              thd->mem_root);
  }
  if (unlikely(thd->is_fatal_error))
    DBUG_RETURN(TRUE);
  if (!cond->fixed)
  {
    Item *tmp_item= (Item*) cond;
    cond->fix_fields(thd, &tmp_item);
    DBUG_ASSERT(cond == tmp_item);
  }
  if (join_tab->select)
  {
    Item *UNINIT_VAR(cond_copy);
    if (join_tab->select->pre_idx_push_select_cond)
      cond_copy= cond->copy_andor_structure(thd);
    if (join_tab->select->cond)
      error=(int) cond->add(join_tab->select->cond, thd->mem_root);
    join_tab->select->cond= cond;
    if (join_tab->select->pre_idx_push_select_cond)
    {
      Item *new_cond= and_conds(thd, cond_copy,
                                join_tab->select->pre_idx_push_select_cond);
      if (new_cond->fix_fields_if_needed(thd, &new_cond))
        error= 1;
      join_tab->pre_idx_push_select_cond=
        join_tab->select->pre_idx_push_select_cond= new_cond;
    }
    join_tab->set_select_cond(cond, __LINE__);
  }
  else if ((join_tab->select= make_select(join_tab->table, 0, 0, cond,
                                          (SORT_INFO*) 0, 0, &error)))
    join_tab->set_select_cond(cond, __LINE__);

  DBUG_RETURN(error ? TRUE : FALSE);
}


/**
  Free joins of subselect of this select.

  @param thd      THD pointer
  @param select   pointer to st_select_lex which subselects joins we will free
*/

void free_underlaid_joins(THD *thd, SELECT_LEX *select)
{
  for (SELECT_LEX_UNIT *unit= select->first_inner_unit();
       unit;
       unit= unit->next_unit())
    unit->cleanup();
}

/****************************************************************************
  ROLLUP handling
****************************************************************************/

/**
  Replace occurences of group by fields in an expression by ref items.

  The function replaces occurrences of group by fields in expr
  by ref objects for these fields unless they are under aggregate
  functions.
  The function also corrects value of the the maybe_null attribute
  for the items of all subexpressions containing group by fields.

  @b EXAMPLES
    @code
      SELECT a+1 FROM t1 GROUP BY a WITH ROLLUP
      SELECT SUM(a)+a FROM t1 GROUP BY a WITH ROLLUP 
  @endcode

  @b IMPLEMENTATION

    The function recursively traverses the tree of the expr expression,
    looks for occurrences of the group by fields that are not under
    aggregate functions and replaces them for the corresponding ref items.

  @note
    This substitution is needed GROUP BY queries with ROLLUP if
    SELECT list contains expressions over group by attributes.

  @param thd                  reference to the context
  @param expr                 expression to make replacement
  @param group_list           list of references to group by items
  @param changed        out:  returns 1 if item contains a replaced field item

  @todo
    - TODO: Some functions are not null-preserving. For those functions
    updating of the maybe_null attribute is an overkill. 

  @retval
    0	if ok
  @retval
    1   on error
*/

static bool change_group_ref(THD *thd, Item_func *expr, ORDER *group_list,
                             bool *changed)
{
  if (expr->argument_count())
  {
    Name_resolution_context *context= &thd->lex->current_select->context;
    Item **arg,**arg_end;
    bool arg_changed= FALSE;
    for (arg= expr->arguments(),
         arg_end= expr->arguments() + expr->argument_count();
         arg != arg_end; arg++)
    {
      Item *item= *arg;
      if (item->type() == Item::FIELD_ITEM || item->type() == Item::REF_ITEM)
      {
        ORDER *group_tmp;
        for (group_tmp= group_list; group_tmp; group_tmp= group_tmp->next)
        {
          if (item->eq(*group_tmp->item,0))
          {
            Item *new_item;
            if (!(new_item= new (thd->mem_root) Item_ref(thd, context,
                                                         group_tmp->item, 0,
                                                         &item->name)))
              return 1;                                 // fatal_error is set
            thd->change_item_tree(arg, new_item);
            arg_changed= TRUE;
          }
        }
      }
      else if (item->type() == Item::FUNC_ITEM)
      {
        if (change_group_ref(thd, (Item_func *) item, group_list, &arg_changed))
          return 1;
      }
    }
    if (arg_changed)
    {
      expr->maybe_null= 1;
      expr->in_rollup= 1;
      *changed= TRUE;
    }
  }
  return 0;
}


/** Allocate memory needed for other rollup functions. */

bool JOIN::rollup_init()
{
  uint i,j;
  Item **ref_array;

  tmp_table_param.quick_group= 0;	// Can't create groups in tmp table
  rollup.state= ROLLUP::STATE_INITED;

  /*
    Create pointers to the different sum function groups
    These are updated by rollup_make_fields()
  */
  tmp_table_param.group_parts= send_group_parts;

  Item_null_result **null_items=
    static_cast<Item_null_result**>(thd->alloc(sizeof(Item*)*send_group_parts));

  rollup.null_items= Item_null_array(null_items, send_group_parts);
  rollup.ref_pointer_arrays=
    static_cast<Ref_ptr_array*>
    (thd->alloc((sizeof(Ref_ptr_array) +
                 all_fields.elements * sizeof(Item*)) * send_group_parts));
  rollup.fields=
    static_cast<List<Item>*>(thd->alloc(sizeof(List<Item>) * send_group_parts));

  if (!null_items || !rollup.ref_pointer_arrays || !rollup.fields)
    return true;

  ref_array= (Item**) (rollup.ref_pointer_arrays+send_group_parts);


  /*
    Prepare space for field list for the different levels
    These will be filled up in rollup_make_fields()
  */
  for (i= 0 ; i < send_group_parts ; i++)
  {
    if (!(rollup.null_items[i]= new (thd->mem_root) Item_null_result(thd)))
      return true;

    List<Item> *rollup_fields= &rollup.fields[i];
    rollup_fields->empty();
    rollup.ref_pointer_arrays[i]= Ref_ptr_array(ref_array, all_fields.elements);
    ref_array+= all_fields.elements;
  }
  for (i= 0 ; i < send_group_parts; i++)
  {
    for (j=0 ; j < fields_list.elements ; j++)
      rollup.fields[i].push_back(rollup.null_items[i], thd->mem_root);
  }
  List_iterator<Item> it(all_fields);
  Item *item;
  while ((item= it++))
  {
    ORDER *group_tmp;
    bool found_in_group= 0;

    for (group_tmp= group_list; group_tmp; group_tmp= group_tmp->next)
    {
      if (*group_tmp->item == item)
      {
        item->maybe_null= 1;
        item->in_rollup= 1;
        found_in_group= 1;
        break;
      }
    }
    if (item->type() == Item::FUNC_ITEM && !found_in_group)
    {
      bool changed= FALSE;
      if (change_group_ref(thd, (Item_func *) item, group_list, &changed))
        return 1;
      /*
        We have to prevent creation of a field in a temporary table for
        an expression that contains GROUP BY attributes.
        Marking the expression item as 'with_sum_func' will ensure this.
      */ 
      if (changed)
        item->with_sum_func= 1;
    }
  }
  return 0;
}

/**
   Wrap all constant Items in GROUP BY list.

   For ROLLUP queries each constant item referenced in GROUP BY list
   is wrapped up into an Item_func object yielding the same value
   as the constant item. The objects of the wrapper class are never
   considered as constant items and besides they inherit all
   properties of the Item_result_field class.
   This wrapping allows us to ensure writing constant items
   into temporary tables whenever the result of the ROLLUP
   operation has to be written into a temporary table, e.g. when
   ROLLUP is used together with DISTINCT in the SELECT list.
   Usually when creating temporary tables for a intermidiate
   result we do not include fields for constant expressions.

   @retval
     0  if ok
   @retval
     1  on error
*/

bool JOIN::rollup_process_const_fields()
{
  ORDER *group_tmp;
  Item *item;
  List_iterator<Item> it(all_fields);

  for (group_tmp= group_list; group_tmp; group_tmp= group_tmp->next)
  {
    if (!(*group_tmp->item)->const_item())
      continue;
    while ((item= it++))
    {
      if (*group_tmp->item == item)
      {
        Item* new_item= new (thd->mem_root) Item_func_rollup_const(thd, item);
        if (!new_item)
          return 1;
        new_item->fix_fields(thd, (Item **) 0);
        thd->change_item_tree(it.ref(), new_item);
        for (ORDER *tmp= group_tmp; tmp; tmp= tmp->next)
        {
          if (*tmp->item == item)
            thd->change_item_tree(tmp->item, new_item);
        }
        break;
      }
    }
    it.rewind();
  }
  return 0;
}
  

/**
  Fill up rollup structures with pointers to fields to use.

  Creates copies of item_sum items for each sum level.

  @param fields_arg		List of all fields (hidden and real ones)
  @param sel_fields		Pointer to selected fields
  @param func			Store here a pointer to all fields

  @retval
    0	if ok;
    In this case func is pointing to next not used element.
  @retval
    1    on error
*/

bool JOIN::rollup_make_fields(List<Item> &fields_arg, List<Item> &sel_fields,
			      Item_sum ***func)
{
  List_iterator_fast<Item> it(fields_arg);
  Item *first_field= sel_fields.head();
  uint level;

  /*
    Create field lists for the different levels

    The idea here is to have a separate field list for each rollup level to
    avoid all runtime checks of which columns should be NULL.

    The list is stored in reverse order to get sum function in such an order
    in func that it makes it easy to reset them with init_sum_functions()

    Assuming:  SELECT a, b, c SUM(b) FROM t1 GROUP BY a,b WITH ROLLUP

    rollup.fields[0] will contain list where a,b,c is NULL
    rollup.fields[1] will contain list where b,c is NULL
    ...
    rollup.ref_pointer_array[#] points to fields for rollup.fields[#]
    ...
    sum_funcs_end[0] points to all sum functions
    sum_funcs_end[1] points to all sum functions, except grand totals
    ...
  */

  for (level=0 ; level < send_group_parts ; level++)
  {
    uint i;
    uint pos= send_group_parts - level -1;
    bool real_fields= 0;
    Item *item;
    List_iterator<Item> new_it(rollup.fields[pos]);
    Ref_ptr_array ref_array_start= rollup.ref_pointer_arrays[pos];
    ORDER *start_group;

    /* Point to first hidden field */
    uint ref_array_ix= fields_arg.elements-1;


    /* Remember where the sum functions ends for the previous level */
    sum_funcs_end[pos+1]= *func;

    /* Find the start of the group for this level */
    for (i= 0, start_group= group_list ;
	 i++ < pos ;
	 start_group= start_group->next)
      ;

    it.rewind();
    while ((item= it++))
    {
      if (item == first_field)
      {
	real_fields= 1;				// End of hidden fields
	ref_array_ix= 0;
      }

      if (item->type() == Item::SUM_FUNC_ITEM && !item->const_item() &&
          (!((Item_sum*) item)->depended_from() ||
           ((Item_sum *)item)->depended_from() == select_lex))
          
      {
	/*
	  This is a top level summary function that must be replaced with
	  a sum function that is reset for this level.

	  NOTE: This code creates an object which is not that nice in a
	  sub select.  Fortunately it's not common to have rollup in
	  sub selects.
	*/
	item= item->copy_or_same(thd);
	((Item_sum*) item)->make_unique();
	*(*func)= (Item_sum*) item;
	(*func)++;
      }
      else 
      {
	/* Check if this is something that is part of this group by */
	ORDER *group_tmp;
	for (group_tmp= start_group, i= pos ;
             group_tmp ; group_tmp= group_tmp->next, i++)
	{
          if (*group_tmp->item == item)
	  {
	    /*
	      This is an element that is used by the GROUP BY and should be
	      set to NULL in this level
	    */
            Item_null_result *null_item= new (thd->mem_root) Item_null_result(thd);
            if (!null_item)
              return 1;
	    item->maybe_null= 1;		// Value will be null sometimes
            null_item->result_field= item->get_tmp_table_field();
            item= null_item;
	    break;
	  }
	}
      }
      ref_array_start[ref_array_ix]= item;
      if (real_fields)
      {
	(void) new_it++;			// Point to next item
	new_it.replace(item);			// Replace previous
	ref_array_ix++;
      }
      else
	ref_array_ix--;
    }
  }
  sum_funcs_end[0]= *func;			// Point to last function
  return 0;
}

/**
  Send all rollup levels higher than the current one to the client.

  @b SAMPLE
    @code
      SELECT a, b, c SUM(b) FROM t1 GROUP BY a,b WITH ROLLUP
  @endcode

  @param idx		Level we are on:
                        - 0 = Total sum level
                        - 1 = First group changed  (a)
                        - 2 = Second group changed (a,b)

  @retval
    0   ok
  @retval
    1   If send_data_failed()
*/

int JOIN::rollup_send_data(uint idx)
{
  uint i;
  for (i= send_group_parts ; i-- > idx ; )
  {
    int res= 0;
    /* Get reference pointers to sum functions in place */
    copy_ref_ptr_array(ref_ptrs, rollup.ref_pointer_arrays[i]);
    if ((!having || having->val_int()))
    {
      if (send_records < unit->select_limit_cnt && do_send_rows &&
	  (res= result->send_data(rollup.fields[i])) > 0)
	return 1;
      if (!res)
        send_records++;
    }
  }
  /* Restore ref_pointer_array */
  set_items_ref_array(current_ref_ptrs);
  return 0;
}

/**
  Write all rollup levels higher than the current one to a temp table.

  @b SAMPLE
    @code
      SELECT a, b, SUM(c) FROM t1 GROUP BY a,b WITH ROLLUP
  @endcode

  @param idx                 Level we are on:
                               - 0 = Total sum level
                               - 1 = First group changed  (a)
                               - 2 = Second group changed (a,b)
  @param table               reference to temp table

  @retval
    0   ok
  @retval
    1   if write_data_failed()
*/

int JOIN::rollup_write_data(uint idx, TMP_TABLE_PARAM *tmp_table_param_arg, TABLE *table_arg)
{
  uint i;
  for (i= send_group_parts ; i-- > idx ; )
  {
    /* Get reference pointers to sum functions in place */
    copy_ref_ptr_array(ref_ptrs, rollup.ref_pointer_arrays[i]);
    if ((!having || having->val_int()))
    {
      int write_error;
      Item *item;
      List_iterator_fast<Item> it(rollup.fields[i]);
      while ((item= it++))
      {
        if (item->type() == Item::NULL_ITEM && item->is_result_field())
          item->save_in_result_field(1);
      }
      copy_sum_funcs(sum_funcs_end[i+1], sum_funcs_end[i]);
      if (unlikely((write_error=
                    table_arg->file->ha_write_tmp_row(table_arg->record[0]))))
      {
	if (create_internal_tmp_table_from_heap(thd, table_arg, 
                                                tmp_table_param_arg->start_recinfo,
                                                &tmp_table_param_arg->recinfo,
                                                write_error, 0, NULL))
	  return 1;		     
      }
    }
  }
  /* Restore ref_pointer_array */
  set_items_ref_array(current_ref_ptrs);
  return 0;
}

/**
  clear results if there are not rows found for group
  (end_send_group/end_write_group)
*/

void JOIN::clear()
{
  clear_tables(this, 0);
  copy_fields(&tmp_table_param);

  if (sum_funcs)
  {
    Item_sum *func, **func_ptr= sum_funcs;
    while ((func= *(func_ptr++)))
      func->clear();
  }
}


/**
  Print an EXPLAIN line with all NULLs and given message in the 'Extra' column

  @retval
    0  ok
    1  OOM error or error from send_data()
*/

int print_explain_message_line(select_result_sink *result, 
                               uint8 options, bool is_analyze,
                               uint select_number,
                               const char *select_type,
                               ha_rows *rows,
                               const char *message)
{
  THD *thd= result->thd;
  MEM_ROOT *mem_root= thd->mem_root;
  Item *item_null= new (mem_root) Item_null(thd);
  List<Item> item_list;

  item_list.push_back(new (mem_root) Item_int(thd, (int32) select_number),
                      mem_root);
  item_list.push_back(new (mem_root) Item_string_sys(thd, select_type),
                      mem_root);
  /* `table` */
  item_list.push_back(item_null, mem_root);
  
  /* `partitions` */
  if (options & DESCRIBE_PARTITIONS)
    item_list.push_back(item_null, mem_root);
  
  /* type, possible_keys, key, key_len, ref */
  for (uint i=0 ; i < 5; i++)
    item_list.push_back(item_null, mem_root);

  /* `rows` */
  if (rows)
  {
    item_list.push_back(new (mem_root) Item_int(thd, *rows,
                                     MY_INT64_NUM_DECIMAL_DIGITS),
                        mem_root);
  }
  else
    item_list.push_back(item_null, mem_root);

  /* `r_rows` */
  if (is_analyze)
    item_list.push_back(item_null, mem_root);

  /* `filtered` */
  if (is_analyze || options & DESCRIBE_EXTENDED)
    item_list.push_back(item_null, mem_root);
  
  /* `r_filtered` */
  if (is_analyze)
    item_list.push_back(item_null, mem_root);

  /* `Extra` */
  if (message)
    item_list.push_back(new (mem_root) Item_string_sys(thd, message),
                        mem_root);
  else
    item_list.push_back(item_null, mem_root);

  if (unlikely(thd->is_fatal_error) || unlikely(result->send_data(item_list)))
    return 1;
  return 0;
}


/*
  Append MRR information from quick select to the given string
*/

void explain_append_mrr_info(QUICK_RANGE_SELECT *quick, String *res)
{
  char mrr_str_buf[128];
  mrr_str_buf[0]=0;
  int len;
  handler *h= quick->head->file;
  len= h->multi_range_read_explain_info(quick->mrr_flags, mrr_str_buf,
                                        sizeof(mrr_str_buf));
  if (len > 0)
  {
    //res->append(STRING_WITH_LEN("; "));
    res->append(mrr_str_buf, len);
  }
}


///////////////////////////////////////////////////////////////////////////////
int append_possible_keys(MEM_ROOT *alloc, String_list &list, TABLE *table, 
                         key_map possible_keys)
{
  uint j;
  for (j=0 ; j < table->s->keys ; j++)
  {
    if (possible_keys.is_set(j))
      if (!(list.append_str(alloc, table->key_info[j].name.str)))
        return 1;
  }
  return 0;
}


bool JOIN_TAB::save_explain_data(Explain_table_access *eta,
                                 table_map prefix_tables, 
                                 bool distinct_arg, JOIN_TAB *first_top_tab)
{
  int quick_type;
  CHARSET_INFO *cs= system_charset_info;
  THD *thd=      join->thd;
  TABLE_LIST *table_list= table->pos_in_table_list;
  QUICK_SELECT_I *cur_quick= NULL;
  my_bool key_read;
  char table_name_buffer[SAFE_NAME_LEN];
  KEY *key_info= 0;
  uint key_len= 0;
  quick_type= -1;

  explain_plan= eta;
  eta->key.clear();
  eta->quick_info= NULL;

  SQL_SELECT *tab_select;
  /* 
    We assume that if this table does pre-sorting, then it doesn't do filtering
    with SQL_SELECT.
  */
  DBUG_ASSERT(!(select && filesort));
  tab_select= (filesort)? filesort->select : select;

  if (filesort)
  {
    if (!(eta->pre_join_sort=
          new (thd->mem_root) Explain_aggr_filesort(thd->mem_root,
                                                    thd->lex->analyze_stmt,
                                                    filesort)))
      return 1;
  }
  
  tracker= &eta->tracker;
  jbuf_tracker= &eta->jbuf_tracker;

  /* Enable the table access time tracker only for "ANALYZE stmt" */
  if (thd->lex->analyze_stmt)
    table->file->set_time_tracker(&eta->op_tracker);

  /* No need to save id and select_type here, they are kept in Explain_select */

  /* table */
  if (table->derived_select_number)
  {
    /* Derived table name generation */
    size_t len= my_snprintf(table_name_buffer, sizeof(table_name_buffer)-1,
                         "<derived%u>",
                         table->derived_select_number);
    eta->table_name.copy(table_name_buffer, len, cs);
  }
  else if (bush_children)
  {
    JOIN_TAB *ctab= bush_children->start;
    /* table */
    size_t len= my_snprintf(table_name_buffer,
                         sizeof(table_name_buffer)-1,
                         "<subquery%d>", 
                         ctab->emb_sj_nest->sj_subq_pred->get_identifier());
    eta->table_name.copy(table_name_buffer, len, cs);
  }
  else
  {
    TABLE_LIST *real_table= table->pos_in_table_list;
    /*
      When multi-table UPDATE/DELETE does updates/deletes to a VIEW, the view
      is merged in a certain particular way (grep for DT_MERGE_FOR_INSERT).

      As a result, view's underlying tables have $tbl->pos_in_table_list={view}.
      We don't want to print view name in EXPLAIN, we want underlying table's
      alias (like specified in the view definition).
    */
    if (real_table->merged_for_insert)
    {
      TABLE_LIST *view_child= real_table->view->select_lex.table_list.first;
      for (;view_child; view_child= view_child->next_local)
      {
        if (view_child->table == table)
        {
          real_table= view_child;
          break;
        }
      }
    }
    eta->table_name.copy(real_table->alias.str, real_table->alias.length, cs);
  }

  /* "partitions" column */
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    partition_info *part_info;
    if (!table->derived_select_number && 
        (part_info= table->part_info))
    { //TODO: all thd->mem_root here should be fixed
      make_used_partitions_str(thd->mem_root, part_info, &eta->used_partitions,
                               eta->used_partitions_list);
      eta->used_partitions_set= true;
    }
    else
      eta->used_partitions_set= false;
#else
    /* just produce empty column if partitioning is not compiled in */
    eta->used_partitions_set= false;
#endif
  }

  /* "type" column */
  enum join_type tab_type= type;
  if ((type == JT_ALL || type == JT_HASH) &&
       tab_select && tab_select->quick && use_quick != 2)
  {
    cur_quick= tab_select->quick;
    quick_type= cur_quick->get_type();
    if ((quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_INTERSECT) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT) ||
        (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_UNION))
      tab_type= type == JT_ALL ? JT_INDEX_MERGE : JT_HASH_INDEX_MERGE;
    else
      tab_type= type == JT_ALL ? JT_RANGE : JT_HASH_RANGE;
  }
  eta->type= tab_type;

  /* Build "possible_keys" value */
  // psergey-todo: why does this use thd MEM_ROOT??? Doesn't this 
  // break ANALYZE ? thd->mem_root will be freed, and after that we will
  // attempt to print the query plan?
  if (append_possible_keys(thd->mem_root, eta->possible_keys, table, keys))
    return 1;
  // psergey-todo: ^ check for error return code 

  /* Build "key", "key_len", and "ref" */
  if (tab_type == JT_NEXT)
  {
    key_info= table->key_info+index;
    key_len= key_info->key_length;
  }
  else if (ref.key_parts)
  {
    key_info= get_keyinfo_by_key_no(ref.key);
    key_len= ref.key_length;
  }
  
  /*
    In STRAIGHT_JOIN queries, there can be join tabs with JT_CONST type
    that still have quick selects.
  */
  if (tab_select && tab_select->quick && tab_type != JT_CONST)
  {
    if (!(eta->quick_info= tab_select->quick->get_explain(thd->mem_root)))
      return 1;
  }

  if (key_info) /* 'index' or 'ref' access */
  {
    eta->key.set(thd->mem_root, key_info, key_len);

    if (ref.key_parts && tab_type != JT_FT)
    {
      store_key **key_ref= ref.key_copy;
      for (uint kp= 0; kp < ref.key_parts; kp++)
      {
        if ((key_part_map(1) << kp) & ref.const_ref_part_map)
        {
          if (!(eta->ref_list.append_str(thd->mem_root, "const")))
            return 1;
          /*
            create_ref_for_key() handles keypart=const equalities as follows:
              - non-EXPLAIN execution will copy the "const" to lookup tuple
                immediately and will not add an element to ref.key_copy
              - EXPLAIN will put an element into ref.key_copy. Since we've
                just printed "const" for it, we should skip it here
          */
          if (thd->lex->describe)
            key_ref++;
        }
        else
        {
          if (!(eta->ref_list.append_str(thd->mem_root, (*key_ref)->name())))
            return 1;
          key_ref++;
        }
      }
    }
  }

  if (tab_type == JT_HASH_NEXT) /* full index scan + hash join */
  {
    eta->hash_next_key.set(thd->mem_root, 
                           & table->key_info[index], 
                           table->key_info[index].key_length);
    // psergey-todo: ^ is the above correct? are we necessarily joining on all
    // columns?
  }

  if (!key_info)
  {
    if (table_list && /* SJM bushes don't have table_list */
        table_list->schema_table &&
        table_list->schema_table->i_s_requested_object & OPTIMIZE_I_S_TABLE)
    {
      IS_table_read_plan *is_table_read_plan= table_list->is_table_read_plan;
      const char *tmp_buff;
      int f_idx;
      StringBuffer<64> key_name_buf;
      if (is_table_read_plan->trivial_show_command ||
          is_table_read_plan->has_db_lookup_value())
      {
        /* The "key" has the name of the column referring to the database */
        f_idx= table_list->schema_table->idx_field1;
        tmp_buff= table_list->schema_table->fields_info[f_idx].field_name;
        key_name_buf.append(tmp_buff, strlen(tmp_buff), cs);
      }          
      if (is_table_read_plan->trivial_show_command ||
          is_table_read_plan->has_table_lookup_value())
      {
        if (is_table_read_plan->trivial_show_command ||
            is_table_read_plan->has_db_lookup_value())
          key_name_buf.append(',');

        f_idx= table_list->schema_table->idx_field2;
        tmp_buff= table_list->schema_table->fields_info[f_idx].field_name;
        key_name_buf.append(tmp_buff, strlen(tmp_buff), cs);
      }

      if (key_name_buf.length())
        eta->key.set_pseudo_key(thd->mem_root, key_name_buf.c_ptr_safe());
    }
  }
  
  /* "rows" */
  if (table_list /* SJM bushes don't have table_list */ &&
      table_list->schema_table)
  {
    /* I_S tables have rows=extra=NULL */
    eta->rows_set= false;
    eta->filtered_set= false;
  }
  else
  {
    ha_rows examined_rows= get_examined_rows();

    eta->rows_set= true;
    eta->rows= examined_rows;

    /* "filtered"  */
    float f= 0.0; 
    if (examined_rows)
    {
      double pushdown_cond_selectivity= cond_selectivity;
      if (pushdown_cond_selectivity == 1.0)
        f= (float) (100.0 * records_read / examined_rows);
      else
        f= (float) (100.0 * pushdown_cond_selectivity);
    }
    set_if_smaller(f, 100.0);
    eta->filtered_set= true;
    eta->filtered= f;
  }

  /* Build "Extra" field and save it */
  key_read= table->file->keyread_enabled();
  if ((tab_type == JT_NEXT || tab_type == JT_CONST) &&
      table->covering_keys.is_set(index))
    key_read=1;
  if (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT &&
      !((QUICK_ROR_INTERSECT_SELECT*)cur_quick)->need_to_fetch_row)
    key_read=1;
    
  if (info)
  {
    eta->push_extra(info);
  }
  else if (packed_info & TAB_INFO_HAVE_VALUE)
  {
    if (packed_info & TAB_INFO_USING_INDEX)
      eta->push_extra(ET_USING_INDEX);
    if (packed_info & TAB_INFO_USING_WHERE)
      eta->push_extra(ET_USING_WHERE);
    if (packed_info & TAB_INFO_FULL_SCAN_ON_NULL)
      eta->push_extra(ET_FULL_SCAN_ON_NULL_KEY);
  }
  else
  {
    uint keyno= MAX_KEY;
    if (ref.key_parts)
      keyno= ref.key;
    else if (tab_select && cur_quick)
      keyno = cur_quick->index;

    if (keyno != MAX_KEY && keyno == table->file->pushed_idx_cond_keyno &&
        table->file->pushed_idx_cond)
    {
      eta->push_extra(ET_USING_INDEX_CONDITION);
      eta->pushed_index_cond= table->file->pushed_idx_cond;
    }
    else if (cache_idx_cond)
    {
      eta->push_extra(ET_USING_INDEX_CONDITION_BKA);
      eta->pushed_index_cond= cache_idx_cond;
    }

    if (quick_type == QUICK_SELECT_I::QS_TYPE_ROR_UNION || 
        quick_type == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT ||
        quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_INTERSECT ||
        quick_type == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE)
    {
      eta->push_extra(ET_USING);
    }
    if (tab_select)
    {
      if (use_quick == 2)
      {
        eta->push_extra(ET_RANGE_CHECKED_FOR_EACH_RECORD);
        eta->range_checked_fer= new (thd->mem_root) Explain_range_checked_fer;
        if (eta->range_checked_fer)
          eta->range_checked_fer->
            append_possible_keys_stat(thd->mem_root, table, keys);
      }
      else if (tab_select->cond ||
               (cache_select && cache_select->cond))
      {
        const COND *pushed_cond= table->file->pushed_cond;

        if ((table->file->ha_table_flags() &
              HA_CAN_TABLE_CONDITION_PUSHDOWN) &&
            pushed_cond)
        {
          eta->push_extra(ET_USING_WHERE_WITH_PUSHED_CONDITION);
        }
        else
        {
          eta->where_cond= tab_select->cond;
          eta->cache_cond= cache_select? cache_select->cond : NULL;
          eta->push_extra(ET_USING_WHERE);
        }
      }
    }
    if (table_list /* SJM bushes don't have table_list */ &&
        table_list->schema_table &&
        table_list->schema_table->i_s_requested_object & OPTIMIZE_I_S_TABLE)
    {
      if (!table_list->table_open_method)
        eta->push_extra(ET_SKIP_OPEN_TABLE);
      else if (table_list->table_open_method == OPEN_FRM_ONLY)
        eta->push_extra(ET_OPEN_FRM_ONLY);
      else
        eta->push_extra(ET_OPEN_FULL_TABLE);
      /* psergey-note: the following has a bug.*/
      if (table_list->is_table_read_plan->trivial_show_command ||
          (table_list->is_table_read_plan->has_db_lookup_value() &&
           table_list->is_table_read_plan->has_table_lookup_value()))
        eta->push_extra(ET_SCANNED_0_DATABASES);
      else if (table_list->is_table_read_plan->has_db_lookup_value() ||
               table_list->is_table_read_plan->has_table_lookup_value())
        eta->push_extra(ET_SCANNED_1_DATABASE);
      else
        eta->push_extra(ET_SCANNED_ALL_DATABASES);
    }
    if (key_read)
    {
      if (quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX)
      {
        QUICK_GROUP_MIN_MAX_SELECT *qgs= 
          (QUICK_GROUP_MIN_MAX_SELECT *) tab_select->quick;
        eta->push_extra(ET_USING_INDEX_FOR_GROUP_BY);
        eta->loose_scan_is_scanning= qgs->loose_scan_is_scanning();
      }
      else
        eta->push_extra(ET_USING_INDEX);
    }
    if (table->reginfo.not_exists_optimize)
      eta->push_extra(ET_NOT_EXISTS);

    if (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE)
    {
      explain_append_mrr_info((QUICK_RANGE_SELECT*)(tab_select->quick),
                              &eta->mrr_type);
      if (eta->mrr_type.length() > 0)
        eta->push_extra(ET_USING_MRR);
    }

    if (shortcut_for_distinct)
      eta->push_extra(ET_DISTINCT);

    if (loosescan_match_tab)
    {
      eta->push_extra(ET_LOOSESCAN);
    }

    if (first_weedout_table)
    {
      eta->start_dups_weedout= true;
      eta->push_extra(ET_START_TEMPORARY);
    }
    if (check_weed_out_table)
    {
      eta->push_extra(ET_END_TEMPORARY);
      eta->end_dups_weedout= true;
    }

    else if (do_firstmatch)
    {
      if (do_firstmatch == /*join->join_tab*/ first_top_tab - 1)
        eta->push_extra(ET_FIRST_MATCH);
      else
      {
        eta->push_extra(ET_FIRST_MATCH);
        TABLE *prev_table=do_firstmatch->table;
        if (prev_table->derived_select_number)
        {
          char namebuf[NAME_LEN];
          /* Derived table name generation */
          size_t len= my_snprintf(namebuf, sizeof(namebuf)-1,
                               "<derived%u>",
                               prev_table->derived_select_number);
          eta->firstmatch_table_name.append(namebuf, len);
        }
        else
          eta->firstmatch_table_name.append(&prev_table->pos_in_table_list->alias);
      }
    }

    for (uint part= 0; part < ref.key_parts; part++)
    {
      if (ref.cond_guards[part])
      {
        eta->push_extra(ET_FULL_SCAN_ON_NULL_KEY);
        eta->full_scan_on_null_key= true;
        break;
      }
    }

    if (cache)
    {
      eta->push_extra(ET_USING_JOIN_BUFFER);
      if (cache->save_explain_data(&eta->bka_type))
        return 1;
    }
  }

  /* 
    In case this is a derived table, here we remember the number of 
    subselect that used to produce it.
  */
  if (!(table_list && table_list->is_with_table_recursive_reference()))
    eta->derived_select_number= table->derived_select_number;

  /* The same for non-merged semi-joins */
  eta->non_merged_sjm_number = get_non_merged_semijoin_select();

  return 0;
}


/*
  Walk through join->aggr_tables and save aggregation/grouping query plan into
  an Explain_select object

  @retval
  0 ok
  1 error
*/

bool save_agg_explain_data(JOIN *join, Explain_select *xpl_sel)
{
  JOIN_TAB *join_tab=join->join_tab + join->exec_join_tab_cnt();
  Explain_aggr_node *prev_node;
  Explain_aggr_node *node= xpl_sel->aggr_tree;
  bool is_analyze= join->thd->lex->analyze_stmt;
  THD *thd= join->thd;

  for (uint i= 0; i < join->aggr_tables; i++, join_tab++)
  {
    // Each aggregate means a temp.table
    prev_node= node;
    if (!(node= new (thd->mem_root) Explain_aggr_tmp_table))
      return 1;
    node->child= prev_node;

    if (join_tab->window_funcs_step)
    {
      Explain_aggr_node *new_node= 
        join_tab->window_funcs_step->save_explain_plan(thd->mem_root,
                                                       is_analyze);
      if (!new_node)
        return 1;

      prev_node=node;
      node= new_node;
      node->child= prev_node;
    }

    /* The below matches execution in join_init_read_record() */
    if (join_tab->distinct)
    {
      prev_node= node;
      if (!(node= new (thd->mem_root) Explain_aggr_remove_dups))
        return 1;
      node->child= prev_node;
    }

    if (join_tab->filesort)
    {
      Explain_aggr_filesort *eaf =
        new (thd->mem_root) Explain_aggr_filesort(thd->mem_root, is_analyze, join_tab->filesort);
      if (!eaf)
        return 1;
      prev_node= node;
      node= eaf;
      node->child= prev_node;
    }
  }
  xpl_sel->aggr_tree= node;
  return 0;
}


/**
  Save Query Plan Footprint

  @note
    Currently, this function may be called multiple times

  @retval
  0 ok
  1 error
*/

int JOIN::save_explain_data_intern(Explain_query *output, 
                                   bool need_tmp_table_arg,
                                   bool need_order_arg, bool distinct_arg, 
                                   const char *message)
{
  JOIN *join= this; /* Legacy: this code used to be a non-member function */
  DBUG_ENTER("JOIN::save_explain_data_intern");
  DBUG_PRINT("info", ("Select %p, type %s, message %s",
		      join->select_lex, join->select_lex->type,
		      message ? message : "NULL"));
  DBUG_ASSERT(have_query_plan == QEP_AVAILABLE);
  /* fake_select_lex is created/printed by Explain_union */
  DBUG_ASSERT(join->select_lex != join->unit->fake_select_lex);

  /* There should be no attempts to save query plans for merged selects */
  DBUG_ASSERT(!join->select_lex->master_unit()->derived ||
              join->select_lex->master_unit()->derived->is_materialized_derived() ||
              join->select_lex->master_unit()->derived->is_with_table());

  /* Don't log this into the slow query log */

  if (message)
  {
    if (!(explain= new (output->mem_root)
          Explain_select(output->mem_root,
                         thd->lex->analyze_stmt)))
      DBUG_RETURN(1);
#ifndef DBUG_OFF
    explain->select_lex= select_lex;
#endif
    join->select_lex->set_explain_type(true);

    explain->select_id= join->select_lex->select_number;
    explain->select_type= join->select_lex->type;
    explain->linkage= select_lex->linkage;
    explain->using_temporary= need_tmp;
    explain->using_filesort=  need_order_arg;
    /* Setting explain->message means that all other members are invalid */
    explain->message= message;

    if (select_lex->master_unit()->derived)
      explain->connection_type= Explain_node::EXPLAIN_NODE_DERIVED;
    if (save_agg_explain_data(this, explain))
      DBUG_RETURN(1);

    output->add_node(explain);
  }
  else if (pushdown_query)
  {
    if (!(explain= new (output->mem_root)
          Explain_select(output->mem_root,
                         thd->lex->analyze_stmt)))
      DBUG_RETURN(1);
    select_lex->set_explain_type(true);

    explain->select_id=   select_lex->select_number;
    explain->select_type= select_lex->type;
    explain->linkage= select_lex->linkage;
    explain->using_temporary= need_tmp;
    explain->using_filesort=  need_order_arg;
    explain->message= "Storage engine handles GROUP BY";

    if (select_lex->master_unit()->derived)
      explain->connection_type= Explain_node::EXPLAIN_NODE_DERIVED;
    output->add_node(explain);
  }
  else
  {
    Explain_select *xpl_sel;
    explain= xpl_sel= 
      new (output->mem_root) Explain_select(output->mem_root, 
                                            thd->lex->analyze_stmt);
    if (!explain)
      DBUG_RETURN(1);

    table_map used_tables=0;

    join->select_lex->set_explain_type(true);
    xpl_sel->select_id= join->select_lex->select_number;
    xpl_sel->select_type= join->select_lex->type;
    xpl_sel->linkage= select_lex->linkage;
    xpl_sel->is_lateral= ((select_lex->linkage == DERIVED_TABLE_TYPE) &&
                          (select_lex->uncacheable & UNCACHEABLE_DEPENDENT));
    if (select_lex->master_unit()->derived)
      xpl_sel->connection_type= Explain_node::EXPLAIN_NODE_DERIVED;
    
    if (save_agg_explain_data(this, xpl_sel))
      DBUG_RETURN(1);

    xpl_sel->exec_const_cond= exec_const_cond;
    xpl_sel->outer_ref_cond= outer_ref_cond;
    xpl_sel->pseudo_bits_cond= pseudo_bits_cond;
    if (tmp_having)
      xpl_sel->having= tmp_having;
    else
      xpl_sel->having= having;
    xpl_sel->having_value= having_value;

    JOIN_TAB* const first_top_tab= join->first_breadth_first_tab();
    JOIN_TAB* prev_bush_root_tab= NULL;

    Explain_basic_join *cur_parent= xpl_sel;
    
    for (JOIN_TAB *tab= first_explain_order_tab(join); tab;
         tab= next_explain_order_tab(join, tab))
    {
      JOIN_TAB *saved_join_tab= NULL;
      TABLE *cur_table= tab->table;

      /* Don't show eliminated tables */
      if (cur_table->map & join->eliminated_tables)
      {
        used_tables|= cur_table->map;
        continue;
      }


      Explain_table_access *eta= (new (output->mem_root)
                                  Explain_table_access(output->mem_root));

      if (!eta)
        DBUG_RETURN(1);
      if (tab->bush_root_tab != prev_bush_root_tab)
      {
        if (tab->bush_root_tab)
        {
          /* 
            We've entered an SJ-Materialization nest. Create an object for it.
          */
          if (!(cur_parent=
                new (output->mem_root) Explain_basic_join(output->mem_root)))
            DBUG_RETURN(1);

          JOIN_TAB *first_child= tab->bush_root_tab->bush_children->start;
          cur_parent->select_id=
            first_child->emb_sj_nest->sj_subq_pred->get_identifier();
        }
        else
        {
          /* 
            We've just left an SJ-Materialization nest. We are at the join tab
            that 'embeds the nest'
          */
          DBUG_ASSERT(tab->bush_children);
          eta->sjm_nest= cur_parent;
          cur_parent= xpl_sel;
        }
      }
      prev_bush_root_tab= tab->bush_root_tab;

      cur_parent->add_table(eta, output);
      if (tab->save_explain_data(eta, used_tables, distinct_arg, first_top_tab))
        DBUG_RETURN(1);

      if (saved_join_tab)
        tab= saved_join_tab;

      // For next iteration
      used_tables|= cur_table->map;
    }
    output->add_node(xpl_sel);
  }

  for (SELECT_LEX_UNIT *tmp_unit= join->select_lex->first_inner_unit();
       tmp_unit;
       tmp_unit= tmp_unit->next_unit())
  {
    /* 
      Display subqueries only if 
      (1) they are not parts of ON clauses that were eliminated by table 
          elimination.
      (2) they are not merged derived tables
      (3) they are not hanging CTEs (they are needed for execution)
    */
    if (!(tmp_unit->item && tmp_unit->item->eliminated) &&    // (1)
        (!tmp_unit->derived ||
         tmp_unit->derived->is_materialized_derived()) &&     // (2)
        (!tmp_unit->with_element  ||
         (tmp_unit->derived &&
          tmp_unit->derived->derived_result &&
          !tmp_unit->with_element->is_hanging_recursive())))  // (3)
   {
      explain->add_child(tmp_unit->first_select()->select_number);
    }
  }

  if (select_lex->is_top_level_node())
    output->query_plan_ready();

  DBUG_RETURN(0);
}


/*
  This function serves as "shortcut point" for EXPLAIN queries.
  
  The EXPLAIN statement executes just like its SELECT counterpart would
  execute, except that JOIN::exec() will call select_describe() instead of
  actually executing the query.

  Inside select_describe():
  - Query plan is updated with latest QEP choices made at the start of
    JOIN::exec().
  - the proces of "almost execution" is invoked for the children subqueries.

  Overall, select_describe() is a legacy of old EXPLAIN implementation and
  should be removed.
*/ 

static void select_describe(JOIN *join, bool need_tmp_table, bool need_order,
			    bool distinct,const char *message)
{
  THD *thd=join->thd;
  select_result *result=join->result;
  DBUG_ENTER("select_describe");
  
  /* Update the QPF with latest values of using_temporary, using_filesort */
  for (SELECT_LEX_UNIT *unit= join->select_lex->first_inner_unit();
       unit;
       unit= unit->next_unit())
  {
    /*
      This fix_fields() call is to handle an edge case like this:
       
        SELECT ... UNION SELECT ... ORDER BY (SELECT ...)
      
      for such queries, we'll get here before having called
      subquery_expr->fix_fields(), which will cause failure to
    */
    if (unit->item && !unit->item->fixed)
    {
      Item *ref= unit->item;
      if (unit->item->fix_fields(thd, &ref))
        DBUG_VOID_RETURN;
      DBUG_ASSERT(ref == unit->item);
    }

    /* 
      Save plans for child subqueries, when
      (1) they are not parts of eliminated WHERE/ON clauses.
      (2) they are not VIEWs that were "merged for INSERT".
      (3) they are not hanging CTEs (they are needed for execution)
    */
    if (!(unit->item && unit->item->eliminated) &&                     // (1)
        !(unit->derived && unit->derived->merged_for_insert) &&        // (2)
        (!unit->with_element ||
          (unit->derived &&
           unit->derived->derived_result &&
           !unit->with_element->is_hanging_recursive())))              // (3)
    {
      if (mysql_explain_union(thd, unit, result))
        DBUG_VOID_RETURN;
    }
  }
  DBUG_VOID_RETURN;
}


bool mysql_explain_union(THD *thd, SELECT_LEX_UNIT *unit, select_result *result)
{
  DBUG_ENTER("mysql_explain_union");
  bool res= 0;
  SELECT_LEX *first= unit->first_select();

  for (SELECT_LEX *sl= first; sl; sl= sl->next_select())
  {
    sl->set_explain_type(FALSE);
    sl->options|= SELECT_DESCRIBE;
  }

  if (unit->is_unit_op() || unit->fake_select_lex)
  {
    if (unit->union_needs_tmp_table() && unit->fake_select_lex)
    {
      unit->fake_select_lex->select_number= FAKE_SELECT_LEX_ID; // just for initialization
      unit->fake_select_lex->type= unit_operation_text[unit->common_op()];
      unit->fake_select_lex->options|= SELECT_DESCRIBE;
    }
    if (!(res= unit->prepare(unit->derived, result,
                             SELECT_NO_UNLOCK | SELECT_DESCRIBE)))
      res= unit->exec();
  }
  else
  {
    thd->lex->current_select= first;
    unit->set_limit(unit->global_parameters());
    res= mysql_select(thd, 
                      first->table_list.first,
                      first->with_wild, first->item_list,
                      first->where,
                      first->order_list.elements + first->group_list.elements,
                      first->order_list.first,
                      first->group_list.first,
                      first->having,
                      thd->lex->proc_list.first,
                      first->options | thd->variables.option_bits | SELECT_DESCRIBE,
                      result, unit, first);
  }
  DBUG_RETURN(res || thd->is_error());
}


static void print_table_array(THD *thd, 
                              table_map eliminated_tables,
                              String *str, TABLE_LIST **table, 
                              TABLE_LIST **end,
                              enum_query_type query_type)
{
  (*table)->print(thd, eliminated_tables, str, query_type);

  for (TABLE_LIST **tbl= table + 1; tbl < end; tbl++)
  {
    TABLE_LIST *curr= *tbl;
    
    /*
      The "eliminated_tables &&" check guards againist the case of 
      printing the query for CREATE VIEW. We do that without having run 
      JOIN::optimize() and so will have nested_join->used_tables==0.
    */
    if (eliminated_tables &&
        ((curr->table && (curr->table->map & eliminated_tables)) ||
         (curr->nested_join && !(curr->nested_join->used_tables &
                                ~eliminated_tables))))
    {
      /* as of 5.5, print_join doesnt put eliminated elements into array */
      DBUG_ASSERT(0); 
      continue;
    }

    /* JOIN_TYPE_OUTER is just a marker unrelated to real join */
    if (curr->outer_join & (JOIN_TYPE_LEFT|JOIN_TYPE_RIGHT))
    {
      /* MySQL converts right to left joins */
      str->append(STRING_WITH_LEN(" left join "));
    }
    else if (curr->straight)
      str->append(STRING_WITH_LEN(" straight_join "));
    else if (curr->sj_inner_tables)
      str->append(STRING_WITH_LEN(" semi join "));
    else
      str->append(STRING_WITH_LEN(" join "));
    
    curr->print(thd, eliminated_tables, str, query_type);
    if (curr->on_expr)
    {
      str->append(STRING_WITH_LEN(" on("));
      curr->on_expr->print(str, query_type);
      str->append(')');
    }
  }
}


/*
  Check if the passed table is 
   - a base table which was eliminated, or
   - a join nest which only contained eliminated tables (and so was eliminated,
     too)
*/

static bool is_eliminated_table(table_map eliminated_tables, TABLE_LIST *tbl)
{
  return eliminated_tables &&
    ((tbl->table && (tbl->table->map & eliminated_tables)) ||
     (tbl->nested_join && !(tbl->nested_join->used_tables &
                            ~eliminated_tables)));
}

/**
  Print joins from the FROM clause.

  @param thd     thread handler
  @param str     string where table should be printed
  @param tables  list of tables in join
  @query_type    type of the query is being generated
*/

static void print_join(THD *thd,
                       table_map eliminated_tables,
                       String *str,
                       List<TABLE_LIST> *tables,
                       enum_query_type query_type)
{
  /* List is reversed => we should reverse it before using */
  List_iterator_fast<TABLE_LIST> ti(*tables);
  TABLE_LIST **table;
  DBUG_ENTER("print_join");

  /*
    If the QT_NO_DATA_EXPANSION flag is specified, we print the
    original table list, including constant tables that have been
    optimized away, as the constant tables may be referenced in the
    expression printed by Item_field::print() when this flag is given.
    Otherwise, only non-const tables are printed.

    Example:

    Original SQL:
    select * from (select 1) t

    Printed without QT_NO_DATA_EXPANSION:
    select '1' AS `1` from dual

    Printed with QT_NO_DATA_EXPANSION:
    select `t`.`1` from (select 1 AS `1`) `t`
  */
  const bool print_const_tables= (query_type & QT_NO_DATA_EXPANSION);
  size_t tables_to_print= 0;

  for (TABLE_LIST *t= ti++; t ; t= ti++)
  {
    /* See comment in print_table_array() about the second condition */
    if (print_const_tables || !t->optimized_away)
      if (!is_eliminated_table(eliminated_tables, t))
        tables_to_print++;
  }
  if (tables_to_print == 0)
  {
    str->append(STRING_WITH_LEN("dual"));
    DBUG_VOID_RETURN;                   // all tables were optimized away
  }
  ti.rewind();

  if (!(table= static_cast<TABLE_LIST **>(thd->alloc(sizeof(TABLE_LIST*) *
                                                     tables_to_print))))
    DBUG_VOID_RETURN;                   // out of memory

  TABLE_LIST *tmp, **t= table + (tables_to_print - 1);
  while ((tmp= ti++))
  {
    if (tmp->optimized_away && !print_const_tables)
      continue;
    if (is_eliminated_table(eliminated_tables, tmp))
      continue;
    *t--= tmp;
  }

  DBUG_ASSERT(tables->elements >= 1);
  /*
    Assert that the first table in the list isn't eliminated. This comes from
    the fact that the first table can't be inner table of an outer join.
  */
  DBUG_ASSERT(!eliminated_tables || 
              !(((*table)->table && ((*table)->table->map & eliminated_tables)) ||
                ((*table)->nested_join && !((*table)->nested_join->used_tables &
                                           ~eliminated_tables))));
  /* 
    If the first table is a semi-join nest, swap it with something that is
    not a semi-join nest.
  */
  if ((*table)->sj_inner_tables)
  {
    TABLE_LIST **end= table + tables_to_print;
    for (TABLE_LIST **t2= table; t2!=end; t2++)
    {
      if (!(*t2)->sj_inner_tables)
      {
        tmp= *t2;
        *t2= *table;
        *table= tmp;
        break;
      }
    }
  }
  print_table_array(thd, eliminated_tables, str, table, 
                    table +  tables_to_print, query_type);
  DBUG_VOID_RETURN;
}

/**
  @brief Print an index hint

  @details Prints out the USE|FORCE|IGNORE index hint.

  @param      thd         the current thread
  @param[out] str         appends the index hint here
  @param      hint        what the hint is (as string : "USE INDEX"|
                          "FORCE INDEX"|"IGNORE INDEX")
  @param      hint_length the length of the string in 'hint'
  @param      indexes     a list of index names for the hint
*/

void 
Index_hint::print(THD *thd, String *str)
{
  switch (type)
  {
    case INDEX_HINT_IGNORE: str->append(STRING_WITH_LEN("IGNORE INDEX")); break;
    case INDEX_HINT_USE:    str->append(STRING_WITH_LEN("USE INDEX")); break;
    case INDEX_HINT_FORCE:  str->append(STRING_WITH_LEN("FORCE INDEX")); break;
  }
  str->append (STRING_WITH_LEN(" ("));
  if (key_name.length)
  {
    if (thd && !my_strnncoll(system_charset_info,
                             (const uchar *)key_name.str, key_name.length, 
                             (const uchar *)primary_key_name, 
                             strlen(primary_key_name)))
      str->append(primary_key_name);
    else
      append_identifier(thd, str, &key_name);
}
  str->append(')');
}


/**
  Print table as it should be in join list.

  @param str   string where table should be printed
*/

void TABLE_LIST::print(THD *thd, table_map eliminated_tables, String *str, 
                       enum_query_type query_type)
{
  if (nested_join)
  {
    str->append('(');
    print_join(thd, eliminated_tables, str, &nested_join->join_list, query_type);
    str->append(')');
  }
  else if (jtbm_subselect)
  {
    if (jtbm_subselect->engine->engine_type() ==
          subselect_engine::SINGLE_SELECT_ENGINE)
    {
      /* 
        We get here when conversion into materialization didn't finish (this
        happens when
        - The subquery is a degenerate case which produces 0 or 1 record
        - subquery's optimization didn't finish because of @@max_join_size
          limits
        - ... maybe some other cases like this 
      */
      str->append(STRING_WITH_LEN(" <materialize> ("));
      jtbm_subselect->engine->print(str, query_type);
      str->append(')');
    }
    else
    {
      str->append(STRING_WITH_LEN(" <materialize> ("));
      subselect_hash_sj_engine *hash_engine;
      hash_engine= (subselect_hash_sj_engine*)jtbm_subselect->engine;
      hash_engine->materialize_engine->print(str, query_type);
      str->append(')');
    }
  }
  else
  {
    const char *cmp_name;                         // Name to compare with alias
    if (view_name.str)
    {
      // A view

      if (!(belong_to_view &&
            belong_to_view->compact_view_format))
      {
        append_identifier(thd, str, &view_db);
        str->append('.');
      }
      append_identifier(thd, str, &view_name);
      cmp_name= view_name.str;
    }
    else if (derived)
    {
      if (!is_with_table())
      {
        // A derived table
        str->append('(');
        derived->print(str, query_type);
        str->append(')');
        cmp_name= "";                               // Force printing of alias
      }
      else
      {
        append_identifier(thd, str, &table_name);
        cmp_name= table_name.str;
      }
    }
    else
    {
      // A normal table

      if (!(belong_to_view &&
            belong_to_view->compact_view_format))
      {
        append_identifier(thd, str, &db);
        str->append('.');
      }
      if (schema_table)
      {
        append_identifier(thd, str, &schema_table_name);
        cmp_name= schema_table_name.str;
      }
      else
      {
        append_identifier(thd, str, &table_name);
        cmp_name= table_name.str;
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
      if (partition_names && partition_names->elements)
      {
        int i, num_parts= partition_names->elements;
        List_iterator<String> name_it(*(partition_names));
        str->append(STRING_WITH_LEN(" PARTITION ("));
        for (i= 1; i <= num_parts; i++)
        {
          String *name= name_it++;
          append_identifier(thd, str, name->c_ptr(), name->length());
          if (i != num_parts)
            str->append(',');
        }
        str->append(')');
      }
#endif /* WITH_PARTITION_STORAGE_ENGINE */
    }
    if (table && table->versioned())
      vers_conditions.print(str, query_type);

    if (my_strcasecmp(table_alias_charset, cmp_name, alias.str))
    {
      char t_alias_buff[MAX_ALIAS_NAME];
      LEX_CSTRING t_alias= alias;

      str->append(' ');
      if (lower_case_table_names == 1)
      {
        if (alias.str && alias.str[0])
        {
          strmov(t_alias_buff, alias.str);
          t_alias.length= my_casedn_str(files_charset_info, t_alias_buff);
          t_alias.str= t_alias_buff;
        }
      }

      append_identifier(thd, str, &t_alias);
    }

    if (index_hints)
    {
      List_iterator<Index_hint> it(*index_hints);
      Index_hint *hint;

      while ((hint= it++))
      {
        str->append (STRING_WITH_LEN(" "));
        hint->print (thd, str);
      }
    }
  }
}


void st_select_lex::print(THD *thd, String *str, enum_query_type query_type)
{
  DBUG_ASSERT(thd);

  if (tvc)
  {
    tvc->print(thd, str, query_type);
    return;
  }

  if ((query_type & QT_SHOW_SELECT_NUMBER) &&
      thd->lex->all_selects_list &&
      thd->lex->all_selects_list->link_next &&
      select_number != UINT_MAX &&
      select_number != INT_MAX)
  {
    str->append("/* select#");
    str->append_ulonglong(select_number);
    str->append(" */ ");
  }

  str->append(STRING_WITH_LEN("select "));

  if (join && join->cleaned)
  {
    /*
      JOIN already cleaned up so it is dangerous to print items
      because temporary tables they pointed on could be freed.
    */
    str->append('#');
    str->append(select_number);
    return;
  }

  /* First add options */
  if (options & SELECT_STRAIGHT_JOIN)
    str->append(STRING_WITH_LEN("straight_join "));
  if (options & SELECT_HIGH_PRIORITY)
    str->append(STRING_WITH_LEN("high_priority "));
  if (options & SELECT_DISTINCT)
    str->append(STRING_WITH_LEN("distinct "));
  if (options & SELECT_SMALL_RESULT)
    str->append(STRING_WITH_LEN("sql_small_result "));
  if (options & SELECT_BIG_RESULT)
    str->append(STRING_WITH_LEN("sql_big_result "));
  if (options & OPTION_BUFFER_RESULT)
    str->append(STRING_WITH_LEN("sql_buffer_result "));
  if (options & OPTION_FOUND_ROWS)
    str->append(STRING_WITH_LEN("sql_calc_found_rows "));
  switch (sql_cache)
  {
    case SQL_NO_CACHE:
      str->append(STRING_WITH_LEN("sql_no_cache "));
      break;
    case SQL_CACHE:
      str->append(STRING_WITH_LEN("sql_cache "));
      break;
    case SQL_CACHE_UNSPECIFIED:
      break;
    default:
      DBUG_ASSERT(0);
  }

  //Item List
  bool first= 1;
  List_iterator_fast<Item> it(item_list);
  Item *item;
  while ((item= it++))
  {
    if (first)
      first= 0;
    else
      str->append(',');

    if (is_subquery_function() && item->is_autogenerated_name)
    {
      /*
        Do not print auto-generated aliases in subqueries. It has no purpose
        in a view definition or other contexts where the query is printed.
      */
      item->print(str, query_type);
    }
    else
      item->print_item_w_name(str, query_type);
  }

  /*
    from clause
    TODO: support USING/FORCE/IGNORE index
  */
  if (table_list.elements)
  {
    str->append(STRING_WITH_LEN(" from "));
    /* go through join tree */
    print_join(thd, join? join->eliminated_tables: 0, str, &top_join_list, query_type);
  }
  else if (where)
  {
    /*
      "SELECT 1 FROM DUAL WHERE 2" should not be printed as 
      "SELECT 1 WHERE 2": the 1st syntax is valid, but the 2nd is not.
    */
    str->append(STRING_WITH_LEN(" from DUAL "));
  }

  // Where
  Item *cur_where= where;
  if (join)
    cur_where= join->conds;
  if (cur_where || cond_value != Item::COND_UNDEF)
  {
    str->append(STRING_WITH_LEN(" where "));
    if (cur_where)
      cur_where->print(str, query_type);
    else
      str->append(cond_value != Item::COND_FALSE ? "1" : "0");
  }

  // group by & olap
  if (group_list.elements)
  {
    str->append(STRING_WITH_LEN(" group by "));
    print_order(str, group_list.first, query_type);
    switch (olap)
    {
      case CUBE_TYPE:
	str->append(STRING_WITH_LEN(" with cube"));
	break;
      case ROLLUP_TYPE:
	str->append(STRING_WITH_LEN(" with rollup"));
	break;
      default:
	;  //satisfy compiler
    }
  }

  // having
  Item *cur_having= having;
  if (join)
    cur_having= join->having;

  if (cur_having || having_value != Item::COND_UNDEF)
  {
    str->append(STRING_WITH_LEN(" having "));
    if (cur_having)
      cur_having->print(str, query_type);
    else
      str->append(having_value != Item::COND_FALSE ? "1" : "0");
  }

  if (order_list.elements)
  {
    str->append(STRING_WITH_LEN(" order by "));
    print_order(str, order_list.first, query_type);
  }

  // limit
  print_limit(thd, str, query_type);

  // lock type
  if (lock_type == TL_READ_WITH_SHARED_LOCKS)
    str->append(" lock in share mode");
  else if (lock_type == TL_WRITE)
    str->append(" for update");

  // PROCEDURE unsupported here
}


/**
  Change the select_result object of the JOIN.

  If old_result is not used, forward the call to the current
  select_result in case it is a wrapper around old_result.

  Call prepare() and prepare2() on the new select_result if we decide
  to use it.

  @param new_result New select_result object
  @param old_result Old select_result object (NULL to force change)

  @retval false Success
  @retval true  Error
*/

bool JOIN::change_result(select_result *new_result, select_result *old_result)
{
  DBUG_ENTER("JOIN::change_result");
  if (old_result == NULL || result == old_result)
  {
    result= new_result;
    if (result->prepare(fields_list, select_lex->master_unit()) ||
        result->prepare2(this))
      DBUG_RETURN(true); /* purecov: inspected */
    DBUG_RETURN(false);
  }
  DBUG_RETURN(result->change_result(new_result));
}


/**
  @brief
  Set allowed types of join caches that can be used for join operations

  @details
  The function sets a bitmap of allowed join buffers types in the field
  allowed_join_cache_types of this JOIN structure:
    bit 1 is set if tjoin buffers are allowed to be incremental
    bit 2 is set if the join buffers are allowed to be hashed
    but 3 is set if the join buffers are allowed to be used for BKA
  join algorithms.
  The allowed types are read from system variables.
  Besides the function sets maximum allowed join cache level that is
  also read from a system variable.
*/

void JOIN::set_allowed_join_cache_types()
{
  allowed_join_cache_types= 0;
  if (optimizer_flag(thd, OPTIMIZER_SWITCH_JOIN_CACHE_INCREMENTAL))
    allowed_join_cache_types|= JOIN_CACHE_INCREMENTAL_BIT;
  if (optimizer_flag(thd, OPTIMIZER_SWITCH_JOIN_CACHE_HASHED))
    allowed_join_cache_types|= JOIN_CACHE_HASHED_BIT;
  if (optimizer_flag(thd, OPTIMIZER_SWITCH_JOIN_CACHE_BKA))
    allowed_join_cache_types|= JOIN_CACHE_BKA_BIT;
  allowed_semijoin_with_cache=
    optimizer_flag(thd, OPTIMIZER_SWITCH_SEMIJOIN_WITH_CACHE);
  allowed_outer_join_with_cache=
    optimizer_flag(thd, OPTIMIZER_SWITCH_OUTER_JOIN_WITH_CACHE);
  max_allowed_join_cache_level= thd->variables.join_cache_level;
}


/**
  Save a query execution plan so that the caller can revert to it if needed,
  and reset the current query plan so that it can be reoptimized.

  @param save_to  The object into which the current query plan state is saved
*/

void JOIN::save_query_plan(Join_plan_state *save_to)
{
  DYNAMIC_ARRAY tmp_keyuse;
  /* Swap the current and the backup keyuse internal arrays. */
  tmp_keyuse= keyuse;
  keyuse= save_to->keyuse; /* keyuse is reset to an empty array. */
  save_to->keyuse= tmp_keyuse;

  for (uint i= 0; i < table_count; i++)
  {
    save_to->join_tab_keyuse[i]= join_tab[i].keyuse;
    join_tab[i].keyuse= NULL;
    save_to->join_tab_checked_keys[i]= join_tab[i].checked_keys;
    join_tab[i].checked_keys.clear_all();
  }
  memcpy((uchar*) save_to->best_positions, (uchar*) best_positions,
         sizeof(POSITION) * (table_count + 1));
  memset((uchar*) best_positions, 0, sizeof(POSITION) * (table_count + 1));
  
  /* Save SJM nests */
  List_iterator<TABLE_LIST> it(select_lex->sj_nests);
  TABLE_LIST *tlist;
  SJ_MATERIALIZATION_INFO **p_info= save_to->sj_mat_info;
  while ((tlist= it++))
  {
    *(p_info++)= tlist->sj_mat_info;
  }
}


/**
  Reset a query execution plan so that it can be reoptimized in-place.
*/
void JOIN::reset_query_plan()
{
  for (uint i= 0; i < table_count; i++)
  {
    join_tab[i].keyuse= NULL;
    join_tab[i].checked_keys.clear_all();
  }
}


/**
  Restore a query execution plan previously saved by the caller.

  @param The object from which the current query plan state is restored.
*/

void JOIN::restore_query_plan(Join_plan_state *restore_from)
{
  DYNAMIC_ARRAY tmp_keyuse;
  tmp_keyuse= keyuse;
  keyuse= restore_from->keyuse;
  restore_from->keyuse= tmp_keyuse;

  for (uint i= 0; i < table_count; i++)
  {
    join_tab[i].keyuse= restore_from->join_tab_keyuse[i];
    join_tab[i].checked_keys= restore_from->join_tab_checked_keys[i];
  }

  memcpy((uchar*) best_positions, (uchar*) restore_from->best_positions,
         sizeof(POSITION) * (table_count + 1));
  /* Restore SJM nests */
  List_iterator<TABLE_LIST> it(select_lex->sj_nests);
  TABLE_LIST *tlist;
  SJ_MATERIALIZATION_INFO **p_info= restore_from->sj_mat_info;
  while ((tlist= it++))
  {
    tlist->sj_mat_info= *(p_info++);
  }
}


/**
  Reoptimize a query plan taking into account an additional conjunct to the
  WHERE clause.

  @param added_where  An extra conjunct to the WHERE clause to reoptimize with
  @param join_tables  The set of tables to reoptimize
  @param save_to      If != NULL, save here the state of the current query plan,
                      otherwise reuse the existing query plan structures.

  @notes
  Given a query plan that was already optimized taking into account some WHERE
  clause 'C', reoptimize this plan with a new WHERE clause 'C AND added_where'.
  The reoptimization works as follows:

  1. Call update_ref_and_keys *only* for the new conditions 'added_where'
     that are about to be injected into the query.
  2. Expand if necessary the original KEYUSE array JOIN::keyuse to
     accommodate the new REF accesses computed for the 'added_where' condition.
  3. Add the new KEYUSEs into JOIN::keyuse.
  4. Re-sort and re-filter the JOIN::keyuse array with the newly added
     KEYUSE elements. 
 
  @retval REOPT_NEW_PLAN  there is a new plan.
  @retval REOPT_OLD_PLAN  no new improved plan was produced, use the old one.
  @retval REOPT_ERROR     an irrecovarable error occurred during reoptimization.
*/

JOIN::enum_reopt_result
JOIN::reoptimize(Item *added_where, table_map join_tables,
                 Join_plan_state *save_to)
{
  DYNAMIC_ARRAY added_keyuse;
  SARGABLE_PARAM *sargables= 0; /* Used only as a dummy parameter. */
  uint org_keyuse_elements;

  /* Re-run the REF optimizer to take into account the new conditions. */
  if (update_ref_and_keys(thd, &added_keyuse, join_tab, table_count, added_where,
                          ~outer_join, select_lex, &sargables))
  {
    delete_dynamic(&added_keyuse);
    return REOPT_ERROR;
  }

  if (!added_keyuse.elements)
  {
    delete_dynamic(&added_keyuse);
    return REOPT_OLD_PLAN;
  }

  if (save_to)
    save_query_plan(save_to);
  else
    reset_query_plan();

  if (!keyuse.buffer &&
      my_init_dynamic_array(&keyuse, sizeof(KEYUSE), 20, 64,
                            MYF(MY_THREAD_SPECIFIC)))
  {
    delete_dynamic(&added_keyuse);
    return REOPT_ERROR;
  }

  org_keyuse_elements= save_to ? save_to->keyuse.elements : keyuse.elements;
  allocate_dynamic(&keyuse, org_keyuse_elements + added_keyuse.elements);

  /* If needed, add the access methods from the original query plan. */
  if (save_to)
  {
    DBUG_ASSERT(!keyuse.elements);
    keyuse.elements= save_to->keyuse.elements;
    if (size_t e= keyuse.elements)
      memcpy(keyuse.buffer,
             save_to->keyuse.buffer, e * keyuse.size_of_element);
  }

  /* Add the new access methods to the keyuse array. */
  memcpy(keyuse.buffer + keyuse.elements * keyuse.size_of_element,
         added_keyuse.buffer,
         (size_t) added_keyuse.elements * added_keyuse.size_of_element);
  keyuse.elements+= added_keyuse.elements;
  /* added_keyuse contents is copied, and it is no longer needed. */
  delete_dynamic(&added_keyuse);

  if (sort_and_filter_keyuse(thd, &keyuse, true))
    return REOPT_ERROR;
  optimize_keyuse(this, &keyuse);

  if (optimize_semijoin_nests(this, join_tables))
    return REOPT_ERROR;

  /* Re-run the join optimizer to compute a new query plan. */
  if (choose_plan(this, join_tables))
    return REOPT_ERROR;

  return REOPT_NEW_PLAN;
}


/**
  Cache constant expressions in WHERE, HAVING, ON conditions.
*/

void JOIN::cache_const_exprs()
{
  bool cache_flag= FALSE;
  bool *analyzer_arg= &cache_flag;

  /* No need in cache if all tables are constant. */
  if (const_tables == table_count)
    return;

  if (conds)
    conds->compile(thd, &Item::cache_const_expr_analyzer, (uchar **)&analyzer_arg,
                  &Item::cache_const_expr_transformer, (uchar *)&cache_flag);
  cache_flag= FALSE;
  if (having)
    having->compile(thd, &Item::cache_const_expr_analyzer, (uchar **)&analyzer_arg,
                    &Item::cache_const_expr_transformer, (uchar *)&cache_flag);

  for (JOIN_TAB *tab= first_depth_first_tab(this); tab;
       tab= next_depth_first_tab(this, tab))
  {
    if (*tab->on_expr_ref)
    {
      cache_flag= FALSE;
      (*tab->on_expr_ref)->compile(thd, &Item::cache_const_expr_analyzer,
                                 (uchar **)&analyzer_arg,
                                 &Item::cache_const_expr_transformer,
                                 (uchar *)&cache_flag);
    }
  }
}

 
/*
  Get a cost of reading rows_limit rows through index keynr.

  @detail
   - If there is a quick select, we try to use it.
   - if there is a ref(const) access, we try to use it, too.
   - quick and ref(const) use different cost formulas, so if both are possible
      we should make a cost-based choice.
  
  @param  tab              JOIN_TAB with table access (is NULL for single-table
                           UPDATE/DELETE)
  @param  read_time OUT    Cost of reading using quick or ref(const) access.


  @return 
    true   There was a possible quick or ref access, its cost is in the OUT
           parameters.
    false  No quick or ref(const) possible (and so, the caller will attempt 
           to use a full index scan on this index).
*/

static bool get_range_limit_read_cost(const JOIN_TAB *tab, 
                                      const TABLE *table, 
                                      uint keynr, 
                                      ha_rows rows_limit,
                                      double *read_time)
{
  bool res= false;
  /* 
    We need to adjust the estimates if we had a quick select (or ref(const)) on
    index keynr.
  */
  if (table->quick_keys.is_set(keynr))
  {
    /*
      Start from quick select's rows and cost. These are always cheaper than
      full index scan/cost.
    */
    double best_rows= (double)table->quick_rows[keynr];
    double best_cost= (double)table->quick_costs[keynr];
    
    /*
      Check if ref(const) access was possible on this index. 
    */
    if (tab)
    {
      key_part_map map= 1;
      uint kp;
      /* Find how many key parts would be used by ref(const) */
      for (kp=0; kp < MAX_REF_PARTS; map=map << 1, kp++)
      {
        if (!(table->const_key_parts[keynr] & map))
          break;
      }
      
      if (kp > 0)
      {
        ha_rows ref_rows;
        /*
          Two possible cases:
          1. ref(const) uses the same #key parts as range access. 
          2. ref(const) uses fewer key parts, becasue there is a
            range_cond(key_part+1).
        */
        if (kp == table->quick_key_parts[keynr])
          ref_rows= table->quick_rows[keynr];
        else
          ref_rows= (ha_rows) table->key_info[keynr].actual_rec_per_key(kp-1);

        if (ref_rows > 0)
        {
          double tmp= (double)ref_rows;
          /* Reuse the cost formula from best_access_path: */
          set_if_smaller(tmp, (double) tab->join->thd->variables.max_seeks_for_key);
          if (table->covering_keys.is_set(keynr))
            tmp= table->file->keyread_time(keynr, 1, (ha_rows) tmp);
          else
            tmp= table->file->read_time(keynr, 1,
                                        (ha_rows) MY_MIN(tmp,tab->worst_seeks));
          if (tmp < best_cost)
          {
            best_cost= tmp;
            best_rows= (double)ref_rows;
          }
        }
      }
    }
 
    if (best_rows > rows_limit)
    {
      /*
        LIMIT clause specifies that we will need to read fewer records than
        quick select will return. Assume that quick select's cost is
        proportional to the number of records we need to return (e.g. if we 
        only need 1/3rd of records, it will cost us 1/3rd of quick select's
        read time)
      */
      best_cost *= rows_limit / best_rows;
    }
    *read_time= best_cost;
    res= true;
  }
  return res;
}


/**
  Find a cheaper access key than a given @a key

  @param          tab                 NULL or JOIN_TAB of the accessed table
  @param          order               Linked list of ORDER BY arguments
  @param          table               Table if tab == NULL or tab->table
  @param          usable_keys         Key map to find a cheaper key in
  @param          ref_key             
                   0 <= key < MAX_KEY  - Key that is currently used for finding
                                         row
                   MAX_KEY             - means index_merge is used
                   -1                  - means we're currently not using an
                                         index to find rows.

  @param          select_limit        LIMIT value
  @param [out]    new_key             Key number if success, otherwise undefined
  @param [out]    new_key_direction   Return -1 (reverse) or +1 if success,
                                      otherwise undefined
  @param [out]    new_select_limit    Return adjusted LIMIT
  @param [out]    new_used_key_parts  NULL by default, otherwise return number
                                      of new_key prefix columns if success
                                      or undefined if the function fails
  @param [out]  saved_best_key_parts  NULL by default, otherwise preserve the
                                      value for further use in QUICK_SELECT_DESC

  @note
    This function takes into account table->quick_condition_rows statistic
    (that is calculated by the make_join_statistics function).
    However, single table procedures such as mysql_update() and mysql_delete()
    never call make_join_statistics, so they have to update it manually
    (@see get_index_for_order()).
*/

static bool
test_if_cheaper_ordering(const JOIN_TAB *tab, ORDER *order, TABLE *table,
                         key_map usable_keys,  int ref_key,
                         ha_rows select_limit_arg,
                         int *new_key, int *new_key_direction,
                         ha_rows *new_select_limit, uint *new_used_key_parts,
                         uint *saved_best_key_parts)
{
  DBUG_ENTER("test_if_cheaper_ordering");
  /*
    Check whether there is an index compatible with the given order
    usage of which is cheaper than usage of the ref_key index (ref_key>=0)
    or a table scan.
    It may be the case if ORDER/GROUP BY is used with LIMIT.
  */
  ha_rows best_select_limit= HA_POS_ERROR;
  JOIN *join= tab ? tab->join : NULL;
  uint nr;
  key_map keys;
  uint best_key_parts= 0;
  int best_key_direction= 0;
  ha_rows best_records= 0;
  double read_time;
  int best_key= -1;
  bool is_best_covering= FALSE;
  double fanout= 1;
  ha_rows table_records= table->stat_records();
  bool group= join && join->group && order == join->group_list;
  ha_rows refkey_rows_estimate= table->quick_condition_rows;
  const bool has_limit= (select_limit_arg != HA_POS_ERROR);

  /*
    If not used with LIMIT, only use keys if the whole query can be
    resolved with a key;  This is because filesort() is usually faster than
    retrieving all rows through an index.
  */
  if (select_limit_arg >= table_records)
  {
    keys= *table->file->keys_to_use_for_scanning();
    keys.merge(table->covering_keys);

    /*
      We are adding here also the index specified in FORCE INDEX clause, 
      if any.
      This is to allow users to use index in ORDER BY.
    */
    if (table->force_index) 
      keys.merge(group ? table->keys_in_use_for_group_by :
                         table->keys_in_use_for_order_by);
    keys.intersect(usable_keys);
  }
  else
    keys= usable_keys;

  if (join)
  {
    uint tablenr= (uint)(tab - join->join_tab);
    read_time= join->best_positions[tablenr].read_time;
    for (uint i= tablenr+1; i < join->table_count; i++)
      fanout*= join->best_positions[i].records_read; // fanout is always >= 1
  }
  else
    read_time= table->file->scan_time();
  
  /*
    TODO: add cost of sorting here.
  */
  read_time += COST_EPS;

  /*
    Calculate the selectivity of the ref_key for REF_ACCESS. For
    RANGE_ACCESS we use table->quick_condition_rows.
  */
  if (ref_key >= 0 && ref_key != MAX_KEY && tab->type == JT_REF)
  {
    /*
      If ref access uses keypart=const for all its key parts,
      and quick select uses the same # of key parts, then they are equivalent.
      Reuse #rows estimate from quick select as it is more precise.
    */
    if (tab->ref.const_ref_part_map ==
        make_prev_keypart_map(tab->ref.key_parts) &&
        table->quick_keys.is_set(ref_key) &&
        table->quick_key_parts[ref_key] == tab->ref.key_parts)
      refkey_rows_estimate= table->quick_rows[ref_key];
    else
    {
      const KEY *ref_keyinfo= table->key_info + ref_key;
      refkey_rows_estimate= ref_keyinfo->rec_per_key[tab->ref.key_parts - 1];
    }
    set_if_bigger(refkey_rows_estimate, 1);
  }

  for (nr=0; nr < table->s->keys ; nr++)
  {
    int direction;
    ha_rows select_limit= select_limit_arg;
    uint used_key_parts= 0;

    if (keys.is_set(nr) &&
        (direction= test_if_order_by_key(join, order, table, nr,
                                         &used_key_parts)))
    {
      /*
        At this point we are sure that ref_key is a non-ordering
        key (where "ordering key" is a key that will return rows
        in the order required by ORDER BY).
      */
      DBUG_ASSERT (ref_key != (int) nr);

      bool is_covering= (table->covering_keys.is_set(nr) ||
                         (table->file->index_flags(nr, 0, 1) &
                          HA_CLUSTERED_INDEX));
      /* 
        Don't use an index scan with ORDER BY without limit.
        For GROUP BY without limit always use index scan
        if there is a suitable index. 
        Why we hold to this asymmetry hardly can be explained
        rationally. It's easy to demonstrate that using
        temporary table + filesort could be cheaper for grouping
        queries too.
      */ 
      if (is_covering ||
          select_limit != HA_POS_ERROR || 
          (ref_key < 0 && (group || table->force_index)))
      { 
        double rec_per_key;
        double index_scan_time;
        KEY *keyinfo= table->key_info+nr;
        if (select_limit == HA_POS_ERROR)
          select_limit= table_records;
        if (group)
        {
          /* 
            Used_key_parts can be larger than keyinfo->user_defined_key_parts
            when using a secondary index clustered with a primary 
            key (e.g. as in Innodb). 
            See Bug #28591 for details.
          */  
          uint used_index_parts= keyinfo->user_defined_key_parts;
          uint used_pk_parts= 0;
          if (used_key_parts > used_index_parts)
            used_pk_parts= used_key_parts-used_index_parts;
          rec_per_key= used_key_parts ?
	               keyinfo->actual_rec_per_key(used_key_parts-1) : 1;
          /* Take into account the selectivity of the used pk prefix */
          if (used_pk_parts)
	  {
            KEY *pkinfo=tab->table->key_info+table->s->primary_key;
            /*
              If the values of of records per key for the prefixes
              of the primary key are considered unknown we assume
              they are equal to 1.
	    */
            if (used_key_parts == pkinfo->user_defined_key_parts ||
                pkinfo->rec_per_key[0] == 0)
              rec_per_key= 1;                 
            if (rec_per_key > 1)
	    {
              rec_per_key*= pkinfo->actual_rec_per_key(used_pk_parts-1);
              rec_per_key/= pkinfo->actual_rec_per_key(0);
              /* 
                The value of rec_per_key for the extended key has
                to be adjusted accordingly if some components of
                the secondary key are included in the primary key.
	      */
               for(uint i= 1; i < used_pk_parts; i++)
	      {
	        if (pkinfo->key_part[i].field->key_start.is_set(nr))
	        {
                  /* 
                    We presume here that for any index rec_per_key[i] != 0
                    if rec_per_key[0] != 0.
	          */
                  DBUG_ASSERT(pkinfo->actual_rec_per_key(i));
                  rec_per_key*= pkinfo->actual_rec_per_key(i-1);
                  rec_per_key/= pkinfo->actual_rec_per_key(i);
                }
	      }
            }    
          }
          set_if_bigger(rec_per_key, 1);
          /*
            With a grouping query each group containing on average
            rec_per_key records produces only one row that will
            be included into the result set.
          */  
          if (select_limit > table_records/rec_per_key)
            select_limit= table_records;
          else
            select_limit= (ha_rows) (select_limit*rec_per_key);
        } /* group */

        /* 
          If tab=tk is not the last joined table tn then to get first
          L records from the result set we can expect to retrieve
          only L/fanout(tk,tn) where fanout(tk,tn) says how many
          rows in the record set on average will match each row tk.
          Usually our estimates for fanouts are too pessimistic.
          So the estimate for L/fanout(tk,tn) will be too optimistic
          and as result we'll choose an index scan when using ref/range
          access + filesort will be cheaper.
        */
        select_limit= (ha_rows) (select_limit < fanout ?
                                 1 : select_limit/fanout);
        /*
          We assume that each of the tested indexes is not correlated
          with ref_key. Thus, to select first N records we have to scan
          N/selectivity(ref_key) index entries. 
          selectivity(ref_key) = #scanned_records/#table_records =
          refkey_rows_estimate/table_records.
          In any case we can't select more than #table_records.
          N/(refkey_rows_estimate/table_records) > table_records
          <=> N > refkey_rows_estimate.
         */
        if (select_limit > refkey_rows_estimate)
          select_limit= table_records;
        else
          select_limit= (ha_rows) (select_limit *
                                   (double) table_records /
                                    refkey_rows_estimate);
        rec_per_key= keyinfo->actual_rec_per_key(keyinfo->user_defined_key_parts-1);
        set_if_bigger(rec_per_key, 1);
        /*
          Here we take into account the fact that rows are
          accessed in sequences rec_per_key records in each.
          Rows in such a sequence are supposed to be ordered
          by rowid/primary key. When reading the data
          in a sequence we'll touch not more pages than the
          table file contains.
          TODO. Use the formula for a disk sweep sequential access
          to calculate the cost of accessing data rows for one 
          index entry.
        */
        index_scan_time= select_limit/rec_per_key *
                         MY_MIN(rec_per_key, table->file->scan_time());
        double range_scan_time;
        if (get_range_limit_read_cost(tab, table, nr, select_limit, 
                                       &range_scan_time))
        {
          if (range_scan_time < index_scan_time)
            index_scan_time= range_scan_time;
        }

        if ((ref_key < 0 && (group || table->force_index || is_covering)) ||
            index_scan_time < read_time)
        {
          ha_rows quick_records= table_records;
          ha_rows refkey_select_limit= (ref_key >= 0 &&
                                        !is_hash_join_key_no(ref_key) &&
                                        table->covering_keys.is_set(ref_key)) ?
                                        refkey_rows_estimate :
                                        HA_POS_ERROR;
          if ((is_best_covering && !is_covering) ||
              (is_covering && refkey_select_limit < select_limit))
            continue;
          if (table->quick_keys.is_set(nr))
            quick_records= table->quick_rows[nr];
          if (best_key < 0 ||
              (select_limit <= MY_MIN(quick_records,best_records) ?
               keyinfo->user_defined_key_parts < best_key_parts :
               quick_records < best_records) ||
              (!is_best_covering && is_covering))
          {
            best_key= nr;
            best_key_parts= keyinfo->user_defined_key_parts;
            if (saved_best_key_parts)
              *saved_best_key_parts= used_key_parts;
            best_records= quick_records;
            is_best_covering= is_covering;
            best_key_direction= direction; 
            best_select_limit= select_limit;
          }
        }   
      }      
    }
  }

  if (best_key < 0 || best_key == ref_key)
    DBUG_RETURN(FALSE);
  
  *new_key= best_key;
  *new_key_direction= best_key_direction;
  *new_select_limit= has_limit ? best_select_limit : table_records;
  if (new_used_key_parts != NULL)
    *new_used_key_parts= best_key_parts;

  DBUG_RETURN(TRUE);
}


/**
  Find a key to apply single table UPDATE/DELETE by a given ORDER

  @param       order           Linked list of ORDER BY arguments
  @param       table           Table to find a key
  @param       select          Pointer to access/update select->quick (if any)
  @param       limit           LIMIT clause parameter 
  @param [out] scanned_limit   How many records we expect to scan
                               Valid if *need_sort=FALSE.
  @param [out] need_sort       TRUE if filesort needed
  @param [out] reverse
    TRUE if the key is reversed again given ORDER (undefined if key == MAX_KEY)

  @return
    - MAX_KEY if no key found                        (need_sort == TRUE)
    - MAX_KEY if quick select result order is OK     (need_sort == FALSE)
    - key number (either index scan or quick select) (need_sort == FALSE)

  @note
    Side effects:
    - may deallocate or deallocate and replace select->quick;
    - may set table->quick_condition_rows and table->quick_rows[...]
      to table->file->stats.records. 
*/

uint get_index_for_order(ORDER *order, TABLE *table, SQL_SELECT *select,
                         ha_rows limit, ha_rows *scanned_limit,
                         bool *need_sort, bool *reverse)
{
  if (!order)
  {
    *need_sort= FALSE;
    if (select && select->quick)
      return select->quick->index; // index or MAX_KEY, use quick select as is
    else
      return table->file->key_used_on_scan; // MAX_KEY or index for some engines
  }

  if (!is_simple_order(order)) // just to cut further expensive checks
  {
    *need_sort= TRUE;
    return MAX_KEY;
  }

  if (select && select->quick)
  {
    if (select->quick->index == MAX_KEY)
    {
      *need_sort= TRUE;
      return MAX_KEY;
    }

    uint used_key_parts;
    switch (test_if_order_by_key(NULL, order, table, select->quick->index,
                                 &used_key_parts)) {
    case 1: // desired order
      *need_sort= FALSE; 
      *scanned_limit= MY_MIN(limit, select->quick->records);
      return select->quick->index;
    case 0: // unacceptable order
      *need_sort= TRUE;
      return MAX_KEY;
    case -1: // desired order, but opposite direction
      {
        QUICK_SELECT_I *reverse_quick;
        if ((reverse_quick=
               select->quick->make_reverse(used_key_parts)))
        {
          select->set_quick(reverse_quick);
          *need_sort= FALSE;
          *scanned_limit= MY_MIN(limit, select->quick->records);
          return select->quick->index;
        }
        else
        {
          *need_sort= TRUE;
          return MAX_KEY;
        }
      }
    }
    DBUG_ASSERT(0);
  }
  else if (limit != HA_POS_ERROR)
  { // check if some index scan & LIMIT is more efficient than filesort
    
    /*
      Update quick_condition_rows since single table UPDATE/DELETE procedures
      don't call make_join_statistics() and leave this variable uninitialized.
    */
    table->quick_condition_rows= table->stat_records();
    
    int key, direction;
    if (test_if_cheaper_ordering(NULL, order, table,
                                 table->keys_in_use_for_order_by, -1,
                                 limit,
                                 &key, &direction, &limit) &&
        !is_key_used(table, key, table->write_set))
    {
      *need_sort= FALSE;
      *scanned_limit= limit;
      *reverse= (direction < 0);
      return key;
    }
  }
  *need_sort= TRUE;
  return MAX_KEY;
}


/*
  Count how many times the specified conditions are true for first rows_to_read
  rows of the table.

  @param thd                  Thread handle
  @param rows_to_read         How many rows to sample
  @param table                Table to use
  @conds conds         INOUT  List of conditions and counters for them

  @return Number of we've checked. It can be equal or less than rows_to_read.
          0 is returned for error or when the table had no rows.
*/

ulong check_selectivity(THD *thd,
                        ulong rows_to_read,
                        TABLE *table,
                        List<COND_STATISTIC> *conds)
{
  ulong count= 0;
  COND_STATISTIC *cond;
  List_iterator_fast<COND_STATISTIC> it(*conds);
  handler *file= table->file;
  uchar *record= table->record[0];
  int error= 0;
  DBUG_ENTER("check_selectivity");

  DBUG_ASSERT(rows_to_read > 0);
  while ((cond= it++))
  {
    DBUG_ASSERT(cond->cond);
    DBUG_ASSERT(cond->cond->used_tables() == table->map);
    cond->positive= 0;
  }
  it.rewind();

  if (unlikely(file->ha_rnd_init_with_error(1)))
    DBUG_RETURN(0);
  do
  {
    error= file->ha_rnd_next(record);

    if (unlikely(thd->killed))
    {
      thd->send_kill_message();
      count= 0;
      goto err;
    }
    if (unlikely(error))
    {
      if (error == HA_ERR_END_OF_FILE)
	break;
      goto err;
    }

    count++;
    while ((cond= it++))
    {
      if (cond->cond->val_bool())
        cond->positive++;
    }
    it.rewind();

  } while (count < rows_to_read);

  file->ha_rnd_end();
  DBUG_RETURN(count);

err:
  DBUG_PRINT("error", ("error %d", error));
  file->ha_rnd_end();
  DBUG_RETURN(0);
}

/****************************************************************************
  AGGR_OP implementation
****************************************************************************/

/**
  @brief Instantiate tmp table for aggregation and start index scan if needed
  @todo Tmp table always would be created, even for empty result. Extend
        executor to avoid tmp table creation when no rows were written
        into tmp table.
  @return
    true  error
    false ok
*/

bool
AGGR_OP::prepare_tmp_table()
{
  TABLE *table= join_tab->table;
  JOIN *join= join_tab->join;
  int rc= 0;

  if (!join_tab->table->is_created())
  {
    if (instantiate_tmp_table(table, join_tab->tmp_table_param->keyinfo,
                              join_tab->tmp_table_param->start_recinfo,
                              &join_tab->tmp_table_param->recinfo,
                              join->select_options))
      return true;
    (void) table->file->extra(HA_EXTRA_WRITE_CACHE);
  }
  /* If it wasn't already, start index scan for grouping using table index. */
  if (!table->file->inited && table->group &&
      join_tab->tmp_table_param->sum_func_count && table->s->keys)
    rc= table->file->ha_index_init(0, 0);
  else
  {
    /* Start index scan in scanning mode */
    rc= table->file->ha_rnd_init(true);
  }
  if (rc)
  {
    table->file->print_error(rc, MYF(0));
    return true;
  }
  return false;
}


/**
  @brief Prepare table if necessary and call write_func to save record

  @param end_of_records  the end_of_record signal to pass to the writer

  @return return one of enum_nested_loop_state.
*/

enum_nested_loop_state
AGGR_OP::put_record(bool end_of_records)
{
  // Lasy tmp table creation/initialization
  if (!join_tab->table->file->inited)
    if (prepare_tmp_table())
      return NESTED_LOOP_ERROR;
  enum_nested_loop_state rc= (*write_func)(join_tab->join, join_tab,
                                           end_of_records);
  return rc;
}


/**
  @brief Finish rnd/index scan after accumulating records, switch ref_array,
         and send accumulated records further.
  @return return one of enum_nested_loop_state.
*/

enum_nested_loop_state
AGGR_OP::end_send()
{
  enum_nested_loop_state rc= NESTED_LOOP_OK;
  TABLE *table= join_tab->table;
  JOIN *join= join_tab->join;

  // All records were stored, send them further
  int tmp, new_errno= 0;

  if ((rc= put_record(true)) < NESTED_LOOP_OK)
    return rc;

  if ((tmp= table->file->extra(HA_EXTRA_NO_CACHE)))
  {
    DBUG_PRINT("error",("extra(HA_EXTRA_NO_CACHE) failed"));
    new_errno= tmp;
  }
  if ((tmp= table->file->ha_index_or_rnd_end()))
  {
    DBUG_PRINT("error",("ha_index_or_rnd_end() failed"));
    new_errno= tmp;
  }
  if (new_errno)
  {
    table->file->print_error(new_errno,MYF(0));
    return NESTED_LOOP_ERROR;
  }

  // Update ref array
  join_tab->join->set_items_ref_array(*join_tab->ref_array);
  bool keep_last_filesort_result = join_tab->filesort ? false : true;
  if (join_tab->window_funcs_step)
  {
    if (join_tab->window_funcs_step->exec(join, keep_last_filesort_result))
      return NESTED_LOOP_ERROR;
  }

  table->reginfo.lock_type= TL_UNLOCK;

  bool in_first_read= true;
  while (rc == NESTED_LOOP_OK)
  {
    int error;
    if (in_first_read)
    {
      in_first_read= false;
      error= join_init_read_record(join_tab);
    }
    else
      error= join_tab->read_record.read_record();

    if (unlikely(error > 0 || (join->thd->is_error())))   // Fatal error
      rc= NESTED_LOOP_ERROR;
    else if (error < 0)
      break;
    else if (unlikely(join->thd->killed))		  // Aborted by user
    {
      join->thd->send_kill_message();
      rc= NESTED_LOOP_KILLED;
    }
    else
    {
      rc= evaluate_join_record(join, join_tab, 0);
    }
  }

  if (keep_last_filesort_result)
  {
    delete join_tab->filesort_result;
    join_tab->filesort_result= NULL;
  }

  // Finish rnd scn after sending records
  if (join_tab->table->file->inited)
    join_tab->table->file->ha_rnd_end();

  return rc;
}


/**
  @brief
  Remove marked top conjuncts of a condition
    
  @param thd    The thread handle
  @param cond   The condition which subformulas are to be removed    

  @details
    The function removes all top conjuncts marked with the flag
    FULL_EXTRACTION_FL from the condition 'cond'. The resulting
    formula is returned a the result of the function
    If 'cond' s marked with such flag the function returns 0. 
    The function clear the extraction flags for the removed
    formulas

   @retval
     condition without removed subformulas
     0 if the whole 'cond' is removed
*/ 

Item *remove_pushed_top_conjuncts(THD *thd, Item *cond)
{
  if (cond->get_extraction_flag() == FULL_EXTRACTION_FL)
  {
    cond->clear_extraction_flag();
    return 0; 
  }
  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
      Item *item;
      while ((item= li++))
      {
	if (item->get_extraction_flag() == FULL_EXTRACTION_FL)
	{
	  item->clear_extraction_flag();
	  li.remove();
	}
      }
      switch (((Item_cond*) cond)->argument_list()->elements) 
      {
      case 0:
	return 0;			
      case 1:
	return ((Item_cond*) cond)->argument_list()->head();
      default:
	return cond;
      }
    }
  }
  return cond;
}

/*
  There are 5 cases in which we shortcut the join optimization process as we
  conclude that the join would be a degenerate one
    1) IMPOSSIBLE WHERE
    2) MIN/MAX optimization (@see opt_sum_query)
    3) EMPTY CONST TABLE
  If a window function is present in any of the above cases then to get the
  result of the window function, we need to execute it. So we need to
  create a temporary table for its execution. Here we need to take in mind
  that aggregate functions and non-aggregate function need not be executed.

*/


void JOIN::handle_implicit_grouping_with_window_funcs()
{
  if (select_lex->have_window_funcs() && send_row_on_empty_set())
  {
    const_tables= top_join_tab_count= table_count= 0;
  }
}


/*
  @brief
    Perform a partial cleanup for the JOIN_TAB structure

  @note
    this is used to cleanup resources for the re-execution of correlated
    subqueries.
*/
void JOIN_TAB::partial_cleanup()
{
  if (!table)
    return;

  if (table->is_created())
  {
    table->file->ha_index_or_rnd_end();
    DBUG_PRINT("info", ("close index: %s.%s  alias: %s",
               table->s->db.str,
               table->s->table_name.str,
               table->alias.c_ptr()));
    if (aggr)
    {
      int tmp= 0;
      if ((tmp= table->file->extra(HA_EXTRA_NO_CACHE)))
        table->file->print_error(tmp, MYF(0));
    }
  }
  delete filesort_result;
  filesort_result= NULL;
  free_cache(&read_record);
}


/**
  @} (end of group Query_Optimizer)
*/
