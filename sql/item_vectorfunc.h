#ifndef ITEM_VECTORFUNC_INCLUDED
#define ITEM_VECTORFUNC_INCLUDED

/* Copyright (C) 2023, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* This file defines all vector functions */
#include "lex_string.h"
#include "item_func.h"

class Item_func_vec_distance: public Item_real_func
{
  Item_field *get_field_arg() const
  {
    if (args[0]->type() == Item::FIELD_ITEM && args[1]->const_item())
      return (Item_field*)(args[0]);
    if (args[1]->type() == Item::FIELD_ITEM && args[0]->const_item())
      return (Item_field*)(args[1]);
    return NULL;
  }
  bool check_arguments() const override
  {
    return check_argument_types_or_binary(NULL, 0, arg_count);
  }
public:
  Item_func_vec_distance(THD *thd, Item *a, Item *b)
   :Item_real_func(thd, a, b) {}
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    return Item_real_func::fix_length_and_dec(thd);
  }
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("vec_distance") };
    return name;
  }
  key_map part_of_sortkey() const override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_vec_distance>(thd, this); }
};

#endif
