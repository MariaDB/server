/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#include "grn.h"

#ifdef __cplusplus
extern "C" {
#endif

void grn_alloc_init_from_env(void);

void grn_alloc_init_ctx_impl(grn_ctx *ctx);
void grn_alloc_fin_ctx_impl(grn_ctx *ctx);

void grn_alloc_info_init(void);
void grn_alloc_info_fin(void);

void grn_alloc_info_dump(grn_ctx *ctx);
void grn_alloc_info_free(grn_ctx *ctx);

#define GRN_MALLOC(s)     grn_malloc(ctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CALLOC(s)     grn_calloc(ctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_REALLOC(p,s)  grn_realloc(ctx,p,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_STRDUP(s)     grn_strdup(ctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_GMALLOC(s)    grn_malloc(&grn_gctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_GCALLOC(s)    grn_calloc(&grn_gctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_GREALLOC(p,s) grn_realloc(&grn_gctx,p,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_GSTRDUP(s)    grn_strdup(&grn_gctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_FREE(p)       grn_free(ctx,p,__FILE__,__LINE__,__FUNCTION__)
#define GRN_MALLOCN(t,n)  ((t *)(GRN_MALLOC(sizeof(t) * (n))))
#define GRN_GFREE(p)      grn_free(&grn_gctx,p,__FILE__,__LINE__,__FUNCTION__)
#define GRN_GMALLOCN(t,n) ((t *)(GRN_GMALLOC(sizeof(t) * (n))))

#define GRN_CTX_ALLOC(ctx,s)   grn_ctx_calloc(ctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CTX_FREE(ctx,p)    grn_ctx_free(ctx,p,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CTX_ALLOC_L(ctx,s) grn_ctx_alloc_lifo(ctx,s,f,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CTX_FREE_L(ctx,p)  grn_ctx_free_lifo(ctx,p,__FILE__,__LINE__,__FUNCTION__)

void *grn_ctx_malloc(grn_ctx *ctx,
                     size_t size,
                     const char *file,
                     int line,
                     const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void *grn_ctx_calloc(grn_ctx *ctx,
                     size_t size,
                     const char *file,
                     int line,
                     const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void *grn_ctx_realloc(grn_ctx *ctx,
                      void *ptr,
                      size_t size,
                      const char *file,
                      int line,
                      const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(3);
char *grn_ctx_strdup(grn_ctx *ctx, const char *s,
                     const char* file, int line, const char *func);
void grn_ctx_free(grn_ctx *ctx, void *ptr,
                  const char* file, int line, const char *func);
void *grn_ctx_alloc_lifo(grn_ctx *ctx,
                         size_t size,
                         const char *file,
                         int line,
                         const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void grn_ctx_free_lifo(grn_ctx *ctx, void *ptr,
                       const char* file, int line, const char *func);

#ifdef USE_DYNAMIC_MALLOC_CHANGE
typedef void *(*grn_malloc_func) (grn_ctx *ctx, size_t size,
                                  const char *file, int line, const char *func);
typedef void *(*grn_calloc_func) (grn_ctx *ctx, size_t size,
                                  const char *file, int line, const char *func);
typedef void *(*grn_realloc_func) (grn_ctx *ctx, void *ptr, size_t size,
                                   const char *file, int line, const char *func);
typedef char *(*grn_strdup_func) (grn_ctx *ctx, const char *string,
                                  const char *file, int line, const char *func);
typedef void (*grn_free_func) (grn_ctx *ctx, void *ptr,
                               const char *file, int line, const char *func);
grn_malloc_func grn_ctx_get_malloc(grn_ctx *ctx);
void grn_ctx_set_malloc(grn_ctx *ctx, grn_malloc_func malloc_func);
grn_calloc_func grn_ctx_get_calloc(grn_ctx *ctx);
void grn_ctx_set_calloc(grn_ctx *ctx, grn_calloc_func calloc_func);
grn_realloc_func grn_ctx_get_realloc(grn_ctx *ctx);
void grn_ctx_set_realloc(grn_ctx *ctx, grn_realloc_func realloc_func);
grn_strdup_func grn_ctx_get_strdup(grn_ctx *ctx);
void grn_ctx_set_strdup(grn_ctx *ctx, grn_strdup_func strdup_func);
grn_free_func grn_ctx_get_free(grn_ctx *ctx);
void grn_ctx_set_free(grn_ctx *ctx, grn_free_func free_func);

void *grn_malloc(grn_ctx *ctx,
                 size_t size,
                 const char *file,
                 int line,
                 const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void *grn_calloc(grn_ctx *ctx,
                 size_t size,
                 const char *file,
                 int line,
                 const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void *grn_realloc(grn_ctx *ctx,
                  void *ptr,
                  size_t size,
                  const char *file,
                  int line,
                  const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(3);
char *grn_strdup(grn_ctx *ctx, const char *s, const char* file, int line, const char *func);
void grn_free(grn_ctx *ctx, void *ptr, const char *file, int line, const char *func);
#else
#  define grn_malloc  grn_malloc_default
#  define grn_calloc  grn_calloc_default
#  define grn_realloc grn_realloc_default
#  define grn_strdup  grn_strdup_default
#  define grn_free    grn_free_default
#endif

GRN_API void *grn_malloc_default(grn_ctx *ctx,
                                 size_t size,
                                 const char *file,
                                 int line,
                                 const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void *grn_calloc_default(grn_ctx *ctx,
                         size_t size,
                         const char *file,
                         int line,
                         const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(2);
void *grn_realloc_default(grn_ctx *ctx,
                          void *ptr,
                          size_t size,
                          const char *file,
                          int line,
                          const char *func) GRN_ATTRIBUTE_ALLOC_SIZE(3);
GRN_API char *grn_strdup_default(grn_ctx *ctx, const char *s, const char* file, int line, const char *func);
GRN_API void grn_free_default(grn_ctx *ctx, void *ptr, const char* file, int line, const char *func);

#ifdef USE_FAIL_MALLOC
int grn_fail_malloc_check(size_t size, const char *file, int line, const char *func);
void *grn_malloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_calloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_realloc_fail(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func);
char *grn_strdup_fail(grn_ctx *ctx, const char *s, const char* file, int line, const char *func);
#endif

int grn_alloc_count(void);

#ifdef __cplusplus
}
#endif
