/*
   Copyright (c) 2025, MariaDB
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

#ifndef SQL_JSON_LIB
#define SQL_JSON_LIB

#include "my_global.h"
#include "json_lib.h"
#include "sql_string.h"
#include "table.h"

/*
  A syntax sugar interface to json_string_t
*/
class Json_string
{
  json_string_t str;

public:
  explicit Json_string(const char *name)
  {
    json_string_set_str(&str, (const uchar *) name,
                        (const uchar *) name + strlen(name));
    json_string_set_cs(&str, system_charset_info);
  }
  json_string_t *get() { return &str; }
};

/*
  This [partially] saves the JSON parser state and then can rollback the parser
  to it.
  The goal of this is to be able to make multiple json_key_matches() calls:
    Json_saved_parser_state save(je);
    if (json_key_matches(je, KEY_NAME_1)) {
      ...
      return;
    }
    save.restore_to(je);
    if (json_key_matches(je, KEY_NAME_2)) {
      ...
    }
  This allows one to parse JSON objects where [optional] members come in any
  order.
*/
class Json_saved_parser_state
{
  const uchar *c_str;
  my_wc_t c_next;
  int state;

public:
  explicit Json_saved_parser_state(const json_engine_t *je)
      : c_str(je->s.c_str), c_next(je->s.c_next), state(je->state)
  {
  }
  void restore_to(json_engine_t *je)
  {
    je->s.c_str= c_str;
    je->s.c_next= c_next;
    je->state= state;
  }
};

/*
  @brief
    Un-escape a JSON string and save it into *out.
*/
bool json_unescape_to_string(const char *val, int val_len, String *out);

/*
  @brief
    Escape a JSON string and save it into *out.
*/
int json_escape_to_string(const String *str, String *out);

namespace json_reader {
class Read_value;
};

/*
  Description of a JSON object member that is to be read by json_read_object().
  Intended usage:

    char *var1;
    int var2;
    Read_named_member memb[]= {
        {"member1", Read_string(thd->mem_root, &var1), false},
        {"member2", Read_double(&var2), false},
        {NULL,      Read_double(NULL), true}
    };
    json_read_object(je, memb, err);
*/

class Read_named_member
{
public:
  const char *name;       /* JSON object name */
  /*
    Reader object holding the datatype and place for the value.
    It's an lvalue reference so we can have both inherited classes
    and refer to unnamed objects on the stack.
  */
  json_reader::Read_value &&value;

  const bool is_optional; /* Can this member be omitted in JSON? */

  bool value_assigned= false; /* Filled and checked by json_read_object() */
};

/* Read an object from JSON according to description in *members */
int json_read_object(json_engine_t *je, Read_named_member *members,
                     String *err_buf);

namespace json_reader { /* Things to use with Read_named_member */

bool read_string(THD *thd, json_engine_t *je, const char *read_elem_key,
                 String *err_buf, char *&value);

bool read_double(json_engine_t *je, const char *read_elem_key, String *err_buf,
                 double &value);

bool read_ha_rows_and_check_limit(json_engine_t *je, const char *read_elem_key,
                                  String *err_buf, ha_rows &value,
                                  ha_rows LIMIT_VAL,
                                  const char *limit_val_type,
                                  bool unescape_required);

/*
  Interface to read a value with value_name from Json,
  while in the process of parsing the Json document
*/
class Read_value
{
public:
  virtual int read_value(json_engine_t *je, const char *value_name,
                         String *err_buf)= 0;
  virtual ~Read_value(){};
};

class Read_string : public Read_value
{
  char **ptr;
  THD *thd; /* The string will be allocated on thd->mem_root */
public:
  Read_string(THD *thd_arg, char **ptr_arg) : ptr(ptr_arg), thd(thd_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    return read_string(thd, je, value_name, err_buf, *ptr);
  }
};

class Read_double : public Read_value
{
  double *ptr;

public:
  Read_double(double *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    return read_double(je, value_name, err_buf, *ptr);
  }
};

template <typename T, ha_rows MAX_VALUE>
class Read_non_neg_integer : public Read_value
{
  T *ptr;

public:
  Read_non_neg_integer(T *ptr_arg) : ptr(ptr_arg) {}
  int read_value(json_engine_t *je, const char *value_name,
                 String *err_buf) override
  {
    ha_rows temp_val;
    const char *type= "";

    switch (MAX_VALUE)
    {
    case 1:
      type= "boolean";
      break;
    case UINT_MAX:
      type= "unsigned int";
      break;
    case LONGLONG_MAX:
      type= "longlong";
      break;
    case ULONGLONG_MAX:
      type= "unsigned longlong";
      break;
    default:
      err_buf->append(STRING_WITH_LEN("wrong MAX_VALUE provided i.e.: "));
      err_buf->q_append_int64(MAX_VALUE);
      return 1;
    }

    if (read_ha_rows_and_check_limit(je, value_name, err_buf, temp_val,
                                     MAX_VALUE, type, false))
    {
      return 1;
    }

    switch (MAX_VALUE)
    {
    case 1:
      *ptr= temp_val == 1;
      break;
    case UINT_MAX:
    case LONGLONG_MAX:
    case ULONGLONG_MAX:
      *ptr= (T) temp_val;
    default:;
    }
    return 0;
  }
};

}; /* namespace json_reader */

#endif
