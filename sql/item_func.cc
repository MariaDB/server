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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  @brief
  This file defines all numerical functions
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "sql_plugin.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "slave.h"				// for wait_for_master_pos
#include "sql_show.h"                           // append_identifier
#include "strfunc.h"                            // find_type
#include "sql_parse.h"                          // is_update_query
#include "sql_acl.h"                            // EXECUTE_ACL
#include "mysqld.h"                             // LOCK_short_uuid_generator
#include "rpl_mi.h"
#include "sql_time.h"
#include <m_ctype.h>
#include <hash.h>
#include <time.h>
#include <ft_global.h>
#include <my_bit.h>

#include "sp_head.h"
#include "sp_rcontext.h"
#include "sp.h"
#include "set_var.h"
#include "debug_sync.h"
#include "sql_base.h"
#include "sql_cte.h"

#ifdef NO_EMBEDDED_ACCESS_CHECKS
#define sp_restore_security_context(A,B) while (0) {}
#endif

bool check_reserved_words(const LEX_CSTRING *name)
{
  if (lex_string_eq(name, STRING_WITH_LEN("GLOBAL")) ||
      lex_string_eq(name, STRING_WITH_LEN("LOCAL")) ||
      lex_string_eq(name, STRING_WITH_LEN("SESSION")))
    return TRUE;
  return FALSE;
}


/**
   Test if the sum of arguments overflows the ulonglong range.
*/
static inline bool test_if_sum_overflows_ull(ulonglong arg1, ulonglong arg2)
{
  return ULONGLONG_MAX - arg1 < arg2;
}


/**
  Allocate memory for arguments using tmp_args or thd->alloc().
  @retval false  - success
  @retval true   - error (arg_count is set to 0 for conveniece)
*/
bool Item_args::alloc_arguments(THD *thd, uint count)
{
  if (count <= 2)
  {
    args= tmp_arg;
    return false;
  }
  if ((args= (Item**) thd->alloc(sizeof(Item*) * count)) == NULL)
  {
    arg_count= 0;
    return true;
  }
  return false;
}


void Item_args::set_arguments(THD *thd, List<Item> &list)
{
  if (alloc_arguments(thd, list.elements))
    return;
  List_iterator_fast<Item> li(list);
  Item *item;
  for (arg_count= 0; (item= li++); )
    args[arg_count++]= item;
}


Item_args::Item_args(THD *thd, const Item_args *other)
  :arg_count(other->arg_count)
{
  if (arg_count <= 2)
  {
    args= tmp_arg;
  }
  else if (!(args= (Item**) thd->alloc(sizeof(Item*) * arg_count)))
  {
    arg_count= 0;
    return;
  }
  if (arg_count)
    memcpy(args, other->args, sizeof(Item*) * arg_count);
}


void Item_func::sync_with_sum_func_and_with_field(List<Item> &list)
{
  List_iterator_fast<Item> li(list);
  Item *item;
  while ((item= li++))
    with_flags|= item->with_flags;
}


bool Item_func::check_argument_types_like_args0() const
{
  if (arg_count < 2)
    return false;
  uint cols= args[0]->cols();
  bool is_scalar= args[0]->type_handler()->is_scalar_type();
  for (uint i= 1; i < arg_count; i++)
  {
    if (is_scalar != args[i]->type_handler()->is_scalar_type())
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
               args[0]->type_handler()->name().ptr(),
               args[i]->type_handler()->name().ptr(), func_name());
      return true;
    }
    if (args[i]->check_cols(cols))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_or_binary(const Type_handler *handler,
                                               uint start, uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_or_binary(func_name_cstring(), handler))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_traditional_scalar(uint start,
                                                        uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_traditional_scalar(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_can_return_int(uint start,
                                                    uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_can_return_int(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_can_return_real(uint start,
                                                     uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_can_return_real(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_can_return_text(uint start,
                                                     uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_can_return_text(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_can_return_str(uint start,
                                                    uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_can_return_str(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_can_return_date(uint start,
                                                     uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_can_return_date(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_can_return_time(uint start,
                                                     uint end) const
{
  for (uint i= start; i < end ; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_can_return_time(func_name_cstring()))
      return true;
  }
  return false;
}


bool Item_func::check_argument_types_scalar(uint start, uint end) const
{
  for (uint i= start; i < end; i++)
  {
    DBUG_ASSERT(i < arg_count);
    if (args[i]->check_type_scalar(func_name_cstring()))
      return true;
  }
  return false;
}


/*
  Resolve references to table column for a function and its argument

  SYNOPSIS:
  fix_fields()
  thd		Thread object
  ref		Pointer to where this object is used.  This reference
		is used if we want to replace this object with another
		one (for example in the summary functions).

  DESCRIPTION
    Call fix_fields() for all arguments to the function.  The main intention
    is to allow all Item_field() objects to setup pointers to the table fields.

    Sets as a side effect the following class variables:
      maybe_null        Set if any argument may return NULL
      with_sum_func     Set if any of the arguments contains a sum function
      with_window_func()  Set if any of the arguments contain a window function
      with_field        Set if any of the arguments contains or is a field
      used_tables_cache Set to union of the tables used by arguments

      str_value.charset If this is a string function, set this to the
			character set for the first argument.
			If any argument is binary, this is set to binary

   If for any item any of the defaults are wrong, then this can
   be fixed in the fix_length_and_dec() function that is called
   after this one or by writing a specialized fix_fields() for the
   item.

  RETURN VALUES
  FALSE	ok
  TRUE	Got error.  Stored with my_error().
*/

bool
Item_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  Item **arg,**arg_end;
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function

  /*
    The Used_tables_and_const_cache of "this" was initialized by
    the constructor, or by Item_func::cleanup().
  */
  DBUG_ASSERT(used_tables_cache == 0);
  DBUG_ASSERT(const_item_cache == true);

  not_null_tables_cache= 0;

  /*
    Use stack limit of STACK_MIN_SIZE * 2 since
    on some platforms a recursive call to fix_fields
    requires more than STACK_MIN_SIZE bytes (e.g. for
    MIPS, it takes about 22kB to make one recursive
    call to Item_func::fix_fields())
  */
  if (check_stack_overrun(thd, STACK_MIN_SIZE * 2, buff))
    return TRUE;				// Fatal error if flag is set!
  if (arg_count)
  {						// Print purify happy
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      Item *item;
      /*
	We can't yet set item to *arg as fix_fields may change *arg
	We shouldn't call fix_fields() twice, so check 'fixed' field first
      */
      if ((*arg)->fix_fields_if_needed(thd, arg))
	return TRUE;				/* purecov: inspected */
      item= *arg;

      base_flags|= item->base_flags & item_base_t::MAYBE_NULL;
      with_flags|= item->with_flags;
      used_tables_and_const_cache_join(item);
      not_null_tables_cache|= item->not_null_tables();
    }
  }
  if (check_arguments())
    return true;
  if (fix_length_and_dec(thd))
    return TRUE;
  base_flags|= item_base_t::FIXED;
  return FALSE;
}

void
Item_func::quick_fix_field()
{
  Item **arg,**arg_end;
  if (arg_count)
  {
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      if (!(*arg)->fixed())
        (*arg)->quick_fix_field();
    }
  }
  base_flags|= item_base_t::FIXED;
}


bool
Item_func::eval_not_null_tables(void *opt_arg)
{
  Item **arg,**arg_end;
  not_null_tables_cache= 0;
  if (arg_count)
  {		
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      not_null_tables_cache|= (*arg)->not_null_tables();
    }
  }
  return FALSE;
}


bool
Item_func::find_not_null_fields(table_map allowed)
{
  if (~allowed & used_tables())
    return false;

  Item **arg,**arg_end;
  if (arg_count)
  {
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      if (!(*arg)->find_not_null_fields(allowed))
        continue;
    }
  }
  return false;
}


void Item_func::fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                  bool merge)
{
  Item **arg,**arg_end;

  used_tables_and_const_cache_init();
  not_null_tables_cache= 0;

  if (arg_count)
  {
    for (arg=args, arg_end=args+arg_count; arg != arg_end ; arg++)
    {
      (*arg)->fix_after_pullout(new_parent, arg, merge);
      Item *item= *arg;

      used_tables_and_const_cache_join(item);
      not_null_tables_cache|= item->not_null_tables();
    }
  }
}


void Item_func::traverse_cond(Cond_traverser traverser,
                              void *argument, traverse_order order)
{
  if (arg_count)
  {
    Item **arg,**arg_end;

    switch (order) {
    case(PREFIX):
      (*traverser)(this, argument);
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      break;
    case (POSTFIX):
      for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
      {
	(*arg)->traverse_cond(traverser, argument, order);
      }
      (*traverser)(this, argument);
    }
  }
  else
    (*traverser)(this, argument);
}


bool Item_args::transform_args(THD *thd, Item_transformer transformer, uchar *arg)
{
  for (uint i= 0; i < arg_count; i++)
  {
    Item *new_item= args[i]->transform(thd, transformer, arg);
    if (!new_item)
      return true;
    /*
      THD::change_item_tree() should be called only if the tree was
      really transformed, i.e. when a new item has been created.
      Otherwise we'll be allocating a lot of unnecessary memory for
      change records at each execution.
    */
    if (args[i] != new_item)
      thd->change_item_tree(&args[i], new_item);
  }
  return false;
}


/**
  Transform an Item_func object with a transformer callback function.

    The function recursively applies the transform method to each
    argument of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
  @param transformer   the transformer callback function to be applied to
                       the nodes of the tree of the object
  @param argument      parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node
*/

Item *Item_func::transform(THD *thd, Item_transformer transformer, uchar *argument)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());
  if (transform_args(thd, transformer, argument))
    return 0;
  return (this->*transformer)(thd, argument);
}


/**
  Compile Item_func object with a processor and a transformer
  callback functions.

    First the function applies the analyzer to the root node of
    the Item_func object. Then if the analyzer succeeds (returns TRUE)
    the function recursively applies the compile method to each argument
    of the Item_func node.
    If the call of the method for an argument item returns a new item
    the old item is substituted for a new one.
    After this the transformer is applied to the root node
    of the Item_func object. 
    The compile function is not called if the analyzer returns NULL
    in the parameter arg_p. 

  @param analyzer      the analyzer callback function to be applied to the
                       nodes of the tree of the object
  @param[in,out] arg_p parameter to be passed to the processor
  @param transformer   the transformer callback function to be applied to the
                       nodes of the tree of the object
  @param arg_t         parameter to be passed to the transformer

  @return
    Item returned as the result of transformation of the root node
*/

Item *Item_func::compile(THD *thd, Item_analyzer analyzer, uchar **arg_p,
                         Item_transformer transformer, uchar *arg_t)
{
  if (!(this->*analyzer)(arg_p))
    return 0;
  if (*arg_p && arg_count)
  {
    Item **arg,**arg_end;
    for (arg= args, arg_end= args+arg_count; arg != arg_end; arg++)
    {
      /* 
        The same parameter value of arg_p must be passed
        to analyze any argument of the condition formula.
      */
      uchar *arg_v= *arg_p;
      Item *new_item= (*arg)->compile(thd, analyzer, &arg_v, transformer,
                                      arg_t);
      if (new_item && *arg != new_item)
        thd->change_item_tree(arg, new_item);
    }
  }
  return (this->*transformer)(thd, arg_t);
}


void Item_args::propagate_equal_fields(THD *thd,
                                       const Item::Context &ctx,
                                       COND_EQUAL *cond)
{
  uint i;
  for (i= 0; i < arg_count; i++)
    args[i]->propagate_equal_fields_and_change_item_tree(thd, ctx, cond,
                                                         &args[i]);
}


Sql_mode_dependency Item_args::value_depends_on_sql_mode_bit_or() const
{
  Sql_mode_dependency res;
  for (uint i= 0; i < arg_count; i++)
    res|= args[i]->value_depends_on_sql_mode();
  return res;
}


/**
  See comments in Item_cond::split_sum_func()
*/

void Item_func::split_sum_func(THD *thd,  Ref_ptr_array ref_pointer_array,
                               List<Item> &fields, uint flags)
{
  Item **arg, **arg_end;
  DBUG_ENTER("Item_func::split_sum_func");

  for (arg= args, arg_end= args+arg_count; arg != arg_end ; arg++)
    (*arg)->split_sum_func2(thd, ref_pointer_array, fields, arg,
                            flags | SPLIT_SUM_SKIP_REGISTERED);
  DBUG_VOID_RETURN;
}


table_map Item_func::not_null_tables() const
{
  return not_null_tables_cache;
}


void Item_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  print_args(str, 0, query_type);
  str->append(')');
}


void Item_func::print_args(String *str, uint from, enum_query_type query_type)
{
  for (uint i=from ; i < arg_count ; i++)
  {
    if (i != from)
      str->append(',');
    args[i]->print(str, query_type);
  }
}


void Item_func::print_op(String *str, enum_query_type query_type)
{
  for (uint i=0 ; i < arg_count-1 ; i++)
  {
    args[i]->print_parenthesised(str, query_type, precedence());
    str->append(' ');
    str->append(func_name_cstring());
    str->append(' ');
  }
  args[arg_count-1]->print_parenthesised(str, query_type, higher_precedence());
}


bool Item_func::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;
  /*
    Ensure that we are comparing two functions and that the function
    is deterministic.
  */
  if (item->type() != FUNC_ITEM || (used_tables() & RAND_TABLE_BIT))
    return 0;
  Item_func *item_func=(Item_func*) item;
  Item_func::Functype func_type;
  if ((func_type= functype()) != item_func->functype() ||
      arg_count != item_func->arg_count ||
      (func_type != Item_func::FUNC_SP &&
       func_name() != item_func->func_name()) ||
      (func_type == Item_func::FUNC_SP &&
       my_strcasecmp(system_charset_info, func_name(), item_func->func_name())))
    return 0;
  return Item_args::eq(item_func, binary_cmp);
}


/*
bool Item_func::is_expensive_processor(uchar *arg)
{
  return is_expensive();
}
*/


bool Item_hybrid_func::fix_attributes(Item **items, uint nitems)
{
  bool rc= Item_hybrid_func::type_handler()->
             Item_hybrid_func_fix_attributes(current_thd,
                                             func_name_cstring(), this, this,
                                             items, nitems);
  DBUG_ASSERT(!rc || current_thd->is_error());
  return rc;
}


String *Item_real_func::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  str->set_real(nr, decimals, collation.collation);
  return str;
}


my_decimal *Item_real_func::val_decimal(my_decimal *decimal_value)
{
  DBUG_ASSERT(fixed());
  double nr= val_real();
  if (null_value)
    return 0; /* purecov: inspected */
  double2my_decimal(E_DEC_FATAL_ERROR, nr, decimal_value);
  return decimal_value;
}


#ifdef HAVE_DLOPEN
void Item_udf_func::fix_num_length_and_dec()
{
  uint fl_length= 0;
  decimals=0;
  for (uint i=0 ; i < arg_count ; i++)
  {
    set_if_bigger(decimals,args[i]->decimals);
    set_if_bigger(fl_length, args[i]->max_length);
  }
  max_length=float_length(decimals);
  if (fl_length > max_length)
  {
    decimals= NOT_FIXED_DEC;
    max_length= float_length(NOT_FIXED_DEC);
  }
}
#endif


void Item_func::signal_divide_by_null()
{
  THD *thd= current_thd;
  if (thd->variables.sql_mode & MODE_ERROR_FOR_DIVISION_BY_ZERO)
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_DIVISION_BY_ZERO,
                 ER_THD(thd, ER_DIVISION_BY_ZERO));
  null_value= 1;
}


Item *Item_func::get_tmp_table_item(THD *thd)
{
  if (!with_sum_func() && !const_item())
  {
    auto item_field= new (thd->mem_root) Item_field(thd, result_field);
    if (item_field)
      item_field->set_refers_to_temp_table(true);
    return item_field;
  }
  return copy_or_same(thd);
}

double Item_int_func::val_real()
{
  DBUG_ASSERT(fixed());

  return unsigned_flag ? (double) ((ulonglong) val_int()) : (double) val_int();
}


String *Item_int_func::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, collation.collation);
  return str;
}


bool Item_func_connection_id::fix_length_and_dec(THD *thd)
{
  if (Item_long_func::fix_length_and_dec(thd))
    return TRUE;
  max_length= 10;
  return FALSE;
}


bool Item_func_connection_id::fix_fields(THD *thd, Item **ref)
{
  if (Item_int_func::fix_fields(thd, ref))
    return TRUE;
  thd->thread_specific_used= TRUE;
  value= thd->variables.pseudo_thread_id;
  return FALSE;
}


bool Item_num_op::fix_type_handler(const Type_aggregator *aggregator)
{
  DBUG_ASSERT(arg_count == 2);
  const Type_handler *h0= args[0]->cast_to_int_type_handler();
  const Type_handler *h1= args[1]->cast_to_int_type_handler();
  if (!aggregate_for_num_op(aggregator, h0, h1))
    return false;
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
           h0->name().ptr(), h1->name().ptr(), func_name());
  return true;
}


bool Item_func_plus::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_plus::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  const Type_aggregator *aggregator= &type_handler_data->m_type_aggregator_for_plus;
  DBUG_EXECUTE_IF("num_op", aggregator= &type_handler_data->m_type_aggregator_for_result;);
  DBUG_ASSERT(aggregator->is_commutative());
  if (fix_type_handler(aggregator))
    DBUG_RETURN(TRUE);
  if (Item_func_plus::type_handler()->Item_func_plus_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}


String *Item_func_hybrid_field_type::val_str_from_int_op(String *str)
{
  longlong nr= int_op();
  if (null_value)
    return 0; /* purecov: inspected */
  str->set_int(nr, unsigned_flag, collation.collation);
  return str;
}

double Item_func_hybrid_field_type::val_real_from_int_op()
{
  longlong result= int_op();
  return unsigned_flag ? (double) ((ulonglong) result) : (double) result;
}

my_decimal *
Item_func_hybrid_field_type::val_decimal_from_int_op(my_decimal *dec)
{
  longlong result= int_op();
  if (null_value)
    return NULL;
  int2my_decimal(E_DEC_FATAL_ERROR, result, unsigned_flag, dec);
  return dec;
}


String *Item_func_hybrid_field_type::val_str_from_real_op(String *str)
{
  double nr= real_op();
  if (null_value)
    return 0; /* purecov: inspected */
  str->set_real(nr, decimals, collation.collation);
  return str;
}

longlong Item_func_hybrid_field_type::val_int_from_real_op()
{
  return Converter_double_to_longlong(real_op(), unsigned_flag).result();
}

my_decimal *
Item_func_hybrid_field_type::val_decimal_from_real_op(my_decimal *dec)
{
  double result= (double) real_op();
  if (null_value)
    return NULL;
  double2my_decimal(E_DEC_FATAL_ERROR, result, dec);
  return dec;
}


String *Item_func_hybrid_field_type::val_str_from_date_op(String *str)
{
  MYSQL_TIME ltime;
  if (date_op_with_null_check(current_thd, &ltime) ||
      (null_value= str->alloc(MAX_DATE_STRING_REP_LENGTH)))
    return (String *) 0;
  str->length(my_TIME_to_str(&ltime, const_cast<char*>(str->ptr()), decimals));
  str->set_charset(&my_charset_bin);
  DBUG_ASSERT(!null_value);
  return str;
}

double Item_func_hybrid_field_type::val_real_from_date_op()
{
  MYSQL_TIME ltime;
  if (date_op_with_null_check(current_thd, &ltime))
    return 0;
  return TIME_to_double(&ltime);
}

longlong Item_func_hybrid_field_type::val_int_from_date_op()
{
  MYSQL_TIME ltime;
  if (date_op_with_null_check(current_thd, &ltime))
    return 0;
  return TIME_to_ulonglong(&ltime);
}

my_decimal *
Item_func_hybrid_field_type::val_decimal_from_date_op(my_decimal *dec)
{
  MYSQL_TIME ltime;
  if (date_op_with_null_check(current_thd, &ltime))
  {
    my_decimal_set_zero(dec);
    return 0;
  }
  return date2my_decimal(&ltime, dec);
}


String *Item_func_hybrid_field_type::val_str_from_time_op(String *str)
{
  MYSQL_TIME ltime;
  if (time_op_with_null_check(current_thd, &ltime) ||
      (null_value= my_TIME_to_str(&ltime, str, decimals)))
    return NULL;
  return str;
}

double Item_func_hybrid_field_type::val_real_from_time_op()
{
  MYSQL_TIME ltime;
  return time_op_with_null_check(current_thd, &ltime) ? 0 :
         TIME_to_double(&ltime);
}

longlong Item_func_hybrid_field_type::val_int_from_time_op()
{
  MYSQL_TIME ltime;
  return time_op_with_null_check(current_thd, &ltime) ? 0 :
         TIME_to_ulonglong(&ltime);
}

my_decimal *
Item_func_hybrid_field_type::val_decimal_from_time_op(my_decimal *dec)
{
  MYSQL_TIME ltime;
  if (time_op_with_null_check(current_thd, &ltime))
  {
    my_decimal_set_zero(dec);
    return 0;
  }
  return date2my_decimal(&ltime, dec);
}


double Item_func_hybrid_field_type::val_real_from_str_op()
{
  String *res= str_op_with_null_check(&str_value);
  return res ? double_from_string_with_check(res) : 0.0;
}

longlong Item_func_hybrid_field_type::val_int_from_str_op()
{
  String *res= str_op_with_null_check(&str_value);
  return res ? longlong_from_string_with_check(res) : 0;
}

my_decimal *
Item_func_hybrid_field_type::val_decimal_from_str_op(my_decimal *decimal_value)
{
  String *res= str_op_with_null_check(&str_value);
  return res ? decimal_from_string_with_check(decimal_value, res) : 0;
}


void Item_func_signed::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as signed)"));

}


void Item_func_unsigned::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as unsigned)"));

}


my_decimal *Item_decimal_typecast::val_decimal(my_decimal *dec)
{
  VDec tmp(args[0]);
  bool sign;
  uint precision;

  if ((null_value= tmp.is_null()))
    return NULL;
  tmp.round_to(dec, decimals, HALF_UP);
  sign= dec->sign();
  if (unsigned_flag)
  {
    if (sign)
    {
      my_decimal_set_zero(dec);
      goto err;
    }
  }
  precision= my_decimal_length_to_precision(max_length,
                                            decimals, unsigned_flag);
  if (precision - decimals < (uint) my_decimal_intg(dec))
  {
    max_my_decimal(dec, precision, decimals);
    dec->sign(sign);
    goto err;
  }
  return dec;

err:
  THD *thd= current_thd;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_WARN_DATA_OUT_OF_RANGE,
                      ER_THD(thd, ER_WARN_DATA_OUT_OF_RANGE),
                      name.str,
                      thd->get_stmt_da()->current_row_for_warning());
  return dec;
}


void Item_decimal_typecast::print(String *str, enum_query_type query_type)
{
  char len_buf[20*3 + 1];
  char *end;

  uint precision= my_decimal_length_to_precision(max_length, decimals,
                                                 unsigned_flag);
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as decimal("));

  end=int10_to_str(precision, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(',');

  end=int10_to_str(decimals, len_buf,10);
  str->append(len_buf, (uint32) (end - len_buf));

  str->append(')');
  str->append(')');
}


double Item_real_typecast::val_real_with_truncate(double max_value)
{
  int error;
  double tmp= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;

  if (unlikely((error= truncate_double(&tmp, max_length, decimals,
                                       false/*unsigned_flag*/, max_value))))
  {
    /*
      We don't want automatic escalation from a warning to an error
      in this scenario:
        INSERT INTO t1 (float_field) VALUES (CAST(1e100 AS FLOAT));
      The above statement should work even in the strict mode.
      So let's use a note rather than a warning.
    */
    THD *thd= current_thd;
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_NOTE,
                        ER_WARN_DATA_OUT_OF_RANGE,
                        ER_THD(thd, ER_WARN_DATA_OUT_OF_RANGE),
                        name.str, (ulong) 1);
    if (error < 0)
    {
      null_value= 1;                            // Illegal value
      tmp= 0.0;
    }
  }
  return tmp;
}


void Item_real_typecast::print(String *str, enum_query_type query_type)
{
  char len_buf[20*3 + 1];
  char *end;
  Name name= type_handler()->name();

  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as "));
  str->append(name.ptr(), name.length());
  if (decimals != NOT_FIXED_DEC)
  {
    str->append('(');
    end= int10_to_str(max_length, len_buf,10);
    str->append(len_buf, (uint32) (end - len_buf));
    str->append(',');
    end= int10_to_str(decimals, len_buf,10);
    str->append(len_buf, (uint32) (end - len_buf));
    str->append(')');
  }
  str->append(')');
}

double Item_func_plus::real_op()
{
  double value= args[0]->val_real() + args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}

#if defined(__powerpc64__) && GCC_VERSION >= 6003 && GCC_VERSION <= 10002
#pragma GCC push_options
#pragma GCC optimize ("no-expensive-optimizations")
#endif

longlong Item_func_plus::int_op()
{
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  bool     res_unsigned= FALSE;
  longlong res;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;
  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().
  */
  if (args[0]->unsigned_flag)
  {
    if (args[1]->unsigned_flag || val1 >= 0)
    {
      if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) val1))
        goto err;
      res_unsigned= TRUE;
    }
    else
    {
      /* val1 is negative */
      if ((ulonglong) val0 > (ulonglong) LONGLONG_MAX)
        res_unsigned= TRUE;
    }
  }
  else
  {
    if (args[1]->unsigned_flag)
    {
      if (val0 >= 0)
      {
        if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) val1))
          goto err;
        res_unsigned= TRUE;
      }
      else
      {
        if ((ulonglong) val1 > (ulonglong) LONGLONG_MAX)
          res_unsigned= TRUE;
      }
    }
    else
    {
      if (val0 >=0 && val1 >= 0)
        res_unsigned= TRUE;
      else if (val0 < 0 && val1 < 0 && val0 < (LONGLONG_MIN - val1))
        goto err;
    }
  }

  if (res_unsigned)
    res= (longlong) ((ulonglong) val0 + (ulonglong) val1);
  else
    res= val0 + val1;

  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}

#if defined(__powerpc64__) && GCC_VERSION >= 6003 && GCC_VERSION <= 10002
#pragma GCC pop_options
#endif

/**
  Calculate plus of two decimals.

  @param decimal_value	Buffer that can be used to store result

  @retval
    0  Value was NULL;  In this case null_value is set
  @retval
    \# Value of operation as a decimal
*/

my_decimal *Item_func_plus::decimal_op(my_decimal *decimal_value)
{
  VDec2_lazy val(args[0], args[1]);
  if (!(null_value= (val.has_null() ||
                     check_decimal_overflow(my_decimal_add(E_DEC_FATAL_ERROR &
                                                           ~E_DEC_OVERFLOW,
                                                           decimal_value,
                                                           val.m_a.ptr(),
                                                           val.m_b.ptr())) > 3)))
    return decimal_value;
  return 0;
}

/**
  Set precision of results for additive operations (+ and -)
*/
void Item_func_additive_op::result_precision()
{
  decimals= MY_MAX(args[0]->decimal_scale(), args[1]->decimal_scale());
  int arg1_int= args[0]->decimal_precision() - args[0]->decimal_scale();
  int arg2_int= args[1]->decimal_precision() - args[1]->decimal_scale();
  int precision= MY_MAX(arg1_int, arg2_int) + 1 + decimals;

  DBUG_ASSERT(arg1_int >= 0);
  DBUG_ASSERT(arg2_int >= 0);

  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


/**
  The following function is here to allow the user to force
  subtraction of UNSIGNED BIGINT to return negative values.
*/
void Item_func_minus::fix_unsigned_flag()
{
  if (unsigned_flag &&
      (current_thd->variables.sql_mode & MODE_NO_UNSIGNED_SUBTRACTION))
  {
    unsigned_flag=0;
    set_handler(Item_func_minus::type_handler()->type_handler_signed());
  }
}


bool Item_func_minus::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_minus::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  const Type_aggregator *aggregator= &type_handler_data->m_type_aggregator_for_minus;
  DBUG_EXECUTE_IF("num_op", aggregator= &type_handler_data->m_type_aggregator_non_commutative_test;);
  DBUG_ASSERT(!aggregator->is_commutative());
  if (fix_type_handler(aggregator))
    DBUG_RETURN(TRUE);
  if (Item_func_minus::type_handler()->Item_func_minus_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  m_depends_on_sql_mode_no_unsigned_subtraction= unsigned_flag;
  fix_unsigned_flag();
  DBUG_RETURN(FALSE);
}


Sql_mode_dependency Item_func_minus::value_depends_on_sql_mode() const
{
  Sql_mode_dependency dep= Item_func_additive_op::value_depends_on_sql_mode();
  if (m_depends_on_sql_mode_no_unsigned_subtraction)
    dep|= Sql_mode_dependency(0, MODE_NO_UNSIGNED_SUBTRACTION);
  return dep;
}


double Item_func_minus::real_op()
{
  double value= args[0]->val_real() - args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


#if defined(__powerpc64__) && GCC_VERSION >= 6003 && GCC_VERSION <= 10002
#pragma GCC push_options
#pragma GCC optimize ("no-expensive-optimizations")
#endif

longlong Item_func_minus::int_op()
{
  longlong val0= args[0]->val_int();
  longlong val1= args[1]->val_int();
  bool     res_unsigned= FALSE;
  longlong res;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().
  */
  if (args[0]->unsigned_flag)
  {
    if (args[1]->unsigned_flag)
    {
      if ((ulonglong) val0 < (ulonglong) val1)
        goto err;
      res_unsigned= TRUE;
    }
    else
    {
      if (val1 >= 0)
      {
        if ((ulonglong) val0 > (ulonglong) val1)
          res_unsigned= TRUE;
      }
      else
      {
        if (test_if_sum_overflows_ull((ulonglong) val0, (ulonglong) -val1))
          goto err;
        res_unsigned= TRUE;
      }
    }
  }
  else
  {
    if (args[1]->unsigned_flag)
    {
      if (((ulonglong) val0 - (ulonglong) LONGLONG_MIN) < (ulonglong) val1)
        goto err;
    }
    else
    {
      if (val0 > 0 && val1 < 0)
        res_unsigned= TRUE;
      else if (val0 < 0 && val1 > 0 && val0 < (LONGLONG_MIN + val1))
        goto err;
    }
  }
  if (res_unsigned)
    res= (longlong) ((ulonglong) val0 - (ulonglong) val1);
  else
    res= val0 - val1;

  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}

#if defined(__powerpc64__) && GCC_VERSION >= 6003 && GCC_VERSION <= 10002
#pragma GCC pop_options
#endif

/**
  See Item_func_plus::decimal_op for comments.
*/

my_decimal *Item_func_minus::decimal_op(my_decimal *decimal_value)
{
  VDec2_lazy val(args[0], args[1]);
  if (!(null_value= (val.has_null() ||
                     check_decimal_overflow(my_decimal_sub(E_DEC_FATAL_ERROR &
                                                           ~E_DEC_OVERFLOW,
                                                           decimal_value,
                                                           val.m_a.ptr(),
                                                           val.m_b.ptr())) > 3)))
    return decimal_value;
  return 0;
}


double Item_func_mul::real_op()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real() * args[1]->val_real();
  if ((null_value=args[0]->null_value || args[1]->null_value))
    return 0.0;
  return check_float_overflow(value);
}


longlong Item_func_mul::int_op()
{
  DBUG_ASSERT(fixed());
  longlong a= args[0]->val_int();
  longlong b= args[1]->val_int();
  longlong res;
  ulonglong res0, res1;
  ulong a0, a1, b0, b1;
  bool     res_unsigned= FALSE;
  bool     a_negative= FALSE, b_negative= FALSE;

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;

  /*
    First check whether the result can be represented as a
    (bool unsigned_flag, longlong value) pair, then check if it is compatible
    with this Item's unsigned_flag by calling check_integer_overflow().

    Let a = a1 * 2^32 + a0 and b = b1 * 2^32 + b0. Then
    a * b = (a1 * 2^32 + a0) * (b1 * 2^32 + b0) = a1 * b1 * 2^64 +
            + (a1 * b0 + a0 * b1) * 2^32 + a0 * b0;
    We can determine if the above sum overflows the ulonglong range by
    sequentially checking the following conditions:
    1. If both a1 and b1 are non-zero.
    2. Otherwise, if (a1 * b0 + a0 * b1) is greater than ULONG_MAX.
    3. Otherwise, if (a1 * b0 + a0 * b1) * 2^32 + a0 * b0 is greater than
    ULONGLONG_MAX.

    Since we also have to take the unsigned_flag for a and b into account,
    it is easier to first work with absolute values and set the
    correct sign later.
  */
  if (!args[0]->unsigned_flag && a < 0)
  {
    a_negative= TRUE;
    a= -a;
  }
  if (!args[1]->unsigned_flag && b < 0)
  {
    b_negative= TRUE;
    b= -b;
  }

  a0= 0xFFFFFFFFUL & a;
  a1= ((ulonglong) a) >> 32;
  b0= 0xFFFFFFFFUL & b;
  b1= ((ulonglong) b) >> 32;

  if (a1 && b1)
    goto err;

  res1= (ulonglong) a1 * b0 + (ulonglong) a0 * b1;
  if (res1 > 0xFFFFFFFFUL)
    goto err;

  res1= res1 << 32;
  res0= (ulonglong) a0 * b0;

  if (test_if_sum_overflows_ull(res1, res0))
    goto err;
  res= res1 + res0;

  if (a_negative != b_negative)
  {
    if ((ulonglong) res > (ulonglong) LONGLONG_MIN + 1)
      goto err;
    res= -res;
  }
  else
    res_unsigned= TRUE;

  return check_integer_overflow(res, res_unsigned);

err:
  return raise_integer_overflow();
}


/** See Item_func_plus::decimal_op for comments. */

my_decimal *Item_func_mul::decimal_op(my_decimal *decimal_value)
{
  VDec2_lazy val(args[0], args[1]);
  if (!(null_value= (val.has_null() ||
                     check_decimal_overflow(my_decimal_mul(E_DEC_FATAL_ERROR &
                                                           ~E_DEC_OVERFLOW,
                                                           decimal_value,
                                                           val.m_a.ptr(),
                                                           val.m_b.ptr())) > 3)))
    return decimal_value;
  return 0;
}


void Item_func_mul::result_precision()
{
  decimals= MY_MIN(args[0]->decimal_scale() + args[1]->decimal_scale(),
                   DECIMAL_MAX_SCALE);
  uint est_prec = args[0]->decimal_precision() + args[1]->decimal_precision();
  uint precision= MY_MIN(est_prec, DECIMAL_MAX_PRECISION);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


bool Item_func_mul::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_mul::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  const Type_aggregator *aggregator= &type_handler_data->m_type_aggregator_for_mul;
  DBUG_EXECUTE_IF("num_op", aggregator= &type_handler_data->m_type_aggregator_for_result;);
  DBUG_ASSERT(aggregator->is_commutative());
  if (fix_type_handler(aggregator))
    DBUG_RETURN(TRUE);
  if (Item_func_mul::type_handler()->Item_func_mul_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}


double Item_func_div::real_op()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0;
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return check_float_overflow(value/val2);
}


my_decimal *Item_func_div::decimal_op(my_decimal *decimal_value)
{
  int err;
  VDec2_lazy val(args[0], args[1]);
  if ((null_value= val.has_null()))
    return 0;
  if ((err= check_decimal_overflow(my_decimal_div(E_DEC_FATAL_ERROR &
                                                  ~E_DEC_OVERFLOW &
                                                  ~E_DEC_DIV_ZERO,
                                                  decimal_value,
                                                  val.m_a.ptr(), val.m_b.ptr(),
                                                  prec_increment))) > 3)
  {
    if (err == E_DEC_DIV_ZERO)
      signal_divide_by_null();
    null_value= 1;
    return 0;
  }
  return decimal_value;
}


void Item_func_div::result_precision()
{
  /*
    We need to add args[1]->divisor_precision_increment(),
    to properly handle the cases like this:
      SELECT 5.05 / 0.014; -> 360.714286
    i.e. when the divisor has a zero integer part
    and non-zero digits appear only after the decimal point.
    Precision in this example is calculated as
      args[0]->decimal_precision()           +  // 3
      args[1]->divisor_precision_increment() +  // 3
      prec_increment                            // 4
    which gives 10 decimals digits.
  */
  uint precision=MY_MIN(args[0]->decimal_precision() + 
                     args[1]->divisor_precision_increment() + prec_increment,
                     DECIMAL_MAX_PRECISION);
  decimals= MY_MIN(args[0]->decimal_scale() + prec_increment, DECIMAL_MAX_SCALE);
  max_length= my_decimal_precision_to_length_no_truncation(precision, decimals,
                                                           unsigned_flag);
}


void Item_func_div::fix_length_and_dec_double(void)
{
  Item_num_op::fix_length_and_dec_double();
  decimals= MY_MAX(args[0]->decimals, args[1]->decimals) + prec_increment;
  set_if_smaller(decimals, NOT_FIXED_DEC);
  uint tmp= float_length(decimals);
  if (decimals == NOT_FIXED_DEC)
    max_length= tmp;
  else
  {
    max_length=args[0]->max_length - args[0]->decimals + decimals;
    set_if_smaller(max_length, tmp);
  }
}


void Item_func_div::fix_length_and_dec_int(void)
{
  set_handler(&type_handler_newdecimal);
  DBUG_PRINT("info", ("Type changed: %s", type_handler()->name().ptr()));
  Item_num_op::fix_length_and_dec_decimal();
}


bool Item_func_div::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_div::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  prec_increment= thd->variables.div_precincrement;
  set_maybe_null(); // division by zero

  const Type_aggregator *aggregator= &type_handler_data->m_type_aggregator_for_div;
  DBUG_EXECUTE_IF("num_op", aggregator= &type_handler_data->m_type_aggregator_non_commutative_test;);
  DBUG_ASSERT(!aggregator->is_commutative());
  if (fix_type_handler(aggregator))
    DBUG_RETURN(TRUE);
  if (Item_func_div::type_handler()->Item_func_div_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}


/* Integer division */
longlong Item_func_int_div::val_int()
{
  DBUG_ASSERT(fixed());

  /*
    Perform division using DECIMAL math if either of the operands has a
    non-integer type
  */
  if (args[0]->result_type() != INT_RESULT ||
      args[1]->result_type() != INT_RESULT)
  {
    VDec2_lazy val(args[0], args[1]);
    if ((null_value= val.has_null()))
      return 0;

    int err;
    my_decimal tmp;
    if ((err= my_decimal_div(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, &tmp,
                             val.m_a.ptr(), val.m_b.ptr(), 0)) > 3)
    {
      if (err == E_DEC_DIV_ZERO)
        signal_divide_by_null();
      return 0;
    }

    my_decimal truncated;
    if (tmp.round_to(&truncated, 0, TRUNCATE))
      DBUG_ASSERT(false);

    longlong res;
    if (my_decimal2int(E_DEC_FATAL_ERROR, &truncated, unsigned_flag, &res) &
        E_DEC_OVERFLOW)
      raise_integer_overflow();
    return res;
  }

  Longlong_hybrid val0= args[0]->to_longlong_hybrid();
  Longlong_hybrid val1= args[1]->to_longlong_hybrid();
  if ((null_value= (args[0]->null_value || args[1]->null_value)))
    return 0;
  if (val1 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  bool res_negative= val0.neg() != val1.neg();
  ulonglong res= val0.abs() / val1.abs();
  if (res_negative)
  {
    if (res > (ulonglong) LONGLONG_MAX)
      return raise_integer_overflow();
    res= (ulonglong) (-(longlong) res);
  }
  return check_integer_overflow(res, !res_negative);
}


bool Item_func_int_div::fix_length_and_dec(THD *thd)
{
  uint32 prec= args[0]->decimal_int_part();
  set_if_smaller(prec, MY_INT64_NUM_DECIMAL_DIGITS);
  fix_char_length(prec);
  set_maybe_null();
  unsigned_flag=args[0]->unsigned_flag | args[1]->unsigned_flag;
  return false;
}


longlong Item_func_mod::int_op()
{
  DBUG_ASSERT(fixed());
  Longlong_hybrid val0= args[0]->to_longlong_hybrid();
  Longlong_hybrid val1= args[1]->to_longlong_hybrid();

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0; /* purecov: inspected */
  if (val1 == 0)
  {
    signal_divide_by_null();
    return 0;
  }

  /*
    '%' is calculated by integer division internally. Since dividing
    LONGLONG_MIN by -1 generates SIGFPE, we calculate using unsigned values and
    then adjust the sign appropriately.
  */
  ulonglong res= val0.abs() % val1.abs();
  return check_integer_overflow(val0.neg() ? -(longlong) res : res,
                                !val0.neg());
}

double Item_func_mod::real_op()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  double val2=  args[1]->val_real();
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0.0; /* purecov: inspected */
  if (val2 == 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return fmod(value,val2);
}


my_decimal *Item_func_mod::decimal_op(my_decimal *decimal_value)
{
  VDec2_lazy val(args[0], args[1]);
  if ((null_value= val.has_null()))
    return 0;
  switch (my_decimal_mod(E_DEC_FATAL_ERROR & ~E_DEC_DIV_ZERO, decimal_value,
                         val.m_a.ptr(), val.m_b.ptr())) {
  case E_DEC_TRUNCATED:
  case E_DEC_OK:
    return decimal_value;
  case E_DEC_DIV_ZERO:
    signal_divide_by_null();
    /* fall through */
  default:
    null_value= 1;
    return 0;
  }
}


void Item_func_mod::result_precision()
{
  unsigned_flag= args[0]->unsigned_flag;
  decimals= MY_MAX(args[0]->decimal_scale(), args[1]->decimal_scale());
  uint prec= MY_MAX(args[0]->decimal_precision(), args[1]->decimal_precision());
  fix_char_length(my_decimal_precision_to_length_no_truncation(prec, decimals,
                                                               unsigned_flag));
}


bool Item_func_mod::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_mod::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  set_maybe_null(); // division by zero
  const Type_aggregator *aggregator= &type_handler_data->m_type_aggregator_for_mod;
  DBUG_EXECUTE_IF("num_op", aggregator= &type_handler_data->m_type_aggregator_non_commutative_test;);
  DBUG_ASSERT(!aggregator->is_commutative());
  if (fix_type_handler(aggregator))
    DBUG_RETURN(TRUE);
  if (Item_func_mod::type_handler()->Item_func_mod_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}

static void calc_hash_for_unique(ulong &nr1, ulong &nr2, String *str)
{
  CHARSET_INFO *cs;
  uchar l[4];
  int4store(l, str->length());
  cs= str->charset();
  cs->hash_sort(l, sizeof(l), &nr1, &nr2);
  cs= str->charset();
  cs->hash_sort((uchar *)str->ptr(), str->length(), &nr1, &nr2);
}

longlong  Item_func_hash::val_int()
{
  DBUG_EXECUTE_IF("same_long_unique_hash", return 9;);
  unsigned_flag= true;
  ulong nr1= 1,nr2= 4;
  String * str;
  for(uint i= 0;i<arg_count;i++)
  {
    str = args[i]->val_str();
    if(args[i]->null_value)
    {
      null_value= 1;
      return 0;
    }
   calc_hash_for_unique(nr1, nr2, str);
  }
  null_value= 0;
  return   (longlong)nr1;
}


bool Item_func_hash::fix_length_and_dec(THD *thd)
{
  decimals= 0;
  max_length= 8;
  return false;
}



double Item_func_neg::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return -value;
}


longlong Item_func_neg::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  if (args[0]->unsigned_flag &&
      (ulonglong) value > (ulonglong) LONGLONG_MAX + 1)
    return raise_integer_overflow();

  if (value == LONGLONG_MIN)
  {
    if (args[0]->unsigned_flag != unsigned_flag)
      /* negation of LONGLONG_MIN is LONGLONG_MIN. */
      return LONGLONG_MIN; 
    else
      return raise_integer_overflow();
  }

  return check_integer_overflow(-value, !args[0]->unsigned_flag && value < 0);
}


my_decimal *Item_func_neg::decimal_op(my_decimal *decimal_value)
{
  VDec value(args[0]);
  if (!(null_value= value.is_null()))
  {
    my_decimal2decimal(value.ptr(), decimal_value);
    my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}


void Item_func_neg::fix_length_and_dec_int()
{
  max_length= args[0]->max_length + 1;
  set_handler(type_handler_long_or_longlong());

  /*
    If this is in integer context keep the context as integer if possible
    (This is how multiplication and other integer functions works)
    Use val() to get value as arg_type doesn't mean that item is
    Item_int or Item_float due to existence of Item_param.
  */
  if (args[0]->const_item())
  {
    longlong val= args[0]->val_int();
    if ((ulonglong) val >= (ulonglong) LONGLONG_MIN &&
        ((ulonglong) val != (ulonglong) LONGLONG_MIN ||
         !args[0]->is_of_type(CONST_ITEM, INT_RESULT)))
    {
      /*
        Ensure that result is converted to DECIMAL, as longlong can't hold
        the negated number
      */
      set_handler(&type_handler_newdecimal);
      DBUG_PRINT("info", ("Type changed: DECIMAL_RESULT"));
    }
  }
  unsigned_flag= false;
}


void Item_func_neg::fix_length_and_dec_double()
{
  set_handler(&type_handler_double);
  decimals= args[0]->decimals; // Preserve NOT_FIXED_DEC
  max_length= args[0]->max_length + 1;
  // Limit length with something reasonable
  uint32 mlen= type_handler()->max_display_length(this);
  set_if_smaller(max_length, mlen);
  unsigned_flag= false;
}


void Item_func_neg::fix_length_and_dec_decimal()
{
  set_handler(&type_handler_newdecimal);
  decimals= args[0]->decimal_scale(); // Do not preserve NOT_FIXED_DEC
  max_length= args[0]->max_length + 1;
  unsigned_flag= false;
}


bool Item_func_neg::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_neg::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  if (args[0]->cast_to_int_type_handler()->
      Item_func_neg_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}


double Item_func_abs::real_op()
{
  double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return fabs(value);
}


longlong Item_func_abs::int_op()
{
  longlong value= args[0]->val_int();
  if ((null_value= args[0]->null_value))
    return 0;
  if (unsigned_flag)
    return value;
  /* -LONGLONG_MIN = LONGLONG_MAX + 1 => outside of signed longlong range */
  if (value == LONGLONG_MIN)
    return raise_integer_overflow();
  return (value >= 0) ? value : -value;
}


my_decimal *Item_func_abs::decimal_op(my_decimal *decimal_value)
{
  VDec value(args[0]);
  if (!(null_value= value.is_null()))
  {
    my_decimal2decimal(value.ptr(), decimal_value);
    if (decimal_value->sign())
      my_decimal_neg(decimal_value);
    return decimal_value;
  }
  return 0;
}

void Item_func_abs::fix_length_and_dec_int()
{
  max_length= args[0]->max_length;
  unsigned_flag= args[0]->unsigned_flag;
  set_handler(type_handler_long_or_longlong());
}


void Item_func_abs::fix_length_and_dec_double()
{
  set_handler(&type_handler_double);
  decimals= args[0]->decimals; // Preserve NOT_FIXED_DEC
  max_length= float_length(decimals);
  unsigned_flag= args[0]->unsigned_flag;
}


void Item_func_abs::fix_length_and_dec_decimal()
{
  set_handler(&type_handler_newdecimal);
  decimals= args[0]->decimal_scale(); // Do not preserve NOT_FIXED_DEC
  max_length= args[0]->max_length;
  unsigned_flag= args[0]->unsigned_flag;
}


bool Item_func_abs::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_abs::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  if (args[0]->cast_to_int_type_handler()->
      Item_func_abs_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}


/** Gateway to natural LOG function. */
double Item_func_ln::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log(value);
}

/** 
  Extended but so slower LOG function.

  We have to check if all values are > zero and first one is not one
  as these are the cases then result is not a number.
*/ 
double Item_func_log::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  if (arg_count == 2)
  {
    double value2= args[1]->val_real();
    if ((null_value= args[1]->null_value))
      return 0.0;
    if (value2 <= 0.0 || value == 1.0)
    {
      signal_divide_by_null();
      return 0.0;
    }
    return log(value2) / log(value);
  }
  return log(value);
}

double Item_func_log2::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();

  if ((null_value=args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log(value) / M_LN2;
}

double Item_func_log10::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value= args[0]->null_value))
    return 0.0;
  if (value <= 0.0)
  {
    signal_divide_by_null();
    return 0.0;
  }
  return log10(value);
}

double Item_func_exp::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0; /* purecov: inspected */
  return check_float_overflow(exp(value));
}

double Item_func_sqrt::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || value < 0)))
    return 0.0; /* purecov: inspected */
  return sqrt(value);
}

double Item_func_pow::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  double val2= args[1]->val_real();
  if ((null_value=(args[0]->null_value || args[1]->null_value)))
    return 0.0; /* purecov: inspected */
  return check_float_overflow(pow(value,val2));
}

// Trigonometric functions

double Item_func_acos::val_real()
{
  DBUG_ASSERT(fixed());
  /* One can use this to defer SELECT processing. */
  DEBUG_SYNC(current_thd, "before_acos_function");
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return acos(value);
}

double Item_func_asin::val_real()
{
  DBUG_ASSERT(fixed());
  // the volatile's for BUG #2338 to calm optimizer down (because of gcc's bug)
  volatile double value= args[0]->val_real();
  if ((null_value=(args[0]->null_value || (value < -1.0 || value > 1.0))))
    return 0.0;
  return asin(value);
}

double Item_func_atan::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  if (arg_count == 2)
  {
    double val2= args[1]->val_real();
    if ((null_value=args[1]->null_value))
      return 0.0;
    return check_float_overflow(atan2(value,val2));
  }
  return atan(value);
}

double Item_func_cos::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return cos(value);
}

double Item_func_sin::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return sin(value);
}

double Item_func_tan::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return check_float_overflow(tan(value));
}


double Item_func_cot::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0.0;
  return check_float_overflow(1.0 / tan(value));
}


// Shift-functions, same as << and >> in C/C++


class Func_handler_shift_left_int_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return item->arguments()[0]->to_longlong_null() <<
           item->arguments()[1]->to_longlong_null();
  }
};


class Func_handler_shift_left_decimal_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return VDec(item->arguments()[0]).to_xlonglong_null() <<
           item->arguments()[1]->to_longlong_null();
  }
};


bool Item_func_shift_left::fix_length_and_dec(THD *thd)
{
  static Func_handler_shift_left_int_to_ulonglong ha_int_to_ull;
  static Func_handler_shift_left_decimal_to_ulonglong ha_dec_to_ull;
  return fix_length_and_dec_op1_std(&ha_int_to_ull, &ha_dec_to_ull);
}


class Func_handler_shift_right_int_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return item->arguments()[0]->to_longlong_null() >>
           item->arguments()[1]->to_longlong_null();
  }
};


class Func_handler_shift_right_decimal_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return VDec(item->arguments()[0]).to_xlonglong_null() >>
           item->arguments()[1]->to_longlong_null();
  }
};


bool Item_func_shift_right::fix_length_and_dec(THD *thd)
{
  static Func_handler_shift_right_int_to_ulonglong ha_int_to_ull;
  static Func_handler_shift_right_decimal_to_ulonglong ha_dec_to_ull;
  return fix_length_and_dec_op1_std(&ha_int_to_ull, &ha_dec_to_ull);
}


class Func_handler_bit_neg_int_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return ~ item->arguments()[0]->to_longlong_null();
  }
};


class Func_handler_bit_neg_decimal_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return ~ VDec(item->arguments()[0]).to_xlonglong_null();
  }
};


bool Item_func_bit_neg::fix_length_and_dec(THD *thd)
{
  static Func_handler_bit_neg_int_to_ulonglong ha_int_to_ull;
  static Func_handler_bit_neg_decimal_to_ulonglong ha_dec_to_ull;
  return fix_length_and_dec_op1_std(&ha_int_to_ull, &ha_dec_to_ull);
}


// Conversion functions

void Item_func_int_val::fix_length_and_dec_int_or_decimal()
{
  DBUG_ASSERT(args[0]->cmp_type() == DECIMAL_RESULT);
  DBUG_ASSERT(args[0]->max_length <= DECIMAL_MAX_STR_LENGTH);
  /*
    FLOOR() for negative numbers can increase length:   floor(-9.9) -> -10
    CEILING() for positive numbers can increase length:  ceil(9.9)  -> 10
  */
  decimal_round_mode mode= round_mode();
  uint length_increase= args[0]->decimals > 0 &&
                        (mode == CEILING ||
                         (mode == FLOOR && !args[0]->unsigned_flag)) ? 1 : 0;
  uint precision= args[0]->decimal_int_part() + length_increase;
  set_if_bigger(precision, 1);

  /*
    The BIGINT data type can store:
    UNSIGNED BIGINT: 0..18446744073709551615                     - up to 19 digits
      SIGNED BIGINT:   -9223372036854775808..9223372036854775807 - up to 18 digits

    The INT data type can store:
        UNSIGNED INT:  0..4294967295          - up to 9 digits
          SIGNED INT: -2147483648..2147483647 - up to 9 digits
  */
  if (precision > 18)
  {
    unsigned_flag= args[0]->unsigned_flag;
    fix_char_length(
      my_decimal_precision_to_length_no_truncation(precision, 0,
                                                   unsigned_flag));
    set_handler(&type_handler_newdecimal);
  }
  else
  {
    uint sign_length= (unsigned_flag= args[0]->unsigned_flag) ? 0 : 1;
    fix_char_length(precision + sign_length);
    if (precision > 9)
    {
      if (unsigned_flag)
        set_handler(&type_handler_ulonglong);
      else
        set_handler(&type_handler_slonglong);
    }
    else
    {
      if (unsigned_flag)
        set_handler(&type_handler_ulong);
      else
        set_handler(&type_handler_slong);
    }
  }
}


void Item_func_int_val::fix_length_and_dec_double()
{
  set_handler(&type_handler_double);
  max_length= float_length(0);
  decimals= 0;
}


bool Item_func_int_val::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_int_val::fix_length_and_dec");
  DBUG_PRINT("info", ("name %s", func_name()));
  /*
    We don't want to translate ENUM/SET to CHAR here.
    So let's call real_type_handler(), not type_handler().
  */
  if (args[0]->real_type_handler()->Item_func_int_val_fix_length_and_dec(this))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("Type: %s", real_type_handler()->name().ptr()));
  DBUG_RETURN(FALSE);
}


longlong Item_func_ceiling::int_op()
{
  switch (args[0]->result_type()) {
  case STRING_RESULT: // hex hybrid
  case INT_RESULT:
    return val_int_from_item(args[0]);
  case DECIMAL_RESULT:
    return VDec_op(this).to_longlong(unsigned_flag);
  default:
    break;
  }
  return (longlong) Item_func_ceiling::real_op();
}


double Item_func_ceiling::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return ceil(value);
}


my_decimal *Item_func_ceiling::decimal_op(my_decimal *decimal_value)
{
  VDec value(args[0]);
  if (!(null_value= (value.is_null() ||
                     value.round_to(decimal_value, 0, CEILING) > 1)))
    return decimal_value;
  return 0;
}


bool Item_func_ceiling::date_op(THD *thd, MYSQL_TIME *to,
                                date_mode_t fuzzydate)
{
  Datetime::Options opt(thd, TIME_FRAC_TRUNCATE);
  Datetime *tm= new (to) Datetime(thd, args[0], opt);
  tm->ceiling(thd);
  null_value= !tm->is_valid_datetime();
  DBUG_ASSERT(maybe_null() || !null_value);
  return null_value;
}


bool Item_func_ceiling::time_op(THD *thd, MYSQL_TIME *to)
{
  static const Time::Options_for_round opt;
  Time *tm= new (to) Time(thd, args[0], opt);
  tm->ceiling();
  null_value= !tm->is_valid_time();
  DBUG_ASSERT(maybe_null() || !null_value);
  return null_value;
}


longlong Item_func_floor::int_op()
{
  switch (args[0]->result_type()) {
  case STRING_RESULT: // hex hybrid
  case INT_RESULT:
    return val_int_from_item(args[0]);
  case DECIMAL_RESULT:
  {
    my_decimal dec_buf, *dec;
    return (!(dec= Item_func_floor::decimal_op(&dec_buf))) ? 0 :
           dec->to_longlong(unsigned_flag);
  }
  default:
    break;
  }
  return (longlong) Item_func_floor::real_op();
}


double Item_func_floor::real_op()
{
  /*
    the volatile's for BUG #3051 to calm optimizer down (because of gcc's
    bug)
  */
  volatile double value= args[0]->val_real();
  null_value= args[0]->null_value;
  return floor(value);
}


my_decimal *Item_func_floor::decimal_op(my_decimal *decimal_value)
{
  VDec value(args[0]);
  if (!(null_value= (value.is_null() ||
                     value.round_to(decimal_value, 0, FLOOR) > 1)))
    return decimal_value;
  return 0;
}


bool Item_func_floor::date_op(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate)
{
  // DATETIME is not negative, so FLOOR means just truncation
  Datetime::Options opt(thd, TIME_FRAC_TRUNCATE);
  Datetime *tm= new (to) Datetime(thd, args[0], opt, 0);
  null_value= !tm->is_valid_datetime();
  DBUG_ASSERT(maybe_null() || !null_value);
  return null_value;
}


bool Item_func_floor::time_op(THD *thd, MYSQL_TIME *to)
{
  static const Time::Options_for_round opt;
  Time *tm= new (to) Time(thd, args[0], opt);
  tm->floor();
  null_value= !tm->is_valid_time();
  DBUG_ASSERT(maybe_null() || !null_value);
  return null_value;
}


void Item_func_round::fix_length_and_dec_decimal(uint decimals_to_set)
{
  int decimals_delta= args[0]->decimals - decimals_to_set;
  int length_increase= (decimals_delta <= 0 || truncate) ? 0 : 1;
  int precision= args[0]->decimal_precision() + length_increase -
                                                decimals_delta;
  DBUG_ASSERT(decimals_to_set <= DECIMAL_MAX_SCALE);
  set_handler(&type_handler_newdecimal);
  unsigned_flag= args[0]->unsigned_flag;
  decimals= decimals_to_set;
  if (!precision)
    precision= 1; // DECIMAL(0,0) -> DECIMAL(1,0)
  max_length= my_decimal_precision_to_length_no_truncation(precision,
                                                           decimals,
                                                           unsigned_flag);
}

void Item_func_round::fix_length_and_dec_double(uint decimals_to_set)
{
  set_handler(&type_handler_double);
  unsigned_flag= args[0]->unsigned_flag;
  decimals= decimals_to_set;
  max_length= float_length(decimals_to_set);
}


void Item_func_round::fix_arg_decimal()
{
  if (args[1]->const_item())
  {
    Longlong_hybrid dec= args[1]->to_longlong_hybrid();
    if (args[1]->null_value)
      fix_length_and_dec_double(NOT_FIXED_DEC);
    else
      fix_length_and_dec_decimal(dec.to_uint(DECIMAL_MAX_SCALE));
  }
  else
  {
    set_handler(&type_handler_newdecimal);
    unsigned_flag= args[0]->unsigned_flag;
    decimals= args[0]->decimals;
    max_length= args[0]->max_length;
  }
}


void Item_func_round::fix_arg_double()
{
  if (args[1]->const_item())
  {
    Longlong_hybrid dec= args[1]->to_longlong_hybrid();
    fix_length_and_dec_double(args[1]->null_value ? NOT_FIXED_DEC :
                              dec.to_uint(NOT_FIXED_DEC));
  }
  else
    fix_length_and_dec_double(args[0]->decimals);
}


void Item_func_round::fix_arg_temporal(const Type_handler *h,
                                       uint int_part_length)
{
  set_handler(h);
  if (args[1]->can_eval_in_optimize())
  {
    Longlong_hybrid_null dec= args[1]->to_longlong_hybrid_null();
    fix_attributes_temporal(int_part_length,
                            dec.is_null() ? args[0]->decimals :
                            dec.to_uint(TIME_SECOND_PART_DIGITS));
  }
  else
    fix_attributes_temporal(int_part_length, args[0]->decimals);
}


void Item_func_round::fix_arg_time()
{
  fix_arg_temporal(&type_handler_time2, MIN_TIME_WIDTH);
}


void Item_func_round::fix_arg_datetime()
{
  /*
    Day increment operations are not supported for '0000-00-00',
    see get_date_from_daynr() for details. Therefore, expressions like
      ROUND('0000-00-00 23:59:59.999999')
    return NULL.
  */
  if (!truncate)
    set_maybe_null();
  fix_arg_temporal(&type_handler_datetime2, MAX_DATETIME_WIDTH);
}


bool Item_func_round::test_if_length_can_increase()
{
  if (truncate)
    return false;
  if (args[1]->can_eval_in_optimize())
  {
    // Length can increase in some cases: e.g. ROUND(9,-1) -> 10.
    Longlong_hybrid val1= args[1]->to_longlong_hybrid();
    return !args[1]->null_value && val1.neg();
  }
  return true; // ROUND(x,n), where n is not a constant.
}


/**
  Calculate data type and attributes for INT-alike input.

  @param [IN] preferred - The preferred data type handler for simple cases
                          such as ROUND(x) and TRUNCATE(x,0), when the input
                          is short enough to fit into an integer type
                          (without extending to DECIMAL).
                          - If `preferred` is not NULL, then the code tries
                            to preserve the given data type handler and
                            the data type attributes `preferred_attrs`.
                          - If `preferred` is NULL, then the code fully
                            calculates attributes using
                            args[0]->decimal_precision() and chooses between
                            INT and BIGINT, depending on attributes.
  @param [IN] preferred_attrs - The preferred data type attributes for
                                simple cases.
*/
void Item_func_round::fix_arg_int(const Type_handler *preferred,
                                  const Type_std_attributes *preferred_attrs,
                                  bool use_decimal_on_length_increase)
{
  DBUG_ASSERT(args[0]->decimals == 0);

  Type_std_attributes::set(preferred_attrs);
  if (!test_if_length_can_increase())
  {
    // Preserve the exact data type and attributes
    set_handler(preferred);
  }
  else
  {
    max_length++;
    if (use_decimal_on_length_increase)
      set_handler(&type_handler_newdecimal);
    else
      set_handler(type_handler_long_or_longlong());
  }
}


void Item_func_round::fix_arg_hex_hybrid()
{
  DBUG_ASSERT(args[0]->decimals == 0);
  DBUG_ASSERT(args[0]->decimal_precision() < DECIMAL_LONGLONG_DIGITS);
  DBUG_ASSERT(args[0]->unsigned_flag); // no needs to add sign length
  bool length_can_increase= test_if_length_can_increase();
  max_length= args[0]->decimal_precision() + MY_TEST(length_can_increase);
  unsigned_flag= true;
  decimals= 0;
  if (length_can_increase && args[0]->max_length >= 8)
    set_handler(&type_handler_newdecimal);
  else
    set_handler(type_handler_long_or_longlong());
}


double my_double_round(double value, longlong dec, bool dec_unsigned,
                       bool truncate)
{
  double tmp;
  bool dec_negative= (dec < 0) && !dec_unsigned;
  ulonglong abs_dec= dec_negative ? -dec : dec;
  /*
    tmp2 is here to avoid return the value with 80 bit precision
    This will fix that the test round(0.1,1) = round(0.1,1) is true
    Tagging with volatile is no guarantee, it may still be optimized away...
  */
  volatile double tmp2;

  tmp=(abs_dec < array_elements(log_10) ?
       log_10[abs_dec] : pow(10.0,(double) abs_dec));

  // Pre-compute these, to avoid optimizing away e.g. 'floor(v/tmp) * tmp'.
  volatile double value_div_tmp= value / tmp;
  volatile double value_mul_tmp= value * tmp;

  if (!dec_negative && std::isinf(tmp)) // "dec" is too large positive number
    return value;

  if (dec_negative && std::isinf(tmp))
    tmp2= 0.0;
  else if (!dec_negative && std::isinf(value_mul_tmp))
    tmp2= value;
  else if (truncate)
  {
    if (value >= 0.0)
      tmp2= dec < 0 ? floor(value_div_tmp) * tmp : floor(value_mul_tmp) / tmp;
    else
      tmp2= dec < 0 ? ceil(value_div_tmp) * tmp : ceil(value_mul_tmp) / tmp;
  }
  else
    tmp2=dec < 0 ? rint(value_div_tmp) * tmp : rint(value_mul_tmp) / tmp;

  return tmp2;
}


double Item_func_round::real_op()
{
  double value= args[0]->val_real();

  if (!(null_value= args[0]->null_value))
  {
    longlong dec= args[1]->val_int();
    if (!(null_value= args[1]->null_value))
      return my_double_round(value, dec, args[1]->unsigned_flag, truncate);
  }
  return 0.0;
}

/*
  Rounds a given value to a power of 10 specified as the 'to' argument,
  avoiding overflows when the value is close to the ulonglong range boundary.
*/

static inline ulonglong my_unsigned_round(ulonglong value, ulonglong to)
{
  ulonglong tmp= value / to * to;
  return (value - tmp < (to >> 1)) ? tmp : tmp + to;
}


longlong Item_func_round::int_op()
{
  longlong value= args[0]->val_int();
  longlong dec= args[1]->val_int();
  decimals= 0;
  ulonglong abs_dec;
  if ((null_value= args[0]->null_value || args[1]->null_value))
    return 0;
  if ((dec >= 0) || args[1]->unsigned_flag)
    return value; // integer have not digits after point

  abs_dec= -dec;
  longlong tmp;
  
  if(abs_dec >= array_elements(log_10_int))
    return 0;
  
  tmp= log_10_int[abs_dec];
  
  if (truncate)
    value= (unsigned_flag) ?
      ((ulonglong) value / tmp) * tmp : (value / tmp) * tmp;
  else
    value= (unsigned_flag || value >= 0) ?
      my_unsigned_round((ulonglong) value, tmp) :
      -(longlong) my_unsigned_round((ulonglong) -value, tmp);
  return value;
}


my_decimal *Item_func_round::decimal_op(my_decimal *decimal_value)
{
  VDec value(args[0]);
  longlong dec= args[1]->val_int();
  if (dec >= 0 || args[1]->unsigned_flag)
    dec= MY_MIN((ulonglong) dec, decimals);
  else if (dec < INT_MIN)
    dec= INT_MIN;
    
  if (!(null_value= (value.is_null() || args[1]->null_value ||
                     value.round_to(decimal_value, (int) dec,
                                    truncate ? TRUNCATE : HALF_UP) > 1)))
    return decimal_value;
  return 0;
}


bool Item_func_round::time_op(THD *thd, MYSQL_TIME *to)
{
  DBUG_ASSERT(args[0]->type_handler()->mysql_timestamp_type() ==
              MYSQL_TIMESTAMP_TIME);
  Time::Options_for_round opt(truncate ? TIME_FRAC_TRUNCATE : TIME_FRAC_ROUND);
  Longlong_hybrid_null dec= args[1]->to_longlong_hybrid_null();
  Time *tm= new (to) Time(thd, args[0], opt,
                          dec.to_uint(TIME_SECOND_PART_DIGITS));
  null_value= !tm->is_valid_time() || dec.is_null();
  DBUG_ASSERT(maybe_null() || !null_value);
  return null_value;
}


bool Item_func_round::date_op(THD *thd, MYSQL_TIME *to, date_mode_t fuzzydate)
{
  DBUG_ASSERT(args[0]->type_handler()->mysql_timestamp_type() ==
              MYSQL_TIMESTAMP_DATETIME);
  Datetime::Options opt(thd, truncate ? TIME_FRAC_TRUNCATE : TIME_FRAC_ROUND);
  Longlong_hybrid_null dec= args[1]->to_longlong_hybrid_null();
  Datetime *tm= new (to) Datetime(thd, args[0], opt,
                                  dec.to_uint(TIME_SECOND_PART_DIGITS));
  null_value= !tm->is_valid_datetime() || dec.is_null();
  DBUG_ASSERT(maybe_null() || !null_value);
  return null_value;
}


void Item_func_rand::seed_random(Item *arg)
{
  /*
    TODO: do not do reinit 'rand' for every execute of PS/SP if
    args[0] is a constant.
  */
  uint32 tmp= (uint32) arg->val_int();
#ifdef WITH_WSREP
  if (WSREP_ON)
  {
    THD *thd= current_thd;
    if (WSREP(thd))
    {
      if (wsrep_thd_is_applying(thd))
        tmp= thd->wsrep_rand;
      else
        thd->wsrep_rand= tmp;
    }
  }
#endif /* WITH_WSREP */

  my_rnd_init(rand, (uint32) (tmp*0x10001L+55555555L),
             (uint32) (tmp*0x10000001L));
}


bool Item_func_rand::fix_fields(THD *thd,Item **ref)
{
  if (Item_real_func::fix_fields(thd, ref))
    return TRUE;
  used_tables_cache|= RAND_TABLE_BIT;
  if (arg_count)
  {					// Only use argument once in query
    /*
      Allocate rand structure once: we must use thd->stmt_arena
      to create rand in proper mem_root if it's a prepared statement or
      stored procedure.

      No need to send a Rand log event if seed was given eg: RAND(seed),
      as it will be replicated in the query as such.
    */
    if (!rand && !(rand= (struct my_rnd_struct*)
                   thd->stmt_arena->alloc(sizeof(*rand))))
      return TRUE;
  }
  else
  {
    /*
      Save the seed only the first time RAND() is used in the query
      Once events are forwarded rather than recreated,
      the following can be skipped if inside the slave thread
    */
    if (!thd->rand_used)
    {
      thd->rand_used= 1;
      thd->rand_saved_seed1= thd->rand.seed1;
      thd->rand_saved_seed2= thd->rand.seed2;
    }
    rand= &thd->rand;
  }
  return FALSE;
}

void Item_func_rand::update_used_tables()
{
  Item_real_func::update_used_tables();
  used_tables_cache|= RAND_TABLE_BIT;
}


double Item_func_rand::val_real()
{
  DBUG_ASSERT(fixed());
  if (arg_count)
  {
    if (!args[0]->const_item())
      seed_random(args[0]);
    else if (first_eval)
    {
      /*
        Constantness of args[0] may be set during JOIN::optimize(), if arg[0]
        is a field item of "constant" table. Thus, we have to evaluate
        seed_random() for constant arg there but not at the fix_fields method.
      */
      first_eval= FALSE;
      seed_random(args[0]);
    }
  }
  return my_rnd(rand);
}

longlong Item_func_sign::val_int()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  null_value=args[0]->null_value;
  return value < 0.0 ? -1 : (value > 0 ? 1 : 0);
}


double Item_func_units::val_real()
{
  DBUG_ASSERT(fixed());
  double value= args[0]->val_real();
  if ((null_value=args[0]->null_value))
    return 0;
  return check_float_overflow(value * mul + add);
}


bool Item_func_min_max::fix_attributes(Item **items, uint nitems)
{
  bool rc= Item_func_min_max::type_handler()->
             Item_func_min_max_fix_attributes(current_thd, this, items, nitems);
  DBUG_ASSERT(!rc || current_thd->is_error());
  return rc;
}


/*
  Compare item arguments using DATETIME/DATE/TIME representation.

  DESCRIPTION
    Compare item arguments as DATETIME values and return the index of the
    least/greatest argument in the arguments array.
    The correct DATE/DATETIME value of the found argument is
    stored to the value pointer, if latter is provided.

  RETURN
   1	If one of arguments is NULL or there was a execution error
   0    Otherwise
*/

bool Item_func_min_max::get_date_native(THD *thd, MYSQL_TIME *ltime,
                                        date_mode_t fuzzydate)
{
  longlong UNINIT_VAR(min_max);
  DBUG_ASSERT(fixed());

  for (uint i=0; i < arg_count ; i++)
  {
    longlong res= args[i]->val_datetime_packed(thd);

    /* Check if we need to stop (because of error or KILL) and stop the loop */
    if (unlikely(args[i]->null_value))
      return (null_value= 1);

    if (i == 0 || (res < min_max ? cmp_sign : -cmp_sign) > 0)
      min_max= res;
  }
  unpack_time(min_max, ltime, mysql_timestamp_type());

  if (!(fuzzydate & TIME_TIME_ONLY) &&
      unlikely((null_value= check_date_with_warn(thd, ltime, fuzzydate,
                                         MYSQL_TIMESTAMP_ERROR))))
    return true;

  return (null_value= 0);
}


bool Item_func_min_max::get_time_native(THD *thd, MYSQL_TIME *ltime)
{
  DBUG_ASSERT(fixed());

  Time value(thd, args[0], Time::Options(thd), decimals);
  if (!value.is_valid_time())
    return (null_value= true);

  for (uint i= 1; i < arg_count ; i++)
  {
    Time tmp(thd, args[i], Time::Options(thd), decimals);
    if (!tmp.is_valid_time())
      return (null_value= true);

    int cmp= value.cmp(&tmp);
    if ((cmp_sign < 0 ? cmp : -cmp) < 0)
      value= tmp;
  }
  value.copy_to_mysql_time(ltime);
  return (null_value= 0);
}


String *Item_func_min_max::val_str_native(String *str)
{
  String *UNINIT_VAR(res);
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      res=args[i]->val_str(str);
    else
    {
      String *res2;
      res2= args[i]->val_str(res == str ? &tmp_value : str);
      if (res2)
      {
        int cmp= sortcmp(res,res2,collation.collation);
        if ((cmp_sign < 0 ? cmp : -cmp) < 0)
          res=res2;
      }
    }
    if ((null_value= args[i]->null_value))
      return 0;
  }
  res->set_charset(collation.collation);
  return res;
}


double Item_func_min_max::val_real_native()
{
  double value=0.0;
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value= args[i]->val_real();
    else
    {
      double tmp= args[i]->val_real();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


longlong Item_func_min_max::val_int_native()
{
  DBUG_ASSERT(fixed());
  longlong value=0;
  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      value=args[i]->val_int();
    else
    {
      longlong tmp=args[i]->val_int();
      if (!args[i]->null_value && (tmp < value ? cmp_sign : -cmp_sign) > 0)
	value=tmp;
    }
    if ((null_value= args[i]->null_value))
      break;
  }
  return value;
}


my_decimal *Item_func_min_max::val_decimal_native(my_decimal *dec)
{
  DBUG_ASSERT(fixed());
  my_decimal tmp_buf, *tmp, *UNINIT_VAR(res);

  for (uint i=0; i < arg_count ; i++)
  {
    if (i == 0)
      res= args[i]->val_decimal(dec);
    else
    {
      tmp= args[i]->val_decimal(&tmp_buf);      // Zero if NULL
      if (tmp && (my_decimal_cmp(tmp, res) * cmp_sign) < 0)
      {
        if (tmp == &tmp_buf)
        {
          /* Move value out of tmp_buf as this will be reused on next loop */
          my_decimal2decimal(tmp, dec);
          res= dec;
        }
        else
          res= tmp;
      }
    }
    if ((null_value= args[i]->null_value))
    {
      res= 0;
      break;
    }
  }
  return res;
}


bool Item_func_min_max::val_native(THD *thd, Native *native)
{
  DBUG_ASSERT(fixed());
  const Type_handler *handler= Item_hybrid_func::type_handler();
  NativeBuffer<STRING_BUFFER_USUAL_SIZE> cur;
  for (uint i= 0; i < arg_count; i++)
  {
    if (val_native_with_conversion_from_item(thd, args[i],
                                             i == 0 ? native : &cur,
                                             handler))
      return true;
    if (i > 0)
    {
      int cmp= handler->cmp_native(*native, cur);
      if ((cmp_sign < 0 ? cmp : -cmp) < 0 && native->copy(cur))
        return null_value= true;
    }
  }
  return null_value= false;
}


longlong Item_func_bit_length::val_int()
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&value);
  return (null_value= !res) ? 0 : (longlong) res->length() * 8;
}


longlong Item_func_octet_length::val_int()
{
  DBUG_ASSERT(fixed());
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->length();
}


longlong Item_func_char_length::val_int()
{
  DBUG_ASSERT(fixed());
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  return (longlong) res->numchars();
}


longlong Item_func_coercibility::val_int()
{
  DBUG_ASSERT(fixed());
  null_value= 0;
  return (longlong) args[0]->collation.derivation;
}


longlong Item_func_locate::val_int()
{
  DBUG_ASSERT(fixed());
  String *a=args[0]->val_str(&value1);
  String *b=args[1]->val_str(&value2);
  if (!a || !b)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;
  /* must be longlong to avoid truncation */
  longlong start=  0; 
  longlong start0= 0;
  my_match_t match;

  if (arg_count == 3)
  {
    start0= start= args[2]->val_int();

    if ((start <= 0) || (start > a->length()))
      return 0;
    start0--; start--;

    /* start is now sufficiently valid to pass to charpos function */
    start= a->charpos((int) start);

    if (start + b->length() > a->length())
      return 0;
  }

  if (!b->length())				// Found empty string at start
    return start + 1;
  
  if (!cmp_collation.collation->instr(a->ptr() + start,
                                      (uint) (a->length() - start),
                                      b->ptr(), b->length(),
                                      &match, 1))
    return 0;
  return (longlong) match.mb_len + start0 + 1;
}


void Item_func_locate::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("locate("));
  args[1]->print(str, query_type);
  str->append(',');
  args[0]->print(str, query_type);
  if (arg_count == 3)
  {
    str->append(',');
    args[2]->print(str, query_type);
  }
  str->append(')');
}


longlong Item_func_field::val_int()
{
  DBUG_ASSERT(fixed());

  if (cmp_type == STRING_RESULT)
  {
    String *field;
    if (!(field= args[0]->val_str(&value)))
      return 0;
    for (uint i=1 ; i < arg_count ; i++)
    {
      String *tmp_value=args[i]->val_str(&tmp);
      if (tmp_value && !sortcmp(field,tmp_value,cmp_collation.collation))
        return (longlong) (i);
    }
  }
  else if (cmp_type == INT_RESULT)
  {
    longlong val= args[0]->val_int();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_int() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  else if (cmp_type == DECIMAL_RESULT)
  {
    VDec dec(args[0]);
    if (dec.is_null())
      return 0;
    my_decimal dec_arg_buf;
    for (uint i=1; i < arg_count; i++)
    {
      my_decimal *dec_arg= args[i]->val_decimal(&dec_arg_buf);
      if (!args[i]->null_value && !dec.cmp(dec_arg))
        return (longlong) (i);
    }
  }
  else
  {
    double val= args[0]->val_real();
    if (args[0]->null_value)
      return 0;
    for (uint i=1; i < arg_count ; i++)
    {
      if (val == args[i]->val_real() && !args[i]->null_value)
        return (longlong) (i);
    }
  }
  return 0;
}


bool Item_func_field::fix_length_and_dec(THD *thd)
{
  base_flags&= ~item_base_t::MAYBE_NULL;
  max_length=3;
  cmp_type= args[0]->result_type();
  for (uint i=1; i < arg_count ; i++)
    cmp_type= item_cmp_type(cmp_type, args[i]->result_type());
  if (cmp_type == STRING_RESULT)
    return agg_arg_charsets_for_comparison(cmp_collation, args, arg_count);
  return FALSE;
}


longlong Item_func_ascii::val_int()
{
  DBUG_ASSERT(fixed());
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  return (longlong) (res->length() ? (uchar) (*res)[0] : (uchar) 0);
}

longlong Item_func_ord::val_int()
{
  DBUG_ASSERT(fixed());
  String *res=args[0]->val_str(&value);
  if (!res)
  {
    null_value=1;
    return 0;
  }
  null_value=0;
  if (!res->length()) return 0;
#ifdef USE_MB
  if (res->use_mb())
  {
    const char *str=res->ptr();
    uint32 n=0, l=my_ismbchar(res->charset(),str,str+res->length());
    if (!l)
      return (longlong)((uchar) *str);
    while (l--)
      n=(n<<8)|(uint32)((uchar) *str++);
    return (longlong) n;
  }
#endif
  return (longlong) ((uchar) (*res)[0]);
}

	/* Search after a string in a string of strings separated by ',' */
	/* Returns number of found type >= 1 or 0 if not found */
	/* This optimizes searching in enums to bit testing! */

bool Item_func_find_in_set::fix_length_and_dec(THD *thd)
{
  decimals=0;
  max_length=3;					// 1-999
  if (args[0]->const_item() && args[1]->type() == FIELD_ITEM)
  {
    Field *field= ((Item_field*) args[1])->field;
    if (field->real_type() == MYSQL_TYPE_SET)
    {
      String *find=args[0]->val_str(&value);
      if (find)
      {
        // find is not NULL pointer so args[0] is not a null-value
        DBUG_ASSERT(!args[0]->null_value);
	enum_value= find_type(((Field_enum*) field)->typelib,find->ptr(),
			      find->length(), 0);
	enum_bit=0;
	if (enum_value)
	  enum_bit= 1ULL << (enum_value-1);
      }
    }
  }
  return agg_arg_charsets_for_comparison(cmp_collation, args, 2);
}

static const char separator=',';

longlong Item_func_find_in_set::val_int()
{
  DBUG_ASSERT(fixed());
  if (enum_value)
  {
    // enum_value is set iff args[0]->const_item() in fix_length_and_dec().
    DBUG_ASSERT(args[0]->const_item());

    ulonglong tmp= (ulonglong) args[1]->val_int();
    null_value= args[1]->null_value;
    /* 
      No need to check args[0]->null_value since enum_value is set iff
      args[0] is a non-null const item. Note: no DBUG_ASSERT on
      args[0]->null_value here because args[0] may have been replaced
      by an Item_cache on which val_int() has not been called. See
      BUG#11766317
    */
    if (!null_value)
    {
      if (tmp & enum_bit)
        return enum_value;
    }
    return 0L;
  }

  String *find=args[0]->val_str(&value);
  String *buffer=args[1]->val_str(&value2);
  if (!find || !buffer)
  {
    null_value=1;
    return 0; /* purecov: inspected */
  }
  null_value=0;

  if ((int) (buffer->length() - find->length()) >= 0)
  {
    my_wc_t wc= 0;
    CHARSET_INFO *cs= cmp_collation.collation;
    const char *str_begin= buffer->ptr();
    const char *str_end= buffer->ptr();
    const char *real_end= str_end+buffer->length();
    const char *find_str= find->ptr();
    uint find_str_len= find->length();
    int position= 0;
    while (1)
    {
      int symbol_len;
      if ((symbol_len= cs->mb_wc(&wc, (uchar*) str_end,
                                 (uchar*) real_end)) > 0)
      {
        const char *substr_end= str_end + symbol_len;
        bool is_last_item= (substr_end == real_end);
        bool is_separator= (wc == (my_wc_t) separator);
        if (is_separator || is_last_item)
        {
          position++;
          if (is_last_item && !is_separator)
            str_end= substr_end;
          if (!cs->strnncoll(str_begin, (uint) (str_end - str_begin),
                             find_str, find_str_len))
            return (longlong) position;
          else
            str_begin= substr_end;
        }
        str_end= substr_end;
      }
      else if (str_end - str_begin == 0 &&
               find_str_len == 0 &&
               wc == (my_wc_t) separator)
        return (longlong) ++position;
      else
        return 0;
    }
  }
  return 0;
}


class Func_handler_bit_count_int_to_slong:
        public Item_handled_func::Handler_slong2
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return item->arguments()[0]->to_longlong_null().bit_count();
  }
};


class Func_handler_bit_count_decimal_to_slong:
        public Item_handled_func::Handler_slong2
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return VDec(item->arguments()[0]).to_xlonglong_null().bit_count();
  }
};


bool Item_func_bit_count::fix_length_and_dec(THD *thd)
{
  static Func_handler_bit_count_int_to_slong ha_int_to_slong;
  static Func_handler_bit_count_decimal_to_slong ha_dec_to_slong;
  set_func_handler(args[0]->cmp_type() == INT_RESULT ?
                   (const Handler *) &ha_int_to_slong :
                   (const Handler *) &ha_dec_to_slong);
  return m_func_handler->fix_length_and_dec(this);
}


/****************************************************************************
** Functions to handle dynamic loadable functions
** Original source by: Alexis Mikhailov <root@medinf.chuvashia.su>
** Rewritten by monty.
****************************************************************************/

#ifdef HAVE_DLOPEN

void udf_handler::cleanup()
{
  if (!not_original)
  {
    if (initialized)
    {
      if (u_d->func_deinit != NULL)
      {
        Udf_func_deinit deinit= u_d->func_deinit;
        (*deinit)(&initid);
      }
      free_udf(u_d);
      initialized= FALSE;
    }
    if (buffers)				// Because of bug in ecc
      delete [] buffers;
    buffers= 0;
  }
}


bool
udf_handler::fix_fields(THD *thd, Item_func_or_sum *func,
			uint arg_count, Item **arguments)
{
  uchar buff[STACK_BUFF_ALLOC];			// Max argument in function
  DBUG_ENTER("Item_udf_func::fix_fields");

  if (check_stack_overrun(thd, STACK_MIN_SIZE, buff))
    DBUG_RETURN(TRUE);				// Fatal error flag is set!

  udf_func *tmp_udf=find_udf(u_d->name.str,u_d->name.length,1);

  if (!tmp_udf)
  {
    my_error(ER_CANT_FIND_UDF, MYF(0), u_d->name.str);
    DBUG_RETURN(TRUE);
  }
  u_d=tmp_udf;
  args=arguments;

  /* Fix all arguments */
  func->base_flags&= ~item_base_t::MAYBE_NULL;
  func->used_tables_and_const_cache_init();

  if ((f_args.arg_count=arg_count))
  {
    if (!(f_args.arg_type= (Item_result*)
	  thd->alloc(f_args.arg_count*sizeof(Item_result))))

    {
    err_exit:
      free_udf(u_d);
      DBUG_RETURN(TRUE);
    }
    uint i;
    Item **arg,**arg_end;
    for (i=0, arg=arguments, arg_end=arguments+arg_count;
	 arg != arg_end ;
	 arg++,i++)
    {
      if ((*arg)->fix_fields_if_needed_for_scalar(thd, arg))
	DBUG_RETURN(true);
      // we can't assign 'item' before, because fix_fields() can change arg
      Item *item= *arg;
      /*
	TODO: We should think about this. It is not always
	right way just to set an UDF result to return my_charset_bin
	if one argument has binary sorting order.
	The result collation should be calculated according to arguments
	derivations in some cases and should not in other cases.
	Moreover, some arguments can represent a numeric input
	which doesn't effect the result character set and collation.
	There is no a general rule for UDF. Everything depends on
        the particular user defined function.
      */
      if (item->collation.collation->state & MY_CS_BINSORT)
	func->collation.set(&my_charset_bin);
      func->base_flags|= item->base_flags & item_base_t::MAYBE_NULL;
      func->with_flags|= item->with_flags;
      func->used_tables_and_const_cache_join(item);
      f_args.arg_type[i]=item->result_type();
    }
    buffers=new (thd->mem_root) String[arg_count];
    if (!buffers ||
        !multi_alloc_root(thd->mem_root,
                          &f_args.args,              arg_count * sizeof(char *),
                          &f_args.lengths,           arg_count * sizeof(long),
                          &f_args.maybe_null,        arg_count * sizeof(char),
                          &num_buffer,               arg_count * sizeof(double),
                          &f_args.attributes,        arg_count * sizeof(char *),
                          &f_args.attribute_lengths, arg_count * sizeof(long),
                          NullS))
      goto err_exit;
  }
  if (func->fix_length_and_dec(thd))
    DBUG_RETURN(TRUE);
  initid.max_length=func->max_length;
  initid.maybe_null=func->maybe_null();
  initid.const_item=func->const_item_cache;
  initid.decimals=func->decimals;
  initid.ptr=0;
  for (uint i1= 0 ; i1 < arg_count ; i1++)
    buffers[i1].set_thread_specific();

  if (u_d->func_init)
  {
    char init_msg_buff[MYSQL_ERRMSG_SIZE];
    char *to=num_buffer;
    for (uint i=0; i < arg_count; i++)
    {
      /*
       For a constant argument i, args->args[i] points to the argument value. 
       For non-constant, args->args[i] is NULL.
      */
      f_args.args[i]= NULL;         /* Non-const unless updated below. */

      f_args.lengths[i]= arguments[i]->max_length;
      f_args.maybe_null[i]= (char) arguments[i]->maybe_null();
      f_args.attributes[i]= arguments[i]->name.str;
      f_args.attribute_lengths[i]= (ulong)arguments[i]->name.length;

      if (arguments[i]->const_item())
      {
        switch (arguments[i]->result_type()) {
        case STRING_RESULT:
        case DECIMAL_RESULT:
        {
          String *res= arguments[i]->val_str(&buffers[i]);
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= (char*) res->c_ptr_safe();
          f_args.lengths[i]= res->length();
          break;
        }
        case INT_RESULT:
          *((longlong*) to)= arguments[i]->val_int();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(longlong));
          break;
        case REAL_RESULT:
          *((double*) to)= arguments[i]->val_real();
          if (arguments[i]->null_value)
            continue;
          f_args.args[i]= to;
          to+= ALIGN_SIZE(sizeof(double));
          break;
        case ROW_RESULT:
        case TIME_RESULT:
          DBUG_ASSERT(0);          // This case should never be chosen
          break;
        }
      }
    }
    Udf_func_init init= u_d->func_init;
    if (unlikely((error=(uchar) init(&initid, &f_args, init_msg_buff))))
    {
      my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
               u_d->name.str, init_msg_buff);
      goto err_exit;
    }
    func->max_length=MY_MIN(initid.max_length,MAX_BLOB_WIDTH);
    func->set_maybe_null(initid.maybe_null);
    /*
      The above call for init() can reset initid.const_item to "false",
      e.g. when the UDF function wants to be non-deterministic.
      See sequence_init() in udf_example.cc.
    */
    func->const_item_cache= initid.const_item;
    func->decimals=MY_MIN(initid.decimals,NOT_FIXED_DEC);
  }
  initialized=1;
  if (unlikely(error))
  {
    my_error(ER_CANT_INITIALIZE_UDF, MYF(0),
             u_d->name.str, ER_THD(thd, ER_UNKNOWN_ERROR));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


bool udf_handler::get_arguments()
{
  if (unlikely(error))
    return 1;					// Got an error earlier
  char *to= num_buffer;
  uint str_count=0;
  for (uint i=0; i < f_args.arg_count; i++)
  {
    f_args.args[i]=0;
    switch (f_args.arg_type[i]) {
    case STRING_RESULT:
    case DECIMAL_RESULT:
      {
	String *res=args[i]->val_str(&buffers[str_count++]);
	if (!(args[i]->null_value))
	{
	  f_args.args[i]=    (char*) res->ptr();
	  f_args.lengths[i]= res->length();
	}
	else
	{
	  f_args.lengths[i]= 0;
	}
	break;
      }
    case INT_RESULT:
      *((longlong*) to) = args[i]->val_int();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(longlong));
      }
      break;
    case REAL_RESULT:
      *((double*) to)= args[i]->val_real();
      if (!args[i]->null_value)
      {
	f_args.args[i]=to;
	to+= ALIGN_SIZE(sizeof(double));
      }
      break;
    case ROW_RESULT:
    case TIME_RESULT:
      DBUG_ASSERT(0);              // This case should never be chosen
      break;
    }
  }
  return 0;
}

/**
  @return
    (String*)NULL in case of NULL values
*/
String *udf_handler::val_str(String *str,String *save_str)
{
  uchar is_null_tmp=0;
  ulong res_length;
  DBUG_ENTER("udf_handler::val_str");

  if (get_arguments())
    DBUG_RETURN(0);
  char * (*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  if ((res_length=str->alloced_length()) < MAX_FIELD_WIDTH)
  {						// This happens VERY seldom
    if (str->alloc(MAX_FIELD_WIDTH))
    {
      error=1;
      DBUG_RETURN(0);
    }
  }
  char *res=func(&initid, &f_args, (char*) str->ptr(), &res_length,
		 &is_null_tmp, &error);
  DBUG_PRINT("info", ("udf func returned, res_length: %lu", res_length));
  if (is_null_tmp || !res || unlikely(error))	// The !res is for safety
  {
    DBUG_PRINT("info", ("Null or error"));
    DBUG_RETURN(0);
  }
  if (res == str->ptr())
  {
    str->length(res_length);
    DBUG_PRINT("exit", ("str: %*.s", (int) str->length(), str->ptr()));
    DBUG_RETURN(str);
  }
  save_str->set(res, res_length, str->charset());
  DBUG_PRINT("exit", ("save_str: %s", save_str->ptr()));
  DBUG_RETURN(save_str);
}


/*
  For the moment, UDF functions are returning DECIMAL values as strings
*/

my_decimal *udf_handler::val_decimal(my_bool *null_value, my_decimal *dec_buf)
{
  char buf[DECIMAL_MAX_STR_LENGTH+1], *end;
  ulong res_length= DECIMAL_MAX_STR_LENGTH;

  if (get_arguments())
  {
    *null_value=1;
    return 0;
  }
  char *(*func)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *)=
    (char* (*)(UDF_INIT *, UDF_ARGS *, char *, ulong *, uchar *, uchar *))
    u_d->func;

  char *res= func(&initid, &f_args, buf, &res_length, &is_null, &error);
  if (is_null || unlikely(error))
  {
    *null_value= 1;
    return 0;
  }
  end= res+ res_length;
  str2my_decimal(E_DEC_FATAL_ERROR, res, dec_buf, &end);
  return dec_buf;
}


void Item_udf_func::cleanup()
{
  udf.cleanup();
  Item_func::cleanup();
}


void Item_udf_func::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  for (uint i=0 ; i < arg_count ; i++)
  {
    if (i != 0)
      str->append(',');
    args[i]->print_item_w_name(str, query_type);
  }
  str->append(')');
}


double Item_func_udf_float::val_real()
{
  double res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_func_udf_float::val");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
		     args[0]->result_type(), arg_count));
  res= udf.val(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_func_udf_float::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  double nr= val_real();
  if (null_value)
    return 0;					/* purecov: inspected */
  str->set_real(nr,decimals,&my_charset_bin);
  return str;
}


longlong Item_func_udf_int::val_int()
{
  longlong res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_func_udf_int::val_int");
  res= udf.val_int(&tmp_null_value);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


String *Item_func_udf_int::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  longlong nr=val_int();
  if (null_value)
    return 0;
  str->set_int(nr, unsigned_flag, &my_charset_bin);
  return str;
}


my_decimal *Item_func_udf_decimal::val_decimal(my_decimal *dec_buf)
{
  my_decimal *res;
  my_bool tmp_null_value;
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_func_udf_decimal::val_decimal");
  DBUG_PRINT("info",("result_type: %d  arg_count: %d",
                     args[0]->result_type(), arg_count));

  res= udf.val_decimal(&tmp_null_value, dec_buf);
  null_value= tmp_null_value;
  DBUG_RETURN(res);
}


/* Default max_length is max argument length */

bool Item_func_udf_str::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_udf_str::fix_length_and_dec");
  max_length=0;
  for (uint i = 0; i < arg_count; i++)
    set_if_bigger(max_length,args[i]->max_length);
  DBUG_RETURN(FALSE);
}

String *Item_func_udf_str::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  String *res=udf.val_str(str,&str_value);
  null_value = !res;
  return res;
}


/**
  @note
  This has to come last in the udf_handler methods, or C for AIX
  version 6.0.0.0 fails to compile with debugging enabled. (Yes, really.)
*/

udf_handler::~udf_handler()
{
  /* Everything should be properly cleaned up by this moment. */
  DBUG_ASSERT(not_original || !(initialized || buffers));
}

#else
bool udf_handler::get_arguments() { return 0; }
#endif /* HAVE_DLOPEN */


longlong Item_master_pos_wait::val_int()
{
  DBUG_ASSERT(fixed());
  THD* thd = current_thd;
  String *log_name = args[0]->val_str(&value);
  int event_count= 0;
  DBUG_ENTER("Item_master_pos_wait::val_int");

  null_value=0;
  if (thd->slave_thread || !log_name || !log_name->length())
  {
    null_value = 1;
    DBUG_RETURN(0);
  }
#ifdef HAVE_REPLICATION
  longlong pos = (ulong)args[1]->val_int();
  longlong timeout = (arg_count>=3) ? args[2]->val_int() : 0 ;
  String connection_name_buff;
  LEX_CSTRING connection_name;
  Master_info *mi= NULL;
  if (arg_count >= 4)
  {
    String *con;
    if (!(con= args[3]->val_str(&connection_name_buff)))
      goto err;

    connection_name.str= con->ptr();
    connection_name.length= con->length();
    if (check_master_connection_name(&connection_name))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(ME_WARNING),
               "MASTER_CONNECTION_NAME");
      goto err;
    }
  }
  else
    connection_name= thd->variables.default_master_connection;

  if (!(mi= get_master_info(&connection_name, Sql_condition::WARN_LEVEL_WARN)))
    goto err;

  if ((event_count = mi->rli.wait_for_pos(thd, log_name, pos, timeout)) == -2)
  {
    null_value = 1;
    event_count=0;
  }
  mi->release();
#endif
  DBUG_PRINT("exit", ("event_count: %d  null_value: %d", event_count,
                      (int) null_value));
  DBUG_RETURN(event_count);

#ifdef HAVE_REPLICATION
err:
  {
    null_value = 1;
    DBUG_RETURN(0);
  }
#endif
}


longlong Item_master_gtid_wait::val_int()
{
  DBUG_ASSERT(fixed());
  longlong result= 0;
  String *gtid_pos __attribute__((unused)) = args[0]->val_str(&value);
  DBUG_ENTER("Item_master_gtid_wait::val_int");

  if (args[0]->null_value)
  {
    null_value= 1;
    DBUG_RETURN(0);
  }

  null_value=0;
#ifdef HAVE_REPLICATION
  THD* thd= current_thd;
  longlong timeout_us;

  if (arg_count==2 && !args[1]->null_value)
    timeout_us= (longlong)(1e6*args[1]->val_real());
  else
    timeout_us= (longlong)-1;

  result= rpl_global_gtid_waiting.wait_for_pos(thd, gtid_pos, timeout_us);
#else
  null_value= 0;
#endif /* REPLICATION */
  DBUG_RETURN(result);
}


/**
  Enables a session to wait on a condition until a timeout or a network
  disconnect occurs.

  @remark The connection is polled every m_interrupt_interval nanoseconds.
*/

class Interruptible_wait
{
  THD *m_thd;
  struct timespec m_abs_timeout;
  static const ulonglong m_interrupt_interval;

  public:
    Interruptible_wait(THD *thd)
    : m_thd(thd) {}

    ~Interruptible_wait() {}

  public:
    /**
      Set the absolute timeout.

      @param timeout The amount of time in nanoseconds to wait
    */
    void set_timeout(ulonglong timeout)
    {
      /*
        Calculate the absolute system time at the start so it can
        be controlled in slices. It relies on the fact that once
        the absolute time passes, the timed wait call will fail
        automatically with a timeout error.
      */
      set_timespec_nsec(m_abs_timeout, timeout);
    }

    /** The timed wait. */
    int wait(mysql_cond_t *, mysql_mutex_t *);
};


/** Time to wait before polling the connection status. */
const ulonglong Interruptible_wait::m_interrupt_interval= 5 * 1000000000ULL;


/**
  Wait for a given condition to be signaled.

  @param cond   The condition variable to wait on.
  @param mutex  The associated mutex.

  @remark The absolute timeout is preserved across calls.

  @retval return value from mysql_cond_timedwait
*/

int Interruptible_wait::wait(mysql_cond_t *cond, mysql_mutex_t *mutex)
{
  int error;
  struct timespec timeout;

  while (1)
  {
    /* Wait for a fixed interval. */
    set_timespec_nsec(timeout, m_interrupt_interval);

    /* But only if not past the absolute timeout. */
    if (cmp_timespec(timeout, m_abs_timeout) > 0)
      timeout= m_abs_timeout;

    error= mysql_cond_timedwait(cond, mutex, &timeout);
    if (m_thd->check_killed())
      break;
    if (error == ETIMEDOUT || error == ETIME)
    {
      /* Return error if timed out or connection is broken. */
      if (!cmp_timespec(timeout, m_abs_timeout) || !m_thd->is_connected())
        break;
    }
    /* Otherwise, propagate status to the caller. */
    else
      break;
  }

  return error;
}


/**
  For locks with EXPLICIT duration, MDL returns a new ticket
  every time a lock is granted. This allows to implement recursive
  locks without extra allocation or additional data structures, such
  as below. However, if there are too many tickets in the same
  MDL_context, MDL_context::find_ticket() is getting too slow,
  since it's using a linear search.
  This is why a separate structure is allocated for a user
  level lock, and before requesting a new lock from MDL,
  GET_LOCK() checks thd->ull_hash if such lock is already granted,
  and if so, simply increments a reference counter.
*/

class User_level_lock
{
public:
  MDL_ticket *lock;
  int refs;
};


/** Extract a hash key from User_level_lock. */

uchar *ull_get_key(const uchar *ptr, size_t *length,
                   my_bool not_used __attribute__((unused)))
{
  User_level_lock *ull = (User_level_lock*) ptr;
  MDL_key *key = ull->lock->get_key();
  *length= key->length();
  return (uchar*) key->ptr();
}


/**
  Release all user level locks for this THD.
*/

void mysql_ull_cleanup(THD *thd)
{
  User_level_lock *ull;
  DBUG_ENTER("mysql_ull_cleanup");

  for (uint i= 0; i < thd->ull_hash.records; i++)
  {
    ull = (User_level_lock*) my_hash_element(&thd->ull_hash, i);
    thd->mdl_context.release_lock(ull->lock);
    my_free(ull);
  }

  my_hash_free(&thd->ull_hash);

  DBUG_VOID_RETURN;
}


/**
  Set explicit duration for metadata locks corresponding to
  user level locks to protect them from being released at the end
  of transaction.
*/

void mysql_ull_set_explicit_lock_duration(THD *thd)
{
  User_level_lock *ull;
  DBUG_ENTER("mysql_ull_set_explicit_lock_duration");

  for (uint i= 0; i < thd->ull_hash.records; i++)
  {
    ull= (User_level_lock*) my_hash_element(&thd->ull_hash, i);
    thd->mdl_context.set_lock_duration(ull->lock, MDL_EXPLICIT);
  }
  DBUG_VOID_RETURN;
}


/**
  When MDL detects a lock wait timeout, it pushes
  an error into the statement diagnostics area.
  For GET_LOCK(), lock wait timeout is not an error,
  but a special return value (0).
  Similarly, killing get_lock wait is not an error either,
  but a return value NULL.
  Capture and suppress lock wait timeouts and kills.
*/

class Lock_wait_timeout_handler: public Internal_error_handler
{
public:
  Lock_wait_timeout_handler() :m_lock_wait_timeout(false) {}

  bool m_lock_wait_timeout;

  bool handle_condition(THD * /* thd */, uint sql_errno,
                        const char * /* sqlstate */,
                        Sql_condition::enum_warning_level* /* level */,
                        const char *message,
                        Sql_condition ** /* cond_hdl */);
};

bool
Lock_wait_timeout_handler::
handle_condition(THD *thd, uint sql_errno,
                 const char * /* sqlstate */,
                 Sql_condition::enum_warning_level* /* level */,
                 const char *message,
                 Sql_condition ** /* cond_hdl */)
{
  if (sql_errno == ER_LOCK_WAIT_TIMEOUT)
  {
    m_lock_wait_timeout= true;
    return true;                                /* condition handled */
  }
  if (thd->is_killed())
    return true;

  return false;
}


static int ull_name_ok(String *name)
{
  if (!name || !name->length())
    return 0;

  if (name->length() > NAME_LEN)
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name->c_ptr_safe());
    return 0;
  }
  return 1;
}


/**
  Get a user level lock.

  @retval
    1    : Got lock
  @retval
    0    : Timeout
  @retval
    NULL : Error
*/

longlong Item_func_get_lock::val_int()
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&value);
  double timeout= args[1]->val_real();
  THD *thd= current_thd;
  User_level_lock *ull;
  DBUG_ENTER("Item_func_get_lock::val_int");

  null_value= 1;
  /*
    In slave thread no need to get locks, everything is serialized. Anyway
    there is no way to make GET_LOCK() work on slave like it did on master
    (i.e. make it return exactly the same value) because we don't have the
    same other concurrent threads environment. No matter what we return here,
    it's not guaranteed to be same as on master.
  */
  if (thd->slave_thread)
  {
    null_value= 0;
    DBUG_RETURN(1);
  }

  if (args[1]->null_value ||
      (!args[1]->unsigned_flag && ((longlong) timeout < 0)))
  {
    char buf[22];
    if (args[1]->null_value)
      strmov(buf, "NULL");
    else
      llstr(((longlong) timeout), buf);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_VALUE_FOR_TYPE, ER(ER_WRONG_VALUE_FOR_TYPE),
                        "timeout", buf, "get_lock");
    null_value= 1;
    DBUG_RETURN(0);
  }

  if (!ull_name_ok(res))
    DBUG_RETURN(0);
  DBUG_PRINT("enter", ("lock: %.*s", res->length(), res->ptr()));
  /* HASH entries are of type User_level_lock. */
  if (! my_hash_inited(&thd->ull_hash) &&
        my_hash_init(key_memory_User_level_lock, &thd->ull_hash,
                     &my_charset_bin, 16 /* small hash */, 0, 0, ull_get_key,
                     NULL, 0))
  {
    DBUG_RETURN(0);
  }

  MDL_request ull_request;
  MDL_REQUEST_INIT(&ull_request, MDL_key::USER_LOCK, res->c_ptr_safe(), "",
                   MDL_SHARED_NO_WRITE, MDL_EXPLICIT);
  MDL_key *ull_key= &ull_request.key;


  if ((ull= (User_level_lock*)
       my_hash_search(&thd->ull_hash, ull_key->ptr(), ull_key->length())))
  {
    /* Recursive lock */
    ull->refs++;
    null_value= 0;
    DBUG_PRINT("info", ("recursive lock, ref-count: %d", (int) ull->refs));
    DBUG_RETURN(1);
  }

  Lock_wait_timeout_handler lock_wait_timeout_handler;
  thd->push_internal_handler(&lock_wait_timeout_handler);
  bool error= thd->mdl_context.acquire_lock(&ull_request, timeout);
  (void) thd->pop_internal_handler();
  if (unlikely(error))
  {
    if (lock_wait_timeout_handler.m_lock_wait_timeout)
      null_value= 0;
    DBUG_RETURN(0);
  }

  ull= (User_level_lock*) my_malloc(key_memory_User_level_lock,
                                    sizeof(User_level_lock),
                                    MYF(MY_WME|MY_THREAD_SPECIFIC));
  if (ull == NULL)
  {
    thd->mdl_context.release_lock(ull_request.ticket);
    DBUG_RETURN(0);
  }

  ull->lock= ull_request.ticket;
  ull->refs= 1;

  if (my_hash_insert(&thd->ull_hash, (uchar*) ull))
  {
    thd->mdl_context.release_lock(ull->lock);
    my_free(ull);
    DBUG_RETURN(0);
  }
  null_value= 0;

  DBUG_RETURN(1);
}


/**
  Release all user level locks.
  @return
    - N if N-lock released
    - 0 if lock wasn't held
*/
longlong Item_func_release_all_locks::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  ulong num_unlocked= 0;
  DBUG_ENTER("Item_func_release_all_locks::val_int");
  for (size_t i= 0; i < thd->ull_hash.records; i++)
  {
    auto ull= (User_level_lock *) my_hash_element(&thd->ull_hash, i);
    thd->mdl_context.release_lock(ull->lock);
    num_unlocked+= ull->refs;
    my_free(ull);
  }
  my_hash_free(&thd->ull_hash);
  DBUG_RETURN(num_unlocked);
}


/**
  Release a user level lock.
  @return
    - 1 if lock released
    - 0 if lock wasn't held
    - (SQL) NULL if no such lock
*/

longlong Item_func_release_lock::val_int()
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&value);
  THD *thd= current_thd;
  DBUG_ENTER("Item_func_release_lock::val_int");
  null_value= 1;

  if (!ull_name_ok(res))
    DBUG_RETURN(0);

  DBUG_PRINT("enter", ("lock: %.*s", res->length(), res->ptr()));

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LOCK, res->c_ptr_safe(), "");

  User_level_lock *ull;

  if (!my_hash_inited(&thd->ull_hash) ||
      !(ull=
        (User_level_lock*) my_hash_search(&thd->ull_hash,
                                          ull_key.ptr(), ull_key.length())))
  {
    null_value= thd->mdl_context.get_lock_owner(&ull_key) == 0;
    DBUG_RETURN(0);
  }
  DBUG_PRINT("info", ("ref count: %d", (int) ull->refs));
  null_value= 0;
  if (--ull->refs == 0)
  {
    my_hash_delete(&thd->ull_hash, (uchar*) ull);
    thd->mdl_context.release_lock(ull->lock);
    my_free(ull);
  }
  DBUG_RETURN(1);
}


/**
  Check a user level lock.

  Sets null_value=TRUE on error.

  @retval
    1		Available
  @retval
    0		Already taken, or error
*/

longlong Item_func_is_free_lock::val_int()
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&value);
  THD *thd= current_thd;
  null_value= 1;

  if (!ull_name_ok(res))
    return 0;

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LOCK, res->c_ptr_safe(), "");

  null_value= 0;
  return thd->mdl_context.get_lock_owner(&ull_key) == 0;
}


longlong Item_func_is_used_lock::val_int()
{
  DBUG_ASSERT(fixed());
  String *res= args[0]->val_str(&value);
  THD *thd= current_thd;
  null_value= 1;

  if (!ull_name_ok(res))
    return 0;

  MDL_key ull_key;
  ull_key.mdl_key_init(MDL_key::USER_LOCK, res->c_ptr_safe(), "");
  ulong thread_id = thd->mdl_context.get_lock_owner(&ull_key);
  if (thread_id == 0)
    return 0;

  null_value= 0;
  return thread_id;
}


longlong Item_func_last_insert_id::val_int()
{
  THD *thd= current_thd;
  DBUG_ASSERT(fixed());
  if (arg_count)
  {
    longlong value= args[0]->val_int();
    null_value= args[0]->null_value;
    /*
      LAST_INSERT_ID(X) must affect the client's mysql_insert_id() as
      documented in the manual. We don't want to touch
      first_successful_insert_id_in_cur_stmt because it would make
      LAST_INSERT_ID(X) take precedence over an generated auto_increment
      value for this row.
    */
    thd->arg_of_last_insert_id_function= TRUE;
    thd->first_successful_insert_id_in_prev_stmt= value;
    return value;
  }
  return
    static_cast<longlong>(thd->read_first_successful_insert_id_in_prev_stmt());
}


bool Item_func_last_insert_id::fix_fields(THD *thd, Item **ref)
{
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
  return Item_int_func::fix_fields(thd, ref);
}


/* This function is just used to test speed of different functions */

longlong Item_func_benchmark::val_int()
{
  DBUG_ASSERT(fixed());
  char buff[MAX_FIELD_WIDTH];
  String tmp(buff,sizeof(buff), &my_charset_bin);
  my_decimal tmp_decimal;
  THD *thd= current_thd;
  ulonglong loop_count;

  loop_count= (ulonglong) args[0]->val_int();

  if (args[0]->null_value ||
      (!args[0]->unsigned_flag && (((longlong) loop_count) < 0)))
  {
    if (!args[0]->null_value)
    {
      char buff[22];
      llstr(((longlong) loop_count), buff);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WRONG_VALUE_FOR_TYPE,
                          ER_THD(thd, ER_WRONG_VALUE_FOR_TYPE),
                          "count", buff, "benchmark");
    }

    null_value= 1;
    return 0;
  }

  null_value=0;
  for (ulonglong loop=0 ; loop < loop_count && !thd->killed; loop++)
  {
    switch (args[1]->result_type()) {
    case REAL_RESULT:
      (void) args[1]->val_real();
      break;
    case INT_RESULT:
      (void) args[1]->val_int();
      break;
    case STRING_RESULT:
      (void) args[1]->val_str(&tmp);
      break;
    case DECIMAL_RESULT:
      (void) args[1]->val_decimal(&tmp_decimal);
      break;
    case ROW_RESULT:
    case TIME_RESULT:
      DBUG_ASSERT(0);              // This case should never be chosen
      return 0;
    }
  }
  return 0;
}


void Item_func_benchmark::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("benchmark("));
  args[0]->print(str, query_type);
  str->append(',');
  args[1]->print(str, query_type);
  str->append(')');
}


mysql_mutex_t LOCK_item_func_sleep;

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_item_func_sleep;

static PSI_mutex_info item_func_sleep_mutexes[]=
{
  { &key_LOCK_item_func_sleep, "LOCK_item_func_sleep", PSI_FLAG_GLOBAL}
};


static void init_item_func_sleep_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(item_func_sleep_mutexes);
  PSI_server->register_mutex(category, item_func_sleep_mutexes, count);
}
#endif

static bool item_func_sleep_inited= 0;


void item_func_sleep_init(void)
{
#ifdef HAVE_PSI_INTERFACE
  init_item_func_sleep_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_item_func_sleep, &LOCK_item_func_sleep, MY_MUTEX_INIT_SLOW);
  item_func_sleep_inited= 1;
}


void item_func_sleep_free(void)
{
  if (item_func_sleep_inited)
  {
    item_func_sleep_inited= 0;
    mysql_mutex_destroy(&LOCK_item_func_sleep);
  }
}


/** This function is just used to create tests with time gaps. */

longlong Item_func_sleep::val_int()
{
  THD *thd= current_thd;
  Interruptible_wait timed_cond(thd);
  mysql_cond_t cond;
  double timeout;
  int error;

  DBUG_ASSERT(fixed());

  timeout= args[0]->val_real();
  /*
    On 64-bit OSX mysql_cond_timedwait() waits forever
    if passed abstime time has already been exceeded by 
    the system time.
    When given a very short timeout (< 10 mcs) just return 
    immediately.
    We assume that the lines between this test and the call 
    to mysql_cond_timedwait() will be executed in less than 0.00001 sec.
  */
  if (timeout < 0.00001)
    return 0;

  timed_cond.set_timeout((ulonglong) (timeout * 1000000000.0));

  mysql_cond_init(key_item_func_sleep_cond, &cond, NULL);
  mysql_mutex_lock(&LOCK_item_func_sleep);

  THD_STAGE_INFO(thd, stage_user_sleep);
  thd->mysys_var->current_mutex= &LOCK_item_func_sleep;
  thd->mysys_var->current_cond=  &cond;

  error= 0;
  thd_wait_begin(thd, THD_WAIT_SLEEP);
  while (!thd->killed)
  {
    error= timed_cond.wait(&cond, &LOCK_item_func_sleep);
    if (error == ETIMEDOUT || error == ETIME)
      break;
    error= 0;
  }
  thd_wait_end(thd);
  mysql_mutex_unlock(&LOCK_item_func_sleep);
  mysql_mutex_lock(&thd->mysys_var->mutex);
  thd->mysys_var->current_mutex= 0;
  thd->mysys_var->current_cond=  0;
  mysql_mutex_unlock(&thd->mysys_var->mutex);

  mysql_cond_destroy(&cond);

  DBUG_EXECUTE_IF("sleep_inject_query_done_debug_sync", {
      debug_sync_set_action
        (thd, STRING_WITH_LEN("dispatch_command_end SIGNAL query_done"));
    };);

  return MY_TEST(!error);                  // Return 1 killed
}


bool Item_func_user_var::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function("@", name.str, arg, VCOL_NON_DETERMINISTIC);
}

#define extra_size sizeof(double)

user_var_entry *get_variable(HASH *hash, LEX_CSTRING *name,
                             bool create_if_not_exists)
{
  user_var_entry *entry;

  if (!(entry = (user_var_entry*) my_hash_search(hash, (uchar*) name->str,
                                                 name->length)) &&
      create_if_not_exists)
  {
    size_t size=ALIGN_SIZE(sizeof(user_var_entry))+name->length+1+extra_size;
    if (!my_hash_inited(hash))
      return 0;
    if (!(entry = (user_var_entry*) my_malloc(key_memory_user_var_entry, size,
                                              MYF(MY_WME | ME_FATAL |
                                                  MY_THREAD_SPECIFIC))))
      return 0;
    entry->name.str=(char*) entry+ ALIGN_SIZE(sizeof(user_var_entry))+
      extra_size;
    entry->name.length=name->length;
    entry->value=0;
    entry->length=0;
    entry->update_query_id=0;
    entry->set_charset(NULL);
    entry->unsigned_flag= 0;
    /*
      If we are here, we were called from a SET or a query which sets a
      variable. Imagine it is this:
      INSERT INTO t SELECT @a:=10, @a:=@a+1.
      Then when we have a Item_func_get_user_var (because of the @a+1) so we
      think we have to write the value of @a to the binlog. But before that,
      we have a Item_func_set_user_var to create @a (@a:=10), in this we mark
      the variable as "already logged" (line below) so that it won't be logged
      by Item_func_get_user_var (because that's not necessary).
    */
    entry->used_query_id=current_thd->query_id;
    entry->type=STRING_RESULT;
    memcpy((char*) entry->name.str, name->str, name->length+1);
    if (my_hash_insert(hash,(uchar*) entry))
    {
      my_free(entry);
      return 0;
    }
  }
  return entry;
}


void Item_func_set_user_var::cleanup()
{
  Item_func::cleanup();
  m_var_entry= NULL;
}


bool Item_func_set_user_var::set_entry(THD *thd, bool create_if_not_exists)
{
  if (m_var_entry && thd->thread_id == entry_thread_id)
    goto end; // update entry->update_query_id for PS
  if (!(m_var_entry= get_variable(&thd->user_vars, &name, create_if_not_exists)))
  {
    entry_thread_id= 0;
    return TRUE;
  }
  entry_thread_id= thd->thread_id;
  /* 
     Remember the last query which updated it, this way a query can later know
     if this variable is a constant item in the query (it is if update_query_id
     is different from query_id).
  */
end:
  m_var_entry->update_query_id= thd->query_id;
  return FALSE;
}


/*
  When a user variable is updated (in a SET command or a query like
  SELECT @a:= ).
*/

bool Item_func_set_user_var::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  /* fix_fields will call Item_func_set_user_var::fix_length_and_dec */
  if (Item_func::fix_fields(thd, ref) || set_entry(thd, TRUE))
    return TRUE;
  /*
    As it is wrong and confusing to associate any 
    character set with NULL, @a should be latin2
    after this query sequence:

      SET @a=_latin2'string';
      SET @a=NULL;

    I.e. the second query should not change the charset
    to the current default value, but should keep the 
    original value assigned during the first query.
    In order to do it, we don't copy charset
    from the argument if the argument is NULL
    and the variable has previously been initialized.
  */
  null_item= (args[0]->type() == NULL_ITEM);
  if (!m_var_entry->charset() || !null_item)
    m_var_entry->set_charset(args[0]->collation.derivation == DERIVATION_NUMERIC ?
                             &my_charset_numeric : args[0]->collation.collation);
  collation.set(m_var_entry->charset(),
                args[0]->collation.derivation == DERIVATION_NUMERIC ?
                DERIVATION_NUMERIC : DERIVATION_IMPLICIT);
  switch (args[0]->result_type()) {
  case STRING_RESULT:
  case TIME_RESULT:
    set_handler(type_handler_long_blob.
                type_handler_adjusted_to_max_octet_length(max_length,
                                                          collation.collation));
    break;
  case REAL_RESULT:
    set_handler(&type_handler_double);
    break;
  case INT_RESULT:
    set_handler(Type_handler::type_handler_long_or_longlong(max_char_length(),
                                                            unsigned_flag));
    break;
  case DECIMAL_RESULT:
    set_handler(&type_handler_newdecimal);
    break;
  case ROW_RESULT:
    DBUG_ASSERT(0);
    set_handler(&type_handler_row);
    break;
  }
  if (thd->lex->current_select)
  {
    /*
      When this function is used in a derived table/view force the derived
      table to be materialized to preserve possible side-effect of setting a
      user variable.
    */
    SELECT_LEX_UNIT *unit= thd->lex->current_select->master_unit();
    TABLE_LIST *derived;
    for (derived= unit->derived;
         derived;
         derived= unit->derived)
    {
      derived->set_materialized_derived();
      derived->prohibit_cond_pushdown= true;
      if (unit->with_element && unit->with_element->is_recursive)
        break;
      unit= derived->select_lex->master_unit();
    }
  }

  return FALSE;
}


bool
Item_func_set_user_var::fix_length_and_dec(THD *thd)
{
  base_flags|= (args[0]->base_flags & item_base_t::MAYBE_NULL);
  decimals=args[0]->decimals;
  if (args[0]->collation.derivation == DERIVATION_NUMERIC)
  {
    collation.set(DERIVATION_NUMERIC);
    fix_length_and_charset(args[0]->max_char_length(), &my_charset_numeric);
  }
  else
  {
    collation.set(DERIVATION_IMPLICIT);
    fix_length_and_charset(args[0]->max_char_length(),
                           args[0]->collation.collation);
  }
  unsigned_flag= args[0]->unsigned_flag;
  return FALSE;
}


/*
  Mark field in read_map

  NOTES
    This is used by filesort to register used fields in a a temporary
    column read set or to register used fields in a view
*/

bool Item_func_set_user_var::register_field_in_read_map(void *arg)
{
  if (result_field)
  {
    TABLE *table= (TABLE *) arg;
    if (result_field->table == table || !table)
      bitmap_set_bit(result_field->table->read_set, result_field->field_index);
    if (result_field->vcol_info)
      return result_field->vcol_info->
               expr->walk(&Item::register_field_in_read_map, 1, arg);
  }
  return 0;
}

/*
  Mark field in bitmap supplied as *arg

*/

bool Item_func_set_user_var::register_field_in_bitmap(void *arg)
{
  MY_BITMAP *bitmap = (MY_BITMAP *) arg;
  DBUG_ASSERT(bitmap);
  if (result_field)
  {
    if (!bitmap)
      return 1;
    bitmap_set_bit(bitmap, result_field->field_index);
  }
  return 0;
}

/**
  Set value to user variable.

  @param entry          pointer to structure representing variable
  @param set_null       should we set NULL value ?
  @param ptr            pointer to buffer with new value
  @param length         length of new value
  @param type           type of new value
  @param cs             charset info for new value
  @param dv             derivation for new value
  @param unsigned_arg   indicates if a value of type INT_RESULT is unsigned

  @note Sets error and fatal error if allocation fails.

  @retval
    false   success
  @retval
    true    failure
*/

bool
update_hash(user_var_entry *entry, bool set_null, void *ptr, size_t length,
            Item_result type, CHARSET_INFO *cs,
            bool unsigned_arg)
{
  if (set_null)
  {
    char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
    if (entry->value && entry->value != pos)
      my_free(entry->value);
    entry->value= 0;
    entry->length= 0;
  }
  else
  {
    if (type == STRING_RESULT)
      length++;					// Store strings with end \0
    if (length <= extra_size)
    {
      /* Save value in value struct */
      char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
      if (entry->value != pos)
      {
	if (entry->value)
	  my_free(entry->value);
	entry->value=pos;
      }
    }
    else
    {
      /* Allocate variable */
      if (entry->length != length)
      {
	char *pos= (char*) entry+ ALIGN_SIZE(sizeof(user_var_entry));
	if (entry->value == pos)
	  entry->value=0;
        entry->value= (char*) my_realloc(key_memory_user_var_entry_value,
                                         entry->value, length,
                                         MYF(MY_ALLOW_ZERO_PTR | MY_WME |
                                             ME_FATAL | MY_THREAD_SPECIFIC));
        if (!entry->value)
	  return 1;
      }
    }
    if (type == STRING_RESULT)
    {
      length--;					// Fix length change above
      entry->value[length]= 0;			// Store end \0
    }
    if (length)
      memmove(entry->value, ptr, length);
    if (type == DECIMAL_RESULT)
      ((my_decimal*)entry->value)->fix_buffer_pointer();
    entry->length= length;
    entry->set_charset(cs);
    entry->unsigned_flag= unsigned_arg;
  }
  entry->type=type;
#ifdef USER_VAR_TRACKING
#ifndef EMBEDDED_LIBRARY
  THD *thd= current_thd;
  thd->session_tracker.user_variables.mark_as_changed(thd, entry);
#endif
#endif // USER_VAR_TRACKING
  return 0;
}


bool
Item_func_set_user_var::update_hash(void *ptr, size_t length,
                                    Item_result res_type,
                                    CHARSET_INFO *cs,
                                    bool unsigned_arg)
{
  /*
    If we set a variable explicitly to NULL then keep the old
    result type of the variable
  */
  if (args[0]->type() == Item::FIELD_ITEM)
  {
    /* args[0]->null_value may be outdated */
    null_value= ((Item_field*)args[0])->field->is_null();
  }
  else
    null_value= args[0]->null_value;
  if (null_value && null_item)
    res_type= m_var_entry->type;                 // Don't change type of item
  if (::update_hash(m_var_entry, null_value,
                    ptr, length, res_type, cs, unsigned_arg))
  {
    null_value= 1;
    return 1;
  }
  return 0;
}


/** Get the value of a variable as a double. */

double user_var_entry::val_real(bool *null_value)
{
  if ((*null_value= (value == 0)))
    return 0.0;

  switch (type) {
  case REAL_RESULT:
    return *(double*) value;
  case INT_RESULT:
    return (double) *(longlong*) value;
  case DECIMAL_RESULT:
    return ((my_decimal *)value)->to_double();
  case STRING_RESULT:
    return my_atof(value);                      // This is null terminated
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return 0.0;					// Impossible
}


/** Get the value of a variable as an integer. */

longlong user_var_entry::val_int(bool *null_value) const
{
  if ((*null_value= (value == 0)))
    return 0;

  switch (type) {
  case REAL_RESULT:
    return (longlong) *(double*) value;
  case INT_RESULT:
    return *(longlong*) value;
  case DECIMAL_RESULT:
    return ((my_decimal *)value)->to_longlong(false);
  case STRING_RESULT:
  {
    int error;
    return my_strtoll10(value, (char**) 0, &error);// String is null terminated
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return 0;					// Impossible
}


/** Get the value of a variable as a string. */

String *user_var_entry::val_str(bool *null_value, String *str,
                                uint decimals) const
{
  if ((*null_value= (value == 0)))
    return (String*) 0;

  switch (type) {
  case REAL_RESULT:
    str->set_real(*(double*) value, decimals, charset());
    break;
  case INT_RESULT:
    if (!unsigned_flag)
      str->set(*(longlong*) value, charset());
    else
      str->set(*(ulonglong*) value, charset());
    break;
  case DECIMAL_RESULT:
    str_set_decimal((my_decimal *) value, str, charset());
    break;
  case STRING_RESULT:
    if (str->copy(value, length, charset()))
      str= 0;					// EOM error
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return(str);
}

/** Get the value of a variable as a decimal. */

my_decimal *user_var_entry::val_decimal(bool *null_value, my_decimal *val)
{
  if ((*null_value= (value == 0)))
    return 0;

  switch (type) {
  case REAL_RESULT:
    double2my_decimal(E_DEC_FATAL_ERROR, *(double*) value, val);
    break;
  case INT_RESULT:
    int2my_decimal(E_DEC_FATAL_ERROR, *(longlong*) value, 0, val);
    break;
  case DECIMAL_RESULT:
    my_decimal2decimal((my_decimal *) value, val);
    break;
  case STRING_RESULT:
    str2my_decimal(E_DEC_FATAL_ERROR, value, length, charset(), val);
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);				// Impossible
    break;
  }
  return(val);
}

/**
  This functions is invoked on SET \@variable or
  \@variable:= expression.

  Evaluate (and check expression), store results.

  @note
    For now it always return OK. All problem with value evaluating
    will be caught by thd->is_error() check in sql_set_variables().

  @retval
    FALSE OK.
*/

bool
Item_func_set_user_var::check(bool use_result_field)
{
  DBUG_ENTER("Item_func_set_user_var::check");
  if (use_result_field && !result_field)
    use_result_field= FALSE;

  switch (result_type()) {
  case REAL_RESULT:
  {
    save_result.vreal= use_result_field ? result_field->val_real() :
                        args[0]->val_real();
    break;
  }
  case INT_RESULT:
  {
    save_result.vint= use_result_field ? result_field->val_int() :
                       args[0]->val_int();
    unsigned_flag= (use_result_field ?
                    ((Field_num*)result_field)->unsigned_flag:
                    args[0]->unsigned_flag);
    break;
  }
  case STRING_RESULT:
  {
    save_result.vstr= use_result_field ? result_field->val_str(&value) :
                       args[0]->val_str(&value);
    break;
  }
  case DECIMAL_RESULT:
  {
    save_result.vdec= use_result_field ?
                       result_field->val_decimal(&decimal_buff) :
                       args[0]->val_decimal(&decimal_buff);
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    break;
  }
  DBUG_RETURN(FALSE);
}


/**
  @brief Evaluate and store item's result.
  This function is invoked on "SELECT ... INTO @var ...".
  
  @param    item    An item to get value from.
*/

void Item_func_set_user_var::save_item_result(Item *item)
{
  DBUG_ENTER("Item_func_set_user_var::save_item_result");

  switch (args[0]->result_type()) {
  case REAL_RESULT:
    save_result.vreal= item->val_result();
    break;
  case INT_RESULT:
    save_result.vint= item->val_int_result();
    unsigned_flag= item->unsigned_flag;
    break;
  case STRING_RESULT:
    save_result.vstr= item->str_result(&value);
    break;
  case DECIMAL_RESULT:
    save_result.vdec= item->val_decimal_result(&decimal_buff);
    break;
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    break;
  }
  DBUG_VOID_RETURN;
}


/**
  This functions is invoked on
  SET \@variable or \@variable:= expression.

  @note
    We have to store the expression as such in the variable, independent of
    the value method used by the user

  @retval
    0	OK
  @retval
    1	EOM Error

*/

bool
Item_func_set_user_var::update()
{
  bool res= 0;
  DBUG_ENTER("Item_func_set_user_var::update");

  switch (result_type()) {
  case REAL_RESULT:
  {
    res= update_hash((void*) &save_result.vreal,sizeof(save_result.vreal),
		     REAL_RESULT, &my_charset_numeric, 0);
    break;
  }
  case INT_RESULT:
  {
    res= update_hash((void*) &save_result.vint, sizeof(save_result.vint),
                     INT_RESULT, &my_charset_numeric, unsigned_flag);
    break;
  }
  case STRING_RESULT:
  {
    if (!save_result.vstr)					// Null value
      res= update_hash((void*) 0, 0, STRING_RESULT, &my_charset_bin, 0);
    else
      res= update_hash((void*) save_result.vstr->ptr(),
		       save_result.vstr->length(), STRING_RESULT,
		       save_result.vstr->charset(), 0);
    break;
  }
  case DECIMAL_RESULT:
  {
    if (!save_result.vdec)					// Null value
      res= update_hash((void*) 0, 0, DECIMAL_RESULT, &my_charset_bin, 0);
    else
      res= update_hash((void*) save_result.vdec,
                       sizeof(my_decimal), DECIMAL_RESULT,
                       &my_charset_numeric, 0);
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);                // This case should never be chosen
    break;
  }
  DBUG_RETURN(res);
}


double Item_func_set_user_var::val_real()
{
  DBUG_ASSERT(fixed());
  check(0);
  update();					// Store expression
  return m_var_entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int()
{
  DBUG_ASSERT(fixed());
  check(0);
  update();					// Store expression
  return m_var_entry->val_int(&null_value);
}

String *Item_func_set_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  check(0);
  update();					// Store expression
  return m_var_entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal(my_decimal *val)
{
  DBUG_ASSERT(fixed());
  check(0);
  update();					// Store expression
  return m_var_entry->val_decimal(&null_value, val);
}


double Item_func_set_user_var::val_result()
{
  DBUG_ASSERT(fixed());
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_real(&null_value);
}

longlong Item_func_set_user_var::val_int_result()
{
  DBUG_ASSERT(fixed());
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_int(&null_value);
}

bool Item_func_set_user_var::val_bool_result()
{
  DBUG_ASSERT(fixed());
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_int(&null_value) != 0;
}

String *Item_func_set_user_var::str_result(String *str)
{
  DBUG_ASSERT(fixed());
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_str(&null_value, str, decimals);
}


my_decimal *Item_func_set_user_var::val_decimal_result(my_decimal *val)
{
  DBUG_ASSERT(fixed());
  check(TRUE);
  update();					// Store expression
  return m_var_entry->val_decimal(&null_value, val);
}


bool Item_func_set_user_var::is_null_result()
{
  DBUG_ASSERT(fixed());
  check(TRUE);
  update();					// Store expression
  return is_null();
}


void Item_func_set_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("@"));
  str->append(&name);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print_parenthesised(str, query_type, precedence());
}


void Item_func_set_user_var::print_as_stmt(String *str,
                                           enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("set @"));
  str->append(&name);
  str->append(STRING_WITH_LEN(":="));
  args[0]->print_parenthesised(str, query_type, precedence());
}

bool Item_func_set_user_var::send(Protocol *protocol, st_value *buffer)
{
  if (result_field)
  {
    check(1);
    update();
    return protocol->store(result_field);
  }
  return Item::send(protocol, buffer);
}

void Item_func_set_user_var::make_send_field(THD *thd, Send_field *tmp_field)
{
  if (result_field)
  {
    result_field->make_send_field(tmp_field);
    DBUG_ASSERT(tmp_field->table_name.str != 0);
    if (Item::name.str)
      tmp_field->col_name= Item::name;          // Use user supplied name
  }
  else
    Item::make_send_field(thd, tmp_field);
}


/*
  Save the value of a user variable into a field

  SYNOPSIS
    save_in_field()
      field           target field to save the value to
      no_conversion   flag indicating whether conversions are allowed

  DESCRIPTION
    Save the function value into a field and update the user variable
    accordingly. If a result field is defined and the target field doesn't
    coincide with it then the value from the result field will be used as
    the new value of the user variable.

    The reason to have this method rather than simply using the result
    field in the val_xxx() methods is that the value from the result field
    not always can be used when the result field is defined.
    Let's consider the following cases:
    1) when filling a tmp table the result field is defined but the value of it
    is undefined because it has to be produced yet. Thus we can't use it.
    2) on execution of an INSERT ... SELECT statement the save_in_field()
    function will be called to fill the data in the new record. If the SELECT
    part uses a tmp table then the result field is defined and should be
    used in order to get the correct result.

    The difference between the SET_USER_VAR function and regular functions
    like CONCAT is that the Item_func objects for the regular functions are
    replaced by Item_field objects after the values of these functions have
    been stored in a tmp table. Yet an object of the Item_field class cannot
    be used to update a user variable.
    Due to this we have to handle the result field in a special way here and
    in the Item_func_set_user_var::send() function.

  RETURN VALUES
    FALSE       Ok
    TRUE        Error
*/

int Item_func_set_user_var::save_in_field(Field *field, bool no_conversions,
                                          bool can_use_result_field)
{
  bool use_result_field= (!can_use_result_field ? 0 :
                          (result_field && result_field != field));
  int error;

  /* Update the value of the user variable */
  check(use_result_field);
  update();

  if (result_type() == STRING_RESULT ||
      (result_type() == REAL_RESULT &&
       field->result_type() == STRING_RESULT))
  {
    String *result;
    CHARSET_INFO *cs= collation.collation;
    char buff[MAX_FIELD_WIDTH];		// Alloc buffer for small columns
    str_value.set_buffer_if_not_allocated(buff, sizeof(buff), cs);
    result= m_var_entry->val_str(&null_value, &str_value, decimals);

    if (null_value)
    {
      str_value.set_buffer_if_not_allocated(0, 0, cs);
      return set_field_to_null_with_conversions(field, no_conversions);
    }

    /* NOTE: If null_value == FALSE, "result" must be not NULL.  */

    field->set_notnull();
    error=field->store(result->ptr(),result->length(),cs);
    str_value.set_buffer_if_not_allocated(0, 0, cs);
  }
  else if (result_type() == REAL_RESULT)
  {
    double nr= m_var_entry->val_real(&null_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store(nr);
  }
  else if (result_type() == DECIMAL_RESULT)
  {
    my_decimal decimal_value;
    my_decimal *val= m_var_entry->val_decimal(&null_value, &decimal_value);
    if (null_value)
      return set_field_to_null(field);
    field->set_notnull();
    error=field->store_decimal(val);
  }
  else
  {
    longlong nr= m_var_entry->val_int(&null_value);
    if (null_value)
      return set_field_to_null_with_conversions(field, no_conversions);
    field->set_notnull();
    error=field->store(nr, unsigned_flag);
  }
  return error;
}


String *
Item_func_get_user_var::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_func_get_user_var::val_str");
  if (!m_var_entry)
    DBUG_RETURN((String*) 0);			// No such variable
  DBUG_RETURN(m_var_entry->val_str(&null_value, str, decimals));
}


double Item_func_get_user_var::val_real()
{
  DBUG_ASSERT(fixed());
  if (!m_var_entry)
    return 0.0;					// No such variable
  return (m_var_entry->val_real(&null_value));
}


my_decimal *Item_func_get_user_var::val_decimal(my_decimal *dec)
{
  DBUG_ASSERT(fixed());
  if (!m_var_entry)
    return 0;
  return m_var_entry->val_decimal(&null_value, dec);
}


longlong Item_func_get_user_var::val_int()
{
  DBUG_ASSERT(fixed());
  if (!m_var_entry)
    return 0;				// No such variable
  return (m_var_entry->val_int(&null_value));
}


/**
  Get variable by name and, if necessary, put the record of variable 
  use into the binary log.

  When a user variable is invoked from an update query (INSERT, UPDATE etc),
  stores this variable and its value in thd->user_var_events, so that it can be
  written to the binlog (will be written just before the query is written, see
  log.cc).

  @param      thd        Current thread
  @param      name       Variable name
  @param[out] out_entry  variable structure or NULL. The pointer is set
                         regardless of whether function succeeded or not.

  @retval
    0  OK
  @retval
    1  Failed to put appropriate record into binary log

*/

static int
get_var_with_binlog(THD *thd, enum_sql_command sql_command,
                    LEX_CSTRING *name, user_var_entry **out_entry)
{
  BINLOG_USER_VAR_EVENT *user_var_event;
  user_var_entry *var_entry;
  var_entry= get_variable(&thd->user_vars, name, 0);

  /*
    Any reference to user-defined variable which is done from stored
    function or trigger affects their execution and the execution of the
    calling statement. We must log all such variables even if they are 
    not involved in table-updating statements.
  */
  if (!(opt_bin_log && 
       (is_update_query(sql_command) || thd->in_sub_stmt)))
  {
    *out_entry= var_entry;
    return 0;
  }

  if (!var_entry)
  {
    /*
      If the variable does not exist, it's NULL, but we want to create it so
      that it gets into the binlog (if it didn't, the slave could be
      influenced by a variable of the same name previously set by another
      thread).
      We create it like if it had been explicitly set with SET before.
      The 'new' mimics what sql_yacc.yy does when 'SET @a=10;'.
      sql_set_variables() is what is called from 'case SQLCOM_SET_OPTION'
      in dispatch_command()). Instead of building a one-element list to pass to
      sql_set_variables(), we could instead manually call check() and update();
      this would save memory and time; but calling sql_set_variables() makes
      one unique place to maintain (sql_set_variables()). 

      Manipulation with lex is necessary since free_underlaid_joins
      is going to release memory belonging to the main query.
    */

    List<set_var_base> tmp_var_list;
    LEX *sav_lex= thd->lex, lex_tmp;
    thd->lex= &lex_tmp;
    lex_start(thd);
    tmp_var_list.push_back(new (thd->mem_root)
                           set_var_user(new (thd->mem_root)
                                        Item_func_set_user_var(thd, name,
                                                               new (thd->mem_root) Item_null(thd))),
                           thd->mem_root);
    /* Create the variable if the above allocations succeeded */
    if (unlikely(thd->is_fatal_error) ||
        unlikely(sql_set_variables(thd, &tmp_var_list, false)))
    {
      thd->lex= sav_lex;
      goto err;
    }
    thd->lex= sav_lex;
    if (unlikely(!(var_entry= get_variable(&thd->user_vars, name, 0))))
      goto err;
  }
  else if (var_entry->used_query_id == thd->query_id ||
           mysql_bin_log.is_query_in_union(thd, var_entry->used_query_id))
  {
    /* 
       If this variable was already stored in user_var_events by this query
       (because it's used in more than one place in the query), don't store
       it.
    */
    *out_entry= var_entry;
    return 0;
  }

  size_t size;
  /*
    First we need to store value of var_entry, when the next situation
    appears:
    > set @a:=1;
    > insert into t1 values (@a), (@a:=@a+1), (@a:=@a+1);
    We have to write to binlog value @a= 1.

    We allocate the user_var_event on user_var_events_alloc pool, not on
    the this-statement-execution pool because in SPs user_var_event objects 
    may need to be valid after current [SP] statement execution pool is
    destroyed.
  */
  size= ALIGN_SIZE(sizeof(BINLOG_USER_VAR_EVENT)) + var_entry->length;
  if (unlikely(!(user_var_event= (BINLOG_USER_VAR_EVENT *)
                 alloc_root(thd->user_var_events_alloc, size))))
    goto err;

  user_var_event->value= (char*) user_var_event +
    ALIGN_SIZE(sizeof(BINLOG_USER_VAR_EVENT));
  user_var_event->user_var_event= var_entry;
  user_var_event->type= var_entry->type;
  user_var_event->charset_number= var_entry->charset()->number;
  user_var_event->unsigned_flag= var_entry->unsigned_flag;
  if (!var_entry->value)
  {
    /* NULL value*/
    user_var_event->length= 0;
    user_var_event->value= 0;
  }
  else
  {
    user_var_event->length= var_entry->length;
    memcpy(user_var_event->value, var_entry->value,
           var_entry->length);
  }
  /* Mark that this variable has been used by this query */
  var_entry->used_query_id= thd->query_id;
  if (insert_dynamic(&thd->user_var_events, (uchar*) &user_var_event))
    goto err;

  *out_entry= var_entry;
  return 0;

err:
  *out_entry= var_entry;
  return 1;
}

bool Item_func_get_user_var::fix_length_and_dec(THD *thd)
{
  int error;
  set_maybe_null();
  decimals=NOT_FIXED_DEC;
  max_length=MAX_BLOB_WIDTH;

  error= get_var_with_binlog(thd, thd->lex->sql_command, &name, &m_var_entry);

  /*
    If the variable didn't exist it has been created as a STRING-type.
    'm_var_entry' is NULL only if there occurred an error during the call to
    get_var_with_binlog.
  */
  if (likely(!error && m_var_entry))
  {
    unsigned_flag= m_var_entry->unsigned_flag;
    max_length= (uint32)m_var_entry->length;
    switch (m_var_entry->type) {
    case REAL_RESULT:
      collation.set(&my_charset_numeric, DERIVATION_NUMERIC);
      fix_char_length(DBL_DIG + 8);
      set_handler(&type_handler_double);
      break;
    case INT_RESULT:
      collation.set(&my_charset_numeric, DERIVATION_NUMERIC);
      fix_char_length(MAX_BIGINT_WIDTH);
      decimals=0;
      if (unsigned_flag)
        set_handler(&type_handler_ulonglong);
      else
        set_handler(&type_handler_slonglong);
      break;
    case STRING_RESULT:
      collation.set(m_var_entry->charset(), DERIVATION_IMPLICIT);
      max_length= MAX_BLOB_WIDTH - 1;
      set_handler(&type_handler_long_blob);
      break;
    case DECIMAL_RESULT:
      collation.set(&my_charset_numeric, DERIVATION_NUMERIC);
      fix_char_length(DECIMAL_MAX_STR_LENGTH);
      decimals= DECIMAL_MAX_SCALE;
      set_handler(&type_handler_newdecimal);
      break;
    case ROW_RESULT:                            // Keep compiler happy
    case TIME_RESULT:
      DBUG_ASSERT(0);                // This case should never be chosen
      break;
    }
  }
  else
  {
    collation.set(&my_charset_bin, DERIVATION_IMPLICIT);
    null_value= 1;
    set_handler(&type_handler_long_blob);
    max_length= MAX_BLOB_WIDTH;
  }
  return false;
}


bool Item_func_get_user_var::const_item() const
{
  return (!m_var_entry ||
          current_thd->query_id != m_var_entry->update_query_id);
}


void Item_func_get_user_var::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("@"));
  append_identifier(current_thd, str, &name);
}


bool Item_func_get_user_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_user_var *other=(Item_func_get_user_var*) item;
  return (name.length == other->name.length &&
	  !memcmp(name.str, other->name.str, name.length));
}


bool Item_func_get_user_var::set_value(THD *thd,
                                       sp_rcontext * /*ctx*/, Item **it)
{
  LEX_CSTRING tmp_name= get_name();
  Item_func_set_user_var *suv= new (thd->mem_root) Item_func_set_user_var(thd, &tmp_name, *it);
  /*
    Item_func_set_user_var is not fixed after construction, call
    fix_fields().
  */
  return (!suv || suv->fix_fields(thd, it) || suv->check(0) || suv->update());
}


bool Item_user_var_as_out_param::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(!fixed());
  DBUG_ASSERT(thd->lex->exchange);
  if (!(entry= get_variable(&thd->user_vars, &org_name, 1)))
    return TRUE;
  entry->type= STRING_RESULT;
  /*
    Let us set the same collation which is used for loading
    of fields in LOAD DATA INFILE.
    (Since Item_user_var_as_out_param is used only there).
  */
  entry->set_charset(thd->lex->exchange->cs ?
                     thd->lex->exchange->cs :
                     thd->variables.collation_database);
  entry->update_query_id= thd->query_id;
  return FALSE;
}


void Item_user_var_as_out_param::set_null_value(CHARSET_INFO* cs)
{
  ::update_hash(entry, TRUE, 0, 0, STRING_RESULT, cs, 0 /* unsigned_arg */);
}


void Item_user_var_as_out_param::set_value(const char *str, uint length,
                                           CHARSET_INFO* cs)
{
  ::update_hash(entry, FALSE, (void*)str, length, STRING_RESULT, cs,
                0 /* unsigned_arg */);
}


double Item_user_var_as_out_param::val_real()
{
  DBUG_ASSERT(0);
  return 0.0;
}


longlong Item_user_var_as_out_param::val_int()
{
  DBUG_ASSERT(0);
  return 0;
}


String* Item_user_var_as_out_param::val_str(String *str)
{
  DBUG_ASSERT(0);
  return 0;
}


my_decimal* Item_user_var_as_out_param::val_decimal(my_decimal *decimal_buffer)
{
  DBUG_ASSERT(0);
  return 0;
}


bool Item_user_var_as_out_param::get_date(THD *thd, MYSQL_TIME *ltime,
                                          date_mode_t fuzzydate)
{
  DBUG_ASSERT(0);
  return true;
}


void Item_user_var_as_out_param::load_data_print_for_log_event(THD *thd,
                                                               String *str)
                                                               const
{
  str->append('@');
  append_identifier(thd, str, &org_name);
}


Item_func_get_system_var::
Item_func_get_system_var(THD *thd, sys_var *var_arg, enum_var_type var_type_arg,
                       LEX_CSTRING *component_arg, const char *name_arg,
                       size_t name_len_arg):
  Item_func(thd), var(var_arg), var_type(var_type_arg),
  orig_var_type(var_type_arg), component(*component_arg), cache_present(0)
{
  /* set_name() will allocate the name */
  set_name(thd, name_arg, (uint) name_len_arg, system_charset_info);
}


bool Item_func_get_system_var::is_written_to_binlog()
{
  return var->is_written_to_binlog(var_type);
}


void Item_func_get_system_var::update_null_value()
{
  THD *thd= current_thd;
  int save_no_errors= thd->no_errors;
  thd->no_errors= TRUE;
  type_handler()->Item_update_null_value(this);
  thd->no_errors= save_no_errors;
}


bool Item_func_get_system_var::fix_length_and_dec(THD *thd)
{
  const char *cptr;
  set_maybe_null();
  max_length= 0;

  if (var->check_type(var_type))
  {
    if (var_type != OPT_DEFAULT)
    {
      my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0),
               var->name.str, var_type == OPT_GLOBAL ? "SESSION" : "GLOBAL");
      return TRUE;
    }
    /* As there was no local variable, return the global value */
    var_type= OPT_GLOBAL;
  }

  switch (var->show_type())
  {
    case SHOW_HA_ROWS:
    case SHOW_UINT:
    case SHOW_ULONG:
    case SHOW_ULONGLONG:
      unsigned_flag= TRUE;
      /* fall through */
    case SHOW_SINT:
    case SHOW_SLONG:
    case SHOW_SLONGLONG:
      collation= DTCollation_numeric();
      fix_char_length(MY_INT64_NUM_DECIMAL_DIGITS);
      decimals=0;
      break;
    case SHOW_CHAR:
    case SHOW_CHAR_PTR:
      mysql_mutex_lock(&LOCK_global_system_variables);
      cptr= var->show_type() == SHOW_CHAR ?
          reinterpret_cast<const char*>(var->value_ptr(thd, var_type,
                                                       &component)) :
          *reinterpret_cast<const char* const*>(var->value_ptr(thd,
                                                               var_type,
                                                               &component));
      if (cptr)
        max_length= (uint32) system_charset_info->numchars(cptr,
                                                           cptr + strlen(cptr));
      mysql_mutex_unlock(&LOCK_global_system_variables);
      collation.set(system_charset_info, DERIVATION_SYSCONST);
      max_length*= system_charset_info->mbmaxlen;
      decimals=NOT_FIXED_DEC;
      break;
    case SHOW_LEX_STRING:
      {
        mysql_mutex_lock(&LOCK_global_system_variables);
        const LEX_STRING *ls=
                reinterpret_cast<const LEX_STRING*>(var->value_ptr(current_thd,
                                                                   var_type,
                                                                   &component));
        max_length= (uint32) system_charset_info->numchars(ls->str,
                                                           ls->str + ls->length);
        mysql_mutex_unlock(&LOCK_global_system_variables);
        collation.set(system_charset_info, DERIVATION_SYSCONST);
        max_length*= system_charset_info->mbmaxlen;
        decimals=NOT_FIXED_DEC;
      }
      break;
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      collation= DTCollation_numeric();
      fix_char_length(1);
      decimals=0;
      break;
    case SHOW_DOUBLE:
      decimals= 6;
      collation= DTCollation_numeric();
      fix_char_length(DBL_DIG + 6);
      break;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      break;
  }
  return FALSE;
}


void Item_func_get_system_var::print(String *str, enum_query_type query_type)
{
  if (name.length)
    str->append(&name);
  else
  {
    str->append(STRING_WITH_LEN("@@"));
    if (component.length)
    {
      str->append(&component);
      str->append('.');
    }
    else if (var_type == SHOW_OPT_GLOBAL && var->scope() != sys_var::GLOBAL)
    {
      str->append(STRING_WITH_LEN("global."));
    }
    str->append(&var->name);
  }
}

bool Item_func_get_system_var::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function("@@", var->name.str, arg, VCOL_SESSION_FUNC);
}


const Type_handler *Item_func_get_system_var::type_handler() const
{
  switch (var->show_type())
  {
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
    case SHOW_SINT:
    case SHOW_SLONG:
    case SHOW_SLONGLONG:
      return &type_handler_slonglong;
    case SHOW_UINT:
    case SHOW_ULONG:
    case SHOW_ULONGLONG:
    case SHOW_HA_ROWS:
      return &type_handler_ulonglong;
    case SHOW_CHAR: 
    case SHOW_CHAR_PTR: 
    case SHOW_LEX_STRING:
      return &type_handler_varchar;
    case SHOW_DOUBLE:
      return &type_handler_double;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
      return &type_handler_varchar;              // keep the compiler happy
  }
}


longlong Item_func_get_system_var::val_int()
{
  THD *thd= current_thd;

  DBUG_EXECUTE_IF("simulate_non_gtid_aware_master",
                  {
                    if (0 == strcmp("gtid_domain_id", var->name.str))
                    {
                      my_error(ER_VAR_CANT_BE_READ, MYF(0), var->name.str);
                      return 0;
                    }
                  });
  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      return cached_llval;
    } 
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      cached_llval= (longlong) cached_dval;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_llval= longlong_from_string_with_check(&cached_strval);
      else
        cached_llval= 0;
      cache_present|= GET_SYS_VAR_CACHE_LONG;
      return cached_llval;
    }
  }

  cached_llval= var->val_int(&null_value, thd, var_type, &component);
  cache_present |= GET_SYS_VAR_CACHE_LONG;
  used_query_id= thd->query_id;
  cached_null_value= null_value;
  return cached_llval;
}


String* Item_func_get_system_var::val_str(String* str)
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set (cached_llval, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_strval.set_real (cached_dval, decimals, collation.collation);
      cache_present|= GET_SYS_VAR_CACHE_STRING;
      return null_value ? NULL : &cached_strval;
    }
  }

  str= var->val_str(&cached_strval, thd, var_type, &component);
  cache_present|= GET_SYS_VAR_CACHE_STRING;
  used_query_id= thd->query_id;
  cached_null_value= null_value= !str;
  return str;
}


double Item_func_get_system_var::val_real()
{
  THD *thd= current_thd;

  if (cache_present && thd->query_id == used_query_id)
  {
    if (cache_present & GET_SYS_VAR_CACHE_DOUBLE)
    {
      null_value= cached_null_value;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_LONG)
    {
      null_value= cached_null_value;
      cached_dval= (double)cached_llval;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
    else if (cache_present & GET_SYS_VAR_CACHE_STRING)
    {
      null_value= cached_null_value;
      if (!null_value)
        cached_dval= double_from_string_with_check(&cached_strval);
      else
        cached_dval= 0;
      cache_present|= GET_SYS_VAR_CACHE_DOUBLE;
      return cached_dval;
    }
  }

  cached_dval= var->val_real(&null_value, thd, var_type, &component);
  cache_present |= GET_SYS_VAR_CACHE_DOUBLE;
  used_query_id= thd->query_id;
  cached_null_value= null_value;
  return cached_dval;
}


bool Item_func_get_system_var::eq(const Item *item, bool binary_cmp) const
{
  /* Assume we don't have rtti */
  if (this == item)
    return 1;					// Same item is same.
  /* Check if other type is also a get_user_var() object */
  if (item->type() != FUNC_ITEM ||
      ((Item_func*) item)->functype() != functype())
    return 0;
  Item_func_get_system_var *other=(Item_func_get_system_var*) item;
  return (var == other->var && var_type == other->var_type);
}


void Item_func_get_system_var::cleanup()
{
  Item_func::cleanup();
  cache_present= 0;
  var_type= orig_var_type;
  cached_strval.free();
}

/**
   @retval
   0 ok
   1 OOM error
*/

bool Item_func_match::init_search(THD *thd, bool no_order)
{
  DBUG_ENTER("Item_func_match::init_search");

  if (!table->file->is_open())
    DBUG_RETURN(0);

  /* Check if init_search() has been called before */
  if (ft_handler)
  {
    if (join_key)
      table->file->ft_handler= ft_handler;
    DBUG_RETURN(0);
  }

  if (key == NO_SUCH_KEY)
  {
    List<Item> fields;
    fields.push_back(new (thd->mem_root)
                     Item_string(thd, " ", 1, cmp_collation.collation),
                     thd->mem_root);
    for (uint i= 1; i < arg_count; i++)
      fields.push_back(args[i]);
    concat_ws= new (thd->mem_root) Item_func_concat_ws(thd, fields);
    if (unlikely(thd->is_fatal_error))
      DBUG_RETURN(1);                           // OOM in new or push_back
    /*
      Above function used only to get value and do not need fix_fields for it:
      Item_string - basic constant
      fields - fix_fields() was already called for this arguments
      Item_func_concat_ws - do not need fix_fields() to produce value
    */
    concat_ws->quick_fix_field();
  }

  if (master)
  {
    join_key= master->join_key= join_key | master->join_key;
    if (master->init_search(thd, no_order))
      DBUG_RETURN(1);
    ft_handler= master->ft_handler;
    join_key= master->join_key;
    DBUG_RETURN(0);
  }

  String *ft_tmp= 0;

  // MATCH ... AGAINST (NULL) is meaningless, but possible
  if (!(ft_tmp=key_item()->val_str(&value)))
  {
    ft_tmp= &value;
    value.set("", 0, cmp_collation.collation);
  }

  if (ft_tmp->charset() != cmp_collation.collation)
  {
    uint dummy_errors;
    if (search_value.copy(ft_tmp->ptr(), ft_tmp->length(), ft_tmp->charset(),
                          cmp_collation.collation, &dummy_errors))
      DBUG_RETURN(1);
    ft_tmp= &search_value;
  }

  if (join_key && !no_order)
    match_flags|=FT_SORTED;

  if (key != NO_SUCH_KEY)
    THD_STAGE_INFO(table->in_use, stage_fulltext_initialization);

  ft_handler= table->file->ft_init_ext(match_flags, key, ft_tmp);

  if (join_key)
    table->file->ft_handler=ft_handler;

  DBUG_RETURN(0);
}


bool Item_func_match::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  Item *UNINIT_VAR(item);                        // Safe as arg_count is > 1

  status_var_increment(thd->status_var.feature_fulltext);

  set_maybe_null();
  join_key=0;

  /*
    const_item is assumed in quite a bit of places, so it would be difficult
    to remove;  If it would ever to be removed, this should include
    modifications to find_best and auto_close as complement to auto_init code
    above.
   */
  if (Item_func::fix_fields(thd, ref) ||
      !args[0]->const_during_execution())
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"AGAINST");
    return TRUE;
  }

  bool allows_multi_table_search= true;
  const_item_cache=0;
  table= 0;
  for (uint i=1 ; i < arg_count ; i++)
  {
    item= args[i]= args[i]->real_item();
    /*
      When running in PS mode, some Item_field's can already be replaced
      to Item_func_conv_charset during PREPARE time. This is possible
      in case of "MATCH (f1,..,fN) AGAINST (... IN BOOLEAN MODE)"
      when running without any fulltext indexes and when fields f1..fN
      have different character sets.
      So we check for FIELD_ITEM only during prepare time and in non-PS mode,
      and do not check in PS execute time.
    */
    if (!thd->stmt_arena->is_stmt_execute() &&
        item->type() != Item::FIELD_ITEM)
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "MATCH");
      return TRUE;
    }
    /*
      During the prepare-time execution of fix_fields() of a PS query some
      Item_fields's could have been already replaced to Item_func_conv_charset
      (by the call for agg_arg_charsets_for_comparison below()).
      But agg_arg_charsets_for_comparison() is written in a way that
      at least *one* of the Item_field's is not replaced.
      This makes sure that "table" gets initialized during PS execution time.
    */
    if (item->type() == Item::FIELD_ITEM)
      table= ((Item_field *)item)->field->table;

    allows_multi_table_search &= allows_search_on_non_indexed_columns(table);
  }

  /*
    Check that all columns come from the same table.
    We've already checked that columns in MATCH are fields so
    PARAM_TABLE_BIT can only appear from AGAINST argument.
  */
  if ((used_tables_cache & ~PARAM_TABLE_BIT) != item->used_tables())
    key=NO_SUCH_KEY;

  if (key == NO_SUCH_KEY && !allows_multi_table_search)
  {
    my_error(ER_WRONG_ARGUMENTS,MYF(0),"MATCH");
    return TRUE;
  }
  if (!(table->file->ha_table_flags() & HA_CAN_FULLTEXT))
  {
    my_error(ER_TABLE_CANT_HANDLE_FT, MYF(0), table->file->table_type());
    return 1;
  }
  table->fulltext_searched=1;
  return agg_arg_charsets_for_comparison(cmp_collation, args+1, arg_count-1);
}

bool Item_func_match::fix_index()
{
  Item_field *item;
  uint ft_to_key[MAX_KEY], ft_cnt[MAX_KEY], fts=0, keynr;
  uint max_cnt=0, mkeys=0, i;

  /*
    We will skip execution if the item is not fixed
    with fix_field
  */
  if (!fixed())
    return false;

  if (key == NO_SUCH_KEY)
    return 0;
  
  if (!table) 
    goto err;

  for (keynr=0 ; keynr < table->s->keys ; keynr++)
  {
    if ((table->key_info[keynr].flags & HA_FULLTEXT) &&
        (match_flags & FT_BOOL ?
         table->keys_in_use_for_query.is_set(keynr) :
         table->s->usable_indexes(table->in_use).is_set(keynr)))
    {
      ft_to_key[fts]=keynr;
      ft_cnt[fts]=0;
      fts++;
    }
  }

  if (!fts)
    goto err;

  for (i=1; i < arg_count; i++)
  {
    if (args[i]->type() != FIELD_ITEM)
      goto err;
    item=(Item_field*)args[i];
    for (keynr=0 ; keynr < fts ; keynr++)
    {
      KEY *ft_key=&table->key_info[ft_to_key[keynr]];
      uint key_parts=ft_key->user_defined_key_parts;

      for (uint part=0 ; part < key_parts ; part++)
      {
	if (item->field->eq(ft_key->key_part[part].field))
	  ft_cnt[keynr]++;
      }
    }
  }

  for (keynr=0 ; keynr < fts ; keynr++)
  {
    if (ft_cnt[keynr] > max_cnt)
    {
      mkeys=0;
      max_cnt=ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
    if (max_cnt && ft_cnt[keynr] == max_cnt)
    {
      mkeys++;
      ft_cnt[mkeys]=ft_cnt[keynr];
      ft_to_key[mkeys]=ft_to_key[keynr];
      continue;
    }
  }

  for (keynr=0 ; keynr <= mkeys ; keynr++)
  {
    // partial keys doesn't work
    if (max_cnt < arg_count-1 ||
        max_cnt < table->key_info[ft_to_key[keynr]].user_defined_key_parts)
      continue;

    key=ft_to_key[keynr];

    return 0;
  }

err:
  if (allows_search_on_non_indexed_columns(table))
  {
    key=NO_SUCH_KEY;
    return 0;
  }
  my_message(ER_FT_MATCHING_KEY_NOT_FOUND,
             ER(ER_FT_MATCHING_KEY_NOT_FOUND), MYF(0));
  return 1;
}


bool Item_func_match::eq(const Item *item, bool binary_cmp) const
{
  if (item->type() != FUNC_ITEM ||
      ((Item_func*)item)->functype() != FT_FUNC ||
      match_flags != ((Item_func_match*)item)->match_flags)
    return 0;

  Item_func_match *ifm=(Item_func_match*) item;

  if (key == ifm->key && table == ifm->table &&
      key_item()->eq(ifm->key_item(), binary_cmp))
    return 1;

  return 0;
}


double Item_func_match::val_real()
{
  DBUG_ASSERT(fixed());
  DBUG_ENTER("Item_func_match::val");
  if (ft_handler == NULL)
    DBUG_RETURN(-1.0);

  if (key != NO_SUCH_KEY && table->null_row) /* NULL row from an outer join */
    DBUG_RETURN(0.0);

  if (join_key)
  {
    if (table->file->ft_handler)
      DBUG_RETURN(ft_handler->please->get_relevance(ft_handler));
    join_key=0;
  }

  if (key == NO_SUCH_KEY)
  {
    String *a= concat_ws->val_str(&value);
    if ((null_value= (a == 0)) || !a->length())
      DBUG_RETURN(0);
    DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
				      (uchar *)a->ptr(), a->length()));
  }
  DBUG_RETURN(ft_handler->please->find_relevance(ft_handler,
                                                 table->record[0], 0));
}

void Item_func_match::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("(match "));
  print_args(str, 1, query_type);
  str->append(STRING_WITH_LEN(" against ("));
  args[0]->print(str, query_type);
  if (match_flags & FT_BOOL)
    str->append(STRING_WITH_LEN(" in boolean mode"));
  else if (match_flags & FT_EXPAND)
    str->append(STRING_WITH_LEN(" with query expansion"));
  str->append(STRING_WITH_LEN("))"));
}


class Func_handler_bit_xor_int_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return item->arguments()[0]->to_longlong_null() ^
           item->arguments()[1]->to_longlong_null();
  }
};


class Func_handler_bit_xor_dec_to_ulonglong:
        public Item_handled_func::Handler_ulonglong
{
public:
  Longlong_null to_longlong_null(Item_handled_func *item) const
  {
    DBUG_ASSERT(item->fixed());
    return VDec(item->arguments()[0]).to_xlonglong_null() ^
           VDec(item->arguments()[1]).to_xlonglong_null();
  }
};


bool Item_func_bit_xor::fix_length_and_dec(THD *thd)
{
  static const Func_handler_bit_xor_int_to_ulonglong ha_int_to_ull;
  static const Func_handler_bit_xor_dec_to_ulonglong ha_dec_to_ull;
  return fix_length_and_dec_op2_std(&ha_int_to_ull, &ha_dec_to_ull);
}


/***************************************************************************
  System variables
****************************************************************************/

/**
  Return value of an system variable base[.name] as a constant item.

  @param thd			Thread handler
  @param var_type		global / session
  @param name		        Name of base or system variable
  @param component		Component.

  @note
    If component.str = 0 then the variable name is in 'name'

  @return
    - 0  : error
    - #  : constant item
*/


Item *get_system_var(THD *thd, enum_var_type var_type,
                     const LEX_CSTRING *name,
		     const LEX_CSTRING *component)
{
  sys_var *var;
  LEX_CSTRING base_name, component_name;

  if (component->str)
  {
    base_name= *component;
    component_name= *name;
  }
  else
  {
    base_name= *name;
    component_name= *component;			// Empty string
  }

  if (!(var= find_sys_var(thd, base_name.str, base_name.length)))
    return 0;
  if (component->str)
  {
    if (!var->is_struct())
    {
      my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), base_name.str);
      return 0;
    }
  }
  thd->lex->uncacheable(UNCACHEABLE_SIDEEFFECT);

  set_if_smaller(component_name.length, MAX_SYS_VAR_LENGTH);

  return new (thd->mem_root) Item_func_get_system_var(thd, var, var_type,
                                                      &component_name,
                                                      NULL, 0);
}


longlong Item_func_row_count::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;

  return thd->get_row_count_func();
}




Item_func_sp::Item_func_sp(THD *thd, Name_resolution_context *context_arg,
                           sp_name *name, const Sp_handler *sph):
  Item_func(thd), Item_sp(thd, context_arg, name), m_handler(sph)
{
  set_maybe_null();
}


Item_func_sp::Item_func_sp(THD *thd, Name_resolution_context *context_arg,
                           sp_name *name_arg, const Sp_handler *sph,
                           List<Item> &list):
  Item_func(thd, list), Item_sp(thd, context_arg, name_arg), m_handler(sph)
{
  set_maybe_null();
}


void
Item_func_sp::cleanup()
{
  Item_sp::cleanup();
  Item_func::cleanup();
}

LEX_CSTRING
Item_func_sp::func_name_cstring() const
{
  return Item_sp::func_name_cstring(current_thd,
                                    m_handler == &sp_handler_package_function);
}


void my_missing_function_error(const LEX_CSTRING &token, const char *func_name)
{
  if (token.length && is_lex_native_function (&token))
    my_error(ER_FUNC_INEXISTENT_NAME_COLLISION, MYF(0), func_name);
  else
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION", func_name);
}


/**
  @note
  Deterministic stored procedures are considered inexpensive.
  Consequently such procedures may be evaluated during optimization,
  if they are constant (checked by the optimizer).
*/

bool Item_func_sp::is_expensive()
{
  return !m_sp->detistic() ||
          current_thd->locked_tables_mode < LTM_LOCK_TABLES;
}


/**
  @brief Initialize local members with values from the Field interface.

  @note called from Item::fix_fields.
*/

bool Item_func_sp::fix_length_and_dec(THD *thd)
{
  DBUG_ENTER("Item_func_sp::fix_length_and_dec");

  DBUG_ASSERT(sp_result_field);
  Type_std_attributes::set(sp_result_field->type_std_attributes());
  // There is a bug in the line below. See MDEV-11292 for details.
  collation.derivation= DERIVATION_COERCIBLE;
  set_maybe_null();

  DBUG_RETURN(FALSE);
}


bool
Item_func_sp::execute()
{
  /* Execute function and store the return value in the field. */
  return Item_sp::execute(current_thd, &null_value, args, arg_count);
}


void
Item_func_sp::make_send_field(THD *thd, Send_field *tmp_field)
{
  DBUG_ENTER("Item_func_sp::make_send_field");
  DBUG_ASSERT(sp_result_field);
  sp_result_field->make_send_field(tmp_field);
  if (name.str)
  {
    DBUG_ASSERT(name.length == strlen(name.str));
    tmp_field->col_name= name;
  }
  DBUG_VOID_RETURN;
}


const Type_handler *Item_func_sp::type_handler() const
{
  DBUG_ENTER("Item_func_sp::type_handler");
  DBUG_PRINT("info", ("m_sp = %p", (void *) m_sp));
  DBUG_ASSERT(sp_result_field);
  // This converts ENUM/SET to STRING
  const Type_handler *handler= sp_result_field->type_handler();
  DBUG_RETURN(handler->type_handler_for_item_field());
}


longlong Item_func_found_rows::val_int()
{
  DBUG_ASSERT(fixed());
  return current_thd->found_rows();
}


longlong Item_func_oracle_sql_rowcount::val_int()
{
  DBUG_ASSERT(fixed());
  THD *thd= current_thd;
  /*
    In case when a query like this:
      INSERT a INTO @va FROM t1;
    returns multiple rows, SQL%ROWCOUNT should report 1 rather than -1.
  */
  longlong rows= thd->get_row_count_func();
  return rows != -1 ? rows :                   // ROW_COUNT()
                      thd->found_rows();       // FOUND_ROWS()
}


longlong Item_func_sqlcode::val_int()
{
  DBUG_ASSERT(fixed());
  DBUG_ASSERT(!null_value);
  Diagnostics_area::Sql_condition_iterator it=
    current_thd->get_stmt_da()->sql_conditions();
  const Sql_condition *err;
  if ((err= it++))
    return err->get_sql_errno();
  return 0;
}


bool
Item_func_sp::fix_fields(THD *thd, Item **ref)
{
  bool res;
  DBUG_ENTER("Item_func_sp::fix_fields");
  DBUG_ASSERT(fixed() == 0);
  sp_head *sp= m_handler->sp_find_routine(thd, m_name, true);

  /* 
    Checking privileges to execute the function while creating view and
    executing the function of select.
   */
  if (!(thd->lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW) ||
      (thd->lex->sql_command == SQLCOM_CREATE_VIEW))
  {
    Security_context *save_security_ctx= thd->security_ctx;
    if (context && context->security_ctx)
      thd->security_ctx= context->security_ctx;

    /*
      If the routine is not found, let's still check EXECUTE_ACL to decide
      whether to return "Access denied" or "Routine does not exist".
    */
    res= sp ? sp->check_execute_access(thd) :
              check_routine_access(thd, EXECUTE_ACL, &m_name->m_db,
                                   &m_name->m_name,
                                   &sp_handler_function, false);
    thd->security_ctx= save_security_ctx;

    if (res)
    {
      process_error(thd);
      DBUG_RETURN(res);
    }
  }


  /* Custom aggregates are transformed into an Item_sum_sp. We can not do this
     earlier as we have no way of knowing what kind of Item we should create
     when parsing the query.

     TODO(cvicentiu): See if this limitation can be lifted.
  */

  DBUG_ASSERT(m_sp == NULL);
  if (!(m_sp= sp))
  {
    my_missing_function_error(m_name->m_name, ErrConvDQName(m_name).ptr());
    process_error(thd);
    DBUG_RETURN(TRUE);
  }

  /*
    We must call init_result_field before Item_func::fix_fields()
    to make m_sp and result_field members available to fix_length_and_dec(),
    which is called from Item_func::fix_fields().
  */
  res= init_result_field(thd, max_length, maybe_null(), &null_value, &name);

  if (res)
    DBUG_RETURN(TRUE);

  if (m_sp->agg_type() == GROUP_AGGREGATE)
  {
    Item_sum_sp *item_sp;
    Query_arena *arena, backup;
    arena= thd->activate_stmt_arena_if_needed(&backup);

    if (arg_count)
    {
      List<Item> list;
      for (uint i= 0; i < arg_count; i++)
        list.push_back(args[i]);
      item_sp= new (thd->mem_root) Item_sum_sp(thd, context, m_name, sp, list);
    }
    else
      item_sp= new (thd->mem_root) Item_sum_sp(thd, context, m_name, sp);

    if (arena)
      thd->restore_active_arena(arena, &backup);
    if (!item_sp)
      DBUG_RETURN(TRUE);
    *ref= item_sp;
    item_sp->name= name;
    bool err= item_sp->fix_fields(thd, ref);
    if (err)
      DBUG_RETURN(TRUE);

    DBUG_RETURN(FALSE);
  }

  res= Item_func::fix_fields(thd, ref);

  if (res)
    DBUG_RETURN(TRUE);

  if (thd->lex->is_view_context_analysis())
  {
    /*
      Here we check privileges of the stored routine only during view
      creation, in order to validate the view.  A runtime check is
      performed in Item_func_sp::execute(), and this method is not
      called during context analysis.  Notice, that during view
      creation we do not infer into stored routine bodies and do not
      check privileges of its statements, which would probably be a
      good idea especially if the view has SQL SECURITY DEFINER and
      the used stored procedure has SQL SECURITY DEFINER.
    */
    res= sp_check_access(thd);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /*
      Try to set and restore the security context to see whether it's valid
    */
    Security_context *save_secutiry_ctx;
    res= set_routine_security_ctx(thd, m_sp, &save_secutiry_ctx);
    if (!res)
      m_sp->m_security_ctx.restore_security_context(thd, save_secutiry_ctx);
    
#endif /* ! NO_EMBEDDED_ACCESS_CHECKS */
  }

  if (!m_sp->detistic())
  {
    used_tables_cache |= RAND_TABLE_BIT;
    const_item_cache= FALSE;
  }

  DBUG_RETURN(res);
}


void Item_func_sp::update_used_tables()
{
  Item_func::update_used_tables();

  if (!m_sp->detistic())
  {
    used_tables_cache |= RAND_TABLE_BIT;
    const_item_cache= FALSE;
  }
}

bool Item_func_sp::check_vcol_func_processor(void *arg)
{
  return mark_unsupported_function(func_name(), "()", arg, VCOL_IMPOSSIBLE);
}

/*
  uuid_short handling.

  The short uuid is defined as a longlong that contains the following bytes:

  Bytes  Comment
  1      Server_id & 255
  4      Startup time of server in seconds
  3      Incrementor

  This means that an uuid is guaranteed to be unique
  even in a replication environment if the following holds:

  - The last byte of the server id is unique
  - If you between two shutdown of the server don't get more than
    an average of 2^24 = 16M calls to uuid_short() per second.
*/

ulonglong uuid_value;

void uuid_short_init()
{
  uuid_value= ((((ulonglong) global_system_variables.server_id) << 56) +
               (((ulonglong) server_start_time) << 24));
}

ulonglong server_uuid_value()
{
  ulonglong val;
  mysql_mutex_lock(&LOCK_short_uuid_generator);
  val= uuid_value++;
  mysql_mutex_unlock(&LOCK_short_uuid_generator);
  return val;
}

longlong Item_func_uuid_short::val_int()
{
  return (longlong) server_uuid_value();
}


/**
  Last_value - return last argument.
*/

void Item_func_last_value::evaluate_sideeffects()
{
  DBUG_ASSERT(fixed() && arg_count > 0);
  for (uint i= 0; i < arg_count-1 ; i++)
    args[i]->val_int();
}

String *Item_func_last_value::val_str(String *str)
{
  String *tmp;
  evaluate_sideeffects();
  tmp= last_value->val_str(str);
  null_value= last_value->null_value;
  return tmp;
}


bool Item_func_last_value::val_native(THD *thd, Native *to)
{
  evaluate_sideeffects();
  return val_native_from_item(thd, last_value, to);
}


longlong Item_func_last_value::val_int()
{
  longlong tmp;
  evaluate_sideeffects();
  tmp= last_value->val_int();
  null_value= last_value->null_value;
  return tmp;
}

double Item_func_last_value::val_real()
{
  double tmp;
  evaluate_sideeffects();
  tmp= last_value->val_real();
  null_value= last_value->null_value;
  return tmp;
}

my_decimal *Item_func_last_value::val_decimal(my_decimal *decimal_value)
{
  my_decimal *tmp;
  evaluate_sideeffects();
  tmp= last_value->val_decimal(decimal_value);
  null_value= last_value->null_value;
  return tmp;
}


bool Item_func_last_value::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  evaluate_sideeffects();
  bool tmp= last_value->get_date(thd, ltime, fuzzydate);
  null_value= last_value->null_value;
  return tmp;
}


bool Item_func_last_value::fix_length_and_dec(THD *thd)
{
  last_value=          args[arg_count -1];
  Type_std_attributes::set(last_value);
  set_maybe_null(last_value->maybe_null());
  return FALSE;
}


void Cursor_ref::print_func(String *str, const LEX_CSTRING &func_name)
{
  append_identifier(current_thd, str, &m_cursor_name);
  str->append(func_name);
}


sp_cursor *Cursor_ref::get_open_cursor_or_error()
{
  THD *thd= current_thd;
  sp_cursor *c= thd->spcont->get_cursor(m_cursor_offset);
  DBUG_ASSERT(c);
  if (!c/*safety*/ || !c->is_open())
  {
    my_message(ER_SP_CURSOR_NOT_OPEN, ER_THD(thd, ER_SP_CURSOR_NOT_OPEN),
               MYF(0));
    return NULL;
  }
  return c;
}


longlong Item_func_cursor_isopen::val_int()
{
  sp_cursor *c= current_thd->spcont->get_cursor(m_cursor_offset);
  DBUG_ASSERT(c != NULL);
  return c ? c->is_open() : 0;
}


longlong Item_func_cursor_found::val_int()
{
  sp_cursor *c= get_open_cursor_or_error();
  return !(null_value= (!c || c->fetch_count() == 0)) && c->found();
}


longlong Item_func_cursor_notfound::val_int()
{
  sp_cursor *c= get_open_cursor_or_error();
  return !(null_value= (!c || c->fetch_count() == 0)) && !c->found();
}


longlong Item_func_cursor_rowcount::val_int()
{
  sp_cursor *c= get_open_cursor_or_error();
  return !(null_value= !c) ? c->row_count() : 0;
}

/*****************************************************************************
  SEQUENCE functions
*****************************************************************************/

longlong Item_func_nextval::val_int()
{
  longlong value;
  int error;
  const char *key;
  uint length= get_table_def_key(table_list, &key);
  THD *thd;
  SEQUENCE_LAST_VALUE *entry;
  char buff[80];
  String key_buff(buff,sizeof(buff), &my_charset_bin);
  DBUG_ENTER("Item_func_nextval::val_int");
  update_table();
  DBUG_ASSERT(table && table->s->sequence);
  thd= table->in_use;

  if (thd->count_cuted_fields == CHECK_FIELD_EXPRESSION)
  {
    /* Alter table checking if function works */
    null_value= 0;
    DBUG_RETURN(0);
  }

  if (table->s->tmp_table != NO_TMP_TABLE)
  {
    /*
      Temporary tables has an extra \0 at end to distinguish it from
      normal tables
    */
    key_buff.copy(key, length, &my_charset_bin);
    key_buff.append((char) 0);
    key= key_buff.ptr();
    length++;
  }

  if (!(entry= ((SEQUENCE_LAST_VALUE*)
                my_hash_search(&thd->sequences, (uchar*) key, length))))
  {
    if (!(key= (char*) my_memdup(PSI_INSTRUMENT_ME, key, length, MYF(MY_WME))) ||
        !(entry= new SEQUENCE_LAST_VALUE((uchar*) key, length)))
    {
      /* EOM, error given */
      my_free((char*) key);
      delete entry;
      null_value= 1;
      DBUG_RETURN(0);
    }
    if (my_hash_insert(&thd->sequences, (uchar*) entry))
    {
      /* EOM, error given */
      delete entry;
      null_value= 1;
      DBUG_RETURN(0);
    }
  }
  entry->null_value= null_value= 0;
  value= table->s->sequence->next_value(table, 0, &error);
  entry->value= value;
  entry->set_version(table);

  if (unlikely(error))                          // Warning already printed
    entry->null_value= null_value= 1;           // For not strict mode
  DBUG_RETURN(value);
}


/* Print for nextval and lastval */

void Item_func_nextval::print(String *str, enum_query_type query_type)
{
  char d_name_buff[MAX_ALIAS_NAME], t_name_buff[MAX_ALIAS_NAME];
  LEX_CSTRING d_name= table_list->db;
  LEX_CSTRING t_name= table_list->table_name;
  bool use_db_name= d_name.str && d_name.str[0];
  THD *thd= current_thd;                        // Don't trust 'table'

  str->append(func_name_cstring());
  str->append('(');

  /*
    for next_val we assume that table_list has been updated to contain
    the current db.
  */

  if (lower_case_table_names > 0)
  {
    strmake(t_name_buff, t_name.str, MAX_ALIAS_NAME-1);
    t_name.length= my_casedn_str(files_charset_info, t_name_buff);
    t_name.str= t_name_buff;
    if (use_db_name)
    {
      strmake(d_name_buff, d_name.str, MAX_ALIAS_NAME-1);
      d_name.length= my_casedn_str(files_charset_info, d_name_buff);
      d_name.str= d_name_buff;
    }
  }

  if (use_db_name)
  {
    append_identifier(thd, str, &d_name);
    str->append('.');
  }
  append_identifier(thd, str, &t_name);
  str->append(')');
}


/* Return last used value for sequence or NULL if sequence hasn't been used */

longlong Item_func_lastval::val_int()
{
  const char *key;
  SEQUENCE_LAST_VALUE *entry;
  uint length= get_table_def_key(table_list, &key);
  THD *thd;
  char buff[80];
  String key_buff(buff,sizeof(buff), &my_charset_bin);
  DBUG_ENTER("Item_func_lastval::val_int");
  update_table();
  thd= table->in_use;

  if (table->s->tmp_table != NO_TMP_TABLE)
  {
    /*
      Temporary tables has an extra \0 at end to distinguish it from
      normal tables
    */
    key_buff.copy(key, length, &my_charset_bin);
    key_buff.append((char) 0);
    key= key_buff.ptr();
    length++;
  }

  if (!(entry= ((SEQUENCE_LAST_VALUE*)
                my_hash_search(&thd->sequences, (uchar*) key, length))))
  {
    /* Sequence not used */
    null_value= 1;
    DBUG_RETURN(0);
  }
  if (entry->check_version(table))
  {
    /* Table droped and re-created, remove current version */
    my_hash_delete(&thd->sequences, (uchar*) entry);
    null_value= 1;
    DBUG_RETURN(0);
  }    

  null_value= entry->null_value;
  DBUG_RETURN(entry->value);
}


/*
  Sets next value to be returned from sequences

  SELECT setval(foo, 42, 0);           Next nextval will return 43
  SELECT setval(foo, 42, 0, true);     Same as above
  SELECT setval(foo, 42, 0, false);    Next nextval will return 42
*/

longlong Item_func_setval::val_int()
{
  longlong value;
  int error;
  THD *thd;
  DBUG_ENTER("Item_func_setval::val_int");

  update_table();
  DBUG_ASSERT(table && table->s->sequence);
  thd= table->in_use;

  if (unlikely(thd->count_cuted_fields == CHECK_FIELD_EXPRESSION))
  {
    /* Alter table checking if function works */
    null_value= 0;
    DBUG_RETURN(0);
  }

  value= nextval;
  error= table->s->sequence->set_value(table, nextval, round, is_used);
  if (unlikely(error))
  {
    null_value= 1;
    value= 0;
  }
  DBUG_RETURN(value);
}


/* Print for setval */

void Item_func_setval::print(String *str, enum_query_type query_type)
{
  char d_name_buff[MAX_ALIAS_NAME], t_name_buff[MAX_ALIAS_NAME];
  LEX_CSTRING d_name= table_list->db;
  LEX_CSTRING t_name= table_list->table_name;
  bool use_db_name= d_name.str && d_name.str[0];
  THD *thd= current_thd;                        // Don't trust 'table'

  str->append(func_name_cstring());
  str->append('(');

  /*
    for next_val we assume that table_list has been updated to contain
    the current db.
  */

  if (lower_case_table_names > 0)
  {
    strmake(t_name_buff, t_name.str, MAX_ALIAS_NAME-1);
    t_name.length= my_casedn_str(files_charset_info, t_name_buff);
    t_name.str= t_name_buff;
    if (use_db_name)
    {
      strmake(d_name_buff, d_name.str, MAX_ALIAS_NAME-1);
      d_name.length= my_casedn_str(files_charset_info, d_name_buff);
      d_name.str= d_name_buff;
    }
  }

  if (use_db_name)
  {
    append_identifier(thd, str, &d_name);
    str->append('.');
  }
  append_identifier(thd, str, &t_name);
  str->append(',');
  str->append_longlong(nextval);
  str->append(',');
  str->append_longlong(is_used);
  str->append(',');
  str->append_ulonglong(round);
  str->append(')');
}


/*
  Return how many row combinations has accepted so far + 1

  The + 1 is to ensure that, for example, 'WHERE ROWNUM <=1' returns one row
*/

longlong Item_func_rownum::val_int()
{
  if (!accepted_rows)
  {
    /*
      Rownum is not properly set up. Probably used in wrong context when
      it should not be used. In this case returning 0 is probably the best
      solution.
    */
    return 0;
  }
  return (longlong) *accepted_rows+1;
}


Item_func_rownum::Item_func_rownum(THD *thd):
  Item_longlong_func(thd),accepted_rows(0)
{
  /*
    Remember the select context.
    Add the function to the list fix_after_optimize in the select context
    so that we can easily initializef all rownum functions with the pointers
    to the row counters.
  */
  select= thd->lex->current_select;
  select->fix_after_optimize.push_back(this, thd->mem_root);

  /*
    Mark that query is using rownum() and ensure that this select is
    not merged with other selects
  */
  select->with_rownum= 1;
  thd->lex->with_rownum= 1;
  thd->lex->uncacheable(UNCACHEABLE_RAND);
  with_flags= with_flags | item_with_t::ROWNUM_FUNC;

  /* If this command changes data, mark it as unsafe for statement logging */
  if (sql_command_flags[thd->lex->sql_command] &
      (CF_UPDATES_DATA | CF_DELETES_DATA))
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
}


/*
  Store a reference to the variable that contains number of accepted rows
*/

void Item_func_rownum::fix_after_optimize(THD *thd)
{
  accepted_rows= &select->join->accepted_rows;
}

/*
  Inform all ROWNUM() function where the number of rows are stored
*/

void fix_rownum_pointers(THD *thd, SELECT_LEX *select_lex, ha_rows *ptr)
{
  List_iterator<Item> li(select_lex->fix_after_optimize);
  while (Item *item= li++)
  {
    if (item->type() == Item::FUNC_ITEM &&
        ((Item_func*) item)->functype() == Item_func::ROWNUM_FUNC)
      ((Item_func_rownum*) item)->store_pointer_to_row_counter(ptr);
  }
}
