/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#ifdef  __cplusplus
extern "C" {
#endif

GRN_API uint32_t grn_thread_get_limit(void);
GRN_API void grn_thread_set_limit(uint32_t new_limit);


typedef uint32_t (*grn_thread_get_limit_func)(void *data);
GRN_API void grn_thread_set_get_limit_func(grn_thread_get_limit_func func,
                                           void *data);
typedef void (*grn_thread_set_limit_func)(uint32_t new_limit, void *data);
GRN_API void grn_thread_set_set_limit_func(grn_thread_set_limit_func func,
                                           void *data);

#ifdef __cplusplus
}
#endif
