#ifndef ITEM_JSONFUNC_INCLUDED
#define ITEM_JSONFUNC_INCLUDED

/* Copyright (c) 2016, 2021, MariaDB

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
#include "item_sum.h"


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


void report_path_error_ex(const char *ps, json_path_t *p,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv);
void report_json_error_ex(const char *js, json_engine_t *je,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv);

class Json_engine_scan: public json_engine_t
{
public:
  Json_engine_scan(CHARSET_INFO *i_cs, const uchar *str, const uchar *end)
  {
    json_scan_start(this, i_cs, str, end);
  }
  Json_engine_scan(const String &str)
   :Json_engine_scan(str.charset(), (const uchar *) str.ptr(),
                                    (const uchar *) str.end())
  { }
  bool check_and_get_value_scalar(String *res, int *error);
  bool check_and_get_value_complex(String *res, int *error);
};


class Json_path_extractor: public json_path_with_flags
{
protected:
  String tmp_js, tmp_path;
  virtual ~Json_path_extractor() { }
  virtual bool check_and_get_value(Json_engine_scan *je,
                                   String *to, int *error)=0;
  bool extract(String *to, Item *js, Item *jp, CHARSET_INFO *cs);
};


class Item_func_json_valid: public Item_bool_func
{
protected:
  String tmp_value;

public:
  Item_func_json_valid(THD *thd, Item *json) : Item_bool_func(thd, json) {}
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_valid") };
    return name;
  }
  bool fix_length_and_dec() override
  {
    if (Item_bool_func::fix_length_and_dec())
      return TRUE;
    set_maybe_null();
    return FALSE;
  }
  bool set_format_by_check_constraint(Send_field_extended_metadata *to) const
    override
  {
    static const Lex_cstring fmt(STRING_WITH_LEN("json"));
    return to->set_format_name(fmt);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_valid>(thd, this); }
  enum Functype functype() const override { return JSON_VALID_FUNC; }
};


class Item_func_json_exists: public Item_bool_func
{
protected:
  json_path_with_flags path;
  String tmp_js, tmp_path;

public:
  Item_func_json_exists(THD *thd, Item *js, Item *i_path):
    Item_bool_func(thd, js, i_path) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_exists") };
    return name;
  }
  bool fix_length_and_dec() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_exists>(thd, this); }
  longlong val_int() override;
};


class Item_json_func: public Item_str_func
{
public:
  Item_json_func(THD *thd)
   :Item_str_func(thd) { }
  Item_json_func(THD *thd, Item *a)
   :Item_str_func(thd, a) { }
  Item_json_func(THD *thd, Item *a, Item *b)
   :Item_str_func(thd, a, b) { }
  Item_json_func(THD *thd, List<Item> &list)
   :Item_str_func(thd, list) { }
  bool is_json_type() override { return true; }
  void make_send_field(THD *thd, Send_field *tmp_field) override
  {
    Item_str_func::make_send_field(thd, tmp_field);
    static const Lex_cstring fmt(STRING_WITH_LEN("json"));
    tmp_field->set_format_name(fmt);
  }
};


class Item_func_json_value: public Item_str_func,
                            public Json_path_extractor
{

public:
  Item_func_json_value(THD *thd, Item *js, Item *i_path):
    Item_str_func(thd, js, i_path) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_value") };
    return name;
  }
  bool fix_length_and_dec() override ;
  String *val_str(String *to) override
  {
    null_value= Json_path_extractor::extract(to, args[0], args[1],
                                             collation.collation);
    return null_value ? NULL : to;
  }
  bool check_and_get_value(Json_engine_scan *je,
                           String *res, int *error) override
  {
    return je->check_and_get_value_scalar(res, error);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_value>(thd, this); }
};


class Item_func_json_query: public Item_json_func,
                            public Json_path_extractor
{
public:
  Item_func_json_query(THD *thd, Item *js, Item *i_path):
    Item_json_func(thd, js, i_path) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_query") };
    return name;
  }
  bool fix_length_and_dec() override;
  String *val_str(String *to) override
  {
    null_value= Json_path_extractor::extract(to, args[0], args[1],
                                             collation.collation);
    return null_value ? NULL : to;
  }
  bool check_and_get_value(Json_engine_scan *je,
                           String *res, int *error) override
  {
    return je->check_and_get_value_complex(res, error);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_query>(thd, this); }
};


class Item_func_json_quote: public Item_str_func
{
protected:
  String tmp_s;

public:
  Item_func_json_quote(THD *thd, Item *s): Item_str_func(thd, s) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_quote") };
    return name;
  }
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_quote>(thd, this); }
};


class Item_func_json_unquote: public Item_str_func
{
protected:
  String tmp_s;
  String *read_json(json_engine_t *je);
public:
  Item_func_json_unquote(THD *thd, Item *s): Item_str_func(thd, s) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_unquote") };
    return name;
  }
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_unquote>(thd, this); }
};


class Item_json_str_multipath: public Item_json_func
{
protected:
  json_path_with_flags *paths;
  String *tmp_paths;
public:
  Item_json_str_multipath(THD *thd, List<Item> &list):
    Item_json_func(thd, list), tmp_paths(0) {}
  bool fix_fields(THD *thd, Item **ref);
  void cleanup();
  virtual uint get_n_paths() const = 0;
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
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_extract") };
    return name;
  }
  enum Functype functype() const override { return JSON_EXTRACT_FUNC; }
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  longlong val_int() override;
  double val_real() override;
  my_decimal *val_decimal(my_decimal *) override;
  uint get_n_paths() const override { return arg_count - 1; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_extract>(thd, this); }
};


class Item_func_json_contains: public Item_bool_func
{
protected:
  String tmp_js;
  json_path_with_flags path;
  String tmp_path;
  bool a2_constant, a2_parsed;
  String tmp_val, *val;
public:
  Item_func_json_contains(THD *thd, List<Item> &list):
    Item_bool_func(thd, list) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_contains") };
    return name;
  }
  bool fix_length_and_dec() override;
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_contains>(thd, this); }
};


class Item_func_json_contains_path: public Item_bool_func
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
    Item_bool_func(thd, list), tmp_paths(0) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_contains_path") };
    return name;
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec() override;
  void cleanup() override;
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_contains_path>(thd, this); }
};


class Item_func_json_array: public Item_json_func
{
protected:
  String tmp_val;
  ulong result_limit;
public:
  Item_func_json_array(THD *thd):
    Item_json_func(thd) {}
  Item_func_json_array(THD *thd, List<Item> &list):
    Item_json_func(thd, list) {}
  String *val_str(String *) override;
  bool fix_length_and_dec() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_array") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_array>(thd, this); }
};


class Item_func_json_array_append: public Item_json_str_multipath
{
protected:
  String tmp_js;
  String tmp_val;
public:
  Item_func_json_array_append(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count/2; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_array_append") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_array_append>(thd, this); }
};


class Item_func_json_array_insert: public Item_func_json_array_append
{
public:
  Item_func_json_array_insert(THD *thd, List<Item> &list):
    Item_func_json_array_append(thd, list) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_array_insert") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_array_insert>(thd, this); }
};


class Item_func_json_object: public Item_func_json_array
{
public:
  Item_func_json_object(THD *thd):
    Item_func_json_array(thd) {}
  Item_func_json_object(THD *thd, List<Item> &list):
    Item_func_json_array(thd, list) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_object") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_object>(thd, this); }
};


class Item_func_json_merge: public Item_func_json_array
{
protected:
  String tmp_js1, tmp_js2;
public:
  Item_func_json_merge(THD *thd, List<Item> &list):
    Item_func_json_array(thd, list) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_merge_preserve") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_merge>(thd, this); }
};

class Item_func_json_merge_patch: public Item_func_json_merge
{
public:
  Item_func_json_merge_patch(THD *thd, List<Item> &list):
    Item_func_json_merge(thd, list) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_merge_patch") };
    return name;
  }
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_merge_patch>(thd, this); }
};

class Item_func_json_length: public Item_long_func
{
  bool check_arguments() const override
  {
    return args[0]->check_type_can_return_text(func_name_cstring()) ||
           (arg_count > 1 &&
            args[1]->check_type_general_purpose_string(func_name_cstring()));
  }
protected:
  json_path_with_flags path;
  String tmp_js;
  String tmp_path;
public:
  Item_func_json_length(THD *thd, List<Item> &list):
    Item_long_func(thd, list) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_length") };
    return name;
  }
  bool fix_length_and_dec() override;
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_length>(thd, this); }
};


class Item_func_json_depth: public Item_long_func
{
  bool check_arguments() const override
  { return args[0]->check_type_can_return_text(func_name_cstring()); }
protected:
  String tmp_js;
public:
  Item_func_json_depth(THD *thd, Item *js): Item_long_func(thd, js) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_depth") };
    return name;
  }
  bool fix_length_and_dec() override { max_length= 10; return FALSE; }
  longlong val_int() override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_depth>(thd, this); }
};


class Item_func_json_type: public Item_str_func
{
protected:
  String tmp_js;
public:
  Item_func_json_type(THD *thd, Item *js): Item_str_func(thd, js) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_type") };
    return name;
  }
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_type>(thd, this); }
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
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count/2; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING json_set=    {STRING_WITH_LEN("json_set") };
    static LEX_CSTRING json_insert= {STRING_WITH_LEN("json_insert") };
    static LEX_CSTRING json_replace= {STRING_WITH_LEN("json_replace") };
    return (mode_insert ?
            (mode_replace ? json_set : json_insert) : json_replace);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_insert>(thd, this); }
};


class Item_func_json_remove: public Item_json_str_multipath
{
protected:
  String tmp_js;
public:
  Item_func_json_remove(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count - 1; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_remove") };
    return name;
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_remove>(thd, this); }
};


class Item_func_json_keys: public Item_str_func
{
protected:
  json_path_with_flags path;
  String tmp_js, tmp_path;

public:
  Item_func_json_keys(THD *thd, List<Item> &list):
    Item_str_func(thd, list) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_keys") };
    return name;
  }
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_keys>(thd, this); }
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
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_search") };
    return name;
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec() override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count > 4 ? arg_count - 4 : 0; }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_search>(thd, this); }
};


class Item_func_json_format: public Item_json_func
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
    Item_json_func(thd, js), fmt(format) {}
  Item_func_json_format(THD *thd, List<Item> &list):
    Item_json_func(thd, list), fmt(DETAILED) {}

  LEX_CSTRING func_name_cstring() const override;
  bool fix_length_and_dec() override;
  String *val_str(String *str) override;
  String *val_json(String *str) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_format>(thd, this); }
};


class Item_func_json_arrayagg : public Item_func_group_concat
{
protected:
  /*
    Overrides Item_func_group_concat::skip_nulls()
    NULL-s should be added to the result as JSON null value.
  */
  bool skip_nulls() const override { return false; }
  String *get_str_from_item(Item *i, String *tmp) override;
  String *get_str_from_field(Item *i, Field *f, String *tmp,
                             const uchar *key, size_t offset) override;
  void cut_max_length(String *result,
                      uint old_length, uint max_length) const override;
public:
  String m_tmp_json; /* Used in get_str_from_*.. */
  Item_func_json_arrayagg(THD *thd, Name_resolution_context *context_arg,
                          bool is_distinct, List<Item> *is_select,
                          const SQL_I_List<ORDER> &is_order, String *is_separator,
                          bool limit_clause, Item *row_limit, Item *offset_limit):
      Item_func_group_concat(thd, context_arg, is_distinct, is_select, is_order,
                             is_separator, limit_clause, row_limit, offset_limit)
  {
  }
  Item_func_json_arrayagg(THD *thd, Item_func_json_arrayagg *item) :
    Item_func_group_concat(thd, item) {}
  bool is_json_type() override { return true; }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_arrayagg(") };
    return name;
  }
  enum Sumfunctype sum_func() const override { return JSON_ARRAYAGG_FUNC; }

  String* val_str(String *str) override;

  Item *copy_or_same(THD* thd) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_arrayagg>(thd, this); }
};


class Item_func_json_objectagg : public Item_sum
{
  String result;
public:
  Item_func_json_objectagg(THD *thd, Item *key, Item *value) :
    Item_sum(thd, key, value)
  {
    quick_group= FALSE;
    result.append('{');
  }

  Item_func_json_objectagg(THD *thd, Item_func_json_objectagg *item);
  bool is_json_type() override { return true; }
  void cleanup() override;

  enum Sumfunctype sum_func () const override { return JSON_OBJECTAGG_FUNC;}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_objectagg") };
    return name;
  }
  const Type_handler *type_handler() const override
  {
    if (too_big_for_varchar())
      return &type_handler_blob;
    return &type_handler_varchar;
  }
  void clear() override;
  bool add() override;
  void reset_field() override { DBUG_ASSERT(0); }        // not used
  void update_field() override { DBUG_ASSERT(0); }       // not used
  bool fix_fields(THD *,Item **) override;

  double val_real() override { return 0.0; }
  longlong val_int() override { return 0; }
  my_decimal *val_decimal(my_decimal *decimal_value) override
  {
    my_decimal_set_zero(decimal_value);
    return decimal_value;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return get_date_from_string(thd, ltime, fuzzydate);
  }
  String* val_str(String* str) override;
  Item *copy_or_same(THD* thd) override;
  void no_rows_in_result() override {}
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_json_objectagg>(thd, this); }
};


#endif /* ITEM_JSONFUNC_INCLUDED */
