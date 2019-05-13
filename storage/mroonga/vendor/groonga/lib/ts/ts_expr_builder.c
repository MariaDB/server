/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "ts_expr_builder.h"

#include <string.h>

#include "../grn_ctx.h"
#include "../grn_db.h"

#include "ts_log.h"
#include "ts_util.h"

/*-------------------------------------------------------------
 * grn_ts_expr_bridge.
 */

/* grn_ts_expr_bridge_init() initializes a bridge. */
static void
grn_ts_expr_bridge_init(grn_ctx *ctx, grn_ts_expr_bridge *bridge)
{
  memset(bridge, 0, sizeof(*bridge));
  bridge->src_table = NULL;
  bridge->dest_table = NULL;
}

/* grn_ts_expr_bridge_fin() finalizes a bridge. */
static void
grn_ts_expr_bridge_fin(grn_ctx *ctx, grn_ts_expr_bridge *bridge)
{
  if (bridge->dest_table) {
    grn_obj_unlink(ctx, bridge->dest_table);
  }
  /* Note: bridge->src_table does not increment a reference count. */
}

/*-------------------------------------------------------------
 * grn_ts_expr_builder.
 */

/* grn_ts_expr_builder_init() initializes an expression builder. */
static void
grn_ts_expr_builder_init(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  memset(builder, 0, sizeof(*builder));
  builder->table = NULL;
  builder->curr_table = NULL;
  builder->nodes = NULL;
  builder->bridges = NULL;
}

/* grn_ts_expr_builder_fin() finalizes an expression builder. */
static void
grn_ts_expr_builder_fin(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  size_t i;
  if (builder->bridges) {
    for (i = 0; i < builder->n_bridges; i++) {
      grn_ts_expr_bridge_fin(ctx, &builder->bridges[i]);
    }
    GRN_FREE(builder->bridges);
  }
  if (builder->nodes) {
    for (i = 0; i < builder->n_nodes; i++) {
      if (builder->nodes[i]) {
        grn_ts_expr_node_close(ctx, builder->nodes[i]);
      }
    }
    GRN_FREE(builder->nodes);
  }
  /* Note: builder->curr_table does not increment a reference count. */
  if (builder->table) {
    grn_obj_unlink(ctx, builder->table);
  }
}

grn_rc
grn_ts_expr_builder_open(grn_ctx *ctx, grn_obj *table,
                         grn_ts_expr_builder **builder)
{
  grn_rc rc;
  grn_ts_expr_builder *new_builder;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) || !builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  new_builder = GRN_MALLOCN(grn_ts_expr_builder, 1);
  if (!new_builder) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE,
                      sizeof(grn_ts_expr_builder));
  }
  rc = grn_ts_obj_increment_ref_count(ctx, table);
  if (rc != GRN_SUCCESS) {
    GRN_FREE(new_builder);
    return rc;
  }
  grn_ts_expr_builder_init(ctx, new_builder);
  new_builder->table = table;
  new_builder->curr_table = table;
  *builder = new_builder;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_builder_close(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  grn_ts_expr_builder_fin(ctx, builder);
  GRN_FREE(builder);
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_builder_complete(grn_ctx *ctx, grn_ts_expr_builder *builder,
                             grn_ts_expr **expr)
{
  grn_rc rc;
  grn_ts_expr *new_expr;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder || (builder->n_nodes != 1) || builder->n_bridges || !expr) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_node_deref(ctx, &builder->nodes[0]);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_open(ctx, builder->table, builder->nodes[0], &new_expr);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  builder->n_nodes = 0;
  *expr = new_expr;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_builder_clear(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  size_t i;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (builder->bridges) {
    for (i = 0; i < builder->n_bridges; i++) {
      grn_ts_expr_bridge_fin(ctx, &builder->bridges[i]);
    }
    builder->n_bridges = 0;
  }
  if (builder->nodes) {
    for (i = 0; i < builder->n_nodes; i++) {
      if (builder->nodes[i]) {
        grn_ts_expr_node_close(ctx, builder->nodes[i]);
      }
    }
    builder->n_nodes = 0;
  }
  builder->curr_table = builder->table;
  return GRN_SUCCESS;
}

/*
 * grn_ts_expr_builder_push_node() pushes a node.
 * The given node will be closed on failure.
 */
static grn_rc
grn_ts_expr_builder_push_node(grn_ctx *ctx, grn_ts_expr_builder *builder,
                              grn_ts_expr_node *node)
{
  if (builder->n_nodes == builder->max_n_nodes) {
    size_t n_bytes, new_max_n_nodes;
    grn_ts_expr_node **new_nodes;
    new_max_n_nodes = builder->n_nodes ? (builder->n_nodes * 2) : 1;
    n_bytes = sizeof(grn_ts_expr_node *) * new_max_n_nodes;
    new_nodes = (grn_ts_expr_node **)GRN_REALLOC(builder->nodes, n_bytes);
    if (!new_nodes) {
      grn_ts_expr_node_close(ctx, node);
      GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                        "GRN_REALLOC failed: %" GRN_FMT_SIZE, n_bytes);
    }
    builder->nodes = new_nodes;
    builder->max_n_nodes = new_max_n_nodes;
  }
  builder->nodes[builder->n_nodes++] = node;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_builder_push_name(grn_ctx *ctx, grn_ts_expr_builder *builder,
                              grn_ts_str name)
{
  grn_obj *column;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder || !grn_ts_str_is_name(name)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (grn_ts_str_is_id_name(name)) {
    return grn_ts_expr_builder_push_id(ctx, builder);
  }
  if (grn_ts_str_is_score_name(name)) {
    return grn_ts_expr_builder_push_score(ctx, builder);
  }
  if (grn_ts_str_is_key_name(name)) {
    return grn_ts_expr_builder_push_key(ctx, builder);
  }
  if (grn_ts_str_is_value_name(name)) {
    return grn_ts_expr_builder_push_value(ctx, builder);
  }
  /* grn_obj_column() returns a column or accessor. */
  column = grn_obj_column(ctx, builder->curr_table, name.ptr, name.size);
  if (!column) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "object not found: \"%.*s\"",
                      (int)name.size, name.ptr);
  }
  return grn_ts_expr_builder_push_obj(ctx, builder, column);
}

#define GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(TYPE, KIND, kind)\
  case GRN_DB_ ## TYPE: {\
    value.as_ ## kind = (grn_ts_ ## kind)GRN_ ## TYPE ## _VALUE(obj);\
    return grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_ ## KIND,\
                                          obj->header.domain, value);\
  }
/* grn_ts_expr_push_builder_bulk() pushes a scalar const. */
static grn_rc
grn_ts_expr_builder_push_bulk(grn_ctx *ctx, grn_ts_expr_builder *builder,
                              grn_obj *obj)
{
  grn_ts_any value;
  switch (obj->header.domain) {
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(BOOL, BOOL, bool)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(INT8, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(INT16, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(INT32, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(INT64, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(UINT8, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(UINT16, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(UINT32, INT, int)
    /* The behavior is undefined if a value is greater than 2^63 - 1. */
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(UINT64, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(FLOAT, FLOAT, float)
    GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE(TIME, TIME, time)
    case GRN_DB_SHORT_TEXT:
    case GRN_DB_TEXT:
    case GRN_DB_LONG_TEXT: {
      value.as_text.ptr = GRN_TEXT_VALUE(obj);
      value.as_text.size = GRN_TEXT_LEN(obj);
      return grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_TEXT,
                                            obj->header.domain, value);
    }
    case GRN_DB_TOKYO_GEO_POINT:
    case GRN_DB_WGS84_GEO_POINT: {
      GRN_GEO_POINT_VALUE(obj, value.as_geo.latitude, value.as_geo.longitude);
      return grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_GEO,
                                            obj->header.domain, value);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "not bulk");
    }
  }
}
#undef GRN_TS_EXPR_BUILDER_PUSH_BULK_CASE

#define GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(TYPE, KIND, kind)\
  case GRN_DB_ ## TYPE: {\
    value.as_ ## kind ## _vector.ptr = (grn_ts_ ## kind *)GRN_BULK_HEAD(obj);\
    value.as_ ## kind ## _vector.size = grn_uvector_size(ctx, obj);\
    return grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_ ## KIND,\
                                          obj->header.domain, value);\
  }
#define GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(TYPE, KIND, kind)\
  case GRN_DB_ ## TYPE: {\
    size_t i;\
    grn_rc rc;\
    grn_ts_ ## kind *buf;\
    grn_ts_ ## kind ## _vector vector = { NULL, grn_uvector_size(ctx, obj) };\
    if (!vector.size) {\
      value.as_ ## kind ## _vector = vector;\
      return grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_ ## KIND,\
                                            obj->header.domain, value);\
    }\
    buf = GRN_MALLOCN(grn_ts_ ## kind, vector.size);\
    if (!buf) {\
      GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,\
                        "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",\
                        sizeof(grn_ts_ ## kind));\
    }\
    for (i = 0; i < vector.size; i++) {\
      buf[i] = GRN_ ## TYPE ##_VALUE_AT(obj, i);\
    }\
    vector.ptr = buf;\
    value.as_ ## kind ## _vector = vector;\
    rc = grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_ ## KIND,\
                                        obj->header.domain, value);\
    GRN_FREE(buf);\
    return rc;\
  }
/* grn_ts_expr_builder_push_uvector() pushes an array of fixed-size values. */
static grn_rc
grn_ts_expr_builder_push_uvector(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                 grn_obj *obj)
{
  grn_ts_any value;
  switch (obj->header.domain) {
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(BOOL, BOOL, bool)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(INT8, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(INT16, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(INT32, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(INT64, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(UINT8, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(UINT16, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST(UINT32, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(UINT64, INT, int)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(TIME, TIME, time)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(TOKYO_GEO_POINT, GEO, geo)
    GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE(WGS84_GEO_POINT, GEO, geo)
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data type: %d",
                        obj->header.domain);
    }
  }
}
#undef GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE_WITH_TYPECAST
#undef GRN_TS_EXPR_BUILDER_PUSH_UVECTOR_CASE

/* grn_ts_expr_builder_push_vector() pushes a Text vector. */
static grn_rc
grn_ts_expr_builder_push_vector(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                grn_obj *obj)
{
  switch (obj->header.domain) {
    case GRN_DB_SHORT_TEXT:
    case GRN_DB_TEXT:
    case GRN_DB_LONG_TEXT: {
      size_t i;
      grn_rc rc;
      grn_ts_any value;
      grn_ts_text *buf;
      grn_ts_text_vector vector = { NULL, grn_vector_size(ctx, obj) };
      if (!vector.size) {
        value.as_text_vector = vector;
        return grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_TEXT_VECTOR,
                                              obj->header.domain, value);
      }
      buf = GRN_MALLOCN(grn_ts_text, vector.size);
      if (!buf) {
        GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                          "GRN_MALLOCN failed: "
                          "%" GRN_FMT_SIZE " x %" GRN_FMT_SIZE,
                          sizeof(grn_ts_text), vector.size);
      }
      for (i = 0; i < vector.size; i++) {
        buf[i].size = grn_vector_get_element(ctx, obj, i, &buf[i].ptr,
                                             NULL, NULL);
      }
      vector.ptr = buf;
      value.as_text_vector = vector;
      rc = grn_ts_expr_builder_push_const(ctx, builder, GRN_TS_TEXT_VECTOR,
                                          obj->header.domain, value);
      GRN_FREE(buf);
      return rc;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data type: %d",
                        obj->header.domain);
    }
  }
}

static grn_rc
grn_ts_expr_builder_push_single_accessor(grn_ctx *ctx,
                                         grn_ts_expr_builder *builder,
                                         grn_accessor *accessor)
{
  switch (accessor->action) {
    case GRN_ACCESSOR_GET_ID: {
      return grn_ts_expr_builder_push_id(ctx, builder);
    }
    case GRN_ACCESSOR_GET_SCORE: {
      return grn_ts_expr_builder_push_score(ctx, builder);
    }
    case GRN_ACCESSOR_GET_KEY: {
      if (accessor->obj != builder->curr_table) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "table conflict");
      }
      return grn_ts_expr_builder_push_key(ctx, builder);
    }
    case GRN_ACCESSOR_GET_VALUE: {
      if (accessor->obj != builder->curr_table) {
        GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "table conflict");
      }
      return grn_ts_expr_builder_push_value(ctx, builder);
    }
    case GRN_ACCESSOR_GET_COLUMN_VALUE: {
      return grn_ts_expr_builder_push_column(ctx, builder, accessor->obj);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid accessor action: %d",
                        accessor->action);
    }
  }
}

static grn_rc
grn_ts_expr_builder_push_accessor(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                  grn_accessor *accessor)
{
  grn_rc rc = grn_ts_expr_builder_push_single_accessor(ctx, builder, accessor);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  for (accessor = accessor->next; accessor; accessor = accessor->next) {
    rc = grn_ts_expr_builder_begin_subexpr(ctx, builder);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    rc = grn_ts_expr_builder_push_single_accessor(ctx, builder, accessor);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    rc = grn_ts_expr_builder_end_subexpr(ctx, builder);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_builder_push_obj(grn_ctx *ctx, grn_ts_expr_builder *builder,
                             grn_obj *obj)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder || !obj) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  switch (obj->header.type) {
    case GRN_BULK: {
      return grn_ts_expr_builder_push_bulk(ctx, builder, obj);
    }
    case GRN_UVECTOR: {
      return grn_ts_expr_builder_push_uvector(ctx, builder, obj);
    }
    case GRN_VECTOR: {
      return grn_ts_expr_builder_push_vector(ctx, builder, obj);
    }
    case GRN_ACCESSOR: {
      return grn_ts_expr_builder_push_accessor(ctx, builder,
                                               (grn_accessor *)obj);
    }
    case GRN_COLUMN_FIX_SIZE:
    case GRN_COLUMN_VAR_SIZE: {
      return grn_ts_expr_builder_push_column(ctx, builder, obj);
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid object type: %d",
                        obj->header.type);
    }
  }
}

grn_rc
grn_ts_expr_builder_push_id(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_rc rc;
  grn_ts_expr_node *node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_id_node_open(ctx, &node);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_expr_builder_push_node(ctx, builder, node);
  }
  return rc;
}

grn_rc
grn_ts_expr_builder_push_score(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_rc rc;
  grn_ts_expr_node *node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_score_node_open(ctx, &node);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_expr_builder_push_node(ctx, builder, node);
  }
  return rc;
}

grn_rc
grn_ts_expr_builder_push_key(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_rc rc;
  grn_ts_expr_node *node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_key_node_open(ctx, builder->curr_table, &node);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_expr_builder_push_node(ctx, builder, node);
  }
  return rc;
}

grn_rc
grn_ts_expr_builder_push_value(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_rc rc;
  grn_ts_expr_node *node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_value_node_open(ctx, builder->curr_table, &node);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_expr_builder_push_node(ctx, builder, node);
  }
  return rc;
}

grn_rc
grn_ts_expr_builder_push_const(grn_ctx *ctx, grn_ts_expr_builder *builder,
                               grn_ts_data_kind kind, grn_ts_data_type type,
                               grn_ts_any value)
{
  grn_rc rc;
  grn_ts_expr_node *node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_const_node_open(ctx, kind, type, value, &node);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_expr_builder_push_node(ctx, builder, node);
  }
  return rc;
}

grn_rc
grn_ts_expr_builder_push_column(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                grn_obj *column)
{
  grn_rc rc;
  grn_ts_expr_node *node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder || !column || !grn_ts_obj_is_column(ctx, column) ||
      (DB_OBJ(builder->curr_table)->id != column->header.domain)) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_column_node_open(ctx, column, &node);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_expr_builder_push_node(ctx, builder, node);
  }
  return rc;
}

/*
 * grn_ts_expr_builder_get_max_n_args() returns the number of nodes in the
 * current subexpression.
 */
static size_t
grn_ts_expr_builder_get_max_n_args(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  size_t max_n_args = builder->n_nodes;
  if (builder->n_bridges) {
   max_n_args -= builder->bridges[builder->n_bridges - 1].n_nodes;
  }
  return max_n_args;
}

grn_rc
grn_ts_expr_builder_push_op(grn_ctx *ctx, grn_ts_expr_builder *builder,
                            grn_ts_op_type op_type)
{
  grn_rc rc;
  grn_ts_expr_node **args, *node;
  size_t n_args, max_n_args;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  n_args = grn_ts_op_get_n_args(op_type);
  if (!n_args) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "invalid #arguments: %" GRN_FMT_SIZE,
                      n_args);
  }
  max_n_args = grn_ts_expr_builder_get_max_n_args(ctx, builder);
  if (n_args > max_n_args) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "invalid #arguments: %" GRN_FMT_SIZE ", %" GRN_FMT_SIZE,
                      n_args, builder->n_nodes);
  }
  /* Arguments are the top n_args nodes in the stack. */
  args = &builder->nodes[builder->n_nodes - n_args];
  builder->n_nodes -= n_args;
  rc = grn_ts_expr_op_node_open(ctx, op_type, args, n_args, &node);
  if (rc == GRN_SUCCESS) {
    builder->nodes[builder->n_nodes++] = node;
  }
  return rc;
}

/* grn_ts_expr_builder_push_bridge() pushes a bridge. */
static grn_rc
grn_ts_expr_builder_push_bridge(grn_ctx *ctx, grn_ts_expr_builder *builder,
                                grn_ts_expr_bridge *bridge)
{
  if (builder->n_bridges == builder->max_n_bridges) {
    size_t n_bytes, new_max_n_bridges;
    grn_ts_expr_bridge *new_bridges;
    new_max_n_bridges = builder->n_bridges ? (builder->n_bridges * 2) : 1;
    n_bytes = sizeof(grn_ts_expr_bridge) * new_max_n_bridges;
    new_bridges = (grn_ts_expr_bridge *)GRN_REALLOC(builder->bridges, n_bytes);
    if (!new_bridges) {
      grn_ts_expr_bridge_fin(ctx, bridge);
      GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                        "GRN_REALLOC failed: %" GRN_FMT_SIZE, n_bytes);
    }
    builder->bridges = new_bridges;
    builder->max_n_bridges = new_max_n_bridges;
  }
  builder->bridges[builder->n_bridges++] = *bridge;
  builder->curr_table = bridge->dest_table;
  return GRN_SUCCESS;
}

/* grn_ts_expr_builder_pop_bridge() pops a bridge. */
static void
grn_ts_expr_builder_pop_bridge(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_ts_expr_bridge *bridge = &builder->bridges[builder->n_bridges - 1];
  builder->curr_table = bridge->src_table;
  grn_ts_expr_bridge_fin(ctx, bridge);
  builder->n_bridges--;
}

grn_rc
grn_ts_expr_builder_begin_subexpr(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_rc rc;
  size_t max_n_args;
  grn_obj *obj;
  grn_ts_expr_node *node;
  grn_ts_expr_bridge bridge;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  max_n_args = grn_ts_expr_builder_get_max_n_args(ctx, builder);
  if (!max_n_args) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  /* Check whehter or not the latest node refers to a table. */
  node = builder->nodes[builder->n_nodes - 1];
  if ((node->data_kind & ~GRN_TS_VECTOR_FLAG) != GRN_TS_REF) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                      node->data_kind);
  }
  obj = grn_ctx_at(ctx, node->data_type);
  if (!obj) {
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "grn_ctx_at failed: %d",
                      node->data_type);
  }
  if (!grn_ts_obj_is_table(ctx, obj)) {
    grn_obj_unlink(ctx, obj);
    GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "not table: %d", node->data_type);
  }
  /* Creates a bridge to a subexpression. */
  grn_ts_expr_bridge_init(ctx, &bridge);
  bridge.src_table = builder->curr_table;
  bridge.dest_table = obj;
  bridge.n_nodes = builder->n_nodes;
  rc = grn_ts_expr_builder_push_bridge(ctx, builder, &bridge);
  if (rc != GRN_SUCCESS) {
    grn_obj_unlink(ctx, obj);
    return rc;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ts_expr_builder_end_subexpr(grn_ctx *ctx, grn_ts_expr_builder *builder)
{
  grn_rc rc;
  grn_ts_expr_node **args, *node;
  if (!ctx || !builder || (builder->n_nodes < 2) || !builder->n_bridges) {
    return GRN_INVALID_ARGUMENT;
  }
  /* Check whehter or not the subexpression is complete.*/
  if (grn_ts_expr_builder_get_max_n_args(ctx, builder) != 1) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  /* Creates a bridge node. */
  args = &builder->nodes[builder->n_nodes - 2];
  rc = grn_ts_expr_bridge_node_open(ctx, args[0], args[1], &node);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  /* Note: The following grn_ts_expr_push_node() must not fail. */
  builder->n_nodes -= 2;
  grn_ts_expr_builder_push_node(ctx, builder, node);
  grn_ts_expr_builder_pop_bridge(ctx, builder);
  return GRN_SUCCESS;
}
