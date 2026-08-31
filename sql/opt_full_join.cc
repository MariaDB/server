/*
   Copyright (c) 2026, MariaDB

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

/**
  @file

  @brief
    FULL JOIN rewrite, planning, and execution support.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "opt_subselect.h"
#include "opt_full_join.h"
#include "sql_base.h"

/* Used to build a pointer to a permanently null Item * for on_expr_ref. */
static Item * const null_ptr= NULL;


/*
  The tables of one side of a join.  A side is a nested join when the
  query parenthesised it and a single table otherwise, and this holds for
  either side of a FULL JOIN.
*/

static table_map join_side_map(TABLE_LIST *side)
{
  return side->nested_join ? side->nested_join->used_tables
                           : side->table->map;
}


/*
  Collect the entries that are the right side of a surviving FULL JOIN.

  Since a side can be a nested join, what is collected here is not always
  a base table and leaf_tables does not hold all of them.  The search
  walks the join list and descends into every nest.

  Returns true on error.
*/

bool collect_full_join_right_sides(List<TABLE_LIST> *join_list,
                                          List<TABLE_LIST> *right_sides,
                                          MEM_ROOT *mem_root)
{
  List_iterator<TABLE_LIST> li(*join_list);
  TABLE_LIST *tl;
  while ((tl= li++))
  {
    if ((tl->outer_join & (JOIN_TYPE_FULL | JOIN_TYPE_RIGHT)) ==
        (JOIN_TYPE_FULL | JOIN_TYPE_RIGHT) && tl->foj_partner &&
        right_sides->push_back(tl, mem_root))
      return true;
    if (tl->nested_join &&
        collect_full_join_right_sides(&tl->nested_join->join_list,
                                      right_sides, mem_root))
      return true;
  }
  return false;
}


/*
  The top level conjuncts of a condition.

  A condition is either a single predicate or a top level AND of
  predicates.  A single predicate goes into the caller's throwaway list
  so both shapes iterate the same way.  Returns NULL on error.
*/

static List<Item> *top_level_conjuncts(THD *thd, COND *cond,
                                       List<Item> *single)
{
  if (cond->type() == Item::COND_ITEM &&
      ((Item_cond *) cond)->functype() == Item_func::COND_AND_FUNC)
    return ((Item_cond *) cond)->argument_list();
  if (single->push_back(cond, thd->mem_root))
    return NULL;
  return single;
}


/*
  The right side that must record its match before a condition over the
  given tables may be evaluated.

  A condition that reads a surviving FULL JOIN's left side and not its
  right side cannot be evaluated while the left side row is being
  matched, since rejecting the row there also loses the record of its
  match.  It has to wait for the right side, where the match is
  recorded.  When several FULL JOINs have the tables on their left side,
  the outermost one is the one to wait for, and that is the one with the
  larger left side, so the match is recorded by every FULL JOIN before
  the condition can reject the row.

  Returns NULL when the tables name no surviving FULL JOIN's left side,
  or name a right side as well, in which case that side already defers
  the condition behind its own match.
*/

static TABLE_LIST *full_join_right_side_for(List<TABLE_LIST> *right_sides,
                                            table_map used)
{
  TABLE_LIST *best= NULL;
  table_map best_left= 0;
  List_iterator<TABLE_LIST> li(*right_sides);
  TABLE_LIST *r;

  while ((r= li++))
  {
    table_map left= join_side_map(r->foj_partner);

    if (!(used & left) || (used & join_side_map(r)))
      continue;
    if (!best || my_count_bits(left) > my_count_bits(best_left))
    {
      best= r;
      best_left= left;
    }
  }
  return best;
}


/*
  Move a surviving FULL JOIN's left side WHERE predicates out of the WHERE.

  A FULL JOIN runs as a LEFT JOIN of its left side over its right side,
  then a pass rescans the right side and emits the rows that never
  matched a left row.  This is correct only if the join pass records
  every left-right match, so the pass must enumerate every left row and
  reach the right side for each match.

  In that LEFT JOIN pass the right side is the inner table, so its WHERE
  predicates are already deferred behind the match by the found-match
  guard: a right row that matches and is then WHERE-rejected still
  records its match.  The left side is the outer table, so its WHERE
  predicates are applied directly, both to prune rows in the nested loop
  and to drive ref and range access.  Either one drops a left row before
  its match is recorded, and the matched right row then wrongly
  reappears as a right-only row.  In a FULL JOIN the left side is
  null-complemented in the right-only rows just as the right side is in
  the left-only rows, so its predicates are inner-side predicates too
  and must be deferred the same way.

  Lift the WHERE conjuncts that reference a FULL JOIN's left side but not
  its right side out of the WHERE and hold them on the right side
  partner.  Removed from the WHERE, they build no ref or range on the
  left side, so it is read in full; make_join_select reattaches them to
  the right partner under the found-match guard, so they apply to every
  output row only after the match is recorded.

  This runs before the join order is known and works on whole conjuncts,
  which is the granularity that ref and range analysis reads the WHERE
  at.  It is not the granularity the WHERE is distributed over the plan
  at, and it is not the last word on the WHERE either, since the WHERE
  goes on being rewritten afterwards.  Whether a condition may be
  evaluated on the left side is therefore decided again in
  make_join_select, on whatever condition is about to be attached to a
  tab, and that decision is the one correctness rests on.  What this
  function still owns is keeping the left side out of ref and range
  analysis, which happens before make_join_select runs.

  A multiple equality is left alone when the FULL JOIN rewrite is
  enabled.  build_equal_items splices the list nodes of
  cond_equal.current_level onto the top level AND, and the equality
  substitution later unlinks that same run of nodes, so taking a
  multiple equality out of the AND here breaks a list the substitution
  still walks.  Leaving it in place costs nothing for the answer,
  because a predicate over the left side is deferred at attachment time
  whether it was lifted or not.  What it costs is that the multiple
  equality still reaches ref and range analysis on the left side.

  Called once per execution on the statement's working copy of the
  WHERE, so the held conjuncts are reset first.  Returns the WHERE with
  the lifted conjuncts removed.
*/

COND *lift_full_join_left_where(JOIN *join, COND *conds)
{
  THD *thd= join->thd;
  if (!thd->lex->full_join_count || !conds)
    return conds;

  List<TABLE_LIST> right_sides;
  if (collect_full_join_right_sides(join->join_list, &right_sides,
                                    thd->mem_root))
    return conds;
  if (right_sides.is_empty())
    return conds;
  List_iterator<TABLE_LIST> li(right_sides);
  TABLE_LIST *tl;
  while ((tl= li++))
    tl->fj_left_cond= NULL;

  List<Item> single;
  List<Item> *conjuncts= top_level_conjuncts(thd, conds, &single);
  if (!conjuncts)
    return conds;

  const bool rewrite_enabled=
    join_transform_enabled(thd, OPTIMIZER_SWITCH_FULL_JOIN_REWRITE);

  List_iterator<Item> it(*conjuncts);
  Item *c;
  while ((c= it++))
  {
    if (rewrite_enabled &&
        c->type() == Item::FUNC_ITEM &&
        ((Item_func *) c)->functype() == Item_func::MULT_EQUAL_FUNC)
      continue;

    TABLE_LIST *best= full_join_right_side_for(&right_sides,
                                               c->used_tables());
    if (!best)
      continue;

    if (best->fj_left_cond)
    {
      Item *both= new (thd->mem_root)
        Item_cond_and(thd, best->fj_left_cond, c);
      if (!both)
        return conds;
      both->fix_fields(thd, 0);
      both->update_used_tables();
      best->fj_left_cond= both;
    }
    else
      best->fj_left_cond= c;
    it.remove();
  }

  if (conjuncts == &single)
    return single.elements ? conds : NULL;

  if (((Item_cond *) conds)->argument_list()->elements == 0)
    return NULL;
  conds->update_used_tables();
  return conds;
}


/*
  Whether a key value may position a table on the left side of a FULL
  JOIN that still runs as one.

  The pass that produces the right side rows emits a right row only
  when no left row matched it, so the record of which right rows
  matched is complete only once every left row has been read.  An
  access method that positions a left side table on a key value taken
  from outside the left side reads fewer left rows than that, and a
  right row whose match went unread comes back as a row that never
  matched.

  A key value that reads only tables of the same left side is allowed.
  Those tables are joined among themselves before the FULL JOIN pairs
  the two sides, so such an access decides where one left side table is
  read from another, not how much of the left side the FULL JOIN sees.

  A rewritten FULL JOIN has JOIN_TYPE_FULL cleared on both sides, so
  the operand below is found only while the join still runs as a FULL
  JOIN.
*/

bool full_join_left_side_allows(TABLE_LIST *tl,
                                       table_map used_tables)
{
  for ( ; tl ; tl= tl->embedding)
  {
    if ((tl->outer_join & (JOIN_TYPE_FULL | JOIN_TYPE_LEFT)) !=
        (JOIN_TYPE_FULL | JOIN_TYPE_LEFT))
      continue;
    return !(used_tables &
             ~(tl->nested_join ? tl->nested_join->used_tables
                               : tl->table->map));
  }
  return true;
}


/*
  Find the span of the picked join order occupied by one FULL JOIN
  operand nest and record it, if the nest is worth a run of its own.

  A nest of a single surviving table is left out, since a run holding one
  table is that table.  A nest whose tables the order did not keep
  together is left out as well, which leaves it joined table by table as
  before.  restrict_to_unplaced_fj_tables is what keeps them together,
  so a broken span means that rule and this one disagree.
*/

static void
record_full_join_nest_span(JOIN *join, TABLE_LIST *nest)
{
  table_map nest_tables= nest->nested_join->used_tables &
                         ~join->eliminated_tables;
  uint first= 0, count= 0;
  bool broken= false;

  for (uint i= 0; i < join->table_count; i++)
  {
    if (!(join->best_positions[i].table->table->map & nest_tables))
      continue;
    if (!count)
      first= i;
    else if (i != first + count)
      broken= true;
    count++;
  }

  DBUG_ASSERT(!broken);
  if (broken || count < 2)
    return;

  Full_join_nest_span *span=
    &join->fj_nest_spans[join->fj_nest_span_count++];
  span->nest= nest;
  span->first= first;
  span->count= count;
}


/*
  Walk the join tree and record the span of every nest that is an operand
  of a FULL JOIN.  A nest is handled before the tree below it, so the
  spans come out outermost first.
*/

static void
record_full_join_nest_spans(JOIN *join, List<TABLE_LIST> *lst)
{
  List_iterator<TABLE_LIST> li(*lst);
  TABLE_LIST *tl;

  while ((tl= li++))
  {
    if (!tl->nested_join)
      continue;
    if (tl->outer_join & JOIN_TYPE_FULL)
    {
      /*
        Whether the nest ends up with a run of its own follows from the
        join order, so the answer from a previous optimization says
        nothing here.
      */
      tl->nested_join->materialized_full_join= FALSE;
      tl->nested_join->materialized_full_join_tab= NULL;
      record_full_join_nest_span(join, tl);
    }
    record_full_join_nest_spans(join, &tl->nested_join->join_list);
  }
}


/*
  Record where the FULL JOIN operand nests land in the picked join order.

  The join order search is over by the time this runs, so each nest now
  has a definite place, which is what the layout of the JOIN_TAB array
  needs in order to gather a nest into a run of its own.
*/

bool JOIN::record_full_join_nest_spans()
{
  fj_nest_spans= NULL;
  fj_nest_span_count= 0;

  if (!full_join_nest_tables)
    return false;

  /*
    The nests are strictly nested or disjoint and each holds at least two
    tables, so there are fewer of them than there are tables.
  */
  if (!(fj_nest_spans= thd->alloc<Full_join_nest_span>(table_count)))
    return true;

  ::record_full_join_nest_spans(this, &select_lex->top_join_list);

  return false;
}


/*
  Open a run for the FULL JOIN operand nest whose span starts at tablenr.

  Allocates the nest's JOIN_TABs, links them under a placeholder JOIN_TAB
  the way an SJ-Materialization nest is linked under its own placeholder,
  and pushes a new entry onto the run stack (run_root/run_last_pos), so
  that further tables and any runs opened at the tables to follow are
  read as being inside this nest.

  RETURN
    FALSE  OK, *j advanced to the placeholder's bush_children.start
    TRUE   Out of memory
*/

bool
open_full_join_nest_run(THD *thd, JOIN *join, JOIN_TAB *&j, uint tablenr,
                         Full_join_nest_span *span,
                         JOIN_TAB **run_root, uint *run_last_pos,
                         uint &run_depth)
{
  JOIN_TAB *jt;

  if (!(jt= thd->alloc<JOIN_TAB>(span->count)))
    return TRUE;
  bzero((void*) j, sizeof(JOIN_TAB));
  j->join= join;
  j->table= NULL;
  j->ref.key= -1;
  j->on_expr_ref= (Item**) &null_ptr;
  j->bush_root_tab= run_depth ? run_root[run_depth-1] : NULL;
  j->bush_children.start= jt;
  j->bush_children.end= jt + span->count;
  j->bush_children.kind= JOIN_TAB_RANGE_FULL_JOIN;
  j->bush_children.nest= span->nest;
  j->bush_children.nest->nested_join->materialized_full_join= TRUE;
  j->bush_children.nest->nested_join->materialized_full_join_tab= j;
  if (join->join_tab_ranges.push_back(&j->bush_children, thd->mem_root))
    return TRUE;
  DBUG_ASSERT(run_depth < MAX_TABLES);
  run_root[run_depth]= j;
  run_last_pos[run_depth]= tablenr + span->count - 1;
  run_depth++;
  j= jt;
  return FALSE;
}


bool TABLE_LIST::is_materialized_full_join()
{
  return nested_join && nested_join->materialized_full_join;
}


/*
  Collect an Item_field for every column of the tables under a run that
  the optimizer marked as read.  A run inside this one is descended into,
  since the temporary table has to hold the values themselves and not a
  reference to the table that produced them.
*/

static bool
collect_full_join_nest_cols(THD *thd, JOIN_TAB *start, JOIN_TAB *end,
                            List<Item> *cols)
{
  for (JOIN_TAB *tab= start; tab != end; tab++)
  {
    TABLE *table;

    if (tab->has_bush_children())
    {
      if (collect_full_join_nest_cols(thd, tab->bush_children.start,
                                      tab->bush_children.end, cols))
        return TRUE;
      continue;
    }

    table= tab->table;
    for (Field **field= table->field; *field; field++)
    {
      Item_field *item;

      if (!bitmap_is_set(table->read_set, (*field)->field_index))
        continue;
      if (!(item= new (thd->mem_root) Item_field(thd, *field)) ||
          cols->push_back(item, thd->mem_root))
        return TRUE;
    }
  }
  return FALSE;
}


/*
  Create the temporary table that holds the rows of a FULL JOIN operand
  nest.

  The table holds every column of the nest's tables that is read, which
  is a superset of what anything outside the nest refers to.  Reading a
  row back into those same columns therefore restores every value an
  enclosing condition can ask for, and lets such a condition go on
  naming the nest's tables as the query wrote them.

  Two things a materialized semi join does are wrong here.  Duplicate
  rows are kept, since the enclosing join is an ordinary join and not a
  test for existence.  A row holding NULL is kept as well, since a nest
  containing an outer join produces null complemented rows that are part
  of the answer, and producing them inside the nest rather than outside
  it is the whole point of the run.
*/

bool
setup_full_join_materialization(JOIN_TAB *tab)
{
  THD *thd= tab->join->thd;
  TABLE_LIST *nest= tab->bush_children.nest;
  Full_join_mat_info *mat;
  const LEX_CSTRING full_join_name= { STRING_WITH_LEN("full-join") };
  DBUG_ENTER("setup_full_join_materialization");
  DBUG_ASSERT(tab->is_full_join_nest());

  if (!(mat= new (thd->mem_root) Full_join_mat_info))
    DBUG_RETURN(TRUE);

  mat->table_param.init();
  mat->table_param.bit_fields_as_long= TRUE;

  if (collect_full_join_nest_cols(thd, tab->bush_children.start,
                                  tab->bush_children.end, &mat->table_cols))
    DBUG_RETURN(TRUE);

  /*
    Nothing in the query need read a column of the nest, and then the only
    thing the temporary table carries is how many rows the nest produced.
    A table still has to have a column, so give it a constant one, the
    same way create_dummy_tmp_table does.  Reading a row back copies
    nothing, since there is no column of the nest to restore.
  */
  if (mat->table_cols.is_empty())
  {
    Item *column_item= new (thd->mem_root) Item_int(thd, 1);
    if (!column_item ||
        mat->table_cols.push_back(column_item, thd->mem_root))
      DBUG_RETURN(TRUE);
  }

  mat->table_param.field_count= mat->table_cols.elements;
  mat->table_param.func_count= mat->table_param.field_count;

  if (!(mat->table= create_tmp_table(thd, &mat->table_param, mat->table_cols,
                                     (ORDER*) 0, FALSE /* distinct */,
                                     1 /* save_sum_fields */,
                                     thd->variables.option_bits |
                                     TMP_TABLE_ALL_COLUMNS,
                                     HA_POS_ERROR /* rows_limit */,
                                     &full_join_name)))
    DBUG_RETURN(TRUE);

  mat->table->map= nest->nested_join->used_tables;
  mat->table->file->extra(HA_EXTRA_WRITE_CACHE);
  mat->copy_field= NULL;
  mat->materialized= FALSE;

  if (tab->join->sj_tmp_tables.push_back(mat->table, thd->mem_root) ||
      tab->join->full_join_mat_list.push_back(mat, thd->mem_root))
    DBUG_RETURN(TRUE);

  tab->bush_children.mat= mat;
  tab->table= mat->table;
  tab->tab_list= nest;
  mat->table->pos_in_table_list= nest;

  DBUG_RETURN(FALSE);
}


/*
  A copy of a materialized FULL JOIN operand nest's recorded join
  condition, for this execution to evaluate.

  The recording happens once, during the first optimization of a
  statement, and it is an AND node of the server's own making rather than
  a part of the parsed tree.  Every item a statement creates is cleaned
  up when an execution ends, which leaves that node unfixed, and no later
  execution records it again because by then the expression it was copied
  from has moved into the enclosing FULL JOIN's ON.  Restoring the
  condition from the recording for each execution, the way an ON
  expression is restored from prep_on_expr, keeps the recording itself
  out of execution and gives every execution a node it fixed.
*/

static Item *restore_full_join_nest_cond(THD *thd, Item *recorded)
{
  Item *cond= recorded->copy_andor_structure(thd);

  if (!cond)
    return NULL;
  if (!cond->fixed())
  {
    cond->fix_fields(thd, 0);
    cond->update_used_tables();
  }
  return cond;
}


/*
  Apply a materialized FULL JOIN operand nest's own join condition while
  the nest is computed.

  Simplifying the join tree moved the ON expressions of the inner joins
  inside the nest into the enclosing FULL JOIN's ON, which is checked only
  once that join has positioned the whole nest.  The nest is computed
  before that, so without its own condition it produces the cross product
  of its tables.  The condition goes on the last table of the run, where
  every table of the nest holds a row.
*/

void attach_full_join_nest_conds(THD *thd, JOIN *join)
{
  for (JOIN_TAB *tab= first_depth_first_tab(join); tab;
       tab= next_depth_first_tab(join, tab))
  {
    Item *recorded;
    Item *cond;

    if (!tab->is_full_join_nest())
      continue;

    recorded= tab->bush_children.nest->nested_join->fj_inner_cond;
    if (!recorded)
      continue;

    if (!(cond= restore_full_join_nest_cond(thd, recorded)))
      continue;

    JOIN_TAB *last= tab->bush_children.end - 1;
    add_cond_and_fix(thd, &last->select_cond, cond);
    if (last->select)
      last->select->cond= last->select_cond;
  }
}


/*
  The tab that completes one side of a FULL JOIN.

  A predicate over the whole side can only be checked once every table of
  the side holds a row.  That is the entry standing for the side when the
  side is a nest computed into a temporary table, and otherwise the last
  tab in the plan holding one of the side's tables, which for a side that
  is a single table is that table.
*/

static JOIN_TAB *tab_for_full_join_right_side(JOIN *join, TABLE_LIST *side)
{
  table_map side_map= join_side_map(side);
  JOIN_TAB *last= NULL;

  for (JOIN_TAB *tab= first_depth_first_tab(join); tab;
       tab= next_depth_first_tab(join, tab))
  {
    if (!tab->table)
      continue;
    if (tab->tab_list == side)
      return tab;
    if (tab->table->map & side_map)
      last= tab;
  }
  return last;
}


/*
  The tab a lifted left side predicate can first be checked on.

  A lifted predicate references the FULL JOIN's left side and may also
  reference tables outside the FULL JOIN, which the planner is free to
  place after the right partner.  A predicate checked on a tab that
  precedes a table it references reads that table's record buffer before
  the table has been read, so the check waits for the last of them.  The
  tables of a nest computed into a temporary table hold their row once
  the nest's own tab produces one, so a reference into such a nest
  counts as a reference to the nest.

  Returns the right partner when no referenced table follows it.
*/

static JOIN_TAB *tab_for_full_join_left_cond(JOIN *join, JOIN_TAB *partner,
                                             table_map used)
{
  JOIN_TAB *last= partner;
  JOIN_TAB *root= NULL;
  uint pos= 0;
  uint root_pos= 0;
  uint last_pos= 0;
  bool seen_partner= false;

  for (JOIN_TAB *tab= first_depth_first_tab(join); tab;
       tab= next_depth_first_tab(join, tab), pos++)
  {
    if (!tab->bush_root_tab)
    {
      root= tab;
      root_pos= pos;
    }
    if (tab == partner)
    {
      last_pos= pos;
      seen_partner= true;
      continue;
    }
    if (!seen_partner || !tab->table || !(tab->table->map & used))
      continue;
    if (root && root_pos > last_pos)
    {
      last= root;
      last_pos= root_pos;
    }
  }
  return last;
}


/*
  Hold a condition the plan wants to check on a surviving FULL JOIN's
  left side on the right partner instead.

  Distributing the WHERE over the plan does not preserve the units the
  WHERE was written in.  From a disjunction it takes, for each table,
  the part of every disjunct that the table alone can decide and ORs
  those parts together, and the result is a condition no conjunct of the
  WHERE ever contained.  The equality machinery likewise leaves
  conditions behind that were not there when the WHERE was first
  examined.  Anything reaching a left side table this way is a predicate
  the left side is not allowed to apply, for the reason
  lift_full_join_left_where describes: the left row also carries the
  record that a right row matched, so dropping it before the match
  reaches the right partner turns a matched right row into a right-only
  row.

  Move such a condition to the right partner, where
  attach_full_join_left_conds puts it behind the found-match guard.  The
  right partner follows every table of the left side in the join order,
  so every table the condition reads holds a row there.  A condition
  that also reads a table the plan places after the right partner cannot
  be checked there and stays where it is; being after the partner, it is
  already checked after the match.

  prefix is the tables the plan has positioned at the point the
  condition is being attached.  A condition manufactured from a
  disjunction inherits the whole disjunction's used_tables(), which
  names tables the manufactured condition does not read, so the tables
  it can read are the ones the two maps have in common.

  Returns true when the condition was taken.
*/

bool hold_full_join_left_cond(THD *thd, JOIN *join,
                                     List<TABLE_LIST> *right_sides,
                                     table_map prefix, COND *cond)
{
  table_map used= cond->used_tables() & prefix;
  TABLE_LIST *side;
  JOIN_TAB *partner;

  if (right_sides->is_empty())
    return false;
  if (!(side= full_join_right_side_for(right_sides, used)))
    return false;
  if (!(partner= tab_for_full_join_right_side(join, side)))
    return false;
  if (tab_for_full_join_left_cond(join, partner, used) != partner)
    return false;

  add_cond_and_fix(thd, &side->fj_left_cond, cond);
  return true;
}


/*
  Return the lifted conjuncts the plan does not need lifted.

  A conjunct is lifted so that a predicate over the FULL JOIN's left
  side is checked only after the match is recorded on the right partner.
  A conjunct that also references a table the plan places after the
  right partner is already checked after the match without any help,
  and it cannot be checked on the right partner at all, since that table
  has not been read there.  Put those back in the WHERE, where the usual
  distribution of predicates over the plan places them on the last table
  they reference.

  Called once the join order is known and before the WHERE is
  distributed.  Returns the WHERE with the returned conjuncts restored.
*/

COND *restore_full_join_left_conds(JOIN *join, COND *conds)
{
  THD *thd= join->thd;
  List<TABLE_LIST> right_sides;

  if (!thd->lex->full_join_count)
    return conds;
  if (collect_full_join_right_sides(join->join_list, &right_sides,
                                    thd->mem_root))
    return conds;

  List_iterator<TABLE_LIST> li(right_sides);
  TABLE_LIST *side;
  while ((side= li++))
  {
    JOIN_TAB *partner;
    Item *cond= side->fj_left_cond;

    if (!cond)
      continue;
    if (!(partner= tab_for_full_join_right_side(join, side)))
      continue;
    if (tab_for_full_join_left_cond(join, partner, cond->used_tables()) ==
        partner)
      continue;

    side->fj_left_cond= NULL;
    if (!conds)
    {
      conds= cond;
      continue;
    }
    /*
      lift_full_join_left_where took the conjunct out of the top level
      AND, so putting it back there restores the WHERE it started from.
    */
    if (conds->type() == Item::COND_ITEM &&
        ((Item_cond *) conds)->functype() == Item_func::COND_AND_FUNC)
    {
      if (((Item_cond *) conds)->argument_list()->push_back(cond,
                                                            thd->mem_root))
        return conds;
      conds->update_used_tables();
      continue;
    }
    add_cond_and_fix(thd, &conds, cond);
  }
  return conds;
}


/*
  Attach each surviving FULL JOIN's lifted left side predicates to its
  right partner under the found-match guard, so they are checked only
  after the match is recorded.  lift_full_join_left_where moved these
  conjuncts onto the right partner's TABLE_LIST and
  restore_full_join_left_conds returned the ones the plan does not need
  lifted; here the rest join the partner's pushed-down conditions.
  Returns true on error.
*/

bool attach_full_join_left_conds(THD *thd, JOIN *join)
{
  List<TABLE_LIST> right_sides;
  if (collect_full_join_right_sides(join->join_list, &right_sides,
                                    thd->mem_root))
    return true;

  List_iterator<TABLE_LIST> li(right_sides);
  TABLE_LIST *side;
  while ((side= li++))
  {
    if (!side->fj_left_cond)
      continue;
    JOIN_TAB *tab= tab_for_full_join_right_side(join, side);
    if (!tab)
      continue;
    COND *guarded= add_found_match_trig_cond(thd, tab->first_inner,
                                             side->fj_left_cond, 0);
    if (!guarded)
      return true;
    add_cond_and_fix(thd, &tab->select_cond, guarded);
    if (tab->select)
      tab->select->cond= tab->select_cond;
  }
  return false;
}


/*
  Move the WHERE predicates over a surviving FULL JOIN's right side out
  of the WHERE, when that side is a nest the chosen join order computes
  into a temporary table.

  Distributing the WHERE over the plan places such a predicate on a table
  inside the nest, since that is the last tab holding a table it
  references.  There it decides which rows the nest holds rather than
  which rows of the join survive.  A nest row it drops is a row the FULL
  JOIN can no longer match, so a left side row that would have matched it
  reports no match and is emitted null complemented, and a WHERE that
  accepts NULLs then passes that row on.  A side that is a single table
  needs nothing here, since the predicate lands on that table and that is
  also where the null complemented row is built.

  Held on the right side entry, the predicates are attached by
  attach_full_join_nest_where to the tab that stands for the whole nest.
  Only the predicates a tab exists for are taken, so nothing is left with
  no place to be checked.  Called once the join order is known and after
  the constant part of the WHERE has been taken out.  Returns the WHERE
  with the held conjuncts removed.
*/

COND *lift_full_join_nest_where(THD *thd, JOIN *join, COND *cond)
{
  List<TABLE_LIST> right_sides;
  List<Item> single;
  List<Item> *conjuncts;
  List<Item> keep;
  Item_cond_and *reduced;
  bool lifted= false;

  if (!thd->lex->full_join_count)
    return cond;
  if (collect_full_join_right_sides(join->join_list, &right_sides,
                                    thd->mem_root))
    return cond;
  if (right_sides.is_empty())
    return cond;

  List_iterator<TABLE_LIST> li(right_sides);
  TABLE_LIST *side;
  while ((side= li++))
    side->fj_nest_where= NULL;

  if (!cond)
    return cond;
  if (!(conjuncts= top_level_conjuncts(thd, cond, &single)))
    return cond;

  List_iterator<Item> it(*conjuncts);
  Item *c;
  while ((c= it++))
  {
    table_map used= c->used_tables();
    bool held= false;

    li.rewind();
    while ((side= li++))
    {
      table_map side_map;

      if (!side->is_materialized_full_join())
        continue;
      if (!tab_for_full_join_right_side(join, side))
        continue;
      side_map= join_side_map(side);
      if (!(used & side_map) || (used & ~side_map))
        continue;
      add_cond_and_fix(thd, &side->fj_nest_where, c);
      held= lifted= true;
      break;
    }
    if (!held && keep.push_back(c, thd->mem_root))
      return cond;
  }

  if (!lifted)
    return cond;
  if (keep.is_empty())
    return NULL;
  if (keep.elements == 1)
    return keep.head();
  if (!(reduced= new (thd->mem_root) Item_cond_and(thd, keep)))
    return cond;
  reduced->quick_fix_field();
  reduced->update_used_tables();
  return reduced;
}


/*
  Attach the predicates lift_full_join_nest_where holds on a surviving
  FULL JOIN's right side to the tab that stands for that side.

  The tab produces the whole nest as one row, and the FULL JOIN null
  complements that row for a left side row that matched nothing, so this
  is the first place the predicate can see the row the query returns.
  The found-match guard holds the predicate back until the FULL JOIN
  match has been recorded, so rejecting a row here cannot turn a right
  side row that did match into a right-only row.

  Returns true on error.
*/

bool attach_full_join_nest_where(THD *thd, JOIN *join)
{
  List<TABLE_LIST> right_sides;

  if (!thd->lex->full_join_count)
    return false;
  if (collect_full_join_right_sides(join->join_list, &right_sides,
                                    thd->mem_root))
    return true;

  List_iterator<TABLE_LIST> li(right_sides);
  TABLE_LIST *side;
  while ((side= li++))
  {
    JOIN_TAB *tab;
    COND *guarded;

    if (!side->fj_nest_where)
      continue;
    if (!(tab= tab_for_full_join_right_side(join, side)))
      continue;
    if (!(guarded= add_found_match_trig_cond(thd, tab->first_inner,
                                             side->fj_nest_where, 0)))
      return true;
    add_cond_and_fix(thd, &tab->select_cond, guarded);
    if (tab->select)
      tab->select->cond= tab->select_cond;
  }
  return false;
}


/*
  Find the table an outer join's ON condition must be attached to when a
  surviving FULL JOIN sits inside that outer join.

  A surviving FULL JOIN runs as a LEFT JOIN of its left side over its
  right side, then a pass rescans the right side and emits the rows that
  never matched a left row.  That is correct only if every left side row
  reaches the right side, so that every match is recorded.  An enclosing
  outer join's ON condition breaks this when it lands on the FULL JOIN's
  left side, either in a left table's select_cond or in the on_precond
  that stops the left side from being read at all.  A left row it rejects
  never reaches the right side, so a right row that did match is never
  recorded and the pass emits it as a row the FULL JOIN never produced.

  The condition belongs on the FULL JOIN's right side table instead.  That
  table is the inner table of the FULL JOIN's own outer join scope, so the
  standard wrapping in make_join_select adds that scope's found-match
  guard, which holds the condition back until the FULL JOIN match has been
  recorded and then applies it to the complete FULL JOIN row.  This is the
  same treatment an enclosing ON condition already gets when it lands on
  the inner table of a plain nested outer join.

  Return the right side table of the last surviving FULL JOIN that lies
  after dest and within the inner scope starting at first_inner_tab, or
  NULL when there is none.  The last one is chosen so that chained FULL
  JOINs all record their matches before the condition applies.  Since the
  search starts after dest, and dest is never earlier than
  first_inner_tab, a FULL JOIN whose own ON condition this is can never be
  returned.
*/

JOIN_TAB *fj_deferral_target(JOIN *join, JOIN_TAB *dest,
                                    JOIN_TAB *first_inner_tab)
{
  JOIN_TAB *target= NULL;

  if (!join->thd->lex->full_join_count)
    return NULL;

  for (JOIN_TAB *tab= dest + 1; tab <= first_inner_tab->last_inner; tab++)
  {
    TABLE_LIST *tl= tab->tab_list;
    if (!tab->table || !tl || !tl->foj_partner)
      continue;
    if ((tl->outer_join & (JOIN_TYPE_FULL | JOIN_TYPE_RIGHT)) ==
        (JOIN_TYPE_FULL | JOIN_TYPE_RIGHT))
      target= tab;
  }
  return target;
}


/*
  end_select compatible function that writes one row of a FULL JOIN
  operand nest into the temporary table the nest is computed into.

  Every row the nest produces is written.  A semi join's version drops a
  row holding NULL and lets the table remove duplicates, neither of which
  applies here, since the enclosing join is an ordinary join over the
  nest's result and a nest containing an outer join produces null
  complemented rows that belong to that result.
*/

enum_nested_loop_state
end_full_join_materialize(JOIN *join, JOIN_TAB *join_tab, bool end_of_records)
{
  int error;
  THD *thd= join->thd;
  Full_join_mat_info *mat= join_tab[-1].bush_root_tab->bush_children.mat;
  DBUG_ENTER("end_full_join_materialize");

  if (!end_of_records)
  {
    TABLE *table= mat->table;

    fill_record(thd, table, table->field, mat->table_cols, true, false, true);
    if (unlikely(thd->is_error()))
      DBUG_RETURN(NESTED_LOOP_ERROR);
    if (unlikely((error= table->file->ha_write_tmp_row(table->record[0]))))
    {
      if (table->file->is_fatal_error(error, HA_CHECK_DUP) &&
          create_internal_tmp_table_from_heap(thd, table,
                                              mat->table_param.start_recinfo,
                                              &mat->table_param.recinfo,
                                              error, 1, NULL))
        DBUG_RETURN(NESTED_LOOP_ERROR);
    }
  }
  DBUG_RETURN(NESTED_LOOP_OK);
}


/*
  Set up reading the temporary table a FULL JOIN operand nest was
  computed into.

  The table is always scanned, since the enclosing join has no key into
  it.  Each column read is copied back into the field of the nest table
  it was taken from, which is what lets a condition outside the nest go
  on naming that table.  The last entry of the run sends its rows to the
  temporary table rather than onwards, which is how the nest is computed
  in isolation.
*/

bool
setup_full_join_materialization_scan(JOIN_TAB *tab)
{
  Full_join_mat_info *mat= tab->bush_children.mat;
  List_iterator<Item> it(mat->table_cols);
  Item *item;
  /* Position in table_cols, which is also the temporary table's column. */
  uint i= 0;
  /* How many of those columns stand for a column of the nest. */
  uint copies= 0;
  DBUG_ENTER("setup_full_join_materialization_scan");
  DBUG_ASSERT(tab->is_full_join_nest());

  if (!(mat->copy_field= new Copy_field[mat->table_cols.elements]))
    DBUG_RETURN(TRUE);

  while ((item= it++))
  {
    /*
      A column that stands for no column of the nest has nothing to be
      copied back into.
    */
    if (item->type() == Item::FIELD_ITEM)
    {
      Field *copy_to= ((Item_field *) item)->field;

      /*
        A column of the nest declared NOT NULL still reads as NULL when an
        outer join inside the nest null complemented its row, so the copy
        back has to restore that state rather than store a zero.
      */
      mat->copy_field[copies].set_restoring_null_row(copy_to,
                                                     mat->table->field[i]);
      /* The write_set of the nest's tables must allow the copying. */
      bitmap_set_bit(copy_to->table->write_set, copy_to->field_index);
      copies++;
    }
    i++;
  }

  tab->type= JT_ALL;
  tab->read_first_record= join_init_read_record;
  tab->read_record.copy_field= mat->copy_field;
  tab->read_record.copy_field_end= mat->copy_field + copies;

  tab->bush_children.end[-1].next_select= end_full_join_materialize;

  DBUG_RETURN(FALSE);
}


/*
  Return true if the given TABLE_LIST is a FULL JOIN operand or is
  embedded (at any depth) within a FULL JOIN nest.  Join caching is
  disabled for such tables because the null-complement rescan requires a
  plain sequential scan of the right side (rather than one that reads
  from a buffered join cache).
*/
bool is_in_full_join_scope(TABLE_LIST *tl)
{
  if (tl->outer_join & JOIN_TYPE_FULL)
    return true;
  for (TABLE_LIST *embedding= tl->embedding;
       embedding;
       embedding= embedding->embedding)
  {
    if (embedding->outer_join & JOIN_TYPE_FULL)
      return true;
  }
  return false;
}


/**
   Assert that the ON expression of the right operand is a proper
   condition.

   When the right operand is a nested join, descend into the nest and
   apply the same check to every table it contains.

   @param right_table the right operand of the FULL JOIN
*/

static void assert_full_join_on_expr_is_cond(TABLE_LIST *right_table)
{
  // Recursive case.
  if (right_table->nested_join)
  {
    List_iterator<TABLE_LIST> it(right_table->nested_join->join_list);
    TABLE_LIST *tbl;
    while ((tbl= it++))
      assert_full_join_on_expr_is_cond(tbl);

    return;
  }

  // Base case.
  if (right_table->on_expr &&
      !(right_table->outer_join & JOIN_TYPE_NATURAL))
  {
    DBUG_ASSERT((right_table->on_expr->base_flags &
                 item_base_t::IS_COND) == item_base_t::IS_COND);
  }
}


/**
   Rewrite a FULL JOIN to a LEFT JOIN by mutating the
   left and right table state to make them appear as though
   the user wrote the FULL JOIN as a LEFT JOIN originally.

   @param left_table  table t1 in t1 FULL JOIN t2
   @param right_table table t2 in t1 FULL JOIN t2
*/

static void rewrite_full_to_left(TABLE_LIST *left_table,
                                 TABLE_LIST *right_table)
{
  // Grammar does not mark the left table at all
  left_table->outer_join= 0;

  /*
    Clear FULL JOIN flag and do as the grammar does by marking
    the right table as JOIN_TYPE_LEFT.
  */
  right_table->outer_join= JOIN_TYPE_LEFT;

  /*
    The right table must have an ON clause.  NATURAL JOINs get
    this not from the grammar but they're built before simplify_joins
    is called.
  */
  DBUG_ASSERT(right_table->on_expr);

#ifdef DBUG_ASSERT_EXISTS
  assert_full_join_on_expr_is_cond(right_table);
#endif
}


/**
   Swap the left and right operands of a FULL JOIN that survives
   simplify_joins.

   FULL JOIN is symmetric on its operands, so swapping does not
   change query semantics.  The swap is needed because the null
   complement pass keys off a JOIN_TAB carrying JOIN_TYPE_FULL |
   JOIN_TYPE_RIGHT.  alloc_full_join_duplicate_filters attaches the
   fj_dups filter to that JOIN_TAB, and the rescan reads its rowid.

   This is a temporary limitation given that, at this point in time,
   we don't support anything but base tables on the right side of
   a FULL JOIN.

   When the parser places a nested join expression as the
   right operand and a single base table as the left operand,
   the FULL|RIGHT bits land on the nest, so no
   fj_dups is allocated and the null complement pass never
   fires.  Swapping puts the leaf on the right where the filter can
   be attached and the rescan can run.

   @param left_table  table t1 in t1 FULL JOIN t2
   @param right_table table t2 in t1 FULL JOIN t2
*/

static void swap_full_join_sides(TABLE_LIST *left_table,
                                 TABLE_LIST *right_table)
{
  DBUG_ASSERT(test_all_bits(left_table->outer_join,
                            JOIN_TYPE_FULL | JOIN_TYPE_LEFT));
  DBUG_ASSERT(test_all_bits(right_table->outer_join,
                            JOIN_TYPE_FULL | JOIN_TYPE_RIGHT));
  DBUG_ASSERT(right_table->on_expr);

  /*
    Swap the LEFT|RIGHT roles, keeping the FULL bit (and any other
    bits, e.g., JOIN_TYPE_NATURAL) intact on both sides.
  */
  left_table->outer_join= (left_table->outer_join & ~JOIN_TYPE_LEFT)
                          | JOIN_TYPE_RIGHT;
  right_table->outer_join= (right_table->outer_join & ~JOIN_TYPE_RIGHT)
                          | JOIN_TYPE_LEFT;

  /*
    The parser attaches the ON clause to the right operand of a FULL
    JOIN.  After the swap the new right operand (was left) carries it.
  */
  left_table->on_expr= right_table->on_expr;
  right_table->on_expr= nullptr;

  left_table->prep_on_expr= right_table->prep_on_expr;
  right_table->prep_on_expr= nullptr;

  left_table->on_context= right_table->on_context;
  right_table->on_context= nullptr;
}


/**
   Rewrite a FULL JOIN to a RIGHT JOIN by mutating the
   left and right table state to make them appear as though
   the user wrote the FULL JOIN as a RIGHT JOIN originally.

   It's important to keep in mind that this function does its
   work updating the tables to prepare them to be swapped in
   the join order.  Had the user written the query as a RIGHT
   JOIN, it would've then been converted to a LEFT JOIN by
   convert_right_join.  The caller will swap them in the join
   list, so we prepare them in place, then once they're swapped
   they will have the correct respective state.

   Consequently, in this method, we change the right_table with
   the understanding that it will swap places with the left_table
   very shortly (similarly with respect to the right_table).

   @param left_table  table t1 in t1 FULL JOIN t2
   @param right_table table t2 in t1 FULL JOIN t2
*/

static void rewrite_full_to_right(TABLE_LIST *left_table,
                                  TABLE_LIST *right_table)
{
  // Grammar does not mark the right table at all.
  right_table->outer_join= 0;

  /*
    Clear FULL JOIN flag and do as convert_right_join does which
    has the effect of marking the left table as JOIN_TYPE_RIGHT.
  */
  left_table->outer_join= JOIN_TYPE_RIGHT;

  /*
    The right table must have an ON clause.  NATURAL JOINs get
    this from setup_natural_join_row_types().

    The ON clause is moved from the right table to the left one
    because, again, the tables will be swapped in the join list
    to imitate the convert_right_join operation that would've been
    done had the user written this query as a RIGHT JOIN instead
    of a FULL JOIN.
  */
  DBUG_ASSERT(right_table->on_expr);
  left_table->on_expr= right_table->on_expr;
  right_table->on_expr= nullptr;

  /*
    Update prep_on_expr to match the post-rewrite state so that
    reinit_before_use() restores the correct ON expressions for
    prepared statement re-execution.  The ON expression moved from
    the right table to the left, so prep_on_expr must follow.
  */
  left_table->prep_on_expr= right_table->prep_on_expr;
  right_table->prep_on_expr= nullptr;

  /*
    Prepare the right table to become the left table by
    clearing its context.  The left table retains the context
    set by the grammar.
  */
  right_table->on_context= nullptr;
}


/**
  The tables the current conditions reject NULLs for, as far as the
  simplifications in simplify_joins may rely on that rejection.

  A condition allows dropping an outer join's null complemented rows
  only when a row that keeps those NULLs cannot reach the result.  The
  WHERE clause has that property, and so does the ON expression of a
  LEFT or RIGHT JOIN with respect to its own inner operand, since rows
  of that operand which fail the ON are not emitted.

  The ON expression of a FULL JOIN does not have it while the FULL JOIN
  stays a FULL JOIN, since the operand keeps the rows that fail the ON,
  null complemented on the other side.  Null rejection in that ON then
  says nothing about which rows survive and must not drive any
  simplification inside the operand it is attached to.  Nothing here
  restricts the WHERE clause, which goes on rewriting FULL JOINs the
  way it did before, nested ones included.

  A FULL JOIN the WHERE clause turns into a LEFT or RIGHT JOIN does get
  the property back for whichever operand lands on the inner side.  The
  descent into the operand runs before that outcome is known, so the
  rejection is given up there as well.  The cost is a plan that keeps
  an outer join it could have dropped, never a wrong result.

  @param conds       the conditions in force at this point
  @param fj_operand  the FULL JOIN operand nest conds is the ON
                     expression of, NULL when conds is not one

  @return the tables NULLs are rejected for, or no tables at all when
          the rejection cannot be relied on
*/

table_map usable_not_null_tables(COND *conds, TABLE_LIST *fj_operand)
{
  if (!conds || fj_operand)
    return (table_map) 0;

  return conds->not_null_tables();
}


/**
  What the WHERE clause allows a FULL JOIN to become.
*/

enum Full_join_outcome
{
  /*
    The WHERE clause rejects NULLs on the right operand, so no row that
    is null complemented on that side reaches the result.  Unmatched
    left rows are therefore dead and the FULL JOIN means the same thing
    as a RIGHT JOIN.  RIGHT JOIN does not exist at execution, so the
    operands are swapped afterwards and a LEFT JOIN remains.
  */
  FULL_JOIN_TO_RIGHT,
  /*
    The WHERE clause rejects NULLs on the left operand, so unmatched
    right rows are dead and the FULL JOIN means the same thing as a
    LEFT JOIN.  The operands keep their positions.
  */
  FULL_JOIN_TO_LEFT,
  /*
    Neither operand is covered by a null rejecting predicate, or the
    rewrite the WHERE clause would otherwise allow is unsafe.  The FULL
    JOIN stays a FULL JOIN and the null complement pass produces its
    unmatched rows at execution.
  */
  FULL_JOIN_SURVIVES
};


/**
  Decide what a FULL JOIN can become, given the tables the WHERE clause
  rejects NULLs for.

  This is the only place the decision to rewrite a FULL JOIN is made.
  Everything the caller does afterwards either carries out that decision
  or is bookkeeping that runs whatever the decision was.  It is also
  where optimizer_switch=full_join_rewrite takes effect, subject to the
  simplify_joins umbrella.

  @param thd                 current thread, for the optimizer switch
  @param left_used_tables    tables of the left operand
  @param right_used_tables   tables of the right operand
  @param not_null_tables     tables the WHERE clause rejects NULLs for
  @param left_conds_hoisted  true <=> descending into the left operand
                             moved conditions into the WHERE clause

  @return the outcome the WHERE clause permits
*/

static Full_join_outcome
classify_full_join(THD *thd,
                   table_map left_used_tables,
                   table_map right_used_tables,
                   table_map not_null_tables,
                   bool left_conds_hoisted)
{
  if (!join_transform_enabled(thd, OPTIMIZER_SWITCH_FULL_JOIN_REWRITE))
    return FULL_JOIN_SURVIVES;

  /*
    A rewrite to a RIGHT JOIN puts the left operand on the inner side.
    A condition that moved out of the left operand into the WHERE
    clause filters that operand's rows only while it stays on the outer
    side.  Once it is the inner side, the condition would reject its
    null complemented rows and lose them, so no rewrite happens.
  */
  if ((right_used_tables & not_null_tables) && !left_conds_hoisted)
    return FULL_JOIN_TO_RIGHT;

  if (left_used_tables & not_null_tables)
    return FULL_JOIN_TO_LEFT;

  return FULL_JOIN_SURVIVES;
}


/*
  The condition a FULL JOIN operand collected from the inner joins inside
  it, or NULL when the operand is a single table or collected nothing.
*/

static COND *full_join_operand_conds(TABLE_LIST *operand)
{
  if (!operand->nested_join)
    return NULL;
  return operand->nested_join->fj_inner_cond;
}


/**
  Descend into the left operand of a FULL JOIN.

  simplify_joins skips the left operand of a FULL JOIN while the join is
  still a FULL JOIN, so this is the descent that collects the operand's
  own condition.  It runs whether or not the FULL JOIN is rewritten
  afterwards, as the nest's used_tables and the ON expressions inside it
  are needed in either case.

  The descent starts from no condition rather than from the WHERE
  clause, so the ON expressions of the inner joins inside the operand
  collect into a condition of their own instead of joining the WHERE
  clause.  They decide which rows the operand has and say nothing about
  a row where the whole operand is null, which is what the enclosing
  FULL JOIN produces for a right side row that matched nothing.  The
  caller decides where the collected condition belongs once it knows
  whether the FULL JOIN survives.

  Starting from no condition also keeps the WHERE clause out of the
  null rejection analysis inside the operand.  A WHERE clause predicate
  cannot convert an outer join inside the operand while the enclosing
  FULL JOIN survives, for the same reason.

  @param join       reference to the query info
  @param in_sj      TRUE <=> processing semi-join nest's children
  @param fj_operand the FULL JOIN operand nest the enclosing conds is
                    the ON expression of, NULL when it is not one
  @param left_table the left operand
  @param right_not_null_tables not_null_tables computed for the right
                               operand
  @param left_used_tables      OUT tables of the left operand
  @param left_not_null_tables  OUT tables the left operand rejects
                               NULLs for

  @return the operand's own condition, NULL when it has none or on
          error, which the caller tells apart by the error state
*/

static COND *simplify_full_join_left_operand(JOIN *join,
                                             bool in_sj,
                                             TABLE_LIST *fj_operand,
                                             TABLE_LIST *left_table,
                                             table_map right_not_null_tables,
                                             table_map *left_used_tables,
                                             table_map *left_not_null_tables)
{
  COND *left_conds;

  if (!left_table->nested_join)
  {
    *left_used_tables= left_table->get_map();
    *left_not_null_tables= right_not_null_tables;
    return NULL;
  }

  left_conds= simplify_nested_join(join, left_table, NULL, in_sj, fj_operand,
                                   TRUE, left_used_tables,
                                   left_not_null_tables);
  /*
    An earlier descent into the operand already moved the ON expressions
    of the inner joins out of the tables that held them, so this descent
    collects nothing and the condition is the one that descent recorded.
  */
  if (!left_conds && !join->thd->is_error())
    left_conds= full_join_operand_conds(left_table);
  return left_conds;
}


/**
  Do the bookkeeping for a FULL JOIN that stays a FULL JOIN.

  Neither of the two things that happen here is an optimization.  The
  operands may be swapped so the FULL|RIGHT bits land on a leaf, which
  the null complement pass requires, and not_null_tables may be cleared
  so the caller does not go on to convert the FULL JOIN to an inner
  join.

  @param left_table  the left operand
  @param right_table IN/OUT the right operand, and after a swap the new
                     right operand
  @param li          IN/OUT the iterator into the join list
  @param left_used_tables     tables of the left operand
  @param left_not_null_tables tables the left operand rejects NULLs for
  @param used_tables          IN/OUT used_tables from simplify_joins
  @param not_null_tables      IN/OUT not_null_tables from simplify_joins
*/

static void keep_full_join(TABLE_LIST *left_table,
                           TABLE_LIST **right_table,
                           List_iterator<TABLE_LIST> *li,
                           table_map left_used_tables,
                           table_map left_not_null_tables,
                           table_map *used_tables,
                           table_map *not_null_tables)
{
  if (!left_table->nested_join && (*right_table)->nested_join &&
      (*right_table)->contains_full_join())
  {
    /*
      The FULL JOIN survives simplification with a leaf on the left
      and a nested join on the right, so the FULL|RIGHT bits sit on
      a nest, which is never a JOIN_TAB, and the null complement
      pass has no JOIN_TAB to attach an fj_dups filter to.  Swap so
      the leaf carries those bits; see swap_full_join_sides.
    */
    swap_full_join_sides(left_table, *right_table);
    *used_tables= left_used_tables;
    *right_table= li->swap_next();
  }
  else if (*used_tables & *not_null_tables)
  {
    /*
      The WHERE clause rejects NULLs on the right side, yet no rewrite
      happened.  Zero not_null_tables so the caller does not go on to
      convert this FULL JOIN table to an inner join, which would drop
      the null complemented rows the FULL JOIN still has to produce.
    */
    *not_null_tables= 0;
    return;
  }

  *not_null_tables= left_not_null_tables;
}


/*
  Move a FULL JOIN operand's own condition into the condition that
  encloses the join, which is where the operand's rows are decided once
  the operand is the outer side of a rewritten join.  This is the same
  move an inner join's ON expression makes on its own when no FULL JOIN
  encloses it; see hoist_on_expr_to_conds.
*/

static COND *merge_full_join_operand_conds(THD *thd, COND *conds,
                                           COND *operand_conds)
{
  if (!operand_conds)
    return conds;
  if (!conds)
    return operand_conds;

  conds= and_conds(thd, conds, operand_conds);
  conds->top_level_item();
  /* conds is always a new item as both conditions existed */
  DBUG_ASSERT(!conds->fixed());
  conds->fix_fields(thd, &conds);
  return conds;
}


/**
  Attempt to rewrite [NATURAL] FULL JOIN to LEFT, RIGHT, or INNER JOIN,
  depending on the WHERE clause and whether it rejects NULLs.  For example,
  the following queries are equivalent:

    SELECT * FROM t1 FULL JOIN t2 ON t1.v = t2.v WHERE t1.v IS NOT NULL;
    SELECT * FROM t1 LEFT JOIN t2 ON t1.v = t2.v;

  The rewritten query, be it a LEFT or RIGHT JOIN, may yet again be
  rewritten to an INNER JOIN if the WHERE clause permits.

  These parameters are the same as in simplify_joins:
  @param join        reference to the query info
  @param join_list   list representation of the join to be converted
  @param conds       WHERE expressions.  Will be AND'ed with ON expressions
                     if rewrite happens.
  @param top         true <=> conds is the where condition
  @param in_sj       TRUE <=> processing semi-join nest's children
  @param fj_operand  the FULL JOIN operand nest conds is the ON
                     expression of, NULL when conds is not one

  The following parameters are IN/OUT parameters and are mutated by
  this function:
  @param table_ptr           the current TABLE_LIST from the join list
  @param li_ptr              the iterator into the join list
  @param used_tables_ptr     used_tables from simplify_joins
  @param not_null_tables_ptr not_null_tables from simplify_joins

  @return
    - The new condition, if success
    - nullptr, otherwise
*/

COND *rewrite_full_outer_joins(JOIN *join,
                                      COND *conds,
                                      bool in_sj,
                                      TABLE_LIST *fj_operand,
                                      TABLE_LIST **right_table,
                                      List_iterator<TABLE_LIST> *li,
                                      table_map *used_tables,
                                      table_map *not_null_tables)
{
  DBUG_ENTER("rewrite_full_outer_joins");

  /*
    The join_list enumerates the tables from t_n, ..., t_0 so we always
    see the right table first.  If, on this call to rewrite_full_outer_joins,
    the current table is left member of the JOIN (e.g., left_member FULL JOIN
    ...) it means we couldn't rewrite the FULL JOIN as a LEFT, RIGHT, or
    INNER JOIN, so emit an error (unless we're in an EXPLAIN EXTENDED, permit
    that).
  */
  if ((*right_table)->outer_join & JOIN_TYPE_LEFT)
    DBUG_RETURN(conds);

  /*
    Must always see the right table before the left.  Down below, we deal
    with the left table at the same time as the right, so we'll never get
    to this point with a single table remaining in the join_list.  If
    there's a right table remaining then there will be a left one, too.
  */
  DBUG_ASSERT((*right_table)->outer_join & JOIN_TYPE_RIGHT);

  /*
    If the left table is a nested join, then recursively rewrite any
    FULL JOINs within it.  Otherwise continue to attempt to rewrite
    in the base case.
   */
  TABLE_LIST *left_table= li->peek();
  table_map left_used_tables= 0;
  table_map left_not_null_tables= 0;
  DBUG_ASSERT(test_all_bits(left_table->outer_join,
                            JOIN_TYPE_FULL | JOIN_TYPE_LEFT));

  COND *left_conds= simplify_full_join_left_operand(join, in_sj, fj_operand,
                                                    left_table,
                                                    *not_null_tables,
                                                    &left_used_tables,
                                                    &left_not_null_tables);
  if (join->thd->is_error())
    DBUG_RETURN(nullptr);

  switch (classify_full_join(join->thd, left_used_tables, *used_tables,
                             *not_null_tables, left_conds != NULL))
  {
  case FULL_JOIN_TO_RIGHT:
    /*
      A condition of the left operand's own would end up on the inner
      side of the rewritten join, which classify_full_join refuses.
    */
    DBUG_ASSERT(!left_conds);
    /*
      The right operand is the outer side of the rewritten join, so no
      row of it is null complemented and its own condition decides the
      same rows from the WHERE clause as it does inside the operand.
      The condition also stays in the ON expression it moved into, where
      it no longer decides which rows the operand has, since a row of the
      outer side reaches the result whether or not the ON expression
      holds.  Only a nest computed into a temporary table reads the
      condition back, and the rewrite below leaves no such nest.
    */
    conds= merge_full_join_operand_conds(join->thd, conds,
                                         full_join_operand_conds(*right_table));
    /*
      RIGHT JOINs don't actually exist in MariaDB!  This will do what
      the grammar does and convert_right_join together do when given a
      RIGHT JOIN.
    */
    rewrite_full_to_right(left_table, *right_table);

    // This will be reflected to the caller, too.
    *used_tables= left_used_tables;

    /*
      Swap myself with the left as though we did convert_right_join().
      Then we will have effectively done the following transformation:
        FULL -> RIGHT -> LEFT.
      Again, RIGHT JOINs don't actually exist in MariaDB!
    */
    *right_table= li->swap_next();
    --join->thd->lex->full_join_count;
    break;

  case FULL_JOIN_TO_LEFT:
    rewrite_full_to_left(left_table, *right_table);
    --join->thd->lex->full_join_count;
    *not_null_tables= left_not_null_tables;
    /*
      The left operand is the outer side of the rewritten join, so no row
      of it is null complemented and its own condition decides the same
      rows from the WHERE clause as it does inside the operand.
    */
    conds= merge_full_join_operand_conds(join->thd, conds, left_conds);
    break;

  case FULL_JOIN_SURVIVES:
    keep_full_join(left_table, right_table, li, left_used_tables,
                   left_not_null_tables, used_tables, not_null_tables);
    /*
      The operand's condition stays with the operand, which is computed
      into a temporary table before the FULL JOIN reads it.  A condition
      an earlier descent recorded is already there.
    */
    if (left_conds && left_conds != full_join_operand_conds(left_table))
      record_full_join_nest_cond(join->thd, left_table, left_conds);
    break;
  }

  DBUG_RETURN(conds);
}


/*
  Drop what an earlier optimization recorded on the FULL JOIN operand
  nests of a join list.

  A recorded condition is a copy of ON expressions that are themselves
  restored from prep_on_expr for every optimization, so a copy an earlier
  one made belongs to an arena that is gone.  One sweep before
  simplify_joins runs is what drops them.  simplify_joins cannot drop
  them itself as it descends into a nest more than once, and only the
  first descent still finds the ON expressions to collect.
*/

void clear_full_join_nest_conds(List<TABLE_LIST> *join_list)
{
  List_iterator<TABLE_LIST> li(*join_list);
  TABLE_LIST *table;

  while ((table= li++))
  {
    if (!table->nested_join)
      continue;
    table->nested_join->fj_inner_cond= NULL;
    clear_full_join_nest_conds(&table->nested_join->join_list);
  }
}


/*
  Remember an inner join's ON expression as the join condition of the FULL
  JOIN operand nest that holds it, before the expression moves into the
  enclosing FULL JOIN's ON.  Only a nest computed into a temporary table
  reads this back, so the expression stays in the enclosing ON as well.

  The nest is the operand as a whole, not whatever nest inside it the
  expression came from.  An ON expression that moves out of an inner join
  at any depth holds for every row the operand produces, and the operand
  is the only nest of the group that survives to be computed, so a
  condition left on a nest below it is never applied.  An operand that
  collects more than one such expression arrives here once per
  expression, and the conjunction is built up across those calls.
*/

void record_full_join_nest_cond(THD *thd, TABLE_LIST *nest,
                                       Item *on_expr)
{
  NESTED_JOIN *nested_join= nest->nested_join;
  Item *both;
  /*
    The expression itself cannot be kept, since it is about to become part
    of the enclosing FULL JOIN's ON and equality propagation rewrites that
    tree, moving conjuncts into multiple equalities and leaving the AND
    node they came from empty.  Own a copy of the AND structure for the
    same reason prep_on_expr does, which shares the conjuncts themselves
    because those are replaced in a list rather than changed in place.
  */
  Item *own= on_expr->copy_andor_structure(thd);

  if (!own)
    return;
  if (!own->fixed())
  {
    own->fix_fields(thd, 0);
    own->update_used_tables();
  }

  if (!nested_join->fj_inner_cond)
  {
    nested_join->fj_inner_cond= own;
    return;
  }
  if (!(both= new (thd->mem_root) Item_cond_and(thd,
                                                nested_join->fj_inner_cond,
                                                own)))
    return;
  both->fix_fields(thd, 0);
  both->update_used_tables();
  nested_join->fj_inner_cond= both;
}


/*
  Record a set of tables that has to occupy an unbroken span of the join
  order.  The same nest is reached only once, but a set can repeat when a
  FULL JOIN has a single nest under it, so identical sets are dropped.
*/

static void
add_full_join_group(JOIN *join, table_map group)
{
  for (uint i= 0; i < join->full_join_group_count; i++)
  {
    if (join->full_join_groups[i] == group)
      return;
  }
  join->full_join_groups[join->full_join_group_count++]= group;
}


/*
  Walk the join tree and collect, into full_join_nest_tables, every
  table that participates in any FULL JOIN.  The optimizer must keep
  those tables adjacent in the join order because the FULL JOIN
  null-complement algorithm requires it.

  A FULL JOIN table can be an actual table or a nested join.  We
  recognize either by the JOIN_TYPE_FULL flag, then OR in all of the
  tables it covers.

  A nested join under that flag also becomes a group of its own, so that
  its tables stay together rather than merely staying inside the wider
  span.  Materializing such a nest needs it to occupy one span, and the
  wider span alone permits the two operands of a FULL JOIN to interleave.
*/

static void
collect_full_join_tables(JOIN *join, List<TABLE_LIST> *lst)
{
  TABLE_LIST *tl= nullptr;
  List_iterator<TABLE_LIST> it(*lst);

  while ((tl= it++))
  {
    if (tl->outer_join & JOIN_TYPE_FULL)
    {
      if (tl->nested_join)
      {
        join->full_join_nest_tables|= tl->nested_join->used_tables;
        add_full_join_group(join, tl->nested_join->used_tables);
      }
      else if (tl->table)
        join->full_join_nest_tables|= tl->table->map;
    }

    if (tl->nested_join)
      collect_full_join_tables(join, &tl->nested_join->join_list);
  }
}


bool
compute_full_join_nest_tables(JOIN *join, SELECT_LEX *lex)
{
  join->full_join_nest_tables= 0;
  join->full_join_groups= NULL;
  join->full_join_group_count= 0;
  if (!join->thd->lex->full_join_count)
    return false;

  /*
    A group per join nest that is an operand of a FULL JOIN, of which
    there are fewer than there are tables, and one more for all the FULL
    JOIN tables together.
  */
  if (!(join->full_join_groups=
        join->thd->alloc<table_map>(join->table_count + 1)))
    return true;

  collect_full_join_tables(join, &lex->top_join_list);

  if (join->full_join_nest_tables)
    add_full_join_group(join, join->full_join_nest_tables);

  return false;
}


/*
  Keep each set of tables recorded in full_join_groups on an unbroken
  span of the join order.  Once a table of a set has been placed, every
  subsequent table must come from that same set until all of it is
  placed.  Other tables may appear before or after such a span.

  The sets nest, so a table can be inside more than one of them at once,
  and the one with the least left to place is the one that decides.

  Returns the set of tables allowed next, otherwise 0 (no restriction).
*/

table_map
restrict_to_unplaced_fj_tables(JOIN *join, uint idx, table_map pool)
{
  // Nothing to place.
  if (!join->full_join_group_count)
    return 0;

  /*
    Const tables come first in the join order, skip those as there
    cannot be FULL JOIN tables that are constant (const table
    optimization for FULL JOIN tables disabled).
   */
  table_map placed= 0;
  for (uint i= join->const_tables; i < idx; i++)
    placed|= join->positions[i].table->table->map;

  /*
    A set that has been entered and not finished allows only what it has
    left.  A set that has not been entered, or that is already complete,
    says nothing about what comes next.
  */
  table_map allowed= ~(table_map) 0;
  bool restricted= false;
  for (uint i= 0; i < join->full_join_group_count; i++)
  {
    table_map group= join->full_join_groups[i];
    table_map remaining_in_group= group & ~placed;

    if (!(group & placed) || !remaining_in_group)
      continue;

    allowed&= remaining_in_group;
    restricted= true;
  }

  if (!restricted)
    return 0;

  // Inside a span, only the tables that span still owes are allowed.
  table_map remaining= 0;
  for (uint i= idx; i < join->table_count; i++)
    remaining|= join->best_ref[i]->table->map;

  return allowed & remaining & pool;
}


/*
  Helper function called by find_left_most_join_tab exclusively,
  see that function's block comment for context before reading
  this function.

  Test whether the TABLE_LIST dart is the same as target or
  appears anywhere underneath it.
*/
static bool table_on_full_join_left_side(TABLE_LIST *target,
                                         TABLE_LIST *dart)
{
  // We found it.
  if (target == dart)
    return true;

  /*
    If we didn't find it and target isn't a nested join, then
    whatever candidate we last tested has to be it (caller saved
    the last candidate).
  */
  if (!target->nested_join)
    return false;

  /*
    Walk the join nest looking for the table that will correspond
    to the left-most JOIN_TAB in the join order.
   */
  List_iterator<TABLE_LIST> li(target->nested_join->join_list);
  TABLE_LIST *child;
  while ((child= li++))
  {
    // Obviously we need to recurse on the tables in the join nest.
    if (table_on_full_join_left_side(child, dart))
      return true;  // found it
  }

  // Ultimately didn't find it.
  return false;
}


/*
  Locate the left-most JOIN_TAB corresponding to the given right_tab.
  Because full_join_nest_tables forces all tables of a FULL JOIN nest
  to be placed contiguously, the FULL JOIN's left side tables are in a
  contiguous range immediately to the left of right_tab.  Walk
  backward from right_tab-1, collecting tabs in the left side, but
  stopping at the first tab outside it.  The last collected tab is the
  left-most JOIN_TAB.

  A left side of a single base table has no run of its own, so its own
  JOIN_TAB is the answer directly.  A left side of two or more tables
  always gets a run of its own (see open_full_join_nest_run(), called
  from get_best_combination()), whose placeholder JOIN_TAB is recorded
  on the nest as materialized_full_join_tab, so that is the answer for
  that case, again directly.

  The one case a run does not cover is a nest that table elimination
  shrank to a single surviving table, which leaves record_full_join_nest_span()
  declining it a run of its own (see its count < 2 check) since a run
  holding one table is that table.  There, fall back to walking
  backward from right_tab, because the surviving table can be
  anywhere in what is left of the nest's original span.
*/
static JOIN_TAB *find_left_most_join_tab(JOIN *join, JOIN_TAB *right_tab)
{
  DBUG_ASSERT(right_tab->tab_list->outer_join &
              (JOIN_TYPE_FULL|JOIN_TYPE_RIGHT));

  TABLE_LIST *left_side= right_tab->tab_list->foj_partner;
  DBUG_ASSERT(left_side);

  if (!left_side->nested_join)
    return left_side->table->reginfo.join_tab;

  if (left_side->is_materialized_full_join())
    return left_side->nested_join->materialized_full_join_tab;

  /*
    right_tab lives in either join->join_tab or, when the FULL JOIN is
    inside a materialized semijoin, in the bush's JOIN_TAB_RANGE.
  */
  JOIN_TAB *join_tab= nullptr;
  int stopping_point= 0;
  if (right_tab->bush_root_tab)
  {
    // Setup walk from right_tab back to the start of the bush children.
    join_tab= right_tab->bush_root_tab->bush_children.start;
    stopping_point= 0;
  }
  else
  {
    // Setup walk from right_tab back to the start of the joined tables.
    join_tab= join->join_tab;
    stopping_point= static_cast<int>(join->const_tables);
  }
  const int starting_point= static_cast<int>(right_tab - join_tab);
  DBUG_ASSERT(starting_point >= 0);
  JOIN_TAB *leftmost_jt= nullptr;

  /*
    Each JOIN_TAB preceding right_tab is a candidate left-most
    JOIN_TAB, so walk them starting from the first JOIN_TAB to the
    left of right_tab and going backwards.
  */
  for (int i= starting_point - 1; i >= stopping_point; --i)
  {
    /*
      tab_list isn't a list, it's just the TABLE_LIST associated with
      the i'th JOIN_TAB.  Check to see if it is in the left side of
      the FULL JOIN which would mean that we (might) have found the
      left-most JOIN_TAB for the current FULL JOIN (but we will keep
      looking until we're sure).  This will return false when we've
      walked past the left-most JOIN_TAB.
    */
    if (!table_on_full_join_left_side(left_side, join_tab[i].tab_list))
      break;

    /*
      join_tab[i] is the current candidate for left-most, but keep going
      until we exhaust candidates, which happens when we break (above).
    */
    leftmost_jt= &join_tab[i];
  }

  return leftmost_jt;
}


/*
  Allocate a full_join_duplicate_filter for each right side FULL JOIN
  table in the toplevel JOIN_TAB range [start_tab, start_tab+count).

  The filter records right side rowids matched during the LEFT JOIN
  pass so the null-complement rescan can skip them.  Only base tables
  are supported on the right side of a FULL JOIN, but a query may
  contain multiple (possibly nested) FULL JOINs, so each right side
  JOIN_TAB gets its own filter.

  After allocating the filters, link each FULL JOIN right JOIN_TAB
  into its corresponding left-most JOIN_TAB's fj_first_target list.
  Append at the tail so chained FULL JOINs land in inside-out order:
  the inner FULL JOIN's right JOIN_TAB runs its rescan before the
  outer FULL JOIN's right JOIN_TAB, so the inner rescan's forwarded
  rows can update the outer fj_dups filter through the normal forward
  chain before the outer rescan reads it.

  Returns true on allocation failure (error already reported).  Filters
  created before the failure are freed, so a failed call leaves no
  filters allocated.
*/

/*
  Duplicate Row Filter for FULL JOINs.

  During the first (LEFT JOIN) pass of a FULL JOIN, the filter records
  the rowids of right-side rows that were matched.  During the second
  (null-complement) pass, the filter is consulted to skip rows that
  were already emitted, so that only unmatched right-side rows produce
  NULL-complemented output.

  Saved rowids are consulted at the end of each 'outer' JOIN_TAB's
  execution to generate null-complements for the partial join (aka
  join prefix).

  Internally this reuses the semi-join weedout infrastructure
  (SJ_TMP_TABLE).
*/
class full_join_duplicate_filter : public Sql_alloc
{
  // Weedout temp table that stores seen rowids.
  SJ_TMP_TABLE tbl;

public:
  /*
    Allocate and populate the weedout temp table for the right side of
    a FULL JOIN.  Builds an SJ_TMP_TABLE whose record is the right
    table's rowid.  Returns true on error.
  */
  bool init(THD *thd, JOIN_TAB *right_tab)
  {
    DBUG_ASSERT(thd);
    DBUG_ASSERT(right_tab);

    tbl.tmp_table= NULL;
    tbl.is_degenerate= false;
    tbl.have_degenerate_row= false;
    tbl.next_flush_table= nullptr;

    if (!(tbl.tabs= thd->alloc<SJ_TMP_TABLE::TAB>(1)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATAL));
      return true;
    }

    uint jt_rowid_offset= 0;
    uint jt_null_bits= 0;

    tbl.tabs[0].join_tab= right_tab;
    tbl.tabs[0].rowid_offset= jt_rowid_offset;
    jt_rowid_offset+= right_tab->table->file->ref_length;
    if (right_tab->table->maybe_null)
    {
      tbl.tabs[0].null_byte= jt_null_bits / 8;
      tbl.tabs[0].null_bit= jt_null_bits++;
    }

    tbl.tabs_end= tbl.tabs + 1;
    tbl.rowid_len= jt_rowid_offset;
    tbl.null_bits= jt_null_bits;
    tbl.null_bytes= (jt_null_bits + 7) / 8;

    right_tab->table->prepare_for_position();
    right_tab->keep_current_rowid= TRUE;

    if (tbl.create_sj_weedout_tmp_table(thd))
      return true;
    return false;
  }

  /*
    Record the current right-side rowid during the first (LEFT JOIN)
    pass.  Duplicate-key errors are silently ignored because, during
    the first pass, we only need to remember that the rowid was seen at
    least once.  Returns 0 on success, 1 on error.
  */
  int remember_rowids(THD *thd)
  {
    DBUG_ASSERT(thd);
    int res= tbl.sj_weedout_check_row(thd);
    if (res == -1)
      return 1;
    return 0;
  }

  /*
    Check whether the current right-side rowid was already emitted.
    Called during the second (null-complement) pass: if the rowid is
    already in the temp table, sets *is_duplicate so the caller can
    skip emitting a NULL-complemented row for a right-side row that
    was already matched.  Returns 0 on success, 1 on error.
  */
  int check_rowids(THD *thd, bool *is_duplicate)
  {
    DBUG_ASSERT(thd);
    DBUG_ASSERT(is_duplicate);
    int res= tbl.sj_weedout_check_row(thd);
    if (res == -1)
      return 1;
    *is_duplicate= (res == 1);
    return 0;
  }

  /*
    Delete all recorded rows but keep the temp table allocated
    so it can be reused.
  */
  void reset()
  {
    tbl.sj_weedout_delete_rows();
  }

  /*
    Delete all recorded rows and free the weedout temp table.  Must
    be called after FULL JOIN execution is complete.
  */
  void cleanup(THD *thd)
  {
    tbl.sj_weedout_delete_rows();
    if (tbl.tmp_table)
    {
      tbl.tmp_table->file->ha_index_or_rnd_end();
      free_tmp_table(thd, tbl.tmp_table);
      tbl.tmp_table= NULL;
    }
  }
};


bool alloc_full_join_duplicate_filters(JOIN *join, JOIN_TAB *start_tab,
                                              uint count)
{
  // No FULL JOINs in this query, do nothing.
  if (!join->thd->lex->full_join_count)
    return false;

  // First, initialize all pointers to NULL...
  for (uint i= 0; i < count; ++i)
  {
    start_tab[i].fj_dups= nullptr;
    start_tab[i].fj_first_target= nullptr;
    start_tab[i].fj_next_target= nullptr;
  }

  // ...then, setup the duplicate filters.
  for (uint i= 0; i < count; ++i)
  {
    /*
      Descend into a particular bush_child (most likely a materialized
      semijoin) so its FULL JOIN tables get their own fj_dups filters
      (well, so at least the right sides of any FULL JOINs get them, see
      down below).
    */
    if (start_tab[i].has_bush_children())
    {
      JOIN_TAB *bush_start= start_tab[i].bush_children.start;
      uint bush_count= (uint)(start_tab[i].bush_children.end - bush_start);
      if (alloc_full_join_duplicate_filters(join, bush_start, bush_count))
      {
        free_full_join_duplicate_filters(join, start_tab, count);
        return true;
      }
    }

    /*
      Right side of FULL JOINs only beyond this point.  All the
      bookkeeping stuff goes on the right side of the FULL JOIN.
    */
    if (!(start_tab[i].tab_list->outer_join & JOIN_TYPE_FULL) ||
        !(start_tab[i].tab_list->outer_join & JOIN_TYPE_RIGHT))
      continue;

    /*
      If we're allocating a filter, then it's for a FULL JOIN and there
      must be at least two tables in the JOIN.
    */
    DBUG_ASSERT(count >= 2);
    full_join_duplicate_filter *fj_dups= new full_join_duplicate_filter;
    if (!fj_dups || fj_dups->init(join->thd, &start_tab[i]))
    {
      free_full_join_duplicate_filters(join, start_tab, count);
      return true;
    }
    start_tab[i].fj_dups= fj_dups;

    /*
      Link this JOIN_TAB (which must be on the right side of a FULL
      JOIN) into the target list of the corresponding left-most
      JOIN_TAB.  The rescan that emits null-complement rows from the
      right side of this FULL JOIN will fire at the end of that left
      JOIN_TAB's sub_select call.

      Append at the tail of the list rather than at the head.
      The enclosing loop walks JOIN_TABs in order, so for a
      chained FULL JOIN like (A FJ B) FJ C the inner JOIN_TAB B lands
      on A's list before the C.  Order
      matters because the inner rescan's emitted rows must reach
      the R's fj_dups filter through next_select before
      the rescan reads that filter.  If we prepended, the
      outer rescan would run first and emit already matched
      right side rows again as unmatched.
    */
    JOIN_TAB *leftmost_jt= find_left_most_join_tab(join, &start_tab[i]);
    if (!leftmost_jt)
      leftmost_jt= &start_tab[i];
    DBUG_ASSERT(leftmost_jt);
    JOIN_TAB **slot= &leftmost_jt->fj_first_target;
    while (*slot) // walk to the end of the linked list...
      slot= &(*slot)->fj_next_target;
    *slot= &start_tab[i]; // ...and stick start_tab[i] at the end.
  }
  return false;
}


/*
  Release the temp tables backing each FULL JOIN duplicate filter
  allocated by alloc_full_join_duplicate_filters.
*/

void free_full_join_duplicate_filters(JOIN *join, JOIN_TAB *start_tab,
                                             uint count)
{
  if (!join->thd->lex->full_join_count)
    return;

  for (uint i= 0; i < count; ++i)
  {
    /*
      Mirror alloc's descent into a materialized semijoin so filters
      set up inside the bush are released, too.
    */
    if (start_tab[i].has_bush_children())
    {
      JOIN_TAB *bush_start= start_tab[i].bush_children.start;
      uint bush_count= (uint)(start_tab[i].bush_children.end - bush_start);
      free_full_join_duplicate_filters(join, bush_start, bush_count);
    }

    if (!(start_tab[i].tab_list->outer_join & JOIN_TYPE_FULL) ||
        start_tab[i].fj_dups == nullptr)
      continue;
    start_tab[i].fj_dups->cleanup(join->thd);
    start_tab[i].fj_dups= nullptr;
  }
}


/*
  Rescan the right table of a FULL JOIN to emit null-complemented
  rows for the right-side rows that were not matched during the first
  (LEFT JOIN) pass.

  The rescan is forced to a plain sequential scan (not the original
  JT_REF / JT_EQ_REF access method, which would look up keys derived
  from the now-nullified left side and return zero rows).  The pushed
  SQL_SELECT, the on_precond and the pushed rowid filter are cleared
  for the rescan and restored afterwards so that subsequent executions
  (prepared statement re-execution, correlated subquery iterations) see
  the originals.

  A rowid filter belongs to the ref access this rescan replaces.  It
  holds the primary keys the ref lookup would have been allowed to
  return, so a sequential scan has no use for it and an engine is
  entitled to assume it will never see one outside an index read.
*/

static enum_nested_loop_state
run_fj_null_complement_pass(JOIN *join, JOIN_TAB *join_tab)
{
  join_tab->writing_null_complements= true;
  Item *saved_on_precond= join_tab->on_precond;
  join_tab->on_precond= nullptr;

  /*
    Restart reading from the right table as a full scan.  The keyread
    state must be saved and restored because a correlated subquery
    will expect the keyread to be active during a later read.
  */
  const int saved_keyread= join_tab->table->file->ha_end_active_keyread();
  if (join_tab->type == JT_FT)
    join_tab->table->file->ha_ft_end();
  else
    join_tab->table->file->ha_index_or_rnd_end();

  // Save-off important state before restarting the full scan.
  READ_RECORD saved_read_record= join_tab->read_record;
  READ_RECORD::Setup_func saved_read_first= join_tab->read_first_record;
  SQL_SELECT *saved_select= join_tab->select;
  join_tab->read_first_record= join_init_read_record;
  join_tab->select= nullptr;
  if (join_tab->rowid_filter)
    join_tab->table->file->disable_pushed_rowid_filter();

  // full scan of right table and null-complement generation
  enum_nested_loop_state nls= sub_select(join, join_tab, 0);

  // restore the saved-off state.
  join_tab->read_first_record= saved_read_first;
  join_tab->read_record= saved_read_record;
  join_tab->select= saved_select;
  join_tab->writing_null_complements= false;
  join_tab->on_precond= saved_on_precond;
  /*
    join_init_read_record (via join_tab->read_first_record above)
    started an RND scan and we must end it before restoring the keyread
    state that we saved near the start of this function.

    TODO: probably this should be a scope_exit in case we ever have to
    return early, before getting to this point.
  */
  if (join_tab->table->file->inited)
    join_tab->table->file->ha_index_or_rnd_end();
  join_tab->table->file->ha_restart_keyread(saved_keyread);
  /*
    Put the rowid filter back only once the sequential scan is closed,
    so that no read that must not use it can still be in progress.
  */
  if (join_tab->rowid_filter)
    join_tab->table->file->enable_pushed_rowid_filter();

  if (nls == NESTED_LOOP_NO_MORE_ROWS)
    nls= NESTED_LOOP_OK;
  return nls;
}


void
reset_fj_duplicate_filters(JOIN_TAB *join_tab)
{
  /*
    If this tab is the left-most JOIN_TAB of a FULL JOIN right
    side JOIN_TAB, then reset its duplicate filters so that
    each fresh iteration of this tab accumulates a clean set of
    matched right side rowids.  The matching null-complement rescans
    fire at the end of sub_select for this JOIN_TAB, below.

    Skip when this tab is itself in the middle of a null-complement
    rescan (i.e., writing_null_complements == true).  That path is
    entered from run_fj_null_complement_pass and is not a fresh
    outer scope iteration; resetting fj_dups here would wipe out
    the matches of that outer scan.
  */
  if (join_tab->writing_null_complements)
    return;
  for (JOIN_TAB *target= join_tab->fj_first_target;
       target;
       target= target->fj_next_target)
    target->fj_dups->reset();
}


enum_nested_loop_state
run_fj_null_complement_passes(JOIN *join, JOIN_TAB *join_tab)
{
  /*
    At the end of this JOIN_TAB's scan, run the FULL JOIN null-
    complement rescan for each right side tab whose left-most JOIN_TAB
    is this tab.
  */
  enum_nested_loop_state rc= NESTED_LOOP_OK;
  if (!join_tab->writing_null_complements)
  {
    for (JOIN_TAB *target= join_tab->fj_first_target;
         target;
         target= target->fj_next_target)
    {
      rc= run_fj_null_complement_pass(join, target);
      if (rc != NESTED_LOOP_OK)
        break;
    }
  }
  return rc;
}


/*
  Recursively mark all base tables within a TABLE_LIST as null rows.
  A plain table carries table alone and a nest carries nested_join
  alone, but a merged derived table or view carries both, its own
  placeholder and the tables it was merged into.  The query reads the
  fields of those tables, so the walk cannot stop at the placeholder.
*/
void mark_table_list_as_null_row(TABLE_LIST *tl)
{
  if (tl->table)
    mark_as_null_row(tl->table);
  if (tl->nested_join)
  {
    List_iterator<TABLE_LIST> li(tl->nested_join->join_list);
    TABLE_LIST *child;
    while ((child= li++))
      mark_table_list_as_null_row(child);
  }
}


/* Reverse of mark_table_list_as_null_row: restore real row data. */
static void unmark_table_list_as_null_row(TABLE_LIST *tl)
{
  if (tl->table)
    unmark_as_null_row(tl->table);
  if (tl->nested_join)
  {
    List_iterator<TABLE_LIST> li(tl->nested_join->join_list);
    TABLE_LIST *child;
    while ((child= li++))
      unmark_table_list_as_null_row(child);
  }
}


/*
  Handle a single row read during a FULL JOIN null-complement rescan.

  Called from evaluate_join_record when writing_null_complements is
  set and the current table has an fj_dups filter (i.e. it is the
  right side of a FULL JOIN).  The steps are:

    1. Skip rows whose rowid was already recorded during the first
       (LEFT JOIN) pass.
    2. Null-complement the FULL JOIN partner side.
    3. Apply WHERE (only) to the null-complemented row.  select_cond
       has the structure
         trigcond(found, WHERE) AND trigcond(not_null_compl, ON)
       so setting found=1 activates WHERE while not_null_compl=0
       disables ON (which returns TRUE when its trigcond is off).
       Restore both flags after evaluation.
    4. Forward the row through the remaining join tabs.
    5. Unmark the partner side before returning.
*/

enum_nested_loop_state
evaluate_fj_null_complement_row(JOIN *join, JOIN_TAB *join_tab,
                                COND *select_cond)
{
  bool is_dup= false;
  if (join_tab->fj_dups->check_rowids(join->thd, &is_dup))
    return NESTED_LOOP_ERROR;
  if (is_dup)
    return NESTED_LOOP_OK;

  mark_table_list_as_null_row(join_tab->tab_list->foj_partner);

  if (select_cond)
  {
    bool saved_found= join_tab->found;
    bool saved_nnc= join_tab->not_null_compl;
    join_tab->found= 1;
    join_tab->not_null_compl= 0;
    bool where_ok= select_cond->val_bool();
    join_tab->found= saved_found;
    join_tab->not_null_compl= saved_nnc;
    if (!where_ok)
    {
      unmark_table_list_as_null_row(join_tab->tab_list->foj_partner);
      return NESTED_LOOP_OK;
    }
  }

  /*
    This row is a row of every outer join whose inner scope ends at this
    table, so those scopes have a match and must not be null complemented.
    The main loop does the same bookkeeping in evaluate_join_record when it
    walks first_upper after a match.
  */
  for (JOIN_TAB *upper= join_tab->first_upper;
       upper && upper->last_inner == join_tab;
       upper= upper->first_upper)
    upper->found= 1;

  enum_nested_loop_state rc=
    (*join_tab->next_select)(join, join_tab+1, false);
  join->thd->get_stmt_da()->inc_current_row_for_warning();

  unmark_table_list_as_null_row(join_tab->tab_list->foj_partner);

  if (rc != NESTED_LOOP_OK && rc != NESTED_LOOP_NO_MORE_ROWS)
    return rc;
  return NESTED_LOOP_OK;
}


/*
  Record the rowid of a FULL JOIN right side row whose match condition
  holds, while the found-match guard of that FULL JOIN is already open.

  The condition attached to the right side tab holds the match condition
  together with predicates that are no part of it, the WHERE predicates
  over the join's result and any condition deferred here from an
  enclosing outer join.  Those are wrapped in this outer join's own
  found-match guard, which is closed until the first match of the current
  left side row.  While it is closed the condition evaluates the match
  alone, which is why the caller records the rowid whatever the WHERE
  then makes of the row.  Once the guard is open, one of the wrapped
  predicates can reject a later row of the same left side row before its
  match is recorded, and the null complement pass would emit that row as
  a row that matched nothing.

  Closing the guard again gives the residual match condition.  A part of
  the match that a ref or range access already applied is not in the
  condition, and the ON expression carries no guard for this join's own
  scope, so what the closed guard leaves is exactly what decides the
  match.

  Returns true on error.
*/

/*
  Remember the current right-side rowid of a FULL JOIN so the null
  complement pass skips it.  A thin wrapper so callers outside this file
  do not need full_join_duplicate_filter to be a complete type.

  Returns true on error.
*/

bool remember_full_join_right_rowid(JOIN_TAB *join_tab, THD *thd)
{
  return join_tab->fj_dups->remember_rowids(thd);
}


bool record_full_join_right_match(JOIN *join, JOIN_TAB *join_tab,
                                         COND *select_cond)
{
  JOIN_TAB *fj_inner= join_tab->first_inner;
  bool matched;

  fj_inner->found= 0;
  matched= !select_cond || MY_TEST(select_cond->val_bool());
  fj_inner->found= 1;

  if (unlikely(join->thd->is_error()))
    return true;
  if (matched && join_tab->fj_dups->remember_rowids(join->thd))
    return true;
  return false;
}


/*
  Read a row of a materialized FULL JOIN nest and leave the nest's tables
  in the state a read of those tables would have left them in.

  Copying the columns back restores the values and, where a column of the
  nest cannot hold a NULL, whether the row was null complemented.  A
  table carries one more piece of row state, TABLE::status, which says
  whether it holds a usable row at all.  The tables of the nest are never
  read by the enclosing join, so their status still describes whatever
  read last touched them, and code that consults it treats the row as
  absent.  Item_equal::val_bool, for one, then skips the field and
  reports the equality as satisfied.

  The status of a table that holds a row is zero, or STATUS_NULL_ROW when
  the row is a null complemented one, which is the pairing
  mark_as_null_row and unmark_as_null_row maintain.

  A table of the nest that the query reads no column of has no column to
  copy and keeps its old status.  No condition can name such a table, so
  nothing reads the state that is left stale.
*/

int read_record_func_for_full_join_nest(READ_RECORD *info)
{
  int error;

  if ((error= read_record_func_for_rr_and_unpack(info)))
    return error;

  for (Copy_field *cp= info->copy_field; cp != info->copy_field_end; cp++)
  {
    TABLE *table= cp->to_field->table;
    table->status= table->null_row ? STATUS_NULL_ROW : 0;
  }

  return error;
}


/*
  Return true if tl is the left side of a surviving FULL JOIN or lies
  inside the nest that is one.  A rewritten FULL JOIN has JOIN_TYPE_FULL
  cleared on both sides, so the flag is set only while the join still
  runs as a FULL JOIN.
*/

bool is_in_full_join_left_side(TABLE_LIST *tl)
{
  for ( ; tl ; tl= tl->embedding)
  {
    if ((tl->outer_join & (JOIN_TYPE_FULL | JOIN_TYPE_LEFT)) ==
        (JOIN_TYPE_FULL | JOIN_TYPE_LEFT))
      return true;
  }
  return false;
}


/*
  Return true when this view/derived table contains a FULL JOIN.
*/
static bool join_list_contains_full_join(List<TABLE_LIST> *join_list)
{
  List_iterator<TABLE_LIST> it(*join_list);
  TABLE_LIST *tbl;

  while ((tbl= it++))
  {
    if (tbl->outer_join & JOIN_TYPE_FULL)
      return true;
    if (tbl->nested_join &&
        join_list_contains_full_join(&tbl->nested_join->join_list))
      return true;
  }

  return false;
}


bool TABLE_LIST::contains_full_join() const
{
  List<TABLE_LIST> *join_list= nullptr;

  if (view)
    join_list= &view->first_select_lex()->top_join_list;
  else if (derived)
    join_list= &derived->first_select()->top_join_list;
  else if (nested_join)
    join_list= &nested_join->join_list;
  else
    return false;

  return join_list_contains_full_join(join_list);
}


static inline bool is_coalesce_item(Item *item)
{
  return item->type() == Item::FUNC_ITEM &&
         ((Item_func*) item)->functype() == Item_func::COALESCE_FUNC;
}


/*
  Append 'item' to 'list', flattening it when it is itself a COALESCE so
  COALESCE(a, b) contributes its arguments a, b rather than a nested
  COALESCE.  'item' is only read, never mutated--it may be shared by an
  already-built equi-join condition (see natural_join_eq_operand).

  @return TRUE on out-of-memory, FALSE on success.
*/

static bool append_coalesce_or_item(THD *thd, List<Item> *list, Item *item)
{
  if (is_coalesce_item(item))
  {
    Item_func *fc= (Item_func*) item;
    for (Item **a= fc->arguments(), **end= a + fc->argument_count();
         a < end; a++)
    {
      if (list->push_back(*a, thd->mem_root))
        return TRUE;
    }
    return FALSE;
  }
  return list->push_back(item, thd->mem_root);
}


/*
  Build COALESCE(first, second).  When either operand is already a
  COALESCE, return a single flattened COALESCE over all of the operands
  instead of nesting one COALESCE inside another, so that a chain such as
    (t1 natural full join t2) natural full join t3
  yields COALESCE(t1.x, t2.x, t3.x) rather than
  COALESCE(COALESCE(t1.x, t2.x), t3.x).  Neither operand is mutated, since
  an operand may be shared by an equi-join condition built earlier.

  @return the COALESCE item, or NULL on out-of-memory.
*/

static Item_func_coalesce *coalesce_items(THD *thd, Item *first, Item *second)
{
  if (!is_coalesce_item(first) && !is_coalesce_item(second))
    return new (thd->mem_root) Item_func_coalesce(thd, first, second);

  List<Item> list;
  if (append_coalesce_or_item(thd, &list, first) ||
      append_coalesce_or_item(thd, &list, second))
    return NULL;
  return new (thd->mem_root) Item_func_coalesce(thd, list);
}


/*
  For some pair of tables (t1, t2) such that
    t1 NATURAL FULL JOIN t2
  generate a set of output columns
    COALESCE(t1.x_1, t2.y_1), ..., COALESCE(t1.x_n, t2.y_n)
  such that NULL results won't appear in the NATURAL FULL JOIN.

  @param thd                 the current thread
  @param left_tab_col        common columns originating in t1
  @param right_tab_col       common columns originating in t2
  @return TRUE on out-of-memory, FALSE on success.
*/

int coalesce_natural_full_join(THD *thd,
                                      List<Natural_join_column> *left_tab_col,
                                      List<Natural_join_column> *right_tab_col)
{
  /*
    It's a NATURAL JOIN so the number of columns from the left table better
    match the number from the right table.
  */
  DBUG_ASSERT(left_tab_col->elements == right_tab_col->elements);

  /*
    Walk the left table and right table columns in lock-step, creating a
    new COALESCE() over each pair of columns.  The calling function relies
    on the state of left_join_columns, so set the COALESCE() item instance
    on members of that list.
   */
  List_iterator<Natural_join_column> left(*left_tab_col);
  List_iterator<Natural_join_column> right(*right_tab_col);
  Natural_join_column *left_col= nullptr;
  Natural_join_column *right_col= nullptr;
  while ((left_col= left++) && (right_col= right++))
  {
    /*
      When an operand is itself a NATURAL FULL JOIN its common column is the
      COALESCE built for that join.  coalesce_items() flattens such operands
      so a chain such as
        (t1 natural full join t2) natural full join t3
      yields
        COALESCE(t1.x, t2.x, t3.x)
      rather than a nested COALESCE(COALESCE(t1.x, t2.x), t3.x).

      TODO (future improvement):
        Create a new function 'coalesce_items()'.  If first item is of
        type Item_func_coalesce() then just add the item to the arg list
        otherwise call Item_func_coalesce(thd, a, b);
    */
    Item *left_field=  left_col->get_item();
    Item *right_field= right_col->get_item();
    Item_func_coalesce *coal= coalesce_items(thd, left_field, right_field);
    if (!coal)
      return TRUE;  // out of memory

    // Makes the field `COALESCE(left, right) AS left`.
    coal->set_name(thd, left_field->name);

    // Save the result into the set of left_join_columns.
    left_col->natural_full_join_field= coal;
  }

  return FALSE;
}
