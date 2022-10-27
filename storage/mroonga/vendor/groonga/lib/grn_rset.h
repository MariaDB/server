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

typedef struct {
  double score;
  int n_subrecs;
  int subrecs[1];
} grn_rset_recinfo;

typedef struct {
  grn_id rid;
  uint32_t sid;
  uint32_t pos;
} grn_rset_posinfo;

#define GRN_RSET_UTIL_BIT (0x80000000)

#define GRN_RSET_N_SUBRECS_SIZE (sizeof(int))
#define GRN_RSET_MAX_SIZE       (sizeof(int64_t))
#define GRN_RSET_MIN_SIZE       (sizeof(int64_t))
#define GRN_RSET_SUM_SIZE       (sizeof(int64_t))
#define GRN_RSET_AVG_SIZE       (sizeof(double))

#define GRN_RSET_SCORE_SIZE (sizeof(double))

#define GRN_RSET_N_SUBRECS(ri) ((ri)->n_subrecs & ~GRN_RSET_UTIL_BIT)

#define GRN_RSET_SUBREC_SIZE(subrec_size) \
  (GRN_RSET_SCORE_SIZE + subrec_size)
#define GRN_RSET_SUBRECS_CMP(a,b,dir) (((a) - (b))*(dir))
#define GRN_RSET_SUBRECS_NTH(subrecs,size,n) \
  ((double *)((byte *)subrecs + n * GRN_RSET_SUBREC_SIZE(size)))
#define GRN_RSET_SUBRECS_COPY(subrecs,size,n,src) \
  (grn_memcpy(GRN_RSET_SUBRECS_NTH(subrecs, size, n), src, GRN_RSET_SUBREC_SIZE(size)))
#define GRN_RSET_SUBRECS_SIZE(subrec_size,n) \
  (GRN_RSET_SUBREC_SIZE(subrec_size) * n)

uint32_t grn_rset_recinfo_calc_values_size(grn_ctx *ctx,
                                           grn_table_group_flags flags);
void grn_rset_recinfo_update_calc_values(grn_ctx *ctx,
                                         grn_rset_recinfo *ri,
                                         grn_obj *table,
                                         grn_obj *value);

int64_t *grn_rset_recinfo_get_max_(grn_ctx *ctx,
                                   grn_rset_recinfo *ri,
                                   grn_obj *table);
int64_t grn_rset_recinfo_get_max(grn_ctx *ctx,
                                 grn_rset_recinfo *ri,
                                 grn_obj *table);
void grn_rset_recinfo_set_max(grn_ctx *ctx,
                              grn_rset_recinfo *ri,
                              grn_obj *table,
                              int64_t max);

int64_t *grn_rset_recinfo_get_min_(grn_ctx *ctx,
                                   grn_rset_recinfo *ri,
                                   grn_obj *table);
int64_t grn_rset_recinfo_get_min(grn_ctx *ctx,
                                 grn_rset_recinfo *ri,
                                 grn_obj *table);
void grn_rset_recinfo_set_min(grn_ctx *ctx,
                              grn_rset_recinfo *ri,
                              grn_obj *table,
                              int64_t min);

int64_t *grn_rset_recinfo_get_sum_(grn_ctx *ctx,
                                   grn_rset_recinfo *ri,
                                   grn_obj *table);
int64_t grn_rset_recinfo_get_sum(grn_ctx *ctx,
                                 grn_rset_recinfo *ri,
                                 grn_obj *table);
void grn_rset_recinfo_set_sum(grn_ctx *ctx,
                              grn_rset_recinfo *ri,
                              grn_obj *table,
                              int64_t sum);

double *grn_rset_recinfo_get_avg_(grn_ctx *ctx,
                                  grn_rset_recinfo *ri,
                                  grn_obj *table);
double grn_rset_recinfo_get_avg(grn_ctx *ctx,
                                grn_rset_recinfo *ri,
                                grn_obj *table);
void grn_rset_recinfo_set_avg(grn_ctx *ctx,
                              grn_rset_recinfo *ri,
                              grn_obj *table,
                              double avg);

#ifdef __cplusplus
}
#endif
