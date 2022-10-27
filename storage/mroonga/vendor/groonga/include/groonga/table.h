/*
  Copyright(C) 2009-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef  __cplusplus
extern "C" {
#endif

#define GRN_TABLE_MAX_KEY_SIZE         (0x1000)

GRN_API grn_obj *grn_table_create(grn_ctx *ctx,
                                  const char *name, unsigned int name_size,
                                  const char *path, grn_table_flags flags,
                                  grn_obj *key_type, grn_obj *value_type);

#define GRN_TABLE_OPEN_OR_CREATE(ctx,name,name_size,path,flags,key_type,value_type,table) \
  (((table) = grn_ctx_get((ctx), (name), (name_size))) ||\
   ((table) = grn_table_create((ctx), (name), (name_size), (path), (flags), (key_type), (value_type))))

/* TODO: int *added -> grn_bool *added */
GRN_API grn_id grn_table_add(grn_ctx *ctx, grn_obj *table,
                             const void *key, unsigned int key_size, int *added);
GRN_API grn_id grn_table_get(grn_ctx *ctx, grn_obj *table,
                             const void *key, unsigned int key_size);
GRN_API grn_id grn_table_at(grn_ctx *ctx, grn_obj *table, grn_id id);
GRN_API grn_id grn_table_lcp_search(grn_ctx *ctx, grn_obj *table,
                                    const void *key, unsigned int key_size);
GRN_API int grn_table_get_key(grn_ctx *ctx, grn_obj *table,
                              grn_id id, void *keybuf, int buf_size);
GRN_API grn_rc grn_table_delete(grn_ctx *ctx, grn_obj *table,
                                const void *key, unsigned int key_size);
GRN_API grn_rc grn_table_delete_by_id(grn_ctx *ctx, grn_obj *table, grn_id id);
GRN_API grn_rc grn_table_update_by_id(grn_ctx *ctx, grn_obj *table, grn_id id,
                                      const void *dest_key, unsigned int dest_key_size);
GRN_API grn_rc grn_table_update(grn_ctx *ctx, grn_obj *table,
                                const void *src_key, unsigned int src_key_size,
                                const void *dest_key, unsigned int dest_key_size);
GRN_API grn_rc grn_table_truncate(grn_ctx *ctx, grn_obj *table);

#define GRN_CURSOR_ASCENDING           (0x00<<0)
#define GRN_CURSOR_DESCENDING          (0x01<<0)
#define GRN_CURSOR_GE                  (0x00<<1)
#define GRN_CURSOR_GT                  (0x01<<1)
#define GRN_CURSOR_LE                  (0x00<<2)
#define GRN_CURSOR_LT                  (0x01<<2)
#define GRN_CURSOR_BY_KEY              (0x00<<3)
#define GRN_CURSOR_BY_ID               (0x01<<3)
#define GRN_CURSOR_PREFIX              (0x01<<4)
#define GRN_CURSOR_SIZE_BY_BIT         (0x01<<5)
#define GRN_CURSOR_RK                  (0x01<<6)

GRN_API grn_table_cursor *grn_table_cursor_open(grn_ctx *ctx, grn_obj *table,
                                                const void *min, unsigned int min_size,
                                                const void *max, unsigned int max_size,
                                                int offset, int limit, int flags);
GRN_API grn_rc grn_table_cursor_close(grn_ctx *ctx, grn_table_cursor *tc);
GRN_API grn_id grn_table_cursor_next(grn_ctx *ctx, grn_table_cursor *tc);
GRN_API int grn_table_cursor_get_key(grn_ctx *ctx, grn_table_cursor *tc, void **key);
GRN_API int grn_table_cursor_get_value(grn_ctx *ctx, grn_table_cursor *tc, void **value);
GRN_API grn_rc grn_table_cursor_set_value(grn_ctx *ctx, grn_table_cursor *tc,
                                          const void *value, int flags);
GRN_API grn_rc grn_table_cursor_delete(grn_ctx *ctx, grn_table_cursor *tc);
GRN_API grn_obj *grn_table_cursor_table(grn_ctx *ctx, grn_table_cursor *tc);

GRN_API grn_obj *grn_index_cursor_open(grn_ctx *ctx, grn_table_cursor *tc, grn_obj *index,
                                       grn_id rid_min, grn_id rid_max, int flags);
GRN_API grn_posting *grn_index_cursor_next(grn_ctx *ctx, grn_obj *ic, grn_id *tid);

#define GRN_TABLE_EACH(ctx,table,head,tail,id,key,key_size,value,block) do {\
  (ctx)->errlvl = GRN_LOG_NOTICE;\
  (ctx)->rc = GRN_SUCCESS;\
  if ((ctx)->seqno & 1) {\
    (ctx)->subno++;\
  } else {\
    (ctx)->seqno++;\
  }\
  if (table) {\
    switch ((table)->header.type) {\
    case GRN_TABLE_PAT_KEY :\
      GRN_PAT_EACH((ctx), (grn_pat *)(table), (id), (key), (key_size), (value), block);\
      break;\
    case GRN_TABLE_DAT_KEY :\
      GRN_DAT_EACH((ctx), (grn_dat *)(table), (id), (key), (key_size), block);\
      break;\
    case GRN_TABLE_HASH_KEY :\
      GRN_HASH_EACH((ctx), (grn_hash *)(table), (id), (key), (key_size), (value), block);\
      break;\
    case GRN_TABLE_NO_KEY :\
      GRN_ARRAY_EACH((ctx), (grn_array *)(table), (head), (tail), (id), (value), block);\
      break;\
    }\
  }\
  if ((ctx)->subno) {\
    (ctx)->subno--;\
  } else {\
    (ctx)->seqno++;\
  }\
} while (0)

#define GRN_TABLE_EACH_BEGIN(ctx, table, cursor, id) do {\
  if ((table)) {\
    grn_table_cursor *cursor;\
    cursor = grn_table_cursor_open((ctx), (table),\
                                   NULL, 0,\
                                   NULL, 0,\
                                   0, -1, GRN_CURSOR_ASCENDING);\
    if (cursor) {\
      grn_id id;\
      while ((id = grn_table_cursor_next((ctx), cursor))) {

#define GRN_TABLE_EACH_BEGIN_FLAGS(ctx, table, cursor, id, flags) do {\
  if ((table)) {\
    grn_table_cursor *cursor;\
    cursor = grn_table_cursor_open((ctx), (table),\
                                   NULL, 0,\
                                   NULL, 0,\
                                   0, -1, (flags));\
    if (cursor) {\
      grn_id id;\
      while ((id = grn_table_cursor_next((ctx), cursor))) {

#define GRN_TABLE_EACH_BEGIN_MIN(ctx, table, cursor, id,\
                                 min, min_size, flags) do {\
  if ((table)) {\
    grn_table_cursor *cursor;\
    cursor = grn_table_cursor_open((ctx), (table),\
                                   (min), (min_size),\
                                   NULL, 0,\
                                   0, -1, (flags));\
    if (cursor) {\
      grn_id id;\
      while ((id = grn_table_cursor_next((ctx), cursor))) {

#define GRN_TABLE_EACH_END(ctx, cursor)\
      }\
      grn_table_cursor_close((ctx), cursor);\
    }\
  }\
} while (0)

typedef struct _grn_table_sort_key grn_table_sort_key;
typedef unsigned char grn_table_sort_flags;

#define GRN_TABLE_SORT_ASC             (0x00<<0)
#define GRN_TABLE_SORT_DESC            (0x01<<0)

struct _grn_table_sort_key {
  grn_obj *key;
  grn_table_sort_flags flags;
  int offset;
};

GRN_API int grn_table_sort(grn_ctx *ctx, grn_obj *table, int offset, int limit,
                           grn_obj *result, grn_table_sort_key *keys, int n_keys);

typedef struct _grn_table_group_result grn_table_group_result;
typedef uint32_t grn_table_group_flags;

#define GRN_TABLE_GROUP_CALC_COUNT     (0x01<<3)
#define GRN_TABLE_GROUP_CALC_MAX       (0x01<<4)
#define GRN_TABLE_GROUP_CALC_MIN       (0x01<<5)
#define GRN_TABLE_GROUP_CALC_SUM       (0x01<<6)
#define GRN_TABLE_GROUP_CALC_AVG       (0x01<<7)

struct _grn_table_group_result {
  grn_obj *table;
  unsigned char key_begin;
  unsigned char key_end;
  int limit;
  grn_table_group_flags flags;
  grn_operator op;
  unsigned int max_n_subrecs;
  grn_obj *calc_target;
};

GRN_API grn_rc grn_table_group(grn_ctx *ctx, grn_obj *table,
                               grn_table_sort_key *keys, int n_keys,
                               grn_table_group_result *results, int n_results);
GRN_API grn_rc grn_table_setoperation(grn_ctx *ctx, grn_obj *table1, grn_obj *table2,
                                      grn_obj *res, grn_operator op);
GRN_API grn_rc grn_table_difference(grn_ctx *ctx, grn_obj *table1, grn_obj *table2,
                                    grn_obj *res1, grn_obj *res2);
GRN_API int grn_table_columns(grn_ctx *ctx, grn_obj *table,
                              const char *name, unsigned int name_size,
                              grn_obj *res);

GRN_API unsigned int grn_table_size(grn_ctx *ctx, grn_obj *table);

GRN_API grn_rc grn_table_rename(grn_ctx *ctx, grn_obj *table,
                                const char *name, unsigned int name_size);

GRN_API grn_obj *grn_table_select(grn_ctx *ctx, grn_obj *table, grn_obj *expr,
                                  grn_obj *res, grn_operator op);

GRN_API grn_table_sort_key *grn_table_sort_key_from_str(grn_ctx *ctx,
                                                        const char *str, unsigned int str_size,
                                                        grn_obj *table, unsigned int *nkeys);
GRN_API grn_rc grn_table_sort_key_close(grn_ctx *ctx,
                                        grn_table_sort_key *keys, unsigned int nkeys);

GRN_API grn_bool grn_table_is_grouped(grn_ctx *ctx, grn_obj *table);

GRN_API unsigned int grn_table_max_n_subrecs(grn_ctx *ctx, grn_obj *table);

GRN_API grn_obj *grn_table_create_for_group(grn_ctx *ctx,
                                            const char *name,
                                            unsigned int name_size,
                                            const char *path,
                                            grn_obj *group_key,
                                            grn_obj *value_type,
                                            unsigned int max_n_subrecs);

GRN_API unsigned int grn_table_get_subrecs(grn_ctx *ctx, grn_obj *table,
                                           grn_id id, grn_id *subrecbuf,
                                           int *scorebuf, int buf_size);

GRN_API grn_obj *grn_table_tokenize(grn_ctx *ctx, grn_obj *table,
                                    const char *str, unsigned int str_len,
                                    grn_obj *buf, grn_bool addp);

GRN_API grn_rc grn_table_apply_expr(grn_ctx *ctx,
                                    grn_obj *table,
                                    grn_obj *output_column,
                                    grn_obj *expr);

GRN_API grn_id grn_table_find_reference_object(grn_ctx *ctx, grn_obj *table);

#ifdef __cplusplus
}
#endif
