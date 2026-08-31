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

/*
  FULL JOIN rewrite, planning, and execution support.  Declarations for
  the functions in opt_full_join.cc that are called from outside it.
*/

bool collect_full_join_right_sides(List<TABLE_LIST> *join_list,
                                          List<TABLE_LIST> *right_sides,
                                          MEM_ROOT *mem_root);

COND *lift_full_join_left_where(JOIN *join, COND *conds);

bool full_join_left_side_allows(TABLE_LIST *tl,
                                       table_map used_tables);

bool
open_full_join_nest_run(THD *thd, JOIN *join, JOIN_TAB *&j, uint tablenr,
                         Full_join_nest_span *span,
                         JOIN_TAB **run_root, uint *run_last_pos,
                         uint &run_depth);

bool
setup_full_join_materialization(JOIN_TAB *tab);

void attach_full_join_nest_conds(THD *thd, JOIN *join);

bool hold_full_join_left_cond(THD *thd, JOIN *join,
                                     List<TABLE_LIST> *right_sides,
                                     table_map prefix, COND *cond);

COND *restore_full_join_left_conds(JOIN *join, COND *conds);

bool attach_full_join_left_conds(THD *thd, JOIN *join);

COND *lift_full_join_nest_where(THD *thd, JOIN *join, COND *cond);

bool attach_full_join_nest_where(THD *thd, JOIN *join);

JOIN_TAB *fj_deferral_target(JOIN *join, JOIN_TAB *dest,
                                    JOIN_TAB *first_inner_tab);

bool
setup_full_join_materialization_scan(JOIN_TAB *tab);

bool is_in_full_join_scope(TABLE_LIST *tl);

table_map usable_not_null_tables(COND *conds, TABLE_LIST *fj_operand);

COND *rewrite_full_outer_joins(JOIN *join,
                                      COND *conds,
                                      bool in_sj,
                                      TABLE_LIST *fj_operand,
                                      TABLE_LIST **right_table,
                                      List_iterator<TABLE_LIST> *li,
                                      table_map *used_tables,
                                      table_map *not_null_tables);

void clear_full_join_nest_conds(List<TABLE_LIST> *join_list);

void record_full_join_nest_cond(THD *thd, TABLE_LIST *nest,
                                       Item *on_expr);

bool
compute_full_join_nest_tables(JOIN *join, SELECT_LEX *lex);

table_map
restrict_to_unplaced_fj_tables(JOIN *join, uint idx, table_map pool);

bool alloc_full_join_duplicate_filters(JOIN *join, JOIN_TAB *start_tab,
                                              uint count);

void free_full_join_duplicate_filters(JOIN *join, JOIN_TAB *start_tab,
                                             uint count);

void
reset_fj_duplicate_filters(JOIN_TAB *join_tab);

enum_nested_loop_state
run_fj_null_complement_passes(JOIN *join, JOIN_TAB *join_tab);

void mark_table_list_as_null_row(TABLE_LIST *tl);

enum_nested_loop_state
evaluate_fj_null_complement_row(JOIN *join, JOIN_TAB *join_tab,
                                COND *select_cond);

bool remember_full_join_right_rowid(JOIN_TAB *join_tab, THD *thd);
bool record_full_join_right_match(JOIN *join, JOIN_TAB *join_tab,
                                         COND *select_cond);

int read_record_func_for_full_join_nest(READ_RECORD *info);

bool is_in_full_join_left_side(TABLE_LIST *tl);

int coalesce_natural_full_join(THD *thd,
                                List<Natural_join_column> *left_tab_col,
                                List<Natural_join_column> *right_tab_col);
