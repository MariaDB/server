/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2010, 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "lex_ident_sys.h"
#include "mariadb.h"
#include "sql_base.h"
#include "sql_parse.h"      // check_stack_overrun

/*
  Support for Oracle's outer join syntax.

  == Contents ==

  1. Basic syntax
  1.1 Multiple outer join operators
  1.2 Building equvialent LEFT JOIN expression
  2. Implementation
  2.1 Parser
  2.2 Building the hypergraph
  2.3 From hypergraph to outer join structure
  2.4 Building the TABLE_LIST structure
  3. Debugging

  == 1. Basic syntax ==

  Oracle's outer join syntax is like this:

    set sql_mode='oracle';
    select * from t1, t2 where t1.col=t2.col(+)

  The (+) is the "outer join operator". It specifies that table t2 is outer-
  joined (i.e. is INNER) and the predicate containing the (+) is the outer
  join's ON expression. This makes the above query to be equivalent to:

    t1.col left join t2 on t1.col=t2.col

  === 1.1 Multiple outer join operators ===

  The WHERE condition may have multiple "outer join operators" that specify
  multiple outer join operations.
  Outer Join predicates must be located in the WHERE's top-level predicates
  that are connected with AND.

  Following Oracle, we have a rule that
   - A predicate may reference only one outer-joined (aka "INNER") table.
   - Also it may reference zero, one or more "OUTER" tables.

  If the predicate has references to OUTER table(s), it describes an edge in
  a hypergraph where vertices are tables and hyper-edges are outer join
  relationships:

     {outer_tbl1, ..., outer_tblN} -> inner_tbl

  If the predicate only refers to one INNER table (like "t2.col(+)=1"), then
  it doesn't specify a hyper-dege. It just specifies an extra condition to be
  added to t2's ON expression.

  === 1.2 Building equvialent LEFT JOIN expression ==

  One can walk the hypergraph and write out a LEFT JOIN expression that is
  equivalent to the original query with Oracle's syntax.

  Start with a node that has no incoming edges. It may have several outgoing
  edges. For example:

    select *
    from t1,t2,t3
    where t1.col=t2.col(+) and t1.col=t3.col(+)

  denotes a hypergraph with eges:

     t1 -> t2
     t1 -> t3

  which can be written as

    select *
    from (t1 left join t2 on t2.col=t1.col) left join t3 on t1.col=t3.col

  or as equvalent query:

    select *
    from (t1 left join t3 on t3.col=t1.col) left join t2 on t1.col=t2.col

  One can write out edges in any order. However, MariaDB's optimizer is not
  (yet) able to convert between the above two query forms, so we write outgoing
  edges in the order the connected tables were listed in the original query's
  FROM clause.

  The hypergraph may also have chains:

    select *
    from t1,t2,t3
    where cond1(t1.col, t2.col(+)) and cond2(t2.col, t3.col(+))

  gives a hypergraph

     t1 -> t2
     t2 -> t3

  Here, the operation at the start of the chain is done first. The above gives

    select *
    from
      t1
      left join t2 on cond1(t1.col, t2.col)
      left join t3 on cond2(t2.col, t3.col)

  That is, we get
     (t1 left join t2) left join t3
  and not
     t1 left join (t2 left join t3)
  These two expressions are in general not equivalent.

  Note that the hypergraph must not have cycles. A query that produces a graph
  with cycles is aborted with error.
  Alternative paths are fine, though. Example: t1->t2->t3 and t1->t4->t3.

  == 2. Implementation ==

  == 2.1 Parser ==
  The parser recognizes the "(+)" operator. After parsing, Item objects that
  have a (+) operator somewhere inside them have 
  (item->with_flags() & ORA_JOIN) flag set.

  == 2.2 Building the hypergraph ==

  At Name Resolution phase, we convert (+) operators into LEFT JOIN data
  structures (that is, a tree of TABLE_LIST objects with ON expressions).
  This is done in setup_oracle_join().

  First, we create an array of table_pos structures. These are the vertices
  of the hypergraph.
  Then, we analyze the WHERE clause and construct hypergraph's edges.

  == 2.3 From hypergraph to outer joins ==
  Then, we do an analog of topological sorting: build a linked list of
  table_pos entries (connected via table_pos::{next,prev}) that goes in the
  order that is
    1. Required by use of LEFT JOIN syntax: outer tables before their inner.
    2. (after satisfying #1) follows the order the tables were listed in in the
      original FROM clause.

  == 2.4 Building the TABLE_LIST structure ==
  Then, we walk through the table_pos objects via {table_pos::next} edges and
  create a parsed LEFT JOIN data sructure.

  For a chain of t1-t2-t3-t4-t5 we would create:

     (
         (
             (
                 t1
                 [left] join t2 on cond2
             )
             [left] join t3 on cond3
         )
         [left] join t4 on cond4
     )
     [left] join t5 on cond5

  Each pair of () brackets is a TABLE_LIST object representing a join nest. It
  has a NESTED_JOIN object which includes its two children.

  Some of these are redundant, this is not a problem because simplify_joins()
  will remove them.

  == 3. Debugging ==
  One can examine the conversion result by doing this:

    create view v1 as select ...
    show create view v1;

  Unlike regular EXPLAIN, this bypasses simplify_joins() call.
*/

/**
  Structure to collect oracle outer join relations and order tables
*/

struct table_pos: public Sql_alloc
{
  table_pos *next; // user to order tables correctly
  table_pos *prev;
  List<table_pos> inner_side;
  List<table_pos> outer_side;
  List<Item> on_conds;
  TABLE_LIST *table;

  /* Ordinal number of the table in the original FROM clause */
  int order;

  /* TRUE <=> this table is already a part of prev<=>next chain. */
  bool processed;
  bool outer_processed;

  bool is_outer_of(table_pos *tab)
  {
    List_iterator_fast<table_pos> it(outer_side);
    table_pos *t;
    while((t= it++))
    {
      if (t == tab)
        return TRUE;
    }
    return FALSE;
   }
};


/*
  Order the tables:
   - INNER table comes before its OUTER
     (TODO why do we just check immediate neighbors in outer_side,
      without checking a->outer_side[IDX1]->outer_side[IDX2]==b ?)
   - then, tables that were later in the FROM clause come first
*/

static int
table_pos_sort(table_pos *a, table_pos *b, void *arg)
{
  if (a->is_outer_of(b))
    return -1;
  if (b->is_outer_of(a))
    return 1;
  return b->order - a->order;
}


/**
  @brief
    Collect info about table relationships from a part of WHERE condition

  @detail
    This processes an individual AND-part of the WHERE clause.
    We catch these patterns:

    1. Condition refers to one or more OUTER tables and one INNER table:
       cond(outer_table1.col, outer_table2.col, ..., inner_table.col(+))

    2. Condition refers to just one inner table:
       cond(inner_table.col(+), constants)

    We note one single inner table and zero or more outer tables and record
    these dependencies in the table graph.

    Also, the predicates that will form the ON expression are collected in
    table_list::on_conds.
*/

static bool
ora_join_process_expression(THD *thd, Item *cond,
                            table_pos *tab, uint n_tables)
{
  DBUG_ENTER("ora_join_process_expression");

  struct ora_join_processor_param param;
  param.inner= NULL;
  param.outer.empty();
  param.or_present= FALSE;

  if (cond->walk(&Item::ora_join_processor, (void *)(&param), WALK_NO_REF))
    DBUG_RETURN(TRUE);

  /*
    There should be at least one table for inner part (outer can be absent in
    case of constants)
  */
  DBUG_ASSERT(param.inner != NULL);
  table_pos *inner_tab= tab + param.inner->ora_join_table_no;

  if (param.outer.elements > 0)
  {
    if (param.or_present)
    {
      my_error(ER_INVALID_USE_OF_ORA_JOIN_WRONG_FUNC, MYF(0));
      DBUG_RETURN(TRUE);
    }
    {
      // Permanent list can be used in AND
      Query_arena_stmt on_stmt_arena(thd);
      inner_tab->on_conds.push_back(cond);
    }
    List_iterator_fast<TABLE_LIST> it(param.outer);
    TABLE_LIST *t;
    while ((t= it++))
    {
      table_pos *outer_tab= tab + t->ora_join_table_no;
      outer_tab->inner_side.push_back(inner_tab);
      inner_tab->outer_side.push_back(outer_tab);
    }
  }
  else
  {
    // Permanent list can be used in AND
    Query_arena_stmt on_stmt_arena(thd);
    inner_tab->on_conds.push_back(cond);
  }

  DBUG_RETURN(FALSE);
}


/**
  Put table in the table order list

  Note: the table should not be in the list
*/

static void process_tab(table_pos *t, table_pos *end, uint &processed)
{
  DBUG_ASSERT(t->next == NULL);
  DBUG_ASSERT(t->prev == NULL);
  if (end)
  {
    t->next= end->next;
    end->next= t;
    t->prev= end;
  }
  t->processed= TRUE;
  processed++;
}


static bool put_after(THD *thd,
                      table_pos *tab,
                      table_pos *first,
                      uint &processed,
                      uint n_tables);
static bool process_outer_relations(THD* thd,
                                    table_pos *tab,
                                    table_pos *first,
                                    uint &processed,
                                    uint n_tables);


/**
  @brief
  Check presence of directional cycles (starting from "tab" with beginning
  of check in "beginning") in case we have non-directional cycle

  @bdetail
    Recusively check if table "beginning" is reachable from table "tab" through
    the table_pos::inner_side pointers.
*/

static bool check_directed_cycle(THD* thd, table_pos *tab,
                                 table_pos* beginning, uint lvl, uint max)
{
  List_iterator_fast<table_pos> it(tab->inner_side);
  table_pos *t;
  uchar buff[STACK_BUFF_ALLOC]; // Max argument in function
  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return(TRUE);// Fatal error flag is set!

  if ((++lvl) >= max)
  {
    /*
      We checked tables more times than tables we have => we have other cycle
      reachable from "beginning"

      TODO: try to make such test
    */
    return FALSE;
  }
  while ((t= it++))
  {
    if (t == beginning)
      return true;
    if (check_directed_cycle(thd, t, beginning, lvl, max))
      return TRUE;
  }
  return FALSE;
}


/**
  Process inner relation of the table just added to the order list

  @param  tab        Table for which to process
  param   processed  Counter.
  @param  n_tables   Total number of tables in the join
*/

static bool process_inner_relations(THD* thd,
                                    table_pos *tab,
                                    uint &processed,
                                    uint n_tables)
{
  if (tab->inner_side.elements > 0)
  {
    // process this "sub-graph"
    List_iterator_fast<table_pos> it(tab->inner_side);
    table_pos *t;
    while ((t= it++))
    {
      if (t->processed)
      {
        /*
          it is case of "non-cyclic" loop (or just processed already
          branch)
           for example: here tab=t2, t=t3, t3->processed=true already:

           t1-> t3
                 ^
                 |
              t2-+
          (it would be t1 then t3 then t2 and again probe t3 (from t2)

          Check if it is directional loop also:
        */
        if (check_directed_cycle(thd, t, t, 0, n_tables))
        {
          /*
             "round" cyclic reference happened

              t1 -> t2 -> t3 -+
                    ^         |
                    |         |
                    +---------+

              t2 is "processed" in the example
          */
          my_error(ER_INVALID_USE_OF_ORA_JOIN_CYCLE, MYF(0));
          return TRUE;
        }
      }
      else
      {
        if (put_after(thd, t, tab, processed, n_tables))
          return TRUE;
      }
    }
    it.rewind();
    while ((t= it++))
    {
      if (!t->outer_processed)
      {
        if (process_outer_relations(thd, t, tab, processed, n_tables))
          return TRUE;
      }
    }
  }
  return FALSE;
}


/*
  Put "tab" between "first_in_this_subgraph" and "last_in_the_subgraph".

  Used to process OUTER tables of "last_in_the_subgraph" inserted as
  INNER table of "first_in_this_subgraph".
*/

static bool put_between(THD *thd,
                        table_pos *tab,
                        table_pos *first, table_pos *last,
                        uint &processed,
                        uint n_tables)
{
  table_pos *curr= last;
  uchar buff[STACK_BUFF_ALLOC]; // Max argument in function
  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return(TRUE);// Fatal error flag is set!

  DBUG_ASSERT(first != last);
  // find place to insert
  while (curr->prev != first && tab->order > curr->prev->order &&
         !curr->is_outer_of(curr->prev))
    curr= curr->prev;
  process_tab(tab, curr->prev, processed);

  process_inner_relations(thd, tab, processed, n_tables);

  return FALSE;
}


static bool process_outer_relations(THD* thd,
                                    table_pos *tab,
                                    table_pos *first,
                                    uint &processed,
                                    uint n_tables)
{
  uchar buff[STACK_BUFF_ALLOC]; // Max argument in function
  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return(TRUE);// Fatal error flag is set!
  tab->outer_processed= TRUE;
  if (tab->outer_side.elements)
  {
    List_iterator_fast<table_pos> it(tab->outer_side);
    table_pos *t;
    while((t= it++))
    {
      if (!t->processed)
      {
        /*
          This (t3 in the example) table serve as inner table for several
          others.

          For example we have such dependences (outer to the right and inner
          to the left):
          SELECT *
            FROM t1,t2,t3,t4
            WHERE t1.a=t2.a(+) AND t2.a=t3.a(+) AND
                  t4.a=t3.a(+);

          t1->t2-+
                 |
                 +==>t3
                 |
          t4-----+

          So we have built following list of left joins already (we
          started from the first independent table we have found - t1
          and went by inner_side relation till table t3):

          first
          |
          *->t1 => t2 => t3
                         ^
                         |
            t->t4       tab

          Now we go by list of unprocessed outer relation of t3 and put
          them  before t3.
          So we have to put t4 somewhere between t1 and t2 or
          between t2 and t3 (depends on its original position because we
          are trying to keep original order where it is possible).

             t1 => t2 => t4 => t3

          SELECT *
            FROM t1 left join t2 on (t1.a=t2.a),
                 t4
                 left join t3 on (t2.a=t3.a and t1.a=t3)

          We can also put it before t1, but as far as we have found t1 first
          it have definitely early position in the original list of tables
          than t4.
        */
        if (put_between(thd, t, first, tab,
                        processed, n_tables))
          return TRUE;
      }
    }
  }
  return process_inner_relations(thd, tab, processed, n_tables);
}


/**
  TODO:WRONG:Put tab after prev and process its OUTER side relations

  @brief
    Add @tab into the ordering after @first.
*/

static bool put_after(THD *thd,
                      table_pos *tab,
                      table_pos *first,
                      uint &processed,
                      uint n_tables)
{
  //TODO: why check for stack overrun here, we are just going to call a trivial
  //      function?
  uchar buff[STACK_BUFF_ALLOC]; // Max argument in function
  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return(TRUE);// Fatal error flag is set!

  process_tab(tab, first, processed);

  return FALSE;
}


#ifndef DBUG_OFF
static void dbug_print_tab_pos(table_pos *t)
{
  DBUG_PRINT("XXXX", ("Table: %s", t->table->alias.str));
  List_iterator_fast iti(t->on_conds);
  Item *item;
  while ((item= iti++))
  {
    StringBuffer<STRING_BUFFER_USUAL_SIZE> buf;
    String expr(buf);
    item->print(&expr, QT_ORDINARY);
    DBUG_PRINT("INFO", ("  On: %s", expr.c_ptr()));
  }
  List_iterator_fast itot(t->outer_side);
  table_pos *tbl;
  while ((tbl= itot++))
  {
    DBUG_PRINT("INFO", ("  Inner side: %s",
               tbl->table->alias.str));
  }
  List_iterator_fast itit(t->inner_side);
  while ((tbl= itit++))
  {
    DBUG_PRINT("INFO", ("  Outer side: %s",
               tbl->table->alias.str));
  }
}


static void dbug_print_table_name(const char *prefix,
                                  const char *legend, TABLE_LIST *t)
{
  // TODO: dbug_print_item is Single-threaded! It's only for gdb!
  // one can't use it when writing to debug trace!
  if (t)
    DBUG_PRINT(prefix, ("%s Table: '%s' %p  outer: %s  on: %s", legend,
                        (t->alias.str?t->alias.str:""), t,
                        (t->outer_join ? "YES" : "no"),
                        (t->on_expr ? dbug_print_item(t->on_expr) : "NULL")));

  else
    DBUG_PRINT(prefix, ("%s Table: NULL", legend));
}

static void dbug_print_table(TABLE_LIST *t)
{
  dbug_print_table_name("XXX", "---", t);
  dbug_print_table_name("INFO", "Embedding", t->embedding);
  if (t->join_list)
  {
    DBUG_PRINT("INFO", ("Join list: %p elements %d",
                        t->join_list, t->join_list->elements));
    List_iterator_fast<TABLE_LIST> it(*t->join_list);
    TABLE_LIST *tbl;
    while ((tbl= it++))
    {
      dbug_print_table_name("INFO", "join_list", tbl);
    }
  }
  else
    DBUG_PRINT("INFO", ("Join list: NULL"));
}
#endif


/**
  @brief
    Convert Oracle's outer join (+) operators into regular outer join
    structures

  @param conds             INOUT  The WHERE condition
  @param select_join_list  INOUT  Top-level join list

  @return
    TRUE   Error
    FALSE  Ok, conversion is either done or not needed.
*/

bool setup_oracle_join(THD *thd, COND **conds,
                       TABLE_LIST *tables,
                       SQL_I_List<TABLE_LIST> &select_table_list,
                       List<TABLE_LIST> *select_join_list)
{
  DBUG_ENTER("setup_oracle_join");
  uint n_tables= select_table_list.elements;
  Item_cond_and *and_item= NULL;

  if (!(*conds)->with_ora_join() || n_tables == 0)
    DBUG_RETURN(FALSE); // no oracle joins

  table_pos *tab= (table_pos *) new(thd->mem_root) table_pos[n_tables];
  table_pos *t= tab;
  TABLE_LIST *table= tables;
  uint i= 0;

  // Setup the original table order (TODO: what is this)
  /*
    Create hypergraph vertices for each table.
  */
  for (;table; i++, t++, table= table->next_local)
  {
    DBUG_ASSERT(i < n_tables);
    if (table->outer_join || table->nested_join || table->natural_join ||
        table->embedding || table->straight)
    {
      // mixed with other JOIN operations
      my_error(ER_INVALID_USE_OF_ORA_JOIN_MIX, MYF(0));
      DBUG_RETURN(TRUE);
    }
    t->next= t->prev=  NULL;
    t->inner_side.empty();
    t->outer_side.empty();
    t->on_conds.empty();
    t->table= table;
    // psergey-todo: can't we keep ora_join_table_no in table_pos?
    table->ora_join_table_no= t->order= i;
    t->processed= t->outer_processed= FALSE;
  }
  DBUG_ASSERT(i == n_tables);

  /*
    Process the WHERE clause:
     - Find outer join conditions
     - Create hypergraph edges from them
     - Remove them from the WHERE clause
  */
  if (is_cond_and(*conds))
  {
    and_item= (Item_cond_and *)(*conds);
    Item *item;
    List_iterator<Item> it(*and_item->argument_list());
    while ((item= it++))
    {
      if (item->with_ora_join())
      {
        if (ora_join_process_expression(thd, item, tab, n_tables))
          DBUG_RETURN(TRUE);
        item->walk(&Item::remove_ora_join_processor, 0, 0);
        it.remove(); // will be moved to ON
      }
    }
  }
  else
  {
    if ((*conds)->with_ora_join())
    {
     if (ora_join_process_expression(thd, (*conds), tab, n_tables))
        DBUG_RETURN(TRUE);
      (*conds)->walk(&Item::remove_ora_join_processor, 0, 0);
      *conds= NULL; // will be moved to ON
    }
  }

  List<Item> return_to_where;
  return_to_where.empty();
  // Sort relations if needed and check sainles
  for (i= 0; i < n_tables; i++)
  {
    if (tab[i].on_conds.elements > 0 &&
        tab[i].outer_side.elements == 0)
    {
      /*
        This table is marked as INNER but it has no matching OUTER tables. This
        can happen for queries like:

          select * from t1,t2 where t2.a(+)=123;

        Issue a warning and move ON condition predicates back to the WHERE.
      */
      List_iterator_fast it(tab[i].on_conds);
      StringBuffer<STRING_BUFFER_USUAL_SIZE> expr;
      Item *item;
      while ((item= it++))
      {
        expr.set("", 0, system_charset_info);
        item->print(&expr, QT_ORDINARY);
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            WARN_ORA_JOIN_IGNORED,
                            ER_THD(thd, WARN_ORA_JOIN_IGNORED),
                            expr.c_ptr());
      }
      return_to_where.append(&tab[i].on_conds);
      tab[i].on_conds.empty();
    }
    /*
      we do not need inner side sorted, becouse it always processed ba
      inserts with order check

    if (tab[i].outer_side.elements > 1)
       bubble_sort<table_pos>(&tab[i].outer_side,
                              table_pos_sort, NULL);
    */

    /*
      we have to sort this backward becouse after insert the list will be
      reverted
    */
    if (tab[i].inner_side.elements > 1)
       bubble_sort<table_pos>(&tab[i].inner_side,
                              table_pos_sort, NULL);
#ifndef DBUG_OFF
    dbug_print_tab_pos(tab + i);
    dbug_print_table(tab[i].table);
#endif
  }
  // order tables
  table_pos *list= NULL;
  table_pos *end= NULL;
  uint processed= 0;
  i= 0;
  do
  {
    // Find the first independent
    for(;
        i < n_tables && (tab[i].processed || tab[i].outer_side.elements != 0);
        i++);
    if (i >= n_tables)
      break;

    // Process "sub-graph" with this independent is top of one of branches
    if (list == NULL)
      list= tab + i;
    else
    {
      if (end == NULL)
        end= list;
      while(end->next) end= end->next; // go to the new end
    }
    process_tab(tab + i, end, processed);
    process_inner_relations(thd, tab + i, processed, n_tables);
  } while (i < n_tables);

  if (processed < n_tables)
  {
    /*
      "round" cyclic dependence happened

       t1 -> t2 -> t3 -+
        ^              |
        |              |
        +--------------+

       there is no starting point for subgraph processing,
       so this table are left not "processed"
    */
    my_error(ER_INVALID_USE_OF_ORA_JOIN_CYCLE, MYF(0));
    DBUG_RETURN(TRUE);
  }

#ifndef DBUG_OFF
  for(table_pos *t= list; t != NULL; t= t->next)
  {
    dbug_print_tab_pos(t);
  }
#endif


  /*
    Now we build new permanent list of table according to our new order

    table1 [left inner] join table2 ... [left inner] join tableN

    which parses in:

     top_join_list of SELECT_LEX
       |
       nest_tableN-1  --nested_join-> NESTED_JOIN_N-1
       join_list of NESTED_JOIN_N-1
        /\
  tableN  nest_tableN-2
         ...
           join_list of of NESTED_JOIN_2
            /\
      table3  nest_table1  --nested_join-> NESTED_JOIN_1
              join_list of NESTED_JOIN1
               /\
         table2  table1

  */
  TABLE_LIST *new_from= list->table;
  if (n_tables > 1) // nothing to do with only one table
  {
    // changes are permanent
    Query_arena_stmt on_stmt_arena(thd);
    TABLE_LIST *prev_table= list->table;
    TABLE_LIST *nest_table_lists;
    NESTED_JOIN *nested_joins;
    if (!(nest_table_lists=
          (TABLE_LIST*)thd->calloc(ALIGN_SIZE(sizeof(TABLE_LIST)) *
                                     (n_tables - 1) +
                                   ALIGN_SIZE(sizeof(NESTED_JOIN)) *
                                     (n_tables - 1))))
    {
      DBUG_RETURN(TRUE); // EOM
    }
    nested_joins= (NESTED_JOIN *)(((char*)nest_table_lists) +
                                  ALIGN_SIZE(sizeof(TABLE_LIST)) *
                                  (n_tables - 1));
    for (uint i= 0; i < n_tables - 1; i++)
    {
      nest_table_lists[i].nested_join= nested_joins + i;
    }
    nested_joins[0].join_list.empty();
    nested_joins[0].join_list.push_front(list->table);
    DBUG_ASSERT(list->table->embedding == NULL);
    list->table->embedding= nest_table_lists;
    list->table->join_list= &nested_joins->join_list;
    DBUG_ASSERT(list->table->outer_join == 0);
    uint i;
    table_pos *curr;
    for (i=0, curr= list->next; curr; i++, curr= curr->next)
    {
      DBUG_ASSERT(i <= n_tables - 2);
      TABLE_LIST *next_embedding= ((i < n_tables - 2) ?
                                    nest_table_lists + (i+1) :
                                    NULL);
      // join type
      DBUG_ASSERT(curr->table->outer_join == 0);
      DBUG_ASSERT(curr->table->on_expr == 0);
      if (curr->outer_side.elements)
      {
        DBUG_ASSERT(curr->on_conds.elements > 0);
        curr->table->outer_join|=JOIN_TYPE_LEFT;
        if (curr->on_conds.elements == 1)
        {
          curr->table->on_expr= curr->on_conds.head();
        }
        else
        {
          curr->table->on_expr=
            new(thd->mem_root) Item_cond_and(thd, curr->on_conds);
        }
      }
      else
      {
        DBUG_ASSERT(curr->on_conds.elements == 0);
      }

      // add real table
      prev_table->next_local= curr->table;
      nested_joins[i].join_list.push_front(curr->table);
      DBUG_ASSERT(curr->table->embedding == NULL);
      curr->table->embedding= nest_table_lists + i;
      curr->table->join_list= &nested_joins[i].join_list;
      // prepare fake table
      nest_table_lists[i].alias= {STRING_WITH_LEN("(nest_last_join)")};
      nest_table_lists[i].embedding= next_embedding;
      //nest_table_lists[i].nested_join= nested_joins + i;

      if (next_embedding)
      {
        nested_joins[i+1].join_list.empty();
        nested_joins[i+1].join_list.push_front(nest_table_lists + i);
        nest_table_lists[i].join_list= &nested_joins[i + 1].join_list;
      }
      else
      {
        DBUG_ASSERT(i == n_tables - 2);
        // all tables should be there because query was without JOIN
        // operators except oracle ones
        DBUG_ASSERT(select_join_list->elements == n_tables);
        select_join_list->empty();
        select_join_list->push_front(nest_table_lists + i);
        nest_table_lists[i].join_list= select_join_list;
      }

      prev_table= curr->table;
    }
    prev_table->next_local= NULL;
    select_table_list.first= new_from;
    select_table_list.next= &prev_table->next_local;
  }
  // make conds looks nice;
  {
    // changes are permanent
    Query_arena_stmt on_stmt_arena(thd);
    uint number_of_cond_parts= return_to_where.elements;
    if ((*conds) != NULL)
    {
       if (is_cond_and(*conds))
       {
          uint elements= ((Item_cond_and *)(*conds))->argument_list()->elements;
          switch (elements)
          {
            case 0:
              (*conds)= NULL;
              break;
            case 1:
              (*conds)=  ((Item_cond_and *)(*conds))->argument_list()->head();
              number_of_cond_parts++;
              break;
             default:
               number_of_cond_parts+= elements;
          }
       }
       else
        number_of_cond_parts++;
    }

    if (number_of_cond_parts == 0)
    {
      DBUG_ASSERT((*conds) == NULL);
      // all is at its place
    }
    else if (number_of_cond_parts == 1)
    {
      if ((*conds) == NULL)
      {
        DBUG_ASSERT(return_to_where.elements == 1);
        (*conds)= return_to_where.head();
      }
      else
      {
        DBUG_ASSERT(return_to_where.elements == 0);
        DBUG_ASSERT((*conds) != NULL);
        // all is at its place
      }
    }
    else
    {
      if ((*conds) == NULL || !is_cond_and(*conds))
      {
        if (*conds)
          return_to_where.push_back(*conds);
      }
      else
      {
        return_to_where.append(((Item_cond_and *)(*conds))->argument_list());
      }
      (*conds)= new(thd->mem_root) Item_cond_and(thd, return_to_where);
      if (!(*conds) || (*conds)->fix_fields(thd, conds))
        DBUG_RETURN(TRUE);
    }
  }
  DBUG_PRINT("YYY", ("new from %p", new_from));
#ifndef DBUG_OFF
  for (TABLE_LIST *t= new_from; t; t= t->next_local)
  {
    dbug_print_table(t);
  }
#endif

  // clean after processing
  if (and_item)
  {
    if (and_item->argument_list()->elements == 0)
    {
      // No condition left
      (*conds)= NULL;
    }
    if (and_item->argument_list()->elements == 1)
    {
      // only one condition left
      (*conds)= (Item *)and_item->argument_list()->head();
    }
  }
  if ((*conds)) // remove flags because we converted them
    (*conds)->walk(&Item::remove_ora_join_processor, 0, 0);
  DBUG_RETURN(FALSE);
}

