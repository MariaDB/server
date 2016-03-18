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


bool
Item_window_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);

  if (window_name && resolve_window_name(thd))
    return true;
  
  if (window_spec->window_frame && is_frame_prohibited())
  {
    my_error(ER_NOT_ALLOWED_WINDOW_FRAME, MYF(0), window_func->func_name());
    return true;
  }

  if (window_spec->order_list->elements == 0 && is_order_list_mandatory())
  {
    my_error(ER_NO_ORDER_LIST_IN_WINDOW_SPEC, MYF(0), window_func->func_name());
    return true;
  }
  /*
    TODO: why the last parameter is 'ref' in this call? What if window_func
    decides to substitute itself for something else and does *ref=.... ? 
    This will substitute *this (an Item_window_func object) with Item_sum
    object. Is this the intent?
  */
  if (window_func->fix_fields(thd, ref))
    return true;

  fix_length_and_dec();

  max_length= window_func->max_length;
  maybe_null= window_func->maybe_null;

  fixed= 1;
  force_return_blank= true;
  read_value_from_result_field= false;
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
  for (uint i=0; i < window_func->argument_count(); i++)
  {
    Item **p_item= &window_func->arguments()[i];
    (*p_item)->split_sum_func2(thd, ref_pointer_array, fields, p_item, flags);
  }
}


/*
  This must be called before advance_window() can be called.

  @detail
    If we attempt to do it in fix_fields(), partition_fields will refer
    to the original window function arguments.
    We need it to refer to temp.table columns.
*/

void Item_window_func::setup_partition_border_check(THD *thd)
{
  for (ORDER *curr= window_spec->partition_list->first; curr; curr=curr->next)
  {
    //curr->item_ptr->fix_fields(thd, curr->item);
    Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);  
    partition_fields.push_back(tmp);
  }
  window_func->setup_window_func(thd, window_spec);
}


void Item_sum_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: move this into Item_window_func? */
  for (ORDER *curr= window_spec->order_list->first; curr; curr=curr->next)
  {
    Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);
    orderby_fields.push_back(tmp);
  }
  clear();
}

void Item_sum_dense_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: consider moving this && Item_sum_rank's implementation */
  for (ORDER *curr= window_spec->order_list->first; curr; curr=curr->next)
  {
    Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);
    orderby_fields.push_back(tmp);
  }
  clear();
}

bool Item_sum_dense_rank::add()
{
  if (test_if_group_changed(orderby_fields) > -1)
   dense_rank++;

  return false;
}


bool Item_sum_rank::add()
{
  row_number++;
  if (test_if_group_changed(orderby_fields) > -1)
  {
    /* Row value changed */
    cur_rank= row_number;
  }
  return false; 
}

int Item_window_func::check_partition_bound()
{
  return test_if_group_changed(partition_fields);
}

void Item_window_func::advance_window()
{
  int changed= check_partition_bound();

  if (changed > -1)
  {
    /* Next partition */
    window_func->clear();
  }
  window_func->add();
}

bool Item_sum_percent_rank::add()
{
  row_number++;
  if (test_if_group_changed(orderby_fields) > -1)
  {
    /* Row value changed. */
    cur_rank= row_number;
  }
  return false;
}

void Item_sum_percent_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: move this into Item_window_func? */
  for (ORDER *curr= window_spec->order_list->first; curr; curr=curr->next)
  {
    Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);
    orderby_fields.push_back(tmp);
  }
  clear();
}


