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

bool LEX::check_dependencies_in_with_clauses()
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
    Resolve references to CTE in specification of hanging CTE

  @details
    A CTE to which there are no references in the query is called hanging CTE.
    Although such CTE is not used for execution its specification must be
    subject to context analysis. All errors concerning references to
    non-existing tables or fields occurred in the specification must be
    reported as well as all other errors caught at the prepare stage.
    The specification of a hanging CTE might contain references to other
    CTE outside of the specification and within it if the specification
    contains a with clause. This function resolves all such references for
    all hanging CTEs encountered in the processed query.

  @retval
    false   on success
    true    on failure
*/

bool
LEX::resolve_references_to_cte_in_hanging_cte()
{
  for (With_clause *with_clause= with_clauses_list;
       with_clause; with_clause= with_clause->next_with_clause)
  {
    for (With_element *with_elem= with_clause->with_list.first;
         with_elem; with_elem= with_elem->next)
    {
      if (!with_elem->is_referenced())
      {
        TABLE_LIST *first_tbl=
                     with_elem->spec->first_select()->table_list.first;
        TABLE_LIST **with_elem_end_pos= with_elem->head->tables_pos.end_pos;
        if (first_tbl && resolve_references_to_cte(first_tbl, with_elem_end_pos))
          return true;
      }
    }
  }
  return false;
}


/**
  @brief
    Resolve table references to CTE from a sub-chain of table references

  @param tables      Points to the beginning of the sub-chain
  @param tables_last Points to the address with the sub-chain barrier

  @details
    The method resolves tables references to CTE from the chain of
    table references specified by the parameters 'tables' and 'tables_last'.
    It resolves the references against the CTE definition occurred in a query
    or the specification of a CTE whose parsing tree is represented by
    this LEX structure. The method is always called right after the process
    of parsing the query or of the specification of a CTE has been finished,
    thus the chain of table references used in the parsed fragment has been
    already built. It is assumed that parameters of the method specify a
    a sub-chain of this chain.
    If a table reference can be potentially a table reference to a CTE and it
    has not been resolved yet then the method tries to find the definition
    of the CTE against which the reference can be resolved. If it succeeds
    it sets the field TABLE_LIST::with to point to the found definition.
    It also sets the field TABLE_LIST::derived to point to the specification
    of the found CTE and sets TABLE::db.str to empty_c_string. This will
    allow to handle this table reference like a reference to a derived handle.
    If another table reference has been already resolved against this CTE
    and this CTE is not recursive then a clone of the CTE specification is
    constructed using the function With_element::clone_parsed_spec() and
    TABLE_LIST::derived is set to point to this clone rather than to the
    original specification.
    If the method does not find a matched CTE definition in the parsed fragment
    then in the case when the flag this->only_cte_resolution is set to true
    it just moves to the resolution of the next table reference from the
    specified sub-chain while in the case when this->only_cte_resolution is set
    to false the method additionally sets an mdl request for this table
    reference.

  @notes
    The flag this->only_cte_resolution is set to true in the cases when
    the failure to resolve a table reference as a CTE reference within
    the fragment associated with this LEX structure does not imply that
    this table reference cannot be resolved as such at all.

  @retval false  On success: no errors reported, no memory allocations failed
  @retval true   Otherwise
*/

bool LEX::resolve_references_to_cte(TABLE_LIST *tables,
                                    TABLE_LIST **tables_last)
{
  With_element *with_elem= 0;

  for (TABLE_LIST *tbl= tables; tbl != *tables_last; tbl= tbl->next_global)
  {
    if (tbl->derived)
      continue;
    if (!tbl->db && !tbl->with)
      tbl->with= tbl->select_lex->find_table_def_in_with_clauses(tbl);
    if (!tbl->with)    // no CTE matches table reference tbl
    {
      if (only_cte_resolution)
        continue;
      if (!tbl->db)   // no database specified in table reference tbl
      {
        if (!thd->db) // no default database is set
        {
          my_message(ER_NO_DB_ERROR, ER(ER_NO_DB_ERROR), MYF(0));
          return true;
        }
        if (copy_db_to(&tbl->db, &tbl->db_length))
          return true;
        if (!(tbl->table_options & TL_OPTION_ALIAS))
          tbl->mdl_request.init(MDL_key::TABLE, tbl->db,
                                tbl->table_name,
                                tbl->mdl_type, MDL_TRANSACTION);
        tbl->mdl_request.set_type((tbl->lock_type >= TL_WRITE_ALLOW_WRITE) ?
                                   MDL_SHARED_WRITE : MDL_SHARED_READ);
      }
      continue;
    }
    with_elem= tbl->with;
    if (tbl->is_recursive_with_table() &&
        !tbl->is_with_table_recursive_reference())
    {
      tbl->with->rec_outer_references++;
      while ((with_elem= with_elem->get_next_mutually_recursive()) !=
             tbl->with)
	with_elem->rec_outer_references++;
    }
    if (!with_elem->is_used_in_query || with_elem->is_recursive)
    {
      tbl->derived= with_elem->spec;
      if (tbl->derived != tbl->select_lex->master_unit() &&
          !with_elem->is_recursive &&
          !tbl->is_with_table_recursive_reference())
      {
        tbl->derived->move_as_slave(tbl->select_lex);
      }
      with_elem->is_used_in_query= true;
    }
    else
    {
      if (!(tbl->derived= tbl->with->clone_parsed_spec(thd->lex, tbl)))
        return true;
    }
    tbl->db= empty_c_string;
    tbl->db_length= 0;
    tbl->schema_table= 0;
    if (tbl->derived)
    {
      tbl->derived->first_select()->linkage= DERIVED_TABLE_TYPE;
    }
    if (tbl->with->is_recursive && tbl->is_with_table_recursive_reference())
      continue;
    with_elem->inc_references();
  }
  return false;
}


/**
  @brief
    Find out dependencies between CTEs, resolve references to them

  @details
    The function can be called in two modes. With this->with_cte_resolution
    set to false the function only finds out all dependencies between CTEs
    used in a query expression with a WITH clause whose parsing has been
    just finished. Based on these dependencies recursive CTEs are detected.
    If this->with_cte_resolution is set to true the function additionally
    resolves all references to CTE occurred in this query expression.

  @retval
    true   on failure
    false  on success
*/

bool
LEX::check_cte_dependencies_and_resolve_references()
{
  if (check_dependencies_in_with_clauses())
    return true;
  if (!with_cte_resolution)
    return false;
  if (resolve_references_to_cte(query_tables, query_tables_last))
    return true;
  if (resolve_references_to_cte_in_hanging_cte())
    return true;
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
      if (my_strcasecmp(system_charset_info, with_elem->get_name_str(),
			  elem->get_name_str()) == 0)
      {
        my_error(ER_DUP_QUERY_NAME, MYF(0),
                 with_elem->get_name_str());
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
    recursion.
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
    if (my_strcasecmp(system_charset_info, with_elem->get_name_str(),
                      table->table_name) == 0 &&
        !table->is_fqtn)
    {
      table->set_derived();
      with_elem->referenced= true;
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
      else
        top_level_dep_map|= tbl->with->get_elem_map();
    }
  }
  /* Now look for the dependencies in the subqueries of sl */
  st_select_lex_unit *inner_unit= sl->first_inner_unit();
  for (; inner_unit; inner_unit= inner_unit->next_unit())
  {
    if (!inner_unit->with_element)
      check_dependencies_in_unit(inner_unit, ctxt, in_subq, dep_map);
  }
}


/**
  @brief
    Find a recursive reference to this with element in subqueries of a select
 
  @param  sel      The select in whose subqueries the reference
                   to be looked for

  @details
    The function looks for a recursive reference to this with element in
    subqueries of select sl. When the first such reference is found
    it is returned as the result.
    The function assumes that the identification of all CTE references
    has been performed earlier.

  @retval
    Pointer to the found recursive reference if the search succeeded
    NULL - otherwise 
*/

TABLE_LIST *With_element::find_first_sq_rec_ref_in_select(st_select_lex *sel)
{
  TABLE_LIST *rec_ref= NULL;
  st_select_lex_unit *inner_unit= sel->first_inner_unit();
  for (; inner_unit; inner_unit= inner_unit->next_unit())
  {
    st_select_lex *sl= inner_unit->first_select();
    for (; sl; sl= sl->next_select())
    {
      for (TABLE_LIST *tbl= sl->table_list.first; tbl; tbl= tbl->next_local)
      {
        if (tbl->derived || tbl->nested_join)
          continue;
        if (tbl->with && tbl->with->owner== this->owner &&
            (tbl->with_internal_reference_map & mutually_recursive))
	{
	  rec_ref= tbl;
          return rec_ref;
        }
      }
      if ((rec_ref= find_first_sq_rec_ref_in_select(sl)))
        return rec_ref;
    } 
  }
  return 0;
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
        with_elem->get_name_str());
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
          with_elem->get_name_str());
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
    Additionally for the other selects of the specification if none of them
    contains a recursive reference to this with element or a mutually recursive
    one the method looks for the first such reference in the first recursive 
    select and set a pointer to it in this->sq_rec_ref.   
*/

void With_element::move_anchors_ahead()
{
  st_select_lex *next_sl;
  st_select_lex *new_pos= spec->first_select();
  new_pos->linkage= UNION_TYPE;
  for (st_select_lex *sl= new_pos; sl; sl= next_sl)
  {
    next_sl= sl->next_select(); 
    if (is_anchor(sl))
    {
      sl->move_node(new_pos);
      if (new_pos == spec->first_select())
      {
        enum sub_select_type type= new_pos->linkage;
        new_pos->linkage= sl->linkage;
        sl->linkage= type;
        new_pos->with_all_modifier= sl->with_all_modifier;
        sl->with_all_modifier= false;
      }
      new_pos= sl->next_select();
    }
    else if (!sq_rec_ref && no_rec_ref_on_top_level())
    {
      sq_rec_ref= find_first_sq_rec_ref_in_select(sl);
      DBUG_ASSERT(sq_rec_ref != NULL);
    }
  }
  first_recursive= new_pos;
  spec->first_select()->linkage= DERIVED_TABLE_TYPE;
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
    if ((with_elem->is_hanging_recursive() || !with_elem->is_referenced()) &&
        with_elem->prepare_unreferenced(thd))
      return true;
  }

  return false;
}


/**
  @brief
    Save the specification of the given with table as a string

  @param thd         The context of the statement containing this with element
  @param spec_start  The beginning of the specification in the input string
  @param spec_end    The end of the specification in the input string
  @param spec_offset The offset of the specification in the input string

  @details
    The method creates for a string copy of the specification used in this
    element. The method is called when the element is parsed. The copy may be
    used to create clones of the specification whenever they are needed.

  @retval
    false   on success
    true    on failure
*/
  
bool With_element::set_unparsed_spec(THD *thd, char *spec_start, char *spec_end,
                                     uint spec_offset)
{
  stmt_prepare_mode= thd->m_parser_state->m_lip.stmt_prepare_mode;
  unparsed_spec.length= spec_end - spec_start;
  if (stmt_prepare_mode || !thd->lex->sphead)
    unparsed_spec.str= spec_start;
  else
  {
    unparsed_spec.str= (char*) thd->memdup(spec_start, unparsed_spec.length+1);
    unparsed_spec.str[unparsed_spec.length]= '\0';
  }
  unparsed_spec_offset= spec_offset;

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
    
  @param old_lex    The LEX structure created for the query or CTE specification
                    where this With_element is defined
  @param with_table The reference to the table defined in this element for which
                     the clone is created.

  @details
    The method creates a clone of the specification used in this element.
    The clone is created for the given reference to the table defined by
    this element.
    The clone is created when the string with the specification saved in
    unparsed_spec is fed into the parser as an input string. The parsing
    this string a unit object representing the specification is built.
    A chain of all table references occurred in the specification is also
    formed.
    The method includes the new unit and its sub-unit into hierarchy of
    the units of the main query. I also insert the constructed chain of the 
    table references into the chain of all table references of the main query.
    The method resolves all references to CTE in the clone.

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

st_select_lex_unit *With_element::clone_parsed_spec(LEX *old_lex,
                                                    TABLE_LIST *with_table)
{
  THD *thd= old_lex->thd;
  LEX *lex;
  st_select_lex_unit *res= NULL;

  if (!(lex= (LEX*) new(thd->mem_root) st_lex_local))
    return res;
  thd->lex= lex;

  bool parse_status= false;
  st_select_lex *with_select;
  st_select_lex *last_clone_select;

  char save_end= unparsed_spec.str[unparsed_spec.length];
  unparsed_spec.str[unparsed_spec.length]= '\0';

  lex_start(thd);
  lex->clone_spec_offset= unparsed_spec_offset;
  lex->with_cte_resolution= true;
  /*
    There's no need to add SPs/SFs referenced in the clone to the global
    list of the SPs/SFs used in the query as they were added when the first
    reference to the cloned CTE was parsed. Yet the recursive call of the
    parser must to know that they were already included into the list.
  */
  lex->sroutines= old_lex->sroutines;
  lex->sroutines_list_own_last= old_lex->sroutines_list_own_last;
  lex->sroutines_list_own_elements= old_lex->sroutines_list_own_elements;

  /*
    The specification of a CTE is to be parsed as a regular query.
    At the very end of the parsing query the function
    check_cte_dependencies_and_resolve_references() will be called.
    It will check the dependencies between CTEs that are defined
    within the query and will resolve CTE references in this query.
    If a table reference is not resolved as a CTE reference within
    this query it still can be resolved as a reference to a CTE defined
    in the same clause as the CTE whose specification is to be parsed
    or defined in an embedding CTE definition.

    Example:
      with
      cte1 as ( ... ),
      cte2 as ([WITH ...] select ... from cte1 ...)
      select ... from cte2 as r, ..., cte2 as s ...

    Here the specification of cte2 has be cloned for table reference
    with alias s1. The specification contains a reference to cte1
    that is defined outside this specification. If the reference to
    cte1 cannot be resolved within the specification of cte2 it's
    not necessarily has to be a reference to a non-CTE table. That's
    why the flag lex->only_cte_resolution has to be set to true
    before parsing of the specification of cte2 invoked by this
    function starts. Otherwise an mdl_lock would be requested for s
    and this would not be correct.
  */

  lex->only_cte_resolution= true;

  lex->stmt_lex= old_lex->stmt_lex ? old_lex->stmt_lex : old_lex;

  parse_status= thd->sql_parser(old_lex, lex,
                                (char*) unparsed_spec.str,
                                (unsigned int)unparsed_spec.length,
                                stmt_prepare_mode);

  unparsed_spec.str[unparsed_spec.length]= save_end;
  with_select= lex->unit.first_select();
  with_select->select_number= ++lex->stmt_lex->current_select_number;

  if (parse_status)
    goto err;

  /*
    The unit of the specification that just has been parsed is included
    as a slave of the select that contained in its from list the table
    reference for which the unit has been created.
  */
  lex->unit.include_down(with_table->select_lex);
  lex->unit.set_slave(with_select);
  lex->unit.cloned_from= spec;

  /*
    Now all references to the CTE defined outside of the cloned specification
    has to be resolved. Additionally if old_lex->only_cte_resolution == false
    for the table references that has not been resolved requests for mdl_locks
    has to be set.
  */
  lex->only_cte_resolution= old_lex->only_cte_resolution;
  if (lex->resolve_references_to_cte(lex->query_tables,
                                     lex->query_tables_last))
  {
    res= NULL;
    goto err;
  }

  /*
    The global chain of TABLE_LIST objects created for the specification that
    just has been parsed is added to such chain that contains the reference
    to the CTE whose specification is parsed right after the TABLE_LIST object
    created for the reference.
  */
  if (lex->query_tables)
  {
    head->tables_pos.set_start_pos(&with_table->next_global);
    head->tables_pos.set_end_pos(lex->query_tables_last);
    TABLE_LIST *next_tbl= with_table->next_global;
    if (next_tbl)
    {
      *(lex->query_tables->prev_global= next_tbl->prev_global)=
        lex->query_tables;
      *(next_tbl->prev_global= lex->query_tables_last)= next_tbl;
    }
    else
    {
      *(lex->query_tables->prev_global= old_lex->query_tables_last)=
        lex->query_tables;
      old_lex->query_tables_last= lex->query_tables_last;
    }
  }
  old_lex->sroutines_list_own_last= lex->sroutines_list_own_last;
  old_lex->sroutines_list_own_elements= lex->sroutines_list_own_elements;
  res= &lex->unit;
  res->with_element= this;
  
  last_clone_select= lex->all_selects_list;
  while (last_clone_select->next_select_in_list())
    last_clone_select= last_clone_select->next_select_in_list();
  old_lex->all_selects_list=
    (st_select_lex*) (lex->all_selects_list->
                     insert_chain_before(
                       (st_select_lex_node **) &(old_lex->all_selects_list),
                       last_clone_select));

 lex->sphead= NULL;    // in order not to delete lex->sphead
  lex_end(lex);
err:
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

    Query_arena *arena, backup;
    arena= thd->activate_stmt_arena_if_needed(&backup);

    /* Rename the columns of the first select in the unit */
    while ((item= it++, name= nm++))
    {
      item->set_name(thd, name->str, (uint) name->length, system_charset_info);
      item->is_autogenerated_name= false;
    }

    if (arena)
      thd->restore_active_arena(arena, &backup);
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
  With_element *found= NULL;
  With_clause *containing_with_clause= NULL;
  st_select_lex_unit *master_unit;
  st_select_lex *outer_sl;
  for (st_select_lex *sl= this; sl; sl= outer_sl)
  {
    /* 
      If sl->master_unit() is the spec of a with element then the search for 
      a definition was already done by With_element::check_dependencies_in_spec
      and it was unsuccesful. Yet for units cloned from the spec it has not 
      been done yet.
    */
    With_clause *attached_with_clause= sl->get_with_clause();
    if (attached_with_clause &&
        attached_with_clause != containing_with_clause &&
        (found= attached_with_clause->find_table_def(table, NULL)))
      break;
    master_unit= sl->master_unit();
    outer_sl= master_unit->outer_select();
    With_element *with_elem= sl->get_with_element();
    if (with_elem)
    {
      containing_with_clause= with_elem->get_owner();
      With_element *barrier= containing_with_clause->with_recursive ?
                               NULL : with_elem;
      if ((found= containing_with_clause->find_table_def(table, barrier)))
        break;
      if (outer_sl && !outer_sl->get_with_element())
        break;
    }
    /* Do not look for the table's definition beyond the scope of the view */
    if (master_unit->is_view)
      break; 
  }
  return found;
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
  if ((with_sum_func && !with_elem->is_anchor(this)) ||
      (with_elem->contains_sq_with_recursive_reference()))
    with_elem->get_owner()->add_unrestricted(
                              with_elem->get_mutually_recursive());

  /* Report an error on unrestricted specification if this is required */
  if (only_standard_compliant && with_elem->is_unrestricted())
  {
    my_error(ER_NOT_STANDARD_COMPLIANT_RECURSIVE,
             MYF(0), with_elem->get_name_str());
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
      else if (with_elem ==this)
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
		 MYF(0), with_elem->get_name_str());
	return true;
      }
      if (!sl_master->item)
	continue;
      Item_subselect *subq= (Item_subselect *) sl_master->item;
      subq->with_recursive_reference= true;
      subq->register_as_with_rec_ref(tbl->with);
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
  str->append(get_name());
  if (column_list.elements)
  {
    List_iterator_fast<LEX_STRING> li(column_list);
    str->append('(');
    for (LEX_STRING *col_name= li++; ; )
    {
      str->append(col_name);
      col_name= li++;
      if (!col_name)
      {
        str->append(')');
        break;
      }
      str->append(',');
    }
  }
  str->append(STRING_WITH_LEN(" as "));
  str->append('(');
  spec->print(str, query_type);
  str->append(')');
}


bool With_element::instantiate_tmp_tables()
{
  List_iterator_fast<TABLE_LIST> li(rec_result->rec_table_refs);
  TABLE_LIST *rec_tbl;
  while ((rec_tbl= li++))
  {
    TABLE *rec_table= rec_tbl->table;
    if (!rec_table->is_created() &&
        instantiate_tmp_table(rec_table,
                              rec_table->s->key_info,
                              rec_result->tmp_table_param.start_recinfo,
                              &rec_result->tmp_table_param.recinfo,
                              0))
      return true;

    rec_table->file->extra(HA_EXTRA_WRITE_CACHE);
    rec_table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  }
  return false;
}

