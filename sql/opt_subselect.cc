/*
   Copyright (c) 2010, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file

  @brief
    Semi-join subquery optimizations code

*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_base.h"
#include "sql_const.h"
#include "sql_select.h"
#include "filesort.h"
#include "opt_subselect.h"
#include "sql_test.h"
#include <my_bit.h>
#include "opt_trace.h"

/*
  This file contains optimizations for semi-join subqueries.
  
  Contents
  --------
  1. What is a semi-join subquery
  2. General idea about semi-join execution
  2.1 Correlated vs uncorrelated semi-joins
  2.2 Mergeable vs non-mergeable semi-joins
  3. Code-level view of semi-join processing
  3.1 Conversion
  3.1.1 Merged semi-join TABLE_LIST object
  3.1.2 Non-merged semi-join data structure
  3.2 Semi-joins and query optimization
  3.2.1 Non-merged semi-joins and join optimization
  3.2.2 Merged semi-joins and join optimization
  3.3 Semi-joins and query execution

  1. What is a semi-join subquery
  -------------------------------
  We use this definition of semi-join:

    outer_tbl SEMI JOIN inner_tbl ON cond = {set of outer_tbl.row such that
                                             exist inner_tbl.row, for which 
                                             cond(outer_tbl.row,inner_tbl.row)
                                             is satisfied}
  
  That is, semi-join operation is similar to inner join operation, with
  exception that we don't care how many matches a row from outer_tbl has in
  inner_tbl.

  In SQL terms: a semi-join subquery is an IN subquery that is an AND-part of
  the WHERE/ON clause.

  2. General idea about semi-join execution
  -----------------------------------------
  We can execute semi-join in a way similar to inner join, with exception that
  we need to somehow ensure that we do not generate record combinations that
  differ only in rows of inner tables.
  There is a number of different ways to achieve this property, implemented by
  a number of semi-join execution strategies.
  Some strategies can handle any semi-joins, other can be applied only to
  semi-joins that have certain properties that are described below:

  2.1 Correlated vs uncorrelated semi-joins
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  Uncorrelated semi-joins are special in the respect that they allow to
   - execute the subquery (possible as it's uncorrelated)
   - somehow make sure that generated set does not have duplicates
   - perform an inner join with outer tables.
  
  or, rephrasing in SQL form:

  SELECT ... FROM ot WHERE ot.col IN (SELECT it.col FROM it WHERE uncorr_cond)
    ->
  SELECT ... FROM ot JOIN (SELECT DISTINCT it.col FROM it WHERE uncorr_cond)

  2.2 Mergeable vs non-mergeable semi-joins
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  Semi-join operation has some degree of commutability with inner join
  operation: we can join subquery's tables with ouside table(s) and eliminate
  duplicate record combination after that:

    ot1 JOIN ot2 SEMI_JOIN{it1,it2} (it1 JOIN it2) ON sjcond(ot2,it*) ->
              |
              +-------------------------------+
                                              v
    ot1 SEMI_JOIN{it1,it2} (it1 JOIN it2 JOIN ot2) ON sjcond(ot2,it*)
 
  In order for this to work, subquery's top-level operation must be join, and
  grouping or ordering with limit (grouping or ordering with limit are not
  commutative with duplicate removal). In other words, the conversion is
  possible when the subquery doesn't have GROUP BY clause, any aggregate
  functions*, or ORDER BY ... LIMIT clause.

  Definitions:
  - Subquery whose top-level operation is a join is called *mergeable semi-join*
  - All other kinds of semi-join subqueries are considered non-mergeable.

  *- this requirement is actually too strong, but its exceptions are too
  complicated to be considered here.

  3. Code-level view of semi-join processing
  ------------------------------------------
  
  3.1 Conversion and pre-optimization data structures
  ---------------------------------------------------
  * When doing JOIN::prepare for the subquery, we detect that it can be
    converted into a semi-join and register it in parent_join->sj_subselects

  * At the start of parent_join->optimize(), the predicate is converted into 
    a semi-join node. A semi-join node is a TABLE_LIST object that is linked
    somewhere in parent_join->join_list (either it is just present there, or
    it is a descendant of some of its members).
  
  There are two kinds of semi-joins:
  - Merged semi-joins
  - Non-merged semi-joins
   
  3.1.1 Merged semi-join TABLE_LIST object
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  Merged semi-join object is a TABLE_LIST that contains a sub-join of 
  subquery tables and the semi-join ON expression (in this respect it is 
  very similar to nested outer join representation)
  Merged semi-join represents this SQL:

    ... SEMI JOIN (inner_tbl1 JOIN ... JOIN inner_tbl_n) ON sj_on_expr
  
  Semi-join objects of this kind have TABLE_LIST::sj_subq_pred set.
 
  3.1.2 Non-merged semi-join data structure
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  Non-merged semi-join object is a leaf TABLE_LIST object that has a subquery
  that produces rows. It is similar to a base table and represents this SQL:
    
    ... SEMI_JOIN (SELECT non_mergeable_select) ON sj_on_expr
  
  Subquery items that were converted into semi-joins are removed from the WHERE
  clause. (They do remain in PS-saved WHERE clause, and they replace themselves
  with Item_int(1) on subsequent re-executions).

  3.2 Semi-joins and join optimization
  ------------------------------------
  
  3.2.1 Non-merged semi-joins and join optimization
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  For join optimization purposes, non-merged semi-join nests are similar to
  base tables. Each such nest is represented by one one JOIN_TAB, which has 
  two possible access strategies:
   - full table scan (representing SJ-Materialization-Scan strategy)
   - eq_ref-like table lookup (representing SJ-Materialization-Lookup)

  Unlike regular base tables, non-merged semi-joins have:
   - non-zero JOIN_TAB::startup_cost, and
   - join_tab->table->is_filled_at_execution()==TRUE, which means one
     cannot do const table detection, range analysis or other dataset-dependent
     optimizations.
     Instead, get_delayed_table_estimates() will run optimization for the
     subquery and produce an E(materialized table size).
  
  3.2.2 Merged semi-joins and join optimization
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   - optimize_semijoin_nests() does pre-optimization 
   - during join optimization, the join has one JOIN_TAB (or is it POSITION?) 
     array, and suffix-based detection is used, see advance_sj_state()
   - after join optimization is done, get_best_combination() switches 
     the data-structure to prefix-based, multiple JOIN_TAB ranges format.

  3.3 Semi-joins and query execution
  ----------------------------------
  * Join executor has hooks for all semi-join strategies.
    TODO elaborate.

*/

/*
EqualityPropagationAndSjmNests
******************************

Equalities are used for:
P1. Equality propagation 
P2. Equality substitution [for a certain join order]

The equality propagation is not affected by SJM nests. In fact, it is done 
before we determine the execution plan, i.e. before we even know we will use
SJM-nests for execution.

The equality substitution is affected. 

Substitution without SJMs
=========================
When one doesn't have SJM nests, tables have a strict join order:

  ---------------------------------> 
    t1 -- t2 -- t3 -- t4 --- t5 


       ?  ^
           \
            --(part-of-WHERE)


parts WHERE/ON and ref. expressions are attached at some point along the axis.
Expression is allowed to refer to a table column if the table is to the left of
the attachment point. For any given expression, we have a goal: 

  "Move leftmost allowed attachment point as much as possible to the left"

Substitution with SJMs - task setting
=====================================

When SJM nests are present, there is no global strict table ordering anymore:

   
  ---------------------------------> 

    ot1 -- ot2 --- sjm -- ot4 --- ot5 
                   |
                   |                Main execution
   - - - - - - - - - - - - - - - - - - - - - - - -                 
                   |                 Materialization
      it1 -- it2 --/    


Besides that, we must take into account that
 - values for outer table columns, otN.col, are inaccessible at
   materialization step                                           (SJM-RULE)
 - values for inner table columns, itN.col, are inaccessible at Main execution
   step, except for SJ-Materialization-Scan and columns that are in the 
   subquery's select list.                                        (SJM-RULE)

Substitution with SJMs - solution
=================================

First, we introduce global strict table ordering like this:

  ot1 - ot2 --\                    /--- ot3 -- ot5 
               \--- it1 --- it2 --/

Now, let's see how to meet (SJM-RULE).

SJ-Materialization is only applicable for uncorrelated subqueries. From this, it
follows that any multiple equality will either
1. include only columns of outer tables, or
2. include only columns of inner tables, or
3. include columns of inner and outer tables, joined together through one 
   of IN-equalities.

Cases #1 and #2 can be handled in the same way as with regular inner joins.

Case #3 requires special handling, so that we don't construct violations of
(SJM-RULE). Let's consider possible ways to build violations.

Equality propagation starts with the clause in this form

   top_query_where AND subquery_where AND in_equalities

First, it builds multi-equalities. It can also build a mixed multi-equality

  multiple-equal(ot1.col, ot2.col, ... it1.col, itN.col) 

Multi-equalities are pushed down the OR-clauses in top_query_where and in
subquery_where, so it's possible that clauses like this one are built:

   subquery_cond OR (multiple-equal(it1.col, ot1.col,...) AND ...)
   ^^^^^^^^^^^^^                                 \
         |                                        this must be evaluated
         \- can only be evaluated                 at the main phase.
            at the materialization phase

Finally, equality substitution is started. It does two operations:


1. Field reference substitution 
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

(In the code, this is Item_field::replace_equal_field)

This is a process of replacing each reference to "tblX.col" 
with the first element of the multi-equality.          (REF-SUBST-ORIG)

This behaviour can cause problems with Semi-join nests. Suppose, we have a
condition: 

  func(it1.col, it2.col)

and a multi-equality(ot1.col, it1.col). Then, reference to "it1.col" will be 
replaced with "ot1.col", constructing a condition
   
   func(ot1.col, it2.col)

which will be a violation of (SJM-RULE).

In order to avoid this, (REF-SUBST-ORIG) is amended as follows: 

- references to tables "itX.col" that are inner wrt some SJM nest, are
  replaced with references to the first inner table from the same SJM nest.

- references to top-level tables "otX.col" are replaced with references to
  the first element of the multi-equality, no matter if that first element is
  a column of a top-level table or of table from some SJM nest.
                                                              (REF-SUBST-SJM)

  The case where the first element is a table from an SJM nest $SJM is ok, 
  because it can be proven that $SJM uses SJ-Materialization-Scan, and 
  "unpacks" correct column values to the first element during the main
  execution phase.

2. Item_equal elimination
~~~~~~~~~~~~~~~~~~~~~~~~~
(In the code: eliminate_item_equal) This is a process of taking 

  multiple-equal(a,b,c,d,e)

and replacing it with an equivalent expression which is an AND of pair-wise 
equalities:

  a=b AND a=c AND ...

The equalities are picked such that for any given join prefix (t1,t2...) the
subset of equalities that can be evaluated gives the most restrictive
filtering. 

Without SJM nests, it is sufficient to compare every multi-equality member
with the first one:

  elem1=elem2 AND elem1=elem3 AND elem1=elem4 ... 

When SJM nests are present, we should take care not to construct equalities
that violate the (SJM-RULE). This is achieved by generating separate sets of
equalites for top-level tables and for inner tables. That is, for the join
order 

  ot1 - ot2 --\                    /--- ot3 -- ot5 
               \--- it1 --- it2 --/

we will generate
   ot1.col=ot2.col
   ot1.col=ot3.col
   ot1.col=ot5.col
   it2.col=it1.col


2.1 The problem with Item_equals and ORs
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
As has been mentioned above, multiple equalities are pushed down into OR
clauses, possibly building clauses like this:

   func(it.col2) OR multiple-equal(it1.col1, it1.col2, ot1.col)      (1)

where the first part of the clause has references to inner tables, while the
second has references to the top-level tables, which is a violation of
(SJM-RULE).

AND-clauses of this kind do not create problems, because make_cond_for_table()
will take them apart. OR-clauses will not be split. It is possible to
split-out the part that's dependent on the inner table:

   func(it.col2) OR it1.col1=it1.col2

but this is a less-restrictive condition than condition (1). Current execution
scheme will still try to generate the "remainder" condition:

   func(it.col2) OR it1.col1=ot1.col

which is a violation of (SJM-RULE).

QQ: "ot1.col=it1.col" is checked at the upper level. Why was it not removed
here?
AA: because has a proper subset of conditions that are found on this level.
    consider a join order of  ot, sjm(it)
    and a condition
      ot.col=it.col AND ( ot.col=it.col='foo' OR it.col2='bar')

    we will produce: 
       table ot:  nothing
       table it:  ot.col=it.col AND (ot.col='foo' OR it.col2='bar')
                                     ^^^^        ^^^^^^^^^^^^^^^^       
                                      |          \ the problem is that 
                                      |            this part condition didnt
                                      |            receive a substitution
                                      |
                                      +--- it was correct to subst, 'ot' is 
                                           the left-most.


Does it make sense to push "inner=outer" down into ORs?
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Yes. Consider the query:

  select * from ot 
  where ot.col in (select it.col from it where (it.col='foo' OR it.col='bar'))

here, it may be useful to infer that 

   (ot.col='foo' OR ot.col='bar')       (CASE-FOR-SUBST)

and attach that condition to the table 'ot'.

Possible solutions for Item_equals and ORs
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Solution #1
~~~~~~~~~~~
Let make_cond_for_table() chop analyze the OR clauses it has produced and
discard them if they violate (SJM-RULE). This solution would allow to handle
cases like (CASE-FOR-SUBST) at the expense of making semantics of
make_cond_for_table() complicated.

Solution #2
~~~~~~~~~~~
Before the equality propagation phase, none of the OR clauses violate the
(SJM-RULE). This way, if we remember which tables the original equality
referred to, we can only generate equalities that refer to the outer (or inner)
tables. Note that this will disallow handling of cases like (CASE-FOR-SUBST).

Currently, solution #2 is implemented.
*/

LEX_CSTRING weedout_key= {STRING_WITH_LEN("weedout_key")};

static
bool subquery_types_allow_materialization(THD *thd, Item_in_subselect *in_subs);
static bool replace_where_subcondition(JOIN *, Item **, Item *, Item *, bool);
static int subq_sj_candidate_cmp(Item_in_subselect* el1, Item_in_subselect* el2,
                                 void *arg);
static void reset_equality_number_for_subq_conds(Item * cond);
static bool convert_subq_to_sj(JOIN *parent_join, Item_in_subselect *subq_pred);
static bool convert_subq_to_jtbm(JOIN *parent_join, 
                                 Item_in_subselect *subq_pred, bool *remove);
static TABLE_LIST *alloc_join_nest(THD *thd);
static uint get_tmp_table_rec_length(Ref_ptr_array p_list, uint elements);
bool find_eq_ref_candidate(TABLE *table, table_map sj_inner_tables);
static SJ_MATERIALIZATION_INFO *
at_sjmat_pos(const JOIN *join, table_map remaining_tables, const JOIN_TAB *tab,
             uint idx, bool *loose_scan);
static Item *create_subq_in_equalities(THD *thd, SJ_MATERIALIZATION_INFO *sjm, 
                                Item_in_subselect *subq_pred);
static bool remove_sj_conds(THD *thd, Item **tree);
static bool is_cond_sj_in_equality(Item *item);
static bool sj_table_is_included(JOIN *join, JOIN_TAB *join_tab);
static Item *remove_additional_cond(Item* conds);
static void remove_subq_pushed_predicates(JOIN *join, Item **where);

enum_nested_loop_state 
end_sj_materialize(JOIN *join, JOIN_TAB *join_tab, bool end_of_records);


/*
  Check if Materialization strategy is allowed for given subquery predicate.

  @param thd           Thread handle
  @param in_subs       The subquery predicate
  @param child_select  The select inside predicate (the function will
                       check it is the only one)

  @return TRUE  - Materialization is applicable 
          FALSE - Otherwise
*/

bool is_materialization_applicable(THD *thd, Item_in_subselect *in_subs,
                                   st_select_lex *child_select)
{
  st_select_lex_unit* parent_unit= child_select->master_unit();
  /*
    Check if the subquery predicate can be executed via materialization.
    The required conditions are:
    0. The materialization optimizer switch was set.
    1. Subquery is a single SELECT (not a UNION).
       TODO: this is a limitation that can be fixed
    2. Subquery is not a table-less query. In this case there is no
       point in materializing.
    2A The upper query is not a table-less SELECT ... FROM DUAL. We
       can't do materialization for SELECT .. FROM DUAL because it
       does not call setup_subquery_materialization(). We could make 
       SELECT ... FROM DUAL call that function but that doesn't seem
       to be the case that is worth handling.
    3. Either the subquery predicate is a top-level predicate, or at
       least one partial match strategy is enabled. If no partial match
       strategy is enabled, then materialization cannot be used for
       non-top-level queries because it cannot handle NULLs correctly.
    4. Subquery is non-correlated
       TODO:
       This condition is too restrictive (limitation). It can be extended to:
       (Subquery is non-correlated ||
        Subquery is correlated to any query outer to IN predicate ||
        (Subquery is correlated to the immediate outer query &&
         Subquery !contains {GROUP BY, ORDER BY [LIMIT],
         aggregate functions}) && subquery predicate is not under "NOT IN"))
    5. Subquery does not contain recursive references

  A note about prepared statements: we want the if-branch to be taken on
  PREPARE and each EXECUTE. The rewrites are only done once, but we need 
  select_lex->sj_subselects list to be populated for every EXECUTE. 

  */
  if (optimizer_flag(thd, OPTIMIZER_SWITCH_MATERIALIZATION) &&      // 0
        !child_select->is_part_of_union() &&                          // 1
        parent_unit->first_select()->leaf_tables.elements &&          // 2
        child_select->outer_select() &&
        child_select->outer_select()->table_list.first &&             // 2A
        subquery_types_allow_materialization(thd, in_subs) &&
        (in_subs->is_top_level_item() ||                               //3
         optimizer_flag(thd,
                        OPTIMIZER_SWITCH_PARTIAL_MATCH_ROWID_MERGE) || //3
         optimizer_flag(thd,
                        OPTIMIZER_SWITCH_PARTIAL_MATCH_TABLE_SCAN)) && //3
        !in_subs->is_correlated &&                                     //4
        !in_subs->with_recursive_reference)                            //5
   {
     return TRUE;
   }
  return FALSE;
}


/*
  Check if we need JOIN::prepare()-phase subquery rewrites and if yes, do them

  SYNOPSIS
     check_and_do_in_subquery_rewrites()
       join  Subquery's join

  DESCRIPTION
    Check if we need to do
     - subquery -> mergeable semi-join rewrite
     - if the subquery can be handled with materialization
     - 'substitution' rewrite for table-less subqueries like "(select 1)"
     - IN->EXISTS rewrite
    and, depending on the rewrite, either do it, or record it to be done at a
    later phase.

  RETURN
    0      - OK
    Other  - Some sort of query error
*/

int check_and_do_in_subquery_rewrites(JOIN *join)
{
  THD *thd=join->thd;
  st_select_lex *select_lex= join->select_lex;
  st_select_lex_unit* parent_unit= select_lex->master_unit();
  DBUG_ENTER("check_and_do_in_subquery_rewrites");

  /*
    IN/ALL/ANY rewrites are not applicable for so called fake select
    (this select exists only to filter results of union if it is needed).
  */
  if (select_lex == select_lex->master_unit()->fake_select_lex)
    DBUG_RETURN(0);

  /*
    If 
      1) this join is inside a subquery (of any type except FROM-clause 
         subquery) and
      2) we aren't just normalizing a VIEW

    Then perform early unconditional subquery transformations:
     - Convert subquery predicate into semi-join, or
     - Mark the subquery for execution using materialization, or
     - Perform IN->EXISTS transformation, or
     - Perform more/less ALL/ANY -> MIN/MAX rewrite
     - Substitute trivial scalar-context subquery with its value

    TODO: for PS, make the whole block execute only on the first execution
  */
  Item_subselect *subselect;
  if (!thd->lex->is_view_context_analysis() &&          // (1)
      (subselect= parent_unit->item))                   // (2)
  {
    Item_in_subselect *in_subs= NULL;
    Item_allany_subselect *allany_subs= NULL;
    Item_subselect::subs_type substype= subselect->substype();
    switch (substype) {
    case Item_subselect::IN_SUBS:
      in_subs= subselect->get_IN_subquery();
      break;
    case Item_subselect::ALL_SUBS:
    case Item_subselect::ANY_SUBS:
      DBUG_ASSERT(subselect->get_IN_subquery());
      allany_subs= (Item_allany_subselect *)subselect;
      break;
    default:
      break;
    }

    /*
      Try removing "ORDER BY" or even "ORDER BY ... LIMIT" from certain kinds
      of subqueries. The removal might enable further transformations.
    */
    if (substype == Item_subselect::IN_SUBS ||
        substype == Item_subselect::EXISTS_SUBS ||
        substype == Item_subselect::ANY_SUBS ||
        substype == Item_subselect::ALL_SUBS)
    {
      // (1) - ORDER BY without LIMIT can be removed from IN/EXISTS subqueries
      // (2) - for EXISTS, can also remove "ORDER BY ... LIMIT n",
      //       but cannot remove "ORDER BY ... LIMIT n OFFSET m"
      if (!select_lex->limit_params.select_limit ||                  // (1)
          (substype == Item_subselect::EXISTS_SUBS &&                // (2)
           !select_lex->limit_params.offset_limit))                  // (2)
      {
        select_lex->join->order= 0;
        select_lex->join->skip_sort_order= 1;
      }
    }

    /* Resolve expressions and perform semantic analysis for IN query */
    if (in_subs != NULL)
      /*
        TODO: Add the condition below to this if statement when we have proper
        support for is_correlated handling for materialized semijoins.
        If we were to add this condition now, the fix_fields() call in
        convert_subq_to_sj() would force the flag is_correlated to be set
        erroneously for prepared queries.

        thd->stmt_arena->state != Query_arena::PREPARED)
      */
    {
      SELECT_LEX *current= thd->lex->current_select;
      thd->lex->current_select= current->return_after_parsing();
      char const *save_where= thd->where;
      thd->where= "IN/ALL/ANY subquery";

      Item **left= in_subs->left_exp_ptr();
      bool failure= (*left)->fix_fields_if_needed(thd, left);
      thd->lex->current_select= current;
      thd->where= save_where;
      if (failure)
        DBUG_RETURN(-1); /* purecov: deadcode */

      // fix_field above can rewrite left expression
      uint ncols= (*left)->cols();
      /*
        Check if the left and right expressions have the same # of
        columns, i.e. we don't have a case like 
          (oe1, oe2) IN (SELECT ie1, ie2, ie3 ...)

        TODO why do we have this duplicated in IN->EXISTS transformers?
        psergey-todo: fix these: grep for duplicated_subselect_card_check
      */
      if (select_lex->item_list.elements != ncols)
      {
        my_error(ER_OPERAND_COLUMNS, MYF(0), ncols);
        DBUG_RETURN(-1);
      }
    }

    DBUG_PRINT("info", ("Checking if subq can be converted to semi-join"));
    /*
      Check if we're in subquery that is a candidate for flattening into a
      semi-join (which is done in flatten_subqueries()). The
      requirements are:
        1. Subquery predicate is an IN/=ANY subq predicate
        2. Subquery is a single SELECT (not a UNION)
        3. Subquery does not have GROUP BY or ORDER BY
        4. Subquery does not use aggregate functions or HAVING
        5. Subquery predicate is at the AND-top-level of ON/WHERE clause
        6. We are not in a subquery of a single table UPDATE/DELETE that 
             doesn't have a JOIN (TODO: We should handle this at some
             point by switching to multi-table UPDATE/DELETE)
        7. We're not in a table-less subquery like "SELECT 1"
        8. No execution method was already chosen (by a prepared statement)
        9. Parent select is not a table-less select
        10. Neither parent nor child select have STRAIGHT_JOIN option.
        11. It is first optimisation (the subquery could be moved from ON
        clause during first optimisation and then be considered for SJ
        on the second when it is too late)
    */
    if (optimizer_flag(thd, OPTIMIZER_SWITCH_SEMIJOIN) &&
        in_subs &&                                                    // 1
        !select_lex->is_part_of_union() &&                            // 2
        !select_lex->group_list.elements && !join->order &&           // 3
        !join->having && !select_lex->with_sum_func &&                // 4
        in_subs->emb_on_expr_nest &&                                  // 5
        select_lex->outer_select()->join &&                           // 6
        parent_unit->first_select()->leaf_tables.elements &&          // 7
        !in_subs->has_strategy() &&                                   // 8
        select_lex->outer_select()->table_list.first &&               // 9
        !((join->select_options |                                     // 10
           select_lex->outer_select()->join->select_options)          // 10
          & SELECT_STRAIGHT_JOIN) &&                                  // 10
        select_lex->first_cond_optimization)                          // 11
    {
      DBUG_PRINT("info", ("Subquery is semi-join conversion candidate"));

      //(void)subquery_types_allow_materialization(thd, in_subs);

      in_subs->is_flattenable_semijoin= TRUE;

      /* Register the subquery for further processing in flatten_subqueries() */
      if (!in_subs->is_registered_semijoin)
      {
        Query_arena *arena, backup;
        arena= thd->activate_stmt_arena_if_needed(&backup);
        select_lex->outer_select()->sj_subselects.push_back(in_subs,
                                                            thd->mem_root);
        if (arena)
          thd->restore_active_arena(arena, &backup);
        in_subs->is_registered_semijoin= TRUE;
      }

      /*
        Print the transformation into trace. Do it when we've just set
        is_registered_semijoin=TRUE above, and also do it when we've already
        had it set.
      */
      if (in_subs->is_registered_semijoin)
      {
        OPT_TRACE_TRANSFORM(thd, trace_wrapper, trace_transform,
                            select_lex->select_number,
                            "IN (SELECT)", "semijoin");
        trace_transform.add("chosen", true);
      }
    }
    else
    {
      DBUG_PRINT("info", ("Subquery can't be converted to merged semi-join"));
      /* Test if the user has set a legal combination of optimizer switches. */
      DBUG_ASSERT(optimizer_flag(thd, OPTIMIZER_SWITCH_IN_TO_EXISTS |
                                      OPTIMIZER_SWITCH_MATERIALIZATION));
      /*
        Transform each subquery predicate according to its overloaded
        transformer.
      */
      if (subselect->select_transformer(join))
        DBUG_RETURN(-1);

      /*
        If the subquery predicate is IN/=ANY, analyse and set all possible
        subquery execution strategies based on optimizer switches and syntactic
        properties.
      */
      if (in_subs && !in_subs->has_strategy())
      {
        if (is_materialization_applicable(thd, in_subs, select_lex))
        {
          in_subs->add_strategy(SUBS_MATERIALIZATION);

          /*
            If the subquery is an AND-part of WHERE register for being processed
            with jtbm strategy
          */
          if (in_subs->emb_on_expr_nest == NO_JOIN_NEST &&
              optimizer_flag(thd, OPTIMIZER_SWITCH_SEMIJOIN))
          {
            in_subs->is_flattenable_semijoin= FALSE;
            if (!in_subs->is_registered_semijoin)
	    {
              Query_arena *arena, backup;
              arena= thd->activate_stmt_arena_if_needed(&backup);
              select_lex->outer_select()->sj_subselects.push_back(in_subs,
                                                                  thd->mem_root);
              if (arena)
                thd->restore_active_arena(arena, &backup);
              in_subs->is_registered_semijoin= TRUE;
            }
          }
        }

        /*
          IN-TO-EXISTS is the only universal strategy. Choose it if the user
          allowed it via an optimizer switch, or if materialization is not
          possible.
        */
        if (optimizer_flag(thd, OPTIMIZER_SWITCH_IN_TO_EXISTS) ||
            !in_subs->has_strategy())
          in_subs->add_strategy(SUBS_IN_TO_EXISTS);
      }

      /* Check if max/min optimization applicable */
      if (allany_subs && !allany_subs->is_set_strategy())
      {
        uchar strategy= (allany_subs->is_maxmin_applicable(join) ?
                         (SUBS_MAXMIN_INJECTED | SUBS_MAXMIN_ENGINE) :
                         SUBS_IN_TO_EXISTS);
        allany_subs->add_strategy(strategy);
      }

    }
  }
  DBUG_RETURN(0);
}


/**
  @brief Check if subquery's compared types allow materialization.

  @param in_subs Subquery predicate, updated as follows:
    types_allow_materialization TRUE if subquery materialization is allowed.
    sjm_scan_allowed            If types_allow_materialization is TRUE,
                                indicates whether it is possible to use subquery
                                materialization and scan the materialized table.

  @retval TRUE   If subquery types allow materialization.
  @retval FALSE  Otherwise.

  @details
    This is a temporary fix for BUG#36752.
    
    There are two subquery materialization strategies:

    1. Materialize and do index lookups in the materialized table. See 
       BUG#36752 for description of restrictions we need to put on the
       compared expressions.

    2. Materialize and then do a full scan of the materialized table. At the
       moment, this strategy's applicability criteria are even stricter than
       in #1.

       This is so because of the following: consider an uncorrelated subquery
       
       ...WHERE (ot1.col1, ot2.col2 ...) IN (SELECT ie1,ie2,... FROM it1 ...)

       and a join order that could be used to do sjm-materialization: 
          
          SJM-Scan(it1, it1), ot1, ot2
       
       IN-equalities will be parts of conditions attached to the outer tables:

         ot1:  ot1.col1 = ie1 AND ... (C1)
         ot2:  ot1.col2 = ie2 AND ... (C2)
       
       besides those there may be additional references to ie1 and ie2
       generated by equality propagation. The problem with evaluating C1 and
       C2 is that ie{1,2} refer to subquery tables' columns, while we only have 
       current value of materialization temptable. Our solution is to 
        * require that all ie{N} are table column references. This allows 
          to copy the values of materialization temptable columns to the
          original table's columns (see setup_sj_materialization for more
          details)
        * require that compared columns have exactly the same type. This is
          a temporary measure to avoid BUG#36752-type problems.

    JOIN_TAB::keyuse_is_valid_for_access_in_chosen_plan expects that for Semi Join Materialization
    Scan all the items in the select list of the IN Subquery are of the type Item::FIELD_ITEM.
*/

static 
bool subquery_types_allow_materialization(THD* thd, Item_in_subselect *in_subs)
{
  Item *left_exp= in_subs->left_exp();
  DBUG_ENTER("subquery_types_allow_materialization");

  DBUG_ASSERT(left_exp->fixed());

  List_iterator<Item> it(in_subs->unit->first_select()->item_list);
  uint elements= in_subs->unit->first_select()->item_list.elements;
  const char* cause= NULL;

  in_subs->types_allow_materialization= FALSE;  // Assign default values
  in_subs->sjm_scan_allowed= FALSE;

  OPT_TRACE_TRANSFORM(thd, trace_wrapper, trace_transform,
                     in_subs->get_select_lex()->select_number,
                      "IN (SELECT)", "materialization");

  /*
    The checks here must be kept in sync with the one in
    Item_func_in::in_predicate_to_in_subs_transformer().
  */

  bool all_are_fields= TRUE;
  uint32 total_key_length = 0;
  bool converted_from_in_predicate= in_subs->converted_from_in_predicate;
  for (uint i= 0; i < elements; i++)
  {
    Item *outer= left_exp->element_index(i);
    Item *inner= it++;
    all_are_fields &= (outer->real_item()->type() == Item::FIELD_ITEM && 
                       inner->real_item()->type() == Item::FIELD_ITEM);
    total_key_length += inner->max_length;
    if (!inner->
         type_handler()->
         subquery_type_allows_materialization(inner,
                                              outer,
                                              converted_from_in_predicate))
    {
      trace_transform.add("possible", false);
      trace_transform.add("cause", "types mismatch");
      DBUG_RETURN(FALSE);
    }
  }

  /*
     Make sure that create_tmp_table will not fail due to too long keys.
     See MDEV-7122. This check is performed inside create_tmp_table also and
     we must do it so that we know the table has keys created.
     Make sure that the length of the key for the temp_table is atleast
     greater than 0.
  */
  if (!total_key_length)
    cause= "zero length key for materialized table";
  else if (total_key_length > tmp_table_max_key_length())
    cause= "length of key greater than allowed key length for materialized tables";
  else if (elements > tmp_table_max_key_parts())
    cause= "#keyparts greater than allowed key parts for materialized tables";
  else
  {
    in_subs->types_allow_materialization= TRUE;
    in_subs->sjm_scan_allowed= all_are_fields;
    trace_transform.add("sjm_scan_allowed", all_are_fields)
                   .add("possible", true);
    DBUG_PRINT("info",("subquery_types_allow_materialization: ok, allowed"));
    DBUG_RETURN(TRUE);
  }
  trace_transform.add("possible", false).add("cause", cause);
  DBUG_RETURN(FALSE);
}


/**
  Apply max min optimization of all/any subselect
*/

bool JOIN::transform_max_min_subquery()
{
  DBUG_ENTER("JOIN::transform_max_min_subquery");
  Item_subselect *subselect= unit->item;
  if (!subselect || (subselect->substype() != Item_subselect::ALL_SUBS &&
                     subselect->substype() != Item_subselect::ANY_SUBS))
    DBUG_RETURN(0);
  DBUG_RETURN(((Item_allany_subselect *) subselect)->
              transform_into_max_min(this));
}


/*
  Finalize IN->EXISTS conversion in case we couldn't use materialization.

  DESCRIPTION  Invoke the IN->EXISTS converter
    Replace the Item_in_subselect with its wrapper Item_in_optimizer in WHERE.

  RETURN 
    FALSE - Ok
    TRUE  - Fatal error
*/

bool make_in_exists_conversion(THD *thd, JOIN *join, Item_in_subselect *item)
{
  DBUG_ENTER("make_in_exists_conversion");
  JOIN *child_join= item->unit->first_select()->join;
  bool res;

  /* 
    We're going to finalize IN->EXISTS conversion. 
    Normally, IN->EXISTS conversion takes place inside the 
    Item_subselect::fix_fields() call, where item_subselect->fixed()==FALSE (as
    fix_fields() haven't finished yet) and item_subselect->changed==FALSE (as 
    the conversion haven't been finalized)

    At the end of Item_subselect::fix_fields() we had to set fixed=TRUE,
    changed=TRUE (the only other option would have been to return error).

    So, now we have to set these back for the duration of select_transformer()
    call.
  */
  item->changed= 0;
  item->base_flags|= item_base_t::FIXED;

  SELECT_LEX *save_select_lex= thd->lex->current_select;
  thd->lex->current_select= item->unit->first_select();

  res= item->select_transformer(child_join);

  thd->lex->current_select= save_select_lex;

  if (res)
    DBUG_RETURN(TRUE);

  item->changed= 1;
  DBUG_ASSERT(item->fixed());

  Item *substitute= item->substitution;
  bool do_fix_fields= !item->substitution->fixed();
  /*
    The Item_subselect has already been wrapped with Item_in_optimizer, so we
    should search for item->optimizer, not 'item'.
  */
  Item *replace_me= item->optimizer;
  DBUG_ASSERT(replace_me==substitute);

  Item **tree= (item->emb_on_expr_nest == NO_JOIN_NEST)?
                 &join->conds : &(item->emb_on_expr_nest->on_expr);
  if (replace_where_subcondition(join, tree, replace_me, substitute, 
                                 do_fix_fields))
    DBUG_RETURN(TRUE);
  item->substitution= NULL;
   
    /*
      If this is a prepared statement, repeat the above operation for
      prep_where (or prep_on_expr). 
    */
  if (!thd->stmt_arena->is_conventional())
  {
    tree= (item->emb_on_expr_nest == (TABLE_LIST*)NO_JOIN_NEST)?
           &join->select_lex->prep_where : 
           &(item->emb_on_expr_nest->prep_on_expr);

    if (replace_where_subcondition(join, tree, replace_me, substitute, 
                                   FALSE))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


bool check_for_outer_joins(List<TABLE_LIST> *join_list)
{
  TABLE_LIST *table;
  NESTED_JOIN *nested_join;
  List_iterator<TABLE_LIST> li(*join_list);
  while ((table= li++))
  {
    if ((nested_join= table->nested_join))
    {
      if (check_for_outer_joins(&nested_join->join_list))
        return TRUE;
    }
    
    if (table->outer_join)
      return TRUE;
  }
  return FALSE;
}


void find_and_block_conversion_to_sj(Item *to_find,
				     List_iterator_fast<Item_in_subselect> &li)
{
  if (to_find->type() == Item::FUNC_ITEM &&
     ((Item_func*)to_find)->functype() == Item_func::IN_OPTIMIZER_FUNC)
    to_find= ((Item_in_optimizer*)to_find)->get_wrapped_in_subselect_item();

  if (to_find->type() != Item::SUBSELECT_ITEM ||
      ((Item_subselect *) to_find)->substype() != Item_subselect::IN_SUBS)
    return;
  Item_in_subselect *in_subq;
  li.rewind();
  while ((in_subq= li++))
  {
    if (in_subq == to_find)
    {
      in_subq->block_conversion_to_sj();
      return;
    }
  }
}


/*
  Convert semi-join subquery predicates into semi-join join nests

  SYNOPSIS
    convert_join_subqueries_to_semijoins()
 
  DESCRIPTION

    Convert candidate subquery predicates into semi-join join nests. This 
    transformation is performed once in query lifetime and is irreversible.
    
    Conversion of one subquery predicate
    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    We start with a join that has a semi-join subquery:

      SELECT ...
      FROM ot, ...
      WHERE oe IN (SELECT ie FROM it1 ... itN WHERE subq_where) AND outer_where

    and convert it into a semi-join nest:

      SELECT ...
      FROM ot SEMI JOIN (it1 ... itN), ...
      WHERE outer_where AND subq_where AND oe=ie

    that is, in order to do the conversion, we need to 

     * Create the "SEMI JOIN (it1 .. itN)" part and add it into the parent
       query's FROM structure.
     * Add "AND subq_where AND oe=ie" into parent query's WHERE (or ON if
       the subquery predicate was in an ON expression)
     * Remove the subquery predicate from the parent query's WHERE

    Considerations when converting many predicates
    ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    A join may have at most MAX_TABLES tables. This may prevent us from
    flattening all subqueries when the total number of tables in parent and
    child selects exceeds MAX_TABLES.
    We deal with this problem by flattening children's subqueries first and
    then using a heuristic rule to determine each subquery predicate's
    "priority".

  RETURN 
    FALSE  OK
    TRUE   Error
*/

bool convert_join_subqueries_to_semijoins(JOIN *join)
{
  Query_arena *arena, backup;
  Item_in_subselect *in_subq;
  THD *thd= join->thd;
  DBUG_ENTER("convert_join_subqueries_to_semijoins");

  if (join->select_lex->sj_subselects.is_empty())
    DBUG_RETURN(FALSE);

  List_iterator_fast<Item_in_subselect> li(join->select_lex->sj_subselects);

  while ((in_subq= li++))
  {
    SELECT_LEX *subq_sel= in_subq->get_select_lex();
    if (subq_sel->handle_derived(thd->lex, DT_MERGE))
      DBUG_RETURN(TRUE);
    if (subq_sel->join->transform_in_predicates_into_in_subq(thd))
      DBUG_RETURN(TRUE);
    subq_sel->update_used_tables();
  }

  /* 
    Check all candidates to semi-join conversion that occur
    in ON expressions of outer join. Set the flag blocking
    this conversion for them.
  */
  TABLE_LIST *tbl;
  List_iterator<TABLE_LIST> ti(join->select_lex->leaf_tables);
  while ((tbl= ti++))
  {
    TABLE_LIST *embedded;
    TABLE_LIST *embedding= tbl;
    do
    {
      embedded= embedding;
      bool block_conversion_to_sj= false;
      if (embedded->on_expr)
      {
        /*
          Conversion of an IN subquery predicate into semi-join
          is blocked now if the predicate occurs:
          - in the ON expression of an outer join
          - in the ON expression of an inner join embedded directly
            or indirectly in the inner nest of an outer join
	*/
        for (TABLE_LIST *tl= embedded; tl; tl= tl->embedding)
	{
          if (tl->outer_join)
	  {
            block_conversion_to_sj= true;
            break;
          }
        }
      }
      if (block_conversion_to_sj)
      {
	Item *cond= embedded->on_expr;
        if (!cond)
          ;
        else if (cond->type() != Item::COND_ITEM)
          find_and_block_conversion_to_sj(cond, li);
        else if (((Item_cond*) cond)->functype() ==
	         Item_func::COND_AND_FUNC)
	{
          Item *item;
          List_iterator<Item> it(*(((Item_cond*) cond)->argument_list()));
          while ((item= it++))
	  {
	    find_and_block_conversion_to_sj(item, li);
          }
	}
      }
      embedding= embedded->embedding;
    }
    while (embedding &&
           embedding->nested_join->join_list.head() == embedded);
  }

  /* 
    Block conversion to semi-joins for those candidates that
    are encountered in the WHERE condition of the multi-table view
    with CHECK OPTION if this view is used in UPDATE/DELETE.
    (This limitation can be, probably, easily lifted.) 
  */  
  li.rewind();
  while ((in_subq= li++))
  {
    if (in_subq->emb_on_expr_nest != NO_JOIN_NEST &&
        in_subq->emb_on_expr_nest->effective_with_check)
    {
      in_subq->block_conversion_to_sj();
    }
  }

  if (join->select_options & SELECT_STRAIGHT_JOIN)
  {
    /* Block conversion to semijoins for all candidates */ 
    li.rewind();
    while ((in_subq= li++))
    {
      in_subq->block_conversion_to_sj();
    }
  }
      
  li.rewind();
  /* First, convert child join's subqueries. We proceed bottom-up here */
  while ((in_subq= li++)) 
  {
    st_select_lex *child_select= in_subq->get_select_lex();
    JOIN *child_join= child_select->join;
    child_join->outer_tables = child_join->table_count;

    /*
      child_select->where contains only the WHERE predicate of the
      subquery itself here. We may be selecting from a VIEW, which has its
      own predicate. The combined predicates are available in child_join->conds,
      which was built by setup_conds() doing prepare_where() for all views.
    */
    child_select->where= child_join->conds;

    if (convert_join_subqueries_to_semijoins(child_join))
      DBUG_RETURN(TRUE);


    in_subq->sj_convert_priority= 
      MY_TEST(in_subq->do_not_convert_to_sj) * MAX_TABLES * 2 +
      in_subq->is_correlated * MAX_TABLES + child_join->outer_tables;
  }
  
  // Temporary measure: disable semi-joins when they are together with outer
  // joins.
#if 0  
  if (check_for_outer_joins(join->join_list))
  {
    in_subq= join->select_lex->sj_subselects.head();
    arena= thd->activate_stmt_arena_if_needed(&backup);
    goto skip_conversion;
  }
#endif
  //dump_TABLE_LIST_struct(select_lex, select_lex->leaf_tables);
  /* 
    2. Pick which subqueries to convert:
      sort the subquery array
      - prefer correlated subqueries over uncorrelated;
      - prefer subqueries that have greater number of outer tables;
  */
  bubble_sort<Item_in_subselect>(&join->select_lex->sj_subselects,
				 subq_sj_candidate_cmp, NULL);
  // #tables-in-parent-query + #tables-in-subquery < MAX_TABLES
  /* Replace all subqueries to be flattened with Item_int(1) */
  arena= thd->activate_stmt_arena_if_needed(&backup);
 
  li.rewind();
  while ((in_subq= li++))
  {
    bool remove_item= TRUE;
    subquery_types_allow_materialization(thd, in_subq);

    /* Stop processing if we've reached a subquery that's attached to the ON clause */
    if (in_subq->do_not_convert_to_sj)
    {
      OPT_TRACE_TRANSFORM(thd, trace_wrapper, trace_transform,
                          in_subq->get_select_lex()->select_number,
                          "IN (SELECT)", "semijoin");
      trace_transform.add("converted_to_semi_join", false)
                     .add("cause", "subquery attached to the ON clause");
      break;
    }

    if (in_subq->is_flattenable_semijoin) 
    {
      OPT_TRACE_TRANSFORM(thd, trace_wrapper, trace_transform,
                          in_subq->get_select_lex()->select_number,
                          "IN (SELECT)", "semijoin");
      if (join->table_count + 
          in_subq->unit->first_select()->join->table_count >= MAX_TABLES)
      {
        trace_transform.add("converted_to_semi_join", false);
        trace_transform.add("cause",
                            "table in parent join now exceeds MAX_TABLES");
        break;
      }
      if (convert_subq_to_sj(join, in_subq))
        goto restore_arena_and_fail;
      trace_transform.add("converted_to_semi_join", true);
    }
    else
    {
      if (join->table_count + 1 >= MAX_TABLES)
        break;
      if (convert_subq_to_jtbm(join, in_subq, &remove_item))
        goto restore_arena_and_fail;
    }
    if (remove_item)
    {
      Item **tree= (in_subq->emb_on_expr_nest == NO_JOIN_NEST)?
                     &join->conds : &(in_subq->emb_on_expr_nest->on_expr);
      Item *replace_me= in_subq->original_item();
      if (replace_where_subcondition(join, tree, replace_me,
                                     new (thd->mem_root) Item_int(thd, 1),
                                     FALSE))
        goto restore_arena_and_fail;
    }
  }
//skip_conversion:
  /* 
    3. Finalize (perform IN->EXISTS rewrite) the subqueries that we didn't
    convert:
  */
  while (in_subq)
  {
    JOIN *child_join= in_subq->unit->first_select()->join;
    in_subq->changed= 0;
    in_subq->base_flags|= item_base_t::FIXED;

    SELECT_LEX *save_select_lex= thd->lex->current_select;
    thd->lex->current_select= in_subq->unit->first_select();

    bool res= in_subq->select_transformer(child_join);

    thd->lex->current_select= save_select_lex;

    if (res)
      DBUG_RETURN(TRUE);

    in_subq->changed= 1;
    DBUG_ASSERT(in_subq->fixed());

    Item *substitute= in_subq->substitution;
    bool do_fix_fields= !in_subq->substitution->fixed();
    Item **tree= (in_subq->emb_on_expr_nest == NO_JOIN_NEST)?
                   &join->conds : &(in_subq->emb_on_expr_nest->on_expr);
    Item *replace_me= in_subq->original_item();
    if (replace_where_subcondition(join, tree, replace_me, substitute, 
                                   do_fix_fields))
      DBUG_RETURN(TRUE);
    in_subq->substitution= NULL;
    /*
      If this is a prepared statement, repeat the above operation for
      prep_where (or prep_on_expr). Subquery-to-semijoin conversion is 
      done once for prepared statement.
    */
    if (!thd->stmt_arena->is_conventional())
    {
      tree= (in_subq->emb_on_expr_nest == NO_JOIN_NEST)?
             &join->select_lex->prep_where : 
             &(in_subq->emb_on_expr_nest->prep_on_expr);
      /* 
        prep_on_expr/ prep_where may be NULL in some cases. 
        If that is the case, do nothing - simplify_joins() will copy 
        ON/WHERE expression into prep_on_expr/prep_where.
      */
      if (*tree && replace_where_subcondition(join, tree, replace_me, substitute, 
                                     FALSE))
        DBUG_RETURN(TRUE);
    }
    /*
      Revert to the IN->EXISTS strategy in the rare case when the subquery could
      not be flattened.
    */
    in_subq->reset_strategy(SUBS_IN_TO_EXISTS);
    if (is_materialization_applicable(thd, in_subq, 
                                      in_subq->unit->first_select()))
    {
      in_subq->add_strategy(SUBS_MATERIALIZATION);
    }

    in_subq= li++;
  }

  if (arena)
    thd->restore_active_arena(arena, &backup);
  join->select_lex->sj_subselects.empty();
  DBUG_RETURN(FALSE);

restore_arena_and_fail:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(TRUE);
}


/*
  Get #output_rows and scan_time estimates for a "delayed" table.

  SYNOPSIS
    get_delayed_table_estimates()
      table         IN    Table to get estimates for
      out_rows      OUT   E(#rows in the table)
      scan_time     OUT   E(scan_time).
      startup_cost  OUT   cost to populate the table.

  DESCRIPTION
    Get #output_rows and scan_time estimates for a "delayed" table. By
    "delayed" here we mean that the table is filled at the start of query
    execution. This means that the optimizer can't use table statistics to 
    get #rows estimate for it, it has to call this function instead.

    This function is expected to make different actions depending on the nature
    of the table. At the moment there is only one kind of delayed tables,
    non-flattenable semi-joins.
*/

void get_delayed_table_estimates(TABLE *table,
                                 ha_rows *out_rows, 
                                 double *scan_time,
                                 double *startup_cost)
{
  Item_in_subselect *item= table->pos_in_table_list->jtbm_subselect;
  Table_function_json_table *table_function=
                               table->pos_in_table_list->table_function;

  if (table_function)
  {
    table_function->get_estimates(out_rows, scan_time, startup_cost);
    return;
  }

  DBUG_ASSERT(item->engine->engine_type() ==
              subselect_engine::HASH_SJ_ENGINE);

  subselect_hash_sj_engine *hash_sj_engine=
    ((subselect_hash_sj_engine*)item->engine);

  *out_rows= (ha_rows)item->jtbm_record_count;
  *startup_cost= item->jtbm_read_time;

  /* Calculate cost of scanning the temptable */
  double data_size= COST_MULT(item->jtbm_record_count,
                              hash_sj_engine->tmp_table->s->reclength);
  /* Do like in handler::scan_time() */
  *scan_time= ((data_size/table->file->stats.block_size+2) *
               table->file->avg_io_cost());
}


/**
   @brief Replaces an expression destructively inside the expression tree of
   the WHERE clase.

   @note We substitute AND/OR structure because it was copied by
   copy_andor_structure and some changes could be done in the copy but
   should be left permanent, also there could be several layers of AND over
   AND and OR over OR because ::fix_field() possibly is not called.

   @param join The top-level query.
   @param old_cond The expression to be replaced.
   @param new_cond The expression to be substituted.
   @param do_fix_fields If true, Item::fix_fields(THD*, Item**) is called for
   the new expression.
   @return <code>true</code> if there was an error, <code>false</code> if
   successful.
*/

static bool replace_where_subcondition(JOIN *join, Item **expr, 
                                       Item *old_cond, Item *new_cond,
                                       bool do_fix_fields)
{
  if (*expr == old_cond)
  {
    *expr= new_cond;
    if (do_fix_fields)
      new_cond->fix_fields(join->thd, expr);
    return FALSE;
  }
  
  if ((*expr)->type() == Item::COND_ITEM) 
  {
    List_iterator<Item> li(*((Item_cond*)(*expr))->argument_list());
    Item *item;
    while ((item= li++))
    {
      if (item == old_cond)
      {
        li.replace(new_cond);
        if (do_fix_fields)
          new_cond->fix_fields(join->thd, li.ref());
        return FALSE;
      }
      else if (item->type() == Item::COND_ITEM)
      {
        replace_where_subcondition(join, li.ref(),
                                   old_cond, new_cond,
                                   do_fix_fields);
      }
    }
  }
  /* 
    We can come to here when 
     - we're doing replace operations on both on_expr and prep_on_expr
     - on_expr is the same as prep_on_expr, or they share a sub-tree 
       (so, when we do replace in on_expr, we replace in prep_on_expr, too,
        and when we try doing a replace in prep_on_expr, the item we wanted 
        to replace there has already been replaced)
  */
  return FALSE;
}

static int subq_sj_candidate_cmp(Item_in_subselect* el1, Item_in_subselect* el2,
                                 void *arg)
{
  return (el1->sj_convert_priority > el2->sj_convert_priority) ? -1 : 
         ( (el1->sj_convert_priority == el2->sj_convert_priority)? 0 : 1);
}


/**
    @brief
    reset the value of the field in_eqaulity_no for all Item_func_eq
    items in the where clause of the subquery.

    Look for in_equality_no description in Item_func_eq class

    DESCRIPTION
    Lets have an example:
    SELECT t1.a FROM t1 WHERE t1.a IN
      (SELECT t2.a FROM t2 where t2.b IN
          (select t3.b from t3 where t3.c=27 ))

    So for such a query we have the parent, child and
    grandchild select.

    So for the equality t2.b = t3.b we set the value for in_equality_no to
    0 according to its description. Wewe do the same for t1.a = t2.a.
    But when we look at the child select (with the grandchild select merged),
    the query would be

    SELECT t1.a FROM t1 WHERE t1.a IN
      (SELECT t2.a FROM t2 where t2.b = t3.b and t3.c=27)

    and then when the child select is merged into the parent select the query
    would look like

    SELECT t1.a FROM t1, semi-join-nest(t2,t3)
            WHERE t1.a =t2.a and t2.b = t3.b and t3.c=27

    Still we would have in_equality_no set for t2.b = t3.b
    though it does not take part in the semi-join equality for the parent select,
    so we should reset its value to UINT_MAX.

    @param cond WHERE clause of the subquery
*/

static void reset_equality_number_for_subq_conds(Item * cond)
{
  if (!cond)
    return;
  if (cond->type() == Item::COND_ITEM)
  {
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (item->type() == Item::FUNC_ITEM &&
      ((Item_func*)item)->functype()== Item_func::EQ_FUNC)
        ((Item_func_eq*)item)->in_equality_no= UINT_MAX;
    }
  }
  else
  {
    if (cond->type() == Item::FUNC_ITEM &&
      ((Item_func*)cond)->functype()== Item_func::EQ_FUNC)
        ((Item_func_eq*)cond)->in_equality_no= UINT_MAX;
  }
  return;
}

/*
  Convert a subquery predicate into a TABLE_LIST semi-join nest

  SYNOPSIS
    convert_subq_to_sj()
       parent_join  Parent join, the one that has subq_pred in its WHERE/ON 
                    clause
       subq_pred    Subquery predicate to be converted
  
  DESCRIPTION
    Convert a subquery predicate into a TABLE_LIST semi-join nest. All the 
    prerequisites are already checked, so the conversion is always successfull.

    Prepared Statements: the transformation is permanent:
     - Changes in TABLE_LIST structures are naturally permanent
     - Item tree changes are performed on statement MEM_ROOT:
        = we activate statement MEM_ROOT 
        = this function is called before the first fix_prepare_information
          call.

    This is intended because the criteria for subquery-to-sj conversion remain
    constant for the lifetime of the Prepared Statement.

  RETURN
    FALSE  OK
    TRUE   Out of memory error
*/

static bool convert_subq_to_sj(JOIN *parent_join, Item_in_subselect *subq_pred)
{
  SELECT_LEX *parent_lex= parent_join->select_lex;
  TABLE_LIST *emb_tbl_nest= NULL;
  TABLE_LIST *orig_tl;
  List<TABLE_LIST> *emb_join_list= &parent_lex->top_join_list;
  THD *thd= parent_join->thd;
  SELECT_LEX *save_lex;
  Item **left;
  Item *left_exp;
  Item *left_exp_orig;

  uint ncols;
  DBUG_ENTER("convert_subq_to_sj");

  /*
    1. Find out where to put the predicate into.
     Note: for "t1 LEFT JOIN t2" this will be t2, a leaf.
  */
  if ((void*)subq_pred->emb_on_expr_nest != (void*)NO_JOIN_NEST)
  {
    if (subq_pred->emb_on_expr_nest->nested_join)
    {
      /*
        We're dealing with

          ... [LEFT] JOIN  ( ... ) ON (subquery AND whatever) ...

        The sj-nest will be inserted into the brackets nest.
      */
      emb_tbl_nest=  subq_pred->emb_on_expr_nest;
      emb_join_list= &emb_tbl_nest->nested_join->join_list;
    }
    else if (!subq_pred->emb_on_expr_nest->outer_join)
    {
      /*
        We're dealing with

          ... INNER JOIN tblX ON (subquery AND whatever) ...

        The sj-nest will be tblX's "sibling", i.e. another child of its
        parent. This is ok because tblX is joined as an inner join.
      */
      emb_tbl_nest= subq_pred->emb_on_expr_nest->embedding;
      if (emb_tbl_nest)
        emb_join_list= &emb_tbl_nest->nested_join->join_list;
    }
    else if (!subq_pred->emb_on_expr_nest->nested_join)
    {
      TABLE_LIST *outer_tbl= subq_pred->emb_on_expr_nest;
      TABLE_LIST *wrap_nest;
      LEX_CSTRING sj_wrap_name= { STRING_WITH_LEN("(sj-wrap)") };
      /*
        We're dealing with

          ... LEFT JOIN tbl ON (on_expr AND subq_pred) ...

        we'll need to convert it into:

          ... LEFT JOIN ( tbl SJ (subq_tables) ) ON (on_expr AND subq_pred) ...
                        |                      |
                        |<----- wrap_nest ---->|
        
        Q:  other subqueries may be pointing to this element. What to do?
        A1: simple solution: copy *subq_pred->expr_join_nest= *parent_nest.
            But we'll need to fix other pointers.
        A2: Another way: have TABLE_LIST::next_ptr so the following
            subqueries know the table has been nested.
        A3: changes in the TABLE_LIST::outer_join will make everything work
            automatically.
      */
      if (!(wrap_nest= alloc_join_nest(thd)))
      {
        DBUG_RETURN(TRUE);
      }
      wrap_nest->embedding= outer_tbl->embedding;
      wrap_nest->join_list= outer_tbl->join_list;
      wrap_nest->alias= sj_wrap_name;

      wrap_nest->nested_join->join_list.empty();
      wrap_nest->nested_join->join_list.push_back(outer_tbl, thd->mem_root);

      outer_tbl->embedding= wrap_nest;
      outer_tbl->join_list= &wrap_nest->nested_join->join_list;

      /*
        wrap_nest will take place of outer_tbl, so move the outer join flag
        and on_expr
      */
      wrap_nest->outer_join= outer_tbl->outer_join;
      outer_tbl->outer_join= 0;

      wrap_nest->on_expr= outer_tbl->on_expr;
      outer_tbl->on_expr= NULL;

      List_iterator<TABLE_LIST> li(*wrap_nest->join_list);
      TABLE_LIST *tbl;
      while ((tbl= li++))
      {
        if (tbl == outer_tbl)
        {
          li.replace(wrap_nest);
          break;
        }
      }
      /*
        Ok now wrap_nest 'contains' outer_tbl and we're ready to add the 
        semi-join nest into it
      */
      emb_join_list= &wrap_nest->nested_join->join_list;
      emb_tbl_nest=  wrap_nest;
    }
  }

  TABLE_LIST *sj_nest;
  NESTED_JOIN *nested_join;
  LEX_CSTRING sj_nest_name= { STRING_WITH_LEN("(sj-nest)") };
  if (!(sj_nest= alloc_join_nest(thd)))
  {
    DBUG_RETURN(TRUE);
  }
  nested_join= sj_nest->nested_join;

  sj_nest->join_list= emb_join_list;
  sj_nest->embedding= emb_tbl_nest;
  sj_nest->alias= sj_nest_name;
  sj_nest->sj_subq_pred= subq_pred;
  sj_nest->original_subq_pred_used_tables= subq_pred->used_tables() |
                                           subq_pred->left_exp()->used_tables();
  /* Nests do not participate in those 'chains', so: */
  /* sj_nest->next_leaf= sj_nest->next_local= sj_nest->next_global == NULL*/
  emb_join_list->push_back(sj_nest, thd->mem_root);

  /* 
    nested_join->used_tables and nested_join->not_null_tables are
    initialized in simplify_joins().
  */
  
  /* 
    2. Walk through subquery's top list and set 'embedding' to point to the
       sj-nest.
  */
  st_select_lex *subq_lex= subq_pred->unit->first_select();
  DBUG_ASSERT(subq_lex->next_select() == NULL);
  nested_join->join_list.empty();
  List_iterator_fast<TABLE_LIST> li(subq_lex->top_join_list);
  TABLE_LIST *tl;
  while ((tl= li++))
  {
    tl->embedding= sj_nest;
    tl->join_list= &nested_join->join_list;
    nested_join->join_list.push_back(tl, thd->mem_root);
  }
  
  /*
    Reconnect the next_leaf chain.
    TODO: Do we have to put subquery's tables at the end of the chain?
          Inserting them at the beginning would be a bit faster.
    NOTE: We actually insert them at the front! That's because the order is
          reversed in this list.
  */
  parent_lex->leaf_tables.append(&subq_lex->leaf_tables);

  if (subq_lex->options & OPTION_SCHEMA_TABLE)
    parent_lex->options |= OPTION_SCHEMA_TABLE;

  /*
    Same as above for next_local chain
    (a theory: a next_local chain always starts with ::leaf_tables
     because view's tables are inserted after the view)
  */
  
  for (orig_tl= (TABLE_LIST*)(parent_lex->table_list.first);
       orig_tl->next_local;
       orig_tl= orig_tl->next_local)
  {}

  orig_tl->next_local= subq_lex->join->tables_list;

  /* A theory: no need to re-connect the next_global chain */

  /* 3. Remove the original subquery predicate from the WHERE/ON */

  /*TODO: also reset the 'm_with_subquery' there. */

  /* n. Adjust the parent_join->table_count counter */
  uint table_no= parent_join->table_count;
  /* n. Walk through child's tables and adjust table->map */
  List_iterator_fast<TABLE_LIST> si(subq_lex->leaf_tables);
  while ((tl= si++))
  {
    tl->set_tablenr(table_no);
    if (tl->is_jtbm())
    {
      tl->jtbm_table_no= table_no;
      Item *dummy= tl->jtbm_subselect;
      tl->jtbm_subselect->fix_after_pullout(parent_lex, &dummy, true);
      DBUG_ASSERT(dummy == tl->jtbm_subselect);
    }
    else if (tl->table_function)
    {
      tl->table_function->fix_after_pullout(tl, parent_lex, true);
    }
    SELECT_LEX *old_sl= tl->select_lex;
    tl->select_lex= parent_join->select_lex; 
    for (TABLE_LIST *emb= tl->embedding;
         emb && emb->select_lex == old_sl;
         emb= emb->embedding)
      emb->select_lex= parent_join->select_lex;
    table_no++;
  }
  parent_join->table_count += subq_lex->join->table_count;
  //parent_join->table_count += subq_lex->leaf_tables.elements;

  /* 
    Put the subquery's WHERE into semi-join's sj_on_expr
    Add the subquery-induced equalities too.
  */
  save_lex= thd->lex->current_select;
  table_map subq_pred_used_tables;

  thd->lex->current_select=subq_lex;
  left= subq_pred->left_exp_ptr();
  if ((*left)->fix_fields_if_needed(thd, left))
    goto restore_tl_and_exit;
  left_exp= *left;
  left_exp_orig= subq_pred->left_exp_orig();
  thd->lex->current_select=save_lex;

  subq_pred_used_tables= subq_pred->used_tables();
  sj_nest->nested_join->sj_corr_tables= subq_pred_used_tables;
  sj_nest->nested_join->sj_depends_on=  subq_pred_used_tables |
                                        left_exp->used_tables();
  sj_nest->sj_on_expr= subq_lex->join->conds;

  /*
    Create the IN-equalities and inject them into semi-join's ON expression.
    Additionally, for LooseScan strategy
     - Record the number of IN-equalities.
     - Create list of pointers to (oe1, ..., ieN). We'll need the list to
       see which of the expressions are bound and which are not (for those
       we'll produce a distinct stream of (ie_i1,...ie_ik).

       (TODO: can we just create a list of pointers and hope the expressions
       will not substitute themselves on fix_fields()? or we need to wrap
       them into Item_direct_view_refs and store pointers to those. The
       pointers to Item_direct_view_refs are guaranteed to be stable as 
       Item_direct_view_refs doesn't substitute itself with anything in 
       Item_direct_view_ref::fix_fields.
  */
  ncols= sj_nest->sj_in_exprs= left_exp->cols();
  sj_nest->nested_join->sj_outer_expr_list.empty();
  reset_equality_number_for_subq_conds(sj_nest->sj_on_expr);

  if (ncols == 1)
  {
    /* add left = select_list_element */
    nested_join->sj_outer_expr_list.push_back(left,
                                              thd->mem_root);
    /*
      Create Item_func_eq. Note that
      1. this is done on the statement, not execution, arena
      2. if it's a PS then this happens only once - on the first execution.
         On following re-executions, the item will be fix_field-ed normally.
      3. Thus it should be created as if it was fix_field'ed, in particular
         all pointers to items in the execution arena should be protected
         with thd->change_item_tree
    */
    Item_func_eq *item_eq=
      new (thd->mem_root) Item_func_eq(thd, left_exp_orig,
                                       subq_lex->ref_pointer_array[0]);
    if (!item_eq)
      goto restore_tl_and_exit;
    if (left_exp_orig != left_exp)
      thd->change_item_tree(item_eq->arguments(), left_exp);
    item_eq->in_equality_no= 0;
    sj_nest->sj_on_expr= and_items(thd, sj_nest->sj_on_expr, item_eq);
  }
  else if (left_exp->type() == Item::ROW_ITEM)
  {
    /*
      disassemple left expression and add
      left1 = select_list_element1 and left2 = select_list_element2 ...
    */
    for (uint i= 0; i < ncols; i++)
    {
      nested_join->sj_outer_expr_list.push_back(left_exp->addr(i),
                                                thd->mem_root);
      Item_func_eq *item_eq=
        new (thd->mem_root)
        Item_func_eq(thd, left_exp_orig->element_index(i),
                     subq_lex->ref_pointer_array[i]);
      if (!item_eq)
        goto restore_tl_and_exit;
      DBUG_ASSERT(left_exp->element_index(i)->fixed());
      if (left_exp_orig->element_index(i) !=
          left_exp->element_index(i))
        thd->change_item_tree(item_eq->arguments(),
                              left_exp->element_index(i));
      item_eq->in_equality_no= i;
      sj_nest->sj_on_expr= and_items(thd, sj_nest->sj_on_expr, item_eq);
    }
  }
  else
  {
    /*
      add row operation
      left = (select_list_element1, select_list_element2, ...)
    */
    Item_row *row= new (thd->mem_root) Item_row(thd, subq_lex->pre_fix);
    /* fix fields on subquery was call so they should be the same */
    if (!row)
      goto restore_tl_and_exit;
    DBUG_ASSERT(ncols == row->cols());
    nested_join->sj_outer_expr_list.push_back(left);
    Item_func_eq *item_eq=
      new (thd->mem_root) Item_func_eq(thd, left_exp_orig, row);
    if (!item_eq)
      goto restore_tl_and_exit;
    for (uint i= 0; i < row->cols(); i++)
    {
      if (row->element_index(i) != subq_lex->ref_pointer_array[i])
        thd->change_item_tree(row->addr(i), subq_lex->ref_pointer_array[i]);
    }
    item_eq->in_equality_no= 0;
    sj_nest->sj_on_expr= and_items(thd, sj_nest->sj_on_expr, item_eq);
  }
  /*
    Fix the created equality and AND

    Note that fix_fields() can actually fail in a meaningful way here. One
    example is when the IN-equality is not valid, because it compares columns
    with incompatible collations. (One can argue it would be more appropriate
    to check for this at name resolution stage, but as a legacy of IN->EXISTS
    we have in here).
  */
  if (sj_nest->sj_on_expr->fix_fields_if_needed(thd, &sj_nest->sj_on_expr))
    goto restore_tl_and_exit;

  /*
    Walk through sj nest's WHERE and ON expressions and call
    item->fix_table_changes() for all items.
  */
  sj_nest->sj_on_expr->fix_after_pullout(parent_lex, &sj_nest->sj_on_expr,
                                         TRUE);
  fix_list_after_tbl_changes(parent_lex, &sj_nest->nested_join->join_list);


  /* Unlink the child select_lex so it doesn't show up in EXPLAIN: */
  subq_lex->master_unit()->exclude_level();

  DBUG_EXECUTE("where",
               print_where(sj_nest->sj_on_expr,"SJ-EXPR", QT_ORDINARY););

  /* Inject sj_on_expr into the parent's WHERE or ON */
  if (emb_tbl_nest)
  {
    emb_tbl_nest->on_expr= and_items(thd, emb_tbl_nest->on_expr,
                                     sj_nest->sj_on_expr);
    emb_tbl_nest->on_expr->top_level_item();
    if (emb_tbl_nest->on_expr->fix_fields_if_needed(thd,
                                                    &emb_tbl_nest->on_expr))
      goto restore_tl_and_exit;
  }
  else
  {
    /* Inject into the WHERE */
    parent_join->conds= and_items(thd, parent_join->conds, sj_nest->sj_on_expr);
    parent_join->conds->top_level_item();
    /*
      fix_fields must update the properties (e.g. st_select_lex::cond_count of
      the correct select_lex.
    */
    save_lex= thd->lex->current_select;
    thd->lex->current_select=parent_join->select_lex;
    if (parent_join->conds->fix_fields_if_needed(thd, &parent_join->conds))
      goto restore_tl_and_exit;

    thd->lex->current_select=save_lex;
    parent_join->select_lex->where= parent_join->conds;
  }

  if (subq_lex->ftfunc_list->elements)
  {
    Item_func_match *ifm;
    List_iterator_fast<Item_func_match> li(*(subq_lex->ftfunc_list));
    while ((ifm= li++))
      parent_lex->ftfunc_list->push_front(ifm, thd->mem_root);
  }

  // The subqueries were replaced for Item_int(1) earlier
  subq_pred->reset_strategy(SUBS_SEMI_JOIN);       // for subsequent executions

  parent_lex->have_merged_subqueries= TRUE;
  /* Fatal error may have been set to by fix_after_pullout() */
  DBUG_RETURN(thd->is_fatal_error);

restore_tl_and_exit:
  orig_tl->next_local= NULL;
  DBUG_RETURN(TRUE);
}


const int SUBQERY_TEMPTABLE_NAME_MAX_LEN= 20;

static void create_subquery_temptable_name(LEX_STRING *str, uint number)
{
  char *to= str->str;
  DBUG_ASSERT(number < 10000);       
  to= strmov(to, "<subquery");
  to= int10_to_str((int) number, to, 10);
  to[0]= '>';
  to[1]= 0;
  str->length= (size_t) (to - str->str)+1;
}


/*
  Convert subquery predicate into non-mergeable semi-join nest.

  TODO: 
    why does this do IN-EXISTS conversion? Can't we unify it with mergeable
    semi-joins? currently, convert_subq_to_sj() cannot fail to convert (unless
    fatal errors)

    
  RETURN 
    FALSE - Ok
    TRUE  - Fatal error
*/

static bool convert_subq_to_jtbm(JOIN *parent_join, 
                                 Item_in_subselect *subq_pred, 
                                 bool *remove_item)
{
  SELECT_LEX *parent_lex= parent_join->select_lex;
  List<TABLE_LIST> *emb_join_list= &parent_lex->top_join_list;
  TABLE_LIST *emb_tbl_nest= NULL; // will change when we learn to handle outer joins
  TABLE_LIST *tl;
  bool optimization_delayed= TRUE;
  TABLE_LIST *jtbm;
  LEX_STRING tbl_alias;
  THD *thd= parent_join->thd;
  DBUG_ENTER("convert_subq_to_jtbm");

  subq_pred->set_strategy(SUBS_MATERIALIZATION);
  subq_pred->is_jtbm_merged= TRUE;

  *remove_item= TRUE;

  if (!(tbl_alias.str= (char*)thd->calloc(SUBQERY_TEMPTABLE_NAME_MAX_LEN)) ||
      !(jtbm= alloc_join_nest(thd))) //todo: this is not a join nest!
  {
    DBUG_RETURN(TRUE);
  }

  jtbm->join_list= emb_join_list;
  jtbm->embedding= emb_tbl_nest;
  jtbm->jtbm_subselect= subq_pred;
  jtbm->nested_join= NULL;

  /* Nests do not participate in those 'chains', so: */
  /* jtbm->next_leaf= jtbm->next_local= jtbm->next_global == NULL*/
  emb_join_list->push_back(jtbm, thd->mem_root);
  
  /* 
    Inject the jtbm table into TABLE_LIST::next_leaf list, so that 
    make_join_statistics() and co. can find it.
  */
  parent_lex->leaf_tables.push_back(jtbm, thd->mem_root);

  if (subq_pred->unit->first_select()->options & OPTION_SCHEMA_TABLE)
    parent_lex->options |= OPTION_SCHEMA_TABLE;

  /*
    Same as above for TABLE_LIST::next_local chain
    (a theory: a next_local chain always starts with ::leaf_tables
     because view's tables are inserted after the view)
  */
  for (tl= (TABLE_LIST*)(parent_lex->table_list.first); tl->next_local; tl= tl->next_local)
  {}
  tl->next_local= jtbm;

  /* A theory: no need to re-connect the next_global chain */
  if (optimization_delayed)
  {
    DBUG_ASSERT(parent_join->table_count < MAX_TABLES);

    jtbm->jtbm_table_no= parent_join->table_count;

    create_subquery_temptable_name(&tbl_alias,
                                   subq_pred->unit->first_select()->select_number);
    jtbm->alias.str=    tbl_alias.str;
    jtbm->alias.length= tbl_alias.length;
    parent_join->table_count++;
    DBUG_RETURN(thd->is_fatal_error);
  }
  subselect_hash_sj_engine *hash_sj_engine=
    ((subselect_hash_sj_engine*)subq_pred->engine);
  jtbm->table= hash_sj_engine->tmp_table;

  jtbm->table->tablenr= parent_join->table_count;
  jtbm->table->map= table_map(1) << (parent_join->table_count);
  jtbm->jtbm_table_no= jtbm->table->tablenr;

  parent_join->table_count++;
  DBUG_ASSERT(parent_join->table_count < MAX_TABLES);

  Item *conds= hash_sj_engine->semi_join_conds;
  conds->fix_after_pullout(parent_lex, &conds, TRUE);

  DBUG_EXECUTE("where", print_where(conds,"SJ-EXPR", QT_ORDINARY););
  
  create_subquery_temptable_name(&tbl_alias, hash_sj_engine->materialize_join->
                                              select_lex->select_number);
  jtbm->alias.str=    tbl_alias.str;
  jtbm->alias.length= tbl_alias.length;

  parent_lex->have_merged_subqueries= TRUE;

  /* Don't unlink the child subselect, as the subquery will be used. */

  DBUG_RETURN(thd->is_fatal_error);
}


static TABLE_LIST *alloc_join_nest(THD *thd)
{
  TABLE_LIST *tbl;
  if (!(tbl= (TABLE_LIST*) thd->calloc(ALIGN_SIZE(sizeof(TABLE_LIST))+
                                       sizeof(NESTED_JOIN))))
    return NULL;
  tbl->nested_join= (NESTED_JOIN*) ((uchar*)tbl + 
                                    ALIGN_SIZE(sizeof(TABLE_LIST)));
  return tbl;
}

/*
  @Note  thd->is_fatal_error can be set in case of OOM
*/

void fix_list_after_tbl_changes(SELECT_LEX *new_parent, List<TABLE_LIST> *tlist)
{
  List_iterator<TABLE_LIST> it(*tlist);
  TABLE_LIST *table;
  while ((table= it++))
  {
    if (table->on_expr)
      table->on_expr->fix_after_pullout(new_parent, &table->on_expr, TRUE);
    if (table->nested_join)
      fix_list_after_tbl_changes(new_parent, &table->nested_join->join_list);
  }
}


static void set_emb_join_nest(List<TABLE_LIST> *tables, TABLE_LIST *emb_sj_nest)
{
  List_iterator<TABLE_LIST> it(*tables);
  TABLE_LIST *tbl;
  while ((tbl= it++))
  {
    /*
      Note: check for nested_join first. 
       derived-merged tables have tbl->table!=NULL &&
       tbl->table->reginfo==NULL.
    */
    if (tbl->nested_join)
      set_emb_join_nest(&tbl->nested_join->join_list, emb_sj_nest);
    else if (tbl->table)
      tbl->table->reginfo.join_tab->emb_sj_nest= emb_sj_nest;

  }
}

/*
  Pull tables out of semi-join nests, if possible

  SYNOPSIS
    pull_out_semijoin_tables()
      join  The join where to do the semi-join flattening

  DESCRIPTION
    Try to pull tables out of semi-join nests.
     
    PRECONDITIONS
    When this function is called, the join may have several semi-join nests
    but it is guaranteed that one semi-join nest does not contain another.
   
    ACTION
    A table can be pulled out of the semi-join nest if
     - It is a constant table, or
     - It is accessed via eq_ref(outer_tables)

    POSTCONDITIONS
     * Tables that were pulled out have JOIN_TAB::emb_sj_nest == NULL
     * Tables that were not pulled out have JOIN_TAB::emb_sj_nest pointing 
       to semi-join nest they are in.
     * Semi-join nests' TABLE_LIST::sj_inner_tables is updated accordingly

    This operation is (and should be) performed at each PS execution since
    tables may become/cease to be constant across PS reexecutions.
    
  NOTE
    Table pullout may make uncorrelated subquery correlated. Consider this
    example:
    
     ... WHERE oe IN (SELECT it1.primary_key WHERE p(it1, it2) ... ) 
    
    here table it1 can be pulled out (we have it1.primary_key=oe which gives
    us functional dependency). Once it1 is pulled out, all references to it1
    from p(it1, it2) become references to outside of the subquery and thus
    make the subquery (i.e. its semi-join nest) correlated.
    Making the subquery (i.e. its semi-join nest) correlated prevents us from
    using Materialization or LooseScan to execute it. 

  RETURN 
    0 - OK
    1 - Out of memory error
*/

int pull_out_semijoin_tables(JOIN *join)
{
  TABLE_LIST *sj_nest;
  DBUG_ENTER("pull_out_semijoin_tables");
  List_iterator<TABLE_LIST> sj_list_it(join->select_lex->sj_nests);

  /* Try pulling out of the each of the semi-joins */
  while ((sj_nest= sj_list_it++))
  {
    List_iterator<TABLE_LIST> child_li(sj_nest->nested_join->join_list);
    TABLE_LIST *tbl;
    Json_writer_object trace_wrapper(join->thd);
    Json_writer_object trace(join->thd, "semijoin_table_pullout");
    Json_writer_array trace_arr(join->thd, "pulled_out_tables");

    /*
      Don't do table pull-out for nested joins (if we get nested joins here, it
      means these are outer joins. It is theoretically possible to do pull-out
      for some of the outer tables but we don't support this currently.
    */
    bool have_join_nest_children= FALSE;

    set_emb_join_nest(&sj_nest->nested_join->join_list, sj_nest);

    while ((tbl= child_li++))
    {
      if (tbl->nested_join)
      {
        have_join_nest_children= TRUE;
        break;
      }
    }
    
    table_map pulled_tables= 0;
    table_map dep_tables= 0;
    if (have_join_nest_children)
      goto skip;

    /*
      Calculate set of tables within this semi-join nest that have
      other dependent tables
    */
    child_li.rewind();
    while ((tbl= child_li++))
    {
      TABLE *const table= tbl->table;
      if (table &&
         (table->reginfo.join_tab->dependent &
          sj_nest->nested_join->used_tables))
        dep_tables|= table->reginfo.join_tab->dependent;
    }

    /* Action #1: Mark the constant tables to be pulled out */
    child_li.rewind();
    while ((tbl= child_li++))
    {
      if (tbl->table)
      {
        tbl->table->reginfo.join_tab->emb_sj_nest= sj_nest;
#if 0 
        /* 
          Do not pull out tables because they are constant. This operation has
          a problem:
          - Some constant tables may become/cease to be constant across PS
            re-executions
          - Contrary to our initial assumption, it turned out that table pullout 
            operation is not easily undoable.

          The solution is to leave constant tables where they are. This will
          affect only constant tables that are 1-row or empty, tables that are
          constant because they are accessed via eq_ref(const) access will
          still be pulled out as functionally-dependent.

          This will cause us to miss the chance to flatten some of the 
          subqueries, but since const tables do not generate many duplicates,
          it really doesn't matter that much whether they were pulled out or
          not.

          All of this was done as fix for BUG#43768.
        */
        if (tbl->table->map & join->const_table_map)
        {
          pulled_tables |= tbl->table->map;
          DBUG_PRINT("info", ("Table %s pulled out (reason: constant)",
                              tbl->table->alias));
        }
#endif
      }
    }
    
    /*
      Action #2: Find which tables we can pull out based on
      update_ref_and_keys() data. Note that pulling one table out can allow
      us to pull out some other tables too.
    */
    bool pulled_a_table;
    do 
    {
      pulled_a_table= FALSE;
      child_li.rewind();
      while ((tbl= child_li++))
      {
        if (tbl->table && !(pulled_tables & tbl->table->map) &&
            !(dep_tables & tbl->table->map))
        {
          if (find_eq_ref_candidate(tbl->table, 
                                    sj_nest->nested_join->used_tables & 
                                    ~pulled_tables))
          {
            pulled_a_table= TRUE;
            pulled_tables |= tbl->table->map;
            DBUG_PRINT("info", ("Table %s pulled out (reason: func dep)",
                                tbl->table->alias.c_ptr_safe()));
            trace_arr.add(tbl->table->alias.c_ptr_safe());
            /*
              Pulling a table out of uncorrelated subquery in general makes
              makes it correlated. See the NOTE to this funtion. 
            */
            sj_nest->sj_subq_pred->is_correlated= TRUE;
            sj_nest->nested_join->sj_corr_tables|= tbl->table->map;
            sj_nest->nested_join->sj_depends_on|= tbl->table->map;
          }
        }
      }
    } while (pulled_a_table);
 
    child_li.rewind();
  skip:
    /*
      Action #3: Move the pulled out TABLE_LIST elements to the parents.
    */
    table_map inner_tables= sj_nest->nested_join->used_tables & 
                            ~pulled_tables;
    /* Record the bitmap of inner tables */
    sj_nest->sj_inner_tables= inner_tables;
    if (pulled_tables)
    {
      List<TABLE_LIST> *upper_join_list= (sj_nest->embedding != NULL)?
                                           (&sj_nest->embedding->nested_join->join_list): 
                                           (&join->select_lex->top_join_list);
      Query_arena *arena, backup;
      arena= join->thd->activate_stmt_arena_if_needed(&backup);
      while ((tbl= child_li++))
      {
        if (tbl->table)
        {
          if (inner_tables & tbl->table->map)
          {
            /* This table is not pulled out */
            tbl->table->reginfo.join_tab->emb_sj_nest= sj_nest;
          }
          else
          {
            /* This table has been pulled out of the semi-join nest */
            tbl->table->reginfo.join_tab->emb_sj_nest= NULL;
            /*
              Pull the table up in the same way as simplify_joins() does:
              update join_list and embedding pointers but keep next[_local]
              pointers.
            */
            child_li.remove();
            sj_nest->nested_join->used_tables &= ~tbl->table->map;
            upper_join_list->push_back(tbl, join->thd->mem_root);
            tbl->join_list= upper_join_list;
            tbl->embedding= sj_nest->embedding;
          }
        }
      }

      /* Remove the sj-nest itself if we've removed everything from it */
      if (!inner_tables)
      {
        List_iterator<TABLE_LIST> li(*upper_join_list);
        /* Find the sj_nest in the list. */
        while (sj_nest != li++) ;
        li.remove();
        /* Also remove it from the list of SJ-nests: */
        sj_list_it.remove();
      }

      if (arena)
        join->thd->restore_active_arena(arena, &backup);
    }
  }
  DBUG_RETURN(0);
}


/* 
  Optimize semi-join nests that could be run with sj-materialization

  SYNOPSIS
    optimize_semijoin_nests()
      join           The join to optimize semi-join nests for
      all_table_map  Bitmap of all tables in the join

  DESCRIPTION
    Optimize each of the semi-join nests that can be run with
    materialization. For each of the nests, we
     - Generate the best join order for this "sub-join" and remember it;
     - Remember the sub-join execution cost (it's part of materialization
       cost);
     - Calculate other costs that will be incurred if we decide 
       to use materialization strategy for this semi-join nest.

    All obtained information is saved and will be used by the main join
    optimization pass.
  
  NOTES 
    Because of Join::reoptimize(), this function may be called multiple times.

  RETURN
    FALSE  Ok 
    TRUE   Out of memory error
*/

bool optimize_semijoin_nests(JOIN *join, table_map all_table_map)
{
  DBUG_ENTER("optimize_semijoin_nests");
  THD *thd= join->thd;
  List_iterator<TABLE_LIST> sj_list_it(join->select_lex->sj_nests);
  TABLE_LIST *sj_nest;
  if (!join->select_lex->sj_nests.elements)
    DBUG_RETURN(FALSE);
  Json_writer_object wrapper(thd);
  Json_writer_object trace_semijoin_nest(thd,
                              "execution_plan_for_potential_materialization");
  Json_writer_array trace_steps_array(thd, "steps");
  while ((sj_nest= sj_list_it++))
  {
    /* semi-join nests with only constant tables are not valid */
   /// DBUG_ASSERT(sj_nest->sj_inner_tables & ~join->const_table_map);

    sj_nest->sj_mat_info= NULL;
    /*
      The statement may have been executed with 'semijoin=on' earlier.
      We need to verify that 'semijoin=on' still holds.
     */
    if (optimizer_flag(join->thd, OPTIMIZER_SWITCH_SEMIJOIN) &&
        optimizer_flag(join->thd, OPTIMIZER_SWITCH_MATERIALIZATION))
    {
      if ((sj_nest->sj_inner_tables  & ~join->const_table_map) && /* not everything was pulled out */
          !sj_nest->sj_subq_pred->is_correlated && 
           sj_nest->sj_subq_pred->types_allow_materialization)
      {
        join->emb_sjm_nest= sj_nest;
        if (choose_plan(join, all_table_map &~join->const_table_map))
          DBUG_RETURN(TRUE); /* purecov: inspected */
        /*
          The best plan to run the subquery is now in join->best_positions,
          save it.
        */
        uint n_tables= my_count_bits(sj_nest->sj_inner_tables & ~join->const_table_map);
        SJ_MATERIALIZATION_INFO* sjm;
        if (!(sjm= new SJ_MATERIALIZATION_INFO) ||
            !(sjm->positions= (POSITION*)join->thd->alloc(sizeof(POSITION)*
                                                          n_tables)))
          DBUG_RETURN(TRUE); /* purecov: inspected */
        sjm->tables= n_tables;
        sjm->is_used= FALSE;
        double subjoin_out_rows, subjoin_read_time;

        /*
        join->get_partial_cost_and_fanout(n_tables + join->const_tables,
                                          table_map(-1),
                                          &subjoin_read_time, 
                                          &subjoin_out_rows);
        */
        join->get_prefix_cost_and_fanout(n_tables, 
                                         &subjoin_read_time,
                                         &subjoin_out_rows);

        sjm->materialization_cost.convert_from_cost(subjoin_read_time);
        sjm->rows_with_duplicates= sjm->rows= subjoin_out_rows;
        
        // Don't use the following list because it has "stale" items. use
        // ref_pointer_array instead:
        //
        //List<Item> &right_expr_list= 
        //  sj_nest->sj_subq_pred->unit->first_select()->item_list;
        /*
          Adjust output cardinality estimates. If the subquery has form

           ... oe IN (SELECT t1.colX, t2.colY, func(X,Y,Z) )

           then the number of distinct output record combinations has an
           upper bound of product of number of records matching the tables 
           that are used by the SELECT clause.
           TODO:
             We can get a more precise estimate if we
              - use rec_per_key cardinality estimates. For simple cases like 
                "oe IN (SELECT t.key ...)" it is trivial. 
              - Functional dependencies between the tables in the semi-join
                nest (the payoff is probably less here?)
          
          See also get_post_group_estimate().
        */
        SELECT_LEX *subq_select= sj_nest->sj_subq_pred->unit->first_select();
        {
          for (uint i=0 ; i < join->const_tables + sjm->tables ; i++)
          {
            JOIN_TAB *tab= join->best_positions[i].table;
            join->map2table[tab->table->tablenr]= tab;
          }
          table_map map= 0;
          for (uint i=0; i < subq_select->item_list.elements; i++)
            map|= subq_select->ref_pointer_array[i]->used_tables();
          map= map & ~PSEUDO_TABLE_BITS;
          Table_map_iterator tm_it(map);
          int tableno;
          double rows= 1.0;
          while ((tableno = tm_it.next_bit()) != Table_map_iterator::BITMAP_END)
            rows= COST_MULT(rows,
			    join->map2table[tableno]->table->opt_range_condition_rows);
          sjm->rows= MY_MIN(sjm->rows, rows);
        }
        memcpy((uchar*) sjm->positions,
               (uchar*) (join->best_positions + join->const_tables),
               sizeof(POSITION) * n_tables);

        /*
          Calculate temporary table parameters and usage costs
        */
        uint rowlen= get_tmp_table_rec_length(subq_select->ref_pointer_array,
                                              subq_select->item_list.elements);
        double lookup_cost= get_tmp_table_lookup_cost(join->thd,
                                                      subjoin_out_rows, rowlen);
        double write_cost= get_tmp_table_write_cost(join->thd,
                                                    subjoin_out_rows, rowlen);

        /*
          Let materialization cost include the cost to write the data into the
          temporary table:
        */ 
        sjm->materialization_cost.add_io(subjoin_out_rows, write_cost);
        
        /*
          Set the cost to do a full scan of the temptable (will need this to 
          consider doing sjm-scan):
        */ 
        sjm->scan_cost.reset();
        sjm->scan_cost.add_io(sjm->rows, lookup_cost);

        sjm->lookup_cost.convert_from_cost(lookup_cost);
        sj_nest->sj_mat_info= sjm;
        DBUG_EXECUTE("opt", print_sjm(sjm););
      }
    }
  }
  join->emb_sjm_nest= NULL;
  DBUG_RETURN(FALSE);
}


/*
  Get estimated record length for semi-join materialization temptable
  
  SYNOPSIS
    get_tmp_table_rec_length()
      items  IN subquery's select list.

  DESCRIPTION
    Calculate estimated record length for semi-join materialization
    temptable. It's an estimate because we don't follow every bit of
    create_tmp_table()'s logic. This isn't necessary as the return value of
    this function is used only for cost calculations.

  RETURN
    Length of the temptable record, in bytes
*/

static uint get_tmp_table_rec_length(Ref_ptr_array p_items, uint elements)
{
  uint len= 0;
  Item *item;
  //List_iterator<Item> it(items);
  for (uint i= 0; i < elements ; i++)
  {
    item = p_items[i];
    switch (item->result_type()) {
    case REAL_RESULT:
      len += sizeof(double);
      break;
    case INT_RESULT:
      if (item->max_length >= (MY_INT32_NUM_DECIMAL_DIGITS - 1))
        len += 8;
      else
        len += 4;
      break;
    case STRING_RESULT:
      enum enum_field_types type;
      /* DATE/TIME and GEOMETRY fields have STRING_RESULT result type.  */
      if ((type= item->field_type()) == MYSQL_TYPE_DATETIME ||
          type == MYSQL_TYPE_TIME || type == MYSQL_TYPE_DATE ||
          type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_GEOMETRY)
        len += 8;
      else
        len += item->max_length;
      break;
    case DECIMAL_RESULT:
      len += 10;
      break;
    case ROW_RESULT:
    default:
      DBUG_ASSERT(0); /* purecov: deadcode */
      break;
    }
  }
  return len;
}


/**
  The cost of a lookup into a unique hash/btree index on a temporary table
  with 'row_count' rows each of size 'row_size'.

  @param thd  current query context
  @param row_count  number of rows in the temp table
  @param row_size   average size in bytes of the rows

  @return  the cost of one lookup
*/

double
get_tmp_table_lookup_cost(THD *thd, double row_count, uint row_size)
{
  if (row_count > thd->variables.max_heap_table_size / (double) row_size)
    return (double) DISK_TEMPTABLE_LOOKUP_COST;
  else
    return (double) HEAP_TEMPTABLE_LOOKUP_COST;
}

/**
  The cost of writing a row into a temporary table with 'row_count' unique
  rows each of size 'row_size'.

  @param thd  current query context
  @param row_count  number of rows in the temp table
  @param row_size   average size in bytes of the rows

  @return  the cost of writing one row
*/

double
get_tmp_table_write_cost(THD *thd, double row_count, uint row_size)
{
  double lookup_cost= get_tmp_table_lookup_cost(thd, row_count, row_size);
  /*
    TODO:
    This is an optimistic estimate. Add additional costs resulting from
    actually writing the row to memory/disk and possible index reorganization.
  */
  return lookup_cost;
}


/*
  Check if table's KEYUSE elements have an eq_ref(outer_tables) candidate

  SYNOPSIS
    find_eq_ref_candidate()
      table             Table to be checked
      sj_inner_tables   Bitmap of inner tables. eq_ref(inner_table) doesn't
                        count.

  DESCRIPTION
    Check if table's KEYUSE elements have an eq_ref(outer_tables) candidate

  TODO
    Check again if it is feasible to factor common parts with constant table
    search

    Also check if it's feasible to factor common parts with table elimination

  RETURN
    TRUE  - There exists an eq_ref(outer-tables) candidate
    FALSE - Otherwise
*/

bool find_eq_ref_candidate(TABLE *table, table_map sj_inner_tables)
{
  KEYUSE *keyuse= table->reginfo.join_tab->keyuse;

  if (keyuse)
  {
    do
    {
      uint key= keyuse->key;
      key_part_map bound_parts= 0;
      if (!keyuse->is_for_hash_join() &&
          (table->key_info[key].flags & HA_NOSAME))
      {
        KEY *keyinfo= table->key_info + key;
        do  /* For all equalities on all key parts */
        {
          /*
            Check if this is "t.keypart = expr(outer_tables)

            Don't allow variants that can produce duplicates:
            - Dont allow "ref or null"
            - the keyuse (that is, the operation) must be null-rejecting,
              unless the other expression is non-NULLable.
          */
          if (!(keyuse->used_tables & sj_inner_tables) &&
              !(keyuse->optimize & KEY_OPTIMIZE_REF_OR_NULL) &&
              (keyuse->null_rejecting || !keyuse->val->maybe_null()))
          {
            bound_parts |= 1 << keyuse->keypart;
          }
          keyuse++;
        } while (keyuse->key == key && keyuse->table == table);

        if (bound_parts == PREV_BITS(uint, keyinfo->user_defined_key_parts))
          return TRUE;
      }
      else
      {
        do
        {
          keyuse++;
        } while (keyuse->key == key && keyuse->table == table);
      }
    } while (keyuse->table == table);
  }
  return FALSE;
}


/*
  Do semi-join optimization step after we've added a new tab to join prefix

  SYNOPSIS
    advance_sj_state()
      join                        The join we're optimizing
      remaining_tables            Tables not in the join prefix
      new_join_tab                Join tab we've just added to the join prefix
      idx                         Index of this join tab (i.e. number of tables
                                  in the prefix minus one)
      current_record_count INOUT  Estimate of #records in join prefix's output
      current_read_time    INOUT  Cost to execute the join prefix
      loose_scan_pos       IN     A POSITION with LooseScan plan to access 
                                  table new_join_tab
                                  (produced by the last best_access_path call)

  DESCRIPTION
    Update semi-join optimization state after we've added another tab (table 
    and access method) to the join prefix.
    
    The state is maintained in join->positions[#prefix_size]. Each of the
    available strategies has its own state variables.
    
    for each semi-join strategy
    {
      update strategy's state variables;

      if (join prefix has all the tables that are needed to consider
          using this strategy for the semi-join(s))
      {
        calculate cost of using the strategy
        if ((this is the first strategy to handle the semi-join nest(s)  ||
            the cost is less than other strategies))
        {
          // Pick this strategy
          pos->sj_strategy= ..
          ..
        }
      }

    Most of the new state is saved join->positions[idx] (and hence no undo
    is necessary). Several members of class JOIN are updated also, these
    changes can be rolled back with restore_prev_sj_state().

    See setup_semijoin_dups_elimination() for a description of what kinds of
    join prefixes each strategy can handle.
*/

bool is_multiple_semi_joins(JOIN *join, POSITION *prefix, uint idx, table_map inner_tables)
{
  for (int i= (int)idx; i >= 0; i--)
  {
    TABLE_LIST *emb_sj_nest;
    if ((emb_sj_nest= prefix[i].table->emb_sj_nest))
    {
      if (inner_tables & emb_sj_nest->sj_inner_tables)
        return !MY_TEST(inner_tables == (emb_sj_nest->sj_inner_tables &
                                         ~join->const_table_map));
    }
  }
  return FALSE;
}


void advance_sj_state(JOIN *join, table_map remaining_tables, uint idx, 
                      double *current_record_count, double *current_read_time,
                      POSITION *loose_scan_pos)
{
  POSITION *pos= join->positions + idx;
  const JOIN_TAB *new_join_tab= pos->table; 

#ifdef HAVE_valgrind
  new (&pos->firstmatch_picker) Firstmatch_picker;
  new (&pos->loosescan_picker) LooseScan_picker;
  new (&pos->sjmat_picker) Sj_materialization_picker;
  new (&pos->dups_weedout_picker) Duplicate_weedout_picker;
#endif

  if (join->emb_sjm_nest || //(1)
      !join->select_lex->have_merged_subqueries) //(2)
  {
    /* 
      (1): We're performing optimization inside SJ-Materialization nest:
       - there are no other semi-joins inside semi-join nests
       - attempts to build semi-join strategies here will confuse
         the optimizer, so bail out.
      (2): Don't waste time on semi-join optimizations if we don't have any
           semi-joins
    */
    pos->sj_strategy= SJ_OPT_NONE;
    return;
  }

  Semi_join_strategy_picker *pickers[]=
  {
    &pos->firstmatch_picker,
    &pos->loosescan_picker,
    &pos->sjmat_picker,
    &pos->dups_weedout_picker,
    NULL,
  };
  Json_writer_array trace_steps(join->thd, "semijoin_strategy_choice");
  /* 
    Update join->cur_sj_inner_tables (Used by FirstMatch in this function and
    LooseScan detector in best_access_path)
  */
  remaining_tables &= ~new_join_tab->table->map;
  table_map dups_producing_tables, UNINIT_VAR(prev_dups_producing_tables),
    UNINIT_VAR(prev_sjm_lookup_tables);

  if (idx == join->const_tables)
    dups_producing_tables= 0;
  else
    dups_producing_tables= pos[-1].dups_producing_tables;

  TABLE_LIST *emb_sj_nest;
  if ((emb_sj_nest= new_join_tab->emb_sj_nest))
    dups_producing_tables |= emb_sj_nest->sj_inner_tables;

  Semi_join_strategy_picker **strategy, **prev_strategy= 0;
  if (idx == join->const_tables)
  {
    /* First table, initialize pickers */
    for (strategy= pickers; *strategy != NULL; strategy++)
      (*strategy)->set_empty();
    pos->inner_tables_handled_with_other_sjs= 0;
  }
  else
  {
    for (strategy= pickers; *strategy != NULL; strategy++)
    {
      (*strategy)->set_from_prev(pos - 1);
    }
    pos->inner_tables_handled_with_other_sjs=
       pos[-1].inner_tables_handled_with_other_sjs;
  }

  pos->prefix_cost.convert_from_cost(*current_read_time);
  pos->prefix_record_count= *current_record_count;

  {
    pos->sj_strategy= SJ_OPT_NONE;

    for (strategy= pickers; *strategy != NULL; strategy++)
    {
      table_map handled_fanout;
      sj_strategy_enum sj_strategy;
      double rec_count= *current_record_count;
      double read_time= *current_read_time;
      if ((*strategy)->check_qep(join, idx, remaining_tables, 
                                 new_join_tab,
                                 &rec_count,
                                 &read_time,
                                 &handled_fanout,
                                 &sj_strategy,
                                 loose_scan_pos))
      {
        /*
          It's possible to use the strategy. Use it, if
           - it removes semi-join fanout that was not removed before
           - using it is cheaper than using something else,
               and {if some other strategy has removed fanout
               that this strategy is trying to remove, then it
               did remove the fanout only for one semi-join}
               This is to avoid a situation when
                1. strategy X removes fanout for semijoin X,Y
                2. using strategy Z is cheaper, but it only removes
                   fanout from semijoin X.
                3. We have no clue what to do about fanount of semi-join Y.
        */
        if ((dups_producing_tables & handled_fanout) ||
            (read_time < *current_read_time &&
             !(handled_fanout & pos->inner_tables_handled_with_other_sjs)))
        {
          DBUG_ASSERT(pos->sj_strategy != sj_strategy);
          /*
            If the strategy chosen first time or
            the strategy replace strategy which was used to exectly the same
            tables
          */
          if (pos->sj_strategy == SJ_OPT_NONE ||
              handled_fanout ==
                (prev_dups_producing_tables ^ dups_producing_tables))
          {
            prev_strategy= strategy;
            if (pos->sj_strategy == SJ_OPT_NONE)
            {
              prev_dups_producing_tables= dups_producing_tables;
              prev_sjm_lookup_tables= join->sjm_lookup_tables;
            }
            /* Mark strategy as used */
            (*strategy)->mark_used();
            pos->sj_strategy= sj_strategy;
            if (sj_strategy == SJ_OPT_MATERIALIZE)
              join->sjm_lookup_tables |= handled_fanout;
            else
              join->sjm_lookup_tables &= ~handled_fanout;
            *current_read_time= read_time;
            *current_record_count= rec_count;
            dups_producing_tables &= ~handled_fanout;

            //TODO: update bitmap of semi-joins that were handled together with
            // others.
            if (is_multiple_semi_joins(join, join->positions, idx,
                                       handled_fanout))
              pos->inner_tables_handled_with_other_sjs |= handled_fanout;
          }
          else
          {
            /* Conflict fall to most general variant */
            (*prev_strategy)->set_empty();
            dups_producing_tables= prev_dups_producing_tables;
            join->sjm_lookup_tables= prev_sjm_lookup_tables;
            // mark it 'none' to avpoid loops
            pos->sj_strategy= SJ_OPT_NONE;
            // next skip to last;
            strategy= pickers +
              (sizeof(pickers)/sizeof(Semi_join_strategy_picker*) - 3);
            continue;
          }
        }
        else
        {
          /* We decided not to apply the strategy. */
          (*strategy)->set_empty();
        }
      }
    }

    if (unlikely(join->thd->trace_started() && pos->sj_strategy != SJ_OPT_NONE))
    {
      Json_writer_object tr(join->thd);
      const char *sname;
      switch (pos->sj_strategy) {
        case SJ_OPT_MATERIALIZE:
          sname= "SJ-Materialization";
          break;
        case SJ_OPT_MATERIALIZE_SCAN:
          sname= "SJ-Materialization-Scan";
          break;
        case SJ_OPT_FIRST_MATCH:
          sname= "FirstMatch";
          break;
        case SJ_OPT_DUPS_WEEDOUT:
          sname= "DuplicateWeedout";
          break;
        case SJ_OPT_LOOSE_SCAN:
          sname= "LooseScan";
          break;
        default:
          DBUG_ASSERT(0);
          sname="Invalid";
      }
      tr.add("chosen_strategy", sname);
    }
  }

  if ((emb_sj_nest= new_join_tab->emb_sj_nest))
  {
    join->cur_sj_inner_tables |= emb_sj_nest->sj_inner_tables;

    /* Remove the sj_nest if all of its SJ-inner tables are in cur_table_map */
    if (!(remaining_tables &
          emb_sj_nest->sj_inner_tables & ~new_join_tab->table->map))
      join->cur_sj_inner_tables &= ~emb_sj_nest->sj_inner_tables;
  }

  pos->prefix_cost.convert_from_cost(*current_read_time);
  pos->prefix_record_count= *current_record_count;
  pos->dups_producing_tables= dups_producing_tables;
}


void Sj_materialization_picker::set_from_prev(POSITION *prev)
{
  if (prev->sjmat_picker.is_used)
    set_empty();
  else
  {
    sjm_scan_need_tables= prev->sjmat_picker.sjm_scan_need_tables; 
    sjm_scan_last_inner=  prev->sjmat_picker.sjm_scan_last_inner;
  }
  is_used= FALSE;
}


bool Sj_materialization_picker::check_qep(JOIN *join,
                                          uint idx,
                                          table_map remaining_tables, 
                                          const JOIN_TAB *new_join_tab,
                                          double *record_count,
                                          double *read_time,
                                          table_map *handled_fanout,
                                          sj_strategy_enum *strategy,
                                          POSITION *loose_scan_pos)
{
  bool sjm_scan;
  SJ_MATERIALIZATION_INFO *mat_info;
  THD *thd= join->thd;
  if ((mat_info= at_sjmat_pos(join, remaining_tables,
                              new_join_tab, idx, &sjm_scan)))
  {
    if (sjm_scan)
    {
      /*
        We can't yet evaluate this option yet. This is because we can't
        accout for fanout of sj-inner tables yet:

          ntX  SJM-SCAN(it1 ... itN) | ot1 ... otN  |
                                     ^(1)           ^(2)

        we're now at position (1). SJM temptable in general has multiple
        records, so at point (1) we'll get the fanout from sj-inner tables (ie
        there will be multiple record combinations).

        The final join result will not contain any semi-join produced
        fanout, i.e. tables within SJM-SCAN(...) will not contribute to
        the cardinality of the join output.  Extra fanout produced by 
        SJM-SCAN(...) will be 'absorbed' into fanout produced by ot1 ...  otN.

        The simple way to model this is to remove SJM-SCAN(...) fanout once
        we reach the point #2.
      */
      sjm_scan_need_tables=
        new_join_tab->emb_sj_nest->sj_inner_tables | 
        new_join_tab->emb_sj_nest->nested_join->sj_depends_on |
        new_join_tab->emb_sj_nest->nested_join->sj_corr_tables;
      sjm_scan_last_inner= idx;
    }
    else
    {
      /* This is SJ-Materialization with lookups */
      Cost_estimate prefix_cost; 
      signed int first_tab= (int)idx - mat_info->tables;
      double prefix_rec_count;
      Json_writer_object trace(join->thd);
      trace.add("strategy", "SJ-Materialization");

      if (first_tab < (int)join->const_tables)
      {
        prefix_cost.reset();
        prefix_rec_count= 1.0;
      }
      else
      {
        prefix_cost= join->positions[first_tab].prefix_cost;
        prefix_rec_count= join->positions[first_tab].prefix_record_count;
      }

      double mat_read_time= prefix_cost.total_cost();
      mat_read_time=
        COST_ADD(mat_read_time,
                 COST_ADD(mat_info->materialization_cost.total_cost(),
                          COST_MULT(prefix_rec_count,
                                    mat_info->lookup_cost.total_cost())));

      /*
        NOTE: When we pick to use SJM[-Scan] we don't memcpy its POSITION
        elements to join->positions as that makes it hard to return things
        back when making one step back in join optimization. That's done 
        after the QEP has been chosen.
      */
      *read_time=    mat_read_time;
      *record_count= prefix_rec_count;
      *handled_fanout= new_join_tab->emb_sj_nest->sj_inner_tables;
      *strategy= SJ_OPT_MATERIALIZE;
      if (unlikely(trace.trace_started()))
      {
        trace.add("records", *record_count);
        trace.add("read_time", *read_time);
      }
      return TRUE;
    }
  }
  
  /* 4.A SJM-Scan second phase check */
  if (sjm_scan_need_tables && /* Have SJM-Scan prefix */
      !(sjm_scan_need_tables & remaining_tables))
  {
    Json_writer_object trace(join->thd);
    trace.add("strategy", "SJ-Materialization-Scan");
    TABLE_LIST *mat_nest= 
      join->positions[sjm_scan_last_inner].table->emb_sj_nest;
    SJ_MATERIALIZATION_INFO *mat_info= mat_nest->sj_mat_info;

    double prefix_cost;
    double prefix_rec_count;
    int first_tab= sjm_scan_last_inner + 1 - mat_info->tables;
    /* Get the prefix cost */
    if (first_tab == (int)join->const_tables)
    {
      prefix_rec_count= 1.0;
      prefix_cost= 0.0;
    }
    else
    {
      prefix_cost= join->positions[first_tab - 1].prefix_cost.total_cost();
      prefix_rec_count= join->positions[first_tab - 1].prefix_record_count;
    }

    /* Add materialization cost */
    prefix_cost=
      COST_ADD(prefix_cost,
               COST_ADD(mat_info->materialization_cost.total_cost(),
                        COST_MULT(prefix_rec_count,
                                  mat_info->scan_cost.total_cost())));
    prefix_rec_count= COST_MULT(prefix_rec_count, mat_info->rows);
    
    uint i;
    table_map rem_tables= remaining_tables;
    for (i= idx; i != (first_tab + mat_info->tables - 1); i--)
      rem_tables |= join->positions[i].table->table->map;

    POSITION curpos, dummy;
    /* Need to re-run best-access-path as we prefix_rec_count has changed */
    bool disable_jbuf= (join->thd->variables.join_cache_level == 0);
    Json_writer_temp_disable trace_semijoin_mat_scan(thd);
    for (i= first_tab + mat_info->tables; i <= idx; i++)
    {
      best_access_path(join, join->positions[i].table, rem_tables,
                       join->positions, i,
                       disable_jbuf, prefix_rec_count, &curpos, &dummy);
      prefix_rec_count= COST_MULT(prefix_rec_count, curpos.records_read);
      prefix_cost= COST_ADD(prefix_cost, curpos.read_time);
      prefix_cost= COST_ADD(prefix_cost,
                            prefix_rec_count / TIME_FOR_COMPARE);
      //TODO: take into account join condition selectivity here
    }

    *strategy= SJ_OPT_MATERIALIZE_SCAN;
    *read_time=    prefix_cost;
    /*
      Note: the next line means we did not remove the subquery's fanout from
      *record_count. It needs to be removed, as the join prefix is

        ntX  SJM-SCAN(it1 ... itN) | (ot1 ... otN) ...

      here, the SJM-SCAN may have introduced subquery's fanout (duplicate rows,
      rows that don't have matches in ot1_i). All this fanout is gone after
      table otN (or earlier) but taking it into account is hard.

      Some consolation here is that SJM-Scan strategy is applicable when the
      subquery is smaller than tables otX. If the subquery has large cardinality,
      we can greatly overestimate *record_count here, but it doesn't matter as
      SJ-Materialization-Lookup is a better strategy anyway.
    */
    *record_count= prefix_rec_count;
    *handled_fanout= mat_nest->sj_inner_tables;
    if (unlikely(trace.trace_started()))
    {
      trace.add("records", *record_count);
      trace.add("read_time", *read_time);
    }
    return TRUE;
  }
  return FALSE;
}


void LooseScan_picker::set_from_prev(POSITION *prev)
{
  if (prev->loosescan_picker.is_used)
    set_empty();
  else
  {
    first_loosescan_table= prev->loosescan_picker.first_loosescan_table;
    loosescan_need_tables= prev->loosescan_picker.loosescan_need_tables;
  }
  is_used= FALSE;
}


bool LooseScan_picker::check_qep(JOIN *join,
                                 uint idx,
                                 table_map remaining_tables, 
                                 const JOIN_TAB *new_join_tab,
                                 double *record_count, 
                                 double *read_time,
                                 table_map *handled_fanout,
                                 sj_strategy_enum *strategy,
                                 POSITION *loose_scan_pos)
{
  POSITION *first= join->positions + first_loosescan_table; 
  /* 
    LooseScan strategy can't handle interleaving between tables from the 
    semi-join that LooseScan is handling and any other tables.

    If we were considering LooseScan for the join prefix (1)
       and the table we're adding creates an interleaving (2)
    then 
       stop considering loose scan
  */
  if ((first_loosescan_table != MAX_TABLES) &&   // (1)
      (first->table->emb_sj_nest->sj_inner_tables & remaining_tables) && //(2)
      new_join_tab->emb_sj_nest != first->table->emb_sj_nest) //(2)
  {
    first_loosescan_table= MAX_TABLES;
  }

  /*
    If we got an option to use LooseScan for the current table, start
    considering using LooseScan strategy
  */
  if (loose_scan_pos->read_time != DBL_MAX && !join->outer_join)
  {
    first_loosescan_table= idx;
    loosescan_need_tables=
      new_join_tab->emb_sj_nest->sj_inner_tables | 
      new_join_tab->emb_sj_nest->nested_join->sj_depends_on |
      new_join_tab->emb_sj_nest->nested_join->sj_corr_tables;
  }
  
  if ((first_loosescan_table != MAX_TABLES) && 
      !(remaining_tables & loosescan_need_tables) &&
      (new_join_tab->table->map & loosescan_need_tables))
  {
    Json_writer_object trace(join->thd);
    trace.add("strategy", "LooseScan");
    /* 
      Ok we have LooseScan plan and also have all LooseScan sj-nest's
      inner tables and outer correlated tables into the prefix.
    */

    first= join->positions + first_loosescan_table; 
    uint n_tables= my_count_bits(first->table->emb_sj_nest->sj_inner_tables);
    /* Got a complete LooseScan range. Calculate its cost */
    /*
      The same problem as with FirstMatch - we need to save POSITIONs
      somewhere but reserving space for all cases would require too
      much space. We will re-calculate POSITION structures later on. 
    */
    bool disable_jbuf= (join->thd->variables.join_cache_level == 0);
    optimize_wo_join_buffering(join, first_loosescan_table, idx,
                               remaining_tables, 
                               TRUE,  //first_alt
                               disable_jbuf ? join->table_count :
                                 first_loosescan_table + n_tables,
                               record_count,
                               read_time);
    /*
      We don't yet have any other strategies that could handle this
      semi-join nest (the other options are Duplicate Elimination or
      Materialization, which need at least the same set of tables in 
      the join prefix to be considered) so unconditionally pick the 
      LooseScan.
    */
    *strategy= SJ_OPT_LOOSE_SCAN;
    *handled_fanout= first->table->emb_sj_nest->sj_inner_tables;
    if (unlikely(trace.trace_started()))
    {
      trace.add("records", *record_count);
      trace.add("read_time", *read_time);
    }
    return TRUE;
  }
  return FALSE;
}

void Firstmatch_picker::set_from_prev(POSITION *prev)
{
  if (prev->firstmatch_picker.is_used)
    invalidate_firstmatch_prefix();
  else
  {
    first_firstmatch_table= prev->firstmatch_picker.first_firstmatch_table;
    first_firstmatch_rtbl=  prev->firstmatch_picker.first_firstmatch_rtbl;
    firstmatch_need_tables= prev->firstmatch_picker.firstmatch_need_tables;
  }
  is_used= FALSE;
}

bool Firstmatch_picker::check_qep(JOIN *join,
                                  uint idx,
                                  table_map remaining_tables, 
                                  const JOIN_TAB *new_join_tab,
                                  double *record_count,
                                  double *read_time,
                                  table_map *handled_fanout,
                                  sj_strategy_enum *strategy,
                                  POSITION *loose_scan_pos)
{
  if (new_join_tab->emb_sj_nest &&
      optimizer_flag(join->thd, OPTIMIZER_SWITCH_FIRSTMATCH) &&
      !join->outer_join)
  {
    const table_map outer_corr_tables=
      new_join_tab->emb_sj_nest->nested_join->sj_corr_tables |
      new_join_tab->emb_sj_nest->nested_join->sj_depends_on;
    const table_map sj_inner_tables=
      new_join_tab->emb_sj_nest->sj_inner_tables & ~join->const_table_map;

    /* 
      Enter condition:
       1. The next join tab belongs to semi-join nest
          (verified for the encompassing code block above).
       2. We're not in a duplicate producer range yet
       3. All outer tables that
           - the subquery is correlated with, or
           - referred to from the outer_expr 
          are in the join prefix
       4. All inner tables are still part of remaining_tables.
    */
    if (!join->cur_sj_inner_tables &&              // (2)
        !(remaining_tables & outer_corr_tables) && // (3)
        (sj_inner_tables ==                        // (4)
         ((remaining_tables | new_join_tab->table->map) & sj_inner_tables)))
    {
      /* Start tracking potential FirstMatch range */
      first_firstmatch_table= idx;
      firstmatch_need_tables= sj_inner_tables;
      first_firstmatch_rtbl= remaining_tables;
    }

    if (in_firstmatch_prefix())
    {
      if (outer_corr_tables & first_firstmatch_rtbl)
      {
        /*
          Trying to add an sj-inner table whose sj-nest has an outer correlated 
          table that was not in the prefix. This means FirstMatch can't be used.
        */
        invalidate_firstmatch_prefix();
      }
      else
      {
        /* Record that we need all of this semi-join's inner tables, too */
        firstmatch_need_tables|= sj_inner_tables;
      }
    
      if (in_firstmatch_prefix() && 
          !(firstmatch_need_tables & remaining_tables))
      {
        Json_writer_object trace(join->thd);
        trace.add("strategy", "FirstMatch");
        /*
          Got a complete FirstMatch range. Calculate correct costs and fanout
        */

        if (idx == first_firstmatch_table && 
            optimizer_flag(join->thd, OPTIMIZER_SWITCH_SEMIJOIN_WITH_CACHE))
        {
          /* 
            An important special case: only one inner table, and @@optimizer_switch
            allows join buffering.
             - read_time is the same (i.e. FirstMatch doesn't add any cost
             - remove fanout added by the last table
          */
          if (*record_count)
            *record_count /= join->positions[idx].records_read;
        }
        else
        {
          optimize_wo_join_buffering(join, first_firstmatch_table, idx,
                                     remaining_tables, FALSE, idx,
                                     record_count, 
                                     read_time);
        }
        /*
          We ought to save the alternate POSITIONs produced by
          optimize_wo_join_buffering but the problem is that providing save
          space uses too much space. Instead, we will re-calculate the
          alternate POSITIONs after we've picked the best QEP.
        */
        *handled_fanout= firstmatch_need_tables;
        /* *record_count and *read_time were set by the above call */
        *strategy= SJ_OPT_FIRST_MATCH;
        if (unlikely(trace.trace_started()))
        {
          trace.add("records", *record_count);
          trace.add("read_time", *read_time);
        }
        return TRUE;
      }
    }
  }
  else
    invalidate_firstmatch_prefix();
  return FALSE;
}


void Duplicate_weedout_picker::set_from_prev(POSITION *prev)
{
  if (prev->dups_weedout_picker.is_used)
    set_empty();
  else
  {
    dupsweedout_tables=      prev->dups_weedout_picker.dupsweedout_tables;
    first_dupsweedout_table= prev->dups_weedout_picker.first_dupsweedout_table;
  }
  is_used= FALSE;
}


bool Duplicate_weedout_picker::check_qep(JOIN *join,
                                         uint idx,
                                         table_map remaining_tables, 
                                         const JOIN_TAB *new_join_tab,
                                         double *record_count,
                                         double *read_time,
                                         table_map *handled_fanout,
                                         sj_strategy_enum *strategy,
                                         POSITION *loose_scan_pos
                                         )
{
  TABLE_LIST *nest;
  if ((nest= new_join_tab->emb_sj_nest))
  {
    if (!dupsweedout_tables)
      first_dupsweedout_table= idx;

    dupsweedout_tables |= nest->sj_inner_tables |
                          nest->nested_join->sj_depends_on |
                          nest->nested_join->sj_corr_tables;
  }
  
  if (dupsweedout_tables)
  {
    /* we're in the process of constructing a DuplicateWeedout range */
    TABLE_LIST *emb= new_join_tab->table->pos_in_table_list->embedding;
    /* and we've entered an inner side of an outer join*/
    if (emb && emb->on_expr)
      dupsweedout_tables |= emb->nested_join->used_tables;
  }
  
  /* If this is the last table that we need for DuplicateWeedout range */
  if (dupsweedout_tables && !(remaining_tables & ~new_join_tab->table->map &
                              dupsweedout_tables))
  {
    /*
      Ok, reached a state where we could put a dups weedout point.
      Walk back and calculate
        - the join cost (this is needed as the accumulated cost may assume 
          some other duplicate elimination method)
        - extra fanout that will be removed by duplicate elimination
        - duplicate elimination cost
      There are two cases:
        1. We have other strategy/ies to remove all of the duplicates.
        2. We don't.
      
      We need to calculate the cost in case #2 also because we need to make
      choice between this join order and others.
    */
    uint first_tab= first_dupsweedout_table;
    double dups_cost;
    double prefix_rec_count;
    double sj_inner_fanout= 1.0;
    double sj_outer_fanout= 1.0;
    uint temptable_rec_size;
    Json_writer_object trace(join->thd);
    trace.add("strategy", "DuplicateWeedout");

    if (first_tab == join->const_tables)
    {
      prefix_rec_count= 1.0;
      temptable_rec_size= 0;
      dups_cost= 0.0;
    }
    else
    {
      dups_cost= join->positions[first_tab - 1].prefix_cost.total_cost();
      prefix_rec_count= join->positions[first_tab - 1].prefix_record_count;
      temptable_rec_size= 8; /* This is not true but we'll make it so */
    }
    
    table_map dups_removed_fanout= 0;
    double current_fanout= prefix_rec_count;
    for (uint j= first_dupsweedout_table; j <= idx; j++)
    {
      POSITION *p= join->positions + j;
      current_fanout= COST_MULT(current_fanout, p->records_read);
      dups_cost= COST_ADD(dups_cost,
                          COST_ADD(p->read_time,
                                   current_fanout / TIME_FOR_COMPARE));
      if (p->table->emb_sj_nest)
      {
        sj_inner_fanout= COST_MULT(sj_inner_fanout, p->records_read);
        dups_removed_fanout |= p->table->table->map;
      }
      else
      {
        sj_outer_fanout= COST_MULT(sj_outer_fanout, p->records_read);
        temptable_rec_size += p->table->table->file->ref_length;
      }
    }

    /*
      Add the cost of temptable use. The table will have sj_outer_fanout
      records, and we will make 
      - sj_outer_fanout table writes
      - sj_inner_fanout*sj_outer_fanout  lookups.

    */
    double one_lookup_cost= get_tmp_table_lookup_cost(join->thd,
                                                      sj_outer_fanout,
                                                      temptable_rec_size);
    double one_write_cost= get_tmp_table_write_cost(join->thd,
                                                    sj_outer_fanout,
                                                    temptable_rec_size);

    double write_cost= COST_MULT(join->positions[first_tab].prefix_record_count,
                                 sj_outer_fanout * one_write_cost);
    double full_lookup_cost=
             COST_MULT(join->positions[first_tab].prefix_record_count,
                       COST_MULT(sj_outer_fanout,
                                 sj_inner_fanout * one_lookup_cost));
    dups_cost= COST_ADD(dups_cost, COST_ADD(write_cost, full_lookup_cost));
    
    *read_time= dups_cost;
    *record_count= prefix_rec_count * sj_outer_fanout;
    *handled_fanout= dups_removed_fanout;
    *strategy= SJ_OPT_DUPS_WEEDOUT;
    if (unlikely(trace.trace_started()))
    {
      trace.add("records", *record_count);
      trace.add("read_time", *read_time);
    }
    return TRUE;
  }
  return FALSE;
}


/*
  Remove the last join tab from from join->cur_sj_inner_tables bitmap
  we assume remaining_tables doesnt contain @tab.
*/

void restore_prev_sj_state(const table_map remaining_tables, 
                                  const JOIN_TAB *tab, uint idx)
{
  TABLE_LIST *emb_sj_nest;

  if (tab->emb_sj_nest)
  {
    table_map subq_tables= tab->emb_sj_nest->sj_inner_tables;
    tab->join->sjm_lookup_tables &= ~subq_tables;
  }

  if ((emb_sj_nest= tab->emb_sj_nest))
  {
    /* If we're removing the last SJ-inner table, remove the sj-nest */
    if ((remaining_tables & emb_sj_nest->sj_inner_tables) == 
        (emb_sj_nest->sj_inner_tables & ~tab->table->map))
    {
      tab->join->cur_sj_inner_tables &= ~emb_sj_nest->sj_inner_tables;
    }
  }
}


/*
  Given a semi-join nest, find out which of the IN-equalities are bound

  SYNOPSIS
    get_bound_sj_equalities()
      sj_nest           Semi-join nest
      remaining_tables  Tables that are not yet bound

  DESCRIPTION
    Given a semi-join nest, find out which of the IN-equalities have their
    left part expression bound (i.e. the said expression doesn't refer to
    any of remaining_tables and can be evaluated).

  RETURN
    Bitmap of bound IN-equalities.
*/

ulonglong get_bound_sj_equalities(TABLE_LIST *sj_nest, 
                                  table_map remaining_tables)
{
  List_iterator<Item_ptr> li(sj_nest->nested_join->sj_outer_expr_list);
  Item **item;
  uint i= 0;
  ulonglong res= 0;
  while ((item= li++))
  {
    /*
      Q: should this take into account equality propagation and how?
      A: If e->outer_side is an Item_field, walk over the equality
         class and see if there is an element that is bound?
      (this is an optional feature)
    */
    if (!(item[0]->used_tables() & remaining_tables))
    {
      res |= 1ULL << i;
    }
    i++;
  }
  return res;
}


/*
  Check if the last tables of the partial join order allow to use
  sj-materialization strategy for them

  SYNOPSIS
    at_sjmat_pos()
      join              
      remaining_tables
      tab                the last table's join tab
      idx                last table's index
      loose_scan    OUT  TRUE <=> use LooseScan

  RETURN
    TRUE   Yes, can apply sj-materialization
    FALSE  No, some of the requirements are not met
*/

static SJ_MATERIALIZATION_INFO *
at_sjmat_pos(const JOIN *join, table_map remaining_tables, const JOIN_TAB *tab,
             uint idx, bool *loose_scan)
{
  /*
   Check if 
    1. We're in a semi-join nest that can be run with SJ-materialization
    2. All the tables correlated through the IN subquery are in the prefix
  */
  TABLE_LIST *emb_sj_nest= tab->emb_sj_nest;
  table_map suffix= remaining_tables & ~tab->table->map;
  if (emb_sj_nest && emb_sj_nest->sj_mat_info &&
      !(suffix & emb_sj_nest->sj_inner_tables))
  {
    /* 
      Walk back and check if all immediately preceding tables are from
      this semi-join.
    */
    uint n_tables= my_count_bits(tab->emb_sj_nest->sj_inner_tables);
    for (uint i= 1; i < n_tables ; i++)
    {
      if (join->positions[idx - i].table->emb_sj_nest != tab->emb_sj_nest)
        return NULL;
    }
    *loose_scan= MY_TEST(remaining_tables & ~tab->table->map &
                                (emb_sj_nest->sj_inner_tables |
                                 emb_sj_nest->nested_join->sj_depends_on));
    if (*loose_scan && !emb_sj_nest->sj_subq_pred->sjm_scan_allowed)
      return NULL;
    else
      return emb_sj_nest->sj_mat_info;
  }
  return NULL;
}


/*
  Re-calculate values of join->best_positions[start..end].prefix_record_count
*/

static void recalculate_prefix_record_count(JOIN *join, uint start, uint end)
{
  for (uint j= start; j < end ;j++)
  {
    double prefix_count;
    if (j == join->const_tables)
      prefix_count= 1.0;
    else
      prefix_count= COST_MULT(join->best_positions[j-1].prefix_record_count,
			      join->best_positions[j-1].records_read);

    join->best_positions[j].prefix_record_count= prefix_count;
  }
}


/*
  Fix semi-join strategies for the picked join order

  SYNOPSIS
    fix_semijoin_strategies_for_picked_join_order()
      join  The join with the picked join order

  DESCRIPTION
    Fix semi-join strategies for the picked join order. This is a step that
    needs to be done right after we have fixed the join order. What we do
    here is switch join's semi-join strategy description from backward-based
    to forwards based.
    
    When join optimization is in progress, we re-consider semi-join
    strategies after we've added another table. Here's an illustration.
    Suppose the join optimization is underway:

    1) ot1  it1  it2 
                 sjX  -- looking at (ot1, it1, it2) join prefix, we decide
                         to use semi-join strategy sjX.

    2) ot1  it1  it2  ot2 
                 sjX  sjY -- Having added table ot2, we now may consider
                             another semi-join strategy and decide to use a 
                             different strategy sjY. Note that the record
                             of sjX has remained under it2. That is
                             necessary because we need to be able to get
                             back to (ot1, it1, it2) join prefix.
      what makes things even worse is that there are cases where the choice
      of sjY changes the way we should access it2. 

    3) [ot1  it1  it2  ot2  ot3]
                  sjX  sjY  -- This means that after join optimization is
                               finished, semi-join info should be read
                               right-to-left (while nearly all plan refinement
                               functions, EXPLAIN, etc proceed from left to 
                               right)

    This function does the needed reversal, making it possible to read the
    join and semi-join order from left to right.
*/    

void fix_semijoin_strategies_for_picked_join_order(JOIN *join)
{
  join->sjm_lookup_tables= 0;
  join->sjm_scan_tables= 0;
  if (!join->select_lex->sj_nests.elements)
    return;

  THD *thd= join->thd;
  uint table_count=join->table_count;
  uint tablenr;
  table_map remaining_tables= 0;
  table_map handled_tabs= 0;
  Json_writer_object trace_wrapper(thd);
  Json_writer_array trace_semijoin_strategies(thd,
                                              "fix_semijoin_strategies_for_picked_join_order");

  for (tablenr= table_count - 1 ; tablenr != join->const_tables - 1; tablenr--)
  {
    POSITION *pos= join->best_positions + tablenr;
    JOIN_TAB *s= pos->table;
    uint UNINIT_VAR(first); // Set by every branch except SJ_OPT_NONE which doesn't use it

    if ((handled_tabs & s->table->map) || pos->sj_strategy == SJ_OPT_NONE)
    {
      remaining_tables |= s->table->map;
      continue;
    }
    
    if (pos->sj_strategy == SJ_OPT_MATERIALIZE)
    {
      SJ_MATERIALIZATION_INFO *sjm= s->emb_sj_nest->sj_mat_info;
      sjm->is_used= TRUE;
      sjm->is_sj_scan= FALSE;
      memcpy((uchar*) (pos - sjm->tables + 1), (uchar*) sjm->positions,
             sizeof(POSITION) * sjm->tables);
      recalculate_prefix_record_count(join, tablenr - sjm->tables + 1,
                                      tablenr);
      first= tablenr - sjm->tables + 1;
      join->best_positions[first].n_sj_tables= sjm->tables;
      join->best_positions[first].sj_strategy= SJ_OPT_MATERIALIZE;
      Json_writer_object semijoin_strategy(thd);
      semijoin_strategy.add("semi_join_strategy","SJ-Materialization");
      Json_writer_array semijoin_plan(thd, "join_order");
      for (uint i= first; i < first+ sjm->tables; i++)
      {
        if (unlikely(thd->trace_started()))
        {
          Json_writer_object trace_one_table(thd);
          trace_one_table.add_table_name(join->best_positions[i].table);
        }
        join->sjm_lookup_tables |= join->best_positions[i].table->table->map;
      }
    }
    else if (pos->sj_strategy == SJ_OPT_MATERIALIZE_SCAN)
    {
      POSITION *first_inner= join->best_positions + pos->sjmat_picker.sjm_scan_last_inner;
      SJ_MATERIALIZATION_INFO *sjm= first_inner->table->emb_sj_nest->sj_mat_info;
      sjm->is_used= TRUE;
      sjm->is_sj_scan= TRUE;
      first= pos->sjmat_picker.sjm_scan_last_inner - sjm->tables + 1;
      memcpy((uchar*) (join->best_positions + first),
             (uchar*) sjm->positions, sizeof(POSITION) * sjm->tables);
      recalculate_prefix_record_count(join, first, first + sjm->tables);
      join->best_positions[first].sj_strategy= SJ_OPT_MATERIALIZE_SCAN;
      join->best_positions[first].n_sj_tables= sjm->tables;
      /* 
        Do what advance_sj_state did: re-run best_access_path for every table
        in the [last_inner_table + 1; pos..) range
      */
      double prefix_rec_count;
      /* Get the prefix record count */
      if (first == join->const_tables)
        prefix_rec_count= 1.0;
      else
        prefix_rec_count= join->best_positions[first-1].prefix_record_count;
      
      /* Add materialization record count*/
      prefix_rec_count *= sjm->rows;
      
      uint i;
      table_map rem_tables= remaining_tables;
      for (i= tablenr; i != (first + sjm->tables - 1); i--)
        rem_tables |= join->best_positions[i].table->table->map;

      for (i= first; i < first+ sjm->tables; i++)
        join->sjm_scan_tables |= join->best_positions[i].table->table->map;

      POSITION dummy;
      join->cur_sj_inner_tables= 0;
      Json_writer_object semijoin_strategy(thd);
      semijoin_strategy.add("semi_join_strategy","SJ-Materialization-Scan");
      Json_writer_array semijoin_plan(thd, "join_order");
      for (i= first + sjm->tables; i <= tablenr; i++)
      {
        Json_writer_object trace_one_table(thd);
        if (unlikely(thd->trace_started()))
        {
          trace_one_table.add_table_name(join->best_positions[i].table);
        }
        best_access_path(join, join->best_positions[i].table, rem_tables,
                         join->best_positions, i,
                         FALSE, prefix_rec_count,
                         join->best_positions + i, &dummy);
        prefix_rec_count *= join->best_positions[i].records_read;
        rem_tables &= ~join->best_positions[i].table->table->map;
      }
    }
 
    if (pos->sj_strategy == SJ_OPT_FIRST_MATCH)
    {
      first= pos->firstmatch_picker.first_firstmatch_table;
      join->best_positions[first].sj_strategy= SJ_OPT_FIRST_MATCH;
      join->best_positions[first].n_sj_tables= tablenr - first + 1;
      POSITION dummy; // For loose scan paths
      double record_count= (first== join->const_tables)? 1.0: 
                           join->best_positions[tablenr - 1].prefix_record_count;
      
      table_map rem_tables= remaining_tables;
      uint idx;
      for (idx= first; idx <= tablenr; idx++)
      {
        rem_tables |= join->best_positions[idx].table->table->map;
      }
      /*
        Re-run best_access_path to produce best access methods that do not use
        join buffering
      */ 
      join->cur_sj_inner_tables= 0;
      Json_writer_object semijoin_strategy(thd);
      semijoin_strategy.add("semi_join_strategy","FirstMatch");
      Json_writer_array semijoin_plan(thd, "join_order");
      for (idx= first; idx <= tablenr; idx++)
      {
        Json_writer_object trace_one_table(thd);
        if (unlikely(thd->trace_started()))
        {
          trace_one_table.add_table_name(join->best_positions[idx].table);
        }
        if (join->best_positions[idx].use_join_buffer)
        {
           best_access_path(join, join->best_positions[idx].table,
                            rem_tables, join->best_positions, idx,
                            TRUE /* no jbuf */,
                            record_count, join->best_positions + idx, &dummy);
        }
        record_count *= join->best_positions[idx].records_read;
        rem_tables &= ~join->best_positions[idx].table->table->map;
      }
    }

    if (pos->sj_strategy == SJ_OPT_LOOSE_SCAN) 
    {
      first= pos->loosescan_picker.first_loosescan_table;
      POSITION *first_pos= join->best_positions + first;
      POSITION loose_scan_pos; // For loose scan paths
      double record_count= (first== join->const_tables)? 1.0: 
                           join->best_positions[tablenr - 1].prefix_record_count;
      
      table_map rem_tables= remaining_tables;
      uint idx;
      for (idx= first; idx <= tablenr; idx++)
        rem_tables |= join->best_positions[idx].table->table->map;
      /*
        Re-run best_access_path to produce best access methods that do not use
        join buffering
      */ 
      join->cur_sj_inner_tables= 0;
      Json_writer_object semijoin_strategy(thd);
      semijoin_strategy.add("semi_join_strategy","LooseScan");
      Json_writer_array semijoin_plan(thd, "join_order");
      for (idx= first; idx <= tablenr; idx++)
      {
        Json_writer_object trace_one_table(thd);
        if (unlikely(thd->trace_started()))
        {
          trace_one_table.add_table_name(join->best_positions[idx].table);
        }
        if (join->best_positions[idx].use_join_buffer || (idx == first))
        {
           best_access_path(join, join->best_positions[idx].table,
                            rem_tables, join->best_positions, idx,
                            TRUE /* no jbuf */,
                            record_count, join->best_positions + idx,
                            &loose_scan_pos);
           if (idx==first)
           {
             join->best_positions[idx]= loose_scan_pos;
             /*
               If LooseScan is based on ref access (including the "degenerate"
               one with 0 key parts), we should use full index scan.

               Unfortunately, lots of code assumes that if tab->type==JT_ALL && 
               tab->quick!=NULL, then quick select should be used. The only
               simple way to fix this is to remove the quick select:
             */
             if (join->best_positions[idx].key)
             {
               delete join->best_positions[idx].table->quick;
               join->best_positions[idx].table->quick= NULL;
             }
           }
        }
        rem_tables &= ~join->best_positions[idx].table->table->map;
        record_count *= join->best_positions[idx].records_read;
      }
      first_pos->sj_strategy= SJ_OPT_LOOSE_SCAN;
      first_pos->n_sj_tables= my_count_bits(first_pos->table->emb_sj_nest->sj_inner_tables);
    }

    if (pos->sj_strategy == SJ_OPT_DUPS_WEEDOUT)
    {
      Json_writer_object semijoin_strategy(thd);
      semijoin_strategy.add("semi_join_strategy","DuplicateWeedout");
      /* 
        Duplicate Weedout starting at pos->first_dupsweedout_table, ending at
        this table.
      */
      first= pos->dups_weedout_picker.first_dupsweedout_table;
      join->best_positions[first].sj_strategy= SJ_OPT_DUPS_WEEDOUT;
      join->best_positions[first].n_sj_tables= tablenr - first + 1;
    }
    
    uint i_end= first + join->best_positions[first].n_sj_tables;
    for (uint i= first; i < i_end; i++)
    {
      if (i != first)
        join->best_positions[i].sj_strategy= SJ_OPT_NONE;
      handled_tabs |= join->best_positions[i].table->table->map;
    }

    if (tablenr != first)
      pos->sj_strategy= SJ_OPT_NONE;
    remaining_tables |= s->table->map;
    join->join_tab[first].sj_strategy= join->best_positions[first].sj_strategy;
    join->join_tab[first].n_sj_tables= join->best_positions[first].n_sj_tables;
  }
}


/*
  Return the number of tables at the top-level of the JOIN

  SYNOPSIS
    get_number_of_tables_at_top_level()
      join  The join with the picked join order

  DESCRIPTION
    The number of tables in the JOIN currently include all the inner tables of the
    mergeable semi-joins. The function would make sure that we only count the semi-join
    nest and not the inner tables of teh semi-join nest.
*/

uint get_number_of_tables_at_top_level(JOIN *join)
{
  uint j= 0, tables= 0;
  while(j < join->table_count)
  {
    POSITION *cur_pos= &join->best_positions[j];
    tables++;
    if (cur_pos->sj_strategy == SJ_OPT_MATERIALIZE ||
        cur_pos->sj_strategy == SJ_OPT_MATERIALIZE_SCAN)
    {
      SJ_MATERIALIZATION_INFO *sjm= cur_pos->table->emb_sj_nest->sj_mat_info;
      j= j + sjm->tables;
    }
    else
      j++;
  }
  return tables;
}


/*
  Setup semi-join materialization strategy for one semi-join nest
  
  SYNOPSIS

  setup_sj_materialization()
    tab  The first tab in the semi-join

  DESCRIPTION
    Setup execution structures for one semi-join materialization nest:
    - Create the materialization temporary table
    - If we're going to do index lookups
        create TABLE_REF structure to make the lookus
    - else (if we're going to do a full scan of the temptable)
        create Copy_field structures to do copying.

  RETURN
    FALSE  Ok
    TRUE   Error
*/

bool setup_sj_materialization_part1(JOIN_TAB *sjm_tab)
{
  JOIN_TAB *tab= sjm_tab->bush_children->start;
  TABLE_LIST *emb_sj_nest= tab->table->pos_in_table_list->embedding;
  SJ_MATERIALIZATION_INFO *sjm;
  THD *thd;

  DBUG_ENTER("setup_sj_materialization");
  
  /* Walk out of outer join nests until we reach the semi-join nest we're in */
  while (!emb_sj_nest->sj_mat_info)
    emb_sj_nest= emb_sj_nest->embedding;

  sjm= emb_sj_nest->sj_mat_info;
  thd= tab->join->thd;
  /* First the calls come to the materialization function */

  DBUG_ASSERT(sjm->is_used);
  /* 
    Set up the table to write to, do as select_union::create_result_table does
  */
  sjm->sjm_table_param.init();
  sjm->sjm_table_param.bit_fields_as_long= TRUE;
  SELECT_LEX *subq_select= emb_sj_nest->sj_subq_pred->unit->first_select();
  const LEX_CSTRING sj_materialize_name= { STRING_WITH_LEN("sj-materialize") };
  List_iterator<Item> it(subq_select->item_list);
  Item *item;
  while((item= it++))
  {
    /*
      This semi-join replaced the subquery (subq_select) and so on
      re-executing it will not be prepared. To use the Items from its
      select list we have to prepare (fix_fields) them
    */
    if (item->fix_fields_if_needed(thd, it.ref()))
      DBUG_RETURN(TRUE);
    item= *(it.ref()); // it can be changed by fix_fields
    DBUG_ASSERT(!item->name.length || item->name.length == strlen(item->name.str));
    sjm->sjm_table_cols.push_back(item, thd->mem_root);
  }

  sjm->sjm_table_param.field_count= subq_select->item_list.elements;
  sjm->sjm_table_param.force_not_null_cols= TRUE;

  if (!(sjm->table= create_tmp_table(thd, &sjm->sjm_table_param, 
                                     sjm->sjm_table_cols, (ORDER*) 0, 
                                     TRUE /* distinct */, 
                                     1, /*save_sum_fields*/
                                     thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS, 
                                     HA_POS_ERROR /*rows_limit */, 
                                     &sj_materialize_name)))
    DBUG_RETURN(TRUE); /* purecov: inspected */
  sjm->table->map=  emb_sj_nest->nested_join->used_tables;
  sjm->table->file->extra(HA_EXTRA_WRITE_CACHE);
  sjm->table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);

  tab->join->sj_tmp_tables.push_back(sjm->table, thd->mem_root);
  tab->join->sjm_info_list.push_back(sjm, thd->mem_root);
  
  sjm->materialized= FALSE;
  sjm_tab->table= sjm->table;
  sjm->table->pos_in_table_list= emb_sj_nest;
 
  DBUG_RETURN(FALSE);
}

/**
   @retval
   FALSE ok
   TRUE  error
*/

bool setup_sj_materialization_part2(JOIN_TAB *sjm_tab)
{
  DBUG_ENTER("setup_sj_materialization_part2");
  JOIN_TAB *tab= sjm_tab->bush_children->start;
  TABLE_LIST *emb_sj_nest= tab->table->pos_in_table_list->embedding;
  /* Walk out of outer join nests until we reach the semi-join nest we're in */
  while (!emb_sj_nest->sj_mat_info)
    emb_sj_nest= emb_sj_nest->embedding;
  SJ_MATERIALIZATION_INFO *sjm= emb_sj_nest->sj_mat_info;
  THD *thd= tab->join->thd;
  uint i;

  if (!sjm->is_sj_scan)
  {
    KEY           *tmp_key; /* The only index on the temporary table. */
    uint          tmp_key_parts; /* Number of keyparts in tmp_key. */
    tmp_key= sjm->table->key_info;
    tmp_key_parts= tmp_key->user_defined_key_parts;
    
    /*
      Create/initialize everything we will need to index lookups into the
      temptable.
    */
    TABLE_REF *tab_ref;
    tab_ref= &sjm_tab->ref;
    tab_ref->key= 0; /* The only temp table index. */
    tab_ref->key_length= tmp_key->key_length;
    if (!(tab_ref->key_buff=
          (uchar*) thd->calloc(ALIGN_SIZE(tmp_key->key_length) * 2)) ||
        !(tab_ref->key_copy=
          (store_key**) thd->alloc((sizeof(store_key*) *
                                    (tmp_key_parts + 1)))) ||
        !(tab_ref->items=
          (Item**) thd->alloc(sizeof(Item*) * tmp_key_parts)))
      DBUG_RETURN(TRUE); /* purecov: inspected */

    tab_ref->key_buff2=tab_ref->key_buff+ALIGN_SIZE(tmp_key->key_length);
    tab_ref->key_err=1;
    tab_ref->null_rejecting= 1;
    tab_ref->disable_cache= FALSE;

    KEY_PART_INFO *cur_key_part= tmp_key->key_part;
    store_key **ref_key= tab_ref->key_copy;
    uchar *cur_ref_buff= tab_ref->key_buff;
    
    for (i= 0; i < tmp_key_parts; i++, cur_key_part++, ref_key++)
    {
      tab_ref->items[i]=
        emb_sj_nest->sj_subq_pred->left_exp()->element_index(i);
      int null_count= MY_TEST(cur_key_part->field->real_maybe_null());
      *ref_key= new store_key_item(thd, cur_key_part->field,
                                   /* TODO:
                                      the NULL byte is taken into account in
                                      cur_key_part->store_length, so instead of
                                      cur_ref_buff + MY_TEST(maybe_null), we could
                                      use that information instead.
                                   */
                                   cur_ref_buff + null_count,
                                   null_count ? cur_ref_buff : 0,
                                   cur_key_part->length, tab_ref->items[i],
                                   FALSE);
      if (!*ref_key)
        DBUG_RETURN(TRUE);
      cur_ref_buff+= cur_key_part->store_length;
    }
    *ref_key= NULL; /* End marker. */
      
    /*
      We don't ever have guarded conditions for SJM tables, but code at SQL
      layer depends on cond_guards array being alloced.
    */
    if (!(tab_ref->cond_guards= (bool**) thd->calloc(sizeof(uint*)*tmp_key_parts)))
    {
      DBUG_RETURN(TRUE);
    }

    tab_ref->key_err= 1;
    tab_ref->key_parts= tmp_key_parts;
    sjm->tab_ref= tab_ref;

    /*
      Remove the injected semi-join IN-equalities from join_tab conds. This
      needs to be done because the IN-equalities refer to columns of
      sj-inner tables which are not available after the materialization
      has been finished.
    */
    for (i= 0; i < sjm->tables; i++)
    {
      if (remove_sj_conds(thd, &tab[i].select_cond) ||
          (tab[i].select && remove_sj_conds(thd, &tab[i].select->cond)))
        DBUG_RETURN(TRUE);
    }
    if (!(sjm->in_equality= create_subq_in_equalities(thd, sjm,
                                                      emb_sj_nest->sj_subq_pred)))
      DBUG_RETURN(TRUE); /* purecov: inspected */
    sjm_tab->type= JT_EQ_REF;
    sjm_tab->select_cond= sjm->in_equality;
  }
  else
  {
    /*
      We'll be doing full scan of the temptable.  
      Setup copying of temptable columns back to the record buffers
      for their source tables. We need this because IN-equalities
      refer to the original tables.

      EXAMPLE

      Consider the query:
        SELECT * FROM ot WHERE ot.col1 IN (SELECT it.col2 FROM it)
      
      Suppose it's executed with SJ-Materialization-scan. We choose to do scan
      if we can't do the lookup, i.e. the join order is (it, ot). The plan
      would look as follows:

        table    access method      condition
         it      materialize+scan    -
         ot      (whatever)          ot1.col1=it.col2 (C2)

      The condition C2 refers to current row of table it. The problem is
      that by the time we evaluate C2, we would have finished with scanning
      it itself and will be scanning the temptable. 

      At the moment, our solution is to copy back: when we get the next
      temptable record, we copy its columns to their corresponding columns
      in the record buffers for the source tables. 
    */
    if (!(sjm->copy_field= new Copy_field[sjm->sjm_table_cols.elements]))
      DBUG_RETURN(TRUE);

    //it.rewind();
    Ref_ptr_array p_items= emb_sj_nest->sj_subq_pred->unit->first_select()->ref_pointer_array;
    for (uint i=0; i < sjm->sjm_table_cols.elements; i++)
    {
      bool dummy;
      Item_equal *item_eq;
      //Item *item= (it++)->real_item();
      Item *item= p_items[i]->real_item();
      DBUG_ASSERT(item->type() == Item::FIELD_ITEM);
      Field *copy_to= ((Item_field*)item)->field;
      /*
        Tricks with Item_equal are due to the following: suppose we have a
        query:
        
        ... WHERE cond(ot.col) AND ot.col IN (SELECT it2.col FROM it1,it2
                                               WHERE it1.col= it2.col)
         then equality propagation will create an 
         
           Item_equal(it1.col, it2.col, ot.col) 
         
         then substitute_for_best_equal_field() will change the conditions
         according to the join order:

         table | attached condition
         ------+--------------------
          it1  |
          it2  | it1.col=it2.col
          ot   | cond(it1.col)

         although we've originally had "SELECT it2.col", conditions attached 
         to subsequent outer tables will refer to it1.col, so SJM-Scan will
         need to unpack data to there. 
         That is, if an element from subquery's select list participates in 
         equality propagation, then we need to unpack it to the first
         element equality propagation member that refers to table that is
         within the subquery.
      */
      item_eq= find_item_equal(tab->join->cond_equal, copy_to, &dummy);

      if (item_eq)
      {
        List_iterator<Item> it(item_eq->equal_items);
        /* We're interested in field items only */
        if (item_eq->get_const())
          it++;
        Item *item;
        while ((item= it++))
        {
          if (!(item->used_tables() & ~emb_sj_nest->sj_inner_tables))
          {
            DBUG_ASSERT(item->real_item()->type() == Item::FIELD_ITEM);
            copy_to= ((Item_field *) (item->real_item()))->field;
            break;
          }
        }
      }
      sjm->copy_field[i].set(copy_to, sjm->table->field[i], FALSE);
      /* The write_set for source tables must be set up to allow the copying */
      bitmap_set_bit(copy_to->table->write_set, copy_to->field_index);
    }
    sjm_tab->type= JT_ALL;

    /* Initialize full scan */
    sjm_tab->read_first_record= join_init_read_record;
    sjm_tab->read_record.copy_field= sjm->copy_field;
    sjm_tab->read_record.copy_field_end= sjm->copy_field +
                                         sjm->sjm_table_cols.elements;
    sjm_tab->read_record.read_record_func= read_record_func_for_rr_and_unpack;
  }

  sjm_tab->bush_children->end[-1].next_select= end_sj_materialize;

  DBUG_RETURN(FALSE);
}



/*
  Create subquery IN-equalities assuming use of materialization strategy
  
  SYNOPSIS
    create_subq_in_equalities()
      thd        Thread handle
      sjm        Semi-join materialization structure
      subq_pred  The subquery predicate

  DESCRIPTION
    Create subquery IN-equality predicates. That is, for a subquery
    
      (oe1, oe2, ...) IN (SELECT ie1, ie2, ... FROM ...)
    
    create "oe1=ie1 AND ie1=ie2 AND ..." expression, such that ie1, ie2, ..
    refer to the columns of the table that's used to materialize the
    subquery.

  RETURN 
    Created condition
*/

static Item *create_subq_in_equalities(THD *thd, SJ_MATERIALIZATION_INFO *sjm, 
                                Item_in_subselect *subq_pred)
{
  Item *res= NULL;
  Item *left_exp= subq_pred->left_exp();
  uint ncols= left_exp->cols();
  if (ncols == 1)
  {
    if (!(res= new (thd->mem_root) Item_func_eq(thd, left_exp,
                                new (thd->mem_root) Item_field(thd, sjm->table->field[0]))))
      return NULL; /* purecov: inspected */
  }
  else
  {
    Item *conj;
    for (uint i= 0; i < ncols; i++)
    {
      if (!(conj= new (thd->mem_root) Item_func_eq(thd, left_exp->element_index(i),
                                   new (thd->mem_root) Item_field(thd, sjm->table->field[i]))) ||
          !(res= and_items(thd, res, conj)))
        return NULL; /* purecov: inspected */
    }
  }
  if (res->fix_fields(thd, &res))
    return NULL; /* purecov: inspected */
  return res;
}


/**
  @retval
  0 ok
  1 error
*/

static bool remove_sj_conds(THD *thd, Item **tree)
{
  if (*tree)
  {
    if (is_cond_sj_in_equality(*tree))
    {
      *tree= NULL;
      return 0;
    }
    else if ((*tree)->type() == Item::COND_ITEM) 
    {
      Item *item;
      List_iterator<Item> li(*(((Item_cond*)*tree)->argument_list()));
      while ((item= li++))
      {
        if (is_cond_sj_in_equality(item))
        {
          Item_int *tmp= new (thd->mem_root) Item_int(thd, 1);
          if (!tmp)
            return 1;
          li.replace(tmp);
        }
      }
    }
  }
  return 0;
}


/* Check if given Item was injected by semi-join equality */
static bool is_cond_sj_in_equality(Item *item)
{
  if (item->type() == Item::FUNC_ITEM &&
      ((Item_func*)item)->functype()== Item_func::EQ_FUNC)
  {
    Item_func_eq *item_eq= (Item_func_eq*)item;
    return MY_TEST(item_eq->in_equality_no != UINT_MAX);
  }
  return FALSE;
}


/*
  Create a temporary table to weed out duplicate rowid combinations

  SYNOPSIS

    create_sj_weedout_tmp_table()
      thd                    Thread handle

  DESCRIPTION
    Create a temporary table to weed out duplicate rowid combinations. The
    table has a single column that is a concatenation of all rowids in the
    combination. 

    Depending on the needed length, there are two cases:

    1. When the length of the column < max_key_length:

      CREATE TABLE tmp (col VARBINARY(n) NOT NULL, UNIQUE KEY(col));

    2. Otherwise (not a valid SQL syntax but internally supported):

      CREATE TABLE tmp (col VARBINARY NOT NULL, UNIQUE CONSTRAINT(col));

    The code in this function was produced by extraction of relevant parts
    from create_tmp_table().

  RETURN
    created table
    NULL on error
*/

bool
SJ_TMP_TABLE::create_sj_weedout_tmp_table(THD *thd)
{
  MEM_ROOT *mem_root_save, own_root;
  TABLE *table;
  TABLE_SHARE *share;
  uint  temp_pool_slot=MY_BIT_NONE;
  char	*tmpname,path[FN_REFLEN];
  Field **reg_field;
  KEY_PART_INFO *key_part_info;
  KEY *keyinfo;
  uchar *group_buff;
  uchar *bitmaps;
  uint *blob_field;
  bool using_unique_constraint=FALSE;
  bool use_packed_rows= FALSE;
  Field *field, *key_field;
  uint null_pack_length, null_count;
  uchar *null_flags;
  uchar *pos;
  DBUG_ENTER("create_sj_weedout_tmp_table");
  DBUG_ASSERT(!is_degenerate);

  tmp_table= NULL;
  uint uniq_tuple_length_arg= rowid_len + null_bytes;
  /*
    STEP 1: Get temporary table name
  */
  if (use_temp_pool && !(test_flags & TEST_KEEP_TMP_TABLES))
    temp_pool_slot = temp_pool_set_next();

  if (temp_pool_slot != MY_BIT_NONE) // we got a slot
    sprintf(path, "%s-subquery-%lx-%i", tmp_file_prefix,
	    current_pid, temp_pool_slot);
  else
  {
    /* if we run out of slots or we are not using tempool */
    sprintf(path,"%s-subquery-%lx-%lx-%x", tmp_file_prefix,current_pid,
            (ulong) thd->thread_id, thd->tmp_table++);
  }
  fn_format(path, path, mysql_tmpdir, "", MY_REPLACE_EXT|MY_UNPACK_FILENAME);

  /* STEP 2: Figure if we'll be using a key or blob+constraint */
  /* it always has my_charset_bin, so mbmaxlen==1 */
  if (uniq_tuple_length_arg >= CONVERT_IF_BIGGER_TO_BLOB)
    using_unique_constraint= TRUE;

  /* STEP 3: Allocate memory for temptable description */
  init_sql_alloc(PSI_INSTRUMENT_ME, &own_root, TABLE_ALLOC_BLOCK_SIZE, 0,
                 MYF(MY_THREAD_SPECIFIC));
  if (!multi_alloc_root(&own_root,
                        &table, sizeof(*table),
                        &share, sizeof(*share),
                        &reg_field, sizeof(Field*) * (1+1),
                        &blob_field, sizeof(uint)*2,
                        &keyinfo, sizeof(*keyinfo),
                        &key_part_info, sizeof(*key_part_info) * 2,
                        &start_recinfo,
                        sizeof(*recinfo)*(1*2+4),
                        &tmpname, (uint) strlen(path)+1,
                        &group_buff, (!using_unique_constraint ?
                                      uniq_tuple_length_arg : 0),
                        &bitmaps, bitmap_buffer_size(1)*6,
                        NullS))
  {
    if (temp_pool_slot != MY_BIT_NONE)
      temp_pool_clear_bit(temp_pool_slot);
    DBUG_RETURN(TRUE);
  }
  strmov(tmpname,path);
  

  /* STEP 4: Create TABLE description */
  bzero((char*) table,sizeof(*table));
  bzero((char*) reg_field,sizeof(Field*)*2);

  table->mem_root= own_root;
  mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  table->field=reg_field;
  table->alias.set("weedout-tmp", sizeof("weedout-tmp")-1,
                   table_alias_charset);
  table->reginfo.lock_type=TL_WRITE;	/* Will be updated */
  table->db_stat=HA_OPEN_KEYFILE;
  table->map=1;
  table->temp_pool_slot = temp_pool_slot;
  table->copy_blobs= 1;
  table->in_use= thd;

  table->s= share;
  init_tmp_table_share(thd, share, "", 0, tmpname, tmpname);
  share->blob_field= blob_field;
  share->table_charset= NULL;
  share->primary_key= MAX_KEY;               // Indicate no primary key

  /* Create the field */
  {
    LEX_CSTRING field_name= {STRING_WITH_LEN("rowids") };
    /*
      For the sake of uniformity, always use Field_varstring (altough we could
      use Field_string for shorter keys)
    */
    field= new Field_varstring(uniq_tuple_length_arg, FALSE, &field_name,
                               share, &my_charset_bin);
    if (!field)
      DBUG_RETURN(0);
    field->table= table;
    field->key_start.clear_all();
    field->part_of_key.clear_all();
    field->part_of_sortkey.clear_all();
    field->unireg_check= Field::NONE;
    field->flags= (NOT_NULL_FLAG | BINARY_FLAG | NO_DEFAULT_VALUE_FLAG);
    field->reset_fields();
    field->init(table);
    field->orig_table= NULL;
     
    field->field_index= 0;
    
    *(reg_field++)= field;
    *blob_field= 0;
    *reg_field= 0;

    share->fields= 1;
    share->blob_fields= 0;
  }

  uint reclength= field->pack_length();
  if (using_unique_constraint || thd->variables.tmp_memory_table_size == 0)
  { 
    share->db_plugin= ha_lock_engine(0, TMP_ENGINE_HTON);
    table->file= get_new_handler(share, &table->mem_root,
                                 share->db_type());
  }
  else
  {
    share->db_plugin= ha_lock_engine(0, heap_hton);
    table->file= get_new_handler(share, &table->mem_root,
                                 share->db_type());
    DBUG_ASSERT(!table->file || uniq_tuple_length_arg <= table->file->max_key_length());
  }
  if (!table->file)
    goto err;

  if (table->file->set_ha_share_ref(&share->ha_share))
  {
    delete table->file;
    goto err;
  }

  null_count=1;
  
  null_pack_length= 1;
  reclength += null_pack_length;

  share->reclength= reclength;
  {
    uint alloc_length=ALIGN_SIZE(share->reclength + MI_UNIQUE_HASH_LENGTH+1);
    share->rec_buff_length= alloc_length;
    if (!(table->record[0]= (uchar*)
                            alloc_root(&table->mem_root, alloc_length*3)))
      goto err;
    table->record[1]= table->record[0]+alloc_length;
    share->default_values= table->record[1]+alloc_length;
  }
  setup_tmp_table_column_bitmaps(table, bitmaps, table->s->fields);

  recinfo= start_recinfo;
  null_flags=(uchar*) table->record[0];
  pos=table->record[0]+ null_pack_length;
  if (null_pack_length)
  {
    bzero((uchar*) recinfo,sizeof(*recinfo));
    recinfo->type=FIELD_NORMAL;
    recinfo->length=null_pack_length;
    recinfo++;
    bfill(null_flags,null_pack_length,255);	// Set null fields

    table->null_flags= (uchar*) table->record[0];
    share->null_fields= null_count;
    share->null_bytes= null_pack_length;
  }
  null_count=1;

  {
    //Field *field= *reg_field;
    uint length;
    bzero((uchar*) recinfo,sizeof(*recinfo));
    field->move_field(pos,(uchar*) 0,0);

    field->reset();
    /*
      Test if there is a default field value. The test for ->ptr is to skip
      'offset' fields generated by initialize_tables
    */
    // Initialize the table field:
    bzero(field->ptr, field->pack_length());

    length=field->pack_length();
    pos+= length;

    /* Make entry for create table */
    recinfo->length=length;
    recinfo->type= field->tmp_engine_column_type(use_packed_rows);
    field->set_table_name(&table->alias);
  }

  if (thd->variables.tmp_memory_table_size == ~ (ulonglong) 0)	// No limit
    share->max_rows= ~(ha_rows) 0;
  else
    share->max_rows= (ha_rows) (((share->db_type() == heap_hton) ?
                                 MY_MIN(thd->variables.tmp_memory_table_size,
                                        thd->variables.max_heap_table_size) :
                                 thd->variables.tmp_disk_table_size) /
			         share->reclength);
  set_if_bigger(share->max_rows,1);		// For dummy start options


  //// keyinfo= param->keyinfo;
  if (TRUE)
  {
    DBUG_PRINT("info",("Creating group key in temporary table"));
    share->keys=1;
    share->uniques= MY_TEST(using_unique_constraint);
    table->key_info=keyinfo;
    keyinfo->key_part=key_part_info;
    keyinfo->flags=HA_NOSAME;
    keyinfo->usable_key_parts= keyinfo->user_defined_key_parts= 1;
    keyinfo->key_length=0;
    keyinfo->rec_per_key=0;
    keyinfo->algorithm= HA_KEY_ALG_UNDEF;
    keyinfo->name= weedout_key;
    {
      key_part_info->null_bit=0;
      key_part_info->field=  field;
      key_part_info->offset= field->offset(table->record[0]);
      key_part_info->length= (uint16) field->key_length();
      key_part_info->type=   (uint8) field->key_type();
      key_part_info->key_type = FIELDFLAG_BINARY;
      if (!using_unique_constraint)
      {
	if (!(key_field= field->new_key_field(thd->mem_root, table,
                                              group_buff,
                                              key_part_info->length,
                                              field->null_ptr,
                                              field->null_bit)))
	  goto err;
      }
      keyinfo->key_length+=  key_part_info->length;
    }
  }

  if (unlikely(thd->is_fatal_error))            // If end of memory
    goto err;
  share->db_record_offset= 1;
  table->no_rows= 1;              		// We don't need the data

  // recinfo must point after last field
  recinfo++;
  if (share->db_type() == TMP_ENGINE_HTON)
  {
    if (unlikely(create_internal_tmp_table(table, keyinfo, start_recinfo,
                                           &recinfo, 0)))
      goto err;
  }
  if (unlikely(open_tmp_table(table)))
    goto err;

  thd->mem_root= mem_root_save;
  tmp_table= table;
  DBUG_RETURN(FALSE);

err:
  thd->mem_root= mem_root_save;
  free_tmp_table(thd,table);                    /* purecov: inspected */
  if (temp_pool_slot != MY_BIT_NONE)
    temp_pool_clear_bit(temp_pool_slot);
  DBUG_RETURN(TRUE);				/* purecov: inspected */
}


/*
  SemiJoinDuplicateElimination: Reset the temporary table
*/

int SJ_TMP_TABLE::sj_weedout_delete_rows()
{
  DBUG_ENTER("SJ_TMP_TABLE::sj_weedout_delete_rows");
  if (tmp_table)
  {
    int rc= tmp_table->file->ha_delete_all_rows();
    DBUG_RETURN(rc);
  }
  have_degenerate_row= FALSE;
  DBUG_RETURN(0);
}


/*
  SemiJoinDuplicateElimination: Weed out duplicate row combinations

  SYNPOSIS
    sj_weedout_check_row()
      thd    Thread handle

  DESCRIPTION
    Try storing current record combination of outer tables (i.e. their
    rowids) in the temporary table. This records the fact that we've seen 
    this record combination and also tells us if we've seen it before.

  RETURN
    -1  Error
    1   The row combination is a duplicate (discard it)
    0   The row combination is not a duplicate (continue)
*/

int SJ_TMP_TABLE::sj_weedout_check_row(THD *thd)
{
  int error;
  SJ_TMP_TABLE::TAB *tab= tabs;
  SJ_TMP_TABLE::TAB *tab_end= tabs_end;
  uchar *ptr;
  uchar *nulls_ptr;

  DBUG_ENTER("SJ_TMP_TABLE::sj_weedout_check_row");

  if (is_degenerate)
  {
    if (have_degenerate_row) 
      DBUG_RETURN(1);

    have_degenerate_row= TRUE;
    DBUG_RETURN(0);
  }

  ptr= tmp_table->record[0] + 1;

  /* Put the the rowids tuple into table->record[0]: */

  // 1. Store the length 
  if (((Field_varstring*)(tmp_table->field[0]))->length_bytes == 1)
  {
    *ptr= (uchar)(rowid_len + null_bytes);
    ptr++;
  }
  else
  {
    int2store(ptr, rowid_len + null_bytes);
    ptr += 2;
  }

  nulls_ptr= ptr;
  // 2. Zero the null bytes 
  if (null_bytes)
  {
    bzero(ptr, null_bytes);
    ptr += null_bytes; 
  }

  // 3. Put the rowids
  for (uint i=0; tab != tab_end; tab++, i++)
  {
    handler *h= tab->join_tab->table->file;
    if (tab->join_tab->table->maybe_null && tab->join_tab->table->null_row)
    {
      /* It's a NULL-complemented row */
      *(nulls_ptr + tab->null_byte) |= tab->null_bit;
      bzero(ptr + tab->rowid_offset, h->ref_length);
    }
    else
    {
      /* Copy the rowid value */
      memcpy(ptr + tab->rowid_offset, h->ref, h->ref_length);
    }
  }

  error= tmp_table->file->ha_write_tmp_row(tmp_table->record[0]);
  if (unlikely(error))
  {
    /* create_internal_tmp_table_from_heap will generate error if needed */
    if (!tmp_table->file->is_fatal_error(error, HA_CHECK_DUP))
      DBUG_RETURN(1); /* Duplicate */

    bool is_duplicate;
    if (create_internal_tmp_table_from_heap(thd, tmp_table, start_recinfo,
                                            &recinfo, error, 1, &is_duplicate))
      DBUG_RETURN(-1);
    if (is_duplicate)
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


int init_dups_weedout(JOIN *join, uint first_table, int first_fanout_table, uint n_tables)
{
  THD *thd= join->thd;
  DBUG_ENTER("init_dups_weedout");
  SJ_TMP_TABLE::TAB sjtabs[MAX_TABLES];
  SJ_TMP_TABLE::TAB *last_tab= sjtabs;
  uint jt_rowid_offset= 0; // # tuple bytes are already occupied (w/o NULL bytes)
  uint jt_null_bits= 0;    // # null bits in tuple bytes
  /*
    Walk through the range and remember
     - tables that need their rowids to be put into temptable
     - the last outer table
  */
  for (JOIN_TAB *j=join->join_tab + first_table; 
       j < join->join_tab + first_table + n_tables; j++)
  {
    if (sj_table_is_included(join, j))
    {
      last_tab->join_tab= j;
      last_tab->rowid_offset= jt_rowid_offset;
      jt_rowid_offset += j->table->file->ref_length;
      if (j->table->maybe_null)
      {
        last_tab->null_byte= jt_null_bits / 8;
        last_tab->null_bit= jt_null_bits++;
      }
      last_tab++;
      j->table->prepare_for_position();
      j->keep_current_rowid= TRUE;
    }
  }

  SJ_TMP_TABLE *sjtbl;
  if (jt_rowid_offset) /* Temptable has at least one rowid */
  {
    size_t tabs_size= (last_tab - sjtabs) * sizeof(SJ_TMP_TABLE::TAB);
    if (!(sjtbl= (SJ_TMP_TABLE*)thd->alloc(sizeof(SJ_TMP_TABLE))) ||
        !(sjtbl->tabs= (SJ_TMP_TABLE::TAB*) thd->alloc(tabs_size)))
      DBUG_RETURN(TRUE); /* purecov: inspected */
    memcpy(sjtbl->tabs, sjtabs, tabs_size);
    sjtbl->is_degenerate= FALSE;
    sjtbl->tabs_end= sjtbl->tabs + (last_tab - sjtabs);
    sjtbl->rowid_len= jt_rowid_offset;
    sjtbl->null_bits= jt_null_bits;
    sjtbl->null_bytes= (jt_null_bits + 7)/8;
    if (sjtbl->create_sj_weedout_tmp_table(thd))
      DBUG_RETURN(TRUE);
    join->sj_tmp_tables.push_back(sjtbl->tmp_table, thd->mem_root);
  }
  else
  {
    /* 
      This is a special case where the entire subquery predicate does 
      not depend on anything at all, ie this is 
        WHERE const IN (uncorrelated select)
    */
    if (!(sjtbl= (SJ_TMP_TABLE*)thd->alloc(sizeof(SJ_TMP_TABLE))))
      DBUG_RETURN(TRUE); /* purecov: inspected */
    sjtbl->tmp_table= NULL;
    sjtbl->is_degenerate= TRUE;
    sjtbl->have_degenerate_row= FALSE;
  }

  sjtbl->next_flush_table= join->join_tab[first_table].flush_weedout_table;
  join->join_tab[first_table].flush_weedout_table= sjtbl;
  join->join_tab[first_fanout_table].first_weedout_table= sjtbl;
  join->join_tab[first_table + n_tables - 1].check_weed_out_table= sjtbl;
  DBUG_RETURN(0);
}


/*
  @brief
    Set up semi-join Loose Scan strategy for execution

  @detail
    Other strategies are done in setup_semijoin_dups_elimination(),
    however, we need to set up Loose Scan earlier, before make_join_select is
    called. This is to prevent make_join_select() from switching full index
    scans into quick selects (which will break Loose Scan access).

  @return
    0  OK
    1  Error
*/

int setup_semijoin_loosescan(JOIN *join)
{
  uint i;
  DBUG_ENTER("setup_semijoin_loosescan");

  POSITION *pos= join->best_positions + join->const_tables;
  for (i= join->const_tables ; i < join->top_join_tab_count; )
  {
    JOIN_TAB *tab=join->join_tab + i;
    switch (pos->sj_strategy) {
      case SJ_OPT_MATERIALIZE:
      case SJ_OPT_MATERIALIZE_SCAN:
        i+= 1; /* join tabs are embedded in the nest */
        pos += pos->n_sj_tables;
        break;
      case SJ_OPT_LOOSE_SCAN:
      {
        /* We jump from the last table to the first one */
        tab->loosescan_match_tab= tab + pos->n_sj_tables - 1;

        /* LooseScan requires records to be produced in order */
        if (tab->select && tab->select->quick)
          tab->select->quick->need_sorted_output();

        for (uint j= i; j < i + pos->n_sj_tables; j++)
          join->join_tab[j].inside_loosescan_range= TRUE;

        /* Calculate key length */
        uint keylen= 0;
        uint keyno= pos->loosescan_picker.loosescan_key;
        for (uint kp=0; kp < pos->loosescan_picker.loosescan_parts; kp++)
          keylen += tab->table->key_info[keyno].key_part[kp].store_length;

        tab->loosescan_key= keyno;
        tab->loosescan_key_len= keylen;
        if (pos->n_sj_tables > 1) 
          tab[pos->n_sj_tables - 1].do_firstmatch= tab;
        i+= pos->n_sj_tables;
        pos+= pos->n_sj_tables;
        break;
      }
      default:
      {
        i++;
        pos++;
        break;
      }
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  Setup the strategies to eliminate semi-join duplicates.
  
  SYNOPSIS
    setup_semijoin_dups_elimination()
      join           Join to process
      options        Join options (needed to see if join buffering will be 
                     used or not)
      no_jbuf_after  Another bit of information re where join buffering will
                     be used.

  DESCRIPTION
    Setup the strategies to eliminate semi-join duplicates. ATM there are 4
    strategies:

    1. DuplicateWeedout (use of temptable to remove duplicates based on rowids
                         of row combinations)
    2. FirstMatch (pick only the 1st matching row combination of inner tables)
    3. LooseScan (scanning the sj-inner table in a way that groups duplicates
                  together and picking the 1st one)
    4. SJ-Materialization.
    
    The join order has "duplicate-generating ranges", and every range is
    served by one strategy or a combination of FirstMatch with with some
    other strategy.
    
    "Duplicate-generating range" is defined as a range within the join order
    that contains all of the inner tables of a semi-join. All ranges must be
    disjoint, if tables of several semi-joins are interleaved, then the ranges
    are joined together, which is equivalent to converting
      SELECT ... WHERE oe1 IN (SELECT ie1 ...) AND oe2 IN (SELECT ie2 )
    to
      SELECT ... WHERE (oe1, oe2) IN (SELECT ie1, ie2 ... ...)
    .

    Applicability conditions are as follows:

    DuplicateWeedout strategy
    ~~~~~~~~~~~~~~~~~~~~~~~~~

      (ot|nt)*  [ it ((it|ot|nt)* (it|ot))]  (nt)*
      +------+  +=========================+  +---+
        (1)                 (2)               (3)

       (1) - Prefix of OuterTables (those that participate in 
             IN-equality and/or are correlated with subquery) and outer 
             Non-correlated tables.
       (2) - The handled range. The range starts with the first sj-inner
             table, and covers all sj-inner and outer tables 
             Within the range,  Inner, Outer, outer non-correlated tables
             may follow in any order.
       (3) - The suffix of outer non-correlated tables.
    
    FirstMatch strategy
    ~~~~~~~~~~~~~~~~~~~

      (ot|nt)*  [ it ((it|nt)* it) ]  (nt)*
      +------+  +==================+  +---+
        (1)             (2)          (3)

      (1) - Prefix of outer and non-correlated tables
      (2) - The handled range, which may contain only inner and
            non-correlated tables.
      (3) - The suffix of outer non-correlated tables.

    LooseScan strategy 
    ~~~~~~~~~~~~~~~~~~

     (ot|ct|nt) [ loosescan_tbl (ot|nt|it)* it ]  (ot|nt)*
     +--------+   +===========+ +=============+   +------+
        (1)           (2)          (3)              (4)
     
      (1) - Prefix that may contain any outer tables. The prefix must contain
            all the non-trivially correlated outer tables. (non-trivially means
            that the correlation is not just through the IN-equality).
      
      (2) - Inner table for which the LooseScan scan is performed.

      (3) - The remainder of the duplicate-generating range. It is served by 
            application of FirstMatch strategy, with the exception that
            outer IN-correlated tables are considered to be non-correlated.

      (4) - THe suffix of outer and outer non-correlated tables.

  
  The choice between the strategies is made by the join optimizer (see
  advance_sj_state() and fix_semijoin_strategies_for_picked_join_order()).
  This function sets up all fields/structures/etc needed for execution except
  for setup/initialization of semi-join materialization which is done in 
  setup_sj_materialization() (todo: can't we move that to here also?)

  RETURN
    FALSE  OK 
    TRUE   Out of memory error
*/

int setup_semijoin_dups_elimination(JOIN *join, ulonglong options, 
                                    uint no_jbuf_after)
{
  uint i;
  DBUG_ENTER("setup_semijoin_dups_elimination");
  
  join->complex_firstmatch_tables= table_map(0);

  POSITION *pos= join->best_positions + join->const_tables;
  for (i= join->const_tables ; i < join->top_join_tab_count; )
  {
    JOIN_TAB *tab=join->join_tab + i;
    switch (pos->sj_strategy) {
      case SJ_OPT_MATERIALIZE:
      case SJ_OPT_MATERIALIZE_SCAN:
        /* Do nothing */
        i+= 1;// It used to be pos->n_sj_tables, but now they are embedded in a nest
        pos += pos->n_sj_tables;
        break;
      case SJ_OPT_LOOSE_SCAN:
      {
        /* Setup already handled by setup_semijoin_loosescan */
        i+= pos->n_sj_tables;
        pos+= pos->n_sj_tables;
        break;
      }
      case SJ_OPT_DUPS_WEEDOUT:
      {
        /*
          Check for join buffering. If there is one, move the first table
          forwards, but do not destroy other duplicate elimination methods.
        */
        uint first_table= i;

        uint join_cache_level= join->thd->variables.join_cache_level;
        for (uint j= i; j < i + pos->n_sj_tables; j++)
        {
          /*
            When we'll properly take join buffering into account during
            join optimization, the below check should be changed to 
            "if (join->best_positions[j].use_join_buffer && 
                 j <= no_jbuf_after)".
            For now, use a rough criteria:
          */
          JOIN_TAB *js_tab=join->join_tab + j; 
          if (j != join->const_tables && js_tab->use_quick != 2 &&
              j <= no_jbuf_after &&
              ((js_tab->type == JT_ALL && join_cache_level != 0) ||
               (join_cache_level > 2 && (js_tab->type == JT_REF || 
                                         js_tab->type == JT_EQ_REF))))
          {
            /* Looks like we'll be using join buffer */
            first_table= join->const_tables;
            /* 
              Make sure that possible sorting of rows from the head table 
              is not to be employed.
            */
            if (join->get_sort_by_join_tab())
	    {
              join->simple_order= 0;
              join->simple_group= 0;
              join->need_tmp= join->test_if_need_tmp_table();
            }
            break;
          }
        }

        init_dups_weedout(join, first_table, i, i + pos->n_sj_tables - first_table);
        i+= pos->n_sj_tables;
        pos+= pos->n_sj_tables;
        break;
      }
      case SJ_OPT_FIRST_MATCH:
      {
        JOIN_TAB *j;
        JOIN_TAB *jump_to= tab-1;

        bool complex_range= FALSE;
        table_map tables_in_range= table_map(0);

        for (j= tab; j != tab + pos->n_sj_tables; j++)
        {
          tables_in_range |= j->table->map;
          if (!j->emb_sj_nest)
          {
            /* 
              Got a table that's not within any semi-join nest. This is a case
              like this:

              SELECT * FROM ot1, nt1 WHERE ot1.col IN (SELECT expr FROM it1, it2)

              with a join order of 

                   +----- FirstMatch range ----+
                   |                           |
              ot1 it1 nt1 nt2 it2 it3 ...
                   |   ^
                   |   +-------- 'j' points here
                   +------------- SJ_OPT_FIRST_MATCH was set for this table as
                                  it's the first one that produces duplicates
              
            */
            DBUG_ASSERT(j != tab);  /* table ntX must have an itX before it */

            /* 
              If the table right before us is an inner table (like it1 in the
              picture), it should be set to jump back to previous outer-table
            */
            if (j[-1].emb_sj_nest)
              j[-1].do_firstmatch= jump_to;

            jump_to= j; /* Jump back to us */
            complex_range= TRUE;
          }
          else
          {
            j->first_sj_inner_tab= tab;
            j->last_sj_inner_tab= tab + pos->n_sj_tables - 1;
          }
        }
        j[-1].do_firstmatch= jump_to;
        i+= pos->n_sj_tables;
        pos+= pos->n_sj_tables;

        if (complex_range)
          join->complex_firstmatch_tables|= tables_in_range;
        break;
      }
      case SJ_OPT_NONE:
        i++;
        pos++;
        break;
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  Destroy all temporary tables created by NL-semijoin runtime
*/

void destroy_sj_tmp_tables(JOIN *join)
{
  List_iterator<TABLE> it(join->sj_tmp_tables);
  TABLE *table;
  while ((table= it++))
  {
    /* 
      SJ-Materialization tables are initialized for either sequential reading 
      or index lookup, DuplicateWeedout tables are not initialized for read 
      (we only write to them), so need to call ha_index_or_rnd_end.
    */
    table->file->ha_index_or_rnd_end();
    free_tmp_table(join->thd, table);
  }
  join->sj_tmp_tables.empty();
  join->sjm_info_list.empty();
}


/*
  Remove all records from all temp tables used by NL-semijoin runtime

  SYNOPSIS
    clear_sj_tmp_tables()
      join  The join to remove tables for

  DESCRIPTION
    Remove all records from all temp tables used by NL-semijoin runtime. This 
    must be done before every join re-execution.
*/

int clear_sj_tmp_tables(JOIN *join)
{
  int res;
  List_iterator<TABLE> it(join->sj_tmp_tables);
  TABLE *table;
  while ((table= it++))
  {
    if ((res= table->file->ha_delete_all_rows()))
      return res; /* purecov: inspected */
  }

  SJ_MATERIALIZATION_INFO *sjm;
  List_iterator<SJ_MATERIALIZATION_INFO> it2(join->sjm_info_list);
  while ((sjm= it2++))
  {
    sjm->materialized= FALSE;
  }
  return 0;
}


/*
  Check if the table's rowid is included in the temptable

  SYNOPSIS
    sj_table_is_included()
      join      The join
      join_tab  The table to be checked

  DESCRIPTION
    SemiJoinDuplicateElimination: check the table's rowid should be included
    in the temptable. This is so if

    1. The table is not embedded within some semi-join nest
    2. The has been pulled out of a semi-join nest, or

    3. The table is functionally dependent on some previous table

    [4. This is also true for constant tables that can't be
        NULL-complemented but this function is not called for such tables]

  RETURN
    TRUE  - Include table's rowid
    FALSE - Don't
*/

static bool sj_table_is_included(JOIN *join, JOIN_TAB *join_tab)
{
  if (join_tab->emb_sj_nest)
    return FALSE;
  
  /* Check if this table is functionally dependent on the tables that
     are within the same outer join nest
  */
  TABLE_LIST *embedding= join_tab->table->pos_in_table_list->embedding;
  if (join_tab->type == JT_EQ_REF)
  {
    table_map depends_on= 0;
    uint idx;

    for (uint kp= 0; kp < join_tab->ref.key_parts; kp++)
      depends_on |= join_tab->ref.items[kp]->used_tables();

    Table_map_iterator it(depends_on & ~PSEUDO_TABLE_BITS);
    while ((idx= it.next_bit())!=Table_map_iterator::BITMAP_END)
    {
      JOIN_TAB *ref_tab= join->map2table[idx];
      if (embedding != ref_tab->table->pos_in_table_list->embedding)
        return TRUE;
    }
    /* Ok, functionally dependent */
    return FALSE;
  }
  /* Not functionally dependent => need to include*/
  return TRUE;
}


/*
  Index lookup-based subquery: save some flags for EXPLAIN output

  SYNOPSIS
    save_index_subquery_explain_info()
      join_tab  Subquery's join tab (there is only one as index lookup is
                only used for subqueries that are single-table SELECTs)
      where     Subquery's WHERE clause

  DESCRIPTION
    For index lookup-based subquery (i.e. one executed with
    subselect_uniquesubquery_engine or subselect_indexsubquery_engine),
    check its EXPLAIN output row should contain 
      "Using index" (TAB_INFO_FULL_SCAN_ON_NULL) 
      "Using Where" (TAB_INFO_USING_WHERE)
      "Full scan on NULL key" (TAB_INFO_FULL_SCAN_ON_NULL)
    and set appropriate flags in join_tab->packed_info.
*/

static void save_index_subquery_explain_info(JOIN_TAB *join_tab, Item* where)
{
  join_tab->packed_info= TAB_INFO_HAVE_VALUE;
  if (join_tab->table->covering_keys.is_set(join_tab->ref.key))
    join_tab->packed_info |= TAB_INFO_USING_INDEX;
  if (where)
    join_tab->packed_info |= TAB_INFO_USING_WHERE;
  for (uint i = 0; i < join_tab->ref.key_parts; i++)
  {
    if (join_tab->ref.cond_guards[i])
    {
      join_tab->packed_info |= TAB_INFO_FULL_SCAN_ON_NULL;
      break;
    }
  }
}


/*
  Check if the join can be rewritten to [unique_]indexsubquery_engine

  DESCRIPTION
    Check if the join can be changed into [unique_]indexsubquery_engine.

    The check is done after join optimization, the idea is that if the join
    has only one table and uses a [eq_]ref access generated from subselect's
    IN-equality then we replace it with a subselect_indexsubquery_engine or a
    subselect_uniquesubquery_engine.

  RETURN 
    0 - Ok, rewrite done (stop join optimization and return)
    1 - Fatal error (stop join optimization and return)
   -1 - No rewrite performed, continue with join optimization
*/

int rewrite_to_index_subquery_engine(JOIN *join)
{
  THD *thd= join->thd;
  JOIN_TAB* join_tab=join->join_tab;
  SELECT_LEX_UNIT *unit= join->unit;
  DBUG_ENTER("rewrite_to_index_subquery_engine");

  /*
    is this simple IN subquery?
  */
  /* TODO: In order to use these more efficient subquery engines in more cases,
     the following problems need to be solved:
     - the code that removes GROUP BY (group_list), also adds an ORDER BY
       (order), thus GROUP BY queries (almost?) never pass through this branch.
       Solution: remove the test below '!join->order', because we remove the
       ORDER clase for subqueries anyway.
     - in order to set a more efficient engine, the optimizer needs to both
       decide to remove GROUP BY, *and* select one of the JT_[EQ_]REF[_OR_NULL]
       access methods, *and* loose scan should be more expensive or
       inapliccable. When is that possible?
     - Consider expanding the applicability of this rewrite for loose scan
       for group by queries.
  */
  if (!join->group_list && !join->order &&
      join->unit->item && 
      join->unit->item->substype() == Item_subselect::IN_SUBS &&
      join->table_count == 1 && join->conds &&
      !join->unit->is_unit_op())
  {
    if (!join->having)
    {
      Item *where= join->conds;
      if (join_tab[0].type == JT_EQ_REF &&
	  join_tab[0].ref.items[0]->name.str == in_left_expr_name.str)
      {
        remove_subq_pushed_predicates(join, &where);
        save_index_subquery_explain_info(join_tab, where);
        join_tab[0].type= JT_UNIQUE_SUBQUERY;
        join->error= 0;
        DBUG_RETURN(unit->item->
                    change_engine(new
                                  subselect_uniquesubquery_engine(thd,
                                                                  join_tab,
                                                                  unit->item->get_IN_subquery(),
                                                                  where)));
      }
      else if (join_tab[0].type == JT_REF &&
	       join_tab[0].ref.items[0]->name.str == in_left_expr_name.str)
      {
	remove_subq_pushed_predicates(join, &where);
        save_index_subquery_explain_info(join_tab, where);
        join_tab[0].type= JT_INDEX_SUBQUERY;
        join->error= 0;
        DBUG_RETURN(unit->item->
                    change_engine(new
                                  subselect_indexsubquery_engine(thd,
                                                                 join_tab,
                                                                 unit->item->get_IN_subquery(),
                                                                 where,
                                                                 NULL,
                                                                 0)));
      }
    } else if (join_tab[0].type == JT_REF_OR_NULL &&
	       join_tab[0].ref.items[0]->name.str == in_left_expr_name.str &&
               join->having->name.str == in_having_cond.str)
    {
      join_tab[0].type= JT_INDEX_SUBQUERY;
      join->error= 0;
      join->conds= remove_additional_cond(join->conds);
      save_index_subquery_explain_info(join_tab, join->conds);
      DBUG_RETURN(unit->item->
		  change_engine(new subselect_indexsubquery_engine(thd,
								   join_tab,
								   unit->item->get_IN_subquery(),
								   join->conds,
                                                                   join->having,
								   1)));
    }
  }

  DBUG_RETURN(-1); /* Haven't done the rewrite */
}


/**
  Remove additional condition inserted by IN/ALL/ANY transformation.

  @param conds   condition for processing

  @return
    new conditions
*/

static Item *remove_additional_cond(Item* conds)
{
  if (conds->name.str == in_additional_cond.str)
    return 0;
  if (conds->type() == Item::COND_ITEM)
  {
    Item_cond *cnd= (Item_cond*) conds;
    List_iterator<Item> li(*(cnd->argument_list()));
    Item *item;
    while ((item= li++))
    {
      if (item->name.str == in_additional_cond.str)
      {
	li.remove();
	if (cnd->argument_list()->elements == 1)
	  return cnd->argument_list()->head();
	return conds;
      }
    }
  }
  return conds;
}


/*
  Remove the predicates pushed down into the subquery

  SYNOPSIS
    remove_subq_pushed_predicates()
      where   IN  Must be NULL
              OUT The remaining WHERE condition, or NULL

  DESCRIPTION
    Given that this join will be executed using (unique|index)_subquery,
    without "checking NULL", remove the predicates that were pushed down
    into the subquery.

    If the subquery compares scalar values, we can remove the condition that
    was wrapped into trig_cond (it will be checked when needed by the subquery
    engine)

    If the subquery compares row values, we need to keep the wrapped
    equalities in the WHERE clause: when the left (outer) tuple has both NULL
    and non-NULL values, we'll do a full table scan and will rely on the
    equalities corresponding to non-NULL parts of left tuple to filter out
    non-matching records.

    TODO: We can remove the equalities that will be guaranteed to be true by the
    fact that subquery engine will be using index lookup. This must be done only
    for cases where there are no conversion errors of significance, e.g. 257
    that is searched in a byte. But this requires homogenization of the return 
    codes of all Field*::store() methods.
*/

static void remove_subq_pushed_predicates(JOIN *join, Item **where)
{
  if (join->conds->type() == Item::FUNC_ITEM &&
      ((Item_func *)join->conds)->functype() == Item_func::EQ_FUNC &&
      ((Item_func *)join->conds)->arguments()[0]->type() == Item::REF_ITEM &&
      ((Item_func *)join->conds)->arguments()[1]->type() == Item::FIELD_ITEM &&
      test_if_ref (join->conds,
                   (Item_field *)((Item_func *)join->conds)->arguments()[1],
                   ((Item_func *)join->conds)->arguments()[0]))
  {
    *where= 0;
    return;
  }
}




/**
  Optimize all subqueries of a query that were not flattened into a semijoin.

  @details
  Optimize all immediate children subqueries of a query.

  This phase must be called after substitute_for_best_equal_field() because
  that function may replace items with other items from a multiple equality,
  and we need to reference the correct items in the index access method of the
  IN predicate.

  @return Operation status
  @retval FALSE     success.
  @retval TRUE      error occurred.
*/

bool JOIN::optimize_unflattened_subqueries()
{
  return select_lex->optimize_unflattened_subqueries(false);
}

/**
  Optimize all constant subqueries of a query that were not flattened into
  a semijoin.

  @details
  Similar to other constant conditions, constant subqueries can be used in
  various constant optimizations. Having optimized constant subqueries before
  these constant optimizations, makes it possible to estimate if a subquery
  is "cheap" enough to be executed during the optimization phase.

  Constant subqueries can be optimized and evaluated independent of the outer
  query, therefore if const_only = true, this method can be called early in
  the optimization phase of the outer query.

  @return Operation status
  @retval FALSE     success.
  @retval TRUE      error occurred.
*/
 
bool JOIN::optimize_constant_subqueries()
{
  ulonglong save_options= select_lex->options;
  bool res;
  /*
    Constant subqueries may be executed during the optimization phase.
    In EXPLAIN mode the optimizer doesn't initialize many of the data structures
    needed for execution. In order to make it possible to execute subqueries
    during optimization, constant subqueries must be optimized for execution,
    not for EXPLAIN.
  */
  select_lex->options&= ~SELECT_DESCRIBE;
  res= select_lex->optimize_unflattened_subqueries(true);
  select_lex->options= save_options;
  return res;
}


/*
  Join tab execution startup function.

  SYNOPSIS
    join_tab_execution_startup()
      tab  Join tab to perform startup actions for

  DESCRIPTION
    Join tab execution startup function. This is different from
    tab->read_first_record in the regard that this has actions that are to be
    done once per join execution.

    Currently there are only two possible startup functions, so we have them
    both here inside if (...) branches. In future we could switch to function
    pointers.

  TODO: consider moving this together with JOIN_TAB::preread_init
  
  RETURN 
    NESTED_LOOP_OK - OK
    NESTED_LOOP_ERROR| NESTED_LOOP_KILLED - Error, abort the join execution
*/

enum_nested_loop_state join_tab_execution_startup(JOIN_TAB *tab)
{
  Item_in_subselect *in_subs;
  DBUG_ENTER("join_tab_execution_startup");
  
  if (tab->table->pos_in_table_list && 
      (in_subs= tab->table->pos_in_table_list->jtbm_subselect))
  {
    /* It's a non-merged SJM nest */
    DBUG_ASSERT(in_subs->engine->engine_type() ==
                subselect_engine::HASH_SJ_ENGINE);
    subselect_hash_sj_engine *hash_sj_engine=
      ((subselect_hash_sj_engine*)in_subs->engine);
    if (!hash_sj_engine->is_materialized)
    {
      hash_sj_engine->materialize_join->exec();
      hash_sj_engine->is_materialized= TRUE; 

      if (unlikely(hash_sj_engine->materialize_join->error) ||
          unlikely(tab->join->thd->is_fatal_error))
        DBUG_RETURN(NESTED_LOOP_ERROR);
    }
  }
  else if (tab->bush_children)
  {
    /* It's a merged SJM nest */
    enum_nested_loop_state rc;
    SJ_MATERIALIZATION_INFO *sjm= tab->bush_children->start->emb_sj_nest->sj_mat_info;

    if (!sjm->materialized)
    {
      JOIN *join= tab->join;
      JOIN_TAB *join_tab= tab->bush_children->start;
      JOIN_TAB *save_return_tab= join->return_tab;
      /*
        Now run the join for the inner tables. The first call is to run the
        join, the second one is to signal EOF (this is essential for some
        join strategies, e.g. it will make join buffering flush the records)
      */
      if ((rc= sub_select(join, join_tab, FALSE/* no EOF */)) < 0 ||
          (rc= sub_select(join, join_tab, TRUE/* now EOF */)) < 0)
      {
        join->return_tab= save_return_tab;
        DBUG_RETURN(rc); /* it's NESTED_LOOP_(ERROR|KILLED)*/
      }
      join->return_tab= save_return_tab;
      sjm->materialized= TRUE;
    }
  }

  DBUG_RETURN(NESTED_LOOP_OK);
}


/*
  Create a dummy temporary table, useful only for the sake of having a 
  TABLE* object with map,tablenr and maybe_null properties.
  
  This is used by non-mergeable semi-join materilization code to handle
  degenerate cases where materialized subquery produced "Impossible WHERE" 
  and thus wasn't materialized.
*/

TABLE *create_dummy_tmp_table(THD *thd)
{
  DBUG_ENTER("create_dummy_tmp_table");
  TABLE *table;
  TMP_TABLE_PARAM sjm_table_param;
  sjm_table_param.init();
  sjm_table_param.field_count= 1;
  List<Item> sjm_table_cols;
  const LEX_CSTRING dummy_name= { STRING_WITH_LEN("dummy") };
  Item *column_item= new (thd->mem_root) Item_int(thd, 1);
  if (!column_item)
    DBUG_RETURN(NULL);

  sjm_table_cols.push_back(column_item, thd->mem_root);
  if (!(table= create_tmp_table(thd, &sjm_table_param, 
                                sjm_table_cols, (ORDER*) 0, 
                                TRUE /* distinct */, 
                                1, /*save_sum_fields*/
                                thd->variables.option_bits |
                                TMP_TABLE_ALL_COLUMNS, 
                                HA_POS_ERROR /*rows_limit */, 
                                &dummy_name, TRUE /* Do not open */)))
  {
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(table);
}


/*
  A class that is used to catch one single tuple that is sent to the join
  output, and save it in Item_cache element(s).

  It is very similar to select_singlerow_subselect but doesn't require a 
  Item_singlerow_subselect item.
*/

class select_value_catcher :public select_subselect
{
public:
  select_value_catcher(THD *thd_arg, Item_subselect *item_arg):
    select_subselect(thd_arg, item_arg)
  {}
  int send_data(List<Item> &items);
  int setup(List<Item> *items);
  bool assigned;  /* TRUE <=> we've caught a value */
  uint n_elements; /* How many elements we get */
  Item_cache **row; /* Array of cache elements */
};


int select_value_catcher::setup(List<Item> *items)
{
  assigned= FALSE;
  n_elements= items->elements;
 
  if (!(row= (Item_cache**) thd->alloc(sizeof(Item_cache*) * n_elements)))
    return TRUE;
  
  Item *sel_item;
  List_iterator<Item> li(*items);
  for (uint i= 0; (sel_item= li++); i++)
  {
    if (!(row[i]= sel_item->get_cache(thd)))
      return TRUE;
    row[i]->setup(thd, sel_item);
  }
  return FALSE;
}


int select_value_catcher::send_data(List<Item> &items)
{
  DBUG_ENTER("select_value_catcher::send_data");
  DBUG_ASSERT(!assigned);
  DBUG_ASSERT(items.elements == n_elements);

  Item *val_item;
  List_iterator_fast<Item> li(items);
  for (uint i= 0; (val_item= li++); i++)
  {
    row[i]->store(val_item);
    row[i]->cache_value();
  }
  assigned= TRUE;
  DBUG_RETURN(0);
}


/**
  @brief
    Attach conditions to already optimized condition

  @param thd              the thread handle
  @param cond             the condition to which add new conditions
  @param cond_eq          IN/OUT the multiple equalities of cond
  @param new_conds        the list of conditions to be added
  @param cond_value       the returned value of the condition
                          if it can be evaluated

  @details
    The method creates new condition through union of cond and
    the conditions from new_conds list.
    The method is called after optimize_cond() for cond. The result
    of the union should be the same as if it was done before the
    the optimize_cond() call.

  @retval otherwise  the created condition
  @retval NULL       if an error occurs
*/

Item *and_new_conditions_to_optimized_cond(THD *thd, Item *cond,
                                           COND_EQUAL **cond_eq,
                                           List<Item> &new_conds,
                                           Item::cond_result *cond_value)
{
  COND_EQUAL new_cond_equal;
  Item *item;
  Item_equal *mult_eq;
  bool is_simplified_cond= false;
  /* The list where parts of the new condition are stored. */
  List_iterator<Item> li(new_conds);
  List_iterator_fast<Item_equal> it(new_cond_equal.current_level);

  /*
    Create multiple equalities from the equalities of the list new_conds.
    Save the created multiple equalities in new_cond_equal.
    If multiple equality can't be created or the condition
    from new_conds list isn't an equality leave it in new_conds
    list.

    The equality can't be converted into the multiple equality if it
    is a knowingly false or true equality.
    For example, (3 = 1) equality.
  */
  while ((item=li++))
  {
    if (item->type() == Item::FUNC_ITEM &&
        ((Item_func *) item)->functype() == Item_func::EQ_FUNC &&
        check_simple_equality(thd,
                             Item::Context(Item::ANY_SUBST,
                             ((Item_func_eq *)item)->compare_type_handler(),
                             ((Item_func_eq *)item)->compare_collation()),
                             ((Item_func *)item)->arguments()[0],
                             ((Item_func *)item)->arguments()[1],
                             &new_cond_equal))
      li.remove();
  }

  it.rewind();
  if (cond && cond->type() == Item::COND_ITEM &&
      ((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
  {
    /*
      Case when cond is an AND-condition.
      Union AND-condition cond, created multiple equalities from
      new_cond_equal and remaining conditions from new_conds.
    */
    COND_EQUAL *cond_equal= &((Item_cond_and *) cond)->m_cond_equal;
    List<Item_equal> *cond_equalities= &cond_equal->current_level;
    List<Item> *and_args= ((Item_cond_and *)cond)->argument_list();

    /*
      Disjoin multiple equalities of cond.
      Merge these multiple equalities with the multiple equalities of
      new_cond_equal. Save the result in new_cond_equal.
      Check if after the merge some multiple equalities are knowingly
      true or false.
    */
    and_args->disjoin((List<Item> *) cond_equalities);
    while ((mult_eq= it++))
    {
      mult_eq->upper_levels= 0;
      mult_eq->merge_into_list(thd, cond_equalities, false, false);
    }
    List_iterator_fast<Item_equal> ei(*cond_equalities);
    while ((mult_eq= ei++))
    {
      if (mult_eq->const_item() && !mult_eq->val_int())
        is_simplified_cond= true;
      else
      {
        mult_eq->unfix_fields();
        if (mult_eq->fix_fields(thd, NULL))
          return NULL;
      }
    }

    li.rewind();
    while ((item=li++))
    {
      /*
        There still can be some equalities at not top level of new_conds
        conditions that are not transformed into multiple equalities.
        To transform them build_item_equal() is called.

        Examples of not top level equalities:

        1. (t1.a = 3) OR (t1.b > 5)
            (t1.a = 3) - not top level equality.
            It is inside OR condition

        2. ((t3.d = t3.c) AND (t3.c < 15)) OR (t3.d > 1)
           (t1.d = t3.c) - not top level equality.
           It is inside AND condition which is a part of OR condition
      */
      if (item->type() == Item::COND_ITEM &&
          ((Item_cond *)item)->functype() == Item_func::COND_OR_FUNC)
      {
        item= item->build_equal_items(thd,
                                      &((Item_cond_and *) cond)->m_cond_equal,
                                      false, NULL);
      }
      and_args->push_back(item, thd->mem_root);
    }
    and_args->append((List<Item> *) cond_equalities);
    *cond_eq= &((Item_cond_and *) cond)->m_cond_equal;
  }
  else
  {
    /*
      Case when cond isn't an AND-condition or is NULL.
      There can be several cases:

      1. cond is a multiple equality.
         In this case merge cond with the multiple equalities of
         new_cond_equal.
         Create new condition from the created multiple equalities
         and new_conds list conditions.
      2. cond is NULL
         Create new condition from new_conds list conditions
         and multiple equalities from new_cond_equal.
      3. Otherwise
         Create new condition through union of cond, conditions from new_conds
         list and created multiple equalities from new_cond_equal.
    */
    List<Item> new_conds_list;
    /* Flag is set to true if cond is a multiple equality */
    bool is_mult_eq= (cond && cond->type() == Item::FUNC_ITEM &&
        ((Item_func*) cond)->functype() == Item_func::MULT_EQUAL_FUNC);

    /*
      If cond is non-empty and is not multiple equality save it as
      a part of a new condition.
    */
    if (cond && !is_mult_eq &&
        new_conds_list.push_back(cond, thd->mem_root))
      return NULL;

    /*
      If cond is a multiple equality merge it with new_cond_equal
      multiple equalities.
    */
    if (is_mult_eq)
    {
      Item_equal *eq_cond= (Item_equal *)cond;
      eq_cond->upper_levels= 0;
      eq_cond->merge_into_list(thd, &new_cond_equal.current_level,
                               false, false);
    }

    /**
      Fix created multiple equalities and check if they are knowingly
      true or false.
    */
    List_iterator_fast<Item_equal> ei(new_cond_equal.current_level);
    while ((mult_eq=ei++))
    {
      if (mult_eq->const_item() && !mult_eq->val_int())
        is_simplified_cond= true;
      else
      {
        mult_eq->unfix_fields();
        if (mult_eq->fix_fields(thd, NULL))
          return NULL;
      }
    }

    /*
      Create AND condition if new condition will have two or
      more elements.
    */
    Item_cond_and *and_cond= 0;
    COND_EQUAL *inherited= 0;
    if (new_conds_list.elements +
        new_conds.elements +
        new_cond_equal.current_level.elements > 1)
    {
      and_cond= new (thd->mem_root) Item_cond_and(thd);
      and_cond->m_cond_equal.copy(new_cond_equal);
      inherited= &and_cond->m_cond_equal;
    }

    li.rewind();
    while ((item=li++))
    {
      /*
        Look for the comment in the case when cond is an
        AND condition above the build_equal_items() call.
      */
      if (item->type() == Item::COND_ITEM &&
          ((Item_cond *)item)->functype() == Item_func::COND_OR_FUNC)
      {
        item= item->build_equal_items(thd, inherited, false, NULL);
      }
      new_conds_list.push_back(item, thd->mem_root);
    }
    new_conds_list.append((List<Item> *)&new_cond_equal.current_level);

    if (and_cond)
    {
      and_cond->argument_list()->append(&new_conds_list);
      cond= (Item *)and_cond;
      *cond_eq= &((Item_cond_and *) cond)->m_cond_equal;
    }
    else
    {
      List_iterator_fast<Item> iter(new_conds_list);
      cond= iter++;
      if (cond->type() == Item::FUNC_ITEM &&
          ((Item_func *)cond)->functype() == Item_func::MULT_EQUAL_FUNC)
      {
        if (!(*cond_eq))
          *cond_eq= new COND_EQUAL();
        (*cond_eq)->copy(new_cond_equal);
      }
      else
        *cond_eq= 0;
    }
  }

  if (!cond)
    return NULL;

  if (*cond_eq)
  {
    /*
      The multiple equalities are attached only to the upper level
      of AND-condition cond.
      Push them down to the bottom levels of cond AND-condition if needed.
    */
    propagate_new_equalities(thd, cond,
                             &(*cond_eq)->current_level,
                             0,
                             &is_simplified_cond);
    cond= cond->propagate_equal_fields(thd,
                                       Item::Context_boolean(),
                                       *cond_eq);
    cond->update_used_tables();
  }
  /* Check if conds has knowingly true or false parts. */
  if (cond &&
      !is_simplified_cond &&
      cond->walk(&Item::is_simplified_cond_processor, 0, 0))
    is_simplified_cond= true;


  /*
    If it was found that there are some knowingly true or false equalities
    remove them  from cond and set cond_value to the appropriate value.
  */
  if (cond && is_simplified_cond)
    cond= cond->remove_eq_conds(thd, cond_value, true);

  if (cond && cond->fix_fields_if_needed(thd, NULL))
    return NULL;

  return cond;
}


/**
  @brief  Materialize a degenerate jtbm semi join

  @param thd        thread handler
  @param tbl        table list for the target jtbm semi join table
  @param subq_pred  IN subquery predicate with the degenerate jtbm semi join
  @param eq_list    IN/OUT the list where to add produced equalities

  @details
    The method materializes the degenerate jtbm semi join for the
    subquery from the IN subquery predicate subq_pred taking table
    as the target for materialization.
    Any degenerate table is guaranteed to produce 0 or 1 record.
    Examples of both cases:

    select * from ot where col in (select ... from it where 2>3)
    select * from ot where col in (select MY_MIN(it.key) from it)

    in this case, there is no necessity to create a temp.table for
    materialization.
    We now just need to
    1. Check whether 1 or 0 records are produced, setup this as a
       constant join tab.
    2. Create a dummy temporary table, because all of the join
       optimization code relies on TABLE object being present.

    In the case when materialization produces one row the function
    additionally creates equalities between the expressions from the
    left part of the IN subquery predicate and the corresponding
    columns of the produced row. These equalities are added to the
    list eq_list. They are supposed to be conjuncted with the condition
    of the WHERE clause.

  @retval TRUE   if an error occurs
  @retval FALSE  otherwise
*/

bool execute_degenerate_jtbm_semi_join(THD *thd,
                                       TABLE_LIST *tbl,
                                       Item_in_subselect *subq_pred,
                                       List<Item> &eq_list)
{
  DBUG_ENTER("execute_degenerate_jtbm_semi_join");
  select_value_catcher *new_sink;

  DBUG_ASSERT(subq_pred->engine->engine_type() ==
              subselect_engine::SINGLE_SELECT_ENGINE);
  subselect_single_select_engine *engine=
    (subselect_single_select_engine*)subq_pred->engine;
  if (!(new_sink= new (thd->mem_root) select_value_catcher(thd, subq_pred)))
    DBUG_RETURN(TRUE);
  if (new_sink->setup(&engine->select_lex->join->fields_list) ||
      engine->select_lex->join->change_result(new_sink, NULL) ||
      engine->exec())
  {
    DBUG_RETURN(TRUE);
  }
  subq_pred->is_jtbm_const_tab= TRUE;

  if (new_sink->assigned)
  {
    /*
      Subselect produced one row, which is saved in new_sink->row.
      Save "left_expr[i] == row[i]" equalities into the eq_list.
    */
    subq_pred->jtbm_const_row_found= TRUE;

    Item *eq_cond;
    Item *left_exp= subq_pred->left_exp();
    uint ncols= left_exp->cols();
    for (uint i= 0; i < ncols; i++)
    {
      eq_cond=
        new (thd->mem_root) Item_func_eq(thd,
                                         left_exp->element_index(i),
                                         new_sink->row[i]);
      if (!eq_cond || eq_cond->fix_fields(thd, NULL) ||
          eq_list.push_back(eq_cond, thd->mem_root))
        DBUG_RETURN(TRUE);
    }
  }
  else
  {
    /* Subselect produced no rows. Just set the flag */
    subq_pred->jtbm_const_row_found= FALSE;
  }

  TABLE *dummy_table;
  if (!(dummy_table= create_dummy_tmp_table(thd)))
    DBUG_RETURN(TRUE);
  tbl->table= dummy_table;
  tbl->table->pos_in_table_list= tbl;
  /*
    Note: the table created above may be freed by:
    1. JOIN_TAB::cleanup(), when the parent join is a regular join.
    2. cleanup_empty_jtbm_semi_joins(), when the parent join is a
       degenerate join (e.g. one with "Impossible where").
  */
  setup_table_map(tbl->table, tbl, tbl->jtbm_table_no);
  DBUG_RETURN(FALSE);
}


/**
  @brief
    Execute degenerate jtbm semi joins before optimize_cond() for parent

  @param join       the parent join for jtbm semi joins
  @param join_list  the list of tables where jtbm semi joins are processed
  @param eq_list    IN/OUT the list where to add equalities produced after
                    materialization of single-row degenerate jtbm semi joins

  @details
    The method traverses join_list trying to find any degenerate jtbm semi
    joins for subqueries of IN predicates. For each degenerate jtbm
    semi join execute_degenerate_jtbm_semi_join() is called. As a result
    of this call new equalities that substitute for single-row materialized
    jtbm semi join are added to eq_list.

    In the case when a table is nested in another table 'nested_join' the
    method is recursively called for the join_list of the 'nested_join' trying
    to find in the list any degenerate jtbm semi joins. Currently a jtbm semi
    join may occur in a mergeable semi join nest.

  @retval TRUE   if an error occurs
  @retval FALSE  otherwise
*/

bool setup_degenerate_jtbm_semi_joins(JOIN *join,
                                      List<TABLE_LIST> *join_list,
				      List<Item> &eq_list)
{
  TABLE_LIST *table;
  NESTED_JOIN *nested_join;
  List_iterator<TABLE_LIST> li(*join_list);
  THD *thd= join->thd;
  DBUG_ENTER("setup_degenerate_jtbm_semi_joins");

  while ((table= li++))
  {
    Item_in_subselect *subq_pred;

    if ((subq_pred= table->jtbm_subselect))
    {
      JOIN *subq_join= subq_pred->unit->first_select()->join;

      if (!subq_join->tables_list || !subq_join->table_count)
      {
        if (execute_degenerate_jtbm_semi_join(thd,
                                              table,
                                              subq_pred,
                                              eq_list))
          DBUG_RETURN(TRUE);
        join->is_orig_degenerated= true;
      }
    }
    if ((nested_join= table->nested_join))
    {
      if (setup_degenerate_jtbm_semi_joins(join,
                                           &nested_join->join_list,
                                           eq_list))
	DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


/**
  @brief
    Optimize jtbm semi joins for materialization

  @param join       the parent join for jtbm semi joins
  @param join_list  the list of TABLE_LIST objects where jtbm semi join
                    can occur
  @param eq_list    IN/OUT the list where to add produced equalities

  @details
    This method is called by the optimizer after the call of
    optimize_cond() for parent select.
    The method traverses join_list trying to find any jtbm semi joins for
    subqueries from IN predicates and optimizes them.
    After the optimization some of jtbm semi joins may become degenerate.
    For example the subquery 'SELECT MAX(b) FROM t2' from the query

    SELECT * FROM t1 WHERE 4 IN (SELECT MAX(b) FROM t2);

    will become degenerate if there is an index on t2.b.
    If a subquery becomes degenerate it is handled by the function
    execute_degenerate_jtbm_semi_join().

    Otherwise the method creates a temporary table in which the subquery
    of the jtbm semi join will be materialied.

    The function saves the equalities between all pairs of the expressions
    from the left part of the IN subquery predicate and the corresponding
    columns of the subquery from the predicate in eq_list appending them
    to the list. The equalities of eq_list will be later conjucted with the
    condition of the WHERE clause.

    In the case when a table is nested in another table 'nested_join' the
    method is recursively called for the join_list of the 'nested_join' trying
    to find in the list any degenerate jtbm semi joins. Currently a jtbm semi
    join may occur in a mergeable semi join nest.

  @retval TRUE   if an error occurs
  @retval FALSE  otherwise
*/

bool setup_jtbm_semi_joins(JOIN *join, List<TABLE_LIST> *join_list,
                           List<Item> &eq_list)
{
  TABLE_LIST *table;
  NESTED_JOIN *nested_join;
  List_iterator<TABLE_LIST> li(*join_list);
  THD *thd= join->thd;
  DBUG_ENTER("setup_jtbm_semi_joins");

  while ((table= li++))
  {
    Item_in_subselect *subq_pred;
    
    if ((subq_pred= table->jtbm_subselect))
    {
      double rows;
      double read_time;

      /*
        Perform optimization of the subquery, so that we know estimated
        - cost of materialization process 
        - how many records will be in the materialized temp.table
      */
      if (subq_pred->optimize(&rows, &read_time))
        DBUG_RETURN(TRUE);

      subq_pred->jtbm_read_time= read_time;
      subq_pred->jtbm_record_count=rows;
      JOIN *subq_join= subq_pred->unit->first_select()->join;

      if (!subq_join->tables_list || !subq_join->table_count)
      {
        if (!join->is_orig_degenerated &&
            execute_degenerate_jtbm_semi_join(thd, table, subq_pred,
                                               eq_list))
          DBUG_RETURN(TRUE);
      }
      else
      {
        DBUG_ASSERT(subq_pred->test_set_strategy(SUBS_MATERIALIZATION));
        subq_pred->is_jtbm_const_tab= FALSE;
        subselect_hash_sj_engine *hash_sj_engine=
          ((subselect_hash_sj_engine*)subq_pred->engine);
        
        table->table= hash_sj_engine->tmp_table;
        table->table->pos_in_table_list= table;

        setup_table_map(table->table, table, table->jtbm_table_no);

        List_iterator<Item> li(*hash_sj_engine->semi_join_conds->argument_list());
        Item *item;
        while ((item=li++))
        {
          item->update_used_tables();
          if (eq_list.push_back(item, thd->mem_root))
            DBUG_RETURN(TRUE);
        }
      }
      table->table->maybe_null= MY_TEST(join->mixed_implicit_grouping);
    }
    if ((nested_join= table->nested_join))
    {
      if (setup_jtbm_semi_joins(join, &nested_join->join_list, eq_list))
        DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  Cleanup non-merged semi-joins (JBMs) that have empty.

  This function is to cleanups for a special case:  
  Consider a query like 

    select * from t1 where 1=2 AND t1.col IN (select max(..) ... having 1=2)

  For this query, optimization of subquery will short-circuit, and 
  setup_jtbm_semi_joins() will call create_dummy_tmp_table() so that we have
  empty, constant temp.table to stand in as materialized temp. table.

  Now, suppose that the upper join is also found to be degenerate. In that
  case, no JOIN_TAB array will be produced, and hence, JOIN::cleanup() will
  have a problem with cleaning up empty JTBMs (non-empty ones are cleaned up
  through Item::cleanup() calls).
*/

void cleanup_empty_jtbm_semi_joins(JOIN *join, List<TABLE_LIST> *join_list)
{
  List_iterator<TABLE_LIST> li(*join_list);
  TABLE_LIST *table;
  while ((table= li++))
  {
    if ((table->jtbm_subselect && table->jtbm_subselect->is_jtbm_const_tab))
    {
      if (table->table)
      {
        free_tmp_table(join->thd, table->table);
        table->table= NULL;
      }
    }
    else if (table->nested_join && table->sj_subq_pred)
    {
      cleanup_empty_jtbm_semi_joins(join, &table->nested_join->join_list);
    }
  }
}


/**
  Choose an optimal strategy to execute an IN/ALL/ANY subquery predicate
  based on cost.

  @param join_tables  the set of tables joined in the subquery

  @notes
  The method chooses between the materialization and IN=>EXISTS rewrite
  strategies for the execution of a non-flattened subquery IN predicate.
  The cost-based decision is made as follows:

  1. compute materialize_strategy_cost based on the unmodified subquery
  2. reoptimize the subquery taking into account the IN-EXISTS predicates
  3. compute in_exists_strategy_cost based on the reoptimized plan
  4. compare and set the cheaper strategy
     if (materialize_strategy_cost >= in_exists_strategy_cost)
       in_strategy = MATERIALIZATION
     else
       in_strategy = IN_TO_EXISTS
  5. if in_strategy = MATERIALIZATION and it is not possible to initialize it
       revert to IN_TO_EXISTS
  6. if (in_strategy == MATERIALIZATION)
       revert the subquery plan to the original one before reoptimizing
     else
       inject the IN=>EXISTS predicates into the new EXISTS subquery plan

  The implementation itself is a bit more complicated because it takes into
  account two more factors:
  - whether the user allowed both strategies through an optimizer_switch, and
  - if materialization was the cheaper strategy, whether it can be executed
    or not.

  @retval FALSE     success.
  @retval TRUE      error occurred.
*/

bool JOIN::choose_subquery_plan(table_map join_tables)
{
  enum_reopt_result reopt_result= REOPT_NONE;
  Item_in_subselect *in_subs;

  /*
    IN/ALL/ANY optimizations are not applicable for so called fake select
    (this select exists only to filter results of union if it is needed).
  */
  if (select_lex == select_lex->master_unit()->fake_select_lex)
    return 0;

  if (is_in_subquery())
  {
    in_subs= unit->item->get_IN_subquery();
    if (in_subs->create_in_to_exists_cond(this))
      return true;
  }
  else
    return false;

  /* A strategy must be chosen earlier. */
  DBUG_ASSERT(in_subs->has_strategy());
  DBUG_ASSERT(in_to_exists_where || in_to_exists_having);
  DBUG_ASSERT(!in_to_exists_where || in_to_exists_where->fixed());
  DBUG_ASSERT(!in_to_exists_having || in_to_exists_having->fixed());

  /* The original QEP of the subquery. */
  Join_plan_state save_qep(table_count);

  /*
    Compute and compare the costs of materialization and in-exists if both
    strategies are possible and allowed by the user (checked during the prepare
    phase.
  */
  if (in_subs->test_strategy(SUBS_MATERIALIZATION) &&
      in_subs->test_strategy(SUBS_IN_TO_EXISTS))
  {
    JOIN *outer_join;
    JOIN *inner_join= this;
    /* Number of unique value combinations filtered by the IN predicate. */
    double outer_lookup_keys;
    /* Cost and row count of the unmodified subquery. */
    double inner_read_time_1, inner_record_count_1;
    /* Cost of the subquery with injected IN-EXISTS predicates. */
    double inner_read_time_2;
    /* The cost to compute IN via materialization. */
    double materialize_strategy_cost;
    /* The cost of the IN->EXISTS strategy. */
    double in_exists_strategy_cost;
    double dummy;

    /*
      A. Estimate the number of rows of the outer table that will be filtered
      by the IN predicate.
    */
    outer_join= unit->outer_select() ? unit->outer_select()->join : NULL;
    /*
      Get the cost of the outer join if:
      (1) It has at least one table, and
      (2) It has been already optimized (if there is no join_tab, then the
          outer join has not been optimized yet).
    */
    if (outer_join && outer_join->table_count > 0 && // (1)
        outer_join->join_tab &&                      // (2)
        !in_subs->const_item())
    {
      /*
        TODO:
        Currently outer_lookup_keys is computed as the number of rows in
        the partial join including the JOIN_TAB where the IN predicate is
        pushed to. In the general case this is a gross overestimate because
        due to caching we are interested only in the number of unique keys.
        The search key may be formed by columns from much fewer than all
        tables in the partial join. Example:
        select * from t1, t2 where t1.c1 = t2.key AND t2.c2 IN (select ...);
        If the join order: t1, t2, the number of unique lookup keys is ~ to
        the number of unique values t2.c2 in the partial join t1 join t2.
      */
      outer_join->get_partial_cost_and_fanout(in_subs->get_join_tab_idx(),
                                              table_map(-1),
                                              &dummy,
                                              &outer_lookup_keys);
    }
    else
    {
      /*
        TODO: outer_join can be NULL for DELETE statements.
        How to compute its cost?
      */
      outer_lookup_keys= 1;
    }

    /*
      B. Estimate the cost and number of records of the subquery both
      unmodified, and with injected IN->EXISTS predicates.
    */
    inner_read_time_1= inner_join->best_read;
    inner_record_count_1= inner_join->join_record_count;

    if (in_to_exists_where && const_tables != table_count)
    {
      /*
        Re-optimize and cost the subquery taking into account the IN-EXISTS
        conditions.
      */
      reopt_result= reoptimize(in_to_exists_where, join_tables, &save_qep);
      if (reopt_result == REOPT_ERROR)
        return TRUE;

      /* Get the cost of the modified IN-EXISTS plan. */
      inner_read_time_2= inner_join->best_read;

    }
    else
    {
      /* Reoptimization would not produce any better plan. */
      inner_read_time_2= inner_read_time_1;
    }

    /*
      C. Compute execution costs.
    */
    /* C.1 Compute the cost of the materialization strategy. */
    //uint rowlen= get_tmp_table_rec_length(unit->first_select()->item_list);
    uint rowlen= get_tmp_table_rec_length(ref_ptrs, 
                                          select_lex->item_list.elements);
    /* The cost of writing one row into the temporary table. */
    double write_cost= get_tmp_table_write_cost(thd, inner_record_count_1,
                                                rowlen);
    /* The cost of a lookup into the unique index of the materialized table. */
    double lookup_cost= get_tmp_table_lookup_cost(thd, inner_record_count_1,
                                                  rowlen);
    /*
      The cost of executing the subquery and storing its result in an indexed
      temporary table.
    */
    double materialization_cost= COST_ADD(inner_read_time_1,
                                          COST_MULT(write_cost,
                                                    inner_record_count_1));

    materialize_strategy_cost= COST_ADD(materialization_cost,
                                        COST_MULT(outer_lookup_keys,
                                                  lookup_cost));

    /* C.2 Compute the cost of the IN=>EXISTS strategy. */
    in_exists_strategy_cost= COST_MULT(outer_lookup_keys, inner_read_time_2);

    /* C.3 Compare the costs and choose the cheaper strategy. */
    if (materialize_strategy_cost >= in_exists_strategy_cost)
      in_subs->set_strategy(SUBS_IN_TO_EXISTS);
    else
      in_subs->set_strategy(SUBS_MATERIALIZATION);

    DBUG_PRINT("info",
               ("mat_strategy_cost: %.2f, mat_cost: %.2f, write_cost: %.2f, lookup_cost: %.2f",
                materialize_strategy_cost, materialization_cost, write_cost, lookup_cost));
    DBUG_PRINT("info",
               ("inx_strategy_cost: %.2f, inner_read_time_2: %.2f",
                in_exists_strategy_cost, inner_read_time_2));
    DBUG_PRINT("info",("outer_lookup_keys: %.2f", outer_lookup_keys));
  }

  /*
    If (1) materialization is a possible strategy based on semantic analysis
    during the prepare phase, then if
      (2) it is more expensive than the IN->EXISTS transformation, and
      (3) it is not possible to create usable indexes for the materialization
          strategy,
      fall back to IN->EXISTS.
    otherwise
      use materialization.
  */
  if (in_subs->test_strategy(SUBS_MATERIALIZATION) &&
      in_subs->setup_mat_engine())
  {
    /*
      If materialization was the cheaper or the only user-selected strategy,
      but it is not possible to execute it due to limitations in the
      implementation, fall back to IN-TO-EXISTS.
    */
    in_subs->set_strategy(SUBS_IN_TO_EXISTS);
  }

  if (in_subs->test_strategy(SUBS_MATERIALIZATION))
  {
    /* Restore the original query plan used for materialization. */
    if (reopt_result == REOPT_NEW_PLAN)
      restore_query_plan(&save_qep);

    in_subs->unit->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;
    select_lex->uncacheable&= ~UNCACHEABLE_DEPENDENT_INJECTED;

    /*
      Reset the "LIMIT 1" set in Item_exists_subselect::fix_length_and_dec.
      TODO:
      Currently we set the subquery LIMIT to infinity, and this is correct
      because we forbid at parse time LIMIT inside IN subqueries (see
      Item_in_subselect::test_limit). However, once we allow this, here
      we should set the correct limit if given in the query.
    */
    in_subs->unit->global_parameters()->limit_params.select_limit= NULL;
    in_subs->unit->set_limit(unit->global_parameters());
    /*
      Set the limit of this JOIN object as well, because normally its being
      set in the beginning of JOIN::optimize, which was already done.
    */
    select_limit= in_subs->unit->lim.get_select_limit();
  }
  else if (in_subs->test_strategy(SUBS_IN_TO_EXISTS))
  {
    if (reopt_result == REOPT_NONE && in_to_exists_where &&
        const_tables != table_count)
    {
      /*
        The subquery was not reoptimized with the newly injected IN-EXISTS
        conditions either because the user allowed only the IN-EXISTS strategy,
        or because materialization was not possible based on semantic analysis.
      */
      reopt_result= reoptimize(in_to_exists_where, join_tables, NULL);
      if (reopt_result == REOPT_ERROR)
        return TRUE;
    }

    if (in_subs->inject_in_to_exists_cond(this))
      return TRUE;
    /*
      If the injected predicate is correlated the IN->EXISTS transformation
      make the subquery dependent.
    */
    if ((in_to_exists_where &&
         in_to_exists_where->used_tables() & OUTER_REF_TABLE_BIT) ||
        (in_to_exists_having &&
         in_to_exists_having->used_tables() & OUTER_REF_TABLE_BIT))
    {
      in_subs->unit->uncacheable|= UNCACHEABLE_DEPENDENT_INJECTED;
      select_lex->uncacheable|= UNCACHEABLE_DEPENDENT_INJECTED;
    }
    select_limit= 1;
  }
  else
    DBUG_ASSERT(FALSE);

  return FALSE;
}


/**
  Choose a query plan for a table-less subquery.

  @notes

  @retval FALSE     success.
  @retval TRUE      error occurred.
*/

bool JOIN::choose_tableless_subquery_plan()
{
  DBUG_ASSERT(!tables_list || !table_count);
  if (unit->item)
  {
    DBUG_ASSERT(unit->item->type() == Item::SUBSELECT_ITEM);
    Item_subselect *subs_predicate= unit->item;

    /*
      If the optimizer determined that his query has an empty result,
      in most cases the subquery predicate is a known constant value -
      either of TRUE, FALSE or NULL. The implementation of
      Item_subselect::no_rows_in_result() determines which one.
    */
    if (zero_result_cause)
    {
      if (!implicit_grouping)
      {
        /*
          Both group by queries and non-group by queries without aggregate
          functions produce empty subquery result. There is no need to further
          rewrite the subquery because it will not be executed at all.
        */
        exec_const_cond= 0;
        return FALSE;
      }

      /* @todo
         A further optimization is possible when a non-group query with
         MIN/MAX/COUNT is optimized by opt_sum_query. Then, if there are
         only MIN/MAX functions over an empty result set, the subquery
         result is a NULL value/row, thus the value of subs_predicate is
         NULL.
      */
    }
    
    /*
      For IN subqueries, use IN->EXISTS transfomation, unless the subquery 
      has been converted to a JTBM semi-join. In that case, just leave
      everything as-is, setup_jtbm_semi_joins() has special handling for cases
      like this.
    */
    Item_in_subselect *in_subs;
    in_subs= subs_predicate->get_IN_subquery();
    if (in_subs &&
        !(subs_predicate->substype() == Item_subselect::IN_SUBS &&
          in_subs->is_jtbm_merged))
    {
      in_subs->set_strategy(SUBS_IN_TO_EXISTS);
      if (in_subs->create_in_to_exists_cond(this) ||
          in_subs->inject_in_to_exists_cond(this))
        return TRUE;
      tmp_having= having;
    }
  }
  exec_const_cond= zero_result_cause ? 0 : conds;
  return FALSE;
}


bool Item::pushable_equality_checker_for_subquery(uchar *arg)
{
  return
  get_corresponding_field_pair(this,
                               ((Item *)arg)->get_IN_subquery()->
                                 corresponding_fields);
}


/*
  Checks if 'item' or some item equal to it is equal to the field from
  some Field_pair of 'pair_list' and returns matching Field_pair or
  NULL if the matching Field_pair wasn't found.
*/

Field_pair *find_matching_field_pair(Item *item, List<Field_pair> pair_list)
{
  Field_pair *field_pair= get_corresponding_field_pair(item, pair_list);
  if (field_pair)
    return field_pair;

  Item_equal *item_equal= item->get_item_equal();
  if (item_equal)
  {
    Item_equal_fields_iterator it(*item_equal);
    Item *equal_item;
    while ((equal_item= it++))
    {
      if (equal_item->const_item())
        continue;
      field_pair= get_corresponding_field_pair(equal_item, pair_list);
      if (field_pair)
        return field_pair;
    }
  }
  return NULL;
}


bool Item_field::excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred)
{
  if (find_matching_field_pair(((Item *) this), subq_pred->corresponding_fields))
    return true;
  return false;
}


bool Item_direct_view_ref::excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred)
{
  if (item_equal)
  {
    DBUG_ASSERT(real_item()->type() == Item::FIELD_ITEM);
    if (get_corresponding_field_pair(((Item *)this), subq_pred->corresponding_fields))
      return true;
  }
  return (*ref)->excl_dep_on_in_subq_left_part(subq_pred);
}


bool Item_equal::excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred)
{
  Item *left_item = get_const();
  Item_equal_fields_iterator it(*this);
  Item *item;
  if (!left_item)
  {
    while ((item=it++))
    {
      if (item->excl_dep_on_in_subq_left_part(subq_pred))
      {
        left_item= item;
        break;
      }
    }
  }
  if (!left_item)
    return false;
  while ((item=it++))
  {
    if (item->excl_dep_on_in_subq_left_part(subq_pred))
      return true;
  }
  return false;
}


/**
  @brief
    Get corresponding item from the select of the right part of IN subquery

  @param thd        the thread handle
  @param item       the item from the left part of subq_pred for which
                    corresponding item should be found
  @param subq_pred  the IN subquery predicate

  @details
    This method looks through the fields of the select of the right part of
    the IN subquery predicate subq_pred trying to find the corresponding
    item 'new_item' for item. If item has equal items it looks through
    the fields of the select of the right part of subq_pred for each equal
    item trying to find the corresponding item.
    The method assumes that the given item is either a field item or
    a reference to a field item.

  @retval <item*>  reference to the corresponding item
  @retval NULL     if item was not found
*/

static
Item *get_corresponding_item(THD *thd, Item *item,
                             Item_in_subselect *subq_pred)
{
  DBUG_ASSERT(item->type() == Item::FIELD_ITEM ||
              (item->type() == Item::REF_ITEM &&
              ((Item_ref *) item)->ref_type() == Item_ref::VIEW_REF));

  Field_pair *field_pair;
  Item_equal *item_equal= item->get_item_equal();

  if (item_equal)
  {
    Item_equal_fields_iterator it(*item_equal);
    Item *equal_item;
    while ((equal_item= it++))
    {
      field_pair=
        get_corresponding_field_pair(equal_item, subq_pred->corresponding_fields);
      if (field_pair)
        return field_pair->corresponding_item;
    }
  }
  else
  {
    field_pair=
        get_corresponding_field_pair(item, subq_pred->corresponding_fields);
    if (field_pair)
        return field_pair->corresponding_item;
  }
  return NULL;
}


Item *Item_field::in_subq_field_transformer_for_where(THD *thd, uchar *arg)
{
  Item_in_subselect *subq_pred= ((Item *)arg)->get_IN_subquery();
  Item *producing_item= get_corresponding_item(thd, this, subq_pred);
  if (producing_item)
    return producing_item->build_clone(thd);
  return this;
}


Item *Item_direct_view_ref::in_subq_field_transformer_for_where(THD *thd,
                                                                uchar *arg)
{
  if (item_equal)
  {
    Item_in_subselect *subq_pred= ((Item *)arg)->get_IN_subquery();
    Item *producing_item= get_corresponding_item(thd, this, subq_pred);
    DBUG_ASSERT (producing_item != NULL);
    return producing_item->build_clone(thd);
  }
  return this;
}


/**
  @brief
    Transforms item so it can be pushed into the IN subquery HAVING clause

  @param thd        the thread handle
  @param in_item    the item for which pushable item should be created
  @param subq_pred  the IN subquery predicate

  @details
    This method finds for in_item that is a field from the left part of the
    IN subquery predicate subq_pred its corresponding item from the right part
    of subq_pred.
    If corresponding item is found, a shell for this item is created.
    This shell can be pushed into the HAVING part of subq_pred select.

  @retval <item*>  reference to the created corresponding item shell for in_item
  @retval NULL     if mistake occurs
*/

static Item*
get_corresponding_item_for_in_subq_having(THD *thd, Item *in_item,
                                          Item_in_subselect *subq_pred)
{
  Item *new_item= get_corresponding_item(thd, in_item, subq_pred);

  if (new_item)
  {
    Item_ref *ref=
      new (thd->mem_root) Item_ref(thd,
                                   &subq_pred->unit->first_select()->context,
                                   new_item->name);
    if (!ref)
      DBUG_ASSERT(0);
    return ref;
  }
  return new_item;
}


Item *Item_field::in_subq_field_transformer_for_having(THD *thd, uchar *arg)
{
  DBUG_ASSERT(((Item *)arg)->get_IN_subquery());
  return get_corresponding_item_for_in_subq_having(thd, this,
                                                   (Item_in_subselect *)arg);
}


Item *Item_direct_view_ref::in_subq_field_transformer_for_having(THD *thd,
                                                                 uchar *arg)
{
  if (!item_equal)
    return this;
  else
  {
    DBUG_ASSERT(((Item *)arg)->get_IN_subquery());
    Item *new_item= get_corresponding_item_for_in_subq_having(thd, this,
                                                    (Item_in_subselect *)arg);
    if (!new_item)
      return this;
    return new_item;
  }
}


/**
  @brief
    Find fields that are used in the GROUP BY of the select

  @param thd            the thread handle
  @param sel            the select of the IN subquery predicate
  @param fields         fields of the left part of the IN subquery predicate
  @param grouping_list  GROUP BY clause

  @details
    This method traverses fields which are used in the GROUP BY of
    sel and saves them with their corresponding items from fields.
*/

bool grouping_fields_in_the_in_subq_left_part(THD *thd,
                                              st_select_lex *sel,
                                              List<Field_pair> *fields,
                                              ORDER *grouping_list)
{
  DBUG_ENTER("grouping_fields_in_the_in_subq_left_part");
  sel->grouping_tmp_fields.empty();
  List_iterator<Field_pair> it(*fields);
  Field_pair *item;
  while ((item= it++))
  {
    for (ORDER *ord= grouping_list; ord; ord= ord->next)
    {
      if ((*ord->item)->eq(item->corresponding_item, 0))
      {
        if (sel->grouping_tmp_fields.push_back(item, thd->mem_root))
          DBUG_RETURN(TRUE);
      }
    }
  }
  DBUG_RETURN(FALSE);
}


/**
  @brief
    Extract condition that can be pushed into select of this IN subquery

  @param thd   the thread handle
  @param cond  current condition

  @details
    This function builds the most restrictive condition depending only on
    the list of fields of the left part of this IN subquery predicate
    (directly or indirectly through equality) that can be extracted from the
    given condition cond and pushes it into this IN subquery.

    Example of the transformation:

    SELECT * FROM t1
    WHERE a>3 AND b>10 AND
          (a,b) IN (SELECT x,MAX(y) FROM t2 GROUP BY x);

    =>

    SELECT * FROM t1
    WHERE a>3 AND b>10 AND
          (a,b) IN (SELECT x,max(y)
                    FROM t2
                    WHERE x>3
                    GROUP BY x
                    HAVING MAX(y)>10);


    In details:
    1. Check what pushable formula can be extracted from cond
    2. Build a clone PC of the formula that can be extracted
       (the clone is built only if the extracted formula is a AND subformula
        of cond or conjunction of such subformulas)
    3. If there is no HAVING clause prepare PC to be conjuncted with
       WHERE clause of this subquery. Otherwise do 4-7.
    4. Check what formula PC_where can be extracted from PC to be pushed
       into the WHERE clause of the subquery
    5. Build PC_where and if PC_where is a conjunct(s) of PC remove it from PC
       getting PC_having
    6. Prepare PC_where to be conjuncted with the WHERE clause of
       the IN subquery
    7. Prepare PC_having to be conjuncted with the HAVING clause of
       the IN subquery

  @note
    This method is similar to pushdown_cond_for_derived()

  @retval TRUE   if an error occurs
  @retval FALSE  otherwise
*/

bool Item_in_subselect::pushdown_cond_for_in_subquery(THD *thd, Item *cond)
{
  DBUG_ENTER("Item_in_subselect::pushdown_cond_for_in_subquery");
  Item *remaining_cond= NULL;

  if (!cond)
    DBUG_RETURN(FALSE);

  st_select_lex *sel = unit->first_select();

  if (is_jtbm_const_tab)
    DBUG_RETURN(FALSE);

  if (!sel->cond_pushdown_is_allowed())
    DBUG_RETURN(FALSE);

  /*
    Create a list of Field_pair items for this IN subquery.
    It consists of the pairs of fields from the left part of this IN subquery
    predicate 'left_part' and the respective fields from the select of the
    right part of the IN subquery 'sel' (the field from left_part with the
    corresponding field from the sel projection list).
    Attach this list to the IN subquery.
  */
  corresponding_fields.empty();
  List_iterator_fast<Item> it(sel->join->fields_list);
  Item *item;
  for (uint i= 0; i < left_expr->cols(); i++)
  {
    item= it++;
    Item *elem= left_expr->element_index(i);

    if (elem->real_item()->type() != Item::FIELD_ITEM)
      continue;

    if (corresponding_fields.push_back(
          new Field_pair(((Item_field *)(elem->real_item()))->field,
                                         item)))
      DBUG_RETURN(TRUE);
  }

  /* 1. Check what pushable formula can be extracted from cond */
  Item *extracted_cond;
  cond->check_pushable_cond(&Item::pushable_cond_checker_for_subquery,
                            (uchar *)this);
  /* 2. Build a clone PC of the formula that can be extracted */
  extracted_cond=
    cond->build_pushable_cond(thd,
                              &Item::pushable_equality_checker_for_subquery,
                              (uchar *)this);
  /* Nothing to push */
  if (!extracted_cond)
  {
    DBUG_RETURN(FALSE);
  }

  /* Collect fields that are used in the GROUP BY of sel */
  st_select_lex *save_curr_select= thd->lex->current_select;
  if (sel->have_window_funcs())
  {
    if (sel->group_list.first || sel->join->implicit_grouping)
      goto exit;
    ORDER *common_partition_fields=
       sel->find_common_window_func_partition_fields(thd);
    if (!common_partition_fields)
      goto exit;

    if (grouping_fields_in_the_in_subq_left_part(thd, sel, &corresponding_fields,
                                                 common_partition_fields))
      DBUG_RETURN(TRUE);
  }
  else if (grouping_fields_in_the_in_subq_left_part(thd, sel,
                                                    &corresponding_fields,
                                                    sel->group_list.first))
    DBUG_RETURN(TRUE);

  /* Do 4-6 */
  sel->pushdown_cond_into_where_clause(thd, extracted_cond,
                                    &remaining_cond,
                                    &Item::in_subq_field_transformer_for_where,
                                    (uchar *) this);
  if (!remaining_cond)
    goto exit;
  /*
    7. Prepare PC_having to be conjuncted with the HAVING clause of
       the IN subquery
  */
  remaining_cond=
    remaining_cond->transform(thd,
                              &Item::in_subq_field_transformer_for_having,
                              (uchar *)this);
  if (!remaining_cond ||
      remaining_cond->walk(&Item::cleanup_excluding_const_fields_processor,
                           0, 0))
    goto exit;

  mark_or_conds_to_avoid_pushdown(remaining_cond);

  sel->cond_pushed_into_having= remaining_cond;

exit:
  thd->lex->current_select= save_curr_select;
  DBUG_RETURN(FALSE);
}

/*
  @brief
    Check if a table is a SJM Scan table

  @retval
    TRUE     SJM scan table
    FALSE    Otherwise
*/
bool TABLE_LIST::is_sjm_scan_table()
{
  return is_active_sjm() && sj_mat_info->is_sj_scan;
}
