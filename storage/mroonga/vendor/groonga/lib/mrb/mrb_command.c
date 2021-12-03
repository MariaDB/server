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
#include <groonga/command.h>

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/value.h>
#include <mruby/string.h>

#include "mrb_ctx.h"
#include "mrb_command.h"

static struct mrb_data_type mrb_grn_command_type = {
  "Groonga::Command",
  NULL
};

mrb_value
mrb_grn_command_instantiate(grn_ctx *ctx, grn_obj *command)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_size;
  mrb_value mrb_name;
  struct RClass *command_class;
  struct RClass *target_command_class;
  mrb_value mrb_target_command_class;
  mrb_value mrb_arguments[1];

  name_size = grn_obj_name(ctx, command, name, GRN_TABLE_MAX_KEY_SIZE);
  mrb_name = mrb_str_new(mrb, name, name_size);

  command_class = mrb_class_get_under(mrb, module, "Command");
  mrb_target_command_class = mrb_funcall(mrb,
                                         mrb_obj_value(command_class),
                                         "find_class", 1, mrb_name);
  if (mrb_nil_p(mrb_target_command_class)) {
    target_command_class = command_class;
  } else {
    target_command_class = mrb_class_ptr(mrb_target_command_class);
  }
  mrb_arguments[0] = mrb_cptr_value(mrb, command);
  return mrb_obj_new(mrb, target_command_class, 1, mrb_arguments);
}

static void
mrb_grn_command_run_wrapper(grn_ctx *ctx,
                            grn_obj *command,
                            grn_command_input *input,
                            void *user_data)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  int arena_index;
  mrb_value mrb_command;
  mrb_value mrb_input;

  arena_index = mrb_gc_arena_save(mrb);
  mrb_command = mrb_grn_command_instantiate(ctx, command);
  {
    struct RClass *command_input_class;
    mrb_value mrb_arguments[1];
    command_input_class = mrb_class_get_under(mrb, module, "CommandInput");
    mrb_arguments[0] = mrb_cptr_value(mrb, input);
    mrb_input = mrb_obj_new(mrb, command_input_class, 1, mrb_arguments);
  }
  mrb_funcall(mrb, mrb_command, "run_internal", 1, mrb_input);
  if (ctx->rc == GRN_SUCCESS && mrb->exc) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size;
    name_size = grn_obj_name(ctx, command, name, GRN_TABLE_MAX_KEY_SIZE);
    if (mrb->exc == mrb->nomem_err) {
      MERR("failed to allocate memory in mruby: <%.*s>",
           name_size, name);
    } else {
      mrb_value reason;
      reason = mrb_funcall(mrb, mrb_obj_value(mrb->exc), "inspect", 0);
      ERR(GRN_COMMAND_ERROR,
          "failed to run command: <%*.s>: %.*s",
          name_size, name,
          (int)RSTRING_LEN(reason), RSTRING_PTR(reason));
    }
  }
  mrb_gc_arena_restore(mrb, arena_index);
}

static mrb_value
mrb_grn_command_class_register(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_name;
  mrb_value *mrb_arguments;
  mrb_int n_arguments;

  mrb_get_args(mrb, "Sa", &mrb_name, &mrb_arguments, &n_arguments);

  {
    grn_expr_var *vars;
    mrb_int i;

    for (i = 0; i < n_arguments; i++) {
      mrb_arguments[i] = mrb_convert_type(mrb, mrb_arguments[i],
                                          MRB_TT_STRING, "String", "to_str");
    }
    vars = GRN_MALLOCN(grn_expr_var, n_arguments);
    for (i = 0; i < n_arguments; i++) {
      mrb_value mrb_argument = mrb_arguments[i];
      grn_expr_var *var = &vars[i];
      var->name = RSTRING_PTR(mrb_argument);
      var->name_size = RSTRING_LEN(mrb_argument);
      GRN_TEXT_INIT(&(var->value), 0);
    }

    grn_command_register(ctx,
                         RSTRING_PTR(mrb_name),
                         RSTRING_LEN(mrb_name),
                         mrb_grn_command_run_wrapper,
                         vars,
                         n_arguments,
                         NULL);

    for (i = 0; i < n_arguments; i++) {
      grn_expr_var *var = &vars[i];
      GRN_OBJ_FIN(ctx, &(var->value));
    }
    GRN_FREE(vars);
  }

  grn_mrb_ctx_check(mrb);

  {
    grn_mrb_data *data = &(ctx->impl->mrb);
    struct RClass *command_class;
    command_class = mrb_class_get_under(mrb, data->module, "Command");
    mrb_funcall(mrb,
                mrb_obj_value(command_class),
                "register_class", 2, mrb_name, klass);
  }

  return mrb_nil_value();
}

static mrb_value
mrb_grn_command_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_command_ptr;

  mrb_get_args(mrb, "o", &mrb_command_ptr);
  DATA_TYPE(self) = &mrb_grn_command_type;
  DATA_PTR(self) = mrb_cptr(mrb_command_ptr);
  return self;
}

void
grn_mrb_command_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *procedure_class;
  struct RClass *klass;

  procedure_class = mrb_class_get_under(mrb, module, "Procedure");
  klass = mrb_define_class_under(mrb, module, "Command", procedure_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "register",
                          mrb_grn_command_class_register,
                          MRB_ARGS_REQ(2));

  mrb_define_method(mrb, klass, "initialize",
                    mrb_grn_command_initialize, MRB_ARGS_REQ(1));
}
#endif
