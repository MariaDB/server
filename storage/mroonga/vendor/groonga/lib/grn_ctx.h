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
#include "grn_alloc.h"
#include "grn_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/**** api in/out ****/

#define GRN_API_ENTER do {\
  if ((ctx)->seqno & 1) {\
    (ctx)->subno++;\
  } else {\
    (ctx)->errlvl = GRN_OK;\
    if ((ctx)->rc != GRN_CANCEL) {\
      (ctx)->rc = GRN_SUCCESS;\
    }\
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
    if (((grn_ctx *)ctx)->rc != GRN_CANCEL) {\
      ((grn_ctx *)ctx)->rc = GRN_SUCCESS;\
      ((grn_ctx *)ctx)->errbuf[0] = '\0';\
    }\
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
  if (!p) {\
    GRN_LOG((ctx), lvl, "backtrace_symbols failed");\
  } else {\
    for (i = 0; i < (ctx)->ntrace; i++) {\
      GRN_LOG((ctx), lvl, "%s", p[i]);\
    }\
    free(p);\
  }\
} while (0)
#else  /* HAVE_BACKTRACE */
#define LOGTRACE(ctx,msg)
#endif /* HAVE_BACKTRACE */

#define ERRSET(ctx,lvl,r,...) do {\
  grn_ctx *ctx_ = (grn_ctx *)ctx;\
  ctx_->errlvl = (lvl);\
  if (ctx_->rc != GRN_CANCEL) {\
    ctx_->rc = (r);\
  }\
  ctx_->errfile = __FILE__;\
  ctx_->errline = __LINE__;\
  ctx_->errfunc = __FUNCTION__;\
  grn_ctx_log(ctx, __VA_ARGS__);\
  if (grn_ctx_impl_should_log(ctx)) {\
    grn_ctx_impl_set_current_error_message(ctx);\
    GRN_LOG(ctx, lvl, __VA_ARGS__);\
    if (lvl <= GRN_LOG_ERROR) { LOGTRACE(ctx, lvl); }\
  }\
} while (0)

#define ERRP(ctx,lvl) \
  (((ctx) && ((grn_ctx *)(ctx))->errlvl <= (lvl)) || (grn_gctx.errlvl <= (lvl)))

#ifdef ERR
#  undef ERR
#endif /* ERR */
#define CRIT(rc,...) ERRSET(ctx, GRN_CRIT, (rc),  __VA_ARGS__)
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

#define USER_MESSAGE_SIZE 1024

#ifdef WIN32

#define SERR(...) do {\
  grn_rc rc;\
  int error_code;\
  const char *system_message;\
  char user_message[USER_MESSAGE_SIZE];\
  error_code = GetLastError();\
  system_message = grn_current_error_message();\
  rc = grn_windows_error_code_to_rc(error_code);\
  grn_snprintf(user_message,\
               USER_MESSAGE_SIZE, USER_MESSAGE_SIZE,\
               __VA_ARGS__);\
  ERR(rc, "system error[%d]: %s: %s",\
      error_code, system_message, user_message);\
} while (0)

#define SOERR(...) do {\
  grn_rc rc;\
  const char *m;\
  char user_message[USER_MESSAGE_SIZE];\
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
  grn_snprintf(user_message,\
               USER_MESSAGE_SIZE, USER_MESSAGE_SIZE,\
               __VA_ARGS__);\
  ERR(rc, "socket error[%d]: %s: %s",\
      e, m, user_message);\
} while (0)

#define ERRNO_ERR(...) do {\
  grn_rc rc;\
  int errno_keep = errno;\
  grn_bool show_errno = GRN_FALSE;\
  const char *system_message;\
  char user_message[USER_MESSAGE_SIZE];\
  system_message = grn_strerror(errno);\
  switch (errno_keep) {\
  case EPERM : rc = GRN_OPERATION_NOT_PERMITTED; break;\
  case ENOENT : rc = GRN_NO_SUCH_FILE_OR_DIRECTORY; break;\
  case ESRCH : rc = GRN_NO_SUCH_PROCESS; break;\
  case EINTR : rc = GRN_INTERRUPTED_FUNCTION_CALL; break;\
  case EIO : rc = GRN_INPUT_OUTPUT_ERROR; break;\
  case E2BIG : rc = GRN_ARG_LIST_TOO_LONG; break;\
  case ENOEXEC : rc = GRN_EXEC_FORMAT_ERROR; break;\
  case EBADF : rc = GRN_BAD_FILE_DESCRIPTOR; break;\
  case ECHILD : rc = GRN_NO_CHILD_PROCESSES; break;\
  case EAGAIN: rc = GRN_OPERATION_WOULD_BLOCK; break;\
  case ENOMEM : rc = GRN_NO_MEMORY_AVAILABLE; break;\
  case EACCES : rc = GRN_PERMISSION_DENIED; break;\
  case EFAULT : rc = GRN_BAD_ADDRESS; break;\
  case EEXIST : rc = GRN_FILE_EXISTS; break;\
  /* case EXDEV : */\
  case ENODEV : rc = GRN_NO_SUCH_DEVICE; break;\
  case ENOTDIR : rc = GRN_NOT_A_DIRECTORY; break;\
  case EISDIR : rc = GRN_IS_A_DIRECTORY; break;\
  case EINVAL : rc = GRN_INVALID_ARGUMENT; break;\
  case EMFILE : rc = GRN_TOO_MANY_OPEN_FILES; break;\
  case ENOTTY : rc = GRN_INAPPROPRIATE_I_O_CONTROL_OPERATION; break;\
  case EFBIG : rc = GRN_FILE_TOO_LARGE; break;\
  case ENOSPC : rc = GRN_NO_SPACE_LEFT_ON_DEVICE; break;\
  case ESPIPE : rc = GRN_INVALID_SEEK; break;\
  case EROFS : rc = GRN_READ_ONLY_FILE_SYSTEM; break;\
  case EMLINK : rc = GRN_TOO_MANY_LINKS; break;\
  case EPIPE : rc = GRN_BROKEN_PIPE; break;\
  case EDOM : rc = GRN_DOMAIN_ERROR; break;\
  case ERANGE : rc = GRN_RANGE_ERROR; break;\
  case EDEADLOCK : rc = GRN_RESOURCE_DEADLOCK_AVOIDED; break;\
  case ENAMETOOLONG : rc = GRN_FILENAME_TOO_LONG; break;\
  case EILSEQ : rc = GRN_ILLEGAL_BYTE_SEQUENCE; break;\
  /* case STRUNCATE : */\
  default :\
    rc = GRN_UNKNOWN_ERROR;\
    show_errno = GRN_TRUE;\
    break;\
  }\
  grn_snprintf(user_message,\
               USER_MESSAGE_SIZE, USER_MESSAGE_SIZE,\
               __VA_ARGS__);\
  if (show_errno) {\
    ERR(rc, "system call error[%d]: %s: %s",\
        errno_keep, system_message, user_message);\
  } else {\
    ERR(rc, "system call error: %s: %s",\
        system_message, user_message);\
  }\
} while (0)

#else /* WIN32 */

#define SERR(...) do {\
  grn_rc rc;\
  int errno_keep = errno;\
  grn_bool show_errno = GRN_FALSE;\
  const char *system_message = grn_current_error_message();\
  char user_message[USER_MESSAGE_SIZE];\
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
  grn_snprintf(user_message,\
               USER_MESSAGE_SIZE, USER_MESSAGE_SIZE,\
               __VA_ARGS__);\
  if (show_errno) {\
    ERR(rc, "system call error[%d]: %s: %s",\
        errno_keep, system_message, user_message);\
  } else {\
    ERR(rc, "system call error: %s: %s",\
        system_message, user_message);\
  }\
} while (0)

#define SOERR(...) SERR(__VA_ARGS__)

#define ERRNO_ERR(...) SERR(__VA_ARGS__)

#endif /* WIN32 */

#define GERR(rc,...) ERRSET(&grn_gctx, GRN_ERROR, (rc),  __VA_ARGS__)
#define GMERR(...)   ERRSET(&grn_gctx, GRN_ALERT, GRN_NO_MEMORY_AVAILABLE,  __VA_ARGS__)

#ifdef DEBUG
#define GRN_ASSERT(s) grn_assert(ctx,(s),__FILE__,__LINE__,__FUNCTION__)
#else
#define GRN_ASSERT(s)
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

extern grn_timeval grn_starttime;

GRN_API void grn_ctx_log(grn_ctx *ctx, const char *fmt, ...) GRN_ATTRIBUTE_PRINTF(2);
GRN_API void grn_ctx_logv(grn_ctx *ctx, const char *fmt, va_list ap);
void grn_ctx_loader_clear(grn_ctx *ctx);
void grn_log_reopen(grn_ctx *ctx);

GRN_API grn_rc grn_ctx_sendv(grn_ctx *ctx, int argc, char **argv, int flags);
void grn_ctx_set_keep_command(grn_ctx *ctx, grn_obj *command);

grn_content_type grn_get_ctype(grn_obj *var);
grn_content_type grn_content_type_parse(grn_ctx *ctx,
                                        grn_obj *var,
                                        grn_content_type default_value);

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
  (db_obj)->obj.header.domain = GRN_ID_NIL;\
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

/**** receive handler ****/

GRN_API void grn_ctx_stream_out_func(grn_ctx *c, int flags, void *stream);

grn_rc grn_db_init_builtin_procs(grn_ctx *ctx);

#ifdef __cplusplus
}
#endif
