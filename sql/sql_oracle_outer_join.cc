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
  1.1. Outer join operator
  1.2. Outer-joining tables
  1.3 Example 1: peer outer joins
  1.4 Example 2: chained outer joins
  1.5 Outer join graph
  2. Implementation
  2.1 Parser
  2.2 Conversion to LEFT JOIN tree
  2.2.1 Building the graph
  2.2.2 Ordering the graph
  2.2.3 Building the TABLE_LIST structure
  3. Debugging


  == 1. Basic syntax ==

  Oracle's outer join syntax is like this:

    set sql_mode='oracle';
    select * from t1, t2 where t1.col=t2.col(+)

  The (+) is the "outer join operator". It specifies that table t2 is outer-
  joined (i.e. is INNER) and the predicate containing the (+) is the outer
  join's ON expression. This makes the above query to be equivalent to:

    select * from t1.col left join t2 on t1.col=t2.col

  == 1.1. Outer join operator ==
  Outer join operators may occur only in the WHERE clause. The WHERE clause
  may be one predicate or multiple predicates connected with AND. Each of the
  predicates
   - may reference only one outer-joined (aka the "INNER") table (all
     references to its columns have "outer join operator")
   - may reference zero, one or more "OUTER" tables (without outer join
     operator).

  A predicate that refers to an INNER table and OUTER table(s) prescribes that
  the INNER table is joined with outer join operation.

  A predicate that only refers to an INNER table (like "t1.col(+)=124") will be
  added to the table's ON expression, provided there is another predicate that
  prescribes that the inner table is joined with outer join operation
  (otherwise, the predicate remains in the WHERE and a warning is issued).

  == 1.2. Outer-joining tables ==

  If a query uses outer join operators, the FROM clause must be a simple
  comma-separated list of tables (denoting inner join operations):

    FROM t1, t2, ..., tN

  If outer join operators prescribe that some table t_j is joined with outer
  join, the FROM clause becomes:

    FROM (t1, ..., tbl) LEFT JOIN t_j ON outer_join_predicates, ... tN

  Here, all tables used by outer_join_predicates are moved to the left (ok to
  do as inner join is commutative).

  == 1.3 Example 1: peer outer joins ==
  Consider a query:

    select *
    from t1,t2,t3
    where t1.col=t2.col(+) and t1.col=t3.col(+)

  This has two outer join predicates
    OUTER=t1, INNER=t2
    OUTER=t1, INNER=t3

  The query can be transformed to

    select *
    from (t1 left join t2 on t2.col=t1.col) left join t3 on t1.col=t3.col

  or an equvalent query:

    select *
    from (t1 left join t3 on t3.col=t1.col) left join t2 on t1.col=t2.col

  MariaDB optimizer will try to preserve the original order the tables were
  listed in the WHERE clause.

  == 1.4 Example 2: chained outer joins ==
  The query

    select ...
    from t1,t2,t3
    where cond1(t1.col, t2.col(+)) and cond2(t2.col, t3.col(+))

  has two outer join predicates
    OUTER=t1, INNER=t2
    OUTER=t2, INNER=t3

  The query is transformed into

    select ...
    from
      t1
      left join t2 on cond1(t1.col, t2.col)
      left join t3 on cond2(t2.col, t3.col)

  Note that the result of transformation is
     (t1 left join t2) left join t3
  and not
     t1 left join (t2 left join t3)

  (these two expressions are in general not equivalent) There's always just one
  table on the inner side of the outer join.

  == 1.5 Outer join graph ==
  If we take tables as vertexes and OUTER->INNER relationships as edges, then
  we get a directed graph.

  The graph must not have cycles. A query that produces a graph with cycles
  is aborted with error.
  The graph may have alternative paths, like t1->t2->t3 and t1->t4->t3.

  In order to produce the query expression with LEFT JOIN syntax, one needs to
  perform topological sorting of the graph.
  Then, write out the graph vertices (tables) in an order such that all edges
  would come from left to the right.

  == 2. Implementation ==

  == 2.1 Parser ==
  The parser recognizes the "(+)" operator. After parsing, Item objects that
  have a (+) operator somewhere inside have (item->with_flags() & ORA_JOIN)
  flag set.

  == 2.2 Conversion to LEFT JOIN tree ==

  At Name Resolution phase, we convert (+) operators into LEFT JOIN data
  structures (that is, a tree of TABLE_LIST objects with ON expressions).
  This is done in setup_oracle_join().

  === 2.2.1 Building the graph ===
  First, we create an array of table_pos structures. These are the vertices
  of the graph.
  Then, we analyze the WHERE clause and construct graph's edges.

  == 2.2.2 Ordering the graph ==

  Then, we walk the graph and construct a linked list of table_pos structures
  (connected via table_pos::{next,prev}) so that they come in what we call
  "LEFT JOIN syntax order". In decreasing order of importance, the criteria
  are:
  1. Outer tables must come before their inner tables.
  2. Tables that are connected to the tables already in the order must come
     before those who are not
  3. Tables that were listed earlier in the original FROM clause come before.

  == 2.2.3 Building the TABLE_LIST structure ==
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

  Some of the brackets are redundant, this is not a problem because
  simplify_joins() will remove them.

  == 3. Debugging ==
  One can examine the conversion result by doing this:

    create view v1 as select ...
    show create view v1;

  Unlike regular EXPLAIN, this bypasses simplify_joins() call.
*/


/**
  An outer join graph vertex.
*/

struct table_pos: public Sql_alloc
{
  /*
    Links the tables in the "LEFT JOIN syntax order"
  */
  table_pos *next;
  table_pos *prev;

  /* Tables we have outgoing edges to. Duplicates are possible. */
  List<table_pos> inner_side;

  /* Incoming edges */
  List<table_pos> outer_side;

  /* ON condition expressions (to be AND-ed together) */
  List<Item> on_conds;
  TABLE_LIST *table;

  /* Ordinal number of the table in the original FROM clause */
  int order;

  /* TRUE <=> this table is already linked (in the prev<=>next chain) */
  bool processed;

  /* TRUE <=> All tables in outer_side are already linked in prev/next */
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

static bool add_conditions_to_where(THD *thd, Item **conds,
                                    List<Item> &&return_to_where);

/*
  Order the tables (which are inner_side peers of some table)
   - INNER table comes before its OUTER
   - then, tables that were later in the FROM clause come first
*/

static int table_pos_sort(table_pos *a, table_pos *b, void *arg)
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
  @brief
    Put the table @t into "LEFT JOIN syntax order" list after table @end.

  @detail
    @t must not be already in that list.
*/

static void insert_element_after(table_pos *end, table_pos *t,
                                 uint * const processed)
{
  DBUG_ASSERT(t->next == NULL);
  DBUG_ASSERT(t->prev == NULL);
  if (end)
  {
    if ((t->next= end->next))
      end->next->prev= t;
    end->next= t;
    t->prev= end;
  }
  t->processed= TRUE;
  (*processed)++;
}


static bool process_outer_relations(THD* thd,
                                    table_pos *tab,
                                    table_pos *first,
                                    uint * const processed,
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
      The loop of interest would like this:

       t1->t2->t3->t4->-+
               ^        |
               |        |
               +--------+
      However for such graph we would first call
      check_directed_cycle(tab=t3,beginning=t3) and detect the loop.
      We won't get to the point where we we would call
      check_directed_cycle(tab=t3,beginning=t1)
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
  @brief
    Table @tab has been added to the "LEFT JOIN syntax ordering". Add its
    inner-side neighbors first, then add all their connections.

  @param  tab        Table for which to process
  param   processed  Counter of processed tables.
  @param  n_tables   Total number of tables in the join

  @return
    FALSE  OK
    TRUE   Error, found a circular loop in outer join relationships.
*/

static bool process_inner_relations(THD* thd,
                                    table_pos *tab,
                                    uint *const processed,
                                    uint n_tables)
{
  if (tab->inner_side.elements > 0)
  {
    List_iterator_fast<table_pos> it(tab->inner_side);
    table_pos *t;

    /* First, add all inner_side neighbors */
    while ((t= it++))
    {
      if (t->processed)
      {
        /*
          it is case of "non-cyclic" loop (or just processed already
          branch)

             tab->t
                  ^
                  |
              t1--+
          (it would be t1 then t then tab and again probe t (from tab))

          Check if it is also directional loop:
        */
        if (check_directed_cycle(thd, t, t, 0, n_tables))
        {
          /*
             Found a circular dependency:

              t1->tab -> t -+
                   ^        |
                   |        |
                   +--....--+
          */
          my_error(ER_INVALID_USE_OF_ORA_JOIN_CYCLE, MYF(0));
          return TRUE;
        }
      }
      else
        insert_element_after(tab, t, processed);
    }

    /* Second, process the connections of each neighbor */
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
  @brief
    Insert @tab into the "LEFT JOIN syntax ordering" list between @first and
    @last.
*/

static
void insert_element_between(THD *thd, table_pos *tab,
                            table_pos *first, table_pos *last,
                            uint *const processed)
{
  table_pos *curr= last;

  DBUG_ASSERT(first != last);
  // find place to insert
  while (curr->prev != first && tab->order > curr->prev->order &&
         !curr->is_outer_of(curr->prev))
    curr= curr->prev;

  insert_element_after(curr->prev, tab, processed);
}


/*
  @brief
    Table @tab has been added to the "LEFT JOIN syntax ordering". Add its
    outer-side neighbors and all their connections.

  @param tab    Examine tab->outer_side and their connection.
  @param first  All outer-side connections must be added after "first" and
                before the "tab".

  @detail
    Outer-side neighbors need to be added *before* the table tab.
*/

static bool process_outer_relations(THD* thd,
                                    table_pos *tab,
                                    table_pos *first,
                                    uint * const processed,
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
          This (t3 in the example) table serves as inner table for several
          others.

          For example we have such dependencies (outer to the right and inner
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
          So we have to put t4 between t1 and t2 or between t2 and t3
          (depends on its original position because we
          are trying to keep original order where it is possible).

             t1 => t2 => t4 => t3

            SELECT *
            FROM t1 left join t2 on (t1.a=t2.a),
                 t4
                 left join t3 on (t2.a=t3.a and t4.a=t3.a)

          We can also put it before t1, but as far as we have found t1 first
          it have definitely early position in the original list of tables
          than t4.
        */
        insert_element_between(thd, t, first, tab, processed);
        if (process_inner_relations(thd, t, processed, n_tables))
          return TRUE;
      }
    }
  }
  return process_inner_relations(thd, tab, processed, n_tables);
}


#ifndef DBUG_OFF
static void dbug_trace_table_pos(table_pos *t)
{
  DBUG_ENTER("dbug_trace_table_pos");
  DBUG_PRINT("table_pos", ("Table: %s", t->table->alias.str));
  List_iterator_fast iti(t->on_conds);
  Item *item;
  while ((item= iti++))
  {
    StringBuffer<STRING_BUFFER_USUAL_SIZE> expr;
    item->print(&expr, QT_ORDINARY);
    DBUG_PRINT("INFO", ("  On_conds: %s", expr.c_ptr()));
  }
  List_iterator_fast itot(t->outer_side);
  table_pos *tbl;
  while ((tbl= itot++))
    DBUG_PRINT("INFO", ("  Outer side: %s", tbl->table->alias.str));

  List_iterator_fast itit(t->inner_side);
  while ((tbl= itit++))
    DBUG_PRINT("INFO", ("  Inner side: %s", tbl->table->alias.str));

  DBUG_VOID_RETURN;
}


static void dbug_trace_table_entry(const char *prefix,
                                   const char *legend, TABLE_LIST *t)
{
  if (t)
  {
    StringBuffer<STRING_BUFFER_USUAL_SIZE> expr;
    if (t->on_expr)
      t->on_expr->print(&expr, QT_ORDINARY);

    DBUG_PRINT(prefix, ("%s Table: '%s' %p  outer: %s  on_expr: %s", legend,
                        (t->alias.str ? t->alias.str : ""), t,
                        (t->outer_join ? "YES" : "no"),
                        (t->on_expr ? expr.c_ptr() : "NULL")));
  }
  else
    DBUG_PRINT(prefix, ("%s Table: NULL", legend));
}


static void dbug_trace_table_list(TABLE_LIST *t)
{
  DBUG_ENTER("dbug_trace_table_list");
  dbug_trace_table_entry("TABLE_LIST", "---", t);
  dbug_trace_table_entry("INFO", "Embedding", t->embedding);
  if (t->join_list)
  {
    DBUG_PRINT("INFO", ("Join list: %p elements %d",
                        t->join_list, t->join_list->elements));
    List_iterator_fast<TABLE_LIST> it(*t->join_list);
    TABLE_LIST *tbl;
    while ((tbl= it++))
    {
      dbug_trace_table_entry("INFO", "join_list", tbl);
    }
  }
  else
    DBUG_PRINT("INFO", ("Join list: NULL"));

  DBUG_VOID_RETURN;
}
#endif


/**
  @brief
    Init array of graph vertexes

  @return
    TRUE   Error
    FALSE  Ok, conversion is either done or not needed.
*/

static bool init_tables_array(TABLE_LIST *tables,
                              uint n_tables,
                              table_pos *tab)
{
  table_pos *t= tab;
  TABLE_LIST *table= tables;
  uint i= 0;
  DBUG_ENTER("init_tables_array");

  /*
    Create a graph vertex for each table.
    Also note the original order of tables in the FROM clause.
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
  DBUG_RETURN(FALSE);
}


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

bool setup_oracle_join(THD *thd, Item **conds,
                       TABLE_LIST *tables,
                       SQL_I_List<TABLE_LIST> &select_table_list,
                       List<TABLE_LIST> *select_join_list,
                       List<Item> *all_fields)
{
  DBUG_ENTER("setup_oracle_join");
  uint n_tables= select_table_list.elements;
  uint i= 0;

  if (!(*conds)->with_ora_join() || n_tables == 0)
    DBUG_RETURN(FALSE); // no oracle joins

  table_pos *tab= (table_pos *) new(thd->mem_root) table_pos[n_tables];
  if (init_tables_array(tables, n_tables, tab))
    DBUG_RETURN(TRUE); // mixed with other joins


  /*
    Process the WHERE clause:
     - Find Outer Join conditions
     - Create graph edges from them
     - Remove them from the WHERE clause
  */
  if (is_cond_and(*conds))
  {
    Item_cond_and *and_item= (Item_cond_and *)(*conds);
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

  /*
    Check for inner tables that don't have outer tables;
    Prepare for producing the ordering.
  */
  List<Item> return_to_where;
  return_to_where.empty();
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

        // The item will be moved into the WHERE clause
        item->walk(&Item::remove_ora_join_processor, 0, 0);
      }
      return_to_where.append(&tab[i].on_conds);
      tab[i].on_conds.empty();
    }
    /*
      Sort the outgoing edges in reverse order (those that should come
      first are the last). This is because we will do this:

        for each T in tab[i].inner_side
          insert_element_after(tab[i], T);

      after which the elements will be in the right order.
    */
    if (tab[i].inner_side.elements > 1)
       bubble_sort<table_pos>(&tab[i].inner_side, table_pos_sort, NULL);

#ifndef DBUG_OFF
    dbug_trace_table_pos(tab + i);
    dbug_trace_table_list(tab[i].table);
#endif
  }

  /*
    Order the tables according to the "LEFT JOIN syntax order".
  */
  table_pos *list= NULL;
  table_pos *end= NULL;
  uint processed= 0;
  i= 0;
  do
  {
    // Find the first independent table
    for(;
        i < n_tables && (tab[i].processed || tab[i].outer_side.elements != 0);
        i++);
    if (i >= n_tables)
      break;

    // Set (list, end) to point to the list we've collected so far
    if (list == NULL)
      list= tab + i;
    else
    {
      if (end == NULL)
        end= list;
      while(end->next) end= end->next; // go to the new end
    }

    // Process "sub-graph" with this independent is top of one of branches
    insert_element_after(end, tab + i, &processed);
    process_inner_relations(thd, tab + i, &processed, n_tables);
  } while (i < n_tables);

  if (processed < n_tables)
  {
    /*
      Some tables are not processed but they all have incoming edges.
      This can only happen if there are circular dependencies:

       t1 -> t2 -> t3 -+
        ^              |
        |              |
        +--------------+
    */
    my_error(ER_INVALID_USE_OF_ORA_JOIN_CYCLE, MYF(0));
    DBUG_RETURN(TRUE);
  }

#ifndef DBUG_OFF
  for (table_pos *t= list; t != NULL; t= t->next)
    dbug_trace_table_pos(t);
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
        // update maybe_null which was previously set in setup_table_map()
        if (curr->table->table)
          curr->table->table->maybe_null= JOIN_TYPE_LEFT;
        if (curr->on_conds.elements == 1)
        {
          curr->table->on_expr= curr->on_conds.head();
        }
        else
        {
          Item *item= new(thd->mem_root) Item_cond_and(thd, curr->on_conds);
          if (!item)
            DBUG_RETURN(TRUE);
          item->top_level_item();
          curr->table->on_expr= item;
          /* setup_on_expr() will call fix_fields() for on_expr */
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

  DBUG_PRINT("INFO", ("new FROM clause: %p", new_from));
#ifndef DBUG_OFF
  for (TABLE_LIST *t= new_from; t; t= t->next_local)
  {
    dbug_trace_table_list(t);
  }
#endif

  if (add_conditions_to_where(thd, conds, std::move(return_to_where)))
    DBUG_RETURN(TRUE);

  // Refresh nullability of already fixed parts:

  // WHERE
  if (conds[0])
  {
    conds[0]->update_used_tables();
    conds[0]->walk(&Item::add_maybe_null_after_ora_join_processor, 0, 0);
  }
  // SELECT list and hidden fields
  if (all_fields)
  {
    List_iterator<Item> it(*all_fields);
    Item *item;
    while((item= it++))
    {
      item->update_used_tables();
      item->walk(&Item::add_maybe_null_after_ora_join_processor, 0, 0);
    }
  }
  // parts of WHERE moved to ON (original ONs will be fixed later)
  for (i= 0; i < n_tables; i++)
  {
    // we have to count becaust this lists are included in other lists
    List_iterator<Item> it(*all_fields);
    Item *item;
    for (uint j= 0; j < tab[i].on_conds.elements && (item= it++); j++)
    {
      item->update_used_tables();
      item->walk(&Item::add_maybe_null_after_ora_join_processor, 0, 0);
    }
  }

  DBUG_RETURN(FALSE);
}


/*
  @brief
    Add conditions from return_to_where into *conds.
    Then, normalize *conds: it can be an Item_cond_and with one or zero
    children.

  @return
    false  OK
    true   Fatal error
*/

static bool add_conditions_to_where(THD *thd, Item **conds,
                                    List<Item> &&return_to_where)
{
  // Make changes on statement's mem_root
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
          (*conds)= ((Item_cond_and *)(*conds))->argument_list()->head();
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
    /* Nothing is left in the WHERE */
    DBUG_ASSERT((*conds) == NULL);
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
      // There is one remaining condition, it's in *conds.
      DBUG_ASSERT(return_to_where.elements == 0);
      DBUG_ASSERT((*conds) != NULL);
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
      return_to_where.append(((Item_cond_and *)(*conds))->argument_list());

    if (!((*conds)= new(thd->mem_root) Item_cond_and(thd, return_to_where)))
      return true;
    (*conds)->top_level_item();
    if ((*conds)->fix_fields(thd, conds))
      return true;
  }
  return false;
}

