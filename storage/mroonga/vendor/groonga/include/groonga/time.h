/*
  Copyright(C) 2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#include <time.h>

#ifdef  __cplusplus
extern "C" {
#endif

#define GRN_TIMEVAL_TO_MSEC(timeval)                    \
  (((timeval)->tv_sec * GRN_TIME_MSEC_PER_SEC) +        \
   ((timeval)->tv_nsec / GRN_TIME_NSEC_PER_MSEC))

#define GRN_TIME_NSEC_PER_SEC 1000000000
#define GRN_TIME_NSEC_PER_SEC_F 1000000000.0
#define GRN_TIME_NSEC_PER_MSEC 1000000
#define GRN_TIME_NSEC_PER_USEC (GRN_TIME_NSEC_PER_SEC / GRN_TIME_USEC_PER_SEC)
#define GRN_TIME_MSEC_PER_SEC 1000
#define GRN_TIME_NSEC_TO_USEC(nsec) ((nsec) / GRN_TIME_NSEC_PER_USEC)
#define GRN_TIME_USEC_TO_NSEC(usec) ((usec) * GRN_TIME_NSEC_PER_USEC)

#define GRN_TIME_USEC_PER_SEC 1000000
#define GRN_TIME_PACK(sec, usec) ((int64_t)(sec) * GRN_TIME_USEC_PER_SEC + (usec))
#define GRN_TIME_UNPACK(time_value, sec, usec) do {\
  sec = (time_value) / GRN_TIME_USEC_PER_SEC;\
  usec = (time_value) % GRN_TIME_USEC_PER_SEC;\
} while (0)

GRN_API grn_rc grn_timeval_now(grn_ctx *ctx, grn_timeval *tv);

GRN_API void grn_time_now(grn_ctx *ctx, grn_obj *obj);

#define GRN_TIME_NOW(ctx,obj) (grn_time_now((ctx), (obj)))

GRN_API grn_bool grn_time_to_tm(grn_ctx *ctx,
                                int64_t time,
                                struct tm *tm);
GRN_API grn_bool grn_time_from_tm(grn_ctx *ctx,
                                  int64_t *time,
                                  struct tm *tm);

#ifdef __cplusplus
}
#endif
