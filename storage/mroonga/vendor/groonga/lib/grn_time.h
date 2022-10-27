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

#ifndef GRN_TIMEVAL_STR_SIZE
#define GRN_TIMEVAL_STR_SIZE 0x100
#endif /* GRN_TIMEVAL_STR_SIZE */
#ifndef GRN_TIMEVAL_STR_FORMAT
#define GRN_TIMEVAL_STR_FORMAT "%04d-%02d-%02d %02d:%02d:%02d.%06d"
#endif /* GRN_TIMEVAL_STR_FORMAT */

GRN_API grn_rc grn_timeval2str(grn_ctx *ctx, grn_timeval *tv, char *buf, size_t buf_size);
struct tm *grn_timeval2tm(grn_ctx *ctx, grn_timeval *tv, struct tm *tm_buffer);
grn_rc grn_str2timeval(const char *str, uint32_t str_len, grn_timeval *tv);

#ifdef __cplusplus
}
#endif
