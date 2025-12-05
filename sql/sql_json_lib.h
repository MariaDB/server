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

bool read_ha_rows_and_check_limit(json_engine_t *je, const char *read_elem_key,
                                  String *err_buf, ha_rows &value,
                                  ha_rows LIMIT_VAL,
                                  const char *limit_val_type,
                                  bool unescape_required);

bool read_string(THD *thd, json_engine_t *je, const char *read_elem_key,
                 String *err_buf, char *&value);

bool read_double(json_engine_t *je, const char *read_elem_key, String *err_buf,
                 double &value);

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
  THD *thd;

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

/*
  A place holder to keep track of the field, and its corresponding
  Read_value class to be used for fetching the field value from Json.
  It tracks whether the value was successfully read from the Json or not.
 */
class Read_named_member
{
public:
  const char *name;
  Read_value &&value;
  const bool is_optional;

  bool value_assigned= false;
};

int read_all_elements(json_engine_t *je, Read_named_member *arr,
                      String *err_buf);

#endif
