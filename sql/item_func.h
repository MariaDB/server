#ifndef ITEM_FUNC_INCLUDED
#define ITEM_FUNC_INCLUDED
/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2016, MariaDB

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


/* Function items used by mysql */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#ifdef HAVE_IEEEFP_H
extern "C"				/* Bug in BSDI include file */
{
#include <ieeefp.h>
}
#endif


class Item_func :public Item_func_or_sum
{
  void sync_with_sum_func_and_with_field(List<Item> &list);
protected:
  /*
    Allowed numbers of columns in result (usually 1, which means scalar value)
    0 means get this number from first argument
  */
  uint allowed_arg_cols;
  String *val_str_from_val_str_ascii(String *str, String *str2);

  void count_only_length(Item **item, uint nitems);
  void count_real_length(Item **item, uint nitems);
  void count_decimal_length(Item **item, uint nitems);
  void count_datetime_length(enum_field_types field_type,
                             Item **item, uint nitems);
  bool count_string_result_length(enum_field_types field_type,
                                  Item **item, uint nitems);
public:
  table_map not_null_tables_cache;

  enum Functype { UNKNOWN_FUNC,EQ_FUNC,EQUAL_FUNC,NE_FUNC,LT_FUNC,LE_FUNC,
		  GE_FUNC,GT_FUNC,FT_FUNC,
		  LIKE_FUNC,ISNULL_FUNC,ISNOTNULL_FUNC,
		  COND_AND_FUNC, COND_OR_FUNC, XOR_FUNC,
                  BETWEEN, IN_FUNC, MULT_EQUAL_FUNC,
		  INTERVAL_FUNC, ISNOTNULLTEST_FUNC,
		  SP_EQUALS_FUNC, SP_DISJOINT_FUNC,SP_INTERSECTS_FUNC,
		  SP_TOUCHES_FUNC,SP_CROSSES_FUNC,SP_WITHIN_FUNC,
		  SP_CONTAINS_FUNC,SP_OVERLAPS_FUNC,
		  SP_STARTPOINT,SP_ENDPOINT,SP_EXTERIORRING,
		  SP_POINTN,SP_GEOMETRYN,SP_INTERIORRINGN, SP_RELATE_FUNC,
                  NOT_FUNC, NOT_ALL_FUNC,
                  NOW_FUNC, NOW_UTC_FUNC, SYSDATE_FUNC, TRIG_COND_FUNC,
                  SUSERVAR_FUNC, GUSERVAR_FUNC, COLLATE_FUNC,
                  EXTRACT_FUNC, CHAR_TYPECAST_FUNC, FUNC_SP, UDF_FUNC,
                  NEG_FUNC, GSYSVAR_FUNC, IN_OPTIMIZER_FUNC, DYNCOL_FUNC,
                  JSON_EXTRACT_FUNC };
  enum Type type() const { return FUNC_ITEM; }
  virtual enum Functype functype() const   { return UNKNOWN_FUNC; }
  Item_func(THD *thd): Item_func_or_sum(thd), allowed_arg_cols(1)
  {
    with_sum_func= 0;
    with_field= 0;
    with_param= 0;
  }
  Item_func(THD *thd, Item *a): Item_func_or_sum(thd, a), allowed_arg_cols(1)
  {
    with_sum_func= a->with_sum_func;
    with_param= a->with_param;
    with_field= a->with_field;
  }
  Item_func(THD *thd, Item *a, Item *b):
    Item_func_or_sum(thd, a, b), allowed_arg_cols(1)
  {
    with_sum_func= a->with_sum_func || b->with_sum_func;
    with_param= a->with_param || b->with_param;
    with_field= a->with_field || b->with_field;
  }
  Item_func(THD *thd, Item *a, Item *b, Item *c):
    Item_func_or_sum(thd, a, b, c), allowed_arg_cols(1)
  {
    with_sum_func= a->with_sum_func || b->with_sum_func || c->with_sum_func;
    with_field= a->with_field || b->with_field || c->with_field;
    with_param= a->with_param || b->with_param || c->with_param;
  }
  Item_func(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_func_or_sum(thd, a, b, c, d), allowed_arg_cols(1)
  {
    with_sum_func= a->with_sum_func || b->with_sum_func ||
                   c->with_sum_func || d->with_sum_func;
    with_field= a->with_field || b->with_field ||
                c->with_field || d->with_field;
    with_param= a->with_param || b->with_param ||
                c->with_param || d->with_param;
  }
  Item_func(THD *thd, Item *a, Item *b, Item *c, Item *d, Item* e):
    Item_func_or_sum(thd, a, b, c, d, e), allowed_arg_cols(1)
  {
    with_sum_func= a->with_sum_func || b->with_sum_func ||
                   c->with_sum_func || d->with_sum_func || e->with_sum_func;
    with_field= a->with_field || b->with_field ||
                c->with_field || d->with_field || e->with_field;
    with_param= a->with_param || b->with_param ||
                c->with_param || d->with_param || e->with_param;
  }
  Item_func(THD *thd, List<Item> &list):
    Item_func_or_sum(thd, list), allowed_arg_cols(1)
  {
    set_arguments(thd, list);
  }
  // Constructor used for Item_cond_and/or (see Item comment)
  Item_func(THD *thd, Item_func *item):
    Item_func_or_sum(thd, item),
    allowed_arg_cols(item->allowed_arg_cols),
    not_null_tables_cache(item->not_null_tables_cache)
  {
  }
  bool fix_fields(THD *, Item **ref);
  void cleanup()
  {
    Item_func_or_sum::cleanup();
    used_tables_and_const_cache_init();
  }
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge);
  void quick_fix_field();
  table_map not_null_tables() const;
  void update_used_tables()
  {
    used_tables_and_const_cache_init();
    used_tables_and_const_cache_update_and_join(arg_count, args);
  }
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref);
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr)
  {
    DBUG_ENTER("Item_func::get_mm_tree");
    DBUG_RETURN(const_item() ? get_mm_tree_for_const(param) : NULL);
  }
  bool eq(const Item *item, bool binary_cmp) const;
  virtual Item *key_item() const { return args[0]; }
  void set_arguments(THD *thd, List<Item> &list)
  {
    allowed_arg_cols= 1;
    Item_args::set_arguments(thd, list);
    sync_with_sum_func_and_with_field(list);
    list.empty();                                     // Fields are used
  }
  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                      List<Item> &fields, uint flags);
  virtual void print(String *str, enum_query_type query_type);
  void print_op(String *str, enum_query_type query_type);
  void print_args(String *str, uint from, enum_query_type query_type);
  inline bool get_arg0_date(MYSQL_TIME *ltime, ulonglong fuzzy_date)
  {
    return (null_value=args[0]->get_date_with_conversion(ltime, fuzzy_date));
  }
  inline bool get_arg0_time(MYSQL_TIME *ltime)
  {
    null_value= args[0]->get_time(ltime);
    DBUG_ASSERT(null_value ||
                ltime->time_type != MYSQL_TIMESTAMP_TIME || ltime->day == 0);
    return null_value;
  }
  bool is_null() { 
    update_null_value();
    return null_value; 
  }
  void signal_divide_by_null();
  friend class udf_handler;
  Field *create_field_for_create_select(TABLE *table)
  {
    return result_type() != STRING_RESULT ?
           create_tmp_field(false, table, MY_INT32_NUM_DECIMAL_DIGITS) :
           tmp_table_field_from_field_type(table, false, false);
  }
  Item *get_tmp_table_item(THD *thd);

  my_decimal *val_decimal(my_decimal *);

  void fix_char_length_ulonglong(ulonglong max_char_length_arg)
  {
    ulonglong max_result_length= max_char_length_arg *
                                 collation.collation->mbmaxlen;
    if (max_result_length >= MAX_BLOB_WIDTH)
    {
      max_length= MAX_BLOB_WIDTH;
      maybe_null= 1;
    }
    else
      max_length= (uint32) max_result_length;
  }
  Item *transform(THD *thd, Item_transformer transformer, uchar *arg);
  Item* compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                Item_transformer transformer, uchar *arg_t);
  void traverse_cond(Cond_traverser traverser,
                     void * arg, traverse_order order);
  bool eval_not_null_tables(void *opt_arg);
 // bool is_expensive_processor(void *arg);
 // virtual bool is_expensive() { return 0; }
  inline void raise_numeric_overflow(const char *type_name)
  {
    char buf[256];
    String str(buf, sizeof(buf), system_charset_info);
    str.length(0);
    print(&str, QT_NO_DATA_EXPANSION);
    my_error(ER_DATA_OUT_OF_RANGE, MYF(0), type_name, str.c_ptr_safe());
  }
  inline double raise_float_overflow()
  {
    raise_numeric_overflow("DOUBLE");
    return 0.0;
  }
  inline longlong raise_integer_overflow()
  {
    raise_numeric_overflow(unsigned_flag ? "BIGINT UNSIGNED": "BIGINT");
    return 0;
  }
  inline int raise_decimal_overflow()
  {
    raise_numeric_overflow("DECIMAL");
    return E_DEC_OVERFLOW;
  }
  /**
     Throw an error if the input double number is not finite, i.e. is either
     +/-INF or NAN.
  */
  inline double check_float_overflow(double value)
  {
    return std::isfinite(value) ? value : raise_float_overflow();
  }
  /**
    Throw an error if the input BIGINT value represented by the
    (longlong value, bool unsigned flag) pair cannot be returned by the
    function, i.e. is not compatible with this Item's unsigned_flag.
  */
  inline longlong check_integer_overflow(longlong value, bool val_unsigned)
  {
    if ((unsigned_flag && !val_unsigned && value < 0) ||
        (!unsigned_flag && val_unsigned &&
         (ulonglong) value > (ulonglong) LONGLONG_MAX))
      return raise_integer_overflow();
    return value;
  }
  /**
     Throw an error if the error code of a DECIMAL operation is E_DEC_OVERFLOW.
  */
  inline int check_decimal_overflow(int error)
  {
    return (error == E_DEC_OVERFLOW) ? raise_decimal_overflow() : error;
  }

  bool has_timestamp_args()
  {
    DBUG_ASSERT(fixed == TRUE);
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->type() == Item::FIELD_ITEM &&
          args[i]->field_type() == MYSQL_TYPE_TIMESTAMP)
        return TRUE;
    }
    return FALSE;
  }

  bool has_date_args()
  {
    DBUG_ASSERT(fixed == TRUE);
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->type() == Item::FIELD_ITEM &&
          (args[i]->field_type() == MYSQL_TYPE_DATE ||
           args[i]->field_type() == MYSQL_TYPE_DATETIME))
        return TRUE;
    }
    return FALSE;
  }

  bool has_time_args()
  {
    DBUG_ASSERT(fixed == TRUE);
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->type() == Item::FIELD_ITEM &&
          (args[i]->field_type() == MYSQL_TYPE_TIME ||
           args[i]->field_type() == MYSQL_TYPE_DATETIME))
        return TRUE;
    }
    return FALSE;
  }

  bool has_datetime_args()
  {
    DBUG_ASSERT(fixed == TRUE);
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->type() == Item::FIELD_ITEM &&
          args[i]->field_type() == MYSQL_TYPE_DATETIME)
        return TRUE;
    }
    return FALSE;
  }

  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
  {
    /*
      By default only substitution for a field whose two different values
      are never equal is allowed in the arguments of a function.
      This is overruled for the direct arguments of comparison functions.
    */
    Item_args::propagate_equal_fields(thd, Context_identity(), cond);
    return this;
  }

  bool excl_dep_on_table(table_map tab_map)
  {
    if (used_tables() & OUTER_REF_TABLE_BIT)
      return false; 
    return !(used_tables() & ~tab_map) || 
           Item_args::excl_dep_on_table(tab_map);
  }

  bool excl_dep_on_grouping_fields(st_select_lex *sel)
  {
    return Item_args::excl_dep_on_grouping_fields(sel);
  }

  /*
    We assume the result of any function that has a TIMESTAMP argument to be
    timezone-dependent, since a TIMESTAMP value in both numeric and string
    contexts is interpreted according to the current timezone.
    The only exception is UNIX_TIMESTAMP() which returns the internal
    representation of a TIMESTAMP argument verbatim, and thus does not depend on
    the timezone.
   */
  virtual bool check_valid_arguments_processor(void *bool_arg)
  {
    return has_timestamp_args();
  }

  virtual bool find_function_processor (void *arg)
  {
    return functype() == *(Functype *) arg;
  }

  void no_rows_in_result()
  {
    for (uint i= 0; i < arg_count; i++)
    {
      args[i]->no_rows_in_result();
    }
  }
  void restore_to_before_no_rows_in_result()
  {
    for (uint i= 0; i < arg_count; i++)
    {
      args[i]->no_rows_in_result();
    }
  }
  void convert_const_compared_to_int_field(THD *thd);
  /**
    Prepare arguments and setup a comparator.
    Used in Item_func_xxx with two arguments and a comparator,
    e.g. Item_bool_func2 and Item_func_nullif.
    args[0] or args[1] can be modified:
    - converted to character set and collation of the operation
    - or replaced to an Item_int_with_ref
  */
  bool setup_args_and_comparator(THD *thd, Arg_comparator *cmp);
};


class Item_real_func :public Item_func
{
public:
  Item_real_func(THD *thd): Item_func(thd) { collation.set_numeric(); }
  Item_real_func(THD *thd, Item *a): Item_func(thd, a)
  { collation.set_numeric(); }
  Item_real_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b)
  { collation.set_numeric(); }
  Item_real_func(THD *thd, List<Item> &list): Item_func(thd, list)
  { collation.set_numeric(); }
  String *val_str(String*str);
  my_decimal *val_decimal(my_decimal *decimal_value);
  longlong val_int()
  {
    DBUG_ASSERT(fixed == 1);
    return Converter_double_to_longlong(val_real(), unsigned_flag).result();
  }
  enum Item_result result_type () const { return REAL_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  bool fix_length_and_dec()
  { decimals= NOT_FIXED_DEC; max_length= float_length(decimals); return FALSE; }
};


/**
  Functions whose returned field type is determined at fix_fields() time.
*/
class Item_hybrid_func: public Item_func,
                        public Type_handler_hybrid_field_type
{
protected:
  void fix_attributes(Item **item, uint nitems);
public:
  Item_hybrid_func(THD *thd): Item_func(thd) { }
  Item_hybrid_func(THD *thd, Item *a):  Item_func(thd, a) { }
  Item_hybrid_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b) { }
  Item_hybrid_func(THD *thd, Item *a, Item *b, Item *c):
    Item_func(thd, a, b, c) { }
  Item_hybrid_func(THD *thd, List<Item> &list): Item_func(thd, list) { }
  Item_hybrid_func(THD *thd, Item_hybrid_func *item)
    :Item_func(thd, item), Type_handler_hybrid_field_type(item) { }
  enum_field_types field_type() const
  { return Type_handler_hybrid_field_type::field_type(); }
  enum Item_result result_type () const
  { return Type_handler_hybrid_field_type::result_type(); }
  enum Item_result cmp_type () const
  { return Type_handler_hybrid_field_type::cmp_type(); }
};


/**
  Functions that at fix_fields() time determine the returned field type,
  trying to preserve the exact data type of the arguments.

  The descendants have to implement "native" value methods,
  i.e. str_op(), date_op(), int_op(), real_op(), decimal_op().
  fix_fields() chooses which of the above value methods will be
  used during execution time, according to the returned field type.

  For example, if fix_fields() determines that the returned value type
  is MYSQL_TYPE_LONG, then:
  - int_op() is chosen as the execution time native method.
  - val_int() returns the result of int_op() as is.
  - all other methods, i.e. val_real(), val_decimal(), val_str(), get_date(),
    call int_op() first, then convert the result to the requested data type.
*/
class Item_func_hybrid_field_type: public Item_hybrid_func
{
  /*
    Helper methods to make sure that the result of
    decimal_op(), str_op() and date_op() is properly synched with null_value.
  */
  bool date_op_with_null_check(MYSQL_TIME *ltime)
  {
     bool rc= date_op(ltime,
                      field_type() == MYSQL_TYPE_TIME ? TIME_TIME_ONLY : 0);
     DBUG_ASSERT(!rc ^ null_value);
     return rc;
  }
  String *str_op_with_null_check(String *str)
  {
    String *res= str_op(str);
    DBUG_ASSERT((res != NULL) ^ null_value);
    return res;
  }
  my_decimal *decimal_op_with_null_check(my_decimal *decimal_buffer)
  {
    my_decimal *res= decimal_op(decimal_buffer);
    DBUG_ASSERT((res != NULL) ^ null_value);
    return res;
  }

public:
  Item_func_hybrid_field_type(THD *thd):
    Item_hybrid_func(thd)
  { collation.set_numeric(); }
  Item_func_hybrid_field_type(THD *thd, Item *a):
    Item_hybrid_func(thd, a)
  { collation.set_numeric(); }
  Item_func_hybrid_field_type(THD *thd, Item *a, Item *b):
    Item_hybrid_func(thd, a, b)
  { collation.set_numeric(); }
  Item_func_hybrid_field_type(THD *thd, Item *a, Item *b, Item *c):
    Item_hybrid_func(thd, a, b, c)
  { collation.set_numeric(); }
  Item_func_hybrid_field_type(THD *thd, List<Item> &list):
    Item_hybrid_func(thd, list)
  { collation.set_numeric(); }

  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String*str);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);

  /**
     @brief Performs the operation that this functions implements when the
     result type is INT.

     @return The result of the operation.
  */
  virtual longlong int_op()= 0;

  /**
     @brief Performs the operation that this functions implements when the
     result type is REAL.

     @return The result of the operation.
  */
  virtual double real_op()= 0;

  /**
     @brief Performs the operation that this functions implements when the
     result type is DECIMAL.

     @param A pointer where the DECIMAL value will be allocated.
     @return 
       - 0 If the result is NULL
       - The same pointer it was given, with the area initialized to the
         result of the operation.
  */
  virtual my_decimal *decimal_op(my_decimal *)= 0;

  /**
     @brief Performs the operation that this functions implements when the
     result type is a string type.

     @return The result of the operation.
  */
  virtual String *str_op(String *)= 0;

  /**
     @brief Performs the operation that this functions implements when
     field type is a temporal type.
     @return The result of the operation.
  */
  virtual bool date_op(MYSQL_TIME *res, uint fuzzy_date)= 0;

};


class Item_func_numhybrid: public Item_func_hybrid_field_type
{
protected:

  inline void fix_decimals()
  {
    DBUG_ASSERT(result_type() == DECIMAL_RESULT);
    if (decimals == NOT_FIXED_DEC)
      set_if_smaller(decimals, max_length - 1);
  }

public:
  Item_func_numhybrid(THD *thd): Item_func_hybrid_field_type(thd)
  { }
  Item_func_numhybrid(THD *thd, Item *a): Item_func_hybrid_field_type(thd, a)
  { }
  Item_func_numhybrid(THD *thd, Item *a, Item *b):
    Item_func_hybrid_field_type(thd, a, b)
  { }
  Item_func_numhybrid(THD *thd, Item *a, Item *b, Item *c):
    Item_func_hybrid_field_type(thd, a, b, c)
  { }
  Item_func_numhybrid(THD *thd, List<Item> &list):
    Item_func_hybrid_field_type(thd, list)
  { }
  String *str_op(String *str) { DBUG_ASSERT(0); return 0; }
  bool date_op(MYSQL_TIME *ltime, uint fuzzydate) { DBUG_ASSERT(0); return true; }
};


/* function where type of result detected by first argument */
class Item_func_num1: public Item_func_numhybrid
{
public:
  Item_func_num1(THD *thd, Item *a): Item_func_numhybrid(thd, a) {}
  Item_func_num1(THD *thd, Item *a, Item *b): Item_func_numhybrid(thd, a, b) {}
  bool fix_length_and_dec();
};


/* Base class for operations like '+', '-', '*' */
class Item_num_op :public Item_func_numhybrid
{
 public:
  Item_num_op(THD *thd, Item *a, Item *b): Item_func_numhybrid(thd, a, b) {}
  virtual void result_precision()= 0;

  virtual inline void print(String *str, enum_query_type query_type)
  {
    print_op(str, query_type);
  }
  bool fix_length_and_dec();
  bool need_parentheses_in_default() { return true; }
};


class Item_int_func :public Item_func
{
public:
  Item_int_func(THD *thd): Item_func(thd)
  { collation.set_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a): Item_func(thd, a)
  { collation.set_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b)
  { collation.set_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a, Item *b, Item *c): Item_func(thd, a, b, c)
  { collation.set_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_func(thd, a, b, c, d)
  { collation.set_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, List<Item> &list): Item_func(thd, list)
  { collation.set_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item_int_func *item) :Item_func(thd, item)
  { collation.set_numeric(); }
  double val_real();
  String *val_str(String*str);
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  bool fix_length_and_dec() { return FALSE; }
};


class Item_func_connection_id :public Item_int_func
{
  longlong value;

public:
  Item_func_connection_id(THD *thd): Item_int_func(thd) {}
  const char *func_name() const { return "connection_id"; }
  bool fix_length_and_dec();
  bool fix_fields(THD *thd, Item **ref);
  longlong val_int() { DBUG_ASSERT(fixed == 1); return value; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_connection_id>(thd, mem_root, this); }
};


class Item_func_signed :public Item_int_func
{
public:
  Item_func_signed(THD *thd, Item *a): Item_int_func(thd, a)
  {
    unsigned_flag= 0;
  }
  const char *func_name() const { return "cast_as_signed"; }
  longlong val_int()
  {
    longlong value= args[0]->val_int_signed_typecast();
    null_value= args[0]->null_value;
    return value;
  }
  bool fix_length_and_dec()
  {
    uint32 char_length= MY_MIN(args[0]->max_char_length(),
                               MY_INT64_NUM_DECIMAL_DIGITS);
    /*
      args[0]->max_char_length() can return 0.
      Reserve max_length to fit at least one character for one digit,
      plus one character for the sign (if signed).
    */
    set_if_bigger(char_length, 1U + (unsigned_flag ? 0 : 1));
    fix_char_length(char_length);
    return FALSE;
  }
  virtual void print(String *str, enum_query_type query_type);
  uint decimal_precision() const { return args[0]->decimal_precision(); }
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_signed>(thd, mem_root, this); }
};


class Item_func_unsigned :public Item_func_signed
{
public:
  Item_func_unsigned(THD *thd, Item *a): Item_func_signed(thd, a)
  {
    unsigned_flag= 1;
  }
  const char *func_name() const { return "cast_as_unsigned"; }
  longlong val_int()
  {
    longlong value= args[0]->val_int_unsigned_typecast();
    null_value= args[0]->null_value;
    return value;
  }
  virtual void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_unsigned>(thd, mem_root, this); }
};


class Item_decimal_typecast :public Item_func
{
  my_decimal decimal_value;
public:
  Item_decimal_typecast(THD *thd, Item *a, int len, int dec): Item_func(thd, a)
  {
    decimals= (uint8) dec;
    collation.set_numeric();
    fix_char_length(my_decimal_precision_to_length_no_truncation(len, dec,
                                                                 unsigned_flag));
  }
  String *val_str(String *str);
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal*);
  enum Item_result result_type () const { return DECIMAL_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
  bool fix_length_and_dec() { return FALSE; }
  const char *func_name() const { return "decimal_typecast"; }
  virtual void print(String *str, enum_query_type query_type);
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_decimal_typecast>(thd, mem_root, this); }
};


class Item_double_typecast :public Item_real_func
{
public:
  Item_double_typecast(THD *thd, Item *a, int len, int dec):
    Item_real_func(thd, a)
  {
    decimals=   (uint8)  dec;
    max_length= (uint32) len;
  }
  double val_real();
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  bool fix_length_and_dec() { maybe_null= 1; return FALSE; }
  const char *func_name() const { return "double_typecast"; }
  virtual void print(String *str, enum_query_type query_type);
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_double_typecast>(thd, mem_root, this); }
};



class Item_func_additive_op :public Item_num_op
{
public:
  Item_func_additive_op(THD *thd, Item *a, Item *b): Item_num_op(thd, a, b) {}
  void result_precision();
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
};


class Item_func_plus :public Item_func_additive_op
{
public:
  Item_func_plus(THD *thd, Item *a, Item *b):
    Item_func_additive_op(thd, a, b) {}
  const char *func_name() const { return "+"; }
  enum precedence precedence() const { return ADD_PRECEDENCE; }
  longlong int_op();
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_plus>(thd, mem_root, this); }
};

class Item_func_minus :public Item_func_additive_op
{
  bool m_depends_on_sql_mode_no_unsigned_subtraction;
public:
  Item_func_minus(THD *thd, Item *a, Item *b):
    Item_func_additive_op(thd, a, b),
    m_depends_on_sql_mode_no_unsigned_subtraction(false)
  { }
  const char *func_name() const { return "-"; }
  enum precedence precedence() const { return ADD_PRECEDENCE; }
  Sql_mode_dependency value_depends_on_sql_mode() const;
  longlong int_op();
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_minus>(thd, mem_root, this); }
};


class Item_func_mul :public Item_num_op
{
public:
  Item_func_mul(THD *thd, Item *a, Item *b):
    Item_num_op(thd, a, b) {}
  const char *func_name() const { return "*"; }
  enum precedence precedence() const { return MUL_PRECEDENCE; }
  longlong int_op();
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  void result_precision();
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_mul>(thd, mem_root, this); }
};


class Item_func_div :public Item_num_op
{
public:
  uint prec_increment;
  Item_func_div(THD *thd, Item *a, Item *b): Item_num_op(thd, a, b) {}
  longlong int_op() { DBUG_ASSERT(0); return 0; }
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  const char *func_name() const { return "/"; }
  enum precedence precedence() const { return MUL_PRECEDENCE; }
  bool fix_length_and_dec();
  void result_precision();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_div>(thd, mem_root, this); }
};


class Item_func_int_div :public Item_int_func
{
public:
  Item_func_int_div(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b)
  {}
  longlong val_int();
  const char *func_name() const { return "DIV"; }
  enum precedence precedence() const { return MUL_PRECEDENCE; }
  bool fix_length_and_dec();
  void print(String *str, enum_query_type query_type)
  {
    print_op(str, query_type);
  }

  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_int_div>(thd, mem_root, this); }
};


class Item_func_mod :public Item_num_op
{
public:
  Item_func_mod(THD *thd, Item *a, Item *b): Item_num_op(thd, a, b) {}
  longlong int_op();
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  const char *func_name() const { return "%"; }
  enum precedence precedence() const { return MUL_PRECEDENCE; }
  void result_precision();
  bool fix_length_and_dec();
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_mod>(thd, mem_root, this); }
};


class Item_func_neg :public Item_func_num1
{
public:
  Item_func_neg(THD *thd, Item *a): Item_func_num1(thd, a) {}
  double real_op();
  longlong int_op();
  my_decimal *decimal_op(my_decimal *);
  const char *func_name() const { return "-"; }
  enum Functype functype() const   { return NEG_FUNC; }
  enum precedence precedence() const { return NEG_PRECEDENCE; }
  void print(String *str, enum_query_type query_type)
  {
    str->append(func_name());
    args[0]->print_parenthesised(str, query_type, precedence());
  }
  bool fix_length_and_dec();
  uint decimal_precision() const { return args[0]->decimal_precision(); }
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  bool need_parentheses_in_default() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_neg>(thd, mem_root, this); }
};


class Item_func_abs :public Item_func_num1
{
public:
  Item_func_abs(THD *thd, Item *a): Item_func_num1(thd, a) {}
  double real_op();
  longlong int_op();
  my_decimal *decimal_op(my_decimal *);
  const char *func_name() const { return "abs"; }
  bool fix_length_and_dec();
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_abs>(thd, mem_root, this); }
};

// A class to handle logarithmic and trigonometric functions

class Item_dec_func :public Item_real_func
{
 public:
  Item_dec_func(THD *thd, Item *a): Item_real_func(thd, a) {}
  Item_dec_func(THD *thd, Item *a, Item *b): Item_real_func(thd, a, b) {}
  bool fix_length_and_dec()
  {
    decimals=NOT_FIXED_DEC; max_length=float_length(decimals);
    maybe_null=1;
    return FALSE;
  }
};

class Item_func_exp :public Item_dec_func
{
public:
  Item_func_exp(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "exp"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_exp>(thd, mem_root, this); }
};


class Item_func_ln :public Item_dec_func
{
public:
  Item_func_ln(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "ln"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_ln>(thd, mem_root, this); }
};


class Item_func_log :public Item_dec_func
{
public:
  Item_func_log(THD *thd, Item *a): Item_dec_func(thd, a) {}
  Item_func_log(THD *thd, Item *a, Item *b): Item_dec_func(thd, a, b) {}
  double val_real();
  const char *func_name() const { return "log"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_log>(thd, mem_root, this); }
};


class Item_func_log2 :public Item_dec_func
{
public:
  Item_func_log2(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "log2"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_log2>(thd, mem_root, this); }
};


class Item_func_log10 :public Item_dec_func
{
public:
  Item_func_log10(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "log10"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_log10>(thd, mem_root, this); }
};


class Item_func_sqrt :public Item_dec_func
{
public:
  Item_func_sqrt(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "sqrt"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_sqrt>(thd, mem_root, this); }
};


class Item_func_pow :public Item_dec_func
{
public:
  Item_func_pow(THD *thd, Item *a, Item *b): Item_dec_func(thd, a, b) {}
  double val_real();
  const char *func_name() const { return "pow"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_pow>(thd, mem_root, this); }
};


class Item_func_acos :public Item_dec_func
{
public:
  Item_func_acos(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "acos"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_acos>(thd, mem_root, this); }
};

class Item_func_asin :public Item_dec_func
{
public:
  Item_func_asin(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "asin"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_asin>(thd, mem_root, this); }
};

class Item_func_atan :public Item_dec_func
{
public:
  Item_func_atan(THD *thd, Item *a): Item_dec_func(thd, a) {}
  Item_func_atan(THD *thd, Item *a, Item *b): Item_dec_func(thd, a, b) {}
  double val_real();
  const char *func_name() const { return "atan"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_atan>(thd, mem_root, this); }
};

class Item_func_cos :public Item_dec_func
{
public:
  Item_func_cos(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "cos"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_cos>(thd, mem_root, this); }
};

class Item_func_sin :public Item_dec_func
{
public:
  Item_func_sin(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "sin"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_sin>(thd, mem_root, this); }
};

class Item_func_tan :public Item_dec_func
{
public:
  Item_func_tan(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "tan"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_tan>(thd, mem_root, this); }
};

class Item_func_cot :public Item_dec_func
{
public:
  Item_func_cot(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real();
  const char *func_name() const { return "cot"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_cot>(thd, mem_root, this); }
};


class Item_func_int_val :public Item_func_num1
{
public:
  Item_func_int_val(THD *thd, Item *a): Item_func_num1(thd, a) {}
  bool fix_length_and_dec();
};


class Item_func_ceiling :public Item_func_int_val
{
public:
  Item_func_ceiling(THD *thd, Item *a): Item_func_int_val(thd, a) {}
  const char *func_name() const { return "ceiling"; }
  longlong int_op();
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_ceiling>(thd, mem_root, this); }
};


class Item_func_floor :public Item_func_int_val
{
public:
  Item_func_floor(THD *thd, Item *a): Item_func_int_val(thd, a) {}
  const char *func_name() const { return "floor"; }
  longlong int_op();
  double real_op();
  my_decimal *decimal_op(my_decimal *);
  bool check_partition_func_processor(void *int_arg) {return FALSE;}
  bool check_vcol_func_processor(void *arg) { return FALSE;}
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_floor>(thd, mem_root, this); }
};

/* This handles round and truncate */

class Item_func_round :public Item_func_num1
{
  bool truncate;
public:
  Item_func_round(THD *thd, Item *a, Item *b, bool trunc_arg)
    :Item_func_num1(thd, a, b), truncate(trunc_arg) {}
  const char *func_name() const { return truncate ? "truncate" : "round"; }
  double real_op();
  longlong int_op();
  my_decimal *decimal_op(my_decimal *);
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_round>(thd, mem_root, this); }
};


class Item_func_rand :public Item_real_func
{
  struct my_rnd_struct *rand;
  bool first_eval; // TRUE if val_real() is called 1st time
public:
  Item_func_rand(THD *thd, Item *a):
    Item_real_func(thd, a), rand(0), first_eval(TRUE) {}
  Item_func_rand(THD *thd): Item_real_func(thd) {}
  double val_real();
  const char *func_name() const { return "rand"; }
  bool const_item() const { return 0; }
  void update_used_tables();
  bool fix_fields(THD *thd, Item **ref);
  void cleanup() { first_eval= TRUE; Item_real_func::cleanup(); }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_rand>(thd, mem_root, this); }
private:
  void seed_random (Item * val);  
};


class Item_func_sign :public Item_int_func
{
public:
  Item_func_sign(THD *thd, Item *a): Item_int_func(thd, a) {}
  const char *func_name() const { return "sign"; }
  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_sign>(thd, mem_root, this); }
};


class Item_func_units :public Item_real_func
{
  char *name;
  double mul,add;
public:
  Item_func_units(THD *thd, char *name_arg, Item *a, double mul_arg,
                  double add_arg):
    Item_real_func(thd, a), name(name_arg), mul(mul_arg), add(add_arg) {}
  double val_real();
  const char *func_name() const { return name; }
  bool fix_length_and_dec()
  { decimals= NOT_FIXED_DEC; max_length= float_length(decimals); return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_units>(thd, mem_root, this); }
};


/**
  Item_func_min_max does not derive from Item_func_hybrid_field_type
  because the way how its methods val_xxx() and get_date() work depend
  not only by its arguments, but also on the context in which
  LEAST() and GREATEST() appear.
  For example, using Item_func_min_max in a CAST like this:
    CAST(LEAST('11','2') AS SIGNED)
  forces Item_func_min_max to compare the arguments as numbers rather
  than strings.
  Perhaps this should be changed eventually (see MDEV-5893).
*/
class Item_func_min_max :public Item_hybrid_func
{
  String tmp_value;
  int cmp_sign;
public:
  Item_func_min_max(THD *thd, List<Item> &list, int cmp_sign_arg):
    Item_hybrid_func(thd, list), cmp_sign(cmp_sign_arg)
  {}
  double val_real();
  longlong val_int();
  String *val_str(String *);
  my_decimal *val_decimal(my_decimal *);
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  bool fix_length_and_dec();
};

class Item_func_min :public Item_func_min_max
{
public:
  Item_func_min(THD *thd, List<Item> &list): Item_func_min_max(thd, list, 1) {}
  const char *func_name() const { return "least"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_min>(thd, mem_root, this); }
};

class Item_func_max :public Item_func_min_max
{
public:
  Item_func_max(THD *thd, List<Item> &list): Item_func_min_max(thd, list, -1) {}
  const char *func_name() const { return "greatest"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_max>(thd, mem_root, this); }
};


/* 
  Objects of this class are used for ROLLUP queries to wrap up 
  each constant item referred to in GROUP BY list. 
*/

class Item_func_rollup_const :public Item_func
{
public:
  Item_func_rollup_const(THD *thd, Item *a): Item_func(thd, a)
  {
    name= a->name;
    name_length= a->name_length;
  }
  double val_real() { return val_real_from_item(args[0]); }
  longlong val_int() { return val_int_from_item(args[0]); }
  String *val_str(String *str) { return val_str_from_item(args[0], str); }
  my_decimal *val_decimal(my_decimal *dec)
    { return val_decimal_from_item(args[0], dec); }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
    { return get_date_from_item(args[0], ltime, fuzzydate); }
  const char *func_name() const { return "rollup_const"; }
  bool const_item() const { return 0; }
  Item_result result_type() const { return args[0]->result_type(); }
  enum_field_types field_type() const { return args[0]->field_type(); }
  bool fix_length_and_dec()
  {
    collation= args[0]->collation;
    max_length= args[0]->max_length;
    decimals=args[0]->decimals; 
    /* The item could be a NULL constant. */
    null_value= args[0]->is_null();
    return FALSE;
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_rollup_const>(thd, mem_root, this); }
};


class Item_func_length :public Item_int_func
{
  String value;
public:
  Item_func_length(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "length"; }
  bool fix_length_and_dec() { max_length=10; return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_length>(thd, mem_root, this); }
};

class Item_func_bit_length :public Item_func_length
{
public:
  Item_func_bit_length(THD *thd, Item *a): Item_func_length(thd, a) {}
  longlong val_int()
    { DBUG_ASSERT(fixed == 1); return Item_func_length::val_int()*8; }
  const char *func_name() const { return "bit_length"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_bit_length>(thd, mem_root, this); }
};

class Item_func_char_length :public Item_int_func
{
  String value;
public:
  Item_func_char_length(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "char_length"; }
  bool fix_length_and_dec() { max_length=10; return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_char_length>(thd, mem_root, this); }
};

class Item_func_coercibility :public Item_int_func
{
public:
  Item_func_coercibility(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "coercibility"; }
  bool fix_length_and_dec() { max_length=10; maybe_null= 0; return FALSE; }
  bool eval_not_null_tables(void *)
  {
    not_null_tables_cache= 0;
    return false;
  }
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
  { return this; }
  bool const_item() const { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_coercibility>(thd, mem_root, this); }
};

class Item_func_locate :public Item_int_func
{
  String value1,value2;
  DTCollation cmp_collation;
public:
  Item_func_locate(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  Item_func_locate(THD *thd, Item *a, Item *b, Item *c): Item_int_func(thd, a, b, c) {}
  const char *func_name() const { return "locate"; }
  longlong val_int();
  bool fix_length_and_dec();
  virtual void print(String *str, enum_query_type query_type);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_locate>(thd, mem_root, this); }
};


class Item_func_field :public Item_int_func
{
  String value,tmp;
  Item_result cmp_type;
  DTCollation cmp_collation;
public:
  Item_func_field(THD *thd, List<Item> &list): Item_int_func(thd, list) {}
  longlong val_int();
  const char *func_name() const { return "field"; }
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_field>(thd, mem_root, this); }
};


class Item_func_ascii :public Item_int_func
{
  String value;
public:
  Item_func_ascii(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "ascii"; }
  bool fix_length_and_dec() { max_length=3; return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_ascii>(thd, mem_root, this); }
};

class Item_func_ord :public Item_int_func
{
  String value;
public:
  Item_func_ord(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "ord"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_ord>(thd, mem_root, this); }
};

class Item_func_find_in_set :public Item_int_func
{
  String value,value2;
  uint enum_value;
  ulonglong enum_bit;
  DTCollation cmp_collation;
public:
  Item_func_find_in_set(THD *thd, Item *a, Item *b):
    Item_int_func(thd, a, b), enum_value(0) {}
  longlong val_int();
  const char *func_name() const { return "find_in_set"; }
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_find_in_set>(thd, mem_root, this); }
};

/* Base class for all bit functions: '~', '|', '^', '&', '>>', '<<' */

class Item_func_bit: public Item_int_func
{
public:
  Item_func_bit(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  Item_func_bit(THD *thd, Item *a): Item_int_func(thd, a) {}
  bool fix_length_and_dec() { unsigned_flag= 1; return FALSE; }

  virtual inline void print(String *str, enum_query_type query_type)
  {
    print_op(str, query_type);
  }
  bool need_parentheses_in_default() { return true; }
};

class Item_func_bit_or :public Item_func_bit
{
public:
  Item_func_bit_or(THD *thd, Item *a, Item *b): Item_func_bit(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "|"; }
  enum precedence precedence() const { return BITOR_PRECEDENCE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_bit_or>(thd, mem_root, this); }
};

class Item_func_bit_and :public Item_func_bit
{
public:
  Item_func_bit_and(THD *thd, Item *a, Item *b): Item_func_bit(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "&"; }
  enum precedence precedence() const { return BITAND_PRECEDENCE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_bit_and>(thd, mem_root, this); }
};

class Item_func_bit_count :public Item_int_func
{
public:
  Item_func_bit_count(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "bit_count"; }
  bool fix_length_and_dec() { max_length=2; return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_bit_count>(thd, mem_root, this); }
};

class Item_func_shift_left :public Item_func_bit
{
public:
  Item_func_shift_left(THD *thd, Item *a, Item *b): Item_func_bit(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "<<"; }
  enum precedence precedence() const { return SHIFT_PRECEDENCE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_shift_left>(thd, mem_root, this); }
};

class Item_func_shift_right :public Item_func_bit
{
public:
  Item_func_shift_right(THD *thd, Item *a, Item *b): Item_func_bit(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return ">>"; }
  enum precedence precedence() const { return SHIFT_PRECEDENCE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_shift_right>(thd, mem_root, this); }
};

class Item_func_bit_neg :public Item_func_bit
{
public:
  Item_func_bit_neg(THD *thd, Item *a): Item_func_bit(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "~"; }
  enum precedence precedence() const { return NEG_PRECEDENCE; }
  void print(String *str, enum_query_type query_type)
  {
    str->append(func_name());
    args[0]->print_parenthesised(str, query_type, precedence());
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_bit_neg>(thd, mem_root, this); }
};


class Item_func_last_insert_id :public Item_int_func
{
public:
  Item_func_last_insert_id(THD *thd): Item_int_func(thd) {}
  Item_func_last_insert_id(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "last_insert_id"; }
  bool fix_length_and_dec()
  {
    unsigned_flag= TRUE;
    if (arg_count)
      max_length= args[0]->max_length;
    unsigned_flag=1;
    return FALSE;
  }
  bool fix_fields(THD *thd, Item **ref);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_last_insert_id>(thd, mem_root, this); }
};


class Item_func_benchmark :public Item_int_func
{
public:
  Item_func_benchmark(THD *thd, Item *count_expr, Item *expr):
    Item_int_func(thd, count_expr, expr)
  {}
  longlong val_int();
  const char *func_name() const { return "benchmark"; }
  bool fix_length_and_dec() { max_length=1; maybe_null=0; return FALSE; }
  virtual void print(String *str, enum_query_type query_type);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_benchmark>(thd, mem_root, this); }
};


void item_func_sleep_init(void);
void item_func_sleep_free(void);

class Item_func_sleep :public Item_int_func
{
public:
  Item_func_sleep(THD *thd, Item *a): Item_int_func(thd, a) {}
  bool const_item() const { return 0; }
  const char *func_name() const { return "sleep"; }
  table_map used_tables() const
  {
    return used_tables_cache | RAND_TABLE_BIT;
  }
  bool is_expensive() { return 1; }
  longlong val_int();
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_sleep>(thd, mem_root, this); }
};



#ifdef HAVE_DLOPEN

class Item_udf_func :public Item_func
{
  /**
    Mark "this" as non-deterministic if it uses no tables
    and is not a constant at the same time.
  */
  void set_non_deterministic_if_needed()
  {
    if (!const_item_cache && !used_tables_cache)
      used_tables_cache= RAND_TABLE_BIT;
  }
protected:
  udf_handler udf;
  bool is_expensive_processor(void *arg) { return TRUE; }

public:
  Item_udf_func(THD *thd, udf_func *udf_arg):
    Item_func(thd), udf(udf_arg) {}
  Item_udf_func(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_func(thd, list), udf(udf_arg) {}
  const char *func_name() const { return udf.name(); }
  enum Functype functype() const   { return UDF_FUNC; }
  bool fix_fields(THD *thd, Item **ref)
  {
    DBUG_ASSERT(fixed == 0);
    bool res= udf.fix_fields(thd, this, arg_count, args);
    set_non_deterministic_if_needed();
    fixed= 1;
    return res;
  }
  void fix_num_length_and_dec();
  void update_used_tables()
  {
    /*
      TODO: Make a member in UDF_INIT and return if a UDF is deterministic or
      not.
      Currently UDF_INIT has a member (const_item) that is an in/out 
      parameter to the init() call.
      The code in udf_handler::fix_fields also duplicates the arguments 
      handling code in Item_func::fix_fields().
      
      The lack of information if a UDF is deterministic makes writing
      a correct update_used_tables() for UDFs impossible.
      One solution to this would be :
       - Add a is_deterministic member of UDF_INIT
       - (optionally) deprecate the const_item member of UDF_INIT
       - Take away the duplicate code from udf_handler::fix_fields() and
         make Item_udf_func call Item_func::fix_fields() to process its 
         arguments as for any other function.
       - Store the deterministic flag returned by <udf>_init into the 
       udf_handler. 
       - Don't implement Item_udf_func::fix_fields, implement
       Item_udf_func::fix_length_and_dec() instead (similar to non-UDF
       functions).
       - Override Item_func::update_used_tables to call 
       Item_func::update_used_tables() and add a RAND_TABLE_BIT to the 
       result of Item_func::update_used_tables() if the UDF is 
       non-deterministic.
       - (optionally) rename RAND_TABLE_BIT to NONDETERMINISTIC_BIT to
       better describe its usage.
       
      The above would require a change of the UDF API.
      Until that change is done here's how the current code works:
      We call Item_func::update_used_tables() only when we know that
      the function depends on real non-const tables and is deterministic.
      This can be done only because we know that the optimizer will
      call update_used_tables() only when there's possibly a new const
      table. So update_used_tables() can only make a Item_func more
      constant than it is currently.
      That's why we don't need to do anything if a function is guaranteed
      to return non-constant (it's non-deterministic) or is already a
      const.
    */  
    if ((used_tables_cache & ~PSEUDO_TABLE_BITS) && 
        !(used_tables_cache & RAND_TABLE_BIT))
    {
      Item_func::update_used_tables();
      set_non_deterministic_if_needed();
    }
  }
  void cleanup();
  Item_result result_type () const { return udf.result_type(); }
  bool eval_not_null_tables(void *opt_arg)
  {
    not_null_tables_cache= 0;
    return 0;
  }
  bool is_expensive() { return 1; }
  virtual void print(String *str, enum_query_type query_type);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_NON_DETERMINISTIC);
  }
};


class Item_func_udf_float :public Item_udf_func
{
 public:
  Item_func_udf_float(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_float(THD *thd, udf_func *udf_arg,
                      List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  longlong val_int()
  {
    DBUG_ASSERT(fixed == 1);
    return Converter_double_to_longlong(Item_func_udf_float::val_real(),
                                        unsigned_flag).result();
  }
  my_decimal *val_decimal(my_decimal *dec_buf)
  {
    double res=val_real();
    if (null_value)
      return NULL;
    double2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf);
    return dec_buf;
  }
  double val_real();
  String *val_str(String *str);
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  bool fix_length_and_dec() { fix_num_length_and_dec(); return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_udf_float>(thd, mem_root, this); }
};


class Item_func_udf_int :public Item_udf_func
{
public:
  Item_func_udf_int(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_int(THD *thd, udf_func *udf_arg,
                    List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  longlong val_int();
  double val_real() { return (double) Item_func_udf_int::val_int(); }
  String *val_str(String *str);
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  bool fix_length_and_dec() { decimals= 0; max_length= 21; return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_udf_int>(thd, mem_root, this); }
};


class Item_func_udf_decimal :public Item_udf_func
{
public:
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  longlong val_int();
  double val_real();
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String *str);
  enum Item_result result_type () const { return DECIMAL_RESULT; }
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
  bool fix_length_and_dec() { fix_num_length_and_dec(); return FALSE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_udf_decimal>(thd, mem_root, this); }
};


class Item_func_udf_str :public Item_udf_func
{
public:
  Item_func_udf_str(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_str(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  String *val_str(String *);
  double val_real()
  {
    int err_not_used;
    char *end_not_used;
    String *res;
    res= val_str(&str_value);
    return res ? my_strntod(res->charset(),(char*) res->ptr(), 
                            res->length(), &end_not_used, &err_not_used) : 0.0;
  }
  longlong val_int()
  {
    int err_not_used;
    String *res;  res=val_str(&str_value);
    return res ? my_strntoll(res->charset(),res->ptr(),res->length(),10,
                             (char**) 0, &err_not_used) : (longlong) 0;
  }
  my_decimal *val_decimal(my_decimal *dec_buf)
  {
    String *res=val_str(&str_value);
    if (!res)
      return NULL;
    string2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf);
    return dec_buf;
  }
  enum Item_result result_type () const { return STRING_RESULT; }
  enum_field_types field_type() const { return string_field_type(); }
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_udf_str>(thd, mem_root, this); }
};

#else /* Dummy functions to get sql_yacc.cc compiled */

class Item_func_udf_float :public Item_real_func
{
 public:
  Item_func_udf_float(THD *thd, udf_func *udf_arg):
    Item_real_func(thd) {}
  Item_func_udf_float(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_real_func(thd, list) {}
  double val_real() { DBUG_ASSERT(fixed == 1); return 0.0; }
};


class Item_func_udf_int :public Item_int_func
{
public:
  Item_func_udf_int(THD *thd, udf_func *udf_arg):
    Item_int_func(thd) {}
  Item_func_udf_int(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_int_func(thd, list) {}
  longlong val_int() { DBUG_ASSERT(fixed == 1); return 0; }
};


class Item_func_udf_decimal :public Item_int_func
{
public:
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg):
    Item_int_func(thd) {}
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_int_func(thd, list) {}
  my_decimal *val_decimal(my_decimal *) { DBUG_ASSERT(fixed == 1); return 0; }
};


class Item_func_udf_str :public Item_func
{
public:
  Item_func_udf_str(THD *thd, udf_func *udf_arg):
    Item_func(thd) {}
  Item_func_udf_str(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_func(thd, list) {}
  String *val_str(String *)
    { DBUG_ASSERT(fixed == 1); null_value=1; return 0; }
  double val_real() { DBUG_ASSERT(fixed == 1); null_value= 1; return 0.0; }
  longlong val_int() { DBUG_ASSERT(fixed == 1); null_value=1; return 0; }
  enum Item_result result_type () const { return STRING_RESULT; }
  bool fix_length_and_dec() { maybe_null=1; max_length=0; return FALSE; }
};

#endif /* HAVE_DLOPEN */

void mysql_ull_cleanup(THD *thd);
void mysql_ull_set_explicit_lock_duration(THD *thd);

class Item_func_get_lock :public Item_int_func
{
  String value;
 public:
  Item_func_get_lock(THD *thd, Item *a, Item *b) :Item_int_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "get_lock"; }
  bool fix_length_and_dec() { max_length=1; maybe_null=1; return FALSE; }
  table_map used_tables() const
  {
    return used_tables_cache | RAND_TABLE_BIT;
  }
  bool const_item() const { return 0; }
  bool is_expensive() { return 1; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_get_lock>(thd, mem_root, this); }
};

class Item_func_release_lock :public Item_int_func
{
  String value;
public:
  Item_func_release_lock(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "release_lock"; }
  bool fix_length_and_dec() { max_length= 1; maybe_null= 1; return FALSE; }
  table_map used_tables() const
  {
    return used_tables_cache | RAND_TABLE_BIT;
  }
  bool const_item() const { return 0; }
  bool is_expensive() { return 1; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_release_lock>(thd, mem_root, this); }
};

/* replication functions */

class Item_master_pos_wait :public Item_int_func
{
  String value;
public:
  Item_master_pos_wait(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  Item_master_pos_wait(THD *thd, Item *a, Item *b, Item *c):
    Item_int_func(thd, a, b, c) {}
  Item_master_pos_wait(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_int_func(thd, a, b, c, d) {}
  longlong val_int();
  const char *func_name() const { return "master_pos_wait"; }
  bool fix_length_and_dec() { max_length=21; maybe_null=1; return FALSE; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_master_pos_wait>(thd, mem_root, this); }
};


class Item_master_gtid_wait :public Item_int_func
{
  String value;
public:
  Item_master_gtid_wait(THD *thd, Item *a): Item_int_func(thd, a) {}
  Item_master_gtid_wait(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "master_gtid_wait"; }
  bool fix_length_and_dec() { max_length=2; return FALSE; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_master_gtid_wait>(thd, mem_root, this); }
};


/* Handling of user definable variables */

class user_var_entry;


/**
  A class to set and get user variables
*/
class Item_func_user_var :public Item_hybrid_func
{
protected:
  user_var_entry *m_var_entry;
public:
  LEX_STRING name; // keep it public
  Item_func_user_var(THD *thd, LEX_STRING a)
    :Item_hybrid_func(thd), m_var_entry(NULL), name(a) { }
  Item_func_user_var(THD *thd, LEX_STRING a, Item *b)
    :Item_hybrid_func(thd, b), m_var_entry(NULL), name(a) { }
  Item_func_user_var(THD *thd, Item_func_user_var *item)
    :Item_hybrid_func(thd, item),
    m_var_entry(item->m_var_entry), name(item->name) { }
  bool check_vcol_func_processor(void *arg);
};


class Item_func_set_user_var :public Item_func_user_var
{
  /*
    The entry_thread_id variable is used:
    1) to skip unnecessary updates of the entry field (see above);
    2) to reset the entry field that was initialized in the other thread
       (for example, an item tree of a trigger that updates user variables
       may be shared between several connections, and the entry_thread_id field
       prevents updates of one connection user variables from a concurrent
       connection calling the same trigger that initially updated some
       user variable it the first connection context).
  */
  my_thread_id entry_thread_id;
  String value;
  my_decimal decimal_buff;
  bool null_item;
  union
  {
    longlong vint;
    double vreal;
    String *vstr;
    my_decimal *vdec;
  } save_result;

public:
  Item_func_set_user_var(THD *thd, LEX_STRING a, Item *b):
    Item_func_user_var(thd, a, b),
    entry_thread_id(0)
  {}
  Item_func_set_user_var(THD *thd, Item_func_set_user_var *item)
    :Item_func_user_var(thd, item),
    entry_thread_id(item->entry_thread_id),
    value(item->value), decimal_buff(item->decimal_buff),
    null_item(item->null_item), save_result(item->save_result)
  {}

  enum Functype functype() const { return SUSERVAR_FUNC; }
  double val_real();
  longlong val_int();
  String *val_str(String *str);
  my_decimal *val_decimal(my_decimal *);
  double val_result();
  longlong val_int_result();
  bool val_bool_result();
  String *str_result(String *str);
  my_decimal *val_decimal_result(my_decimal *);
  bool is_null_result();
  bool update_hash(void *ptr, uint length, enum Item_result type,
                   CHARSET_INFO *cs, bool unsigned_arg);
  bool send(Protocol *protocol, String *str_arg);
  void make_field(THD *thd, Send_field *tmp_field);
  bool check(bool use_result_field);
  void save_item_result(Item *item);
  bool update();
  bool fix_fields(THD *thd, Item **ref);
  bool fix_length_and_dec();
  Field *create_field_for_create_select(TABLE *table)
  {
    return result_type() != STRING_RESULT ?
           create_tmp_field(false, table, MY_INT32_NUM_DECIMAL_DIGITS) :
           tmp_table_field_from_field_type(table, false, true);
  }
  void print(String *str, enum_query_type query_type);
  enum precedence precedence() const { return ASSIGN_PRECEDENCE; }
  void print_as_stmt(String *str, enum_query_type query_type);
  const char *func_name() const { return "set_user_var"; }
  int save_in_field(Field *field, bool no_conversions,
                    bool can_use_result_field);
  int save_in_field(Field *field, bool no_conversions)
  {
    return save_in_field(field, no_conversions, 1);
  }
  void save_org_in_field(Field *field,
                         fast_field_copier data __attribute__ ((__unused__)))
    { (void)save_in_field(field, 1, 0); }
  bool register_field_in_read_map(void *arg);
  bool register_field_in_bitmap(void *arg);
  bool set_entry(THD *thd, bool create_if_not_exists);
  void cleanup();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_set_user_var>(thd, mem_root, this); }
  bool excl_dep_on_table(table_map tab_map) { return false; }
};


class Item_func_get_user_var :public Item_func_user_var,
                              private Settable_routine_parameter
{
public:
  Item_func_get_user_var(THD *thd, LEX_STRING a):
    Item_func_user_var(thd, a) {}
  enum Functype functype() const { return GUSERVAR_FUNC; }
  LEX_STRING get_name() { return name; }
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal*);
  String *val_str(String* str);
  bool fix_length_and_dec();
  virtual void print(String *str, enum_query_type query_type);
  /*
    We must always return variables as strings to guard against selects of type
    select @t1:=1,@t1,@t:="hello",@t from foo where (@t1:= t2.b)
  */
  const char *func_name() const { return "get_user_var"; }
  bool const_item() const;
  table_map used_tables() const
  { return const_item() ? 0 : RAND_TABLE_BIT; }
  bool eq(const Item *item, bool binary_cmp) const;
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_get_user_var>(thd, mem_root, this); }
private:
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it);

public:
  Settable_routine_parameter *get_settable_routine_parameter()
  {
    return this;
  }
};


/*
  This item represents user variable used as out parameter (e.g in LOAD DATA),
  and it is supposed to be used only for this purprose. So it is simplified
  a lot. Actually you should never obtain its value.

  The only two reasons for this thing being an Item is possibility to store it
  in List<Item> and desire to place this code somewhere near other functions
  working with user variables.
*/
class Item_user_var_as_out_param :public Item,
                                  public Load_data_outvar
{
  LEX_STRING name;
  user_var_entry *entry;
public:
  Item_user_var_as_out_param(THD *thd, LEX_STRING a): Item(thd), name(a)
  { set_name(thd, a.str, 0, system_charset_info); }
  Load_data_outvar *get_load_data_outvar()
  {
    return this;
  }
  bool load_data_set_null(THD *thd, const Load_data_param *param)
  {
    set_null_value(param->charset());
    return false;
  }
  bool load_data_set_no_data(THD *thd, const Load_data_param *param)
  {
    set_null_value(param->charset());
    return false;
  }
  bool load_data_set_value(THD *thd, const char *pos, uint length,
                           const Load_data_param *param)
  {
    set_value(pos, length, param->charset());
    return false;
  }
  void load_data_print_for_log_event(THD *thd, String *to) const;
  bool load_data_add_outvar(THD *thd, Load_data_param *param) const
  {
    return param->add_outvar_user_var(thd);
  }
  uint load_data_fixed_length() const
  {
    return 0;
  }
  /* We should return something different from FIELD_ITEM here */
  enum Type type() const { return STRING_ITEM;}
  double val_real();
  longlong val_int();
  String *val_str(String *str);
  my_decimal *val_decimal(my_decimal *decimal_buffer);
  /* fix_fields() binds variable name with its entry structure */
  bool fix_fields(THD *thd, Item **ref);
  void set_null_value(CHARSET_INFO* cs);
  void set_value(const char *str, uint length, CHARSET_INFO* cs);
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_user_var_as_out_param>(thd, mem_root, this); }
};


/* A system variable */

#define GET_SYS_VAR_CACHE_LONG     1
#define GET_SYS_VAR_CACHE_DOUBLE   2
#define GET_SYS_VAR_CACHE_STRING   4

class Item_func_get_system_var :public Item_func
{
  sys_var *var;
  enum_var_type var_type, orig_var_type;
  LEX_STRING component;
  longlong cached_llval;
  double cached_dval;
  String cached_strval;
  bool cached_null_value;
  query_id_t used_query_id;
  uchar cache_present;

public:
  Item_func_get_system_var(THD *thd, sys_var *var_arg,
                           enum_var_type var_type_arg,
                           LEX_STRING *component_arg, const char *name_arg,
                           size_t name_len_arg);
  enum Functype functype() const { return GSYSVAR_FUNC; }
  void update_null_value();
  bool fix_length_and_dec();
  void print(String *str, enum_query_type query_type);
  bool const_item() const { return true; }
  table_map used_tables() const { return 0; }
  enum Item_result result_type() const;
  enum_field_types field_type() const;
  double val_real();
  longlong val_int();
  String* val_str(String*);
  my_decimal *val_decimal(my_decimal *dec_buf)
  { return val_decimal_from_real(dec_buf); }
  /* TODO: fix to support views */
  const char *func_name() const { return "get_system_var"; }
  /**
    Indicates whether this system variable is written to the binlog or not.

    Variables are written to the binlog as part of "status_vars" in
    Query_log_event, as an Intvar_log_event, or a Rand_log_event.

    @return true if the variable is written to the binlog, false otherwise.
  */
  bool is_written_to_binlog();
  bool eq(const Item *item, bool binary_cmp) const;

  void cleanup();
  bool check_vcol_func_processor(void *arg);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_get_system_var>(thd, mem_root, this); }
};


/* for fulltext search */

class Item_func_match :public Item_real_func
{
public:
  uint key, flags;
  bool join_key;
  DTCollation cmp_collation;
  FT_INFO *ft_handler;
  TABLE *table;
  Item_func_match *master;   // for master-slave optimization
  Item *concat_ws;           // Item_func_concat_ws
  String value;              // value of concat_ws
  String search_value;       // key_item()'s value converted to cmp_collation

  Item_func_match(THD *thd, List<Item> &a, uint b):
    Item_real_func(thd, a), key(0), flags(b), join_key(0), ft_handler(0),
    table(0), master(0), concat_ws(0) { }
  void cleanup()
  {
    DBUG_ENTER("Item_func_match::cleanup");
    Item_real_func::cleanup();
    if (!master && ft_handler)
      ft_handler->please->close_search(ft_handler);
    ft_handler= 0;
    concat_ws= 0;
    table= 0;           // required by Item_func_match::eq()
    DBUG_VOID_RETURN;
  }
  bool is_expensive_processor(void *arg) { return TRUE; }
  enum Functype functype() const { return FT_FUNC; }
  const char *func_name() const { return "match"; }
  bool eval_not_null_tables(void *opt_arg)
  {
    not_null_tables_cache= 0;
    return 0;
  }
  bool fix_fields(THD *thd, Item **ref);
  bool eq(const Item *, bool binary_cmp) const;
  /* The following should be safe, even if we compare doubles */
  longlong val_int() { DBUG_ASSERT(fixed == 1); return val_real() != 0.0; }
  double val_real();
  virtual void print(String *str, enum_query_type query_type);

  bool fix_index();
  void init_search(THD *thd, bool no_order);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function("match ... against()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_match>(thd, mem_root, this); }
  Item *build_clone(THD *thd, MEM_ROOT *mem_root) { return 0; }
private:
  /**
     Check whether storage engine for given table, 
     allows FTS Boolean search on non-indexed columns.

     @todo A flag should be added to the extended fulltext API so that 
           it may be checked whether search on non-indexed columns are 
           supported. Currently, it is not possible to check for such a 
           flag since @c this->ft_handler is not yet set when this function is 
           called.  The current hack is to assume that search on non-indexed
           columns are supported for engines that does not support the extended
           fulltext API (e.g., MyISAM), while it is not supported for other 
           engines (e.g., InnoDB)

     @param table_arg Table for which storage engine to check

     @retval true if BOOLEAN search on non-indexed columns is supported
     @retval false otherwise
   */
  bool allows_search_on_non_indexed_columns(TABLE* table_arg)
  {
    // Only Boolean search may support non_indexed columns
    if (!(flags & FT_BOOL))
      return false;

    DBUG_ASSERT(table_arg && table_arg->file);

    // Assume that if extended fulltext API is not supported,
    // non-indexed columns are allowed.  This will be true for MyISAM.
    if ((table_arg->file->ha_table_flags() & HA_CAN_FULLTEXT_EXT) == 0)
      return true;

    return false;
  }
};


class Item_func_bit_xor : public Item_func_bit
{
public:
  Item_func_bit_xor(THD *thd, Item *a, Item *b): Item_func_bit(thd, a, b) {}
  longlong val_int();
  const char *func_name() const { return "^"; }
  enum precedence precedence() const { return BITXOR_PRECEDENCE; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_bit_xor>(thd, mem_root, this); }
};

class Item_func_is_free_lock :public Item_int_func
{
  String value;
public:
  Item_func_is_free_lock(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "is_free_lock"; }
  bool fix_length_and_dec()
  {
    decimals=0; max_length=1; maybe_null=1;
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_is_free_lock>(thd, mem_root, this); }
};

class Item_func_is_used_lock :public Item_int_func
{
  String value;
public:
  Item_func_is_used_lock(THD *thd, Item *a): Item_int_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "is_used_lock"; }
  bool fix_length_and_dec()
  {
    decimals=0; max_length=10; maybe_null=1;
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_is_used_lock>(thd, mem_root, this); }
};

/* For type casts */

enum Cast_target
{
  ITEM_CAST_BINARY, ITEM_CAST_SIGNED_INT, ITEM_CAST_UNSIGNED_INT,
  ITEM_CAST_DATE, ITEM_CAST_TIME, ITEM_CAST_DATETIME, ITEM_CAST_CHAR,
  ITEM_CAST_DECIMAL, ITEM_CAST_DOUBLE
};


struct Lex_cast_type_st: public Lex_length_and_dec_st
{
private:
  Cast_target m_type;
public:
  void set(Cast_target type, const char *length, const char *dec)
  {
    m_type= type;
    Lex_length_and_dec_st::set(length, dec);
  }
  void set(Cast_target type, Lex_length_and_dec_st length_and_dec)
  {
    m_type= type;
    Lex_length_and_dec_st::operator=(length_and_dec);
  }
  void set(Cast_target type, const char *length)
  {
    set(type, length, 0);
  }
  void set(Cast_target type)
  {
    set(type, 0, 0);
  }
  Cast_target type() const { return m_type; }
};


class Item_func_row_count :public Item_int_func
{
public:
  Item_func_row_count(THD *thd): Item_int_func(thd) {}
  longlong val_int();
  const char *func_name() const { return "row_count"; }
  bool fix_length_and_dec() { decimals= 0; maybe_null=0; return FALSE; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_row_count>(thd, mem_root, this); }
};


/*
 *
 * Stored FUNCTIONs
 *
 */

class sp_head;
class sp_name;
struct st_sp_security_context;

class Item_func_sp :public Item_func
{
private:
  Name_resolution_context *context;
  sp_name *m_name;
  mutable sp_head *m_sp;
  TABLE *dummy_table;
  uchar result_buf[64];
  /*
     The result field of the concrete stored function.
  */
  Field *sp_result_field;

  bool execute();
  bool execute_impl(THD *thd);
  bool init_result_field(THD *thd);

protected:
  bool is_expensive_processor(void *arg)
  { return is_expensive(); }
  
public:

  Item_func_sp(THD *thd, Name_resolution_context *context_arg, sp_name *name);

  Item_func_sp(THD *thd, Name_resolution_context *context_arg,
               sp_name *name, List<Item> &list);

  virtual ~Item_func_sp()
  {}

  void update_used_tables();

  void cleanup();

  const char *func_name() const;

  enum enum_field_types field_type() const;

  Field *create_field_for_create_select(TABLE *table)
  {
    return result_type() != STRING_RESULT ?
           sp_result_field :
           tmp_table_field_from_field_type(table, false, false);
  }
  void make_field(THD *thd, Send_field *tmp_field);

  Item_result result_type() const;

  longlong val_int()
  {
    if (execute())
      return (longlong) 0;
    return sp_result_field->val_int();
  }

  double val_real()
  {
    if (execute())
      return 0.0;
    return sp_result_field->val_real();
  }

  my_decimal *val_decimal(my_decimal *dec_buf)
  {
    if (execute())
      return NULL;
    return sp_result_field->val_decimal(dec_buf);
  }

  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  {
    if (execute())
      return true;
    return sp_result_field->get_date(ltime, fuzzydate);
  }

  String *val_str(String *str)
  {
    String buf;
    char buff[20];
    buf.set(buff, 20, str->charset());
    buf.length(0);
    if (execute())
      return NULL;
    /*
      result_field will set buf pointing to internal buffer
      of the resul_field. Due to this it will change any time
      when SP is executed. In order to prevent occasional
      corruption of returned value, we make here a copy.
    */
    sp_result_field->val_str(&buf);
    str->copy(buf);
    return str;
  }

  void update_null_value()
  { 
    execute();
  }

  virtual bool change_context_processor(void *cntx)
    { context= (Name_resolution_context *)cntx; return FALSE; }

  bool sp_check_access(THD * thd);
  virtual enum Functype functype() const { return FUNC_SP; }

  bool fix_fields(THD *thd, Item **ref);
  bool fix_length_and_dec(void);
  bool is_expensive();

  inline Field *get_sp_result_field()
  {
    return sp_result_field;
  }

  bool check_vcol_func_processor(void *arg);
  bool limit_index_condition_pushdown_processor(void *opt_arg)
  {
    return TRUE;
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root) { return 0; }
  bool eval_not_null_tables(void *opt_arg)
  {
    not_null_tables_cache= 0;
    return 0;
  }
};


class Item_func_found_rows :public Item_int_func
{
public:
  Item_func_found_rows(THD *thd): Item_int_func(thd) {}
  longlong val_int();
  const char *func_name() const { return "found_rows"; }
  bool fix_length_and_dec() { decimals= 0; maybe_null=0; return FALSE; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_found_rows>(thd, mem_root, this); }
};


void uuid_short_init();

class Item_func_uuid_short :public Item_int_func
{
public:
  Item_func_uuid_short(THD *thd): Item_int_func(thd) {}
  const char *func_name() const { return "uuid_short"; }
  longlong val_int();
  bool const_item() const { return false; }
  bool fix_length_and_dec()
  { max_length= 21; unsigned_flag=1; return FALSE; }
  table_map used_tables() const { return RAND_TABLE_BIT; }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_uuid_short>(thd, mem_root, this); }
};


class Item_func_last_value :public Item_func
{
protected:
  Item *last_value;
public:
  Item_func_last_value(THD *thd, List<Item> &list): Item_func(thd, list) {}
  double val_real();
  longlong val_int();
  String *val_str(String *);
  my_decimal *val_decimal(my_decimal *);
  bool fix_length_and_dec();
  enum Item_result result_type () const { return last_value->result_type(); }
  const char *func_name() const { return "last_value"; }
  bool eval_not_null_tables(void *)
  {
    not_null_tables_cache= 0;
    return 0;
  }
  enum_field_types field_type() const { return last_value->field_type(); }
  bool const_item() const { return 0; }
  void evaluate_sideeffects();
  void update_used_tables()
  {
    Item_func::update_used_tables();
    maybe_null= last_value->maybe_null;
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_last_value>(thd, mem_root, this); }
};


Item *get_system_var(THD *thd, enum_var_type var_type, LEX_STRING name,
                     LEX_STRING component);
extern bool check_reserved_words(LEX_STRING *name);
extern enum_field_types agg_field_type(Item **items, uint nitems,
                                       bool treat_bit_as_number);
double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate);

extern bool volatile  mqh_used;

bool update_hash(user_var_entry *entry, bool set_null, void *ptr, uint length,
                 Item_result type, CHARSET_INFO *cs,
                 bool unsigned_arg);

#endif /* ITEM_FUNC_INCLUDED */
