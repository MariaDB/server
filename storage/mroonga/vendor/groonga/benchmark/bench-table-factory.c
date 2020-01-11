/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2008-2016  Kouhei Sutou <kou@clear-code.com>

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

#define DEFAULT_FLAGS (GRN_OBJ_PERSISTENT | GRN_OBJ_TABLE_PAT_KEY)
#define DEFAULT_VALUE_SIZE (1024)
#define VALUE_TYPE_NAME "<value_type>"

typedef struct _BenchmarkData
{
  gchar *base_dir;

  grn_ctx *context;
  const char *name;
  unsigned name_size;
  char *path;
  grn_obj_flags flags;
  grn_obj *key_type;
  unsigned value_size;
  grn_encoding encoding;
} BenchmarkData;

static void
bench_normal(gpointer user_data)
{
  BenchmarkData *data = user_data;
  grn_obj *table;
  grn_obj *value_type = grn_ctx_get(data->context, VALUE_TYPE_NAME, strlen(VALUE_TYPE_NAME));
  if (!value_type) {
    value_type = grn_type_create(data->context, VALUE_TYPE_NAME, strlen(VALUE_TYPE_NAME),
                                 0, data->value_size);
  }
  table = grn_table_create(data->context,
                           data->name, data->name_size,
                           data->path, data->flags,
                           data->key_type, value_type);
  grn_obj_close(data->context, table);
}

static void
bench_normal_temporary(gpointer user_data)
{
  BenchmarkData *data = user_data;
  grn_obj *table;
  grn_obj *value_type = grn_ctx_get(data->context, VALUE_TYPE_NAME, strlen(VALUE_TYPE_NAME));
  if (!value_type) {
    value_type = grn_type_create(data->context, VALUE_TYPE_NAME, strlen(VALUE_TYPE_NAME),
                                 0, data->value_size);
  }
  GRN_CTX_SET_ENCODING(data->context, data->encoding);
  table = grn_table_create(data->context,
                           data->name, data->name_size,
                           NULL, data->flags & ~GRN_OBJ_PERSISTENT,
                           data->key_type, value_type);
  grn_obj_close(data->context, table);
}

typedef struct _grn_table_factory
{
  grn_ctx *context;
  char *name;
  unsigned name_size;
  char *path;
  grn_obj_flags flags;
  grn_obj *key_type;
  unsigned value_size;
  grn_encoding encoding;
} grn_table_factory;

static grn_table_factory *
grn_table_factory_create(void)
{
  grn_table_factory *factory;

  factory = g_new0(grn_table_factory, 1);

  factory->context = NULL;
  factory->name = NULL;
  factory->name_size = 0;
  factory->path = NULL;
  factory->flags = DEFAULT_FLAGS;
  factory->key_type = NULL;
  factory->value_size = DEFAULT_VALUE_SIZE;
  factory->encoding = GRN_ENC_DEFAULT;

  return factory;
}

static void
grn_table_factory_set_context(grn_table_factory *factory, grn_ctx *context)
{
  factory->context = context;
}

static void
grn_table_factory_set_name(grn_table_factory *factory, const char *name)
{
  factory->name = g_strdup(name);
  factory->name_size = strlen(name);
}

static void
grn_table_factory_set_path(grn_table_factory *factory, const char *path)
{
  factory->path = g_strdup(path);
  if (path)
    factory->flags |= GRN_OBJ_PERSISTENT;
  else
    factory->flags &= ~GRN_OBJ_PERSISTENT;
}

static void
grn_table_factory_set_key_type(grn_table_factory *factory, grn_obj *key_type)
{
  factory->key_type = key_type;
}

static grn_obj *
grn_table_factory_make(grn_table_factory *factory)
{
  grn_obj *value_type = grn_ctx_get(factory->context, VALUE_TYPE_NAME, strlen(VALUE_TYPE_NAME));
  if (!value_type) {
    value_type = grn_type_create(factory->context, VALUE_TYPE_NAME, strlen(VALUE_TYPE_NAME),
                                 0, factory->value_size);
  }
  GRN_CTX_SET_ENCODING(factory->context, factory->encoding);
  return grn_table_create(factory->context,
                          factory->name, factory->name_size,
                          factory->path, factory->flags,
                          factory->key_type, value_type);
}

static void
grn_table_factory_close(grn_table_factory *factory)
{
  g_free(factory->name);
  g_free(factory->path);
  g_free(factory);
}

static void
bench_factory(gpointer user_data)
{
  BenchmarkData *data = user_data;
  grn_table_factory *factory;
  grn_obj *table;

  factory = grn_table_factory_create();
  grn_table_factory_set_context(factory, data->context);
  grn_table_factory_set_name(factory, data->name);
  grn_table_factory_set_path(factory, data->path);
  grn_table_factory_set_key_type(factory, data->key_type);

  table = grn_table_factory_make(factory);
  grn_obj_close(data->context, table);

  grn_table_factory_close(factory);
}

static void
bench_factory_temporary(gpointer user_data)
{
  BenchmarkData *data = user_data;
  grn_table_factory *factory;
  grn_obj *table;

  factory = grn_table_factory_create();
  grn_table_factory_set_context(factory, data->context);
  grn_table_factory_set_name(factory, data->name);
  grn_table_factory_set_key_type(factory, data->key_type);

  table = grn_table_factory_make(factory);
  grn_obj_close(data->context, table);

  grn_table_factory_close(factory);
}

static void
bench_setup(gpointer user_data)
{
  BenchmarkData *data = user_data;
  const gchar *type_name;

  bench_utils_remove_path_recursive_force(data->base_dir);
  g_mkdir_with_parents(data->base_dir, 0755);

  grn_ctx_init(data->context, GRN_CTX_USE_QL);

  type_name = "name";
  data->key_type = grn_type_create(data->context,
                                   type_name, strlen(type_name),
                                   GRN_OBJ_KEY_UINT, sizeof(grn_id));

}

static void
bench_teardown(gpointer user_data)
{
  BenchmarkData *data = user_data;

  grn_obj_close(data->context, data->key_type);
  grn_ctx_fin(data->context);
  bench_utils_remove_path_recursive_force(data->base_dir);
}

int
main(int argc, gchar **argv)
{
  grn_rc rc;
  BenchmarkData data;
  BenchReporter *reporter;
  gint n = 100;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    g_print("failed to initialize Groonga: <%d>: %s\n",
            rc, grn_get_global_error_message());
    return EXIT_FAILURE;
  }
  bench_init(&argc, &argv);

  data.context = g_new(grn_ctx, 1);
  data.base_dir = g_build_filename(g_get_tmp_dir(), "groonga-bench", NULL);
  data.name = "table";
  data.name_size = strlen(data.name);
  data.path = g_build_filename(data.base_dir, "table", NULL);
  data.flags = DEFAULT_FLAGS;
  data.key_type = NULL;
  data.value_size = DEFAULT_VALUE_SIZE;
  data.encoding = GRN_ENC_DEFAULT;

  reporter = bench_reporter_new();
  bench_reporter_register(reporter, "normal (persistent)", n,
                          bench_setup, bench_normal, bench_teardown, &data);
  bench_reporter_register(reporter, "factory (persistent)", n,
                          bench_setup, bench_factory, bench_teardown, &data);
  bench_reporter_register(reporter, "normal (temporary)", n,
                          bench_setup, bench_normal_temporary, bench_teardown,
                          &data);
  bench_reporter_register(reporter, "factory (temporary)", n,
                          bench_setup, bench_factory_temporary, bench_teardown,
                          &data);
  bench_reporter_run(reporter);
  g_object_unref(reporter);

  bench_utils_remove_path_recursive_force(data.base_dir);

  g_free(data.path);
  g_free(data.base_dir);
  g_free(data.context);

  bench_quit();
  grn_fin();

  return EXIT_SUCCESS;
}
