/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2015 Brazil

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
#include <string.h>
#include "grn_request_canceler.h"
#include "grn_tokenizers.h"
#include "grn_ctx_impl.h"
#include "grn_pat.h"
#include "grn_plugin.h"
#include "grn_snip.h"
#include "grn_output.h"
#include "grn_normalizer.h"
#include "grn_ctx_impl_mrb.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif /* HAVE_NETINET_IN_H */

#define GRN_CTX_INITIALIZER(enc) \
  { GRN_SUCCESS, 0, enc, 0, GRN_LOG_NOTICE,\
    GRN_CTX_FIN, 0, 0, 0, 0, {0}, NULL, NULL, NULL, NULL, NULL }

#define GRN_CTX_CLOSED(ctx) ((ctx)->stat == GRN_CTX_FIN)

#ifdef USE_EXACT_ALLOC_COUNT
#define GRN_ADD_ALLOC_COUNT(count) do { \
  uint32_t alloced; \
  GRN_ATOMIC_ADD_EX(&alloc_count, count, alloced); \
} while (0)
#else /* USE_EXACT_ALLOC_COUNT */
#define GRN_ADD_ALLOC_COUNT(count) do { \
  alloc_count += count; \
} while (0)
#endif

grn_ctx grn_gctx = GRN_CTX_INITIALIZER(GRN_ENC_DEFAULT);
int grn_pagesize;
grn_critical_section grn_glock;
uint32_t grn_gtick;
int grn_lock_timeout = GRN_LOCK_TIMEOUT;

#ifdef USE_UYIELD
int grn_uyield_count = 0;
#endif

void
grn_sleep(uint32_t seconds)
{
#ifdef WIN32
  Sleep(seconds * 1000);
#else  // WIN32
  sleep(seconds);
#endif  // WIN32
}

void
grn_nanosleep(uint64_t nanoseconds)
{
#ifdef WIN32
  Sleep((DWORD)(nanoseconds / 1000000));
#else  // WIN32
  struct timespec interval;
  interval.tv_sec = (time_t)(nanoseconds / 1000000000);
  interval.tv_nsec = (long)(nanoseconds % 1000000000);
  nanosleep(&interval, NULL);
#endif  // WIN32
}

/* fixme by 2038 */

grn_rc
grn_timeval_now(grn_ctx *ctx, grn_timeval *tv)
{
#ifdef HAVE_CLOCK_GETTIME
  struct timespec t;
  if (clock_gettime(CLOCK_REALTIME, &t)) {
    SERR("clock_gettime");
  } else {
    tv->tv_sec = t.tv_sec;
    tv->tv_nsec = t.tv_nsec;
  }
  return ctx->rc;
#else /* HAVE_CLOCK_GETTIME */
#ifdef WIN32
  time_t t;
  struct _timeb tb;
  time(&t);
  _ftime(&tb);
  tv->tv_sec = t;
  tv->tv_nsec = tb.millitm * (GRN_TIME_NSEC_PER_SEC / 1000);
  return GRN_SUCCESS;
#else /* WIN32 */
  struct timeval t;
  if (gettimeofday(&t, NULL)) {
    SERR("gettimeofday");
  } else {
    tv->tv_sec = t.tv_sec;
    tv->tv_nsec = GRN_TIME_USEC_TO_NSEC(t.tv_usec);
  }
  return ctx->rc;
#endif /* WIN32 */
#endif /* HAVE_CLOCK_GETTIME */
}

void
grn_time_now(grn_ctx *ctx, grn_obj *obj)
{
  grn_timeval tv;
  grn_timeval_now(ctx, &tv);
  GRN_TIME_SET(ctx, obj, GRN_TIME_PACK(tv.tv_sec,
                                       GRN_TIME_NSEC_TO_USEC(tv.tv_nsec)));
}

grn_rc
grn_timeval2str(grn_ctx *ctx, grn_timeval *tv, char *buf)
{
  struct tm *ltm;
  const char *function_name;
#ifdef HAVE__LOCALTIME64_S
  struct tm tm;
  time_t t = tv->tv_sec;
  function_name = "localtime_s";
  ltm = (localtime_s(&tm, &t) == 0) ? &tm : NULL;
#else /* HAVE__LOCALTIME64_S */
# ifdef HAVE_LOCALTIME_R
  struct tm tm;
  time_t t = tv->tv_sec;
  function_name = "localtime_r";
  ltm = localtime_r(&t, &tm);
# else /* HAVE_LOCALTIME_R */
  time_t tvsec = (time_t) tv->tv_sec;
  function_name = "localtime";
  ltm = localtime(&tvsec);
# endif /* HAVE_LOCALTIME_R */
#endif /* HAVE__LOCALTIME64_S */
  if (!ltm) { SERR(function_name); }
  snprintf(buf, GRN_TIMEVAL_STR_SIZE - 1, GRN_TIMEVAL_STR_FORMAT,
           ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday,
           ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
           (int)(GRN_TIME_NSEC_TO_USEC(tv->tv_nsec)));
  buf[GRN_TIMEVAL_STR_SIZE - 1] = '\0';
  return ctx->rc;
}

grn_rc
grn_str2timeval(const char *str, uint32_t str_len, grn_timeval *tv)
{
  struct tm tm;
  const char *r1, *r2, *rend = str + str_len;
  uint32_t uv;
  memset(&tm, 0, sizeof(struct tm));

  tm.tm_year = (int)grn_atoui(str, rend, &r1) - 1900;
  if ((r1 + 1) >= rend || (*r1 != '/' && *r1 != '-') ||
      tm.tm_year < 0) { return GRN_INVALID_ARGUMENT; }
  r1++;
  tm.tm_mon = (int)grn_atoui(r1, rend, &r1) - 1;
  if ((r1 + 1) >= rend || (*r1 != '/' && *r1 != '-') ||
      tm.tm_mon < 0 || tm.tm_mon >= 12) { return GRN_INVALID_ARGUMENT; }
  r1++;
  tm.tm_mday = (int)grn_atoui(r1, rend, &r1);
  if ((r1 + 1) >= rend || *r1 != ' ' ||
      tm.tm_mday < 1 || tm.tm_mday > 31) { return GRN_INVALID_ARGUMENT; }

  tm.tm_hour = (int)grn_atoui(++r1, rend, &r2);
  if ((r2 + 1) >= rend || r1 == r2 || *r2 != ':' ||
      tm.tm_hour < 0 || tm.tm_hour >= 24) {
    return GRN_INVALID_ARGUMENT;
  }
  r1 = r2 + 1;
  tm.tm_min = (int)grn_atoui(r1, rend, &r2);
  if ((r2 + 1) >= rend || r1 == r2 || *r2 != ':' ||
      tm.tm_min < 0 || tm.tm_min >= 60) {
    return GRN_INVALID_ARGUMENT;
  }
  r1 = r2 + 1;
  tm.tm_sec = (int)grn_atoui(r1, rend, &r2);
  if (r1 == r2 ||
      tm.tm_sec < 0 || tm.tm_sec > 61 /* leap 2sec */) {
    return GRN_INVALID_ARGUMENT;
  }
  r1 = r2;
  tm.tm_yday = -1;
  tm.tm_isdst = -1;

  /* tm_yday is set appropriately (0-365) on successful completion. */
  tv->tv_sec = mktime(&tm);
  if (tm.tm_yday == -1) { return GRN_INVALID_ARGUMENT; }
  if ((r1 + 1) < rend && *r1 == '.') { r1++; }
  uv = grn_atoi(r1, rend, &r2);
  while (r2 < r1 + 6) {
    uv *= 10;
    r2++;
  }
  if (uv >= GRN_TIME_USEC_PER_SEC) { return GRN_INVALID_ARGUMENT; }
  tv->tv_nsec = GRN_TIME_USEC_TO_NSEC(uv);
  return GRN_SUCCESS;
}

#ifdef USE_MEMORY_DEBUG
inline static void
grn_alloc_info_set_backtrace(char *buffer, size_t size)
{
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
      memcpy(buffer, symbols[i], symbol_length);
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
}

inline static void
grn_alloc_info_add(void *address, const char *file, int line, const char *func)
{
  grn_ctx *ctx;
  grn_alloc_info *new_alloc_info;

  ctx = &grn_gctx;
  if (!ctx->impl) { return; }

  new_alloc_info = malloc(sizeof(grn_alloc_info));
  new_alloc_info->address = address;
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

inline static void
grn_alloc_info_change(void *old_address, void *new_address)
{
  grn_ctx *ctx;
  grn_alloc_info *alloc_info;

  ctx = &grn_gctx;
  if (!ctx->impl) { return; }

  alloc_info = ctx->impl->alloc_info;
  for (; alloc_info; alloc_info = alloc_info->next) {
    if (alloc_info->address == old_address) {
      alloc_info->address = new_address;
      grn_alloc_info_set_backtrace(alloc_info->alloc_backtrace,
                                   sizeof(alloc_info->alloc_backtrace));
    }
  }
}

inline static void
grn_alloc_info_dump(grn_ctx *ctx)
{
  int i = 0;
  grn_alloc_info *alloc_info;

  if (!ctx) { return; }
  if (!ctx->impl) { return; }

  alloc_info = ctx->impl->alloc_info;
  for (; alloc_info; alloc_info = alloc_info->next) {
    if (alloc_info->freed) {
      printf("address[%d][freed]: %p\n", i, alloc_info->address);
    } else {
      printf("address[%d][not-freed]: %p: %s:%d: %s()\n%s",
             i,
             alloc_info->address,
             alloc_info->file ? alloc_info->file : "(unknown)",
             alloc_info->line,
             alloc_info->func ? alloc_info->func : "(unknown)",
             alloc_info->alloc_backtrace);
    }
    i++;
  }
}

inline static void
grn_alloc_info_check(void *address)
{
  grn_ctx *ctx;
  grn_alloc_info *alloc_info;

  ctx = &grn_gctx;
  if (!ctx->impl) { return; }
  /* grn_alloc_info_dump(ctx); */

  alloc_info = ctx->impl->alloc_info;
  for (; alloc_info; alloc_info = alloc_info->next) {
    if (alloc_info->address == address) {
      if (alloc_info->freed) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "double free: (%p):\nalloc backtrace:\n%sfree backtrace:\n%s",
                alloc_info->address,
                alloc_info->alloc_backtrace,
                alloc_info->free_backtrace);
      } else {
        alloc_info->freed = GRN_TRUE;
        grn_alloc_info_set_backtrace(alloc_info->free_backtrace,
                                     sizeof(alloc_info->free_backtrace));
      }
      return;
    }
  }
}

inline static void
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
#  define grn_alloc_info_add(address, file, line, func)
#  define grn_alloc_info_change(old_address, new_address)
#  define grn_alloc_info_check(address)
#  define grn_alloc_info_dump(ctx)
#  define grn_alloc_info_free(ctx)
#endif /* USE_MEMORY_DEBUG */

#ifdef USE_FAIL_MALLOC
int grn_fmalloc_prob = 0;
char *grn_fmalloc_func = NULL;
char *grn_fmalloc_file = NULL;
int grn_fmalloc_line = 0;
#endif /* USE_FAIL_MALLOC */

#define GRN_CTX_SEGMENT_SIZE    (1<<22)
#define GRN_CTX_SEGMENT_MASK    (GRN_CTX_SEGMENT_SIZE - 1)

#define GRN_CTX_SEGMENT_WORD    (1<<31)
#define GRN_CTX_SEGMENT_VLEN    (1<<30)
#define GRN_CTX_SEGMENT_LIFO    (1<<29)
#define GRN_CTX_SEGMENT_DIRTY   (1<<28)

#ifdef USE_DYNAMIC_MALLOC_CHANGE
static void
grn_ctx_impl_init_malloc(grn_ctx *ctx)
{
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
}
#endif

static void
grn_loader_init(grn_loader *loader)
{
  GRN_TEXT_INIT(&loader->values, 0);
  GRN_UINT32_INIT(&loader->level, GRN_OBJ_VECTOR);
  GRN_PTR_INIT(&loader->columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  loader->key_offset = -1;
  loader->table = NULL;
  loader->last = NULL;
  loader->ifexists = NULL;
  loader->each = NULL;
  loader->values_size = 0;
  loader->nrecords = 0;
  loader->stat = GRN_LOADER_BEGIN;
}

void
grn_ctx_loader_clear(grn_ctx *ctx)
{
  grn_loader *loader = &ctx->impl->loader;
  grn_obj *v = (grn_obj *)(GRN_BULK_HEAD(&loader->values));
  grn_obj *ve = (grn_obj *)(GRN_BULK_CURR(&loader->values));
  grn_obj **p = (grn_obj **)GRN_BULK_HEAD(&loader->columns);
  uint32_t i = GRN_BULK_VSIZE(&loader->columns) / sizeof(grn_obj *);
  if (ctx->impl->db) { while (i--) { grn_obj_unlink(ctx, *p++); } }
  if (loader->ifexists) { grn_obj_unlink(ctx, loader->ifexists); }
  if (loader->each) { grn_obj_unlink(ctx, loader->each); }
  while (v < ve) { GRN_OBJ_FIN(ctx, v++); }
  GRN_OBJ_FIN(ctx, &loader->values);
  GRN_OBJ_FIN(ctx, &loader->level);
  GRN_OBJ_FIN(ctx, &loader->columns);
  grn_loader_init(loader);
}

#define IMPL_SIZE ((sizeof(struct _grn_ctx_impl) + (grn_pagesize - 1)) & ~(grn_pagesize - 1))

#ifdef GRN_WITH_MESSAGE_PACK
static int
grn_msgpack_buffer_write(void *data, const char *buf, unsigned int len)
{
  grn_ctx *ctx = (grn_ctx *)data;
  return grn_bulk_write(ctx, ctx->impl->outbuf, buf, len);
}
#endif

static void
grn_ctx_impl_init(grn_ctx *ctx)
{
  grn_io_mapinfo mi;
  if (!(ctx->impl = grn_io_anon_map(ctx, &mi, IMPL_SIZE))) {
    ctx->impl = NULL;
    return;
  }
#ifdef USE_DYNAMIC_MALLOC_CHANGE
  grn_ctx_impl_init_malloc(ctx);
#endif
#ifdef USE_MEMORY_DEBUG
  ctx->impl->alloc_info = NULL;
#endif
  ctx->impl->encoding = ctx->encoding;
  ctx->impl->lifoseg = -1;
  ctx->impl->currseg = -1;
  CRITICAL_SECTION_INIT(ctx->impl->lock);
  if (!(ctx->impl->values = grn_array_create(ctx, NULL, sizeof(grn_db_obj *),
                                             GRN_ARRAY_TINY))) {
    CRITICAL_SECTION_FIN(ctx->impl->lock);
    grn_io_anon_unmap(ctx, &mi, IMPL_SIZE);
    ctx->impl = NULL;
    return;
  }
  if (!(ctx->impl->ios = grn_hash_create(ctx, NULL, GRN_TABLE_MAX_KEY_SIZE,
                                         sizeof(grn_io *),
                                         GRN_OBJ_KEY_VAR_SIZE|GRN_HASH_TINY))) {
    grn_array_close(ctx, ctx->impl->values);
    CRITICAL_SECTION_FIN(ctx->impl->lock);
    grn_io_anon_unmap(ctx, &mi, IMPL_SIZE);
    ctx->impl = NULL;
    return;
  }
  ctx->impl->db = NULL;

  ctx->impl->expr_vars = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(grn_obj *), 0);
  ctx->impl->stack_curr = 0;
  ctx->impl->curr_expr = NULL;
  ctx->impl->qe_next = NULL;
  ctx->impl->parser = NULL;

  GRN_TEXT_INIT(&ctx->impl->names, GRN_OBJ_VECTOR);
  GRN_UINT32_INIT(&ctx->impl->levels, GRN_OBJ_VECTOR);

  if (ctx == &grn_gctx) {
    ctx->impl->command_version = GRN_COMMAND_VERSION_STABLE;
  } else {
    ctx->impl->command_version = grn_get_default_command_version();
  }

  if (ctx == &grn_gctx) {
    ctx->impl->match_escalation_threshold =
      GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD;
  } else {
    ctx->impl->match_escalation_threshold =
      grn_get_default_match_escalation_threshold();
  }

  ctx->impl->finalizer = NULL;

  ctx->impl->com = NULL;
  ctx->impl->outbuf = grn_obj_open(ctx, GRN_BULK, 0, 0);
  ctx->impl->output = NULL;
  ctx->impl->data.ptr = NULL;
  ctx->impl->tv.tv_sec = 0;
  ctx->impl->tv.tv_nsec = 0;
  ctx->impl->edge = NULL;
  grn_loader_init(&ctx->impl->loader);
  ctx->impl->plugin_path = NULL;

  GRN_TEXT_INIT(&ctx->impl->query_log_buf, 0);

  ctx->impl->previous_errbuf[0] = '\0';
  ctx->impl->n_same_error_messages = 0;

#ifdef GRN_WITH_MESSAGE_PACK
  msgpack_packer_init(&ctx->impl->msgpacker, ctx, grn_msgpack_buffer_write);
#endif

  grn_ctx_impl_mrb_init(ctx);
}

void
grn_ctx_set_next_expr(grn_ctx *ctx, grn_obj *expr)
{
  ctx->impl->qe_next = expr;
}

static void
grn_ctx_impl_clear_n_same_error_mssagges(grn_ctx *ctx)
{
  if (ctx->impl->n_same_error_messages == 0) {
    return;
  }

  GRN_LOG(ctx, GRN_LOG_NOTICE, "(%u same messages are truncated)",
          ctx->impl->n_same_error_messages);
  ctx->impl->n_same_error_messages = 0;
}

grn_bool
grn_ctx_impl_should_log(grn_ctx *ctx)
{
  if (!ctx->impl) {
    return GRN_TRUE;
  }

  if (strcmp(ctx->errbuf, ctx->impl->previous_errbuf) == 0) {
    ctx->impl->n_same_error_messages++;
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

void
grn_ctx_impl_set_current_error_message(grn_ctx *ctx)
{
  if (!ctx->impl) {
    return;
  }

  grn_ctx_impl_clear_n_same_error_mssagges(ctx);
  strcpy(ctx->impl->previous_errbuf, ctx->errbuf);
}

static grn_rc
grn_ctx_init_internal(grn_ctx *ctx, int flags)
{
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  // if (ctx->stat != GRN_CTX_FIN) { return GRN_INVALID_ARGUMENT; }
  ERRCLR(ctx);
  ctx->flags = flags;
  if (getenv("GRN_CTX_PER_DB") && strcmp(getenv("GRN_CTX_PER_DB"), "yes") == 0) {
    ctx->flags |= GRN_CTX_PER_DB;
  }
  if (ERRP(ctx, GRN_ERROR)) { return ctx->rc; }
  ctx->stat = GRN_CTX_INITED;
  ctx->encoding = grn_gctx.encoding;
  ctx->seqno = 0;
  ctx->seqno2 = 0;
  ctx->subno = 0;
  ctx->impl = NULL;
  ctx->user_data.ptr = NULL;
  CRITICAL_SECTION_ENTER(grn_glock);
  ctx->next = grn_gctx.next;
  ctx->prev = &grn_gctx;
  grn_gctx.next->prev = ctx;
  grn_gctx.next = ctx;
  CRITICAL_SECTION_LEAVE(grn_glock);
  ctx->errline = 0;
  ctx->errfile = "";
  ctx->errfunc = "";
  ctx->trace[0] = NULL;
  ctx->errbuf[0] = '\0';
  return ctx->rc;
}

grn_rc
grn_ctx_init(grn_ctx *ctx, int flags)
{
  grn_rc rc;

  rc = grn_ctx_init_internal(ctx, flags);
  if (rc == GRN_SUCCESS) {
    grn_ctx_impl_init(ctx);
    rc = ctx->rc;
  }

  return rc;
}

grn_ctx *
grn_ctx_open(int flags)
{
  grn_ctx *ctx = GRN_GMALLOCN(grn_ctx, 1);
  if (ctx) {
    grn_ctx_init(ctx, flags|GRN_CTX_ALLOCATED);
    if (ERRP(ctx, GRN_ERROR)) {
      grn_ctx_fin(ctx);
      GRN_GFREE(ctx);
      ctx = NULL;
    }
  }
  return ctx;
}

grn_rc
grn_ctx_fin(grn_ctx *ctx)
{
  grn_rc rc = GRN_SUCCESS;
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  if (ctx->stat == GRN_CTX_FIN) { return GRN_INVALID_ARGUMENT; }
  if (!(ctx->flags & GRN_CTX_ALLOCATED)) {
    CRITICAL_SECTION_ENTER(grn_glock);
    ctx->next->prev = ctx->prev;
    ctx->prev->next = ctx->next;
    CRITICAL_SECTION_LEAVE(grn_glock);
  }
  if (ctx->impl) {
    grn_ctx_impl_clear_n_same_error_mssagges(ctx);
    if (ctx->impl->finalizer) {
      ctx->impl->finalizer(ctx, 0, NULL, &(ctx->user_data));
    }
    grn_ctx_impl_mrb_fin(ctx);
    grn_ctx_loader_clear(ctx);
    if (ctx->impl->parser) {
      grn_expr_parser_close(ctx);
    }
    if (ctx->impl->values) {
#ifndef USE_MEMORY_DEBUG
      grn_db_obj *o;
      GRN_ARRAY_EACH(ctx, ctx->impl->values, 0, 0, id, &o, {
        grn_obj_close(ctx, *((grn_obj **)o));
      });
#endif
      grn_array_close(ctx, ctx->impl->values);
    }
    if (ctx->impl->ios) {
      grn_hash_close(ctx, ctx->impl->ios);
    }
    if (ctx->impl->com) {
      if (ctx->stat != GRN_CTX_QUIT) {
        int flags;
        char *str;
        unsigned int str_len;
        grn_ctx_send(ctx, "quit", 4, GRN_CTX_HEAD);
        grn_ctx_recv(ctx, &str, &str_len, &flags);
      }
      grn_ctx_send(ctx, "ACK", 3, GRN_CTX_HEAD);
      rc = grn_com_close(ctx, ctx->impl->com);
    }
    GRN_OBJ_FIN(ctx, &ctx->impl->names);
    GRN_OBJ_FIN(ctx, &ctx->impl->levels);
    GRN_OBJ_FIN(ctx, &ctx->impl->query_log_buf);
    rc = grn_obj_close(ctx, ctx->impl->outbuf);
    {
      grn_hash **vp;
      grn_obj *value;
      GRN_HASH_EACH(ctx, ctx->impl->expr_vars, eid, NULL, NULL, &vp, {
        if (*vp) {
          GRN_HASH_EACH(ctx, *vp, id, NULL, NULL, &value, {
            GRN_OBJ_FIN(ctx, value);
          });
        }
        grn_hash_close(ctx, *vp);
      });
    }
    grn_hash_close(ctx, ctx->impl->expr_vars);
    if (ctx->impl->db && ctx->flags & GRN_CTX_PER_DB) {
      grn_obj *db = ctx->impl->db;
      ctx->impl->db = NULL;
      grn_obj_close(ctx, db);
    }
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
    grn_alloc_info_dump(ctx);
    grn_alloc_info_free(ctx);
    CRITICAL_SECTION_FIN(ctx->impl->lock);
    {
      grn_io_mapinfo mi;
      mi.map = (void *)ctx->impl;
      grn_io_anon_unmap(ctx, &mi, IMPL_SIZE);
    }
    ctx->impl = NULL;
  }
  ctx->stat = GRN_CTX_FIN;
  return rc;
}

grn_rc
grn_ctx_set_finalizer(grn_ctx *ctx, grn_proc_func *finalizer)
{
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  if (!ctx->impl) {
    if (ERRP(ctx, GRN_ERROR)) { return ctx->rc; }
  }
  ctx->impl->finalizer = finalizer;
  return GRN_SUCCESS;
}

grn_timeval grn_starttime;

static char *default_logger_path = NULL;
static FILE *default_logger_file = NULL;
static grn_critical_section default_logger_lock;

static void
default_logger_log(grn_ctx *ctx, grn_log_level level,
                   const char *timestamp, const char *title,
                   const char *message, const char *location, void *user_data)
{
  const char slev[] = " EACewnid-";
  if (default_logger_path) {
    CRITICAL_SECTION_ENTER(default_logger_lock);
    if (!default_logger_file) {
      default_logger_file = fopen(default_logger_path, "a");
    }
    if (default_logger_file) {
      if (location && *location) {
        fprintf(default_logger_file, "%s|%c|%s %s %s\n",
                timestamp, *(slev + level), title, message, location);
      } else {
        fprintf(default_logger_file, "%s|%c|%s %s\n", timestamp,
                *(slev + level), title, message);
      }
      fflush(default_logger_file);
    }
    CRITICAL_SECTION_LEAVE(default_logger_lock);
  }
}

static void
default_logger_reopen(grn_ctx *ctx, void *user_data)
{
  GRN_LOG(ctx, GRN_LOG_NOTICE, "log will be closed.");
  CRITICAL_SECTION_ENTER(default_logger_lock);
  if (default_logger_file) {
    fclose(default_logger_file);
    default_logger_file = NULL;
  }
  CRITICAL_SECTION_LEAVE(default_logger_lock);
  GRN_LOG(ctx, GRN_LOG_NOTICE, "log opened.");
}

static void
default_logger_fin(grn_ctx *ctx, void *user_data)
{
  CRITICAL_SECTION_ENTER(default_logger_lock);
  if (default_logger_file) {
    fclose(default_logger_file);
    default_logger_file = NULL;
  }
  CRITICAL_SECTION_LEAVE(default_logger_lock);
}

static grn_logger default_logger = {
  GRN_LOG_DEFAULT_LEVEL,
  GRN_LOG_TIME|GRN_LOG_MESSAGE,
  NULL,
  default_logger_log,
  default_logger_reopen,
  default_logger_fin
};

static grn_logger current_logger = {
  GRN_LOG_DEFAULT_LEVEL,
  GRN_LOG_TIME|GRN_LOG_MESSAGE,
  NULL,
  NULL,
  NULL,
  NULL
};

void
grn_default_logger_set_max_level(grn_log_level max_level)
{
  default_logger.max_level = max_level;
  if (current_logger.log == default_logger_log) {
    current_logger.max_level = max_level;
  }
}

grn_log_level
grn_default_logger_get_max_level(void)
{
  return default_logger.max_level;
}

void
grn_default_logger_set_path(const char *path)
{
  if (default_logger_path) {
    free(default_logger_path);
  }

  if (path) {
    default_logger_path = strdup(path);
  } else {
    default_logger_path = NULL;
  }
}

const char *
grn_default_logger_get_path(void)
{
  return default_logger_path;
}

void
grn_logger_reopen(grn_ctx *ctx)
{
  if (current_logger.reopen) {
    current_logger.reopen(ctx, current_logger.user_data);
  }
}

static void
grn_logger_fin(grn_ctx *ctx)
{
  if (current_logger.fin) {
    current_logger.fin(ctx, current_logger.user_data);
  }
}

static void
logger_info_func_wrapper(grn_ctx *ctx, grn_log_level level,
                         const char *timestamp, const char *title,
                         const char *message, const char *location,
                         void *user_data)
{
  grn_logger_info *info = user_data;
  info->func(level, timestamp, title, message, location, info->func_arg);
}

/* Deprecated since 2.1.2. */
grn_rc
grn_logger_info_set(grn_ctx *ctx, const grn_logger_info *info)
{
  if (info) {
    grn_logger logger;

    memset(&logger, 0, sizeof(grn_logger));
    logger.max_level = info->max_level;
    logger.flags = info->flags;
    if (info->func) {
      logger.log       = logger_info_func_wrapper;
      logger.user_data = (grn_logger_info *)info;
    } else {
      logger.log    = default_logger_log;
      logger.reopen = default_logger_reopen;
      logger.fin    = default_logger_fin;
    }
    return grn_logger_set(ctx, &logger);
  } else {
    return grn_logger_set(ctx, NULL);
  }
}

grn_rc
grn_logger_set(grn_ctx *ctx, const grn_logger *logger)
{
  grn_logger_fin(ctx);
  if (logger) {
    current_logger = *logger;
  } else {
    current_logger = default_logger;
  }
  return GRN_SUCCESS;
}

void
grn_logger_set_max_level(grn_ctx *ctx, grn_log_level max_level)
{
  current_logger.max_level = max_level;
}

grn_log_level
grn_logger_get_max_level(grn_ctx *ctx)
{
  return current_logger.max_level;
}

grn_bool
grn_logger_pass(grn_ctx *ctx, grn_log_level level)
{
  return level <= current_logger.max_level;
}

#define TBUFSIZE GRN_TIMEVAL_STR_SIZE
#define MBUFSIZE 0x1000
#define LBUFSIZE 0x400

void
grn_logger_put(grn_ctx *ctx, grn_log_level level,
               const char *file, int line, const char *func, const char *fmt, ...)
{
  if (level <= current_logger.max_level && current_logger.log) {
    char tbuf[TBUFSIZE];
    char mbuf[MBUFSIZE];
    char lbuf[LBUFSIZE];
    tbuf[0] = '\0';
    if (current_logger.flags & GRN_LOG_TIME) {
      grn_timeval tv;
      grn_timeval_now(ctx, &tv);
      grn_timeval2str(ctx, &tv, tbuf);
    }
    if (current_logger.flags & GRN_LOG_MESSAGE) {
      va_list argp;
      va_start(argp, fmt);
      vsnprintf(mbuf, MBUFSIZE - 1, fmt, argp);
      va_end(argp);
      mbuf[MBUFSIZE - 1] = '\0';
    } else {
      mbuf[0] = '\0';
    }
    if (current_logger.flags & GRN_LOG_LOCATION) {
      snprintf(lbuf, LBUFSIZE - 1, "%d %s:%d %s()", getpid(), file, line, func);
      lbuf[LBUFSIZE - 1] = '\0';
    } else {
      lbuf[0] = '\0';
    }
    current_logger.log(ctx, level, tbuf, "", mbuf, lbuf,
                       current_logger.user_data);
  }
}

static void
logger_init(void)
{
  if (!default_logger_path) {
    default_logger_path = strdup(GRN_LOG_PATH);
  }
  memcpy(&current_logger, &default_logger, sizeof(grn_logger));
  CRITICAL_SECTION_INIT(default_logger_lock);
}

static void
logger_fin(grn_ctx *ctx)
{
  grn_logger_fin(ctx);
  if (default_logger_path) {
    free(default_logger_path);
    default_logger_path = NULL;
  }
  CRITICAL_SECTION_FIN(default_logger_lock);
}


static char *default_query_logger_path = NULL;
static FILE *default_query_logger_file = NULL;
static grn_critical_section default_query_logger_lock;

static void
default_query_logger_log(grn_ctx *ctx, unsigned int flag,
                         const char *timestamp, const char *info,
                         const char *message, void *user_data)
{
  if (default_query_logger_path) {
    CRITICAL_SECTION_ENTER(default_query_logger_lock);
    if (!default_query_logger_file) {
      default_query_logger_file = fopen(default_query_logger_path, "a");
    }
    if (default_query_logger_file) {
      fprintf(default_query_logger_file, "%s|%s%s\n", timestamp, info, message);
      fflush(default_query_logger_file);
    }
    CRITICAL_SECTION_LEAVE(default_query_logger_lock);
  }
}

static void
default_query_logger_close(grn_ctx *ctx, void *user_data)
{
  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_DESTINATION, " ",
                "query log will be closed: <%s>", default_query_logger_path);
  CRITICAL_SECTION_ENTER(default_query_logger_lock);
  if (default_query_logger_file) {
    fclose(default_query_logger_file);
    default_query_logger_file = NULL;
  }
  CRITICAL_SECTION_LEAVE(default_query_logger_lock);
}

static void
default_query_logger_reopen(grn_ctx *ctx, void *user_data)
{
  default_query_logger_close(ctx, user_data);
  if (default_query_logger_path) {
    GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_DESTINATION, " ",
                  "query log is opened: <%s>", default_query_logger_path);
  }
}

static void
default_query_logger_fin(grn_ctx *ctx, void *user_data)
{
  if (default_query_logger_file) {
    default_query_logger_close(ctx, user_data);
  }
}

static grn_query_logger default_query_logger = {
  GRN_QUERY_LOG_DEFAULT,
  NULL,
  default_query_logger_log,
  default_query_logger_reopen,
  default_query_logger_fin
};

static grn_query_logger current_query_logger = {
  GRN_QUERY_LOG_DEFAULT,
  NULL,
  NULL,
  NULL,
  NULL
};

void
grn_default_query_logger_set_flags(unsigned int flags)
{
  default_query_logger.flags = flags;
  if (current_query_logger.log == default_query_logger_log) {
    current_query_logger.flags = flags;
  }
}

unsigned int
grn_default_query_logger_get_flags(void)
{
  return default_query_logger.flags;
}

void
grn_default_query_logger_set_path(const char *path)
{
  if (default_query_logger_path) {
    free(default_query_logger_path);
  }

  if (path) {
    default_query_logger_path = strdup(path);
  } else {
    default_query_logger_path = NULL;
  }
}

const char *
grn_default_query_logger_get_path(void)
{
  return default_query_logger_path;
}

void
grn_query_logger_reopen(grn_ctx *ctx)
{
  if (current_query_logger.reopen) {
    current_query_logger.reopen(ctx, current_query_logger.user_data);
  }
}

static void
grn_query_logger_fin(grn_ctx *ctx)
{
  if (current_query_logger.fin) {
    current_query_logger.fin(ctx, current_query_logger.user_data);
  }
}

grn_rc
grn_query_logger_set(grn_ctx *ctx, const grn_query_logger *logger)
{
  grn_query_logger_fin(ctx);
  if (logger) {
    current_query_logger = *logger;
  } else {
    current_query_logger = default_query_logger;
  }
  return GRN_SUCCESS;
}

grn_bool
grn_query_logger_pass(grn_ctx *ctx, unsigned int flag)
{
  return current_query_logger.flags & flag;
}

#define TIMESTAMP_BUFFER_SIZE    TBUFSIZE
/* 8+a(%p) + 1(|) + 1(mark) + 15(elapsed time) = 25+a */
#define INFO_BUFFER_SIZE         40

void
grn_query_logger_put(grn_ctx *ctx, unsigned int flag, const char *mark,
                     const char *format, ...)
{
  char timestamp[TIMESTAMP_BUFFER_SIZE];
  char info[INFO_BUFFER_SIZE];
  grn_obj *message = &ctx->impl->query_log_buf;

  if (!current_query_logger.log) {
    return;
  }

  {
    grn_timeval tv;
    timestamp[0] = '\0';
    grn_timeval_now(ctx, &tv);
    grn_timeval2str(ctx, &tv, timestamp);
  }

  if (flag & (GRN_QUERY_LOG_COMMAND | GRN_QUERY_LOG_DESTINATION)) {
    snprintf(info, INFO_BUFFER_SIZE - 1, "%p|%s", ctx, mark);
    info[INFO_BUFFER_SIZE - 1] = '\0';
  } else {
    grn_timeval tv;
    uint64_t elapsed_time;
    grn_timeval_now(ctx, &tv);
    elapsed_time =
      (uint64_t)(tv.tv_sec - ctx->impl->tv.tv_sec) * GRN_TIME_NSEC_PER_SEC +
      (tv.tv_nsec - ctx->impl->tv.tv_nsec);

    snprintf(info, INFO_BUFFER_SIZE - 1,
             "%p|%s%015" GRN_FMT_INT64U " ", ctx, mark, elapsed_time);
    info[INFO_BUFFER_SIZE - 1] = '\0';
  }

  {
    va_list args;

    va_start(args, format);
    GRN_BULK_REWIND(message);
    grn_text_vprintf(ctx, message, format, args);
    va_end(args);
    GRN_TEXT_PUTC(ctx, message, '\0');
  }

  current_query_logger.log(ctx, flag, timestamp, info, GRN_TEXT_VALUE(message),
                           current_query_logger.user_data);
}

static void
query_logger_init(void)
{
  memcpy(&current_query_logger, &default_query_logger, sizeof(grn_query_logger));
  CRITICAL_SECTION_INIT(default_query_logger_lock);
}

static void
query_logger_fin(grn_ctx *ctx)
{
  grn_query_logger_fin(ctx);
  if (default_query_logger_path) {
    free(default_query_logger_path);
  }
  CRITICAL_SECTION_FIN(default_query_logger_lock);
}

void
grn_log_reopen(grn_ctx *ctx)
{
  grn_logger_reopen(ctx);
  grn_query_logger_reopen(ctx);
}


static void
check_overcommit_memory(grn_ctx *ctx)
{
  FILE *file;
  int value;
  file = fopen("/proc/sys/vm/overcommit_memory", "r");
  if (!file) { return; }
  value = fgetc(file);
  if (value != '1') {
    GRN_LOG(ctx, GRN_LOG_NOTICE,
            "vm.overcommit_memory kernel parameter should be 1: <%c>: "
            "See INFO level log to resolve this",
            value);
    GRN_LOG(ctx, GRN_LOG_INFO,
            "Some processings with vm.overcommit_memory != 1 "
            "may break DB under low memory condition.");
    GRN_LOG(ctx, GRN_LOG_INFO,
            "To set vm.overcommit_memory to 1");
    GRN_LOG(ctx, GRN_LOG_INFO,
            "add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and "
            "restart your system or");
    GRN_LOG(ctx, GRN_LOG_INFO,
            "run 'sudo /sbin/sysctl vm.overcommit_memory=1' command.");
  }
  fclose(file);
}

static void
check_grn_ja_skip_same_value_put(grn_ctx *ctx)
{
  const char *grn_ja_skip_same_value_put_env;

  grn_ja_skip_same_value_put_env = getenv("GRN_JA_SKIP_SAME_VALUE_PUT");
  if (grn_ja_skip_same_value_put_env &&
      strcmp(grn_ja_skip_same_value_put_env, "no") == 0) {
    grn_ja_skip_same_value_put = GRN_FALSE;
  }
}

grn_rc
grn_init(void)
{
  grn_rc rc;
  grn_ctx *ctx = &grn_gctx;
  logger_init();
  query_logger_init();
  CRITICAL_SECTION_INIT(grn_glock);
  grn_gtick = 0;
  ctx->next = ctx;
  ctx->prev = ctx;
  grn_ctx_init_internal(ctx, 0);
  ctx->encoding = grn_encoding_parse(GRN_DEFAULT_ENCODING);
  grn_timeval_now(ctx, &grn_starttime);
#ifdef WIN32
  {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    grn_pagesize = si.dwAllocationGranularity;
  }
#else /* WIN32 */
  if ((grn_pagesize = sysconf(_SC_PAGESIZE)) == -1) {
    SERR("_SC_PAGESIZE");
    return ctx->rc;
  }
#endif /* WIN32 */
  if (grn_pagesize & (grn_pagesize - 1)) {
    GRN_LOG(ctx, GRN_LOG_CRIT, "pagesize=%x", grn_pagesize);
  }
  // expand_stack();
#ifdef USE_FAIL_MALLOC
  if (getenv("GRN_FMALLOC_PROB")) {
    grn_fmalloc_prob = strtod(getenv("GRN_FMALLOC_PROB"), 0) * RAND_MAX;
    if (getenv("GRN_FMALLOC_SEED")) {
      srand((unsigned int)atoi(getenv("GRN_FMALLOC_SEED")));
    } else {
      srand((unsigned int)time(NULL));
    }
  }
  if (getenv("GRN_FMALLOC_FUNC")) {
    grn_fmalloc_func = getenv("GRN_FMALLOC_FUNC");
  }
  if (getenv("GRN_FMALLOC_FILE")) {
    grn_fmalloc_file = getenv("GRN_FMALLOC_FILE");
  }
  if (getenv("GRN_FMALLOC_LINE")) {
    grn_fmalloc_line = atoi(getenv("GRN_FMALLOC_LINE"));
  }
#endif /* USE_FAIL_MALLOC */
  if ((rc = grn_com_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_com_init failed (%d)", rc);
    return rc;
  }
  grn_ctx_impl_init(ctx);
  if ((rc = grn_io_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "io initialize failed (%d)", rc);
    return rc;
  }
  if ((rc = grn_plugins_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "plugins initialize failed (%d)", rc);
    return rc;
  }
  if ((rc = grn_normalizer_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_normalizer_init failed (%d)", rc);
    return rc;
  }
  if ((rc = grn_tokenizers_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_tokenizers_init failed (%d)", rc);
    return rc;
  }
  /*
  if ((rc = grn_index_init())) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "index initialize failed (%d)", rc);
    return rc;
  }
  */
  grn_cache_init();
  if (!grn_request_canceler_init()) {
    rc = ctx->rc;
    grn_cache_fin();
    GRN_LOG(ctx, GRN_LOG_ALERT,
            "failed to initialize request canceler (%d)", rc);
    return rc;
  }
  GRN_LOG(ctx, GRN_LOG_NOTICE, "grn_init");
  check_overcommit_memory(ctx);
  check_grn_ja_skip_same_value_put(ctx);
  return rc;
}

grn_encoding
grn_get_default_encoding(void)
{
  return grn_gctx.encoding;
}

grn_rc
grn_set_default_encoding(grn_encoding encoding)
{
  switch (encoding) {
  case GRN_ENC_DEFAULT :
    grn_gctx.encoding = grn_encoding_parse(GRN_DEFAULT_ENCODING);
    return GRN_SUCCESS;
  case GRN_ENC_NONE :
  case GRN_ENC_EUC_JP :
  case GRN_ENC_UTF8 :
  case GRN_ENC_SJIS :
  case GRN_ENC_LATIN1 :
  case GRN_ENC_KOI8R :
    grn_gctx.encoding = encoding;
    return GRN_SUCCESS;
  default :
    return GRN_INVALID_ARGUMENT;
  }
}

grn_command_version
grn_get_default_command_version(void)
{
  return grn_ctx_get_command_version(&grn_gctx);
}

grn_rc
grn_set_default_command_version(grn_command_version version)
{
  return grn_ctx_set_command_version(&grn_gctx, version);
}

long long int
grn_get_default_match_escalation_threshold(void)
{
  return grn_ctx_get_match_escalation_threshold(&grn_gctx);
}

grn_rc
grn_set_default_match_escalation_threshold(long long int threshold)
{
  return grn_ctx_set_match_escalation_threshold(&grn_gctx, threshold);
}

int
grn_get_lock_timeout(void)
{
  return grn_lock_timeout;
}

grn_rc
grn_set_lock_timeout(int timeout)
{
  grn_lock_timeout = timeout;
  return GRN_SUCCESS;
}

static int alloc_count = 0;

grn_rc
grn_fin(void)
{
  grn_ctx *ctx, *ctx_;
  if (grn_gctx.stat == GRN_CTX_FIN) { return GRN_INVALID_ARGUMENT; }
  for (ctx = grn_gctx.next; ctx != &grn_gctx; ctx = ctx_) {
    ctx_ = ctx->next;
    if (ctx->stat != GRN_CTX_FIN) { grn_ctx_fin(ctx); }
    if (ctx->flags & GRN_CTX_ALLOCATED) {
      ctx->next->prev = ctx->prev;
      ctx->prev->next = ctx->next;
      GRN_GFREE(ctx);
    }
  }
  query_logger_fin(ctx);
  grn_request_canceler_fin();
  grn_cache_fin();
  grn_tokenizers_fin();
  grn_normalizer_fin();
  grn_plugins_fin();
  grn_io_fin();
  grn_ctx_fin(ctx);
  grn_com_fin();
  GRN_LOG(ctx, GRN_LOG_NOTICE, "grn_fin (%d)", alloc_count);
  logger_fin(ctx);
  CRITICAL_SECTION_FIN(grn_glock);
  return GRN_SUCCESS;
}

grn_rc
grn_ctx_connect(grn_ctx *ctx, const char *host, int port, int flags)
{
  GRN_API_ENTER;
  if (!ctx->impl) { goto exit; }
  {
    grn_com *com = grn_com_copen(ctx, NULL, host, port);
    if (com) {
      ctx->impl->com = com;
    }
  }
exit :
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_ctx_close(grn_ctx *ctx)
{
  grn_rc rc = grn_ctx_fin(ctx);
  ctx->next->prev = ctx->prev;
  ctx->prev->next = ctx->next;
  GRN_GFREE(ctx);
  return rc;
}

grn_command_version
grn_ctx_get_command_version(grn_ctx *ctx)
{
  if (ctx->impl) {
    return ctx->impl->command_version;
  } else {
    return GRN_COMMAND_VERSION_STABLE;
  }
}

grn_rc
grn_ctx_set_command_version(grn_ctx *ctx, grn_command_version version)
{
  switch (version) {
  case GRN_COMMAND_VERSION_DEFAULT :
    ctx->impl->command_version = GRN_COMMAND_VERSION_STABLE;
    return GRN_SUCCESS;
  default :
    if (GRN_COMMAND_VERSION_MIN <= version &&
        version <= GRN_COMMAND_VERSION_MAX) {
      ctx->impl->command_version = version;
      return GRN_SUCCESS;
    } else {
      return GRN_UNSUPPORTED_COMMAND_VERSION;
    }
  }
}

grn_content_type
grn_ctx_get_output_type(grn_ctx *ctx)
{
  if (ctx->impl) {
    return ctx->impl->output_type;
  } else {
    return GRN_CONTENT_NONE;
  }
}

grn_rc
grn_ctx_set_output_type(grn_ctx *ctx, grn_content_type type)
{
  grn_rc rc = GRN_SUCCESS;

  if (ctx->impl) {
    ctx->impl->output_type = type;
  } else {
    rc = GRN_INVALID_ARGUMENT;
  }

  return rc;
}

const char *
grn_ctx_get_mime_type(grn_ctx *ctx)
{
  if (ctx->impl) {
    return ctx->impl->mime_type;
  } else {
    return NULL;
  }
}

long long int
grn_ctx_get_match_escalation_threshold(grn_ctx *ctx)
{
  if (ctx->impl) {
    return ctx->impl->match_escalation_threshold;
  } else {
    return GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD;
  }
}

grn_rc
grn_ctx_set_match_escalation_threshold(grn_ctx *ctx, long long int threshold)
{
  ctx->impl->match_escalation_threshold = threshold;
  return GRN_SUCCESS;
}

grn_content_type
grn_get_ctype(grn_obj *var)
{
  grn_content_type ct = GRN_CONTENT_JSON;
  if (var->header.domain == GRN_DB_INT32) {
    ct = GRN_INT32_VALUE(var);
  } else if (GRN_TEXT_LEN(var)) {
    switch (*(GRN_TEXT_VALUE(var))) {
    case 't' :
    case 'T' :
      ct = GRN_CONTENT_TSV;
      break;
    case 'j' :
    case 'J' :
      ct = GRN_CONTENT_JSON;
      break;
    case 'x' :
    case 'X' :
      ct = GRN_CONTENT_XML;
      break;
    }
  }
  return ct;
}

static void
get_content_mime_type(grn_ctx *ctx, const char *p, const char *pe)
{
  ctx->impl->output_type = GRN_CONTENT_NONE;
  ctx->impl->mime_type = "application/octet-stream";

  if (p + 2 <= pe) {
    switch (*p) {
    case 'c' :
      if (p + 3 == pe && !memcmp(p, "css", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "text/css";
      }
      break;
    case 'g' :
      if (p + 3 == pe && !memcmp(p, "gif", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "image/gif";
      }
      break;
    case 'h' :
      if (p + 4 == pe && !memcmp(p, "html", 4)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "text/html";
      }
      break;
    case 'j' :
      if (!memcmp(p, "js", 2)) {
        if (p + 2 == pe) {
          ctx->impl->output_type = GRN_CONTENT_NONE;
          ctx->impl->mime_type = "text/javascript";
        } else if (p + 4 == pe && !memcmp(p + 2, "on", 2)) {
          ctx->impl->output_type = GRN_CONTENT_JSON;
          ctx->impl->mime_type = "application/json";
        }
      } else if (p + 3 == pe && !memcmp(p, "jpg", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "image/jpeg";
      }
      break;
#ifdef GRN_WITH_MESSAGE_PACK
    case 'm' :
      if (p + 7 == pe && !memcmp(p, "msgpack", 7)) {
        ctx->impl->output_type = GRN_CONTENT_MSGPACK;
        ctx->impl->mime_type = "application/x-msgpack";
      }
      break;
#endif
    case 'p' :
      if (p + 3 == pe && !memcmp(p, "png", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "image/png";
      }
      break;
    case 't' :
      if (p + 3 == pe && !memcmp(p, "txt", 3)) {
        ctx->impl->output_type = GRN_CONTENT_NONE;
        ctx->impl->mime_type = "text/plain";
      } else if (p + 3 == pe && !memcmp(p, "tsv", 3)) {
        ctx->impl->output_type = GRN_CONTENT_TSV;
        ctx->impl->mime_type = "text/plain";
      }
      break;
    case 'x':
      if (p + 3 == pe && !memcmp(p, "xml", 3)) {
        ctx->impl->output_type = GRN_CONTENT_XML;
        ctx->impl->mime_type = "text/xml";
      }
      break;
    }
  }
}

static void
grn_str_get_mime_type(grn_ctx *ctx, const char *p, const char *pe,
                     const char **key_end, const char **filename_end)
{
  const char *pd = NULL;
  for (; p < pe && *p != '?' && *p != '#'; p++) {
    if (*p == '.') { pd = p; }
  }
  *filename_end = p;
  if (pd && pd < p) {
    get_content_mime_type(ctx, pd + 1, p);
    *key_end = pd;
  } else {
    *key_end = pe;
  }
}

static void
get_command_version(grn_ctx *ctx, const char *p, const char *pe)
{
  grn_command_version version;
  const char *rest;

  version = grn_atoui(p, pe, &rest);
  if (pe == rest) {
    grn_rc rc;
    rc = grn_ctx_set_command_version(ctx, version);
    if (rc == GRN_UNSUPPORTED_COMMAND_VERSION) {
      ERR(rc,
          "unsupported command version is specified: %d: "
          "stable command version: %d: "
          "available command versions: %d-%d",
          version,
          GRN_COMMAND_VERSION_STABLE,
          GRN_COMMAND_VERSION_MIN, GRN_COMMAND_VERSION_MAX);
    }
  }
}

#define INDEX_HTML          "index.html"
#define OUTPUT_TYPE         "output_type"
#define COMMAND_VERSION     "command_version"
#define REQUEST_ID          "request_id"
#define EXPR_MISSING        "expr_missing"
#define OUTPUT_TYPE_LEN     (sizeof(OUTPUT_TYPE) - 1)
#define COMMAND_VERSION_LEN (sizeof(COMMAND_VERSION) - 1)
#define REQUEST_ID_LEN      (sizeof(REQUEST_ID) - 1)

#define HTTP_QUERY_PAIR_DELIMITER   "="
#define HTTP_QUERY_PAIRS_DELIMITERS "&;"

static inline int
command_proc_p(grn_obj *expr)
{
  return (expr->header.type == GRN_PROC &&
          ((grn_proc *)expr)->type == GRN_PROC_COMMAND);
}

grn_obj *
grn_ctx_qe_exec_uri(grn_ctx *ctx, const char *path, uint32_t path_len)
{
  grn_obj buf, *expr, *val;
  grn_obj request_id;
  const char *p = path, *e = path + path_len, *v, *key_end, *filename_end;
  GRN_TEXT_INIT(&buf, 0);
  GRN_TEXT_INIT(&request_id, 0);
  p = grn_text_urldec(ctx, &buf, p, e, '?');
  if (!GRN_TEXT_LEN(&buf)) { GRN_TEXT_SETS(ctx, &buf, INDEX_HTML); }
  v = GRN_TEXT_VALUE(&buf);
  grn_str_get_mime_type(ctx, v, GRN_BULK_CURR(&buf), &key_end, &filename_end);
  if ((GRN_TEXT_LEN(&buf) >= 2 && v[0] == 'd' && v[1] == '/')) {
    const char *command_name = v + 2;
    int command_name_size = key_end - command_name;
    expr = grn_ctx_get(ctx, command_name, command_name_size);
    if (expr && command_proc_p(expr)) {
      while (p < e) {
        int l;
        GRN_BULK_REWIND(&buf);
        p = grn_text_cgidec(ctx, &buf, p, e, HTTP_QUERY_PAIR_DELIMITER);
        v = GRN_TEXT_VALUE(&buf);
        l = GRN_TEXT_LEN(&buf);
        if (l == OUTPUT_TYPE_LEN && !memcmp(v, OUTPUT_TYPE, OUTPUT_TYPE_LEN)) {
          GRN_BULK_REWIND(&buf);
          p = grn_text_cgidec(ctx, &buf, p, e, HTTP_QUERY_PAIRS_DELIMITERS);
          v = GRN_TEXT_VALUE(&buf);
          get_content_mime_type(ctx, v, GRN_BULK_CURR(&buf));
        } else if (l == COMMAND_VERSION_LEN &&
                   !memcmp(v, COMMAND_VERSION, COMMAND_VERSION_LEN)) {
          GRN_BULK_REWIND(&buf);
          p = grn_text_cgidec(ctx, &buf, p, e, HTTP_QUERY_PAIRS_DELIMITERS);
          get_command_version(ctx, GRN_TEXT_VALUE(&buf), GRN_BULK_CURR(&buf));
          if (ctx->rc) { goto exit; }
        } else if (l == REQUEST_ID_LEN &&
                   !memcmp(v, REQUEST_ID, REQUEST_ID_LEN)) {
          GRN_BULK_REWIND(&request_id);
          p = grn_text_cgidec(ctx, &request_id, p, e,
                              HTTP_QUERY_PAIRS_DELIMITERS);
          if (ctx->rc) { goto exit; }
        } else {
          if (!(val = grn_expr_get_or_add_var(ctx, expr, v, l))) {
            val = &buf;
          }
          grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
          p = grn_text_cgidec(ctx, val, p, e, HTTP_QUERY_PAIRS_DELIMITERS);
        }
      }
      if (GRN_TEXT_LEN(&request_id) > 0) {
        grn_request_canceler_register(ctx,
                                      GRN_TEXT_VALUE(&request_id),
                                      GRN_TEXT_LEN(&request_id));
      }
      ctx->impl->curr_expr = expr;
      grn_expr_exec(ctx, expr, 0);
      if (GRN_TEXT_LEN(&request_id) > 0) {
        grn_request_canceler_unregister(ctx,
                                        GRN_TEXT_VALUE(&request_id),
                                        GRN_TEXT_LEN(&request_id));
      }
    } else {
      ERR(GRN_INVALID_ARGUMENT, "invalid command name: %.*s",
          command_name_size, command_name);
    }
  } else if ((expr = grn_ctx_get(ctx, GRN_EXPR_MISSING_NAME,
                                 strlen(GRN_EXPR_MISSING_NAME)))) {
    if ((val = grn_expr_get_var_by_offset(ctx, expr, 0))) {
      grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
      GRN_TEXT_SET(ctx, val, v, filename_end - v);
    }
    ctx->impl->curr_expr = expr;
    grn_expr_exec(ctx, expr, 0);
  }
exit :
  GRN_OBJ_FIN(ctx, &buf);
  return expr;
}

grn_obj *
grn_ctx_qe_exec(grn_ctx *ctx, const char *str, uint32_t str_len)
{
  char tok_type;
  int offset = 0;
  grn_obj buf, *expr = NULL, *val = NULL;
  grn_obj request_id;
  const char *p = str, *e = str + str_len, *v;
  GRN_TEXT_INIT(&buf, 0);
  GRN_TEXT_INIT(&request_id, 0);
  p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
  expr = grn_ctx_get(ctx, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
  while (p < e) {
    GRN_BULK_REWIND(&buf);
    p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
    v = GRN_TEXT_VALUE(&buf);
    switch (tok_type) {
    case GRN_TOK_VOID :
      p = e;
      break;
    case GRN_TOK_SYMBOL :
      if (GRN_TEXT_LEN(&buf) > 2 && v[0] == '-' && v[1] == '-') {
        int l = GRN_TEXT_LEN(&buf) - 2;
        v += 2;
        if (l == OUTPUT_TYPE_LEN && !memcmp(v, OUTPUT_TYPE, OUTPUT_TYPE_LEN)) {
          GRN_BULK_REWIND(&buf);
          p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
          v = GRN_TEXT_VALUE(&buf);
          get_content_mime_type(ctx, v, GRN_BULK_CURR(&buf));
        } else if (l == COMMAND_VERSION_LEN &&
                   !memcmp(v, COMMAND_VERSION, COMMAND_VERSION_LEN)) {
          GRN_BULK_REWIND(&buf);
          p = grn_text_unesc_tok(ctx, &buf, p, e, &tok_type);
          get_command_version(ctx, GRN_TEXT_VALUE(&buf), GRN_BULK_CURR(&buf));
          if (ctx->rc) { goto exit; }
        } else if (l == REQUEST_ID_LEN &&
                   !memcmp(v, REQUEST_ID, REQUEST_ID_LEN)) {
          GRN_BULK_REWIND(&request_id);
          p = grn_text_unesc_tok(ctx, &request_id, p, e, &tok_type);
          if (ctx->rc) { goto exit; }
        } else if (expr && (val = grn_expr_get_or_add_var(ctx, expr, v, l))) {
          grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
          p = grn_text_unesc_tok(ctx, val, p, e, &tok_type);
        } else {
          p = e;
        }
        break;
      }
      // fallthru
    case GRN_TOK_STRING :
    case GRN_TOK_QUOTE :
      if (expr && (val = grn_expr_get_var_by_offset(ctx, expr, offset++))) {
        grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
        GRN_TEXT_PUT(ctx, val, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf));
      } else {
        p = e;
      }
      break;
    }
  }
  if (GRN_TEXT_LEN(&request_id) > 0) {
    grn_request_canceler_register(ctx,
                                  GRN_TEXT_VALUE(&request_id),
                                  GRN_TEXT_LEN(&request_id));
  }
  ctx->impl->curr_expr = expr;
  if (expr && command_proc_p(expr)) {
    grn_expr_exec(ctx, expr, 0);
  } else {
    GRN_BULK_REWIND(&buf);
    grn_text_unesc_tok(ctx, &buf, str, str + str_len, &tok_type);
    if (GRN_TEXT_LEN(&buf)) {
      ERR(GRN_INVALID_ARGUMENT, "invalid command name: %.*s",
          (int)GRN_TEXT_LEN(&buf), GRN_TEXT_VALUE(&buf));
    }
  }
  if (GRN_TEXT_LEN(&request_id) > 0) {
    grn_request_canceler_unregister(ctx,
                                    GRN_TEXT_VALUE(&request_id),
                                    GRN_TEXT_LEN(&request_id));
  }
exit :
  GRN_OBJ_FIN(ctx, &request_id);
  GRN_OBJ_FIN(ctx, &buf);
  return expr;
}

grn_rc
grn_ctx_sendv(grn_ctx *ctx, int argc, char **argv, int flags)
{
  grn_obj buf;
  GRN_TEXT_INIT(&buf, 0);
  while (argc--) {
    // todo : encode into json like syntax
    GRN_TEXT_PUTS(ctx, &buf, *argv);
    argv++;
    if (argc) { GRN_TEXT_PUTC(ctx, &buf, ' '); }
  }
  grn_ctx_send(ctx, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf), flags);
  GRN_OBJ_FIN(ctx, &buf);
  return ctx->rc;
}

static int
comment_command_p(const char *command, unsigned int length)
{
  const char *p, *e;

  e = command + length;
  for (p = command; p < e; p++) {
    switch (*p) {
    case '#' :
      return GRN_TRUE;
    case ' ' :
    case '\t' :
      break;
    default :
      return GRN_FALSE;
    }
  }
  return GRN_FALSE;
}

unsigned int
grn_ctx_send(grn_ctx *ctx, const char *str, unsigned int str_len, int flags)
{
  if (!ctx) { return 0; }
  GRN_API_ENTER;
  if (ctx->impl) {
    if (ctx->impl->com) {
      grn_rc rc;
      grn_com_header sheader;
      grn_timeval_now(ctx, &ctx->impl->tv);
      if ((flags & GRN_CTX_MORE)) { flags |= GRN_CTX_QUIET; }
      if (ctx->stat == GRN_CTX_QUIT) { flags |= GRN_CTX_QUIT; }
      sheader.proto = GRN_COM_PROTO_GQTP;
      sheader.qtype = 0;
      sheader.keylen = 0;
      sheader.level = 0;
      sheader.flags = flags;
      sheader.status = 0;
      sheader.opaque = 0;
      sheader.cas = 0;
      if ((rc = grn_com_send(ctx, ctx->impl->com, &sheader, (char *)str, str_len, 0))) {
        ERR(rc, "grn_com_send failed");
      }
      goto exit;
    } else {
      grn_obj *expr = NULL;
      if (comment_command_p(str, str_len)) { goto output; };
      if (ctx->impl->qe_next) {
        grn_obj *val;
        expr = ctx->impl->qe_next;
        ctx->impl->qe_next = NULL;
        if ((val = grn_expr_get_var_by_offset(ctx, expr, 0))) {
          grn_obj_reinit(ctx, val, GRN_DB_TEXT, 0);
          GRN_TEXT_PUT(ctx, val, str, str_len);
        }
        grn_expr_exec(ctx, expr, 0);
      } else {
        ctx->impl->mime_type = "application/json";
        ctx->impl->output_type = GRN_CONTENT_JSON;
        grn_timeval_now(ctx, &ctx->impl->tv);
        GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_COMMAND,
                      ">", "%.*s", str_len, str);
        if (str_len && *str == '/') {
          expr = grn_ctx_qe_exec_uri(ctx, str + 1, str_len - 1);
        } else {
          expr = grn_ctx_qe_exec(ctx, str, str_len);
        }
      }
      if (ctx->stat == GRN_CTX_QUITTING) { ctx->stat = GRN_CTX_QUIT; }
      if (ctx->impl->qe_next) {
        ERRCLR(ctx);
      } else {
        GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_RESULT_CODE,
                      "<", "rc=%d", ctx->rc);
      }
    output :
      if (!ERRP(ctx, GRN_CRIT)) {
        if (!(flags & GRN_CTX_QUIET) && ctx->impl->output) {
          ctx->impl->output(ctx, GRN_CTX_TAIL, ctx->impl->data.ptr);
        }
      }
      if (expr) { grn_expr_clear_vars(ctx, expr); }
      goto exit;
    }
  }
  ERR(GRN_INVALID_ARGUMENT, "invalid ctx assigned");
exit :
  GRN_API_RETURN(0);
}

unsigned int
grn_ctx_recv(grn_ctx *ctx, char **str, unsigned int *str_len, int *flags)
{
  if (!ctx) { return GRN_INVALID_ARGUMENT; }
  if (ctx->stat == GRN_CTX_QUIT) {
    *str = NULL;
    *str_len = 0;
    *flags = GRN_CTX_QUIT;
    return 0;
  }
  GRN_API_ENTER;
  if (ctx->impl) {
    if (ctx->impl->com) {
      grn_com_header header;
      if (grn_com_recv(ctx, ctx->impl->com, &header, ctx->impl->outbuf)) {
        *str = NULL;
        *str_len = 0;
        *flags = 0;
      } else {
        *str = GRN_BULK_HEAD(ctx->impl->outbuf);
        *str_len = GRN_BULK_VSIZE(ctx->impl->outbuf);
        if (header.flags & GRN_CTX_QUIT) {
          ctx->stat = GRN_CTX_QUIT;
          *flags = GRN_CTX_QUIT;
        } else {
          *flags = (header.flags & GRN_CTX_TAIL) ? 0 : GRN_CTX_MORE;
        }
        ctx->impl->output_type = header.qtype;
        ctx->rc = (int16_t)ntohs(header.status);
        ctx->errbuf[0] = '\0';
        ctx->errline = 0;
        ctx->errfile = NULL;
        ctx->errfunc = NULL;
      }
      goto exit;
    } else {
      grn_obj *buf = ctx->impl->outbuf;
      unsigned int head = 0, tail = GRN_BULK_VSIZE(buf);
      *str = GRN_BULK_HEAD(buf) + head;
      *str_len = tail - head;
      GRN_BULK_REWIND(ctx->impl->outbuf);
      goto exit;
    }
  }
  ERR(GRN_INVALID_ARGUMENT, "invalid ctx assigned");
exit :
  GRN_API_RETURN(0);
}

void
grn_ctx_stream_out_func(grn_ctx *ctx, int flags, void *stream)
{
  if (ctx && ctx->impl) {
    grn_obj *buf = ctx->impl->outbuf;
    uint32_t size = GRN_BULK_VSIZE(buf);
    if (size) {
      if (fwrite(GRN_BULK_HEAD(buf), 1, size, (FILE *)stream)) {
        fputc('\n', (FILE *)stream);
        fflush((FILE *)stream);
      }
      GRN_BULK_REWIND(buf);
    }
  }
}

void
grn_ctx_recv_handler_set(grn_ctx *ctx, void (*func)(grn_ctx *, int, void *), void *func_arg)
{
  if (ctx && ctx->impl) {
    ctx->impl->output = func;
    ctx->impl->data.ptr = func_arg;
  }
}

grn_rc
grn_ctx_info_get(grn_ctx *ctx, grn_ctx_info *info)
{
  if (!ctx || !ctx->impl) { return GRN_INVALID_ARGUMENT; }
  if (ctx->impl->com) {
    info->fd = ctx->impl->com->fd;
    info->com_status = ctx->impl->com_status;
    info->outbuf = ctx->impl->outbuf;
    info->stat = ctx->stat;
  } else {
    info->fd = -1;
    info->com_status = 0;
    info->outbuf = ctx->impl->outbuf;
    info->stat = ctx->stat;
  }
  return GRN_SUCCESS;
}


typedef struct _grn_cache_entry grn_cache_entry;

struct _grn_cache {
  grn_cache_entry *next;
  grn_cache_entry *prev;
  grn_hash *hash;
  grn_mutex mutex;
  uint32_t max_nentries;
  uint32_t nfetches;
  uint32_t nhits;
};

struct _grn_cache_entry {
  grn_cache_entry *next;
  grn_cache_entry *prev;
  grn_obj *value;
  grn_timeval tv;
  grn_id id;
  uint32_t nref;
};

static grn_cache *grn_cache_current = NULL;
static grn_cache *grn_cache_default = NULL;

grn_cache *
grn_cache_open(grn_ctx *ctx)
{
  grn_cache *cache = NULL;

  GRN_API_ENTER;
  cache = GRN_MALLOC(sizeof(grn_cache));
  if (!cache) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "[cache] failed to allocate grn_cache");
    goto exit;
  }

  cache->next = (grn_cache_entry *)cache;
  cache->prev = (grn_cache_entry *)cache;
  cache->hash = grn_hash_create(&grn_gctx, NULL, GRN_TABLE_MAX_KEY_SIZE,
                                sizeof(grn_cache_entry), GRN_OBJ_KEY_VAR_SIZE);
  MUTEX_INIT(cache->mutex);
  cache->max_nentries = GRN_CACHE_DEFAULT_MAX_N_ENTRIES;
  cache->nfetches = 0;
  cache->nhits = 0;

exit :
  GRN_API_RETURN(cache);
}

grn_rc
grn_cache_close(grn_ctx *ctx, grn_cache *cache)
{
  grn_ctx *ctx_original = ctx;
  grn_cache_entry *vp;

  GRN_API_ENTER;

  ctx = &grn_gctx;
  GRN_HASH_EACH(ctx, cache->hash, id, NULL, NULL, &vp, {
    grn_obj_close(ctx, vp->value);
  });
  grn_hash_close(ctx, cache->hash);
  MUTEX_FIN(cache->mutex);
  ctx = ctx_original;
  GRN_FREE(cache);

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_cache_current_set(grn_ctx *ctx, grn_cache *cache)
{
  grn_cache_current = cache;
  return GRN_SUCCESS;
}

grn_cache *
grn_cache_current_get(grn_ctx *ctx)
{
  return grn_cache_current;
}

void
grn_cache_init(void)
{
  grn_cache_default = grn_cache_open(&grn_gctx);
  grn_cache_current_set(&grn_gctx, grn_cache_default);
}

grn_rc
grn_cache_set_max_n_entries(grn_ctx *ctx, grn_cache *cache, unsigned int n)
{
  if (!cache) {
    return GRN_INVALID_ARGUMENT;
  }
  cache->max_nentries = n;
  return GRN_SUCCESS;
}

uint32_t
grn_cache_get_max_n_entries(grn_ctx *ctx, grn_cache *cache)
{
  if (!cache) {
    return 0;
  }
  return cache->max_nentries;
}

void
grn_cache_get_statistics(grn_ctx *ctx, grn_cache *cache,
                         grn_cache_statistics *statistics)
{
  MUTEX_LOCK(cache->mutex);
  statistics->nentries = GRN_HASH_SIZE(cache->hash);
  statistics->max_nentries = cache->max_nentries;
  statistics->nfetches = cache->nfetches;
  statistics->nhits = cache->nhits;
  MUTEX_UNLOCK(cache->mutex);
}

static void
grn_cache_expire_entry(grn_cache *cache, grn_cache_entry *ce)
{
  if (!ce->nref) {
    ce->prev->next = ce->next;
    ce->next->prev = ce->prev;
    grn_obj_close(&grn_gctx, ce->value);
    grn_hash_delete_by_id(&grn_gctx, cache->hash, ce->id, NULL);
  }
}

grn_obj *
grn_cache_fetch(grn_ctx *ctx, grn_cache *cache,
                const char *str, uint32_t str_len)
{
  grn_cache_entry *ce;
  grn_obj *obj = NULL;
  if (!ctx->impl || !ctx->impl->db) { return obj; }
  MUTEX_LOCK(cache->mutex);
  cache->nfetches++;
  if (grn_hash_get(&grn_gctx, cache->hash, str, str_len, (void **)&ce)) {
    if (ce->tv.tv_sec <= grn_db_lastmod(ctx->impl->db)) {
      grn_cache_expire_entry(cache, ce);
      goto exit;
    }
    ce->nref++;
    obj = ce->value;
    ce->prev->next = ce->next;
    ce->next->prev = ce->prev;
    {
      grn_cache_entry *ce0 = (grn_cache_entry *)cache;
      ce->next = ce0->next;
      ce->prev = ce0;
      ce0->next->prev = ce;
      ce0->next = ce;
    }
    cache->nhits++;
  }
exit :
  MUTEX_UNLOCK(cache->mutex);
  return obj;
}

void
grn_cache_unref(grn_ctx *ctx, grn_cache *cache,
                const char *str, uint32_t str_len)
{
  grn_cache_entry *ce;
  ctx = &grn_gctx;
  MUTEX_LOCK(cache->mutex);
  if (grn_hash_get(ctx, cache->hash, str, str_len, (void **)&ce)) {
    if (ce->nref) { ce->nref--; }
  }
  MUTEX_UNLOCK(cache->mutex);
}

void
grn_cache_update(grn_ctx *ctx, grn_cache *cache,
                 const char *str, uint32_t str_len, grn_obj *value)
{
  grn_id id;
  int added = 0;
  grn_cache_entry *ce;
  grn_rc rc = GRN_SUCCESS;
  grn_obj *old = NULL, *obj;
  if (!ctx->impl || !cache->max_nentries) { return; }
  if (!(obj = grn_obj_open(&grn_gctx, GRN_BULK, 0, GRN_DB_TEXT))) { return; }
  GRN_TEXT_PUT(&grn_gctx, obj, GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));
  MUTEX_LOCK(cache->mutex);
  if ((id = grn_hash_add(&grn_gctx, cache->hash, str, str_len, (void **)&ce, &added))) {
    if (!added) {
      if (ce->nref) {
        rc = GRN_RESOURCE_BUSY;
        goto exit;
      }
      old = ce->value;
      ce->prev->next = ce->next;
      ce->next->prev = ce->prev;
    }
    ce->id = id;
    ce->value = obj;
    ce->tv = ctx->impl->tv;
    ce->nref = 0;
    {
      grn_cache_entry *ce0 = (grn_cache_entry *)cache;
      ce->next = ce0->next;
      ce->prev = ce0;
      ce0->next->prev = ce;
      ce0->next = ce;
    }
    if (GRN_HASH_SIZE(cache->hash) > cache->max_nentries) {
      grn_cache_expire_entry(cache, cache->prev);
    }
  } else {
    rc = GRN_NO_MEMORY_AVAILABLE;
  }
exit :
  MUTEX_UNLOCK(cache->mutex);
  if (rc) { grn_obj_close(&grn_gctx, obj); }
  if (old) { grn_obj_close(&grn_gctx, old); }
}

void
grn_cache_expire(grn_cache *cache, int32_t size)
{
  grn_cache_entry *ce0 = (grn_cache_entry *)cache;
  MUTEX_LOCK(cache->mutex);
  while (ce0 != ce0->prev && size--) {
    grn_cache_expire_entry(cache, ce0->prev);
  }
  MUTEX_UNLOCK(cache->mutex);
}

void
grn_cache_fin(void)
{
  grn_cache_current_set(&grn_gctx, NULL);
  grn_cache_close(&grn_gctx, grn_cache_default);
}

/**** memory allocation ****/

#define ALIGN_SIZE (1<<3)
#define ALIGN_MASK (ALIGN_SIZE-1)
#define GRN_CTX_ALLOC_CLEAR 1

void *
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
      if (!grn_io_anon_map(ctx, mi, npages * grn_pagesize)) { goto exit; }
      //GRN_LOG(ctx, GRN_LOG_NOTICE, "map i=%d (%d)", i, npages * grn_pagesize);
      mi->nref = (uint32_t) npages;
      mi->count = GRN_CTX_SEGMENT_VLEN;
      ctx->impl->currseg = -1;
      header = mi->map;
      header[0] = i;
      header[1] = (int32_t) size;
    } else {
      i = ctx->impl->currseg;
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
        //GRN_LOG(ctx, GRN_LOG_NOTICE, "map i=%d", i);
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
      memcpy(res, ptr, size_ > size ? size : size_);
      grn_ctx_free(ctx, ptr, file, line, func);
    }
  } else {
    grn_ctx_free(ctx, ptr, file, line, func);
  }
  return res;
}

char *
grn_ctx_strdup(grn_ctx *ctx, const char *s, const char* file, int line, const char *func)
{
  void *res = NULL;
  if (s) {
    size_t size = strlen(s) + 1;
    if ((res = grn_ctx_alloc(ctx, size, 0, file, line, func))) {
      memcpy(res, s, size);
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

#define DB_P(s) ((s) && (s)->header.type == GRN_DB)

grn_rc
grn_ctx_use(grn_ctx *ctx, grn_obj *db)
{
  GRN_API_ENTER;
  if (db && !DB_P(db)) {
    ctx->rc = GRN_INVALID_ARGUMENT;
  } else {
    if (!ctx->rc) {
      ctx->impl->db = db;
      if (db) {
        grn_obj buf;
        GRN_TEXT_INIT(&buf, 0);
        grn_obj_get_info(ctx, db, GRN_INFO_ENCODING, &buf);
        ctx->encoding = *(grn_encoding *)GRN_BULK_HEAD(&buf);
        grn_obj_close(ctx, &buf);
      }
    }
  }
  GRN_API_RETURN(ctx->rc);
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
      if (!grn_io_anon_map(ctx, mi, npages * grn_pagesize)) { return NULL; }
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

#if USE_DYNAMIC_MALLOC_CHANGE
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

void *
grn_malloc(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->malloc_func) {
    return ctx->impl->malloc_func(ctx, size, file, line, func);
  } else {
    return grn_malloc_default(ctx, size, file, line, func);
  }
}

void *
grn_calloc(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->calloc_func) {
    return ctx->impl->calloc_func(ctx, size, file, line, func);
  } else {
    return grn_calloc_default(ctx, size, file, line, func);
  }
}

void *
grn_realloc(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->realloc_func) {
    return ctx->impl->realloc_func(ctx, ptr, size, file, line, func);
  } else {
    return grn_realloc_default(ctx, ptr, size, file, line, func);
  }
}

char *
grn_strdup(grn_ctx *ctx, const char *string, const char* file, int line, const char *func)
{
  if (ctx && ctx->impl && ctx->impl->strdup_func) {
    return ctx->impl->strdup_func(ctx, string, file, line, func);
  } else {
    return grn_strdup_default(ctx, string, file, line, func);
  }
}
#endif

void *
grn_malloc_default(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    void *res = malloc(size);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, file, line, func);
    } else {
      if (!(res = malloc(size))) {
        MERR("malloc fail (%" GRN_FMT_SIZE ")=%p (%s:%d) <%d>",
             size, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
        grn_alloc_info_add(res, file, line, func);
      }
    }
    return res;
  }
}

void *
grn_calloc_default(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    void *res = calloc(size, 1);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, file, line, func);
    } else {
      if (!(res = calloc(size, 1))) {
        MERR("calloc fail (%" GRN_FMT_LLU ")=%p (%s:%d) <%" GRN_FMT_LLU ">",
             (unsigned long long int)size, res, file, line,
             (unsigned long long int)alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
        grn_alloc_info_add(res, file, line, func);
      }
    }
    return res;
  }
}

void
grn_free_default(grn_ctx *ctx, void *ptr, const char* file, int line, const char *func)
{
  if (!ctx) { return; }
  grn_alloc_info_check(ptr);
  {
    free(ptr);
    if (ptr) {
      GRN_ADD_ALLOC_COUNT(-1);
    } else {
      GRN_LOG(ctx, GRN_LOG_ALERT, "free fail (%p) (%s:%d) <%d>", ptr, file, line, alloc_count);
    }
  }
}

void *
grn_realloc_default(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func)
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
      grn_alloc_info_change(ptr, res);
    } else {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, file, line, func);
    }
  } else {
    if (!ptr) { return NULL; }
    grn_alloc_info_check(ptr);
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
grn_strdup_default(grn_ctx *ctx, const char *s, const char* file, int line, const char *func)
{
  if (!ctx) { return NULL; }
  {
    char *res = strdup(s);
    if (res) {
      GRN_ADD_ALLOC_COUNT(1);
      grn_alloc_info_add(res, file, line, func);
    } else {
      if (!(res = strdup(s))) {
        MERR("strdup(%p)=%p (%s:%d) <%d>", s, res, file, line, alloc_count);
      } else {
        GRN_ADD_ALLOC_COUNT(1);
        grn_alloc_info_add(res, file, line, func);
      }
    }
    return res;
  }
}

#ifdef USE_FAIL_MALLOC
int
grn_fail_malloc_check(size_t size, const char *file, int line, const char *func)
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
grn_malloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_malloc_default(ctx, size, file, line, func);
  } else {
    MERR("fail_malloc (%d) (%s:%d@%s) <%d>", size, file, line, func, alloc_count);
    return NULL;
  }
}

void *
grn_calloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(size, file, line, func)) {
    return grn_calloc_default(ctx, size, file, line, func);
  } else {
    MERR("fail_calloc (%d) (%s:%d@%s) <%d>", size, file, line, func, alloc_count);
    return NULL;
  }
}

void *
grn_realloc_fail(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line,
                 const char *func)
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
grn_strdup_fail(grn_ctx *ctx, const char *s, const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(strlen(s), file, line, func)) {
    return grn_strdup_default(ctx, s, file, line, func);
  } else {
    MERR("fail_strdup(%p) (%s:%d@%s) <%d>", s, file, line, func, alloc_count);
    return NULL;
  }
}
#endif /* USE_FAIL_MALLOC */

/* don't handle error inside logger functions */

void
grn_ctx_log(grn_ctx *ctx, const char *fmt, ...)
{
  va_list argp;
  va_start(argp, fmt);
  vsnprintf(ctx->errbuf, GRN_CTX_MSGSIZE, fmt, argp);
  va_end(argp);
}

void
grn_assert(grn_ctx *ctx, int cond, const char* file, int line, const char* func)
{
  if (!cond) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "ASSERT fail on %s %s:%d", func, file, line);
  }
}

const char *
grn_get_version(void)
{
  return GRN_VERSION;
}

const char *
grn_get_package(void)
{
  return PACKAGE;
}

#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
static int segv_received = 0;
static void
segv_handler(int signal_number, siginfo_t *info, void *context)
{
  grn_ctx *ctx = &grn_gctx;

  if (segv_received) {
    GRN_LOG(ctx, GRN_LOG_CRIT, "SEGV received in SEGV handler.");
    exit(EXIT_FAILURE);
  }
  segv_received = 1;

  GRN_LOG(ctx, GRN_LOG_CRIT, "-- CRASHED!!! --");
#ifdef HAVE_BACKTRACE
#  define N_TRACE_LEVEL 1024
  {
    static void *trace[N_TRACE_LEVEL];
    int n = backtrace(trace, N_TRACE_LEVEL);
    char **symbols = backtrace_symbols(trace, n);
    int i;

    if (symbols) {
      for (i = 0; i < n; i++) {
        GRN_LOG(ctx, GRN_LOG_CRIT, "%s", symbols[i]);
      }
      free(symbols);
    }
  }
#else /* HAVE_BACKTRACE */
  GRN_LOG(ctx, GRN_LOG_CRIT, "backtrace() isn't available.");
#endif /* HAVE_BACKTRACE */
  GRN_LOG(ctx, GRN_LOG_CRIT, "----------------");
  abort();
}
#endif /* defined(HAVE_SIGNAL_H) && !defined(WIN32) */

grn_rc
grn_set_segv_handler(void)
{
  grn_rc rc = GRN_SUCCESS;
#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
  grn_ctx *ctx = &grn_gctx;
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_sigaction = segv_handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;

  if (sigaction(SIGSEGV, &action, NULL)) {
    SERR("failed to set SIGSEGV action");
    rc = ctx->rc;
  };
#endif
  return rc;
}

#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
static struct sigaction old_int_handler;
static void
int_handler(int signal_number, siginfo_t *info, void *context)
{
  grn_gctx.stat = GRN_CTX_QUIT;
  sigaction(signal_number, &old_int_handler, NULL);
}

static struct sigaction old_term_handler;
static void
term_handler(int signal_number, siginfo_t *info, void *context)
{
  grn_gctx.stat = GRN_CTX_QUIT;
  sigaction(signal_number, &old_term_handler, NULL);
}
#endif /* defined(HAVE_SIGNAL_H) && !defined(WIN32) */

grn_rc
grn_set_int_handler(void)
{
  grn_rc rc = GRN_SUCCESS;
#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
  grn_ctx *ctx = &grn_gctx;
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_sigaction = int_handler;
  action.sa_flags = SA_SIGINFO;

  if (sigaction(SIGINT, &action, &old_int_handler)) {
    SERR("failed to set SIGINT action");
    rc = ctx->rc;
  }
#endif
  return rc;
}

grn_rc
grn_set_term_handler(void)
{
  grn_rc rc = GRN_SUCCESS;
#if defined(HAVE_SIGNAL_H) && !defined(WIN32)
  grn_ctx *ctx = &grn_gctx;
  struct sigaction action;

  sigemptyset(&action.sa_mask);
  action.sa_sigaction = term_handler;
  action.sa_flags = SA_SIGINFO;

  if (sigaction(SIGTERM, &action, &old_term_handler)) {
    SERR("failed to set SIGTERM action");
    rc = ctx->rc;
  }
#endif
  return rc;
}

void
grn_ctx_output_flush(grn_ctx *ctx, int flags)
{
  if (flags & GRN_CTX_QUIET) {
    return;
  }
  if (!ctx->impl->output) {
    return;
  }
  ctx->impl->output(ctx, 0, ctx->impl->data.ptr);
}

void
grn_ctx_output_array_open(grn_ctx *ctx, const char *name, int nelements)
{
  grn_output_array_open(ctx, ctx->impl->outbuf, ctx->impl->output_type,
                        name, nelements);
}

void
grn_ctx_output_array_close(grn_ctx *ctx)
{
  grn_output_array_close(ctx, ctx->impl->outbuf, ctx->impl->output_type);
}

void
grn_ctx_output_map_open(grn_ctx *ctx, const char *name, int nelements)
{
  grn_output_map_open(ctx, ctx->impl->outbuf, ctx->impl->output_type,
                      name, nelements);
}

void
grn_ctx_output_map_close(grn_ctx *ctx)
{
  grn_output_map_close(ctx, ctx->impl->outbuf, ctx->impl->output_type);
}

void
grn_ctx_output_int32(grn_ctx *ctx, int value)
{
  grn_output_int32(ctx, ctx->impl->outbuf, ctx->impl->output_type, value);
}

void
grn_ctx_output_int64(grn_ctx *ctx, long long int value)
{
  grn_output_int64(ctx, ctx->impl->outbuf, ctx->impl->output_type, value);
}

void
grn_ctx_output_float(grn_ctx *ctx, double value)
{
  grn_output_float(ctx, ctx->impl->outbuf, ctx->impl->output_type, value);
}

void
grn_ctx_output_cstr(grn_ctx *ctx, const char *value)
{
  grn_output_cstr(ctx, ctx->impl->outbuf, ctx->impl->output_type, value);
}

void
grn_ctx_output_str(grn_ctx *ctx, const char *value, unsigned int value_len)
{
  grn_output_str(ctx, ctx->impl->outbuf, ctx->impl->output_type,
                 value, value_len);
}

void
grn_ctx_output_bool(grn_ctx *ctx, grn_bool value)
{
  grn_output_bool(ctx, ctx->impl->outbuf, ctx->impl->output_type, value);
}

void
grn_ctx_output_obj(grn_ctx *ctx, grn_obj *value, grn_obj_format *format)
{
  grn_output_obj(ctx, ctx->impl->outbuf, ctx->impl->output_type,
                 value, format);
}

void
grn_ctx_output_table_columns(grn_ctx *ctx, grn_obj *table,
                             grn_obj_format *format)
{
  grn_output_table_columns(ctx,
                           ctx->impl->outbuf,
                           ctx->impl->output_type,
                           table,
                           format);
}

void
grn_ctx_output_table_records(grn_ctx *ctx, grn_obj *table,
                             grn_obj_format *format)
{
  grn_output_table_records(ctx,
                           ctx->impl->outbuf,
                           ctx->impl->output_type,
                           table,
                           format);
}
