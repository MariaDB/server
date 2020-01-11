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

#include <string.h>

#include "grn.h"
#include "grn_db.h"
#include "grn_ctx_impl.h"

struct _grn_command_input {
  grn_obj *command;
  grn_hash *arguments;
};

grn_command_input *
grn_command_input_open(grn_ctx *ctx, grn_obj *command)
{
  grn_command_input *input = NULL;

  GRN_API_ENTER;
  input = GRN_MALLOC(sizeof(grn_command_input));
  if (!input) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[command-input] failed to allocate grn_command_input");
    goto exit;
  }

  input->command = command;
  /* TODO: Allocate by self. */
  {
    uint32_t n;
    input->arguments = grn_expr_get_vars(ctx, input->command, &n);
  }

exit :
  GRN_API_RETURN(input);
}

grn_rc
grn_command_input_close(grn_ctx *ctx, grn_command_input *input)
{
  GRN_API_ENTER;

  /* TODO: Free input->arguments by self. */
  /* grn_expr_clear_vars(ctx, input->command); */
  GRN_FREE(input);

  GRN_API_RETURN(ctx->rc);
}

grn_obj *
grn_command_input_add(grn_ctx *ctx,
                      grn_command_input *input,
                      const char *name,
                      int name_size,
                      grn_bool *added)
{
  grn_obj *argument = NULL;
  /* TODO: Use grn_bool */
  int internal_added = GRN_FALSE;

  GRN_API_ENTER;

  if (name_size == -1) {
    name_size = strlen(name);
  }
  if (input->arguments) {
    grn_hash_add(ctx, input->arguments, name, name_size, (void **)&argument,
                 &internal_added);
    if (internal_added) {
      GRN_TEXT_INIT(argument, 0);
    }
  }
  if (added) {
    *added = internal_added;
  }

  GRN_API_RETURN(argument);
}

grn_obj *
grn_command_input_get(grn_ctx *ctx,
                      grn_command_input *input,
                      const char *name,
                      int name_size)
{
  grn_obj *argument = NULL;

  GRN_API_ENTER;

  if (name_size == -1) {
    name_size = strlen(name);
  }
  if (input->arguments) {
    grn_hash_get(ctx, input->arguments, name, name_size, (void **)&argument);
  }

  GRN_API_RETURN(argument);
}

grn_obj *
grn_command_input_at(grn_ctx *ctx,
                     grn_command_input *input,
                     unsigned int offset)
{
  grn_obj *argument = NULL;

  GRN_API_ENTER;
  if (input->arguments) {
    argument = (grn_obj *)grn_hash_get_value_(ctx, input->arguments,
                                              offset + 1, NULL);
  }
  GRN_API_RETURN(argument);
}

grn_obj *
grn_command_input_get_arguments(grn_ctx *ctx,
                                grn_command_input *input)
{
  GRN_API_ENTER;
  GRN_API_RETURN((grn_obj *)(input->arguments));
}

grn_rc
grn_command_register(grn_ctx *ctx,
                     const char *command_name,
                     int command_name_size,
                     grn_command_run_func *run,
                     grn_expr_var *vars,
                     unsigned int n_vars,
                     void *user_data)
{
  GRN_API_ENTER;

  if (command_name_size == -1) {
    command_name_size = strlen(command_name);
  }

  {
    grn_obj *command_object;
    command_object = grn_proc_create(ctx,
                                     command_name,
                                     command_name_size,
                                     GRN_PROC_COMMAND,
                                     NULL, NULL, NULL, n_vars, vars);
    if (!command_object) {
      GRN_PLUGIN_ERROR(ctx, GRN_COMMAND_ERROR,
                       "[command][%.*s] failed to grn_proc_create()",
                       command_name_size, command_name);
      GRN_API_RETURN(ctx->rc);
    }

    {
      grn_proc *command = (grn_proc *)command_object;
      command->callbacks.command.run = run;
      command->user_data = user_data;
    }
  }

  GRN_API_RETURN(GRN_SUCCESS);
}

grn_rc
grn_command_run(grn_ctx *ctx,
                grn_obj *command,
                grn_command_input *input)
{
  grn_proc *proc;

  GRN_API_ENTER;

  proc = (grn_proc *)command;
  if (proc->callbacks.command.run) {
    proc->callbacks.command.run(ctx, command, input, proc->user_data);
  } else {
    /* TODO: REMOVE ME. For backward compatibility. */
    uint32_t stack_curr = ctx->impl->stack_curr;
    grn_proc_call(ctx, command, 0, command);
    if (ctx->impl->stack_curr > stack_curr) {
      grn_ctx_pop(ctx);
    }
  }

  GRN_API_RETURN(ctx->rc);
}

