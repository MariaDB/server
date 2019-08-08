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

#include "ts_expr_node.h"

#include <math.h>
#include <string.h>

#include "../grn_ctx.h"
#include "../grn_dat.h"
#include "../grn_db.h"
#include "../grn_geo.h"
#include "../grn_hash.h"
#include "../grn_pat.h"
#include "../grn_store.h"

#include "ts_log.h"
#include "ts_str.h"
#include "ts_util.h"

/*-------------------------------------------------------------
 * Built-in data kinds.
 */

/* grn_ts_bool_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_bool_is_valid(grn_ts_bool value)
{
  return GRN_TRUE;
}

/* grn_ts_int_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_int_is_valid(grn_ts_int value)
{
  return GRN_TRUE;
}

/* grn_ts_float_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_float_is_valid(grn_ts_float value)
{
  return isfinite(value);
}

/* grn_ts_time_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_time_is_valid(grn_ts_time value)
{
  return GRN_TRUE;
}

/* grn_ts_text_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_text_is_valid(grn_ts_text value)
{
  return value.ptr || !value.size;
}

/* grn_ts_geo_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_geo_is_valid(grn_ts_geo value)
{
  return ((value.latitude >= GRN_GEO_MIN_LATITUDE) &&
          (value.latitude <= GRN_GEO_MAX_LATITUDE)) &&
         ((value.longitude >= GRN_GEO_MIN_LONGITUDE) &&
          (value.longitude <= GRN_GEO_MAX_LONGITUDE));
}

#define GRN_TS_VECTOR_IS_VALID(type)\
  if (value.size) {\
    size_t i;\
    if (!value.ptr) {\
      return GRN_FALSE;\
    }\
    for (i = 0; i < value.size; i++) {\
      if (!grn_ts_ ## type ## _is_valid(value.ptr[i])) {\
        return GRN_FALSE;\
      }\
    }\
  }\
  return GRN_TRUE;
/* grn_ts_bool_vector_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_bool_vector_is_valid(grn_ts_bool_vector value)
{
  GRN_TS_VECTOR_IS_VALID(bool)
}

/* grn_ts_int_vector_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_int_vector_is_valid(grn_ts_int_vector value)
{
  GRN_TS_VECTOR_IS_VALID(int)
}

/* grn_ts_float_vector_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_float_vector_is_valid(grn_ts_float_vector value)
{
  GRN_TS_VECTOR_IS_VALID(float)
}

/* grn_ts_time_vector_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_time_vector_is_valid(grn_ts_time_vector value)
{
  GRN_TS_VECTOR_IS_VALID(time)
}

/* grn_ts_text_vector_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_text_vector_is_valid(grn_ts_text_vector value)
{
  GRN_TS_VECTOR_IS_VALID(text)
}

/* grn_ts_geo_vector_is_valid() returns whether a value is valid or not. */
inline static grn_ts_bool
grn_ts_geo_vector_is_valid(grn_ts_geo_vector value)
{
  GRN_TS_VECTOR_IS_VALID(geo)
}
#undef GRN_TS_VECTOR_IS_VALID

/* grn_ts_bool_zero() returns a zero. */
inline static grn_ts_bool
grn_ts_bool_zero(void)
{
  return GRN_FALSE;
}

/* grn_ts_int_zero() returns a zero. */
inline static grn_ts_int
grn_ts_int_zero(void)
{
  return 0;
}

/* grn_ts_float_zero() returns a zero. */
inline static grn_ts_float
grn_ts_float_zero(void)
{
  return 0.0;
}

/* grn_ts_time_zero() returns a zero. */
inline static grn_ts_time
grn_ts_time_zero(void)
{
  return 0;
}

/* grn_ts_text_zero() returns a zero. */
inline static grn_ts_text
grn_ts_text_zero(void)
{
  return (grn_ts_text){ NULL, 0 };
}

/* grn_ts_geo_zero() returns a zero. */
inline static grn_ts_geo
grn_ts_geo_zero(void)
{
  return (grn_ts_geo){ 0, 0 };
}

/* grn_ts_ref_zero() returns a zero. */
inline static grn_ts_ref
grn_ts_ref_zero(void)
{
  return (grn_ts_ref){ 0, 0.0 };
}

/* grn_ts_data_type_to_kind() returns a kind associated with a type. */
static grn_ts_data_kind
grn_ts_data_type_to_kind(grn_ts_data_type type)
{
  switch (type) {
    case GRN_DB_VOID: {
      return GRN_TS_VOID;
    }
    case GRN_DB_BOOL: {
      return GRN_TS_BOOL;
    }
    case GRN_DB_INT8:
    case GRN_DB_INT16:
    case GRN_DB_INT32:
    case GRN_DB_INT64:
    case GRN_DB_UINT8:
    case GRN_DB_UINT16:
    case GRN_DB_UINT32:
    case GRN_DB_UINT64: {
      return GRN_TS_INT;
    }
    case GRN_DB_FLOAT: {
      return GRN_TS_FLOAT;
    }
    case GRN_DB_TIME: {
      return GRN_TS_TIME;
    }
    case GRN_DB_SHORT_TEXT:
    case GRN_DB_TEXT:
    case GRN_DB_LONG_TEXT: {
      return GRN_TS_TEXT;
    }
    case GRN_DB_TOKYO_GEO_POINT:
    case GRN_DB_WGS84_GEO_POINT: {
      return GRN_TS_GEO;
    }
    default: {
      return GRN_TS_REF;
    }
  }
}

/* grn_ts_data_kind_to_type() returns a type associated with a kind. */
static grn_ts_data_type
grn_ts_data_kind_to_type(grn_ts_data_kind kind)
{
  switch (kind & ~GRN_TS_VECTOR_FLAG) {
    case GRN_TS_BOOL: {
      return GRN_DB_BOOL;
    }
    case GRN_TS_INT: {
      return GRN_DB_INT64;
    }
    case GRN_TS_FLOAT: {
      return GRN_DB_FLOAT;
    }
    case GRN_TS_TIME: {
      return GRN_DB_TIME;
    }
    case GRN_TS_TEXT: {
      return GRN_DB_TEXT;
    }
    case GRN_TS_GEO: {
      /* GRN_DB_TOKYO_GEO_POINT or GRN_DB_WGS84_GEO_POINT. */
      return GRN_DB_VOID;
    }
    case GRN_TS_REF: {
      /*
       * grn_ts_data_kind does not have enough information to get a correct
       * table ID.
       */
      return GRN_DB_VOID;
    }
    default: {
      return GRN_DB_VOID;
    }
  }
}

/*-------------------------------------------------------------
 * Operators.
 */

/* grn_ts_op_logical_not_bool() returns !arg. */
inline static grn_ts_bool
grn_ts_op_logical_not_bool(grn_ts_bool arg)
{
  return !arg;
}

/* grn_ts_op_bitwise_not_bool() returns ~arg. */
inline static grn_ts_bool
grn_ts_op_bitwise_not_bool(grn_ts_bool arg)
{
  return !arg;
}

/* grn_ts_op_bitwise_not_int() returns ~arg. */
inline static grn_ts_int
grn_ts_op_bitwise_not_int(grn_ts_int arg)
{
  return ~arg;
}

/* grn_ts_op_positive_int() returns +arg. */
inline static grn_ts_int
grn_ts_op_positive_int(grn_ts_int arg)
{
  return arg;
}

/* grn_ts_op_positive_float() returns +arg. */
inline static grn_ts_float
grn_ts_op_positive_float(grn_ts_float arg)
{
  return arg;
}

/* grn_ts_op_negative_int() returns -arg. */
inline static grn_ts_int
grn_ts_op_negative_int(grn_ts_int arg)
{
  return -arg;
}

/* grn_ts_op_negative_float() returns -arg. */
inline static grn_ts_float
grn_ts_op_negative_float(grn_ts_float arg)
{
  return -arg;
}

/* grn_ts_op_float() returns (Float)arg. */
static grn_rc
grn_ts_op_float(grn_ctx *ctx, grn_ts_int arg, grn_ts_float *out)
{
  *out = (grn_ts_float)arg;
  return GRN_SUCCESS;
}

/* grn_ts_op_time() returns (Time)arg. */
static grn_rc
grn_ts_op_time(grn_ctx *ctx, grn_ts_text arg, grn_ts_time *out)
{
  grn_timeval value;
  grn_rc rc = grn_str2timeval(arg.ptr, arg.size, &value);
  if (rc != GRN_SUCCESS) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "grn_str2timeval failed");
  }
  *out = (grn_ts_time)((value.tv_sec * 1000000) + (value.tv_nsec / 1000));
  return GRN_SUCCESS;
}

/* grn_ts_op_bitwise_and_bool() returns lhs & rhs. */
inline static grn_ts_bool
grn_ts_op_bitwise_and_bool(grn_ts_bool lhs, grn_ts_bool rhs)
{
  return lhs & rhs;
}

/* grn_ts_op_bitwise_and_int() returns lhs & rhs. */
inline static grn_ts_int
grn_ts_op_bitwise_and_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs & rhs;
}

/* grn_ts_op_bitwise_or_bool() returns lhs | rhs. */
inline static grn_ts_bool
grn_ts_op_bitwise_or_bool(grn_ts_bool lhs, grn_ts_bool rhs)
{
  return lhs | rhs;
}

/* grn_ts_op_bitwise_or_int() returns lhs | rhs. */
inline static grn_ts_int
grn_ts_op_bitwise_or_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs | rhs;
}

/* grn_ts_op_bitwise_xor_bool() returns lhs ^ rhs. */
inline static grn_ts_bool
grn_ts_op_bitwise_xor_bool(grn_ts_bool lhs, grn_ts_bool rhs)
{
  return lhs ^ rhs;
}

/* grn_ts_op_bitwise_xor_int() returns lhs ^ rhs. */
inline static grn_ts_int
grn_ts_op_bitwise_xor_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs ^ rhs;
}

/* grn_ts_op_equal_bool() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_bool(grn_ts_bool lhs, grn_ts_bool rhs)
{
  return lhs == rhs;
}

/* grn_ts_op_equal_int() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs == rhs;
}

/* grn_ts_op_equal_float() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_float(grn_ts_float lhs, grn_ts_float rhs)
{
  /* To suppress warnings, "lhs == rhs" is not used. */
  return (lhs <= rhs) && (lhs >= rhs);
}

/* grn_ts_op_equal_time() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_time(grn_ts_time lhs, grn_ts_time rhs)
{
  return lhs == rhs;
}

/* grn_ts_op_equal_text() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_text(grn_ts_text lhs, grn_ts_text rhs)
{
  return (lhs.size == rhs.size) && !memcmp(lhs.ptr, rhs.ptr, lhs.size);
}

/* grn_ts_op_equal_geo() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_geo(grn_ts_geo lhs, grn_ts_geo rhs)
{
  return (lhs.latitude == rhs.latitude) && (lhs.longitude == rhs.longitude);
}

/* grn_ts_op_equal_ref() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_ref(grn_ts_ref lhs, grn_ts_ref rhs)
{
  /* Ignore scores. */
  return lhs.id == rhs.id;
}

#define GRN_TS_OP_EQUAL_VECTOR(kind)\
  size_t i;\
  if (lhs.size != rhs.size) {\
    return GRN_FALSE;\
  }\
  for (i = 0; i < lhs.size; i++) {\
    if (!grn_ts_op_equal_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
      return GRN_FALSE;\
    }\
  }\
  return GRN_TRUE;
/* grn_ts_op_equal_bool_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_bool_vector(grn_ts_bool_vector lhs, grn_ts_bool_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(bool)
}

/* grn_ts_op_equal_int_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_int_vector(grn_ts_int_vector lhs, grn_ts_int_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(int)
}

/* grn_ts_op_equal_float_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_float_vector(grn_ts_float_vector lhs, grn_ts_float_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(float)
}

/* grn_ts_op_equal_time_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_time_vector(grn_ts_time_vector lhs, grn_ts_time_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(time)
}

/* grn_ts_op_equal_text_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_text_vector(grn_ts_text_vector lhs, grn_ts_text_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(text)
}

/* grn_ts_op_equal_geo_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_geo_vector(grn_ts_geo_vector lhs, grn_ts_geo_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(geo)
}

/* grn_ts_op_equal_ref_vector() returns lhs == rhs. */
inline static grn_ts_bool
grn_ts_op_equal_ref_vector(grn_ts_ref_vector lhs, grn_ts_ref_vector rhs)
{
  GRN_TS_OP_EQUAL_VECTOR(ref)
}
#undef GRN_TS_OP_EQUAL_VECTOR

/* grn_ts_op_not_equal_bool() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_bool(grn_ts_bool lhs, grn_ts_bool rhs)
{
  return lhs != rhs;
}

/* grn_ts_op_not_equal_int() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs != rhs;
}

/* grn_ts_op_not_equal_float() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_float(grn_ts_float lhs, grn_ts_float rhs)
{
  /* To suppress warnings, "lhs != rhs" is not used. */
  return !grn_ts_op_equal_float(lhs, rhs);
}

/* grn_ts_op_not_equal_time() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_time(grn_ts_time lhs, grn_ts_time rhs)
{
  return lhs != rhs;
}

/* grn_ts_op_not_equal_text() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_text(grn_ts_text lhs, grn_ts_text rhs)
{
  return (lhs.size != rhs.size) || memcmp(lhs.ptr, rhs.ptr, lhs.size);
}

/* grn_ts_op_not_equal_geo() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_geo(grn_ts_geo lhs, grn_ts_geo rhs)
{
  return (lhs.latitude != rhs.latitude) || (lhs.longitude != rhs.longitude);
}

/* grn_ts_op_not_equal_ref() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_ref(grn_ts_ref lhs, grn_ts_ref rhs)
{
  /* Ignore scores. */
  return lhs.id != rhs.id;
}

#define GRN_TS_OP_NOT_EQUAL_VECTOR(kind)\
  size_t i;\
  if (lhs.size != rhs.size) {\
    return GRN_TRUE;\
  }\
  for (i = 0; i < lhs.size; i++) {\
    if (grn_ts_op_not_equal_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
      return GRN_TRUE;\
    }\
  }\
  return GRN_FALSE;
/* grn_ts_op_not_equal_bool_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_bool_vector(grn_ts_bool_vector lhs, grn_ts_bool_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(bool)
}

/* grn_ts_op_not_equal_int_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_int_vector(grn_ts_int_vector lhs, grn_ts_int_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(int)
}

/* grn_ts_op_not_equal_float_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_float_vector(grn_ts_float_vector lhs,
                                 grn_ts_float_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(float)
}

/* grn_ts_op_not_equal_time_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_time_vector(grn_ts_time_vector lhs, grn_ts_time_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(time)
}

/* grn_ts_op_not_equal_text_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_text_vector(grn_ts_text_vector lhs, grn_ts_text_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(text)
}

/* grn_ts_op_not_equal_geo_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_geo_vector(grn_ts_geo_vector lhs, grn_ts_geo_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(geo)
}

/* grn_ts_op_not_equal_ref_vector() returns lhs != rhs. */
inline static grn_ts_bool
grn_ts_op_not_equal_ref_vector(grn_ts_ref_vector lhs, grn_ts_ref_vector rhs)
{
  GRN_TS_OP_NOT_EQUAL_VECTOR(ref)
}
#undef GRN_TS_OP_NOT_EQUAL_VECTOR

/* grn_ts_op_less_int() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs < rhs;
}

/* grn_ts_op_less_float() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_float(grn_ts_float lhs, grn_ts_float rhs)
{
  return lhs < rhs;
}

/* grn_ts_op_less_time() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_time(grn_ts_time lhs, grn_ts_time rhs)
{
  return lhs < rhs;
}

/* grn_ts_op_less_text() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_text(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;
  int cmp = memcmp(lhs.ptr, rhs.ptr, min_size);
  return cmp ? (cmp < 0) : (lhs.size < rhs.size);
}

#define GRN_TS_OP_LESS_VECTOR(kind)\
  size_t i, min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;\
  for (i = 0; i < min_size; i++) {\
    if (grn_ts_op_not_equal_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
      if (grn_ts_op_less_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
        return GRN_TRUE;\
      }\
    }\
  }\
  return lhs.size < rhs.size;
/* grn_ts_op_less_int_vector() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_int_vector(grn_ts_int_vector lhs, grn_ts_int_vector rhs)
{
  GRN_TS_OP_LESS_VECTOR(int)
}

/* grn_ts_op_less_float_vector() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_float_vector(grn_ts_float_vector lhs, grn_ts_float_vector rhs)
{
  GRN_TS_OP_LESS_VECTOR(float)
}

/* grn_ts_op_less_time_vector() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_time_vector(grn_ts_time_vector lhs, grn_ts_time_vector rhs)
{
  GRN_TS_OP_LESS_VECTOR(time)
}

/* grn_ts_op_less_text_vector() returns lhs < rhs. */
inline static grn_ts_bool
grn_ts_op_less_text_vector(grn_ts_text_vector lhs, grn_ts_text_vector rhs)
{
  GRN_TS_OP_LESS_VECTOR(text)
}
#undef GRN_TS_OP_LESS_VECTOR

/* grn_ts_op_less_equal_int() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs <= rhs;
}

/* grn_ts_op_less_equal_float() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_float(grn_ts_float lhs, grn_ts_float rhs)
{
  return lhs <= rhs;
}

/* grn_ts_op_less_equal_time() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_time(grn_ts_time lhs, grn_ts_time rhs)
{
  return lhs <= rhs;
}

/* grn_ts_op_less_equal_text() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_text(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;
  int cmp = memcmp(lhs.ptr, rhs.ptr, min_size);
  return cmp ? (cmp < 0) : (lhs.size <= rhs.size);
}

#define GRN_TS_OP_LESS_EQUAL_VECTOR(kind)\
  size_t i, min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;\
  for (i = 0; i < min_size; i++) {\
    if (grn_ts_op_not_equal_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
      if (grn_ts_op_less_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
        return GRN_TRUE;\
      }\
    }\
  }\
  return lhs.size <= rhs.size;
/* grn_ts_op_less_equal_int_vector() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_int_vector(grn_ts_int_vector lhs, grn_ts_int_vector rhs)
{
  GRN_TS_OP_LESS_EQUAL_VECTOR(int)
}

/* grn_ts_op_less_equal_float_vector() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_float_vector(grn_ts_float_vector lhs,
                                  grn_ts_float_vector rhs)
{
  GRN_TS_OP_LESS_EQUAL_VECTOR(float)
}

/* grn_ts_op_less_equal_time_vector() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_time_vector(grn_ts_time_vector lhs,
                                 grn_ts_time_vector rhs)
{
  GRN_TS_OP_LESS_EQUAL_VECTOR(time)
}

/* grn_ts_op_less_equal_text_vector() returns lhs <= rhs. */
inline static grn_ts_bool
grn_ts_op_less_equal_text_vector(grn_ts_text_vector lhs,
                                 grn_ts_text_vector rhs)
{
  GRN_TS_OP_LESS_EQUAL_VECTOR(text)
}
#undef GRN_TS_OP_LESS_EQUAL_VECTOR

/* grn_ts_op_greater_int() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs > rhs;
}

/* grn_ts_op_greater_float() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_float(grn_ts_float lhs, grn_ts_float rhs)
{
  return lhs > rhs;
}

/* grn_ts_op_greater_time() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_time(grn_ts_time lhs, grn_ts_time rhs)
{
  return lhs > rhs;
}

/* grn_ts_op_greater_text() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_text(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;
  int cmp = memcmp(lhs.ptr, rhs.ptr, min_size);
  return cmp ? (cmp > 0) : (lhs.size > rhs.size);
}

#define GRN_TS_OP_GREATER_VECTOR(kind)\
  size_t i, min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;\
  for (i = 0; i < min_size; i++) {\
    if (grn_ts_op_not_equal_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
      if (grn_ts_op_greater_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
        return GRN_TRUE;\
      }\
    }\
  }\
  return lhs.size > rhs.size;
/* grn_ts_op_greater_int_vector() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_int_vector(grn_ts_int_vector lhs, grn_ts_int_vector rhs)
{
  GRN_TS_OP_GREATER_VECTOR(int)
}

/* grn_ts_op_greater_float_vector() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_float_vector(grn_ts_float_vector lhs,
                               grn_ts_float_vector rhs)
{
  GRN_TS_OP_GREATER_VECTOR(float)
}

/* grn_ts_op_greater_time_vector() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_time_vector(grn_ts_time_vector lhs, grn_ts_time_vector rhs)
{
  GRN_TS_OP_GREATER_VECTOR(time)
}

/* grn_ts_op_greater_text_vector() returns lhs > rhs. */
inline static grn_ts_bool
grn_ts_op_greater_text_vector(grn_ts_text_vector lhs, grn_ts_text_vector rhs)
{
  GRN_TS_OP_GREATER_VECTOR(text)
}
#undef GRN_TS_OP_GREATER_VECTOR

/* grn_ts_op_greater_equal_int() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_int(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs >= rhs;
}

/* grn_ts_op_greater_equal_float() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_float(grn_ts_float lhs, grn_ts_float rhs)
{
  return lhs >= rhs;
}

/* grn_ts_op_greater_equal_time() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_time(grn_ts_time lhs, grn_ts_time rhs)
{
  return lhs >= rhs;
}

/* grn_ts_op_greater_equal_text() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_text(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;
  int cmp = memcmp(lhs.ptr, rhs.ptr, min_size);
  return cmp ? (cmp > 0) : (lhs.size >= rhs.size);
}

#define GRN_TS_OP_GREATER_EQUAL_VECTOR(kind)\
  size_t i, min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;\
  for (i = 0; i < min_size; i++) {\
    if (grn_ts_op_not_equal_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
      if (grn_ts_op_greater_ ## kind(lhs.ptr[i], rhs.ptr[i])) {\
        return GRN_TRUE;\
      }\
    }\
  }\
  return lhs.size >= rhs.size;
/* grn_ts_op_greater_equal_int_vector() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_int_vector(grn_ts_int_vector lhs,
                                   grn_ts_int_vector rhs)
{
  GRN_TS_OP_GREATER_EQUAL_VECTOR(int)
}

/* grn_ts_op_greater_equal_float_vector() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_float_vector(grn_ts_float_vector lhs,
                                     grn_ts_float_vector rhs)
{
  GRN_TS_OP_GREATER_EQUAL_VECTOR(float)
}

/* grn_ts_op_greater_equal_time_vector() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_time_vector(grn_ts_time_vector lhs,
                                    grn_ts_time_vector rhs)
{
  GRN_TS_OP_GREATER_EQUAL_VECTOR(time)
}

/* grn_ts_op_greater_equal_text_vector() returns lhs >= rhs. */
inline static grn_ts_bool
grn_ts_op_greater_equal_text_vector(grn_ts_text_vector lhs,
                                    grn_ts_text_vector rhs)
{
  GRN_TS_OP_GREATER_EQUAL_VECTOR(text)
}
#undef GRN_TS_OP_GREATER_EQUAL_VECTOR

/* grn_ts_op_shift_arithmetic_left() returns lhs << rhs. */
inline static grn_ts_int
grn_ts_op_shift_arithmetic_left(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs << rhs;
}

/* grn_ts_op_shift_arithmetic_right() returns lhs << rhs. */
inline static grn_ts_int
grn_ts_op_shift_arithmetic_right(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs >> rhs;
}

/* grn_ts_op_shift_logical_left() returns lhs << rhs. */
inline static grn_ts_int
grn_ts_op_shift_logical_left(grn_ts_int lhs, grn_ts_int rhs)
{
  return lhs << rhs;
}

/* grn_ts_op_shift_logical_right() returns lhs << rhs. */
inline static grn_ts_int
grn_ts_op_shift_logical_right(grn_ts_int lhs, grn_ts_int rhs)
{
  return (uint64_t)lhs >> rhs;
}

inline static grn_rc
grn_ts_op_plus_int_int(grn_ctx *ctx, grn_ts_int lhs, grn_ts_int rhs,
                       grn_ts_int *out)
{
  *out = lhs + rhs;
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_plus_float_float(grn_ctx *ctx, grn_ts_float lhs, grn_ts_float rhs,
                           grn_ts_float *out)
{
  *out = lhs + rhs;
  if (!grn_ts_float_is_valid(*out)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "%g + %g = %g", lhs, rhs, *out);
  }
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_plus_time_int(grn_ctx *ctx, grn_ts_time lhs, grn_ts_int rhs,
                        grn_ts_time *out)
{
  *out = lhs + (rhs * 1000000);
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_plus_time_float(grn_ctx *ctx, grn_ts_time lhs, grn_ts_float rhs,
                          grn_ts_time *out)
{
  *out = (grn_ts_time)(lhs + (rhs * 1000000.0));
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_minus_int_int(grn_ctx *ctx, grn_ts_int lhs, grn_ts_int rhs,
                        grn_ts_int *out)
{
  *out = lhs - rhs;
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_minus_float_float(grn_ctx *ctx, grn_ts_float lhs, grn_ts_float rhs,
                            grn_ts_float *out)
{
  *out = lhs - rhs;
  if (!grn_ts_float_is_valid(*out)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "%g - %g = %g", lhs, rhs, *out);
  }
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_minus_time_time(grn_ctx *ctx, grn_ts_time lhs, grn_ts_time rhs,
                          grn_ts_float *out)
{
  *out = (lhs - rhs) * 0.000001;
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_minus_time_int(grn_ctx *ctx, grn_ts_time lhs, grn_ts_int rhs,
                         grn_ts_time *out)
{
  *out = lhs - (rhs * 1000000);
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_minus_time_float(grn_ctx *ctx, grn_ts_time lhs, grn_ts_float rhs,
                           grn_ts_time *out)
{
  *out = lhs - (grn_ts_int)(rhs * 1000000.0);
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_multiplication_int_int(grn_ctx *ctx, grn_ts_int lhs, grn_ts_int rhs,
                                 grn_ts_int *out)
{
  *out = lhs * rhs;
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_multiplication_float_float(grn_ctx *ctx, grn_ts_float lhs,
                                     grn_ts_float rhs, grn_ts_float *out)
{
  *out = lhs * rhs;
  if (!grn_ts_float_is_valid(*out)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "%g * %g = %g", lhs, rhs, *out);
  }
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_division_int_int(grn_ctx *ctx, grn_ts_int lhs, grn_ts_int rhs,
                           grn_ts_int *out)
{
  if (!rhs) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "%" GRN_FMT_INT64D " / %" GRN_FMT_INT64D
                      " causes division by zero",
                      lhs, rhs);
  }
  *out = (rhs != -1) ? (lhs / rhs) : -lhs;
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_division_float_float(grn_ctx *ctx, grn_ts_float lhs,
                               grn_ts_float rhs, grn_ts_float *out)
{
  *out = lhs / rhs;
  if (!grn_ts_float_is_valid(*out)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "%g / %g = %g", lhs, rhs, *out);
  }
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_modulus_int_int(grn_ctx *ctx, grn_ts_int lhs, grn_ts_int rhs,
                          grn_ts_int *out)
{
  if (!rhs) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "%" GRN_FMT_INT64D " %% %" GRN_FMT_INT64D
                      " causes division by zero",
                      lhs, rhs);
  }
  *out = (rhs != -1) ? (lhs % rhs) : -lhs;
  return GRN_SUCCESS;
}

inline static grn_rc
grn_ts_op_modulus_float_float(grn_ctx *ctx, grn_ts_float lhs, grn_ts_float rhs,
                              grn_ts_float *out)
{
  *out = fmod(lhs, rhs);
  if (!grn_ts_float_is_valid(*out)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "%g %% %g = %g", lhs, rhs, *out);
  }
  return GRN_SUCCESS;
}

static grn_ts_bool
grn_ts_op_match(grn_ts_text lhs, grn_ts_text rhs)
{
  const char *lhs_ptr, *lhs_ptr_last;
  if (lhs.size < rhs.size) {
    return GRN_FALSE;
  }
  lhs_ptr_last = lhs.ptr + lhs.size - rhs.size;
  for (lhs_ptr = lhs.ptr; lhs_ptr <= lhs_ptr_last; lhs_ptr++) {
    size_t i;
    for (i = 0; i < rhs.size; i++) {
      if (lhs_ptr[i] != rhs.ptr[i]) {
        break;
      }
    }
    if (i == rhs.size) {
      return GRN_TRUE;
    }
  }
  return GRN_FALSE;
}

static grn_ts_bool
grn_ts_op_prefix_match(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t i;
  if (lhs.size < rhs.size) {
    return GRN_FALSE;
  }
  for (i = 0; i < rhs.size; i++) {
    if (lhs.ptr[i] != rhs.ptr[i]) {
      return GRN_FALSE;
    }
  }
  return GRN_TRUE;
}

static grn_ts_bool
grn_ts_op_suffix_match(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t i;
  const char *lhs_ptr;
  if (lhs.size < rhs.size) {
    return GRN_FALSE;
  }
  lhs_ptr = lhs.ptr + lhs.size - rhs.size;
  for (i = 0; i < rhs.size; i++) {
    if (lhs_ptr[i] != rhs.ptr[i]) {
      return GRN_FALSE;
    }
  }
  return GRN_TRUE;
}

/*-------------------------------------------------------------
 * Groonga objects.
 */

#define GRN_TS_TABLE_GET_KEY(type)\
  uint32_t key_size;\
  const void *key_ptr = _grn_ ## type ## _key(ctx, type, id, &key_size);\
  if (!key_ptr) {\
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "_grn_" #type "_key failed: %u", id);\
  }\
/* grn_ts_hash_get_bool_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_bool_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                         grn_ts_bool *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const grn_ts_bool *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_int8_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_int8_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                         grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const int8_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_int16_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_int16_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                          grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const int16_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_int32_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_int32_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                          grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const int32_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_int64_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_int64_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                          grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const int64_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_uint8_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_uint8_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                          grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const uint8_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_uint16_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_uint16_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                           grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const uint16_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_uint32_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_uint32_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                           grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const uint32_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_uint64_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_uint64_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                           grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = (grn_ts_int)*(const uint64_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_float_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_float_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                          grn_ts_float *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const grn_ts_float *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_time_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_time_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                         grn_ts_time *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const grn_ts_time *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_geo_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_geo_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                        grn_ts_geo *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  *key = *(const grn_ts_geo *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_text_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_text_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                         grn_ts_text *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  key->ptr = key_ptr;
  key->size = key_size;
  return GRN_SUCCESS;
}

/* grn_ts_hash_get_ref_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_hash_get_ref_key(grn_ctx *ctx, grn_hash *hash, grn_ts_id id,
                        grn_ts_ref *key)
{
  GRN_TS_TABLE_GET_KEY(hash)
  key->id = *(const grn_ts_id *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_bool_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_bool_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                        grn_ts_bool *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  *key = *(const grn_ts_bool *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_int8_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_int8_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                        grn_ts_int *key)
{
  int8_t tmp;
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntohi(&tmp, key_ptr, sizeof(tmp));
  *key = tmp;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_int16_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_int16_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                         grn_ts_int *key)
{
  int16_t tmp;
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntohi(&tmp, key_ptr, sizeof(tmp));
  *key = tmp;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_int32_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_int32_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                         grn_ts_int *key)
{
  int32_t tmp;
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntohi(&tmp, key_ptr, sizeof(tmp));
  *key = tmp;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_int64_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_int64_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                         grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntohi(key, key_ptr, sizeof(grn_ts_int));
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_uint8_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_uint8_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                         grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  *key = *(const uint8_t *)key_ptr;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_uint16_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_uint16_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                          grn_ts_int *key)
{
  uint16_t tmp;
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntoh(&tmp, key_ptr, sizeof(tmp));
  *key = tmp;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_uint32_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_uint32_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                          grn_ts_int *key)
{
  uint32_t tmp;
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntoh(&tmp, key_ptr, sizeof(tmp));
  *key = tmp;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_uint64_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_uint64_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                          grn_ts_int *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntoh(key, key_ptr, sizeof(grn_ts_int));
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_float_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_float_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                         grn_ts_float *key)
{
  int64_t tmp;
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntoh(&tmp, key_ptr, sizeof(tmp));
  tmp ^= (((tmp ^ ((int64_t)1 << 63)) >> 63) | ((int64_t)1 << 63));
  *(int64_t *)key = tmp;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_time_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_time_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                        grn_ts_time *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntohi(key, key_ptr, sizeof(grn_ts_time));
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_geo_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_geo_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                       grn_ts_geo *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntog(key, key_ptr, sizeof(grn_ts_geo));
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_text_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_text_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                        grn_ts_text *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  key->ptr = key_ptr;
  key->size = key_size;
  return GRN_SUCCESS;
}

/* grn_ts_pat_get_ref_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_pat_get_ref_key(grn_ctx *ctx, grn_pat *pat, grn_ts_id id,
                       grn_ts_ref *key)
{
  GRN_TS_TABLE_GET_KEY(pat)
  grn_ntoh(&key->id, key_ptr, sizeof(key->id));
  return GRN_SUCCESS;
}

/* grn_ts_dat_get_text_key() gets a reference to a key (_key). */
static grn_rc
grn_ts_dat_get_text_key(grn_ctx *ctx, grn_dat *dat, grn_ts_id id,
                        grn_ts_text *key)
{
  GRN_TS_TABLE_GET_KEY(dat)
  key->ptr = key_ptr;
  key->size = key_size;
  return GRN_SUCCESS;
}
#undef GRN_TS_TABLE_GET_KEY

/*-------------------------------------------------------------
 * grn_ts_expr_id_node.
 */

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
} grn_ts_expr_id_node;

/* grn_ts_expr_id_node_init() initializes a node. */
static void
grn_ts_expr_id_node_init(grn_ctx *ctx, grn_ts_expr_id_node *node)
{
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_ID_NODE;
  node->data_kind = GRN_TS_INT;
  node->data_type = GRN_DB_UINT32;
}

/* grn_ts_expr_id_node_fin() finalizes a node. */
static void
grn_ts_expr_id_node_fin(grn_ctx *ctx, grn_ts_expr_id_node *node)
{
  /* Nothing to do. */
}

grn_rc
grn_ts_expr_id_node_open(grn_ctx *ctx, grn_ts_expr_node **node)
{
  grn_ts_expr_id_node *new_node = GRN_MALLOCN(grn_ts_expr_id_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_id_node));
  }
  grn_ts_expr_id_node_init(ctx, new_node);
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_id_node_close() destroys a node. */
static void
grn_ts_expr_id_node_close(grn_ctx *ctx, grn_ts_expr_id_node *node)
{
  grn_ts_expr_id_node_fin(ctx, node);
  GRN_FREE(node);
}

/* grn_ts_expr_id_node_evaluate() outputs IDs. */
static grn_rc
grn_ts_expr_id_node_evaluate(grn_ctx *ctx, grn_ts_expr_id_node *node,
                             const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i;
  grn_ts_int *out_ptr = (grn_ts_int *)out;
  for (i = 0; i < n_in; i++) {
    out_ptr[i] = (grn_ts_int)in[i].id;
  }
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_expr_score_node.
 */

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
} grn_ts_expr_score_node;

/* grn_ts_expr_score_node_init() initializes a node. */
static void
grn_ts_expr_score_node_init(grn_ctx *ctx, grn_ts_expr_score_node *node)
{
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_SCORE_NODE;
  node->data_kind = GRN_TS_FLOAT;
  node->data_type = GRN_DB_FLOAT;
}

/* grn_ts_expr_score_node_fin() finalizes a node. */
static void
grn_ts_expr_score_node_fin(grn_ctx *ctx, grn_ts_expr_score_node *node)
{
  /* Nothing to do. */
}

grn_rc
grn_ts_expr_score_node_open(grn_ctx *ctx, grn_ts_expr_node **node)
{
  grn_ts_expr_score_node *new_node = GRN_MALLOCN(grn_ts_expr_score_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_score_node));
  }
  grn_ts_expr_score_node_init(ctx, new_node);
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_score_node_close() destroys a node. */
static void
grn_ts_expr_score_node_close(grn_ctx *ctx, grn_ts_expr_score_node *node)
{
  grn_ts_expr_score_node_fin(ctx, node);
  GRN_FREE(node);
}

/* grn_ts_expr_score_node_evaluate() outputs scores. */
static grn_rc
grn_ts_expr_score_node_evaluate(grn_ctx *ctx, grn_ts_expr_score_node *node,
                                const grn_ts_record *in, size_t n_in,
                                void *out)
{
  size_t i;
  grn_ts_float *out_ptr = (grn_ts_float *)out;
  for (i = 0; i < n_in; i++) {
    out_ptr[i] = (grn_ts_float)in[i].score;
  }
  return GRN_SUCCESS;
}

/* grn_ts_expr_score_node_adjust() does nothing. */
static grn_rc
grn_ts_expr_score_node_adjust(grn_ctx *ctx, grn_ts_expr_score_node *node,
                              grn_ts_record *io, size_t n_io)
{
  /* Nothing to do. */
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_expr_key_node.
 */

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
  grn_obj *table;
  grn_ts_buf buf;
} grn_ts_expr_key_node;

/* grn_ts_expr_key_node_init() initializes a node. */
static void
grn_ts_expr_key_node_init(grn_ctx *ctx, grn_ts_expr_key_node *node)
{
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_KEY_NODE;
  node->table = NULL;
  grn_ts_buf_init(ctx, &node->buf);
}

/* grn_ts_expr_key_node_fin() finalizes a node. */
static void
grn_ts_expr_key_node_fin(grn_ctx *ctx, grn_ts_expr_key_node *node)
{
  grn_ts_buf_fin(ctx, &node->buf);
  if (node->table) {
    grn_obj_unlink(ctx, node->table);
  }
}

grn_rc
grn_ts_expr_key_node_open(grn_ctx *ctx, grn_obj *table,
                          grn_ts_expr_node **node)
{
  grn_rc rc;
  grn_ts_expr_key_node *new_node;
  if (!grn_ts_table_has_key(ctx, table)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "the table has no _key");
  }
  new_node = GRN_MALLOCN(grn_ts_expr_key_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_key_node));
  }
  grn_ts_expr_key_node_init(ctx, new_node);
  rc = grn_ts_obj_increment_ref_count(ctx, table);
  if (rc != GRN_SUCCESS) {
    grn_ts_expr_key_node_fin(ctx, new_node);
    GRN_FREE(new_node);
    return rc;
  }
  new_node->data_kind = grn_ts_data_type_to_kind(table->header.domain);
  new_node->data_type = table->header.domain;
  new_node->table = table;
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_key_node_close() destroys a node. */
static void
grn_ts_expr_key_node_close(grn_ctx *ctx, grn_ts_expr_key_node *node)
{
  grn_ts_expr_key_node_fin(ctx, node);
  GRN_FREE(node);
}

#define GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(table, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    for (i = 0; i < n_in; i++) {\
      rc = grn_ts_ ## table ## _get_ ## kind ## _key(ctx, table, in[i].id,\
                                                     &out_ptr[i]);\
      if (rc != GRN_SUCCESS) {\
        out_ptr[i] = grn_ts_ ## kind ## _zero();\
      }\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(table, TYPE, type)\
  case GRN_DB_ ## TYPE: {\
    grn_ts_int *out_ptr = (grn_ts_int *)out;\
    for (i = 0; i < n_in; i++) {\
      rc = grn_ts_ ## table ## _get_ ## type ## _key(ctx, table, in[i].id,\
                                                     &out_ptr[i]);\
      if (rc != GRN_SUCCESS) {\
        out_ptr[i] = grn_ts_int_zero();\
      }\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_KEY_NODE_EVALUATE_TEXT_CASE(table)\
  case GRN_TS_TEXT: {\
    char *buf_ptr;\
    grn_ts_text *out_ptr = (grn_ts_text *)out;\
    node->buf.pos = 0;\
    for (i = 0; i < n_in; i++) {\
      grn_ts_text key;\
      rc = grn_ts_ ## table ## _get_text_key(ctx, table, in[i].id, &key);\
      if (rc != GRN_SUCCESS) {\
        key = grn_ts_text_zero();\
      }\
      rc = grn_ts_buf_write(ctx, &node->buf, key.ptr, key.size);\
      if (rc != GRN_SUCCESS) {\
        return rc;\
      }\
      out_ptr[i].size = key.size;\
    }\
    buf_ptr = (char *)node->buf.ptr;\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i].ptr = buf_ptr;\
      buf_ptr += out_ptr[i].size;\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_KEY_NODE_EVALUATE_REF_CASE(table)\
  case GRN_TS_REF: {\
    grn_ts_ref *out_ptr = (grn_ts_ref *)out;\
    for (i = 0; i < n_in; i++) {\
      rc = grn_ts_ ## table ## _get_ref_key(ctx, table, in[i].id,\
                                            &out_ptr[i]);\
      if (rc != GRN_SUCCESS) {\
        out_ptr[i] = grn_ts_ref_zero();\
      }\
      out_ptr[i].score = in[i].score;\
    }\
    return GRN_SUCCESS;\
  }
/* grn_ts_expr_key_node_evaluate() outputs keys. */
static grn_rc
grn_ts_expr_key_node_evaluate(grn_ctx *ctx, grn_ts_expr_key_node *node,
                              const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i;
  grn_rc rc;
  switch (node->table->header.type) {
    case GRN_TABLE_HASH_KEY: {
      grn_hash *hash = (grn_hash *)node->table;
      switch (node->data_kind) {
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(hash, BOOL, bool)
        case GRN_TS_INT: {
          switch (node->data_type) {
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, INT8, int8)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, INT16, int16)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, INT32, int32)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, INT64, int64)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, UINT8, uint8)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, UINT16, uint16)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, UINT32, uint32)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(hash, UINT64, uint64)
          }
        }
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(hash, FLOAT, float)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(hash, TIME, time)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_TEXT_CASE(hash)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(hash, GEO, geo)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_REF_CASE(hash)
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                            node->data_kind);
        }
      }
    }
    case GRN_TABLE_PAT_KEY: {
      grn_pat *pat = (grn_pat *)node->table;
      switch (node->data_kind) {
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(pat, BOOL, bool)
        case GRN_TS_INT: {
          switch (node->data_type) {
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, INT8, int8)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, INT16, int16)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, INT32, int32)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, INT64, int64)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, UINT8, uint8)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, UINT16, uint16)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, UINT32, uint32)
            GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE(pat, UINT64, uint64)
          }
        }
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(pat, FLOAT, float)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(pat, TIME, time)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_TEXT_CASE(pat)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE(pat, GEO, geo)
        GRN_TS_EXPR_KEY_NODE_EVALUATE_REF_CASE(pat)
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                            node->data_kind);
        }
      }
    }
    case GRN_TABLE_DAT_KEY: {
      grn_dat *dat = (grn_dat *)node->table;
      switch (node->data_kind) {
        GRN_TS_EXPR_KEY_NODE_EVALUATE_TEXT_CASE(dat)
        /* GRN_TABLE_DAT_KEY supports only Text. */
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                            node->data_kind);
        }
      }
    }
    /* GRN_TABLE_NO_KEY doesn't support _key. */
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid table type: %d",
                        node->table->header.type);
    }
  }
}
#undef GRN_TS_EXPR_KEY_NODE_EVALUATE_REF_CASE
#undef GRN_TS_EXPR_KEY_NODE_EVALUATE_TEXT_CASE
#undef GRN_TS_EXPR_KEY_NODE_EVALUATE_INT_CASE
#undef GRN_TS_EXPR_KEY_NODE_EVALUATE_CASE

/* grn_ts_expr_key_node_filter() filters records. */
static grn_rc
grn_ts_expr_key_node_filter(grn_ctx *ctx, grn_ts_expr_key_node *node,
                            grn_ts_record *in, size_t n_in,
                            grn_ts_record *out, size_t *n_out)
{
  size_t i, count;
  grn_ts_bool key;
  switch (node->table->header.type) {
    case GRN_TABLE_HASH_KEY: {
      grn_hash *hash = (grn_hash *)node->table;
      for (i = 0, count = 0; i < n_in; i++) {
        grn_rc rc = grn_ts_hash_get_bool_key(ctx, hash, in[i].id, &key);
        if (rc != GRN_SUCCESS) {
          key = grn_ts_bool_zero();
        }
        if (key) {
          out[count++] = in[i];
        }
      }
      *n_out = count;
      return GRN_SUCCESS;
    }
    case GRN_TABLE_PAT_KEY: {
      grn_pat *pat = (grn_pat *)node->table;
      for (i = 0, count = 0; i < n_in; i++) {
        grn_rc rc = grn_ts_pat_get_bool_key(ctx, pat, in[i].id, &key);
        if (rc != GRN_SUCCESS) {
          key = grn_ts_bool_zero();
        }
        if (key) {
          out[count++] = in[i];
        }
      }
      *n_out = count;
      return GRN_SUCCESS;
    }
    /* GRN_TABLE_DAT_KEY and GRN_TABLE_NO_KEY don't support a Bool key. */
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid table type: %d",
                        node->table->header.type);
    }
  }
}

/* grn_ts_expr_key_node_adjust() updates scores. */
static grn_rc
grn_ts_expr_key_node_adjust(grn_ctx *ctx, grn_ts_expr_key_node *node,
                            grn_ts_record *io, size_t n_io)
{
  size_t i;
  grn_ts_float key;
  switch (node->table->header.type) {
    case GRN_TABLE_HASH_KEY: {
      grn_hash *hash = (grn_hash *)node->table;
      for (i = 0; i < n_io; i++) {
        grn_rc rc = grn_ts_hash_get_float_key(ctx, hash, io[i].id, &key);
        if (rc != GRN_SUCCESS) {
          key = grn_ts_float_zero();
        }
        io[i].score = (grn_ts_score)key;
      }
      return GRN_SUCCESS;
    }
    case GRN_TABLE_PAT_KEY: {
      grn_pat *pat = (grn_pat *)node->table;
      for (i = 0; i < n_io; i++) {
        grn_rc rc = grn_ts_pat_get_float_key(ctx, pat, io[i].id, &key);
        if (rc != GRN_SUCCESS) {
          key = grn_ts_float_zero();
        }
        io[i].score = (grn_ts_score)key;
      }
      return GRN_SUCCESS;
    }
    /* GRN_TABLE_DAT_KEY and GRN_TABLE_NO_KEY don't support a Float key. */
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid table type: %d",
                        node->table->header.type);
    }
  }
}

/*-------------------------------------------------------------
 * grn_ts_expr_value_node.
 */

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
  grn_obj *table;
} grn_ts_expr_value_node;

/* grn_ts_expr_value_node_init() initializes a node. */
static void
grn_ts_expr_value_node_init(grn_ctx *ctx, grn_ts_expr_value_node *node)
{
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_VALUE_NODE;
  node->table = NULL;
}

/* grn_ts_expr_value_node_fin() finalizes a node. */
static void
grn_ts_expr_value_node_fin(grn_ctx *ctx, grn_ts_expr_value_node *node)
{
  if (node->table) {
    grn_obj_unlink(ctx, node->table);
  }
}

grn_rc
grn_ts_expr_value_node_open(grn_ctx *ctx, grn_obj *table,
                            grn_ts_expr_node **node)
{
  grn_rc rc;
  grn_ts_expr_value_node *new_node;
  if (!grn_ts_table_has_value(ctx, table)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "table has no _value");
  }
  new_node = GRN_MALLOCN(grn_ts_expr_value_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_value_node));
  }
  grn_ts_expr_value_node_init(ctx, new_node);
  rc = grn_ts_obj_increment_ref_count(ctx, table);
  if (rc != GRN_SUCCESS) {
    GRN_FREE(new_node);
    return rc;
  }
  new_node->data_kind = grn_ts_data_type_to_kind(DB_OBJ(table)->range);
  new_node->data_type = DB_OBJ(table)->range;
  new_node->table = table;
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_value_node_close() destroys a node. */
static void
grn_ts_expr_value_node_close(grn_ctx *ctx, grn_ts_expr_value_node *node)
{
  grn_ts_expr_value_node_fin(ctx, node);
  GRN_FREE(node);
}

#define GRN_TS_EXPR_VALUE_NODE_EVALUATE_CASE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    size_t i;\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    for (i = 0; i < n_in; i++) {\
      const void *ptr = grn_ts_table_get_value(ctx, node->table, in[i].id);\
      if (ptr) {\
        out_ptr[i] = *(const grn_ts_ ## kind *)ptr;\
      } else {\
        out_ptr[i] = grn_ts_ ## kind ## _zero();\
      }\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(TYPE, type)\
  case GRN_DB_ ## TYPE: {\
    size_t i;\
    grn_ts_int *out_ptr = (grn_ts_int *)out;\
    for (i = 0; i < n_in; i++) {\
      const void *ptr = grn_ts_table_get_value(ctx, node->table, in[i].id);\
      if (ptr) {\
        out_ptr[i] = (grn_ts_int)*(const type ## _t *)ptr;\
      } else {\
        out_ptr[i] = grn_ts_int_zero();\
      }\
    }\
    return GRN_SUCCESS;\
  }
/* grn_ts_expr_value_node_evaluate() outputs values. */
static grn_rc
grn_ts_expr_value_node_evaluate(grn_ctx *ctx, grn_ts_expr_value_node *node,
                                const grn_ts_record *in, size_t n_in,
                                void *out)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_VALUE_NODE_EVALUATE_CASE(BOOL, bool)
    case GRN_TS_INT: {
      switch (node->data_type) {
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(INT8, int8)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(INT16, int16)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(INT32, int32)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(INT64, int64)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(UINT8, uint8)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(UINT16, uint16)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(UINT32, uint32)
        GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE(UINT64, uint64)
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data type: %d",
                            node->data_type);
        }
      }
    }
    GRN_TS_EXPR_VALUE_NODE_EVALUATE_CASE(FLOAT, float)
    GRN_TS_EXPR_VALUE_NODE_EVALUATE_CASE(TIME, time)
    GRN_TS_EXPR_VALUE_NODE_EVALUATE_CASE(GEO, geo)
    case GRN_TS_REF: {
      size_t i;
      grn_ts_ref *out_ptr = (grn_ts_ref *)out;
      for (i = 0; i < n_in; i++) {
        const void *ptr = grn_ts_table_get_value(ctx, node->table, in[i].id);
        if (ptr) {
          out_ptr[i].id = *(const grn_ts_id *)ptr;
          out_ptr[i].score = in[i].score;
        } else {
          out_ptr[i] = grn_ts_ref_zero();
        }
      }
      return GRN_SUCCESS;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_VALUE_NODE_EVALUATE_INT_CASE
#undef GRN_TS_EXPR_VALUE_NODE_EVALUATE_CASE

/* grn_ts_expr_value_node_filter() filters records. */
static grn_rc
grn_ts_expr_value_node_filter(grn_ctx *ctx, grn_ts_expr_value_node *node,
                              grn_ts_record *in, size_t n_in,
                              grn_ts_record *out, size_t *n_out)
{
  size_t i, count = 0;
  for (i = 0; i < n_in; i++) {
    const void *ptr = grn_ts_table_get_value(ctx, node->table, in[i].id);
    if (ptr && *(const grn_ts_bool *)ptr) {
      out[count++] = in[i];
    }
  }
  *n_out = count;
  return GRN_SUCCESS;
}

/* grn_ts_expr_value_node_adjust() updates scores. */
static grn_rc
grn_ts_expr_value_node_adjust(grn_ctx *ctx, grn_ts_expr_value_node *node,
                              grn_ts_record *io, size_t n_io)
{
  size_t i;
  for (i = 0; i < n_io; i++) {
    const void *ptr = grn_ts_table_get_value(ctx, node->table, io[i].id);
    if (ptr) {
      io[i].score = (grn_ts_score)*(const grn_ts_float *)ptr;
    }
  }
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_expr_const_node.
 */

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
  grn_ts_any content;
  grn_ts_buf text_buf;
  grn_ts_buf vector_buf;
} grn_ts_expr_const_node;

/* grn_ts_expr_const_node_init() initializes a node. */
static void
grn_ts_expr_const_node_init(grn_ctx *ctx, grn_ts_expr_const_node *node)
{
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_CONST_NODE;
  grn_ts_buf_init(ctx, &node->text_buf);
  grn_ts_buf_init(ctx, &node->vector_buf);
}

/* grn_ts_expr_const_node_fin() finalizes a node. */
static void
grn_ts_expr_const_node_fin(grn_ctx *ctx, grn_ts_expr_const_node *node)
{
  grn_ts_buf_fin(ctx, &node->vector_buf);
  grn_ts_buf_fin(ctx, &node->text_buf);
}

#define GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    node->content.as_ ## kind = value.as_ ## kind;\
    return GRN_SUCCESS;\
  }
/* grn_ts_expr_const_node_set_scalar() sets a scalar value. */
static grn_rc
grn_ts_expr_const_node_set_scalar(grn_ctx *ctx, grn_ts_expr_const_node *node,
                                  grn_ts_any value)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE(BOOL, bool)
    GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE(INT, int)
    GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE(FLOAT, float)
    GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE(TIME, time)
    case GRN_TS_TEXT: {
      grn_rc rc = grn_ts_buf_write(ctx, &node->text_buf,
                                   value.as_text.ptr, value.as_text.size);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      node->content.as_text.ptr = (const char *)node->text_buf.ptr;
      node->content.as_text.size = value.as_text.size;
      return GRN_SUCCESS;
    }
    GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE(GEO, geo)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_CONST_NODE_SET_SCALAR_CASE

#define GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE(KIND, kind)\
  case GRN_TS_ ## KIND ## _VECTOR: {\
    grn_rc rc;\
    size_t n_bytes;\
    const grn_ts_ ## kind *buf_ptr;\
    grn_ts_ ## kind ## _vector vector;\
    vector = value.as_ ## kind ## _vector;\
    n_bytes = sizeof(grn_ts_ ## kind) * vector.size;\
    rc = grn_ts_buf_write(ctx, &node->vector_buf, vector.ptr, n_bytes);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
    buf_ptr = (const grn_ts_ ## kind *)node->vector_buf.ptr;\
    node->content.as_ ## kind ## _vector.ptr = buf_ptr;\
    node->content.as_ ## kind ## _vector.size = vector.size;\
    return GRN_SUCCESS;\
  }
/* grn_ts_expr_const_node_set_vector() sets a vector value. */
static grn_rc
grn_ts_expr_const_node_set_vector(grn_ctx *ctx, grn_ts_expr_const_node *node,
                                  grn_ts_any value)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE(BOOL, bool)
    GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE(INT, int)
    GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE(FLOAT, float)
    GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE(TIME, time)
    case GRN_TS_TEXT_VECTOR: {
      grn_rc rc;
      size_t i, n_bytes, offset, total_size;
      grn_ts_text_vector vector = value.as_text_vector;
      grn_ts_text *vector_buf;
      char *text_buf;
      n_bytes = sizeof(grn_ts_text) * vector.size;
      rc = grn_ts_buf_resize(ctx, &node->vector_buf, n_bytes);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      vector_buf = (grn_ts_text *)node->vector_buf.ptr;
      total_size = 0;
      for (i = 0; i < vector.size; i++) {
        total_size += vector.ptr[i].size;
      }
      rc = grn_ts_buf_resize(ctx, &node->text_buf, total_size);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      text_buf = (char *)node->text_buf.ptr;
      offset = 0;
      for (i = 0; i < vector.size; i++) {
        grn_memcpy(text_buf + offset, vector.ptr[i].ptr, vector.ptr[i].size);
        vector_buf[i].ptr = text_buf + offset;
        vector_buf[i].size = vector.ptr[i].size;
        offset += vector.ptr[i].size;
      }
      node->content.as_text_vector.ptr = vector_buf;
      node->content.as_text_vector.size = vector.size;
      return GRN_SUCCESS;
    }
    GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE(GEO, geo)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_CONST_NODE_SET_VECTOR_CASE

#define GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    if (!grn_ts_ ## kind ## _is_valid(value.as_ ## kind)) {\
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");\
    }\
    return GRN_SUCCESS;\
  }
static grn_rc
grn_ts_expr_const_node_check_value(grn_ctx *ctx, grn_ts_data_kind kind,
                                   grn_ts_any value)
{
  switch (kind) {
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(BOOL, bool)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(INT, int)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(FLOAT, float)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(TIME, time)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(TEXT, text)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(GEO, geo)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(BOOL_VECTOR, bool_vector)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(INT_VECTOR, int_vector)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(FLOAT_VECTOR, float_vector)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(TIME_VECTOR, time_vector)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(TEXT_VECTOR, text_vector)
    GRN_TS_EXPR_CONST_NODE_CHECK_VALUE(GEO_VECTOR, geo_vector)
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
    }
  }
}
#undef GRN_TS_EXPR_CONST_NODE_CHECK_VALUE

grn_rc
grn_ts_expr_const_node_open(grn_ctx *ctx, grn_ts_data_kind data_kind,
                            grn_ts_data_type data_type,
                            grn_ts_any value, grn_ts_expr_node **node)
{
  grn_rc rc = grn_ts_expr_const_node_check_value(ctx, data_kind, value);
  grn_ts_expr_const_node *new_node;
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  new_node = GRN_MALLOCN(grn_ts_expr_const_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_const_node));
  }
  grn_ts_expr_const_node_init(ctx, new_node);
  new_node->data_kind = data_kind;
  if (data_type != GRN_DB_VOID) {
    new_node->data_type = data_type;
  } else {
    new_node->data_type = grn_ts_data_kind_to_type(data_kind);
  }
  if (data_kind & GRN_TS_VECTOR_FLAG) {
    rc = grn_ts_expr_const_node_set_vector(ctx, new_node, value);
  } else {
    rc = grn_ts_expr_const_node_set_scalar(ctx, new_node, value);
  }
  if (rc != GRN_SUCCESS) {
    grn_ts_expr_const_node_fin(ctx, new_node);
    GRN_FREE(new_node);
    return rc;
  }
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_const_node_close() destroys a node. */
static void
grn_ts_expr_const_node_close(grn_ctx *ctx, grn_ts_expr_const_node *node)
{
  grn_ts_expr_const_node_fin(ctx, node);
  GRN_FREE(node);
}

#define GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    size_t i;\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i] = node->content.as_ ## kind;\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(KIND, kind)\
  GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(KIND ## _VECTOR, kind ## _vector)
/* grn_ts_expr_const_node_evaluate() outputs the stored const. */
static grn_rc
grn_ts_expr_const_node_evaluate(grn_ctx *ctx, grn_ts_expr_const_node *node,
                                const grn_ts_record *in, size_t n_in,
                                void *out)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(BOOL, bool)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(INT, int)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(FLOAT, float)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(TIME, time)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(TEXT, text)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE(GEO, geo)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(BOOL, bool)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(INT, int)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(FLOAT, float)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(TIME, time)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(TEXT, text)
    GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE(GEO, geo)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_CONST_NODE_EVALUATE_VECTOR_CASE
#undef GRN_TS_EXPR_CONST_NODE_EVALUATE_CASE

/* grn_ts_expr_const_node_filter() filters records. */
static grn_rc
grn_ts_expr_const_node_filter(grn_ctx *ctx, grn_ts_expr_const_node *node,
                              grn_ts_record *in, size_t n_in,
                              grn_ts_record *out, size_t *n_out)
{
  if (node->content.as_bool) {
    /* All the records pass through the filter. */
    if (in != out) {
      size_t i;
      for (i = 0; i < n_in; i++) {
        out[i] = in[i];
      }
    }
    *n_out = n_in;
  } else {
    /* All the records are discarded. */
    *n_out = 0;
  }
  return GRN_SUCCESS;
}

/* grn_ts_expr_const_node_adjust() updates scores. */
static grn_rc
grn_ts_expr_const_node_adjust(grn_ctx *ctx, grn_ts_expr_const_node *node,
                              grn_ts_record *io, size_t n_io)
{
  size_t i;
  grn_ts_score score = (grn_ts_score)node->content.as_float;
  for (i = 0; i < n_io; i++) {
    io[i].score = score;
  }
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_expr_column_node.
 */

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
  grn_obj *column;
  grn_ts_buf buf;
  grn_ts_buf body_buf;
  grn_ja_reader *reader;
} grn_ts_expr_column_node;

/* grn_ts_expr_column_node_init() initializes a node. */
static void
grn_ts_expr_column_node_init(grn_ctx *ctx, grn_ts_expr_column_node *node)
{
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_COLUMN_NODE;
  node->column = NULL;
  grn_ts_buf_init(ctx, &node->buf);
  grn_ts_buf_init(ctx, &node->body_buf);
  node->reader = NULL;
}

/* grn_ts_expr_column_node_fin() finalizes a node. */
static void
grn_ts_expr_column_node_fin(grn_ctx *ctx, grn_ts_expr_column_node *node)
{
  if (node->reader) {
    grn_ja_reader_close(ctx, node->reader);
  }
  grn_ts_buf_fin(ctx, &node->body_buf);
  grn_ts_buf_fin(ctx, &node->buf);
  if (node->column) {
    grn_obj_unlink(ctx, node->column);
  }
}

#define GRN_TS_EXPR_COLUMN_NODE_OPEN_CASE(TYPE)\
  case GRN_DB_ ## TYPE: {\
    GRN_ ## TYPE ## _INIT(&new_node->buf, GRN_OBJ_VECTOR);\
    break;\
  }
grn_rc
grn_ts_expr_column_node_open(grn_ctx *ctx, grn_obj *column,
                             grn_ts_expr_node **node)
{
  grn_rc rc;
  grn_ts_expr_column_node *new_node = GRN_MALLOCN(grn_ts_expr_column_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_column_node));
  }
  grn_ts_expr_column_node_init(ctx, new_node);
  new_node->data_kind = grn_ts_data_type_to_kind(DB_OBJ(column)->range);
  if (column->header.type == GRN_COLUMN_VAR_SIZE) {
    grn_obj_flags type = column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK;
    if (type == GRN_OBJ_COLUMN_VECTOR) {
      new_node->data_kind |= GRN_TS_VECTOR_FLAG;
    }
  }
  new_node->data_type = DB_OBJ(column)->range;
  rc = grn_ts_obj_increment_ref_count(ctx, column);
  if (rc != GRN_SUCCESS) {
    grn_ts_expr_column_node_fin(ctx, new_node);
    GRN_FREE(new_node);
    return rc;
  }
  new_node->column = column;
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}
#undef GRN_TS_EXPR_COLUMN_NODE_OPEN_CASE

/* grn_ts_expr_column_node_close() destroys a node. */
static void
grn_ts_expr_column_node_close(grn_ctx *ctx, grn_ts_expr_column_node *node)
{
  grn_ts_expr_column_node_fin(ctx, node);
  GRN_FREE(node);
}

#define GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_CASE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    size_t i;\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    grn_ra *ra = (grn_ra *)node->column;\
    grn_ra_cache cache;\
    GRN_RA_CACHE_INIT(ra, &cache);\
    for (i = 0; i < n_in; i++) {\
      grn_ts_ ## kind *ptr = NULL;\
      if (in[i].id) {\
        ptr = (grn_ts_ ## kind *)grn_ra_ref_cache(ctx, ra, in[i].id, &cache);\
      }\
      out_ptr[i] = ptr ? *ptr : grn_ts_ ## kind ## _zero();\
    }\
    GRN_RA_CACHE_FIN(ra, &cache);\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(TYPE, type)\
  case GRN_DB_ ## TYPE: {\
    size_t i;\
    grn_ts_int *out_ptr = (grn_ts_int *)out;\
    grn_ra *ra = (grn_ra *)node->column;\
    grn_ra_cache cache;\
    GRN_RA_CACHE_INIT(ra, &cache);\
    for (i = 0; i < n_in; i++) {\
      type ## _t *ptr = NULL;\
      if (in[i].id) {\
        ptr = (type ## _t *)grn_ra_ref_cache(ctx, ra, in[i].id, &cache);\
      }\
      out_ptr[i] = ptr ? (grn_ts_int)*ptr : grn_ts_int_zero();\
    }\
    GRN_RA_CACHE_FIN(ra, &cache);\
    return GRN_SUCCESS;\
  }
/* grn_ts_expr_column_node_evaluate_scalar() outputs scalar column values. */
static grn_rc
grn_ts_expr_column_node_evaluate_scalar(grn_ctx *ctx,
                                        grn_ts_expr_column_node *node,
                                        const grn_ts_record *in, size_t n_in,
                                        void *out)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_CASE(BOOL, bool)
    case GRN_TS_INT: {
      switch (node->data_type) {
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(INT8, int8)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(INT16, int16)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(INT32, int32)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(INT64, int64)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(UINT8, uint8)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(UINT16, uint16)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(UINT32, uint32)
        /* The behavior is undefined if a value is greater than 2^63 - 1. */
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE(UINT64, uint64)
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data type: %d",
                            node->data_type);
        }
      }
    }
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_CASE(FLOAT, float)
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_CASE(TIME, time)
    case GRN_TS_TEXT: {
      size_t i;
      char *buf_ptr;
      grn_rc rc;
      grn_ts_text *out_ptr = (grn_ts_text *)out;
      if (!node->reader) {
        rc = grn_ja_reader_open(ctx, (grn_ja *)node->column, &node->reader);
        if (rc != GRN_SUCCESS) {
          GRN_TS_ERR_RETURN(rc, "grn_ja_reader_open failed");
        }
      } else {
        grn_ja_reader_unref(ctx, node->reader);
      }
      node->buf.pos = 0;
      for (i = 0; i < n_in; i++) {
        rc = grn_ja_reader_seek(ctx, node->reader, in[i].id);
        if (rc == GRN_SUCCESS) {
          if (node->reader->ref_avail) {
            void *addr;
            rc = grn_ja_reader_ref(ctx, node->reader, &addr);
            if (rc == GRN_SUCCESS) {
              out_ptr[i].ptr = (char *)addr;
            }
          } else {
            rc = grn_ts_buf_reserve(ctx, &node->buf,
                                    node->buf.pos + node->reader->value_size);
            if (rc == GRN_SUCCESS) {
              rc = grn_ja_reader_read(ctx, node->reader,
                                      (char *)node->buf.ptr + node->buf.pos);
              if (rc == GRN_SUCCESS) {
                out_ptr[i].ptr = NULL;
                node->buf.pos += node->reader->value_size;
              }
            }
          }
        }
        if (rc == GRN_SUCCESS) {
          out_ptr[i].size = node->reader->value_size;
        } else {
          out_ptr[i].ptr = NULL;
          out_ptr[i].size = 0;
        }
      }
      buf_ptr = (char *)node->buf.ptr;
      for (i = 0; i < n_in; i++) {
        if (!out_ptr[i].ptr) {
          out_ptr[i].ptr = buf_ptr;
          buf_ptr += out_ptr[i].size;
        }
      }
      return GRN_SUCCESS;
    }
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_CASE(GEO, geo)
    case GRN_TS_REF: {
      size_t i;
      grn_ts_ref *out_ptr = (grn_ts_ref *)out;
      grn_ra *ra = (grn_ra *)node->column;
      grn_ra_cache cache;
      GRN_RA_CACHE_INIT(ra, &cache);
      for (i = 0; i < n_in; i++) {
        grn_ts_id *ptr = NULL;
        if (in[i].id) {
          ptr = (grn_ts_id *)grn_ra_ref_cache(ctx, ra, in[i].id, &cache);
        }
        out_ptr[i].id = ptr ? *ptr : GRN_ID_NIL;
        out_ptr[i].score = in[i].score;
      }
      GRN_RA_CACHE_FIN(ra, &cache);
      return GRN_SUCCESS;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_INT_CASE
#undef GRN_TS_EXPR_COLUMN_NODE_EVALUATE_SCALAR_CASE

/*
 * grn_ts_expr_column_node_evaluate_text_vector() outputs text vector column
 * values.
 */
static grn_rc
grn_ts_expr_column_node_evaluate_text_vector(grn_ctx *ctx,
                                             grn_ts_expr_column_node *node,
                                             const grn_ts_record *in,
                                             size_t n_in, void *out)
{
  grn_rc rc;
  char *buf_ptr;
  size_t i, j, n_bytes, n_values, total_n_bytes = 0, total_n_values = 0;
  grn_ts_text *text_ptr;
  grn_ts_text_vector *out_ptr = (grn_ts_text_vector *)out;
  /* Read encoded values into node->body_buf and get the size of each value. */
  node->body_buf.pos = 0;
  for (i = 0; i < n_in; i++) {
    char *ptr;
    rc = grn_ts_ja_get_value(ctx, node->column, in[i].id,
                             &node->body_buf, &n_bytes);
    if (rc == GRN_SUCCESS) {
      ptr = (char *)node->body_buf.ptr + total_n_bytes;
      GRN_B_DEC(n_values, ptr);
    } else {
      n_bytes = 0;
      n_values = 0;
    }
    grn_memcpy(&out_ptr[i].ptr, &n_bytes, sizeof(n_bytes));
    out_ptr[i].size = n_values;
    total_n_bytes += n_bytes;
    total_n_values += n_values;
  }
  /* Resize node->buf. */
  n_bytes = sizeof(grn_ts_text) * total_n_values;
  rc = grn_ts_buf_reserve(ctx, &node->buf, n_bytes);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  /* Decode values and compose the result. */
  buf_ptr = (char *)node->body_buf.ptr;
  text_ptr = (grn_ts_text *)node->buf.ptr;
  for (i = 0; i < n_in; i++) {
    char *ptr = buf_ptr;
    grn_memcpy(&n_bytes, &out_ptr[i].ptr, sizeof(n_bytes));
    buf_ptr += n_bytes;
    GRN_B_DEC(n_values, ptr);
    out_ptr[i].ptr = text_ptr;
    for (j = 0; j < out_ptr[i].size; j++) {
      GRN_B_DEC(text_ptr[j].size, ptr);
    }
    for (j = 0; j < out_ptr[i].size; j++) {
      text_ptr[j].ptr = ptr;
      ptr += text_ptr[j].size;
    }
    text_ptr += out_ptr[i].size;
  }
  return GRN_SUCCESS;
}

/*
 * grn_ts_expr_column_node_evaluate_ref_vector() outputs ref vector column
 * values.
 */
static grn_rc
grn_ts_expr_column_node_evaluate_ref_vector(grn_ctx *ctx,
                                             grn_ts_expr_column_node *node,
                                             const grn_ts_record *in,
                                             size_t n_in, void *out)
{
  grn_rc rc;
  size_t i, j, n_bytes, offset = 0;
  grn_ts_id *buf_ptr;
  grn_ts_ref *ref_ptr;
  grn_ts_ref_vector *out_ptr = (grn_ts_ref_vector *)out;
  /* Read column values into node->body_buf and get the size of each value. */
  node->body_buf.pos = 0;
  for (i = 0; i < n_in; i++) {
    size_t size;
    rc = grn_ts_ja_get_value(ctx, node->column, in[i].id,
                             &node->body_buf, &size);
    if (rc == GRN_SUCCESS) {
      out_ptr[i].size = size / sizeof(grn_ts_id);
      offset += out_ptr[i].size;
    } else {
      out_ptr[i].size = 0;
    }
  }
  /* Resize node->buf. */
  n_bytes = sizeof(grn_ts_ref) * offset;
  rc = grn_ts_buf_reserve(ctx, &node->buf, n_bytes);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  /* Compose the result. */
  buf_ptr = (grn_ts_id *)node->body_buf.ptr;
  ref_ptr = (grn_ts_ref *)node->buf.ptr;
  for (i = 0; i < n_in; i++) {
    out_ptr[i].ptr = ref_ptr;
    for (j = 0; j < out_ptr[i].size; j++, buf_ptr++, ref_ptr++) {
      ref_ptr->id = *buf_ptr;
      ref_ptr->score = in[i].score;
    }
  }
  return GRN_SUCCESS;
}

#define GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_CASE(KIND, kind)\
  case GRN_TS_ ## KIND ## _VECTOR: {\
    size_t i;\
    grn_ts_ ## kind *buf_ptr;\
    grn_ts_ ## kind ## _vector *out_ptr = (grn_ts_ ## kind ## _vector *)out;\
    /* Read column values into node->buf and save the size of each value. */\
    node->buf.pos = 0;\
    for (i = 0; i < n_in; i++) {\
      size_t n_bytes;\
      grn_rc rc = grn_ts_ja_get_value(ctx, node->column, in[i].id,\
                                      &node->buf, &n_bytes);\
      if (rc == GRN_SUCCESS) {\
        out_ptr[i].size = n_bytes / sizeof(grn_ts_ ## kind);\
      } else {\
        out_ptr[i].size = 0;\
      }\
    }\
    buf_ptr = (grn_ts_ ## kind *)node->buf.ptr;\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i].ptr = buf_ptr;\
      buf_ptr += out_ptr[i].size;\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(TYPE, type)\
  case GRN_DB_ ## TYPE: {\
    size_t i, j;\
    grn_ts_int *buf_ptr;\
    grn_ts_int_vector *out_ptr = (grn_ts_int_vector *)out;\
    /*
     * Read column values into body_buf and typecast the values to grn_ts_int.
     * Then, store the grn_ts_int values into node->buf and save the size of
     * each value.
     */\
    node->buf.pos = 0;\
    for (i = 0; i < n_in; i++) {\
      grn_rc rc;\
      size_t n_bytes, new_n_bytes;\
      node->body_buf.pos = 0;\
      rc = grn_ts_ja_get_value(ctx, node->column, in[i].id,\
                               &node->body_buf, &n_bytes);\
      if (rc == GRN_SUCCESS) {\
        out_ptr[i].size = n_bytes / sizeof(type ## _t);\
      } else {\
        out_ptr[i].size = 0;\
      }\
      new_n_bytes = node->buf.pos + (sizeof(grn_ts_int) * out_ptr[i].size);\
      rc = grn_ts_buf_reserve(ctx, &node->buf, new_n_bytes);\
      if (rc == GRN_SUCCESS) {\
        type ## _t *src_ptr = (type ## _t *)node->body_buf.ptr;\
        grn_ts_int *dest_ptr;\
        dest_ptr = (grn_ts_int *)((char *)node->buf.ptr + node->buf.pos);\
        for (j = 0; j < out_ptr[i].size; j++) {\
          dest_ptr[j] = (grn_ts_int)src_ptr[j];\
        }\
        node->buf.pos = new_n_bytes;\
      } else {\
        out_ptr[i].size = 0;\
      }\
    }\
    buf_ptr = (grn_ts_int *)node->buf.ptr;\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i].ptr = buf_ptr;\
      buf_ptr += out_ptr[i].size;\
    }\
    return GRN_SUCCESS;\
  }
/* grn_ts_expr_column_node_evaluate_vector() outputs vector column values. */
static grn_rc
grn_ts_expr_column_node_evaluate_vector(grn_ctx *ctx,
                                        grn_ts_expr_column_node *node,
                                        const grn_ts_record *in, size_t n_in,
                                        void *out)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_CASE(BOOL, bool)
    case GRN_TS_INT_VECTOR: {
      switch (node->data_type) {
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(INT8, int8)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(INT16, int16)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(INT32, int32)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(INT64, int64)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(UINT8, uint8)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(UINT16, uint16)
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(UINT32, uint32)
        /* The behavior is undefined if a value is greater than 2^63 - 1. */
        GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE(UINT64, uint64)
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data type: %d",
                            node->data_type);
        }
      }
    }
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_CASE(FLOAT, float)
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_CASE(TIME, time)
    case GRN_TS_TEXT_VECTOR: {
      return grn_ts_expr_column_node_evaluate_text_vector(ctx, node, in, n_in,
                                                          out);
    }
    GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_CASE(GEO, geo)
    case GRN_TS_REF_VECTOR: {
      return grn_ts_expr_column_node_evaluate_ref_vector(ctx, node, in, n_in,
                                                         out);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_INT_CASE
#undef GRN_TS_EXPR_COLUMN_NODE_EVALUATE_VECTOR_CASE

/* grn_ts_expr_column_node_evaluate() outputs column values. */
static grn_rc
grn_ts_expr_column_node_evaluate(grn_ctx *ctx, grn_ts_expr_column_node *node,
                                 const grn_ts_record *in, size_t n_in,
                                 void *out)
{
  if (node->data_kind & GRN_TS_VECTOR_FLAG) {
    return grn_ts_expr_column_node_evaluate_vector(ctx, node, in, n_in, out);
  } else {
    return grn_ts_expr_column_node_evaluate_scalar(ctx, node, in, n_in, out);
  }
}

/* grn_ts_expr_column_node_filter() filters records. */
static grn_rc
grn_ts_expr_column_node_filter(grn_ctx *ctx, grn_ts_expr_column_node *node,
                               grn_ts_record *in, size_t n_in,
                               grn_ts_record *out, size_t *n_out)
{
  size_t i, count = 0;
  grn_ra *ra = (grn_ra *)node->column;
  grn_ra_cache cache;
  GRN_RA_CACHE_INIT(ra, &cache);
  for (i = 0; i < n_in; i++) {
    grn_ts_bool *ptr = NULL;
    if (in[i].id) {
      ptr = grn_ra_ref_cache(ctx, ra, in[i].id, &cache);
    }
    if (ptr && *ptr) {
      out[count++] = in[i];
    }
  }
  GRN_RA_CACHE_FIN(ra, &cache);
  *n_out = count;
  return GRN_SUCCESS;
}

/* grn_ts_expr_column_node_adjust() updates scores. */
static grn_rc
grn_ts_expr_column_node_adjust(grn_ctx *ctx, grn_ts_expr_column_node *node,
                               grn_ts_record *io, size_t n_io)
{
  size_t i;
  grn_ra *ra = (grn_ra *)node->column;
  grn_ra_cache cache;
  GRN_RA_CACHE_INIT(ra, &cache);
  for (i = 0; i < n_io; i++) {
    grn_ts_float *ptr = NULL;
    if (io[i].id) {
      ptr = grn_ra_ref_cache(ctx, ra, io[i].id, &cache);
    }
    if (ptr) {
      io[i].score = (grn_ts_score)*ptr;
    }
  }
  GRN_RA_CACHE_FIN(ra, &cache);
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_expr_op_node.
 */

enum {
  GRN_TS_EXPR_OP_NODE_MAX_N_ARGS = 3,
  GRN_TS_EXPR_OP_NODE_N_BUFS = 3
};

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
  grn_ts_op_type op_type;
  grn_ts_expr_node *args[GRN_TS_EXPR_OP_NODE_MAX_N_ARGS];
  size_t n_args;
  grn_ts_buf bufs[GRN_TS_EXPR_OP_NODE_N_BUFS];
} grn_ts_expr_op_node;

/* grn_ts_expr_op_node_init() initializes a node. */
static void
grn_ts_expr_op_node_init(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  size_t i;
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_OP_NODE;
  for (i = 0; i < GRN_TS_EXPR_OP_NODE_MAX_N_ARGS; i++) {
    node->args[i] = NULL;
  }
  for (i = 0; i < GRN_TS_EXPR_OP_NODE_N_BUFS; i++) {
    grn_ts_buf_init(ctx, &node->bufs[i]);
  }
}

/* grn_ts_expr_op_node_fin() finalizes a node. */
static void
grn_ts_expr_op_node_fin(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  size_t i;
  for (i = 0; i < GRN_TS_EXPR_OP_NODE_N_BUFS; i++) {
    grn_ts_buf_fin(ctx, &node->bufs[i]);
  }
  for (i = 0; i < GRN_TS_EXPR_OP_NODE_MAX_N_ARGS; i++) {
    if (node->args[i]) {
      grn_ts_expr_node_close(ctx, node->args[i]);
    }
  }
}

/*
 * grn_ts_expr_op_node_deref_args_for_equal() resolves references if required.
 */
static grn_rc
grn_ts_expr_op_node_deref_args_for_equal(grn_ctx *ctx,
                                         grn_ts_expr_op_node *node)
{
  grn_rc rc;
  if (node->n_args != 2) {
    GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid #args: %" GRN_FMT_SIZE,
                      node->n_args);
  }
  if ((node->args[0]->data_kind & ~GRN_TS_VECTOR_FLAG) != GRN_TS_REF) {
    return grn_ts_expr_node_deref(ctx, &node->args[1]);
  }
  if ((node->args[1]->data_kind & ~GRN_TS_VECTOR_FLAG) != GRN_TS_REF) {
    return grn_ts_expr_node_deref(ctx, &node->args[0]);
  }

  /* FIXME: Arguments should be compared as references if possible. */
  rc = grn_ts_expr_node_deref(ctx, &node->args[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_node_deref(ctx, &node->args[1]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  return GRN_SUCCESS;
}

/* grn_ts_expr_op_node_deref_args() resolves references if required. */
static grn_rc
grn_ts_expr_op_node_deref_args(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  switch (node->op_type) {
    case GRN_TS_OP_EQUAL:
    case GRN_TS_OP_NOT_EQUAL: {
      return grn_ts_expr_op_node_deref_args_for_equal(ctx, node);
    }
    /* TODO: Add a ternary operator. */
    default: {
      size_t i;
      for (i = 0; i < node->n_args; i++) {
        grn_rc rc = grn_ts_expr_node_deref(ctx, &node->args[i]);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      return GRN_SUCCESS;
    }
  }
}

/*
 * grn_ts_op_plus_check_args() checks arguments. Note that arguments are
 * rearranged in some cases.
 */
static grn_rc
grn_ts_op_plus_check_args(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  grn_rc rc;
  if ((node->args[0]->data_kind == GRN_TS_INT) &&
      (node->args[1]->data_kind == GRN_TS_FLOAT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[0],
                                  1, &node->args[0]);
    if (rc != GRN_SUCCESS) {
      node->args[0] = NULL;
      return rc;
    }
  } else if ((node->args[0]->data_kind == GRN_TS_FLOAT) &&
             (node->args[1]->data_kind == GRN_TS_INT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[1],
                                  1, &node->args[1]);
    if (rc != GRN_SUCCESS) {
      node->args[1] = NULL;
      return rc;
    }
  }

  switch (node->args[0]->data_kind) {
    case GRN_TS_INT: {
      switch (node->args[1]->data_kind) {
        case GRN_TS_INT: {
          /* Int + Int = Int. */
          node->data_kind = GRN_TS_INT;
          node->data_type = GRN_DB_INT64;
          return GRN_SUCCESS;
        }
        case GRN_TS_TIME: {
          /* Int + Time = Time + Int = Time. */
          grn_ts_expr_node *tmp = node->args[0];
          node->args[0] = node->args[1];
          node->args[1] = tmp;
          node->data_kind = GRN_TS_TIME;
          node->data_type = GRN_DB_TIME;
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[1]->data_kind);
        }
      }
    }
    case GRN_TS_FLOAT: {
      switch (node->args[1]->data_kind) {
        case GRN_TS_FLOAT: {
          /* Float + Float = Float. */
          node->data_kind = GRN_TS_FLOAT;
          node->data_type = GRN_DB_FLOAT;
          return GRN_SUCCESS;
        }
        case GRN_TS_TIME: {
          /* Float + Time = Time + Float = Time. */
          grn_ts_expr_node *tmp = node->args[0];
          node->args[0] = node->args[1];
          node->args[1] = tmp;
          node->data_kind = GRN_TS_TIME;
          node->data_type = GRN_DB_TIME;
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[1]->data_kind);
        }
      }
    }
    case GRN_TS_TIME: {
      switch (node->args[1]->data_kind) {
        case GRN_TS_INT:
        case GRN_TS_FLOAT: {
          /* Time + Int or Float = Time. */
          node->data_kind = GRN_TS_TIME;
          node->data_type = GRN_DB_TIME;
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[1]->data_kind);
        }
      }
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}

/* grn_ts_op_minus_check_args() checks arguments. */
static grn_rc
grn_ts_op_minus_check_args(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  grn_rc rc;
  if ((node->args[0]->data_kind == GRN_TS_INT) &&
      (node->args[1]->data_kind == GRN_TS_FLOAT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[0],
                                  1, &node->args[0]);
    if (rc != GRN_SUCCESS) {
      node->args[0] = NULL;
      return rc;
    }
  } else if ((node->args[0]->data_kind == GRN_TS_FLOAT) &&
             (node->args[1]->data_kind == GRN_TS_INT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[1],
                                  1, &node->args[1]);
    if (rc != GRN_SUCCESS) {
      node->args[1] = NULL;
      return rc;
    }
  }

  switch (node->args[0]->data_kind) {
    case GRN_TS_INT: {
      if (node->args[1]->data_kind != GRN_TS_INT) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                          node->args[1]->data_kind);
      }
      /* Int - Int = Int. */
      node->data_kind = GRN_TS_INT;
      node->data_type = GRN_DB_INT64;
      return GRN_SUCCESS;
    }
    case GRN_TS_FLOAT: {
      if (node->args[1]->data_kind != GRN_TS_FLOAT) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                          node->args[1]->data_kind);
      }
      /* Float - Float = Float. */
      node->data_kind = GRN_TS_FLOAT;
      node->data_type = GRN_DB_FLOAT;
      return GRN_SUCCESS;
    }
    case GRN_TS_TIME: {
      switch (node->args[1]->data_kind) {
        case GRN_TS_INT:
        case GRN_TS_FLOAT: {
          /* Time - Int or Float = Time. */
          node->data_kind = GRN_TS_TIME;
          node->data_type = GRN_DB_TIME;
          return GRN_SUCCESS;
        }
        case GRN_TS_TIME: {
          /* Time - Time = Float. */
          node->data_kind = GRN_TS_FLOAT;
          node->data_type = GRN_DB_FLOAT;
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[1]->data_kind);
        }
      }
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}

/*
 * grn_ts_expr_op_node_typecast_args_for_cmp() inserts a typecast operator for
 * comparison.
 */
static grn_rc
grn_ts_expr_op_node_typecast_args_for_cmp(grn_ctx *ctx,
                                          grn_ts_expr_op_node *node)
{
  grn_rc rc;
  if ((node->args[0]->data_kind == GRN_TS_INT) &&
      (node->args[1]->data_kind == GRN_TS_FLOAT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[0],
                                  1, &node->args[0]);
    if (rc != GRN_SUCCESS) {
      node->args[0] = NULL;
      return rc;
    }
  } else if ((node->args[0]->data_kind == GRN_TS_FLOAT) &&
             (node->args[1]->data_kind == GRN_TS_INT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[1],
                                  1, &node->args[1]);
    if (rc != GRN_SUCCESS) {
      node->args[1] = NULL;
      return rc;
    }
  } else if ((node->args[0]->data_kind == GRN_TS_TIME) &&
             (node->args[1]->data_kind == GRN_TS_TEXT)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_TIME, &node->args[1],
                                  1, &node->args[1]);
    if (rc != GRN_SUCCESS) {
      node->args[1] = NULL;
      return rc;
    }
  } else if ((node->args[0]->data_kind == GRN_TS_TEXT) &&
             (node->args[1]->data_kind == GRN_TS_TIME)) {
    rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_TIME, &node->args[0],
                                  1, &node->args[0]);
    if (rc != GRN_SUCCESS) {
      node->args[0] = NULL;
      return rc;
    }
  } else {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "data kind conflict: %d != %d",
                      node->args[0]->data_kind,
                      node->args[1]->data_kind);
  }
  return GRN_SUCCESS;
}

/*
 * grn_ts_expr_op_node_check_args() checks the combination of an operator and
 * its arguments.
 */
static grn_rc
grn_ts_expr_op_node_check_args(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  switch (node->op_type) {
    case GRN_TS_OP_LOGICAL_NOT: {
      if (node->args[0]->data_kind != GRN_TS_BOOL) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                          node->args[0]->data_kind);
      }
      node->data_kind = GRN_TS_BOOL;
      node->data_type = GRN_DB_BOOL;
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_BITWISE_NOT: {
      switch (node->args[0]->data_kind) {
        case GRN_TS_BOOL:
        case GRN_TS_INT: {
          node->data_kind = node->args[0]->data_kind;
          node->data_type = grn_ts_data_kind_to_type(node->data_kind);
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[0]->data_kind);
        }
      }
    }
    case GRN_TS_OP_POSITIVE:
    case GRN_TS_OP_NEGATIVE: {
      if ((node->args[0]->data_kind != GRN_TS_INT) &&
          (node->args[0]->data_kind != GRN_TS_FLOAT)) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                          node->args[0]->data_kind);
      }
      node->data_kind = node->args[0]->data_kind;
      node->data_type = grn_ts_data_kind_to_type(node->data_kind);
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_FLOAT: {
      if (node->args[0]->data_kind != GRN_TS_INT) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                          node->args[0]->data_kind);
      }
      node->data_kind = GRN_TS_FLOAT;
      node->data_type = GRN_DB_FLOAT;
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_TIME: {
      if (node->args[0]->data_kind != GRN_TS_TEXT) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                          node->args[0]->data_kind);
      }
      node->data_kind = GRN_TS_TIME;
      node->data_type = GRN_DB_TIME;
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_LOGICAL_AND:
    case GRN_TS_OP_LOGICAL_OR:
    case GRN_TS_OP_LOGICAL_SUB: {
      if ((node->args[0]->data_kind != GRN_TS_BOOL) ||
          (node->args[1]->data_kind != GRN_TS_BOOL)) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d, %d",
                          node->args[0]->data_kind, node->args[1]->data_kind);
      }
      node->data_kind = GRN_TS_BOOL;
      node->data_type = GRN_DB_BOOL;
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_BITWISE_AND:
    case GRN_TS_OP_BITWISE_OR:
    case GRN_TS_OP_BITWISE_XOR: {
      if (node->args[0]->data_kind != node->args[1]->data_kind) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "data kind conflict: %d != %d",
                          node->args[0]->data_kind, node->args[1]->data_kind);
      }
      switch (node->args[0]->data_kind) {
        case GRN_TS_BOOL:
        case GRN_TS_INT: {
          node->data_kind = node->args[0]->data_kind;
          node->data_type = grn_ts_data_kind_to_type(node->data_kind);
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[0]->data_kind);
        }
      }
      node->data_kind = GRN_TS_BOOL;
      node->data_type = GRN_DB_BOOL;
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_EQUAL:
    case GRN_TS_OP_NOT_EQUAL: {
      grn_ts_data_kind scalar_data_kind;
      if (node->args[0]->data_kind != node->args[1]->data_kind) {
        grn_rc rc = grn_ts_expr_op_node_typecast_args_for_cmp(ctx, node);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      scalar_data_kind = node->args[0]->data_kind & ~GRN_TS_VECTOR_FLAG;
      if (((scalar_data_kind == GRN_TS_REF) ||
           (scalar_data_kind == GRN_TS_GEO)) &&
          (node->args[0]->data_type != node->args[1]->data_type)) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "data type conflict: %d != %d",
                          node->args[0]->data_type, node->args[1]->data_type);
      }
      node->data_kind = GRN_TS_BOOL;
      node->data_type = GRN_DB_BOOL;
      return GRN_SUCCESS;
    }
    case GRN_TS_OP_LESS:
    case GRN_TS_OP_LESS_EQUAL:
    case GRN_TS_OP_GREATER:
    case GRN_TS_OP_GREATER_EQUAL: {
      if (node->args[0]->data_kind != node->args[1]->data_kind) {
        grn_rc rc = grn_ts_expr_op_node_typecast_args_for_cmp(ctx, node);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      switch (node->args[0]->data_kind) {
        case GRN_TS_INT:
        case GRN_TS_FLOAT:
        case GRN_TS_TIME:
        case GRN_TS_TEXT:
        case GRN_TS_INT_VECTOR:
        case GRN_TS_FLOAT_VECTOR:
        case GRN_TS_TIME_VECTOR:
        case GRN_TS_TEXT_VECTOR: {
          node->data_kind = GRN_TS_BOOL;
          node->data_type = GRN_DB_BOOL;
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                            node->args[0]->data_kind);
        }
      }
      case GRN_TS_OP_SHIFT_ARITHMETIC_LEFT:
      case GRN_TS_OP_SHIFT_ARITHMETIC_RIGHT:
      case GRN_TS_OP_SHIFT_LOGICAL_LEFT:
      case GRN_TS_OP_SHIFT_LOGICAL_RIGHT: {
        if ((node->args[0]->data_kind != GRN_TS_INT) ||
            (node->args[1]->data_kind != GRN_TS_INT)) {
          GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d, %d",
                            node->args[0]->data_kind,
                            node->args[1]->data_kind);
        }
        node->data_kind = GRN_TS_INT;
        node->data_type = GRN_DB_INT64;
        return GRN_SUCCESS;
      }
      case GRN_TS_OP_PLUS: {
        return grn_ts_op_plus_check_args(ctx, node);
      }
      case GRN_TS_OP_MINUS: {
        return grn_ts_op_minus_check_args(ctx, node);
      }
      case GRN_TS_OP_MULTIPLICATION:
      case GRN_TS_OP_DIVISION:
      case GRN_TS_OP_MODULUS: {
        if (node->args[0]->data_kind != node->args[1]->data_kind) {
          grn_rc rc;
          if ((node->args[0]->data_kind == GRN_TS_INT) &&
              (node->args[1]->data_kind == GRN_TS_FLOAT)) {
            rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[0],
                                          1, &node->args[0]);
            if (rc != GRN_SUCCESS) {
              node->args[0] = NULL;
              return rc;
            }
          } else if ((node->args[0]->data_kind == GRN_TS_FLOAT) &&
                     (node->args[1]->data_kind == GRN_TS_INT)) {
            rc = grn_ts_expr_op_node_open(ctx, GRN_TS_OP_FLOAT, &node->args[1],
                                          1, &node->args[1]);
            if (rc != GRN_SUCCESS) {
              node->args[1] = NULL;
              return rc;
            }
          } else {
            GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                              "data kind conflict: %d != %d",
                              node->args[0]->data_kind,
                              node->args[1]->data_kind);
          }
        }
        switch (node->args[0]->data_kind) {
          case GRN_TS_INT:
          case GRN_TS_FLOAT: {
            node->data_kind = node->args[0]->data_kind;
            node->data_type = grn_ts_data_kind_to_type(node->data_kind);
            return GRN_SUCCESS;
          }
          default: {
            GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                              node->args[0]->data_kind);
          }
        }
      }
    }
    case GRN_TS_OP_MATCH:
    case GRN_TS_OP_PREFIX_MATCH:
    case GRN_TS_OP_SUFFIX_MATCH: {
      if ((node->args[0]->data_kind != GRN_TS_TEXT) ||
          (node->args[1]->data_kind != GRN_TS_TEXT)) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d, %d",
                          node->args[0]->data_kind,
                          node->args[1]->data_kind);
      }
      node->data_kind = GRN_TS_BOOL;
      node->data_type = GRN_DB_BOOL;
      return GRN_SUCCESS;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid operator: %d",
                        node->op_type);
    }
  }
}

/* grn_ts_expr_op_node_setup() sets up an operator node. */
static grn_rc
grn_ts_expr_op_node_setup(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  grn_rc rc = grn_ts_expr_op_node_deref_args(ctx, node);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_op_node_check_args(ctx, node);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (node->data_kind == GRN_TS_VOID) {
    GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                      GRN_TS_VOID);
  } else if (node->data_type == GRN_DB_VOID) {
    GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data type: %d",
                      GRN_DB_VOID);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_op_node_open(grn_ctx *ctx, grn_ts_op_type op_type,
                         grn_ts_expr_node **args, size_t n_args,
                         grn_ts_expr_node **node)
{
  size_t i;
  grn_rc rc;
  grn_ts_expr_op_node *new_node = GRN_MALLOCN(grn_ts_expr_op_node, 1);
  if (!new_node) {
    for (i = 0; i < n_args; i++) {
      grn_ts_expr_node_close(ctx, args[i]);
    }
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_op_node));
  }
  grn_ts_expr_op_node_init(ctx, new_node);
  new_node->op_type = op_type;
  for (i = 0; i < n_args; i++) {
    new_node->args[i] = args[i];
  }
  new_node->n_args = n_args;
  rc = grn_ts_expr_op_node_setup(ctx, new_node);
  if (rc != GRN_SUCCESS) {
    grn_ts_expr_op_node_fin(ctx, new_node);
    GRN_FREE(new_node);
    return rc;
  }
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_op_node_close() destroys a node. */
static void
grn_ts_expr_op_node_close(grn_ctx *ctx, grn_ts_expr_op_node *node)
{
  grn_ts_expr_op_node_fin(ctx, node);
  GRN_FREE(node);
}

/* grn_ts_op_logical_not_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_logical_not_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i;
  grn_ts_bool *out_ptr = (grn_ts_bool *)out;
  grn_rc rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (i = 0; i < n_in; i++) {
    out_ptr[i] = grn_ts_op_logical_not_bool(out_ptr[i]);
  }
  return GRN_SUCCESS;
}

/* grn_ts_op_bitwise_not_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_bitwise_not_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i;
  grn_rc rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  switch (node->data_kind) {
    case GRN_TS_BOOL: {
      grn_ts_bool *out_ptr = (grn_ts_bool *)out;
      for (i = 0; i < n_in; i++) {
        out_ptr[i] = grn_ts_op_bitwise_not_bool(out_ptr[i]);
      }
      return GRN_SUCCESS;
    }
    case GRN_TS_INT: {
      grn_ts_int *out_ptr = (grn_ts_int *)out;
      for (i = 0; i < n_in; i++) {
        out_ptr[i] = grn_ts_op_bitwise_not_int(out_ptr[i]);
      }
      return GRN_SUCCESS;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}

#define GRN_TS_OP_SIGN_EVALUATE_CASE(type, KIND, kind) \
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i] = grn_ts_op_ ## type ## _ ## kind(out_ptr[i]);\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_OP_SIGN_EVALUATE(type) \
  size_t i;\
  grn_rc rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
  if (rc != GRN_SUCCESS) {\
    return rc;\
  }\
  switch (node->data_kind) {\
    GRN_TS_OP_SIGN_EVALUATE_CASE(type, INT, int)\
    GRN_TS_OP_SIGN_EVALUATE_CASE(type, FLOAT, float)\
    default: {\
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",\
                        node->data_kind);\
    }\
  }
/* grn_ts_op_positive_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_positive_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                            const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_SIGN_EVALUATE(positive)
}

/* grn_ts_op_negative_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_negative_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                            const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_SIGN_EVALUATE(negative)
}
#undef GRN_TS_OP_SIGN_EVALUATE
#undef GRN_TS_OP_SIGN_EVALUATE_CASE

/* grn_ts_op_float_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_float_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                         const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i;
  grn_ts_int *buf_ptr;
  grn_ts_float *out_ptr = (grn_ts_float *)out;
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                               &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptr = (grn_ts_int *)node->bufs[0].ptr;
  for (i = 0; i < n_in; i++) {
    rc = grn_ts_op_float(ctx, buf_ptr[i], &out_ptr[i]);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_op_time_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_time_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                        const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i;
  grn_ts_text *buf_ptr;
  grn_ts_time *out_ptr = (grn_ts_time *)out;
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                               &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptr = (grn_ts_text *)node->bufs[0].ptr;
  for (i = 0; i < n_in; i++) {
    rc = grn_ts_op_time(ctx, buf_ptr[i], &out_ptr[i]);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_op_logical_and_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_logical_and_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i, j, count;
  grn_rc rc;
  grn_ts_bool *buf_ptrs[2], *out_ptr = (grn_ts_bool *)out;
  grn_ts_buf *tmp_in_buf = &node->bufs[2];
  grn_ts_record *tmp_in;

  /* Evaluate the 1st argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                        &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptrs[0] = (grn_ts_bool *)node->bufs[0].ptr;

  /* Create a list of true records. */
  rc = grn_ts_buf_reserve(ctx, tmp_in_buf, sizeof(grn_ts_record) * n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp_in = (grn_ts_record *)tmp_in_buf->ptr;
  count = 0;
  for (i = 0; i < n_in; i++) {
    if (buf_ptrs[0][i]) {
      tmp_in[count++] = in[i];
    }
  }

  /* Evaluate the 2nd argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1], tmp_in, count,
                                        &node->bufs[1]);
  buf_ptrs[1] = (grn_ts_bool *)node->bufs[1].ptr;

  /* Merge the results. */
  count = 0;
  for (i = 0, j = 0; i < n_in; i++) {
    out_ptr[count++] = buf_ptrs[0][i] && buf_ptrs[1][j++];
  }
  return GRN_SUCCESS;
}

/* grn_ts_op_logical_or_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_logical_or_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i, j, count;
  grn_rc rc;
  grn_ts_bool *buf_ptrs[2], *out_ptr = (grn_ts_bool *)out;
  grn_ts_buf *tmp_in_buf = &node->bufs[2];
  grn_ts_record *tmp_in;

  /* Evaluate the 1st argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                        &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptrs[0] = (grn_ts_bool *)node->bufs[0].ptr;

  /* Create a list of false records. */
  rc = grn_ts_buf_reserve(ctx, tmp_in_buf, sizeof(grn_ts_record) * n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp_in = (grn_ts_record *)tmp_in_buf->ptr;
  count = 0;
  for (i = 0; i < n_in; i++) {
    if (!buf_ptrs[0][i]) {
      tmp_in[count++] = in[i];
    }
  }

  /* Evaluate the 2nd argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1], tmp_in, count,
                                        &node->bufs[1]);
  buf_ptrs[1] = (grn_ts_bool *)node->bufs[1].ptr;

  /* Merge the results. */
  count = 0;
  for (i = 0, j = 0; i < n_in; i++) {
    out_ptr[count++] = buf_ptrs[0][i] || buf_ptrs[1][j++];
  }
  return GRN_SUCCESS;
}

/* grn_ts_op_logical_sub_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_logical_sub_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  size_t i, j, count;
  grn_rc rc;
  grn_ts_bool *buf_ptrs[2], *out_ptr = (grn_ts_bool *)out;
  grn_ts_buf *tmp_in_buf = &node->bufs[2];
  grn_ts_record *tmp_in;

  /* Evaluate the 1st argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                        &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptrs[0] = (grn_ts_bool *)node->bufs[0].ptr;

  /* Create a list of true records. */
  rc = grn_ts_buf_reserve(ctx, tmp_in_buf, sizeof(grn_ts_record) * n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp_in = (grn_ts_record *)tmp_in_buf->ptr;
  count = 0;
  for (i = 0; i < n_in; i++) {
    if (buf_ptrs[0][i]) {
      tmp_in[count++] = in[i];
    }
  }

  /* Evaluate the 2nd argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1], tmp_in, count,
                                        &node->bufs[1]);
  buf_ptrs[1] = (grn_ts_bool *)node->bufs[1].ptr;

  /* Merge the results. */
  count = 0;
  for (i = 0, j = 0; i < n_in; i++) {
    out_ptr[count++] = buf_ptrs[0][i] &&
                       grn_ts_op_logical_not_bool(buf_ptrs[1][j++]);
  }
  return GRN_SUCCESS;
}

#define GRN_TS_OP_BITWISE_EVALUATE_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    /*
     * Use the output buffer to put evaluation results of the 1st argument,
     * because the data kind is same.
     */\
    size_t i;\
    grn_rc rc;\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
    if (rc == GRN_SUCCESS) {\
      rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1],\
                                            in, n_in, &node->bufs[0]);\
      if (rc == GRN_SUCCESS) {\
        grn_ts_ ## kind *buf_ptr = (grn_ts_ ## kind *)node->bufs[0].ptr;\
        for (i = 0; i < n_in; i++) {\
          out_ptr[i] = grn_ts_op_bitwise_ ## type ## _ ## kind(out_ptr[i],\
                                                               buf_ptr[i]);\
        }\
      }\
    }\
    return rc;\
  }
/* grn_ts_op_bitwise_and_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_bitwise_and_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->args[0]->data_kind) {
    GRN_TS_OP_BITWISE_EVALUATE_CASE(and, BOOL, bool)
    GRN_TS_OP_BITWISE_EVALUATE_CASE(and, INT, int)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}

/* grn_ts_op_bitwise_or_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_bitwise_or_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                              const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->args[0]->data_kind) {
    GRN_TS_OP_BITWISE_EVALUATE_CASE(or, BOOL, bool)
    GRN_TS_OP_BITWISE_EVALUATE_CASE(or, INT, int)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}

/* grn_ts_op_bitwise_xor_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_bitwise_xor_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->args[0]->data_kind) {
    GRN_TS_OP_BITWISE_EVALUATE_CASE(xor, BOOL, bool)
    GRN_TS_OP_BITWISE_EVALUATE_CASE(xor, INT, int)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}
#undef GRN_TS_OP_BITWISE_EVALUATE_CASE

#define GRN_TS_OP_CHK_EVALUATE_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *buf_ptrs[] = {\
      (grn_ts_ ## kind *)node->bufs[0].ptr,\
      (grn_ts_ ## kind *)node->bufs[1].ptr\
    };\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i] = grn_ts_op_ ## type ## _ ## kind(buf_ptrs[0][i],\
                                                   buf_ptrs[1][i]);\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, KIND, kind)\
  GRN_TS_OP_CHK_EVALUATE_CASE(type, KIND ## _VECTOR, kind ## _vector)
#define GRN_TS_OP_CHK_EVALUATE(type)\
  size_t i;\
  grn_rc rc;\
  grn_ts_bool *out_ptr = (grn_ts_bool *)out;\
  if (node->args[0]->data_kind == GRN_TS_BOOL) {\
    /*
     * Use the output buffer to put evaluation results of the 1st argument,
     * because the data kind is same.
     */\
    rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
    if (rc == GRN_SUCCESS) {\
      grn_ts_buf *buf = &node->bufs[0];\
      rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1],\
                                            in, n_in, buf);\
      if (rc == GRN_SUCCESS) {\
        grn_ts_bool *buf_ptr = (grn_ts_bool *)buf->ptr;\
        for (i = 0; i < n_in; i++) {\
          out_ptr[i] = grn_ts_op_ ## type ## _bool(out_ptr[i], buf_ptr[i]);\
        }\
      }\
    }\
    return rc;\
  }\
  for (i = 0; i < 2; i++) {\
    rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                          &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  switch (node->args[0]->data_kind) {\
    GRN_TS_OP_CHK_EVALUATE_CASE(type, INT, int)\
    GRN_TS_OP_CHK_EVALUATE_CASE(type, FLOAT, float)\
    GRN_TS_OP_CHK_EVALUATE_CASE(type, TIME, time)\
    GRN_TS_OP_CHK_EVALUATE_CASE(type, TEXT, text)\
    GRN_TS_OP_CHK_EVALUATE_CASE(type, GEO, geo)\
    GRN_TS_OP_CHK_EVALUATE_CASE(type, REF, ref)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, BOOL, bool)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, INT, int)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, FLOAT, float)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, TIME, time)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, TEXT, text)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, GEO, geo)\
    GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE(type, REF, ref)\
    default: {\
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",\
                        node->args[0]->data_kind);\
    }\
  }
/* grn_ts_op_equal_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_equal_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                         const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_CHK_EVALUATE(equal)
}

/* grn_ts_op_not_equal_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_not_equal_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_CHK_EVALUATE(not_equal)
}
#undef GRN_TS_OP_CHK_EVALUATE
#undef GRN_TS_OP_CHK_EVALUATE_VECTOR_CASE
#undef GRN_TS_OP_CHK_EVALUATE_CASE

#define GRN_TS_OP_CMP_EVALUATE_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *buf_ptrs[] = {\
      (grn_ts_ ## kind *)node->bufs[0].ptr,\
      (grn_ts_ ## kind *)node->bufs[1].ptr\
    };\
    for (i = 0; i < n_in; i++) {\
      out_ptr[i] = grn_ts_op_ ## type ## _ ## kind(buf_ptrs[0][i],\
                                                   buf_ptrs[1][i]);\
    }\
    return GRN_SUCCESS;\
  }
#define GRN_TS_OP_CMP_EVALUATE_VECTOR_CASE(type, KIND, kind)\
  GRN_TS_OP_CMP_EVALUATE_CASE(type, KIND ## _VECTOR, kind ## _vector)
#define GRN_TS_OP_CMP_EVALUATE(type)\
  size_t i;\
  grn_rc rc;\
  grn_ts_bool *out_ptr = (grn_ts_bool *)out;\
  for (i = 0; i < 2; i++) {\
    rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                          &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  switch (node->args[0]->data_kind) {\
    GRN_TS_OP_CMP_EVALUATE_CASE(type, INT, int)\
    GRN_TS_OP_CMP_EVALUATE_CASE(type, FLOAT, float)\
    GRN_TS_OP_CMP_EVALUATE_CASE(type, TIME, time)\
    GRN_TS_OP_CMP_EVALUATE_CASE(type, TEXT, text)\
    GRN_TS_OP_CMP_EVALUATE_VECTOR_CASE(type, INT, int)\
    GRN_TS_OP_CMP_EVALUATE_VECTOR_CASE(type, FLOAT, float)\
    GRN_TS_OP_CMP_EVALUATE_VECTOR_CASE(type, TIME, time)\
    GRN_TS_OP_CMP_EVALUATE_VECTOR_CASE(type, TEXT, text)\
    default: {\
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",\
                        node->args[0]->data_kind);\
    }\
  }
/* grn_ts_op_less_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_less_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                        const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_CMP_EVALUATE(less)
}

/* grn_ts_op_less_equal_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_less_equal_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                              const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_CMP_EVALUATE(less_equal)
}

/* grn_ts_op_greater_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_greater_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                           const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_CMP_EVALUATE(greater)
}

/* grn_ts_op_greater_equal_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_greater_equal_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                                 const grn_ts_record *in, size_t n_in,
                                 void *out)
{
  GRN_TS_OP_CMP_EVALUATE(greater_equal)
}
#undef GRN_TS_OP_CMP_EVALUATE
#undef GRN_TS_OP_CMP_EVALUATE_VECTOR_CASE
#undef GRN_TS_OP_CMP_EVALUATE_CASE

#define GRN_TS_OP_SHIFT_EVALUATE(type)\
  size_t i;\
  grn_rc rc;\
  grn_ts_int *out_ptr = (grn_ts_int *)out;\
  rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
  if (rc == GRN_SUCCESS) {\
    rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1],\
                                          in, n_in, &node->bufs[0]);\
    if (rc == GRN_SUCCESS) {\
      grn_ts_int *buf_ptr = (grn_ts_int *)node->bufs[0].ptr;\
      for (i = 0; i < n_in; i++) {\
        out_ptr[i] = grn_ts_op_shift_ ## type(out_ptr[i], buf_ptr[i]);\
      }\
    }\
  }\
  return rc;
/* grn_ts_op_shift_arithmetic_left_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_shift_arithmetic_left_evaluate(grn_ctx *ctx,
                                         grn_ts_expr_op_node *node,
                                         const grn_ts_record *in, size_t n_in,
                                         void *out)
{
  GRN_TS_OP_SHIFT_EVALUATE(arithmetic_left)
}

/* grn_ts_op_shift_arithmetic_right_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_shift_arithmetic_right_evaluate(grn_ctx *ctx,
                                          grn_ts_expr_op_node *node,
                                          const grn_ts_record *in, size_t n_in,
                                          void *out)
{
  GRN_TS_OP_SHIFT_EVALUATE(arithmetic_right)
}

/* grn_ts_op_shift_logical_left_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_shift_logical_left_evaluate(grn_ctx *ctx,
                                      grn_ts_expr_op_node *node,
                                      const grn_ts_record *in, size_t n_in,
                                      void *out)
{
  GRN_TS_OP_SHIFT_EVALUATE(logical_left)
}

/* grn_ts_op_shift_logical_right_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_shift_logical_right_evaluate(grn_ctx *ctx,
                                       grn_ts_expr_op_node *node,
                                       const grn_ts_record *in, size_t n_in,
                                       void *out)
{
  GRN_TS_OP_SHIFT_EVALUATE(logical_right)
}
#undef GRN_TS_OP_SHIFT_EVALUATE

#define GRN_TS_OP_ARITH_EVALUATE(type, lhs_kind, rhs_kind)\
  /*
   * Use the output buffer to put evaluation results of the 1st argument,
   * because the data kind is same.
   */\
  size_t i;\
  grn_rc rc;\
  grn_ts_ ## lhs_kind *out_ptr = (grn_ts_ ## lhs_kind *)out;\
  rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
  if (rc == GRN_SUCCESS) {\
    rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1],\
                                          in, n_in, &node->bufs[0]);\
    if (rc == GRN_SUCCESS) {\
      grn_ts_ ## rhs_kind *buf_ptr = (grn_ts_ ## rhs_kind *)node->bufs[0].ptr;\
      for (i = 0; i < n_in; i++) {\
        rc = grn_ts_op_ ## type ## _ ## lhs_kind ## _ ## rhs_kind(\
          ctx, out_ptr[i], buf_ptr[i], &out_ptr[i]);\
        if (rc != GRN_SUCCESS) {\
          return rc;\
        }\
      }\
    }\
  }\
  return rc;

#define GRN_TS_OP_ARITH_EVALUATE_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    /*
     * Use the output buffer to put evaluation results of the 1st argument,
     * because the data kind is same.
     */\
    size_t i;\
    grn_rc rc;\
    grn_ts_ ## kind *out_ptr = (grn_ts_ ## kind *)out;\
    rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
    if (rc == GRN_SUCCESS) {\
      rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1],\
                                            in, n_in, &node->bufs[0]);\
      if (rc == GRN_SUCCESS) {\
        grn_ts_ ## kind *buf_ptr = (grn_ts_ ## kind *)node->bufs[0].ptr;\
        for (i = 0; i < n_in; i++) {\
          out_ptr[i] = grn_ts_op_ ## type ## _ ## kind(out_ptr[i],\
                                                       buf_ptr[i]);\
        }\
      }\
    }\
    return rc;\
  }
#define GRN_TS_OP_ARITH_EVALUATE_TIME_CASE(type, KIND, lhs, rhs)\
  case GRN_TS_ ## KIND: {\
    /*
     * Use the output buffer to put evaluation results of the 1st argument,
     * because the data kind is same.
     */\
    size_t i;\
    grn_rc rc;\
    grn_ts_ ## lhs *out_ptr = (grn_ts_ ## lhs *)out;\
    rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, out);\
    if (rc == GRN_SUCCESS) {\
      rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1],\
                                            in, n_in, &node->bufs[0]);\
      if (rc == GRN_SUCCESS) {\
        grn_ts_ ## rhs *buf_ptr = (grn_ts_ ## rhs *)node->bufs[0].ptr;\
        for (i = 0; i < n_in; i++) {\
          out_ptr[i] = grn_ts_op_ ## type ## _ ## lhs ## _ ## rhs(out_ptr[i],\
                                                                  buf_ptr[i]);\
        }\
      }\
    }\
    return rc;\
  }
/* grn_ts_op_plus_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_plus_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                        const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->args[0]->data_kind) {
    case GRN_TS_INT: {
      GRN_TS_OP_ARITH_EVALUATE(plus, int, int)
    }
    case GRN_TS_FLOAT: {
      GRN_TS_OP_ARITH_EVALUATE(plus, float, float)
    }
    case GRN_TS_TIME: {
      switch (node->args[1]->data_kind) {
        case GRN_TS_INT: {
          GRN_TS_OP_ARITH_EVALUATE(plus, time, int)
        }
        case GRN_TS_FLOAT: {
          GRN_TS_OP_ARITH_EVALUATE(plus, time, float)
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "data kind conflict: %d, %d",
                            node->args[0]->data_kind,
                            node->args[1]->data_kind);
        }
      }
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}

/* grn_ts_op_minus_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_minus_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                         const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->args[0]->data_kind) {
    case GRN_TS_INT: {
      GRN_TS_OP_ARITH_EVALUATE(minus, int, int)
    }
    case GRN_TS_FLOAT: {
      GRN_TS_OP_ARITH_EVALUATE(minus, float, float)
    }
    case GRN_TS_TIME: {
      switch (node->args[1]->data_kind) {
        case GRN_TS_INT: {
          GRN_TS_OP_ARITH_EVALUATE(minus, time, int)
        }
        case GRN_TS_FLOAT: {
          GRN_TS_OP_ARITH_EVALUATE(minus, time, float)
        }
        case GRN_TS_TIME: {
          size_t i;
          grn_rc rc;
          grn_ts_float *out_ptr = (grn_ts_float *)out;
          grn_ts_time *buf_ptrs[2];
          rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                                &node->bufs[0]);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
          rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1], in, n_in,
                                                &node->bufs[1]);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
          buf_ptrs[0] = (grn_ts_time *)node->bufs[0].ptr;
          buf_ptrs[1] = (grn_ts_time *)node->bufs[1].ptr;
          for (i = 0; i < n_in; i++) {
            rc = grn_ts_op_minus_time_time(ctx, buf_ptrs[0][i], buf_ptrs[1][i],
                                           &out_ptr[i]);
            if (rc != GRN_SUCCESS) {
              return rc;
            }
          }
          return GRN_SUCCESS;
        }
        default: {
          GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "data kind conflict: %d, %d",
                            node->args[0]->data_kind,
                            node->args[1]->data_kind);
        }
      }
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->args[0]->data_kind);
    }
  }
}
#undef GRN_TS_OP_ARITH_EVALUATE_TIME_CASE

/* grn_ts_op_multiplication_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_multiplication_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                                  const grn_ts_record *in, size_t n_in,
                                  void *out)
{
  switch (node->data_kind) {
    case GRN_TS_INT: {
      GRN_TS_OP_ARITH_EVALUATE(multiplication, int, int)
    }
    case GRN_TS_FLOAT: {
      GRN_TS_OP_ARITH_EVALUATE(multiplication, float, float)
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}

/* grn_ts_op_division_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_division_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                            const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->data_kind) {
    case GRN_TS_INT: {
      GRN_TS_OP_ARITH_EVALUATE(division, int, int)
    }
    case GRN_TS_FLOAT: {
      GRN_TS_OP_ARITH_EVALUATE(division, float, float)
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}

/* grn_ts_op_modulus_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_modulus_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                           const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->data_kind) {
    case GRN_TS_INT: {
      GRN_TS_OP_ARITH_EVALUATE(modulus, int, int)
    }
    case GRN_TS_FLOAT: {
      GRN_TS_OP_ARITH_EVALUATE(modulus, float, float)
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",
                        node->data_kind);
    }
  }
}
#undef GRN_TS_OP_ARITH_EVALUATE_CASE

#define GRN_TS_OP_MATCH_EVALUATE(type)\
  size_t i;\
  grn_rc rc;\
  grn_ts_bool *out_ptr = (grn_ts_bool *)out;\
  grn_ts_text *buf_ptrs[2];\
  for (i = 0; i < 2; i++) {\
    rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                          &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  buf_ptrs[0] = (grn_ts_text *)node->bufs[0].ptr;\
  buf_ptrs[1] = (grn_ts_text *)node->bufs[1].ptr;\
  for (i = 0; i < n_in; i++) {\
    out_ptr[i] = grn_ts_op_ ## type(buf_ptrs[0][i], buf_ptrs[1][i]);\
  }\
  return GRN_SUCCESS;\
/* grn_ts_op_match_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_match_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                         const grn_ts_record *in, size_t n_in, void *out)
{
  GRN_TS_OP_MATCH_EVALUATE(match)
}

/* grn_ts_op_prefix_match_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_prefix_match_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                                const grn_ts_record *in, size_t n_in,
                                void *out)
{
  GRN_TS_OP_MATCH_EVALUATE(prefix_match)
}

/* grn_ts_op_suffix_match_evaluate() evaluates an operator. */
static grn_rc
grn_ts_op_suffix_match_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                                const grn_ts_record *in, size_t n_in,
                                void *out)
{
  GRN_TS_OP_MATCH_EVALUATE(suffix_match)
}
#undef GRN_TS_OP_MATCH_EVALUATE

/* grn_ts_expr_op_node_evaluate() evaluates an operator. */
static grn_rc
grn_ts_expr_op_node_evaluate(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->op_type) {
    case GRN_TS_OP_LOGICAL_NOT: {
      return grn_ts_op_logical_not_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_BITWISE_NOT: {
      return grn_ts_op_bitwise_not_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_POSITIVE: {
      return grn_ts_op_positive_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_NEGATIVE: {
      return grn_ts_op_negative_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_FLOAT: {
      return grn_ts_op_float_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_TIME: {
      return grn_ts_op_time_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_LOGICAL_AND: {
      return grn_ts_op_logical_and_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_LOGICAL_OR: {
      return grn_ts_op_logical_or_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_LOGICAL_SUB: {
      return grn_ts_op_logical_sub_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_BITWISE_AND: {
      return grn_ts_op_bitwise_and_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_BITWISE_OR: {
      return grn_ts_op_bitwise_or_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_BITWISE_XOR: {
      return grn_ts_op_bitwise_xor_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_EQUAL: {
      return grn_ts_op_equal_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_NOT_EQUAL: {
      return grn_ts_op_not_equal_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_LESS: {
      return grn_ts_op_less_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_LESS_EQUAL: {
      return grn_ts_op_less_equal_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_GREATER: {
      return grn_ts_op_greater_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_GREATER_EQUAL: {
      return grn_ts_op_greater_equal_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_SHIFT_ARITHMETIC_LEFT: {
      return grn_ts_op_shift_arithmetic_left_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_SHIFT_ARITHMETIC_RIGHT: {
      return grn_ts_op_shift_arithmetic_right_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_SHIFT_LOGICAL_LEFT: {
      return grn_ts_op_shift_logical_left_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_SHIFT_LOGICAL_RIGHT: {
      return grn_ts_op_shift_logical_right_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_PLUS: {
      return grn_ts_op_plus_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_MINUS: {
      return grn_ts_op_minus_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_MULTIPLICATION: {
      return grn_ts_op_multiplication_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_DIVISION: {
      return grn_ts_op_division_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_MODULUS: {
      return grn_ts_op_modulus_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_MATCH: {
      return grn_ts_op_match_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_PREFIX_MATCH: {
      return grn_ts_op_prefix_match_evaluate(ctx, node, in, n_in, out);
    }
    case GRN_TS_OP_SUFFIX_MATCH: {
      return grn_ts_op_suffix_match_evaluate(ctx, node, in, n_in, out);
    }
    // TODO: Add operators.
    default: {
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                        "operator not supported: %d", node->op_type);
    }
  }
}

/* grn_ts_op_logical_not_filter() filters records. */
static grn_rc
grn_ts_op_logical_not_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             grn_ts_record *in, size_t n_in,
                             grn_ts_record *out, size_t *n_out)
{
  size_t i, count;
  grn_rc rc;
  grn_ts_bool *buf_ptr;
  rc = grn_ts_buf_reserve(ctx, &node->bufs[0], sizeof(grn_ts_bool) * n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptr = (grn_ts_bool *)node->bufs[0].ptr;
  rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, buf_ptr);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (i = 0, count = 0; i < n_in; i++) {
    if (grn_ts_op_logical_not_bool(buf_ptr[i])) {
      out[count++] = in[i];
    }
  }
  *n_out = count;
  return GRN_SUCCESS;
}

/* grn_ts_op_bitwise_not_filter() filters records. */
static grn_rc
grn_ts_op_bitwise_not_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             grn_ts_record *in, size_t n_in,
                             grn_ts_record *out, size_t *n_out)
{
  size_t i, count;
  grn_rc rc;
  grn_ts_bool *buf_ptr;
  rc = grn_ts_buf_reserve(ctx, &node->bufs[0], sizeof(grn_ts_bool) * n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptr = (grn_ts_bool *)node->bufs[0].ptr;
  rc = grn_ts_expr_node_evaluate(ctx, node->args[0], in, n_in, buf_ptr);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (i = 0, count = 0; i < n_in; i++) {
    if (grn_ts_op_bitwise_not_bool(buf_ptr[i])) {
      out[count++] = in[i];
    }
  }
  *n_out = count;
  return GRN_SUCCESS;
}

/* grn_ts_op_logical_and_filter() filters records. */
static grn_rc
grn_ts_op_logical_and_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             grn_ts_record *in, size_t n_in,
                             grn_ts_record *out, size_t *n_out)
{
  grn_rc rc = grn_ts_expr_node_filter(ctx, node->args[0], in, n_in,
                                      out, n_out);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  return grn_ts_expr_node_filter(ctx, node->args[1], out, *n_out, out, n_out);
}

/* grn_ts_op_logical_or_filter() filters records. */
static grn_rc
grn_ts_op_logical_or_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                            grn_ts_record *in, size_t n_in,
                            grn_ts_record *out, size_t *n_out)
{
  size_t i, j, count;
  grn_rc rc;
  grn_ts_bool *buf_ptrs[2];
  grn_ts_buf *tmp_in_buf = &node->bufs[2];
  grn_ts_record *tmp_in;

  /* Evaluate the 1st argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], in, n_in,
                                        &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptrs[0] = (grn_ts_bool *)node->bufs[0].ptr;

  /* Create a list of false records. */
  rc = grn_ts_buf_reserve(ctx, tmp_in_buf, sizeof(grn_ts_record) * n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp_in = (grn_ts_record *)tmp_in_buf->ptr;
  count = 0;
  for (i = 0; i < n_in; i++) {
    if (!buf_ptrs[0][i]) {
      tmp_in[count++] = in[i];
    }
  }

  /* Evaluate the 2nd argument. */
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1], tmp_in, count,
                                        &node->bufs[1]);
  buf_ptrs[1] = (grn_ts_bool *)node->bufs[1].ptr;

  /* Merge the results. */
  count = 0;
  for (i = 0, j = 0; i < n_in; i++) {
    if (buf_ptrs[0][i] || buf_ptrs[1][j++]) {
      out[count++] = in[i];
    }
  }
  *n_out = count;
  return GRN_SUCCESS;
}

/* grn_ts_op_logical_sub_filter() filters records. */
static grn_rc
grn_ts_op_logical_sub_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             grn_ts_record *in, size_t n_in,
                             grn_ts_record *out, size_t *n_out)
{
  size_t i, n, count;
  grn_ts_bool *buf_ptr;
  grn_rc rc = grn_ts_expr_node_filter(ctx, node->args[0], in, n_in, out, &n);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[1], out, n,
                                        &node->bufs[0]);
  buf_ptr = (grn_ts_bool *)node->bufs[0].ptr;
  for (i = 0, count = 0; i < n; i++) {
    if (grn_ts_op_logical_not_bool(buf_ptr[i])) {
      out[count++] = out[i];
    }
  }
  *n_out = count;
  return GRN_SUCCESS;
}

#define GRN_TS_OP_BITWISE_FILTER(type)\
  size_t i, count = 0;\
  grn_ts_bool *buf_ptrs[2];\
  for (i = 0; i < 2; i++) {\
    grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                                 &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
    buf_ptrs[i] = (grn_ts_bool *)node->bufs[i].ptr;\
  }\
  for (i = 0; i < n_in; i++) {\
    if (grn_ts_op_bitwise_ ## type ## _bool(buf_ptrs[0][i], buf_ptrs[1][i])) {\
      out[count++] = in[i];\
    }\
  }\
  *n_out = count;\
  return GRN_SUCCESS;\
/* grn_ts_op_bitwise_and_filter() filters records. */
static grn_rc
grn_ts_op_bitwise_and_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             grn_ts_record *in, size_t n_in,
                             grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_BITWISE_FILTER(and);
}

/* grn_ts_op_bitwise_or_filter() filters records. */
static grn_rc
grn_ts_op_bitwise_or_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                            grn_ts_record *in, size_t n_in,
                            grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_BITWISE_FILTER(or);
}

/* grn_ts_op_bitwise_xor_filter() filters records. */
static grn_rc
grn_ts_op_bitwise_xor_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                             grn_ts_record *in, size_t n_in,
                             grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_BITWISE_FILTER(xor);
}
#undef GRN_TS_OP_BITWISE_FILTER_CASE

#define GRN_TS_OP_CHK_FILTER_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *buf_ptrs[] = {\
      (grn_ts_ ## kind *)node->bufs[0].ptr,\
      (grn_ts_ ## kind *)node->bufs[1].ptr\
    };\
    for (i = 0; i < n_in; i++) {\
      if (grn_ts_op_ ## type ## _ ## kind(buf_ptrs[0][i], buf_ptrs[1][i])) {\
        out[count++] = in[i];\
      }\
    }\
    *n_out = count;\
    return GRN_SUCCESS;\
  }
#define GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, KIND, kind)\
  GRN_TS_OP_CHK_FILTER_CASE(type, KIND ## _VECTOR, kind ## _vector)
#define GRN_TS_OP_CHK_FILTER(type)\
  size_t i, count = 0;\
  for (i = 0; i < 2; i++) {\
    grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                                 &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  switch (node->args[0]->data_kind) {\
    GRN_TS_OP_CHK_FILTER_CASE(type, BOOL, bool)\
    GRN_TS_OP_CHK_FILTER_CASE(type, INT, int)\
    GRN_TS_OP_CHK_FILTER_CASE(type, FLOAT, float)\
    GRN_TS_OP_CHK_FILTER_CASE(type, TIME, time)\
    GRN_TS_OP_CHK_FILTER_CASE(type, TEXT, text)\
    GRN_TS_OP_CHK_FILTER_CASE(type, GEO, geo)\
    GRN_TS_OP_CHK_FILTER_CASE(type, REF, ref)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, BOOL, bool)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, INT, int)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, FLOAT, float)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, TIME, time)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, TEXT, text)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, GEO, geo)\
    GRN_TS_OP_CHK_FILTER_VECTOR_CASE(type, REF, ref)\
    default: {\
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",\
                        node->args[0]->data_kind);\
    }\
  }
/* grn_ts_op_equal_filter() filters records. */
static grn_rc
grn_ts_op_equal_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                       const grn_ts_record *in, size_t n_in,
                       grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_CHK_FILTER(equal)
}

/* grn_ts_op_not_equal_filter() filters records. */
static grn_rc
grn_ts_op_not_equal_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                           const grn_ts_record *in, size_t n_in,
                           grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_CHK_FILTER(not_equal)
}
#undef GRN_TS_OP_CHK_FILTER
#undef GRN_TS_OP_CHK_FILTER_VECTOR_CASE
#undef GRN_TS_OP_CHK_FILTER_CASE

#define GRN_TS_OP_CMP_FILTER_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *buf_ptrs[] = {\
      (grn_ts_ ## kind *)node->bufs[0].ptr,\
      (grn_ts_ ## kind *)node->bufs[1].ptr\
    };\
    for (i = 0; i < n_in; i++) {\
      if (grn_ts_op_ ## type ## _ ## kind(buf_ptrs[0][i], buf_ptrs[1][i])) {\
        out[count++] = in[i];\
      }\
    }\
    *n_out = count;\
    return GRN_SUCCESS;\
  }
#define GRN_TS_OP_CMP_FILTER_VECTOR_CASE(type, KIND, kind)\
  GRN_TS_OP_CMP_FILTER_CASE(type, KIND ## _VECTOR, kind ## _vector)
#define GRN_TS_OP_CMP_FILTER(type)\
  size_t i, count = 0;\
  for (i = 0; i < 2; i++) {\
    grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                                 &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  switch (node->args[0]->data_kind) {\
    GRN_TS_OP_CMP_FILTER_CASE(type, INT, int)\
    GRN_TS_OP_CMP_FILTER_CASE(type, FLOAT, float)\
    GRN_TS_OP_CMP_FILTER_CASE(type, TIME, time)\
    GRN_TS_OP_CMP_FILTER_CASE(type, TEXT, text)\
    GRN_TS_OP_CMP_FILTER_VECTOR_CASE(type, INT, int)\
    GRN_TS_OP_CMP_FILTER_VECTOR_CASE(type, FLOAT, float)\
    GRN_TS_OP_CMP_FILTER_VECTOR_CASE(type, TIME, time)\
    GRN_TS_OP_CMP_FILTER_VECTOR_CASE(type, TEXT, text)\
    default: {\
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid data kind: %d",\
                        node->args[0]->data_kind);\
    }\
  }
/* grn_ts_op_less_filter() filters records. */
static grn_rc
grn_ts_op_less_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                      const grn_ts_record *in, size_t n_in,
                      grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_CMP_FILTER(less)
}

/* grn_ts_op_less_equal_filter() filters records. */
static grn_rc
grn_ts_op_less_equal_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                            const grn_ts_record *in, size_t n_in,
                            grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_CMP_FILTER(less_equal)
}

/* grn_ts_op_greater_filter() filters records. */
static grn_rc
grn_ts_op_greater_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                         const grn_ts_record *in, size_t n_in,
                         grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_CMP_FILTER(greater)
}

/* grn_ts_op_greater_equal_filter() filters records. */
static grn_rc
grn_ts_op_greater_equal_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                               const grn_ts_record *in, size_t n_in,
                               grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_CMP_FILTER(greater_equal)
}
#undef GRN_TS_OP_CMP_FILTER
#undef GRN_TS_OP_CMP_FILTER_VECTOR_CASE
#undef GRN_TS_OP_CMP_FILTER_CASE

#define GRN_TS_OP_MATCH_FILTER_CASE(type, KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *buf_ptrs[] = {\
      (grn_ts_ ## kind *)node->bufs[0].ptr,\
      (grn_ts_ ## kind *)node->bufs[1].ptr\
    };\
    for (i = 0; i < n_in; i++) {\
      if (grn_ts_op_ ## type ## _ ## kind(buf_ptrs[0][i], buf_ptrs[1][i])) {\
        out[count++] = in[i];\
      }\
    }\
    *n_out = count;\
    return GRN_SUCCESS;\
  }

#define GRN_TS_OP_MATCH_FILTER(type)\
  size_t i, count = 0;\
  grn_ts_text *buf_ptrs[2];\
  for (i = 0; i < 2; i++) {\
    grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], in, n_in,\
                                                 &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  buf_ptrs[0] = (grn_ts_text *)node->bufs[0].ptr;\
  buf_ptrs[1] = (grn_ts_text *)node->bufs[1].ptr;\
  for (i = 0; i < n_in; i++) {\
    if (grn_ts_op_ ## type(buf_ptrs[0][i], buf_ptrs[1][i])) {\
      out[count++] = in[i];\
    }\
  }\
  *n_out = count;\
  return GRN_SUCCESS;\
/* grn_ts_op_match_filter() filters records. */
static grn_rc
grn_ts_op_match_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                       const grn_ts_record *in, size_t n_in,
                       grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_MATCH_FILTER(match)
}

/* grn_ts_op_prefix_match_filter() filters records. */
static grn_rc
grn_ts_op_prefix_match_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                              const grn_ts_record *in, size_t n_in,
                              grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_MATCH_FILTER(prefix_match)
}

/* grn_ts_op_suffix_match_filter() filters records. */
static grn_rc
grn_ts_op_suffix_match_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                              const grn_ts_record *in, size_t n_in,
                              grn_ts_record *out, size_t *n_out)
{
  GRN_TS_OP_MATCH_FILTER(suffix_match)
}
#undef GRN_TS_OP_MATCH_FILTER

/* grn_ts_expr_op_node_filter() filters records. */
static grn_rc
grn_ts_expr_op_node_filter(grn_ctx *ctx, grn_ts_expr_op_node *node,
                           grn_ts_record *in, size_t n_in,
                           grn_ts_record *out, size_t *n_out)
{
  switch (node->op_type) {
    case GRN_TS_OP_LOGICAL_NOT: {
      return grn_ts_op_logical_not_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_BITWISE_NOT: {
      return grn_ts_op_bitwise_not_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_LOGICAL_AND: {
      return grn_ts_op_logical_and_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_LOGICAL_OR: {
      return grn_ts_op_logical_or_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_LOGICAL_SUB: {
      return grn_ts_op_logical_sub_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_BITWISE_AND: {
      return grn_ts_op_bitwise_and_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_BITWISE_OR: {
      return grn_ts_op_bitwise_or_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_BITWISE_XOR: {
      return grn_ts_op_bitwise_xor_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_EQUAL: {
      return grn_ts_op_equal_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_NOT_EQUAL: {
      return grn_ts_op_not_equal_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_LESS: {
      return grn_ts_op_less_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_LESS_EQUAL: {
      return grn_ts_op_less_equal_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_GREATER: {
      return grn_ts_op_greater_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_GREATER_EQUAL: {
      return grn_ts_op_greater_equal_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_MATCH: {
      return grn_ts_op_match_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_PREFIX_MATCH: {
      return grn_ts_op_prefix_match_filter(ctx, node, in, n_in, out, n_out);
    }
    case GRN_TS_OP_SUFFIX_MATCH: {
      return grn_ts_op_suffix_match_filter(ctx, node, in, n_in, out, n_out);
    }
    // TODO: Add operators.
    default: {
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                        "operator not supported: %d", node->op_type);
    }
  }
}

#define GRN_TS_OP_SIGN_ADJUST(type)\
  size_t i;\
  grn_ts_float *buf_ptr;\
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], io, n_io,\
                                               &node->bufs[0]);\
  if (rc != GRN_SUCCESS) {\
    return rc;\
  }\
  buf_ptr = (grn_ts_float *)node->bufs[0].ptr;\
  for (i = 0; i < n_io; i++) {\
    grn_ts_float result = grn_ts_op_ ## type ## _float(buf_ptr[i]);\
    io[i].score = (grn_ts_score)result;\
    if (!isfinite(io[i].score)) {\
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid score: %g", result);\
    }\
  }\
  return GRN_SUCCESS;
/* grn_ts_op_positive_adjust() updates scores. */
static grn_rc
grn_ts_op_positive_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                          grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_SIGN_ADJUST(positive)
}

/* grn_ts_op_negative_adjust() updates scores. */
static grn_rc
grn_ts_op_negative_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                          grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_SIGN_ADJUST(negative)
}
#undef GRN_TS_OP_SIGN_ADJUST

/* grn_ts_op_float_adjust() updates scores. */
static grn_rc
grn_ts_op_float_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                       grn_ts_record *io, size_t n_io)
{
  size_t i;
  grn_ts_int *buf_ptr;
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[0], io, n_io,
                                               &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  buf_ptr = (grn_ts_int *)node->bufs[0].ptr;
  for (i = 0; i < n_io; i++) {
    grn_ts_float result;
    rc = grn_ts_op_float(ctx, buf_ptr[i], &result);
    io[i].score = (grn_ts_score)result;
    if (!isfinite(io[i].score)) {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid score: %g", result);
    }
  }
  return GRN_SUCCESS;
}

#define GRN_TS_OP_ARITH_ADJUST(type)\
  grn_rc rc;\
  size_t i;\
  grn_ts_float *buf_ptrs[2];\
  for (i = 0; i < 2; i++) {\
    rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->args[i], io, n_io,\
                                          &node->bufs[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  buf_ptrs[0] = (grn_ts_float *)node->bufs[0].ptr;\
  buf_ptrs[1] = (grn_ts_float *)node->bufs[1].ptr;\
  for (i = 0; i < n_io; i++) {\
    grn_ts_float result;\
    rc = grn_ts_op_ ## type ## _float_float(ctx, buf_ptrs[0][i],\
                                            buf_ptrs[1][i], &result);\
    io[i].score = (grn_ts_score)result;\
    if (!isfinite(io[i].score)) {\
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid score: %g", result);\
    }\
  }\
  return GRN_SUCCESS;
/* grn_ts_op_plus_adjust() updates scores. */
static grn_rc
grn_ts_op_plus_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                      grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_ARITH_ADJUST(plus)
}

/* grn_ts_op_minus_adjust() updates scores. */
static grn_rc
grn_ts_op_minus_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                       grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_ARITH_ADJUST(minus)
}

/* grn_ts_op_multiplication_adjust() updates scores. */
static grn_rc
grn_ts_op_multiplication_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                                grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_ARITH_ADJUST(multiplication)
}

/* grn_ts_op_division_adjust() updates scores. */
static grn_rc
grn_ts_op_division_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                          grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_ARITH_ADJUST(division)
}

/* grn_ts_op_modulus_adjust() updates scores. */
static grn_rc
grn_ts_op_modulus_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                         grn_ts_record *io, size_t n_io)
{
  GRN_TS_OP_ARITH_ADJUST(modulus)
}
#undef GRN_TS_OP_ARITH_ADJUST

/* grn_ts_expr_op_node_adjust() updates scores. */
static grn_rc
grn_ts_expr_op_node_adjust(grn_ctx *ctx, grn_ts_expr_op_node *node,
                           grn_ts_record *io, size_t n_io)
{
  switch (node->op_type) {
    case GRN_TS_OP_POSITIVE: {
      return grn_ts_op_positive_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_NEGATIVE: {
      return grn_ts_op_negative_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_FLOAT: {
      return grn_ts_op_float_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_PLUS: {
      return grn_ts_op_plus_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_MINUS: {
      return grn_ts_op_minus_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_MULTIPLICATION: {
      return grn_ts_op_multiplication_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_DIVISION: {
      return grn_ts_op_division_adjust(ctx, node, io, n_io);
    }
    case GRN_TS_OP_MODULUS: {
      return grn_ts_op_modulus_adjust(ctx, node, io, n_io);
    }
    // TODO: Add operators.
    default: {
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                        "operator not supported: %d", node->op_type);
    }
  }
}

/*-------------------------------------------------------------
 * grn_ts_expr_bridge_node.
 */

enum { GRN_TS_EXPR_BRIDGE_NODE_N_BUFS = 2 };

typedef struct {
  GRN_TS_EXPR_NODE_COMMON_MEMBERS
  grn_ts_expr_node *src;
  grn_ts_expr_node *dest;
  grn_ts_buf bufs[GRN_TS_EXPR_BRIDGE_NODE_N_BUFS];
} grn_ts_expr_bridge_node;

/* grn_ts_expr_bridge_node_init() initializes a node. */
static void
grn_ts_expr_bridge_node_init(grn_ctx *ctx, grn_ts_expr_bridge_node *node)
{
  size_t i;
  memset(node, 0, sizeof(*node));
  node->type = GRN_TS_EXPR_BRIDGE_NODE;
  node->src = NULL;
  node->dest = NULL;
  for (i = 0; i < GRN_TS_EXPR_BRIDGE_NODE_N_BUFS; i++) {
    grn_ts_buf_init(ctx, &node->bufs[i]);
  }
}

/* grn_ts_expr_bridge_node_fin() finalizes a node. */
static void
grn_ts_expr_bridge_node_fin(grn_ctx *ctx, grn_ts_expr_bridge_node *node)
{
  size_t i;
  for (i = 0; i < GRN_TS_EXPR_BRIDGE_NODE_N_BUFS; i++) {
    grn_ts_buf_fin(ctx, &node->bufs[i]);
  }
  if (node->dest) {
    grn_ts_expr_node_close(ctx, node->dest);
  }
  if (node->src) {
    grn_ts_expr_node_close(ctx, node->src);
  }
}

grn_rc
grn_ts_expr_bridge_node_open(grn_ctx *ctx, grn_ts_expr_node *src,
                             grn_ts_expr_node *dest, grn_ts_expr_node **node)
{
  grn_ts_expr_bridge_node *new_node = GRN_MALLOCN(grn_ts_expr_bridge_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_expr_bridge_node));
  }
  grn_ts_expr_bridge_node_init(ctx, new_node);
  new_node->data_kind = dest->data_kind;
  new_node->data_type = dest->data_type;
  new_node->src = src;
  new_node->dest = dest;
  *node = (grn_ts_expr_node *)new_node;
  return GRN_SUCCESS;
}

/* grn_ts_expr_bridge_node_close() destroys a node. */
static void
grn_ts_expr_bridge_node_close(grn_ctx *ctx, grn_ts_expr_bridge_node *node)
{
  grn_ts_expr_bridge_node_fin(ctx, node);
  GRN_FREE(node);
}

/* grn_ts_expr_bridge_node_evaluate() evaluates a bridge. */
static grn_rc
grn_ts_expr_bridge_node_evaluate(grn_ctx *ctx, grn_ts_expr_bridge_node *node,
                                 const grn_ts_record *in, size_t n_in,
                                 void *out)
{
  grn_ts_record *tmp;
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->src, in, n_in,
                                               &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp = (grn_ts_record *)node->bufs[0].ptr;
  return grn_ts_expr_node_evaluate(ctx, node->dest, tmp, n_in, out);
}

/* grn_ts_expr_bridge_node_filter() filters records. */
static grn_rc
grn_ts_expr_bridge_node_filter(grn_ctx *ctx, grn_ts_expr_bridge_node *node,
                               grn_ts_record *in, size_t n_in,
                               grn_ts_record *out, size_t *n_out)
{
  size_t i, count;
  grn_ts_bool *values;
  grn_ts_record *tmp;
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->src, in, n_in,
                                               &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp = (grn_ts_record *)node->bufs[0].ptr;
  rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->dest, in, n_in,
                                        &node->bufs[1]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  values = (grn_ts_bool *)&node->bufs[1].ptr;
  for (i = 0, count = 0; i < n_in; i++) {
    if (values[i]) {
      out[count++] = in[i];
    }
  }
  *n_out = count;
  return GRN_SUCCESS;
}

/* grn_ts_expr_bridge_node_adjust() updates scores. */
static grn_rc
grn_ts_expr_bridge_node_adjust(grn_ctx *ctx, grn_ts_expr_bridge_node *node,
                               grn_ts_record *io, size_t n_io)
{
  size_t i;
  grn_ts_record *tmp;
  grn_rc rc = grn_ts_expr_node_evaluate_to_buf(ctx, node->src, io, n_io,
                                               &node->bufs[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  tmp = (grn_ts_record *)node->bufs[0].ptr;
  rc = grn_ts_expr_node_adjust(ctx, node->dest, tmp, n_io);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (i = 0; i < n_io; i++) {
    io[i].score = tmp[i].score;
  }
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_expr_node.
 */

#define GRN_TS_EXPR_NODE_CLOSE_CASE(TYPE, type)\
  case GRN_TS_EXPR_ ## TYPE ## _NODE: {\
    grn_ts_expr_ ## type ## _node *type ## _node;\
    type ## _node = (grn_ts_expr_ ## type ## _node *)node;\
    grn_ts_expr_ ## type ## _node_close(ctx, type ## _node);\
    return;\
  }
void
grn_ts_expr_node_close(grn_ctx *ctx, grn_ts_expr_node *node)
{
  switch (node->type) {
    GRN_TS_EXPR_NODE_CLOSE_CASE(ID, id)
    GRN_TS_EXPR_NODE_CLOSE_CASE(SCORE, score)
    GRN_TS_EXPR_NODE_CLOSE_CASE(KEY, key)
    GRN_TS_EXPR_NODE_CLOSE_CASE(VALUE, value)
    GRN_TS_EXPR_NODE_CLOSE_CASE(CONST, const)
    GRN_TS_EXPR_NODE_CLOSE_CASE(COLUMN, column)
    GRN_TS_EXPR_NODE_CLOSE_CASE(OP, op)
    GRN_TS_EXPR_NODE_CLOSE_CASE(BRIDGE, bridge)
  }
}
#undef GRN_TS_EXPR_NODE_CLOSE_CASE

/* grn_ts_expr_node_deref_once() resolves a reference. */
static grn_rc
grn_ts_expr_node_deref_once(grn_ctx *ctx, grn_ts_expr_node *in,
                            grn_ts_expr_node **out)
{
  grn_rc rc;
  grn_id table_id = in->data_type;
  grn_ts_expr_node *key_node, *bridge_node;
  grn_obj *table = grn_ctx_at(ctx, table_id);
  if (!table) {
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "grn_ctx_at failed: %d", table_id);
  }
  if (!grn_ts_obj_is_table(ctx, table)) {
    grn_obj_unlink(ctx, table);
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "not table: %d", table_id);
  }
  rc = grn_ts_expr_key_node_open(ctx, table, &key_node);
  grn_obj_unlink(ctx, table);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_bridge_node_open(ctx, in, key_node, &bridge_node);
  if (rc != GRN_SUCCESS) {
    grn_ts_expr_node_close(ctx, key_node);
    return rc;
  }
  *out = bridge_node;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_node_deref(grn_ctx *ctx, grn_ts_expr_node **node_ptr)
{
  grn_ts_expr_node *node = *node_ptr, **in_ptr = NULL;
  while ((node->data_kind & ~GRN_TS_VECTOR_FLAG) == GRN_TS_REF) {
    grn_ts_expr_node *new_node= 0;
    grn_rc rc = grn_ts_expr_node_deref_once(ctx, node, &new_node);
    if (rc != GRN_SUCCESS) {
      if (in_ptr) {
        *in_ptr = NULL;
        grn_ts_expr_node_close(ctx, node);
      }
      return rc;
    }
    if (node == *node_ptr) {
      grn_ts_expr_bridge_node *bridge_node;
      bridge_node = (grn_ts_expr_bridge_node *)new_node;
      if (bridge_node->src != node) {
        GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "broken bridge node");
      }
      in_ptr = &bridge_node->src;
    }
    node = new_node;
  }
  *node_ptr = node;
  return GRN_SUCCESS;
}

#define GRN_TS_EXPR_NODE_EVALUATE_CASE(TYPE, type)\
  case GRN_TS_EXPR_ ## TYPE ## _NODE: {\
    grn_ts_expr_ ## type ## _node *type ## _node;\
    type ## _node = (grn_ts_expr_ ## type ## _node *)node;\
    return grn_ts_expr_ ## type ## _node_evaluate(ctx, type ## _node,\
                                                  in, n_in, out);\
  }
grn_rc
grn_ts_expr_node_evaluate(grn_ctx *ctx, grn_ts_expr_node *node,
                          const grn_ts_record *in, size_t n_in, void *out)
{
  switch (node->type) {
    GRN_TS_EXPR_NODE_EVALUATE_CASE(ID, id)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(SCORE, score)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(KEY, key)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(VALUE, value)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(CONST, const)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(COLUMN, column)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(OP, op)
    GRN_TS_EXPR_NODE_EVALUATE_CASE(BRIDGE, bridge)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT,
                        "invalid node type: %d", node->type);
    }
  }
}
#undef GRN_TS_EXPR_NODE_EVALUATE_CASE

#define GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_rc rc = grn_ts_buf_reserve(ctx, out, sizeof(grn_ts_ ## kind) * n_in);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
    return grn_ts_expr_node_evaluate(ctx, node, in, n_in, out->ptr);\
  }
#define GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(KIND, kind)\
  GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(KIND ## _VECTOR, kind ## _vector)
grn_rc
grn_ts_expr_node_evaluate_to_buf(grn_ctx *ctx, grn_ts_expr_node *node,
                                 const grn_ts_record *in, size_t n_in,
                                 grn_ts_buf *out)
{
  switch (node->data_kind) {
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(BOOL, bool)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(INT, int)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(FLOAT, float)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(TIME, time)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(TEXT, text)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(GEO, geo)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE(REF, ref)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(BOOL, bool)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(INT, int)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(FLOAT, float)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(TIME, time)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(TEXT, text)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(GEO, geo)
    GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE(REF, ref)
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT,
                        "invalid data kind: %d", node->data_kind);
    }
  }
}
#undef GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_VECTOR_CASE
#undef GRN_TS_EXPR_NODE_EVALUATE_TO_BUF_CASE

#define GRN_TS_EXPR_NODE_FILTER_CASE(TYPE, type)\
  case GRN_TS_EXPR_ ## TYPE ## _NODE: {\
    grn_ts_expr_ ## type ## _node *type ## _node;\
    type ## _node = (grn_ts_expr_ ## type ## _node *)node;\
    return grn_ts_expr_ ## type ## _node_filter(ctx, type ## _node,\
                                                in, n_in, out, n_out);\
  }
grn_rc
grn_ts_expr_node_filter(grn_ctx *ctx, grn_ts_expr_node *node,
                        grn_ts_record *in, size_t n_in,
                        grn_ts_record *out, size_t *n_out)
{
  if (node->data_kind != GRN_TS_BOOL) {
    GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                      "invalid data kind: %d", node->data_kind);
  }
  switch (node->type) {
    GRN_TS_EXPR_NODE_FILTER_CASE(KEY, key)
    GRN_TS_EXPR_NODE_FILTER_CASE(VALUE, value)
    GRN_TS_EXPR_NODE_FILTER_CASE(CONST, const)
    GRN_TS_EXPR_NODE_FILTER_CASE(COLUMN, column)
    GRN_TS_EXPR_NODE_FILTER_CASE(OP, op)
    GRN_TS_EXPR_NODE_FILTER_CASE(BRIDGE, bridge)
    default: {
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                        "invalid node type: %d", node->type);
    }
  }
}
#undef GRN_TS_EXPR_NODE_FILTER_CASE

#define GRN_TS_EXPR_NODE_ADJUST_CASE(TYPE, type)\
  case GRN_TS_EXPR_ ## TYPE ## _NODE: {\
    grn_ts_expr_ ## type ## _node *type ## _node;\
    type ## _node = (grn_ts_expr_ ## type ## _node *)node;\
    return grn_ts_expr_ ## type ## _node_adjust(ctx, type ## _node, io, n_io);\
  }
grn_rc
grn_ts_expr_node_adjust(grn_ctx *ctx, grn_ts_expr_node *node,
                        grn_ts_record *io, size_t n_io)
{
  if (node->data_kind != GRN_TS_FLOAT) {
    GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                      "invalid data kind: %d", node->data_kind);
  }
  switch (node->type) {
    GRN_TS_EXPR_NODE_ADJUST_CASE(SCORE, score)
    GRN_TS_EXPR_NODE_ADJUST_CASE(KEY, key)
    GRN_TS_EXPR_NODE_ADJUST_CASE(VALUE, value)
    GRN_TS_EXPR_NODE_ADJUST_CASE(CONST, const)
    GRN_TS_EXPR_NODE_ADJUST_CASE(COLUMN, column)
    GRN_TS_EXPR_NODE_ADJUST_CASE(OP, op)
    GRN_TS_EXPR_NODE_ADJUST_CASE(BRIDGE, bridge)
    default: {
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED,
                        "invalid node type: %d", node->type);
    }
  }
}
#undef GRN_TS_EXPR_NODE_ADJUST_CASE
