/*
   Copyright (c) 2016, 2017 MariaDB

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

#ifndef SQL_CTE_INCLUDED
#define SQL_CTE_INCLUDED
#include "sql_list.h"
#include "sql_lex.h"
#include "sql_select.h"

class select_unit;
struct st_unit_ctxt_elem;


/**
  @class With_element_head
  @brief Head of the definition of a CTE table

  It contains the name of the CTE and it contains the position of the subchain
  of table references used in the definition in the global chain of table
  references used in the query where this definition is encountered.
*/

class With_element_head : public Sql_alloc
{
  /* The name of the defined CTE */
  LEX_CSTRING *query_name;

public:
  /*
    The structure describing the subchain of the table references used in
    the specification of the defined CTE in the global chain of table
    references used in the query. The structure is fully defined only
    after the CTE definition has been parsed.
  */
  TABLE_CHAIN tables_pos;

  With_element_head(LEX_CSTRING *name)
    : query_name(name)
  {
    tables_pos.set_start_pos(0);
    tables_pos.set_end_pos(0);
  }
  friend class With_element;
};


/**
  @class With_element
  @brief Definition of a CTE table
	
  It contains a reference to the name of the table introduced by this with element,
  and a reference to the unit that specificies this table. Also it contains
  a reference to the with clause to which this element belongs to.	
*/

class With_element : public Sql_alloc
{
private:
  With_clause *owner;      // with clause this object belongs to
  With_element *next;      // next element in the with clause
  uint number;  // number of the element in the with clause (starting from 0)
  table_map elem_map;  // The map where with only one 1 set in this->number   
  /* 
    The map base_dep_map has 1 in the i-th position if the query that
    specifies this with element contains a reference to the with element number i
    in the query FROM list.
    (In this case this with element depends directly on the i-th with element.)  
  */
  table_map base_dep_map; 
  /* 
    The map derived_dep_map has 1 in i-th position if this with element depends
    directly or indirectly from the i-th with element. 
  */
  table_map derived_dep_map;
  /* 
    The map sq_dep_map has 1 in i-th position if there is a reference to this
    with element somewhere in subqueries of the specifications of the tables
    defined in the with clause containing this element;
  */   
  table_map sq_dep_map;
  table_map work_dep_map;  // dependency map used for work
  /* Dependency map of with elements mutually recursive with this with element */
  table_map mutually_recursive;
  /* 
    Dependency map built only for the top level references i.e. for those that
    are encountered in from lists of the selects of the specification unit
  */ 
  table_map top_level_dep_map;
  /*
    Points to a recursive reference in subqueries.
    Used only for specifications without recursive references on the top level.
  */
  TABLE_LIST *sq_rec_ref;
  /* 
    The next with element from the circular chain of the with elements
    mutually recursive with this with element. 
    (If This element is simply recursive than next_mutually_recursive contains
    the pointer to itself. If it's not recursive than next_mutually_recursive
    is set to NULL.) 
  */  
  With_element *next_mutually_recursive;
  /* 
    Total number of references to this element in the FROM lists of
    the queries that are in the scope of the element (including
    subqueries and specifications of other with elements).
  */ 
  uint references;

  /*
    true <=> this With_element is referred in the query in which the
    element is defined
  */
  bool referenced;

  /*
    true <=> this With_element is needed for the execution of the query
    in which the element is defined
  */
  bool is_used_in_query;

  /* 
    Unparsed specification of the query that specifies this element.
    It's used to build clones of the specification if they are needed.
  */
  LEX_CSTRING unparsed_spec;
  /* Offset of the specification in the input string */
  my_ptrdiff_t unparsed_spec_offset;

  /* True if the with element is used a prepared statement */
  bool stmt_prepare_mode;

  /* Return the map where 1 is set only in the position for this element */
  table_map get_elem_map() { return (table_map) 1 << number; }
 
public:
  /*
    Contains the name of the defined With element and the position of
    the subchain of the tables references used by its definition in the
    global chain of TABLE_LIST objects created for the whole query.
  */
  With_element_head *head;

  /*
    Optional list of column names to name the columns of the table introduced
    by this with element. It is used in the case when the names are not
    inherited from the query that specified the table. Otherwise the list is
    always empty.
  */
  List <Lex_ident_sys> column_list;
  List <Lex_ident_sys> *cycle_list;
  /* The query that specifies the table introduced by this with element */
  st_select_lex_unit *spec;
  /* 
    Set to true is recursion is used (directly or indirectly)
    for the definition of this element
  */
  bool is_recursive;
  /*
    For a simple recursive CTE: the number of references to the CTE from
    outside of the CTE specification.
    For a CTE mutually recursive with other CTEs : the total number of
    references to all these CTEs outside of their specification.
    Each of these mutually recursive CTEs has the same value in this field.
  */
  uint rec_outer_references;
  /*
    Any non-recursive select in the specification of a recursive
    with element is a called anchor. In the case mutually recursive
    elements the specification of some them may be without any anchor.
    Yet at least one of them must contain an anchor.
    All anchors of any recursivespecification are moved ahead before
    the prepare stage.
  */  
  /* Set to true if this is a recursive element with an anchor */ 
  bool with_anchor;
  /* 
    Set to the first recursive select of the unit specifying the element
    after all anchor have been moved to the head of the unit.
  */
  st_select_lex *first_recursive;
  
  /* 
    The number of the last performed iteration for recursive table
    (the number of the initial non-recursive step is 0, the number
     of the first iteration is 1).
  */
  uint level;

  /* 
    The pointer to the object used to materialize this with element
    if it's recursive. This object is built at the end of prepare 
    stage and is used at the execution stage.
  */
  select_union_recursive *rec_result;

  /* List of Item_subselects containing recursive references to this CTE */
  SQL_I_List<Item_subselect> sq_with_rec_ref;
  /* List of derived tables containing recursive references to this CTE */
  SQL_I_List<TABLE_LIST> derived_with_rec_ref;

  With_element(With_element_head *h,
               List <Lex_ident_sys> list,
               st_select_lex_unit *unit)
    : next(NULL), base_dep_map(0), derived_dep_map(0),
      sq_dep_map(0), work_dep_map(0), mutually_recursive(0),
      top_level_dep_map(0), sq_rec_ref(NULL),
      next_mutually_recursive(NULL), references(0), 
      referenced(false), is_used_in_query(false),
      head(h), column_list(list), cycle_list(0), spec(unit),
      is_recursive(false), rec_outer_references(0), with_anchor(false),
      level(0), rec_result(NULL)
  { unit->with_element= this; }

  LEX_CSTRING *get_name() { return head->query_name; }
  const char *get_name_str() { return get_name()->str; }

  void set_tables_start_pos(TABLE_LIST **pos)
  { head->tables_pos.set_start_pos(pos); }
  void set_tables_end_pos(TABLE_LIST **pos)
  { head->tables_pos.set_end_pos(pos); }

  bool check_dependencies_in_spec();
  
  void check_dependencies_in_select(st_select_lex *sl, st_unit_ctxt_elem *ctxt,
                                    bool in_subq, table_map *dep_map);
      
  void check_dependencies_in_unit(st_select_lex_unit *unit,
                                  st_unit_ctxt_elem *ctxt,
                                  bool in_subq,
                                  table_map *dep_map);

  void check_dependencies_in_with_clause(With_clause *with_clause, 
                                         st_unit_ctxt_elem *ctxt,
                                         bool in_subq,
                                         table_map *dep_map);

  void  set_dependency_on(With_element *with_elem)
  { base_dep_map|= with_elem->get_elem_map(); }

  bool check_dependency_on(With_element *with_elem)
  { return base_dep_map & with_elem->get_elem_map(); }

  TABLE_LIST *find_first_sq_rec_ref_in_select(st_select_lex *sel);

  bool set_unparsed_spec(THD *thd, const char *spec_start, const char *spec_end,
                         my_ptrdiff_t spec_offset);

  st_select_lex_unit *clone_parsed_spec(LEX *old_lex, TABLE_LIST *with_table);

  bool is_referenced() { return referenced; }

  bool is_hanging_recursive() { return is_recursive && !rec_outer_references; }

  void inc_references() { references++; }

  bool process_columns_of_derived_unit(THD *thd, st_select_lex_unit *unit);

  bool prepare_unreferenced(THD *thd);

  bool check_unrestricted_recursive(st_select_lex *sel,
                                    table_map &unrestricted,
                                    table_map &encountered);

  void print(THD *thd, String *str, enum_query_type query_type);

  With_clause *get_owner() { return owner; }

  bool contains_sq_with_recursive_reference()
  { return sq_dep_map & mutually_recursive; }

  bool no_rec_ref_on_top_level()
  { return !(top_level_dep_map & mutually_recursive); }

  table_map get_mutually_recursive() { return mutually_recursive; }

  With_element *get_next_mutually_recursive()
  { return next_mutually_recursive; }

  TABLE_LIST *get_sq_rec_ref() { return sq_rec_ref; }

  bool is_anchor(st_select_lex *sel);

  void move_anchors_ahead(); 

  bool is_unrestricted();

  bool is_with_prepared_anchor();

  void mark_as_with_prepared_anchor();

  bool is_cleaned();

  void mark_as_cleaned();

  void reset_recursive_for_exec();

  void cleanup_stabilized();

  void set_as_stabilized();

  bool is_stabilized();

  bool all_are_stabilized();

  bool instantiate_tmp_tables();

  void prepare_for_next_iteration();

  void set_cycle_list(List<Lex_ident_sys> *cycle_list_arg);

  friend class With_clause;

  friend
  bool LEX::resolve_references_to_cte(TABLE_LIST *tables,
                                      TABLE_LIST **tables_last);
};

const uint max_number_of_elements_in_with_clause= sizeof(table_map)*8;

/**
  @class With_clause
  @brief Set of with_elements

  It has a reference to the first with element from this with clause.
  This reference allows to navigate through all the elements of the with clause.
  It contains a reference to the unit to which this with clause is attached.
  It also contains a flag saying whether this with clause was specified as recursive.
*/ 

class With_clause : public Sql_alloc
{
private:
  st_select_lex_unit *owner; // the unit this with clause attached to

  /* The list of all with elements from this with clause */
  SQL_I_List<With_element> with_list; 
  /*
    The with clause immediately containing this with clause if there is any,
    otherwise NULL. Now used  only at parsing.
  */
  With_clause *embedding_with_clause;
  /*
    The next with the clause of the chain of with clauses encountered
    in the current statement
  */
  With_clause *next_with_clause;
  /* Set to true if dependencies between with elements have been checked */
  bool dependencies_are_checked;
  /* 
    The bitmap of all recursive with elements whose specifications
    are not complied with restrictions imposed by the SQL standards
    on recursive specifications.
  */ 
  table_map unrestricted;
  /* 
    The bitmap of all recursive with elements whose anchors
    has been already prepared.
  */
  table_map with_prepared_anchor;
  table_map cleaned;
  /* 
    The bitmap of all recursive with elements that
    has been already materialized
  */
  table_map stabilized;

public:
 /* If true the specifier RECURSIVE is present in the with clause */
  bool with_recursive;

  With_clause(bool recursive_fl, With_clause *emb_with_clause)
    : owner(NULL), embedding_with_clause(emb_with_clause),
      next_with_clause(NULL), dependencies_are_checked(false), unrestricted(0),
      with_prepared_anchor(0), cleaned(0), stabilized(0),
      with_recursive(recursive_fl)
  { }

  bool add_with_element(With_element *elem);

  /* Add this with clause to the list of with clauses used in the statement */
  void add_to_list(With_clause **ptr, With_clause ** &last_next)
  {
    if (embedding_with_clause)
    {
      /* 
        An embedded with clause is always placed before the embedding one
        in the list of with clauses used in the query.
      */
      while (*ptr != embedding_with_clause)
        ptr= &(*ptr)->next_with_clause;
      *ptr= this;
      next_with_clause= embedding_with_clause;
    }
    else
    {
      *last_next= this;
      last_next= &this->next_with_clause;
    }
  }

  st_select_lex_unit *get_owner() { return owner; }

  void set_owner(st_select_lex_unit *unit) { owner= unit; }

  void attach_to(st_select_lex *select_lex);

  With_clause *pop() { return embedding_with_clause; }
      
  bool check_dependencies();

  bool check_anchors();

  void move_anchors_ahead();

  With_element *find_table_def(TABLE_LIST *table, With_element *barrier);

  With_element *find_table_def_in_with_clauses(TABLE_LIST *table);

  bool prepare_unreferenced_elements(THD *thd);

  void add_unrestricted(table_map map) { unrestricted|= map; }

  void print(THD *thd, String *str, enum_query_type query_type);

  friend class With_element;

  friend
  bool LEX::check_dependencies_in_with_clauses();
};

inline
bool With_element::is_unrestricted() 
{
  return owner->unrestricted & get_elem_map();
}

inline
bool With_element::is_with_prepared_anchor() 
{
  return owner->with_prepared_anchor & get_elem_map();
}

inline
void With_element::mark_as_with_prepared_anchor() 
{
  owner->with_prepared_anchor|= mutually_recursive;
}


inline
bool With_element::is_cleaned() 
{
  return owner->cleaned & get_elem_map();
}


inline
void With_element::mark_as_cleaned() 
{
  owner->cleaned|= get_elem_map();
}


inline
void With_element::reset_recursive_for_exec()
{
  DBUG_ASSERT(is_recursive);
  level= 0;
  owner->with_prepared_anchor&= ~mutually_recursive;
  owner->cleaned&= ~get_elem_map();
  cleanup_stabilized();
  spec->columns_are_renamed= false;
}



inline
void With_element::cleanup_stabilized()
{
  owner->stabilized&= ~mutually_recursive;
}


inline
void With_element::set_as_stabilized()
{
  owner->stabilized|= get_elem_map();
}


inline
bool With_element::is_stabilized()
{
  return owner->stabilized & get_elem_map();
}


inline
bool With_element::all_are_stabilized()
{
  return (owner->stabilized & mutually_recursive) == mutually_recursive;
}


inline
void With_element::prepare_for_next_iteration()
{
  With_element *with_elem= this;
  while ((with_elem= with_elem->get_next_mutually_recursive()) != this)
  {
    TABLE *rec_table= with_elem->rec_result->first_rec_table_to_update;
    if (rec_table)
      rec_table->reginfo.join_tab->preread_init_done= false;        
  }
}


inline
void With_clause::attach_to(st_select_lex *select_lex)
{
  for (With_element *with_elem= with_list.first;
       with_elem;
       with_elem= with_elem->next)
  {
    select_lex->register_unit(with_elem->spec, NULL);
  }
}


inline
void st_select_lex::set_with_clause(With_clause *with_clause)
{
  master_unit()->with_clause= with_clause;
  if (with_clause)
    with_clause->set_owner(master_unit());
}

#endif /* SQL_CTE_INCLUDED */
