/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2009-2016  Kouhei Sutou <kou@clear-code.com>

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

/*
  groonga: 3ad91b868909444f66a36dbcbdbe2292ed14bd72
  CFLAGS: -O0 -g
  CPU: AMD Athlon(tm) 64 Processor 3000+

  % test/benchmark/bench-geo-distance
                       (time)
  rectangular (WGS84): (0.0813662)
  rectangular (TOKYO): (0.0621928)
    spherical (WGS84): (0.0760155)
    spherical (TOKYO): (0.0660843)
       hubeny (WGS84): (0.110684)
       hubeny (TOKYO): (0.0702277)
  % test/benchmark/bench-geo-distance
                       (time)
  rectangular (WGS84): (0.0742154)
  rectangular (TOKYO): (0.0816863)
    spherical (WGS84): (0.074316)
    spherical (TOKYO): (0.0696254)
       hubeny (WGS84): (0.0650147)
       hubeny (TOKYO): (0.0644057)
  % test/benchmark/bench-geo-distance
                       (time)
  rectangular (WGS84): (0.0781161)
  rectangular (TOKYO): (0.0706679)
    spherical (WGS84): (0.075739)
    spherical (TOKYO): (0.0809402)
       hubeny (WGS84): (0.0727023)
       hubeny (TOKYO): (0.0718146)
 */

#include <string.h>
#include <stdlib.h>

#include <grn_db.h>
#include <groonga.h>

#include "lib/benchmark.h"

#define GET(context, name) (grn_ctx_get(context, name, strlen(name)))

grn_obj *grn_expr_get_value(grn_ctx *ctx, grn_obj *expr, int offset);

typedef struct _BenchmarkData
{
  gchar *base_dir;
  gboolean report_result;

  grn_ctx *context;
  grn_obj *database;
  grn_obj *geo_distance_proc;
  grn_obj *expression;
  grn_obj *start_point;
  grn_obj *end_point;
} BenchmarkData;

static void
bench_geo_distance(gpointer user_data)
{
  BenchmarkData *data = user_data;

  grn_proc_call(data->context, data->geo_distance_proc,
                2, data->expression);
}

static void
bench_setup_common(gpointer user_data)
{
  BenchmarkData *data = user_data;

  grn_ctx_init(data->context, GRN_CTX_USE_QL);
  data->database = grn_db_create(data->context, NULL, NULL);
  data->expression = grn_expr_create(data->context, NULL, 0);
}

static void
bench_setup_points(gpointer user_data,
                   const gchar *start_point_string,
                   const gchar *end_point_string,
                   grn_builtin_type wgs84_or_tgs)
{
  BenchmarkData *data = user_data;
  grn_obj start_point_text, end_point_text;

  GRN_TEXT_INIT(&start_point_text, 0);
  GRN_TEXT_INIT(&end_point_text, 0);
  GRN_TEXT_SETS(data->context, &start_point_text, start_point_string);
  GRN_TEXT_SETS(data->context, &end_point_text, end_point_string);

  data->start_point = grn_obj_open(data->context, GRN_BULK, 0, wgs84_or_tgs);
  data->end_point = grn_obj_open(data->context, GRN_BULK, 0, wgs84_or_tgs);
  grn_obj_cast(data->context, &start_point_text, data->start_point, GRN_FALSE);
  grn_obj_cast(data->context, &end_point_text, data->end_point, GRN_FALSE);
  grn_ctx_push(data->context, data->start_point);
  grn_ctx_push(data->context, data->end_point);

  grn_obj_unlink(data->context, &start_point_text);
  grn_obj_unlink(data->context, &end_point_text);
}

static void
bench_setup_wgs84(gpointer user_data)
{
  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "127980000x502560000",
                     "128880000x503640000",
                     GRN_DB_WGS84_GEO_POINT);
}

static void
bench_setup_tgs(gpointer user_data)
{
  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "127980000x502560000",
                     "128880000x503640000",
                     GRN_DB_TOKYO_GEO_POINT);
}

static void
bench_setup_rectangular_wgs84(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_wgs84(user_data);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_tgs(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_tgs(user_data);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_1st_to_2nd_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "128452975x503157902",
                     "139380000x-31920000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_2nd_to_1st_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "139380000x-31920000",
                     "128452975x503157902",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_1st_to_3rd_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "128452975x503157902",
                     "-56880000x-172310000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_3rd_to_1st_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-56880000x-172310000",
                     "128452975x503157902",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_1st_to_4th_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "128452975x503157902",
                     "-122100000x66300000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_4th_to_1st_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-122100000x66300000",
                     "128452975x503157902",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_2nd_to_4th_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "139380000x-31920000",
                     "-122100000x66300000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_4th_to_2nd_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-122100000x66300000",
                     "139380000x-31920000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_1st_to_2nd_quadrant_long(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "128452975x503157902",
                     "135960000x-440760000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_2nd_to_1st_quadrant_long(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "135960000x-440760000",
                     "128452975x503157902",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_2nd_to_3rd_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "135960000x-440760000",
                     "-56880000x-172310000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_3rd_to_2nd_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-56880000x-172310000",
                     "135960000x-440760000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_3rd_to_4th_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-56880000x-172310000",
                     "-122100000x66300000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_4th_to_3rd_quadrant_short(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-122100000x66300000",
                     "-56880000x-172310000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_3rd_to_4th_quadrant_long(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-56880000x-172310000",
                     "-121926000x544351000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_rectangular_wgs84_4th_to_3rd_quadrant_long(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_common(user_data);
  bench_setup_points(user_data,
                     "-121926000x544351000",
                     "-56880000x-172310000",
                     GRN_DB_WGS84_GEO_POINT);
  data->geo_distance_proc = GET(data->context, "geo_distance");
}

static void
bench_setup_spherical_wgs84(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_wgs84(user_data);
  data->geo_distance_proc = GET(data->context, "geo_distance2");
}

static void
bench_setup_spherical_tgs(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_tgs(user_data);
  data->geo_distance_proc = GET(data->context, "geo_distance2");
}

static void
bench_setup_hubeny_wgs84(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_wgs84(user_data);
  data->geo_distance_proc = GET(data->context, "geo_distance3");
}

static void
bench_setup_hubeny_tgs(gpointer user_data)
{
  BenchmarkData *data = user_data;

  bench_setup_tgs(user_data);
  data->geo_distance_proc = GET(data->context, "geo_distance3");
}

static void
bench_teardown(gpointer user_data)
{
  BenchmarkData *data = user_data;

  if (data->report_result) {
      grn_obj *result;
      result = grn_expr_get_value(data->context, data->expression, 0);
      g_print("result: %g\n", GRN_FLOAT_VALUE(result));
      /* http://vldb.gsi.go.jp/ says '38820.79' in WGS84 and
         '38816.42' in Tokyo geodetic system for a distance
         between '127980000x502560000' and '128880000x503640000'. */
  }

  grn_obj_unlink(data->context, data->end_point);
  grn_obj_unlink(data->context, data->start_point);
  grn_obj_unlink(data->context, data->expression);
  grn_obj_unlink(data->context, data->database);
  grn_ctx_fin(data->context);
}

int
main(int argc, gchar **argv)
{
  grn_rc rc;
  BenchmarkData data;
  BenchReporter *reporter;
  gint n = 1000;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    g_print("failed to initialize Groonga: <%d>: %s\n",
            rc, grn_get_global_error_message());
    return EXIT_FAILURE;
  }
  bench_init(&argc, &argv);

  data.report_result = g_getenv("GROONGA_BENCH_REPORT_RESULT") != NULL;
  data.context = g_new(grn_ctx, 1);

  {
    const gchar *groonga_bench_n;
    groonga_bench_n = g_getenv("GROONGA_BENCH_N");
    if (groonga_bench_n) {
      n = atoi(groonga_bench_n);
    }
  }

  reporter = bench_reporter_new();

#define REGISTER(label, setup)                          \
  bench_reporter_register(reporter, label, n,           \
                          bench_setup_ ## setup,        \
                          bench_geo_distance,           \
                          bench_teardown,               \
                          &data)
  REGISTER("rectangular (WGS84)", rectangular_wgs84);
  REGISTER("rectangular (TOKYO)", rectangular_tgs);
  REGISTER("rectangular (WGS84 Tokyo to Lisbon)",
            rectangular_wgs84_1st_to_2nd_quadrant_short);
  REGISTER("rectangular (WGS84 Lisbon to Tokyo)",
            rectangular_wgs84_2nd_to_1st_quadrant_short);
  REGISTER("rectangular (WGS84 Tokyo to San Francisco)",
            rectangular_wgs84_1st_to_2nd_quadrant_long);
  REGISTER("rectangular (WGS84 San Francisco to Tokyo)",
            rectangular_wgs84_2nd_to_1st_quadrant_long);
  REGISTER("rectangular (WGS84 Brasplia to Cape Town)",
            rectangular_wgs84_3rd_to_4th_quadrant_short);
  REGISTER("rectangular (WGS84 Cape Town to Brasplia)",
            rectangular_wgs84_4th_to_3rd_quadrant_short);
  REGISTER("rectangular (WGS84 Brasplia to Sydney)",
            rectangular_wgs84_3rd_to_4th_quadrant_long);
  REGISTER("rectangular (WGS84 Sydney to Brasplia)",
            rectangular_wgs84_4th_to_3rd_quadrant_long);
  REGISTER("rectangular (WGS84 Tokyo to Brasplia)",
            rectangular_wgs84_1st_to_4th_quadrant_short);
  REGISTER("rectangular (WGS84 Brasplia to Tokyo)",
            rectangular_wgs84_4th_to_1st_quadrant_short);
  REGISTER("rectangular (WGS84 Lisbon to Cape Town)",
            rectangular_wgs84_2nd_to_3rd_quadrant_short);
  REGISTER("rectangular (WGS84 Cape Town to Lisbon)",
            rectangular_wgs84_3rd_to_2nd_quadrant_short);
  REGISTER("rectangular (WGS84 Tokyo to Cape Town)",
            rectangular_wgs84_1st_to_3rd_quadrant_short);
  REGISTER("rectangular (WGS84 Cape Town to Tokyo)",
            rectangular_wgs84_3rd_to_1st_quadrant_short);
  REGISTER("rectangular (WGS84 Lisbon to Cape Town)",
            rectangular_wgs84_2nd_to_4th_quadrant_short);
  REGISTER("rectangular (WGS84 Cape Town to Lisbon)",
            rectangular_wgs84_4th_to_2nd_quadrant_short);
  REGISTER("spherical (WGS84)", spherical_wgs84);
  REGISTER("spherical (TOKYO)", spherical_tgs);
  REGISTER("hubeny (WGS84)", hubeny_wgs84);
  REGISTER("hubeny (TOKYO)", hubeny_tgs);
#undef REGISTER

  bench_reporter_run(reporter);
  g_object_unref(reporter);

  g_free(data.context);

  bench_quit();
  grn_fin();

  return EXIT_SUCCESS;
}
