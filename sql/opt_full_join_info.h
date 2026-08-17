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

#ifndef OPT_FULL_JOIN_INFO_INCLUDED
#define OPT_FULL_JOIN_INFO_INCLUDED

#include "sql_alloc.h"

class JOIN;
struct TABLE_LIST;
class Item;
typedef class Item COND;
typedef struct st_join_table JOIN_TAB;
class full_join_duplicate_filter;

/*
  A FULL JOIN that simplify_joins did not rewrite.  It runs as a LEFT
  JOIN of its left side over its right side, followed by a pass over the
  right side that emits the rows no left row matched.  A side is a join
  nest when the query parenthesised it and a single table otherwise, and
  each side is the other's foj_partner.

  The sides are set when the JOIN is optimized, and the held conditions
  while it is optimized.  The members after them are set by
  alloc_full_join_duplicate_filters at the start of each execution, and
  dups is released by free_full_join_duplicate_filters at its end.
*/

class Full_join_info : public Sql_alloc
{
public:
  /* The side marked JOIN_TYPE_FULL | JOIN_TYPE_LEFT. */
  TABLE_LIST *left;
  /* The side marked JOIN_TYPE_FULL | JOIN_TYPE_RIGHT. */
  TABLE_LIST *right;

  /*
    The WHERE conjuncts that reference the left side, lifted out of the
    WHERE before access selection so they neither prune nor drive access
    on the left side.  make_join_select attaches them to the right tab
    under the found-match guard.
  */
  COND *left_cond;
  /*
    When the chosen join order computes the right side into a temporary
    table, the WHERE conjuncts that reference nothing but that side,
    taken out of the WHERE once the join order is known.  Left in the
    WHERE they would be placed on a table inside the nest and decide
    which rows the nest holds.  make_join_select attaches them to the tab
    that stands for the whole nest instead.
  */
  COND *nest_where;

  /*
    The tab whose tab_list is right.  It records the rowid of each right
    row that matched, and the null complement pass scans it again.
  */
  JOIN_TAB *rescan_tab;
  /* The rowids of the right rows that matched. */
  full_join_duplicate_filter *dups;
  /*
    The next FULL JOIN whose null complement pass runs when the scan of
    the same tab ends, see JOIN_TAB::fj_passes.
  */
  Full_join_info *next_pass;

  Full_join_info(TABLE_LIST *left_arg, TABLE_LIST *right_arg)
    : left(left_arg), right(right_arg), left_cond(NULL), nest_where(NULL),
      rescan_tab(NULL), dups(NULL), next_pass(NULL) {}
  table_map left_map() const;
  table_map right_map() const;
  JOIN_TAB *right_tab(JOIN *join) const;
  JOIN_TAB *find_left_most_tab(JOIN *join) const;
};

#endif
