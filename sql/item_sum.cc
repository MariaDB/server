/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB

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


/**
  @file

  @brief
  Sum functions (COUNT, MIN...)
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "uniques.h"
#include "sp_rcontext.h"
#include "sp.h"
#include "sql_parse.h"
#include "sp_head.h"

/**
  Calculate the affordable RAM limit for structures like TREE or Unique
  used in Item_sum_*
*/

size_t Item_sum::ram_limitation(THD *thd)
{
  return MY_MAX(1024,
           (size_t)MY_MIN(thd->variables.tmp_memory_table_size,
                          thd->variables.max_heap_table_size));
}


/*
  Force create_tmp_table() to convert BIT columns to BIGINT.
  This is needed because BIT fields store parts of their data in table's
  null bits, and we don't have methods to compare two table records with
  bit fields.
*/

static void store_bit_fields_as_bigint_in_tempory_table(List<Item> *list)
{
  List_iterator_fast<Item> li(*list);
  Item *item;
  while ((item= li++))
  {
    if (item->type() == Item::FIELD_ITEM &&
        ((Item_field*) item)->field->type() == FIELD_TYPE_BIT)
      item->marker= MARKER_NULL_KEY;
  }
}

/**
  Prepare an aggregate function item for checking context conditions.

    The function initializes the members of the Item_sum object created
    for a set function that are used to check validity of the set function
    occurrence.
    If the set function is not allowed in any subquery where it occurs
    an error is reported immediately.

  @param thd      reference to the thread context info

  @note
    This function is to be called for any item created for a set function
    object when the traversal of trees built for expressions used in the query
    is performed at the phase of context analysis. This function is to
    be invoked at the descent of this traversal.
  @retval
    TRUE   if an error is reported
  @retval
    FALSE  otherwise
*/
 
bool Item_sum::init_sum_func_check(THD *thd)
{
  SELECT_LEX *curr_sel= thd->lex->current_select;
  if (curr_sel && curr_sel->name_visibility_map.is_clear_all())
  {
    for (SELECT_LEX *sl= curr_sel; sl; sl= sl->context.outer_select())
    {
      curr_sel->name_visibility_map.set_bit(sl->nest_level);
    }
  }
  if (!curr_sel ||
      !(thd->lex->allow_sum_func.is_overlapping(curr_sel->name_visibility_map)))
  {
    my_message(ER_INVALID_GROUP_FUNC_USE, ER_THD(thd, ER_INVALID_GROUP_FUNC_USE),
               MYF(0));
    return TRUE;
  }
  /* Set a reference to the nesting set function if there is  any */
  in_sum_func= thd->lex->in_sum_func;
  /* Save a pointer to object to be used in items for nested set functions */
  thd->lex->in_sum_func= this;
  nest_level= thd->lex->current_select->nest_level;
  ref_by= 0;
  aggr_level= -1;
  aggr_sel= NULL;
  max_arg_level= -1;
  max_sum_func_level= -1;
  outer_fields.empty();
  return FALSE;
}

/**
  Check constraints imposed on a usage of a set function.

    The method verifies whether context conditions imposed on a usage
    of any set function are met for this occurrence.

    The function first checks if we are using any window functions as
    arguments to the set function. In that case it returns an error.

    Afterwards, it checks whether the set function occurs in the position where it
    can be aggregated and, when it happens to occur in argument of another
    set function, the method checks that these two functions are aggregated in
    different subqueries.
    If the context conditions are not met the method reports an error.
    If the set function is aggregated in some outer subquery the method
    adds it to the chain of items for such set functions that is attached
    to the the st_select_lex structure for this subquery.

    A number of designated members of the object are used to check the
    conditions. They are specified in the comment before the Item_sum
    class declaration.
    Additionally a bitmap variable called allow_sum_func is employed.
    It is included into the thd->lex structure.
    The bitmap contains 1 at n-th position if the set function happens
    to occur under a construct of the n-th level subquery where usage
    of set functions are allowed (i.e either in the SELECT list or
    in the HAVING clause of the corresponding subquery)
    Consider the query:
    @code
       SELECT SUM(t1.b) FROM t1 GROUP BY t1.a
         HAVING t1.a IN (SELECT t2.c FROM t2 WHERE AVG(t1.b) > 20) AND
                t1.a > (SELECT MIN(t2.d) FROM t2);
    @endcode
    allow_sum_func will contain: 
    - for SUM(t1.b) - 1 at the first position 
    - for AVG(t1.b) - 1 at the first position, 0 at the second position
    - for MIN(t2.d) - 1 at the first position, 1 at the second position.

  @param thd  reference to the thread context info
  @param ref  location of the pointer to this item in the embedding expression

  @note
    This function is to be called for any item created for a set function
    object when the traversal of trees built for expressions used in the query
    is performed at the phase of context analysis. This function is to
    be invoked at the ascent of this traversal.

  @retval
    TRUE   if an error is reported
  @retval
    FALSE  otherwise
*/
 
bool Item_sum::check_sum_func(THD *thd, Item **ref)
{
  SELECT_LEX *curr_sel= thd->lex->current_select;
  nesting_map allow_sum_func(thd->lex->allow_sum_func);
  allow_sum_func.intersect(curr_sel->name_visibility_map);
  bool invalid= FALSE;
  // should be set already
  DBUG_ASSERT(!curr_sel->name_visibility_map.is_clear_all());

  /*
     Window functions can not be used as arguments to sum functions.
     Aggregation happes before window function computation, so there
     are no values to aggregate over.
  */
  if (with_window_func())
  {
    my_message(ER_SUM_FUNC_WITH_WINDOW_FUNC_AS_ARG,
               ER_THD(thd, ER_SUM_FUNC_WITH_WINDOW_FUNC_AS_ARG),
               MYF(0));
    return TRUE;
  }

  if (window_func_sum_expr_flag)
    return false;
  /*  
    The value of max_arg_level is updated if an argument of the set function
    contains a column reference resolved  against a subquery whose level is
    greater than the current value of max_arg_level.
    max_arg_level cannot be greater than nest level.
    nest level is always >= 0  
  */ 
  if (nest_level == max_arg_level)
  {
    /*
      The function must be aggregated in the current subquery, 
      If it is there under a construct where it is not allowed 
      we report an error. 
    */ 
    invalid= !(allow_sum_func.is_set(max_arg_level));
  }
  else if (max_arg_level >= 0 ||
           !(allow_sum_func.is_set(nest_level)))
  {
    /*
      The set function can be aggregated only in outer subqueries.
      Try to find a subquery where it can be aggregated;
      If we fail to find such a subquery report an error.
    */
    if (register_sum_func(thd, ref))
      return TRUE;
    invalid= aggr_level < 0 &&
             !(allow_sum_func.is_set(nest_level));
    if (!invalid && thd->variables.sql_mode & MODE_ANSI)
      invalid= aggr_level < 0 && max_arg_level < nest_level;
  }
  if (!invalid && aggr_level < 0)
  {
    aggr_level= nest_level;
    aggr_sel= curr_sel;
  }
  /*
    By this moment we either found a subquery where the set function is
    to be aggregated  and assigned a value that is  >= 0 to aggr_level,
    or set the value of 'invalid' to TRUE to report later an error. 
  */
  /* 
    Additionally we have to check whether possible nested set functions
    are acceptable here: they are not, if the level of aggregation of
    some of them is less than aggr_level.
  */
  if (!invalid) 
    invalid= aggr_level <= max_sum_func_level;
  if (invalid)  
  {
    my_message(ER_INVALID_GROUP_FUNC_USE,
               ER_THD(thd, ER_INVALID_GROUP_FUNC_USE),
               MYF(0));
    return TRUE;
  }

  if (in_sum_func)
  {
    /*
      If the set function is nested adjust the value of
      max_sum_func_level for the nesting set function.
      We take into account only enclosed set functions that are to be 
      aggregated on the same level or above of the nest level of 
      the enclosing set function.
      But we must always pass up the max_sum_func_level because it is
      the maximum nested level of all directly and indirectly enclosed
      set functions. We must do that even for set functions that are
      aggregated inside of their enclosing set function's nest level
      because the enclosing function may contain another enclosing
      function that is to be aggregated outside or on the same level
      as its parent's nest level.
    */
    if (in_sum_func->nest_level >= aggr_level)
      set_if_bigger(in_sum_func->max_sum_func_level, aggr_level);
    set_if_bigger(in_sum_func->max_sum_func_level, max_sum_func_level);
  }

  /*
    Check that non-aggregated fields and sum functions aren't mixed in the
    same select in the ONLY_FULL_GROUP_BY mode.
  */
  if (outer_fields.elements)
  {
    Item_field *field;
    /*
      Here we compare the nesting level of the select to which an outer field
      belongs to with the aggregation level of the sum function. All fields in
      the outer_fields list are checked.

      If the nesting level is equal to the aggregation level then the field is
        aggregated by this sum function.
      If the nesting level is less than the aggregation level then the field
        belongs to an outer select. In this case if there is an embedding sum
        function add current field to functions outer_fields list. If there is
        no embedding function then the current field treated as non aggregated
        and the select it belongs to is marked accordingly.
      If the nesting level is greater than the aggregation level then it means
        that this field was added by an inner sum function.
        Consider an example:

          select avg ( <-- we are here, checking outer.f1
            select (
              select sum(outer.f1 + inner.f1) from inner
            ) from outer)
          from most_outer;

        In this case we check that no aggregate functions are used in the
        select the field belongs to. If there are some then an error is
        raised.
    */
    List_iterator<Item_field> of(outer_fields);
    while ((field= of++))
    {
      SELECT_LEX *sel= field->field->table->pos_in_table_list->select_lex;
      if (sel->nest_level < aggr_level)
      {
        if (in_sum_func)
        {
          /*
            Let upper function decide whether this field is a non
            aggregated one.
          */
          in_sum_func->outer_fields.push_back(field, thd->mem_root);
        }
        else
          sel->set_non_agg_field_used(true);
      }
      if (sel->nest_level > aggr_level &&
          (sel->agg_func_used()) &&
          !sel->group_list.elements)
      {
        my_message(ER_MIX_OF_GROUP_FUNC_AND_FIELDS,
                   ER_THD(thd, ER_MIX_OF_GROUP_FUNC_AND_FIELDS), MYF(0));
        return TRUE;
      }
    }
  }
  aggr_sel->set_agg_func_used(true);
  if (sum_func() == SP_AGGREGATE_FUNC)
    aggr_sel->set_custom_agg_func_used(true);
  update_used_tables();
  thd->lex->in_sum_func= in_sum_func;
  return FALSE;
}

/**
  Attach a set function to the subquery where it must be aggregated.

    The function looks for an outer subquery where the set function must be
    aggregated. If it finds such a subquery then aggr_level is set to
    the nest level of this subquery and the item for the set function
    is added to the list of set functions used in nested subqueries
    inner_sum_func_list defined for each subquery. When the item is placed 
    there the field 'ref_by' is set to ref.

  @note
    Now we 'register' only set functions that are aggregated in outer
    subqueries. Actually it makes sense to link all set function for
    a subquery in one chain. It would simplify the process of 'splitting'
    for set functions.

  @param thd  reference to the thread context info
  @param ref  location of the pointer to this item in the embedding expression

  @retval
    FALSE  if the executes without failures (currently always)
  @retval
    TRUE   otherwise
*/  

bool Item_sum::register_sum_func(THD *thd, Item **ref)
{
  SELECT_LEX *sl;
  nesting_map allow_sum_func= thd->lex->allow_sum_func;
  for (sl= thd->lex->current_select->context.outer_select() ;
       sl && sl->nest_level > max_arg_level;
       sl= sl->context.outer_select())
  {
    if (aggr_level < 0 &&
        (allow_sum_func.is_set(sl->nest_level)))
    {
      /* Found the most nested subquery where the function can be aggregated */
      aggr_level= sl->nest_level;
      aggr_sel= sl;
    }
  }
  if (sl && (allow_sum_func.is_set(sl->nest_level)))
  {
    /* 
      We reached the subquery of level max_arg_level and checked
      that the function can be aggregated here. 
      The set function will be aggregated in this subquery.
    */   
    aggr_level= sl->nest_level;
    aggr_sel= sl;

  }
  if (aggr_level >= 0)
  {
    ref_by= ref;
    /* Add the object to the list of registered objects assigned to aggr_sel */
    if (!aggr_sel->inner_sum_func_list)
      next= this;
    else
    {
      next= aggr_sel->inner_sum_func_list->next;
      aggr_sel->inner_sum_func_list->next= this;
    }
    aggr_sel->inner_sum_func_list= this;
    aggr_sel->with_sum_func= 1;

    /* 
      Mark Item_subselect(s) as containing aggregate function all the way up
      to aggregate function's calculation context.
      Note that we must not mark the Item of calculation context itself
      because with_sum_func on the calculation context st_select_lex is
      already set above.

      with_sum_func being set for an Item means that this Item refers 
      (somewhere in it, e.g. one of its arguments if it's a function) directly
      or through intermediate items to an aggregate function that is calculated
      in a context "outside" of the Item (e.g. in the current or outer select).

      with_sum_func being set for an st_select_lex means that this st_select_lex
      has aggregate functions directly referenced (i.e. not through a sub-select).
    */
    for (sl= thd->lex->current_select; 
         sl && sl != aggr_sel && sl->master_unit()->item;
         sl= sl->master_unit()->outer_select() )
      sl->master_unit()->item->with_flags|= item_with_t::SUM_FUNC;
  }
  thd->lex->current_select->mark_as_dependent(thd, aggr_sel, NULL);

  if ((thd->lex->describe & DESCRIBE_EXTENDED) && aggr_sel)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_WARN_AGGFUNC_DEPENDENCE,
                        ER_THD(thd, ER_WARN_AGGFUNC_DEPENDENCE),
                        func_name(),
                        thd->lex->current_select->select_number,
                        aggr_sel->select_number);
  }
  return FALSE;
}


bool Item_sum::collect_outer_ref_processor(void *param)
{
  Collect_deps_prm *prm= (Collect_deps_prm *)param;
  SELECT_LEX *ds;
  if ((ds= depended_from()) &&
      ds->nest_level_base == prm->nest_level_base &&
      ds->nest_level < prm->nest_level)
  {
    if (prm->collect)
      prm->parameters->add_unique(this, &cmp_items);
    else
      prm->count++;
  }
  return FALSE;
}


Item_sum::Item_sum(THD *thd, List<Item> &list): Item_func_or_sum(thd, list)
{
  if (!(orig_args= (Item **) thd->alloc(sizeof(Item *) * arg_count)))
  {
    args= NULL;
  }
  mark_as_sum_func();
  init_aggregator();
  list.empty();					// Fields are used
}


/**
  Constructor used in processing select with temporary tebles.
*/

Item_sum::Item_sum(THD *thd, Item_sum *item):
  Item_func_or_sum(thd, item),
  aggr_sel(item->aggr_sel),
  nest_level(item->nest_level), aggr_level(item->aggr_level),
  quick_group(item->quick_group),
  orig_args(NULL)
{
  if (arg_count <= 2)
  {
    orig_args=tmp_orig_args;
  }
  else
  {
    if (!(orig_args= (Item**) thd->alloc(sizeof(Item*)*arg_count)))
      return;
  }
  if (arg_count)
    memcpy(orig_args, item->orig_args, sizeof(Item*)*arg_count);
  init_aggregator();
  with_distinct= item->with_distinct;
  if (item->aggr)
    set_aggregator(thd, item->aggr->Aggrtype());
}


void Item_sum::mark_as_sum_func()
{
  SELECT_LEX *cur_select= current_thd->lex->current_select;
  cur_select->n_sum_items++;
  cur_select->with_sum_func= 1;
  const_item_cache= false;
  with_flags= (with_flags | item_with_t::SUM_FUNC) & ~item_with_t::FIELD;
  window_func_sum_expr_flag= false;
}


void Item_sum::print(String *str, enum_query_type query_type)
{
  /* orig_args is not filled with valid values until fix_fields() */
  Item **pargs= fixed() ? orig_args : args;
  str->append(func_name_cstring());
  /*
    TODO:
    The fact that func_name() may return a name with an extra '('
    is really annoying. This shoud be fixed.
  */
  if (!is_aggr_sum_func())
    str->append('(');
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (i)
      str->append(',');
    pargs[i]->print(str, query_type);
  }
  str->append(')');
}

void Item_sum::fix_num_length_and_dec()
{
  decimals=0;
  for (uint i=0 ; i < arg_count ; i++)
    set_if_bigger(decimals,args[i]->decimals);
  max_length=float_length(decimals);
}

Item *Item_sum::get_tmp_table_item(THD *thd)
{
  Item_sum* sum_item= (Item_sum *) copy_or_same(thd);
  if (sum_item && sum_item->result_field)	   // If not a const sum func
  {
    Field *result_field_tmp= sum_item->result_field;
    for (uint i=0 ; i < sum_item->arg_count ; i++)
    {
      Item *arg= sum_item->args[i];
      if (!arg->const_item())
      {
        if (arg->type() == Item::FIELD_ITEM)
        {
          ((Item_field*) arg)->field= result_field_tmp++;
        }
        else
        {
          auto item_field=
            new (thd->mem_root) Item_field(thd, result_field_tmp++);
          if (item_field)
            item_field->set_refers_to_temp_table(true);
          sum_item->args[i]= item_field;
        }
      }
    }
  }
  return sum_item;
}


void Item_sum::update_used_tables ()
{
  if (!Item_sum::const_item())
  {
    used_tables_cache= 0;
    for (uint i=0 ; i < arg_count ; i++)
    {
      args[i]->update_used_tables();
      used_tables_cache|= args[i]->used_tables();
    }
    /*
      MariaDB: don't run the following {
      
      used_tables_cache&= PSEUDO_TABLE_BITS;

      // the aggregate function is aggregated into its local context
      used_tables_cache|= ((table_map)1 << aggr_sel->join->tables) - 1;
      
      } because if we do it, table elimination will assume that
        - constructs like "COUNT(*)" use columns from all tables
        - so, it is not possible to eliminate any table
      our solution for COUNT(*) is that it has
        item->used_tables() == 0 && !item->const_item()
    */
  }
}


Item *Item_sum::set_arg(uint i, THD *thd, Item *new_val) 
{
  thd->change_item_tree(args + i, new_val);
  return new_val;
}


int Item_sum::set_aggregator(THD *thd, Aggregator::Aggregator_type aggregator)
{
  /*
    Dependent subselects may be executed multiple times, making
    set_aggregator to be called multiple times. The aggregator type
    will be the same, but it needs to be reset so that it is
    reevaluated with the new dependent data.
    This function may also be called multiple times during query optimization.
    In this case, the type may change, so we delete the old aggregator,
    and create a new one.
  */
  if (aggr && aggregator == aggr->Aggrtype())
  {
    aggr->clear();
    return FALSE;
  }

  delete aggr;
  switch (aggregator)
  {
  case Aggregator::DISTINCT_AGGREGATOR:
    aggr= new (thd->mem_root) Aggregator_distinct(this);
    break;
  case Aggregator::SIMPLE_AGGREGATOR:
    aggr= new (thd->mem_root) Aggregator_simple(this);
    break;
  };
  return aggr ? FALSE : TRUE;
}


void Item_sum::cleanup()
{
  if (aggr)
  {
    delete aggr;
    aggr= NULL;
  }
  Item_result_field::cleanup();
  const_item_cache= false;
}

Item *Item_sum::result_item(THD *thd, Field *field)
{
  return new (thd->mem_root) Item_field(thd, field);
}

bool Item_sum::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function(func_name(), 
                                   is_aggr_sum_func() ? ")" : "()",
                                   arg, VCOL_IMPOSSIBLE);
}


/**
  Compare keys consisting of single field that cannot be compared as binary.
 
  Used by the Unique class to compare keys. Will do correct comparisons
  for all field types.

  @param    arg     Pointer to the relevant Field class instance
  @param    key1    left key image
  @param    key2    right key image
  @return   comparison result
    @retval < 0       if key1 < key2
    @retval = 0       if key1 = key2
    @retval > 0       if key1 > key2
*/

int simple_str_key_cmp(void* arg, uchar* key1, uchar* key2)
{
  Field *f= (Field*) arg;
  return f->cmp(key1, key2);
}


C_MODE_START

int count_distinct_walk(void *elem, element_count count, void *arg)
{
  (*((ulonglong*)arg))++;
  return 0;
}

C_MODE_END


/**
  Correctly compare composite keys.
 
  Used by the Unique class to compare keys. Will do correct comparisons
  for composite keys with various field types.

  @param arg     Pointer to the relevant Aggregator_distinct instance
  @param key1    left key image
  @param key2    right key image
  @return        comparison result
    @retval <0       if key1 < key2
    @retval =0       if key1 = key2
    @retval >0       if key1 > key2
*/

int Aggregator_distinct::composite_key_cmp(void* arg, uchar* key1, uchar* key2)
{
  Aggregator_distinct *aggr= (Aggregator_distinct *) arg;
  Field **field    = aggr->table->field;
  Field **field_end= field + aggr->table->s->fields;
  uint32 *lengths=aggr->field_lengths;
  for (; field < field_end; ++field)
  {
    Field* f = *field;
    int len = *lengths++;
    int res = f->cmp(key1, key2);
    if (res)
      return res;
    key1 += len;
    key2 += len;
  }
  return 0;
}


/***************************************************************************/

C_MODE_START

/* Declarations for auxiliary C-callbacks */

int simple_raw_key_cmp(void* arg, const void* key1, const void* key2)
{
    return memcmp(key1, key2, *(uint *) arg);
}


static int item_sum_distinct_walk_for_count(void *element, 
                                            element_count num_of_dups,
                                            void *item)
{
  return ((Aggregator_distinct*) (item))->unique_walk_function_for_count(element);
}
 

static int item_sum_distinct_walk(void *element, element_count num_of_dups,
                                  void *item)
{
  return ((Aggregator_distinct*) (item))->unique_walk_function(element);
}

C_MODE_END

/***************************************************************************/
/**
  Called before feeding the first row. Used to allocate/setup
  the internal structures used for aggregation.
 
  @param thd Thread descriptor
  @return status
    @retval FALSE success
    @retval TRUE  failure  

    Prepares Aggregator_distinct to process the incoming stream.
    Creates the temporary table and the Unique class if needed.
    Called by Item_sum::aggregator_setup()
*/

bool Aggregator_distinct::setup(THD *thd)
{
  endup_done= FALSE;
  /*
    Setup can be called twice for ROLLUP items. This is a bug.
    Please add DBUG_ASSERT(tree == 0) here when it's fixed.
  */
  if (tree || table || tmp_table_param)
    return FALSE;

  if (item_sum->setup(thd))
    return TRUE;
  if (item_sum->sum_func() == Item_sum::COUNT_FUNC || 
      item_sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC)
  {
    List<Item> list;
    SELECT_LEX *select_lex= thd->lex->current_select;

    if (!(tmp_table_param= new (thd->mem_root) TMP_TABLE_PARAM))
      return TRUE;

    /* Create a table with an unique key over all parameters */
    for (uint i=0; i < item_sum->get_arg_count() ; i++)
    {
      Item *item=item_sum->get_arg(i);
      if (list.push_back(item, thd->mem_root))
        return TRUE;                              // End of memory
      if (item->const_item() && item->is_null())
        always_null= true;
    }
    if (always_null)
      return FALSE;
    count_field_types(select_lex, tmp_table_param, list, 0);
    tmp_table_param->force_copy_fields= item_sum->has_force_copy_fields();
    DBUG_ASSERT(table == 0);
    /*
      Convert bit fields to bigint's in temporary table.
      Needed by Unique which is used when HEAP table is used.
    */
    store_bit_fields_as_bigint_in_tempory_table(&list);

    if (!(table= create_tmp_table(thd, tmp_table_param, list, (ORDER*) 0, 1,
                                  0,
                                  (select_lex->options |
                                   thd->variables.option_bits),
                                  HA_POS_ERROR, &empty_clex_str)))
      return TRUE;
    table->file->extra(HA_EXTRA_NO_ROWS);		// Don't update rows
    table->no_rows=1;

    if (table->s->db_type() == heap_hton)
    {
      /*
        No blobs, otherwise it would have been MyISAM: set up a compare
        function and its arguments to use with Unique.
      */
      qsort_cmp2 compare_key;
      void* cmp_arg;
      Field **field= table->field;
      Field **field_end= field + table->s->fields;
      bool all_binary= TRUE;

      for (tree_key_length= 0; field < field_end; ++field)
      {
        Field *f= *field;
        enum enum_field_types type= f->type();
        tree_key_length+= f->pack_length();
        if ((type == MYSQL_TYPE_VARCHAR) ||
            (!f->binary() && (type == MYSQL_TYPE_STRING ||
                             type == MYSQL_TYPE_VAR_STRING)))
        {
          all_binary= FALSE;
          break;
        }
      }
      if (all_binary)
      {
        cmp_arg= (void*) &tree_key_length;
        compare_key= (qsort_cmp2) simple_raw_key_cmp;
      }
      else
      {
        if (table->s->fields == 1)
        {
          /*
            If we have only one field, which is the most common use of
            count(distinct), it is much faster to use a simpler key
            compare method that can take advantage of not having to worry
            about other fields.
          */
          compare_key= (qsort_cmp2) simple_str_key_cmp;
          cmp_arg= (void*) table->field[0];
          /* tree_key_length has been set already */
        }
        else
        {
          uint32 *length;
          compare_key= (qsort_cmp2) composite_key_cmp;
          cmp_arg= (void*) this;
          field_lengths= (uint32*) thd->alloc(table->s->fields * sizeof(uint32));
          for (tree_key_length= 0, length= field_lengths, field= table->field;
               field < field_end; ++field, ++length)
          {
            *length= (*field)->pack_length();
            tree_key_length+= *length;
          }
        }
      }
      DBUG_ASSERT(tree == 0);
      tree= (new (thd->mem_root)
             Unique(compare_key, cmp_arg, tree_key_length,
                    item_sum->ram_limitation(thd)));
      /*
        The only time tree_key_length could be 0 is if someone does
        count(distinct) on a char(0) field - stupid thing to do,
        but this has to be handled - otherwise someone can crash
        the server with a DoS attack
      */
      if (! tree)
        return TRUE;
    }
    return FALSE;
  }
  else
  {
    Item *arg;
    DBUG_ENTER("Aggregator_distinct::setup");
    /* It's legal to call setup() more than once when in a subquery */
    if (tree)
      DBUG_RETURN(FALSE);

    /*
      Virtual table and the tree are created anew on each re-execution of
      PS/SP. Hence all further allocations are performed in the runtime
      mem_root.
    */

    item_sum->null_value= 1;
    item_sum->set_maybe_null();
    item_sum->quick_group= 0;

    DBUG_ASSERT(item_sum->get_arg(0)->fixed());

    arg= item_sum->get_arg(0);
    if (arg->const_item())
    {
      (void) arg->is_null();
      if (arg->null_value)
        always_null= true;
    }

    if (always_null)
      DBUG_RETURN(FALSE);

    Field *field= arg->type_handler()->
                    make_num_distinct_aggregator_field(thd->mem_root, arg);
    if (!field || !(table= create_virtual_tmp_table(thd, field)))
      DBUG_RETURN(TRUE);

    /* XXX: check that the case of CHAR(0) works OK */
    tree_key_length= table->s->reclength - table->s->null_bytes;

    /*
      Unique handles all unique elements in a tree until they can't fit
      in.  Then the tree is dumped to the temporary file. We can use
      simple_raw_key_cmp because the table contains numbers only; decimals
      are converted to binary representation as well.
    */
    tree= (new (thd->mem_root)
           Unique(simple_raw_key_cmp, &tree_key_length, tree_key_length,
                  item_sum->ram_limitation(thd)));

    DBUG_RETURN(tree == 0);
  }
}


/**
  Invalidate calculated value and clear the distinct rows.
 
  Frees space used by the internal data structures.
  Removes the accumulated distinct rows. Invalidates the calculated result.
*/

void Aggregator_distinct::clear()
{
  endup_done= FALSE;
  item_sum->clear();
  if (tree)
    tree->reset();
  /* tree and table can be both null only if always_null */
  if (item_sum->sum_func() == Item_sum::COUNT_FUNC || 
      item_sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC)
  {
    if (!tree && table)
    {
      table->file->extra(HA_EXTRA_NO_CACHE);
      table->file->ha_delete_all_rows();
      table->file->extra(HA_EXTRA_WRITE_CACHE);
    }
  }
  else
  {
    item_sum->null_value= 1;
  }
}


/**
  Process incoming row. 
  
  Add it to Unique/temp hash table if it's unique. Skip the row if 
  not unique.
  Prepare Aggregator_distinct to process the incoming stream.
  Create the temporary table and the Unique class if needed.
  Called by Item_sum::aggregator_add().
  To actually get the result value in item_sum's buffers 
  Aggregator_distinct::endup() must be called.

  @return status
    @retval FALSE     success
    @retval TRUE      failure
*/

bool Aggregator_distinct::add()
{
  if (always_null)
    return 0;

  if (item_sum->sum_func() == Item_sum::COUNT_FUNC || 
      item_sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC)
  {
    int error;
    copy_fields(tmp_table_param);
    if (copy_funcs(tmp_table_param->items_to_copy, table->in_use))
      return TRUE;

    for (Field **field=table->field ; *field ; field++)
      if ((*field)->is_real_null(0))
        return 0;					// Don't count NULL

    if (tree)
    {
      /*
        The first few bytes of record (at least one) are just markers
        for deleted and NULLs. We want to skip them since they will
        bloat the tree without providing any valuable info. Besides,
        key_length used to initialize the tree didn't include space for them.
      */
      return tree->unique_add(table->record[0] + table->s->null_bytes);
    }
    if (unlikely((error= table->file->ha_write_tmp_row(table->record[0]))) &&
        table->file->is_fatal_error(error, HA_CHECK_DUP))
      return TRUE;
    return FALSE;
  }
  else
  {
    item_sum->get_arg(0)->save_in_field(table->field[0], FALSE);
    if (table->field[0]->is_null())
      return 0;
    DBUG_ASSERT(tree);
    item_sum->null_value= 0;
    /*
      '0' values are also stored in the tree. This doesn't matter
      for SUM(DISTINCT), but is important for AVG(DISTINCT)
    */
    return tree->unique_add(table->field[0]->ptr);
  }
}


/**
  Calculate the aggregate function value.
 
  Since Distinct_aggregator::add() just collects the distinct rows,
  we must go over the distinct rows and feed them to the aggregation
  function before returning its value.
  This is what endup () does. It also sets the result validity flag
  endup_done to TRUE so it will not recalculate the aggregate value
  again if the Item_sum hasn't been reset.
*/

void Aggregator_distinct::endup()
{
  /* prevent consecutive recalculations */
  if (endup_done)
    return;

  /* we are going to calculate the aggregate value afresh */
  item_sum->clear();

  /* The result will definitely be null : no more calculations needed */
  if (always_null)
    return;

  if (item_sum->sum_func() == Item_sum::COUNT_FUNC || 
      item_sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC)
  {
    DBUG_ASSERT(item_sum->fixed());
    Item_sum_count *sum= (Item_sum_count *)item_sum;
    if (tree && tree->elements == 0)
    {
      /* everything fits in memory */
      sum->count= (longlong) tree->elements_in_tree();
      endup_done= TRUE;
    }
    if (!tree)
    {
      /* there were blobs */
      table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
      sum->count= table->file->stats.records;
      endup_done= TRUE;
    }
  }

 /*
   We don't have a tree only if 'setup()' hasn't been called;
   this is the case of sql_executor.cc:return_zero_rows.
 */
  if (tree && !endup_done)
  {
   /*
     All tree's values are not NULL.
     Note that value of field is changed as we walk the tree, in
     Aggregator_distinct::unique_walk_function, but it's always not NULL.
   */
   table->field[0]->set_notnull();
    /* go over the tree of distinct keys and calculate the aggregate value */
    use_distinct_values= TRUE;
    tree_walk_action func;
    if (item_sum->sum_func() == Item_sum::COUNT_DISTINCT_FUNC)
      func= item_sum_distinct_walk_for_count;
    else
      func= item_sum_distinct_walk;
    tree->walk(table, func, (void*) this);
    use_distinct_values= FALSE;
  }
  /* prevent consecutive recalculations */
  endup_done= TRUE;
}


String *
Item_sum_int::val_str(String *str)
{
  return val_string_from_int(str);
}


my_decimal *Item_sum_int::val_decimal(my_decimal *decimal_value)
{
  return val_decimal_from_int(decimal_value);
}


bool
Item_sum_num::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);

  if (init_sum_func_check(thd))
    return TRUE;

  decimals=0;
  set_maybe_null(sum_func() != COUNT_FUNC);
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (args[i]->fix_fields_if_needed_for_scalar(thd, &args[i]))
      return TRUE;
    set_if_bigger(decimals, args[i]->decimals);
    /* We should ignore FIELD's in arguments to sum functions */
    with_flags|= (args[i]->with_flags & ~item_with_t::FIELD);
  }
  result_field=0;
  max_length=float_length(decimals);
  null_value=1;
  if (fix_length_and_dec(thd) ||
      check_sum_func(thd, ref))
    return TRUE;

  if (arg_count)
    memcpy (orig_args, args, sizeof (Item *) * arg_count);
  base_flags|= item_base_t::FIXED;
  return FALSE;
}


bool
Item_sum_min_max::fix_fields(THD *thd, Item **ref)
{
  DBUG_ENTER("Item_sum_min_max::fix_fields");
  DBUG_ASSERT(fixed() == 0);

  if (init_sum_func_check(thd))
    DBUG_RETURN(TRUE);

  // 'item' can be changed during fix_fields
  if (args[0]->fix_fields_if_needed_for_scalar(thd, &args[0]))
    DBUG_RETURN(TRUE);

  /* We should ignore FIELD's in arguments to sum functions */
  with_flags|= (args[0]->with_flags & ~item_with_t::FIELD);
  if (fix_length_and_dec(thd))
    DBUG_RETURN(TRUE);

  if (!is_window_func_sum_expr())
    setup_hybrid(thd, args[0], NULL);
  result_field=0;

  if (check_sum_func(thd, ref))
    DBUG_RETURN(TRUE);

  orig_args[0]= args[0];
  base_flags|= item_base_t::FIXED;
  DBUG_RETURN(FALSE);
}


bool Item_sum_hybrid::fix_length_and_dec_generic()
{
  Item *item= arguments()[0];
  Type_std_attributes::set(item);
  set_handler(item->type_handler());
  return false;
}


/**
  MAX/MIN for the traditional numeric types preserve the exact data type
  from Fields, but do not preserve the exact type from Items:
    MAX(float_field)              -> FLOAT
    MAX(smallint_field)           -> LONGLONG
    MAX(COALESCE(float_field))    -> DOUBLE
    MAX(COALESCE(smallint_field)) -> LONGLONG
  QQ: Items should probably be fixed to preserve the exact type.
*/
bool Item_sum_hybrid::fix_length_and_dec_numeric(const Type_handler *handler)
{
  Item *item= arguments()[0];
  Item *item2= item->real_item();
  Type_std_attributes::set(item);
  if (item2->type() == Item::FIELD_ITEM)
    set_handler(item2->type_handler());
  else
    set_handler(handler);
  return false;
}


/**
   MAX(str_field) converts ENUM/SET to CHAR, and preserve all other types
   for Fields.
   QQ: This works differently from UNION, which preserve the exact data
   type for ENUM/SET if the joined ENUM/SET fields are equally defined.
   Perhaps should be fixed.
   MAX(str_item) chooses the best suitable string type.
*/
bool Item_sum_hybrid::fix_length_and_dec_string()
{
  Item *item= arguments()[0];
  Item *item2= item->real_item();
  Type_std_attributes::set(item);
  if (item2->type() == Item::FIELD_ITEM)
  {
    // Fields: convert ENUM/SET to CHAR, preserve the type otherwise.
    set_handler(item->type_handler());
  }
  else
  {
    // Items: choose VARCHAR/BLOB/MEDIUMBLOB/LONGBLOB, depending on length.
    set_handler(type_handler_varchar.
          type_handler_adjusted_to_max_octet_length(max_length,
                                                    collation.collation));
  }
  return false;
}


bool Item_sum_min_max::fix_length_and_dec(THD *thd)
{
  DBUG_ASSERT(args[0]->field_type() == args[0]->real_item()->field_type());
  DBUG_ASSERT(args[0]->result_type() == args[0]->real_item()->result_type());
  /* MIN/MAX can return NULL for empty set indepedent of the used column */
  set_maybe_null();
  null_value= true;
  return args[0]->type_handler()->Item_sum_hybrid_fix_length_and_dec(this);
}


/**
  MIN/MAX function setup.

  @param item       argument of MIN/MAX function
  @param value_arg  calculated value of MIN/MAX function

  @details
    Setup cache/comparator of MIN/MAX functions. When called by the
    copy_or_same function value_arg parameter contains calculated value
    of the original MIN/MAX object and it is saved in this object's cache.

    We mark the value and arg_cache with 'RAND_TABLE_BIT' to ensure
    that Arg_comparator::compare_datetime() doesn't allocate new
    item inside of Arg_comparator.  This would cause compare_datetime()
    and Item_sum_min::add() to use different values!
*/

void Item_sum_min_max::setup_hybrid(THD *thd, Item *item, Item *value_arg)
{
  DBUG_ENTER("Item_sum_min_max::setup_hybrid");
  if (!(value= item->get_cache(thd)))
    DBUG_VOID_RETURN;
  value->setup(thd, item);
  value->store(value_arg);
  /* Don't cache value, as it will change */
  if (!item->const_item())
    value->set_used_tables(RAND_TABLE_BIT);
  if (!(arg_cache= item->get_cache(thd)))
    DBUG_VOID_RETURN;
  arg_cache->setup(thd, item);
  /* Don't cache value, as it will change */
  if (!item->const_item())
    arg_cache->set_used_tables(RAND_TABLE_BIT);
  cmp= new (thd->mem_root) Arg_comparator();
  if (cmp)
    cmp->set_cmp_func(thd, this, (Item**)&arg_cache, (Item**)&value, FALSE);
  DBUG_VOID_RETURN;
}


Field *Item_sum_min_max::create_tmp_field(MEM_ROOT *root,
                                          bool group, TABLE *table)
{
  DBUG_ENTER("Item_sum_min_max::create_tmp_field");

  if (args[0]->type() == Item::FIELD_ITEM)
  {
    Field *field= ((Item_field*) args[0])->field;
    if ((field= field->create_tmp_field(root, table, true)))
    {
      DBUG_ASSERT((field->flags & NOT_NULL_FLAG) == 0);
      field->field_name= name;
    }
    DBUG_RETURN(field);
  }
  DBUG_RETURN(tmp_table_field_from_field_type(root, table));
}

/***********************************************************************
** Item_sum_sp class
***********************************************************************/

Item_sum_sp::Item_sum_sp(THD *thd, Name_resolution_context *context_arg,
                           sp_name *name_arg, sp_head *sp, List<Item> &list)
  :Item_sum(thd, list), Item_sp(thd, context_arg, name_arg)
{
  set_maybe_null();
  quick_group= 0;
  m_sp= sp;
}

Item_sum_sp::Item_sum_sp(THD *thd, Name_resolution_context *context_arg,
                           sp_name *name_arg, sp_head *sp)
  :Item_sum(thd), Item_sp(thd, context_arg, name_arg)
{
  set_maybe_null();
  quick_group= 0;
  m_sp= sp;
}

Item_sum_sp::Item_sum_sp(THD *thd, Item_sum_sp *item):
             Item_sum(thd, item), Item_sp(thd, item)
{
  base_flags|= (item->base_flags & item_base_t::MAYBE_NULL);
  quick_group= item->quick_group;
}

bool
Item_sum_sp::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  if (init_sum_func_check(thd))
    return TRUE;
  decimals= 0;

  m_sp= m_sp ? m_sp : sp_handler_function.sp_find_routine(thd, m_name, true);

  if (!m_sp)
  {
    my_missing_function_error(m_name->m_name, ErrConvDQName(m_name).ptr());
    process_error(thd);
    return TRUE;
  }

  if (init_result_field(thd, max_length, maybe_null(), &null_value, &name))
    return TRUE;

  for (uint i= 0 ; i < arg_count ; i++)
  {
    if (args[i]->fix_fields_if_needed_for_scalar(thd, &args[i]))
      return TRUE;
    set_if_bigger(decimals, args[i]->decimals);
  /* We should ignore FIELD's in arguments to sum functions */
    with_flags|= (args[i]->with_flags & ~item_with_t::FIELD);
  }
  result_field= NULL;
  max_length= float_length(decimals);
  null_value= 1;
  if (fix_length_and_dec(thd))
    return TRUE;

  if (check_sum_func(thd, ref))
    return TRUE;

  if (arg_count)
    memcpy(orig_args, args, sizeof(Item *) * arg_count);
  base_flags|= item_base_t::FIXED;
  return FALSE;
}

/**
  Execute function to store value in result field.
  This is called when we need the value to be returned for the function.
  Here we send a signal in form of the server status that all rows have been
  fetched and now we have to exit from the function with the return value.
  @return Function returns error status.
  @retval FALSE on success.
  @retval TRUE if an error occurred.
*/

bool
Item_sum_sp::execute()
{
  THD *thd= current_thd;
  bool res;
  uint old_server_status= thd->server_status;

  /*
    We set server status so we can send a signal to exit from the
    function with the return value.
  */

  thd->server_status|= SERVER_STATUS_LAST_ROW_SENT;
  res= Item_sp::execute(thd, &null_value, args, arg_count);
  thd->server_status= old_server_status;
  return res;
}

/**
  Handles the aggregation of the values.
  @note: See class description for more details on how and why this is done.
  @return The error state.
  @retval FALSE on success.
  @retval TRUE if an error occurred.
*/

bool
Item_sum_sp::add()
{
  return execute_impl(current_thd, args, arg_count);
}


void
Item_sum_sp::clear()
{
  delete func_ctx;
  func_ctx= NULL;
  sp_query_arena->free_items();
  free_root(&sp_mem_root, MYF(0));
}

const Type_handler *Item_sum_sp::type_handler() const
{
  DBUG_ENTER("Item_sum_sp::type_handler");
  DBUG_PRINT("info", ("m_sp = %p", (void *) m_sp));
  DBUG_ASSERT(sp_result_field);
  // This converts ENUM/SET to STRING
  const Type_handler *handler= sp_result_field->type_handler();
  DBUG_RETURN(handler->type_handler_for_item_field());
}

void
Item_sum_sp::cleanup()
{
  Item_sp::cleanup();
  Item_sum::cleanup();
}

/**
  Initialize local members with values from the Field interface.
  @note called from Item::fix_fields.
*/

bool
Item_sum_sp::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_sum_sp::fix_length_and_dec");
  DBUG_ASSERT(sp_result_field);
  Type_std_attributes::set(sp_result_field->type_std_attributes());
  bool res= Item_sum::fix_length_and_dec(thd);
  DBUG_RETURN(res);
}

LEX_CSTRING Item_sum_sp::func_name_cstring() const
{
  return Item_sp::func_name_cstring(current_thd, false);
}

Item* Item_sum_sp::copy_or_same(THD *thd)
{
  Item_sum_sp *copy_item= new (thd->mem_root) Item_sum_sp(thd, this);
  copy_item->init_result_field(thd, max_length, maybe_null(), 
                               &copy_item->null_value, &copy_item->name);
  return copy_item;
}

/***********************************************************************
** reset and add of sum_func
***********************************************************************/

/**
  @todo
  check if the following assignments are really needed
*/
Item_sum_sum::Item_sum_sum(THD *thd, Item_sum_sum *item) 
  :Item_sum_num(thd, item),
   Type_handler_hybrid_field_type(item),
   direct_added(FALSE), direct_reseted_field(FALSE),
   curr_dec_buff(item->curr_dec_buff),
   count(item->count)
{
  /* TODO: check if the following assignments are really needed */
  if (result_type() == DECIMAL_RESULT)
  {
    my_decimal2decimal(item->dec_buffs, dec_buffs);
    my_decimal2decimal(item->dec_buffs + 1, dec_buffs + 1);
  }
  else
    sum= item->sum;
}

Item *Item_sum_sum::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_sum(thd, this);
}


void Item_sum_sum::cleanup()
{
  DBUG_ENTER("Item_sum_sum::cleanup");
  direct_added= direct_reseted_field= FALSE;
  Item_sum_num::cleanup();
  DBUG_VOID_RETURN;
}


void Item_sum_sum::clear()
{
  DBUG_ENTER("Item_sum_sum::clear");
  null_value=1;
  count= 0;
  if (result_type() == DECIMAL_RESULT)
  {
    curr_dec_buff= 0;
    my_decimal_set_zero(dec_buffs);
  }
  else
    sum= 0.0;
  DBUG_VOID_RETURN;
}


void Item_sum_sum::fix_length_and_dec_double()
{
  set_handler(&type_handler_double); // Change FLOAT to DOUBLE
  decimals= args[0]->decimals;
  sum= 0.0;
}


void Item_sum_sum::fix_length_and_dec_decimal()
{
  set_handler(&type_handler_newdecimal); // Change temporal to new DECIMAL
  decimals= args[0]->decimals;
  /* SUM result can't be longer than length(arg) + length(MAX_ROWS) */
  int precision= args[0]->decimal_precision() + DECIMAL_LONGLONG_DIGITS;
  decimals= MY_MIN(decimals, DECIMAL_MAX_SCALE);
  precision= MY_MIN(precision, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                           decimals,
                                                           unsigned_flag);
  curr_dec_buff= 0;
  my_decimal_set_zero(dec_buffs);
}


bool Item_sum_sum::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_sum_sum::fix_length_and_dec");
  set_maybe_null();
  null_value=1;
  if (args[0]->cast_to_int_type_handler()->
      Item_sum_sum_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s (%d, %d)", type_handler()->name().ptr(),
                      max_length, (int) decimals));
  DBUG_RETURN(FALSE);
}


void Item_sum_sum::direct_add(my_decimal *add_sum_decimal)
{
  DBUG_ENTER("Item_sum_sum::direct_add");
  DBUG_PRINT("info", ("add_sum_decimal: %p", add_sum_decimal));
  direct_added= TRUE;
  direct_reseted_field= FALSE;
  if (add_sum_decimal)
  {
    direct_sum_is_null= FALSE;
    direct_sum_decimal= *add_sum_decimal;
  }
  else
  {
    direct_sum_is_null= TRUE;
    direct_sum_decimal= decimal_zero;
  }
  DBUG_VOID_RETURN;
}


void Item_sum_sum::direct_add(double add_sum_real, bool add_sum_is_null)
{
  DBUG_ENTER("Item_sum_sum::direct_add");
  DBUG_PRINT("info", ("add_sum_real: %f", add_sum_real));
  direct_added= TRUE;
  direct_reseted_field= FALSE;
  direct_sum_is_null= add_sum_is_null;
  direct_sum_real= add_sum_real;
  DBUG_VOID_RETURN;
}


bool Item_sum_sum::add()
{
  DBUG_ENTER("Item_sum_sum::add");
  add_helper(false);
  DBUG_RETURN(0);
}

void Item_sum_sum::add_helper(bool perform_removal)
{
  DBUG_ENTER("Item_sum_sum::add_helper");

  if (result_type() == DECIMAL_RESULT)
  {
    if (unlikely(direct_added))
    {
      /* Add value stored by Item_sum_sum::direct_add */
      DBUG_ASSERT(!perform_removal);

      direct_added= FALSE;
      if (likely(!direct_sum_is_null))
      {
        my_decimal_add(E_DEC_FATAL_ERROR, dec_buffs + (curr_dec_buff^1),
                       &direct_sum_decimal, dec_buffs + curr_dec_buff);
        curr_dec_buff^= 1;
        null_value= 0;
      }
    }
    else
    {
      direct_reseted_field= FALSE;
      my_decimal value;
      const my_decimal *val= aggr->arg_val_decimal(&value);
      if (!aggr->arg_is_null(true))
      {
        if (perform_removal)
        {
          if (count > 0)
          {
            my_decimal_sub(E_DEC_FATAL_ERROR, dec_buffs + (curr_dec_buff ^ 1),
              dec_buffs + curr_dec_buff, val);
            count--;
          }
          else
            DBUG_VOID_RETURN;
        }
        else
        {
          count++;
          my_decimal_add(E_DEC_FATAL_ERROR, dec_buffs + (curr_dec_buff ^ 1),
            val, dec_buffs + curr_dec_buff);
        }
        curr_dec_buff^= 1;
        null_value= (count > 0) ? 0 : 1;
      }
    }
  }
  else
  {
    if (unlikely(direct_added))
    {
      /* Add value stored by Item_sum_sum::direct_add */
      DBUG_ASSERT(!perform_removal);

      direct_added= FALSE;
      if (!direct_sum_is_null)
      {
        sum+= direct_sum_real;
        null_value= 0;
      }
    }
    else
    {
      direct_reseted_field= FALSE;
      if (perform_removal && count > 0)
        sum-= aggr->arg_val_real();
      else
        sum+= aggr->arg_val_real();
      if (!aggr->arg_is_null(true))
      {
        if (perform_removal)
        {
          if (count > 0)
          {
            count--;
          }
        }
        else
          count++;

        null_value= (count > 0) ? 0 : 1;
      }
    }
  }
  DBUG_VOID_RETURN;
}


longlong Item_sum_sum::val_int()
{
  DBUG_ASSERT(fixed());
  if (aggr)
    aggr->endup();
  if (result_type() == DECIMAL_RESULT)
    return dec_buffs[curr_dec_buff].to_longlong(unsigned_flag);
  return val_int_from_real();
}


double Item_sum_sum::val_real()
{
  DBUG_ASSERT(fixed());
  if (aggr)
    aggr->endup();
  if (result_type() == DECIMAL_RESULT)
    sum= dec_buffs[curr_dec_buff].to_double();
  return sum;
}


String *Item_sum_sum::val_str(String *str)
{
  if (aggr)
    aggr->endup();
  if (result_type() == DECIMAL_RESULT)
    return VDec(this).to_string_round(str, decimals);
  return val_string_from_real(str);
}


my_decimal *Item_sum_sum::val_decimal(my_decimal *val)
{
  if (aggr)
    aggr->endup();
  if (result_type() == DECIMAL_RESULT)
    return null_value ? NULL : (dec_buffs + curr_dec_buff);
  return val_decimal_from_real(val);
}

void Item_sum_sum::remove()
{
  DBUG_ENTER("Item_sum_sum::remove");
  add_helper(true);
  DBUG_VOID_RETURN;
}

/**
  Aggregate a distinct row from the distinct hash table.
 
  Called for each row into the hash table 'Aggregator_distinct::table'.
  Includes the current distinct row into the calculation of the 
  aggregate value. Uses the Field classes to get the value from the row.
  This function is used for AVG/SUM(DISTINCT). For COUNT(DISTINCT) 
  it's called only when there are no blob arguments and the data don't
  fit into memory (so Unique makes persisted trees on disk). 

  @param element     pointer to the row data.
  
  @return status
    @retval FALSE     success
    @retval TRUE      failure
*/
  
bool Aggregator_distinct::unique_walk_function(void *element)
{
  memcpy(table->field[0]->ptr, element, tree_key_length);
  item_sum->add();
  return 0;
}


/*
  A variant of unique_walk_function() that is to be used with Item_sum_count.

  COUNT is a special aggregate function: it doesn't need the values, it only
  needs to count them. COUNT needs to know the values are not NULLs, but NULL
  values are not put into the Unique, so we don't need to check for NULLs here.
*/

bool Aggregator_distinct::unique_walk_function_for_count(void *element)
{
  Item_sum_count *sum= (Item_sum_count *)item_sum;
  sum->count++;
  return 0;
}


Aggregator_distinct::~Aggregator_distinct()
{
  if (tree)
  {
    delete tree;
    tree= NULL;
  }
  if (table)
  {
    free_tmp_table(table->in_use, table);
    table=NULL;
  }
  if (tmp_table_param)
  {
    delete tmp_table_param;
    tmp_table_param= NULL;
  }
}


my_decimal *Aggregator_simple::arg_val_decimal(my_decimal *value)
{
  return item_sum->args[0]->val_decimal(value);
}


double Aggregator_simple::arg_val_real()
{
  return item_sum->args[0]->val_real();
}


bool Aggregator_simple::arg_is_null(bool use_null_value)
{
  Item **item= item_sum->args;
  const uint item_count= item_sum->arg_count;
  if (use_null_value)
  {
    for (uint i= 0; i < item_count; i++)
    {
      if (item[i]->null_value)
        return true;
    }
  }
  else
  {
    for (uint i= 0; i < item_count; i++)
    {
      if (item[i]->maybe_null() && item[i]->is_null())
        return true;
    }
  }
  return false;
}


my_decimal *Aggregator_distinct::arg_val_decimal(my_decimal * value)
{
  return use_distinct_values ? table->field[0]->val_decimal(value) :
    item_sum->args[0]->val_decimal(value);
}


double Aggregator_distinct::arg_val_real()
{
  return use_distinct_values ? table->field[0]->val_real() :
    item_sum->args[0]->val_real();
}


bool Aggregator_distinct::arg_is_null(bool use_null_value)
{
  if (use_distinct_values)
  {
    const bool rc= table->field[0]->is_null();
    DBUG_ASSERT(!rc); // NULLs are never stored in 'tree'
    return rc;
  }
  return use_null_value ?
    item_sum->args[0]->null_value :
    (item_sum->args[0]->maybe_null() && item_sum->args[0]->is_null());
}


Item *Item_sum_count::copy_or_same(THD* thd)
{
  DBUG_ENTER("Item_sum_count::copy_or_same");
  DBUG_RETURN(new (thd->mem_root) Item_sum_count(thd, this));
}


void Item_sum_count::direct_add(longlong add_count)
{
  DBUG_ENTER("Item_sum_count::direct_add");
  DBUG_PRINT("info", ("add_count: %lld", add_count));
  direct_counted= TRUE;
  direct_reseted_field= FALSE;
  direct_count= add_count;
  DBUG_VOID_RETURN;
}


void Item_sum_count::clear()
{
  DBUG_ENTER("Item_sum_count::clear");
  count= 0;
  DBUG_VOID_RETURN;
}


bool Item_sum_count::add()
{
  DBUG_ENTER("Item_sum_count::add");
  if (direct_counted)
  {
    direct_counted= FALSE;
    count+= direct_count;
  }
  else
  {
    direct_reseted_field= FALSE;
    if (aggr->arg_is_null(false))
      DBUG_RETURN(0);
    count++;
  }
  DBUG_RETURN(0);
}


/*
  Remove a row. This is used by window functions.
*/

void Item_sum_count::remove()
{
  DBUG_ASSERT(aggr->Aggrtype() == Aggregator::SIMPLE_AGGREGATOR);
  if (aggr->arg_is_null(false))
    return;
  if (count > 0)
    count--;
}

longlong Item_sum_count::val_int()
{
  DBUG_ENTER("Item_sum_count::val_int");
  DBUG_ASSERT(fixed());
  if (aggr)
    aggr->endup();
  DBUG_RETURN((longlong)count);
}


void Item_sum_count::cleanup()
{
  DBUG_ENTER("Item_sum_count::cleanup");
  count= 0;
  direct_counted= FALSE;
  direct_reseted_field= FALSE;
  Item_sum_int::cleanup();
  DBUG_VOID_RETURN;
}


/*
  Average
*/

void Item_sum_avg::fix_length_and_dec_decimal()
{
  Item_sum_sum::fix_length_and_dec_decimal();
  int precision= args[0]->decimal_precision() + prec_increment;
  decimals= MY_MIN(args[0]->decimal_scale() + prec_increment, DECIMAL_MAX_SCALE);
  max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                           decimals,
                                                           unsigned_flag);
  f_precision= MY_MIN(precision+DECIMAL_LONGLONG_DIGITS, DECIMAL_MAX_PRECISION);
  f_scale= args[0]->decimal_scale();
  dec_bin_size= my_decimal_get_binary_size(f_precision, f_scale);
}


void Item_sum_avg::fix_length_and_dec_double()
{
  Item_sum_sum::fix_length_and_dec_double();
  decimals= MY_MIN(args[0]->decimals + prec_increment,
                   FLOATING_POINT_DECIMALS);
  max_length= MY_MIN(args[0]->max_length + prec_increment, float_length(decimals));
}


bool Item_sum_avg::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_sum_avg::fix_length_and_dec");
  prec_increment= current_thd->variables.div_precincrement;
  set_maybe_null();
  null_value=1;
  if (args[0]->cast_to_int_type_handler()->
      Item_sum_avg_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s (%d, %d)", type_handler()->name().ptr(),
                      max_length, (int) decimals));
  DBUG_RETURN(FALSE);
}


Item *Item_sum_avg::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_avg(thd, this);
}


Field *Item_sum_avg::create_tmp_field(MEM_ROOT *root, bool group, TABLE *table)
{

  if (group)
  {
    /*
      We must store both value and counter in the temporary table in one field.
      The easiest way is to do this is to store both value in a string
      and unpack on access.
    */
    Field *field= new (root)
      Field_string(((result_type() == DECIMAL_RESULT) ?
                   dec_bin_size : sizeof(double)) + sizeof(longlong),
                   0, &name, &my_charset_bin);
    if (field)
      field->init(table);
    return field;
  }
  return tmp_table_field_from_field_type(root, table);
}


void Item_sum_avg::clear()
{
  Item_sum_sum::clear();
  count=0;
}


bool Item_sum_avg::add()
{
  if (Item_sum_sum::add())
    return TRUE;
  if (!aggr->arg_is_null(true))
    count++;
  return FALSE;
}

void Item_sum_avg::remove()
{
  Item_sum_sum::remove();
  if (!aggr->arg_is_null(true))
  {
    if (count > 0)
      count--;
  }
}

double Item_sum_avg::val_real()
{
  DBUG_ASSERT(fixed());
  if (aggr)
    aggr->endup();
  if (!count)
  {
    null_value=1;
    return 0.0;
  }
  return Item_sum_sum::val_real() / ulonglong2double(count);
}


my_decimal *Item_sum_avg::val_decimal(my_decimal *val)
{
  my_decimal cnt;
  const my_decimal *sum_dec;
  DBUG_ASSERT(fixed());
  if (aggr)
    aggr->endup();
  if (!count)
  {
    null_value=1;
    return NULL;
  }

  /*
    For non-DECIMAL result_type() the division will be done in
    Item_sum_avg::val_real().
  */
  if (result_type() != DECIMAL_RESULT)
    return val_decimal_from_real(val);

  sum_dec= dec_buffs + curr_dec_buff;
  int2my_decimal(E_DEC_FATAL_ERROR, count, 0, &cnt);
  my_decimal_div(E_DEC_FATAL_ERROR, val, sum_dec, &cnt, prec_increment);
  return val;
}


String *Item_sum_avg::val_str(String *str)
{
  if (aggr)
    aggr->endup();
  if (result_type() == DECIMAL_RESULT)
    return VDec(this).to_string_round(str, decimals);
  return val_string_from_real(str);
}


/*
  Standard deviation
*/

double Item_sum_std::val_real()
{
  DBUG_ASSERT(fixed());
  double nr= Item_sum_variance::val_real();
  if (std::isnan(nr))
  {
    /*
      variance_fp_recurrence_next() can overflow in some cases and return "nan":

      CREATE OR REPLACE TABLE t1 (a DOUBLE);
      INSERT INTO t1 VALUES (1.7e+308), (-1.7e+308), (0);
      SELECT STDDEV_SAMP(a) FROM t1;
    */
    null_value= true; // Convert "nan" to NULL
    return 0;
  }
  if (std::isinf(nr))
    return DBL_MAX;
  DBUG_ASSERT(nr >= 0.0);
  return sqrt(nr);
}

Item *Item_sum_std::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_std(thd, this);
}


Item *Item_sum_std::result_item(THD *thd, Field *field)
{
  return new (thd->mem_root) Item_std_field(thd, this);
}


/*
  Variance
*/


/**
  Variance implementation for floating-point implementations, without
  catastrophic cancellation, from Knuth's _TAoCP_, 3rd ed, volume 2, pg232.
  This alters the value at m, s, and increments count.
*/

/*
  These two functions are used by the Item_sum_variance and the
  Item_variance_field classes, which are unrelated, and each need to calculate
  variance.  The difference between the two classes is that the first is used
  for a mundane SELECT, while the latter is used in a GROUPing SELECT.
*/
void Stddev::recurrence_next(double nr)
{
  if (!m_count++)
  {
    DBUG_ASSERT(m_m == 0);
    DBUG_ASSERT(m_s == 0);
    m_m= nr;
  }
  else
  {
    double m_kminusone= m_m;
    volatile double diff= nr - m_kminusone;
    m_m= m_kminusone + diff / (double) m_count;
    m_s= m_s + diff * (nr - m_m);
  }
}


double Stddev::result(bool is_sample_variance)
{
  if (m_count == 1)
    return 0.0;

  if (is_sample_variance)
    return m_s / (m_count - 1);

  /* else, is a population variance */
  return m_s / m_count;
}


Item_sum_variance::Item_sum_variance(THD *thd, Item_sum_variance *item):
  Item_sum_double(thd, item),
    m_stddev(item->m_stddev), sample(item->sample),
    prec_increment(item->prec_increment)
{ }


void Item_sum_variance::fix_length_and_dec_double()
{
  DBUG_ASSERT(Item_sum_variance::type_handler() == &type_handler_double);
  decimals= MY_MIN(args[0]->decimals + 4, FLOATING_POINT_DECIMALS);
}


void Item_sum_variance::fix_length_and_dec_decimal()
{
  DBUG_ASSERT(Item_sum_variance::type_handler() == &type_handler_double);
  int precision= args[0]->decimal_precision() * 2 + prec_increment;
  decimals= MY_MIN(args[0]->decimals + prec_increment,
                   FLOATING_POINT_DECIMALS - 1);
  max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                           decimals,
                                                           unsigned_flag);
}


bool Item_sum_variance::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_sum_variance::fix_length_and_dec");
  set_maybe_null();
  null_value= 1;
  prec_increment= current_thd->variables.div_precincrement;

  /*
    According to the SQL2003 standard (Part 2, Foundations; sec 10.9,
    aggregate function; paragraph 7h of Syntax Rules), "the declared 
    type of the result is an implementation-defined approximate numeric
    type.
  */
  if (args[0]->type_handler()->Item_sum_variance_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s (%d, %d)", type_handler()->name().ptr(),
                      max_length, (int)decimals));
  DBUG_RETURN(FALSE);
}


Item *Item_sum_variance::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_variance(thd, this);
}


/**
  Create a new field to match the type of value we're expected to yield.
  If we're grouping, then we need some space to serialize variables into, to
  pass around.
*/
Field *Item_sum_variance::create_tmp_field(MEM_ROOT *root,
                                           bool group, TABLE *table)
{
  Field *field;
  if (group)
  {
    /*
      We must store both value and counter in the temporary table in one field.
      The easiest way is to do this is to store both value in a string
      and unpack on access.
    */
    field= new (root) Field_string(Stddev::binary_size(), 0,
                                   &name, &my_charset_bin);
  }
  else
    field= new (root) Field_double(max_length, maybe_null(), &name, decimals,
                                   TRUE);

  if (field != NULL)
    field->init(table);

  return field;
}


void Item_sum_variance::clear()
{
  m_stddev= Stddev();
}

bool Item_sum_variance::add()
{
  /* 
    Why use a temporary variable?  We don't know if it is null until we
    evaluate it, which has the side-effect of setting null_value .
  */
  double nr= args[0]->val_real();
  
  if (!args[0]->null_value)
    m_stddev.recurrence_next(nr);
  return 0;
}

double Item_sum_variance::val_real()
{
  DBUG_ASSERT(fixed());

  /*
    'sample' is a 1/0 boolean value.  If it is 1/true, id est this is a sample
    variance call, then we should set nullness when the count of the items
    is one or zero.  If it's zero, i.e. a population variance, then we only
    set nullness when the count is zero.

    Another way to read it is that 'sample' is the numerical threshold, at and
    below which a 'count' number of items is called NULL.
  */
  DBUG_ASSERT((sample == 0) || (sample == 1));
  if (m_stddev.count() <= sample)
  {
    null_value=1;
    return 0.0;
  }

  null_value=0;
  return m_stddev.result(sample);
}


void Item_sum_variance::reset_field()
{
  double nr;
  uchar *res= result_field->ptr;

  nr= args[0]->val_real();              /* sets null_value as side-effect */

  if (args[0]->null_value)
    bzero(res,Stddev::binary_size());
  else
    Stddev(nr).to_binary(res);
}


Stddev::Stddev(const uchar *ptr)
{
  float8get(m_m, ptr);
  float8get(m_s, ptr + sizeof(double));
  m_count= sint8korr(ptr + sizeof(double) * 2);
}


void Stddev::to_binary(uchar *ptr) const
{
  /* Serialize format is (double)m, (double)s, (longlong)count */
  float8store(ptr, m_m);
  float8store(ptr + sizeof(double), m_s);
  ptr+= sizeof(double)*2;
  int8store(ptr, m_count);
}


void Item_sum_variance::update_field()
{
  uchar *res=result_field->ptr;

  double nr= args[0]->val_real();       /* sets null_value as side-effect */

  if (args[0]->null_value)
    return;

  /* Serialize format is (double)m, (double)s, (longlong)count */
  Stddev field_stddev(res);
  field_stddev.recurrence_next(nr);
  field_stddev.to_binary(res);
}


Item *Item_sum_variance::result_item(THD *thd, Field *field)
{
  return new (thd->mem_root) Item_variance_field(thd, this);
}

/* min & max */

void Item_sum_min_max::clear()
{
  DBUG_ENTER("Item_sum_min_max::clear");
  value->clear();
  null_value= 1;
  DBUG_VOID_RETURN;
}


bool
Item_sum_min_max::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return true;
  bool retval= value->get_date(thd, ltime, fuzzydate);
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == true);
  return retval;
}


void Item_sum_min_max::direct_add(Item *item)
{
  DBUG_ENTER("Item_sum_min_max::direct_add");
  DBUG_PRINT("info", ("item: %p", item));
  direct_added= TRUE;
  direct_item= item;
  DBUG_VOID_RETURN;
}


double Item_sum_min_max::val_real()
{
  DBUG_ENTER("Item_sum_min_max::val_real");
  DBUG_ASSERT(fixed());
  if (null_value)
    DBUG_RETURN(0.0);
  double retval= value->val_real();
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == 0.0);
  DBUG_RETURN(retval);
}

longlong Item_sum_min_max::val_int()
{
  DBUG_ENTER("Item_sum_min_max::val_int");
  DBUG_ASSERT(fixed());
  if (null_value)
    DBUG_RETURN(0);
  longlong retval= value->val_int();
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == 0);
  DBUG_RETURN(retval);
}


my_decimal *Item_sum_min_max::val_decimal(my_decimal *val)
{
  DBUG_ENTER("Item_sum_min_max::val_decimal");
  DBUG_ASSERT(fixed());
  if (null_value)
    DBUG_RETURN(0);
  my_decimal *retval= value->val_decimal(val);
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == NULL);
  DBUG_RETURN(retval);
}


String *
Item_sum_min_max::val_str(String *str)
{
  DBUG_ENTER("Item_sum_min_max::val_str");
  DBUG_ASSERT(fixed());
  if (null_value)
    DBUG_RETURN(0);
  String *retval= value->val_str(str);
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == NULL);
  DBUG_RETURN(retval);
}


bool Item_sum_min_max::val_native(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return true;
  return val_native_from_item(thd, value, to);
}


void Item_sum_min_max::cleanup()
{
  DBUG_ENTER("Item_sum_min_max::cleanup");
  Item_sum::cleanup();
  if (cmp)
    delete cmp;
  cmp= 0;
  /*
    by default it is TRUE to avoid TRUE reporting by
    Item_func_not_all/Item_func_nop_all if this item was never called.

    no_rows_in_result() set it to FALSE if was not results found.
    If some results found it will be left unchanged.
  */
  was_values= TRUE;
  DBUG_VOID_RETURN;
}

void Item_sum_min_max::no_rows_in_result()
{
  DBUG_ENTER("Item_sum_min_max::no_rows_in_result");
  /* We may be called here twice in case of ref field in function */
  if (was_values)
  {
    was_values= FALSE;
    was_null_value= value->null_value;
    clear();
  }
  DBUG_VOID_RETURN;
}

void Item_sum_min_max::restore_to_before_no_rows_in_result()
{
  if (!was_values)
  {
    was_values= TRUE;
    null_value= value->null_value= was_null_value;
  }
}


Item *Item_sum_min::copy_or_same(THD* thd)
{
  DBUG_ENTER("Item_sum_min::copy_or_same");
  Item_sum_min *item= new (thd->mem_root) Item_sum_min(thd, this);
  item->setup_hybrid(thd, args[0], value);
  DBUG_RETURN(item);
}


bool Item_sum_min::add()
{
  Item *UNINIT_VAR(tmp_item);
  DBUG_ENTER("Item_sum_min::add");
  DBUG_PRINT("enter", ("this: %p", this));

  if (unlikely(direct_added))
  {
    /* Change to use direct_item */
    tmp_item= arg_cache->get_item();
    arg_cache->store(direct_item);
  }
  DBUG_PRINT("info", ("null_value: %s", null_value ? "TRUE" : "FALSE"));
  /* args[0] < value */
  arg_cache->cache_value();
  if (!arg_cache->null_value &&
      (null_value || cmp->compare() < 0))
  {
    value->store(arg_cache);
    value->cache_value();
    null_value= 0;
  }
  if (unlikely(direct_added))
  {
    /* Restore original item */
    direct_added= FALSE;
    arg_cache->store(tmp_item);
  }
  DBUG_RETURN(0);
}


Item *Item_sum_max::copy_or_same(THD* thd)
{
  Item_sum_max *item= new (thd->mem_root) Item_sum_max(thd, this);
  item->setup_hybrid(thd, args[0], value);
  return item;
}


bool Item_sum_max::add()
{
  Item * UNINIT_VAR(tmp_item);
  DBUG_ENTER("Item_sum_max::add");
  DBUG_PRINT("enter", ("this: %p", this));

  if (unlikely(direct_added))
  {
    /* Change to use direct_item */
    tmp_item= arg_cache->get_item();
    arg_cache->store(direct_item);
  }
  /* args[0] > value */
  arg_cache->cache_value();
  DBUG_PRINT("info", ("null_value: %s", null_value ? "TRUE" : "FALSE"));
  if (!arg_cache->null_value &&
      (null_value || cmp->compare() > 0))
  {
    value->store(arg_cache);
    value->cache_value();
    null_value= 0;
  }
  if (unlikely(direct_added))
  {
    /* Restore original item */
    direct_added= FALSE;
    arg_cache->store(tmp_item);
  }
  DBUG_RETURN(0);
}


/* bit_or and bit_and */

longlong Item_sum_bit::val_int()
{
  DBUG_ASSERT(fixed());
  return (longlong) bits;
}


void Item_sum_bit::clear()
{
  bits= reset_bits;
  if (as_window_function)
    clear_as_window();
}

Item *Item_sum_or::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_or(thd, this);
}

bool Item_sum_bit::clear_as_window()
{
  memset(bit_counters, 0, sizeof(bit_counters));
  num_values_added= 0;
  set_bits_from_counters();
  return 0;
}

bool Item_sum_bit::remove_as_window(ulonglong value)
{
  DBUG_ASSERT(as_window_function);
  if (num_values_added == 0)
    return 0; // Nothing to remove.

  for (int i= 0; i < NUM_BIT_COUNTERS; i++)
  {
    if (!bit_counters[i])
    {
      // Don't attempt to remove values that were never added.
      DBUG_ASSERT((value & (1ULL << i)) == 0);
      continue;
    }
    bit_counters[i]-= (value & (1ULL << i)) ? 1 : 0;
  }

  // Prevent overflow;
  num_values_added = MY_MIN(num_values_added, num_values_added - 1);
  set_bits_from_counters();
  return 0;
}

bool Item_sum_bit::add_as_window(ulonglong value)
{
  DBUG_ASSERT(as_window_function);
  for (int i= 0; i < NUM_BIT_COUNTERS; i++)
  {
    bit_counters[i]+= (value & (1ULL << i)) ? 1 : 0;
  }
  // Prevent overflow;
  num_values_added = MY_MAX(num_values_added, num_values_added + 1);
  set_bits_from_counters();
  return 0;
}

void Item_sum_or::set_bits_from_counters()
{
  ulonglong value= 0;
  for (uint i= 0; i < NUM_BIT_COUNTERS; i++)
  {
    value|= bit_counters[i] > 0 ? (1ULL << i) : 0ULL;
  }
  bits= value | reset_bits;
}

bool Item_sum_or::add()
{
  ulonglong value= (ulonglong) args[0]->val_int();
  if (!args[0]->null_value)
  {
    if (as_window_function)
      return add_as_window(value);
    bits|=value;
  }
  return 0;
}

void Item_sum_xor::set_bits_from_counters()
{
  ulonglong value= 0;
  for (int i= 0; i < NUM_BIT_COUNTERS; i++)
  {
    value|= (bit_counters[i] % 2) ? (1 << i) : 0;
  }
  bits= value ^ reset_bits;
}

Item *Item_sum_xor::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_xor(thd, this);
}


bool Item_sum_xor::add()
{
  ulonglong value= (ulonglong) args[0]->val_int();
  if (!args[0]->null_value)
  {
    if (as_window_function)
      return add_as_window(value);
    bits^=value;
  }
  return 0;
}

void Item_sum_and::set_bits_from_counters()
{
  ulonglong value= 0;
  if (!num_values_added)
  {
    bits= reset_bits;
    return;
  }

  for (int i= 0; i < NUM_BIT_COUNTERS; i++)
  {
    // We've only added values of 1 for this bit.
    if (bit_counters[i] == num_values_added)
      value|= (1ULL << i);
  }
  bits= value & reset_bits;
}
Item *Item_sum_and::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_and(thd, this);
}


bool Item_sum_and::add()
{
  ulonglong value= (ulonglong) args[0]->val_int();
  if (!args[0]->null_value)
  {
    if (as_window_function)
      return add_as_window(value);
    bits&=value;
  }
  return 0;
}

/************************************************************************
** reset result of a Item_sum with is saved in a tmp_table
*************************************************************************/

void Item_sum_min_max::reset_field()
{
  Item *UNINIT_VAR(tmp_item), *arg0;
  DBUG_ENTER("Item_sum_min_max::reset_field");

  arg0= args[0];
  if (unlikely(direct_added))
  {
    /* Switch to use direct item */
    tmp_item= value->get_item();
    value->store(direct_item);
    arg0= direct_item;
  }

  switch(result_type()) {
  case STRING_RESULT:
  {
    char buff[MAX_FIELD_WIDTH];
    String tmp(buff,sizeof(buff),result_field->charset()),*res;

    res= arg0->val_str(&tmp);
    if (arg0->null_value)
    {
      result_field->set_null();
      result_field->reset();
    }
    else
    {
      result_field->set_notnull();
      result_field->store(res->ptr(),res->length(),tmp.charset());
    }
    break;
  }
  case INT_RESULT:
  {
    longlong nr= arg0->val_int();

    if (maybe_null())
    {
      if (arg0->null_value)
      {
	nr=0;
	result_field->set_null();
      }
      else
	result_field->set_notnull();
    }
    DBUG_PRINT("info", ("nr: %lld", nr));
    result_field->store(nr, unsigned_flag);
    break;
  }
  case REAL_RESULT:
  {
    double nr= arg0->val_real();

    if (maybe_null())
    {
      if (arg0->null_value)
      {
	nr=0.0;
	result_field->set_null();
      }
      else
	result_field->set_notnull();
    }
    result_field->store(nr);
    break;
  }
  case DECIMAL_RESULT:
  {
    VDec arg_dec(arg0);

    if (maybe_null())
    {
      if (arg_dec.is_null())
        result_field->set_null();
      else
        result_field->set_notnull();
    }
    /*
      We must store zero in the field as we will use the field value in
      add()
    */
    result_field->store_decimal(arg_dec.ptr_or(&decimal_zero));
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);
  }

  if (unlikely(direct_added))
  {
    direct_added= FALSE;
    value->store(tmp_item);
  }
  DBUG_VOID_RETURN;
}


void Item_sum_sum::reset_field()
{
  my_bool null_flag;
  DBUG_ASSERT (aggr->Aggrtype() != Aggregator::DISTINCT_AGGREGATOR);
  if (result_type() == DECIMAL_RESULT)
  {
    if (unlikely(direct_added))
      result_field->store_decimal(&direct_sum_decimal);
    else
      result_field->store_decimal(VDec(args[0]).ptr_or(&decimal_zero));
  }
  else
  {
    DBUG_ASSERT(result_type() == REAL_RESULT);
    double nr= likely(!direct_added) ? args[0]->val_real() : direct_sum_real;
    float8store(result_field->ptr, nr);
  }

  if (unlikely(direct_added))
  {
    direct_added= FALSE;
    direct_reseted_field= TRUE;
    null_flag= direct_sum_is_null;
  }
  else
    null_flag= args[0]->null_value;

  if (null_flag)
    result_field->set_null();
  else
    result_field->set_notnull();
}


void Item_sum_count::reset_field()
{
  DBUG_ENTER("Item_sum_count::reset_field");
  uchar *res=result_field->ptr;
  longlong nr=0;
  DBUG_ASSERT (aggr->Aggrtype() != Aggregator::DISTINCT_AGGREGATOR);

  if (unlikely(direct_counted))
  {
    nr= direct_count;
    direct_counted= FALSE;
    direct_reseted_field= TRUE;
  }
  else if (!args[0]->maybe_null() || !args[0]->is_null())
    nr= 1;
  DBUG_PRINT("info", ("nr: %lld", nr));
  int8store(res,nr);
  DBUG_VOID_RETURN;
}


void Item_sum_avg::reset_field()
{
  uchar *res=result_field->ptr;
  DBUG_ASSERT (aggr->Aggrtype() != Aggregator::DISTINCT_AGGREGATOR);
  if (result_type() == DECIMAL_RESULT)
  {
    longlong tmp;
    VDec value(args[0]);
    tmp= value.is_null() ? 0 : 1;
    value.to_binary(res, f_precision, f_scale);
    res+= dec_bin_size;
    int8store(res, tmp);
  }
  else
  {
    double nr= args[0]->val_real();

    if (args[0]->null_value)
      bzero(res,sizeof(double)+sizeof(longlong));
    else
    {
      longlong tmp= 1;
      float8store(res,nr);
      res+=sizeof(double);
      int8store(res,tmp);
    }
  }
}


void Item_sum_bit::reset_field()
{
  reset_and_add();
  int8store(result_field->ptr, bits);
}

void Item_sum_bit::update_field()
{
  // We never call update_field when computing the function as a window
  // function. Setting bits to a random value invalidates the bits counters and
  // the result of the bit function becomes erroneous.
  DBUG_ASSERT(!as_window_function);
  uchar *res=result_field->ptr;
  bits= uint8korr(res);
  add();
  int8store(res, bits);
}


/**
  calc next value and merge it with field_value.
*/

void Item_sum_sum::update_field()
{
  DBUG_ASSERT (aggr->Aggrtype() != Aggregator::DISTINCT_AGGREGATOR);
  if (result_type() == DECIMAL_RESULT)
  {
    my_decimal value, *arg_val;
    my_bool null_flag;
    if (unlikely(direct_added || direct_reseted_field))
    {
      direct_added= direct_reseted_field= FALSE;
      arg_val= &direct_sum_decimal;
      null_flag= direct_sum_is_null;
    }
    else
    {
      arg_val= args[0]->val_decimal(&value);
      null_flag= args[0]->null_value;
    }

    if (!null_flag)
    {
      if (!result_field->is_null())
      {
        my_decimal field_value(result_field);
        my_decimal_add(E_DEC_FATAL_ERROR, dec_buffs, arg_val, &field_value);
        result_field->store_decimal(dec_buffs);
      }
      else
      {
        result_field->store_decimal(arg_val);
        result_field->set_notnull();
      }
    }
  }
  else
  {
    double old_nr,nr;
    uchar *res= result_field->ptr;
    my_bool null_flag;

    float8get(old_nr,res);
    if (unlikely(direct_added || direct_reseted_field))
    {
      direct_added= direct_reseted_field= FALSE;
      null_flag= direct_sum_is_null;
      nr= direct_sum_real;
    }
    else
    {
      nr= args[0]->val_real();
      null_flag= args[0]->null_value;
    }
    if (!null_flag)
    {
      old_nr+=nr;
      result_field->set_notnull();
    }
    float8store(res,old_nr);
  }
}


void Item_sum_count::update_field()
{
  DBUG_ENTER("Item_sum_count::update_field");
  longlong nr;
  uchar *res=result_field->ptr;

  nr=sint8korr(res);
  if (unlikely(direct_counted || direct_reseted_field))
  {
    direct_counted= direct_reseted_field= FALSE;
    nr+= direct_count;
  }
  else if (!args[0]->maybe_null() || !args[0]->is_null())
    nr++;
  DBUG_PRINT("info", ("nr: %lld", nr));
  int8store(res,nr);
  DBUG_VOID_RETURN;
}


void Item_sum_avg::update_field()
{
  longlong field_count;
  uchar *res=result_field->ptr;

  DBUG_ASSERT (aggr->Aggrtype() != Aggregator::DISTINCT_AGGREGATOR);

  if (result_type() == DECIMAL_RESULT)
  {
    VDec tmp(args[0]);
    if (!tmp.is_null())
    {
      binary2my_decimal(E_DEC_FATAL_ERROR, res,
                        dec_buffs + 1, f_precision, f_scale);
      field_count= sint8korr(res + dec_bin_size);
      my_decimal_add(E_DEC_FATAL_ERROR, dec_buffs, tmp.ptr(), dec_buffs + 1);
      dec_buffs->to_binary(res, f_precision, f_scale);
      res+= dec_bin_size;
      field_count++;
      int8store(res, field_count);
    }
  }
  else
  {
    double nr;

    nr= args[0]->val_real();
    if (!args[0]->null_value)
    {
      double old_nr;
      float8get(old_nr, res);
      field_count= sint8korr(res + sizeof(double));
      old_nr+= nr;
      float8store(res,old_nr);
      res+= sizeof(double);
      field_count++;
      int8store(res, field_count);
    }
  }
}


Item *Item_sum_avg::result_item(THD *thd, Field *field)
{
  return
    result_type() == DECIMAL_RESULT ?
    (Item_avg_field*) new (thd->mem_root) Item_avg_field_decimal(thd, this) :
    (Item_avg_field*) new (thd->mem_root) Item_avg_field_double(thd, this);
}


void Item_sum_min_max::update_field()
{
  DBUG_ENTER("Item_sum_min_max::update_field");
  Item *UNINIT_VAR(tmp_item);
  if (unlikely(direct_added))
  {
    tmp_item= args[0];
    args[0]= direct_item;
  }
  if (Item_sum_min_max::type_handler()->is_val_native_ready())
  {
    /*
      TODO-10.5: change Item_sum_min_max to use val_native() for all data types
      - make all type handlers val_native() ready
      - use min_max_update_native_field() for all data types
      - remove Item_sum_min_max::min_max_update_{str|real|int|decimal}_field()
    */
    min_max_update_native_field();
  }
  else
  {
    switch (Item_sum_min_max::type_handler()->cmp_type()) {
    case STRING_RESULT:
    case TIME_RESULT:
      min_max_update_str_field();
      break;
    case INT_RESULT:
      min_max_update_int_field();
      break;
    case DECIMAL_RESULT:
      min_max_update_decimal_field();
      break;
    default:
      min_max_update_real_field();
    }
  }
  if (unlikely(direct_added))
  {
    direct_added= FALSE;
    args[0]= tmp_item;
  }
  DBUG_VOID_RETURN;
}


void Arg_comparator::min_max_update_field_native(THD *thd,
                                                 Field *field,
                                                 Item *item,
                                                 int cmp_sign)
{
  DBUG_ENTER("Arg_comparator::min_max_update_field_native");
  if (!item->val_native(current_thd, &m_native2))
  {
    if (field->is_null())
      field->store_native(m_native2); // The first non-null value
    else
    {
      field->val_native(&m_native1);
      if ((cmp_sign * m_compare_handler->cmp_native(m_native2, m_native1)) < 0)
        field->store_native(m_native2);
    }
    field->set_notnull();
  }
  DBUG_VOID_RETURN;
}


void
Item_sum_min_max::min_max_update_native_field()
{
  DBUG_ENTER("Item_sum_min_max::min_max_update_native_field");
  DBUG_ASSERT(cmp);
  DBUG_ASSERT(type_handler_for_comparison() == cmp->compare_type_handler());
  THD *thd= current_thd;
  cmp->min_max_update_field_native(thd, result_field, args[0], cmp_sign);
  DBUG_VOID_RETURN;
}


void
Item_sum_min_max::min_max_update_str_field()
{
  DBUG_ENTER("Item_sum_min_max::min_max_update_str_field");
  DBUG_ASSERT(cmp);
  String *res_str=args[0]->val_str(&cmp->value1);

  if (!args[0]->null_value)
  {
    if (result_field->is_null())
      result_field->store(res_str->ptr(),res_str->length(),res_str->charset());
    else
    {
      result_field->val_str(&cmp->value2);
      if ((cmp_sign * sortcmp(res_str,&cmp->value2,collation.collation)) < 0)
        result_field->store(res_str->ptr(),res_str->length(),res_str->charset());
    }
    result_field->set_notnull();
  }
  DBUG_VOID_RETURN;
}


void
Item_sum_min_max::min_max_update_real_field()
{
  double nr,old_nr;

  DBUG_ENTER("Item_sum_min_max::min_max_update_real_field");
  old_nr=result_field->val_real();
  nr= args[0]->val_real();
  if (!args[0]->null_value)
  {
    if (result_field->is_null(0) ||
	(cmp_sign > 0 ? old_nr > nr : old_nr < nr))
      old_nr=nr;
    result_field->set_notnull();
  }
  else if (result_field->is_null(0))
    result_field->set_null();
  result_field->store(old_nr);
  DBUG_VOID_RETURN;
}


void
Item_sum_min_max::min_max_update_int_field()
{
  longlong nr,old_nr;

  DBUG_ENTER("Item_sum_min_max::min_max_update_int_field");
  old_nr=result_field->val_int();
  nr=args[0]->val_int();
  if (!args[0]->null_value)
  {
    if (result_field->is_null(0))
      old_nr=nr;
    else
    {
      bool res=(unsigned_flag ?
		(ulonglong) old_nr > (ulonglong) nr :
		old_nr > nr);
      /* (cmp_sign > 0 && res) || (!(cmp_sign > 0) && !res) */
      if ((cmp_sign > 0) ^ (!res))
	old_nr=nr;
    }
    result_field->set_notnull();
  }
  else if (result_field->is_null(0))
    result_field->set_null();
  DBUG_PRINT("info", ("nr: %lld", old_nr));
  result_field->store(old_nr, unsigned_flag);
  DBUG_VOID_RETURN;
}


/**
  @todo
  optimize: do not get result_field in case of args[0] is NULL
*/
void
Item_sum_min_max::min_max_update_decimal_field()
{
  DBUG_ENTER("Item_sum_min_max::min_max_update_decimal_field");
  my_decimal old_val, nr_val;
  const my_decimal *old_nr;
  const my_decimal *nr= args[0]->val_decimal(&nr_val);
  if (!args[0]->null_value)
  {
    if (result_field->is_null(0))
      old_nr=nr;
    else
    {
      old_nr= result_field->val_decimal(&old_val);
      bool res= my_decimal_cmp(old_nr, nr) > 0;
      /* (cmp_sign > 0 && res) || (!(cmp_sign > 0) && !res) */
      if ((cmp_sign > 0) ^ (!res))
        old_nr=nr;
    }
    result_field->set_notnull();
    result_field->store_decimal(old_nr);
  }
  else if (result_field->is_null(0))
    result_field->set_null();
  DBUG_VOID_RETURN;
}


double Item_avg_field_double::val_real()
{
  // fix_fields() never calls for this Item
  double nr;
  longlong count;
  uchar *res;

  float8get(nr,field->ptr);
  res= (field->ptr+sizeof(double));
  count= sint8korr(res);

  if ((null_value= !count))
    return 0.0;
  return nr/(double) count;
}


my_decimal *Item_avg_field_decimal::val_decimal(my_decimal *dec_buf)
{
  // fix_fields() never calls for this Item
  longlong count= sint8korr(field->ptr + dec_bin_size);
  if ((null_value= !count))
    return 0;

  my_decimal dec_count, dec_field(field->ptr, f_precision, f_scale);
  int2my_decimal(E_DEC_FATAL_ERROR, count, 0, &dec_count);
  my_decimal_div(E_DEC_FATAL_ERROR, dec_buf,
                 &dec_field, &dec_count, prec_increment);
  return dec_buf;
}


double Item_std_field::val_real()
{
  double nr;
  // fix_fields() never calls for this Item
  nr= Item_variance_field::val_real();
  DBUG_ASSERT(nr >= 0.0);
  return sqrt(nr);
}


double Item_variance_field::val_real()
{
  // fix_fields() never calls for this Item
  Stddev tmp(field->ptr);
  if ((null_value= (tmp.count() <= sample)))
    return 0.0;

  return tmp.result(sample);
}


/****************************************************************************
** Functions to handle dynamic loadable aggregates
** Original source by: Alexis Mikhailov <root@medinf.chuvashia.su>
** Adapted for UDAs by: Andreas F. Bobak <bobak@relog.ch>.
** Rewritten by: Monty.
****************************************************************************/

#ifdef HAVE_DLOPEN

void Item_udf_sum::clear()
{
  DBUG_ENTER("Item_udf_sum::clear");
  udf.clear();
  DBUG_VOID_RETURN;
}

bool Item_udf_sum::add()
{
  my_bool tmp_null_value;
  DBUG_ENTER("Item_udf_sum::add");
  udf.add(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(0);
}


bool Item_udf_sum::supports_removal() const
{
  DBUG_ENTER("Item_udf_sum::supports_remove");
  DBUG_PRINT("info", ("support: %d", udf.supports_removal()));
  DBUG_RETURN(udf.supports_removal());
}


void Item_udf_sum::remove()
{
  my_bool tmp_null_value;
  DBUG_ENTER("Item_udf_sum::remove");
  udf.remove(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_VOID_RETURN;
}


void Item_udf_sum::cleanup()
{
  /*
    udf_handler::cleanup() nicely handles case when we have not
    original item but one created by copy_or_same() method.
  */
  udf.cleanup();
  Item_sum::cleanup();
}


void Item_udf_sum::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (i)
      str->append(',');
    args[i]->print(str, query_type);
  }
  str->append(')');
}


Item *Item_sum_udf_float::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_udf_float(thd, this);
}

double Item_sum_udf_float::val_real()
{
  my_bool tmp_null_value;
  double res;
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_sum_udf_float::val");
  DBUG_PRINT("enter",("result_type: %d  arg_count: %d",
		     args[0]->result_type(), arg_count));
  res= udf.val(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_sum_udf_float::val_str(String *str)
{
  return val_string_from_real(str);
}


my_decimal *Item_sum_udf_float::val_decimal(my_decimal *dec)
{
  return val_decimal_from_real(dec);
}


my_decimal *Item_sum_udf_decimal::val_decimal(my_decimal *dec_buf)
{
  my_decimal *res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_func_udf_decimal::val_decimal");
  DBUG_PRINT("enter",("result_type: %d  arg_count: %d",
                     args[0]->result_type(), arg_count));

  res= udf.val_decimal(&tmp_null_value, dec_buf);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


Item *Item_sum_udf_decimal::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_udf_decimal(thd, this);
}


Item *Item_sum_udf_int::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_udf_int(thd, this);
}

longlong Item_sum_udf_int::val_int()
{
  my_bool tmp_null_value;
  longlong res;
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_sum_udf_int::val_int");
  DBUG_PRINT("enter",("result_type: %d  arg_count: %d",
		     args[0]->result_type(), arg_count));
  res= udf.val_int(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_sum_udf_int::val_str(String *str)
{
  return val_string_from_int(str);
}

my_decimal *Item_sum_udf_int::val_decimal(my_decimal *dec)
{
  return val_decimal_from_int(dec);
}


/** Default max_length is max argument length. */

bool Item_sum_udf_str::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_sum_udf_str::fix_length_and_dec");
  max_length=0;
  for (uint i = 0; i < arg_count; i++)
    set_if_bigger(max_length,args[i]->max_length);
  DBUG_RETURN(FALSE);
}


Item *Item_sum_udf_str::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_sum_udf_str(thd, this);
}


my_decimal *Item_sum_udf_str::val_decimal(my_decimal *dec)
{
  return val_decimal_from_string(dec);
}

String *Item_sum_udf_str::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_sum_udf_str::str");
  String *res=udf.val_str(str,&str_value);
  null_value = !res;
  DBUG_RETURN(res);
}

#endif /* HAVE_DLOPEN */


/*****************************************************************************
 GROUP_CONCAT function

 SQL SYNTAX:
  GROUP_CONCAT([DISTINCT] expr,... [ORDER BY col [ASC|DESC],...]
    [SEPARATOR str_const])

 concat of values from "group by" operation

 BUGS
   Blobs doesn't work with DISTINCT or ORDER BY
*****************************************************************************/



/** 
  Compares the values for fields in expr list of GROUP_CONCAT.
  @note
       
     GROUP_CONCAT([DISTINCT] expr [,expr ...]
              [ORDER BY {unsigned_integer | col_name | expr}
                  [ASC | DESC] [,col_name ...]]
              [SEPARATOR str_val])
 
  @return
  @retval -1 : key1 < key2 
  @retval  0 : key1 = key2
  @retval  1 : key1 > key2 
*/

extern "C"
int group_concat_key_cmp_with_distinct(void* arg, const void* key1,
                                       const void* key2)
{
  Item_func_group_concat *item_func= (Item_func_group_concat*)arg;

  for (uint i= 0; i < item_func->arg_count_field; i++)
  {
    Item *item= item_func->args[i];
    /*
      If item is a const item then either get_tmp_table_field returns 0
      or it is an item over a const table.
    */
    if (item->const_item())
      continue;
    /*
      We have to use get_tmp_table_field() instead of
      real_item()->get_tmp_table_field() because we want the field in
      the temporary table, not the original field
    */
    Field *field= item->get_tmp_table_field();

    if (!field)
      continue;

    uint offset= (field->offset(field->table->record[0]) -
                  field->table->s->null_bytes);
    int res= field->cmp((uchar*)key1 + offset, (uchar*)key2 + offset);
    if (res)
      return res;
  }
  return 0;
}


/*
  @brief
    Comparator function for DISTINCT clause taking into account NULL values.

  @note
    Used for JSON_ARRAYAGG function
*/

int group_concat_key_cmp_with_distinct_with_nulls(void* arg,
                                                  const void* key1_arg,
                                                  const void* key2_arg)
{
  Item_func_group_concat *item_func= (Item_func_group_concat*)arg;

  uchar *key1= (uchar*)key1_arg + item_func->table->s->null_bytes;
  uchar *key2= (uchar*)key2_arg + item_func->table->s->null_bytes;

  /*
    JSON_ARRAYAGG function only accepts one argument.
  */

  Item *item= item_func->args[0];
  /*
    If item is a const item then either get_tmp_table_field returns 0
    or it is an item over a const table.
  */
  if (item->const_item())
    return 0;
  /*
    We have to use get_tmp_table_field() instead of
    real_item()->get_tmp_table_field() because we want the field in
    the temporary table, not the original field
  */
  Field *field= item->get_tmp_table_field();

  if (!field)
    return 0;

  if (field->is_null_in_record((uchar*)key1_arg) &&
      field->is_null_in_record((uchar*)key2_arg))
    return 0;

  if (field->is_null_in_record((uchar*)key1_arg))
    return -1;

  if (field->is_null_in_record((uchar*)key2_arg))
    return 1;

  uint offset= (field->offset(field->table->record[0]) -
                field->table->s->null_bytes);
  int res= field->cmp(key1 + offset, key2 + offset);
  if (res)
    return res;
  return 0;
}


/**
  function of sort for syntax: GROUP_CONCAT(expr,... ORDER BY col,... )
*/

extern "C"
int group_concat_key_cmp_with_order(void* arg, const void* key1, 
                                    const void* key2)
{
  Item_func_group_concat* grp_item= (Item_func_group_concat*) arg;
  ORDER **order_item, **end;

  for (order_item= grp_item->order, end=order_item+ grp_item->arg_count_order;
       order_item < end;
       order_item++)
  {
    Item *item= *(*order_item)->item;
    /* 
      If field_item is a const item then either get_tmp_table_field returns 0
      or it is an item over a const table. 
    */
    if (item->const_item())
      continue;
    /*
      If item is a const item then either get_tmp_table_field returns 0
      or it is an item over a const table.
    */
    if (item->const_item())
      continue;
    /*
      We have to use get_tmp_table_field() instead of
      real_item()->get_tmp_table_field() because we want the field in
      the temporary table, not the original field

      Note that for the case of ROLLUP, field may point to another table
      tham grp_item->table. This is however ok as the table definitions are
      the same.
    */
    Field *field= item->get_tmp_table_field();
    if (!field)
      continue;

    uint offset= (field->offset(field->table->record[0]) -
                  field->table->s->null_bytes);
    int res= field->cmp((uchar*)key1 + offset, (uchar*)key2 + offset);
    if (res)
      return ((*order_item)->direction == ORDER::ORDER_ASC) ? res : -res;
  }
  /*
    We can't return 0 because in that case the tree class would remove this
    item as double value. This would cause problems for case-changes and
    if the returned values are not the same we do the sort on.
  */
  return 1;
}


/*
  @brief
    Comparator function for ORDER BY clause taking into account NULL values.

  @note
    Used for JSON_ARRAYAGG function
*/

int group_concat_key_cmp_with_order_with_nulls(void *arg, const void *key1_arg,
                                               const void *key2_arg)
{
  Item_func_group_concat* grp_item= (Item_func_group_concat*) arg;
  ORDER **order_item, **end;

  uchar *key1= (uchar*)key1_arg + grp_item->table->s->null_bytes;
  uchar *key2= (uchar*)key2_arg + grp_item->table->s->null_bytes;

  for (order_item= grp_item->order, end=order_item+ grp_item->arg_count_order;
       order_item < end;
       order_item++)
  {
    Item *item= *(*order_item)->item;
    /*
      If field_item is a const item then either get_tmp_table_field returns 0
      or it is an item over a const table.
    */
    if (item->const_item())
      continue;
    /*
      We have to use get_tmp_table_field() instead of
      real_item()->get_tmp_table_field() because we want the field in
      the temporary table, not the original field

      Note that for the case of ROLLUP, field may point to another table
      tham grp_item->table. This is however ok as the table definitions are
      the same.
    */
    Field *field= item->get_tmp_table_field();
    if (!field)
      continue;

    if (field->is_null_in_record((uchar*)key1_arg) &&
        field->is_null_in_record((uchar*)key2_arg))
      continue;

    if (field->is_null_in_record((uchar*)key1_arg))
      return ((*order_item)->direction == ORDER::ORDER_ASC) ?  -1 : 1;

    if (field->is_null_in_record((uchar*)key2_arg))
      return ((*order_item)->direction == ORDER::ORDER_ASC) ?  1 :  -1;

    uint offset= (field->offset(field->table->record[0]) -
                  field->table->s->null_bytes);
    int res= field->cmp((uchar*)key1 + offset, (uchar*)key2 + offset);
    if (res)
      return ((*order_item)->direction == ORDER::ORDER_ASC) ? res : -res;
  }
  /*
    We can't return 0 because in that case the tree class would remove this
    item as double value. This would cause problems for case-changes and
    if the returned values are not the same we do the sort on.
  */
  return 1;
}


static void report_cut_value_error(THD *thd, uint row_count, const char *fname)
{
  size_t fn_len= strlen(fname);
  char *fname_upper= (char *) my_alloca(fn_len + 1);
  if (!fname_upper)
    fname_upper= (char*) fname;                 // Out of memory
  else
    memcpy(fname_upper, fname, fn_len+1);
  my_caseup_str(&my_charset_latin1, fname_upper);
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_CUT_VALUE_GROUP_CONCAT,
                      ER_THD(thd, ER_CUT_VALUE_GROUP_CONCAT),
                      row_count, fname_upper);
  my_afree(fname_upper);
}


void Item_func_group_concat::cut_max_length(String *result,
        uint old_length, uint max_length) const
{
  const char *ptr= result->ptr();
  /*
    It's ok to use item->result.length() as the fourth argument
    as this is never used to limit the length of the data.
    Cut is done with the third argument.
  */
  size_t add_length= Well_formed_prefix(collation.collation,
                                      ptr + old_length,
                                      ptr + max_length,
                                      result->length()).length();
  result->length(old_length + add_length);
}


/**
  Append data from current leaf to item->result.
*/

extern "C"
int dump_leaf_key(void* key_arg, element_count count __attribute__((unused)),
                  void* item_arg)
{
  Item_func_group_concat *item= (Item_func_group_concat *) item_arg;
  TABLE *table= item->table;
  uint max_length= table->in_use->variables.group_concat_max_len;
  String tmp((char *)table->record[1], table->s->reclength,
             default_charset_info);
  String tmp2;
  uchar *key= (uchar *) key_arg;
  String *result= &item->result;
  Item **arg= item->args, **arg_end= item->args + item->arg_count_field;
  uint old_length= result->length();

  ulonglong *offset_limit= &item->copy_offset_limit;
  ulonglong *row_limit = &item->copy_row_limit;
  if (item->limit_clause && !(*row_limit))
  {
    item->result_finalized= true;
    return 1;
  }

  tmp.length(0);

  if (item->limit_clause && (*offset_limit))
  {
    item->row_count++;
    (*offset_limit)--;
    return 0;
  }

  if (!item->result_finalized)
    item->result_finalized= true;
  else
    result->append(*item->separator);

  for (; arg < arg_end; arg++)
  {
    String *res;
    /*
      We have to use get_tmp_table_field() instead of
      real_item()->get_tmp_table_field() because we want the field in
      the temporary table, not the original field
      We also can't use table->field array to access the fields
      because it contains both order and arg list fields.
     */
    if ((*arg)->const_item())
      res= item->get_str_from_item(*arg, &tmp);
    else
    {
      Field *field= (*arg)->get_tmp_table_field();
      if (field)
      {
        uint offset= (field->offset(field->table->record[0]) -
                      table->s->null_bytes);
        DBUG_ASSERT(offset < table->s->reclength);
        res= item->get_str_from_field(*arg, field, &tmp, key,
                                      offset + item->get_null_bytes());
      }
      else
        res= item->get_str_from_item(*arg, &tmp);
    }

    if (res)
      result->append(*res);
  }

  if (item->limit_clause)
    (*row_limit)--;
  item->row_count++;

  /* stop if length of result more than max_length */
  if (result->length() > max_length)
  {
    THD *thd= current_thd;
    item->cut_max_length(result, old_length, max_length);
    item->warning_for_row= TRUE;
    report_cut_value_error(thd, item->row_count, item->func_name());

    /**
       To avoid duplicated warnings in Item_func_group_concat::val_str()
    */
    if (table && table->blob_storage)
      table->blob_storage->set_truncated_value(false);
    return 1;
  }
  return 0;
}


/**
  Constructor of Item_func_group_concat.

  @param distinct_arg   distinct
  @param select_list    list of expression for show values
  @param order_list     list of sort columns
  @param separator_arg  string value of separator.
*/

Item_func_group_concat::
Item_func_group_concat(THD *thd, Name_resolution_context *context_arg,
                       bool distinct_arg, List<Item> *select_list,
                       const SQL_I_List<ORDER> &order_list,
                       String *separator_arg, bool limit_clause,
                       Item *row_limit_arg, Item *offset_limit_arg)
  :Item_sum(thd), tmp_table_param(0), separator(separator_arg), tree(0),
   unique_filter(NULL), table(0),
   order(0), context(context_arg),
   arg_count_order(order_list.elements),
   arg_count_field(select_list->elements),
   row_count(0),
   distinct(distinct_arg),
   warning_for_row(FALSE), always_null(FALSE),
   force_copy_fields(0), row_limit(NULL),
   offset_limit(NULL), limit_clause(limit_clause),
   copy_offset_limit(0), copy_row_limit(0), original(0)
{
  Item *item_select;
  Item **arg_ptr;

  quick_group= FALSE;
  arg_count= arg_count_field + arg_count_order;

  /*
    We need to allocate:
    args - arg_count_field+arg_count_order
           (for possible order items in temporary tables)
    order - arg_count_order
  */
  if (!(args= (Item**) thd->alloc(sizeof(Item*) * arg_count * 2 +
                                  sizeof(ORDER*)*arg_count_order)))
    return;

  order= (ORDER**)(args + arg_count);

  /* fill args items of show and sort */
  List_iterator_fast<Item> li(*select_list);

  for (arg_ptr=args ; (item_select= li++) ; arg_ptr++)
    *arg_ptr= item_select;

  if (arg_count_order)
  {
    ORDER **order_ptr= order;
    for (ORDER *order_item= order_list.first;
         order_item != NULL;
         order_item= order_item->next)
    {
      (*order_ptr++)= order_item;
      *arg_ptr= *order_item->item;
      order_item->item= arg_ptr++;
    }
  }

  /* orig_args is only used for print() */
  orig_args= (Item**) (order + arg_count_order);
  if (arg_count)
    memcpy(orig_args, args, sizeof(Item*) * arg_count);
  if (limit_clause)
  {
    row_limit= row_limit_arg;
    offset_limit= offset_limit_arg;
  }
}


Item_func_group_concat::Item_func_group_concat(THD *thd,
                                               Item_func_group_concat *item)
  :Item_sum(thd, item),
  tmp_table_param(item->tmp_table_param),
  separator(item->separator),
  tree(item->tree),
  tree_len(item->tree_len),
  unique_filter(item->unique_filter),
  table(item->table),
  context(item->context),
  arg_count_order(item->arg_count_order),
  arg_count_field(item->arg_count_field),
  row_count(item->row_count),
  distinct(item->distinct),
  warning_for_row(item->warning_for_row),
  always_null(item->always_null),
  force_copy_fields(item->force_copy_fields),
  row_limit(item->row_limit), offset_limit(item->offset_limit),
  limit_clause(item->limit_clause),copy_offset_limit(item->copy_offset_limit),
  copy_row_limit(item->copy_row_limit), original(item)
{
  quick_group= item->quick_group;
  result.set_charset(collation.collation);

  /*
    Since the ORDER structures pointed to by the elements of the 'order' array
    may be modified in find_order_in_list() called from
    Item_func_group_concat::setup(), create a copy of those structures so that
    such modifications done in this object would not have any effect on the
    object being copied.
  */
  ORDER *tmp;
  if (!(tmp= (ORDER *) thd->alloc(sizeof(ORDER *) * arg_count_order +
                                  sizeof(ORDER) * arg_count_order)))
    return;
  order= (ORDER **)(tmp + arg_count_order);
  for (uint i= 0; i < arg_count_order; i++, tmp++)
  {
    /*
      Compiler generated copy constructor is used to
      to copy all the members of ORDER struct.
      It's also necessary to update ORDER::next pointer
      so that it points to new ORDER element.
    */
    new (tmp) st_order(*(item->order[i])); 
    tmp->next= (i + 1 == arg_count_order) ? NULL : (tmp + 1);
    order[i]= tmp;
  }
}


void Item_func_group_concat::cleanup()
{
  DBUG_ENTER("Item_func_group_concat::cleanup");
  Item_sum::cleanup();

  /*
    Free table and tree if they belong to this item (if item have not pointer
    to original item from which was made copy => it own its objects )
  */
  if (!original)
  {
    delete tmp_table_param;
    tmp_table_param= 0;
    if (table)
    {
      THD *thd= table->in_use;
      if (table->blob_storage)
        delete table->blob_storage;
      free_tmp_table(thd, table);
      table= 0;
      if (tree)
      {
        delete_tree(tree, 0);
        tree= 0;
      }
      if (unique_filter)
      {
        delete unique_filter;
        unique_filter= NULL;
      }
    }
    DBUG_ASSERT(tree == 0);
  }
  /*
    As the ORDER structures pointed to by the elements of the
    'order' array may be modified in find_order_in_list() called
    from Item_func_group_concat::setup() to point to runtime
    created objects, we need to reset them back to the original
    arguments of the function.
  */
  ORDER **order_ptr= order;
  for (uint i= 0; i < arg_count_order; i++)
  {
    (*order_ptr)->item= &args[arg_count_field + i];
    order_ptr++;
  }
  DBUG_VOID_RETURN;
}


Item *Item_func_group_concat::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_func_group_concat(thd, this);
}


void Item_func_group_concat::clear()
{
  result.length(0);
  result.copy();
  null_value= TRUE;
  warning_for_row= FALSE;
  result_finalized= false;
  if (offset_limit)
    copy_offset_limit= offset_limit->val_int();
  if (row_limit)
    copy_row_limit= row_limit->val_int();
  if (tree)
  {
    reset_tree(tree);
    tree_len= 0;
  }
  if (unique_filter)
    unique_filter->reset();
  if (table && table->blob_storage)
    table->blob_storage->reset();
  /* No need to reset the table as we never call write_row */
}

struct st_repack_tree {
  TREE tree;
  TABLE *table;
  size_t len, maxlen;
};

extern "C"
int copy_to_tree(void* key, element_count count __attribute__((unused)),
                 void* arg)
{
  struct st_repack_tree *st= (struct st_repack_tree*)arg;
  TABLE *table= st->table;
  Field* field= table->field[0];
  const uchar *ptr= field->ptr_in_record((uchar*)key - table->s->null_bytes);
  size_t len= (size_t)field->val_int(ptr);

  DBUG_ASSERT(count == 1);
  if (!tree_insert(&st->tree, key, 0, st->tree.custom_arg))
    return 1;

  st->len += len;
  return st->len > st->maxlen;
}

bool Item_func_group_concat::repack_tree(THD *thd)
{
  struct st_repack_tree st;
  int size= tree->size_of_element;
  if (!tree->offset_to_key)
    size-= sizeof(void*);

  init_tree(&st.tree, (size_t) MY_MIN(thd->variables.max_heap_table_size,
                                      thd->variables.sortbuff_size/16), 0,
            size, get_comparator_function_for_order_by(), NULL,
            (void*) this, MYF(MY_THREAD_SPECIFIC));
  DBUG_ASSERT(tree->size_of_element == st.tree.size_of_element);
  st.table= table;
  st.len= 0;
  st.maxlen= thd->variables.group_concat_max_len;
  tree_walk(tree, &copy_to_tree, &st, left_root_right);
  if (st.len <= st.maxlen) // Copying aborted. Must be OOM
  {
    delete_tree(&st.tree, 0);
    return 1;
  }
  delete_tree(tree, 0);
  *tree= st.tree;
  tree_len= st.len;
  return 0;
}


/*
  Repacking the tree is expensive. But it keeps the tree small, and
  inserting into an unnecessary large tree is also waste of time.

  The following number is best-by-test. Test execution time slowly
  decreases up to N=10 (that is, factor=1024) and then starts to increase,
  again, very slowly.
*/
#define GCONCAT_REPACK_FACTOR 10

bool Item_func_group_concat::add(bool exclude_nulls)
{
  if (always_null && exclude_nulls)
    return 0;
  copy_fields(tmp_table_param);
  if (copy_funcs(tmp_table_param->items_to_copy, table->in_use))
    return TRUE;

  size_t row_str_len= 0;
  StringBuffer<MAX_FIELD_WIDTH> buf;
  String *res;
  for (uint i= 0; i < arg_count_field; i++)
  {
    Item *show_item= args[i];
    if (show_item->const_item())
      continue;

    Field *field= show_item->get_tmp_table_field();
    if (field)
    {
      if (field->is_null_in_record((const uchar*) table->record[0]) &&
          exclude_nulls)
        return 0;                    // Skip row if it contains null

      buf.set_buffer_if_not_allocated(&my_charset_bin);
      if (tree && (res= field->val_str(&buf)))
        row_str_len+= res->length();
    }
    else
    {
      /*
        should not reach here, we create temp table for all the arguments of
        the group_concat function
      */
      DBUG_ASSERT(0);
    }
  }

  null_value= FALSE;
  bool row_eligible= TRUE;

  if (distinct) 
  {
    /* Filter out duplicate rows. */
    uint count= unique_filter->elements_in_tree();
    unique_filter->unique_add(get_record_pointer());
    if (count == unique_filter->elements_in_tree())
      row_eligible= FALSE;
  }

  TREE_ELEMENT *el= 0;                          // Only for safety
  if (row_eligible && tree)
  {
    THD *thd= table->in_use;
    table->field[0]->store(row_str_len, FALSE);
    if ((tree_len >> GCONCAT_REPACK_FACTOR) > thd->variables.group_concat_max_len
        && tree->elements_in_tree > 1)
      if (repack_tree(thd))
        return 1;
    el= tree_insert(tree, get_record_pointer(), 0, tree->custom_arg);
    /* check if there was enough memory to insert the row */
    if (!el)
      return 1;
    tree_len+= row_str_len;
  }

  /*
    In case of GROUP_CONCAT with DISTINCT or ORDER BY (or both) don't dump the
    row to the output buffer here. That will be done in val_str.
  */
  if (row_eligible && !warning_for_row && (!tree && !distinct))
    dump_leaf_key(get_record_pointer(), 1, this);

  return 0;
}


bool
Item_func_group_concat::fix_fields(THD *thd, Item **ref)
{
  uint i;                       /* for loop variable */
  DBUG_ASSERT(fixed() == 0);

  if (init_sum_func_check(thd))
    return TRUE;

  set_maybe_null();

  /*
    Fix fields for select list and ORDER clause
  */

  for (i=0 ; i < arg_count ; i++)
  {
    if (args[i]->fix_fields_if_needed_for_scalar(thd, &args[i]))
      return TRUE;
    /* We should ignore FIELD's in arguments to sum functions */
    with_flags|= (args[i]->with_flags & ~item_with_t::FIELD);
  }

  /* skip charset aggregation for order columns */
  if (agg_arg_charsets_for_string_result(collation,
                                         args, arg_count - arg_count_order))
    return 1;

  result.set_charset(collation.collation);
  result_field= 0;
  null_value= 1;
  max_length= (uint32) MY_MIN((ulonglong) thd->variables.group_concat_max_len
                              / collation.collation->mbminlen
                              * collation.collation->mbmaxlen, UINT_MAX32);

  uint32 offset;
  if (separator->needs_conversion(separator->length(), separator->charset(),
                                  collation.collation, &offset))
  {
    uint32 buflen= collation.collation->mbmaxlen * separator->length();
    uint errors, conv_length;
    char *buf;
    String *new_separator;

    if (!(buf= (char*) thd->stmt_arena->alloc(buflen)) ||
        !(new_separator= new(thd->stmt_arena->mem_root)
                           String(buf, buflen, collation.collation)))
      return TRUE;
    
    conv_length= copy_and_convert(buf, buflen, collation.collation,
                                  separator->ptr(), separator->length(),
                                  separator->charset(), &errors);
    new_separator->length(conv_length);
    separator= new_separator;
  }

  if (check_sum_func(thd, ref))
    return TRUE;

  base_flags|= item_base_t::FIXED;
  return FALSE;
}


bool Item_func_group_concat::setup(THD *thd)
{
  List<Item> list;
  SELECT_LEX *select_lex= thd->lex->current_select;
  const bool order_or_distinct= MY_TEST(arg_count_order > 0 || distinct);
  DBUG_ENTER("Item_func_group_concat::setup");

  /*
    Currently setup() can be called twice. Please add
    assertion here when this is fixed.
  */
  if (table || tree)
    DBUG_RETURN(FALSE);

  if (!(tmp_table_param= new (thd->mem_root) TMP_TABLE_PARAM))
    DBUG_RETURN(TRUE);

  /* Push all not constant fields to the list and create a temp table */
  always_null= 0;
  for (uint i= 0; i < arg_count_field; i++)
  {
    Item *item= args[i];
    if (list.push_back(item, thd->mem_root))
      DBUG_RETURN(TRUE);
    if (item->const_item() && item->is_null() && skip_nulls())
    {
      always_null= 1;
      DBUG_RETURN(FALSE);
    }
  }

  List<Item> all_fields(list);
  /*
    Try to find every ORDER expression in the list of GROUP_CONCAT
    arguments. If an expression is not found, prepend it to
    "all_fields". The resulting field list is used as input to create
    tmp table columns.
  */
  if (arg_count_order)
  {
    uint n_elems= arg_count_order + all_fields.elements;
    ref_pointer_array= static_cast<Item**>(thd->alloc(sizeof(Item*) * n_elems));
    if (!ref_pointer_array)
      DBUG_RETURN(TRUE);
    memcpy(ref_pointer_array, args, arg_count * sizeof(Item*));
    DBUG_ASSERT(context);
    if (setup_order(thd, Ref_ptr_array(ref_pointer_array, n_elems),
                    context->table_list, list,  all_fields, *order))
      DBUG_RETURN(TRUE);
    /*
      Prepend the field to store the length of the string representation
      of this row. Used to detect when the tree goes over group_concat_max_len
    */
    Item *item= new (thd->mem_root)
                    Item_uint(thd, thd->variables.group_concat_max_len);
    if (!item || all_fields.push_front(item, thd->mem_root))
      DBUG_RETURN(TRUE);
  }

  count_field_types(select_lex, tmp_table_param, all_fields, 0);
  tmp_table_param->force_copy_fields= force_copy_fields;
  tmp_table_param->hidden_field_count= (arg_count_order > 0);
  DBUG_ASSERT(table == 0);
  if (order_or_distinct)
  {
    /*
      Convert bit fields to bigint's in the temporary table.
      Needed as we cannot compare two table records containing BIT fields
      stored in the the tree used for distinct/order by.
      Moreover we don't even save in the tree record null bits 
      where BIT fields store parts of their data.
    */
    store_bit_fields_as_bigint_in_tempory_table(&all_fields);
  }

  /*
    We have to create a temporary table to get descriptions of fields
    (types, sizes and so on).

    Note that in the table, we first have the ORDER BY fields, then the
    field list.
  */
  if (!(table= create_tmp_table(thd, tmp_table_param, all_fields,
                                (ORDER*) 0, 0, TRUE,
                                (select_lex->options |
                                 thd->variables.option_bits),
                                HA_POS_ERROR, &empty_clex_str)))
    DBUG_RETURN(TRUE);
  table->file->extra(HA_EXTRA_NO_ROWS);
  table->no_rows= 1;

  /**
    Initialize blob_storage if GROUP_CONCAT is used
    with ORDER BY | DISTINCT and BLOB field count > 0.    
  */
  if (order_or_distinct && table->s->blob_fields)
    table->blob_storage= new (thd->mem_root) Blob_mem_storage();

  /*
     Need sorting or uniqueness: init tree and choose a function to sort.
     Don't reserve space for NULLs: if any of gconcat arguments is NULL,
     the row is not added to the result.
  */
  uint tree_key_length= table->s->reclength - table->s->null_bytes;

  if (arg_count_order)
  {
    tree= &tree_base;
    /*
      Create a tree for sorting. The tree is used to sort (according to the
      syntax of this function). If there is no ORDER BY clause, we don't
      create this tree.
    */
    init_tree(tree, (size_t)MY_MIN(thd->variables.max_heap_table_size,
                                   thd->variables.sortbuff_size/16), 0,
              tree_key_length + get_null_bytes(),
              get_comparator_function_for_order_by(), NULL, (void*) this,
              MYF(MY_THREAD_SPECIFIC));
    tree_len= 0;
  }

  if (distinct)
    unique_filter= (new (thd->mem_root)
                    Unique(get_comparator_function_for_distinct(),
                           (void*)this,
                           tree_key_length + get_null_bytes(),
                           ram_limitation(thd)));
  if ((row_limit && row_limit->cmp_type() != INT_RESULT) ||
      (offset_limit && offset_limit->cmp_type() != INT_RESULT))
  {
    my_error(ER_INVALID_VALUE_TO_LIMIT, MYF(0));
    DBUG_RETURN(TRUE);
  }
  
  DBUG_RETURN(FALSE);
}


/* This is used by rollup to create a separate usable copy of the function */

void Item_func_group_concat::make_unique()
{
  tmp_table_param= 0;
  table=0;
  original= 0;
  force_copy_fields= 1;
  tree= 0;
}


String* Item_func_group_concat::val_str(String* str)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return 0;

  if (!result_finalized) // Result yet to be written.
  {
    if (tree != NULL) // order by
      tree_walk(tree, &dump_leaf_key, this, left_root_right);
    else if (distinct) // distinct (and no order by).
      unique_filter->walk(table, &dump_leaf_key, this);
    else if (row_limit && copy_row_limit == (ulonglong)row_limit->val_int())
      return &result;
    else
      DBUG_ASSERT(false); // Can't happen
  }

  if (table && table->blob_storage && 
      table->blob_storage->is_truncated_value())
  {
    warning_for_row= true;
    report_cut_value_error(current_thd, row_count, func_name());
  }

  return &result;
}


/*
  @brief
    Get the comparator function for DISTINT clause
*/

qsort_cmp2 Item_func_group_concat::get_comparator_function_for_distinct()
{
  return skip_nulls() ?
         group_concat_key_cmp_with_distinct :
         group_concat_key_cmp_with_distinct_with_nulls;
}


/*
  @brief
    Get the comparator function for ORDER BY clause
*/

qsort_cmp2 Item_func_group_concat::get_comparator_function_for_order_by()
{
  return skip_nulls() ?
         group_concat_key_cmp_with_order :
         group_concat_key_cmp_with_order_with_nulls;
}


/*

  @brief
    Get the record pointer of the current row of the table

  @details
    look at the comments for Item_func_group_concat::get_null_bytes
*/

uchar* Item_func_group_concat::get_record_pointer()
{
  return skip_nulls() ?
         table->record[0] + table->s->null_bytes :
         table->record[0];
}


/*
  @brief
    Get the null bytes for the table if required.

  @details
    This function is used for GROUP_CONCAT (or JSON_ARRAYAGG) implementation
    where the Unique tree or the ORDER BY tree may store the null values,
    in such case we also store the null bytes inside each node of the tree.

*/

uint Item_func_group_concat::get_null_bytes()
{
  return skip_nulls() ? 0 : table->s->null_bytes;
}


void Item_func_group_concat::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  if (distinct)
    str->append(STRING_WITH_LEN("distinct "));
  for (uint i= 0; i < arg_count_field; i++)
  {
    if (i)
      str->append(',');
    orig_args[i]->print(str, query_type);
  }
  if (arg_count_order)
  {
    str->append(STRING_WITH_LEN(" order by "));
    for (uint i= 0 ; i < arg_count_order ; i++)
    {
      if (i)
        str->append(',');
      orig_args[i + arg_count_field]->print(str, query_type);
      if (order[i]->direction == ORDER::ORDER_ASC)
        str->append(STRING_WITH_LEN(" ASC"));
     else
        str->append(STRING_WITH_LEN(" DESC"));
    }
  }

  if (sum_func() == GROUP_CONCAT_FUNC)
  {
    str->append(STRING_WITH_LEN(" separator \'"));
    str->append_for_single_quote(separator->ptr(), separator->length());
    str->append(STRING_WITH_LEN("\'"));
  }

  if (limit_clause)
  {
    str->append(STRING_WITH_LEN(" limit "));
    if (offset_limit)
    {
      offset_limit->print(str, query_type);
      str->append(',');
    }
    row_limit->print(str, query_type);
  }
  str->append(STRING_WITH_LEN(")"));
}


Item_func_group_concat::~Item_func_group_concat()
{
  if (!original && unique_filter)
    delete unique_filter;    
}
