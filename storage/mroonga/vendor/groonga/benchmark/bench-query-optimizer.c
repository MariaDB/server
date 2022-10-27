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
  Groonga: c4379140c02699e3c74b94cd9e7b88d372202aa5

  CFLAGS: -O0 -g3
  % make --quiet -C benchmark run-bench-query-optimizer
  run-bench-query-optimizer:
  Process 100 times in each pattern
                                 (time)
     1 condition: with    mruby: (0.0362s)
     1 condition: without mruby: (0.0216s)
    4 conditions: with    mruby: (0.0864s)
    4 conditions: without mruby: (0.0271s)

  CFLAGS: -O2 -g
  % make --quiet -C benchmark run-bench-query-optimizer
  run-bench-query-optimizer:
  Process 100 times in each pattern
                                 (time)
     1 condition: with    mruby: (0.0243s)
     1 condition: without mruby: (0.0159s)
    4 conditions: with    mruby: (0.0452s)
    4 conditions: without mruby: (0.0188s)
*/

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <groonga.h>

#include "lib/benchmark.h"

typedef struct _BenchmarkData {
  grn_ctx context;
  grn_obj *database;
  grn_bool use_mruby;
  GString *command;
} BenchmarkData;

static void
bench(gpointer user_data)
{
  BenchmarkData *data = user_data;
  grn_ctx *context = &(data->context);
  char *response;
  unsigned int response_length;
  int flags;

  grn_ctx_send(context, data->command->str, data->command->len, 0);
  grn_ctx_recv(context, &response, &response_length, &flags);
}

static void
bench_setup(gpointer user_data)
{
  BenchmarkData *data = user_data;

  if (data->use_mruby) {
    g_setenv("GRN_MRUBY_ENABLED", "yes", TRUE);
  } else {
    g_setenv("GRN_MRUBY_ENABLED", "no", TRUE);
  }
  grn_ctx_init(&(data->context), 0);
  grn_ctx_use(&(data->context), data->database);
}

static void
bench_teardown(gpointer user_data)
{
  BenchmarkData *data = user_data;

  grn_ctx_fin(&(data->context));
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

static grn_obj *
setup_database(grn_ctx *context)
{
  gchar *tmp_dir;
  gchar *database_path;
  grn_obj *database;
  const gchar *warmup_command = "dump";
  gchar *response;
  unsigned int response_length;
  int flags;

  tmp_dir = get_tmp_dir();
  database_path = g_build_filename(tmp_dir, "query-optimizer", "db", NULL);
  database = grn_db_open(context, database_path);
  grn_ctx_send(context, warmup_command, strlen(warmup_command), 0);
  grn_ctx_recv(context, &response, &response_length, &flags);

  g_free(database_path);

  return database;
}

static void
teardown_database(grn_ctx *context, grn_obj *database)
{
  grn_obj_close(context, database);
}

int
main(int argc, gchar **argv)
{
  grn_rc rc;
  grn_ctx context;
  grn_obj *database;
  BenchReporter *reporter;
  gint n = 100;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    g_print("failed to initialize Groonga: <%d>: %s\n",
            rc, grn_get_global_error_message());
    return EXIT_FAILURE;
  }
  bench_init(&argc, &argv);

  grn_ctx_init(&context, 0);

  database = setup_database(&context);

  reporter = bench_reporter_new();

  g_print("Process %d times in each pattern\n", n);
  {
    BenchmarkData data_one_condition_with_mruby;
    BenchmarkData data_one_condition_without_mruby;
    BenchmarkData data_multiple_conditions_with_mruby;
    BenchmarkData data_multiple_conditions_without_mruby;

#define REGISTER(label, data, use_mruby_, command_)     \
    data.database = database;                           \
    data.use_mruby = use_mruby_;                        \
    data.command = g_string_new(command_);              \
    bench_reporter_register(reporter, label,            \
                            n,                          \
                            bench_setup,                \
                            bench,                      \
                            bench_teardown,             \
                            &data)

    REGISTER("1 condition: with    mruby", data_one_condition_with_mruby,
             GRN_TRUE,
             "select Entries --cache no --query 'name:@Groonga'");
    REGISTER("1 condition: without mruby", data_one_condition_without_mruby,
             GRN_FALSE,
             "select Entries --cache no --query 'name:@Groonga'");
    REGISTER("4 conditions: with    mruby",
             data_multiple_conditions_with_mruby,
             GRN_TRUE,
             "select Entries --cache no --filter '"
             "name @ \"Groonga\" && "
             "description @ \"search\" && "
             "last_modified >= \"2014-2-9 00:00:00\" && "
             "last_modified <= \"2014-11-29 00:00:00\""
             "'");
    REGISTER("4 conditions: without mruby",
             data_multiple_conditions_without_mruby,
             GRN_FALSE,
             "select Entries --cache no --filter '"
             "name @ \"Groonga\" && "
             "description @ \"search\" && "
             "last_modified >= \"2014-2-9 00:00:00\" && "
             "last_modified <= \"2014-11-29 00:00:00\""
             "'");

#undef REGISTER

    bench_reporter_run(reporter);
  }
  g_object_unref(reporter);

  teardown_database(&context, database);

  grn_ctx_fin(&context);

  grn_fin();

  return EXIT_SUCCESS;
}
