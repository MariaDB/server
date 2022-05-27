#ifndef ITEM_UUIDFUNC_INCLUDED
#define ITEM_UUIDFUNC_INCLUDED

/* Copyright (c) 2019,2021, MariaDB Corporation

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


#include "item.h"

class Item_func_sys_guid: public Item_str_func
{
protected:
  bool with_dashes;
  size_t uuid_len() const
  { return MY_UUID_BARE_STRING_LENGTH + with_dashes*MY_UUID_SEPARATORS; }
public:
  Item_func_sys_guid(THD *thd): Item_str_func(thd), with_dashes(false) {}
  bool fix_length_and_dec(THD *thd) override
  {
    collation.set(DTCollation_numeric());
    fix_char_length(uuid_len());
    return FALSE;
  }
  bool const_item() const override { return false; }
  table_map used_tables() const override { return RAND_TABLE_BIT; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sys_guid") };
    return name;
  }
  String *val_str(String *) override;
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_sys_guid>(thd, this); }
};

class Item_func_uuid: public Item_func_sys_guid
{
public:
  Item_func_uuid(THD *thd): Item_func_sys_guid(thd) { with_dashes= true; }
  const Type_handler *type_handler() const override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uuid") };
    return name;
  }
  bool val_native(THD *thd, Native *to) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_uuid>(thd, this); }
};

#endif // ITEM_UUIDFUNC_INCLUDED
