#include "item_windowfunc.h" 

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

  fixed= 1;
  return FALSE;
}
