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

#include "grn_load.h"
#include "grn_ctx_impl.h"
#include "grn_db.h"
#include "grn_util.h"

static void
grn_loader_save_error(grn_ctx *ctx, grn_loader *loader)
{
  loader->rc = ctx->rc;
  grn_strcpy(loader->errbuf, GRN_CTX_MSGSIZE, ctx->errbuf);
}

static grn_obj *
values_add(grn_ctx *ctx, grn_loader *loader)
{
  grn_obj *res;
  uint32_t curr_size = loader->values_size * sizeof(grn_obj);
  if (curr_size < GRN_TEXT_LEN(&loader->values)) {
    res = (grn_obj *)(GRN_TEXT_VALUE(&loader->values) + curr_size);
    res->header.domain = GRN_DB_TEXT;
    GRN_BULK_REWIND(res);
  } else {
    if (grn_bulk_space(ctx, &loader->values, sizeof(grn_obj))) { return NULL; }
    res = (grn_obj *)(GRN_TEXT_VALUE(&loader->values) + curr_size);
    GRN_TEXT_INIT(res, 0);
  }
  loader->values_size++;
  loader->last = res;
  return res;
}

static grn_obj *
values_next(grn_ctx *ctx, grn_obj *value)
{
  if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET ||
      value->header.domain == GRN_JSON_LOAD_OPEN_BRACE) {
    value += GRN_UINT32_VALUE(value);
  }
  return value + 1;
}

static int
values_len(grn_ctx *ctx, grn_obj *head, grn_obj *tail)
{
  int len;
  for (len = 0; head < tail; head = values_next(ctx, head), len++) ;
  return len;
}

static grn_id
loader_add(grn_ctx *ctx, grn_obj *key)
{
  int added = 0;
  grn_loader *loader = &ctx->impl->loader;
  grn_id id = grn_table_add_by_key(ctx, loader->table, key, &added);
  if (id == GRN_ID_NIL) {
    grn_loader_save_error(ctx, loader);
    return id;
  }
  if (!added && loader->ifexists) {
    grn_obj *v = grn_expr_get_var_by_offset(ctx, loader->ifexists, 0);
    grn_obj *result;
    GRN_RECORD_SET(ctx, v, id);
    result = grn_expr_exec(ctx, loader->ifexists, 0);
    if (!grn_obj_is_true(ctx, result)) {
      id = 0;
    }
  }
  return id;
}

static void
add_weight_vector(grn_ctx *ctx,
                  grn_obj *column,
                  grn_obj *value,
                  grn_obj *vector)
{
  unsigned int i, n;
  grn_obj weight_buffer;

  n = GRN_UINT32_VALUE(value);
  GRN_UINT32_INIT(&weight_buffer, 0);
  for (i = 0; i < n; i += 2) {
    grn_rc rc;
    grn_obj *key, *weight;

    key = value + 1 + i;
    weight = key + 1;

    GRN_BULK_REWIND(&weight_buffer);
    rc = grn_obj_cast(ctx, weight, &weight_buffer, GRN_TRUE);
    if (rc != GRN_SUCCESS) {
      grn_obj *range;
      range = grn_ctx_at(ctx, weight_buffer.header.domain);
      ERR_CAST(column, range, weight);
      grn_obj_unlink(ctx, range);
      break;
    }
    grn_vector_add_element(ctx,
                           vector,
                           GRN_BULK_HEAD(key),
                           GRN_BULK_VSIZE(key),
                           GRN_UINT32_VALUE(&weight_buffer),
                           key->header.domain);
  }
  GRN_OBJ_FIN(ctx, &weight_buffer);
}

static void
set_vector(grn_ctx *ctx, grn_obj *column, grn_id id, grn_obj *vector)
{
  int n = GRN_UINT32_VALUE(vector);
  grn_obj buf, *v = vector + 1;
  grn_id range_id;
  grn_obj *range;

  range_id = DB_OBJ(column)->range;
  range = grn_ctx_at(ctx, range_id);
  if (grn_obj_is_table(ctx, range)) {
    GRN_RECORD_INIT(&buf, GRN_OBJ_VECTOR, range_id);
    while (n--) {
      grn_bool cast_failed = GRN_FALSE;
      grn_obj record, *element = v;
      if (range_id != element->header.domain) {
        GRN_RECORD_INIT(&record, 0, range_id);
        if (grn_obj_cast(ctx, element, &record, GRN_TRUE)) {
          cast_failed = GRN_TRUE;
          ERR_CAST(column, range, element);
        }
        element = &record;
      }
      if (!cast_failed) {
        GRN_UINT32_PUT(ctx, &buf, GRN_RECORD_VALUE(element));
      }
      if (element == &record) { GRN_OBJ_FIN(ctx, element); }
      v = values_next(ctx, v);
    }
  } else {
    if (((struct _grn_type *)range)->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
      GRN_TEXT_INIT(&buf, GRN_OBJ_VECTOR);
      while (n--) {
        switch (v->header.domain) {
        case GRN_DB_TEXT :
        {
          grn_bool cast_failed = GRN_FALSE;
          grn_obj casted_element, *element = v;
          if (range_id != element->header.domain) {
            GRN_OBJ_INIT(&casted_element, GRN_BULK, 0, range_id);
            if (grn_obj_cast(ctx, element, &casted_element, GRN_TRUE)) {
              cast_failed = GRN_TRUE;
              ERR_CAST(column, range, element);
            }
            element = &casted_element;
          }
          if (!cast_failed) {
            grn_vector_add_element(ctx, &buf,
                                   GRN_TEXT_VALUE(element),
                                   GRN_TEXT_LEN(element),
                                   0,
                                   element->header.domain);
          }
          if (element == &casted_element) { GRN_OBJ_FIN(ctx, element); }
          break;
        }
        case GRN_JSON_LOAD_OPEN_BRACE :
          add_weight_vector(ctx, column, v, &buf);
          n -= GRN_UINT32_VALUE(v);
          break;
        default :
          ERR(GRN_INVALID_ARGUMENT, "array must contain string or object");
          break;
        }
        v = values_next(ctx, v);
      }
    } else {
      grn_id value_size = ((grn_db_obj *)range)->range;
      GRN_VALUE_FIX_SIZE_INIT(&buf, GRN_OBJ_VECTOR, range_id);
      while (n--) {
        grn_bool cast_failed = GRN_FALSE;
        grn_obj casted_element, *element = v;
        if (range_id != element->header.domain) {
          GRN_OBJ_INIT(&casted_element, GRN_BULK, 0, range_id);
          if (grn_obj_cast(ctx, element, &casted_element, GRN_TRUE)) {
            cast_failed = GRN_TRUE;
            ERR_CAST(column, range, element);
          }
          element = &casted_element;
        }
        if (!cast_failed) {
          grn_bulk_write(ctx, &buf, GRN_TEXT_VALUE(element), value_size);
        }
        if (element == &casted_element) { GRN_OBJ_FIN(ctx, element); }
        v = values_next(ctx, v);
      }
    }
  }
  grn_obj_set_value(ctx, column, id, &buf, GRN_OBJ_SET);
  GRN_OBJ_FIN(ctx, &buf);
}

static void
set_weight_vector(grn_ctx *ctx, grn_obj *column, grn_id id, grn_obj *value)
{
  if (!grn_obj_is_weight_vector_column(ctx, column)) {
    char column_name[GRN_TABLE_MAX_KEY_SIZE];
    int column_name_size;
    column_name_size = grn_obj_name(ctx, column, column_name,
                                    GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_INVALID_ARGUMENT,
        "<%.*s>: columns except weight vector column don't support object value",
        column_name_size, column_name);
    return;
  }

  {
    grn_obj vector;

    GRN_TEXT_INIT(&vector, GRN_OBJ_VECTOR);
    add_weight_vector(ctx, column, value, &vector);
    grn_obj_set_value(ctx, column, id, &vector, GRN_OBJ_SET);
    GRN_OBJ_FIN(ctx, &vector);
  }
}

static inline int
name_equal(const char *p, unsigned int size, const char *name)
{
  if (strlen(name) != size) { return 0; }
  if (*p != GRN_DB_PSEUDO_COLUMN_PREFIX) { return 0; }
  return !memcmp(p + 1, name + 1, size - 1);
}

static void
report_set_column_value_failure(grn_ctx *ctx,
                                grn_obj *key,
                                const char *column_name,
                                unsigned int column_name_size,
                                grn_obj *column_value)
{
  grn_obj key_inspected, column_value_inspected;

  GRN_TEXT_INIT(&key_inspected, 0);
  GRN_TEXT_INIT(&column_value_inspected, 0);
  grn_inspect_limited(ctx, &key_inspected, key);
  grn_inspect_limited(ctx, &column_value_inspected, column_value);
  GRN_LOG(ctx, GRN_LOG_ERROR,
          "[table][load] failed to set column value: %s: "
          "key: <%.*s>, column: <%.*s>, value: <%.*s>",
          ctx->errbuf,
          (int)GRN_TEXT_LEN(&key_inspected),
          GRN_TEXT_VALUE(&key_inspected),
          column_name_size,
          column_name,
          (int)GRN_TEXT_LEN(&column_value_inspected),
          GRN_TEXT_VALUE(&column_value_inspected));
  GRN_OBJ_FIN(ctx, &key_inspected);
  GRN_OBJ_FIN(ctx, &column_value_inspected);
}

static grn_id
parse_id_value(grn_ctx *ctx, grn_obj *value)
{
  switch (value->header.type) {
  case GRN_DB_UINT32 :
    return GRN_UINT32_VALUE(value);
  case GRN_DB_INT32 :
    return GRN_INT32_VALUE(value);
  default :
    {
      grn_id id = GRN_ID_NIL;
      grn_obj casted_value;
      GRN_UINT32_INIT(&casted_value, 0);
      if (grn_obj_cast(ctx, value, &casted_value, GRN_FALSE) != GRN_SUCCESS) {
        grn_obj inspected;
        GRN_TEXT_INIT(&inspected, 0);
        grn_inspect(ctx, &inspected, value);
        ERR(GRN_INVALID_ARGUMENT,
            "<%s>: failed to cast to <UInt32>: <%.*s>",
            GRN_COLUMN_NAME_ID,
            (int)GRN_TEXT_LEN(&inspected),
            GRN_TEXT_VALUE(&inspected));
        GRN_OBJ_FIN(ctx, &inspected);
      } else {
        id = GRN_UINT32_VALUE(&casted_value);
      }
      GRN_OBJ_FIN(ctx, &casted_value);
      return id;
    }
  }
}

static void
bracket_close(grn_ctx *ctx, grn_loader *loader)
{
  grn_id id = GRN_ID_NIL;
  grn_obj *value, *value_end, *id_value = NULL, *key_value = NULL;
  grn_obj *col, **cols; /* Columns except _id and _key. */
  uint32_t i, begin;
  uint32_t ncols;   /* Number of columns except _id and _key. */
  uint32_t nvalues; /* Number of values in brackets. */
  uint32_t depth;
  grn_bool is_record_load = GRN_FALSE;

  cols = (grn_obj **)GRN_BULK_HEAD(&loader->columns);
  ncols = GRN_BULK_VSIZE(&loader->columns) / sizeof(grn_obj *);
  GRN_UINT32_POP(&loader->level, begin);
  value = (grn_obj *)GRN_TEXT_VALUE(&loader->values) + begin;
  value_end = (grn_obj *)GRN_TEXT_VALUE(&loader->values) + loader->values_size;
  GRN_ASSERT(value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET);
  GRN_UINT32_SET(ctx, value, loader->values_size - begin - 1);
  value++;
  depth = GRN_BULK_VSIZE(&loader->level);
  if (depth > sizeof(uint32_t) * loader->emit_level) {
    return;
  }
  if (depth == 0 || !loader->table ||
      loader->columns_status == GRN_LOADER_COLUMNS_BROKEN) {
    goto exit;
  }
  nvalues = values_len(ctx, value, value_end);

  if (loader->columns_status == GRN_LOADER_COLUMNS_UNSET) {
    /*
     * Target columns and _id or _key are not specified yet and values are
     * handled as column names and "_id" or "_key".
     */
    for (i = 0; i < nvalues; i++) {
      const char *col_name;
      unsigned int col_name_size;
      if (value->header.domain != GRN_DB_TEXT) {
        grn_obj buffer;
        GRN_TEXT_INIT(&buffer, 0);
        grn_inspect(ctx, &buffer, value);
        ERR(GRN_INVALID_ARGUMENT,
            "column name must be string: <%.*s>",
            (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
        grn_loader_save_error(ctx, loader);
        GRN_OBJ_FIN(ctx, &buffer);
        loader->columns_status = GRN_LOADER_COLUMNS_BROKEN;
        goto exit;
      }
      col_name = GRN_TEXT_VALUE(value);
      col_name_size = GRN_TEXT_LEN(value);
      col = grn_obj_column(ctx, loader->table, col_name, col_name_size);
      if (!col) {
        ERR(GRN_INVALID_ARGUMENT, "nonexistent column: <%.*s>",
            col_name_size, col_name);
        grn_loader_save_error(ctx, loader);
        loader->columns_status = GRN_LOADER_COLUMNS_BROKEN;
        goto exit;
      }
      if (name_equal(col_name, col_name_size, GRN_COLUMN_NAME_ID)) {
        grn_obj_unlink(ctx, col);
        if (loader->id_offset != -1 || loader->key_offset != -1) {
          /* _id and _key must not appear more than once. */
          if (loader->id_offset != -1) {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_ID, i,
                GRN_COLUMN_NAME_ID, loader->id_offset);
          } else {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_ID, i,
                GRN_COLUMN_NAME_KEY, loader->key_offset);
          }
          grn_loader_save_error(ctx, loader);
          loader->columns_status = GRN_LOADER_COLUMNS_BROKEN;
          goto exit;
        }
        loader->id_offset = i;
      } else if (name_equal(col_name, col_name_size, GRN_COLUMN_NAME_KEY)) {
        grn_obj_unlink(ctx, col);
        if (loader->id_offset != -1 || loader->key_offset != -1) {
          /* _id and _key must not appear more than once. */
          if (loader->id_offset != -1) {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_KEY, i,
                GRN_COLUMN_NAME_ID, loader->id_offset);
          } else {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_KEY, i,
                GRN_COLUMN_NAME_KEY, loader->key_offset);
          }
          grn_loader_save_error(ctx, loader);
          loader->columns_status = GRN_LOADER_COLUMNS_BROKEN;
          goto exit;
        }
        loader->key_offset = i;
      } else {
        GRN_PTR_PUT(ctx, &loader->columns, col);
      }
      value++;
    }
    switch (loader->table->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
      if (loader->id_offset == -1 && loader->key_offset == -1) {
        ERR(GRN_INVALID_ARGUMENT, "missing id or key column");
        grn_loader_save_error(ctx, loader);
        loader->columns_status = GRN_LOADER_COLUMNS_BROKEN;
        goto exit;
      }
      break;
    }
    loader->columns_status = GRN_LOADER_COLUMNS_SET;
    goto exit;
  }

  is_record_load = GRN_TRUE;

  /* Target columns and _id or _key are already specified. */
  if (!nvalues) {
    /*
     * Accept empty arrays because a dump command may output a load command
     * which contains empty arrays for a table with deleted records.
     */
    id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
  } else {
    uint32_t expected_nvalues = ncols;
    if (loader->id_offset != -1 || loader->key_offset != -1) {
      expected_nvalues++;
    }
    if (nvalues != expected_nvalues) {
      ERR(GRN_INVALID_ARGUMENT,
          "unexpected #values: expected:%u, actual:%u",
          expected_nvalues, nvalues);
      grn_loader_save_error(ctx, loader);
      goto exit;
    }
    if (loader->id_offset != -1) {
      id_value = value + loader->id_offset;
      id = parse_id_value(ctx, id_value);
      if (grn_table_at(ctx, loader->table, id) == GRN_ID_NIL) {
        id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
      }
    } else if (loader->key_offset != -1) {
      key_value = value + loader->key_offset;
      id = loader_add(ctx, key_value);
    } else {
      id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
    }
  }
  if (id == GRN_ID_NIL) {
    /* Target record is not available. */
    goto exit;
  }

  for (i = 0; i < nvalues; i++, value = values_next(ctx, value)) {
    if ((uint) i == (uint) loader->id_offset ||
        (uint) i == (uint) loader->key_offset) {
       /* Skip _id and _key, because it's already used to get id. */
       continue;
    }
    col = *cols;
    if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET) {
      set_vector(ctx, col, id, value);
    } else if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACE) {
      set_weight_vector(ctx, col, id, value);
    } else {
      grn_obj_set_value(ctx, col, id, value, GRN_OBJ_SET);
    }
    if (ctx->rc != GRN_SUCCESS) {
      char column_name[GRN_TABLE_MAX_KEY_SIZE];
      unsigned int column_name_size;
      grn_loader_save_error(ctx, loader);
      column_name_size = grn_obj_name(ctx, col, column_name,
                                      GRN_TABLE_MAX_KEY_SIZE);
      report_set_column_value_failure(ctx, key_value,
                                      column_name, column_name_size,
                                      value);
      ERRCLR(ctx);
    }
    cols++;
  }
  if (loader->each) {
    grn_obj *v = grn_expr_get_var_by_offset(ctx, loader->each, 0);
    GRN_RECORD_SET(ctx, v, id);
    grn_expr_exec(ctx, loader->each, 0);
  }
  loader->nrecords++;
exit:
  if (is_record_load) {
    if (loader->output_ids) {
      GRN_UINT32_PUT(ctx, &(loader->ids), id);
    }
    if (loader->output_errors) {
      GRN_INT32_PUT(ctx, &(loader->return_codes), ctx->rc);
      grn_vector_add_element(ctx,
                             &(loader->error_messages),
                             ctx->errbuf,
                             strlen(ctx->errbuf),
                             0,
                             GRN_DB_TEXT);
    }
  }
  loader->values_size = begin;
  ERRCLR(ctx);
}

static void
brace_close(grn_ctx *ctx, grn_loader *loader)
{
  grn_id id = GRN_ID_NIL;
  grn_obj *value, *value_begin, *value_end;
  grn_obj *id_value = NULL, *key_value = NULL;
  uint32_t begin;

  GRN_UINT32_POP(&loader->level, begin);
  value_begin = (grn_obj *)GRN_TEXT_VALUE(&loader->values) + begin;
  value_end = (grn_obj *)GRN_TEXT_VALUE(&loader->values) + loader->values_size;
  GRN_ASSERT(value->header.domain == GRN_JSON_LOAD_OPEN_BRACE);
  GRN_UINT32_SET(ctx, value_begin, loader->values_size - begin - 1);
  value_begin++;
  if ((size_t) GRN_BULK_VSIZE(&loader->level) > sizeof(uint32_t) * loader->emit_level) {
    return;
  }
  if (!loader->table) {
    goto exit;
  }

  /* Scan values to find _id or _key. */
  for (value = value_begin; value + 1 < value_end;
       value = values_next(ctx, value)) {
    const char *name = GRN_TEXT_VALUE(value);
    unsigned int name_size = GRN_TEXT_LEN(value);
    if (value->header.domain != GRN_DB_TEXT) {
      grn_obj buffer;
      GRN_TEXT_INIT(&buffer, 0);
      grn_inspect(ctx, &buffer, value);
      GRN_LOG(ctx, GRN_LOG_ERROR,
              "column name must be string: <%.*s>",
              (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
      GRN_OBJ_FIN(ctx, &buffer);
      goto exit;
    }
    value++;
    if (name_equal(name, name_size, GRN_COLUMN_NAME_ID)) {
      if (id_value || key_value) {
        if (loader->table->header.type == GRN_TABLE_NO_KEY) {
          GRN_LOG(ctx, GRN_LOG_ERROR, "duplicated '_id' column");
          goto exit;
        } else {
          GRN_LOG(ctx, GRN_LOG_ERROR,
                  "duplicated key columns: %s and %s",
                  id_value ? GRN_COLUMN_NAME_ID : GRN_COLUMN_NAME_KEY,
                  GRN_COLUMN_NAME_ID);
          goto exit;
        }
      }
      id_value = value;
    } else if (name_equal(name, name_size, GRN_COLUMN_NAME_KEY)) {
      if (id_value || key_value) {
        GRN_LOG(ctx, GRN_LOG_ERROR,
                "duplicated key columns: %s and %s",
                id_value ? GRN_COLUMN_NAME_ID : GRN_COLUMN_NAME_KEY,
                GRN_COLUMN_NAME_KEY);
        goto exit;
      }
      key_value = value;
    }
  }

  switch (loader->table->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    /* The target table requires _id or _key. */
    if (!id_value && !key_value) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "neither _key nor _id is assigned");
      goto exit;
    }
    break;
  default :
    /* The target table does not have _key. */
    if (key_value) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "nonexistent key value");
      goto exit;
    }
    break;
  }

  if (id_value) {
    id = parse_id_value(ctx, id_value);
    if (grn_table_at(ctx, loader->table, id) == GRN_ID_NIL) {
      if (ctx->rc == GRN_SUCCESS) {
        id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
      }
    }
  } else if (key_value) {
    id = loader_add(ctx, key_value);
  } else {
    id = grn_table_add(ctx, loader->table, NULL, 0, NULL);
  }
  if (id == GRN_ID_NIL) {
    /* Target record is not available. */
    goto exit;
  }

  for (value = value_begin; value + 1 < value_end;
       value = values_next(ctx, value)) {
    grn_obj *col;
    const char *name = GRN_TEXT_VALUE(value);
    unsigned int name_size = GRN_TEXT_LEN(value);
    value++;
    if (value == id_value || value == key_value) {
      /* Skip _id and _key, because it's already used to get id. */
      continue;
    }
    col = grn_obj_column(ctx, loader->table, name, name_size);
    if (!col) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "invalid column('%.*s')",
              (int)name_size, name);
      /* Automatic column creation is disabled. */
      /*
      if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET) {
        grn_obj *v = value + 1;
        col = grn_column_create(ctx, loader->table, name, name_size,
                                NULL, GRN_OBJ_PERSISTENT|GRN_OBJ_COLUMN_VECTOR,
                                grn_ctx_at(ctx, v->header.domain));
      } else {
        col = grn_column_create(ctx, loader->table, name, name_size,
                                NULL, GRN_OBJ_PERSISTENT,
                                grn_ctx_at(ctx, value->header.domain));
      }
      */
    } else {
      if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACKET) {
        set_vector(ctx, col, id, value);
      } else if (value->header.domain == GRN_JSON_LOAD_OPEN_BRACE) {
        set_weight_vector(ctx, col, id, value);
      } else {
        grn_obj_set_value(ctx, col, id, value, GRN_OBJ_SET);
      }
      if (ctx->rc != GRN_SUCCESS) {
        grn_loader_save_error(ctx, loader);
        report_set_column_value_failure(ctx, key_value,
                                        name, name_size, value);
        ERRCLR(ctx);
      }
      grn_obj_unlink(ctx, col);
    }
  }
  if (loader->each) {
    value = grn_expr_get_var_by_offset(ctx, loader->each, 0);
    GRN_RECORD_SET(ctx, value, id);
    grn_expr_exec(ctx, loader->each, 0);
  }
  loader->nrecords++;
exit:
  if (loader->output_ids) {
    GRN_UINT32_PUT(ctx, &(loader->ids), id);
  }
  if (loader->output_errors) {
    GRN_INT32_PUT(ctx, &(loader->return_codes), ctx->rc);
    grn_vector_add_element(ctx,
                           &(loader->error_messages),
                           ctx->errbuf,
                           strlen(ctx->errbuf),
                           0,
                           GRN_DB_TEXT);
  }
  loader->values_size = begin;
  ERRCLR(ctx);
}

#define JSON_READ_OPEN_BRACKET() do {\
  GRN_UINT32_PUT(ctx, &loader->level, loader->values_size);\
  values_add(ctx, loader);\
  loader->last->header.domain = GRN_JSON_LOAD_OPEN_BRACKET;\
  loader->stat = GRN_LOADER_TOKEN;\
  str++;\
} while (0)

#define JSON_READ_OPEN_BRACE() do {\
  GRN_UINT32_PUT(ctx, &loader->level, loader->values_size);\
  values_add(ctx, loader);\
  loader->last->header.domain = GRN_JSON_LOAD_OPEN_BRACE;\
  loader->stat = GRN_LOADER_TOKEN;\
  str++;\
} while (0)

static void
json_read(grn_ctx *ctx, grn_loader *loader, const char *str, unsigned int str_len)
{
  const char *const beg = str;
  char c;
  int len;
  const char *se = str + str_len;
  while (str < se) {
    c = *str;
    switch (loader->stat) {
    case GRN_LOADER_BEGIN :
      if ((len = grn_isspace(str, ctx->encoding))) {
        str += len;
        continue;
      }
      switch (c) {
      case '[' :
        JSON_READ_OPEN_BRACKET();
        break;
      case '{' :
        JSON_READ_OPEN_BRACE();
        break;
      default :
        ERR(GRN_INVALID_ARGUMENT,
            "JSON must start with '[' or '{': <%.*s>", str_len, beg);
        loader->stat = GRN_LOADER_END;
        break;
      }
      break;
    case GRN_LOADER_TOKEN :
      if ((len = grn_isspace(str, ctx->encoding))) {
        str += len;
        continue;
      }
      switch (c) {
      case '"' :
        loader->stat = GRN_LOADER_STRING;
        values_add(ctx, loader);
        str++;
        break;
      case '[' :
        JSON_READ_OPEN_BRACKET();
        break;
      case '{' :
        JSON_READ_OPEN_BRACE();
        break;
      case ':' :
        str++;
        break;
      case ',' :
        str++;
        break;
      case ']' :
        bracket_close(ctx, loader);
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        if (ctx->rc == GRN_CANCEL) {
          loader->stat = GRN_LOADER_END;
        }
        str++;
        break;
      case '}' :
        brace_close(ctx, loader);
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        if (ctx->rc == GRN_CANCEL) {
          loader->stat = GRN_LOADER_END;
        }
        str++;
        break;
      case '+' : case '-' : case '0' : case '1' : case '2' : case '3' :
      case '4' : case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->stat = GRN_LOADER_NUMBER;
        values_add(ctx, loader);
        break;
      default :
        if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('_' == c)) {
          loader->stat = GRN_LOADER_SYMBOL;
          values_add(ctx, loader);
        } else {
          if ((len = grn_charlen(ctx, str, se))) {
            GRN_LOG(ctx, GRN_LOG_ERROR, "ignored invalid char('%c') at", c);
            GRN_LOG(ctx, GRN_LOG_ERROR, "%.*s", (int)(str - beg) + len, beg);
            GRN_LOG(ctx, GRN_LOG_ERROR, "%*s", (int)(str - beg) + 1, "^");
            str += len;
          } else {
            GRN_LOG(ctx, GRN_LOG_ERROR, "ignored invalid char(\\x%.2x) after", c);
            GRN_LOG(ctx, GRN_LOG_ERROR, "%.*s", (int)(str - beg), beg);
            str = se;
          }
        }
        break;
      }
      break;
    case GRN_LOADER_SYMBOL :
      if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') ||
          ('0' <= c && c <= '9') || ('_' == c)) {
        GRN_TEXT_PUTC(ctx, loader->last, c);
        str++;
      } else {
        char *v = GRN_TEXT_VALUE(loader->last);
        switch (*v) {
        case 'n' :
          if (GRN_TEXT_LEN(loader->last) == 4 && !memcmp(v, "null", 4)) {
            loader->last->header.domain = GRN_DB_VOID;
            GRN_BULK_REWIND(loader->last);
          }
          break;
        case 't' :
          if (GRN_TEXT_LEN(loader->last) == 4 && !memcmp(v, "true", 4)) {
            loader->last->header.domain = GRN_DB_BOOL;
            GRN_BOOL_SET(ctx, loader->last, GRN_TRUE);
          }
          break;
        case 'f' :
          if (GRN_TEXT_LEN(loader->last) == 5 && !memcmp(v, "false", 5)) {
            loader->last->header.domain = GRN_DB_BOOL;
            GRN_BOOL_SET(ctx, loader->last, GRN_FALSE);
          }
          break;
        default :
          break;
        }
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
      }
      break;
    case GRN_LOADER_NUMBER :
      switch (c) {
      case '+' : case '-' : case '.' : case 'e' : case 'E' :
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        GRN_TEXT_PUTC(ctx, loader->last, c);
        str++;
        break;
      default :
        {
          const char *cur, *str = GRN_BULK_HEAD(loader->last);
          const char *str_end = GRN_BULK_CURR(loader->last);
          int64_t i = grn_atoll(str, str_end, &cur);
          if (cur == str_end) {
            loader->last->header.domain = GRN_DB_INT64;
            GRN_INT64_SET(ctx, loader->last, i);
          } else if (cur != str) {
            uint64_t i = grn_atoull(str, str_end, &cur);
            if (cur == str_end) {
              loader->last->header.domain = GRN_DB_UINT64;
              GRN_UINT64_SET(ctx, loader->last, i);
            } else if (cur != str) {
              double d;
              char *end;
              grn_obj buf;
              GRN_TEXT_INIT(&buf, 0);
              GRN_TEXT_PUT(ctx, &buf, str, GRN_BULK_VSIZE(loader->last));
              GRN_TEXT_PUTC(ctx, &buf, '\0');
              errno = 0;
              d = strtod(GRN_TEXT_VALUE(&buf), &end);
              if (!errno && end + 1 == GRN_BULK_CURR(&buf)) {
                loader->last->header.domain = GRN_DB_FLOAT;
                GRN_FLOAT_SET(ctx, loader->last, d);
              }
              GRN_OBJ_FIN(ctx, &buf);
            }
          }
        }
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        break;
      }
      break;
    case GRN_LOADER_STRING :
      switch (c) {
      case '\\' :
        loader->stat = GRN_LOADER_STRING_ESC;
        str++;
        break;
      case '"' :
        str++;
        loader->stat = GRN_BULK_VSIZE(&loader->level) ? GRN_LOADER_TOKEN : GRN_LOADER_END;
        /*
        *(GRN_BULK_CURR(loader->last)) = '\0';
        GRN_LOG(ctx, GRN_LOG_ALERT, "read str(%s)", GRN_TEXT_VALUE(loader->last));
        */
        break;
      default :
        if ((len = grn_charlen(ctx, str, se))) {
          GRN_TEXT_PUT(ctx, loader->last, str, len);
          str += len;
        } else {
          GRN_LOG(ctx, GRN_LOG_ERROR, "ignored invalid char(\\x%.2x) after", c);
          GRN_LOG(ctx, GRN_LOG_ERROR, "%.*s", (int)(str - beg), beg);
          str = se;
        }
        break;
      }
      break;
    case GRN_LOADER_STRING_ESC :
      switch (c) {
      case 'b' :
        GRN_TEXT_PUTC(ctx, loader->last, '\b');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'f' :
        GRN_TEXT_PUTC(ctx, loader->last, '\f');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'n' :
        GRN_TEXT_PUTC(ctx, loader->last, '\n');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'r' :
        GRN_TEXT_PUTC(ctx, loader->last, '\r');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 't' :
        GRN_TEXT_PUTC(ctx, loader->last, '\t');
        loader->stat = GRN_LOADER_STRING;
        break;
      case 'u' :
        loader->stat = GRN_LOADER_UNICODE0;
        break;
      default :
        GRN_TEXT_PUTC(ctx, loader->last, c);
        loader->stat = GRN_LOADER_STRING;
        break;
      }
      str++;
      break;
    case GRN_LOADER_UNICODE0 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar = (c - '0') * 0x1000;
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar = (c - 'a' + 10) * 0x1000;
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar = (c - 'A' + 10) * 0x1000;
        break;
      default :
        ;// todo : error
      }
      loader->stat = GRN_LOADER_UNICODE1;
      str++;
      break;
    case GRN_LOADER_UNICODE1 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar += (c - '0') * 0x100;
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar += (c - 'a' + 10) * 0x100;
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar += (c - 'A' + 10) * 0x100;
        break;
      default :
        ;// todo : error
      }
      loader->stat = GRN_LOADER_UNICODE2;
      str++;
      break;
    case GRN_LOADER_UNICODE2 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar += (c - '0') * 0x10;
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar += (c - 'a' + 10) * 0x10;
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar += (c - 'A' + 10) * 0x10;
        break;
      default :
        ;// todo : error
      }
      loader->stat = GRN_LOADER_UNICODE3;
      str++;
      break;
    case GRN_LOADER_UNICODE3 :
      switch (c) {
      case '0' : case '1' : case '2' : case '3' : case '4' :
      case '5' : case '6' : case '7' : case '8' : case '9' :
        loader->unichar += (c - '0');
        break;
      case 'a' : case 'b' : case 'c' : case 'd' : case 'e' : case 'f' :
        loader->unichar += (c - 'a' + 10);
        break;
      case 'A' : case 'B' : case 'C' : case 'D' : case 'E' : case 'F' :
        loader->unichar += (c - 'A' + 10);
        break;
      default :
        ;// todo : error
      }
      {
        uint32_t u = loader->unichar;
        if (u < 0x80) {
          GRN_TEXT_PUTC(ctx, loader->last, u);
        } else {
          if (u < 0x800) {
            GRN_TEXT_PUTC(ctx, loader->last, ((u >> 6) & 0x1f) | 0xc0);
          } else {
            GRN_TEXT_PUTC(ctx, loader->last, (u >> 12) | 0xe0);
            GRN_TEXT_PUTC(ctx, loader->last, ((u >> 6) & 0x3f) | 0x80);
          }
          GRN_TEXT_PUTC(ctx, loader->last, (u & 0x3f) | 0x80);
        }
      }
      loader->stat = GRN_LOADER_STRING;
      str++;
      break;
    case GRN_LOADER_END :
      str = se;
      break;
    }
  }
}

#undef JSON_READ_OPEN_BRACKET
#undef JSON_READ_OPEN_BRACE

/*
 * grn_loader_parse_columns parses a columns parameter.
 * Columns except _id and _key are appended to loader->columns.
 * If it contains _id or _key, loader->id_offset or loader->key_offset is set.
 */
static grn_rc
grn_loader_parse_columns(grn_ctx *ctx, grn_loader *loader,
                         const char *str, unsigned int str_size)
{
  const char *ptr = str, *ptr_end = ptr + str_size, *rest;
  const char *tokens[256], *token_end;
  while (ptr < ptr_end) {
    int i, n = grn_tokenize(ptr, ptr_end - ptr, tokens, 256, &rest);
    for (i = 0; i < n; i++) {
      grn_obj *column;
      token_end = tokens[i];
      while (ptr < token_end && (' ' == *ptr || ',' == *ptr)) {
        ptr++;
      }
      column = grn_obj_column(ctx, loader->table, ptr, token_end - ptr);
      if (!column) {
        ERR(GRN_INVALID_ARGUMENT, "nonexistent column: <%.*s>",
            (int)(token_end - ptr), ptr);
        return ctx->rc;
      }
      if (name_equal(ptr, token_end - ptr, GRN_COLUMN_NAME_ID)) {
        grn_obj_unlink(ctx, column);
        if (loader->id_offset != -1 || loader->key_offset != -1) {
          /* _id and _key must not appear more than once. */
          if (loader->id_offset != -1) {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_ID, i,
                GRN_COLUMN_NAME_ID, loader->id_offset);
          } else {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_ID, i,
                GRN_COLUMN_NAME_KEY, loader->key_offset);
          }
          return ctx->rc;
        }
        loader->id_offset = i;
      } else if (name_equal(ptr, token_end - ptr, GRN_COLUMN_NAME_KEY)) {
        grn_obj_unlink(ctx, column);
        if (loader->id_offset != -1 || loader->key_offset != -1) {
          /* _id and _key must not appear more than once. */
          if (loader->id_offset != -1) {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_KEY, i,
                GRN_COLUMN_NAME_ID, loader->id_offset);
          } else {
            ERR(GRN_INVALID_ARGUMENT,
                "duplicated id and key columns: <%s> at %d and <%s> at %d",
                GRN_COLUMN_NAME_KEY, i,
                GRN_COLUMN_NAME_KEY, loader->key_offset);
          }
          return ctx->rc;
        }
        loader->key_offset = i;
      } else {
        GRN_PTR_PUT(ctx, &loader->columns, column);
      }
      ptr = token_end;
    }
    ptr = rest;
  }
  switch (loader->table->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    if (loader->id_offset == -1 && loader->key_offset == -1) {
      ERR(GRN_INVALID_ARGUMENT, "missing id or key column");
      return ctx->rc;
    }
    break;
  }
  return ctx->rc;
}

static grn_com_addr *addr;

void
grn_load_internal(grn_ctx *ctx, grn_load_input *input)
{
  grn_loader *loader = &ctx->impl->loader;

  loader->emit_level = input->emit_level;
  if (ctx->impl->edge) {
    grn_edge *edge = grn_edges_add_communicator(ctx, addr);
    grn_obj *msg = grn_msg_open(ctx, edge->com, &ctx->impl->edge->send_old);
    /* build msg */
    grn_edge_dispatch(ctx, edge, msg);
  }
  if (input->table.length > 0) {
    grn_ctx_loader_clear(ctx);
    loader->input_type = input->type;
    if (grn_db_check_name(ctx, input->table.value, input->table.length)) {
      GRN_DB_CHECK_NAME_ERR("[table][load]",
                            input->table.value,
                            (int)(input->table.length));
      loader->stat = GRN_LOADER_END;
      return;
    }
    loader->table = grn_ctx_get(ctx, input->table.value, input->table.length);
    if (!loader->table) {
      ERR(GRN_INVALID_ARGUMENT,
          "nonexistent table: <%.*s>",
          (int)(input->table.length),
          input->table.value);
      loader->stat = GRN_LOADER_END;
      return;
    }
    if (input->columns.length > 0) {
      grn_rc rc = grn_loader_parse_columns(ctx,
                                           loader,
                                           input->columns.value,
                                           input->columns.length);
      if (rc != GRN_SUCCESS) {
        loader->columns_status = GRN_LOADER_COLUMNS_BROKEN;
        loader->stat = GRN_LOADER_END;
        return;
      }
      loader->columns_status = GRN_LOADER_COLUMNS_SET;
    }
    if (input->if_exists.length > 0) {
      grn_obj *v;
      GRN_EXPR_CREATE_FOR_QUERY(ctx, loader->table, loader->ifexists, v);
      if (loader->ifexists && v) {
        grn_expr_parse(ctx,
                       loader->ifexists,
                       input->if_exists.value,
                       input->if_exists.length,
                       NULL, GRN_OP_EQUAL, GRN_OP_AND,
                       GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
      }
    }
    if (input->each.length > 0) {
      grn_obj *v;
      GRN_EXPR_CREATE_FOR_QUERY(ctx, loader->table, loader->each, v);
      if (loader->each && v) {
        grn_expr_parse(ctx, loader->each,
                       input->each.value,
                       input->each.length,
                       NULL, GRN_OP_EQUAL, GRN_OP_AND,
                       GRN_EXPR_SYNTAX_SCRIPT|GRN_EXPR_ALLOW_UPDATE);
      }
    }
    loader->output_ids = input->output_ids;
    loader->output_errors = input->output_errors;
  } else {
    if (!loader->table) {
      ERR(GRN_INVALID_ARGUMENT, "mandatory \"table\" parameter is absent");
      loader->stat = GRN_LOADER_END;
      return;
    }
  }
  switch (loader->input_type) {
  case GRN_CONTENT_JSON :
    json_read(ctx, loader, input->values.value, input->values.length);
    break;
  case GRN_CONTENT_NONE :
  case GRN_CONTENT_TSV :
  case GRN_CONTENT_XML :
  case GRN_CONTENT_MSGPACK :
  case GRN_CONTENT_GROONGA_COMMAND_LIST :
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "unsupported input_type");
    loader->stat = GRN_LOADER_END;
    // todo
    break;
  }
}

grn_rc
grn_load(grn_ctx *ctx, grn_content_type input_type,
         const char *table, unsigned int table_len,
         const char *columns, unsigned int columns_len,
         const char *values, unsigned int values_len,
         const char *ifexists, unsigned int ifexists_len,
         const char *each, unsigned int each_len)
{
  if (!ctx || !ctx->impl) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return ctx->rc;
  }
  GRN_API_ENTER;
  {
    grn_load_input input;
    input.type = input_type;
    input.table.value = table;
    input.table.length = table_len;
    input.columns.value = columns;
    input.columns.length = columns_len;
    input.values.value = values;
    input.values.length = values_len;
    input.if_exists.value = ifexists;
    input.if_exists.length = ifexists_len;
    input.each.value = each;
    input.each.length = each_len;
    input.output_ids = GRN_FALSE;
    input.output_errors = GRN_FALSE;
    input.emit_level = 1;
    grn_load_internal(ctx, &input);
  }
  GRN_API_RETURN(ctx->rc);
}
