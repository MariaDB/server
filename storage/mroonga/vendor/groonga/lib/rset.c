/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2015 Brazil

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
#include "grn_db.h"

uint32_t
grn_rset_recinfo_calc_values_size(grn_ctx *ctx, grn_table_group_flags flags)
{
  uint32_t size = 0;

  if (flags & GRN_TABLE_GROUP_CALC_MAX) {
    size += GRN_RSET_MAX_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_MIN) {
    size += GRN_RSET_MIN_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_SUM) {
    size += GRN_RSET_SUM_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_AVG) {
    size += GRN_RSET_AVG_SIZE;
  }

  return size;
}

void
grn_rset_recinfo_update_calc_values(grn_ctx *ctx,
                                    grn_rset_recinfo *ri,
                                    grn_obj *table,
                                    grn_obj *value)
{
  grn_table_group_flags flags;
  byte *values;
  grn_obj value_int64;
  grn_obj value_float;

  flags = DB_OBJ(table)->flags.group;

  values = (((byte *)ri->subrecs) +
            GRN_RSET_SUBRECS_SIZE(DB_OBJ(table)->subrec_size,
                                  DB_OBJ(table)->max_n_subrecs));

  GRN_INT64_INIT(&value_int64, 0);
  GRN_FLOAT_INIT(&value_float, 0);

  if (flags & (GRN_TABLE_GROUP_CALC_MAX |
               GRN_TABLE_GROUP_CALC_MIN |
               GRN_TABLE_GROUP_CALC_SUM)) {
    grn_obj_cast(ctx, value, &value_int64, GRN_FALSE);
  }
  if (flags & GRN_TABLE_GROUP_CALC_AVG) {
    grn_obj_cast(ctx, value, &value_float, GRN_FALSE);
  }

  if (flags & GRN_TABLE_GROUP_CALC_MAX) {
    int64_t current_max = *((int64_t *)values);
    int64_t value_raw = GRN_INT64_VALUE(&value_int64);
    if (ri->n_subrecs == 1 || value_raw > current_max) {
      *((int64_t *)values) = value_raw;
    }
    values += GRN_RSET_MAX_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_MIN) {
    int64_t current_min = *((int64_t *)values);
    int64_t value_raw = GRN_INT64_VALUE(&value_int64);
    if (ri->n_subrecs == 1 || value_raw < current_min) {
      *((int64_t *)values) = value_raw;
    }
    values += GRN_RSET_MIN_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_SUM) {
    int64_t value_raw = GRN_INT64_VALUE(&value_int64);
    *((int64_t *)values) += value_raw;
    values += GRN_RSET_SUM_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_AVG) {
    double current_average = *((double *)values);
    double value_raw = GRN_FLOAT_VALUE(&value_float);
    *((double *)values) += (value_raw - current_average) / ri->n_subrecs;
    values += GRN_RSET_AVG_SIZE;
  }

  GRN_OBJ_FIN(ctx, &value_float);
  GRN_OBJ_FIN(ctx, &value_int64);
}

int64_t *
grn_rset_recinfo_get_max_(grn_ctx *ctx,
                          grn_rset_recinfo *ri,
                          grn_obj *table)
{
  grn_table_group_flags flags;
  byte *values;

  flags = DB_OBJ(table)->flags.group;
  if (!(flags & GRN_TABLE_GROUP_CALC_MAX)) {
    return NULL;
  }

  values = (((byte *)ri->subrecs) +
            GRN_RSET_SUBRECS_SIZE(DB_OBJ(table)->subrec_size,
                                  DB_OBJ(table)->max_n_subrecs));

  return (int64_t *)values;
}

int64_t
grn_rset_recinfo_get_max(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table)
{
  int64_t *max_address;

  max_address = grn_rset_recinfo_get_max_(ctx, ri, table);
  if (max_address) {
    return *max_address;
  } else {
    return 0;
  }
}

void
grn_rset_recinfo_set_max(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table,
                         int64_t max)
{
  int64_t *max_address;

  max_address = grn_rset_recinfo_get_max_(ctx, ri, table);
  if (!max_address) {
    return;
  }

  *max_address = max;
}

int64_t *
grn_rset_recinfo_get_min_(grn_ctx *ctx,
                          grn_rset_recinfo *ri,
                          grn_obj *table)
{
  grn_table_group_flags flags;
  byte *values;

  flags = DB_OBJ(table)->flags.group;
  if (!(flags & GRN_TABLE_GROUP_CALC_MIN)) {
    return NULL;
  }

  values = (((byte *)ri->subrecs) +
            GRN_RSET_SUBRECS_SIZE(DB_OBJ(table)->subrec_size,
                                  DB_OBJ(table)->max_n_subrecs));

  if (flags & GRN_TABLE_GROUP_CALC_MAX) {
    values += GRN_RSET_MAX_SIZE;
  }

  return (int64_t *)values;
}

int64_t
grn_rset_recinfo_get_min(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table)
{
  int64_t *min_address;

  min_address = grn_rset_recinfo_get_min_(ctx, ri, table);
  if (min_address) {
    return *min_address;
  } else {
    return 0;
  }
}

void
grn_rset_recinfo_set_min(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table,
                         int64_t min)
{
  int64_t *min_address;

  min_address = grn_rset_recinfo_get_min_(ctx, ri, table);
  if (!min_address) {
    return;
  }

  *min_address = min;
}

int64_t *
grn_rset_recinfo_get_sum_(grn_ctx *ctx,
                          grn_rset_recinfo *ri,
                          grn_obj *table)
{
  grn_table_group_flags flags;
  byte *values;

  flags = DB_OBJ(table)->flags.group;
  if (!(flags & GRN_TABLE_GROUP_CALC_SUM)) {
    return NULL;
  }

  values = (((byte *)ri->subrecs) +
            GRN_RSET_SUBRECS_SIZE(DB_OBJ(table)->subrec_size,
                                  DB_OBJ(table)->max_n_subrecs));

  if (flags & GRN_TABLE_GROUP_CALC_MAX) {
    values += GRN_RSET_MAX_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_MIN) {
    values += GRN_RSET_MIN_SIZE;
  }

  return (int64_t *)values;
}

int64_t
grn_rset_recinfo_get_sum(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table)
{
  int64_t *sum_address;

  sum_address = grn_rset_recinfo_get_sum_(ctx, ri, table);
  if (sum_address) {
    return *sum_address;
  } else {
    return 0;
  }
}

void
grn_rset_recinfo_set_sum(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table,
                         int64_t sum)
{
  int64_t *sum_address;

  sum_address = grn_rset_recinfo_get_sum_(ctx, ri, table);
  if (!sum_address) {
    return;
  }

  *sum_address = sum;
}

double *
grn_rset_recinfo_get_avg_(grn_ctx *ctx,
                          grn_rset_recinfo *ri,
                          grn_obj *table)
{
  grn_table_group_flags flags;
  byte *values;

  flags = DB_OBJ(table)->flags.group;
  if (!(flags & GRN_TABLE_GROUP_CALC_AVG)) {
    return NULL;
  }

  values = (((byte *)ri->subrecs) +
            GRN_RSET_SUBRECS_SIZE(DB_OBJ(table)->subrec_size,
                                  DB_OBJ(table)->max_n_subrecs));

  if (flags & GRN_TABLE_GROUP_CALC_MAX) {
    values += GRN_RSET_MAX_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_MIN) {
    values += GRN_RSET_MIN_SIZE;
  }
  if (flags & GRN_TABLE_GROUP_CALC_SUM) {
    values += GRN_RSET_SUM_SIZE;
  }

  return (double *)values;
}

double
grn_rset_recinfo_get_avg(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table)
{
  double *avg_address;

  avg_address = grn_rset_recinfo_get_avg_(ctx, ri, table);
  if (avg_address) {
    return *avg_address;
  } else {
    return 0;
  }
}

void
grn_rset_recinfo_set_avg(grn_ctx *ctx,
                         grn_rset_recinfo *ri,
                         grn_obj *table,
                         double avg)
{
  double *avg_address;

  avg_address = grn_rset_recinfo_get_avg_(ctx, ri, table);
  if (!avg_address) {
    return;
  }

  *avg_address = avg;
}
