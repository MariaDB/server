/*
   Copyright (c) 2025, Rakuten Securities
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
#ifndef ITEM_COMPOSITE_INCLUDED
#define ITEM_COMPOSITE_INCLUDED

#include "item.h"

class Field_composite;

class Item_composite_base
{
public:
  virtual ~Item_composite_base() = default;

  // For associative arrays
  /// Returns the number of columns for the elements of the array
  virtual uint rows() const { return 1; }
  virtual bool get_key(String *key, bool is_first) { return true; }
  virtual bool get_next_key(const String *curr_key, String *next_key)
  {
    return true;
  }
  virtual Item *element_by_key(THD *thd, String *key) { return nullptr; }
  virtual Item **element_addr_by_key(THD *thd, Item **addr_arg, String *key)
  {
    return addr_arg;
  }

  /*
    Get the composite field for the item, when applicable.
  */
  virtual Field_composite *get_composite_field() const { return nullptr; }
};


class Item_composite: public Item_fixed_hybrid,
                      protected Item_args,
                      public Item_composite_base
{
public:
  Item_composite(THD *thd, List<Item> &list)
    :Item_fixed_hybrid(thd), Item_args(thd, list)
  { }
  Item_composite(THD *thd, Item_args *other)
    :Item_fixed_hybrid(thd), Item_args(thd, other)
  { }
  Item_composite(THD *thd)
    :Item_fixed_hybrid(thd)
  { }

  enum Type type() const override { return ROW_ITEM; }

  void illegal_method_call(const char *);

  void make_send_field(THD *thd, Send_field *) override
  {
    illegal_method_call((const char*)"make_send_field");
  };
  double val_real() override
  {
    illegal_method_call((const char*)"val");
    return 0;
  };
  longlong val_int() override
  {
    illegal_method_call((const char*)"val_int");
    return 0;
  };
  String *val_str(String *) override
  {
    illegal_method_call((const char*)"val_str");
    return 0;
  };
  my_decimal *val_decimal(my_decimal *) override
  {
    illegal_method_call((const char*)"val_decimal");
    return 0;
  };
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    illegal_method_call((const char*)"get_date");
    return true;
  }
};


#endif /* ITEM_COMPOSITE_INCLUDED */
