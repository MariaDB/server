#include "sql_class.h"
#include "sql_lex.h"
#include "sql_cte.h"
#include "sql_view.h"    // for make_valid_column_names
#include "sql_parse.h"
#include "sql_select.h"


/**
  @brief
    Add a new element to this with clause 

  @param elem  The with element to add to this with clause

  @details
    The method adds the with element 'elem' to the elements
    in this with clause. The method reports an error if
    the number of the added element exceeds the value
    of the constant max_number_of_elements_in_with_clause.

  @retval
    true    if an error is reported 
    false   otherwise
*/
  
bool With_clause::add_with_element(With_element *elem)
{ 
  if (with_list.elements == max_number_of_elements_in_with_clause)
  {
    my_error(ER_TOO_MANY_DEFINITIONS_IN_WITH_CLAUSE, MYF(0));
    return true;
  }
  elem->owner= this;
  elem->number= with_list.elements;
  elem->spec->with_element= elem;
  with_list.link_in_list(elem, &elem->next);
  return false;
}


/**
  @brief
    Check dependencies between tables defined in a list of with clauses
 
  @param 
    with_clauses_list  Pointer to the first clause in the list

  @details
    For each with clause from the given list the procedure finds all
    dependencies between tables defined in the clause by calling the
    method With_clause::checked_dependencies.
    Additionally, based on the info collected by this method the procedure
    finds anchors for each recursive definition and moves them at the head
    of the definition.

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
    The method performs the following for this with clause:
    - checks that there are no definitions of the tables with the same name
    - for each table T defined in this with clause looks for the tables
      from the same with clause that are used in the query that specifies T
      and set the dependencies of T on these tables in a bitmap. 
    - builds the transitive closure of the above direct dependencies
      to find out all recursive definitions.

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
    For each table T defined in this with clause look for all other tables
    from the same with clause that are used in the specification of T.
    For each such table set the dependency bit in the dependency map of
    the with element for T.
  */
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    for (With_element *elem= with_list.first;
         elem != with_elem;
         elem= elem->next)
    {
      if (my_strcasecmp(system_charset_info, with_elem->query_name->str,
                        elem->query_name->str) == 0)
      {
	my_error(ER_DUP_QUERY_NAME, MYF(0), with_elem->query_name->str);
	return true;
      }
    }
    if (with_elem->check_dependencies_in_spec())
      return true;
  }
  /* Build the transitive closure of the direct dependencies found above */
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
    with_elem->derived_dep_map= with_elem->base_dep_map;
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    table_map with_elem_map=  with_elem->get_elem_map();
    for (With_element *elem= with_list.first; elem; elem= elem->next)
    {
      if (elem->derived_dep_map & with_elem_map)
        elem->derived_dep_map |= with_elem->derived_dep_map;
    }   
  }

  /*
    Mark those elements where tables are defined with direct or indirect
   make recursion.
  */ 
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    if (with_elem->derived_dep_map & with_elem->get_elem_map())
      with_elem->is_recursive= true;
  }   
	
  dependencies_are_checked= true;
  return false;
}


/*
  This structure describes an element of the stack of embedded units.
  The stack is used when looking for a definition of a table in
  with clauses. The definition can be found only in the scopes
  of the with clauses attached to the units from the stack.
  The with clauses are looked through from starting from the top
  element of the stack. 
*/

struct st_unit_ctxt_elem
{
  st_unit_ctxt_elem *prev;   // the previous element of the stack
  st_select_lex_unit *unit;   
};


/**
  @brief
    Find the dependencies of this element on its siblings in its specification

  @details
    For each table reference ref(T) from the FROM list of every select sl 
    immediately contained in the specification query of this element this
    method searches for the definition of T in the the with clause which
    this element belongs to. If such definition is found then the dependency
    on it is set in sl->with_dep and in this->base_dep_map.  
*/

bool With_element::check_dependencies_in_spec()
{ 
  for (st_select_lex *sl=  spec->first_select(); sl; sl= sl->next_select())
  {
    st_unit_ctxt_elem ctxt0= {NULL, owner->owner};
    st_unit_ctxt_elem ctxt1= {&ctxt0, spec};
    check_dependencies_in_select(sl, &ctxt1, false, &sl->with_dep);
    base_dep_map|= sl->with_dep;
  }
  return false;
}


/**
  @brief
    Search for the definition of a table among the elements of this with clause
 
  @param table    The reference to the table that is looked for
  @param barrier  The barrier with element for the search

  @details
    The function looks through the elements of this with clause trying to find
    the definition of the given table. When it encounters the element with
    the same query name as the table's name it returns this element. If no
    such definitions are found the function returns NULL.

  @retval
    found with element if the search succeeded
    NULL - otherwise
*/    

With_element *With_clause::find_table_def(TABLE_LIST *table,
                                          With_element *barrier)
{
  for (With_element *with_elem= with_list.first; 
       with_elem != barrier;
       with_elem= with_elem->next)
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
    Search for the definition of a table in with clauses
 
  @param tbl     The reference to the table that is looked for
  @param ctxt    The context describing in what with clauses of the upper
                 levels the table has to be searched for.

  @details
    The function looks for the definition of the table tbl in the definitions
    of the with clauses from the upper levels specified by the parameter ctxt.
    When it encounters the element with the same query name as the table's name
    it returns this element. If no such definitions are found the function
    returns NULL.

  @retval
    found with element if the search succeeded
    NULL - otherwise
*/    

With_element *find_table_def_in_with_clauses(TABLE_LIST *tbl,
                                             st_unit_ctxt_elem *ctxt)
{
  With_element *barrier= NULL;
  for (st_unit_ctxt_elem *unit_ctxt_elem= ctxt;
       unit_ctxt_elem;
       unit_ctxt_elem= unit_ctxt_elem->prev)
  {
    st_select_lex_unit *unit= unit_ctxt_elem->unit;
    With_clause *with_clause= unit->with_clause;
    if (with_clause &&
	(tbl->with= with_clause->find_table_def(tbl, barrier)))
      return tbl->with;
    barrier= NULL;
    if (unit->with_element && !unit->with_element->get_owner()->with_recursive)
    {
      /* 
        This unit is the specification if the with element unit->with_element.
        The with element belongs to a with clause without the specifier RECURSIVE.
        So when searching for the matching definition of tbl this with clause must
        be looked up to this with element
      */
      barrier= unit->with_element;
    }
  }
  return NULL;
}


/**
  @brief
    Find the dependencies of this element on its siblings in a select
 
  @param  sl       The select where to look for the dependencies
  @param  ctxt     The structure specifying the scope of the definitions
                   of the with elements of the upper levels
  @param  in_sbq   if true mark dependencies found in subqueries in 
                   this->sq_dep_map
  @param  dep_map  IN/OUT The bit where to mark the found dependencies

  @details
    For each table reference ref(T) from the FROM list of the select sl
    the method searches in with clauses for the definition of the table T.
    If the found definition belongs to the same with clause as this with
    element then the method set dependency on T in the in/out parameter
    dep_map, add if required - in this->sq_dep_map.
    The parameter ctxt describes the proper context for the search 
    of the definition of T.      
*/

void With_element::check_dependencies_in_select(st_select_lex *sl,
                                                st_unit_ctxt_elem *ctxt,
                                                bool in_subq,
                                                table_map *dep_map)
{
  With_clause *with_clause= sl->get_with_clause();
  for (TABLE_LIST *tbl= sl->table_list.first; tbl; tbl= tbl->next_local)
  {
    if (tbl->derived || tbl->nested_join)
      continue;
    tbl->with_internal_reference_map= 0;
    /*
      If there is a with clause attached to the unit containing sl
      look first for the definition of tbl in this with clause.
      If such definition is not found there look in the with
      clauses of the upper levels.
      If the definition of tbl is found somewhere in with clauses
       then tbl->with is set to point to this definition 
    */
    if (with_clause && !tbl->with)
      tbl->with= with_clause->find_table_def(tbl, NULL);
    if (!tbl->with)
      tbl->with= find_table_def_in_with_clauses(tbl, ctxt);

    if (tbl->with && tbl->with->owner== this->owner)
    {   
      /* 
        The found definition T of tbl belongs to the same
        with clause as this with element. In this case:
        - set the dependence on T in the bitmap dep_map
        - set tbl->with_internal_reference_map with
          the bitmap for this definition
        - set the dependence on T in the bitmap this->sq_dep_map
          if needed 
      */     
      *dep_map|= tbl->with->get_elem_map();
      tbl->with_internal_reference_map= get_elem_map();
      if (in_subq)
        sq_dep_map|= tbl->with->get_elem_map();
    }
  }
  /* Now look for the dependencies in the subqueries of sl */
  st_select_lex_unit *inner_unit= sl->first_inner_unit();
  for (; inner_unit; inner_unit= inner_unit->next_unit())
    check_dependencies_in_unit(inner_unit, ctxt, in_subq, dep_map);
}


/**
  @brief
    Find the dependencies of this element on its siblings in a unit
 
  @param  unit     The unit where to look for the dependencies
  @param  ctxt     The structure specifying the scope of the definitions
                   of the with elements of the upper levels
  @param  in_sbq   if true mark dependencies found in subqueries in 
                   this->sq_dep_map
  @param  dep_map  IN/OUT The bit where to mark the found dependencies

  @details
    This method searches in the unit 'unit' for the the references in FROM
    lists of all selects contained in this unit and in the with clause
    attached to this unit that refer to definitions of tables from the
    same with clause as this element.
    If such definitions are found then the dependencies on them are
    set in the in/out parameter dep_map and optionally in this->sq_dep_map.  
    The parameter ctxt describes the proper context for the search.      
*/

void With_element::check_dependencies_in_unit(st_select_lex_unit *unit,
                                              st_unit_ctxt_elem *ctxt,
                                              bool in_subq,
                                              table_map *dep_map)
{
  if (unit->with_clause)
    check_dependencies_in_with_clause(unit->with_clause, ctxt, in_subq, dep_map);
  in_subq |= unit->item != NULL;
  st_unit_ctxt_elem unit_ctxt_elem= {ctxt, unit};
  st_select_lex *sl= unit->first_select();
  for (; sl; sl= sl->next_select())
  {
    check_dependencies_in_select(sl, &unit_ctxt_elem, in_subq, dep_map);
  }
}


/**
  @brief
    Find the dependencies of this element on its siblings in a with clause
 
  @param  witt_clause  The with clause where to look for the dependencies
  @param  ctxt         The structure specifying the scope of the definitions
                       of the with elements of the upper levels
  @param  in_sbq       if true mark dependencies found in subqueries in 
                       this->sq_dep_map
  @param  dep_map      IN/OUT The bit where to mark the found dependencies

  @details
    This method searches in the with_clause for the the references in FROM
    lists of all selects contained in the specifications of the with elements
    from this with_clause that refer to definitions of tables from the
    same with clause as this element.
    If such definitions are found then the dependencies on them are
    set in the in/out parameter dep_map and optionally in this->sq_dep_map.  
    The parameter ctxt describes the proper context for the search.      
*/

void 
With_element::check_dependencies_in_with_clause(With_clause *with_clause,
                                                st_unit_ctxt_elem *ctxt,
                                                bool in_subq,
                                                table_map *dep_map)
{
  for (With_element *with_elem= with_clause->with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    check_dependencies_in_unit(with_elem->spec, ctxt, in_subq, dep_map);
  }
}


/**
  @brief
    Find mutually recursive with elements and check that they have ancors
 
  @details
    This method performs the following:
    - for each recursive with element finds all mutually recursive with it
    - links each group of mutually recursive with elements into a ring chain
    - checks that every group of mutually recursive with elements contains
      at least one anchor
    - checks that after removing any with element with anchor the remaining
      with elements mutually recursive with the removed one are not recursive
      anymore

  @retval
    true    if an error is reported 
    false   otherwise
*/

bool With_clause::check_anchors()
{
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    if (!with_elem->is_recursive)
      continue;

  /* 
    It with_elem is recursive with element find all elements mutually recursive
    with it (any recursive element is mutually recursive with itself). Mark all
    these elements in the bitmap->mutually_recursive. Also link all these 
    elements into a ring chain.
  */
    if (!with_elem->next_mutually_recursive)
    {
      With_element *last_mutually_recursive= with_elem;
      table_map with_elem_dep= with_elem->derived_dep_map;
      table_map with_elem_map= with_elem->get_elem_map();
      for (With_element *elem= with_elem; elem; elem= elem->next)
      {
        if (!elem->is_recursive)
          continue;

        if (elem == with_elem ||
	    ((elem->derived_dep_map & with_elem_map) &&
	     (with_elem_dep & elem->get_elem_map())))
	{        
          elem->next_mutually_recursive= with_elem;
          last_mutually_recursive->next_mutually_recursive= elem;
          last_mutually_recursive= elem;
	  with_elem->mutually_recursive|= elem->get_elem_map();
	}
      }
      for (With_element *elem= with_elem->next_mutually_recursive;
           elem != with_elem; 
           elem=  elem->next_mutually_recursive)
	elem->mutually_recursive= with_elem->mutually_recursive;
    }
 
    /*
      For each select from the specification of 'with_elem' check whether
      it is an anchor i.e. does not depend on any with elements mutually
      recursive with 'with_elem".
    */
    for (st_select_lex *sl= with_elem->spec->first_select();
         sl;
         sl= sl->next_select())
    {
      if (with_elem->is_anchor(sl))
      {
        with_elem->with_anchor= true;
        break;
      }
    }
  }

  /* 
    Check that for any group of mutually recursive with elements
    - there is at least one anchor
    - after removing any with element with anchor the remaining with elements
      mutually recursive with the removed one are not recursive anymore
  */ 
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    if (!with_elem->is_recursive)
      continue;
    
    if (!with_elem->with_anchor)
    {
      /* 
        Check that the other with elements mutually recursive with 'with_elem' 
        contain at least one anchor.
      */
      With_element *elem= with_elem;
      while ((elem= elem->get_next_mutually_recursive()) != with_elem)
      {
        if (elem->with_anchor)
          break;
      }
      if (elem == with_elem)
      {
        my_error(ER_RECURSIVE_WITHOUT_ANCHORS, MYF(0),
        with_elem->query_name->str);
        return true;
      }
    }
    else
    {
      /* 'with_elem' is a with element with an anchor */
      With_element *elem= with_elem;
      /* 
        For the other with elements mutually recursive with 'with_elem'
        set dependency bits between those elements in the field work_dep_map
        and build transitive closure of these dependencies
      */         
      while ((elem= elem->get_next_mutually_recursive()) != with_elem)
	elem->work_dep_map= elem->base_dep_map & elem->mutually_recursive;
      elem= with_elem;
      while ((elem= elem->get_next_mutually_recursive()) != with_elem)
      {
        table_map elem_map= elem->get_elem_map();
        With_element *el= with_elem;
        while ((el= el->get_next_mutually_recursive()) != with_elem)
	{
          if (el->work_dep_map & elem_map)
	    el->work_dep_map|= elem->work_dep_map;          
        }
      }
      /* If the transitive closure displays any cycle report an arror */
      elem= with_elem;
      while ((elem= elem->get_next_mutually_recursive()) != with_elem)
      {
        if (elem->work_dep_map & elem->get_elem_map())
	{
          my_error(ER_UNACCEPTABLE_MUTUAL_RECURSION, MYF(0),
          with_elem->query_name->str);
          return true;
	}
      }
    }
  }

  return false;
}


/**
  @brief
    Move anchors at the beginning of the specifications for with elements
 
  @details
    This method moves anchors at the beginning of the specifications for
    all recursive with elements.
*/

void With_clause::move_anchors_ahead()
{
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    if (with_elem->is_recursive)
     with_elem->move_anchors_ahead();
  }
}
	

/**
  @brief
    Move anchors at the beginning of the specification of this with element
 
  @details
    If the specification of this with element contains anchors the method
    moves them at the very beginning of the specification.
*/

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
  for (With_element *with_elem= with_list.first; 
       with_elem;
       with_elem= with_elem->next)
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
    The method creates for a string copy of the specification used in this
    element. The method is called when the element is parsed. The copy may be
    used to create clones of the specification whenever they are needed.

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


bool With_element::is_anchor(st_select_lex *sel)
{
  return !(mutually_recursive & sel->with_dep);
}   


/**
   @brief
     Search for the definition of the given table referred in this select node

  @param table  reference to the table whose definition is searched for
     
  @details  
    The method looks for the definition of the table whose reference is occurred
    in the FROM list of this select node. First it searches for it in the
    with clause attached to the unit this select node belongs to. If such a
    definition is not found then the embedding units are looked through.

  @retval
    pointer to the found definition if the search has been successful
    NULL -  otherwise
*/    

With_element *st_select_lex::find_table_def_in_with_clauses(TABLE_LIST *table)
{
  st_select_lex_unit *master_unit= NULL;
  With_element *found= NULL;
  for (st_select_lex *sl= this;
       sl;
       sl= master_unit->outer_select())
  {
    With_element *with_elem= sl->get_with_element();
    /* 
      If sl->master_unit() is the spec of a with element then the search for 
      a definition was already done by With_element::check_dependencies_in_spec
      and it was unsuccesful. Yet for units cloned from the spec it has not 
      been done yet.
    */
    if (with_elem && sl->master_unit() == with_elem->spec)
      break;      
    With_clause *with_clause=sl->get_with_clause();
    if (with_clause)
    {
      With_element *barrier= with_clause->with_recursive ? NULL : with_elem;
      if ((found= with_clause->find_table_def(table, barrier)))
        break;
    }
    master_unit= sl->master_unit();
    /* Do not look for the table's definition beyond the scope of the view */
    if (master_unit->is_view)
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


/*
  A reference to a with table T is recursive if it occurs somewhere
  in the query specifying T or in the query specifying one of the tables
  mutually recursive with T.
*/

bool TABLE_LIST::is_with_table_recursive_reference()
{
  return (with_internal_reference_map &&
          (with->get_mutually_recursive() & with_internal_reference_map));
}


/* 
  Specifications of with tables with recursive table references
  in non-mergeable derived tables are not allowed in this
  implementation. 
*/


/*
  We say that the specification of a with table T is restricted  
  if all below is true.
    1. Any immediate select of the specification contains at most one 
     recursive table reference taking into account table references
     from mergeable derived tables.
    2. Any recursive table reference is not an inner operand of an
     outer join operation used in an immediate select of the
     specification.
    3. Any immediate select from the specification of T does not
     contain aggregate functions.
    4. The specification of T does not contain recursive table references.

  If the specification of T is not restricted we call the corresponding
  with element unrestricted.

  The SQL standards allows only with elements with restricted specification.
  By default we comply with the standards here.  
 
  Yet we allow unrestricted specification if the status variable
  'standards_compliant_cte' set to 'off'(0).
*/


/**
  @brief
    Check if this select makes the including specification unrestricted
 
  @param
    only_standards_compliant  true if  the system variable
                              'standards_compliant_cte' is set to 'on'
  @details
    This method checks whether the conditions 1-4 (see the comment above)
    are satisfied for this select. If not then mark this element as
    unrestricted and report an error if 'only_standards_compliant' is true.

  @retval
    true    if an error is reported 
    false   otherwise
*/

bool st_select_lex::check_unrestricted_recursive(bool only_standard_compliant)
{
  With_element *with_elem= get_with_element();
  if (!with_elem ||!with_elem->is_recursive)
  {
    /*
      If this select is not from the specifiocation of a with elememt or
      if this not a recursive with element then there is nothing to check.
    */
    return false;
  }

  /* Check conditions 1-2 for restricted specification*/
  table_map unrestricted= 0;
  table_map encountered= 0;
  if (with_elem->check_unrestricted_recursive(this, 
					      unrestricted, 
					      encountered))
    return true;
  with_elem->get_owner()->add_unrestricted(unrestricted);


  /* Check conditions 3-4 for restricted specification*/
  if (with_sum_func ||
      (with_elem->contains_sq_with_recursive_reference()))
    with_elem->get_owner()->add_unrestricted(
                              with_elem->get_mutually_recursive());

  /* Report an error on unrestricted specification if this is required */
  if (only_standard_compliant && with_elem->is_unrestricted())
  {
    my_error(ER_NOT_STANDARD_COMPLIANT_RECURSIVE,
	     MYF(0), with_elem->query_name->str);
    return true;
  }

  return false;
}


/**
  @brief
    Check if a select from the spec of this with element is partially restricted
 
  @param
    sel             select from the specification of this element where to check
                    whether conditions 1-2 are satisfied
    unrestricted    IN/OUT bitmap where to mark unrestricted specs
    encountered     IN/OUT bitmap where to mark encountered recursive references
  @details
    This method checks whether the conditions 1-2 (see the comment above)
    are satisfied for the select sel. 
    This method is called recursively for derived tables.

  @retval
    true    if an error is reported 
    false   otherwise
*/

bool With_element::check_unrestricted_recursive(st_select_lex *sel, 
					        table_map &unrestricted, 
						table_map &encountered)
{
  /* Check conditions 1 for restricted specification*/
  List_iterator<TABLE_LIST> ti(sel->leaf_tables);
  TABLE_LIST *tbl;
  while ((tbl= ti++))
  {
    st_select_lex_unit *unit= tbl->get_unit();
    if (unit)
    { 
      if(!tbl->is_with_table())
      {
        if (check_unrestricted_recursive(unit->first_select(), 
					 unrestricted, 
				         encountered))
          return true;
      }
      if (!(tbl->is_recursive_with_table() && unit->with_element->owner == owner))
        continue;
      With_element *with_elem= unit->with_element;
      if (encountered & with_elem->get_elem_map())
        unrestricted|= with_elem->mutually_recursive;
      else
        encountered|= with_elem->get_elem_map();
    }
  } 
  for (With_element *with_elem= owner->with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    if (!with_elem->is_recursive && (unrestricted & with_elem->get_elem_map()))
      continue;
    if (encountered & with_elem->get_elem_map())
    {
      uint cnt= 0;
      table_map encountered_mr= encountered & with_elem->mutually_recursive;
      for (table_map map= encountered_mr >> with_elem->number;
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

  
  /* Check conditions 2 for restricted specification*/
  ti.rewind();
  while ((tbl= ti++))
  {
    if (!tbl->is_with_table_recursive_reference())
      continue;
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
  Check subqueries with recursive table references from FROM list of this select
 
 @details
    For each recursive table reference from the FROM list of this select 
    this method checks:
    - whether this reference is within a materialized derived table and
      if so it report an error
    - whether this reference is within a subquery and if so it set a flag
      in this subquery that disallows some optimization strategies for
      this subquery.
 
  @retval
    true    if an error is reported 
    false   otherwise
*/

bool st_select_lex::check_subqueries_with_recursive_references()
{
  st_select_lex_unit *sl_master= master_unit();
  List_iterator<TABLE_LIST> ti(leaf_tables);
  TABLE_LIST *tbl;
  while ((tbl= ti++))
  {
    if (!(tbl->is_with_table_recursive_reference() && sl_master->item))
      continue;
    With_element *with_elem= tbl->with;
    bool check_embedding_materialized_derived= true;
    for (st_select_lex *sl= this; sl; sl= sl_master->outer_select())
    { 
      sl_master= sl->master_unit();
      if (with_elem->get_owner() == sl_master->with_clause)
         check_embedding_materialized_derived= false;
      if (check_embedding_materialized_derived && !sl_master->with_element && 
          sl_master->derived && sl_master->derived->is_materialized_derived())
      {
	my_error(ER_REF_TO_RECURSIVE_WITH_TABLE_IN_DERIVED,
	  	     MYF(0), with_elem->query_name->str);
	return true;
      }
      if (!sl_master->item)
	continue;
      Item_subselect *subq= (Item_subselect *) sl_master->item;
      subq->with_recursive_reference= true;
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
  /*
    Any with clause contains just definitions of CTE tables.
    No data expansion is applied to these definitions.
  */
  query_type= (enum_query_type) (query_type | QT_NO_DATA_EXPANSION);

  str->append(STRING_WITH_LEN("with "));
  if (with_recursive)
    str->append(STRING_WITH_LEN("recursive "));
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    if (with_elem != with_list.first)
      str->append(", ");
    with_elem->print(str, query_type);
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


bool With_element::instantiate_tmp_tables()
{
  List_iterator_fast<TABLE> li(rec_result->rec_tables);
  TABLE *rec_table;
  while ((rec_table= li++))
  {
    if (!rec_table->is_created() &&
        instantiate_tmp_table(rec_table,
                              rec_result->tmp_table_param.keyinfo,
                              rec_result->tmp_table_param.start_recinfo,
                              &rec_result->tmp_table_param.recinfo,
                              0))
      return true;

    rec_table->file->extra(HA_EXTRA_WRITE_CACHE);
    rec_table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  }
  return false;
}

