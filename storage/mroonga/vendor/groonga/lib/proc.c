/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "grn_proc.h"
#include "grn_ii.h"
#include "grn_db.h"
#include "grn_util.h"
#include "grn_output.h"
#include "grn_pat.h"
#include "grn_geo.h"
#include "grn_token_cursor.h"
#include "grn_expr.h"

#include <string.h>
#include <float.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef grn_rc (*grn_substitute_term_func) (grn_ctx *ctx,
                                            const char *term,
                                            unsigned int term_len,
                                            grn_obj *substituted_term,
                                            grn_user_data *user_data);
typedef struct {
  grn_obj *table;
  grn_obj *column;
} grn_substitute_term_by_column_data;

/**** globals for procs ****/
const char *grn_document_root = NULL;

#define VAR GRN_PROC_GET_VAR_BY_OFFSET

#define GRN_SELECT_INTERNAL_VAR_CONDITION     "$condition"
#define GRN_SELECT_INTERNAL_VAR_MATCH_COLUMNS "$match_columns"

/* bulk must be initialized grn_bulk or grn_msg */
static int
grn_bulk_put_from_file(grn_ctx *ctx, grn_obj *bulk, const char *path)
{
  /* FIXME: implement more smartly with grn_bulk */
  int fd, ret = 0;
  struct stat stat;
  if ((fd = GRN_OPEN(path, O_RDONLY|O_NOFOLLOW|O_BINARY)) == -1) {
    switch (errno) {
    case EACCES :
      ERR(GRN_OPERATION_NOT_PERMITTED, "request is not allowed: <%s>", path);
      break;
    case ENOENT :
      ERR(GRN_NO_SUCH_FILE_OR_DIRECTORY, "no such file: <%s>", path);
      break;
#ifndef WIN32
    case ELOOP :
      ERR(GRN_NO_SUCH_FILE_OR_DIRECTORY,
          "symbolic link is not allowed: <%s>", path);
      break;
#endif /* WIN32 */
    default :
      ERR(GRN_UNKNOWN_ERROR, "GRN_OPEN() failed(errno: %d): <%s>", errno, path);
      break;
    }
    return 0;
  }
  if (fstat(fd, &stat) != -1) {
    char *buf, *bp;
    off_t rest = stat.st_size;
    if ((buf = GRN_MALLOC(rest))) {
      ssize_t ss;
      for (bp = buf; rest; rest -= ss, bp += ss) {
        if ((ss = GRN_READ(fd, bp, rest)) == -1) { goto exit; }
      }
      GRN_TEXT_PUT(ctx, bulk, buf, stat.st_size);
      ret = 1;
    }
    GRN_FREE(buf);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "cannot stat file: <%s>", path);
  }
exit :
  GRN_CLOSE(fd);
  return ret;
}

#ifdef stat
#  undef stat
#endif /* stat */

/**** query expander ****/

static grn_rc
substitute_term_by_func(grn_ctx *ctx, const char *term, unsigned int term_len,
                        grn_obj *expanded_term, grn_user_data *user_data)
{
  grn_rc rc;
  grn_obj *expander = user_data->ptr;
  grn_obj grn_term;
  grn_obj *caller;
  grn_obj *rc_object;
  int nargs = 0;

  GRN_TEXT_INIT(&grn_term, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &grn_term, term, term_len);
  grn_ctx_push(ctx, &grn_term);
  nargs++;
  grn_ctx_push(ctx, expanded_term);
  nargs++;

  caller = grn_expr_create(ctx, NULL, 0);
  rc = grn_proc_call(ctx, expander, nargs, caller);
  GRN_OBJ_FIN(ctx, &grn_term);
  rc_object = grn_ctx_pop(ctx);
  rc = GRN_INT32_VALUE(rc_object);
  grn_obj_unlink(ctx, caller);

  return rc;
}

static grn_rc
substitute_term_by_column(grn_ctx *ctx, const char *term, unsigned int term_len,
                          grn_obj *expanded_term, grn_user_data *user_data)
{
  grn_rc rc = GRN_END_OF_DATA;
  grn_id id;
  grn_substitute_term_by_column_data *data = user_data->ptr;
  grn_obj *table, *column;

  table = data->table;
  column = data->column;
  if ((id = grn_table_get(ctx, table, term, term_len))) {
    if ((column->header.type == GRN_COLUMN_VAR_SIZE) &&
        ((column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_VECTOR)) {
      unsigned int i, n;
      grn_obj values;
      GRN_TEXT_INIT(&values, GRN_OBJ_VECTOR);
      grn_obj_get_value(ctx, column, id, &values);
      n = grn_vector_size(ctx, &values);
      if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, '('); }
      for (i = 0; i < n; i++) {
        const char *value;
        unsigned int length;
        if (i > 0) {
          GRN_TEXT_PUTS(ctx, expanded_term, " OR ");
        }
        if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, '('); }
        length = grn_vector_get_element(ctx, &values, i, &value, NULL, NULL);
        GRN_TEXT_PUT(ctx, expanded_term, value, length);
        if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, ')'); }
      }
      if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, ')'); }
      GRN_OBJ_FIN(ctx, &values);
    } else {
      grn_obj_get_value(ctx, column, id, expanded_term);
    }
    rc = GRN_SUCCESS;
  }
  return rc;
}

static grn_rc
substitute_terms(grn_ctx *ctx, const char *query, unsigned int query_len,
                 grn_expr_flags flags,
                 grn_obj *expanded_query,
                 grn_substitute_term_func substitute_term_func,
                 grn_user_data *user_data)
{
  grn_obj buf;
  unsigned int len;
  const char *start, *cur = query, *query_end = query + (size_t)query_len;
  GRN_TEXT_INIT(&buf, 0);
  for (;;) {
    while (cur < query_end && grn_isspace(cur, ctx->encoding)) {
      if (!(len = grn_charlen(ctx, cur, query_end))) { goto exit; }
      GRN_TEXT_PUT(ctx, expanded_query, cur, len);
      cur += len;
    }
    if (query_end <= cur) { break; }
    switch (*cur) {
    case '\0' :
      goto exit;
      break;
    case GRN_QUERY_AND :
    case GRN_QUERY_ADJ_INC :
    case GRN_QUERY_ADJ_DEC :
    case GRN_QUERY_ADJ_NEG :
    case GRN_QUERY_AND_NOT :
    case GRN_QUERY_PARENL :
    case GRN_QUERY_PARENR :
    case GRN_QUERY_PREFIX :
      GRN_TEXT_PUTC(ctx, expanded_query, *cur);
      cur++;
      break;
    case GRN_QUERY_QUOTEL :
      GRN_BULK_REWIND(&buf);
      for (start = cur++; cur < query_end; cur += len) {
        if (!(len = grn_charlen(ctx, cur, query_end))) {
          goto exit;
        } else if (len == 1) {
          if (*cur == GRN_QUERY_QUOTER) {
            cur++;
            break;
          } else if (cur + 1 < query_end && *cur == GRN_QUERY_ESCAPE) {
            cur++;
            len = grn_charlen(ctx, cur, query_end);
          }
        }
        GRN_TEXT_PUT(ctx, &buf, cur, len);
      }
      if (substitute_term_func(ctx, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf),
                               expanded_query, user_data)) {
        GRN_TEXT_PUT(ctx, expanded_query, start, cur - start);
      }
      break;
    case 'O' :
      if (cur + 2 <= query_end && cur[1] == 'R' &&
          (cur + 2 == query_end || grn_isspace(cur + 2, ctx->encoding))) {
        GRN_TEXT_PUT(ctx, expanded_query, cur, 2);
        cur += 2;
        break;
      }
      /* fallthru */
    default :
      for (start = cur; cur < query_end; cur += len) {
        if (!(len = grn_charlen(ctx, cur, query_end))) {
          goto exit;
        } else if (grn_isspace(cur, ctx->encoding)) {
          break;
        } else if (len == 1) {
          if (*cur == GRN_QUERY_PARENL ||
              *cur == GRN_QUERY_PARENR ||
              *cur == GRN_QUERY_PREFIX) {
            break;
          } else if (flags & GRN_EXPR_ALLOW_COLUMN && *cur == GRN_QUERY_COLUMN) {
            if (cur + 1 < query_end) {
              switch (cur[1]) {
              case '!' :
              case '@' :
              case '^' :
              case '$' :
                cur += 2;
                break;
              case '=' :
                cur += (flags & GRN_EXPR_ALLOW_UPDATE) ? 2 : 1;
                break;
              case '<' :
              case '>' :
                cur += (cur + 2 < query_end && cur[2] == '=') ? 3 : 2;
                break;
              default :
                cur += 1;
                break;
              }
            } else {
              cur += 1;
            }
            GRN_TEXT_PUT(ctx, expanded_query, start, cur - start);
            start = cur;
            break;
          }
        }
      }
      if (start < cur) {
        if (substitute_term_func(ctx, start, cur - start,
                                 expanded_query, user_data)) {
          GRN_TEXT_PUT(ctx, expanded_query, start, cur - start);
        }
      }
      break;
    }
  }
exit :
  GRN_OBJ_FIN(ctx, &buf);
  return GRN_SUCCESS;
}

static grn_rc
expand_query(grn_ctx *ctx, const char *query, unsigned int query_len,
             grn_expr_flags flags,
             const char *query_expander_name,
             unsigned int query_expander_name_len,
             grn_obj *expanded_query)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *query_expander;

  query_expander = grn_ctx_get(ctx,
                               query_expander_name, query_expander_name_len);
  if (!query_expander) {
    ERR(GRN_INVALID_ARGUMENT,
        "nonexistent query expansion column: <%.*s>",
        query_expander_name_len, query_expander_name);
    return GRN_INVALID_ARGUMENT;
  }

  switch (query_expander->header.type) {
  case GRN_PROC :
    if (((grn_proc *)query_expander)->type == GRN_PROC_FUNCTION) {
      grn_user_data user_data;
      user_data.ptr = query_expander;
      substitute_terms(ctx, query, query_len, flags, expanded_query,
                       substitute_term_by_func, &user_data);
    } else {
      rc = GRN_INVALID_ARGUMENT;
      ERR(rc,
          "[expand-query] must be function proc: <%.*s>",
          query_expander_name_len, query_expander_name);
    }
    break;
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
    {
      grn_obj *query_expansion_table;
      query_expansion_table = grn_column_table(ctx, query_expander);
      if (query_expansion_table) {
        grn_user_data user_data;
        grn_substitute_term_by_column_data data;
        user_data.ptr = &data;
        data.table = query_expansion_table;
        data.column = query_expander;
        substitute_terms(ctx, query, query_len, flags, expanded_query,
                         substitute_term_by_column, &user_data);
        grn_obj_unlink(ctx, query_expansion_table);
      } else {
        rc = GRN_INVALID_ARGUMENT;
        ERR(rc,
            "[expand-query] failed to get table of column: <%.*s>",
            query_expander_name_len, query_expander_name);
      }
    }
    break;
  default :
    rc = GRN_INVALID_ARGUMENT;
    {
      grn_obj type_name;
      GRN_TEXT_INIT(&type_name, 0);
      grn_inspect_type(ctx, &type_name, query_expander->header.type);
      ERR(rc,
          "[expand-query] must be a column or function proc: <%.*s>(%.*s)",
          query_expander_name_len, query_expander_name,
          (int)GRN_TEXT_LEN(&type_name), GRN_TEXT_VALUE(&type_name));
      GRN_OBJ_FIN(ctx, &type_name);
    }
    break;
  }
  grn_obj_unlink(ctx, query_expander);

  return rc;
}


/**** procs ****/

#define DEFAULT_LIMIT           10
#define DEFAULT_OUTPUT_COLUMNS  "_id, _key, *"
#define DEFAULT_DRILLDOWN_LIMIT           10
#define DEFAULT_DRILLDOWN_OUTPUT_COLUMNS  "_key, _nsubrecs"
#define DUMP_COLUMNS            "_id, _key, _value, *"

static grn_expr_flags
grn_parse_query_flags(grn_ctx *ctx, const char *query_flags,
                      unsigned int query_flags_len)
{
  grn_expr_flags flags = 0;
  const char *query_flags_end = query_flags + query_flags_len;

  while (query_flags < query_flags_end) {
    if (*query_flags == '|' || *query_flags == ' ') {
      query_flags += 1;
      continue;
    }

#define CHECK_EXPR_FLAG(name)\
  if (((query_flags_end - query_flags) >= (sizeof(#name) - 1)) &&\
      (!memcmp(query_flags, #name, sizeof(#name) - 1))) {\
    flags |= GRN_EXPR_ ## name;\
    query_flags += sizeof(#name) - 1;\
    continue;\
  }

    CHECK_EXPR_FLAG(ALLOW_PRAGMA);
    CHECK_EXPR_FLAG(ALLOW_COLUMN);
    CHECK_EXPR_FLAG(ALLOW_UPDATE);
    CHECK_EXPR_FLAG(ALLOW_LEADING_NOT);

#define GRN_EXPR_NONE 0
    CHECK_EXPR_FLAG(NONE);
#undef GNR_EXPR_NONE

    ERR(GRN_INVALID_ARGUMENT, "invalid query flag: <%.*s>",
        (int)(query_flags_end - query_flags), query_flags);
    return 0;
#undef CHECK_EXPR_FLAG
  }

  return flags;
}

static int
grn_select_apply_adjuster_ensure_factor(grn_ctx *ctx, grn_obj *factor_object)
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
grn_select_apply_adjuster_adjust(grn_ctx *ctx, grn_obj *table, grn_obj *res,
                                 grn_obj *column, grn_obj *value,
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
    ERR(GRN_INVALID_ARGUMENT,
        "adjuster requires index column for the target column: <%.*s>",
        column_name_size, column_name);
    return;
  }

  factor_value = grn_select_apply_adjuster_ensure_factor(ctx, factor);

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

    grn_obj_search(ctx, index, value, res, GRN_OP_ADJUST, &options);
  }
}

static void
grn_select_apply_adjuster(grn_ctx *ctx, grn_obj *table, grn_obj *res,
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
    grn_select_apply_adjuster_adjust(ctx, table, res, column, value, factor);
  }
}

static void
grn_select_output_columns(grn_ctx *ctx, grn_obj *res,
                          int n_hits, int offset, int limit,
                          const char *columns, int columns_len,
                          grn_obj *condition)
{
  grn_rc rc;
  grn_obj_format format;

  GRN_OBJ_FORMAT_INIT(&format, n_hits, offset, limit, offset);
  format.flags =
    GRN_OBJ_FORMAT_WITH_COLUMN_NAMES|
    GRN_OBJ_FORMAT_XML_ELEMENT_RESULTSET;
  rc = grn_output_format_set_columns(ctx, &format, res, columns, columns_len);
  /* TODO: check rc */
  if (format.expression) {
    grn_obj *condition_ptr;
    condition_ptr =
      grn_expr_get_or_add_var(ctx, format.expression,
                              GRN_SELECT_INTERNAL_VAR_CONDITION,
                              strlen(GRN_SELECT_INTERNAL_VAR_CONDITION));
    GRN_PTR_INIT(condition_ptr, 0, GRN_DB_OBJECT);
    GRN_PTR_SET(ctx, condition_ptr, condition);
  }
  GRN_OUTPUT_OBJ(res, &format);
  GRN_OBJ_FORMAT_FIN(ctx, &format);
}

typedef struct {
  const char *label;
  unsigned int label_len;
  const char *keys;
  unsigned int keys_len;
  const char *sortby;
  unsigned int sortby_len;
  const char *output_columns;
  unsigned int output_columns_len;
  int offset;
  int limit;
  grn_table_group_flags calc_types;
  const char *calc_target_name;
  unsigned int calc_target_name_len;
} drilldown_info;

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
  if (((calc_types_end - calc_types) >= (sizeof(#name) - 1)) &&\
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

    ERR(GRN_INVALID_ARGUMENT, "invalid table group calc type: <%.*s>",
        (int)(calc_types_end - calc_types), calc_types);
    return 0;
#undef CHECK_TABLE_GROUP_CALC_TYPE
  }

  return flags;
}

static void
drilldown_info_fill(grn_ctx *ctx,
                    drilldown_info *drilldown,
                    grn_obj *keys,
                    grn_obj *sortby,
                    grn_obj *output_columns,
                    grn_obj *offset,
                    grn_obj *limit,
                    grn_obj *calc_types,
                    grn_obj *calc_target)
{
  if (keys) {
    drilldown->keys = GRN_TEXT_VALUE(keys);
    drilldown->keys_len = GRN_TEXT_LEN(keys);
  } else {
    drilldown->keys = NULL;
    drilldown->keys_len = 0;
  }

  if (sortby) {
    drilldown->sortby = GRN_TEXT_VALUE(sortby);
    drilldown->sortby_len = GRN_TEXT_LEN(sortby);
  } else {
    drilldown->sortby = NULL;
    drilldown->sortby_len = 0;
  }

  if (output_columns) {
    drilldown->output_columns = GRN_TEXT_VALUE(output_columns);
    drilldown->output_columns_len = GRN_TEXT_LEN(output_columns);
  } else {
    drilldown->output_columns = NULL;
    drilldown->output_columns_len = 0;
  }
  if (!drilldown->output_columns_len) {
    drilldown->output_columns = DEFAULT_DRILLDOWN_OUTPUT_COLUMNS;
    drilldown->output_columns_len = strlen(DEFAULT_DRILLDOWN_OUTPUT_COLUMNS);
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

  if (calc_target && GRN_TEXT_LEN(calc_target)) {
    drilldown->calc_target_name = GRN_TEXT_VALUE(calc_target);
    drilldown->calc_target_name_len = GRN_TEXT_LEN(calc_target);
  } else {
    drilldown->calc_target_name = NULL;
    drilldown->calc_target_name_len = 0;
  }
}

static void
grn_select_drilldown(grn_ctx *ctx, grn_obj *table,
                     grn_table_sort_key *keys, uint32_t n_keys,
                     drilldown_info *drilldown)
{
  uint32_t i;
  for (i = 0; i < n_keys; i++) {
    grn_table_group_result g = {NULL, 0, 0, 1, GRN_TABLE_GROUP_CALC_COUNT, 0};
    uint32_t n_hits;
    int offset;
    int limit;

    if (drilldown->calc_target_name) {
      g.calc_target = grn_obj_column(ctx, table,
                                     drilldown->calc_target_name,
                                     drilldown->calc_target_name_len);
    }
    if (g.calc_target) {
      g.flags |= drilldown->calc_types;
    }

    grn_table_group(ctx, table, &keys[i], 1, &g, 1);
    if (ctx->rc != GRN_SUCCESS) {
      break;
    }
    n_hits = grn_table_size(ctx, g.table);

    offset = drilldown->offset;
    limit = drilldown->limit;
    grn_normalize_offset_and_limit(ctx, n_hits, &offset, &limit);

    if (drilldown->sortby_len) {
      grn_table_sort_key *sort_keys;
      uint32_t n_sort_keys;
      sort_keys = grn_table_sort_key_from_str(ctx,
                                              drilldown->sortby,
                                              drilldown->sortby_len,
                                              g.table, &n_sort_keys);
      if (sort_keys) {
        grn_obj *sorted;
        sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY,
                                  NULL, g.table);
        if (sorted) {
          grn_obj_format format;
          grn_table_sort(ctx, g.table, offset, limit,
                         sorted, sort_keys, n_sort_keys);
          GRN_OBJ_FORMAT_INIT(&format, n_hits, 0, limit, offset);
          format.flags =
            GRN_OBJ_FORMAT_WITH_COLUMN_NAMES|
            GRN_OBJ_FORMAT_XML_ELEMENT_NAVIGATIONENTRY;
          grn_obj_columns(ctx, sorted,
                          drilldown->output_columns,
                          drilldown->output_columns_len,
                          &format.columns);
          GRN_OUTPUT_OBJ(sorted, &format);
          GRN_OBJ_FORMAT_FIN(ctx, &format);
          grn_obj_unlink(ctx, sorted);
        }
        grn_table_sort_key_close(ctx, sort_keys, n_sort_keys);
      }
    } else {
      grn_obj_format format;
      GRN_OBJ_FORMAT_INIT(&format, n_hits, offset, limit, offset);
      format.flags =
        GRN_OBJ_FORMAT_WITH_COLUMN_NAMES|
        GRN_OBJ_FORMAT_XML_ELEMENT_NAVIGATIONENTRY;
      grn_obj_columns(ctx, g.table,
                      drilldown->output_columns,
                      drilldown->output_columns_len,
                      &format.columns);
      GRN_OUTPUT_OBJ(g.table, &format);
      GRN_OBJ_FORMAT_FIN(ctx, &format);
    }
    grn_obj_unlink(ctx, g.table);
    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                  ":", "drilldown(%d)", n_hits);
  }
}

static void
grn_select_drilldowns(grn_ctx *ctx, grn_obj *table,
                      drilldown_info *drilldowns, unsigned int n_drilldowns,
                      grn_obj *condition)
{
  unsigned int i;

  /* TODO: Remove invalid key drilldowns from the count. */
  GRN_OUTPUT_MAP_OPEN("DRILLDOWNS", n_drilldowns);
  for (i = 0; i < n_drilldowns; i++) {
    drilldown_info *drilldown = &(drilldowns[i]);
    grn_table_sort_key *keys = NULL;
    unsigned int n_keys;
    uint32_t n_hits;
    int offset;
    int limit;
    grn_table_group_result result = {
      NULL, 0, 0, 1, GRN_TABLE_GROUP_CALC_COUNT, 0, 0, NULL
    };

    keys = grn_table_sort_key_from_str(ctx,
                                       drilldown->keys,
                                       drilldown->keys_len,
                                       table, &n_keys);
    if (!keys) {
      continue;
    }

    GRN_OUTPUT_STR(drilldown->label, drilldown->label_len);

    result.key_begin = 0;
    result.key_end = n_keys - 1;
    if (n_keys > 1) {
      result.max_n_subrecs = 1;
    }
    if (drilldown->calc_target_name) {
      result.calc_target = grn_obj_column(ctx, table,
                                          drilldown->calc_target_name,
                                          drilldown->calc_target_name_len);
    }
    if (result.calc_target) {
      result.flags |= drilldown->calc_types;
    }

    grn_table_group(ctx, table, keys, n_keys, &result, 1);
    n_hits = grn_table_size(ctx, result.table);

    offset = drilldown->offset;
    limit = drilldown->limit;
    grn_normalize_offset_and_limit(ctx, n_hits, &offset, &limit);

    if (drilldown->sortby_len) {
      grn_table_sort_key *sort_keys;
      uint32_t n_sort_keys;
      sort_keys = grn_table_sort_key_from_str(ctx,
                                              drilldown->sortby,
                                              drilldown->sortby_len,
                                              result.table, &n_sort_keys);
      if (sort_keys) {
        grn_obj *sorted;
        sorted = grn_table_create(ctx, NULL, 0, NULL, GRN_OBJ_TABLE_NO_KEY,
                                  NULL, result.table);
        if (sorted) {
          grn_table_sort(ctx, result.table, offset, limit,
                         sorted, sort_keys, n_sort_keys);
          grn_select_output_columns(ctx, sorted, n_hits, 0, limit,
                                    drilldown->output_columns,
                                    drilldown->output_columns_len,
                                    condition);
          grn_obj_unlink(ctx, sorted);
        }
        grn_table_sort_key_close(ctx, sort_keys, n_sort_keys);
      }
    } else {
      grn_select_output_columns(ctx, result.table, n_hits, offset, limit,
                                drilldown->output_columns,
                                drilldown->output_columns_len,
                                condition);
    }

    grn_table_sort_key_close(ctx, keys, n_keys);
    if (result.calc_target) {
      grn_obj_unlink(ctx, result.calc_target);
    }
    grn_obj_unlink(ctx, result.table);

    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                  ":", "drilldown(%d)[%.*s]", n_hits,
                  (int)(drilldown->label_len), drilldown->label);
  }
  GRN_OUTPUT_MAP_CLOSE();
}

static grn_rc
grn_select(grn_ctx *ctx, const char *table, unsigned int table_len,
           const char *match_columns, unsigned int match_columns_len,
           const char *query, unsigned int query_len,
           const char *filter, unsigned int filter_len,
           const char *scorer, unsigned int scorer_len,
           const char *sortby, unsigned int sortby_len,
           const char *output_columns, unsigned int output_columns_len,
           int offset, int limit,
           drilldown_info *drilldowns,
           unsigned int n_drilldowns,
           const char *cache, unsigned int cache_len,
           const char *match_escalation_threshold, unsigned int match_escalation_threshold_len,
           const char *query_expander, unsigned int query_expander_len,
           const char *query_flags, unsigned int query_flags_len,
           const char *adjuster, unsigned int adjuster_len)
{
  uint32_t nkeys, nhits;
  uint16_t cacheable = 1, taintable = 0;
  grn_table_sort_key *keys;
  grn_obj *outbuf = ctx->impl->outbuf;
  grn_content_type output_type = ctx->impl->output_type;
  grn_obj *table_, *match_columns_ = NULL, *cond = NULL, *scorer_, *res = NULL, *sorted;
  char cache_key[GRN_TABLE_MAX_KEY_SIZE];
  uint32_t cache_key_size;
  long long int threshold, original_threshold = 0;
  grn_cache *cache_obj = grn_cache_current_get(ctx);

  cache_key_size = table_len + 1 + match_columns_len + 1 + query_len + 1 +
    filter_len + 1 + scorer_len + 1 + sortby_len + 1 + output_columns_len + 1 +
    match_escalation_threshold_len + 1 +
    query_expander_len + 1 + query_flags_len + 1 + adjuster_len + 1 +
    sizeof(grn_content_type) + sizeof(int) * 2;
  {
    unsigned int i;
    for (i = 0; i < n_drilldowns; i++) {
      drilldown_info *drilldown = &(drilldowns[i]);
      cache_key_size +=
        drilldown->keys_len + 1 +
        drilldown->sortby_len + 1 +
        drilldown->output_columns_len + 1 +
        sizeof(int) * 2;
    }
  }
  if (cache_key_size <= GRN_TABLE_MAX_KEY_SIZE) {
    grn_obj *cache_value;
    char *cp = cache_key;
    memcpy(cp, table, table_len);
    cp += table_len; *cp++ = '\0';
    memcpy(cp, match_columns, match_columns_len);
    cp += match_columns_len; *cp++ = '\0';
    memcpy(cp, query, query_len);
    cp += query_len; *cp++ = '\0';
    memcpy(cp, filter, filter_len);
    cp += filter_len; *cp++ = '\0';
    memcpy(cp, scorer, scorer_len);
    cp += scorer_len; *cp++ = '\0';
    memcpy(cp, sortby, sortby_len);
    cp += sortby_len; *cp++ = '\0';
    memcpy(cp, output_columns, output_columns_len);
    cp += output_columns_len; *cp++ = '\0';
    {
      unsigned int i;
      for (i = 0; i < n_drilldowns; i++) {
        drilldown_info *drilldown = &(drilldowns[i]);
        memcpy(cp, drilldown->keys, drilldown->keys_len);
        cp += drilldown->keys_len; *cp++ = '\0';
        memcpy(cp, drilldown->sortby, drilldown->sortby_len);
        cp += drilldown->sortby_len; *cp++ = '\0';
        memcpy(cp, drilldown->output_columns, drilldown->output_columns_len);
        cp += drilldown->output_columns_len; *cp++ = '\0';
      }
    }
    memcpy(cp, match_escalation_threshold, match_escalation_threshold_len);
    cp += match_escalation_threshold_len; *cp++ = '\0';
    memcpy(cp, query_expander, query_expander_len);
    cp += query_expander_len; *cp++ = '\0';
    memcpy(cp, query_flags, query_flags_len);
    cp += query_flags_len; *cp++ = '\0';
    memcpy(cp, adjuster, adjuster_len);
    cp += adjuster_len; *cp++ = '\0';
    memcpy(cp, &output_type, sizeof(grn_content_type)); cp += sizeof(grn_content_type);
    memcpy(cp, &offset, sizeof(int)); cp += sizeof(int);
    memcpy(cp, &limit, sizeof(int)); cp += sizeof(int);
    {
      unsigned int i;
      for (i = 0; i < n_drilldowns; i++) {
        drilldown_info *drilldown = &(drilldowns[i]);
        memcpy(cp, &(drilldown->offset), sizeof(int)); cp += sizeof(int);
        memcpy(cp, &(drilldown->limit), sizeof(int)); cp += sizeof(int);
      }
    }
    cache_value = grn_cache_fetch(ctx, cache_obj, cache_key, cache_key_size);
    if (cache_value) {
      GRN_TEXT_PUT(ctx, outbuf,
                   GRN_TEXT_VALUE(cache_value),
                   GRN_TEXT_LEN(cache_value));
      grn_cache_unref(ctx, cache_obj, cache_key, cache_key_size);
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_CACHE,
                    ":", "cache(%" GRN_FMT_LLD ")",
                    (long long int)GRN_TEXT_LEN(cache_value));
      return ctx->rc;
    }
  }
  if (match_escalation_threshold_len) {
    const char *end, *rest;
    original_threshold = grn_ctx_get_match_escalation_threshold(ctx);
    end = match_escalation_threshold + match_escalation_threshold_len;
    threshold = grn_atoll(match_escalation_threshold, end, &rest);
    if (end == rest) {
      grn_ctx_set_match_escalation_threshold(ctx, threshold);
    }
  }
  if ((table_ = grn_ctx_get(ctx, table, table_len))) {
    // match_columns_ = grn_obj_column(ctx, table_, match_columns, match_columns_len);
    if (query_len || filter_len) {
      grn_obj *v;
      GRN_EXPR_CREATE_FOR_QUERY(ctx, table_, cond, v);
      if (cond) {
        if (match_columns_len) {
          GRN_EXPR_CREATE_FOR_QUERY(ctx, table_, match_columns_, v);
          if (match_columns_) {
            grn_expr_parse(ctx, match_columns_, match_columns, match_columns_len,
                           NULL, GRN_OP_MATCH, GRN_OP_AND,
                           GRN_EXPR_SYNTAX_SCRIPT);
          } else {
            /* todo */
          }
        }
        if (query_len) {
          grn_expr_flags flags;
          grn_obj query_expander_buf;
          GRN_TEXT_INIT(&query_expander_buf, 0);
          flags = GRN_EXPR_SYNTAX_QUERY;
          if (query_flags_len) {
            flags |= grn_parse_query_flags(ctx, query_flags, query_flags_len);
          } else {
            flags |= GRN_EXPR_ALLOW_PRAGMA|GRN_EXPR_ALLOW_COLUMN;
            if (ctx->rc) {
              goto exit;
            }
          }
          if (query_expander_len) {
            if (expand_query(ctx, query, query_len, flags,
                             query_expander, query_expander_len,
                             &query_expander_buf) == GRN_SUCCESS) {
              query = GRN_TEXT_VALUE(&query_expander_buf);
              query_len = GRN_TEXT_LEN(&query_expander_buf);
            } else {
              GRN_OBJ_FIN(ctx, &query_expander_buf);
              goto exit;
            }
          }
          grn_expr_parse(ctx, cond, query, query_len,
                         match_columns_, GRN_OP_MATCH, GRN_OP_AND, flags);
          GRN_OBJ_FIN(ctx, &query_expander_buf);
          if (!ctx->rc && filter_len) {
            grn_expr_parse(ctx, cond, filter, filter_len,
                           match_columns_, GRN_OP_MATCH, GRN_OP_AND,
                           GRN_EXPR_SYNTAX_SCRIPT);
            if (!ctx->rc) { grn_expr_append_op(ctx, cond, GRN_OP_AND, 2); }
          }
        } else {
          grn_expr_parse(ctx, cond, filter, filter_len,
                         match_columns_, GRN_OP_MATCH, GRN_OP_AND,
                         GRN_EXPR_SYNTAX_SCRIPT);
        }
        cacheable *= ((grn_expr *)cond)->cacheable;
        taintable += ((grn_expr *)cond)->taintable;
        /*
        grn_obj strbuf;
        GRN_TEXT_INIT(&strbuf, 0);
        grn_expr_inspect(ctx, &strbuf, cond);
        GRN_TEXT_PUTC(ctx, &strbuf, '\0');
        GRN_LOG(ctx, GRN_LOG_NOTICE, "query=(%s)", GRN_TEXT_VALUE(&strbuf));
        GRN_OBJ_FIN(ctx, &strbuf);
        */
        if (!ctx->rc) { res = grn_table_select(ctx, table_, cond, NULL, GRN_OP_OR); }
      } else {
        /* todo */
        ERRCLR(ctx);
      }
    } else {
      res = table_;
    }
    nhits = res ? grn_table_size(ctx, res) : 0;
    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                  ":", "select(%d)", nhits);

    if (res) {
      uint32_t ngkeys;
      grn_table_sort_key *gkeys = NULL;
      int result_size = 1;
      if (!ctx->rc && n_drilldowns > 0) {
        if (n_drilldowns == 1 && !drilldowns[0].label) {
          gkeys = grn_table_sort_key_from_str(ctx,
                                              drilldowns[0].keys,
                                              drilldowns[0].keys_len,
                                              res, &ngkeys);
          if (gkeys) {
            result_size += ngkeys;
          }
        } else {
          result_size += 1;
        }
      }

      if (adjuster && adjuster_len) {
        grn_obj *adjuster_;
        grn_obj *v;
        GRN_EXPR_CREATE_FOR_QUERY(ctx, table_, adjuster_, v);
        if (adjuster_ && v) {
          grn_rc rc;
          rc = grn_expr_parse(ctx, adjuster_, adjuster, adjuster_len, NULL,
                              GRN_OP_MATCH, GRN_OP_ADJUST,
                              GRN_EXPR_SYNTAX_ADJUSTER);
          if (rc) {
            grn_obj_unlink(ctx, adjuster_);
            goto exit;
          }
          cacheable *= ((grn_expr *)adjuster_)->cacheable;
          taintable += ((grn_expr *)adjuster_)->taintable;
          grn_select_apply_adjuster(ctx, table_, res, adjuster_);
          grn_obj_unlink(ctx, adjuster_);
        }
        GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                      ":", "adjust(%d)", nhits);
      }

      if (scorer && scorer_len) {
        grn_obj *v;
        GRN_EXPR_CREATE_FOR_QUERY(ctx, res, scorer_, v);
        if (scorer_ && v) {
          grn_table_cursor *tc;
          grn_expr_parse(ctx, scorer_, scorer, scorer_len, NULL, GRN_OP_MATCH, GRN_OP_AND,
                         GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
          cacheable *= ((grn_expr *)scorer_)->cacheable;
          taintable += ((grn_expr *)scorer_)->taintable;
          if ((tc = grn_table_cursor_open(ctx, res, NULL, 0, NULL, 0, 0, -1, 0))) {
            grn_id id;
            while ((id = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL) {
              GRN_RECORD_SET(ctx, v, id);
              grn_expr_exec(ctx, scorer_, 0);
              if (ctx->rc) {
                break;
              }
            }
            grn_table_cursor_close(ctx, tc);
          }
          grn_obj_unlink(ctx, scorer_);
        }
        GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                      ":", "score(%d)", nhits);
      }

      GRN_OUTPUT_ARRAY_OPEN("RESULT", result_size);

      grn_normalize_offset_and_limit(ctx, nhits, &offset, &limit);

      if (sortby_len &&
          (keys = grn_table_sort_key_from_str(ctx, sortby, sortby_len, res, &nkeys))) {
        if ((sorted = grn_table_create(ctx, NULL, 0, NULL,
                                       GRN_OBJ_TABLE_NO_KEY, NULL, res))) {
          grn_table_sort(ctx, res, offset, limit, sorted, keys, nkeys);
          GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                        ":", "sort(%d)", limit);
          grn_select_output_columns(ctx, sorted, nhits, 0, limit,
                                    output_columns, output_columns_len, cond);
          grn_obj_unlink(ctx, sorted);
        }
        grn_table_sort_key_close(ctx, keys, nkeys);
      } else {
        if (!ctx->rc) {
          grn_select_output_columns(ctx, res, nhits, offset, limit,
                                    output_columns, output_columns_len, cond);
        }
      }
      GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                    ":", "output(%d)", limit);
      if (!ctx->rc) {
        if (gkeys) {
          drilldown_info *drilldown = &(drilldowns[0]);
          grn_select_drilldown(ctx, res, gkeys, ngkeys, drilldown);
        } else if (n_drilldowns > 0) {
          grn_select_drilldowns(ctx, res, drilldowns, n_drilldowns, cond);
        }
      }
      if (gkeys) {
        grn_table_sort_key_close(ctx, gkeys, ngkeys);
      }
      if (res != table_) { grn_obj_unlink(ctx, res); }
    } else {
      GRN_OUTPUT_ARRAY_OPEN("RESULT", 0);
    }
    GRN_OUTPUT_ARRAY_CLOSE();
    if (!ctx->rc && cacheable && cache_key_size <= GRN_TABLE_MAX_KEY_SIZE
        && (!cache || cache_len != 2 || *cache != 'n' || *(cache + 1) != 'o')) {
      grn_cache_update(ctx, cache_obj, cache_key, cache_key_size, outbuf);
    }
    if (taintable) { grn_db_touch(ctx, DB_OBJ(table_)->db); }
    grn_obj_unlink(ctx, table_);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid table name: <%.*s>", table_len, table);
  }
exit :
  if (match_escalation_threshold_len) {
    grn_ctx_set_match_escalation_threshold(ctx, original_threshold);
  }
  if (match_columns_) {
    grn_obj_unlink(ctx, match_columns_);
  }
  if (cond) {
    grn_obj_unlink(ctx, cond);
  }
  /* GRN_LOG(ctx, GRN_LOG_NONE, "%d", ctx->seqno); */
  return ctx->rc;
}

static void
proc_select_find_all_drilldown_labels(grn_ctx *ctx, grn_user_data *user_data,
                                      grn_obj *labels)
{
  grn_obj *vars = GRN_PROC_GET_VARS();
  grn_table_cursor *cursor;
  cursor = grn_table_cursor_open(ctx, vars, NULL, 0, NULL, 0, 0, -1, 0);
  if (cursor) {
    const char *prefix = "drilldown[";
    int prefix_len = strlen(prefix);
    const char *suffix = "].keys";
    int suffix_len = strlen(suffix);
    while (grn_table_cursor_next(ctx, cursor)) {
      void *key;
      char *name;
      int name_len;
      name_len = grn_table_cursor_get_key(ctx, cursor, &key);
      name = key;
      if (name_len < (prefix_len + 1 + suffix_len)) {
        continue;
      }
      if (strncmp(prefix, name, prefix_len) != 0) {
        continue;
      }
      if (strncmp(suffix, name + name_len - suffix_len, suffix_len) != 0) {
        continue;
      }
      grn_vector_add_element(ctx, labels,
                             name + prefix_len,
                             name_len - prefix_len - suffix_len,
                             0, GRN_ID_NIL);
    }
    grn_table_cursor_close(ctx, cursor);
  }
}

static grn_obj *
proc_select(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
#define MAX_N_DRILLDOWNS 10
  int offset = GRN_TEXT_LEN(VAR(7))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(7)), GRN_BULK_CURR(VAR(7)), NULL)
    : 0;
  int limit = GRN_TEXT_LEN(VAR(8))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(8)), GRN_BULK_CURR(VAR(8)), NULL)
    : DEFAULT_LIMIT;
  const char *output_columns = GRN_TEXT_VALUE(VAR(6));
  uint32_t output_columns_len = GRN_TEXT_LEN(VAR(6));
  drilldown_info drilldowns[MAX_N_DRILLDOWNS];
  unsigned int n_drilldowns = 0;
  grn_obj drilldown_labels;
  grn_obj *query_expansion = VAR(16);
  grn_obj *query_expander = VAR(18);
  grn_obj *adjuster = VAR(19);

  if (GRN_TEXT_LEN(query_expander) == 0 && GRN_TEXT_LEN(query_expansion) > 0) {
    query_expander = query_expansion;
  }

  if (!output_columns_len) {
    output_columns = DEFAULT_OUTPUT_COLUMNS;
    output_columns_len = strlen(DEFAULT_OUTPUT_COLUMNS);
  }

  GRN_TEXT_INIT(&drilldown_labels, GRN_OBJ_VECTOR);
  if (GRN_TEXT_LEN(VAR(9))) {
    drilldown_info *drilldown = &(drilldowns[0]);
    drilldown->label = NULL;
    drilldown->label_len = 0;
    drilldown_info_fill(ctx, drilldown,
                        VAR(9), VAR(10), VAR(11), VAR(12), VAR(13),
                        VAR(20), VAR(21));
    n_drilldowns++;
  } else {
    unsigned int i;
    proc_select_find_all_drilldown_labels(ctx, user_data, &drilldown_labels);
    n_drilldowns = grn_vector_size(ctx, &drilldown_labels);
    for (i = 0; i < n_drilldowns; i++) {
      drilldown_info *drilldown = &(drilldowns[i]);
      const char *label;
      int label_len;
      char key_name[GRN_TABLE_MAX_KEY_SIZE];
      grn_obj *keys;
      grn_obj *sortby;
      grn_obj *output_columns;
      grn_obj *offset;
      grn_obj *limit;
      grn_obj *calc_types;
      grn_obj *calc_target;

      label_len = grn_vector_get_element(ctx, &drilldown_labels, i,
                                         &label, NULL, NULL);
      drilldown->label = label;
      drilldown->label_len = label_len;

#define GET_VAR(name)\
      snprintf(key_name, GRN_TABLE_MAX_KEY_SIZE,\
               "drilldown[%.*s]." # name, label_len, label);\
      name = GRN_PROC_GET_VAR(key_name);

      GET_VAR(keys);
      GET_VAR(sortby);
      GET_VAR(output_columns);
      GET_VAR(offset);
      GET_VAR(limit);
      GET_VAR(calc_types);
      GET_VAR(calc_target);

#undef GET_VAR

      drilldown_info_fill(ctx, drilldown,
                          keys, sortby, output_columns, offset, limit,
                          calc_types, calc_target);
    }
  }
  if (grn_select(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)),
                 GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)),
                 GRN_TEXT_VALUE(VAR(2)), GRN_TEXT_LEN(VAR(2)),
                 GRN_TEXT_VALUE(VAR(3)), GRN_TEXT_LEN(VAR(3)),
                 GRN_TEXT_VALUE(VAR(4)), GRN_TEXT_LEN(VAR(4)),
                 GRN_TEXT_VALUE(VAR(5)), GRN_TEXT_LEN(VAR(5)),
                 output_columns, output_columns_len,
                 offset, limit,
                 drilldowns, n_drilldowns,
                 GRN_TEXT_VALUE(VAR(14)), GRN_TEXT_LEN(VAR(14)),
                 GRN_TEXT_VALUE(VAR(15)), GRN_TEXT_LEN(VAR(15)),
                 GRN_TEXT_VALUE(query_expander), GRN_TEXT_LEN(query_expander),
                 GRN_TEXT_VALUE(VAR(17)), GRN_TEXT_LEN(VAR(17)),
                 GRN_TEXT_VALUE(adjuster), GRN_TEXT_LEN(adjuster))) {
  }
  GRN_OBJ_FIN(ctx, &drilldown_labels);
#undef MAX_N_DRILLDOWNS

  return NULL;
}

static grn_obj *
proc_define_selector(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  uint32_t i, nvars;
  grn_expr_var *vars;
  grn_proc_get_info(ctx, user_data, &vars, &nvars, NULL);
  for (i = 1; i < nvars; i++) {
    GRN_TEXT_SET(ctx, &((vars + i)->value),
                 GRN_TEXT_VALUE(VAR(i)), GRN_TEXT_LEN(VAR(i)));
  }
  grn_proc_create(ctx,
                  GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)),
                  GRN_PROC_COMMAND, proc_select, NULL, NULL, nvars - 1, vars + 1);
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_load(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_load(ctx, grn_get_ctype(VAR(4)),
           GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)),
           GRN_TEXT_VALUE(VAR(2)), GRN_TEXT_LEN(VAR(2)),
           GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)),
           GRN_TEXT_VALUE(VAR(3)), GRN_TEXT_LEN(VAR(3)),
           GRN_TEXT_VALUE(VAR(5)), GRN_TEXT_LEN(VAR(5)));
  if (ctx->impl->loader.stat != GRN_LOADER_END) {
    grn_ctx_set_next_expr(ctx, grn_proc_get_info(ctx, user_data, NULL, NULL, NULL));
  } else {
    GRN_OUTPUT_INT64(ctx->impl->loader.nrecords);
    if (ctx->impl->loader.table) {
      grn_db_touch(ctx, DB_OBJ(ctx->impl->loader.table)->db);
    }
    /* maybe necessary : grn_ctx_loader_clear(ctx); */
  }
  return NULL;
}

static grn_obj *
proc_status(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_timeval now;
  grn_cache *cache;
  grn_cache_statistics statistics;

  grn_timeval_now(ctx, &now);
  cache = grn_cache_current_get(ctx);
  grn_cache_get_statistics(ctx, cache, &statistics);
  GRN_OUTPUT_MAP_OPEN("RESULT", 9);
  GRN_OUTPUT_CSTR("alloc_count");
  GRN_OUTPUT_INT32(grn_alloc_count());
  GRN_OUTPUT_CSTR("starttime");
  GRN_OUTPUT_INT32(grn_starttime.tv_sec);
  GRN_OUTPUT_CSTR("uptime");
  GRN_OUTPUT_INT32(now.tv_sec - grn_starttime.tv_sec);
  GRN_OUTPUT_CSTR("version");
  GRN_OUTPUT_CSTR(grn_get_version());
  GRN_OUTPUT_CSTR("n_queries");
  GRN_OUTPUT_INT64(statistics.nfetches);
  GRN_OUTPUT_CSTR("cache_hit_rate");
  if (statistics.nfetches == 0) {
    GRN_OUTPUT_FLOAT(0.0);
  } else {
    double cache_hit_rate;
    cache_hit_rate = (double)statistics.nhits / (double)statistics.nfetches;
    GRN_OUTPUT_FLOAT(cache_hit_rate * 100.0);
  }
  GRN_OUTPUT_CSTR("command_version");
  GRN_OUTPUT_INT32(grn_ctx_get_command_version(ctx));
  GRN_OUTPUT_CSTR("default_command_version");
  GRN_OUTPUT_INT32(grn_get_default_command_version());
  GRN_OUTPUT_CSTR("max_command_version");
  GRN_OUTPUT_INT32(GRN_COMMAND_VERSION_MAX);
  GRN_OUTPUT_MAP_CLOSE();
  return NULL;
}

static grn_obj_flags
grn_parse_table_create_flags(grn_ctx *ctx, const char *nptr, const char *end)
{
  grn_obj_flags flags = 0;
  while (nptr < end) {
    if (*nptr == '|' || *nptr == ' ') {
      nptr += 1;
      continue;
    }
    if (!memcmp(nptr, "TABLE_HASH_KEY", 14)) {
      flags |= GRN_OBJ_TABLE_HASH_KEY;
      nptr += 14;
    } else if (!memcmp(nptr, "TABLE_PAT_KEY", 13)) {
      flags |= GRN_OBJ_TABLE_PAT_KEY;
      nptr += 13;
    } else if (!memcmp(nptr, "TABLE_DAT_KEY", 13)) {
      flags |= GRN_OBJ_TABLE_DAT_KEY;
      nptr += 13;
    } else if (!memcmp(nptr, "TABLE_NO_KEY", 12)) {
      flags |= GRN_OBJ_TABLE_NO_KEY;
      nptr += 12;
    } else if (!memcmp(nptr, "KEY_NORMALIZE", 13)) {
      flags |= GRN_OBJ_KEY_NORMALIZE;
      nptr += 13;
    } else if (!memcmp(nptr, "KEY_WITH_SIS", 12)) {
      flags |= GRN_OBJ_KEY_WITH_SIS;
      nptr += 12;
    } else {
      ERR(GRN_INVALID_ARGUMENT, "invalid flags option: %.*s",
          (int)(end - nptr), nptr);
      return 0;
    }
  }
  return flags;
}

static grn_obj_flags
grn_parse_column_create_flags(grn_ctx *ctx, const char *nptr, const char *end)
{
  grn_obj_flags flags = 0;
  while (nptr < end) {
    if (*nptr == '|' || *nptr == ' ') {
      nptr += 1;
      continue;
    }
    if (!memcmp(nptr, "COLUMN_SCALAR", 13)) {
      flags |= GRN_OBJ_COLUMN_SCALAR;
      nptr += 13;
    } else if (!memcmp(nptr, "COLUMN_VECTOR", 13)) {
      flags |= GRN_OBJ_COLUMN_VECTOR;
      nptr += 13;
    } else if (!memcmp(nptr, "COLUMN_INDEX", 12)) {
      flags |= GRN_OBJ_COLUMN_INDEX;
      nptr += 12;
    } else if (!memcmp(nptr, "COMPRESS_ZLIB", 13)) {
      flags |= GRN_OBJ_COMPRESS_ZLIB;
      nptr += 13;
    } else if (!memcmp(nptr, "COMPRESS_LZ4", 12)) {
      flags |= GRN_OBJ_COMPRESS_LZ4;
      nptr += 12;
    } else if (!memcmp(nptr, "WITH_SECTION", 12)) {
      flags |= GRN_OBJ_WITH_SECTION;
      nptr += 12;
    } else if (!memcmp(nptr, "WITH_WEIGHT", 11)) {
      flags |= GRN_OBJ_WITH_WEIGHT;
      nptr += 11;
    } else if (!memcmp(nptr, "WITH_POSITION", 13)) {
      flags |= GRN_OBJ_WITH_POSITION;
      nptr += 13;
    } else if (!memcmp(nptr, "RING_BUFFER", 11)) {
      flags |= GRN_OBJ_RING_BUFFER;
      nptr += 11;
    } else {
      ERR(GRN_INVALID_ARGUMENT, "invalid flags option: %.*s",
          (int)(end - nptr), nptr);
      return 0;
    }
  }
  return flags;
}

static void
grn_table_create_flags_to_text(grn_ctx *ctx, grn_obj *buf, grn_obj_flags flags)
{
  GRN_BULK_REWIND(buf);
  switch (flags & GRN_OBJ_TABLE_TYPE_MASK) {
  case GRN_OBJ_TABLE_HASH_KEY:
    GRN_TEXT_PUTS(ctx, buf, "TABLE_HASH_KEY");
    break;
  case GRN_OBJ_TABLE_PAT_KEY:
    GRN_TEXT_PUTS(ctx, buf, "TABLE_PAT_KEY");
    break;
  case GRN_OBJ_TABLE_DAT_KEY:
    GRN_TEXT_PUTS(ctx, buf, "TABLE_DAT_KEY");
    break;
  case GRN_OBJ_TABLE_NO_KEY:
    GRN_TEXT_PUTS(ctx, buf, "TABLE_NO_KEY");
    break;
  }
  if (flags & GRN_OBJ_KEY_WITH_SIS) {
    GRN_TEXT_PUTS(ctx, buf, "|KEY_WITH_SIS");
  }
  if (flags & GRN_OBJ_KEY_NORMALIZE) {
    GRN_TEXT_PUTS(ctx, buf, "|KEY_NORMALIZE");
  }
  if (flags & GRN_OBJ_PERSISTENT) {
    GRN_TEXT_PUTS(ctx, buf, "|PERSISTENT");
  }
}

static void
grn_column_create_flags_to_text(grn_ctx *ctx, grn_obj *buf, grn_obj_flags flags)
{
  GRN_BULK_REWIND(buf);
  switch (flags & GRN_OBJ_COLUMN_TYPE_MASK) {
  case GRN_OBJ_COLUMN_SCALAR:
    GRN_TEXT_PUTS(ctx, buf, "COLUMN_SCALAR");
    break;
  case GRN_OBJ_COLUMN_VECTOR:
    GRN_TEXT_PUTS(ctx, buf, "COLUMN_VECTOR");
    if (flags & GRN_OBJ_WITH_WEIGHT) {
      GRN_TEXT_PUTS(ctx, buf, "|WITH_WEIGHT");
    }
    break;
  case GRN_OBJ_COLUMN_INDEX:
    GRN_TEXT_PUTS(ctx, buf, "COLUMN_INDEX");
    if (flags & GRN_OBJ_WITH_SECTION) {
      GRN_TEXT_PUTS(ctx, buf, "|WITH_SECTION");
    }
    if (flags & GRN_OBJ_WITH_WEIGHT) {
      GRN_TEXT_PUTS(ctx, buf, "|WITH_WEIGHT");
    }
    if (flags & GRN_OBJ_WITH_POSITION) {
      GRN_TEXT_PUTS(ctx, buf, "|WITH_POSITION");
    }
    break;
  }
  switch (flags & GRN_OBJ_COMPRESS_MASK) {
  case GRN_OBJ_COMPRESS_NONE:
    break;
  case GRN_OBJ_COMPRESS_ZLIB:
    GRN_TEXT_PUTS(ctx, buf, "|COMPRESS_ZLIB");
    break;
  case GRN_OBJ_COMPRESS_LZ4:
    GRN_TEXT_PUTS(ctx, buf, "|COMPRESS_LZ4");
    break;
  }
  if (flags & GRN_OBJ_PERSISTENT) {
    GRN_TEXT_PUTS(ctx, buf, "|PERSISTENT");
  }
}

static grn_bool
proc_table_create_set_token_filters_put(grn_ctx *ctx,
                                        grn_obj *token_filters,
                                        const char *token_filter_name,
                                        int token_filter_name_length)
{
  grn_obj *token_filter;

  token_filter = grn_ctx_get(ctx,
                             token_filter_name,
                             token_filter_name_length);
  if (token_filter) {
    GRN_PTR_PUT(ctx, token_filters, token_filter);
    return GRN_TRUE;
  } else {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create][token-filter] nonexistent token filter: <%.*s>",
        token_filter_name_length, token_filter_name);
    return GRN_FALSE;
  }
}

static grn_bool
proc_table_create_set_token_filters_fill(grn_ctx *ctx,
                                         grn_obj *token_filters,
                                         grn_obj *token_filter_names)
{
  const char *start, *current, *end;
  const char *name_start, *name_end;
  const char *last_name_end;

  start = GRN_TEXT_VALUE(token_filter_names);
  end = start + GRN_TEXT_LEN(token_filter_names);
  current = start;
  name_start = NULL;
  name_end = NULL;
  last_name_end = start;
  while (current < end) {
    switch (current[0]) {
    case ' ' :
      if (name_start && !name_end) {
        name_end = current;
      }
      break;
    case ',' :
      if (!name_start) {
        goto break_loop;
      }
      if (!name_end) {
        name_end = current;
      }
      proc_table_create_set_token_filters_put(ctx,
                                              token_filters,
                                              name_start,
                                              name_end - name_start);
      last_name_end = name_end + 1;
      name_start = NULL;
      name_end = NULL;
      break;
    default :
      if (!name_start) {
        name_start = current;
      }
      break;
    }
    current++;
  }

break_loop:
  if (!name_start) {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create][token-filter] empty token filter name: "
        "<%.*s|%.*s|%.*s>",
        (int)(last_name_end - start), start,
        (int)(current - last_name_end), last_name_end,
        (int)(end - current), current);
    return GRN_FALSE;
  }

  if (!name_end) {
    name_end = current;
  }
  proc_table_create_set_token_filters_put(ctx,
                                          token_filters,
                                          name_start,
                                          name_end - name_start);

  return GRN_TRUE;
}

static void
proc_table_create_set_token_filters(grn_ctx *ctx,
                                    grn_obj *table,
                                    grn_obj *token_filter_names)
{
  grn_obj token_filters;

  if (GRN_TEXT_LEN(token_filter_names) == 0) {
    return;
  }

  GRN_PTR_INIT(&token_filters, GRN_OBJ_VECTOR, 0);
  if (proc_table_create_set_token_filters_fill(ctx,
                                               &token_filters,
                                               token_filter_names)) {
    grn_obj_set_info(ctx, table, GRN_INFO_TOKEN_FILTERS, &token_filters);
  }
  grn_obj_unlink(ctx, &token_filters);
}

static grn_obj *
proc_table_create(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table;
  const char *rest;
  grn_obj_flags flags = grn_atoi(GRN_TEXT_VALUE(VAR(1)),
                                 GRN_BULK_CURR(VAR(1)), &rest);
  if (GRN_TEXT_VALUE(VAR(1)) == rest) {
    flags = grn_parse_table_create_flags(ctx, GRN_TEXT_VALUE(VAR(1)),
                                         GRN_BULK_CURR(VAR(1)));
    if (ctx->rc) { goto exit; }
  }
  if (GRN_TEXT_LEN(VAR(0))) {
    grn_obj *key_type = NULL, *value_type = NULL;
    if (GRN_TEXT_LEN(VAR(2)) > 0) {
      key_type = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(2)),
                             GRN_TEXT_LEN(VAR(2)));
      if (!key_type) {
        ERR(GRN_INVALID_ARGUMENT,
            "[table][create] key type doesn't exist: <%.*s> (%.*s)",
            (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
            (int)GRN_TEXT_LEN(VAR(2)), GRN_TEXT_VALUE(VAR(2)));
        return NULL;
      }
    }
    if (GRN_TEXT_LEN(VAR(3)) > 0) {
      value_type = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(3)),
                               GRN_TEXT_LEN(VAR(3)));
      if (!value_type) {
        ERR(GRN_INVALID_ARGUMENT,
            "[table][create] value type doesn't exist: <%.*s> (%.*s)",
            (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
            (int)GRN_TEXT_LEN(VAR(3)), GRN_TEXT_VALUE(VAR(3)));
        return NULL;
      }
    }
    flags |= GRN_OBJ_PERSISTENT;
    table = grn_table_create(ctx,
                             GRN_TEXT_VALUE(VAR(0)),
                             GRN_TEXT_LEN(VAR(0)),
                             NULL, flags,
                             key_type,
                             value_type);
    if (table) {
      grn_obj *normalizer_name;
      grn_obj_set_info(ctx, table,
                       GRN_INFO_DEFAULT_TOKENIZER,
                       grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(4)),
                                   GRN_TEXT_LEN(VAR(4))));
      normalizer_name = VAR(5);
      if (GRN_TEXT_LEN(normalizer_name) > 0) {
        grn_obj_set_info(ctx, table,
                         GRN_INFO_NORMALIZER,
                         grn_ctx_get(ctx,
                                     GRN_TEXT_VALUE(normalizer_name),
                                     GRN_TEXT_LEN(normalizer_name)));
      }
      proc_table_create_set_token_filters(ctx, table, VAR(6));
      grn_obj_unlink(ctx, table);
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT,
        "[table][create] should not create anonymous table");
  }
exit :
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_table_remove(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table;
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)),
                           GRN_TEXT_LEN(VAR(0)));
  if (table) {
    grn_obj_remove(ctx,table);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "table not found.");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_table_rename(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *table = NULL;
  if (GRN_TEXT_LEN(VAR(0)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc, "[table][rename] table name isn't specified");
    goto exit;
  }
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!table) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][rename] table isn't found: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(1)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][rename] new table name isn't specified: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    goto exit;
  }
  rc = grn_table_rename(ctx, table,
                        GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)));
  if (rc != GRN_SUCCESS && ctx->rc == GRN_SUCCESS) {
    ERR(rc,
        "[table][rename] failed to rename: <%.*s> -> <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
  }
exit :
  GRN_OUTPUT_BOOL(!rc);
  if (table) { grn_obj_unlink(ctx, table); }
  return NULL;
}

static grn_rc
proc_column_create_resolve_source_name(grn_ctx *ctx,
                                       grn_obj *table,
                                       const char *source_name,
                                       int source_name_length,
                                       grn_obj *source_ids)
{
  grn_obj *column;

  column = grn_obj_column(ctx, table, source_name, source_name_length);
  if (!column) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] nonexistent source: <%.*s>",
        source_name_length, source_name);
    return ctx->rc;
  }

  if (column->header.type == GRN_ACCESSOR) {
    if (strncmp(source_name, "_key", source_name_length) == 0) {
      grn_id source_id = grn_obj_id(ctx, table);
      GRN_UINT32_PUT(ctx, source_ids, source_id);
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "[column][create] pseudo column except <_key> is invalid: <%.*s>",
          source_name_length, source_name);
    }
  } else {
    grn_id source_id = grn_obj_id(ctx, column);
    GRN_UINT32_PUT(ctx, source_ids, source_id);
  }
  grn_obj_unlink(ctx, column);

  return ctx->rc;
}

static grn_rc
proc_column_create_resolve_source_names(grn_ctx *ctx,
                                        grn_obj *table,
                                        grn_obj *source_names,
                                        grn_obj *source_ids)
{
  int i, names_length;
  int start, source_name_length;
  const char *names;

  names = GRN_TEXT_VALUE(source_names);
  start = 0;
  source_name_length = 0;
  names_length = GRN_TEXT_LEN(source_names);
  for (i = 0; i < names_length; i++) {
    switch (names[i]) {
    case ' ' :
      if (source_name_length == 0) {
        start++;
      }
      break;
    case ',' :
      {
        grn_rc rc;
        const char *source_name = names + start;
        rc = proc_column_create_resolve_source_name(ctx,
                                                    table,
                                                    source_name,
                                                    source_name_length,
                                                    source_ids);
        if (rc) {
          return rc;
        }
        start = i + 1;
        source_name_length = 0;
      }
      break;
    default :
      source_name_length++;
      break;
    }
  }

  if (source_name_length > 0) {
    grn_rc rc;
    const char *source_name = names + start;
    rc = proc_column_create_resolve_source_name(ctx,
                                                table,
                                                source_name,
                                                source_name_length,
                                                source_ids);
    if (rc) {
      return rc;
    }
  }

  return GRN_SUCCESS;
}

static grn_obj *
proc_column_create(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_bool succeeded = GRN_TRUE;
  grn_obj *column, *table = NULL, *type = NULL;
  const char *rest;
  grn_obj_flags flags = grn_atoi(GRN_TEXT_VALUE(VAR(2)),
                                 GRN_BULK_CURR(VAR(2)), &rest);
  if (GRN_TEXT_VALUE(VAR(2)) == rest) {
    flags = grn_parse_column_create_flags(ctx, GRN_TEXT_VALUE(VAR(2)),
                                          GRN_BULK_CURR(VAR(2)));
    if (ctx->rc) {
      succeeded = GRN_FALSE;
      goto exit;
    }
  }
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] table doesn't exist: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    succeeded = GRN_FALSE;
    goto exit;
  }
  type = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(3)),
                     GRN_TEXT_LEN(VAR(3)));
  if (!type) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] type doesn't exist: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(3)), GRN_TEXT_VALUE(VAR(3))) ;
    succeeded = GRN_FALSE;
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(1))) {
    flags |= GRN_OBJ_PERSISTENT;
  } else {
    ERR(GRN_INVALID_ARGUMENT, "[column][create] name is missing");
    succeeded = GRN_FALSE;
    goto exit;
  }
  column = grn_column_create(ctx, table,
                             GRN_TEXT_VALUE(VAR(1)),
                             GRN_TEXT_LEN(VAR(1)),
                             NULL, flags, type);
  if (column) {
    if (GRN_TEXT_LEN(VAR(4))) {
      grn_rc rc;
      grn_obj source_ids;
      GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
      rc = proc_column_create_resolve_source_names(ctx,
                                                   type,
                                                   VAR(4),
                                                   &source_ids);
      if (!rc && GRN_BULK_VSIZE(&source_ids)) {
        grn_obj_set_info(ctx, column, GRN_INFO_SOURCE, &source_ids);
        rc = ctx->rc;
      }
      GRN_OBJ_FIN(ctx, &source_ids);
      if (rc) {
        grn_obj_remove(ctx, column);
        succeeded = GRN_FALSE;
        goto exit;
      }
    }
    grn_obj_unlink(ctx, column);
  } else {
    succeeded = GRN_FALSE;
  }
exit :
  GRN_OUTPUT_BOOL(succeeded);
  if (table) { grn_obj_unlink(ctx, table); }
  if (type) { grn_obj_unlink(ctx, type); }
  return NULL;
}

static grn_obj *
proc_column_remove(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table, *col;
  char *colname,fullname[GRN_TABLE_MAX_KEY_SIZE];
  unsigned int colname_len,fullname_len;

  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)),
                           GRN_TEXT_LEN(VAR(0)));

  colname = GRN_TEXT_VALUE(VAR(1));
  colname_len = GRN_TEXT_LEN(VAR(1));

  if ((fullname_len = grn_obj_name(ctx, table, fullname, GRN_TABLE_MAX_KEY_SIZE))) {
    fullname[fullname_len] = GRN_DB_DELIMITER;
    memcpy((fullname + fullname_len + 1), colname, colname_len);
    fullname_len += colname_len + 1;
    //TODO:check fullname_len < GRN_TABLE_MAX_KEY_SIZE
    col = grn_ctx_get(ctx, fullname, fullname_len);
    if (col) {
      grn_obj_remove(ctx, col);
    } else {
      ERR(GRN_INVALID_ARGUMENT, "column not found.");
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "table not found.");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_column_rename(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *table = NULL;
  grn_obj *column = NULL;
  if (GRN_TEXT_LEN(VAR(0)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc, "[column][rename] table name isn't specified");
    goto exit;
  }
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!table) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] table isn't found: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(1)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] column name isn't specified: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    goto exit;
  }
  column = grn_obj_column(ctx, table,
                          GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)));
  if (!column) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] column isn't found: <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(2)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] new column name isn't specified: <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
    goto exit;
  }
  rc = grn_column_rename(ctx, column,
                         GRN_TEXT_VALUE(VAR(2)), GRN_TEXT_LEN(VAR(2)));
  if (rc != GRN_SUCCESS && ctx->rc == GRN_SUCCESS) {
    ERR(rc,
        "[column][rename] failed to rename: <%.*s.%.*s> -> <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)),
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(2)), GRN_TEXT_VALUE(VAR(2)));
  }
exit :
  GRN_OUTPUT_BOOL(!rc);
  if (column) { grn_obj_unlink(ctx, column); }
  if (table) { grn_obj_unlink(ctx, table); }
  return NULL;
}

#define GRN_STRLEN(s) ((s) ? strlen(s) : 0)

static void
output_column_name(grn_ctx *ctx, grn_obj *column)
{
  grn_obj bulk;
  int name_len;
  char name[GRN_TABLE_MAX_KEY_SIZE];

  GRN_TEXT_INIT(&bulk, GRN_OBJ_DO_SHALLOW_COPY);
  name_len = grn_column_name(ctx, column, name, GRN_TABLE_MAX_KEY_SIZE);
  GRN_TEXT_SET(ctx, &bulk, name, name_len);

  GRN_OUTPUT_OBJ(&bulk, NULL);
  GRN_OBJ_FIN(ctx, &bulk);
}

static void
output_object_name(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj bulk;
  int name_len;
  char name[GRN_TABLE_MAX_KEY_SIZE];

  if (obj) {
    GRN_TEXT_INIT(&bulk, GRN_OBJ_DO_SHALLOW_COPY);
    name_len = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
    GRN_TEXT_SET(ctx, &bulk, name, name_len);
  } else {
    GRN_VOID_INIT(&bulk);
  }

  GRN_OUTPUT_OBJ(&bulk, NULL);
  GRN_OBJ_FIN(ctx, &bulk);
}

static void
output_object_id_name(grn_ctx *ctx, grn_id id)
{
  grn_obj *obj = NULL;

  if (id != GRN_ID_NIL) {
    obj = grn_ctx_at(ctx, id);
  }

  output_object_name(ctx, obj);
}

static int
output_column_info(grn_ctx *ctx, grn_obj *column)
{
  grn_obj o;
  grn_id id;
  const char *type;
  const char *path;

  switch (column->header.type) {
  case GRN_COLUMN_FIX_SIZE:
    type = "fix";
    break;
  case GRN_COLUMN_VAR_SIZE:
    type = "var";
    break;
  case GRN_COLUMN_INDEX:
    type = "index";
    break;
  default:
    GRN_LOG(ctx, GRN_LOG_NOTICE, "invalid header type %d\n", column->header.type);
    return 0;
  }
  id = grn_obj_id(ctx, column);
  path = grn_obj_path(ctx, column);
  GRN_TEXT_INIT(&o, 0);
  GRN_OUTPUT_ARRAY_OPEN("COLUMN", 8);
  GRN_OUTPUT_INT64(id);
  output_column_name(ctx, column);
  GRN_OUTPUT_CSTR(path);
  GRN_OUTPUT_CSTR(type);
  grn_column_create_flags_to_text(ctx, &o, column->header.flags);
  GRN_OUTPUT_OBJ(&o, NULL);
  output_object_id_name(ctx, column->header.domain);
  output_object_id_name(ctx, grn_obj_get_range(ctx, column));
  {
    grn_db_obj *obj = (grn_db_obj *)column;
    grn_id *s = obj->source;
    int i = 0, n = obj->source_size / sizeof(grn_id);
    GRN_OUTPUT_ARRAY_OPEN("SOURCES", n);
    for (i = 0; i < n; i++, s++) {
      output_object_id_name(ctx, *s);
    }
    GRN_OUTPUT_ARRAY_CLOSE();

  }
  //  output_obj_source(ctx, (grn_db_obj *)column);
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OBJ_FIN(ctx, &o);
  return 1;
}

static grn_obj *
proc_column_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table;
  if ((table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)),
                           GRN_TEXT_LEN(VAR(0))))) {
    grn_hash *cols;
    grn_obj *col;
    int column_list_size = -1;
#ifdef GRN_WITH_MESSAGE_PACK
    column_list_size = 1; /* [header, (key), (COLUMNS)] */
    if ((col = grn_obj_column(ctx, table,
                              GRN_COLUMN_NAME_KEY,
                              GRN_COLUMN_NAME_KEY_LEN))) {
      column_list_size++;
      grn_obj_unlink(ctx, col);
    }
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      column_list_size += grn_table_columns(ctx, table, NULL, 0,
                                            (grn_obj *)cols);
      grn_hash_close(ctx, cols);
    }
#endif
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      GRN_OUTPUT_ARRAY_OPEN("COLUMN_LIST", column_list_size);
      GRN_OUTPUT_ARRAY_OPEN("HEADER", 8);
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("id");
      GRN_OUTPUT_CSTR("UInt32");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("name");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("path");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("type");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("flags");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("domain");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("range");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("source");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_CLOSE();
      if ((col = grn_obj_column(ctx, table,
                                GRN_COLUMN_NAME_KEY,
                                GRN_COLUMN_NAME_KEY_LEN))) {
        int name_len;
        char name_buf[GRN_TABLE_MAX_KEY_SIZE];
        grn_id id;
        grn_obj buf;
        GRN_TEXT_INIT(&buf, 0);
        GRN_OUTPUT_ARRAY_OPEN("COLUMN", 8);
        id = grn_obj_id(ctx, table);
        GRN_OUTPUT_INT64(id);
        GRN_OUTPUT_CSTR(GRN_COLUMN_NAME_KEY);
        GRN_OUTPUT_CSTR("");
        GRN_OUTPUT_CSTR("");
        grn_column_create_flags_to_text(ctx, &buf, 0);
        GRN_OUTPUT_OBJ(&buf, NULL);
        name_len = grn_obj_name(ctx, table, name_buf, GRN_TABLE_MAX_KEY_SIZE);
        GRN_OUTPUT_STR(name_buf, name_len);
        output_object_id_name(ctx, table->header.domain);
        GRN_OUTPUT_ARRAY_OPEN("SOURCES", 0);
        GRN_OUTPUT_ARRAY_CLOSE();
        GRN_OUTPUT_ARRAY_CLOSE();
        GRN_OBJ_FIN(ctx, &buf);
        grn_obj_unlink(ctx, col);
      }
      if (grn_table_columns(ctx, table, NULL, 0, (grn_obj *)cols) >= 0) {
        grn_id *key;
        GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
          if ((col = grn_ctx_at(ctx, *key))) {
            output_column_info(ctx, col);
            grn_obj_unlink(ctx, col);
          }
        });
      }
      GRN_OUTPUT_ARRAY_CLOSE();
      grn_hash_close(ctx, cols);
    }
    grn_obj_unlink(ctx, table);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "table '%.*s' does not exist.",
        (int)GRN_TEXT_LEN(VAR(0)),
        GRN_TEXT_VALUE(VAR(0)));
  }
  return NULL;
}

static int
output_table_info(grn_ctx *ctx, grn_obj *table)
{
  grn_id id;
  grn_obj o;
  const char *path;
  grn_obj *default_tokenizer;
  grn_obj *normalizer;

  id = grn_obj_id(ctx, table);
  path = grn_obj_path(ctx, table);
  GRN_TEXT_INIT(&o, 0);
  GRN_OUTPUT_ARRAY_OPEN("TABLE", 8);
  GRN_OUTPUT_INT64(id);
  output_object_id_name(ctx, id);
  GRN_OUTPUT_CSTR(path);
  grn_table_create_flags_to_text(ctx, &o, table->header.flags);
  GRN_OUTPUT_OBJ(&o, NULL);
  output_object_id_name(ctx, table->header.domain);
  output_object_id_name(ctx, grn_obj_get_range(ctx, table));
  default_tokenizer = grn_obj_get_info(ctx, table, GRN_INFO_DEFAULT_TOKENIZER,
                                       NULL);
  output_object_name(ctx, default_tokenizer);
  normalizer = grn_obj_get_info(ctx, table, GRN_INFO_NORMALIZER, NULL);
  output_object_name(ctx, normalizer);
  grn_obj_unlink(ctx, normalizer);
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OBJ_FIN(ctx, &o);
  return 1;
}

static grn_obj *
proc_table_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj tables;
  int n_top_level_elements;
  int n_elements_for_header = 1;
  int n_tables;
  int i;

  GRN_PTR_INIT(&tables, GRN_OBJ_VECTOR, GRN_ID_NIL);
  grn_ctx_get_all_tables(ctx, &tables);
  n_tables = GRN_BULK_VSIZE(&tables) / sizeof(grn_obj *);
  n_top_level_elements = n_elements_for_header + n_tables;
  GRN_OUTPUT_ARRAY_OPEN("TABLE_LIST", n_top_level_elements);

  GRN_OUTPUT_ARRAY_OPEN("HEADER", 8);
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("id");
  GRN_OUTPUT_CSTR("UInt32");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("name");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("path");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("flags");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("domain");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("range");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("default_tokenizer");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
  GRN_OUTPUT_CSTR("normalizer");
  GRN_OUTPUT_CSTR("ShortText");
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_CLOSE();

  for (i = 0; i < n_tables; i++) {
    grn_obj *table = GRN_PTR_VALUE_AT(&tables, i);
    output_table_info(ctx, table);
    grn_obj_unlink(ctx, table);
  }
  GRN_OBJ_FIN(ctx, &tables);

  GRN_OUTPUT_ARRAY_CLOSE();

  return NULL;
}

static grn_obj *
proc_missing(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  uint32_t plen;
  grn_obj *outbuf = ctx->impl->outbuf;
  static int grn_document_root_len = -1;
  if (!grn_document_root) { return NULL; }
  if (grn_document_root_len < 0) {
    size_t l;
    if ((l = strlen(grn_document_root)) > PATH_MAX) {
      return NULL;
    }
    grn_document_root_len = (int)l;
    if (l > 0 && grn_document_root[l - 1] == '/') { grn_document_root_len--; }
  }
  if ((plen = GRN_TEXT_LEN(VAR(0))) + grn_document_root_len < PATH_MAX) {
    char path[PATH_MAX];
    memcpy(path, grn_document_root, grn_document_root_len);
    path[grn_document_root_len] = '/';
    grn_str_url_path_normalize(ctx,
                               GRN_TEXT_VALUE(VAR(0)),
                               GRN_TEXT_LEN(VAR(0)),
                               path + grn_document_root_len + 1,
                               PATH_MAX - grn_document_root_len - 1);
    grn_bulk_put_from_file(ctx, outbuf, path);
  } else {
    uint32_t abbrlen = 32;
    ERR(GRN_INVALID_ARGUMENT,
        "too long path name: <%s/%.*s...> %u(%u)",
        grn_document_root,
        abbrlen < plen ? abbrlen : plen, GRN_TEXT_VALUE(VAR(0)),
        plen + grn_document_root_len, PATH_MAX);
  }
  return NULL;
}

static grn_obj *
proc_quit(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  ctx->stat = GRN_CTX_QUITTING;
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_shutdown(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_gctx.stat = GRN_CTX_QUIT;
  ctx->stat = GRN_CTX_QUITTING;
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_lock_clear(grn_ctx *ctx, int nargs, grn_obj **args,
                grn_user_data *user_data)
{
  int target_name_len;
  grn_obj *target_name;
  grn_obj *obj;

  target_name = VAR(0);
  target_name_len = GRN_TEXT_LEN(target_name);

  if (target_name_len) {
    obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(target_name), target_name_len);
  } else {
    obj = ctx->impl->db;
  }

  if (obj) {
    grn_obj_clear_lock(ctx, obj);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "[lock_clear] target object not found: <%.*s>",
        target_name_len, GRN_TEXT_VALUE(target_name));
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_defrag(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  int olen, threshold;
  olen = GRN_TEXT_LEN(VAR(0));

  if (olen) {
    obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), olen);
  } else {
    obj = ctx->impl->db;
  }

  threshold = GRN_TEXT_LEN(VAR(1))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(1)), GRN_BULK_CURR(VAR(1)), NULL)
    : 0;

  if (obj) {
    grn_obj_defrag(ctx, obj, threshold);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "defrag object not found");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static char slev[] = " EACewnid-";

static grn_obj *
proc_log_level(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  char *p;
  if (GRN_TEXT_LEN(VAR(0)) &&
      (p = strchr(slev, GRN_TEXT_VALUE(VAR(0))[0]))) {
    grn_log_level max_level = (grn_log_level)(p - slev);
    grn_logger_set_max_level(ctx, max_level);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid log level.");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_log_put(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  char *p;
  if (GRN_TEXT_LEN(VAR(0)) &&
      (p = strchr(slev, GRN_TEXT_VALUE(VAR(0))[0]))) {
    GRN_TEXT_PUTC(ctx, VAR(1), '\0');
    GRN_LOG(ctx, (int)(p - slev), "%s", GRN_TEXT_VALUE(VAR(1)));
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid log level.");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_log_reopen(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_log_reopen(ctx);
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_rc
proc_delete_validate_selector(grn_ctx *ctx, grn_obj *table, grn_obj *table_name,
                              grn_obj *key, grn_obj *id, grn_obj *filter)
{
  grn_rc rc = GRN_SUCCESS;

  if (!table) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] table doesn't exist: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name));
    return rc;
  }

  if (GRN_TEXT_LEN(key) == 0 &&
      GRN_TEXT_LEN(id) == 0 &&
      GRN_TEXT_LEN(filter) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] either key, id or filter must be specified: "
        "table: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name));
    return rc;
  }

  if (GRN_TEXT_LEN(key) && GRN_TEXT_LEN(id) && GRN_TEXT_LEN(filter)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "record selector must be one of key, id and filter: "
        "table: <%.*s>, key: <%.*s>, id: <%.*s>, filter: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(key), GRN_TEXT_VALUE(key),
        (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id),
        (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter));
    return rc;
  }

  if (GRN_TEXT_LEN(key) && GRN_TEXT_LEN(id) && GRN_TEXT_LEN(filter) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "can't use both key and id: table: <%.*s>, key: <%.*s>, id: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(key), GRN_TEXT_VALUE(key),
        (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id));
    return rc;
  }

  if (GRN_TEXT_LEN(key) && GRN_TEXT_LEN(id) == 0 && GRN_TEXT_LEN(filter)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "can't use both key and filter: "
        "table: <%.*s>, key: <%.*s>, filter: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(key), GRN_TEXT_VALUE(key),
        (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter));
    return rc;
  }

  if (GRN_TEXT_LEN(key) == 0 && GRN_TEXT_LEN(id) && GRN_TEXT_LEN(filter)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "can't use both id and filter: "
        "table: <%.*s>, id: <%.*s>, filter: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id),
        (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter));
    return rc;
  }

  return rc;
}

static grn_obj *
proc_delete(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_obj *table_name = VAR(0);
  grn_obj *key = VAR(1);
  grn_obj *id = VAR(2);
  grn_obj *filter = VAR(3);
  grn_obj *table = NULL;

  if (GRN_TEXT_LEN(table_name) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc, "[table][record][delete] table name isn't specified");
    goto exit;
  }

  table = grn_ctx_get(ctx,
                      GRN_TEXT_VALUE(table_name),
                      GRN_TEXT_LEN(table_name));
  rc = proc_delete_validate_selector(ctx, table, table_name, key, id, filter);
  if (rc != GRN_SUCCESS) { goto exit; }

  if (GRN_TEXT_LEN(key)) {
    grn_obj casted_key;
    if (key->header.domain != table->header.domain) {
      GRN_OBJ_INIT(&casted_key, GRN_BULK, 0, table->header.domain);
      grn_obj_cast(ctx, key, &casted_key, GRN_FALSE);
      key = &casted_key;
    }
    if (ctx->rc) {
      rc = ctx->rc;
    } else {
      rc = grn_table_delete(ctx, table, GRN_BULK_HEAD(key), GRN_BULK_VSIZE(key));
      if (key == &casted_key) {
        GRN_OBJ_FIN(ctx, &casted_key);
      }
    }
  } else if (GRN_TEXT_LEN(id)) {
    const char *end;
    grn_id parsed_id = grn_atoui(GRN_TEXT_VALUE(id), GRN_BULK_CURR(id), &end);
    if (end == GRN_BULK_CURR(id)) {
      rc = grn_table_delete_by_id(ctx, table, parsed_id);
    } else {
      rc = GRN_INVALID_ARGUMENT;
      ERR(rc,
          "[table][record][delete] id should be number: "
          "table: <%.*s>, id: <%.*s>, detail: <%.*s|%c|%.*s>",
          (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
          (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id),
          (int)(end - GRN_TEXT_VALUE(id)), GRN_TEXT_VALUE(id),
          end[0],
          (int)(GRN_TEXT_VALUE(id) - end - 1), end + 1);
    }
  } else if (GRN_TEXT_LEN(filter)) {
    grn_obj *cond, *v;

    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, cond, v);
    grn_expr_parse(ctx, cond,
                   GRN_TEXT_VALUE(filter),
                   GRN_TEXT_LEN(filter),
                   NULL, GRN_OP_MATCH, GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc) {
      char original_error_message[GRN_CTX_MSGSIZE];
      strcpy(original_error_message, ctx->errbuf);
      rc = ctx->rc;
      ERR(rc,
          "[table][record][delete] failed to parse filter: "
          "table: <%.*s>, filter: <%.*s>, detail: <%s>",
          (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
          (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter),
          original_error_message);
    } else {
      grn_obj *records;

      records = grn_table_select(ctx, table, cond, NULL, GRN_OP_OR);
      if (records) {
        void *key = NULL;
        GRN_TABLE_EACH(ctx, records, GRN_ID_NIL, GRN_ID_NIL,
                       result_id, &key, NULL, NULL, {
          grn_id id = *(grn_id *)key;
          grn_table_delete_by_id(ctx, table, id);
          if (ctx->rc == GRN_OPERATION_NOT_PERMITTED) {
            ERRCLR(ctx);
          }
        });
        grn_obj_unlink(ctx, records);
      }
    }
    grn_obj_unlink(ctx, cond);
  }

exit :
  if (table) {
    grn_obj_unlink(ctx, table);
  }
  GRN_OUTPUT_BOOL(!rc);
  return NULL;
}

static const size_t DUMP_FLUSH_THRESHOLD_SIZE = 256 * 1024;

static void
dump_name(grn_ctx *ctx, grn_obj *outbuf, const char *name, int name_len)
{
  grn_obj escaped_name;
  GRN_TEXT_INIT(&escaped_name, 0);
  grn_text_esc(ctx, &escaped_name, name, name_len);
  /* is no character escaped? */
  /* TODO false positive with spaces inside names */
  if (GRN_TEXT_LEN(&escaped_name) == name_len + 2) {
    GRN_TEXT_PUT(ctx, outbuf, name, name_len);
  } else {
    GRN_TEXT_PUT(ctx, outbuf,
                 GRN_TEXT_VALUE(&escaped_name), GRN_TEXT_LEN(&escaped_name));
  }
  grn_obj_close(ctx, &escaped_name);
}

static void
dump_obj_name(grn_ctx *ctx, grn_obj *outbuf, grn_obj *obj)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;
  name_len = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
  dump_name(ctx, outbuf, name, name_len);
}

static void
dump_column_name(grn_ctx *ctx, grn_obj *outbuf, grn_obj *column)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;
  name_len = grn_column_name(ctx, column, name, GRN_TABLE_MAX_KEY_SIZE);
  dump_name(ctx, outbuf, name, name_len);
}

static void
dump_index_column_sources(grn_ctx *ctx, grn_obj *outbuf, grn_obj *column)
{
  grn_obj sources;
  grn_id *source_ids;
  int i, n;

  GRN_OBJ_INIT(&sources, GRN_BULK, 0, GRN_ID_NIL);
  grn_obj_get_info(ctx, column, GRN_INFO_SOURCE, &sources);

  n = GRN_BULK_VSIZE(&sources) / sizeof(grn_id);
  source_ids = (grn_id *)GRN_BULK_HEAD(&sources);
  if (n > 0) {
    GRN_TEXT_PUTC(ctx, outbuf, ' ');
  }
  for (i = 0; i < n; i++) {
    grn_obj *source;
    if ((source = grn_ctx_at(ctx, *source_ids))) {
      if (i) { GRN_TEXT_PUTC(ctx, outbuf, ','); }
      switch (source->header.type) {
      case GRN_TABLE_PAT_KEY:
      case GRN_TABLE_DAT_KEY:
      case GRN_TABLE_HASH_KEY:
        GRN_TEXT_PUT(ctx, outbuf, GRN_COLUMN_NAME_KEY, GRN_COLUMN_NAME_KEY_LEN);
        break;
      default:
        dump_column_name(ctx, outbuf, source);
        break;
      }
    }
    source_ids++;
  }
  grn_obj_close(ctx, &sources);
}

static void
dump_column(grn_ctx *ctx, grn_obj *outbuf , grn_obj *table, grn_obj *column)
{
  grn_obj *type;
  grn_obj_flags default_flags = GRN_OBJ_PERSISTENT;
  grn_obj buf;

  type = grn_ctx_at(ctx, ((grn_db_obj *)column)->range);
  if (!type) {
    // ERR(GRN_RANGE_ERROR, "couldn't get column's type object");
    return;
  }

  GRN_TEXT_PUTS(ctx, outbuf, "column_create ");
  dump_obj_name(ctx, outbuf, table);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  dump_column_name(ctx, outbuf, column);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  if (type->header.type == GRN_TYPE) {
    default_flags |= type->header.flags;
  }
  GRN_TEXT_INIT(&buf, 0);
  grn_column_create_flags_to_text(ctx, &buf, column->header.flags & ~default_flags);
  GRN_TEXT_PUT(ctx, outbuf, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
  GRN_OBJ_FIN(ctx, &buf);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  dump_obj_name(ctx, outbuf, type);
  if (column->header.flags & GRN_OBJ_COLUMN_INDEX) {
    dump_index_column_sources(ctx, outbuf, column);
  }
  GRN_TEXT_PUTC(ctx, outbuf, '\n');

  grn_obj_unlink(ctx, type);
}

static int
reference_column_p(grn_ctx *ctx, grn_obj *column)
{
  grn_obj *range;

  range = grn_ctx_at(ctx, grn_obj_get_range(ctx, column));
  if (!range) {
    return GRN_FALSE;
  }

  switch (range->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
  case GRN_TABLE_NO_KEY:
    return GRN_TRUE;
  default:
    return GRN_FALSE;
  }
}

static void
dump_columns(grn_ctx *ctx, grn_obj *outbuf, grn_obj *table,
             grn_obj *pending_columns)
{
  grn_hash *columns;
  columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                            GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
  if (!columns) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "couldn't create a hash to hold columns");
    return;
  }

  if (grn_table_columns(ctx, table, NULL, 0, (grn_obj *)columns) >= 0) {
    grn_id *key;

    GRN_HASH_EACH(ctx, columns, id, &key, NULL, NULL, {
      grn_obj *column;
      if ((column = grn_ctx_at(ctx, *key))) {
        if (reference_column_p(ctx, column)) {
          GRN_PTR_PUT(ctx, pending_columns, column);
        } else {
          dump_column(ctx, outbuf, table, column);
          grn_obj_unlink(ctx, column);
        }
      }
    });
  }
  grn_hash_close(ctx, columns);
}

static void
dump_record_column_vector(grn_ctx *ctx, grn_obj *outbuf, grn_id id,
                          grn_obj *column, grn_id range_id, grn_obj *buf)
{
  grn_obj *range;

  range = grn_ctx_at(ctx, range_id);
  if (GRN_OBJ_TABLEP(range) ||
      (range->header.flags & GRN_OBJ_KEY_VAR_SIZE) == 0) {
    GRN_OBJ_INIT(buf, GRN_UVECTOR, 0, range_id);
    grn_obj_get_value(ctx, column, id, buf);
    grn_text_otoj(ctx, outbuf, buf, NULL);
  } else {
    grn_obj_format *format_argument = NULL;
    grn_obj_format format;
    if (column->header.flags & GRN_OBJ_WITH_WEIGHT) {
      format.flags = GRN_OBJ_FORMAT_WITH_WEIGHT;
      format_argument = &format;
    }
    GRN_OBJ_INIT(buf, GRN_VECTOR, 0, range_id);
    grn_obj_get_value(ctx, column, id, buf);
    grn_text_otoj(ctx, outbuf, buf, format_argument);
  }
  grn_obj_unlink(ctx, range);
  grn_obj_unlink(ctx, buf);
}

static void
dump_records(grn_ctx *ctx, grn_obj *outbuf, grn_obj *table)
{
  grn_obj **columns;
  grn_id old_id = 0, id;
  grn_table_cursor *cursor;
  int i, ncolumns, n_use_columns;
  grn_obj columnbuf, delete_commands, use_columns, column_name;

  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
  case GRN_TABLE_NO_KEY:
    break;
  default:
    return;
  }

  if (grn_table_size(ctx, table) == 0) {
    return;
  }

  GRN_TEXT_INIT(&delete_commands, 0);

  GRN_TEXT_PUTS(ctx, outbuf, "load --table ");
  dump_obj_name(ctx, outbuf, table);
  GRN_TEXT_PUTS(ctx, outbuf, "\n[\n");

  GRN_PTR_INIT(&columnbuf, GRN_OBJ_VECTOR, GRN_ID_NIL);
  grn_obj_columns(ctx, table, DUMP_COLUMNS, strlen(DUMP_COLUMNS), &columnbuf);
  columns = (grn_obj **)GRN_BULK_HEAD(&columnbuf);
  ncolumns = GRN_BULK_VSIZE(&columnbuf)/sizeof(grn_obj *);

  GRN_PTR_INIT(&use_columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  GRN_TEXT_INIT(&column_name, 0);
  for (i = 0; i < ncolumns; i++) {
    if (GRN_OBJ_INDEX_COLUMNP(columns[i])) {
      continue;
    }
    GRN_BULK_REWIND(&column_name);
    grn_column_name_(ctx, columns[i], &column_name);
    if (((table->header.type == GRN_TABLE_HASH_KEY ||
          table->header.type == GRN_TABLE_PAT_KEY ||
          table->header.type == GRN_TABLE_DAT_KEY) &&
         GRN_TEXT_LEN(&column_name) == GRN_COLUMN_NAME_ID_LEN &&
         !memcmp(GRN_TEXT_VALUE(&column_name),
                 GRN_COLUMN_NAME_ID,
                 GRN_COLUMN_NAME_ID_LEN)) ||
        (table->header.type == GRN_TABLE_NO_KEY &&
         GRN_TEXT_LEN(&column_name) == GRN_COLUMN_NAME_KEY_LEN &&
         !memcmp(GRN_TEXT_VALUE(&column_name),
                 GRN_COLUMN_NAME_KEY,
                 GRN_COLUMN_NAME_KEY_LEN))) {
      continue;
    }
    GRN_PTR_PUT(ctx, &use_columns, columns[i]);
  }

  n_use_columns = GRN_BULK_VSIZE(&use_columns) / sizeof(grn_obj *);
  GRN_TEXT_PUTC(ctx, outbuf, '[');
  for (i = 0; i < n_use_columns; i++) {
    grn_obj *column;
    column = *((grn_obj **)GRN_BULK_HEAD(&use_columns) + i);
    if (i) { GRN_TEXT_PUTC(ctx, outbuf, ','); }
    GRN_BULK_REWIND(&column_name);
    grn_column_name_(ctx, column, &column_name);
    grn_text_otoj(ctx, outbuf, &column_name, NULL);
  }
  GRN_TEXT_PUTS(ctx, outbuf, "],\n");

  cursor = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1,
                                 GRN_CURSOR_BY_KEY);
  for (i = 0; (id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL;
       ++i, old_id = id) {
    int is_value_column;
    int j;
    grn_obj buf;
    if (i) { GRN_TEXT_PUTS(ctx, outbuf, ",\n"); }
    if (table->header.type == GRN_TABLE_NO_KEY && old_id + 1 < id) {
      grn_id current_id;
      for (current_id = old_id + 1; current_id < id; current_id++) {
        GRN_TEXT_PUTS(ctx, outbuf, "[],\n");
        GRN_TEXT_PUTS(ctx, &delete_commands, "delete --table ");
        dump_obj_name(ctx, &delete_commands, table);
        GRN_TEXT_PUTS(ctx, &delete_commands, " --id ");
        grn_text_lltoa(ctx, &delete_commands, current_id);
        GRN_TEXT_PUTC(ctx, &delete_commands, '\n');
      }
    }
    GRN_TEXT_PUTC(ctx, outbuf, '[');
    for (j = 0; j < n_use_columns; j++) {
      grn_id range;
      grn_obj *column;
      column = *((grn_obj **)GRN_BULK_HEAD(&use_columns) + j);
      GRN_BULK_REWIND(&column_name);
      grn_column_name_(ctx, column, &column_name);
      if (GRN_TEXT_LEN(&column_name) == GRN_COLUMN_NAME_VALUE_LEN &&
          !memcmp(GRN_TEXT_VALUE(&column_name),
                  GRN_COLUMN_NAME_VALUE,
                  GRN_COLUMN_NAME_VALUE_LEN)) {
        is_value_column = 1;
      } else {
        is_value_column = 0;
      }
      range = grn_obj_get_range(ctx, column);

      if (j) { GRN_TEXT_PUTC(ctx, outbuf, ','); }
      switch (column->header.type) {
      case GRN_COLUMN_VAR_SIZE:
      case GRN_COLUMN_FIX_SIZE:
        switch (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
        case GRN_OBJ_COLUMN_VECTOR:
          dump_record_column_vector(ctx, outbuf, id, column, range, &buf);
          break;
        case GRN_OBJ_COLUMN_SCALAR:
          {
            GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
            grn_obj_get_value(ctx, column, id, &buf);
            grn_text_otoj(ctx, outbuf, &buf, NULL);
            grn_obj_unlink(ctx, &buf);
          }
          break;
        default:
          ERR(GRN_OPERATION_NOT_SUPPORTED,
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
            buf.header.domain = ((grn_db_obj *)table)->range;
          }
          grn_text_otoj(ctx, outbuf, &buf, NULL);
          grn_obj_unlink(ctx, &buf);
        }
        break;
      default:
        ERR(GRN_OPERATION_NOT_SUPPORTED,
            "unsupported header type %#x",
            column->header.type);
        break;
      }
    }
    GRN_TEXT_PUTC(ctx, outbuf, ']');
    if (GRN_TEXT_LEN(outbuf) >= DUMP_FLUSH_THRESHOLD_SIZE) {
      grn_ctx_output_flush(ctx, 0);
    }
  }
  GRN_TEXT_PUTS(ctx, outbuf, "\n]\n");
  GRN_TEXT_PUT(ctx, outbuf, GRN_TEXT_VALUE(&delete_commands),
                            GRN_TEXT_LEN(&delete_commands));
  grn_obj_unlink(ctx, &delete_commands);
  grn_obj_unlink(ctx, &column_name);
  grn_obj_unlink(ctx, &use_columns);

  grn_table_cursor_close(ctx, cursor);
  for (i = 0; i < ncolumns; i++) {
    grn_obj_unlink(ctx, columns[i]);
  }
  grn_obj_unlink(ctx, &columnbuf);
}

static void
dump_table(grn_ctx *ctx, grn_obj *outbuf, grn_obj *table,
           grn_obj *pending_columns)
{
  grn_obj *domain = NULL, *range = NULL;
  grn_obj_flags default_flags = GRN_OBJ_PERSISTENT;
  grn_obj *default_tokenizer;
  grn_obj *normalizer;
  grn_obj buf;

  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
    domain = grn_ctx_at(ctx, table->header.domain);
    break;
  default:
    break;
  }

  GRN_TEXT_PUTS(ctx, outbuf, "table_create ");
  dump_obj_name(ctx, outbuf, table);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  GRN_TEXT_INIT(&buf, 0);
  grn_table_create_flags_to_text(ctx, &buf, table->header.flags & ~default_flags);
  GRN_TEXT_PUT(ctx, outbuf, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
  GRN_OBJ_FIN(ctx, &buf);
  if (domain) {
    GRN_TEXT_PUTC(ctx, outbuf, ' ');
    dump_obj_name(ctx, outbuf, domain);
  }
  if (((grn_db_obj *)table)->range != GRN_ID_NIL) {
    range = grn_ctx_at(ctx, ((grn_db_obj *)table)->range);
    if (!range) {
      // ERR(GRN_RANGE_ERROR, "couldn't get table's value_type object");
      return;
    }
    if (table->header.type != GRN_TABLE_NO_KEY) {
      GRN_TEXT_PUTC(ctx, outbuf, ' ');
    } else {
      GRN_TEXT_PUTS(ctx, outbuf, " --value_type ");
    }
    dump_obj_name(ctx, outbuf, range);
    grn_obj_unlink(ctx, range);
  }
  default_tokenizer = grn_obj_get_info(ctx, table, GRN_INFO_DEFAULT_TOKENIZER,
                                       NULL);
  if (default_tokenizer) {
    GRN_TEXT_PUTS(ctx, outbuf, " --default_tokenizer ");
    dump_obj_name(ctx, outbuf, default_tokenizer);
  }
  normalizer = grn_obj_get_info(ctx, table, GRN_INFO_NORMALIZER, NULL);
  if (normalizer) {
    GRN_TEXT_PUTS(ctx, outbuf, " --normalizer ");
    dump_obj_name(ctx, outbuf, normalizer);
  }
  if (table->header.type != GRN_TABLE_NO_KEY) {
    grn_obj token_filters;
    int n_token_filters;

    GRN_PTR_INIT(&token_filters, GRN_OBJ_VECTOR, GRN_ID_NIL);
    grn_obj_get_info(ctx, table, GRN_INFO_TOKEN_FILTERS, &token_filters);
    n_token_filters = GRN_BULK_VSIZE(&token_filters) / sizeof(grn_obj *);
    if (n_token_filters > 0) {
      int i;
      GRN_TEXT_PUTS(ctx, outbuf, " --token_filters ");
      for (i = 0; i < n_token_filters; i++) {
        grn_obj *token_filter = GRN_PTR_VALUE_AT(&token_filters, i);
        if (i > 0) {
          GRN_TEXT_PUTC(ctx, outbuf, ',');
        }
        dump_obj_name(ctx, outbuf, token_filter);
      }
    }
    GRN_OBJ_FIN(ctx, &token_filters);
  }

  GRN_TEXT_PUTC(ctx, outbuf, '\n');

  if (domain) {
    grn_obj_unlink(ctx, domain);
  }

  dump_columns(ctx, outbuf, table, pending_columns);
}

/* can we move this to groonga.h? */
#define GRN_PTR_POP(obj,value) do {\
  if (GRN_BULK_VSIZE(obj) >= sizeof(grn_obj *)) {\
    GRN_BULK_INCR_LEN((obj), -(sizeof(grn_obj *)));\
    value = *(grn_obj **)(GRN_BULK_CURR(obj));\
  } else {\
    value = NULL;\
  }\
} while (0)

static void
dump_schema(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj *db = ctx->impl->db;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                   GRN_CURSOR_BY_ID))) {
    grn_id id;
    grn_obj pending_columns;

    GRN_PTR_INIT(&pending_columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
    while ((id = grn_table_cursor_next(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *object;

      if ((object = grn_ctx_at(ctx, id))) {
        switch (object->header.type) {
        case GRN_TABLE_HASH_KEY:
        case GRN_TABLE_PAT_KEY:
        case GRN_TABLE_DAT_KEY:
        case GRN_TABLE_NO_KEY:
          dump_table(ctx, outbuf, object, &pending_columns);
          break;
        default:
          break;
        }
        grn_obj_unlink(ctx, object);
      } else {
        /* XXX: this clause is executed when MeCab tokenizer is enabled in
           database but the groonga isn't supported MeCab.
           We should return error mesage about it and error exit status
           but it's too difficult for this architecture. :< */
        ERRCLR(ctx);
      }
    }
    grn_table_cursor_close(ctx, cur);

    while (GRN_TRUE) {
      grn_obj *table, *column;
      GRN_PTR_POP(&pending_columns, column);
      if (!column) {
        break;
      }
      table = grn_ctx_at(ctx, column->header.domain);
      dump_column(ctx, outbuf, table, column);
      grn_obj_unlink(ctx, column);
      grn_obj_unlink(ctx, table);
    }
    grn_obj_close(ctx, &pending_columns);
  }
}

static void
dump_selected_tables_records(grn_ctx *ctx, grn_obj *outbuf, grn_obj *tables)
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

    if ((table = grn_ctx_get(ctx, token, token_e - token))) {
      dump_records(ctx, outbuf, table);
      grn_obj_unlink(ctx, table);
    } else {
      GRN_LOG(ctx, GRN_LOG_WARNING,
              "nonexistent table name is ignored: <%.*s>\n",
              (int)(token_e - token), token);
    }
  }
}

static void
dump_all_records(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj *db = ctx->impl->db;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                   GRN_CURSOR_BY_ID))) {
    grn_id id;

    while ((id = grn_table_cursor_next(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *table;

      if ((table = grn_ctx_at(ctx, id))) {
        dump_records(ctx, outbuf, table);
        grn_obj_unlink(ctx, table);
      } else {
        /* XXX: this clause is executed when MeCab tokenizer is enabled in
           database but the groonga isn't supported MeCab.
           We should return error mesage about it and error exit status
           but it's too difficult for this architecture. :< */
        ERRCLR(ctx);
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
}

static grn_obj *
proc_dump(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *outbuf = ctx->impl->outbuf;
  ctx->impl->output_type = GRN_CONTENT_NONE;
  ctx->impl->mime_type = "text/x-groonga-command-list";
  dump_schema(ctx, outbuf);
  grn_ctx_output_flush(ctx, 0);
  /* To update index columns correctly, we first create the whole schema, then
     load non-derivative records, while skipping records of index columns. That
     way, groonga will silently do the job of updating index columns for us. */
  if (GRN_TEXT_LEN(VAR(0)) > 0) {
    dump_selected_tables_records(ctx, outbuf, VAR(0));
  } else {
    dump_all_records(ctx, outbuf);
  }

  /* remove the last newline because another one will be added by the caller.
     maybe, the caller of proc functions currently doesn't consider the
     possibility of multiple-line output from proc functions. */
  if (GRN_BULK_VSIZE(outbuf) > 0) {
    grn_bulk_truncate(ctx, outbuf, GRN_BULK_VSIZE(outbuf) - 1);
  }
  return NULL;
}

static grn_obj *
proc_cache_limit(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_cache *cache;
  unsigned int current_max_n_entries;

  cache = grn_cache_current_get(ctx);
  current_max_n_entries = grn_cache_get_max_n_entries(ctx, cache);
  if (GRN_TEXT_LEN(VAR(0))) {
    const char *rest;
    uint32_t max = grn_atoui(GRN_TEXT_VALUE(VAR(0)),
                             GRN_BULK_CURR(VAR(0)), &rest);
    if (GRN_BULK_CURR(VAR(0)) == rest) {
      grn_cache_set_max_n_entries(ctx, cache, max);
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "max value is invalid unsigned integer format: <%.*s>",
          (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    }
  }
  if (ctx->rc == GRN_SUCCESS) {
    GRN_OUTPUT_INT64(current_max_n_entries);
  }
  return NULL;
}

static grn_obj *
proc_register(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  if (GRN_TEXT_LEN(VAR(0))) {
    const char *name;
    GRN_TEXT_PUTC(ctx, VAR(0), '\0');
    name = GRN_TEXT_VALUE(VAR(0));
    grn_plugin_register(ctx, name);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "path is required");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

void grn_ii_buffer_check(grn_ctx *ctx, grn_ii *ii, uint32_t seg);

static grn_obj *
proc_check(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT,
        "no such object: <%.*s>", (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    GRN_OUTPUT_BOOL(!ctx->rc);
  } else {
    switch (obj->header.type) {
    case GRN_DB :
      GRN_OUTPUT_BOOL(!ctx->rc);
      break;
    case GRN_TABLE_PAT_KEY :
      grn_pat_check(ctx, (grn_pat *)obj);
      break;
    case GRN_TABLE_HASH_KEY :
      grn_hash_check(ctx, (grn_hash *)obj);
      break;
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
    case GRN_COLUMN_FIX_SIZE :
      GRN_OUTPUT_BOOL(!ctx->rc);
      break;
    case GRN_COLUMN_VAR_SIZE :
      grn_ja_check(ctx, (grn_ja *)obj);
      break;
    case GRN_COLUMN_INDEX :
      {
        grn_ii *ii = (grn_ii *)obj;
        struct grn_ii_header *h = ii->header;
        char buf[8];
        GRN_OUTPUT_ARRAY_OPEN("RESULT", 8);
        {
          uint32_t i, j, g =0, a = 0, b = 0;
          uint32_t max = 0;
          for (i = h->bgqtail; i != h->bgqhead; i = ((i + 1) & (GRN_II_BGQSIZE - 1))) {
            j = h->bgqbody[i];
            g++;
            if (j > max) { max = j; }
          }
          for (i = 0; i < GRN_II_MAX_LSEG; i++) {
            j = h->binfo[i];
            if (j < 0x20000) {
              if (j > max) { max = j; }
              b++;
            }
          }
          for (i = 0; i < GRN_II_MAX_LSEG; i++) {
            j = h->ainfo[i];
            if (j < 0x20000) {
              if (j > max) { max = j; }
              a++;
            }
          }
          GRN_OUTPUT_MAP_OPEN("SUMMARY", 12);
          GRN_OUTPUT_CSTR("flags");
          grn_itoh(h->flags, buf, 8);
          GRN_OUTPUT_STR(buf, 8);
          GRN_OUTPUT_CSTR("max sid");
          GRN_OUTPUT_INT64(h->smax);
          GRN_OUTPUT_CSTR("number of garbage segments");
          GRN_OUTPUT_INT64(g);
          GRN_OUTPUT_CSTR("number of array segments");
          GRN_OUTPUT_INT64(a);
          GRN_OUTPUT_CSTR("max id of array segment");
          GRN_OUTPUT_INT64(h->amax);
          GRN_OUTPUT_CSTR("number of buffer segments");
          GRN_OUTPUT_INT64(b);
          GRN_OUTPUT_CSTR("max id of buffer segment");
          GRN_OUTPUT_INT64(h->bmax);
          GRN_OUTPUT_CSTR("max id of physical segment in use");
          GRN_OUTPUT_INT64(max);
          GRN_OUTPUT_CSTR("number of unmanaged segments");
          GRN_OUTPUT_INT64(h->pnext - a - b - g);
          GRN_OUTPUT_CSTR("total chunk size");
          GRN_OUTPUT_INT64(h->total_chunk_size);
          for (max = 0, i = 0; i < (GRN_II_MAX_CHUNK >> 3); i++) {
            if ((j = h->chunks[i])) {
              int k;
              for (k = 0; k < 8; k++) {
                if ((j & (1 << k))) { max = (i << 3) + j; }
              }
            }
          }
          GRN_OUTPUT_CSTR("max id of chunk segments in use");
          GRN_OUTPUT_INT64(max);
          GRN_OUTPUT_CSTR("number of garbage chunk");
          GRN_OUTPUT_ARRAY_OPEN("NGARBAGES", GRN_II_N_CHUNK_VARIATION);
          for (i = 0; i <= GRN_II_N_CHUNK_VARIATION; i++) {
            GRN_OUTPUT_INT64(h->ngarbages[i]);
          }
          GRN_OUTPUT_ARRAY_CLOSE();
          GRN_OUTPUT_MAP_CLOSE();
          for (i = 0; i < GRN_II_MAX_LSEG; i++) {
            if (h->binfo[i] < 0x20000) { grn_ii_buffer_check(ctx, ii, i); }
          }
        }
        GRN_OUTPUT_ARRAY_CLOSE();
      }
      break;
    }
  }
  return NULL;
}

static grn_obj *
proc_truncate(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  const char *target_name;
  int target_name_len;

  target_name_len = GRN_TEXT_LEN(VAR(0));
  if (target_name_len > 0) {
    target_name = GRN_TEXT_VALUE(VAR(0));
  } else {
    target_name_len = GRN_TEXT_LEN(VAR(1));
    if (target_name_len == 0) {
      ERR(GRN_INVALID_ARGUMENT, "[truncate] table name is missing");
      goto exit;
    }
    target_name = GRN_TEXT_VALUE(VAR(1));
  }

  {
    grn_obj *target = grn_ctx_get(ctx, target_name, target_name_len);
    if (!target) {
      ERR(GRN_INVALID_ARGUMENT,
          "[truncate] no such target: <%.*s>", target_name_len, target_name);
      goto exit;
    }

    switch (target->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
      grn_table_truncate(ctx, target);
      break;
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_INDEX :
      grn_column_truncate(ctx, target);
      break;
    default:
      {
        grn_obj buffer;
        GRN_TEXT_INIT(&buffer, 0);
        grn_inspect(ctx, &buffer, target);
        ERR(GRN_INVALID_ARGUMENT,
            "[truncate] not a table nor column object: <%.*s>",
            (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
        GRN_OBJ_FIN(ctx, &buffer);
      }
      break;
    }
  }

exit :
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static int
parse_normalize_flags(grn_ctx *ctx, grn_obj *flag_names)
{
  int flags = 0;
  const char *names, *names_end;
  int length;

  names = GRN_TEXT_VALUE(flag_names);
  length = GRN_TEXT_LEN(flag_names);
  names_end = names + length;
  while (names < names_end) {
    if (*names == '|' || *names == ' ') {
      names += 1;
      continue;
    }

#define CHECK_FLAG(name)\
    if (((names_end - names) >= (sizeof(#name) - 1)) &&\
        (!memcmp(names, #name, sizeof(#name) - 1))) {\
      flags |= GRN_STRING_ ## name;\
      names += sizeof(#name) - 1;\
      continue;\
    }

    CHECK_FLAG(REMOVE_BLANK);
    CHECK_FLAG(WITH_TYPES);
    CHECK_FLAG(WITH_CHECKS);
    CHECK_FLAG(REMOVE_TOKENIZED_DELIMITER);

#define GRN_STRING_NONE 0
    CHECK_FLAG(NONE);
#undef GRN_STRING_NONE

    ERR(GRN_INVALID_ARGUMENT, "[normalize] invalid flag: <%.*s>",
        (int)(names_end - names), names);
    return 0;
#undef CHECK_FLAG
  }

  return flags;
}

static grn_bool
is_normalizer(grn_ctx *ctx, grn_obj *object)
{
  if (object->header.type != GRN_PROC) {
    return GRN_FALSE;
  }

  if (grn_proc_get_type(ctx, object) != GRN_PROC_NORMALIZER) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static grn_bool
is_tokenizer(grn_ctx *ctx, grn_obj *object)
{
  if (object->header.type != GRN_PROC) {
    return GRN_FALSE;
  }

  if (grn_proc_get_type(ctx, object) != GRN_PROC_TOKENIZER) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static const char *
char_type_name(grn_char_type type)
{
  const char *name = "unknown";

  switch (type) {
  case GRN_CHAR_NULL :
    name = "null";
    break;
  case GRN_CHAR_ALPHA :
    name = "alpha";
    break;
  case GRN_CHAR_DIGIT :
    name = "digit";
    break;
  case GRN_CHAR_SYMBOL :
    name = "symbol";
    break;
  case GRN_CHAR_HIRAGANA :
    name = "hiragana";
    break;
  case GRN_CHAR_KATAKANA :
    name = "katakana";
    break;
  case GRN_CHAR_KANJI :
    name = "kanji";
    break;
  case GRN_CHAR_OTHERS :
    name = "others";
    break;
  }

  return name;
}

static grn_obj *
proc_normalize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *normalizer_name;
  grn_obj *string;
  grn_obj *flag_names;

  normalizer_name = VAR(0);
  string = VAR(1);
  flag_names = VAR(2);
  if (GRN_TEXT_LEN(normalizer_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "normalizer name is missing");
    GRN_OUTPUT_CSTR("");
    return NULL;
  }

  {
    grn_obj *normalizer;
    grn_obj *grn_string;
    int flags;
    unsigned int normalized_length_in_bytes;
    unsigned int normalized_n_characters;

    flags = parse_normalize_flags(ctx, flag_names);
    normalizer = grn_ctx_get(ctx,
                             GRN_TEXT_VALUE(normalizer_name),
                             GRN_TEXT_LEN(normalizer_name));
    if (!normalizer) {
      ERR(GRN_INVALID_ARGUMENT,
          "[normalize] nonexistent normalizer: <%.*s>",
          (int)GRN_TEXT_LEN(normalizer_name),
          GRN_TEXT_VALUE(normalizer_name));
      GRN_OUTPUT_CSTR("");
      return NULL;
    }

    if (!is_normalizer(ctx, normalizer)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, normalizer);
      ERR(GRN_INVALID_ARGUMENT,
          "[normalize] not normalizer: %.*s",
          (int)GRN_TEXT_LEN(&inspected),
          GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      GRN_OUTPUT_CSTR("");
      grn_obj_unlink(ctx, normalizer);
      return NULL;
    }

    grn_string = grn_string_open(ctx,
                                 GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                                 normalizer, flags);
    grn_obj_unlink(ctx, normalizer);

    GRN_OUTPUT_MAP_OPEN("RESULT", 3);
    {
      const char *normalized;

      grn_string_get_normalized(ctx, grn_string,
                                &normalized,
                                &normalized_length_in_bytes,
                                &normalized_n_characters);
      GRN_OUTPUT_CSTR("normalized");
      GRN_OUTPUT_STR(normalized, normalized_length_in_bytes);
    }
    {
      const unsigned char *types;

      types = grn_string_get_types(ctx, grn_string);
      GRN_OUTPUT_CSTR("types");
      if (types) {
        unsigned int i;
        GRN_OUTPUT_ARRAY_OPEN("types", normalized_n_characters);
        for (i = 0; i < normalized_n_characters; i++) {
          GRN_OUTPUT_CSTR(char_type_name(types[i]));
        }
        GRN_OUTPUT_ARRAY_CLOSE();
      } else {
        GRN_OUTPUT_ARRAY_OPEN("types", 0);
        GRN_OUTPUT_ARRAY_CLOSE();
      }
    }
    {
      const short *checks;

      checks = grn_string_get_checks(ctx, grn_string);
      GRN_OUTPUT_CSTR("checks");
      if (checks) {
        unsigned int i;
        GRN_OUTPUT_ARRAY_OPEN("checks", normalized_length_in_bytes);
        for (i = 0; i < normalized_length_in_bytes; i++) {
          GRN_OUTPUT_INT32(checks[i]);
        }
        GRN_OUTPUT_ARRAY_CLOSE();
      } else {
        GRN_OUTPUT_ARRAY_OPEN("checks", 0);
        GRN_OUTPUT_ARRAY_CLOSE();
      }
    }
    GRN_OUTPUT_MAP_CLOSE();

    grn_obj_unlink(ctx, grn_string);
  }

  return NULL;
}

static unsigned int
parse_tokenize_flags(grn_ctx *ctx, grn_obj *flag_names)
{
  unsigned int flags = 0;
  const char *names, *names_end;
  int length;

  names = GRN_TEXT_VALUE(flag_names);
  length = GRN_TEXT_LEN(flag_names);
  names_end = names + length;
  while (names < names_end) {
    if (*names == '|' || *names == ' ') {
      names += 1;
      continue;
    }

#define CHECK_FLAG(name)\
    if (((names_end - names) >= (sizeof(#name) - 1)) &&\
        (!memcmp(names, #name, sizeof(#name) - 1))) {\
      flags |= GRN_TOKEN_CURSOR_ ## name;\
      names += sizeof(#name) - 1;\
      continue;\
    }

    CHECK_FLAG(ENABLE_TOKENIZED_DELIMITER);

#define GRN_TOKEN_CURSOR_NONE 0
    CHECK_FLAG(NONE);
#undef GRN_TOKEN_CURSOR_NONE

    ERR(GRN_INVALID_ARGUMENT, "[tokenize] invalid flag: <%.*s>",
        (int)(names_end - names), names);
    return 0;
#undef CHECK_FLAG
  }

  return flags;
}

typedef struct {
  grn_id id;
  int32_t position;
} tokenize_token;

static void
output_tokens(grn_ctx *ctx, grn_obj *tokens, grn_obj *lexicon)
{
  int i, n_tokens;

  n_tokens = GRN_BULK_VSIZE(tokens) / sizeof(tokenize_token);
  GRN_OUTPUT_ARRAY_OPEN("TOKENS", n_tokens);
  for (i = 0; i < n_tokens; i++) {
    tokenize_token *token;
    char value[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int value_size;

    token = ((tokenize_token *)(GRN_BULK_HEAD(tokens))) + i;

    GRN_OUTPUT_MAP_OPEN("TOKEN", 2);

    GRN_OUTPUT_CSTR("value");
    value_size = grn_table_get_key(ctx, lexicon, token->id,
                                   value, GRN_TABLE_MAX_KEY_SIZE);
    GRN_OUTPUT_STR(value, value_size);

    GRN_OUTPUT_CSTR("position");
    GRN_OUTPUT_INT32(token->position);

    GRN_OUTPUT_MAP_CLOSE();
  }
  GRN_OUTPUT_ARRAY_CLOSE();
}

static grn_obj *
create_lexicon_for_tokenize(grn_ctx *ctx,
                            grn_obj *tokenizer_name,
                            grn_obj *normalizer_name,
                            grn_obj *token_filter_names)
{
  grn_obj *lexicon;
  grn_obj *tokenizer;
  grn_obj *normalizer = NULL;

  tokenizer = grn_ctx_get(ctx,
                          GRN_TEXT_VALUE(tokenizer_name),
                          GRN_TEXT_LEN(tokenizer_name));
  if (!tokenizer) {
    ERR(GRN_INVALID_ARGUMENT,
        "[tokenize] nonexistent tokenizer: <%.*s>",
        (int)GRN_TEXT_LEN(tokenizer_name),
        GRN_TEXT_VALUE(tokenizer_name));
    return NULL;
  }

  if (!is_tokenizer(ctx, tokenizer)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, tokenizer);
    ERR(GRN_INVALID_ARGUMENT,
        "[tokenize] not tokenizer: %.*s",
        (int)GRN_TEXT_LEN(&inspected),
        GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    grn_obj_unlink(ctx, tokenizer);
    return NULL;
  }

  if (GRN_TEXT_LEN(normalizer_name) > 0) {
    normalizer = grn_ctx_get(ctx,
                             GRN_TEXT_VALUE(normalizer_name),
                             GRN_TEXT_LEN(normalizer_name));
    if (!normalizer) {
      grn_obj_unlink(ctx, tokenizer);
      ERR(GRN_INVALID_ARGUMENT,
          "[tokenize] nonexistent normalizer: <%.*s>",
          (int)GRN_TEXT_LEN(normalizer_name),
          GRN_TEXT_VALUE(normalizer_name));
      return NULL;
    }

    if (!is_normalizer(ctx, normalizer)) {
      grn_obj inspected;
      grn_obj_unlink(ctx, tokenizer);
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, normalizer);
      ERR(GRN_INVALID_ARGUMENT,
          "[tokenize] not normalizer: %.*s",
          (int)GRN_TEXT_LEN(&inspected),
          GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      grn_obj_unlink(ctx, normalizer);
      return NULL;
    }
  }

  lexicon = grn_table_create(ctx, NULL, 0,
                             NULL,
                             GRN_OBJ_TABLE_HASH_KEY,
                             grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
                             NULL);
  grn_obj_set_info(ctx, lexicon,
                   GRN_INFO_DEFAULT_TOKENIZER, tokenizer);
  grn_obj_unlink(ctx, tokenizer);
  if (normalizer) {
    grn_obj_set_info(ctx, lexicon,
                     GRN_INFO_NORMALIZER, normalizer);
    grn_obj_unlink(ctx, normalizer);
  }
  proc_table_create_set_token_filters(ctx, lexicon, token_filter_names);

  return lexicon;
}

static void
tokenize(grn_ctx *ctx, grn_obj *lexicon, grn_obj *string, grn_tokenize_mode mode,
         unsigned int flags, grn_obj *tokens)
{
  grn_token_cursor *token_cursor;

  token_cursor =
    grn_token_cursor_open(ctx, lexicon,
                          GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                          mode, flags);
  if (!token_cursor) {
    return;
  }

  while (token_cursor->status == GRN_TOKEN_CURSOR_DOING) {
    grn_id token_id = grn_token_cursor_next(ctx, token_cursor);
    tokenize_token *current_token;
    if (token_id == GRN_ID_NIL) {
      continue;
    }
    grn_bulk_space(ctx, tokens, sizeof(tokenize_token));
    current_token = ((tokenize_token *)(GRN_BULK_CURR(tokens))) - 1;
    current_token->id = token_id;
    current_token->position = token_cursor->pos;
  }
  grn_token_cursor_close(ctx, token_cursor);
}

static grn_obj *
proc_tokenize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *tokenizer_name;
  grn_obj *string;
  grn_obj *normalizer_name;
  grn_obj *flag_names;
  grn_obj *mode_name;
  grn_obj *token_filter_names;

  tokenizer_name = VAR(0);
  string = VAR(1);
  normalizer_name = VAR(2);
  flag_names = VAR(3);
  mode_name = VAR(4);
  token_filter_names = VAR(5);

  if (GRN_TEXT_LEN(tokenizer_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[tokenize] tokenizer name is missing");
    return NULL;
  }

  if (GRN_TEXT_LEN(string) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[tokenize] string is missing");
    return NULL;
  }

  {
    unsigned int flags;
    grn_obj *lexicon;

    flags = parse_tokenize_flags(ctx, flag_names);
    if (ctx->rc != GRN_SUCCESS) {
      return NULL;
    }

    lexicon = create_lexicon_for_tokenize(ctx,
                                          tokenizer_name,
                                          normalizer_name,
                                          token_filter_names);
    if (!lexicon) {
      return NULL;
    }

#define MODE_NAME_EQUAL(name)\
    (GRN_TEXT_LEN(mode_name) == strlen(name) &&\
     memcmp(GRN_TEXT_VALUE(mode_name), name, strlen(name)) == 0)

    {
      grn_obj tokens;
      GRN_VALUE_FIX_SIZE_INIT(&tokens, GRN_OBJ_VECTOR, GRN_ID_NIL);
      if (GRN_TEXT_LEN(mode_name) == 0 || MODE_NAME_EQUAL("ADD")) {
        tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
        output_tokens(ctx, &tokens, lexicon);
      } else if (MODE_NAME_EQUAL("GET")) {
        tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
        GRN_BULK_REWIND(&tokens);
        tokenize(ctx, lexicon, string, GRN_TOKEN_GET, flags, &tokens);
        output_tokens(ctx, &tokens, lexicon);
      } else {
        ERR(GRN_INVALID_ARGUMENT, "[tokenize] invalid mode: <%.*s>",
            (int)GRN_TEXT_LEN(mode_name), GRN_TEXT_VALUE(mode_name));
      }
      GRN_OBJ_FIN(ctx, &tokens);
    }
#undef MODE_NAME_EQUAL

    grn_obj_unlink(ctx, lexicon);
  }

  return NULL;
}

static grn_obj *
proc_table_tokenize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table_name;
  grn_obj *string;
  grn_obj *flag_names;
  grn_obj *mode_name;

  table_name = VAR(0);
  string = VAR(1);
  flag_names = VAR(2);
  mode_name = VAR(3);

  if (GRN_TEXT_LEN(table_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[table_tokenize] table name is missing");
    return NULL;
  }

  if (GRN_TEXT_LEN(string) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[table_tokenize] string is missing");
    return NULL;
  }

  {
    unsigned int flags;
    grn_obj *lexicon;

    flags = parse_tokenize_flags(ctx, flag_names);
    if (ctx->rc != GRN_SUCCESS) {
      return NULL;
    }

    lexicon = grn_ctx_get(ctx, GRN_TEXT_VALUE(table_name), GRN_TEXT_LEN(table_name));

    if (!lexicon) {
      return NULL;
    }

#define MODE_NAME_EQUAL(name)\
    (GRN_TEXT_LEN(mode_name) == strlen(name) &&\
     memcmp(GRN_TEXT_VALUE(mode_name), name, strlen(name)) == 0)

    {
      grn_obj tokens;
      GRN_VALUE_FIX_SIZE_INIT(&tokens, GRN_OBJ_VECTOR, GRN_ID_NIL);
    if (GRN_TEXT_LEN(mode_name) == 0 || MODE_NAME_EQUAL("GET")) {
      tokenize(ctx, lexicon, string, GRN_TOKEN_GET, flags, &tokens);
      output_tokens(ctx, &tokens, lexicon);
    } else if (MODE_NAME_EQUAL("ADD")) {
      tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
      output_tokens(ctx, &tokens, lexicon);
    } else {
      ERR(GRN_INVALID_ARGUMENT, "[table_tokenize] invalid mode: <%.*s>",
          (int)GRN_TEXT_LEN(mode_name), GRN_TEXT_VALUE(mode_name));
    }
      GRN_OBJ_FIN(ctx, &tokens);
    }
#undef MODE_NAME_EQUAL

    grn_obj_unlink(ctx, lexicon);
  }

  return NULL;
}

static void
list_proc(grn_ctx *ctx, grn_proc_type target_proc_type,
          const char *name, const char *plural_name)
{
  grn_obj *db;
  grn_table_cursor *cursor;
  grn_obj target_procs;

  db = grn_ctx_db(ctx);
  cursor = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                 GRN_CURSOR_BY_ID);
  if (!cursor) {
    return;
  }

  GRN_PTR_INIT(&target_procs, GRN_OBJ_VECTOR, GRN_ID_NIL);
  {
    grn_id id;

    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_obj *obj;
      grn_proc_type proc_type;

      obj = grn_ctx_at(ctx, id);
      if (!obj) {
        continue;
      }

      if (obj->header.type != GRN_PROC) {
        grn_obj_unlink(ctx, obj);
        continue;
      }

      proc_type = grn_proc_get_type(ctx, obj);
      if (proc_type != target_proc_type) {
        grn_obj_unlink(ctx, obj);
        continue;
      }

      GRN_PTR_PUT(ctx, &target_procs, obj);
    }
    grn_table_cursor_close(ctx, cursor);

    {
      int i, n_procs;

      n_procs = GRN_BULK_VSIZE(&target_procs) / sizeof(grn_obj *);
      GRN_OUTPUT_ARRAY_OPEN(plural_name, n_procs);
      for (i = 0; i < n_procs; i++) {
        grn_obj *proc;
        char name[GRN_TABLE_MAX_KEY_SIZE];
        int name_size;

        proc = GRN_PTR_VALUE_AT(&target_procs, i);
        name_size = grn_obj_name(ctx, proc, name, GRN_TABLE_MAX_KEY_SIZE);
        GRN_OUTPUT_MAP_OPEN(name, 1);
        GRN_OUTPUT_CSTR("name");
        GRN_OUTPUT_STR(name, name_size);
        GRN_OUTPUT_MAP_CLOSE();

        grn_obj_unlink(ctx, proc);
      }
      GRN_OUTPUT_ARRAY_CLOSE();
    }

    grn_obj_unlink(ctx, &target_procs);
  }
}

static grn_obj *
proc_tokenizer_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  list_proc(ctx, GRN_PROC_TOKENIZER, "tokenizer", "tokenizers");
  return NULL;
}

static grn_obj *
proc_normalizer_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  list_proc(ctx, GRN_PROC_NORMALIZER, "normalizer", "normalizers");
  return NULL;
}

static grn_obj *
func_rand(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  int val;
  grn_obj *obj;
  if (nargs > 0) {
    int max = GRN_INT32_VALUE(args[0]);
    val = (int) (1.0 * max * rand() / (RAND_MAX + 1.0));
  } else {
    val = rand();
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_INT32, 0))) {
    GRN_INT32_SET(ctx, obj, val);
  }
  return obj;
}

static grn_obj *
func_now(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  if ((obj = GRN_PROC_ALLOC(GRN_DB_TIME, 0))) {
    GRN_TIME_NOW(ctx, obj);
  }
  return obj;
}

static inline grn_bool
is_comparable_number_type(grn_id type)
{
  return GRN_DB_INT8 <= type && type <= GRN_DB_TIME;
}

static inline grn_id
larger_number_type(grn_id type1, grn_id type2)
{
  if (type1 == type2) {
    return type1;
  }

  switch (type1) {
  case GRN_DB_FLOAT :
    return type1;
  case GRN_DB_TIME :
    if (type2 == GRN_DB_FLOAT) {
      return type2;
    } else {
      return type1;
    }
  default :
    if (type2 > type1) {
      return type2;
    } else {
      return type1;
    }
  }
}

static inline grn_id
smaller_number_type(grn_id type1, grn_id type2)
{
  if (type1 == type2) {
    return type1;
  }

  switch (type1) {
  case GRN_DB_FLOAT :
    return type1;
  case GRN_DB_TIME :
    if (type2 == GRN_DB_FLOAT) {
      return type2;
    } else {
      return type1;
    }
  default :
    {
      grn_id smaller_number_type;
      if (type2 > type1) {
        smaller_number_type = type2;
      } else {
        smaller_number_type = type1;
      }
      switch (smaller_number_type) {
      case GRN_DB_UINT8 :
        return GRN_DB_INT8;
      case GRN_DB_UINT16 :
        return GRN_DB_INT16;
      case GRN_DB_UINT32 :
        return GRN_DB_INT32;
      case GRN_DB_UINT64 :
        return GRN_DB_INT64;
      default :
        return smaller_number_type;
      }
    }
  }
}

static inline grn_bool
is_negative_value(grn_obj *number)
{
  switch (number->header.domain) {
  case GRN_DB_INT8 :
    return GRN_INT8_VALUE(number) < 0;
  case GRN_DB_INT16 :
    return GRN_INT16_VALUE(number) < 0;
  case GRN_DB_INT32 :
    return GRN_INT32_VALUE(number) < 0;
  case GRN_DB_INT64 :
    return GRN_INT64_VALUE(number) < 0;
  case GRN_DB_TIME :
    return GRN_TIME_VALUE(number) < 0;
  case GRN_DB_FLOAT :
    return GRN_FLOAT_VALUE(number) < 0;
  default :
    return GRN_FALSE;
  }
}

static inline grn_bool
number_safe_cast(grn_ctx *ctx, grn_obj *src, grn_obj *dest, grn_id type)
{
  grn_obj_reinit(ctx, dest, type, 0);
  if (src->header.domain == type) {
    GRN_TEXT_SET(ctx, dest, GRN_TEXT_VALUE(src), GRN_TEXT_LEN(src));
    return GRN_TRUE;
  }

  switch (type) {
  case GRN_DB_UINT8 :
    if (is_negative_value(src)) {
      GRN_UINT8_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  case GRN_DB_UINT16 :
    if (is_negative_value(src)) {
      GRN_UINT16_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  case GRN_DB_UINT32 :
    if (is_negative_value(src)) {
      GRN_UINT32_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  case GRN_DB_UINT64 :
    if (is_negative_value(src)) {
      GRN_UINT64_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  default :
    return grn_obj_cast(ctx, src, dest, GRN_FALSE) == GRN_SUCCESS;
  }
}

static inline int
compare_number(grn_ctx *ctx, grn_obj *number1, grn_obj *number2, grn_id type)
{
#define COMPARE_AND_RETURN(type, value1, value2)\
  {\
    type computed_value1 = value1;\
    type computed_value2 = value2;\
    if (computed_value1 > computed_value2) {\
      return 1;\
    } else if (computed_value1 < computed_value2) {\
      return -1;\
    } else {\
      return 0;\
    }\
  }

  switch (type) {
  case GRN_DB_INT8 :
    COMPARE_AND_RETURN(int8_t,
                       GRN_INT8_VALUE(number1),
                       GRN_INT8_VALUE(number2));
  case GRN_DB_UINT8 :
    COMPARE_AND_RETURN(uint8_t,
                       GRN_UINT8_VALUE(number1),
                       GRN_UINT8_VALUE(number2));
  case GRN_DB_INT16 :
    COMPARE_AND_RETURN(int16_t,
                       GRN_INT16_VALUE(number1),
                       GRN_INT16_VALUE(number2));
  case GRN_DB_UINT16 :
    COMPARE_AND_RETURN(uint16_t,
                       GRN_UINT16_VALUE(number1),
                       GRN_UINT16_VALUE(number2));
  case GRN_DB_INT32 :
    COMPARE_AND_RETURN(int32_t,
                       GRN_INT32_VALUE(number1),
                       GRN_INT32_VALUE(number2));
  case GRN_DB_UINT32 :
    COMPARE_AND_RETURN(uint32_t,
                       GRN_UINT32_VALUE(number1),
                       GRN_UINT32_VALUE(number2));
  case GRN_DB_INT64 :
    COMPARE_AND_RETURN(int64_t,
                       GRN_INT64_VALUE(number1),
                       GRN_INT64_VALUE(number2));
  case GRN_DB_UINT64 :
    COMPARE_AND_RETURN(uint64_t,
                       GRN_UINT64_VALUE(number1),
                       GRN_UINT64_VALUE(number2));
  case GRN_DB_FLOAT :
    COMPARE_AND_RETURN(double,
                       GRN_FLOAT_VALUE(number1),
                       GRN_FLOAT_VALUE(number2));
  case GRN_DB_TIME :
    COMPARE_AND_RETURN(int64_t,
                       GRN_TIME_VALUE(number1),
                       GRN_TIME_VALUE(number2));
  default :
    return 0;
  }

#undef COMPARE_AND_RETURN
}

static grn_obj *
func_max(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *max;
  grn_id cast_type = GRN_DB_INT8;
  grn_obj casted_max, casted_number;
  int i;

  max = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  if (!max) {
    return max;
  }

  GRN_VOID_INIT(&casted_max);
  GRN_VOID_INIT(&casted_number);
  for (i = 0; i < nargs; i++) {
    grn_obj *number = args[i];
    grn_id domain = number->header.domain;
    if (!is_comparable_number_type(domain)) {
      continue;
    }
    cast_type = larger_number_type(cast_type, domain);
    if (!number_safe_cast(ctx, number, &casted_number, cast_type)) {
      continue;
    }
    if (max->header.domain == GRN_DB_VOID) {
      grn_obj_reinit(ctx, max, cast_type, 0);
      GRN_TEXT_SET(ctx, max,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
      continue;
    }

    if (max->header.domain != cast_type) {
      if (!number_safe_cast(ctx, max, &casted_max, cast_type)) {
        continue;
      }
      grn_obj_reinit(ctx, max, cast_type, 0);
      GRN_TEXT_SET(ctx, max,
                   GRN_TEXT_VALUE(&casted_max),
                   GRN_TEXT_LEN(&casted_max));
    }
    if (compare_number(ctx, &casted_number, max, cast_type) > 0) {
      grn_obj_reinit(ctx, max, cast_type, 0);
      GRN_TEXT_SET(ctx, max,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
    }
  }
  GRN_OBJ_FIN(ctx, &casted_max);
  GRN_OBJ_FIN(ctx, &casted_number);

  return max;
}

static grn_obj *
func_min(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *min;
  grn_id cast_type = GRN_DB_INT8;
  grn_obj casted_min, casted_number;
  int i;

  min = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  if (!min) {
    return min;
  }

  GRN_VOID_INIT(&casted_min);
  GRN_VOID_INIT(&casted_number);
  for (i = 0; i < nargs; i++) {
    grn_obj *number = args[i];
    grn_id domain = number->header.domain;
    if (!is_comparable_number_type(domain)) {
      continue;
    }
    cast_type = smaller_number_type(cast_type, domain);
    if (!number_safe_cast(ctx, number, &casted_number, cast_type)) {
      continue;
    }
    if (min->header.domain == GRN_DB_VOID) {
      grn_obj_reinit(ctx, min, cast_type, 0);
      GRN_TEXT_SET(ctx, min,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
      continue;
    }

    if (min->header.domain != cast_type) {
      if (!number_safe_cast(ctx, min, &casted_min, cast_type)) {
        continue;
      }
      grn_obj_reinit(ctx, min, cast_type, 0);
      GRN_TEXT_SET(ctx, min,
                   GRN_TEXT_VALUE(&casted_min),
                   GRN_TEXT_LEN(&casted_min));
    }
    if (compare_number(ctx, &casted_number, min, cast_type) < 0) {
      grn_obj_reinit(ctx, min, cast_type, 0);
      GRN_TEXT_SET(ctx, min,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
    }
  }
  GRN_OBJ_FIN(ctx, &casted_min);
  GRN_OBJ_FIN(ctx, &casted_number);

  return min;
}

static grn_obj *
func_geo_in_circle(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  unsigned char r = GRN_FALSE;
  grn_geo_approximate_type type = GRN_GEO_APPROXIMATE_RECTANGLE;
  switch (nargs) {
  case 4 :
    if (grn_geo_resolve_approximate_type(ctx, args[3], &type) != GRN_SUCCESS) {
      break;
    }
    /* fallthru */
  case 3 :
    r = grn_geo_in_circle(ctx, args[0], args[1], args[2], type);
    break;
  default :
    break;
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, r);
  }
  return obj;
}

static grn_obj *
func_geo_in_rectangle(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  unsigned char r = GRN_FALSE;
  if (nargs == 3) {
    r = grn_geo_in_rectangle(ctx, args[0], args[1], args[2]);
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, r);
  }
  return obj;
}

static grn_obj *
func_geo_distance(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  double d = 0.0;
  grn_geo_approximate_type type = GRN_GEO_APPROXIMATE_RECTANGLE;
  switch (nargs) {
  case 3 :
    if (grn_geo_resolve_approximate_type(ctx, args[2], &type) != GRN_SUCCESS) {
      break;
    }
    /* fallthru */
  case 2 :
    d = grn_geo_distance(ctx, args[0], args[1], type);
    break;
  default:
    break;
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
}

/* deprecated. */
static grn_obj *
func_geo_distance2(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  double d = 0;
  if (nargs == 2) {
    d = grn_geo_distance_sphere(ctx, args[0], args[1]);
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
}

/* deprecated. */
static grn_obj *
func_geo_distance3(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  double d = 0;
  if (nargs == 2) {
    d = grn_geo_distance_ellipsoid(ctx, args[0], args[1]);
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
}

#define DIST(ox,oy) (dists[((lx + 1) * (oy)) + (ox)])

static grn_obj *
func_edit_distance(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  int d = 0;
  grn_obj *obj;
  if (nargs == 2) {
    uint32_t cx, lx, cy, ly, *dists;
    char *px, *sx = GRN_TEXT_VALUE(args[0]), *ex = GRN_BULK_CURR(args[0]);
    char *py, *sy = GRN_TEXT_VALUE(args[1]), *ey = GRN_BULK_CURR(args[1]);
    for (px = sx, lx = 0; px < ex && (cx = grn_charlen(ctx, px, ex)); px += cx, lx++);
    for (py = sy, ly = 0; py < ey && (cy = grn_charlen(ctx, py, ey)); py += cy, ly++);
    if ((dists = GRN_MALLOC((lx + 1) * (ly + 1) * sizeof(uint32_t)))) {
      uint32_t x, y;
      for (x = 0; x <= lx; x++) { DIST(x, 0) = x; }
      for (y = 0; y <= ly; y++) { DIST(0, y) = y; }
      for (x = 1, px = sx; x <= lx; x++, px += cx) {
        cx = grn_charlen(ctx, px, ex);
        for (y = 1, py = sy; y <= ly; y++, py += cy) {
          cy = grn_charlen(ctx, py, ey);
          if (cx == cy && !memcmp(px, py, cx)) {
            DIST(x, y) = DIST(x - 1, y - 1);
          } else {
            uint32_t a = DIST(x - 1, y) + 1;
            uint32_t b = DIST(x, y - 1) + 1;
            uint32_t c = DIST(x - 1, y - 1) + 1;
            DIST(x, y) = ((a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c));
          }
        }
      }
      d = DIST(lx, ly);
      GRN_FREE(dists);
    }
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, d);
  }
  return obj;
}

static grn_obj *
func_all_records(grn_ctx *ctx, int nargs, grn_obj **args,
                 grn_user_data *user_data)
{
  grn_obj *true_value;
  if ((true_value = GRN_PROC_ALLOC(GRN_DB_BOOL, 0))) {
    GRN_BOOL_SET(ctx, true_value, GRN_TRUE);
  }
  return true_value;
}

static grn_rc
selector_all_records(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                     int nargs, grn_obj **args,
                     grn_obj *res, grn_operator op)
{
  grn_ii_posting posting;

  memset(&posting, 0, sizeof(grn_ii_posting));
  GRN_TABLE_EACH(ctx, table, 0, 0, id, NULL, NULL, NULL, {
    posting.rid = id;
    grn_ii_posting_add(ctx, &posting, (grn_hash *)res, GRN_OP_OR);
  });

  return ctx->rc;
}

static grn_obj *
snippet_exec(grn_ctx *ctx, grn_obj *snip, grn_obj *text,
             grn_user_data *user_data)
{
  grn_rc rc;
  unsigned int i, n_results, max_tagged_length;
  grn_obj snippet_buffer;
  grn_obj *snippets;

  if (GRN_TEXT_LEN(text) == 0) {
    return NULL;
  }

  rc = grn_snip_exec(ctx, snip,
                     GRN_TEXT_VALUE(text), GRN_TEXT_LEN(text),
                     &n_results, &max_tagged_length);
  if (rc != GRN_SUCCESS) {
    return NULL;
  }

  if (n_results == 0) {
    return GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  }

  snippets = GRN_PROC_ALLOC(GRN_DB_SHORT_TEXT, GRN_OBJ_VECTOR);
  if (!snippets) {
    return NULL;
  }

  GRN_TEXT_INIT(&snippet_buffer, 0);
  grn_bulk_space(ctx, &snippet_buffer, max_tagged_length);
  for (i = 0; i < n_results; i++) {
    unsigned int snippet_length;

    GRN_BULK_REWIND(&snippet_buffer);
    rc = grn_snip_get_result(ctx, snip, i,
                             GRN_TEXT_VALUE(&snippet_buffer),
                             &snippet_length);
    if (rc == GRN_SUCCESS) {
      grn_vector_add_element(ctx, snippets,
                             GRN_TEXT_VALUE(&snippet_buffer), snippet_length,
                             0, GRN_DB_SHORT_TEXT);
    }
  }
  GRN_OBJ_FIN(ctx, &snippet_buffer);

  return snippets;
}

static grn_obj *
func_snippet_html(grn_ctx *ctx, int nargs, grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *snippets = NULL;

  /* TODO: support parameters */
  if (nargs == 1) {
    grn_obj *text = args[0];
    grn_obj *expression = NULL;
    grn_obj *condition_ptr = NULL;
    grn_obj *condition = NULL;
    grn_obj *snip = NULL;
    int flags = GRN_SNIP_SKIP_LEADING_SPACES;
    unsigned int width = 200;
    unsigned int max_n_results = 3;
    const char *open_tag = "<span class=\"keyword\">";
    const char *close_tag = "</span>";
    grn_snip_mapping *mapping = GRN_SNIP_MAPPING_HTML_ESCAPE;

    grn_proc_get_info(ctx, user_data, NULL, NULL, &expression);
    condition_ptr = grn_expr_get_var(ctx, expression,
                                     GRN_SELECT_INTERNAL_VAR_CONDITION,
                                     strlen(GRN_SELECT_INTERNAL_VAR_CONDITION));
    if (condition_ptr) {
      condition = GRN_PTR_VALUE(condition_ptr);
    }

    if (condition) {
      snip = grn_snip_open(ctx, flags, width, max_n_results,
                           open_tag, strlen(open_tag),
                           close_tag, strlen(close_tag),
                           mapping);
      if (snip) {
        grn_snip_set_normalizer(ctx, snip, GRN_NORMALIZER_AUTO);
        grn_expr_snip_add_conditions(ctx, condition, snip,
                                     0, NULL, NULL, NULL, NULL);
      }
    }

    if (snip) {
      snippets = snippet_exec(ctx, snip, text, user_data);
      grn_obj_close(ctx, snip);
    }
  }

  if (!snippets) {
    snippets = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  }

  return snippets;
}

typedef struct {
  grn_obj *found;
  grn_obj *table;
  grn_obj *records;
} selector_to_function_data;

static grn_bool
selector_to_function_data_init(grn_ctx *ctx,
                               selector_to_function_data *data,
                               grn_user_data *user_data)
{
  grn_obj *condition = NULL;
  grn_obj *variable;

  data->table = NULL;
  data->records = NULL;

  data->found = GRN_PROC_ALLOC(GRN_DB_BOOL, 0);
  if (!data->found) {
    return GRN_FALSE;
  }
  GRN_BOOL_SET(ctx, data->found, GRN_FALSE);

  grn_proc_get_info(ctx, user_data, NULL, NULL, &condition);
  if (!condition) {
    return GRN_FALSE;
  }

  variable = grn_expr_get_var_by_offset(ctx, condition, 0);
  if (!variable) {
    return GRN_FALSE;
  }

  data->table = grn_ctx_at(ctx, variable->header.domain);
  if (!data->table) {
    return GRN_FALSE;
  }

  data->records = grn_table_create(ctx, NULL, 0, NULL,
                                   GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                   data->table, NULL);
  if (!data->records) {
    return GRN_FALSE;
  }

  {
    grn_rset_posinfo pi;
    unsigned int key_size;
    memset(&pi, 0, sizeof(grn_rset_posinfo));
    pi.rid = GRN_RECORD_VALUE(variable);
    key_size = ((grn_hash *)(data->records))->key_size;
    if (grn_table_add(ctx, data->records, &pi, key_size, NULL) == GRN_ID_NIL) {
      return GRN_FALSE;
    }
  }

  return GRN_TRUE;
}

static void
selector_to_function_data_selected(grn_ctx *ctx,
                                   selector_to_function_data *data)
{
  GRN_BOOL_SET(ctx, data->found, grn_table_size(ctx, data->records) > 0);
}

static void
selector_to_function_data_fin(grn_ctx *ctx,
                              selector_to_function_data *data)
{
  if (data->records) {
    grn_obj_unlink(ctx, data->records);
  }

  if (data->table) {
    grn_obj_unlink(ctx, data->table);
  }
}

static grn_rc
run_query(grn_ctx *ctx, grn_obj *table,
          int nargs, grn_obj **args,
          grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *match_columns_string;
  grn_obj *query;
  grn_obj *query_expander_name = NULL;
  grn_obj *match_columns = NULL;
  grn_obj *condition = NULL;
  grn_obj *dummy_variable;

  /* TODO: support flags by parameters */
  if (!(2 <= nargs && nargs <= 3)) {
    ERR(GRN_INVALID_ARGUMENT,
        "wrong number of arguments (%d for 2..3)", nargs);
    rc = ctx->rc;
    goto exit;
  }

  match_columns_string = args[0];
  query = args[1];
  if (nargs > 2) {
    query_expander_name = args[2];
  }

  if (match_columns_string->header.domain == GRN_DB_TEXT &&
      GRN_TEXT_LEN(match_columns_string) > 0) {
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, match_columns, dummy_variable);
    if (!match_columns) {
      rc = ctx->rc;
      goto exit;
    }

    grn_expr_parse(ctx, match_columns,
                   GRN_TEXT_VALUE(match_columns_string),
                   GRN_TEXT_LEN(match_columns_string),
                   NULL, GRN_OP_MATCH, GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      rc = ctx->rc;
      goto exit;
    }
  }

  if (query->header.domain == GRN_DB_TEXT && GRN_TEXT_LEN(query) > 0) {
    const char *query_string;
    unsigned int query_string_len;
    grn_obj expanded_query;
    grn_expr_flags flags =
      GRN_EXPR_SYNTAX_QUERY|GRN_EXPR_ALLOW_PRAGMA|GRN_EXPR_ALLOW_COLUMN;

    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, condition, dummy_variable);
    if (!condition) {
      rc = ctx->rc;
      goto exit;
    }

    query_string = GRN_TEXT_VALUE(query);
    query_string_len = GRN_TEXT_LEN(query);

    GRN_TEXT_INIT(&expanded_query, 0);
    if (query_expander_name &&
        query_expander_name->header.domain == GRN_DB_TEXT &&
        GRN_TEXT_LEN(query_expander_name) > 0) {
      rc = expand_query(ctx, query_string, query_string_len, flags,
                        GRN_TEXT_VALUE(query_expander_name),
                        GRN_TEXT_LEN(query_expander_name),
                        &expanded_query);
      if (rc != GRN_SUCCESS) {
        GRN_OBJ_FIN(ctx, &expanded_query);
        goto exit;
      }
      query_string = GRN_TEXT_VALUE(&expanded_query);
      query_string_len = GRN_TEXT_LEN(&expanded_query);
    }
    grn_expr_parse(ctx, condition,
                   query_string,
                   query_string_len,
                   match_columns, GRN_OP_MATCH, GRN_OP_AND, flags);
    rc = ctx->rc;
    GRN_OBJ_FIN(ctx, &expanded_query);
    if (rc != GRN_SUCCESS) {
      goto exit;
    }
    grn_table_select(ctx, table, condition, res, op);
    rc = ctx->rc;
  }

exit :
  if (match_columns) {
    grn_obj_unlink(ctx, match_columns);
  }
  if (condition) {
    grn_obj_unlink(ctx, condition);
  }

  return rc;
}

static grn_obj *
func_query(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  selector_to_function_data data;

  if (selector_to_function_data_init(ctx, &data, user_data)) {
    grn_rc rc;
    rc = run_query(ctx, data.table, nargs, args, data.records, GRN_OP_AND);
    if (rc == GRN_SUCCESS) {
      selector_to_function_data_selected(ctx, &data);
    }
  }
  selector_to_function_data_fin(ctx, &data);

  return data.found;
}

static grn_rc
selector_query(grn_ctx *ctx, grn_obj *table, grn_obj *index,
               int nargs, grn_obj **args,
               grn_obj *res, grn_operator op)
{
  return run_query(ctx, table, nargs - 1, args + 1, res, op);
}

static grn_rc
run_sub_filter(grn_ctx *ctx, grn_obj *table,
               int nargs, grn_obj **args,
               grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *scope;
  grn_obj *sub_filter_string;
  grn_obj *scope_domain = NULL;
  grn_obj *sub_filter = NULL;
  grn_obj *dummy_variable = NULL;

  if (nargs != 2) {
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): wrong number of arguments (%d for 2)", nargs);
    rc = ctx->rc;
    goto exit;
  }

  scope = args[0];
  sub_filter_string = args[1];

  switch (scope->header.type) {
  case GRN_ACCESSOR :
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
    break;
  default :
    /* TODO: put inspected the 1nd argument to message */
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): the 1nd argument must be column or accessor");
    rc = ctx->rc;
    goto exit;
    break;
  }

  scope_domain = grn_ctx_at(ctx, grn_obj_get_range(ctx, scope));

  if (sub_filter_string->header.domain != GRN_DB_TEXT) {
    /* TODO: put inspected the 2nd argument to message */
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): the 2nd argument must be String");
    rc = ctx->rc;
    goto exit;
  }
  if (GRN_TEXT_LEN(sub_filter_string) == 0) {
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): the 2nd argument must not be empty String");
    rc = ctx->rc;
    goto exit;
  }

  GRN_EXPR_CREATE_FOR_QUERY(ctx, scope_domain, sub_filter, dummy_variable);
  if (!sub_filter) {
    rc = ctx->rc;
    goto exit;
  }

  grn_expr_parse(ctx, sub_filter,
                 GRN_TEXT_VALUE(sub_filter_string),
                 GRN_TEXT_LEN(sub_filter_string),
                 NULL, GRN_OP_MATCH, GRN_OP_AND,
                 GRN_EXPR_SYNTAX_SCRIPT);
  if (ctx->rc != GRN_SUCCESS) {
    rc = ctx->rc;
    goto exit;
  }

  {
    grn_obj *base_res = NULL;
    grn_obj *resolve_res = NULL;

    base_res = grn_table_create(ctx, NULL, 0, NULL,
                                GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                scope_domain, NULL);
    grn_table_select(ctx, scope_domain, sub_filter, base_res, GRN_OP_OR);
    if (scope->header.type == GRN_ACCESSOR) {
      rc = grn_accessor_resolve(ctx, scope, -1, base_res, &resolve_res, NULL);
    } else {
      grn_accessor accessor;
      accessor.header.type = GRN_ACCESSOR;
      accessor.obj = scope;
      accessor.action = GRN_ACCESSOR_GET_COLUMN_VALUE;
      accessor.next = NULL;
      rc = grn_accessor_resolve(ctx, (grn_obj *)&accessor, -1, base_res,
                                &resolve_res, NULL);
    }
    if (resolve_res) {
      rc = grn_table_setoperation(ctx, res, resolve_res, res, op);
      grn_obj_unlink(ctx, resolve_res);
    }
    grn_obj_unlink(ctx, base_res);
  }

exit :
  if (scope_domain) {
    grn_obj_unlink(ctx, scope_domain);
  }
  if (sub_filter) {
    grn_obj_unlink(ctx, sub_filter);
  }

  return rc;
}

static grn_obj *
func_sub_filter(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  selector_to_function_data data;

  if (selector_to_function_data_init(ctx, &data, user_data)) {
    grn_rc rc;
    rc = run_sub_filter(ctx, data.table, nargs, args, data.records, GRN_OP_AND);
    if (rc == GRN_SUCCESS) {
      selector_to_function_data_selected(ctx, &data);
    }
  }
  selector_to_function_data_fin(ctx, &data);

  return data.found;
}

static grn_rc
selector_sub_filter(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                    int nargs, grn_obj **args,
                    grn_obj *res, grn_operator op)
{
  return run_sub_filter(ctx, table, nargs - 1, args + 1, res, op);
}

static grn_obj *
func_html_untag(grn_ctx *ctx, int nargs, grn_obj **args,
                grn_user_data *user_data)
{
  grn_obj *html_arg;
  int html_arg_domain;
  grn_obj html;
  grn_obj *text;
  const char *html_raw;
  int i, length;
  grn_bool in_tag = GRN_FALSE;

  if (nargs != 1) {
    ERR(GRN_INVALID_ARGUMENT, "HTML is missing");
    return NULL;
  }

  html_arg = args[0];
  html_arg_domain = html_arg->header.domain;
  switch (html_arg_domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    GRN_VALUE_VAR_SIZE_INIT(&html, GRN_OBJ_DO_SHALLOW_COPY, html_arg_domain);
    GRN_TEXT_SET(ctx, &html, GRN_TEXT_VALUE(html_arg), GRN_TEXT_LEN(html_arg));
    break;
  default :
    GRN_TEXT_INIT(&html, 0);
    if (grn_obj_cast(ctx, html_arg, &html, GRN_FALSE)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, html_arg);
      ERR(GRN_INVALID_ARGUMENT, "failed to cast to text: <%.*s>",
          (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      GRN_OBJ_FIN(ctx, &html);
      return NULL;
    }
    break;
  }

  text = GRN_PROC_ALLOC(html.header.domain, 0);
  if (!text) {
    GRN_OBJ_FIN(ctx, &html);
    return NULL;
  }

  html_raw = GRN_TEXT_VALUE(&html);
  length = GRN_TEXT_LEN(&html);
  for (i = 0; i < length; i++) {
    switch (html_raw[i]) {
    case '<' :
      in_tag = GRN_TRUE;
      break;
    case '>' :
      if (in_tag) {
        in_tag = GRN_FALSE;
      } else {
        GRN_TEXT_PUTC(ctx, text, html_raw[i]);
      }
      break;
    default :
      if (!in_tag) {
        GRN_TEXT_PUTC(ctx, text, html_raw[i]);
      }
      break;
    }
  }

  GRN_OBJ_FIN(ctx, &html);

  return text;
}

static grn_bool
grn_text_equal_cstr(grn_ctx *ctx, grn_obj *text, const char *cstr)
{
  int cstr_len;

  cstr_len = strlen(cstr);
  return (GRN_TEXT_LEN(text) == cstr_len &&
          strncmp(GRN_TEXT_VALUE(text), cstr, cstr_len) == 0);
}

typedef enum {
  BETWEEN_BORDER_INVALID,
  BETWEEN_BORDER_INCLUDE,
  BETWEEN_BORDER_EXCLUDE
} between_border_type;

typedef struct {
  grn_obj *value;
  grn_obj *min;
  grn_obj casted_min;
  between_border_type min_border_type;
  grn_obj *max;
  grn_obj casted_max;
  between_border_type max_border_type;
} between_data;

static void
between_data_init(grn_ctx *ctx, between_data *data)
{
  GRN_VOID_INIT(&(data->casted_min));
  GRN_VOID_INIT(&(data->casted_max));
}

static void
between_data_fin(grn_ctx *ctx, between_data *data)
{
  GRN_OBJ_FIN(ctx, &(data->casted_min));
  GRN_OBJ_FIN(ctx, &(data->casted_max));
}

static between_border_type
between_parse_border(grn_ctx *ctx, grn_obj *border,
                     const char *argument_description)
{
  grn_obj inspected;

  /* TODO: support other text types */
  if (border->header.domain == GRN_DB_TEXT) {
    if (grn_text_equal_cstr(ctx, border, "include")) {
      return BETWEEN_BORDER_INCLUDE;
    } else if (grn_text_equal_cstr(ctx, border, "exclude")) {
      return BETWEEN_BORDER_EXCLUDE;
    }
  }

  GRN_TEXT_INIT(&inspected, 0);
  grn_inspect(ctx, &inspected, border);
  ERR(GRN_INVALID_ARGUMENT,
      "between(): %s must be \"include\" or \"exclude\": <%.*s>",
      argument_description,
      (int)GRN_TEXT_LEN(&inspected),
      GRN_TEXT_VALUE(&inspected));
  grn_obj_unlink(ctx, &inspected);

  return BETWEEN_BORDER_INVALID;
}

static grn_rc
between_cast(grn_ctx *ctx, grn_obj *source, grn_obj *destination, grn_id domain,
             const char *target_argument_name)
{
  grn_rc rc;

  GRN_OBJ_INIT(destination, GRN_BULK, 0, domain);
  rc = grn_obj_cast(ctx, source, destination, GRN_FALSE);
  if (rc != GRN_SUCCESS) {
    grn_obj inspected_source;
    grn_obj *domain_object;
    char domain_name[GRN_TABLE_MAX_KEY_SIZE];
    int domain_name_length;

    GRN_TEXT_INIT(&inspected_source, 0);
    grn_inspect(ctx, &inspected_source, source);

    domain_object = grn_ctx_at(ctx, domain);
    domain_name_length =
      grn_obj_name(ctx, domain_object, domain_name, GRN_TABLE_MAX_KEY_SIZE);

    ERR(rc, "between(): failed to cast %s: <%.*s> -> <%.*s>",
        target_argument_name,
        (int)GRN_TEXT_LEN(&inspected_source),
        GRN_TEXT_VALUE(&inspected_source),
        domain_name_length,
        domain_name);

    grn_obj_unlink(ctx, &inspected_source);
    grn_obj_unlink(ctx, domain_object);
  }

  return rc;
}

static grn_rc
between_parse_args(grn_ctx *ctx, int nargs, grn_obj **args, between_data *data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *min_border;
  grn_obj *max_border;

  if (nargs != 5) {
    ERR(GRN_INVALID_ARGUMENT,
        "between(): wrong number of arguments (%d for 5)", nargs);
    rc = ctx->rc;
    goto exit;
  }

  data->value = args[0];
  data->min   = args[1];
  min_border  = args[2];
  data->max   = args[3];
  max_border  = args[4];

  data->min_border_type =
    between_parse_border(ctx, min_border, "the 3rd argument (min_border)");
  if (data->min_border_type == BETWEEN_BORDER_INVALID) {
    rc = ctx->rc;
    goto exit;
  }

  data->max_border_type =
    between_parse_border(ctx, max_border, "the 5th argument (max_border)");
  if (data->max_border_type == BETWEEN_BORDER_INVALID) {
    rc = ctx->rc;
    goto exit;
  }

  {
    grn_id value_type;
    if (data->value->header.type == GRN_BULK) {
      value_type = data->value->header.domain;
    } else {
      value_type = grn_obj_get_range(ctx, data->value);
    }
    if (value_type != data->min->header.domain) {
      rc = between_cast(ctx, data->min, &data->casted_min, value_type, "min");
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      data->min = &(data->casted_min);
    }

    if (value_type != data->max->header.domain) {
      rc = between_cast(ctx, data->max, &data->casted_max, value_type, "max");
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      data->max = &(data->casted_max);
    }
  }

exit :
  return rc;
}

static grn_bool
between_create_expr(grn_ctx *ctx, grn_obj *table, between_data *data,
                    grn_obj **expr, grn_obj **variable)
{
  GRN_EXPR_CREATE_FOR_QUERY(ctx, table, *expr, *variable);
  if (!*expr) {
    return GRN_FALSE;
  }

  if (data->value->header.type == GRN_BULK) {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_PUSH, 1);
  } else {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_GET_VALUE, 1);
  }
  grn_expr_append_obj(ctx, *expr, data->min, GRN_OP_PUSH, 1);
  if (data->min_border_type == BETWEEN_BORDER_INCLUDE) {
    grn_expr_append_op(ctx, *expr, GRN_OP_GREATER_EQUAL, 2);
  } else {
    grn_expr_append_op(ctx, *expr, GRN_OP_GREATER, 2);
  }

  if (data->value->header.type == GRN_BULK) {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_PUSH, 1);
  } else {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_GET_VALUE, 1);
  }
  grn_expr_append_obj(ctx, *expr, data->max, GRN_OP_PUSH, 1);
  if (data->max_border_type == BETWEEN_BORDER_INCLUDE) {
    grn_expr_append_op(ctx, *expr, GRN_OP_LESS_EQUAL, 2);
  } else {
    grn_expr_append_op(ctx, *expr, GRN_OP_LESS, 2);
  }

  grn_expr_append_op(ctx, *expr, GRN_OP_AND, 2);

  return GRN_TRUE;
}

static grn_obj *
func_between(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *found;
  between_data data;
  grn_obj *condition = NULL;
  grn_obj *variable;
  grn_obj *table = NULL;
  grn_obj *between_expr;
  grn_obj *between_variable;
  grn_obj *result;

  found = GRN_PROC_ALLOC(GRN_DB_BOOL, 0);
  if (!found) {
    return NULL;
  }
  GRN_BOOL_SET(ctx, found, GRN_FALSE);

  grn_proc_get_info(ctx, user_data, NULL, NULL, &condition);
  if (!condition) {
    return found;
  }

  variable = grn_expr_get_var_by_offset(ctx, condition, 0);
  if (!variable) {
    return found;
  }

  between_data_init(ctx, &data);
  rc = between_parse_args(ctx, nargs, args, &data);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  table = grn_ctx_at(ctx, variable->header.domain);
  if (!table) {
    goto exit;
  }
  if (!between_create_expr(ctx, table, &data, &between_expr, &between_variable)) {
    goto exit;
  }

  GRN_RECORD_SET(ctx, between_variable, GRN_RECORD_VALUE(variable));
  result = grn_expr_exec(ctx, between_expr, 0);
  if (result) {
    grn_bool result_boolean;
    GRN_TRUEP(ctx, result, result_boolean);
    if (result_boolean) {
      GRN_BOOL_SET(ctx, found, GRN_TRUE);
    }
  }

  grn_obj_unlink(ctx, between_expr);
  grn_obj_unlink(ctx, table);

exit :
  between_data_fin(ctx, &data);
  if (table) {
    grn_obj_unlink(ctx, table);
  }

  return found;
}

static grn_bool
selector_between_sequential_search_should_use(grn_ctx *ctx,
                                              grn_obj *table,
                                              grn_obj *index,
                                              grn_obj *index_table,
                                              between_data *data,
                                              grn_obj *res,
                                              grn_operator op,
                                              double too_many_index_match_ratio)
{
  int n_index_keys;

  if (too_many_index_match_ratio < 0.0) {
    return GRN_FALSE;
  }

  if (op != GRN_OP_AND) {
    return GRN_FALSE;
  }

  if (index->header.flags & GRN_OBJ_WITH_WEIGHT) {
    return GRN_FALSE;
  }

  n_index_keys = grn_table_size(ctx, index_table);
  if (n_index_keys == 0) {
    return GRN_FALSE;
  }

  switch (index_table->header.domain) {
  /* TODO: */
  /* case GRN_DB_INT8 : */
  /* case GRN_DB_UINT8 : */
  /* case GRN_DB_INT16 : */
  /* case GRN_DB_UINT16 : */
  /* case GRN_DB_INT32 : */
  /* case GRN_DB_UINT32 : */
  /* case GRN_DB_INT64 : */
  /* case GRN_DB_UINT64 : */
  /* case GRN_DB_FLOAT : */
  case GRN_DB_TIME :
    break;
  default :
    return GRN_FALSE;
  }

  {
    grn_table_cursor *cursor;
    long long int all_min;
    long long int all_max;
    cursor = grn_table_cursor_open(ctx, index_table,
                                   NULL, -1,
                                   NULL, -1,
                                   0, 1,
                                   GRN_CURSOR_BY_KEY | GRN_CURSOR_ASCENDING);
    if (!cursor) {
      return GRN_FALSE;
    }
    if (grn_table_cursor_next(ctx, cursor) == GRN_ID_NIL) {
      grn_table_cursor_close(ctx, cursor);
      return GRN_FALSE;
    }
    {
      long long int *key;
      grn_table_cursor_get_key(ctx, cursor, (void **)&key);
      all_min = *key;
    }
    grn_table_cursor_close(ctx, cursor);

    cursor = grn_table_cursor_open(ctx, index_table,
                                   NULL, 0, NULL, 0,
                                   0, 1,
                                   GRN_CURSOR_BY_KEY | GRN_CURSOR_DESCENDING);
    if (!cursor) {
      return GRN_FALSE;
    }
    if (grn_table_cursor_next(ctx, cursor) == GRN_ID_NIL) {
      grn_table_cursor_close(ctx, cursor);
      return GRN_FALSE;
    }
    {
      long long int *key;
      grn_table_cursor_get_key(ctx, cursor, (void **)&key);
      all_max = *key;
    }
    grn_table_cursor_close(ctx, cursor);

    /*
     * We assume the following:
     *   * homogeneous index key distribution.
     *   * each index key matches only 1 record.
     * TODO: Improve me.
     */
    {
      int n_existing_records;
      int n_indexed_records;
      long long int all_difference;
      long long int argument_difference;

      n_existing_records = grn_table_size(ctx, res);

      all_difference = all_max - all_min;
      if (all_difference <= 0) {
        return GRN_FALSE;
      }
      argument_difference =
        GRN_TIME_VALUE(data->max) - GRN_TIME_VALUE(data->min);
      if (argument_difference <= 0) {
        return GRN_FALSE;
      }
      n_indexed_records =
        n_index_keys * ((double)argument_difference / (double)all_difference);

      /*
       * Same as:
       * ((n_existing_record / n_indexed_records) > too_many_index_match_ratio)
       */
      if (n_existing_records > (n_indexed_records * too_many_index_match_ratio)) {
        return GRN_FALSE;
      }
    }
  }

  return GRN_TRUE;
}

static grn_bool
selector_between_sequential_search(grn_ctx *ctx,
                                   grn_obj *table,
                                   grn_obj *index, grn_obj *index_table,
                                   between_data *data,
                                   grn_obj *res, grn_operator op)
{
  double too_many_index_match_ratio = 0.01;

  {
    const char *too_many_index_match_ratio_env =
      getenv("GRN_BETWEEN_TOO_MANY_INDEX_MATCH_RATIO");
    if (too_many_index_match_ratio_env) {
      too_many_index_match_ratio = atof(too_many_index_match_ratio_env);
    }
  }

  if (!selector_between_sequential_search_should_use(
        ctx, table, index, index_table, data, res, op,
        too_many_index_match_ratio)) {
    return GRN_FALSE;
  }

  {
    int offset = 0;
    int limit = -1;
    int flags = 0;
    grn_table_cursor *cursor;
    grn_obj *expr;
    grn_obj *variable;
    grn_id id;

    if (!between_create_expr(ctx, table, data, &expr, &variable)) {
      return GRN_FALSE;
    }

    cursor = grn_table_cursor_open(ctx, res,
                                   NULL, 0,
                                   NULL, 0,
                                   offset, limit, flags);
    if (!cursor) {
      grn_obj_unlink(ctx, expr);
      return GRN_FALSE;
    }

    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_id record_id;
      grn_obj *result;
      {
        grn_id *key;
        grn_table_cursor_get_key(ctx, cursor, (void **)&key);
        record_id = *key;
      }
      GRN_RECORD_SET(ctx, variable, record_id);
      result = grn_expr_exec(ctx, expr, 0);
      if (ctx->rc) {
        break;
      }
      if (result) {
        grn_bool result_boolean;
        GRN_TRUEP(ctx, result, result_boolean);
        if (result_boolean) {
          grn_ii_posting posting;
          posting.rid = record_id;
          posting.sid = 1;
          posting.pos = 0;
          posting.weight = 0;
          grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
        }
      }
    }
    grn_obj_unlink(ctx, expr);
    grn_table_cursor_close(ctx, cursor);

    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  }

  return GRN_TRUE;
}

static grn_rc
selector_between(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                 int nargs, grn_obj **args,
                 grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  int offset = 0;
  int limit = -1;
  int flags = GRN_CURSOR_ASCENDING | GRN_CURSOR_BY_KEY;
  between_data data;
  grn_obj *index_table = NULL;
  grn_table_cursor *cursor;
  grn_id id;

  if (!index) {
    return GRN_INVALID_ARGUMENT;
  }

  between_data_init(ctx, &data);
  rc = between_parse_args(ctx, nargs - 1, args + 1, &data);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  if (data.min_border_type == BETWEEN_BORDER_EXCLUDE) {
    flags |= GRN_CURSOR_GT;
  }
  if (data.max_border_type == BETWEEN_BORDER_EXCLUDE) {
    flags |= GRN_CURSOR_LT;
  }

  index_table = grn_ctx_at(ctx, index->header.domain);
  if (selector_between_sequential_search(ctx, table, index, index_table,
                                         &data, res, op)) {
    goto exit;
  }

  cursor = grn_table_cursor_open(ctx, index_table,
                                 GRN_BULK_HEAD(data.min),
                                 GRN_BULK_VSIZE(data.min),
                                 GRN_BULK_HEAD(data.max),
                                 GRN_BULK_VSIZE(data.max),
                                 offset, limit, flags);
  if (!cursor) {
    rc = ctx->rc;
    goto exit;
  }

  while ((id = grn_table_cursor_next(ctx, cursor))) {
    grn_ii_at(ctx, (grn_ii *)index, id, (grn_hash *)res, op);
  }
  grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  grn_table_cursor_close(ctx, cursor);

exit :
  between_data_fin(ctx, &data);
  if (index_table) {
    grn_obj_unlink(ctx, index_table);
  }

  return rc;
}

static void
grn_pat_tag_keys_put_original_text(grn_ctx *ctx, grn_obj *output,
                                   const char *text, unsigned int length,
                                   grn_bool use_html_escape)
{
  if (use_html_escape) {
    grn_text_escape_xml(ctx, output, text, length);
  } else {
    GRN_TEXT_PUT(ctx, output, text, length);
  }
}

static grn_rc
grn_pat_tag_keys(grn_ctx *ctx, grn_obj *keywords,
                 const char *string, unsigned int string_length,
                 const char **open_tags, unsigned int *open_tag_lengths,
                 const char **close_tags, unsigned int *close_tag_lengths,
                 unsigned int n_tags,
                 grn_obj *highlighted,
                 grn_bool use_html_escape)
{
  while (string_length > 0) {
#define MAX_N_HITS 1024
    grn_pat_scan_hit hits[MAX_N_HITS];
    const char *rest;
    unsigned int i, n_hits;
    unsigned int previous = 0;

    n_hits = grn_pat_scan(ctx, (grn_pat *)keywords,
                          string, string_length,
                          hits, MAX_N_HITS, &rest);
    for (i = 0; i < n_hits; i++) {
      unsigned int nth_tag;
      if (hits[i].offset - previous > 0) {
        grn_pat_tag_keys_put_original_text(ctx,
                                           highlighted,
                                           string + previous,
                                           hits[i].offset - previous,
                                           use_html_escape);
      }
      nth_tag = ((hits[i].id - 1) % n_tags);
      GRN_TEXT_PUT(ctx, highlighted,
                   open_tags[nth_tag], open_tag_lengths[nth_tag]);
      grn_pat_tag_keys_put_original_text(ctx,
                                         highlighted,
                                         string + hits[i].offset,
                                         hits[i].length,
                                         use_html_escape);
      GRN_TEXT_PUT(ctx, highlighted,
                   close_tags[nth_tag], close_tag_lengths[nth_tag]);
      previous = hits[i].offset + hits[i].length;
    }
    if (string_length - previous > 0) {
      grn_pat_tag_keys_put_original_text(ctx,
                                         highlighted,
                                         string + previous,
                                         string_length - previous,
                                         use_html_escape);
    }
    string_length -= rest - string;
    string = rest;
#undef MAX_N_HITS
  }

  return GRN_SUCCESS;
}

static grn_obj *
func_highlight_html(grn_ctx *ctx, int nargs, grn_obj **args,
                    grn_user_data *user_data)
{
  grn_obj *highlighted = NULL;

#define N_REQUIRED_ARGS 1
  if (nargs == N_REQUIRED_ARGS) {
    grn_obj *string = args[0];
    grn_obj *expression = NULL;
    grn_obj *condition_ptr = NULL;
    grn_obj *condition = NULL;
    grn_bool use_html_escape = GRN_TRUE;
    unsigned int n_keyword_sets = 1;
    const char *open_tags[1];
    unsigned int open_tag_lengths[1];
    const char *close_tags[1];
    unsigned int close_tag_lengths[1];
    grn_obj *keywords;

    open_tags[0] = "<span class=\"keyword\">";
    open_tag_lengths[0] = strlen("<span class=\"keyword\">");
    close_tags[0]  = "</span>";
    close_tag_lengths[0] = strlen("</span>");

    keywords = grn_table_create(ctx, NULL, 0, NULL,
                                GRN_OBJ_TABLE_PAT_KEY,
                                grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
                                NULL);
    {
      grn_obj *normalizer;
      normalizer = grn_ctx_get(ctx, "NormalizerAuto", -1);
      grn_obj_set_info(ctx, keywords, GRN_INFO_NORMALIZER, normalizer);
      grn_obj_unlink(ctx, normalizer);
    }

    grn_proc_get_info(ctx, user_data, NULL, NULL, &expression);
    condition_ptr = grn_expr_get_var(ctx, expression,
                                     GRN_SELECT_INTERNAL_VAR_CONDITION,
                                     strlen(GRN_SELECT_INTERNAL_VAR_CONDITION));
    if (condition_ptr) {
      condition = GRN_PTR_VALUE(condition_ptr);
    }

    if (condition) {
      grn_obj current_keywords;
      GRN_PTR_INIT(&current_keywords, GRN_OBJ_VECTOR, GRN_ID_NIL);
      grn_expr_get_keywords(ctx, condition, &current_keywords);

      for (;;) {
        grn_obj *keyword;
        GRN_PTR_POP(&current_keywords, keyword);
        if (!keyword) { break; }
        grn_table_add(ctx, keywords,
                      GRN_TEXT_VALUE(keyword),
                      GRN_TEXT_LEN(keyword),
                      NULL);
      }
      grn_obj_unlink(ctx, &current_keywords);
    }

    highlighted = GRN_PROC_ALLOC(GRN_DB_TEXT, 0);
    grn_pat_tag_keys(ctx, keywords,
                     GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                     open_tags,
                     open_tag_lengths,
                     close_tags,
                     close_tag_lengths,
                     n_keyword_sets,
                     highlighted,
                     use_html_escape);

    grn_obj_unlink(ctx, keywords);
  }
#undef N_REQUIRED_ARGS

  if (!highlighted) {
    highlighted = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  }

  return highlighted;
}

static grn_obj *
func_highlight_full(grn_ctx *ctx, int nargs, grn_obj **args,
                    grn_user_data *user_data)
{
  grn_obj *highlighted = NULL;

#define N_REQUIRED_ARGS 3
#define KEYWORD_SET_SIZE 3
  if (nargs >= (N_REQUIRED_ARGS + KEYWORD_SET_SIZE) &&
      (nargs - N_REQUIRED_ARGS) % KEYWORD_SET_SIZE == 0) {
    grn_obj *string = args[0];
    grn_obj *normalizer_name = args[1];
    grn_obj *use_html_escape = args[2];
    grn_obj **keyword_set_args = args + N_REQUIRED_ARGS;
    unsigned int n_keyword_sets = (nargs - N_REQUIRED_ARGS) / KEYWORD_SET_SIZE;
    unsigned int i;
    grn_obj open_tags;
    grn_obj open_tag_lengths;
    grn_obj close_tags;
    grn_obj close_tag_lengths;
    grn_obj *keywords;

    keywords = grn_table_create(ctx, NULL, 0, NULL,
                                GRN_OBJ_TABLE_PAT_KEY,
                                grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
                                NULL);

    if (GRN_TEXT_LEN(normalizer_name)) {
      grn_obj *normalizer;
      normalizer = grn_ctx_get(ctx,
                               GRN_TEXT_VALUE(normalizer_name),
                               GRN_TEXT_LEN(normalizer_name));
      if (!is_normalizer(ctx, normalizer)) {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, normalizer);
        ERR(GRN_INVALID_ARGUMENT,
            "[highlight_full] not normalizer: %.*s",
            (int)GRN_TEXT_LEN(&inspected),
            GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
        grn_obj_unlink(ctx, normalizer);
        grn_obj_unlink(ctx, keywords);
        return NULL;
      }
      grn_obj_set_info(ctx, keywords, GRN_INFO_NORMALIZER, normalizer);
      grn_obj_unlink(ctx, normalizer);
    }

    GRN_OBJ_INIT(&open_tags, GRN_BULK, 0, GRN_DB_VOID);
    GRN_OBJ_INIT(&open_tag_lengths, GRN_BULK, 0, GRN_DB_VOID);
    GRN_OBJ_INIT(&close_tags, GRN_BULK, 0, GRN_DB_VOID);
    GRN_OBJ_INIT(&close_tag_lengths, GRN_BULK, 0, GRN_DB_VOID);
    for (i = 0; i < n_keyword_sets; i++) {
      grn_obj *keyword   = keyword_set_args[i * KEYWORD_SET_SIZE + 0];
      grn_obj *open_tag  = keyword_set_args[i * KEYWORD_SET_SIZE + 1];
      grn_obj *close_tag = keyword_set_args[i * KEYWORD_SET_SIZE + 2];

      grn_table_add(ctx, keywords,
                    GRN_TEXT_VALUE(keyword),
                    GRN_TEXT_LEN(keyword),
                    NULL);

      {
        const char *open_tag_content = GRN_TEXT_VALUE(open_tag);
        grn_bulk_write(ctx, &open_tags,
                       (const char *)(&open_tag_content),
                       sizeof(char *));
      }
      {
        unsigned int open_tag_length = GRN_TEXT_LEN(open_tag);
        grn_bulk_write(ctx, &open_tag_lengths,
                       (const char *)(&open_tag_length),
                       sizeof(unsigned int));
      }
      {
        const char *close_tag_content = GRN_TEXT_VALUE(close_tag);
        grn_bulk_write(ctx, &close_tags,
                       (const char *)(&close_tag_content),
                       sizeof(char *));
      }
      {
        unsigned int close_tag_length = GRN_TEXT_LEN(close_tag);
        grn_bulk_write(ctx, &close_tag_lengths,
                       (const char *)(&close_tag_length),
                       sizeof(unsigned int));
      }
    }

    highlighted = GRN_PROC_ALLOC(GRN_DB_TEXT, 0);
    grn_pat_tag_keys(ctx, keywords,
                     GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                     (const char **)GRN_BULK_HEAD(&open_tags),
                     (unsigned int *)GRN_BULK_HEAD(&open_tag_lengths),
                     (const char **)GRN_BULK_HEAD(&close_tags),
                     (unsigned int *)GRN_BULK_HEAD(&close_tag_lengths),
                     n_keyword_sets,
                     highlighted,
                     GRN_BOOL_VALUE(use_html_escape));

    grn_obj_unlink(ctx, keywords);
    grn_obj_unlink(ctx, &open_tags);
    grn_obj_unlink(ctx, &open_tag_lengths);
    grn_obj_unlink(ctx, &close_tags);
    grn_obj_unlink(ctx, &close_tag_lengths);
  }
#undef N_REQUIRED_ARGS
#undef KEYWORD_SET_SIZE

  if (!highlighted) {
    highlighted = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  }

  return highlighted;
}

static grn_obj *
func_in_values(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *found;
  grn_obj *target_value;
  int i;

  found = GRN_PROC_ALLOC(GRN_DB_BOOL, 0);
  if (!found) {
    return NULL;
  }
  GRN_BOOL_SET(ctx, found, GRN_FALSE);

  if (nargs < 1) {
    ERR(GRN_INVALID_ARGUMENT,
        "in_values(): wrong number of arguments (%d for 1..)", nargs);
    return found;
  }

  target_value = args[0];
  for (i = 1; i < nargs; i++) {
    grn_obj *value = args[i];
    grn_bool result;

    result = grn_operator_exec_equal(ctx, target_value, value);
    if (ctx->rc) {
      break;
    }

    if (result) {
      GRN_BOOL_SET(ctx, found, GRN_TRUE);
      break;
    }
  }

  return found;
}

static grn_bool
is_reference_type_column(grn_ctx *ctx, grn_obj *column)
{
  grn_bool is_reference_type;
  grn_obj *range;

  range = grn_ctx_at(ctx, grn_obj_get_range(ctx, column));
  switch (range->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    is_reference_type = GRN_TRUE;
    break;
  default :
    is_reference_type = GRN_FALSE;
    break;
  }
  grn_obj_unlink(ctx, range);

  return is_reference_type;
}

static grn_obj *
selector_in_values_find_source(grn_ctx *ctx, grn_obj *index, grn_obj *res)
{
  grn_id source_id = GRN_ID_NIL;
  grn_obj source_ids;
  unsigned int n_source_ids;

  GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
  grn_obj_get_info(ctx, index, GRN_INFO_SOURCE, &source_ids);
  n_source_ids = GRN_BULK_VSIZE(&source_ids) / sizeof(grn_id);
  if (n_source_ids == 1) {
    source_id = GRN_UINT32_VALUE_AT(&source_ids, 0);
  }
  GRN_OBJ_FIN(ctx, &source_ids);

  if (source_id == GRN_ID_NIL) {
    return NULL;
  } else {
    return grn_ctx_at(ctx, source_id);
  }
}

static grn_bool
selector_in_values_sequential_search(grn_ctx *ctx,
                                     grn_obj *table,
                                     grn_obj *index,
                                     int n_values,
                                     grn_obj **values,
                                     grn_obj *res,
                                     grn_operator op)
{
  grn_obj *source;
  int n_existing_records;
  double too_many_index_match_ratio = 0.01;

  {
    const char *too_many_index_match_ratio_env =
      getenv("GRN_IN_VALUES_TOO_MANY_INDEX_MATCH_RATIO");
    if (too_many_index_match_ratio_env) {
      too_many_index_match_ratio = atof(too_many_index_match_ratio_env);
    }
  }

  if (too_many_index_match_ratio < 0.0) {
    return GRN_FALSE;
  }

  if (op != GRN_OP_AND) {
    return GRN_FALSE;
  }

  if (index->header.flags & GRN_OBJ_WITH_WEIGHT) {
    return GRN_FALSE;
  }

  n_existing_records = grn_table_size(ctx, res);
  if (n_existing_records == 0) {
    return GRN_TRUE;
  }

  source = selector_in_values_find_source(ctx, index, res);
  if (!source) {
    return GRN_FALSE;
  }

  if (!is_reference_type_column(ctx, source)) {
    grn_obj_unlink(ctx, source);
    return GRN_FALSE;
  }

  {
    grn_obj value_ids;
    int i, n_value_ids;
    int n_indexed_records = 0;

    {
      grn_id range_id;
      grn_obj *range;

      range_id = grn_obj_get_range(ctx, source);
      range = grn_ctx_at(ctx, range_id);
      if (!range) {
        grn_obj_unlink(ctx, source);
        return GRN_FALSE;
      }

      GRN_RECORD_INIT(&value_ids, GRN_OBJ_VECTOR, range_id);
      for (i = 0; i < n_values; i++) {
        grn_obj *value = values[i];
        grn_id value_id;

        value_id = grn_table_get(ctx, range,
                                 GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));
        if (value_id == GRN_ID_NIL) {
          continue;
        }
        GRN_RECORD_PUT(ctx, &value_ids, value_id);
      }
      grn_obj_unlink(ctx, range);
    }

    n_value_ids = GRN_BULK_VSIZE(&value_ids) / sizeof(grn_id);
    for (i = 0; i < n_value_ids; i++) {
      grn_id value_id = GRN_RECORD_VALUE_AT(&value_ids, i);
      n_indexed_records += grn_ii_estimate_size(ctx, (grn_ii *)index, value_id);
    }

    /*
     * Same as:
     * ((n_existing_record / n_indexed_records) > too_many_index_match_ratio)
    */
    if (n_existing_records > (n_indexed_records * too_many_index_match_ratio)) {
      grn_obj_unlink(ctx, &value_ids);
      grn_obj_unlink(ctx, source);
      return GRN_FALSE;
    }

    {
      grn_obj *accessor;
      char local_source_name[GRN_TABLE_MAX_KEY_SIZE];
      int local_source_name_length;

      local_source_name_length = grn_column_name(ctx, source,
                                                 local_source_name,
                                                 GRN_TABLE_MAX_KEY_SIZE);
      grn_obj_unlink(ctx, source);
      accessor = grn_obj_column(ctx, res,
                                local_source_name,
                                local_source_name_length);
      {
        grn_table_cursor *cursor;
        grn_id record_id;
        grn_obj record_value;
        GRN_RECORD_INIT(&record_value, 0, grn_obj_id(ctx, res));
        cursor = grn_table_cursor_open(ctx, res,
                                       NULL, 0, NULL, 0,
                                       0, -1, GRN_CURSOR_ASCENDING);
        while ((record_id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
          GRN_BULK_REWIND(&record_value);
          grn_obj_get_value(ctx, accessor, record_id, &record_value);
          for (i = 0; i < n_value_ids; i++) {
            grn_id value_id = GRN_RECORD_VALUE_AT(&value_ids, i);
            if (value_id == GRN_RECORD_VALUE(&record_value)) {
              grn_ii_posting posting;
              posting.rid = record_id;
              posting.sid = 1;
              posting.pos = 0;
              posting.weight = 0;
              grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
            }
          }
        }
        grn_table_cursor_close(ctx, cursor);
        grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
        GRN_OBJ_FIN(ctx, &record_value);
      }
      grn_obj_unlink(ctx, accessor);
    }
    grn_obj_unlink(ctx, &value_ids);
  }
  grn_obj_unlink(ctx, source);

  return GRN_TRUE;
}

static grn_rc
selector_in_values(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                   int nargs, grn_obj **args,
                   grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  int i, n_values;
  grn_obj **values;

  if (!index) {
    return GRN_INVALID_ARGUMENT;
  }

  if (nargs < 2) {
    ERR(GRN_INVALID_ARGUMENT,
        "in_values(): wrong number of arguments (%d for 1..)", nargs);
    return ctx->rc;
  }

  n_values = nargs - 2;
  values = args + 2;

  if (n_values == 0) {
    return rc;
  }

  if (selector_in_values_sequential_search(ctx, table, index,
                                           n_values, values,
                                           res, op)) {
    return ctx->rc;
  }

  ctx->flags |= GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
  for (i = 0; i < n_values; i++) {
    grn_obj *value = values[i];
    grn_search_optarg search_options;
    memset(&search_options, 0, sizeof(grn_search_optarg));
    search_options.mode = GRN_OP_EXACT;
    search_options.similarity_threshold = 0;
    search_options.max_interval = 0;
    search_options.weight_vector = NULL;
    search_options.vector_size = 0;
    search_options.proc = NULL;
    search_options.max_size = 0;
    search_options.scorer = NULL;
    if (i == n_values - 1) {
      ctx->flags &= ~GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
    }
    rc = grn_obj_search(ctx, index, value, res, op, &search_options);
    if (rc != GRN_SUCCESS) {
      break;
    }
  }

  return rc;
}

static grn_obj *
proc_range_filter(grn_ctx *ctx, int nargs, grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *table_name = VAR(0);
  grn_obj *column_name = VAR(1);
  grn_obj *min = VAR(2);
  grn_obj *min_border = VAR(3);
  grn_obj *max = VAR(4);
  grn_obj *max_border = VAR(5);
  grn_obj *offset = VAR(6);
  grn_obj *limit = VAR(7);
  grn_obj *filter = VAR(8);
  grn_obj *output_columns = VAR(9);
  grn_obj *table;
  grn_obj *res = NULL;
  grn_obj *filter_expr = NULL;
  grn_obj *filter_variable = NULL;
  int real_offset;
  int real_limit;

  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(table_name), GRN_TEXT_LEN(table_name));
  if (!table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[range_filter] nonexistent table <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name));
    return NULL;
  }

  if (GRN_TEXT_LEN(filter) > 0) {
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, filter_expr, filter_variable);
    if (!filter_expr) {
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] failed to create expression");
      goto exit;
    }

    grn_expr_parse(ctx, filter_expr,
                   GRN_TEXT_VALUE(filter), GRN_TEXT_LEN(filter),
                   NULL, GRN_OP_MATCH, GRN_OP_AND, GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      goto exit;
    }
  }

  res = grn_table_create(ctx, NULL, 0, NULL,
                         GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                         table, NULL);
  if (!res) {
    ERR(GRN_INVALID_ARGUMENT,
        "[range_filter] failed to result table");
    goto exit;
  }

  {
    grn_obj int32_value;

    GRN_INT32_INIT(&int32_value, 0);

    if (GRN_TEXT_LEN(offset) > 0) {
      if (grn_obj_cast(ctx, offset, &int32_value, GRN_FALSE) != GRN_SUCCESS) {
        ERR(GRN_INVALID_ARGUMENT,
            "[range_filter] invalid offset format: <%.*s>",
            (int)GRN_TEXT_LEN(offset), GRN_TEXT_VALUE(offset));
        GRN_OBJ_FIN(ctx, &int32_value);
        goto exit;
      }
      real_offset = GRN_INT32_VALUE(&int32_value);
    } else {
      real_offset = 0;
    }

    GRN_BULK_REWIND(&int32_value);

    if (GRN_TEXT_LEN(limit) > 0) {
      if (grn_obj_cast(ctx, limit, &int32_value, GRN_FALSE) != GRN_SUCCESS) {
        ERR(GRN_INVALID_ARGUMENT,
            "[range_filter] invalid limit format: <%.*s>",
            (int)GRN_TEXT_LEN(limit), GRN_TEXT_VALUE(limit));
        GRN_OBJ_FIN(ctx, &int32_value);
        goto exit;
      }
      real_limit = GRN_INT32_VALUE(&int32_value);
    } else {
      real_limit = DEFAULT_LIMIT;
    }

    GRN_OBJ_FIN(ctx, &int32_value);
  }
  {
    grn_rc rc;
    int original_offset = real_offset;
    int original_limit = real_limit;
    rc = grn_normalize_offset_and_limit(ctx, grn_table_size(ctx, table),
                                        &real_offset, &real_limit);
    switch (rc) {
    case GRN_TOO_SMALL_OFFSET :
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] too small offset: <%d>", original_offset);
      goto exit;
    case GRN_TOO_LARGE_OFFSET :
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] too large offset: <%d>", original_offset);
      goto exit;
    case GRN_TOO_SMALL_LIMIT :
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] too small limit: <%d>", original_limit);
      goto exit;
    default :
      break;
    }
  }

  if (real_limit != 0) {
    grn_table_sort_key *sort_keys;
    unsigned int n_sort_keys;
    sort_keys = grn_table_sort_key_from_str(ctx,
                                            GRN_TEXT_VALUE(column_name),
                                            GRN_TEXT_LEN(column_name),
                                            table,
                                            &n_sort_keys);
    if (n_sort_keys == 1) {
      grn_table_sort_key *sort_key;
      grn_obj *index;
      int n_indexes;
      grn_operator op = GRN_OP_OR;

      sort_key = &(sort_keys[0]);
      n_indexes = grn_column_index(ctx, sort_key->key, GRN_OP_LESS,
                                   &index, 1, NULL);
      if (n_indexes > 0) {
        grn_obj *lexicon;
        grn_table_cursor *table_cursor;
        int table_cursor_flags = 0;
        between_border_type min_border_type;
        between_border_type max_border_type;
        grn_obj real_min;
        grn_obj real_max;
        int n_records = 0;
        grn_obj *index_cursor;
        int index_cursor_flags = 0;
        grn_posting *posting;

        lexicon = grn_ctx_at(ctx, index->header.domain);
        if (sort_key->flags & GRN_TABLE_SORT_DESC) {
          table_cursor_flags |= GRN_CURSOR_DESCENDING;
        } else {
          table_cursor_flags |= GRN_CURSOR_ASCENDING;
        }
        if (GRN_TEXT_LEN(min_border) > 0) {
          min_border_type = between_parse_border(ctx, min_border, "min_border");
        } else {
          min_border_type = BETWEEN_BORDER_INCLUDE;
        }
        if (GRN_TEXT_LEN(max_border) > 0) {
          max_border_type = between_parse_border(ctx, max_border, "max_border");
        } else {
          max_border_type = BETWEEN_BORDER_INCLUDE;
        }
        if (min_border_type == BETWEEN_BORDER_EXCLUDE) {
          table_cursor_flags |= GRN_CURSOR_GT;
        }
        if (max_border_type == BETWEEN_BORDER_EXCLUDE) {
          table_cursor_flags |= GRN_CURSOR_LT;
        }
        GRN_OBJ_INIT(&real_min, GRN_BULK, 0, lexicon->header.domain);
        GRN_OBJ_INIT(&real_max, GRN_BULK, 0, lexicon->header.domain);
        if (GRN_TEXT_LEN(min) > 0) {
          grn_obj_cast(ctx, min, &real_min, GRN_FALSE);
        }
        if (GRN_TEXT_LEN(max) > 0) {
          grn_obj_cast(ctx, max, &real_max, GRN_FALSE);
        }
        table_cursor = grn_table_cursor_open(ctx, lexicon,
                                             GRN_BULK_HEAD(&real_min),
                                             GRN_BULK_VSIZE(&real_min),
                                             GRN_BULK_HEAD(&real_max),
                                             GRN_BULK_VSIZE(&real_max),
                                             0, -1, table_cursor_flags);
        index_cursor = grn_index_cursor_open(ctx, table_cursor,
                                             index, GRN_ID_NIL, GRN_ID_NIL,
                                             index_cursor_flags);
        while ((posting = grn_index_cursor_next(ctx, index_cursor, NULL))) {
          grn_bool result_boolean = GRN_FALSE;

          if (filter_expr) {
            grn_obj *result;
            GRN_RECORD_SET(ctx, filter_variable, posting->rid);
            result = grn_expr_exec(ctx, filter_expr, 0);
            if (ctx->rc) {
              break;
            }
            if (result) {
              GRN_TRUEP(ctx, result, result_boolean);
            }
          } else {
            result_boolean = GRN_TRUE;
          }

          if (result_boolean) {
            if (n_records >= real_offset) {
              grn_ii_posting ii_posting;
              ii_posting.rid = posting->rid;
              ii_posting.sid = posting->sid;
              ii_posting.pos = posting->pos;
              ii_posting.weight = posting->weight;
              grn_ii_posting_add(ctx, &ii_posting, (grn_hash *)res, op);
            }
            n_records++;
            if (n_records == real_limit) {
              break;
            }
          }
        }
        grn_obj_unlink(ctx, index_cursor);
        grn_table_cursor_close(ctx, table_cursor);

        GRN_OBJ_FIN(ctx, &real_min);
        GRN_OBJ_FIN(ctx, &real_max);
      }
      grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
    }
    grn_table_sort_key_close(ctx, sort_keys, n_sort_keys);
  }

  if (ctx->rc == GRN_SUCCESS) {
    const char *raw_output_columns;
    int raw_output_columns_len;

    raw_output_columns = GRN_TEXT_VALUE(output_columns);
    raw_output_columns_len = GRN_TEXT_LEN(output_columns);
    if (raw_output_columns_len == 0) {
      raw_output_columns = DEFAULT_OUTPUT_COLUMNS;
      raw_output_columns_len = strlen(raw_output_columns);
    }
    grn_select_output_columns(ctx, res, -1, real_offset, real_limit,
                              raw_output_columns,
                              raw_output_columns_len,
                              filter_expr);
  }

exit :
  if (filter_expr) {
    grn_obj_unlink(ctx, filter_expr);
  }
  if (res) {
    grn_obj_unlink(ctx, res);
  }

  return NULL;
}

static grn_obj *
proc_request_cancel(grn_ctx *ctx, int nargs, grn_obj **args,
                    grn_user_data *user_data)
{
  grn_obj *id = VAR(0);
  grn_bool canceled;

  if (GRN_TEXT_LEN(id) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[request_cancel] ID is missing");
    return NULL;
  }

  canceled = grn_request_canceler_cancel(GRN_TEXT_VALUE(id), GRN_TEXT_LEN(id));

  GRN_OUTPUT_MAP_OPEN("result", 2);
  GRN_OUTPUT_CSTR("id");
  GRN_OUTPUT_STR(GRN_TEXT_VALUE(id), GRN_TEXT_LEN(id));
  GRN_OUTPUT_CSTR("canceled");
  GRN_OUTPUT_BOOL(canceled);
  GRN_OUTPUT_MAP_CLOSE();

  return NULL;
}

#define DEF_VAR(v,name_str) do {\
  (v).name = (name_str);\
  (v).name_size = GRN_STRLEN(name_str);\
  GRN_TEXT_INIT(&(v).value, 0);\
} while (0)

#define DEF_COMMAND(name, func, nvars, vars)\
  (grn_proc_create(ctx, (name), (sizeof(name) - 1),\
                   GRN_PROC_COMMAND, (func), NULL, NULL, (nvars), (vars)))

void
grn_db_init_builtin_query(grn_ctx *ctx)
{
  grn_expr_var vars[23];

  DEF_VAR(vars[0], "name");
  DEF_VAR(vars[1], "table");
  DEF_VAR(vars[2], "match_columns");
  DEF_VAR(vars[3], "query");
  DEF_VAR(vars[4], "filter");
  DEF_VAR(vars[5], "scorer");
  DEF_VAR(vars[6], "sortby");
  DEF_VAR(vars[7], "output_columns");
  DEF_VAR(vars[8], "offset");
  DEF_VAR(vars[9], "limit");
  DEF_VAR(vars[10], "drilldown");
  DEF_VAR(vars[11], "drilldown_sortby");
  DEF_VAR(vars[12], "drilldown_output_columns");
  DEF_VAR(vars[13], "drilldown_offset");
  DEF_VAR(vars[14], "drilldown_limit");
  DEF_VAR(vars[15], "cache");
  DEF_VAR(vars[16], "match_escalation_threshold");
  /* Deprecated. Use query_expander instead. */
  DEF_VAR(vars[17], "query_expansion");
  DEF_VAR(vars[18], "query_flags");
  DEF_VAR(vars[19], "query_expander");
  DEF_VAR(vars[20], "adjuster");
  DEF_VAR(vars[21], "drilldown_calc_types");
  DEF_VAR(vars[22], "drilldown_calc_target");
  DEF_COMMAND("define_selector", proc_define_selector, 23, vars);
  DEF_COMMAND("select", proc_select, 22, vars + 1);

  DEF_VAR(vars[0], "values");
  DEF_VAR(vars[1], "table");
  DEF_VAR(vars[2], "columns");
  DEF_VAR(vars[3], "ifexists");
  DEF_VAR(vars[4], "input_type");
  DEF_VAR(vars[5], "each");
  DEF_COMMAND("load", proc_load, 6, vars);

  DEF_COMMAND("status", proc_status, 0, vars);

  DEF_COMMAND("table_list", proc_table_list, 0, vars);

  DEF_VAR(vars[0], "table");
  DEF_COMMAND("column_list", proc_column_list, 1, vars);

  DEF_VAR(vars[0], "name");
  DEF_VAR(vars[1], "flags");
  DEF_VAR(vars[2], "key_type");
  DEF_VAR(vars[3], "value_type");
  DEF_VAR(vars[4], "default_tokenizer");
  DEF_VAR(vars[5], "normalizer");
  DEF_VAR(vars[6], "token_filters");
  DEF_COMMAND("table_create", proc_table_create, 7, vars);

  DEF_VAR(vars[0], "name");
  DEF_COMMAND("table_remove", proc_table_remove, 1, vars);

  DEF_VAR(vars[0], "name");
  DEF_VAR(vars[1], "new_name");
  DEF_COMMAND("table_rename", proc_table_rename, 2, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "name");
  DEF_VAR(vars[2], "flags");
  DEF_VAR(vars[3], "type");
  DEF_VAR(vars[4], "source");
  DEF_COMMAND("column_create", proc_column_create, 5, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "name");
  DEF_COMMAND("column_remove", proc_column_remove, 2, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "name");
  DEF_VAR(vars[2], "new_name");
  DEF_COMMAND("column_rename", proc_column_rename, 3, vars);

  DEF_VAR(vars[0], "path");
  DEF_COMMAND(GRN_EXPR_MISSING_NAME, proc_missing, 1, vars);

  DEF_COMMAND("quit", proc_quit, 0, vars);

  DEF_COMMAND("shutdown", proc_shutdown, 0, vars);

  /* Deprecated. Use "lock_clear" instead. */
  DEF_VAR(vars[0], "target_name");
  DEF_COMMAND("clearlock", proc_lock_clear, 1, vars);

  DEF_VAR(vars[0], "target_name");
  DEF_COMMAND("lock_clear", proc_lock_clear, 1, vars);

  DEF_VAR(vars[0], "target_name");
  DEF_VAR(vars[1], "threshold");
  DEF_COMMAND("defrag", proc_defrag, 2, vars);

  DEF_VAR(vars[0], "level");
  DEF_COMMAND("log_level", proc_log_level, 1, vars);

  DEF_VAR(vars[0], "level");
  DEF_VAR(vars[1], "message");
  DEF_COMMAND("log_put", proc_log_put, 2, vars);

  DEF_COMMAND("log_reopen", proc_log_reopen, 0, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "key");
  DEF_VAR(vars[2], "id");
  DEF_VAR(vars[3], "filter");
  DEF_COMMAND("delete", proc_delete, 4, vars);

  DEF_VAR(vars[0], "max");
  DEF_COMMAND("cache_limit", proc_cache_limit, 1, vars);

  DEF_VAR(vars[0], "tables");
  DEF_COMMAND("dump", proc_dump, 1, vars);

  DEF_VAR(vars[0], "path");
  DEF_COMMAND("register", proc_register, 1, vars);

  DEF_VAR(vars[0], "obj");
  DEF_COMMAND("check", proc_check, 1, vars);

  DEF_VAR(vars[0], "target_name");
  DEF_VAR(vars[1], "table");
  DEF_COMMAND("truncate", proc_truncate, 2, vars);

  DEF_VAR(vars[0], "normalizer");
  DEF_VAR(vars[1], "string");
  DEF_VAR(vars[2], "flags");
  DEF_COMMAND("normalize", proc_normalize, 3, vars);

  DEF_VAR(vars[0], "tokenizer");
  DEF_VAR(vars[1], "string");
  DEF_VAR(vars[2], "normalizer");
  DEF_VAR(vars[3], "flags");
  DEF_VAR(vars[4], "mode");
  DEF_VAR(vars[5], "token_filters");
  DEF_COMMAND("tokenize", proc_tokenize, 6, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "string");
  DEF_VAR(vars[2], "flags");
  DEF_VAR(vars[3], "mode");
  DEF_COMMAND("table_tokenize", proc_table_tokenize, 4, vars);

  DEF_COMMAND("tokenizer_list", proc_tokenizer_list, 0, vars);

  DEF_COMMAND("normalizer_list", proc_normalizer_list, 0, vars);

  DEF_VAR(vars[0], "seed");
  grn_proc_create(ctx, "rand", -1, GRN_PROC_FUNCTION, func_rand,
                  NULL, NULL, 0, vars);

  grn_proc_create(ctx, "now", -1, GRN_PROC_FUNCTION, func_now,
                  NULL, NULL, 0, vars);

  grn_proc_create(ctx, "max", -1, GRN_PROC_FUNCTION, func_max,
                  NULL, NULL, 0, vars);
  grn_proc_create(ctx, "min", -1, GRN_PROC_FUNCTION, func_min,
                  NULL, NULL, 0, vars);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "geo_in_circle", -1, GRN_PROC_FUNCTION,
                                    func_geo_in_circle, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, grn_selector_geo_in_circle);

    selector_proc = grn_proc_create(ctx, "geo_in_rectangle", -1,
                                    GRN_PROC_FUNCTION,
                                    func_geo_in_rectangle, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, grn_selector_geo_in_rectangle);
  }

  grn_proc_create(ctx, "geo_distance", -1, GRN_PROC_FUNCTION,
                  func_geo_distance, NULL, NULL, 0, NULL);

  /* deprecated. */
  grn_proc_create(ctx, "geo_distance2", -1, GRN_PROC_FUNCTION,
                  func_geo_distance2, NULL, NULL, 0, NULL);

  /* deprecated. */
  grn_proc_create(ctx, "geo_distance3", -1, GRN_PROC_FUNCTION,
                  func_geo_distance3, NULL, NULL, 0, NULL);

  grn_proc_create(ctx, "edit_distance", -1, GRN_PROC_FUNCTION,
                  func_edit_distance, NULL, NULL, 0, NULL);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "all_records", -1, GRN_PROC_FUNCTION,
                                    func_all_records, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_all_records);
  }

  /* experimental */
  grn_proc_create(ctx, "snippet_html", -1, GRN_PROC_FUNCTION,
                  func_snippet_html, NULL, NULL, 0, NULL);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "query", -1, GRN_PROC_FUNCTION,
                                    func_query, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_query);
  }

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "sub_filter", -1, GRN_PROC_FUNCTION,
                                    func_sub_filter, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_sub_filter);
  }

  grn_proc_create(ctx, "html_untag", -1, GRN_PROC_FUNCTION,
                  func_html_untag, NULL, NULL, 0, NULL);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "between", -1, GRN_PROC_FUNCTION,
                                    func_between, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_between);
  }

  grn_proc_create(ctx, "highlight_html", -1, GRN_PROC_FUNCTION,
                  func_highlight_html, NULL, NULL, 0, NULL);

  grn_proc_create(ctx, "highlight_full", -1, GRN_PROC_FUNCTION,
                  func_highlight_full, NULL, NULL, 0, NULL);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "in_values", -1, GRN_PROC_FUNCTION,
                                    func_in_values, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_in_values);
  }

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "column");
  DEF_VAR(vars[2], "min");
  DEF_VAR(vars[3], "min_border");
  DEF_VAR(vars[4], "max");
  DEF_VAR(vars[5], "max_border");
  DEF_VAR(vars[6], "offset");
  DEF_VAR(vars[7], "limit");
  DEF_VAR(vars[8], "filter");
  DEF_VAR(vars[9], "output_columns");
  DEF_COMMAND("range_filter", proc_range_filter, 10, vars);

  DEF_VAR(vars[0], "id");
  DEF_COMMAND("request_cancel", proc_request_cancel, 1, vars);
}
