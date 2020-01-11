/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#pragma once

#include "../grn.h"

#include "ts_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------
 * Byte.
 */

/* grn_ts_byte_is_decimal() returns whether or not a byte is decimal. */
grn_ts_bool grn_ts_byte_is_decimal(uint8_t byte);

/*
 * grn_ts_byte_is_name_char() returns whether or not a byte is allowed as a
 * part of a name.
 */
grn_ts_bool grn_ts_byte_is_name_char(uint8_t byte);

/*-------------------------------------------------------------
 * String.
 */

typedef struct {
  const char *ptr; /* The starting address. */
  size_t size;     /* The size in bytes. */
} grn_ts_str;

/* grn_ts_str_has_prefix() returns whether or not str starts with prefix. */
grn_ts_bool grn_ts_str_starts_with(grn_ts_str str, grn_ts_str prefix);

/* grn_ts_str_trim_left() returns a string without leading white-spaces. */
grn_ts_str grn_ts_str_trim_left(grn_ts_str str);

/*
 * grn_ts_str_trim_score_assignment() returns a string without leading
 * white-spaces and an assignment to _score. If `str` does not start with
 * an assignment, this function returns `grn_ts_str_trim_left(str)`.
 */
grn_ts_str grn_ts_str_trim_score_assignment(grn_ts_str str);

/*
 * grn_ts_str_has_number_prefix() returns whether or not a string starts with a
 * number or not.
 */
grn_ts_bool grn_ts_str_has_number_prefix(grn_ts_str str);

/*
 * grn_ts_str_is_name_prefix() returns whether or not a string is valid as a
 * name prefix. Note that an empty string is a name prefix.
 */
grn_ts_bool grn_ts_str_is_name_prefix(grn_ts_str str);

/*
 * grn_ts_str_is_name() returns whether or not a string is valid as a name.
 * Note that an empty string is invalid as a name.
 */
grn_ts_bool grn_ts_str_is_name(grn_ts_str str);

/* grn_ts_str_is_true() returns str == "true". */
grn_ts_bool grn_ts_str_is_true(grn_ts_str str);

/* grn_ts_str_is_false() returns str == "false". */
grn_ts_bool grn_ts_str_is_false(grn_ts_str str);

/* grn_ts_str_is_bool() returns (str == "true") || (str == "false"). */
grn_ts_bool grn_ts_str_is_bool(grn_ts_str str);

/* grn_ts_str_is_id_name() returns str == "_id". */
grn_ts_bool grn_ts_str_is_id_name(grn_ts_str str);

/* grn_ts_str_is_score_name() returns str == "_score". */
grn_ts_bool grn_ts_str_is_score_name(grn_ts_str str);

/* grn_ts_str_is_key_name() returns str == "_key". */
grn_ts_bool grn_ts_str_is_key_name(grn_ts_str str);

/* grn_ts_str_is_value_name() returns str == "_value". */
grn_ts_bool grn_ts_str_is_value_name(grn_ts_str str);

#ifdef __cplusplus
}
#endif

