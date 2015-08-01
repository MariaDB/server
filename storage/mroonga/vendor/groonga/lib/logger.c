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

#include "grn_logger.h"
#include "grn_ctx.h"
#include "grn_ctx_impl.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef WIN32
# include <share.h>
#endif /* WIN32 */

#ifdef WIN32
# define fileno(file) _fileno(file)
#endif

static void
rotate_log_file(grn_ctx *ctx, const char *current_path)
{
  char rotated_path[PATH_MAX];
  grn_timeval now;
  struct tm tm_buffer;
  struct tm *tm;

  grn_timeval_now(ctx, &now);
  tm = grn_timeval2tm(ctx, &now, &tm_buffer);
  grn_snprintf(rotated_path, PATH_MAX, PATH_MAX,
               "%s.%04d-%02d-%02d-%02d-%02d-%02d-%06d",
               current_path,
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec,
               (int)(GRN_TIME_NSEC_TO_USEC(now.tv_nsec)));
  rename(current_path, rotated_path);
}

static char *default_logger_path = NULL;
static FILE *default_logger_file = NULL;
static grn_critical_section default_logger_lock;
static off_t default_logger_size = 0;
static off_t default_logger_rotate_threshold_size = 0;

#define LOGGER_NEED_ROTATE(size, threshold) \
  ((threshold) > 0 && (size) >= (threshold))

static void
default_logger_log(grn_ctx *ctx, grn_log_level level,
                   const char *timestamp, const char *title,
                   const char *message, const char *location, void *user_data)
{
  const char slev[] = " EACewnid-";
  if (default_logger_path) {
    CRITICAL_SECTION_ENTER(default_logger_lock);
    if (!default_logger_file) {
      default_logger_file = grn_fopen(default_logger_path, "a");
      default_logger_size = 0;
      if (default_logger_file) {
        struct stat stat;
        if (fstat(fileno(default_logger_file), &stat) != -1) {
          default_logger_size = stat.st_size;
        }
      }
    }
    if (default_logger_file) {
      int written;
      if (location && *location) {
        written = fprintf(default_logger_file, "%s|%c|%s %s %s\n",
                          timestamp, *(slev + level), title, message, location);
      } else {
        written = fprintf(default_logger_file, "%s|%c|%s %s\n", timestamp,
                          *(slev + level), title, message);
      }
      if (written > 0) {
        default_logger_size += written;
        if (LOGGER_NEED_ROTATE(default_logger_size,
                               default_logger_rotate_threshold_size)) {
          fclose(default_logger_file);
          default_logger_file = NULL;
          rotate_log_file(ctx, default_logger_path);
        } else {
          fflush(default_logger_file);
        }
      }
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
    default_logger_path = grn_strdup_raw(path);
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
grn_default_logger_set_rotate_threshold_size(off_t threshold)
{
  default_logger_rotate_threshold_size = threshold;
}

off_t
grn_default_logger_get_rotate_threshold_size(void)
{
  return default_logger_rotate_threshold_size;
}

void
grn_logger_reopen(grn_ctx *ctx)
{
  if (current_logger.reopen) {
    current_logger.reopen(ctx, current_logger.user_data);
  }
}

static void
current_logger_fin(grn_ctx *ctx)
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
  current_logger_fin(ctx);
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
      grn_timeval2str(ctx, &tv, tbuf, TBUFSIZE);
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
      grn_snprintf(lbuf, LBUFSIZE, LBUFSIZE,
                   "%d %s:%d %s()", getpid(), file, line, func);
      lbuf[LBUFSIZE - 1] = '\0';
    } else {
      lbuf[0] = '\0';
    }
    current_logger.log(ctx, level, tbuf, "", mbuf, lbuf,
                       current_logger.user_data);
  }
}

void
grn_logger_init(void)
{
  grn_memcpy(&current_logger, &default_logger, sizeof(grn_logger));
  CRITICAL_SECTION_INIT(default_logger_lock);
}

void
grn_logger_fin(grn_ctx *ctx)
{
  current_logger_fin(ctx);
  if (default_logger_path) {
    free(default_logger_path);
    default_logger_path = NULL;
  }
  CRITICAL_SECTION_FIN(default_logger_lock);
}


static char *default_query_logger_path = NULL;
static FILE *default_query_logger_file = NULL;
static grn_critical_section default_query_logger_lock;
static off_t default_query_logger_size = 0;
static off_t default_query_logger_rotate_threshold_size = 0;

static void
default_query_logger_log(grn_ctx *ctx, unsigned int flag,
                         const char *timestamp, const char *info,
                         const char *message, void *user_data)
{
  if (default_query_logger_path) {
    CRITICAL_SECTION_ENTER(default_query_logger_lock);
    if (!default_query_logger_file) {
      default_query_logger_file = grn_fopen(default_query_logger_path, "a");
      default_query_logger_size = 0;
      if (default_query_logger_file) {
        struct stat stat;
        if (fstat(fileno(default_query_logger_file), &stat) != -1) {
          default_query_logger_size = stat.st_size;
        }
      }
    }
    if (default_query_logger_file) {
      int written;
      written = fprintf(default_query_logger_file, "%s|%s%s\n",
                        timestamp, info, message);
      if (written > 0) {
        default_query_logger_size += written;
        if (LOGGER_NEED_ROTATE(default_query_logger_size,
                               default_query_logger_rotate_threshold_size)) {
          fclose(default_query_logger_file);
          default_query_logger_file = NULL;
          rotate_log_file(ctx, default_query_logger_path);
        } else {
          fflush(default_query_logger_file);
        }
      }
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
    default_query_logger_path = grn_strdup_raw(path);
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
grn_default_query_logger_set_rotate_threshold_size(off_t threshold)
{
  default_query_logger_rotate_threshold_size = threshold;
}

off_t
grn_default_query_logger_get_rotate_threshold_size(void)
{
  return default_query_logger_rotate_threshold_size;
}

void
grn_query_logger_reopen(grn_ctx *ctx)
{
  if (current_query_logger.reopen) {
    current_query_logger.reopen(ctx, current_query_logger.user_data);
  }
}

static void
current_query_logger_fin(grn_ctx *ctx)
{
  if (current_query_logger.fin) {
    current_query_logger.fin(ctx, current_query_logger.user_data);
  }
}

grn_rc
grn_query_logger_set(grn_ctx *ctx, const grn_query_logger *logger)
{
  current_query_logger_fin(ctx);
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
    grn_timeval2str(ctx, &tv, timestamp, TIMESTAMP_BUFFER_SIZE);
  }

  if (flag & (GRN_QUERY_LOG_COMMAND | GRN_QUERY_LOG_DESTINATION)) {
    grn_snprintf(info, INFO_BUFFER_SIZE, INFO_BUFFER_SIZE,
                 "%p|%s", ctx, mark);
    info[INFO_BUFFER_SIZE - 1] = '\0';
  } else {
    grn_timeval tv;
    uint64_t elapsed_time;
    grn_timeval_now(ctx, &tv);
    elapsed_time =
      (uint64_t)(tv.tv_sec - ctx->impl->tv.tv_sec) * GRN_TIME_NSEC_PER_SEC +
      (tv.tv_nsec - ctx->impl->tv.tv_nsec);

    grn_snprintf(info, INFO_BUFFER_SIZE, INFO_BUFFER_SIZE,
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

void
grn_query_logger_init(void)
{
  grn_memcpy(&current_query_logger,
             &default_query_logger, sizeof(grn_query_logger));
  CRITICAL_SECTION_INIT(default_query_logger_lock);
}

void
grn_query_logger_fin(grn_ctx *ctx)
{
  current_query_logger_fin(ctx);
  if (default_query_logger_path) {
    free(default_query_logger_path);
    default_query_logger_path = NULL;
  }
  CRITICAL_SECTION_FIN(default_query_logger_lock);
}

void
grn_log_reopen(grn_ctx *ctx)
{
  grn_logger_reopen(ctx);
  grn_query_logger_reopen(ctx);
}
