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
#ifndef GRN_CTX_H
#define GRN_CTX_H

#include "grn.h"
#include "grn_error.h"

#include <errno.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#define GRN_BREAK_POINT raise(SIGTRAP)
#endif /* HAVE_SIGNAL_H */

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif /* HAVE_EXECINFO_H */

#include "grn_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**** api in/out ****/

#define GRN_API_ENTER do {\
  if ((ctx)->seqno & 1) {\
    (ctx)->subno++;\
  } else {\
    (ctx)->errlvl = GRN_OK;\
    (ctx)->rc = GRN_SUCCESS;\
    (ctx)->seqno++;\
  }\
  GRN_TEST_YIELD();\
} while (0)

/* CAUTION!! : pass only variables or constants as r */
#define GRN_API_RETURN(r) do {\
  if (ctx->subno) {\
    ctx->subno--;\
  } else {\
    ctx->seqno++;\
  }\
  GRN_TEST_YIELD();\
  return r;\
} while (0)

/**** error handling ****/

#define  GRN_EMERG  GRN_LOG_EMERG
#define  GRN_ALERT  GRN_LOG_ALERT
#define  GRN_CRIT   GRN_LOG_CRIT
#define  GRN_ERROR  GRN_LOG_ERROR
#define  GRN_WARN   GRN_LOG_WARNING
#define  GRN_OK     GRN_LOG_NOTICE

#define ERRCLR(ctx) do {\
  if (ctx) {\
    ((grn_ctx *)ctx)->errlvl = GRN_OK;\
    ((grn_ctx *)ctx)->rc = GRN_SUCCESS;\
  }\
  errno = 0;\
  grn_gctx.errlvl = GRN_OK;\
  grn_gctx.rc = GRN_SUCCESS;\
} while (0)

#ifdef HAVE_BACKTRACE
#define BACKTRACE(ctx) ((ctx)->ntrace = (unsigned char)backtrace((ctx)->trace, 16))
#else /* HAVE_BACKTRACE */
#define BACKTRACE(ctx)
#endif /* HAVE_BACKTRACE */

GRN_API grn_bool grn_ctx_impl_should_log(grn_ctx *ctx);
GRN_API void grn_ctx_impl_set_current_error_message(grn_ctx *ctx);

#ifdef HAVE_BACKTRACE
#define LOGTRACE(ctx,lvl) do {\
  int i;\
  char **p;\
  BACKTRACE(ctx);\
  p = backtrace_symbols((ctx)->trace, (ctx)->ntrace);\
  for (i = 0; i < (ctx)->ntrace; i++) {\
    GRN_LOG((ctx), lvl, "%s", p[i]);\
  }\
  free(p);\
} while (0)
#else  /* HAVE_BACKTRACE */
#define LOGTRACE(ctx,msg)
#endif /* HAVE_BACKTRACE */

#define ERRSET(ctx,lvl,r,...) do {\
  grn_ctx *ctx_ = (grn_ctx *)ctx;\
  ctx_->errlvl = (lvl);\
  ctx_->rc = (r);\
  ctx_->errfile = __FILE__;\
  ctx_->errline = __LINE__;\
  ctx_->errfunc = __FUNCTION__;\
  grn_ctx_log(ctx, __VA_ARGS__);\
  if (grn_ctx_impl_should_log(ctx)) {\
    grn_ctx_impl_set_current_error_message(ctx);\
    GRN_LOG(ctx, lvl, __VA_ARGS__);\
    BACKTRACE(ctx);\
    if (lvl <= GRN_LOG_ERROR) { LOGTRACE(ctx, lvl); }\
  }\
} while (0)

#define ERRP(ctx,lvl) \
  (((ctx) && ((grn_ctx *)(ctx))->errlvl <= (lvl)) || (grn_gctx.errlvl <= (lvl)))

#ifdef ERR
#  undef ERR
#endif /* ERR */
#define ERR(rc,...) ERRSET(ctx, GRN_ERROR, (rc),  __VA_ARGS__)
#define WARN(rc,...) ERRSET(ctx, GRN_WARN, (rc),  __VA_ARGS__)
#define MERR(...) ERRSET(ctx, GRN_ALERT, GRN_NO_MEMORY_AVAILABLE,  __VA_ARGS__)
#define ALERT(...) ERRSET(ctx, GRN_ALERT, GRN_SUCCESS,  __VA_ARGS__)

#define ERR_CAST(column, range, element) do {\
  grn_obj inspected;\
  char column_name[GRN_TABLE_MAX_KEY_SIZE];\
  int column_name_size;\
  char range_name[GRN_TABLE_MAX_KEY_SIZE];\
  int range_name_size;\
  GRN_TEXT_INIT(&inspected, 0);\
  grn_inspect(ctx, &inspected, element);\
  column_name_size = grn_obj_name(ctx, column, column_name,\
                                  GRN_TABLE_MAX_KEY_SIZE);\
  range_name_size = grn_obj_name(ctx, range, range_name,\
                                 GRN_TABLE_MAX_KEY_SIZE);\
  ERR(GRN_INVALID_ARGUMENT, "<%.*s>: failed to cast to <%.*s>: <%.*s>",\
      column_name_size, column_name,\
      range_name_size, range_name,\
      (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));\
  GRN_OBJ_FIN(ctx, &inspected);\
} while (0)

#ifdef WIN32

#define SYSTEM_ERROR_MESSAGE_BUFFER_SIZE 1024
#define SERR(str) do {\
  grn_rc rc;\
  const char *system_message;\
  int error = GetLastError();\
  system_message = grn_current_error_message();\
  switch (error) {\
  case ERROR_FILE_NOT_FOUND :\
  case ERROR_PATH_NOT_FOUND :\
    rc = GRN_NO_SUCH_FILE_OR_DIRECTORY;\
    break;\
  case ERROR_TOO_MANY_OPEN_FILES :\
    rc = GRN_TOO_MANY_OPEN_FILES;\
    break;\
  case ERROR_ACCESS_DENIED :\
    rc = GRN_PERMISSION_DENIED;\
    break;\
  case ERROR_INVALID_HANDLE :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_ARENA_TRASHED :\
    rc = GRN_ADDRESS_IS_NOT_AVAILABLE;\
    break;\
  case ERROR_NOT_ENOUGH_MEMORY :\
    rc = GRN_NO_MEMORY_AVAILABLE;\
    break;\
  case ERROR_INVALID_BLOCK :\
  case ERROR_BAD_ENVIRONMENT :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_BAD_FORMAT :\
    rc = GRN_INVALID_FORMAT;\
    break;\
  case ERROR_INVALID_DATA :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_OUTOFMEMORY :\
    rc = GRN_NO_MEMORY_AVAILABLE;\
    break;\
  case ERROR_INVALID_DRIVE :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_WRITE_PROTECT :\
    rc = GRN_PERMISSION_DENIED;\
    break;\
  case ERROR_BAD_LENGTH :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_SEEK :\
    rc = GRN_INVALID_SEEK;\
    break;\
  case ERROR_NOT_SUPPORTED :\
    rc = GRN_OPERATION_NOT_SUPPORTED;\
    break;\
  case ERROR_NETWORK_ACCESS_DENIED :\
    rc = GRN_OPERATION_NOT_PERMITTED;\
    break;\
  case ERROR_FILE_EXISTS :\
    rc = GRN_FILE_EXISTS;\
    break;\
  case ERROR_INVALID_PARAMETER :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_BROKEN_PIPE :\
    rc = GRN_BROKEN_PIPE;\
    break;\
  case ERROR_CALL_NOT_IMPLEMENTED :\
    rc = GRN_FUNCTION_NOT_IMPLEMENTED;\
    break;\
  case ERROR_INVALID_NAME :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_BUSY_DRIVE :\
  case ERROR_PATH_BUSY :\
    rc = GRN_RESOURCE_BUSY;\
    break;\
  case ERROR_BAD_ARGUMENTS :\
    rc = GRN_INVALID_ARGUMENT;\
    break;\
  case ERROR_BUSY :\
    rc = GRN_RESOURCE_BUSY;\
    break;\
  case ERROR_ALREADY_EXISTS :\
    rc = GRN_FILE_EXISTS;\
    break;\
  case ERROR_BAD_EXE_FORMAT :\
    rc = GRN_EXEC_FORMAT_ERROR;\
    break;\
  default:\
    rc = GRN_UNKNOWN_ERROR;\
    break;\
  }\
  ERR(rc, "syscall error '%s' (%s)[%d]", str, system_message, error);\
} while (0)

#define SOERR(str) do {\
  grn_rc rc;\
  const char *m;\
  int e = WSAGetLastError();\
  switch (e) {\
  case WSANOTINITIALISED :\
    rc = GRN_SOCKET_NOT_INITIALIZED;\
    m = "please call grn_com_init first";\
    break;\
  case WSAEFAULT :\
    rc = GRN_BAD_ADDRESS;\
    m = "bad address";\
    break;\
  case WSAEINVAL :\
    rc = GRN_INVALID_ARGUMENT;\
    m = "invalid argument";\
    break;\
  case WSAEMFILE :\
    rc = GRN_TOO_MANY_OPEN_FILES;\
    m = "too many sockets";\
    break;\
  case WSAEWOULDBLOCK :\
    rc = GRN_OPERATION_WOULD_BLOCK;\
    m = "operation would block";\
    break;\
  case WSAENOTSOCK :\
    rc = GRN_NOT_SOCKET;\
    m = "given fd is not socket fd";\
    break;\
  case WSAEOPNOTSUPP :\
    rc = GRN_OPERATION_NOT_SUPPORTED;\
    m = "operation is not supported";\
    break;\
  case WSAEADDRINUSE :\
    rc = GRN_ADDRESS_IS_IN_USE;\
    m = "address is already in use";\
    break;\
  case WSAEADDRNOTAVAIL :\
    rc = GRN_ADDRESS_IS_NOT_AVAILABLE;\
    m = "address is not available";\
    break;\
  case WSAENETDOWN :\
    rc = GRN_NETWORK_IS_DOWN;\
    m = "network is down";\
    break;\
  case WSAENOBUFS :\
    rc = GRN_NO_BUFFER;\
    m = "no buffer";\
    break;\
  case WSAEISCONN :\
    rc = GRN_SOCKET_IS_ALREADY_CONNECTED;\
    m = "socket is already connected";\
    break;\
  case WSAENOTCONN :\
    rc = GRN_SOCKET_IS_NOT_CONNECTED;\
    m = "socket is not connected";\
    break;\
  case WSAESHUTDOWN :\
    rc = GRN_SOCKET_IS_ALREADY_SHUTDOWNED;\
    m = "socket is already shutdowned";\
    break;\
  case WSAETIMEDOUT :\
    rc = GRN_OPERATION_TIMEOUT;\
    m = "connection time out";\
    break;\
  case WSAECONNREFUSED :\
    rc = GRN_CONNECTION_REFUSED;\
    m = "connection refused";\
    break;\
  case WSAEINTR :\
    rc = GRN_INTERRUPTED_FUNCTION_CALL;\
    m = "interrupted function call";\
    break;\
  default:\
    rc = GRN_UNKNOWN_ERROR;\
    m = "unknown error";\
    break;\
  }\
  ERR(rc, "socket error '%s' (%s)[%d]", str, m, e);\
} while (0)

#else /* WIN32 */
#define SERR(str) do {\
  grn_rc rc;\
  int errno_keep = errno;\
  grn_bool show_errno = GRN_FALSE;\
  const char *system_message = grn_current_error_message();\
  switch (errno_keep) {\
  case ELOOP : rc = GRN_TOO_MANY_SYMBOLIC_LINKS; break;\
  case ENAMETOOLONG : rc = GRN_FILENAME_TOO_LONG; break;\
  case ENOENT : rc = GRN_NO_SUCH_FILE_OR_DIRECTORY; break;\
  case ENOMEM : rc = GRN_NO_MEMORY_AVAILABLE; break;\
  case ENOTDIR : rc = GRN_NOT_A_DIRECTORY; break;\
  case EPERM : rc = GRN_OPERATION_NOT_PERMITTED; break;\
  case ESRCH : rc = GRN_NO_SUCH_PROCESS; break;\
  case EINTR : rc = GRN_INTERRUPTED_FUNCTION_CALL; break;\
  case EIO : rc = GRN_INPUT_OUTPUT_ERROR; break;\
  case ENXIO : rc = GRN_NO_SUCH_DEVICE_OR_ADDRESS; break;\
  case E2BIG : rc = GRN_ARG_LIST_TOO_LONG; break;\
  case ENOEXEC : rc = GRN_EXEC_FORMAT_ERROR; break;\
  case EBADF : rc = GRN_BAD_FILE_DESCRIPTOR; break;\
  case ECHILD : rc = GRN_NO_CHILD_PROCESSES; break;\
  case EACCES : rc = GRN_PERMISSION_DENIED; break;\
  case EFAULT : rc = GRN_BAD_ADDRESS; break;\
  case EBUSY : rc = GRN_RESOURCE_BUSY; break;\
  case EEXIST : rc = GRN_FILE_EXISTS; break;\
  case ENODEV : rc = GRN_NO_SUCH_DEVICE; break;\
  case EISDIR : rc = GRN_IS_A_DIRECTORY; break;\
  case EINVAL : rc = GRN_INVALID_ARGUMENT; break;\
  case EMFILE : rc = GRN_TOO_MANY_OPEN_FILES; break;\
  case EFBIG : rc = GRN_FILE_TOO_LARGE; break;\
  case ENOSPC : rc = GRN_NO_SPACE_LEFT_ON_DEVICE; break;\
  case EROFS : rc = GRN_READ_ONLY_FILE_SYSTEM; break;\
  case EMLINK : rc = GRN_TOO_MANY_LINKS; break;\
  case EPIPE : rc = GRN_BROKEN_PIPE; break;\
  case EDOM : rc = GRN_DOMAIN_ERROR; break;\
  case ERANGE : rc = GRN_RANGE_ERROR; break;\
  case ENOTSOCK : rc = GRN_NOT_SOCKET; break;\
  case EADDRINUSE : rc = GRN_ADDRESS_IS_IN_USE; break;\
  case ENETDOWN : rc = GRN_NETWORK_IS_DOWN; break;\
  case ENOBUFS : rc = GRN_NO_BUFFER; break;\
  case EISCONN : rc = GRN_SOCKET_IS_ALREADY_CONNECTED; break;\
  case ENOTCONN : rc = GRN_SOCKET_IS_NOT_CONNECTED; break;\
    /*\
  case ESOCKTNOSUPPORT :\
  case EOPNOTSUPP :\
  case EPFNOSUPPORT :\
    */\
  case EPROTONOSUPPORT : rc = GRN_OPERATION_NOT_SUPPORTED; break;\
  case ESHUTDOWN : rc = GRN_SOCKET_IS_ALREADY_SHUTDOWNED; break;\
  case ETIMEDOUT : rc = GRN_OPERATION_TIMEOUT; break;\
  case ECONNREFUSED: rc = GRN_CONNECTION_REFUSED; break;\
  case EAGAIN: rc = GRN_OPERATION_WOULD_BLOCK; break;\
  default :\
    rc = GRN_UNKNOWN_ERROR;\
    show_errno = GRN_TRUE;\
    break;\
  }\
  if (show_errno) {\
    ERR(rc, "syscall error '%s' (%s)[%d]", str, system_message, errno_keep); \
  } else {\
    ERR(rc, "syscall error '%s' (%s)", str, system_message);\
  }\
} while (0)

#define SOERR(str) SERR(str)

#endif /* WIN32 */

#define GERR(rc,...) ERRSET(&grn_gctx, GRN_ERROR, (rc),  __VA_ARGS__)
#define GMERR(...)   ERRSET(&grn_gctx, GRN_ALERT, GRN_NO_MEMORY_AVAILABLE,  __VA_ARGS__)

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

#ifdef DEBUG
#define GRN_ASSERT(s) grn_assert(ctx,(s),__FILE__,__LINE__,__FUNCTION__)
#else
#define GRN_ASSERT(s)
#endif

#define GRN_CTX_ALLOC(ctx,s)   grn_ctx_calloc(ctx,s,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CTX_FREE(ctx,p)    grn_ctx_free(ctx,p,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CTX_ALLOC_L(ctx,s) grn_ctx_alloc_lifo(ctx,s,f,__FILE__,__LINE__,__FUNCTION__)
#define GRN_CTX_FREE_L(ctx,p)  grn_ctx_free_lifo(ctx,p,__FILE__,__LINE__,__FUNCTION__)

void *grn_ctx_alloc(grn_ctx *ctx, size_t size, int flags,
                    const char* file, int line, const char *func);
void *grn_ctx_malloc(grn_ctx *ctx, size_t size,
                    const char* file, int line, const char *func);
void *grn_ctx_calloc(grn_ctx *ctx, size_t size,
                    const char* file, int line, const char *func);
void *grn_ctx_realloc(grn_ctx *ctx, void *ptr, size_t size,
                      const char* file, int line, const char *func);
char *grn_ctx_strdup(grn_ctx *ctx, const char *s,
                     const char* file, int line, const char *func);
void grn_ctx_free(grn_ctx *ctx, void *ptr,
                  const char* file, int line, const char *func);
void *grn_ctx_alloc_lifo(grn_ctx *ctx, size_t size,
                         const char* file, int line, const char *func);
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
grn_malloc_func grn_ctx_get_malloc(grn_ctx *ctx);
void grn_ctx_set_malloc(grn_ctx *ctx, grn_malloc_func malloc_func);
grn_calloc_func grn_ctx_get_calloc(grn_ctx *ctx);
void grn_ctx_set_calloc(grn_ctx *ctx, grn_calloc_func calloc_func);
grn_realloc_func grn_ctx_get_realloc(grn_ctx *ctx);
void grn_ctx_set_realloc(grn_ctx *ctx, grn_realloc_func realloc_func);
grn_strdup_func grn_ctx_get_strdup(grn_ctx *ctx);
void grn_ctx_set_strdup(grn_ctx *ctx, grn_strdup_func strdup_func);

void *grn_malloc(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_calloc(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_realloc(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func);
char *grn_strdup(grn_ctx *ctx, const char *s, const char* file, int line, const char *func);
#else
#  define grn_malloc  grn_malloc_default
#  define grn_calloc  grn_calloc_default
#  define grn_realloc grn_realloc_default
#  define grn_strdup  grn_strdup_default
#  define grn_free    grn_free_default
#endif

GRN_API void *grn_malloc_default(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_calloc_default(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_realloc_default(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func);
GRN_API char *grn_strdup_default(grn_ctx *ctx, const char *s, const char* file, int line, const char *func);
GRN_API void grn_free_default(grn_ctx *ctx, void *ptr, const char* file, int line, const char *func);

#ifdef USE_FAIL_MALLOC
int grn_fail_malloc_check(size_t size, const char *file, int line, const char *func);
void *grn_malloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_calloc_fail(grn_ctx *ctx, size_t size, const char* file, int line, const char *func);
void *grn_realloc_fail(grn_ctx *ctx, void *ptr, size_t size, const char* file, int line, const char *func);
char *grn_strdup_fail(grn_ctx *ctx, const char *s, const char* file, int line, const char *func);
#endif

void grn_assert(grn_ctx *ctx, int cond, const char* file, int line, const char* func);

/**** grn_ctx ****/

GRN_VAR grn_ctx grn_gctx;
extern int grn_pagesize;
extern grn_critical_section grn_glock;
extern uint32_t grn_gtick;
extern int grn_lock_timeout;

#define GRN_CTX_ALLOCATED                            (0x80)
#define GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND (0x40)

typedef struct {
  int64_t tv_sec;
  int32_t tv_nsec;
} grn_timeval;

extern grn_timeval grn_starttime;

#ifndef GRN_TIMEVAL_STR_SIZE
#define GRN_TIMEVAL_STR_SIZE 0x100
#endif /* GRN_TIMEVAL_STR_SIZE */
#ifndef GRN_TIMEVAL_STR_FORMAT
#define GRN_TIMEVAL_STR_FORMAT "%04d-%02d-%02d %02d:%02d:%02d.%06d"
#endif /* GRN_TIMEVAL_STR_FORMAT */
#define GRN_TIME_NSEC_PER_SEC 1000000000
#define GRN_TIME_NSEC_PER_SEC_F 1000000000.0
#define GRN_TIME_NSEC_PER_USEC (GRN_TIME_NSEC_PER_SEC / GRN_TIME_USEC_PER_SEC)
#define GRN_TIME_NSEC_TO_USEC(nsec) ((nsec) / GRN_TIME_NSEC_PER_USEC)
#define GRN_TIME_USEC_TO_NSEC(usec) ((usec) * GRN_TIME_NSEC_PER_USEC)

GRN_API grn_rc grn_timeval_now(grn_ctx *ctx, grn_timeval *tv);
GRN_API grn_rc grn_timeval2str(grn_ctx *ctx, grn_timeval *tv, char *buf);
grn_rc grn_str2timeval(const char *str, uint32_t str_len, grn_timeval *tv);

GRN_API void grn_ctx_log(grn_ctx *ctx, const char *fmt, ...) GRN_ATTRIBUTE_PRINTF(2);
void grn_ctx_loader_clear(grn_ctx *ctx);
void grn_log_reopen(grn_ctx *ctx);

GRN_API grn_rc grn_ctx_sendv(grn_ctx *ctx, int argc, char **argv, int flags);
GRN_API void grn_ctx_set_next_expr(grn_ctx *ctx, grn_obj *expr);

int grn_alloc_count(void);

grn_content_type grn_get_ctype(grn_obj *var);

/**** db_obj ****/

/* flag values used for grn_obj.header.impl_flags */

#define GRN_OBJ_ALLOCATED              (0x01<<2) /* allocated by ctx */
#define GRN_OBJ_EXPRVALUE              (0x01<<3) /* value allocated by grn_expr */
#define GRN_OBJ_EXPRCONST              (0x01<<4) /* constant allocated by grn_expr */

typedef struct _grn_hook grn_hook;

typedef struct {
  grn_obj_header header;
  grn_id range;  /* table: type of subrecords, column: type of values */
  /* -- compatible with grn_accessor -- */
  grn_id id;
  grn_obj *db;
  grn_user_data user_data;
  grn_proc_func *finalizer;
  grn_hook *hooks[5];
  void *source;
  uint32_t source_size;
  uint32_t max_n_subrecs;
  uint8_t subrec_size;
  uint8_t subrec_offset;
  uint8_t record_unit;
  uint8_t subrec_unit;
  union {
    grn_table_group_flags group;
  } flags;
  //  grn_obj_flags flags;
} grn_db_obj;

#define GRN_DB_OBJ_SET_TYPE(db_obj,obj_type) do {\
  (db_obj)->obj.header.type = (obj_type);\
  (db_obj)->obj.header.impl_flags = 0;\
  (db_obj)->obj.header.flags = 0;\
  (db_obj)->obj.id = GRN_ID_NIL;\
  (db_obj)->obj.user_data.ptr = NULL;\
  (db_obj)->obj.finalizer = NULL;\
  (db_obj)->obj.hooks[0] = NULL;\
  (db_obj)->obj.hooks[1] = NULL;\
  (db_obj)->obj.hooks[2] = NULL;\
  (db_obj)->obj.hooks[3] = NULL;\
  (db_obj)->obj.hooks[4] = NULL;\
  (db_obj)->obj.source = NULL;\
  (db_obj)->obj.source_size = 0;\
} while (0)

/**** cache ****/

typedef struct {
  uint32_t nentries;
  uint32_t max_nentries;
  uint32_t nfetches;
  uint32_t nhits;
} grn_cache_statistics;

void grn_cache_init(void);
grn_obj *grn_cache_fetch(grn_ctx *ctx, grn_cache *cache,
                         const char *str, uint32_t str_size);
void grn_cache_unref(grn_ctx *ctx, grn_cache *cache,
                     const char *str, uint32_t str_size);
void grn_cache_update(grn_ctx *ctx, grn_cache *cache,
                      const char *str, uint32_t str_size, grn_obj *value);
void grn_cache_expire(grn_cache *cache, int32_t size);
void grn_cache_fin(void);
void grn_cache_get_statistics(grn_ctx *ctx, grn_cache *cache,
                              grn_cache_statistics *statistics);

/**** receive handler ****/

GRN_API void grn_ctx_stream_out_func(grn_ctx *c, int flags, void *stream);

grn_rc grn_db_init_builtin_procs(grn_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* GRN_CTX_H */
