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

#ifdef __cplusplus
extern "C" {
#endif

/*-------------------------------------------------------------
 * Built-in data types.
 */

/* grn_builtin_type or table ID. */
typedef grn_id grn_ts_data_type;

/* ID (_id). */
typedef grn_id grn_ts_id;

/* Score (_score). */
typedef float grn_ts_score;

/* Record (_id, _score). */
typedef struct {
  grn_ts_id id;
  grn_ts_score score;
} grn_ts_record;

/*-------------------------------------------------------------
 * Built-in scalar data kinds.
 */

/* Bool. */
typedef grn_bool grn_ts_bool;

/* Int. */
typedef int64_t grn_ts_int;

/* Float. */
typedef double grn_ts_float;

/* Time. */
typedef int64_t grn_ts_time;

/* Text. */
typedef struct {
  const char *ptr;
  size_t size;
} grn_ts_text;

/* Geo. */
typedef grn_geo_point grn_ts_geo;
typedef grn_geo_point grn_ts_tokyo_geo;
typedef grn_geo_point grn_ts_wgs84_geo;

/* Ref. */
typedef grn_ts_record grn_ts_ref;

/*-------------------------------------------------------------
 * Built-in vector data kinds.
 */

/* BoolVector. */
typedef struct {
  const grn_ts_bool *ptr;
  size_t size;
} grn_ts_bool_vector;

/* IntVector. */
typedef struct {
  const grn_ts_int *ptr;
  size_t size;
} grn_ts_int_vector;

/* FloatVector. */
typedef struct {
  const grn_ts_float *ptr;
  size_t size;
} grn_ts_float_vector;

/* TimeVector. */
typedef struct {
  const grn_ts_time *ptr;
  size_t size;
} grn_ts_time_vector;

/* TextVector. */
typedef struct {
  const grn_ts_text *ptr;
  size_t size;
} grn_ts_text_vector;

/* GeoVector. */
typedef struct {
  const grn_ts_geo *ptr;
  size_t size;
} grn_ts_geo_vector;
typedef grn_ts_geo_vector grn_ts_tokyo_geo_vector;
typedef grn_ts_geo_vector grn_ts_wgs84_geo_vector;

/* RefVector. */
typedef struct {
  const grn_ts_ref *ptr;
  size_t size;
} grn_ts_ref_vector;

/*-------------------------------------------------------------
 * Built-in data kinds.
 */

enum { GRN_TS_VECTOR_FLAG = 1 << 7 };

typedef enum {
  GRN_TS_VOID         = 0, /* GRN_DB_VOID */
  GRN_TS_BOOL         = 1, /* GRN_DB_BOOL */
  GRN_TS_INT          = 2, /* GRN_DB_[U]INT(8/16/32/64) */
  GRN_TS_FLOAT        = 3, /* GRN_DB_FLOAT */
  GRN_TS_TIME         = 4, /* GRN_DB_TIME */
  GRN_TS_TEXT         = 5, /* GRN_DB_[SHORT_/LONG_]TEST */
  GRN_TS_GEO          = 6, /* GRN_DB_(TOKYO/WGS84)_GEO_POINT */
  GRN_TS_REF          = 7, /* Table reference. */
  GRN_TS_BOOL_VECTOR  = GRN_TS_VECTOR_FLAG | GRN_TS_BOOL,
  GRN_TS_INT_VECTOR   = GRN_TS_VECTOR_FLAG | GRN_TS_INT,
  GRN_TS_FLOAT_VECTOR = GRN_TS_VECTOR_FLAG | GRN_TS_FLOAT,
  GRN_TS_TIME_VECTOR  = GRN_TS_VECTOR_FLAG | GRN_TS_TIME,
  GRN_TS_TEXT_VECTOR  = GRN_TS_VECTOR_FLAG | GRN_TS_TEXT,
  GRN_TS_GEO_VECTOR   = GRN_TS_VECTOR_FLAG | GRN_TS_GEO,
  GRN_TS_REF_VECTOR   = GRN_TS_VECTOR_FLAG | GRN_TS_REF
} grn_ts_data_kind;

typedef union {
  grn_ts_bool as_bool;
  grn_ts_int as_int;
  grn_ts_float as_float;
  grn_ts_time as_time;
  grn_ts_text as_text;
  grn_ts_geo as_geo;
  grn_ts_ref as_ref;
  grn_ts_bool_vector as_bool_vector;
  grn_ts_int_vector as_int_vector;
  grn_ts_float_vector as_float_vector;
  grn_ts_time_vector as_time_vector;
  grn_ts_text_vector as_text_vector;
  grn_ts_geo_vector as_geo_vector;
  grn_ts_ref_vector as_ref_vector;
} grn_ts_any;

#ifdef __cplusplus
}
#endif

