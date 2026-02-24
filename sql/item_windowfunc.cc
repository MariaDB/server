/*
   Copyright (c) 2016, 2020, MariaDB

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

#include "mariadb.h"
#include "item_windowfunc.h" 
#include "sql_select.h" // test if group changed


bool
Item_window_func::resolve_window_name(THD *thd)
{
  if (window_spec)
  {
    /* The window name has been already resolved */
    return false;
  }
  DBUG_ASSERT(window_name != NULL && window_spec == NULL);
  const char *ref_name= window_name->str;

  /* !TODO: Add the code to resolve ref_name in outer queries */ 
  /* 
    First look for the deinition of the window with 'window_name'
    in the current select
  */
  List<Window_spec> curr_window_specs= 
    List<Window_spec> (thd->lex->current_select->window_specs);
  List_iterator_fast<Window_spec> it(curr_window_specs);
  Window_spec *win_spec;
  while((win_spec= it++))
  {
    const char *win_spec_name= win_spec->name();
    if (win_spec_name &&
        my_strcasecmp(system_charset_info, ref_name, win_spec_name) == 0)
    {
      window_spec= win_spec;
      break;
    }
  }

  if (!window_spec)
  {
    my_error(ER_WRONG_WINDOW_SPEC_NAME, MYF(0), ref_name);
    return true;
  }

  return false;
}


void
Item_window_func::update_used_tables()
{
  used_tables_cache= 0;
  window_func()->update_used_tables();
  used_tables_cache|= window_func()->used_tables();
  for (ORDER *ord= window_spec->partition_list->first; ord; ord=ord->next)
  {
    Item *item= *ord->item;
    item->update_used_tables();
    used_tables_cache|= item->used_tables();
  }
  for (ORDER *ord= window_spec->order_list->first; ord; ord=ord->next)
  {
    Item *item= *ord->item;
    item->update_used_tables();
    used_tables_cache|= item->used_tables();
  }  
}


bool
Item_window_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);

  if (!thd->lex->current_select ||
      (thd->lex->current_select->context_analysis_place != SELECT_LIST &&
       thd->lex->current_select->context_analysis_place != IN_ORDER_BY))
  {
    my_error(ER_WRONG_PLACEMENT_OF_WINDOW_FUNCTION, MYF(0));
    return true;
  }

  if (window_name && resolve_window_name(thd))
    return true;
  
  if (window_spec->window_frame && is_frame_prohibited())
  {
    my_error(ER_NOT_ALLOWED_WINDOW_FRAME, MYF(0),
             window_func()->func_name());
    return true;
  }

  if (window_spec->order_list->elements == 0 && is_order_list_mandatory())
  {
    my_error(ER_NO_ORDER_LIST_IN_WINDOW_SPEC, MYF(0),
             window_func()->func_name());
    return true;
  }

  window_func()->mark_as_window_func_sum_expr();

  /*
    TODO: why the last parameter is 'ref' in this call? What if window_func
    decides to substitute itself for something else and does *ref=.... ? 
    This will substitute *this (an Item_window_func object) with Item_sum
    object. Is this the intent?
  */
  if (window_func()->fix_fields(thd, ref))
    return true;

  const_item_cache= false;

  with_flags= (with_flags & ~item_with_t::SUM_FUNC) | item_with_t::WINDOW_FUNC;

  if (fix_length_and_dec(thd))
    return TRUE;

  max_length= window_func()->max_length;
  set_maybe_null(window_func()->maybe_null());

  base_flags|= item_base_t::FIXED;
  set_phase_to_initial();
  return false;
}


/*
  @detail
    Window function evaluates its arguments when it is scanning the temporary
    table in partition/order-by order. That is, arguments should be read from
    the temporary table, not from the original base columns.

    In order for this to work, we need to call "split_sum_func" for each
    argument. The effect of the call is:
     1. the argument is added into ref_pointer_array. This will cause the
        argument to be saved in the temp.table
     2. argument item is replaced with an Item_ref object. this object refers
        the argument through the ref_pointer_array.

    then, change_to_use_tmp_fields() will replace ref_pointer_array with an
    array that points to the temp.table fields.
    This way, when window_func attempts to evaluate its arguments, it will use
    Item_ref objects which will read data from the temp.table.

    Note: Before window functions, aggregate functions never needed to do such
    transformations on their arguments. This is because grouping operation
    does not need to read from the temp.table.
    (Q: what happens when we first sort and then do grouping in a
      group-after-group mode? dont group by items read from temp.table, then?)
*/

void Item_window_func::split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                                      List<Item> &fields, uint flags)
{
  for (uint i=0; i < window_func()->argument_count(); i++)
  {
    Item **p_item= &window_func()->arguments()[i];
    (*p_item)->split_sum_func2(thd, ref_pointer_array, fields, p_item, flags);
  }
  window_func()->setup_caches(thd);
}

bool Item_window_func::check_result_type_of_order_item()
{
  switch (window_func()->sum_func()) {
  case Item_sum::PERCENTILE_CONT_FUNC:
  {
    Item_result rtype= window_spec->order_list->first->item[0]->cmp_type();
    // TODO (varun) : support date type in percentile_cont function
    if (rtype != REAL_RESULT && rtype != INT_RESULT &&
        rtype != DECIMAL_RESULT && rtype != TIME_RESULT)
    {
      my_error(ER_WRONG_TYPE_FOR_PERCENTILE_FUNC, MYF(0),
               window_func()->func_name());
      return true;
    }
    return false;
  }
  case Item_sum::PERCENTILE_DISC_FUNC:
  {
    Item *src_item= window_spec->order_list->first->item[0];
    Item_sum_percentile_disc *func=
      static_cast<Item_sum_percentile_disc*>(window_func());
    func->set_handler(src_item->type_handler());
    func->Type_std_attributes::set(src_item);
    Type_std_attributes::set(src_item);
    return false;
  }
  default:
    break;
  }
  return FALSE;
}

/*
  This must be called before attempting to compute the window function values.
  @detail
    If we attempt to do it in fix_fields(), partition_fields will refer
    to the original window function arguments.
    We need it to refer to temp.table columns.
*/

void Item_sum_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: move this into Item_window_func? */
  peer_tracker = new Group_bound_tracker(thd, window_spec->order_list);
  peer_tracker->init();
  clear();
}

void Item_sum_dense_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: consider moving this && Item_sum_rank's implementation */
  peer_tracker = new Group_bound_tracker(thd, window_spec->order_list);
  peer_tracker->init();
  clear();
}

void Item_sum_percentile_disc::setup_window_func(THD *thd, Window_spec *window_spec)
{
  order_item= window_spec->order_list->first->item[0];
  if (!(value= order_item->get_cache(thd)))
    return;
  value->setup(thd, order_item);
  value->store(order_item);
}

void Item_sum_percentile_cont::setup_window_func(THD *thd, Window_spec *window_spec)
{
  order_item= window_spec->order_list->first->item[0];
  /* TODO(varun): need to discuss and finalise what type should we
     return for percentile cont functions
  */
  if (!(ceil_value= order_item->get_cache(thd)))
    return;
  ceil_value->setup(thd, order_item);
  ceil_value->store(order_item);

  if (!(floor_value= order_item->get_cache(thd)))
    return;
  floor_value->setup(thd, order_item);
  floor_value->store(order_item);
}
bool Item_sum_percentile_cont::fix_fields(THD *thd, Item **ref)
{
  bool res;
  res= Item_sum_num::fix_fields(thd, ref);
  if (res)
    return res;

  switch(args[0]->cmp_type())
  {
    case DECIMAL_RESULT:
    case REAL_RESULT:
    case INT_RESULT:
      break;
    default:
      my_error(ER_WRONG_TYPE_OF_ARGUMENT, MYF(0), func_name());
      return TRUE;
  }
  return res;
}
bool Item_sum_percentile_disc::fix_fields(THD *thd, Item **ref)
{
  bool res;
  res= Item_sum_num::fix_fields(thd, ref);
  if (res)
    return res;

  switch(args[0]->cmp_type())
  {
    case DECIMAL_RESULT:
    case REAL_RESULT:
    case INT_RESULT:
      break;
    default:
      my_error(ER_WRONG_TYPE_OF_ARGUMENT, MYF(0), func_name());
      return TRUE;
  }
  return res;

}

bool Item_sum_dense_rank::add()
{
  if (peer_tracker->check_if_next_group() || first_add)
  {
    first_add= false;
    dense_rank++;
  }

  return false;
}


bool Item_sum_rank::add()
{
  row_number++;
  if (peer_tracker->check_if_next_group())
  {
    /* Row value changed */
    cur_rank= row_number;
  }
  return false; 
}

bool Item_sum_percent_rank::add()
{
  row_number++;
  if (peer_tracker->check_if_next_group())
  {
    /* Row value changed. */
    cur_rank= row_number;
  }
  return false;
}

void Item_sum_percent_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: move this into Item_window_func? */
  peer_tracker = new Group_bound_tracker(thd, window_spec->order_list);
  peer_tracker->init();
  clear();
}


bool Item_sum_hybrid_simple::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);

  if (init_sum_func_check(thd))
    return TRUE;

  for (uint i= 0; i < arg_count; i++)
  {
    if (args[i]->fix_fields_if_needed_for_scalar(thd, &args[i]))
      return TRUE;
    with_flags|= args[i]->with_flags;
  }

  if (fix_length_and_dec(thd))
    return TRUE;

  setup_hybrid(thd, args[0]);
  result_field=0;

  if (check_sum_func(thd, ref))
    return TRUE;
  for (uint i= 0; i < arg_count; i++)
    orig_args[i]= args[i];

  base_flags|= item_base_t::FIXED;
  return FALSE;
}


bool Item_sum_hybrid_simple::fix_length_and_dec(THD *thd)
{
  set_maybe_null();
  null_value= true;
  return args[0]->type_handler()->Item_sum_hybrid_fix_length_and_dec(this);
}


bool Item_sum_hybrid_simple::add()
{
  value->store(args[0]);
  value->cache_value();
  null_value= value->null_value;
  return false;
}

void Item_sum_hybrid_simple::setup_hybrid(THD *thd, Item *item)
{
  if (!(value= item->get_cache(thd)))
    return;
  value->setup(thd, item);
  value->store(item);
  if (!item->const_item())
    value->set_used_tables(RAND_TABLE_BIT);
  collation.set(item->collation);
}

double Item_sum_hybrid_simple::val_real()
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return 0.0;
  double retval= value->val_real();
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == 0.0);
  return retval;
}

longlong Item_sum_hybrid_simple::val_int()
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return 0;
  longlong retval= value->val_int();
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == 0);
  return retval;
}

my_decimal *Item_sum_hybrid_simple::val_decimal(my_decimal *val)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return 0;
  my_decimal *retval= value->val_decimal(val);
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == NULL);
  return retval;
}

String *
Item_sum_hybrid_simple::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return 0;
  String *retval= value->val_str(str);
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == NULL);
  return retval;
}

bool Item_sum_hybrid_simple::val_native(THD *thd, Native *to)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return true;
  return val_native_from_item(thd, value, to);
}

bool Item_sum_hybrid_simple::get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
{
  DBUG_ASSERT(fixed());
  if (null_value)
    return true;
  bool retval= value->get_date(thd, ltime, fuzzydate);
  if ((null_value= value->null_value))
    DBUG_ASSERT(retval == true);
  return retval;
}

Field *Item_sum_hybrid_simple::create_tmp_field(MEM_ROOT *root,
                                                bool group, TABLE *table)
{
  DBUG_ASSERT(0);
  return NULL;
}

void Item_sum_hybrid_simple::reset_field()
{
  switch(result_type()) {
  case STRING_RESULT:
  {
    char buff[MAX_FIELD_WIDTH];
    String tmp(buff,sizeof(buff),result_field->charset()),*res;

    res=args[0]->val_str(&tmp);
    if (args[0]->null_value)
    {
      result_field->set_null();
      result_field->reset();
    }
    else
    {
      result_field->set_notnull();
      result_field->store(res->ptr(),res->length(),tmp.charset());
    }
    break;
  }
  case INT_RESULT:
  {
    longlong nr=args[0]->val_int();

    if (maybe_null())
    {
      if (args[0]->null_value)
      {
	nr=0;
	result_field->set_null();
      }
      else
	result_field->set_notnull();
    }
    result_field->store(nr, unsigned_flag);
    break;
  }
  case REAL_RESULT:
  {
    double nr= args[0]->val_real();

    if (maybe_null())
    {
      if (args[0]->null_value)
      {
	nr=0.0;
	result_field->set_null();
      }
      else
	result_field->set_notnull();
    }
    result_field->store(nr);
    break;
  }
  case DECIMAL_RESULT:
  {
    VDec arg_dec(args[0]);

    if (maybe_null())
    {
      if (arg_dec.is_null())
        result_field->set_null();
      else
        result_field->set_notnull();
    }
    /*
      We must store zero in the field as we will use the field value in
      add()
    */
    result_field->store_decimal(arg_dec.ptr_or(&decimal_zero));
    break;
  }
  case ROW_RESULT:
  case TIME_RESULT:
    DBUG_ASSERT(0);
  }
}

void Item_sum_hybrid_simple::update_field()
{
  DBUG_ASSERT(0);
}

void Item_window_func::print(String *str, enum_query_type query_type)
{
  if (only_single_element_order_list())
  {
    print_for_percentile_functions(str, query_type);
    return;
  }
  window_func()->print(str, query_type);
  str->append(STRING_WITH_LEN(" over "));
  if (!window_spec)
    str->append(window_name);
  else
    window_spec->print(str, query_type);
}
void Item_window_func::print_for_percentile_functions(String *str, enum_query_type query_type)
{
  window_func()->print(str, query_type);
  str->append(STRING_WITH_LEN(" within group "));
  str->append('(');
  window_spec->print_order(str,query_type);
  str->append(')');
  str->append(STRING_WITH_LEN(" over "));
  str->append('(');
  window_spec->print_partition(str,query_type);
  str->append(')');
}
