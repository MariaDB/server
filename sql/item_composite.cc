#include "my_global.h"
#include "item.h"
#include "item_composite.h"

void Item_composite::illegal_method_call(const char *method)
{
  DBUG_ENTER("Item_composite::illegal_method_call");
  DBUG_PRINT("error", ("!!! %s method was called for %s",
                       method, composite_name()));
  DBUG_ASSERT(0);
  my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
  DBUG_VOID_RETURN;
}
