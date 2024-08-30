/* Copyright (C) 2010-2014 Kentoku Shiba

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

int spider_udf_set_copy_tables_param_default(
  SPIDER_COPY_TABLES *copy_tables
);

int spider_udf_parse_copy_tables_param(
  SPIDER_COPY_TABLES *copy_tables,
  char *param,
  int param_length
);

int spider_udf_get_copy_tgt_tables(
  THD *thd,
  SPIDER_COPY_TABLES *copy_tables,
  MEM_ROOT *mem_root,
  bool need_lock
);

int spider_udf_get_copy_tgt_conns(
  SPIDER_COPY_TABLES *copy_tables
);

void spider_udf_free_copy_tables_alloc(
  SPIDER_COPY_TABLES *copy_tables
);

int spider_udf_copy_tables_create_table_list(
  SPIDER_COPY_TABLES *copy_tables,
  char *spider_table_name,
  uint spider_table_name_length,
  char *src_link_idx_list,
  uint src_link_idx_list_length,
  char *dst_link_idx_list,
  uint dst_link_idx_list_length
);

int spider_udf_bg_copy_exec_sql(
  SPIDER_COPY_TABLE_CONN *table_conn
);
