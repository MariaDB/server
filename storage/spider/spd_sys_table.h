/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019 MariaDB corp

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

#define SPIDER_SYS_XA_TABLE_NAME_STR "spider_xa"
#define SPIDER_SYS_XA_TABLE_NAME_LEN 9
#define SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR "spider_xa_member"
#define SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN 16
#define SPIDER_SYS_TABLES_TABLE_NAME_STR "spider_tables"
#define SPIDER_SYS_TABLES_TABLE_NAME_LEN 13
#define SPIDER_SYS_LINK_MON_TABLE_NAME_STR "spider_link_mon_servers"
#define SPIDER_SYS_LINK_MON_TABLE_NAME_LEN 23
#define SPIDER_SYS_LINK_FAILED_TABLE_NAME_STR "spider_link_failed_log"
#define SPIDER_SYS_LINK_FAILED_TABLE_NAME_LEN 22
#define SPIDER_SYS_XA_FAILED_TABLE_NAME_STR "spider_xa_failed_log"
#define SPIDER_SYS_XA_FAILED_TABLE_NAME_LEN 20
#define SPIDER_SYS_POS_FOR_RECOVERY_TABLE_NAME_STR "spider_table_position_for_recovery"
#define SPIDER_SYS_POS_FOR_RECOVERY_TABLE_NAME_LEN 34
#define SPIDER_SYS_TABLE_STS_TABLE_NAME_STR "spider_table_sts"
#define SPIDER_SYS_TABLE_STS_TABLE_NAME_LEN 16
#define SPIDER_SYS_TABLE_CRD_TABLE_NAME_STR "spider_table_crd"
#define SPIDER_SYS_TABLE_CRD_TABLE_NAME_LEN 16
#define SPIDER_SYS_RW_TBLS_TABLE_NAME_STR "spider_rewrite_tables"
#define SPIDER_SYS_RW_TBLS_TABLE_NAME_LEN 21
#define SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_STR "spider_rewrite_table_tables"
#define SPIDER_SYS_RW_TBL_TBLS_TABLE_NAME_LEN 27
#define SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_STR "spider_rewrite_table_partitions"
#define SPIDER_SYS_RW_TBL_PTTS_TABLE_NAME_LEN 31
#define SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_STR "spider_rewrite_table_subpartitions"
#define SPIDER_SYS_RW_TBL_SPTTS_TABLE_NAME_LEN 34
#define SPIDER_SYS_RWN_TBLS_TABLE_NAME_STR "spider_rewritten_tables"
#define SPIDER_SYS_RWN_TBLS_TABLE_NAME_LEN 23

#define SPIDER_SYS_XA_PREPARED_STR "PREPARED"
#define SPIDER_SYS_XA_NOT_YET_STR "NOT YET"
#define SPIDER_SYS_XA_COMMIT_STR "COMMIT"
#define SPIDER_SYS_XA_ROLLBACK_STR "ROLLBACK"

#define SPIDER_SYS_XA_COL_CNT 5
#define SPIDER_SYS_XA_PK_COL_CNT 3
#define SPIDER_SYS_XA_IDX1_COL_CNT 1
#define SPIDER_SYS_XA_MEMBER_COL_CNT 21
#define SPIDER_SYS_XA_MEMBER_PK_COL_CNT 6
#define SPIDER_SYS_TABLES_COL_CNT 28
#define SPIDER_SYS_TABLES_PK_COL_CNT 3
#define SPIDER_SYS_TABLES_IDX1_COL_CNT 1
#define SPIDER_SYS_TABLES_UIDX1_COL_CNT 3
#define SPIDER_SYS_LINK_MON_TABLE_COL_CNT 22
#define SPIDER_SYS_LINK_FAILED_TABLE_COL_CNT 4
#define SPIDER_SYS_XA_FAILED_TABLE_COL_CNT 24
#define SPIDER_SYS_POS_FOR_RECOVERY_TABLE_COL_CNT 7
#define SPIDER_SYS_TABLE_STS_COL_CNT 11
#define SPIDER_SYS_TABLE_STS_PK_COL_CNT 2
#define SPIDER_SYS_TABLE_CRD_COL_CNT 4
#define SPIDER_SYS_TABLE_CRD_PK_COL_CNT 3
#define SPIDER_SYS_RW_TBLS_COL_CNT 3
#define SPIDER_SYS_RW_TBL_TBLS_COL_CNT 8
#define SPIDER_SYS_RW_TBL_PTTS_COL_CNT 7
#define SPIDER_SYS_RW_TBL_SPTTS_COL_CNT 8
#define SPIDER_SYS_RWN_TBLS_COL_CNT 4

#define SPIDER_SYS_LINK_MON_TABLE_DB_NAME_SIZE 64
#define SPIDER_SYS_LINK_MON_TABLE_TABLE_NAME_SIZE 64
#define SPIDER_SYS_LINK_MON_TABLE_LINK_ID_SIZE 64

class SPIDER_MON_KEY: public SPIDER_SORT
{
public:
  char db_name[SPIDER_SYS_LINK_MON_TABLE_DB_NAME_SIZE + 1];
  char table_name[SPIDER_SYS_LINK_MON_TABLE_TABLE_NAME_SIZE + 1];
  char link_id[SPIDER_SYS_LINK_MON_TABLE_LINK_ID_SIZE + 1];
  uint db_name_length;
  uint table_name_length;
  uint link_id_length;
};

TABLE *spider_open_sys_table(
  THD *thd,
  const char *table_name,
  int table_name_length,
  bool write,
  SPIDER_Open_tables_backup *open_tables_backup,
  bool need_lock,
  int *error_num
);

void spider_close_sys_table(
  THD *thd,
  TABLE *table,
  SPIDER_Open_tables_backup *open_tables_backup,
  bool need_lock
);

bool spider_sys_open_and_lock_tables(
  THD *thd,
  TABLE_LIST **tables,
  SPIDER_Open_tables_backup *open_tables_backup
);

TABLE *spider_sys_open_table(
  THD *thd,
  TABLE_LIST *tables,
  SPIDER_Open_tables_backup *open_tables_backup
);

void spider_sys_close_table(
  THD *thd,
  SPIDER_Open_tables_backup *open_tables_backup
);

int spider_sys_index_init(
  TABLE *table,
  uint idx,
  bool sorted
);

int spider_sys_index_end(
  TABLE *table
);

int spider_sys_rnd_init(
  TABLE *table,
  bool scan
);

int spider_sys_rnd_end(
  TABLE *table
);

int spider_check_sys_table(
  TABLE *table,
  char *table_key
);

int spider_check_sys_table_with_find_flag(
  TABLE *table,
  char *table_key,
  enum ha_rkey_function find_flag
);

int spider_check_sys_table_for_update_all_columns(
  TABLE *table,
  char *table_key
);

int spider_get_sys_table_by_idx(
  TABLE *table,
  char *table_key,
  const int idx,
  const int col_count
);

int spider_sys_index_next_same(
  TABLE *table,
  char *table_key
);

int spider_sys_index_first(
  TABLE *table,
  const int idx
);

int spider_sys_index_last(
  TABLE *table,
  const int idx
);

int spider_sys_index_next(
  TABLE *table
);

void spider_store_xa_pk(
  TABLE *table,
  XID *xid
);

void spider_store_xa_bqual_length(
  TABLE *table,
  XID *xid
);

void spider_store_xa_status(
  TABLE *table,
  const char *status
);

void spider_store_xa_member_pk(
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn
);

void spider_store_xa_member_info(
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn
);

void spider_store_tables_name(
  TABLE *table,
  const char *name,
  const uint name_length
);

void spider_store_db_and_table_name(
  TABLE *table,
  const char *db_name,
  const uint db_name_length,
  const char *table_name,
  const uint table_name_length
);

void spider_store_tables_link_idx(
  TABLE *table,
  int link_idx
);

void spider_store_tables_link_idx_str(
  TABLE *table,
  const char *link_idx,
  const uint link_idx_length
);

void spider_store_tables_static_link_id(
  TABLE *table,
  const char *static_link_id,
  const uint static_link_id_length
);

void spider_store_tables_priority(
  TABLE *table,
  longlong priority
);

void spider_store_tables_connect_info(
  TABLE *table,
  SPIDER_ALTER_TABLE *alter_table,
  int link_idx
);

void spider_store_tables_link_status(
  TABLE *table,
  long link_status
);

void spider_store_binlog_pos_failed_link_idx(
  TABLE *table,
  int failed_link_idx
);

void spider_store_binlog_pos_source_link_idx(
  TABLE *table,
  int source_link_idx
);

void spider_store_binlog_pos_binlog_file(
  TABLE *table,
  const char *file_name,
  int file_name_length,
  const char *position,
  int position_length,
  CHARSET_INFO *binlog_pos_cs
);

void spider_store_binlog_pos_gtid(
  TABLE *table,
  const char *gtid,
  int gtid_length,
  CHARSET_INFO *binlog_pos_cs
);

void spider_store_table_sts_info(
  TABLE *table,
  ha_statistics *stat
);

void spider_store_table_crd_info(
  TABLE *table,
  uint *seq,
  longlong *cardinality
);

int spider_insert_xa(
  TABLE *table,
  XID *xid,
  const char *status
);

int spider_insert_xa_member(
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn
);

int spider_insert_tables(
  TABLE *table,
  SPIDER_SHARE *share
);

int spider_insert_sys_table(
  TABLE *table
);

int spider_insert_or_update_table_sts(
  TABLE *table,
  const char *name,
  uint name_length,
  ha_statistics *stat
);

int spider_insert_or_update_table_crd(
  TABLE *table,
  const char *name,
  uint name_length,
  longlong *cardinality,
  uint number_of_keys
);

int spider_log_tables_link_failed(
  TABLE *table,
  char *name,
  uint name_length,
  int link_idx
);

int spider_log_xa_failed(
  THD *thd,
  TABLE *table,
  XID *xid,
  SPIDER_CONN *conn,
  const char *status
);

int spider_update_xa(
  TABLE *table,
  XID *xid,
  const char *status
);

int spider_update_tables_name(
  TABLE *table,
  const char *from,
  const char *to,
  int *old_link_count
);

int spider_update_tables_priority(
  TABLE *table,
  SPIDER_ALTER_TABLE *alter_table,
  const char *name,
  int *old_link_count
);

int spider_update_tables_link_status(
  TABLE *table,
  char *name,
  uint name_length,
  int link_idx,
  long link_status
);

int spider_update_sys_table(
  TABLE *table
);

int spider_delete_xa(
  TABLE *table,
  XID *xid
);

int spider_delete_xa_member(
  TABLE *table,
  XID *xid
);

int spider_delete_tables(
  TABLE *table,
  const char *name,
  int *old_link_count
);

int spider_delete_table_sts(
  TABLE *table,
  const char *name,
  uint name_length
);

int spider_delete_table_crd(
  TABLE *table,
  const char *name,
  uint name_length
);

int spider_get_sys_xid(
  TABLE *table,
  XID *xid,
  MEM_ROOT *mem_root
);

int spider_get_sys_server_info(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
);

int spider_check_sys_xa_status(
  TABLE *table,
  const char *status1,
  const char *status2,
  const char *status3,
  const int check_error_num,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables(
  TABLE *table,
  char **db_name,
  char **table_name,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_connect_info(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_monitoring_binlog_pos_at_failing(
  TABLE *table,
  long *monitoring_binlog_pos_at_failing,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_link_status(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_link_status(
  TABLE *table,
  long *link_status,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_link_idx(
  TABLE *table,
  int *link_idx,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_static_link_id(
  TABLE *table,
  char **static_link_id,
  uint *static_link_id_length,
  MEM_ROOT *mem_root
);

void spider_get_sys_table_sts_info(
  TABLE *table,
  ha_statistics *stat
);

void spider_get_sys_table_crd_info(
  TABLE *table,
  longlong *cardinality,
  uint number_of_keys
);

int spider_sys_update_tables_link_status(
  THD *thd,
  char *name,
  uint name_length,
  int link_idx,
  long link_status,
  bool need_lock
);

int spider_sys_log_tables_link_failed(
  THD *thd,
  char *name,
  uint name_length,
  int link_idx,
  bool need_lock
);

int spider_sys_log_xa_failed(
  THD *thd,
  XID *xid,
  SPIDER_CONN *conn,
  const char *status,
  bool need_lock
);

int spider_get_sys_link_mon_key(
  TABLE *table,
  SPIDER_MON_KEY *mon_key,
  MEM_ROOT *mem_root,
  int *same
);

int spider_get_sys_link_mon_server_id(
  TABLE *table,
  uint32 *server_id,
  MEM_ROOT *mem_root
);

int spider_get_sys_link_mon_connect_info(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
);

int spider_get_link_statuses(
  TABLE *table,
  SPIDER_SHARE *share,
  MEM_ROOT *mem_root
);

int spider_sys_insert_or_update_table_sts(
  THD *thd,
  const char *name,
  uint name_length,
  ha_statistics *stat,
  bool need_lock
);

int spider_sys_insert_or_update_table_crd(
  THD *thd,
  const char *name,
  uint name_length,
  longlong *cardinality,
  uint number_of_keys,
  bool need_lock
);

int spider_sys_delete_table_sts(
  THD *thd,
  const char *name,
  uint name_length,
  bool need_lock
);

int spider_sys_delete_table_crd(
  THD *thd,
  const char *name,
  uint name_length,
  bool need_lock
);

int spider_sys_get_table_sts(
  THD *thd,
  const char *name,
  uint name_length,
  ha_statistics *stat,
  bool need_lock
);

int spider_sys_get_table_crd(
  THD *thd,
  const char *name,
  uint name_length,
  longlong *cardinality,
  uint number_of_keys,
  bool need_lock
);

int spider_sys_replace(
  TABLE *table,
  bool *modified_non_trans_table
);

#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
TABLE *spider_mk_sys_tmp_table(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const LEX_CSTRING *field_name,
  CHARSET_INFO *cs
);
#else
TABLE *spider_mk_sys_tmp_table(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name,
  CHARSET_INFO *cs
);
#endif

void spider_rm_sys_tmp_table(
  THD *thd,
  TABLE *tmp_table,
  TMP_TABLE_PARAM *tmp_tbl_prm
);

#ifdef SPIDER_use_LEX_CSTRING_for_Field_blob_constructor
TABLE *spider_mk_sys_tmp_table_for_result(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const LEX_CSTRING *field_name1,
  const LEX_CSTRING *field_name2,
  const LEX_CSTRING *field_name3,
  CHARSET_INFO *cs
);
#else
TABLE *spider_mk_sys_tmp_table_for_result(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name1,
  const char *field_name2,
  const char *field_name3,
  CHARSET_INFO *cs
);
#endif

void spider_rm_sys_tmp_table_for_result(
  THD *thd,
  TABLE *tmp_table,
  TMP_TABLE_PARAM *tmp_tbl_prm
);

TABLE *spider_find_temporary_table(
  THD *thd,
  TABLE_LIST *table_list
);
