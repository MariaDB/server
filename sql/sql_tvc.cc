/* Copyright (c) 2017, MariaDB

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

#include "mariadb.h"
#include "sql_list.h"
#include "sql_tvc.h"
#include "sql_class.h"
#include "opt_range.h"
#include "sql_select.h"
#include "sql_explain.h"
#include "sql_parse.h"
#include "sql_cte.h"

/**
  @brief
    Fix fields for TVC values

  @param
    @param thd	 The context of the statement
    @param li	 The iterator on the list of lists

  @details
    Call fix_fields procedure for TVC values.

  @retval
    true     if an error was reported
    false    otherwise
*/

bool fix_fields_for_tvc(THD *thd, List_iterator_fast<List_item> &li)
{
  DBUG_ENTER("fix_fields_for_tvc");
  List_item *lst;
  li.rewind();

  while ((lst= li++))
  {
    List_iterator_fast<Item> it(*lst);
    Item *item;

    while ((item= it++))
    {
      if (item->fix_fields(thd, 0))
	DBUG_RETURN(true);
    }
  }
  DBUG_RETURN(false);
}


/**
  @brief
    Defines types of matrix columns elements where matrix rows are defined by
    some lists of values.

  @param
    @param thd   	 The context of the statement
    @param li	     	 The iterator on the list of lists
    @param holders   	 The structure where types of matrix columns are stored
    @param first_list_el_count  Count of the list values. It should be the same
                                for each list of lists elements. It contains
			        number of elements of the first list from list of
                                lists.

  @details
    For each list list_a from list of lists the procedure gets its elements
    types and aggregates them with the previous ones stored in holders. If
    list_a is the first one in the list of lists its elements types are put in
    holders. The errors can be reported when count of list_a elements is
    different from the first_list_el_count. Also error can be reported whe
    n aggregation can't be made.

  @retval
    true    if an error was reported
    false   otherwise
*/

bool join_type_handlers_for_tvc(THD *thd, List_iterator_fast<List_item> &li,
			        Type_holder *holders, uint first_list_el_count)
{
  DBUG_ENTER("join_type_handlers_for_tvc");
  List_item *lst;
  li.rewind();
  bool first= true;
  
  while ((lst= li++))
  {
    List_iterator_fast<Item> it(*lst);
    Item *item;
  
    if (first_list_el_count != lst->elements)
    {
      my_message(ER_WRONG_NUMBER_OF_VALUES_IN_TVC,
                 ER_THD(thd, ER_WRONG_NUMBER_OF_VALUES_IN_TVC),
                 MYF(0));
      DBUG_RETURN(true);
    }
    for (uint pos= 0; (item=it++); pos++)
    {
      const Type_handler *item_type_handler= item->real_type_handler();
      if (first)
        holders[pos].set_handler(item_type_handler);
      else if (holders[pos].aggregate_for_result(item_type_handler))
      {
        my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
                 holders[pos].type_handler()->name().ptr(),
                 item_type_handler->name().ptr(),
                 "TABLE VALUE CONSTRUCTOR");
        DBUG_RETURN(true);
      }
    }
    first= false;
  }
  DBUG_RETURN(false);
}


/**
  @brief
    Define attributes of matrix columns elements where matrix rows are defined
    by some lists of values.

  @param
    @param thd	  	  The context of the statement
    @param li	     	  The iterator on the list of lists
    @param holders   	  The structure where names of matrix columns are stored
    @param count_of_lists Count of list of lists elements
    @param first_list_el_count  Count of the list values. It should be the same
                                for each list of lists elements. It contains
				number of elements of the first list from list
                                of lists.

  @details
    For each list list_a from list of lists the procedure gets its elements
    attributes and aggregates them with the previous ones stored in holders.
    The errors can be reported when aggregation can't be made.

  @retval
    true     if an error was reported
    false    otherwise
*/

bool get_type_attributes_for_tvc(THD *thd,
			         List_iterator_fast<List_item> &li, 
                                 Type_holder *holders, uint count_of_lists,
				 uint first_list_el_count)
{
  DBUG_ENTER("get_type_attributes_for_tvc");
  List_item *lst;
  li.rewind();
  
  for (uint pos= 0; pos < first_list_el_count; pos++)
  {
    if (holders[pos].alloc_arguments(thd, count_of_lists))
      DBUG_RETURN(true);
  }
  
  while ((lst= li++))
  {
    List_iterator_fast<Item> it(*lst);
    Item *item;
    for (uint holder_pos= 0 ; (item= it++); holder_pos++)
    {
      DBUG_ASSERT(item->is_fixed());
      holders[holder_pos].add_argument(item);
    }
  }
  
  for (uint pos= 0; pos < first_list_el_count; pos++)
  {
    if (holders[pos].aggregate_attributes(thd))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/**
  @brief
    Prepare of TVC

  @param
    @param thd	        The context of the statement
    @param sl	     	The select where this TVC is defined
    @param tmp_result	Structure that contains the information
			about where to send the result of the query
    @param unit_arg  	The union where sl is defined

  @details
    Gets types and attributes of values of this TVC that will be used
    for temporary table creation for this TVC. It creates Item_type_holders
    for each element of the first list from list of lists (VALUES from tvc),
    using its elements name, defined type and attribute.

  @retval
    true     if an error was reported
    false    otherwise
*/

bool table_value_constr::prepare(THD *thd, SELECT_LEX *sl,
				 select_result *tmp_result,
				 st_select_lex_unit *unit_arg)
{
  DBUG_ENTER("table_value_constr::prepare");
  select_lex->in_tvc= true;
  List_iterator_fast<List_item> li(lists_of_values);
  
  List_item *first_elem= li++;
  uint cnt= first_elem->elements;
  Type_holder *holders;
  
  if (cnt == 0)
  {
    my_error(ER_EMPTY_ROW_IN_TVC, MYF(0));
    DBUG_RETURN(true);
  }

  if (fix_fields_for_tvc(thd, li))
    DBUG_RETURN(true);

  if (!(holders= new (thd->stmt_arena->mem_root) Type_holder[cnt]) ||
       join_type_handlers_for_tvc(thd, li, holders, cnt) ||
       get_type_attributes_for_tvc(thd, li, holders,
				   lists_of_values.elements, cnt))
    DBUG_RETURN(true);
  
  List_iterator_fast<Item> it(*first_elem);
  Item *item;
  Query_arena *arena, backup;
  arena=thd->activate_stmt_arena_if_needed(&backup);
  
  sl->item_list.empty();
  for (uint pos= 0; (item= it++); pos++)
  {
    /* Error's in 'new' will be detected after loop */
    Item_type_holder *new_holder= new (thd->mem_root)
                      Item_type_holder(thd, item, holders[pos].type_handler(),
                                       &holders[pos]/*Type_all_attributes*/,
                                       holders[pos].get_maybe_null());
    sl->item_list.push_back(new_holder);
  }
  if (arena)
    thd->restore_active_arena(arena, &backup);
  
  if (unlikely(thd->is_fatal_error))
    DBUG_RETURN(true); // out of memory
    
  result= tmp_result;
  
  if (result && result->prepare(sl->item_list, unit_arg))
    DBUG_RETURN(true);

  select_lex->in_tvc= false;
  DBUG_RETURN(false);
}


/**
    Save Query Plan Footprint
*/

int table_value_constr::save_explain_data_intern(THD *thd,
						 Explain_query *output)
{
  const char *message= "No tables used";
  DBUG_ENTER("table_value_constr::save_explain_data_intern");
  DBUG_PRINT("info", ("Select %p, type %s, message %s",
		      select_lex, select_lex->type,
		      message));
  DBUG_ASSERT(have_query_plan == QEP_AVAILABLE);

  /* There should be no attempts to save query plans for merged selects */
  DBUG_ASSERT(!select_lex->master_unit()->derived ||
               select_lex->master_unit()->derived->is_materialized_derived() ||
               select_lex->master_unit()->derived->is_with_table());

  explain= new (output->mem_root) Explain_select(output->mem_root,
                                                 thd->lex->analyze_stmt);
  if (!explain)
    DBUG_RETURN(1);

  select_lex->set_explain_type(true);

  explain->select_id= select_lex->select_number;
  explain->select_type= select_lex->type;
  explain->linkage= select_lex->get_linkage();
  explain->using_temporary= false;
  explain->using_filesort= false;
  /* Setting explain->message means that all other members are invalid */
  explain->message= message;

  if (select_lex->master_unit()->derived)
    explain->connection_type= Explain_node::EXPLAIN_NODE_DERIVED;

  output->add_node(explain);

  if (select_lex->is_top_level_node())
    output->query_plan_ready();

  DBUG_RETURN(0);
}


/**
  Optimization of TVC
*/

bool table_value_constr::optimize(THD *thd)
{
  create_explain_query_if_not_exists(thd->lex, thd->mem_root);
  have_query_plan= QEP_AVAILABLE;

  if (select_lex->select_number != UINT_MAX &&
      select_lex->select_number != INT_MAX /* this is not a UNION's "fake select */ &&
      have_query_plan != QEP_NOT_PRESENT_YET &&
      thd->lex->explain && // for "SET" command in SPs.
      (!thd->lex->explain->get_select(select_lex->select_number)))
  {
    return save_explain_data_intern(thd, thd->lex->explain);
  }
  return 0;
}


/**
  Execute of TVC
*/

bool table_value_constr::exec(SELECT_LEX *sl)
{
  DBUG_ENTER("table_value_constr::exec");
  List_iterator_fast<List_item> li(lists_of_values);
  List_item *elem;
  
  if (select_options & SELECT_DESCRIBE)
    DBUG_RETURN(false);

  if (result->send_result_set_metadata(sl->item_list,
                                       Protocol::SEND_NUM_ROWS |
                                       Protocol::SEND_EOF))
  {
    DBUG_RETURN(true);
  }

  while ((elem= li++))
  {
    result->send_data(*elem);
  }

  if (result->send_eof())
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}


/**
  @brief
    Print list

  @param str         The reference on the string representation of the list
  @param list	     The list that needed to be print
  @param query_type  The mode of printing

  @details
    The method saves a string representation of list in the
    string str.
*/

void print_list_item(String *str, List_item *list,
		     enum_query_type query_type)
{
  bool is_first_elem= true;
  List_iterator_fast<Item> it(*list);
  Item *item;

  str->append('(');

  while ((item= it++))
  {
    if (is_first_elem)
      is_first_elem= false;
    else
      str->append(',');

    item->print(str, query_type);
  }

  str->append(')');
}


/**
  @brief
    Print this TVC

  @param thd         The context of the statement
  @param str         The reference on the string representation of this TVC
  @param query_type  The mode of printing

  @details
    The method saves a string representation of this TVC in the
    string str.
*/

void table_value_constr::print(THD *thd, String *str,
			       enum_query_type query_type)
{
  DBUG_ASSERT(thd);

  str->append(STRING_WITH_LEN("values "));

  bool is_first_elem= true;
  List_iterator_fast<List_item> li(lists_of_values);
  List_item *list;

  while ((list= li++))
  {
    if (is_first_elem)
      is_first_elem= false;
    else
      str->append(',');

    print_list_item(str, list, query_type);
  }
}


/**
  @brief
    Create list of lists for TVC from the list of this IN predicate

  @param thd         The context of the statement
  @param values      TVC list of values

  @details
    The method uses the list of values of this IN predicate to build
    an equivalent list of values that can be used in TVC.

    E.g.:

    <value_list> = 5,2,7
    <transformed_value_list> = (5),(2),(7)

    <value_list> = (5,2),(7,1)
    <transformed_value_list> = (5,2),(7,1)

  @retval
    false     if the method succeeds
    true      otherwise
*/

bool Item_func_in::create_value_list_for_tvc(THD *thd,
				             List< List<Item> > *values)
{
  bool is_list_of_rows= args[1]->type() == Item::ROW_ITEM;

  for (uint i=1; i < arg_count; i++)
  {
    char col_name[8];
    List<Item> *tvc_value;
    if (!(tvc_value= new (thd->mem_root) List<Item>()))
      return true;

    if (is_list_of_rows)
    {
      Item_row *row_list= (Item_row *)(args[i]);

      for (uint j=0; j < row_list->cols(); j++)
      {
        if (i == 1)
	{
          sprintf(col_name, "_col_%i", j+1);
          row_list->element_index(j)->set_name(thd, col_name, strlen(col_name),
                                               thd->charset());
        }
	if (tvc_value->push_back(row_list->element_index(j),
				 thd->mem_root))
	  return true;
      }
    }
    else
    {
      if (i == 1)
      {
        sprintf(col_name, "_col_%i", 1);
        args[i]->set_name(thd, col_name, strlen(col_name), thd->charset());
      }
      if (tvc_value->push_back(args[i]->real_item()))
        return true;
    }

    if (values->push_back(tvc_value, thd->mem_root))
      return true;
  }
  return false;
}


/**
  @brief
    Create name for the derived table defined by TVC

  @param thd               The context of the statement
  @param parent_select     The SELECT where derived table is used
  @param alias		   The returned created name

  @details
    Create name for the derived table using current TVC number
    for this parent_select stored in parent_select

  @retval
    true     if creation fails
    false    otherwise
*/

static bool create_tvc_name(THD *thd, st_select_lex *parent_select,
			    LEX_CSTRING *alias)
{
  char buff[6];

  alias->length= my_snprintf(buff, sizeof(buff),
                            "tvc_%u", parent_select->curr_tvc_name);
  alias->str= thd->strmake(buff, alias->length);
  if (!alias->str)
    return true;

  return false;
}


bool Item_subselect::wrap_tvc_in_derived_table(THD *thd,
					       st_select_lex *tvc_sl)
{
  LEX *lex= thd->lex;
  /* SELECT_LEX object where the transformation is performed */
  SELECT_LEX *parent_select= lex->current_select;
  uint8 save_derived_tables= lex->derived_tables;

  Query_arena backup;
  Query_arena *arena= thd->activate_stmt_arena_if_needed(&backup);

  /*
    Create SELECT_LEX of the subquery SQ used in the result of transformation
  */
  lex->current_select= tvc_sl;
  if (mysql_new_select(lex, 0, NULL))
    goto err;
  mysql_init_select(lex);
  /* Create item list as '*' for the subquery SQ */
  Item *item;
  SELECT_LEX *sq_select; // select for IN subquery;
  sq_select= lex->current_select;
  sq_select->set_linkage(tvc_sl->get_linkage());
  sq_select->parsing_place= SELECT_LIST;
  item= new (thd->mem_root) Item_field(thd, &sq_select->context,
                                       NULL, NULL, &star_clex_str);
  if (item == NULL || add_item_to_list(thd, item))
    goto err;
  (sq_select->with_wild)++;
  
  /* Exclude SELECT with TVC */
  tvc_sl->exclude();
  /*
    Create derived table DT that will wrap TVC in the result of transformation
  */
  SELECT_LEX *tvc_select; // select for tvc
  SELECT_LEX_UNIT *derived_unit; // unit for tvc_select
  if (mysql_new_select(lex, 1, tvc_sl))
    goto err;
  tvc_select= lex->current_select;
  derived_unit= tvc_select->master_unit();
  tvc_select->set_linkage(DERIVED_TABLE_TYPE);

  lex->current_select= sq_select;

  /*
    Create the name of the wrapping derived table and
    add it to the FROM list of the subquery SQ
   */
  Table_ident *ti;
  LEX_CSTRING alias;
  TABLE_LIST *derived_tab;
  if (!(ti= new (thd->mem_root) Table_ident(derived_unit)) ||
      create_tvc_name(thd, parent_select, &alias))
    goto err;
  if (!(derived_tab=
          sq_select->add_table_to_list(thd,
				       ti, &alias, 0,
                                       TL_READ, MDL_SHARED_READ)))
    goto err;
  sq_select->add_joined_table(derived_tab);
  sq_select->add_where_field(derived_unit->first_select());
  sq_select->context.table_list= sq_select->table_list.first;
  sq_select->context.first_name_resolution_table= sq_select->table_list.first;
  sq_select->table_list.first->derived_type= DTYPE_TABLE | DTYPE_MATERIALIZE;
  lex->derived_tables|= DERIVED_SUBQUERY;

  sq_select->where= 0;
  sq_select->set_braces(false);
  derived_unit->set_with_clause(0);

  if (engine->engine_type() == subselect_engine::SINGLE_SELECT_ENGINE)
    ((subselect_single_select_engine *) engine)->change_select(sq_select);

  if (arena)
    thd->restore_active_arena(arena, &backup);
  lex->current_select= sq_select;
  return false;

err:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  lex->derived_tables= save_derived_tables;
  lex->current_select= parent_select;
  return true;
}


/**
  @brief
    Transform IN predicate into IN subquery

  @param thd     The context of the statement
  @param arg     Not used

  @details
    The method transforms this IN predicate into in equivalent IN subquery:

    <left_expr> IN (<value_list>)
    =>
    <left_expr> IN (SELECT * FROM (VALUES <transformed_value_list>) AS tvc_#)

    E.g.:

    <value_list> = 5,2,7
    <transformed_value_list> = (5),(2),(7)

    <value_list> = (5,2),(7,1)
    <transformed_value_list> = (5,2),(7,1)

    If the transformation succeeds the method returns the result IN subquery,
    otherwise this IN predicate is returned.

  @retval
    pointer to the result of transformation if succeeded
    pointer to this IN predicate otherwise
*/

Item *Item_func_in::in_predicate_to_in_subs_transformer(THD *thd,
							uchar *arg)
{
  if (!transform_into_subq)
    return this;
  
  transform_into_subq= false;

  List<List_item> values;

  LEX *lex= thd->lex;
  /* SELECT_LEX object where the transformation is performed */
  SELECT_LEX *parent_select= lex->current_select;
  uint8 save_derived_tables= lex->derived_tables;
  
  for (uint i=1; i < arg_count; i++)
  {
    if (!args[i]->const_item())
      return this;
  }

  Query_arena backup;
  Query_arena *arena= thd->activate_stmt_arena_if_needed(&backup);

  /*
    Create SELECT_LEX of the subquery SQ used in the result of transformation
  */
  if (mysql_new_select(lex, 1, NULL))
    goto err;
  mysql_init_select(lex);
  /* Create item list as '*' for the subquery SQ */
  Item *item;
  SELECT_LEX *sq_select; // select for IN subquery;
  sq_select= lex->current_select;
  sq_select->parsing_place= SELECT_LIST;
  item= new (thd->mem_root) Item_field(thd, &sq_select->context,
                                       NULL, NULL, &star_clex_str);
  if (item == NULL || add_item_to_list(thd, item))
    goto err;
  (sq_select->with_wild)++;
  /*
    Create derived table DT that will wrap TVC in the result of transformation
  */
  SELECT_LEX *tvc_select; // select for tvc
  SELECT_LEX_UNIT *derived_unit; // unit for tvc_select
  if (mysql_new_select(lex, 1, NULL))
    goto err;
  mysql_init_select(lex);
  tvc_select= lex->current_select;
  derived_unit= tvc_select->master_unit();
  tvc_select->set_linkage(DERIVED_TABLE_TYPE);

  /* Create TVC used in the transformation */
  if (create_value_list_for_tvc(thd, &values))
    goto err;
  if (!(tvc_select->tvc=
          new (thd->mem_root)
	    table_value_constr(values,
                               tvc_select,
                               tvc_select->options)))
    goto err;

  lex->current_select= sq_select;

  /*
    Create the name of the wrapping derived table and
    add it to the FROM list of the subquery SQ
   */
  Table_ident *ti;
  LEX_CSTRING alias;
  TABLE_LIST *derived_tab;
  if (!(ti= new (thd->mem_root) Table_ident(derived_unit)) ||
      create_tvc_name(thd, parent_select, &alias))
    goto err;
  if (!(derived_tab=
          sq_select->add_table_to_list(thd,
				       ti, &alias, 0,
                                       TL_READ, MDL_SHARED_READ)))
    goto err;
  sq_select->add_joined_table(derived_tab);
  sq_select->add_where_field(derived_unit->first_select());
  sq_select->context.table_list= sq_select->table_list.first;
  sq_select->context.first_name_resolution_table= sq_select->table_list.first;
  sq_select->table_list.first->derived_type= DTYPE_TABLE | DTYPE_MATERIALIZE;
  lex->derived_tables|= DERIVED_SUBQUERY;

  sq_select->where= 0;
  sq_select->set_braces(false);
  derived_unit->set_with_clause(0);

  /* Create IN subquery predicate */
  sq_select->parsing_place= parent_select->parsing_place;
  Item_in_subselect *in_subs;
  Item *sq;
  if (!(in_subs=
          new (thd->mem_root) Item_in_subselect(thd, args[0], sq_select)))
    goto err;
  sq= in_subs;
  if (negated)
    sq= negate_expression(thd, in_subs);
  else
    in_subs->emb_on_expr_nest= emb_on_expr_nest;
  
  if (arena)
    thd->restore_active_arena(arena, &backup);
  thd->lex->current_select= parent_select;

  if (sq->fix_fields(thd, (Item **)&sq))
    goto err;

  parent_select->curr_tvc_name++;
  return sq;

err:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  lex->derived_tables= save_derived_tables;
  thd->lex->current_select= parent_select;
  return NULL;
}


/**
  @brief
    Check if this IN-predicate can be transformed in IN-subquery
    with TVC

  @param thd     The context of the statement

  @details
    Compare the number of elements in the list of
    values in this IN-predicate with the
    in_subquery_conversion_threshold special variable

  @retval
    true     if transformation can be made
    false    otherwise
*/

bool Item_func_in::to_be_transformed_into_in_subq(THD *thd)
{
  uint values_count= arg_count-1;

  if (args[1]->type() == Item::ROW_ITEM)
    values_count*= ((Item_row *)(args[1]))->cols();

  if (values_count < thd->variables.in_subquery_conversion_threshold)
    return false;

  return true;
}


/**
  @brief
    Transform IN predicates into IN subqueries in WHERE and ON expressions

  @param thd     The context of the statement

  @details
    For each IN predicate from AND parts of the WHERE condition and/or
    ON expressions of the SELECT for this join the method performs
    the intransformation into an equivalent IN sunquery if it's needed.

  @retval
    false     always
*/

bool JOIN::transform_in_predicates_into_in_subq(THD *thd)
{
  DBUG_ENTER("JOIN::transform_in_predicates_into_in_subq");
  if (!select_lex->in_funcs.elements)
    DBUG_RETURN(false);

  SELECT_LEX *save_current_select= thd->lex->current_select;
  enum_parsing_place save_parsing_place= select_lex->parsing_place;
  thd->lex->current_select= select_lex;
  if (conds)
  {
    select_lex->parsing_place= IN_WHERE;
    conds=
      conds->transform(thd,
		       &Item::in_predicate_to_in_subs_transformer,
                       (uchar*) 0);
    if (!conds)
      DBUG_RETURN(true);
    select_lex->prep_where= conds ? conds->copy_andor_structure(thd) : 0;
    select_lex->where= conds;
  }

  if (join_list)
  {
    TABLE_LIST *table;
    List_iterator<TABLE_LIST> li(*join_list);
    select_lex->parsing_place= IN_ON;

    while ((table= li++))
    {
      if (table->on_expr)
      {
        table->on_expr=
          table->on_expr->transform(thd,
		                    &Item::in_predicate_to_in_subs_transformer,
                                    (uchar*) 0);
	if (!table->on_expr)
	  DBUG_RETURN(true);
	table->prep_on_expr= table->on_expr ?
                             table->on_expr->copy_andor_structure(thd) : 0;
      }
    }
  }

  select_lex->in_funcs.empty();
  select_lex->parsing_place= save_parsing_place;
  thd->lex->current_select= save_current_select;
  DBUG_RETURN(false);
}

