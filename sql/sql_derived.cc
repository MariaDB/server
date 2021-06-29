/*
   Copyright (c) 2002, 2011, Oracle and/or its affiliates.
   Copyright (c) 2010, 2015, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/*
  Derived tables
  These were introduced by Sinisa <sinisa@mysql.com>
*/


#include <my_global.h>                         /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "unireg.h"
#include "sql_derived.h"
#include "sql_select.h"
#include "sql_base.h"
#include "sql_view.h"                         // check_duplicate_names
#include "sql_acl.h"                          // SELECT_ACL
#include "sql_class.h"
#include "sql_cte.h"

typedef bool (*dt_processor)(THD *thd, LEX *lex, TABLE_LIST *derived);

bool mysql_derived_init(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_prepare(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_optimize(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_merge(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_create(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_fill(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_reinit(THD *thd, LEX *lex, TABLE_LIST *derived);
bool mysql_derived_merge_for_insert(THD *thd, LEX *lex, TABLE_LIST *derived);


dt_processor processors[]=
{
  &mysql_derived_init,
  &mysql_derived_prepare,
  &mysql_derived_optimize,
  &mysql_derived_merge,
  &mysql_derived_merge_for_insert,
  &mysql_derived_create,
  &mysql_derived_fill,
  &mysql_derived_reinit,
};

/*
  Run specified phases on all derived tables/views in given LEX.

  @param lex              LEX for this thread
  @param phases           phases to run derived tables/views through

  @return FALSE  OK
  @return TRUE   Error
*/
bool
mysql_handle_derived(LEX *lex, uint phases)
{
  bool res= FALSE;
  DBUG_ENTER("mysql_handle_derived");
  DBUG_PRINT("enter", ("phases: 0x%x", phases));
  if (!lex->derived_tables)
    DBUG_RETURN(FALSE);

  lex->thd->derived_tables_processing= TRUE;

  for (uint phase= 0; phase < DT_PHASES && !res; phase++)
  {
    uint phase_flag= DT_INIT << phase;
    if (phase_flag > phases)
      break;
    if (!(phases & phase_flag))
      continue;

    for (SELECT_LEX *sl= lex->all_selects_list;
	 sl && !res;
	 sl= sl->next_select_in_list())
    {
      TABLE_LIST *cursor= sl->get_table_list();
      sl->changed_elements|= TOUCHED_SEL_DERIVED;
      /*
        DT_MERGE_FOR_INSERT is not needed for views/derived tables inside
        subqueries. Views and derived tables of subqueries should be
        processed normally.
      */
      if (phases == DT_MERGE_FOR_INSERT &&
          cursor && cursor->top_table()->select_lex != &lex->select_lex)
        continue;
      for (;
	   cursor && !res;
	   cursor= cursor->next_local)
      {
        if (!cursor->is_view_or_derived() && phases == DT_MERGE_FOR_INSERT)
          continue;
        uint8 allowed_phases= (cursor->is_merged_derived() ? DT_PHASES_MERGE :
                               DT_PHASES_MATERIALIZE | DT_MERGE_FOR_INSERT);
        /*
          Skip derived tables to which the phase isn't applicable.
          TODO: mark derived at the parse time, later set it's type
          (merged or materialized)
        */
        if ((phase_flag != DT_PREPARE && !(allowed_phases & phase_flag)) ||
            (cursor->merged_for_insert && phase_flag != DT_REINIT &&
             phase_flag != DT_PREPARE))
          continue;
	res= (*processors[phase])(lex->thd, lex, cursor);
      }
      if (lex->describe)
      {
	/*
	  Force join->join_tmp creation, because we will use this JOIN
	  twice for EXPLAIN and we have to have unchanged join for EXPLAINing
	*/
	sl->uncacheable|= UNCACHEABLE_EXPLAIN;
	sl->master_unit()->uncacheable|= UNCACHEABLE_EXPLAIN;
      }
    }
  }
  lex->thd->derived_tables_processing= FALSE;
  DBUG_RETURN(res);
}

/*
  Run through phases for the given derived table/view.

  @param lex             LEX for this thread
  @param derived         the derived table to handle
  @param phase_map       phases to process tables/views through

  @details

  This function process the derived table (view) 'derived' to performs all
  actions that are to be done on the table at the phases specified by
  phase_map. The processing is carried out starting from the actions
  performed at the earlier phases (those having smaller ordinal numbers).

  @note
  This function runs specified phases of the derived tables handling on the
  given derived table/view. This function is used in the chain of calls:
    SELECT_LEX::handle_derived ->
      TABLE_LIST::handle_derived ->
        mysql_handle_single_derived
  This chain of calls implements the bottom-up handling of the derived tables:
  i.e. most inner derived tables/views are handled first. This order is
  required for the all phases except the merge and the create steps.
  For the sake of code simplicity this order is kept for all phases.

  @return FALSE ok
  @return TRUE  error
*/

bool
mysql_handle_single_derived(LEX *lex, TABLE_LIST *derived, uint phases)
{
  bool res= FALSE;
  uint8 allowed_phases= (derived->is_merged_derived() ? DT_PHASES_MERGE :
                         DT_PHASES_MATERIALIZE);
  DBUG_ENTER("mysql_handle_single_derived");
  DBUG_PRINT("enter", ("phases: 0x%x  allowed: 0x%x  alias: '%s'",
                       phases, allowed_phases,
                       (derived->alias ? derived->alias : "<NULL>")));
  if (!lex->derived_tables)
    DBUG_RETURN(FALSE);

  lex->thd->derived_tables_processing= TRUE;

  for (uint phase= 0; phase < DT_PHASES; phase++)
  {
    uint phase_flag= DT_INIT << phase;
    if (phase_flag > phases)
      break;
    if (!(phases & phase_flag))
      continue;
    /* Skip derived tables to which the phase isn't applicable.  */
    if (phase_flag != DT_PREPARE &&
        !(allowed_phases & phase_flag))
      continue;

    if ((res= (*processors[phase])(lex->thd, lex, derived)))
      break;
  }
  lex->thd->derived_tables_processing= FALSE;
  DBUG_RETURN(res);
}


/**
  Merge a derived table/view into the embedding select

  @param thd     thread handle
  @param lex     LEX of the embedding query.
  @param derived reference to the derived table.

  @details
  This function merges the given derived table / view into the parent select
  construction. Any derived table/reference to view occurred in the FROM
  clause of the embedding select is represented by a TABLE_LIST structure a
  pointer to which is passed to the function as in the parameter 'derived'.
  This structure contains  the number/map, alias, a link to SELECT_LEX of the
  derived table and other info. If the 'derived' table is used in a nested join
  then additionally the structure contains a reference to the ON expression
  for this join.

  The merge process results in elimination of the derived table (or the
  reference to a view) such that:
    - the FROM list of the derived table/view is wrapped into a nested join
      after which the nest is added to the FROM list of the embedding select
    - the WHERE condition of the derived table (view) is ANDed with the ON
      condition attached to the table.

  @note
  Tables are merged into the leaf_tables list, original derived table is removed
  from this list also. SELECT_LEX::table_list list is left untouched.
  Where expression is merged with derived table's on_expr and can be found after
  the merge through the SELECT_LEX::table_list.

  Examples of the derived table/view merge:

  Schema:
  Tables: t1(f1), t2(f2), t3(f3)
  View v1: SELECT f1 FROM t1 WHERE f1 < 1

  Example with a view:
    Before merge:

    The query (Q1): SELECT f1,f2 FROM t2 LEFT JOIN v1 ON f1 = f2

       (LEX of the main query)
                 |
           (select_lex)
                 |
         (FROM table list)
                 |
            (join list)= t2, v1
                             / \
                            /  (on_expr)= (f1 = f2)
                            |
                    (LEX of the v1 view)
                            |
                       (select_lex)= SELECT f1 FROM t1 WHERE f1 < 1


    After merge:

    The rewritten query Q1 (Q1'):
      SELECT f1,f2 FROM t2 LEFT JOIN (t1) ON ((f1 = f2) and (f1 < 1))

        (LEX of the main query)
                   |
             (select_lex)
                   |
           (FROM table list)
                   |
               (join list)= t2, (t1)
                                    \
                                   (on_expr)= (f1 = f2) and (f1 < 1)

    In this example table numbers are assigned as follows:
      (outer select): t2 - 1, v1 - 2
      (inner select): t1 - 1
    After the merge table numbers will be:
      (outer select): t2 - 1, t1 - 2

  Example with a derived table:
    The query Q2:
      SELECT f1,f2
       FROM (SELECT f1 FROM t1, t3 WHERE f1=f3 and f1 < 1) tt, t2
       WHERE f1 = f2

    Before merge:
              (LEX of the main query)
                        |
                  (select_lex)
                  /           \
       (FROM table list)   (WHERE clause)= (f1 = f2)
                  |
           (join list)= tt, t2
                       / \
                      /  (on_expr)= (empty)
                     /
           (select_lex)= SELECT f1 FROM t1, t3 WHERE f1 = f3 and f1 < 1

    After merge:

    The rewritten query Q2 (Q2'):
      SELECT f1,f2
       FROM (t1, t3) JOIN t2 ON (f1 = f3 and f1 < 1)
       WHERE f1 = f2

              (LEX of the main query)
                        |
                  (select_lex)
                  /           \
       (FROM table list)   (WHERE clause)= (f1 = f2)
                 |
          (join list)= t2, (t1, t3)
                                   \
                                 (on_expr)= (f1 = f3 and f1 < 1)

  In this example table numbers are assigned as follows:
    (outer select): tt - 1, t2 - 2
    (inner select): t1 - 1, t3 - 2
  After the merge table numbers will be:
    (outer select): t1 - 1, t2 - 2, t3 - 3

  @return FALSE if derived table/view were successfully merged.
  @return TRUE if an error occur.
*/

bool mysql_derived_merge(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  bool res= FALSE;
  SELECT_LEX *dt_select= derived->get_single_select();
  table_map map;
  uint tablenr;
  SELECT_LEX *parent_lex= derived->select_lex;
  Query_arena *arena, backup;
  DBUG_ENTER("mysql_derived_merge");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));

  if (derived->merged)
  {

    DBUG_PRINT("info", ("Irreversibly merged: exit"));
    DBUG_RETURN(FALSE);
  }

  if (dt_select->uncacheable & UNCACHEABLE_RAND)
  {
    /* There is random function => fall back to materialization. */
    derived->change_refs_to_fields();
    derived->set_materialized_derived();
    DBUG_RETURN(FALSE);
  }

  arena= thd->activate_stmt_arena_if_needed(&backup);  // For easier test

  if (!derived->merged_for_insert || 
      (derived->is_multitable() && 
       (thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
        thd->lex->sql_command == SQLCOM_DELETE_MULTI)))
  {
    /*
      Check whether there is enough free bits in table map to merge subquery.
      If not - materialize it. This check isn't cached so when there is a big
      and small subqueries, and the bigger one can't be merged it wouldn't
      block the smaller one.
    */
    if (parent_lex->get_free_table_map(&map, &tablenr))
    {
      /* There is no enough table bits, fall back to materialization. */
      goto unconditional_materialization;
    }

    if (dt_select->leaf_tables.elements + tablenr > MAX_TABLES)
    {
      /* There is no enough table bits, fall back to materialization. */
      goto unconditional_materialization;
    }

    if (dt_select->options & OPTION_SCHEMA_TABLE)
      parent_lex->options |= OPTION_SCHEMA_TABLE;

    if (!derived->get_unit()->prepared)
    {
      dt_select->leaf_tables.empty();
      make_leaves_list(thd, dt_select->leaf_tables, derived, TRUE, 0);
    } 

    derived->nested_join= (NESTED_JOIN*) thd->calloc(sizeof(NESTED_JOIN));
    if (!derived->nested_join)
    {
      res= TRUE;
      goto exit_merge;
    }

    /* Merge derived table's subquery in the parent select. */
    if (parent_lex->merge_subquery(thd, derived, dt_select, tablenr, map))
    {
      res= TRUE;
      goto exit_merge;
    }

    /*
      exclude select lex so it doesn't show up in explain.
      do this only for derived table as for views this is already done.

      From sql_view.cc
        Add subqueries units to SELECT into which we merging current view.
        unit(->next)* chain starts with subqueries that are used by this
        view and continues with subqueries that are used by other views.
        We must not add any subquery twice (otherwise we'll form a loop),
        to do this we remember in end_unit the first subquery that has
        been already added.
    */
    derived->get_unit()->exclude_level();
    if (parent_lex->join) 
      parent_lex->join->table_count+= dt_select->join->table_count - 1;
  }
  derived->merged= TRUE;
  if (derived->get_unit()->prepared)
  {
    Item *expr= derived->on_expr;
    expr= and_conds(thd, expr, dt_select->join ? dt_select->join->conds : 0);
    if (expr)
      expr->top_level_item();

    if (expr && (derived->prep_on_expr || expr != derived->on_expr))
    {
      derived->on_expr= expr;
      derived->prep_on_expr= expr->copy_andor_structure(thd);
    }
    thd->where= "on clause";
    if (derived->on_expr &&
        ((!derived->on_expr->fixed &&
          derived->on_expr->fix_fields(thd, &derived->on_expr)) ||
          derived->on_expr->check_cols(1)))
    {
      res= TRUE; /* purecov: inspected */
      goto exit_merge;
    }
    // Update used tables cache according to new table map
    if (derived->on_expr)
    {
      derived->on_expr->fix_after_pullout(parent_lex, &derived->on_expr,
                                          TRUE);
      fix_list_after_tbl_changes(parent_lex, &derived->nested_join->join_list);
    }
  }

exit_merge:
  if (arena)
    thd->restore_active_arena(arena, &backup);
  DBUG_RETURN(res);

unconditional_materialization:
  derived->change_refs_to_fields();
  derived->set_materialized_derived();
  if (!derived->table || !derived->table->is_created())
    res= mysql_derived_create(thd, lex, derived);
  goto exit_merge;
}


/**
  Merge a view for the embedding INSERT/UPDATE/DELETE

  @param thd     thread handle
  @param lex     LEX of the embedding query.
  @param derived reference to the derived table.

  @details
  This function substitutes the derived table for the first table from
  the query of the derived table thus making it a correct target table for the
  INSERT/UPDATE/DELETE statements. As this operation is correct only for
  single table views only, for multi table views this function does nothing.
  The derived parameter isn't checked to be a view as derived tables aren't
  allowed for INSERT/UPDATE/DELETE statements.

  @return FALSE if derived table/view were successfully merged.
  @return TRUE if an error occur.
*/

bool mysql_derived_merge_for_insert(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  DBUG_ENTER("mysql_derived_merge_for_insert");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));
  DBUG_PRINT("info", ("merged_for_insert: %d  is_materialized_derived: %d  "
                      "is_multitable: %d  single_table_updatable: %d  "
                      "merge_underlying_list: %d",
                      derived->merged_for_insert,
                      derived->is_materialized_derived(),
                      derived->is_multitable(),
                      derived->single_table_updatable(),
                      derived->merge_underlying_list != 0));
  if (derived->merged_for_insert)
    DBUG_RETURN(FALSE);
  if (derived->init_derived(thd, FALSE))
    DBUG_RETURN(TRUE);
  if (derived->is_materialized_derived())
    DBUG_RETURN(mysql_derived_prepare(thd, lex, derived));
  if ((thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
       thd->lex->sql_command == SQLCOM_DELETE_MULTI))
    DBUG_RETURN(FALSE);
  if (!derived->is_multitable())
  {
    if (!derived->single_table_updatable())
      DBUG_RETURN(derived->create_field_translation(thd));
    if (derived->merge_underlying_list)
    {
      derived->table= derived->merge_underlying_list->table;
      derived->schema_table= derived->merge_underlying_list->schema_table;
      derived->merged_for_insert= TRUE;
      DBUG_ASSERT(derived->table);
    }
  }
  DBUG_RETURN(FALSE);
}


/*
  Initialize a derived table/view

  @param thd	     Thread handle
  @param lex         LEX of the embedding query.
  @param derived     reference to the derived table.

  @detail
  Fill info about derived table/view without preparing an
  underlying select. Such as: create a field translation for views, mark it as
  a multitable if it is and so on.

  @return
    false  OK
    true   Error
*/


bool mysql_derived_init(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  SELECT_LEX_UNIT *unit= derived->get_unit();
  DBUG_ENTER("mysql_derived_init");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));

  // Skip already prepared views/DT
  if (!unit || unit->prepared)
    DBUG_RETURN(FALSE);

  bool res= derived->init_derived(thd, TRUE);

  derived->updatable= derived->updatable && derived->is_view();

  DBUG_RETURN(res);
}


/*
  @brief
    Prevent name resolution out of context of ON expressions in derived tables

  @param
    join_list  list of tables used in from list of a derived

  @details
    The function sets the Name_resolution_context::outer_context to NULL
    for all ON expressions contexts in the given join list. It does this
    recursively for all nested joins the list contains.
*/

static void nullify_outer_context_for_on_clauses(List<TABLE_LIST>& join_list)
{
  List_iterator<TABLE_LIST> li(join_list);
  while (TABLE_LIST *table= li++)
  {
    if (table->on_context)
      table->on_context->outer_context= NULL;
    if (table->nested_join)
      nullify_outer_context_for_on_clauses(table->nested_join->join_list);
  }
}


/*
  Create temporary table structure (but do not fill it)

  @param thd	     Thread handle
  @param lex         LEX of the embedding query.
  @param derived     reference to the derived table.

  @detail
  Prepare underlying select for a derived table/view. To properly resolve
  names in the embedding query the TABLE structure is created. Actual table
  is created later by the mysql_derived_create function.

  This function is called before any command containing derived table
  is executed. All types of derived tables are handled by this function:
  - Anonymous derived tables, or
  - Named derived tables (aka views).

  The table reference, contained in @c derived, is updated with the
  fields of a new temporary table.
  Derived tables are stored in @c thd->derived_tables and closed by
  close_thread_tables().

  This function is part of the procedure that starts in
  open_and_lock_tables(), a procedure that - among other things - introduces
  new table and table reference objects (to represent derived tables) that
  don't exist in the privilege database. This means that normal privilege
  checking cannot handle them. Hence this function does some extra tricks in
  order to bypass normal privilege checking, by exploiting the fact that the
  current state of privilege verification is attached as GRANT_INFO structures
  on the relevant TABLE and TABLE_REF objects.

  For table references, the current state of accrued access is stored inside
  TABLE_LIST::grant. Hence this function must update the state of fulfilled
  privileges for the new TABLE_LIST, an operation which is normally performed
  exclusively by the table and database access checking functions,
  check_access() and check_grant(), respectively. This modification is done
  for both views and anonymous derived tables: The @c SELECT privilege is set
  as fulfilled by the user. However, if a view is referenced and the table
  reference is queried against directly (see TABLE_LIST::referencing_view),
  the state of privilege checking (GRANT_INFO struct) is copied as-is to the
  temporary table.

  Only the TABLE structure is created here, actual table is created by the
  mysql_derived_create function.

  @note This function sets @c SELECT_ACL for @c TEMPTABLE views as well as
  anonymous derived tables, but this is ok since later access checking will
  distinguish between them.

  @see mysql_handle_derived(), mysql_derived_fill(), GRANT_INFO

  @return
    false  OK
    true   Error
*/


bool mysql_derived_prepare(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  SELECT_LEX_UNIT *unit= derived->get_unit();
  DBUG_ENTER("mysql_derived_prepare");
  bool res= FALSE;
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       unit));

  if (!unit)
    DBUG_RETURN(FALSE);

  SELECT_LEX *first_select= unit->first_select();

  if (derived->is_recursive_with_table() &&
      !derived->is_with_table_recursive_reference() &&
      !derived->with->rec_result && derived->with->get_sq_rec_ref())
  {
    /*
      This is a non-recursive reference to a recursive CTE whose
      specification unit has not been prepared at the regular processing of
      derived table references. This can happen  only in the case when
      the specification unit has no recursive references at the top level. 
      Force the preparation of the specification unit. Use a recursive
      table reference from a subquery for this.
    */
    DBUG_ASSERT(derived->with->get_sq_rec_ref());
    if (mysql_derived_prepare(lex->thd, lex, derived->with->get_sq_rec_ref()))
      DBUG_RETURN(TRUE);
  }

  if (unit->prepared && derived->is_recursive_with_table() &&
      !derived->table)
  {
    /* 
      Here 'derived' is either a non-recursive table reference to a recursive
      with table or a recursive table reference to a recursvive table whose
      specification has been already prepared (a secondary recursive table
      reference.
    */ 
    if (!(derived->derived_result= new (thd->mem_root) select_union(thd)))
      DBUG_RETURN(TRUE); // out of memory
    thd->create_tmp_table_for_derived= TRUE;
    res= derived->derived_result->create_result_table(
                                  thd, &unit->types, FALSE,
                                  (first_select->options |
                                   thd->variables.option_bits |
                                   TMP_TABLE_ALL_COLUMNS),
                                  derived->alias, FALSE, FALSE);
    thd->create_tmp_table_for_derived= FALSE;

    if (!res && !derived->table)
    {
      derived->derived_result->set_unit(unit);
      derived->table= derived->derived_result->table;
      if (derived->is_with_table_recursive_reference())
      {
        /* Here 'derived" is a secondary recursive table reference */
         unit->with_element->rec_result->rec_table_refs.push_back(derived);
      }
    }
    DBUG_ASSERT(derived->table || res);
    goto exit;
  }

  // Skip already prepared views/DT
  if (unit->prepared ||
      (derived->merged_for_insert && 
       !(derived->is_multitable() &&
         (thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
          thd->lex->sql_command == SQLCOM_DELETE_MULTI))))
    DBUG_RETURN(FALSE);

  /* prevent name resolving out of derived table */
  for (SELECT_LEX *sl= first_select; sl; sl= sl->next_select())
  {
    // Prevent it for the WHERE clause
    sl->context.outer_context= 0;

    // And for ON clauses, if there are any
    nullify_outer_context_for_on_clauses(*sl->join_list);

    if (!derived->is_with_table_recursive_reference() ||
        (!derived->with->with_anchor && 
         !derived->with->is_with_prepared_anchor()))
    {
      /* 
        Prepare underlying views/DT first unless 'derived' is a recursive
        table reference and either the anchors from the specification of
        'derived' has been already prepared or there no anchor in this
        specification
      */  
      if ((res= sl->handle_derived(lex, DT_PREPARE)))
        goto exit;
    }
    if (derived->outer_join && sl->first_cond_optimization)
    {
      /* Mark that table is part of OUTER JOIN and fields may be NULL */
      for (TABLE_LIST *cursor= (TABLE_LIST*) sl->table_list.first;
           cursor;
           cursor= cursor->next_local)
        cursor->outer_join|= JOIN_TYPE_OUTER;
    }
  }

  unit->derived= derived;

  /*
    Above cascade call of prepare is important for PS protocol, but after it
    is called we can check if we really need prepare for this derived
  */
  if (derived->merged)
  {
    DBUG_PRINT("info", ("Irreversibly merged: exit"));
    DBUG_RETURN(FALSE);
  }

  derived->fill_me= FALSE;

  if ((!derived->is_with_table_recursive_reference() ||
       !derived->derived_result) &&
      !(derived->derived_result= new (thd->mem_root) select_union(thd)))
    DBUG_RETURN(TRUE); // out of memory

  // st_select_lex_unit::prepare correctly work for single select
  if ((res= unit->prepare(thd, derived->derived_result, 0)))
    goto exit;
  if (derived->with &&
      (res= derived->with->rename_columns_of_derived_unit(thd, unit)))
    goto exit; 
  if ((res= check_duplicate_names(thd, unit->types, 0)))
    goto exit;

  /*
    Check whether we can merge this derived table into main select.
    Depending on the result field translation will or will not
    be created.
  */
  if (!derived->is_with_table_recursive_reference() &&
      derived->init_derived(thd, FALSE))
    goto exit;

  /*
    Temp table is created so that it hounours if UNION without ALL is to be 
    processed

    As 'distinct' parameter we always pass FALSE (0), because underlying
    query will control distinct condition by itself. Correct test of
    distinct underlying query will be is_union &&
    !unit->union_distinct->next_select() (i.e. it is union and last distinct
    SELECT is last SELECT of UNION).
  */
  thd->create_tmp_table_for_derived= TRUE;
  if (!(derived->table) &&
      derived->derived_result->create_result_table(thd, &unit->types, FALSE,
                                                   (first_select->options |
                                                   thd->variables.option_bits |
                                                   TMP_TABLE_ALL_COLUMNS),
                                                   derived->alias,
                                                   FALSE, FALSE, FALSE))
  { 
    thd->create_tmp_table_for_derived= FALSE;
    goto exit;
  }
  thd->create_tmp_table_for_derived= FALSE;

  if (!derived->table)
    derived->table= derived->derived_result->table;
  DBUG_ASSERT(derived->table);
  if (derived->is_derived() && derived->is_merged_derived())
    first_select->mark_as_belong_to_derived(derived);

exit:
  /* Hide "Unknown column" or "Unknown function" error */
  if (derived->view)
  {
    if (thd->is_error() &&
        (thd->get_stmt_da()->sql_errno() == ER_BAD_FIELD_ERROR ||
        thd->get_stmt_da()->sql_errno() == ER_FUNC_INEXISTENT_NAME_COLLISION ||
        thd->get_stmt_da()->sql_errno() == ER_SP_DOES_NOT_EXIST))
    {
      thd->clear_error();
      my_error(ER_VIEW_INVALID, MYF(0), derived->db,
               derived->table_name);
    }
  }

  /*
    if it is preparation PS only or commands that need only VIEW structure
    then we do not need real data and we can skip execution (and parameters
    is not defined, too)
  */
  if (res)
  {
    if (!derived->is_with_table_recursive_reference())
    {
      if (derived->table)
        free_tmp_table(thd, derived->table);
      delete derived->derived_result;
    }
  }
  else
  {
    TABLE *table= derived->table;
    table->derived_select_number= first_select->select_number;
    table->s->tmp_table= INTERNAL_TMP_TABLE;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    if (derived->is_view())
      table->grant= derived->grant;
    else
    {
      DBUG_ASSERT(derived->is_derived());
      DBUG_ASSERT(derived->is_anonymous_derived_table());
      table->grant.privilege= SELECT_ACL;
      derived->grant.privilege= SELECT_ACL;
    }
#endif
    /* Add new temporary table to list of open derived tables */
    if (!derived->is_with_table_recursive_reference())
    {
      table->next= thd->derived_tables;
      thd->derived_tables= table;
    }

    /* If table is used by a left join, mark that any column may be null */
    if (derived->outer_join)
      table->maybe_null= 1;
  }
  DBUG_RETURN(res);
}


/**
  Runs optimize phase for a derived table/view.

  @param thd     thread handle
  @param lex     LEX of the embedding query.
  @param derived reference to the derived table.

  @details
  Runs optimize phase for given 'derived' derived table/view.
  If optimizer finds out that it's of the type "SELECT a_constant" then this
  functions also materializes it.

  @return FALSE ok.
  @return TRUE if an error occur.
*/

bool mysql_derived_optimize(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  SELECT_LEX_UNIT *unit= derived->get_unit();
  SELECT_LEX *first_select= unit->first_select();
  SELECT_LEX *save_current_select= lex->current_select;

  bool res= FALSE;
  DBUG_ENTER("mysql_derived_optimize");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));
  if (derived->merged)
  {
    DBUG_PRINT("info", ("Irreversibly merged: exit"));
    DBUG_RETURN(FALSE);
  }

  if (unit->optimized)
    DBUG_RETURN(FALSE);
  lex->current_select= first_select;

  if (unit->is_union())
  {
    // optimize union without execution
    res= unit->optimize();
  }
  else if (unit->derived)
  {
    if (!derived->is_merged_derived())
    {
      JOIN *join= first_select->join;
      unit->set_limit(unit->global_parameters());
      unit->optimized= TRUE;
      if ((res= join->optimize()))
        goto err;
      if (join->table_count == join->const_tables)
        derived->fill_me= TRUE;
    }
  }
  /*
    Materialize derived tables/views of the "SELECT a_constant" type.
    Such tables should be materialized at the optimization phase for
    correct constant evaluation.
  */
  if (!res && derived->fill_me && !derived->merged_for_insert)
  {
    if (derived->is_merged_derived())
    {
      derived->change_refs_to_fields();
      derived->set_materialized_derived();
    }
    if ((res= mysql_derived_create(thd, lex, derived)))
      goto err;
    if ((res= mysql_derived_fill(thd, lex, derived)))
      goto err;
  }
err:
  lex->current_select= save_current_select;
  DBUG_RETURN(res);
}


/**
  Actually create result table for a materialized derived table/view.

  @param thd     thread handle
  @param lex     LEX of the embedding query.
  @param derived reference to the derived table.

  @details
  This function actually creates the result table for given 'derived'
  table/view, but it doesn't fill it.
  'thd' and 'lex' parameters are not used  by this function.

  @return FALSE ok.
  @return TRUE if an error occur.
*/

bool mysql_derived_create(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  DBUG_ENTER("mysql_derived_create");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));
  TABLE *table= derived->table;
  SELECT_LEX_UNIT *unit= derived->get_unit();

  if (table->is_created())
    DBUG_RETURN(FALSE);
  select_union *result= derived->derived_result;
  if (table->s->db_type() == TMP_ENGINE_HTON)
  {
    result->tmp_table_param.keyinfo= table->s->key_info;
    if (create_internal_tmp_table(table, result->tmp_table_param.keyinfo,
                                  result->tmp_table_param.start_recinfo,
                                  &result->tmp_table_param.recinfo,
                                  (unit->first_select()->options |
                                   thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS)))
      DBUG_RETURN(TRUE);
  }
  if (open_tmp_table(table))
    DBUG_RETURN(TRUE);
  table->file->extra(HA_EXTRA_WRITE_CACHE);
  table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  DBUG_RETURN(FALSE);
}


/**
  @brief
    Fill the recursive with table 

  @param thd  The thread handle

  @details
    The method is called only for recursive with tables. 
    The method executes the recursive part of the specification
    of this with table until no more rows are added to the table
    or the number of the performed iteration reaches the allowed
    maximum. 

  @retval
    false   on success
    true    on failure 
*/

bool TABLE_LIST::fill_recursive(THD *thd)
{
  bool rc= false;
  st_select_lex_unit *unit= get_unit();
  rc= with->instantiate_tmp_tables();
  while (!rc && !with->all_are_stabilized())
  {
    if (with->level > thd->variables.max_recursive_iterations)
      break;
    with->prepare_for_next_iteration();
    rc= unit->exec_recursive();
  }
  if (!rc)
  {
    TABLE *src= with->rec_result->table;
    rc =src->insert_all_rows_into_tmp_table(thd,
                                            table,
                                            &with->rec_result->tmp_table_param,
                                            true);
  } 
  return rc;
}


/*
  Execute subquery of a materialized derived table/view and fill the result
  table.

  @param thd      Thread handle
  @param lex      LEX for this thread
  @param derived  reference to the derived table.

  @details
  Execute subquery of given 'derived' table/view and fill the result
  table. After result table is filled, if this is not the EXPLAIN statement
  and the table is not specified with a recursion the entire unit / node
  is deleted. unit is deleted if UNION is used  for derived table and node
  is deleted is it is a simple SELECT.
  'lex' is unused and 'thd' is passed as an argument to an underlying function.

  @note
  If you use this function, make sure it's not called at prepare.
  Due to evaluation of LIMIT clause it can not be used at prepared stage.

  @return FALSE  OK
  @return TRUE   Error
*/


bool mysql_derived_fill(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  Field_iterator_table field_iterator;
  SELECT_LEX_UNIT *unit= derived->get_unit();
  bool derived_is_recursive= derived->is_recursive_with_table();
  bool res= FALSE;
  DBUG_ENTER("mysql_derived_fill");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));

  if (unit->executed && !unit->uncacheable && !unit->describe &&
      !derived_is_recursive)
    DBUG_RETURN(FALSE);
  /*check that table creation passed without problems. */
  DBUG_ASSERT(derived->table && derived->table->is_created());
  select_union *derived_result= derived->derived_result;
  SELECT_LEX *save_current_select= lex->current_select;
  
  if (derived_is_recursive)
  {
    if (derived->is_with_table_recursive_reference())
    {
      /* Here only one iteration step is performed */
      res= unit->exec_recursive();
    }
    else
    {
      /* In this case all iteration are performed */
      res= derived->fill_recursive(thd);
    }
  }
  else if (unit->is_union() || unit->fake_select_lex)
  {
    // execute union without clean up
    res= unit->exec();
  }
  else
  {
    SELECT_LEX *first_select= unit->first_select();
    unit->set_limit(unit->global_parameters());
    if (unit->select_limit_cnt == HA_POS_ERROR)
      first_select->options&= ~OPTION_FOUND_ROWS;

    lex->current_select= first_select;
    res= mysql_select(thd,
                      first_select->table_list.first,
                      first_select->with_wild,
                      first_select->item_list, first_select->where,
                      (first_select->order_list.elements+
                       first_select->group_list.elements),
                      first_select->order_list.first,
                      first_select->group_list.first,
                      first_select->having, (ORDER*) NULL,
                      (first_select->options |thd->variables.option_bits |
                       SELECT_NO_UNLOCK),
                      derived_result, unit, first_select);
  }

  if (!res && !derived_is_recursive)
  {
    if (derived_result->flush())
      res= TRUE;
    unit->executed= TRUE;

    if (derived->field_translation)
    {
      /* reset translation table to materialized table */
      field_iterator.set_table(derived->table);
      for (uint i= 0;
           !field_iterator.end_of_fields();
           field_iterator.next(), i= i + 1)
      {
        Item *item;

        if (!(item= field_iterator.create_item(thd)))
        {
          res= TRUE;
          break;
        }
        thd->change_item_tree(&derived->field_translation[i].item, item);
      }
    }
  }

  if (res || (!lex->describe && !derived_is_recursive))
    unit->cleanup();
  lex->current_select= save_current_select;

  DBUG_RETURN(res);
}


/**
  Re-initialize given derived table/view for the next execution.

  @param  thd         thread handle
  @param  lex         LEX for this thread
  @param  derived     reference to the derived table.

  @details
  Re-initialize given 'derived' table/view for the next execution.
  All underlying views/derived tables are recursively reinitialized prior
  to re-initialization of given derived table.
  'thd' and 'lex' are passed as arguments to called functions.

  @return FALSE  OK
  @return TRUE   Error
*/

bool mysql_derived_reinit(THD *thd, LEX *lex, TABLE_LIST *derived)
{
  DBUG_ENTER("mysql_derived_reinit");
  DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                       (derived->alias ? derived->alias : "<NULL>"),
                       derived->get_unit()));
  st_select_lex_unit *unit= derived->get_unit();

  derived->merged_for_insert= FALSE;
  unit->unclean();
  unit->types.empty();
  /* for derived tables & PS (which can't be reset by Item_subselect) */
  unit->reinit_exec_mechanism();
  for (st_select_lex *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    sl->cond_pushed_into_where= NULL;
    sl->cond_pushed_into_having= NULL;
  }
  unit->set_thd(thd);
  DBUG_RETURN(FALSE);
}


/*
  @brief
    Given condition cond and transformer+argument, try transforming as many
    conjuncts as possible.

  @detail
    The motivation of this function is to convert the condition that's being
    pushed into a WHERE clause with derived_field_transformer_for_where or
    with derived_grouping_field_transformer_for_where.
    The transformer may fail for some sub-condition, in this case we want to
    convert the most restrictive part of the condition that can be pushed.

    This function only does it for top-level AND: conjuncts that could not be
    converted are dropped.

  @return
    Converted condition, or NULL if nothing could be converted
*/

static
Item *transform_condition_or_part(THD *thd,
                                  Item *cond,
                                  Item_transformer transformer,
                                  uchar *arg)
{
  if (cond->type() != Item::COND_ITEM ||
      ((Item_cond*) cond)->functype() != Item_func::COND_AND_FUNC)
  {
    Item *new_item= cond->transform(thd, transformer, arg);
      // Indicate that the condition is not pushable
    if (!new_item)
      cond->clear_extraction_flag();
    return new_item;
  }

  List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
  Item *item;
  while ((item=li++))
  {
    Item *new_item= item->transform(thd, transformer, arg);
    if (!new_item)
    {
      // Indicate that the condition is not pushable
      item->clear_extraction_flag();
      li.remove();
    }
    else
      li.replace(new_item);
  }

  switch (((Item_cond*) cond)->argument_list()->elements)
  {
  case 0:
    return NULL;
  case 1:
    return ((Item_cond*) cond)->argument_list()->head();
  default:
    return cond;
  }
}


/**
  @brief
  Extract the condition depended on derived table/view and pushed it there 
   
  @param thd       The thread handle
  @param cond      The condition from which to extract the pushed condition 
  @param derived   The reference to the derived table/view

  @details
   This functiom builds the most restrictive condition depending only on
   the derived table/view that can be extracted from the condition cond. 
   The built condition is pushed into the having clauses of the
   selects contained in the query specifying the derived table/view.
   The function also checks for each select whether any condition depending
   only on grouping fields can be extracted from the pushed condition.
   If so, it pushes the condition over grouping fields into the where
   clause of the select.
  
  @retval
    true    if an error is reported 
    false   otherwise
*/

bool pushdown_cond_for_derived(THD *thd, Item *cond, TABLE_LIST *derived)
{
  DBUG_ENTER("pushdown_cond_for_derived");
  if (!cond)
    DBUG_RETURN(false);

  st_select_lex_unit *unit= derived->get_unit();
  st_select_lex *first_sl= unit->first_select();
  st_select_lex *sl= first_sl;

  if (derived->prohibit_cond_pushdown)
    DBUG_RETURN(false);

  /* Do not push conditions into constant derived */
  if (unit->executed)
    DBUG_RETURN(false);

  /* Do not push conditions into recursive with tables */
  if (derived->is_recursive_with_table())
    DBUG_RETURN(false);

  /* Do not push conditions into unit with global ORDER BY ... LIMIT */
  if (unit->fake_select_lex && unit->fake_select_lex->explicit_limit)
    DBUG_RETURN(false);

  /* Check whether any select of 'unit' allows condition pushdown */
  bool some_select_allows_cond_pushdown= false;
  for (; sl; sl= sl->next_select())
  {
    if (sl->cond_pushdown_is_allowed())
    {
      some_select_allows_cond_pushdown= true;
      break;
    }
  }
  if (!some_select_allows_cond_pushdown)
    DBUG_RETURN(false);

  /*
    Build the most restrictive condition extractable from 'cond'
    that can be pushed into the derived table 'derived'.
    All subexpressions of this condition are cloned from the
    subexpressions of 'cond'.
    This condition has to be fixed yet.
  */
  Item *extracted_cond;
  derived->check_pushable_cond_for_table(cond);
  extracted_cond= derived->build_pushable_cond_for_table(thd, cond);
  if (!extracted_cond)
  {
    /* Nothing can be pushed into the derived table */
    DBUG_RETURN(false);
  }
  /* Push extracted_cond into every select of the unit specifying 'derived' */
  st_select_lex *save_curr_select= thd->lex->current_select;
  for (; sl; sl= sl->next_select())
  {
    if (!sl->cond_pushdown_is_allowed())
      continue;
    thd->lex->current_select= sl;
    /*
      For each select of the unit except the last one
      create a clone of extracted_cond
    */
    Item *extracted_cond_copy= !sl->next_select() ? extracted_cond :
                               extracted_cond->build_clone(thd, thd->mem_root);
    if (!extracted_cond_copy)
      continue;

    if (!sl->join->group_list && !sl->with_sum_func)
    {
      /* extracted_cond_copy is pushed into where of sl */
      extracted_cond_copy=
        transform_condition_or_part(thd,
                                    extracted_cond_copy,
                                    &Item::derived_field_transformer_for_where,
                                    (uchar*)sl);
      if (extracted_cond_copy)
      {
        extracted_cond_copy->walk(
          &Item::cleanup_excluding_const_fields_processor, 0, 0);
        sl->cond_pushed_into_where= extracted_cond_copy;
      }      
  
      continue;
    }

    /*
      Figure out what can be extracted from the pushed condition
      that could be pushed into the where clause of sl
    */
    Item *cond_over_grouping_fields;
    sl->collect_grouping_fields(thd);
    sl->check_cond_extraction_for_grouping_fields(extracted_cond_copy,
                                                  derived);
    cond_over_grouping_fields=
      sl->build_cond_for_grouping_fields(thd, extracted_cond_copy, true);
  
    /*
      Transform the references to the 'derived' columns from the condition
      pushed into the where clause of sl to make them usable in the new context
    */
    if (cond_over_grouping_fields)
    {
      cond_over_grouping_fields= 
        transform_condition_or_part(thd, cond_over_grouping_fields,
                                    &Item::derived_grouping_field_transformer_for_where,
                                    (uchar*) sl);
    }
     
    if (cond_over_grouping_fields)
    {
      /*
        In extracted_cond_copy remove top conjuncts that
        has been pushed into the where clause of sl
      */
      extracted_cond_copy= remove_pushed_top_conjuncts(thd, extracted_cond_copy);

      cond_over_grouping_fields->walk(
        &Item::cleanup_excluding_const_fields_processor, 0, 0);
      sl->cond_pushed_into_where= cond_over_grouping_fields;

      if (!extracted_cond_copy)
        continue;
    }

    /*
      Rename the columns of all non-first selects of a union to be compatible
      by names with the columns of the first select. It will allow to use copies
      of the same expression pushed into having clauses of different selects.
    */
    if (sl != first_sl)
    {
      DBUG_ASSERT(sl->item_list.elements == first_sl->item_list.elements);
      List_iterator_fast<Item> it(sl->item_list);
      List_iterator_fast<Item> nm_it(unit->types);
      Item * item;
      while((item= it++))
      {
        item->share_name_with(nm_it++);
      }
    }

    /*
      Transform the references to the 'derived' columns from the condition
      pushed into the having clause of sl to make them usable in the new context
    */
    extracted_cond_copy= extracted_cond_copy->transform(thd,
                         &Item::derived_field_transformer_for_having,
                         (uchar*) sl);
    if (!extracted_cond_copy)
      continue;

    extracted_cond_copy->walk(&Item::cleanup_excluding_const_fields_processor,
                              0, 0);
    sl->cond_pushed_into_having= extracted_cond_copy;
  }
  thd->lex->current_select= save_curr_select;
  DBUG_RETURN(false);
}
