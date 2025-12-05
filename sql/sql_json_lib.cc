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

#include "sql_json_lib.h"
#include "mysql.h"
#include "sql_select.h"

/*
  check if the given read_elem_key can be read from the json_engine.
  if not, fill the err_buf with an error message
  @return
    false  OK
    true  Parse Error
*/
static bool check_reading_of_elem_key(json_engine_t *je,
                                      const char *read_elem_key,
                                      String *err_buf)
{
  if (json_read_value(je))
  {
    err_buf->append(STRING_WITH_LEN("error reading "));
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" value"));
    return true;
  }
  return false;
}

/*
  parse the json to read and put a double into the argument value.
  fill in the err_buf if any error has occurred during parsing
  @return
    false  OK
    true  Parse Error
*/
bool read_double(json_engine_t *je, const char *read_elem_key, String *err_buf,
                 double &value)
{
  if (check_reading_of_elem_key(je, read_elem_key, err_buf))
    return true;

  const char *size= (const char *) je->value_begin;
  char *size_end= (char *) je->value_end;
  int conv_err;
  value= my_strtod(size, &size_end, &conv_err);
  if (conv_err)
  {
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" member must be a floating point value"));
    return true;
  }
  return false;
}

/*
  parse the json to read and put a string into the argument value.
  fill in the err_buf if any error has occurred during parsing
  @return
    false  OK
    true  Parse Error
*/
bool read_string(THD *thd, json_engine_t *je, const char *read_elem_key,
                 String *err_buf, char *&value)
{
  if (check_reading_of_elem_key(je, read_elem_key, err_buf))
    return true;

  StringBuffer<128> val_buf;
  if (json_unescape_to_string((const char *) je->value, je->value_len,
                              &val_buf))
  {
    err_buf->append(STRING_WITH_LEN("un-escaping error of "));
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" element"));
    return true;
  }

  value= strdup_root(thd->mem_root, val_buf.c_ptr_safe());
  return false;
}

/*
  parse the json to read and put a ha_rows into the argument value.
  fill in the err_buf if any error has occurred either during parsing,
  or string to numeric conversion or during the limit check.
  Remove the quotes around the value if unescape_required is enabled.
  @return
    false  OK
    true  Error
*/
bool read_ha_rows_and_check_limit(json_engine_t *je, const char *read_elem_key,
                                  String *err_buf, ha_rows &value,
                                  ha_rows LIMIT_VAL,
                                  const char *limit_val_type,
                                  bool unescape_required)
{
  if (check_reading_of_elem_key(je, read_elem_key, err_buf))
    return true;

  StringBuffer<32> size_buf;
  char *size;
  char *size_end;
  int conv_err;
  if (unescape_required)
  {
    if (json_unescape_to_string((const char *) je->value, je->value_len,
                                &size_buf))
    {
      err_buf->append(
          STRING_WITH_LEN("un-escaping error of rec_per_key element"));
      return true;
    }
    size= size_buf.c_ptr_safe();
    size_end= size + strlen(size);
  }
  else
  {
    size= (char *) je->value_begin;
    size_end= (char *) je->value_end;
  }

  value= my_strtoll10(size, &size_end, &conv_err);

  if (conv_err)
  {
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" member must be a numeric value"));
    return true;
  }
  else if (value > LIMIT_VAL)
  {
    err_buf->append(read_elem_key, strlen(read_elem_key));
    err_buf->append(STRING_WITH_LEN(" is out of range of "));
    err_buf->append(limit_val_type, strlen(limit_val_type));
    return true;
  }
  return false;
}

/*
  function to read all the registered members in Read_named_member array
  from json, and check if the value was assigned to them or not.
  If any of the mandatory fields are not assigned a value, then the
  function returns an error.
  @return
    0  OK
    1  An Error occured
*/
int read_all_elements(json_engine_t *je, Read_named_member *arr,
                      String *err_buf)
{
  int rc;
  while (!(rc= json_scan_next(je)) && je->state != JST_OBJ_END)
  {
    Json_saved_parser_state save1(je);
    for (Read_named_member *memb= arr; memb->name; memb++)
    {
      Json_string js_name(memb->name);
      if (json_key_matches(je, js_name.get()))
      {
        if (memb->value.read_value(je, memb->name, err_buf))
          return 1;
        memb->value_assigned= true;
        break;
      }
      save1.restore_to(je);
    }
  }

  /* Check if all members got values */
  for (Read_named_member *memb= arr; memb->name; memb++)
  {
    if (!memb->is_optional && !memb->value_assigned)
    {
      err_buf->append(STRING_WITH_LEN("\""));
      err_buf->append(memb->name, strlen(memb->name));
      err_buf->append(STRING_WITH_LEN("\" element not present"));
      return 1;
    }
  }
  return 0;
}
