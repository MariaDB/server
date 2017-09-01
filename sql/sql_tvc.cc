#include "sql_list.h"
#include "sql_tvc.h"
#include "sql_class.h"
#include "opt_range.h"
#include "sql_select.h"
#include "sql_explain.h"
#include "sql_parse.h"

/**
  @brief
    Defines types of matrix columns elements where matrix rows are defined by
    some lists of values.

  @param
    @param thd_arg   	        The context of the statement
    @param li	     		The iterator on the list of lists
    @param holders   		The structure where types of matrix columns are stored
    @param first_list_el_count  Count of the list values that should be. It should
				be the same for each list of lists elements. It contains
				number of elements of the first list from list of lists.

  @details
    For each list list_a from list of lists the procedure gets its elements types and
    aggregates them with the previous ones stored in holders. If list_a is the first
    one in the list of lists its elements types are put in holders.
    The errors can be reported when count of list_a elements is different from the
    first_list_el_count. Also error can be reported when aggregation can't be made.

  @retval
    true    if an error was reported
    false   otherwise
*/

bool join_type_handlers_for_tvc(THD *thd_arg, List_iterator_fast<List_item> &li,
			        Type_holder *holders, uint first_list_el_count)
{
  DBUG_ENTER("join_type_handlers_for_tvc");
  List_item *lst;
  li.rewind();
  bool first= true;
  
  while ((lst=li++))
  {
    List_iterator_fast<Item> it(*lst);
    Item *item;
  
    if (first_list_el_count != lst->elements)
    {
      my_message(ER_WRONG_NUMBER_OF_VALUES_IN_TVC,
                 ER_THD(thd_arg, ER_WRONG_NUMBER_OF_VALUES_IN_TVC),
                 MYF(0));
      DBUG_RETURN(true);
    }
    for (uint pos= 0; (item=it++); pos++)
    {
       if (item->type() == Item::FIELD_ITEM)
      {
        my_error(ER_UNKNOWN_VALUE_IN_TVC, MYF(0),
		 ((Item_field *)item)->full_name(),
		 MYF(0));
	DBUG_RETURN(true);
      }
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
    Defines attributes of matrix columns elements where matrix rows are defined by
    some lists of values.

  @param
    @param thd_arg   	        The context of the statement
    @param li	     		The iterator on the list of lists
    @param holders   		The structure where names of matrix columns are stored
    @param count_of_lists	Count of list of lists elements
    @param first_list_el_count  Count of the list values that should be. It should
				be the same for each list of lists elements. It contains
				number of elements of the first list from list of lists.

  @details
    For each list list_a from list of lists the procedure gets its elements attributes and
    aggregates them with the previous ones stored in holders.
    The errors can be reported when aggregation can't be made.

  @retval
    true     if an error was reported
    false    otherwise
*/

bool get_type_attributes_for_tvc(THD *thd_arg,
			         List_iterator_fast<List_item> &li, 
                                 Type_holder *holders, uint count_of_lists,
				 uint first_list_el_count)
{
  DBUG_ENTER("get_type_attributes_for_tvc");
  List_item *lst;
  li.rewind();
  
  for (uint pos= 0; pos < first_list_el_count; pos++)
  {
    if (holders[pos].alloc_arguments(thd_arg, count_of_lists))
      DBUG_RETURN(true);
  }
  
  while ((lst=li++))
  {
    List_iterator_fast<Item> it(*lst);
    Item *item;
    for (uint holder_pos= 0 ; (item= it++); holder_pos++)
    {
      DBUG_ASSERT(item->fixed);
      holders[holder_pos].add_argument(item);
    }
  }
  
  for (uint pos= 0; pos < first_list_el_count; pos++)
  {
    if (holders[pos].aggregate_attributes(thd_arg))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/**
  @brief
    Prepare of TVC

  @param
    @param thd_arg      The context of the statement
    @param sl	     	The select where this TVC is defined
    @param tmp_result	Structure that contains the information
			about where result of the query should be sent
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

bool table_value_constr::prepare(THD *thd_arg, SELECT_LEX *sl,
				 select_result *tmp_result,
				 st_select_lex_unit *unit_arg)
{
  DBUG_ENTER("table_value_constr::prepare");
  List_iterator_fast<List_item> li(lists_of_values);
  
  List_item *first_elem= li++;
  uint cnt= first_elem->elements;
  Type_holder *holders;
  
  if (!(holders= new (thd_arg->mem_root)
                Type_holder[cnt]) || 
       join_type_handlers_for_tvc(thd_arg, li, holders,
				  cnt) ||
       get_type_attributes_for_tvc(thd_arg, li, holders,
				   lists_of_values.elements, cnt))
    DBUG_RETURN(true);
  
  List_iterator_fast<Item> it(*first_elem);
  Item *item;
  
  sl->item_list.empty();
  for (uint pos= 0; (item= it++); pos++)
  {
    /* Error's in 'new' will be detected after loop */
    Item_type_holder *new_holder= new (thd_arg->mem_root)
                      Item_type_holder(thd_arg,
                                       &item->name,
                                       holders[pos].type_handler(),
                                       &holders[pos]/*Type_all_attributes*/,
                                       holders[pos].get_maybe_null());
    new_holder->fix_fields(thd_arg, 0);
    sl->item_list.push_back(new_holder);
  }
  
  if (thd_arg->is_fatal_error)
    DBUG_RETURN(true); // out of memory
    
  result= tmp_result;
  
  if (result && result->prepare(sl->item_list, unit_arg))
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}


/**
    Save Query Plan Footprint
*/

int table_value_constr::save_explain_data_intern(THD *thd_arg,
						 Explain_query *output)
{
  const char *message= "No tables used";
  DBUG_ENTER("table_value_constr::save_explain_data_intern");
  DBUG_PRINT("info", ("Select 0x%lx, type %s, message %s",
		      (ulong)select_lex, select_lex->type,
		      message));
  DBUG_ASSERT(have_query_plan == QEP_AVAILABLE);

  /* There should be no attempts to save query plans for merged selects */
  DBUG_ASSERT(!select_lex->master_unit()->derived ||
               select_lex->master_unit()->derived->is_materialized_derived() ||
               select_lex->master_unit()->derived->is_with_table());

  explain= new (output->mem_root) Explain_select(output->mem_root,
                                                 thd_arg->lex->analyze_stmt);
  select_lex->set_explain_type(true);

  explain->select_id= select_lex->select_number;
  explain->select_type= select_lex->type;
  explain->linkage= select_lex->linkage;
  explain->using_temporary= NULL;
  explain->using_filesort=  NULL;
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

void table_value_constr::optimize(THD *thd_arg)
{
  create_explain_query_if_not_exists(thd_arg->lex, thd_arg->mem_root);
  have_query_plan= QEP_AVAILABLE;

  if (select_lex->select_number != UINT_MAX &&
      select_lex->select_number != INT_MAX /* this is not a UNION's "fake select */ &&
      have_query_plan != QEP_NOT_PRESENT_YET &&
      thd_arg->lex->explain && // for "SET" command in SPs.
      (!thd_arg->lex->explain->get_select(select_lex->select_number)))
  {
    save_explain_data_intern(thd_arg, thd_arg->lex->explain);
  }
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

  while ((elem=li++))
  {
    result->send_data(*elem);
  }

  if (result->send_eof())
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}

/**
  @brief
    Print list of lists

  @param str         Where to print to
  @param query_type  The mode of printing
  @param values      List of lists that needed to be print

  @details
    The method prints a string representation of list of lists in the
    string str. The parameter query_type specifies the mode of printing.
*/

void print_list_of_lists(String *str,
			 enum_query_type query_type,
			 List<List_item> *values)
{
  str->append(STRING_WITH_LEN("values "));

  bool first= 1;
  List_iterator_fast<List_item> li(*values);
  List_item *list;
  while ((list=li++))
  {
    if (first)
      first= 0;
    else
      str->append(',');

    str->append('(');

    List_iterator_fast<Item> it(*list);
    Item *item;
    first= 1;

    while ((item=it++))
    {
      if (first)
        first= 0;
      else
        str->append(',');

      item->print(str, query_type);
    }
    str->append(')');
  }
}


/**
  @brief
    Print this TVC

  @param thd_arg     The context of the statement
  @param str         Where to print to
  @param query_type  The mode of printing

  @details
    The method prints a string representation of this TVC in the
    string str. The parameter query_type specifies the mode of printing.
*/

void table_value_constr::print(THD *thd_arg, String *str,
			       enum_query_type query_type)
{
  DBUG_ASSERT(thd_arg);

  print_list_of_lists(str, query_type, &lists_of_values);
}


/**
  @brief
    Transforms IN-predicate in IN-subselect

  @param thd_arg     The context of the statement
  @param arg         Argument is 0 in this context

  @details
    The method creates this SELECT statement:

    SELECT * FROM (VALUES  values) AS new_tvc

    If during creation of SELECT statement some action is
    unsuccesfull backup is made to the state in which system
    was at the beginning of the procedure.

  @retval
    pointer to the created SELECT statement
    NULL - if creation was unsuccesfull
*/

Item *Item_func_in::in_predicate_to_in_subs_transformer(THD *thd,
							uchar *arg)
{
  SELECT_LEX *old_select= thd->lex->current_select;

  List<List_item> values;
  Item *item;
  SELECT_LEX *sel;
  SELECT_LEX_UNIT *unit;
  TABLE_LIST *new_tab;
  Table_ident *ti;
  Item_in_subselect *in_subs;

  Query_arena backup;
  Query_arena *arena= thd->activate_stmt_arena_if_needed(&backup);
  LEX *lex= thd->lex;

  char buff[6];
  LEX_CSTRING alias;

  /*
    Creation of values list of lists
  */
  bool list_of_lists= false;

  if (args[1]->type() == Item::ROW_ITEM)
    list_of_lists= true;

  for (uint i=1; i < arg_count; i++)
  {
    List<Item> *new_value= new (thd->mem_root) List<Item>();

    if (list_of_lists)
    {
      Item_row *in_list= (Item_row *)(args[i]);

      for (uint j=0; j < in_list->cols(); i++)
	new_value->push_back(in_list->element_index(j), thd->mem_root);
    }
    else
      new_value->push_back(args[i]);

    values.push_back(new_value, thd->mem_root);
  }

  /*
    Creation of TVC name
  */
  alias.length= my_snprintf(buff, sizeof(buff),
                            "tvc_%u", old_select->cur_tvc);
  alias.str= thd->strmake(buff, alias.length);
  if (!alias.str)
    goto err;

  /*
    Creation of SELECT statement: SELECT * FROM ...
  */

  if (mysql_new_select(lex, 1, NULL))
    goto err;

  mysql_init_select(lex);
  lex->current_select->parsing_place= SELECT_LIST;

  item= new (thd->mem_root) Item_field(thd, &lex->current_select->context,
                                       NULL, NULL, &star_clex_str);
  if (item == NULL)
    goto err;
  if (add_item_to_list(thd, item))
    goto err;
  (lex->current_select->with_wild)++;

  /*
    Creation of TVC as derived table
  */

  lex->derived_tables|= DERIVED_SUBQUERY;
  if (mysql_new_select(lex, 1, NULL))
    goto err;

  mysql_init_select(lex);

  sel= lex->current_select;
  unit= sel->master_unit();
  sel->linkage= DERIVED_TABLE_TYPE;

  if (!(sel->tvc=
          new (thd->mem_root)
	    table_value_constr(values,
                               sel,
                               sel->options)))
    goto err;

  lex->check_automatic_up(UNSPECIFIED_TYPE);
  lex->current_select= sel= unit->outer_select();

  ti= new (thd->mem_root) Table_ident(unit);
  if (ti == NULL)
    goto err;

  if (!(new_tab= sel->add_table_to_list(thd,
                                        ti, &alias, 0,
                                        TL_READ, MDL_SHARED_READ)))
    goto err;

  sel->add_joined_table(new_tab);

  new_tab->select_lex->add_where_field(new_tab->derived->first_select());

  sel->context.table_list=
  sel->context.first_name_resolution_table=
  sel->table_list.first;

  sel->where= 0;
  sel->set_braces(false);
  unit->with_clause= 0;

  if (!sel)
    goto err;

  sel->parsing_place= old_select->parsing_place;
  sel->table_list.first->derived_type= 10;

  in_subs= new (thd->mem_root) Item_in_subselect(thd, args[0], sel);
  thd->lex->derived_tables |= DERIVED_SUBQUERY;
  in_subs->emb_on_expr_nest= emb_on_expr_nest;

  old_select->cur_tvc++;
  thd->lex->current_select= old_select;

  if (arena)
    thd->restore_active_arena(arena, &backup);

  in_subs->fix_fields(thd, (Item **)&in_subs);
  return in_subs;

err:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  return this;
}

/**
  @brief
    Checks if this IN-predicate can be transformed in IN-subquery
    with TVC

  @param thd     The context of the statement

  @details
    Compares the number of elements in the list of
    values in this IN-predicate with the
    in_subquery_conversion_threshold special variable

  @retval
    true     if transformation can be made
    false    otherwise
*/

bool Item_func_in::can_be_transformed_in_tvc(THD *thd)
{
  uint opt_can_be_used= arg_count;

  if (args[1]->type() == Item::ROW_ITEM)
    opt_can_be_used*= ((Item_row *)(args[1]))->cols();

  if (opt_can_be_used < thd->variables.in_subquery_conversion_threshold)
    return false;

  return true;
}

/**
  @brief
    Calls transformer that transforms IN-predicate into IN-subquery
    for this select

  @param thd_arg     The context of the statement

  @details
    Calls in_predicate_to_in_subs_transformer
    for WHERE-part and each table from join list of this SELECT
*/

bool JOIN::transform_in_predicate_into_tvc(THD *thd_arg)
{
  if (!select_lex->in_funcs.elements)
    return false;

  SELECT_LEX *old_select= thd_arg->lex->current_select;
  enum_parsing_place old_parsing_place= select_lex->parsing_place;

  thd_arg->lex->current_select= select_lex;
  if (conds)
  {
    select_lex->parsing_place= IN_WHERE;
    conds=
      conds->transform(thd_arg,
		       &Item::in_predicate_to_in_subs_transformer,
                       (uchar*) 0);
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
          table->on_expr->transform(thd_arg,
		                    &Item::in_predicate_to_in_subs_transformer,
                                    (uchar*) 0);
	table->prep_on_expr= table->on_expr ?
                             table->on_expr->copy_andor_structure(thd) : 0;
      }
    }
  }
  select_lex->in_funcs.empty();
  select_lex->parsing_place= old_parsing_place;
  thd_arg->lex->current_select= old_select;
  return false;
}