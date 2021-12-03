/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates.

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


/**
  @file

  @brief
  Buffers to save and compare item values
*/

#include "mariadb.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"          // THD
#include "set_var.h"            // Cached_item, Cached_item_field, ...

/**
  Create right type of Cached_item for an item.
*/

Cached_item *new_Cached_item(THD *thd, Item *item, bool pass_through_ref)
{
  if (pass_through_ref && item->real_item()->type() == Item::FIELD_ITEM &&
      !(((Item_field *) (item->real_item()))->field->flags & BLOB_FLAG))
  {
    Item_field *real_item= (Item_field *) item->real_item();
    Field *cached_field= real_item->field;
    return new (thd->mem_root) Cached_item_field(thd, cached_field);
  }
  switch (item->result_type()) {
  case STRING_RESULT:
    return new Cached_item_str(thd, item);
  case INT_RESULT:
    return new Cached_item_int(item);
  case REAL_RESULT:
    return new Cached_item_real(item);
  case DECIMAL_RESULT:
    return new Cached_item_decimal(item);
  case ROW_RESULT:
  default:
    DBUG_ASSERT(0);
    return 0;
  }
}

Cached_item::~Cached_item() {}

/**
  Compare with old value and replace value with new value.

  @return
    Return true if values have changed
*/

Cached_item_str::Cached_item_str(THD *thd, Item *arg)
  :Cached_item_item(arg),
   value_max_length(MY_MIN(arg->max_length, thd->variables.max_sort_length)),
   value(value_max_length)
{}

bool Cached_item_str::cmp(void)
{
  String *res;
  bool tmp;

  if ((res=item->val_str(&tmp_value)))
    res->length(MY_MIN(res->length(), value_max_length));
  if (null_value != item->null_value)
  {
    if ((null_value= item->null_value))
      return TRUE;				// New value was null
    tmp=TRUE;
  }
  else if (null_value)
    return 0;					// new and old value was null
  else
    tmp= sortcmp(&value,res,item->collation.collation) != 0;
  if (tmp)
    value.copy(*res);				// Remember for next cmp
  return tmp;
}


int Cached_item_str::cmp_read_only()
{
  String *res= item->val_str(&tmp_value);

  if (null_value)
  {
    if (item->null_value)
      return 0;
    else
      return -1;
  }
  if (item->null_value)
    return 1;

  return sortcmp(&value, res, item->collation.collation);
}


Cached_item_str::~Cached_item_str()
{
  item=0;					// Safety
}

bool Cached_item_real::cmp(void)
{
  double nr= item->val_real();
  if (null_value != item->null_value || nr != value)
  {
    null_value= item->null_value;
    value=nr;
    return TRUE;
  }
  return FALSE;
}


int Cached_item_real::cmp_read_only()
{
  double nr= item->val_real();
  if (null_value)
  {
    if (item->null_value)
      return 0;
    else
      return -1;
  }
  if (item->null_value)
    return 1;
  return (nr == value)? 0 : ((nr < value)? 1: -1);
}


bool Cached_item_int::cmp(void)
{
  longlong nr=item->val_int();
  if (null_value != item->null_value || nr != value)
  {
    null_value= item->null_value;
    value=nr;
    return TRUE;
  }
  return FALSE;
}


int Cached_item_int::cmp_read_only()
{
  longlong nr= item->val_int();
  if (null_value)
  {
    if (item->null_value)
      return 0;
    else
      return -1;
  }
  if (item->null_value)
    return 1;
  return (nr == value)? 0 : ((nr < value)? 1: -1);
}


bool Cached_item_field::cmp(void)
{
  bool tmp= FALSE;                              // Value is identical
  /* Note that field can't be a blob here ! */
  if (null_value != field->is_null())
  {
    null_value= !null_value;
    tmp= TRUE;                                  // Value has changed
  }

  /*
    If value is not null and value changed (from null to not null or
    because of value change), then copy the new value to buffer.
    */
  if (! null_value && (tmp || (tmp= (field->cmp(buff) != 0))))
    field->get_image(buff,length,field->charset());
  return tmp;
}


int Cached_item_field::cmp_read_only()
{
  if (null_value)
  {
    if (field->is_null())
      return 0;
    else
      return -1;
  }
  if (field->is_null())
    return 1;

  return field->cmp(buff);
}


Cached_item_decimal::Cached_item_decimal(Item *it)
  :Cached_item_item(it)
{
  my_decimal_set_zero(&value);
}


bool Cached_item_decimal::cmp()
{
  VDec tmp(item);
  if (null_value != tmp.is_null() ||
      (!tmp.is_null() && tmp.cmp(&value)))
  {
    null_value= tmp.is_null();
    /* Save only not null values */
    if (!null_value)
    {
      my_decimal2decimal(tmp.ptr(), &value);
      return TRUE;
    }
    return FALSE;
  }
  return FALSE;
}


int Cached_item_decimal::cmp_read_only()
{
  VDec tmp(item);
  if (null_value)
    return tmp.is_null() ? 0 : -1;
  return tmp.is_null() ? 1 : value.cmp(tmp.ptr());
}

