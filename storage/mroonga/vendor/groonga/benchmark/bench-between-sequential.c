/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2016  Kouhei Sutou <kou@clear-code.com>

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
  Intel(R) Core(TM) i7-6700 CPU @ 3.40GHz

  CFLAGS: -O2 -g

  Groonga: e2971d9a555a90724b76964cc8c8805373500b4a
  % make --quiet -C benchmark run-bench-between-sequential
  run-bench-between-sequential:
  Process 10 times in each pattern
                                         (total)    (average)  (median)
    (   500,    600] (   1000): between: (0.0528s)  (0.0053s)  (0.0043s)
    (   500,    600] (   1000):   range: (0.0120s)  (0.0012s)  (0.2500ms)
    (  5000,   5100] (  10000): between: (0.4052s)  (0.0405s)  (0.0395s)
    (  5000,   5100] (  10000):   range: (0.0197s)  (0.0020s)  (0.0010s)
    ( 50000,  50100] ( 100000): between: (3.9343s)  (0.3934s)  (0.3900s)
    ( 50000,  50100] ( 100000):   range: (0.0969s)  (0.0097s)  (0.0088s)
    (500000, 500100] (1000000): between: (38.2969s)  (3.8297s)  (3.7983s)
    (500000, 500100] (1000000):   range: (0.9158s)  (0.0916s)  (0.0900s)

  Groonga: 35e4e431bb7660b3170e98c329f7219bd6723f05
  % make --quiet -C benchmark run-bench-between-sequential
  run-bench-between-sequential:
  Process 10 times in each pattern
                                         (total)    (average)  (median)
    (   500,    600] (   1000): between: (0.0130s)  (0.0013s)  (0.2590ms)
    (   500,    600] (   1000):   range: (0.0124s)  (0.0012s)  (0.2530ms)
    (  5000,   5100] (  10000): between: (0.0163s)  (0.0016s)  (0.6440ms)
    (  5000,   5100] (  10000):   range: (0.0205s)  (0.0021s)  (0.0011s)
    ( 50000,  50100] ( 100000): between: (0.0611s)  (0.0061s)  (0.0051s)
    ( 50000,  50100] ( 100000):   range: (0.1004s)  (0.0100s)  (0.0091s)
    (500000, 500100] (1000000): between: (0.4518s)  (0.0452s)  (0.0442s)
    (500000, 500100] (1000000):   range: (0.8866s)  (0.0887s)  (0.0878s)
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
  database_last_component_name = g_strdup_printf("db-%d", data->n_records);
  database_path = g_build_filename(tmp_dir,
                                   "between-sequential",
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
    BenchmarkData data_small_between;
    BenchmarkData data_small_range;
    BenchmarkData data_medium_between;
    BenchmarkData data_medium_range;
    BenchmarkData data_large_between;
    BenchmarkData data_large_range;
    BenchmarkData data_very_large_between;
    BenchmarkData data_very_large_range;

#define REGISTER(data, n_records_, min, max, is_between)        \
    do {                                                        \
      gchar *label;                                             \
      label =                                                   \
        g_strdup_printf("(%6d, %6d] (%7d): %7s",                \
                        min, max, n_records_,                   \
                        is_between ? "between" : "range");      \
      data.n_records = n_records_;                              \
      if (is_between) {                                         \
        data.command =                                          \
          "select Entries --cache no "                          \
          "--filter "                                           \
          "'between(rank, " #min ", \"exclude\","               \
                         " " #max ", \"include\")'";            \
      } else {                                                  \
        data.command =                                          \
          "select Entries --cache no "                          \
          "--filter 'rank > " #min " &&  rank <= " #max "'";    \
      }                                                         \
      bench_startup(&data);                                     \
      bench_reporter_register(reporter, label,                  \
                              n,                                \
                              NULL,                             \
                              bench,                            \
                              NULL,                             \
                              &data);                           \
      g_free(label);                                            \
    } while(FALSE)

    REGISTER(data_small_between,
             1000,
             500, 600,
             TRUE);
    REGISTER(data_small_range,
             1000,
             500, 600,
             FALSE);
    REGISTER(data_medium_between,
             10000,
             5000, 5100,
             TRUE);
    REGISTER(data_medium_range,
             10000,
             5000, 5100,
             FALSE);
    REGISTER(data_large_between,
             100000,
             50000, 50100,
             TRUE);
    REGISTER(data_large_range,
             100000,
             50000, 50100,
             FALSE);
    REGISTER(data_very_large_between,
             1000000,
             500000, 500100,
             TRUE);
    REGISTER(data_very_large_range,
             1000000,
             500000, 500100,
             FALSE);

#undef REGISTER

    bench_reporter_run(reporter);

    bench_shutdown(&data_small_between);
    bench_shutdown(&data_small_range);
    bench_shutdown(&data_medium_between);
    bench_shutdown(&data_medium_range);
    bench_shutdown(&data_large_between);
    bench_shutdown(&data_large_range);
    bench_shutdown(&data_very_large_between);
    bench_shutdown(&data_very_large_range);
  }
  g_object_unref(reporter);

  grn_fin();

  return EXIT_SUCCESS;
}
