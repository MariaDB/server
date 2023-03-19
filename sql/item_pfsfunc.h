#ifndef ITEM_PFS_FUNC_INCLUDED
#define ITEM_PFS_FUNC_INCLUDED

/* Copyright (c) 2000, 2023, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "item_func.h"      // Item_str_func, Item_int_func

/** format_pico_time() */

class Item_func_pfs_format_pico_time : public Item_str_func {
  String m_value;
  char m_value_buffer[12];

public:
  Item_func_pfs_format_pico_time(THD *thd, Item *a)
  : Item_str_func(thd, a){};
  String *val_str(String *str __attribute__ ((__unused__))) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("format_pico_time")};
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *get_copy(THD *thd) override
  {
    return get_item_copy<Item_func_pfs_format_pico_time>(thd, this);
  }
};

#endif
