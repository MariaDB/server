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
#include "sql_type_json.h"

#ifndef DBUG_OFF
/*
  Holds the reading count still across a reading the released server does
  not do.

  The count is of the work a server does, and a debug build's reading back
  of a value to check what was claimed about it is not that work: counting
  it would move a number kept to watch queries for reasons no query has.
  Where the reading is one call the count is taken straight back off it;
  where it is a whole expression, whose readings the caller cannot count,
  the figure is put back as it stood.
*/
class Json_scans_unbilled
{
  THD *m_thd;
  ulong m_scans;
public:
  Json_scans_unbilled(THD *thd);
  ~Json_scans_unbilled();
};
#endif

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
bool check_overlaps(json_engine_t *js, json_engine_t *value, bool compare_whole);

/*
  What an append of escaped text to a document came to.  The two ways
  of failing are kept apart because they are not answered alike: a
  buffer that would not grow has already raised an error of its own and
  the statement is over, while a character that cannot be written into
  a document is an ordinary property of the data, which callers have
  always carried on past.
*/
enum json_append_result
{
  JSON_APPEND_OK= 0,
  JSON_APPEND_OOM= 1,
  JSON_APPEND_BAD_CHR= 2
};

json_append_result st_append_escaped(String *s, const String *a);
int json_find_overlap_with_object(json_engine_t *js,
                                              json_engine_t *value,
                                              bool compare_whole);
void json_skip_current_level(json_engine_t *js, json_engine_t *value);
bool json_find_overlap_with_scalar(json_engine_t *js, json_engine_t *value);
bool json_compare_arrays_in_order_in_order(json_engine_t *js, json_engine_t *value);
bool json_compare_arr_and_obj(json_engine_t *js, json_engine_t* value);
int json_find_overlap_with_array(json_engine_t *js,
                                             json_engine_t *value,
                                             bool compare_whole);



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
  bool extract(String *to, Item *js, Item *jp, CHARSET_INFO *cs,
               LEX_CSTRING func_name, bool allow_wildcard);
};


class Item_func_json_valid: public Item_bool_func
{
protected:
  String tmp_value;

public:
  Item_func_json_valid(THD *thd, Item *json) : Item_bool_func(thd, json) {}
  bool val_bool() override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_valid") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    if (Item_bool_func::fix_length_and_dec(thd))
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
  enum Functype functype() const override { return JSON_VALID_FUNC; }

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_valid>(thd, this); }
};


class Item_func_json_equals: public Item_bool_func
{
public:
  Item_func_json_equals(THD *thd, Item *a, Item *b):
    Item_bool_func(thd, a, b) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_equals") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_equals>(thd, this); }
  bool val_bool() override;
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
  bool fix_length_and_dec(THD *thd) override;
  bool val_bool() override;

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_exists>(thd, this); }
};


class Item_json_func: public Item_str_func
{
protected:
  Json_result_marks m_marks;
  /*
    Return a document as this function's answer rather than reading it
    again to find out what it is - out of line, where the rule it stands
    on is written.
  */
  String *return_json(String *to, const String *from);
public:
  Item_json_func(THD *thd)
   :Item_str_func(thd) { }
  Item_json_func(THD *thd, Item *a)
   :Item_str_func(thd, a) { }
  Item_json_func(THD *thd, Item *a, Item *b)
   :Item_str_func(thd, a, b) { }
  Item_json_func(THD *thd, List<Item> &list)
   :Item_str_func(thd, list) { }
  const Type_handler *type_handler() const override
  {
    return Type_handler_json_common::json_type_handler(max_length);
  }
  bool is_valid_json() const override { return m_marks.valid(); }
  bool is_nice_json() const override { return m_marks.nice(); }
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
  bool fix_length_and_dec(THD *thd) override ;
  String *val_str(String *to) override
  {
    null_value= Json_path_extractor::extract(to, args[0], args[1],
                                             collation.collation, func_name_cstring(), false);
    return null_value ? NULL : to;
  }
  bool check_and_get_value(Json_engine_scan *je,
                           String *res, int *error) override
  {
    return je->check_and_get_value_scalar(res, error);
  }

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *to) override
  {
    m_marks.clear();
    null_value= Json_path_extractor::extract(to, args[0], args[1],
                                             collation.collation, func_name_cstring(), true);
    if (null_value)
      return NULL;
    /*
      The piece is copied out with whatever spacing the document it came
      from was written with, so it is formatted the loose way exactly when
      that document was.  Cutting it out cannot change that: the loose
      form writes the same punctuation wherever a value sits, so a value
      inside a document is written there the way it would be written on
      its own, and the two ends of the cut are the two ends of the value
      with no spacing of the document's left on either side.
    */
    m_marks.set(to, true, args[0]->is_nice_json());
    return to;
  }
  bool check_and_get_value(Json_engine_scan *je,
                           String *res, int *error) override
  {
    return je->check_and_get_value_complex(res, error);
  }
  /*
    Returns a slice of the searched document, delimited by the two ends
    of a value the scanner has just parsed.  A fully parsed value is a
    document in its own right.
  */
  bool is_valid_json_static() const override { return true; }

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_quote>(thd, this); }
};


class Item_func_json_unquote: public Item_str_func
{
protected:
  String tmp_s;
  String *read_json(json_engine_t *je);
  String *return_as_is(String *str, String *js);
public:
  Item_func_json_unquote(THD *thd, Item *s): Item_str_func(thd, s) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_unquote") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_unquote>(thd, this); }
};


class Item_json_str_multipath: public Item_json_func
{
protected:
  json_path_with_flags *paths;
  String *tmp_paths;
private:
  /**
    Number of paths returned by calling virtual method get_n_paths() and
    remembered inside fix_fields(). It is used by the virtual destructor
    ~Item_json_str_multipath() to iterate along allocated memory chunks stored
    in the array tmp_paths and free every of them. The virtual method
    get_n_paths() can't be used for this goal from within virtual destructor.
    We could get rid of the virtual method get_n_paths() and store the number
    of paths directly in the constructor of classes derived from the class
    Item_json_str_multipath but presence of the method get_n_paths() allows
    to check invariant that the number of arguments not changed between
    sequential runs of the same prepared statement that seems to be useful.
  */
  uint n_paths;
public:
  Item_json_str_multipath(THD *thd, List<Item> &list):
    Item_json_func(thd, list), paths(NULL), tmp_paths(0), n_paths(0) {}
  virtual ~Item_json_str_multipath();

  bool fix_fields(THD *thd, Item **ref) override;
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;
  longlong val_int() override;
  double val_real() override;
  my_decimal *val_decimal(my_decimal *) override;
  uint get_n_paths() const override { return arg_count - 1; }
  /*
    The values found are put together into a result of their own.  Each
    of them was read as a document on the way in and is written back out
    as one, so what holds them is all that can be wrong with the result
    - and that is the brackets, written only when several values can
    match.  In a character set that cannot encode those, the result is
    read back through json_nice() as it always was, and anything that
    will not read comes back as NULL.
  */
  bool is_valid_json_static() const override { return true; }

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  bool val_bool() override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  virtual ~Item_func_json_contains_path();
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_contains_path") };
    return name;
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec(THD *thd) override;
  bool val_bool() override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_array") };
    return name;
  }

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count/2; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_array_append") };
    return name;
  }
  /*
    Each of the six functions that EDIT a document returns a document
    or NULL, and for the same two reasons in all of them.

    Where the document it was given is_valid, and the result character
    set can represent the punctuation being written, the result is a
    document by construction: the parts retained were parsed as one on
    input, and what goes between them is written here.  Where either
    does not hold, the whole result is re-parsed through json_nice()
    before being returned, and anything that fails to parse gives
    NULL.
  */
  bool is_valid_json_static() const override { return true; }

protected:
  Item *shallow_copy(THD *thd) const override
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

protected:
  Item *shallow_copy(THD *thd) const override
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

protected:
  Item *shallow_copy(THD *thd) const override
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
  /*
    A document or nothing, for the reason given where
    Item_func_json_array_append declares the same.
  */
  bool is_valid_json_static() const override { return true; }

protected:
  Item *shallow_copy(THD *thd) const override
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

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_merge_patch>(thd, this); }
};


/*
  Reads its argument through in full and writes the document out again
  in a normal form, in utf8mb4 whatever the argument arrived in.  What
  it writes is the compact formatting, so it is never in the loose form.
*/
class Item_func_json_normalize: public Item_json_func
{
public:
  Item_func_json_normalize(THD *thd, Item *a):
    Item_json_func(thd, a) {}
  String *val_str(String *) override;
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_normalize") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  /*
    Written out afresh from a document read through in full, and always
    in utf8mb4, which can encode everything written into it whatever the
    argument arrived in.
  */
  bool is_valid_json_static() const override { return true; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_normalize>(thd, this); }
};


class Item_func_json_length: public Item_long_func
{
  bool check_arguments() const override
  {
    const LEX_CSTRING name= func_name_cstring();
    if (arg_count == 0 || arg_count > 2)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name.str);
      return true;
    }
    return args[0]->check_type_can_return_text(name) ||
      (arg_count > 1 && args[1]->check_type_general_purpose_string(name));
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
  bool fix_length_and_dec(THD *thd) override;
  longlong val_int() override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override { max_length= 10; return FALSE; }
  longlong val_int() override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count/2; }
  /*
    A document or nothing, for the reason given where
    Item_func_json_array_append declares the same.
  */
  bool is_valid_json_static() const override { return true; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING json_set=    {STRING_WITH_LEN("json_set") };
    static LEX_CSTRING json_insert= {STRING_WITH_LEN("json_insert") };
    static LEX_CSTRING json_replace= {STRING_WITH_LEN("json_replace") };
    return (mode_insert ?
            (mode_replace ? json_set : json_insert) : json_replace);
  }

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_insert>(thd, this); }
};


class Item_func_json_remove: public Item_json_str_multipath
{
protected:
  String tmp_js;
public:
  Item_func_json_remove(THD *thd, List<Item> &list):
    Item_json_str_multipath(thd, list) {}
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count - 1; }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_remove") };
    return name;
  }
  /*
    A document or nothing, for the reason given where
    Item_func_json_array_append declares the same.
  */
  bool is_valid_json_static() const override { return true; }

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *) override;
  uint get_n_paths() const override { return arg_count > 4 ? arg_count - 4 : 0; }
  /*
    A path is a JSON string, and every path returned here is built from
    pieces of a document that has just been parsed.  Several of them go
    inside brackets, which needs a character set that can represent a
    bracket - and a document in a character set that cannot is a
    scalar, there being no way to write a container in it, so it holds
    one value and yields one path.
  */
  bool is_valid_json_static() const override { return true; }

protected:
  Item *shallow_copy(THD *thd) const override
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
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *str) override;
  String *val_json(String *str) override;

protected:
  Item *shallow_copy(THD *thd) const override
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
  /*
    A row of this group could not be written out at all, the buffer
    having failed to grow.  Neither a row whose value does not parse as
    JSON nor one holding a character no document can carry is this:
    the first is written out as it stands and the second is dropped
    where it always was, both with a note.  Reset for each group by
    clear().
  */
  bool m_bad_element;
  /*
    The brackets have been put on already.  Nothing says how often the
    result of a group is asked for, and the buffer they go into belongs
    to this item and outlives the asking, so putting them on once per
    call would put on one pair per call.  Reset for each group by
    clear().
  */
  bool m_closed;
  /*
    Whether everything that went into this group leaves the array around
    it answerable for.  Cleared by a value that did not read as JSON, or
    one holding a character that could not be written, which is dropped
    and leaves the separator either side of it with nothing between
    them.  Reset for each group by clear(), the same as above, and kept
    apart from it because they say different things - that one means the
    group is missing a row, this one that the group is complete and
    still not a document.
  */
  bool m_elements_valid;
  Json_result_marks m_marks;
public:
  String m_tmp_json; /* Used in get_str_from_*.. */
  Item_func_json_arrayagg(THD *thd, Name_resolution_context *context_arg,
                          bool is_distinct, List<Item> *is_select,
                          const SQL_I_List<ORDER> &is_order, String *is_separator,
                          bool limit_clause, Item *row_limit, Item *offset_limit):
      Item_func_group_concat(thd, context_arg, is_distinct, is_select, is_order,
                             is_separator, limit_clause, row_limit, offset_limit),
      m_bad_element(false), m_closed(false), m_elements_valid(true)
  {
  }
  /*
    A copy is not fixed again, so anything fix_fields() settled has to
    be settled here too.  The buffer's character set is one of those:
    left at its default the copy would read the values it is given as
    bytes rather than as the characters they are, and say they were
    not JSON.
  */
  Item_func_json_arrayagg(THD *thd, Item_func_json_arrayagg *item) :
    Item_func_group_concat(thd, item), m_bad_element(false),
    m_closed(false), m_elements_valid(true)
  {
    m_tmp_json.set_charset(collation.collation);
  }
  const Type_handler *type_handler() const override
  {
    return Type_handler_json_common::json_type_handler_sum(this);
  }
  bool is_valid_json() const override { return m_marks.valid(); }
  bool is_nice_json() const override { return m_marks.nice(); }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_arrayagg(") };
    return name;
  }
  bool fix_fields(THD *thd, Item **ref) override;
  enum Sumfunctype sum_func() const override { return JSON_ARRAYAGG_FUNC; }

  void clear() override;
  String* val_str(String *str) override;

  Item *copy_or_same(THD* thd) override;

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_arrayagg>(thd, this); }
};


class Item_func_json_objectagg : public Item_sum
{
  String result;
  /*
    A pair of this group could not be written out in full, the buffer
    having failed to grow part way through it.  Reset for each group by
    clear().
  */
  bool m_bad_pair;
  /*
    The closing brace has been written already.  Nothing says how often
    the result of a group is asked for, and the buffer it goes into is
    this item's own and outlives the asking, so writing it once per call
    would write one brace per call.  Reset for each group by clear().
  */
  bool m_closed;
  /*
    Whether everything that went into this group leaves the object
    around it answerable for.  Cleared by a key or a value holding a
    character that could not be written, which goes in as however much
    of it fitted, or by a value that did not read as JSON.  Reset for
    each group by clear(), and kept apart from the flag above for the
    same reason as in the sister aggregate - one means a pair is
    missing, this one that the pairs are all there and still do not make
    a document.
  */
  bool m_pairs_valid;
  Json_result_marks m_marks;
public:
  /*
    The opening brace is not written here.  This runs while the
    expression is being parsed, before there is a character set to write
    it in, and a brace put down now would be one byte wide however wide a
    character of the result turns out to be.  clear() writes it instead,
    once per group and once the width is known.
  */
  Item_func_json_objectagg(THD *thd, Item *key, Item *value) :
    Item_sum(thd, key, value), m_bad_pair(false), m_closed(false),
    m_pairs_valid(true)
  {
    quick_group= FALSE;
  }

  Item_func_json_objectagg(THD *thd, Item_func_json_objectagg *item);
  void cleanup() override;

  enum Sumfunctype sum_func () const override { return JSON_OBJECTAGG_FUNC;}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_objectagg") };
    return name;
  }
  const Type_handler *type_handler() const override
  {
    return Type_handler_json_common::json_type_handler_sum(this);
  }
  bool is_valid_json() const override { return m_marks.valid(); }
  bool is_nice_json() const override { return m_marks.nice(); }
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

protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_objectagg>(thd, this); }
};

extern bool is_json_type(const Item *item);

class Item_func_json_overlaps: public Item_bool_func
{
  String tmp_js;
  bool a2_constant, a2_parsed;
  String tmp_val, *val;
public:
  Item_func_json_overlaps(THD *thd, Item *a, Item *b):
    Item_bool_func(thd, a, b) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("json_overlaps") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override;
  bool val_bool() override;
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_json_overlaps>(thd, this); }
};

#endif /* ITEM_JSONFUNC_INCLUDED */
