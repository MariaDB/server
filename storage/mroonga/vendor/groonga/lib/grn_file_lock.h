/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2017 Brazil

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

typedef struct {
  const char *path;
#ifdef WIN32
  HANDLE handle;
#else /* WIN32 */
  int fd;
#endif /* WIN32 */
} grn_file_lock;

void grn_file_lock_init(grn_ctx *ctx,
                        grn_file_lock *file_lock,
                        const char *path);
grn_bool grn_file_lock_acquire(grn_ctx *ctx,
                               grn_file_lock *file_lock,
                               int timeout,
                               const char *error_message_tag);
void grn_file_lock_release(grn_ctx *ctx, grn_file_lock *file_lock);
void grn_file_lock_fin(grn_ctx *ctx, grn_file_lock *file_lock);

#ifdef __cplusplus
}
#endif
