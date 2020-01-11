/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2015-2016  Kouhei Sutou <kou@clear-code.com>

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

#include <string.h>
#include <stdlib.h>

#include <groonga.h>

#include "lib/benchmark.h"

typedef struct _BenchmarkData
{
  gchar *base_dir;
  grn_ctx *context;
  grn_obj *source_table;
  grn_obj *result_set;
} BenchmarkData;

static void
bench_n(BenchmarkData *data, gint64 n)
{
  gint64 i;
  grn_ctx *ctx;
  grn_hash *result_set;

  ctx = data->context;
  result_set = (grn_hash *)data->result_set;
  for (i = 0; i < n; i++) {
    grn_id id = i;
    grn_hash_add(ctx, result_set, &id, sizeof(grn_id), NULL, NULL);
  }
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
bench_100000(gpointer user_data)
{
  BenchmarkData *data = user_data;
  bench_n(data, 100000);
}

static void
bench_setup(gpointer user_data)
{
  BenchmarkData *data = user_data;

  data->result_set = grn_table_create(data->context,
                                      NULL, 0, NULL,
                                      GRN_TABLE_HASH_KEY | GRN_OBJ_WITH_SUBREC,
                                      data->source_table,
                                      NULL);

}

static void
bench_teardown(gpointer user_data)
{
  BenchmarkData *data = user_data;

  grn_obj_close(data->context, data->result_set);
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

  data.context = &ctx;

  base_dir = g_build_filename(g_get_tmp_dir(), "groonga-bench", NULL);
  bench_utils_remove_path_recursive_force(base_dir);
  g_mkdir_with_parents(base_dir, 0755);

  {
    gchar *database_path;
    const gchar *source_table_name = "Sources";

    grn_ctx_init(&ctx, 0);
    database_path = g_build_filename(base_dir, "db", NULL);
    grn_db_create(&ctx, database_path, NULL);
    g_free(database_path);

    data.source_table = grn_table_create(&ctx,
                                         source_table_name,
                                         strlen(source_table_name),
                                         NULL,
                                         GRN_TABLE_PAT_KEY | GRN_OBJ_PERSISTENT,
                                         grn_ctx_at(&ctx, GRN_DB_SHORT_TEXT),
                                         NULL);
  }

  reporter = bench_reporter_new();
  bench_reporter_register(reporter, "1000", n,
                          bench_setup, bench_1000,   bench_teardown, &data);
  bench_reporter_register(reporter, "10000", n,
                          bench_setup, bench_10000,  bench_teardown, &data);
  bench_reporter_register(reporter, "100000", n,
                          bench_setup, bench_100000, bench_teardown, &data);
  bench_reporter_run(reporter);
  g_object_unref(reporter);

  grn_ctx_fin(&ctx);

  bench_utils_remove_path_recursive_force(data.base_dir);

  bench_quit();
  grn_fin();

  return EXIT_SUCCESS;
}
