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
#include "../grn_raw_string.h"
#include "../grn_expr.h"
#include "../grn_str.h"
#include "../grn_output.h"
#include "../grn_util.h"
#include "../grn_cache.h"
#include "../grn_ii.h"

#include "../grn_ts.h"

#include <groonga/plugin.h>

#define GRN_SELECT_INTERNAL_VAR_MATCH_COLUMNS "$match_columns"

#define DEFAULT_DRILLDOWN_LIMIT           10
#define DEFAULT_DRILLDOWN_OUTPUT_COLUMNS  "_key, _nsubrecs"

typedef enum {
  GRN_COLUMN_STAGE_INITIAL,
  GRN_COLUMN_STAGE_FILTERED,
  GRN_COLUMN_STAGE_OUTPUT
} grn_column_stage;

typedef struct {
  grn_raw_string label;
  grn_column_stage stage;
  grn_obj *type;
  grn_obj_flags flags;
  grn_raw_string value;
  struct {
    grn_raw_string sort_keys;
    grn_raw_string group_keys;
  } window;
} grn_column_data;

typedef struct {
  grn_hash *initial;
  grn_hash *filtered;
  grn_hash *output;
} grn_columns;

typedef struct {
  grn_raw_string match_columns;
  grn_raw_string query;
  grn_raw_string query_expander;
  grn_raw_string query_flags;
  grn_raw_string filter;
  struct {
    grn_obj *match_columns;
    grn_obj *expression;
  } condition;
  grn_obj *filtered;
} grn_filter_data;

typedef struct {
  grn_raw_string label;
  grn_filter_data filter;
  grn_raw_string sort_keys;
  grn_raw_string output_columns;
  int offset;
  int limit;
  grn_obj *table;
} grn_slice_data;

typedef struct {
  grn_raw_string label;
  grn_raw_string keys;
  grn_table_sort_key *parsed_keys;
  int n_parsed_keys;
  grn_raw_string sort_keys;
  grn_raw_string output_columns;
  int offset;
  int limit;
  grn_table_group_flags calc_types;
  grn_raw_string calc_target_name;
  grn_raw_string filter;
  grn_raw_string table_name;
  grn_columns columns;
  grn_table_group_result result;
  grn_obj *filtered_result;
} grn_drilldown_data;

typedef struct _grn_select_output_formatter grn_select_output_formatter;

typedef struct {
  /* inputs */
  grn_raw_string table;
  grn_filter_data filter;
  grn_raw_string scorer;
  grn_raw_string sort_keys;
  grn_raw_string output_columns;
  int offset;
  int limit;
  grn_hash *slices;
  grn_drilldown_data drilldown;
  grn_hash *drilldowns;
  grn_raw_string cache;
  grn_raw_string match_escalation_threshold;
  grn_raw_string adjuster;
  grn_columns columns;

  /* for processing */
  struct {
    grn_obj *target;
    grn_obj *initial;
    grn_obj *result;
    grn_obj *sorted;
    grn_obj *output;
  } tables;
  uint16_t cacheable;
  uint16_t taintable;
  struct {
    int n_elements;
    grn_select_output_formatter *formatter;
  } output;
} grn_select_data;

typedef void grn_select_output_slices_label_func(grn_ctx *ctx,
                                                 grn_select_data *data);
typedef void grn_select_output_slices_open_func(grn_ctx *ctx,
                                                grn_select_data *data,
                                                unsigned int n_result_sets);
typedef void grn_select_output_slices_close_func(grn_ctx *ctx,
                                                 grn_select_data *data);
typedef void grn_select_output_slice_label_func(grn_ctx *ctx,
                                                grn_select_data *data,
                                                grn_slice_data *slice);
typedef void grn_select_output_drilldowns_label_func(grn_ctx *ctx,
                                                     grn_select_data *data);
typedef void grn_select_output_drilldowns_open_func(grn_ctx *ctx,
                                                    grn_select_data *data,
                                                    unsigned int n_result_sets);
typedef void grn_select_output_drilldowns_close_func(grn_ctx *ctx,
                                                     grn_select_data *data);
typedef void grn_select_output_drilldown_label_func(grn_ctx *ctx,
                                                    grn_select_data *data,
                                                    grn_drilldown_data *drilldown);

struct _grn_select_output_formatter {
  grn_select_output_slices_label_func      *slices_label;
  grn_select_output_slices_open_func       *slices_open;
  grn_select_output_slices_close_func      *slices_close;
  grn_select_output_slice_label_func       *slice_label;
  grn_select_output_drilldowns_label_func  *drilldowns_label;
  grn_select_output_drilldowns_open_func   *drilldowns_open;
  grn_select_output_drilldowns_close_func  *drilldowns_close;
  grn_select_output_drilldown_label_func   *drilldown_label;
};

grn_rc
grn_proc_syntax_expand_query(grn_ctx *ctx,
                             const char *query,
                             unsigned int query_len,
                             grn_expr_flags flags,
                             const char *query_expander_name,
                             unsigned int query_expander_name_len,
                             const char *term_column_name,
                             unsigned int term_column_name_len,
                             const char *expanded_term_column_name,
                             unsigned int expanded_term_column_name_len,
                             grn_obj *expanded_query,
                             const char *error_message_tag)
{
  grn_obj *query_expander;

  query_expander = grn_ctx_get(ctx,
                               query_expander_name,
                               query_expander_name_len);
  if (!query_expander) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "%s nonexistent query expander: <%.*s>",
                     error_message_tag,
                     (int)query_expander_name_len,
                     query_expander_name);
    return ctx->rc;
  }

  if (expanded_term_column_name_len == 0) {
    return grn_expr_syntax_expand_query(ctx, query, query_len, flags,
                                        query_expander, expanded_query);
  }

  if (!grn_obj_is_table(ctx, query_expander)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, query_expander);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "%s query expander with expanded term column "
                     "must be table: <%.*s>",
                     error_message_tag,
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    return ctx->rc;
  }

  {
    grn_obj *term_column = NULL;
    grn_obj *expanded_term_column;

    expanded_term_column = grn_obj_column(ctx,
                                          query_expander,
                                          expanded_term_column_name,
                                          expanded_term_column_name_len);
    if (!expanded_term_column) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, query_expander);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "%s nonexistent expanded term column: <%.*s>: "
                       "query expander: <%.*s>",
                       error_message_tag,
                       (int)expanded_term_column_name_len,
                       expanded_term_column_name,
                       (int)GRN_TEXT_LEN(&inspected),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return ctx->rc;
    }

    if (term_column_name_len > 0) {
      term_column = grn_obj_column(ctx,
                                   query_expander,
                                   term_column_name,
                                   term_column_name_len);
      if (!term_column) {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, query_expander);
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "%s nonexistent term column: <%.*s>: "
                         "query expander: <%.*s>",
                         error_message_tag,
                         (int)term_column_name_len,
                         term_column_name,
                         (int)GRN_TEXT_LEN(&inspected),
                         GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        if (grn_obj_is_accessor(ctx, expanded_term_column)) {
          grn_obj_unlink(ctx, expanded_term_column);
        }
        return ctx->rc;
      }
    }

    grn_expr_syntax_expand_query_by_table(ctx,
                                          query, query_len,
                                          flags,
                                          term_column,
                                          expanded_term_column,
                                          expanded_query);
    if (grn_obj_is_accessor(ctx, term_column)) {
      grn_obj_unlink(ctx, term_column);
    }
    if (grn_obj_is_accessor(ctx, expanded_term_column)) {
      grn_obj_unlink(ctx, expanded_term_column);
    }
    return ctx->rc;
  }
}

static grn_table_group_flags
grn_parse_table_group_calc_types(grn_ctx *ctx,
                                 const char *calc_types,
                                 unsigned int calc_types_len)
{
  grn_table_group_flags flags = 0;
  const char *calc_types_end = calc_types + calc_types_len;

  while (calc_types < calc_types_end) {
    if (*calc_types == ',' || *calc_types == ' ') {
      calc_types += 1;
      continue;
    }

#define CHECK_TABLE_GROUP_CALC_TYPE(name)\
    if (((unsigned long) (calc_types_end - calc_types) >= (unsigned long) (sizeof(#name) - 1)) && \
      (!memcmp(calc_types, #name, sizeof(#name) - 1))) {\
    flags |= GRN_TABLE_GROUP_CALC_ ## name;\
    calc_types += sizeof(#name) - 1;\
    continue;\
  }

    CHECK_TABLE_GROUP_CALC_TYPE(COUNT);
    CHECK_TABLE_GROUP_CALC_TYPE(MAX);
    CHECK_TABLE_GROUP_CALC_TYPE(MIN);
    CHECK_TABLE_GROUP_CALC_TYPE(SUM);
    CHECK_TABLE_GROUP_CALC_TYPE(AVG);

#define GRN_TABLE_GROUP_CALC_NONE 0
    CHECK_TABLE_GROUP_CALC_TYPE(NONE);
#undef GRN_TABLE_GROUP_CALC_NONE

    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "invalid table group calc type: <%.*s>",
                     (int)(calc_types_end - calc_types),
                     calc_types);
    return 0;
#undef CHECK_TABLE_GROUP_CALC_TYPE
  }

  return flags;
}

static const char *
grn_column_stage_name(grn_column_stage stage)
{
  switch (stage) {
  case GRN_COLUMN_STAGE_INITIAL :
    return "initial";
  case GRN_COLUMN_STAGE_FILTERED :
    return "filtered";
  case GRN_COLUMN_STAGE_OUTPUT :
    return "output";
  default :
    return "unknown";
  }
}

static grn_bool
grn_column_data_init(grn_ctx *ctx,
                     const char *label,
                     size_t label_len,
                     grn_column_stage stage,
                     grn_hash **columns)
{
  void *column_raw;
  grn_column_data *column;
  int added;

  if (!*columns) {
    *columns = grn_hash_create(ctx,
                               NULL,
                               GRN_TABLE_MAX_KEY_SIZE,
                               sizeof(grn_column_data),
                               GRN_OBJ_TABLE_HASH_KEY |
                               GRN_OBJ_KEY_VAR_SIZE |
                               GRN_HASH_TINY);
  }
  if (!*columns) {
    return GRN_FALSE;
  }

  grn_hash_add(ctx,
               *columns,
               label,
               label_len,
               &column_raw,
               &added);
  if (!added) {
    return GRN_TRUE;
  }

  column = column_raw;
  column->label.value = label;
  column->label.length = label_len;
  column->stage = stage;
  column->type = grn_ctx_at(ctx, GRN_DB_TEXT);
  column->flags = GRN_OBJ_COLUMN_SCALAR;
  GRN_RAW_STRING_INIT(column->value);
  GRN_RAW_STRING_INIT(column->window.sort_keys);
  GRN_RAW_STRING_INIT(column->window.group_keys);

  return GRN_TRUE;
}

static grn_bool
grn_column_data_fill(grn_ctx *ctx,
                     grn_column_data *column,
                     grn_obj *type_raw,
                     grn_obj *flags,
                     grn_obj *value,
                     grn_obj *window_sort_keys,
                     grn_obj *window_group_keys)
{
  if (type_raw && GRN_TEXT_LEN(type_raw) > 0) {
    grn_obj *type;

    type = grn_ctx_get(ctx, GRN_TEXT_VALUE(type_raw), GRN_TEXT_LEN(type_raw));
    if (!type) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][columns][%s][%.*s] unknown type: <%.*s>",
                       grn_column_stage_name(column->stage),
                       (int)(column->label.length),
                       column->label.value,
                       (int)(GRN_TEXT_LEN(type_raw)),
                       GRN_TEXT_VALUE(type_raw));
      return GRN_FALSE;
    }
    if (!(grn_obj_is_type(ctx, type) || grn_obj_is_table(ctx, type))) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, type);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][columns][%s][%.*s] invalid type: %.*s",
                       grn_column_stage_name(column->stage),
                       (int)(column->label.length),
                       column->label.value,
                       (int)(GRN_TEXT_LEN(&inspected)),
                       GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      grn_obj_unlink(ctx, type);
      return GRN_FALSE;
    }
    column->type = type;
  }

  if (flags && GRN_TEXT_LEN(flags) > 0) {
    char error_message_tag[GRN_TABLE_MAX_KEY_SIZE];

    grn_snprintf(error_message_tag,
                 GRN_TABLE_MAX_KEY_SIZE,
                 GRN_TABLE_MAX_KEY_SIZE,
                 "[select][columns][%s][%.*s]",
                 grn_column_stage_name(column->stage),
                 (int)(column->label.length),
                 column->label.value);
    column->flags =
      grn_proc_column_parse_flags(ctx,
                                  error_message_tag,
                                  GRN_TEXT_VALUE(flags),
                                  GRN_TEXT_VALUE(flags) + GRN_TEXT_LEN(flags));
    if (ctx->rc != GRN_SUCCESS) {
      return GRN_FALSE;
    }
  }

  GRN_RAW_STRING_FILL(column->value, value);
  GRN_RAW_STRING_FILL(column->window.sort_keys, window_sort_keys);
  GRN_RAW_STRING_FILL(column->window.group_keys, window_group_keys);

  return GRN_TRUE;
}

static grn_bool
grn_column_data_collect(grn_ctx *ctx,
                        grn_user_data *user_data,
                        grn_hash *columns,
                        const char *prefix_label,
                        size_t prefix_label_len)
{
  grn_hash_cursor *cursor = NULL;

  cursor = grn_hash_cursor_open(ctx, columns,
                                NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    return GRN_FALSE;
  }

  while (grn_hash_cursor_next(ctx, cursor)) {
    grn_column_data *column;
    char key_name[GRN_TABLE_MAX_KEY_SIZE];
    grn_obj *type = NULL;
    grn_obj *flags = NULL;
    grn_obj *value = NULL;
    struct {
      grn_obj *sort_keys;
      grn_obj *group_keys;
    } window;

    window.sort_keys = NULL;
    window.group_keys = NULL;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&column);

#define GET_VAR_RAW(parameter_key, name)                                \
    if (!name) {                                                        \
      grn_snprintf(key_name,                                            \
                   GRN_TABLE_MAX_KEY_SIZE,                              \
                   GRN_TABLE_MAX_KEY_SIZE,                              \
                   "%.*s%s[%.*s]." # name,                              \
                   (int)prefix_label_len,                               \
                   prefix_label,                                        \
                   parameter_key,                                       \
                   (int)(column->label.length),                         \
                   column->label.value);                                \
      name = grn_plugin_proc_get_var(ctx, user_data, key_name, -1);     \
    }

#define GET_VAR(name) do {                      \
      GET_VAR_RAW("columns", name);             \
      /* For backward compatibility */          \
      GET_VAR_RAW("column", name);              \
    } while (GRN_FALSE)

    GET_VAR(type);
    GET_VAR(flags);
    GET_VAR(value);
    GET_VAR(window.sort_keys);
    GET_VAR(window.group_keys);

#undef GET_VAR

#undef GET_VAR_RAW

    grn_column_data_fill(ctx, column,
                         type, flags, value,
                         window.sort_keys,
                         window.group_keys);
  }
  grn_hash_cursor_close(ctx, cursor);
  return GRN_TRUE;
}

static void
grn_columns_init(grn_ctx *ctx, grn_columns *columns)
{
  columns->initial = NULL;
  columns->filtered = NULL;
  columns->output = NULL;
}

static void
grn_columns_fin(grn_ctx *ctx, grn_columns *columns)
{
  if (columns->initial) {
    grn_hash_close(ctx, columns->initial);
  }

  if (columns->filtered) {
    grn_hash_close(ctx, columns->filtered);
  }

  if (columns->output) {
    grn_hash_close(ctx, columns->output);
  }
}

static grn_bool
grn_columns_collect(grn_ctx *ctx,
                    grn_user_data *user_data,
                    grn_columns *columns,
                    const char *prefix,
                    const char *base_prefix,
                    size_t base_prefix_len)
{
  grn_obj *vars;
  grn_table_cursor *cursor;
  size_t prefix_len;
  const char *suffix = "].stage";
  size_t suffix_len;

  vars = grn_plugin_proc_get_vars(ctx, user_data);
  cursor = grn_table_cursor_open(ctx, vars, NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    return GRN_FALSE;
  }

  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  while (grn_table_cursor_next(ctx, cursor)) {
    void *key;
    char *variable_name;
    unsigned int variable_name_len;
    char *column_name;
    size_t column_name_len;
    void *value_raw;
    grn_obj *value;
    grn_column_stage stage;
    grn_hash **target_columns;

    variable_name_len = grn_table_cursor_get_key(ctx, cursor, &key);
    variable_name = key;
    if (variable_name_len < base_prefix_len + prefix_len + suffix_len + 1) {
      continue;
    }

    if (base_prefix_len > 0) {
      if (memcmp(base_prefix, variable_name, base_prefix_len) != 0) {
        continue;
      }
    }

    if (memcmp(prefix, variable_name + base_prefix_len, prefix_len) != 0) {
      continue;
    }

    if (memcmp(suffix,
               variable_name + (variable_name_len - suffix_len),
               suffix_len) != 0) {
      continue;
    }

    grn_table_cursor_get_value(ctx, cursor, &value_raw);
    value = value_raw;
    if (GRN_TEXT_EQUAL_CSTRING(value, "initial")) {
      stage = GRN_COLUMN_STAGE_INITIAL;
      target_columns = &(columns->initial);
    } else if (GRN_TEXT_EQUAL_CSTRING(value, "filtered")) {
      stage = GRN_COLUMN_STAGE_FILTERED;
      target_columns = &(columns->filtered);
    } else if (GRN_TEXT_EQUAL_CSTRING(value, "output")) {
      stage = GRN_COLUMN_STAGE_OUTPUT;
      target_columns = &(columns->output);
    } else {
      continue;
    }

    column_name = variable_name + base_prefix_len + prefix_len;
    column_name_len =
      variable_name_len - base_prefix_len - prefix_len - suffix_len;
    if (!grn_column_data_init(ctx,
                              column_name,
                              column_name_len,
                              stage,
                              target_columns)) {
      grn_table_cursor_close(ctx, cursor);
      return GRN_FALSE;
    }
  }
  grn_table_cursor_close(ctx, cursor);

  return GRN_TRUE;
}

static grn_bool
grn_columns_fill(grn_ctx *ctx,
                 grn_user_data *user_data,
                 grn_columns *columns,
                 const char *prefix,
                 size_t prefix_length)
{
  if (!grn_columns_collect(ctx, user_data, columns,
                           "columns[", prefix, prefix_length)) {
    return GRN_FALSE;
  }

  /* For backward compatibility */
  if (!grn_columns_collect(ctx, user_data, columns,
                           "column[", prefix, prefix_length)) {
    return GRN_FALSE;
  }

  if (columns->initial) {
    if (!grn_column_data_collect(ctx,
                                 user_data,
                                 columns->initial,
                                 prefix,
                                 prefix_length)) {
      return GRN_FALSE;
    }
  }

  if (columns->filtered) {
    if (!grn_column_data_collect(ctx,
                                 user_data,
                                 columns->filtered,
                                 prefix,
                                 prefix_length)) {
      return GRN_FALSE;
    }
  }

  if (columns->output) {
    if (!grn_column_data_collect(ctx,
                                 user_data,
                                 columns->output,
                                 prefix,
                                 prefix_length)) {
      return GRN_FALSE;
    }
  }

  return GRN_TRUE;
}

static void
grn_filter_data_init(grn_ctx *ctx, grn_filter_data *data)
{
  GRN_RAW_STRING_INIT(data->match_columns);
  GRN_RAW_STRING_INIT(data->query);
  GRN_RAW_STRING_INIT(data->query_expander);
  GRN_RAW_STRING_INIT(data->query_flags);
  GRN_RAW_STRING_INIT(data->filter);
  data->condition.match_columns = NULL;
  data->condition.expression = NULL;
  data->filtered = NULL;
}

static void
grn_filter_data_fin(grn_ctx *ctx, grn_filter_data *data)
{
  if (data->filtered) {
    grn_obj_unlink(ctx, data->filtered);
  }
  if (data->condition.expression) {
    grn_obj_close(ctx, data->condition.expression);
  }
  if (data->condition.match_columns) {
    grn_obj_close(ctx, data->condition.match_columns);
  }
}

static void
grn_filter_data_fill(grn_ctx *ctx,
                     grn_filter_data *data,
                     grn_obj *match_columns,
                     grn_obj *query,
                     grn_obj *query_expander,
                     grn_obj *query_flags,
                     grn_obj *filter)
{
  GRN_RAW_STRING_FILL(data->match_columns, match_columns);
  GRN_RAW_STRING_FILL(data->query, query);
  GRN_RAW_STRING_FILL(data->query_expander, query_expander);
  GRN_RAW_STRING_FILL(data->query_flags, query_flags);
  GRN_RAW_STRING_FILL(data->filter, filter);
}

static grn_bool
grn_filter_data_execute(grn_ctx *ctx,
                        grn_filter_data *data,
                        grn_obj *table,
                        const char *tag)
{
  grn_obj *variable;

  if (data->query.length == 0 && data->filter.length == 0) {
    return GRN_TRUE;
  }

  GRN_EXPR_CREATE_FOR_QUERY(ctx,
                            table,
                            data->condition.expression,
                            variable);
  if (!data->condition.expression) {
    grn_rc rc = ctx->rc;
    if (rc == GRN_SUCCESS) {
      rc = GRN_NO_MEMORY_AVAILABLE;
    }
    GRN_PLUGIN_ERROR(ctx,
                     rc,
                     "%s[condition] "
                     "failed to create expression for condition: %s",
                     tag,
                     ctx->errbuf);
    return GRN_FALSE;
  }

  if (data->query.length > 0) {
    if (data->match_columns.length > 0) {
      GRN_EXPR_CREATE_FOR_QUERY(ctx,
                                table,
                                data->condition.match_columns,
                                variable);
      if (!data->condition.match_columns) {
        grn_rc rc = ctx->rc;
        if (rc == GRN_SUCCESS) {
          rc = GRN_NO_MEMORY_AVAILABLE;
        }
        GRN_PLUGIN_ERROR(ctx,
                         rc,
                         "%s[match_columns] "
                         "failed to create expression for match columns: "
                         "<%.*s>: %s",
                         tag,
                         (int)(data->match_columns.length),
                         data->match_columns.value,
                         ctx->errbuf);
        return GRN_FALSE;
      }

      grn_expr_parse(ctx,
                     data->condition.match_columns,
                     data->match_columns.value,
                     data->match_columns.length,
                     NULL, GRN_OP_MATCH, GRN_OP_AND,
                     GRN_EXPR_SYNTAX_SCRIPT);
      if (ctx->rc != GRN_SUCCESS) {
        return GRN_FALSE;
      }
    }

    {
      grn_expr_flags flags;
      grn_obj query_expander_buf;
      const char *query = data->query.value;
      unsigned int query_len = data->query.length;

      flags = GRN_EXPR_SYNTAX_QUERY;
      if (data->query_flags.length) {
        flags |= grn_proc_expr_query_flags_parse(ctx,
                                                 data->query_flags.value,
                                                 data->query_flags.length,
                                                 tag);
        if (ctx->rc != GRN_SUCCESS) {
          return GRN_FALSE;
        }
      } else {
        flags |= GRN_EXPR_ALLOW_PRAGMA|GRN_EXPR_ALLOW_COLUMN;
      }

      GRN_TEXT_INIT(&query_expander_buf, 0);
      if (data->query_expander.length > 0) {
        grn_rc rc;
        rc = grn_proc_syntax_expand_query(ctx,
                                          data->query.value,
                                          data->query.length,
                                          flags,
                                          data->query_expander.value,
                                          data->query_expander.length,
                                          NULL, 0,
                                          NULL, 0,
                                          &query_expander_buf,
                                          tag);
        if (rc == GRN_SUCCESS) {
          query = GRN_TEXT_VALUE(&query_expander_buf);
          query_len = GRN_TEXT_LEN(&query_expander_buf);
        } else {
          GRN_OBJ_FIN(ctx, &query_expander_buf);
          return GRN_FALSE;
        }
      }

      grn_expr_parse(ctx,
                     data->condition.expression,
                     query,
                     query_len,
                     data->condition.match_columns,
                     GRN_OP_MATCH,
                     GRN_OP_AND,
                     flags);
      GRN_OBJ_FIN(ctx, &query_expander_buf);

      if (ctx->rc != GRN_SUCCESS) {
        return GRN_FALSE;
      }
    }
  }

  if (data->filter.length > 0) {
    grn_expr_parse(ctx,
                   data->condition.expression,
                   data->filter.value,
                   data->filter.length,
                   data->condition.match_columns,
                   GRN_OP_MATCH,
                   GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      return GRN_FALSE;
    }

    if (data->query.length > 0) {
      grn_expr_append_op(ctx, data->condition.expression, GRN_OP_AND, 2);
    }

    if (ctx->rc != GRN_SUCCESS) {
      return GRN_FALSE;
    }
  }

  data->filtered = grn_table_select(ctx,
                                    table,
                                    data->condition.expression,
                                    NULL,
                                    GRN_OP_OR);

  return ctx->rc == GRN_SUCCESS;
}

static void
grn_slice_data_init(grn_ctx *ctx,
                    grn_slice_data *slice,
                    const char *label,
                    size_t label_len)
{
  slice->label.value = label;
  slice->label.length = label_len;
  grn_filter_data_init(ctx, &(slice->filter));
  GRN_RAW_STRING_INIT(slice->sort_keys);
  GRN_RAW_STRING_INIT(slice->output_columns);
  slice->offset = 0;
  slice->limit = GRN_SELECT_DEFAULT_LIMIT;
  slice->table = NULL;
}

static void
grn_slice_data_fin(grn_ctx *ctx, grn_slice_data *slice)
{
  grn_filter_data_fin(ctx, &(slice->filter));
}

static void
grn_slice_data_fill(grn_ctx *ctx,
                    grn_slice_data *slice,
                    grn_obj *match_columns,
                    grn_obj *query,
                    grn_obj *query_expander,
                    grn_obj *query_flags,
                    grn_obj *filter,
                    grn_obj *sort_keys,
                    grn_obj *output_columns,
                    grn_obj *offset,
                    grn_obj *limit)
{
  grn_filter_data_fill(ctx,
                       &(slice->filter),
                       match_columns,
                       query,
                       query_expander,
                       query_flags,
                       filter);

  GRN_RAW_STRING_FILL(slice->sort_keys, sort_keys);

  GRN_RAW_STRING_FILL(slice->output_columns, output_columns);
  if (slice->output_columns.length == 0) {
    slice->output_columns.value = GRN_SELECT_DEFAULT_OUTPUT_COLUMNS;
    slice->output_columns.length = strlen(GRN_SELECT_DEFAULT_OUTPUT_COLUMNS);
  }

  slice->offset = grn_proc_option_value_int32(ctx, offset, 0);
  slice->limit = grn_proc_option_value_int32(ctx,
                                             limit,
                                             GRN_SELECT_DEFAULT_LIMIT);
}

static void
grn_drilldown_data_init(grn_ctx *ctx,
                        grn_drilldown_data *drilldown,
                        const char *label,
                        size_t label_len)
{
  drilldown->label.value = label;
  drilldown->label.length = label_len;
  GRN_RAW_STRING_INIT(drilldown->keys);
  drilldown->parsed_keys = NULL;
  drilldown->n_parsed_keys = 0;
  GRN_RAW_STRING_INIT(drilldown->sort_keys);
  GRN_RAW_STRING_INIT(drilldown->output_columns);
  drilldown->offset = 0;
  drilldown->limit = DEFAULT_DRILLDOWN_LIMIT;
  drilldown->calc_types = 0;
  GRN_RAW_STRING_INIT(drilldown->calc_target_name);
  GRN_RAW_STRING_INIT(drilldown->filter);
  GRN_RAW_STRING_INIT(drilldown->table_name);
  grn_columns_init(ctx, &(drilldown->columns));
  drilldown->result.table = NULL;
  drilldown->filtered_result = NULL;
}

static void
grn_drilldown_data_fin(grn_ctx *ctx, grn_drilldown_data *drilldown)
{
  grn_table_group_result *result;

  grn_columns_fin(ctx, &(drilldown->columns));

  if (drilldown->filtered_result) {
    grn_obj_close(ctx, drilldown->filtered_result);
  }

  result = &(drilldown->result);
  if (result->table) {
    if (result->calc_target) {
      grn_obj_unlink(ctx, result->calc_target);
    }
    if (result->table) {
      grn_obj_close(ctx, result->table);
    }
  }
}

static void
grn_drilldown_data_fill(grn_ctx *ctx,
                        grn_drilldown_data *drilldown,
                        grn_obj *keys,
                        grn_obj *sort_keys,
                        grn_obj *output_columns,
                        grn_obj *offset,
                        grn_obj *limit,
                        grn_obj *calc_types,
                        grn_obj *calc_target,
                        grn_obj *filter,
                        grn_obj *table)
{
  GRN_RAW_STRING_FILL(drilldown->keys, keys);

  GRN_RAW_STRING_FILL(drilldown->sort_keys, sort_keys);

  GRN_RAW_STRING_FILL(drilldown->output_columns, output_columns);
  if (drilldown->output_columns.length == 0) {
    drilldown->output_columns.value = DEFAULT_DRILLDOWN_OUTPUT_COLUMNS;
    drilldown->output_columns.length = strlen(DEFAULT_DRILLDOWN_OUTPUT_COLUMNS);
  }

  if (offset && GRN_TEXT_LEN(offset)) {
    drilldown->offset =
      grn_atoi(GRN_TEXT_VALUE(offset), GRN_BULK_CURR(offset), NULL);
  } else {
    drilldown->offset = 0;
  }

  if (limit && GRN_TEXT_LEN(limit)) {
    drilldown->limit =
      grn_atoi(GRN_TEXT_VALUE(limit), GRN_BULK_CURR(limit), NULL);
  } else {
    drilldown->limit = DEFAULT_DRILLDOWN_LIMIT;
  }

  if (calc_types && GRN_TEXT_LEN(calc_types)) {
    drilldown->calc_types =
      grn_parse_table_group_calc_types(ctx,
                                       GRN_TEXT_VALUE(calc_types),
                                       GRN_TEXT_LEN(calc_types));
  } else {
    drilldown->calc_types = 0;
  }

  GRN_RAW_STRING_FILL(drilldown->calc_target_name, calc_target);

  GRN_RAW_STRING_FILL(drilldown->filter, filter);

  GRN_RAW_STRING_FILL(drilldown->table_name, table);
}

grn_expr_flags
grn_proc_expr_query_flags_parse(grn_ctx *ctx,
                                const char *query_flags,
                                size_t query_flags_size,
                                const char *error_message_tag)
{
  grn_expr_flags flags = 0;
  const char *query_flags_end = query_flags + query_flags_size;

  while (query_flags < query_flags_end) {
    if (*query_flags == '|' || *query_flags == ' ') {
      query_flags += 1;
      continue;
    }

#define CHECK_EXPR_FLAG(name)                                           \
    if (((unsigned long) (query_flags_end - query_flags) >= (unsigned long) (sizeof(#name) - 1)) &&     \
        (memcmp(query_flags, #name, sizeof(#name) - 1) == 0) &&         \
        (((query_flags_end - query_flags) == (sizeof(#name) - 1)) ||    \
         (query_flags[sizeof(#name) - 1] == '|') ||                     \
         (query_flags[sizeof(#name) - 1] == ' '))) {                    \
      flags |= GRN_EXPR_ ## name;                                       \
      query_flags += sizeof(#name) - 1;                                 \
      continue;                                                         \
    }

    CHECK_EXPR_FLAG(ALLOW_PRAGMA);
    CHECK_EXPR_FLAG(ALLOW_COLUMN);
    CHECK_EXPR_FLAG(ALLOW_UPDATE);
    CHECK_EXPR_FLAG(ALLOW_LEADING_NOT);
    CHECK_EXPR_FLAG(QUERY_NO_SYNTAX_ERROR);

#define GRN_EXPR_NONE 0
    CHECK_EXPR_FLAG(NONE);
#undef GNR_EXPR_NONE

    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "%s invalid query flag: <%.*s>",
                     error_message_tag,
                     (int)(query_flags_end - query_flags),
                     query_flags);
    return 0;
#undef CHECK_EXPR_FLAG
  }

  return flags;
}

static void
grn_select_expression_set_condition(grn_ctx *ctx,
                                    grn_obj *expression,
                                    grn_obj *condition)
{
  grn_obj *condition_ptr;

  if (!expression) {
    return;
  }

  condition_ptr =
    grn_expr_get_or_add_var(ctx, expression,
                            GRN_SELECT_INTERNAL_VAR_CONDITION,
                            GRN_SELECT_INTERNAL_VAR_CONDITION_LEN);
  GRN_PTR_INIT(condition_ptr, 0, GRN_DB_OBJECT);
  GRN_PTR_SET(ctx, condition_ptr, condition);
}

grn_bool
grn_proc_select_format_init(grn_ctx *ctx,
                            grn_obj_format *format,
                            grn_obj *result_set,
                            int n_hits,
                            int offset,
                            int limit,
                            const char *columns,
                            int columns_len,
                            grn_obj *condition)
{
  grn_rc rc;

  GRN_OBJ_FORMAT_INIT(format, n_hits, offset, limit, offset);
  format->flags =
    GRN_OBJ_FORMAT_WITH_COLUMN_NAMES|
    GRN_OBJ_FORMAT_XML_ELEMENT_RESULTSET;
  rc = grn_output_format_set_columns(ctx,
                                     format,
                                     result_set,
                                     columns,
                                     columns_len);
  if (rc != GRN_SUCCESS) {
    GRN_OBJ_FORMAT_FIN(ctx, format);
    return GRN_FALSE;
  }

  grn_select_expression_set_condition(ctx, format->expression, condition);

  return ctx->rc == GRN_SUCCESS;
}

grn_bool
grn_proc_select_format_fin(grn_ctx *ctx, grn_obj_format *format)
{
  GRN_OBJ_FORMAT_FIN(ctx, format);

  return ctx->rc == GRN_SUCCESS;
}

grn_bool
grn_proc_select_output_columns_open(grn_ctx *ctx,
                                    grn_obj_format *format,
                                    grn_obj *res,
                                    int n_hits,
                                    int offset,
                                    int limit,
                                    const char *columns,
                                    int columns_len,
                                    grn_obj *condition,
                                    uint32_t n_additional_elements)
{
  grn_bool succeeded;

  if (!grn_proc_select_format_init(ctx,
                                   format,
                                   res,
                                   n_hits,
                                   offset,
                                   limit,
                                   columns,
                                   columns_len,
                                   condition)) {
    return GRN_FALSE;
  }

  GRN_OUTPUT_RESULT_SET_OPEN(res, format, n_additional_elements);
  succeeded = (ctx->rc == GRN_SUCCESS);
  if (!succeeded) {
    GRN_OUTPUT_RESULT_SET_CLOSE(res, format);
  }

  return succeeded;
}

grn_bool
grn_proc_select_output_columns_close(grn_ctx *ctx,
                                     grn_obj_format *format,
                                     grn_obj *result_set)
{
  GRN_OUTPUT_RESULT_SET_CLOSE(result_set, format);

  return grn_proc_select_format_fin(ctx, format);
}

grn_bool
grn_proc_select_output_columns(grn_ctx *ctx,
                               grn_obj *res,
                               int n_hits,
                               int offset,
                               int limit,
                               const char *columns,
                               int columns_len,
                               grn_obj *condition)
{
  grn_obj_format format;
  uint32_t n_additional_elements = 0;

  if (!grn_proc_select_output_columns_open(ctx,
                                           &format,
                                           res,
                                           n_hits,
                                           offset,
                                           limit,
                                           columns,
                                           columns_len,
                                           condition,
                                           n_additional_elements)) {
    return GRN_FALSE;
  }

  return grn_proc_select_output_columns_close(ctx, &format, res);
}

static grn_obj *
grn_select_create_all_selected_result_table(grn_ctx *ctx,
                                            grn_obj *table)
{
  grn_obj *result;
  grn_posting posting;

  result = grn_table_create(ctx, NULL, 0, NULL,
                            GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                            table, NULL);
  if (!result) {
    return NULL;
  }

  memset(&posting, 0, sizeof(grn_posting));
  GRN_TABLE_EACH_BEGIN(ctx, table, cursor, id) {
    posting.rid = id;
    grn_ii_posting_add(ctx,
                       &posting,
                       (grn_hash *)(result),
                       GRN_OP_OR);
  } GRN_TABLE_EACH_END(ctx, cursor);

  return result;
}

static grn_obj *
grn_select_create_no_sort_keys_sorted_table(grn_ctx *ctx,
                                            grn_select_data *data,
                                            grn_obj *table)
{
  grn_obj *sorted;
  grn_table_cursor *cursor;

  sorted = grn_table_create(ctx, NULL, 0, NULL,
                            GRN_OBJ_TABLE_NO_KEY,
                            NULL,
                            table);

  if (!sorted) {
    return NULL;
  }

  cursor = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0,
                                 data->offset,
                                 data->limit,
                                 GRN_CURSOR_ASCENDING);
  if (cursor) {
    grn_id id;
    while ((id = grn_table_cursor_next(ctx, cursor))) {
      grn_id *value;
      if (grn_array_add(ctx, (grn_array *)sorted, (void **)&value)) {
        *value = id;
      }
    }
    grn_table_cursor_close(ctx, cursor);
  }

  return sorted;
}


static void
grn_select_apply_columns(grn_ctx *ctx,
                         grn_select_data *data,
                         grn_obj *table,
                         grn_hash *columns)
{
  grn_hash_cursor *columns_cursor;

  columns_cursor = grn_hash_cursor_open(ctx, columns,
                                        NULL, 0, NULL, 0, 0, -1, 0);
  if (!columns_cursor) {
    return;
  }

  while (grn_hash_cursor_next(ctx, columns_cursor) != GRN_ID_NIL) {
    grn_column_data *column_data;
    grn_obj *column;
    grn_obj *expression;
    grn_obj *record;

    grn_hash_cursor_get_value(ctx, columns_cursor, (void **)&column_data);

    column = grn_column_create(ctx,
                               table,
                               column_data->label.value,
                               column_data->label.length,
                               NULL,
                               column_data->flags,
                               column_data->type);
    if (!column) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][column][%s][%.*s] failed to create column: %s",
                       grn_column_stage_name(column_data->stage),
                       (int)(column_data->label.length),
                       column_data->label.value,
                       ctx->errbuf);
      break;
    }

    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, expression, record);
    if (!expression) {
      grn_obj_close(ctx, column);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][column][%s][%.*s] "
                       "failed to create expression to compute value: %s",
                       grn_column_stage_name(column_data->stage),
                       (int)(column_data->label.length),
                       column_data->label.value,
                       ctx->errbuf);
      break;
    }
    grn_expr_parse(ctx,
                   expression,
                   column_data->value.value,
                   column_data->value.length,
                   NULL,
                   GRN_OP_MATCH,
                   GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      grn_obj_close(ctx, expression);
      grn_obj_close(ctx, column);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][column][%s][%.*s] "
                       "failed to parse value: <%.*s>: %s",
                       grn_column_stage_name(column_data->stage),
                       (int)(column_data->label.length),
                       column_data->label.value,
                       (int)(column_data->value.length),
                       column_data->value.value,
                       ctx->errbuf);
      break;
    }
    grn_select_expression_set_condition(ctx,
                                        expression,
                                        data->filter.condition.expression);

    if (column_data->window.sort_keys.length > 0 ||
        column_data->window.group_keys.length > 0) {
      grn_window_definition definition;
      grn_rc rc;

      if (column_data->window.sort_keys.length > 0) {
        int n_sort_keys;
        definition.sort_keys =
          grn_table_sort_key_from_str(ctx,
                                      column_data->window.sort_keys.value,
                                      column_data->window.sort_keys.length,
                                      table, &n_sort_keys);
        definition.n_sort_keys = n_sort_keys;
        if (!definition.sort_keys) {
          grn_obj_close(ctx, expression);
          grn_obj_close(ctx, column);
          GRN_PLUGIN_ERROR(ctx,
                           GRN_INVALID_ARGUMENT,
                           "[select][column][%s][%.*s] "
                           "failed to parse sort keys: %s",
                           grn_column_stage_name(column_data->stage),
                           (int)(column_data->label.length),
                           column_data->label.value,
                           ctx->errbuf);
          break;
        }
      } else {
        definition.sort_keys = NULL;
        definition.n_sort_keys = 0;
      }

      if (column_data->window.group_keys.length > 0) {
        int n_group_keys;
        definition.group_keys =
          grn_table_sort_key_from_str(ctx,
                                      column_data->window.group_keys.value,
                                      column_data->window.group_keys.length,
                                      table, &n_group_keys);
        definition.n_group_keys = n_group_keys;
        if (!definition.group_keys) {
          grn_obj_close(ctx, expression);
          grn_obj_close(ctx, column);
          if (definition.sort_keys) {
            grn_table_sort_key_close(ctx,
                                     definition.sort_keys,
                                     definition.n_sort_keys);
          }
          GRN_PLUGIN_ERROR(ctx,
                           GRN_INVALID_ARGUMENT,
                           "[select][column][%s][%.*s] "
                           "failed to parse group keys: %s",
                           grn_column_stage_name(column_data->stage),
                           (int)(column_data->label.length),
                           column_data->label.value,
                           ctx->errbuf);
          break;
        }
      } else {
        definition.group_keys = NULL;
        definition.n_group_keys = 0;
      }

      rc = grn_table_apply_window_function(ctx,
                                           table,
                                           column,
                                           &definition,
                                           expression);
      if (definition.sort_keys) {
        grn_table_sort_key_close(ctx,
                                 definition.sort_keys,
                                 definition.n_sort_keys);
      }
      if (definition.group_keys) {
        grn_table_sort_key_close(ctx,
                                 definition.group_keys,
                                 definition.n_group_keys);
      }
      if (rc != GRN_SUCCESS) {
        grn_obj_close(ctx, expression);
        grn_obj_close(ctx, column);
        break;
      }
    } else {
      grn_rc rc;
      rc = grn_table_apply_expr(ctx, table, column, expression);
      if (rc != GRN_SUCCESS) {
        grn_obj_close(ctx, expression);
        grn_obj_close(ctx, column);
        GRN_PLUGIN_ERROR(ctx,
                         GRN_INVALID_ARGUMENT,
                         "[select][column][%s][%.*s] "
                         "failed to apply expression to generate column values: "
                         "%s",
                         grn_column_stage_name(column_data->stage),
                         (int)(column_data->label.length),
                         column_data->label.value,
                         ctx->errbuf);
        break;
      }
    }

    grn_obj_close(ctx, expression);

    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                  ":", "columns[%.*s](%d)",
                  (int)(column_data->label.length),
                  column_data->label.value,
                  grn_table_size(ctx, table));
  }

  grn_hash_cursor_close(ctx, columns_cursor);
}

static grn_bool
grn_select_apply_initial_columns(grn_ctx *ctx,
                                 grn_select_data *data)
{
  if (!data->columns.initial) {
    return GRN_TRUE;
  }

  data->tables.initial =
    grn_select_create_all_selected_result_table(ctx, data->tables.target);
  if (!data->tables.initial) {
    return GRN_FALSE;
  }

  grn_select_apply_columns(ctx,
                           data,
                           data->tables.initial,
                           data->columns.initial);

  return ctx->rc == GRN_SUCCESS;
}

static grn_bool
grn_select_filter(grn_ctx *ctx,
                  grn_select_data *data)
{
  if (!grn_filter_data_execute(ctx,
                               &(data->filter),
                               data->tables.initial,
                               "[select]")) {
    return GRN_FALSE;
  }

  data->tables.result = data->filter.filtered;
  if (!data->tables.result) {
    data->tables.result = data->tables.initial;
  }

  {
    grn_expr *expression;
    expression = (grn_expr *)(data->filter.condition.expression);
    if (expression) {
      data->cacheable *= expression->cacheable;
      data->taintable += expression->taintable;
    }
  }

  return GRN_TRUE;
}

static grn_bool
grn_select_apply_filtered_columns(grn_ctx *ctx,
                                  grn_select_data *data)
{
  if (!data->columns.filtered) {
    return GRN_TRUE;
  }

  if (data->tables.result == data->tables.initial) {
    data->tables.result =
      grn_select_create_all_selected_result_table(ctx, data->tables.initial);
    if (!data->tables.result) {
      return GRN_FALSE;
    }
  }

  grn_select_apply_columns(ctx,
                           data,
                           data->tables.result,
                           data->columns.filtered);

  return ctx->rc == GRN_SUCCESS;
}

static int
grn_select_apply_adjuster_execute_ensure_factor(grn_ctx *ctx,
                                                grn_obj *factor_object)
{
  if (!factor_object) {
    return 1;
  } else if (factor_object->header.domain == GRN_DB_INT32) {
    return GRN_INT32_VALUE(factor_object);
  } else {
    grn_rc rc;
    grn_obj int32_object;
    int factor;
    GRN_INT32_INIT(&int32_object, 0);
    rc = grn_obj_cast(ctx, factor_object, &int32_object, GRN_FALSE);
    if (rc == GRN_SUCCESS) {
      factor = GRN_INT32_VALUE(&int32_object);
    } else {
      /* TODO: Log or return error? */
      factor = 1;
    }
    GRN_OBJ_FIN(ctx, &int32_object);
    return factor;
  }
}

static void
grn_select_apply_adjuster_execute_adjust(grn_ctx *ctx,
                                         grn_obj *table,
                                         grn_obj *column,
                                         grn_obj *value,
                                         grn_obj *factor)
{
  grn_obj *index;
  unsigned int n_indexes;
  int factor_value;

  n_indexes = grn_column_index(ctx, column, GRN_OP_MATCH, &index, 1, NULL);
  if (n_indexes == 0) {
    char column_name[GRN_TABLE_MAX_KEY_SIZE];
    int column_name_size;
    column_name_size = grn_obj_name(ctx, column,
                                    column_name, GRN_TABLE_MAX_KEY_SIZE);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "adjuster requires index column for the target column: "
                     "<%.*s>",
                     column_name_size,
                     column_name);
    return;
  }

  factor_value = grn_select_apply_adjuster_execute_ensure_factor(ctx, factor);

  {
    grn_search_optarg options;
    memset(&options, 0, sizeof(grn_search_optarg));

    options.mode = GRN_OP_EXACT;
    options.similarity_threshold = 0;
    options.max_interval = 0;
    options.weight_vector = NULL;
    options.vector_size = factor_value;
    options.proc = NULL;
    options.max_size = 0;
    options.scorer = NULL;

    grn_obj_search(ctx, index, value, table, GRN_OP_ADJUST, &options);
  }
}

static void
grn_select_apply_adjuster_execute(grn_ctx *ctx,
                                  grn_obj *table,
                                  grn_obj *adjuster)
{
  grn_expr *expr = (grn_expr *)adjuster;
  grn_expr_code *code, *code_end;

  code = expr->codes;
  code_end = expr->codes + expr->codes_curr;
  while (code < code_end) {
    grn_obj *column, *value, *factor;

    if (code->op == GRN_OP_PLUS) {
      code++;
      continue;
    }

    column = code->value;
    code++;
    value = code->value;
    code++;
    code++; /* op == GRN_OP_MATCH */
    if ((code_end - code) >= 2 && code[1].op == GRN_OP_STAR) {
      factor = code->value;
      code++;
      code++; /* op == GRN_OP_STAR */
    } else {
      factor = NULL;
    }
    grn_select_apply_adjuster_execute_adjust(ctx, table, column, value, factor);
  }
}

static grn_bool
grn_select_apply_adjuster(grn_ctx *ctx,
                          grn_select_data *data)
{
  grn_obj *adjuster;
  grn_obj *record;
  grn_rc rc;

  if (data->adjuster.length == 0) {
    return GRN_TRUE;
  }

  GRN_EXPR_CREATE_FOR_QUERY(ctx, data->tables.target, adjuster, record);
  if (!adjuster) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[select][adjuster] "
                     "failed to create expression: %s",
                     ctx->errbuf);
    return GRN_FALSE;
  }

  rc = grn_expr_parse(ctx, adjuster,
                      data->adjuster.value,
                      data->adjuster.length,
                      NULL,
                      GRN_OP_MATCH, GRN_OP_ADJUST,
                      GRN_EXPR_SYNTAX_ADJUSTER);
  if (rc != GRN_SUCCESS) {
    grn_obj_unlink(ctx, adjuster);
    GRN_PLUGIN_ERROR(ctx,
                     rc,
                     "[select][adjuster] "
                     "failed to parse: %s",
                     ctx->errbuf);
    return GRN_FALSE;
  }

  data->cacheable *= ((grn_expr *)adjuster)->cacheable;
  data->taintable += ((grn_expr *)adjuster)->taintable;
  grn_select_apply_adjuster_execute(ctx, data->tables.result, adjuster);
  grn_obj_unlink(ctx, adjuster);

  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                ":", "adjust(%d)", grn_table_size(ctx, data->tables.result));

  return GRN_TRUE;
}

static grn_bool
grn_select_apply_scorer(grn_ctx *ctx,
                        grn_select_data *data)
{
  grn_obj *scorer;
  grn_obj *record;
  grn_rc rc = GRN_SUCCESS;

  if (data->scorer.length == 0) {
    return GRN_TRUE;
  }

  GRN_EXPR_CREATE_FOR_QUERY(ctx, data->tables.result, scorer, record);
  if (!scorer) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[select][scorer] "
                     "failed to create expression: %s",
                     ctx->errbuf);
    return GRN_FALSE;
  }

  rc = grn_expr_parse(ctx,
                      scorer,
                      data->scorer.value,
                      data->scorer.length,
                      NULL,
                      GRN_OP_MATCH,
                      GRN_OP_AND,
                      GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
  if (rc != GRN_SUCCESS) {
    grn_obj_unlink(ctx, scorer);
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[select][scorer] "
                     "failed to parse: %s",
                     ctx->errbuf);
    return GRN_FALSE;
  }

  data->cacheable *= ((grn_expr *)scorer)->cacheable;
  data->taintable += ((grn_expr *)scorer)->taintable;
  GRN_TABLE_EACH_BEGIN(ctx, data->tables.result, cursor, id) {
    GRN_RECORD_SET(ctx, record, id);
    grn_expr_exec(ctx, scorer, 0);
    if (ctx->rc) {
      rc = ctx->rc;
      GRN_PLUGIN_ERROR(ctx,
                       rc,
                       "[select][scorer] "
                       "failed to execute: <%.*s>: %s",
                       (int)(data->scorer.length),
                       data->scorer.value,
                       ctx->errbuf);
      break;
    }
  } GRN_TABLE_EACH_END(ctx, cursor);
  grn_obj_unlink(ctx, scorer);

  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                ":", "score(%d)", grn_table_size(ctx, data->tables.result));

  return rc == GRN_SUCCESS;
}

static grn_bool
grn_select_sort(grn_ctx *ctx,
                grn_select_data *data)
{
  grn_table_sort_key *keys;
  uint32_t n_keys;

  if (data->sort_keys.length == 0) {
    return GRN_TRUE;
  }

  keys = grn_table_sort_key_from_str(ctx,
                                     data->sort_keys.value,
                                     data->sort_keys.length,
                                     data->tables.result,
                                     &n_keys);
  if (!keys) {
    if (ctx->rc == GRN_SUCCESS) {
      return GRN_TRUE;
    } else {
      GRN_PLUGIN_ERROR(ctx,
                       ctx->rc,
                       "[select][sort] "
                       "failed to parse: <%.*s>: %s",
                       (int)(data->sort_keys.length),
                       data->sort_keys.value,
                       ctx->errbuf);
      return GRN_FALSE;
    }
  }

  data->tables.sorted = grn_table_create(ctx, NULL, 0, NULL,
                                         GRN_OBJ_TABLE_NO_KEY,
                                         NULL,
                                         data->tables.result);
  if (!data->tables.sorted) {
    GRN_PLUGIN_ERROR(ctx,
                     ctx->rc,
                     "[select][sort] "
                     "failed to create table to store sorted record: "
                     "<%.*s>: %s",
                     (int)(data->sort_keys.length),
                     data->sort_keys.value,
                     ctx->errbuf);
    return GRN_FALSE;
  }

  grn_table_sort(ctx,
                 data->tables.result,
                 data->offset,
                 data->limit,
                 data->tables.sorted,
                 keys,
                 n_keys);

  grn_table_sort_key_close(ctx, keys, n_keys);

  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                ":", "sort(%d)", data->limit);

  return ctx->rc == GRN_SUCCESS;
}

static grn_bool
grn_select_apply_output_columns(grn_ctx *ctx,
                                grn_select_data *data)
{
  if (!data->columns.output) {
    return GRN_TRUE;
  }

  if (!data->tables.sorted) {
    data->tables.sorted =
      grn_select_create_no_sort_keys_sorted_table(ctx,
                                                  data,
                                                  data->tables.result);
    if (!data->tables.sorted) {
      return GRN_FALSE;
    }
  }

  grn_select_apply_columns(ctx,
                           data,
                           data->tables.sorted,
                           data->columns.output);

  return ctx->rc == GRN_SUCCESS;
}

static grn_bool
grn_select_output_match_open(grn_ctx *ctx,
                             grn_select_data *data,
                             grn_obj_format *format,
                             uint32_t n_additional_elements)
{
  grn_bool succeeded = GRN_TRUE;
  int offset;
  grn_obj *output_table;

  if (data->tables.sorted) {
    offset = 0;
    output_table = data->tables.sorted;
  } else {
    offset = data->offset;
    output_table = data->tables.result;
  }
  succeeded =
    grn_proc_select_output_columns_open(ctx,
                                        format,
                                        output_table,
                                        grn_table_size(ctx, data->tables.result),
                                        offset,
                                        data->limit,
                                        data->output_columns.value,
                                        data->output_columns.length,
                                        data->filter.condition.expression,
                                        n_additional_elements);
  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                ":", "output(%d)", data->limit);

  return succeeded;
}

static grn_bool
grn_select_output_match_close(grn_ctx *ctx,
                              grn_select_data *data,
                              grn_obj_format *format)
{
  grn_obj *output_table;

  if (data->tables.sorted) {
    output_table = data->tables.sorted;
  } else {
    output_table = data->tables.result;
  }

  return grn_proc_select_output_columns_close(ctx, format, output_table);
}

static grn_bool
grn_select_output_match(grn_ctx *ctx, grn_select_data *data)
{
  grn_obj_format format;
  uint32_t n_additional_elements = 0;

  if (!grn_select_output_match_open(ctx, data, &format, n_additional_elements)) {
    return GRN_FALSE;
  }

  return grn_select_output_match_close(ctx, data, &format);
}

static grn_bool
grn_select_slice_execute(grn_ctx *ctx,
                         grn_select_data *data,
                         grn_obj *table,
                         grn_slice_data *slice)
{
  char tag[GRN_TABLE_MAX_KEY_SIZE];
  grn_filter_data *filter;

  grn_snprintf(tag, GRN_TABLE_MAX_KEY_SIZE, GRN_TABLE_MAX_KEY_SIZE,
               "[select][slices][%.*s]",
               (int)(slice->label.length),
               slice->label.value);
  filter = &(slice->filter);
  if (filter->query.length == 0 && filter->filter.length == 0) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "%s slice requires query or filter",
                     tag);
    return GRN_FALSE;
  }

  if (!grn_filter_data_execute(ctx, filter, table, tag)) {
    return GRN_FALSE;
  }

  slice->table = filter->filtered;

  return GRN_TRUE;
}

static grn_bool
grn_select_slices_execute(grn_ctx *ctx,
                          grn_select_data *data,
                          grn_obj *table,
                          grn_hash *slices)
{
  grn_bool succeeded = GRN_TRUE;

  GRN_HASH_EACH_BEGIN(ctx, slices, cursor, id) {
    grn_slice_data *slice;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);
    if (!grn_select_slice_execute(ctx, data, table, slice)) {
      succeeded = GRN_FALSE;
      break;
    }
  } GRN_HASH_EACH_END(ctx, cursor);

  return succeeded;
}

static grn_bool
grn_select_prepare_slices(grn_ctx *ctx,
                          grn_select_data *data)
{
  if (!data->slices) {
    return GRN_TRUE;
  }

  if (!grn_select_slices_execute(ctx, data, data->tables.result, data->slices)) {
    return GRN_FALSE;
  }

  data->output.n_elements += 1;

  return GRN_TRUE;
}

static grn_bool
grn_select_output_slices(grn_ctx *ctx,
                         grn_select_data *data)
{
  grn_bool succeeded = GRN_TRUE;
  unsigned int n_available_results = 0;

  if (!data->slices) {
    return GRN_TRUE;
  }

  data->output.formatter->slices_label(ctx, data);

  GRN_HASH_EACH_BEGIN(ctx, data->slices, cursor, id) {
    grn_slice_data *slice;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);
    if (slice->table) {
      n_available_results++;
    }
  } GRN_HASH_EACH_END(ctx, cursor);

  data->output.formatter->slices_open(ctx, data, n_available_results);

  GRN_HASH_EACH_BEGIN(ctx, data->slices, cursor, id) {
    grn_slice_data *slice;
    uint32_t n_hits;
    int offset;
    int limit;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);
    if (!slice->table) {
      continue;
    }

    n_hits = grn_table_size(ctx, slice->table);

    offset = slice->offset;
    limit = slice->limit;
    grn_normalize_offset_and_limit(ctx, n_hits, &offset, &limit);

    if (slice->sort_keys.length > 0) {
      grn_table_sort_key *sort_keys;
      uint32_t n_sort_keys;
      sort_keys = grn_table_sort_key_from_str(ctx,
                                              slice->sort_keys.value,
                                              slice->sort_keys.length,
                                              slice->table, &n_sort_keys);
      if (sort_keys) {
        grn_obj *sorted;
        sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY,
                                  NULL, slice->table);
        if (sorted) {
          grn_table_sort(ctx, slice->table, offset, limit,
                         sorted, sort_keys, n_sort_keys);
          data->output.formatter->slice_label(ctx, data, slice);
          if (!grn_proc_select_output_columns(ctx,
                                              sorted,
                                              n_hits,
                                              0,
                                              limit,
                                              slice->output_columns.value,
                                              slice->output_columns.length,
                                              slice->filter.condition.expression)) {
            succeeded = GRN_FALSE;
          }
          grn_obj_unlink(ctx, sorted);
        }
        grn_table_sort_key_close(ctx, sort_keys, n_sort_keys);
      } else {
        succeeded = GRN_FALSE;
      }
    } else {
      data->output.formatter->slice_label(ctx, data, slice);
      if (!grn_proc_select_output_columns(ctx,
                                          slice->table,
                                          n_hits,
                                          offset,
                                          limit,
                                          slice->output_columns.value,
                                          slice->output_columns.length,
                                          slice->filter.condition.expression)) {
        succeeded = GRN_FALSE;
      }
    }

    if (!succeeded) {
      break;
    }

    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                  ":", "slice(%d)[%.*s]",
                  n_hits,
                  (int)(slice->label.length),
                  slice->label.value);
  } GRN_HASH_EACH_END(ctx, cursor);

  data->output.formatter->slices_close(ctx, data);

  return succeeded;
}

static grn_bool
grn_select_drilldown_execute(grn_ctx *ctx,
                             grn_select_data *data,
                             grn_obj *table,
                             grn_hash *drilldowns,
                             grn_id id)
{
  grn_table_sort_key *keys = NULL;
  unsigned int n_keys = 0;
  grn_obj *target_table = table;
  grn_drilldown_data *drilldown;
  grn_table_group_result *result;

  drilldown =
    (grn_drilldown_data *)grn_hash_get_value_(ctx, drilldowns, id, NULL);
  result = &(drilldown->result);

  result->limit = 1;
  result->flags = GRN_TABLE_GROUP_CALC_COUNT;
  result->op = 0;
  result->max_n_subrecs = 0;
  result->key_begin = 0;
  result->key_end = 0;
  if (result->calc_target) {
    grn_obj_unlink(ctx, result->calc_target);
  }
  result->calc_target = NULL;

  if (drilldown->table_name.length > 0) {
    grn_id dependent_id;
    dependent_id = grn_hash_get(ctx,
                                drilldowns,
                                drilldown->table_name.value,
                                drilldown->table_name.length,
                                NULL);
    if (dependent_id == GRN_ID_NIL) {
      if (data->slices) {
        grn_slice_data *slice;
        dependent_id = grn_hash_get(ctx,
                                    data->slices,
                                    drilldown->table_name.value,
                                    drilldown->table_name.length,
                                    NULL);
        if (dependent_id) {
          slice =
            (grn_slice_data *)grn_hash_get_value_(ctx, data->slices,
                                                  dependent_id, NULL);
          target_table = slice->table;
        }
      }
      if (dependent_id == GRN_ID_NIL) {
        GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                         "[select][drilldowns][%.*s][table] "
                         "nonexistent label: <%.*s>",
                         (int)(drilldown->label.length),
                         drilldown->label.value,
                         (int)(drilldown->table_name.length),
                         drilldown->table_name.value);
        return GRN_FALSE;
      }
    } else {
      grn_drilldown_data *dependent_drilldown;
      grn_table_group_result *dependent_result;

      dependent_drilldown =
        (grn_drilldown_data *)grn_hash_get_value_(ctx,
                                                  drilldowns,
                                                  dependent_id,
                                                  NULL);
      dependent_result = &(dependent_drilldown->result);
      target_table = dependent_result->table;
    }
  }

  if (drilldown->parsed_keys) {
    result->key_end = drilldown->n_parsed_keys;
  } else if (drilldown->keys.length > 0) {
    keys = grn_table_sort_key_from_str(ctx,
                                       drilldown->keys.value,
                                       drilldown->keys.length,
                                       target_table, &n_keys);
    if (!keys) {
      GRN_PLUGIN_CLEAR_ERROR(ctx);
      return GRN_FALSE;
    }

    result->key_end = n_keys - 1;
    if (n_keys > 1) {
      result->max_n_subrecs = 1;
    }
  }

  if (drilldown->calc_target_name.length > 0) {
    result->calc_target = grn_obj_column(ctx, target_table,
                                         drilldown->calc_target_name.value,
                                         drilldown->calc_target_name.length);
  }
  if (result->calc_target) {
    result->flags |= drilldown->calc_types;
  }

  if (drilldown->parsed_keys) {
    grn_table_group(ctx,
                    target_table,
                    drilldown->parsed_keys,
                    drilldown->n_parsed_keys,
                    result,
                    1);
  } else {
    grn_table_group(ctx, target_table, keys, n_keys, result, 1);
  }

  if (keys) {
    grn_table_sort_key_close(ctx, keys, n_keys);
  }

  if (!result->table) {
    return GRN_FALSE;
  }

  if (drilldown->columns.initial) {
    grn_select_apply_columns(ctx,
                             data,
                             result->table,
                             drilldown->columns.initial);
  }

  if (drilldown->filter.length > 0) {
    grn_obj *expression;
    grn_obj *record;
    GRN_EXPR_CREATE_FOR_QUERY(ctx, result->table, expression, record);
    if (!expression) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][drilldowns]%s%.*s%s[filter] "
                       "failed to create expression for filter: %s",
                       drilldown->label.length > 0 ? "[" : "",
                       (int)(drilldown->label.length),
                       drilldown->label.value,
                       drilldown->label.length > 0 ? "]" : "",
                       ctx->errbuf);
      return GRN_FALSE;
    }
    grn_expr_parse(ctx,
                   expression,
                   drilldown->filter.value,
                   drilldown->filter.length,
                   NULL,
                   GRN_OP_MATCH,
                   GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      grn_obj_close(ctx, expression);
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][drilldowns]%s%.*s%s[filter] "
                       "failed to parse filter: <%.*s>: %s",
                       drilldown->label.length > 0 ? "[" : "",
                       (int)(drilldown->label.length),
                       drilldown->label.value,
                       drilldown->label.length > 0 ? "]" : "",
                       (int)(drilldown->filter.length),
                       drilldown->filter.value,
                       ctx->errbuf);
      return GRN_FALSE;
    }
    drilldown->filtered_result = grn_table_select(ctx,
                                                  result->table,
                                                  expression,
                                                  NULL,
                                                  GRN_OP_OR);
    if (ctx->rc != GRN_SUCCESS) {
      grn_obj_close(ctx, expression);
      if (drilldown->filtered_result) {
        grn_obj_close(ctx, drilldown->filtered_result);
        drilldown->filtered_result = NULL;
      }
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][drilldowns]%s%.*s%s[filter] "
                       "failed to execute filter: <%.*s>: %s",
                       drilldown->label.length > 0 ? "[" : "",
                       (int)(drilldown->label.length),
                       drilldown->label.value,
                       drilldown->label.length > 0 ? "]" : "",
                       (int)(drilldown->filter.length),
                       drilldown->filter.value,
                       ctx->errbuf);
      return GRN_FALSE;
    }
    grn_obj_close(ctx, expression);
  }

  {
    unsigned int n_hits;

    if (drilldown->filtered_result) {
      n_hits = grn_table_size(ctx, drilldown->filtered_result);
    } else {
      n_hits = grn_table_size(ctx, result->table);
    }
    if (data->drilldown.keys.length == 0) {
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                    ":", "drilldowns[%.*s](%u)",
                    (int)(drilldown->label.length),
                    drilldown->label.value,
                    n_hits);
    } else {
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                    ":", "drilldown(%u)",
                    n_hits);
    }
  }

  return GRN_TRUE;
}

typedef enum {
  TSORT_STATUS_NOT_VISITED,
  TSORT_STATUS_VISITING,
  TSORT_STATUS_VISITED
} tsort_status;

static grn_bool
drilldown_tsort_visit(grn_ctx *ctx,
                      grn_hash *drilldowns,
                      tsort_status *statuses,
                      grn_obj *ids,
                      grn_id id)
{
  grn_bool cycled = GRN_TRUE;
  uint32_t index = id - 1;

  switch (statuses[index]) {
  case TSORT_STATUS_VISITING :
    cycled = GRN_TRUE;
    break;
  case TSORT_STATUS_VISITED :
    cycled = GRN_FALSE;
    break;
  case TSORT_STATUS_NOT_VISITED :
    cycled = GRN_FALSE;
    statuses[index] = TSORT_STATUS_VISITING;
    {
      grn_drilldown_data *drilldown;
      drilldown =
        (grn_drilldown_data *)grn_hash_get_value_(ctx, drilldowns, id, NULL);
      if (drilldown->table_name.length > 0) {
        grn_id dependent_id;
        dependent_id = grn_hash_get(ctx, drilldowns,
                                    drilldown->table_name.value,
                                    drilldown->table_name.length,
                                    NULL);
        if (dependent_id != GRN_ID_NIL) {
          cycled = drilldown_tsort_visit(ctx,
                                         drilldowns,
                                         statuses,
                                         ids,
                                         dependent_id);
          if (cycled) {
            GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                             "[select][drilldowns][%.*s][table] "
                             "cycled dependency: <%.*s>",
                             (int)(drilldown->label.length),
                             drilldown->label.value,
                             (int)(drilldown->table_name.length),
                             drilldown->table_name.value);
          }
        }
      }
    }
    if (!cycled) {
      statuses[index] = TSORT_STATUS_VISITED;
      GRN_RECORD_PUT(ctx, ids, id);
    }
    break;
  }

  return cycled;
}

static grn_bool
drilldown_tsort_body(grn_ctx *ctx,
                     grn_hash *drilldowns,
                     tsort_status *statuses,
                     grn_obj *ids)
{
  grn_bool succeeded = GRN_TRUE;

  GRN_HASH_EACH_BEGIN(ctx, drilldowns, cursor, id) {
    if (drilldown_tsort_visit(ctx, drilldowns, statuses, ids, id)) {
      succeeded = GRN_FALSE;
      break;
    }
  } GRN_HASH_EACH_END(ctx, cursor);

  return succeeded;
}

static void
drilldown_tsort_init(grn_ctx *ctx,
                     tsort_status *statuses,
                     size_t n_statuses)
{
  size_t i;
  for (i = 0; i < n_statuses; i++) {
    statuses[i] = TSORT_STATUS_NOT_VISITED;
  }
}

static grn_bool
drilldown_tsort(grn_ctx *ctx,
                grn_hash *drilldowns,
                grn_obj *ids)
{
  tsort_status *statuses;
  size_t n_statuses;
  grn_bool succeeded;

  n_statuses = grn_hash_size(ctx, drilldowns);
  statuses = GRN_PLUGIN_MALLOCN(ctx, tsort_status, n_statuses);
  if (!statuses) {
    return GRN_FALSE;
  }

  drilldown_tsort_init(ctx, statuses, n_statuses);
  succeeded = drilldown_tsort_body(ctx, drilldowns, statuses, ids);
  GRN_PLUGIN_FREE(ctx, statuses);
  return succeeded;
}

static grn_bool
grn_select_drilldowns_execute(grn_ctx *ctx,
                              grn_select_data *data)
{
  grn_bool succeeded = GRN_TRUE;
  grn_obj tsorted_ids;
  size_t i;
  size_t n_drilldowns;

  GRN_RECORD_INIT(&tsorted_ids, GRN_OBJ_VECTOR, GRN_ID_NIL);
  if (!drilldown_tsort(ctx, data->drilldowns, &tsorted_ids)) {
    succeeded = GRN_FALSE;
    goto exit;
  }

  n_drilldowns = GRN_BULK_VSIZE(&tsorted_ids) / sizeof(grn_id);
  for (i = 0; i < n_drilldowns; i++) {
    grn_id id;

    id = GRN_RECORD_VALUE_AT(&tsorted_ids, i);
    if (!grn_select_drilldown_execute(ctx,
                                      data,
                                      data->tables.result,
                                      data->drilldowns,
                                      id)) {
      if (ctx->rc != GRN_SUCCESS) {
        succeeded = GRN_FALSE;
        break;
      }
    }
  }

exit :
  GRN_OBJ_FIN(ctx, &tsorted_ids);

  return succeeded;
}

static grn_drilldown_data *
grn_select_data_drilldowns_add(grn_ctx *ctx,
                               grn_select_data *data,
                               const char *label,
                               size_t label_len)
{
  grn_drilldown_data *drilldown = NULL;
  int added;

  if (!data->drilldowns) {
    data->drilldowns = grn_hash_create(ctx,
                                       NULL,
                                       GRN_TABLE_MAX_KEY_SIZE,
                                       sizeof(grn_drilldown_data),
                                       GRN_OBJ_TABLE_HASH_KEY |
                                       GRN_OBJ_KEY_VAR_SIZE |
                                       GRN_HASH_TINY);
    if (!data->drilldowns) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][drilldowns] "
                       "failed to allocate drilldowns data: %s",
                       ctx->errbuf);
      return NULL;
    }
  }

  grn_hash_add(ctx,
               data->drilldowns,
               label,
               label_len,
               (void **)&drilldown,
               &added);
  if (added) {
    grn_drilldown_data_init(ctx, drilldown, label, label_len);
  }

  return drilldown;
}

static grn_bool
grn_select_prepare_drilldowns(grn_ctx *ctx,
                              grn_select_data *data)
{
  if (data->drilldown.keys.length > 0) {
    data->drilldown.parsed_keys =
      grn_table_sort_key_from_str(ctx,
                                  data->drilldown.keys.value,
                                  data->drilldown.keys.length,
                                  data->tables.result,
                                  &(data->drilldown.n_parsed_keys));
    if (data->drilldown.parsed_keys) {
      int i;
      grn_obj buffer;

      GRN_TEXT_INIT(&buffer, 0);
      for (i = 0; i < data->drilldown.n_parsed_keys; i++) {
        grn_drilldown_data *drilldown;

        GRN_BULK_REWIND(&buffer);
        grn_text_printf(ctx, &buffer, "drilldown%d", i);
        drilldown = grn_select_data_drilldowns_add(ctx,
                                                   data,
                                                   GRN_TEXT_VALUE(&buffer),
                                                   GRN_TEXT_LEN(&buffer));
        if (!drilldown) {
          continue;
        }

        drilldown->parsed_keys = data->drilldown.parsed_keys + i;
        drilldown->n_parsed_keys = 1;

#define COPY(field)                                     \
        drilldown->field = data->drilldown.field

        COPY(sort_keys);
        COPY(output_columns);
        COPY(offset);
        COPY(limit);
        COPY(calc_types);
        COPY(calc_target_name);
        COPY(filter);

#undef COPY
      }
    }
  }

  if (!data->drilldowns) {
    return GRN_TRUE;
  }

  if (!grn_select_drilldowns_execute(ctx, data)) {
    return GRN_FALSE;
  }

  {
    unsigned int n_available_results = 0;

    GRN_HASH_EACH_BEGIN(ctx, data->drilldowns, cursor, id) {
      grn_drilldown_data *drilldown;
      grn_table_group_result *result;

      grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);
      result = &(drilldown->result);
      if (result->table) {
        n_available_results++;
      }
    } GRN_HASH_EACH_END(ctx, cursor);

    if (data->drilldown.keys.length > 0) {
      data->output.n_elements += n_available_results;
    } else {
      if (n_available_results > 0) {
        data->output.n_elements += 1;
      }
    }
  }

  return GRN_TRUE;
}

static grn_bool
grn_select_output_drilldowns(grn_ctx *ctx,
                             grn_select_data *data)
{
  grn_bool succeeded = GRN_TRUE;
  unsigned int n_available_results = 0;
  grn_bool is_labeled;

  if (!data->drilldowns) {
    return GRN_TRUE;
  }

  data->output.formatter->drilldowns_label(ctx, data);

  GRN_HASH_EACH_BEGIN(ctx, data->drilldowns, cursor, id) {
    grn_drilldown_data *drilldown;
    grn_table_group_result *result;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);
    result = &(drilldown->result);
    if (result->table) {
      n_available_results++;
    }
  } GRN_HASH_EACH_END(ctx, cursor);

  is_labeled = (data->drilldown.keys.length == 0);

  data->output.formatter->drilldowns_open(ctx, data, n_available_results);

  GRN_HASH_EACH_BEGIN(ctx, data->drilldowns, cursor, id) {
    grn_drilldown_data *drilldown;
    grn_table_group_result *result;
    grn_obj *target_table;
    uint32_t n_hits;
    int offset;
    int limit;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);
    result = &(drilldown->result);

    if (!result->table) {
      continue;
    }

    if (drilldown->filtered_result) {
      target_table = drilldown->filtered_result;
    } else {
      target_table = result->table;
    }

    n_hits = grn_table_size(ctx, target_table);

    offset = drilldown->offset;
    limit = drilldown->limit;
    grn_normalize_offset_and_limit(ctx, n_hits, &offset, &limit);

    if (drilldown->sort_keys.length > 0) {
      grn_table_sort_key *sort_keys;
      uint32_t n_sort_keys;
      sort_keys = grn_table_sort_key_from_str(ctx,
                                              drilldown->sort_keys.value,
                                              drilldown->sort_keys.length,
                                              target_table, &n_sort_keys);
      if (sort_keys) {
        grn_obj *sorted;
        sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY,
                                  NULL, target_table);
        if (sorted) {
          grn_table_sort(ctx, target_table, offset, limit,
                         sorted, sort_keys, n_sort_keys);
          data->output.formatter->drilldown_label(ctx, data, drilldown);
          if (!grn_proc_select_output_columns(ctx,
                                              sorted,
                                              n_hits,
                                              0,
                                              limit,
                                              drilldown->output_columns.value,
                                              drilldown->output_columns.length,
                                              data->filter.condition.expression)) {
            succeeded = GRN_FALSE;
          }
          grn_obj_unlink(ctx, sorted);
        }
        grn_table_sort_key_close(ctx, sort_keys, n_sort_keys);
      } else {
        succeeded = GRN_FALSE;
      }
    } else {
      data->output.formatter->drilldown_label(ctx, data, drilldown);
      if (!grn_proc_select_output_columns(ctx,
                                          target_table,
                                          n_hits,
                                          offset,
                                          limit,
                                          drilldown->output_columns.value,
                                          drilldown->output_columns.length,
                                          data->filter.condition.expression)) {
        succeeded = GRN_FALSE;
      }
    }

    if (!succeeded) {
      break;
    }

    if (is_labeled) {
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                    ":", "output.drilldowns[%.*s](%d)",
                    (int)(drilldown->label.length),
                    drilldown->label.value,
                    n_hits);
    } else {
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                    ":", "output.drilldown(%d)", n_hits);
    }
  } GRN_HASH_EACH_END(ctx, cursor);

  data->output.formatter->drilldowns_close(ctx, data);

  return succeeded;
}

static grn_bool
grn_select_output(grn_ctx *ctx, grn_select_data *data)
{
  grn_bool succeeded = GRN_TRUE;

  if (grn_ctx_get_command_version(ctx) < GRN_COMMAND_VERSION_3) {
    GRN_OUTPUT_ARRAY_OPEN("RESULT", data->output.n_elements);
    succeeded = grn_select_output_match(ctx, data);
    if (succeeded) {
      succeeded = grn_select_output_slices(ctx, data);
    }
    if (succeeded) {
      succeeded = grn_select_output_drilldowns(ctx, data);
    }
    GRN_OUTPUT_ARRAY_CLOSE();
  } else {
    grn_obj_format format;
    uint32_t n_additional_elements = 0;

    if (data->slices) {
      n_additional_elements++;
    }
    if (data->drilldowns) {
      n_additional_elements++;
    }

    succeeded = grn_select_output_match_open(ctx,
                                             data,
                                             &format,
                                             n_additional_elements);
    if (succeeded) {
      succeeded = grn_select_output_slices(ctx, data);
      if (succeeded) {
        succeeded = grn_select_output_drilldowns(ctx, data);
      }
      if (!grn_select_output_match_close(ctx, data, &format)) {
        succeeded = GRN_FALSE;
      }
    }
  }

  return succeeded;
}

static void
grn_select_output_slices_label_v1(grn_ctx *ctx, grn_select_data *data)
{
}

static void
grn_select_output_slices_open_v1(grn_ctx *ctx,
                                 grn_select_data *data,
                                 unsigned int n_result_sets)
{
  GRN_OUTPUT_MAP_OPEN("SLICES", n_result_sets);
}

static void
grn_select_output_slices_close_v1(grn_ctx *ctx, grn_select_data *data)
{
  GRN_OUTPUT_MAP_CLOSE();
}

static void
grn_select_output_slice_label_v1(grn_ctx *ctx,
                                 grn_select_data *data,
                                 grn_slice_data *slice)
{
  GRN_OUTPUT_STR(slice->label.value, slice->label.length);
}

static void
grn_select_output_drilldowns_label_v1(grn_ctx *ctx, grn_select_data *data)
{
}

static void
grn_select_output_drilldowns_open_v1(grn_ctx *ctx,
                                     grn_select_data *data,
                                     unsigned int n_result_sets)
{
  if (data->drilldown.keys.length == 0) {
    GRN_OUTPUT_MAP_OPEN("DRILLDOWNS", n_result_sets);
  }
}

static void
grn_select_output_drilldowns_close_v1(grn_ctx *ctx, grn_select_data *data)
{
  if (data->drilldown.keys.length == 0) {
    GRN_OUTPUT_MAP_CLOSE();
  }
}

static void
grn_select_output_drilldown_label_v1(grn_ctx *ctx,
                                     grn_select_data *data,
                                     grn_drilldown_data *drilldown)
{
  if (data->drilldown.keys.length == 0) {
    GRN_OUTPUT_STR(drilldown->label.value, drilldown->label.length);
  }
}

static grn_select_output_formatter grn_select_output_formatter_v1 = {
  grn_select_output_slices_label_v1,
  grn_select_output_slices_open_v1,
  grn_select_output_slices_close_v1,
  grn_select_output_slice_label_v1,
  grn_select_output_drilldowns_label_v1,
  grn_select_output_drilldowns_open_v1,
  grn_select_output_drilldowns_close_v1,
  grn_select_output_drilldown_label_v1
};

static void
grn_select_output_slices_label_v3(grn_ctx *ctx, grn_select_data *data)
{
  GRN_OUTPUT_CSTR("slices");
}

static void
grn_select_output_slices_open_v3(grn_ctx *ctx,
                                 grn_select_data *data,
                                 unsigned int n_result_sets)
{
  GRN_OUTPUT_MAP_OPEN("slices", n_result_sets);
}

static void
grn_select_output_slices_close_v3(grn_ctx *ctx, grn_select_data *data)
{
  GRN_OUTPUT_MAP_CLOSE();
}

static void
grn_select_output_slice_label_v3(grn_ctx *ctx,
                                 grn_select_data *data,
                                 grn_slice_data *slice)
{
  GRN_OUTPUT_STR(slice->label.value, slice->label.length);
}

static void
grn_select_output_drilldowns_label_v3(grn_ctx *ctx, grn_select_data *data)
{
  GRN_OUTPUT_CSTR("drilldowns");
}

static void
grn_select_output_drilldowns_open_v3(grn_ctx *ctx,
                                     grn_select_data *data,
                                     unsigned int n_result_sets)
{
  GRN_OUTPUT_MAP_OPEN("drilldowns", n_result_sets);
}

static void
grn_select_output_drilldowns_close_v3(grn_ctx *ctx, grn_select_data *data)
{
  GRN_OUTPUT_MAP_CLOSE();
}

static void
grn_select_output_drilldown_label_v3(grn_ctx *ctx,
                                     grn_select_data *data,
                                     grn_drilldown_data *drilldown)
{
  if (data->drilldown.keys.length == 0) {
    GRN_OUTPUT_STR(drilldown->label.value, drilldown->label.length);
  } else {
    grn_obj *key;
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_len;

    key = drilldown->parsed_keys[0].key;
    switch (key->header.type) {
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_INDEX :
      name_len = grn_column_name(ctx, key, name, GRN_TABLE_MAX_KEY_SIZE);
      break;
    default :
      name_len = grn_obj_name(ctx, key, name, GRN_TABLE_MAX_KEY_SIZE);
      break;
    }
    GRN_OUTPUT_STR(name, name_len);
  }
}

static grn_select_output_formatter grn_select_output_formatter_v3 = {
  grn_select_output_slices_label_v3,
  grn_select_output_slices_open_v3,
  grn_select_output_slices_close_v3,
  grn_select_output_slice_label_v3,
  grn_select_output_drilldowns_label_v3,
  grn_select_output_drilldowns_open_v3,
  grn_select_output_drilldowns_close_v3,
  grn_select_output_drilldown_label_v3
};

static grn_rc
grn_select(grn_ctx *ctx, grn_select_data *data)
{
  uint32_t nhits;
  grn_obj *outbuf = ctx->impl->output.buf;
  grn_content_type output_type = ctx->impl->output.type;
  char cache_key[GRN_CACHE_MAX_KEY_SIZE];
  uint32_t cache_key_size;
  long long int threshold, original_threshold = 0;
  grn_cache *cache_obj = grn_cache_current_get(ctx);

  if (grn_ctx_get_command_version(ctx) < GRN_COMMAND_VERSION_3) {
    data->output.formatter = &grn_select_output_formatter_v1;
  } else {
    data->output.formatter = &grn_select_output_formatter_v3;
  }

  data->cacheable = 1;
  data->taintable = 0;

  data->output.n_elements = 0;

  grn_raw_string_lstrip(ctx, &(data->filter.query));

  cache_key_size =
    data->table.length + 1 +
    data->filter.match_columns.length + 1 +
    data->filter.query.length + 1 +
    data->filter.filter.length + 1 +
    data->scorer.length + 1 +
    data->sort_keys.length + 1 +
    data->output_columns.length + 1 +
    data->match_escalation_threshold.length + 1 +
    data->filter.query_expander.length + 1 +
    data->filter.query_flags.length + 1 +
    data->adjuster.length + 1 +
    sizeof(grn_content_type) +
    sizeof(int) * 2 +
    sizeof(grn_command_version) +
    sizeof(grn_bool);
  if (data->slices) {
    GRN_HASH_EACH_BEGIN(ctx, data->slices, cursor, id) {
      grn_slice_data *slice;
      grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);
      grn_raw_string_lstrip(ctx, &(slice->filter.query));
      cache_key_size +=
        slice->filter.match_columns.length + 1 +
        slice->filter.query.length + 1 +
        slice->filter.query_expander.length + 1 +
        slice->filter.query_flags.length + 1 +
        slice->filter.filter.length + 1 +
        slice->sort_keys.length + 1 +
        slice->output_columns.length + 1 +
        slice->label.length + 1 +
        sizeof(int) * 2;
    } GRN_HASH_EACH_END(ctx, cursor);
  }
#define DRILLDOWN_CACHE_SIZE(drilldown)         \
  drilldown->keys.length + 1 +                  \
  drilldown->sort_keys.length + 1 +             \
    drilldown->output_columns.length + 1 +      \
    drilldown->label.length + 1 +               \
    drilldown->calc_target_name.length + 1 +    \
    drilldown->filter.length + 1 +              \
    drilldown->table_name.length + 1 +          \
    sizeof(int) * 2 +                           \
    sizeof(grn_table_group_flags)
  if (data->drilldown.keys.length > 0) {
    grn_drilldown_data *drilldown = &(data->drilldown);
    cache_key_size += DRILLDOWN_CACHE_SIZE(drilldown);
  }
  if (data->drilldowns) {
    GRN_HASH_EACH_BEGIN(ctx, data->drilldowns, cursor, id) {
      grn_drilldown_data *drilldown;
      grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);
      cache_key_size += DRILLDOWN_CACHE_SIZE(drilldown);
    } GRN_HASH_EACH_END(ctx, cursor);
  }
#undef DRILLDOWN_CACHE_SIZE
  if (cache_key_size <= GRN_CACHE_MAX_KEY_SIZE) {
    char *cp = cache_key;

#define PUT_CACHE_KEY(string)                                   \
    if ((string).value)                                         \
      grn_memcpy(cp, (string).value, (string).length);          \
    cp += (string).length;                                      \
    *cp++ = '\0'

    PUT_CACHE_KEY(data->table);
    PUT_CACHE_KEY(data->filter.match_columns);
    PUT_CACHE_KEY(data->filter.query);
    PUT_CACHE_KEY(data->filter.filter);
    PUT_CACHE_KEY(data->scorer);
    PUT_CACHE_KEY(data->sort_keys);
    PUT_CACHE_KEY(data->output_columns);
    if (data->slices) {
      GRN_HASH_EACH_BEGIN(ctx, data->slices, cursor, id) {
        grn_slice_data *slice;
        grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);
        PUT_CACHE_KEY(slice->filter.match_columns);
        PUT_CACHE_KEY(slice->filter.query);
        PUT_CACHE_KEY(slice->filter.query_expander);
        PUT_CACHE_KEY(slice->filter.query_flags);
        PUT_CACHE_KEY(slice->filter.filter);
        PUT_CACHE_KEY(slice->sort_keys);
        PUT_CACHE_KEY(slice->output_columns);
        PUT_CACHE_KEY(slice->label);
        grn_memcpy(cp, &(slice->offset), sizeof(int));
        cp += sizeof(int);
        grn_memcpy(cp, &(slice->limit), sizeof(int));
        cp += sizeof(int);
      } GRN_HASH_EACH_END(ctx, cursor);
    }
#define PUT_CACHE_KEY_DRILLDOWN(drilldown) do {                 \
      PUT_CACHE_KEY(drilldown->keys);                           \
      PUT_CACHE_KEY(drilldown->sort_keys);                      \
      PUT_CACHE_KEY(drilldown->output_columns);                 \
      PUT_CACHE_KEY(drilldown->label);                          \
      PUT_CACHE_KEY(drilldown->calc_target_name);               \
      PUT_CACHE_KEY(drilldown->filter);                         \
      PUT_CACHE_KEY(drilldown->table_name);                     \
      grn_memcpy(cp, &(drilldown->offset), sizeof(int));        \
      cp += sizeof(int);                                        \
      grn_memcpy(cp, &(drilldown->limit), sizeof(int));         \
      cp += sizeof(int);                                        \
      grn_memcpy(cp,                                            \
                 &(drilldown->calc_types),                      \
                 sizeof(grn_table_group_flags));                \
      cp += sizeof(grn_table_group_flags);                      \
    } while (GRN_FALSE)
    if (data->drilldown.keys.length > 0) {
      grn_drilldown_data *drilldown = &(data->drilldown);
      PUT_CACHE_KEY_DRILLDOWN(drilldown);
    }
    if (data->drilldowns) {
      GRN_HASH_EACH_BEGIN(ctx, data->drilldowns, cursor, id) {
        grn_drilldown_data *drilldown;
        grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);
        PUT_CACHE_KEY_DRILLDOWN(drilldown);
      } GRN_HASH_EACH_END(ctx, cursor);
    }
#undef PUT_CACHE_KEY_DRILLDOWN
    PUT_CACHE_KEY(data->match_escalation_threshold);
    PUT_CACHE_KEY(data->filter.query_expander);
    PUT_CACHE_KEY(data->filter.query_flags);
    PUT_CACHE_KEY(data->adjuster);
    grn_memcpy(cp, &output_type, sizeof(grn_content_type));
    cp += sizeof(grn_content_type);
    grn_memcpy(cp, &(data->offset), sizeof(int));
    cp += sizeof(int);
    grn_memcpy(cp, &(data->limit), sizeof(int));
    cp += sizeof(int);
    grn_memcpy(cp, &(ctx->impl->command.version), sizeof(grn_command_version));
    cp += sizeof(grn_command_version);
    grn_memcpy(cp, &(ctx->impl->output.is_pretty), sizeof(grn_bool));
    cp += sizeof(grn_bool);
#undef PUT_CACHE_KEY

    {
      grn_rc rc;
      rc = grn_cache_fetch(ctx, cache_obj, cache_key, cache_key_size, outbuf);
      if (rc == GRN_SUCCESS) {
        GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_CACHE,
                      ":", "cache(%" GRN_FMT_LLD ")",
                      (long long int)GRN_TEXT_LEN(outbuf));
        return ctx->rc;
      }
    }
  }
  if (data->match_escalation_threshold.length) {
    const char *end, *rest;
    original_threshold = grn_ctx_get_match_escalation_threshold(ctx);
    end =
      data->match_escalation_threshold.value +
      data->match_escalation_threshold.length;
    threshold = grn_atoll(data->match_escalation_threshold.value, end, &rest);
    if (end == rest) {
      grn_ctx_set_match_escalation_threshold(ctx, threshold);
    }
  }

  data->tables.target = grn_ctx_get(ctx, data->table.value, data->table.length);
  if (!data->tables.target) {
    GRN_PLUGIN_ERROR(ctx,
                     GRN_INVALID_ARGUMENT,
                     "[select][table] invalid name: <%.*s>",
                     (int)(data->table.length),
                     data->table.value);
    goto exit;
  }

  {
    if (data->filter.filter.length > 0 &&
        (data->filter.filter.value[0] == '?') &&
        (ctx->impl->output.type == GRN_CONTENT_JSON)) {
      ctx->rc = grn_ts_select(ctx, data->tables.target,
                              data->filter.filter.value + 1,
                              data->filter.filter.length - 1,
                              data->scorer.value,
                              data->scorer.length,
                              data->sort_keys.value,
                              data->sort_keys.length,
                              data->output_columns.value,
                              data->output_columns.length,
                              data->offset,
                              data->limit);
      if (!ctx->rc &&
          data->cacheable > 0 &&
          cache_key_size <= GRN_CACHE_MAX_KEY_SIZE &&
          (!data->cache.value ||
           data->cache.length != 2 ||
           data->cache.value[0] != 'n' ||
           data->cache.value[1] != 'o')) {
        grn_cache_update(ctx, cache_obj, cache_key, cache_key_size, outbuf);
      }
      goto exit;
    }

    data->tables.initial = data->tables.target;
    if (!grn_select_apply_initial_columns(ctx, data)) {
      goto exit;
    }

    if (!grn_select_filter(ctx, data)) {
      goto exit;
    }

    nhits = grn_table_size(ctx, data->tables.result);
    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                  ":", "select(%d)", nhits);

    if (!grn_select_apply_filtered_columns(ctx, data)) {
      goto exit;
    }

    {
      grn_bool succeeded;

      /* For select results */
      data->output.n_elements = 1;

      if (!grn_select_apply_adjuster(ctx, data)) {
        goto exit;
      }

      if (!grn_select_apply_scorer(ctx, data)) {
        goto exit;
      }

      grn_normalize_offset_and_limit(ctx, nhits,
                                     &(data->offset), &(data->limit));

      if (!grn_select_sort(ctx, data)) {
        goto exit;
      }

      if (!grn_select_apply_output_columns(ctx, data)) {
        goto exit;
      }

      if (!grn_select_prepare_slices(ctx, data)) {
        goto exit;
      }

      if (!grn_select_prepare_drilldowns(ctx, data)) {
        goto exit;
      }

      succeeded = grn_select_output(ctx, data);
      if (!succeeded) {
        goto exit;
      }
    }
    if (!ctx->rc &&
        data->cacheable &&
        cache_key_size <= GRN_CACHE_MAX_KEY_SIZE &&
        (!data->cache.value ||
         data->cache.length != 2 ||
         data->cache.value[0] != 'n' ||
         data->cache.value[1] != 'o')) {
      grn_cache_update(ctx, cache_obj, cache_key, cache_key_size, outbuf);
    }
    if (data->taintable > 0) {
      grn_db_touch(ctx, DB_OBJ(data->tables.target)->db);
    }
  }

exit :
  if (data->match_escalation_threshold.length > 0) {
    grn_ctx_set_match_escalation_threshold(ctx, original_threshold);
  }

  /* GRN_LOG(ctx, GRN_LOG_NONE, "%d", ctx->seqno); */

  return ctx->rc;
}

static grn_slice_data *
grn_select_data_slices_add(grn_ctx *ctx,
                           grn_select_data *data,
                           const char *label,
                           size_t label_len)
{
  grn_slice_data *slice = NULL;
  int added;

  if (!data->slices) {
    data->slices = grn_hash_create(ctx,
                                   NULL,
                                   GRN_TABLE_MAX_KEY_SIZE,
                                   sizeof(grn_slice_data),
                                   GRN_OBJ_TABLE_HASH_KEY |
                                   GRN_OBJ_KEY_VAR_SIZE |
                                   GRN_HASH_TINY);
    if (!data->slices) {
      GRN_PLUGIN_ERROR(ctx,
                       GRN_INVALID_ARGUMENT,
                       "[select][slices] "
                       "failed to allocate slices data: %s",
                       ctx->errbuf);
      return NULL;
    }
  }

  grn_hash_add(ctx,
               data->slices,
               label,
               label_len,
               (void **)&slice,
               &added);
  if (added) {
    grn_slice_data_init(ctx, slice, label, label_len);
  }

  return slice;
}

static grn_bool
grn_select_data_fill_slice_labels(grn_ctx *ctx,
                                  grn_user_data *user_data,
                                  grn_select_data *data)
{
  grn_obj *vars;
  grn_table_cursor *cursor;
  const char *prefix = "slices[";
  int prefix_len;

  vars = grn_plugin_proc_get_vars(ctx, user_data);

  cursor = grn_table_cursor_open(ctx, vars, NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    return GRN_FALSE;
  }

  prefix_len = strlen(prefix);
  while (grn_table_cursor_next(ctx, cursor)) {
    void *key;
    char *name;
    int name_len;
    name_len = grn_table_cursor_get_key(ctx, cursor, &key);
    name = key;
    if (name_len > prefix_len + 1 &&
        strncmp(prefix, name, prefix_len) == 0) {
      const char *label_end;
      size_t label_len;
      label_end = memchr(name + prefix_len + 1,
                         ']',
                         name_len - prefix_len - 1);
      if (!label_end) {
        continue;
      }
      label_len = (label_end - name) - prefix_len;
      grn_select_data_slices_add(ctx,
                                 data,
                                 name + prefix_len,
                                 label_len);
    }
  }
  grn_table_cursor_close(ctx, cursor);

  return GRN_TRUE;
}

static grn_bool
grn_select_data_fill_slices(grn_ctx *ctx,
                            grn_user_data *user_data,
                            grn_select_data *data)
{
  if (!grn_select_data_fill_slice_labels(ctx, user_data, data)) {
    return GRN_FALSE;
  }

  GRN_HASH_EACH_BEGIN(ctx, data->slices, cursor, id) {
    grn_slice_data *slice;
    char slice_label[GRN_TABLE_MAX_KEY_SIZE];
    char key_name[GRN_TABLE_MAX_KEY_SIZE];
    grn_obj *match_columns;
    grn_obj *query;
    grn_obj *query_expander;
    grn_obj *query_flags;
    grn_obj *filter;
    grn_obj *sort_keys;
    grn_obj *output_columns;
    grn_obj *offset;
    grn_obj *limit;

    grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);

    grn_snprintf(slice_label,
                 GRN_TABLE_MAX_KEY_SIZE,
                 GRN_TABLE_MAX_KEY_SIZE,
                 "slices[%.*s].",
                 (int)(slice->label.length),
                 slice->label.value);

#define GET_VAR(name)                                                   \
      grn_snprintf(key_name,                                            \
                   GRN_TABLE_MAX_KEY_SIZE,                              \
                   GRN_TABLE_MAX_KEY_SIZE,                              \
                   "%s%s", slice_label, #name);                         \
      name = grn_plugin_proc_get_var(ctx, user_data, key_name, -1);

      GET_VAR(match_columns);
      GET_VAR(query);
      GET_VAR(query_expander);
      GET_VAR(query_flags);
      GET_VAR(filter);
      GET_VAR(sort_keys);
      GET_VAR(output_columns);
      GET_VAR(offset);
      GET_VAR(limit);

#undef GET_VAR

      grn_slice_data_fill(ctx,
                          slice,
                          match_columns,
                          query,
                          query_expander,
                          query_flags,
                          filter,
                          sort_keys,
                          output_columns,
                          offset,
                          limit);
  } GRN_HASH_EACH_END(ctx, cursor);

  return GRN_TRUE;
}

static grn_bool
grn_select_data_fill_drilldown_labels(grn_ctx *ctx,
                                      grn_user_data *user_data,
                                      grn_select_data *data,
                                      const char *prefix)
{
  grn_obj *vars;
  grn_table_cursor *cursor;
  int prefix_len;

  vars = grn_plugin_proc_get_vars(ctx, user_data);

  cursor = grn_table_cursor_open(ctx, vars, NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    return GRN_FALSE;
  }

  prefix_len = strlen(prefix);
  while (grn_table_cursor_next(ctx, cursor)) {
    void *key;
    char *name;
    int name_len;
    name_len = grn_table_cursor_get_key(ctx, cursor, &key);
    name = key;
    if (name_len > prefix_len + 1 &&
        strncmp(prefix, name, prefix_len) == 0) {
      const char *label_end;
      size_t label_len;
      label_end = memchr(name + prefix_len + 1,
                         ']',
                         name_len - prefix_len - 1);
      if (!label_end) {
        continue;
      }
      label_len = (label_end - name) - prefix_len;
      grn_select_data_drilldowns_add(ctx,
                                     data,
                                     name + prefix_len,
                                     label_len);
    }
  }
  grn_table_cursor_close(ctx, cursor);

  return GRN_TRUE;
}

static grn_bool
grn_select_data_fill_drilldown_columns(grn_ctx *ctx,
                                       grn_user_data *user_data,
                                       grn_drilldown_data *drilldown,
                                       const char *parameter_key)
{
  char prefix[GRN_TABLE_MAX_KEY_SIZE];

  grn_snprintf(prefix,
               GRN_TABLE_MAX_KEY_SIZE,
               GRN_TABLE_MAX_KEY_SIZE,
               "%s[%.*s].",
               parameter_key,
               (int)(drilldown->label.length),
               drilldown->label.value);
  return grn_columns_fill(ctx,
                          user_data,
                          &(drilldown->columns),
                          prefix,
                          strlen(prefix));
}

static grn_bool
grn_select_data_fill_drilldowns(grn_ctx *ctx,
                                grn_user_data *user_data,
                                grn_select_data *data)
{
  grn_obj *drilldown;

  drilldown = grn_plugin_proc_get_var(ctx, user_data, "drilldown", -1);
  if (GRN_TEXT_LEN(drilldown) > 0) {
    grn_obj *sort_keys;

    sort_keys = grn_plugin_proc_get_var(ctx, user_data,
                                        "drilldown_sort_keys", -1);
    if (GRN_TEXT_LEN(sort_keys) == 0) {
      /* For backward compatibility */
      sort_keys = grn_plugin_proc_get_var(ctx, user_data,
                                          "drilldown_sortby", -1);
    }
    grn_drilldown_data_fill(ctx,
                            &(data->drilldown),
                            drilldown,
                            sort_keys,
                            grn_plugin_proc_get_var(ctx, user_data,
                                                    "drilldown_output_columns",
                                                    -1),
                            grn_plugin_proc_get_var(ctx, user_data,
                                                    "drilldown_offset", -1),
                            grn_plugin_proc_get_var(ctx, user_data,
                                                    "drilldown_limit", -1),
                            grn_plugin_proc_get_var(ctx, user_data,
                                                    "drilldown_calc_types", -1),
                            grn_plugin_proc_get_var(ctx, user_data,
                                                    "drilldown_calc_target", -1),
                            grn_plugin_proc_get_var(ctx, user_data,
                                                    "drilldown_filter", -1),
                            NULL);
    return GRN_TRUE;
  } else {
    grn_bool succeeded = GRN_TRUE;

    if (!grn_select_data_fill_drilldown_labels(ctx, user_data, data,
                                               "drilldowns[")) {
      return GRN_FALSE;
    }

    /* For backward compatibility */
    if (!grn_select_data_fill_drilldown_labels(ctx, user_data, data,
                                               "drilldown[")) {
      return GRN_FALSE;
    }

    GRN_HASH_EACH_BEGIN(ctx, data->drilldowns, cursor, id) {
      grn_drilldown_data *drilldown;
      grn_obj *keys = NULL;
      grn_obj *sort_keys = NULL;
      grn_obj *output_columns = NULL;
      grn_obj *offset = NULL;
      grn_obj *limit = NULL;
      grn_obj *calc_types = NULL;
      grn_obj *calc_target = NULL;
      grn_obj *filter = NULL;
      grn_obj *table = NULL;

      grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);

      succeeded = grn_select_data_fill_drilldown_columns(ctx,
                                                         user_data,
                                                         drilldown,
                                                         "drilldowns");
      if (!succeeded) {
        break;
      }

      /* For backward compatibility */
      succeeded = grn_select_data_fill_drilldown_columns(ctx,
                                                         user_data,
                                                         drilldown,
                                                         "drilldown");
      if (!succeeded) {
        break;
      }

#define GET_VAR_RAW(parameter_key, name) do {                           \
        if (!name) {                                                    \
          char key_name[GRN_TABLE_MAX_KEY_SIZE];                        \
          grn_snprintf(key_name,                                        \
                       GRN_TABLE_MAX_KEY_SIZE,                          \
                       GRN_TABLE_MAX_KEY_SIZE,                          \
                       "%s[%.*s].%s",                                   \
                       (parameter_key),                                 \
                       (int)(drilldown->label.length),                  \
                       drilldown->label.value,                          \
                       #name);                                          \
          name = grn_plugin_proc_get_var(ctx, user_data, key_name, -1); \
        }                                                               \
      } while (GRN_FALSE)

#define GET_VAR(name) do {                                              \
        GET_VAR_RAW("drilldowns", name);                                \
        /* For backward compatibility */                                \
        GET_VAR_RAW("drilldown", name);                                 \
      } while (GRN_FALSE)

      GET_VAR(keys);
      GET_VAR(sort_keys);
      if (!sort_keys) {
        grn_obj *sortby = NULL;
        GET_VAR(sortby);
        sort_keys = sortby;
      }
      GET_VAR(output_columns);
      GET_VAR(offset);
      GET_VAR(limit);
      GET_VAR(calc_types);
      GET_VAR(calc_target);
      GET_VAR(filter);
      GET_VAR(table);

#undef GET_VAR

#undef GET_VAR_RAW

      grn_drilldown_data_fill(ctx,
                              drilldown,
                              keys,
                              sort_keys,
                              output_columns,
                              offset,
                              limit,
                              calc_types,
                              calc_target,
                              filter,
                              table);
    } GRN_HASH_EACH_END(ctx, cursor);

    return succeeded;
  }
}

static grn_obj *
command_select(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_select_data data;

  grn_columns_init(ctx, &(data.columns));
  grn_filter_data_init(ctx, &(data.filter));

  data.tables.target = NULL;
  data.tables.initial = NULL;
  data.tables.result = NULL;
  data.tables.sorted = NULL;

  data.slices = NULL;
  grn_drilldown_data_init(ctx, &(data.drilldown), NULL, 0);
  data.drilldowns = NULL;

  data.table.value = grn_plugin_proc_get_var_string(ctx, user_data,
                                                    "table", -1,
                                                    &(data.table.length));
#define GET_VAR(name)                                           \
  grn_plugin_proc_get_var(ctx, user_data, name, strlen(name))

  {
    grn_obj *query_expander;

    query_expander = GET_VAR("query_expander");
    if (GRN_TEXT_LEN(query_expander) == 0) {
      query_expander = GET_VAR("query_expansion");
    }

    grn_filter_data_fill(ctx,
                         &(data.filter),
                         GET_VAR("match_columns"),
                         GET_VAR("query"),
                         query_expander,
                         GET_VAR("query_flags"),
                         GET_VAR("filter"));
  }
#undef GET_VAR

  data.scorer.value =
    grn_plugin_proc_get_var_string(ctx, user_data,
                                   "scorer", -1,
                                   &(data.scorer.length));
  data.sort_keys.value =
    grn_plugin_proc_get_var_string(ctx, user_data,
                                   "sort_keys", -1,
                                   &(data.sort_keys.length));
  if (data.sort_keys.length == 0) {
    /* For backward compatibility */
    data.sort_keys.value =
      grn_plugin_proc_get_var_string(ctx, user_data,
                                     "sortby", -1,
                                     &(data.sort_keys.length));
  }
  data.output_columns.value =
    grn_plugin_proc_get_var_string(ctx, user_data,
                                   "output_columns", -1,
                                   &(data.output_columns.length));
  if (!data.output_columns.value) {
    data.output_columns.value = GRN_SELECT_DEFAULT_OUTPUT_COLUMNS;
    data.output_columns.length = strlen(GRN_SELECT_DEFAULT_OUTPUT_COLUMNS);
  }
  data.offset = grn_plugin_proc_get_var_int32(ctx, user_data,
                                              "offset", -1,
                                              0);
  data.limit = grn_plugin_proc_get_var_int32(ctx, user_data,
                                             "limit", -1,
                                             GRN_SELECT_DEFAULT_LIMIT);

  data.cache.value = grn_plugin_proc_get_var_string(ctx, user_data,
                                                    "cache", -1,
                                                    &(data.cache.length));
  data.match_escalation_threshold.value =
    grn_plugin_proc_get_var_string(ctx, user_data,
                                   "match_escalation_threshold", -1,
                                   &(data.match_escalation_threshold.length));

  data.adjuster.value =
    grn_plugin_proc_get_var_string(ctx, user_data,
                                   "adjuster", -1,
                                   &(data.adjuster.length));

  if (!grn_select_data_fill_slices(ctx, user_data, &data)) {
    goto exit;
  }

  if (!grn_select_data_fill_drilldowns(ctx, user_data, &data)) {
    goto exit;
  }

  if (!grn_columns_fill(ctx, user_data, &(data.columns), NULL, 0)) {
    goto exit;
  }

  grn_select(ctx, &data);

exit :
  if (data.drilldowns) {
    GRN_HASH_EACH_BEGIN(ctx, data.drilldowns, cursor, id) {
      grn_drilldown_data *drilldown;
      grn_hash_cursor_get_value(ctx, cursor, (void **)&drilldown);
      grn_drilldown_data_fin(ctx, drilldown);
    } GRN_HASH_EACH_END(ctx, cursor);
    grn_hash_close(ctx, data.drilldowns);
  }

  if (data.drilldown.parsed_keys) {
    grn_table_sort_key_close(ctx,
                             data.drilldown.parsed_keys,
                             data.drilldown.n_parsed_keys);
  }
  grn_drilldown_data_fin(ctx, &(data.drilldown));

  if (data.slices) {
    GRN_HASH_EACH_BEGIN(ctx, data.slices, cursor, id) {
      grn_slice_data *slice;
      grn_hash_cursor_get_value(ctx, cursor, (void **)&slice);
      grn_slice_data_fin(ctx, slice);
    } GRN_HASH_EACH_END(ctx, cursor);
    grn_hash_close(ctx, data.slices);
  }

  if (data.tables.sorted) {
    grn_obj_unlink(ctx, data.tables.sorted);
  }

  if (data.tables.result == data.filter.filtered) {
    data.tables.result = NULL;
  }
  grn_filter_data_fin(ctx, &(data.filter));

  if (data.tables.result &&
      data.tables.result != data.tables.initial &&
      data.tables.result != data.tables.target) {
    grn_obj_unlink(ctx, data.tables.result);
  }

  if (data.tables.initial && data.tables.initial != data.tables.target) {
    grn_obj_unlink(ctx, data.tables.initial);
  }

  if (data.tables.target) {
    grn_obj_unlink(ctx, data.tables.target);
  }

  grn_columns_fin(ctx, &(data.columns));

  return NULL;
}

#define N_VARS 26
#define DEFINE_VARS grn_expr_var vars[N_VARS]

static void
init_vars(grn_ctx *ctx, grn_expr_var *vars)
{
  grn_plugin_expr_var_init(ctx, &(vars[0]), "name", -1);
  grn_plugin_expr_var_init(ctx, &(vars[1]), "table", -1);
  grn_plugin_expr_var_init(ctx, &(vars[2]), "match_columns", -1);
  grn_plugin_expr_var_init(ctx, &(vars[3]), "query", -1);
  grn_plugin_expr_var_init(ctx, &(vars[4]), "filter", -1);
  grn_plugin_expr_var_init(ctx, &(vars[5]), "scorer", -1);
  /* Deprecated since 6.0.3. Use sort_keys instead. */
  grn_plugin_expr_var_init(ctx, &(vars[6]), "sortby", -1);
  grn_plugin_expr_var_init(ctx, &(vars[7]), "output_columns", -1);
  grn_plugin_expr_var_init(ctx, &(vars[8]), "offset", -1);
  grn_plugin_expr_var_init(ctx, &(vars[9]), "limit", -1);
  grn_plugin_expr_var_init(ctx, &(vars[10]), "drilldown", -1);
  /* Deprecated since 6.0.3. Use drilldown_sort_keys instead. */
  grn_plugin_expr_var_init(ctx, &(vars[11]), "drilldown_sortby", -1);
  grn_plugin_expr_var_init(ctx, &(vars[12]), "drilldown_output_columns", -1);
  grn_plugin_expr_var_init(ctx, &(vars[13]), "drilldown_offset", -1);
  grn_plugin_expr_var_init(ctx, &(vars[14]), "drilldown_limit", -1);
  grn_plugin_expr_var_init(ctx, &(vars[15]), "cache", -1);
  grn_plugin_expr_var_init(ctx, &(vars[16]), "match_escalation_threshold", -1);
  /* Deprecated. Use query_expander instead. */
  grn_plugin_expr_var_init(ctx, &(vars[17]), "query_expansion", -1);
  grn_plugin_expr_var_init(ctx, &(vars[18]), "query_flags", -1);
  grn_plugin_expr_var_init(ctx, &(vars[19]), "query_expander", -1);
  grn_plugin_expr_var_init(ctx, &(vars[20]), "adjuster", -1);
  grn_plugin_expr_var_init(ctx, &(vars[21]), "drilldown_calc_types", -1);
  grn_plugin_expr_var_init(ctx, &(vars[22]), "drilldown_calc_target", -1);
  grn_plugin_expr_var_init(ctx, &(vars[23]), "drilldown_filter", -1);
  grn_plugin_expr_var_init(ctx, &(vars[24]), "sort_keys", -1);
  grn_plugin_expr_var_init(ctx, &(vars[25]), "drilldown_sort_keys", -1);
}

void
grn_proc_init_select(grn_ctx *ctx)
{
  DEFINE_VARS;

  init_vars(ctx, vars);
  grn_plugin_command_create(ctx,
                            "select", -1,
                            command_select,
                            N_VARS - 1,
                            vars + 1);
}

static grn_obj *
command_define_selector(grn_ctx *ctx, int nargs, grn_obj **args,
                        grn_user_data *user_data)
{
  uint32_t i, nvars;
  grn_expr_var *vars;

  grn_proc_get_info(ctx, user_data, &vars, &nvars, NULL);
  for (i = 1; i < nvars; i++) {
    grn_obj *var;
    var = grn_plugin_proc_get_var_by_offset(ctx, user_data, i);
    GRN_TEXT_SET(ctx, &((vars + i)->value),
                 GRN_TEXT_VALUE(var),
                 GRN_TEXT_LEN(var));
  }
  {
    grn_obj *name;
    name = grn_plugin_proc_get_var(ctx, user_data, "name", -1);
    grn_plugin_command_create(ctx,
                              GRN_TEXT_VALUE(name),
                              GRN_TEXT_LEN(name),
                              command_select,
                              nvars - 1,
                              vars + 1);
  }
  GRN_OUTPUT_BOOL(!ctx->rc);

  return NULL;
}

void
grn_proc_init_define_selector(grn_ctx *ctx)
{
  DEFINE_VARS;

  init_vars(ctx, vars);
  grn_plugin_command_create(ctx,
                            "define_selector", -1,
                            command_define_selector,
                            N_VARS,
                            vars);
}
