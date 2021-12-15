/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2017  Kouhei Sutou <kou@clear-code.com>

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
  Groonga: eb65125330b3a8f920693ef3ad53011c7412f2c9
  CFLAGS: -O2 -g3
  CPU: Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz

  % make --silent -C benchmark run-bench-cache
  run-bench-cache:
           (total)    (average)  (median)
     1000: (0.0458s)  (0.4576ms) (0.4170ms)
    10000: (0.3464s)  (0.0035s)  (0.0034s) 
  % GRN_CACHE_TYPE=persistent make --silent -C benchmark run-bench-cache
  run-bench-cache:
           (total)    (average)  (median)
     1000: (0.0480s)  (0.4801ms) (0.4700ms)
    10000: (0.4033s)  (0.0040s)  (0.0040s) 
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <grn_cache.h>

#include "lib/benchmark.h"

typedef struct _BenchmarkData
{
  grn_ctx *context;
  grn_cache *cache;
  grn_obj value;
} BenchmarkData;

static void
bench_n(BenchmarkData *data, gint64 n)
{
  gint64 i;
  grn_ctx *ctx;
  grn_cache *cache;
  grn_obj *value;
  grn_obj fetch_buffer;

  ctx = data->context;
  cache = data->cache;
  value = &(data->value);
  GRN_TEXT_INIT(&fetch_buffer, 0);
  for (i = 0; i < n; i++) {
    char key[GRN_TABLE_MAX_KEY_SIZE];
    grn_snprintf(key,
                 GRN_TABLE_MAX_KEY_SIZE,
                 GRN_TABLE_MAX_KEY_SIZE,
                 "key:%" GRN_FMT_INT64D,
                 i);
    GRN_BULK_REWIND(&fetch_buffer);
    grn_cache_fetch(ctx, cache, key, strlen(key), &fetch_buffer);
    grn_cache_update(ctx, cache, key, strlen(key), value);
  }
  GRN_OBJ_FIN(ctx, &fetch_buffer);
}

static void
bench_1000(gpointer user_data)
{
  BenchmarkData *data = user_data;
  bench_n(data, 1000);
}

static void
bench_10000(gpointer user_data)
{
  BenchmarkData *data = user_data;
  bench_n(data, 10000);
}

static void
bench_setup(gpointer user_data)
{
  BenchmarkData *data = user_data;

  data->cache = grn_cache_open(data->context);
  GRN_TEXT_INIT(&(data->value), 0);
  while (GRN_TEXT_LEN(&(data->value)) < 1024) {
    GRN_TEXT_PUTS(data->context, &(data->value), "XXXXXXXXXXX");
  }
}

static void
bench_teardown(gpointer user_data)
{
  BenchmarkData *data = user_data;

  grn_obj_close(data->context, &(data->value));
  grn_cache_close(data->context, data->cache);
}

int
main(int argc, gchar **argv)
{
  grn_rc rc;
  BenchmarkData data;
  BenchReporter *reporter;
  gchar *base_dir;
  grn_ctx ctx;
  gint n = 100;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    g_print("failed to initialize Groonga: <%d>: %s\n",
            rc, grn_get_global_error_message());
    return EXIT_FAILURE;
  }
  bench_init(&argc, &argv);

  grn_ctx_init(&ctx, 0);

  data.context = &ctx;

  base_dir = g_build_filename(g_get_tmp_dir(), "groonga-bench", NULL);
  bench_utils_remove_path_recursive_force(base_dir);
  g_mkdir_with_parents(base_dir, 0755);

  reporter = bench_reporter_new();
  bench_reporter_register(reporter, "1000", n,
                          bench_setup, bench_1000,   bench_teardown, &data);
  bench_reporter_register(reporter, "10000", n,
                          bench_setup, bench_10000,  bench_teardown, &data);
  bench_reporter_run(reporter);
  g_object_unref(reporter);

  grn_ctx_fin(&ctx);

  bench_utils_remove_path_recursive_force(base_dir);

  bench_quit();
  grn_fin();

  return EXIT_SUCCESS;
}
