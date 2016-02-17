#include "item_windowfunc.h" 
#include "my_dbug.h"
#include "my_global.h"
#include "sql_select.h" // test if group changed


bool
Item_window_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  
  /*
    TODO: why the last parameter is 'ref' in this call? What if window_func
    decides to substitute itself for something else and does *ref=.... ? 
    This will substitute *this (an Item_window_func object) with Item_sum
    object. Is this the intent?
  */
  if (window_func->fix_fields(thd, ref))
    return TRUE;

  max_length= window_func->max_length;

  fixed= 1;
  force_return_blank= true;
  read_value_from_result_field= false;
  return FALSE;
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
  for (ORDER *curr= window_spec->partition_list.first; curr; curr=curr->next)
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
  for (ORDER *curr= window_spec->order_list.first; curr; curr=curr->next)
  {
    Cached_item *tmp= new_Cached_item(thd, curr->item[0], TRUE);
    orderby_fields.push_back(tmp);
  }
  clear();
}

void Item_sum_dense_rank::setup_window_func(THD *thd, Window_spec *window_spec)
{
  /* TODO: consider moving this && Item_sum_rank's implementation */
  for (ORDER *curr= window_spec->order_list.first; curr; curr=curr->next)
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
