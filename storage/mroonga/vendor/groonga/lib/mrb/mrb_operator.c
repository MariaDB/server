/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014-2015 Brazil

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

#include "mrb_operator.h"

mrb_value
grn_mrb_value_from_operator(mrb_state *mrb, grn_operator op)
{
  grn_ctx *ctx = (grn_ctx *)(mrb->ud);
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_value mrb_op_raw;
  mrb_value mrb_op;

  mrb_op_raw = mrb_fixnum_value(op);
  mrb_op = mrb_funcall(mrb, mrb_obj_value(data->groonga.operator_class),
                       "find", 1, mrb_op_raw);
  if (mrb_nil_p(mrb_op)) {
    return mrb_op_raw;
  } else {
    return mrb_op;
  }
}

grn_operator
grn_mrb_value_to_operator(mrb_state *mrb, mrb_value mrb_op)
{
  if (!mrb_fixnum_p(mrb_op)) {
    mrb_op = mrb_funcall(mrb, mrb_op, "value", 0);
  }

  return mrb_fixnum(mrb_op);
}

void
grn_mrb_operator_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = ctx->impl->mrb.module;
  struct RClass *klass;
  mrb_value klass_obj;

  klass = mrb_class_get_under(mrb, module, "Operator");
  data->groonga.operator_class = klass;

  klass_obj = mrb_obj_value(klass);
#define DEFINE_OPERATOR(name)                                   \
  mrb_funcall(mrb, klass_obj, "register", 1,                    \
              mrb_funcall(mrb, klass_obj, "new", 2,             \
                          mrb_str_new_lit(mrb, #name),          \
                          mrb_fixnum_value(GRN_OP_ ## name)))

  DEFINE_OPERATOR(PUSH);
  DEFINE_OPERATOR(POP);
  DEFINE_OPERATOR(NOP);
  DEFINE_OPERATOR(CALL);
  DEFINE_OPERATOR(INTERN);
  DEFINE_OPERATOR(GET_REF);
  DEFINE_OPERATOR(GET_VALUE);
  DEFINE_OPERATOR(AND);
  DEFINE_OPERATOR(AND_NOT);
  DEFINE_OPERATOR(OR);
  DEFINE_OPERATOR(ASSIGN);
  DEFINE_OPERATOR(STAR_ASSIGN);
  DEFINE_OPERATOR(SLASH_ASSIGN);
  DEFINE_OPERATOR(MOD_ASSIGN);
  DEFINE_OPERATOR(PLUS_ASSIGN);
  DEFINE_OPERATOR(MINUS_ASSIGN);
  DEFINE_OPERATOR(SHIFTL_ASSIGN);
  DEFINE_OPERATOR(SHIFTR_ASSIGN);
  DEFINE_OPERATOR(SHIFTRR_ASSIGN);
  DEFINE_OPERATOR(AND_ASSIGN);
  DEFINE_OPERATOR(XOR_ASSIGN);
  DEFINE_OPERATOR(OR_ASSIGN);
  DEFINE_OPERATOR(JUMP);
  DEFINE_OPERATOR(CJUMP);
  DEFINE_OPERATOR(COMMA);
  DEFINE_OPERATOR(BITWISE_OR);
  DEFINE_OPERATOR(BITWISE_XOR);
  DEFINE_OPERATOR(BITWISE_AND);
  DEFINE_OPERATOR(BITWISE_NOT);
  DEFINE_OPERATOR(EQUAL);
  DEFINE_OPERATOR(NOT_EQUAL);
  DEFINE_OPERATOR(LESS);
  DEFINE_OPERATOR(GREATER);
  DEFINE_OPERATOR(LESS_EQUAL);
  DEFINE_OPERATOR(GREATER_EQUAL);
  DEFINE_OPERATOR(IN);
  DEFINE_OPERATOR(MATCH);
  DEFINE_OPERATOR(NEAR);
  DEFINE_OPERATOR(NEAR2);
  DEFINE_OPERATOR(SIMILAR);
  DEFINE_OPERATOR(TERM_EXTRACT);
  DEFINE_OPERATOR(SHIFTL);
  DEFINE_OPERATOR(SHIFTR);
  DEFINE_OPERATOR(SHIFTRR);
  DEFINE_OPERATOR(PLUS);
  DEFINE_OPERATOR(MINUS);
  DEFINE_OPERATOR(STAR);
  DEFINE_OPERATOR(SLASH);
  DEFINE_OPERATOR(MOD);
  DEFINE_OPERATOR(DELETE);
  DEFINE_OPERATOR(INCR);
  DEFINE_OPERATOR(DECR);
  DEFINE_OPERATOR(INCR_POST);
  DEFINE_OPERATOR(DECR_POST);
  DEFINE_OPERATOR(NOT);
  DEFINE_OPERATOR(ADJUST);
  DEFINE_OPERATOR(EXACT);
  DEFINE_OPERATOR(LCP);
  DEFINE_OPERATOR(PARTIAL);
  DEFINE_OPERATOR(UNSPLIT);
  DEFINE_OPERATOR(PREFIX);
  DEFINE_OPERATOR(SUFFIX);
  DEFINE_OPERATOR(GEO_DISTANCE1);
  DEFINE_OPERATOR(GEO_DISTANCE2);
  DEFINE_OPERATOR(GEO_DISTANCE3);
  DEFINE_OPERATOR(GEO_DISTANCE4);
  DEFINE_OPERATOR(GEO_WITHINP5);
  DEFINE_OPERATOR(GEO_WITHINP6);
  DEFINE_OPERATOR(GEO_WITHINP8);
  DEFINE_OPERATOR(OBJ_SEARCH);
  DEFINE_OPERATOR(EXPR_GET_VAR);
  DEFINE_OPERATOR(TABLE_CREATE);
  DEFINE_OPERATOR(TABLE_SELECT);
  DEFINE_OPERATOR(TABLE_SORT);
  DEFINE_OPERATOR(TABLE_GROUP);
  DEFINE_OPERATOR(JSON_PUT);
  DEFINE_OPERATOR(GET_MEMBER);
  DEFINE_OPERATOR(REGEXP);
  DEFINE_OPERATOR(FUZZY);

#undef DEFINE_OPERATOR
}
#endif
