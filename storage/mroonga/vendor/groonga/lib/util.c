/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2017 Brazil

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

#include "grn_db.h"
#include "grn_pat.h"
#include "grn_ii.h"
#include "grn_util.h"
#include "grn_string.h"
#include "grn_expr.h"
#include "grn_load.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef WIN32
# include <io.h>
# include <share.h>
#endif /* WIN32 */

grn_rc
grn_normalize_offset_and_limit(grn_ctx *ctx, int size, int *p_offset, int *p_limit)
{
  int end;
  int offset = *p_offset;
  int limit = *p_limit;

  if (offset < 0) {
    offset += size;
    if (offset < 0) {
      *p_offset = 0;
      *p_limit = 0;
      return GRN_TOO_SMALL_OFFSET;
    }
  } else if (offset != 0 && offset >= size) {
    *p_offset = 0;
    *p_limit = 0;
    return GRN_TOO_LARGE_OFFSET;
  }

  if (limit < 0) {
    limit += size + 1;
    if (limit < 0) {
      *p_offset = 0;
      *p_limit = 0;
      return GRN_TOO_SMALL_LIMIT;
    }
  } else if (limit > size) {
    limit = size;
  }

  /* At this point, offset and limit must be zero or positive. */
  end = offset + limit;
  if (end > size) {
    limit -= end - size;
  }
  *p_offset = offset;
  *p_limit = limit;
  return GRN_SUCCESS;
}

grn_obj *
grn_inspect_name(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  int name_size;

  name_size = grn_obj_name(ctx, obj, NULL, 0);
  if (name_size > 0) {
    grn_bulk_space(ctx, buf, name_size);
    grn_obj_name(ctx, obj, GRN_BULK_CURR(buf) - name_size, name_size);
  } else {
    grn_id id;

    id = grn_obj_id(ctx, obj);
    if (id == GRN_ID_NIL) {
      GRN_TEXT_PUTS(ctx, buf, "(nil)");
    } else {
      GRN_TEXT_PUTS(ctx, buf, "(anonymous:");
      grn_text_lltoa(ctx, buf, id);
      GRN_TEXT_PUTS(ctx, buf, ")");
    }
  }

  return buf;
}

grn_obj *
grn_inspect_encoding(grn_ctx *ctx, grn_obj *buf, grn_encoding encoding)
{
  switch (encoding) {
  case GRN_ENC_DEFAULT :
    GRN_TEXT_PUTS(ctx, buf, "default(");
    grn_inspect_encoding(ctx, buf, grn_get_default_encoding());
    GRN_TEXT_PUTS(ctx, buf, ")");
    break;
  case GRN_ENC_NONE :
    GRN_TEXT_PUTS(ctx, buf, "none");
    break;
  case GRN_ENC_EUC_JP :
    GRN_TEXT_PUTS(ctx, buf, "EUC-JP");
    break;
  case GRN_ENC_UTF8 :
    GRN_TEXT_PUTS(ctx, buf, "UTF-8");
    break;
  case GRN_ENC_SJIS :
    GRN_TEXT_PUTS(ctx, buf, "Shift_JIS");
    break;
  case GRN_ENC_LATIN1 :
    GRN_TEXT_PUTS(ctx, buf, "Latin-1");
    break;
  case GRN_ENC_KOI8R :
    GRN_TEXT_PUTS(ctx, buf, "KOI8-R");
    break;
  default :
    GRN_TEXT_PUTS(ctx, buf, "unknown(");
    grn_text_itoa(ctx, buf, encoding);
    GRN_TEXT_PUTS(ctx, buf, ")");
    break;
  }

  return buf;
}

grn_obj *
grn_inspect_type(grn_ctx *ctx, grn_obj *buf, unsigned char type)
{
  switch (type) {
  case GRN_VOID :
    GRN_TEXT_PUTS(ctx, buf, "GRN_VOID");
    break;
  case GRN_BULK :
    GRN_TEXT_PUTS(ctx, buf, "GRN_BULK");
    break;
  case GRN_PTR :
    GRN_TEXT_PUTS(ctx, buf, "GRN_PTR");
    break;
  case GRN_UVECTOR :
    GRN_TEXT_PUTS(ctx, buf, "GRN_UVECTOR");
    break;
  case GRN_PVECTOR :
    GRN_TEXT_PUTS(ctx, buf, "GRN_PVECTOR");
    break;
  case GRN_VECTOR :
    GRN_TEXT_PUTS(ctx, buf, "GRN_VECTOR");
    break;
  case GRN_MSG :
    GRN_TEXT_PUTS(ctx, buf, "GRN_MSG");
    break;
  case GRN_QUERY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_QUERY");
    break;
  case GRN_ACCESSOR :
    GRN_TEXT_PUTS(ctx, buf, "GRN_ACCESSOR");
    break;
  case GRN_SNIP :
    GRN_TEXT_PUTS(ctx, buf, "GRN_SNIP");
    break;
  case GRN_PATSNIP :
    GRN_TEXT_PUTS(ctx, buf, "GRN_PATSNIP");
    break;
  case GRN_STRING :
    GRN_TEXT_PUTS(ctx, buf, "GRN_STRING");
    break;
  case GRN_CURSOR_TABLE_HASH_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_CURSOR_TABLE_HASH_KEY");
    break;
  case GRN_CURSOR_TABLE_PAT_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_CURSOR_TABLE_PAT_KEY");
    break;
  case GRN_CURSOR_TABLE_DAT_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_CURSOR_TABLE_DAT_KEY");
    break;
  case GRN_CURSOR_TABLE_NO_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_CURSOR_TABLE_NO_KEY");
    break;
  case GRN_CURSOR_COLUMN_INDEX :
    GRN_TEXT_PUTS(ctx, buf, "GRN_CURSOR_COLUMN_INDEX");
    break;
  case GRN_CURSOR_COLUMN_GEO_INDEX :
    GRN_TEXT_PUTS(ctx, buf, "GRN_CURSOR_COLUMN_GEO_INDEX");
    break;
  case GRN_TYPE :
    GRN_TEXT_PUTS(ctx, buf, "GRN_TYPE");
    break;
  case GRN_PROC :
    GRN_TEXT_PUTS(ctx, buf, "GRN_PROC");
    break;
  case GRN_EXPR :
    GRN_TEXT_PUTS(ctx, buf, "GRN_EXPR");
    break;
  case GRN_TABLE_HASH_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_TABLE_HASH_KEY");
    break;
  case GRN_TABLE_PAT_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_TABLE_PAT_KEY");
    break;
  case GRN_TABLE_DAT_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_TABLE_DAT_KEY");
    break;
  case GRN_TABLE_NO_KEY :
    GRN_TEXT_PUTS(ctx, buf, "GRN_TABLE_NO_KEY");
    break;
  case GRN_DB :
    GRN_TEXT_PUTS(ctx, buf, "GRN_DB");
    break;
  case GRN_COLUMN_FIX_SIZE :
    GRN_TEXT_PUTS(ctx, buf, "GRN_COLUMN_FIX_SIZE");
    break;
  case GRN_COLUMN_VAR_SIZE :
    GRN_TEXT_PUTS(ctx, buf, "GRN_COLUMN_VAR_SIZE");
    break;
  case GRN_COLUMN_INDEX :
    GRN_TEXT_PUTS(ctx, buf, "GRN_COLUMN_INDEX");
    break;
  default:
    {
#define TYPE_IN_HEX_SIZE 5 /* "0xXX" */
      char type_in_hex[TYPE_IN_HEX_SIZE];
      grn_snprintf(type_in_hex,
                   TYPE_IN_HEX_SIZE,
                   TYPE_IN_HEX_SIZE,
                   "%#02x", type);
#undef TYPE_IN_HEX_SIZE
      GRN_TEXT_PUTS(ctx, buf, "(unknown: ");
      GRN_TEXT_PUTS(ctx, buf, type_in_hex);
      GRN_TEXT_PUTS(ctx, buf, ")");
    }
    break;
  }

  return buf;
}


grn_obj *
grn_inspect_query_log_flags(grn_ctx *ctx, grn_obj *buffer, unsigned int flags)
{
  grn_bool have_content = GRN_FALSE;

  if (flags == GRN_QUERY_LOG_NONE) {
    GRN_TEXT_PUTS(ctx, buffer, "NONE");
    return buffer;
  }

#define CHECK_FLAG(NAME) do {                   \
    if (flags & GRN_QUERY_LOG_ ## NAME) {       \
      if (have_content) {                       \
        GRN_TEXT_PUTS(ctx, buffer, "|");        \
      }                                         \
      GRN_TEXT_PUTS(ctx, buffer, #NAME);        \
      have_content = GRN_TRUE;                  \
    }                                           \
  } while (GRN_FALSE)

  CHECK_FLAG(COMMAND);
  CHECK_FLAG(RESULT_CODE);
  CHECK_FLAG(DESTINATION);
  CHECK_FLAG(CACHE);
  CHECK_FLAG(SIZE);
  CHECK_FLAG(SCORE);

#undef CHECK_FALG

  return buffer;
}

static grn_rc
grn_proc_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_proc *proc = (grn_proc *)obj;
  uint32_t i;

  GRN_TEXT_PUTS(ctx, buf, "#<proc:");
  switch (proc->type) {
  case GRN_PROC_INVALID :
    GRN_TEXT_PUTS(ctx, buf, "invalid");
    GRN_TEXT_PUTS(ctx, buf, ">");
    return GRN_SUCCESS;
    break;
  case GRN_PROC_TOKENIZER :
    GRN_TEXT_PUTS(ctx, buf, "tokenizer");
    break;
  case GRN_PROC_COMMAND :
    GRN_TEXT_PUTS(ctx, buf, "command");
    break;
  case GRN_PROC_FUNCTION :
    GRN_TEXT_PUTS(ctx, buf, "function");
    break;
  case GRN_PROC_HOOK :
    GRN_TEXT_PUTS(ctx, buf, "hook");
    break;
  case GRN_PROC_NORMALIZER :
    GRN_TEXT_PUTS(ctx, buf, "normalizer");
    break;
  case GRN_PROC_TOKEN_FILTER :
    GRN_TEXT_PUTS(ctx, buf, "token-filter");
    break;
  case GRN_PROC_SCORER :
    GRN_TEXT_PUTS(ctx, buf, "scorer");
    break;
  case GRN_PROC_WINDOW_FUNCTION :
    GRN_TEXT_PUTS(ctx, buf, "window-function");
    break;
  }
  GRN_TEXT_PUTS(ctx, buf, " ");

  grn_inspect_name(ctx, buf, obj);
  GRN_TEXT_PUTS(ctx, buf, " ");

  GRN_TEXT_PUTS(ctx, buf, "arguments:[");
  for (i = 0; i < proc->nvars; i++) {
    grn_expr_var *var = proc->vars + i;
    if (i != 0) {
      GRN_TEXT_PUTS(ctx, buf, ", ");
    }
    GRN_TEXT_PUT(ctx, buf, var->name, var->name_size);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");

  GRN_TEXT_PUTS(ctx, buf, ">");

  return GRN_SUCCESS;
}

grn_rc
grn_expr_code_inspect_indented(grn_ctx *ctx,
                               grn_obj *buffer,
                               grn_expr_code *code,
                               const char *indent)
{
  if (!code) {
    GRN_TEXT_PUTS(ctx, buffer, "(NULL)");
    return GRN_SUCCESS;
  }

  GRN_TEXT_PUTS(ctx, buffer, "<");
  GRN_TEXT_PUTS(ctx, buffer, grn_operator_to_string(code->op));
  GRN_TEXT_PUTS(ctx, buffer, " ");
  GRN_TEXT_PUTS(ctx, buffer, "n_args:");
  grn_text_itoa(ctx, buffer, code->nargs);
  GRN_TEXT_PUTS(ctx, buffer, ", ");
  GRN_TEXT_PUTS(ctx, buffer, "flags:");
  grn_text_itoh(ctx, buffer, code->flags, 1);
  GRN_TEXT_PUTS(ctx, buffer, ", ");
  GRN_TEXT_PUTS(ctx, buffer, "modify:");
  grn_text_itoa(ctx, buffer, code->modify);
  GRN_TEXT_PUTS(ctx, buffer, ", ");
  GRN_TEXT_PUTS(ctx, buffer, "value:");
  grn_inspect_indented(ctx, buffer, code->value, "      ");
  GRN_TEXT_PUTS(ctx, buffer, ">");

  return GRN_SUCCESS;
}

grn_rc
grn_expr_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;

  GRN_TEXT_PUTS(ctx, buffer, "#<expr\n");
  {
    int i = 0;
    grn_obj *value;
    const char *name;
    uint32_t name_len;
    unsigned int n_vars;
    grn_hash *vars = grn_expr_get_vars(ctx, expr, &n_vars);
    GRN_TEXT_PUTS(ctx, buffer, "  vars:{");
    GRN_HASH_EACH(ctx, vars, id, &name, &name_len, &value, {
      if (i++) {
        GRN_TEXT_PUTC(ctx, buffer, ',');
      }
      GRN_TEXT_PUTS(ctx, buffer, "\n    ");
      GRN_TEXT_PUT(ctx, buffer, name, name_len);
      GRN_TEXT_PUTC(ctx, buffer, ':');
      grn_inspect_indented(ctx, buffer, value, "    ");
    });
    GRN_TEXT_PUTS(ctx, buffer, "\n  },");
  }

  {
    uint32_t i;
    grn_expr_code *code;
    GRN_TEXT_PUTS(ctx, buffer, "\n  codes:{");
    for (i = 0, code = e->codes; i < e->codes_curr; i++, code++) {
      if (i) { GRN_TEXT_PUTC(ctx, buffer, ','); }
      GRN_TEXT_PUTS(ctx, buffer, "\n    ");
      grn_text_itoa(ctx, buffer, i);
      GRN_TEXT_PUTS(ctx, buffer, ":");
      grn_expr_code_inspect_indented(ctx, buffer, code, "      ");
    }
    GRN_TEXT_PUTS(ctx, buffer, "\n  }");
  }

  GRN_TEXT_PUTS(ctx, buffer, "\n>");

  return GRN_SUCCESS;
}

static grn_rc
grn_ptr_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *ptr)
{
  size_t size;

  GRN_TEXT_PUTS(ctx, buffer, "#<ptr:");

  size = GRN_BULK_VSIZE(ptr);
  if (size == 0) {
    GRN_TEXT_PUTS(ctx, buffer, "(empty)");
  } else if (size >= sizeof(grn_obj *)) {
    grn_obj *content = GRN_PTR_VALUE(ptr);
    grn_inspect(ctx, buffer, content);
    if (size > sizeof(grn_obj *)) {
      grn_text_printf(ctx, buffer,
                      " (and more data: %" GRN_FMT_SIZE ")",
                      size - sizeof(grn_obj *));
    }
  }
  GRN_TEXT_PUTS(ctx, buffer, ">");

  return GRN_SUCCESS;
}

static grn_rc
grn_pvector_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *pvector)
{
  int i, n;

  GRN_TEXT_PUTS(ctx, buffer, "[");
  n = GRN_BULK_VSIZE(pvector) / sizeof(grn_obj *);
  for (i = 0; i < n; i++) {
    grn_obj *element = GRN_PTR_VALUE_AT(pvector, i);

    if (i > 0) {
      GRN_TEXT_PUTS(ctx, buffer, ", ");
    }

    grn_inspect(ctx, buffer, element);
  }
  GRN_TEXT_PUTS(ctx, buffer, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_vector_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *vector)
{
  int i;
  grn_obj *body = vector->u.v.body;

  GRN_TEXT_PUTS(ctx, buffer, "[");
  for (i = 0; i < vector->u.v.n_sections; i++) {
    grn_section *section = &(vector->u.v.sections[i]);
    const char *value_raw;

    if (i > 0) {
      GRN_TEXT_PUTS(ctx, buffer, ", ");
    }

    value_raw = GRN_BULK_HEAD(body) + section->offset;
    GRN_TEXT_PUTS(ctx, buffer, "{");
    GRN_TEXT_PUTS(ctx, buffer, "\"value\":");
    {
      grn_obj value_object;
      GRN_OBJ_INIT(&value_object, GRN_BULK, GRN_OBJ_DO_SHALLOW_COPY,
                   section->domain);
      GRN_TEXT_SET(ctx, &value_object, value_raw, section->length);
      grn_inspect(ctx, buffer, &value_object);
      GRN_OBJ_FIN(ctx, &value_object);
    }
    GRN_TEXT_PUTS(ctx, buffer, ", \"weight\":");
    grn_text_itoa(ctx, buffer, section->weight);
    GRN_TEXT_PUTS(ctx, buffer, "}");
  }
  GRN_TEXT_PUTS(ctx, buffer, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_accessor_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_accessor *accessor = (grn_accessor *)obj;

  GRN_TEXT_PUTS(ctx, buf, "#<accessor ");
  for (; accessor; accessor = accessor->next) {
    grn_bool show_obj_name = GRN_FALSE;
    grn_bool show_obj_domain_name = GRN_FALSE;

    if (accessor != (grn_accessor *)obj) {
      GRN_TEXT_PUTS(ctx, buf, ".");
    }
    switch (accessor->action) {
    case GRN_ACCESSOR_GET_ID :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_ID,
                   GRN_COLUMN_NAME_ID_LEN);
      show_obj_name = GRN_TRUE;
      break;
    case GRN_ACCESSOR_GET_KEY :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_KEY,
                   GRN_COLUMN_NAME_KEY_LEN);
      show_obj_name = GRN_TRUE;
      break;
    case GRN_ACCESSOR_GET_VALUE :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_VALUE,
                   GRN_COLUMN_NAME_VALUE_LEN);
      show_obj_name = GRN_TRUE;
      break;
    case GRN_ACCESSOR_GET_SCORE :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_SCORE,
                   GRN_COLUMN_NAME_SCORE_LEN);
      break;
    case GRN_ACCESSOR_GET_NSUBRECS :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_NSUBRECS,
                   GRN_COLUMN_NAME_NSUBRECS_LEN);
      break;
    case GRN_ACCESSOR_GET_MAX :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_MAX,
                   GRN_COLUMN_NAME_MAX_LEN);
      break;
    case GRN_ACCESSOR_GET_MIN :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_MIN,
                   GRN_COLUMN_NAME_MIN_LEN);
      break;
    case GRN_ACCESSOR_GET_SUM :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_SUM,
                   GRN_COLUMN_NAME_SUM_LEN);
      break;
    case GRN_ACCESSOR_GET_AVG :
      GRN_TEXT_PUT(ctx,
                   buf,
                   GRN_COLUMN_NAME_AVG,
                   GRN_COLUMN_NAME_AVG_LEN);
      break;
    case GRN_ACCESSOR_GET_COLUMN_VALUE :
      grn_column_name_(ctx, accessor->obj, buf);
      show_obj_domain_name = GRN_TRUE;
      break;
    case GRN_ACCESSOR_GET_DB_OBJ :
      grn_text_printf(ctx, buf, "(_db)");
      break;
    case GRN_ACCESSOR_LOOKUP :
      grn_text_printf(ctx, buf, "(_lookup)");
      break;
    case GRN_ACCESSOR_FUNCALL :
      grn_text_printf(ctx, buf, "(_funcall)");
      break;
    default :
      grn_text_printf(ctx, buf, "(unknown:%u)", accessor->action);
      break;
    }

    if (show_obj_name || show_obj_domain_name) {
      grn_obj *target = accessor->obj;
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;

      if (show_obj_domain_name) {
        target = grn_ctx_at(ctx, target->header.domain);
      }

      name_size = grn_obj_name(ctx,
                               target,
                               name,
                               GRN_TABLE_MAX_KEY_SIZE);
      GRN_TEXT_PUTS(ctx, buf, "(");
      if (name_size == 0) {
        GRN_TEXT_PUTS(ctx, buf, "anonymous");
      } else {
        GRN_TEXT_PUT(ctx, buf, name, name_size);
      }
      GRN_TEXT_PUTS(ctx, buf, ")");
    }
  }
  GRN_TEXT_PUTS(ctx, buf, ">");

  return GRN_SUCCESS;
}

static grn_rc
grn_type_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_id range_id;

  GRN_TEXT_PUTS(ctx, buf, "#<type ");
  grn_inspect_name(ctx, buf, obj);

  range_id = grn_obj_get_range(ctx, obj);
  GRN_TEXT_PUTS(ctx, buf, " size:");
  grn_text_lltoa(ctx, buf, range_id);

  GRN_TEXT_PUTS(ctx, buf, " type:");
  if (obj->header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    GRN_TEXT_PUTS(ctx, buf, "var_size");
  } else {
    switch (obj->header.flags & GRN_OBJ_KEY_MASK) {
    case GRN_OBJ_KEY_UINT :
      GRN_TEXT_PUTS(ctx, buf, "uint");
      break;
    case GRN_OBJ_KEY_INT :
      GRN_TEXT_PUTS(ctx, buf, "int");
      break;
    case GRN_OBJ_KEY_FLOAT :
      GRN_TEXT_PUTS(ctx, buf, "float");
      break;
    case GRN_OBJ_KEY_GEO_POINT :
      GRN_TEXT_PUTS(ctx, buf, "geo_point");
      break;
    default :
      break;
    }
  }

  GRN_TEXT_PUTS(ctx, buf, ">");
  return GRN_SUCCESS;
}

static grn_rc
grn_column_inspect_common(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_id range_id;

  grn_inspect_name(ctx, buf, obj);

  range_id = grn_obj_get_range(ctx, obj);
  if (range_id) {
    grn_obj *range = grn_ctx_at(ctx, range_id);
    GRN_TEXT_PUTS(ctx, buf, " range:");
    if (range) {
      grn_inspect_name(ctx, buf, range);
    } else {
      grn_text_lltoa(ctx, buf, range_id);
    }
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_store_inspect_body(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_column_inspect_common(ctx, buf, obj);
  GRN_TEXT_PUTS(ctx, buf, " type:");
  switch (obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
  case GRN_OBJ_COLUMN_VECTOR :
    GRN_TEXT_PUTS(ctx, buf, "vector");
    break;
  case GRN_OBJ_COLUMN_SCALAR :
    GRN_TEXT_PUTS(ctx, buf, "scalar");
    break;
  default:
    break;
  }

  GRN_TEXT_PUTS(ctx, buf, " compress:");
  switch (obj->header.flags & GRN_OBJ_COMPRESS_MASK) {
  case GRN_OBJ_COMPRESS_NONE :
    GRN_TEXT_PUTS(ctx, buf, "none");
    break;
  case GRN_OBJ_COMPRESS_ZLIB :
    GRN_TEXT_PUTS(ctx, buf, "zlib");
    break;
  case GRN_OBJ_COMPRESS_LZ4 :
    GRN_TEXT_PUTS(ctx, buf, "lz4");
    break;
  case GRN_OBJ_COMPRESS_ZSTD :
    GRN_TEXT_PUTS(ctx, buf, "zstd");
    break;
  default:
    break;
  }

  if (obj->header.flags & GRN_OBJ_RING_BUFFER) {
    GRN_TEXT_PUTS(ctx, buf, " ring_buffer:true");
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_ra_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  GRN_TEXT_PUTS(ctx, buf, "#<column:fix_size ");
  grn_store_inspect_body(ctx, buf, obj);
  GRN_TEXT_PUTS(ctx, buf, ">");
  return GRN_SUCCESS;
}

static grn_rc
grn_ja_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  GRN_TEXT_PUTS(ctx, buf, "#<column:var_size ");
  grn_store_inspect_body(ctx, buf, obj);
  GRN_TEXT_PUTS(ctx, buf, ">");
  return GRN_SUCCESS;
}

static grn_rc
grn_ii_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_obj sources;
  int i, n, have_flags = 0;
  grn_id *source_ids;

  GRN_TEXT_PUTS(ctx, buf, "#<column:index ");
  grn_column_inspect_common(ctx, buf, obj);

  GRN_TEXT_INIT(&sources, 0);
  grn_obj_get_info(ctx, obj, GRN_INFO_SOURCE, &sources);
  source_ids = (grn_id *)GRN_BULK_HEAD(&sources);
  n = GRN_BULK_VSIZE(&sources) / sizeof(grn_id);
  GRN_TEXT_PUTS(ctx, buf, " sources:[");
  for (i = 0; i < n; i++) {
    grn_id source_id;
    grn_obj *source;
    if (i) { GRN_TEXT_PUTS(ctx, buf, ", "); }
    source_id = source_ids[i];
    source = grn_ctx_at(ctx, source_id);
    if (source) {
      grn_inspect_name(ctx, buf, source);
    } else {
      grn_text_lltoa(ctx, buf, source_id);
    }
  }
  GRN_TEXT_PUTS(ctx, buf, "]");
  GRN_OBJ_FIN(ctx, &sources);

  GRN_TEXT_PUTS(ctx, buf, " flags:");
  if (obj->header.flags & GRN_OBJ_WITH_SECTION) {
    GRN_TEXT_PUTS(ctx, buf, "SECTION");
    have_flags = 1;
  }
  if (obj->header.flags & GRN_OBJ_WITH_WEIGHT) {
    if (have_flags) { GRN_TEXT_PUTS(ctx, buf, "|"); }
    GRN_TEXT_PUTS(ctx, buf, "WEIGHT");
    have_flags = 1;
  }
  if (obj->header.flags & GRN_OBJ_WITH_POSITION) {
    if (have_flags) { GRN_TEXT_PUTS(ctx, buf, "|"); }
    GRN_TEXT_PUTS(ctx, buf, "POSITION");
    have_flags = 1;
  }
  if (!have_flags) {
    GRN_TEXT_PUTS(ctx, buf, "NONE");
  }

  GRN_TEXT_PUTS(ctx, buf, ">");

  return GRN_SUCCESS;
}

static grn_rc
grn_table_type_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  switch (obj->header.type) {
  case GRN_TABLE_HASH_KEY:
    GRN_TEXT_PUTS(ctx, buf, "hash");
    break;
  case GRN_TABLE_PAT_KEY:
    GRN_TEXT_PUTS(ctx, buf, "pat");
    break;
  case GRN_TABLE_DAT_KEY:
    GRN_TEXT_PUTS(ctx, buf, "dat");
    break;
  case GRN_TABLE_NO_KEY:
    GRN_TEXT_PUTS(ctx, buf, "no_key");
    break;
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_table_key_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_obj *domain;
  grn_id domain_id;

  GRN_TEXT_PUTS(ctx, buf, "key:");
  domain_id = obj->header.domain;
  domain = grn_ctx_at(ctx, domain_id);
  if (domain) {
    grn_inspect_name(ctx, buf, domain);
  } else if (domain_id) {
    grn_text_lltoa(ctx, buf, domain_id);
  } else {
    GRN_TEXT_PUTS(ctx, buf, "(nil)");
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_table_columns_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_hash *cols;

  GRN_TEXT_PUTS(ctx, buf, "columns:[");
  if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                              GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
    if (grn_table_columns(ctx, obj, "", 0, (grn_obj *)cols)) {
      int i = 0;
      grn_id *key;
      GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
          grn_obj *col = grn_ctx_at(ctx, *key);
          if (col) {
            if (i++ > 0) { GRN_TEXT_PUTS(ctx, buf, ", "); }
            grn_column_name_(ctx, col, buf);
          }
        });
    }
    grn_hash_close(ctx, cols);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_table_ids_and_values_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  int i = 0;
  grn_obj value;

  GRN_VALUE_FIX_SIZE_INIT(&value, 0, grn_obj_get_range(ctx, obj));

  GRN_TEXT_PUTS(ctx, buf, "ids&values:[");
  GRN_TABLE_EACH_BEGIN(ctx, obj, cursor, id) {
    void *value_buffer;
    int value_size;

    if (i++ > 0) {
      GRN_TEXT_PUTS(ctx, buf, ", ");
    }

    GRN_TEXT_PUTS(ctx, buf, "\n  ");
    grn_text_lltoa(ctx, buf, id);
    GRN_TEXT_PUTS(ctx, buf, ":");
    value_size = grn_table_cursor_get_value(ctx, cursor, &value_buffer);
    grn_bulk_write_from(ctx, &value, value_buffer, 0, value_size);
    grn_inspect(ctx, buf, &value);
  } GRN_TABLE_EACH_END(ctx, cursor);
  GRN_TEXT_PUTS(ctx, buf, "\n]");

  GRN_OBJ_FIN(ctx, &value);

  return GRN_SUCCESS;
}

static grn_rc
grn_table_ids_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_table_cursor *tc;

  GRN_TEXT_PUTS(ctx, buf, "ids:[");
  tc = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0,
                             0, -1, GRN_CURSOR_ASCENDING);
  if (tc) {
    int i = 0;
    grn_id id;
    while ((id = grn_table_cursor_next(ctx, tc))) {
      if (i++ > 0) { GRN_TEXT_PUTS(ctx, buf, ", "); }
      grn_text_lltoa(ctx, buf, id);
    }
    grn_table_cursor_close(ctx, tc);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_table_default_tokenizer_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_obj *default_tokenizer;

  GRN_TEXT_PUTS(ctx, buf, "default_tokenizer:");
  default_tokenizer = grn_obj_get_info(ctx, obj,
                                       GRN_INFO_DEFAULT_TOKENIZER, NULL);
  if (default_tokenizer) {
    grn_inspect_name(ctx, buf, default_tokenizer);
  } else {
    GRN_TEXT_PUTS(ctx, buf, "(nil)");
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_table_normalizer_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_obj *normalizer;

  GRN_TEXT_PUTS(ctx, buf, "normalizer:");
  normalizer = grn_obj_get_info(ctx, obj, GRN_INFO_NORMALIZER, NULL);
  if (normalizer) {
    grn_inspect_name(ctx, buf, normalizer);
  } else {
    GRN_TEXT_PUTS(ctx, buf, "(nil)");
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_table_keys_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_table_cursor *tc;
  int max_n_keys = 10;

  /* TODO */
  /* max_n_keys = grn_atoi(grn_getenv("GRN_INSPECT_TABLE_MAX_N_KEYS")); */

  GRN_TEXT_PUTS(ctx, buf, "keys:[");
  tc = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0,
                             0, -1, GRN_CURSOR_ASCENDING);
  if (tc) {
    int i = 0;
    grn_id id;
    grn_obj key;
    GRN_OBJ_INIT(&key, GRN_BULK, 0, obj->header.domain);
    while ((id = grn_table_cursor_next(ctx, tc))) {
      if (max_n_keys > 0 && i >= max_n_keys) {
        GRN_TEXT_PUTS(ctx, buf, ", ...");
        break;
      }
      if (i++ > 0) { GRN_TEXT_PUTS(ctx, buf, ", "); }
      grn_table_get_key2(ctx, obj, id, &key);
      grn_inspect(ctx, buf, &key);
      GRN_BULK_REWIND(&key);
    }
    GRN_OBJ_FIN(ctx, &key);
    grn_table_cursor_close(ctx, tc);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_table_subrec_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  GRN_TEXT_PUTS(ctx, buf, "subrec:");
  if (obj->header.flags & GRN_OBJ_WITH_SUBREC) {
    switch (obj->header.flags & GRN_OBJ_UNIT_MASK) {
    case GRN_OBJ_UNIT_DOCUMENT_NONE :
      GRN_TEXT_PUTS(ctx, buf, "document:none");
      break;
    case GRN_OBJ_UNIT_DOCUMENT_SECTION :
      GRN_TEXT_PUTS(ctx, buf, "document:section");
      break;
    case GRN_OBJ_UNIT_DOCUMENT_POSITION :
      GRN_TEXT_PUTS(ctx, buf, "document:position");
      break;
    case GRN_OBJ_UNIT_SECTION_NONE :
      GRN_TEXT_PUTS(ctx, buf, "section:none");
      break;
    case GRN_OBJ_UNIT_SECTION_POSITION :
      GRN_TEXT_PUTS(ctx, buf, "section:popsition");
      break;
    case GRN_OBJ_UNIT_POSITION_NONE :
      GRN_TEXT_PUTS(ctx, buf, "section:none");
      break;
    case GRN_OBJ_UNIT_USERDEF_DOCUMENT :
      GRN_TEXT_PUTS(ctx, buf, "userdef:document");
      break;
    case GRN_OBJ_UNIT_USERDEF_SECTION :
      GRN_TEXT_PUTS(ctx, buf, "userdef:section");
      break;
    case GRN_OBJ_UNIT_USERDEF_POSITION :
      GRN_TEXT_PUTS(ctx, buf, "userdef:position");
      break;
    }
  } else {
    GRN_TEXT_PUTS(ctx, buf, "none");
  }

  return GRN_SUCCESS;
}

static grn_rc
grn_table_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_id range_id;
  grn_obj *range;

  GRN_TEXT_PUTS(ctx, buf, "#<table:");
  grn_table_type_inspect(ctx, buf, obj);
  GRN_TEXT_PUTS(ctx, buf, " ");

  grn_inspect_name(ctx, buf, obj);

  if (obj->header.type != GRN_TABLE_NO_KEY) {
    GRN_TEXT_PUTS(ctx, buf, " ");
    grn_table_key_inspect(ctx, buf, obj);
  }

  GRN_TEXT_PUTS(ctx, buf, " value:");
  range_id = grn_obj_get_range(ctx, obj);
  range = grn_ctx_at(ctx, range_id);
  if (range) {
    grn_inspect_name(ctx, buf, range);
  } else if (range_id) {
    grn_text_lltoa(ctx, buf, range_id);
  } else {
    GRN_TEXT_PUTS(ctx, buf, "(nil)");
  }

  GRN_TEXT_PUTS(ctx, buf, " size:");
  grn_text_lltoa(ctx, buf, grn_table_size(ctx, obj));

  GRN_TEXT_PUTS(ctx, buf, " ");
  grn_table_columns_inspect(ctx, buf, obj);

  if (obj->header.type == GRN_TABLE_NO_KEY) {
    GRN_TEXT_PUTS(ctx, buf, " ");
    if (range) {
      grn_table_ids_and_values_inspect(ctx, buf, obj);
    } else {
      grn_table_ids_inspect(ctx, buf, obj);
    }
  } else {
    GRN_TEXT_PUTS(ctx, buf, " ");
    grn_table_default_tokenizer_inspect(ctx, buf, obj);

    GRN_TEXT_PUTS(ctx, buf, " ");
    grn_table_normalizer_inspect(ctx, buf, obj);

    GRN_TEXT_PUTS(ctx, buf, " ");
    grn_table_keys_inspect(ctx, buf, obj);
  }

  GRN_TEXT_PUTS(ctx, buf, " ");
  grn_table_subrec_inspect(ctx, buf, obj);

  if (obj->header.type == GRN_TABLE_PAT_KEY) {
    GRN_TEXT_PUTS(ctx, buf, " nodes:");
    grn_pat_inspect_nodes(ctx, (grn_pat *)obj, buf);
  }

  GRN_TEXT_PUTS(ctx, buf, ">");

  return GRN_SUCCESS;
}

static grn_rc
grn_db_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_db *db = (grn_db *)obj;

  GRN_TEXT_PUTS(ctx, buf, "#<db");

  GRN_TEXT_PUTS(ctx, buf, " key_type:");
  grn_table_type_inspect(ctx, buf, db->keys);

  GRN_TEXT_PUTS(ctx, buf, " size:");
  grn_text_lltoa(ctx, buf, grn_table_size(ctx, obj));

  GRN_TEXT_PUTS(ctx, buf, ">");

  return GRN_SUCCESS;
}

static grn_rc
grn_time_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj)
{
  int64_t time_raw;
  int64_t sec;
  int32_t usec;

  time_raw = GRN_TIME_VALUE(obj);
  GRN_TIME_UNPACK(time_raw, sec, usec);
  grn_text_printf(ctx, buffer,
                  "%" GRN_FMT_INT64D ".%d",
                  sec, usec);

  return GRN_SUCCESS;
}

static grn_rc
grn_geo_point_inspect_point(grn_ctx *ctx, grn_obj *buf, int point)
{
  GRN_TEXT_PUTS(ctx, buf, "(");
  grn_text_itoa(ctx, buf, point / 1000 / 3600 % 3600);
  GRN_TEXT_PUTS(ctx, buf, ", ");
  grn_text_itoa(ctx, buf, point / 1000 / 60 % 60);
  GRN_TEXT_PUTS(ctx, buf, ", ");
  grn_text_itoa(ctx, buf, point / 1000 % 60);
  GRN_TEXT_PUTS(ctx, buf, ", ");
  grn_text_itoa(ctx, buf, point % 1000);
  GRN_TEXT_PUTS(ctx, buf, ")");

  return GRN_SUCCESS;
}

static grn_rc
grn_geo_point_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  int latitude, longitude;

  GRN_GEO_POINT_VALUE(obj, latitude, longitude);

  GRN_TEXT_PUTS(ctx, buf, "[");
  GRN_TEXT_PUTS(ctx, buf, "(");
  grn_text_itoa(ctx, buf, latitude);
  GRN_TEXT_PUTS(ctx, buf, ",");
  grn_text_itoa(ctx, buf, longitude);
  GRN_TEXT_PUTS(ctx, buf, ")");

  GRN_TEXT_PUTS(ctx, buf, " (");
  grn_geo_point_inspect_point(ctx, buf, latitude);
  GRN_TEXT_PUTS(ctx, buf, ",");
  grn_geo_point_inspect_point(ctx, buf, longitude);
  GRN_TEXT_PUTS(ctx, buf, ")");

  {
    int i, j;
    grn_geo_point point;
    uint8_t encoded[sizeof(grn_geo_point)];

    GRN_TEXT_PUTS(ctx, buf, " [");
    point.latitude = latitude;
    point.longitude = longitude;
    grn_gton(encoded, &point, sizeof(grn_geo_point));
    for (i = 0; i < sizeof(grn_geo_point); i++) {
      if (i != 0) {
        GRN_TEXT_PUTS(ctx, buf, " ");
      }
      for (j = 0; j < 8; j++) {
        grn_text_itoa(ctx, buf, (encoded[i] >> (7 - j)) & 1);
      }
    }
    GRN_TEXT_PUTS(ctx, buf, "]");
  }

  GRN_TEXT_PUTS(ctx, buf, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_json_load_open_bracket_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  uint32_t i, n;

  n = GRN_UINT32_VALUE(obj);

  GRN_TEXT_PUTS(ctx, buf, "[");
  for (i = 0; i < n; i++) {
    grn_obj *value;
    value = obj + 1 + i;
    if (i > 0) {
      GRN_TEXT_PUTS(ctx, buf, ", ");
    }
    grn_inspect(ctx, buf, value);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");

  return GRN_SUCCESS;
}

static grn_rc
grn_json_load_open_brace_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  uint32_t i, n;

  n = GRN_UINT32_VALUE(obj);

  GRN_TEXT_PUTS(ctx, buf, "{");
  for (i = 0; i < n; i += 2) {
    grn_obj *key, *value;
    key = obj + 1 + i;
    value = key + 1;
    if (i > 0) {
      GRN_TEXT_PUTS(ctx, buf, ", ");
    }
    grn_inspect(ctx, buf, key);
    GRN_TEXT_PUTS(ctx, buf, ": ");
    grn_inspect(ctx, buf, value);
  }
  GRN_TEXT_PUTS(ctx, buf, "}");

  return GRN_SUCCESS;
}

static grn_rc
grn_record_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_obj *table;
  grn_hash *cols;

  table = grn_ctx_at(ctx, obj->header.domain);
  GRN_TEXT_PUTS(ctx, buf, "#<record:");
  if (table) {
    grn_table_type_inspect(ctx, buf, table);
    GRN_TEXT_PUTS(ctx, buf, ":");
    grn_inspect_name(ctx, buf, table);
  } else {
    GRN_TEXT_PUTS(ctx, buf, "(anonymous table:");
    grn_text_lltoa(ctx, buf, obj->header.domain);
    GRN_TEXT_PUTS(ctx, buf, ")");
  }

  GRN_TEXT_PUTS(ctx, buf, " id:");
  if (GRN_BULK_VSIZE(obj) == 0) {
    GRN_TEXT_PUTS(ctx, buf, "(no value)");
  } else {
    grn_id id;

  id = GRN_RECORD_VALUE(obj);
  grn_text_lltoa(ctx, buf, id);

  if (table && grn_table_at(ctx, table, id)) {
    if (table->header.type != GRN_TABLE_NO_KEY) {
      grn_obj key;
      GRN_TEXT_PUTS(ctx, buf, " key:");
      GRN_OBJ_INIT(&key, GRN_BULK, 0, table->header.domain);
      grn_table_get_key2(ctx, table, id, &key);
      grn_inspect(ctx, buf, &key);
      GRN_OBJ_FIN(ctx, &key);
    }
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      if (grn_table_columns(ctx, table, "", 0, (grn_obj *)cols)) {
        grn_id *key;
        GRN_HASH_EACH(ctx, cols, column_id, &key, NULL, NULL, {
            grn_obj *col = grn_ctx_at(ctx, *key);
            if (col) {
              grn_obj value;
              GRN_TEXT_INIT(&value, 0);
              GRN_TEXT_PUTS(ctx, buf, " ");
              grn_column_name_(ctx, col, buf);
              GRN_TEXT_PUTS(ctx, buf, ":");
              grn_obj_get_value(ctx, col, id, &value);
              grn_inspect(ctx, buf, &value);
              GRN_OBJ_FIN(ctx, &value);
            }
          });
      }
      grn_hash_close(ctx, cols);
    }
  } else {
    GRN_TEXT_PUTS(ctx, buf, "(nonexistent)");
  }
  }

  GRN_TEXT_PUTS(ctx, buf, ">");

  return GRN_SUCCESS;
}

static grn_rc
grn_uvector_record_inspect(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  unsigned int i, n = 0;
  grn_obj record;

  GRN_RECORD_INIT(&record, 0, obj->header.domain);
  GRN_TEXT_PUTS(ctx, buf, "[");
  n = grn_vector_size(ctx, obj);
  for (i = 0; i < n; i++) {
    grn_id id;
    unsigned int weight;

    if (i > 0) {
      GRN_TEXT_PUTS(ctx, buf, ", ");
    }

    id = grn_uvector_get_element(ctx, obj, i, &weight);
    GRN_TEXT_PUTS(ctx, buf, "#<element record:");
    GRN_RECORD_SET(ctx, &record, id);
    grn_inspect(ctx, buf, &record);
    grn_text_printf(ctx, buf, ", weight:%u>", weight);
  }
  GRN_TEXT_PUTS(ctx, buf, "]");
  GRN_OBJ_FIN(ctx, &record);

  return GRN_SUCCESS;
}

grn_obj *
grn_inspect(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj)
{
  grn_obj *domain;

  if (!buffer) {
    buffer = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_TEXT);
  }

  if (!obj) {
    GRN_TEXT_PUTS(ctx, buffer, "(NULL)");
    return buffer;
  }

  switch (obj->header.type) {
  case GRN_VOID :
    /* TODO */
    break;
  case GRN_BULK :
    switch (obj->header.domain) {
    case GRN_DB_TIME :
      grn_time_inspect(ctx, buffer, obj);
      return buffer;
    case GRN_DB_TOKYO_GEO_POINT :
    case GRN_DB_WGS84_GEO_POINT :
      grn_geo_point_inspect(ctx, buffer, obj);
      return buffer;
    case GRN_JSON_LOAD_OPEN_BRACKET :
      grn_json_load_open_bracket_inspect(ctx, buffer, obj);
      return buffer;
    case GRN_JSON_LOAD_OPEN_BRACE :
      grn_json_load_open_brace_inspect(ctx, buffer, obj);
      return buffer;
    default :
      domain = grn_ctx_at(ctx, obj->header.domain);
      if (domain) {
        grn_id type = domain->header.type;
        switch (type) {
        case GRN_TABLE_HASH_KEY :
        case GRN_TABLE_PAT_KEY :
        case GRN_TABLE_NO_KEY :
          grn_record_inspect(ctx, buffer, obj);
          return buffer;
        default :
          break;
        }
      }
    }
    break;
  case GRN_PTR :
    grn_ptr_inspect(ctx, buffer, obj);
    break;
  case GRN_UVECTOR :
    domain = grn_ctx_at(ctx, obj->header.domain);
    if (domain) {
      grn_id type = domain->header.type;
      switch (type) {
      case GRN_TABLE_HASH_KEY :
      case GRN_TABLE_PAT_KEY :
      case GRN_TABLE_NO_KEY :
        grn_uvector_record_inspect(ctx, buffer, obj);
        return buffer;
      default :
        break;
      }
    }
    break;
  case GRN_PVECTOR :
    grn_pvector_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_VECTOR :
    grn_vector_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_MSG :
    /* TODO */
    break;
  case GRN_ACCESSOR :
    grn_accessor_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_SNIP :
  case GRN_PATSNIP :
    /* TODO */
    break;
  case GRN_STRING :
    grn_string_inspect(ctx, buffer, obj);
    break;
  case GRN_CURSOR_TABLE_HASH_KEY :
    /* TODO */
    break;
  case GRN_CURSOR_TABLE_PAT_KEY :
    grn_pat_cursor_inspect(ctx, (grn_pat_cursor *)obj, buffer);
    return buffer;
  case GRN_CURSOR_TABLE_DAT_KEY :
  case GRN_CURSOR_TABLE_NO_KEY :
  case GRN_CURSOR_COLUMN_INDEX :
  case GRN_CURSOR_COLUMN_GEO_INDEX :
    /* TODO */
    break;
  case GRN_TYPE :
    grn_type_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_PROC :
    grn_proc_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_EXPR :
    grn_expr_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
  case GRN_TABLE_NO_KEY :
    grn_table_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_DB :
    grn_db_inspect(ctx, buffer, obj);
    break;
  case GRN_COLUMN_FIX_SIZE :
    grn_ra_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_COLUMN_VAR_SIZE :
    grn_ja_inspect(ctx, buffer, obj);
    return buffer;
  case GRN_COLUMN_INDEX :
    grn_ii_inspect(ctx, buffer, obj);
    return buffer;
  default :
    break;
  }

  grn_text_otoj(ctx, buffer, obj, NULL);
  return buffer;
}

grn_obj *
grn_inspect_indented(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj,
                     const char *indent)
{
  grn_obj sub_buffer;

  GRN_TEXT_INIT(&sub_buffer, 0);
  grn_inspect(ctx, &sub_buffer, obj);
  {
    const char *inspected = GRN_TEXT_VALUE(&sub_buffer);
    size_t inspected_size = GRN_TEXT_LEN(&sub_buffer);
    size_t i, line_start;

    if (!buffer) {
      buffer = grn_obj_open(ctx, GRN_BULK, 0, GRN_DB_TEXT);
    }

    line_start = 0;
    for (i = 0; i < inspected_size; i++) {
      if (inspected[i] == '\n') {
        if (line_start != 0) {
          GRN_TEXT_PUTS(ctx, buffer, indent);
        }
        GRN_TEXT_PUT(ctx, buffer, inspected + line_start, i + 1 - line_start);
        line_start = i + 1;
      }
    }
    if (line_start != 0) {
      GRN_TEXT_PUTS(ctx, buffer, indent);
    }
    GRN_TEXT_PUT(ctx, buffer,
                 inspected + line_start,
                 inspected_size - line_start);
  }
  GRN_OBJ_FIN(ctx, &sub_buffer);

  return buffer;
}

grn_obj *
grn_inspect_limited(grn_ctx *ctx, grn_obj *buffer, grn_obj *obj)
{
  grn_obj sub_buffer;
  unsigned int max_size = GRN_CTX_MSGSIZE / 2;

  GRN_TEXT_INIT(&sub_buffer, 0);
  grn_inspect(ctx, &sub_buffer, obj);
  if (GRN_TEXT_LEN(&sub_buffer) > max_size) {
    GRN_TEXT_PUT(ctx, buffer, GRN_TEXT_VALUE(&sub_buffer), max_size);
    GRN_TEXT_PUTS(ctx, buffer, "...(");
    grn_text_lltoa(ctx, buffer, GRN_TEXT_LEN(&sub_buffer));
    GRN_TEXT_PUTS(ctx, buffer, ")");
  } else {
    GRN_TEXT_PUT(ctx,
                 buffer,
                 GRN_TEXT_VALUE(&sub_buffer),
                 GRN_TEXT_LEN(&sub_buffer));
  }
  GRN_OBJ_FIN(ctx, &sub_buffer);

  return buffer;
}

void
grn_p(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj buffer;

  GRN_TEXT_INIT(&buffer, 0);
  grn_inspect(ctx, &buffer, obj);
  printf("%.*s\n", (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
  GRN_OBJ_FIN(ctx, &buffer);
}

void
grn_p_geo_point(grn_ctx *ctx, grn_geo_point *point)
{
  grn_obj obj;

  GRN_WGS84_GEO_POINT_INIT(&obj, 0);
  GRN_GEO_POINT_SET(ctx, &obj, point->latitude, point->longitude);
  grn_p(ctx, &obj);
  GRN_OBJ_FIN(ctx, &obj);
}

void
grn_p_ii_values(grn_ctx *ctx, grn_obj *ii)
{
  grn_obj buffer;

  GRN_TEXT_INIT(&buffer, 0);
  grn_ii_inspect_values(ctx, (grn_ii *)ii, &buffer);
  printf("%.*s\n", (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
  GRN_OBJ_FIN(ctx, &buffer);
}

void
grn_p_expr_code(grn_ctx *ctx, grn_expr_code *code)
{
  grn_obj buffer;

  GRN_TEXT_INIT(&buffer, 0);
  grn_expr_code_inspect_indented(ctx, &buffer, code, "");
  printf("%.*s\n", (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
  GRN_OBJ_FIN(ctx, &buffer);
}

void
grn_p_record(grn_ctx *ctx, grn_obj *table, grn_id id)
{
  grn_obj record;

  GRN_RECORD_INIT(&record, 0, grn_obj_id(ctx, table));
  GRN_RECORD_SET(ctx, &record, id);
  grn_p(ctx, &record);
  GRN_OBJ_FIN(ctx, &record);
}

#ifdef WIN32
int
grn_mkstemp(char *path_template)
{
  errno_t error;
  size_t path_template_size;
  int fd;

  path_template_size = strlen(path_template) + 1;
  error = _mktemp_s(path_template, path_template_size);
  if (error != 0) {
    return -1;
  }

  error = _sopen_s(&fd,
                   path_template,
                   _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
                   _SH_DENYNO,
                   _S_IREAD | _S_IWRITE);
  if (error != 0) {
    return -1;
  }

  return fd;
}
#else /* WIN32 */
int
grn_mkstemp(char *path_template)
{
# ifdef HAVE_MKSTEMP
  return mkstemp(path_template);
# else /* HAVE_MKSTEMP */
  mktemp(path_template);
  return open(path_template, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
# endif /* HAVE_MKSTEMP */
}
#endif /* WIN32 */

grn_bool
grn_path_exist(const char *path)
{
  struct stat status;
  return stat(path, &status) == 0;
}

/* todo : refine */
/*
 * grn_tokenize splits a string into at most buf_size tokens and
 * returns the number of tokens. The ending address of each token is
 * written into tokbuf. Delimiters are ' ' and ','.
 * Then, the address to the remaining is set to rest.
 */
int
grn_tokenize(const char *str, size_t str_len,
             const char **tokbuf, int buf_size,
             const char **rest)
{
  const char **tok = tokbuf, **tok_end = tokbuf + buf_size;
  if (buf_size > 0) {
    const char *str_end = str + str_len;
    while (str < str_end && (' ' == *str || ',' == *str)) { str++; }
    for (;;) {
      if (str == str_end) {
        *tok++ = str;
        break;
      }
      if (' ' == *str || ',' == *str) {
        /* *str = '\0'; */
        *tok++ = str;
        if (tok == tok_end) { break; }
        do { str++; } while (str < str_end && (' ' == *str || ',' == *str));
      } else {
        str++;
      }
    }
  }
  if (rest) { *rest = str; }
  return tok - tokbuf;
}
