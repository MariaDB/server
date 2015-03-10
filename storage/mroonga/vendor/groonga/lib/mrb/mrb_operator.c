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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>

#include "mrb_operator.h"

void
grn_mrb_operator_init(grn_ctx *ctx)
{
  mrb_state *mrb = ctx->impl->mrb.state;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *operator_module;

  operator_module = mrb_define_module_under(mrb, module, "Operator");

  mrb_define_const(mrb, operator_module, "PUSH",
                   mrb_fixnum_value(GRN_OP_PUSH));
  mrb_define_const(mrb, operator_module, "POP",
                   mrb_fixnum_value(GRN_OP_POP));
  mrb_define_const(mrb, operator_module, "NOP",
                   mrb_fixnum_value(GRN_OP_NOP));
  mrb_define_const(mrb, operator_module, "CALL",
                   mrb_fixnum_value(GRN_OP_CALL));
  mrb_define_const(mrb, operator_module, "INTERN",
                   mrb_fixnum_value(GRN_OP_INTERN));
  mrb_define_const(mrb, operator_module, "GET_REF",
                   mrb_fixnum_value(GRN_OP_GET_REF));
  mrb_define_const(mrb, operator_module, "GET_VALUE",
                   mrb_fixnum_value(GRN_OP_GET_VALUE));
  mrb_define_const(mrb, operator_module, "AND",
                   mrb_fixnum_value(GRN_OP_AND));
  mrb_define_const(mrb, operator_module, "AND_NOT",
                   mrb_fixnum_value(GRN_OP_AND_NOT));
  mrb_define_const(mrb, operator_module, "OR",
                   mrb_fixnum_value(GRN_OP_OR));
  mrb_define_const(mrb, operator_module, "ASSIGN",
                   mrb_fixnum_value(GRN_OP_ASSIGN));
  mrb_define_const(mrb, operator_module, "STAR_ASSIGN",
                   mrb_fixnum_value(GRN_OP_STAR_ASSIGN));
  mrb_define_const(mrb, operator_module, "SLASH_ASSIGN",
                   mrb_fixnum_value(GRN_OP_SLASH_ASSIGN));
  mrb_define_const(mrb, operator_module, "MOD_ASSIGN",
                   mrb_fixnum_value(GRN_OP_MOD_ASSIGN));
  mrb_define_const(mrb, operator_module, "PLUS_ASSIGN",
                   mrb_fixnum_value(GRN_OP_PLUS_ASSIGN));
  mrb_define_const(mrb, operator_module, "MINUS_ASSIGN",
                   mrb_fixnum_value(GRN_OP_MINUS_ASSIGN));
  mrb_define_const(mrb, operator_module, "SHIFTL_ASSIGN",
                   mrb_fixnum_value(GRN_OP_SHIFTL_ASSIGN));
  mrb_define_const(mrb, operator_module, "SHIFTR_ASSIGN",
                   mrb_fixnum_value(GRN_OP_SHIFTR_ASSIGN));
  mrb_define_const(mrb, operator_module, "SHIFTRR_ASSIGN",
                   mrb_fixnum_value(GRN_OP_SHIFTRR_ASSIGN));
  mrb_define_const(mrb, operator_module, "AND_ASSIGN",
                   mrb_fixnum_value(GRN_OP_AND_ASSIGN));
  mrb_define_const(mrb, operator_module, "XOR_ASSIGN",
                   mrb_fixnum_value(GRN_OP_XOR_ASSIGN));
  mrb_define_const(mrb, operator_module, "OR_ASSIGN",
                   mrb_fixnum_value(GRN_OP_OR_ASSIGN));
  mrb_define_const(mrb, operator_module, "JUMP",
                   mrb_fixnum_value(GRN_OP_JUMP));
  mrb_define_const(mrb, operator_module, "CJUMP",
                   mrb_fixnum_value(GRN_OP_CJUMP));
  mrb_define_const(mrb, operator_module, "COMMA",
                   mrb_fixnum_value(GRN_OP_COMMA));
  mrb_define_const(mrb, operator_module, "BITWISE_OR",
                   mrb_fixnum_value(GRN_OP_BITWISE_OR));
  mrb_define_const(mrb, operator_module, "BITWISE_XOR",
                   mrb_fixnum_value(GRN_OP_BITWISE_XOR));
  mrb_define_const(mrb, operator_module, "BITWISE_AND",
                   mrb_fixnum_value(GRN_OP_BITWISE_AND));
  mrb_define_const(mrb, operator_module, "BITWISE_NOT",
                   mrb_fixnum_value(GRN_OP_BITWISE_NOT));
  mrb_define_const(mrb, operator_module, "EQUAL",
                   mrb_fixnum_value(GRN_OP_EQUAL));
  mrb_define_const(mrb, operator_module, "NOT_EQUAL",
                   mrb_fixnum_value(GRN_OP_NOT_EQUAL));
  mrb_define_const(mrb, operator_module, "LESS",
                   mrb_fixnum_value(GRN_OP_LESS));
  mrb_define_const(mrb, operator_module, "GREATER",
                   mrb_fixnum_value(GRN_OP_GREATER));
  mrb_define_const(mrb, operator_module, "LESS_EQUAL",
                   mrb_fixnum_value(GRN_OP_LESS_EQUAL));
  mrb_define_const(mrb, operator_module, "GREATER_EQUAL",
                   mrb_fixnum_value(GRN_OP_GREATER_EQUAL));
  mrb_define_const(mrb, operator_module, "IN",
                   mrb_fixnum_value(GRN_OP_IN));
  mrb_define_const(mrb, operator_module, "MATCH",
                   mrb_fixnum_value(GRN_OP_MATCH));
  mrb_define_const(mrb, operator_module, "NEAR",
                   mrb_fixnum_value(GRN_OP_NEAR));
  mrb_define_const(mrb, operator_module, "NEAR2",
                   mrb_fixnum_value(GRN_OP_NEAR2));
  mrb_define_const(mrb, operator_module, "SIMILAR",
                   mrb_fixnum_value(GRN_OP_SIMILAR));
  mrb_define_const(mrb, operator_module, "TERM_EXTRACT",
                   mrb_fixnum_value(GRN_OP_TERM_EXTRACT));
  mrb_define_const(mrb, operator_module, "SHIFTL",
                   mrb_fixnum_value(GRN_OP_SHIFTL));
  mrb_define_const(mrb, operator_module, "SHIFTR",
                   mrb_fixnum_value(GRN_OP_SHIFTR));
  mrb_define_const(mrb, operator_module, "SHIFTRR",
                   mrb_fixnum_value(GRN_OP_SHIFTRR));
  mrb_define_const(mrb, operator_module, "PLUS",
                   mrb_fixnum_value(GRN_OP_PLUS));
  mrb_define_const(mrb, operator_module, "MINUS",
                   mrb_fixnum_value(GRN_OP_MINUS));
  mrb_define_const(mrb, operator_module, "STAR",
                   mrb_fixnum_value(GRN_OP_STAR));
  mrb_define_const(mrb, operator_module, "SLASH",
                   mrb_fixnum_value(GRN_OP_SLASH));
  mrb_define_const(mrb, operator_module, "MOD",
                   mrb_fixnum_value(GRN_OP_MOD));
  mrb_define_const(mrb, operator_module, "DELETE",
                   mrb_fixnum_value(GRN_OP_DELETE));
  mrb_define_const(mrb, operator_module, "INCR",
                   mrb_fixnum_value(GRN_OP_INCR));
  mrb_define_const(mrb, operator_module, "DECR",
                   mrb_fixnum_value(GRN_OP_DECR));
  mrb_define_const(mrb, operator_module, "INCR_POST",
                   mrb_fixnum_value(GRN_OP_INCR_POST));
  mrb_define_const(mrb, operator_module, "DECR_POST",
                   mrb_fixnum_value(GRN_OP_DECR_POST));
  mrb_define_const(mrb, operator_module, "NOT",
                   mrb_fixnum_value(GRN_OP_NOT));
  mrb_define_const(mrb, operator_module, "ADJUST",
                   mrb_fixnum_value(GRN_OP_ADJUST));
  mrb_define_const(mrb, operator_module, "EXACT",
                   mrb_fixnum_value(GRN_OP_EXACT));
  mrb_define_const(mrb, operator_module, "LCP",
                   mrb_fixnum_value(GRN_OP_LCP));
  mrb_define_const(mrb, operator_module, "PARTIAL",
                   mrb_fixnum_value(GRN_OP_PARTIAL));
  mrb_define_const(mrb, operator_module, "UNSPLIT",
                   mrb_fixnum_value(GRN_OP_UNSPLIT));
  mrb_define_const(mrb, operator_module, "PREFIX",
                   mrb_fixnum_value(GRN_OP_PREFIX));
  mrb_define_const(mrb, operator_module, "SUFFIX",
                   mrb_fixnum_value(GRN_OP_SUFFIX));
  mrb_define_const(mrb, operator_module, "GEO_DISTANCE1",
                   mrb_fixnum_value(GRN_OP_GEO_DISTANCE1));
  mrb_define_const(mrb, operator_module, "GEO_DISTANCE2",
                   mrb_fixnum_value(GRN_OP_GEO_DISTANCE2));
  mrb_define_const(mrb, operator_module, "GEO_DISTANCE3",
                   mrb_fixnum_value(GRN_OP_GEO_DISTANCE3));
  mrb_define_const(mrb, operator_module, "GEO_DISTANCE4",
                   mrb_fixnum_value(GRN_OP_GEO_DISTANCE4));
  mrb_define_const(mrb, operator_module, "GEO_WITHINP5",
                   mrb_fixnum_value(GRN_OP_GEO_WITHINP5));
  mrb_define_const(mrb, operator_module, "GEO_WITHINP6",
                   mrb_fixnum_value(GRN_OP_GEO_WITHINP6));
  mrb_define_const(mrb, operator_module, "GEO_WITHINP8",
                   mrb_fixnum_value(GRN_OP_GEO_WITHINP8));
  mrb_define_const(mrb, operator_module, "OBJ_SEARCH",
                   mrb_fixnum_value(GRN_OP_OBJ_SEARCH));
  mrb_define_const(mrb, operator_module, "EXPR_GET_VAR",
                   mrb_fixnum_value(GRN_OP_EXPR_GET_VAR));
  mrb_define_const(mrb, operator_module, "TABLE_CREATE",
                   mrb_fixnum_value(GRN_OP_TABLE_CREATE));
  mrb_define_const(mrb, operator_module, "TABLE_SELECT",
                   mrb_fixnum_value(GRN_OP_TABLE_SELECT));
  mrb_define_const(mrb, operator_module, "TABLE_SORT",
                   mrb_fixnum_value(GRN_OP_TABLE_SORT));
  mrb_define_const(mrb, operator_module, "TABLE_GROUP",
                   mrb_fixnum_value(GRN_OP_TABLE_GROUP));
  mrb_define_const(mrb, operator_module, "JSON_PUT",
                   mrb_fixnum_value(GRN_OP_JSON_PUT));
  mrb_define_const(mrb, operator_module, "GET_MEMBER",
                   mrb_fixnum_value(GRN_OP_GET_MEMBER));
}
#endif
