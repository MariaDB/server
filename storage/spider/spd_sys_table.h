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

#define SPIDER_SYS_XA_TABLE_NAME_STR "spider_xa"
#define SPIDER_SYS_XA_TABLE_NAME_LEN (sizeof(SPIDER_SYS_XA_TABLE_NAME_STR) - 1)
#define SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR "spider_xa_member"
#define SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN (sizeof(SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR) - 1)
#define SPIDER_SYS_TABLES_TABLE_NAME_STR "spider_tables"
#define SPIDER_SYS_TABLES_TABLE_NAME_LEN (sizeof(SPIDER_SYS_TABLES_TABLE_NAME_STR) - 1)
#define SPIDER_SYS_LINK_MON_TABLE_NAME_STR "spider_link_mon_servers"
#define SPIDER_SYS_LINK_MON_TABLE_NAME_LEN (sizeof(SPIDER_SYS_LINK_MON_TABLE_NAME_STR) - 1)
#define SPIDER_SYS_LINK_FAILED_TABLE_NAME_STR "spider_link_failed_log"
#define SPIDER_SYS_LINK_FAILED_TABLE_NAME_LEN (sizeof(SPIDER_SYS_LINK_FAILED_TABLE_NAME_STR) - 1)
#define SPIDER_SYS_XA_FAILED_TABLE_NAME_STR "spider_xa_failed_log"
#define SPIDER_SYS_XA_FAILED_TABLE_NAME_LEN (sizeof(SPIDER_SYS_XA_FAILED_TABLE_NAME_STR) - 1)

#define SPIDER_SYS_XA_PREPARED_STR "PREPARED"
#define SPIDER_SYS_XA_NOT_YET_STR "NOT YET"
#define SPIDER_SYS_XA_COMMIT_STR "COMMIT"
#define SPIDER_SYS_XA_ROLLBACK_STR "ROLLBACK"

#define SPIDER_SYS_XA_COL_CNT 5
#define SPIDER_SYS_XA_PK_COL_CNT 3
#define SPIDER_SYS_XA_IDX1_COL_CNT 1
#define SPIDER_SYS_XA_MEMBER_COL_CNT 18
#define SPIDER_SYS_XA_MEMBER_PK_COL_CNT 6
#define SPIDER_SYS_TABLES_COL_CNT 22
#define SPIDER_SYS_TABLES_PK_COL_CNT 2
#define SPIDER_SYS_TABLES_IDX1_COL_CNT 1
#define SPIDER_SYS_LINK_MON_TABLE_COL_CNT 19

#define SPIDER_SYS_LINK_MON_TABLE_DB_NAME_SIZE 64
#define SPIDER_SYS_LINK_MON_TABLE_TABLE_NAME_SIZE 64
#define SPIDER_SYS_LINK_MON_TABLE_LINK_ID_SIZE 10

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

#if MYSQL_VERSION_ID < 50500
TABLE *spider_open_sys_table(
  THD *thd,
  const char *table_name,
  int table_name_length,
  bool write,
  Open_tables_state *open_tables_backup,
  bool need_lock,
  int *error_num
);

void spider_close_sys_table(
  THD *thd,
  TABLE *table,
  Open_tables_state *open_tables_backup,
  bool need_lock
);
#else
TABLE *spider_open_sys_table(
  THD *thd,
  const char *table_name,
  int table_name_length,
  bool write,
  Open_tables_backup *open_tables_backup,
  bool need_lock,
  int *error_num
);

void spider_close_sys_table(
  THD *thd,
  TABLE *table,
  Open_tables_backup *open_tables_backup,
  bool need_lock
);

bool spider_sys_open_tables(
  THD *thd,
  TABLE_LIST **tables,
  Open_tables_backup *open_tables_backup
);

TABLE *spider_sys_open_table(
  THD *thd,
  TABLE_LIST *tables,
  Open_tables_backup *open_tables_backup
);

void spider_sys_close_table(
  THD *thd,
  Open_tables_backup *open_tables_backup
);
#endif

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

void spider_store_link_chk_server_id(
  TABLE *table,
  uint32 server_id
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

int spider_get_sys_tables_link_status(
  TABLE *table,
  SPIDER_SHARE *share,
  int link_idx,
  MEM_ROOT *mem_root
);

int spider_get_sys_tables_link_idx(
  TABLE *table,
  int *link_idx,
  MEM_ROOT *mem_root
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

int spider_sys_replace(
  TABLE *table,
  bool *modified_non_trans_table
);

TABLE *spider_mk_sys_tmp_table(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name,
  CHARSET_INFO *cs
);

void spider_rm_sys_tmp_table(
  THD *thd,
  TABLE *tmp_table,
  TMP_TABLE_PARAM *tmp_tbl_prm
);

TABLE *spider_mk_sys_tmp_table_for_result(
  THD *thd,
  TABLE *table,
  TMP_TABLE_PARAM *tmp_tbl_prm,
  const char *field_name1,
  const char *field_name2,
  const char *field_name3,
  CHARSET_INFO *cs
);

void spider_rm_sys_tmp_table_for_result(
  THD *thd,
  TABLE *tmp_table,
  TMP_TABLE_PARAM *tmp_tbl_prm
);
