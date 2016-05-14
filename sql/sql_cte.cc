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

bool check_dependencies_in_with_clauses(THD *thd, With_clause *with_clauses_list)
{
  for (With_clause *with_clause= with_clauses_list;
       with_clause;
       with_clause= with_clause->next_with_clause)
  {
    if (with_clause->check_dependencies(thd))
      return true;
    if (with_clause->check_anchors())
      return true;
    with_clause->move_anchors_ahead();
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

bool With_clause::check_dependencies(THD *thd)
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
    if (with_elem->check_dependencies_in_spec(thd))
      return true;
  }
  /* Build the transitive closure of the direct dependencies found above */
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
    with_elem->derived_dep_map= with_elem->base_dep_map;
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    table_map with_elem_map=  with_elem->get_elem_map();
    for (With_element *elem= first_elem; elem != NULL; elem= elem->next_elem)
    {
      if (elem->derived_dep_map & with_elem_map)
        elem->derived_dep_map |= with_elem->derived_dep_map;
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
    if (with_elem->derived_dep_map & with_elem->get_elem_map())
      with_elem->is_recursive= true;
  }   
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (with_elem->is_recursive)
    {
#if 0  
      my_error(ER_RECURSIVE_QUERY_IN_WITH_CLAUSE, MYF(0),
               with_elem->query_name->str);
      return true;
#endif
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


bool With_element::check_dependencies_in_spec(THD *thd)
{ 
  for (st_select_lex *sl=  spec->first_select(); sl; sl= sl->next_select())
  {
    check_dependencies_in_select(sl, sl->with_dep);
    base_dep_map|= sl->with_dep;
  }
  return false;
}


void With_element::check_dependencies_in_select(st_select_lex *sl, table_map &dep_map)
{
  for (TABLE_LIST *tbl= sl->table_list.first; tbl; tbl= tbl->next_local)
  {
    tbl->with_internal_reference_map= 0;
    if (!tbl->with)
      tbl->with= owner->find_table_def(tbl);
    if (!tbl->with && tbl->select_lex)
      tbl->with= tbl->select_lex->find_table_def_in_with_clauses(tbl);
    if (tbl->with && tbl->with->owner== this->owner)
    {
      dep_map|= tbl->with->get_elem_map();
      tbl->with_internal_reference_map= get_elem_map();
    }
  }
  st_select_lex_unit *inner_unit= sl->first_inner_unit();
  for (; inner_unit; inner_unit= inner_unit->next_unit())
    check_dependencies_in_unit(inner_unit, dep_map);
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

void With_element::check_dependencies_in_unit(st_select_lex_unit *unit,
                                              table_map &dep_map)
{
  st_select_lex *sl= unit->first_select();
  for (; sl; sl= sl->next_select())
  {
    check_dependencies_in_select(sl, dep_map);
  }
}


bool With_clause::check_anchors()
{
  /* Find mutually recursive with elements */
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (!with_elem->is_recursive)
      continue;

    table_map with_elem_dep= with_elem->derived_dep_map;
    table_map with_elem_map= with_elem->get_elem_map();
    for (With_element *elem= with_elem;
         elem != NULL;
	 elem= elem->next_elem)
    {
      if (!elem->is_recursive)
        continue;

      if (elem == with_elem ||
	  ((elem->derived_dep_map & with_elem_map) &&
	   (with_elem_dep & elem->get_elem_map())))
	{
	  with_elem->mutually_recursive|= elem->get_elem_map();
	  elem->mutually_recursive|= with_elem_map;
	}
    }
 
    for (st_select_lex *sl= with_elem->spec->first_select();
         sl;
         sl= sl->next_select())
    {
      if (!(with_elem->mutually_recursive & sl->with_dep))
      {
        with_elem->with_anchor= true;
        break;
      }
    }
  }

  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (!with_elem->is_recursive || with_elem->with_anchor)
      continue;

    table_map anchored= 0;
    for (With_element *elem= with_elem;
	 elem != NULL;
	 elem= elem->next_elem)
    {
      if (elem->mutually_recursive && elem->with_anchor)
	  anchored |= elem->get_elem_map();
    }
    table_map non_anchored= with_elem->mutually_recursive & ~anchored;
    with_elem->work_dep_map= non_anchored & with_elem->base_dep_map;
  }

  /*Building transitive clousure on work_dep_map*/
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    table_map with_elem_map= with_elem->get_elem_map();
    for (With_element *elem= first_elem; elem != NULL; elem= elem->next_elem)
    {
      if (elem->work_dep_map & with_elem_map)
	elem->work_dep_map|= with_elem->work_dep_map;
    }
  }

  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (with_elem->work_dep_map & with_elem->get_elem_map())
    {
      my_error(ER_RECURSIVE_WITHOUT_ANCHORS, MYF(0),
      with_elem->query_name->str);
      return true;
    }
  }

  return false;
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
    if (my_strcasecmp(system_charset_info, with_elem->query_name->str,
		      table->table_name) == 0) 
    {
      table->set_derived();
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
  unparsed_spec.str= (char*) thd->memdup(spec_start, unparsed_spec.length+1);
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
    Rename columns of the unit derived from the spec of this with element
  @param thd        The context of the statement containing the with element
  @param unit       The specification of the with element or its clone

  @details
    The method assumes that the parameter unit is either specification itself
    of this with element or a clone of this specification. The looks through
    the column list in this with element. It reports an error if the cardinality
    of this list differs from the cardinality of select lists in 'unit'.
    Otherwise it renames the columns  of the first select list and sets the flag
    unit->column_list_is_processed to true preventing renaming columns for the
    second time.

  @retval
    true   if an error was reported 
    false  otherwise
*/    

bool 
With_element::rename_columns_of_derived_unit(THD *thd, 
                                             st_select_lex_unit *unit)
{
  if (unit->columns_are_renamed)
    return false;

  st_select_lex *select= unit->first_select();

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
    /* Rename the columns of the first select in the unit */
    while ((item= it++, name= nm++))
    {
      item->set_name(thd, name->str, (uint) name->length, system_charset_info);
      item->is_autogenerated_name= false;
    }
  }
  else
    make_valid_column_names(thd, select->item_list);

  unit->columns_are_renamed= true;

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
       rename_columns_of_derived_unit(thd, spec) ||
       check_duplicate_names(thd, first_sl->item_list, 1)))
    rc= true;
 
  thd->lex->context_analysis_only&= ~CONTEXT_ANALYSIS_ONLY_DERIVED;
  return rc;
}



void With_clause::move_anchors_ahead()
{
  for (With_element *with_elem= first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (with_elem->is_recursive)
     with_elem->move_anchors_ahead();
  }
}
	

void With_element::move_anchors_ahead()
{
  st_select_lex *next_sl;
  st_select_lex *new_pos= spec->first_select();
  st_select_lex *last_sl;
  new_pos->linkage= UNION_TYPE;
  for (st_select_lex *sl= new_pos; sl; sl= next_sl)
  {
    next_sl= sl->next_select(); 
    if (is_anchor(sl))
    {
      sl->move_node(new_pos);
      new_pos= sl->next_select();
    }
    last_sl= sl;
  }
  if (spec->union_distinct)
    spec->union_distinct= last_sl;
  first_recursive= new_pos;
}


bool With_element::is_anchor(st_select_lex *sel)
{
  return !(mutually_recursive & sel->with_dep);
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
    /* Do not look for the table's definition beyond the scope of the view */
    if (sl->master_unit()->is_view)
      break;      
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
  if (!with_elem->is_referenced() || with_elem->is_recursive)
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


bool TABLE_LIST::is_recursive_with_table()
{
  return with && with->is_recursive;
}


bool TABLE_LIST::is_with_table_recursive_reference()
{
  return (with_internal_reference_map &&
          (with->mutually_recursive & with_internal_reference_map));
}



bool st_select_lex::check_unrestricted_recursive()
{
  With_element *with_elem= get_with_element();
  if (!with_elem ||!with_elem->is_recursive)
    return false;
  table_map unrestricted= 0;
  table_map encountered= 0;
  if (with_elem->check_unrestricted_recursive(this, 
					      unrestricted, 
					      encountered))
    return true;
  with_elem->owner->unrestricted|= unrestricted;
  if (with_sum_func)
    with_elem->owner->unrestricted|= with_elem->mutually_recursive;
  return false;
}


bool With_element::check_unrestricted_recursive(st_select_lex *sel, 
					        table_map &unrestricted, 
						table_map &encountered)
{
  List_iterator<TABLE_LIST> ti(sel->leaf_tables);
  TABLE_LIST *tbl;
  while ((tbl= ti++))
  {
    if (tbl->get_unit() && !tbl->is_with_table())
    {
      st_select_lex_unit *unit= tbl->get_unit();
      if (tbl->is_materialized_derived())
      {
        table_map dep_map;
        check_dependencies_in_unit(unit, dep_map);
        if (dep_map & get_elem_map())
        {
	  my_error(ER_REF_TO_RECURSIVE_WITH_TABLE_IN_DERIVED,
	  	   MYF(0), query_name->str);
	  return true;
        }
      }
      if (check_unrestricted_recursive(unit->first_select(), 
					     unrestricted, 
				             encountered))
        return true;
      if (!(tbl->is_recursive_with_table() && unit->with_element->owner == owner))
        continue;
      With_element *with_elem= unit->with_element;
      if (encountered & with_elem->get_elem_map())
        unrestricted|= with_elem->mutually_recursive;
      else
        encountered|= with_elem->get_elem_map();
    }
  }
  for (With_element *with_elem= sel->get_with_element()->owner->first_elem;
       with_elem != NULL;
       with_elem= with_elem->next_elem)
  {
    if (!with_elem->is_recursive && (unrestricted & with_elem->get_elem_map()))
      continue;
    if (encountered & with_elem->get_elem_map())
    {
      uint cnt= 0;
      table_map mutually_recursive= with_elem->mutually_recursive;
      for (table_map map= mutually_recursive >> with_elem->number;
           map != 0;
           map>>= 1) 
      {
        if (map & 1)
        { 
          if (cnt)
          { 
            unrestricted|= with_elem->mutually_recursive;
            break;
          }
          else
            cnt++;
	}
      }
    }
  }
  ti.rewind();
  while ((tbl= ti++))
  {
    for (TABLE_LIST *tab= tbl; tab; tab= tab->embedding)
    {
      if (tab->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT))
      {
        unrestricted|= mutually_recursive;
	break;
      }
    }
  }
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
  str->append(STRING_WITH_LEN("with "));
  if (with_recursive)
    str->append(STRING_WITH_LEN("recursive "));
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
  str->append(STRING_WITH_LEN(" as "));
  str->append('(');
  spec->print(str, query_type);
  str->append(')');
}


