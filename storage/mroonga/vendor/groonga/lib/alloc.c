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

#include "grn.h"
#include "grn_alloc.h"
#include "grn_ctx_impl.h"

static int alloc_count = 0;

#ifdef USE_FAIL_MALLOC
static int grn_fmalloc_prob = 0;
static char *grn_fmalloc_func = NULL;
static char *grn_fmalloc_file = NULL;
static int grn_fmalloc_line = 0;
#endif /* USE_FAIL_MALLOC */

#ifdef USE_EXACT_ALLOC_COUNT
# define GRN_ADD_ALLOC_COUNT(count) do { \
  uint32_t alloced; \
  GRN_ATOMIC_ADD_EX(&alloc_count, count, alloced); \
} while (0)
#else /* USE_EXACT_ALLOC_COUNT */
# define GRN_ADD_ALLOC_COUNT(count) do { \
  alloc_count += count; \
} while (0)
#endif

void
grn_alloc_init_from_env(void)
{
#ifdef USE_FAIL_MALLOC
  {
    char grn_fmalloc_prob_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_FMALLOC_PROB",
               grn_fmalloc_prob_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_fmalloc_prob_env[0]) {
      char grn_fmalloc_seed_env[GRN_ENV_BUFFER_SIZE];
      grn_fmalloc_prob = strtod(grn_fmalloc_prob_env, 0) * RAND_MAX;
      grn_getenv("GRN_FMALLOC_SEED",
                 grn_fmalloc_seed_env,
                 GRN_ENV_BUFFER_SIZE);
      if (grn_fmalloc_seed_env[0]) {
        srand((unsigned int)atoi(grn_fmalloc_seed_env));
      } else {
        srand((unsigned int)time(NULL));
      }
    }
  }
  {
    static char grn_fmalloc_func_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_FMALLOC_FUNC",
               grn_fmalloc_func_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_fmalloc_func_env[0]) {
      grn_fmalloc_func = grn_fmalloc_func_env;
    }
  }
  {
    static char grn_fmalloc_file_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_FMALLOC_FILE",
               grn_fmalloc_file_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_fmalloc_file_env[0]) {
      grn_fmalloc_file = grn_fmalloc_file_env;
    }
  }
  {
    char grn_fmalloc_line_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_FMALLOC_LINE",
               grn_fmalloc_line_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_fmalloc_line_env[0]) {
      grn_fmalloc_line = atoi(grn_fmalloc_line_env);
    }
  }
#endif /* USE_FAIL_MALLOC */
}

#ifdef USE_MEMORY_DEBUG
static grn_critical_section grn_alloc_info_lock;

void
grn_alloc_info_init(void)
{
  CRITICAL_SECTION_INIT(grn_alloc_info_lock);
}

void
grn_alloc_info_fin(void)
{
  CRITICAL_SECTION_FIN(grn_alloc_info_lock);
}

inline static void
grn_alloc_info_set_backtrace(char *buffer, size_t size)
{
# ifdef HAVE_BACKTRACE
#  define N_TRACE_LEVEL 100
  static void *trace[N_TRACE_LEVEL];
  char **symbols;
  int i, n, rest;

  rest = size;
  n = backtrace(trace, N_TRACE_LEVEL);
  symbols = backtrace_symbols(trace, n);
  if (symbols) {
    for (i = 0; i < n; i++) {
      int symbol_length;

      symbol_length = strlen(symbols[i]);
      if (symbol_length + 2 > rest) {
        break;
      }
      grn_memcpy(buffer, symbols[i], symbol_length);
      buffer += symbol_length;
      rest -= symbol_length;
      buffer[0] = '\n';
      buffer++;
      rest--;
      buffer[0] = '\0';
      rest--;
    }
    free(symbols);
  } else {
    buffer[0] = '\0';
  }
#  undef N_TRACE_LEVEL
# else /* HAVE_BACKTRACE */
  buffer[0] = '\0';
# endif /* HAVE_BACKTRACE */
}

inline static void
grn_alloc_info_add(void *address, size_t size,
                   const char *file, int line, const char *func)
{
  grn_ctx *ctx;
  grn_alloc_info *new_alloc_info;

  ctx = &grn_gctx;
  if (!ctx->impl) { return; }

  CRITICAL_SECTION_ENTER(grn_alloc_info_lock);
  new_alloc_info = malloc(sizeof(grn_alloc_info));
  if (new_alloc_info) {
    new_alloc_info->address = address;
    new_alloc_info->size = size;
    new_alloc_info->freed = GRN_FALSE;
    grn_alloc_info_set_backtrace(new_alloc_info->alloc_backtrace,
                                 sizeof(new_alloc_info->alloc_backtrace));
    if (file) {
      new_alloc_info->file = strdup(file);
    } else {
      new_alloc_info->file = NULL;
    }
    new_alloc_info->line = line;
    if (func) {
      new_alloc_info->func = strdup(func);
    } else {
      new_alloc_info->func = NULL;
    }
    new_alloc_info->next = ctx->impl->alloc_info;
    ctx->impl->alloc_info = new_alloc_info;
  }
  CRITICAL_SECTION_LEAVE(grn_alloc_info_lock);
}

inline static void
grn_alloc_info_change(void *old_address, void *new_address, size_t size)
{
  grn_ctx *ctx;
  grn_alloc_info *alloc_info;

  ctx = &grn_gctx;
  if (!ctx->impl) { return; }

  CRITICAL_SECTION_ENTER(grn_alloc_info_lock);
  alloc_info = ctx->impl->alloc_info;
  for (; alloc_info; alloc_info = alloc_info->next) {
    if (alloc_info->address == old_address) {
      alloc_info->address = new_address;
      alloc_info->size = size;
      grn_alloc_info_set_backtrace(alloc_info->alloc_backtrace,
                                   sizeof(alloc_info->alloc_backtrace));
    }
  }
  CRITICAL_SECTION_LEAVE(grn_alloc_info_lock);
}

void
grn_alloc_info_dump(grn_ctx *ctx)
{
  int i = 0;
  grn_alloc_info *alloc_info;

  if (!ctx) { return; }
  if (!ctx->impl) { return; }

  alloc_info = ctx->impl->alloc_info;
  for (; alloc_info; alloc_info = alloc_info->next) {
    if (alloc_info->freed) {
      printf("address[%d][freed]: %p(%" GRN_FMT_SIZE ")\n",
             i, alloc_info->address, alloc_info->size);
    } else {
      printf("address[%d][not-freed]: %p(%" GRN_FMT_SIZE "): %s:%d: %s()\n%s",
             i,
             alloc_info->address,
             alloc_info->size,
             alloc_info->file ? alloc_info->file : "(unknown)",
             alloc_info->line,
             alloc_info->func ? alloc_info->func : "(unknown)",
             alloc_info->alloc_backtrace);
    }
    i++;
  }
}

inline static void
grn_alloc_info_check(grn_ctx *ctx, void *address)
{
  grn_alloc_info *alloc_info;

  if (!grn_gctx.impl) { return; }
  /* grn_alloc_info_dump(ctx); */

  CRITICAL_SECTION_ENTER(grn_alloc_info_lock);
  alloc_info = grn_gctx.impl->alloc_info;
  for (; alloc_info; alloc_info = alloc_info->next) {
    if (alloc_info->address == address) {
      if (alloc_info->freed) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "double free: %p(%" GRN_FMT_SIZE "):\n"
                "alloc backtrace:\n"
                "%sfree backtrace:\n"
                "%s",
                alloc_info->address,
                alloc_info->size,
                alloc_info->alloc_backtrace,
                alloc_info->free_backtrace);
      } else {
        alloc_info->freed = GRN_TRUE;
        grn_alloc_info_set_backtrace(alloc_info->free_backtrace,
                                     sizeof(alloc_info->free_backtrace));
      }
      break;
    }
  }
  CRITICAL_SECTION_LEAVE(grn_alloc_info_lock);
}

void
grn_alloc_info_free(grn_ctx *ctx)
{
  grn_alloc_info *alloc_info;

  if (!ctx) { return; }
  if (!ctx->impl) { return; }

  alloc_info = ctx->impl->alloc_info;
  while (alloc_info) {
    grn_alloc_info *current_alloc_info = alloc_info;
    alloc_info = alloc_info->next;
    current_alloc_info->next = NULL;
    free(current_alloc_info->file);
    free(current_alloc_info->func);
    free(current_alloc_info);
  }
  ctx->impl->alloc_info = NULL;
}

#else /* USE_MEMORY_DEBUG */
void
grn_alloc_info_init(void)
{
}

void
grn_alloc_info_fin(void)
{
}

#  define grn_alloc_info_add(address, size, file, line, func)
#  define grn_alloc_info_change(old_address, new_address, size)
#  define grn_alloc_info_check(ctx, address)

void
grn_alloc_info_dump(grn_ctx *ctx)
{
}

void
grn_alloc_info_free(grn_ctx *ctx)
{
}
#endif /* USE_MEMORY_DEBUG */

#define GRN_CTX_SEGMENT_SIZE    (1U <<22)
#define GRN_CTX_SEGMENT_MASK    (GRN_CTX_SEGMENT_SIZE - 1)

#define GRN_CTX_SEGMENT_WORD    (1U <<31)
#define GRN_CTX_SEGMENT_VLEN    (1U <<30)
#define GRN_CTX_SEGMENT_LIFO    (1U <<29)
#define GRN_CTX_SEGMENT_DIRTY   (1U <<28)

void
grn_alloc_init_ctx_impl(grn_ctx *ctx)
{
#ifdef USE_DYNAMIC_MALLOC_CHANGE
# ifdef USE_FAIL_MALLOC
  ctx->impl->malloc_func = grn_malloc_fail;
  ctx->impl->calloc_func = grn_calloc_fail;
  ctx->impl->realloc_func = grn_realloc_fail;
  ctx->impl->strdup_func = grn_strdup_fail;
# else
  ctx->impl->malloc_func = grn_malloc_default;
  ctx->impl->calloc_func = grn_calloc_default;
  ctx->impl->realloc_func = grn_realloc_default;
  ctx->impl->strdup_func = grn_strdup_default;
# endif
#endif

#ifdef USE_MEMORY_DEBUG
  ctx->impl->alloc_info = NULL;
#endif
}

void
grn_alloc_fin_ctx_impl(grn_ctx *ctx)
{
  int i;
  grn_io_mapinfo *mi;
  for (i = 0, mi = ctx->impl->segs; i < GRN_CTX_N_SEGMENTS; i++, mi++) {
    if (mi->map) {
      //GRN_LOG(ctx, GRN_LOG_NOTICE, "unmap in ctx_fin(%d,%d,%d)", i, (mi->count & GRN_CTX_SEGMENT_MASK), mi->nref);
      if (mi->count & GRN_CTX_SEGMENT_VLEN) {
        grn_io_anon_unmap(ctx, mi, mi->nref * grn_pagesize);
      } else {
        grn_io_anon_unmap(ctx, mi, GRN_CTX_SEGMENT_SIZE);
      }
    }
  }
}

#define ALIGN_SIZE (1<<3)
#define ALIGN_MASK (ALIGN_SIZE-1)
#define GRN_CTX_ALLOC_CLEAR 1

static void *
grn_ctx_alloc(grn_ctx *ctx, size_t size, int flags,
              const char* file, int line, const char *func)
{
  void *res = NULL;
  if (!ctx) { return res; }
  if (!ctx->impl) {
    if (ERRP(ctx, GRN_ERROR)) { return res; }
  }
  CRITICAL_SECTION_ENTER(ctx->impl->lock);
  {
    int32_t i;
    int32_t *header;
    grn_io_mapinfo *mi;
    size = ((size + ALIGN_MASK) & ~ALIGN_MASK) + ALIGN_SIZE;
    if (size > GRN_CTX_SEGMENT_SIZE) {
      uint64_t npages = (size + (grn_pagesize - 1)) / grn_pagesize;
      size_t aligned_size;
      if (npages >= (1LL<<32)) {
        MERR("too long request size=%" GRN_FMT_SIZE, size);
        goto exit;
      }
      for (i = 0, mi = ctx->impl->segs;; i++, mi++) {
        if (i >= GRN_CTX_N_SEGMENTS) {
          MERR("all segments are full");
          goto exit;
        }
        if (!mi->map) { break; }
      }
      aligned_size = grn_pagesize * ((size_t)npages);
      if (!grn_io_anon_map(ctx, mi, aligned_size)) { goto exit; }
      /* GRN_LOG(ctx, GRN_LOG_NOTICE, "map i=%d (%d)", i, npages * grn_pagesize); */
      mi->nref = (uint32_t) npages;
      mi->count = GRN_CTX_SEGMENT_VLEN;
      ctx->impl->currseg = -1;
      header = mi->map;
      header[0] = i;
      header[1] = (int32_t) size;
    } else {
      if ((i = ctx->impl->currseg) >= 0)
        mi = &ctx->impl->segs[i];
      if (i < 0 || size + mi->nref > GRN_CTX_SEGMENT_SIZE) {
        for (i = 0, mi = ctx->impl->segs;; i++, mi++) {
          if (i >= GRN_CTX_N_SEGMENTS) {
            MERR("all segments are full");
            goto exit;
          }
          if (!mi->map) { break; }
        }
        if (!grn_io_anon_map(ctx, mi, GRN_CTX_SEGMENT_SIZE)) { goto exit; }
        /* GRN_LOG(ctx, GRN_LOG_NOTICE, "map i=%d", i); */
        mi->nref = 0;
        mi->count = GRN_CTX_SEGMENT_WORD;
        ctx->impl->currseg = i;
      }
      header = (int32_t *)((byte *)mi->map + mi->nref);
      mi->nref += size;
      mi->count++;
      header[0] = i;
      header[1] = (int32_t) size;
      if ((flags & GRN_CTX_ALLOC_CLEAR) &&
          (mi->count & GRN_CTX_SEGMENT_DIRTY) && (size > ALIGN_SIZE)) {
        memset(&header[2], 0, size - ALIGN_SIZE);
      }
    }
    /*
    {
      char g = (ctx == &grn_gctx) ? 'g' : ' ';
      GRN_LOG(ctx, GRN_LOG_NOTICE, "+%c(%p) %s:%d(%s) (%d:%d)%p mi(%d:%d)", g, ctx, file, line, func, header[0], header[1], &header[2], mi->nref, (mi->count & GRN_CTX_SEGMENT_MASK));
    }
    */
    res = &header[2];
  }
exit :
  CRITICAL_SECTION_LEAVE(ctx->impl->lock);
  return res;
}

void *
grn_ctx_malloc(grn_ctx *ctx, size_t size,
              const char* file, int line, const char *func)
{
  return grn_ctx_alloc(ctx, size, 0, file, line, func);
}

void *
grn_ctx_calloc(grn_ctx *ctx, size_t size,
              const char* file, int line, const char *func)
{
  return grn_ctx_alloc(ctx, size, GRN_CTX_ALLOC_CLEAR, file, line, func);
}

void *
grn_ctx_realloc(grn_ctx *ctx, void *ptr, size_t size,
                const char* file, int line, const char *func)
{
  void *res = NULL;
  if (size) {
    /* todo : expand if possible */
    res = grn_ctx_alloc(ctx, size, 0, file, line, func);
    if (res && ptr) {
      int32_t *header = &((int32_t *)ptr)[-2];
      size_t size_ = header[1];
      grn_memcpy(res, ptr, size_ > size ? size : size_);
      grn_ctx_free(ctx, ptr, file, line, func);
    }
  } else {
    grn_ctx_free(ctx, ptr, file, line, func);
  }
  return res;
}

char *
grn_ctx_strdup(grn_ctx *ctx, const char *s,
               const char* file, int line, const char *func)
{
  void *res = NULL;
  if (s) {
    size_t size = strlen(s) + 1;
    if ((res = grn_ctx_alloc(ctx, size, 0, file, line, func))) {
      grn_memcpy(res, s, size);
    }
  }
  return res;
}

void
grn_ctx_free(grn_ctx *ctx, void *ptr,
             const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  if (!ctx->impl) {
    ERR(GRN_INVALID_ARGUMENT,"ctx without impl passed.");
    return;
  }
  CRITICAL_SECTION_ENTER(ctx->impl->lock);
  if (ptr) {
    int32_t *header = &((int32_t *)ptr)[-2];

    if (header[0] >= GRN_CTX_N_SEGMENTS) {
      ERR(GRN_INVALID_ARGUMENT,"invalid ptr passed. ptr=%p seg=%d", ptr, *header);
      goto exit;
    }
    /*
    {
      int32_t i = header[0];
      char c = 'X', g = (ctx == &grn_gctx) ? 'g' : ' ';
      grn_io_mapinfo *mi = &ctx->impl->segs[i];
      if (!(mi->count & GRN_CTX_SEGMENT_VLEN) &&
          mi->map <= (void *)header && (char *)header < ((char *)mi->map + GRN_CTX_SEGMENT_SIZE)) { c = '-'; }
      GRN_LOG(ctx, GRN_LOG_NOTICE, "%c%c(%p) %s:%d(%s) (%d:%d)%p mi(%d:%d)", c, g, ctx, file, line, func, header[0], header[1], &header[2], mi->nref, (mi->count & GRN_CTX_SEGMENT_MASK));
    }
    */
    {
      int32_t i = header[0];
      grn_io_mapinfo *mi = &ctx->impl->segs[i];
      if (mi->count & GRN_CTX_SEGMENT_VLEN) {
        if (mi->map != header) {
          ERR(GRN_INVALID_ARGUMENT,"invalid ptr passed.. ptr=%p seg=%d", ptr, i);
          goto exit;
        }
        //GRN_LOG(ctx, GRN_LOG_NOTICE, "umap i=%d (%d)", i, mi->nref * grn_pagesize);
        grn_io_anon_unmap(ctx, mi, mi->nref * grn_pagesize);
        mi->map = NULL;
      } else {
        if (!mi->map) {
          ERR(GRN_INVALID_ARGUMENT,"invalid ptr passed... ptr=%p seg=%d", ptr, i);
          goto exit;
        }
        mi->count--;
        if (!(mi->count & GRN_CTX_SEGMENT_MASK)) {
          //GRN_LOG(ctx, GRN_LOG_NOTICE, "umap i=%d", i);
          if (i == ctx->impl->currseg) {
            mi->count |= GRN_CTX_SEGMENT_DIRTY;
            mi->nref = 0;
          } else {
            grn_io_anon_unmap(ctx, mi, GRN_CTX_SEGMENT_SIZE);
            mi->map = NULL;
          }
        }
      }
    }
  }
exit :
  CRITICAL_SECTION_LEAVE(ctx->impl->lock);
}

void *
grn_ctx_alloc_lifo(grn_ctx *ctx, size_t size,
                   const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  if (!ctx->impl) {
    if (ERRP(ctx, GRN_ERROR)) { return NULL; }
  }
  {
    int32_t i = ctx->impl->lifoseg;
    grn_io_mapinfo *mi = &ctx->impl->segs[i];
    if (size > GRN_CTX_SEGMENT_SIZE) {
      uint64_t npages = (size + (grn_pagesize - 1)) / grn_pagesize;
      size_t aligned_size;
      if (npages >= (1LL<<32)) {
        MERR("too long request size=%" GRN_FMT_SIZE, size);
        return NULL;
      }
      for (;;) {
        if (++i >= GRN_CTX_N_SEGMENTS) {
          MERR("all segments are full");
          return NULL;
        }
        mi++;
        if (!mi->map) { break; }
      }
      aligned_size = grn_pagesize * ((size_t)npages);
      if (!grn_io_anon_map(ctx, mi, aligned_size)) { return NULL; }
      mi->nref = (uint32_t) npages;
      mi->count = GRN_CTX_SEGMENT_VLEN|GRN_CTX_SEGMENT_LIFO;
      ctx->impl->lifoseg = i;
      return mi->map;
    } else {
      size = (size + ALIGN_MASK) & ~ALIGN_MASK;
      if (i < 0 || (mi->count & GRN_CTX_SEGMENT_VLEN) || size + mi->nref > GRN_CTX_SEGMENT_SIZE) {
        for (;;) {
          if (++i >= GRN_CTX_N_SEGMENTS) {
            MERR("all segments are full");
            return NULL;
          }
          if (!(++mi)->map) { break; }
        }
        if (!grn_io_anon_map(ctx, mi, GRN_CTX_SEGMENT_SIZE)) { return NULL; }
        mi->nref = 0;
        mi->count = GRN_CTX_SEGMENT_WORD|GRN_CTX_SEGMENT_LIFO;
        ctx->impl->lifoseg = i;
      }
      {
        uint32_t u = mi->nref;
        mi->nref += size;
        return (byte *)mi->map + u;
      }
    }
  }
}

void
grn_ctx_free_lifo(grn_ctx *ctx, void *ptr,
                  const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  if (!ctx->impl) {
    ERR(GRN_INVALID_ARGUMENT,"ctx without impl passed.");
    return;
  }
  {
    int32_t i = ctx->impl->lifoseg, done = 0;
    grn_io_mapinfo *mi = &ctx->impl->segs[i];
    if (i < 0) {
      ERR(GRN_INVALID_ARGUMENT, "lifo buffer is void");
      return;
    }
    for (; i >= 0; i--, mi--) {
      if (!(mi->count & GRN_CTX_SEGMENT_LIFO)) { continue; }
      if (done) { break; }
      if (mi->count & GRN_CTX_SEGMENT_VLEN) {
        if (mi->map == ptr) { done = 1; }
        grn_io_anon_unmap(ctx, mi, mi->nref * grn_pagesize);
        mi->map = NULL;
      } else {
        if (mi->map == ptr) {
          done = 1;
        } else {
          if (mi->map < ptr && ptr < (void *)((byte*)mi->map + mi->nref)) {
            mi->nref = (uint32_t) ((uintptr_t)ptr - (uintptr_t)mi->map);
            break;
          }
        }
        grn_io_anon_unmap(ctx, mi, GRN_CTX_SEGMENT_SIZE);
        mi->map = NULL;
      }
    }
    ctx->impl->lifoseg = i;
  }
}

#if defined(USE_DYNAMIC_MALLOC_CHANGE)
grn_malloc_func
grn_ctx_get_malloc(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->malloc_func;
}

void
grn_ctx_set_malloc(grn_ctx *ctx, grn_malloc_func malloc_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->malloc_func = malloc_func;
}

grn_calloc_func
grn_ctx_get_calloc(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->calloc_func;
}

void
grn_ctx_set_calloc(grn_ctx *ctx, grn_calloc_func calloc_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->calloc_func = calloc_func;
}

grn_realloc_func
grn_ctx_get_realloc(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->realloc_func;
}

void
grn_ctx_set_realloc(grn_ctx *ctx, grn_realloc_func realloc_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->realloc_func = realloc_func;
}

grn_strdup_func
grn_ctx_get_strdup(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->strdup_func;
}

void
grn_ctx_set_strdup(grn_ctx *ctx, grn_strdup_func strdup_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->strdup_func = strdup_func;
}

grn_free_func
grn_ctx_get_free(grn_ctx *ctx)
{
  if (!ctx || !ctx->impl) { return NULL; }
  return ctx->impl->free_func;
}

void
grn_ctx_set_free(grn_ctx *ctx, grn_free_func free_func)
{
  if (!ctx || !ctx->impl) { return; }
  ctx->impl->free_func = free_func;
}

void *
grn_malloc(grn_ctx *ctx, size_t size,
           const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->malloc_func) {
    return ctx->impl->malloc_func(ctx, size, file, line, func);
  } else {
    return grn_malloc_default(ctx, size, file, line, func);
  }
}

void *
grn_calloc(grn_ctx *ctx, size_t size,
           const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->calloc_func) {
    return ctx->impl->calloc_func(ctx, size, file, line, func);
  } else {
    return grn_calloc_default(ctx, size, file, line, func);
  }
}

void *
grn_realloc(grn_ctx *ctx, void *ptr, size_t size,
            const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->realloc_func) {
    return ctx->impl->realloc_func(ctx, ptr, size, file, line, func);
  } else {
    return grn_realloc_default(ctx, ptr, size, file, line, func);
  }
}

char *
grn_strdup(grn_ctx *ctx, const char *string,
           const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->strdup_func) {
    return ctx->impl->strdup_func(ctx, string, file, line, func);
  } else {
    return grn_strdup_default(ctx, string, file, line, func);
  }
}

void
grn_free(grn_ctx *ctx, void *ptr,
         const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->free_func) {
    return ctx->impl->free_func(ctx, ptr, file, line, func);
  } else {
    return grn_free_default(ctx, ptr, file, line, func);
  }
}
#endif

void *
grn_malloc_default(grn_ctx *ctx, size_t size,
                   const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    void *res = malloc(size);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, size, file, line, func);
    } else {
      if (!(res = malloc(size))) {
        MERR("malloc fail (%" GRN_FMT_SIZE ")=%p (%s:%d) <%d>",
             size, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
        grn_alloc_info_add(res, size, file, line, func);
      }
    }
    return res;
  }
}

void *
grn_calloc_default(grn_ctx *ctx, size_t size,
                   const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    void *res = calloc(size, 1);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, size, file, line, func);
    } else {
      if (!(res = calloc(size, 1))) {
        MERR("calloc fail (%" GRN_FMT_SIZE ")=%p (%s:%d) <%d>",
             size, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
        grn_alloc_info_add(res, size, file, line, func);
      }
    }
    return res;
  }
}

void
grn_free_default(grn_ctx *ctx, void *ptr,
                 const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  grn_alloc_info_check(ctx, ptr);
  {
    free(ptr);
    if (ptr) {
      GRN_ADD_ALLOC_COUNT(-1);
    } else {
      GRN_LOG(ctx, GRN_LOG_ALERT, "free fail (%p) (%s:%d) <%d>",
              ptr, file, line, alloc_count);
    }
  }
}

void *
grn_realloc_default(grn_ctx *ctx, void *ptr, size_t size,
                    const char* file, int line, const char *func)
{
  void *res;
  if (!ctx) { return NULL; }
  if (size) {
    if (!(res = realloc(ptr, size))) {
      if (!(res = realloc(ptr, size))) {
        MERR("realloc fail (%p,%" GRN_FMT_SIZE ")=%p (%s:%d) <%d>",
             ptr, size, res, file, line, alloc_count);
        return NULL;
      }
    }
    if (ptr) {
      grn_alloc_info_change(ptr, res, size);
    } else {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, size, file, line, func);
    }
  } else {
    if (!ptr) { return NULL; }
    grn_alloc_info_check(ctx, ptr);
    GRN_ADD_ALLOC_COUNT(-1);
    free(ptr);
    res = NULL;
  }
  return res;
}

int
grn_alloc_count(void)
{
  return alloc_count;
}

char *
grn_strdup_default(grn_ctx *ctx, const char *s,
                   const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    char *res = grn_strdup_raw(s);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, strlen(res) + 1, file, line, func);
    } else {
      if (!(res = grn_strdup_raw(s))) {
        MERR("strdup(%p)=%p (%s:%d) <%d>", s, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
        grn_alloc_info_add(res, strlen(res) + 1, file, line, func);
      }
    }
    return res;
  }
}

#ifdef USE_FAIL_MALLOC
int
grn_fail_malloc_check(size_t size,
                      const char *file, int line, const char *func)
{
  if ((grn_fmalloc_file && strcmp(file, grn_fmalloc_file)) ||
      (grn_fmalloc_line && line != grn_fmalloc_line) ||
      (grn_fmalloc_func && strcmp(func, grn_fmalloc_func))) {
    return 1;
  }
  if (grn_fmalloc_prob && grn_fmalloc_prob >= rand()) {
    return 0;
  }
  return 1;
}

void *
grn_malloc_fail(grn_ctx *ctx, size_t size,
                const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_malloc_default(ctx, size, file, line, func);
  } else {
    MERR("fail_malloc (%" GRN_FMT_SIZE ") (%s:%d@%s) <%d>",
         size, file, line, func, alloc_count);
    return NULL;
  }
}

void *
grn_calloc_fail(grn_ctx *ctx, size_t size,
                const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_calloc_default(ctx, size, file, line, func);
  } else {
    MERR("fail_calloc (%" GRN_FMT_SIZE ") (%s:%d@%s) <%d>",
         size, file, line, func, alloc_count);
    return NULL;
  }
}

void *
grn_realloc_fail(grn_ctx *ctx, void *ptr, size_t size,
                 const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_realloc_default(ctx, ptr, size, file, line, func);
  } else {
    MERR("fail_realloc (%p,%" GRN_FMT_SIZE ") (%s:%d@%s) <%d>",
         ptr, size, file, line, func, alloc_count);
    return NULL;
  }
}

char *
grn_strdup_fail(grn_ctx *ctx, const char *s,
                const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(strlen(s), file, line, func)) {
    return grn_strdup_default(ctx, s, file, line, func);
  } else {
    MERR("fail_strdup(%p) (%s:%d@%s) <%d>", s, file, line, func, alloc_count);
    return NULL;
  }
}
#endif /* USE_FAIL_MALLOC */
