/*
   Copyights (c) 2025, Rakuten Securities
   Copyright (c) 2025, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#include "sql_type.h"
#include "sql_type_composite.h"
#include "item.h"
#include "item_cmpfunc.h"

#include "sql_type_assoc_array.h"

const Name & Type_handler_composite::default_value() const
{
  DBUG_ASSERT(0);
  static Name def(STRING_WITH_LEN(""));
  return def;
}


bool Type_handler_composite::
       Column_definition_prepare_stage1(THD *thd,
                                        MEM_ROOT *mem_root,
                                        Column_definition *def,
                                        column_definition_type_t type,
                                        const Column_derived_attributes
                                              *derived_attr)
                                        const
{
  def->charset= &my_charset_bin;
  def->create_length_to_internal_length_null();
  return false;
}


bool Type_handler_composite::Item_eq_value(THD *thd,
                                           const Type_cmp_attributes *attr,
                                           Item *a, Item *b) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_composite::
       Item_save_in_value(THD *thd, Item *item, st_value *value) const
{
  DBUG_ASSERT(0);
  value->m_type= DYN_COL_NULL;
  return true;
}


bool Type_handler_composite::
  Item_param_set_from_value(THD *thd,
                            Item_param *param,
                            const Type_all_attributes *attr,
                            const st_value *val) const
{
  DBUG_ASSERT(0);
  param->set_null();
  return true;
}


void Type_handler_composite::Item_update_null_value(Item *item) const
{
  DBUG_ASSERT(0);
  item->null_value= true;
}


longlong Type_handler_composite::
           Item_func_between_val_int(Item_func_between *func) const
{
  DBUG_ASSERT(0);
  func->null_value= true;
  return 0;
}


bool Type_handler_composite::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_composite::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_composite::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_composite::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_composite::
       Item_func_plus_fix_length_and_dec(Item_func_plus *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_composite::
       Item_func_minus_fix_length_and_dec(Item_func_minus *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_composite::
       Item_func_mul_fix_length_and_dec(Item_func_mul *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_composite::
       Item_func_div_fix_length_and_dec(Item_func_div *item) const
{
  DBUG_ASSERT(0);
  return true;
}


bool Type_handler_composite::
       Item_func_mod_fix_length_and_dec(Item_func_mod *item) const
{
  DBUG_ASSERT(0);
  return true;
}
