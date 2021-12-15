/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2014-2016  Kouhei Sutou <kou@clear-code.com>

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
  Groonga: 09a4c4e00832fb90dee74c5b97b7cf0f5952f85b

  CPU Intel(R) Core(TM) i7 CPU         860  @ 2.80GHz (fam: 06, model: 1e, stepping: 05)

  CFLAGS: -O0 -g3
  % make --quiet -C benchmark run-bench-range-select
  run-bench-range-select:
  Process 10 times in each pattern
                                               (total)    (average)  (median)
    (   500,    600] (   1000):    with mruby: (0.0058s)  (0.5773ms) (0.5830ms)
    (   500,    600] (   1000): without mruby: (0.0062s)  (0.6203ms) (0.6200ms)
    (  5000,   5100] (  10000):    with mruby: (0.0058s)  (0.5827ms) (0.5800ms)
    (  5000,   5100] (  10000): without mruby: (0.0473s)  (0.0047s)  (0.0048s)
    ( 50000,  50100] ( 100000):    with mruby: (0.0064s)  (0.6397ms) (0.6370ms)
    ( 50000,  50100] ( 100000): without mruby: (0.4498s)  (0.0450s)  (0.0442s)
    (500000, 500100] (1000000):    with mruby: (0.0057s)  (0.5710ms) (0.5190ms)
    (500000, 500100] (1000000): without mruby: (4.3193s)  (0.4319s)  (0.4306s)

  CFLAGS: -O2 -g
  % make --quiet -C benchmark run-bench-range-select
  run-bench-range-select:
  Process 10 times in each pattern
                                               (total)    (average)  (median)
    (   500,    600] (   1000):    with mruby: (0.0031s)  (0.3058ms) (0.2890ms)
    (   500,    600] (   1000): without mruby: (0.0031s)  (0.3132ms) (0.3090ms)
    (  5000,   5100] (  10000):    with mruby: (0.0031s)  (0.3063ms) (0.3100ms)
    (  5000,   5100] (  10000): without mruby: (0.0239s)  (0.0024s)  (0.0023s)
    ( 50000,  50100] ( 100000):    with mruby: (0.0028s)  (0.2825ms) (0.2660ms)
    ( 50000,  50100] ( 100000): without mruby: (0.2117s)  (0.0212s)  (0.0211s)
    (500000, 500100] (1000000):    with mruby: (0.0028s)  (0.2757ms) (0.2650ms)
    (500000, 500100] (1000000): without mruby: (2.0874s)  (0.2087s)  (0.2092s)
*/

#include <stdio.h>
#include <string.h>

#include <grn_db.h>
#include <groonga.h>

#include "lib/benchmark.h"

#define GET(context, name) (grn_ctx_get(context, name, strlen(name)))

typedef struct _BenchmarkData
{
  grn_ctx context;
  grn_obj *database;
  guint n_records;
  grn_bool use_mruby;
  const gchar *command;
} BenchmarkData;

static void
run_command(grn_ctx *context, const gchar *command)
{
  gchar *response;
  unsigned int response_length;
  int flags;

  grn_ctx_send(context, command, strlen(command), 0);
  grn_ctx_recv(context, &response, &response_length, &flags);
}

static void
bench(gpointer user_data)
{
  BenchmarkData *data = user_data;
  grn_ctx *context = &(data->context);

  run_command(context, data->command);
}

static gchar *
get_tmp_dir(void)
{
  gchar *current_dir;
  gchar *tmp_dir;

  current_dir = g_get_current_dir();
  tmp_dir = g_build_filename(current_dir, "tmp", NULL);
  g_free(current_dir);

  return tmp_dir;
}

static void
setup_database(BenchmarkData *data)
{
  grn_ctx *context = &(data->context);
  gchar *tmp_dir;
  gchar *database_last_component_name;
  gchar *database_path;
  guint i;

  tmp_dir = get_tmp_dir();
  database_last_component_name =
    g_strdup_printf("db-%d-%s-mruby",
                    data->n_records,
                    data->use_mruby ? "with" : "without");
  database_path = g_build_filename(tmp_dir,
                                   "range-select",
                                   database_last_component_name,
                                   NULL);
  g_free(database_last_component_name);

  if (g_file_test(database_path, G_FILE_TEST_EXISTS)) {
    data->database = grn_db_open(context, database_path);
    run_command(context, "dump");
  } else {
    data->database = grn_db_create(context, database_path, NULL);

    run_command(context, "table_create Entries TABLE_NO_KEY");
    run_command(context, "column_create Entries rank COLUMN_SCALAR Int32");
    run_command(context, "table_create Ranks TABLE_PAT_KEY Int32");
    run_command(context,
                "column_create Ranks entries_rank COLUMN_INDEX Entries rank");

    run_command(context, "load --table Entries");
    run_command(context, "[");
    for (i = 0; i < data->n_records; i++) {
#define BUFFER_SIZE 4096
      gchar buffer[BUFFER_SIZE];
      const gchar *separator;
      if (i == (data->n_records - 1)) {
        separator = "";
      } else {
        separator = ",";
      }
      snprintf(buffer, BUFFER_SIZE, "{\"rank\": %u}%s", i, separator);
      run_command(context, buffer);
#undef BUFFER_SIZE
    }
    run_command(context, "]");
  }

  g_free(database_path);
}

static void
bench_startup(BenchmarkData *data)
{
  if (data->use_mruby) {
    g_setenv("GRN_MRUBY_ENABLED", "yes", TRUE);
  } else {
    g_setenv("GRN_MRUBY_ENABLED", "no", TRUE);
  }
  grn_ctx_init(&(data->context), 0);
  setup_database(data);
}

static void
bench_shutdown(BenchmarkData *data)
{
  grn_ctx *context = &(data->context);

  grn_obj_close(context, data->database);
  grn_ctx_fin(context);
}

int
main(int argc, gchar **argv)
{
  grn_rc rc;
  BenchReporter *reporter;
  gint n = 10;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    g_print("failed to initialize Groonga: <%d>: %s\n",
            rc, grn_get_global_error_message());
    return EXIT_FAILURE;
  }

  g_print("Process %d times in each pattern\n", n);

  bench_init(&argc, &argv);
  reporter = bench_reporter_new();

  {
    BenchmarkData data_small_with_mruby;
    BenchmarkData data_small_without_mruby;
    BenchmarkData data_medium_with_mruby;
    BenchmarkData data_medium_without_mruby;
    BenchmarkData data_large_with_mruby;
    BenchmarkData data_large_without_mruby;
    BenchmarkData data_very_large_with_mruby;
    BenchmarkData data_very_large_without_mruby;

#define REGISTER(data, n_records_, min, max, use_mruby_)        \
    do {                                                        \
      gchar *label;                                             \
      label = g_strdup_printf("(%6d, %6d] (%7d): %7s mruby",    \
                              min, max, n_records_,             \
                              use_mruby_ ? "with" : "without"); \
      data.use_mruby = use_mruby_;                              \
      data.n_records = n_records_;                              \
      data.command =                                            \
        "select Entries --cache no "                            \
        "--filter 'rank > " #min " && rank <= " #max "'";       \
      bench_startup(&data);                                     \
      bench_reporter_register(reporter, label,                  \
                              n,                                \
                              NULL,                             \
                              bench,                            \
                              NULL,                             \
                              &data);                           \
      g_free(label);                                            \
    } while(FALSE)

    REGISTER(data_small_with_mruby,
             1000,
             500, 600,
             GRN_TRUE);
    REGISTER(data_small_without_mruby,
             1000,
             500, 600,
             GRN_FALSE);
    REGISTER(data_medium_with_mruby,
             10000,
             5000, 5100,
             GRN_TRUE);
    REGISTER(data_medium_without_mruby,
             10000,
             5000, 5100,
             GRN_FALSE);
    REGISTER(data_large_with_mruby,
             100000,
             50000, 50100,
             GRN_TRUE);
    REGISTER(data_large_without_mruby,
             100000,
             50000, 50100,
             GRN_FALSE);
    REGISTER(data_very_large_with_mruby,
             1000000,
             500000, 500100,
             GRN_TRUE);
    REGISTER(data_very_large_without_mruby,
             1000000,
             500000, 500100,
             GRN_FALSE);

#undef REGISTER

    bench_reporter_run(reporter);

    bench_shutdown(&data_small_with_mruby);
    bench_shutdown(&data_small_without_mruby);
    bench_shutdown(&data_medium_with_mruby);
    bench_shutdown(&data_medium_without_mruby);
    bench_shutdown(&data_large_with_mruby);
    bench_shutdown(&data_large_without_mruby);
    bench_shutdown(&data_very_large_with_mruby);
    bench_shutdown(&data_very_large_without_mruby);
  }
  g_object_unref(reporter);

  grn_fin();

  return EXIT_SUCCESS;
}
