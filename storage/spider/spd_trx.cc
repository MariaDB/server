/* Copyright (C) 2008-2015 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#define MYSQL_SERVER 1
#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "records.h"
#endif
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_sys_table.h"
#include "ha_spider.h"
#include "spd_trx.h"
#include "spd_db_conn.h"
#include "spd_table.h"
#include "spd_conn.h"
#include "spd_ping_table.h"
#include "spd_malloc.h"

#ifdef SPIDER_XID_USES_xid_cache_iterate
#else
#ifdef XID_CACHE_IS_SPLITTED
extern uint *spd_db_att_xid_cache_split_num;
#endif
extern pthread_mutex_t *spd_db_att_LOCK_xid_cache;
extern HASH *spd_db_att_xid_cache;
#endif
extern struct charset_info_st *spd_charset_utf8_bin;

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
pthread_mutex_t spider_thread_id_mutex;
ulonglong spider_thread_id = 1;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key spd_key_mutex_udf_table;
#endif

extern HASH spider_allocated_thds;
extern uint spider_allocated_thds_id;
extern const char *spider_allocated_thds_func_name;
extern const char *spider_allocated_thds_file_name;
extern ulong spider_allocated_thds_line_no;
extern pthread_mutex_t spider_allocated_thds_mutex;

// for spider_alter_tables
uchar *spider_alter_tbl_get_key(
  SPIDER_ALTER_TABLE *alter_table,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_alter_tbl_get_key");
  *length = alter_table->table_name_length;
  DBUG_PRINT("info",("spider table_name_length=%zu", *length));
  DBUG_PRINT("info",("spider table_name=%s", alter_table->table_name));
  DBUG_RETURN((uchar*) alter_table->table_name);
}

// for SPIDER_TRX_HA
uchar *spider_trx_ha_get_key(
  SPIDER_TRX_HA *trx_ha,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_trx_ha_get_key");
  *length = trx_ha->table_name_length;
  DBUG_PRINT("info",("spider table_name_length=%zu", *length));
  DBUG_PRINT("info",("spider table_name=%s", trx_ha->table_name));
  DBUG_RETURN((uchar*) trx_ha->table_name);
}

int spider_free_trx_conn(
  SPIDER_TRX *trx,
  bool trx_free
) {
  int roop_count;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_free_trx_conn");
  roop_count = 0;
  if (
    trx_free ||
    spider_param_conn_recycle_mode(trx->thd) != 2
  ) {
    while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
      roop_count)))
    {
      spider_conn_clear_queue_at_commit(conn);
      if (conn->table_lock)
      {
        DBUG_ASSERT(!trx_free);
        roop_count++;
      } else
        spider_free_conn_from_trx(trx, conn, FALSE, trx_free, &roop_count);
    }
    trx->trx_conn_adjustment++;
  } else {
    while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
      roop_count)))
    {
      spider_conn_clear_queue_at_commit(conn);
      if (conn->table_lock)
      {
        DBUG_ASSERT(!trx_free);
      } else
        conn->error_mode = 1;
      roop_count++;
    }
  }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  roop_count = 0;
  if (
    trx_free ||
    spider_param_hs_r_conn_recycle_mode(trx->thd) != 2
  ) {
    while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_hs_r_conn_hash,
      roop_count)))
    {
      if (conn->table_lock)
      {
        DBUG_ASSERT(!trx_free);
        roop_count++;
      } else
        spider_free_conn_from_trx(trx, conn, FALSE, trx_free, &roop_count);
    }
    trx->trx_hs_r_conn_adjustment++;
  } else {
    while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_hs_r_conn_hash,
      roop_count)))
    {
      if (conn->table_lock)
      {
        DBUG_ASSERT(!trx_free);
      } else
        conn->error_mode = 1;
      roop_count++;
    }
  }
  roop_count = 0;
  if (
    trx_free ||
    spider_param_hs_w_conn_recycle_mode(trx->thd) != 2
  ) {
    while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_hs_w_conn_hash,
      roop_count)))
    {
      if (conn->table_lock)
      {
        DBUG_ASSERT(!trx_free);
        roop_count++;
      } else
        spider_free_conn_from_trx(trx, conn, FALSE, trx_free, &roop_count);
    }
    trx->trx_hs_w_conn_adjustment++;
  } else {
    while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_hs_w_conn_hash,
      roop_count)))
    {
      if (conn->table_lock)
      {
        DBUG_ASSERT(!trx_free);
      } else
        conn->error_mode = 1;
      roop_count++;
    }
  }
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (trx_free)
  {
    while ((conn = (SPIDER_CONN*) my_hash_element(
      &trx->trx_direct_hs_r_conn_hash, 0)))
    {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(&trx->trx_direct_hs_r_conn_hash,
        conn->conn_key_hash_value, (uchar*) conn);
#else
      my_hash_delete(&trx->trx_direct_hs_r_conn_hash, (uchar*) conn);
#endif
      spider_free_conn(conn);
    }
    while ((conn = (SPIDER_CONN*) my_hash_element(
      &trx->trx_direct_hs_w_conn_hash, 0)))
    {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(&trx->trx_direct_hs_w_conn_hash,
        conn->conn_key_hash_value, (uchar*) conn);
#else
      my_hash_delete(&trx->trx_direct_hs_w_conn_hash, (uchar*) conn);
#endif
      spider_free_conn(conn);
    }
  }
#endif
  DBUG_RETURN(0);
}

int spider_free_trx_another_conn(
  SPIDER_TRX *trx,
  bool lock
) {
  int error_num, tmp_error_num;
  int roop_count = 0;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_free_trx_another_conn");
  trx->tmp_spider->conns = &conn;
  error_num = 0;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_another_conn_hash,
    roop_count)))
  {
    if (lock && (tmp_error_num = spider_db_unlock_tables(trx->tmp_spider, 0)))
      error_num = tmp_error_num;
    spider_free_conn_from_trx(trx, conn, TRUE, TRUE, &roop_count);
  }
  DBUG_RETURN(error_num);
}

int spider_trx_another_lock_tables(
  SPIDER_TRX *trx
) {
  int error_num;
  int roop_count = 0, need_mon = 0;
  THD *thd = trx->thd;
  SPIDER_CONN *conn;
  ha_spider tmp_spider;
  SPIDER_SHARE tmp_share;
  char sql_buf[MAX_FIELD_WIDTH];
  spider_string sql_str(sql_buf, sizeof(sql_buf), system_charset_info);
  DBUG_ENTER("spider_trx_another_lock_tables");
  SPIDER_BACKUP_DASTATUS;
  sql_str.init_calc_mem(188);
  sql_str.length(0);
  memset((void*)&tmp_spider, 0, sizeof(ha_spider));
  memset(&tmp_share, 0, sizeof(SPIDER_SHARE));
  tmp_spider.share = &tmp_share;
  tmp_spider.trx = trx;
  tmp_share.access_charset = system_charset_info;
/*
  if ((error_num = spider_db_append_set_names(&tmp_share)))
    DBUG_RETURN(error_num);
*/
  tmp_spider.conns = &conn;
  tmp_spider.result_list.sqls = &sql_str;
  tmp_spider.need_mons = &need_mon;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_another_conn_hash,
    roop_count)))
  {
    if ((error_num = spider_db_lock_tables(&tmp_spider, 0)))
    {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
      {
/*
        spider_db_free_set_names(&tmp_share);
*/
        DBUG_RETURN(error_num);
      }
    }
    roop_count++;
  }
/*
  spider_db_free_set_names(&tmp_share);
*/
  DBUG_RETURN(0);
}

int spider_trx_another_flush_tables(
  SPIDER_TRX *trx
) {
  int error_num;
  int roop_count = 0, need_mon = 0;
  THD *thd = trx->thd;
  SPIDER_CONN *conn;
  ha_spider tmp_spider;
  SPIDER_SHARE tmp_share;
  long tmp_link_statuses = SPIDER_LINK_STATUS_OK;
  DBUG_ENTER("spider_trx_another_flush_tables");
  SPIDER_BACKUP_DASTATUS;
  memset((void*)&tmp_spider, 0, sizeof(ha_spider));
  tmp_share.link_count = 1;
  tmp_share.all_link_count = 1;
  tmp_share.link_statuses = &tmp_link_statuses;
  tmp_share.link_statuses_length = 1;
  tmp_spider.share = &tmp_share;
  tmp_spider.conns = &conn;
  tmp_spider.need_mons = &need_mon;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_another_conn_hash,
    roop_count)))
  {
    if ((error_num = spider_db_flush_tables(&tmp_spider, FALSE)))
    {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }
  DBUG_RETURN(0);
}

int spider_trx_all_flush_tables(
  SPIDER_TRX *trx
) {
  int error_num;
  int roop_count = 0, need_mon = 0;
  THD *thd = trx->thd;
  SPIDER_CONN *conn;
  ha_spider tmp_spider;
  SPIDER_SHARE tmp_share;
  long tmp_link_statuses = SPIDER_LINK_STATUS_OK;
  DBUG_ENTER("spider_trx_all_flush_tables");
  SPIDER_BACKUP_DASTATUS;
  memset((void*)&tmp_spider, 0, sizeof(ha_spider));
  tmp_share.link_count = 1;
  tmp_share.all_link_count = 1;
  tmp_share.link_statuses = &tmp_link_statuses;
  tmp_share.link_statuses_length = 1;
  tmp_spider.share = &tmp_share;
  tmp_spider.conns = &conn;
  tmp_spider.need_mons = &need_mon;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
    roop_count)))
  {
    if ((error_num = spider_db_flush_tables(&tmp_spider, TRUE)))
    {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }
  DBUG_RETURN(0);
}

int spider_trx_all_unlock_tables(
  SPIDER_TRX *trx
) {
  int error_num;
  int roop_count = 0;
  THD *thd = trx->thd;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_trx_all_unlock_tables");
  SPIDER_BACKUP_DASTATUS;
  trx->tmp_spider->conns = &conn;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
    roop_count)))
  {
    if ((error_num = spider_db_unlock_tables(trx->tmp_spider, 0)))
    {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }
  DBUG_RETURN(0);
}

int spider_trx_all_start_trx(
  SPIDER_TRX *trx
) {
  int error_num, need_mon = 0;
  int roop_count = 0;
  THD *thd = trx->thd;
  SPIDER_CONN *conn;
  ha_spider tmp_spider;
  DBUG_ENTER("spider_trx_all_start_trx");
  SPIDER_BACKUP_DASTATUS;
  memset((void*)&tmp_spider, 0, sizeof(ha_spider));
  tmp_spider.trx = trx;
  tmp_spider.need_mons = &need_mon;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
    roop_count)))
  {
    if (
      (spider_param_sync_trx_isolation(trx->thd) &&
        (error_num = spider_check_and_set_trx_isolation(conn, &need_mon))) ||
      (error_num = spider_internal_start_trx(&tmp_spider, conn, 0))
    ) {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }
  DBUG_RETURN(0);
}

int spider_trx_all_flush_logs(
  SPIDER_TRX *trx
) {
  int error_num;
  int roop_count = 0, need_mon = 0;
  THD *thd = trx->thd;
  SPIDER_CONN *conn;
  ha_spider tmp_spider;
  SPIDER_SHARE tmp_share;
  long tmp_link_statuses = SPIDER_LINK_STATUS_OK;
  uint conn_link_idx = 0;
  long net_read_timeout = 600;
  long net_write_timeout = 600;
  DBUG_ENTER("spider_trx_all_flush_logs");
  SPIDER_BACKUP_DASTATUS;
  memset((void*)&tmp_spider, 0, sizeof(ha_spider));
  tmp_share.link_count = 1;
  tmp_share.all_link_count = 1;
  tmp_share.link_statuses = &tmp_link_statuses;
  tmp_share.link_statuses_length = 1;
  tmp_share.net_read_timeouts = &net_read_timeout;
  tmp_share.net_read_timeouts_length = 1;
  tmp_share.net_write_timeouts = &net_write_timeout;
  tmp_share.net_write_timeouts_length = 1;
  tmp_spider.share = &tmp_share;
  tmp_spider.conns = &conn;
  tmp_spider.need_mons = &need_mon;
  tmp_spider.conn_link_idx = &conn_link_idx;
  tmp_spider.trx = trx;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
    roop_count)))
  {
    if ((error_num = spider_db_flush_logs(&tmp_spider)))
    {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
        DBUG_RETURN(error_num);
    }
    roop_count++;
  }
  DBUG_RETURN(0);
}

void spider_free_trx_alter_table_alloc(
  SPIDER_TRX *trx,
  SPIDER_ALTER_TABLE *alter_table
) {
  DBUG_ENTER("spider_free_trx_alter_table_alloc");
#ifdef HASH_UPDATE_WITH_HASH_VALUE
  my_hash_delete_with_hash_value(&trx->trx_alter_table_hash,
    alter_table->table_name_hash_value, (uchar*) alter_table);
#else
  my_hash_delete(&trx->trx_alter_table_hash, (uchar*) alter_table);
#endif
  if (alter_table->tmp_char)
    spider_free(trx, alter_table->tmp_char, MYF(0));
  spider_free(trx, alter_table, MYF(0));
  DBUG_VOID_RETURN;
}

int spider_free_trx_alter_table(
  SPIDER_TRX *trx
) {
  SPIDER_ALTER_TABLE *alter_table;
  DBUG_ENTER("spider_free_trx_alter_table");
  while ((alter_table =
    (SPIDER_ALTER_TABLE*) my_hash_element(&trx->trx_alter_table_hash, 0)))
  {
    spider_free_trx_alter_table_alloc(trx, alter_table);
  }
  DBUG_RETURN(0);
}

int spider_create_trx_alter_table(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  bool now_create
) {
  int error_num, roop_count;
  SPIDER_ALTER_TABLE *alter_table, *share_alter;
  char *tmp_name;
  char **tmp_server_names;
  char **tmp_tgt_table_names;
  char **tmp_tgt_dbs;
  char **tmp_tgt_hosts;
  char **tmp_tgt_usernames;
  char **tmp_tgt_passwords;
  char **tmp_tgt_sockets;
  char **tmp_tgt_wrappers;
  char **tmp_tgt_ssl_cas;
  char **tmp_tgt_ssl_capaths;
  char **tmp_tgt_ssl_certs;
  char **tmp_tgt_ssl_ciphers;
  char **tmp_tgt_ssl_keys;
  char **tmp_tgt_default_files;
  char **tmp_tgt_default_groups;
  uint *tmp_server_names_lengths;
  uint *tmp_tgt_table_names_lengths;
  uint *tmp_tgt_dbs_lengths;
  uint *tmp_tgt_hosts_lengths;
  uint *tmp_tgt_usernames_lengths;
  uint *tmp_tgt_passwords_lengths;
  uint *tmp_tgt_sockets_lengths;
  uint *tmp_tgt_wrappers_lengths;
  uint *tmp_tgt_ssl_cas_lengths;
  uint *tmp_tgt_ssl_capaths_lengths;
  uint *tmp_tgt_ssl_certs_lengths;
  uint *tmp_tgt_ssl_ciphers_lengths;
  uint *tmp_tgt_ssl_keys_lengths;
  uint *tmp_tgt_default_files_lengths;
  uint *tmp_tgt_default_groups_lengths;
  long *tmp_tgt_ports;
  long *tmp_tgt_ssl_vscs;
  long *tmp_link_statuses;
  char *tmp_server_names_char;
  char *tmp_tgt_table_names_char;
  char *tmp_tgt_dbs_char;
  char *tmp_tgt_hosts_char;
  char *tmp_tgt_usernames_char;
  char *tmp_tgt_passwords_char;
  char *tmp_tgt_sockets_char;
  char *tmp_tgt_wrappers_char;
  char *tmp_tgt_ssl_cas_char;
  char *tmp_tgt_ssl_capaths_char;
  char *tmp_tgt_ssl_certs_char;
  char *tmp_tgt_ssl_ciphers_char;
  char *tmp_tgt_ssl_keys_char;
  char *tmp_tgt_default_files_char;
  char *tmp_tgt_default_groups_char;
  uint old_elements;

  DBUG_ENTER("spider_create_trx_alter_table");
  share_alter = &share->alter_table;

  if (!(alter_table = (SPIDER_ALTER_TABLE *)
    spider_bulk_malloc(spider_current_trx, 55, MYF(MY_WME | MY_ZEROFILL),
      &alter_table, sizeof(*alter_table),
      &tmp_name, sizeof(char) * (share->table_name_length + 1),

      &tmp_server_names, sizeof(char *) * share->all_link_count,
      &tmp_tgt_table_names, sizeof(char *) * share->all_link_count,
      &tmp_tgt_dbs, sizeof(char *) * share->all_link_count,
      &tmp_tgt_hosts, sizeof(char *) * share->all_link_count,
      &tmp_tgt_usernames, sizeof(char *) * share->all_link_count,
      &tmp_tgt_passwords, sizeof(char *) * share->all_link_count,
      &tmp_tgt_sockets, sizeof(char *) * share->all_link_count,
      &tmp_tgt_wrappers, sizeof(char *) * share->all_link_count,
      &tmp_tgt_ssl_cas, sizeof(char *) * share->all_link_count,
      &tmp_tgt_ssl_capaths, sizeof(char *) * share->all_link_count,
      &tmp_tgt_ssl_certs, sizeof(char *) * share->all_link_count,
      &tmp_tgt_ssl_ciphers, sizeof(char *) * share->all_link_count,
      &tmp_tgt_ssl_keys, sizeof(char *) * share->all_link_count,
      &tmp_tgt_default_files, sizeof(char *) * share->all_link_count,
      &tmp_tgt_default_groups, sizeof(char *) * share->all_link_count,

      &tmp_server_names_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_table_names_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_dbs_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_hosts_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_usernames_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_passwords_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_sockets_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_wrappers_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_ssl_cas_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_ssl_capaths_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_ssl_certs_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_ssl_ciphers_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_ssl_keys_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_default_files_lengths, sizeof(uint) * share->all_link_count,
      &tmp_tgt_default_groups_lengths, sizeof(uint) * share->all_link_count,

      &tmp_tgt_ports, sizeof(long) * share->all_link_count,
      &tmp_tgt_ssl_vscs, sizeof(long) * share->all_link_count,
      &tmp_link_statuses, sizeof(long) * share->all_link_count,

      &tmp_server_names_char, sizeof(char) *
        (share_alter->tmp_server_names_charlen + 1),
      &tmp_tgt_table_names_char, sizeof(char) *
        (share_alter->tmp_tgt_table_names_charlen + 1),
      &tmp_tgt_dbs_char, sizeof(char) *
        (share_alter->tmp_tgt_dbs_charlen + 1),
      &tmp_tgt_hosts_char, sizeof(char) *
        (share_alter->tmp_tgt_hosts_charlen + 1),
      &tmp_tgt_usernames_char, sizeof(char) *
        (share_alter->tmp_tgt_usernames_charlen + 1),
      &tmp_tgt_passwords_char, sizeof(char) *
        (share_alter->tmp_tgt_passwords_charlen + 1),
      &tmp_tgt_sockets_char, sizeof(char) *
        (share_alter->tmp_tgt_sockets_charlen + 1),
      &tmp_tgt_wrappers_char, sizeof(char) *
        (share_alter->tmp_tgt_wrappers_charlen + 1),
      &tmp_tgt_ssl_cas_char, sizeof(char) *
        (share_alter->tmp_tgt_ssl_cas_charlen + 1),
      &tmp_tgt_ssl_capaths_char, sizeof(char) *
        (share_alter->tmp_tgt_ssl_capaths_charlen + 1),
      &tmp_tgt_ssl_certs_char, sizeof(char) *
        (share_alter->tmp_tgt_ssl_certs_charlen + 1),
      &tmp_tgt_ssl_ciphers_char, sizeof(char) *
        (share_alter->tmp_tgt_ssl_ciphers_charlen + 1),
      &tmp_tgt_ssl_keys_char, sizeof(char) *
        (share_alter->tmp_tgt_ssl_keys_charlen + 1),
      &tmp_tgt_default_files_char, sizeof(char) *
        (share_alter->tmp_tgt_default_files_charlen + 1),
      &tmp_tgt_default_groups_char, sizeof(char) *
        (share_alter->tmp_tgt_default_groups_charlen + 1),
      NullS))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_alloc_alter_table;
  }
  alter_table->now_create = now_create;
  alter_table->table_name = tmp_name;
  memcpy(alter_table->table_name, share->table_name, share->table_name_length);
  alter_table->table_name_length = share->table_name_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  DBUG_PRINT("info",("spider table_name_hash_value=%u",
    share->table_name_hash_value));
  alter_table->table_name_hash_value = share->table_name_hash_value;
#endif
  alter_table->tmp_priority = share->priority;
  alter_table->link_count = share->link_count;
  alter_table->all_link_count = share->all_link_count;

  alter_table->tmp_server_names = tmp_server_names;
  alter_table->tmp_tgt_table_names = tmp_tgt_table_names;
  alter_table->tmp_tgt_dbs = tmp_tgt_dbs;
  alter_table->tmp_tgt_hosts = tmp_tgt_hosts;
  alter_table->tmp_tgt_usernames = tmp_tgt_usernames;
  alter_table->tmp_tgt_passwords = tmp_tgt_passwords;
  alter_table->tmp_tgt_sockets = tmp_tgt_sockets;
  alter_table->tmp_tgt_wrappers = tmp_tgt_wrappers;
  alter_table->tmp_tgt_ssl_cas = tmp_tgt_ssl_cas;
  alter_table->tmp_tgt_ssl_capaths = tmp_tgt_ssl_capaths;
  alter_table->tmp_tgt_ssl_certs = tmp_tgt_ssl_certs;
  alter_table->tmp_tgt_ssl_ciphers = tmp_tgt_ssl_ciphers;
  alter_table->tmp_tgt_ssl_keys = tmp_tgt_ssl_keys;
  alter_table->tmp_tgt_default_files = tmp_tgt_default_files;
  alter_table->tmp_tgt_default_groups = tmp_tgt_default_groups;

  alter_table->tmp_tgt_ports = tmp_tgt_ports;
  alter_table->tmp_tgt_ssl_vscs = tmp_tgt_ssl_vscs;
  alter_table->tmp_link_statuses = tmp_link_statuses;

  alter_table->tmp_server_names_lengths = tmp_server_names_lengths;
  alter_table->tmp_tgt_table_names_lengths = tmp_tgt_table_names_lengths;
  alter_table->tmp_tgt_dbs_lengths = tmp_tgt_dbs_lengths;
  alter_table->tmp_tgt_hosts_lengths = tmp_tgt_hosts_lengths;
  alter_table->tmp_tgt_usernames_lengths = tmp_tgt_usernames_lengths;
  alter_table->tmp_tgt_passwords_lengths = tmp_tgt_passwords_lengths;
  alter_table->tmp_tgt_sockets_lengths = tmp_tgt_sockets_lengths;
  alter_table->tmp_tgt_wrappers_lengths = tmp_tgt_wrappers_lengths;
  alter_table->tmp_tgt_ssl_cas_lengths = tmp_tgt_ssl_cas_lengths;
  alter_table->tmp_tgt_ssl_capaths_lengths = tmp_tgt_ssl_capaths_lengths;
  alter_table->tmp_tgt_ssl_certs_lengths = tmp_tgt_ssl_certs_lengths;
  alter_table->tmp_tgt_ssl_ciphers_lengths = tmp_tgt_ssl_ciphers_lengths;
  alter_table->tmp_tgt_ssl_keys_lengths = tmp_tgt_ssl_keys_lengths;
  alter_table->tmp_tgt_default_files_lengths = tmp_tgt_default_files_lengths;
  alter_table->tmp_tgt_default_groups_lengths = tmp_tgt_default_groups_lengths;

  for(roop_count = 0; roop_count < (int) share->all_link_count; roop_count++)
  {
    tmp_server_names[roop_count] = tmp_server_names_char;
    memcpy(tmp_server_names_char,
      share_alter->tmp_server_names[roop_count],
      sizeof(char) * share_alter->tmp_server_names_lengths[roop_count]);
    tmp_server_names_char +=
      share_alter->tmp_server_names_lengths[roop_count] + 1;

    tmp_tgt_table_names[roop_count] = tmp_tgt_table_names_char;
    memcpy(tmp_tgt_table_names_char,
      share_alter->tmp_tgt_table_names[roop_count],
      sizeof(char) * share_alter->tmp_tgt_table_names_lengths[roop_count]);
    tmp_tgt_table_names_char +=
      share_alter->tmp_tgt_table_names_lengths[roop_count] + 1;

    tmp_tgt_dbs[roop_count] = tmp_tgt_dbs_char;
    memcpy(tmp_tgt_dbs_char, share_alter->tmp_tgt_dbs[roop_count],
      sizeof(char) * share_alter->tmp_tgt_dbs_lengths[roop_count]);
    tmp_tgt_dbs_char +=
      share_alter->tmp_tgt_dbs_lengths[roop_count] + 1;

    tmp_tgt_hosts[roop_count] = tmp_tgt_hosts_char;
    memcpy(tmp_tgt_hosts_char, share_alter->tmp_tgt_hosts[roop_count],
      sizeof(char) * share_alter->tmp_tgt_hosts_lengths[roop_count]);
    tmp_tgt_hosts_char +=
      share_alter->tmp_tgt_hosts_lengths[roop_count] + 1;

    tmp_tgt_usernames[roop_count] = tmp_tgt_usernames_char;
    memcpy(tmp_tgt_usernames_char, share_alter->tmp_tgt_usernames[roop_count],
      sizeof(char) * share_alter->tmp_tgt_usernames_lengths[roop_count]);
    tmp_tgt_usernames_char +=
      share_alter->tmp_tgt_usernames_lengths[roop_count] + 1;

    tmp_tgt_passwords[roop_count] = tmp_tgt_passwords_char;
    memcpy(tmp_tgt_passwords_char, share_alter->tmp_tgt_passwords[roop_count],
      sizeof(char) * share_alter->tmp_tgt_passwords_lengths[roop_count]);
    tmp_tgt_passwords_char +=
      share_alter->tmp_tgt_passwords_lengths[roop_count] + 1;

    tmp_tgt_sockets[roop_count] = tmp_tgt_sockets_char;
    memcpy(tmp_tgt_sockets_char, share_alter->tmp_tgt_sockets[roop_count],
      sizeof(char) * share_alter->tmp_tgt_sockets_lengths[roop_count]);
    tmp_tgt_sockets_char +=
      share_alter->tmp_tgt_sockets_lengths[roop_count] + 1;

    tmp_tgt_wrappers[roop_count] = tmp_tgt_wrappers_char;
    memcpy(tmp_tgt_wrappers_char, share_alter->tmp_tgt_wrappers[roop_count],
      sizeof(char) * share_alter->tmp_tgt_wrappers_lengths[roop_count]);
    tmp_tgt_wrappers_char +=
      share_alter->tmp_tgt_wrappers_lengths[roop_count] + 1;

    tmp_tgt_ssl_cas[roop_count] = tmp_tgt_ssl_cas_char;
    memcpy(tmp_tgt_ssl_cas_char, share_alter->tmp_tgt_ssl_cas[roop_count],
      sizeof(char) * share_alter->tmp_tgt_ssl_cas_lengths[roop_count]);
    tmp_tgt_ssl_cas_char +=
      share_alter->tmp_tgt_ssl_cas_lengths[roop_count] + 1;

    tmp_tgt_ssl_capaths[roop_count] = tmp_tgt_ssl_capaths_char;
    memcpy(tmp_tgt_ssl_capaths_char,
      share_alter->tmp_tgt_ssl_capaths[roop_count],
      sizeof(char) * share_alter->tmp_tgt_ssl_capaths_lengths[roop_count]);
    tmp_tgt_ssl_capaths_char +=
      share_alter->tmp_tgt_ssl_capaths_lengths[roop_count] + 1;

    tmp_tgt_ssl_certs[roop_count] = tmp_tgt_ssl_certs_char;
    memcpy(tmp_tgt_ssl_certs_char, share_alter->tmp_tgt_ssl_certs[roop_count],
      sizeof(char) * share_alter->tmp_tgt_ssl_certs_lengths[roop_count]);
    tmp_tgt_ssl_certs_char +=
      share_alter->tmp_tgt_ssl_certs_lengths[roop_count] + 1;

    tmp_tgt_ssl_ciphers[roop_count] = tmp_tgt_ssl_ciphers_char;
    memcpy(tmp_tgt_ssl_ciphers_char,
      share_alter->tmp_tgt_ssl_ciphers[roop_count],
      sizeof(char) * share_alter->tmp_tgt_ssl_ciphers_lengths[roop_count]);
    tmp_tgt_ssl_ciphers_char +=
      share_alter->tmp_tgt_ssl_ciphers_lengths[roop_count] + 1;

    tmp_tgt_ssl_keys[roop_count] = tmp_tgt_ssl_keys_char;
    memcpy(tmp_tgt_ssl_keys_char, share_alter->tmp_tgt_ssl_keys[roop_count],
      sizeof(char) * share_alter->tmp_tgt_ssl_keys_lengths[roop_count]);
    tmp_tgt_ssl_keys_char +=
      share_alter->tmp_tgt_ssl_keys_lengths[roop_count] + 1;

    tmp_tgt_default_files[roop_count] = tmp_tgt_default_files_char;
    memcpy(tmp_tgt_default_files_char,
      share_alter->tmp_tgt_default_files[roop_count],
      sizeof(char) * share_alter->tmp_tgt_default_files_lengths[roop_count]);
    tmp_tgt_default_files_char +=
      share_alter->tmp_tgt_default_files_lengths[roop_count] + 1;

    tmp_tgt_default_groups[roop_count] = tmp_tgt_default_groups_char;
    memcpy(tmp_tgt_default_groups_char,
      share_alter->tmp_tgt_default_groups[roop_count],
      sizeof(char) * share_alter->tmp_tgt_default_groups_lengths[roop_count]);
    tmp_tgt_default_groups_char +=
      share_alter->tmp_tgt_default_groups_lengths[roop_count] + 1;
  }

  memcpy(tmp_tgt_ports, share_alter->tmp_tgt_ports,
    sizeof(long) * share->all_link_count);
  memcpy(tmp_tgt_ssl_vscs, share_alter->tmp_tgt_ssl_vscs,
    sizeof(long) * share->all_link_count);
  memcpy(tmp_link_statuses, share_alter->tmp_link_statuses,
    sizeof(long) * share->all_link_count);

  memcpy(tmp_server_names_lengths, share_alter->tmp_server_names_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_table_names_lengths, share_alter->tmp_tgt_table_names_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_dbs_lengths, share_alter->tmp_tgt_dbs_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_hosts_lengths, share_alter->tmp_tgt_hosts_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_usernames_lengths, share_alter->tmp_tgt_usernames_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_passwords_lengths, share_alter->tmp_tgt_passwords_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_sockets_lengths, share_alter->tmp_tgt_sockets_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_wrappers_lengths, share_alter->tmp_tgt_wrappers_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_ssl_cas_lengths, share_alter->tmp_tgt_ssl_cas_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_ssl_capaths_lengths, share_alter->tmp_tgt_ssl_capaths_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_ssl_certs_lengths, share_alter->tmp_tgt_ssl_certs_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_ssl_ciphers_lengths, share_alter->tmp_tgt_ssl_ciphers_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_ssl_keys_lengths, share_alter->tmp_tgt_ssl_keys_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_default_files_lengths,
    share_alter->tmp_tgt_default_files_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(tmp_tgt_default_groups_lengths,
    share_alter->tmp_tgt_default_groups_lengths,
    sizeof(uint) * share->all_link_count);

  alter_table->tmp_server_names_length =
    share_alter->tmp_server_names_length;
  alter_table->tmp_tgt_table_names_length =
    share_alter->tmp_tgt_table_names_length;
  alter_table->tmp_tgt_dbs_length =
    share_alter->tmp_tgt_dbs_length;
  alter_table->tmp_tgt_hosts_length =
    share_alter->tmp_tgt_hosts_length;
  alter_table->tmp_tgt_usernames_length =
    share_alter->tmp_tgt_usernames_length;
  alter_table->tmp_tgt_passwords_length =
    share_alter->tmp_tgt_passwords_length;
  alter_table->tmp_tgt_sockets_length =
    share_alter->tmp_tgt_sockets_length;
  alter_table->tmp_tgt_wrappers_length =
    share_alter->tmp_tgt_wrappers_length;
  alter_table->tmp_tgt_ssl_cas_length =
    share_alter->tmp_tgt_ssl_cas_length;
  alter_table->tmp_tgt_ssl_capaths_length =
    share_alter->tmp_tgt_ssl_capaths_length;
  alter_table->tmp_tgt_ssl_certs_length =
    share_alter->tmp_tgt_ssl_certs_length;
  alter_table->tmp_tgt_ssl_ciphers_length =
    share_alter->tmp_tgt_ssl_ciphers_length;
  alter_table->tmp_tgt_ssl_keys_length =
    share_alter->tmp_tgt_ssl_keys_length;
  alter_table->tmp_tgt_default_files_length =
    share_alter->tmp_tgt_default_files_length;
  alter_table->tmp_tgt_default_groups_length =
    share_alter->tmp_tgt_default_groups_length;
  alter_table->tmp_tgt_ports_length =
    share_alter->tmp_tgt_ports_length;
  alter_table->tmp_tgt_ssl_vscs_length =
    share_alter->tmp_tgt_ssl_vscs_length;
  alter_table->tmp_link_statuses_length =
    share_alter->tmp_link_statuses_length;

  old_elements = trx->trx_alter_table_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
  if (my_hash_insert_with_hash_value(&trx->trx_alter_table_hash,
    alter_table->table_name_hash_value, (uchar*) alter_table))
#else
  if (my_hash_insert(&trx->trx_alter_table_hash, (uchar*) alter_table))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
  if (trx->trx_alter_table_hash.array.max_element > old_elements)
  {
    spider_alloc_calc_mem(spider_current_trx,
      trx->trx_alter_table_hash,
      (trx->trx_alter_table_hash.array.max_element - old_elements) *
      trx->trx_alter_table_hash.array.size_of_element);
  }
  DBUG_RETURN(0);

error:
  spider_free(trx, alter_table, MYF(0));
error_alloc_alter_table:
  DBUG_RETURN(error_num);
}

bool spider_cmp_trx_alter_table(
  SPIDER_ALTER_TABLE *cmp1,
  SPIDER_ALTER_TABLE *cmp2
) {
  int roop_count;
  DBUG_ENTER("spider_cmp_trx_alter_table");
  if (
    cmp1->tmp_priority != cmp2->tmp_priority ||
    cmp1->link_count != cmp2->link_count ||
    cmp1->all_link_count != cmp2->all_link_count
  )
    DBUG_RETURN(TRUE);

  for (roop_count = 0; roop_count < (int) cmp1->all_link_count; roop_count++)
  {
    if (
      (
        cmp1->tmp_server_names[roop_count] !=
          cmp2->tmp_server_names[roop_count] &&
        (
          !cmp1->tmp_server_names[roop_count] ||
          !cmp2->tmp_server_names[roop_count] ||
          strcmp(cmp1->tmp_server_names[roop_count],
            cmp2->tmp_server_names[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_table_names[roop_count] !=
          cmp2->tmp_tgt_table_names[roop_count] &&
        (
          !cmp1->tmp_tgt_table_names[roop_count] ||
          !cmp2->tmp_tgt_table_names[roop_count] ||
          strcmp(cmp1->tmp_tgt_table_names[roop_count],
            cmp2->tmp_tgt_table_names[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_dbs[roop_count] !=
          cmp2->tmp_tgt_dbs[roop_count] &&
        (
          !cmp1->tmp_tgt_dbs[roop_count] ||
          !cmp2->tmp_tgt_dbs[roop_count] ||
          strcmp(cmp1->tmp_tgt_dbs[roop_count],
            cmp2->tmp_tgt_dbs[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_hosts[roop_count] !=
          cmp2->tmp_tgt_hosts[roop_count] &&
        (
          !cmp1->tmp_tgt_hosts[roop_count] ||
          !cmp2->tmp_tgt_hosts[roop_count] ||
          strcmp(cmp1->tmp_tgt_hosts[roop_count],
            cmp2->tmp_tgt_hosts[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_usernames[roop_count] !=
          cmp2->tmp_tgt_usernames[roop_count] &&
        (
          !cmp1->tmp_tgt_usernames[roop_count] ||
          !cmp2->tmp_tgt_usernames[roop_count] ||
          strcmp(cmp1->tmp_tgt_usernames[roop_count],
            cmp2->tmp_tgt_usernames[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_passwords[roop_count] !=
          cmp2->tmp_tgt_passwords[roop_count] &&
        (
          !cmp1->tmp_tgt_passwords[roop_count] ||
          !cmp2->tmp_tgt_passwords[roop_count] ||
          strcmp(cmp1->tmp_tgt_passwords[roop_count],
            cmp2->tmp_tgt_passwords[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_sockets[roop_count] !=
          cmp2->tmp_tgt_sockets[roop_count] &&
        (
          !cmp1->tmp_tgt_sockets[roop_count] ||
          !cmp2->tmp_tgt_sockets[roop_count] ||
          strcmp(cmp1->tmp_tgt_sockets[roop_count],
            cmp2->tmp_tgt_sockets[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_wrappers[roop_count] !=
          cmp2->tmp_tgt_wrappers[roop_count] &&
        (
          !cmp1->tmp_tgt_wrappers[roop_count] ||
          !cmp2->tmp_tgt_wrappers[roop_count] ||
          strcmp(cmp1->tmp_tgt_wrappers[roop_count],
            cmp2->tmp_tgt_wrappers[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_ssl_cas[roop_count] !=
          cmp2->tmp_tgt_ssl_cas[roop_count] &&
        (
          !cmp1->tmp_tgt_ssl_cas[roop_count] ||
          !cmp2->tmp_tgt_ssl_cas[roop_count] ||
          strcmp(cmp1->tmp_tgt_ssl_cas[roop_count],
            cmp2->tmp_tgt_ssl_cas[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_ssl_capaths[roop_count] !=
          cmp2->tmp_tgt_ssl_capaths[roop_count] &&
        (
          !cmp1->tmp_tgt_ssl_capaths[roop_count] ||
          !cmp2->tmp_tgt_ssl_capaths[roop_count] ||
          strcmp(cmp1->tmp_tgt_ssl_capaths[roop_count],
            cmp2->tmp_tgt_ssl_capaths[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_ssl_certs[roop_count] !=
          cmp2->tmp_tgt_ssl_certs[roop_count] &&
        (
          !cmp1->tmp_tgt_ssl_certs[roop_count] ||
          !cmp2->tmp_tgt_ssl_certs[roop_count] ||
          strcmp(cmp1->tmp_tgt_ssl_certs[roop_count],
            cmp2->tmp_tgt_ssl_certs[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_ssl_ciphers[roop_count] !=
          cmp2->tmp_tgt_ssl_ciphers[roop_count] &&
        (
          !cmp1->tmp_tgt_ssl_ciphers[roop_count] ||
          !cmp2->tmp_tgt_ssl_ciphers[roop_count] ||
          strcmp(cmp1->tmp_tgt_ssl_ciphers[roop_count],
            cmp2->tmp_tgt_ssl_ciphers[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_ssl_keys[roop_count] !=
          cmp2->tmp_tgt_ssl_keys[roop_count] &&
        (
          !cmp1->tmp_tgt_ssl_keys[roop_count] ||
          !cmp2->tmp_tgt_ssl_keys[roop_count] ||
          strcmp(cmp1->tmp_tgt_ssl_keys[roop_count],
            cmp2->tmp_tgt_ssl_keys[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_default_files[roop_count] !=
          cmp2->tmp_tgt_default_files[roop_count] &&
        (
          !cmp1->tmp_tgt_default_files[roop_count] ||
          !cmp2->tmp_tgt_default_files[roop_count] ||
          strcmp(cmp1->tmp_tgt_default_files[roop_count],
            cmp2->tmp_tgt_default_files[roop_count])
        )
      ) ||
      (
        cmp1->tmp_tgt_default_groups[roop_count] !=
          cmp2->tmp_tgt_default_groups[roop_count] &&
        (
          !cmp1->tmp_tgt_default_groups[roop_count] ||
          !cmp2->tmp_tgt_default_groups[roop_count] ||
          strcmp(cmp1->tmp_tgt_default_groups[roop_count],
            cmp2->tmp_tgt_default_groups[roop_count])
        )
      ) ||
      cmp1->tmp_tgt_ports[roop_count] != cmp2->tmp_tgt_ports[roop_count] ||
      cmp1->tmp_tgt_ssl_vscs[roop_count] !=
        cmp2->tmp_tgt_ssl_vscs[roop_count] ||
      cmp1->tmp_link_statuses[roop_count] !=
        cmp2->tmp_link_statuses[roop_count]
    )
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

int spider_free_trx_alloc(
  SPIDER_TRX *trx
) {
  int roop_count;
  DBUG_ENTER("spider_free_trx_alloc");
  if (trx->tmp_spider)
  {
    for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; ++roop_count)
    {
      if (trx->tmp_spider->dbton_handler[roop_count])
      {
        delete trx->tmp_spider->dbton_handler[roop_count];
        trx->tmp_spider->dbton_handler[roop_count] = NULL;
      }
    }
    if (trx->tmp_spider->result_list.sqls)
    {
      delete [] trx->tmp_spider->result_list.sqls;
      trx->tmp_spider->result_list.sqls = NULL;
    }
    delete trx->tmp_spider;
    trx->tmp_spider = NULL;
  }
  if (trx->tmp_share)
  {
    for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; ++roop_count)
    {
      if (trx->tmp_share->dbton_share[roop_count])
      {
        delete trx->tmp_share->dbton_share[roop_count];
        trx->tmp_share->dbton_share[roop_count] = NULL;
      }
    }
    spider_free_tmp_share_alloc(trx->tmp_share);
  }
  spider_db_udf_free_set_names(trx);
  for (roop_count = spider_param_udf_table_lock_mutex_count() - 1;
    roop_count >= 0; roop_count--)
    pthread_mutex_destroy(&trx->udf_table_mutexes[roop_count]);
  spider_free_trx_ha(trx);
  spider_free_trx_conn(trx, TRUE);
  spider_free_trx_alter_table(trx);
  spider_free_mem_calc(spider_current_trx,
    trx->trx_conn_hash_id,
    trx->trx_conn_hash.array.max_element *
    trx->trx_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_conn_hash);
  spider_free_mem_calc(spider_current_trx,
    trx->trx_another_conn_hash_id,
    trx->trx_another_conn_hash.array.max_element *
    trx->trx_another_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_another_conn_hash);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_free_mem_calc(spider_current_trx,
    trx->trx_direct_hs_r_conn_hash_id,
    trx->trx_direct_hs_r_conn_hash.array.max_element *
    trx->trx_direct_hs_r_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_direct_hs_r_conn_hash);
  spider_free_mem_calc(spider_current_trx,
    trx->trx_direct_hs_w_conn_hash_id,
    trx->trx_direct_hs_w_conn_hash.array.max_element *
    trx->trx_direct_hs_w_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_direct_hs_w_conn_hash);
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_free_mem_calc(spider_current_trx,
    trx->trx_hs_r_conn_hash_id,
    trx->trx_hs_r_conn_hash.array.max_element *
    trx->trx_hs_r_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_hs_r_conn_hash);
  spider_free_mem_calc(spider_current_trx,
    trx->trx_hs_w_conn_hash_id,
    trx->trx_hs_w_conn_hash.array.max_element *
    trx->trx_hs_w_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_hs_w_conn_hash);
#endif
  spider_free_mem_calc(spider_current_trx,
    trx->trx_ha_hash_id,
    trx->trx_ha_hash.array.max_element *
    trx->trx_ha_hash.array.size_of_element);
  my_hash_free(&trx->trx_ha_hash);
  spider_free_mem_calc(spider_current_trx,
    trx->trx_alter_table_hash_id,
    trx->trx_alter_table_hash.array.max_element *
    trx->trx_alter_table_hash.array.size_of_element);
  my_hash_free(&trx->trx_alter_table_hash);
  free_root(&trx->mem_root, MYF(0));
  DBUG_RETURN(0);
}

SPIDER_TRX *spider_get_trx(
  THD *thd,
  bool regist_allocated_thds,
  int *error_num
) {
  int roop_count = 0, roop_count2;
  SPIDER_TRX *trx;
  SPIDER_SHARE *tmp_share;
  pthread_mutex_t *udf_table_mutexes;
  DBUG_ENTER("spider_get_trx");

  if (
    !thd ||
    !(trx = (SPIDER_TRX*) *thd_ha_data(thd, spider_hton_ptr))
  ) {
    DBUG_PRINT("info",("spider create new trx"));
    if (!(trx = (SPIDER_TRX *)
      spider_bulk_malloc(NULL, 56, MYF(MY_WME | MY_ZEROFILL),
        &trx, sizeof(*trx),
        &tmp_share, sizeof(SPIDER_SHARE),
        &udf_table_mutexes, sizeof(pthread_mutex_t) *
          spider_param_udf_table_lock_mutex_count(),
        NullS))
    )
      goto error_alloc_trx;

    SPD_INIT_ALLOC_ROOT(&trx->mem_root, 4096, 0, MYF(MY_WME));
    trx->tmp_share = tmp_share;
    trx->udf_table_mutexes = udf_table_mutexes;

    for (roop_count = 0;
      roop_count < (int) spider_param_udf_table_lock_mutex_count();
      roop_count++)
    {
#if MYSQL_VERSION_ID < 50500
      if (pthread_mutex_init(&trx->udf_table_mutexes[roop_count],
        MY_MUTEX_INIT_FAST))
#else
      if (mysql_mutex_init(spd_key_mutex_udf_table,
        &trx->udf_table_mutexes[roop_count], MY_MUTEX_INIT_FAST))
#endif
        goto error_init_udf_table_mutex;
    }

    if (
      my_hash_init(&trx->trx_conn_hash, spd_charset_utf8_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0)
    )
      goto error_init_hash;
    spider_alloc_calc_mem_init(trx->trx_conn_hash, 151);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_conn_hash,
      trx->trx_conn_hash.array.max_element *
      trx->trx_conn_hash.array.size_of_element);

    if (
      my_hash_init(&trx->trx_another_conn_hash, spd_charset_utf8_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0)
    )
      goto error_init_another_hash;
    spider_alloc_calc_mem_init(trx->trx_another_conn_hash, 152);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_another_conn_hash,
      trx->trx_another_conn_hash.array.max_element *
      trx->trx_another_conn_hash.array.size_of_element);

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (
      my_hash_init(&trx->trx_hs_r_conn_hash, spd_charset_utf8_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0)
    )
      goto error_hs_r_init_hash;
    spider_alloc_calc_mem_init(trx->trx_hs_r_conn_hash, 153);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_hs_r_conn_hash,
      trx->trx_hs_r_conn_hash.array.max_element *
      trx->trx_hs_r_conn_hash.array.size_of_element);

    if (
      my_hash_init(&trx->trx_hs_w_conn_hash, spd_charset_utf8_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0)
    )
      goto error_hs_w_init_hash;
    spider_alloc_calc_mem_init(trx->trx_hs_w_conn_hash, 154);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_hs_w_conn_hash,
      trx->trx_hs_w_conn_hash.array.max_element *
      trx->trx_hs_w_conn_hash.array.size_of_element);
#endif

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (
      my_hash_init(&trx->trx_direct_hs_r_conn_hash, spd_charset_utf8_bin, 32,
        0, 0, (my_hash_get_key) spider_conn_get_key, 0, 0)
    )
      goto error_direct_hs_r_init_hash;
    spider_alloc_calc_mem_init(trx->trx_direct_hs_r_conn_hash, 155);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_direct_hs_r_conn_hash,
      trx->trx_direct_hs_r_conn_hash.array.max_element *
      trx->trx_direct_hs_r_conn_hash.array.size_of_element);

    if (
      my_hash_init(&trx->trx_direct_hs_w_conn_hash, spd_charset_utf8_bin, 32,
        0, 0, (my_hash_get_key) spider_conn_get_key, 0, 0)
    )
      goto error_direct_hs_w_init_hash;
    spider_alloc_calc_mem_init(trx->trx_direct_hs_w_conn_hash, 156);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_direct_hs_w_conn_hash,
      trx->trx_direct_hs_w_conn_hash.array.max_element *
      trx->trx_direct_hs_w_conn_hash.array.size_of_element);
#endif

    if (
      my_hash_init(&trx->trx_alter_table_hash, spd_charset_utf8_bin, 32, 0, 0,
                   (my_hash_get_key) spider_alter_tbl_get_key, 0, 0)
    )
      goto error_init_alter_hash;
    spider_alloc_calc_mem_init(trx->trx_alter_table_hash, 157);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_alter_table_hash,
      trx->trx_alter_table_hash.array.max_element *
      trx->trx_alter_table_hash.array.size_of_element);

    if (
      my_hash_init(&trx->trx_ha_hash, spd_charset_utf8_bin, 32, 0, 0,
                   (my_hash_get_key) spider_trx_ha_get_key, 0, 0)
    )
      goto error_init_trx_ha_hash;
    spider_alloc_calc_mem_init(trx->trx_ha_hash, 158);
    spider_alloc_calc_mem(
      thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
      trx->trx_ha_hash,
      trx->trx_ha_hash.array.max_element *
      trx->trx_ha_hash.array.size_of_element);

    trx->thd = (THD*) thd;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    if (thd)
      trx->thd_hash_value = my_calc_hash(&spider_allocated_thds,
        (uchar*) thd, sizeof(THD *));
    else
      trx->thd_hash_value = 0;
#endif
    pthread_mutex_lock(&spider_thread_id_mutex);
    trx->spider_thread_id = spider_thread_id;
    ++spider_thread_id;
    pthread_mutex_unlock(&spider_thread_id_mutex);
    trx->trx_conn_adjustment = 1;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    trx->trx_hs_r_conn_adjustment = 1;
    trx->trx_hs_w_conn_adjustment = 1;
#endif

    if (thd)
    {
      spider_set_tmp_share_pointer(trx->tmp_share, trx->tmp_connect_info,
        trx->tmp_connect_info_length, trx->tmp_long, trx->tmp_longlong);
      if (
        spider_set_connect_info_default(
          trx->tmp_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
          NULL,
          NULL,
#endif
          NULL
        ) ||
        spider_set_connect_info_default_db_table(
          trx->tmp_share,
          "", 0,
          "", 0
        ) ||
        spider_create_conn_keys(trx->tmp_share)
      ) {
        goto error_set_connect_info_default;
      }

      if (!(trx->tmp_spider = new (&trx->mem_root) ha_spider()))
      {
        goto error_alloc_spider;
      }
      trx->tmp_spider->need_mons = &trx->tmp_need_mon;
      trx->tmp_spider->share = trx->tmp_share;
      trx->tmp_spider->trx = trx;
      trx->tmp_spider->dbton_handler = trx->tmp_dbton_handler;
      if (!(trx->tmp_spider->result_list.sqls =
        new spider_string[trx->tmp_share->link_count]))
      {
        goto error_init_result_list_sql;
      }
      for (roop_count2 = 0; roop_count2 < (int) trx->tmp_share->link_count;
        ++roop_count2)
      {
        trx->tmp_spider->result_list.sqls[roop_count2].init_calc_mem(121);
        trx->tmp_spider->result_list.sqls[roop_count2].set_charset(
          trx->tmp_share->access_charset);
      }

      for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; ++roop_count2)
      {
        if (!spider_dbton[roop_count2].init)
          continue;

        if (!(trx->tmp_share->dbton_share[roop_count2] =
          spider_dbton[roop_count2].create_db_share(trx->tmp_share)))
        {
          goto error_create_db_share;
        }
        if (trx->tmp_share->dbton_share[roop_count2]->init())
        {
          delete trx->tmp_share->dbton_share[roop_count2];
          trx->tmp_share->dbton_share[roop_count2] = NULL;
          goto error_create_db_share;
        }

        if (!(trx->tmp_spider->dbton_handler[roop_count2] =
          spider_dbton[roop_count2].create_db_handler(trx->tmp_spider,
            trx->tmp_share->dbton_share[roop_count2])))
        {
          goto error_create_db_share;
        }
        if (trx->tmp_spider->dbton_handler[roop_count2]->init())
        {
          delete trx->tmp_spider->dbton_handler[roop_count2];
          trx->tmp_spider->dbton_handler[roop_count2] = NULL;
          goto error_create_db_share;
        }
      }

      if (regist_allocated_thds)
      {
        pthread_mutex_lock(&spider_allocated_thds_mutex);
        uint old_elements = spider_allocated_thds.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        if (my_hash_insert_with_hash_value(&spider_allocated_thds,
          trx->thd_hash_value, (uchar*) thd))
#else
        if (my_hash_insert(&spider_allocated_thds, (uchar*) thd))
#endif
        {
          pthread_mutex_unlock(&spider_allocated_thds_mutex);
          goto error_allocated_thds_insert;
        }
        if (spider_allocated_thds.array.max_element > old_elements)
        {
          spider_alloc_calc_mem(trx,
            spider_allocated_thds,
            (spider_allocated_thds.array.max_element - old_elements) *
            spider_allocated_thds.array.size_of_element);
        }
        pthread_mutex_unlock(&spider_allocated_thds_mutex);
        trx->registed_allocated_thds = TRUE;
      }
      *thd_ha_data(thd, spider_hton_ptr) = (void *) trx;
    }
  }

  DBUG_PRINT("info",("spider trx=%p", trx));
  DBUG_RETURN(trx);

error_allocated_thds_insert:
error_alloc_spider:
error_create_db_share:
  if (thd)
  {
    delete [] trx->tmp_spider->result_list.sqls;
    trx->tmp_spider->result_list.sqls = NULL;
  }
error_init_result_list_sql:
  if (thd)
  {
    delete trx->tmp_spider;
    trx->tmp_spider = NULL;
    for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; ++roop_count2)
    {
      if (trx->tmp_spider->dbton_handler[roop_count2])
      {
        delete trx->tmp_spider->dbton_handler[roop_count2];
        trx->tmp_spider->dbton_handler[roop_count2] = NULL;
      }
      if (trx->tmp_share->dbton_share[roop_count2])
      {
        delete trx->tmp_share->dbton_share[roop_count2];
        trx->tmp_share->dbton_share[roop_count2] = NULL;
      }
    }
  }
error_set_connect_info_default:
  if (thd)
  {
    spider_free_tmp_share_alloc(trx->tmp_share);
  }
  spider_free_mem_calc(trx,
    trx->trx_ha_hash_id,
    trx->trx_ha_hash.array.max_element *
    trx->trx_ha_hash.array.size_of_element);
  my_hash_free(&trx->trx_ha_hash);
error_init_trx_ha_hash:
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_alter_table_hash_id,
    trx->trx_alter_table_hash.array.max_element *
    trx->trx_alter_table_hash.array.size_of_element);
  my_hash_free(&trx->trx_alter_table_hash);
error_init_alter_hash:
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_direct_hs_w_conn_hash_id,
    trx->trx_direct_hs_w_conn_hash.array.max_element *
    trx->trx_direct_hs_w_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_direct_hs_w_conn_hash);
error_direct_hs_w_init_hash:
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_direct_hs_r_conn_hash_id,
    trx->trx_direct_hs_r_conn_hash.array.max_element *
    trx->trx_direct_hs_r_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_direct_hs_r_conn_hash);
error_direct_hs_r_init_hash:
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_hs_w_conn_hash_id,
    trx->trx_hs_w_conn_hash.array.max_element *
    trx->trx_hs_w_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_hs_w_conn_hash);
error_hs_w_init_hash:
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_hs_r_conn_hash_id,
    trx->trx_hs_r_conn_hash.array.max_element *
    trx->trx_hs_r_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_hs_r_conn_hash);
error_hs_r_init_hash:
#endif
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_another_conn_hash_id,
    trx->trx_another_conn_hash.array.max_element *
    trx->trx_another_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_another_conn_hash);
error_init_another_hash:
  spider_free_mem_calc(
    thd ? ((SPIDER_TRX *) *thd_ha_data(thd, spider_hton_ptr)) : NULL,
    trx->trx_conn_hash_id,
    trx->trx_conn_hash.array.max_element *
    trx->trx_conn_hash.array.size_of_element);
  my_hash_free(&trx->trx_conn_hash);
error_init_hash:
  if (roop_count > 0)
  {
    for (roop_count--; roop_count >= 0; roop_count--)
      pthread_mutex_destroy(&trx->udf_table_mutexes[roop_count]);
  }
error_init_udf_table_mutex:
  free_root(&trx->mem_root, MYF(0));
  spider_free(NULL, trx, MYF(0));
error_alloc_trx:
  *error_num = HA_ERR_OUT_OF_MEM;
  DBUG_RETURN(NULL);
}

int spider_free_trx(
  SPIDER_TRX *trx,
  bool need_lock
) {
  DBUG_ENTER("spider_free_trx");
  if (trx->thd)
  {
    if (trx->registed_allocated_thds)
    {
      if (need_lock)
        pthread_mutex_lock(&spider_allocated_thds_mutex);
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(&spider_allocated_thds,
        trx->thd_hash_value, (uchar*) trx->thd);
#else
      my_hash_delete(&spider_allocated_thds, (uchar*) trx->thd);
#endif
      if (need_lock)
        pthread_mutex_unlock(&spider_allocated_thds_mutex);
    }
    *thd_ha_data(trx->thd, spider_hton_ptr) = (void *) NULL;
  }
  spider_free_trx_alloc(trx);
  spider_merge_mem_calc(trx, TRUE);
  spider_free(NULL, trx, MYF(0));
  DBUG_RETURN(0);
}

int spider_check_and_set_trx_isolation(
  SPIDER_CONN *conn,
  int *need_mon
) {
  int trx_isolation;
  DBUG_ENTER("spider_check_and_set_trx_isolation");

  trx_isolation = thd_tx_isolation(conn->thd);
  DBUG_PRINT("info",("spider local trx_isolation=%d", trx_isolation));
/*
  DBUG_PRINT("info",("spider conn->trx_isolation=%d", conn->trx_isolation));
  if (conn->trx_isolation != trx_isolation)
  {
*/
    spider_conn_queue_trx_isolation(conn, trx_isolation);
/*
    conn->trx_isolation = trx_isolation;
  }
*/
  DBUG_RETURN(0);
}

int spider_check_and_set_autocommit(
  THD *thd,
  SPIDER_CONN *conn,
  int *need_mon
) {
  bool autocommit;
  DBUG_ENTER("spider_check_and_set_autocommit");

  autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT);
  if (autocommit)
  {
    spider_conn_queue_autocommit(conn, TRUE);
  } else {
    spider_conn_queue_autocommit(conn, FALSE);
  }
/*
  if (autocommit && conn->autocommit != 1)
  {
    spider_conn_queue_autocommit(conn, TRUE);
    conn->autocommit = 1;
  } else if (!autocommit && conn->autocommit != 0)
  {
    spider_conn_queue_autocommit(conn, FALSE);
    conn->autocommit = 0;
  }
*/
  DBUG_RETURN(0);
}

int spider_check_and_set_sql_log_off(
  THD *thd,
  SPIDER_CONN *conn,
  int *need_mon
) {
  bool internal_sql_log_off;
  DBUG_ENTER("spider_check_and_set_sql_log_off");

  internal_sql_log_off = spider_param_internal_sql_log_off(thd);
  if (internal_sql_log_off)
  {
    spider_conn_queue_sql_log_off(conn, TRUE);
  } else {
    spider_conn_queue_sql_log_off(conn, FALSE);
  }
/*
  if (internal_sql_log_off && conn->sql_log_off != 1)
  {
    spider_conn_queue_sql_log_off(conn, TRUE);
    conn->sql_log_off = 1;
  } else if (!internal_sql_log_off && conn->sql_log_off != 0)
  {
    spider_conn_queue_sql_log_off(conn, FALSE);
    conn->sql_log_off = 0;
  }
*/
  DBUG_RETURN(0);
}

int spider_check_and_set_time_zone(
  THD *thd,
  SPIDER_CONN *conn,
  int *need_mon
) {
  Time_zone *time_zone;
  DBUG_ENTER("spider_check_and_set_time_zone");

  time_zone = thd->variables.time_zone;
  DBUG_PRINT("info",("spider local time_zone=%p", time_zone));
/*
  DBUG_PRINT("info",("spider conn->time_zone=%p", conn->time_zone));
  if (time_zone != conn->time_zone)
  {
*/
    spider_conn_queue_time_zone(conn, time_zone);
/*
    conn->time_zone = time_zone;
  }
*/
  DBUG_RETURN(0);
}

int spider_xa_lock(
  XID_STATE *xid_state
) {
  THD *thd = current_thd;
  int error_num;
  const char *old_proc_info;
  DBUG_ENTER("spider_xa_lock");
#ifdef SPIDER_XID_USES_xid_cache_iterate
#else
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value = my_calc_hash(spd_db_att_xid_cache,
    (uchar*) xid_state->xid.key(), xid_state->xid.key_length());
#ifdef XID_CACHE_IS_SPLITTED
  uint idx = hash_value % *spd_db_att_xid_cache_split_num;
#endif
#endif
#endif
  old_proc_info = thd_proc_info(thd, "Locking xid by Spider");
#ifdef SPIDER_XID_USES_xid_cache_iterate
  if (xid_cache_insert(thd, xid_state))
  {
    error_num = my_errno;
    goto error;
  }
#else
#ifdef XID_CACHE_IS_SPLITTED
  pthread_mutex_lock(&spd_db_att_LOCK_xid_cache[idx]);
#else
  pthread_mutex_lock(spd_db_att_LOCK_xid_cache);
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
#ifdef XID_CACHE_IS_SPLITTED
  if (my_hash_search_using_hash_value(&spd_db_att_xid_cache[idx], hash_value,
    xid_state->xid.key(), xid_state->xid.key_length()))
#else
  if (my_hash_search_using_hash_value(spd_db_att_xid_cache, hash_value,
    xid_state->xid.key(), xid_state->xid.key_length()))
#endif
#else
  if (my_hash_search(spd_db_att_xid_cache,
    xid_state->xid.key(), xid_state->xid.key_length()))
#endif
  {
    error_num = ER_SPIDER_XA_LOCKED_NUM;
    goto error;
  }
#ifdef HASH_UPDATE_WITH_HASH_VALUE
#ifdef XID_CACHE_IS_SPLITTED
  if (my_hash_insert_with_hash_value(&spd_db_att_xid_cache[idx], hash_value,
    (uchar*)xid_state))
#else
  if (my_hash_insert_with_hash_value(spd_db_att_xid_cache, hash_value,
    (uchar*)xid_state))
#endif
#else
  if (my_hash_insert(spd_db_att_xid_cache, (uchar*)xid_state))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
#ifdef XID_CACHE_IS_SPLITTED
  pthread_mutex_unlock(&spd_db_att_LOCK_xid_cache[idx]);
#else
  pthread_mutex_unlock(spd_db_att_LOCK_xid_cache);
#endif
#endif
  thd_proc_info(thd, old_proc_info);
  DBUG_RETURN(0);

error:
#ifdef SPIDER_XID_USES_xid_cache_iterate
#else
#ifdef XID_CACHE_IS_SPLITTED
  pthread_mutex_unlock(&spd_db_att_LOCK_xid_cache[idx]);
#else
  pthread_mutex_unlock(spd_db_att_LOCK_xid_cache);
#endif
#endif
  thd_proc_info(thd, old_proc_info);
  DBUG_RETURN(error_num);
}

int spider_xa_unlock(
  XID_STATE *xid_state
) {
  THD *thd = current_thd;
  const char *old_proc_info;
  DBUG_ENTER("spider_xa_unlock");
#ifdef SPIDER_XID_USES_xid_cache_iterate
#else
#if defined(SPIDER_HAS_HASH_VALUE_TYPE) && defined(HASH_UPDATE_WITH_HASH_VALUE)
  my_hash_value_type hash_value = my_calc_hash(spd_db_att_xid_cache,
    (uchar*) xid_state->xid.key(), xid_state->xid.key_length());
#ifdef XID_CACHE_IS_SPLITTED
  uint idx = hash_value % *spd_db_att_xid_cache_split_num;
#endif
#endif
#endif
  old_proc_info = thd_proc_info(thd, "Unlocking xid by Spider");
#ifdef SPIDER_XID_USES_xid_cache_iterate
  xid_cache_delete(thd, xid_state);
#else
#ifdef XID_CACHE_IS_SPLITTED
  pthread_mutex_lock(&spd_db_att_LOCK_xid_cache[idx]);
#else
  pthread_mutex_lock(spd_db_att_LOCK_xid_cache);
#endif
#if defined(SPIDER_HAS_HASH_VALUE_TYPE) && defined(HASH_UPDATE_WITH_HASH_VALUE)
#ifdef XID_CACHE_IS_SPLITTED
  my_hash_delete_with_hash_value(&spd_db_att_xid_cache[idx],
    hash_value, (uchar *)xid_state);
#else
  my_hash_delete_with_hash_value(spd_db_att_xid_cache,
    hash_value, (uchar *)xid_state);
#endif
#else
  my_hash_delete(spd_db_att_xid_cache, (uchar *)xid_state);
#endif
#ifdef XID_CACHE_IS_SPLITTED
  pthread_mutex_unlock(&spd_db_att_LOCK_xid_cache[idx]);
#else
  pthread_mutex_unlock(spd_db_att_LOCK_xid_cache);
#endif
#endif
  thd_proc_info(thd, old_proc_info);
  DBUG_RETURN(0);
}

int spider_start_internal_consistent_snapshot(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn,
  int *need_mon
) {
  DBUG_ENTER("spider_start_internal_consistent_snapshot");
  if (trx->trx_consistent_snapshot)
    DBUG_RETURN(spider_db_consistent_snapshot(conn, need_mon));
  DBUG_RETURN(0);
}

int spider_internal_start_trx(
  ha_spider *spider,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  SPIDER_TRX *trx = spider->trx;
  THD *thd = trx->thd;
  bool sync_autocommit = spider_param_sync_autocommit(thd);
  bool sync_time_zone = spider_param_sync_time_zone(thd);
  double ping_interval_at_trx_start =
    spider_param_ping_interval_at_trx_start(thd);
  bool xa_lock = FALSE;
  time_t tmp_time = (time_t) time((time_t*) 0);
  DBUG_ENTER("spider_internal_start_trx");

  if (
    conn->server_lost ||
    difftime(tmp_time, conn->ping_time) >= ping_interval_at_trx_start
  ) {
    spider_conn_queue_ping(spider, conn, link_idx);
  }
  conn->disable_reconnect = TRUE;
  if (!trx->trx_start)
  {
    if (!trx->trx_consistent_snapshot)
    {
      trx->use_consistent_snapshot =
        spider_param_use_consistent_snapshot(thd);
      trx->internal_xa = spider_param_internal_xa(thd);
      trx->internal_xa_snapshot = spider_param_internal_xa_snapshot(thd);
    }
  }
  if (
    (error_num = spider_check_and_set_sql_log_off(thd, conn,
      &spider->need_mons[link_idx])) ||
    (sync_time_zone &&
      (error_num = spider_check_and_set_time_zone(thd, conn,
        &spider->need_mons[link_idx]))) ||
    (sync_autocommit &&
      (error_num = spider_check_and_set_autocommit(thd, conn,
        &spider->need_mons[link_idx])))
  )
    goto error;
  if (trx->trx_consistent_snapshot)
  {
    if (trx->internal_xa && trx->internal_xa_snapshot < 2)
    {
      error_num = ER_SPIDER_CANT_USE_BOTH_INNER_XA_AND_SNAPSHOT_NUM;
      my_message(error_num, ER_SPIDER_CANT_USE_BOTH_INNER_XA_AND_SNAPSHOT_STR,
        MYF(0));
      goto error;
    } else if (!trx->internal_xa || trx->internal_xa_snapshot == 2)
    {
      if ((error_num = spider_start_internal_consistent_snapshot(trx, conn,
        &spider->need_mons[link_idx])))
        goto error;
    }
  }
  DBUG_PRINT("info",("spider trx->trx_start= %s",
    trx->trx_start ? "TRUE" : "FALSE"));
  if (!trx->trx_start)
  {
    if (
      thd->transaction.xid_state.xa_state == XA_ACTIVE &&
      spider_param_support_xa()
    ) {
      trx->trx_xa = TRUE;
      thd_get_xid(thd, (MYSQL_XID*) &trx->xid);
    }

    if (
      !trx->trx_xa &&
      trx->internal_xa &&
      (!trx->trx_consistent_snapshot || trx->internal_xa_snapshot == 3) &&
      spider->sql_command != SQLCOM_LOCK_TABLES
    ) {
      trx->trx_xa = TRUE;
      trx->xid.formatID = 1;
      if (spider_param_internal_xa_id_type(thd) == 0)
      {
        trx->xid.gtrid_length
          = my_sprintf(trx->xid.data,
          (trx->xid.data, "%lx", thd_get_thread_id(thd)));
      } else {
        trx->xid.gtrid_length
          = my_sprintf(trx->xid.data,
          (trx->xid.data, "%lx%016llx", thd_get_thread_id(thd),
            thd->query_id));
      }
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
      trx->xid.bqual_length
        = my_sprintf(trx->xid.data + trx->xid.gtrid_length,
        (trx->xid.data + trx->xid.gtrid_length, "%lx",
        thd->variables.server_id));
#else
      trx->xid.bqual_length
        = my_sprintf(trx->xid.data + trx->xid.gtrid_length,
        (trx->xid.data + trx->xid.gtrid_length, "%x",
        thd->server_id));
#endif

      trx->internal_xid_state.xa_state = XA_ACTIVE;
      trx->internal_xid_state.xid.set(&trx->xid);
#ifdef SPIDER_XID_STATE_HAS_in_thd
      trx->internal_xid_state.in_thd = 1;
#endif
      if ((error_num = spider_xa_lock(&trx->internal_xid_state)))
      {
        if (error_num == ER_SPIDER_XA_LOCKED_NUM)
          my_message(error_num, ER_SPIDER_XA_LOCKED_STR, MYF(0));
        goto error;
      }
      xa_lock = TRUE;
    } else
      trx->internal_xa = FALSE;

    DBUG_PRINT("info",("spider trx->trx_consistent_snapshot= %s",
      trx->trx_consistent_snapshot ? "TRUE" : "FALSE"));
    if (!trx->trx_consistent_snapshot)
    {
      trans_register_ha(thd, FALSE, spider_hton_ptr);
      if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
        trans_register_ha(thd, TRUE, spider_hton_ptr);
    }
    trx->trx_start = TRUE;
    trx->trx_xa_prepared = FALSE;
  }

  DBUG_PRINT("info",("spider sync_autocommit = %d", sync_autocommit));
  DBUG_PRINT("info",("spider conn->semi_trx_chk = %d", conn->semi_trx_chk));
  DBUG_PRINT("info",("spider conn->table_lock = %d", conn->table_lock));
  DBUG_PRINT("info",("spider conn->autocommit = %d", conn->autocommit));
  DBUG_PRINT("info",("spider semi_trx = %d", spider_param_semi_trx(thd)));
  conn->semi_trx = FALSE;
  if (conn->table_lock == 3)
  {
    DBUG_PRINT("info",("spider conn->table_lock == 3"));
    conn->disable_xa = TRUE;
  } else if (trx->trx_xa)
  {
    DBUG_PRINT("info",("spider trx->trx_xa"));
    if (
      sync_autocommit &&
      conn->semi_trx_chk &&
      !conn->table_lock &&
      (
        (!conn->queued_autocommit && conn->autocommit == 1) ||
        (conn->queued_autocommit && conn->queued_autocommit_val == TRUE)
      ) &&
      spider_param_semi_trx(thd)
    ) {
      DBUG_PRINT("info",("spider semi_trx is set"));
      conn->semi_trx = TRUE;
    }
    spider_conn_queue_xa_start(conn, &trx->xid);
    conn->disable_xa = FALSE;
  } else if (
    !trx->trx_consistent_snapshot &&
    !thd_test_options(thd, OPTION_BEGIN) &&
    sync_autocommit &&
    conn->semi_trx_chk &&
    !conn->table_lock &&
    (
      (!conn->queued_autocommit && conn->autocommit == 1) ||
      (conn->queued_autocommit && conn->queued_autocommit_val == TRUE)
    ) &&
    spider_param_semi_trx(thd)
  ) {
    DBUG_PRINT("info",("spider semi_trx is set"));
    spider_conn_queue_start_transaction(conn);
    conn->semi_trx = TRUE;
  } else if (
    !trx->trx_consistent_snapshot &&
    thd_test_options(thd, OPTION_BEGIN)
  ) {
    DBUG_PRINT("info",("spider start transaction"));
    spider_conn_queue_start_transaction(conn);
  }

  conn->join_trx = 1;
  if (trx->join_trx_top)
    spider_tree_insert(trx->join_trx_top, conn);
  else {
    conn->p_small = NULL;
    conn->p_big = NULL;
    conn->c_small = NULL;
    conn->c_big = NULL;
    trx->join_trx_top = conn;
  }
  DBUG_RETURN(0);

error:
  if (xa_lock)
    spider_xa_unlock(&trx->internal_xid_state);
  DBUG_RETURN(error_num);
}

int spider_internal_xa_commit(
  THD* thd,
  SPIDER_TRX *trx,
  XID* xid,
  TABLE *table_xa,
  TABLE *table_xa_member
) {
  int error_num, tmp_error_num;
  char xa_key[MAX_KEY_LENGTH];
  SPIDER_CONN *conn;
  uint force_commit = spider_param_force_commit(thd);
  MEM_ROOT mem_root;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  bool table_xa_opened = FALSE;
  bool table_xa_member_opened = FALSE;
  DBUG_ENTER("spider_internal_xa_commit");

  /*
    select
      status
    from
      mysql.spider_xa
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  spider_store_xa_pk(table_xa, &trx->xid);
  if (
    (error_num = spider_check_sys_table(table_xa, xa_key))
  ) {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table_xa->file->print_error(error_num, MYF(0));
      goto error;
    }
    my_message(ER_SPIDER_XA_NOT_EXISTS_NUM, ER_SPIDER_XA_NOT_EXISTS_STR,
      MYF(0));
    error_num = ER_SPIDER_XA_NOT_EXISTS_NUM;
    goto error;
  }
  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  if (
    force_commit != 2 &&
    (error_num = spider_check_sys_xa_status(
      table_xa,
      SPIDER_SYS_XA_PREPARED_STR,
      SPIDER_SYS_XA_COMMIT_STR,
      NULL,
      ER_SPIDER_XA_NOT_PREPARED_NUM,
      &mem_root))
  ) {
    free_root(&mem_root, MYF(0));
    if (error_num == ER_SPIDER_XA_NOT_PREPARED_NUM)
      my_message(error_num, ER_SPIDER_XA_NOT_PREPARED_STR, MYF(0));
    goto error;
  }
  free_root(&mem_root, MYF(0));

  /*
    update
      mysql.spider_xa
    set
      status = 'COMMIT'
    where
      format_id = trx->xid.format_id and
      gtrid_length = trx->xid.gtrid_length and
      data = trx->xid.data
  */
  if (
    (error_num = spider_update_xa(
      table_xa, &trx->xid, SPIDER_SYS_XA_COMMIT_STR))
  )
    goto error;
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;

  SPIDER_BACKUP_DASTATUS;
  if ((conn = spider_tree_first(trx->join_trx_top)))
  {
    do {
      if (conn->bg_search)
        spider_bg_conn_break(conn, NULL);
      DBUG_PRINT("info",("spider conn=%p", conn));
      DBUG_PRINT("info",("spider conn->join_trx=%u", conn->join_trx));
      if (conn->join_trx)
      {
        if ((tmp_error_num = spider_db_xa_commit(conn, &trx->xid)))
        {
          if (force_commit == 0 ||
            (force_commit == 1 && tmp_error_num != ER_XAER_NOTA))
          {
            SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
            if (!error_num && tmp_error_num)
              error_num = tmp_error_num;
          }
          spider_sys_log_xa_failed(thd, &trx->xid, conn,
            SPIDER_SYS_XA_COMMIT_STR, TRUE);
        }
        if ((tmp_error_num = spider_end_trx(trx, conn)))
        {
          SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
          if (!error_num && tmp_error_num)
            error_num = tmp_error_num;
        }
        conn->join_trx = 0;
      }
    } while ((conn = spider_tree_next(conn)));
    trx->join_trx_top = NULL;
  }
  if (error_num)
    goto error_in_commit;

  /*
    delete from
      mysql.spider_xa_member
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa_member = spider_open_sys_table(
      thd, SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
      SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
      &error_num))
  )
    goto error_open_table;
  table_xa_member_opened = TRUE;
  if ((error_num = spider_delete_xa_member(table_xa_member, &trx->xid)))
    goto error;
  spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
  table_xa_member_opened = FALSE;

  /*
    delete from
      mysql.spider_xa
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  if ((error_num = spider_delete_xa(table_xa, &trx->xid)))
    goto error;
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;
  spider_xa_unlock(&trx->internal_xid_state);
  trx->internal_xid_state.xa_state = XA_NOTR;
  DBUG_RETURN(0);

error:
  if (table_xa_opened)
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  if (table_xa_member_opened)
    spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
error_in_commit:
error_open_table:
  spider_xa_unlock(&trx->internal_xid_state);
  trx->internal_xid_state.xa_state = XA_NOTR;
  DBUG_RETURN(error_num);
}

int spider_internal_xa_rollback(
  THD* thd,
  SPIDER_TRX *trx
) {
  int error_num = 0, tmp_error_num;
  TABLE *table_xa, *table_xa_member;
  char xa_key[MAX_KEY_LENGTH];
  SPIDER_CONN *conn;
  uint force_commit = spider_param_force_commit(thd);
  MEM_ROOT mem_root;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  bool server_lost = FALSE;
  bool table_xa_opened = FALSE;
  bool table_xa_member_opened = FALSE;
  DBUG_ENTER("spider_internal_xa_rollback");

  if (trx->trx_xa_prepared)
  {
    /*
      select
        status
      from
        mysql.spider_xa
      where
        format_id = xid->format_id and
        gtrid_length = xid->gtrid_length and
        data = xid->data
    */
    if (
      !(table_xa = spider_open_sys_table(
        thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
        TRUE, &open_tables_backup, TRUE, &error_num))
    )
      goto error_open_table;
    table_xa_opened = TRUE;
    spider_store_xa_pk(table_xa, &trx->xid);
    if (
      (error_num = spider_check_sys_table(table_xa, xa_key))
    ) {
      if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
      {
        table_xa->file->print_error(error_num, MYF(0));
        goto error;
      }
      my_message(ER_SPIDER_XA_NOT_EXISTS_NUM, ER_SPIDER_XA_NOT_EXISTS_STR,
        MYF(0));
      error_num = ER_SPIDER_XA_NOT_EXISTS_NUM;
      goto error;
    }
    SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
    if (
      force_commit != 2 &&
      (error_num = spider_check_sys_xa_status(
        table_xa,
        SPIDER_SYS_XA_PREPARED_STR,
        SPIDER_SYS_XA_ROLLBACK_STR,
        NULL,
        ER_SPIDER_XA_NOT_PREPARED_NUM,
        &mem_root))
    ) {
      free_root(&mem_root, MYF(0));
      if (error_num == ER_SPIDER_XA_NOT_PREPARED_NUM)
        my_message(error_num, ER_SPIDER_XA_NOT_PREPARED_STR, MYF(0));
      goto error;
    }
    free_root(&mem_root, MYF(0));

    /*
      update
        mysql.spider_xa
      set
        status = 'COMMIT'
      where
        format_id = trx->xid.format_id and
        gtrid_length = trx->xid.gtrid_length and
        data = trx->xid.data
    */
    if (
      (error_num = spider_update_xa(
        table_xa, &trx->xid, SPIDER_SYS_XA_ROLLBACK_STR))
    )
      goto error;
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
    table_xa_opened = FALSE;
  }

  SPIDER_BACKUP_DASTATUS;
  if ((conn = spider_tree_first(trx->join_trx_top)))
  {
    do {
      if (conn->bg_search)
        spider_bg_conn_break(conn, NULL);
      if (conn->join_trx)
      {
        if (conn->disable_xa)
        {
          if (conn->table_lock != 3 && !trx->trx_xa_prepared)
          {
            if (
              !conn->server_lost &&
              (tmp_error_num = spider_db_rollback(conn))
            ) {
              SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
              if (!error_num && tmp_error_num)
                error_num = tmp_error_num;
            }
          }
        } else {
          if (!conn->server_lost)
          {
            if (
              !trx->trx_xa_prepared &&
              (tmp_error_num = spider_db_xa_end(conn, &trx->xid))
            ) {
              if (
                force_commit == 0 ||
                (force_commit == 1 &&
                  (
                    tmp_error_num != ER_XAER_NOTA &&
                    tmp_error_num != ER_XA_RBTIMEOUT &&
                    tmp_error_num != ER_XA_RBDEADLOCK
                  )
                )
              ) {
                SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
                if (!error_num && tmp_error_num)
                  error_num = tmp_error_num;
              }
            }
            if ((tmp_error_num = spider_db_xa_rollback(conn, &trx->xid)))
            {
              if (
                force_commit == 0 ||
                (force_commit == 1 &&
                  (
                    tmp_error_num != ER_XAER_NOTA &&
                    tmp_error_num != ER_XA_RBTIMEOUT &&
                    tmp_error_num != ER_XA_RBDEADLOCK
                  )
                )
              ) {
                SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
                if (!error_num && tmp_error_num)
                  error_num = tmp_error_num;
              }
            }
          }
        }
        if ((tmp_error_num = spider_end_trx(trx, conn)))
        {
          SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
          if (!error_num && tmp_error_num)
            error_num = tmp_error_num;
        }
        conn->join_trx = 0;
        if (conn->server_lost)
          server_lost = TRUE;
      }
    } while ((conn = spider_tree_next(conn)));
    trx->join_trx_top = NULL;
  }
  if (error_num)
    goto error_in_rollback;

  if (
    trx->trx_xa_prepared &&
    !server_lost
  ) {
    /*
      delete from
        mysql.spider_xa_member
      where
        format_id = xid->format_id and
        gtrid_length = xid->gtrid_length and
        data = xid->data
    */
    if (
      !(table_xa_member = spider_open_sys_table(
        thd, SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
        SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
        &error_num))
    )
      goto error_open_table;
    table_xa_member_opened = TRUE;
    if ((error_num = spider_delete_xa_member(table_xa_member, &trx->xid)))
      goto error;
    spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
    table_xa_member_opened = FALSE;

    /*
      delete from
        mysql.spider_xa
      where
        format_id = xid->format_id and
        gtrid_length = xid->gtrid_length and
        data = xid->data
    */
    if (
      !(table_xa = spider_open_sys_table(
        thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
        TRUE, &open_tables_backup, TRUE, &error_num))
    )
      goto error_open_table;
    table_xa_opened = TRUE;
    if ((error_num = spider_delete_xa(table_xa, &trx->xid)))
      goto error;
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
    table_xa_opened = FALSE;
  }
  spider_xa_unlock(&trx->internal_xid_state);
  trx->internal_xid_state.xa_state = XA_NOTR;
  DBUG_RETURN(0);

error:
  if (table_xa_opened)
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  if (table_xa_member_opened)
    spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
error_in_rollback:
error_open_table:
  spider_xa_unlock(&trx->internal_xid_state);
  trx->internal_xid_state.xa_state = XA_NOTR;
  DBUG_RETURN(error_num);
}

int spider_internal_xa_prepare(
  THD* thd,
  SPIDER_TRX *trx,
  TABLE *table_xa,
  TABLE *table_xa_member,
  bool internal_xa
) {
  int error_num;
  SPIDER_CONN *conn;
  uint force_commit = spider_param_force_commit(thd);
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  bool table_xa_opened = FALSE;
  bool table_xa_member_opened = FALSE;
  DBUG_ENTER("spider_internal_xa_prepare");
  /*
    insert into mysql.spider_xa
      (format_id, gtrid_length, bqual_length, data, status) values
      (trx->xid.format_id, trx->xid.gtrid_length, trx->xid.bqual_length,
      trx->xid.data, 'NOT YET')
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  if (
    (error_num = spider_insert_xa(
      table_xa, &trx->xid, SPIDER_SYS_XA_NOT_YET_STR))
  )
    goto error;
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;

  if (
    !(table_xa_member = spider_open_sys_table(
      thd, SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
      SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
      &error_num))
  )
    goto error_open_table;
  table_xa_member_opened = TRUE;
  SPIDER_BACKUP_DASTATUS;
  if ((conn = spider_tree_first(trx->join_trx_top)))
  {
    do {
      if (conn->bg_search)
        spider_bg_conn_break(conn, NULL);
      if (conn->disable_xa)
      {
        if (conn->table_lock != 3)
        {
          if ((error_num = spider_db_rollback(conn)))
          {
            SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
            if (error_num)
              goto error;
          }
        }
        if ((error_num = spider_end_trx(trx, conn)))
        {
          SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
          if (error_num)
            goto error;
        }
        conn->join_trx = 0;
      } else {
        /*
          insert into mysql.spider_xa_member
            (format_id, gtrid_length, bqual_length, data,
            scheme, host, port, socket, username, password) values
            (trx->xid.format_id, trx->xid.gtrid_length,
            trx->xid.bqual_length, trx->xid.data,
            conn->tgt_wrapper,
            conn->tgt_host,
            conn->tgt_port,
            conn->tgt_socket,
            conn->tgt_username,
            conn->tgt_password)
        */
        if (
          (error_num = spider_insert_xa_member(
            table_xa_member, &trx->xid, conn))
        ) {
          SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
          if (error_num)
            goto error;
        }

        if ((error_num = spider_db_xa_end(conn, &trx->xid)))
        {
          if (force_commit == 0 ||
            (force_commit == 1 && error_num != ER_XAER_NOTA))
          {
            SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
            if (error_num)
              goto error;
          }
        }
        if ((error_num = spider_db_xa_prepare(conn, &trx->xid)))
        {
          if (force_commit == 0 ||
            (force_commit == 1 && error_num != ER_XAER_NOTA))
          {
            SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
            if (error_num)
              goto error;
          }
        }
/*
        if (!internal_xa)
        {
          if ((error_num = spider_end_trx(trx, conn)))
            DBUG_RETURN(error_num);
          conn->join_trx = 0;
        }
*/
      }
    } while ((conn = spider_tree_next(conn)));
/*
    if (!internal_xa)
      trx->join_trx_top = NULL;
*/
  }
  spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
  table_xa_member_opened = FALSE;

  /*
    update
      mysql.spider_xa
    set
      status = 'PREPARED'
    where
      format_id = trx->xid.format_id and
      gtrid_length = trx->xid.gtrid_length and
      data = trx->xid.data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  if (
    (error_num = spider_update_xa(
      table_xa, &trx->xid, SPIDER_SYS_XA_PREPARED_STR))
  )
    goto error;
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;
  if (internal_xa)
    trx->internal_xid_state.xa_state = XA_PREPARED;
  DBUG_RETURN(0);

error:
  if (table_xa_opened)
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  if (table_xa_member_opened)
    spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
error_open_table:
  DBUG_RETURN(error_num);
}

int spider_internal_xa_recover(
  THD* thd,
  XID* xid_list,
  uint len
) {
  TABLE *table_xa;
  int cnt = 0;
  char xa_key[MAX_KEY_LENGTH];
  MEM_ROOT mem_root;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  DBUG_ENTER("spider_internal_xa_recover");
  /*
    select
      format_id,
      gtrid_length,
      bqual_length,
      data
    from
      mysql.spider_xa
    where
      status = 'PREPARED'
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      FALSE, &open_tables_backup, TRUE, &my_errno))
  )
    goto error_open_table;
  spider_store_xa_status(table_xa, SPIDER_SYS_XA_PREPARED_STR);
  if (
    (my_errno = spider_get_sys_table_by_idx(table_xa, xa_key, 1,
    SPIDER_SYS_XA_IDX1_COL_CNT))
  ) {
    spider_sys_index_end(table_xa);
    if (my_errno != HA_ERR_KEY_NOT_FOUND && my_errno != HA_ERR_END_OF_FILE)
    {
      table_xa->file->print_error(my_errno, MYF(0));
      goto error;
    }
    goto error;
  }

  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  do {
    spider_get_sys_xid(table_xa, &xid_list[cnt], &mem_root);
    cnt++;
    my_errno = spider_sys_index_next_same(table_xa, xa_key);
  } while (my_errno == 0 && cnt < (int) len);
  free_root(&mem_root, MYF(0));
  spider_sys_index_end(table_xa);
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  DBUG_RETURN(cnt);

error:
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
error_open_table:
  DBUG_RETURN(0);
}

int spider_initinal_xa_recover(
  XID* xid_list,
  uint len
) {
  int error_num;
  static THD *thd = NULL;
  static TABLE *table_xa = NULL;
  static READ_RECORD *read_record = NULL;
#if MYSQL_VERSION_ID < 50500
  static Open_tables_state *open_tables_backup = NULL;
#else
  static Open_tables_backup *open_tables_backup = NULL;
#endif
  int cnt = 0;
  MEM_ROOT mem_root;
  DBUG_ENTER("spider_initinal_xa_recover");
  if (!open_tables_backup)
  {
#if MYSQL_VERSION_ID < 50500
    if (!(open_tables_backup = new Open_tables_state))
#else
    if (!(open_tables_backup = new Open_tables_backup))
#endif
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_create_state;
    }
  }
  if (!read_record)
  {
    if (!(read_record = new READ_RECORD))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_create_read_record;
    }
  }

/*
  if (!thd)
  {
*/
    if (!(thd = spider_create_tmp_thd()))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_create_thd;
    }
/*
  }
*/

  /*
    select
      format_id,
      gtrid_length,
      bqual_length,
      data
    from
      mysql.spider_xa
  */
  if (!table_xa)
  {
    if (
      !(table_xa = spider_open_sys_table(
        thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
        FALSE, open_tables_backup, TRUE, &error_num))
    )
      goto error_open_table;
    init_read_record(read_record, thd, table_xa, NULL, TRUE, FALSE, FALSE);
  }
  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  while ((!(read_record->read_record(read_record))) && cnt < (int) len)
  {
    spider_get_sys_xid(table_xa, &xid_list[cnt], &mem_root);
    cnt++;
  }
  free_root(&mem_root, MYF(0));

/*
  if (cnt < (int) len)
  {
*/
    end_read_record(read_record);
    spider_close_sys_table(thd, table_xa, open_tables_backup, TRUE);
    table_xa = NULL;
    spider_free_tmp_thd(thd);
    thd = NULL;
    delete read_record;
    read_record = NULL;
    delete open_tables_backup;
    open_tables_backup = NULL;
/*
  }
*/
  DBUG_RETURN(cnt);

/*
error:
  end_read_record(&read_record_info);
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa = NULL;
*/
error_open_table:
  spider_free_tmp_thd(thd);
  thd = NULL;
error_create_thd:
  delete read_record;
  read_record = NULL;
error_create_read_record:
  delete open_tables_backup;
  open_tables_backup = NULL;
error_create_state:
  DBUG_RETURN(0);
}

int spider_internal_xa_commit_by_xid(
  THD* thd,
  SPIDER_TRX *trx,
  XID* xid
) {
  TABLE *table_xa, *table_xa_member;
  int error_num;
  char xa_key[MAX_KEY_LENGTH];
  char xa_member_key[MAX_KEY_LENGTH];
  SPIDER_SHARE tmp_share;
  char *tmp_connect_info[SPIDER_TMP_SHARE_CHAR_PTR_COUNT];
  uint tmp_connect_info_length[SPIDER_TMP_SHARE_UINT_COUNT];
  long tmp_long[SPIDER_TMP_SHARE_LONG_COUNT];
  longlong tmp_longlong[SPIDER_TMP_SHARE_LONGLONG_COUNT];
  SPIDER_CONN *conn;
  uint force_commit = spider_param_force_commit(thd);
  MEM_ROOT mem_root;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  bool table_xa_opened = FALSE;
  bool table_xa_member_opened = FALSE;
  DBUG_ENTER("spider_internal_xa_commit_by_xid");
  /*
    select
      status
    from
      mysql.spider_xa
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  spider_store_xa_pk(table_xa, xid);
  if (
    (error_num = spider_check_sys_table(table_xa, xa_key))
  ) {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table_xa->file->print_error(error_num, MYF(0));
      goto error;
    }
    my_message(ER_SPIDER_XA_NOT_EXISTS_NUM, ER_SPIDER_XA_NOT_EXISTS_STR,
      MYF(0));
    error_num = ER_SPIDER_XA_NOT_EXISTS_NUM;
    goto error;
  }
  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  if (
    force_commit != 2 &&
    (error_num = spider_check_sys_xa_status(
      table_xa,
      SPIDER_SYS_XA_PREPARED_STR,
      SPIDER_SYS_XA_COMMIT_STR,
      NULL,
      ER_SPIDER_XA_NOT_PREPARED_NUM,
      &mem_root))
  ) {
    free_root(&mem_root, MYF(0));
    if (error_num == ER_SPIDER_XA_NOT_PREPARED_NUM)
      my_message(error_num, ER_SPIDER_XA_NOT_PREPARED_STR, MYF(0));
    goto error;
  }

  /*
    update
      mysql.spider_xa
    set
      status = 'COMMIT'
    where
      format_id = trx->xid.format_id and
      gtrid_length = trx->xid.gtrid_length and
      data = trx->xid.data
  */
  if (
    (error_num = spider_update_xa(
      table_xa, xid, SPIDER_SYS_XA_COMMIT_STR))
  ) {
    free_root(&mem_root, MYF(0));
    goto error;
  }
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;

  /*
    select
      scheme tmp_share.tgt_wrappers,
      host tmp_share.tgt_hosts,
      port tmp_share.tgt_ports,
      socket tmp_share.tgt_sockets,
      username tmp_share.tgt_usernames,
      password tmp_share.tgt_passwords
    from
      mysql.spider_xa_member
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa_member = spider_open_sys_table(
      thd, SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
      SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
      &error_num))
  ) {
    free_root(&mem_root, MYF(0));
    goto error_open_table;
  }
  table_xa_member_opened = TRUE;
  spider_store_xa_pk(table_xa_member, xid);
  if (
    (error_num = spider_get_sys_table_by_idx(table_xa_member, xa_member_key, 0,
    SPIDER_SYS_XA_PK_COL_CNT))
  ) {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      free_root(&mem_root, MYF(0));
      table_xa_member->file->print_error(error_num, MYF(0));
      goto error;
    } else {
      free_root(&mem_root, MYF(0));
      spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
      table_xa_member_opened = FALSE;
      goto xa_delete;
    }
  }

  memset(&tmp_share, 0, sizeof(SPIDER_SHARE));
  memset(&tmp_connect_info, 0,
    sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT);
  spider_set_tmp_share_pointer(&tmp_share, tmp_connect_info,
    tmp_connect_info_length, tmp_long, tmp_longlong);
  do {
    SPIDER_BACKUP_DASTATUS;
    spider_get_sys_server_info(table_xa_member, &tmp_share, 0, &mem_root);
    if ((error_num = spider_create_conn_keys(&tmp_share)))
    {
      spider_sys_index_end(table_xa_member);
      free_root(&mem_root, MYF(0));
      goto error;
    }

    if (
      !(conn = spider_get_conn(
        &tmp_share, 0, tmp_share.conn_keys[0], trx, NULL, FALSE, FALSE,
        SPIDER_CONN_KIND_MYSQL, &error_num)) &&
      (force_commit == 0 ||
        (force_commit == 1 && error_num != ER_XAER_NOTA))
    ) {
      spider_sys_index_end(table_xa_member);
      spider_free_tmp_share_alloc(&tmp_share);
      free_root(&mem_root, MYF(0));
      goto error;
    }
    conn->error_mode &= spider_param_error_read_mode(thd, 0);
    conn->error_mode &= spider_param_error_write_mode(thd, 0);
    if (
      (error_num = spider_db_xa_commit(conn, xid)) &&
      (force_commit == 0 ||
        (force_commit == 1 && error_num != ER_XAER_NOTA))
    ) {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
      {
        spider_sys_index_end(table_xa_member);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        goto error;
      }
    }
    spider_free_tmp_share_alloc(&tmp_share);
    error_num = spider_sys_index_next_same(table_xa_member, xa_member_key);
  } while (error_num == 0);
  if ((error_num = spider_sys_index_end(table_xa_member)))
  {
    free_root(&mem_root, MYF(0));
    goto error;
  }
  free_root(&mem_root, MYF(0));
  spider_reuse_trx_ha(trx);
  spider_free_trx_conn(trx, FALSE);

  /*
    delete from
      mysql.spider_xa_member
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if ((error_num = spider_delete_xa_member(table_xa_member, xid)))
    goto error;
  spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
  table_xa_member_opened = FALSE;

xa_delete:
  /*
    delete from
      mysql.spider_xa
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  if ((error_num = spider_delete_xa(table_xa, xid)))
    goto error;
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;
  DBUG_RETURN(0);

error:
  if (table_xa_opened)
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  if (table_xa_member_opened)
    spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
error_open_table:
  DBUG_RETURN(error_num);
}

int spider_internal_xa_rollback_by_xid(
  THD* thd,
  SPIDER_TRX *trx,
  XID* xid
) {
  TABLE *table_xa, *table_xa_member;
  int error_num;
  char xa_key[MAX_KEY_LENGTH];
  char xa_member_key[MAX_KEY_LENGTH];
  SPIDER_SHARE tmp_share;
  char *tmp_connect_info[SPIDER_TMP_SHARE_CHAR_PTR_COUNT];
  uint tmp_connect_info_length[SPIDER_TMP_SHARE_UINT_COUNT];
  long tmp_long[SPIDER_TMP_SHARE_LONG_COUNT];
  longlong tmp_longlong[SPIDER_TMP_SHARE_LONGLONG_COUNT];
  SPIDER_CONN *conn;
  uint force_commit = spider_param_force_commit(thd);
  MEM_ROOT mem_root;
#if MYSQL_VERSION_ID < 50500
  Open_tables_state open_tables_backup;
#else
  Open_tables_backup open_tables_backup;
#endif
  bool table_xa_opened = FALSE;
  bool table_xa_member_opened = FALSE;
  DBUG_ENTER("spider_internal_xa_rollback_by_xid");
  /*
    select
      status
    from
      mysql.spider_xa
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  spider_store_xa_pk(table_xa, xid);
  if (
    (error_num = spider_check_sys_table(table_xa, xa_key))
  ) {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table_xa->file->print_error(error_num, MYF(0));
      goto error;
    }
    error_num = ER_SPIDER_XA_NOT_EXISTS_NUM;
    goto error;
  }
  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  if (
    force_commit != 2 &&
    (error_num = spider_check_sys_xa_status(
      table_xa,
      SPIDER_SYS_XA_NOT_YET_STR,
      SPIDER_SYS_XA_PREPARED_STR,
      SPIDER_SYS_XA_ROLLBACK_STR,
      ER_SPIDER_XA_PREPARED_NUM,
      &mem_root))
  ) {
    free_root(&mem_root, MYF(0));
    if (error_num == ER_SPIDER_XA_PREPARED_NUM)
      my_message(error_num, ER_SPIDER_XA_PREPARED_STR, MYF(0));
    goto error;
  }

  /*
    update
      mysql.spider_xa
    set
      status = 'ROLLBACK'
    where
      format_id = trx->xid.format_id and
      gtrid_length = trx->xid.gtrid_length and
      data = trx->xid.data
  */
  if (
    (error_num = spider_update_xa(
      table_xa, xid, SPIDER_SYS_XA_ROLLBACK_STR))
  ) {
    free_root(&mem_root, MYF(0));
    goto error;
  }
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;

  /*
    select
      scheme tmp_share.tgt_wrappers,
      host tmp_share.tgt_hosts,
      port tmp_share.tgt_ports,
      socket tmp_share.tgt_sockets,
      username tmp_share.tgt_usernames,
      password tmp_share.tgt_passwords
    from
      mysql.spider_xa_member
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa_member = spider_open_sys_table(
      thd, SPIDER_SYS_XA_MEMBER_TABLE_NAME_STR,
      SPIDER_SYS_XA_MEMBER_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
      &error_num))
  ) {
    free_root(&mem_root, MYF(0));
    goto error_open_table;
  }
  table_xa_member_opened = TRUE;
  spider_store_xa_pk(table_xa_member, xid);
  if (
    (error_num = spider_get_sys_table_by_idx(table_xa_member, xa_member_key, 0,
    SPIDER_SYS_XA_PK_COL_CNT))
  ) {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      free_root(&mem_root, MYF(0));
      table_xa_member->file->print_error(error_num, MYF(0));
      goto error;
    } else {
      free_root(&mem_root, MYF(0));
      spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
      table_xa_member_opened = FALSE;
      goto xa_delete;
    }
  }

  memset(&tmp_share, 0, sizeof(SPIDER_SHARE));
  memset(&tmp_connect_info, 0,
    sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT);
  spider_set_tmp_share_pointer(&tmp_share, tmp_connect_info,
    tmp_connect_info_length, tmp_long, tmp_longlong);
  do {
    SPIDER_BACKUP_DASTATUS;
    spider_get_sys_server_info(table_xa_member, &tmp_share, 0, &mem_root);
    if ((error_num = spider_create_conn_keys(&tmp_share)))
    {
      spider_sys_index_end(table_xa_member);
      free_root(&mem_root, MYF(0));
      goto error;
    }

    if (
      !(conn = spider_get_conn(
        &tmp_share, 0, tmp_share.conn_keys[0], trx, NULL, FALSE, FALSE,
        SPIDER_CONN_KIND_MYSQL, &error_num)) &&
      (force_commit == 0 ||
        (force_commit == 1 && error_num != ER_XAER_NOTA))
    ) {
      spider_sys_index_end(table_xa_member);
      spider_free_tmp_share_alloc(&tmp_share);
      free_root(&mem_root, MYF(0));
      goto error;
    }
    conn->error_mode &= spider_param_error_read_mode(thd, 0);
    conn->error_mode &= spider_param_error_write_mode(thd, 0);
    if (
      (error_num = spider_db_xa_rollback(conn, xid)) &&
      (force_commit == 0 ||
        (force_commit == 1 && error_num != ER_XAER_NOTA))
    ) {
      SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_ERROR_NUM;
      if (error_num)
      {
        spider_sys_index_end(table_xa_member);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        goto error;
      }
    }
    spider_free_tmp_share_alloc(&tmp_share);
    error_num = spider_sys_index_next_same(table_xa_member, xa_member_key);
  } while (error_num == 0);
  if ((error_num = spider_sys_index_end(table_xa_member)))
  {
    free_root(&mem_root, MYF(0));
    goto error;
  }
  free_root(&mem_root, MYF(0));
  spider_reuse_trx_ha(trx);
  spider_free_trx_conn(trx, FALSE);

  /*
    delete from
      mysql.spider_xa_member
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if ((error_num = spider_delete_xa_member(table_xa_member, xid)))
    goto error;
  spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
  table_xa_member_opened = FALSE;

xa_delete:
  /*
    delete from
      mysql.spider_xa
    where
      format_id = xid->format_id and
      gtrid_length = xid->gtrid_length and
      data = xid->data
  */
  if (
    !(table_xa = spider_open_sys_table(
      thd, SPIDER_SYS_XA_TABLE_NAME_STR, SPIDER_SYS_XA_TABLE_NAME_LEN,
      TRUE, &open_tables_backup, TRUE, &error_num))
  )
    goto error_open_table;
  table_xa_opened = TRUE;
  if ((error_num = spider_delete_xa(table_xa, xid)))
    goto error;
  spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  table_xa_opened = FALSE;
  DBUG_RETURN(0);

error:
  if (table_xa_opened)
    spider_close_sys_table(thd, table_xa, &open_tables_backup, TRUE);
  if (table_xa_member_opened)
    spider_close_sys_table(thd, table_xa_member, &open_tables_backup, TRUE);
error_open_table:
  DBUG_RETURN(error_num);
}

int spider_start_consistent_snapshot(
  handlerton *hton,
  THD* thd
) {
  int error_num;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_start_consistent_snapshot");

  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
    DBUG_RETURN(error_num);
  if (spider_param_use_consistent_snapshot(trx->thd))
  {
    if (spider_param_internal_xa(trx->thd) &&
      spider_param_internal_xa_snapshot(trx->thd) == 1)
    {
      error_num = ER_SPIDER_CANT_USE_BOTH_INNER_XA_AND_SNAPSHOT_NUM;
      my_message(error_num, ER_SPIDER_CANT_USE_BOTH_INNER_XA_AND_SNAPSHOT_STR,
        MYF(0));
      goto error;
    } else {
      trx->trx_consistent_snapshot = TRUE;
      trx->use_consistent_snapshot = TRUE;
      trx->internal_xa_snapshot = spider_param_internal_xa_snapshot(trx->thd);
      trans_register_ha(trx->thd, FALSE, spider_hton_ptr);
      trans_register_ha(trx->thd, TRUE, spider_hton_ptr);
      if (spider_param_use_all_conns_snapshot(trx->thd))
      {
        trx->internal_xa = FALSE;
        if ((error_num = spider_open_all_tables(trx, TRUE)))
          goto error_open_all_tables;
        if (
          spider_param_use_snapshot_with_flush_tables(trx->thd) == 1 &&
          (error_num = spider_trx_all_flush_tables(trx))
        )
          goto error_trx_all_flush_tables;
        if (spider_param_use_snapshot_with_flush_tables(trx->thd) == 2)
        {
          if ((error_num = spider_trx_another_lock_tables(trx)))
            goto error_trx_another_lock_tables;
          if ((error_num = spider_trx_another_flush_tables(trx)))
            goto error_trx_another_flush_tables;
        }
        if ((error_num = spider_trx_all_start_trx(trx)))
          goto error_trx_all_start_trx;
        if (spider_param_use_snapshot_with_flush_tables(trx->thd) == 1)
        {
          if (
            spider_param_use_flash_logs(trx->thd) &&
            (error_num = spider_trx_all_flush_logs(trx))
          )
            goto error_trx_all_flush_logs;
          if ((error_num = spider_trx_all_unlock_tables(trx)))
            goto error_trx_all_unlock_tables;
        }
        if (spider_param_use_snapshot_with_flush_tables(trx->thd) == 2)
        {
          if (
            spider_param_use_flash_logs(trx->thd) &&
            (error_num = spider_trx_all_flush_logs(trx))
          )
            goto error_trx_all_flush_logs2;
          if ((error_num = spider_free_trx_another_conn(trx, TRUE)))
            goto error_free_trx_another_conn;
        }
      } else
        trx->internal_xa = spider_param_internal_xa(trx->thd);
    }
  }

  DBUG_RETURN(0);

error_trx_all_flush_logs:
error_trx_all_start_trx:
error_trx_another_flush_tables:
error_trx_another_lock_tables:
error_trx_all_flush_tables:
  if (spider_param_use_snapshot_with_flush_tables(trx->thd) == 1)
    spider_trx_all_unlock_tables(trx);
error_trx_all_flush_logs2:
error_trx_all_unlock_tables:
error_open_all_tables:
  if (spider_param_use_snapshot_with_flush_tables(trx->thd) == 2)
    spider_free_trx_another_conn(trx, TRUE);
error_free_trx_another_conn:
error:
  DBUG_RETURN(error_num);
}

int spider_commit(
  handlerton *hton,
  THD *thd,
  bool all
) {
  SPIDER_TRX *trx;
  TABLE *table_xa = NULL;
  TABLE *table_xa_member = NULL;
  int error_num = 0;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_commit");

  if (!(trx = (SPIDER_TRX*) *thd_ha_data(thd, spider_hton_ptr)))
    DBUG_RETURN(0); /* transaction is not started */

#ifdef HA_CAN_BULK_ACCESS
  DBUG_PRINT("info",("spider trx->bulk_access_conn_first=%p",
    trx->bulk_access_conn_first));
  trx->bulk_access_conn_first = NULL;
#endif

  if (all || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)))
  {
    if (trx->trx_start)
    {
      if (trx->trx_xa)
      {
        if (trx->internal_xa && !trx->trx_xa_prepared)
        {
          if (
            (error_num = spider_internal_xa_prepare(
              thd, trx, table_xa, table_xa_member, TRUE))
          ) {
/*
            if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
            {
*/
              /* rollback for semi_trx */
              spider_rollback(hton, thd, all);
/*
            }
*/
            DBUG_RETURN(error_num);
          }
          trx->trx_xa_prepared = TRUE;
        }
        int tmp_error_num;
        if (
          (tmp_error_num = spider_internal_xa_commit(
            thd, trx, &trx->xid, table_xa, table_xa_member))
        ) {
          if (tmp_error_num)
            error_num = tmp_error_num;
        }
        trx->trx_xa = FALSE;
        trx->join_trx_top = NULL;
      } else {
        if ((conn = spider_tree_first(trx->join_trx_top)))
        {
          SPIDER_BACKUP_DASTATUS;
          int tmp_error_num;
          do {
            if (
              (conn->autocommit != 1 || conn->trx_start) &&
              (tmp_error_num = spider_db_commit(conn))
            ) {
              SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
              if (tmp_error_num)
                error_num = tmp_error_num;
            }
            if ((tmp_error_num = spider_end_trx(trx, conn)))
            {
              SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
              if (tmp_error_num)
                error_num = tmp_error_num;
            }
            conn->join_trx = 0;
          } while ((conn = spider_tree_next(conn)));
          trx->join_trx_top = NULL;
        }
      }
      trx->trx_start = FALSE;
      DBUG_PRINT("info",("spider trx->trx_start=FALSE"));
    }
    spider_reuse_trx_ha(trx);
    spider_free_trx_conn(trx, FALSE);
    trx->trx_consistent_snapshot = FALSE;
  }
  spider_merge_mem_calc(trx, FALSE);
  DBUG_RETURN(error_num);
}

int spider_rollback(
  handlerton *hton,
  THD *thd,
  bool all
) {
  SPIDER_TRX *trx;
  int error_num = 0;
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_rollback");

  if (!(trx = (SPIDER_TRX*) *thd_ha_data(thd, spider_hton_ptr)))
    DBUG_RETURN(0); /* transaction is not started */

#ifdef HA_CAN_BULK_ACCESS
  DBUG_PRINT("info",("spider trx->bulk_access_conn_first=%p",
    trx->bulk_access_conn_first));
  trx->bulk_access_conn_first = NULL;
#endif

  if (all || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)))
  {
    if (trx->trx_start)
    {
      if (trx->trx_xa)
      {
        int tmp_error_num;
        if (
          (tmp_error_num = spider_internal_xa_rollback(thd, trx))
        ) {
          if (tmp_error_num)
            error_num = tmp_error_num;
        }
        trx->trx_xa = FALSE;
        trx->join_trx_top = NULL;
      } else {
        if ((conn = spider_tree_first(trx->join_trx_top)))
        {
          SPIDER_BACKUP_DASTATUS;
          int tmp_error_num;
          do {
            if (
              !conn->server_lost &&
              (conn->autocommit != 1 || conn->trx_start) &&
              (tmp_error_num = spider_db_rollback(conn))
            ) {
              SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
              if (tmp_error_num)
                error_num = tmp_error_num;
            }
            if ((tmp_error_num = spider_end_trx(trx, conn)))
            {
              SPIDER_CONN_RESTORE_DASTATUS_AND_RESET_TMP_ERROR_NUM;
              if (tmp_error_num)
                error_num = tmp_error_num;
            }
            conn->join_trx = 0;
          } while ((conn = spider_tree_next(conn)));
          trx->join_trx_top = NULL;
        }
      }
      trx->trx_start = FALSE;
      DBUG_PRINT("info",("spider trx->trx_start=FALSE"));
    }
    spider_reuse_trx_ha(trx);
    spider_free_trx_conn(trx, FALSE);
    trx->trx_consistent_snapshot = FALSE;
  }

  spider_merge_mem_calc(trx, FALSE);
  DBUG_RETURN(error_num);
}

int spider_xa_prepare(
  handlerton *hton,
  THD* thd,
  bool all
) {
  int error_num;
  SPIDER_TRX *trx;
  TABLE *table_xa = NULL;
  TABLE *table_xa_member = NULL;
  DBUG_ENTER("spider_xa_prepare");

  if (all || (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)))
  {
    if (!(trx = (SPIDER_TRX*) *thd_ha_data(thd, spider_hton_ptr)))
      DBUG_RETURN(0); /* transaction is not started */

    DBUG_PRINT("info",("spider trx_start=%s",
      trx->trx_start ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider trx_xa=%s",
      trx->trx_xa ? "TRUE" : "FALSE"));
    if (trx->trx_start && trx->trx_xa)
    {
      if ((error_num = spider_internal_xa_prepare(
        thd, trx, table_xa, table_xa_member, FALSE)))
        goto error;
      trx->trx_xa_prepared = TRUE;
    }
  }

  DBUG_RETURN(0);

error:
  DBUG_RETURN(error_num);
}

int spider_xa_recover(
  handlerton *hton,
  XID* xid_list,
  uint len
) {
  THD* thd = current_thd;
  DBUG_ENTER("spider_xa_recover");
  if (len == 0 || xid_list == NULL)
    DBUG_RETURN(0);

  if (thd)
    DBUG_RETURN(spider_internal_xa_recover(thd, xid_list, len));
  else
    DBUG_RETURN(spider_initinal_xa_recover(xid_list, len));
}

int spider_xa_commit_by_xid(
  handlerton *hton,
  XID* xid
) {
  SPIDER_TRX *trx;
  int error_num;
  THD* thd = current_thd;
  DBUG_ENTER("spider_xa_commit_by_xid");

  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
    goto error_get_trx;

  if ((error_num = spider_internal_xa_commit_by_xid(thd, trx, xid)))
    goto error;

  DBUG_RETURN(0);

error:
error_get_trx:
  DBUG_RETURN(error_num);
}

int spider_xa_rollback_by_xid(
  handlerton *hton,
  XID* xid
) {
  SPIDER_TRX *trx;
  int error_num;
  THD* thd = current_thd;
  DBUG_ENTER("spider_xa_rollback_by_xid");

  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
    goto error_get_trx;

  if ((error_num = spider_internal_xa_rollback_by_xid(thd, trx, xid)))
    goto error;

  DBUG_RETURN(0);

error:
error_get_trx:
  DBUG_RETURN(error_num);
}

void spider_copy_table_free_trx_conn(
  SPIDER_TRX *trx
) {
  SPIDER_CONN *conn;
  DBUG_ENTER("spider_copy_table_free_trx_conn");
  if ((conn = spider_tree_first(trx->join_trx_top)))
  {
    do {
      spider_end_trx(trx, conn);
      conn->join_trx = 0;
    } while ((conn = spider_tree_next(conn)));
    trx->join_trx_top = NULL;
  }
  spider_reuse_trx_ha(trx);
  spider_free_trx_conn(trx, FALSE);
  trx->trx_consistent_snapshot = FALSE;
  spider_merge_mem_calc(trx, FALSE);
  DBUG_VOID_RETURN;
}

int spider_end_trx(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn
) {
  int error_num = 0, need_mon = 0;
  DBUG_ENTER("spider_end_trx");
  if (conn->table_lock == 3)
  {
    trx->tmp_spider->conns = &conn;
    conn->table_lock = 0;
    conn->disable_reconnect = FALSE;
    if (
      !conn->server_lost &&
      (error_num = spider_db_unlock_tables(trx->tmp_spider, 0))
    ) {
      if (error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM)
        error_num = 0;
    }
  } else if (!conn->table_lock)
    conn->disable_reconnect = FALSE;
  if (
    conn->semi_trx_isolation >= 0 &&
    conn->trx_isolation != conn->semi_trx_isolation
  ) {
    DBUG_PRINT("info",("spider conn=%p", conn));
    DBUG_PRINT("info",("spider conn->trx_isolation=%d", conn->trx_isolation));
    if (
      !conn->server_lost &&
      !conn->queued_semi_trx_isolation &&
      (error_num = spider_db_set_trx_isolation(
        conn, conn->trx_isolation, &need_mon))
    ) {
      if (
        !conn->disable_reconnect &&
        error_num == ER_SPIDER_REMOTE_SERVER_GONE_AWAY_NUM
      )
        error_num = 0;
    }
  }
  conn->semi_trx_isolation = -2;
  conn->semi_trx_isolation_chk = FALSE;
  conn->semi_trx_chk = FALSE;
  DBUG_RETURN(error_num);
}

int spider_check_trx_and_get_conn(
  THD *thd,
  ha_spider *spider,
  bool use_conn_kind
) {
  int error_num, roop_count, search_link_idx;
  SPIDER_TRX *trx;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  char first_byte, first_byte_bak;
  int semi_table_lock_conn = spider_param_semi_table_lock_connection(thd,
    share->semi_table_lock_conn);
  DBUG_ENTER("spider_check_trx_and_get_conn");
  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
  {
    DBUG_PRINT("info",("spider get trx error"));
    DBUG_RETURN(error_num);
  }
  spider->trx = trx;
  spider->set_error_mode();
  if (
    spider->sql_command != SQLCOM_DROP_TABLE &&
    spider->sql_command != SQLCOM_ALTER_TABLE
  ) {
    SPIDER_TRX_HA *trx_ha = spider_check_trx_ha(trx, spider);
    if (!trx_ha || trx_ha->wait_for_reusing)
      spider_trx_set_link_idx_for_all(spider);

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    if (use_conn_kind)
    {
      for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
      {
        if (
/*
          spider->conn_kind[roop_count] != SPIDER_CONN_KIND_MYSQL &&
*/
          share->hs_dbton_ids[spider->conn_link_idx[roop_count]] ==
            SPIDER_DBTON_SIZE
        ) {
          /* can't use hs interface */
          spider->conn_kind[roop_count] = SPIDER_CONN_KIND_MYSQL;
          spider_clear_bit(spider->do_hs_direct_update, roop_count);
        }
      }
    }
#endif
#endif

    if (semi_table_lock_conn)
      first_byte = '0' +
        spider_param_semi_table_lock(thd, share->semi_table_lock);
    else
      first_byte = '0';
    DBUG_PRINT("info",("spider semi_table_lock_conn = %d",
      semi_table_lock_conn));
    DBUG_PRINT("info",("spider semi_table_lock = %d",
      spider_param_semi_table_lock(thd, share->semi_table_lock)));
    DBUG_PRINT("info",("spider first_byte = %d", first_byte));
    if (
      !trx_ha ||
      trx_ha->wait_for_reusing ||
      trx->spider_thread_id != spider->spider_thread_id ||
      trx->trx_conn_adjustment != spider->trx_conn_adjustment ||
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      (use_conn_kind &&
        (
          trx->trx_hs_r_conn_adjustment != spider->trx_hs_r_conn_adjustment ||
          trx->trx_hs_w_conn_adjustment != spider->trx_hs_w_conn_adjustment
        )
      ) ||
#endif
      first_byte != *spider->conn_keys[0] ||
      share->link_statuses[spider->conn_link_idx[spider->search_link_idx]] ==
        SPIDER_LINK_STATUS_NG
    ) {
      DBUG_PRINT("info",(first_byte != *spider->conn_keys[0] ?
        "spider change conn type" : trx != spider->trx ? "spider change thd" :
        "spider next trx"));
      spider->trx = trx;
      spider->trx_conn_adjustment = trx->trx_conn_adjustment;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      if (use_conn_kind)
      {
        spider->trx_hs_r_conn_adjustment = trx->trx_hs_r_conn_adjustment;
        spider->trx_hs_w_conn_adjustment = trx->trx_hs_w_conn_adjustment;
      }
#endif
      if (
        spider->spider_thread_id != trx->spider_thread_id ||
        spider->search_link_query_id != thd->query_id
      ) {
        search_link_idx = spider_conn_first_link_idx(thd,
          share->link_statuses, share->access_balances, spider->conn_link_idx,
          share->link_count, SPIDER_LINK_STATUS_OK);
        if (search_link_idx == -1)
        {
          TABLE *table = spider->get_table();
          TABLE_SHARE *table_share = table->s;
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
          char *db, *table_name;
          if (!(db = (char *)
            spider_bulk_malloc(spider_current_trx, 57, MYF(MY_WME),
              &db, table_share->db.length + 1,
              &table_name, table_share->table_name.length + 1,
              NullS))
          ) {
            my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
#else
          char db[table_share->db.length + 1],
            table_name[table_share->table_name.length + 1];
#endif
          memcpy(db, table_share->db.str, table_share->db.length);
          db[table_share->db.length] = '\0';
          memcpy(table_name, table_share->table_name.str,
            table_share->table_name.length);
          table_name[table_share->table_name.length] = '\0';
          my_printf_error(ER_SPIDER_ALL_LINKS_FAILED_NUM,
            ER_SPIDER_ALL_LINKS_FAILED_STR, MYF(0), db, table_name);
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
          spider_free(trx, db, MYF(MY_WME));
#endif
          DBUG_RETURN(ER_SPIDER_ALL_LINKS_FAILED_NUM);
        }
        spider->search_link_idx = search_link_idx;
        spider->search_link_query_id = thd->query_id;
      }
      spider->spider_thread_id = trx->spider_thread_id;

      first_byte_bak = *spider->conn_keys[0];
      *spider->conn_keys[0] = first_byte;
      for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
      {
        if (!spider->handler_opened(roop_count, SPIDER_CONN_KIND_MYSQL))
          spider->conns[roop_count] = NULL;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        if (!spider->handler_opened(roop_count, SPIDER_CONN_KIND_HS_READ))
          spider->hs_r_conns[roop_count] = NULL;
        if (!spider->handler_opened(roop_count, SPIDER_CONN_KIND_HS_WRITE))
          spider->hs_w_conns[roop_count] = NULL;
#endif
      }
      bool search_link_idx_is_checked = FALSE;
      for (
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_count < (int) share->link_count;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        uint tgt_conn_kind = (use_conn_kind ? spider->conn_kind[roop_count] :
          SPIDER_CONN_KIND_MYSQL);
        if (roop_count == spider->search_link_idx)
          search_link_idx_is_checked = TRUE;
        if (
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          (
#endif
            tgt_conn_kind == SPIDER_CONN_KIND_MYSQL &&
              !spider->conns[roop_count]
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          ) ||
          (tgt_conn_kind == SPIDER_CONN_KIND_HS_READ &&
            !spider->hs_r_conns[roop_count]) ||
          (tgt_conn_kind == SPIDER_CONN_KIND_HS_WRITE &&
            !spider->hs_w_conns[roop_count])
#endif
        ) {
          *spider->conn_keys[roop_count] = first_byte;
          if (
            !(conn =
              spider_get_conn(share, roop_count,
                spider->conn_keys[roop_count], trx,
                spider, FALSE, TRUE,
                use_conn_kind ? spider->conn_kind[roop_count] :
                  SPIDER_CONN_KIND_MYSQL,
                &error_num))
          ) {
            if (
              share->monitoring_kind[roop_count] &&
              spider->need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                trx,
                trx->thd,
                share,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
            }
            DBUG_PRINT("info",("spider get conn error"));
            *spider->conn_keys[0] = first_byte_bak;
            spider->spider_thread_id = 0;
            DBUG_RETURN(error_num);
          }
          conn->error_mode &= spider->error_mode;
        }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
        if (
          spider->do_direct_update &&
          spider_bit_is_set(spider->do_hs_direct_update, roop_count) &&
          !spider->hs_w_conns[roop_count]
        ) {
          if (
            !(conn =
              spider_get_conn(share, roop_count,
                spider->conn_keys[roop_count], trx,
                spider, FALSE, TRUE,
                SPIDER_CONN_KIND_HS_WRITE,
                &error_num))
          ) {
            if (
              share->monitoring_kind[roop_count] &&
              spider->need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                trx,
                trx->thd,
                share,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
            }
            DBUG_PRINT("info",("spider get conn error"));
            *spider->conn_keys[0] = first_byte_bak;
            spider->spider_thread_id = 0;
            DBUG_RETURN(error_num);
          }
          conn->error_mode &= spider->error_mode;
        }
#endif
#endif
      }
      if (!search_link_idx_is_checked)
      {
        TABLE *table = spider->get_table();
        TABLE_SHARE *table_share = table->s;
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
        char *db, *table_name;
        if (!(db = (char *)
          spider_bulk_malloc(spider_current_trx, 57, MYF(MY_WME),
            &db, table_share->db.length + 1,
            &table_name, table_share->table_name.length + 1,
            NullS))
        ) {
          my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
#else
        char db[table_share->db.length + 1],
          table_name[table_share->table_name.length + 1];
#endif
        memcpy(db, table_share->db.str, table_share->db.length);
        db[table_share->db.length] = '\0';
        memcpy(table_name, table_share->table_name.str,
          table_share->table_name.length);
        table_name[table_share->table_name.length] = '\0';
        my_printf_error(ER_SPIDER_LINK_MON_JUST_NG_NUM,
          ER_SPIDER_LINK_MON_JUST_NG_STR, MYF(0), db, table_name);
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
        spider_free(trx, db, MYF(MY_WME));
#endif
        DBUG_RETURN(ER_SPIDER_LINK_MON_JUST_NG_NUM);
      }
    } else {
      DBUG_PRINT("info",("spider link_status = %ld",
        share->link_statuses[spider->conn_link_idx[spider->search_link_idx]]));
      bool search_link_idx_is_checked = FALSE;
      for (
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_count < (int) share->link_count;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (roop_count == spider->search_link_idx)
          search_link_idx_is_checked = TRUE;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        if (
          !use_conn_kind ||
          spider->conn_kind[roop_count] == SPIDER_CONN_KIND_MYSQL
        ) {
#endif
          conn = spider->conns[roop_count];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        } else if (spider->conn_kind[roop_count] == SPIDER_CONN_KIND_HS_READ)
        {
          conn = spider->hs_r_conns[roop_count];
        } else {
          conn = spider->hs_w_conns[roop_count];
        }
#endif

        if (!conn)
        {
          DBUG_PRINT("info",("spider get conn %d", roop_count));
          if (
            !(conn =
              spider_get_conn(share, roop_count,
                spider->conn_keys[roop_count], trx,
                spider, FALSE, TRUE,
                use_conn_kind ? spider->conn_kind[roop_count] :
                  SPIDER_CONN_KIND_MYSQL,
                &error_num))
          ) {
            if (
              share->monitoring_kind[roop_count] &&
              spider->need_mons[roop_count]
            ) {
              error_num = spider_ping_table_mon_from_table(
                trx,
                trx->thd,
                share,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                TRUE
              );
            }
            DBUG_PRINT("info",("spider get conn error"));
            DBUG_RETURN(error_num);
          }
        }
        conn->error_mode &= spider->error_mode;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
        if (
          spider->do_direct_update &&
          spider_bit_is_set(spider->do_hs_direct_update, roop_count)
        ) {
          conn = spider->hs_w_conns[roop_count];
          if (!conn)
          {
            DBUG_PRINT("info",("spider get hs_w_conn %d", roop_count));
            if (
              !(conn =
                spider_get_conn(share, roop_count,
                  spider->conn_keys[roop_count], trx,
                  spider, FALSE, TRUE,
                  SPIDER_CONN_KIND_HS_WRITE,
                  &error_num))
            ) {
              if (
                share->monitoring_kind[roop_count] &&
                spider->need_mons[roop_count]
              ) {
                error_num = spider_ping_table_mon_from_table(
                  trx,
                  trx->thd,
                  share,
                  (uint32) share->monitoring_sid[roop_count],
                  share->table_name,
                  share->table_name_length,
                  spider->conn_link_idx[roop_count],
                  NULL,
                  0,
                  share->monitoring_kind[roop_count],
                  share->monitoring_limit[roop_count],
                  share->monitoring_flag[roop_count],
                  TRUE
                );
              }
              DBUG_PRINT("info",("spider get conn error"));
              DBUG_RETURN(error_num);
            }
          }
        }
        conn->error_mode &= spider->error_mode;
#endif
#endif
      }
      if (!search_link_idx_is_checked)
      {
        TABLE *table = spider->get_table();
        TABLE_SHARE *table_share = table->s;
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
        char *db, *table_name;
        if (!(db = (char *)
          spider_bulk_malloc(spider_current_trx, 57, MYF(MY_WME),
            &db, table_share->db.length + 1,
            &table_name, table_share->table_name.length + 1,
            NullS))
        ) {
          my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
#else
        char db[table_share->db.length + 1],
          table_name[table_share->table_name.length + 1];
#endif
        memcpy(db, table_share->db.str, table_share->db.length);
        db[table_share->db.length] = '\0';
        memcpy(table_name, table_share->table_name.str,
          table_share->table_name.length);
        table_name[table_share->table_name.length] = '\0';
        my_printf_error(ER_SPIDER_LINK_MON_JUST_NG_NUM,
          ER_SPIDER_LINK_MON_JUST_NG_STR, MYF(0), db, table_name);
#if defined(_MSC_VER) || defined(__SUNPRO_CC)
        spider_free(trx, db, MYF(MY_WME));
#endif
        DBUG_RETURN(ER_SPIDER_LINK_MON_JUST_NG_NUM);
      }
    }
    spider->set_first_link_idx();
    DBUG_RETURN(spider_create_trx_ha(trx, spider, trx_ha));
  }
  spider->spider_thread_id = trx->spider_thread_id;
  DBUG_RETURN(0);
}

THD *spider_create_tmp_thd()
{
  THD *thd;
  DBUG_ENTER("spider_create_tmp_thd");
  if (!(thd = new THD))
    DBUG_RETURN(NULL);
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
  thd->killed = NOT_KILLED;
#else
  thd->killed = THD::NOT_KILLED;
#endif
#if MYSQL_VERSION_ID < 50500
  thd->locked_tables = FALSE;
#endif
  thd->proc_info = "";
  thd->thread_id = thd->variables.pseudo_thread_id = 0;
  thd->thread_stack = (char*) &thd;
  if (thd->store_globals())
    DBUG_RETURN(NULL);
  lex_start(thd);
  DBUG_RETURN(thd);
}

void spider_free_tmp_thd(
  THD *thd
) {
  DBUG_ENTER("spider_free_tmp_thd");
  thd->cleanup();
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
  thd->reset_globals();
#else
  thd->restore_globals();
#endif
  delete thd;
  DBUG_VOID_RETURN;
}

int spider_create_trx_ha(
  SPIDER_TRX *trx,
  ha_spider *spider,
  SPIDER_TRX_HA *trx_ha
) {
  bool need_create;
  char *tmp_name;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_create_trx_ha");
  if (!trx_ha)
  {
    DBUG_PRINT("info",("spider need create"));
    need_create = TRUE;
  } else if (
    trx_ha->share != share ||
    trx_ha->link_count != share->link_count ||
    trx_ha->link_bitmap_size != share->link_bitmap_size
  ) {
    DBUG_PRINT("info",("spider need recreate"));
    need_create = TRUE;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&trx->trx_ha_hash,
      share->table_name_hash_value, (uchar*) trx_ha);
#else
    my_hash_delete(&trx->trx_ha_hash, (uchar*) trx_ha);
#endif
    spider_free(trx, trx_ha, MYF(0));
  } else {
    DBUG_PRINT("info",("spider use this"));
    trx_ha->wait_for_reusing = FALSE;
    need_create = FALSE;
  }
  if (need_create)
  {
    if (!(trx_ha = (SPIDER_TRX_HA *)
      spider_bulk_malloc(spider_current_trx, 58, MYF(MY_WME),
        &trx_ha, sizeof(SPIDER_TRX_HA),
        &tmp_name, sizeof(char *) * (share->table_name_length + 1),
        &conn_link_idx, sizeof(uint) * share->link_count,
        &conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
        NullS))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    trx_ha->table_name = tmp_name;
    memcpy(trx_ha->table_name, share->table_name, share->table_name_length);
    trx_ha->table_name[share->table_name_length] = '\0';
    trx_ha->table_name_length = share->table_name_length;
    trx_ha->trx = trx;
    trx_ha->share = share;
    trx_ha->link_count = share->link_count;
    trx_ha->link_bitmap_size = share->link_bitmap_size;
    trx_ha->conn_link_idx = conn_link_idx;
    trx_ha->conn_can_fo = conn_can_fo;
    trx_ha->wait_for_reusing = FALSE;
    uint old_elements = trx->trx_ha_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    if (my_hash_insert_with_hash_value(&trx->trx_ha_hash,
      share->table_name_hash_value, (uchar*) trx_ha))
#else
    if (my_hash_insert(&trx->trx_ha_hash, (uchar*) trx_ha))
#endif
    {
      spider_free(trx, trx_ha, MYF(0));
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    if (trx->trx_ha_hash.array.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        trx->trx_ha_hash,
        (trx->trx_ha_hash.array.max_element - old_elements) *
        trx->trx_ha_hash.array.size_of_element);
    }
  }
  memcpy(trx_ha->conn_link_idx, spider->conn_link_idx,
    sizeof(uint) * share->link_count);
  memcpy(trx_ha->conn_can_fo, spider->conn_can_fo,
    sizeof(uint) * share->link_bitmap_size);
  DBUG_RETURN(0);
}

SPIDER_TRX_HA *spider_check_trx_ha(
  SPIDER_TRX *trx,
  ha_spider *spider
) {
  SPIDER_TRX_HA *trx_ha;
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_check_trx_ha");
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((trx_ha = (SPIDER_TRX_HA *) my_hash_search_using_hash_value(
    &trx->trx_ha_hash, share->table_name_hash_value,
    (uchar*) share->table_name, share->table_name_length)))
#else
  if ((trx_ha = (SPIDER_TRX_HA *) my_hash_search(&trx->trx_ha_hash,
    (uchar*) share->table_name, share->table_name_length)))
#endif
  {
    memcpy(spider->conn_link_idx, trx_ha->conn_link_idx,
      sizeof(uint) * share->link_count);
    memcpy(spider->conn_can_fo, trx_ha->conn_can_fo,
      sizeof(uint) * share->link_bitmap_size);
    DBUG_RETURN(trx_ha);
  }
  DBUG_RETURN(NULL);
}

void spider_free_trx_ha(
  SPIDER_TRX *trx
) {
  ulong roop_count;
  SPIDER_TRX_HA *trx_ha;
  DBUG_ENTER("spider_free_trx_ha");
  for (roop_count = 0; roop_count < trx->trx_ha_hash.records; roop_count++)
  {
    trx_ha = (SPIDER_TRX_HA *) my_hash_element(&trx->trx_ha_hash, roop_count);
    spider_free(spider_current_trx, trx_ha, MYF(0));
  }
  my_hash_reset(&trx->trx_ha_hash);
  DBUG_VOID_RETURN;
}

void spider_reuse_trx_ha(
  SPIDER_TRX *trx
) {
  ulong roop_count;
  SPIDER_TRX_HA *trx_ha;
  DBUG_ENTER("spider_reuse_trx_ha");
  if (trx->trx_ha_reuse_count < 10000)
  {
    trx->trx_ha_reuse_count++;
    for (roop_count = 0; roop_count < trx->trx_ha_hash.records; roop_count++)
    {
      trx_ha = (SPIDER_TRX_HA *) my_hash_element(&trx->trx_ha_hash,
        roop_count);
      trx_ha->wait_for_reusing = TRUE;
    }
  } else {
    trx->trx_ha_reuse_count = 0;
    spider_free_trx_ha(trx);
  }
  DBUG_VOID_RETURN;
}

void spider_trx_set_link_idx_for_all(
  ha_spider *spider
) {
  int roop_count, roop_count2;
  SPIDER_SHARE *share = spider->share;
  long *link_statuses = share->link_statuses;
  uint *conn_link_idx = spider->conn_link_idx;
  int link_count = share->link_count;
  int all_link_count = share->all_link_count;
  uchar *conn_can_fo = spider->conn_can_fo;
  DBUG_ENTER("spider_trx_set_link_idx_for_all");
  DBUG_PRINT("info",("spider set link_count=%d", link_count));
  DBUG_PRINT("info",("spider set all_link_count=%d", all_link_count));
  memset(conn_can_fo, 0, sizeof(uchar) * share->link_bitmap_size);
  for (roop_count = 0; roop_count < link_count; roop_count++)
  {
    for (roop_count2 = roop_count; roop_count2 < all_link_count;
      roop_count2 += link_count)
    {
      if (link_statuses[roop_count2] <= SPIDER_LINK_STATUS_RECOVERY)
        break;
    }
    if (roop_count2 < all_link_count)
    {
      conn_link_idx[roop_count] = roop_count2;
      if (roop_count2 + link_count < all_link_count)
        spider_set_bit(conn_can_fo, roop_count);
      DBUG_PRINT("info",("spider set conn_link_idx[%d]=%d",
        roop_count, roop_count2));
    } else {
      conn_link_idx[roop_count] = roop_count;
      DBUG_PRINT("info",("spider set2 conn_link_idx[%d]=%d",
        roop_count, roop_count));
    }
    spider->conn_keys[roop_count] =
      ADD_TO_PTR(spider->conn_keys_first_ptr,
        PTR_BYTE_DIFF(share->conn_keys[conn_link_idx[roop_count]],
          share->conn_keys[0]), char*);
    DBUG_PRINT("info",("spider conn_keys[%d]=%s",
      roop_count, spider->conn_keys[roop_count]));
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    spider->hs_r_conn_keys[roop_count] =
      ADD_TO_PTR(spider->conn_keys_first_ptr,
        PTR_BYTE_DIFF(share->hs_read_conn_keys[conn_link_idx[roop_count]],
          share->conn_keys[0]), char*);
    DBUG_PRINT("info",("spider hs_r_conn_keys[%d]=%s",
      roop_count, spider->hs_r_conn_keys[roop_count]));
    spider->hs_w_conn_keys[roop_count] =
      ADD_TO_PTR(spider->conn_keys_first_ptr,
        PTR_BYTE_DIFF(share->hs_write_conn_keys[conn_link_idx[roop_count]],
          share->conn_keys[0]), char*);
    DBUG_PRINT("info",("spider hs_w_conn_keys[%d]=%s",
      roop_count, spider->hs_w_conn_keys[roop_count]));
#endif
  }
  DBUG_VOID_RETURN;
}

int spider_trx_check_link_idx_failed(
  ha_spider *spider
) {
  int roop_count;
  SPIDER_SHARE *share = spider->share;
  long *link_statuses = share->link_statuses;
  uint *conn_link_idx = spider->conn_link_idx;
  int link_count = share->link_count;
  uchar *conn_can_fo = spider->conn_can_fo;
  DBUG_ENTER("spider_trx_check_link_idx_failed");
  for (roop_count = 0; roop_count < link_count; roop_count++)
  {
    if (
      link_statuses[conn_link_idx[roop_count]] == SPIDER_LINK_STATUS_NG &&
      spider_bit_is_set(conn_can_fo, roop_count)
    ) {
      my_message(ER_SPIDER_LINK_IS_FAILOVER_NUM,
        ER_SPIDER_LINK_IS_FAILOVER_STR, MYF(0));
      DBUG_RETURN(ER_SPIDER_LINK_IS_FAILOVER_NUM);
    }
  }
  DBUG_RETURN(0);
}

#ifdef HA_CAN_BULK_ACCESS
void spider_trx_add_bulk_access_conn(
  SPIDER_TRX *trx,
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_trx_add_bulk_access_conn");
  DBUG_PRINT("info",("spider trx=%p", trx));
  DBUG_PRINT("info",("spider conn=%p", conn));
  DBUG_PRINT("info",("spider conn->bulk_access_requests=%u",
    conn->bulk_access_requests));
  DBUG_PRINT("info",("spider conn->bulk_access_sended=%u",
    conn->bulk_access_sended));
  DBUG_PRINT("info",("spider trx->bulk_access_conn_first=%p",
    trx->bulk_access_conn_first));
  if (!conn->bulk_access_requests && !conn->bulk_access_sended)
  {
    if (!trx->bulk_access_conn_first)
    {
      trx->bulk_access_conn_first = conn;
    } else {
      trx->bulk_access_conn_last->bulk_access_next = conn;
    }
    trx->bulk_access_conn_last = conn;
    conn->bulk_access_next = NULL;
  }
  conn->bulk_access_requests++;
  DBUG_PRINT("info",("spider conn->bulk_access_requests=%u",
    conn->bulk_access_requests));
  DBUG_VOID_RETURN;
}
#endif
