#ifndef ITEM_FUNC_INCLUDED
#define ITEM_FUNC_INCLUDED
/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
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

#include "sql_udf.h"    // udf_handler
#include "my_decimal.h" // string2my_decimal
#include <cmath>


class Item_func :public Item_func_or_sum
{
  void sync_with_sum_func_and_with_field(List<Item> &list);
protected:
  virtual bool check_arguments() const
  {
    return check_argument_types_scalar(0, arg_count);
  }
  bool check_argument_types_like_args0() const;
  bool check_argument_types_scalar(uint start, uint end) const;
  bool check_argument_types_traditional_scalar(uint start, uint end) const;
  bool check_argument_types_or_binary(const Type_handler *handler,
                                      uint start, uint end) const;
  bool check_argument_types_can_return_int(uint start, uint end) const;
  bool check_argument_types_can_return_real(uint start, uint end) const;
  bool check_argument_types_can_return_str(uint start, uint end) const;
  bool check_argument_types_can_return_text(uint start, uint end) const;
  bool check_argument_types_can_return_date(uint start, uint end) const;
  bool check_argument_types_can_return_time(uint start, uint end) const;
  void print_cast_temporal(String *str, enum_query_type query_type);
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
                  NOT_FUNC, NOT_ALL_FUNC, TEMPTABLE_ROWID,
                  NOW_FUNC, NOW_UTC_FUNC, SYSDATE_FUNC, TRIG_COND_FUNC,
                  SUSERVAR_FUNC, GUSERVAR_FUNC, COLLATE_FUNC,
                  EXTRACT_FUNC, CHAR_TYPECAST_FUNC, FUNC_SP, UDF_FUNC,
                  NEG_FUNC, GSYSVAR_FUNC, IN_OPTIMIZER_FUNC, DYNCOL_FUNC,
                  JSON_EXTRACT_FUNC, JSON_VALID_FUNC, ROWNUM_FUNC,
                  CASE_SEARCHED_FUNC, // Used by ColumnStore/Spider
                  CASE_SIMPLE_FUNC,   // Used by ColumnStore/spider,
                };
  static scalar_comparison_op functype_to_scalar_comparison_op(Functype type)
  {
    switch (type) {
    case EQ_FUNC:    return SCALAR_CMP_EQ;
    case EQUAL_FUNC: return SCALAR_CMP_EQUAL;
    case LT_FUNC:    return SCALAR_CMP_LT;
    case LE_FUNC:    return SCALAR_CMP_LE;
    case GE_FUNC:    return SCALAR_CMP_GE;
    case GT_FUNC:    return SCALAR_CMP_GT;
    default: break;
    }
    DBUG_ASSERT(0);
    return SCALAR_CMP_EQ;
  }
  enum Type type() const override { return FUNC_ITEM; }
  virtual enum Functype functype() const   { return UNKNOWN_FUNC; }
  Item_func(THD *thd): Item_func_or_sum(thd)
  {
    DBUG_ASSERT(with_flags == item_with_t::NONE);
    with_flags= item_with_t::NONE;
  }
  Item_func(THD *thd, Item *a): Item_func_or_sum(thd, a)
  {
    with_flags= a->with_flags;
  }
  Item_func(THD *thd, Item *a, Item *b):
    Item_func_or_sum(thd, a, b)
  {
    with_flags= a->with_flags | b->with_flags;
  }
  Item_func(THD *thd, Item *a, Item *b, Item *c):
    Item_func_or_sum(thd, a, b, c)
  {
    with_flags|= a->with_flags | b->with_flags | c->with_flags;
  }
  Item_func(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_func_or_sum(thd, a, b, c, d)
  {
    with_flags= a->with_flags | b->with_flags | c->with_flags | d->with_flags;
  }
  Item_func(THD *thd, Item *a, Item *b, Item *c, Item *d, Item* e):
    Item_func_or_sum(thd, a, b, c, d, e)
  {
    with_flags= (a->with_flags | b->with_flags | c->with_flags | d->with_flags |
                 e->with_flags);
  }
  Item_func(THD *thd, List<Item> &list):
    Item_func_or_sum(thd, list)
  {
    set_arguments(thd, list);
  }
  // Constructor used for Item_cond_and/or (see Item comment)
  Item_func(THD *thd, Item_func *item):
    Item_func_or_sum(thd, item),
    not_null_tables_cache(item->not_null_tables_cache)
  { }
  bool fix_fields(THD *, Item **ref) override;
  void cleanup() override
  {
    Item_func_or_sum::cleanup();
    used_tables_and_const_cache_init();
  }
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  void quick_fix_field() override;
  table_map not_null_tables() const override;
  void update_used_tables() override
  {
    used_tables_and_const_cache_init();
    used_tables_and_const_cache_update_and_join(arg_count, args);
  }
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override
  {
    DBUG_ENTER("Item_func::get_mm_tree");
    DBUG_RETURN(const_item() ? get_mm_tree_for_const(param) : NULL);
  }
  bool eq(const Item *item, bool binary_cmp) const override;
  virtual Item *key_item() const { return args[0]; }
  void set_arguments(THD *thd, List<Item> &list)
  {
    Item_args::set_arguments(thd, list);
    sync_with_sum_func_and_with_field(list);
    list.empty();                                     // Fields are used
  }
  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                      List<Item> &fields, uint flags) override;
  void print(String *str, enum_query_type query_type) override;
  void print_op(String *str, enum_query_type query_type);
  void print_args(String *str, uint from, enum_query_type query_type);
  bool is_null() override
  { 
    update_null_value();
    return null_value; 
  }
  String *val_str_from_val_str_ascii(String *str, String *str2);

  void signal_divide_by_null();
  friend class udf_handler;
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table) override
  { return tmp_table_field_from_field_type(root, table); }
  Item *get_tmp_table_item(THD *thd) override;

  void fix_char_length_ulonglong(ulonglong max_char_length_arg)
  {
    ulonglong max_result_length= max_char_length_arg *
                                 collation.collation->mbmaxlen;
    if (max_result_length >= MAX_BLOB_WIDTH)
    {
      max_length= MAX_BLOB_WIDTH;
      set_maybe_null();
    }
    else
      max_length= (uint32) max_result_length;
  }
  Item *transform(THD *thd, Item_transformer transformer, uchar *arg) override;
  Item* compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                Item_transformer transformer, uchar *arg_t) override;
  void traverse_cond(Cond_traverser traverser,
                     void * arg, traverse_order order) override;
  bool eval_not_null_tables(void *opt_arg) override;
  bool find_not_null_fields(table_map allowed) override;
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
    DBUG_ASSERT(fixed());
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
    DBUG_ASSERT(fixed());
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
    DBUG_ASSERT(fixed());
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
    DBUG_ASSERT(fixed());
    for (uint i= 0; i < arg_count; i++)
    {
      if (args[i]->type() == Item::FIELD_ITEM &&
          args[i]->field_type() == MYSQL_TYPE_DATETIME)
        return TRUE;
    }
    return FALSE;
  }

  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    /*
      By default only substitution for a field whose two different values
      are never equal is allowed in the arguments of a function.
      This is overruled for the direct arguments of comparison functions.
    */
    Item_args::propagate_equal_fields(thd, Context_identity(), cond);
    return this;
  }

  bool has_rand_bit()
  {
    return used_tables() & RAND_TABLE_BIT;
  }

  bool excl_dep_on_table(table_map tab_map) override
  {
    if (used_tables() & (OUTER_REF_TABLE_BIT | RAND_TABLE_BIT))
      return false; 
    return !(used_tables() & ~tab_map) || 
           Item_args::excl_dep_on_table(tab_map);
  }

  bool excl_dep_on_grouping_fields(st_select_lex *sel) override
  {
    if (has_rand_bit() || with_subquery())
      return false;
    return Item_args::excl_dep_on_grouping_fields(sel);
  }

  bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred) override
  {
    return Item_args::excl_dep_on_in_subq_left_part(subq_pred);
  }

  /*
    We assume the result of any function that has a TIMESTAMP argument to be
    timezone-dependent, since a TIMESTAMP value in both numeric and string
    contexts is interpreted according to the current timezone.
    The only exception is UNIX_TIMESTAMP() which returns the internal
    representation of a TIMESTAMP argument verbatim, and thus does not depend on
    the timezone.
   */
  bool check_valid_arguments_processor(void *bool_arg) override
  {
    return has_timestamp_args();
  }

  bool find_function_processor (void *arg) override
  {
    return functype() == *(Functype *) arg;
  }

  void no_rows_in_result() override
  {
    for (uint i= 0; i < arg_count; i++)
    {
      args[i]->no_rows_in_result();
    }
  }
  void restore_to_before_no_rows_in_result() override
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
  Item_func *get_item_func() override { return this; }
  bool is_simplified_cond_processor(void *arg) override
  { return const_item() && !val_int(); }
};


class Item_real_func :public Item_func
{
public:
  Item_real_func(THD *thd): Item_func(thd) { collation= DTCollation_numeric(); }
  Item_real_func(THD *thd, Item *a): Item_func(thd, a)
  { collation= DTCollation_numeric(); }
  Item_real_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b)
  { collation= DTCollation_numeric(); }
  Item_real_func(THD *thd, List<Item> &list): Item_func(thd, list)
  { collation= DTCollation_numeric(); }
  String *val_str(String*str) override;
  my_decimal *val_decimal(my_decimal *decimal_value) override;
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    return Converter_double_to_longlong(val_real(), unsigned_flag).result();
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_real(thd, ltime, fuzzydate); }
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  bool fix_length_and_dec() override
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(decimals);
    return FALSE;
  }
};


/**
  Functions whose returned field type is determined at fix_fields() time.
*/
class Item_hybrid_func: public Item_func,
                        public Type_handler_hybrid_field_type
{
protected:
  bool fix_attributes(Item **item, uint nitems);
public:
  Item_hybrid_func(THD *thd): Item_func(thd) { }
  Item_hybrid_func(THD *thd, Item *a):  Item_func(thd, a) { }
  Item_hybrid_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b) { }
  Item_hybrid_func(THD *thd, Item *a, Item *b, Item *c):
    Item_func(thd, a, b, c) { }
  Item_hybrid_func(THD *thd, List<Item> &list): Item_func(thd, list) { }
  Item_hybrid_func(THD *thd, Item_hybrid_func *item)
    :Item_func(thd, item), Type_handler_hybrid_field_type(item) { }
  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }
  void fix_length_and_dec_long_or_longlong(uint char_length, bool unsigned_arg)
  {
    collation= DTCollation_numeric();
    unsigned_flag= unsigned_arg;
    max_length= char_length;
    set_handler(Type_handler::type_handler_long_or_longlong(char_length,
                                                            unsigned_arg));
  }
  void fix_length_and_dec_ulong_or_ulonglong_by_nbits(uint nbits)
  {
    uint digits= Type_handler_bit::Bit_decimal_notation_int_digits_by_nbits(nbits);
    collation= DTCollation_numeric();
    unsigned_flag= true;
    max_length= digits;
    if (nbits > 32)
      set_handler(&type_handler_ulonglong);
    else
      set_handler(&type_handler_ulong);
  }
};


class Item_handled_func: public Item_func
{
public:
  class Handler
  {
  public:
    virtual ~Handler() { }
    virtual String *val_str(Item_handled_func *, String *) const= 0;
    virtual String *val_str_ascii(Item_handled_func *, String *) const= 0;
    virtual double val_real(Item_handled_func *) const= 0;
    virtual longlong val_int(Item_handled_func *) const= 0;
    virtual my_decimal *val_decimal(Item_handled_func *, my_decimal *) const= 0;
    virtual bool get_date(THD *thd, Item_handled_func *, MYSQL_TIME *, date_mode_t fuzzydate) const= 0;
    virtual bool val_native(THD *thd, Item_handled_func *, Native *to) const
    {
      DBUG_ASSERT(0);
      to->length(0);
      return true;
    }
    virtual const Type_handler *
      return_type_handler(const Item_handled_func *item) const= 0;
    virtual const Type_handler *
      type_handler_for_create_select(const Item_handled_func *item) const
    {
      return return_type_handler(item);
    }
    virtual bool fix_length_and_dec(Item_handled_func *) const= 0;
  };

  class Handler_str: public Handler
  {
  public:
    String *val_str_ascii(Item_handled_func *item, String *str) const
    {
      return item->Item::val_str_ascii(str);
    }
    double val_real(Item_handled_func *item) const
    {
      DBUG_ASSERT(item->fixed());
      StringBuffer<64> tmp;
      String *res= item->val_str(&tmp);
      return res ? item->double_from_string_with_check(res) : 0.0;
    }
    longlong val_int(Item_handled_func *item) const
    {
      DBUG_ASSERT(item->fixed());
      StringBuffer<22> tmp;
      String *res= item->val_str(&tmp);
      return res ? item->longlong_from_string_with_check(res) : 0;
    }
    my_decimal *val_decimal(Item_handled_func *item, my_decimal *to) const
    {
      return item->val_decimal_from_string(to);
    }
    bool get_date(THD *thd, Item_handled_func *item, MYSQL_TIME *to,
                  date_mode_t fuzzydate) const
    {
      return item->get_date_from_string(thd, to, fuzzydate);
    }
  };

  /**
    Abstract class for functions returning TIME, DATE, DATETIME or string values,
    whose data type depends on parameters and is set at fix_fields time.
  */
  class Handler_temporal: public Handler
  {
  public:
    String *val_str(Item_handled_func *item, String *to) const
    {
      StringBuffer<MAX_FIELD_WIDTH> ascii_buf;
      return item->val_str_from_val_str_ascii(to, &ascii_buf);
    }
  };

  /**
    Abstract class for functions returning strings,
    which are generated from get_date() results,
    when get_date() can return different MYSQL_TIMESTAMP_XXX per row.
  */
  class Handler_temporal_string: public Handler_temporal
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *) const
    {
      return &type_handler_string;
    }
    const Type_handler *
      type_handler_for_create_select(const Item_handled_func *item) const
    {
      return return_type_handler(item)->type_handler_for_tmp_table(item);
    }
    double val_real(Item_handled_func *item) const
    {
      return Temporal_hybrid(item).to_double();
    }
    longlong val_int(Item_handled_func *item) const
    {
      return Temporal_hybrid(item).to_longlong();
    }
    my_decimal *val_decimal(Item_handled_func *item, my_decimal *to) const
    {
      return Temporal_hybrid(item).to_decimal(to);
    }
    String *val_str_ascii(Item_handled_func *item, String *to) const
    {
      return Temporal_hybrid(item).to_string(to, item->decimals);
    }
  };


  class Handler_date: public Handler_temporal
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *) const
    {
      return &type_handler_newdate;
    }
    bool fix_length_and_dec(Item_handled_func *item) const
    {
      item->fix_attributes_date();
      return false;
    }
    double val_real(Item_handled_func *item) const
    {
      return Date(item).to_double();
    }
    longlong val_int(Item_handled_func *item) const
    {
      return Date(item).to_longlong();
    }
    my_decimal *val_decimal(Item_handled_func *item, my_decimal *to) const
    {
      return Date(item).to_decimal(to);
    }
    String *val_str_ascii(Item_handled_func *item, String *to) const
    {
      return Date(item).to_string(to);
    }
  };


  class Handler_time: public Handler_temporal
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *) const
    {
      return &type_handler_time2;
    }
    double val_real(Item_handled_func *item) const
    {
      return Time(item).to_double();
    }
    longlong val_int(Item_handled_func *item) const
    {
      return Time(item).to_longlong();
    }
    my_decimal *val_decimal(Item_handled_func *item, my_decimal *to) const
    {
      return Time(item).to_decimal(to);
    }
    String *val_str_ascii(Item_handled_func *item, String *to) const
    {
      return Time(item).to_string(to, item->decimals);
    }
    bool val_native(THD *thd, Item_handled_func *item, Native *to) const
    {
      return Time(thd, item).to_native(to, item->decimals);
    }
  };


  class Handler_datetime: public Handler_temporal
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *) const
    {
      return &type_handler_datetime2;
    }
    double val_real(Item_handled_func *item) const
    {
      return Datetime(item).to_double();
    }
    longlong val_int(Item_handled_func *item) const
    {
      return Datetime(item).to_longlong();
    }
    my_decimal *val_decimal(Item_handled_func *item, my_decimal *to) const
    {
      return Datetime(item).to_decimal(to);
    }
    String *val_str_ascii(Item_handled_func *item, String *to) const
    {
      return Datetime(item).to_string(to, item->decimals);
    }
  };


  class Handler_int: public Handler
  {
  public:
    String *val_str(Item_handled_func *item, String *to) const
    {
      longlong nr= val_int(item);
      if (item->null_value)
        return 0;
      to->set_int(nr, item->unsigned_flag, item->collation.collation);
      return to;
    }
    String *val_str_ascii(Item_handled_func *item, String *to) const
    {
      return item->Item::val_str_ascii(to);
    }
    double val_real(Item_handled_func *item) const
    {
      return item->unsigned_flag ? (double) ((ulonglong) val_int(item)) :
                                   (double) val_int(item);
    }
    my_decimal *val_decimal(Item_handled_func *item, my_decimal *to) const
    {
      return item->val_decimal_from_int(to);
    }
    bool get_date(THD *thd, Item_handled_func *item,
                  MYSQL_TIME *to, date_mode_t fuzzydate) const
    {
      return item->get_date_from_int(thd, to, fuzzydate);
    }
    longlong val_int(Item_handled_func *item) const
    {
      Longlong_null tmp= to_longlong_null(item);
      item->null_value= tmp.is_null();
      return tmp.value();
    }
    virtual Longlong_null to_longlong_null(Item_handled_func *item) const= 0;
  };

  class Handler_slong: public Handler_int
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *item) const
    {
      return &type_handler_slong;
    }
    bool fix_length_and_dec(Item_handled_func *item) const
    {
      item->unsigned_flag= false;
      item->collation= DTCollation_numeric();
      item->fix_char_length(11);
      return false;
    }
  };

  class Handler_slong2: public Handler_slong
  {
  public:
    bool fix_length_and_dec(Item_handled_func *func) const
    {
      bool rc= Handler_slong::fix_length_and_dec(func);
      func->max_length= 2;
      return rc;
    }
  };

  class Handler_ulonglong: public Handler_int
  {
  public:
    const Type_handler *return_type_handler(const Item_handled_func *item) const
    {
      return &type_handler_ulonglong;
    }
    bool fix_length_and_dec(Item_handled_func *item) const
    {
      item->unsigned_flag= true;
      item->collation= DTCollation_numeric();
      item->fix_char_length(21);
      return false;
    }
  };

protected:
  const Handler *m_func_handler;
public:
  Item_handled_func(THD *thd, Item *a)
   :Item_func(thd, a), m_func_handler(NULL) { }
  Item_handled_func(THD *thd, Item *a, Item *b)
   :Item_func(thd, a, b), m_func_handler(NULL) { }
  void set_func_handler(const Handler *handler)
  {
    m_func_handler= handler;
  }
  const Type_handler *type_handler() const override
  {
    return m_func_handler->return_type_handler(this);
  }
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table) override
  {
    DBUG_ASSERT(fixed());
    const Type_handler *h= m_func_handler->type_handler_for_create_select(this);
    return h->make_and_init_table_field(root, &name,
                                        Record_addr(maybe_null()),
                                        *this, table);
  }
  String *val_str(String *to) override
  {
    return m_func_handler->val_str(this, to);
  }
  String *val_str_ascii(String *to) override
  {
    return m_func_handler->val_str_ascii(this, to);
  }
  double val_real() override
  {
    return m_func_handler->val_real(this);
  }
  longlong val_int() override
  {
    return m_func_handler->val_int(this);
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    return m_func_handler->val_decimal(this, to);
  }
  bool get_date(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate) override
  {
    return m_func_handler->get_date(thd, this, to, fuzzydate);
  }
  bool val_native(THD *thd, Native *to) override
  {
    return m_func_handler->val_native(thd, this, to);
  }
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
  bool date_op_with_null_check(THD *thd, MYSQL_TIME *ltime)
  {
     bool rc= date_op(thd, ltime, date_mode_t(0));
     DBUG_ASSERT(!rc ^ null_value);
     return rc;
  }
  bool time_op_with_null_check(THD *thd, MYSQL_TIME *ltime)
  {
     bool rc= time_op(thd, ltime);
     DBUG_ASSERT(!rc ^ null_value);
     DBUG_ASSERT(rc || ltime->time_type == MYSQL_TIMESTAMP_TIME);
     return rc;
  }
  String *str_op_with_null_check(String *str)
  {
    String *res= str_op(str);
    DBUG_ASSERT((res != NULL) ^ null_value);
    return res;
  }

public:
  // Value methods that involve no conversion
  String *val_str_from_str_op(String *str)
  {
    return str_op_with_null_check(&str_value);
  }
  longlong val_int_from_int_op()
  {
    return int_op();
  }
  double val_real_from_real_op()
  {
    return real_op();
  }

  // Value methods that involve conversion
  String *val_str_from_real_op(String *str);
  String *val_str_from_int_op(String *str);
  String *val_str_from_date_op(String *str);
  String *val_str_from_time_op(String *str);

  my_decimal *val_decimal_from_str_op(my_decimal *dec);
  my_decimal *val_decimal_from_real_op(my_decimal *dec);
  my_decimal *val_decimal_from_int_op(my_decimal *dec);
  my_decimal *val_decimal_from_date_op(my_decimal *dec);
  my_decimal *val_decimal_from_time_op(my_decimal *dec);

  longlong val_int_from_str_op();
  longlong val_int_from_real_op();
  longlong val_int_from_date_op();
  longlong val_int_from_time_op();

  double val_real_from_str_op();
  double val_real_from_date_op();
  double val_real_from_time_op();
  double val_real_from_int_op();

public:
  Item_func_hybrid_field_type(THD *thd):
    Item_hybrid_func(thd)
  { collation= DTCollation_numeric(); }
  Item_func_hybrid_field_type(THD *thd, Item *a):
    Item_hybrid_func(thd, a)
  { collation= DTCollation_numeric(); }
  Item_func_hybrid_field_type(THD *thd, Item *a, Item *b):
    Item_hybrid_func(thd, a, b)
  { collation= DTCollation_numeric(); }
  Item_func_hybrid_field_type(THD *thd, Item *a, Item *b, Item *c):
    Item_hybrid_func(thd, a, b, c)
  { collation= DTCollation_numeric(); }
  Item_func_hybrid_field_type(THD *thd, List<Item> &list):
    Item_hybrid_func(thd, list)
  { collation= DTCollation_numeric(); }

  double val_real() override
  {
    DBUG_ASSERT(fixed());
    return Item_func_hybrid_field_type::type_handler()->
           Item_func_hybrid_field_type_val_real(this);
  }
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    return Item_func_hybrid_field_type::type_handler()->
           Item_func_hybrid_field_type_val_int(this);
  }
  my_decimal *val_decimal(my_decimal *dec) override
  {
    DBUG_ASSERT(fixed());
    return Item_func_hybrid_field_type::type_handler()->
           Item_func_hybrid_field_type_val_decimal(this, dec);
  }
  String *val_str(String*str) override
  {
    DBUG_ASSERT(fixed());
    String *res= Item_func_hybrid_field_type::type_handler()->
                 Item_func_hybrid_field_type_val_str(this, str);
    DBUG_ASSERT(null_value == (res == NULL));
    return res;
  }
  bool get_date(THD *thd, MYSQL_TIME *to, date_mode_t mode) override
  {
    DBUG_ASSERT(fixed());
    return Item_func_hybrid_field_type::type_handler()->
           Item_func_hybrid_field_type_get_date_with_warn(thd, this, to, mode);
  }

  bool val_native(THD *thd, Native *to) override
  {
    DBUG_ASSERT(fixed());
    return native_op(thd, to);
  }

  /**
     @brief Performs the operation that this functions implements when the
     result type is INT.

     @return The result of the operation.
  */
  virtual longlong int_op()= 0;
  Longlong_null to_longlong_null_op()
  {
    longlong nr= int_op();
    /*
      C++ does not guarantee the order of parameter evaluation,
      so to make sure "null_value" is passed to the constructor
      after the int_op() call, int_op() is caled on a separate line.
    */
    return Longlong_null(nr, null_value);
  }
  Longlong_hybrid_null to_longlong_hybrid_null_op()
  {
    return Longlong_hybrid_null(to_longlong_null_op(), unsigned_flag);
  }

  /**
     @brief Performs the operation that this functions implements when the
     result type is REAL.

     @return The result of the operation.
  */
  virtual double real_op()= 0;
  Double_null to_double_null_op()
  {
    // val_real() must be caleed on a separate line. See to_longlong_null()
    double nr= real_op();
    return Double_null(nr, null_value);
  }

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
     field type is DATETIME or DATE.
     @return The result of the operation.
  */
  virtual bool date_op(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate)= 0;

  /**
     @brief Performs the operation that this functions implements when
     field type is TIME.
     @return The result of the operation.
  */
  virtual bool time_op(THD *thd, MYSQL_TIME *res)= 0;

  virtual bool native_op(THD *thd, Native *native)= 0;
};


/*
  This class resembles SQL standard CASE-alike expressions:
  CASE and its abbreviations COALESCE, NULLIF, IFNULL, IF.

  <case expression> ::=   <case abbreviation>
                        | <case specification>
*/
class Item_func_case_expression: public Item_func_hybrid_field_type
{
public:
  Item_func_case_expression(THD *thd)
   :Item_func_hybrid_field_type(thd)
  { }
  Item_func_case_expression(THD *thd, Item *a)
   :Item_func_hybrid_field_type(thd, a)
  { }
  Item_func_case_expression(THD *thd, Item *a, Item *b)
   :Item_func_hybrid_field_type(thd, a, b)
  { }
  Item_func_case_expression(THD *thd, Item *a, Item *b, Item *c)
   :Item_func_hybrid_field_type(thd, a, b, c)
  { }
  Item_func_case_expression(THD *thd, List<Item> &list):
    Item_func_hybrid_field_type(thd, list)
  { }
  bool find_not_null_fields(table_map allowed) { return false; }
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
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool time_op(THD *thd, MYSQL_TIME *ltime)
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool native_op(THD *thd, Native *to)
  {
    DBUG_ASSERT(0);
    return true;
  }
};


/* function where type of result detected by first argument */
class Item_func_num1: public Item_func_numhybrid
{
public:
  Item_func_num1(THD *thd, Item *a): Item_func_numhybrid(thd, a) {}
  Item_func_num1(THD *thd, Item *a, Item *b): Item_func_numhybrid(thd, a, b) {}
  bool check_partition_func_processor(void *int_arg) { return FALSE; }
  bool check_vcol_func_processor(void *arg) { return FALSE; }
};


/* Base class for operations like '+', '-', '*' */
class Item_num_op :public Item_func_numhybrid
{
protected:
  bool check_arguments() const override
  {
    return false; // Checked by aggregate_for_num_op()
  }
public:
  Item_num_op(THD *thd, Item *a, Item *b): Item_func_numhybrid(thd, a, b) {}
  virtual void result_precision()= 0;

  void print(String *str, enum_query_type query_type) override
  {
    print_op(str, query_type);
  }
  bool fix_type_handler(const Type_aggregator *aggregator);
  void fix_length_and_dec_double()
  {
    aggregate_numeric_attributes_real(args, arg_count);
    max_length= float_length(decimals);
  }
  void fix_length_and_dec_decimal()
  {
    unsigned_flag= args[0]->unsigned_flag & args[1]->unsigned_flag;
    result_precision();
    fix_decimals();
  }
  void fix_length_and_dec_int()
  {
    unsigned_flag= args[0]->unsigned_flag | args[1]->unsigned_flag;
    result_precision();
    decimals= 0;
    set_handler(type_handler_long_or_longlong());
  }
  void fix_length_and_dec_temporal(bool downcast_decimal_to_int)
  {
    set_handler(&type_handler_newdecimal);
    fix_length_and_dec_decimal();
    if (decimals == 0 && downcast_decimal_to_int)
      set_handler(type_handler_long_or_longlong());
  }
  bool need_parentheses_in_default() override { return true; }
};


class Item_int_func :public Item_func
{
public:
  /*
    QQ: shouldn't 20 characters be enough:
    Max unsigned =  18,446,744,073,709,551,615 = 20 digits, 20 characters
    Max signed   =   9,223,372,036,854,775,807 = 19 digits, 19 characters
    Min signed   =  -9,223,372,036,854,775,808 = 19 digits, 20 characters
  */
  Item_int_func(THD *thd): Item_func(thd)
  { collation= DTCollation_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a): Item_func(thd, a)
  { collation= DTCollation_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a, Item *b): Item_func(thd, a, b)
  { collation= DTCollation_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a, Item *b, Item *c): Item_func(thd, a, b, c)
  { collation= DTCollation_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_func(thd, a, b, c, d)
  { collation= DTCollation_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, List<Item> &list): Item_func(thd, list)
  { collation= DTCollation_numeric(); fix_char_length(21); }
  Item_int_func(THD *thd, Item_int_func *item) :Item_func(thd, item)
  { collation= DTCollation_numeric(); }
  double val_real() override;
  String *val_str(String*str) override;
  my_decimal *val_decimal(my_decimal *decimal_value) override
  {
    return val_decimal_from_int(decimal_value);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return get_date_from_int(thd, ltime, fuzzydate); }
  const Type_handler *type_handler() const override= 0;
  bool fix_length_and_dec() override { return FALSE; }
};


class Item_long_func: public Item_int_func
{
public:
  Item_long_func(THD *thd): Item_int_func(thd) { }
  Item_long_func(THD *thd, Item *a): Item_int_func(thd, a) {}
  Item_long_func(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  Item_long_func(THD *thd, Item *a, Item *b, Item *c): Item_int_func(thd, a, b, c) {}
  Item_long_func(THD *thd, List<Item> &list): Item_int_func(thd, list) { }
  Item_long_func(THD *thd, Item_long_func *item) :Item_int_func(thd, item) {}
  const Type_handler *type_handler() const override
  {
    if (unsigned_flag)
      return &type_handler_ulong;
    return &type_handler_slong;
  }
  bool fix_length_and_dec() override { max_length= 11; return FALSE; }
};


class Item_func_hash: public Item_int_func
{
public:
  Item_func_hash(THD *thd, List<Item> &item): Item_int_func(thd, item)
  {}
  longlong val_int() override;
  bool fix_length_and_dec() override;
  const Type_handler *type_handler() const override
  { return &type_handler_slong; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_hash>(thd, this); }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<hash>") };
    return name;
  }
};

class Item_longlong_func: public Item_int_func
{
public:
  Item_longlong_func(THD *thd): Item_int_func(thd) { }
  Item_longlong_func(THD *thd, Item *a): Item_int_func(thd, a) {}
  Item_longlong_func(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  Item_longlong_func(THD *thd, Item *a, Item *b, Item *c): Item_int_func(thd, a, b, c) {}
  Item_longlong_func(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_int_func(thd, a, b, c, d) {}
  Item_longlong_func(THD *thd, List<Item> &list): Item_int_func(thd, list) { }
  Item_longlong_func(THD *thd, Item_longlong_func *item) :Item_int_func(thd, item) {}
  const Type_handler *type_handler() const override
  {
    if (unsigned_flag)
      return &type_handler_ulonglong;
    return &type_handler_slonglong;
  }
};


class Cursor_ref
{
protected:
  LEX_CSTRING m_cursor_name;
  uint m_cursor_offset;
  class sp_cursor *get_open_cursor_or_error();
  Cursor_ref(const LEX_CSTRING *name, uint offset)
   :m_cursor_name(*name), m_cursor_offset(offset)
  { }
  void print_func(String *str, const LEX_CSTRING &func_name);
};



class Item_func_cursor_rowcount: public Item_longlong_func,
                                 public Cursor_ref
{
public:
  Item_func_cursor_rowcount(THD *thd, const LEX_CSTRING *name, uint offset)
   :Item_longlong_func(thd), Cursor_ref(name, offset)
  {
    set_maybe_null();
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("%ROWCOUNT") };
    return name;
  }
  longlong val_int() override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), arg, VCOL_SESSION_FUNC);
  }
  void print(String *str, enum_query_type query_type) override
  {
    return Cursor_ref::print_func(str, func_name_cstring());
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_cursor_rowcount>(thd, this); }
};



class Item_func_connection_id :public Item_long_func
{
  longlong value;

public:
  Item_func_connection_id(THD *thd): Item_long_func(thd) { unsigned_flag=1; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("connection_id") };
    return name;
  }
  bool fix_length_and_dec() override;
  bool fix_fields(THD *thd, Item **ref) override;
  longlong val_int() override { DBUG_ASSERT(fixed()); return value; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_SESSION_FUNC);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_connection_id>(thd, this); }
};


class Item_func_signed :public Item_int_func
{
public:
  Item_func_signed(THD *thd, Item *a): Item_int_func(thd, a)
  {
    unsigned_flag= 0;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cast_as_signed") };
    return name;
  }
  const Type_handler *type_handler() const override
  {
    return Type_handler::type_handler_long_or_longlong(max_char_length(),
                                                       false);
  }
  longlong val_int() override
  {
    longlong value= args[0]->val_int_signed_typecast();
    null_value= args[0]->null_value;
    return value;
  }
  void fix_length_and_dec_double()
  {
    fix_char_length(MAX_BIGINT_WIDTH);
  }
  void fix_length_and_dec_generic()
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
  }
  void fix_length_and_dec_string()
  {
    /*
      For strings, use decimal_int_part() instead of max_char_length().
      This is important for Item_hex_hybrid:
        SELECT CAST(0x1FFFFFFFF AS SIGNED);
      Length is 5, decimal_int_part() is 13.
    */
    uint32 char_length= MY_MIN(args[0]->decimal_int_part(),
                               MY_INT64_NUM_DECIMAL_DIGITS);
    set_if_bigger(char_length, 1U + (unsigned_flag ? 0 : 1));
    fix_char_length(char_length);
  }
  bool fix_length_and_dec() override
  {
    return args[0]->type_handler()->Item_func_signed_fix_length_and_dec(this);
  }
  void print(String *str, enum_query_type query_type) override;
  decimal_digits_t decimal_precision() const override
  { return args[0]->decimal_precision(); }
  bool need_parentheses_in_default() override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_signed>(thd, this); }
};


class Item_func_unsigned :public Item_func_signed
{
public:
  Item_func_unsigned(THD *thd, Item *a): Item_func_signed(thd, a)
  {
    unsigned_flag= 1;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cast_as_unsigned") };
    return name;
  }
  const Type_handler *type_handler() const override
  {
    if (max_char_length() <= MY_INT32_NUM_DECIMAL_DIGITS - 1)
      return &type_handler_ulong;
    return &type_handler_ulonglong;
  }
  longlong val_int() override
  {
    longlong value= args[0]->val_int_unsigned_typecast();
    null_value= args[0]->null_value;
    return value;
  }
  bool fix_length_and_dec() override
  {
    return args[0]->type_handler()->Item_func_unsigned_fix_length_and_dec(this);
  }
  decimal_digits_t decimal_precision() const override { return max_length; }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_unsigned>(thd, this); }
};


class Item_decimal_typecast :public Item_func
{
  my_decimal decimal_value;
public:
  Item_decimal_typecast(THD *thd, Item *a, uint len, decimal_digits_t dec)
   :Item_func(thd, a)
  {
    decimals= dec;
    collation= DTCollation_numeric();
    fix_char_length(my_decimal_precision_to_length_no_truncation(len, dec,
                                                                 unsigned_flag));
  }
  String *val_str(String *str) override { return VDec(this).to_string(str); }
  double val_real() override { return VDec(this).to_double(); }
  longlong val_int() override { return VDec(this).to_longlong(unsigned_flag); }
  my_decimal *val_decimal(my_decimal*) override;
  bool get_date(THD *thd, MYSQL_TIME *to, date_mode_t mode) override
  {
    return decimal_to_datetime_with_warn(thd, VDec(this).ptr(), to, mode,
                                         NULL, NULL);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
  void fix_length_and_dec_generic() {}
  bool fix_length_and_dec() override
  {
    return
      args[0]->type_handler()->Item_decimal_typecast_fix_length_and_dec(this);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("decimal_typecast") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  bool need_parentheses_in_default() override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_decimal_typecast>(thd, this); }
};


class Item_real_typecast: public Item_real_func
{
protected:
  double val_real_with_truncate(double max_value);
public:
  Item_real_typecast(THD *thd, Item *a, uint len, uint dec)
   :Item_real_func(thd, a)
  {
    decimals=   (uint8)  dec;
    max_length= (uint32) len;
  }
  bool need_parentheses_in_default() { return true; }
  void print(String *str, enum_query_type query_type);
  void fix_length_and_dec_generic()
  {
    set_maybe_null();
  }
};


class Item_float_typecast :public Item_real_typecast
{
public:
  Item_float_typecast(THD *thd, Item *a)
   :Item_real_typecast(thd, a, MAX_FLOAT_STR_LENGTH, NOT_FIXED_DEC)
  { }
  const Type_handler *type_handler() const override
  { return &type_handler_float; }
  bool fix_length_and_dec() override
  {
    return
      args[0]->type_handler()->Item_float_typecast_fix_length_and_dec(this);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("float_typecast") };
    return name;
  }
  double val_real() override
  {
    return (double) (float) val_real_with_truncate(FLT_MAX);
  }
  String *val_str(String*str) override
  {
    Float nr(Item_float_typecast::val_real());
    if (null_value)
      return 0;
    nr.to_string(str, decimals);
    return str;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_float_typecast>(thd, this); }
};


class Item_double_typecast :public Item_real_typecast
{
public:
  Item_double_typecast(THD *thd, Item *a, uint len, uint dec):
    Item_real_typecast(thd, a, len, dec)
  { }
  bool fix_length_and_dec() override
  {
    return
      args[0]->type_handler()->Item_double_typecast_fix_length_and_dec(this);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("double_typecast") };
    return name;
  }
  double val_real() override { return val_real_with_truncate(DBL_MAX); }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_double_typecast>(thd, this); }
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
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("+") };
    return name;
  }
  enum precedence precedence() const override { return ADD_PRECEDENCE; }
  bool fix_length_and_dec() override;
  longlong int_op() override;
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_plus>(thd, this); }
};

class Item_func_minus :public Item_func_additive_op
{
  bool m_depends_on_sql_mode_no_unsigned_subtraction;
public:
  Item_func_minus(THD *thd, Item *a, Item *b):
    Item_func_additive_op(thd, a, b),
    m_depends_on_sql_mode_no_unsigned_subtraction(false)
  { }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("-") };
    return name;
  }
  enum precedence precedence() const override { return ADD_PRECEDENCE; }
  Sql_mode_dependency value_depends_on_sql_mode() const override;
  longlong int_op() override;
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  bool fix_length_and_dec() override;
  void fix_unsigned_flag();
  void fix_length_and_dec_double()
  {
    Item_func_additive_op::fix_length_and_dec_double();
    fix_unsigned_flag();
  }
  void fix_length_and_dec_decimal()
  {
    Item_func_additive_op::fix_length_and_dec_decimal();
    fix_unsigned_flag();
  }
  void fix_length_and_dec_int()
  {
    Item_func_additive_op::fix_length_and_dec_int();
    fix_unsigned_flag();
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_minus>(thd, this); }
};


class Item_func_mul :public Item_num_op
{
public:
  Item_func_mul(THD *thd, Item *a, Item *b):
    Item_num_op(thd, a, b) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("*") };
    return name;
  }
  enum precedence precedence() const override { return MUL_PRECEDENCE; }
  longlong int_op() override;
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  void result_precision() override;
  bool fix_length_and_dec() override;
  bool check_partition_func_processor(void *int_arg) override {return FALSE;}
  bool check_vcol_func_processor(void *arg) override { return FALSE;}
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_mul>(thd, this); }
};


class Item_func_div :public Item_num_op
{
public:
  uint prec_increment;
  Item_func_div(THD *thd, Item *a, Item *b): Item_num_op(thd, a, b) {}
  longlong int_op() override { DBUG_ASSERT(0); return 0; }
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("/") };
    return name;
  }
  enum precedence precedence() const override { return MUL_PRECEDENCE; }
  bool fix_length_and_dec() override;
  void fix_length_and_dec_double();
  void fix_length_and_dec_int();
  void result_precision() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_div>(thd, this); }
};


class Item_func_int_div :public Item_int_func
{
public:
  Item_func_int_div(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b)
  {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("DIV") };
    return name;
  }
  enum precedence precedence() const override { return MUL_PRECEDENCE; }
  const Type_handler *type_handler() const override
  { return type_handler_long_or_longlong(); }
  bool fix_length_and_dec() override;
  void print(String *str, enum_query_type query_type) override
  {
    print_op(str, query_type);
  }

  bool check_partition_func_processor(void *int_arg) override {return FALSE;}
  bool check_vcol_func_processor(void *arg) override { return FALSE;}
  bool need_parentheses_in_default() override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_int_div>(thd, this); }
};


class Item_func_mod :public Item_num_op
{
public:
  Item_func_mod(THD *thd, Item *a, Item *b): Item_num_op(thd, a, b) {}
  longlong int_op() override;
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("MOD") };
    return name;
  }
  enum precedence precedence() const override { return MUL_PRECEDENCE; }
  void result_precision() override;
  bool fix_length_and_dec() override;
  void fix_length_and_dec_double()
  {
    Item_num_op::fix_length_and_dec_double();
    unsigned_flag= args[0]->unsigned_flag;
  }
  void fix_length_and_dec_decimal()
  {
    result_precision();
    fix_decimals();
  }
  void fix_length_and_dec_int()
  {
    result_precision();
    DBUG_ASSERT(decimals == 0);
    set_handler(type_handler_long_or_longlong());
  }
  bool check_partition_func_processor(void *int_arg) override {return FALSE;}
  bool check_vcol_func_processor(void *arg) override { return FALSE;}
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_mod>(thd, this); }
};


class Item_func_neg :public Item_func_num1
{
public:
  Item_func_neg(THD *thd, Item *a): Item_func_num1(thd, a) {}
  double real_op() override;
  longlong int_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("-") };
    return name;
  }
  enum Functype functype() const override { return NEG_FUNC; }
  enum precedence precedence() const override { return NEG_PRECEDENCE; }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(func_name_cstring());
    args[0]->print_parenthesised(str, query_type, precedence());
  }
  void fix_length_and_dec_int();
  void fix_length_and_dec_double();
  void fix_length_and_dec_decimal();
  bool fix_length_and_dec() override;
  decimal_digits_t decimal_precision() const  override
  { return args[0]->decimal_precision(); }
  bool need_parentheses_in_default() override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_neg>(thd, this); }
};


class Item_func_abs :public Item_func_num1
{
public:
  Item_func_abs(THD *thd, Item *a): Item_func_num1(thd, a) {}
  double real_op() override;
  longlong int_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("abs") };
    return name;
  }
  void fix_length_and_dec_int();
  void fix_length_and_dec_double();
  void fix_length_and_dec_decimal();
  bool fix_length_and_dec() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_abs>(thd, this); }
};

// A class to handle logarithmic and trigonometric functions

class Item_dec_func :public Item_real_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_real(0, arg_count); }
 public:
  Item_dec_func(THD *thd, Item *a): Item_real_func(thd, a) {}
  Item_dec_func(THD *thd, Item *a, Item *b): Item_real_func(thd, a, b) {}
  bool fix_length_and_dec() override
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(decimals);
    set_maybe_null();
    return FALSE;
  }
};

class Item_func_exp :public Item_dec_func
{
public:
  Item_func_exp(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("exp") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_exp>(thd, this); }
};


class Item_func_ln :public Item_dec_func
{
public:
  Item_func_ln(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ln") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ln>(thd, this); }
};


class Item_func_log :public Item_dec_func
{
public:
  Item_func_log(THD *thd, Item *a): Item_dec_func(thd, a) {}
  Item_func_log(THD *thd, Item *a, Item *b): Item_dec_func(thd, a, b) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("log") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_log>(thd, this); }
};


class Item_func_log2 :public Item_dec_func
{
public:
  Item_func_log2(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("log2") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_log2>(thd, this); }
};


class Item_func_log10 :public Item_dec_func
{
public:
  Item_func_log10(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("log10") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_log10>(thd, this); }
};


class Item_func_sqrt :public Item_dec_func
{
public:
  Item_func_sqrt(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sqrt") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sqrt>(thd, this); }
};


class Item_func_pow :public Item_dec_func
{
public:
  Item_func_pow(THD *thd, Item *a, Item *b): Item_dec_func(thd, a, b) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("pow") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_pow>(thd, this); }
};


class Item_func_acos :public Item_dec_func
{
public:
  Item_func_acos(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("acos") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_acos>(thd, this); }
};

class Item_func_asin :public Item_dec_func
{
public:
  Item_func_asin(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("asin") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_asin>(thd, this); }
};

class Item_func_atan :public Item_dec_func
{
public:
  Item_func_atan(THD *thd, Item *a): Item_dec_func(thd, a) {}
  Item_func_atan(THD *thd, Item *a, Item *b): Item_dec_func(thd, a, b) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("atan") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_atan>(thd, this); }
};

class Item_func_cos :public Item_dec_func
{
public:
  Item_func_cos(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cos") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_cos>(thd, this); }
};

class Item_func_sin :public Item_dec_func
{
public:
  Item_func_sin(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sin") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sin>(thd, this); }
};

class Item_func_tan :public Item_dec_func
{
public:
  Item_func_tan(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("tan") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_tan>(thd, this); }
};

class Item_func_cot :public Item_dec_func
{
public:
  Item_func_cot(THD *thd, Item *a): Item_dec_func(thd, a) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cot") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_cot>(thd, this); }
};


class Item_func_int_val :public Item_func_hybrid_field_type
{
public:
  Item_func_int_val(THD *thd, Item *a): Item_func_hybrid_field_type(thd, a) {}
  bool check_partition_func_processor(void *int_arg) override { return FALSE; }
  bool check_vcol_func_processor(void *arg) override { return FALSE; }
  virtual decimal_round_mode round_mode() const= 0;
  void fix_length_and_dec_double();
  void fix_length_and_dec_int_or_decimal();
  void fix_length_and_dec_time()
  {
    fix_attributes_time(0);
    set_handler(&type_handler_time2);
  }
  void fix_length_and_dec_datetime()
  {
    fix_attributes_datetime(0);
    set_handler(&type_handler_datetime2);
    // Thinks like CEILING(TIMESTAMP'0000-01-01 23:59:59.9') returns NULL
    set_maybe_null();
  }
  bool fix_length_and_dec() override;
  String *str_op(String *str) override { DBUG_ASSERT(0); return 0; }
  bool native_op(THD *thd, Native *to) override
  {
    DBUG_ASSERT(0);
    return true;
  }
};


class Item_func_ceiling :public Item_func_int_val
{
public:
  Item_func_ceiling(THD *thd, Item *a): Item_func_int_val(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ceiling") };
    return name;
  }
  decimal_round_mode round_mode() const override { return CEILING; }
  longlong int_op() override;
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ceiling>(thd, this); }
};


class Item_func_floor :public Item_func_int_val
{
public:
  Item_func_floor(THD *thd, Item *a): Item_func_int_val(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("floor") };
    return name;
  }
  decimal_round_mode round_mode() const override { return FLOOR; }
  longlong int_op() override;
  double real_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_floor>(thd, this); }
};

/* This handles round and truncate */

class Item_func_round :public Item_func_hybrid_field_type
{
  bool truncate;
  void fix_length_and_dec_decimal(uint decimals_to_set);
  void fix_length_and_dec_double(uint decimals_to_set);
  bool test_if_length_can_increase();
public:
  Item_func_round(THD *thd, Item *a, Item *b, bool trunc_arg)
    :Item_func_hybrid_field_type(thd, a, b), truncate(trunc_arg) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING truncate_name= {STRING_WITH_LEN("truncate") };
    static LEX_CSTRING round_name= {STRING_WITH_LEN("round") };
    return truncate ? truncate_name : round_name;
  }
  double real_op() override;
  longlong int_op() override;
  my_decimal *decimal_op(my_decimal *) override;
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  bool native_op(THD *thd, Native *to) override
  {
    DBUG_ASSERT(0);
    return true;
  }
  String *str_op(String *str) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  void fix_arg_decimal();
  void fix_arg_int(const Type_handler *preferred,
                   const Type_std_attributes *preferred_attributes,
                   bool use_decimal_on_length_increase);
  void fix_arg_hex_hybrid();
  void fix_arg_double();
  void fix_arg_time();
  void fix_arg_datetime();
  void fix_arg_temporal(const Type_handler *h, uint int_part_length);
  bool fix_length_and_dec() override
  {
    /*
      We don't want to translate ENUM/SET to CHAR here.
      So let's real_type_handler(), not type_handler().
    */
    return args[0]->real_type_handler()->Item_func_round_fix_length_and_dec(this);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_round>(thd, this); }
};


class Item_func_rand :public Item_real_func
{
  struct my_rnd_struct *rand;
  bool first_eval; // TRUE if val_real() is called 1st time
  bool check_arguments() const override
  { return check_argument_types_can_return_int(0, arg_count); }
  void seed_random (Item * val);
public:
  Item_func_rand(THD *thd, Item *a):
    Item_real_func(thd, a), rand(0), first_eval(TRUE) {}
  Item_func_rand(THD *thd): Item_real_func(thd) {}
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rand") };
    return name;
  }
  bool const_item() const override { return 0; }
  void update_used_tables() override;
  bool fix_fields(THD *thd, Item **ref) override;
  void cleanup() override { first_eval= TRUE; Item_real_func::cleanup(); }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_rand>(thd, this); }
};


class Item_func_rownum final :public Item_longlong_func
{
  /*
    This points to a variable that contains the number of rows
    accpted so far in the result set
  */
  ha_rows *accepted_rows;
  SELECT_LEX *select;
public:
  Item_func_rownum(THD *thd);
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rownum") };
    return name;
  }
  enum Functype functype() const override { return ROWNUM_FUNC; }
  void update_used_tables() override {}
  bool const_item() const override { return 0; }
  void fix_after_optimize(THD *thd) override;
  bool fix_length_and_dec() override
  {
    unsigned_flag= 1;
    used_tables_cache= RAND_TABLE_BIT;
    const_item_cache=0;
    set_maybe_null();
    return FALSE;
  }
  void cleanup() override
  {
    Item_longlong_func::cleanup();
    /* Ensure we don't point to freed memory */
    accepted_rows= 0;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_IMPOSSIBLE);
  }
  bool check_handler_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override { return 0; }
  /* This function is used in insert, update and delete */
  void store_pointer_to_row_counter(ha_rows *row_counter)
  {
    accepted_rows= row_counter;
  }
};

void fix_rownum_pointers(THD *thd, SELECT_LEX *select_lex, ha_rows *ptr);


class Item_func_sign :public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_real(func_name_cstring()); }
public:
  Item_func_sign(THD *thd, Item *a): Item_long_func(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sign") };
    return name;
  }
  decimal_digits_t decimal_precision() const override { return 1; }
  bool fix_length_and_dec() override { fix_char_length(2); return FALSE; }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sign>(thd, this); }
};


class Item_func_units :public Item_real_func
{
  LEX_CSTRING name;
  double mul,add;
  bool check_arguments() const override
  { return check_argument_types_can_return_real(0, arg_count); }
public:
  Item_func_units(THD *thd, char *name_arg, Item *a, double mul_arg,
                  double add_arg):
    Item_real_func(thd, a), mul(mul_arg), add(add_arg)
  {
    name.str= name_arg;
    name.length= strlen(name_arg);
  }
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override { return name; }
  bool fix_length_and_dec() override
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(decimals);
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_units>(thd, this); }
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
protected:
  bool check_arguments() const override
  {
    return false; // Checked by aggregate_for_min_max()
  }
  bool fix_attributes(Item **item, uint nitems);
public:
  Item_func_min_max(THD *thd, List<Item> &list, int cmp_sign_arg):
    Item_hybrid_func(thd, list), cmp_sign(cmp_sign_arg)
  {}
  String *val_str_native(String *str);
  double val_real_native();
  longlong val_int_native();
  my_decimal *val_decimal_native(my_decimal *);
  bool get_date_native(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate);
  bool get_time_native(THD *thd, MYSQL_TIME *res);

  double val_real() override
  {
    DBUG_ASSERT(fixed());
    return Item_func_min_max::type_handler()->
             Item_func_min_max_val_real(this);
  }
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    return Item_func_min_max::type_handler()->
             Item_func_min_max_val_int(this);
  }
  String *val_str(String *str) override
  {
    DBUG_ASSERT(fixed());
    return Item_func_min_max::type_handler()->
             Item_func_min_max_val_str(this, str);
  }
  my_decimal *val_decimal(my_decimal *dec) override
  {
    DBUG_ASSERT(fixed());
    return Item_func_min_max::type_handler()->
             Item_func_min_max_val_decimal(this, dec);
  }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate) override
  {
    DBUG_ASSERT(fixed());
    return Item_func_min_max::type_handler()->
             Item_func_min_max_get_date(thd, this, res, fuzzydate);
  }
  bool val_native(THD *thd, Native *to) override;
  void aggregate_attributes_real(Item **items, uint nitems)
  {
    /*
      Aggregating attributes for the double data type for LEAST/GREATEST
      is almost the same with aggregating for CASE-alike hybrid functions,
      (CASE..THEN, COALESCE, IF, etc).
      There is one notable difference though, when a numeric argument is mixed
      with a string argument:
      - CASE-alike functions return a string data type in such cases
        COALESCE(10,'x') -> VARCHAR(2) = '10'
      - LEAST/GREATEST returns double:
        GREATEST(10,'10e4') -> DOUBLE = 100000
      As the string argument can represent a number in the scientific notation,
      like in the example above, max_length of the result can be longer than
      max_length of the arguments. To handle this properly, max_length is
      additionally assigned to the result of float_length(decimals).
    */
    Item_func::aggregate_attributes_real(items, nitems);
    max_length= float_length(decimals);
  }
  bool fix_length_and_dec() override
  {
    if (aggregate_for_min_max(func_name_cstring(), args, arg_count))
      return true;
    fix_attributes(args, arg_count);
    return false;
  }
};

class Item_func_min :public Item_func_min_max
{
public:
  Item_func_min(THD *thd, List<Item> &list): Item_func_min_max(thd, list, 1) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("least") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_min>(thd, this); }
};

class Item_func_max :public Item_func_min_max
{
public:
  Item_func_max(THD *thd, List<Item> &list): Item_func_min_max(thd, list, -1) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("greatest") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_max>(thd, this); }
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
  }
  double val_real() override { return val_real_from_item(args[0]); }
  longlong val_int() override { return val_int_from_item(args[0]); }
  String *val_str(String *str) override
  { return val_str_from_item(args[0], str); }
  bool val_native(THD *thd, Native *to) override
  { return val_native_from_item(thd, args[0], to); }
  my_decimal *val_decimal(my_decimal *dec) override
    { return val_decimal_from_item(args[0], dec); }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
    { return get_date_from_item(thd, args[0], ltime, fuzzydate); }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("rollup_const") };
    return name;
  }
  bool const_item() const override { return 0; }
  const Type_handler *type_handler() const override
  { return args[0]->type_handler(); }
  bool fix_length_and_dec() override
  {
    Type_std_attributes::set(*args[0]);
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_rollup_const>(thd, this); }
};


class Item_long_func_length: public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_str(func_name_cstring()); }
public:
  Item_long_func_length(THD *thd, Item *a): Item_long_func(thd, a) {}
  bool fix_length_and_dec() override { max_length=10; return FALSE; }
};


class Item_func_octet_length :public Item_long_func_length
{
  String value;
public:
  Item_func_octet_length(THD *thd, Item *a): Item_long_func_length(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("octet_length") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_octet_length>(thd, this); }
};

class Item_func_bit_length :public Item_longlong_func
{
  String value;
public:
  Item_func_bit_length(THD *thd, Item *a): Item_longlong_func(thd, a) {}
  bool fix_length_and_dec() override
  {
    max_length= 11; // 0x100000000*8 = 34,359,738,368
    return FALSE;
  }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("bit_length") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_bit_length>(thd, this); }
};

class Item_func_char_length :public Item_long_func_length
{
  String value;
public:
  Item_func_char_length(THD *thd, Item *a): Item_long_func_length(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("char_length") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_char_length>(thd, this); }
};

class Item_func_coercibility :public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_str(func_name_cstring()); }
public:
  Item_func_coercibility(THD *thd, Item *a): Item_long_func(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("coercibility") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    max_length=10;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return FALSE;
  }
  bool eval_not_null_tables(void *) override
  {
    not_null_tables_cache= 0;
    return false;
  }
  bool find_not_null_fields(table_map allowed) override
  {
    return false;
  }
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  { return this; }
  bool const_item() const override { return true; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_coercibility>(thd, this); }
};


/*
  In the corner case LOCATE could return (4,294,967,296 + 1),
  which would not fit into Item_long_func range.
  But string lengths are limited with max_allowed_packet,
  which cannot be bigger than 1024*1024*1024.
*/
class Item_func_locate :public Item_long_func
{
  bool check_arguments() const override
  {
    return check_argument_types_can_return_str(0, 2) ||
           (arg_count > 2 && args[2]->check_type_can_return_int(func_name_cstring()));
  }
  String value1,value2;
  DTCollation cmp_collation;
public:
  Item_func_locate(THD *thd, Item *a, Item *b)
   :Item_long_func(thd, a, b) {}
  Item_func_locate(THD *thd, Item *a, Item *b, Item *c)
   :Item_long_func(thd, a, b, c) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("locate") };
    return name;
  }
  longlong val_int() override;
  bool fix_length_and_dec() override
  {
    max_length= MY_INT32_NUM_DECIMAL_DIGITS;
    return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
  }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_locate>(thd, this); }
};


class Item_func_field :public Item_long_func
{
  String value,tmp;
  Item_result cmp_type;
  DTCollation cmp_collation;
public:
  Item_func_field(THD *thd, List<Item> &list): Item_long_func(thd, list) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("field") };
    return name;
  }
  bool fix_length_and_dec() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_field>(thd, this); }
};


class Item_func_ascii :public Item_long_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_str(0, arg_count); }
  String value;
public:
  Item_func_ascii(THD *thd, Item *a): Item_long_func(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ascii") };
    return name;
  }
  bool fix_length_and_dec() override { max_length=3; return FALSE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ascii>(thd, this); }
};

class Item_func_ord :public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_str(func_name_cstring()); }
  String value;
public:
  Item_func_ord(THD *thd, Item *a): Item_long_func(thd, a) {}
  bool fix_length_and_dec() override { fix_char_length(7); return FALSE; }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ord") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ord>(thd, this); }
};

class Item_func_find_in_set :public Item_long_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_str(0, 2); }
  String value,value2;
  uint enum_value;
  ulonglong enum_bit;
  DTCollation cmp_collation;
public:
  Item_func_find_in_set(THD *thd, Item *a, Item *b):
    Item_long_func(thd, a, b), enum_value(0) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("find_in_set") };
    return name;
  }
  bool fix_length_and_dec() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_find_in_set>(thd, this); }
};

/* Base class for all bit functions: '~', '|', '^', '&', '>>', '<<' */

class Item_func_bit_operator: public Item_handled_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_int(0, arg_count); }
protected:
  bool fix_length_and_dec_op1_std(const Handler *ha_int, const Handler *ha_dec)
  {
    set_func_handler(args[0]->cmp_type() == INT_RESULT ? ha_int : ha_dec);
    return m_func_handler->fix_length_and_dec(this);
  }
  bool fix_length_and_dec_op2_std(const Handler *ha_int, const Handler *ha_dec)
  {
    set_func_handler(args[0]->cmp_type() == INT_RESULT &&
                     args[1]->cmp_type() == INT_RESULT ? ha_int : ha_dec);
    return m_func_handler->fix_length_and_dec(this);
  }
public:
  Item_func_bit_operator(THD *thd, Item *a)
   :Item_handled_func(thd, a) {}
  Item_func_bit_operator(THD *thd, Item *a, Item *b)
   :Item_handled_func(thd, a, b) {}
  void print(String *str, enum_query_type query_type) override
  {
    print_op(str, query_type);
  }
  bool need_parentheses_in_default() override { return true; }
};

class Item_func_bit_or :public Item_func_bit_operator
{
public:
  Item_func_bit_or(THD *thd, Item *a, Item *b)
   :Item_func_bit_operator(thd, a, b) {}
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("|") };
    return name;
  }
  enum precedence precedence() const override { return BITOR_PRECEDENCE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_bit_or>(thd, this); }
};

class Item_func_bit_and :public Item_func_bit_operator
{
public:
  Item_func_bit_and(THD *thd, Item *a, Item *b)
   :Item_func_bit_operator(thd, a, b) {}
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("&") };
    return name;
  }
  enum precedence precedence() const override { return BITAND_PRECEDENCE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_bit_and>(thd, this); }
};

class Item_func_bit_count :public Item_handled_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_int(func_name_cstring()); }
public:
  Item_func_bit_count(THD *thd, Item *a): Item_handled_func(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("bit_count") };
    return name;
  }
  bool fix_length_and_dec() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_bit_count>(thd, this); }
};

class Item_func_shift_left :public Item_func_bit_operator
{
public:
  Item_func_shift_left(THD *thd, Item *a, Item *b)
   :Item_func_bit_operator(thd, a, b) {}
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<<") };
    return name;
  }
  enum precedence precedence() const override { return SHIFT_PRECEDENCE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_shift_left>(thd, this); }
};

class Item_func_shift_right :public Item_func_bit_operator
{
public:
  Item_func_shift_right(THD *thd, Item *a, Item *b)
   :Item_func_bit_operator(thd, a, b) {}
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN(">>") };
    return name;
  }
  enum precedence precedence() const override { return SHIFT_PRECEDENCE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_shift_right>(thd, this); }
};

class Item_func_bit_neg :public Item_func_bit_operator
{
public:
  Item_func_bit_neg(THD *thd, Item *a): Item_func_bit_operator(thd, a) {}
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("~") };
    return name;
  }
  enum precedence precedence() const override { return NEG_PRECEDENCE; }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(func_name_cstring());
    args[0]->print_parenthesised(str, query_type, precedence());
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_bit_neg>(thd, this); }
};


class Item_func_last_insert_id :public Item_longlong_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_int(0, arg_count); }
public:
  Item_func_last_insert_id(THD *thd): Item_longlong_func(thd) {}
  Item_func_last_insert_id(THD *thd, Item *a): Item_longlong_func(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("last_insert_id") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    unsigned_flag= true;
    if (arg_count)
      max_length= args[0]->max_length;
    return FALSE;
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_last_insert_id>(thd, this); }
};


class Item_func_benchmark :public Item_long_func
{
  bool check_arguments() const override
  {
    return args[0]->check_type_can_return_int(func_name_cstring()) ||
           args[1]->check_type_scalar(func_name_cstring());
  }
public:
  Item_func_benchmark(THD *thd, Item *count_expr, Item *expr):
    Item_long_func(thd, count_expr, expr)
  {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("benchmark") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    max_length=1;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return FALSE;
  }
  void print(String *str, enum_query_type query_type) override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_benchmark>(thd, this); }
};


void item_func_sleep_init(void);
void item_func_sleep_free(void);

class Item_func_sleep :public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_real(func_name_cstring()); }
public:
  Item_func_sleep(THD *thd, Item *a): Item_long_func(thd, a) {}
  bool fix_length_and_dec() override { fix_char_length(1); return FALSE; }
  bool const_item() const override { return 0; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sleep") };
    return name;
  }
  table_map used_tables() const override
  {
    return used_tables_cache | RAND_TABLE_BIT;
  }
  bool is_expensive() override { return 1; }
  longlong val_int() override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sleep>(thd, this); }
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
  bool is_expensive_processor(void *arg) override { return TRUE; }

  class VDec_udf: public Dec_ptr_and_buffer
  {
  public:
    VDec_udf(Item_udf_func *func, udf_handler *udf)
    {
      my_bool tmp_null_value;
      m_ptr= udf->val_decimal(&tmp_null_value, &m_buffer);
      DBUG_ASSERT(is_null() == (tmp_null_value != 0));
      func->null_value= is_null();
    }
  };

public:
  Item_udf_func(THD *thd, udf_func *udf_arg):
    Item_func(thd), udf(udf_arg) {}
  Item_udf_func(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_func(thd, list), udf(udf_arg) {}
  LEX_CSTRING func_name_cstring() const override
  {
    const char *tmp= udf.name();
    return { tmp, strlen(tmp) };
  }
  enum Functype functype() const override { return UDF_FUNC; }
  bool fix_fields(THD *thd, Item **ref) override
  {
    DBUG_ASSERT(fixed() == 0);
    bool res= udf.fix_fields(thd, this, arg_count, args);
    set_non_deterministic_if_needed();
    base_flags|= item_base_t::FIXED;
    return res;
  }
  void fix_num_length_and_dec();
  void update_used_tables() override
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
  void cleanup() override;
  bool eval_not_null_tables(void *opt_arg) override
  {
    not_null_tables_cache= 0;
    return 0;
  }
  bool find_not_null_fields(table_map allowed) override
  {
    return false;
  }
  bool is_expensive() override { return 1; }
  void print(String *str, enum_query_type query_type) override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_NON_DETERMINISTIC);
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override
  { return false; }
};


class Item_func_udf_float :public Item_udf_func
{
 public:
  Item_func_udf_float(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_float(THD *thd, udf_func *udf_arg,
                      List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    return Converter_double_to_longlong(Item_func_udf_float::val_real(),
                                        unsigned_flag).result();
  }
  my_decimal *val_decimal(my_decimal *dec_buf) override
  {
    double res=val_real();
    if (null_value)
      return NULL;
    double2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf);
    return dec_buf;
  }
  double val_real() override;
  String *val_str(String *str) override;
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  bool fix_length_and_dec() override { fix_num_length_and_dec(); return FALSE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_udf_float>(thd, this); }
};


class Item_func_udf_int :public Item_udf_func
{
public:
  Item_func_udf_int(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_int(THD *thd, udf_func *udf_arg,
                    List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  longlong val_int() override;
  double val_real() override { return (double) Item_func_udf_int::val_int(); }
  my_decimal *val_decimal(my_decimal *decimal_value) override
  {
    return val_decimal_from_int(decimal_value);
  }
  String *val_str(String *str) override;
  const Type_handler *type_handler() const override
  {
    if (unsigned_flag)
      return &type_handler_ulonglong;
    return &type_handler_slonglong;
  }
  bool fix_length_and_dec() override { decimals= 0; max_length= 21; return FALSE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_udf_int>(thd, this); }
};


class Item_func_udf_decimal :public Item_udf_func
{
public:
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  longlong val_int() override
  {
    return VDec_udf(this, &udf).to_longlong(unsigned_flag);
  }
  double val_real() override
  {
    return VDec_udf(this, &udf).to_double();
  }
  my_decimal *val_decimal(my_decimal *) override;
  String *val_str(String *str) override
  {
    return VDec_udf(this, &udf).to_string_round(str, decimals);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
  bool fix_length_and_dec() override { fix_num_length_and_dec(); return FALSE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_udf_decimal>(thd, this); }
};


class Item_func_udf_str :public Item_udf_func
{
public:
  Item_func_udf_str(THD *thd, udf_func *udf_arg):
    Item_udf_func(thd, udf_arg) {}
  Item_func_udf_str(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_udf_func(thd, udf_arg, list) {}
  String *val_str(String *) override;
  double val_real() override
  {
    int err_not_used;
    char *end_not_used;
    String *res;
    res= val_str(&str_value);
    return res ? res->charset()->strntod((char*) res->ptr(), res->length(),
                                         &end_not_used, &err_not_used) : 0.0;
  }
  longlong val_int() override
  {
    int err_not_used;
    String *res;  res=val_str(&str_value);
    return res ? res->charset()->strntoll(res->ptr(),res->length(),10,
                                          (char**) 0, &err_not_used) : (longlong) 0;
  }
  my_decimal *val_decimal(my_decimal *dec_buf) override
  {
    String *res=val_str(&str_value);
    if (!res)
      return NULL;
    string2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf);
    return dec_buf;
  }
  const Type_handler *type_handler() const override
  { return string_type_handler(); }
  bool fix_length_and_dec() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_udf_str>(thd, this); }
};

#else /* Dummy functions to get yy_*.cc files compiled */

class Item_func_udf_float :public Item_real_func
{
 public:
  Item_func_udf_float(THD *thd, udf_func *udf_arg):
    Item_real_func(thd) {}
  Item_func_udf_float(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_real_func(thd, list) {}
  double val_real() { DBUG_ASSERT(fixed()); return 0.0; }
};


class Item_func_udf_int :public Item_int_func
{
public:
  Item_func_udf_int(THD *thd, udf_func *udf_arg):
    Item_int_func(thd) {}
  Item_func_udf_int(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_int_func(thd, list) {}
  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }
  longlong val_int() { DBUG_ASSERT(fixed()); return 0; }
};


class Item_func_udf_decimal :public Item_int_func
{
public:
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg):
    Item_int_func(thd) {}
  Item_func_udf_decimal(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_int_func(thd, list) {}
  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }
  my_decimal *val_decimal(my_decimal *) { DBUG_ASSERT(fixed()); return 0; }
};


class Item_func_udf_str :public Item_func
{
public:
  Item_func_udf_str(THD *thd, udf_func *udf_arg):
    Item_func(thd) {}
  Item_func_udf_str(THD *thd, udf_func *udf_arg, List<Item> &list):
    Item_func(thd, list) {}
  String *val_str(String *)
    { DBUG_ASSERT(fixed()); null_value=1; return 0; }
  double val_real() { DBUG_ASSERT(fixed()); null_value= 1; return 0.0; }
  longlong val_int() { DBUG_ASSERT(fixed()); null_value=1; return 0; }
  bool fix_length_and_dec() override
  { base_flags|= item_base_t::MAYBE_NULL; max_length=0; return FALSE; }
};

#endif /* HAVE_DLOPEN */

void mysql_ull_cleanup(THD *thd);
void mysql_ull_set_explicit_lock_duration(THD *thd);


class Item_func_lock :public Item_long_func
{
 public:
  Item_func_lock(THD *thd): Item_long_func(thd) { }
  Item_func_lock(THD *thd, Item *a): Item_long_func(thd, a) {}
  Item_func_lock(THD *thd, Item *a, Item *b): Item_long_func(thd, a, b) {}
  table_map used_tables() const override
  {
    return used_tables_cache | RAND_TABLE_BIT;
  }
  bool const_item() const override { return 0; }
  bool is_expensive() override { return 1; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
};


class Item_func_get_lock final :public Item_func_lock
{
  bool check_arguments() const override
  {
    return args[0]->check_type_general_purpose_string(func_name_cstring()) ||
           args[1]->check_type_can_return_real(func_name_cstring());
  }
  String value;
 public:
  Item_func_get_lock(THD *thd, Item *a, Item *b) :Item_func_lock(thd, a, b) {}
  longlong val_int() final;
  LEX_CSTRING func_name_cstring() const override final
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("get_lock") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    max_length= 1;
    set_maybe_null();
    return FALSE;
  }
  Item *get_copy(THD *thd) final
  { return get_item_copy<Item_func_get_lock>(thd, this); }
};


class Item_func_release_all_locks final :public Item_func_lock
{
public:
  Item_func_release_all_locks(THD *thd): Item_func_lock(thd)
  { unsigned_flag= 1; }
  longlong val_int() final;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("release_all_locks") };
    return name;
  }
  Item *get_copy(THD *thd) final
  { return get_item_copy<Item_func_release_all_locks>(thd, this); }
};


class Item_func_release_lock final :public Item_func_lock
{
  bool check_arguments() const override
  { return args[0]->check_type_general_purpose_string(func_name_cstring()); }
  String value;
public:
  Item_func_release_lock(THD *thd, Item *a): Item_func_lock(thd, a) {}
  longlong val_int() final;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("release_lock") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    max_length= 1;
    set_maybe_null();
    return FALSE;
  }
  Item *get_copy(THD *thd) final
  { return get_item_copy<Item_func_release_lock>(thd, this); }
};


/* replication functions */

class Item_master_pos_wait :public Item_longlong_func
{
  bool check_arguments() const override
  {
    return
      args[0]->check_type_general_purpose_string(func_name_cstring()) ||
      args[1]->check_type_can_return_int(func_name_cstring()) ||
      (arg_count > 2 && args[2]->check_type_can_return_int(func_name_cstring())) ||
      (arg_count > 3 && args[3]->check_type_general_purpose_string(func_name_cstring()));
  }
  String value;
public:
  Item_master_pos_wait(THD *thd, Item *a, Item *b)
   :Item_longlong_func(thd, a, b) {}
  Item_master_pos_wait(THD *thd, Item *a, Item *b, Item *c):
    Item_longlong_func(thd, a, b, c) {}
  Item_master_pos_wait(THD *thd, Item *a, Item *b, Item *c, Item *d):
    Item_longlong_func(thd, a, b, c, d) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("master_pos_wait") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    max_length=21;
    set_maybe_null();
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_master_pos_wait>(thd, this); }
};


class Item_master_gtid_wait :public Item_long_func
{
  bool check_arguments() const override
  {
    return args[0]->check_type_general_purpose_string(func_name_cstring()) ||
      (arg_count > 1 && args[1]->check_type_can_return_real(func_name_cstring()));
  }
  String value;
public:
  Item_master_gtid_wait(THD *thd, Item *a)
   :Item_long_func(thd, a) {}
  Item_master_gtid_wait(THD *thd, Item *a, Item *b)
   :Item_long_func(thd, a, b) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("master_gtid_wait") };
    return name;
  }
  bool fix_length_and_dec() override { max_length=2; return FALSE; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_master_gtid_wait>(thd, this); }
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
  LEX_CSTRING name; // keep it public
  Item_func_user_var(THD *thd, const LEX_CSTRING *a)
    :Item_hybrid_func(thd), m_var_entry(NULL), name(*a) { }
  Item_func_user_var(THD *thd, const LEX_CSTRING *a, Item *b)
    :Item_hybrid_func(thd, b), m_var_entry(NULL), name(*a) { }
  Item_func_user_var(THD *thd, Item_func_user_var *item)
    :Item_hybrid_func(thd, item),
    m_var_entry(item->m_var_entry), name(item->name) { }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param)
  {
    DBUG_ASSERT(fixed());
    return create_tmp_field_ex_from_handler(root, table, src, param,
                                            type_handler());
  }
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table)
  { return create_table_field_from_handler(root, table); }
  bool check_vcol_func_processor(void *arg);
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
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
  Item_func_set_user_var(THD *thd, const LEX_CSTRING *a, Item *b):
    Item_func_user_var(thd, a, b),
    entry_thread_id(0)
  {}
  Item_func_set_user_var(THD *thd, Item_func_set_user_var *item)
    :Item_func_user_var(thd, item),
    entry_thread_id(item->entry_thread_id),
    value(item->value), decimal_buff(item->decimal_buff),
    null_item(item->null_item), save_result(item->save_result)
  {}

  enum Functype functype() const override { return SUSERVAR_FUNC; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *str) override;
  my_decimal *val_decimal(my_decimal *) override;
  double val_result() override;
  longlong val_int_result() override;
  bool val_bool_result() override;
  String *str_result(String *str) override;
  my_decimal *val_decimal_result(my_decimal *) override;
  bool is_null_result() override;
  bool update_hash(void *ptr, size_t length, enum Item_result type,
                   CHARSET_INFO *cs, bool unsigned_arg);
  bool send(Protocol *protocol, st_value *buffer) override;
  void make_send_field(THD *thd, Send_field *tmp_field) override;
  bool check(bool use_result_field);
  void save_item_result(Item *item);
  bool update();
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec() override;
  void print(String *str, enum_query_type query_type) override;
  enum precedence precedence() const override { return ASSIGN_PRECEDENCE; }
  void print_as_stmt(String *str, enum_query_type query_type);
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("set_user_var") };
    return name;
  }
  int save_in_field(Field *field, bool no_conversions,
                    bool can_use_result_field);
  int save_in_field(Field *field, bool no_conversions) override
  {
    return save_in_field(field, no_conversions, 1);
  }
  void save_org_in_field(Field *field,
                         fast_field_copier data __attribute__ ((__unused__)))
    override
  { (void) save_in_field(field, 1, 0); }
  bool register_field_in_read_map(void *arg) override;
  bool register_field_in_bitmap(void *arg) override;
  bool set_entry(THD *thd, bool create_if_not_exists);
  void cleanup() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_set_user_var>(thd, this); }
  bool excl_dep_on_table(table_map tab_map) override { return false; }
};


class Item_func_get_user_var :public Item_func_user_var,
                              private Settable_routine_parameter
{
public:
  Item_func_get_user_var(THD *thd, const LEX_CSTRING *a):
    Item_func_user_var(thd, a) {}
  enum Functype functype() const override { return GUSERVAR_FUNC; }
  LEX_CSTRING get_name() { return name; }
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal*) override;
  String *val_str(String* str) override;
  bool fix_length_and_dec() override;
  void print(String *str, enum_query_type query_type) override;
  /*
    We must always return variables as strings to guard against selects of type
    select @t1:=1,@t1,@t:="hello",@t from foo where (@t1:= t2.b)
  */
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("get_user_var") };
    return name;
  }
  bool const_item() const override;
  table_map used_tables() const override
  { return const_item() ? 0 : RAND_TABLE_BIT; }
  bool eq(const Item *item, bool binary_cmp) const override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_get_user_var>(thd, this); }
private:
  bool set_value(THD *thd, sp_rcontext *ctx, Item **it) override;

public:
  Settable_routine_parameter *get_settable_routine_parameter() override
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
class Item_user_var_as_out_param :public Item_fixed_hybrid,
                                  public Load_data_outvar
{
  LEX_CSTRING org_name;
  user_var_entry *entry;
public:
  Item_user_var_as_out_param(THD *thd, const LEX_CSTRING *a)
  :Item_fixed_hybrid(thd)
  {
    DBUG_ASSERT(a->length < UINT_MAX32);
    org_name= *a;
    set_name(thd, a->str, a->length, system_charset_info);
  }
  Load_data_outvar *get_load_data_outvar() override
  {
    return this;
  }
  bool load_data_set_null(THD *thd, const Load_data_param *param) override
  {
    set_null_value(param->charset());
    return false;
  }
  bool load_data_set_no_data(THD *thd, const Load_data_param *param) override
  {
    set_null_value(param->charset());
    return false;
  }
  bool load_data_set_value(THD *thd, const char *pos, uint length,
                           const Load_data_param *param) override
  {
    set_value(pos, length, param->charset());
    return false;
  }
  void load_data_print_for_log_event(THD *thd, String *to) const override;
  bool load_data_add_outvar(THD *thd, Load_data_param *param) const override
  {
    return param->add_outvar_user_var(thd);
  }
  uint load_data_fixed_length() const override
  {
    return 0;
  }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  /* We should return something different from FIELD_ITEM here */
  enum Type type() const override { return CONST_ITEM;}
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *str) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  my_decimal *val_decimal(my_decimal *decimal_buffer) override;
  /* fix_fields() binds variable name with its entry structure */
  bool fix_fields(THD *thd, Item **ref) override;
  void set_null_value(CHARSET_INFO* cs);
  void set_value(const char *str, uint length, CHARSET_INFO* cs);
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_user_var_as_out_param>(thd, this); }
};


/* A system variable */

#define GET_SYS_VAR_CACHE_LONG     1
#define GET_SYS_VAR_CACHE_DOUBLE   2
#define GET_SYS_VAR_CACHE_STRING   4

class Item_func_get_system_var :public Item_func
{
  sys_var *var;
  enum_var_type var_type, orig_var_type;
  LEX_CSTRING component;
  longlong cached_llval;
  double cached_dval;
  String cached_strval;
  bool cached_null_value;
  query_id_t used_query_id;
  uchar cache_present;

public:
  Item_func_get_system_var(THD *thd, sys_var *var_arg,
                           enum_var_type var_type_arg,
                           LEX_CSTRING *component_arg, const char *name_arg,
                           size_t name_len_arg);
  enum Functype functype() const override { return GSYSVAR_FUNC; }
  void update_null_value() override;
  bool fix_length_and_dec() override;
  void print(String *str, enum_query_type query_type) override;
  bool const_item() const override { return true; }
  table_map used_tables() const override { return 0; }
  const Type_handler *type_handler() const override;
  double val_real() override;
  longlong val_int() override;
  String* val_str(String*) override;
  my_decimal *val_decimal(my_decimal *dec_buf) override
  { return val_decimal_from_real(dec_buf); }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return type_handler()->Item_get_date_with_warn(thd, this, ltime, fuzzydate);
  }
  /* TODO: fix to support views */
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("get_system_var") };
    return name;
  }
  /**
    Indicates whether this system variable is written to the binlog or not.

    Variables are written to the binlog as part of "status_vars" in
    Query_log_event, as an Intvar_log_event, or a Rand_log_event.

    @return true if the variable is written to the binlog, false otherwise.
  */
  bool is_written_to_binlog();
  bool eq(const Item *item, bool binary_cmp) const override;

  void cleanup() override;
  bool check_vcol_func_processor(void *arg) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_get_system_var>(thd, this); }
};


/* for fulltext search */

class Item_func_match :public Item_real_func
{
public:
  uint key, match_flags;
  bool join_key;
  DTCollation cmp_collation;
  FT_INFO *ft_handler;
  TABLE *table;
  Item_func_match *master;   // for master-slave optimization
  Item *concat_ws;           // Item_func_concat_ws
  String value;              // value of concat_ws
  String search_value;       // key_item()'s value converted to cmp_collation

  Item_func_match(THD *thd, List<Item> &a, uint b):
    Item_real_func(thd, a), key(0), match_flags(b), join_key(0), ft_handler(0),
    table(0), master(0), concat_ws(0) { }
  void cleanup() override
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
  bool is_expensive_processor(void *arg) override { return TRUE; }
  enum Functype functype() const override { return FT_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("match") };
    return name;
  }
  bool eval_not_null_tables(void *opt_arg) override
  {
    not_null_tables_cache= 0;
    return 0;
  }
  bool find_not_null_fields(table_map allowed) override
  {
    return false;
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool eq(const Item *, bool binary_cmp) const override;
  /* The following should be safe, even if we compare doubles */
  longlong val_int() override { DBUG_ASSERT(fixed()); return val_real() != 0.0; }
  double val_real() override;
  void print(String *str, enum_query_type query_type) override;

  bool fix_index();
  bool init_search(THD *thd, bool no_order);
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function("match ... against()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_match>(thd, this); }
  Item *build_clone(THD *thd) override { return 0; }
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
    if (!(match_flags & FT_BOOL))
      return false;

    DBUG_ASSERT(table_arg && table_arg->file);

    // Assume that if extended fulltext API is not supported,
    // non-indexed columns are allowed.  This will be true for MyISAM.
    if ((table_arg->file->ha_table_flags() & HA_CAN_FULLTEXT_EXT) == 0)
      return true;

    return false;
  }
};


class Item_func_bit_xor : public Item_func_bit_operator
{
public:
  Item_func_bit_xor(THD *thd, Item *a, Item *b)
   :Item_func_bit_operator(thd, a, b) {}
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("^") };
    return name;
  }
  enum precedence precedence() const override { return BITXOR_PRECEDENCE; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_bit_xor>(thd, this); }
};

class Item_func_is_free_lock :public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_general_purpose_string(func_name_cstring()); }
  String value;
public:
  Item_func_is_free_lock(THD *thd, Item *a): Item_long_func(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("is_free_lock") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    decimals=0;
    max_length=1;
    set_maybe_null();
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_is_free_lock>(thd, this); }
};

class Item_func_is_used_lock :public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_general_purpose_string(func_name_cstring()); }
  String value;
public:
  Item_func_is_used_lock(THD *thd, Item *a): Item_long_func(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("is_used_lock") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    decimals=0; max_length=10;
    set_maybe_null();
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_is_used_lock>(thd, this); }
};


struct Lex_cast_type_st: public Lex_length_and_dec_st
{
private:
  const Type_handler *m_type_handler;
public:
  void set(const Type_handler *handler, const char *length, const char *dec)
  {
    m_type_handler= handler;
    Lex_length_and_dec_st::set(length, dec);
  }
  void set(const Type_handler *handler, Lex_length_and_dec_st length_and_dec)
  {
    m_type_handler= handler;
    Lex_length_and_dec_st::operator=(length_and_dec);
  }
  void set(const Type_handler *handler, const char *length)
  {
    set(handler, length, 0);
  }
  void set(const Type_handler *handler)
  {
    set(handler, 0, 0);
  }
  const Type_handler *type_handler() const { return m_type_handler; }
  Item *create_typecast_item(THD *thd, Item *item,
                             CHARSET_INFO *cs= NULL) const
  {
    return m_type_handler->
      create_typecast_item(thd, item,
                           Type_cast_attributes(length(), dec(), cs));
  }
  Item *create_typecast_item_or_error(THD *thd, Item *item,
                                      CHARSET_INFO *cs= NULL) const;
};


class Item_func_row_count :public Item_longlong_func
{
public:
  Item_func_row_count(THD *thd): Item_longlong_func(thd) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("row_count") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    decimals= 0;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_row_count>(thd, this); }
};


/*
 *
 * Stored FUNCTIONs
 *
 */

class Item_func_sp :public Item_func,
                    public Item_sp
{
private:
  const Sp_handler *m_handler;

  bool execute();

protected:
  bool is_expensive_processor(void *arg) override
  { return is_expensive(); }

  bool check_arguments() const override
  {
    // sp_prepare_func_item() checks that the number of columns is correct
    return false;
  } 
public:

  Item_func_sp(THD *thd, Name_resolution_context *context_arg,
               sp_name *name, const Sp_handler *sph);

  Item_func_sp(THD *thd, Name_resolution_context *context_arg,
               sp_name *name, const Sp_handler *sph, List<Item> &list);

  virtual ~Item_func_sp()
  {}

  void update_used_tables() override;

  void cleanup() override;

  LEX_CSTRING func_name_cstring() const override;

  const Type_handler *type_handler() const override;

  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override;
  Field *create_field_for_create_select(MEM_ROOT *root, TABLE *table) override
  {
    return result_type() != STRING_RESULT ?
           sp_result_field :
           create_table_field_from_handler(root, table);
  }
  void make_send_field(THD *thd, Send_field *tmp_field) override;

  longlong val_int() override
  {
    if (execute())
      return (longlong) 0;
    return sp_result_field->val_int();
  }

  double val_real() override
  {
    if (execute())
      return 0.0;
    return sp_result_field->val_real();
  }

  my_decimal *val_decimal(my_decimal *dec_buf) override
  {
    if (execute())
      return NULL;
    return sp_result_field->val_decimal(dec_buf);
  }

  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    if (execute())
      return true;
    return sp_result_field->get_date(ltime, fuzzydate);
  }

  String *val_str(String *str) override
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

  bool val_native(THD *thd, Native *to) override
  {
    if (execute())
      return true;
    return (null_value= sp_result_field->val_native(to));
  }

  void update_null_value() override
  { 
    execute();
  }

  bool change_context_processor(void *cntx) override
  { context= (Name_resolution_context *)cntx; return FALSE; }

  enum Functype functype() const override { return FUNC_SP; }

  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(void) override;
  bool is_expensive() override;

  inline Field *get_sp_result_field()
  {
    return sp_result_field;
  }
  const sp_name *get_sp_name() const
  {
    return m_name;
  }

  bool check_vcol_func_processor(void *arg) override;
  bool limit_index_condition_pushdown_processor(void *opt_arg) override
  {
    return TRUE;
  }
  Item *get_copy(THD *) override { return 0; }
  bool eval_not_null_tables(void *opt_arg) override
  {
    not_null_tables_cache= 0;
    return 0;
  }
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override
  { return false; }
  bool find_not_null_fields(table_map allowed) override
  {
    return false;
  }
};


class Item_func_found_rows :public Item_longlong_func
{
public:
  Item_func_found_rows(THD *thd): Item_longlong_func(thd) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("found_rows") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    decimals= 0;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return FALSE;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_found_rows>(thd, this); }
};


class Item_func_oracle_sql_rowcount :public Item_longlong_func
{
public:
  Item_func_oracle_sql_rowcount(THD *thd): Item_longlong_func(thd) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("SQL%ROWCOUNT") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(func_name_cstring());
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_oracle_sql_rowcount>(thd, this); }
};


class Item_func_sqlcode: public Item_long_func
{
public:
  Item_func_sqlcode(THD *thd): Item_long_func(thd) { }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("SQLCODE") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(func_name_cstring());
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
  }
  bool fix_length_and_dec() override
  {
    base_flags&= ~item_base_t::MAYBE_NULL;
    null_value= false;
    max_length= 11;
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sqlcode>(thd, this); }
};


void uuid_short_init();
ulonglong server_uuid_value();

class Item_func_uuid_short :public Item_longlong_func
{
public:
  Item_func_uuid_short(THD *thd): Item_longlong_func(thd) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uuid_short") };
    return name;
  }
  longlong val_int() override;
  bool const_item() const override { return false; }
  bool fix_length_and_dec() override
  { max_length= 21; unsigned_flag=1; return FALSE; }
  table_map used_tables() const override { return RAND_TABLE_BIT; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_uuid_short>(thd, this); }
};


class Item_func_last_value :public Item_func
{
protected:
  Item *last_value;
public:
  Item_func_last_value(THD *thd, List<Item> &list): Item_func(thd, list) {}
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool val_native(THD *thd, Native *) override;
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("last_value") };
    return name;
  }
  const Type_handler *type_handler() const override
  { return last_value->type_handler(); }
  bool eval_not_null_tables(void *) override
  {
    not_null_tables_cache= 0;
    return 0;
  }
  bool find_not_null_fields(table_map allowed) override
  {
    return false;
  }
  bool const_item() const override { return 0; }
  void evaluate_sideeffects();
  void update_used_tables() override
  {
    Item_func::update_used_tables();
    copy_flags(last_value, item_base_t::MAYBE_NULL);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_last_value>(thd, this); }
};


/* Implementation for sequences: NEXT VALUE FOR sequence and NEXTVAL() */

class Item_func_nextval :public Item_longlong_func
{
protected:
  TABLE_LIST *table_list;
  TABLE *table;
public:
  Item_func_nextval(THD *thd, TABLE_LIST *table_list_arg):
  Item_longlong_func(thd), table_list(table_list_arg) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("nextval") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    unsigned_flag= 0;
    max_length= MAX_BIGINT_WIDTH;
    set_maybe_null();             /* In case of errors */
    return FALSE;
  }
  /*
    update_table() function must be called during the value function
    as in case of DEFAULT the sequence table may not yet be open
    while fix_fields() are called
  */
  void update_table()
  {
    if (!(table= table_list->table))
    {
      /*
        If nextval was used in DEFAULT then next_local points to
        the table_list used by to open the sequence table
      */
      table= table_list->next_local->table;
    }
  }
  bool const_item() const override { return 0; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_nextval>(thd, this); }
  void print(String *str, enum_query_type query_type) override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     (VCOL_NON_DETERMINISTIC |
                                      VCOL_NOT_VIRTUAL));
  }
};


/* Implementation for sequences: LASTVAL(sequence), PostgreSQL style */

class Item_func_lastval :public Item_func_nextval
{
public:
  Item_func_lastval(THD *thd, TABLE_LIST *table_list_arg):
  Item_func_nextval(thd, table_list_arg) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("lastval") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_lastval>(thd, this); }
};


/* Implementation for sequences: SETVAL(sequence), PostgreSQL style */

class Item_func_setval :public Item_func_nextval
{
  longlong nextval;
  ulonglong round;
  bool is_used;
public:
  Item_func_setval(THD *thd, TABLE_LIST *table_list_arg, longlong nextval_arg,
                   ulonglong round_arg, bool is_used_arg)
    : Item_func_nextval(thd, table_list_arg),
    nextval(nextval_arg), round(round_arg), is_used(is_used_arg)
  {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("setval") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_setval>(thd, this); }
};


Item *get_system_var(THD *thd, enum_var_type var_type,
                     const LEX_CSTRING *name, const LEX_CSTRING *component);
extern bool check_reserved_words(const LEX_CSTRING *name);
double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate);

extern bool volatile  mqh_used;

bool update_hash(user_var_entry *entry, bool set_null, void *ptr, size_t length,
                 Item_result type, CHARSET_INFO *cs,
                 bool unsigned_arg);

#endif /* ITEM_FUNC_INCLUDED */
