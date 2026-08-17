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
    Where the runs of a picked join order begin and end, and the opening
    and finishing of their JOIN_TAB arrays.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "opt_full_join.h"
#include "opt_join_runs.h"

/* Used to build a pointer to a permanently null Item * for on_expr_ref. */
static Item * const null_ptr= NULL;


/*
  The materialized semi join whose run begins at the given position of
  the picked join order, otherwise NULL.
*/

static SJ_MATERIALIZATION_INFO *sjm_run_at(JOIN *join, uint pos)
{
  POSITION *cur_pos= &join->best_positions[pos];

  if (cur_pos->sj_strategy != SJ_OPT_MATERIALIZE &&
      cur_pos->sj_strategy != SJ_OPT_MATERIALIZE_SCAN)
    return NULL;
  return cur_pos->table->emb_sj_nest->sj_mat_info;
}


/*
  Whether run a is opened before run b, in the order described at
  Join_run_layout.
*/

static bool opens_before(const Join_run *a, const Join_run *b)
{
  if (a->first != b->first)
    return a->first < b->first;
  if (a->count != b->count)
    return a->count > b->count;
  return a->kind == JOIN_TAB_RANGE_SJM && b->kind != JOIN_TAB_RANGE_SJM;
}


/*
  Record the runs of the picked join order.  The semi join strategies
  have to be final.

  RETURN
    false  OK
    true   Out of memory
*/

bool Join_run_layout::init(JOIN *join)
{
  const Join_run *top= NULL;

  runs= NULL;
  run_count= 0;
  max_runs= 0;

  /*
    The FULL JOIN operand nests are strictly nested or disjoint and each
    run of one holds at least two tables, so there are fewer of them than
    there are tables.  At most one semi join run begins at a position.
  */
  if (join->full_join_nest_tables)
    max_runs+= join->table_count;
  for (uint i= 0; i < join->table_count; i++)
  {
    if (sjm_run_at(join, i))
      max_runs++;
  }
  if (max_runs && !(runs= join->thd->alloc<Join_run>(max_runs)))
    return true;

  if (join->full_join_nest_tables)
    record_full_join_runs(join, this);
  for (uint i= 0; i < join->table_count; i++)
  {
    SJ_MATERIALIZATION_INFO *sjm= sjm_run_at(join, i);

    if (sjm)
      add(JOIN_TAB_RANGE_SJM, i, sjm->tables, NULL, sjm);
  }

  /*
    A run that begins at a position the previous top level run covers is
    held by it.  Every other run takes one entry of the top level array
    for all the positions it covers.
  */
  top_level_count= join->table_count;
  for (const Join_run *run= begin(); run != end(); run++)
  {
    if (top && run->first <= top->last())
      continue;
    top= run;
    top_level_count-= run->count - 1;
  }
  return false;
}


/*
  Add a run, keeping the runs in the order they are opened.  Runs that
  opens_before() does not order keep the order they were added in.
*/

void Join_run_layout::add(Join_tab_range_type kind, uint first, uint count,
                          TABLE_LIST *nest, SJ_MATERIALIZATION_INFO *sjm)
{
  Join_run run= { kind, first, count, nest, sjm };
  uint i;

  DBUG_ASSERT(run_count < max_runs);
  for (i= run_count; i && opens_before(&run, &runs[i - 1]); i--)
    runs[i]= runs[i - 1];
  runs[i]= run;
  run_count++;
}


/*
  Open a run at entry j of the array being filled in.  j becomes the
  run's placeholder and is then pointed at the first entry of the run's
  own array, so the tables to follow and any run opened among them go
  into it.

  The array has an entry for each position the run covers, which is an
  upper bound because a run inside it takes one entry for two positions
  or more.  The array is narrowed to the entries used when the run
  finishes.

  A semi join placeholder reads the temporary table holding the semi
  join's result, and takes its row count from the materialization.  A
  FULL JOIN placeholder is recorded on its nest.

  RETURN
    false  OK
    true   Out of memory
*/

bool Join_run_stack::open(THD *thd, JOIN *join, const Join_run *run,
                          JOIN_TAB *&j)
{
  JOIN_TAB *children;

  DBUG_ASSERT(!depth || run->last() <= last[depth - 1]);
  if (!(children= thd->alloc<JOIN_TAB>(run->count)))
    return true;
  bzero((void*) j, sizeof(JOIN_TAB));
  j->join= join;
  j->table= NULL;
  j->ref.key= -1;
  j->on_expr_ref= (Item**) &null_ptr;
  j->bush_root_tab= innermost();
  j->bush_children.start= children;
  j->bush_children.end= children + run->count;
  j->bush_children.kind= run->kind;
  j->bush_children.nest= run->nest;

  if (run->kind == JOIN_TAB_RANGE_SJM)
  {
    SJ_MATERIALIZATION_INFO *sjm= run->sjm;

    /* The unique index is always in 'possible keys' in EXPLAIN */
    j->keys= key_map(1);
    j->records_read= (sjm->is_sj_scan ? sjm->rows : 1.0);
    j->records_init= j->records_out= j->records_read;
    j->records= (ha_rows) j->records_read;
    j->cond_selectivity= 1.0;
    j->join_read_time= 0.0; /* Not saved currently */
    j->join_loops= 0.0;
    join->join_tab_ranges.push_back(&j->bush_children, thd->mem_root);
  }
  else
  {
    run->nest->nested_join->materialized_full_join= TRUE;
    run->nest->nested_join->materialized_full_join_tab= j;
    if (join->join_tab_ranges.push_back(&j->bush_children, thd->mem_root))
      return true;
  }

  DBUG_ASSERT(depth < MAX_TABLES);
  placeholder[depth]= j;
  last[depth]= run->last();
  depth++;
  j= children;
  return false;
}


/*
  Finish the runs whose last position is pos, innermost first, once j,
  the entry for pos, is filled in.  j is the last entry of the innermost
  of them, and the placeholder of each finished run is the last entry of
  the run that holds it.

  RETURN
    The entry the next position follows, which is the placeholder of the
    outermost run finished here, or j when no run ends at pos.
*/

JOIN_TAB *Join_run_stack::finish_ended(uint pos, JOIN_TAB *j)
{
  while (depth && last[depth - 1] == pos)
  {
    JOIN_TAB *root= placeholder[--depth];

    j->last_leaf_in_bush= TRUE;
    root->bush_children.end= j + 1;
    j= root;
  }
  return j;
}


Join_tab_walk::Join_tab_walk(JOIN *join_arg)
  : join(join_arg), cur(first_depth_first_tab(join_arg)), depth(0)
{}


void Join_tab_walk::next()
{
  if (ends_set_aside())
    depth--;
  cur= next_depth_first_tab(join, cur);
}


/*
  The placeholder of the outermost run of the given kind that holds the
  current JOIN_TAB, or NULL when no such run holds it.
*/

JOIN_TAB *Join_tab_walk::outermost_run(Join_tab_range_type kind) const
{
  JOIN_TAB *found= NULL;
  for (JOIN_TAB *root= cur->bush_root_tab; root; root= root->bush_root_tab)
  {
    if (root->bush_children.kind == kind)
      found= root;
  }
  return found;
}


/*
  Set map aside until the walk reaches run, the placeholder of a run
  that holds the current JOIN_TAB.
*/

void Join_tab_walk::set_aside(JOIN_TAB *run, table_map map)
{
  DBUG_ASSERT(run);
#ifndef DBUG_OFF
  if (depth)
  {
    JOIN_TAB *root= run->bush_root_tab;
    while (root && root != placeholder[depth - 1])
      root= root->bush_root_tab;
    DBUG_ASSERT(root);
  }
#endif
  DBUG_ASSERT(depth < MAX_TABLES);
  placeholder[depth]= run;
  set_aside_map[depth]= map;
  depth++;
}
