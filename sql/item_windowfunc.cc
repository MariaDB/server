#include "item_windowfunc.h" 
#include "my_dbug.h"
#include "my_global.h"
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
  char *ref_name= window_name->str;

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
    char *win_spec_name= win_spec->name();
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
  DBUG_ASSERT(fixed == 0);

  enum_parsing_place place= thd->lex->current_select->parsing_place;

  if (!(place == SELECT_LIST || place == IN_ORDER_BY))
  {
    my_error(ER_WRONG_PLACEMENT_OF_WINDOW_FUNCTION, MYF(0));
    return true;
  }

  if (window_name && resolve_window_name(thd))
    return true;
  
  if (window_spec->window_frame && is_frame_prohibited())
  {
    my_error(ER_NOT_ALLOWED_WINDOW_FRAME, MYF(0), window_func()->func_name());
    return true;
  }

  if (window_spec->order_list->elements == 0 && is_order_list_mandatory())
  {
    my_error(ER_NO_ORDER_LIST_IN_WINDOW_SPEC, MYF(0), window_func()->func_name());
    return true;
  }
  /*
    TODO: why the last parameter is 'ref' in this call? What if window_func
    decides to substitute itself for something else and does *ref=.... ? 
    This will substitute *this (an Item_window_func object) with Item_sum
    object. Is this the intent?
  */
  if (window_func()->fix_fields(thd, ref))
    return true;

  const_item_cache= false;
  with_window_func= true;
  with_sum_func= false;

  fix_length_and_dec();

  max_length= window_func()->max_length;
  maybe_null= window_func()->maybe_null;

  fixed= 1;
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

bool Item_sum_first_value::add()
{
  if (value_added)
    return false;

  /* TODO(cvicentiu) This is done like this due to how Item_sum_hybrid works.
     For this usecase we can actually get rid of arg_cache. arg_cache is just
     for running a comparison function. */
  value_added= true;
  arg_cache->cache_value();
  value->store(arg_cache);
  null_value= arg_cache->null_value;
  return false;
}

bool Item_sum_last_value::add()
{
  arg_cache->cache_value();
  value->store(arg_cache);
  null_value= arg_cache->null_value;
  return false;
}
