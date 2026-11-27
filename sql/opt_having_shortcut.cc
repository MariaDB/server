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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
  Early scan termination for implicit-group queries whose HAVING has a
  conjunct on an aggregate that can no longer become true.
*/

#include "mariadb.h"
#include "sql_select.h"
#include "sql_class.h"
#include "item_sum.h"
#include "sql_lex.h"
#include "opt_trace.h"
#include "my_json_writer.h"


/**
  Split a HAVING conjunct "aggr op const" / "const op aggr" into its parts.

  @param cond      the conjunct
  @param[out] sum  the aggregate
  @param[out] c    the constant
  @param[out] op   the comparison, normalized to have the aggregate on the
                   left

  @retval true   the conjunct has one of the forms above, with a numeric
                 comparison and a non-NULL literal or bound PS parameter
                 constant
  @retval false  the conjunct has another shape
*/

static bool split_aggr_comparison(Item *cond, Item_sum **sum, Item **c,
                                  scalar_comparison_op *op)
{
  if (cond->type() != Item::FUNC_ITEM)
    return false;
  Item_func *func= static_cast<Item_func*>(cond);
  switch (func->functype()) {
  case Item_func::EQ_FUNC:
  case Item_func::LT_FUNC:
  case Item_func::LE_FUNC:
  case Item_func::GT_FUNC:
  case Item_func::GE_FUNC:
    break;
  default:
    return false;
  }
  Item_bool_func2_with_rev *cmp= static_cast<Item_bool_func2_with_rev*>(func);

  /*
    Only numeric comparison is monotonic in the aggregate value. E.g. a
    count compared with a DATE is converted to a date, which is not.
  */
  switch (cmp->compare_type_handler()->cmp_type()) {
  case INT_RESULT:
  case REAL_RESULT:
  case DECIMAL_RESULT:
    break;
  default:
    return false;
  }

  /*
    const_item() only means that the value is constant to the optimizer.
    E.g. @v changed by a stored function in WHERE is "const", but the HAVING
    result would differ if we stop on the first value. A <cache> item is
    filled on its first read, so using it here would move that read to the
    first row. Accept only literals and bound PS parameters
    (basic_const_item()). A NULL constant never makes the
    comparison FALSE, so it cannot stop the scan.
  */
  Item *left= func->arguments()[0]->real_item();
  Item *right= func->arguments()[1]->real_item();
  if (left->type() == Item::SUM_FUNC_ITEM && right->basic_const_item() &&
      !right->is_null())
  {
    *sum= static_cast<Item_sum*>(left);
    *c= right;
    *op= Item_func::functype_to_scalar_comparison_op(func->functype());
  }
  else if (right->type() == Item::SUM_FUNC_ITEM && left->basic_const_item() &&
           !left->is_null())
  {
    *sum= static_cast<Item_sum*>(right);
    *c= left;
    *op= Item_func::functype_to_scalar_comparison_op(cmp->rev_functype());
  }
  else
    return false;
  return true;
}


/**
  Find the AND-parts of HAVING that stay FALSE once they become
  FALSE for a part of the group (see Item_sum::having_false_is_final()).
  end_send_group() checks them after each row and stops the scan once one
  is FALSE (not NULL).

  Only done with implicit grouping, where there is a single group and the
  sums are accumulated directly by end_send_group().

  @return true on error (OOM, or an error already raised), false otherwise
*/

bool JOIN::setup_having_stop_conds()
{
  having_stop_conds.empty();
  /*
    Don't stop early if:
    (1) There is no HAVING,
    (2) Or there is no single implicit group, or the groups are not
        accumulated by end_send_group() (GROUP BY, temporary table,
        ROLLUP, window functions, a procedure),
    (3) Or GROUP BY was optimized away (implicit_grouping is then only set
        artificially, so stay conservative),
    (4) Or HAVING refers to an outer query (is correlated),
    (5) Or there are no sum functions that this JOIN accumulates,
    (6) Or there are no tables to scan, or only constant tables.
  */
  if (!having ||                                                  // (1)
      !implicit_grouping || group_list || need_tmp ||             // (2)
      aggr_tables || rollup.state != ROLLUP::STATE_NONE ||        // (2)
      procedure || select_lex->have_window_funcs() ||             // (2)
      group_optimized_away ||                                     // (3)
      having_is_correlated ||                                     // (4)
      !sum_funcs ||                                               // (5)
      !tables_list || only_const_tables())                        // (6)
    return false;

  /* Add the conjunct to having_stop_conds if it can stop the scan */
  auto add_if_stop_cond= [this](Item *cond) -> bool
  {
    Item_sum *sum;
    Item *c;
    scalar_comparison_op op;
    if (!split_aggr_comparison(cond, &sum, &c, &op) ||
        sum->aggr_sel != select_lex || !sum->having_false_is_final(op, c))
      return false;

    /* The aggregate must be one that this JOIN accumulates itself */
    Item_sum **sp= sum_funcs;
    while (*sp && *sp != sum)
      sp++;
    return *sp && having_stop_conds.push_back(cond, thd->mem_root);
  };

  Item *cond;
  if (having->type() == Item::COND_ITEM &&
      static_cast<Item_cond*>(having)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator_fast<Item> li(*static_cast<Item_cond*>(having)->
                                argument_list());
    while ((cond= li++))
      if (add_if_stop_cond(cond))
        return true;
  }
  else if (add_if_stop_cond(having))
    return true;
  if (thd->is_error())          // e.g. from val_real() of the constant
    return true;

  if (!having_stop_conds.is_empty() && unlikely(thd->trace_started()))
  {
    Json_writer_object trace_wrapper(thd);
    Json_writer_object trace_shortcut(thd, "having_shortcut");
    Json_writer_array trace_conds(thd, "stop_conditions");
    List_iterator_fast<Item> it(having_stop_conds);
    while ((cond= it++))
      trace_conds.add(cond);
  }
  return false;
}
