/*
   Copyright (c) 2002, 2011, Oracle and/or its affiliates.
   Copyright (c) 2011, 2020, MariaDB Corporation.

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
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // THD, set_var.h: THD
#include "set_var.h"

void Item_row::illegal_method_call(const char *method)
{
  DBUG_ENTER("Item_row::illegal_method_call");
  DBUG_PRINT("error", ("!!! %s method was called for row item", method));
  DBUG_ASSERT(0);
  my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
  DBUG_VOID_RETURN;
}

bool Item_row::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed == 0);
  null_value= 0;
  maybe_null= 0;
  Item **arg, **arg_end;
  for (arg= args, arg_end= args + arg_count; arg != arg_end ; arg++)
  {
    if ((*arg)->fix_fields_if_needed(thd, arg))
      return TRUE;
    // we can't assign 'item' before, because fix_fields() can change arg
    Item *item= *arg;
    used_tables_cache |= item->used_tables();
    const_item_cache&= item->const_item() && !with_null;
    not_null_tables_cache|= item->not_null_tables();

    if (const_item_cache)
    {
      if (item->cols() > 1)
	with_null|= item->null_inside();
      else
      {
	if (item->is_null())
          with_null|= 1;
      }
    }
    maybe_null|= item->maybe_null;
    with_sum_func= with_sum_func || item->with_sum_func;
    with_window_func = with_window_func || item->with_window_func;
    with_field= with_field || item->with_field;
    with_subquery|= item->with_subquery;
    with_param|= item->with_param;
  }
  fixed= 1;
  return FALSE;
}


bool
Item_row::eval_not_null_tables(void *opt_arg)
{
  Item **arg,**arg_end;
  not_null_tables_cache= 0;
  if (arg_count)
  {		
    for (arg= args, arg_end= args + arg_count; arg != arg_end ; arg++)
    {
      not_null_tables_cache|= (*arg)->not_null_tables();
    }
  }
  return FALSE;
}


bool
Item_row::find_not_null_fields(table_map allowed)
{
  if (~allowed & used_tables())
    return false;

  Item **arg,**arg_end;
  if (arg_count)
  {
    for (arg= args, arg_end= args + arg_count; arg != arg_end ; arg++)
    {
      if (!(*arg)->find_not_null_fields(allowed))
        continue;
    }
  }
  return false;
}


void Item_row::cleanup()
{
  DBUG_ENTER("Item_row::cleanup");

  Item_fixed_hybrid::cleanup();
  /* Reset to the original values */
  used_tables_and_const_cache_init();
  with_null= 0;

  DBUG_VOID_RETURN;
}


void Item_row::split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags)
{
  Item **arg, **arg_end;
  for (arg= args, arg_end= args + arg_count; arg != arg_end ; arg++)
    (*arg)->split_sum_func2(thd, ref_pointer_array, fields, arg,
                            flags | SPLIT_SUM_SKIP_REGISTERED);
}


void Item_row::fix_after_pullout(st_select_lex *new_parent, Item **ref,
                                 bool merge)
{
  used_tables_and_const_cache_init();
  not_null_tables_cache= 0;
  for (uint i= 0; i < arg_count; i++)
  {
    args[i]->fix_after_pullout(new_parent, &args[i], merge);
    used_tables_and_const_cache_join(args[i]);
    not_null_tables_cache|= args[i]->not_null_tables();
  }
}


bool Item_row::check_cols(uint c)
{
  if (c != arg_count)
  {
    my_error(ER_OPERAND_COLUMNS, MYF(0), c);
    return 1;
  }
  return 0;
}

void Item_row::print(String *str, enum_query_type query_type)
{
  str->append('(');
  for (uint i= 0; i < arg_count; i++)
  {
    if (i)
      str->append(',');
    args[i]->print(str, query_type);
  }
  str->append(')');
}


Item *Item_row::transform(THD *thd, Item_transformer transformer, uchar *arg)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  if (transform_args(thd, transformer, arg))
    return 0;
  return (this->*transformer)(thd, arg);
}

void Item_row::bring_value()
{
  for (uint i= 0; i < arg_count; i++)
    args[i]->bring_value();
}


Item* Item_row::build_clone(THD *thd)
{
  Item **copy_args= static_cast<Item**>
    (alloc_root(thd->mem_root, sizeof(Item*) * arg_count));
  if (unlikely(!copy_args))
    return 0;
  for (uint i= 0; i < arg_count; i++)
  {
    Item *arg_clone= args[i]->build_clone(thd);
    if (!arg_clone)
      return 0;
    copy_args[i]= arg_clone;
  }
  Item_row *copy= (Item_row *) get_copy(thd);
  if (unlikely(!copy))
    return 0;
  copy->args= copy_args;
  return copy;
}
