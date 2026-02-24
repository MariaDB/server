/* Copyright (C) 2009-2014 Kentoku Shiba

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

uint spider_udf_calc_hash(
  char *key,
  uint mod
);

int spider_udf_direct_sql_create_table_list(
  SPIDER_DIRECT_SQL *direct_sql,
  char *table_name_list,
  uint table_name_list_length
);

int spider_udf_direct_sql_create_conn_key(
  SPIDER_DIRECT_SQL *direct_sql
);

SPIDER_CONN *spider_udf_direct_sql_create_conn(
  const SPIDER_DIRECT_SQL *direct_sql,
  int *error_num
);

SPIDER_CONN *spider_udf_direct_sql_get_conn(
  const SPIDER_DIRECT_SQL *direct_sql,
  SPIDER_TRX *trx,
  int *error_num
);

int spider_udf_direct_sql_get_server(
  SPIDER_DIRECT_SQL *direct_sql
);

int spider_udf_parse_direct_sql_param(
  SPIDER_TRX *trx,
  SPIDER_DIRECT_SQL *direct_sql,
  const char *param,
  int param_length
);

int spider_udf_set_direct_sql_param_default(
  SPIDER_TRX *trx,
  SPIDER_DIRECT_SQL *direct_sql
);

void spider_udf_free_direct_sql_alloc(
  SPIDER_DIRECT_SQL *direct_sql,
  my_bool bg
);

int spider_udf_bg_direct_sql(
  SPIDER_DIRECT_SQL *direct_sql
);
