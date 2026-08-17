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

#ifndef OPT_JOIN_RUNS_INCLUDED
#define OPT_JOIN_RUNS_INCLUDED

/*
  A run is a stretch of consecutive positions of the picked join order
  whose tables get a JOIN_TAB array of their own.  One entry of the
  enclosing array, the placeholder, stands for all of them.  A
  materialized semi join gets a run, and so does a FULL JOIN operand nest
  of two tables or more.  Two runs are disjoint or one holds the other.
*/

struct Join_run
{
  /* JOIN_TAB_RANGE_SJM or JOIN_TAB_RANGE_FULL_JOIN. */
  Join_tab_range_type kind;
  /* Index into JOIN::best_positions of the run's first table. */
  uint first;
  /* How many positions the run covers. */
  uint count;
  /* The FULL JOIN operand nest, for a run of kind JOIN_TAB_RANGE_FULL_JOIN. */
  TABLE_LIST *nest;
  /* The semi join's materialization, for a run of kind JOIN_TAB_RANGE_SJM. */
  SJ_MATERIALIZATION_INFO *sjm;

  uint last() const { return first + count - 1; }
};


/*
  The runs of the picked join order, in the order their JOIN_TAB arrays
  are opened.  That is by first position, and among the runs that begin
  at the same position, by the number of positions covered, most first.
  A run that holds another has to be opened first, because the inner
  run's placeholder is an entry of the outer run's array.

  A semi join run is opened before a FULL JOIN run that begins at the
  same position and covers as many positions, which places the FULL JOIN
  run inside the semi join.  FULL JOIN runs covering the same positions
  are opened in the order of the join tree, outer nest first.
*/

class Join_run_layout
{
  Join_run *runs;
  uint run_count;
  uint max_runs;
  /* Entries of the top level JOIN_TAB array, one per run or table. */
  uint top_level_count;

public:
  bool init(JOIN *join);
  void add(Join_tab_range_type kind, uint first, uint count,
           TABLE_LIST *nest, SJ_MATERIALIZATION_INFO *sjm);
  const Join_run *begin() const { return runs; }
  const Join_run *end() const { return runs + run_count; }
  uint top_level_entries() const { return top_level_count; }
};


/*
  The runs opened and not yet finished while the JOIN_TAB arrays are
  filled in, outermost first.
*/

class Join_run_stack
{
  JOIN_TAB *placeholder[MAX_TABLES];
  uint last[MAX_TABLES];
  uint depth;

public:
  Join_run_stack(): depth(0) {}
  /* The placeholder of the innermost open run, NULL outside every run. */
  JOIN_TAB *innermost() const
  {
    return depth ? placeholder[depth - 1] : NULL;
  }
  bool is_empty() const { return !depth; }
  bool open(THD *thd, JOIN *join, const Join_run *run, JOIN_TAB *&j);
  JOIN_TAB *finish_ended(uint pos, JOIN_TAB *j);
};


/*
  A walk over the JOIN_TABs of a plan in the order of
  first_depth_first_tab and next_depth_first_tab, which reaches the
  tables of a run before the placeholder that stands for the run.

  A caller entering a run can set a table map aside.  The map is
  available again at the run's placeholder and is dropped when the walk
  moves past it.  A run that sets a map aside is inside every run whose
  map is still set aside, so the maps form a stack with the innermost
  run on top.
*/

class Join_tab_walk
{
  JOIN *join;
  JOIN_TAB *cur;
  /* The placeholders of the runs that set a map aside, outermost first. */
  JOIN_TAB *placeholder[MAX_TABLES];
  table_map set_aside_map[MAX_TABLES];
  uint depth;

public:
  explicit Join_tab_walk(JOIN *join_arg);
  JOIN_TAB *tab() const { return cur; }
  void next();
  JOIN_TAB *outermost_run(Join_tab_range_type kind) const;
  void set_aside(JOIN_TAB *run, table_map map);
  /*
    True when the current JOIN_TAB is the placeholder of the innermost
    run that set a map aside.
  */
  bool ends_set_aside() const
  {
    return depth && placeholder[depth - 1] == cur;
  }
  /* The map the current JOIN_TAB's run set aside, 0 when it set none. */
  table_map tables_set_aside() const
  {
    return ends_set_aside() ? set_aside_map[depth - 1] : 0;
  }
};

#endif
