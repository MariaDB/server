/* Copyright (C) 2008-2014 Kentoku Shiba

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

int spider_free_trx_conn(
  SPIDER_TRX *trx,
  bool trx_free
);

int spider_free_trx_another_conn(
  SPIDER_TRX *trx,
  bool lock
);

int spider_trx_another_lock_tables(
  SPIDER_TRX *trx
);

int spider_trx_another_flush_tables(
  SPIDER_TRX *trx
);

int spider_trx_all_flush_tables(
  SPIDER_TRX *trx
);

int spider_trx_all_unlock_tables(
  SPIDER_TRX *trx
);

int spider_trx_all_start_trx(
  SPIDER_TRX *trx
);

int spider_trx_all_flush_logs(
  SPIDER_TRX *trx
);

int spider_free_trx_alloc(
  SPIDER_TRX *trx
);

void spider_free_trx_alter_table_alloc(
  SPIDER_TRX *trx,
  SPIDER_ALTER_TABLE *alter_table
);

int spider_free_trx_alter_table(
  SPIDER_TRX *trx
);

int spider_create_trx_alter_table(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  bool now_create
);

bool spider_cmp_trx_alter_table(
  SPIDER_ALTER_TABLE *cmp1,
  SPIDER_ALTER_TABLE *cmp2
);

SPIDER_TRX *spider_get_trx(
  THD *thd,
  bool regist_allocated_thds,
  int *error_num
);

int spider_free_trx(
  SPIDER_TRX *trx,
  bool need_lock
);

int spider_check_and_set_trx_isolation(
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_check_and_set_autocommit(
  THD *thd,
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_check_and_set_sql_log_off(
  THD *thd,
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_check_and_set_time_zone(
  THD *thd,
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_xa_lock(
  XID_STATE *xid_state
);

int spider_xa_unlock(
  XID_STATE *xid_state
);

int spider_start_internal_consistent_snapshot(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  int *need_mon
);

int spider_internal_start_trx(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
);

int spider_internal_xa_commit(
  THD* thd,
  SPIDER_TRX *trx,
  XID* xid,
  TABLE *table_xa,
  TABLE *table_xa_member
);

int spider_internal_xa_rollback(
  THD* thd,
  SPIDER_TRX *trx
);

int spider_internal_xa_prepare(
  THD* thd,
  SPIDER_TRX *trx,
  TABLE *table_xa,
  TABLE *table_xa_member,
  bool internal_xa
);

int spider_internal_xa_recover(
  THD* thd,
  XID* xid_list,
  uint len
);

int spider_initinal_xa_recover(
  XID* xid_list,
  uint len
);

int spider_internal_xa_commit_by_xid(
  THD* thd,
  SPIDER_TRX *trx,
  XID* xid
);

int spider_internal_xa_rollback_by_xid(
  THD* thd,
  SPIDER_TRX *trx,
  XID* xid
);

int spider_start_consistent_snapshot(
  handlerton *hton,
  THD* thd
);

int spider_commit(
  handlerton *hton,
  THD *thd,
  bool all
);

int spider_rollback(
  handlerton *hton,
  THD *thd,
  bool all
);

int spider_xa_prepare(
  handlerton *hton,
  THD* thd,
  bool all
);

int spider_xa_recover(
  handlerton *hton,
  XID* xid_list,
  uint len
);

int spider_xa_commit_by_xid(
  handlerton *hton,
  XID* xid
);

int spider_xa_rollback_by_xid(
  handlerton *hton,
  XID* xid
);

void spider_copy_table_free_trx_conn(
  SPIDER_TRX *trx
);

int spider_end_trx(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn
);

int spider_check_trx_and_get_conn(
  THD *thd,
  ha_spider *spider,
  bool use_conn_kind
);

THD *spider_create_tmp_thd();

void spider_free_tmp_thd(
  THD *thd
);

int spider_create_trx_ha(
  SPIDER_TRX *trx,
  ha_spider *spider,
  SPIDER_TRX_HA *trx_ha
);

SPIDER_TRX_HA *spider_check_trx_ha(
  SPIDER_TRX *trx,
  ha_spider *spider
);

void spider_free_trx_ha(
  SPIDER_TRX *trx
);

void spider_reuse_trx_ha(
  SPIDER_TRX *trx
);

void spider_trx_set_link_idx_for_all(
  ha_spider *spider
);

int spider_trx_check_link_idx_failed(
  ha_spider *spider
);

#ifdef HA_CAN_BULK_ACCESS
void spider_trx_add_bulk_access_conn(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn
);
#endif
