#include "sql_class.h"
#include "sql_lex.h"
#include "sql_cte.h"
#include "sql_view.h"    // for make_valid_column_names
#include "sql_parse.h"


/**
  @brief
    Check dependencies between tables defined in a list of with clauses
 
  @param 
    with_clauses_list  Pointer to the first clause in the list

  @details
    The procedure just calls the method With_clause::check_dependencies
    for each member of the given list.

  @retval
    false   on success
    true    on failure
*/

bool check_dependencies_in_with_clauses(With_clause *with_clauses_list)
{
  for (With_clause *with_clause= with_clauses_list;
       with_clause;
       with_clause= with_clause->next_with_clause)
  {
    if (with_clause->check_dependencies())
      return true;
  }
  return false;
}


/**
  @brief
    Check dependencies between tables defined in this with clause

 @details
    The method performs the following actions for this with clause:

    1. Test for definitions of the tables with the same name.
    2. For each table T defined in this with clause look for tables
       from the same with clause that are used in the query that
       specifies T and set the dependencies of T on these tables
       in dependency_map. 
    3. Build the transitive closure of the above direct dependencies
       to find out all recursive definitions.
    4. If this with clause is not specified as recursive then
       for each with table T defined in this with clause check whether
       it is used in any definition that follows the definition of T.

  @retval
    true    if an error is reported 
    false   otherwise
*/

bool With_clause::check_dependencies()
{
  if (dependencies_are_checked)
    return false;
  /* 
    Look for for definitions with the same query name.
    When found report an error and return true immediately.
    For each table T defined in this with clause look for all other tables from
    the same with with clause that are used in the specification of T.
    For each such table set the dependency bit in the dependency map of
    with element for T.
  */
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    for (With_element *elem= first_elem;
         elem != with_elem;
         elem= elem->next_elem)
    {
      if (my_strcasecmp(system_charset_info, with_elem->query_name->str,
                        elem->query_name->str) == 0)
      {
	my_error(ER_DUP_QUERY_NAME, MYF(0), with_elem->query_name->str);
	return true;
      }
    }
    with_elem->check_dependencies_in_unit(with_elem->spec);   
  }
  /* Build the transitive closure of the direct dependencies found above */
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    table_map with_elem_map=  with_elem->get_elem_map();
    for (With_element *elem= first_elem; elem != NULL; elem= elem->next_elem)
    {
      if (elem->dependency_map & with_elem_map)
        elem->dependency_map |= with_elem->dependency_map;
    }   
  }

  /*
    Mark those elements where tables are defined with direct or indirect recursion.
    Report an error when recursion (direct or indirect) is used to define a table.
  */ 
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (with_elem->dependency_map & with_elem->get_elem_map())
      with_elem->is_recursive= true;
  }   
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (with_elem->is_recursive)
    {  
      my_error(ER_RECURSIVE_QUERY_IN_WITH_CLAUSE, MYF(0),
               with_elem->query_name->str);
      return true;
    }
  }

  if (!with_recursive)
  {
    /* 
      For each with table T defined in this with clause check whether
      it is used in any definition that follows the definition of T.
    */
    for (With_element *with_elem= first_elem;
         with_elem != NULL;
         with_elem= with_elem->next_elem)
    {
      With_element *checked_elem= with_elem->next_elem;
      for (uint i = with_elem->number+1;
           i < elements;
           i++, checked_elem= checked_elem->next_elem)
      {
        if (with_elem->check_dependency_on(checked_elem))
        {
          my_error(ER_WRONG_ORDER_IN_WITH_CLAUSE, MYF(0),
                   with_elem->query_name->str, checked_elem->query_name->str);
          return true;
        }
      }
    }
  }
	
  dependencies_are_checked= true;
  return false;
}


/**
  @brief
    Check dependencies on the sibling with tables used in the given unit

  @param unit  The unit where the siblings are to be searched for

  @details
    The method recursively looks through all from lists encountered
    the given unit. If it finds a reference to a table that is
    defined in the same with clause to which this element belongs
    the method set the bit of dependency on this table in the
    dependency_map of this element.
*/

void With_element::check_dependencies_in_unit(st_select_lex_unit *unit)
{
  st_select_lex *sl= unit->first_select();
  for (; sl; sl= sl->next_select())
  {
    for (TABLE_LIST *tbl= sl->table_list.first; tbl; tbl= tbl->next_local)
    {
      if (!tbl->with)
        tbl->with= owner->find_table_def(tbl);
      if (!tbl->with && tbl->select_lex)
        tbl->with= tbl->select_lex->find_table_def_in_with_clauses(tbl);
      if (tbl->with && tbl->with->owner== this->owner)
        set_dependency_on(tbl->with);
    }
    st_select_lex_unit *inner_unit= sl->first_inner_unit();
    for (; inner_unit; inner_unit= inner_unit->next_unit())
      check_dependencies_in_unit(inner_unit);
  }
}


/**
  @brief
    Search for the definition of a table among the elements of this with clause
 
  @param table    The reference to the table that is looked for

  @details
    The function looks through the elements of this with clause trying to find
    the definition of the given table. When it encounters the element with
    the same query name as the table's name it returns this element. If no
    such definitions are found the function returns NULL.

  @retval
    found with element if the search succeeded
    NULL - otherwise
*/    

With_element *With_clause::find_table_def(TABLE_LIST *table)
{
  for (With_element *with_elem= first_elem; 
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (my_strcasecmp(system_charset_info, with_elem->query_name->str, table->table_name) == 0) 
    {
      return with_elem;
    }
  }
  return NULL;
}


/**
  @brief
    Perform context analysis for all unreferenced tables defined in with clause

  @param thd   The context of the statement containing this with clause

  @details
    For each unreferenced table T defined in this with clause the method
    calls the method With_element::prepare_unreferenced that performs
    context analysis of the element with the definition of T.

  @retval
    false   If context analysis does not report any error
    true    Otherwise
*/

bool With_clause::prepare_unreferenced_elements(THD *thd)
{
  for (With_element *with_elem= first_elem; 
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (!with_elem->is_referenced() && with_elem->prepare_unreferenced(thd))
      return true;
  }

  return false;
}


/**
  @brief
    Save the specification of the given with table as a string

  @param thd        The context of the statement containing this with element
  @param spec_start The beginning of the specification in the input string
  @param spec_end   The end of the specification in the input string

  @details
    The method creates for a string copy of the specification used in this element.
    The method is called when the element is parsed. The copy may be used to
    create clones of the specification whenever they are needed.

  @retval
    false   on success
    true    on failure
*/
  
bool With_element::set_unparsed_spec(THD *thd, char *spec_start, char *spec_end)
{
  unparsed_spec.length= spec_end - spec_start;
  unparsed_spec.str= (char*) sql_memdup(spec_start, unparsed_spec.length+1);
  unparsed_spec.str[unparsed_spec.length]= '\0';

  if (!unparsed_spec.str)
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATALERROR), 
             static_cast<int>(unparsed_spec.length));
    return true;
  }
  return false;
}


/**
  @brief
   Create a clone of the specification for the given with table
    
  @param thd        The context of the statement containing this with element
  @param with_table The reference to the table defined in this element for which
                     the clone is created.

  @details
    The method creates a clone of the specification used in this element.
    The clone is created for the given reference to the table defined by
    this element.
    The clone is created when the string with the specification saved in
    unparsed_spec is fed into the parser as an input string. The parsing
    this string a unit object representing the specification is build.
    A chain of all table references occurred in the specification is also
    formed.
    The method includes the new unit and its sub-unit into hierarchy of
    the units of the main query. I also insert the constructed chain of the 
    table references into the chain of all table references of the main query.

  @note
    Clones is created only for not first references to tables defined in
    the with clause. They are necessary for merged specifications because
    the optimizer handles any such specification as independent on the others.
    When a table defined in the with clause is materialized in a temporary table
    one could do without specification clones. However in this case they
    are created as well, because currently different table references to a
    the same temporary table cannot share the same definition structure.

  @retval
     pointer to the built clone if succeeds
     NULL - otherwise
*/

st_select_lex_unit *With_element::clone_parsed_spec(THD *thd,
                                                    TABLE_LIST *with_table)
{
  LEX *lex;
  st_select_lex_unit *res= NULL; 
  Query_arena backup;
  Query_arena *arena= thd->activate_stmt_arena_if_needed(&backup);

  if (!(lex= (LEX*) new(thd->mem_root) st_lex_local))
  {
    if (arena)
      thd->restore_active_arena(arena, &backup);
    return res;
  }
  LEX *old_lex= thd->lex;
  thd->lex= lex;

  bool parse_status= false;
  Parser_state parser_state;
  TABLE_LIST *spec_tables;
  TABLE_LIST *spec_tables_tail;
  st_select_lex *with_select;

  if (parser_state.init(thd, unparsed_spec.str, unparsed_spec.length))
    goto err;
  lex_start(thd);
  with_select= &lex->select_lex;
  with_select->select_number= ++thd->select_number;
  parse_status= parse_sql(thd, &parser_state, 0);
  if (parse_status)
    goto err;
  spec_tables= lex->query_tables;
  spec_tables_tail= 0;
  for (TABLE_LIST *tbl= spec_tables;
       tbl;
       tbl= tbl->next_global)
  {
    tbl->grant.privilege= with_table->grant.privilege;
    spec_tables_tail= tbl;
  }
  if (spec_tables)
  {
    if (with_table->next_global)
    {
      spec_tables_tail->next_global= with_table->next_global;
      with_table->next_global->prev_global= &spec_tables_tail->next_global;
    }
    else
    {
      old_lex->query_tables_last= &spec_tables_tail->next_global;
    }
    spec_tables->prev_global= &with_table->next_global;
    with_table->next_global= spec_tables;
  }
  res= &lex->unit;
  
  lex->unit.include_down(with_table->select_lex);
  lex->unit.set_slave(with_select); 
  old_lex->all_selects_list=
    (st_select_lex*) (lex->all_selects_list->
		      insert_chain_before(
			(st_select_lex_node **) &(old_lex->all_selects_list),
                        with_select));
  lex_end(lex);
err:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  thd->lex= old_lex;
  return res;
}


/**
  @brief
    Process optional column list of this with element
     
  @details
    The method processes the column list in this with element.    
    It reports an error if the cardinality of this list differs from
    the cardinality of the select lists in the specification of the table
    defined by this with element. Otherwise it renames the columns
    of these select lists and sets the flag column_list_is_processed to true
    preventing processing the list for the second time.

  @retval
    true   if an error was reported 
    false  otherwise
*/    

bool With_element::process_column_list()
{
  if (column_list_is_processed)
    return false;

  st_select_lex *select= spec->first_select();

  if (column_list.elements)  //  The column list is optional
  {
    List_iterator_fast<Item> it(select->item_list);
    List_iterator_fast<LEX_STRING> nm(column_list);
    Item *item;
    LEX_STRING *name;

    if (column_list.elements != select->item_list.elements)
    {
      my_error(ER_WITH_COL_WRONG_LIST, MYF(0));
      return true;
    }
    /* Rename the columns of the first select in the specification query */
    while ((item= it++, name= nm++))
    {
      item->set_name(name->str, (uint) name->length, system_charset_info);
      item->is_autogenerated_name= false;
    }
  }

  make_valid_column_names(select->item_list);

  column_list_is_processed= true;
  return false;
}


/**
  @brief
    Perform context analysis the definition of an unreferenced table
     
  @param thd        The context of the statement containing this with element

  @details
    The method assumes that this with element contains the definition
    of a table that is not used anywhere. In this case one has to check
    that context conditions are met. 

  @retval
    true   if an error was reported 
    false  otherwise
*/    

bool With_element::prepare_unreferenced(THD *thd)
{
  bool rc= false;
  st_select_lex *first_sl= spec->first_select();

  /* Prevent name resolution for field references out of with elements */
  for (st_select_lex *sl= first_sl;
       sl;
       sl= sl->next_select())
    sl->context.outer_context= 0;

  thd->lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_DERIVED;
  if (!spec->prepared &&
      (spec->prepare(thd, 0, 0) ||
       process_column_list() ||
       check_duplicate_names(first_sl->item_list, 1)))
    rc= true;
 
  thd->lex->context_analysis_only&= ~CONTEXT_ANALYSIS_ONLY_DERIVED;
  return rc;
}


/**
   @brief
     Search for the definition of the given table referred in this select node

  @param table  reference to the table whose definition is searched for
     
  @details  
    The method looks for the definition the table whose reference is occurred
    in the FROM list of this select node. First it searches for it in the
    with clause attached to the unit this select node belongs to. If such a
    definition is not found there the embedding units are looked through.

  @retval
    pointer to the found definition if the search has been successful
    NULL -  otherwise
*/    

With_element *st_select_lex::find_table_def_in_with_clauses(TABLE_LIST *table)
{
  With_element *found= NULL;
  for (st_select_lex *sl= this;
       sl;
       sl= sl->master_unit()->outer_select())
  {
    With_clause *with_clause=sl->get_with_clause();
    if (with_clause && (found= with_clause->find_table_def(table)))
      return found;      
  }
  return found;
}


/**
   @brief
     Set the specifying unit in this reference to a with table  
     
  @details  
    The method assumes that the given element with_elem defines the table T
    this table reference refers to.
    If this is the first reference to T the method just sets its specification
    in the field 'derived' as the unit that yields T. Otherwise the method  
    first creates a clone specification and sets rather this clone in this field.
 
  @retval
    false   on success
    true    on failure
*/    

bool TABLE_LIST::set_as_with_table(THD *thd, With_element *with_elem)
{
  with= with_elem;
  if (!with_elem->is_referenced())
    derived= with_elem->spec;
  else 
  {
    if(!(derived= with_elem->clone_parsed_spec(thd, this)))
      return true;
    derived->with_element= with_elem;
  }
  with_elem->inc_references();
  return false;
}


/**
  @brief
    Print this with clause
  
  @param str         Where to print to
  @param query_type  The mode of printing 
     
  @details
    The method prints a string representation of this clause in the 
    string str. The parameter query_type specifies the mode of printing.
*/    

void With_clause::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("WITH "));
  if (with_recursive)
    str->append(STRING_WITH_LEN("RECURSIVE "));
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    with_elem->print(str, query_type);
    if (with_elem != first_elem)
      str->append(", ");
  }
}


/**
   @brief
     Print this with element
  
  @param str         Where to print to
  @param query_type  The mode of printing 
     
  @details
    The method prints a string representation of this with element in the 
    string str. The parameter query_type specifies the mode of printing.
*/

void With_element::print(String *str, enum_query_type query_type)
{
  str->append(query_name);
  str->append(STRING_WITH_LEN(" AS "));
  str->append('(');
  spec->print(str, query_type);
  str->append(')');
}

