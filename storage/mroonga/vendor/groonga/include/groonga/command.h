/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#pragma once

#include <groonga/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

typedef struct _grn_command_input grn_command_input;

GRN_PLUGIN_EXPORT grn_command_input *grn_command_input_open(grn_ctx *ctx,
                                                            grn_obj *command);
GRN_PLUGIN_EXPORT grn_rc grn_command_input_close(grn_ctx *ctx,
                                                 grn_command_input *input);

GRN_PLUGIN_EXPORT grn_obj *grn_command_input_add(grn_ctx *ctx,
                                                 grn_command_input *input,
                                                 const char *name,
                                                 int name_size,
                                                 grn_bool *added);
GRN_PLUGIN_EXPORT grn_obj *grn_command_input_get(grn_ctx *ctx,
                                                 grn_command_input *input,
                                                 const char *name,
                                                 int name_size);
GRN_PLUGIN_EXPORT grn_obj *grn_command_input_at(grn_ctx *ctx,
                                                grn_command_input *input,
                                                unsigned int offset);
GRN_PLUGIN_EXPORT grn_obj *grn_command_input_get_arguments(grn_ctx *ctx,
                                                           grn_command_input *input);

typedef void grn_command_run_func(grn_ctx *ctx,
                                  grn_obj *command,
                                  grn_command_input *input,
                                  void *user_data);

/*
  grn_command_register() registers a command to the database which is
  associated with `ctx'. `command_name' and `command_name_size'
  specify the command name. Alphabetic letters ('A'-'Z' and 'a'-'z'),
  digits ('0'-'9') and an underscore ('_') are capable characters.

  `run' is called for running the command.

  grn_command_register() returns GRN_SUCCESS on success, an error
  code on failure.
 */
GRN_PLUGIN_EXPORT grn_rc grn_command_register(grn_ctx *ctx,
                                              const char *command_name,
                                              int command_name_size,
                                              grn_command_run_func *run,
                                              grn_expr_var *vars,
                                              unsigned int n_vars,
                                              void *user_data);

GRN_PLUGIN_EXPORT grn_rc grn_command_run(grn_ctx *ctx,
                                         grn_obj *command,
                                         grn_command_input *input);

#ifdef __cplusplus
}  /* extern "C" */
#endif  /* __cplusplus */
