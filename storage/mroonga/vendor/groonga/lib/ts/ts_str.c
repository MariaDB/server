/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "ts_str.h"

#include <ctype.h>
#include <string.h>

/*-------------------------------------------------------------
 * Byte.
 */

grn_ts_bool
grn_ts_byte_is_decimal(uint8_t byte)
{
  return (byte >= '0') && (byte <= '9');
}

grn_ts_bool
grn_ts_byte_is_name_char(uint8_t byte)
{
  /*
   * Note: A table name allows '#', '@' and '-'.
   * http://groonga.org/docs/reference/commands/table_create.html#name
   */
  if (((byte >= '0') && (byte <= '9')) || ((byte >= 'A') && (byte <= 'Z')) ||
      ((byte >= 'a') && (byte <= 'z')) || (byte == '_')) {
    return GRN_TRUE;
  }
  return GRN_FALSE;
}

/*-------------------------------------------------------------
 * String.
 */

grn_ts_bool
grn_ts_str_starts_with(grn_ts_str str, grn_ts_str prefix)
{
  if (str.size < prefix.size) {
    return GRN_FALSE;
  }
  return !memcmp(str.ptr, prefix.ptr, prefix.size);
}

grn_ts_str
grn_ts_str_trim_left(grn_ts_str str)
{
  size_t i;
  for (i = 0; i < str.size; i++) {
    if (!isspace((uint8_t)str.ptr[i])) {
      break;
    }
  }
  str.ptr += i;
  str.size -= i;
  return str;
}

grn_ts_str
grn_ts_str_trim_score_assignment(grn_ts_str str)
{
  grn_ts_str rest;
  str = grn_ts_str_trim_left(str);
  if (!grn_ts_str_starts_with(str, (grn_ts_str){ "_score", 6 })) {
    return str;
  }
  rest.ptr = str.ptr + 6;
  rest.size = str.size - 6;
  rest = grn_ts_str_trim_left(rest);
  if (!rest.size || (rest.ptr[0] != '=') ||
      ((rest.size >= 2) && (rest.ptr[1] == '='))) {
    return str;
  }
  rest.ptr++;
  rest.size--;
  return grn_ts_str_trim_left(rest);
}

grn_ts_bool
grn_ts_str_has_number_prefix(grn_ts_str str)
{
  if (!str.size) {
    return GRN_FALSE;
  }
  if (grn_ts_byte_is_decimal(str.ptr[0])) {
    return GRN_TRUE;
  }
  if (str.size == 1) {
    return GRN_FALSE;
  }
  switch (str.ptr[0]) {
    case '+': case '-': {
      if (grn_ts_byte_is_decimal(str.ptr[1])) {
        return GRN_TRUE;
      }
      if (str.size == 2) {
        return GRN_FALSE;
      }
      return (str.ptr[1] == '.') && grn_ts_byte_is_decimal(str.ptr[2]);
    }
    case '.': {
      return grn_ts_byte_is_decimal(str.ptr[1]);
    }
    default: {
      return GRN_FALSE;
    }
  }
}

grn_ts_bool
grn_ts_str_is_name_prefix(grn_ts_str str)
{
  size_t i;
  for (i = 0; i < str.size; i++) {
    if (!grn_ts_byte_is_name_char(str.ptr[i])) {
      return GRN_FALSE;
    }
  }
  return GRN_TRUE;
}

grn_ts_bool
grn_ts_str_is_name(grn_ts_str str)
{
  if (!str.size) {
    return GRN_FALSE;
  }
  return grn_ts_str_is_name_prefix(str);
}

grn_ts_bool
grn_ts_str_is_true(grn_ts_str str)
{
  return (str.size == 4) && !memcmp(str.ptr, "true", 4);
}

grn_ts_bool
grn_ts_str_is_false(grn_ts_str str)
{
  return (str.size == 5) && !memcmp(str.ptr, "false", 5);
}

grn_ts_bool
grn_ts_str_is_bool(grn_ts_str str)
{
  return grn_ts_str_is_true(str) || grn_ts_str_is_false(str);
}

grn_ts_bool
grn_ts_str_is_id_name(grn_ts_str str)
{
  return (str.size == GRN_COLUMN_NAME_ID_LEN) &&
         !memcmp(str.ptr, GRN_COLUMN_NAME_ID, GRN_COLUMN_NAME_ID_LEN);
}

grn_ts_bool
grn_ts_str_is_score_name(grn_ts_str str)
{
  return (str.size == GRN_COLUMN_NAME_SCORE_LEN) &&
         !memcmp(str.ptr, GRN_COLUMN_NAME_SCORE, GRN_COLUMN_NAME_SCORE_LEN);
}

grn_ts_bool
grn_ts_str_is_key_name(grn_ts_str str)
{
  return (str.size == GRN_COLUMN_NAME_KEY_LEN) &&
         !memcmp(str.ptr, GRN_COLUMN_NAME_KEY, GRN_COLUMN_NAME_KEY_LEN);
}

grn_ts_bool
grn_ts_str_is_value_name(grn_ts_str str)
{
  return (str.size == GRN_COLUMN_NAME_VALUE_LEN) &&
         !memcmp(str.ptr, GRN_COLUMN_NAME_VALUE, GRN_COLUMN_NAME_VALUE_LEN);
}
