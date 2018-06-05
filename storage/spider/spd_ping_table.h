/* Copyright (C) 2009-2017 Kentoku Shiba

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

SPIDER_TABLE_MON_LIST *spider_get_ping_table_mon_list(
  SPIDER_TRX *trx,
  THD *thd,
  spider_string *str,
  uint conv_name_length,
  int link_idx,
  char *static_link_id,
  uint static_link_id_length,
  uint32 server_id,
  bool need_lock,
  int *error_num
);

void spider_free_ping_table_mon_list(
  SPIDER_TABLE_MON_LIST *table_mon_list
);

void spider_release_ping_table_mon_list_loop(
  uint mutex_hash,
  SPIDER_TABLE_MON_LIST *table_mon_list
);

int spider_release_ping_table_mon_list(
  const char *conv_name,
  uint conv_name_length,
  int link_idx
);

int spider_get_ping_table_mon(
  THD *thd,
  SPIDER_TABLE_MON_LIST *table_mon_list,
  char *name,
  uint name_length,
  int link_idx,
  uint32 server_id,
  MEM_ROOT *mem_root,
  bool need_lock
);

SPIDER_TABLE_MON_LIST *spider_get_ping_table_tgt(
  THD *thd,
  char *name,
  uint name_length,
  int link_idx,
  char *static_link_id,
  uint static_link_id_length,
  uint32 server_id,
  spider_string *str,
  bool need_lock,
  int *error_num
);

SPIDER_CONN *spider_get_ping_table_tgt_conn(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  int *error_num
);

int spider_get_ping_table_gtid_pos(
  SPIDER_TRX *trx,
  THD *thd,
  spider_string *str,
  uint conv_name_length,
  int failed_link_idx,
  uint32 server_id,
  bool need_lock,
  spider_string *tmp_str
);

int spider_init_ping_table_mon_cache(
  THD *thd,
  MEM_ROOT *mem_root,
  bool need_lock
);

int spider_ping_table_cache_compare(
  TABLE *table,
  MEM_ROOT *mem_root
);

void spider_ping_table_free_mon_list(
  SPIDER_TABLE_MON_LIST *table_mon_list
);

void spider_ping_table_free_mon(
  SPIDER_TABLE_MON *table_mon
);

int spider_ping_table_mon_from_table(
  SPIDER_TRX *trx,
  THD *thd,
  SPIDER_SHARE *share,
  int base_link_idx,
  uint32 server_id,
  char *conv_name,
  uint conv_name_length,
  int link_idx,
  char *where_clause,
  uint where_clause_length,
  long monitoring_kind,
  longlong monitoring_limit,
  long monitoring_flag,
  bool need_lock
);
