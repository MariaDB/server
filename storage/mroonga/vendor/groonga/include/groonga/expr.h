/*
  Copyright(C) 2009-2017 Brazil

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

typedef unsigned int grn_expr_flags;

#define GRN_EXPR_SYNTAX_QUERY          (0x00)
#define GRN_EXPR_SYNTAX_SCRIPT         (0x01)
#define GRN_EXPR_SYNTAX_OUTPUT_COLUMNS (0x20)
#define GRN_EXPR_SYNTAX_ADJUSTER       (0x40)
#define GRN_EXPR_ALLOW_PRAGMA          (0x02)
#define GRN_EXPR_ALLOW_COLUMN          (0x04)
#define GRN_EXPR_ALLOW_UPDATE          (0x08)
#define GRN_EXPR_ALLOW_LEADING_NOT     (0x10)
#define GRN_EXPR_QUERY_NO_SYNTAX_ERROR (0x80)

GRN_API grn_obj *grn_expr_create(grn_ctx *ctx, const char *name, unsigned int name_size);
GRN_API grn_rc grn_expr_close(grn_ctx *ctx, grn_obj *expr);
GRN_API grn_obj *grn_expr_add_var(grn_ctx *ctx, grn_obj *expr,
                                  const char *name, unsigned int name_size);
GRN_API grn_obj *grn_expr_get_var(grn_ctx *ctx, grn_obj *expr,
                                  const char *name, unsigned int name_size);
GRN_API grn_obj *grn_expr_get_var_by_offset(grn_ctx *ctx, grn_obj *expr, unsigned int offset);
GRN_API grn_rc grn_expr_clear_vars(grn_ctx *ctx, grn_obj *expr);

GRN_API void grn_expr_take_obj(grn_ctx *ctx, grn_obj *expr, grn_obj *obj);

GRN_API grn_obj *grn_expr_append_obj(grn_ctx *ctx, grn_obj *expr, grn_obj *obj,
                                     grn_operator op, int nargs);
GRN_API grn_obj *grn_expr_append_const(grn_ctx *ctx, grn_obj *expr, grn_obj *obj,
                                       grn_operator op, int nargs);
GRN_API grn_obj *grn_expr_append_const_str(grn_ctx *ctx, grn_obj *expr,
                                           const char *str, unsigned int str_size,
                                           grn_operator op, int nargs);
GRN_API grn_obj *grn_expr_append_const_int(grn_ctx *ctx, grn_obj *expr, int i,
                                           grn_operator op, int nargs);
GRN_API grn_rc grn_expr_append_op(grn_ctx *ctx, grn_obj *expr, grn_operator op, int nargs);

GRN_API grn_rc grn_expr_get_keywords(grn_ctx *ctx, grn_obj *expr, grn_obj *keywords);

GRN_API grn_rc grn_expr_syntax_escape(grn_ctx *ctx,
                                      const char *query, int query_size,
                                      const char *target_characters,
                                      char escape_character,
                                      grn_obj *escaped_query);
GRN_API grn_rc grn_expr_syntax_escape_query(grn_ctx *ctx,
                                            const char *query, int query_size,
                                            grn_obj *escaped_query);
GRN_API grn_rc grn_expr_syntax_expand_query(grn_ctx *ctx,
                                            const char *query, int query_size,
                                            grn_expr_flags flags,
                                            grn_obj *expander,
                                            grn_obj *expanded_query);
GRN_API grn_rc grn_expr_syntax_expand_query_by_table(grn_ctx *ctx,
                                                     const char *query,
                                                     int query_size,
                                                     grn_expr_flags flags,
                                                     grn_obj *term_column,
                                                     grn_obj *expanded_term_column,
                                                     grn_obj *expanded_query);

GRN_API grn_rc grn_expr_compile(grn_ctx *ctx, grn_obj *expr);
GRN_API grn_obj *grn_expr_rewrite(grn_ctx *ctx, grn_obj *expr);
GRN_API grn_rc grn_expr_dump_plan(grn_ctx *ctx, grn_obj *expr, grn_obj *buffer);
GRN_API grn_obj *grn_expr_exec(grn_ctx *ctx, grn_obj *expr, int nargs);

GRN_API grn_obj *grn_expr_alloc(grn_ctx *ctx, grn_obj *expr,
                                grn_id domain, unsigned char flags);

#define GRN_EXPR_CREATE_FOR_QUERY(ctx,table,expr,var) do {\
  if (((expr) = grn_expr_create((ctx), NULL, 0)) &&\
      ((var) = grn_expr_add_var((ctx), (expr), NULL, 0))) {\
    GRN_RECORD_INIT((var), 0, grn_obj_id((ctx), (table)));\
  } else {\
    (var) = NULL;\
  }\
} while (0)

GRN_API grn_rc grn_expr_parse(grn_ctx *ctx, grn_obj *expr,
                              const char *str, unsigned int str_size,
                              grn_obj *default_column, grn_operator default_mode,
                              grn_operator default_op, grn_expr_flags flags);

GRN_API grn_obj *grn_expr_snip(grn_ctx *ctx, grn_obj *expr, int flags,
                               unsigned int width, unsigned int max_results,
                               unsigned int n_tags,
                               const char **opentags, unsigned int *opentag_lens,
                               const char **closetags, unsigned int *closetag_lens,
                               grn_snip_mapping *mapping);
GRN_API grn_rc grn_expr_snip_add_conditions(grn_ctx *ctx,
                                            grn_obj *expr,
                                            grn_obj *snip,
                                            unsigned int n_tags,
                                            const char **opentags,
                                            unsigned int *opentag_lens,
                                            const char **closetags,
                                            unsigned int *closetag_lens);

GRN_API unsigned int grn_expr_estimate_size(grn_ctx *ctx, grn_obj *expr);

#ifdef __cplusplus
}
#endif
