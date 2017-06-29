#include "sql_list.h"
#include "sql_tvc.h"
#include "sql_class.h"

/**
  The method searches types of columns for temporary table where values from TVC will be stored
*/

bool join_type_handlers_for_tvc(List_iterator_fast<List_item> &li, 
			        Type_holder *holders, uint cnt)
{
  List_item *lst;
  li.rewind();
  bool first= true;
  
  while ((lst=li++))
  {
    List_iterator_fast<Item> it(*lst);
    Item *item;
  
    if (cnt != lst->elements)
    {
      /*error wrong number of values*/
      return true;
    }
    for (uint pos= 0; (item=it++); pos++)
    {
      const Type_handler *item_type_handler= item->real_type_handler();
      if (first)
        holders[pos].set_handler(item_type_handler);
      else if (holders[pos].aggregate_for_result(item_type_handler))
      {
        /*error ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION*/
        return true;
      }
    }
    first= false;
  }
  return false;
}

/**
  The method searches names of columns for temporary table where values from TVC will be stored
*/

bool get_type_attributes_for_tvc(THD *thd_arg,
			         List_iterator_fast<List_item> &li, 
                                 Type_holder *holders, uint count)
{
  List_item *lst;
  li.rewind();
  
  lst= li++;
  uint first_list_el_count= lst->elements;
  
  for (uint pos= 0; pos < first_list_el_count; pos++)
  {
    if (holders[pos].alloc_arguments(thd_arg, count))
      return true;
  }
  
  List_iterator_fast<Item> it(*lst);
  Item *item;
  
  for (uint holder_pos= 0 ; (item= it++); holder_pos++)
  {
    DBUG_ASSERT(item->fixed);
    holders[holder_pos].add_argument(item);
  }
  
  for (uint pos= 0; pos < first_list_el_count; pos++)
  {
    if (holders[pos].aggregate_attributes(thd_arg))
      return true;
  }
  return false;
}

bool table_value_constr::prepare(THD *thd_arg, SELECT_LEX *sl, select_result *tmp_result)
{
  List_iterator_fast<List_item> li(lists_of_values);
  
  List_item *first_elem= li++;
  uint cnt= first_elem->elements;
  Type_holder *holders;
  
  if (!(holders= new (thd_arg->mem_root)
                Type_holder[cnt]) || 
       join_type_handlers_for_tvc(li, holders, cnt) ||
       get_type_attributes_for_tvc(thd_arg, li, holders, cnt))
    return true;
  
  List_iterator_fast<Item> it(*first_elem);
  Item *item;
  
  sl->item_list.empty();
  for (uint pos= 0; (item= it++); pos++)
  {
    /* Error's in 'new' will be detected after loop */
    Item_type_holder *new_holder= new (thd_arg->mem_root)
                      Item_type_holder(thd_arg,
                                       &item->name,
                                       holders[pos].type_handler(),
                                       &holders[pos]/*Type_all_attributes*/,
                                       holders[pos].get_maybe_null());
    new_holder->fix_fields(thd_arg, 0);
    sl->item_list.push_back(new_holder);
  }
  
  if (thd_arg->is_fatal_error)
    return true; // out of memory
    
  result= tmp_result;
  
  return false;
}

bool table_value_constr::exec()
{
  List_iterator_fast<List_item> li(lists_of_values);
  List_item *elem;
  
  while ((elem=li++))
  {
    result->send_data(*elem);
  }
  return false;
}