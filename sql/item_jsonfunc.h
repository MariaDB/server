#ifndef ITEM_JSONFUNC_INCLUDED
#define ITEM_JSONFUNC_INCLUDED

/* Copyright (c) 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* This file defines all JSON functions */


#include <json_lib.h>
#include "item_cmpfunc.h"      // Item_bool_func
#include "item_strfunc.h"      // Item_str_func


class json_path_with_flags
{
public:
  json_path_t p;
  bool constant;
  bool parsed;
  json_path_step_t *cur_step;
  void set_constant_flag(bool s_constant)
  {
    constant= s_constant;
    parsed= FALSE;
  }
};


class Item_func_json_valid: public Item_int_func
{
protected:
  String tmp_value;

public:
  Item_func_json_valid(THD *thd, Item *json) : Item_int_func(thd, json) {}
  longlong val_int();
  const char *func_name() const { return "json_valid"; }
  bool fix_length_and_dec()
  {
    if (Item_int_func::fix_length_and_dec())
      return TRUE;
    maybe_null= 1;
    return FALSE;
  }
  bool is_bool_type() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_valid>(thd, mem_root, this); }
};


class Item_func_json_exists: public Item_int_func
{
protected:
  json_path_with_flags path;
  String tmp_js, tmp_path;

public:
  Item_func_json_exists(THD *thd, Item *js, Item *i_path):
    Item_int_func(thd, js, i_path) {}
  const char *func_name() const { return "json_exists"; }
  bool is_bool_type() { return true; }
  bool fix_length_and_dec();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_exists>(thd, mem_root, this); }
  longlong val_int();
};


class Item_func_json_value: public Item_str_func
{
protected:
  json_path_with_flags path;
  String tmp_js, tmp_path;

public:
  Item_func_json_value(THD *thd, Item *js, Item *i_path):
    Item_str_func(thd, js, i_path) {}
  const char *func_name() const { return "json_value"; }
  bool fix_length_and_dec();
  String *val_str(String *);
  virtual bool check_and_get_value(json_engine_t *je, String *res, int *error);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_value>(thd, mem_root, this); }
};


class Item_func_json_query: public Item_func_json_value
{
public:
  Item_func_json_query(THD *thd, Item *js, Item *i_path):
    Item_func_json_value(thd, js, i_path) {}
  bool is_json_type() { return true; }
  const char *func_name() const { return "json_query"; }
  bool check_and_get_value(json_engine_t *je, String *res, int *error);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_query>(thd, mem_root, this); }
};


class Item_func_json_quote: public Item_str_func
{
protected:
  String tmp_s;

public:
  Item_func_json_quote(THD *thd, Item *s): Item_str_func(thd, s) {}
  const char *func_name() const { return "json_quote"; }
  bool fix_length_and_dec();
  String *val_str(String *);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_quote>(thd, mem_root, this); }
};


class Item_func_json_unquote: public Item_str_func
{
protected:
  String tmp_s;
  String *read_json(json_engine_t *je);
public:
  Item_func_json_unquote(THD *thd, Item *s): Item_str_func(thd, s) {}
  const char *func_name() const { return "json_unquote"; }
  bool fix_length_and_dec();
  String *val_str(String *);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_unquote>(thd, mem_root, this); }
};


class Item_json_str_multipath: public Item_str_func
{
protected:
  json_path_with_flags *paths;
  String *tmp_paths;
public:
  Item_json_str_multipath(THD *thd, List<Item> &list):
    Item_str_func(thd, list), tmp_paths(0) {}
  bool fix_fields(THD *thd, Item **ref);
  void cleanup();
  virtual uint get_n_paths() const = 0;
  bool is_json_type() { return true; }
};


class Item_func_json_extract: public Item_json_str_multipath
{
protected:
  String tmp_js;
public:
  String *read_json(String *str, json_value_types *type,
                    char **out_val, int *value_len);
  Item_func_json_extract(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  const char *func_name() const { return "json_extract"; }
  enum Functype functype() const   { return JSON_EXTRACT_FUNC; }
  bool fix_length_and_dec();
  String *val_str(String *);
  longlong val_int();
  double val_real();
  my_decimal *val_decimal(my_decimal *);
  uint get_n_paths() const { return arg_count - 1; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_extract>(thd, mem_root, this); }
};


class Item_func_json_contains: public Item_int_func
{
protected:
  String tmp_js;
  json_path_with_flags path;
  String tmp_path;
  bool a2_constant, a2_parsed;
  String tmp_val, *val;
public:
  Item_func_json_contains(THD *thd, List<Item> &list):
    Item_int_func(thd, list) {}
  const char *func_name() const { return "json_contains"; }
  bool fix_length_and_dec();
  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_contains>(thd, mem_root, this); }
};


class Item_func_json_contains_path: public Item_int_func
{
protected:
  String tmp_js;
  json_path_with_flags *paths;
  String *tmp_paths;
  bool mode_one;
  bool ooa_constant, ooa_parsed;
  bool *p_found;

public:
  Item_func_json_contains_path(THD *thd, List<Item> &list):
    Item_int_func(thd, list), tmp_paths(0) {}
  const char *func_name() const { return "json_contains_path"; }
  bool fix_fields(THD *thd, Item **ref);
  bool fix_length_and_dec();
  void cleanup();
  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_contains_path>(thd, mem_root, this); }
};


class Item_func_json_array: public Item_str_func
{
protected:
  String tmp_val;
  ulong result_limit;
public:
  Item_func_json_array(THD *thd):
    Item_str_func(thd) {}
  Item_func_json_array(THD *thd, List<Item> &list):
    Item_str_func(thd, list) {}
  String *val_str(String *);
  bool is_json_type() { return true; }
  bool fix_length_and_dec();
  const char *func_name() const { return "json_array"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_array>(thd, mem_root, this); }
};


class Item_func_json_array_append: public Item_json_str_multipath
{
protected:
  String tmp_js;
  String tmp_val;
public:
  Item_func_json_array_append(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  bool fix_length_and_dec();
  String *val_str(String *);
  uint get_n_paths() const { return arg_count/2; }
  const char *func_name() const { return "json_array_append"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_array_append>(thd, mem_root, this); }
};


class Item_func_json_array_insert: public Item_func_json_array_append
{
public:
  Item_func_json_array_insert(THD *thd, List<Item> &list):
    Item_func_json_array_append(thd, list) {}
  String *val_str(String *);
  const char *func_name() const { return "json_array_insert"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_array_insert>(thd, mem_root, this); }
};


class Item_func_json_object: public Item_func_json_array
{
public:
  Item_func_json_object(THD *thd):
    Item_func_json_array(thd) {}
  Item_func_json_object(THD *thd, List<Item> &list):
    Item_func_json_array(thd, list) {}
  String *val_str(String *);
  bool is_json_type() { return true; }
  const char *func_name() const { return "json_object"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_object>(thd, mem_root, this); }
};


class Item_func_json_merge: public Item_func_json_array
{
protected:
  String tmp_js1, tmp_js2;
public:
  Item_func_json_merge(THD *thd, List<Item> &list):
    Item_func_json_array(thd, list) {}
  String *val_str(String *);
  bool is_json_type() { return true; }
  const char *func_name() const { return "json_merge_preserve"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_merge>(thd, mem_root, this); }
};


class Item_func_json_merge_patch: public Item_func_json_merge
{
public:
  Item_func_json_merge_patch(THD *thd, List<Item> &list):
    Item_func_json_merge(thd, list) {}
  const char *func_name() const { return "json_merge_patch"; }
  String *val_str(String *);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_merge_patch>(thd, mem_root, this); }
};


class Item_func_json_length: public Item_int_func
{
protected:
  json_path_with_flags path;
  String tmp_js;
  String tmp_path;
public:
  Item_func_json_length(THD *thd, List<Item> &list):
    Item_int_func(thd, list) {}
  const char *func_name() const { return "json_length"; }
  bool fix_length_and_dec();
  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_length>(thd, mem_root, this); }
};


class Item_func_json_depth: public Item_int_func
{
protected:
  String tmp_js;
public:
  Item_func_json_depth(THD *thd, Item *js): Item_int_func(thd, js) {}
  const char *func_name() const { return "json_depth"; }
  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_depth>(thd, mem_root, this); }
};


class Item_func_json_type: public Item_str_func
{
protected:
  String tmp_js;
public:
  Item_func_json_type(THD *thd, Item *js): Item_str_func(thd, js) {}
  const char *func_name() const { return "json_type"; }
  bool fix_length_and_dec();
  String *val_str(String *);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_type>(thd, mem_root, this); }
};


class Item_func_json_insert: public Item_json_str_multipath
{
protected:
  String tmp_js;
  String tmp_val;
  bool mode_insert, mode_replace;
public:
  Item_func_json_insert(bool i_mode, bool r_mode, THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list),
      mode_insert(i_mode), mode_replace(r_mode) {}
  bool fix_length_and_dec();
  String *val_str(String *);
  uint get_n_paths() const { return arg_count/2; }
  const char *func_name() const
  {
    return mode_insert ?
             (mode_replace ? "json_set" : "json_insert") : "json_update";
  }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_insert>(thd, mem_root, this); }
};


class Item_func_json_remove: public Item_json_str_multipath
{
protected:
  String tmp_js;
public:
  Item_func_json_remove(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  bool fix_length_and_dec();
  String *val_str(String *);
  uint get_n_paths() const { return arg_count - 1; }
  const char *func_name() const { return "json_remove"; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_remove>(thd, mem_root, this); }
};


class Item_func_json_keys: public Item_str_func
{
protected:
  json_path_with_flags path;
  String tmp_js, tmp_path;

public:
  Item_func_json_keys(THD *thd, List<Item> &list):
    Item_str_func(thd, list) {}
  const char *func_name() const { return "json_keys"; }
  bool fix_length_and_dec();
  String *val_str(String *);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_keys>(thd, mem_root, this); }
};


class Item_func_json_search: public Item_json_str_multipath
{
protected:
  String tmp_js, tmp_path, esc_value;
  bool mode_one;
  bool ooa_constant, ooa_parsed;
  int escape;
  int n_path_found;
  json_path_t sav_path;

  int compare_json_value_wild(json_engine_t *je, const String *cmp_str);

public:
  Item_func_json_search(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  const char *func_name() const { return "json_search"; }
  bool fix_fields(THD *thd, Item **ref);
  bool fix_length_and_dec();
  String *val_str(String *);
  uint get_n_paths() const { return arg_count > 4 ? arg_count - 4 : 0; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_search>(thd, mem_root, this); }
};


class Item_func_json_format: public Item_str_func
{
public:
  enum formats
  {
    NONE,
    COMPACT,
    LOOSE,
    DETAILED
  };
protected:
  formats fmt;
  String tmp_js;
public:
  Item_func_json_format(THD *thd, Item *js, formats format):
    Item_str_func(thd, js), fmt(format) {}
  Item_func_json_format(THD *thd, List<Item> &list):
    Item_str_func(thd, list), fmt(DETAILED) {}

  const char *func_name() const;
  bool fix_length_and_dec();
  String *val_str(String *str);
  String *val_json(String *str);
  bool is_json_type() { return true; }
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_json_format>(thd, mem_root, this); }
};


#endif /* ITEM_JSONFUNC_INCLUDED */
