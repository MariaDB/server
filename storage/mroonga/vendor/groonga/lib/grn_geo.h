/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#include "grn.h"
#include "grn_ii.h"
#include "grn_db.h"

#if defined(WIN32) || defined(__sun)
# define _USE_MATH_DEFINES
# ifndef MAX
#  define MAX(a, b) ((a) > (b) ? (a) : (b))
# endif

# ifndef MIN
#  define MIN(a, b) ((a) < (b) ? (a) : (b))
# endif
#endif /* WIN32 or __sun */
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRN_GEO_RESOLUTION   3600000
#define GRN_GEO_RADIUS       6357303
#define GRN_GEO_BES_C1       6334834
#define GRN_GEO_BES_C2       6377397
#define GRN_GEO_BES_C3       0.006674
#define GRN_GEO_GRS_C1       6335439
#define GRN_GEO_GRS_C2       6378137
#define GRN_GEO_GRS_C3       0.006694
#define GRN_GEO_INT2RAD(x)   ((M_PI / (GRN_GEO_RESOLUTION * 180)) * (x))
#define GRN_GEO_RAD2INT(x)   ((int)(((GRN_GEO_RESOLUTION * 180) / M_PI) * (x)))

#define GRN_GEO_MAX_LATITUDE  324000000       /*  90 * 60 * 60 * 1000 */
#define GRN_GEO_MAX_LONGITUDE (648000000 - 1) /* 180 * 60 * 60 * 1000 - 1 */
#define GRN_GEO_MIN_LATITUDE  -GRN_GEO_MAX_LATITUDE
#define GRN_GEO_MIN_LONGITUDE -GRN_GEO_MAX_LONGITUDE

#define GRN_GEO_POINT_VALUE_RAW(obj) (grn_geo_point *)GRN_BULK_HEAD(obj)
#define GRN_GEO_POINT_VALUE_RADIUS(obj,_latitude,_longitude) do {\
  grn_geo_point *_val = (grn_geo_point *)GRN_BULK_HEAD(obj);\
  _latitude = GRN_GEO_INT2RAD(_val->latitude);\
  _longitude = GRN_GEO_INT2RAD(_val->longitude);\
} while (0)

#define GRN_GEO_KEY_MAX_BITS 64

typedef enum {
  GRN_GEO_APPROXIMATE_RECTANGLE,
  GRN_GEO_APPROXIMATE_SPHERE,
  GRN_GEO_APPROXIMATE_ELLIPSOID
} grn_geo_approximate_type;

typedef enum {
  GRN_GEO_CURSOR_ENTRY_STATUS_NONE            = 0,
  GRN_GEO_CURSOR_ENTRY_STATUS_TOP_INCLUDED    = 1 << 0,
  GRN_GEO_CURSOR_ENTRY_STATUS_BOTTOM_INCLUDED = 1 << 1,
  GRN_GEO_CURSOR_ENTRY_STATUS_LEFT_INCLUDED   = 1 << 2,
  GRN_GEO_CURSOR_ENTRY_STATUS_RIGHT_INCLUDED  = 1 << 3,
  GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER  = 1 << 4,
  GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER = 1 << 5
} grn_geo_cursor_entry_status_flag;

typedef enum {
  GRN_GEO_AREA_NORTH_EAST,
  GRN_GEO_AREA_NORTH_WEST,
  GRN_GEO_AREA_SOUTH_WEST,
  GRN_GEO_AREA_SOUTH_EAST,
  GRN_GEO_AREA_LAST
} grn_geo_area_type;

#define GRN_GEO_N_AREAS GRN_GEO_AREA_LAST

typedef struct {
  uint8_t key[sizeof(grn_geo_point)];
  int target_bit;
  int status_flags;
} grn_geo_cursor_entry;

typedef struct {
  grn_geo_point top_left;
  grn_geo_point bottom_right;
  uint8_t top_left_key[sizeof(grn_geo_point)];
  uint8_t bottom_right_key[sizeof(grn_geo_point)];
  int current_entry;
  grn_geo_cursor_entry entries[GRN_GEO_KEY_MAX_BITS];
} grn_geo_cursor_area;

typedef struct {
  grn_db_obj obj;
  grn_obj *pat;
  grn_obj *index;
  grn_geo_point top_left;
  grn_geo_point bottom_right;
  grn_geo_point current;
  grn_table_cursor *pat_cursor;
  grn_ii_cursor *ii_cursor;
  int offset;
  int rest;
  int minimum_reduce_bit;
  grn_geo_area_type current_area;
  grn_geo_cursor_area areas[GRN_GEO_N_AREAS];
} grn_geo_cursor_in_rectangle;

grn_rc grn_geo_cursor_close(grn_ctx *ctx, grn_obj *geo_cursor);


grn_rc grn_geo_resolve_approximate_type(grn_ctx *ctx, grn_obj *type_name,
                                        grn_geo_approximate_type *type);

/**
 * grn_geo_select_in_circle:
 * @index: the index column for TokyoGeoPoint or WGS84GeoPpoint type.
 * @center_point: the center point of the target circle. (ShortText, Text,
 * LongText, TokyoGeoPoint or WGS84GeoPoint)
 * @distance: the radius of the target circle (Int32,
 * UInt32, Int64, UInt64 or Float) or the point
 * on the circumference of the target circle. (ShortText, Text, LongText,
 * TokyoGeoPoint or WGS84GeoPoint)
 * @approximate_type: the approximate type to compute
 * distance.
 * @res: the table to store found record IDs. It must be
 * GRN_TABLE_HASH_KEY type table.
 * @op: the operator for matched records.
 *
 * It selects records that are in the circle specified by
 * @center_point and @distance from @center_point. Records
 * are searched by @index. Found records are added to @res
 * table with @op operation.
 **/
grn_rc grn_geo_select_in_circle(grn_ctx *ctx,
                                grn_obj *index,
                                grn_obj *center_point,
                                grn_obj *distance,
                                grn_geo_approximate_type approximate_type,
                                grn_obj *res,
                                grn_operator op);

grn_rc grn_selector_geo_in_circle(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                                  int nargs, grn_obj **args,
                                  grn_obj *res, grn_operator op);
grn_rc grn_selector_geo_in_rectangle(grn_ctx *ctx,
                                     grn_obj *table, grn_obj *index,
                                     int nargs, grn_obj **args,
                                     grn_obj *res, grn_operator op);

GRN_API grn_bool grn_geo_in_circle(grn_ctx *ctx, grn_obj *point, grn_obj *center,
                           grn_obj *radius_or_point,
                           grn_geo_approximate_type approximate_type);
GRN_API grn_bool grn_geo_in_rectangle(grn_ctx *ctx, grn_obj *point,
                                      grn_obj *top_left, grn_obj *bottom_right);
grn_bool grn_geo_in_rectangle_raw(grn_ctx *ctx, grn_geo_point *point,
                                  grn_geo_point *top_left,
                                  grn_geo_point *bottom_right);
double grn_geo_distance(grn_ctx *ctx, grn_obj *point1, grn_obj *point2,
                        grn_geo_approximate_type type);
GRN_API double grn_geo_distance_rectangle(grn_ctx *ctx, grn_obj *point1,
                                          grn_obj *point2);
GRN_API double grn_geo_distance_sphere(grn_ctx *ctx, grn_obj *point1,
                                       grn_obj *point2);
GRN_API double grn_geo_distance_ellipsoid(grn_ctx *ctx, grn_obj *point1,
                                          grn_obj *point2);
double grn_geo_distance_rectangle_raw(grn_ctx *ctx,
                                      grn_geo_point *point1,
                                      grn_geo_point *point2);
double grn_geo_distance_sphere_raw(grn_ctx *ctx,
                                   grn_geo_point *point1,
                                   grn_geo_point *point2);
double grn_geo_distance_ellipsoid_raw(grn_ctx *ctx,
                                      grn_geo_point *point1,
                                      grn_geo_point *point2,
                                      int c1, int c2, double c3);
double grn_geo_distance_ellipsoid_raw_tokyo(grn_ctx *ctx,
                                            grn_geo_point *point1,
                                            grn_geo_point *point2);
double grn_geo_distance_ellipsoid_raw_wgs84(grn_ctx *ctx,
                                            grn_geo_point *point1,
                                            grn_geo_point *point2);

#ifdef __cplusplus
}
#endif
