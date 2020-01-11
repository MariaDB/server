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

typedef struct _grn_obj_format grn_obj_format;

#define GRN_OBJ_FORMAT_WITH_COLUMN_NAMES   (0x01<<0)
#define GRN_OBJ_FORMAT_AS_ARRAY            (0x01<<3)
/* Deprecated since 4.0.1. It will be removed at 5.0.0.
   Use GRN_OBJ_FORMAT_AS_ARRAY instead.*/
#define GRN_OBJ_FORMAT_ASARRAY             GRN_OBJ_FORMAT_AS_ARRAY
#define GRN_OBJ_FORMAT_WITH_WEIGHT         (0x01<<4)

struct _grn_obj_format {
  grn_obj columns;
  const void *min;
  const void *max;
  unsigned int min_size;
  unsigned int max_size;
  int nhits;
  int offset;
  int limit;
  int hits_offset;
  int flags;
  grn_obj *expression;
};

#define GRN_OBJ_FORMAT_INIT(format,format_nhits,format_offset,format_limit,format_hits_offset) do { \
  GRN_PTR_INIT(&(format)->columns, GRN_OBJ_VECTOR, GRN_ID_NIL);\
  (format)->nhits = (format_nhits);\
  (format)->offset = (format_offset);\
  (format)->limit = (format_limit);\
  (format)->hits_offset = (format_hits_offset);\
  (format)->flags = 0;\
  (format)->expression = NULL;\
} while (0)

#define GRN_OBJ_FORMAT_FIN(ctx,format) do {\
  int ncolumns = GRN_BULK_VSIZE(&(format)->columns) / sizeof(grn_obj *);\
  grn_obj **columns = (grn_obj **)GRN_BULK_HEAD(&(format)->columns);\
  while (ncolumns--) {\
    grn_obj *column = *columns;\
    columns++;\
    if (grn_obj_is_accessor((ctx), column)) {\
      grn_obj_close((ctx), column);\
    }\
  }\
  GRN_OBJ_FIN((ctx), &(format)->columns);\
  if ((format)->expression) { GRN_OBJ_FIN((ctx), (format)->expression); } \
} while (0)

GRN_API void grn_output_obj(grn_ctx *ctx, grn_obj *outbuf, grn_content_type output_type,
                            grn_obj *obj, grn_obj_format *format);
GRN_API void grn_output_envelope(grn_ctx *ctx, grn_rc rc,
                                 grn_obj *head, grn_obj *body, grn_obj *foot,
                                 const char *file, int line);

GRN_API void grn_ctx_output_flush(grn_ctx *ctx, int flags);
GRN_API void grn_ctx_output_array_open(grn_ctx *ctx,
                                       const char *name, int nelements);
GRN_API void grn_ctx_output_array_close(grn_ctx *ctx);
GRN_API void grn_ctx_output_map_open(grn_ctx *ctx,
                                     const char *name, int nelements);
GRN_API void grn_ctx_output_map_close(grn_ctx *ctx);
GRN_API void grn_ctx_output_null(grn_ctx *ctx);
GRN_API void grn_ctx_output_int32(grn_ctx *ctx, int value);
GRN_API void grn_ctx_output_int64(grn_ctx *ctx, int64_t value);
GRN_API void grn_ctx_output_uint64(grn_ctx *ctx, uint64_t value);
GRN_API void grn_ctx_output_float(grn_ctx *ctx, double value);
GRN_API void grn_ctx_output_cstr(grn_ctx *ctx, const char *value);
GRN_API void grn_ctx_output_str(grn_ctx *ctx,
                                const char *value, unsigned int value_len);
GRN_API void grn_ctx_output_bool(grn_ctx *ctx, grn_bool value);
GRN_API void grn_ctx_output_obj(grn_ctx *ctx,
                                grn_obj *value, grn_obj_format *format);
GRN_API void grn_ctx_output_result_set_open(grn_ctx *ctx,
                                            grn_obj *result_set,
                                            grn_obj_format *format,
                                            uint32_t n_additional_elements);
GRN_API void grn_ctx_output_result_set_close(grn_ctx *ctx,
                                             grn_obj *result_set,
                                             grn_obj_format *format);
GRN_API void grn_ctx_output_result_set(grn_ctx *ctx,
                                       grn_obj *result_set,
                                       grn_obj_format *format);
GRN_API void grn_ctx_output_table_columns(grn_ctx *ctx,
                                          grn_obj *table,
                                          grn_obj_format *format);
GRN_API void grn_ctx_output_table_records(grn_ctx *ctx,
                                          grn_obj *table,
                                          grn_obj_format *format);


GRN_API grn_content_type grn_ctx_get_output_type(grn_ctx *ctx);
GRN_API grn_rc grn_ctx_set_output_type(grn_ctx *ctx, grn_content_type type);
GRN_API const char *grn_ctx_get_mime_type(grn_ctx *ctx);

/* obsolete */
GRN_API grn_rc grn_text_otoj(grn_ctx *ctx, grn_obj *bulk, grn_obj *obj,
                             grn_obj_format *format);

#ifdef __cplusplus
}
#endif
