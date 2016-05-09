#ifndef SQL_CTE_INCLUDED
#define SQL_CTE_INCLUDED
#include "sql_list.h"
#include "sql_lex.h"

class With_clause;
class select_union;

/**
  @class With_clause
  @brief Set of with_elements

  It has a reference to the first with element from this with clause.
  This reference allows to navigate through all the elements of the with clause.
  It contains a reference to the unit to which this with clause is attached.
  It also contains a flag saying whether this with clause was specified as recursive.
*/ 

class With_element : public Sql_alloc
{
private:
  With_clause *owner;      // with clause this object belongs to
  With_element *next_elem; // next element in the with clause
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
  table_map work_dep_map;  // dependency map used for work
  /* Dependency map of with elements mutually recursive with this with element */
  table_map mutually_recursive; 
  /* 
    Total number of references to this element in the FROM lists of
    the queries that are in the scope of the element (including
    subqueries and specifications of other with elements).
  */ 
  uint references;
  /* 
    Unparsed specification of the query that specifies this element.
    It used to build clones of the specification if they are needed.
  */
  LEX_STRING unparsed_spec;

  /* Return the map where 1 is set only in the position for this element */
  table_map get_elem_map() { return 1 << number; }
 
  TABLE *table;
 
public:
  /*
    The name of the table introduced by this with elememt. The name
     can be used in FROM lists of the queries in the scope of the element.
  */
  LEX_STRING *query_name;
  /*
    Optional list of column names to name the columns of the table introduced
    by this with element. It is used in the case when the names are not
    inherited from the query that specified the table. Otherwise the list is
    always empty.
  */
  List <LEX_STRING> column_list;
  /* The query that specifies the table introduced by this with element */
  st_select_lex_unit *spec;
  /* 
    Set to true is recursion is used (directly or indirectly)
    for the definition of this element
  */
  bool is_recursive;

  bool with_anchor;

  st_select_lex *first_recursive;
  
  uint level;

  select_union *partial_result;
  select_union *final_result;
  select_union_recursive *rec_result;
  TABLE *result_table;

  With_element(LEX_STRING *name,
               List <LEX_STRING> list,
               st_select_lex_unit *unit)
    : next_elem(NULL), base_dep_map(0), derived_dep_map(0),
      work_dep_map(0), mutually_recursive(0), 
      references(0), table(NULL),
      query_name(name), column_list(list), spec(unit),
      is_recursive(false), with_anchor(false),
      partial_result(NULL), final_result(NULL),
      rec_result(NULL), result_table(NULL)
  { reset();}

  bool check_dependencies_in_spec(THD *thd);
  
  void check_dependencies_in_select(st_select_lex *sl, table_map &dep_map);
      
  void check_dependencies_in_unit(st_select_lex_unit *unit, table_map &dep_map);
 
  void  set_dependency_on(With_element *with_elem)
  { base_dep_map|= with_elem->get_elem_map(); }

  bool check_dependency_on(With_element *with_elem)
  { return base_dep_map & with_elem->get_elem_map(); }

  bool set_unparsed_spec(THD *thd, char *spec_start, char *spec_end);

  st_select_lex_unit *clone_parsed_spec(THD *thd, TABLE_LIST *with_table);

  bool is_referenced() { return references != 0; }

  void inc_references() { references++; }

  bool rename_columns_of_derived_unit(THD *thd, st_select_lex_unit *unit);

  bool prepare_unreferenced(THD *thd);

  bool check_unrestricted_recursive(st_select_lex *sel,
                                    table_map &unrestricted,
                                    table_map &encountered);

   void print(String *str, enum_query_type query_type);

  void set_table(TABLE *tab) { table= tab; }

  TABLE *get_table() { return table; }

  bool is_anchor(st_select_lex *sel);

  void move_anchors_ahead(); 

  bool is_unrestricted();

  bool is_with_prepared_anchor();

  void mark_as_with_prepared_anchor();

  bool is_cleaned();

  void mark_as_cleaned();

  void reset()
  {
    level= 0;
  }

  void set_result_table(TABLE *tab) { result_table= tab; }

  friend class With_clause;
  friend
  bool st_select_lex::check_unrestricted_recursive();
  friend
  bool TABLE_LIST::is_with_table_recursive_reference();
};


/**
  @class With_element
  @brief Definition of a CTE table
	
  It contains a reference to the name of the table introduced by this with element,
  and a reference to the unit that specificies this table. Also it contains
  a reference to the with clause to which this element belongs to.	
*/

class With_clause : public Sql_alloc
{
private:
  st_select_lex_unit *owner; // the unit this with clause attached to
  With_element *first_elem; // the first definition in this with clause 
  With_element **last_next; // here is set the link for the next added element
  uint elements; // number of the elements/defintions in this with clauses 
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

  table_map unrestricted;
  table_map with_prepared_anchor;
  table_map cleaned;

public:
 /* If true the specifier RECURSIVE is present in the with clause */
  bool with_recursive;

  With_clause(bool recursive_fl, With_clause *emb_with_clause)
    : owner(NULL), first_elem(NULL), elements(0),
      embedding_with_clause(emb_with_clause), next_with_clause(NULL),
      dependencies_are_checked(false),
    unrestricted(0), with_prepared_anchor(0), cleaned(0),
      with_recursive(recursive_fl)
  { last_next= &first_elem; }

  /* Add a new element to the current with clause */ 
  bool add_with_element(With_element *elem)
  { 
    elem->owner= this;
    elem->number= elements;
    owner= elem->spec;
    owner->with_element= elem;
    *last_next= elem;
    last_next= &elem->next_elem;
    elements++;
    return false;
  }

  /* Add this with clause to the list of with clauses used in the statement */
  void add_to_list(With_clause ** &last_next)
  {
    *last_next= this;
    last_next= &this->next_with_clause;
  }

  With_clause *pop() { return embedding_with_clause; }
      
  bool check_dependencies(THD *thd);

  bool check_anchors();

  void move_anchors_ahead();

  With_element *find_table_def(TABLE_LIST *table);

  With_element *find_table_def_in_with_clauses(TABLE_LIST *table);

  bool prepare_unreferenced_elements(THD *thd);

  void print(String *str, enum_query_type query_type);

  friend class With_element;

  friend
  bool check_dependencies_in_with_clauses(THD *thd, With_clause *with_clauses_list);
  friend
  bool st_select_lex::check_unrestricted_recursive();

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

#endif /* SQL_CTE_INCLUDED */
