/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2017 Brazil

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

#include <stddef.h>

#include <groonga.h>

#ifdef __cplusplus
extern "C" {
#endif

# define GRN_PLUGIN_IMPL_NAME_RAW(type)         \
  grn_plugin_impl_ ## type
# define GRN_PLUGIN_IMPL_NAME_TAGGED(type, tag) \
  GRN_PLUGIN_IMPL_NAME_RAW(type ## _ ## tag)
# define GRN_PLUGIN_IMPL_NAME_TAGGED_EXPANDABLE(type, tag)      \
  GRN_PLUGIN_IMPL_NAME_TAGGED(type, tag)

#ifdef GRN_PLUGIN_FUNCTION_TAG
# define GRN_PLUGIN_IMPL_NAME(type)                             \
  GRN_PLUGIN_IMPL_NAME_TAGGED_EXPANDABLE(type, GRN_PLUGIN_FUNCTION_TAG)
#else /* GRN_PLUGIN_FUNCTION_TAG */
# define GRN_PLUGIN_IMPL_NAME(type)             \
  GRN_PLUGIN_IMPL_NAME_RAW(type)
#endif /* GRN_PLUGIN_FUNCTION_TAG */

#define GRN_PLUGIN_INIT     GRN_PLUGIN_IMPL_NAME(init)
#define GRN_PLUGIN_REGISTER GRN_PLUGIN_IMPL_NAME(register)
#define GRN_PLUGIN_FIN      GRN_PLUGIN_IMPL_NAME(fin)

#if defined(_WIN32) || defined(_WIN64)
#  define GRN_PLUGIN_EXPORT __declspec(dllexport)
#else /* defined(_WIN32) || defined(_WIN64) */
#  define GRN_PLUGIN_EXPORT
#endif /* defined(_WIN32) || defined(_WIN64) */

GRN_PLUGIN_EXPORT grn_rc GRN_PLUGIN_INIT(grn_ctx *ctx);
GRN_PLUGIN_EXPORT grn_rc GRN_PLUGIN_REGISTER(grn_ctx *ctx);
GRN_PLUGIN_EXPORT grn_rc GRN_PLUGIN_FIN(grn_ctx *ctx);

#define GRN_PLUGIN_DECLARE_FUNCTIONS(tag)                               \
  extern grn_rc GRN_PLUGIN_IMPL_NAME_TAGGED(init, tag)(grn_ctx *ctx);   \
  extern grn_rc GRN_PLUGIN_IMPL_NAME_TAGGED(register, tag)(grn_ctx *ctx); \
  extern grn_rc GRN_PLUGIN_IMPL_NAME_TAGGED(fin, tag)(grn_ctx *ctx)

/*
  Don't call these functions directly. Use GRN_PLUGIN_MALLOC(),
  GRN_PLUGIN_CALLOC(), GRN_PLUGIN_REALLOC() and GRN_PLUGIN_FREE() instead.
 */
GRN_API void *grn_plugin_malloc(grn_ctx *ctx,
                                size_t size,
                                const char *file,
                                int line,
                                const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
GRN_API void *grn_plugin_calloc(grn_ctx *ctx,
                                size_t size,
                                const char *file,
                                int line,
                                const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
GRN_API void *grn_plugin_realloc(grn_ctx *ctx,
                                 void *ptr,
                                 size_t size,
                                 const char *file,
                                 int line,
                                 const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(3);
GRN_API void grn_plugin_free(grn_ctx *ctx, void *ptr, const char *file,
                             int line, const char *func);

#define GRN_PLUGIN_MALLOC(ctx, size) \
  grn_plugin_malloc((ctx), (size), __FILE__, __LINE__, __FUNCTION__)
#define GRN_PLUGIN_MALLOCN(ctx, type, n) \
  ((type *)(grn_plugin_malloc((ctx), sizeof(type) * (n), \
                              __FILE__, __LINE__, __FUNCTION__)))
#define GRN_PLUGIN_CALLOC(ctx, size) \
  grn_plugin_calloc((ctx), (size), __FILE__, __LINE__, __FUNCTION__)
#define GRN_PLUGIN_REALLOC(ctx, ptr, size) \
  grn_plugin_realloc((ctx), (ptr), (size), __FILE__, __LINE__, __FUNCTION__)
#define GRN_PLUGIN_FREE(ctx, ptr) \
  grn_plugin_free((ctx), (ptr), __FILE__, __LINE__, __FUNCTION__)

#define GRN_PLUGIN_LOG(ctx, level, ...) \
  GRN_LOG((ctx), (level), __VA_ARGS__)

/*
  Don't call grn_plugin_set_error() directly. This function is used in
  GRN_PLUGIN_SET_ERROR().
 */
GRN_API void grn_plugin_set_error(grn_ctx *ctx, grn_log_level level,
                                  grn_rc error_code,
                                  const char *file, int line, const char *func,
                                  const char *format, ...) GRN_ATTRIBUTE_PRINTF(7);
GRN_API void grn_plugin_clear_error(grn_ctx *ctx);


/*
  Don't call these functions directly. grn_plugin_backtrace() and
  grn_plugin_logtrace() are used in GRN_PLUGIN_SET_ERROR().
 */
GRN_API void grn_plugin_backtrace(grn_ctx *ctx);
GRN_API void grn_plugin_logtrace(grn_ctx *ctx, grn_log_level level);

/*
  Don't use GRN_PLUGIN_SET_ERROR() directly. This macro is used in
  GRN_PLUGIN_ERROR().
 */
#define GRN_PLUGIN_SET_ERROR(ctx, level, error_code, ...) do { \
  grn_plugin_set_error(ctx, level, error_code, \
                       __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__); \
} while (0)

#define GRN_PLUGIN_ERROR(ctx, error_code, ...) \
  GRN_PLUGIN_SET_ERROR(ctx, GRN_LOG_ERROR, error_code, __VA_ARGS__)

#define GRN_PLUGIN_CLEAR_ERROR(ctx) do { \
  grn_plugin_clear_error((ctx)); \
} while (0)

typedef struct _grn_plugin_mutex grn_plugin_mutex;

GRN_API grn_plugin_mutex *grn_plugin_mutex_open(grn_ctx *ctx);

/*
  grn_plugin_mutex_create() is deprecated. Use grn_plugin_mutex_open()
  instead.
*/
GRN_API grn_plugin_mutex *grn_plugin_mutex_create(grn_ctx *ctx);

GRN_API void grn_plugin_mutex_close(grn_ctx *ctx, grn_plugin_mutex *mutex);

/*
  grn_plugin_mutex_destroy() is deprecated. Use grn_plugin_mutex_close()
  instead.
*/
GRN_API void grn_plugin_mutex_destroy(grn_ctx *ctx, grn_plugin_mutex *mutex);

GRN_API void grn_plugin_mutex_lock(grn_ctx *ctx, grn_plugin_mutex *mutex);

GRN_API void grn_plugin_mutex_unlock(grn_ctx *ctx, grn_plugin_mutex *mutex);

GRN_API grn_obj *grn_plugin_proc_alloc(grn_ctx *ctx, grn_user_data *user_data,
                                       grn_id domain, unsigned char flags);

GRN_API grn_obj *grn_plugin_proc_get_vars(grn_ctx *ctx, grn_user_data *user_data);

GRN_API grn_obj *grn_plugin_proc_get_var(grn_ctx *ctx, grn_user_data *user_data,
                                         const char *name, int name_size);
GRN_API grn_bool grn_plugin_proc_get_var_bool(grn_ctx *ctx,
                                              grn_user_data *user_data,
                                              const char *name,
                                              int name_size,
                                              grn_bool default_value);
GRN_API int32_t grn_plugin_proc_get_var_int32(grn_ctx *ctx,
                                              grn_user_data *user_data,
                                              const char *name,
                                              int name_size,
                                              int32_t default_value);
GRN_API const char *grn_plugin_proc_get_var_string(grn_ctx *ctx,
                                                   grn_user_data *user_data,
                                                   const char *name,
                                                   int name_size,
                                                   size_t *size);
GRN_API grn_content_type grn_plugin_proc_get_var_content_type(grn_ctx *ctx,
                                                              grn_user_data *user_data,
                                                              const char *name,
                                                              int name_size,
                                                              grn_content_type default_value);

GRN_API grn_obj *grn_plugin_proc_get_var_by_offset(grn_ctx *ctx,
                                                   grn_user_data *user_data,
                                                   unsigned int offset);

GRN_API grn_obj *grn_plugin_proc_get_caller(grn_ctx *ctx,
                                            grn_user_data *user_data);

/* Deprecated since 5.0.9. Use grn_plugin_windows_base_dir() instead. */
GRN_API const char *grn_plugin_win32_base_dir(void);
GRN_API const char *grn_plugin_windows_base_dir(void);

GRN_API int grn_plugin_charlen(grn_ctx *ctx, const char *str_ptr,
                               unsigned int str_length, grn_encoding encoding);

GRN_API int grn_plugin_isspace(grn_ctx *ctx, const char *str_ptr,
                               unsigned int str_length, grn_encoding encoding);

GRN_API grn_rc grn_plugin_expr_var_init(grn_ctx *ctx,
                                        grn_expr_var *var,
                                        const char *name,
                                        int name_size);

GRN_API grn_obj *grn_plugin_command_create(grn_ctx *ctx,
                                           const char *name,
                                           int name_size,
                                           grn_proc_func func,
                                           unsigned int n_vars,
                                           grn_expr_var *vars);


#ifdef __cplusplus
}
#endif
