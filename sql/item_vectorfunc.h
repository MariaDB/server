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
#include <my_global.h>
#include "item.h"
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
    static LEX_CSTRING name= { STRING_WITH_LEN("VEC_Distance") };
    return name;
  }
  Item *get_const_arg() const
  {
    if (args[0]->type() == Item::FIELD_ITEM && args[1]->const_item())
      return args[1];
    if (args[1]->type() == Item::FIELD_ITEM && args[0]->const_item())
      return args[0];
    return NULL;
  }
  key_map part_of_sortkey() const override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_vec_distance>(thd, this); }
};


class Item_func_vec_totext: public Item_str_ascii_checksum_func
{
  bool check_arguments() const override
  {
    return check_argument_types_or_binary(NULL, 0, arg_count);
  }

public:
  bool fix_length_and_dec(THD *thd) override;
  Item_func_vec_totext(THD *thd, Item *a);
  String *val_str_ascii(String *buf) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("VEC_ToText") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_vec_totext>(thd, this); }
};


class Item_func_vec_fromtext: public Item_str_func
{
  String tmp_js;
public:
  bool fix_length_and_dec(THD *thd) override;
  Item_func_vec_fromtext(THD *thd, Item *a);
  String *val_str(String *buf) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= { STRING_WITH_LEN("VEC_FromText") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_vec_fromtext>(thd, this); }
};


double euclidean_vec_distance(float *v1, float *v2, size_t v_len);
#endif
