/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2015 Brazil

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

#include "grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
# include <string.h>

# include "grn_ctx_impl_mrb.h"

# include "grn_mrb.h"
# include "mrb/mrb_converter.h"
# include "mrb/mrb_error.h"
# include "mrb/mrb_id.h"
# include "mrb/mrb_operator.h"
# include "mrb/mrb_ctx.h"
# include "mrb/mrb_logger.h"
# include "mrb/mrb_void.h"
# include "mrb/mrb_bulk.h"
# include "mrb/mrb_object.h"
# include "mrb/mrb_database.h"
# include "mrb/mrb_table.h"
# include "mrb/mrb_array.h"
# include "mrb/mrb_hash_table.h"
# include "mrb/mrb_patricia_trie.h"
# include "mrb/mrb_double_array_trie.h"
# include "mrb/mrb_column.h"
# include "mrb/mrb_fixed_size_column.h"
# include "mrb/mrb_variable_size_column.h"
# include "mrb/mrb_index_column.h"
# include "mrb/mrb_index_cursor.h"
# include "mrb/mrb_type.h"
# include "mrb/mrb_expr.h"
# include "mrb/mrb_accessor.h"
# include "mrb/mrb_procedure.h"
# include "mrb/mrb_command.h"
# include "mrb/mrb_command_input.h"
# include "mrb/mrb_table_cursor.h"
# include "mrb/mrb_table_cursor_flags.h"
# include "mrb/mrb_writer.h"

# include <mruby/array.h>
# include <mruby/variable.h>
#endif /* GRN_WITH_MRUBY */

#ifdef GRN_WITH_MRUBY
static mrb_value
mrb_kernel_load(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  char *path;

  mrb_get_args(mrb, "z", &path);

  grn_mrb_load(ctx, path);
  if (mrb->exc) {
    mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
  }

  grn_mrb_ctx_check(mrb);

  return mrb_true_value();
}

static void
grn_ctx_impl_mrb_init_bindings(grn_ctx *ctx)
{
  mrb_state *mrb = ctx->impl->mrb.state;

  mrb->ud = ctx;
  ctx->impl->mrb.module = mrb_define_module(mrb, "Groonga");

  mrb_define_class(mrb, "LoadError", mrb_class_get(mrb, "ScriptError"));
  mrb_define_method(mrb, mrb->kernel_module,
                    "load", mrb_kernel_load, MRB_ARGS_REQ(1));

  {
    mrb_value load_path;
    const char *plugins_dir;
    const char *system_ruby_scripts_dir;

    load_path = mrb_ary_new(mrb);
    plugins_dir = grn_plugin_get_system_plugins_dir();
    mrb_ary_push(mrb, load_path,
                 mrb_str_new_cstr(mrb, plugins_dir));
    system_ruby_scripts_dir = grn_mrb_get_system_ruby_scripts_dir(ctx);
    mrb_ary_push(mrb, load_path,
                 mrb_str_new_cstr(mrb, system_ruby_scripts_dir));
    mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$LOAD_PATH"), load_path);
  }

  grn_mrb_load(ctx, "require.rb");
  grn_mrb_load(ctx, "initialize/pre.rb");

  grn_mrb_converter_init(ctx);
  grn_mrb_error_init(ctx);
  grn_mrb_id_init(ctx);
  grn_mrb_operator_init(ctx);
  grn_mrb_ctx_init(ctx);
  grn_mrb_logger_init(ctx);
  grn_mrb_void_init(ctx);
  grn_mrb_bulk_init(ctx);
  grn_mrb_object_init(ctx);
  grn_mrb_database_init(ctx);
  grn_mrb_table_init(ctx);
  grn_mrb_array_init(ctx);
  grn_mrb_hash_table_init(ctx);
  grn_mrb_patricia_trie_init(ctx);
  grn_mrb_double_array_trie_init(ctx);
  grn_mrb_column_init(ctx);
  grn_mrb_fixed_size_column_init(ctx);
  grn_mrb_variable_size_column_init(ctx);
  grn_mrb_index_column_init(ctx);
  grn_mrb_index_cursor_init(ctx);
  grn_mrb_type_init(ctx);
  grn_mrb_expr_init(ctx);
  grn_mrb_accessor_init(ctx);
  grn_mrb_procedure_init(ctx);
  grn_mrb_command_init(ctx);
  grn_mrb_command_input_init(ctx);
  grn_mrb_table_cursor_init(ctx);
  grn_mrb_table_cursor_flags_init(ctx);
  grn_mrb_writer_init(ctx);

  grn_mrb_load(ctx, "initialize/post.rb");
}

void
grn_ctx_impl_mrb_init(grn_ctx *ctx)
{
  const char *grn_mruby_enabled;
  grn_mruby_enabled = getenv("GRN_MRUBY_ENABLED");
  if (grn_mruby_enabled && strcmp(grn_mruby_enabled, "no") == 0) {
    ctx->impl->mrb.state = NULL;
    ctx->impl->mrb.base_directory[0] = '\0';
    ctx->impl->mrb.module = NULL;
    ctx->impl->mrb.object_class = NULL;
    ctx->impl->mrb.checked_procs = NULL;
    ctx->impl->mrb.registered_plugins = NULL;
    ctx->impl->mrb.builtin.time_class = NULL;
  } else {
    mrb_state *mrb;

    mrb = mrb_open();
    ctx->impl->mrb.state = mrb;
    ctx->impl->mrb.base_directory[0] = '\0';
    grn_ctx_impl_mrb_init_bindings(ctx);
    /* TODO: Implement better error handling on init. */
    if (ctx->impl->mrb.state->exc) {
      mrb_print_error(mrb);
    }
    ctx->impl->mrb.checked_procs =
      grn_hash_create(ctx, NULL, sizeof(grn_id), 0, GRN_HASH_TINY);
    ctx->impl->mrb.registered_plugins =
      grn_hash_create(ctx, NULL, sizeof(grn_id), 0, GRN_HASH_TINY);
    GRN_VOID_INIT(&(ctx->impl->mrb.buffer.from));
    GRN_VOID_INIT(&(ctx->impl->mrb.buffer.to));
    ctx->impl->mrb.builtin.time_class = mrb_class_get(mrb, "Time");
  }
}

void
grn_ctx_impl_mrb_fin(grn_ctx *ctx)
{
  if (ctx->impl->mrb.state) {
    mrb_close(ctx->impl->mrb.state);
    ctx->impl->mrb.state = NULL;
    grn_hash_close(ctx, ctx->impl->mrb.checked_procs);
    grn_hash_close(ctx, ctx->impl->mrb.registered_plugins);
    GRN_OBJ_FIN(ctx, &(ctx->impl->mrb.buffer.from));
    GRN_OBJ_FIN(ctx, &(ctx->impl->mrb.buffer.to));
  }
}
#else /* GRN_WITH_MRUBY */
void
grn_ctx_impl_mrb_init(grn_ctx *ctx)
{
}

void
grn_ctx_impl_mrb_fin(grn_ctx *ctx)
{
}
#endif /* GRN_WITH_MRUBY */
