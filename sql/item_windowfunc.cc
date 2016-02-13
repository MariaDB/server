#include "item_windowfunc.h" 

bool
Item_window_func::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  
  if (window_func->fix_fields(thd, ref))
    return TRUE;

  fixed= 1;
  return FALSE;
}
