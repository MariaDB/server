#ifndef ITEM_NUMCONVFUNC_INCLUDED
#define ITEM_NUMCONVFUNC_INCLUDED

/*
   Copyright (c) 2009, 2025, MariaDB Corporation

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


#include "item_func.h"

class Item_func_to_number: public Item_real_func
{
public:
  Item_func_to_number(THD *thd, Item *a, Item *b)
   :Item_real_func(thd, a, b)
  {  }
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    decimals= NOT_FIXED_DEC;
    max_length= float_length(decimals);
    return false;
  }
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("to_number") };
    return name;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_to_number>(thd, this); }
};

#endif // ITEM_NUMCONVFUNC_INCLUDED
