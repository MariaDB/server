#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation                          // gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include <m_ctype.h>
#include "sql_partition.h"
#include "sql_select.h"

#include "opt_trace.h"

static bool is_char_col(Item *item, CHARSET_INFO *coll)
{
  if (item->collation.collation == coll && item->type() == Item::FIELD_ITEM)
  {
    if (dynamic_cast<const Type_handler_longstr*>(item->type_handler()))
    {
      return true;
    }
  }
  return false;
}

static Item* is_convert_col_to_mb4(Item *item)
{
  Item_func_conv_charset *item_func;
  if (item->collation.collation == &my_charset_utf8mb4_general_ci && 
      (item_func= dynamic_cast<Item_func_conv_charset*>(item)))
  {
    Item *arg= item_func->arguments()[0];
    Item *arg_real= arg->real_item();
    if (is_char_col(arg_real, &my_charset_utf8mb3_general_ci))
      return arg;
  }
  return nullptr;
}


/*
  @brief
    Check if this is one of 

      CONVERT(tbl.mb3col USING utf8mb4_general_ci) = tbl2.mb4col
      CONVERT(tbl.mb3col USING utf8mb4_general_ci) = mb4expr
    
    where 
      mb3col's collation utf8mb3_general_ci,
      mb4col or mb4expr's collation is utf8mb4_general_ci

    and if this is true, make the rewrite.
    The first variant becomes

      CONVERT(mb3col USING utf8mb4_general_ci) = mb4col  -- the original
      AND
      mb3col= CONVERT_NARROW(mb4col)
   
    This allows construct ref access in both directions.a

    The second variant becomes
      
      mb3col = CONVERT_NARROW(mb4expr)

  @detail
    The pattern may occur on both sides of the equality.
*/

Item* Item_func_eq::utf8narrow_transformer(THD *thd, uchar *arg)
{
  if (cmp.compare_type() == STRING_RESULT &&
      cmp.compare_collation() == &my_charset_utf8mb4_general_ci)
  {
    Item *arg0= arguments()[0];
    Item *arg1= arguments()[1];
    bool do_rewrite= false;
    Item *mb3_col;
    Item *mb4_col= NULL;
    Item *mb4_val;

    // Try rewriting the left argument
    if ((mb3_col= is_convert_col_to_mb4(arg0)) && 
        arg1->collation.collation == &my_charset_utf8mb4_general_ci)
    {
      do_rewrite= true;
      if (is_char_col(arg1, &my_charset_utf8mb4_general_ci))
        mb4_col= arg1;
      else
        mb4_val= arg1;
    }
    else
    {
      // Same as above: Try rewriting the right argument
      if ((mb3_col= is_convert_col_to_mb4(arg1)) &&
           arg0->collation.collation == &my_charset_utf8mb4_general_ci)
      {
        do_rewrite= true;
        if (is_char_col(arg0, &my_charset_utf8mb4_general_ci))
          mb4_col= arg0;
        else
          mb4_val= arg0;
      }
    }

    if (do_rewrite)
    {
      // Common part:
      // Produce: mb3col= CONVERT_NARROW(mb4*)
      Item *narrow_arg= mb4_col? mb4_col : mb4_val;

      Item* narrowed= new (thd->mem_root) Item_func_conv_charset(thd, narrow_arg, 
                                  &my_charset_utf8mb3_general_ci,
                                  false, /* cache_if_const*/
                                  true /* do_narrowing */);
      Item *res= new (thd->mem_root) Item_func_eq(thd, mb3_col, narrowed);

      if (mb4_col)
      {
        // This is an extra condition, let the optimizer know it can be removed
        res->set_equivalent_extra();

        res= new (thd->mem_root) Item_cond_and(thd, this, res);
      }
      res->fix_fields(thd, &res);

      // TODO: trace_...(thd, this, res);
      return res;
    }
  }
  return this;
}

