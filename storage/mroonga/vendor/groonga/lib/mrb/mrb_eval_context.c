/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Brazil

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
#include <mruby/proc.h>
#include <mruby/compile.h>
#include <mruby/opcode.h>

#include "../grn_mrb.h"
#include "mrb_ctx.h"
#include "mrb_eval_context.h"

static mrb_value
eval_context_compile(mrb_state *mrb, mrb_value self)
{
  char *script;
  mrb_int script_length;
  mrbc_context* compile_ctx;
  struct mrb_parser_state *parser;
  struct RProc *proc;

  mrb_get_args(mrb, "s", &script, &script_length);

  compile_ctx = mrbc_context_new(mrb);
  if (!compile_ctx) {
    mrb_raise(mrb, E_RUNTIME_ERROR,
              "[mruby][eval][compile] failed to allocate context");
  }
  compile_ctx->capture_errors = TRUE;

  parser = mrb_parse_nstring(mrb, script, script_length, compile_ctx);
  if (!parser) {
    mrbc_context_free(mrb, compile_ctx);
    mrb_raise(mrb, E_RUNTIME_ERROR,
              "[mruby][eval][compile] failed to allocate parser");
  }
  if (parser->nerr > 0) {
    struct mrb_parser_message *error = &(parser->error_buffer[0]);
    mrb_value new_args[1];
    mrb_value exception;

    new_args[0] = mrb_format(mrb,
                             "line %S:%S: %S",
                             mrb_fixnum_value(error->lineno),
                             mrb_fixnum_value(error->column),
                             mrb_str_new_cstr(mrb, error->message));
    exception = mrb_obj_new(mrb, E_SYNTAX_ERROR, 1, new_args);
    mrb_parser_free(parser);
    mrbc_context_free(mrb, compile_ctx);

    mrb_exc_raise(mrb, exception);
  }

  proc = mrb_generate_code(mrb, parser);
  {
    mrb_code *iseq = proc->body.irep->iseq;
    while (GET_OPCODE(*iseq) != OP_STOP) {
      iseq++;
    }
    *iseq = MKOP_AB(OP_RETURN, 1, OP_R_NORMAL);
  }
  mrb_parser_free(parser);
  mrbc_context_free(mrb, compile_ctx);
  return mrb_obj_value(proc);
}

void
grn_mrb_eval_context_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "EvalContext", mrb->object_class);

  mrb_define_method(mrb, klass, "compile", eval_context_compile,
                    MRB_ARGS_REQ(1));
}
#endif
