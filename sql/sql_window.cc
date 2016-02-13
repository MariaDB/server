#include "sql_select.h"
#include "sql_window.h"


bool
Window_spec::check_window_names(List_iterator_fast<Window_spec> &it)
{
  char *name= this->name();
  char *ref_name= window_reference();
  bool win_ref_is_resolved= false;
  it.rewind();
  Window_spec *win_spec;
  while((win_spec= it++) && win_spec != this)
  {
    char *win_spec_name= win_spec->name();
    if (win_spec_name)
    {
      if (name && my_strcasecmp(system_charset_info, name, win_spec_name) == 0)
      {
        my_error(ER_DUP_WINDOW_NAME, MYF(0), name);
        return true;
      }
      if (ref_name &&
          my_strcasecmp(system_charset_info, ref_name, win_spec_name) == 0)
      {
        if (win_spec->partition_list.elements)
	{
          my_error(ER_PARTITION_LIST_IN_REFERENCING_WINDOW_SPEC, MYF(0),
                   ref_name);
          return true;
        }
        if (win_spec->order_list.elements && order_list.elements)
	{
          my_error(ER_ORDER_LIST_IN_REFERENCING_WINDOW_SPEC, MYF(0), ref_name);
          return true;              
        } 
        if (win_spec->window_frame) 
	{
          my_error(ER_WINDOW_FRAME_IN_REFERENCED_WINDOW_SPEC, MYF(0), ref_name);
          return true;              
        }
        referenced_win_spec=win_spec; 
        win_ref_is_resolved= true; 
      }
    }
  }
  if (ref_name && !win_ref_is_resolved)
  {
    my_error(ER_WRONG_WINDOW_SPEC_NAME, MYF(0), ref_name);
    return true;              
  }
  return false;
}


int
setup_windows(THD *thd, Ref_ptr_array ref_pointer_array, TABLE_LIST *tables,
	      List<Item> &fields, List<Item> &all_fields, 
              List<Window_spec> win_specs)
{
  Window_spec *win_spec;
  DBUG_ENTER("setup_windows");
  List_iterator<Window_spec> it(win_specs);
  List_iterator_fast<Window_spec> itp(win_specs);
    
  while ((win_spec= it++))
  {
    bool hidden_group_fields;
    if (win_spec->check_window_names(itp) ||
        setup_group(thd, ref_pointer_array, tables, fields, all_fields,
                    win_spec->partition_list.first, &hidden_group_fields) ||
        setup_order(thd, ref_pointer_array, tables, fields, all_fields,
                    win_spec->order_list.first))
    {
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}
