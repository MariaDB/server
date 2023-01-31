#ifndef ITEM_CMPFUNC_INCLUDED
#define ITEM_CMPFUNC_INCLUDED
/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB

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


/* compare and test functions */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "item_func.h"             /* Item_int_func, Item_bool_func */
#include "item.h"

extern Item_result item_cmp_type(Item_result a,Item_result b);
inline Item_result item_cmp_type(const Item *a, const Item *b)
{
  return item_cmp_type(a->cmp_type(), b->cmp_type());
}
inline Item_result item_cmp_type(Item_result a, const Item *b)
{
  return item_cmp_type(a, b->cmp_type());
}
class Item_bool_func2;
class Arg_comparator;

typedef int (Arg_comparator::*arg_cmp_func)();

typedef int (*Item_field_cmpfunc)(Item *f1, Item *f2, void *arg); 

class Arg_comparator: public Sql_alloc
{
  Item **a, **b;
  const Type_handler *m_compare_handler;
  CHARSET_INFO *m_compare_collation;
  arg_cmp_func func;
  Item_func_or_sum *owner;
  bool set_null;                   // TRUE <=> set owner->null_value
  Arg_comparator *comparators;   // used only for compare_row()
  double precision;
  /* Fields used in DATE/DATETIME comparison. */
  Item *a_cache, *b_cache;         // Cached values of a and b items
                                   //   when one of arguments is NULL.

  int set_cmp_func(THD *thd, Item_func_or_sum *owner_arg,
                   Item **a1, Item **a2);

  int compare_not_null_values(longlong val1, longlong val2)
  {
    if (set_null)
      owner->null_value= false;
    if (val1 < val2) return -1;
    if (val1 == val2) return 0;
    return 1;
  }
  NativeBuffer<STRING_BUFFER_USUAL_SIZE> m_native1, m_native2;
public:
  /* Allow owner function to use string buffers. */
  String value1, value2;

  Arg_comparator():
    m_compare_handler(&type_handler_null),
    m_compare_collation(&my_charset_bin),
    set_null(TRUE), comparators(0),
    a_cache(0), b_cache(0) {};
  Arg_comparator(Item **a1, Item **a2): a(a1), b(a2),
    m_compare_handler(&type_handler_null),
    m_compare_collation(&my_charset_bin),
    set_null(TRUE), comparators(0),
    a_cache(0), b_cache(0) {};

public:
  bool set_cmp_func_for_row_arguments(THD *thd);
  bool set_cmp_func_row(THD *thd);
  bool set_cmp_func_string(THD *thd);
  bool set_cmp_func_time(THD *thd);
  bool set_cmp_func_datetime(THD *thd);
  bool set_cmp_func_native(THD *thd);
  bool set_cmp_func_int(THD *thd);
  bool set_cmp_func_real(THD *thd);
  bool set_cmp_func_decimal(THD *thd);

  inline int set_cmp_func(THD *thd, Item_func_or_sum *owner_arg,
			  Item **a1, Item **a2, bool set_null_arg)
  {
    set_null= set_null_arg;
    return set_cmp_func(thd, owner_arg, a1, a2);
  }
  inline int compare() { return (this->*func)(); }

  int compare_string();		 // compare args[0] & args[1]
  int compare_real();            // compare args[0] & args[1]
  int compare_decimal();         // compare args[0] & args[1]
  int compare_int_signed();      // compare args[0] & args[1]
  int compare_int_signed_unsigned();
  int compare_int_unsigned_signed();
  int compare_int_unsigned();
  int compare_row();             // compare args[0] & args[1]
  int compare_e_string();	 // compare args[0] & args[1]
  int compare_e_real();          // compare args[0] & args[1]
  int compare_e_decimal();       // compare args[0] & args[1]
  int compare_e_int();           // compare args[0] & args[1]
  int compare_e_int_diff_signedness();
  int compare_e_row();           // compare args[0] & args[1]
  int compare_real_fixed();
  int compare_e_real_fixed();
  int compare_datetime();
  int compare_e_datetime();
  int compare_time();
  int compare_e_time();
  int compare_native();
  int compare_e_native();
  int compare_json_str_basic(Item *j, Item *s);
  int compare_json_str();
  int compare_str_json();
  int compare_e_json_str_basic(Item *j, Item *s);
  int compare_e_json_str();
  int compare_e_str_json();

  void min_max_update_field_native(THD *thd, Field *field, Item *item,
                                   int cmp_sign);

  Item** cache_converted_constant(THD *thd, Item **value, Item **cache,
                                  const Type_handler *type);
  inline bool is_owner_equal_func()
  {
    return (owner->type() == Item::FUNC_ITEM &&
           ((Item_func*)owner)->functype() == Item_func::EQUAL_FUNC);
  }
  const Type_handler *compare_type_handler() const { return m_compare_handler; }
  Item_result compare_type() const { return m_compare_handler->cmp_type(); }
  CHARSET_INFO *compare_collation() const { return m_compare_collation; }
  Arg_comparator *subcomparators() const { return comparators; }
  void cleanup()
  {
    delete [] comparators;
    comparators= 0;
  }
  friend class Item_func;
  friend class Item_bool_rowready_func2;
};


class SEL_ARG;
struct KEY_PART;

class Item_bool_func :public Item_int_func,
                      public Type_cmp_attributes
{
protected:
  /*
    Build a SEL_TREE for a simple predicate
    @param  param       PARAM from SQL_SELECT::test_quick_select
    @param  field       field in the predicate
    @param  value       constant in the predicate
    @return Pointer to the tree built tree
  */
  virtual SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                                     Field *field, Item *value)
  {
    DBUG_ENTER("Item_bool_func::get_func_mm_tree");
    DBUG_ASSERT(0);
    DBUG_RETURN(0);
  }
  /*
    Return the full select tree for "field_item" and "value":
    - a single SEL_TREE if the field is not in a multiple equality, or
    - a conjunction of all SEL_TREEs for all fields from
      the same multiple equality with "field_item".
  */
  SEL_TREE *get_full_func_mm_tree(RANGE_OPT_PARAM *param,
                                  Item_field *field_item, Item *value);
  /**
    Test if "item" and "value" are suitable for the range optimization
    and get their full select tree.

    "Suitable" means:
    - "item" is a field or a field reference
    - "value" is NULL                (e.g. WHERE field IS NULL), or
      "value" is an unexpensive item (e.g. WHERE field OP value)

    @param item  - the argument that is checked to be a field
    @param value - the other argument
    @returns - NULL if the arguments are not suitable for the range optimizer.
    @returns - the full select tree if the arguments are suitable.
  */
  SEL_TREE *get_full_func_mm_tree_for_args(RANGE_OPT_PARAM *param,
                                           Item *item, Item *value)
  {
    DBUG_ENTER("Item_bool_func::get_full_func_mm_tree_for_args");
    Item *field= item->real_item();
    if (field->type() == Item::FIELD_ITEM && !field->const_item() &&
        (!value || !value->is_expensive()))
      DBUG_RETURN(get_full_func_mm_tree(param, (Item_field *) field, value));
    DBUG_RETURN(NULL);
  }
  SEL_TREE *get_mm_parts(RANGE_OPT_PARAM *param, Field *field,
                         Item_func::Functype type, Item *value);
  SEL_TREE *get_ne_mm_tree(RANGE_OPT_PARAM *param,
                           Field *field, Item *lt_value, Item *gt_value);
  virtual SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, Field *field,
                               KEY_PART *key_part,
                               Item_func::Functype type, Item *value);
public:
  Item_bool_func(THD *thd): Item_int_func(thd) {}
  Item_bool_func(THD *thd, Item *a): Item_int_func(thd, a) {}
  Item_bool_func(THD *thd, Item *a, Item *b): Item_int_func(thd, a, b) {}
  Item_bool_func(THD *thd, Item *a, Item *b, Item *c): Item_int_func(thd, a, b, c) {}
  Item_bool_func(THD *thd, List<Item> &list): Item_int_func(thd, list) { }
  Item_bool_func(THD *thd, Item_bool_func *item) :Item_int_func(thd, item) {}
  const Type_handler *type_handler() const override
  { return &type_handler_bool; }
  const Type_handler *fixed_type_handler() const override
  { return &type_handler_bool; }
  CHARSET_INFO *compare_collation() const override { return NULL; }
  bool fix_length_and_dec(THD *thd) override { decimals=0; max_length=1; return FALSE; }
  decimal_digits_t decimal_precision() const override { return 1; }
  bool need_parentheses_in_default() override { return true; }
};


/**
  Abstract Item class, to represent <code>X IS [NOT] (TRUE | FALSE)</code>
  boolean predicates.
*/

class Item_func_truth : public Item_bool_func
{
public:
  bool val_bool() override;
  longlong val_int() override;
  bool fix_length_and_dec(THD *thd) override;
  void print(String *str, enum_query_type query_type) override;
  enum precedence precedence() const override { return CMP_PRECEDENCE; }

protected:
  Item_func_truth(THD *thd, Item *a, bool a_value, bool a_affirmative):
    Item_bool_func(thd, a), value(a_value), affirmative(a_affirmative)
  {}

  ~Item_func_truth()
  {}
private:
  /**
    True for <code>X IS [NOT] TRUE</code>,
    false for <code>X IS [NOT] FALSE</code> predicates.
  */
  const bool value;
  /**
    True for <code>X IS Y</code>, false for <code>X IS NOT Y</code> predicates.
  */
  const bool affirmative;
};


/**
  This Item represents a <code>X IS TRUE</code> boolean predicate.
*/

class Item_func_istrue : public Item_func_truth
{
public:
  Item_func_istrue(THD *thd, Item *a): Item_func_truth(thd, a, true, true) {}
  ~Item_func_istrue() {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("istrue") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_istrue>(thd, this); }
};


/**
  This Item represents a <code>X IS NOT TRUE</code> boolean predicate.
*/

class Item_func_isnottrue : public Item_func_truth
{
public:
  Item_func_isnottrue(THD *thd, Item *a):
    Item_func_truth(thd, a, true, false) {}
  ~Item_func_isnottrue() {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("isnottrue") };
    return name;
  }
  bool find_not_null_fields(table_map allowed) override { return false; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_isnottrue>(thd, this); }
  bool eval_not_null_tables(void *) override
  { not_null_tables_cache= 0; return false; }
};


/**
  This Item represents a <code>X IS FALSE</code> boolean predicate.
*/

class Item_func_isfalse : public Item_func_truth
{
public:
  Item_func_isfalse(THD *thd, Item *a): Item_func_truth(thd, a, false, true) {}
  ~Item_func_isfalse() {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("isfalse") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_isfalse>(thd, this); }
};


/**
  This Item represents a <code>X IS NOT FALSE</code> boolean predicate.
*/

class Item_func_isnotfalse : public Item_func_truth
{
public:
  Item_func_isnotfalse(THD *thd, Item *a):
    Item_func_truth(thd, a, false, false) {}
  ~Item_func_isnotfalse() {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("isnotfalse") };
    return name;
  }
  bool find_not_null_fields(table_map allowed) override { return false; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_isnotfalse>(thd, this); }
  bool eval_not_null_tables(void *) override
  { not_null_tables_cache= 0; return false; }
};


class Item_cache;
#define UNKNOWN (-1)


/*
  Item_in_optimizer(left_expr, Item_in_subselect(...))

  Item_in_optimizer is used to wrap an instance of Item_in_subselect. This
  class does the following:
   - Evaluate the left expression and store it in Item_cache_* object (to
     avoid re-evaluating it many times during subquery execution)
   - Shortcut the evaluation of "NULL IN (...)" to NULL in the cases where we
     don't care if the result is NULL or FALSE.

  NOTE
    It is not quite clear why the above listed functionality should be
    placed into a separate class called 'Item_in_optimizer'.
*/

class Item_in_optimizer: public Item_bool_func
{
protected:
  Item_cache *cache;
  Item *expr_cache;
  bool save_cache;
  /* 
    Stores the value of "NULL IN (SELECT ...)" for uncorrelated subqueries:
      UNKNOWN - "NULL in (SELECT ...)" has not yet been evaluated
      FALSE   - result is FALSE
      TRUE    - result is NULL
  */
  int result_for_null_param;
public:
  Item_in_optimizer(THD *thd, Item *a, Item *b):
    Item_bool_func(thd, a, b), cache(0), expr_cache(0),
    save_cache(0), result_for_null_param(UNKNOWN)
  {
    with_flags|= item_with_t::SUBQUERY;
  }
  bool fix_fields(THD *, Item **) override;
  bool fix_left(THD *thd);
  table_map not_null_tables() const override { return 0; }
  bool is_null() override;
  longlong val_int() override;
  void cleanup() override;
  enum Functype functype() const override { return IN_OPTIMIZER_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<in_optimizer>") };
    return name;
  }
  Item_cache **get_cache() { return &cache; }
  void keep_top_level_cache();
  Item *transform(THD *thd, Item_transformer transformer, uchar *arg) override;
  Item *expr_cache_insert_transformer(THD *thd, uchar *unused) override;
  bool is_expensive_processor(void *arg) override;
  bool is_expensive() override;
  void set_join_tab_idx(uint8 join_tab_idx_arg) override
  { args[1]->set_join_tab_idx(join_tab_idx_arg); }
  void get_cache_parameters(List<Item> &parameters) override;
  bool eval_not_null_tables(void *opt_arg) override;
  bool find_not_null_fields(table_map allowed) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref,
                         bool merge) override;
  bool invisible_mode();
  void reset_cache() { cache= NULL; }
  void print(String *str, enum_query_type query_type) override;
  void restore_first_argument();
  Item* get_wrapped_in_subselect_item()
  { return args[1]; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_in_optimizer>(thd, this); }
  enum precedence precedence() const override { return args[1]->precedence(); }
};


/*
  Functions and operators with two arguments that can use range optimizer.
*/
class Item_bool_func2 :public Item_bool_func
{                                              /* Bool with 2 string args */
protected:
  void add_key_fields_optimize_op(JOIN *join, KEY_FIELD **key_fields,
                                  uint *and_level, table_map usable_tables,
                                  SARGABLE_PARAM **sargables, bool equal_func);
public:
  Item_bool_func2(THD *thd, Item *a, Item *b):
    Item_bool_func(thd, a, b) { }

  bool is_null() { return MY_TEST(args[0]->is_null() || args[1]->is_null()); }
  COND *remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                        bool top_level);
  bool count_sargable_conds(void *arg);
  /*
    Specifies which result type the function uses to compare its arguments.
    This method is used in equal field propagation.
  */
  virtual const Type_handler *compare_type_handler() const
  {
    /*
      Have STRING_RESULT by default, which means the function compares
      val_str() results of the arguments. This is suitable for Item_func_like
      and for Item_func_spatial_rel.
      Note, Item_bool_rowready_func2 overrides this default behaviour.
    */
    return &type_handler_varchar;
  }
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr)
  {
    DBUG_ENTER("Item_bool_func2::get_mm_tree");
    DBUG_ASSERT(arg_count == 2);
    SEL_TREE *ftree= get_full_func_mm_tree_for_args(param, args[0], args[1]);
    if (!ftree)
      ftree= Item_func::get_mm_tree(param, cond_ptr);
    DBUG_RETURN(ftree);
  }
};


/**
  A class for functions and operators that can use the range optimizer and
  have a reverse function/operator that can also use the range optimizer,
  so this condition:
    WHERE value OP field
  can be optimized as equivalent to:
    WHERE field REV_OP value

  This class covers:
  - scalar comparison predicates:  <, <=, =, <=>, >=, >
  - MBR and precise spatial relation predicates (e.g. SP_TOUCHES(x,y))

  For example:
    WHERE 10 > field
  can be optimized as:
    WHERE field < 10
*/
class Item_bool_func2_with_rev :public Item_bool_func2
{
protected:
  SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                             Field *field, Item *value)
  {
    DBUG_ENTER("Item_bool_func2_with_rev::get_func_mm_tree");
    Item_func::Functype func_type=
      (value != arguments()[0]) ? functype() : rev_functype();
    DBUG_RETURN(get_mm_parts(param, field, func_type, value));
  }
public:
  Item_bool_func2_with_rev(THD *thd, Item *a, Item *b):
    Item_bool_func2(thd, a, b) { }
  virtual enum Functype rev_functype() const= 0;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr)
  {
    DBUG_ENTER("Item_bool_func2_with_rev::get_mm_tree");
    DBUG_ASSERT(arg_count == 2);
    SEL_TREE *ftree;
    /*
      Even if get_full_func_mm_tree_for_args(param, args[0], args[1]) will not
      return a range predicate it may still be possible to create one
      by reversing the order of the operands. Note that this only
      applies to predicates where both operands are fields. Example: A
      query of the form

         WHERE t1.a OP t2.b

      In this case, args[0] == t1.a and args[1] == t2.b.
      When creating range predicates for t2,
      get_full_func_mm_tree_for_args(param, args[0], args[1])
      will return NULL because 'field' belongs to t1 and only
      predicates that applies to t2 are of interest. In this case a
      call to get_full_func_mm_tree_for_args() with reversed operands
      may succeed.
    */
    if (!(ftree= get_full_func_mm_tree_for_args(param, args[0], args[1])) &&
        !(ftree= get_full_func_mm_tree_for_args(param, args[1], args[0])))
      ftree= Item_func::get_mm_tree(param, cond_ptr);
    DBUG_RETURN(ftree);
  }
};


class Item_bool_rowready_func2 :public Item_bool_func2_with_rev
{
protected:
  Arg_comparator cmp;
  bool check_arguments() const override
  {
    return check_argument_types_like_args0();
  }
public:
  Item_bool_rowready_func2(THD *thd, Item *a, Item *b):
    Item_bool_func2_with_rev(thd, a, b), cmp(tmp_arg, tmp_arg + 1)
  { }
  Sql_mode_dependency value_depends_on_sql_mode() const override;
  void print(String *str, enum_query_type query_type) override
  {
    Item_func::print_op(str, query_type);
  }
  enum precedence precedence() const override { return CMP_PRECEDENCE; }
  Item *neg_transformer(THD *thd) override;
  virtual Item *negated_item(THD *thd);
  Item *propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    Item_args::propagate_equal_fields(thd,
                                      Context(ANY_SUBST,
                                              cmp.compare_type_handler(),
                                              compare_collation()),
                                      cond);
    return this;
  }
  bool fix_length_and_dec(THD *thd) override;
  int set_cmp_func(THD *thd)
  {
    return cmp.set_cmp_func(thd, this, tmp_arg, tmp_arg + 1, true);
  }
  CHARSET_INFO *compare_collation() const override
  { return cmp.compare_collation(); }
  const Type_handler *compare_type_handler() const override
  {
    return cmp.compare_type_handler();
  }
  Arg_comparator *get_comparator() { return &cmp; }
  void cleanup() override
  {
    Item_bool_func2::cleanup();
    cmp.cleanup();
  }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override
  {
    return add_key_fields_optimize_op(join, key_fields, and_level,
                                      usable_tables, sargables, false);
  }
  Item *build_clone(THD *thd) override
  {
    Item_bool_rowready_func2 *clone=
      (Item_bool_rowready_func2 *) Item_func::build_clone(thd);
    if (clone)
    {
      clone->cmp.comparators= 0;
    }
    return clone;
  }
};

/**
  XOR inherits from Item_bool_func because it is not optimized yet.
  Later, when XOR is optimized, it needs to inherit from
  Item_cond instead. See WL#5800.
*/
class Item_func_xor :public Item_bool_func
{
public:
  Item_func_xor(THD *thd, Item *i1, Item *i2): Item_bool_func(thd, i1, i2) {}
  enum Functype functype() const override { return XOR_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("xor") };
    return name;
  }
  enum precedence precedence() const override { return XOR_PRECEDENCE; }
  void print(String *str, enum_query_type query_type) override
  { Item_func::print_op(str, query_type); }
  longlong val_int() override;
  bool find_not_null_fields(table_map allowed) override { return false; }
  Item *neg_transformer(THD *thd) override;
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond) override
  {
    Item_args::propagate_equal_fields(thd, Context_boolean(), cond);
    return this;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_xor>(thd, this); }
};

class Item_func_not :public Item_bool_func
{
public:
  Item_func_not(THD *thd, Item *a): Item_bool_func(thd, a) {}
  longlong val_int() override;
  enum Functype functype() const override { return NOT_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("not") };
    return name;
  }
  bool find_not_null_fields(table_map allowed) override { return false; }
  enum precedence precedence() const override { return NEG_PRECEDENCE; }
  Item *neg_transformer(THD *thd) override;
  bool fix_fields(THD *, Item **) override;
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_not>(thd, this); }
};

class Item_maxmin_subselect;

/*
  trigcond<param>(arg) ::= param? arg : TRUE

  The class Item_func_trig_cond is used for guarded predicates 
  which are employed only for internal purposes.
  A guarded predicate is an object consisting of an a regular or
  a guarded predicate P and a pointer to a boolean guard variable g. 
  A guarded predicate P/g is evaluated to true if the value of the
  guard g is false, otherwise it is evaluated to the same value that
  the predicate P: val(P/g)= g ? val(P):true.
  Guarded predicates allow us to include predicates into a conjunction
  conditionally. Currently they are utilized for pushed down predicates
  in queries with outer join operations.

  In the future, probably, it makes sense to extend this class to
  the objects consisting of three elements: a predicate P, a pointer
  to a variable g and a firing value s with following evaluation
  rule: val(P/g,s)= g==s? val(P) : true. It will allow us to build only
  one item for the objects of the form P/g1/g2... 

  Objects of this class are built only for query execution after
  the execution plan has been already selected. That's why this
  class needs only val_int out of generic methods. 
 
  Current uses of Item_func_trig_cond objects:
   - To wrap selection conditions when executing outer joins
   - To wrap condition that is pushed down into subquery
*/

class Item_func_trig_cond: public Item_bool_func
{
  bool *trig_var;
public:
  Item_func_trig_cond(THD *thd, Item *a, bool *f): Item_bool_func(thd, a)
  { trig_var= f; }
  longlong val_int() override { return *trig_var ? args[0]->val_int() : 1; }
  enum Functype functype() const override { return TRIG_COND_FUNC; };
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("trigcond") };
    return name;
  }
  bool const_item() const override { return FALSE; }
  bool *get_trig_var() { return trig_var; }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_trig_cond>(thd, this); }
};

class Item_func_not_all :public Item_func_not
{
  /* allow to check presence of values in max/min optimization */
  Item_sum_min_max *test_sum_item;
  Item_maxmin_subselect *test_sub_item;

public:
  bool show;

  Item_func_not_all(THD *thd, Item *a):
    Item_func_not(thd, a), test_sum_item(0), test_sub_item(0), show(0)
    {}
  table_map not_null_tables() const override { return 0; }
  longlong val_int() override;
  enum Functype functype() const override { return NOT_ALL_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<not>") };
    return name;
  }
  enum precedence precedence() const override
  { return show ? Item_func::precedence() : args[0]->precedence(); }
  bool fix_fields(THD *thd, Item **ref) override
  { return Item_func::fix_fields(thd, ref);}
  void print(String *str, enum_query_type query_type) override;
  void set_sum_test(Item_sum_min_max *item) { test_sum_item= item; test_sub_item= 0; };
  void set_sub_test(Item_maxmin_subselect *item) { test_sub_item= item; test_sum_item= 0;};
  bool empty_underlying_subquery();
  Item *neg_transformer(THD *thd) override;
};


class Item_func_nop_all :public Item_func_not_all
{
public:

  Item_func_nop_all(THD *thd, Item *a): Item_func_not_all(thd, a) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<nop>") };
    return name;
  }
  Item *neg_transformer(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_nop_all>(thd, this); }
};


class Item_func_eq :public Item_bool_rowready_func2
{
public:
  Item_func_eq(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b),
      in_equality_no(UINT_MAX)
  {}
  longlong val_int() override;
  enum Functype functype() const override { return EQ_FUNC; }
  enum Functype rev_functype() const override { return EQ_FUNC; }
  cond_result eq_cmp_result() const override { return COND_TRUE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("=") };
    return name;
  }
  Item *negated_item(THD *thd) override;
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override;
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override
  {
    return add_key_fields_optimize_op(join, key_fields, and_level,
                                      usable_tables, sargables, true);
  }
  bool check_equality(THD *thd, COND_EQUAL *cond, List<Item> *eq_list) override;
  /* 
    - If this equality is created from the subquery's IN-equality:
      number of the item it was created from, e.g. for
       (a,b) IN (SELECT c,d ...)  a=c will have in_equality_no=0, 
       and b=d will have in_equality_no=1.
    - Otherwise, UINT_MAX
  */
  uint in_equality_no;
  uint exists2in_reserved_items() override { return 1; };
  friend class  Arg_comparator;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_eq>(thd, this); }
};

class Item_func_equal final :public Item_bool_rowready_func2
{
public:
  Item_func_equal(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b) {}
  longlong val_int() override;
  bool fix_length_and_dec(THD *thd) override;
  table_map not_null_tables() const override { return 0; }
  bool find_not_null_fields(table_map allowed) override { return false; }
  enum Functype functype() const override { return EQUAL_FUNC; }
  enum Functype rev_functype() const override { return EQUAL_FUNC; }
  cond_result eq_cmp_result() const override { return COND_TRUE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<=>") };
    return name;
  }
  Item *neg_transformer(THD *thd) override { return 0; }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override
  {
    return add_key_fields_optimize_op(join, key_fields, and_level,
                                      usable_tables, sargables, true);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_equal>(thd, this); }
};


class Item_func_ge :public Item_bool_rowready_func2
{
public:
  Item_func_ge(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b) {};
  longlong val_int() override;
  enum Functype functype() const override { return GE_FUNC; }
  enum Functype rev_functype() const override { return LE_FUNC; }
  cond_result eq_cmp_result() const override { return COND_TRUE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN(">=") };
    return name;
  }
  Item *negated_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ge>(thd, this); }
};


class Item_func_gt :public Item_bool_rowready_func2
{
public:
  Item_func_gt(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b) {};
  longlong val_int() override;
  enum Functype functype() const override { return GT_FUNC; }
  enum Functype rev_functype() const override { return LT_FUNC; }
  cond_result eq_cmp_result() const override { return COND_FALSE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN(">") };
    return name;
  }
  Item *negated_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_gt>(thd, this); }
};


class Item_func_le :public Item_bool_rowready_func2
{
public:
  Item_func_le(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b) {};
  longlong val_int() override;
  enum Functype functype() const override { return LE_FUNC; }
  enum Functype rev_functype() const override { return GE_FUNC; }
  cond_result eq_cmp_result() const override { return COND_TRUE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<=") };
    return name;
  }
  Item *negated_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_le>(thd, this); }
};


class Item_func_lt :public Item_bool_rowready_func2
{
public:
  Item_func_lt(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b) {}
  longlong val_int() override;
  enum Functype functype() const override { return LT_FUNC; }
  enum Functype rev_functype() const override { return GT_FUNC; }
  cond_result eq_cmp_result() const override { return COND_FALSE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<") };
    return name;
  }
  Item *negated_item(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_lt>(thd, this); }
};


class Item_func_ne :public Item_bool_rowready_func2
{
protected:
  SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                             Field *field, Item *value) override;
public:
  Item_func_ne(THD *thd, Item *a, Item *b):
    Item_bool_rowready_func2(thd, a, b) {}
  longlong val_int() override;
  enum Functype functype() const override { return NE_FUNC; }
  enum Functype rev_functype() const override { return NE_FUNC; }
  cond_result eq_cmp_result() const override { return COND_FALSE; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<>") };
    return name;
  }
  Item *negated_item(THD *thd) override;
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields, uint *and_level,
                      table_map usable_tables, SARGABLE_PARAM **sargables) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ne>(thd, this); }
};


/*
  The class Item_func_opt_neg is defined to factor out the functionality
  common for the classes Item_func_between and Item_func_in. The objects
  of these classes can express predicates or there negations.
  The alternative approach would be to create pairs Item_func_between,
  Item_func_notbetween and Item_func_in, Item_func_notin.

*/

class Item_func_opt_neg :public Item_bool_func
{
protected:
  /*
    The data type handler that will be used for comparison.
    Data type handlers of all arguments are mixed to here.
  */
  Type_handler_hybrid_field_type m_comparator;
  /*
    The collation that will be used for comparison in case
    when m_compare_type is STRING_RESULT.
  */
  DTCollation cmp_collation;
public:
  bool negated;     /* <=> the item represents NOT <func> */
public:
  Item_func_opt_neg(THD *thd, Item *a, Item *b, Item *c):
    Item_bool_func(thd, a, b, c), negated(0) {}
  Item_func_opt_neg(THD *thd, List<Item> &list):
    Item_bool_func(thd, list), negated(0) {}
public:
  Item *neg_transformer(THD *thd) override
  {
    negated= !negated;
    return this;
  }
  bool eq(const Item *item, bool binary_cmp) const override;
  CHARSET_INFO *compare_collation() const override
  {
    return cmp_collation.collation;
  }
  Item *propagate_equal_fields(THD *, const Context &,
                               COND_EQUAL *) override= 0;
};

class Item_func_between :public Item_func_opt_neg
{
protected:
  SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                             Field *field, Item *value) override;
  bool val_int_cmp_int_finalize(longlong value, longlong a, longlong b);
public:
  String value0,value1,value2;
  Item_func_between(THD *thd, Item *a, Item *b, Item *c):
    Item_func_opt_neg(thd, a, b, c) { }
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    return m_comparator.type_handler()->Item_func_between_val_int(this);
  }
  enum Functype functype() const override { return BETWEEN; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("between") };
    return name;
  }
  enum precedence precedence() const override { return BETWEEN_PRECEDENCE; }
  bool fix_length_and_dec(THD *thd) override;
  bool fix_length_and_dec_string(THD *)
  {
    return agg_arg_charsets_for_comparison(cmp_collation, args, 3);
  }
  bool fix_length_and_dec_temporal(THD *);
  bool fix_length_and_dec_numeric(THD *);
  void print(String *str, enum_query_type query_type) override;
  bool eval_not_null_tables(void *opt_arg) override;
  bool find_not_null_fields(table_map allowed) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  bool count_sargable_conds(void *arg) override;
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override;
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    Item_args::propagate_equal_fields(thd,
                                      Context(ANY_SUBST,
                                              m_comparator.type_handler(),
                                              compare_collation()),
                                      cond);
    return this;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_between>(thd, this); }

  longlong val_int_cmp_string();
  longlong val_int_cmp_datetime();
  longlong val_int_cmp_time();
  longlong val_int_cmp_native();
  longlong val_int_cmp_int();
  longlong val_int_cmp_real();
  longlong val_int_cmp_decimal();
};


class Item_func_strcmp :public Item_long_func
{
  bool check_arguments() const override
  { return check_argument_types_can_return_str(0, 2); }
  String value1, value2;
  DTCollation cmp_collation;
public:
  Item_func_strcmp(THD *thd, Item *a, Item *b):
    Item_long_func(thd, a, b) {}
  longlong val_int() override;
  decimal_digits_t decimal_precision() const override { return 1; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("strcmp") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    if (agg_arg_charsets_for_comparison(cmp_collation, args, 2))
      return TRUE;
    fix_char_length(2); // returns "1" or "0" or "-1"
    return FALSE;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_strcmp>(thd, this); }
};


struct interval_range
{
  Item_result type;
  double dbl;
  my_decimal dec;
};

class Item_func_interval :public Item_long_func
{
  Item_row *row;
  bool use_decimal_comparison;
  interval_range *intervals;
  bool check_arguments() const override
  {
    return check_argument_types_like_args0();
  }
public:
  Item_func_interval(THD *thd, Item_row *a):
    Item_long_func(thd, a), row(a), intervals(0)
  { }
  bool fix_fields(THD *, Item **) override;
  longlong val_int() override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("interval") };
    return name;
  }
  decimal_digits_t decimal_precision() const override { return 2; }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(func_name_cstring());
    print_args(str, 0, query_type);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_interval>(thd, this); }
};


class Item_func_coalesce :public Item_func_case_expression
{
public:
  Item_func_coalesce(THD *thd, Item *a, Item *b):
    Item_func_case_expression(thd, a, b) {}
  Item_func_coalesce(THD *thd, List<Item> &list):
    Item_func_case_expression(thd, list) {}
  double real_op() override;
  longlong int_op() override;
  String *str_op(String *) override;
  my_decimal *decimal_op(my_decimal *) override;
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  bool native_op(THD *thd, Native *to) override;
  bool fix_length_and_dec(THD *thd) override
  {
    if (aggregate_for_result(func_name_cstring(), args, arg_count, true))
      return TRUE;
    fix_attributes(args, arg_count);
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("coalesce") };
    return name;
  }
  table_map not_null_tables() const override { return 0; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_coalesce>(thd, this); }
};


/*
  Case abbreviations that aggregate its result field type by two arguments:
    IFNULL(arg1, arg2)
    IF(switch, arg1, arg2)
    NVL2(switch, arg1, arg2)
*/
class Item_func_case_abbreviation2 :public Item_func_case_expression
{
protected:
  bool fix_length_and_dec2(Item **items)
  {
    if (aggregate_for_result(func_name_cstring(), items, 2, true))
      return TRUE;
    fix_attributes(items, 2);
    return FALSE;
  }

  void cache_type_info(const Item *source, bool maybe_null_arg)
  {
    Type_std_attributes::set(source);
    set_handler(source->type_handler());
    set_maybe_null(maybe_null_arg);
  }

  bool fix_length_and_dec2_eliminate_null(Item **items)
  {
    // Let IF(cond, expr, NULL) and IF(cond, NULL, expr) inherit type from expr.
    if (items[0]->type() == NULL_ITEM)
    {
      cache_type_info(items[1], true);
      // If both arguments are NULL, make resulting type BINARY(0).
      if (items[1]->type() == NULL_ITEM)
        set_handler(&type_handler_string);
    }
    else if (items[1]->type() == NULL_ITEM)
    {
      cache_type_info(items[0], true);
    }
    else
    {
      if (fix_length_and_dec2(items))
        return TRUE;
    }
    return FALSE;
  }

public:
  Item_func_case_abbreviation2(THD *thd, Item *a, Item *b):
    Item_func_case_expression(thd, a, b) { }
  Item_func_case_abbreviation2(THD *thd, Item *a, Item *b, Item *c):
    Item_func_case_expression(thd, a, b, c) { }
};


class Item_func_ifnull :public Item_func_case_abbreviation2
{
public:
  Item_func_ifnull(THD *thd, Item *a, Item *b):
    Item_func_case_abbreviation2(thd, a, b) {}
  double real_op() override;
  longlong int_op() override;
  String *str_op(String *str) override;
  my_decimal *decimal_op(my_decimal *) override;
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  bool native_op(THD *thd, Native *to) override;
  bool fix_length_and_dec(THD *thd) override
  {
    /*
      Set nullability from args[1] by default.
      Note, some type handlers may reset maybe_null
      in Item_hybrid_func_fix_attributes() if args[1]
      is NOT NULL but cannot always be converted to
      the data type of "this" safely.
      E.g. Type_handler_inet6 does:
        IFNULL(inet6_not_null_expr, 'foo') -> INET6 NULL
        IFNULL(inet6_not_null_expr, '::1') -> INET6 NOT NULL
    */
    copy_flags(args[1], item_base_t::MAYBE_NULL);
    if (Item_func_case_abbreviation2::fix_length_and_dec2(args))
      return TRUE;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ifnull") };
    return name;
  }

  table_map not_null_tables() const override { return 0; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_ifnull>(thd, this); }
};


/**
  Case abbreviations that have a switch argument and
  two return arguments to choose from. Returns the value
  of either of the two return arguments depending on the switch argument value.

  IF(switch, arg1, arg2)
  NVL(switch, arg1, arg2)
*/
class Item_func_case_abbreviation2_switch: public Item_func_case_abbreviation2
{
protected:
  virtual Item *find_item() const= 0;

public:
  Item_func_case_abbreviation2_switch(THD *thd, Item *a, Item *b, Item *c)
    :Item_func_case_abbreviation2(thd, a, b, c)
  { }

  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    Datetime_truncation_not_needed dt(thd, find_item(), fuzzydate);
    return (null_value= dt.copy_to_mysql_time(ltime, mysql_timestamp_type()));
  }
  bool time_op(THD *thd, MYSQL_TIME *ltime)
  {
    return (null_value= Time(find_item()).copy_to_mysql_time(ltime));
  }
  longlong int_op()
  {
    return val_int_from_item(find_item());
  }
  double real_op()
  {
    return val_real_from_item(find_item());
  }
  my_decimal *decimal_op(my_decimal *decimal_value)
  {
    return val_decimal_from_item(find_item(), decimal_value);
  }
  String *str_op(String *str)
  {
    return val_str_from_item(find_item(), str);
  }
  bool native_op(THD *thd, Native *to)
  {
    return val_native_with_conversion_from_item(thd, find_item(), to,
                                                type_handler());
  }
};


class Item_func_if :public Item_func_case_abbreviation2_switch
{
protected:
  Item *find_item() const override
  { return args[0]->val_bool() ? args[1] : args[2]; }

public:
  Item_func_if(THD *thd, Item *a, Item *b, Item *c):
    Item_func_case_abbreviation2_switch(thd, a, b, c)
  {}
  bool fix_fields(THD *, Item **) override;
  bool fix_length_and_dec(THD *thd) override
  {
    return fix_length_and_dec2_eliminate_null(args + 1);
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("if") };
    return name;
  }
  bool eval_not_null_tables(void *opt_arg) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_if>(thd, this); }
private:
  void cache_type_info(Item *source);
};


class Item_func_nvl2 :public Item_func_case_abbreviation2_switch
{
protected:
  Item *find_item() const override
  { return args[0]->is_null() ? args[2] : args[1]; }

public:
  Item_func_nvl2(THD *thd, Item *a, Item *b, Item *c):
    Item_func_case_abbreviation2_switch(thd, a, b, c)
  {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("nvl2") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    return fix_length_and_dec2_eliminate_null(args + 1);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_nvl2>(thd, this); }
};


class Item_func_nullif :public Item_func_case_expression
{
  Arg_comparator cmp;
  /*
    NULLIF(a,b) is a short for:
      CASE WHEN a=b THEN NULL ELSE a END

    The left "a" is for comparison purposes.
    The right "a" is for return value purposes.
    These are two different "a" and they can be replaced to different items.

    The left "a" is in a comparison and can be replaced by:
    - Item_func::convert_const_compared_to_int_field()
    - agg_item_set_converter() in set_cmp_func()
    - cache_converted_constant() in set_cmp_func()

    Both "a"s are subject to equal fields propagation and can be replaced by:
    - Item_field::propagate_equal_fields(ANY_SUBST) for the left "a"
    - Item_field::propagate_equal_fields(IDENTITY_SUBST) for the right "a"
  */
  Item_cache *m_cache;
  int compare();
  void reset_first_arg_if_needed()
  { 
    if (arg_count == 3 && args[0] != args[2])
      args[0]= args[2];
  }
  Item *m_arg0;
public:
  /*
    Here we pass three arguments to the parent constructor, as NULLIF
    is a three-argument function, it needs two copies of the first argument
    (see above). But fix_fields() will be confused if we try to prepare the
    same Item twice (if args[0]==args[2]), so we hide the third argument
    (decrementing arg_count) and copy args[2]=args[0] again after fix_fields().
    See also Item_func_nullif::fix_length_and_dec().
  */
  Item_func_nullif(THD *thd, Item *a, Item *b):
    Item_func_case_expression(thd, a, b, a),
    m_cache(NULL),
    m_arg0(NULL)
  { arg_count--; }
  void cleanup() override
  {
    Item_func_hybrid_field_type::cleanup();
    arg_count= 2; // See the comment to the constructor
  }
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  double real_op() override;
  longlong int_op() override;
  String *str_op(String *str) override;
  my_decimal *decimal_op(my_decimal *) override;
  bool native_op(THD *thd, Native *to) override;
  bool fix_length_and_dec(THD *thd) override;
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("nullif") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array, 
                      List<Item> &fields, uint flags) override;
  void update_used_tables() override;
  table_map not_null_tables() const override { return 0; }
  bool is_null() override;
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    Context cmpctx(ANY_SUBST, cmp.compare_type_handler(),
                              cmp.compare_collation());
    const Item *old0= args[0];
    args[0]->propagate_equal_fields_and_change_item_tree(thd, cmpctx,
                                                         cond, &args[0]);
    args[1]->propagate_equal_fields_and_change_item_tree(thd, cmpctx,
                                                         cond, &args[1]);
    /*
      MDEV-9712 Performance degradation of nested NULLIF
      ANY_SUBST is more relaxed than IDENTITY_SUBST.
      If ANY_SUBST did not change args[0],
      then we can skip propagation for args[2].
    */
    if (old0 != args[0])
      args[2]->propagate_equal_fields_and_change_item_tree(thd,
                                                           Context_identity(),
                                                           cond, &args[2]);
    return this;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_nullif>(thd, this); }
  Item *derived_field_transformer_for_having(THD *thd, uchar *arg) override
  { reset_first_arg_if_needed(); return this; }
  Item *derived_field_transformer_for_where(THD *thd, uchar *arg) override
  { reset_first_arg_if_needed(); return this; }
  Item *grouping_field_transformer_for_where(THD *thd, uchar *arg) override
  { reset_first_arg_if_needed(); return this; }
  Item *in_subq_field_transformer_for_where(THD *thd, uchar *arg) override
  { reset_first_arg_if_needed(); return this; }
  Item *in_subq_field_transformer_for_having(THD *thd, uchar *arg) override
  { reset_first_arg_if_needed(); return this; }
};


/* Functions to handle the optimized IN */


/* A vector of values of some type  */

class in_vector :public Sql_alloc
{
public:
  char *base;
  uint size;
  qsort2_cmp compare;
  CHARSET_INFO *collation;
  uint count;
  uint used_count;
  in_vector() {}
  in_vector(THD *thd, uint elements, uint element_length, qsort2_cmp cmp_func,
  	    CHARSET_INFO *cmp_coll)
    :base((char*) thd_calloc(thd, elements * element_length)),
     size(element_length), compare(cmp_func), collation(cmp_coll),
     count(elements), used_count(elements) {}
  virtual ~in_vector() {}
  virtual void set(uint pos,Item *item)=0;
  virtual uchar *get_value(Item *item)=0;
  void sort()
  {
    my_qsort2(base,used_count,size,compare,(void*)collation);
  }
  bool find(Item *item);
  
  /* 
    Create an instance of Item_{type} (e.g. Item_decimal) constant object
    which type allows it to hold an element of this vector without any
    conversions.
    The purpose of this function is to be able to get elements of this
    vector in form of Item_xxx constants without creating Item_xxx object
    for every array element you get (i.e. this implements "FlyWeight" pattern)
  */
  virtual Item* create_item(THD *thd) { return NULL; }
  
  /*
    Store the value at position #pos into provided item object
    SYNOPSIS
      value_to_item()
        pos   Index of value to store
        item  Constant item to store value into. The item must be of the same
              type that create_item() returns.
  */
  virtual void value_to_item(uint pos, Item *item) { }
  
  /* Compare values number pos1 and pos2 for equality */
  bool compare_elems(uint pos1, uint pos2)
  {
    return MY_TEST(compare(collation, base + pos1 * size, base + pos2 * size));
  }
  virtual const Type_handler *type_handler() const= 0;
};

class in_string :public in_vector
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  String tmp;
  class Item_string_for_in_vector: public Item_string
  {
  public:
    Item_string_for_in_vector(THD *thd, CHARSET_INFO *cs):
      Item_string(thd, cs)
    { }
    void set_value(const String *str)
    {
      str_value= *str;
      collation.set(str->charset());
    }
  };
public:
  in_string(THD *thd, uint elements, qsort2_cmp cmp_func, CHARSET_INFO *cs);
  ~in_string();
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  Item* create_item(THD *thd) override;
  void value_to_item(uint pos, Item *item) override
  {    
    String *str=((String*) base)+pos;
    Item_string_for_in_vector *to= (Item_string_for_in_vector*) item;
    to->set_value(str);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_varchar; }
};

class in_longlong :public in_vector
{
protected:
  /*
    Here we declare a temporary variable (tmp) of the same type as the
    elements of this vector. tmp is used in finding if a given value is in 
    the list. 
  */
  struct packed_longlong 
  {
    longlong val;
    longlong unsigned_flag;  // Use longlong, not bool, to preserve alignment
  } tmp;
public:
  in_longlong(THD *thd, uint elements);
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  Item* create_item(THD *thd) override;
  void value_to_item(uint pos, Item *item) override
  {
    ((Item_int*) item)->value= ((packed_longlong*) base)[pos].val;
    ((Item_int*) item)->unsigned_flag= (bool)
      ((packed_longlong*) base)[pos].unsigned_flag;
  }
  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }

  friend int cmp_longlong(void *cmp_arg, packed_longlong *a,packed_longlong *b);
};


class in_timestamp :public in_vector
{
  Timestamp_or_zero_datetime tmp;
public:
  in_timestamp(THD *thd, uint elements);
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  Item* create_item(THD *thd) override;
  void value_to_item(uint pos, Item *item) override;
  const Type_handler *type_handler() const override
  { return &type_handler_timestamp2; }
};


/*
  Class to represent a vector of constant DATE/DATETIME values.
*/
class in_temporal :public in_longlong
{
public:
  /* Cache for the left item. */

  in_temporal(THD *thd, uint elements)
    :in_longlong(thd, elements) {};
  Item *create_item(THD *thd);
  void value_to_item(uint pos, Item *item)
  {
    packed_longlong *val= reinterpret_cast<packed_longlong*>(base)+pos;
    Item_datetime *dt= static_cast<Item_datetime*>(item);
    dt->set(val->val, type_handler()->mysql_timestamp_type());
  }
  friend int cmp_longlong(void *cmp_arg, packed_longlong *a,packed_longlong *b);
};


class in_datetime :public in_temporal
{
public:
  in_datetime(THD *thd, uint elements)
   :in_temporal(thd, elements)
  {}
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  const Type_handler *type_handler() const override
  { return &type_handler_datetime2; }
};


class in_time :public in_temporal
{
public:
  in_time(THD *thd, uint elements)
   :in_temporal(thd, elements)
  {}
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  const Type_handler *type_handler() const override
  { return &type_handler_time2; }
};


class in_double :public in_vector
{
  double tmp;
public:
  in_double(THD *thd, uint elements);
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  Item *create_item(THD *thd) override;
  void value_to_item(uint pos, Item *item) override
  {
    ((Item_float*)item)->value= ((double*) base)[pos];
  }
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
};


class in_decimal :public in_vector
{
  my_decimal val;
public:
  in_decimal(THD *thd, uint elements);
  void set(uint pos, Item *item) override;
  uchar *get_value(Item *item) override;
  Item *create_item(THD *thd) override;
  void value_to_item(uint pos, Item *item) override
  {
    my_decimal *dec= ((my_decimal *)base) + pos;
    Item_decimal *item_dec= (Item_decimal*)item;
    item_dec->set_decimal_value(dec);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
};


/*
** Classes for easy comparing of non const items
*/

class cmp_item :public Sql_alloc
{
public:
  CHARSET_INFO *cmp_charset;
  cmp_item() { cmp_charset= &my_charset_bin; }
  virtual ~cmp_item() {}
  virtual void store_value(Item *item)= 0;
  /**
     @returns result (TRUE, FALSE or UNKNOWN) of
     "stored argument's value <> item's value"
  */
  virtual int cmp(Item *item)= 0;
  virtual int cmp_not_null(const Value *value)= 0;
  // for optimized IN with row
  virtual int compare(cmp_item *item)= 0;
  virtual cmp_item *make_same(THD *thd)= 0;
  virtual void store_value_by_template(THD *thd, cmp_item *tmpl, Item *item)
  {
    store_value(item);
  }
};

/// cmp_item which stores a scalar (i.e. non-ROW).
class cmp_item_scalar : public cmp_item
{
protected:
  bool m_null_value;                            ///< If stored value is NULL
};

class cmp_item_string : public cmp_item_scalar
{
protected:
  String *value_res;
public:
  cmp_item_string () {}
  cmp_item_string (CHARSET_INFO *cs) { cmp_charset= cs; }
  void set_charset(CHARSET_INFO *cs) { cmp_charset= cs; }
  friend class cmp_item_sort_string;
  friend class cmp_item_sort_string_in_static;
};

class cmp_item_sort_string :public cmp_item_string
{
protected:
  char value_buff[STRING_BUFFER_USUAL_SIZE];
  String value;
public:
  cmp_item_sort_string():
    cmp_item_string() {}
  cmp_item_sort_string(CHARSET_INFO *cs):
    cmp_item_string(cs),
    value(value_buff, sizeof(value_buff), cs) {}
  void store_value(Item *item)
  {
    value_res= item->val_str(&value);
    m_null_value= item->null_value;
    // Make sure to cache the result String inside "value"
    if (value_res && value_res != &value)
    {
      if (value.copy(*value_res))
        value.set("", 0, item->collation.collation);
      value_res= &value;
    }
  }
  int cmp_not_null(const Value *val)
  {
    DBUG_ASSERT(!val->is_null());
    DBUG_ASSERT(val->is_string());
    return sortcmp(value_res, &val->m_string, cmp_charset) != 0;
  }
  int cmp(Item *arg)
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String tmp(buff, sizeof(buff), cmp_charset), *res= arg->val_str(&tmp);
    if (m_null_value || arg->null_value)
      return UNKNOWN;
    if (value_res && res)
      return sortcmp(value_res, res, cmp_charset) != 0;
    else if (!value_res && !res)
      return FALSE;
    else
      return TRUE;
  }
  int compare(cmp_item *ci)
  {
    cmp_item_string *l_cmp= (cmp_item_string *) ci;
    return sortcmp(value_res, l_cmp->value_res, cmp_charset);
  } 
  cmp_item *make_same(THD *thd);
  void set_charset(CHARSET_INFO *cs)
  {
    cmp_charset= cs;
    value.set_buffer_if_not_allocated(value_buff, sizeof(value_buff), cs);
  }
};

class cmp_item_int : public cmp_item_scalar
{
  longlong value;
public:
  cmp_item_int() {}                           /* Remove gcc warning */
  void store_value(Item *item)
  {
    value= item->val_int();
    m_null_value= item->null_value;
  }
  int cmp_not_null(const Value *val)
  {
    DBUG_ASSERT(!val->is_null());
    DBUG_ASSERT(val->is_longlong());
    return value != val->value.m_longlong;
  }
  int cmp(Item *arg)
  {
    const bool rc= value != arg->val_int();
    return (m_null_value || arg->null_value) ? UNKNOWN : rc;
  }
  int compare(cmp_item *ci)
  {
    cmp_item_int *l_cmp= (cmp_item_int *)ci;
    return (value < l_cmp->value) ? -1 : ((value == l_cmp->value) ? 0 : 1);
  }
  cmp_item *make_same(THD *thd);
};

/*
  Compare items in the DATETIME context.
*/
class cmp_item_temporal: public cmp_item_scalar
{
protected:
  longlong value;
public:
  cmp_item_temporal() {}
  int compare(cmp_item *ci);
};


class cmp_item_datetime: public cmp_item_temporal
{
public:
  cmp_item_datetime()
   :cmp_item_temporal()
  { }
  void store_value(Item *item)
  {
    value= item->val_datetime_packed(current_thd);
    m_null_value= item->null_value;
  }
  int cmp_not_null(const Value *val);
  int cmp(Item *arg);
  cmp_item *make_same(THD *thd);
};


class cmp_item_time: public cmp_item_temporal
{
public:
  cmp_item_time()
   :cmp_item_temporal()
  { }
  void store_value(Item *item)
  {
    value= item->val_time_packed(current_thd);
    m_null_value= item->null_value;
  }
  int cmp_not_null(const Value *val);
  int cmp(Item *arg);
  cmp_item *make_same(THD *thd);
};


class cmp_item_timestamp: public cmp_item_scalar
{
  Timestamp_or_zero_datetime_native m_native;
public:
  cmp_item_timestamp() :cmp_item_scalar() { }
  void store_value(Item *item);
  int cmp_not_null(const Value *val);
  int cmp(Item *arg);
  int compare(cmp_item *ci);
  cmp_item *make_same(THD *thd);
};


class cmp_item_real : public cmp_item_scalar
{
  double value;
public:
  cmp_item_real() {}                          /* Remove gcc warning */
  void store_value(Item *item)
  {
    value= item->val_real();
    m_null_value= item->null_value;
  }
  int cmp_not_null(const Value *val)
  {
    DBUG_ASSERT(!val->is_null());
    DBUG_ASSERT(val->is_double());
    return value != val->value.m_double;
  }
  int cmp(Item *arg)
  {
    const bool rc= value != arg->val_real();
    return (m_null_value || arg->null_value) ? UNKNOWN : rc;
  }
  int compare(cmp_item *ci)
  {
    cmp_item_real *l_cmp= (cmp_item_real *) ci;
    return (value < l_cmp->value)? -1 : ((value == l_cmp->value) ? 0 : 1);
  }
  cmp_item *make_same(THD *thd);
};


class cmp_item_decimal : public cmp_item_scalar
{
  my_decimal value;
public:
  cmp_item_decimal() {}                       /* Remove gcc warning */
  void store_value(Item *item);
  int cmp(Item *arg);
  int cmp_not_null(const Value *val);
  int compare(cmp_item *c);
  cmp_item *make_same(THD *thd);
};


/* 
   cmp_item for optimized IN with row (right part string, which never
   be changed)
*/

class cmp_item_sort_string_in_static :public cmp_item_string
{
 protected:
  String value;
public:
  cmp_item_sort_string_in_static(CHARSET_INFO *cs):
    cmp_item_string(cs) {}
  void store_value(Item *item)
  {
    value_res= item->val_str(&value);
    m_null_value= item->null_value;
  }
  int cmp_not_null(const Value *val)
  {
    DBUG_ASSERT(false);
    return TRUE;
  }
  int cmp(Item *item)
  {
    // Should never be called
    DBUG_ASSERT(false);
    return TRUE;
  }
  int compare(cmp_item *ci)
  {
    cmp_item_string *l_cmp= (cmp_item_string *) ci;
    return sortcmp(value_res, l_cmp->value_res, cmp_charset);
  }
  cmp_item *make_same(THD *thd)
  {
    return new cmp_item_sort_string_in_static(cmp_charset);
  }
};


/**
  A helper class to handle situations when some item "pred" (the predicant)
  is consequently compared to a list of other items value0..valueN (the values).
  Currently used to handle:
  - <in predicate>
    pred IN (value0, value1, value2)
  - <simple case>
    CASE pred WHEN value0 .. WHEN value1 .. WHEN value2 .. END

  Every pair {pred,valueN} can be compared by its own Type_handler.
  Some pairs can use the same Type_handler.
  In cases when all pairs use exactly the same Type_handler,
  we say "all types are compatible".

  For example, for an expression
    1 IN (1, 1e0, 1.0, 2)
  - pred   is 1
  - value0 is 1
  - value1 is 1e0
  - value2 is 1.1
  - value3 is 2

  Pairs (pred,valueN) are compared as follows:
    N   expr1  Type
    -   -----  ----
    0       1   INT
    1     1e0   DOUBLE
    2     1.0   DECIMAL
    3       2   INT

  Types are not compatible in this example.

  During add_value() calls, each pair {pred,valueN} is analysed:
  - If valueN is an explicit NULL, it can be ignored in the caller asks to do so
  - If valueN is not an explicit NULL (or if the caller didn't ask to skip
    NULLs), then the value add an element in the array m_comparators[].

  Every element m_comparators[] stores the following information:
  1. m_arg_index - the position of the value expression in the original
     argument array, e.g. in Item_func_in::args[] or Item_func_case::args[].

  2. m_handler - the pointer to the data type handler that the owner
     will use to compare the pair {args[m_predicate_index],args[m_arg_index]}.

  3. m_handler_index - the index of an m_comparators[] element corresponding
     to the leftmost pair that uses exactly the same Type_handler for
     comparison. m_handler_index helps to maintain unique data type handlers.
   - m_comparators[i].m_handler_index==i means that this is the
     leftmost pair that uses the Type_handler m_handler for comparision.
   - If m_comparators[i].m_handlex_index!=i, it means that some earlier
     element m_comparators[j<i] is already using this Type_handler
     pointed by m_handler.

  4. m_cmp_item - the pointer to a cmp_item instance to handle comparison
     for this pair. Only unique type handlers have m_cmp_item!=NULL.
     Non-unique type handlers share the same cmp_item instance.
     For all m_comparators[] elements the following assersion it true:
       (m_handler_index==i) == (m_cmp_item!=NULL)
*/
class Predicant_to_list_comparator
{
  // Allocate memory on thd memory root for "nvalues" values.
  bool alloc_comparators(THD *thd, uint nvalues);

  /**
    Look up m_comparators[] for a comparator using the given data type handler.
    @param [OUT] idx     - the index of the found comparator is returned here
    @param [IN]  handler - the data type handler to find
    @param [IN]  count   - search in the range [0,count) only
    @retval true         - this type handler was not found
                           (*idx is not defined in this case).
    @retval false        - this type handler was found (the position of the
                           found handler is returned in idx).
  */
  bool find_handler(uint *idx, const Type_handler *handler, uint count)
  {
    DBUG_ASSERT(count < m_comparator_count);
    for (uint i= 0 ; i < count; i++)
    {
      if (m_comparators[i].m_handler == handler)
      {
        *idx= i;
        return false;
      }
    }
    return true;
  }

  /**
    Populate m_comparators[i].m_handler_index for all elements in
    m_comparators using the information in m_comparators[i].m_handlers,
    which was previously populated by a add_predicant() call and a number
    of add_value() calls.
    @param [OUT] compatible - If all comparator types are compatible,
                              their data type handler is returned here.
    @param [OUT] unuque_cnt - The number of unique data type handlers found.
                              If the value returned in *unique_cnt is 0,
                              it means all values were explicit NULLs:
                                expr0 IN (NULL,NULL,..,NULL)
    @param [OUT] found_type - The bit mask for all found cmp_type()'s.
  */
  void detect_unique_handlers(Type_handler_hybrid_field_type *compatible,
                              uint *unique_cnt, uint *found_types);
  /**
    Creates a cmp_item instances for all unique handlers and stores
    them into m_comparators[i].m_cmp_item, using the information previously
    populated by add_predicant(), add_value(), detect_unque_handlers().
  */

  /*
    Compare the predicant to the value pointed by m_comparators[i].
    @param  args    - the same argument array which was previously used
                      with add_predicant() and add_value().
    @param  i       - which pair to check.
    @retval true    - the predicant is not equal to the value.
    @retval false   - the predicant is equal to the value.
    @retval UNKNOWN - the result is uncertain yet because the predicant
                      and/or the value returned NULL,
                      more pairs {pred,valueN} should be checked.
  */
  int cmp_arg(Item_args *args, uint i)
  {
    Predicant_to_value_comparator *cmp=
      &m_comparators[m_comparators[i].m_handler_index];
    cmp_item *in_item= cmp->m_cmp_item;
    DBUG_ASSERT(in_item);
    /*
      If this is the leftmost pair that uses the data type handler
      pointed by m_comparators[i].m_handler, then we need to cache
      the predicant value representation used by this handler.
    */
    if (m_comparators[i].m_handler_index == i)
      in_item->store_value(args->arguments()[m_predicant_index]);
    /*
      If the predicant item has null_value==true then:
      - In case of scalar expression we can returns UNKNOWN immediately.
        No needs to check the result of the value item.
      - In case of ROW, null_value==true means that *some* row elements
        returned NULL, but *some* elements can still be non-NULL!
        We need to get the result of the value item and test
        if non-NULL elements in the predicant and the value produce
        TRUE (not equal), or UNKNOWN.
    */
    if (args->arguments()[m_predicant_index]->null_value &&
        m_comparators[i].m_handler != &type_handler_row)
      return UNKNOWN;
    return in_item->cmp(args->arguments()[m_comparators[i].m_arg_index]);
  }
  int cmp_args_nulls_equal(THD *thd, Item_args *args, uint i)
  {
    Predicant_to_value_comparator *cmp=
      &m_comparators[m_comparators[i].m_handler_index];
    cmp_item *in_item= cmp->m_cmp_item;
    DBUG_ASSERT(in_item);
    Item *predicant= args->arguments()[m_predicant_index];
    Item *arg= args->arguments()[m_comparators[i].m_arg_index];
    ValueBuffer<MAX_FIELD_WIDTH> val;
    if (m_comparators[i].m_handler_index == i)
      in_item->store_value(predicant);
    m_comparators[i].m_handler->Item_save_in_value(thd, arg, &val);
    if (predicant->null_value && val.is_null())
      return FALSE; // Two nulls are equal
    if (predicant->null_value || val.is_null())
      return UNKNOWN;
    return in_item->cmp_not_null(&val);
  }
  /**
    Predicant_to_value_comparator - a comparator for one pair (pred,valueN).
    See comments above.
  */
  struct Predicant_to_value_comparator
  {
    const Type_handler *m_handler;
    cmp_item *m_cmp_item;
    uint m_arg_index;
    uint m_handler_index;
    void cleanup()
    {
      if (m_cmp_item)
        delete m_cmp_item;
      memset(this, 0, sizeof(*this));
    }
  };

  Predicant_to_value_comparator *m_comparators; // The comparator array
  uint m_comparator_count;// The number of elements in m_comparators[]
  uint m_predicant_index; // The position of the predicant in its argument list,
                          // e.g. for Item_func_in m_predicant_index is 0,
                          // as predicant is stored in Item_func_in::args[0].
                          // For Item_func_case m_predicant_index is
                          // set to Item_func_case::first_expr_num.

public:
  Predicant_to_list_comparator(THD *thd, uint nvalues)
   :m_comparator_count(0),
    m_predicant_index(0)
  {
    alloc_comparators(thd, nvalues);
  }

  uint comparator_count() const { return m_comparator_count; }
  const Type_handler *get_comparator_type_handler(uint i) const
  {
    DBUG_ASSERT(i < m_comparator_count);
    return m_comparators[i].m_handler;
  }
  uint get_comparator_arg_index(uint i) const
  {
    DBUG_ASSERT(i < m_comparator_count);
    return m_comparators[i].m_arg_index;
  }
  cmp_item *get_comparator_cmp_item(uint i) const
  {
    DBUG_ASSERT(i < m_comparator_count);
    return m_comparators[i].m_cmp_item;
  }

#ifndef DBUG_OFF
  void debug_print(THD *thd)
  {
    for (uint i= 0; i < m_comparator_count; i++)
    {
      DBUG_EXECUTE_IF("Predicant_to_list_comparator",
                      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                      ER_UNKNOWN_ERROR, "DBUG: [%d] arg=%d handler=%d (%s)", i,
                      m_comparators[i].m_arg_index,
                      m_comparators[i].m_handler_index,
                      m_comparators[m_comparators[i].m_handler_index].
                                    m_handler->name().ptr()););
    }
  }
#endif

  void add_predicant(Item_args *args, uint predicant_index)
  {
    DBUG_ASSERT(m_comparator_count == 0); // Set in constructor
    DBUG_ASSERT(m_predicant_index == 0);  // Set in constructor
    DBUG_ASSERT(predicant_index < args->argument_count());
    m_predicant_index= predicant_index;
  }
  /**
    Add a new element into m_comparators[], using a {pred,valueN} pair.

    @param funcname        - the name of the operation, for error reporting
    @param args            - the owner function's argument list
    @param value_index     - the value position in args
    @retval true           - could not add an element because of non-comparable
                             arguments (e.g. ROWs with size)
    @retval false          - a new element was successfully added.
  */
  bool add_value(const LEX_CSTRING &funcname, Item_args *args,
                 uint value_index);

  /**
    Add a new element into m_comparators[], ignoring explicit NULL values.
    If the value appeared to be an explicit NULL, nulls_found[0] is set to true.
  */
  bool add_value_skip_null(const LEX_CSTRING &funcname,
                           Item_args *args, uint value_index,
                           bool *nulls_found);

  /**
    Signal "this" that there will be no new add_value*() calls,
    so it can prepare its internal structures for comparison.

    @param [OUT] compatible - If all comparators are compatible,
                              their data type handler is returned here.
    @param [OUT] unuque_cnt - The number of unique data type handlers found.
                              If the value returned in *unique_cnt is 0,
                              it means all values were explicit NULLs:
                                expr0 IN (NULL,NULL,..,NULL)
    @param [OUT] found_type - The bit mask for all found cmp_type()'s.
  */
  void all_values_added(Type_handler_hybrid_field_type *compatible,
                        uint *unique_cnt, uint *found_types)
  {
    detect_unique_handlers(compatible, unique_cnt, found_types);
  }
  /**
    Creates cmp_item instances for all unique handlers and stores
    them into m_comparators[].m_cmp_item, using the information previously
    populated by add_predicant(), add_value() and detect_unque_handlers().
  */
  bool make_unique_cmp_items(THD *thd, CHARSET_INFO *cs);
  void cleanup()
  {
    DBUG_ASSERT(m_comparators);
    for (uint i= 0; i < m_comparator_count; i++)
      m_comparators[i].cleanup();
    memset(m_comparators, 0, sizeof(m_comparators[0]) * m_comparator_count);
    m_comparator_count= 0;
    m_predicant_index= 0;
  }
  bool init_clone(THD *thd, uint nvalues)
  {
    m_comparator_count= 0;
    m_predicant_index= 0;
    return alloc_comparators(thd, nvalues);
  }
  /**
    @param [IN] args  - The argument list that was previously used with
                        add_predicant() and add_value().
    @param [OUT] idx  - In case if a value that is equal to the predicant
                        was found, the index of the matching value is returned
                        here. Otherwise, *idx is not changed.
    @param [IN/OUT] found_unknown_values - how to handle UNKNOWN results.
                        If found_unknown_values is NULL (e.g. Item_func_case),
                        cmp() returns immediately when the first UNKNOWN
                        result is found.
                        If found_unknown_values is non-NULL (Item_func_in),
                        cmp() does not return when an UNKNOWN result is found,
                        sets *found_unknown_values to true, and continues
                        to compare the remaining pairs to find FALSE
                        (i.e. the value that is equal to the predicant).

    @retval     false - Found a value that is equal to the predicant
    @retval     true  - Didn't find an equal value
  */
  bool cmp(Item_args *args, uint *idx, bool *found_unknown_values)
  {
    for (uint i= 0 ; i < m_comparator_count ; i++)
    {
      DBUG_ASSERT(m_comparators[i].m_handler != NULL);
      const int rc= cmp_arg(args, i);
      if (rc == FALSE)
      {
        *idx= m_comparators[i].m_arg_index;
        return false; // Found a matching value
      }
      if (rc == UNKNOWN)
      {
        if (!found_unknown_values)
          return true;
        *found_unknown_values= true;
      }
    }
    return true; // Not found
  }
  /*
    Same as above, but treats two NULLs as equal, e.g. as in DECODE_ORACLE().
  */
  bool cmp_nulls_equal(THD *thd, Item_args *args, uint *idx)
  {
    for (uint i= 0 ; i < m_comparator_count ; i++)
    {
      DBUG_ASSERT(m_comparators[i].m_handler != NULL);
      if (cmp_args_nulls_equal(thd, args, i) == FALSE)
      {
        *idx= m_comparators[i].m_arg_index;
        return false; // Found a matching value
      }
    }
    return true; // Not found
  }
};


/*
  The class Item_func_case is the CASE ... WHEN ... THEN ... END function
  implementation.
*/

class Item_func_case :public Item_func_case_expression
{
protected:
  String tmp_value;
  DTCollation cmp_collation;
  bool aggregate_then_and_else_arguments(THD *thd, uint count);
  virtual Item **else_expr_addr() const= 0;
  virtual Item *find_item()= 0;
  inline void print_when_then_arguments(String *str,
                                        enum_query_type query_type,
                                        Item **items, uint count);
  inline void print_else_argument(String *str, enum_query_type query_type,
                                  Item *item);
  void reorder_args(uint start);
public:
  Item_func_case(THD *thd, List<Item> &list)
   :Item_func_case_expression(thd, list)
  { }
  double real_op() override;
  longlong int_op() override;
  String *str_op(String *) override;
  my_decimal *decimal_op(my_decimal *) override;
  bool date_op(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool time_op(THD *thd, MYSQL_TIME *ltime) override;
  bool native_op(THD *thd, Native *to) override;
  bool fix_fields(THD *thd, Item **ref) override;
  table_map not_null_tables() const override { return 0; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("case") };
    return name;
  }
  CHARSET_INFO *compare_collation() const { return cmp_collation.collation; }
  bool need_parentheses_in_default() override { return true; }
};


/*
  CASE WHEN cond THEN res [WHEN cond THEN res...] [ELSE res] END

  Searched CASE checks all WHEN expressions one after another.
  When some WHEN expression evaluated to TRUE then the
  value of the corresponding THEN expression is returned.
*/
class Item_func_case_searched: public Item_func_case
{
  uint when_count() const { return arg_count / 2; }
  bool with_else() const { return arg_count % 2; }
  Item **else_expr_addr() const override
  { return with_else() ? &args[arg_count - 1] : 0; }
public:
  Item_func_case_searched(THD *thd, List<Item> &list)
   :Item_func_case(thd, list)
  {
    DBUG_ASSERT(arg_count >= 2);
    reorder_args(0);
  }
  enum Functype functype() const override { return CASE_SEARCHED_FUNC; }
  void print(String *str, enum_query_type query_type) override;
  bool fix_length_and_dec(THD *thd) override;
  Item *propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    // None of the arguments are in a comparison context
    Item_args::propagate_equal_fields(thd, Context_identity(), cond);
    return this;
  }
  Item *find_item() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_case_searched>(thd, this); }
};


/*
  CASE pred WHEN value THEN res [WHEN value THEN res...] [ELSE res] END

  When the predicant expression is specified then it is compared to each WHEN
  expression individually. When an equal WHEN expression is found
  the corresponding THEN expression is returned.
  In order to do correct comparisons several comparators are used. One for
  each result type. Different result types that are used in particular
  CASE ... END expression are collected in the fix_length_and_dec() member
  function and only comparators for there result types are used.
*/
class Item_func_case_simple: public Item_func_case,
                             public Predicant_to_list_comparator
{
protected:
  uint m_found_types;
  uint when_count() const { return (arg_count - 1) / 2; }
  bool with_else() const { return arg_count % 2 == 0; }
  Item **else_expr_addr() const override
  { return with_else() ? &args[arg_count - 1] : 0; }
  bool aggregate_switch_and_when_arguments(THD *thd, bool nulls_equal);
  bool prepare_predicant_and_values(THD *thd, uint *found_types,
                                              bool nulls_equal);
public:
  Item_func_case_simple(THD *thd, List<Item> &list)
   :Item_func_case(thd, list),
    Predicant_to_list_comparator(thd, arg_count),
    m_found_types(0)
  {
    DBUG_ASSERT(arg_count >= 3);
    reorder_args(1);
  }
  void cleanup() override
  {
    DBUG_ENTER("Item_func_case_simple::cleanup");
    Item_func::cleanup();
    Predicant_to_list_comparator::cleanup();
    DBUG_VOID_RETURN;
  }
  enum Functype functype() const override { return CASE_SIMPLE_FUNC; }
  void print(String *str, enum_query_type query_type) override;
  bool fix_length_and_dec(THD *thd) override;
  Item *propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override;
  Item *find_item() override;
  Item *build_clone(THD *thd) override
  {
    Item_func_case_simple *clone= (Item_func_case_simple *)
                                  Item_func_case::build_clone(thd);
    uint ncases= when_count();
    if (clone && clone->Predicant_to_list_comparator::init_clone(thd, ncases))
      return NULL;
    return clone;
  } 
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_case_simple>(thd, this); }
};


class Item_func_decode_oracle: public Item_func_case_simple
{
public:
  Item_func_decode_oracle(THD *thd, List<Item> &list)
   :Item_func_case_simple(thd, list)
  { }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("decode_oracle") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  bool fix_length_and_dec(THD *thd) override;
  Item *find_item() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_decode_oracle>(thd, this); }
};


/*
  The Item_func_in class implements
  in_expr IN (<in value list>)
  and
  in_expr NOT IN (<in value list>)

  The current implementation distinguishes 2 cases:
  1) all items in <in value list> are constants and have the same
    result type. This case is handled by in_vector class,
    implementing fast bisection search.
  2) otherwise Item_func_in employs several cmp_item objects to perform
    comparisons of in_expr and an item from <in value list>. One cmp_item
    object for each result type. Different result types are collected in the
    fix_length_and_dec() member function by means of collect_cmp_types()
    function.

  Bisection is possible when:
  1. All types are similar
  2. All expressions in <in value list> are const
  In the presence of NULLs, the correct result of evaluating this item
  must be UNKNOWN or FALSE. To achieve that:
  - If type is scalar, we can use bisection and the "have_null" boolean.
  - If type is ROW, we will need to scan all of <in value list> when
    searching, so bisection is impossible. Unless:
  3. UNKNOWN and FALSE are equivalent results
  4. Neither left expression nor <in value list> contain any NULL value
*/
class Item_func_in :public Item_func_opt_neg,
                    public Predicant_to_list_comparator
{
  /**
     Usable if <in value list> is made only of constants. Returns true if one
     of these constants contains a NULL. Example:
     IN ( (-5, (12,NULL)), ... ).
  */
  bool list_contains_null();
  bool all_items_are_consts(Item **items, uint nitems) const
  {
    for (uint i= 0; i < nitems; i++)
    {
      if (!items[i]->can_eval_in_optimize())
        return false;
    }
    return true;
  }
  bool prepare_predicant_and_values(THD *thd, uint *found_types);
  bool check_arguments() const override
  {
    return check_argument_types_like_args0();
  }
protected:
  SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                             Field *field, Item *value) override;
  bool transform_into_subq;
  bool transform_into_subq_checked;
public:
  /// An array of values, created when the bisection lookup method is used
  in_vector *array;
  /**
    If there is some NULL among <in value list>, during a val_int() call; for
    example
    IN ( (1,(3,'col')), ... ), where 'col' is a column which evaluates to
    NULL.
  */
  bool have_null;
  /**
    true when all arguments of the IN list are of compatible types
    and can be used safely as comparisons for key conditions
  */
  bool arg_types_compatible;

  TABLE_LIST *emb_on_expr_nest;

  Item_func_in(THD *thd, List<Item> &list):
    Item_func_opt_neg(thd, list),
    Predicant_to_list_comparator(thd, arg_count - 1),
    transform_into_subq(false),
    transform_into_subq_checked(false),
    array(0), have_null(0),
    arg_types_compatible(FALSE), emb_on_expr_nest(0)
  { }
  longlong val_int() override;
  bool fix_fields(THD *, Item **) override;
  bool fix_length_and_dec(THD *thd) override;
  bool compatible_types_scalar_bisection_possible()
  {
    DBUG_ASSERT(m_comparator.cmp_type() != ROW_RESULT);
    return all_items_are_consts(args + 1, arg_count - 1);     // Bisection #2
  }
  bool compatible_types_row_bisection_possible()
  {
    DBUG_ASSERT(m_comparator.cmp_type() == ROW_RESULT);
    return all_items_are_consts(args + 1, arg_count - 1) &&   // Bisection #2
           ((is_top_level_item() && !negated) ||              // Bisection #3
            (!list_contains_null() && !args[0]->maybe_null())); // Bisection #4
  }
  bool agg_all_arg_charsets_for_comparison()
  {
    return agg_arg_charsets_for_comparison(cmp_collation, args, arg_count);
  }
  void fix_in_vector();
  bool value_list_convert_const_to_int(THD *thd);
  bool fix_for_scalar_comparison_using_bisection(THD *thd)
  {
    array= m_comparator.type_handler()->make_in_vector(thd, this, arg_count - 1);
    if (!array)      // OOM
      return true;
    fix_in_vector();
    return false;
  }
  bool fix_for_scalar_comparison_using_cmp_items(THD *thd, uint found_types);

  bool fix_for_row_comparison_using_cmp_items(THD *thd);
  bool fix_for_row_comparison_using_bisection(THD *thd);

  void cleanup() override
  {
    DBUG_ENTER("Item_func_in::cleanup");
    Item_int_func::cleanup();
    delete array;
    array= 0;
    Predicant_to_list_comparator::cleanup();
    DBUG_VOID_RETURN;
  }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields, uint *and_level,
                      table_map usable_tables, SARGABLE_PARAM **sargables)
    override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override;
  SEL_TREE *get_func_row_mm_tree(RANGE_OPT_PARAM *param, Item_row *key_row); 
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    /*
      Note, we pass ANY_SUBST, this makes sure that non of the args
      will be replaced to a zero-filled Item_string.
      Such a change would require rebuilding of cmp_items.
    */
    if (arg_types_compatible)
    {
      Context cmpctx(ANY_SUBST, m_comparator.type_handler(),
                     Item_func_in::compare_collation());
      args[0]->propagate_equal_fields_and_change_item_tree(thd, cmpctx,
                                                           cond, &args[0]);
    }
    for (uint i= 0; i < comparator_count(); i++)
    {
      Context cmpctx(ANY_SUBST, get_comparator_type_handler(i),
                     Item_func_in::compare_collation());
      uint idx= get_comparator_arg_index(i);
      args[idx]->propagate_equal_fields_and_change_item_tree(thd, cmpctx,
                                                             cond, &args[idx]);
    }
    return this;
  }
  void print(String *str, enum_query_type query_type) override;
  enum Functype functype() const override { return IN_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("in") };
    return name;
  }
  enum precedence precedence() const override { return IN_PRECEDENCE; }
  bool eval_not_null_tables(void *opt_arg) override;
  bool find_not_null_fields(table_map allowed) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;
  bool count_sargable_conds(void *arg) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_in>(thd, this); }
  Item *build_clone(THD *thd) override
  {
    Item_func_in *clone= (Item_func_in *) Item_func::build_clone(thd);
    if (clone)
    {
      clone->array= 0;
      if (clone->Predicant_to_list_comparator::init_clone(thd, arg_count - 1))
        return NULL;
    }
    return clone;
  }
  void mark_as_condition_AND_part(TABLE_LIST *embedding) override;
  bool to_be_transformed_into_in_subq(THD *thd);
  bool create_value_list_for_tvc(THD *thd, List< List<Item> > *values);
  Item *in_predicate_to_in_subs_transformer(THD *thd, uchar *arg) override;
  Item *in_predicate_to_equality_transformer(THD *thd, uchar *arg) override;
  uint32 max_length_of_left_expr();
};

class cmp_item_row :public cmp_item
{
  cmp_item **comparators;
  uint n;
  bool alloc_comparators(THD *thd, uint n);
  bool aggregate_row_elements_for_comparison(THD *thd,
                                             Type_handler_hybrid_field_type *cmp,
                                             Item_args *tmp,
                                             const LEX_CSTRING &funcname,
                                             uint col,
                                             uint level);
public:
  cmp_item_row(): comparators(0), n(0) {}
  ~cmp_item_row();
  void store_value(Item *item);
  bool prepare_comparators(THD *, const LEX_CSTRING &funcname,
                           const Item_args *args, uint level);
  int cmp(Item *arg);
  int cmp_not_null(const Value *val)
  {
    DBUG_ASSERT(false);
    return TRUE;
  }
  int compare(cmp_item *arg);
  cmp_item *make_same(THD *thd);
  void store_value_by_template(THD *thd, cmp_item *tmpl, Item *);
  friend class Item_func_in;
  cmp_item *get_comparator(uint i) { return comparators[i]; }
};


class in_row :public in_vector
{
  cmp_item_row tmp;
public:
  in_row(THD *thd, uint elements, Item *);
  ~in_row();
  void set(uint pos,Item *item) override;
  uchar *get_value(Item *item) override;
  friend class Item_func_in;
  const Type_handler *type_handler() const override { return &type_handler_row; }
  cmp_item *get_cmp_item() { return &tmp; }
};

/* Functions used by where clause */
class Item_func_null_predicate :public Item_bool_func
{
protected:
  SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                             Field *field, Item *value) override
  {
    DBUG_ENTER("Item_func_null_predicate::get_func_mm_tree");
    DBUG_RETURN(get_mm_parts(param, field, functype(), value));
  }
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, Field *field,
                       KEY_PART *key_part,
                       Item_func::Functype type, Item *value) override;
public:
  Item_func_null_predicate(THD *thd, Item *a): Item_bool_func(thd, a) { }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields, uint *and_level,
                      table_map usable_tables, SARGABLE_PARAM **sargables)
    override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override
  {
    DBUG_ENTER("Item_func_null_predicate::get_mm_tree");
    SEL_TREE *ftree= get_full_func_mm_tree_for_args(param, args[0], NULL);
    if (!ftree)
      ftree= Item_func::get_mm_tree(param, cond_ptr);
    DBUG_RETURN(ftree);
  }
  CHARSET_INFO *compare_collation() const override
  { return args[0]->collation.collation; }
  bool fix_length_and_dec(THD *thd) override
  {
    decimals=0;
    max_length=1;
    base_flags&= ~item_base_t::MAYBE_NULL;
    return FALSE;
  }
  bool count_sargable_conds(void *arg) override;
};


class Item_func_isnull :public Item_func_null_predicate
{
public:
  Item_func_isnull(THD *thd, Item *a): Item_func_null_predicate(thd, a) {}
  longlong val_int() override;
  enum Functype functype() const override { return ISNULL_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("isnull") };
    return name;
  }
  void print(String *str, enum_query_type query_type) override;
  enum precedence precedence() const override { return CMP_PRECEDENCE; }

  bool arg_is_datetime_notnull_field()
  {
    Item **args= arguments();
    if (args[0]->real_item()->type() == Item::FIELD_ITEM)
    {
      Field *field=((Item_field*) args[0]->real_item())->field;

      if ((field->flags & NOT_NULL_FLAG) &&
          field->type_handler()->cond_notnull_field_isnull_to_field_eq_zero())
        return true;
    }
    return false;
  }

  /* Optimize case of not_null_column IS NULL */
  void update_used_tables() override
  {
    if (!args[0]->maybe_null() && !arg_is_datetime_notnull_field())
    {
      used_tables_cache= 0;			/* is always false */
      const_item_cache= 1;
    }
    else
    {
      args[0]->update_used_tables();
      used_tables_cache= args[0]->used_tables();
      const_item_cache= args[0]->const_item();
    }
  }
  COND *remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                        bool top_level) override;
  table_map not_null_tables() const override { return 0; }
  bool find_not_null_fields(table_map allowed) override;
  Item *neg_transformer(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_isnull>(thd, this); }
};

/* Functions used by HAVING for rewriting IN subquery */

class Item_in_subselect;

/* 
  This is like IS NOT NULL but it also remembers if it ever has
  encountered a NULL.
*/
class Item_is_not_null_test :public Item_func_isnull
{
  Item_in_subselect* owner;
public:
  Item_is_not_null_test(THD *thd, Item_in_subselect* ow, Item *a):
    Item_func_isnull(thd, a), owner(ow)
  {}
  enum Functype functype() const override { return ISNOTNULLTEST_FUNC; }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("<is_not_null_test>") };
    return name;
  }
  void update_used_tables() override;
  /*
    we add RAND_TABLE_BIT to prevent moving this item from HAVING to WHERE
  */
  table_map used_tables() const override
    { return used_tables_cache | RAND_TABLE_BIT; }
  bool const_item() const override { return FALSE; }
};


class Item_func_isnotnull :public Item_func_null_predicate
{
public:
  Item_func_isnotnull(THD *thd, Item *a):
  Item_func_null_predicate(thd, a) {}
  longlong val_int() override;
  enum Functype functype() const override { return ISNOTNULL_FUNC; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("isnotnull") };
    return name;
  }
  enum precedence precedence() const override { return CMP_PRECEDENCE; }
  table_map not_null_tables() const override
  { return is_top_level_item() ? not_null_tables_cache : 0; }
  Item *neg_transformer(THD *thd) override;
  void print(String *str, enum_query_type query_type) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_isnotnull>(thd, this); }
};


class Item_func_like :public Item_bool_func2
{
  // Turbo Boyer-Moore data
  bool        canDoTurboBM;	// pattern is '%abcd%' case
  const char* pattern;
  int         pattern_len;

  // TurboBM buffers, *this is owner
  int* bmGs; //   good suffix shift table, size is pattern_len + 1
  int* bmBc; // bad character shift table, size is alphabet_size

  void turboBM_compute_suffixes(int* suff);
  void turboBM_compute_good_suffix_shifts(int* suff);
  void turboBM_compute_bad_character_shifts();
  bool turboBM_matches(const char* text, int text_len) const;
  enum { alphabet_size = 256 };

  Item *escape_item;

  bool escape_used_in_parsing;
  bool use_sampling;

  DTCollation cmp_collation;
  String cmp_value1, cmp_value2;
  bool with_sargable_pattern() const;
protected:
  SEL_TREE *get_func_mm_tree(RANGE_OPT_PARAM *param,
                             Field *field, Item *value) override
  {
    DBUG_ENTER("Item_func_like::get_func_mm_tree");
    DBUG_RETURN(get_mm_parts(param, field, LIKE_FUNC, value));
  }
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, Field *field,
                       KEY_PART *key_part,
                       Item_func::Functype type, Item *value) override;
public:
  int escape;
  bool negated;

  Item_func_like(THD *thd, Item *a, Item *b, Item *escape_arg, bool escape_used):
    Item_bool_func2(thd, a, b), canDoTurboBM(FALSE), pattern(0), pattern_len(0),
    bmGs(0), bmBc(0), escape_item(escape_arg),
    escape_used_in_parsing(escape_used), use_sampling(0), negated(0) {}

  bool get_negated() const { return negated; } // Used by ColumnStore

  Sql_mode_dependency value_depends_on_sql_mode() const override;
  longlong val_int() override;
  enum Functype functype() const override { return LIKE_FUNC; }
  void print(String *str, enum_query_type query_type) override;
  CHARSET_INFO *compare_collation() const override
  { return cmp_collation.collation; }
  cond_result eq_cmp_result() const override
  {
    /**
      We cannot always rewrite conditions as follows:
        from:  WHERE expr1=const AND expr1 LIKE expr2
        to:    WHERE expr1=const AND const LIKE expr2
      or
        from:  WHERE expr1=const AND expr2 LIKE expr1
        to:    WHERE expr1=const AND expr2 LIKE const

      because LIKE works differently comparing to the regular "=" operator:

      1. LIKE performs a stricter one-character-to-one-character comparison
         and does not recognize contractions and expansions.
         Replacing "expr1" to "const in LIKE would make the condition
         stricter in case of a complex collation.

      2. LIKE does not ignore trailing spaces and thus works differently
         from the "=" operator in case of "PAD SPACE" collations
         (which are the majority in MariaDB). So, for "PAD SPACE" collations:

         - expr1=const       - ignores trailing spaces
         - const LIKE expr2  - does not ignore trailing spaces
         - expr2 LIKE const  - does not ignore trailing spaces

      Allow only "binary" for now.
      It neither ignores trailing spaces nor has contractions/expansions.

      TODO:
      We could still replace "expr1" to "const" in "expr1 LIKE expr2"
      in case of a "PAD SPACE" collation, but only if "expr2" has '%'
      at the end.         
    */
    return compare_collation() == &my_charset_bin ? COND_TRUE : COND_OK;
  }
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields, uint *and_level,
                      table_map usable_tables, SARGABLE_PARAM **sargables)
    override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override;
  Item* propagate_equal_fields(THD *thd, const Context &ctx, COND_EQUAL *cond)
    override
  {
    /*
      LIKE differs from the regular comparison operator ('=') in the following:
      - LIKE never ignores trailing spaces (even for PAD SPACE collations)
        Propagation of equal fields with a PAD SPACE collation into LIKE
        is not safe.
        Example:
          WHERE a='a ' AND a LIKE 'a'     - returns true for 'a'
        cannot be rewritten to:
          WHERE a='a ' AND 'a ' LIKE 'a'  - returns false for 'a'
        Note, binary collations in MySQL/MariaDB, e.g. latin1_bin,
        still have the PAD SPACE attribute and ignore trailing spaces!
      - LIKE does not take into account contractions, expansions,
        and ignorable characters.
        Propagation of equal fields with contractions/expansions/ignorables
        is also not safe.

      It's safe to propagate my_charset_bin (BINARY/VARBINARY/BLOB) values,
      because they do not ignore trailing spaces and have one-to-one mapping
      between a string and its weights.
      The below condition should be true only for my_charset_bin
      (as of version 10.1.7).
    */
    uint flags= Item_func_like::compare_collation()->state;
    if ((flags & MY_CS_NOPAD) && !(flags & MY_CS_NON1TO1))
      Item_args::propagate_equal_fields(thd,
                                        Context(ANY_SUBST,
                                                &type_handler_long_blob,
                                                compare_collation()),
                                        cond);
    return this;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("like") };
    return name;
  }
  enum precedence precedence() const override { return IN_PRECEDENCE; }
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= 1;
    return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
  }
  void cleanup() override;

  Item *neg_transformer(THD *thd) override
  {
    negated= !negated;
    return this;
  }

  bool walk(Item_processor processor, bool walk_subquery, void *arg) override
  {
    return (walk_args(processor, walk_subquery, arg) ||
            escape_item->walk(processor, walk_subquery, arg) ||
            (this->*processor)(arg));
  }

  bool find_selective_predicates_list_processor(void *arg) override;
  
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_like>(thd, this); }
};


typedef struct pcre2_real_code_8 pcre2_code;
typedef struct pcre2_real_match_data_8 pcre2_match_data;
#define PCRE2_SIZE size_t
class Regexp_processor_pcre
{
  pcre2_code *m_pcre;
  pcre2_match_data *m_pcre_match_data;
  bool m_conversion_is_needed;
  bool m_is_const;
  int m_library_flags;
  CHARSET_INFO *m_library_charset;
  String m_prev_pattern;
  int m_pcre_exec_rc;
  PCRE2_SIZE *m_SubStrVec;
  void pcre_exec_warn(int rc) const;
  int pcre_exec_with_warn(const pcre2_code *code,
                          pcre2_match_data *data,
                          const char *subject, int length, int startoffset,
                          int options);
public:
  String *convert_if_needed(String *src, String *converter);
  String subject_converter;
  String pattern_converter;
  String replace_converter;
  Regexp_processor_pcre() :
    m_pcre(NULL), m_pcre_match_data(NULL),
    m_conversion_is_needed(true), m_is_const(0),
    m_library_flags(0),
    m_library_charset(&my_charset_utf8mb3_general_ci)
  {}
  int default_regex_flags();
  void init(CHARSET_INFO *data_charset, int extra_flags);
  void fix_owner(Item_func *owner, Item *subject_arg, Item *pattern_arg);
  bool compile(String *pattern, bool send_error);
  bool compile(Item *item, bool send_error);
  bool recompile(Item *item)
  {
    return !m_is_const && compile(item, false);
  }
  bool exec(const char *str, size_t length, size_t offset);
  bool exec(String *str, int offset, uint n_result_offsets_to_convert);
  bool exec(Item *item, int offset, uint n_result_offsets_to_convert);
  bool match() const { return m_pcre_exec_rc < 0 ? 0 : 1; }
  int nsubpatterns() const { return m_pcre_exec_rc <= 0 ? 0 : m_pcre_exec_rc; }
  size_t subpattern_start(int n) const
  {
    return m_pcre_exec_rc <= 0 ? 0 : m_SubStrVec[n * 2];
  }
  size_t subpattern_end(int n) const
  {
    return m_pcre_exec_rc <= 0 ? 0 : m_SubStrVec[n * 2 + 1];
  }
  size_t subpattern_length(int n) const
  {
    return subpattern_end(n) - subpattern_start(n);
  }
  void reset()
  {
    m_pcre= NULL;
    m_pcre_match_data= NULL;
    m_prev_pattern.length(0);
  }
  void cleanup();
  bool is_compiled() const { return m_pcre != NULL; }
  bool is_const() const { return m_is_const; }
  void set_const(bool arg) { m_is_const= arg; }
  CHARSET_INFO * library_charset() const { return m_library_charset; }
};


class Item_func_regex :public Item_bool_func
{
  Regexp_processor_pcre re;
  DTCollation cmp_collation;
public:
  Item_func_regex(THD *thd, Item *a, Item *b): Item_bool_func(thd, a, b)
  {}
  void cleanup() override
  {
    DBUG_ENTER("Item_func_regex::cleanup");
    Item_bool_func::cleanup();
    re.cleanup();
    DBUG_VOID_RETURN;
  }
  longlong val_int() override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("regexp") };
    return name;
  }
  enum precedence precedence() const override { return IN_PRECEDENCE; }
  Item *get_copy(THD *) override { return 0; }
  void print(String *str, enum_query_type query_type) override
  {
    print_op(str, query_type);
  }

  CHARSET_INFO *compare_collation() const override
  { return cmp_collation.collation; }
};


/*
  In the corner case REGEXP_INSTR could return (2^32 + 1),
  which would not fit into Item_long_func range.
  But string lengths are limited with max_allowed_packet,
  which cannot be bigger than 1024*1024*1024.
*/
class Item_func_regexp_instr :public Item_long_func
{
  bool check_arguments() const override
  {
    return (args[0]->check_type_can_return_str(func_name_cstring()) ||
            args[1]->check_type_can_return_text(func_name_cstring()));
  }
  Regexp_processor_pcre re;
  DTCollation cmp_collation;
public:
  Item_func_regexp_instr(THD *thd, Item *a, Item *b)
   :Item_long_func(thd, a, b)
  {}
  void cleanup() override
  {
    DBUG_ENTER("Item_func_regexp_instr::cleanup");
    Item_int_func::cleanup();
    re.cleanup();
    DBUG_VOID_RETURN;
  }
  longlong val_int() override;
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("regexp_instr") };
    return name;
  }
  Item *get_copy(THD *thd) override { return 0; }
};


typedef class Item COND;

class Item_cond :public Item_bool_func
{
protected:
  List<Item> list;
  table_map and_tables_cache;

public:
  Item_cond(THD *thd): Item_bool_func(thd)
  {
    /* Item_cond() is only used to create top level items */
    top_level_item();
    const_item_cache=0;
  }
  Item_cond(THD *thd, Item *i1, Item *i2);
  Item_cond(THD *thd, Item_cond *item);
  Item_cond(THD *thd, List<Item> &nlist):
    Item_bool_func(thd), list(nlist) {}
  bool add(Item *item, MEM_ROOT *root)
  {
    DBUG_ASSERT(item);
    return list.push_back(item, root);
  }
  bool add_at_head(Item *item, MEM_ROOT *root)
  {
    DBUG_ASSERT(item);
    return list.push_front(item, root);
  }
  void add_at_head(List<Item> *nlist)
  {
    DBUG_ASSERT(nlist->elements);
    list.prepend(nlist);
  }
  void add_at_end(List<Item> *nlist)
  {
    DBUG_ASSERT(nlist->elements);
    list.append(nlist);
  }
  bool fix_fields(THD *, Item **ref) override;
  void fix_after_pullout(st_select_lex *new_parent, Item **ref, bool merge)
    override;

  enum Type type() const override { return COND_ITEM; }
  List<Item>* argument_list() { return &list; }
  table_map used_tables() const override;
  void update_used_tables() override
  {
    used_tables_and_const_cache_init();
    used_tables_and_const_cache_update_and_join(list);
  }
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override;
  COND *remove_eq_conds(THD *thd, Item::cond_result *cond_value,
                        bool top_level) override;
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override;
  void print(String *str, enum_query_type query_type) override;
  void split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                      List<Item> &fields, uint flags) override;
  friend int setup_conds(THD *thd, TABLE_LIST *tables, TABLE_LIST *leaves,
                         COND **conds);
  void copy_andor_arguments(THD *thd, Item_cond *item);
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override;
  Item *do_transform(THD *thd, Item_transformer transformer, uchar *arg,
                     bool toplevel);
  Item *transform(THD *thd, Item_transformer transformer, uchar *arg) override
  {
    return do_transform(thd, transformer, arg, 0);
  }
  Item *top_level_transform(THD *thd, Item_transformer transformer, uchar *arg)
    override
  {
    return do_transform(thd, transformer, arg, 1);
  }
  void traverse_cond(Cond_traverser, void *arg, traverse_order order) override;
  void neg_arguments(THD *thd);
  Item* propagate_equal_fields(THD *, const Context &, COND_EQUAL *) override;
  Item *do_compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                   Item_transformer transformer, uchar *arg_t, bool toplevel);
  Item *compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                Item_transformer transformer, uchar *arg_t) override
  {
    return do_compile(thd, analyzer, arg_p, transformer, arg_t, 0);
  }
  Item* top_level_compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                          Item_transformer transformer, uchar *arg_t) override
  {
    return do_compile(thd, analyzer, arg_p, transformer, arg_t, 1);
  }
  bool eval_not_null_tables(void *opt_arg) override;
  bool find_not_null_fields(table_map allowed) override;
  Item *build_clone(THD *thd) override;
  bool excl_dep_on_table(table_map tab_map) override;
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override;

private:
  void merge_sub_condition(List_iterator<Item>& li);
};

template <template<class> class LI, class T> class Item_equal_iterator;

/*
  The class Item_equal is used to represent conjunctions of equality
  predicates of the form field1 = field2, and field=const in where
  conditions and on expressions.

  All equality predicates of the form field1=field2 contained in a
  conjunction are substituted for a sequence of items of this class.
  An item of this class Item_equal(f1,f2,...fk) represents a
  multiple equality f1=f2=...=fk.l

  If a conjunction contains predicates f1=f2 and f2=f3, a new item of
  this class is created Item_equal(f1,f2,f3) representing the multiple
  equality f1=f2=f3 that substitutes the above equality predicates in
  the conjunction.
  A conjunction of the predicates f2=f1 and f3=f1 and f3=f2 will be
  substituted for the item representing the same multiple equality
  f1=f2=f3.
  An item Item_equal(f1,f2) can appear instead of a conjunction of 
  f2=f1 and f1=f2, or instead of just the predicate f1=f2.

  An item of the class Item_equal inherits equalities from outer 
  conjunctive levels.

  Suppose we have a where condition of the following form:
  WHERE f1=f2 AND f3=f4 AND f3=f5 AND ... AND (...OR (f1=f3 AND ...)).
  In this case:
    f1=f2 will be substituted for Item_equal(f1,f2);
    f3=f4 and f3=f5  will be substituted for Item_equal(f3,f4,f5);
    f1=f3 will be substituted for Item_equal(f1,f2,f3,f4,f5);

  An object of the class Item_equal can contain an optional constant
  item c. Then it represents a multiple equality of the form 
  c=f1=...=fk.

  Objects of the class Item_equal are used for the following:

  1. An object Item_equal(t1.f1,...,tk.fk) allows us to consider any
  pair of tables ti and tj as joined by an equi-condition.
  Thus it provide us with additional access paths from table to table.

  2. An object Item_equal(t1.f1,...,tk.fk) is applied to deduce new
  SARGable predicates:
    f1=...=fk AND P(fi) => f1=...=fk AND P(fi) AND P(fj).
  It also can give us additional index scans and can allow us to
  improve selectivity estimates.

  3. An object Item_equal(t1.f1,...,tk.fk) is used to optimize the 
  selected execution plan for the query: if table ti is accessed 
  before the table tj then in any predicate P in the where condition
  the occurrence of tj.fj is substituted for ti.fi. This can allow
  an evaluation of the predicate at an earlier step.

  When feature 1 is supported they say that join transitive closure 
  is employed.
  When feature 2 is supported they say that search argument transitive
  closure is employed.
  Both features are usually supported by preprocessing original query and
  adding additional predicates.
  We do not just add predicates, we rather dynamically replace some
  predicates that can not be used to access tables in the investigated
  plan for those, obtained by substitution of some fields for equal fields,
  that can be used.     

  Prepared Statements/Stored Procedures note: instances of class
  Item_equal are created only at the time a PS/SP is executed and
  are deleted in the end of execution. All changes made to these
  objects need not be registered in the list of changes of the parse
  tree and do not harm PS/SP re-execution.

  Item equal objects are employed only at the optimize phase. Usually they are
  not supposed to be evaluated.  Yet in some cases we call the method val_int()
  for them. We have to take care of restricting the predicate such an
  object represents f1=f2= ...=fn to the projection of known fields fi1=...=fik.
*/

class Item_equal: public Item_bool_func
{
  /*
    The list of equal items. Currently the list can contain:
     - Item_fields items for references to table columns
     - Item_direct_view_ref items for references to view columns
     - one const item

    If the list contains a constant item this item is always first in the list.
    The list contains at least two elements.
    Currently all Item_fields/Item_direct_view_ref items in the list should
    refer to table columns with equavalent type definitions. In particular
    if these are string columns they should have the same charset/collation.

    Use objects of the companion class Item_equal_fields_iterator to iterate
    over all items from the list of the Item_field/Item_direct_view_ref classes.
  */ 
  List<Item> equal_items; 
  /* 
     TRUE <-> one of the items is a const item.
     Such item is always first in in the equal_items list
  */
  bool with_const;        
  /* 
    The field eval_item is used when this item is evaluated
    with the method val_int()
  */  
  cmp_item *eval_item;
  /*
    This initially is set to FALSE. It becomes TRUE when this item is evaluated
    as being always false. If the flag is TRUE the contents of the list 
    the equal_items should be ignored.
  */
  bool cond_false;
  /*
    This initially is set to FALSE. It becomes TRUE when this item is evaluated
    as being always true. If the flag is TRUE the contents of the list 
    the equal_items should be ignored.
  */
  bool cond_true;
  /*
    For Item_equal objects inside an OR clause: one of the fields that were
    used in the original equality.
  */
  Item_field *context_field;

  bool link_equal_fields;

  const Type_handler *m_compare_handler;
  CHARSET_INFO *m_compare_collation;
public:

  COND_EQUAL *upper_levels;       /* multiple equalities of upper and levels */

  Item_equal(THD *thd, const Type_handler *handler,
             Item *f1, Item *f2, bool with_const_item);
  Item_equal(THD *thd, Item_equal *item_equal);
  /* Currently the const item is always the first in the list of equal items */
  inline Item* get_const() { return with_const ? equal_items.head() : NULL; }
  void add_const(THD *thd, Item *c);
  /** Add a non-constant item to the multiple equality */
  void add(Item *f, MEM_ROOT *root) { equal_items.push_back(f, root); }
  bool contains(Field *field);
  Item* get_first(struct st_join_table *context, Item *field);
  /** Get number of field items / references to field items in this object */   
  uint n_field_items() { return equal_items.elements - MY_TEST(with_const); }
  void merge(THD *thd, Item_equal *item);
  bool merge_with_check(THD *thd, Item_equal *equal_item, bool save_merged);
  void merge_into_list(THD *thd, List<Item_equal> *list, bool save_merged,
                      bool only_intersected);
  void update_const(THD *thd);
  enum Functype functype() const override { return MULT_EQUAL_FUNC; }
  longlong val_int() override; 
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("multiple equal") };
    return name;
  }
  void sort(Item_field_cmpfunc compare, void *arg);
  bool fix_length_and_dec(THD *thd) override;
  bool fix_fields(THD *thd, Item **ref) override;
  void cleanup() override
  {
    delete eval_item;
    eval_item= NULL;
  }
  void update_used_tables() override;
  bool find_not_null_fields(table_map allowed) override;
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override;
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields,
                      uint *and_level, table_map usable_tables,
                      SARGABLE_PARAM **sargables) override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override;
  bool walk(Item_processor processor, bool walk_subquery, void *arg) override;
  Item *transform(THD *thd, Item_transformer transformer, uchar *arg) override;
  void print(String *str, enum_query_type query_type) override;
  const Type_handler *compare_type_handler() const { return m_compare_handler; }
  CHARSET_INFO *compare_collation() const override
  { return m_compare_collation; }

  void set_context_field(Item_field *ctx_field) { context_field= ctx_field; }
  void set_link_equal_fields(bool flag) { link_equal_fields= flag; }
  Item* get_copy(THD *thd) override { return 0; }
  /*
    This does not comply with the specification of the virtual method,
    but Item_equal items are processed distinguishly anyway
  */
  bool excl_dep_on_table(table_map tab_map) override
  {
    return used_tables() & tab_map;
  }
  bool excl_dep_on_in_subq_left_part(Item_in_subselect *subq_pred) override;
  bool excl_dep_on_grouping_fields(st_select_lex *sel) override;
  bool create_pushable_equalities(THD *thd, List<Item> *equalities,
                                  Pushdown_checker checker, uchar *arg,
                                  bool clone_const);
  /* Return the number of elements in this multiple equality */
  uint elements_count() { return equal_items.elements; }
  friend class Item_equal_fields_iterator;
  bool count_sargable_conds(void *arg) override;
  Item *multiple_equality_transformer(THD *thd, uchar *arg) override;
  friend class Item_equal_iterator<List_iterator_fast,Item>;
  friend class Item_equal_iterator<List_iterator,Item>;
  friend Item *eliminate_item_equal(THD *thd, COND *cond,
                                    COND_EQUAL *upper_levels,
                                    Item_equal *item_equal);
  friend bool setup_sj_materialization_part1(struct st_join_table *tab);
  friend bool setup_sj_materialization_part2(struct st_join_table *tab);
}; 

class COND_EQUAL: public Sql_alloc
{
public:
  uint max_members;               /* max number of members the current level
                                     list and all lower level lists */ 
  COND_EQUAL *upper_levels;       /* multiple equalities of upper and levels */
  List<Item_equal> current_level; /* list of multiple equalities of 
                                     the current and level           */
  COND_EQUAL()
  { 
    upper_levels= 0;
  }
  COND_EQUAL(Item_equal *item, MEM_ROOT *mem_root)
   :upper_levels(0)
  {
    current_level.push_back(item, mem_root);
  }
  void copy(COND_EQUAL &cond_equal)
  {
    max_members= cond_equal.max_members;
    upper_levels= cond_equal.upper_levels;
    if (cond_equal.current_level.is_empty())
      current_level.empty();
    else
      current_level= cond_equal.current_level;
  }
  bool is_empty()
  {
    return (current_level.elements == 0);
  }
};


/* 
  The template Item_equal_iterator is used to define classes
  Item_equal_fields_iterator and Item_equal_fields_iterator_slow.
  These are helper classes for the class Item equal
  Both classes are used to iterate over references to table/view columns
  from the list of equal items that included in an Item_equal object. 
  The second class supports the operation of removal of the current member
  from the list when performing an iteration.
*/ 

template <template<class> class LI, typename T> class Item_equal_iterator
  : public LI<T>
{
protected:
  Item_equal *item_equal;
  Item *curr_item;
public:
  Item_equal_iterator(Item_equal &item_eq)
    :LI<T> (item_eq.equal_items)
  {
    curr_item= NULL;
    item_equal= &item_eq;
    if (item_eq.with_const)
    {
      LI<T> *list_it= this;
      curr_item=  (*list_it)++;
    }
  }
  Item* operator++(int)
  { 
    LI<T> *list_it= this;
    curr_item= (*list_it)++;
    return curr_item;
  }
  void rewind(void) 
  { 
    LI<T> *list_it= this;
    list_it->rewind();
    if (item_equal->with_const)
      curr_item= (*list_it)++;
  }  
  Field *get_curr_field()
  {
    Item_field *item= (Item_field *) (curr_item->real_item());
     return item->field;
  }  
};

typedef  Item_equal_iterator<List_iterator_fast,Item >  Item_equal_iterator_fast;

class Item_equal_fields_iterator
  :public Item_equal_iterator_fast
{
public:
  Item_equal_fields_iterator(Item_equal &item_eq) 
    :Item_equal_iterator_fast(item_eq)
  { }
  Item ** ref()
  {
    return List_iterator_fast<Item>::ref();
  }
};

typedef Item_equal_iterator<List_iterator,Item > Item_equal_iterator_iterator_slow;

class Item_equal_fields_iterator_slow
  :public Item_equal_iterator_iterator_slow
{
public:
  Item_equal_fields_iterator_slow(Item_equal &item_eq) 
    :Item_equal_iterator_iterator_slow(item_eq)
  { }
  void remove()
  {
    List_iterator<Item>::remove();
  }
};


class Item_cond_and final :public Item_cond
{
public:
  COND_EQUAL m_cond_equal;  /* contains list of Item_equal objects for 
                               the current and level and reference
                               to multiple equalities of upper and levels */  
  Item_cond_and(THD *thd): Item_cond(thd) {}
  Item_cond_and(THD *thd, Item *i1,Item *i2): Item_cond(thd, i1, i2) {}
  Item_cond_and(THD *thd, Item_cond_and *item): Item_cond(thd, item) {}
  Item_cond_and(THD *thd, List<Item> &list_arg): Item_cond(thd, list_arg) {}
  enum Functype functype() const override { return COND_AND_FUNC; }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("and") };
    return name;
  }
  enum precedence precedence() const override { return AND_PRECEDENCE; }
  table_map not_null_tables() const override
  { return is_top_level_item() ? not_null_tables_cache: and_tables_cache; }
  Item *copy_andor_structure(THD *thd) override;
  Item *neg_transformer(THD *thd) override;
  void mark_as_condition_AND_part(TABLE_LIST *embedding) override;
  uint exists2in_reserved_items() override { return list.elements; };
  COND *build_equal_items(THD *thd, COND_EQUAL *inherited,
                          bool link_item_fields,
                          COND_EQUAL **cond_equal_ref) override;
  bool set_format_by_check_constraint(Send_field_extended_metadata *to) const
    override;
  void add_key_fields(JOIN *join, KEY_FIELD **key_fields, uint *and_level,
                      table_map usable_tables, SARGABLE_PARAM **sargables)
    override;
  SEL_TREE *get_mm_tree(RANGE_OPT_PARAM *param, Item **cond_ptr) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cond_and>(thd, this); }
};

inline bool is_cond_and(Item *item)
{
  Item_func *func_item= item->get_item_func();
  return func_item && func_item->functype() == Item_func::COND_AND_FUNC;
}

class Item_cond_or final :public Item_cond
{
public:
  Item_cond_or(THD *thd): Item_cond(thd) {}
  Item_cond_or(THD *thd, Item *i1,Item *i2): Item_cond(thd, i1, i2) {}
  Item_cond_or(THD *thd, Item_cond_or *item): Item_cond(thd, item) {}
  Item_cond_or(THD *thd, List<Item> &list_arg): Item_cond(thd, list_arg) {}
  enum Functype functype() const override { return COND_OR_FUNC; }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("or") };
    return name;
  }
  enum precedence precedence() const override { return OR_PRECEDENCE; }
  table_map not_null_tables() const override { return and_tables_cache; }
  Item *copy_andor_structure(THD *thd) override;
  Item *neg_transformer(THD *thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_cond_or>(thd, this); }
};

class Item_func_dyncol_check :public Item_bool_func
{
public:
  Item_func_dyncol_check(THD *thd, Item *str): Item_bool_func(thd, str) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_check") };
    return name;
  }
  bool need_parentheses_in_default() override { return false; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_dyncol_check>(thd, this); }
};

class Item_func_dyncol_exists :public Item_bool_func
{
public:
  Item_func_dyncol_exists(THD *thd, Item *str, Item *num):
    Item_bool_func(thd, str, num) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("column_exists") };
    return name;
  }
  bool need_parentheses_in_default() override { return false; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_dyncol_exists>(thd, this); }
};


class Item_func_cursor_bool_attr: public Item_bool_func, public Cursor_ref
{
public:
  Item_func_cursor_bool_attr(THD *thd, const LEX_CSTRING *name, uint offset)
   :Item_bool_func(thd), Cursor_ref(name, offset)
  { }
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), arg, VCOL_SESSION_FUNC);
  }
  void print(String *str, enum_query_type query_type)
  {
    Cursor_ref::print_func(str, func_name_cstring());
  }
};


class Item_func_cursor_isopen: public Item_func_cursor_bool_attr
{
public:
  Item_func_cursor_isopen(THD *thd, const LEX_CSTRING *name, uint offset)
   :Item_func_cursor_bool_attr(thd, name, offset) { }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("%ISOPEN") };
    return name;
  }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_cursor_isopen>(thd, this); }
};


class Item_func_cursor_found: public Item_func_cursor_bool_attr
{
public:
  Item_func_cursor_found(THD *thd, const LEX_CSTRING *name, uint offset)
   :Item_func_cursor_bool_attr(thd, name, offset)
  {
    set_maybe_null();
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("%FOUND") };
    return name;
  }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_cursor_found>(thd, this); }
};


class Item_func_cursor_notfound: public Item_func_cursor_bool_attr
{
public:
  Item_func_cursor_notfound(THD *thd, const LEX_CSTRING *name, uint offset)
   :Item_func_cursor_bool_attr(thd, name, offset)
  {
    set_maybe_null();
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("%NOTFOUND") };
    return name;
  }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_cursor_notfound>(thd, this); }
};



inline bool is_cond_or(Item *item)
{
  Item_func *func_item= item->get_item_func();
  return func_item && func_item->functype() == Item_func::COND_OR_FUNC;
}

Item *and_expressions(Item *a, Item *b, Item **org_item);

class Comp_creator
{
public:
  Comp_creator() {}                           /* Remove gcc warning */
  virtual ~Comp_creator() {}                  /* Remove gcc warning */
  /**
    Create operation with given arguments.
  */
  virtual Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b)
                                           const = 0;
  /**
    Create operation with given arguments in swap order.
  */
  virtual Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b)
                                                const = 0;
  virtual const char* symbol(bool invert) const = 0;
  virtual bool eqne_op() const = 0;
  virtual bool l_op() const = 0;
};

class Eq_creator :public Comp_creator
{
public:
  Eq_creator() {}                             /* Remove gcc warning */
  virtual ~Eq_creator() {}                    /* Remove gcc warning */
  Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b) const;
  Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b) const;
  const char* symbol(bool invert) const { return invert? "<>" : "="; }
  bool eqne_op() const { return 1; }
  bool l_op() const { return 0; }
};

class Ne_creator :public Comp_creator
{
public:
  Ne_creator() {}                             /* Remove gcc warning */
  virtual ~Ne_creator() {}                    /* Remove gcc warning */
  Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b) const;
  Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b) const;
  const char* symbol(bool invert) const { return invert? "=" : "<>"; }
  bool eqne_op() const { return 1; }
  bool l_op() const { return 0; }
};

class Gt_creator :public Comp_creator
{
public:
  Gt_creator() {}                             /* Remove gcc warning */
  virtual ~Gt_creator() {}                    /* Remove gcc warning */
  Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b) const;
  Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b) const;
  const char* symbol(bool invert) const { return invert? "<=" : ">"; }
  bool eqne_op() const { return 0; }
  bool l_op() const { return 0; }
};

class Lt_creator :public Comp_creator
{
public:
  Lt_creator() {}                             /* Remove gcc warning */
  virtual ~Lt_creator() {}                    /* Remove gcc warning */
  Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b) const;
  Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b) const;
  const char* symbol(bool invert) const { return invert? ">=" : "<"; }
  bool eqne_op() const { return 0; }
  bool l_op() const { return 1; }
};

class Ge_creator :public Comp_creator
{
public:
  Ge_creator() {}                             /* Remove gcc warning */
  virtual ~Ge_creator() {}                    /* Remove gcc warning */
  Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b) const;
  Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b) const;
  const char* symbol(bool invert) const { return invert? "<" : ">="; }
  bool eqne_op() const { return 0; }
  bool l_op() const { return 0; }
};

class Le_creator :public Comp_creator
{
public:
  Le_creator() {}                             /* Remove gcc warning */
  virtual ~Le_creator() {}                    /* Remove gcc warning */
  Item_bool_rowready_func2* create(THD *thd, Item *a, Item *b) const;
  Item_bool_rowready_func2* create_swap(THD *thd, Item *a, Item *b) const;
  const char* symbol(bool invert) const { return invert? ">" : "<="; }
  bool eqne_op() const { return 0; }
  bool l_op() const { return 1; }
};

/*
  These need definitions from this file but the variables are defined
  in mysqld.h. The variables really belong in this component, but for
  the time being we leave them in mysqld.cc to avoid merge problems.
*/
extern Eq_creator eq_creator;
extern Ne_creator ne_creator;
extern Gt_creator gt_creator;
extern Lt_creator lt_creator;
extern Ge_creator ge_creator;
extern Le_creator le_creator;

#endif /* ITEM_CMPFUNC_INCLUDED */
