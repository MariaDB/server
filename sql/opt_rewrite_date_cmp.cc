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


/**
  @file

  @brief
    Rewrites that make non-sargable date[time] comparisons sargable.
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "my_json_writer.h"
#include "opt_rewrite_date_cmp.h"

Date_cmp_func_rewriter::Date_cmp_func_rewriter(THD* thd,
                                               Item_func_eq *item_func) :
  thd(thd),
  result(item_func)
{
  if (!check_cond_match_and_prepare(item_func))
    return;

  /*
    This is an equality. Do a rewrite like this:
    "YEAR(col) = val"  ->  col BETWEEN year_start(val) AND year_end(val)
    "DATE(col) = val"  ->  col BETWEEN day_start(val) AND day_end(val)
  */
  Item *start_bound, *end_bound;
  if (!(start_bound= create_start_bound()) || !(end_bound= create_end_bound()))
    return;
  Item *new_cond;
  if (!(new_cond= new (thd->mem_root) Item_func_between(thd, field_ref,
                                                        start_bound, end_bound)))
    return;
  if (!new_cond->fix_fields(thd, &new_cond))
    result= new_cond;
}


Date_cmp_func_rewriter::Date_cmp_func_rewriter(THD* thd,
                                               Item_func_ge *item_func) :
  thd(thd),
  result(item_func)
{
  if (!check_cond_match_and_prepare(item_func))
    return;
  rewrite_le_gt_lt_ge();
}


Date_cmp_func_rewriter::Date_cmp_func_rewriter(THD* thd,
                                               Item_func_lt *item_func) :
  thd(thd),
  result(item_func)
{
  if (!check_cond_match_and_prepare(item_func))
    return;
  rewrite_le_gt_lt_ge();
}


Date_cmp_func_rewriter::Date_cmp_func_rewriter(THD* thd,
                                               Item_func_gt *item_func) :
  thd(thd),
  result(item_func)
{
  if (!check_cond_match_and_prepare(item_func))
    return;
  rewrite_le_gt_lt_ge();
}


Date_cmp_func_rewriter::Date_cmp_func_rewriter(THD* thd,
                                               Item_func_le*item_func) :
  thd(thd),
  result(item_func)
{
  if (!check_cond_match_and_prepare(item_func))
    return;
  rewrite_le_gt_lt_ge();
}


bool Date_cmp_func_rewriter::check_cond_match_and_prepare(
    Item_bool_rowready_func2 *item_func)
{
  if (thd->lex->is_ps_or_view_context_analysis())
  {
    DBUG_ASSERT(0);
    return false;
  }

  Item **args= item_func->arguments();
  rewrite_func_type= item_func->functype();
  bool condition_matches= false;
  const Type_handler *comparison_type= item_func->get_comparator()->
                                       compare_type_handler();

  /*
    Check if this is "YEAR(indexed_col) CMP const_item" or
    "DATE(indexed_col) CMP const_item"
  */
  if ((field_ref= is_date_rounded_field(args[0], comparison_type,
                                        &argument_func_type)) &&
      args[1]->basic_const_item())
  {
    const_arg_value= args[1];
    condition_matches= true;
  }
  else
  /*
    Check if this is "const_item CMP YEAR(indexed_col)" or
    "const_item CMP DATE(indexed_col)"
  */
  if ((field_ref= is_date_rounded_field(args[1], comparison_type,
                                        &argument_func_type)) &&
      args[0]->basic_const_item())
  {
    /*
      Ok, the condition has form like "const<YEAR(col)"/"const<DATE(col)".
      Turn it around to be "YEAR(col)>const"/"DATE(col)>const"
    */
    const_arg_value= args[0];

    rewrite_func_type= item_func->rev_functype();
    condition_matches= true;
  }
  return condition_matches;
}

/*
  Check if the passed item is YEAR(key_col) or DATE(key_col).

  Also
  - key_col must be covered by an index usable by the current query
  - key_col must have a DATE[TIME] or TIMESTAMP type
  - The value of the YEAR(..) or DATE(..) function must be compared using an a
    appropriate comparison_type.

  @param item             IN   Item to check
  @param comparison_type  IN   Which datatype is used to compare the item value
  @param out_func_type    OUT  Function (is it YEAR or DATE)

  @return
     key_col  if the check suceeded
     NULL     otherwise
*/
Item_field *Date_cmp_func_rewriter::is_date_rounded_field(Item* item,
                                  const Type_handler *comparison_type,
                                  Item_func::Functype *out_func_type) const
{
  if (item->type() != Item::FUNC_ITEM)
    return nullptr;

  Item_func::Functype func_type= ((Item_func*)item)->functype();
  bool function_ok= false;
  switch (func_type) {
  case Item_func::YEAR_FUNC:
    // The value of YEAR(x) must be compared as integer
    if (comparison_type == &type_handler_slonglong)
      function_ok= true;
    break;
  case Item_func::DATE_FUNC:
    // The value of DATE(x) must be compared as dates.
    if (comparison_type == &type_handler_newdate)
      function_ok= true;
    break;
  default:
    ;// do nothing
  }

  if (function_ok)
  {
    Item* arg= ((Item_func*)item)->arguments()[0];
    // Check if the argument is a column that's covered by some index
    if (arg->real_item()->type() == Item::FIELD_ITEM)
    {
      Item_field *item_field= (Item_field*)(arg->real_item());
      enum_field_types field_type= item_field->field_type();
      if ((field_type == MYSQL_TYPE_DATE ||
           field_type == MYSQL_TYPE_DATETIME ||
           field_type == MYSQL_TYPE_NEWDATE ||
           field_type == MYSQL_TYPE_TIMESTAMP) &&
          item_field->field->flags & PART_KEY_FLAG)
      {
        *out_func_type= func_type;
        return item_field;
      }
    }
  }
  return nullptr;
}


void Date_cmp_func_rewriter::rewrite_le_gt_lt_ge()
{
  if (rewrite_func_type == Item_func::LE_FUNC ||
      rewrite_func_type == Item_func::GT_FUNC)
  {
    const_arg_value= create_end_bound();
  }
  else if (rewrite_func_type == Item_func::LT_FUNC ||
           rewrite_func_type == Item_func::GE_FUNC)
  {
    const_arg_value= create_start_bound();
  }
  if (!const_arg_value)
    return;
  Item *repl= create_cmp_func(rewrite_func_type, field_ref, const_arg_value);
  if (!repl)
    return;
  if (!repl->fix_fields(thd, &repl))
    result= repl;
}


Item *Date_cmp_func_rewriter::create_start_bound()
{
  Item_datetime *res;
  MYSQL_TIME const_arg_ts;
  memset(&const_arg_ts, 0, sizeof(const_arg_ts));
  const_arg_ts.time_type= MYSQL_TIMESTAMP_DATETIME;
  switch (argument_func_type) {
  case Item_func::YEAR_FUNC:
    const_arg_ts.year= static_cast<unsigned int>(const_arg_value->val_int());
    const_arg_ts.month= 1;
    const_arg_ts.day= 1;
    if (check_datetime_range(&const_arg_ts))
      return nullptr;
    res= new (thd->mem_root) Item_datetime(thd);
    res->set(&const_arg_ts);
    break;
  case Item_func::DATE_FUNC:
    if (field_ref->field->type() == MYSQL_TYPE_DATE)
      return const_arg_value;
    else
    {
      Datetime const_arg_dt(current_thd, const_arg_value);
      if (!const_arg_dt.is_valid_datetime())
        return nullptr;
      res= new (thd->mem_root) Item_datetime(thd);
      const_arg_dt.copy_to_mysql_time(&const_arg_ts);
      const_arg_ts.second_part= const_arg_ts.second=
        const_arg_ts.minute= const_arg_ts.hour= 0;
      const_arg_ts.time_type= MYSQL_TIMESTAMP_DATETIME;
      res->set(&const_arg_ts);
    }
    break;
  default:
    DBUG_ASSERT(0);
    res= nullptr;
    break;
  }
  return res;
}


Item *Date_cmp_func_rewriter::create_end_bound()
{
  Item_datetime *res;
  MYSQL_TIME const_arg_ts;
  memset(&const_arg_ts, 0, sizeof(const_arg_ts));
  const_arg_ts.time_type= MYSQL_TIMESTAMP_DATETIME;
  switch (argument_func_type) {
  case Item_func::YEAR_FUNC:
    const_arg_ts.year= static_cast<unsigned int>(const_arg_value->val_int());
    const_arg_ts.month= 12;
    const_arg_ts.day= 31;
    const_arg_ts.hour= 23;
    const_arg_ts.minute= TIME_MAX_MINUTE;
    const_arg_ts.second= TIME_MAX_SECOND;
    const_arg_ts.second_part= TIME_MAX_SECOND_PART;
    if (check_datetime_range(&const_arg_ts))
      return nullptr;
    res= new (thd->mem_root) Item_datetime(thd);
    res->set(&const_arg_ts);
    break;
  case Item_func::DATE_FUNC:
    if (field_ref->field->type() == MYSQL_TYPE_DATE)
      return const_arg_value;
    else
    {
      res= new (thd->mem_root) Item_datetime(thd);
      Datetime const_arg_dt(current_thd, const_arg_value);
      if (!const_arg_dt.is_valid_datetime())
        return nullptr;
      const_arg_dt.copy_to_mysql_time(&const_arg_ts);
      const_arg_ts.hour= 23;
      const_arg_ts.minute= TIME_MAX_MINUTE;
      const_arg_ts.second= TIME_MAX_SECOND;
      const_arg_ts.second_part=TIME_MAX_SECOND_PART;
      const_arg_ts.time_type= MYSQL_TIMESTAMP_DATETIME;
      res->set(&const_arg_ts);
    }
    break;
  default:
    DBUG_ASSERT(0);
    res= nullptr;
    break;
  }
  return res;
}


/*
  Create an Item for "arg1 $CMP arg2", where $CMP is specified by func_type.
*/
Item *Date_cmp_func_rewriter::create_cmp_func(Item_func::Functype func_type,
                                              Item *arg1, Item *arg2)
{
  Item *res;
  switch (func_type) {
    case Item_func::GE_FUNC:
      res= new (thd->mem_root) Item_func_ge(thd, arg1, arg2);
      break;
    case Item_func::GT_FUNC:
      res= new (thd->mem_root) Item_func_gt(thd, arg1, arg2);
      break;
    case Item_func::LE_FUNC:
      res= new (thd->mem_root) Item_func_le(thd, arg1, arg2);
      break;
    case Item_func::LT_FUNC:
      res= new (thd->mem_root) Item_func_lt(thd, arg1, arg2);
      break;
    default:
      DBUG_ASSERT(0);
      res= NULL;
  }
  return res;
}

void trace_date_item_rewrite(THD *thd, Item *new_item, Item *old_item)
{
  if (new_item != old_item)
  {
    Json_writer_object trace_wrapper(thd);
    trace_wrapper.add("transformation", "date_conds_into_sargable")
                 .add("before", old_item)
                 .add("after", new_item);
  }
}

