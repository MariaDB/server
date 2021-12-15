/* Copyright (C) 2012-2014 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define spider_free(A,B,C) spider_free_mem(A,B,C)
#define spider_malloc(A,B,C,D) \
  spider_alloc_mem(A,B,__func__,__FILE__,__LINE__,C,D)
#define spider_bulk_malloc(A,B,C,...) \
  spider_bulk_alloc_mem(A,B,__func__,__FILE__,__LINE__,C,__VA_ARGS__)
#define spider_current_trx \
  (current_thd ? ((SPIDER_TRX *) thd_get_ha_data(current_thd, spider_hton_ptr)) : NULL)

#define init_calc_mem(A) init_mem_calc(A,__func__,__FILE__,__LINE__)

#define SPIDER_CALC_MEM_ID(name) name ## _id
#define SPIDER_CALC_MEM_FUNC(name) name ## _func_name
#define SPIDER_CALC_MEM_FILE(name) name ## _file_name
#define SPIDER_CALC_MEM_LINE(name) name ## _line_no
#define spider_alloc_calc_mem_init(A,B) \
  {SPIDER_CALC_MEM_ID(A) = B; SPIDER_CALC_MEM_FUNC(A) = __func__; SPIDER_CALC_MEM_FILE(A) = __FILE__; SPIDER_CALC_MEM_LINE(A) = __LINE__;}
#define spider_alloc_calc_mem(A,B,C) \
  spider_alloc_mem_calc(A,SPIDER_CALC_MEM_ID(B),SPIDER_CALC_MEM_FUNC(B),SPIDER_CALC_MEM_FILE(B),SPIDER_CALC_MEM_LINE(B),C)

void spider_merge_mem_calc(
  SPIDER_TRX *trx,
  bool force
);

void spider_free_mem_calc(
  SPIDER_TRX *trx,
  uint id,
  size_t size
);

void spider_alloc_mem_calc(
  SPIDER_TRX *trx,
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no,
  size_t size
);

void spider_free_mem(
  SPIDER_TRX *trx,
  void *ptr,
  myf my_flags
);

void *spider_alloc_mem(
  SPIDER_TRX *trx,
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no,
  size_t size,
  myf my_flags
);

void *spider_bulk_alloc_mem(
  SPIDER_TRX *trx,
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no,
  myf my_flags,
  ...
);
