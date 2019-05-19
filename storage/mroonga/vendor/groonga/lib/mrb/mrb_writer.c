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

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/data.h>
#include <mruby/hash.h>
#include <mruby/string.h>

#include "../grn_mrb.h"
#include "../grn_output.h"
#include "mrb_ctx.h"
#include "mrb_writer.h"
#include "mrb_options.h"

static mrb_value
writer_write(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value target;

  mrb_get_args(mrb, "o", &target);

  switch (mrb_type(target)) {
  case MRB_TT_FALSE :
    if (mrb_nil_p(target)) {
      GRN_OUTPUT_NULL();
    } else {
      GRN_OUTPUT_BOOL(GRN_FALSE);
    }
    break;
  case MRB_TT_TRUE :
    GRN_OUTPUT_BOOL(GRN_TRUE);
    break;
  case MRB_TT_FIXNUM :
    GRN_OUTPUT_INT32(mrb_fixnum(target));
    break;
  case MRB_TT_FLOAT :
    GRN_OUTPUT_FLOAT(mrb_float(target));
    break;
  case MRB_TT_SYMBOL :
    {
      const char *name;
      mrb_int name_length;

      name = mrb_sym2name_len(mrb, mrb_symbol(target), &name_length);
      GRN_OUTPUT_STR(name, name_length);
    }
    break;
  case MRB_TT_STRING :
    GRN_OUTPUT_STR(RSTRING_PTR(target), RSTRING_LEN(target));
    break;
  default :
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
               "must be nil, true, false, number, float, symbol or string: "
               "%S",
               target);
    break;
  }

  return mrb_nil_value();
}

static mrb_value
writer_open_array(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *name;
  mrb_int n_elements;

  mrb_get_args(mrb, "zi", &name, &n_elements);
  GRN_OUTPUT_ARRAY_OPEN(name, n_elements);

  return mrb_nil_value();
}

static mrb_value
writer_close_array(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  GRN_OUTPUT_ARRAY_CLOSE();

  return mrb_nil_value();
}

static mrb_value
writer_open_map(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *name;
  mrb_int n_elements;

  mrb_get_args(mrb, "zi", &name, &n_elements);
  GRN_OUTPUT_MAP_OPEN(name, n_elements);

  return mrb_nil_value();
}

static mrb_value
writer_close_map(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  GRN_OUTPUT_MAP_CLOSE();

  return mrb_nil_value();
}

static mrb_value
writer_write_table_columns(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_table;
  char *columns;
  mrb_int columns_size;
  grn_obj *table;
  grn_obj_format format;
  int n_hits = 0;
  int offset = 0;
  int limit = 0;
  int hits_offset = 0;

  mrb_get_args(mrb, "os", &mrb_table, &columns, &columns_size);

  table = DATA_PTR(mrb_table);
  GRN_OBJ_FORMAT_INIT(&format, n_hits, offset, limit, hits_offset);
  format.flags |= GRN_OBJ_FORMAT_WITH_COLUMN_NAMES;
  {
    grn_rc rc;
    rc = grn_output_format_set_columns(ctx, &format,
                                       table, columns, columns_size);
    if (rc != GRN_SUCCESS) {
      GRN_OBJ_FORMAT_FIN(ctx, &format);
      grn_mrb_ctx_check(mrb);
    }
  }
  GRN_OUTPUT_TABLE_COLUMNS(table, &format);
  GRN_OBJ_FORMAT_FIN(ctx, &format);

  return mrb_nil_value();
}

static mrb_value
writer_write_table_records(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_table;
  mrb_value mrb_options = mrb_nil_value();
  char *columns;
  mrb_int columns_size;
  grn_obj *table;
  grn_obj_format format;
  int n_hits = 0;
  int offset = 0;
  int limit = -1;
  int hits_offset = 0;

  mrb_get_args(mrb, "os|H", &mrb_table, &columns, &columns_size, &mrb_options);

  table = DATA_PTR(mrb_table);
  if (!mrb_nil_p(mrb_options)) {
    mrb_value mrb_offset;
    mrb_value mrb_limit;

    mrb_offset = grn_mrb_options_get_lit(mrb, mrb_options, "offset");
    if (!mrb_nil_p(mrb_offset)) {
      offset = mrb_fixnum(mrb_offset);
    }

    mrb_limit = grn_mrb_options_get_lit(mrb, mrb_options, "limit");
    if (!mrb_nil_p(mrb_limit)) {
      limit = mrb_fixnum(mrb_limit);
    }
  }
  if (limit < 0) {
    limit = grn_table_size(ctx, table) + limit + 1;
  }
  GRN_OBJ_FORMAT_INIT(&format, n_hits, offset, limit, hits_offset);
  {
    grn_rc rc;
    rc = grn_output_format_set_columns(ctx, &format,
                                       table, columns, columns_size);
    if (rc != GRN_SUCCESS) {
      GRN_OBJ_FORMAT_FIN(ctx, &format);
      grn_mrb_ctx_check(mrb);
    }
  }
  GRN_OUTPUT_TABLE_RECORDS(table, &format);
  GRN_OBJ_FORMAT_FIN(ctx, &format);

  return mrb_nil_value();
}

static mrb_value
writer_set_content_type(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int content_type;

  mrb_get_args(mrb, "i", &content_type);

  grn_ctx_set_output_type(ctx, content_type);

  return mrb_nil_value();
}

void
grn_mrb_writer_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Writer", mrb->object_class);

  mrb_define_method(mrb, klass, "write", writer_write, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "open_array",
                    writer_open_array, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, klass, "close_array",
                    writer_close_array, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "open_map",
                    writer_open_map, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, klass, "close_map",
                    writer_close_map, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "write_table_columns",
                    writer_write_table_columns, MRB_ARGS_REQ(2));
  mrb_define_method(mrb, klass, "write_table_records",
                    writer_write_table_records, MRB_ARGS_ARG(2, 1));

  mrb_define_method(mrb, klass, "content_type=",
                    writer_set_content_type, MRB_ARGS_REQ(1));
}
#endif
