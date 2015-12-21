/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2015, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/**
  @file

  @brief
  This file defines all compare functions
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include <my_global.h>
#include "sql_priv.h"
#include <m_ctype.h>
#include "sql_select.h"
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_time.h"                  // make_truncated_value_warning
#include "sql_base.h"                  // dynamic_column_error_message


/**
  find an temporal type (item) that others will be converted to
  for the purpose of comparison.

  this is the type that will be used in warnings like
  "Incorrect <<TYPE>> value".
*/
Item *find_date_time_item(Item **args, uint nargs, uint col)
{
  Item *date_arg= 0, **arg, **arg_end;
  for (arg= args, arg_end= args + nargs; arg != arg_end ; arg++)
  {
    Item *item= arg[0]->element_index(col);
    if (item->cmp_type() != TIME_RESULT)
      continue;
    if (item->field_type() == MYSQL_TYPE_DATETIME)
      return item;
    if (!date_arg)
      date_arg= item;
  }
  return date_arg;
}


/*
  Compare row signature of two expressions

  SYNOPSIS:
    cmp_row_type()
    item1          the first expression
    item2         the second expression

  DESCRIPTION
    The function checks that two expressions have compatible row signatures
    i.e. that the number of columns they return are the same and that if they
    are both row expressions then each component from the first expression has 
    a row signature compatible with the signature of the corresponding component
    of the second expression.

  RETURN VALUES
    1  type incompatibility has been detected
    0  otherwise
*/

static int cmp_row_type(Item* item1, Item* item2)
{
  uint n= item1->cols();
  if (item2->check_cols(n))
    return 1;
  for (uint i=0; i<n; i++)
  {
    if (item2->element_index(i)->check_cols(item1->element_index(i)->cols()) ||
        (item1->element_index(i)->result_type() == ROW_RESULT &&
         cmp_row_type(item1->element_index(i), item2->element_index(i))))
      return 1;
  }
  return 0;
}


/**
  Aggregates result types from the array of items.

  SYNOPSIS:
    agg_cmp_type()
    type   [out] the aggregated type
    items        array of items to aggregate the type from
    nitems       number of items in the array

  DESCRIPTION
    This function aggregates result types from the array of items. Found type
    supposed to be used later for comparison of values of these items.
    Aggregation itself is performed by the item_cmp_type() function.
  @param[out] type    the aggregated type
  @param      items        array of items to aggregate the type from
  @param      nitems       number of items in the array

  @retval
    1  type incompatibility has been detected
  @retval
    0  otherwise
*/

static int agg_cmp_type(Item_result *type, Item **items, uint nitems)
{
  uint i;
  type[0]= items[0]->cmp_type();
  for (i= 1 ; i < nitems ; i++)
  {
    type[0]= item_cmp_type(type[0], items[i]);
    /*
      When aggregating types of two row expressions we have to check
      that they have the same cardinality and that each component
      of the first row expression has a compatible row signature with
      the signature of the corresponding component of the second row
      expression.
    */ 
    if (type[0] == ROW_RESULT && cmp_row_type(items[0], items[i]))
      return 1;     // error found: invalid usage of rows
  }
  return 0;
}


/**
  @brief Aggregates field types from the array of items.

  @param[in] items  array of items to aggregate the type from
  @paran[in] nitems number of items in the array
  @param[in] treat_bit_as_number - if BIT should be aggregated to a non-BIT
             counterpart as a LONGLONG number or as a VARBINARY string.

             Currently behaviour depends on the function:
             - LEAST/GREATEST treat BIT as VARBINARY when
               aggregating with a non-BIT counterpart.
               Note, UNION also works this way.

             - CASE, COALESCE, IF, IFNULL treat BIT as LONGLONG when
               aggregating with a non-BIT counterpart;

             This inconsistency may be changed in the future. See MDEV-8867.

             Note, independently from "treat_bit_as_number":
             - a single BIT argument gives BIT as a result
             - two BIT couterparts give BIT as a result

  @details This function aggregates field types from the array of items.
    Found type is supposed to be used later as the result field type
    of a multi-argument function.
    Aggregation itself is performed by the Field::field_type_merge()
    function.

  @note The term "aggregation" is used here in the sense of inferring the
    result type of a function from its argument types.

  @return aggregated field type.
*/

enum_field_types agg_field_type(Item **items, uint nitems,
                                bool treat_bit_as_number)
{
  uint i;
  if (!nitems || items[0]->result_type() == ROW_RESULT)
  {
    DBUG_ASSERT(0);
    return MYSQL_TYPE_NULL;
  }
  enum_field_types res= items[0]->field_type();
  uint unsigned_count= items[0]->unsigned_flag;
  for (i= 1 ; i < nitems ; i++)
  {
    enum_field_types cur= items[i]->field_type();
    if (treat_bit_as_number &&
        ((res == MYSQL_TYPE_BIT) ^ (cur == MYSQL_TYPE_BIT)))
    {
      if (res == MYSQL_TYPE_BIT)
        res= MYSQL_TYPE_LONGLONG; // BIT + non-BIT
      else
        cur= MYSQL_TYPE_LONGLONG; // non-BIT + BIT
    }
    res= Field::field_type_merge(res, cur);
    unsigned_count+= items[i]->unsigned_flag;
  }
  switch (res) {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_BIT:
    if (unsigned_count != 0 && unsigned_count != nitems)
    {
      /*
        If all arguments are of INT-alike type but have different
        unsigned_flag, then convert to DECIMAL.
      */
      return MYSQL_TYPE_NEWDECIMAL;
    }
  default:
    break;
  }
  return res;
}

/*
  Collects different types for comparison of first item with each other items

  SYNOPSIS
    collect_cmp_types()
      items             Array of items to collect types from
      nitems            Number of items in the array
      skip_nulls        Don't collect types of NULL items if TRUE

  DESCRIPTION
    This function collects different result types for comparison of the first
    item in the list with each of the remaining items in the 'items' array.

  RETURN
    0 - if row type incompatibility has been detected (see cmp_row_type)
    Bitmap of collected types - otherwise
*/

static uint collect_cmp_types(Item **items, uint nitems, bool skip_nulls= FALSE)
{
  uint i;
  uint found_types;
  Item_result left_cmp_type= items[0]->cmp_type();
  DBUG_ASSERT(nitems > 1);
  found_types= 0;
  for (i= 1; i < nitems ; i++)
  {
    if (skip_nulls && items[i]->type() == Item::NULL_ITEM)
      continue; // Skip NULL constant items
    if ((left_cmp_type == ROW_RESULT ||
         items[i]->cmp_type() == ROW_RESULT) &&
        cmp_row_type(items[0], items[i]))
      return 0;
    found_types|= 1U << (uint) item_cmp_type(left_cmp_type, items[i]);
  }
  /*
   Even if all right-hand items are NULLs and we are skipping them all, we need
   at least one type bit in the found_type bitmask.
  */
  if (skip_nulls && !found_types)
    found_types= 1U << (uint) left_cmp_type;
  return found_types;
}


/*
  Test functions
  Most of these  returns 0LL if false and 1LL if true and
  NULL if some arg is NULL.
*/

longlong Item_func_not::val_int()
{
  DBUG_ASSERT(fixed == 1);
  bool value= args[0]->val_bool();
  null_value=args[0]->null_value;
  return ((!null_value && value == 0) ? 1 : 0);
}

/*
  We put any NOT expression into parenthesis to avoid
  possible problems with internal view representations where
  any '!' is converted to NOT. It may cause a problem if
  '!' is used in an expression together with other operators
  whose precedence is lower than the precedence of '!' yet
  higher than the precedence of NOT.
*/

void Item_func_not::print(String *str, enum_query_type query_type)
{
  str->append('(');
  Item_func::print(str, query_type);
  str->append(')');
}

/**
  special NOT for ALL subquery.
*/


longlong Item_func_not_all::val_int()
{
  DBUG_ASSERT(fixed == 1);
  bool value= args[0]->val_bool();

  /*
    return TRUE if there was records in underlying select in max/min
    optimization (ALL subquery)
  */
  if (empty_underlying_subquery())
    return 1;

  null_value= args[0]->null_value;
  return ((!null_value && value == 0) ? 1 : 0);
}


bool Item_func_not_all::empty_underlying_subquery()
{
  return ((test_sum_item && !test_sum_item->any_value()) ||
          (test_sub_item && !test_sub_item->any_value()));
}

void Item_func_not_all::print(String *str, enum_query_type query_type)
{
  if (show)
    Item_func::print(str, query_type);
  else
    args[0]->print(str, query_type);
}


/**
  Special NOP (No OPeration) for ALL subquery. It is like
  Item_func_not_all.

  @return
    (return TRUE if underlying subquery do not return rows) but if subquery
    returns some rows it return same value as argument (TRUE/FALSE).
*/

longlong Item_func_nop_all::val_int()
{
  DBUG_ASSERT(fixed == 1);
  longlong value= args[0]->val_int();

  /*
    return FALSE if there was records in underlying select in max/min
    optimization (SAME/ANY subquery)
  */
  if (empty_underlying_subquery())
    return 0;

  null_value= args[0]->null_value;
  return (null_value || value == 0) ? 0 : 1;
}


/**
  Convert a constant item to an int and replace the original item.

    The function converts a constant expression or string to an integer.
    On successful conversion the original item is substituted for the
    result of the item evaluation.
    This is done when comparing DATE/TIME of different formats and
    also when comparing bigint to strings (in which case strings
    are converted to bigints).

  @param  thd             thread handle
  @param  field           item will be converted using the type of this field
  @param[in,out] item     reference to the item to convert

  @note
    This function is called only at prepare stage.
    As all derived tables are filled only after all derived tables
    are prepared we do not evaluate items with subselects here because
    they can contain derived tables and thus we may attempt to use a
    table that has not been populated yet.

  @retval
    0  Can't convert item
  @retval
    1  Item was replaced with an integer version of the item
*/

static bool convert_const_to_int(THD *thd, Item_field *field_item,
                                  Item **item)
{
  Field *field= field_item->field;
  int result= 0;

  /*
    We don't need to convert an integer to an integer,
    pretend it's already converted.

    But we still convert it if it is compared with a Field_year,
    as YEAR(2) may change the value of an integer when converting it
    to an integer (say, 0 to 70).
  */
  if ((*item)->cmp_type() == INT_RESULT &&
      field_item->field_type() != MYSQL_TYPE_YEAR)
    return 1;

  if ((*item)->const_item() && !(*item)->is_expensive())
  {
    TABLE *table= field->table;
    ulonglong orig_sql_mode= thd->variables.sql_mode;
    enum_check_fields orig_count_cuted_fields= thd->count_cuted_fields;
    my_bitmap_map *old_maps[2];
    ulonglong UNINIT_VAR(orig_field_val); /* original field value if valid */

    LINT_INIT_STRUCT(old_maps);

    /* table->read_set may not be set if we come here from a CREATE TABLE */
    if (table && table->read_set)
      dbug_tmp_use_all_columns(table, old_maps, 
                               table->read_set, table->write_set);
    /* For comparison purposes allow invalid dates like 2000-01-32 */
    thd->variables.sql_mode= (orig_sql_mode & ~MODE_NO_ZERO_DATE) | 
                             MODE_INVALID_DATES;
    thd->count_cuted_fields= CHECK_FIELD_IGNORE;

    /*
      Store the value of the field/constant because the call to save_in_field
      below overrides that value. Don't save field value if no data has been
      read yet.
    */
    bool save_field_value= (field_item->const_item() ||
                            !(field->table->status & STATUS_NO_RECORD));
    if (save_field_value)
      orig_field_val= field->val_int();
    if (!(*item)->save_in_field(field, 1) && !field->is_null())
    {
      int field_cmp= 0;
      // If item is a decimal value, we must reject it if it was truncated.
      if (field->type() == MYSQL_TYPE_LONGLONG)
      {
        field_cmp= stored_field_cmp_to_item(thd, field, *item);
        DBUG_PRINT("info", ("convert_const_to_int %d", field_cmp));
      }

      if (0 == field_cmp)
      {
        Item *tmp= new (thd->mem_root) Item_int_with_ref(thd, field->val_int(), *item,
                                         MY_TEST(field->flags & UNSIGNED_FLAG));
        if (tmp)
          thd->change_item_tree(item, tmp);
        result= 1;					// Item was replaced
      }
    }
    /* Restore the original field value. */
    if (save_field_value)
    {
      result= field->store(orig_field_val, TRUE);
      /* orig_field_val must be a valid value that can be restored back. */
      DBUG_ASSERT(!result);
    }
    thd->variables.sql_mode= orig_sql_mode;
    thd->count_cuted_fields= orig_count_cuted_fields;
    if (table && table->read_set)
      dbug_tmp_restore_column_maps(table->read_set, table->write_set, old_maps);
  }
  return result;
}


/*
  Make a special case of compare with fields to get nicer comparisons
  of bigint numbers with constant string.
  This directly contradicts the manual (number and a string should
  be compared as doubles), but seems to provide more
  "intuitive" behavior in some cases (but less intuitive in others).
*/
void Item_func::convert_const_compared_to_int_field(THD *thd)
{
  DBUG_ASSERT(arg_count >= 2); // Item_func_nullif has arg_count == 3
  if (!thd->lex->is_ps_or_view_context_analysis())
  {
    int field;
    if (args[field= 0]->real_item()->type() == FIELD_ITEM ||
        args[field= 1]->real_item()->type() == FIELD_ITEM)
    {
      Item_field *field_item= (Item_field*) (args[field]->real_item());
      if ((field_item->field_type() ==  MYSQL_TYPE_LONGLONG ||
           field_item->field_type() ==  MYSQL_TYPE_YEAR))
        convert_const_to_int(thd, field_item, &args[!field]);
    }
  }
}


bool Item_func::setup_args_and_comparator(THD *thd, Arg_comparator *cmp)
{
  DBUG_ASSERT(arg_count >= 2); // Item_func_nullif has arg_count == 3

  if (args[0]->cmp_type() == STRING_RESULT &&
      args[1]->cmp_type() == STRING_RESULT)
  {
    DTCollation tmp;
    if (agg_arg_charsets_for_comparison(tmp, args, 2))
      return true;
    cmp->m_compare_collation= tmp.collation;
  }
  //  Convert constants when compared to int/year field
  DBUG_ASSERT(functype() != LIKE_FUNC);
  convert_const_compared_to_int_field(thd);

  return cmp->set_cmp_func(this, &args[0], &args[1], true);
}


void Item_bool_rowready_func2::fix_length_and_dec()
{
  max_length= 1;				     // Function returns 0 or 1

  /*
    As some compare functions are generated after sql_yacc,
    we have to check for out of memory conditions here
  */
  if (!args[0] || !args[1])
    return;
  setup_args_and_comparator(current_thd, &cmp);
}


int Arg_comparator::set_compare_func(Item_func_or_sum *item, Item_result type)
{
  owner= item;
  func= comparator_matrix[type]
                         [is_owner_equal_func()];

  switch (type) {
  case TIME_RESULT:
    m_compare_collation= &my_charset_numeric;
    break;
  case ROW_RESULT:
  {
    uint n= (*a)->cols();
    if (n != (*b)->cols())
    {
      my_error(ER_OPERAND_COLUMNS, MYF(0), n);
      comparators= 0;
      return 1;
    }
    if (!(comparators= new Arg_comparator[n]))
      return 1;
    for (uint i=0; i < n; i++)
    {
      if ((*a)->element_index(i)->cols() != (*b)->element_index(i)->cols())
      {
	my_error(ER_OPERAND_COLUMNS, MYF(0), (*a)->element_index(i)->cols());
	return 1;
      }
      if (comparators[i].set_cmp_func(owner, (*a)->addr(i),
                                             (*b)->addr(i), set_null))
        return 1;
    }
    break;
  }
  case INT_RESULT:
  {
    if (func == &Arg_comparator::compare_int_signed)
    {
      if ((*a)->unsigned_flag)
        func= (((*b)->unsigned_flag)?
               &Arg_comparator::compare_int_unsigned :
               &Arg_comparator::compare_int_unsigned_signed);
      else if ((*b)->unsigned_flag)
        func= &Arg_comparator::compare_int_signed_unsigned;
    }
    else if (func== &Arg_comparator::compare_e_int)
    {
      if ((*a)->unsigned_flag ^ (*b)->unsigned_flag)
        func= &Arg_comparator::compare_e_int_diff_signedness;
    }
    break;
  }
  case STRING_RESULT:
  case DECIMAL_RESULT:
    break;
  case REAL_RESULT:
  {
    if ((*a)->decimals < NOT_FIXED_DEC && (*b)->decimals < NOT_FIXED_DEC)
    {
      precision= 5 / log_10[MY_MAX((*a)->decimals, (*b)->decimals) + 1];
      if (func == &Arg_comparator::compare_real)
        func= &Arg_comparator::compare_real_fixed;
      else if (func == &Arg_comparator::compare_e_real)
        func= &Arg_comparator::compare_e_real_fixed;
    }
    break;
  }
  }
  return 0;
}


/**
  Prepare the comparator (set the comparison function) for comparing
  items *a1 and *a2 in the context of 'type'.

  @param[in]      owner_arg  Item, peforming the comparison (e.g. Item_func_eq)
  @param[in,out]  a1         first argument to compare
  @param[in,out]  a2         second argument to compare
  @param[in]      type       type context to compare in

  Both *a1 and *a2 can be replaced by this method - typically by constant
  items, holding the cached converted value of the original (constant) item.
*/

int Arg_comparator::set_cmp_func(Item_func_or_sum *owner_arg,
                                 Item **a1, Item **a2)
{
  thd= current_thd;
  owner= owner_arg;
  set_null= set_null && owner_arg;
  a= a1;
  b= a2;
  m_compare_type= item_cmp_type(*a1, *a2);

  if (m_compare_type == STRING_RESULT &&
      (*a)->result_type() == STRING_RESULT &&
      (*b)->result_type() == STRING_RESULT)
  {
    /*
      We must set cmp_collation here as we may be called from for an automatic
      generated item, like in natural join
    */
    if (owner->agg_arg_charsets_for_comparison(&m_compare_collation, a, b))
      return 1;
  }

  if (m_compare_type == TIME_RESULT)
  {
    enum_field_types f_type= a[0]->field_type_for_temporal_comparison(b[0]);
    if (f_type == MYSQL_TYPE_TIME)
    {
      func= is_owner_equal_func() ? &Arg_comparator::compare_e_time :
                                    &Arg_comparator::compare_time;
    }
    else
    {
      func= is_owner_equal_func() ? &Arg_comparator::compare_e_datetime :
                                    &Arg_comparator::compare_datetime;
    }
    return 0;
  }

  if (m_compare_type == INT_RESULT &&
      (*a)->field_type() == MYSQL_TYPE_YEAR &&
      (*b)->field_type() == MYSQL_TYPE_YEAR)
  {
    m_compare_type= TIME_RESULT;
    func= is_owner_equal_func() ? &Arg_comparator::compare_e_datetime :
                                  &Arg_comparator::compare_datetime;
  }

  a= cache_converted_constant(thd, a, &a_cache, m_compare_type);
  b= cache_converted_constant(thd, b, &b_cache, m_compare_type);
  return set_compare_func(owner_arg, m_compare_type);
}


/**
  Convert and cache a constant.

  @param value      [in]  An item to cache
  @param cache_item [out] Placeholder for the cache item
  @param type       [in]  Comparison type

  @details
    When given item is a constant and its type differs from comparison type
    then cache its value to avoid type conversion of this constant on each
    evaluation. In this case the value is cached and the reference to the cache
    is returned.
    Original value is returned otherwise.

  @return cache item or original value.
*/

Item** Arg_comparator::cache_converted_constant(THD *thd_arg, Item **value,
                                                Item **cache_item,
                                                Item_result type)
{
  /*
    Don't need cache if doing context analysis only.
    Also, get_datetime_value creates Item_cache internally.
    Unless fixed, we should not do it here.
  */
  if (!thd_arg->lex->is_ps_or_view_context_analysis() &&
      (*value)->const_item() && type != (*value)->result_type() &&
      type != TIME_RESULT)
  {
    Item_cache *cache= Item_cache::get_cache(thd_arg, *value, type);
    cache->setup(thd_arg, *value);
    *cache_item= cache;
    return cache_item;
  }
  return value;
}


/**
  Retrieves correct DATETIME value from given item.

  @param[in]     thd         thread handle
  @param[in,out] item_arg    item to retrieve DATETIME value from
  @param[in,out] cache_arg   pointer to place to store the caching item to
  @param[in]     warn_item   item for issuing the conversion warning
  @param[out]    is_null     TRUE <=> the item_arg is null

  @details
    Retrieves the correct DATETIME value from given item for comparison by the
    compare_datetime() function.

    If the value should be compared as time (TIME_RESULT), it's retrieved as
    MYSQL_TIME. Otherwise it's read as a number/string and converted to time.
    Constant items are cached, so the convertion is only done once for them.

    Note the f_type behavior: if the item can be compared as time, then
    f_type is this item's field_type(). Otherwise it's field_type() of
    warn_item (which is the other operand of the comparison operator).
    This logic provides correct string/number to date/time conversion
    depending on the other operand (when comparing a string with a date, it's
    parsed as a date, when comparing a string with a time it's parsed as a time)

    If the item is a constant it is replaced by the Item_cache_int, that
    holds the packed datetime value.

  @return
    MYSQL_TIME value, packed in a longlong, suitable for comparison.
*/

longlong
get_datetime_value(THD *thd, Item ***item_arg, Item **cache_arg,
                   enum_field_types f_type, bool *is_null)
{
  longlong UNINIT_VAR(value);
  Item *item= **item_arg;
  value= item->val_temporal_packed(f_type);
  if ((*is_null= item->null_value))
    return ~(ulonglong) 0;
  if (cache_arg && item->const_item() &&
      !(item->type() == Item::CACHE_ITEM && item->cmp_type() == TIME_RESULT))
  {
    Query_arena backup;
    Query_arena *save_arena= thd->switch_to_arena_for_cached_items(&backup);
    Item_cache_temporal *cache= new (thd->mem_root) Item_cache_temporal(thd, f_type);
    if (save_arena)
      thd->set_query_arena(save_arena);

    cache->store_packed(value, item);
    *cache_arg= cache;
    *item_arg= cache_arg;
  }
  return value;
}


/*
  Compare items values as dates.

  SYNOPSIS
    Arg_comparator::compare_datetime()

  DESCRIPTION
    Compare items values as DATE/DATETIME for both EQUAL_FUNC and from other
    comparison functions. The correct DATETIME values are obtained
    with help of the get_datetime_value() function.

  RETURN
      -1   a < b or at least one item is null
       0   a == b
       1   a > b
*/

int Arg_comparator::compare_temporal(enum_field_types type)
{
  bool a_is_null, b_is_null;
  longlong a_value, b_value;

  if (set_null)
    owner->null_value= 1;

  /* Get DATE/DATETIME/TIME value of the 'a' item. */
  a_value= get_datetime_value(thd, &a, &a_cache, type, &a_is_null);
  if (a_is_null)
    return -1;

  /* Get DATE/DATETIME/TIME value of the 'b' item. */
  b_value= get_datetime_value(thd, &b, &b_cache, type, &b_is_null);
  if (b_is_null)
    return -1;

  /* Here we have two not-NULL values. */
  if (set_null)
    owner->null_value= 0;

  /* Compare values. */
  return a_value < b_value ? -1 : a_value > b_value ? 1 : 0;
}

int Arg_comparator::compare_e_temporal(enum_field_types type)
{
  bool a_is_null, b_is_null;
  longlong a_value, b_value;

  /* Get DATE/DATETIME/TIME value of the 'a' item. */
  a_value= get_datetime_value(thd, &a, &a_cache, type, &a_is_null);

  /* Get DATE/DATETIME/TIME value of the 'b' item. */
  b_value= get_datetime_value(thd, &b, &b_cache, type, &b_is_null);
  return a_is_null || b_is_null ? a_is_null == b_is_null
                                : a_value == b_value;
}

int Arg_comparator::compare_string()
{
  String *res1,*res2;
  if ((res1= (*a)->val_str(&value1)))
  {
    if ((res2= (*b)->val_str(&value2)))
    {
      if (set_null)
        owner->null_value= 0;
      return sortcmp(res1, res2, compare_collation());
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


/**
  Compare strings, but take into account that NULL == NULL.
*/


int Arg_comparator::compare_e_string()
{
  String *res1,*res2;
  res1= (*a)->val_str(&value1);
  res2= (*b)->val_str(&value2);
  if (!res1 || !res2)
    return MY_TEST(res1 == res2);
  return MY_TEST(sortcmp(res1, res2, compare_collation()) == 0);
}


int Arg_comparator::compare_real()
{
  /*
    Fix yet another manifestation of Bug#2338. 'Volatile' will instruct
    gcc to flush double values out of 80-bit Intel FPU registers before
    performing the comparison.
  */
  volatile double val1, val2;
  val1= (*a)->val_real();
  if (!(*a)->null_value)
  {
    val2= (*b)->val_real();
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      if (val1 < val2)	return -1;
      if (val1 == val2) return 0;
      return 1;
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}

int Arg_comparator::compare_decimal()
{
  my_decimal decimal1;
  my_decimal *val1= (*a)->val_decimal(&decimal1);
  if (!(*a)->null_value)
  {
    my_decimal decimal2;
    my_decimal *val2= (*b)->val_decimal(&decimal2);
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      return my_decimal_cmp(val1, val2);
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}

int Arg_comparator::compare_e_real()
{
  double val1= (*a)->val_real();
  double val2= (*b)->val_real();
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return MY_TEST(val1 == val2);
}

int Arg_comparator::compare_e_decimal()
{
  my_decimal decimal1, decimal2;
  my_decimal *val1= (*a)->val_decimal(&decimal1);
  my_decimal *val2= (*b)->val_decimal(&decimal2);
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return MY_TEST(my_decimal_cmp(val1, val2) == 0);
}


int Arg_comparator::compare_real_fixed()
{
  /*
    Fix yet another manifestation of Bug#2338. 'Volatile' will instruct
    gcc to flush double values out of 80-bit Intel FPU registers before
    performing the comparison.
  */
  volatile double val1, val2;
  val1= (*a)->val_real();
  if (!(*a)->null_value)
  {
    val2= (*b)->val_real();
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      if (val1 == val2 || fabs(val1 - val2) < precision)
        return 0;
      if (val1 < val2)
        return -1;
      return 1;
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


int Arg_comparator::compare_e_real_fixed()
{
  double val1= (*a)->val_real();
  double val2= (*b)->val_real();
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return MY_TEST(val1 == val2 || fabs(val1 - val2) < precision);
}


int Arg_comparator::compare_int_signed()
{
  longlong val1= (*a)->val_int();
  if (!(*a)->null_value)
  {
    longlong val2= (*b)->val_int();
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      if (val1 < val2)	return -1;
      if (val1 == val2)   return 0;
      return 1;
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


/**
  Compare values as BIGINT UNSIGNED.
*/

int Arg_comparator::compare_int_unsigned()
{
  ulonglong val1= (*a)->val_int();
  if (!(*a)->null_value)
  {
    ulonglong val2= (*b)->val_int();
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      if (val1 < val2)	return -1;
      if (val1 == val2)   return 0;
      return 1;
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


/**
  Compare signed (*a) with unsigned (*B)
*/

int Arg_comparator::compare_int_signed_unsigned()
{
  longlong sval1= (*a)->val_int();
  if (!(*a)->null_value)
  {
    ulonglong uval2= (ulonglong)(*b)->val_int();
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      if (sval1 < 0 || (ulonglong)sval1 < uval2)
        return -1;
      if ((ulonglong)sval1 == uval2)
        return 0;
      return 1;
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


/**
  Compare unsigned (*a) with signed (*B)
*/

int Arg_comparator::compare_int_unsigned_signed()
{
  ulonglong uval1= (ulonglong)(*a)->val_int();
  if (!(*a)->null_value)
  {
    longlong sval2= (*b)->val_int();
    if (!(*b)->null_value)
    {
      if (set_null)
        owner->null_value= 0;
      if (sval2 < 0)
        return 1;
      if (uval1 < (ulonglong)sval2)
        return -1;
      if (uval1 == (ulonglong)sval2)
        return 0;
      return 1;
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


int Arg_comparator::compare_e_int()
{
  longlong val1= (*a)->val_int();
  longlong val2= (*b)->val_int();
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return MY_TEST(val1 == val2);
}

/**
  Compare unsigned *a with signed *b or signed *a with unsigned *b.
*/
int Arg_comparator::compare_e_int_diff_signedness()
{
  longlong val1= (*a)->val_int();
  longlong val2= (*b)->val_int();
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return (val1 >= 0) && MY_TEST(val1 == val2);
}

int Arg_comparator::compare_row()
{
  int res= 0;
  bool was_null= 0;
  (*a)->bring_value();
  (*b)->bring_value();

  if ((*a)->null_value || (*b)->null_value)
  {
    owner->null_value= 1;
    return -1;
  }

  uint n= (*a)->cols();
  for (uint i= 0; i<n; i++)
  {
    res= comparators[i].compare();
    /* Aggregate functions don't need special null handling. */
    if (owner->null_value && owner->type() == Item::FUNC_ITEM)
    {
      // NULL was compared
      switch (((Item_func*)owner)->functype()) {
      case Item_func::NE_FUNC:
        break; // NE never aborts on NULL even if abort_on_null is set
      case Item_func::LT_FUNC:
      case Item_func::LE_FUNC:
      case Item_func::GT_FUNC:
      case Item_func::GE_FUNC:
        return -1; // <, <=, > and >= always fail on NULL
      case Item_func::EQ_FUNC:
        if (((Item_func_eq*)owner)->abort_on_null)
          return -1; // We do not need correct NULL returning
        break;
      default:
        DBUG_ASSERT(0);
        break;
      }
      was_null= 1;
      owner->null_value= 0;
      res= 0;  // continue comparison (maybe we will meet explicit difference)
    }
    else if (res)
      return res;
  }
  if (was_null)
  {
    /*
      There was NULL(s) in comparison in some parts, but there was no
      explicit difference in other parts, so we have to return NULL.
    */
    owner->null_value= 1;
    return -1;
  }
  return 0;
}


int Arg_comparator::compare_e_row()
{
  (*a)->bring_value();
  (*b)->bring_value();
  uint n= (*a)->cols();
  for (uint i= 0; i<n; i++)
  {
    if (!comparators[i].compare())
      return 0;
  }
  return 1;
}


void Item_func_truth::fix_length_and_dec()
{
  maybe_null= 0;
  null_value= 0;
  decimals= 0;
  max_length= 1;
}


void Item_func_truth::print(String *str, enum_query_type query_type)
{
  str->append('(');
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" is "));
  if (! affirmative)
    str->append(STRING_WITH_LEN("not "));
  if (value)
    str->append(STRING_WITH_LEN("true"));
  else
    str->append(STRING_WITH_LEN("false"));
  str->append(')');
}


bool Item_func_truth::val_bool()
{
  bool val= args[0]->val_bool();
  if (args[0]->null_value)
  {
    /*
      NULL val IS {TRUE, FALSE} --> FALSE
      NULL val IS NOT {TRUE, FALSE} --> TRUE
    */
    return (! affirmative);
  }

  if (affirmative)
  {
    /* {TRUE, FALSE} val IS {TRUE, FALSE} value */
    return (val == value);
  }

  /* {TRUE, FALSE} val IS NOT {TRUE, FALSE} value */
  return (val != value);
}


longlong Item_func_truth::val_int()
{
  return (val_bool() ? 1 : 0);
}


bool Item_in_optimizer::is_top_level_item()
{
  return ((Item_in_subselect *)args[1])->is_top_level_item();
}


void Item_in_optimizer::fix_after_pullout(st_select_lex *new_parent, Item **ref)
{
  /* This will re-calculate attributes of our Item_in_subselect: */
  Item_bool_func::fix_after_pullout(new_parent, ref);

  /* Then, re-calculate not_null_tables_cache: */
  eval_not_null_tables(NULL);
}


bool Item_in_optimizer::eval_not_null_tables(uchar *opt_arg)
{
  not_null_tables_cache= 0;
  if (is_top_level_item())
  {
    /*
      It is possible to determine NULL-rejectedness of the left arguments
      of IN only if it is a top-level predicate.
    */
    not_null_tables_cache= args[0]->not_null_tables();
  }
  return FALSE;
}


bool Item_in_optimizer::fix_left(THD *thd)
{
  DBUG_ENTER("Item_in_optimizer::fix_left");
  /*
    Here we will store pointer on place of main storage of left expression.
    For usual IN (ALL/ANY) it is subquery left_expr.
    For other cases (MAX/MIN optimization, non-transformed EXISTS (10.0))
    it is args[0].
  */
  Item **ref0= args;
  if (args[1]->type() == Item::SUBSELECT_ITEM &&
      ((Item_subselect *)args[1])->is_in_predicate())
  {
    /*
       left_expr->fix_fields() may cause left_expr to be substituted for
       another item. (e.g. an Item_field may be changed into Item_ref). This
       transformation is undone at the end of statement execution (e.g. the
       Item_ref is deleted). However, Item_in_optimizer::args[0] may keep
       the pointer to the post-transformation item. Because of that, on the
       next execution we need to copy args[1]->left_expr again.
    */
    ref0= &(((Item_in_subselect *)args[1])->left_expr);
    args[0]= ((Item_in_subselect *)args[1])->left_expr;
  }
  if ((!(*ref0)->fixed && (*ref0)->fix_fields(thd, ref0)) ||
      (!cache && !(cache= Item_cache::get_cache(thd, *ref0))))
    DBUG_RETURN(1);
  /*
    During fix_field() expression could be substituted.
    So we copy changes before use
  */
  if (args[0] != (*ref0))
    args[0]= (*ref0);
  DBUG_PRINT("info", ("actual fix fields"));

  cache->setup(thd, args[0]);
  if (cache->cols() == 1)
  {
    DBUG_ASSERT(args[0]->type() != ROW_ITEM);
    /* 
      Note: there can be cases when used_tables()==0 && !const_item(). See
      Item_sum::update_used_tables for details.
    */
    if ((used_tables_cache= args[0]->used_tables()) || !args[0]->const_item())
      cache->set_used_tables(OUTER_REF_TABLE_BIT);
    else
      cache->set_used_tables(0);
  }
  else
  {
    uint n= cache->cols();
    for (uint i= 0; i < n; i++)
    {
      /* Check that the expression (part of row) do not contain a subquery */
      if (args[0]->element_index(i)->walk(&Item::is_subquery_processor,
                                          FALSE, NULL))
      {
        my_error(ER_NOT_SUPPORTED_YET, MYF(0),
                 "SUBQUERY in ROW in left expression of IN/ALL/ANY");
        DBUG_RETURN(1);
      }
      Item *element=args[0]->element_index(i);
      if (element->used_tables() || !element->const_item())
      {
	((Item_cache *)cache->element_index(i))->
          set_used_tables(OUTER_REF_TABLE_BIT);
        cache->set_used_tables(OUTER_REF_TABLE_BIT);
      }
      else
	((Item_cache *)cache->element_index(i))->set_used_tables(0);
    }
    used_tables_cache= args[0]->used_tables();
  }
  eval_not_null_tables(NULL);
  with_sum_func= args[0]->with_sum_func;
  with_field= args[0]->with_field;
  if ((const_item_cache= args[0]->const_item()))
  {
    cache->store(args[0]);
    cache->cache_value();
  }
  if (args[1]->fixed)
  {
    /* to avoid overriding is called to update left expression */
    used_tables_and_const_cache_join(args[1]);
    with_sum_func= with_sum_func || args[1]->with_sum_func;
  }
  DBUG_RETURN(0);
}


bool Item_in_optimizer::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  Item_subselect *sub= 0;
  uint col;

  /*
     MAX/MIN optimization can convert the subquery into
     expr + Item_singlerow_subselect
   */
  if (args[1]->type() == Item::SUBSELECT_ITEM)
    sub= (Item_subselect *)args[1];

  if (fix_left(thd))
    return TRUE;
  if (args[0]->maybe_null)
    maybe_null=1;

  if (!args[1]->fixed && args[1]->fix_fields(thd, args+1))
    return TRUE;
  if (!invisible_mode() &&
      ((sub && ((col= args[0]->cols()) != sub->engine->cols())) ||
       (!sub && (args[1]->cols() != (col= 1)))))
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), col);
    return TRUE;
  }
  if (args[1]->maybe_null)
    maybe_null=1;
  with_subselect= 1;
  with_sum_func= with_sum_func || args[1]->with_sum_func;
  with_field= with_field || args[1]->with_field;
  used_tables_and_const_cache_join(args[1]);
  fixed= 1;
  return FALSE;
}

/**
  Check if Item_in_optimizer should work as a pass-through item for its 
  arguments.

  @note 
   Item_in_optimizer should work as pass-through for
    - subqueries that were processed by ALL/ANY->MIN/MAX rewrite
    - subqueries taht were originally EXISTS subqueries (and were coverted by
      the EXISTS->IN rewrite)

   When Item_in_optimizer is not not working as a pass-through, it
    - caches its "left argument", args[0].
    - makes adjustments to subquery item's return value for proper NULL
      value handling
*/

bool Item_in_optimizer::invisible_mode()
{
  /* MAX/MIN transformed or EXISTS->IN prepared => do nothing */
 return (args[1]->type() != Item::SUBSELECT_ITEM ||
         ((Item_subselect *)args[1])->substype() ==
         Item_subselect::EXISTS_SUBS);
}


/**
  Add an expression cache for this subquery if it is needed

  @param thd_arg         Thread handle

  @details
  The function checks whether an expression cache is needed for this item
  and if if so wraps the item into an item of the class
  Item_cache_wrapper with an appropriate expression cache set up there.

  @note
  used from Item::transform()

  @return
  new wrapper item if an expression cache is needed,
  this item - otherwise
*/

Item *Item_in_optimizer::expr_cache_insert_transformer(THD *thd, uchar *unused)
{
  DBUG_ENTER("Item_in_optimizer::expr_cache_insert_transformer");

  if (invisible_mode())
    DBUG_RETURN(this);

  if (expr_cache)
    DBUG_RETURN(expr_cache);

  if (args[1]->expr_cache_is_needed(thd) &&
      (expr_cache= set_expr_cache(thd)))
    DBUG_RETURN(expr_cache);

  DBUG_RETURN(this);
}



/**
    Collect and add to the list cache parameters for this Item.

    @param parameters    The list where to add parameters
*/

void Item_in_optimizer::get_cache_parameters(List<Item> &parameters)
{
  /* Add left expression to the list of the parameters of the subquery */
  if (!invisible_mode())
  {
    if (args[0]->cols() == 1)
      parameters.add_unique(args[0], &cmp_items);
    else
    {
      for (uint i= 0; i < args[0]->cols(); i++)
      {
        parameters.add_unique(args[0]->element_index(i), &cmp_items);
      }
    }
  }
  args[1]->get_cache_parameters(parameters);
}

/**
   The implementation of optimized \<outer expression\> [NOT] IN \<subquery\>
   predicates. The implementation works as follows.

   For the current value of the outer expression
   
   - If it contains only NULL values, the original (before rewrite by the
     Item_in_subselect rewrite methods) inner subquery is non-correlated and
     was previously executed, there is no need to re-execute it, and the
     previous return value is returned.

   - If it contains NULL values, check if there is a partial match for the
     inner query block by evaluating it. For clarity we repeat here the
     transformation previously performed on the sub-query. The expression

     <tt>
     ( oc_1, ..., oc_n ) 
     \<in predicate\>
     ( SELECT ic_1, ..., ic_n
       FROM \<table\>
       WHERE \<inner where\> 
     )
     </tt>

     was transformed into
     
     <tt>
     ( oc_1, ..., oc_n ) 
     \<in predicate\>
     ( SELECT ic_1, ..., ic_n 
       FROM \<table\> 
       WHERE \<inner where\> AND ... ( ic_k = oc_k OR ic_k IS NULL ) 
       HAVING ... NOT ic_k IS NULL
     )
     </tt>

     The evaluation will now proceed according to special rules set up
     elsewhere. These rules include:

     - The HAVING NOT \<inner column\> IS NULL conditions added by the
       aforementioned rewrite methods will detect whether they evaluated (and
       rejected) a NULL value and if so, will cause the subquery to evaluate
       to NULL. 

     - The added WHERE and HAVING conditions are present only for those inner
       columns that correspond to outer column that are not NULL at the moment.
     
     - If there is an eligible index for executing the subquery, the special
       access method "Full scan on NULL key" is employed which ensures that
       the inner query will detect if there are NULL values resulting from the
       inner query. This access method will quietly resort to table scan if it
       needs to find NULL values as well.

     - Under these conditions, the sub-query need only be evaluated in order to
       find out whether it produced any rows.
     
       - If it did, we know that there was a partial match since there are
         NULL values in the outer row expression.

       - If it did not, the result is FALSE or UNKNOWN. If at least one of the
         HAVING sub-predicates rejected a NULL value corresponding to an outer
         non-NULL, and hence the inner query block returns UNKNOWN upon
         evaluation, there was a partial match and the result is UNKNOWN.

   - If it contains no NULL values, the call is forwarded to the inner query
     block.

     @see Item_in_subselect::val_bool()
     @see Item_is_not_null_test::val_int()
*/

longlong Item_in_optimizer::val_int()
{
  bool tmp;
  DBUG_ASSERT(fixed == 1);
  cache->store(args[0]);
  cache->cache_value();
  DBUG_ENTER(" Item_in_optimizer::val_int");

  if (invisible_mode())
  {
    longlong res= args[1]->val_int();
    null_value= args[1]->null_value;
    DBUG_PRINT("info", ("pass trough"));
    DBUG_RETURN(res);
  }

  if (cache->null_value)
  {
     DBUG_PRINT("info", ("Left NULL..."));
    /*
      We're evaluating 
      "<outer_value_list> [NOT] IN (SELECT <inner_value_list>...)" 
      where one or more of the outer values is NULL. 
    */
    if (((Item_in_subselect*)args[1])->is_top_level_item())
    {
      /*
        We're evaluating a top level item, e.g. 
	"<outer_value_list> IN (SELECT <inner_value_list>...)",
	and in this case a NULL value in the outer_value_list means
        that the result shall be NULL/FALSE (makes no difference for
        top level items). The cached value is NULL, so just return
        NULL.
      */
      null_value= 1;
    }
    else
    {
      /*
	We're evaluating an item where a NULL value in either the
        outer or inner value list does not automatically mean that we
        can return NULL/FALSE. An example of such a query is
        "<outer_value_list> NOT IN (SELECT <inner_value_list>...)" 
        The result when there is at least one NULL value is: NULL if the
        SELECT evaluated over the non-NULL values produces at least
        one row, FALSE otherwise
      */
      Item_in_subselect *item_subs=(Item_in_subselect*)args[1]; 
      bool all_left_cols_null= true;
      const uint ncols= cache->cols();

      /*
        Turn off the predicates that are based on column compares for
        which the left part is currently NULL
      */
      for (uint i= 0; i < ncols; i++)
      {
        if (cache->element_index(i)->null_value)
          item_subs->set_cond_guard_var(i, FALSE);
        else 
          all_left_cols_null= false;
      }

      if (!item_subs->is_correlated && 
          all_left_cols_null && result_for_null_param != UNKNOWN)
      {
        /* 
           This is a non-correlated subquery, all values in the outer
           value list are NULL, and we have already evaluated the
           subquery for all NULL values: Return the same result we
           did last time without evaluating the subquery.
        */
        null_value= result_for_null_param;
      } 
      else 
      {
        /* The subquery has to be evaluated */
        (void) item_subs->val_bool_result();
        if (item_subs->engine->no_rows())
          null_value= item_subs->null_value;
        else
          null_value= TRUE;
        if (all_left_cols_null)
          result_for_null_param= null_value;
      }

      /* Turn all predicates back on */
      for (uint i= 0; i < ncols; i++)
        item_subs->set_cond_guard_var(i, TRUE);
    }
    DBUG_RETURN(0);
  }
  tmp= args[1]->val_bool_result();
  null_value= args[1]->null_value;
  DBUG_RETURN(tmp);
}


void Item_in_optimizer::keep_top_level_cache()
{
  cache->keep_array();
  save_cache= 1;
}


void Item_in_optimizer::cleanup()
{
  DBUG_ENTER("Item_in_optimizer::cleanup");
  Item_bool_func::cleanup();
  if (!save_cache)
    cache= 0;
  expr_cache= 0;
  DBUG_VOID_RETURN;
}


bool Item_in_optimizer::is_null()
{
  val_int();
  return null_value;
}


/**
  Transform an Item_in_optimizer and its arguments with a callback function.

  @param transformer the transformer callback function to be applied to the
         nodes of the tree of the object
  @param parameter to be passed to the transformer

  @detail
    Recursively transform the left and the right operand of this Item. The
    Right operand is an Item_in_subselect or its subclass. To avoid the
    creation of new Items, we use the fact the the left operand of the
    Item_in_subselect is the same as the one of 'this', so instead of
    transforming its operand, we just assign the left operand of the
    Item_in_subselect to be equal to the left operand of 'this'.
    The transformation is not applied further to the subquery operand
    if the IN predicate.

  @returns
    @retval pointer to the transformed item
    @retval NULL if an error occurred
*/

Item *Item_in_optimizer::transform(THD *thd, Item_transformer transformer,
                                   uchar *argument)
{
  Item *new_item;

  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());
  DBUG_ASSERT(arg_count == 2);

  /* Transform the left IN operand. */
  new_item= (*args)->transform(thd, transformer, argument);
  if (!new_item)
    return 0;
  /*
    THD::change_item_tree() should be called only if the tree was
    really transformed, i.e. when a new item has been created.
    Otherwise we'll be allocating a lot of unnecessary memory for
    change records at each execution.
  */
  if ((*args) != new_item)
    thd->change_item_tree(args, new_item);

  if (invisible_mode())
  {
    /* MAX/MIN transformed => pass through */
    new_item= args[1]->transform(thd, transformer, argument);
    if (!new_item)
      return 0;
    if (args[1] != new_item)
      thd->change_item_tree(args + 1, new_item);
  }
  else
  {
    /*
      Transform the right IN operand which should be an Item_in_subselect or a
      subclass of it. The left operand of the IN must be the same as the left
      operand of this Item_in_optimizer, so in this case there is no further
      transformation, we only make both operands the same.
      TODO: is it the way it should be?
    */
    DBUG_ASSERT((args[1])->type() == Item::SUBSELECT_ITEM &&
                (((Item_subselect*)(args[1]))->substype() ==
                 Item_subselect::IN_SUBS ||
                 ((Item_subselect*)(args[1]))->substype() ==
                 Item_subselect::ALL_SUBS ||
                 ((Item_subselect*)(args[1]))->substype() ==
                 Item_subselect::ANY_SUBS));

    Item_in_subselect *in_arg= (Item_in_subselect*)args[1];
    thd->change_item_tree(&in_arg->left_expr, args[0]);
  }
  return (this->*transformer)(thd, argument);
}


bool Item_in_optimizer::is_expensive_processor(uchar *arg)
{
  return args[0]->is_expensive_processor(arg) ||
         args[1]->is_expensive_processor(arg);
}


bool Item_in_optimizer::is_expensive()
{
  return args[0]->is_expensive() || args[1]->is_expensive();
}


longlong Item_func_eq::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int value= cmp.compare();
  return value == 0 ? 1 : 0;
}


/** Same as Item_func_eq, but NULL = NULL. */

void Item_func_equal::fix_length_and_dec()
{
  Item_bool_rowready_func2::fix_length_and_dec();
  maybe_null=null_value=0;
}

longlong Item_func_equal::val_int()
{
  DBUG_ASSERT(fixed == 1);
  return cmp.compare();
}

longlong Item_func_ne::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int value= cmp.compare();
  return value != 0 && !null_value ? 1 : 0;
}


longlong Item_func_ge::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int value= cmp.compare();
  return value >= 0 ? 1 : 0;
}


longlong Item_func_gt::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int value= cmp.compare();
  return value > 0 ? 1 : 0;
}

longlong Item_func_le::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int value= cmp.compare();
  return value <= 0 && !null_value ? 1 : 0;
}


longlong Item_func_lt::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int value= cmp.compare();
  return value < 0 && !null_value ? 1 : 0;
}


longlong Item_func_strcmp::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String *a= args[0]->val_str(&value1);
  String *b= args[1]->val_str(&value2);
  if (!a || !b)
  {
    null_value=1;
    return 0;
  }
  int value= cmp_collation.sortcmp(a, b);
  null_value=0;
  return !value ? 0 : (value < 0 ? (longlong) -1 : (longlong) 1);
}


bool Item_func_opt_neg::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;
  if (item->type() != FUNC_ITEM)
    return 0;
  Item_func *item_func=(Item_func*) item;
  if (arg_count != item_func->argument_count() ||
      functype() != item_func->functype())
    return 0;
  if (negated != ((Item_func_opt_neg *) item_func)->negated)
    return 0;
  for (uint i=0; i < arg_count ; i++)
    if (!args[i]->eq(item_func->arguments()[i], binary_cmp))
      return 0;
  return 1;
}


void Item_func_interval::fix_length_and_dec()
{
  uint rows= row->cols();
  
  use_decimal_comparison= ((row->element_index(0)->result_type() ==
                            DECIMAL_RESULT) ||
                           (row->element_index(0)->result_type() ==
                            INT_RESULT));
  if (rows > 8)
  {
    bool not_null_consts= TRUE;

    for (uint i= 1; not_null_consts && i < rows; i++)
    {
      Item *el= row->element_index(i);
      not_null_consts&= el->const_item() && !el->is_null();
    }

    if (not_null_consts &&
        (intervals=
          (interval_range*) sql_alloc(sizeof(interval_range) * (rows - 1))))
    {
      if (use_decimal_comparison)
      {
        for (uint i= 1; i < rows; i++)
        {
          Item *el= row->element_index(i);
          interval_range *range= intervals + (i-1);
          if ((el->result_type() == DECIMAL_RESULT) ||
              (el->result_type() == INT_RESULT))
          {
            range->type= DECIMAL_RESULT;
            range->dec.init();
            my_decimal *dec= el->val_decimal(&range->dec);
            if (dec != &range->dec)
            {
              range->dec= *dec;
            }
          }
          else
          {
            range->type= REAL_RESULT;
            range->dbl= el->val_real();
          }
        }
      }
      else
      {
        for (uint i= 1; i < rows; i++)
        {
          intervals[i-1].dbl= row->element_index(i)->val_real();
        }
      }
    }
  }
  maybe_null= 0;
  max_length= 2;
  used_tables_and_const_cache_join(row);
  not_null_tables_cache= row->not_null_tables();
  with_sum_func= with_sum_func || row->with_sum_func;
  with_field= with_field || row->with_field;
}


/**
  Execute Item_func_interval().

  @note
    If we are doing a decimal comparison, we are evaluating the first
    item twice.

  @return
    - -1 if null value,
    - 0 if lower than lowest
    - 1 - arg_count-1 if between args[n] and args[n+1]
    - arg_count if higher than biggest argument
*/

longlong Item_func_interval::val_int()
{
  DBUG_ASSERT(fixed == 1);
  double value;
  my_decimal dec_buf, *dec= NULL;
  uint i;

  if (use_decimal_comparison)
  {
    dec= row->element_index(0)->val_decimal(&dec_buf);
    if (row->element_index(0)->null_value)
      return -1;
    my_decimal2double(E_DEC_FATAL_ERROR, dec, &value);
  }
  else
  {
    value= row->element_index(0)->val_real();
    if (row->element_index(0)->null_value)
      return -1;
  }

  if (intervals)
  {					// Use binary search to find interval
    uint start,end;
    start= 0;
    end=   row->cols()-2;
    while (start != end)
    {
      uint mid= (start + end + 1) / 2;
      interval_range *range= intervals + mid;
      my_bool cmp_result;
      /*
        The values in the range intervall may have different types,
        Only do a decimal comparision of the first argument is a decimal
        and we are comparing against a decimal
      */
      if (dec && range->type == DECIMAL_RESULT)
        cmp_result= my_decimal_cmp(&range->dec, dec) <= 0;
      else
        cmp_result= (range->dbl <= value);
      if (cmp_result)
	start= mid;
      else
	end= mid - 1;
    }
    interval_range *range= intervals+start;
    return ((dec && range->type == DECIMAL_RESULT) ?
            my_decimal_cmp(dec, &range->dec) < 0 :
            value < range->dbl) ? 0 : start + 1;
  }

  for (i=1 ; i < row->cols() ; i++)
  {
    Item *el= row->element_index(i);
    if (use_decimal_comparison &&
        ((el->result_type() == DECIMAL_RESULT) ||
         (el->result_type() == INT_RESULT)))
    {
      my_decimal e_dec_buf, *e_dec= el->val_decimal(&e_dec_buf);
      /* Skip NULL ranges. */
      if (el->null_value)
        continue;
      if (my_decimal_cmp(e_dec, dec) > 0)
        return i - 1;
    }
    else 
    {
      double val= el->val_real();
      /* Skip NULL ranges. */
      if (el->null_value)
        continue;
      if (val > value)
        return i - 1;
    }
  }
  return i-1;
}


/**
  Perform context analysis of a BETWEEN item tree.

    This function performs context analysis (name resolution) and calculates
    various attributes of the item tree with Item_func_between as its root.
    The function saves in ref the pointer to the item or to a newly created
    item that is considered as a replacement for the original one.

  @param thd     reference to the global context of the query thread
  @param ref     pointer to Item* variable where pointer to resulting "fixed"
                 item is to be assigned

  @note
    Let T0(e)/T1(e) be the value of not_null_tables(e) when e is used on
    a predicate/function level. Then it's easy to show that:
    @verbatim
      T0(e BETWEEN e1 AND e2)     = union(T1(e),T1(e1),T1(e2))
      T1(e BETWEEN e1 AND e2)     = union(T1(e),intersection(T1(e1),T1(e2)))
      T0(e NOT BETWEEN e1 AND e2) = union(T1(e),intersection(T1(e1),T1(e2)))
      T1(e NOT BETWEEN e1 AND e2) = union(T1(e),intersection(T1(e1),T1(e2)))
    @endverbatim

  @retval
    0   ok
  @retval
    1   got error
*/


bool Item_func_between::eval_not_null_tables(uchar *opt_arg)
{
  if (Item_func_opt_neg::eval_not_null_tables(NULL))
    return 1;

  /* not_null_tables_cache == union(T1(e),T1(e1),T1(e2)) */
  if (pred_level && !negated)
    return 0;

  /* not_null_tables_cache == union(T1(e), intersection(T1(e1),T1(e2))) */
  not_null_tables_cache= (args[0]->not_null_tables() |
                          (args[1]->not_null_tables() &
                           args[2]->not_null_tables()));
  return 0;
}  


bool Item_func_between::count_sargable_conds(uchar *arg)
{
  SELECT_LEX *sel= (SELECT_LEX *) arg;
  sel->cond_count++;
  sel->between_count++;
  return 0;
}


void Item_func_between::fix_after_pullout(st_select_lex *new_parent, Item **ref)
{
  /* This will re-calculate attributes of the arguments */
  Item_func_opt_neg::fix_after_pullout(new_parent, ref);
  /* Then, re-calculate not_null_tables_cache according to our special rules */
  eval_not_null_tables(NULL);
}

void Item_func_between::fix_length_and_dec()
{
  THD *thd= current_thd;
  max_length= 1;
  compare_as_dates= 0;

  /*
    As some compare functions are generated after sql_yacc,
    we have to check for out of memory conditions here
  */
  if (!args[0] || !args[1] || !args[2])
    return;
  if (agg_cmp_type(&m_compare_type, args, 3))
    return;

  if (m_compare_type == STRING_RESULT &&
      agg_arg_charsets_for_comparison(cmp_collation, args, 3))
   return;

  /*
    When comparing as date/time, we need to convert non-temporal values
    (e.g.  strings) to MYSQL_TIME. get_datetime_value() does it
    automatically when one of the operands is a date/time.  But here we
    may need to compare two strings as dates (str1 BETWEEN str2 AND date).
    For this to work, we need to know what date/time type we compare
    strings as.
  */
  if (m_compare_type ==  TIME_RESULT)
    compare_as_dates= find_date_time_item(args, 3, 0);

  /* See the comment about the similar block in Item_bool_func2 */
  if (args[0]->real_item()->type() == FIELD_ITEM &&
      !thd->lex->is_ps_or_view_context_analysis())
  {
    Item_field *field_item= (Item_field*) (args[0]->real_item());
    if (field_item->field_type() ==  MYSQL_TYPE_LONGLONG ||
        field_item->field_type() ==  MYSQL_TYPE_YEAR)
    {
      const bool cvt_arg1= convert_const_to_int(thd, field_item, &args[1]);
      const bool cvt_arg2= convert_const_to_int(thd, field_item, &args[2]);
      if (cvt_arg1 && cvt_arg2)
        m_compare_type= INT_RESULT;              // Works for all types.
    }
  }
}


longlong Item_func_between::val_int()
{
  DBUG_ASSERT(fixed == 1);

  switch (m_compare_type) {
  case TIME_RESULT:
  {
    THD *thd= current_thd;
    longlong value, a, b;
    Item *cache, **ptr;
    bool value_is_null, a_is_null, b_is_null;

    ptr= &args[0];
    enum_field_types f_type= field_type_for_temporal_comparison(compare_as_dates);
    value= get_datetime_value(thd, &ptr, &cache, f_type, &value_is_null);
    if (ptr != &args[0])
      thd->change_item_tree(&args[0], *ptr);

    if ((null_value= value_is_null))
      return 0;

    ptr= &args[1];
    a= get_datetime_value(thd, &ptr, &cache, f_type, &a_is_null);
    if (ptr != &args[1])
      thd->change_item_tree(&args[1], *ptr);

    ptr= &args[2];
    b= get_datetime_value(thd, &ptr, &cache, f_type, &b_is_null);
    if (ptr != &args[2])
      thd->change_item_tree(&args[2], *ptr);

    if (!a_is_null && !b_is_null)
      return (longlong) ((value >= a && value <= b) != negated);
    if (a_is_null && b_is_null)
      null_value=1;
    else if (a_is_null)
      null_value= value <= b;			// not null if false range.
    else
      null_value= value >= a;
    break;
  }

  case STRING_RESULT:
  {
    String *value,*a,*b;
    value=args[0]->val_str(&value0);
    if ((null_value=args[0]->null_value))
      return 0;
    a=args[1]->val_str(&value1);
    b=args[2]->val_str(&value2);
    if (!args[1]->null_value && !args[2]->null_value)
      return (longlong) ((sortcmp(value,a,cmp_collation.collation) >= 0 &&
                          sortcmp(value,b,cmp_collation.collation) <= 0) !=
                         negated);
    if (args[1]->null_value && args[2]->null_value)
      null_value=1;
    else if (args[1]->null_value)
    {
      // Set to not null if false range.
      null_value= sortcmp(value,b,cmp_collation.collation) <= 0;
    }
    else
    {
      // Set to not null if false range.
      null_value= sortcmp(value,a,cmp_collation.collation) >= 0;
    }
    break;
  }
  case INT_RESULT:
  {
    longlong value=args[0]->val_int(), a, b;
    if ((null_value=args[0]->null_value))
      return 0;					/* purecov: inspected */
    a=args[1]->val_int();
    b=args[2]->val_int();
    if (!args[1]->null_value && !args[2]->null_value)
      return (longlong) ((value >= a && value <= b) != negated);
    if (args[1]->null_value && args[2]->null_value)
      null_value=1;
    else if (args[1]->null_value)
    {
      null_value= value <= b;			// not null if false range.
    }
    else
    {
      null_value= value >= a;
    }
    break;
  }
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec= args[0]->val_decimal(&dec_buf),
               a_buf, *a_dec, b_buf, *b_dec;
    if ((null_value=args[0]->null_value))
      return 0;					/* purecov: inspected */
    a_dec= args[1]->val_decimal(&a_buf);
    b_dec= args[2]->val_decimal(&b_buf);
    if (!args[1]->null_value && !args[2]->null_value)
      return (longlong) ((my_decimal_cmp(dec, a_dec) >= 0 &&
                          my_decimal_cmp(dec, b_dec) <= 0) != negated);
    if (args[1]->null_value && args[2]->null_value)
      null_value=1;
    else if (args[1]->null_value)
      null_value= (my_decimal_cmp(dec, b_dec) <= 0);
    else
      null_value= (my_decimal_cmp(dec, a_dec) >= 0);
    break;
  }
  case REAL_RESULT:
  {
    double value= args[0]->val_real(),a,b;
    if ((null_value=args[0]->null_value))
      return 0;					/* purecov: inspected */
    a= args[1]->val_real();
    b= args[2]->val_real();
    if (!args[1]->null_value && !args[2]->null_value)
      return (longlong) ((value >= a && value <= b) != negated);
    if (args[1]->null_value && args[2]->null_value)
      null_value=1;
    else if (args[1]->null_value)
    {
      null_value= value <= b;			// not null if false range.
    }
    else
    {
      null_value= value >= a;
    }
    break;
  }
  case ROW_RESULT:
    DBUG_ASSERT(0);
    null_value= 1;
    return 0;
  }
  return (longlong) (!null_value && negated);
}


void Item_func_between::print(String *str, enum_query_type query_type)
{
  str->append('(');
  args[0]->print(str, query_type);
  if (negated)
    str->append(STRING_WITH_LEN(" not"));
  str->append(STRING_WITH_LEN(" between "));
  args[1]->print(str, query_type);
  str->append(STRING_WITH_LEN(" and "));
  args[2]->print(str, query_type);
  str->append(')');
}


void
Item_func_case_abbreviation2::fix_length_and_dec2(Item **args)
{
  uint32 char_length;
  set_handler_by_field_type(agg_field_type(args, 2, true));
  maybe_null=args[0]->maybe_null || args[1]->maybe_null;
  decimals= MY_MAX(args[0]->decimals, args[1]->decimals);
  unsigned_flag= args[0]->unsigned_flag && args[1]->unsigned_flag;

  if (Item_func_case_abbreviation2::result_type() == DECIMAL_RESULT ||
      Item_func_case_abbreviation2::result_type() == INT_RESULT)
  {
    int len0= args[0]->max_char_length() - args[0]->decimals
      - (args[0]->unsigned_flag ? 0 : 1);

    int len1= args[1]->max_char_length() - args[1]->decimals
      - (args[1]->unsigned_flag ? 0 : 1);

    char_length= MY_MAX(len0, len1) + decimals + (unsigned_flag ? 0 : 1);
  }
  else
    char_length= MY_MAX(args[0]->max_char_length(), args[1]->max_char_length());

  switch (Item_func_case_abbreviation2::result_type()) {
  case STRING_RESULT:
    if (count_string_result_length(Item_func_case_abbreviation2::field_type(),
                                   args, 2))
      return;
    break;
  case DECIMAL_RESULT:
  case REAL_RESULT:
    break;
  case INT_RESULT:
    decimals= 0;
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);
  }
  fix_char_length(char_length);
}



uint Item_func_case_abbreviation2::decimal_precision2(Item **args) const
{
  int arg0_int_part= args[0]->decimal_int_part();
  int arg1_int_part= args[1]->decimal_int_part();
  int max_int_part= MY_MAX(arg0_int_part, arg1_int_part);
  int precision= max_int_part + decimals;
  return MY_MIN(precision, DECIMAL_MAX_PRECISION);
}


Field *Item_func_ifnull::tmp_table_field(TABLE *table)
{
  return tmp_table_field_from_field_type(table, false, false);
}

double
Item_func_ifnull::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value= args[0]->val_real();
  if (!args[0]->null_value)
  {
    null_value=0;
    return value;
  }
  value= args[1]->val_real();
  if ((null_value=args[1]->null_value))
    return 0.0;
  return value;
}

longlong
Item_func_ifnull::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong value=args[0]->val_int();
  if (!args[0]->null_value)
  {
    null_value=0;
    return value;
  }
  value=args[1]->val_int();
  if ((null_value=args[1]->null_value))
    return 0;
  return value;
}


my_decimal *Item_func_ifnull::decimal_op(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed == 1);
  my_decimal *value= args[0]->val_decimal(decimal_value);
  if (!args[0]->null_value)
  {
    null_value= 0;
    return value;
  }
  value= args[1]->val_decimal(decimal_value);
  if ((null_value= args[1]->null_value))
    return 0;
  return value;
}


String *
Item_func_ifnull::str_op(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res  =args[0]->val_str(str);
  if (!args[0]->null_value)
  {
    null_value=0;
    res->set_charset(collation.collation);
    return res;
  }
  res=args[1]->val_str(str);
  if ((null_value=args[1]->null_value))
    return 0;
  res->set_charset(collation.collation);
  return res;
}


bool Item_func_ifnull::date_op(MYSQL_TIME *ltime, uint fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  if (!args[0]->get_date_with_conversion(ltime, fuzzydate & ~TIME_FUZZY_DATES))
    return (null_value= false);
  if (!args[1]->get_date_with_conversion(ltime, fuzzydate & ~TIME_FUZZY_DATES))
    return (null_value= false);
  bzero((char*) ltime,sizeof(*ltime));
  return null_value= !(fuzzydate & TIME_FUZZY_DATES);
}


/**
  Perform context analysis of an IF item tree.

    This function performs context analysis (name resolution) and calculates
    various attributes of the item tree with Item_func_if as its root.
    The function saves in ref the pointer to the item or to a newly created
    item that is considered as a replacement for the original one.

  @param thd     reference to the global context of the query thread
  @param ref     pointer to Item* variable where pointer to resulting "fixed"
                 item is to be assigned

  @note
    Let T0(e)/T1(e) be the value of not_null_tables(e) when e is used on
    a predicate/function level. Then it's easy to show that:
    @verbatim
      T0(IF(e,e1,e2)  = T1(IF(e,e1,e2))
      T1(IF(e,e1,e2)) = intersection(T1(e1),T1(e2))
    @endverbatim

  @retval
    0   ok
  @retval
    1   got error
*/

bool
Item_func_if::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  args[0]->top_level_item();

  if (Item_func::fix_fields(thd, ref))
    return 1;

  return 0;
}


bool
Item_func_if::eval_not_null_tables(uchar *opt_arg)
{
  if (Item_func::eval_not_null_tables(NULL))
    return 1;

  not_null_tables_cache= (args[1]->not_null_tables() &
                          args[2]->not_null_tables());

  return 0;
}


void Item_func_if::fix_after_pullout(st_select_lex *new_parent, Item **ref)
{
  /* This will re-calculate attributes of the arguments */
  Item_func::fix_after_pullout(new_parent, ref);
  /* Then, re-calculate not_null_tables_cache according to our special rules */
  eval_not_null_tables(NULL);
}


void Item_func_if::cache_type_info(Item *source)
{
  Type_std_attributes::set(source);
  set_handler_by_field_type(source->field_type());
  maybe_null=         source->maybe_null;
}


void
Item_func_if::fix_length_and_dec()
{
  // Let IF(cond, expr, NULL) and IF(cond, NULL, expr) inherit type from expr.
  if (args[1]->type() == NULL_ITEM)
  {
    cache_type_info(args[2]);
    maybe_null= true;
    // If both arguments are NULL, make resulting type BINARY(0).
    if (args[2]->type() == NULL_ITEM)
      set_handler_by_field_type(MYSQL_TYPE_STRING);
    return;
  }
  if (args[2]->type() == NULL_ITEM)
  {
    cache_type_info(args[1]);
    maybe_null= true;
    return;
  }
  Item_func_case_abbreviation2::fix_length_and_dec2(args + 1);
}


double
Item_func_if::real_op()
{
  DBUG_ASSERT(fixed == 1);
  Item *arg= args[0]->val_bool() ? args[1] : args[2];
  double value= arg->val_real();
  null_value=arg->null_value;
  return value;
}

longlong
Item_func_if::int_op()
{
  DBUG_ASSERT(fixed == 1);
  Item *arg= args[0]->val_bool() ? args[1] : args[2];
  longlong value=arg->val_int();
  null_value=arg->null_value;
  return value;
}

String *
Item_func_if::str_op(String *str)
{
  DBUG_ASSERT(fixed == 1);
  Item *arg= args[0]->val_bool() ? args[1] : args[2];
  String *res=arg->val_str(str);
  if (res)
    res->set_charset(collation.collation);
  if ((null_value=arg->null_value))
    res= NULL;
  return res;
}


my_decimal *
Item_func_if::decimal_op(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed == 1);
  Item *arg= args[0]->val_bool() ? args[1] : args[2];
  my_decimal *value= arg->val_decimal(decimal_value);
  if ((null_value= arg->null_value))
    value= NULL;
  return value;
}


bool Item_func_if::date_op(MYSQL_TIME *ltime, uint fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  Item *arg= args[0]->val_bool() ? args[1] : args[2];
  return (null_value= arg->get_date_with_conversion(ltime, fuzzydate));
}


void
Item_func_nullif::fix_length_and_dec()
{
  if (!args[2])					// Only false if EOM
    return;

  set_handler_by_field_type(args[2]->field_type());
  collation.set(args[2]->collation);
  decimals= args[2]->decimals;
  unsigned_flag= args[2]->unsigned_flag;
  fix_char_length(args[2]->max_char_length());
  maybe_null=1;
  setup_args_and_comparator(current_thd, &cmp);
}


void Item_func_nullif::print(String *str, enum_query_type query_type)
{
  /*
    NULLIF(a,b) is implemented according to the SQL standard as a short for
    CASE WHEN a=b THEN NULL ELSE a END

    The constructor of Item_func_nullif sets args[0] and args[2] to the
    same item "a", and sets args[1] to "b".

    If "this" is a part of a WHERE or ON condition, then:
    - the left "a" is a subject to equal field propagation with ANY_SUBST.
    - the right "a" is a subject to equal field propagation with IDENTITY_SUBST.
    Therefore, after equal field propagation args[0] and args[2] can point
    to different items.
  */
  if (!(query_type & QT_ITEM_FUNC_NULLIF_TO_CASE) || args[0] == args[2])
  {
    /*
      If no QT_ITEM_FUNC_NULLIF_TO_CASE is requested,
      that means we want the original NULLIF() representation,
      e.g. when we are in:
        SHOW CREATE {VIEW|FUNCTION|PROCEDURE}

      The original representation is possible only if
      args[0] and args[2] still point to the same Item.

      The caller must pass call print() with QT_ITEM_FUNC_NULLIF_TO_CASE
      if an expression has undergone some optimization
      (e.g. equal field propagation done in optimize_cond()) already and
      NULLIF() potentially has two different representations of "a":
      - one "a" for comparison
      - another "a" for the returned value!

      Note, the EXPLAIN EXTENDED and EXPLAIN FORMAT=JSON routines
      do pass QT_ITEM_FUNC_NULLIF_TO_CASE to print().
    */
    DBUG_ASSERT(args[0] == args[2]);
    str->append(func_name());
    str->append('(');
    args[2]->print(str, query_type);
    str->append(',');
    args[1]->print(str, query_type);
    str->append(')');
  }
  else
  {
    /*
      args[0] and args[2] are different items.
      This is possible after WHERE optimization (equal fields propagation etc),
      e.g. in EXPLAIN EXTENDED or EXPLAIN FORMAT=JSON.
      As it's not possible to print as a function with 2 arguments any more,
      do it in the CASE style.
    */
    str->append(STRING_WITH_LEN("(case when "));
    args[0]->print(str, query_type);
    str->append(STRING_WITH_LEN(" = "));
    args[1]->print(str, query_type);
    str->append(STRING_WITH_LEN(" then NULL else "));
    args[2]->print(str, query_type);
    str->append(STRING_WITH_LEN(" end)"));
  }
}


/**
  @note
  Note that we have to evaluate the first argument twice as the compare
  may have been done with a different type than return value
  @return
    NULL  if arguments are equal
  @return
    the first argument if not equal
*/

double
Item_func_nullif::real_op()
{
  DBUG_ASSERT(fixed == 1);
  double value;
  if (!cmp.compare())
  {
    null_value=1;
    return 0.0;
  }
  value= args[2]->val_real();
  null_value= args[2]->null_value;
  return value;
}

longlong
Item_func_nullif::int_op()
{
  DBUG_ASSERT(fixed == 1);
  longlong value;
  if (!cmp.compare())
  {
    null_value=1;
    return 0;
  }
  value= args[2]->val_int();
  null_value= args[2]->null_value;
  return value;
}

String *
Item_func_nullif::str_op(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res;
  if (!cmp.compare())
  {
    null_value=1;
    return 0;
  }
  res= args[2]->val_str(str);
  null_value= args[2]->null_value;
  return res;
}


my_decimal *
Item_func_nullif::decimal_op(my_decimal * decimal_value)
{
  DBUG_ASSERT(fixed == 1);
  my_decimal *res;
  if (!cmp.compare())
  {
    null_value=1;
    return 0;
  }
  res= args[2]->val_decimal(decimal_value);
  null_value= args[2]->null_value;
  return res;
}


bool
Item_func_nullif::date_op(MYSQL_TIME *ltime, uint fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  if (!cmp.compare())
    return (null_value= true);
  return (null_value= args[2]->get_date(ltime, fuzzydate));
}


bool
Item_func_nullif::is_null()
{
  return (null_value= (!cmp.compare() ? 1 : args[2]->null_value));
}


Item_func_case::Item_func_case(THD *thd, List<Item> &list,
                               Item *first_expr_arg, Item *else_expr_arg):
  Item_func_hybrid_field_type(thd), first_expr_num(-1), else_expr_num(-1),
  left_cmp_type(INT_RESULT), case_item(0), m_found_types(0)
{
  ncases= list.elements;
  if (first_expr_arg)
  {
    first_expr_num= list.elements;
    list.push_back(first_expr_arg, thd->mem_root);
  }
  if (else_expr_arg)
  {
    else_expr_num= list.elements;
    list.push_back(else_expr_arg, thd->mem_root);
  }
  set_arguments(thd, list);
  bzero(&cmp_items, sizeof(cmp_items));
}

/**
    Find and return matching items for CASE or ELSE item if all compares
    are failed or NULL if ELSE item isn't defined.

  IMPLEMENTATION
    In order to do correct comparisons of the CASE expression (the expression
    between CASE and the first WHEN) with each WHEN expression several
    comparators are used. One for each result type. CASE expression can be
    evaluated up to # of different result types are used. To check whether
    the CASE expression already was evaluated for a particular result type
    a bit mapped variable value_added_map is used. Result types are mapped
    to it according to their int values i.e. STRING_RESULT is mapped to bit
    0, REAL_RESULT to bit 1, so on.

  @retval
    NULL  Nothing found and there is no ELSE expression defined
  @retval
    item  Found item or ELSE item if defined and all comparisons are
           failed
*/

Item *Item_func_case::find_item(String *str)
{
  uint value_added_map= 0;

  if (first_expr_num == -1)
  {
    for (uint i=0 ; i < ncases ; i+=2)
    {
      // No expression between CASE and the first WHEN
      if (args[i]->val_bool())
	return args[i+1];
      continue;
    }
  }
  else
  {
    /* Compare every WHEN argument with it and return the first match */
    for (uint i=0 ; i < ncases ; i+=2)
    {
      if (args[i]->real_item()->type() == NULL_ITEM)
        continue;
      cmp_type= item_cmp_type(left_cmp_type, args[i]);
      DBUG_ASSERT(cmp_type != ROW_RESULT);
      DBUG_ASSERT(cmp_items[(uint)cmp_type]);
      if (!(value_added_map & (1U << (uint)cmp_type)))
      {
        cmp_items[(uint)cmp_type]->store_value(args[first_expr_num]);
        if ((null_value=args[first_expr_num]->null_value))
          return else_expr_num != -1 ? args[else_expr_num] : 0;
        value_added_map|= 1U << (uint)cmp_type;
      }
      if (!cmp_items[(uint)cmp_type]->cmp(args[i]) && !args[i]->null_value)
        return args[i + 1];
    }
  }
  // No, WHEN clauses all missed, return ELSE expression
  return else_expr_num != -1 ? args[else_expr_num] : 0;
}


String *Item_func_case::str_op(String *str)
{
  DBUG_ASSERT(fixed == 1);
  String *res;
  Item *item=find_item(str);

  if (!item)
  {
    null_value=1;
    return 0;
  }
  null_value= 0;
  if (!(res=item->val_str(str)))
    null_value= 1;
  return res;
}


longlong Item_func_case::int_op()
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String dummy_str(buff,sizeof(buff),default_charset());
  Item *item=find_item(&dummy_str);
  longlong res;

  if (!item)
  {
    null_value=1;
    return 0;
  }
  res=item->val_int();
  null_value=item->null_value;
  return res;
}

double Item_func_case::real_op()
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String dummy_str(buff,sizeof(buff),default_charset());
  Item *item=find_item(&dummy_str);
  double res;

  if (!item)
  {
    null_value=1;
    return 0;
  }
  res= item->val_real();
  null_value=item->null_value;
  return res;
}


my_decimal *Item_func_case::decimal_op(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String dummy_str(buff, sizeof(buff), default_charset());
  Item *item= find_item(&dummy_str);
  my_decimal *res;

  if (!item)
  {
    null_value=1;
    return 0;
  }

  res= item->val_decimal(decimal_value);
  null_value= item->null_value;
  return res;
}


bool Item_func_case::date_op(MYSQL_TIME *ltime, uint fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  char buff[MAX_FIELD_WIDTH];
  String dummy_str(buff, sizeof(buff), default_charset());
  Item *item= find_item(&dummy_str);
  if (!item)
    return (null_value= true);
  return (null_value= item->get_date_with_conversion(ltime, fuzzydate));
}


bool Item_func_case::fix_fields(THD *thd, Item **ref)
{
  /*
    buff should match stack usage from
    Item_func_case::val_int() -> Item_func_case::find_item()
  */
  uchar buff[MAX_FIELD_WIDTH*2+sizeof(String)*2+sizeof(String*)*2+sizeof(double)*2+sizeof(longlong)*2];

  if (!(arg_buffer= (Item**) thd->alloc(sizeof(Item*)*(ncases+1))))
    return TRUE;

  bool res= Item_func::fix_fields(thd, ref);
  /*
    Call check_stack_overrun after fix_fields to be sure that stack variable
    is not optimized away
  */
  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return TRUE;				// Fatal error flag is set!
  return res;
}


void Item_func_case::agg_str_lengths(Item* arg)
{
  fix_char_length(MY_MAX(max_char_length(), arg->max_char_length()));
  set_if_bigger(decimals, arg->decimals);
  unsigned_flag= unsigned_flag && arg->unsigned_flag;
}


void Item_func_case::agg_num_lengths(Item *arg)
{
  uint len= my_decimal_length_to_precision(arg->max_length, arg->decimals,
                                           arg->unsigned_flag) - arg->decimals;
  set_if_bigger(max_length, len); 
  set_if_bigger(decimals, arg->decimals);
  unsigned_flag= unsigned_flag && arg->unsigned_flag; 
}


/**
  Check if (*place) and new_value points to different Items and call
  THD::change_item_tree() if needed.

  This function is a workaround for implementation deficiency in
  Item_func_case. The problem there is that the 'args' attribute contains
  Items from different expressions.
 
  The function must not be used elsewhere and will be remove eventually.
*/

static void change_item_tree_if_needed(THD *thd,
                                       Item **place,
                                       Item *new_value)
{
  if (*place == new_value)
    return;

  thd->change_item_tree(place, new_value);
}


void Item_func_case::fix_length_and_dec()
{
  Item **agg= arg_buffer;
  uint nagg;
  THD *thd= current_thd;

  m_found_types= 0;
  if (else_expr_num == -1 || args[else_expr_num]->maybe_null)
    maybe_null= 1;

  /*
    Aggregate all THEN and ELSE expression types
    and collations when string result
  */
  
  for (nagg= 0 ; nagg < ncases/2 ; nagg++)
    agg[nagg]= args[nagg*2+1];
  
  if (else_expr_num != -1)
    agg[nagg++]= args[else_expr_num];
  
  set_handler_by_field_type(agg_field_type(agg, nagg, true));

  if (Item_func_case::result_type() == STRING_RESULT)
  {
    if (count_string_result_length(Item_func_case::field_type(), agg, nagg))
      return;
    /*
      Copy all THEN and ELSE items back to args[] array.
      Some of the items might have been changed to Item_func_conv_charset.
    */
    for (nagg= 0 ; nagg < ncases / 2 ; nagg++)
      change_item_tree_if_needed(thd, &args[nagg * 2 + 1], agg[nagg]);

    if (else_expr_num != -1)
      change_item_tree_if_needed(thd, &args[else_expr_num], agg[nagg++]);
  }
  else
  {
    collation.set_numeric();
    max_length=0;
    decimals=0;
    unsigned_flag= TRUE;
    for (uint i= 0; i < ncases; i+= 2)
      agg_num_lengths(args[i + 1]);
    if (else_expr_num != -1) 
      agg_num_lengths(args[else_expr_num]);
    max_length= my_decimal_precision_to_length_no_truncation(max_length +
                                                             decimals,
                                                             decimals,
                                                             unsigned_flag);
  }
  
  /*
    Aggregate first expression and all WHEN expression types
    and collations when string comparison
  */
  if (first_expr_num != -1)
  {
    uint i;
    agg[0]= args[first_expr_num];
    left_cmp_type= agg[0]->cmp_type();

    /*
      As the first expression and WHEN expressions
      are intermixed in args[] array THEN and ELSE items,
      extract the first expression and all WHEN expressions into 
      a temporary array, to process them easier.
    */
    for (nagg= 0; nagg < ncases/2 ; nagg++)
      agg[nagg+1]= args[nagg*2];
    nagg++;
    if (!(m_found_types= collect_cmp_types(agg, nagg)))
      return;

    Item *date_arg= 0;
    if (m_found_types & (1U << TIME_RESULT))
      date_arg= find_date_time_item(args, arg_count, 0);

    if (m_found_types & (1U << STRING_RESULT))
    {
      /*
        If we'll do string comparison, we also need to aggregate
        character set and collation for first/WHEN items and
        install converters for some of them to cmp_collation when necessary.
        This is done because cmp_item compatators cannot compare
        strings in two different character sets.
        Some examples when we install converters:

        1. Converter installed for the first expression:

           CASE         latin1_item              WHEN utf16_item THEN ... END

        is replaced to:

           CASE CONVERT(latin1_item USING utf16) WHEN utf16_item THEN ... END

        2. Converter installed for the left WHEN item:

          CASE utf16_item WHEN         latin1_item              THEN ... END

        is replaced to:

           CASE utf16_item WHEN CONVERT(latin1_item USING utf16) THEN ... END
      */
      if (agg_arg_charsets_for_comparison(cmp_collation, agg, nagg))
        return;
      /*
        Now copy first expression and all WHEN expressions back to args[]
        arrray, because some of the items might have been changed to converters
        (e.g. Item_func_conv_charset, or Item_string for constants).
      */
      change_item_tree_if_needed(thd, &args[first_expr_num], agg[0]);

      for (nagg= 0; nagg < ncases / 2; nagg++)
        change_item_tree_if_needed(thd, &args[nagg * 2], agg[nagg + 1]);
    }

    for (i= 0; i <= (uint)TIME_RESULT; i++)
    {
      if (m_found_types & (1U << i) && !cmp_items[i])
      {
        DBUG_ASSERT((Item_result)i != ROW_RESULT);

        if (!(cmp_items[i]=
            cmp_item::get_comparator((Item_result)i, date_arg,
                                     cmp_collation.collation)))
          return;
      }
    }
  }
}


Item* Item_func_case::propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
{
  if (first_expr_num == -1)
  {
    // None of the arguments are in a comparison context
    Item_args::propagate_equal_fields(thd, Context_identity(), cond);
    return this;
  }

  for (uint i= 0; i < arg_count; i++)
  {
    /*
      Even "i" values cover items that are in a comparison context:
        CASE x0 WHEN x1 .. WHEN x2 .. WHEN x3 ..
      Odd "i" values cover items that are not in comparison:
        CASE ... THEN y1 ... THEN y2 ... THEN y3 ... ELSE y4 END
    */
    Item *new_item= 0;
    if ((int) i == first_expr_num) // Then CASE (the switch) argument
    {
      /*
        Cannot replace the CASE (the switch) argument if
        there are multiple comparison types were found, or found a single
        comparison type that is not equal to args[0]->cmp_type().

        - Example: multiple comparison types, can't propagate:
            WHERE CASE str_column
                  WHEN 'string' THEN TRUE
                  WHEN 1 THEN TRUE
                  ELSE FALSE END;

        - Example: a single incompatible comparison type, can't propagate:
            WHERE CASE str_column
                  WHEN DATE'2001-01-01' THEN TRUE
                  ELSE FALSE END;

        - Example: a single incompatible comparison type, can't propagate:
            WHERE CASE str_column
                  WHEN 1 THEN TRUE
                  ELSE FALSE END;

        - Example: a single compatible comparison type, ok to propagate:
            WHERE CASE str_column
                  WHEN 'str1' THEN TRUE
                  WHEN 'str2' THEN TRUE
                  ELSE FALSE END;
      */
      if (m_found_types == (1UL << left_cmp_type))
        new_item= args[i]->propagate_equal_fields(thd,
                                                  Context(
                                                    ANY_SUBST,
                                                    left_cmp_type,
                                                    cmp_collation.collation),
                                                  cond);
    }
    else if ((i % 2) == 0) // WHEN arguments
    {
      /*
        These arguments are in comparison.
        Allow invariants of the same value during propagation.
        Note, as we pass ANY_SUBST, none of the WHEN arguments will be
        replaced to zero-filled constants (only IDENTITY_SUBST allows this).
        Such a change for WHEN arguments would require rebuilding cmp_items.
      */
      Item_result tmp_cmp_type= item_cmp_type(args[first_expr_num], args[i]);
      new_item= args[i]->propagate_equal_fields(thd,
                                                Context(
                                                  ANY_SUBST,
                                                  tmp_cmp_type,
                                                  cmp_collation.collation),
                                                cond);
    }
    else // THEN and ELSE arguments (they are not in comparison)
    {
      new_item= args[i]->propagate_equal_fields(thd, Context_identity(), cond);
    }
    if (new_item && new_item != args[i])
      thd->change_item_tree(&args[i], new_item);
  }
  return this;
}


uint Item_func_case::decimal_precision() const
{
  int max_int_part=0;
  for (uint i=0 ; i < ncases ; i+=2)
    set_if_bigger(max_int_part, args[i+1]->decimal_int_part());

  if (else_expr_num != -1) 
    set_if_bigger(max_int_part, args[else_expr_num]->decimal_int_part());
  return MY_MIN(max_int_part + decimals, DECIMAL_MAX_PRECISION);
}


/**
  @todo
    Fix this so that it prints the whole CASE expression
*/

void Item_func_case::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(case "));
  if (first_expr_num != -1)
  {
    args[first_expr_num]->print(str, query_type);
    str->append(' ');
  }
  for (uint i=0 ; i < ncases ; i+=2)
  {
    str->append(STRING_WITH_LEN("when "));
    args[i]->print(str, query_type);
    str->append(STRING_WITH_LEN(" then "));
    args[i+1]->print(str, query_type);
    str->append(' ');
  }
  if (else_expr_num != -1)
  {
    str->append(STRING_WITH_LEN("else "));
    args[else_expr_num]->print(str, query_type);
    str->append(' ');
  }
  str->append(STRING_WITH_LEN("end)"));
}


void Item_func_case::cleanup()
{
  uint i;
  DBUG_ENTER("Item_func_case::cleanup");
  Item_func::cleanup();
  for (i= 0; i <= (uint)TIME_RESULT; i++)
  {
    delete cmp_items[i];
    cmp_items[i]= 0;
  }
  DBUG_VOID_RETURN;
}


/**
  Coalesce - return first not NULL argument.
*/

String *Item_func_coalesce::str_op(String *str)
{
  DBUG_ASSERT(fixed == 1);
  null_value=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    String *res;
    if ((res=args[i]->val_str(str)))
      return res;
  }
  null_value=1;
  return 0;
}

longlong Item_func_coalesce::int_op()
{
  DBUG_ASSERT(fixed == 1);
  null_value=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    longlong res=args[i]->val_int();
    if (!args[i]->null_value)
      return res;
  }
  null_value=1;
  return 0;
}

double Item_func_coalesce::real_op()
{
  DBUG_ASSERT(fixed == 1);
  null_value=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    double res= args[i]->val_real();
    if (!args[i]->null_value)
      return res;
  }
  null_value=1;
  return 0;
}


bool Item_func_coalesce::date_op(MYSQL_TIME *ltime,uint fuzzydate)
{
  DBUG_ASSERT(fixed == 1);
  null_value= 0;
  for (uint i= 0; i < arg_count; i++)
  {
    bool res= args[i]->get_date_with_conversion(ltime,
                                                fuzzydate & ~TIME_FUZZY_DATES);
    if (!args[i]->null_value)
      return res;
  }
  bzero((char*) ltime,sizeof(*ltime));
  return null_value|= !(fuzzydate & TIME_FUZZY_DATES);
}


my_decimal *Item_func_coalesce::decimal_op(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed == 1);
  null_value= 0;
  for (uint i= 0; i < arg_count; i++)
  {
    my_decimal *res= args[i]->val_decimal(decimal_value);
    if (!args[i]->null_value)
      return res;
  }
  null_value=1;
  return 0;
}


void Item_func_coalesce::fix_length_and_dec()
{
  set_handler_by_field_type(agg_field_type(args, arg_count, true));
  switch (Item_func_coalesce::result_type()) {
  case STRING_RESULT:
    if (count_string_result_length(Item_func_coalesce::field_type(),
                                   args, arg_count))
      return;          
    break;
  case DECIMAL_RESULT:
    count_decimal_length();
    break;
  case REAL_RESULT:
    count_real_length();
    break;
  case INT_RESULT:
    count_only_length(args, arg_count);
    decimals= 0;
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);
  }
}

/****************************************************************************
 Classes and function for the IN operator
****************************************************************************/

/*
  Determine which of the signed longlong arguments is bigger

  SYNOPSIS
    cmp_longs()
      a_val     left argument
      b_val     right argument

  DESCRIPTION
    This function will compare two signed longlong arguments
    and will return -1, 0, or 1 if left argument is smaller than,
    equal to or greater than the right argument.

  RETURN VALUE
    -1          left argument is smaller than the right argument.
    0           left argument is equal to the right argument.
    1           left argument is greater than the right argument.
*/
static inline int cmp_longs (longlong a_val, longlong b_val)
{
  return a_val < b_val ? -1 : a_val == b_val ? 0 : 1;
}


/*
  Determine which of the unsigned longlong arguments is bigger

  SYNOPSIS
    cmp_ulongs()
      a_val     left argument
      b_val     right argument

  DESCRIPTION
    This function will compare two unsigned longlong arguments
    and will return -1, 0, or 1 if left argument is smaller than,
    equal to or greater than the right argument.

  RETURN VALUE
    -1          left argument is smaller than the right argument.
    0           left argument is equal to the right argument.
    1           left argument is greater than the right argument.
*/
static inline int cmp_ulongs (ulonglong a_val, ulonglong b_val)
{
  return a_val < b_val ? -1 : a_val == b_val ? 0 : 1;
}


/*
  Compare two integers in IN value list format (packed_longlong) 

  SYNOPSIS
    cmp_longlong()
      cmp_arg   an argument passed to the calling function (my_qsort2)
      a         left argument
      b         right argument

  DESCRIPTION
    This function will compare two integer arguments in the IN value list
    format and will return -1, 0, or 1 if left argument is smaller than,
    equal to or greater than the right argument.
    It's used in sorting the IN values list and finding an element in it.
    Depending on the signedness of the arguments cmp_longlong() will
    compare them as either signed (using cmp_longs()) or unsigned (using
    cmp_ulongs()).

  RETURN VALUE
    -1          left argument is smaller than the right argument.
    0           left argument is equal to the right argument.
    1           left argument is greater than the right argument.
*/
int cmp_longlong(void *cmp_arg, 
                 in_longlong::packed_longlong *a,
                 in_longlong::packed_longlong *b)
{
  if (a->unsigned_flag != b->unsigned_flag)
  { 
    /* 
      One of the args is unsigned and is too big to fit into the 
      positive signed range. Report no match.
    */  
    if ((a->unsigned_flag && ((ulonglong) a->val) > (ulonglong) LONGLONG_MAX)
        ||
        (b->unsigned_flag && ((ulonglong) b->val) > (ulonglong) LONGLONG_MAX))
      return a->unsigned_flag ? 1 : -1;
    /*
      Although the signedness differs both args can fit into the signed 
      positive range. Make them signed and compare as usual.
    */  
    return cmp_longs(a->val, b->val);
  }
  if (a->unsigned_flag)
    return cmp_ulongs((ulonglong) a->val, (ulonglong) b->val);
  return cmp_longs(a->val, b->val);
}

static int cmp_double(void *cmp_arg, double *a,double *b)
{
  return *a < *b ? -1 : *a == *b ? 0 : 1;
}

static int cmp_row(void *cmp_arg, cmp_item_row *a, cmp_item_row *b)
{
  return a->compare(b);
}


static int cmp_decimal(void *cmp_arg, my_decimal *a, my_decimal *b)
{
  /*
    We need call of fixing buffer pointer, because fast sort just copy
    decimal buffers in memory and pointers left pointing on old buffer place
  */
  a->fix_buffer_pointer();
  b->fix_buffer_pointer();
  return my_decimal_cmp(a, b);
}


int in_vector::find(Item *item)
{
  uchar *result=get_value(item);
  if (!result || !used_count)
    return 0;				// Null value

  uint start,end;
  start=0; end=used_count-1;
  while (start != end)
  {
    uint mid=(start+end+1)/2;
    int res;
    if ((res=(*compare)(collation, base+mid*size, result)) == 0)
      return 1;
    if (res < 0)
      start=mid;
    else
      end=mid-1;
  }
  return (int) ((*compare)(collation, base+start*size, result) == 0);
}

in_string::in_string(uint elements,qsort2_cmp cmp_func, CHARSET_INFO *cs)
  :in_vector(elements, sizeof(String), cmp_func, cs),
   tmp(buff, sizeof(buff), &my_charset_bin)
{}

in_string::~in_string()
{
  if (base)
  {
    // base was allocated with help of sql_alloc => following is OK
    for (uint i=0 ; i < count ; i++)
      ((String*) base)[i].free();
  }
}

void in_string::set(uint pos,Item *item)
{
  String *str=((String*) base)+pos;
  String *res=item->val_str(str);
  if (res && res != str)
  {
    if (res->uses_buffer_owned_by(str))
      res->copy();
    if (item->type() == Item::FUNC_ITEM)
      str->copy(*res);
    else
      *str= *res;
  }
  if (!str->charset())
  {
    CHARSET_INFO *cs;
    if (!(cs= item->collation.collation))
      cs= &my_charset_bin;		// Should never happen for STR items
    str->set_charset(cs);
  }
}


uchar *in_string::get_value(Item *item)
{
  return (uchar*) item->val_str(&tmp);
}

Item *in_string::create_item(THD *thd)
{
  return new (thd->mem_root) Item_string_for_in_vector(thd, collation);
}


in_row::in_row(THD *thd, uint elements, Item * item)
{
  base= (char*) new (thd->mem_root) cmp_item_row[count= elements];
  size= sizeof(cmp_item_row);
  compare= (qsort2_cmp) cmp_row;
  /*
    We need to reset these as otherwise we will call sort() with
    uninitialized (even if not used) elements
  */
  used_count= elements;
  collation= 0;
}

in_row::~in_row()
{
  if (base)
    delete [] (cmp_item_row*) base;
}

uchar *in_row::get_value(Item *item)
{
  tmp.store_value(item);
  if (item->is_null())
    return 0;
  return (uchar *)&tmp;
}

void in_row::set(uint pos, Item *item)
{
  DBUG_ENTER("in_row::set");
  DBUG_PRINT("enter", ("pos: %u  item: 0x%lx", pos, (ulong) item));
  ((cmp_item_row*) base)[pos].store_value_by_template(current_thd, &tmp, item);
  DBUG_VOID_RETURN;
}

in_longlong::in_longlong(uint elements)
  :in_vector(elements,sizeof(packed_longlong),(qsort2_cmp) cmp_longlong, 0)
{}

void in_longlong::set(uint pos,Item *item)
{
  struct packed_longlong *buff= &((packed_longlong*) base)[pos];
  
  buff->val= item->val_int();
  buff->unsigned_flag= item->unsigned_flag;
}

uchar *in_longlong::get_value(Item *item)
{
  tmp.val= item->val_int();
  if (item->null_value)
    return 0;
  tmp.unsigned_flag= item->unsigned_flag;
  return (uchar*) &tmp;
}

Item *in_longlong::create_item(THD *thd)
{ 
  /* 
     We're created a signed INT, this may not be correct in 
     general case (see BUG#19342).
  */
  return new (thd->mem_root) Item_int(thd, (longlong)0);
}


void in_datetime::set(uint pos,Item *item)
{
  struct packed_longlong *buff= &((packed_longlong*) base)[pos];

  buff->val= item->val_temporal_packed(warn_item);
  buff->unsigned_flag= 1L;
}

uchar *in_datetime::get_value(Item *item)
{
  bool is_null;
  Item **tmp_item= lval_cache ? &lval_cache : &item;
  enum_field_types f_type=
    tmp_item[0]->field_type_for_temporal_comparison(warn_item);
  tmp.val= get_datetime_value(thd, &tmp_item, &lval_cache, f_type, &is_null);
  if (item->null_value)
    return 0;
  tmp.unsigned_flag= 1L;
  return (uchar*) &tmp;
}

Item *in_datetime::create_item(THD *thd)
{ 
  return new (thd->mem_root) Item_datetime(thd);
}


in_double::in_double(uint elements)
  :in_vector(elements,sizeof(double),(qsort2_cmp) cmp_double, 0)
{}

void in_double::set(uint pos,Item *item)
{
  ((double*) base)[pos]= item->val_real();
}

uchar *in_double::get_value(Item *item)
{
  tmp= item->val_real();
  if (item->null_value)
    return 0;					/* purecov: inspected */
  return (uchar*) &tmp;
}

Item *in_double::create_item(THD *thd)
{ 
  return new (thd->mem_root) Item_float(thd, 0.0, 0);
}


in_decimal::in_decimal(uint elements)
  :in_vector(elements, sizeof(my_decimal),(qsort2_cmp) cmp_decimal, 0)
{}


void in_decimal::set(uint pos, Item *item)
{
  /* as far as 'item' is constant, we can store reference on my_decimal */
  my_decimal *dec= ((my_decimal *)base) + pos;
  dec->len= DECIMAL_BUFF_LENGTH;
  dec->fix_buffer_pointer();
  my_decimal *res= item->val_decimal(dec);
  /* if item->val_decimal() is evaluated to NULL then res == 0 */ 
  if (!item->null_value && res != dec)
    my_decimal2decimal(res, dec);
}


uchar *in_decimal::get_value(Item *item)
{
  my_decimal *result= item->val_decimal(&val);
  if (item->null_value)
    return 0;
  return (uchar *)result;
}

Item *in_decimal::create_item(THD *thd)
{ 
  return new (thd->mem_root) Item_decimal(thd, 0, FALSE);
}


cmp_item* cmp_item::get_comparator(Item_result type, Item *warn_item,
                                   CHARSET_INFO *cs)
{
  switch (type) {
  case STRING_RESULT:
    return new cmp_item_sort_string(cs);
  case INT_RESULT:
    return new cmp_item_int;
  case REAL_RESULT:
    return new cmp_item_real;
  case ROW_RESULT:
    return new cmp_item_row;
  case DECIMAL_RESULT:
    return new cmp_item_decimal;
  case TIME_RESULT:
    DBUG_ASSERT(warn_item);
    return new cmp_item_datetime(warn_item);
  }
  return 0; // to satisfy compiler :)
}


cmp_item* cmp_item_sort_string::make_same()
{
  return new cmp_item_sort_string_in_static(cmp_charset);
}

cmp_item* cmp_item_int::make_same()
{
  return new cmp_item_int();
}

cmp_item* cmp_item_real::make_same()
{
  return new cmp_item_real();
}

cmp_item* cmp_item_row::make_same()
{
  return new cmp_item_row();
}


cmp_item_row::~cmp_item_row()
{
  DBUG_ENTER("~cmp_item_row");
  DBUG_PRINT("enter",("this: 0x%lx", (long) this));
  if (comparators)
  {
    for (uint i= 0; i < n; i++)
    {
      if (comparators[i])
	delete comparators[i];
    }
  }
  DBUG_VOID_RETURN;
}


void cmp_item_row::alloc_comparators()
{
  if (!comparators)
    comparators= (cmp_item **) current_thd->calloc(sizeof(cmp_item *)*n);
}


void cmp_item_row::store_value(Item *item)
{
  DBUG_ENTER("cmp_item_row::store_value");
  n= item->cols();
  alloc_comparators();
  if (comparators)
  {
    item->bring_value();
    item->null_value= 0;
    for (uint i=0; i < n; i++)
    {
      if (!comparators[i])
      {
        DBUG_ASSERT(item->element_index(i)->cmp_type() != TIME_RESULT);
        if (!(comparators[i]=
              cmp_item::get_comparator(item->element_index(i)->result_type(), 0,
                                       item->element_index(i)->collation.collation)))
	  break;					// new failed
      }
      comparators[i]->store_value(item->element_index(i));
      item->null_value|= item->element_index(i)->null_value;
    }
  }
  DBUG_VOID_RETURN;
}


void cmp_item_row::store_value_by_template(THD *thd, cmp_item *t, Item *item)
{
  cmp_item_row *tmpl= (cmp_item_row*) t;
  if (tmpl->n != item->cols())
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), tmpl->n);
    return;
  }
  n= tmpl->n;
  if ((comparators= (cmp_item **) thd->alloc(sizeof(cmp_item *)*n)))
  {
    item->bring_value();
    item->null_value= 0;
    for (uint i=0; i < n; i++)
    {
      if (!(comparators[i]= tmpl->comparators[i]->make_same()))
	break;					// new failed
      comparators[i]->store_value_by_template(thd, tmpl->comparators[i],
					      item->element_index(i));
      item->null_value|= item->element_index(i)->null_value;
    }
  }
}


int cmp_item_row::cmp(Item *arg)
{
  arg->null_value= 0;
  if (arg->cols() != n)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), n);
    return 1;
  }
  bool was_null= 0;
  arg->bring_value();
  for (uint i=0; i < n; i++)
  {
    if (comparators[i]->cmp(arg->element_index(i)))
    {
      if (!arg->element_index(i)->null_value)
	return 1;
      was_null= 1;
    }
  }
  return (arg->null_value= was_null);
}


int cmp_item_row::compare(cmp_item *c)
{
  cmp_item_row *l_cmp= (cmp_item_row *) c;
  for (uint i=0; i < n; i++)
  {
    int res;
    if ((res= comparators[i]->compare(l_cmp->comparators[i])))
      return res;
  }
  return 0;
}


void cmp_item_decimal::store_value(Item *item)
{
  my_decimal *val= item->val_decimal(&value);
  /* val may be zero if item is nnull */
  if (val && val != &value)
    my_decimal2decimal(val, &value);
}


int cmp_item_decimal::cmp(Item *arg)
{
  my_decimal tmp_buf, *tmp= arg->val_decimal(&tmp_buf);
  if (arg->null_value)
    return 1;
  return my_decimal_cmp(&value, tmp);
}


int cmp_item_decimal::compare(cmp_item *arg)
{
  cmp_item_decimal *l_cmp= (cmp_item_decimal*) arg;
  return my_decimal_cmp(&value, &l_cmp->value);
}


cmp_item* cmp_item_decimal::make_same()
{
  return new cmp_item_decimal();
}


void cmp_item_datetime::store_value(Item *item)
{
  bool is_null;
  Item **tmp_item= lval_cache ? &lval_cache : &item;
  enum_field_types f_type=
    tmp_item[0]->field_type_for_temporal_comparison(warn_item);
  value= get_datetime_value(thd, &tmp_item, &lval_cache, f_type, &is_null);
}


int cmp_item_datetime::cmp(Item *arg)
{
  return value != arg->val_temporal_packed(warn_item);
}


int cmp_item_datetime::compare(cmp_item *ci)
{
  cmp_item_datetime *l_cmp= (cmp_item_datetime *)ci;
  return (value < l_cmp->value) ? -1 : ((value == l_cmp->value) ? 0 : 1);
}


cmp_item *cmp_item_datetime::make_same()
{
  return new cmp_item_datetime(warn_item);
}


bool Item_func_in::count_sargable_conds(uchar *arg)
{
  ((SELECT_LEX*) arg)->cond_count++;
  return 0;
}


bool Item_func_in::nulls_in_row()
{
  Item **arg,**arg_end;
  for (arg= args+1, arg_end= args+arg_count; arg != arg_end ; arg++)
  {
    if ((*arg)->null_inside())
      return 1;
  }
  return 0;
}


/**
  Perform context analysis of an IN item tree.

    This function performs context analysis (name resolution) and calculates
    various attributes of the item tree with Item_func_in as its root.
    The function saves in ref the pointer to the item or to a newly created
    item that is considered as a replacement for the original one.

  @param thd     reference to the global context of the query thread
  @param ref     pointer to Item* variable where pointer to resulting "fixed"
                 item is to be assigned

  @note
    Let T0(e)/T1(e) be the value of not_null_tables(e) when e is used on
    a predicate/function level. Then it's easy to show that:
    @verbatim
      T0(e IN(e1,...,en))     = union(T1(e),intersection(T1(ei)))
      T1(e IN(e1,...,en))     = union(T1(e),intersection(T1(ei)))
      T0(e NOT IN(e1,...,en)) = union(T1(e),union(T1(ei)))
      T1(e NOT IN(e1,...,en)) = union(T1(e),intersection(T1(ei)))
    @endverbatim

  @retval
    0   ok
  @retval
    1   got error
*/

bool
Item_func_in::fix_fields(THD *thd, Item **ref)
{

  if (Item_func_opt_neg::fix_fields(thd, ref))
    return 1;

  return 0;
}


bool
Item_func_in::eval_not_null_tables(uchar *opt_arg)
{
  Item **arg, **arg_end;

  if (Item_func_opt_neg::eval_not_null_tables(NULL))
    return 1;

  /* not_null_tables_cache == union(T1(e),union(T1(ei))) */
  if (pred_level && negated)
    return 0;

  /* not_null_tables_cache = union(T1(e),intersection(T1(ei))) */
  not_null_tables_cache= ~(table_map) 0;
  for (arg= args + 1, arg_end= args + arg_count; arg != arg_end; arg++)
    not_null_tables_cache&= (*arg)->not_null_tables();
  not_null_tables_cache|= (*args)->not_null_tables();
  return 0;
}


void Item_func_in::fix_after_pullout(st_select_lex *new_parent, Item **ref)
{
  /* This will re-calculate attributes of the arguments */
  Item_func_opt_neg::fix_after_pullout(new_parent, ref);
  /* Then, re-calculate not_null_tables_cache according to our special rules */
  eval_not_null_tables(NULL);
}

static int srtcmp_in(CHARSET_INFO *cs, const String *x,const String *y)
{
  return cs->coll->strnncollsp(cs,
                               (uchar *) x->ptr(),x->length(),
                               (uchar *) y->ptr(),y->length(), 0);
}

void Item_func_in::fix_length_and_dec()
{
  Item **arg, **arg_end;
  bool const_itm= 1;
  THD *thd= current_thd;
  /* TRUE <=> arguments values will be compared as DATETIMEs. */
  Item *date_arg= 0;
  uint found_types= 0;
  uint type_cnt= 0, i;
  m_compare_type= STRING_RESULT;
  left_cmp_type= args[0]->cmp_type();
  if (!(found_types= collect_cmp_types(args, arg_count, true)))
    return;
  
  for (arg= args + 1, arg_end= args + arg_count; arg != arg_end ; arg++)
  {
    if (!arg[0]->const_item())
    {
      const_itm= 0;
      break;
    }
  }
  for (i= 0; i <= (uint)TIME_RESULT; i++)
  {
    if (found_types & (1U << i))
    {
      (type_cnt)++;
      m_compare_type= (Item_result) i;
    }
  }

  if (type_cnt == 1)
  {
    if (m_compare_type == STRING_RESULT &&
        agg_arg_charsets_for_comparison(cmp_collation, args, arg_count))
      return;
    arg_types_compatible= TRUE;

    if (m_compare_type == ROW_RESULT)
    {
      uint cols= args[0]->cols();
      cmp_item_row *cmp= 0;

      if (const_itm && !nulls_in_row())
      {
        array= new (thd->mem_root) in_row(thd, arg_count-1, 0);
        cmp= &((in_row*)array)->tmp;
      }
      else
      {
        if (!(cmp= new (thd->mem_root) cmp_item_row))
          return;
        cmp_items[ROW_RESULT]= cmp;
      }
      cmp->n= cols;
      cmp->alloc_comparators();

      for (uint col= 0; col < cols; col++)
      {
        date_arg= find_date_time_item(args, arg_count, col);
        if (date_arg)
        {
          cmp_item **cmp= 0;
          if (array)
            cmp= ((in_row*)array)->tmp.comparators + col;
          else
            cmp= ((cmp_item_row*)cmp_items[ROW_RESULT])->comparators + col;
          *cmp= new (thd->mem_root) cmp_item_datetime(date_arg);
        }
      }
    }
  }
  /*
    Row item with NULLs inside can return NULL or FALSE =>
    they can't be processed as static
  */
  if (type_cnt == 1 && const_itm && !nulls_in_row())
  {
    /*
      IN must compare INT columns and constants as int values (the same
      way as equality does).
      So we must check here if the column on the left and all the constant 
      values on the right can be compared as integers and adjust the 
      comparison type accordingly.

      See the comment about the similar block in Item_bool_func2
    */  
    if (args[0]->real_item()->type() == FIELD_ITEM &&
        !thd->lex->is_view_context_analysis() && m_compare_type != INT_RESULT)
    {
      Item_field *field_item= (Item_field*) (args[0]->real_item());
      if (field_item->field_type() ==  MYSQL_TYPE_LONGLONG ||
          field_item->field_type() ==  MYSQL_TYPE_YEAR)
      {
        bool all_converted= TRUE;
        for (arg=args+1, arg_end=args+arg_count; arg != arg_end ; arg++)
        {
           if (!convert_const_to_int(thd, field_item, &arg[0]))
            all_converted= FALSE;
        }
        if (all_converted)
          m_compare_type= INT_RESULT;
      }
    }
    switch (m_compare_type) {
    case STRING_RESULT:
      array=new (thd->mem_root) in_string(arg_count-1,(qsort2_cmp) srtcmp_in, 
                                          cmp_collation.collation);
      break;
    case INT_RESULT:
      array= new (thd->mem_root) in_longlong(arg_count-1);
      break;
    case REAL_RESULT:
      array= new (thd->mem_root) in_double(arg_count-1);
      break;
    case ROW_RESULT:
      /*
        The row comparator was created at the beginning but only DATETIME
        items comparators were initialized. Call store_value() to setup
        others.
      */
      ((in_row*)array)->tmp.store_value(args[0]);
      break;
    case DECIMAL_RESULT:
      array= new (thd->mem_root) in_decimal(arg_count - 1);
      break;
    case TIME_RESULT:
      date_arg= find_date_time_item(args, arg_count, 0);
      array= new (thd->mem_root) in_datetime(date_arg, arg_count - 1);
      break;
    }
    if (array && !(thd->is_fatal_error))		// If not EOM
    {
      uint j=0;
      for (uint i=1 ; i < arg_count ; i++)
      {
        array->set(j,args[i]);
        if (!args[i]->null_value)                      // Skip NULL values
          j++;
        else
          have_null= 1;
      }
      if ((array->used_count= j))
	array->sort();
    }
  }
  else
  {
    if (found_types & (1U << TIME_RESULT))
      date_arg= find_date_time_item(args, arg_count, 0);
    if (found_types & (1U << STRING_RESULT) &&
        agg_arg_charsets_for_comparison(cmp_collation, args, arg_count))
      return;
    for (i= 0; i <= (uint) TIME_RESULT; i++)
    {
      if (found_types & (1U << i) && !cmp_items[i])
      {
        if (!cmp_items[i] && !(cmp_items[i]=
            cmp_item::get_comparator((Item_result)i, date_arg,
                                     cmp_collation.collation)))
          return;
      }
    }
  }
  max_length= 1;
}


void Item_func_in::print(String *str, enum_query_type query_type)
{
  str->append('(');
  args[0]->print(str, query_type);
  if (negated)
    str->append(STRING_WITH_LEN(" not"));
  str->append(STRING_WITH_LEN(" in ("));
  print_args(str, 1, query_type);
  str->append(STRING_WITH_LEN("))"));
}


/*
  Evaluate the function and return its value.

  SYNOPSIS
    val_int()

  DESCRIPTION
    Evaluate the function and return its value.

  IMPLEMENTATION
    If the array object is defined then the value of the function is
    calculated by means of this array.
    Otherwise several cmp_item objects are used in order to do correct
    comparison of left expression and an expression from the values list.
    One cmp_item object correspond to one used comparison type. Left
    expression can be evaluated up to number of different used comparison
    types. A bit mapped variable value_added_map is used to check whether
    the left expression already was evaluated for a particular result type.
    Result types are mapped to it according to their integer values i.e.
    STRING_RESULT is mapped to bit 0, REAL_RESULT to bit 1, so on.

  RETURN
    Value of the function
*/

longlong Item_func_in::val_int()
{
  cmp_item *in_item;
  DBUG_ASSERT(fixed == 1);
  uint value_added_map= 0;
  if (array)
  {
    int tmp=array->find(args[0]);
    null_value=args[0]->null_value || (!tmp && have_null);
    return (longlong) (!null_value && tmp != negated);
  }

  if ((null_value= args[0]->real_item()->type() == NULL_ITEM))
    return 0;

  have_null= 0;
  for (uint i= 1 ; i < arg_count ; i++)
  {
    if (args[i]->real_item()->type() == NULL_ITEM)
    {
      have_null= TRUE;
      continue;
    }
    Item_result cmp_type= item_cmp_type(left_cmp_type, args[i]);
    in_item= cmp_items[(uint)cmp_type];
    DBUG_ASSERT(in_item);
    if (!(value_added_map & (1U << (uint)cmp_type)))
    {
      in_item->store_value(args[0]);
      if ((null_value= args[0]->null_value))
        return 0;
      value_added_map|= 1U << (uint)cmp_type;
    }
    if (!in_item->cmp(args[i]) && !args[i]->null_value)
      return (longlong) (!negated);
    have_null|= args[i]->null_value;
  }

  null_value= have_null;
  return (longlong) (!null_value && negated);
}


longlong Item_func_bit_or::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong arg1= (ulonglong) args[0]->val_int();
  if (args[0]->null_value)
  {
    null_value=1; /* purecov: inspected */
    return 0; /* purecov: inspected */
  }
  ulonglong arg2= (ulonglong) args[1]->val_int();
  if (args[1]->null_value)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (longlong) (arg1 | arg2);
}


longlong Item_func_bit_and::val_int()
{
  DBUG_ASSERT(fixed == 1);
  ulonglong arg1= (ulonglong) args[0]->val_int();
  if (args[0]->null_value)
  {
    null_value=1; /* purecov: inspected */
    return 0; /* purecov: inspected */
  }
  ulonglong arg2= (ulonglong) args[1]->val_int();
  if (args[1]->null_value)
  {
    null_value=1; /* purecov: inspected */
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) (arg1 & arg2);
}

Item_cond::Item_cond(THD *thd, Item_cond *item)
  :Item_bool_func(thd, item),
   abort_on_null(item->abort_on_null),
   and_tables_cache(item->and_tables_cache)
{
  /*
    item->list will be copied by copy_andor_arguments() call
  */
}


Item_cond::Item_cond(THD *thd, Item *i1, Item *i2):
  Item_bool_func(thd), abort_on_null(0)
{
  list.push_back(i1, thd->mem_root);
  list.push_back(i2, thd->mem_root);
}


Item *Item_cond_and::copy_andor_structure(THD *thd)
{
  Item_cond_and *item;
  if ((item= new (thd->mem_root) Item_cond_and(thd, this)))
    item->copy_andor_arguments(thd, this);
  return item;
}


void Item_cond::copy_andor_arguments(THD *thd, Item_cond *item)
{
  List_iterator_fast<Item> li(item->list);
  while (Item *it= li++)
    list.push_back(it->copy_andor_structure(thd), thd->mem_root);
}


bool
Item_cond::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  List_iterator<Item> li(list);
  Item *item;
  uchar buff[sizeof(char*)];			// Max local vars in function
  not_null_tables_cache= 0;
  used_tables_and_const_cache_init();

  /*
    and_table_cache is the value that Item_cond_or() returns for
    not_null_tables()
  */
  and_tables_cache= ~(table_map) 0;

  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    return TRUE;				// Fatal error flag is set!
  /*
    The following optimization reduces the depth of an AND-OR tree.
    E.g. a WHERE clause like
      F1 AND (F2 AND (F2 AND F4))
    is parsed into a tree with the same nested structure as defined
    by braces. This optimization will transform such tree into
      AND (F1, F2, F3, F4).
    Trees of OR items are flattened as well:
      ((F1 OR F2) OR (F3 OR F4))   =>   OR (F1, F2, F3, F4)
    Items for removed AND/OR levels will dangle until the death of the
    entire statement.
    The optimization is currently prepared statements and stored procedures
    friendly as it doesn't allocate any memory and its effects are durable
    (i.e. do not depend on PS/SP arguments).
  */
  while ((item=li++))
  {
    while (item->type() == Item::COND_ITEM &&
	   ((Item_cond*) item)->functype() == functype() &&
           !((Item_cond*) item)->list.is_empty())
    {						// Identical function
      li.replace(((Item_cond*) item)->list);
      ((Item_cond*) item)->list.empty();
      item= *li.ref();				// new current item
    }
    if (abort_on_null)
      item->top_level_item();

    /*
      replace degraded condition:
        was:    <field>
        become: <field> = 1
    */
    if (item->type() == FIELD_ITEM)
    {
      Query_arena backup, *arena;
      Item *new_item;
      arena= thd->activate_stmt_arena_if_needed(&backup);
      if ((new_item= new (thd->mem_root) Item_func_ne(thd, item, new (thd->mem_root) Item_int(thd, 0, 1))))
        li.replace(item= new_item);
      if (arena)
        thd->restore_active_arena(arena, &backup);
    }

    // item can be substituted in fix_fields
    if ((!item->fixed &&
	 item->fix_fields(thd, li.ref())) ||
	(item= *li.ref())->check_cols(1))
      return TRUE; /* purecov: inspected */
    used_tables_cache|=     item->used_tables();
    if (item->const_item())
    {
      if (!item->is_expensive() && !cond_has_datetime_is_null(item) && 
          item->val_int() == 0)
      {
        /* 
          This is "... OR false_cond OR ..." 
          In this case, false_cond has no effect on cond_or->not_null_tables()
        */
      }
      else
      {
        /* 
          This is  "... OR const_cond OR ..."
          In this case, cond_or->not_null_tables()=0, because the condition
          const_cond might evaluate to true (regardless of whether some tables
          were NULL-complemented).
        */
        and_tables_cache= (table_map) 0;
      }
    }
    else
    {
      table_map tmp_table_map= item->not_null_tables();
      not_null_tables_cache|= tmp_table_map;
      and_tables_cache&= tmp_table_map;

      const_item_cache= FALSE;
    } 
  
    with_sum_func=	    with_sum_func || item->with_sum_func;
    with_field=             with_field || item->with_field;
    with_subselect|=        item->has_subquery();
    if (item->maybe_null)
      maybe_null=1;
  }
  fix_length_and_dec();
  fixed= 1;
  return FALSE;
}


bool
Item_cond::eval_not_null_tables(uchar *opt_arg)
{
  Item *item;
  List_iterator<Item> li(list);
  not_null_tables_cache= (table_map) 0;
  and_tables_cache= ~(table_map) 0;
  while ((item=li++))
  {
    table_map tmp_table_map;
    if (item->const_item())
    {
      if (!item->is_expensive() && !cond_has_datetime_is_null(item) && 
          item->val_int() == 0)
      {
        /* 
          This is "... OR false_cond OR ..." 
          In this case, false_cond has no effect on cond_or->not_null_tables()
        */
      }
      else
      {
        /* 
          This is  "... OR const_cond OR ..."
          In this case, cond_or->not_null_tables()=0, because the condition
          some_cond_or might be true regardless of what tables are 
          NULL-complemented.
        */
        and_tables_cache= (table_map) 0;
      }
    }
    else
    {
      tmp_table_map= item->not_null_tables();
      not_null_tables_cache|= tmp_table_map;
      and_tables_cache&= tmp_table_map;
    }
  }
  return 0;
}


void Item_cond::fix_after_pullout(st_select_lex *new_parent, Item **ref)
{
  List_iterator<Item> li(list);
  Item *item;

  used_tables_and_const_cache_init();

  and_tables_cache= ~(table_map) 0; // Here and below we do as fix_fields does
  not_null_tables_cache= 0;

  while ((item=li++))
  {
    table_map tmp_table_map;
    item->fix_after_pullout(new_parent, li.ref());
    item= *li.ref();
    used_tables_and_const_cache_join(item);

    if (item->const_item())
      and_tables_cache= (table_map) 0;
    else
    {
      tmp_table_map= item->not_null_tables();
      not_null_tables_cache|= tmp_table_map;
      and_tables_cache&= tmp_table_map;
      const_item_cache= FALSE;
    }  
  }
}


bool Item_cond::walk(Item_processor processor, bool walk_subquery, uchar *arg)
{
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
    if (item->walk(processor, walk_subquery, arg))
      return 1;
  return Item_func::walk(processor, walk_subquery, arg);
}

bool Item_cond_and::walk_top_and(Item_processor processor, uchar *arg)
{
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
    if (item->walk_top_and(processor, arg))
      return 1;
  return Item_cond::walk_top_and(processor, arg);
}


/**
  Transform an Item_cond object with a transformer callback function.
  
    The function recursively applies the transform method to each
     member item of the condition list.
    If the call of the method for a member item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_cond object. 
     
  @param transformer   the transformer callback function to be applied to
                       the nodes of the tree of the object
  @param arg           parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node 
*/

Item *Item_cond::transform(THD *thd, Item_transformer transformer, uchar *arg)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  List_iterator<Item> li(list);
  Item *item;
  while ((item= li++))
  {
    Item *new_item= item->transform(thd, transformer, arg);
    if (!new_item)
      return 0;

    /*
      THD::change_item_tree() should be called only if the tree was
      really transformed, i.e. when a new item has been created.
      Otherwise we'll be allocating a lot of unnecessary memory for
      change records at each execution.
    */
    if (new_item != item)
      thd->change_item_tree(li.ref(), new_item);
  }
  return Item_func::transform(thd, transformer, arg);
}


/**
  Compile Item_cond object with a processor and a transformer
  callback functions.
  
    First the function applies the analyzer to the root node of
    the Item_func object. Then if the analyzer succeeeds (returns TRUE)
    the function recursively applies the compile method to member
    item of the condition list.
    If the call of the method for a member item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_cond object. 
     
  @param analyzer      the analyzer callback function to be applied to the
                       nodes of the tree of the object
  @param[in,out] arg_p parameter to be passed to the analyzer
  @param transformer   the transformer callback function to be applied to the
                       nodes of the tree of the object
  @param arg_t         parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node 
*/

Item *Item_cond::compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                         Item_transformer transformer, uchar *arg_t)
{
  if (!(this->*analyzer)(arg_p))
    return 0;
  
  List_iterator<Item> li(list);
  Item *item;
  while ((item= li++))
  {
    /* 
      The same parameter value of arg_p must be passed
      to analyze any argument of the condition formula.
    */   
    uchar *arg_v= *arg_p;
    Item *new_item= item->compile(thd, analyzer, &arg_v, transformer, arg_t);
    if (new_item && new_item != item)
      thd->change_item_tree(li.ref(), new_item);
  }
  return Item_func::transform(thd, transformer, arg_t);
}


Item *Item_cond::propagate_equal_fields(THD *thd,
                                        const Context &ctx,
                                        COND_EQUAL *cond)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());
  DBUG_ASSERT(arg_count == 0);
  List_iterator<Item> li(list);
  Item *item;
  while ((item= li++))
  {
    /*
      The exact value of the second parameter to propagate_equal_fields()
      is not important at this point. Item_func derivants will create and
      pass their own context to the arguments.
    */
    Item *new_item= item->propagate_equal_fields(thd, Context_boolean(), cond);
    if (new_item && new_item != item)
      thd->change_item_tree(li.ref(), new_item);
  }
  return this;
}

void Item_cond::traverse_cond(Cond_traverser traverser,
                              void *arg, traverse_order order)
{
  List_iterator<Item> li(list);
  Item *item;

  switch(order) {
  case(PREFIX):
    (*traverser)(this, arg);
    while ((item= li++))
    {
      item->traverse_cond(traverser, arg, order);
    }
    (*traverser)(NULL, arg);
    break;
  case(POSTFIX):
    while ((item= li++))
    {
      item->traverse_cond(traverser, arg, order);
    }
    (*traverser)(this, arg);
  }
}

/**
  Move SUM items out from item tree and replace with reference.

  The split is done to get an unique item for each SUM function
  so that we can easily find and calculate them.
  (Calculation done by update_sum_func() and copy_sum_funcs() in
  sql_select.cc)

  @param thd			Thread handler
  @param ref_pointer_array	Pointer to array of reference fields
  @param fields		All fields in select

  @note
    This function is run on all expression (SELECT list, WHERE, HAVING etc)
    that have or refer (HAVING) to a SUM expression.
*/

void Item_cond::split_sum_func(THD *thd, Item **ref_pointer_array,
                               List<Item> &fields, uint flags)
{
  List_iterator<Item> li(list);
  Item *item;
  while ((item= li++))
    item->split_sum_func2(thd, ref_pointer_array, fields, li.ref(),
                          flags | SPLIT_SUM_SKIP_REGISTERED);
}


table_map
Item_cond::used_tables() const
{						// This caches used_tables
  return used_tables_cache;
}


void Item_cond::print(String *str, enum_query_type query_type)
{
  str->append('(');
  List_iterator_fast<Item> li(list);
  Item *item;
  if ((item=li++))
    item->print(str, query_type);
  while ((item=li++))
  {
    str->append(' ');
    str->append(func_name());
    str->append(' ');
    item->print(str, query_type);
  }
  str->append(')');
}


void Item_cond::neg_arguments(THD *thd)
{
  List_iterator<Item> li(list);
  Item *item;
  while ((item= li++))		/* Apply not transformation to the arguments */
  {
    Item *new_item= item->neg_transformer(thd);
    if (!new_item)
    {
      if (!(new_item= new (thd->mem_root) Item_func_not(thd, item)))
	return;					// Fatal OEM error
    }
    (void) li.replace(new_item);
  }
}


void Item_cond_and::mark_as_condition_AND_part(TABLE_LIST *embedding)
{
  List_iterator<Item> li(list);
  Item *item;
  while ((item=li++))
  {
    item->mark_as_condition_AND_part(embedding);
  }
}

/**
  Evaluation of AND(expr, expr, expr ...).

  @note
    abort_if_null is set for AND expressions for which we don't care if the
    result is NULL or 0. This is set for:
    - WHERE clause
    - HAVING clause
    - IF(expression)

  @retval
    1  If all expressions are true
  @retval
    0  If all expressions are false or if we find a NULL expression and
       'abort_on_null' is set.
  @retval
    NULL if all expression are either 1 or NULL
*/


longlong Item_cond_and::val_int()
{
  DBUG_ASSERT(fixed == 1);
  List_iterator_fast<Item> li(list);
  Item *item;
  null_value= 0;
  while ((item=li++))
  {
    if (!item->val_bool())
    {
      if (abort_on_null || !(null_value= item->null_value))
	return 0;				// return FALSE
    }
  }
  return null_value ? 0 : 1;
}


longlong Item_cond_or::val_int()
{
  DBUG_ASSERT(fixed == 1);
  List_iterator_fast<Item> li(list);
  Item *item;
  null_value=0;
  while ((item=li++))
  {
    if (item->val_bool())
    {
      null_value=0;
      return 1;
    }
    if (item->null_value)
      null_value=1;
  }
  return 0;
}

Item *Item_cond_or::copy_andor_structure(THD *thd)
{
  Item_cond_or *item;
  if ((item= new (thd->mem_root) Item_cond_or(thd, this)))
    item->copy_andor_arguments(thd, this);
  return item;
}


/**
  Create an AND expression from two expressions.

  @param a	expression or NULL
  @param b    	expression.
  @param org_item	Don't modify a if a == *org_item.
                        If a == NULL, org_item is set to point at b,
                        to ensure that future calls will not modify b.

  @note
    This will not modify item pointed to by org_item or b
    The idea is that one can call this in a loop and create and
    'and' over all items without modifying any of the original items.

  @retval
    NULL	Error
  @retval
    Item
*/

Item *and_expressions(THD *thd, Item *a, Item *b, Item **org_item)
{
  if (!a)
    return (*org_item= (Item*) b);
  if (a == *org_item)
  {
    Item_cond *res;
    if ((res= new (thd->mem_root) Item_cond_and(thd, a, (Item*) b)))
    {
      res->used_tables_cache= a->used_tables() | b->used_tables();
      res->not_null_tables_cache= a->not_null_tables() | b->not_null_tables();
    }
    return res;
  }
  if (((Item_cond_and*) a)->add((Item*) b, thd->mem_root))
    return 0;
  ((Item_cond_and*) a)->used_tables_cache|= b->used_tables();
  ((Item_cond_and*) a)->not_null_tables_cache|= b->not_null_tables();
  return a;
}


bool Item_func_null_predicate::count_sargable_conds(uchar *arg)
{
  ((SELECT_LEX*) arg)->cond_count++;
  return 0;
}


longlong Item_func_isnull::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if (const_item() && !args[0]->maybe_null)
    return 0;
  return args[0]->is_null() ? 1: 0;
}


longlong Item_is_not_null_test::val_int()
{
  DBUG_ASSERT(fixed == 1);
  DBUG_ENTER("Item_is_not_null_test::val_int");
  if (const_item() && !args[0]->maybe_null)
    DBUG_RETURN(1);
  if (args[0]->is_null())
  {
    DBUG_PRINT("info", ("null"));
    owner->was_null|= 1;
    DBUG_RETURN(0);
  }
  else
    DBUG_RETURN(1);
}

/**
  Optimize case of not_null_column IS NULL.
*/
void Item_is_not_null_test::update_used_tables()
{
  if (!args[0]->maybe_null)
    used_tables_cache= 0;			/* is always true */
  else
    args[0]->update_used_tables();
}


longlong Item_func_isnotnull::val_int()
{
  DBUG_ASSERT(fixed == 1);
  return args[0]->is_null() ? 0 : 1;
}


void Item_func_isnotnull::print(String *str, enum_query_type query_type)
{
  str->append('(');
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" is not null)"));
}


bool Item_bool_func2::count_sargable_conds(uchar *arg)
{
  ((SELECT_LEX*) arg)->cond_count++;
  return 0;
}


longlong Item_func_like::val_int()
{
  DBUG_ASSERT(fixed == 1);
  String* res= args[0]->val_str(&cmp_value1);
  if (args[0]->null_value)
  {
    null_value=1;
    return 0;
  }
  String* res2= args[1]->val_str(&cmp_value2);
  if (args[1]->null_value)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  if (canDoTurboBM)
    return turboBM_matches(res->ptr(), res->length()) ? 1 : 0;
  return my_wildcmp(cmp_collation.collation,
		    res->ptr(),res->ptr()+res->length(),
		    res2->ptr(),res2->ptr()+res2->length(),
		    escape,wild_one,wild_many) ? 0 : 1;
}


/**
  We can optimize a where if first character isn't a wildcard
*/

bool Item_func_like::with_sargable_pattern() const
{
  if (!args[1]->const_item() || args[1]->is_expensive())
    return false;

  String* res2= args[1]->val_str((String *) &cmp_value2);
  if (!res2)
    return false;

  if (!res2->length()) // Can optimize empty wildcard: column LIKE ''
    return true;

  DBUG_ASSERT(res2->ptr());
  char first= res2->ptr()[0];
  return first != wild_many && first != wild_one;
}


bool Item_func_like::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  if (Item_bool_func2::fix_fields(thd, ref) ||
      escape_item->fix_fields(thd, &escape_item))
    return TRUE;

  if (!escape_item->const_during_execution())
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"ESCAPE");
    return TRUE;
  }
  
  if (escape_item->const_item())
  {
    /* If we are on execution stage */
    String *escape_str= escape_item->val_str(&cmp_value1);
    if (escape_str)
    {
      const char *escape_str_ptr= escape_str->ptr();
      if (escape_used_in_parsing && (
             (((thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) &&
                escape_str->numchars() != 1) ||
               escape_str->numchars() > 1)))
      {
        my_error(ER_WRONG_ARGUMENTS,MYF(0),"ESCAPE");
        return TRUE;
      }

      if (use_mb(cmp_collation.collation))
      {
        CHARSET_INFO *cs= escape_str->charset();
        my_wc_t wc;
        int rc= cs->cset->mb_wc(cs, &wc,
                                (const uchar*) escape_str_ptr,
                                (const uchar*) escape_str_ptr +
                                escape_str->length());
        escape= (int) (rc > 0 ? wc : '\\');
      }
      else
      {
        /*
          In the case of 8bit character set, we pass native
          code instead of Unicode code as "escape" argument.
          Convert to "cs" if charset of escape differs.
        */
        CHARSET_INFO *cs= cmp_collation.collation;
        uint32 unused;
        if (escape_str->needs_conversion(escape_str->length(),
                                         escape_str->charset(), cs, &unused))
        {
          char ch;
          uint errors;
          uint32 cnvlen= copy_and_convert(&ch, 1, cs, escape_str_ptr,
                                          escape_str->length(),
                                          escape_str->charset(), &errors);
          escape= cnvlen ? ch : '\\';
        }
        else
          escape= escape_str_ptr ? *escape_str_ptr : '\\';
      }
    }
    else
      escape= '\\';

    /*
      We could also do boyer-more for non-const items, but as we would have to
      recompute the tables for each row it's not worth it.
    */
    if (args[1]->const_item() && !use_strnxfrm(collation.collation) &&
        !args[1]->is_expensive())
    {
      String* res2= args[1]->val_str(&cmp_value2);
      if (!res2)
        return FALSE;				// Null argument
      
      const size_t len   = res2->length();
      const char*  first = res2->ptr();
      const char*  last  = first + len - 1;
      /*
        len must be > 2 ('%pattern%')
        heuristic: only do TurboBM for pattern_len > 2
      */
      
      if (len > MIN_TURBOBM_PATTERN_LEN + 2 &&
          *first == wild_many &&
          *last  == wild_many)
      {
        const char* tmp = first + 1;
        for (; *tmp != wild_many && *tmp != wild_one && *tmp != escape; tmp++) ;
        canDoTurboBM = (tmp == last) && !use_mb(args[0]->collation.collation);
      }
      if (canDoTurboBM)
      {
        pattern_len = (int) len - 2;
        pattern     = thd->strmake(first + 1, pattern_len);
        DBUG_PRINT("info", ("Initializing pattern: '%s'", first));
        int *suff = (int*) thd->alloc((int) (sizeof(int)*
                                      ((pattern_len + 1)*2+
                                      alphabet_size)));
        bmGs      = suff + pattern_len + 1;
        bmBc      = bmGs + pattern_len + 1;
        turboBM_compute_good_suffix_shifts(suff);
        turboBM_compute_bad_character_shifts();
        DBUG_PRINT("info",("done"));
      }
      use_sampling= (len > 2 && (*first == wild_many || *first == wild_one));
    }
  }
  return FALSE;
}


void Item_func_like::cleanup()
{
  canDoTurboBM= FALSE;
  Item_bool_func2::cleanup();
}


bool Item_func_like::find_selective_predicates_list_processor(uchar *arg)
{
  find_selective_predicates_list_processor_data *data=
    (find_selective_predicates_list_processor_data *) arg;
  if (use_sampling && used_tables() == data->table->map)
  {
    THD *thd= data->table->in_use;
    COND_STATISTIC *stat;
    Item *arg0;
    if (!(stat= (COND_STATISTIC *) thd->alloc(sizeof(COND_STATISTIC))))
      return TRUE;
    stat->cond= this;
    arg0= args[0]->real_item();
    if (args[1]->const_item() && arg0->type() == FIELD_ITEM)
      stat->field_arg= ((Item_field *)arg0)->field;
    else
      stat->field_arg= NULL;
    data->list.push_back(stat, thd->mem_root);
  }
  return FALSE;
}


int Regexp_processor_pcre::default_regex_flags()
{
  return default_regex_flags_pcre(current_thd);
}


/**
  Convert string to lib_charset, if needed.
*/
String *Regexp_processor_pcre::convert_if_needed(String *str, String *converter)
{
  if (m_conversion_is_needed)
  {
    uint dummy_errors;
    if (converter->copy(str->ptr(), str->length(), str->charset(),
                        m_library_charset, &dummy_errors))
      return NULL;
    str= converter;
  }
  return str;
}


/**
  @brief Compile regular expression.

  @param[in]    pattern        the pattern to compile from.
  @param[in]    send_error     send error message if any.

  @details Make necessary character set conversion then 
  compile regular expression passed in the args[1].

  @retval    false  success.
  @retval    true   error occurred.
 */

bool Regexp_processor_pcre::compile(String *pattern, bool send_error)
{
  const char *pcreErrorStr;
  int pcreErrorOffset;

  if (is_compiled())
  {
    if (!stringcmp(pattern, &m_prev_pattern))
      return false;
    m_prev_pattern.copy(*pattern);
    pcre_free(m_pcre);
    m_pcre= NULL;
  }

  if (!(pattern= convert_if_needed(pattern, &pattern_converter)))
    return true;

  m_pcre= pcre_compile(pattern->c_ptr_safe(), m_library_flags,
                       &pcreErrorStr, &pcreErrorOffset, NULL);

  if (m_pcre == NULL)
  {
    if (send_error)
    {
      char buff[MAX_FIELD_WIDTH];
      my_snprintf(buff, sizeof(buff), "%s at offset %d", pcreErrorStr, pcreErrorOffset);
      my_error(ER_REGEXP_ERROR, MYF(0), buff);
    }
    return true;
  }
  return false;
}


bool Regexp_processor_pcre::compile(Item *item, bool send_error)
{
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff, sizeof(buff), &my_charset_bin);
  String *pattern= item->val_str(&tmp);
  if (item->null_value || compile(pattern, send_error))
    return true;
  return false;
}


/**
  Send a warning explaining an error code returned by pcre_exec().
*/
void Regexp_processor_pcre::pcre_exec_warn(int rc) const
{
  char buf[64];
  const char *errmsg= NULL;
  THD *thd= current_thd;

  /*
    Make a descriptive message only for those pcre_exec() error codes
    that can actually happen in MariaDB.
  */
  switch (rc)
  {
  case PCRE_ERROR_NOMEMORY:
    errmsg= "pcre_exec: Out of memory";
    break;
  case PCRE_ERROR_BADUTF8:
    errmsg= "pcre_exec: Invalid utf8 byte sequence in the subject string";
    break;
  case PCRE_ERROR_RECURSELOOP:
    errmsg= "pcre_exec: Recursion loop detected";
    break;
  default:
    /*
      As other error codes should normally not happen,
      we just report the error code without textual description
      of the code.
    */
    my_snprintf(buf, sizeof(buf), "pcre_exec: Internal error (%d)", rc);
    errmsg= buf;
  }
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_REGEXP_ERROR, ER_THD(thd, ER_REGEXP_ERROR), errmsg);
}


/**
  Call pcre_exec() and send a warning if pcre_exec() returned with an error.
*/
int Regexp_processor_pcre::pcre_exec_with_warn(const pcre *code,
                                               const pcre_extra *extra,
                                               const char *subject,
                                               int length, int startoffset,
                                               int options, int *ovector,
                                               int ovecsize)
{
  int rc= pcre_exec(code, extra, subject, length,
                    startoffset, options, ovector, ovecsize);
  DBUG_EXECUTE_IF("pcre_exec_error_123", rc= -123;);
  if (rc < PCRE_ERROR_NOMATCH)
    pcre_exec_warn(rc);
  return rc;
}


bool Regexp_processor_pcre::exec(const char *str, int length, int offset)
{
  m_pcre_exec_rc= pcre_exec_with_warn(m_pcre, NULL, str, length, offset, 0,
                                      m_SubStrVec, m_subpatterns_needed * 3);
  return false;
}


bool Regexp_processor_pcre::exec(String *str, int offset,
                                  uint n_result_offsets_to_convert)
{
  if (!(str= convert_if_needed(str, &subject_converter)))
    return true;
  m_pcre_exec_rc= pcre_exec_with_warn(m_pcre, NULL,
                                      str->c_ptr_safe(), str->length(),
                                      offset, 0,
                                      m_SubStrVec, m_subpatterns_needed * 3);
  if (m_pcre_exec_rc > 0)
  {
    uint i;
    for (i= 0; i < n_result_offsets_to_convert; i++)
    {
      /*
        Convert byte offset into character offset.
      */
      m_SubStrVec[i]= (int) str->charset()->cset->numchars(str->charset(),
                                                           str->ptr(),
                                                           str->ptr() +
                                                           m_SubStrVec[i]);
    }
  }
  return false;
}


bool Regexp_processor_pcre::exec(Item *item, int offset,
                                uint n_result_offsets_to_convert)
{
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff),&my_charset_bin);
  String *res= item->val_str(&tmp);
  if (item->null_value)
    return true;
  return exec(res, offset, n_result_offsets_to_convert);
}


void Regexp_processor_pcre::fix_owner(Item_func *owner,
                                      Item *subject_arg,
                                      Item *pattern_arg)
{
  if (!is_compiled() && pattern_arg->const_item())
  {
    if (compile(pattern_arg, true))
    {
      owner->maybe_null= 1; // Will always return NULL
      return;
    }
    set_const(true);
    owner->maybe_null= subject_arg->maybe_null;
  }
  else
    owner->maybe_null= 1;
}


void
Item_func_regex::fix_length_and_dec()
{
  Item_bool_func::fix_length_and_dec();

  if (agg_arg_charsets_for_comparison(cmp_collation, args, 2))
    return;

  re.init(cmp_collation.collation, 0, 0);
  re.fix_owner(this, args[0], args[1]);
}


longlong Item_func_regex::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if ((null_value= re.recompile(args[1])))
    return 0;

  if ((null_value= re.exec(args[0], 0, 0)))
    return 0;

  return re.match();
}


void
Item_func_regexp_instr::fix_length_and_dec()
{
  if (agg_arg_charsets_for_comparison(cmp_collation, args, 2))
    return;

  re.init(cmp_collation.collation, 0, 1);
  re.fix_owner(this, args[0], args[1]);
}


longlong Item_func_regexp_instr::val_int()
{
  DBUG_ASSERT(fixed == 1);
  if ((null_value= re.recompile(args[1])))
    return 0;

  if ((null_value= re.exec(args[0], 0, 1)))
    return 0;

  return re.match() ? re.subpattern_start(0) + 1 : 0;
}


#ifdef LIKE_CMP_TOUPPER
#define likeconv(cs,A) (uchar) (cs)->toupper(A)
#else
#define likeconv(cs,A) (uchar) (cs)->sort_order[(uchar) (A)]
#endif


/**
  Precomputation dependent only on pattern_len.
*/

void Item_func_like::turboBM_compute_suffixes(int *suff)
{
  const int   plm1 = pattern_len - 1;
  int            f = 0;
  int            g = plm1;
  int *const splm1 = suff + plm1;
  CHARSET_INFO	*cs= cmp_collation.collation;

  *splm1 = pattern_len;

  if (!cs->sort_order)
  {
    int i;
    for (i = pattern_len - 2; i >= 0; i--)
    {
      int tmp = *(splm1 + i - f);
      if (g < i && tmp < i - g)
	suff[i] = tmp;
      else
      {
	if (i < g)
	  g = i; // g = MY_MIN(i, g)
	f = i;
	while (g >= 0 && pattern[g] == pattern[g + plm1 - f])
	  g--;
	suff[i] = f - g;
      }
    }
  }
  else
  {
    int i;
    for (i = pattern_len - 2; 0 <= i; --i)
    {
      int tmp = *(splm1 + i - f);
      if (g < i && tmp < i - g)
	suff[i] = tmp;
      else
      {
	if (i < g)
	  g = i; // g = MY_MIN(i, g)
	f = i;
	while (g >= 0 &&
	       likeconv(cs, pattern[g]) == likeconv(cs, pattern[g + plm1 - f]))
	  g--;
	suff[i] = f - g;
      }
    }
  }
}


/**
  Precomputation dependent only on pattern_len.
*/

void Item_func_like::turboBM_compute_good_suffix_shifts(int *suff)
{
  turboBM_compute_suffixes(suff);

  int *end = bmGs + pattern_len;
  int *k;
  for (k = bmGs; k < end; k++)
    *k = pattern_len;

  int tmp;
  int i;
  int j          = 0;
  const int plm1 = pattern_len - 1;
  for (i = plm1; i > -1; i--)
  {
    if (suff[i] == i + 1)
    {
      for (tmp = plm1 - i; j < tmp; j++)
      {
	int *tmp2 = bmGs + j;
	if (*tmp2 == pattern_len)
	  *tmp2 = tmp;
      }
    }
  }

  int *tmp2;
  for (tmp = plm1 - i; j < tmp; j++)
  {
    tmp2 = bmGs + j;
    if (*tmp2 == pattern_len)
      *tmp2 = tmp;
  }

  tmp2 = bmGs + plm1;
  for (i = 0; i <= pattern_len - 2; i++)
    *(tmp2 - suff[i]) = plm1 - i;
}


/**
   Precomputation dependent on pattern_len.
*/

void Item_func_like::turboBM_compute_bad_character_shifts()
{
  int *i;
  int *end = bmBc + alphabet_size;
  int j;
  const int plm1 = pattern_len - 1;
  CHARSET_INFO	*cs= cmp_collation.collation;

  for (i = bmBc; i < end; i++)
    *i = pattern_len;

  if (!cs->sort_order)
  {
    for (j = 0; j < plm1; j++)
      bmBc[(uint) (uchar) pattern[j]] = plm1 - j;
  }
  else
  {
    for (j = 0; j < plm1; j++)
      bmBc[(uint) likeconv(cs,pattern[j])] = plm1 - j;
  }
}


/**
  Search for pattern in text.

  @return
    returns true/false for match/no match
*/

bool Item_func_like::turboBM_matches(const char* text, int text_len) const
{
  register int bcShift;
  register int turboShift;
  int shift = pattern_len;
  int j     = 0;
  int u     = 0;
  CHARSET_INFO	*cs= cmp_collation.collation;

  const int plm1=  pattern_len - 1;
  const int tlmpl= text_len - pattern_len;

  /* Searching */
  if (!cs->sort_order)
  {
    while (j <= tlmpl)
    {
      register int i= plm1;
      while (i >= 0 && pattern[i] == text[i + j])
      {
	i--;
	if (i == plm1 - shift)
	  i-= u;
      }
      if (i < 0)
	return 1;

      register const int v = plm1 - i;
      turboShift = u - v;
      bcShift    = bmBc[(uint) (uchar) text[i + j]] - plm1 + i;
      shift      = MY_MAX(turboShift, bcShift);
      shift      = MY_MAX(shift, bmGs[i]);
      if (shift == bmGs[i])
	u = MY_MIN(pattern_len - shift, v);
      else
      {
	if (turboShift < bcShift)
	  shift = MY_MAX(shift, u + 1);
	u = 0;
      }
      j+= shift;
    }
    return 0;
  }
  else
  {
    while (j <= tlmpl)
    {
      register int i = plm1;
      while (i >= 0 && likeconv(cs,pattern[i]) == likeconv(cs,text[i + j]))
      {
	i--;
	if (i == plm1 - shift)
	  i-= u;
      }
      if (i < 0)
	return 1;

      register const int v = plm1 - i;
      turboShift = u - v;
      bcShift    = bmBc[(uint) likeconv(cs, text[i + j])] - plm1 + i;
      shift      = MY_MAX(turboShift, bcShift);
      shift      = MY_MAX(shift, bmGs[i]);
      if (shift == bmGs[i])
	u = MY_MIN(pattern_len - shift, v);
      else
      {
	if (turboShift < bcShift)
	  shift = MY_MAX(shift, u + 1);
	u = 0;
      }
      j+= shift;
    }
    return 0;
  }
}


/**
  Make a logical XOR of the arguments.

  If either operator is NULL, return NULL.

  @todo
    (low priority) Change this to be optimized as: @n
    A XOR B   ->  (A) == 1 AND (B) <> 1) OR (A <> 1 AND (B) == 1) @n
    To be able to do this, we would however first have to extend the MySQL
    range optimizer to handle OR better.

  @note
    As we don't do any index optimization on XOR this is not going to be
    very fast to use.
*/

longlong Item_func_xor::val_int()
{
  DBUG_ASSERT(fixed == 1);
  int result= 0;
  null_value= false;
  for (uint i= 0; i < arg_count; i++)
  {
    result^= (args[i]->val_int() != 0);
    if (args[i]->null_value)
    {
      null_value= true;
      return 0;
    }
  }
  return result;
}

/**
  Apply NOT transformation to the item and return a new one.


    Transform the item using next rules:
    @verbatim
       a AND b AND ...    -> NOT(a) OR NOT(b) OR ...
       a OR b OR ...      -> NOT(a) AND NOT(b) AND ...
       NOT(a)             -> a
       a = b              -> a != b
       a != b             -> a = b
       a < b              -> a >= b
       a >= b             -> a < b
       a > b              -> a <= b
       a <= b             -> a > b
       IS NULL(a)         -> IS NOT NULL(a)
       IS NOT NULL(a)     -> IS NULL(a)
    @endverbatim

  @param thd		thread handler

  @return
    New item or
    NULL if we cannot apply NOT transformation (see Item::neg_transformer()).
*/

Item *Item_func_not::neg_transformer(THD *thd)	/* NOT(x)  ->  x */
{
  return args[0];
}


bool Item_func_not::fix_fields(THD *thd, Item **ref)
{
  args[0]->under_not(this);
  if (args[0]->type() == FIELD_ITEM)
  {
    /* replace  "NOT <field>" with "<filed> == 0" */
    Query_arena backup, *arena;
    Item *new_item;
    bool rc= TRUE;
    arena= thd->activate_stmt_arena_if_needed(&backup);
    if ((new_item= new (thd->mem_root) Item_func_eq(thd, args[0], new (thd->mem_root) Item_int(thd, 0, 1))))
    {
      new_item->name= name;
      rc= (*ref= new_item)->fix_fields(thd, ref);
    }
    if (arena)
      thd->restore_active_arena(arena, &backup);
    return rc;
  }
  return Item_func::fix_fields(thd, ref);
}


Item *Item_bool_rowready_func2::neg_transformer(THD *thd)
{
  Item *item= negated_item(thd);
  return item;
}

/**
  XOR can be negated by negating one of the operands:

  NOT (a XOR b)  => (NOT a) XOR b
                 => a       XOR (NOT b)

  @param thd     Thread handle
  @return        New negated item
*/
Item *Item_func_xor::neg_transformer(THD *thd)
{
  Item *neg_operand;
  Item_func_xor *new_item;
  if ((neg_operand= args[0]->neg_transformer(thd)))
    // args[0] has neg_tranformer
    new_item= new(thd->mem_root) Item_func_xor(thd, neg_operand, args[1]);
  else if ((neg_operand= args[1]->neg_transformer(thd)))
    // args[1] has neg_tranformer
    new_item= new(thd->mem_root) Item_func_xor(thd, args[0], neg_operand);
  else
  {
    neg_operand= new(thd->mem_root) Item_func_not(thd, args[0]);
    new_item= new(thd->mem_root) Item_func_xor(thd, neg_operand, args[1]);
  }
  return new_item;
}


/**
  a IS NULL  ->  a IS NOT NULL.
*/
Item *Item_func_isnull::neg_transformer(THD *thd)
{
  Item *item= new (thd->mem_root) Item_func_isnotnull(thd, args[0]);
  return item;
}


/**
  a IS NOT NULL  ->  a IS NULL.
*/
Item *Item_func_isnotnull::neg_transformer(THD *thd)
{
  Item *item= new (thd->mem_root) Item_func_isnull(thd, args[0]);
  return item;
}


Item *Item_cond_and::neg_transformer(THD *thd)	/* NOT(a AND b AND ...)  -> */
					/* NOT a OR NOT b OR ... */
{
  neg_arguments(thd);
  Item *item= new (thd->mem_root) Item_cond_or(thd, list);
  return item;
}


Item *Item_cond_or::neg_transformer(THD *thd)	/* NOT(a OR b OR ...)  -> */
					/* NOT a AND NOT b AND ... */
{
  neg_arguments(thd);
  Item *item= new (thd->mem_root) Item_cond_and(thd, list);
  return item;
}


Item *Item_func_nop_all::neg_transformer(THD *thd)
{
  /* "NOT (e $cmp$ ANY (SELECT ...)) -> e $rev_cmp$" ALL (SELECT ...) */
  Item_func_not_all *new_item= new (thd->mem_root) Item_func_not_all(thd, args[0]);
  Item_allany_subselect *allany= (Item_allany_subselect*)args[0];
  allany->create_comp_func(FALSE);
  allany->all= !allany->all;
  allany->upper_item= new_item;
  return new_item;
}

Item *Item_func_not_all::neg_transformer(THD *thd)
{
  /* "NOT (e $cmp$ ALL (SELECT ...)) -> e $rev_cmp$" ANY (SELECT ...) */
  Item_func_nop_all *new_item= new (thd->mem_root) Item_func_nop_all(thd, args[0]);
  Item_allany_subselect *allany= (Item_allany_subselect*)args[0];
  allany->all= !allany->all;
  allany->create_comp_func(TRUE);
  allany->upper_item= new_item;
  return new_item;
}

Item *Item_func_eq::negated_item(THD *thd) /* a = b  ->  a != b */
{
  return new (thd->mem_root) Item_func_ne(thd, args[0], args[1]);
}


Item *Item_func_ne::negated_item(THD *thd) /* a != b  ->  a = b */
{
  return new (thd->mem_root) Item_func_eq(thd, args[0], args[1]);
}


Item *Item_func_lt::negated_item(THD *thd) /* a < b  ->  a >= b */
{
  return new (thd->mem_root) Item_func_ge(thd, args[0], args[1]);
}


Item *Item_func_ge::negated_item(THD *thd) /* a >= b  ->  a < b */
{
  return new (thd->mem_root) Item_func_lt(thd, args[0], args[1]);
}


Item *Item_func_gt::negated_item(THD *thd) /* a > b  ->  a <= b */
{
  return new (thd->mem_root) Item_func_le(thd, args[0], args[1]);
}


Item *Item_func_le::negated_item(THD *thd) /* a <= b  ->  a > b */
{
  return new (thd->mem_root) Item_func_gt(thd, args[0], args[1]);
}

/**
  just fake method, should never be called.
*/
Item *Item_bool_rowready_func2::negated_item(THD *thd)
{
  DBUG_ASSERT(0);
  return 0;
}


/**
  Construct a minimal multiple equality item

  @param f1               the first equal item
  @param f2               the second equal item
  @param with_const_item  TRUE if the first item is constant

  @details
  The constructor builds a new item equal object for the equality f1=f2.
  One of the equal items can be constant. If this is the case it is passed
  always as the first parameter and the parameter with_const_item serves
  as an indicator of this case.
  Currently any non-constant parameter items must point to an item of the
  of the type Item_field or Item_direct_view_ref(Item_field). 
*/

Item_equal::Item_equal(THD *thd, Item *f1, Item *f2, bool with_const_item):
  Item_bool_func(thd), eval_item(0), cond_false(0), cond_true(0),
  context_field(NULL), link_equal_fields(FALSE),
  m_compare_type(item_cmp_type(f1, f2)),
  m_compare_collation(f2->collation.collation)
{
  const_item_cache= 0;
  with_const= with_const_item;
  equal_items.push_back(f1, thd->mem_root);
  equal_items.push_back(f2, thd->mem_root);
  upper_levels= NULL;
}


/**
  Copy constructor for a multiple equality
  
  @param item_equal   source item for the constructor

  @details
  The function creates a copy of an Item_equal object.
  This constructor is used when an item belongs to a multiple equality
  of an upper level (an upper AND/OR level or an upper level of a nested
  outer join).
*/

Item_equal::Item_equal(THD *thd, Item_equal *item_equal):
  Item_bool_func(thd), eval_item(0), cond_false(0), cond_true(0),
  context_field(NULL), link_equal_fields(FALSE),
  m_compare_type(item_equal->m_compare_type),
  m_compare_collation(item_equal->m_compare_collation)
{
  const_item_cache= 0;
  List_iterator_fast<Item> li(item_equal->equal_items);
  Item *item;
  while ((item= li++))
  {
    equal_items.push_back(item, thd->mem_root);
  }
  with_const= item_equal->with_const;
  cond_false= item_equal->cond_false;
  upper_levels= item_equal->upper_levels;
}


/**
  @brief
  Add a constant item to the Item_equal object

  @param[in]  c  the constant to add
  @param[in]  f  item from the list equal_items the item c is equal to
                 (this parameter is optional)

  @details
  The method adds the constant item c to the equal_items list. If the list
  doesn't have any constant item yet the item c is just put in the front
  the list. Otherwise the value of c is compared with the value of the
  constant item from equal_items. If they are not equal cond_false is set
  to TRUE. This serves as an indicator that this Item_equal is always FALSE.
*/

void Item_equal::add_const(THD *thd, Item *c)
{
  if (cond_false)
    return;
  if (!with_const)
  {
    with_const= TRUE;
    equal_items.push_front(c, thd->mem_root);
    return;
  }
  Item *const_item= get_const();
  switch (Item_equal::compare_type()) {
  case TIME_RESULT:
    {
      enum_field_types f_type= context_field->field_type();
      longlong value0= c->val_temporal_packed(f_type);
      longlong value1= const_item->val_temporal_packed(f_type);
      cond_false= c->null_value || const_item->null_value || value0 != value1;
      break;
    }
  case STRING_RESULT:
    {
      String *str1, *str2;
      /*
        Suppose we have an expression (with a string type field) like this:
          WHERE field=const1 AND field=const2 ...

        For all pairs field=constXXX we know that:

        - Item_func_eq::fix_length_and_dec() performed collation and character
        set aggregation and added character set converters when needed.
        Note, the case like:
          WHERE field=const1 COLLATE latin1_bin AND field=const2
        is not handled here, because the field would be replaced to
        Item_func_set_collation, which cannot get into Item_equal.
        So all constXXX that are handled by Item_equal
        already have compatible character sets with "field".

        - Also, Field_str::test_if_equality_guarantees_uniqueness() guarantees
        that the comparison collation of all equalities handled by Item_equal
        match the the collation of the field.

        Therefore, at Item_equal::add_const() time all constants constXXX
        should be directly comparable to each other without an additional
        character set conversion.
        It's safe to do val_str() for "const_item" and "c" and compare
        them according to the collation of the *field*.

        So in a script like this:
          CREATE TABLE t1 (a VARCHAR(10) COLLATE xxx);
          INSERT INTO t1 VALUES ('a'),('A');
          SELECT * FROM t1 WHERE a='a' AND a='A';
        Item_equal::add_const() effectively rewrites the condition to:
          SELECT * FROM t1 WHERE a='a' AND 'a' COLLATE xxx='A';
        and then to:
          SELECT * FROM t1 WHERE a='a'; // if the two constants were equal
                                        // e.g. in case of latin1_swedish_ci
        or to:
          SELECT * FROM t1 WHERE FALSE; // if the two constants were not equal
                                        // e.g. in case of latin1_bin

        Note, both "const_item" and "c" can return NULL, e.g.:
          SELECT * FROM t1 WHERE a=NULL    AND a='const';
          SELECT * FROM t1 WHERE a='const' AND a=NULL;
          SELECT * FROM t1 WHERE a='const' AND a=(SELECT MAX(a) FROM t2)
      */
      cond_false= !(str1= const_item->val_str(&cmp_value1)) ||
                  !(str2= c->val_str(&cmp_value2)) ||
                  !str1->eq(str2, compare_collation());
      break;
    }
  default:
    {
      Item_func_eq *func= new (thd->mem_root) Item_func_eq(thd, c, const_item);
      if (func->set_cmp_func())
        return;
      func->quick_fix_field();
      cond_false= !func->val_int();
    }
  }
  if (with_const && equal_items.elements == 1)
    cond_true= TRUE;
  if (cond_false || cond_true)
    const_item_cache= 1;
}


/**
  @brief
  Check whether a field is referred to in the multiple equality

  @param field   field whose occurrence is to be checked

  @details
  The function checks whether field is referred to by one of the
  items from the equal_items list.

  @retval
    1       if multiple equality contains a reference to field
  @retval
    0       otherwise    
*/

bool Item_equal::contains(Field *field)
{
  Item_equal_fields_iterator it(*this);
  while (it++)
  {
    if (field->eq(it.get_curr_field()))
        return 1;
  }
  return 0;
}


/**
  @brief
  Join members of another Item_equal object
  
  @param item    multiple equality whose members are to be joined

  @details
  The function actually merges two multiple equalities. After this operation
  the Item_equal object additionally contains the field items of another item of
  the type Item_equal.
  If the optional constant items are not equal the cond_false flag is set to TRUE.

  @notes
  The function is called for any equality f1=f2 such that f1 and f2 are items
  of the type Item_field or Item_direct_view_ref(Item_field), and, f1->field is
  referred to in the list this->equal_items, while the list item->equal_items
  contains a reference to f2->field.  
*/

void Item_equal::merge(THD *thd, Item_equal *item)
{
  Item *c= item->get_const();
  if (c)
    item->equal_items.pop();
  equal_items.append(&item->equal_items);
  if (c)
  {
    /* 
      The flag cond_false will be set to TRUE after this if 
      the multiple equality already contains a constant and its 
      value is not equal to the value of c.
    */
    add_const(thd, c);
  }
  cond_false|= item->cond_false;
} 


/**
  @brief
  Merge members of another Item_equal object into this one
  
  @param item         multiple equality whose members are to be merged
  @param save_merged  keep the list of equalities in 'item' intact
                      (e.g. for other merges)

  @details
  If the Item_equal 'item' happens to have some elements of the list
  of equal items belonging to 'this' object then the function merges
  the equal items from 'item' into this list.
  If both lists contains constants and they are different then
  the value of the cond_false flag is set to TRUE.

  @retval
    1    the lists of equal items in 'item' and 'this' contain common elements 
  @retval
    0    otherwise 

  @notes
  The method 'merge' just joins the list of equal items belonging to 'item'
  to the list of equal items belonging to this object assuming that the lists
  are disjoint. It would be more correct to call the method 'join'.
  The method 'merge_into_with_check' really merges two lists of equal items if
  they have common members.  
*/
  
bool Item_equal::merge_with_check(THD *thd, Item_equal *item, bool save_merged)
{
  bool intersected= FALSE;
  Item_equal_fields_iterator_slow fi(*item);
  
  while (fi++)
  {
    if (contains(fi.get_curr_field()))
    {
      intersected= TRUE;
      if (!save_merged)
        fi.remove();
    }
  }
  if (intersected)
  {
    if (!save_merged)
      merge(thd, item);
    else
    {
      Item *c= item->get_const();
      if (c)
        add_const(thd, c);
      if (!cond_false)
      {
        Item *item;
        fi.rewind();
        while ((item= fi++))
	{
          if (!contains(fi.get_curr_field()))
            add(item, thd->mem_root);
        }
      }
    }         
  }
  return intersected;
}


/**
  @brief
  Merge this object into a list of Item_equal objects 
  
  @param list                 the list of Item_equal objects to merge into
  @param save_merged          keep the list of equalities in 'this' intact
                              (e.g. for other merges)
  @param only_intersected     do not merge if there are no common members
                              in any of Item_equal objects from the list
                              and this Item_equal

  @details
  If the list of equal items from 'this' object contains common members
  with the lists of equal items belonging to Item_equal objects from 'list'
  then all involved Item_equal objects e1,...,ek are merged into one 
  Item equal that replaces e1,...,ek in the 'list'. Otherwise, in the case
  when the value of the parameter only_if_intersected is false, this
  Item_equal is joined to the 'list'.
*/

void Item_equal::merge_into_list(THD *thd, List<Item_equal> *list,
                                 bool save_merged,
                                 bool only_intersected)
{
  Item_equal *item;
  List_iterator<Item_equal> it(*list);
  Item_equal *merge_into= NULL;
  while((item= it++))
  {
    if (!merge_into)
    {
      if (item->merge_with_check(thd, this, save_merged))
        merge_into= item;
    }
    else
    {
      if (merge_into->merge_with_check(thd, item, false))
        it.remove();
    }
  }
  if (!only_intersected && !merge_into)
    list->push_back(this, thd->mem_root);
}


/**
  @brief
  Order equal items of the  multiple equality according to a sorting criteria

  @param compare      function to compare items from the equal_items list
  @param arg          context extra parameter for the cmp function

  @details
  The function performs ordering of the items from the equal_items list
  according to the criteria determined by the cmp callback parameter.
  If cmp(item1,item2,arg)<0 than item1 must be placed after item2.

  @notes
  The function sorts equal items by the bubble sort algorithm.
  The list of field items is looked through and whenever two neighboring
  members follow in a wrong order they are swapped. This is performed
  again and again until we get all members in a right order.
*/

void Item_equal::sort(Item_field_cmpfunc compare, void *arg)
{
  bubble_sort<Item>(&equal_items, compare, arg);
}


/**
  @brief
  Check appearance of new constant items in the multiple equality object

  @details
  The function checks appearance of new constant items among the members
  of the equal_items list. Each new constant item is compared with
  the constant item from the list if there is any. If there is none the first
  new constant item is placed at the very beginning of the list and
  with_const is set to TRUE. If it happens that the compared constant items
  are unequal then the flag cond_false is set to TRUE.

  @notes 
  Currently this function is called only after substitution of constant tables.
*/

void Item_equal::update_const(THD *thd)
{
  List_iterator<Item> it(equal_items);
  if (with_const)
    it++;
  Item *item;
  while ((item= it++))
  {
    if (item->const_item() && !item->is_expensive() &&
        /*
          Don't propagate constant status of outer-joined column.
          Such a constant status here is a result of:
            a) empty outer-joined table: in this case such a column has a
               value of NULL; but at the same time other arguments of
               Item_equal don't have to be NULLs and the value of the whole
               multiple equivalence expression doesn't have to be NULL or FALSE
               because of the outer join nature;
          or
            b) outer-joined table contains only 1 row: the result of
               this column is equal to a row field value *or* NULL.
          Both values are inacceptable as Item_equal constants.
        */
        !item->is_outer_field())
    {
      if (item == equal_items.head())
        with_const= TRUE;
      else
      {
        it.remove();
        add_const(thd, item);
      }
    } 
  }
}


/**
  @brief
  Fix fields in a completely built multiple equality

  @param  thd     currently not used thread handle 
  @param  ref     not used

  @details
  This function is called once the multiple equality has been built out of 
  the WHERE/ON condition and no new members are expected to be added to the
  equal_items list anymore.
  As any implementation of the virtual fix_fields method the function
  calculates the cached values of not_null_tables_cache, used_tables_cache,
  const_item_cache and calls fix_length_and_dec().
  Additionally the function sets a reference to the Item_equal object in
  the non-constant items of the equal_items list unless such a reference has
  been already set.

  @notes 
  Currently this function is called only in the function
  build_equal_items_for_cond.
  
  @retval
  FALSE   always
*/

bool Item_equal::fix_fields(THD *thd, Item **ref)
{ 
  DBUG_ASSERT(fixed == 0);
  Item_equal_fields_iterator it(*this);
  Item *item;
  Field *first_equal_field= NULL;
  Field *last_equal_field= NULL;
  Field *prev_equal_field= NULL;
  not_null_tables_cache= used_tables_cache= 0;
  const_item_cache= 0;
  while ((item= it++))
  {
    table_map tmp_table_map;
    used_tables_cache|= item->used_tables();
    tmp_table_map= item->not_null_tables();
    not_null_tables_cache|= tmp_table_map;
    DBUG_ASSERT(!item->with_sum_func && !item->with_subselect);
    if (item->maybe_null)
      maybe_null= 1;
    if (!item->get_item_equal())
      item->set_item_equal(this);
    if (link_equal_fields && item->real_item()->type() == FIELD_ITEM)
    {
      last_equal_field= ((Item_field *) (item->real_item()))->field;
      if (!prev_equal_field)
        first_equal_field= last_equal_field;
      else
        prev_equal_field->next_equal_field= last_equal_field;
      prev_equal_field= last_equal_field;         
    }
  }
  if (prev_equal_field && last_equal_field != first_equal_field)
    last_equal_field->next_equal_field= first_equal_field;
  fix_length_and_dec();
  fixed= 1;
  return FALSE;
}


/**
  Update the value of the used table attribute and other attributes
 */

void Item_equal::update_used_tables()
{
  not_null_tables_cache= used_tables_cache= 0;
  if ((const_item_cache= cond_false || cond_true))
    return;
  Item_equal_fields_iterator it(*this);
  Item *item;
  const_item_cache= 1;
  while ((item= it++))
  {
    item->update_used_tables();
    used_tables_cache|= item->used_tables();
    /* see commentary at Item_equal::update_const() */
    const_item_cache&= item->const_item() && !item->is_outer_field();
  }
}


bool Item_equal::count_sargable_conds(uchar *arg)
{
  SELECT_LEX *sel= (SELECT_LEX *) arg;
  uint m= equal_items.elements;
  sel->cond_count+= m*(m-1);
  return 0;
}


/**
  @brief
  Evaluate multiple equality

  @details
  The function evaluate multiple equality to a boolean value.
  The function ignores non-constant items from the equal_items list.
  The function returns 1 if all constant items from the list are equal. 
  It returns 0 if there are unequal constant items in the list or 
  one of the constant items is evaluated to NULL. 
  
  @notes 
  Currently this function can be called only at the optimization
  stage after the constant table substitution, since all Item_equals
  are eliminated before the execution stage.
  
  @retval
     0     multiple equality is always FALSE or NULL
     1     otherwise
*/

longlong Item_equal::val_int()
{
  if (cond_false)
    return 0;
  if (cond_true)
    return 1;
  Item *item= get_const();
  Item_equal_fields_iterator it(*this);
  if (!item)
    item= it++;
  eval_item->store_value(item);
  if ((null_value= item->null_value))
    return 0;
  while ((item= it++))
  {
    Field *field= it.get_curr_field();
    /* Skip fields of tables that has not been read yet */
    if (!field->table->status || (field->table->status & STATUS_NULL_ROW))
    {
      if (eval_item->cmp(item) || (null_value= item->null_value))
        return 0;
    }
  }
  return 1;
}


void Item_equal::fix_length_and_dec()
{
  Item *item= get_first(NO_PARTICULAR_TAB, NULL);
  eval_item= cmp_item::get_comparator(item->cmp_type(), item,
                                      item->collation.collation);
}


bool Item_equal::walk(Item_processor processor, bool walk_subquery, uchar *arg)
{
  Item *item;
  Item_equal_fields_iterator it(*this);
  while ((item= it++))
  {
    if (item->walk(processor, walk_subquery, arg))
      return 1;
  }
  return Item_func::walk(processor, walk_subquery, arg);
}


Item *Item_equal::transform(THD *thd, Item_transformer transformer, uchar *arg)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  Item *item;
  Item_equal_fields_iterator it(*this);
  while ((item= it++))
  {
    Item *new_item= item->transform(thd, transformer, arg);
    if (!new_item)
      return 0;

    /*
      THD::change_item_tree() should be called only if the tree was
      really transformed, i.e. when a new item has been created.
      Otherwise we'll be allocating a lot of unnecessary memory for
      change records at each execution.
    */
    if (new_item != item)
      thd->change_item_tree((Item **) it.ref(), new_item);
  }
  return Item_func::transform(thd, transformer, arg);
}


void Item_equal::print(String *str, enum_query_type query_type)
{
  if (cond_false)
  {
    str->append('0');
    return;
  }
  str->append(func_name());
  str->append('(');
  List_iterator_fast<Item> it(equal_items);
  Item *item;
  item= it++;
  item->print(str, query_type);
  while ((item= it++))
  {
    str->append(',');
    str->append(' ');
    item->print(str, query_type);
  }
  str->append(')');
}


/*
  @brief Get the first equal field of multiple equality.
  @param[in] field   the field to get equal field to

  @details Get the first field of multiple equality that is equal to the
  given field. In order to make semi-join materialization strategy work
  correctly we can't propagate equal fields from upper select to a
  materialized semi-join.
  Thus the fields is returned according to following rules:

  1) If the given field belongs to a semi-join then the first field in
     multiple equality which belong to the same semi-join is returned.
     Otherwise NULL is returned.
  2) If the given field doesn't belong to a semi-join then
     the first field in the multiple equality that doesn't belong to any
     semi-join is returned.
     If all fields in the equality are belong to semi-join(s) then NULL
     is returned.
  3) If no field is given then the first field in the multiple equality
     is returned without regarding whether it belongs to a semi-join or not.

  @retval Found first field in the multiple equality.
  @retval 0 if no field found.
*/

Item* Item_equal::get_first(JOIN_TAB *context, Item *field_item)
{
  Item_equal_fields_iterator it(*this);
  Item *item;
  if (!field_item)
    return (it++);
  Field *field= ((Item_field *) (field_item->real_item()))->field;

  /*
    Of all equal fields, return the first one we can use. Normally, this is the
    field which belongs to the table that is the first in the join order.

    There is one exception to this: When semi-join materialization strategy is
    used, and the given field belongs to a table within the semi-join nest, we
    must pick the first field in the semi-join nest.

    Example: suppose we have a join order:

       ot1 ot2  SJ-Mat(it1  it2  it3)  ot3

    and equality ot2.col = it1.col = it2.col
    If we're looking for best substitute for 'it2.col', we should pick it1.col
    and not ot2.col.
    
    eliminate_item_equal() also has code that deals with equality substitution
    in presense of SJM nests.
  */

  TABLE_LIST *emb_nest;
  if (context != NO_PARTICULAR_TAB)
    emb_nest= context->emb_sj_nest;
  else
    emb_nest= field->table->pos_in_table_list->embedding;

  if (emb_nest && emb_nest->sj_mat_info && emb_nest->sj_mat_info->is_used)
  {
    /*
      It's a field from an materialized semi-join. We can substitute it for
       - a constant item 
       - a field from the same semi-join
       Find the first of such items:
    */
    while ((item= it++))
    {
      if (item->const_item() || 
          it.get_curr_field()->table->pos_in_table_list->embedding == emb_nest)
      {
        /*
          If we found given field then return NULL to avoid unnecessary
          substitution.
        */
        return (item != field_item) ? item : NULL;
      }
    }
  }
  else
  {
    /*
      The field is not in SJ-Materialization nest. We must return the first
      field in the join order. The field may be inside a semi-join nest, i.e 
      a join order may look like this:

          SJ-Mat(it1  it2)  ot1  ot2

      where we're looking what to substitute ot2.col for. In this case we must 
      still return it1.col, here's a proof why:

      First let's note that either it1.col or it2.col participates in 
      subquery's IN-equality. It can't be otherwise, because materialization is
      only applicable to uncorrelated subqueries, so the only way we could
      infer "it1.col=ot1.col" is from the IN-equality. Ok, so IN-eqality has 
      it1.col or it2.col on its inner side. it1.col is first such item in the
      join order, so it's not possible for SJ-Mat to be
      SJ-Materialization-lookup, it is SJ-Materialization-Scan. The scan part
      of this strategy will unpack value of it1.col=it2.col into it1.col
      (that's the first equal item inside the subquery), and we'll be able to
      get it from there. qed.
    */

    return equal_items.head();
  }
  // Shouldn't get here.
  DBUG_ASSERT(0);
  return NULL;
}


longlong Item_func_dyncol_check::val_int()
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp(buff, sizeof(buff), &my_charset_bin);
  DYNAMIC_COLUMN col;
  String *str;
  enum enum_dyncol_func_result rc;

  str= args[0]->val_str(&tmp);
  if (args[0]->null_value)
    goto null;
  col.length= str->length();
  /* We do not change the string, so could do this trick */
  col.str= (char *)str->ptr();
  rc= mariadb_dyncol_check(&col);
  if (rc < 0 && rc != ER_DYNCOL_FORMAT)
  {
    dynamic_column_error_message(rc);
    goto null;
  }
  null_value= FALSE;
  return rc == ER_DYNCOL_OK;

null:
  null_value= TRUE;
  return 0;
}

longlong Item_func_dyncol_exists::val_int()
{
  char buff[STRING_BUFFER_USUAL_SIZE], nmstrbuf[11];
  String tmp(buff, sizeof(buff), &my_charset_bin),
         nmbuf(nmstrbuf, sizeof(nmstrbuf), system_charset_info);
  DYNAMIC_COLUMN col;
  String *str;
  LEX_STRING buf, *name= NULL;
  ulonglong num= 0;
  enum enum_dyncol_func_result rc;

  if (args[1]->result_type() == INT_RESULT)
    num= args[1]->val_int();
  else
  {
    String *nm= args[1]->val_str(&nmbuf);
    if (!nm || args[1]->null_value)
    {
      null_value= 1;
      return 1;
    }
    if (my_charset_same(nm->charset(), &my_charset_utf8_general_ci))
    {
      buf.str= (char *) nm->ptr();
      buf.length= nm->length();
    }
    else
    {
      uint strlen;
      uint dummy_errors;
      buf.str= (char *)sql_alloc((strlen= nm->length() *
                                     my_charset_utf8_general_ci.mbmaxlen + 1));
      if (buf.str)
      {
        buf.length=
          copy_and_convert(buf.str, strlen, &my_charset_utf8_general_ci,
                           nm->ptr(), nm->length(), nm->charset(),
                           &dummy_errors);
      }
      else
        buf.length= 0;
    }
    name= &buf;
  }
  str= args[0]->val_str(&tmp);
  if (args[0]->null_value || args[1]->null_value || num > UINT_MAX16)
    goto null;
  col.length= str->length();
  /* We do not change the string, so could do this trick */
  col.str= (char *)str->ptr();
  rc= ((name == NULL) ?
       mariadb_dyncol_exists_num(&col, (uint) num) :
       mariadb_dyncol_exists_named(&col, name));
  if (rc < 0)
  {
    dynamic_column_error_message(rc);
    goto null;
  }
  null_value= FALSE;
  return rc == ER_DYNCOL_YES;

null:
  null_value= TRUE;
  return 0;
}


Item_bool_rowready_func2 *Eq_creator::create(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_eq(thd, a, b);
}


Item_bool_rowready_func2* Eq_creator::create_swap(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_eq(thd, b, a);
}


Item_bool_rowready_func2* Ne_creator::create(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_ne(thd, a, b);
}


Item_bool_rowready_func2* Ne_creator::create_swap(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_ne(thd, b, a);
}


Item_bool_rowready_func2* Gt_creator::create(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_gt(thd, a, b);
}


Item_bool_rowready_func2* Gt_creator::create_swap(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_lt(thd, b, a);
}


Item_bool_rowready_func2* Lt_creator::create(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_lt(thd, a, b);
}


Item_bool_rowready_func2* Lt_creator::create_swap(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_gt(thd, b, a);
}


Item_bool_rowready_func2* Ge_creator::create(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_ge(thd, a, b);
}


Item_bool_rowready_func2* Ge_creator::create_swap(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_le(thd, b, a);
}


Item_bool_rowready_func2* Le_creator::create(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_le(thd, a, b);
}


Item_bool_rowready_func2* Le_creator::create_swap(THD *thd, Item *a, Item *b) const
{
  return new(thd->mem_root) Item_func_ge(thd, b, a);
}
