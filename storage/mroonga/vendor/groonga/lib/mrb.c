/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013 Brazil

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

#include "grn_mrb.h"
#include "grn_ctx_impl.h"
#include "grn_util.h"

#ifdef GRN_WITH_MRUBY
# include <mruby/proc.h>
# include <mruby/compile.h>
# include <mruby/string.h>
#endif

#include <ctype.h>

#define BUFFER_SIZE 2048
#define E_LOAD_ERROR (mrb_class_get(mrb, "LoadError"))

#ifdef GRN_WITH_MRUBY
# ifdef WIN32
static char *win32_ruby_scripts_dir = NULL;
static char win32_ruby_scripts_dir_buffer[PATH_MAX];
static const char *
grn_mrb_get_default_system_ruby_scripts_dir(void)
{
  if (!win32_ruby_scripts_dir) {
    const char *base_dir;
    const char *relative_path = GRN_RELATIVE_RUBY_SCRIPTS_DIR;
    size_t base_dir_length;

    base_dir = grn_win32_base_dir();
    base_dir_length = strlen(base_dir);
    strcpy(win32_ruby_scripts_dir_buffer, base_dir);
    strcat(win32_ruby_scripts_dir_buffer, "/");
    strcat(win32_ruby_scripts_dir_buffer, relative_path);
    win32_ruby_scripts_dir = win32_ruby_scripts_dir_buffer;
  }
  return win32_ruby_scripts_dir;
}

# else /* WIN32 */
static const char *
grn_mrb_get_default_system_ruby_scripts_dir(void)
{
  return GRN_RUBY_SCRIPTS_DIR;
}
# endif /* WIN32 */

const char *
grn_mrb_get_system_ruby_scripts_dir(grn_ctx *ctx)
{
  const char *ruby_scripts_dir;

  ruby_scripts_dir = getenv("GRN_RUBY_SCRIPTS_DIR");
  if (!ruby_scripts_dir) {
    ruby_scripts_dir = grn_mrb_get_default_system_ruby_scripts_dir();
  }

  return ruby_scripts_dir;
}

static grn_bool
grn_mrb_is_absolute_path(const char *path)
{
  if (path[0] == '/') {
    return GRN_TRUE;
  }

  if (isalpha(path[0]) && path[1] == ':' && path[2] == '/') {
    return GRN_TRUE;
  }

  return GRN_FALSE;
}

static grn_bool
grn_mrb_expand_script_path(grn_ctx *ctx, const char *path, char *expanded_path)
{
  const char *ruby_scripts_dir;
  char dir_last_char;
  int path_length, max_path_length;

  if (grn_mrb_is_absolute_path(path)) {
    expanded_path[0] = '\0';
  } else if (path[0] == '.' && path[1] == '/') {
    strcpy(expanded_path, ctx->impl->mrb.base_directory);
    strcat(expanded_path, "/");
  } else {
    ruby_scripts_dir = grn_mrb_get_system_ruby_scripts_dir(ctx);
    strcpy(expanded_path, ruby_scripts_dir);

    dir_last_char = ruby_scripts_dir[strlen(expanded_path) - 1];
    if (dir_last_char != '/') {
      strcat(expanded_path, "/");
    }
  }

  path_length = strlen(path);
  max_path_length = PATH_MAX - strlen(expanded_path) - 1;
  if (path_length > max_path_length) {
    ERR(GRN_INVALID_ARGUMENT,
        "script path is too long: %d (max: %d) <%s%s>",
        path_length, max_path_length,
        expanded_path, path);
    return GRN_FALSE;
  }

  strcat(expanded_path, path);

  return GRN_TRUE;
}

mrb_value
grn_mrb_load(grn_ctx *ctx, const char *path)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  char expanded_path[PATH_MAX];
  FILE *file;
  mrb_value result;
  struct mrb_parser_state *parser;

  if (!mrb) {
    return mrb_nil_value();
  }

  if (!grn_mrb_expand_script_path(ctx, path, expanded_path)) {
    return mrb_nil_value();
  }

  file = fopen(expanded_path, "r");
  if (!file) {
    char message[BUFFER_SIZE];
    mrb_value exception;
    snprintf(message, BUFFER_SIZE - 1,
             "fopen: failed to open mruby script file: <%s>", expanded_path);
    SERR(message);
    exception = mrb_exc_new(mrb, E_LOAD_ERROR,
                            ctx->errbuf, strlen(ctx->errbuf));
    mrb->exc = mrb_obj_ptr(exception);
    return mrb_nil_value();
  }

  {
    char current_base_directory[PATH_MAX];
    char *last_directory;

    strcpy(current_base_directory, data->base_directory);
    strcpy(data->base_directory, expanded_path);
    last_directory = strrchr(data->base_directory, '/');
    if (last_directory) {
      last_directory[0] = '\0';
    }

    parser = mrb_parser_new(mrb);
    mrb_parser_set_filename(parser, expanded_path);
    parser->s = parser->send = NULL;
    parser->f = file;
    mrb_parser_parse(parser, NULL);
    fclose(file);

    {
      struct RProc *proc;
      proc = mrb_generate_code(mrb, parser);
      result = mrb_toplevel_run(mrb, proc);
    }
    mrb_parser_free(parser);

    strcpy(data->base_directory, current_base_directory);
  }

  return result;
}

mrb_value
grn_mrb_eval(grn_ctx *ctx, const char *script, int script_length)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  mrb_value result;
  struct mrb_parser_state *parser;

  if (!mrb) {
    return mrb_nil_value();
  }

  if (script_length < 0) {
    script_length = strlen(script);
  }
  parser = mrb_parse_nstring(mrb, script, script_length, NULL);
  {
    struct RProc *proc;
    struct RClass *eval_context_class;
    mrb_value eval_context;

    proc = mrb_generate_code(mrb, parser);
    eval_context_class = mrb_class_get_under(mrb, data->module, "EvalContext");
    eval_context = mrb_obj_new(mrb, eval_context_class, 0, NULL);
    result = mrb_context_run(mrb, proc, eval_context, 0);
  }
  mrb_parser_free(parser);

  return result;
}

grn_rc
grn_mrb_to_grn(grn_ctx *ctx, mrb_value mrb_object, grn_obj *grn_object)
{
  grn_rc rc = GRN_SUCCESS;
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;

  switch (mrb_type(mrb_object)) {
  case MRB_TT_FALSE :
    if (mrb_nil_p(mrb_object)) {
      grn_obj_reinit(ctx, grn_object, GRN_DB_VOID, 0);
    } else {
      grn_obj_reinit(ctx, grn_object, GRN_DB_BOOL, 0);
      GRN_BOOL_SET(ctx, grn_object, GRN_FALSE);
    }
    break;
  case MRB_TT_TRUE :
    grn_obj_reinit(ctx, grn_object, GRN_DB_BOOL, 0);
    GRN_BOOL_SET(ctx, grn_object, GRN_TRUE);
    break;
  case MRB_TT_FIXNUM :
    grn_obj_reinit(ctx, grn_object, GRN_DB_INT32, 0);
    GRN_INT32_SET(ctx, grn_object, mrb_fixnum(mrb_object));
    break;
  case MRB_TT_STRING :
    grn_obj_reinit(ctx, grn_object, GRN_DB_TEXT, 0);
    GRN_TEXT_SET(ctx, grn_object,
                 RSTRING_PTR(mrb_object),
                 RSTRING_LEN(mrb_object));
    break;
  case MRB_TT_SYMBOL :
    {
      const char *name;
      int name_length;

      grn_obj_reinit(ctx, grn_object, GRN_DB_TEXT, 0);
      GRN_BULK_REWIND(grn_object);
      GRN_TEXT_PUTC(ctx, grn_object, ':');
      name = mrb_sym2name_len(mrb, mrb_symbol(mrb_object), &name_length);
      GRN_TEXT_PUT(ctx, grn_object, name, name_length);
    }
    break;
  default :
    rc = GRN_INVALID_ARGUMENT;
    break;
  }

  return rc;
}
#endif
