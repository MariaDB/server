/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2017 Brazil

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

#include "grn_ctx_impl.h"

#include <string.h>

#ifdef GRN_WITH_MRUBY
# include "grn_ctx_impl_mrb.h"

# include "grn_mrb.h"
# include "mrb/mrb_converter.h"
# include "mrb/mrb_error.h"
# include "mrb/mrb_id.h"
# include "mrb/mrb_operator.h"
# include "mrb/mrb_command_version.h"
# include "mrb/mrb_ctx.h"
# include "mrb/mrb_logger.h"
# include "mrb/mrb_query_logger.h"
# include "mrb/mrb_void.h"
# include "mrb/mrb_bulk.h"
# include "mrb/mrb_pointer.h"
# include "mrb/mrb_cache.h"
# include "mrb/mrb_object.h"
# include "mrb/mrb_object_flags.h"
# include "mrb/mrb_database.h"
# include "mrb/mrb_indexable.h"
# include "mrb/mrb_table.h"
# include "mrb/mrb_array.h"
# include "mrb/mrb_hash_table.h"
# include "mrb/mrb_patricia_trie.h"
# include "mrb/mrb_double_array_trie.h"
# include "mrb/mrb_table_group_flags.h"
# include "mrb/mrb_table_group_result.h"
# include "mrb/mrb_table_sort_flags.h"
# include "mrb/mrb_table_sort_key.h"
# include "mrb/mrb_record.h"
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
# include "mrb/mrb_content_type.h"
# include "mrb/mrb_writer.h"
# include "mrb/mrb_config.h"
# include "mrb/mrb_eval_context.h"
# include "mrb/mrb_thread.h"
# include "mrb/mrb_window_definition.h"

# include <mruby/array.h>
# include <mruby/string.h>
# include <mruby/variable.h>
#endif /* GRN_WITH_MRUBY */

static grn_bool grn_ctx_impl_mrb_mruby_enabled = GRN_TRUE;

void
grn_ctx_impl_mrb_init_from_env(void)
{
  {
    char grn_mruby_enabled_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_MRUBY_ENABLED",
               grn_mruby_enabled_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_mruby_enabled_env[0] &&
        strcmp(grn_mruby_enabled_env, "no") == 0) {
      grn_ctx_impl_mrb_mruby_enabled = GRN_FALSE;
    }
  }
}

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

static mrb_value
mrb_groonga_init(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = mrb->ud;

  mrb_undef_class_method(mrb, ctx->impl->mrb.module, "init");

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
  grn_mrb_command_version_init(ctx);
  grn_mrb_ctx_init(ctx);
  grn_mrb_logger_init(ctx);
  grn_mrb_query_logger_init(ctx);
  grn_mrb_void_init(ctx);
  grn_mrb_bulk_init(ctx);
  grn_mrb_pointer_init(ctx);
  grn_mrb_cache_init(ctx);
  grn_mrb_object_init(ctx);
  grn_mrb_object_flags_init(ctx);
  grn_mrb_database_init(ctx);
  grn_mrb_indexable_init(ctx);
  grn_mrb_table_init(ctx);
  grn_mrb_array_init(ctx);
  grn_mrb_hash_table_init(ctx);
  grn_mrb_patricia_trie_init(ctx);
  grn_mrb_double_array_trie_init(ctx);
  grn_mrb_table_group_flags_init(ctx);
  grn_mrb_table_group_result_init(ctx);
  grn_mrb_table_sort_flags_init(ctx);
  grn_mrb_table_sort_key_init(ctx);
  grn_mrb_record_init(ctx);
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
  grn_mrb_content_type_init(ctx);
  grn_mrb_writer_init(ctx);
  grn_mrb_config_init(ctx);
  grn_mrb_eval_context_init(ctx);
  grn_mrb_thread_init(ctx);
  grn_mrb_window_definition_init(ctx);

  grn_mrb_load(ctx, "initialize/post.rb");

  return mrb_nil_value();
}

static void
grn_ctx_impl_mrb_init_bindings(grn_ctx *ctx)
{
  mrb_state *mrb = ctx->impl->mrb.state;

  mrb->ud = ctx;
  ctx->impl->mrb.module = mrb_define_module(mrb, "Groonga");
  mrb_define_const(mrb,
                   ctx->impl->mrb.module,
                   "ORDER_BY_ESTIMATED_SIZE",
                   grn_mrb_is_order_by_estimated_size_enabled() ?
                   mrb_true_value() :
                   mrb_false_value());
  mrb_define_class_method(mrb, ctx->impl->mrb.module,
                          "init", mrb_groonga_init, MRB_ARGS_NONE());
  mrb_funcall(mrb, mrb_obj_value(ctx->impl->mrb.module), "init", 0);
}

#ifndef USE_MEMORY_DEBUG
static void *
grn_ctx_impl_mrb_allocf(mrb_state *mrb, void *ptr, size_t size, void *ud)
{
  grn_ctx *ctx = ud;

  if (size == 0) {
    if (ptr) {
      grn_free(ctx, ptr, __FILE__, __LINE__, __FUNCTION__);
    }
    return NULL;
  } else {
    if (ptr) {
      return grn_realloc(ctx, ptr, size, __FILE__, __LINE__, __FUNCTION__);
    } else {
      return grn_malloc(ctx, size, __FILE__, __LINE__, __FUNCTION__);
    }
  }
}
#endif /* USE_MEMORY_DEBUG */

static void
grn_ctx_impl_mrb_init_lazy(grn_ctx *ctx)
{
  if (!grn_ctx_impl_mrb_mruby_enabled) {
    ctx->impl->mrb.state = NULL;
    ctx->impl->mrb.base_directory[0] = '\0';
    ctx->impl->mrb.module = NULL;
    ctx->impl->mrb.object_class = NULL;
    ctx->impl->mrb.checked_procs = NULL;
    ctx->impl->mrb.registered_plugins = NULL;
    ctx->impl->mrb.builtin.time_class = NULL;
    ctx->impl->mrb.groonga.operator_class = NULL;
  } else {
    mrb_state *mrb;
#ifdef USE_MEMORY_DEBUG
    mrb = mrb_open();
#else /* USE_MEMORY_DEBUG */
    mrb = mrb_open_allocf(grn_ctx_impl_mrb_allocf, ctx);
#endif /* USE_MEMORY_DEBUG */
    ctx->impl->mrb.state = mrb;
    ctx->impl->mrb.base_directory[0] = '\0';
    grn_ctx_impl_mrb_init_bindings(ctx);
    if (ctx->impl->mrb.state->exc) {
      mrb_value reason;
      reason = mrb_funcall(mrb, mrb_obj_value(mrb->exc), "inspect", 0);
      ERR(GRN_UNKNOWN_ERROR,
          "failed to initialize mruby: %.*s",
          (int)RSTRING_LEN(reason),
          RSTRING_PTR(reason));
      mrb_close(ctx->impl->mrb.state);
      ctx->impl->mrb.state = NULL;
    } else {
      ctx->impl->mrb.checked_procs =
        grn_hash_create(ctx, NULL, sizeof(grn_id), 0, GRN_HASH_TINY);
      ctx->impl->mrb.registered_plugins =
        grn_hash_create(ctx, NULL, sizeof(grn_id), 0, GRN_HASH_TINY);
      GRN_VOID_INIT(&(ctx->impl->mrb.buffer.from));
      GRN_VOID_INIT(&(ctx->impl->mrb.buffer.to));
      ctx->impl->mrb.builtin.time_class = mrb_class_get(mrb, "Time");
    }
  }
}

static void
grn_ctx_impl_mrb_fin_real(grn_ctx *ctx)
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
static void
grn_ctx_impl_mrb_init_lazy(grn_ctx *ctx)
{
}

static void
grn_ctx_impl_mrb_fin_real(grn_ctx *ctx)
{
}
#endif /* GRN_WITH_MRUBY */

void
grn_ctx_impl_mrb_init(grn_ctx *ctx)
{
  ctx->impl->mrb.initialized = GRN_FALSE;
}

void
grn_ctx_impl_mrb_fin(grn_ctx *ctx)
{
  if (!ctx->impl->mrb.initialized) {
    return;
  }

  ctx->impl->mrb.initialized = GRN_FALSE;
  grn_ctx_impl_mrb_fin_real(ctx);
}

void
grn_ctx_impl_mrb_ensure_init(grn_ctx *ctx)
{
  if (!ctx->impl->mrb.initialized) {
    ctx->impl->mrb.initialized = GRN_TRUE;
    grn_ctx_impl_mrb_init_lazy(ctx);
  }
}
