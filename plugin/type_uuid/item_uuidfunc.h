#ifndef ITEM_UUIDFUNC_INCLUDED
#define ITEM_UUIDFUNC_INCLUDED

/* Copyright (c) 2019,2024, MariaDB Corporation

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
#include "item_timefunc.h"
#include "sql_type_uuid_v1.h"
#include "sql_type_uuid_v4.h"
#include "sql_type_uuid_v7.h"

class Item_func_sys_guid: public Item_str_func
{
protected:
  static size_t uuid_len()
  { return MY_UUID_BARE_STRING_LENGTH; }
public:
  Item_func_sys_guid(THD *thd): Item_str_func(thd) {}
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
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_sys_guid>(thd, this); }
};


template<class UUIDvX>
class Item_func_uuid_vx: public Type_handler_uuid_new::Item_fbt_func
{
public:
  using Item_fbt_func::Item_fbt_func;
  bool const_item() const override { return false; }
  table_map used_tables() const override { return RAND_TABLE_BIT; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_NON_DETERMINISTIC);
  }
  String *val_str(String *str) override
  {
    DBUG_ASSERT(fixed());
    return UUIDvX().to_string(str) ? NULL : str;
  }
  bool val_native(THD *thd, Native *to) override
  {
    DBUG_ASSERT(fixed());
    return UUIDvX::construct_native(to);
  }
};


class Item_func_uuid: public Item_func_uuid_vx<UUIDv1>
{
public:
  using Item_func_uuid_vx::Item_func_uuid_vx;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uuid") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_uuid>(thd, this); }
};


class Item_func_uuid_v4: public Item_func_uuid_vx<UUIDv4>
{
public:
  using Item_func_uuid_vx::Item_func_uuid_vx;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uuid_v4") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_uuid_v4>(thd, this); }
};

class Item_func_uuid_v7: public Item_func_uuid_vx<UUIDv7>
{
public:
  using Item_func_uuid_vx::Item_func_uuid_vx;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("uuid_v7") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_uuid_v7>(thd, this); }
};


class Item_func_uuid_timestamp: public Item_timestampfunc
{
  bool check_arguments() const override
  {
    return args[0]->check_type_can_return_str(func_name_cstring());
  }
  bool get_timestamp(my_time_t *sec, ulong *usec);
public:
  Item_func_uuid_timestamp(THD *thd, Item *arg1)
    : Item_timestampfunc(thd, arg1) {}

  LEX_CSTRING func_name_cstring() const override
  { return {STRING_WITH_LEN("uuid_timestamp")}; }

  bool fix_length_and_dec(THD *thd) override;
  bool val_native(THD *thd, Native *to) override;

  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_uuid_timestamp>(thd, this); }
};

#endif // ITEM_UUIDFUNC_INCLUDED
