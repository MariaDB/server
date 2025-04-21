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


const Type_handler *Type_handler_composite::get_handler(Item *item)
{
  if (item->cmp_type() != ROW_RESULT)
    return NULL;

  if (dynamic_cast<Item_assoc_array *>(item) ||
      dynamic_cast<Item_field_assoc_array *>(item))
    return &type_handler_assoc_array;

  return &type_handler_row;
}
