/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2015 Brazil

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
#ifndef GRN_DB_H
#define GRN_DB_H

#include "grn.h"
#include "grn_ctx.h"
#include "grn_store.h"
#include "grn_rset.h"

#include <groonga/command.h>
#include <groonga/token_filter.h>
#include <groonga/scorer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRN_DB_DELIMITER            '.'
#define GRN_DB_PSEUDO_COLUMN_PREFIX '_'

#define GRN_N_RESERVED_TYPES 256

#define GRN_JSON_LOAD_OPEN_BRACKET 0x40000000
#define GRN_JSON_LOAD_OPEN_BRACE   0x40000001

typedef struct _grn_db grn_db;
typedef struct _grn_proc grn_proc;

struct _grn_db {
  grn_db_obj obj;
  grn_obj *keys;
  grn_ja *specs;
  grn_tiny_array values;
  grn_critical_section lock;
};

typedef struct {
  grn_obj_header header;
  grn_id range;
} grn_obj_spec;

GRN_API grn_rc grn_db_close(grn_ctx *ctx, grn_obj *db);

grn_obj *grn_db_keys(grn_obj *s);

uint32_t grn_db_lastmod(grn_obj *s);

grn_rc _grn_table_delete_by_id(grn_ctx *ctx, grn_obj *table, grn_id id,
                               grn_table_delete_optarg *optarg);

grn_id grn_table_get_v(grn_ctx *ctx, grn_obj *table, const void *key, int key_size,
                       void **value);
grn_id grn_table_add_v(grn_ctx *ctx, grn_obj *table, const void *key, int key_size,
                       void **value, int *added);
GRN_API grn_rc grn_table_get_info(grn_ctx *ctx, grn_obj *table, grn_obj_flags *flags,
                                  grn_encoding *encoding, grn_obj **tokenizer,
                                  grn_obj **normalizer,
                                  grn_obj **token_filters);
const char *_grn_table_key(grn_ctx *ctx, grn_obj *table, grn_id id, uint32_t *key_size);

grn_rc grn_table_search(grn_ctx *ctx, grn_obj *table,
                        const void *key, uint32_t key_size,
                        grn_operator mode, grn_obj *res, grn_operator op);

grn_id grn_table_next(grn_ctx *ctx, grn_obj *table, grn_id id);

int grn_table_get_key2(grn_ctx *ctx, grn_obj *table, grn_id id, grn_obj *bulk);

grn_table_cursor *grn_table_cursor_open_by_id(grn_ctx *ctx, grn_obj *table,
                                              grn_id min, grn_id max, int flags);

void grn_table_add_subrec(grn_obj *table, grn_rset_recinfo *ri, double score,
                          grn_rset_posinfo *pi, int dir);

grn_obj *grn_obj_graft(grn_ctx *ctx, grn_obj *obj);

grn_rc grn_column_name_(grn_ctx *ctx, grn_obj *obj, grn_obj *buf);


typedef enum {
  PROC_INIT = 0,
  PROC_NEXT,
  PROC_FIN
} grn_proc_phase;

struct _grn_type {
  grn_db_obj obj;
};

#define GRN_TYPE_SIZE(type) ((type)->range)

#define GRN_TABLE_SORT_GEO            (0x02<<0)

#define GRN_OBJ_TMP_OBJECT 0x80000000

#define GRN_DB_OBJP(obj) \
  (obj &&\
   ((GRN_SNIP == ((grn_db_obj *)obj)->header.type) ||\
    ((GRN_CURSOR_TABLE_HASH_KEY <= ((grn_db_obj *)obj)->header.type) &&\
     (((grn_db_obj *)obj)->header.type <= GRN_COLUMN_INDEX))))

#define GRN_OBJ_TABLEP(obj) \
  (obj &&\
   (GRN_TABLE_HASH_KEY <= ((grn_db_obj *)obj)->header.type) &&\
   (((grn_db_obj *)obj)->header.type <= GRN_DB))

#define GRN_OBJ_INDEX_COLUMNP(obj) \
  (obj &&\
   DB_OBJ(obj)->header.type == GRN_COLUMN_INDEX)

#define GRN_OBJ_VECTOR_COLUMNP(obj) \
  (obj &&\
   DB_OBJ(obj)->header.type == GRN_COLUMN_VAR_SIZE &&\
   (DB_OBJ(obj)->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_VECTOR)

#define GRN_OBJ_WEIGHT_VECTOR_COLUMNP(obj) \
  (GRN_OBJ_VECTOR_COLUMNP(obj) &&\
   (DB_OBJ(obj)->header.flags & GRN_OBJ_WITH_WEIGHT))

typedef struct _grn_proc_ctx grn_proc_ctx;

struct _grn_proc_ctx {
  grn_user_data user_data;
  grn_proc *proc;
  grn_obj *caller;
  //  grn_obj *obj;
  grn_hook *hooks;
  grn_hook *currh;
  grn_proc_phase phase;
  unsigned short nargs;
  unsigned short offset;
  grn_user_data data[16];
};

struct _grn_proc {
  grn_db_obj obj;
  grn_obj name_buf;
  grn_expr_var *vars;
  uint32_t nvars;
  /* -- compatible with grn_expr -- */
  grn_proc_type type;
  grn_proc_func *funcs[3];

  grn_selector_func *selector;

  union {
    struct {
      grn_command_run_func *run;
    } command;
    struct {
      grn_token_filter_init_func   *init;
      grn_token_filter_filter_func *filter;
      grn_token_filter_fin_func    *fin;
    } token_filter;
    struct {
      grn_scorer_score_func *score;
    } scorer;
  } callbacks;

  void *user_data;

  grn_id module;
  //  uint32_t nargs;
  //  uint32_t nresults;
  //  grn_obj results[16];
};

#define GRN_PROC_GET_VARS() (grn_proc_get_vars(ctx, user_data))
#define GRN_PROC_GET_VAR(name) (grn_proc_get_var(ctx, user_data, name, strlen(name)))
#define GRN_PROC_GET_VAR_BY_OFFSET(offset) (grn_proc_get_var_by_offset(ctx, user_data, offset))
#define GRN_PROC_GET_OR_ADD_VAR(name) (grn_proc_get_or_add_var(ctx, user_data, name, strlen(name)))
#define GRN_PROC_ALLOC(domain, flags) (grn_proc_alloc(ctx, user_data, domain, flags))

grn_obj *grn_proc_get_vars(grn_ctx *ctx, grn_user_data *user_data);

grn_obj *grn_proc_get_var(grn_ctx *ctx, grn_user_data *user_data,
                          const char *name, unsigned int name_size);

GRN_API grn_obj *grn_proc_get_var_by_offset(grn_ctx *ctx, grn_user_data *user_data,
                                            unsigned int offset);
GRN_API grn_obj *grn_proc_get_or_add_var(grn_ctx *ctx, grn_user_data *user_data,
                                         const char *name, unsigned int name_size);

GRN_API grn_obj *grn_proc_alloc(grn_ctx *ctx, grn_user_data *user_data,
                                grn_id domain, grn_obj_flags flags);

GRN_API grn_rc grn_proc_call(grn_ctx *ctx, grn_obj *proc,
                             int nargs, grn_obj *caller);

grn_obj *grn_expr_get_or_add_var(grn_ctx *ctx, grn_obj *expr,
                                 const char *name, unsigned int name_size);


typedef struct _grn_accessor grn_accessor;

struct _grn_accessor {
  grn_obj_header header;
  grn_id range;
  /* -- compatible with grn_db_obj -- */
  uint8_t action;
  int offset;
  grn_obj *obj;
  grn_accessor *next;
};

enum {
  GRN_ACCESSOR_VOID = 0,
  GRN_ACCESSOR_GET_ID,
  GRN_ACCESSOR_GET_KEY,
  GRN_ACCESSOR_GET_VALUE,
  GRN_ACCESSOR_GET_SCORE,
  GRN_ACCESSOR_GET_NSUBRECS,
  GRN_ACCESSOR_GET_MAX,
  GRN_ACCESSOR_GET_MIN,
  GRN_ACCESSOR_GET_SUM,
  GRN_ACCESSOR_GET_AVG,
  GRN_ACCESSOR_GET_COLUMN_VALUE,
  GRN_ACCESSOR_GET_DB_OBJ,
  GRN_ACCESSOR_LOOKUP,
  GRN_ACCESSOR_FUNCALL
};

#define DB_OBJ(obj) ((grn_db_obj *)obj)

GRN_API const char *grn_obj_get_value_(grn_ctx *ctx, grn_obj *obj, grn_id id, uint32_t *size);

/* vector */

/*
typedef struct _grn_vector grn_vector;

struct _grn_vector {
  grn_obj str;
  uint32_t *offsets;
  int n_entries;
};

const char *grn_vector_fetch(grn_ctx *ctx, grn_obj *vector, int i, unsigned int *size);
int grn_vector_delimit(grn_ctx *ctx, grn_obj *vector);
int grn_vector_size(grn_ctx *ctx, grn_obj *vector);
*/

unsigned int grn_vector_pop_element(grn_ctx *ctx, grn_obj *vector,
                                    const char **str, unsigned int *weight, grn_id *domain);

grn_rc grn_vector_delimit(grn_ctx *ctx, grn_obj *v, unsigned int weight, grn_id domain);
grn_rc grn_vector_decode(grn_ctx *ctx, grn_obj *v, const char *data, uint32_t data_size);


grn_rc grn_db_init_builtin_types(grn_ctx *ctx);

/* flag value used for grn_obj.header.flags */

#define GRN_OBJ_CUSTOM_NAME            (0x01<<12) /* db_obj which has custom name */

#define GRN_OBJ_RESOLVE(ctx,obj) \
  (((obj)->header.type != GRN_PTR)\
   ? (obj)\
   : GRN_PTR_VALUE(obj)\
      ? GRN_PTR_VALUE(obj)\
      : grn_ctx_at((ctx), (obj)->header.domain))

/* expr */

typedef struct _grn_expr grn_expr;

#define GRN_EXPR_CODE_RELATIONAL_EXPRESSION (0x01)

typedef struct {
  grn_obj *value;
  int32_t nargs;
  grn_operator op;
  uint8_t flags;
  int32_t modify;
} grn_expr_code;

struct _grn_expr {
  grn_db_obj obj;
  grn_obj name_buf;
  grn_expr_var *vars;
  uint32_t nvars;
  /* -- compatible with grn_proc -- */

  uint16_t cacheable;
  uint16_t taintable;
  grn_obj *consts;
  grn_obj *values;
  grn_expr_code *codes;
  uint32_t nconsts;
  uint32_t values_curr;
  uint32_t values_tail;
  uint32_t values_size;
  uint32_t codes_curr;
  uint32_t codes_size;

  grn_obj objs;
  grn_obj dfi;
  grn_expr_code *code0;
};

grn_rc grn_expr_parser_close(grn_ctx *ctx);
GRN_API grn_rc grn_obj_cast(grn_ctx *ctx, grn_obj *src, grn_obj *dest, grn_bool addp);

/**
 * grn_table_open:
 * @name: 開こうとするtableの名前。NULLなら無名tableとなる。
 * @path: 開こうとするtableのファイルパス。
 *
 * ctxが使用するdbの中でnameに対応付けて既存のtableを開く。
 * dbに登録されている名前付きの永続テーブルを開く場合はgrn_ctx_get()を使用するのが望ましい。
 **/
GRN_API grn_obj *grn_table_open(grn_ctx *ctx,
                                const char *name, unsigned int name_size,
                                const char *path);

/**
 * grn_column_open:
 * @table: 対象table
 * @name: カラム名
 * @path: カラムを格納するファイルパス。
 * @type: カラム値の型。
 *
 * 既存の永続的なcolumnを、tableのnameに対応するcolumnとして開く
 * 永続dbに登録されている永続テーブルのカラムを開く場合はgrn_ctx_get()を使用するのが望ましい。
 **/
grn_obj *grn_column_open(grn_ctx *ctx, grn_obj *table,
                         const char *name, unsigned int name_size,
                         const char *path, grn_obj *type);

/**
 * grn_obj_path_rename:
 * @old_path: 旧ファイルパス
 * @new_path: 新ファイルパス
 *
 * old_pathに該当するオブジェクトのファイル名をnew_pathに変更する。
 **/
grn_rc grn_obj_path_rename(grn_ctx *ctx, const char *old_path, const char *new_path);

grn_rc grn_db_check_name(grn_ctx *ctx, const char *name, unsigned int name_size);
#define GRN_DB_CHECK_NAME_ERR(error_context, name, name_size) \
  ERR(GRN_INVALID_ARGUMENT,\
       "%s name can't start with '%c' and contains only 0-9, A-Z, a-z, #, @, - or _: <%.*s>",\
      error_context, GRN_DB_PSEUDO_COLUMN_PREFIX, name_size, name)

#define GRN_DB_P(s) ((s) && ((grn_db *)s)->obj.header.type == GRN_DB)
#define GRN_DB_PERSISTENT_P(s) (((grn_db *)s)->specs)

#define GRN_OBJ_GET_VALUE_IMD (0xffffffffU)

grn_rc grn_db_obj_init(grn_ctx *ctx, grn_obj *db, grn_id id, grn_db_obj *obj);

#define GRN_ACCESSORP(obj) \
  ((obj) && (((grn_obj *)(obj))->header.type == GRN_ACCESSOR))

#define GRN_TRUEP(ctx, v, result) do {\
  switch (v->header.type) {                             \
  case GRN_BULK :                                       \
    switch (v->header.domain) {                         \
    case GRN_DB_BOOL :                                  \
      result = GRN_BOOL_VALUE(v);                       \
      break;                                            \
    case GRN_DB_INT32 :                                 \
      result = GRN_INT32_VALUE(v) != 0;                 \
      break;                                            \
    case GRN_DB_UINT32 :                                \
      result = GRN_UINT32_VALUE(v) != 0;                \
      break;                                            \
    case GRN_DB_FLOAT :                                 \
      {                                                 \
        double float_value;                             \
        float_value = GRN_FLOAT_VALUE(v);               \
        result = (float_value < -DBL_EPSILON ||         \
                  DBL_EPSILON < float_value);           \
      }                                                 \
      break;                                            \
    case GRN_DB_SHORT_TEXT :                            \
    case GRN_DB_TEXT :                                  \
    case GRN_DB_LONG_TEXT :                             \
      result = GRN_TEXT_LEN(v) != 0;                    \
      break;                                            \
    default :                                           \
      result = GRN_FALSE;                               \
      break;                                            \
    }                                                   \
    break;                                              \
  case GRN_VECTOR :                                     \
    result = GRN_TRUE;                                  \
    break;                                              \
  default :                                             \
    result = GRN_FALSE;                                 \
    break;                                              \
  }                                                     \
} while (0)

grn_id grn_obj_register(grn_ctx *ctx, grn_obj *db, const char *name, unsigned int name_size);
int grn_obj_is_persistent(grn_ctx *ctx, grn_obj *obj);
void grn_obj_spec_save(grn_ctx *ctx, grn_db_obj *obj);

grn_rc grn_obj_reinit_for(grn_ctx *ctx, grn_obj *obj, grn_obj *domain_obj);

#define GRN_INT32_POP(obj,value) do {\
  if (GRN_BULK_VSIZE(obj) >= sizeof(int32_t)) {\
    GRN_BULK_INCR_LEN((obj), -(sizeof(int32_t)));\
    value = *(int32_t *)(GRN_BULK_CURR(obj));\
  } else {\
    value = 0;\
  }\
} while (0)

#define GRN_UINT32_POP(obj,value) do {\
  if (GRN_BULK_VSIZE(obj) >= sizeof(uint32_t)) {\
    GRN_BULK_INCR_LEN((obj), -(sizeof(uint32_t)));\
    value = *(uint32_t *)(GRN_BULK_CURR(obj));\
  } else {\
    value = 0;\
  }\
} while (0)

void grn_expr_pack(grn_ctx *ctx, grn_obj *buf, grn_obj *expr);
GRN_API grn_rc grn_expr_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *expr);
grn_hash *grn_expr_get_vars(grn_ctx *ctx, grn_obj *expr, unsigned int *nvars);
grn_obj *grn_expr_open(grn_ctx *ctx, grn_obj_spec *spec, const uint8_t *p, const uint8_t *pe);

GRN_API void grn_load_(grn_ctx *ctx, grn_content_type input_type,
                       const char *table, unsigned int table_len,
                       const char *columns, unsigned int columns_len,
                       const char *values, unsigned int values_len,
                       const char *ifexists, unsigned int ifexists_len,
                       const char *each, unsigned int each_len,
                       uint32_t emit_level);

GRN_API grn_rc grn_table_group_with_range_gap(grn_ctx *ctx, grn_obj *table,
                                              grn_table_sort_key *group_key,
                                              grn_obj *result_set,
                                              uint32_t range_gap);

GRN_API grn_rc grn_column_filter(grn_ctx *ctx, grn_obj *column,
                                 grn_operator op,
                                 grn_obj *value, grn_obj *result_set,
                                 grn_operator set_op);

grn_rc grn_accessor_resolve(grn_ctx *ctx, grn_obj *accessor, int deep,
                            grn_obj *base_res, grn_obj **res,
                            grn_search_optarg *optarg);

#ifdef __cplusplus
}
#endif

#endif /* GRN_DB_H */
