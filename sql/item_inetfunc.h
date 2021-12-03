#ifndef ITEM_INETFUNC_INCLUDED
#define ITEM_INETFUNC_INCLUDED

/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation

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

/*************************************************************************
  Item_func_inet_aton implements INET_ATON() SQL-function.
*************************************************************************/

class Item_func_inet_aton : public Item_longlong_func
{
  bool check_arguments() const
  { return check_argument_types_can_return_text(0, arg_count); }
public:
  Item_func_inet_aton(THD *thd, Item *a): Item_longlong_func(thd, a) {}
  longlong val_int();
  const char *func_name() const { return "inet_aton"; }
  bool fix_length_and_dec()
  {
    decimals= 0;
    max_length= 21;
    maybe_null= 1;
    unsigned_flag= 1;
    return FALSE;
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_inet_aton>(thd, this); }
};


/*************************************************************************
  Item_func_inet_ntoa implements INET_NTOA() SQL-function.
*************************************************************************/

class Item_func_inet_ntoa : public Item_str_func
{
public:
  Item_func_inet_ntoa(THD *thd, Item *a): Item_str_func(thd, a)
  { }
  String* val_str(String* str);
  const char *func_name() const { return "inet_ntoa"; }
  bool fix_length_and_dec()
  {
    decimals= 0;
    fix_length_and_charset(3 * 8 + 7, default_charset());
    maybe_null= 1;
    return FALSE;
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_inet_ntoa>(thd, this); }
};


/*************************************************************************
  Item_func_inet_bool_base implements common code for INET6/IP-related
  functions returning boolean value.
*************************************************************************/

class Item_func_inet_bool_base : public Item_bool_func
{
public:
  inline Item_func_inet_bool_base(THD *thd, Item *ip_addr):
    Item_bool_func(thd, ip_addr)
  {
    null_value= false;
  }
  bool need_parentheses_in_default() { return false; }
};


/*************************************************************************
  Item_func_inet6_aton implements INET6_ATON() SQL-function.
*************************************************************************/

class Item_func_inet6_aton : public Item_str_func
{
public:
  inline Item_func_inet6_aton(THD *thd, Item *ip_addr):
    Item_str_func(thd, ip_addr)
  { }

public:
  virtual const char *func_name() const
  { return "inet6_aton"; }

  virtual bool fix_length_and_dec()
  {
    decimals= 0;
    fix_length_and_charset(16, &my_charset_bin);
    maybe_null= 1;
    return FALSE;
  }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_inet6_aton>(thd, this); }

  String *val_str(String *to);
};


/*************************************************************************
  Item_func_inet6_ntoa implements INET6_NTOA() SQL-function.
*************************************************************************/

class Item_func_inet6_ntoa : public Item_str_ascii_func
{
public:
  inline Item_func_inet6_ntoa(THD *thd, Item *ip_addr):
    Item_str_ascii_func(thd, ip_addr)
  { }

public:
  virtual const char *func_name() const
  { return "inet6_ntoa"; }

  virtual bool fix_length_and_dec()
  {
    decimals= 0;

    // max length: IPv6-address -- 16 bytes
    // 16 bytes / 2 bytes per group == 8 groups => 7 delimiter
    // 4 symbols per group
    fix_length_and_charset(8 * 4 + 7, default_charset());

    maybe_null= 1;
    return FALSE;
  }
  String *val_str_ascii(String *to);
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_inet6_ntoa>(thd, this); }
};


/*************************************************************************
  Item_func_is_ipv4 implements IS_IPV4() SQL-function.
*************************************************************************/

class Item_func_is_ipv4 : public Item_func_inet_bool_base
{
public:
  inline Item_func_is_ipv4(THD *thd, Item *ip_addr):
    Item_func_inet_bool_base(thd, ip_addr)
  { }

public:
  virtual const char *func_name() const
  { return "is_ipv4"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_is_ipv4>(thd, this); }

  longlong val_int();
};


/*************************************************************************
  Item_func_is_ipv6 implements IS_IPV6() SQL-function.
*************************************************************************/

class Item_func_is_ipv6 : public Item_func_inet_bool_base
{
public:
  inline Item_func_is_ipv6(THD *thd, Item *ip_addr):
    Item_func_inet_bool_base(thd, ip_addr)
  { }

  virtual const char *func_name() const
  { return "is_ipv6"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_is_ipv6>(thd, this); }

  longlong val_int();
};


/*************************************************************************
  Item_func_is_ipv4_compat implements IS_IPV4_COMPAT() SQL-function.
*************************************************************************/

class Item_func_is_ipv4_compat : public Item_func_inet_bool_base
{
public:
  inline Item_func_is_ipv4_compat(THD *thd, Item *ip_addr):
    Item_func_inet_bool_base(thd, ip_addr)
  { }
  virtual const char *func_name() const
  { return "is_ipv4_compat"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_is_ipv4_compat>(thd, this); }
  longlong val_int();
};


/*************************************************************************
  Item_func_is_ipv4_mapped implements IS_IPV4_MAPPED() SQL-function.
*************************************************************************/

class Item_func_is_ipv4_mapped : public Item_func_inet_bool_base
{
public:
  inline Item_func_is_ipv4_mapped(THD *thd, Item *ip_addr):
    Item_func_inet_bool_base(thd, ip_addr)
  { }
  virtual const char *func_name() const
  { return "is_ipv4_mapped"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_is_ipv4_mapped>(thd, this); }
  longlong val_int();
};

#endif // ITEM_INETFUNC_INCLUDED
