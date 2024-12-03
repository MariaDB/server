/* Copyright (c) 2023, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include <m_ctype.h>
#include "sql_partition.h"
#include "sql_select.h"

#include "opt_trace.h"

/*
  @brief
    Check if passed item is "UCASE(table.colX)" where colX is either covered
    by some index or is a part of partition expression.

  @return
     Argument of the UCASE if passed item matches
     NULL otherwise.
*/

static Item* is_upper_key_col(Item *item)
{
  Item_func_ucase *item_func;
  if ((item_func= dynamic_cast<Item_func_ucase*>(item)))
  {
    Item *arg= item_func->arguments()[0];
    Item *arg_real= arg->real_item();
    if (arg_real->type() == Item::FIELD_ITEM)
    {
      if (dynamic_cast<const Type_handler_longstr*>(arg_real->type_handler()))
      {
        Field *field= ((Item_field*)arg_real)->field;
        bool appl= (field->flags & PART_KEY_FLAG);

#ifdef WITH_PARTITION_STORAGE_ENGINE
        partition_info *part_info;
        if (!appl && ((part_info= field->table->part_info)))
        {
          appl= bitmap_is_set(&part_info->full_part_field_set,
                              field->field_index);
        }
#endif
        if (appl)
        {
          /*
            Make sure COERCIBILITY(UPPER(col))=COERCIBILITY(col)
          */
          DBUG_ASSERT(arg->collation.derivation ==
                      item_func->collation.derivation);

          /* Return arg, not arg_real. Do not walk into Item_ref objects */
          return arg;
        }
      }
    }
  }
  return nullptr;
}


static void trace_upper_removal_rewrite(THD *thd, Item *old_item, Item *new_item)
{
  Json_writer_object trace_wrapper(thd);
  Json_writer_object obj(thd, "sargable_casefold_removal");
  obj.add("before", old_item)
     .add("after", new_item);
}


/*
  @brief
    Rewrite UPPER(key_varchar_col) = expr into key_varchar_col=expr

  @detail
    UPPER() may occur on both sides of the equality.
    UCASE() is a synonym of UPPER() so we handle it, too.
*/

Item* Item_func_eq::varchar_upper_cmp_transformer(THD *thd, uchar *arg)
{
  if (cmp.compare_type() == STRING_RESULT &&
      cmp.compare_collation()->state & MY_CS_UPPER_EQUAL_AS_EQUAL)
  {
    Item *arg0= arguments()[0];
    Item *arg1= arguments()[1];
    bool do_rewrite= false;
    Item *tmp;

    // Try rewriting the left argument
    if ((tmp= is_upper_key_col(arguments()[0])))
    {
      arg0= tmp;
      do_rewrite= true;
    }

    // Try rewriting the right argument
    if ((tmp= is_upper_key_col(arguments()[1])))
    {
      arg1=tmp;
      do_rewrite= true;
    }

    if (do_rewrite)
    {
      Item *res= new (thd) Item_func_eq(thd, arg0, arg1);
      if (res && !res->fix_fields(thd, &res))
      {
        trace_upper_removal_rewrite(thd, this, res);
        return res;
      }
    }
  }
  return this;
}


/*
  @brief
    Rewrite "UPPER(key_col) IN (const-list)" into "key_col IN (const-list)"
*/

Item* Item_func_in::varchar_upper_cmp_transformer(THD *thd, uchar *arg)
{
  if (arg_types_compatible &&
      m_comparator.cmp_type() == STRING_RESULT &&
      cmp_collation.collation->state & MY_CS_UPPER_EQUAL_AS_EQUAL &&
      all_items_are_consts(args + 1, arg_count - 1))
  {
    Item *arg0= arguments()[0];
    Item *tmp;
    if ((tmp= is_upper_key_col(arg0)))
    {
      Item_func_in *cl= (Item_func_in*)build_clone(thd);
      Item *res;
      cl->arguments()[0]= tmp;
      cl->walk(&Item::cleanup_excluding_const_fields_processor, 0, 0);
      res= cl;
      if (res->fix_fields(thd, &res))
        return this;
      trace_upper_removal_rewrite(thd, this, res);
      return res;
    }
  }
  return this;
}

