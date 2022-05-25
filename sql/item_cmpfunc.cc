/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB

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
  This file defines all compare functions
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include <m_ctype.h>
#include "sql_select.h"
#include "sql_parse.h"                          // check_stack_overrun
#include "sql_base.h"                  // dynamic_column_error_message

#define PCRE2_STATIC 1             /* Important on Windows */
#include "pcre2.h"                 /* pcre2 header file */

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

  This method aggregates comparison handler from the array of items.
  The result handler is used later for comparison of values of these items.

  aggregate_for_comparison()
  funcname                      the function or operator name,
                                for error reporting
  items                         array of items to aggregate the type from
  nitems                        number of items in the array
  int_uint_as_dec               what to do when comparing INT to UINT:
                                set the comparison handler to decimal or int.

  @retval true  type incompatibility has been detected
  @retval false otherwise
*/

bool Type_handler_hybrid_field_type::
aggregate_for_comparison(const LEX_CSTRING &funcname,
                         Item **items,
                         uint nitems,
                         bool int_uint_as_dec)
{
  uint unsigned_count= items[0]->unsigned_flag;
  /*
    Convert sub-type to super-type (e.g. DATE to DATETIME, INT to BIGINT, etc).
    Otherwise Predicant_to_list_comparator will treat sub-types of the same
    super-type as different data types and won't be able to use bisection in
    many cases.
  */
  set_handler(items[0]->type_handler()->type_handler_for_comparison());
  for (uint i= 1 ; i < nitems ; i++)
  {
    unsigned_count+= items[i]->unsigned_flag;
    if (aggregate_for_comparison(items[i]->type_handler()->
                                 type_handler_for_comparison()))
    {
      /*
        For more precise error messages if aggregation failed on the first pair
        {items[0],items[1]}, use the name of items[0]->data_handler().
        Otherwise use the name of this->type_handler(), which is already a
        result of aggregation for items[0]..items[i-1].
      */
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
               i == 1 ? items[0]->type_handler()->name().ptr() :
                        type_handler()->name().ptr(),
               items[i]->type_handler()->name().ptr(),
               funcname.str);
      return true;
    }
    /*
      When aggregating types of two row expressions we have to check
      that they have the same cardinality and that each component
      of the first row expression has a compatible row signature with
      the signature of the corresponding component of the second row
      expression.
    */ 
    if (cmp_type() == ROW_RESULT && cmp_row_type(items[0], items[i]))
      return true;     // error found: invalid usage of rows
  }
  /**
    If all arguments are of INT type but have different unsigned_flag values,
    switch to DECIMAL_RESULT.
  */
  if (int_uint_as_dec &&
      cmp_type() == INT_RESULT &&
      unsigned_count != nitems && unsigned_count != 0)
    set_handler(&type_handler_newdecimal);
  return 0;
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
  DBUG_ASSERT(fixed());
  bool value= args[0]->val_bool();
  null_value=args[0]->null_value;
  return ((!null_value && value == 0) ? 1 : 0);
}

void Item_func_not::print(String *str, enum_query_type query_type)
{
  str->append('!');
  args[0]->print_parenthesised(str, query_type, precedence());
}

/**
  special NOT for ALL subquery.
*/


longlong Item_func_not_all::val_int()
{
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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

  if ((*item)->can_eval_in_optimize())
  {
    TABLE *table= field->table;
    MY_BITMAP *old_maps[2] = { NULL, NULL };
    ulonglong UNINIT_VAR(orig_field_val); /* original field value if valid */
    bool save_field_value;

    /* table->read_set may not be set if we come here from a CREATE TABLE */
    if (table && table->read_set)
      dbug_tmp_use_all_columns(table, old_maps,
                               &table->read_set, &table->write_set);

    /*
      Store the value of the field/constant because the call to save_in_field
      below overrides that value. Don't save field value if no data has been
      read yet.
    */
    save_field_value= (field_item->const_item() ||
                       !(field->table->status & STATUS_NO_RECORD));
    if (save_field_value)
      orig_field_val= field->val_int();
    if (!(*item)->save_in_field_no_warnings(field, 1) && !field->is_null())
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
        Item *tmp= (new (thd->mem_root)
                    Item_int_with_ref(thd, field->val_int(), *item,
                                      MY_TEST(field->flags & UNSIGNED_FLAG)));
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
    if (table && table->read_set)
      dbug_tmp_restore_column_maps(&table->read_set, &table->write_set, old_maps);
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
      if (((field_item->field_type() == MYSQL_TYPE_LONGLONG &&
            field_item->type_handler() != &type_handler_vers_trx_id) ||
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

  return cmp->set_cmp_func(thd, this, &args[0], &args[1], true);
}


/*
  Comparison operators remove arguments' dependency on PAD_CHAR_TO_FULL_LENGTH
  in case of PAD SPACE comparison collations: trailing spaces do not affect
  the comparison result for such collations.
*/
Sql_mode_dependency
Item_bool_rowready_func2::value_depends_on_sql_mode() const
{
  if (compare_collation()->state & MY_CS_NOPAD)
    return Item_func::value_depends_on_sql_mode();
  return ((args[0]->value_depends_on_sql_mode() |
           args[1]->value_depends_on_sql_mode()) &
          Sql_mode_dependency(~0, ~MODE_PAD_CHAR_TO_FULL_LENGTH)).
         soft_to_hard();
}


bool Item_bool_rowready_func2::fix_length_and_dec(THD *thd)
{
  max_length= 1;				     // Function returns 0 or 1

  /*
    As some compare functions are generated after sql_yacc,
    we have to check for out of memory conditions here
  */
  if (!args[0] || !args[1])
    return FALSE;
  return setup_args_and_comparator(current_thd, &cmp);
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

int Arg_comparator::set_cmp_func(THD *thd, Item_func_or_sum *owner_arg,
                                 Item **a1, Item **a2)
{
  owner= owner_arg;
  set_null= set_null && owner_arg;
  a= a1;
  b= a2;
  Item *tmp_args[2]= {*a1, *a2};
  Type_handler_hybrid_field_type tmp;
  if (tmp.aggregate_for_comparison(owner_arg->func_name_cstring(), tmp_args, 2,
                                   false))
  {
    DBUG_ASSERT(thd->is_error());
    return 1;
  }
  m_compare_handler= tmp.type_handler();
  return m_compare_handler->set_comparator_func(thd, this);
}


bool Arg_comparator::set_cmp_func_for_row_arguments(THD *thd)
{
  uint n= (*a)->cols();
  if (n != (*b)->cols())
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), n);
    comparators= 0;
    return true;
  }
  if (!(comparators= new (thd->mem_root) Arg_comparator[n]))
    return true;
  for (uint i=0; i < n; i++)
  {
    if ((*a)->element_index(i)->cols() != (*b)->element_index(i)->cols())
    {
      my_error(ER_OPERAND_COLUMNS, MYF(0), (*a)->element_index(i)->cols());
      return true;
    }
    if (comparators[i].set_cmp_func(thd, owner, (*a)->addr(i),
                                    (*b)->addr(i), set_null))
      return true;
  }
  return false;
}


bool Arg_comparator::set_cmp_func_row(THD *thd)
{
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_row :
                                &Arg_comparator::compare_row;
  return set_cmp_func_for_row_arguments(thd);
}


bool Arg_comparator::set_cmp_func_string(THD *thd)
{
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_string :
                                &Arg_comparator::compare_string;
  if (compare_type() == STRING_RESULT &&
      (*a)->result_type() == STRING_RESULT &&
      (*b)->result_type() == STRING_RESULT)
  {
    /*
      We must set cmp_collation here as we may be called from for an automatic
      generated item, like in natural join
    */
    if (owner->agg_arg_charsets_for_comparison(&m_compare_collation, a, b))
      return true;

    if ((*a)->type() == Item::FUNC_ITEM &&
        ((Item_func *) (*a))->functype() == Item_func::JSON_EXTRACT_FUNC)
    {
      func= is_owner_equal_func() ? &Arg_comparator::compare_e_json_str:
                                    &Arg_comparator::compare_json_str;
      return 0;
    }
    else if ((*b)->type() == Item::FUNC_ITEM &&
             ((Item_func *) (*b))->functype() == Item_func::JSON_EXTRACT_FUNC)
    {
      func= is_owner_equal_func() ? &Arg_comparator::compare_e_json_str:
                                    &Arg_comparator::compare_str_json;
      return 0;
    }
  }

  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
}


bool Arg_comparator::set_cmp_func_time(THD *thd)
{
  m_compare_collation= &my_charset_numeric;
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_time :
                                &Arg_comparator::compare_time;
  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
}


bool Arg_comparator::set_cmp_func_datetime(THD *thd)
{
  m_compare_collation= &my_charset_numeric;
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_datetime :
                                &Arg_comparator::compare_datetime;
  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
}


bool Arg_comparator::set_cmp_func_native(THD *thd)
{
  m_compare_collation= &my_charset_numeric;
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_native :
                                &Arg_comparator::compare_native;
  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
}


bool Arg_comparator::set_cmp_func_int(THD *thd)
{
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_int :
                                &Arg_comparator::compare_int_signed;
  if ((*a)->field_type() == MYSQL_TYPE_YEAR &&
      (*b)->field_type() == MYSQL_TYPE_YEAR)
  {
    func= is_owner_equal_func() ? &Arg_comparator::compare_e_datetime :
                                  &Arg_comparator::compare_datetime;
  }
  else if (func == &Arg_comparator::compare_int_signed)
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
  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
}


bool Arg_comparator::set_cmp_func_real(THD *thd)
{
  if ((((*a)->result_type() == DECIMAL_RESULT && !(*a)->const_item() &&
        (*b)->result_type() == STRING_RESULT  &&  (*b)->const_item()) ||
      ((*b)->result_type() == DECIMAL_RESULT && !(*b)->const_item() &&
       (*a)->result_type() == STRING_RESULT  &&  (*a)->const_item())))
  {
    /*
     <non-const decimal expression> <cmp> <const string expression>
     or
     <const string expression> <cmp> <non-const decimal expression>

     Do comparison as decimal rather than float, in order not to lose precision.
    */
    m_compare_handler= &type_handler_newdecimal;
    return set_cmp_func_decimal(thd);
  }

  func= is_owner_equal_func() ? &Arg_comparator::compare_e_real :
                                &Arg_comparator::compare_real;
  if ((*a)->decimals < NOT_FIXED_DEC && (*b)->decimals < NOT_FIXED_DEC)
  {
    precision= 5 / log_10[MY_MAX((*a)->decimals, (*b)->decimals) + 1];
    if (func == &Arg_comparator::compare_real)
      func= &Arg_comparator::compare_real_fixed;
    else if (func == &Arg_comparator::compare_e_real)
      func= &Arg_comparator::compare_e_real_fixed;
  }
  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
}

bool Arg_comparator::set_cmp_func_decimal(THD *thd)
{
  func= is_owner_equal_func() ? &Arg_comparator::compare_e_decimal :
                                &Arg_comparator::compare_decimal;
  a= cache_converted_constant(thd, a, &a_cache, compare_type_handler());
  b= cache_converted_constant(thd, b, &b_cache, compare_type_handler());
  return false;
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
                                                const Type_handler *handler)
{
  /*
    Don't need cache if doing context analysis only.
  */
  if (!thd_arg->lex->is_ps_or_view_context_analysis() &&
      (*value)->const_item() &&
      handler->type_handler_for_comparison() !=
      (*value)->type_handler_for_comparison())
  {
    Item_cache *cache= handler->Item_get_cache(thd_arg, *value);
    cache->setup(thd_arg, *value);
    *cache_item= cache;
    return cache_item;
  }
  return value;
}


int Arg_comparator::compare_time()
{
  THD *thd= current_thd;
  longlong val1= (*a)->val_time_packed(thd);
  if (!(*a)->null_value)
  {
    longlong val2= (*b)->val_time_packed(thd);
    if (!(*b)->null_value)
      return compare_not_null_values(val1, val2);
  }
  if (set_null)
    owner->null_value= true;
  return -1;
}


int Arg_comparator::compare_e_time()
{
  THD *thd= current_thd;
  longlong val1= (*a)->val_time_packed(thd);
  longlong val2= (*b)->val_time_packed(thd);
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return MY_TEST(val1 == val2);
}



int Arg_comparator::compare_datetime()
{
  THD *thd= current_thd;
  longlong val1= (*a)->val_datetime_packed(thd);
  if (!(*a)->null_value)
  {
    longlong val2= (*b)->val_datetime_packed(thd);
    if (!(*b)->null_value)
      return compare_not_null_values(val1, val2);
  }
  if (set_null)
    owner->null_value= true;
  return -1;
}


int Arg_comparator::compare_e_datetime()
{
  THD *thd= current_thd;
  longlong val1= (*a)->val_datetime_packed(thd);
  longlong val2= (*b)->val_datetime_packed(thd);
  if ((*a)->null_value || (*b)->null_value)
    return MY_TEST((*a)->null_value && (*b)->null_value);
  return MY_TEST(val1 == val2);
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


int Arg_comparator::compare_native()
{
  THD *thd= current_thd;
  if (!(*a)->val_native_with_conversion(thd, &m_native1,
                                        compare_type_handler()))
  {
    if (!(*b)->val_native_with_conversion(thd, &m_native2,
                                          compare_type_handler()))
    {
      if (set_null)
        owner->null_value= 0;
      return compare_type_handler()->cmp_native(m_native1, m_native2);
    }
  }
  if (set_null)
    owner->null_value= 1;
  return -1;
}


int Arg_comparator::compare_e_native()
{
  THD *thd= current_thd;
  bool res1= (*a)->val_native_with_conversion(thd, &m_native1,
                                              compare_type_handler());
  bool res2= (*b)->val_native_with_conversion(thd, &m_native2,
                                              compare_type_handler());
  if (res1 || res2)
    return MY_TEST(res1 == res2);
  return MY_TEST(compare_type_handler()->cmp_native(m_native1, m_native2) == 0);
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
  VDec val1(*a);
  if (!val1.is_null())
  {
    VDec val2(*b);
    if (!val2.is_null())
    {
      if (set_null)
        owner->null_value= 0;
      val1.round_self_if_needed((*a)->decimals, HALF_UP);
      val2.round_self_if_needed((*b)->decimals, HALF_UP);
      return val1.cmp(val2);
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
  VDec val1(*a), val2(*b);
  if (val1.is_null() || val2.is_null())
    return MY_TEST(val1.is_null() && val2.is_null());
  val1.round_self_if_needed((*a)->decimals, HALF_UP);
  val2.round_self_if_needed((*b)->decimals, HALF_UP);
  return MY_TEST(val1.cmp(val2) == 0);
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
      return compare_not_null_values(val1, val2);
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
        break; // NE never aborts on NULL
      case Item_func::LT_FUNC:
      case Item_func::LE_FUNC:
      case Item_func::GT_FUNC:
      case Item_func::GE_FUNC:
        return -1; // <, <=, > and >= always fail on NULL
      case Item_func::EQ_FUNC:
        if (owner->is_top_level_item())
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


int Arg_comparator::compare_json_str()
{
  return compare_json_str_basic(*a, *b);
}


int Arg_comparator::compare_str_json()
{
  return -compare_json_str_basic(*b, *a);
}


int Arg_comparator::compare_e_json_str()
{
  return compare_e_json_str_basic(*a, *b);
}


int Arg_comparator::compare_e_str_json()
{
  return compare_e_json_str_basic(*b, *a);
}


bool Item_func_truth::fix_length_and_dec(THD *thd)
{
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value= 0;
  decimals= 0;
  max_length= 1;
  return FALSE;
}


void Item_func_truth::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, precedence());
  str->append(STRING_WITH_LEN(" is "));
  if (! affirmative)
    str->append(STRING_WITH_LEN("not "));
  if (value)
    str->append(STRING_WITH_LEN("true"));
  else
    str->append(STRING_WITH_LEN("false"));
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


void Item_in_optimizer::fix_after_pullout(st_select_lex *new_parent,
                                          Item **ref, bool merge)
{
  DBUG_ASSERT(fixed());
  /* This will re-calculate attributes of our Item_in_subselect: */
  Item_bool_func::fix_after_pullout(new_parent, ref, merge);

  /* Then, re-calculate not_null_tables_cache: */
  eval_not_null_tables(NULL);
}


bool Item_in_optimizer::eval_not_null_tables(void *opt_arg)
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


bool Item_in_optimizer::find_not_null_fields(table_map allowed)
{
  if (!(~allowed & used_tables()) && is_top_level_item())
  {
    return args[0]->find_not_null_fields(allowed);
  }
  return false;
}

void Item_in_optimizer::print(String *str, enum_query_type query_type)
{
  if (query_type & QT_PARSABLE)
    args[1]->print(str, query_type);
  else
  {
     restore_first_argument();
     Item_func::print(str, query_type);
  }
}


/**
  "Restore" first argument before fix_fields() call (after it is harmless).

  @Note: Main pointer to left part of IN/ALL/ANY subselect is subselect's
  lest_expr (see Item_in_optimizer::fix_left) so changes made during
  fix_fields will be rolled back there which can make
  Item_in_optimizer::args[0] unusable on second execution before fix_left()
  call. This call fix the pointer.
*/

void Item_in_optimizer::restore_first_argument()
{
  Item_in_subselect *in_subs= args[1]->get_IN_subquery();
  if (in_subs)
    args[0]= in_subs->left_exp();
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
  if (!invisible_mode())
  {
    /*
       left_expr->fix_fields() may cause left_expr to be substituted for
       another item. (e.g. an Item_field may be changed into Item_ref). This
       transformation is undone at the end of statement execution (e.g. the
       Item_ref is deleted). However, Item_in_optimizer::args[0] may keep
       the pointer to the post-transformation item. Because of that, on the
       next execution we need to copy args[1]->left_expr again.
    */
    ref0= args[1]->get_IN_subquery()->left_exp_ptr();
    args[0]= (*ref0);
  }
  if ((*ref0)->fix_fields_if_needed(thd, ref0) ||
      (!cache && !(cache= (*ref0)->get_cache(thd))))
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
      if (args[0]->element_index(i)->walk(&Item::is_subquery_processor, 0, 0))
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
  with_flags|= (args[0]->with_flags |
               (args[1]->with_flags & item_with_t::SP_VAR));
  if ((const_item_cache= args[0]->const_item()))
  {
    cache->store(args[0]);
    cache->cache_value();
  }
  if (args[1]->fixed())
  {
    /* to avoid overriding is called to update left expression */
    used_tables_and_const_cache_join(args[1]);
    with_flags|= args[1]->with_flags & item_with_t::SUM_FUNC;
  }
  DBUG_RETURN(0);
}


bool Item_in_optimizer::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
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
  if (args[0]->maybe_null())
    set_maybe_null();

  if (args[1]->fix_fields_if_needed(thd, args + 1))
    return TRUE;
  if (!invisible_mode() &&
      ((sub && ((col= args[0]->cols()) != sub->engine->cols())) ||
       (!sub && (args[1]->cols() != (col= 1)))))
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), col);
    return TRUE;
  }

  base_flags|= (item_base_t::FIXED |
                (args[1]->base_flags & (item_base_t::MAYBE_NULL |
                                        item_base_t::AT_TOP_LEVEL)));
  with_flags|= (item_with_t::SUBQUERY |
                args[1]->with_flags |
                (args[0]->with_flags &
                 (item_with_t::SP_VAR | item_with_t::WINDOW_FUNC)));
  // The subquery cannot have window functions aggregated in this select
  DBUG_ASSERT(!args[1]->with_window_func());
  used_tables_and_const_cache_join(args[1]);
  return FALSE;
}

/**
  Check if Item_in_optimizer should work as a pass-through item for its 
  arguments.

  @note 
   Item_in_optimizer should work as pass-through for
    - subqueries that were processed by ALL/ANY->MIN/MAX rewrite
    - subqueries that were originally EXISTS subqueries (and were coinverted by
      the EXISTS->IN rewrite)

   When Item_in_optimizer is not not working as a pass-through, it
    - caches its "left argument", args[0].
    - makes adjustments to subquery item's return value for proper NULL
      value handling
*/

bool Item_in_optimizer::invisible_mode()
{
  /* MAX/MIN transformed or EXISTS->IN prepared => do nothing */
  return (args[1]->get_IN_subquery() == NULL);
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
  DBUG_ASSERT(fixed());

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
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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

  if (cache->null_value_inside)
  {
     DBUG_PRINT("info", ("Left NULL..."));
    /*
      We're evaluating 
      "<outer_value_list> [NOT] IN (SELECT <inner_value_list>...)" 
      where one or more of the outer values is NULL. 
    */
    if (args[1]->is_top_level_item())
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
      Item_in_subselect *item_subs= args[1]->get_IN_subquery();
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

  DBUG_ASSERT(fixed());
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

    thd->change_item_tree(args[1]->get_IN_subquery()->left_exp_ptr(), args[0]);
  }
  return (this->*transformer)(thd, argument);
}


bool Item_in_optimizer::is_expensive_processor(void *arg)
{
  DBUG_ASSERT(fixed());
  return args[0]->is_expensive_processor(arg) ||
         args[1]->is_expensive_processor(arg);
}


bool Item_in_optimizer::is_expensive()
{
  DBUG_ASSERT(fixed());
  return args[0]->is_expensive() || args[1]->is_expensive();
}


longlong Item_func_eq::val_int()
{
  DBUG_ASSERT(fixed());
  int value= cmp.compare();
  return value == 0 ? 1 : 0;
}


/** Same as Item_func_eq, but NULL = NULL. */

bool Item_func_equal::fix_length_and_dec(THD *thd)
{
  bool rc= Item_bool_rowready_func2::fix_length_and_dec(thd);
  base_flags&= ~item_base_t::MAYBE_NULL;
  null_value=0;
  return rc;
}

longlong Item_func_equal::val_int()
{
  DBUG_ASSERT(fixed());
  return cmp.compare();
}

longlong Item_func_ne::val_int()
{
  DBUG_ASSERT(fixed());
  int value= cmp.compare();
  return value != 0 && !null_value ? 1 : 0;
}


longlong Item_func_ge::val_int()
{
  DBUG_ASSERT(fixed());
  int value= cmp.compare();
  return value >= 0 ? 1 : 0;
}


longlong Item_func_gt::val_int()
{
  DBUG_ASSERT(fixed());
  int value= cmp.compare();
  return value > 0 ? 1 : 0;
}

longlong Item_func_le::val_int()
{
  DBUG_ASSERT(fixed());
  int value= cmp.compare();
  return value <= 0 && !null_value ? 1 : 0;
}


longlong Item_func_lt::val_int()
{
  DBUG_ASSERT(fixed());
  int value= cmp.compare();
  return value < 0 && !null_value ? 1 : 0;
}


longlong Item_func_strcmp::val_int()
{
  DBUG_ASSERT(fixed());
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
  return Item_args::eq(item_func, binary_cmp);
}


bool Item_func_interval::fix_fields(THD *thd, Item **ref)
{
  if (Item_long_func::fix_fields(thd, ref))
    return true;
  for (uint i= 0 ; i < row->cols(); i++)
  {
    if (row->element_index(i)->check_cols(1))
      return true;
  }
  return false;
}


bool Item_func_interval::fix_length_and_dec(THD *thd)
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

    if (not_null_consts)
    {
      intervals= (interval_range*) current_thd->alloc(sizeof(interval_range) *
                                                         (rows - 1));
      if (!intervals)
        return TRUE;

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
  base_flags&= ~item_base_t::MAYBE_NULL;
  max_length= 2;
  used_tables_and_const_cache_join(row);
  not_null_tables_cache= row->not_null_tables();
  with_flags|= row->with_flags;
  return FALSE;
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
  DBUG_ASSERT(fixed());
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
        The values in the range interval may have different types,
        Only do a decimal comparison if the first argument is a decimal
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
      VDec e_dec(el);
      /* Skip NULL ranges. */
      if (e_dec.is_null())
        continue;
      if (e_dec.cmp(dec) > 0)
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


bool Item_func_between::eval_not_null_tables(void *opt_arg)
{
  if (Item_func_opt_neg::eval_not_null_tables(NULL))
    return 1;

  /* not_null_tables_cache == union(T1(e),T1(e1),T1(e2)) */
  if (is_top_level_item() && !negated)
    return 0;

  /* not_null_tables_cache == union(T1(e), intersection(T1(e1),T1(e2))) */
  not_null_tables_cache= (args[0]->not_null_tables() |
                          (args[1]->not_null_tables() &
                           args[2]->not_null_tables()));
  return 0;
}


bool Item_func_between::find_not_null_fields(table_map allowed)
{
  if (negated || !is_top_level_item() || (~allowed & used_tables()))
    return false;
  return args[0]->find_not_null_fields(allowed) ||
         args[1]->find_not_null_fields(allowed) ||
         args[2]->find_not_null_fields(allowed);
}


bool Item_func_between::count_sargable_conds(void *arg)
{
  SELECT_LEX *sel= (SELECT_LEX *) arg;
  sel->cond_count++;
  sel->between_count++;
  return 0;
}


void Item_func_between::fix_after_pullout(st_select_lex *new_parent,
                                          Item **ref, bool merge)
{
  /* This will re-calculate attributes of the arguments */
  Item_func_opt_neg::fix_after_pullout(new_parent, ref, merge);
  /* Then, re-calculate not_null_tables_cache according to our special rules */
  eval_not_null_tables(NULL);
}

bool Item_func_between::fix_length_and_dec(THD *thd)
{
  max_length= 1;

  /*
    As some compare functions are generated after sql_yacc,
    we have to check for out of memory conditions here
  */
  if (!args[0] || !args[1] || !args[2])
    return TRUE;
  if (m_comparator.aggregate_for_comparison(Item_func_between::
                                            func_name_cstring(),
                                            args, 3, false))
  {
    DBUG_ASSERT(thd->is_error());
    return TRUE;
  }

  return m_comparator.type_handler()->
    Item_func_between_fix_length_and_dec(this);
}


bool Item_func_between::fix_length_and_dec_numeric(THD *thd)
{
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
      {
        // Works for all types
        m_comparator.set_handler(&type_handler_slonglong);
      }
    }
  }
  return FALSE;
}


bool Item_func_between::fix_length_and_dec_temporal(THD *thd)
{
  if (!thd->lex->is_ps_or_view_context_analysis())
  {
    for (uint i= 0; i < 3; i ++)
    {
      if (args[i]->const_item() &&
          args[i]->type_handler_for_comparison() != m_comparator.type_handler())
      {
        Item_cache *cache= m_comparator.type_handler()->Item_get_cache(thd, args[i]);
        if (!cache || cache->setup(thd, args[i]))
          return true;
        thd->change_item_tree(&args[i], cache);
      }
    }
  }
  return false;
}


longlong Item_func_between::val_int_cmp_datetime()
{
  THD *thd= current_thd;
  longlong value= args[0]->val_datetime_packed(thd), a, b;
  if ((null_value= args[0]->null_value))
    return 0;
  a= args[1]->val_datetime_packed(thd);
  b= args[2]->val_datetime_packed(thd);
  return val_int_cmp_int_finalize(value, a, b);
}


longlong Item_func_between::val_int_cmp_time()
{
  THD *thd= current_thd;
  longlong value= args[0]->val_time_packed(thd), a, b;
  if ((null_value= args[0]->null_value))
    return 0;
  a= args[1]->val_time_packed(thd);
  b= args[2]->val_time_packed(thd);
  return val_int_cmp_int_finalize(value, a, b);
}


longlong Item_func_between::val_int_cmp_native()
{
  THD *thd= current_thd;
  const Type_handler *h= m_comparator.type_handler();
  NativeBuffer<STRING_BUFFER_USUAL_SIZE> value, a, b;
  if (val_native_with_conversion_from_item(thd, args[0], &value, h))
    return 0;
  bool ra= args[1]->val_native_with_conversion(thd, &a, h);
  bool rb= args[2]->val_native_with_conversion(thd, &b, h);
  if (!ra && !rb)
    return (longlong)
      ((h->cmp_native(value, a) >= 0 &&
        h->cmp_native(value, b) <= 0) != negated);
  if (ra && rb)
    null_value= true;
  else if (ra)
    null_value= h->cmp_native(value, b) <= 0;
  else
    null_value= h->cmp_native(value, a) >= 0;
  return (longlong) (!null_value && negated);
}


longlong Item_func_between::val_int_cmp_string()
{
  String *value,*a,*b;
  value=args[0]->val_str(&value0);
  if ((null_value=args[0]->null_value))
    return 0;
  a= args[1]->val_str(&value1);
  b= args[2]->val_str(&value2);
  if (!args[1]->null_value && !args[2]->null_value)
    return (longlong) ((sortcmp(value,a,cmp_collation.collation) >= 0 &&
                        sortcmp(value,b,cmp_collation.collation) <= 0) !=
                       negated);
  if (args[1]->null_value && args[2]->null_value)
    null_value= true;
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
  return (longlong) (!null_value && negated);
}


longlong Item_func_between::val_int_cmp_int()
{
  Longlong_hybrid value= args[0]->to_longlong_hybrid();
  if ((null_value= args[0]->null_value))
    return 0;					/* purecov: inspected */
  Longlong_hybrid a= args[1]->to_longlong_hybrid();
  Longlong_hybrid b= args[2]->to_longlong_hybrid();
  if (!args[1]->null_value && !args[2]->null_value)
    return (longlong) ((value.cmp(a) >= 0 && value.cmp(b) <= 0) != negated);
  if (args[1]->null_value && args[2]->null_value)
    null_value= true;
  else if (args[1]->null_value)
    null_value= value.cmp(b) <= 0;              // not null if false range.
  else
    null_value= value.cmp(a) >= 0;
  return (longlong) (!null_value && negated);
}


bool Item_func_between::val_int_cmp_int_finalize(longlong value,
                                                 longlong a,
                                                 longlong b)
{
  if (!args[1]->null_value && !args[2]->null_value)
    return (longlong) ((value >= a && value <= b) != negated);
  if (args[1]->null_value && args[2]->null_value)
    null_value= true;
  else if (args[1]->null_value)
    null_value= value <= b;			// not null if false range.
  else
    null_value= value >= a;
  return (longlong) (!null_value && negated);
}


longlong Item_func_between::val_int_cmp_decimal()
{
  VDec dec(args[0]);
  if ((null_value= dec.is_null()))
    return 0;					/* purecov: inspected */
  VDec a_dec(args[1]), b_dec(args[2]);
  if (!a_dec.is_null() && !b_dec.is_null())
    return (longlong) ((dec.cmp(a_dec) >= 0 &&
                        dec.cmp(b_dec) <= 0) != negated);
  if (a_dec.is_null() && b_dec.is_null())
    null_value= true;
  else if (a_dec.is_null())
    null_value= (dec.cmp(b_dec) <= 0);
  else
    null_value= (dec.cmp(a_dec) >= 0);
  return (longlong) (!null_value && negated);
}


longlong Item_func_between::val_int_cmp_real()
{
  double value= args[0]->val_real(),a,b;
  if ((null_value=args[0]->null_value))
    return 0;					/* purecov: inspected */
  a= args[1]->val_real();
  b= args[2]->val_real();
  if (!args[1]->null_value && !args[2]->null_value)
    return (longlong) ((value >= a && value <= b) != negated);
  if (args[1]->null_value && args[2]->null_value)
    null_value= true;
  else if (args[1]->null_value)
  {
    null_value= value <= b;			// not null if false range.
  }
  else
  {
    null_value= value >= a;
  }
  return (longlong) (!null_value && negated);
}


void Item_func_between::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, higher_precedence());
  if (negated)
    str->append(STRING_WITH_LEN(" not"));
  str->append(STRING_WITH_LEN(" between "));
  args[1]->print_parenthesised(str, query_type, precedence());
  str->append(STRING_WITH_LEN(" and "));
  args[2]->print_parenthesised(str, query_type, precedence());
}


double
Item_func_ifnull::real_op()
{
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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


bool Item_func_ifnull::native_op(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  if (!val_native_with_conversion_from_item(thd, args[0], to, type_handler()))
    return false;
  return val_native_with_conversion_from_item(thd, args[1], to, type_handler());
}


bool Item_func_ifnull::date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  for (uint i= 0; i < 2; i++)
  {
    Datetime_truncation_not_needed dt(thd, args[i],
                                      fuzzydate & ~TIME_FUZZY_DATES);
    if (!(dt.copy_to_mysql_time(ltime, mysql_timestamp_type())))
      return (null_value= false);
  }
  return (null_value= true);
}


bool Item_func_ifnull::time_op(THD *thd, MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed());
  for (uint i= 0; i < 2; i++)
  {
    if (!Time(thd, args[i]).copy_to_mysql_time(ltime))
      return (null_value= false);
  }
  return (null_value= true);
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
  DBUG_ASSERT(fixed() == 0);
  /*
    Mark that we don't care if args[0] is NULL or FALSE, we regard both cases as
    false.
  */
  args[0]->top_level_item();

  if (Item_func::fix_fields(thd, ref))
    return 1;

  return 0;
}


bool
Item_func_if::eval_not_null_tables(void *opt_arg)
{
  if (Item_func::eval_not_null_tables(NULL))
    return 1;

  not_null_tables_cache= (args[1]->not_null_tables() &
                          args[2]->not_null_tables());

  return 0;
}


void Item_func_if::fix_after_pullout(st_select_lex *new_parent,
                                     Item **ref, bool merge)
{
  /* This will re-calculate attributes of the arguments */
  Item_func::fix_after_pullout(new_parent, ref, merge);
  /* Then, re-calculate not_null_tables_cache according to our special rules */
  eval_not_null_tables(NULL);
}


void Item_func_nullif::split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                                      List<Item> &fields, uint flags)
{
  if (m_cache)
  {
    flags|= SPLIT_SUM_SKIP_REGISTERED; // See Item_func::split_sum_func
    m_cache->split_sum_func2_example(thd, ref_pointer_array, fields, flags);
    args[1]->split_sum_func2(thd, ref_pointer_array, fields, &args[1], flags);
  }
  else
  {
    Item_func::split_sum_func(thd, ref_pointer_array, fields, flags);
  }
}


bool Item_func_nullif::walk(Item_processor processor,
                            bool walk_subquery, void *arg)
{
  /*
    No needs to iterate through args[2] when it's just a copy of args[0].
    See MDEV-9712 Performance degradation of nested NULLIF
  */
  uint tmp_count= arg_count == 2 || args[0] == args[2] ? 2 : 3;
  for (uint i= 0; i < tmp_count; i++)
  {
    if (args[i]->walk(processor, walk_subquery, arg))
      return true;
  }
  return (this->*processor)(arg);
}


void Item_func_nullif::update_used_tables()
{
  if (m_cache)
  {
    used_tables_and_const_cache_init();
    used_tables_and_const_cache_update_and_join(m_cache->get_example());
    used_tables_and_const_cache_update_and_join(arg_count, args);
  }
  else
  {
    /*
      MDEV-9712 Performance degradation of nested NULLIF
      No needs to iterate through args[2] when it's just a copy of args[0].
    */
    DBUG_ASSERT(arg_count == 3);
    used_tables_and_const_cache_init();
    used_tables_and_const_cache_update_and_join(args[0] == args[2] ? 2 : 3,
                                                args);
  }
}



bool
Item_func_nullif::fix_length_and_dec(THD *thd)
{
  /*
    If this is the first invocation of fix_length_and_dec(), create the
    third argument as a copy of the first. This cannot be done before
    fix_fields(), because fix_fields() might replace items,
    for exampe NOT x --> x==0, or (SELECT 1) --> 1.
    See also class Item_func_nullif declaration.
  */
  if (arg_count == 2)
    args[arg_count++]= m_arg0 ? m_arg0 : args[0];

  /*
    At prepared statement EXECUTE time, args[0] can already
    point to a different Item, created during PREPARE time fix_length_and_dec().
    For example, if character set conversion was needed, arguments can look
    like this:

      args[0]= > Item_func_conv_charset \
                                         l_expr
      args[2]= >------------------------/

    Otherwise (during PREPARE or convensional execution),
    args[0] and args[2] should still point to the same original l_expr.
  */
  DBUG_ASSERT(args[0] == args[2] || thd->stmt_arena->is_stmt_execute());
  if (args[0]->type() == SUM_FUNC_ITEM &&
      !thd->lex->is_ps_or_view_context_analysis())
  {
    /*
      NULLIF(l_expr, r_expr)

        is calculated in the way to return a result equal to:

      CASE WHEN l_expr = r_expr THEN NULL ELSE r_expr END.

      There's nothing special with r_expr, because it's referenced
      only by args[1] and nothing else.

      l_expr needs a special treatment, as it's referenced by both
      args[0] and args[2] initially.

      args[2] is used to return the value. Afrer all transformations
      (e.g. in fix_length_and_dec(), equal field propagation, etc)
      args[2] points to a an Item which preserves the exact data type and
      attributes (e.g. collation) of the original l_expr.
      It can point:
      - to the original l_expr
      - to an Item_cache pointing to l_expr
      - to a constant of the same data type with l_expr.

      args[0] is used for comparison. It can be replaced:

      - to Item_func_conv_charset by character set aggregation routines
      - to a constant Item by equal field propagation routines
        (in case of Item_field)

      The data type and/or the attributes of args[0] can differ from
      the data type and the attributes of the original l_expr, to make
      it comparable to args[1] (which points to r_expr or its replacement).

      For aggregate functions we have to wrap the original args[0]/args[2]
      into Item_cache (see MDEV-9181). In this case the Item_cache
      instance becomes the subject to character set conversion instead of
      the original args[0]/args[2], while the original args[0]/args[2] get
      hidden inside the cache.

      Some examples of what NULLIF can end up with after argument
      substitution (we don't mention args[1] in some cases for simplicity):

      1. l_expr is not an aggregate function:

        a. No conversion happened.
           args[0] and args[2] were not replaced to something else
           (i.e. neither by character set conversion, nor by propagation):

          args[1] > r_expr
          args[0] \
                    l_expr
          args[2] /

        b. Conversion of args[0] happened:

           CREATE OR REPLACE TABLE t1 (
             a CHAR(10) CHARACTER SET latin1,
             b CHAR(10) CHARACTER SET utf8);
           SELECT * FROM t1 WHERE NULLIF(a,b);

           args[1] > r_expr                          (Item_field for t1.b)
           args[0] > Item_func_conv_charset\
                                            l_expr   (Item_field for t1.a)
           args[2] > ----------------------/

        c. Conversion of args[1] happened:

          CREATE OR REPLACE TABLE t1 (
            a CHAR(10) CHARACTER SET utf8,
            b CHAR(10) CHARACTER SET latin1);
          SELECT * FROM t1 WHERE NULLIF(a,b);

          args[1] > Item_func_conv_charset -> r_expr (Item_field for t1.b)
          args[0] \
                   l_expr                            (Item_field for t1.a)
          args[2] /

        d. Conversion of only args[0] happened (by equal field proparation):

           CREATE OR REPLACE TABLE t1 (
             a CHAR(10),
             b CHAR(10));
           SELECT * FROM t1 WHERE NULLIF(a,b) AND a='a';

           args[1] > r_expr            (Item_field for t1.b)
           args[0] > Item_string('a')  (constant replacement for t1.a)
           args[2] > l_expr            (Item_field for t1.a)

        e. Conversion of both args[0] and args[2] happened
           (by equal field propagation):

           CREATE OR REPLACE TABLE t1 (a INT,b INT);
           SELECT * FROM t1 WHERE NULLIF(a,b) AND a=5;

           args[1] > r_expr         (Item_field for "b")
           args[0] \
                    Item_int (5)    (constant replacement for "a")
           args[2] /

      2. In case if l_expr is an aggregate function:

        a. No conversion happened:

          args[0] \
                   Item_cache > l_expr
          args[2] /

        b. Conversion of args[0] happened:

          args[0] > Item_func_conv_charset \
                                            Item_cache > l_expr
          args[2] >------------------------/

        c. Conversion of both args[0] and args[2] happened.
           (e.g. by equal expression propagation)
           TODO: check if it's possible (and add an example query if so).
    */
    m_cache= args[0]->cmp_type() == STRING_RESULT ?
             new (thd->mem_root) Item_cache_str_for_nullif(thd, args[0]) :
             args[0]->get_cache(thd);
    if (!m_cache)
      return TRUE;
    m_cache->setup(thd, args[0]);
    m_cache->store(args[0]);
    m_cache->set_used_tables(args[0]->used_tables());
    thd->change_item_tree(&args[0], m_cache);
    thd->change_item_tree(&args[2], m_cache);
  }
  set_handler(args[2]->type_handler());
  collation.set(args[2]->collation);
  decimals= args[2]->decimals;
  unsigned_flag= args[2]->unsigned_flag;
  fix_char_length(args[2]->max_char_length());
  set_maybe_null();
  m_arg0= args[0];
  if (setup_args_and_comparator(thd, &cmp))
    return TRUE;
  /*
    A special code for EXECUTE..PREPARE.

    If args[0] did not change, then we don't remember it, as it can point
    to a temporary Item object which will be destroyed between PREPARE
    and EXECUTE. EXECUTE time fix_length_and_dec() will correctly set args[2]
    from args[0] again.

    If args[0] changed, then it can be Item_func_conv_charset() for the
    original args[0], which was permanently installed during PREPARE time
    into the item tree as a wrapper for args[0], using change_item_tree(), i.e.

      NULLIF(latin1_field, 'a' COLLATE utf8_bin)

    was "rewritten" to:

      CASE WHEN CONVERT(latin1_field USING utf8) = 'a' COLLATE utf8_bin
        THEN NULL
        ELSE latin1_field

    - m_args0 points to Item_field corresponding to latin1_field
    - args[0] points to Item_func_conv_charset
    - args[0]->args[0] is equal to m_args0
    - args[1] points to Item_func_set_collation
    - args[2] points is eqial to m_args0

    In this case we remember and reuse m_arg0 during EXECUTE time as args[2].

    QQ: How to make sure that m_args0 does not point
    to something temporary which will be destroyed between PREPARE and EXECUTE.
    The condition below should probably be more strict and somehow check that:
    - change_item_tree() was called for the new args[0]
    - m_args0 is referenced from inside args[0], e.g. as a function argument,
      and therefore it is also something that won't be destroyed between
      PREPARE and EXECUTE.
    Any ideas?
  */
  if (args[0] == m_arg0)
    m_arg0= NULL;
  return FALSE;
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
  if ((query_type & QT_ITEM_ORIGINAL_FUNC_NULLIF) ||
      (arg_count == 2) ||
      (args[0] == args[2]))
  {
    /*
      If QT_ITEM_ORIGINAL_FUNC_NULLIF is requested,
      that means we want the original NULLIF() representation,
      e.g. when we are in:
        SHOW CREATE {VIEW|FUNCTION|PROCEDURE}

      The original representation is possible only if
      args[0] and args[2] still point to the same Item.

      The caller must never pass call print() with QT_ITEM_ORIGINAL_FUNC_NULLIF
      if an expression has undergone some optimization
      (e.g. equal field propagation done in optimize_cond()) already and
      NULLIF() potentially has two different representations of "a":
      - one "a" for comparison
      - another "a" for the returned value!
    */
    DBUG_ASSERT(arg_count == 2 ||
                args[0] == args[2] || current_thd->lex->context_analysis_only);
    str->append(func_name_cstring());
    str->append('(');
    if (arg_count == 2)
      args[0]->print(str, query_type);
    else
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


int Item_func_nullif::compare()
{
  if (m_cache)
    m_cache->cache_value();
  return cmp.compare();
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
  DBUG_ASSERT(fixed());
  double value;
  if (!compare())
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
  DBUG_ASSERT(fixed());
  longlong value;
  if (!compare())
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
  DBUG_ASSERT(fixed());
  String *res;
  if (!compare())
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
  DBUG_ASSERT(fixed());
  my_decimal *res;
  if (!compare())
  {
    null_value=1;
    return 0;
  }
  res= args[2]->val_decimal(decimal_value);
  null_value= args[2]->null_value;
  return res;
}


bool
Item_func_nullif::date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  if (!compare())
    return (null_value= true);
  Datetime_truncation_not_needed dt(thd, args[2], fuzzydate);
  return (null_value= dt.copy_to_mysql_time(ltime, mysql_timestamp_type()));
}


bool
Item_func_nullif::time_op(THD *thd, MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed());
  if (!compare())
    return (null_value= true);
  return (null_value= Time(thd, args[2]).copy_to_mysql_time(ltime));

}


bool
Item_func_nullif::native_op(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  if (!compare())
    return (null_value= true);
  return val_native_with_conversion_from_item(thd, args[2], to, type_handler());
}


bool
Item_func_nullif::is_null()
{
  return (null_value= (!compare() ? 1 : args[2]->is_null()));
}

void Item_func_case::reorder_args(uint start)
{
  /*
    Reorder args, to have at first the optional CASE expression, then all WHEN
    expressions, then all THEN expressions. And the optional ELSE expression
    at the end.

    We reorder an even number of arguments, starting from start.
  */
  uint count = (arg_count - start) / 2;
  const size_t size= sizeof(Item*) * count * 2;
  Item **arg_buffer= (Item **)my_safe_alloca(size);
  memcpy(arg_buffer, &args[start], size);
  for (uint i= 0; i < count; i++)
  {
    args[start + i]= arg_buffer[i*2];
    args[start + i + count]= arg_buffer[i*2 + 1];
  }
  my_safe_afree(arg_buffer, size);
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

Item *Item_func_case_searched::find_item()
{
  uint count= when_count();
  for (uint i= 0 ; i < count ; i++)
  {
    if (args[i]->val_bool())
      return args[i + count];
  }
  Item **pos= Item_func_case_searched::else_expr_addr();
  return pos ? pos[0] : 0;
}


Item *Item_func_case_simple::find_item()
{
  /* Compare every WHEN argument with it and return the first match */
  uint idx;
  if (!Predicant_to_list_comparator::cmp(this, &idx, NULL))
    return args[idx + when_count()];
  Item **pos= Item_func_case_simple::else_expr_addr();
  return pos ? pos[0] : 0;
}


Item *Item_func_decode_oracle::find_item()
{
  uint idx;
  if (!Predicant_to_list_comparator::cmp_nulls_equal(current_thd, this, &idx))
    return args[idx + when_count()];
  Item **pos= Item_func_decode_oracle::else_expr_addr();
  return pos ? pos[0] : 0;
}


String *Item_func_case::str_op(String *str)
{
  DBUG_ASSERT(fixed());
  String *res;
  Item *item= find_item();

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
  DBUG_ASSERT(fixed());
  Item *item= find_item();
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
  DBUG_ASSERT(fixed());
  Item *item= find_item();
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
  DBUG_ASSERT(fixed());
  Item *item= find_item();
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


bool Item_func_case::date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  Item *item= find_item();
  if (!item)
    return (null_value= true);
  Datetime_truncation_not_needed dt(thd, item, fuzzydate);
  return (null_value= dt.copy_to_mysql_time(ltime, mysql_timestamp_type()));
}


bool Item_func_case::time_op(THD *thd, MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed());
  Item *item= find_item();
  if (!item)
    return (null_value= true);
  return (null_value= Time(thd, item).copy_to_mysql_time(ltime));
}


bool Item_func_case::native_op(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  Item *item= find_item();
  if (!item)
    return (null_value= true);
  return val_native_with_conversion_from_item(thd, item, to, type_handler());
}


bool Item_func_case::fix_fields(THD *thd, Item **ref)
{
  bool res= Item_func::fix_fields(thd, ref);

  Item **pos= else_expr_addr();
  if (!pos || pos[0]->maybe_null())
    set_maybe_null();
  return res;
}


/**
  Check if (*place) and new_value points to different Items and call
  THD::change_item_tree() if needed.
*/

static void propagate_and_change_item_tree(THD *thd, Item **place,
                                           COND_EQUAL *cond,
                                           const Item::Context &ctx)
{
  Item *new_value= (*place)->propagate_equal_fields(thd, ctx, cond);
  if (new_value && *place != new_value)
    thd->change_item_tree(place, new_value);
}


bool Item_func_case_simple::prepare_predicant_and_values(THD *thd,
                                                         uint *found_types,
                                                         bool nulls_equal)
{
  bool have_null= false;
  uint type_cnt;
  Type_handler_hybrid_field_type tmp;
  uint ncases= when_count();
  add_predicant(this, 0);
  for (uint i= 0 ; i < ncases; i++)
  {
    static LEX_CSTRING case_when= { STRING_WITH_LEN("case..when") };
    if (nulls_equal ?
        add_value(case_when, this, i + 1) :
        add_value_skip_null(case_when, this, i + 1, &have_null))
      return true;
  }
  all_values_added(&tmp, &type_cnt, &m_found_types);
#ifndef DBUG_OFF
  Predicant_to_list_comparator::debug_print(thd);
#endif
  return false;
}


bool Item_func_case_searched::fix_length_and_dec(THD *thd)
{
  return aggregate_then_and_else_arguments(thd, when_count());
}


bool Item_func_case_simple::fix_length_and_dec(THD *thd)
{
  return (aggregate_then_and_else_arguments(thd, when_count() + 1) ||
          aggregate_switch_and_when_arguments(thd, false));
}


bool Item_func_decode_oracle::fix_length_and_dec(THD *thd)
{
  return (aggregate_then_and_else_arguments(thd, when_count() + 1) ||
          aggregate_switch_and_when_arguments(thd, true));
}


/*
  Aggregate all THEN and ELSE expression types
  and collations when string result

  @param THD       - current thd
  @param start     - an element in args to start aggregating from
*/
bool Item_func_case::aggregate_then_and_else_arguments(THD *thd, uint start)
{
  if (aggregate_for_result(func_name_cstring(), args + start,
                           arg_count - start, true))
    return true;

  if (fix_attributes(args + start, arg_count - start))
    return true;

  return false;
}


/*
  Aggregate the predicant expression and all WHEN expression types
  and collations when string comparison
*/
bool Item_func_case_simple::aggregate_switch_and_when_arguments(THD *thd,
                                                                bool nulls_eq)
{
  uint ncases= when_count();
  m_found_types= 0;
  if (prepare_predicant_and_values(thd, &m_found_types, nulls_eq))
  {
    /*
      If Predicant_to_list_comparator() fails to prepare components,
      it must put an error into the diagnostics area. This is needed
      to make fix_fields() catches such errors.
    */
    DBUG_ASSERT(thd->is_error());
    return true;
  }

  if (!(m_found_types= collect_cmp_types(args, ncases + 1)))
    return true;

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
    if (agg_arg_charsets_for_comparison(cmp_collation, args, ncases + 1))
      return true;
  }

  if (make_unique_cmp_items(thd, cmp_collation.collation))
    return true;

  return false;
}


Item* Item_func_case_simple::propagate_equal_fields(THD *thd,
                                                    const Context &ctx,
                                                    COND_EQUAL *cond)
{
  const Type_handler *first_expr_cmp_handler;

  first_expr_cmp_handler= args[0]->type_handler_for_comparison();
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
  if (m_found_types == (1UL << first_expr_cmp_handler->cmp_type()))
    propagate_and_change_item_tree(thd, &args[0], cond,
      Context(ANY_SUBST, first_expr_cmp_handler, cmp_collation.collation));

  /*
    These arguments are in comparison.
    Allow invariants of the same value during propagation.
    Note, as we pass ANY_SUBST, none of the WHEN arguments will be
    replaced to zero-filled constants (only IDENTITY_SUBST allows this).
    Such a change for WHEN arguments would require rebuilding cmp_items.
  */
  uint i, count= when_count();
  for (i= 1; i <= count; i++)
  {
    Type_handler_hybrid_field_type tmp(first_expr_cmp_handler);
    if (!tmp.aggregate_for_comparison(args[i]->type_handler_for_comparison()))
      propagate_and_change_item_tree(thd, &args[i], cond,
        Context(ANY_SUBST, tmp.type_handler(), cmp_collation.collation));
  }

  // THEN and ELSE arguments (they are not in comparison)
  for (; i < arg_count; i++)
    propagate_and_change_item_tree(thd, &args[i], cond, Context_identity());

  return this;
}


inline void Item_func_case::print_when_then_arguments(String *str,
                                                      enum_query_type
                                                      query_type,
                                                      Item **items, uint count)
{
  for (uint i= 0; i < count; i++)
  {
    str->append(STRING_WITH_LEN("when "));
    items[i]->print(str, query_type);
    str->append(STRING_WITH_LEN(" then "));
    items[i + count]->print(str, query_type);
    str->append(' ');
  }
}


inline void Item_func_case::print_else_argument(String *str,
                                                enum_query_type query_type,
                                                Item *item)
{
  str->append(STRING_WITH_LEN("else "));
  item->print(str, query_type);
  str->append(' ');
}


void Item_func_case_searched::print(String *str, enum_query_type query_type)
{
  Item **pos;
  str->append(STRING_WITH_LEN("case "));
  print_when_then_arguments(str, query_type, &args[0], when_count());
  if ((pos= Item_func_case_searched::else_expr_addr()))
    print_else_argument(str, query_type, pos[0]);
  str->append(STRING_WITH_LEN("end"));
}


void Item_func_case_simple::print(String *str, enum_query_type query_type)
{
  Item **pos;
  str->append(STRING_WITH_LEN("case "));
  args[0]->print_parenthesised(str, query_type, precedence());
  str->append(' ');
  print_when_then_arguments(str, query_type, &args[1], when_count());
  if ((pos= Item_func_case_simple::else_expr_addr()))
    print_else_argument(str, query_type, pos[0]);
  str->append(STRING_WITH_LEN("end"));
}


void Item_func_decode_oracle::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  args[0]->print(str, query_type);
  for (uint i= 1, count= when_count() ; i <= count; i++)
  {
    str->append(',');
    args[i]->print(str, query_type);
    str->append(',');
    args[i+count]->print(str, query_type);
  }
  Item **else_expr= Item_func_case_simple::else_expr_addr();
  if (else_expr)
  {
    str->append(',');
    (*else_expr)->print(str, query_type);
  }
  str->append(')');
}


/**
  Coalesce - return first not NULL argument.
*/

String *Item_func_coalesce::str_op(String *str)
{
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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
  DBUG_ASSERT(fixed());
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


bool Item_func_coalesce::date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  for (uint i= 0; i < arg_count; i++)
  {
    Datetime_truncation_not_needed dt(thd, args[i],
                                      fuzzydate & ~TIME_FUZZY_DATES);
    if (!dt.copy_to_mysql_time(ltime, mysql_timestamp_type()))
      return (null_value= false);
  }
  return (null_value= true);
}


bool Item_func_coalesce::time_op(THD *thd, MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed());
  for (uint i= 0; i < arg_count; i++)
  {
    if (!Time(thd, args[i]).copy_to_mysql_time(ltime))
      return (null_value= false);
  }
  return (null_value= true);
}


bool Item_func_coalesce::native_op(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  for (uint i= 0; i < arg_count; i++)
  {
    if (!val_native_with_conversion_from_item(thd, args[i], to, type_handler()))
      return false;
  }
  return (null_value= true);
}


my_decimal *Item_func_coalesce::decimal_op(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
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


bool in_vector::find(Item *item)
{
  uchar *result=get_value(item);
  if (!result || !used_count)
    return false;				// Null value

  uint start,end;
  start=0; end=used_count-1;
  while (start != end)
  {
    uint mid=(start+end+1)/2;
    int res;
    if ((res=(*compare)(collation, base+mid*size, result)) == 0)
      return true;
    if (res < 0)
      start=mid;
    else
      end=mid-1;
  }
  return ((*compare)(collation, base+start*size, result) == 0);
}

in_string::in_string(THD *thd, uint elements, qsort2_cmp cmp_func,
                     CHARSET_INFO *cs)
  :in_vector(thd, elements, sizeof(String), cmp_func, cs),
   tmp(buff, sizeof(buff), &my_charset_bin)
{}

in_string::~in_string()
{
  if (base)
  {
    // base was allocated on THD::mem_root => following is OK
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
  DBUG_PRINT("enter", ("pos: %u  item: %p", pos,item));
  ((cmp_item_row*) base)[pos].store_value_by_template(current_thd, &tmp, item);
  DBUG_VOID_RETURN;
}

in_longlong::in_longlong(THD *thd, uint elements)
  :in_vector(thd, elements, sizeof(packed_longlong),
             (qsort2_cmp) cmp_longlong, 0)
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


static int cmp_timestamp(void *cmp_arg,
                         Timestamp_or_zero_datetime *a,
                         Timestamp_or_zero_datetime *b)
{
  return a->cmp(*b);
}


in_timestamp::in_timestamp(THD *thd, uint elements)
  :in_vector(thd, elements, sizeof(Value), (qsort2_cmp) cmp_timestamp, 0)
{}


void in_timestamp::set(uint pos, Item *item)
{
  Timestamp_or_zero_datetime *buff= &((Timestamp_or_zero_datetime *) base)[pos];
  Timestamp_or_zero_datetime_native_null native(current_thd, item, true);
  if (native.is_null())
    *buff= Timestamp_or_zero_datetime();
  else
    *buff= Timestamp_or_zero_datetime(native);
}


uchar *in_timestamp::get_value(Item *item)
{
  Timestamp_or_zero_datetime_native_null native(current_thd, item, true);
  if (native.is_null())
    return 0;
  tmp= Timestamp_or_zero_datetime(native);
  return (uchar*) &tmp;
}


Item *in_timestamp::create_item(THD *thd)
{
  return new (thd->mem_root) Item_timestamp_literal(thd);
}


void in_timestamp::value_to_item(uint pos, Item *item)
{
  const Timestamp_or_zero_datetime &buff= (((Timestamp_or_zero_datetime*) base)[pos]);
  static_cast<Item_timestamp_literal*>(item)->set_value(buff);
}


void in_datetime::set(uint pos,Item *item)
{
  struct packed_longlong *buff= &((packed_longlong*) base)[pos];

  buff->val= item->val_datetime_packed(current_thd);
  buff->unsigned_flag= 1L;
}

void in_time::set(uint pos,Item *item)
{
  struct packed_longlong *buff= &((packed_longlong*) base)[pos];

  buff->val= item->val_time_packed(current_thd);
  buff->unsigned_flag= 1L;
}

uchar *in_datetime::get_value(Item *item)
{
  tmp.val= item->val_datetime_packed(current_thd);
  if (item->null_value)
    return 0;
  tmp.unsigned_flag= 1L;
  return (uchar*) &tmp;
}

uchar *in_time::get_value(Item *item)
{
  tmp.val= item->val_time_packed(current_thd);
  if (item->null_value)
    return 0;
  tmp.unsigned_flag= 1L;
  return (uchar*) &tmp;
}

Item *in_temporal::create_item(THD *thd)
{ 
  return new (thd->mem_root) Item_datetime(thd);
}


in_double::in_double(THD *thd, uint elements)
  :in_vector(thd, elements, sizeof(double), (qsort2_cmp) cmp_double, 0)
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


in_decimal::in_decimal(THD *thd, uint elements)
  :in_vector(thd, elements, sizeof(my_decimal), (qsort2_cmp) cmp_decimal, 0)
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


bool Predicant_to_list_comparator::alloc_comparators(THD *thd, uint nargs)
{
  size_t nbytes= sizeof(Predicant_to_value_comparator) * nargs;
  if (!(m_comparators= (Predicant_to_value_comparator *) thd->alloc(nbytes)))
    return true;
  memset(m_comparators, 0, nbytes);
  return false;
}


bool Predicant_to_list_comparator::add_value(const LEX_CSTRING &funcname,
                                             Item_args *args,
                                             uint value_index)
{
  DBUG_ASSERT(m_predicant_index < args->argument_count());
  DBUG_ASSERT(value_index < args->argument_count());
  Type_handler_hybrid_field_type tmp;
  Item *tmpargs[2];
  tmpargs[0]= args->arguments()[m_predicant_index];
  tmpargs[1]= args->arguments()[value_index];
  if (tmp.aggregate_for_comparison(funcname, tmpargs, 2, true))
  {
    DBUG_ASSERT(current_thd->is_error());
    return true;
  }
  m_comparators[m_comparator_count].m_handler= tmp.type_handler();
  m_comparators[m_comparator_count].m_arg_index= value_index;
  m_comparator_count++;
  return false;
}


bool Predicant_to_list_comparator::
add_value_skip_null(const LEX_CSTRING &funcname,
                    Item_args *args,
                    uint value_index,
                    bool *nulls_found)
{
  /*
    Skip explicit NULL constant items.
    Using real_item() to correctly detect references to explicit NULLs
    in HAVING clause, e.g. in this example "b" is skipped:
      SELECT a,NULL AS b FROM t1 GROUP BY a HAVING 'A' IN (b,'A');
  */
  if (args->arguments()[value_index]->real_item()->type() == Item::NULL_ITEM)
  {
    *nulls_found= true;
    return false;
  }
  return add_value(funcname, args, value_index);
}


void Predicant_to_list_comparator::
       detect_unique_handlers(Type_handler_hybrid_field_type *compatible,
                              uint *unique_count,
                              uint *found_types)
{
  *unique_count= 0;
  *found_types= 0;
  for (uint i= 0; i < m_comparator_count; i++)
  {
    uint idx;
    if (find_handler(&idx, m_comparators[i].m_handler, i))
    {
      m_comparators[i].m_handler_index= i; // New unique handler
      (*unique_count)++;
      (*found_types)|= 1U << m_comparators[i].m_handler->cmp_type();
      compatible->set_handler(m_comparators[i].m_handler);
    }
    else
    {
      m_comparators[i].m_handler_index= idx; // Non-unique handler
    }
  }
}


bool Predicant_to_list_comparator::make_unique_cmp_items(THD *thd,
                                                         CHARSET_INFO *cs)
{
  for (uint i= 0; i < m_comparator_count; i++)
  {
    if (m_comparators[i].m_handler &&                   // Skip implicit NULLs
        m_comparators[i].m_handler_index == i && // Skip non-unuque
        !(m_comparators[i].m_cmp_item=
          m_comparators[i].m_handler->make_cmp_item(thd, cs)))
       return true;
  }
  return false;
}


cmp_item* cmp_item_sort_string::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_sort_string_in_static(cmp_charset);
}

cmp_item* cmp_item_int::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_int();
}

cmp_item* cmp_item_real::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_real();
}

cmp_item* cmp_item_row::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_row();
}


cmp_item_row::~cmp_item_row()
{
  DBUG_ENTER("~cmp_item_row");
  DBUG_PRINT("enter",("this: %p", this));
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


bool cmp_item_row::alloc_comparators(THD *thd, uint cols)
{
  if (comparators)
  {
    DBUG_ASSERT(cols == n);
    return false;
  }
  return
    !(comparators= (cmp_item **) thd->calloc(sizeof(cmp_item *) * (n= cols)));
}


void cmp_item_row::store_value(Item *item)
{
  DBUG_ENTER("cmp_item_row::store_value");
  DBUG_ASSERT(comparators);
  DBUG_ASSERT(n == item->cols());
  item->bring_value();
  item->null_value= 0;
  for (uint i=0; i < n; i++)
  {
    DBUG_ASSERT(comparators[i]);
    comparators[i]->store_value(item->element_index(i));
    item->null_value|= item->element_index(i)->null_value;
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
      if (!(comparators[i]= tmpl->comparators[i]->make_same(thd)))
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
    const int rc= comparators[i]->cmp(arg->element_index(i));
    switch (rc)
    {
    case UNKNOWN:
      was_null= true;
      break;
    case TRUE:
      return TRUE;
    case FALSE:
      break;                                    // elements #i are equal
    }
    arg->null_value|= arg->element_index(i)->null_value;
  }
  return was_null ? UNKNOWN : FALSE;
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
  m_null_value= item->null_value;
}


int cmp_item_decimal::cmp_not_null(const Value *val)
{
  DBUG_ASSERT(!val->is_null());
  DBUG_ASSERT(val->is_decimal());
  return my_decimal_cmp(&value, &val->m_decimal);
}


int cmp_item_decimal::cmp(Item *arg)
{
  VDec tmp(arg);
  return m_null_value || tmp.is_null() ? UNKNOWN : (tmp.cmp(&value) != 0);
}


int cmp_item_decimal::compare(cmp_item *arg)
{
  cmp_item_decimal *l_cmp= (cmp_item_decimal*) arg;
  return my_decimal_cmp(&value, &l_cmp->value);
}


cmp_item* cmp_item_decimal::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_decimal();
}


int cmp_item_datetime::cmp_not_null(const Value *val)
{
  DBUG_ASSERT(!val->is_null());
  DBUG_ASSERT(val->is_temporal());
  return value != pack_time(&val->value.m_time);
}


int cmp_item_datetime::cmp(Item *arg)
{
  const bool rc= value != arg->val_datetime_packed(current_thd);
  return (m_null_value || arg->null_value) ? UNKNOWN : rc;
}


int cmp_item_time::cmp_not_null(const Value *val)
{
  DBUG_ASSERT(!val->is_null());
  DBUG_ASSERT(val->is_temporal());
  return value != pack_time(&val->value.m_time);
}


int cmp_item_time::cmp(Item *arg)
{
  const bool rc= value != arg->val_time_packed(current_thd);
  return (m_null_value || arg->null_value) ? UNKNOWN : rc;
}


int cmp_item_temporal::compare(cmp_item *ci)
{
  cmp_item_temporal *l_cmp= (cmp_item_temporal *)ci;
  return (value < l_cmp->value) ? -1 : ((value == l_cmp->value) ? 0 : 1);
}


cmp_item *cmp_item_datetime::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_datetime();
}


cmp_item *cmp_item_time::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_time();
}


void cmp_item_timestamp::store_value(Item *item)
{
  item->val_native_with_conversion(current_thd, &m_native,
                                   &type_handler_timestamp2);
  m_null_value= item->null_value;
}


int cmp_item_timestamp::cmp_not_null(const Value *val)
{
  /*
    This method will be implemented when we add this syntax:
      SELECT TIMESTAMP WITH LOCAL TIME ZONE '2001-01-01 10:20:30'
    For now TIMESTAMP is compared to non-TIMESTAMP using DATETIME.
  */
  DBUG_ASSERT(0);
  return 0;
}


int cmp_item_timestamp::cmp(Item *arg)
{
  THD *thd= current_thd;
  Timestamp_or_zero_datetime_native_null tmp(thd, arg, true);
  return m_null_value || tmp.is_null() ? UNKNOWN :
         type_handler_timestamp2.cmp_native(m_native, tmp) != 0;
}


int cmp_item_timestamp::compare(cmp_item *arg)
{
  cmp_item_timestamp *tmp= static_cast<cmp_item_timestamp*>(arg);
  return type_handler_timestamp2.cmp_native(m_native, tmp->m_native);
}


cmp_item* cmp_item_timestamp::make_same(THD *thd)
{
  return new (thd->mem_root) cmp_item_timestamp();
}



bool Item_func_in::count_sargable_conds(void *arg)
{
  ((SELECT_LEX*) arg)->cond_count++;
  return 0;
}


bool Item_func_in::list_contains_null()
{
  Item **arg,**arg_end;
  for (arg= args + 1, arg_end= args+arg_count; arg != arg_end ; arg++)
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
Item_func_in::eval_not_null_tables(void *opt_arg)
{
  Item **arg, **arg_end;

  if (Item_func_opt_neg::eval_not_null_tables(NULL))
    return 1;

  /* not_null_tables_cache == union(T1(e),union(T1(ei))) */
  if (is_top_level_item() && negated)
    return 0;

  /* not_null_tables_cache = union(T1(e),intersection(T1(ei))) */
  not_null_tables_cache= ~(table_map) 0;
  for (arg= args + 1, arg_end= args + arg_count; arg != arg_end; arg++)
    not_null_tables_cache&= (*arg)->not_null_tables();
  not_null_tables_cache|= (*args)->not_null_tables();
  return 0;
}


bool
Item_func_in::find_not_null_fields(table_map allowed)
{
  if (negated || !is_top_level_item() || (~allowed & used_tables()))
    return 0;
  return  args[0]->find_not_null_fields(allowed);
}


void Item_func_in::fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                     bool merge)
{
  /* This will re-calculate attributes of the arguments */
  Item_func_opt_neg::fix_after_pullout(new_parent, ref, merge);
  /* Then, re-calculate not_null_tables_cache according to our special rules */
  eval_not_null_tables(NULL);
}


bool Item_func_in::prepare_predicant_and_values(THD *thd, uint *found_types)
{
  uint type_cnt;
  have_null= false;

  add_predicant(this, 0);
  for (uint i= 1 ; i < arg_count; i++)
  {
    if (add_value_skip_null(Item_func_in::func_name_cstring(), this, i,
                            &have_null))
      return true;
  }
  all_values_added(&m_comparator, &type_cnt, found_types);
  arg_types_compatible= type_cnt < 2;

#ifndef DBUG_OFF
  Predicant_to_list_comparator::debug_print(thd);
#endif
  return false;
}


bool Item_func_in::fix_length_and_dec(THD *thd)
{
  uint found_types;
  m_comparator.set_handler(type_handler_varchar.type_handler_for_comparison());
  max_length= 1;

  if (prepare_predicant_and_values(thd, &found_types))
  {
    DBUG_ASSERT(thd->is_error()); // Must set error
    return TRUE;
  }

  if (arg_types_compatible) // Bisection condition #1
  {
    if (m_comparator.type_handler()->
        Item_func_in_fix_comparator_compatible_types(thd, this))
      return TRUE;
  }
  else
  {
    DBUG_ASSERT(m_comparator.cmp_type() != ROW_RESULT);
    if ( fix_for_scalar_comparison_using_cmp_items(thd, found_types))
      return TRUE;
  }

  DBUG_EXECUTE_IF("Item_func_in",
                  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                  ER_UNKNOWN_ERROR, "DBUG: types_compatible=%s bisect=%s",
                  arg_types_compatible ? "yes" : "no",
                  array != NULL ? "yes" : "no"););
  return FALSE;
}


/**
  Populate Item_func_in::array with constant not-NULL arguments and sort them.

  Sets "have_null" to true if some of the values appeared to be NULL.
  Note, explicit NULLs were found during prepare_predicant_and_values().
  So "have_null" can already be true before the fix_in_vector() call.
  Here we additionally catch implicit NULLs.
*/
void Item_func_in::fix_in_vector()
{
  DBUG_ASSERT(array);
  uint j=0;
  for (uint i=1 ; i < arg_count ; i++)
  {
    array->set(j,args[i]);
    if (!args[i]->null_value)
      j++; // include this cell in the array.
    else
    {
      /*
        We don't put NULL values in array, to avoid erronous matches in
        bisection.
      */
      have_null= 1;
    }
  }
  if ((array->used_count= j))
    array->sort();
}


/**
  Convert all items in <in value list> to INT.

  IN must compare INT columns and constants as int values (the same
  way as equality does).
  So we must check here if the column on the left and all the constant
  values on the right can be compared as integers and adjust the
  comparison type accordingly.

  See the comment about the similar block in Item_bool_func2
*/
bool Item_func_in::value_list_convert_const_to_int(THD *thd)
{
  if (args[0]->real_item()->type() == FIELD_ITEM &&
      !thd->lex->is_view_context_analysis())
  {
    Item_field *field_item= (Item_field*) (args[0]->real_item());
    if (field_item->field_type() == MYSQL_TYPE_LONGLONG ||
        field_item->field_type() == MYSQL_TYPE_YEAR)
    {
      bool all_converted= true;
      Item **arg, **arg_end;
      for (arg=args+1, arg_end=args+arg_count; arg != arg_end ; arg++)
      {
          /*
            Explicit NULLs should not affect data cmp_type resolution:
            - we ignore NULLs when calling collect_cmp_type()
            - we ignore NULLs here
            So this expression:
              year_column IN (DATE'2001-01-01', NULL)
            switches from TIME_RESULT to INT_RESULT.
          */
          if (arg[0]->type() != Item::NULL_ITEM &&
              !convert_const_to_int(thd, field_item, &arg[0]))
           all_converted= false;
      }
      if (all_converted)
        m_comparator.set_handler(&type_handler_slonglong);
    }
  }
  return thd->is_fatal_error; // Catch errrors in convert_const_to_int
}


bool cmp_item_row::
      aggregate_row_elements_for_comparison(THD *thd,
                                            Type_handler_hybrid_field_type *cmp,
                                            Item_args *tmp,
                                            const LEX_CSTRING &funcname,
                                            uint col,
                                            uint level)
{
  DBUG_EXECUTE_IF("cmp_item",
  {
    for (uint i= 0 ; i < tmp->argument_count(); i++)
    {
      Item *arg= tmp->arguments()[i];
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_UNKNOWN_ERROR, "DBUG: %*s[%d,%d] handler=%s",
                          level, "", col, i,
                          arg->type_handler()->name().ptr());
    }
  }
  );
  bool err= cmp->aggregate_for_comparison(funcname, tmp->arguments(),
                                          tmp->argument_count(), true);
  DBUG_EXECUTE_IF("cmp_item",
  {
    if (!err)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_UNKNOWN_ERROR, "DBUG: %*s=> handler=%s",
                          level,"",
                          cmp->type_handler()->name().ptr());
  }
  );
  return err;
}


bool cmp_item_row::prepare_comparators(THD *thd, const LEX_CSTRING &funcname,
                                       const Item_args *args, uint level)
{
  DBUG_EXECUTE_IF("cmp_item",
                  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                  ER_UNKNOWN_ERROR, "DBUG: %*sROW(%d args) level=%d",
                                      level,"",
                  args->argument_count(), level););
  DBUG_ASSERT(args->argument_count() > 0);
  if (alloc_comparators(thd, args->arguments()[0]->cols()))
    return true;
  DBUG_ASSERT(n == args->arguments()[0]->cols());
  for (uint col= 0; col < n; col++)
  {
    Item_args tmp;
    Type_handler_hybrid_field_type cmp;

    if (tmp.alloc_and_extract_row_elements(thd, args, col) ||
        aggregate_row_elements_for_comparison(thd, &cmp, &tmp,
                                              funcname, col, level + 1))
      return true;

    /*
      There is a legacy bug (MDEV-11511) in the code below,
      which should be fixed eventually.
      When performing:
       (predicant0,predicant1) IN ((value00,value01),(value10,value11))
      It uses only the data type and the collation of the predicant
      elements only. It should be fixed to take into account the data type and
      the collation for all elements at the N-th positions of the
      predicate and all values:
      - predicate0, value00, value01
      - predicate1, value10, value11
    */
    Item *item0= args->arguments()[0]->element_index(col);
    CHARSET_INFO *collation= item0->collation.collation;
    if (!(comparators[col]= cmp.type_handler()->make_cmp_item(thd, collation)))
      return true;
    if (cmp.type_handler() == &type_handler_row)
    {
      // Prepare comparators for ROW elements recursively
      cmp_item_row *row= static_cast<cmp_item_row*>(comparators[col]);
      if (row->prepare_comparators(thd, funcname, &tmp, level + 1))
        return true;
    }
  }
  return false;
}


bool Item_func_in::fix_for_row_comparison_using_bisection(THD *thd)
{
  if (unlikely(!(array= new (thd->mem_root) in_row(thd, arg_count-1, 0))))
    return true;
  cmp_item_row *cmp= &((in_row*)array)->tmp;
  if (cmp->prepare_comparators(thd, func_name_cstring(), this, 0))
    return true;
  fix_in_vector();
  return false;
}


/**
  This method is called for scalar data types when bisection is not possible,
    for example:
  - Some of args[1..arg_count] are not constants.
  - args[1..arg_count] are constants, but pairs {args[0],args[1..arg_count]}
    are compared by different data types, e.g.:
      WHERE decimal_expr IN (1, 1e0)
    The pair {args[0],args[1]} is compared by type_handler_decimal.
    The pair {args[0],args[2]} is compared by type_handler_double.
*/
bool Item_func_in::fix_for_scalar_comparison_using_cmp_items(THD *thd,
                                                             uint found_types)
{
  if (found_types & (1U << STRING_RESULT) &&
      agg_arg_charsets_for_comparison(cmp_collation, args, arg_count))
    return true;
  if (make_unique_cmp_items(thd, cmp_collation.collation))
    return true;
  return false;
}


/**
  This method is called for the ROW data type when bisection is not possible.
*/
bool Item_func_in::fix_for_row_comparison_using_cmp_items(THD *thd)
{
  if (make_unique_cmp_items(thd, cmp_collation.collation))
    return true;
  DBUG_ASSERT(get_comparator_type_handler(0) == &type_handler_row);
  DBUG_ASSERT(get_comparator_cmp_item(0));
  cmp_item_row *cmp_row= (cmp_item_row*) get_comparator_cmp_item(0);
  return cmp_row->prepare_comparators(thd, func_name_cstring(), this, 0);
}


void Item_func_in::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, precedence());
  if (negated)
    str->append(STRING_WITH_LEN(" not"));
  str->append(STRING_WITH_LEN(" in ("));
  print_args(str, 1, query_type);
  str->append(STRING_WITH_LEN(")"));
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
  DBUG_ASSERT(fixed());
  if (array)
  {
    bool tmp=array->find(args[0]);
    /*
      NULL on left -> UNKNOWN.
      Found no match, and NULL on right -> UNKNOWN.
      NULL on right can never give a match, as it is not stored in
      array.
      See also the 'bisection_possible' variable in fix_length_and_dec().
    */
    null_value=args[0]->null_value || (!tmp && have_null);
    return (longlong) (!null_value && tmp != negated);
  }

  if ((null_value= args[0]->real_item()->type() == NULL_ITEM))
    return 0;

  null_value= have_null;
  uint idx;
  if (!Predicant_to_list_comparator::cmp(this, &idx, &null_value))
  {
    null_value= false;
    return (longlong) (!negated);
  }
  return (longlong) (!null_value && negated);
}


void Item_func_in::mark_as_condition_AND_part(TABLE_LIST *embedding)
{
  THD *thd= current_thd;

  Query_arena *arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);

  if (!transform_into_subq_checked)
  {
    if ((transform_into_subq= to_be_transformed_into_in_subq(thd)))
      thd->lex->current_select->in_funcs.push_back(this, thd->mem_root);
    transform_into_subq_checked= true;
  }

  if (arena)
    thd->restore_active_arena(arena, &backup);

  emb_on_expr_nest= embedding;
}


class Func_handler_bit_or_int_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    Longlong_null a= item->arguments()[0]->to_longlong_null();
    return a.is_null() ? a : a | item->arguments()[1]->to_longlong_null();
  }
};


class Func_handler_bit_or_dec_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    VDec a(item->arguments()[0]);
    return a.is_null() ? Longlong_null() :
      a.to_xlonglong_null() | VDec(item->arguments()[1]).to_xlonglong_null();
  }
};


bool Item_func_bit_or::fix_length_and_dec(THD *thd)
{
  static Func_handler_bit_or_int_to_ulonglong ha_int_to_ull;
  static Func_handler_bit_or_dec_to_ulonglong ha_dec_to_ull;
  return fix_length_and_dec_op2_std(&ha_int_to_ull, &ha_dec_to_ull);
}


class Func_handler_bit_and_int_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    Longlong_null a= item->arguments()[0]->to_longlong_null();
    return a.is_null() ? a : a & item->arguments()[1]->to_longlong_null();
  }
};


class Func_handler_bit_and_dec_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    VDec a(item->arguments()[0]);
    return a.is_null() ?  Longlong_null() :
      a.to_xlonglong_null() & VDec(item->arguments()[1]).to_xlonglong_null();
  }
};


bool Item_func_bit_and::fix_length_and_dec(THD *thd)
{
  static Func_handler_bit_and_int_to_ulonglong ha_int_to_ull;
  static Func_handler_bit_and_dec_to_ulonglong ha_dec_to_ull;
  return fix_length_and_dec_op2_std(&ha_int_to_ull, &ha_dec_to_ull);
}

Item_cond::Item_cond(THD *thd, Item_cond *item)
  :Item_bool_func(thd, item),
   and_tables_cache(item->and_tables_cache)
{
  base_flags|= (item->base_flags & item_base_t::AT_TOP_LEVEL);

  /*
    item->list will be copied by copy_andor_arguments() call
  */
}


Item_cond::Item_cond(THD *thd, Item *i1, Item *i2):
  Item_bool_func(thd)
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
  DBUG_ASSERT(fixed() == 0);
  List_iterator<Item> li(list);
  Item *item;
  uchar buff[sizeof(char*)];			// Max local vars in function
  bool is_and_cond= functype() == Item_func::COND_AND_FUNC;
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
    if (is_top_level_item())
      item->top_level_item();

    /*
      replace degraded condition:
        was:    <field>
        become: <field> = 1
    */
    Item::Type type= item->type();
    if (type == Item::FIELD_ITEM || type == Item::REF_ITEM)
    {
      Query_arena backup, *arena;
      Item *new_item;
      arena= thd->activate_stmt_arena_if_needed(&backup);
      if ((new_item= new (thd->mem_root) Item_func_ne(thd, item, new (thd->mem_root) Item_int(thd, 0, 1))))
        li.replace(item= new_item);
      if (arena)
        thd->restore_active_arena(arena, &backup);
    }

    if (item->fix_fields_if_needed_for_bool(thd, li.ref()))
      return TRUE; /* purecov: inspected */
    item= *li.ref(); // item can be substituted in fix_fields
    used_tables_cache|=     item->used_tables();
    if (item->can_eval_in_optimize() && !item->with_sp_var() &&
        !cond_has_datetime_is_null(item))
    {
      if (item->eval_const_cond() == is_and_cond && is_top_level_item())
      {
        /* 
          a. This is "... AND true_cond AND ..."
          In this case, true_cond  has no effect on cond_and->not_null_tables()
          b. This is "... OR false_cond/null cond OR ..." 
          In this case, false_cond has no effect on cond_or->not_null_tables()
        */
      }
      else
      {
        /* 
          a. This is "... AND false_cond/null_cond AND ..."
          The whole condition is FALSE/UNKNOWN.
          b. This is  "... OR const_cond OR ..."
          In this case, cond_or->not_null_tables()=0, because the condition
          const_cond might evaluate to true (regardless of whether some tables
          were NULL-complemented).
        */
        not_null_tables_cache= (table_map) 0;
        and_tables_cache= (table_map) 0;
      }
      if (thd->is_error())
        return TRUE;
    }
    else
    {
      table_map tmp_table_map= item->not_null_tables();
      not_null_tables_cache|= tmp_table_map;
      and_tables_cache&= tmp_table_map;

      const_item_cache= FALSE;
    } 
    base_flags|= item->base_flags & item_base_t::MAYBE_NULL;
    with_flags|= item->with_flags;
  }
  if (fix_length_and_dec(thd))
    return TRUE;
  base_flags|= item_base_t::FIXED;
  return FALSE;
}


bool
Item_cond::eval_not_null_tables(void *opt_arg)
{
  Item *item;
  bool is_and_cond= functype() == Item_func::COND_AND_FUNC;
  List_iterator<Item> li(list);
  not_null_tables_cache= (table_map) 0;
  and_tables_cache= ~(table_map) 0;
  while ((item=li++))
  {
    table_map tmp_table_map;
    if (item->can_eval_in_optimize() && !item->with_sp_var() &&
        !cond_has_datetime_is_null(item))
    {
      if (item->eval_const_cond() == is_and_cond && is_top_level_item())
      {
        /* 
          a. This is "... AND true_cond AND ..."
          In this case, true_cond  has no effect on cond_and->not_null_tables()
          b. This is "... OR false_cond/null cond OR ..." 
          In this case, false_cond has no effect on cond_or->not_null_tables()
        */
      }
      else
      {
        /* 
          a. This is "... AND false_cond/null_cond AND ..."
          The whole condition is FALSE/UNKNOWN.
          b. This is  "... OR const_cond OR ..."
          In this case, cond_or->not_null_tables()=0, because the condition
          const_cond might evaluate to true (regardless of whether some tables
          were NULL-complemented).
        */
        not_null_tables_cache= (table_map) 0;
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


/**
   @note
     This implementation of the virtual function find_not_null_fields()
     infers null-rejectedness if fields from tables marked in 'allowed' from
     this condition.
     Currently only top level AND conjuncts that  are not disjunctions are used
     for the inference. Usage of any top level and-or formula with l OR levels
     would require a stack of bitmaps for fields of the height h=2*l+1 So we
     would have to allocate h-1 additional field bitmaps for each table marked
     in 'allowed'.
*/

bool
Item_cond::find_not_null_fields(table_map allowed)
{
  Item *item;
  bool is_and_cond= functype() == Item_func::COND_AND_FUNC;
  if (!is_and_cond)
  {
    /* Now only fields of top AND level conjuncts are taken into account */
    return false;
  }
  uint isnull_func_cnt= 0;
  List_iterator<Item> li(list);
  while ((item=li++))
  {
    bool is_mult_eq= item->type() == Item::FUNC_ITEM &&
         ((Item_func *) item)->functype() == Item_func::MULT_EQUAL_FUNC;
    if (is_mult_eq)
    {
      if (!item->find_not_null_fields(allowed))
        continue;
    }

    if (~allowed & item->used_tables())
      continue;

    /* It is assumed that all constant conjuncts are already eliminated */

    /*
      First infer null-rejectedness of fields from all conjuncts but
      IS NULL predicates
    */
    bool isnull_func= item->type() == Item::FUNC_ITEM &&
         ((Item_func *) item)->functype() == Item_func::ISNULL_FUNC;
    if (isnull_func)
    {
      isnull_func_cnt++;
      continue;
    }
    if (!item->find_not_null_fields(allowed))
      continue;
  }

  /* Now try no get contradictions using IS NULL conjuncts */
  if (isnull_func_cnt)
  {
    li.rewind();
    while ((item=li++) && isnull_func_cnt)
    {
      if (~allowed & item->used_tables())
        continue;

      bool isnull_func= item->type() == Item::FUNC_ITEM &&
           ((Item_func *) item)->functype() == Item_func::ISNULL_FUNC;
      if (isnull_func)
      {
        if  (item->find_not_null_fields(allowed))
          return true;
        isnull_func_cnt--;
      }
    }
  }
  return false;
}

void Item_cond::fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                  bool merge)
{
  List_iterator<Item> li(list);
  Item *item;

  used_tables_and_const_cache_init();

  and_tables_cache= ~(table_map) 0; // Here and below we do as fix_fields does
  not_null_tables_cache= 0;

  while ((item=li++))
  {
    table_map tmp_table_map;
    item->fix_after_pullout(new_parent, li.ref(), merge);
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


bool Item_cond::walk(Item_processor processor, bool walk_subquery, void *arg)
{
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
    if (item->walk(processor, walk_subquery, arg))
      return 1;
  return Item_func::walk(processor, walk_subquery, arg);
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
  while (li++)
  {
    /*
      The exact value of the last parameter to propagate_and_change_item_tree()
      is not important at this point. Item_func derivants will create and
      pass their own context to the arguments.
    */
    propagate_and_change_item_tree(thd, li.ref(), cond, Context_boolean());
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

void Item_cond::split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
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
  List_iterator_fast<Item> li(list);
  Item *item;
  if ((item=li++))
    item->print_parenthesised(str, query_type, precedence());
  while ((item=li++))
  {
    str->append(' ');
    str->append(func_name_cstring());
    str->append(' ');
    item->print_parenthesised(str, query_type, precedence());
  }
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


/**
  @brief
    Building clone for Item_cond
    
  @param thd        thread handle
  @param mem_root   part of the memory for the clone   

  @details
    This method gets copy of the current item and also 
    build clones for its elements. For this elements 
    build_copy is called again.
      
   @retval
     clone of the item
     0 if an error occurred
*/ 

Item *Item_cond::build_clone(THD *thd)
{
  List_iterator_fast<Item> li(list);
  Item *item;
  Item_cond *copy= (Item_cond *) get_copy(thd);
  if (!copy)
    return 0;
  copy->list.empty();
  while ((item= li++))
  {
    Item *arg_clone= item->build_clone(thd);
    if (!arg_clone)
      return 0;
    if (copy->list.push_back(arg_clone, thd->mem_root))
      return 0;
  }
  return copy;
}


bool Item_cond::excl_dep_on_table(table_map tab_map)
{
  if (used_tables() & (OUTER_REF_TABLE_BIT | RAND_TABLE_BIT))
    return false;
  if (!(used_tables() & ~tab_map))
    return true;
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
  {
    if (!item->excl_dep_on_table(tab_map))
      return false;
  }
  return true;
}


bool Item_cond::excl_dep_on_grouping_fields(st_select_lex *sel)
{
  if (has_rand_bit())
    return false;
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
  {
    if (!item->excl_dep_on_grouping_fields(sel))
      return false;
  }
  return true;
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
    There are AND expressions for which we don't care if the
    result is NULL or 0. This is the case for:
    - WHERE clause
    - HAVING clause
    - IF(expression)
    For these we mark them as "top_level_items"

  @retval
    1  If all expressions are true
  @retval
    0  If any of the expressions are false or if we find a NULL expression and
       this is a top_level_item.
  @retval
    NULL if all expression are either 1 or NULL
*/


longlong Item_cond_and::val_int()
{
  DBUG_ASSERT(fixed());
  List_iterator_fast<Item> li(list);
  Item *item;
  null_value= 0;
  while ((item=li++))
  {
    if (!item->val_bool())
    {
      if (is_top_level_item() || !(null_value= item->null_value))
        return 0;
    }
  }
  return null_value ? 0 : 1;
}


longlong Item_cond_or::val_int()
{
  DBUG_ASSERT(fixed());
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


bool Item_func_null_predicate::count_sargable_conds(void *arg)
{
  ((SELECT_LEX*) arg)->cond_count++;
  return 0;
}


longlong Item_func_isnull::val_int()
{
  DBUG_ASSERT(fixed());
  if (const_item() && !args[0]->maybe_null())
    return 0;
  return args[0]->is_null() ? 1: 0;
}


bool Item_func_isnull::find_not_null_fields(table_map allowed)
{
  if (!(~allowed & used_tables()) &&
      args[0]->real_item()->type() == Item::FIELD_ITEM)
  {
    Field *field= ((Item_field *)(args[0]->real_item()))->field;
    if (bitmap_is_set(&field->table->tmp_set, field->field_index))
      return true;
  }
  return false;
}


void Item_func_isnull::print(String *str, enum_query_type query_type)
{
  if (const_item() && !args[0]->maybe_null() &&
      !(query_type & (QT_NO_DATA_EXPANSION | QT_VIEW_INTERNAL)))
    str->append(STRING_WITH_LEN("/*always not null*/ 1"));
  else
    args[0]->print_parenthesised(str, query_type, precedence());
  str->append(STRING_WITH_LEN(" is null"));
}


longlong Item_is_not_null_test::val_int()
{
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_is_not_null_test::val_int");
  if (const_item() && !args[0]->maybe_null())
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
  if (!args[0]->maybe_null())
    used_tables_cache= 0;			/* is always true */
  else
    args[0]->update_used_tables();
}


longlong Item_func_isnotnull::val_int()
{
  DBUG_ASSERT(fixed());
  return args[0]->is_null() ? 0 : 1;
}


void Item_func_isnotnull::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, precedence());
  str->append(STRING_WITH_LEN(" is not null"));
}


bool Item_bool_func2::count_sargable_conds(void *arg)
{
  ((SELECT_LEX*) arg)->cond_count++;
  return 0;
}

void Item_func_like::print(String *str, enum_query_type query_type)
{
  args[0]->print_parenthesised(str, query_type, precedence());
  str->append(' ');
  if (negated)
    str->append(STRING_WITH_LEN(" not "));
  str->append(func_name_cstring());
  str->append(' ');
  if (escape_used_in_parsing)
  {
    args[1]->print_parenthesised(str, query_type, precedence());
    str->append(STRING_WITH_LEN(" escape "));
    escape_item->print_parenthesised(str, query_type, higher_precedence());
  }
  else
    args[1]->print_parenthesised(str, query_type, higher_precedence());
}


longlong Item_func_like::val_int()
{
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(escape != ESCAPE_NOT_INITIALIZED);
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
    return turboBM_matches(res->ptr(), res->length()) ? !negated : negated;
  return cmp_collation.collation->wildcmp(
		    res->ptr(),res->ptr()+res->length(),
		    res2->ptr(),res2->ptr()+res2->length(),
		    escape,wild_one,wild_many) ? negated : !negated;
}


/**
  We can optimize a where if first character isn't a wildcard
*/

bool Item_func_like::with_sargable_pattern() const
{
  if (negated)
    return false;

  if (!args[1]->can_eval_in_optimize())
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


/*
  subject LIKE pattern
  removes subject's dependency on PAD_CHAR_TO_FULL_LENGTH
  if pattern ends with the '%' wildcard.
*/
Sql_mode_dependency Item_func_like::value_depends_on_sql_mode() const
{
  if (!args[1]->value_depends_on_sql_mode_const_item())
    return Item_func::value_depends_on_sql_mode();
  StringBuffer<64> patternbuf;
  String *pattern= args[1]->val_str_ascii(&patternbuf);
  if (!pattern || !pattern->length())
    return Sql_mode_dependency();                  // Will return NULL or 0
  DBUG_ASSERT(pattern->charset()->mbminlen == 1);
  if (pattern->ptr()[pattern->length() - 1] != '%')
    return Item_func::value_depends_on_sql_mode();
  return ((args[0]->value_depends_on_sql_mode() |
           args[1]->value_depends_on_sql_mode()) &
          Sql_mode_dependency(~0, ~MODE_PAD_CHAR_TO_FULL_LENGTH)).
         soft_to_hard();
}


SEL_TREE *Item_func_like::get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr)
{
  MEM_ROOT *tmp_root= param->mem_root;
  param->thd->mem_root= param->old_root;
  bool sargable_pattern= with_sargable_pattern();
  param->thd->mem_root= tmp_root;
  return sargable_pattern ?
    Item_bool_func2::get_mm_tree(param, cond_ptr) :
    Item_func::get_mm_tree(param, cond_ptr);
}


bool fix_escape_item(THD *thd, Item *escape_item, String *tmp_str,
                     bool escape_used_in_parsing, CHARSET_INFO *cmp_cs,
                     int *escape)
{
  /*
    ESCAPE clause accepts only constant arguments and Item_param.

    Subqueries during context_analysis_only might decide they're
    const_during_execution, but not quite const yet, not evaluate-able.
    This is fine, as most of context_analysis_only modes will never
    reach val_int(), so we won't need the value.
    CONTEXT_ANALYSIS_ONLY_DERIVED being a notable exception here.
  */
  if (!escape_item->const_during_execution() ||
     (!escape_item->const_item() &&
      !(thd->lex->context_analysis_only & ~CONTEXT_ANALYSIS_ONLY_DERIVED)))
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"ESCAPE");
    return TRUE;
  }

  IF_DBUG(*escape= ESCAPE_NOT_INITIALIZED,);

  if (escape_item->const_item())
  {
    /* If we are on execution stage */
    /* XXX is it safe to evaluate is_expensive() items here? */
    String *escape_str= escape_item->val_str(tmp_str);
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

      if (cmp_cs->use_mb())
      {
        CHARSET_INFO *cs= escape_str->charset();
        my_wc_t wc;
        int rc= cs->mb_wc(&wc,
                          (const uchar*) escape_str_ptr,
                          (const uchar*) escape_str_ptr +
                          escape_str->length());
        *escape= (int) (rc > 0 ? wc : '\\');
      }
      else
      {
        /*
          In the case of 8bit character set, we pass native
          code instead of Unicode code as "escape" argument.
          Convert to "cs" if charset of escape differs.
        */
        uint32 unused;
        if (escape_str->needs_conversion(escape_str->length(),
                                         escape_str->charset(),cmp_cs,&unused))
        {
          char ch;
          uint errors;
          uint32 cnvlen= copy_and_convert(&ch, 1, cmp_cs, escape_str_ptr,
                                          escape_str->length(),
                                          escape_str->charset(), &errors);
          *escape= cnvlen ? ch : '\\';
        }
        else
          *escape= escape_str_ptr ? *escape_str_ptr : '\\';
      }
    }
    else
      *escape= '\\';
  }

  return FALSE;
}

bool Item_func_like::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  if (Item_bool_func2::fix_fields(thd, ref) ||
      escape_item->fix_fields_if_needed_for_scalar(thd, &escape_item) ||
      fix_escape_item(thd, escape_item, &cmp_value1, escape_used_in_parsing,
                      cmp_collation.collation, &escape))
    return TRUE;

  if (escape_item->const_item())
  {
    /*
      We could also do boyer-more for non-const items, but as we would have to
      recompute the tables for each row it's not worth it.
    */
    if (args[1]->can_eval_in_optimize() && !use_strnxfrm(collation.collation))
    {
      String* res2= args[1]->val_str(&cmp_value2);
      if (!res2)
        return FALSE;				// Null argument
      
      const size_t len= res2->length();

      /*
        len must be > 2 ('%pattern%')
        heuristic: only do TurboBM for pattern_len > 2
      */
      if (len <= 2)
        return FALSE;

      const char*  first= res2->ptr();
      const char*  last=  first + len - 1;
      
      if (len > MIN_TURBOBM_PATTERN_LEN + 2 &&
          *first == wild_many &&
          *last  == wild_many)
      {
        const char* tmp = first + 1;
        for (; *tmp != wild_many && *tmp != wild_one && *tmp != escape; tmp++) ;
        canDoTurboBM = (tmp == last) && !args[0]->collation.collation->use_mb();
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


bool Item_func_like::find_selective_predicates_list_processor(void *arg)
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

void Regexp_processor_pcre::cleanup()
{
  pcre2_match_data_free(m_pcre_match_data);
  pcre2_code_free(m_pcre);
  reset();
}

void Regexp_processor_pcre::init(CHARSET_INFO *data_charset, int extra_flags)
{
  m_library_flags= default_regex_flags() | extra_flags |
                  (data_charset != &my_charset_bin ?
                   (PCRE2_UTF | PCRE2_UCP) : 0) |
                  ((data_charset->state &
                    (MY_CS_BINSORT | MY_CS_CSSORT)) ? 0 : PCRE2_CASELESS);

  // Convert text data to utf-8.
  m_library_charset= data_charset == &my_charset_bin ?
                     &my_charset_bin : &my_charset_utf8mb3_general_ci;

  m_conversion_is_needed= (data_charset != &my_charset_bin) &&
                          !my_charset_same(data_charset, m_library_charset);
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
  int pcreErrorNumber;
  PCRE2_SIZE pcreErrorOffset;

  if (is_compiled())
  {
    if (!stringcmp(pattern, &m_prev_pattern))
      return false;
    cleanup();
    m_prev_pattern.copy(*pattern);
  }

  if (!(pattern= convert_if_needed(pattern, &pattern_converter)))
    return true;

  pcre2_compile_context *cctx= NULL;
#ifndef pcre2_set_depth_limit
  // old pcre2 uses stack - put a limit on that (new pcre2 prefers heap)
  cctx= pcre2_compile_context_create(NULL);
  pcre2_set_compile_recursion_guard(cctx, [](uint32_t cur, void *end) -> int
    { return available_stack_size(&cur, end) < STACK_MIN_SIZE; },
    current_thd->mysys_var->stack_ends_here);
#endif
  m_pcre= pcre2_compile((PCRE2_SPTR8) pattern->ptr(), pattern->length(),
                        m_library_flags,
                        &pcreErrorNumber, &pcreErrorOffset, cctx);
  pcre2_compile_context_free(cctx); // NULL is ok here

  if (unlikely(m_pcre == NULL))
  {
    if (send_error)
    {
      char buff[MAX_FIELD_WIDTH];
      int lmsg= pcre2_get_error_message(pcreErrorNumber,
                                        (PCRE2_UCHAR8 *)buff, sizeof(buff));
      if (lmsg >= 0)
        my_snprintf(buff+lmsg, sizeof(buff)-lmsg,
                    " at offset %d", pcreErrorOffset);
      my_error(ER_REGEXP_ERROR, MYF(0), buff);
    }
    return true;
  }
  m_pcre_match_data= pcre2_match_data_create_from_pattern(m_pcre, NULL);
  if (m_pcre_match_data == NULL)
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  return false;
}


bool Regexp_processor_pcre::compile(Item *item, bool send_error)
{
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff, sizeof(buff), &my_charset_bin);
  String *pattern= item->val_str(&tmp);
  if (unlikely(item->null_value) || (unlikely(compile(pattern, send_error))))
    return true;
  return false;
}


/**
  Send a warning explaining an error code returned by pcre_exec().
*/
void Regexp_processor_pcre::pcre_exec_warn(int rc) const
{
  PCRE2_UCHAR8 buf[128];
  THD *thd= current_thd;

  int errlen= pcre2_get_error_message(rc, buf, sizeof(buf));
  if (errlen <= 0)
  {
    my_snprintf((char *)buf, sizeof(buf), "pcre_exec: Internal error (%d)", rc);
  }
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_REGEXP_ERROR, ER_THD(thd, ER_REGEXP_ERROR), buf);
}


/**
  Call pcre_exec() and send a warning if pcre_exec() returned with an error.
*/
int Regexp_processor_pcre::pcre_exec_with_warn(const pcre2_code *code,
                                               pcre2_match_data *data,
                                               const char *subject,
                                               int length, int startoffset,
                                               int options)
{
  pcre2_match_context *mctx= NULL;
#ifndef pcre2_set_depth_limit
  // old pcre2 uses stack - put a limit on that (new pcre2 prefers heap)
  mctx= pcre2_match_context_create(NULL);
  pcre2_set_recursion_limit(mctx,
    available_stack_size(&mctx, current_thd->mysys_var->stack_ends_here)/544);
#endif
  int rc= pcre2_match(code, (PCRE2_SPTR8) subject, (PCRE2_SIZE) length,
                      (PCRE2_SIZE) startoffset, options, data, mctx);
  pcre2_match_context_free(mctx); // NULL is ok here
  DBUG_EXECUTE_IF("pcre_exec_error_123", rc= -123;);
  if (unlikely(rc < PCRE2_ERROR_NOMATCH))
  {
    m_SubStrVec= NULL;
    pcre_exec_warn(rc);
  }
  else
    m_SubStrVec= pcre2_get_ovector_pointer(data);
  return rc;
}


bool Regexp_processor_pcre::exec(const char *str, size_t length, size_t offset)
{
  m_pcre_exec_rc= pcre_exec_with_warn(m_pcre, m_pcre_match_data,
                                      str, (int)length, (int)offset, 0);
  return false;
}


bool Regexp_processor_pcre::exec(String *str, int offset,
                                  uint n_result_offsets_to_convert)
{
  if (!(str= convert_if_needed(str, &subject_converter)))
    return true;
  m_pcre_exec_rc= pcre_exec_with_warn(m_pcre, m_pcre_match_data,
                                      str->ptr(), str->length(), offset, 0);
  if (m_pcre_exec_rc > 0)
  {
    uint i;
    for (i= 0; i < n_result_offsets_to_convert; i++)
    {
      /*
        Convert byte offset into character offset.
      */
      m_SubStrVec[i]= (int) str->charset()->numchars(str->ptr(),
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
      owner->set_maybe_null(); // Will always return NULL
      return;
    }
    set_const(true);
    owner->base_flags|= subject_arg->base_flags & item_base_t::MAYBE_NULL;
  }
  else
    owner->set_maybe_null();
}


bool
Item_func_regex::fix_length_and_dec(THD *thd)
{
  if (Item_bool_func::fix_length_and_dec(thd) ||
      agg_arg_charsets_for_comparison(cmp_collation, args, 2))
    return TRUE;

  re.init(cmp_collation.collation, 0);
  re.fix_owner(this, args[0], args[1]);
  return FALSE;
}


longlong Item_func_regex::val_int()
{
  DBUG_ASSERT(fixed());
  if ((null_value= re.recompile(args[1])))
    return 0;

  if ((null_value= re.exec(args[0], 0, 0)))
    return 0;

  return re.match();
}


bool
Item_func_regexp_instr::fix_length_and_dec(THD *thd)
{
  if (agg_arg_charsets_for_comparison(cmp_collation, args, 2))
    return TRUE;

  re.init(cmp_collation.collation, 0);
  re.fix_owner(this, args[0], args[1]);
  max_length= MY_INT32_NUM_DECIMAL_DIGITS; // See also Item_func_locate
  return FALSE;
}


longlong Item_func_regexp_instr::val_int()
{
  DBUG_ASSERT(fixed());
  if ((null_value= re.recompile(args[1])))
    return 0;

  if ((null_value= re.exec(args[0], 0, 1)))
    return 0;

  return re.match() ? (longlong) (re.subpattern_start(0) + 1) : 0;
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
  int bcShift;
  int turboShift;
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
      int i= plm1;
      while (i >= 0 && pattern[i] == text[i + j])
      {
	i--;
	if (i == plm1 - shift)
	  i-= u;
      }
      if (i < 0)
	return 1;

      const int v= plm1 - i;
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
      int i= plm1;
      while (i >= 0 && likeconv(cs,pattern[i]) == likeconv(cs,text[i + j]))
      {
	i--;
	if (i == plm1 - shift)
	  i-= u;
      }
      if (i < 0)
	return 1;

      const int v= plm1 - i;
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
  DBUG_ASSERT(fixed());
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
    /* replace  "NOT <field>" with "<field> == 0" */
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


bool
Item_cond_and::set_format_by_check_constraint(
                                      Send_field_extended_metadata *to) const
{
  List_iterator_fast<Item> li(const_cast<List<Item>&>(list));
  Item *item;
  while ((item= li++))
  {
    if (item->set_format_by_check_constraint(to))
      return true;
  }
  return false;
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

Item_equal::Item_equal(THD *thd, const Type_handler *handler,
                       Item *f1, Item *f2, bool with_const_item):
  Item_bool_func(thd), eval_item(0), cond_false(0), cond_true(0),
  context_field(NULL), link_equal_fields(FALSE),
  m_compare_handler(handler),
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
  m_compare_handler(item_equal->m_compare_handler),
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

  cond_false= !Item_equal::compare_type_handler()->Item_eq_value(thd, this, c,
                                                                 get_const());
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
    if (item->can_eval_in_optimize() &&
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
  DBUG_ASSERT(fixed() == 0);
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
    DBUG_ASSERT(!item->with_sum_func() && !item->with_subquery());
    if (item->maybe_null())
      set_maybe_null();
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
  if (fix_length_and_dec(thd))
    return TRUE;
  base_flags|= item_base_t::FIXED;
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


/**
  @note
    This multiple equality can contains elements belonging not to tables {T}
    marked in 'allowed' . So we can ascertain null-rejectedness of field f
    belonging to table t from {T} only if one of the following equality
    predicate can be  extracted from this multiple equality:
    - f=const
    - f=f' where f' is a field of some table from {T}
*/

bool Item_equal::find_not_null_fields(table_map allowed)
{
  if (!(allowed & used_tables()))
    return false;
  bool checked= false;
  Item_equal_fields_iterator it(*this);
  Item *item;
  while ((item= it++))
  {
    if (~allowed & item->used_tables())
      continue;
    if ((with_const || checked) && !item->find_not_null_fields(allowed))
      continue;
    Item_equal_fields_iterator it1(*this);
    Item *item1;
    while ((item1= it1++) && item1 != item)
    {
      if (~allowed & item1->used_tables())
        continue;
      if (!item->find_not_null_fields(allowed) &&
          !item1->find_not_null_fields(allowed))
      {
        checked= true;
        break;
      }
    }
  }
  return false;
}



bool Item_equal::count_sargable_conds(void *arg)
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
      const int rc= eval_item->cmp(item);
      if ((rc == TRUE) || (null_value= (rc == UNKNOWN)))
        return 0;
    }
  }
  return 1;
}


bool Item_equal::fix_length_and_dec(THD *thd)
{
  Item *item= get_first(NO_PARTICULAR_TAB, NULL);
  const Type_handler *handler= item->type_handler();
  eval_item= handler->make_cmp_item(thd, item->collation.collation);
  return eval_item == NULL;
}


bool Item_equal::walk(Item_processor processor, bool walk_subquery, void *arg)
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
  str->append(func_name_cstring());
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
    in presence of SJM nests.
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
    if (my_charset_same(nm->charset(), DYNCOL_UTF))
    {
      buf.str= (char *) nm->ptr();
      buf.length= nm->length();
    }
    else
    {
      uint strlen= nm->length() * DYNCOL_UTF->mbmaxlen + 1;
      uint dummy_errors;
      buf.str= (char *) current_thd->alloc(strlen);
      if (buf.str)
      {
        buf.length=
          copy_and_convert(buf.str, strlen, DYNCOL_UTF,
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


bool
Item_equal::excl_dep_on_grouping_fields(st_select_lex *sel)
{
  Item_equal_fields_iterator it(*this);
  Item *item;

  while ((item=it++))
  {
    if (item->excl_dep_on_grouping_fields(sel))
    {
      set_extraction_flag(MARKER_FULL_EXTRACTION);
      return true;
    }
  }
  return false;
}


/**
  @brief
    Transform multiple equality into list of equalities

  @param thd         the thread handle
  @param equalities  the list where created equalities are stored
  @param checker     the checker callback function to be applied to the nodes
                     of the tree of the object to check if multiple equality
                     elements can be used to create equalities
  @param arg         parameter to be passed to the checker
  @param clone_const true <=> clone the constant member if there is any

  @details
    How the method works on examples:

    Example 1:
    It takes MULT_EQ(x,a,b) and tries to create from its elements a set of
    equalities {(x=a),(x=b)}.

    Example 2:
    It takes MULT_EQ(1,a,b) and tries to create from its elements a set of
    equalities {(a=1),(a=b)}.

    How it is done:

    1. If there is a constant member c the first non-constant member x for
       which the function checker returns true is taken and an item for
       the equality x=c is created. When constructing the equality item
       the left part of the equality is always taken as a clone of x while
       the right part is taken as a clone of c only if clone_const == true.

    2. After this all equalities of the form x=a (where x designates the first
       non-constant member for which checker returns true and a is some other
       such member of the multiplle equality) are created. When constructing
       an equality item both its parts are taken as clones of x and a.
    
       Suppose in the examples above that for 'x', 'a', and 'b' the function
       checker returns true.

       Example 1:
         the equality (x=a) is built
         the equality (x=b) is built

       Example 2:
         the equality (a=1) is built
         the equality (a=b) is built

    3. As a result we get a set of equalities built with the elements of
       this multiple equality. They are saved in the equality list.

       Example 1:
       {(x=a),(x=b)}

       Example 2:
       {(a=1),(a=b)}

  @note
    This method is called for condition pushdown into materialized
    derived table/view, and IN subquery, and pushdown from HAVING into WHERE.
    When it is called for pushdown from HAVING the empty checker is passed.
    This is because in this case the elements of the multiple equality don't
    need to be  checked if they can be used to build equalities: either all
    equalities can be pushed or none of them can be pushed.
    When the function is called for pushdown from HAVING the value of the
    parameter clone_const is always false. In other cases it's always true.

  @retval true   if an error occurs
  @retval false  otherwise
*/

bool Item_equal::create_pushable_equalities(THD *thd,
                                            List<Item> *equalities,
                                            Pushdown_checker checker,
                                            uchar *arg,
                                            bool clone_const)
{
  Item *item;
  Item *left_item= NULL;
  Item *right_item = get_const();
  Item_equal_fields_iterator it(*this);

  while ((item=it++))
  {
    left_item= item;
    if (checker && !((item->*checker) (arg)))
      continue;
    break;
  }

  if (!left_item)
    return false;

  if (right_item)
  {
    Item_func_eq *eq= 0;
    Item *left_item_clone= left_item->build_clone(thd);
    Item *right_item_clone= !clone_const ?
                            right_item : right_item->build_clone(thd);
    if (!left_item_clone || !right_item_clone)
      return true;
    eq= new (thd->mem_root) Item_func_eq(thd,
                                         left_item_clone,
                                         right_item_clone);
    if (!eq ||  equalities->push_back(eq, thd->mem_root))
      return true;
    if (!clone_const)
    {
      /*
        Also set IMMUTABLE_FL for any sub-items of the right_item.
        This is needed to prevent Item::cleanup_excluding_immutables_processor
        from peforming cleanup of the sub-items and so creating an item tree
        where a fixed item has non-fixed items inside it.
      */
      int16 new_flag= MARKER_IMMUTABLE;
      right_item->walk(&Item::set_extraction_flag_processor, false,
                       (void*)&new_flag);
    }
  }

  while ((item=it++))
  {
    if (checker && !((item->*checker) (arg)))
      continue;
    Item_func_eq *eq= 0;
    Item *left_item_clone= left_item->build_clone(thd);
    Item *right_item_clone= item->build_clone(thd);
    if (!(left_item_clone && right_item_clone))
      return true;
    left_item_clone->set_item_equal(NULL);
    right_item_clone->set_item_equal(NULL);
    eq= new (thd->mem_root) Item_func_eq(thd,
                                         right_item_clone,
                                         left_item_clone);
    if (!eq || equalities->push_back(eq, thd->mem_root))
      return true;
  }
  return false;
}


/**
  Transform multiple equality into the AND condition of equalities.

  Example:
  MULT_EQ(x,a,b)
  =>
  (x=a) AND (x=b)

  Equalities are built in Item_equal::create_pushable_equalities() method
  using elements of this multiple equality. The result of this method is
  saved in an equality list.
  This method returns the condition where the elements of the equality list
  are anded.
*/

Item *Item_equal::multiple_equality_transformer(THD *thd, uchar *arg)
{
  List<Item> equalities;
  if (create_pushable_equalities(thd, &equalities, 0, 0, false))
    return 0;

  switch (equalities.elements)
  {
  case 0:
    return 0;
  case 1:
    return equalities.head();
    break;
  default:
    return new (thd->mem_root) Item_cond_and(thd, equalities);
    break;
  }
}
