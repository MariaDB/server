/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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
#include "../grn_rset.h"
#include "../grn_ii.h"

#include <groonga/plugin.h>

#include <string.h>

#define DIST(ox,oy) (dists[((lx + 1) * (oy)) + (ox)])

static uint32_t
calc_edit_distance(grn_ctx *ctx, char *sx, char *ex, char *sy, char *ey, int flags)
{
  int d = 0;
  uint32_t cx, lx, cy, ly, *dists;
  char *px, *py;
  for (px = sx, lx = 0; px < ex && (cx = grn_charlen(ctx, px, ex)); px += cx, lx++);
  for (py = sy, ly = 0; py < ey && (cy = grn_charlen(ctx, py, ey)); py += cy, ly++);
  if ((dists = GRN_PLUGIN_MALLOC(ctx, (lx + 1) * (ly + 1) * sizeof(uint32_t)))) {
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
          if (flags & GRN_TABLE_FUZZY_SEARCH_WITH_TRANSPOSITION &&
              x > 1 && y > 1 && cx == cy &&
              memcmp(px, py - cy, cx) == 0 &&
              memcmp(px - cx, py, cx) == 0) {
            uint32_t t = DIST(x - 2, y - 2) + 1;
            DIST(x, y) = ((DIST(x, y) < t) ? DIST(x, y) : t);
          }
        }
      }
    }
    d = DIST(lx, ly);
    GRN_PLUGIN_FREE(ctx, dists);
  }
  return d;
}

static grn_obj *
func_edit_distance(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
#define N_REQUIRED_ARGS 2
#define MAX_ARGS 3
  int d = 0;
  int flags = 0;
  grn_obj *obj;
  if (nargs >= N_REQUIRED_ARGS && nargs <= MAX_ARGS) {
    if (nargs == MAX_ARGS && GRN_BOOL_VALUE(args[2])) {
      flags |= GRN_TABLE_FUZZY_SEARCH_WITH_TRANSPOSITION;
    }
    d = calc_edit_distance(ctx, GRN_TEXT_VALUE(args[0]), GRN_BULK_CURR(args[0]),
                           GRN_TEXT_VALUE(args[1]), GRN_BULK_CURR(args[1]), flags);
  }
  if ((obj = grn_plugin_proc_alloc(ctx, user_data, GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, d);
  }
  return obj;
#undef N_REQUIRED_ARGS
#undef MAX_ARGS
}

void
grn_proc_init_edit_distance(grn_ctx *ctx)
{
  grn_proc_create(ctx, "edit_distance", -1, GRN_PROC_FUNCTION,
                  func_edit_distance, NULL, NULL, 0, NULL);
}

#define SCORE_HEAP_SIZE 256

typedef struct {
  grn_id id;
  uint32_t score;
} score_heap_node;

typedef struct {
  int n_entries;
  int limit;
  score_heap_node *nodes;
} score_heap;

static inline score_heap *
score_heap_open(grn_ctx *ctx, int max)
{
  score_heap *h = GRN_PLUGIN_MALLOC(ctx, sizeof(score_heap));
  if (!h) { return NULL; }
  h->nodes = GRN_PLUGIN_MALLOC(ctx, sizeof(score_heap_node) * max);
  if (!h->nodes) {
    GRN_PLUGIN_FREE(ctx, h);
    return NULL;
  }
  h->n_entries = 0;
  h->limit = max;
  return h;
}

static inline grn_bool
score_heap_push(grn_ctx *ctx, score_heap *h, grn_id id, uint32_t score)
{
  int n, n2;
  score_heap_node node = {id, score};
  score_heap_node node2;
  if (h->n_entries >= h->limit) {
    int max = h->limit * 2;
    score_heap_node *nodes;
    nodes = GRN_PLUGIN_REALLOC(ctx, h->nodes, sizeof(score_heap) * max);
    if (!nodes) {
      return GRN_FALSE;
    }
    h->limit = max;
    h->nodes = nodes;
  }
  h->nodes[h->n_entries] = node;
  n = h->n_entries++;
  while (n) {
    n2 = (n - 1) >> 1;
    if (h->nodes[n2].score <= h->nodes[n].score) { break; }
    node2 = h->nodes[n];
    h->nodes[n] = h->nodes[n2];
    h->nodes[n2] = node2;
    n = n2;
  }
  return GRN_TRUE;
}

static inline void
score_heap_close(grn_ctx *ctx, score_heap *h)
{
  GRN_PLUGIN_FREE(ctx, h->nodes);
  GRN_PLUGIN_FREE(ctx, h);
}

static grn_rc
sequential_fuzzy_search(grn_ctx *ctx, grn_obj *table, grn_obj *column, grn_obj *query,
                        uint32_t max_distance, uint32_t prefix_match_size,
                        uint32_t max_expansion, int flags, grn_obj *res, grn_operator op)
{
  grn_table_cursor *tc;
  char *sx = GRN_TEXT_VALUE(query);
  char *ex = GRN_BULK_CURR(query);

  if (op == GRN_OP_AND) {
    tc = grn_table_cursor_open(ctx, res, NULL, 0, NULL, 0, 0, -1, GRN_CURSOR_BY_ID);
  } else {
    tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, GRN_CURSOR_BY_ID);
  }
  if (tc) {
    grn_id id;
    grn_obj value;
    score_heap *heap;
    int i, n;
    GRN_TEXT_INIT(&value, 0);

    heap = score_heap_open(ctx, SCORE_HEAP_SIZE);
    if (!heap) {
      grn_table_cursor_close(ctx, tc);
      grn_obj_unlink(ctx, &value);
      return GRN_NO_MEMORY_AVAILABLE;
    }

    while ((id = grn_table_cursor_next(ctx, tc))) {
      unsigned int distance = 0;
      grn_obj *domain;
      grn_id record_id;

      if (op == GRN_OP_AND) {
        grn_id *key;
        grn_table_cursor_get_key(ctx, tc, (void **)&key);
        record_id = *key;
      } else {
        record_id = id;
      }
      GRN_BULK_REWIND(&value);
      grn_obj_get_value(ctx, column, record_id, &value);
      domain = grn_ctx_at(ctx, ((&value))->header.domain);
      if ((&(value))->header.type == GRN_VECTOR) {
        n = grn_vector_size(ctx, &value);
        for (i = 0; i < n; i++) {
          unsigned int length;
          const char *vector_value = NULL;
          length = grn_vector_get_element(ctx, &value, i, &vector_value, NULL, NULL);

          if (!prefix_match_size ||
              (prefix_match_size > 0 && length >= prefix_match_size &&
               !memcmp(sx, vector_value, prefix_match_size))) {
            distance = calc_edit_distance(ctx, sx, ex,
                                          (char *)vector_value,
                                          (char *)vector_value + length, flags);
            if (distance <= max_distance) {
              score_heap_push(ctx, heap, record_id, distance);
              break;
            }
          }
        }
      } else if ((&(value))->header.type == GRN_UVECTOR &&
                  grn_obj_is_table(ctx, domain)) {
        n = grn_vector_size(ctx, &value);
        for (i = 0; i < n; i++) {
          grn_id rid;
          char key_name[GRN_TABLE_MAX_KEY_SIZE];
          int key_length;
          rid = grn_uvector_get_element(ctx, &value, i, NULL);
          key_length = grn_table_get_key(ctx, domain, rid, key_name, GRN_TABLE_MAX_KEY_SIZE);

          if (!prefix_match_size ||
              (prefix_match_size > 0 && key_length >= (int) prefix_match_size &&
               !memcmp(sx, key_name, prefix_match_size))) {
            distance = calc_edit_distance(ctx, sx, ex,
                                          key_name, key_name + key_length, flags);
            if (distance <= max_distance) {
              score_heap_push(ctx, heap, record_id, distance);
              break;
            }
          }
        }
      } else {
        if (grn_obj_is_reference_column(ctx, column)) {
          grn_id rid;
          char key_name[GRN_TABLE_MAX_KEY_SIZE];
          int key_length;
          rid = GRN_RECORD_VALUE(&value);
          key_length = grn_table_get_key(ctx, domain, rid, key_name, GRN_TABLE_MAX_KEY_SIZE);
          if (!prefix_match_size ||
              (prefix_match_size > 0 && key_length >= (int) prefix_match_size &&
               !memcmp(sx, key_name, prefix_match_size))) {
            distance = calc_edit_distance(ctx, sx, ex,
                                          key_name, key_name + key_length, flags);
            if (distance <= max_distance) {
              score_heap_push(ctx, heap, record_id, distance);
            }
          }
        } else {
          if (!prefix_match_size ||
              (prefix_match_size > 0 && GRN_TEXT_LEN(&value) >= prefix_match_size &&
               !memcmp(sx, GRN_TEXT_VALUE(&value), prefix_match_size))) {
            distance = calc_edit_distance(ctx, sx, ex,
                                          GRN_TEXT_VALUE(&value),
                                          GRN_BULK_CURR(&value), flags);
            if (distance <= max_distance) {
              score_heap_push(ctx, heap, record_id, distance);
            }
          }
        }
      }
      grn_obj_unlink(ctx, domain);
    }
    grn_table_cursor_close(ctx, tc);
    grn_obj_unlink(ctx, &value);

    for (i = 0; i < heap->n_entries; i++) {
      if (max_expansion > 0 && (uint32_t) i >= max_expansion) {
        break;
      }
      {
        grn_posting posting;
        posting.rid = heap->nodes[i].id;
        posting.sid = 1;
        posting.pos = 0;
        posting.weight = max_distance - heap->nodes[i].score;
        grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
      }
    }
    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
    score_heap_close(ctx, heap);
  }

  return GRN_SUCCESS;
}

static grn_rc
selector_fuzzy_search(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                      int nargs, grn_obj **args,
                      grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *target = NULL;
  grn_obj *obj;
  grn_obj *query;
  uint32_t max_distance = 1;
  uint32_t prefix_length = 0;
  uint32_t prefix_match_size = 0;
  uint32_t max_expansion = 0;
  int flags = 0;
  grn_bool use_sequential_search = GRN_FALSE;

  if ((nargs - 1) < 2) {
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "fuzzy_search(): wrong number of arguments (%d ...)",
                     nargs - 1);
    rc = ctx->rc;
    goto exit;
  }
  obj = args[1];
  query = args[2];

  if (nargs == 4) {
    grn_obj *options = args[3];

    switch (options->header.type) {
    case GRN_BULK :
      max_distance = GRN_UINT32_VALUE(options);
      break;
    case GRN_TABLE_HASH_KEY :
      {
        grn_hash_cursor *cursor;
        void *key;
        grn_obj *value;
        int key_size;
        cursor = grn_hash_cursor_open(ctx, (grn_hash *)options,
                                      NULL, 0, NULL, 0,
                                      0, -1, 0);
        if (!cursor) {
          GRN_PLUGIN_ERROR(ctx, GRN_NO_MEMORY_AVAILABLE,
                           "fuzzy_search(): couldn't open cursor");
          goto exit;
        }
        while (grn_hash_cursor_next(ctx, cursor) != GRN_ID_NIL) {
          grn_hash_cursor_get_key_value(ctx, cursor, &key, &key_size,
                                        (void **)&value);

          if (key_size == 12 && !memcmp(key, "max_distance", 12)) {
            max_distance = GRN_UINT32_VALUE(value);
          } else if (key_size == 13 && !memcmp(key, "prefix_length", 13)) {
            prefix_length = GRN_UINT32_VALUE(value);
          } else if (key_size == 13 && !memcmp(key, "max_expansion", 13)) {
            max_expansion = GRN_UINT32_VALUE(value);
          } else if (key_size == 18 && !memcmp(key, "with_transposition", 18)) {
            if (GRN_BOOL_VALUE(value)) {
              flags |= GRN_TABLE_FUZZY_SEARCH_WITH_TRANSPOSITION;
            }
          } else {
            GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                             "invalid option name: <%.*s>",
                             key_size, (char *)key);
            grn_hash_cursor_close(ctx, cursor);
            goto exit;
          }
        }
        grn_hash_cursor_close(ctx, cursor);
      }
      break;
    default :
      GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                       "fuzzy_search(): "
                       "3rd argument must be integer or object literal: <%.*s>",
                       (int)GRN_TEXT_LEN(options),
                       GRN_TEXT_VALUE(options));
      goto exit;
    }
  }

  if (index) {
    target = index;
  } else {
    if (obj->header.type == GRN_COLUMN_INDEX) {
      target = obj;
    } else {
      grn_column_index(ctx, obj, GRN_OP_FUZZY, &target, 1, NULL);
    }
  }

  if (target) {
    grn_obj *lexicon;
    use_sequential_search = GRN_TRUE;
    lexicon = grn_ctx_at(ctx, target->header.domain);
    if (lexicon) {
      if (lexicon->header.type == GRN_TABLE_PAT_KEY) {
        use_sequential_search = GRN_FALSE;
      }
      grn_obj_unlink(ctx, lexicon);
    }
  } else {
    if (grn_obj_is_key_accessor(ctx, obj) &&
        table->header.type == GRN_TABLE_PAT_KEY) {
      target = table;
    } else {
      use_sequential_search = GRN_TRUE;
    }
  }

  if (prefix_length) {
    const char *s = GRN_TEXT_VALUE(query);
    const char *e = GRN_BULK_CURR(query);
    const char *p;
    unsigned int cl = 0;
    unsigned int length = 0;
    for (p = s; p < e && (cl = grn_charlen(ctx, p, e)); p += cl) {
      length++;
      if (length > prefix_length) {
        break;
      }
    }
    prefix_match_size = p - s;
  }

  if (use_sequential_search) {
    rc = sequential_fuzzy_search(ctx, table, obj, query,
                                 max_distance, prefix_match_size,
                                 max_expansion, flags, res, op);
    goto exit;
  }

  if (!target) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, target);
    GRN_PLUGIN_ERROR(ctx, GRN_INVALID_ARGUMENT,
                     "fuzzy_search(): "
                     "column must be COLUMN_INDEX or TABLE_PAT_KEY: <%.*s>",
                     (int)GRN_TEXT_LEN(&inspected),
                     GRN_TEXT_VALUE(&inspected));
    rc = ctx->rc;
    GRN_OBJ_FIN(ctx, &inspected);
  } else {
    grn_search_optarg options = {0};
    options.mode = GRN_OP_FUZZY;
    options.fuzzy.prefix_match_size = prefix_match_size;
    options.fuzzy.max_distance = max_distance;
    options.fuzzy.max_expansion = max_expansion;
    options.fuzzy.flags = flags;
    grn_obj_search(ctx, target, query, res, op, &options);
  }

exit :
  return rc;
}

void
grn_proc_init_fuzzy_search(grn_ctx *ctx)
{
  grn_obj *selector_proc;

  selector_proc = grn_proc_create(ctx, "fuzzy_search", -1,
                                  GRN_PROC_FUNCTION,
                                  NULL, NULL, NULL, 0, NULL);
  grn_proc_set_selector(ctx, selector_proc, selector_fuzzy_search);
  grn_proc_set_selector_operator(ctx, selector_proc, GRN_OP_FUZZY);
}
