/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2017, MariaDB Corporation.

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


#include "mariadb.h"
#include "sql_select.h"
#include "opt_trace.h"

/**

INTRODUCTION

This file contains the functions to support the cost based ORDER BY with LIMIT
optimization.

The motivation behind this optimization is to shortcut the join execution
for queries having ORDER BY with LIMIT clause. In other words we would like to
avoid computing the entire join for queries having ORDER BY with LIMIT.

The main idea behind this optimization is to push the LIMIT to a partial join.
For pushing the LIMIT there is one pre-requisite and that is the partial
join MUST resolve the ORDER BY clause.

What does PUSHING THE LIMIT mean?

Pushing the limit to a partial join means that one would only read a fraction
of records of the prefix that are sorted in accordance with the ORDER BY
clause.


Let's say we have tables
  t1, t2, t3, t4 .............tk,tk+1.........................tn
  |<---------prefix------------>|<-------suffix--------------->|

and lets assume the prefix can resolve the ORDER BY clause and we can push
the LIMIT.

So considering the fraction of output we get in a general case with LIMIT is

            +-------------------------------------+
            |                                     |
fraction  = |  LIMIT / cardinality(t1,t2....tn)   |
            |                                     |
            +-------------------------------------+

We assume that the same fraction would be read for the prefix also, so the
records read for the prefix that can resolve the ORDER BY clause is:

              +--------------------------------------+
              |                                      |
records_read= | fraction * (cardinality(t1,t2....tk) |
              |                                      |
              +--------------------------------------+

              +--------------------------------------------------------------+
              |                                                              |
            = | LIMIT * (cardinality(t1,t2....tk) / cardinality(t1,t2....tn))|
              |                                                              |
              +--------------------------------------------------------------+

The LIMIT is pushed to all partial join orders enumerated by the join
planner that can resolve the ORDER BY clause. This is how we achieve a complete
cost based solution for ORDER BY with LIMIT optimization.


IMPLEMENTATION DETAILS

Let us divide the implementation details in 3 stages:

OPTIMIZATION STAGE

- The join planner is invoked to get an estimate of the cardinality for the
  join. This is needed to estimate the number of records that are needed to be
  read from the result of sorting.

- The cost of every potentially usable execution plan such that its first
  joined tables forms a bush the result of which is sorted in accordance with
  the ORDER BY clause. The evaluations takes into account that the LIMIT
  operation can be pushed right after the sort operation.

  The recursive procedure that enumerates such execution plans considers
  inserting a sort operation for any partial join prefix that can resolve the
  ORDER BY clause

  So for each such partial join prefix the procedure considers two options:
    1) to insert the sort operation immediately
    2) to add it later after expanding this partial join.

  For a partial prefix that cannot resolve the required ordering the procedure
  just extends the partial join.

- Access methods that ensure pre-existing ordering are also taken into account
  inside the join planner. There can be indexes on the first non-const table
  that can resolve the ORDER BY clause. So the LIMIT is also pushed to the
  first non-const table also in this case.

  This helps us to enumerate all plans where on can push LIMIT to different
  partial plans. Finally the plan with the lowest cost is picked by the join
  planner.

COMPILATION STAGE

A nest is a subset of join tables.
A materialized nest is a nest whose tables are joined together and result
is put inside a temporary table.
Sort nest is a materialized nest which can be sorted.

Preparation of Sort Nest

Let's say the best join order is:

  t1, t2, t3, t4 .............tk,tk+1.........................tn
  |<---------prefix------------>|<-------suffix--------------->|

The array of join_tab structures would look like

  t1, t2, t3, t4 .............tk, <sort nest>, tk+1.........................tn


Consider the  execution plan finally chosen by the planner.
This is a linear plan whose first node is a temporary table that is created for
the sort nest.

Join used for the sort nest is also executed by a linear plan.

                                  materialize
  t1, t2, t3, t4..............tk ============> <sort nest>
  |<---------prefix----------->|

  Here the sort nest is the first node as stated above:

  <sort nest> [sort], tk+1.........................tn
                      |<-------suffix-------------->|

To create the temporary table of the nest a list of Items that are going to be
stored inside the temporary table is needed. Currently this list contains
fields of the inner tables of the nest that have their bitmap read_set set.

After the temporary table for the sort nest is created the conditions that can
be pushed there are extracted from the WHERE clause. Thus the join with the
sort nest can use only remainder of the extraction. This new condition has to
be re-bound to refer to the columns of the temporary table  whenever references
to inner tables of the nest were used.

Similarly for ON clause, SELECT list, ORDER BY clause and REF items this
rebinding needs to be done.

EXECUTION STAGE

Let's say the best join order is:

  t1, t2, t3, t4 .............tk,tk+1.........................tn
  |<---------prefix------------>|<-------suffix--------------->|

  The prefix are the inner tables of the sort nest while the suffix are the
  tables outside the sort nest.

  On the execution stage, the join executor computes the partial join for the
  tables in the prefix and stores the result inside the temporary table of the
  sort nest.

  The join execution for this optimization can be split in 3 parts

  a) Materialize the prefix
                                     materialize
    t1, t2, t3, t4 .............tk  ============>  <sort nest>
    |<---------prefix------------>|

  b) Sort the <sort nest> in accordance with the ORDER BY clause

  c) Read records from the the result of sorting one by one and join
     with the tables in the suffix with NESTED LOOP JOIN

     <sort nest>, tk+1.........................tn
                  |<----------suffix----------->|

    The execution stops as soon as we get LIMIT records in the output.

*/

int test_if_order_by_key(JOIN *join, ORDER *order, TABLE *table, uint idx,
                         uint *used_key_parts= NULL);
COND* substitute_for_best_equal_field(THD *thd, JOIN_TAB *context_tab,
                                      COND *cond,
                                      COND_EQUAL *cond_equal,
                                      void *table_join_idx,
                                      bool do_substitution);
enum_nested_loop_state
end_nest_materialization(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);
bool get_range_limit_read_cost(const JOIN_TAB *tab, const TABLE *table,
                               ha_rows table_records, uint keynr,
                               ha_rows rows_limit, double *read_time);
Item **get_sargable_cond(JOIN *join, TABLE *table);
void find_cost_of_index_with_ordering(THD *thd, const JOIN_TAB *tab,
                                      TABLE *table,
                                      ha_rows *select_limit_arg,
                                      double fanout, double est_best_records,
                                      uint nr, double *index_scan_time,
                                      Json_writer_object *trace_possible_key);


static Item * const null_ptr= NULL;
/*
  @brief
    Substitute field items of tables inside the nest with nest's field items.

  @details
    Substitute field items of tables inside the sort-nest with sort-nest's
    field items. This is needed for expressions which would be
    evaluated in the post ORDER BY context.

    Example:

      SELECT * FROM t1, t2, t3
      WHERE t1.a = t2.a AND t2.b = t3.b AND t1.c > t3.c
      ORDER BY t1.a,t2.c
      LIMIT 5;

    Let's say in this case the join order is t1,t2,t3 and there is a sort-nest
    on the prefix t1,t2.

    Now looking at the WHERE clause, splitting it into 2 parts:
      (1) t2.b = t3.b AND t1.c > t3.c   ---> condition external to the nest
      (2) t1.a = t2.a                   ---> condition internal to the nest

    Now look at the condition in (1), this would be evaluated in the post
    ORDER BY context.

    So t2.b and t1.c should actually refer to the sort-nest's field items
    instead of field items of the tables inside the sort-nest.
    This is why we need to substitute field items of the tables inside the
    sort-nest with sort-nest's field items.

    For the condition in (2) there is no need for substitution as this
    condition is internal to the nest and would be evaluated before we
    do the sorting for the sort-nest.

    This function does the substitution for
      - WHERE clause
      - SELECT LIST
      - ORDER BY clause
      - ON expression
      - REF access items
*/

void
Mat_join_tab_nest_info::substitute_base_with_nest_field_items()
{
  THD *thd= join->thd;
  List_iterator<Item> it(join->fields_list);
  Item *item, *new_item;

  /* Substituting SELECT list field items with sort-nest's field items */
  while ((item= it++))
  {
    if ((new_item= item->transform(thd,
                                   &Item::replace_with_nest_items, TRUE,
                                   (uchar *) this)) != item)
    {
      new_item->name= item->name;
      thd->change_item_tree(it.ref(), new_item);
    }
    new_item->update_used_tables();
  }

  /* Substituting ORDER BY field items with sort-nest's field items */
  ORDER *ord;
  for (ord= join->order; ord ; ord=ord->next)
  {
    (*ord->item)= (*ord->item)->transform(thd,
                                          &Item::replace_with_nest_items,
                                          TRUE, (uchar *) this);
    (*ord->item)->update_used_tables();
  }

  JOIN_TAB *tab= nest_tab;
  for (uint i= join->const_tables + number_of_tables();
       (tab && i < join->top_join_tab_count); i++, tab++)
  {

    if (tab->type == JT_REF || tab->type == JT_EQ_REF ||
        tab->type == JT_REF_OR_NULL)
      substitute_ref_items(tab);

    /* Substituting ON-EXPR field items with sort-nest's field items */
    if (*tab->on_expr_ref)
    {
      item= (*tab->on_expr_ref)->transform(thd,
                                           &Item::replace_with_nest_items,
                                           TRUE, (uchar *) this);
      *tab->on_expr_ref= item;
      (*tab->on_expr_ref)->update_used_tables();
    }

    /*
      Substituting REF field items for SJM lookup with sort-nest's field items
    */
    if (tab->bush_children)
      substitutions_for_sjm_lookup(tab);
  }

  extract_condition_for_the_nest();

  /* Substituting WHERE clause's field items with sort-nest's field items */
  if (join->conds)
  {
    join->conds= join->conds->transform(thd, &Item::replace_with_nest_items,
                                        TRUE, (uchar *) this);
    join->conds->update_used_tables();
  }
}


/*
  @brief
    Substitute ref access field items with nest's field items.

  @param tab                join tab structure having ref access

*/

void Mat_join_tab_nest_info::substitute_ref_items(JOIN_TAB *tab)
{
  THD *thd= join->thd;
  Item *item;
  /* Substituting REF field items with sort-nest's field items */
  for (uint keypart= 0; keypart < tab->ref.key_parts; keypart++)
  {
    item= tab->ref.items[keypart]->transform(thd,
                                             &Item::replace_with_nest_items,
                                             TRUE, (uchar *) this);
    if (item != tab->ref.items[keypart])
    {
      tab->ref.items[keypart]= item;
      Item *real_item= item->real_item();
      store_key *key_copy= tab->ref.key_copy[keypart];
      if (key_copy->type() == store_key::FIELD_STORE_KEY)
      {
        store_key_field *field_copy= ((store_key_field *)key_copy);
        DBUG_ASSERT(real_item->type() == Item::FIELD_ITEM);
        field_copy->change_source_field((Item_field *) real_item);
      }
    }
  }
}


/*
  @brief
    Substitute the left expression of the IN subquery with nest's field items.

  @param sjm_tab           SJM lookup join tab

  @details
    This substitution is needed for SJM lookup when the SJM materialized
    table is outside the nest.

    For example:
      SELECT t1.a, t2.a
      FROM  t1, t2
      WHERE ot1.a in (SELECT it.b FROM it) AND ot1.b = t1.b
      ORDER BY t1.a desc, ot1.a desc
      LIMIT 5;

    Lets consider the join order here is t1, t2, <subquery2> and there is a
    nest on t1, t2. For <subquery2> we do SJM lookup.
    So for the SJM table there would be a ref access created on the condition
    t2.a=it.b. But as one can see table t2 is inside the nest and the
    condition t2.a=it.b can only be evaluated in the post nest creation
    context, so we need to substitute t2.a with the corresponding field item
    of the nest.

    For example:
      If we had a sort nest on t1,t2 the the condition t2.a = it.b will be
      evaluated in the POST ORDER BY context, so t2.a should refer to the
      field item of the sort nest.

*/

void Mat_join_tab_nest_info::substitutions_for_sjm_lookup(JOIN_TAB *sjm_tab)
{
  THD *thd= join->thd;
  JOIN_TAB *tab= sjm_tab->bush_children->start;
  TABLE_LIST *emb_sj_nest= tab->table->pos_in_table_list->embedding;

  /*
    @see setup_sj_materialization_part1
  */
  while (!emb_sj_nest->sj_mat_info)
    emb_sj_nest= emb_sj_nest->embedding;
  SJ_MATERIALIZATION_INFO *sjm= emb_sj_nest->sj_mat_info;

  if (!sjm->is_sj_scan)
  {
    Item *left_expr= emb_sj_nest->sj_subq_pred->left_expr;
    left_expr= left_expr->transform(thd, &Item::replace_with_nest_items,
                                    TRUE, (uchar *) this);
    left_expr->update_used_tables();
    emb_sj_nest->sj_subq_pred->left_expr= left_expr;
  }
}


/*
  @brief
    Extract from the WHERE clause the sub-condition for tables inside the nest.

  @details
    Extract the sub-condition from the WHERE clause that can be added to the
    tables inside the nest.

  Example
    SELECT * from t1,t2,t3
    WHERE t1.a > t2.a        (1)
     AND  t2.b = t3.b        (2)
    ORDER BY t1.a,t2.a
    LIMIT 5;

    let's say in this case the join order is t1,t2,t3 and there is a nest
    on t1,t2

    From the WHERE clause we would like to extract the condition that depends
    only on the inner tables of the nest. The condition (1) here satisfies
    this criteria so it would be extracted from the WHERE clause.
    The extracted condition here would be t1.a > t2.a.

    The extracted condition is stored inside the Mat_join_tab_nest_info
    structure.

    Also we remove the top level conjuncts of the WHERE clause that were
    present in the extracted condition.

    So after removal the final results would be:
      WHERE clause:    t2.b = t3.b         ----> condition external to the nest
      extracted cond:  t1.a > t2.a         ----> condition internal to the nest

    @note
      For the sort nest the sub-condition will be evaluated before the
      ORDER BY clause is applied.
*/

void Mat_join_tab_nest_info::extract_condition_for_the_nest()
{
  THD *thd= join->thd;
  Item *orig_cond= join->conds;
  Item *extracted_cond;

  /*
    check_pushable_cond_extraction would set the flag NO_EXTRACTION_FL for
    all the predicates that cannot be added to the inner tables of the nest.
  */
  table_map nest_tables_map= get_tables_map();
  join->conds->check_pushable_cond_extraction(
                                       &Item::pushable_cond_checker_for_tables,
                                       (uchar*)&nest_tables_map);

  /*
    build_pushable_condition would create a sub-condition that would be
    added to the inner tables of the nest. This may clone some predicates too.
  */
  extracted_cond= orig_cond->build_pushable_condition(thd, TRUE);

  if (extracted_cond)
  {
    if (extracted_cond->fix_fields_if_needed(thd, 0))
      return;
    extracted_cond->update_used_tables();
    /*
      Remove from the WHERE clause  the top level conjuncts that were
      extracted for the inner tables of the nest
    */
    orig_cond= remove_pushed_top_conjuncts(thd, orig_cond);
    set_nest_cond(extracted_cond);
  }
  join->conds= orig_cond;
}


/*
  @brief
    Propagate the Multiple Equalities for all the ORDER BY items.

  @details
    Propagate the multiple equalities for the ORDER BY items.
    This is needed so that we can generate different join orders
    that would satisfy ordering after taking equality propagation
    into consideration.

    Example
      SELECT * FROM t1, t2, t3
      WHERE t1.a = t2.a AND t2.b = t3.a
      ORDER BY t2.a, t3.a
      LIMIT 10;

    Possible join orders which satisfy the ORDER BY clause and
    which we can get after equality propagation are:
        - t2, sort(t2), t3, t1          ---> substitute t3.a with t2.b
        - t2, sort(t2), t1, t3          ---> substitute t3.a with t2.b
        - t1, t3, sort(t1,t3), t2       ---> substitute t2.a with t1.a
        - t1, t2, sort(t1,t2), t3       ---> substitute t3.a with t2.b

    So with equality propagation for ORDER BY items, we can get more
    join orders that could satisfy the ORDER BY clause.
*/

void JOIN::propagate_equal_field_for_orderby()
{
  if (!sort_nest_possible)
    return;
  ORDER *ord;
  for (ord= order; ord; ord= ord->next)
  {
    if (optimizer_flag(thd, OPTIMIZER_SWITCH_ORDERBY_EQ_PROP) && cond_equal)
    {
      Item *item= ord->item[0];
      /*
        TODO: equality substitution in the context of ORDER BY is
        sometimes allowed when it is not allowed in the general case.
        We make the below call for its side effect: it will locate the
        multiple equality the item belongs to and set item->item_equal
        accordingly.
      */
      (void)item->propagate_equal_fields(thd,
                                         Value_source::
                                         Context_identity(),
                                         cond_equal);
    }
  }
}


/*
  @brief
    Check whether ORDER BY items can be evaluated for a given prefix

  @param previous_tables  table_map of all the tables in the prefix
                          of the current partial plan

  @details
    Here we walk through the ORDER BY items and check if the prefix of the
    join resolves the ordering.
    Also we look at the multiple equalities for each item in the ORDER BY list
    to see if the ORDER BY items can be resolved by the given prefix.

    Example
      SELECT * FROM t1, t2, t3
      WHERE t1.a = t2.a AND t2.b = t3.a
      ORDER BY t2.a, t3.a
      LIMIT 10;

    Let's say the given prefix is table {t1,t3}, then this function would
    return TRUE because there is an equality condition t2.a=t1.a ,
    so t2.a can be resolved with t1.a. Hence the given prefix {t1,t3} would
    resolve the ORDER BY clause.

  @retval
   TRUE   ordering can be evaluated by the given prefix
   FALSE  otherwise

*/

bool JOIN::check_join_prefix_resolves_ordering(table_map previous_tables)
{
  DBUG_ASSERT(order);
  ORDER *ord;
  for (ord= order; ord; ord= ord->next)
  {
    Item *order_item= ord->item[0];
    table_map order_tables=order_item->used_tables();
    if (!(order_tables & ~previous_tables) ||
         (order_item->excl_dep_on_tables(previous_tables, FALSE)))
      continue;
    else
      return FALSE;
  }
  return TRUE;
}


/*
  @brief
    Check if the best plan has a sort-nest or not.

  @param
    n_tables[out]             set to the number of tables inside the sort-nest
    nest_tables_map[out]      map of tables inside the sort-nest
  @details
    This function walks through the JOIN::best_positions array
    which holds the best plan and checks if there is prefix for
    which the join planner had picked a sort-nest.

    Also this function computes a table map for tables that are inside the
    sort-nest

  @retval
    TRUE  sort-nest present
    FALSE no sort-nest present
*/

bool JOIN::check_if_sort_nest_present(uint* n_tables,
                                      table_map *nest_tables_map)
{
  if (!sort_nest_possible)
    return FALSE;

  uint tablenr;
  table_map nest_tables= 0;
  uint tables= 0;
  for (tablenr=const_tables ; tablenr < table_count ; tablenr++)
  {
    tables++;
    POSITION *pos= &best_positions[tablenr];
    if (pos->sj_strategy == SJ_OPT_MATERIALIZE ||
        pos->sj_strategy == SJ_OPT_MATERIALIZE_SCAN)
    {
      SJ_MATERIALIZATION_INFO *sjm= pos->table->emb_sj_nest->sj_mat_info;
      for (uint j= 0; j < sjm->tables; j++)
      {
        JOIN_TAB *tab= (pos+j)->table;
        nest_tables|= tab->table->map;
      }
      tablenr+= (sjm->tables-1);
    }
    else
      nest_tables|= pos->table->table->map;

    if (pos->sort_nest_operation_here)
    {
      *n_tables= tables;
      *nest_tables_map= nest_tables;
      return TRUE;
    }
  }
  return FALSE;
}


/*
  @brief
    Create a sort nest info structure

  @param n_tables          number of tables inside the sort-nest
  @param nest_tables_map   map of top level tables inside the sort-nest

  @details
    This sort-nest structure would hold all the information about the
    sort-nest.

  @retval
    FALSE     successful in creating the sort-nest info structure
    TRUE      error
*/

bool JOIN::create_sort_nest_info(uint n_tables, table_map nest_tables_map)
{
  if (!(sort_nest_info= new Sort_nest_info(this, n_tables, nest_tables_map)))
    return TRUE;
  return FALSE;
}


void JOIN::substitute_best_fields_for_order_by_items()
{
  ORDER *ord;
  /*
    Substitute the ORDER by items with the best field so that equality
    propagation considered during best_access_path can be used.
  */
  for (ord= order; ord; ord=ord->next)
  {
    Item *item= ord->item[0];
    item= substitute_for_best_equal_field(thd, NO_PARTICULAR_TAB, item,
                                          cond_equal,
                                          map2table, true);
    item->update_used_tables();
    ord->item[0]= item;
  }
}


/*
  @brief
    Make the sort-nest.

  @details
    Setup execution structures for sort-nest materialization:
      - Create the list of Items of the inner tables of the sort-nest
        that are needed for the post ORDER BY computations
      - Create the materialization temporary table for the sort-nest

    This function fills up the Sort_nest_info structure

  @retval
    TRUE   : In case of error
    FALSE  : Nest creation successful
*/

bool Mat_join_tab_nest_info::make_nest()
{
  Field_iterator_table field_iterator;

  JOIN_TAB *j;
  JOIN_TAB *tab;
  THD *thd= join->thd;

  if (unlikely(thd->trace_started()))
    add_nest_tables_to_trace(get_name());

  /*
    List of field items of the tables inside the sort-nest is created for
    the field items that are needed to be stored inside the temporary table
    of the sort-nest. Currently Item_field objects are created for the tables
    inside the sort-nest for all the  fields which have bitmap read_set
    set for them.

    TODO varun:
      An improvement would be if to remove the fields from this
      list that are completely internal to the nest because such
      fields would not be used in computing expression in the post
      ORDER BY context
  */

  for (j= start_tab; j < nest_tab; j++)
  {
    if (!j->bush_children)
    {
      TABLE *table= j->table;
      field_iterator.set_table(table);
      for (; !field_iterator.end_of_fields(); field_iterator.next())
      {
        Field *field= field_iterator.field();
        if (!bitmap_is_set(table->read_set, field->field_index))
          continue;
        Item *item;
        if (!(item= field_iterator.create_item(thd)))
          return TRUE;
        nest_base_table_cols.push_back(item, thd->mem_root);
      }
    }
    else
    {
      TABLE_LIST *emb_sj_nest;
      JOIN_TAB *child_tab= j->bush_children->start;
      emb_sj_nest= child_tab->table->pos_in_table_list->embedding;
      /*
        @see setup_sj_materialization_part1
      */
      while (!emb_sj_nest->sj_mat_info)
        emb_sj_nest= emb_sj_nest->embedding;
      Item_in_subselect *item_sub= emb_sj_nest->sj_subq_pred;
      SELECT_LEX *subq_select= item_sub->unit->first_select();
      List_iterator_fast<Item> li(subq_select->item_list);
      Item *item;
      while((item= li++))
        nest_base_table_cols.push_back(item, thd->mem_root);
    }
  }

  tab= nest_tab;
  DBUG_ASSERT(!tab->table);

  uint sort_nest_elements= nest_base_table_cols.elements;
  tmp_table_param.init();
  tmp_table_param.bit_fields_as_long= TRUE;
  tmp_table_param.field_count= sort_nest_elements;
  tmp_table_param.force_not_null_cols= FALSE;

  const LEX_CSTRING order_nest_name= { STRING_WITH_LEN("sort-nest") };
  if (!(tab->table= create_tmp_table(thd, &tmp_table_param,
                                     nest_base_table_cols,
                                     (ORDER*) 0,
                                     FALSE /* distinct */,
                                     0, /*save_sum_fields*/
                                     thd->variables.option_bits |
                                     TMP_TABLE_ALL_COLUMNS,
                                     HA_POS_ERROR /*rows_limit */,
                                     &order_nest_name)))
    return TRUE; /* purecov: inspected */

  tab->table->map= get_tables_map();
  table= tab->table;
  tab->type= JT_ALL;
  tab->table->reginfo.join_tab= tab;

  /*
    The list of temp table items created here, these are needed for the
    substitution for items that would be evaluated in POST SORT NEST context
  */
  field_iterator.set_table(tab->table);
  List_iterator_fast<Item> li(nest_base_table_cols);
  Item *item;
  for (; !field_iterator.end_of_fields() && (item= li++);
       field_iterator.next())
  {
    Field *field= field_iterator.field();
    Item *nest_item;
    if (!(nest_item= new (thd->mem_root)Item_temptable_field(thd, field)))
      return TRUE;
    Item_pair *tmp_field= new Item_pair(item, nest_item);
    mapping_of_items.push_back(tmp_field, thd->mem_root);
  }

  /* Setting up the scan on the temp table */
  tab->read_first_record= join_init_read_record;
  tab->read_record.read_record_func= rr_sequential;
  tab[-1].next_select= end_nest_materialization;
  DBUG_ASSERT(!is_materialized());

  return FALSE;
}


bool Sort_nest_info::make_sort_nest()
{
  return make_nest();
}


/*
  @brief
    setup the join tab for the materialized nest
*/

void Mat_join_tab_nest_info::setup_nest_join_tab(JOIN_TAB *nest_start)
{
  nest_tab->join= join;
  start_tab= nest_start;
  nest_tab->table= NULL;
  nest_tab->ref.key = -1;
  nest_tab->on_expr_ref= (Item**) &null_ptr;
  nest_tab->records_read= calculate_record_count_for_nest();
  nest_tab->records= (ha_rows) nest_tab->records_read;
  nest_tab->cond_selectivity= 1.0;
}


/*
  @brief
    Calculate the cost of adding a sort-nest.

  @param
    join_record_count   the cardinality of the partial join
    idx                 position of the joined table in the partial plan
    rec_len             estimate of length of the record in the sort-nest table

  @details
    The calculation for the cost of the sort-nest is done here, the cost
    includes three components
      1) Filling the sort-nest table
      2) Sorting the sort-nest table
      3) Reading from the sort-nest table
*/

double JOIN::sort_nest_oper_cost(double join_record_count, uint idx,
                                 ulong rec_len)
{
  double cost= 0;
  set_if_bigger(join_record_count, 1);
  /*
    The sort-nest table is not created for sorting when one does sorting
    on the first non-const table. So for this case we don't need to add
    the cost of filling the table.
  */
  if (idx != const_tables)
    cost=  get_tmp_table_write_cost(thd, join_record_count, rec_len) *
           join_record_count; // cost to fill temp table

  // cost to perform  sorting
  cost+= get_tmp_table_lookup_cost(thd, join_record_count, rec_len) +
         (join_record_count == 0 ? 0 :
          join_record_count * log2 (join_record_count)) *
         SORT_INDEX_CMP_COST;

  /*
    cost for scanning the temp table.
    Picked this cost from get_delayed_table_estimates()
  */
  double data_size= COST_MULT(join_record_count * fraction_output_for_nest,
                              rec_len);
  cost+= data_size/IO_SIZE + 2;

  return cost;
}


/*
  @brief
    Calculate the number of records that would be read from the nest.

  @retval Number of records that the optimizer expects to be read from the
          nest
*/

double Mat_join_tab_nest_info::calculate_record_count_for_nest()
{
  double nest_records= 1, record_count;
  for (JOIN_TAB *tab= start_tab; tab < nest_tab; tab++)
  {
    record_count= tab->records_read * tab->cond_selectivity;
    nest_records= COST_MULT(nest_records, record_count);
  }
  return nest_records;
}

/*
  @brief
    Calculate the number of records that would be read from the sort-nest.

  @details
    The number of records read from the sort-nest would be:

      cardinality(join of inner table of nest) * selectivity_of_limit;

    Here selectivity of limit is how many records we would expect in the
    output.
      selectivity_of_limit= limit / cardinality(join of all tables)

    This number of records is what we would also see in the EXPLAIN output
    for the sort-nest in the column "rows".

  @retval Number of records that the optimizer expects to be read from the
          sort-nest
*/

double Sort_nest_info::calculate_record_count_for_nest()
{
  double records;
  records= Mat_join_tab_nest_info::calculate_record_count_for_nest();
  records= records * join->fraction_output_for_nest;
  set_if_bigger(records, 1);
  return records;
}


/*
  @brief
    Find all keys that can resolve the ORDER BY clause for a table

  @details
    This function sets the flag TABLE::keys_with_ordering with all the
    indexes of a table that can resolve the ORDER BY clause.
*/

void JOIN_TAB::find_keys_that_can_achieve_ordering()
{
  if (!join->sort_nest_possible)
    return;

  table->keys_with_ordering.clear_all();
  for (uint index= 0; index < table->s->keys; index++)
  {
    if (table->keys_in_use_for_query.is_set(index) &&
        test_if_order_by_key(join, join->order, table, index))
      table->keys_with_ordering.set_bit(index);
  }
  //  INDEX HINTS for ORDER BY may be provided
  table->keys_with_ordering.intersect(table->keys_in_use_for_order_by);
}


/*
  @brief
    Checks if the given prefix needs Filesort for ordering.

  @param
    idx            position of the joined table in the partial plan
    index_used     >=0 number of the index that is picked as best access
                   -1  no index access chosen

  @details
    Here we check if a given prefix requires Filesort or index on the
    first non-const table to resolve the ORDER BY clause.

  @retval
    TRUE   Filesort is needed
    FALSE  index present that satisfies the ordering
*/

bool JOIN_TAB::needs_filesort(uint idx, int index_used)
{
  if (idx != join->const_tables)
    return TRUE;

  return !check_if_index_satisfies_ordering(index_used);
}


/*
  @brief
    Find a cheaper index that resolves ordering on the first non-const table.

  @param
    tab                       joined table
    read_time [out]           cost for the best index picked if cheaper
    records   [out]           estimate of records going to be accessed by the
                              index
    index_used                >=0 number of index used for best access
                              -1  no index used for best access
    idx                       position of the joined table in the partial plan

  @details
    Here we try to walk through all the indexes for the first non-const table
    of a given prefix.
    From these indexes we are only interested in the indexes that can resolve
    the ORDER BY clause as we want to shortcut the join execution for ORDER BY
    LIMIT optimization.

    For each index we are interested in we try to estimate the records we
    have to read to ensure #limit records in the join output.

    Then with this estimate of records we calculate the cost of using an index
    and try to find the best index for access.
    If the best index found from here has a lower cost than the best access
    found in best_access_path, we switch the access to use the index found
    here.

  @retval
    -1     no cheaper index found for ordering
    >=0    cheaper index found for ordering
*/

int get_best_index_for_order_by_limit(JOIN_TAB *tab,
                                      ha_rows select_limit_arg,
                                      double *read_time,
                                      double *records,
                                      int index_used,
                                      uint idx)
{
  double cardinality;
  JOIN *join= tab->join;
  cardinality= join->cardinality_estimate;
  /**
    Cases when there is no need to consider indexes that can resolve the
    ORDER BY clause

    1) Table in consideration should be the first non-const table.
    2) Query does not use the ORDER BY LIMIT optimization with sort_nest
       @see sort_nest_allowed
    3) Join planner is run to get an estimate of cardinality for a join
    4) No index present that can resolve the ORDER BY clause
  */

  if (idx != join->const_tables ||                             // (1)
      !join->sort_nest_possible ||                             // (2)
      join->get_cardinality_estimate ||                        // (3)
      tab->table->keys_with_ordering.is_clear_all())           // (4)
    return -1;

  THD *thd= join->thd;
  Json_writer_object trace_index_for_ordering(thd);
  TABLE *table= tab->table;
  double save_read_time= *read_time;
  double save_records= *records;
  double est_records= *records;
  double fanout= cardinality / est_records;
  int best_index=-1;
  trace_index_for_ordering.add("rows_estimation", est_records);
  Json_writer_array considered_indexes(thd, "considered_indexes");

  for (uint idx= 0 ; idx < table->s->keys; idx++)
  {
    ha_rows select_limit= select_limit_arg;
    if (!table->keys_with_ordering.is_set(idx))
      continue;
    Json_writer_object possible_key(thd);
    double index_scan_time;
    possible_key.add("index", table->key_info[idx].name);
    find_cost_of_index_with_ordering(thd, tab, table, &select_limit,
                                     fanout, est_records,
                                     idx, &index_scan_time,
                                     &possible_key);

    if (index_scan_time < *read_time)
    {
      best_index= idx;
      *read_time= index_scan_time;
      *records= (double)select_limit;
    }
  }
  considered_indexes.end();

  if (unlikely(thd->trace_started()))
  {
    trace_index_for_ordering.add("best_index",
                                 static_cast<ulonglong>(best_index));
    trace_index_for_ordering.add("records", *records);
    trace_index_for_ordering.add("best_cost", *read_time);
  }

  /*
    If an index already found satisfied the ordering and we picked an index
    for which we choose to do index scan then revert the cost and stick
    with the access picked first.
    Index scan would not help in comparison with ref access.
  */
  if (tab->check_if_index_satisfies_ordering(index_used))
  {
    if (!table->quick_keys.is_set(static_cast<uint>(index_used)))
    {
      best_index= -1;
      *records= save_records;
      *read_time= save_read_time;
    }
  }
  return best_index;
}


/*
  @brief
    Disallow join buffering for tables that are read after sorting is done.

  @param
    tab                      table to check if join buffering is allowed or not

  @details
    Disallow join buffering for all the tables at the top level that are read
    after sorting is done.
    There are 2 cases
      1) Sorting on the first non-const table
         For all the tables join buffering is not allowed
      2) Sorting on a prefix of the join with a sort-nest
         For the tables inside the sort-nest join buffering is allowed but
         for tables outside the sort-nest join buffering is not allowed

    Also for SJM table that come after the sort-nest, join buffering is allowed
    for the inner tables of the SJM.

  @retval
    TRUE   Join buffering is allowed
    FALSE  Otherwise
*/

bool JOIN::is_join_buffering_allowed(JOIN_TAB *tab)
{
  if (!sort_nest_info)
    return TRUE;

  // no need to disable join buffering for the inner tables of SJM
  if (tab->bush_root_tab)
    return TRUE;

  if (tab->table->map & sort_nest_info->get_tables_map())
    return TRUE;
  return FALSE;
}


/*
  @brief
    Check if an index on a table resolves the ORDER BY clause.

  @param
    index_used                  index to be checked

  @retval
    TRUE  index resolves the ORDER BY clause
    FALSE otherwise
*/

bool JOIN_TAB::check_if_index_satisfies_ordering(int index_used)
{
  /*
    index_used is set to
    -1          for Table Scan
    MAX_KEY     for HASH JOIN
    >=0         for ref/range/index access
  */
  if (index_used < 0 || index_used == MAX_KEY)
    return FALSE;

  if (table->keys_with_ordering.is_set(static_cast<uint>(index_used)))
    return TRUE;
  return FALSE;
}


/*
  @brief
    Set up range scan for the table.

  @param
    tab                    table for which range scan needs to be setup
    idx                    index for which range scan needs to created
    records                estimate of records to be read with range scan

  @details
    Range scan is setup here for an index that can resolve the ORDER BY clause.
    There are 2 cases here:
      1) If the range scan is on the same index for which we created
         QUICK_SELECT when we ran the range optimizer earlier, then we try
         to reuse it.
      2) The range scan is on a different index then we need to create
         QUICK_SELECT for the new key. This is done by running the range
         optimizer again.

    Also here we take into account if the ordering is in reverse direction.
    For DESCENDING we try to reverse the QUICK_SELECT.

  @note
    This is done for the ORDER BY LIMIT optimization. We try to force creation
    of range scan for an index that the join planner picked for us. Also here
    we reverse the range scan if the ordering is in reverse direction.
*/

void JOIN::setup_range_scan(JOIN_TAB *tab, uint idx, double records)
{
  SQL_SELECT *sel= NULL;
  Item **sargable_cond= get_sargable_cond(this, tab->table);
  int err, rc, direction;
  uint used_key_parts;
  key_map keymap_for_range;
  Json_writer_array forcing_range(thd, "range_scan_for_order_by_limit");

  sel= make_select(tab->table, const_table_map, const_table_map,
                   *sargable_cond, (SORT_INFO*) 0, 1, &err);
  if (!sel)
    goto use_filesort;

  /*
    If the table already had a range access, check if it is the same as the
    one we wanted to create range scan for, if yes don't run the range
    optimizer again.
  */

  if (!(tab->quick && tab->quick->index == idx))
  {
    /* Free the QUICK_SELECT that was built earlier. */
    delete tab->quick;
    tab->quick= NULL;

    keymap_for_range.clear_all();  // Force the creation of quick select
    keymap_for_range.set_bit(idx); // only for index using range access.

    rc= sel->test_quick_select(thd, keymap_for_range,
                               (table_map) 0,
                               (ha_rows) HA_POS_ERROR,
                               true, false, true, true);
    if (rc <= 0)
      goto use_filesort;
  }
  else
    sel->quick= tab->quick;

  direction= test_if_order_by_key(this, order, tab->table, idx,
                                  &used_key_parts);

  if (direction == -1)
  {
    /*
      QUICK structure is reversed here as the ordering is in DESC order
    */
    QUICK_SELECT_I *reverse_quick;
    if (sel && sel->quick)
    {
      reverse_quick= sel->quick->make_reverse(used_key_parts);
      if (!reverse_quick)
        goto use_filesort;
      sel->set_quick(reverse_quick);
    }
  }

  tab->quick= sel->quick;

  /*
    Fix for explain, the records here should be set to the value
    which was stored in the JOIN::best_positions object. This is needed
    because the estimate of rows to be read for the first non-const table had
    taken selectivity of limit into account.
  */
  if (sort_nest_possible && records < tab->quick->records)
    tab->quick->records= (ha_rows)records;

  sel->quick= NULL;

use_filesort:
  delete sel;
}


/*
  @brief
    Setup range/index scan to resolve ordering on the first non-const table.

  @details
    Here we try to prepare range scan or index scan for an index that can be
    used to resolve the ORDER BY clause. This is used only for the first
    non-const table of the join.

    For range scan
      There is a separate call to setup_range_scan, where the QUICK_SELECT is
      created for range access. In case we are not able to create a range
      access, we switch back to use Filesort on the first table.
      see @setup_range_scan
    For index scan
      We just store the index in Sort_nest_info::index_used.
*/

void JOIN::setup_index_use_for_ordering()
{
  DBUG_ASSERT(sort_nest_info->index_used == -1);

  int index= best_positions[const_tables].index_no;
  sort_nest_info->nest_tab= join_tab + const_tables;
  POSITION *cur_pos= &best_positions[const_tables];
  JOIN_TAB *tab= cur_pos->table;

  if (cur_pos->key) // Ref access
    return;

  index= (index == -1) ?
         (cur_pos->table->quick ? cur_pos->table->quick->index : -1) :
         index;

  if (tab->check_if_index_satisfies_ordering(index))
  {
    if (tab->table->quick_keys.is_set(index))
    {
      // Range scan
      setup_range_scan(tab, index, cur_pos->records_read);
      sort_nest_info->index_used= -1;
    }
    else
    {
       // Index scan
      if (tab->quick)
      {
        delete tab->quick;
        tab->quick= NULL;
      }
      sort_nest_info->index_used= index;
    }
  }
}


/*
  @brief
    Get index used to access the table, if present

  @retval
    >=0 index used to access the table
    -1  no index used to access table, probably table scan is done
*/

int JOIN_TAB::get_index_on_table()
{
  int idx= -1;

  if (type == JT_REF  || type == JT_EQ_REF || type == JT_REF_OR_NULL)
    idx= ref.key;
  else if (type == JT_NEXT)
    idx= index;
  else if (type == JT_ALL && select && select->quick)
    idx= select->quick->index;
  return idx;
}


/*
  @brief
    Calculate the selectivity of limit.

  @details
    The selectivity of limit is calculated as
        selecitivity_of_limit=  rows_in_limit / cardinality_of_join

  @note
    The selectivity that we get is used to make an estimate of rows
    that we would read from the partial join of the tables inside the
    sort-nest.
*/

void JOIN::set_fraction_output_for_nest()
{
  if (sort_nest_possible && !get_cardinality_estimate)
  {
    fraction_output_for_nest= select_limit < cardinality_estimate ?
                              select_limit / cardinality_estimate :
                              1.0;
    if (unlikely(thd->trace_started()))
    {
      Json_writer_object trace_limit(thd);
      trace_limit.add("cardinality", cardinality_estimate);
      trace_limit.add("selectivity_of_limit", fraction_output_for_nest*100);
    }
  }
}


/*
  @brief
    Sort nest is allowed when one can shortcut the join execution.

  @details
    For all the operations where one requires entire join computation to be
    done first and then apply the operation on the join output,
    such operations can't make use of the sort-nest.
    So this function disables the use of sort-nest for such operations.

    Sort nest is not allowed for
    1) No ORDER BY clause
    2) Only constant tables in the join
    3) DISTINCT CLAUSE
    4) GROUP BY CLAUSE
    5) HAVING clause
    6) Aggregate Functions
    7) Window Functions
    8) Using ROLLUP
    9) Using SQL_BUFFER_RESULT
    10) LIMIT is absent
    11) Only SELECT queries can use the sort nest

  @retval
   TRUE     Sort-nest is allowed
   FALSE    Otherwise

*/

bool JOIN::sort_nest_allowed()
{
  return optimizer_flag(thd, OPTIMIZER_SWITCH_COST_BASED_ORDER_BY_LIMIT) &&
         order &&
         !(const_tables == table_count ||
           (select_distinct || group_list) ||
           having  ||
           MY_TEST(select_options & OPTION_BUFFER_RESULT) ||
           (rollup.state != ROLLUP::STATE_NONE && select_distinct) ||
           select_lex->window_specs.elements > 0 ||
           select_lex->agg_func_used() ||
           select_limit == HA_POS_ERROR ||
           thd->lex->sql_command != SQLCOM_SELECT ||
           select_lex->uncacheable & UNCACHEABLE_DEPENDENT ||
           MY_TEST(select_options & SELECT_STRAIGHT_JOIN));
}


/*
  @brief
    Check if indexes on a table are allowed to resolve the ORDER BY clause

  @param
    idx                       position of the table in the partial plan

  @retval
    TRUE    Indexes are allowed to resolve ORDER BY clause
    FALSE   Otherwise

*/

bool JOIN::is_index_with_ordering_allowed(uint idx)
{
  /*
    An index on a table can allowed to resolve ordering in these cases:
      1) Table should be the first non-const table
      2) Query that allows the ORDER BY LIMIT optimization.
         @see sort_nest_allowed
      3) Join planner is not run to get the estimate of cardinality
  */
  return  idx == const_tables &&                                   // (1)
          sort_nest_possible &&                                    // (2)
          !get_cardinality_estimate;                               // (3)
}


/*
  @brief
    Consider adding a sort-nest on a prefix of the join

  @param prefix_tables           map of all the tables in the prefix

  @details
    This function is used during the join planning stage, where the join
    planner decides if it can add a sort-nest on a prefix of a join.
    The join planner does not add the sort-nest in the following cases:
      1) Queries where adding a sort-nest is not possible.
         see @sort_nest_allowed
      2) Join planner is run to get the cardinality of the join
      3) All inner tables of an outer join are inside the nest or outside
      4) All inner tables of a semi-join are inside the nest or outside
      5) Given prefix cannot resolve the ORDER BY clause

  @retval
    TRUE   sort-nest can be added on a prefix of a join
    FALSE  otherwise
*/

bool JOIN::consider_adding_sort_nest(table_map prefix_tables, uint idx)
{
  if (!sort_nest_possible ||                        // (1)
      get_cardinality_estimate ||                   // (2)
      cur_embedding_map ||                          // (3)
      cur_sj_inner_tables ||                        // (4)
      extend_prefix_to_ensure_duplicate_removal(prefix_tables, idx))
    return FALSE;

  return check_join_prefix_resolves_ordering(prefix_tables);  // (5)
}


bool
JOIN::extend_prefix_to_ensure_duplicate_removal(table_map prefix_tables, uint idx)
{
  if (!select_lex->have_merged_subqueries)
    return FALSE;

  POSITION *pos= positions + idx;
  Semi_join_strategy_picker *pickers[]=
  {
    &pos->firstmatch_picker,
    &pos->loosescan_picker,
    &pos->sjmat_picker,
    &pos->dups_weedout_picker,
    NULL,
  };
  Semi_join_strategy_picker **strategy;
  for (strategy= pickers; *strategy != NULL; strategy++)
  {
    if ((*strategy)->sort_nest_allowed_for_sj(prefix_tables))
      return TRUE;
  }
  return FALSE;
}


/*
  @brief
    Find the cost to access a table with an index that can resolve ORDER BY.

  @param
    THD                                  thread structure
    tab                                  join_tab structure for joined table
    table                                first non-const table
    select_limit_arg                     limit for the query
    fanout                               fanout of the join
    est_best_records                     estimate of records for best access
    nr                                   index number
    index_scan_time[out]                 cost to access the table with the
                                         the index
*/

void find_cost_of_index_with_ordering(THD *thd, const JOIN_TAB *tab,
                                      TABLE *table,
                                      ha_rows *select_limit_arg,
                                      double fanout, double est_best_records,
                                      uint nr, double *index_scan_time,
                                      Json_writer_object *trace_possible_key)
{
  KEY *keyinfo= table->key_info + nr;
  ha_rows select_limit= *select_limit_arg;
  double rec_per_key;
  ha_rows table_records= table->stat_records();
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
    refkey_rows_estimate is E(#rows) produced by the table access
    strategy that was picked without regard to ORDER BY ... LIMIT.

    It will be used as the source of selectivity data.
    Use table->cond_selectivity as a better estimate which includes
    condition selectivity too.
  */
  {
    // we use MIN(...), because "Using LooseScan" queries have
    // cond_selectivity=1 while refkey_rows_estimate has a better
    // estimate.
    est_best_records= MY_MIN(est_best_records,
                             ha_rows(table_records * table->cond_selectivity));
  }

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

  if (select_limit > est_best_records)
    select_limit= table_records;
  else
    select_limit= (ha_rows) (select_limit *
                             (double) table_records /
                              est_best_records);

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
  *index_scan_time= select_limit/rec_per_key *
                    MY_MIN(rec_per_key, table->file->scan_time());

  if (unlikely(thd->trace_started()))
  {
    trace_possible_key->add("updated_limit", select_limit);
    trace_possible_key->add("index_scan_time", *index_scan_time);
  }

  double range_scan_time;
  if (get_range_limit_read_cost(tab, table, table_records, nr,
                                select_limit, &range_scan_time))
  {
    trace_possible_key->add("range_scan_time", range_scan_time);
    if (range_scan_time < *index_scan_time)
      *index_scan_time= range_scan_time;
  }
  *select_limit_arg= select_limit;
}

