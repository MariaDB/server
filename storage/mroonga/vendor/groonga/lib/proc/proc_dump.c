/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

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

#include "../grn_proc.h"
#include "../grn_ctx_impl.h"
#include "../grn_db.h"
#include "../grn_str.h"

#include <groonga/plugin.h>

static const size_t DUMP_FLUSH_THRESHOLD_SIZE = 256 * 1024;

typedef struct {
  grn_obj *output;
  grn_bool is_close_opened_object_mode;
  grn_bool have_reference_column;
  grn_bool have_index_column;
  grn_bool is_sort_hash_table;
  grn_obj column_name_buffer;
} grn_dumper;

static void
dumper_collect_statistics_table(grn_ctx *ctx,
                                grn_dumper *dumper,
                                grn_obj *table)
{
  grn_hash *columns;

  columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                            GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
  if (!columns) {
    return;
  }

  grn_table_columns(ctx, table, NULL, 0, (grn_obj *)columns);
  GRN_HASH_EACH_BEGIN(ctx, columns, cursor, id) {
    void *key;
    grn_id column_id;
    grn_obj *column;

    grn_hash_cursor_get_key(ctx, cursor, &key);
    column_id = *((grn_id *)key);

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    column = grn_ctx_at(ctx, column_id);
    if (!column) {
      GRN_PLUGIN_CLEAR_ERROR(ctx);
      goto next_loop;
    }

    if (grn_obj_is_index_column(ctx, column)) {
      dumper->have_index_column = GRN_TRUE;
    } else if (grn_obj_is_reference_column(ctx, column)) {
      dumper->have_reference_column = GRN_TRUE;
    }

  next_loop :
    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  } GRN_HASH_EACH_END(ctx, cursor);
  grn_hash_close(ctx, columns);
}

static void
dumper_collect_statistics(grn_ctx *ctx, grn_dumper *dumper)
{
  GRN_DB_EACH_BEGIN_BY_ID(ctx, cursor, id) {
    void *name;
    int name_size;
    grn_obj *object;

    if (grn_id_is_builtin(ctx, id)) {
      continue;
    }

    name_size = grn_table_cursor_get_key(ctx, cursor, &name);
    if (grn_obj_name_is_column(ctx, name, name_size)) {
      continue;
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    object = grn_ctx_at(ctx, id);
    if (!object) {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      GRN_PLUGIN_CLEAR_ERROR(ctx);
      goto next_loop;
    }

    if (!grn_obj_is_table(ctx, object)) {
      goto next_loop;
    }

    dumper_collect_statistics_table(ctx, dumper, object);

next_loop :
    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  } GRN_DB_EACH_END(ctx, cursor);
}

static void
dump_value_raw(grn_ctx *ctx, grn_obj *output, const char *value, int value_len)
{
  grn_obj escaped_value;
  GRN_TEXT_INIT(&escaped_value, 0);
  grn_text_esc(ctx, &escaped_value, value, value_len);
  /* is no character escaped? */
  /* TODO false positive with spaces inside values */
  if (GRN_TEXT_LEN(&escaped_value) == value_len + 2) {
    GRN_TEXT_PUT(ctx, output, value, value_len);
  } else {
    GRN_TEXT_PUT(ctx, output,
                 GRN_TEXT_VALUE(&escaped_value), GRN_TEXT_LEN(&escaped_value));
  }
  grn_obj_close(ctx, &escaped_value);
}

static void
dump_value(grn_ctx *ctx, grn_dumper *dumper, const char *value, int value_len)
{
  dump_value_raw(ctx, dumper->output, value, value_len);
}

static void
dump_configs(grn_ctx *ctx, grn_dumper *dumper)
{
  grn_obj *config_cursor;

  config_cursor = grn_config_cursor_open(ctx);
  if (!config_cursor)
    return;

  while (grn_config_cursor_next(ctx, config_cursor)) {
    const char *key;
    uint32_t key_size;
    const char *value;
    uint32_t value_size;

    key_size = grn_config_cursor_get_key(ctx, config_cursor, &key);
    value_size = grn_config_cursor_get_value(ctx, config_cursor, &value);

    GRN_TEXT_PUTS(ctx, dumper->output, "config_set ");
    dump_value(ctx, dumper, key, key_size);
    GRN_TEXT_PUTS(ctx, dumper->output, " ");
    dump_value(ctx, dumper, value, value_size);
    GRN_TEXT_PUTC(ctx, dumper->output, '\n');
  }
  grn_obj_close(ctx, config_cursor);
}

static void
dump_plugins(grn_ctx *ctx, grn_dumper *dumper)
{
  grn_obj plugin_names;
  unsigned int i, n;

  GRN_TEXT_INIT(&plugin_names, GRN_OBJ_VECTOR);

  grn_plugin_get_names(ctx, &plugin_names);

  n = grn_vector_size(ctx, &plugin_names);
  if (n == 0) {
    GRN_OBJ_FIN(ctx, &plugin_names);
    return;
  }

  if (GRN_TEXT_LEN(dumper->output) > 0) {
    GRN_TEXT_PUTC(ctx, dumper->output, '\n');
    grn_ctx_output_flush(ctx, 0);
  }
  for (i = 0; i < n; i++) {
    const char *name;
    unsigned int name_size;

    name_size = grn_vector_get_element(ctx, &plugin_names, i, &name, NULL, NULL);
    grn_text_printf(ctx, dumper->output, "plugin_register %.*s\n",
                    (int)name_size, name);
  }

  GRN_OBJ_FIN(ctx, &plugin_names);
}

static void
dump_obj_name_raw(grn_ctx *ctx, grn_obj *output, grn_obj *obj)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;
  name_len = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
  dump_value_raw(ctx, output, name, name_len);
}

static void
dump_obj_name(grn_ctx *ctx, grn_dumper *dumper, grn_obj *obj)
{
  dump_obj_name_raw(ctx, dumper->output, obj);
}

static void
dump_column_name(grn_ctx *ctx, grn_dumper *dumper, grn_obj *column)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;
  name_len = grn_column_name(ctx, column, name, GRN_TABLE_MAX_KEY_SIZE);
  dump_value(ctx, dumper, name, name_len);
}

static void
dump_index_column_sources(grn_ctx *ctx, grn_dumper *dumper, grn_obj *column)
{
  grn_obj sources;
  grn_id *source_ids;
  int i, n;

  GRN_OBJ_INIT(&sources, GRN_BULK, 0, GRN_ID_NIL);
  grn_obj_get_info(ctx, column, GRN_INFO_SOURCE, &sources);

  n = GRN_BULK_VSIZE(&sources) / sizeof(grn_id);
  source_ids = (grn_id *)GRN_BULK_HEAD(&sources);
  if (n > 0) {
    GRN_TEXT_PUTC(ctx, dumper->output, ' ');
  }
  for (i = 0; i < n; i++) {
    grn_id source_id;
    grn_obj *source;

    source_id = *source_ids;
    source_ids++;

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    source = grn_ctx_at(ctx, source_id);
    if (!source) {
      goto next_loop;
    }

    if (i) { GRN_TEXT_PUTC(ctx, dumper->output, ','); }
    switch (source->header.type) {
    case GRN_TABLE_PAT_KEY:
    case GRN_TABLE_DAT_KEY:
    case GRN_TABLE_HASH_KEY:
      GRN_TEXT_PUT(ctx,
                   dumper->output,
                   GRN_COLUMN_NAME_KEY,
                   GRN_COLUMN_NAME_KEY_LEN);
      break;
    default:
      dump_column_name(ctx, dumper, source);
      break;
    }

  next_loop :
    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  }
  grn_obj_close(ctx, &sources);
}

static void
dump_column(grn_ctx *ctx, grn_dumper *dumper, grn_obj *table, grn_obj *column)
{
  grn_id type_id;
  grn_obj *type;
  grn_column_flags flags;
  grn_column_flags default_flags = GRN_OBJ_PERSISTENT;

  type_id = grn_obj_get_range(ctx, column);
  if (dumper->is_close_opened_object_mode) {
    grn_ctx_push_temporary_open_space(ctx);
  }
  type = grn_ctx_at(ctx, type_id);
  if (!type) {
    /* ERR(GRN_RANGE_ERROR, "couldn't get column's type object"); */
    goto exit;
  }

  GRN_TEXT_PUTS(ctx, dumper->output, "column_create ");
  dump_obj_name(ctx, dumper, table);
  GRN_TEXT_PUTC(ctx, dumper->output, ' ');
  dump_column_name(ctx, dumper, column);
  GRN_TEXT_PUTC(ctx, dumper->output, ' ');
  if (type->header.type == GRN_TYPE) {
    default_flags |= type->header.flags;
  }
  flags = grn_column_get_flags(ctx, column);
  grn_dump_column_create_flags(ctx,
                               flags & ~default_flags,
                               dumper->output);
  GRN_TEXT_PUTC(ctx, dumper->output, ' ');
  dump_obj_name(ctx, dumper, type);
  if (column->header.flags & GRN_OBJ_COLUMN_INDEX) {
    dump_index_column_sources(ctx, dumper, column);
  }
  GRN_TEXT_PUTC(ctx, dumper->output, '\n');

exit :
  if (dumper->is_close_opened_object_mode) {
    grn_ctx_pop_temporary_open_space(ctx);
  }
}

static void
dump_columns(grn_ctx *ctx, grn_dumper *dumper, grn_obj *table,
             grn_bool dump_data_column,
             grn_bool dump_reference_column,
             grn_bool dump_index_column)
{
  grn_hash *columns;
  columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                            GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
  if (!columns) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_NO_MEMORY_AVAILABLE,
                     "couldn't create a hash to hold columns");
    return;
  }

  if (grn_table_columns(ctx, table, NULL, 0, (grn_obj *)columns) >= 0) {
    GRN_HASH_EACH_BEGIN(ctx, columns, cursor, id) {
      void *key;
      grn_id column_id;
      grn_obj *column;

      grn_hash_cursor_get_key(ctx, cursor, &key);
      column_id = *((grn_id *)key);

      if (dumper->is_close_opened_object_mode) {
        grn_ctx_push_temporary_open_space(ctx);
      }

      column = grn_ctx_at(ctx, column_id);
      if (!column) {
        GRN_PLUGIN_CLEAR_ERROR(ctx);
        goto next_loop;
      }

      if (grn_obj_is_index_column(ctx, column)) {
        if (dump_index_column) {
          dump_column(ctx, dumper, table, column);
          GRN_PLUGIN_CLEAR_ERROR(ctx);
        }
      } else if (grn_obj_is_reference_column(ctx, column)) {
        if (dump_reference_column) {
          dump_column(ctx, dumper, table, column);
          GRN_PLUGIN_CLEAR_ERROR(ctx);
        }
      } else {
        if (dump_data_column) {
          dump_column(ctx, dumper, table, column);
          GRN_PLUGIN_CLEAR_ERROR(ctx);
        }
      }

    next_loop :
      if (dumper->is_close_opened_object_mode) {
        grn_ctx_pop_temporary_open_space(ctx);
      }
    } GRN_HASH_EACH_END(ctx, cursor);
  }
  grn_hash_close(ctx, columns);
}

static void
dump_record_column_vector(grn_ctx *ctx, grn_dumper *dumper, grn_id id,
                          grn_obj *column, grn_id range_id, grn_obj *buf)
{
  grn_obj *range;
  grn_obj_format *format_argument = NULL;
  grn_obj_format format;

  range = grn_ctx_at(ctx, range_id);
  if (column->header.flags & GRN_OBJ_WITH_WEIGHT) {
    format.flags = GRN_OBJ_FORMAT_WITH_WEIGHT;
    format_argument = &format;
  }

  if (grn_obj_is_table(ctx, range) ||
      (range->header.flags & GRN_OBJ_KEY_VAR_SIZE) == 0) {
    GRN_OBJ_INIT(buf, GRN_UVECTOR, 0, range_id);
    grn_obj_get_value(ctx, column, id, buf);
    grn_text_otoj(ctx, dumper->output, buf, format_argument);
  } else {
    GRN_OBJ_INIT(buf, GRN_VECTOR, 0, range_id);
    grn_obj_get_value(ctx, column, id, buf);
    grn_text_otoj(ctx, dumper->output, buf, format_argument);
  }

  grn_obj_unlink(ctx, range);
  grn_obj_unlink(ctx, buf);
}

static void
dump_record(grn_ctx *ctx, grn_dumper *dumper,
            grn_obj *table,
            grn_id id,
            grn_obj *columns, int n_columns)
{
  int j;
  grn_obj buf;
  grn_obj *column_name = &(dumper->column_name_buffer);

  GRN_TEXT_PUTC(ctx, dumper->output, '[');
  for (j = 0; j < n_columns; j++) {
    grn_bool is_value_column;
    grn_id range;
    grn_obj *column;
    column = GRN_PTR_VALUE_AT(columns, j);
    /* TODO: use grn_obj_is_value_accessor() */
    GRN_BULK_REWIND(column_name);
    grn_column_name_(ctx, column, column_name);
    if (GRN_TEXT_LEN(column_name) == GRN_COLUMN_NAME_VALUE_LEN &&
        !memcmp(GRN_TEXT_VALUE(column_name),
                GRN_COLUMN_NAME_VALUE,
                GRN_COLUMN_NAME_VALUE_LEN)) {
      is_value_column = GRN_TRUE;
    } else {
      is_value_column = GRN_FALSE;
    }
    range = grn_obj_get_range(ctx, column);

    if (j) { GRN_TEXT_PUTC(ctx, dumper->output, ','); }
    switch (column->header.type) {
    case GRN_COLUMN_VAR_SIZE:
    case GRN_COLUMN_FIX_SIZE:
      switch (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
      case GRN_OBJ_COLUMN_VECTOR:
        dump_record_column_vector(ctx, dumper, id, column, range, &buf);
        break;
      case GRN_OBJ_COLUMN_SCALAR:
        {
          GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
          grn_obj_get_value(ctx, column, id, &buf);
          grn_text_otoj(ctx, dumper->output, &buf, NULL);
          grn_obj_unlink(ctx, &buf);
        }
        break;
      default:
        GRN_PLUGIN_ERROR(ctx,
                         GRN_OPERATION_NOT_SUPPORTED,
                         "unsupported column type: %#x",
                         column->header.type);
        break;
      }
      break;
    case GRN_COLUMN_INDEX:
      break;
    case GRN_ACCESSOR:
      {
        GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
        grn_obj_get_value(ctx, column, id, &buf);
        /* XXX maybe, grn_obj_get_range() should not unconditionally return
           GRN_DB_INT32 when column is GRN_ACCESSOR and
           GRN_ACCESSOR_GET_VALUE */
        if (is_value_column) {
          buf.header.domain = grn_obj_get_range(ctx, table);
        }
        grn_text_otoj(ctx, dumper->output, &buf, NULL);
        grn_obj_unlink(ctx, &buf);
      }
      break;
    default:
      GRN_PLUGIN_ERROR(ctx,
                       GRN_OPERATION_NOT_SUPPORTED,
                       "unsupported header type %#x",
                       column->header.type);
      break;
    }
  }
  GRN_TEXT_PUTC(ctx, dumper->output, ']');
  if ((size_t) GRN_TEXT_LEN(dumper->output) >= DUMP_FLUSH_THRESHOLD_SIZE) {
    grn_ctx_output_flush(ctx, 0);
  }
}

static void
dump_records(grn_ctx *ctx, grn_dumper *dumper, grn_obj *table)
{
  grn_table_cursor *cursor;
  int i, n_columns;
  grn_obj columns;
  grn_bool have_index_column = GRN_FALSE;
  grn_bool have_data_column = GRN_FALSE;

  if (grn_table_size(ctx, table) == 0) {
    return;
  }

  if (dumper->is_close_opened_object_mode) {
    grn_ctx_push_temporary_open_space(ctx);
  }

  GRN_PTR_INIT(&columns, GRN_OBJ_VECTOR, GRN_ID_NIL);

  if (table->header.type == GRN_TABLE_NO_KEY) {
    grn_obj *id_accessor;
    id_accessor = grn_obj_column(ctx,
                                 table,
                                 GRN_COLUMN_NAME_ID,
                                 GRN_COLUMN_NAME_ID_LEN);
    GRN_PTR_PUT(ctx, &columns, id_accessor);
  } else if (table->header.domain != GRN_ID_NIL) {
    grn_obj *key_accessor;
    key_accessor = grn_obj_column(ctx,
                                  table,
                                  GRN_COLUMN_NAME_KEY,
                                  GRN_COLUMN_NAME_KEY_LEN);
    GRN_PTR_PUT(ctx, &columns, key_accessor);
  }

  if (grn_obj_get_range(ctx, table) != GRN_ID_NIL) {
    grn_obj *value_accessor;
    value_accessor = grn_obj_column(ctx,
                                    table,
                                    GRN_COLUMN_NAME_VALUE,
                                    GRN_COLUMN_NAME_VALUE_LEN);
    GRN_PTR_PUT(ctx, &columns, value_accessor);
  }

  {
    grn_hash *real_columns;

    real_columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                   GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
    grn_table_columns(ctx, table, NULL, 0, (grn_obj *)real_columns);
    GRN_HASH_EACH_BEGIN(ctx, real_columns, cursor, id) {
      void *key;
      grn_id column_id;
      grn_obj *column;

      if (dumper->is_close_opened_object_mode) {
        grn_ctx_push_temporary_open_space(ctx);
      }

      grn_hash_cursor_get_key(ctx, cursor, &key);
      column_id = *((grn_id *)key);

      column = grn_ctx_at(ctx, column_id);
      if (column) {
        if (grn_obj_is_index_column(ctx, column)) {
          have_index_column = GRN_TRUE;
          if (dumper->is_close_opened_object_mode) {
            grn_ctx_pop_temporary_open_space(ctx);
          }
        } else {
          have_data_column = GRN_TRUE;
          GRN_PTR_PUT(ctx, &columns, column);
          if (dumper->is_close_opened_object_mode) {
            grn_ctx_merge_temporary_open_space(ctx);
          }
        }
      } else {
        GRN_PLUGIN_CLEAR_ERROR(ctx);
        if (dumper->is_close_opened_object_mode) {
          grn_ctx_pop_temporary_open_space(ctx);
        }
      }
    } GRN_HASH_EACH_END(ctx, cursor);
    grn_hash_close(ctx, real_columns);
  }

  n_columns = GRN_BULK_VSIZE(&columns) / sizeof(grn_obj *);

  if (have_index_column && !have_data_column) {
    goto exit;
  }

  if (GRN_TEXT_LEN(dumper->output) > 0) {
    GRN_TEXT_PUTC(ctx, dumper->output, '\n');
  }

  GRN_TEXT_PUTS(ctx, dumper->output, "load --table ");
  dump_obj_name(ctx, dumper, table);
  GRN_TEXT_PUTS(ctx, dumper->output, "\n[\n");

  GRN_TEXT_PUTC(ctx, dumper->output, '[');
  for (i = 0; i < n_columns; i++) {
    grn_obj *column;
    grn_obj *column_name = &(dumper->column_name_buffer);

    column = GRN_PTR_VALUE_AT(&columns, i);
    if (i) { GRN_TEXT_PUTC(ctx, dumper->output, ','); }
    GRN_BULK_REWIND(column_name);
    grn_column_name_(ctx, column, column_name);
    grn_text_otoj(ctx, dumper->output, column_name, NULL);
  }
  GRN_TEXT_PUTS(ctx, dumper->output, "],\n");

  if (table->header.type == GRN_TABLE_HASH_KEY && dumper->is_sort_hash_table) {
    grn_obj *sorted;
    grn_table_sort_key sort_keys[1];
    uint32_t n_sort_keys = 1;
    grn_bool is_first_record = GRN_TRUE;

    sort_keys[0].key = grn_obj_column(ctx, table,
                                      GRN_COLUMN_NAME_KEY,
                                      GRN_COLUMN_NAME_KEY_LEN);
    sort_keys[0].flags = GRN_TABLE_SORT_ASC;
    sort_keys[0].offset = 0;
    sorted = grn_table_create(ctx,
                              NULL, 0, NULL,
                              GRN_TABLE_NO_KEY,
                              NULL,
                              table);
    grn_table_sort(ctx,
                   table, 0, -1,
                   sorted,
                   sort_keys, n_sort_keys);
    cursor = grn_table_cursor_open(ctx,
                                   sorted,
                                   NULL, 0, NULL, 0,
                                   0, -1,
                                   0);
    while (grn_table_cursor_next(ctx, cursor) != GRN_ID_NIL) {
      void *value_raw;
      grn_id id;

      grn_table_cursor_get_value(ctx, cursor, &value_raw);
      id = *((grn_id *)value_raw);

      if (is_first_record) {
        is_first_record = GRN_FALSE;
      } else {
        GRN_TEXT_PUTS(ctx, dumper->output, ",\n");
      }
      dump_record(ctx, dumper, table, id, &columns, n_columns);
    }
    GRN_TEXT_PUTS(ctx, dumper->output, "\n]\n");
    grn_obj_close(ctx, sorted);
    grn_obj_unlink(ctx, sort_keys[0].key);
  } else {
    grn_obj delete_commands;
    grn_id old_id = GRN_ID_NIL;
    grn_id id;

    GRN_TEXT_INIT(&delete_commands, 0);
    cursor = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1,
                                   GRN_CURSOR_BY_KEY);
    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      if (old_id != GRN_ID_NIL) { GRN_TEXT_PUTS(ctx, dumper->output, ",\n"); }
      if (table->header.type == GRN_TABLE_NO_KEY && old_id + 1 < id) {
        grn_id current_id;
        for (current_id = old_id + 1; current_id < id; current_id++) {
          GRN_TEXT_PUTS(ctx, dumper->output, "[],\n");
          GRN_TEXT_PUTS(ctx, &delete_commands, "delete --table ");
          dump_obj_name_raw(ctx, &delete_commands, table);
          GRN_TEXT_PUTS(ctx, &delete_commands, " --id ");
          grn_text_lltoa(ctx, &delete_commands, current_id);
          GRN_TEXT_PUTC(ctx, &delete_commands, '\n');
        }
      }
      dump_record(ctx, dumper, table, id, &columns, n_columns);

      old_id = id;
    }
    grn_table_cursor_close(ctx, cursor);
    GRN_TEXT_PUTS(ctx, dumper->output, "\n]\n");
    GRN_TEXT_PUT(ctx, dumper->output,
                 GRN_TEXT_VALUE(&delete_commands),
                 GRN_TEXT_LEN(&delete_commands));
    GRN_OBJ_FIN(ctx, &delete_commands);
  }
exit :
  for (i = 0; i < n_columns; i++) {
    grn_obj *column;

    column = GRN_PTR_VALUE_AT(&columns, i);
    if (column->header.type == GRN_ACCESSOR) {
      grn_obj_close(ctx, column);
    }
  }
  GRN_OBJ_FIN(ctx, &columns);

  if (dumper->is_close_opened_object_mode) {
    grn_ctx_pop_temporary_open_space(ctx);
  }
}

static void
dump_table(grn_ctx *ctx, grn_dumper *dumper, grn_obj *table)
{
  grn_obj *domain = NULL;
  grn_id range_id;
  grn_obj *range = NULL;
  grn_table_flags flags;
  grn_table_flags default_flags = GRN_OBJ_PERSISTENT;
  grn_obj *default_tokenizer;
  grn_obj *normalizer;
  grn_obj *token_filters;

  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
    domain = grn_ctx_at(ctx, table->header.domain);
    break;
  default:
    break;
  }

  if (GRN_TEXT_LEN(dumper->output) > 0) {
    GRN_TEXT_PUTC(ctx, dumper->output, '\n');
    grn_ctx_output_flush(ctx, 0);
  }

  grn_table_get_info(ctx, table,
                     &flags,
                     NULL,
                     &default_tokenizer,
                     &normalizer,
                     &token_filters);

  GRN_TEXT_PUTS(ctx, dumper->output, "table_create ");
  dump_obj_name(ctx, dumper, table);
  GRN_TEXT_PUTC(ctx, dumper->output, ' ');
  grn_dump_table_create_flags(ctx,
                              flags & ~default_flags,
                              dumper->output);
  if (domain) {
    GRN_TEXT_PUTC(ctx, dumper->output, ' ');
    dump_obj_name(ctx, dumper, domain);
  }
  range_id = grn_obj_get_range(ctx, table);
  if (range_id != GRN_ID_NIL) {
    range = grn_ctx_at(ctx, range_id);
    if (!range) {
      // ERR(GRN_RANGE_ERROR, "couldn't get table's value_type object");
      return;
    }
    if (table->header.type != GRN_TABLE_NO_KEY) {
      GRN_TEXT_PUTC(ctx, dumper->output, ' ');
    } else {
      GRN_TEXT_PUTS(ctx, dumper->output, " --value_type ");
    }
    dump_obj_name(ctx, dumper, range);
    grn_obj_unlink(ctx, range);
  }
  if (default_tokenizer) {
    GRN_TEXT_PUTS(ctx, dumper->output, " --default_tokenizer ");
    dump_obj_name(ctx, dumper, default_tokenizer);
  }
  if (normalizer) {
    GRN_TEXT_PUTS(ctx, dumper->output, " --normalizer ");
    dump_obj_name(ctx, dumper, normalizer);
  }
  if (table->header.type != GRN_TABLE_NO_KEY) {
    int n_token_filters;

    n_token_filters = GRN_BULK_VSIZE(token_filters) / sizeof(grn_obj *);
    if (n_token_filters > 0) {
      int i;
      GRN_TEXT_PUTS(ctx, dumper->output, " --token_filters ");
      for (i = 0; i < n_token_filters; i++) {
        grn_obj *token_filter = GRN_PTR_VALUE_AT(token_filters, i);
        if (i > 0) {
          GRN_TEXT_PUTC(ctx, dumper->output, ',');
        }
        dump_obj_name(ctx, dumper, token_filter);
      }
    }
  }

  GRN_TEXT_PUTC(ctx, dumper->output, '\n');

  dump_columns(ctx, dumper, table, GRN_TRUE, GRN_FALSE, GRN_FALSE);
}

static void
dump_schema(grn_ctx *ctx, grn_dumper *dumper)
{
  GRN_DB_EACH_BEGIN_BY_KEY(ctx, cursor, id) {
    void *name;
    int name_size;
    grn_obj *object;

    if (grn_id_is_builtin(ctx, id)) {
      continue;
    }

    name_size = grn_table_cursor_get_key(ctx, cursor, &name);
    if (grn_obj_name_is_column(ctx, name, name_size)) {
      continue;
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    if ((object = grn_ctx_at(ctx, id))) {
      switch (object->header.type) {
      case GRN_TABLE_HASH_KEY:
      case GRN_TABLE_PAT_KEY:
      case GRN_TABLE_DAT_KEY:
      case GRN_TABLE_NO_KEY:
        dump_table(ctx, dumper, object);
        break;
      default:
        break;
      }
    } else {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      GRN_PLUGIN_CLEAR_ERROR(ctx);
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  } GRN_DB_EACH_END(ctx, cursor);

  if (!dumper->have_reference_column) {
    return;
  }

  GRN_TEXT_PUTC(ctx, dumper->output, '\n');
  grn_ctx_output_flush(ctx, 0);

  GRN_DB_EACH_BEGIN_BY_KEY(ctx, cursor, id) {
    void *name;
    int name_size;
    grn_obj *object;

    if (grn_id_is_builtin(ctx, id)) {
      continue;
    }

    name_size = grn_table_cursor_get_key(ctx, cursor, &name);
    if (grn_obj_name_is_column(ctx, name, name_size)) {
      continue;
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    if ((object = grn_ctx_at(ctx, id))) {
      switch (object->header.type) {
      case GRN_TABLE_HASH_KEY:
      case GRN_TABLE_PAT_KEY:
      case GRN_TABLE_DAT_KEY:
      case GRN_TABLE_NO_KEY:
        dump_columns(ctx, dumper, object, GRN_FALSE, GRN_TRUE, GRN_FALSE);
        break;
      default:
        break;
      }
    } else {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      GRN_PLUGIN_CLEAR_ERROR(ctx);
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  } GRN_DB_EACH_END(ctx, cursor);
}

static void
dump_selected_tables_records(grn_ctx *ctx, grn_dumper *dumper, grn_obj *tables)
{
  const char *p, *e;

  p = GRN_TEXT_VALUE(tables);
  e = p + GRN_TEXT_LEN(tables);
  while (p < e) {
    int len;
    grn_obj *table;
    const char *token, *token_e;

    if ((len = grn_isspace(p, ctx->encoding))) {
      p += len;
      continue;
    }

    token = p;
    if (!(('a' <= *p && *p <= 'z') ||
          ('A' <= *p && *p <= 'Z') ||
          (*p == '_'))) {
      while (p < e && !grn_isspace(p, ctx->encoding)) {
        p++;
      }
      GRN_LOG(ctx, GRN_LOG_WARNING, "invalid table name is ignored: <%.*s>\n",
              (int)(p - token), token);
      continue;
    }
    while (p < e &&
           (('a' <= *p && *p <= 'z') ||
            ('A' <= *p && *p <= 'Z') ||
            ('0' <= *p && *p <= '9') ||
            (*p == '_'))) {
      p++;
    }
    token_e = p;
    while (p < e && (len = grn_isspace(p, ctx->encoding))) {
      p += len;
      continue;
    }
    if (p < e && *p == ',') {
      p++;
    }

    table = grn_ctx_get(ctx, token, token_e - token);
    if (!table) {
      GRN_LOG(ctx, GRN_LOG_WARNING,
              "nonexistent table name is ignored: <%.*s>\n",
              (int)(token_e - token), token);
      continue;
    }

    if (grn_obj_is_table(ctx, table)) {
      dump_records(ctx, dumper, table);
    }
    grn_obj_unlink(ctx, table);
  }
}

static void
dump_all_records(grn_ctx *ctx, grn_dumper *dumper)
{
  GRN_DB_EACH_BEGIN_BY_KEY(ctx, cursor, id) {
    void *name;
    int name_size;
    grn_obj *table;

    if (grn_id_is_builtin(ctx, id)) {
      continue;
    }

    name_size = grn_table_cursor_get_key(ctx, cursor, &name);
    if (grn_obj_name_is_column(ctx, name, name_size)) {
      continue;
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    table = grn_ctx_at(ctx, id);
    if (!table) {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      GRN_PLUGIN_CLEAR_ERROR(ctx);
      goto next_loop;
    }

    if (grn_obj_is_table(ctx, table)) {
      dump_records(ctx, dumper, table);
    }

  next_loop :
    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  } GRN_DB_EACH_END(ctx, cursor);
}

static void
dump_indexes(grn_ctx *ctx, grn_dumper *dumper)
{
  if (!dumper->have_index_column) {
    return;
  }

  if (GRN_TEXT_LEN(dumper->output) > 0) {
    GRN_TEXT_PUTC(ctx, dumper->output, '\n');
  }

  GRN_DB_EACH_BEGIN_BY_KEY(ctx, cursor, id) {
    void *name;
    int name_size;
    grn_obj *object;

    if (grn_id_is_builtin(ctx, id)) {
      continue;
    }

    name_size = grn_table_cursor_get_key(ctx, cursor, &name);
    if (grn_obj_name_is_column(ctx, name, name_size)) {
      continue;
    }

    if (dumper->is_close_opened_object_mode) {
      grn_ctx_push_temporary_open_space(ctx);
    }

    object = grn_ctx_at(ctx, id);
    if (!object) {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      GRN_PLUGIN_CLEAR_ERROR(ctx);
      goto next_loop;
    }

    if (grn_obj_is_table(ctx, object)) {
      dump_columns(ctx, dumper, object, GRN_FALSE, GRN_FALSE, GRN_TRUE);
    }

  next_loop :
    if (dumper->is_close_opened_object_mode) {
      grn_ctx_pop_temporary_open_space(ctx);
    }
  } GRN_DB_EACH_END(ctx, cursor);
}

static grn_obj *
command_dump(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_dumper dumper;
  grn_obj *tables;
  grn_bool is_dump_plugins;
  grn_bool is_dump_schema;
  grn_bool is_dump_records;
  grn_bool is_dump_indexes;
  grn_bool is_dump_configs;

  dumper.output = ctx->impl->output.buf;
  if (grn_thread_get_limit() == 1) {
    dumper.is_close_opened_object_mode = GRN_TRUE;
  } else {
    dumper.is_close_opened_object_mode = GRN_FALSE;
  }
  dumper.have_reference_column = GRN_FALSE;
  dumper.have_index_column = GRN_FALSE;

  tables = grn_plugin_proc_get_var(ctx, user_data, "tables", -1);
  is_dump_plugins = grn_plugin_proc_get_var_bool(ctx, user_data,
                                                 "dump_plugins", -1,
                                                 GRN_TRUE);
  is_dump_schema = grn_plugin_proc_get_var_bool(ctx, user_data,
                                                "dump_schema", -1,
                                                GRN_TRUE);
  is_dump_records = grn_plugin_proc_get_var_bool(ctx, user_data,
                                                 "dump_records", -1,
                                                 GRN_TRUE);
  is_dump_indexes = grn_plugin_proc_get_var_bool(ctx, user_data,
                                                 "dump_indexes", -1,
                                                 GRN_TRUE);
  is_dump_configs = grn_plugin_proc_get_var_bool(ctx, user_data,
                                                 "dump_configs", -1,
                                                 GRN_TRUE);
  dumper.is_sort_hash_table =
    grn_plugin_proc_get_var_bool(ctx, user_data,
                                 "sort_hash_table", -1,
                                 GRN_FALSE);
  GRN_TEXT_INIT(&(dumper.column_name_buffer), 0);

  grn_ctx_set_output_type(ctx, GRN_CONTENT_GROONGA_COMMAND_LIST);

  dumper_collect_statistics(ctx, &dumper);

  if (is_dump_configs) {
    dump_configs(ctx, &dumper);
  }
  if (is_dump_plugins) {
    dump_plugins(ctx, &dumper);
  }
  if (is_dump_schema) {
    dump_schema(ctx, &dumper);
  }
  if (is_dump_records) {
    /* To update index columns correctly, we first create the whole schema, then
       load non-derivative records, while skipping records of index columns. That
       way, Groonga will silently do the job of updating index columns for us. */
    if (GRN_TEXT_LEN(tables) > 0) {
      dump_selected_tables_records(ctx, &dumper, tables);
    } else {
      dump_all_records(ctx, &dumper);
    }
  }
  if (is_dump_indexes) {
    dump_indexes(ctx, &dumper);
  }
  /* remove the last newline because another one will be added by the caller.
     maybe, the caller of proc functions currently doesn't consider the
     possibility of multiple-line output from proc functions. */
  if (GRN_BULK_VSIZE(dumper.output) > 0) {
    grn_bulk_truncate(ctx, dumper.output, GRN_BULK_VSIZE(dumper.output) - 1);
  }

  GRN_OBJ_FIN(ctx, &(dumper.column_name_buffer));

  return NULL;
}

void
grn_proc_init_dump(grn_ctx *ctx)
{
  grn_expr_var vars[7];

  grn_plugin_expr_var_init(ctx, &(vars[0]), "tables", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "dump_plugins", -1);
  grn_plugin_expr_var_init(ctx, &(vars[2]), "dump_schema", -1);
  grn_plugin_expr_var_init(ctx, &(vars[3]), "dump_records", -1);
  grn_plugin_expr_var_init(ctx, &(vars[4]), "dump_indexes", -1);
  grn_plugin_expr_var_init(ctx, &(vars[5]), "dump_configs", -1);
  grn_plugin_expr_var_init(ctx, &(vars[6]), "sort_hash_table", -1);
  grn_plugin_command_create(ctx,
                            "dump", -1,
                            command_dump,
                            sizeof(vars) / sizeof(vars[0]),
                            vars);
}
