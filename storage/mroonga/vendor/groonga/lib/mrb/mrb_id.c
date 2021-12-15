/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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

#include "mrb_id.h"

void
grn_mrb_id_init(grn_ctx *ctx)
{
  mrb_state *mrb = ctx->impl->mrb.state;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *id_module;

  id_module = mrb_define_module_under(mrb, module, "ID");

  mrb_define_const(mrb, id_module, "NIL",
                   mrb_fixnum_value(GRN_ID_NIL));
  mrb_define_const(mrb, id_module, "MAX",
                   mrb_fixnum_value(GRN_ID_MAX));

  mrb_define_const(mrb, id_module, "VOID",
                   mrb_fixnum_value(GRN_DB_VOID));
  mrb_define_const(mrb, id_module, "DB",
                   mrb_fixnum_value(GRN_DB_DB));
  mrb_define_const(mrb, id_module, "OBJECT",
                   mrb_fixnum_value(GRN_DB_OBJECT));
  mrb_define_const(mrb, id_module, "BOOL",
                   mrb_fixnum_value(GRN_DB_BOOL));
  mrb_define_const(mrb, id_module, "INT8",
                   mrb_fixnum_value(GRN_DB_INT8));
  mrb_define_const(mrb, id_module, "UINT8",
                   mrb_fixnum_value(GRN_DB_UINT8));
  mrb_define_const(mrb, id_module, "INT16",
                   mrb_fixnum_value(GRN_DB_INT16));
  mrb_define_const(mrb, id_module, "UINT16",
                   mrb_fixnum_value(GRN_DB_UINT16));
  mrb_define_const(mrb, id_module, "INT32",
                   mrb_fixnum_value(GRN_DB_INT32));
  mrb_define_const(mrb, id_module, "UINT32",
                   mrb_fixnum_value(GRN_DB_UINT32));
  mrb_define_const(mrb, id_module, "INT64",
                   mrb_fixnum_value(GRN_DB_INT64));
  mrb_define_const(mrb, id_module, "UINT64",
                   mrb_fixnum_value(GRN_DB_UINT64));
  mrb_define_const(mrb, id_module, "FLOAT",
                   mrb_fixnum_value(GRN_DB_FLOAT));
  mrb_define_const(mrb, id_module, "TIME",
                   mrb_fixnum_value(GRN_DB_TIME));
  mrb_define_const(mrb, id_module, "SHORT_TEXT",
                   mrb_fixnum_value(GRN_DB_SHORT_TEXT));
  mrb_define_const(mrb, id_module, "TEXT",
                   mrb_fixnum_value(GRN_DB_TEXT));
  mrb_define_const(mrb, id_module, "LONG_TEXT",
                   mrb_fixnum_value(GRN_DB_LONG_TEXT));
  mrb_define_const(mrb, id_module, "TOKYO_GEO_POINT",
                   mrb_fixnum_value(GRN_DB_TOKYO_GEO_POINT));
  mrb_define_const(mrb, id_module, "WGS84_GEO_POINT",
                   mrb_fixnum_value(GRN_DB_WGS84_GEO_POINT));
}
#endif
