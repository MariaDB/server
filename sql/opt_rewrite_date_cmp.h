/*
   Copyright (c) 2023, MariaDB

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

#ifndef OPT_REWRITE_DATE_CMP_INCLUDED
#define OPT_REWRITE_DATE_CMP_INCLUDED

class Item_func_eq;
class Item_func_ge;
class Item_func_gt;
class Item_func_le;
class Item_func_lt;
class Item_bool_rowready_func2;

/*
  @brief Class responsible for rewriting datetime comparison condition.
         It rewrites non-sargable conditions into sargable.

  @detail
  The intent of this class is to do equivalent rewrites as follows:

    YEAR(col) <= val  ->  col <= year_end(val)
    YEAR(col) <  val  ->  col <  year_start(val)
    YEAR(col) >= val  ->  col >= year_start(val)
    YEAR(col) >  val  ->  col >  year_end(val)
    YEAR(col) =  val  ->  col >= year_start(val) AND col<=year_end(val)

  Also the same is done for comparisons with DATE(col):

    DATE(col) <= val  ->  col <= day_end(val)

  if col has a DATE type (not DATETIME), then the rewrite becomes:

    DATE(col) <= val  ->  col <= val

  @usage
    Date_cmp_func_rewriter rwr(thd, item_func);
    Item *new_item= rwr.get_rewrite_result();

    Returned new_item points to an item that item_func was rewritten to.
    new_item already has fixed fields (fix_fields() was called).
    If no rewrite happened, new_item points to the initial item_func parameter

  @todo
    Also handle conditions in form "YEAR(date_col) BETWEEN 2014 AND 2017"
    and "YEAR(col) = c1 AND MONTH(col) = c2"
*/
class Date_cmp_func_rewriter
{
public:
  Date_cmp_func_rewriter(THD* thd, Item_func_eq *item_func);

  Date_cmp_func_rewriter(THD* thd, Item_func_ge *item_func);

  Date_cmp_func_rewriter(THD* thd, Item_func_gt *item_func);

  Date_cmp_func_rewriter(THD* thd, Item_func_le *item_func);

  Date_cmp_func_rewriter(THD* thd, Item_func_lt *item_func);

  Item* get_rewrite_result() const { return result; }

  Date_cmp_func_rewriter() = delete;
  Date_cmp_func_rewriter(const Date_cmp_func_rewriter&) = delete;
  Date_cmp_func_rewriter(Date_cmp_func_rewriter&&) = delete;

private:
  bool check_cond_match_and_prepare(Item_bool_rowready_func2 *item_func);
  Item_field *is_date_rounded_field(Item* item,
                                    const Type_handler *comparison_type,
                                    Item_func::Functype *out_func_type) const;
  void rewrite_le_gt_lt_ge();
  Item *create_bound(uint month, uint day, const TimeOfDay6 &td) const;
  Item *create_start_bound() const
  {
    return create_bound(1, 1, TimeOfDay6());
  }
  Item *create_end_bound() const
  {
    return create_bound(12, 31, TimeOfDay6::end_of_day(field_ref->decimals));
  }
  Item *create_cmp_func(Item_func::Functype func_type, Item *arg1, Item *arg2);

  THD *thd= nullptr;
  Item *const_arg_value= nullptr;
  Item_func::Functype rewrite_func_type= Item_func::UNKNOWN_FUNC;
  Item_func::Functype argument_func_type= Item_func::UNKNOWN_FUNC;
  Item_field *field_ref= nullptr;
  Item *result= nullptr;
};


void trace_date_item_rewrite(THD *thd,Item *new_item, Item *old_item);

template<typename T>
Item* do_date_conds_transformation(THD *thd, T *item)
{
  Date_cmp_func_rewriter rwr(thd, item);
  /* If the rewrite failed for some reason, we get the original item */
  Item *new_item= rwr.get_rewrite_result();
  trace_date_item_rewrite(thd, new_item, item);
  return new_item;
}


#endif
