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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "sql_select.h"
#include "sql_parse.h"                          // check_stack_overrun
#include "opt_plan_refinement.h"


/*
  @brief
    Put the expensive arguments of AND/OR conditions after the cheap ones

  @param thd   Thread handle
  @param cond  Condition to process

  @return
    true   the condition is expensive
    false  the condition is not expensive, or an error occurred (reported
           via thd)

  @details
    Walks down AND, OR and trigger conditions. The arguments of each AND/OR
    are partitioned into cheap and expensive ones, keeping the relative order
    within each group, so that short-circuit evaluation can skip
    the expensive arguments. An AND/OR with a non-deterministic argument
    keeps its order.

    A condition is expensive if it contains a subquery, a stored function
    call, or an item that reports itself as expensive (Item::is_expensive()),
    such as a UDF or MATCH().
*/

bool move_expensive_args_last(THD *thd, Item *cond)
{
  if (!TEST_NEW_MODE_FLAG(thd, NEW_MODE_REORDER_EXPENSIVE_CONDS))
    return false;

  if (cond->type() == Item::FUNC_ITEM &&
      ((Item_func *) cond)->functype() == Item_func::TRIG_COND_FUNC)
    return move_expensive_args_last(thd, ((Item_func *) cond)->arguments()[0]);

  if (!is_cond_and(cond) && !is_cond_or(cond))
  {
    /*
      Not an AND/OR: check whether it is expensive. Stored functions are
      searched for separately, because Item_func_sp::is_expensive() may
      return false for them.
    */
    Item_func::Functype sp_func= Item_func::FUNC_SP;
    return cond->with_subquery() ||
           cond->is_expensive() ||
           cond->walk(&Item::find_function_processor, false, &sp_func);
  }

  /*
    Each nested AND/OR may put up to MAX_ALLOCA_SZ bytes on the stack
    (see allocation of the `expensive` array below)
  */
  if (check_stack_overrun(thd, STACK_MIN_SIZE, NULL))
    return false;

  List<Item> *args= ((Item_cond *) cond)->argument_list();
  List_iterator<Item> list_iter(*args), write_iter(*args);
  Item *item;

  /*
    Keep the order of a list with non-deterministic arguments (RAND(),
    a user variable assigned by the statement and so on), so that they are
    evaluated for the same rows as written. Nested lists are still processed.
  */
  bool keep_order= false;
  while ((item= list_iter++))
  {
    if (item->used_tables() & RAND_TABLE_BIT)
    {
      keep_order= true;
      break;
    }
  }
  list_iter.rewind();
  if (keep_order)
  {
    bool any_expensive= false;
    while ((item= list_iter++))
      any_expensive|= move_expensive_args_last(thd, item);
    return any_expensive;
  }

  Item **expensive= NULL;
  uint n_expensive= 0;
  const size_t expensive_size= sizeof(Item *) * args->elements;
  /*
    `list_iter` reads the arguments. `write_iter` lags behind it and points
    at the slot where the next argument is written.
    `expensive` is allocated only if there are expensive arguments in the list.
  */
  while ((item= list_iter++))
  {
    if (move_expensive_args_last(thd, item))
    {
      if (!expensive &&
          !(expensive= (Item **) my_safe_alloca(expensive_size)))
        return false;
      expensive[n_expensive++]= item;
    }
    else
    {
      write_iter++;
      *write_iter.ref()= item;
    }
  }

  /* Put the expensive arguments into the remaining slots */
  for (uint i= 0; i < n_expensive; i++)
  {
    write_iter++;
    *write_iter.ref()= expensive[i];
  }
  my_safe_afree(expensive, expensive_size);
  return n_expensive != 0;
}
