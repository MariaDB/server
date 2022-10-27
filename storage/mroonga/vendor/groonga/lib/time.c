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

#include "grn_time.h"
#include "grn_ctx.h"
#include "grn_str.h"

#include <stdio.h>
#include <time.h>

#if defined(HAVE__LOCALTIME64_S) && defined(__GNUC__)
# ifdef _WIN64
#  define localtime_s(tm, time) _localtime64_s(tm, time)
# else /* _WIN64 */
#  define localtime_s(tm, time) _localtime32_s(tm, time)
# endif /* _WIN64 */
#endif /* defined(HAVE__LOCALTIME64_S) && defined(__GNUC__) */

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
# ifdef WIN32
  time_t t;
  struct _timeb tb;
  time(&t);
  _ftime(&tb);
  tv->tv_sec = t;
  tv->tv_nsec = tb.millitm * (GRN_TIME_NSEC_PER_SEC / 1000);
  return GRN_SUCCESS;
# else /* WIN32 */
  struct timeval t;
  if (gettimeofday(&t, NULL)) {
    SERR("gettimeofday");
  } else {
    tv->tv_sec = t.tv_sec;
    tv->tv_nsec = GRN_TIME_USEC_TO_NSEC(t.tv_usec);
  }
  return ctx->rc;
# endif /* WIN32 */
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

static grn_bool
grn_time_t_to_tm(grn_ctx *ctx, const time_t time, struct tm *tm)
{
  grn_bool success;
  const char *function_name;
#ifdef HAVE__LOCALTIME64_S
  function_name = "localtime_s";
  success = (localtime_s(tm, &time) == 0);
#else /* HAVE__LOCALTIME64_S */
# ifdef HAVE_LOCALTIME_R
  function_name = "localtime_r";
  success = (localtime_r(&time, tm) != NULL);
# else /* HAVE_LOCALTIME_R */
  function_name = "localtime";
  {
    struct tm *local_tm;
    local_tm = localtime(&time);
    if (local_tm) {
      success = GRN_TRUE;
      memcpy(tm, local_tm, sizeof(struct tm));
    } else {
      success = GRN_FALSE;
    }
  }
# endif /* HAVE_LOCALTIME_R */
#endif /* HAVE__LOCALTIME64_S */
  if (!success) {
    SERR("%s: failed to convert time_t to struct tm: <%" GRN_FMT_INT64D ">",
         function_name,
         (int64_t)time);
  }
  return success;
}

struct tm *
grn_timeval2tm(grn_ctx *ctx, grn_timeval *tv, struct tm *tm)
{
  if (grn_time_t_to_tm(ctx, tv->tv_sec, tm)) {
    return tm;
  } else {
    return NULL;
  }
}

grn_bool
grn_time_to_tm(grn_ctx *ctx, int64_t time, struct tm *tm)
{
  int64_t sec;
  int32_t usec;

  GRN_TIME_UNPACK(time, sec, usec);
  return grn_time_t_to_tm(ctx, sec, tm);
}

static grn_bool
grn_time_t_from_tm(grn_ctx *ctx, time_t *time, struct tm *tm)
{
  grn_bool success;

  tm->tm_yday = -1;
  *time = mktime(tm);
  success = (tm->tm_yday != -1);
  if (!success) {
    ERR(GRN_INVALID_ARGUMENT,
        "mktime: failed to convert struct tm to time_t: "
        "<%04d-%02d-%02dT%02d:%02d:%02d>(%d)",
        1900 + tm->tm_year,
        tm->tm_mon + 1,
        tm->tm_mday,
        tm->tm_hour,
        tm->tm_min,
        tm->tm_sec,
        tm->tm_isdst);
  }
  return success;
}

grn_bool
grn_time_from_tm(grn_ctx *ctx, int64_t *time, struct tm *tm)
{
  time_t sec_time_t;
  int64_t sec;
  int32_t usec = 0;

  if (!grn_time_t_from_tm(ctx, &sec_time_t, tm)) {
    return GRN_FALSE;
  }

  sec = sec_time_t;
  *time = GRN_TIME_PACK(sec, usec);
  return GRN_TRUE;
}

grn_rc
grn_timeval2str(grn_ctx *ctx, grn_timeval *tv, char *buf, size_t buf_size)
{
  struct tm tm;
  struct tm *ltm;
  ltm = grn_timeval2tm(ctx, tv, &tm);
  grn_snprintf(buf, buf_size, GRN_TIMEVAL_STR_SIZE,
               GRN_TIMEVAL_STR_FORMAT,
               ltm->tm_year + 1900, ltm->tm_mon + 1, ltm->tm_mday,
               ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
               (int)(GRN_TIME_NSEC_TO_USEC(tv->tv_nsec)));
  if (buf_size > GRN_TIMEVAL_STR_SIZE) {
    buf[GRN_TIMEVAL_STR_SIZE - 1] = '\0';
  } else {
    buf[buf_size - 1] = '\0';
  }
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
  if ((r1 + 1) >= rend || (*r1 != '/' && *r1 != '-')) {
    return GRN_INVALID_ARGUMENT;
  }
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
