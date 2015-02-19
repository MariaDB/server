/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2015 Brazil

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
#include "grn.h"
#include <groonga/plugin.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#ifdef HAVE_DIRENT_H
# include <dirent.h>
#endif /* HAVE_DIRENT_H */

#ifndef S_ISREG
# ifdef _S_IFREG
#  define S_ISREG(mode) (mode & _S_IFREG)
# endif /* _S_IFREG */
#endif /* !S_ISREG */

#include "grn_db.h"
#include "grn_plugin.h"
#include "grn_ctx_impl.h"
#include "grn_util.h"

#ifdef GRN_WITH_MRUBY
# include <mruby.h>
#endif /* GRN_WITH_MRUBY */

static grn_hash *grn_plugins = NULL;
static grn_critical_section grn_plugins_lock;

#ifdef GRN_WITH_MRUBY
static const char *grn_plugin_mrb_suffix = ".rb";
#endif /* GRN_WITH_MRUBY */

#ifdef HAVE_DLFCN_H
#  include <dlfcn.h>
#  define grn_dl_open(filename)      dlopen(filename, RTLD_LAZY | RTLD_LOCAL)
#  define grn_dl_open_error_label()  dlerror()
#  define grn_dl_close(dl)           (dlclose(dl) == 0)
#  define grn_dl_close_error_label() dlerror()
#  define grn_dl_sym(dl, symbol)     dlsym(dl, symbol)
#  define grn_dl_sym_error_label()   dlerror()
#  define grn_dl_clear_error()       dlerror()
#else
#  define grn_dl_open(filename)      LoadLibrary(filename)
#  define grn_dl_open_error_label()  "LoadLibrary"
#  define grn_dl_close(dl)           (FreeLibrary(dl) != 0)
#  define grn_dl_close_error_label() "FreeLibrary"
#  define grn_dl_sym(dl, symbol)     ((void *)GetProcAddress(dl, symbol))
#  define grn_dl_sym_error_label()   "GetProcAddress"
#  define grn_dl_clear_error()
#endif

static int
compute_name_size(const char *name, int name_size)
{
  if (name_size < 0) {
    if (name) {
      name_size = strlen(name);
    } else {
      name_size = 0;
    }
  }
  return name_size;
}

grn_id
grn_plugin_reference(grn_ctx *ctx, const char *filename)
{
  grn_id id;
  grn_plugin **plugin = NULL;

  CRITICAL_SECTION_ENTER(grn_plugins_lock);
  id = grn_hash_get(&grn_gctx, grn_plugins, filename, strlen(filename),
                    (void **)&plugin);
  if (plugin) {
    (*plugin)->refcount++;
  }
  CRITICAL_SECTION_LEAVE(grn_plugins_lock);

  return id;
}

const char *
grn_plugin_path(grn_ctx *ctx, grn_id id)
{
  const char *path;
  uint32_t key_size;
  const char *system_plugins_dir;
  size_t system_plugins_dir_size;

  if (id == GRN_ID_NIL) {
    return NULL;
  }

  CRITICAL_SECTION_ENTER(grn_plugins_lock);
  path = _grn_hash_key(&grn_gctx, grn_plugins, id, &key_size);
  CRITICAL_SECTION_LEAVE(grn_plugins_lock);

  if (!path) {
    return NULL;
  }

  system_plugins_dir = grn_plugin_get_system_plugins_dir();
  system_plugins_dir_size = strlen(system_plugins_dir);
  if (strncmp(system_plugins_dir, path, system_plugins_dir_size) == 0) {
    const char *plugin_name = path + system_plugins_dir_size;
    while (plugin_name[0] == '/') {
      plugin_name++;
    }
    /* TODO: remove suffix too? */
    return plugin_name;
  } else {
    return path;
  }
}

#define GRN_PLUGIN_FUNC_PREFIX "grn_plugin_impl_"

static grn_rc
grn_plugin_call_init(grn_ctx *ctx, grn_id id)
{
  grn_plugin *plugin;
  if (!grn_hash_get_value(&grn_gctx, grn_plugins, id, &plugin)) {
    return GRN_INVALID_ARGUMENT;
  }
  if (plugin->init_func) {
    return plugin->init_func(ctx);
  }
  return GRN_SUCCESS;
}

#ifdef GRN_WITH_MRUBY
static grn_rc
grn_plugin_call_register_mrb(grn_ctx *ctx, grn_id id, grn_plugin *plugin)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *plugin_loader_class;
  int arena_index;

  {
    int added;
    grn_hash_add(ctx, ctx->impl->mrb.registered_plugins,
                 &id, sizeof(grn_id), NULL, &added);
    if (!added) {
      return ctx->rc;
    }
  }

  arena_index = mrb_gc_arena_save(mrb);
  plugin_loader_class = mrb_class_get_under(mrb, module, "PluginLoader");
  mrb_funcall(mrb, mrb_obj_value(plugin_loader_class),
              "load_file", 1, mrb_str_new_cstr(mrb, ctx->impl->plugin_path));
  mrb_gc_arena_restore(mrb, arena_index);
  return ctx->rc;
}
#endif /*GRN_WITH_MRUBY */

static grn_rc
grn_plugin_call_register(grn_ctx *ctx, grn_id id)
{
  grn_plugin *plugin;
  if (!grn_hash_get_value(&grn_gctx, grn_plugins, id, &plugin)) {
    return GRN_INVALID_ARGUMENT;
  }
#ifdef GRN_WITH_MRUBY
  if (!plugin->dl) {
    return grn_plugin_call_register_mrb(ctx, id, plugin);
  }
#endif /* GRN_WITH_MRUBY */
  if (plugin->register_func) {
    return plugin->register_func(ctx);
  }
  return GRN_SUCCESS;
}

static grn_rc
grn_plugin_call_fin(grn_ctx *ctx, grn_id id)
{
  grn_plugin *plugin;
  if (!grn_hash_get_value(&grn_gctx, grn_plugins, id, &plugin)) {
    return GRN_INVALID_ARGUMENT;
  }
  if (plugin->fin_func) {
    return plugin->fin_func(ctx);
  }
  return GRN_SUCCESS;
}

static grn_rc
grn_plugin_initialize(grn_ctx *ctx, grn_plugin *plugin,
                      grn_dl dl, grn_id id, const char *path)
{
  plugin->dl = dl;

#define GET_SYMBOL(type) do {                                           \
  grn_dl_clear_error();                                                 \
  plugin->type ## _func = grn_dl_sym(dl, GRN_PLUGIN_FUNC_PREFIX #type); \
  if (!plugin->type ## _func) {                                         \
    const char *label;                                                  \
    label = grn_dl_sym_error_label();                                   \
    SERR(label);                                                        \
  }                                                                     \
} while (0)

  GET_SYMBOL(init);
  GET_SYMBOL(register);
  GET_SYMBOL(fin);

#undef GET_SYMBOL

  if (!plugin->init_func || !plugin->register_func || !plugin->fin_func) {
    ERR(GRN_INVALID_FORMAT,
        "init func (%s) %sfound, "
        "register func (%s) %sfound and "
        "fin func (%s) %sfound",
        GRN_PLUGIN_FUNC_PREFIX "init", plugin->init_func ? "" : "not ",
        GRN_PLUGIN_FUNC_PREFIX "register", plugin->register_func ? "" : "not ",
        GRN_PLUGIN_FUNC_PREFIX "fin", plugin->fin_func ? "" : "not ");
  }

  if (!ctx->rc) {
    ctx->impl->plugin_path = path;
    grn_plugin_call_init(ctx, id);
    ctx->impl->plugin_path = NULL;
  }

  return ctx->rc;
}

#ifdef GRN_WITH_MRUBY
static grn_id
grn_plugin_open_mrb(grn_ctx *ctx, const char *filename, size_t filename_size)
{
  grn_id id = GRN_ID_NIL;
  grn_plugin **plugin = NULL;

  id = grn_hash_add(&grn_gctx, grn_plugins, filename, filename_size,
                    (void **)&plugin, NULL);
  if (!id) {
    return id;
  }

  *plugin = GRN_GMALLOCN(grn_plugin, 1);
  if (!*plugin) {
    grn_hash_delete_by_id(&grn_gctx, grn_plugins, id, NULL);
    return GRN_ID_NIL;
  }

  (*plugin)->dl = NULL;
  (*plugin)->init_func = NULL;
  (*plugin)->register_func = NULL;
  (*plugin)->fin_func = NULL;
  (*plugin)->refcount = 1;

  return id;
}
#endif /* GRN_WITH_MRUBY */

grn_id
grn_plugin_open(grn_ctx *ctx, const char *filename)
{
  grn_id id = GRN_ID_NIL;
  grn_dl dl;
  grn_plugin **plugin = NULL;
  size_t filename_size;

  filename_size = strlen(filename);

  CRITICAL_SECTION_ENTER(grn_plugins_lock);
  if ((id = grn_hash_get(&grn_gctx, grn_plugins, filename, filename_size,
                         (void **)&plugin))) {
    (*plugin)->refcount++;
    goto exit;
  }

#ifdef GRN_WITH_MRUBY
  if (filename_size > strlen(grn_plugin_mrb_suffix) &&
      strcmp(filename + (filename_size - strlen(grn_plugin_mrb_suffix)),
             grn_plugin_mrb_suffix) == 0) {
    id = grn_plugin_open_mrb(ctx, filename, filename_size);
    goto exit;
  }
#endif /* GRN_WITH_MRUBY */

  if ((dl = grn_dl_open(filename))) {
    if ((id = grn_hash_add(&grn_gctx, grn_plugins, filename, filename_size,
                           (void **)&plugin, NULL))) {
      *plugin = GRN_GMALLOCN(grn_plugin, 1);
      if (*plugin) {
        if (grn_plugin_initialize(ctx, *plugin, dl, id, filename)) {
          GRN_GFREE(*plugin);
          *plugin = NULL;
        }
      }
      if (!*plugin) {
        grn_hash_delete_by_id(&grn_gctx, grn_plugins, id, NULL);
        if (grn_dl_close(dl)) {
          /* Now, __FILE__ set in plugin is invalid. */
          ctx->errline = 0;
          ctx->errfile = NULL;
        } else {
          const char *label;
          label = grn_dl_close_error_label();
          SERR(label);
        }
        id = GRN_ID_NIL;
      } else {
        (*plugin)->refcount = 1;
      }
    } else {
      if (!grn_dl_close(dl)) {
        const char *label;
        label = grn_dl_close_error_label();
        SERR(label);
      }
    }
  } else {
    const char *label;
    label = grn_dl_open_error_label();
    SERR(label);
  }

exit:
  CRITICAL_SECTION_LEAVE(grn_plugins_lock);

  return id;
}

grn_rc
grn_plugin_close(grn_ctx *ctx, grn_id id)
{
  grn_rc rc;
  grn_plugin *plugin;

  if (id == GRN_ID_NIL) {
    return GRN_INVALID_ARGUMENT;
  }

  CRITICAL_SECTION_ENTER(grn_plugins_lock);
  if (!grn_hash_get_value(&grn_gctx, grn_plugins, id, &plugin)) {
    rc = GRN_INVALID_ARGUMENT;
    goto exit;
  }
  if (--plugin->refcount) {
    rc = GRN_SUCCESS;
    goto exit;
  }
  if (plugin->dl) {
    grn_plugin_call_fin(ctx, id);
    if (!grn_dl_close(plugin->dl)) {
      const char *label;
      label = grn_dl_close_error_label();
      SERR(label);
    }
  }
  GRN_GFREE(plugin);
  rc = grn_hash_delete_by_id(&grn_gctx, grn_plugins, id, NULL);

exit:
  CRITICAL_SECTION_LEAVE(grn_plugins_lock);

  return rc;
}

void *
grn_plugin_sym(grn_ctx *ctx, grn_id id, const char *symbol)
{
  grn_plugin *plugin;
  grn_dl_symbol func;

  if (id == GRN_ID_NIL) {
    return NULL;
  }

  CRITICAL_SECTION_ENTER(grn_plugins_lock);
  if (!grn_hash_get_value(&grn_gctx, grn_plugins, id, &plugin)) {
    func = NULL;
    goto exit;
  }
  grn_dl_clear_error();
  if (!(func = grn_dl_sym(plugin->dl, symbol))) {
    const char *label;
    label = grn_dl_sym_error_label();
    SERR(label);
  }

exit:
  CRITICAL_SECTION_LEAVE(grn_plugins_lock);

  return func;
}

grn_rc
grn_plugins_init(void)
{
  CRITICAL_SECTION_INIT(grn_plugins_lock);
  grn_plugins = grn_hash_create(&grn_gctx, NULL, PATH_MAX, sizeof(grn_plugin *),
                                GRN_OBJ_KEY_VAR_SIZE);
  if (!grn_plugins) { return GRN_NO_MEMORY_AVAILABLE; }
  return GRN_SUCCESS;
}

grn_rc
grn_plugins_fin(void)
{
  grn_rc rc;
  if (!grn_plugins) { return GRN_INVALID_ARGUMENT; }
  GRN_HASH_EACH(&grn_gctx, grn_plugins, id, NULL, NULL, NULL, {
    grn_plugin_close(&grn_gctx, id);
  });
  rc = grn_hash_close(&grn_gctx, grn_plugins);
  CRITICAL_SECTION_FIN(grn_plugins_lock);
  return rc;
}

const char *
grn_plugin_get_suffix(void)
{
  return GRN_PLUGIN_SUFFIX;
}

grn_rc
grn_plugin_register_by_path(grn_ctx *ctx, const char *path)
{
  grn_obj *db;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return ctx->rc;
  }
  GRN_API_ENTER;
  if (GRN_DB_P(db)) {
    grn_id id;
    id = grn_plugin_open(ctx, path);
    if (id) {
      ctx->impl->plugin_path = path;
      ctx->rc = grn_plugin_call_register(ctx, id);
      ctx->impl->plugin_path = NULL;
      grn_plugin_close(ctx, id);
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "invalid db assigned");
  }
  GRN_API_RETURN(ctx->rc);
}

#ifdef WIN32
static char *win32_plugins_dir = NULL;
static char win32_plugins_dir_buffer[PATH_MAX];
static const char *
grn_plugin_get_default_system_plugins_dir(void)
{
  if (!win32_plugins_dir) {
    const char *base_dir;
    const char *relative_path = GRN_RELATIVE_PLUGINS_DIR;
    size_t base_dir_length;

    base_dir = grn_win32_base_dir();
    base_dir_length = strlen(base_dir);
    strcpy(win32_plugins_dir_buffer, base_dir);
    strcat(win32_plugins_dir_buffer, "/");
    strcat(win32_plugins_dir_buffer, relative_path);
    win32_plugins_dir = win32_plugins_dir_buffer;
  }
  return win32_plugins_dir;
}

#else /* WIN32 */
static const char *
grn_plugin_get_default_system_plugins_dir(void)
{
  return GRN_PLUGINS_DIR;
}
#endif /* WIN32 */

const char *
grn_plugin_get_system_plugins_dir(void)
{
  const char *plugins_dir;

  plugins_dir = getenv("GRN_PLUGINS_DIR");
  if (!plugins_dir) {
    plugins_dir = grn_plugin_get_default_system_plugins_dir();
  }

  return plugins_dir;
}

static char *
grn_plugin_find_path_raw(grn_ctx *ctx, const char *path)
{
  struct stat path_stat;

  if (stat(path, &path_stat) != 0) {
    return NULL;
  }

  if (!S_ISREG(path_stat.st_mode)) {
    return NULL;
  }

  return GRN_STRDUP(path);
}

#if GRN_WITH_MRUBY
static char *
grn_plugin_find_path_mrb(grn_ctx *ctx, const char *path, size_t path_len)
{
  char mrb_path[PATH_MAX];
  const char *mrb_suffix = grn_plugin_mrb_suffix;
  size_t mrb_path_len;

  mrb_path_len = path_len + strlen(mrb_suffix);
  if (mrb_path_len >= PATH_MAX) {
    ERR(GRN_FILENAME_TOO_LONG,
        "too long plugin path: <%s%s>",
        path, mrb_suffix);
    return NULL;
  }

  strcpy(mrb_path, path);
  strcat(mrb_path, mrb_suffix);
  return grn_plugin_find_path_raw(ctx, mrb_path);
}
#else /* GRN_WITH_MRUBY */
static char *
grn_plugin_find_path_mrb(grn_ctx *ctx, const char *path, size_t path_len)
{
  return NULL;
}
#endif /* GRN_WITH_MRUBY */

static char *
grn_plugin_find_path_so(grn_ctx *ctx, const char *path, size_t path_len)
{
  char so_path[PATH_MAX];
  const char *so_suffix;
  size_t so_path_len;

  so_suffix = grn_plugin_get_suffix();
  so_path_len = path_len + strlen(so_suffix);
  if (so_path_len >= PATH_MAX) {
    ERR(GRN_FILENAME_TOO_LONG,
        "too long plugin path: <%s%s>",
        path, so_suffix);
    return NULL;
  }

  strcpy(so_path, path);
  strcat(so_path, so_suffix);
  return grn_plugin_find_path_raw(ctx, so_path);
}

static char *
grn_plugin_find_path_libs_so(grn_ctx *ctx, const char *path, size_t path_len)
{
  char libs_so_path[PATH_MAX];
  const char *base_name;
  const char *so_suffix;
  const char *libs_path = "/.libs";
  size_t libs_so_path_len;

  base_name = strrchr(path, '/');
  if (!base_name) {
    return NULL;
  }

  so_suffix = grn_plugin_get_suffix();
  libs_so_path_len =
    base_name - path +
    strlen(libs_path) +
    strlen(base_name) +
    strlen(so_suffix);
  if (libs_so_path_len >= PATH_MAX) {
    ERR(GRN_FILENAME_TOO_LONG,
        "too long plugin path: <%.*s/.libs%s%s>",
        (int)(base_name - path), path, base_name, so_suffix);
    return NULL;
  }

  libs_so_path[0] = '\0';
  strncat(libs_so_path, path, base_name - path);
  strcat(libs_so_path, libs_path);
  strcat(libs_so_path, base_name);
  strcat(libs_so_path, so_suffix);
  return grn_plugin_find_path_raw(ctx, libs_so_path);
}

char *
grn_plugin_find_path(grn_ctx *ctx, const char *name)
{
  const char *plugins_dir;
  char dir_last_char;
  char path[PATH_MAX];
  int name_length, max_name_length;
  char *found_path = NULL;
  size_t path_len;

  GRN_API_ENTER;
  if (name[0] == '/') {
    path[0] = '\0';
  } else {
    plugins_dir = grn_plugin_get_system_plugins_dir();
    strcpy(path, plugins_dir);

    dir_last_char = plugins_dir[strlen(path) - 1];
    if (dir_last_char != '/') {
      strcat(path, "/");
    }
  }

  name_length = strlen(name);
  max_name_length = PATH_MAX - strlen(path) - 1;
  if (name_length > max_name_length) {
    ERR(GRN_INVALID_ARGUMENT,
        "plugin name is too long: %d (max: %d) <%s%s>",
        name_length, max_name_length,
        path, name);
    goto exit;
  }
  strcat(path, name);

  found_path = grn_plugin_find_path_raw(ctx, path);
  if (found_path) {
    goto exit;
  }

  path_len = strlen(path);
  found_path = grn_plugin_find_path_mrb(ctx, path, path_len);
  if (found_path) {
    goto exit;
  }
  if (ctx->rc) {
    goto exit;
  }

  found_path = grn_plugin_find_path_so(ctx, path, path_len);
  if (found_path) {
    goto exit;
  }
  if (ctx->rc) {
    goto exit;
  }

  found_path = grn_plugin_find_path_libs_so(ctx, path, path_len);
  if (found_path) {
    goto exit;
  }
  if (ctx->rc) {
    goto exit;
  }

exit :
  GRN_API_RETURN(found_path);
}

grn_rc
grn_plugin_register(grn_ctx *ctx, const char *name)
{
  grn_rc rc;
  char *path;

  GRN_API_ENTER;
  path = grn_plugin_find_path(ctx, name);
  if (path) {
    rc = grn_plugin_register_by_path(ctx, path);
    GRN_FREE(path);
  } else {
    if (ctx->rc == GRN_SUCCESS) {
      const char *prefix, *prefix_separator, *suffix;
      if (name[0] == '/') {
        prefix = "";
        prefix_separator = "";
        suffix = "";
      } else {
        prefix = grn_plugin_get_system_plugins_dir();
        if (prefix[strlen(prefix) - 1] != '/') {
          prefix_separator = "/";
        } else {
          prefix_separator = "";
        }
        suffix = grn_plugin_get_suffix();
      }
      ERR(GRN_NO_SUCH_FILE_OR_DIRECTORY,
          "cannot find plugin file: <%s%s%s%s>",
          prefix, prefix_separator, name, suffix);
    }
    rc = ctx->rc;
  }
  GRN_API_RETURN(rc);
}

void
grn_plugin_ensure_registered(grn_ctx *ctx, grn_obj *proc)
{
#ifdef GRN_WITH_MRUBY
  grn_id plugin_id;
  const char *plugin_path;
  uint32_t key_size;
  grn_plugin *plugin;
  int value_size;

  if (!ctx->impl->mrb.state) {
    return;
  }

  if (!(proc->header.flags & GRN_OBJ_CUSTOM_NAME)) {
    return;
  }

  {
    grn_id id;
    int added;
    id = DB_OBJ(proc)->id;
    grn_hash_add(ctx, ctx->impl->mrb.checked_procs,
                 &id, sizeof(grn_id), NULL, &added);
    if (!added) {
      return;
    }
  }

  plugin_id = DB_OBJ(proc)->range;
  CRITICAL_SECTION_ENTER(grn_plugins_lock);
  plugin_path = _grn_hash_key(&grn_gctx, grn_plugins, plugin_id, &key_size);
  if (plugin_path) {
    value_size = grn_hash_get_value(&grn_gctx, grn_plugins, plugin_id, &plugin);
  }
  CRITICAL_SECTION_LEAVE(grn_plugins_lock);

  if (!plugin_path) {
    return;
  }

  if (plugin->dl) {
    return;
  }

  ctx->impl->plugin_path = plugin_path;
  grn_plugin_call_register_mrb(ctx, plugin_id, plugin);
  ctx->impl->plugin_path = NULL;
#endif /* GRN_WITH_MRUBY */
}

void *
grn_plugin_malloc(grn_ctx *ctx, size_t size, const char *file, int line,
                  const char *func)
{
  return grn_malloc(ctx, size, file, line, func);
}

void *
grn_plugin_realloc(grn_ctx *ctx, void *ptr, size_t size,
                   const char *file, int line, const char *func)
{
  return grn_realloc(ctx, ptr, size, file, line, func);
}

void
grn_plugin_free(grn_ctx *ctx, void *ptr, const char *file, int line,
                const char *func)
{
  grn_free(ctx, ptr, file, line, func);
}

/*
  grn_plugin_ctx_log() is a clone of grn_ctx_log() in ctx.c. The only
  difference is that grn_plugin_ctx_log() uses va_list instead of `...'.
 */
static void
grn_plugin_ctx_log(grn_ctx *ctx, const char *format, va_list ap)
{
  vsnprintf(ctx->errbuf, GRN_CTX_MSGSIZE, format, ap);
}

void
grn_plugin_set_error(grn_ctx *ctx, grn_log_level level, grn_rc error_code,
                     const char *file, int line, const char *func,
                     const char *format, ...)
{
  ctx->errlvl = level;
  ctx->rc = error_code;
  ctx->errfile = file;
  ctx->errline = line;
  ctx->errfunc = func;

  {
    va_list ap;
    va_start(ap, format);
    grn_plugin_ctx_log(ctx, format, ap);
    va_end(ap);
  }
}

void
grn_plugin_backtrace(grn_ctx *ctx)
{
  BACKTRACE(ctx);
}

void
grn_plugin_logtrace(grn_ctx *ctx, grn_log_level level)
{
  if (level <= GRN_LOG_ERROR) {
    LOGTRACE(ctx, level);
  }
}

struct _grn_plugin_mutex {
  grn_critical_section critical_section;
};

grn_plugin_mutex *
grn_plugin_mutex_open(grn_ctx *ctx)
{
  grn_plugin_mutex * const mutex =
      GRN_PLUGIN_MALLOC(ctx, sizeof(grn_plugin_mutex));
  if (mutex != NULL) {
    CRITICAL_SECTION_INIT(mutex->critical_section);
  }
  return mutex;
}

grn_plugin_mutex *
grn_plugin_mutex_create(grn_ctx *ctx)
{
  return grn_plugin_mutex_open(ctx);
}

void
grn_plugin_mutex_close(grn_ctx *ctx, grn_plugin_mutex *mutex)
{
  if (mutex != NULL) {
    CRITICAL_SECTION_FIN(mutex->critical_section);
    GRN_PLUGIN_FREE(ctx, mutex);
  }
}

void
grn_plugin_mutex_destroy(grn_ctx *ctx, grn_plugin_mutex *mutex)
{
  grn_plugin_mutex_close(ctx, mutex);
}

void
grn_plugin_mutex_lock(grn_ctx *ctx, grn_plugin_mutex *mutex)
{
  if (mutex != NULL) {
    CRITICAL_SECTION_ENTER(mutex->critical_section);
  }
}

void
grn_plugin_mutex_unlock(grn_ctx *ctx, grn_plugin_mutex *mutex)
{
  if (mutex != NULL) {
    CRITICAL_SECTION_LEAVE(mutex->critical_section);
  }
}

grn_obj *
grn_plugin_proc_alloc(grn_ctx *ctx, grn_user_data *user_data,
                      grn_id domain, grn_obj_flags flags)
{
  return grn_proc_alloc(ctx, user_data, domain, flags);
}

grn_obj *
grn_plugin_proc_get_vars(grn_ctx *ctx, grn_user_data *user_data)
{
  return grn_proc_get_vars(ctx, user_data);
}

grn_obj *
grn_plugin_proc_get_var(grn_ctx *ctx, grn_user_data *user_data,
                        const char *name, int name_size)
{
  name_size = compute_name_size(name, name_size);
  return grn_proc_get_var(ctx, user_data, name, name_size);
}

grn_obj *
grn_plugin_proc_get_var_by_offset(grn_ctx *ctx, grn_user_data *user_data,
                                  unsigned int offset)
{
  return grn_proc_get_var_by_offset(ctx, user_data, offset);
}

const char *
grn_plugin_win32_base_dir(void)
{
#ifdef WIN32
  return grn_win32_base_dir();
#else /* WIN32 */
  return NULL;
#endif /* WIN32 */
}

/*
  grn_plugin_charlen() takes the length of a string, unlike grn_charlen_().
 */
int
grn_plugin_charlen(grn_ctx *ctx, const char *str_ptr,
                   unsigned int str_length, grn_encoding encoding)
{
  return grn_charlen_(ctx, str_ptr, str_ptr + str_length, encoding);
}

/*
  grn_plugin_isspace() takes the length of a string, unlike grn_isspace().
 */
int
grn_plugin_isspace(grn_ctx *ctx, const char *str_ptr,
                   unsigned int str_length, grn_encoding encoding)
{
  if ((str_ptr == NULL) || (str_length == 0)) {
    return 0;
  }
  switch ((unsigned char)str_ptr[0]) {
  case ' ' :
  case '\f' :
  case '\n' :
  case '\r' :
  case '\t' :
  case '\v' :
    return 1;
  case 0x81 :
    if ((encoding == GRN_ENC_SJIS) && (str_length >= 2) &&
        ((unsigned char)str_ptr[1] == 0x40)) {
      return 2;
    }
    break;
  case 0xA1 :
    if ((encoding == GRN_ENC_EUC_JP) && (str_length >= 2) &&
        ((unsigned char)str_ptr[1] == 0xA1)) {
      return 2;
    }
    break;
  case 0xE3 :
    if ((encoding == GRN_ENC_UTF8) && (str_length >= 3) &&
        ((unsigned char)str_ptr[1] == 0x80) &&
        ((unsigned char)str_ptr[2] == 0x80)) {
      return 3;
    }
    break;
  default :
    break;
  }
  return 0;
}

grn_rc
grn_plugin_expr_var_init(grn_ctx *ctx,
                         grn_expr_var *var,
                         const char *name,
                         int name_size)
{
  var->name = name;
  var->name_size = compute_name_size(name, name_size);
  GRN_TEXT_INIT(&var->value, 0);
  return GRN_SUCCESS;
}

grn_obj *
grn_plugin_command_create(grn_ctx *ctx,
                          const char *name,
                          int name_size,
                          grn_proc_func func,
                          unsigned int n_vars,
                          grn_expr_var *vars)
{
  grn_obj *proc;
  name_size = compute_name_size(name, name_size);
  proc = grn_proc_create(ctx, name, name_size, GRN_PROC_COMMAND,
                         func, NULL, NULL, n_vars, vars);
  return proc;
}
