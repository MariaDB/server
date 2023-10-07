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

void Item_container::illegal_method_call(const char *method)
{
  DBUG_ENTER("Item_row::illegal_method_call");
  DBUG_PRINT("error", ("!!! %s method was called for row item", method));
  DBUG_ASSERT(0);
  my_error(ER_OPERAND_COLUMNS, MYF(0), 1);
  DBUG_VOID_RETURN;
}

bool Item_container::fix_fields(THD *thd, Item **ref)
{
  DBUG_ASSERT(fixed() == 0);
  null_value= 0;
  base_flags&= ~item_base_t::MAYBE_NULL;

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
    base_flags|= (item->base_flags & item_base_t::MAYBE_NULL);
    with_flags|= item->with_flags;
  }
  base_flags|= item_base_t::FIXED;
  return FALSE;
}


bool
Item_container::eval_not_null_tables(void *opt_arg)
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
Item_container::find_not_null_fields(table_map allowed)
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


void Item_container::cleanup()
{
  DBUG_ENTER("Item_container::cleanup");

  Item_fixed_hybrid::cleanup();
  /* Reset to the original values */
  used_tables_and_const_cache_init();
  with_null= 0;

  DBUG_VOID_RETURN;
}


void Item_container::split_sum_func(THD *thd, Ref_ptr_array ref_pointer_array,
                              List<Item> &fields, uint flags)
{
  Item **arg, **arg_end;
  for (arg= args, arg_end= args + arg_count; arg != arg_end ; arg++)
    (*arg)->split_sum_func2(thd, ref_pointer_array, fields, arg,
                            flags | SPLIT_SUM_SKIP_REGISTERED);
}


void Item_container::fix_after_pullout(st_select_lex *new_parent, Item **ref,
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


bool Item_container::check_cols(uint c)
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


void Item_array::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("ARRAY["));
  for (uint i= 0; i < arg_count; i++)
  {
    if (i)
      str->append(',');
    args[i]->print(str, query_type);
  }
  str->append(']');
}


Item *Item_container::transform(THD *thd, Item_transformer transformer, uchar *arg)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  if (transform_args(thd, transformer, arg))
    return 0;
  return (this->*transformer)(thd, arg);
}

void Item_container::bring_value()
{
  for (uint i= 0; i < arg_count; i++)
    args[i]->bring_value();
}


/*
  See comments in Item_func_or_sum::build_clone()
*/
Item* Item_container::build_clone(THD *thd)
{
  Item_args new_args;
  if (new_args.clone_arguments(thd, *this))
    return 0;
  Item_container *copy= static_cast<Item_container *>(get_copy(thd));
  if (unlikely(!copy))
    return 0;
  copy->Item_args::operator=(new_args);
  return copy;
}
