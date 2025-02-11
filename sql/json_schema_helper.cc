/* Copyright (c) 2016, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include "mariadb.h"
#include "sql_class.h"
#include "sql_parse.h" // For check_stack_overrun
#include <m_string.h>
#include "json_schema_helper.h"


bool json_key_equals(const char* key,  LEX_CSTRING val, int key_len)
{
  return (size_t)key_len == val.length && !strncmp(key, val.str, key_len);
}

bool json_assign_type(uint *curr_type, json_engine_t *je)
{
  const char* curr_value= (const char*)je->value;
  int len= je->value_len;

  if (json_key_equals(curr_value, { STRING_WITH_LEN("number") }, len))
      *curr_type|= (1 << JSON_VALUE_NUMBER);
  else if(json_key_equals(curr_value, { STRING_WITH_LEN("string") }, len))
      *curr_type|= (1 << JSON_VALUE_STRING);
  else if(json_key_equals(curr_value, { STRING_WITH_LEN("array") }, len))
      *curr_type|= (1 << JSON_VALUE_ARRAY);
  else if(json_key_equals(curr_value, { STRING_WITH_LEN("object") }, len))
      *curr_type|= (1 << JSON_VALUE_OBJECT);
  else if (json_key_equals(curr_value, { STRING_WITH_LEN("boolean") }, len))
      *curr_type|= ((1 << JSON_VALUE_TRUE) | (1 << JSON_VALUE_FALSE));
  else if (json_key_equals(curr_value, { STRING_WITH_LEN("null") }, len))
      *curr_type|= (1 << JSON_VALUE_NULL);
  else
  {
    my_error(ER_JSON_INVALID_VALUE_FOR_KEYWORD, MYF(0), "type");
    return true;
  }
  return false;
}

const uchar *get_key_name(const void *key_name_, size_t *length, my_bool)
{
  auto key_name= static_cast<const char *>(key_name_);
  *length= strlen(key_name);
  return reinterpret_cast<const uchar *>(key_name);
}

void json_get_normalized_string(json_engine_t *je, String *res,
                                int *error)
{
  char *val_begin= (char*)je->value, *val_end= NULL;
  String val("",0,je->s.cs);
  DYNAMIC_STRING a_res;

  if (init_dynamic_string(&a_res, NULL, 0, 0))
    goto error;

  if (!json_value_scalar(je))
  {
    if (json_skip_level(je))
      goto error;
  }

  val_end= json_value_scalar(je) ? val_begin+je->value_len :
                                   (char *)je->s.c_str;
  val.set((const char*)val_begin, val_end-val_begin, je->s.cs);

  if (je->value_type == JSON_VALUE_NUMBER ||
      je->value_type == JSON_VALUE_ARRAY ||
      je->value_type == JSON_VALUE_OBJECT)
  {
    if (json_normalize(&a_res, (const char*)val.ptr(),
                       val_end-val_begin, je->s.cs))
      goto error;
  }
  else if(je->value_type == JSON_VALUE_STRING)
  {
    strncpy((char*)a_res.str, val.ptr(), je->value_len);
    a_res.length= je->value_len;
  }

  res->append(a_res.str, a_res.length, je->s.cs);
  *error= 0;

  error:
  dynstr_free(&a_res);

  return;
}
