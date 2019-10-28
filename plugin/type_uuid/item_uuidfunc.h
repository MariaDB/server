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

class Item_func_uuid: public Item_str_func
{
  bool with_separators;
public:
  Item_func_uuid(THD *thd, bool with_separators_arg):
    Item_str_func(thd), with_separators(with_separators_arg) {}
  bool fix_length_and_dec()
  {
    collation.set(DTCollation_numeric());
    fix_char_length(with_separators ? MY_UUID_STRING_LENGTH
                                    : MY_UUID_BARE_STRING_LENGTH);
    return FALSE;
  }
  bool const_item() const { return false; }
  table_map used_tables() const { return RAND_TABLE_BIT; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name1= {STRING_WITH_LEN("uuid") };
    static LEX_CSTRING name2= {STRING_WITH_LEN("sys_guid") };
    return with_separators ? name1 : name2;
  }
  String *val_str(String *);
  bool check_vcol_func_processor(void *arg)
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_NON_DETERMINISTIC);
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_uuid>(thd, this); }
};

#endif // ITEM_UUIDFUNC_INCLUDED
