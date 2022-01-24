/* Copyright (C) 2008-2020 Kentoku Shiba
   Copyright (C) 2019-2022 MariaDB corp

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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "my_getopt.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "sql_servers.h"
#include "sql_select.h"
#include "tztime.h"
#include "sql_parse.h"
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
#include "spd_direct_sql.h"
#include "spd_malloc.h"
#include "spd_group_by_handler.h"
#include "spd_init_query.h"

/* Background thread management */
#ifdef SPIDER_HAS_NEXT_THREAD_ID
#define SPIDER_set_next_thread_id(A)
MYSQL_THD create_thd();
void destroy_thd(MYSQL_THD thd);
#else
ulong *spd_db_att_thread_id;
inline void SPIDER_set_next_thread_id(THD *A)
{
  pthread_mutex_lock(&LOCK_thread_count);
  A->thread_id = (*spd_db_att_thread_id)++;
  pthread_mutex_unlock(&LOCK_thread_count);
}
MYSQL_THD create_thd()
{
  THD *thd = SPIDER_new_THD(next_thread_id());
  if (thd)
  {
    thd->thread_stack = (char*) &thd;
    thd->store_globals();
    thd->set_command(COM_DAEMON);
    thd->security_ctx->host_or_ip = "";
  }
  return thd;
}
void destroy_thd(MYSQL_THD thd)
{
  delete thd;
}
#endif
inline MYSQL_THD spider_create_sys_thd(SPIDER_THREAD *thread)
{
  THD *thd = create_thd();
  if (thd)
  {
    SPIDER_set_next_thread_id(thd);
    thd->mysys_var->current_cond = &thread->cond;
    thd->mysys_var->current_mutex = &thread->mutex;
  }
  return thd;
}
inline void spider_destroy_sys_thd(MYSQL_THD thd)
{
  destroy_thd(thd);
}
inline MYSQL_THD spider_create_thd()
{
  THD *thd;
  my_thread_init();
  if (!(thd = new THD(next_thread_id())))
    my_thread_end();
  else
  {
#ifdef HAVE_PSI_INTERFACE
    mysql_thread_set_psi_id(thd->thread_id);
#endif
    thd->thread_stack = (char *) &thd;
    thd->store_globals();
  }
  return thd;
}
inline void spider_destroy_thd(MYSQL_THD thd)
{
  delete thd;
}

#ifdef SPIDER_XID_USES_xid_cache_iterate
#else
#ifdef XID_CACHE_IS_SPLITTED
uint *spd_db_att_xid_cache_split_num;
#endif
pthread_mutex_t *spd_db_att_LOCK_xid_cache;
HASH *spd_db_att_xid_cache;
#endif
struct charset_info_st *spd_charset_utf8mb3_bin;
const char **spd_defaults_extra_file;
const char **spd_defaults_file;
const char **spd_mysqld_unix_port;
uint *spd_mysqld_port;
bool volatile *spd_abort_loop;
Time_zone *spd_tz_system;
static int *spd_mysqld_server_started;
static pthread_mutex_t *spd_LOCK_server_started;
static pthread_cond_t *spd_COND_server_started;
extern long spider_conn_mutex_id;
handlerton *spider_hton_ptr;
SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
extern SPIDER_DBTON spider_dbton_mysql;
extern SPIDER_DBTON spider_dbton_mariadb;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
extern SPIDER_DBTON spider_dbton_handlersocket;
#endif
#ifdef HAVE_ORACLE_OCI
extern SPIDER_DBTON spider_dbton_oracle;
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
SPIDER_THREAD *spider_table_sts_threads;
SPIDER_THREAD *spider_table_crd_threads;
#endif

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key spd_key_mutex_tbl;
PSI_mutex_key spd_key_mutex_init_error_tbl;
#ifdef WITH_PARTITION_STORAGE_ENGINE
PSI_mutex_key spd_key_mutex_wide_share;
#endif
PSI_mutex_key spd_key_mutex_lgtm_tblhnd_share;
PSI_mutex_key spd_key_mutex_conn;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
PSI_mutex_key spd_key_mutex_hs_r_conn;
PSI_mutex_key spd_key_mutex_hs_w_conn;
#endif
PSI_mutex_key spd_key_mutex_open_conn;
PSI_mutex_key spd_key_mutex_allocated_thds;
PSI_mutex_key spd_key_mutex_mon_table_cache;
PSI_mutex_key spd_key_mutex_udf_table_mon;
PSI_mutex_key spd_key_mutex_mta_conn;
#ifndef WITHOUT_SPIDER_BG_SEARCH
PSI_mutex_key spd_key_mutex_bg_conn_chain;
PSI_mutex_key spd_key_mutex_bg_conn_sync;
PSI_mutex_key spd_key_mutex_bg_conn;
PSI_mutex_key spd_key_mutex_bg_job_stack;
PSI_mutex_key spd_key_mutex_bg_mon;
PSI_mutex_key spd_key_mutex_bg_direct_sql;
#endif
PSI_mutex_key spd_key_mutex_mon_list_caller;
PSI_mutex_key spd_key_mutex_mon_list_receptor;
PSI_mutex_key spd_key_mutex_mon_list_monitor;
PSI_mutex_key spd_key_mutex_mon_list_update_status;
PSI_mutex_key spd_key_mutex_share;
PSI_mutex_key spd_key_mutex_share_sts;
PSI_mutex_key spd_key_mutex_share_crd;
PSI_mutex_key spd_key_mutex_share_auto_increment;
#ifdef WITH_PARTITION_STORAGE_ENGINE
PSI_mutex_key spd_key_mutex_wide_share_sts;
PSI_mutex_key spd_key_mutex_wide_share_crd;
#endif
PSI_mutex_key spd_key_mutex_udf_table;
PSI_mutex_key spd_key_mutex_mem_calc;
PSI_mutex_key spd_key_thread_id;
PSI_mutex_key spd_key_conn_id;
PSI_mutex_key spd_key_mutex_ipport_count;
PSI_mutex_key spd_key_mutex_conn_i;
#ifndef WITHOUT_SPIDER_BG_SEARCH
PSI_mutex_key spd_key_mutex_bg_stss;
PSI_mutex_key spd_key_mutex_bg_crds;
#endif
PSI_mutex_key spd_key_mutex_conn_loop_check;

static PSI_mutex_info all_spider_mutexes[]=
{
  { &spd_key_mutex_tbl, "tbl", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_init_error_tbl, "init_error_tbl", PSI_FLAG_GLOBAL},
#ifdef WITH_PARTITION_STORAGE_ENGINE
  { &spd_key_mutex_wide_share, "wide_share", PSI_FLAG_GLOBAL},
#endif
  { &spd_key_mutex_lgtm_tblhnd_share, "lgtm_tblhnd_share", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_conn, "conn", PSI_FLAG_GLOBAL},
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  { &spd_key_mutex_hs_r_conn, "hs_r_conn", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_hs_w_conn, "hs_w_conn", PSI_FLAG_GLOBAL},
#endif
  { &spd_key_mutex_open_conn, "open_conn", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_allocated_thds, "allocated_thds", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_mon_table_cache, "mon_table_cache", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_udf_table_mon, "udf_table_mon", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_mem_calc, "mem_calc", PSI_FLAG_GLOBAL},
  { &spd_key_thread_id, "thread_id", PSI_FLAG_GLOBAL},
  { &spd_key_conn_id, "conn_id", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_ipport_count, "ipport_count", PSI_FLAG_GLOBAL},
#ifndef WITHOUT_SPIDER_BG_SEARCH
  { &spd_key_mutex_bg_stss, "bg_stss", PSI_FLAG_GLOBAL},
  { &spd_key_mutex_bg_crds, "bg_crds", PSI_FLAG_GLOBAL},
#endif
  { &spd_key_mutex_conn_i, "conn_i", 0},
  { &spd_key_mutex_mta_conn, "mta_conn", 0},
#ifndef WITHOUT_SPIDER_BG_SEARCH
  { &spd_key_mutex_bg_conn_chain, "bg_conn_chain", 0},
  { &spd_key_mutex_bg_conn_sync, "bg_conn_sync", 0},
  { &spd_key_mutex_bg_conn, "bg_conn", 0},
  { &spd_key_mutex_bg_job_stack, "bg_job_stack", 0},
  { &spd_key_mutex_bg_mon, "bg_mon", 0},
  { &spd_key_mutex_bg_direct_sql, "bg_direct_sql", 0},
#endif
  { &spd_key_mutex_mon_list_caller, "mon_list_caller", 0},
  { &spd_key_mutex_mon_list_receptor, "mon_list_receptor", 0},
  { &spd_key_mutex_mon_list_monitor, "mon_list_monitor", 0},
  { &spd_key_mutex_mon_list_update_status, "mon_list_update_status", 0},
  { &spd_key_mutex_share, "share", 0},
  { &spd_key_mutex_share_sts, "share_sts", 0},
  { &spd_key_mutex_share_crd, "share_crd", 0},
  { &spd_key_mutex_share_auto_increment, "share_auto_increment", 0},
#ifdef WITH_PARTITION_STORAGE_ENGINE
  { &spd_key_mutex_wide_share_sts, "wide_share_sts", 0},
  { &spd_key_mutex_wide_share_crd, "wide_share_crd", 0},
#endif
  { &spd_key_mutex_udf_table, "udf_table", 0},
  { &spd_key_mutex_conn_loop_check, "conn_loop_check", 0},
};

#ifndef WITHOUT_SPIDER_BG_SEARCH
PSI_cond_key spd_key_cond_bg_conn_sync;
PSI_cond_key spd_key_cond_bg_conn;
PSI_cond_key spd_key_cond_bg_sts;
PSI_cond_key spd_key_cond_bg_sts_sync;
PSI_cond_key spd_key_cond_bg_crd;
PSI_cond_key spd_key_cond_bg_crd_sync;
PSI_cond_key spd_key_cond_bg_mon;
PSI_cond_key spd_key_cond_bg_mon_sleep;
PSI_cond_key spd_key_cond_bg_direct_sql;
#endif
PSI_cond_key spd_key_cond_udf_table_mon;
PSI_cond_key spd_key_cond_conn_i;
#ifndef WITHOUT_SPIDER_BG_SEARCH
PSI_cond_key spd_key_cond_bg_stss;
PSI_cond_key spd_key_cond_bg_sts_syncs;
PSI_cond_key spd_key_cond_bg_crds;
PSI_cond_key spd_key_cond_bg_crd_syncs;
#endif

static PSI_cond_info all_spider_conds[] = {
#ifndef WITHOUT_SPIDER_BG_SEARCH
  {&spd_key_cond_bg_conn_sync, "bg_conn_sync", 0},
  {&spd_key_cond_bg_conn, "bg_conn", 0},
  {&spd_key_cond_bg_sts, "bg_sts", 0},
  {&spd_key_cond_bg_sts_sync, "bg_sts_sync", 0},
  {&spd_key_cond_bg_crd, "bg_crd", 0},
  {&spd_key_cond_bg_crd_sync, "bg_crd_sync", 0},
  {&spd_key_cond_bg_mon, "bg_mon", 0},
  {&spd_key_cond_bg_mon_sleep, "bg_mon_sleep", 0},
  {&spd_key_cond_bg_direct_sql, "bg_direct_sql", 0},
#endif
  {&spd_key_cond_udf_table_mon, "udf_table_mon", 0},
  {&spd_key_cond_conn_i, "conn_i", 0},
#ifndef WITHOUT_SPIDER_BG_SEARCH
  {&spd_key_cond_bg_stss, "bg_stss", 0},
  {&spd_key_cond_bg_sts_syncs, "bg_sts_syncs", 0},
  {&spd_key_cond_bg_crds, "bg_crds", 0},
  {&spd_key_cond_bg_crd_syncs, "bg_crd_syncs", 0},
#endif
};

#ifndef WITHOUT_SPIDER_BG_SEARCH
PSI_thread_key spd_key_thd_bg;
PSI_thread_key spd_key_thd_bg_sts;
PSI_thread_key spd_key_thd_bg_crd;
PSI_thread_key spd_key_thd_bg_mon;
PSI_thread_key spd_key_thd_bg_stss;
PSI_thread_key spd_key_thd_bg_crds;
#endif

static PSI_thread_info all_spider_threads[] = {
#ifndef WITHOUT_SPIDER_BG_SEARCH
  {&spd_key_thd_bg, "bg", 0},
  {&spd_key_thd_bg_sts, "bg_sts", 0},
  {&spd_key_thd_bg_crd, "bg_crd", 0},
  {&spd_key_thd_bg_mon, "bg_mon", 0},
  {&spd_key_thd_bg_stss, "bg_stss", 0},
  {&spd_key_thd_bg_crds, "bg_crds", 0},
#endif
};
#endif

extern HASH spider_open_connections;
extern HASH spider_ipport_conns;
extern uint spider_open_connections_id;
extern const char *spider_open_connections_func_name;
extern const char *spider_open_connections_file_name;
extern ulong spider_open_connections_line_no;
extern pthread_mutex_t spider_conn_mutex;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
extern HASH spider_hs_r_conn_hash;
extern uint spider_hs_r_conn_hash_id;
extern const char *spider_hs_r_conn_hash_func_name;
extern const char *spider_hs_r_conn_hash_file_name;
extern ulong spider_hs_r_conn_hash_line_no;
extern pthread_mutex_t spider_hs_r_conn_mutex;
extern HASH spider_hs_w_conn_hash;
extern uint spider_hs_w_conn_hash_id;
extern const char *spider_hs_w_conn_hash_func_name;
extern const char *spider_hs_w_conn_hash_file_name;
extern ulong spider_hs_w_conn_hash_line_no;
extern pthread_mutex_t spider_hs_w_conn_mutex;
#endif
extern HASH *spider_udf_table_mon_list_hash;
extern uint spider_udf_table_mon_list_hash_id;
extern const char *spider_udf_table_mon_list_hash_func_name;
extern const char *spider_udf_table_mon_list_hash_file_name;
extern ulong spider_udf_table_mon_list_hash_line_no;
extern pthread_mutex_t *spider_udf_table_mon_mutexes;
extern pthread_cond_t *spider_udf_table_mon_conds;
extern pthread_mutex_t spider_open_conn_mutex;
extern pthread_mutex_t spider_mon_table_cache_mutex;
extern DYNAMIC_ARRAY spider_mon_table_cache;
extern uint spider_mon_table_cache_id;
extern const char *spider_mon_table_cache_func_name;
extern const char *spider_mon_table_cache_file_name;
extern ulong spider_mon_table_cache_line_no;

HASH spider_open_tables;
uint spider_open_tables_id;
const char *spider_open_tables_func_name;
const char *spider_open_tables_file_name;
ulong spider_open_tables_line_no;
pthread_mutex_t spider_tbl_mutex;
HASH spider_init_error_tables;
uint spider_init_error_tables_id;
const char *spider_init_error_tables_func_name;
const char *spider_init_error_tables_file_name;
ulong spider_init_error_tables_line_no;
pthread_mutex_t spider_init_error_tbl_mutex;

extern pthread_mutex_t spider_thread_id_mutex;
extern pthread_mutex_t spider_conn_id_mutex;
extern pthread_mutex_t spider_ipport_conn_mutex;

#ifdef WITH_PARTITION_STORAGE_ENGINE
HASH spider_open_wide_share;
uint spider_open_wide_share_id;
const char *spider_open_wide_share_func_name;
const char *spider_open_wide_share_file_name;
ulong spider_open_wide_share_line_no;
pthread_mutex_t spider_wide_share_mutex;
#endif

HASH spider_lgtm_tblhnd_share_hash;
uint spider_lgtm_tblhnd_share_hash_id;
const char *spider_lgtm_tblhnd_share_hash_func_name;
const char *spider_lgtm_tblhnd_share_hash_file_name;
ulong spider_lgtm_tblhnd_share_hash_line_no;
pthread_mutex_t spider_lgtm_tblhnd_share_mutex;

HASH spider_allocated_thds;
uint spider_allocated_thds_id;
const char *spider_allocated_thds_func_name;
const char *spider_allocated_thds_file_name;
ulong spider_allocated_thds_line_no;
pthread_mutex_t spider_allocated_thds_mutex;

#ifndef WITHOUT_SPIDER_BG_SEARCH
pthread_attr_t spider_pt_attr;
#endif

extern pthread_mutex_t spider_mem_calc_mutex;

extern const char *spider_alloc_func_name[SPIDER_MEM_CALC_LIST_NUM];
extern const char *spider_alloc_file_name[SPIDER_MEM_CALC_LIST_NUM];
extern ulong      spider_alloc_line_no[SPIDER_MEM_CALC_LIST_NUM];
extern ulonglong  spider_total_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
extern longlong   spider_current_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
extern ulonglong  spider_alloc_mem_count[SPIDER_MEM_CALC_LIST_NUM];
extern ulonglong  spider_free_mem_count[SPIDER_MEM_CALC_LIST_NUM];

static char spider_wild_many = '%', spider_wild_one = '_',
  spider_wild_prefix='\\';

static char spider_unique_id_buf[1 + 12 + 1 + (16 * 2) + 1 + 1];
LEX_CSTRING spider_unique_id;

// for spider_open_tables
uchar *spider_tbl_get_key(
  SPIDER_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_tbl_get_key");
  *length = share->table_name_length;
  DBUG_RETURN((uchar*) share->table_name);
}

uchar *spider_wide_share_get_key(
  SPIDER_WIDE_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_wide_share_get_key");
  *length = share->table_name_length;
  DBUG_RETURN((uchar*) share->table_name);
}

uchar *spider_lgtm_tblhnd_share_hash_get_key(
  SPIDER_LGTM_TBLHND_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_lgtm_tblhnd_share_hash_get_key");
  *length = share->table_name_length;
  DBUG_RETURN((uchar*) share->table_name);
}

uchar *spider_link_get_key(
  SPIDER_LINK_FOR_HASH *link_for_hash,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_link_get_key");
  *length = link_for_hash->db_table_str->length();
  DBUG_RETURN((uchar*) link_for_hash->db_table_str->ptr());
}

uchar *spider_ha_get_key(
  ha_spider *spider,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_ha_get_key");
  *length = spider->share->table_name_length;
  DBUG_RETURN((uchar*) spider->share->table_name);
}

uchar *spider_udf_tbl_mon_list_key(
  SPIDER_TABLE_MON_LIST *table_mon_list,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_udf_tbl_mon_list_key");
  DBUG_PRINT("info",("spider hash key=%s", table_mon_list->key));
  DBUG_PRINT("info",("spider hash key length=%u", table_mon_list->key_length));
  *length = table_mon_list->key_length;
  DBUG_RETURN((uchar*) table_mon_list->key);
}

uchar *spider_allocated_thds_get_key(
  THD *thd,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("spider_allocated_thds_get_key");
  *length = sizeof(THD *);
  DBUG_RETURN((uchar*) thd);
}

#ifdef HAVE_PSI_INTERFACE
static void init_spider_psi_keys()
{
  DBUG_ENTER("init_spider_psi_keys");
  if (PSI_server == NULL)
    DBUG_VOID_RETURN;

  PSI_server->register_mutex("spider", all_spider_mutexes,
    array_elements(all_spider_mutexes));
  PSI_server->register_cond("spider", all_spider_conds,
    array_elements(all_spider_conds));
  PSI_server->register_thread("spider", all_spider_threads,
    array_elements(all_spider_threads));
  DBUG_VOID_RETURN;
}
#endif

int spider_get_server(
  SPIDER_SHARE *share,
  int link_idx
) {
  MEM_ROOT mem_root;
  int error_num, length;
  FOREIGN_SERVER *server, server_buf;
  DBUG_ENTER("spider_get_server");
  SPD_INIT_ALLOC_ROOT(&mem_root, 128, 0, MYF(MY_WME));

  if (!(server
       = get_server_by_name(&mem_root, share->server_names[link_idx],
         &server_buf)))
  {
    error_num = ER_FOREIGN_SERVER_DOESNT_EXIST;
    goto error;
  }

  if (!share->tgt_wrappers[link_idx] && server->scheme)
  {
    share->tgt_wrappers_lengths[link_idx] = strlen(server->scheme);
    if (!(share->tgt_wrappers[link_idx] =
      spider_create_string(server->scheme,
      share->tgt_wrappers_lengths[link_idx])))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_wrappers=%s",
      share->tgt_wrappers[link_idx]));
  }

  if (!share->tgt_hosts[link_idx] && server->host)
  {
    share->tgt_hosts_lengths[link_idx] = strlen(server->host);
    if (!(share->tgt_hosts[link_idx] =
      spider_create_string(server->host, share->tgt_hosts_lengths[link_idx])))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_hosts=%s", share->tgt_hosts[link_idx]));
  }

  if (share->tgt_ports[link_idx] == -1)
  {
    share->tgt_ports[link_idx] = server->port;
    DBUG_PRINT("info",("spider tgt_ports=%ld", share->tgt_ports[link_idx]));
  }

  if (!share->tgt_sockets[link_idx] && server->socket)
  {
    share->tgt_sockets_lengths[link_idx] = strlen(server->socket);
    if (!(share->tgt_sockets[link_idx] =
      spider_create_string(server->socket,
      share->tgt_sockets_lengths[link_idx])))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_sockets=%s", share->tgt_sockets[link_idx]));
  }

  if (!share->tgt_dbs[link_idx] && server->db && (length = strlen(server->db)))
  {
    share->tgt_dbs_lengths[link_idx] = length;
    if (!(share->tgt_dbs[link_idx] =
      spider_create_string(server->db, length)))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_dbs=%s", share->tgt_dbs[link_idx]));
  }

  if (!share->tgt_usernames[link_idx] && server->username)
  {
    share->tgt_usernames_lengths[link_idx] = strlen(server->username);
    if (!(share->tgt_usernames[link_idx] =
      spider_create_string(server->username,
      share->tgt_usernames_lengths[link_idx])))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_usernames=%s",
      share->tgt_usernames[link_idx]));
  }

  if (!share->tgt_passwords[link_idx] && server->password)
  {
    share->tgt_passwords_lengths[link_idx] = strlen(server->password);
    if (!(share->tgt_passwords[link_idx] =
      spider_create_string(server->password,
      share->tgt_passwords_lengths[link_idx])))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    DBUG_PRINT("info",("spider tgt_passwords=%s",
      share->tgt_passwords[link_idx]));
  }

  free_root(&mem_root, MYF(0));
  DBUG_RETURN(0);

error:
  free_root(&mem_root, MYF(0));
  my_error(error_num, MYF(0), share->server_names[link_idx]);
  DBUG_RETURN(error_num);
}

int spider_free_share_alloc(
  SPIDER_SHARE *share
) {
  int roop_count;
  DBUG_ENTER("spider_free_share_alloc");
  for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; roop_count--)
  {
    if (share->dbton_share[roop_count])
    {
      delete share->dbton_share[roop_count];
      share->dbton_share[roop_count] = NULL;
    }
  }
  if (share->server_names)
  {
    for (roop_count = 0; roop_count < (int) share->server_names_length;
      roop_count++)
    {
      if (share->server_names[roop_count])
        spider_free(spider_current_trx, share->server_names[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->server_names, MYF(0));
  }
  if (share->tgt_table_names)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_table_names_length;
      roop_count++)
    {
      if (share->tgt_table_names[roop_count])
        spider_free(spider_current_trx, share->tgt_table_names[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_table_names, MYF(0));
  }
  if (share->tgt_dbs)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_dbs_length;
      roop_count++)
    {
      if (share->tgt_dbs[roop_count])
        spider_free(spider_current_trx, share->tgt_dbs[roop_count], MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_dbs, MYF(0));
  }
  if (share->tgt_hosts)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_hosts_length;
      roop_count++)
    {
      if (share->tgt_hosts[roop_count])
        spider_free(spider_current_trx, share->tgt_hosts[roop_count], MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_hosts, MYF(0));
  }
  if (share->tgt_usernames)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_usernames_length;
      roop_count++)
    {
      if (share->tgt_usernames[roop_count])
        spider_free(spider_current_trx, share->tgt_usernames[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_usernames, MYF(0));
  }
  if (share->tgt_passwords)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_passwords_length;
      roop_count++)
    {
      if (share->tgt_passwords[roop_count])
        spider_free(spider_current_trx, share->tgt_passwords[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_passwords, MYF(0));
  }
  if (share->tgt_sockets)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_sockets_length;
      roop_count++)
    {
      if (share->tgt_sockets[roop_count])
        spider_free(spider_current_trx, share->tgt_sockets[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_sockets, MYF(0));
  }
  if (share->tgt_wrappers)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_wrappers_length;
      roop_count++)
    {
      if (share->tgt_wrappers[roop_count])
        spider_free(spider_current_trx, share->tgt_wrappers[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_wrappers, MYF(0));
  }
  if (share->tgt_ssl_cas)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_ssl_cas_length;
      roop_count++)
    {
      if (share->tgt_ssl_cas[roop_count])
        spider_free(spider_current_trx, share->tgt_ssl_cas[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_ssl_cas, MYF(0));
  }
  if (share->tgt_ssl_capaths)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_ssl_capaths_length;
      roop_count++)
    {
      if (share->tgt_ssl_capaths[roop_count])
        spider_free(spider_current_trx, share->tgt_ssl_capaths[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_ssl_capaths, MYF(0));
  }
  if (share->tgt_ssl_certs)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_ssl_certs_length;
      roop_count++)
    {
      if (share->tgt_ssl_certs[roop_count])
        spider_free(spider_current_trx, share->tgt_ssl_certs[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_ssl_certs, MYF(0));
  }
  if (share->tgt_ssl_ciphers)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_ssl_ciphers_length;
      roop_count++)
    {
      if (share->tgt_ssl_ciphers[roop_count])
        spider_free(spider_current_trx, share->tgt_ssl_ciphers[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_ssl_ciphers, MYF(0));
  }
  if (share->tgt_ssl_keys)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_ssl_keys_length;
      roop_count++)
    {
      if (share->tgt_ssl_keys[roop_count])
        spider_free(spider_current_trx, share->tgt_ssl_keys[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_ssl_keys, MYF(0));
  }
  if (share->tgt_default_files)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_default_files_length;
      roop_count++)
    {
      if (share->tgt_default_files[roop_count])
        spider_free(spider_current_trx, share->tgt_default_files[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_default_files, MYF(0));
  }
  if (share->tgt_default_groups)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_default_groups_length;
      roop_count++)
    {
      if (share->tgt_default_groups[roop_count])
        spider_free(spider_current_trx, share->tgt_default_groups[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_default_groups, MYF(0));
  }
  if (share->tgt_dsns)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_dsns_length;
      roop_count++)
    {
      if (share->tgt_dsns[roop_count])
        spider_free(spider_current_trx, share->tgt_dsns[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_dsns, MYF(0));
  }
  if (share->tgt_filedsns)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_filedsns_length;
      roop_count++)
    {
      if (share->tgt_filedsns[roop_count])
        spider_free(spider_current_trx, share->tgt_filedsns[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_filedsns, MYF(0));
  }
  if (share->tgt_drivers)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_drivers_length;
      roop_count++)
    {
      if (share->tgt_drivers[roop_count])
        spider_free(spider_current_trx, share->tgt_drivers[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_drivers, MYF(0));
  }
  if (share->tgt_pk_names)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_pk_names_length;
      roop_count++)
    {
      if (share->tgt_pk_names[roop_count])
        spider_free(spider_current_trx, share->tgt_pk_names[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_pk_names, MYF(0));
  }
  if (share->tgt_sequence_names)
  {
    for (roop_count = 0; roop_count < (int) share->tgt_sequence_names_length;
      roop_count++)
    {
      if (share->tgt_sequence_names[roop_count])
        spider_free(spider_current_trx, share->tgt_sequence_names[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->tgt_sequence_names, MYF(0));
  }
  if (share->static_link_ids)
  {
    for (roop_count = 0; roop_count < (int) share->static_link_ids_length;
      roop_count++)
    {
      if (share->static_link_ids[roop_count])
        spider_free(spider_current_trx, share->static_link_ids[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->static_link_ids, MYF(0));
  }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (share->hs_read_socks)
  {
    for (roop_count = 0; roop_count < (int) share->hs_read_socks_length;
      roop_count++)
    {
      if (share->hs_read_socks[roop_count])
        spider_free(spider_current_trx, share->hs_read_socks[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->hs_read_socks, MYF(0));
  }
  if (share->hs_write_socks)
  {
    for (roop_count = 0; roop_count < (int) share->hs_write_socks_length;
      roop_count++)
    {
      if (share->hs_write_socks[roop_count])
        spider_free(spider_current_trx, share->hs_write_socks[roop_count],
          MYF(0));
    }
    spider_free(spider_current_trx, share->hs_write_socks, MYF(0));
  }
#endif
  if (share->bka_engine)
    spider_free(spider_current_trx, share->bka_engine, MYF(0));
  if (share->conn_keys)
    spider_free(spider_current_trx, share->conn_keys, MYF(0));
  if (share->tgt_ports)
    spider_free(spider_current_trx, share->tgt_ports, MYF(0));
  if (share->tgt_ssl_vscs)
    spider_free(spider_current_trx, share->tgt_ssl_vscs, MYF(0));
  if (share->link_statuses)
    spider_free(spider_current_trx, share->link_statuses, MYF(0));
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (share->monitoring_bg_flag)
    spider_free(spider_current_trx, share->monitoring_bg_flag, MYF(0));
  if (share->monitoring_bg_kind)
    spider_free(spider_current_trx, share->monitoring_bg_kind, MYF(0));
#endif
  if (share->monitoring_binlog_pos_at_failing)
    spider_free(spider_current_trx, share->monitoring_binlog_pos_at_failing, MYF(0));
  if (share->monitoring_flag)
    spider_free(spider_current_trx, share->monitoring_flag, MYF(0));
  if (share->monitoring_kind)
    spider_free(spider_current_trx, share->monitoring_kind, MYF(0));
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (share->use_hs_reads)
    spider_free(spider_current_trx, share->use_hs_reads, MYF(0));
  if (share->use_hs_writes)
    spider_free(spider_current_trx, share->use_hs_writes, MYF(0));
  if (share->hs_read_ports)
    spider_free(spider_current_trx, share->hs_read_ports, MYF(0));
  if (share->hs_write_ports)
    spider_free(spider_current_trx, share->hs_write_ports, MYF(0));
  if (share->hs_write_to_reads)
    spider_free(spider_current_trx, share->hs_write_to_reads, MYF(0));
#endif
  if (share->use_handlers)
    spider_free(spider_current_trx, share->use_handlers, MYF(0));
  if (share->connect_timeouts)
    spider_free(spider_current_trx, share->connect_timeouts, MYF(0));
  if (share->net_read_timeouts)
    spider_free(spider_current_trx, share->net_read_timeouts, MYF(0));
  if (share->net_write_timeouts)
    spider_free(spider_current_trx, share->net_write_timeouts, MYF(0));
  if (share->access_balances)
    spider_free(spider_current_trx, share->access_balances, MYF(0));
  if (share->bka_table_name_types)
    spider_free(spider_current_trx, share->bka_table_name_types, MYF(0));
  if (share->strict_group_bys)
    spider_free(spider_current_trx, share->strict_group_bys, MYF(0));
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (share->monitoring_bg_interval)
    spider_free(spider_current_trx, share->monitoring_bg_interval, MYF(0));
#endif
  if (share->monitoring_limit)
    spider_free(spider_current_trx, share->monitoring_limit, MYF(0));
  if (share->monitoring_sid)
    spider_free(spider_current_trx, share->monitoring_sid, MYF(0));
  if (share->alter_table.tmp_server_names)
    spider_free(spider_current_trx, share->alter_table.tmp_server_names,
      MYF(0));
  if (share->key_hint)
  {
    delete [] share->key_hint;
    share->key_hint = NULL;
  }
  if (share->wide_share)
    spider_free_wide_share(share->wide_share);
  DBUG_RETURN(0);
}

void spider_free_tmp_share_alloc(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_free_tmp_share_alloc");
  if (share->server_names && share->server_names[0])
  {
    spider_free(spider_current_trx, share->server_names[0], MYF(0));
    share->server_names[0] = NULL;
  }
  if (share->tgt_table_names && share->tgt_table_names[0])
  {
    spider_free(spider_current_trx, share->tgt_table_names[0], MYF(0));
    share->tgt_table_names[0] = NULL;
  }
  if (share->tgt_dbs && share->tgt_dbs[0])
  {
    spider_free(spider_current_trx, share->tgt_dbs[0], MYF(0));
    share->tgt_dbs[0] = NULL;
  }
  if (share->tgt_hosts && share->tgt_hosts[0])
  {
    spider_free(spider_current_trx, share->tgt_hosts[0], MYF(0));
    share->tgt_hosts[0] = NULL;
  }
  if (share->tgt_usernames && share->tgt_usernames[0])
  {
    spider_free(spider_current_trx, share->tgt_usernames[0], MYF(0));
    share->tgt_usernames[0] = NULL;
  }
  if (share->tgt_passwords && share->tgt_passwords[0])
  {
    spider_free(spider_current_trx, share->tgt_passwords[0], MYF(0));
    share->tgt_passwords[0] = NULL;
  }
  if (share->tgt_sockets && share->tgt_sockets[0])
  {
    spider_free(spider_current_trx, share->tgt_sockets[0], MYF(0));
    share->tgt_sockets[0] = NULL;
  }
  if (share->tgt_wrappers && share->tgt_wrappers[0])
  {
    spider_free(spider_current_trx, share->tgt_wrappers[0], MYF(0));
    share->tgt_wrappers[0] = NULL;
  }
  if (share->tgt_ssl_cas && share->tgt_ssl_cas[0])
  {
    spider_free(spider_current_trx, share->tgt_ssl_cas[0], MYF(0));
    share->tgt_ssl_cas[0] = NULL;
  }
  if (share->tgt_ssl_capaths && share->tgt_ssl_capaths[0])
  {
    spider_free(spider_current_trx, share->tgt_ssl_capaths[0], MYF(0));
    share->tgt_ssl_capaths[0] = NULL;
  }
  if (share->tgt_ssl_certs && share->tgt_ssl_certs[0])
  {
    spider_free(spider_current_trx, share->tgt_ssl_certs[0], MYF(0));
    share->tgt_ssl_certs[0] = NULL;
  }
  if (share->tgt_ssl_ciphers && share->tgt_ssl_ciphers[0])
  {
    spider_free(spider_current_trx, share->tgt_ssl_ciphers[0], MYF(0));
    share->tgt_ssl_ciphers[0] = NULL;
  }
  if (share->tgt_ssl_keys && share->tgt_ssl_keys[0])
  {
    spider_free(spider_current_trx, share->tgt_ssl_keys[0], MYF(0));
    share->tgt_ssl_keys[0] = NULL;
  }
  if (share->tgt_default_files && share->tgt_default_files[0])
  {
    spider_free(spider_current_trx, share->tgt_default_files[0], MYF(0));
    share->tgt_default_files[0] = NULL;
  }
  if (share->tgt_default_groups && share->tgt_default_groups[0])
  {
    spider_free(spider_current_trx, share->tgt_default_groups[0], MYF(0));
    share->tgt_default_groups[0] = NULL;
  }
  if (share->tgt_dsns && share->tgt_dsns[0])
  {
    spider_free(spider_current_trx, share->tgt_dsns[0], MYF(0));
    share->tgt_dsns[0] = NULL;
  }
  if (share->tgt_filedsns && share->tgt_filedsns[0])
  {
    spider_free(spider_current_trx, share->tgt_filedsns[0], MYF(0));
    share->tgt_filedsns[0] = NULL;
  }
  if (share->tgt_drivers && share->tgt_drivers[0])
  {
    spider_free(spider_current_trx, share->tgt_drivers[0], MYF(0));
    share->tgt_drivers[0] = NULL;
  }
  if (share->tgt_pk_names && share->tgt_pk_names[0])
  {
    spider_free(spider_current_trx, share->tgt_pk_names[0], MYF(0));
    share->tgt_pk_names[0] = NULL;
  }
  if (share->tgt_sequence_names && share->tgt_sequence_names[0])
  {
    spider_free(spider_current_trx, share->tgt_sequence_names[0], MYF(0));
    share->tgt_sequence_names[0] = NULL;
  }
  if (share->static_link_ids && share->static_link_ids[0])
  {
    spider_free(spider_current_trx, share->static_link_ids[0], MYF(0));
    share->static_link_ids[0] = NULL;
  }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (share->hs_read_socks && share->hs_read_socks[0])
  {
    spider_free(spider_current_trx, share->hs_read_socks[0], MYF(0));
    share->hs_read_socks[0] = NULL;
  }
  if (share->hs_write_socks && share->hs_write_socks[0])
  {
    spider_free(spider_current_trx, share->hs_write_socks[0], MYF(0));
    share->hs_write_socks[0] = NULL;
  }
#endif
  if (share->bka_engine)
  {
    spider_free(spider_current_trx, share->bka_engine, MYF(0));
    share->bka_engine = NULL;
  }
  if (share->conn_keys)
  {
    spider_free(spider_current_trx, share->conn_keys, MYF(0));
    share->conn_keys = NULL;
  }
  if (share->static_key_cardinality)
    spider_free(spider_current_trx, share->static_key_cardinality, MYF(0));
  if (share->key_hint)
  {
    delete [] share->key_hint;
    share->key_hint = NULL;
  }
  DBUG_VOID_RETURN;
}

char *spider_get_string_between_quote(
  char *ptr,
  bool alloc,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
) {
  char *start_ptr, *end_ptr, *tmp_ptr, *esc_ptr;
  bool find_flg = FALSE;
  DBUG_ENTER("spider_get_string_between_quote");

  start_ptr = strchr(ptr, '\'');
  end_ptr = strchr(ptr, '"');
  if (start_ptr && (!end_ptr || start_ptr < end_ptr))
  {
    tmp_ptr = ++start_ptr;
    while (!find_flg)
    {
      if (!(end_ptr = strchr(tmp_ptr, '\'')))
        DBUG_RETURN(NULL);
      esc_ptr = tmp_ptr;
      while (!find_flg)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr || esc_ptr > end_ptr)
          find_flg = TRUE;
        else if (esc_ptr == end_ptr - 1)
        {
          tmp_ptr = end_ptr + 1;
          break;
        } else {
          esc_ptr += 2;
        }
      }
    }
  } else if (end_ptr)
  {
    start_ptr = end_ptr;
    tmp_ptr = ++start_ptr;
    while (!find_flg)
    {
      if (!(end_ptr = strchr(tmp_ptr, '"')))
        DBUG_RETURN(NULL);
      esc_ptr = tmp_ptr;
      while (!find_flg)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr || esc_ptr > end_ptr)
          find_flg = TRUE;
        else if (esc_ptr == end_ptr - 1)
        {
          tmp_ptr = end_ptr + 1;
          break;
        } else {
          esc_ptr += 2;
        }
      }
    }
  } else
    DBUG_RETURN(NULL);

  *end_ptr = '\0';

  if (param_string_parse)
    param_string_parse->set_param_value(start_ptr, start_ptr + strlen(start_ptr) + 1);

  if (alloc)
  {
    DBUG_RETURN(
      spider_create_string(
      start_ptr,
      strlen(start_ptr))
    );
  } else {
    DBUG_RETURN(start_ptr);
  }
}

int spider_create_string_list(
  char ***string_list,
  uint **string_length_list,
  uint *list_length,
  char *str,
  uint length,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
) {
  int roop_count;
  char *tmp_ptr, *tmp_ptr2, *tmp_ptr3, *tmp_ptr4, *esc_ptr;
  bool find_flg = FALSE;
  DBUG_ENTER("spider_create_string_list");

  *list_length = 0;
  param_string_parse->init_param_value();
  if (!str)
  {
    *string_list = NULL;
    DBUG_RETURN(0);
  }

  tmp_ptr = str;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  if (*tmp_ptr)
    *list_length = 1;
  else {
    *string_list = NULL;
    DBUG_RETURN(0);
  }

  bool last_esc_flg = FALSE;
  while (TRUE)
  {
    if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
    {
      find_flg = FALSE;
      last_esc_flg = FALSE;
      esc_ptr = tmp_ptr;
      while (!find_flg)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr || esc_ptr > tmp_ptr2)
        {
          find_flg = TRUE;
        }
        else if (esc_ptr == tmp_ptr2 - 1)
        {
          last_esc_flg = TRUE;
          tmp_ptr = tmp_ptr2 + 1;
          break;
        } else {
          last_esc_flg = TRUE;
          esc_ptr += 2;
        }
      }
      if (find_flg)
      {
        (*list_length)++;
        tmp_ptr = tmp_ptr2 + 1;
        while (*tmp_ptr == ' ')
          tmp_ptr++;
      }
    } else
      break;
  }

  if (!(*string_list = (char**)
    spider_bulk_malloc(spider_current_trx, 37, MYF(MY_WME | MY_ZEROFILL),
      string_list, (uint) (sizeof(char*) * (*list_length)),
      string_length_list, (uint) (sizeof(int) * (*list_length)),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  tmp_ptr = str;
  while (*tmp_ptr == ' ')
  {
    *tmp_ptr = '\0';
    tmp_ptr++;
  }
  tmp_ptr3 = tmp_ptr;

  for (roop_count = 0; roop_count < (int) *list_length - 1; roop_count++)
  {
    bool esc_flg = FALSE;
    find_flg = FALSE;
    while (TRUE)
    {
      tmp_ptr2 = strchr(tmp_ptr, ' ');

      esc_ptr = tmp_ptr;
      while (!find_flg)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr || esc_ptr > tmp_ptr2)
        {
          find_flg = TRUE;
        }
        else if (esc_ptr == tmp_ptr2 - 1)
        {
          esc_flg = TRUE;
          tmp_ptr = tmp_ptr2 + 1;
          break;
        } else {
          esc_flg = TRUE;
          esc_ptr += 2;
        }
      }
      if (find_flg)
        break;
    }
    tmp_ptr = tmp_ptr2;

    while (*tmp_ptr == ' ')
    {
      *tmp_ptr = '\0';
      tmp_ptr++;
    }

    (*string_length_list)[roop_count] = strlen(tmp_ptr3);
    if (!((*string_list)[roop_count] = spider_create_string(
      tmp_ptr3, (*string_length_list)[roop_count]))
    ) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }

    if (esc_flg)
    {
      esc_ptr = (*string_list)[roop_count];
      while (TRUE)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr)
          break;
        switch(*(esc_ptr + 1))
        {
          case 'b':
            *esc_ptr = '\b';
            break;
          case 'n':
            *esc_ptr = '\n';
            break;
          case 'r':
            *esc_ptr = '\r';
            break;
          case 't':
            *esc_ptr = '\t';
            break;
          default:
            *esc_ptr = *(esc_ptr + 1);
            break;
        }
        esc_ptr++;
        tmp_ptr4 = esc_ptr;
        do
        {
          *tmp_ptr4 = *(tmp_ptr4 + 1);
          tmp_ptr4++;
        } while (*tmp_ptr4);
        (*string_length_list)[roop_count] -= 1;
      }
    }
    DBUG_PRINT("info",("spider string_list[%d]=%s", roop_count,
      (*string_list)[roop_count]));
    tmp_ptr3 = tmp_ptr;
  }
  (*string_length_list)[roop_count] = strlen(tmp_ptr3);
  if (!((*string_list)[roop_count] = spider_create_string(
    tmp_ptr3, (*string_length_list)[roop_count]))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (last_esc_flg)
  {
    esc_ptr = (*string_list)[roop_count];
    while (TRUE)
    {
      esc_ptr = strchr(esc_ptr, '\\');
      if (!esc_ptr)
        break;
      switch(*(esc_ptr + 1))
      {
        case 'b':
          *esc_ptr = '\b';
          break;
        case 'n':
          *esc_ptr = '\n';
          break;
        case 'r':
          *esc_ptr = '\r';
          break;
        case 't':
          *esc_ptr = '\t';
          break;
        default:
          *esc_ptr = *(esc_ptr + 1);
          break;
      }
      esc_ptr++;
      tmp_ptr4 = esc_ptr;
      do
      {
        *tmp_ptr4 = *(tmp_ptr4 + 1);
        tmp_ptr4++;
      } while (*tmp_ptr4);
      (*string_length_list)[roop_count] -= 1;
    }
  }

  param_string_parse->set_param_value(tmp_ptr3,
                                      tmp_ptr3 + strlen(tmp_ptr3) + 1);

  DBUG_PRINT("info",("spider string_list[%d]=%s", roop_count,
    (*string_list)[roop_count]));

  DBUG_RETURN(0);
}

int spider_create_long_list(
  long **long_list,
  uint *list_length,
  char *str,
  uint length,
  long min_val,
  long max_val,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
) {
  int roop_count;
  char *tmp_ptr;
  DBUG_ENTER("spider_create_long_list");

  *list_length = 0;
  param_string_parse->init_param_value();
  if (!str)
  {
    *long_list = NULL;
    DBUG_RETURN(0);
  }

  tmp_ptr = str;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  if (*tmp_ptr)
    *list_length = 1;
  else {
    *long_list = NULL;
    DBUG_RETURN(0);
  }

  while (TRUE)
  {
    if ((tmp_ptr = strchr(tmp_ptr, ' ')))
    {
      (*list_length)++;
      tmp_ptr = tmp_ptr + 1;
      while (*tmp_ptr == ' ')
        tmp_ptr++;
    } else
      break;
  }

  if (!(*long_list = (long*)
    spider_bulk_malloc(spider_current_trx, 38, MYF(MY_WME | MY_ZEROFILL),
      long_list, (uint) (sizeof(long) * (*list_length)),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  tmp_ptr = str;
  for (roop_count = 0; roop_count < (int) *list_length; roop_count++)
  {
    if (roop_count != 0)
      tmp_ptr = strchr(tmp_ptr, ' ');

    while (*tmp_ptr == ' ')
    {
      *tmp_ptr = '\0';
      tmp_ptr++;
    }
    (*long_list)[roop_count] = atol(tmp_ptr);
    if ((*long_list)[roop_count] < min_val)
      (*long_list)[roop_count] = min_val;
    else if ((*long_list)[roop_count] > max_val)
      (*long_list)[roop_count] = max_val;
  }

  param_string_parse->set_param_value(tmp_ptr,
                                      tmp_ptr + strlen(tmp_ptr) + 1);

#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < (int) *list_length; roop_count++)
  {
    DBUG_PRINT("info",("spider long_list[%d]=%ld", roop_count,
      (*long_list)[roop_count]));
  }
#endif

  DBUG_RETURN(0);
}

int spider_create_longlong_list(
  longlong **longlong_list,
  uint *list_length,
  char *str,
  uint length,
  longlong min_val,
  longlong max_val,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
) {
  int error_num, roop_count;
  char *tmp_ptr;
  DBUG_ENTER("spider_create_longlong_list");

  *list_length = 0;
  param_string_parse->init_param_value();
  if (!str)
  {
    *longlong_list = NULL;
    DBUG_RETURN(0);
  }

  tmp_ptr = str;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  if (*tmp_ptr)
    *list_length = 1;
  else {
    *longlong_list = NULL;
    DBUG_RETURN(0);
  }

  while (TRUE)
  {
    if ((tmp_ptr = strchr(tmp_ptr, ' ')))
    {
      (*list_length)++;
      tmp_ptr = tmp_ptr + 1;
      while (*tmp_ptr == ' ')
        tmp_ptr++;
    } else
      break;
  }

  if (!(*longlong_list = (longlong *)
    spider_bulk_malloc(spider_current_trx, 39, MYF(MY_WME | MY_ZEROFILL),
      longlong_list, (uint) (sizeof(longlong) * (*list_length)),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  tmp_ptr = str;
  for (roop_count = 0; roop_count < (int) *list_length; roop_count++)
  {
    if (roop_count != 0)
      tmp_ptr = strchr(tmp_ptr, ' ');

    while (*tmp_ptr == ' ')
    {
      *tmp_ptr = '\0';
      tmp_ptr++;
    }
    (*longlong_list)[roop_count] = my_strtoll10(tmp_ptr, (char**) NULL,
      &error_num);
    if ((*longlong_list)[roop_count] < min_val)
      (*longlong_list)[roop_count] = min_val;
    else if ((*longlong_list)[roop_count] > max_val)
      (*longlong_list)[roop_count] = max_val;
  }

  param_string_parse->set_param_value(tmp_ptr,
                                      tmp_ptr + strlen(tmp_ptr) + 1);

#ifndef DBUG_OFF
  for (roop_count = 0; roop_count < (int) *list_length; roop_count++)
  {
    DBUG_PRINT("info",("spider longlong_list[%d]=%lld", roop_count,
      (*longlong_list)[roop_count]));
  }
#endif

  DBUG_RETURN(0);
}

int spider_increase_string_list(
  char ***string_list,
  uint **string_length_list,
  uint *list_length,
  uint *list_charlen,
  uint link_count
) {
  int roop_count;
  char **tmp_str_list, *tmp_str;
  uint *tmp_length_list, tmp_length;
  DBUG_ENTER("spider_increase_string_list");
  if (*list_length == link_count)
    DBUG_RETURN(0);
  if (*list_length > 1)
  {
    my_printf_error(ER_SPIDER_DIFFERENT_LINK_COUNT_NUM,
      ER_SPIDER_DIFFERENT_LINK_COUNT_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_DIFFERENT_LINK_COUNT_NUM);
  }

  if (*string_list)
  {
    tmp_str = (*string_list)[0];
    tmp_length = (*string_length_list)[0];
  } else {
    tmp_str = NULL;
    tmp_length = 0;
  }

  if (!(tmp_str_list = (char**)
    spider_bulk_malloc(spider_current_trx, 40, MYF(MY_WME | MY_ZEROFILL),
      &tmp_str_list, (uint) (sizeof(char*) * link_count),
      &tmp_length_list, (uint) (sizeof(uint) * link_count),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  for (roop_count = 0; roop_count < (int) link_count; roop_count++)
  {
    tmp_length_list[roop_count] = tmp_length;
    if (tmp_str)
    {
      if (!(tmp_str_list[roop_count] = spider_create_string(
        tmp_str, tmp_length))
      )
        goto error;
      DBUG_PRINT("info",("spider string_list[%d]=%s", roop_count,
        tmp_str_list[roop_count]));
    } else
      tmp_str_list[roop_count] = NULL;
  }
  if (*string_list)
  {
    if ((*string_list)[0])
      spider_free(spider_current_trx, (*string_list)[0], MYF(0));
    spider_free(spider_current_trx, *string_list, MYF(0));
  }
  *list_charlen = (tmp_length + 1) * link_count - 1;
  *list_length = link_count;
  *string_list = tmp_str_list;
  *string_length_list = tmp_length_list;

  DBUG_RETURN(0);

error:
  for (roop_count = 0; roop_count < (int) link_count; roop_count++)
  {
    if (tmp_str_list[roop_count])
      spider_free(spider_current_trx, tmp_str_list[roop_count], MYF(0));
  }
  if (tmp_str_list)
    spider_free(spider_current_trx, tmp_str_list, MYF(0));
  my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
  DBUG_RETURN(HA_ERR_OUT_OF_MEM);
}

int spider_increase_null_string_list(
  char ***string_list,
  uint **string_length_list,
  uint *list_length,
  uint *list_charlen,
  uint link_count
) {
  int roop_count;
  char **tmp_str_list;
  uint *tmp_length_list;
  DBUG_ENTER("spider_increase_null_string_list");
  if (*list_length == link_count)
    DBUG_RETURN(0);

  if (!(tmp_str_list = (char**)
    spider_bulk_malloc(spider_current_trx, 247, MYF(MY_WME | MY_ZEROFILL),
      &tmp_str_list, (uint) (sizeof(char*) * link_count),
      &tmp_length_list, (uint) (sizeof(uint) * link_count),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  for (roop_count = 0; roop_count < (int) *list_length; roop_count++)
  {
    tmp_str_list[roop_count] = (*string_list)[roop_count];
    tmp_length_list[roop_count] = (*string_length_list)[roop_count];
  }
  if (*string_list)
  {
    spider_free(spider_current_trx, *string_list, MYF(0));
  }
  *list_length = link_count;
  *string_list = tmp_str_list;
  *string_length_list = tmp_length_list;
#ifndef DBUG_OFF
  DBUG_PRINT("info",("spider list_length=%u", *list_length));
  for (roop_count = 0; roop_count < (int) *list_length; roop_count++)
  {
    DBUG_PRINT("info",("spider string_list[%d]=%s", roop_count,
      (*string_list)[roop_count] ? (*string_list)[roop_count] : "NULL"));
    DBUG_PRINT("info",("spider string_length_list[%d]=%u", roop_count,
      (*string_length_list)[roop_count]));
  }
#endif

  DBUG_RETURN(0);
}

int spider_increase_long_list(
  long **long_list,
  uint *list_length,
  uint link_count
) {
  int roop_count;
  long *tmp_long_list, tmp_long;
  DBUG_ENTER("spider_increase_long_list");
  if (*list_length == link_count)
    DBUG_RETURN(0);
  if (*list_length > 1)
  {
    my_printf_error(ER_SPIDER_DIFFERENT_LINK_COUNT_NUM,
      ER_SPIDER_DIFFERENT_LINK_COUNT_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_DIFFERENT_LINK_COUNT_NUM);
  }

  if (*long_list)
    tmp_long = (*long_list)[0];
  else
    tmp_long = -1;

  if (!(tmp_long_list = (long*)
    spider_bulk_malloc(spider_current_trx, 41, MYF(MY_WME | MY_ZEROFILL),
      &tmp_long_list, (uint) (sizeof(long) * link_count),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  for (roop_count = 0; roop_count < (int) link_count; roop_count++)
  {
    tmp_long_list[roop_count] = tmp_long;
    DBUG_PRINT("info",("spider long_list[%d]=%ld", roop_count,
      tmp_long));
  }
  if (*long_list)
    spider_free(spider_current_trx, *long_list, MYF(0));
  *list_length = link_count;
  *long_list = tmp_long_list;

  DBUG_RETURN(0);
}

int spider_increase_longlong_list(
  longlong **longlong_list,
  uint *list_length,
  uint link_count
) {
  int roop_count;
  longlong *tmp_longlong_list, tmp_longlong;
  DBUG_ENTER("spider_increase_longlong_list");
  if (*list_length == link_count)
    DBUG_RETURN(0);
  if (*list_length > 1)
  {
    my_printf_error(ER_SPIDER_DIFFERENT_LINK_COUNT_NUM,
      ER_SPIDER_DIFFERENT_LINK_COUNT_STR, MYF(0));
    DBUG_RETURN(ER_SPIDER_DIFFERENT_LINK_COUNT_NUM);
  }

  if (*longlong_list)
    tmp_longlong = (*longlong_list)[0];
  else
    tmp_longlong = -1;

  if (!(tmp_longlong_list = (longlong*)
    spider_bulk_malloc(spider_current_trx, 42, MYF(MY_WME | MY_ZEROFILL),
      &tmp_longlong_list, (uint) (sizeof(longlong) * link_count),
      NullS))
  ) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  for (roop_count = 0; roop_count < (int) link_count; roop_count++)
  {
    tmp_longlong_list[roop_count] = tmp_longlong;
    DBUG_PRINT("info",("spider longlong_list[%d]=%lld", roop_count,
      tmp_longlong));
  }
  if (*longlong_list)
    spider_free(spider_current_trx, *longlong_list, MYF(0));
  *list_length = link_count;
  *longlong_list = tmp_longlong_list;

  DBUG_RETURN(0);
}

static int spider_set_ll_value(
  longlong *value,
  char *str
) {
  int error_num = 0;
  DBUG_ENTER("spider_set_ll_value");
  *value = my_strtoll10(str, (char**) NULL, &error_num);
  DBUG_RETURN(error_num);
}

/**
  Print a parameter string error message.

  @return                   Error code.
*/

int st_spider_param_string_parse::print_param_error()
{
  if (start_title_ptr)
  {
    /* Restore the input delimiter characters */
    restore_delims();

    /* Print the error message */
    switch (error_num)
    {
    case ER_SPIDER_INVALID_UDF_PARAM_NUM:
      my_printf_error(error_num, ER_SPIDER_INVALID_UDF_PARAM_STR,
                      MYF(0), start_title_ptr);
      break;
    case ER_SPIDER_INVALID_CONNECT_INFO_NUM:
    default:
      my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_STR,
                      MYF(0), start_title_ptr);
    }

    return error_num;
  }
  else
    return 0;
}

#define SPIDER_PARAM_STR_LEN(name) name ## _length
#define SPIDER_PARAM_STR(title_name, param_name) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (!share->param_name) \
    { \
      if ((share->param_name = spider_get_string_between_quote( \
        start_ptr, TRUE, &connect_string_parse))) \
        share->SPIDER_PARAM_STR_LEN(param_name) = strlen(share->param_name); \
      else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%s", share->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_STR_LENS(name) name ## _lengths
#define SPIDER_PARAM_STR_CHARLEN(name) name ## _charlen
#define SPIDER_PARAM_STR_LIST(title_name, param_name) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (!share->param_name) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        share->SPIDER_PARAM_STR_CHARLEN(param_name) = strlen(tmp_ptr2); \
        if ((error_num = spider_create_string_list( \
          &share->param_name, \
          &share->SPIDER_PARAM_STR_LENS(param_name), \
          &share->SPIDER_PARAM_STR_LEN(param_name), \
          tmp_ptr2, \
          share->SPIDER_PARAM_STR_CHARLEN(param_name), \
          &connect_string_parse))) \
          goto error; \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
    } \
    break; \
  }
#define SPIDER_PARAM_HINT(title_name, param_name, check_length, max_size, append_method) \
  if (!strncasecmp(tmp_ptr, title_name, check_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    DBUG_PRINT("info",("spider max_size=%d", max_size)); \
    int hint_num = atoi(tmp_ptr + check_length); \
    DBUG_PRINT("info",("spider hint_num=%d", hint_num)); \
    DBUG_PRINT("info",("spider share->param_name=%p", share->param_name)); \
    if (share->param_name) \
    { \
      if (hint_num < 0 || hint_num >= max_size) \
      { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } else if (share->param_name[hint_num].length() > 0) \
        break; \
      char *hint_str = spider_get_string_between_quote(start_ptr, FALSE); \
      if ((error_num = \
        append_method(&share->param_name[hint_num], hint_str))) \
        goto error; \
      DBUG_PRINT("info",("spider " title_name "[%d]=%s", hint_num, \
        share->param_name[hint_num].ptr())); \
    } else { \
      error_num = connect_string_parse.print_param_error(); \
      goto error; \
    } \
    break; \
  }
#define SPIDER_PARAM_NUMHINT(title_name, param_name, check_length, max_size, append_method) \
  if (!strncasecmp(tmp_ptr, title_name, check_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    DBUG_PRINT("info",("spider max_size=%d", max_size)); \
    int hint_num = atoi(tmp_ptr + check_length); \
    DBUG_PRINT("info",("spider hint_num=%d", hint_num)); \
    DBUG_PRINT("info",("spider share->param_name=%p", share->param_name)); \
    if (share->param_name) \
    { \
      if (hint_num < 0 || hint_num >= max_size) \
      { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } else if (share->param_name[hint_num] != -1) \
        break; \
      char *hint_str = spider_get_string_between_quote(start_ptr, FALSE); \
      if ((error_num = \
        append_method(&share->param_name[hint_num], hint_str))) \
        goto error; \
      DBUG_PRINT("info",("spider " title_name "[%d]=%lld", hint_num, \
        share->param_name[hint_num])); \
    } else { \
      error_num = connect_string_parse.print_param_error(); \
      goto error; \
    } \
    break; \
  }
#define SPIDER_PARAM_LONG_LEN(name) name ## _length
#define SPIDER_PARAM_LONG_LIST_WITH_MAX(title_name, param_name, \
  min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (!share->param_name) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        if ((error_num = spider_create_long_list( \
          &share->param_name, \
          &share->SPIDER_PARAM_LONG_LEN(param_name), \
          tmp_ptr2, \
          strlen(tmp_ptr2), \
          min_val, max_val, \
          &connect_string_parse))) \
          goto error; \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
    } \
    break; \
  }
#define SPIDER_PARAM_LONGLONG_LEN(name) name ## _length
#define SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(title_name, param_name, \
  min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (!share->param_name) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        if ((error_num = spider_create_longlong_list( \
          &share->param_name, \
          &share->SPIDER_PARAM_LONGLONG_LEN(param_name), \
          tmp_ptr2, \
          strlen(tmp_ptr2), \
          min_val, max_val, \
          &connect_string_parse))) \
          goto error; \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
    } \
    break; \
  }
#define SPIDER_PARAM_INT_WITH_MAX(title_name, param_name, min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (share->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        share->param_name = atoi(tmp_ptr2); \
        if (share->param_name < min_val) \
          share->param_name = min_val; \
        else if (share->param_name > max_val) \
          share->param_name = max_val; \
        connect_string_parse.set_param_value(tmp_ptr2, \
                                             tmp_ptr2 + \
                                               strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%d", share->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_INT(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (share->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        share->param_name = atoi(tmp_ptr2); \
        if (share->param_name < min_val) \
          share->param_name = min_val; \
        connect_string_parse.set_param_value(tmp_ptr2, \
                                             tmp_ptr2 + \
                                               strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%d", share->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_DOUBLE(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (share->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        share->param_name = my_atof(tmp_ptr2); \
        if (share->param_name < min_val) \
          share->param_name = min_val; \
        connect_string_parse.set_param_value(tmp_ptr2, \
                                             tmp_ptr2 + \
                                               strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%f", share->param_name)); \
    } \
    break; \
  }
#define SPIDER_PARAM_LONGLONG(title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("spider " title_name " start")); \
    if (share->param_name == -1) \
    { \
      if ((tmp_ptr2 = spider_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        share->param_name = my_strtoll10(tmp_ptr2, (char**) NULL, &error_num); \
        if (share->param_name < min_val) \
          share->param_name = min_val; \
        connect_string_parse.set_param_value(tmp_ptr2, \
                                             tmp_ptr2 + \
                                               strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = connect_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("spider " title_name "=%lld", share->param_name)); \
    } \
    break; \
  }

int spider_parse_connect_info(
  SPIDER_SHARE *share,
  TABLE_SHARE *table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info,
#endif
  uint create_table
) {
  int error_num = 0;
  char *connect_string = NULL;
  char *sprit_ptr;
  char *tmp_ptr, *tmp_ptr2, *start_ptr;
  int roop_count;
  int title_length;
  SPIDER_PARAM_STRING_PARSE connect_string_parse;
  SPIDER_ALTER_TABLE *share_alter;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_element *part_elem;
  partition_element *sub_elem;
#endif
  DBUG_ENTER("spider_parse_connect_info");
#ifdef WITH_PARTITION_STORAGE_ENGINE
#if MYSQL_VERSION_ID < 50500
  DBUG_PRINT("info",("spider partition_info=%s", table_share->partition_info));
#else
  DBUG_PRINT("info",("spider partition_info=%s",
    table_share->partition_info_str));
#endif
  DBUG_PRINT("info",("spider part_info=%p", part_info));
#endif
  DBUG_PRINT("info",("spider s->db=%s", table_share->db.str));
  DBUG_PRINT("info",("spider s->table_name=%s", table_share->table_name.str));
  DBUG_PRINT("info",("spider s->path=%s", table_share->path.str));
  DBUG_PRINT("info",
    ("spider s->normalized_path=%s", table_share->normalized_path.str));
#ifdef WITH_PARTITION_STORAGE_ENGINE
  spider_get_partition_info(share->table_name, share->table_name_length,
    table_share, part_info, &part_elem, &sub_elem);
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
  share->sts_bg_mode = -1;
#endif
  share->sts_interval = -1;
  share->sts_mode = -1;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  share->sts_sync = -1;
#endif
  share->store_last_sts = -1;
  share->load_sts_at_startup = -1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  share->crd_bg_mode = -1;
#endif
  share->crd_interval = -1;
  share->crd_mode = -1;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  share->crd_sync = -1;
#endif
  share->store_last_crd = -1;
  share->load_crd_at_startup = -1;
  share->crd_type = -1;
  share->crd_weight = -1;
  share->internal_offset = -1;
  share->internal_limit = -1;
  share->split_read = -1;
  share->semi_split_read = -1;
  share->semi_split_read_limit = -1;
  share->init_sql_alloc_size = -1;
  share->reset_sql_alloc = -1;
  share->multi_split_read = -1;
  share->max_order = -1;
  share->semi_table_lock = -1;
  share->semi_table_lock_conn = -1;
  share->selupd_lock_mode = -1;
  share->query_cache = -1;
  share->query_cache_sync = -1;
  share->internal_delayed = -1;
  share->bulk_size = -1;
  share->bulk_update_mode = -1;
  share->bulk_update_size = -1;
  share->buffer_size = -1;
  share->internal_optimize = -1;
  share->internal_optimize_local = -1;
  share->scan_rate = -1;
  share->read_rate = -1;
  share->priority = -1;
  share->quick_mode = -1;
  share->quick_page_size = -1;
  share->quick_page_byte = -1;
  share->low_mem_read = -1;
  share->table_count_mode = -1;
  share->select_column_mode = -1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  share->bgs_mode = -1;
  share->bgs_first_read = -1;
  share->bgs_second_read = -1;
#endif
  share->first_read = -1;
  share->second_read = -1;
  share->auto_increment_mode = -1;
  share->use_table_charset = -1;
  share->use_pushdown_udf = -1;
  share->skip_default_condition = -1;
  share->skip_parallel_search = -1;
  share->direct_dup_insert = -1;
  share->direct_order_limit = -1;
  share->bka_mode = -1;
  share->read_only_mode = -1;
  share->error_read_mode = -1;
  share->error_write_mode = -1;
  share->active_link_count = -1;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  share->hs_result_free_size = -1;
#endif
#ifdef HA_CAN_BULK_ACCESS
  share->bulk_access_free = -1;
#endif
#ifdef HA_CAN_FORCE_BULK_UPDATE
  share->force_bulk_update = -1;
#endif
#ifdef HA_CAN_FORCE_BULK_DELETE
  share->force_bulk_delete = -1;
#endif
  share->casual_read = -1;
  share->delete_all_rows_type = -1;
  share->static_records_for_status = -1;
  share->static_mean_rec_length = -1;
  for (roop_count = 0; roop_count < (int) table_share->keys; roop_count++)
  {
    share->static_key_cardinality[roop_count] = -1;
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  for (roop_count = 4; roop_count > 0; roop_count--)
#else
  for (roop_count = 2; roop_count > 0; roop_count--)
#endif
  {
    if (connect_string)
    {
      spider_free(spider_current_trx, connect_string, MYF(0));
      connect_string = NULL;
    }
    switch (roop_count)
    {
#ifdef WITH_PARTITION_STORAGE_ENGINE
      case 4:
        if (!sub_elem || !sub_elem->part_comment)
          continue;
        DBUG_PRINT("info",("spider create sub comment string"));
        if (
          !(connect_string = spider_create_string(
            sub_elem->part_comment,
            strlen(sub_elem->part_comment)))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_conn_string;
        }
        DBUG_PRINT("info",("spider sub comment string=%s", connect_string));
        break;
      case 3:
        if (!part_elem || !part_elem->part_comment)
          continue;
        DBUG_PRINT("info",("spider create part comment string"));
        if (
          !(connect_string = spider_create_string(
            part_elem->part_comment,
            strlen(part_elem->part_comment)))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_conn_string;
        }
        DBUG_PRINT("info",("spider part comment string=%s", connect_string));
        break;
#endif
      case 2:
        if (table_share->comment.length == 0)
          continue;
        DBUG_PRINT("info",("spider create comment string"));
        if (
          !(connect_string = spider_create_string(
            table_share->comment.str,
            table_share->comment.length))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_conn_string;
        }
        DBUG_PRINT("info",("spider comment string=%s", connect_string));
        break;
      default:
        if (table_share->connect_string.length == 0)
          continue;
        DBUG_PRINT("info",("spider create connect_string string"));
        if (
          !(connect_string = spider_create_string(
            table_share->connect_string.str,
            table_share->connect_string.length))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_conn_string;
        }
        DBUG_PRINT("info",("spider connect_string=%s", connect_string));
        break;
    }

    sprit_ptr = connect_string;
    connect_string_parse.init(connect_string, ER_SPIDER_INVALID_CONNECT_INFO_NUM);
    while (sprit_ptr)
    {
      tmp_ptr = sprit_ptr;
      while (*tmp_ptr == ' ' || *tmp_ptr == '\r' ||
        *tmp_ptr == '\n' || *tmp_ptr == '\t')
        tmp_ptr++;

      if (*tmp_ptr == '\0')
        break;

      title_length = 0;
      start_ptr = tmp_ptr;
      while (*start_ptr != ' ' && *start_ptr != '\'' &&
        *start_ptr != '"' && *start_ptr != '\0' &&
        *start_ptr != '\r' && *start_ptr != '\n' &&
        *start_ptr != '\t')
      {
        title_length++;
        start_ptr++;
      }
      connect_string_parse.set_param_title(tmp_ptr, tmp_ptr + title_length);
      if ((error_num = connect_string_parse.get_next_parameter_head(
        start_ptr, &sprit_ptr)))
      {
        goto error;
      }

      switch (title_length)
      {
        case 0:
          error_num = connect_string_parse.print_param_error();
          if (error_num)
            goto error;
          continue;
        case 3:
          SPIDER_PARAM_LONG_LIST_WITH_MAX("abl", access_balances, 0,
            2147483647);
          SPIDER_PARAM_INT_WITH_MAX("aim", auto_increment_mode, 0, 3);
          SPIDER_PARAM_INT("alc", active_link_count, 1);
#ifdef HA_CAN_BULK_ACCESS
          SPIDER_PARAM_INT_WITH_MAX("baf", bulk_access_free, 0, 1);
#endif
          SPIDER_PARAM_INT("bfz", buffer_size, 0);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_LONGLONG("bfr", bgs_first_read, 0);
          SPIDER_PARAM_INT("bmd", bgs_mode, 0);
          SPIDER_PARAM_LONGLONG("bsr", bgs_second_read, 0);
#endif
          SPIDER_PARAM_STR("bke", bka_engine);
          SPIDER_PARAM_INT_WITH_MAX("bkm", bka_mode, 0, 2);
          SPIDER_PARAM_INT("bsz", bulk_size, 0);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("btt", bka_table_name_types,
            0, 1);
          SPIDER_PARAM_INT_WITH_MAX("bum", bulk_update_mode, 0, 2);
          SPIDER_PARAM_INT("bus", bulk_update_size, 0);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_INT_WITH_MAX("cbm", crd_bg_mode, 0, 2);
#endif
          SPIDER_PARAM_DOUBLE("civ", crd_interval, 0);
          SPIDER_PARAM_INT_WITH_MAX("cmd", crd_mode, 0, 3);
          SPIDER_PARAM_INT_WITH_MAX("csr", casual_read, 0, 63);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          SPIDER_PARAM_INT_WITH_MAX("csy", crd_sync, 0, 2);
#endif
          SPIDER_PARAM_LONG_LIST_WITH_MAX("cto", connect_timeouts, 0,
            2147483647);
          SPIDER_PARAM_INT_WITH_MAX("ctp", crd_type, 0, 2);
          SPIDER_PARAM_DOUBLE("cwg", crd_weight, 1);
          SPIDER_PARAM_INT_WITH_MAX("dat", delete_all_rows_type, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("ddi", direct_dup_insert, 0, 1);
          SPIDER_PARAM_STR_LIST("dff", tgt_default_files);
          SPIDER_PARAM_STR_LIST("dfg", tgt_default_groups);
          SPIDER_PARAM_LONGLONG("dol", direct_order_limit, 0);
          SPIDER_PARAM_STR_LIST("drv", tgt_drivers);
          SPIDER_PARAM_STR_LIST("dsn", tgt_dsns);
          SPIDER_PARAM_INT_WITH_MAX("erm", error_read_mode, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("ewm", error_write_mode, 0, 1);
#ifdef HA_CAN_FORCE_BULK_DELETE
          SPIDER_PARAM_INT_WITH_MAX("fbd", force_bulk_delete, 0, 1);
#endif
#ifdef HA_CAN_FORCE_BULK_UPDATE
          SPIDER_PARAM_INT_WITH_MAX("fbu", force_bulk_update, 0, 1);
#endif
          SPIDER_PARAM_STR_LIST("fds", tgt_filedsns);
          SPIDER_PARAM_LONGLONG("frd", first_read, 0);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONGLONG("hrf", hs_result_free_size, 0);
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "hrp", hs_read_ports, 0, 65535);
          SPIDER_PARAM_STR_LIST("hrs", hs_read_socks);
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "hwp", hs_write_ports, 0, 65535);
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "hwr", hs_write_to_reads, 0, 1);
          SPIDER_PARAM_STR_LIST("hws", hs_write_socks);
#endif
          SPIDER_PARAM_INT("isa", init_sql_alloc_size, 0);
          SPIDER_PARAM_INT_WITH_MAX("idl", internal_delayed, 0, 1);
          SPIDER_PARAM_LONGLONG("ilm", internal_limit, 0);
          SPIDER_PARAM_LONGLONG("ios", internal_offset, 0);
          SPIDER_PARAM_INT_WITH_MAX("iom", internal_optimize, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("iol", internal_optimize_local, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("lmr", low_mem_read, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("lcs", load_crd_at_startup, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("lss", load_sts_at_startup, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("lst", link_statuses, 0, 3);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_LONG_LIST_WITH_MAX("mbf", monitoring_bg_flag, 0, 1);
          SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(
            "mbi", monitoring_bg_interval, 0, 4294967295LL);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("mbk", monitoring_bg_kind, 0, 3);
#endif
          SPIDER_PARAM_LONG_LIST_WITH_MAX("mbp", monitoring_binlog_pos_at_failing, 0, 2);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("mfg", monitoring_flag, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("mkd", monitoring_kind, 0, 3);
          SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(
            "mlt", monitoring_limit, 0, 9223372036854775807LL);
          SPIDER_PARAM_INT("mod", max_order, 0);
          SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(
            "msi", monitoring_sid, 0, 4294967295LL);
          SPIDER_PARAM_INT_WITH_MAX("msr", multi_split_read, 0, 2147483647);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("nrt", net_read_timeouts, 0,
            2147483647);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("nwt", net_write_timeouts, 0,
            2147483647);
          SPIDER_PARAM_STR_LIST("pkn", tgt_pk_names);
          SPIDER_PARAM_LONGLONG("prt", priority, 0);
          SPIDER_PARAM_INT_WITH_MAX("qch", query_cache, 0, 2);
          SPIDER_PARAM_INT_WITH_MAX("qcs", query_cache_sync, 0, 3);
          SPIDER_PARAM_INT_WITH_MAX("qmd", quick_mode, 0, 3);
          SPIDER_PARAM_LONGLONG("qpb", quick_page_byte, 0);
          SPIDER_PARAM_LONGLONG("qps", quick_page_size, 0);
          SPIDER_PARAM_INT_WITH_MAX("rom", read_only_mode, 0, 1);
          SPIDER_PARAM_DOUBLE("rrt", read_rate, 0);
          SPIDER_PARAM_INT_WITH_MAX("rsa", reset_sql_alloc, 0, 1);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_INT_WITH_MAX("sbm", sts_bg_mode, 0, 2);
#endif
          SPIDER_PARAM_STR_LIST("sca", tgt_ssl_cas);
          SPIDER_PARAM_STR_LIST("sch", tgt_ssl_ciphers);
          SPIDER_PARAM_INT_WITH_MAX("scm", select_column_mode, 0, 1);
          SPIDER_PARAM_STR_LIST("scp", tgt_ssl_capaths);
          SPIDER_PARAM_STR_LIST("scr", tgt_ssl_certs);
          SPIDER_PARAM_INT_WITH_MAX("sdc", skip_default_condition, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("sgb", strict_group_bys, 0, 1);
          SPIDER_PARAM_DOUBLE("siv", sts_interval, 0);
          SPIDER_PARAM_STR_LIST("sky", tgt_ssl_keys);
          SPIDER_PARAM_STR_LIST("sli", static_link_ids);
          SPIDER_PARAM_INT_WITH_MAX("slc", store_last_crd, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("slm", selupd_lock_mode, 0, 2);
          SPIDER_PARAM_INT_WITH_MAX("sls", store_last_sts, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("smd", sts_mode, 1, 2);
          SPIDER_PARAM_LONGLONG("smr", static_mean_rec_length, 0);
          SPIDER_PARAM_LONGLONG("spr", split_read, 0);
          SPIDER_PARAM_INT_WITH_MAX("sps", skip_parallel_search, 0, 3);
          SPIDER_PARAM_STR_LIST("sqn", tgt_sequence_names);
          SPIDER_PARAM_LONGLONG("srd", second_read, 0);
          SPIDER_PARAM_DOUBLE("srt", scan_rate, 0);
          SPIDER_PARAM_STR_LIST("srv", server_names);
          SPIDER_PARAM_DOUBLE("ssr", semi_split_read, 0);
          SPIDER_PARAM_LONGLONG("ssl", semi_split_read_limit, 0);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          SPIDER_PARAM_INT_WITH_MAX("ssy", sts_sync, 0, 2);
#endif
          SPIDER_PARAM_INT_WITH_MAX("stc", semi_table_lock_conn, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("stl", semi_table_lock, 0, 1);
          SPIDER_PARAM_LONGLONG("srs", static_records_for_status, 0);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("svc", tgt_ssl_vscs, 0, 1);
          SPIDER_PARAM_STR_LIST("tbl", tgt_table_names);
          SPIDER_PARAM_INT_WITH_MAX("tcm", table_count_mode, 0, 3);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("uhd", use_handlers, 0, 3);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "uhr", use_hs_reads, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "uhw", use_hs_writes, 0, 1);
#endif
          SPIDER_PARAM_INT_WITH_MAX("upu", use_pushdown_udf, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("utc", use_table_charset, 0, 1);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 4:
          SPIDER_PARAM_STR_LIST("host", tgt_hosts);
          SPIDER_PARAM_STR_LIST("user", tgt_usernames);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("port", tgt_ports, 0, 65535);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 5:
          SPIDER_PARAM_STR_LIST("table", tgt_table_names);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 6:
          SPIDER_PARAM_STR_LIST("driver", tgt_drivers);
          SPIDER_PARAM_STR_LIST("server", server_names);
          SPIDER_PARAM_STR_LIST("socket", tgt_sockets);
          SPIDER_PARAM_HINT("idx", key_hint, 3, (int) table_share->keys,
            spider_db_append_key_hint);
          SPIDER_PARAM_STR_LIST("ssl_ca", tgt_ssl_cas);
          SPIDER_PARAM_NUMHINT("skc", static_key_cardinality, 3,
            (int) table_share->keys, spider_set_ll_value);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 7:
          SPIDER_PARAM_STR_LIST("filedsn", tgt_filedsns);
          SPIDER_PARAM_STR_LIST("wrapper", tgt_wrappers);
          SPIDER_PARAM_STR_LIST("ssl_key", tgt_ssl_keys);
          SPIDER_PARAM_STR_LIST("pk_name", tgt_pk_names);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 8:
          SPIDER_PARAM_STR_LIST("database", tgt_dbs);
          SPIDER_PARAM_STR_LIST("password", tgt_passwords);
          SPIDER_PARAM_INT_WITH_MAX("sts_mode", sts_mode, 1, 2);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          SPIDER_PARAM_INT_WITH_MAX("sts_sync", sts_sync, 0, 2);
#endif
          SPIDER_PARAM_INT_WITH_MAX("crd_mode", crd_mode, 0, 3);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          SPIDER_PARAM_INT_WITH_MAX("crd_sync", crd_sync, 0, 2);
#endif
          SPIDER_PARAM_INT_WITH_MAX("crd_type", crd_type, 0, 2);
          SPIDER_PARAM_LONGLONG("priority", priority, 0);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_INT("bgs_mode", bgs_mode, 0);
#endif
          SPIDER_PARAM_STR_LIST("ssl_cert", tgt_ssl_certs);
          SPIDER_PARAM_INT_WITH_MAX("bka_mode", bka_mode, 0, 2);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 9:
          SPIDER_PARAM_INT("max_order", max_order, 0);
          SPIDER_PARAM_INT("bulk_size", bulk_size, 0);
          SPIDER_PARAM_DOUBLE("scan_rate", scan_rate, 0);
          SPIDER_PARAM_DOUBLE("read_rate", read_rate, 0);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 10:
          SPIDER_PARAM_DOUBLE("crd_weight", crd_weight, 1);
          SPIDER_PARAM_LONGLONG("split_read", split_read, 0);
          SPIDER_PARAM_INT_WITH_MAX("quick_mode", quick_mode, 0, 3);
          SPIDER_PARAM_STR_LIST("ssl_cipher", tgt_ssl_ciphers);
          SPIDER_PARAM_STR_LIST("ssl_capath", tgt_ssl_capaths);
          SPIDER_PARAM_STR("bka_engine", bka_engine);
          SPIDER_PARAM_LONGLONG("first_read", first_read, 0);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 11:
          SPIDER_PARAM_INT_WITH_MAX("query_cache", query_cache, 0, 2);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_INT_WITH_MAX("crd_bg_mode", crd_bg_mode, 0, 2);
          SPIDER_PARAM_INT_WITH_MAX("sts_bg_mode", sts_bg_mode, 0, 2);
#endif
          SPIDER_PARAM_LONG_LIST_WITH_MAX("link_status", link_statuses, 0, 3);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("use_handler", use_handlers, 0, 3);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONG_LIST_WITH_MAX("use_hs_read", use_hs_reads, 0, 1);
#endif
          SPIDER_PARAM_INT_WITH_MAX("casual_read", casual_read, 0, 63);
          SPIDER_PARAM_INT("buffer_size", buffer_size, 0);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 12:
          SPIDER_PARAM_DOUBLE("sts_interval", sts_interval, 0);
          SPIDER_PARAM_DOUBLE("crd_interval", crd_interval, 0);
          SPIDER_PARAM_INT_WITH_MAX("low_mem_read", low_mem_read, 0, 1);
          SPIDER_PARAM_STR_LIST("default_file", tgt_default_files);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "use_hs_write", use_hs_writes, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "hs_read_port", hs_read_ports, 0, 65535);
#endif
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 13:
          SPIDER_PARAM_STR_LIST("default_group", tgt_default_groups);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "hs_write_port", hs_write_ports, 0, 65535);
#endif
          SPIDER_PARAM_STR_LIST("sequence_name", tgt_sequence_names);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 14:
          SPIDER_PARAM_LONGLONG("internal_limit", internal_limit, 0);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_LONGLONG("bgs_first_read", bgs_first_read, 0);
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_STR_LIST(
            "hs_read_socket", hs_read_socks);
#endif
          SPIDER_PARAM_INT_WITH_MAX("read_only_mode", read_only_mode, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("access_balance", access_balances, 0,
            2147483647);
          SPIDER_PARAM_STR_LIST("static_link_id", static_link_ids);
          SPIDER_PARAM_INT_WITH_MAX("store_last_crd", store_last_crd, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("store_last_sts", store_last_sts, 0, 1);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 15:
          SPIDER_PARAM_LONGLONG("internal_offset", internal_offset, 0);
          SPIDER_PARAM_INT_WITH_MAX("reset_sql_alloc", reset_sql_alloc, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("semi_table_lock", semi_table_lock, 0, 1);
          SPIDER_PARAM_LONGLONG("quick_page_byte", quick_page_byte, 0);
          SPIDER_PARAM_LONGLONG("quick_page_size", quick_page_size, 0);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_LONGLONG("bgs_second_read", bgs_second_read, 0);
#endif
          SPIDER_PARAM_LONG_LIST_WITH_MAX("monitoring_flag", monitoring_flag, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("monitoring_kind", monitoring_kind, 0, 3);
          SPIDER_PARAM_DOUBLE("semi_split_read", semi_split_read, 0);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_STR_LIST(
            "hs_write_socket", hs_write_socks);
#endif
          SPIDER_PARAM_LONG_LIST_WITH_MAX("connect_timeout", connect_timeouts,
            0, 2147483647);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("strict_group_by",
            strict_group_bys, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX("error_read_mode", error_read_mode, 0, 1);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 16:
          SPIDER_PARAM_INT_WITH_MAX(
            "multi_split_read", multi_split_read, 0, 2147483647);
          SPIDER_PARAM_INT_WITH_MAX(
            "selupd_lock_mode", selupd_lock_mode, 0, 2);
          SPIDER_PARAM_INT_WITH_MAX(
            "internal_delayed", internal_delayed, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX(
            "table_count_mode", table_count_mode, 0, 3);
          SPIDER_PARAM_INT_WITH_MAX(
            "use_pushdown_udf", use_pushdown_udf, 0, 1);
          SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(
            "monitoring_limit", monitoring_limit, 0, 9223372036854775807LL);
          SPIDER_PARAM_INT_WITH_MAX(
            "bulk_update_mode", bulk_update_mode, 0, 2);
          SPIDER_PARAM_INT("bulk_update_size", bulk_update_size, 0);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("net_read_timeout",
            net_read_timeouts, 0, 2147483647);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "hs_write_to_read", hs_write_to_reads, 0, 1);
#endif
          SPIDER_PARAM_INT_WITH_MAX(
            "error_write_mode", error_write_mode, 0, 1);
#ifdef HA_CAN_BULK_ACCESS
          SPIDER_PARAM_INT_WITH_MAX(
            "bulk_access_free", bulk_access_free, 0, 1);
#endif
          SPIDER_PARAM_INT_WITH_MAX(
            "query_cache_sync", query_cache_sync, 0, 3);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 17:
          SPIDER_PARAM_INT_WITH_MAX(
            "internal_optimize", internal_optimize, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX(
            "use_table_charset", use_table_charset, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX(
            "direct_dup_insert", direct_dup_insert, 0, 1);
          SPIDER_PARAM_INT("active_link_count", active_link_count, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX("net_write_timeout",
            net_write_timeouts, 0, 2147483647);
#ifdef HA_CAN_FORCE_BULK_DELETE
          SPIDER_PARAM_INT_WITH_MAX(
            "force_bulk_delete", force_bulk_delete, 0, 1);
#endif
#ifdef HA_CAN_FORCE_BULK_UPDATE
          SPIDER_PARAM_INT_WITH_MAX(
            "force_bulk_update", force_bulk_update, 0, 1);
#endif
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 18:
          SPIDER_PARAM_INT_WITH_MAX(
            "select_column_mode", select_column_mode, 0, 1);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "monitoring_bg_flag", monitoring_bg_flag, 0, 1);
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "monitoring_bg_kind", monitoring_bg_kind, 0, 3);
#endif
          SPIDER_PARAM_LONGLONG(
            "direct_order_limit", direct_order_limit, 0);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 19:
          SPIDER_PARAM_INT("init_sql_alloc_size", init_sql_alloc_size, 0);
          SPIDER_PARAM_INT_WITH_MAX(
            "auto_increment_mode", auto_increment_mode, 0, 3);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          SPIDER_PARAM_LONGLONG("hs_result_free_size", hs_result_free_size, 0);
#endif
          SPIDER_PARAM_LONG_LIST_WITH_MAX("bka_table_name_type",
            bka_table_name_types, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX(
            "load_crd_at_startup", load_crd_at_startup, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX(
            "load_sts_at_startup", load_sts_at_startup, 0, 1);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 20:
          SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(
            "monitoring_server_id", monitoring_sid, 0, 4294967295LL);
          SPIDER_PARAM_INT_WITH_MAX(
            "delete_all_rows_type", delete_all_rows_type, 0, 1);
          SPIDER_PARAM_INT_WITH_MAX(
            "skip_parallel_search", skip_parallel_search, 0, 3);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 21:
          SPIDER_PARAM_LONGLONG(
            "semi_split_read_limit", semi_split_read_limit, 0);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 22:
          SPIDER_PARAM_LONG_LIST_WITH_MAX(
            "ssl_verify_server_cert", tgt_ssl_vscs, 0, 1);
#ifndef WITHOUT_SPIDER_BG_SEARCH
          SPIDER_PARAM_LONGLONG_LIST_WITH_MAX(
            "monitoring_bg_interval", monitoring_bg_interval, 0, 4294967295LL);
#endif
          SPIDER_PARAM_INT_WITH_MAX(
            "skip_default_condition", skip_default_condition, 0, 1);
          SPIDER_PARAM_LONGLONG(
            "static_mean_rec_length", static_mean_rec_length, 0);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 23:
          SPIDER_PARAM_INT_WITH_MAX(
            "internal_optimize_local", internal_optimize_local, 0, 1);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 25:
          SPIDER_PARAM_LONGLONG("static_records_for_status",
            static_records_for_status, 0);
          SPIDER_PARAM_NUMHINT("static_key_cardinality", static_key_cardinality,
            3, (int) table_share->keys, spider_set_ll_value);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 26:
          SPIDER_PARAM_INT_WITH_MAX(
            "semi_table_lock_connection", semi_table_lock_conn, 0, 1);
          error_num = connect_string_parse.print_param_error();
          goto error;
        case 32:
          SPIDER_PARAM_LONG_LIST_WITH_MAX("monitoring_binlog_pos_at_failing",
            monitoring_binlog_pos_at_failing, 0, 2);
          error_num = connect_string_parse.print_param_error();
          goto error;
        default:
          error_num = connect_string_parse.print_param_error();
          goto error;
      }

      /* Verify that the remainder of the parameter value is whitespace */
      if ((error_num = connect_string_parse.has_extra_parameter_values()))
          goto error;
    }
  }

  /* check all_link_count */
  share->all_link_count = 1;
  if (share->all_link_count < share->server_names_length)
    share->all_link_count = share->server_names_length;
  if (share->all_link_count < share->tgt_table_names_length)
    share->all_link_count = share->tgt_table_names_length;
  if (share->all_link_count < share->tgt_dbs_length)
    share->all_link_count = share->tgt_dbs_length;
  if (share->all_link_count < share->tgt_hosts_length)
    share->all_link_count = share->tgt_hosts_length;
  if (share->all_link_count < share->tgt_usernames_length)
    share->all_link_count = share->tgt_usernames_length;
  if (share->all_link_count < share->tgt_passwords_length)
    share->all_link_count = share->tgt_passwords_length;
  if (share->all_link_count < share->tgt_sockets_length)
    share->all_link_count = share->tgt_sockets_length;
  if (share->all_link_count < share->tgt_wrappers_length)
    share->all_link_count = share->tgt_wrappers_length;
  if (share->all_link_count < share->tgt_ssl_cas_length)
    share->all_link_count = share->tgt_ssl_cas_length;
  if (share->all_link_count < share->tgt_ssl_capaths_length)
    share->all_link_count = share->tgt_ssl_capaths_length;
  if (share->all_link_count < share->tgt_ssl_certs_length)
    share->all_link_count = share->tgt_ssl_certs_length;
  if (share->all_link_count < share->tgt_ssl_ciphers_length)
    share->all_link_count = share->tgt_ssl_ciphers_length;
  if (share->all_link_count < share->tgt_ssl_keys_length)
    share->all_link_count = share->tgt_ssl_keys_length;
  if (share->all_link_count < share->tgt_default_files_length)
    share->all_link_count = share->tgt_default_files_length;
  if (share->all_link_count < share->tgt_default_groups_length)
    share->all_link_count = share->tgt_default_groups_length;
  if (share->all_link_count < share->tgt_dsns_length)
    share->all_link_count = share->tgt_dsns_length;
  if (share->all_link_count < share->tgt_filedsns_length)
    share->all_link_count = share->tgt_filedsns_length;
  if (share->all_link_count < share->tgt_drivers_length)
    share->all_link_count = share->tgt_drivers_length;
  if (share->all_link_count < share->tgt_pk_names_length)
    share->all_link_count = share->tgt_pk_names_length;
  if (share->all_link_count < share->tgt_sequence_names_length)
    share->all_link_count = share->tgt_sequence_names_length;
  if (share->all_link_count < share->static_link_ids_length)
    share->all_link_count = share->static_link_ids_length;
  if (share->all_link_count < share->tgt_ports_length)
    share->all_link_count = share->tgt_ports_length;
  if (share->all_link_count < share->tgt_ssl_vscs_length)
    share->all_link_count = share->tgt_ssl_vscs_length;
  if (share->all_link_count < share->link_statuses_length)
    share->all_link_count = share->link_statuses_length;
  if (share->all_link_count < share->monitoring_binlog_pos_at_failing_length)
    share->all_link_count = share->monitoring_binlog_pos_at_failing_length;
  if (share->all_link_count < share->monitoring_flag_length)
    share->all_link_count = share->monitoring_flag_length;
  if (share->all_link_count < share->monitoring_kind_length)
    share->all_link_count = share->monitoring_kind_length;
  if (share->all_link_count < share->monitoring_limit_length)
    share->all_link_count = share->monitoring_limit_length;
  if (share->all_link_count < share->monitoring_sid_length)
    share->all_link_count = share->monitoring_sid_length;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (share->all_link_count < share->monitoring_bg_flag_length)
    share->all_link_count = share->monitoring_bg_flag_length;
  if (share->all_link_count < share->monitoring_bg_kind_length)
    share->all_link_count = share->monitoring_bg_kind_length;
  if (share->all_link_count < share->monitoring_bg_interval_length)
    share->all_link_count = share->monitoring_bg_interval_length;
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (share->all_link_count < share->use_hs_reads_length)
    share->all_link_count = share->use_hs_reads_length;
  if (share->all_link_count < share->use_hs_writes_length)
    share->all_link_count = share->use_hs_writes_length;
  if (share->all_link_count < share->hs_read_ports_length)
    share->all_link_count = share->hs_read_ports_length;
  if (share->all_link_count < share->hs_write_ports_length)
    share->all_link_count = share->hs_write_ports_length;
  if (share->all_link_count < share->hs_read_socks_length)
    share->all_link_count = share->hs_read_socks_length;
  if (share->all_link_count < share->hs_write_socks_length)
    share->all_link_count = share->hs_write_socks_length;
  if (share->all_link_count < share->hs_write_to_reads_length)
    share->all_link_count = share->hs_write_to_reads_length;
#endif
  if (share->all_link_count < share->use_handlers_length)
    share->all_link_count = share->use_handlers_length;
  if (share->all_link_count < share->connect_timeouts_length)
    share->all_link_count = share->connect_timeouts_length;
  if (share->all_link_count < share->net_read_timeouts_length)
    share->all_link_count = share->net_read_timeouts_length;
  if (share->all_link_count < share->net_write_timeouts_length)
    share->all_link_count = share->net_write_timeouts_length;
  if (share->all_link_count < share->access_balances_length)
    share->all_link_count = share->access_balances_length;
  if (share->all_link_count < share->bka_table_name_types_length)
    share->all_link_count = share->bka_table_name_types_length;
  if (share->all_link_count < share->strict_group_bys_length)
    share->all_link_count = share->strict_group_bys_length;
  if ((error_num = spider_increase_string_list(
    &share->server_names,
    &share->server_names_lengths,
    &share->server_names_length,
    &share->server_names_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_table_names,
    &share->tgt_table_names_lengths,
    &share->tgt_table_names_length,
    &share->tgt_table_names_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_dbs,
    &share->tgt_dbs_lengths,
    &share->tgt_dbs_length,
    &share->tgt_dbs_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_hosts,
    &share->tgt_hosts_lengths,
    &share->tgt_hosts_length,
    &share->tgt_hosts_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_usernames,
    &share->tgt_usernames_lengths,
    &share->tgt_usernames_length,
    &share->tgt_usernames_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_passwords,
    &share->tgt_passwords_lengths,
    &share->tgt_passwords_length,
    &share->tgt_passwords_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_sockets,
    &share->tgt_sockets_lengths,
    &share->tgt_sockets_length,
    &share->tgt_sockets_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_wrappers,
    &share->tgt_wrappers_lengths,
    &share->tgt_wrappers_length,
    &share->tgt_wrappers_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_ssl_cas,
    &share->tgt_ssl_cas_lengths,
    &share->tgt_ssl_cas_length,
    &share->tgt_ssl_cas_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_ssl_capaths,
    &share->tgt_ssl_capaths_lengths,
    &share->tgt_ssl_capaths_length,
    &share->tgt_ssl_capaths_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_ssl_certs,
    &share->tgt_ssl_certs_lengths,
    &share->tgt_ssl_certs_length,
    &share->tgt_ssl_certs_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_ssl_ciphers,
    &share->tgt_ssl_ciphers_lengths,
    &share->tgt_ssl_ciphers_length,
    &share->tgt_ssl_ciphers_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_ssl_keys,
    &share->tgt_ssl_keys_lengths,
    &share->tgt_ssl_keys_length,
    &share->tgt_ssl_keys_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_default_files,
    &share->tgt_default_files_lengths,
    &share->tgt_default_files_length,
    &share->tgt_default_files_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_default_groups,
    &share->tgt_default_groups_lengths,
    &share->tgt_default_groups_length,
    &share->tgt_default_groups_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_dsns,
    &share->tgt_dsns_lengths,
    &share->tgt_dsns_length,
    &share->tgt_dsns_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_filedsns,
    &share->tgt_filedsns_lengths,
    &share->tgt_filedsns_length,
    &share->tgt_filedsns_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_drivers,
    &share->tgt_drivers_lengths,
    &share->tgt_drivers_length,
    &share->tgt_drivers_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_pk_names,
    &share->tgt_pk_names_lengths,
    &share->tgt_pk_names_length,
    &share->tgt_pk_names_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->tgt_sequence_names,
    &share->tgt_sequence_names_lengths,
    &share->tgt_sequence_names_length,
    &share->tgt_sequence_names_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_null_string_list(
    &share->static_link_ids,
    &share->static_link_ids_lengths,
    &share->static_link_ids_length,
    &share->static_link_ids_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->tgt_ports,
    &share->tgt_ports_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->tgt_ssl_vscs,
    &share->tgt_ssl_vscs_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->link_statuses,
    &share->link_statuses_length,
    share->all_link_count)))
    goto error;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if ((error_num = spider_increase_long_list(
    &share->monitoring_bg_flag,
    &share->monitoring_bg_flag_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->monitoring_bg_kind,
    &share->monitoring_bg_kind_length,
    share->all_link_count)))
    goto error;
#endif
  if ((error_num = spider_increase_long_list(
    &share->monitoring_binlog_pos_at_failing,
    &share->monitoring_binlog_pos_at_failing_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->monitoring_flag,
    &share->monitoring_flag_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->monitoring_kind,
    &share->monitoring_kind_length,
    share->all_link_count)))
    goto error;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if ((error_num = spider_increase_longlong_list(
    &share->monitoring_bg_interval,
    &share->monitoring_bg_interval_length,
    share->all_link_count)))
    goto error;
#endif
  if ((error_num = spider_increase_longlong_list(
    &share->monitoring_limit,
    &share->monitoring_limit_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_longlong_list(
    &share->monitoring_sid,
    &share->monitoring_sid_length,
    share->all_link_count)))
    goto error;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if ((error_num = spider_increase_long_list(
    &share->use_hs_reads,
    &share->use_hs_reads_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->use_hs_writes,
    &share->use_hs_writes_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->hs_read_ports,
    &share->hs_read_ports_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->hs_write_ports,
    &share->hs_write_ports_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->hs_read_socks,
    &share->hs_read_socks_lengths,
    &share->hs_read_socks_length,
    &share->hs_read_socks_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_string_list(
    &share->hs_write_socks,
    &share->hs_write_socks_lengths,
    &share->hs_write_socks_length,
    &share->hs_write_socks_charlen,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->hs_write_to_reads,
    &share->hs_write_to_reads_length,
    share->all_link_count)))
    goto error;
#endif
  if ((error_num = spider_increase_long_list(
    &share->use_handlers,
    &share->use_handlers_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->connect_timeouts,
    &share->connect_timeouts_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->net_read_timeouts,
    &share->net_read_timeouts_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->net_write_timeouts,
    &share->net_write_timeouts_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->access_balances,
    &share->access_balances_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->bka_table_name_types,
    &share->bka_table_name_types_length,
    share->all_link_count)))
    goto error;
  if ((error_num = spider_increase_long_list(
    &share->strict_group_bys,
    &share->strict_group_bys_length,
    share->all_link_count)))
    goto error;

  /* copy for tables start */
  share_alter = &share->alter_table;
  share_alter->all_link_count = share->all_link_count;
  if (!(share_alter->tmp_server_names = (char **)
    spider_bulk_malloc(spider_current_trx, 43, MYF(MY_WME | MY_ZEROFILL),
      &share_alter->tmp_server_names,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_table_names,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_dbs,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_hosts,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_usernames,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_passwords,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_sockets,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_wrappers,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_cas,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_capaths,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_certs,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_ciphers,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_keys,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_default_files,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_default_groups,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_dsns,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_filedsns,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_tgt_drivers,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_static_link_ids,
      (uint) (sizeof(char *) * share->all_link_count),
      &share_alter->tmp_server_names_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_table_names_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_dbs_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_hosts_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_usernames_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_passwords_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_sockets_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_wrappers_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_cas_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_capaths_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_certs_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_ciphers_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_keys_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_default_files_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_default_groups_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_dsns_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_filedsns_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_drivers_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_static_link_ids_lengths,
      (uint) (sizeof(uint *) * share->all_link_count),
      &share_alter->tmp_tgt_ports,
      (uint) (sizeof(long) * share->all_link_count),
      &share_alter->tmp_tgt_ssl_vscs,
      (uint) (sizeof(long) * share->all_link_count),
      &share_alter->tmp_monitoring_binlog_pos_at_failing,
      (uint) (sizeof(long) * share->all_link_count),
      &share_alter->tmp_link_statuses,
      (uint) (sizeof(long) * share->all_link_count),
      NullS))
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }


  memcpy(share_alter->tmp_server_names, share->server_names,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_table_names, share->tgt_table_names,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_dbs, share->tgt_dbs,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_hosts, share->tgt_hosts,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_usernames, share->tgt_usernames,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_passwords, share->tgt_passwords,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_sockets, share->tgt_sockets,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_wrappers, share->tgt_wrappers,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_cas, share->tgt_ssl_cas,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_capaths, share->tgt_ssl_capaths,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_certs, share->tgt_ssl_certs,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_ciphers, share->tgt_ssl_ciphers,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_keys, share->tgt_ssl_keys,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_default_files, share->tgt_default_files,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_default_groups, share->tgt_default_groups,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_dsns, share->tgt_dsns,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_filedsns, share->tgt_filedsns,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_drivers, share->tgt_drivers,
    sizeof(char *) * share->all_link_count);
  memcpy(share_alter->tmp_static_link_ids, share->static_link_ids,
    sizeof(char *) * share->all_link_count);

  memcpy(share_alter->tmp_tgt_ports, share->tgt_ports,
    sizeof(long) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_vscs, share->tgt_ssl_vscs,
    sizeof(long) * share->all_link_count);
  memcpy(share_alter->tmp_monitoring_binlog_pos_at_failing,
    share->monitoring_binlog_pos_at_failing,
    sizeof(long) * share->all_link_count);
  memcpy(share_alter->tmp_link_statuses, share->link_statuses,
    sizeof(long) * share->all_link_count);

  memcpy(share_alter->tmp_server_names_lengths,
    share->server_names_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_table_names_lengths,
    share->tgt_table_names_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_dbs_lengths, share->tgt_dbs_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_hosts_lengths, share->tgt_hosts_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_usernames_lengths,
    share->tgt_usernames_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_passwords_lengths,
    share->tgt_passwords_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_sockets_lengths, share->tgt_sockets_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_wrappers_lengths,
    share->tgt_wrappers_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_cas_lengths,
    share->tgt_ssl_cas_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_capaths_lengths,
    share->tgt_ssl_capaths_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_certs_lengths,
    share->tgt_ssl_certs_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_ciphers_lengths,
    share->tgt_ssl_ciphers_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_ssl_keys_lengths,
    share->tgt_ssl_keys_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_default_files_lengths,
    share->tgt_default_files_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_default_groups_lengths,
    share->tgt_default_groups_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_dsns_lengths,
    share->tgt_dsns_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_filedsns_lengths,
    share->tgt_filedsns_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_tgt_drivers_lengths,
    share->tgt_drivers_lengths,
    sizeof(uint) * share->all_link_count);
  memcpy(share_alter->tmp_static_link_ids_lengths,
    share->static_link_ids_lengths,
    sizeof(uint) * share->all_link_count);

  share_alter->tmp_server_names_charlen = share->server_names_charlen;
  share_alter->tmp_tgt_table_names_charlen = share->tgt_table_names_charlen;
  share_alter->tmp_tgt_dbs_charlen = share->tgt_dbs_charlen;
  share_alter->tmp_tgt_hosts_charlen = share->tgt_hosts_charlen;
  share_alter->tmp_tgt_usernames_charlen = share->tgt_usernames_charlen;
  share_alter->tmp_tgt_passwords_charlen = share->tgt_passwords_charlen;
  share_alter->tmp_tgt_sockets_charlen = share->tgt_sockets_charlen;
  share_alter->tmp_tgt_wrappers_charlen = share->tgt_wrappers_charlen;
  share_alter->tmp_tgt_ssl_cas_charlen = share->tgt_ssl_cas_charlen;
  share_alter->tmp_tgt_ssl_capaths_charlen = share->tgt_ssl_capaths_charlen;
  share_alter->tmp_tgt_ssl_certs_charlen = share->tgt_ssl_certs_charlen;
  share_alter->tmp_tgt_ssl_ciphers_charlen = share->tgt_ssl_ciphers_charlen;
  share_alter->tmp_tgt_ssl_keys_charlen = share->tgt_ssl_keys_charlen;
  share_alter->tmp_tgt_default_files_charlen =
    share->tgt_default_files_charlen;
  share_alter->tmp_tgt_default_groups_charlen =
    share->tgt_default_groups_charlen;
  share_alter->tmp_tgt_dsns_charlen =
    share->tgt_dsns_charlen;
  share_alter->tmp_tgt_filedsns_charlen =
    share->tgt_filedsns_charlen;
  share_alter->tmp_tgt_drivers_charlen =
    share->tgt_drivers_charlen;
  share_alter->tmp_static_link_ids_charlen =
    share->static_link_ids_charlen;

  share_alter->tmp_server_names_length = share->server_names_length;
  share_alter->tmp_tgt_table_names_length = share->tgt_table_names_length;
  share_alter->tmp_tgt_dbs_length = share->tgt_dbs_length;
  share_alter->tmp_tgt_hosts_length = share->tgt_hosts_length;
  share_alter->tmp_tgt_usernames_length = share->tgt_usernames_length;
  share_alter->tmp_tgt_passwords_length = share->tgt_passwords_length;
  share_alter->tmp_tgt_sockets_length = share->tgt_sockets_length;
  share_alter->tmp_tgt_wrappers_length = share->tgt_wrappers_length;
  share_alter->tmp_tgt_ssl_cas_length = share->tgt_ssl_cas_length;
  share_alter->tmp_tgt_ssl_capaths_length = share->tgt_ssl_capaths_length;
  share_alter->tmp_tgt_ssl_certs_length = share->tgt_ssl_certs_length;
  share_alter->tmp_tgt_ssl_ciphers_length = share->tgt_ssl_ciphers_length;
  share_alter->tmp_tgt_ssl_keys_length = share->tgt_ssl_keys_length;
  share_alter->tmp_tgt_default_files_length = share->tgt_default_files_length;
  share_alter->tmp_tgt_default_groups_length =
    share->tgt_default_groups_length;
  share_alter->tmp_tgt_dsns_length =
    share->tgt_dsns_length;
  share_alter->tmp_tgt_filedsns_length =
    share->tgt_filedsns_length;
  share_alter->tmp_tgt_drivers_length =
    share->tgt_drivers_length;
  share_alter->tmp_static_link_ids_length =
    share->static_link_ids_length;
  share_alter->tmp_tgt_ports_length = share->tgt_ports_length;
  share_alter->tmp_tgt_ssl_vscs_length = share->tgt_ssl_vscs_length;
  share_alter->tmp_monitoring_binlog_pos_at_failing_length =
    share->monitoring_binlog_pos_at_failing_length;
  share_alter->tmp_link_statuses_length = share->link_statuses_length;
  /* copy for tables end */

  if ((error_num = spider_set_connect_info_default(
    share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
    part_elem,
    sub_elem,
#endif
    table_share
  )))
    goto error;

  if (create_table)
  {
    for (roop_count = 0; roop_count < (int) share->all_link_count;
      roop_count++)
    {
      int roop_count2;
      for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; roop_count2++)
      {
        if (
          spider_dbton[roop_count2].wrapper &&
          !strcmp(share->tgt_wrappers[roop_count],
            spider_dbton[roop_count2].wrapper)
        ) {
          break;
        }
      }
      if (roop_count2 == SPIDER_DBTON_SIZE)
      {
        DBUG_PRINT("info",("spider err tgt_wrappers[%d]=%s", roop_count,
          share->tgt_wrappers[roop_count]));
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_STR,
          MYF(0), share->tgt_wrappers[roop_count]);
        goto error;
      }

      DBUG_PRINT("info",
        ("spider server_names_lengths[%d] = %u", roop_count,
         share->server_names_lengths[roop_count]));
      if (share->server_names_lengths[roop_count] > SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->server_names[roop_count], "server");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_table_names_lengths[%d] = %u", roop_count,
        share->tgt_table_names_lengths[roop_count]));
      if (share->tgt_table_names_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_table_names[roop_count], "table");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_dbs_lengths[%d] = %u", roop_count,
        share->tgt_dbs_lengths[roop_count]));
      if (share->tgt_dbs_lengths[roop_count] > SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_dbs[roop_count], "database");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_hosts_lengths[%d] = %u", roop_count,
        share->tgt_hosts_lengths[roop_count]));
      if (share->tgt_hosts_lengths[roop_count] > SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_hosts[roop_count], "host");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_usernames_lengths[%d] = %u", roop_count,
        share->tgt_usernames_lengths[roop_count]));
      if (share->tgt_usernames_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_usernames[roop_count], "user");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_passwords_lengths[%d] = %u", roop_count,
        share->tgt_passwords_lengths[roop_count]));
      if (share->tgt_passwords_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_passwords[roop_count], "password");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_sockets_lengths[%d] = %u", roop_count,
        share->tgt_sockets_lengths[roop_count]));
      if (share->tgt_sockets_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_sockets[roop_count], "socket");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_wrappers_lengths[%d] = %u", roop_count,
        share->tgt_wrappers_lengths[roop_count]));
      if (share->tgt_wrappers_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_wrappers[roop_count], "wrapper");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_ssl_cas_lengths[%d] = %u", roop_count,
        share->tgt_ssl_cas_lengths[roop_count]));
      if (share->tgt_ssl_cas_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_ssl_cas[roop_count], "ssl_ca");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_ssl_capaths_lengths[%d] = %u", roop_count,
        share->tgt_ssl_capaths_lengths[roop_count]));
      if (share->tgt_ssl_capaths_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_ssl_capaths[roop_count], "ssl_capath");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_ssl_certs_lengths[%d] = %u", roop_count,
        share->tgt_ssl_certs_lengths[roop_count]));
      if (share->tgt_ssl_certs_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_ssl_certs[roop_count], "ssl_cert");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_ssl_ciphers_lengths[%d] = %u", roop_count,
        share->tgt_ssl_ciphers_lengths[roop_count]));
      if (share->tgt_ssl_ciphers_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_ssl_ciphers[roop_count], "ssl_cipher");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_ssl_keys_lengths[%d] = %u", roop_count,
        share->tgt_ssl_keys_lengths[roop_count]));
      if (share->tgt_ssl_keys_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_ssl_keys[roop_count], "ssl_key");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_default_files_lengths[%d] = %u", roop_count,
        share->tgt_default_files_lengths[roop_count]));
      if (share->tgt_default_files_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_default_files[roop_count], "default_file");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_default_groups_lengths[%d] = %u", roop_count,
        share->tgt_default_groups_lengths[roop_count]));
      if (share->tgt_default_groups_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_default_groups[roop_count], "default_group");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_dsns_lengths[%d] = %u", roop_count,
        share->tgt_dsns_lengths[roop_count]));
      if (share->tgt_dsns_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_dsns[roop_count], "dsn");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_filedsns_lengths[%d] = %u", roop_count,
        share->tgt_filedsns_lengths[roop_count]));
      if (share->tgt_filedsns_lengths[roop_count] >
        SPIDER_CONNECT_INFO_PATH_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_filedsns[roop_count], "filedsn");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_drivers_lengths[%d] = %u", roop_count,
        share->tgt_drivers_lengths[roop_count]));
      if (share->tgt_drivers_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_drivers[roop_count], "driver");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_pk_names_lengths[%d] = %u", roop_count,
        share->tgt_pk_names_lengths[roop_count]));
      if (share->tgt_pk_names_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_pk_names[roop_count], "pk_name");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider tgt_sequence_names_lengths[%d] = %u", roop_count,
        share->tgt_sequence_names_lengths[roop_count]));
      if (share->tgt_sequence_names_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->tgt_sequence_names[roop_count], "sequence_name");
        goto error;
      }

      DBUG_PRINT("info",
        ("spider static_link_ids_lengths[%d] = %u", roop_count,
        share->static_link_ids_lengths[roop_count]));
      if (share->static_link_ids_lengths[roop_count] >
        SPIDER_CONNECT_INFO_MAX_LEN)
      {
        error_num = ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_NUM;
        my_printf_error(error_num, ER_SPIDER_INVALID_CONNECT_INFO_TOO_LONG_STR,
          MYF(0), share->static_link_ids[roop_count], "static_link_id");
        goto error;
      }
      if (share->static_link_ids[roop_count])
      {
        if (
          share->static_link_ids_lengths[roop_count] > 0 &&
          share->static_link_ids[roop_count][0] >= '0' &&
          share->static_link_ids[roop_count][0] <= '9'
        ) {
          error_num = ER_SPIDER_INVALID_CONNECT_INFO_START_WITH_NUM_NUM;
          my_printf_error(error_num,
            ER_SPIDER_INVALID_CONNECT_INFO_START_WITH_NUM_STR,
            MYF(0), share->static_link_ids[roop_count], "static_link_id");
          goto error;
        }
        for (roop_count2 = roop_count + 1;
          roop_count2 < (int) share->all_link_count;
          roop_count2++)
        {
          if (
            share->static_link_ids_lengths[roop_count] ==
              share->static_link_ids_lengths[roop_count2] &&
            !memcmp(share->static_link_ids[roop_count],
              share->static_link_ids[roop_count2],
              share->static_link_ids_lengths[roop_count])
          ) {
            error_num = ER_SPIDER_INVALID_CONNECT_INFO_SAME_NUM;
            my_printf_error(error_num,
              ER_SPIDER_INVALID_CONNECT_INFO_SAME_STR,
              MYF(0), share->static_link_ids[roop_count],
              "static_link_id");
            goto error;
          }
        }
      }
    }
  }

  DBUG_PRINT("info", ("spider share->active_link_count = %d",
    share->active_link_count));
  share->link_count = (uint) share->active_link_count;
  share_alter->link_count = share->link_count;
  share->link_bitmap_size = (share->link_count + 7) / 8;

  if (connect_string)
    spider_free(spider_current_trx, connect_string, MYF(0));
  DBUG_RETURN(0);

error:
  if (connect_string)
    spider_free(spider_current_trx, connect_string, MYF(0));
error_alloc_conn_string:
  DBUG_RETURN(error_num);
}

int spider_set_connect_info_default(
  SPIDER_SHARE *share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_element *part_elem,
  partition_element *sub_elem,
#endif
  TABLE_SHARE *table_share
) {
  bool check_socket;
  bool check_database;
  bool check_default_file;
  bool check_host;
  bool check_port;
  bool socket_has_default_value;
  bool database_has_default_value;
  bool default_file_has_default_value;
  bool host_has_default_value;
  bool port_has_default_value;
  int error_num, roop_count, roop_count2;
  DBUG_ENTER("spider_set_connect_info_default");
  for (roop_count = 0; roop_count < (int) share->all_link_count; roop_count++)
  {
    if (share->server_names[roop_count])
    {
      if ((error_num = spider_get_server(share, roop_count)))
        DBUG_RETURN(error_num);
    }

    if (
      !share->tgt_sockets[roop_count] &&
      (
        !share->tgt_hosts[roop_count] ||
        !strcmp(share->tgt_hosts[roop_count], my_localhost)
      )
    ) {
      check_socket = TRUE;
    } else {
      check_socket = FALSE;
    }
    if (!share->tgt_dbs[roop_count] && table_share)
    {
      check_database = TRUE;
    } else {
      check_database = FALSE;
    }
    if (
      !share->tgt_default_files[roop_count] &&
      share->tgt_default_groups[roop_count] &&
      (*spd_defaults_file || *spd_defaults_extra_file)
    ) {
      check_default_file = TRUE;
    } else {
      check_default_file = FALSE;
    }
    if (!share->tgt_hosts[roop_count])
    {
      check_host = TRUE;
    } else {
      check_host = FALSE;
    }
    if (share->tgt_ports[roop_count] == -1)
    {
      check_port = TRUE;
    } else {
      check_port = FALSE;
    }
    if (check_socket || check_database || check_default_file || check_host ||
      check_port)
    {
      socket_has_default_value = check_socket;
      database_has_default_value = check_database;
      default_file_has_default_value = check_default_file;
      host_has_default_value = check_host;
      port_has_default_value = check_port;
      if (share->tgt_wrappers[roop_count])
      {
        for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; roop_count2++)
        {
          DBUG_PRINT("info",("spider share->tgt_wrappers[%d]=%s", roop_count,
            share->tgt_wrappers[roop_count]));
          DBUG_PRINT("info",("spider spider_dbton[%d].wrapper=%s", roop_count2,
            spider_dbton[roop_count2].wrapper ?
              spider_dbton[roop_count2].wrapper : "NULL"));
          if (
            spider_dbton[roop_count2].wrapper &&
            !strcmp(share->tgt_wrappers[roop_count],
              spider_dbton[roop_count2].wrapper)
          ) {
            if (spider_dbton[roop_count2].db_access_type ==
              SPIDER_DB_ACCESS_TYPE_SQL)
            {
              if (check_socket)
              {
                socket_has_default_value = spider_dbton[roop_count2].
                  db_util->socket_has_default_value();
              }
              if (check_database)
              {
                database_has_default_value = spider_dbton[roop_count2].
                  db_util->database_has_default_value();
              }
              if (check_default_file)
              {
                default_file_has_default_value = spider_dbton[roop_count2].
                  db_util->default_file_has_default_value();
              }
              if (check_host)
              {
                host_has_default_value = spider_dbton[roop_count2].
                  db_util->host_has_default_value();
              }
              if (check_port)
              {
                port_has_default_value = spider_dbton[roop_count2].
                  db_util->port_has_default_value();
              }
              break;
            }
          }
        }
      }
    } else {
      socket_has_default_value = FALSE;
      database_has_default_value = FALSE;
      default_file_has_default_value = FALSE;
      host_has_default_value = FALSE;
      port_has_default_value = FALSE;
    }

    if (!share->tgt_wrappers[roop_count])
    {
      DBUG_PRINT("info",("spider create default tgt_wrappers"));
      share->tgt_wrappers_lengths[roop_count] = SPIDER_DB_WRAPPER_LEN;
      if (
        !(share->tgt_wrappers[roop_count] = spider_create_string(
          SPIDER_DB_WRAPPER_STR,
          share->tgt_wrappers_lengths[roop_count]))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (host_has_default_value)
    {
      DBUG_PRINT("info",("spider create default tgt_hosts"));
      share->tgt_hosts_lengths[roop_count] = strlen(my_localhost);
      if (
        !(share->tgt_hosts[roop_count] = spider_create_string(
          my_localhost,
          share->tgt_hosts_lengths[roop_count]))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (database_has_default_value)
    {
      DBUG_PRINT("info",("spider create default tgt_dbs"));
      share->tgt_dbs_lengths[roop_count] = table_share->db.length;
      if (
        !(share->tgt_dbs[roop_count] = spider_create_string(
          table_share->db.str,
          table_share->db.length))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (!share->tgt_table_names[roop_count] && table_share)
    {
      DBUG_PRINT("info",("spider create default tgt_table_names"));
      share->tgt_table_names_lengths[roop_count] =
        table_share->table_name.length;
      if (
        !(share->tgt_table_names[roop_count] = spider_create_table_name_string(
          table_share->table_name.str,
#ifdef WITH_PARTITION_STORAGE_ENGINE
          (part_elem ? part_elem->partition_name : NULL),
          (sub_elem ? sub_elem->partition_name : NULL)
#else
          NULL,
          NULL
#endif
        ))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (default_file_has_default_value)
    {
      DBUG_PRINT("info",("spider create default tgt_default_files"));
      if (*spd_defaults_extra_file)
      {
        share->tgt_default_files_lengths[roop_count] =
          strlen(*spd_defaults_extra_file);
        if (
          !(share->tgt_default_files[roop_count] = spider_create_string(
            *spd_defaults_extra_file,
            share->tgt_default_files_lengths[roop_count]))
        ) {
          my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
      } else {
        share->tgt_default_files_lengths[roop_count] =
          strlen(*spd_defaults_file);
        if (
          !(share->tgt_default_files[roop_count] = spider_create_string(
            *spd_defaults_file,
            share->tgt_default_files_lengths[roop_count]))
        ) {
          my_error(ER_OUT_OF_RESOURCES, MYF(0), HA_ERR_OUT_OF_MEM);
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
      }
    }

    if (!share->tgt_pk_names[roop_count])
    {
      DBUG_PRINT("info",("spider create default tgt_pk_names"));
      share->tgt_pk_names_lengths[roop_count] = SPIDER_DB_PK_NAME_LEN;
      if (
        !(share->tgt_pk_names[roop_count] = spider_create_string(
          SPIDER_DB_PK_NAME_STR,
          share->tgt_pk_names_lengths[roop_count]))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (!share->tgt_sequence_names[roop_count])
    {
      DBUG_PRINT("info",("spider create default tgt_sequence_names"));
      share->tgt_sequence_names_lengths[roop_count] =
        SPIDER_DB_SEQUENCE_NAME_LEN;
      if (
        !(share->tgt_sequence_names[roop_count] = spider_create_string(
          SPIDER_DB_SEQUENCE_NAME_STR,
          share->tgt_sequence_names_lengths[roop_count]))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

/*
    if (!share->static_link_ids[roop_count])
    {
      DBUG_PRINT("info",("spider create default static_link_ids"));
      share->static_link_ids_lengths[roop_count] =
        SPIDER_DB_STATIC_LINK_ID_LEN;
      if (
        !(share->static_link_ids[roop_count] = spider_create_string(
          SPIDER_DB_STATIC_LINK_ID_STR,
          share->static_link_ids_lengths[roop_count]))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }
*/

    if (port_has_default_value)
    {
      share->tgt_ports[roop_count] = MYSQL_PORT;
    } else if (share->tgt_ports[roop_count] < 0)
    {
      share->tgt_ports[roop_count] = 0;
    } else if (share->tgt_ports[roop_count] > 65535)
    {
      share->tgt_ports[roop_count] = 65535;
    }

    if (share->tgt_ssl_vscs[roop_count] == -1)
      share->tgt_ssl_vscs[roop_count] = 0;

    if (socket_has_default_value)
    {
      DBUG_PRINT("info",("spider create default tgt_sockets"));
      share->tgt_sockets_lengths[roop_count] =
        strlen((char *) MYSQL_UNIX_ADDR);
      if (
        !(share->tgt_sockets[roop_count] = spider_create_string(
          (char *) MYSQL_UNIX_ADDR,
          share->tgt_sockets_lengths[roop_count]))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (share->link_statuses[roop_count] == -1)
      share->link_statuses[roop_count] = SPIDER_LINK_STATUS_NO_CHANGE;

#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (share->monitoring_bg_flag[roop_count] == -1)
      share->monitoring_bg_flag[roop_count] = 0;
    if (share->monitoring_bg_kind[roop_count] == -1)
      share->monitoring_bg_kind[roop_count] = 0;
#endif
    if (share->monitoring_binlog_pos_at_failing[roop_count] == -1)
      share->monitoring_binlog_pos_at_failing[roop_count] = 0;
    if (share->monitoring_flag[roop_count] == -1)
      share->monitoring_flag[roop_count] = 0;
    if (share->monitoring_kind[roop_count] == -1)
      share->monitoring_kind[roop_count] = 0;
#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (share->monitoring_bg_interval[roop_count] == -1)
      share->monitoring_bg_interval[roop_count] = 10000000;
#endif
    if (share->monitoring_limit[roop_count] == -1)
      share->monitoring_limit[roop_count] = 1;
    if (share->monitoring_sid[roop_count] == -1)
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100002
      share->monitoring_sid[roop_count] = global_system_variables.server_id;
#else
      share->monitoring_sid[roop_count] = current_thd->server_id;
#endif

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (share->use_hs_reads[roop_count] == -1)
      share->use_hs_reads[roop_count] = 0;
    if (share->use_hs_writes[roop_count] == -1)
      share->use_hs_writes[roop_count] = 0;
    if (share->hs_read_ports[roop_count] == -1)
    {
      share->hs_read_ports[roop_count] = 9998;
    } else if (share->hs_read_ports[roop_count] < 0)
    {
      share->hs_read_ports[roop_count] = 0;
    } else if (share->hs_read_ports[roop_count] > 65535)
    {
      share->hs_read_ports[roop_count] = 65535;
    }
    if (share->hs_write_ports[roop_count] == -1)
    {
      share->hs_write_ports[roop_count] = 9999;
    } else if (share->hs_write_ports[roop_count] < 0)
    {
      share->hs_write_ports[roop_count] = 0;
    } else if (share->hs_write_ports[roop_count] > 65535)
    {
      share->hs_write_ports[roop_count] = 65535;
    }
    if (share->hs_write_to_reads[roop_count] == -1)
    {
      share->hs_write_to_reads[roop_count] = 1;
    } else if (share->hs_write_to_reads[roop_count] < 0)
    {
      share->hs_write_to_reads[roop_count] = 0;
    } else if (share->hs_write_to_reads[roop_count] > 1)
    {
      share->hs_write_to_reads[roop_count] = 1;
    }
#endif
    if (share->use_handlers[roop_count] == -1)
      share->use_handlers[roop_count] = 0;
    if (share->connect_timeouts[roop_count] == -1)
      share->connect_timeouts[roop_count] = 6;
    if (share->net_read_timeouts[roop_count] == -1)
      share->net_read_timeouts[roop_count] = 600;
    if (share->net_write_timeouts[roop_count] == -1)
      share->net_write_timeouts[roop_count] = 600;
    if (share->access_balances[roop_count] == -1)
      share->access_balances[roop_count] = 100;
    if (share->bka_table_name_types[roop_count] == -1)
      share->bka_table_name_types[roop_count] = 0;
    if (share->strict_group_bys[roop_count] == -1)
      share->strict_group_bys[roop_count] = 1;
  }

#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (share->sts_bg_mode == -1)
    share->sts_bg_mode = 2;
#endif
  if (share->sts_interval == -1)
    share->sts_interval = 10;
  if (share->sts_mode == -1)
    share->sts_mode = 1;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (share->sts_sync == -1)
    share->sts_sync = 0;
#endif
  if (share->store_last_sts == -1)
    share->store_last_sts = 1;
  if (share->load_sts_at_startup == -1)
    share->load_sts_at_startup = 1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (share->crd_bg_mode == -1)
    share->crd_bg_mode = 2;
#endif
  if (share->crd_interval == -1)
    share->crd_interval = 51;
  if (share->crd_mode == -1)
    share->crd_mode = 1;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (share->crd_sync == -1)
    share->crd_sync = 0;
#endif
  if (share->store_last_crd == -1)
    share->store_last_crd = 1;
  if (share->load_crd_at_startup == -1)
    share->load_crd_at_startup = 1;
  if (share->crd_type == -1)
    share->crd_type = 2;
  if (share->crd_weight == -1)
    share->crd_weight = 2;
  if (share->internal_offset == -1)
    share->internal_offset = 0;
  if (share->internal_limit == -1)
    share->internal_limit = 9223372036854775807LL;
  if (share->split_read == -1)
    share->split_read = 9223372036854775807LL;
  if (share->semi_split_read == -1)
    share->semi_split_read = 2;
  if (share->semi_split_read_limit == -1)
    share->semi_split_read_limit = 9223372036854775807LL;
  if (share->init_sql_alloc_size == -1)
    share->init_sql_alloc_size = 1024;
  if (share->reset_sql_alloc == -1)
    share->reset_sql_alloc = 1;
  if (share->multi_split_read == -1)
    share->multi_split_read = 100;
  if (share->max_order == -1)
    share->max_order = 32767;
  if (share->semi_table_lock == -1)
    share->semi_table_lock = 0;
  if (share->semi_table_lock_conn == -1)
    share->semi_table_lock_conn = 1;
  if (share->selupd_lock_mode == -1)
    share->selupd_lock_mode = 1;
  if (share->query_cache == -1)
    share->query_cache = 0;
  if (share->query_cache_sync == -1)
    share->query_cache_sync = 0;
  if (share->internal_delayed == -1)
    share->internal_delayed = 0;
  if (share->bulk_size == -1)
    share->bulk_size = 16000;
  if (share->bulk_update_mode == -1)
    share->bulk_update_mode = 0;
  if (share->bulk_update_size == -1)
    share->bulk_update_size = 16000;
  if (share->buffer_size == -1)
    share->buffer_size = 16000;
  if (share->internal_optimize == -1)
    share->internal_optimize = 0;
  if (share->internal_optimize_local == -1)
    share->internal_optimize_local = 0;
  if (share->scan_rate == -1)
    share->scan_rate = 1;
  if (share->read_rate == -1)
    share->read_rate = 0.0002;
  if (share->priority == -1)
    share->priority = 1000000;
  if (share->quick_mode == -1)
    share->quick_mode = 3;
  if (share->quick_page_size == -1)
    share->quick_page_size = 1024;
  if (share->quick_page_byte == -1)
    share->quick_page_byte = 10485760;
  if (share->low_mem_read == -1)
    share->low_mem_read = 1;
  if (share->table_count_mode == -1)
    share->table_count_mode = 0;
  if (share->select_column_mode == -1)
    share->select_column_mode = 1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (share->bgs_mode == -1)
    share->bgs_mode = 0;
  if (share->bgs_first_read == -1)
    share->bgs_first_read = 2;
  if (share->bgs_second_read == -1)
    share->bgs_second_read = 100;
#endif
  if (share->first_read == -1)
    share->first_read = 0;
  if (share->second_read == -1)
    share->second_read = 0;
  if (share->auto_increment_mode == -1)
    share->auto_increment_mode = 0;
  if (share->use_table_charset == -1)
    share->use_table_charset = 1;
  if (share->use_pushdown_udf == -1)
    share->use_pushdown_udf = 1;
  if (share->skip_default_condition == -1)
    share->skip_default_condition = 0;
  if (share->skip_parallel_search == -1)
    share->skip_parallel_search = 0;
  if (share->direct_dup_insert == -1)
    share->direct_dup_insert = 0;
  if (share->direct_order_limit == -1)
    share->direct_order_limit = 9223372036854775807LL;
  if (share->read_only_mode == -1)
    share->read_only_mode = 0;
  if (share->error_read_mode == -1)
    share->error_read_mode = 0;
  if (share->error_write_mode == -1)
    share->error_write_mode = 0;
  if (share->active_link_count == -1)
    share->active_link_count = share->all_link_count;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (share->hs_result_free_size == -1)
    share->hs_result_free_size = 1048576;
#endif
#ifdef HA_CAN_BULK_ACCESS
  if (share->bulk_access_free == -1)
    share->bulk_access_free = 0;
#endif
#ifdef HA_CAN_FORCE_BULK_UPDATE
  if (share->force_bulk_update == -1)
    share->force_bulk_update = 0;
#endif
#ifdef HA_CAN_FORCE_BULK_DELETE
  if (share->force_bulk_delete == -1)
    share->force_bulk_delete = 0;
#endif
  if (share->casual_read == -1)
    share->casual_read = 0;
  if (share->delete_all_rows_type == -1)
  {
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    share->delete_all_rows_type = 1;
#else
    share->delete_all_rows_type = 0;
#endif
  }
  if (share->bka_mode == -1)
    share->bka_mode = 1;
  if (!share->bka_engine)
  {
    DBUG_PRINT("info",("spider create default bka_engine"));
    share->bka_engine_length = SPIDER_SQL_TMP_BKA_ENGINE_LEN;
    if (
      !(share->bka_engine = spider_create_string(
        SPIDER_SQL_TMP_BKA_ENGINE_STR,
        SPIDER_SQL_TMP_BKA_ENGINE_LEN))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }
  DBUG_RETURN(0);
}

int spider_set_connect_info_default_db_table(
  SPIDER_SHARE *share,
  const char *db_name,
  uint db_name_length,
  const char *table_name,
  uint table_name_length
) {
  uint roop_count, roop_count2;
  bool check_database;
  bool database_has_default_value;
  DBUG_ENTER("spider_set_connect_info_default_db_table");
  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    if (!share->tgt_dbs[roop_count] && db_name)
    {
      check_database = TRUE;
    } else {
      check_database = FALSE;
    }
    if (check_database)
    {
      database_has_default_value = check_database;
      if (share->tgt_wrappers[roop_count])
      {
        for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; roop_count2++)
        {
          DBUG_PRINT("info",("spider share->tgt_wrappers[%d]=%s", roop_count,
            share->tgt_wrappers[roop_count]));
          DBUG_PRINT("info",("spider spider_dbton[%d].wrapper=%s", roop_count2,
            spider_dbton[roop_count2].wrapper ?
              spider_dbton[roop_count2].wrapper : "NULL"));
          if (
            spider_dbton[roop_count2].wrapper &&
            !strcmp(share->tgt_wrappers[roop_count],
              spider_dbton[roop_count2].wrapper)
          ) {
            if (spider_dbton[roop_count2].db_access_type ==
              SPIDER_DB_ACCESS_TYPE_SQL)
            {
              if (check_database)
              {
                database_has_default_value = spider_dbton[roop_count2].
                  db_util->database_has_default_value();
              }
              break;
            }
          }
        }
      }
    } else {
      database_has_default_value = FALSE;
    }

    if (database_has_default_value)
    {
      DBUG_PRINT("info",("spider create default tgt_dbs"));
      share->tgt_dbs_lengths[roop_count] = db_name_length;
      if (
        !(share->tgt_dbs[roop_count] = spider_create_string(
          db_name,
          db_name_length))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }

    if (!share->tgt_table_names[roop_count] && table_name)
    {
      const char *tmp_ptr;
      DBUG_PRINT("info",("spider create default tgt_table_names"));
      if ((tmp_ptr = strstr(table_name, "#P#")))
        table_name_length = (uint) PTR_BYTE_DIFF(tmp_ptr, table_name);
      share->tgt_table_names_lengths[roop_count] = table_name_length;
      if (
        !(share->tgt_table_names[roop_count] = spider_create_string(
          table_name,
          table_name_length))
      ) {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_set_connect_info_default_dbtable(
  SPIDER_SHARE *share,
  const char *dbtable_name,
  int dbtable_name_length
) {
  const char *ptr_db, *ptr_table;
  my_ptrdiff_t ptr_diff_db, ptr_diff_table;
  DBUG_ENTER("spider_set_connect_info_default_dbtable");
  ptr_db = strchr(dbtable_name, FN_LIBCHAR);
  ptr_db++;
  ptr_diff_db = PTR_BYTE_DIFF(ptr_db, dbtable_name);
  DBUG_PRINT("info",("spider ptr_diff_db = %lld", (longlong) ptr_diff_db));
  ptr_table = strchr(ptr_db, FN_LIBCHAR);
  ptr_table++;
  ptr_diff_table = PTR_BYTE_DIFF(ptr_table, ptr_db);
  DBUG_PRINT("info",("spider ptr_diff_table = %lld", (longlong) ptr_diff_table));
  DBUG_RETURN(spider_set_connect_info_default_db_table(
    share,
    ptr_db,
    (uint)(ptr_diff_table - 1),
    ptr_table,
    (uint)(dbtable_name_length - ptr_diff_db - ptr_diff_table)
  ));
}

#ifndef DBUG_OFF
void spider_print_keys(
  const char *key,
  uint length
) {
  const char *end_ptr;
  uint roop_count = 1;
  DBUG_ENTER("spider_print_keys");
  DBUG_PRINT("info",("spider key_length=%u", length));
  end_ptr = key + length;
  while (key < end_ptr)
  {
    DBUG_PRINT("info",("spider key[%u]=%s", roop_count, key));
    key = strchr(key, '\0') + 1;
    roop_count++;
  }
  DBUG_VOID_RETURN;
}
#endif

int spider_create_conn_keys(
  SPIDER_SHARE *share
) {
  int roop_count, roop_count2;
  char *tmp_name, port_str[6];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char *tmp_hs_r_name, *tmp_hs_w_name;
#endif
  uint length_base = sizeof(uint) * share->all_link_count;
  uint *conn_keys_lengths;
  uint *sql_dbton_ids;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  uint *hs_dbton_ids;
  uint *hs_r_conn_keys_lengths;
  uint *hs_w_conn_keys_lengths;
#endif
  DBUG_ENTER("spider_create_conn_keys");
  char *ptr;
  uint length = length_base * 2;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  length += length_base * 3;
#endif
  ptr = (char *) my_alloca(length);
  if (!ptr)
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  conn_keys_lengths = (uint *) ptr;
  ptr += length_base;
  sql_dbton_ids = (uint *) ptr;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  ptr += length_base;
  hs_dbton_ids = (uint *) ptr;
  ptr += length_base;
  hs_r_conn_keys_lengths = (uint *) ptr;
  ptr += length_base;
  hs_w_conn_keys_lengths = (uint *) ptr;
#endif

  share->conn_keys_charlen = 0;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  share->hs_read_conn_keys_charlen = 0;
  share->hs_write_conn_keys_charlen = 0;
#endif
  for (roop_count = 0; roop_count < (int) share->all_link_count; roop_count++)
  {
    bool get_sql_id = FALSE;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    bool get_nosql_id = FALSE;
#endif
    for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; roop_count2++)
    {
      DBUG_PRINT("info",("spider share->tgt_wrappers[%d]=%s", roop_count,
        share->tgt_wrappers[roop_count]));
      DBUG_PRINT("info",("spider spider_dbton[%d].wrapper=%s", roop_count2,
        spider_dbton[roop_count2].wrapper ?
          spider_dbton[roop_count2].wrapper : "NULL"));
      if (
        spider_dbton[roop_count2].wrapper &&
        !strcmp(share->tgt_wrappers[roop_count],
          spider_dbton[roop_count2].wrapper)
      ) {
        spider_set_bit(share->dbton_bitmap, roop_count2);
        if (
          !get_sql_id &&
          spider_dbton[roop_count2].db_access_type == SPIDER_DB_ACCESS_TYPE_SQL
        ) {
          sql_dbton_ids[roop_count] = roop_count2;
          get_sql_id = TRUE;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          if (get_nosql_id)
#endif
            break;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
          else
            continue;
#endif
        }
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        if (
          !get_nosql_id &&
          spider_dbton[roop_count2].db_access_type ==
            SPIDER_DB_ACCESS_TYPE_NOSQL
        ) {
          hs_dbton_ids[roop_count] = roop_count2;
          get_nosql_id = TRUE;
          if (get_sql_id)
            break;
        }
#endif
      }
    }
    if (!get_sql_id)
      sql_dbton_ids[roop_count] = SPIDER_DBTON_SIZE;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    if (!get_nosql_id)
      hs_dbton_ids[roop_count] = SPIDER_DBTON_SIZE;
#endif

    bool tables_on_different_db_are_joinable;
    if (get_sql_id)
    {
      tables_on_different_db_are_joinable =
        spider_dbton[sql_dbton_ids[roop_count]].db_util->
          tables_on_different_db_are_joinable();
    } else {
      tables_on_different_db_are_joinable = TRUE;
    }
    conn_keys_lengths[roop_count]
      = 1
      + share->tgt_wrappers_lengths[roop_count] + 1
      + share->tgt_hosts_lengths[roop_count] + 1
      + 5 + 1
      + share->tgt_sockets_lengths[roop_count] + 1
      + (tables_on_different_db_are_joinable ?
        0 : share->tgt_dbs_lengths[roop_count] + 1)
      + share->tgt_usernames_lengths[roop_count] + 1
      + share->tgt_passwords_lengths[roop_count] + 1
      + share->tgt_ssl_cas_lengths[roop_count] + 1
      + share->tgt_ssl_capaths_lengths[roop_count] + 1
      + share->tgt_ssl_certs_lengths[roop_count] + 1
      + share->tgt_ssl_ciphers_lengths[roop_count] + 1
      + share->tgt_ssl_keys_lengths[roop_count] + 1
      + 1 + 1
      + share->tgt_default_files_lengths[roop_count] + 1
      + share->tgt_default_groups_lengths[roop_count] + 1
      + share->tgt_dsns_lengths[roop_count] + 1
      + share->tgt_filedsns_lengths[roop_count] + 1
      + share->tgt_drivers_lengths[roop_count];
    share->conn_keys_charlen += conn_keys_lengths[roop_count] + 2;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    hs_r_conn_keys_lengths[roop_count]
      = 1
      + share->tgt_wrappers_lengths[roop_count] + 1
      + share->tgt_hosts_lengths[roop_count] + 1
      + 5 + 1
      + share->hs_read_socks_lengths[roop_count];
    share->hs_read_conn_keys_charlen +=
      hs_r_conn_keys_lengths[roop_count] + 2;
    hs_w_conn_keys_lengths[roop_count]
      = 1
      + share->tgt_wrappers_lengths[roop_count] + 1
      + share->tgt_hosts_lengths[roop_count] + 1
      + 5 + 1
      + share->hs_write_socks_lengths[roop_count];
    share->hs_write_conn_keys_charlen +=
      hs_w_conn_keys_lengths[roop_count] + 2;
#endif
  }
  if (!(share->conn_keys = (char **)
    spider_bulk_alloc_mem(spider_current_trx, 45,
      __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
      &share->conn_keys, sizeof(char *) * share->all_link_count,
      &share->conn_keys_lengths, length_base,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      &share->conn_keys_hash_value,
        sizeof(my_hash_value_type) * share->all_link_count,
#endif
      &tmp_name, sizeof(char) * share->conn_keys_charlen,
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      &share->hs_read_conn_keys, sizeof(char *) * share->all_link_count,
      &share->hs_read_conn_keys_lengths, length_base,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      &share->hs_read_conn_keys_hash_value,
        sizeof(my_hash_value_type) * share->all_link_count,
#endif
      &tmp_hs_r_name, sizeof(char) * share->hs_read_conn_keys_charlen,
      &share->hs_write_conn_keys, sizeof(char *) * share->all_link_count,
      &share->hs_write_conn_keys_lengths, length_base,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      &share->hs_write_conn_keys_hash_value,
        sizeof(my_hash_value_type) * share->all_link_count,
#endif
      &tmp_hs_w_name, sizeof(char) * share->hs_write_conn_keys_charlen,
#endif
      &share->sql_dbton_ids, length_base,
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      &share->hs_dbton_ids, length_base,
#endif
      NullS))
  ) {
    my_afree(conn_keys_lengths);
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  share->conn_keys_length = share->all_link_count;
  memcpy(share->conn_keys_lengths, conn_keys_lengths,
    length_base);
  memcpy(share->sql_dbton_ids, sql_dbton_ids, length_base);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  share->hs_read_conn_keys_length = share->all_link_count;
  share->hs_write_conn_keys_length = share->all_link_count;
  memcpy(share->hs_read_conn_keys_lengths, hs_r_conn_keys_lengths,
    length_base);
  memcpy(share->hs_write_conn_keys_lengths, hs_w_conn_keys_lengths,
    length_base);
  memcpy(share->hs_dbton_ids, hs_dbton_ids, length_base);
#endif

  my_afree(conn_keys_lengths);

  for (roop_count = 0; roop_count < (int) share->all_link_count; roop_count++)
  {
    bool tables_on_different_db_are_joinable;
    if (share->sql_dbton_ids[roop_count] != SPIDER_DBTON_SIZE)
    {
      tables_on_different_db_are_joinable =
        spider_dbton[share->sql_dbton_ids[roop_count]].db_util->
          tables_on_different_db_are_joinable();
    } else {
      tables_on_different_db_are_joinable = TRUE;
    }

    share->conn_keys[roop_count] = tmp_name;
    *tmp_name = '0';
    DBUG_PRINT("info",("spider tgt_wrappers[%d]=%s", roop_count,
      share->tgt_wrappers[roop_count]));
    tmp_name = strmov(tmp_name + 1, share->tgt_wrappers[roop_count]);
    if (share->tgt_hosts[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_hosts[%d]=%s", roop_count,
        share->tgt_hosts[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_hosts[roop_count]);
    } else {
      tmp_name++;
    }
    my_sprintf(port_str, (port_str, "%05ld", share->tgt_ports[roop_count]));
    DBUG_PRINT("info",("spider port_str=%s", port_str));
    tmp_name = strmov(tmp_name + 1, port_str);
    if (share->tgt_sockets[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_sockets[%d]=%s", roop_count,
        share->tgt_sockets[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_sockets[roop_count]);
    } else
      tmp_name++;
    if (!tables_on_different_db_are_joinable)
    {
      if (share->tgt_dbs[roop_count])
      {
        DBUG_PRINT("info",("spider tgt_dbs[%d]=%s", roop_count,
          share->tgt_dbs[roop_count]));
        tmp_name = strmov(tmp_name + 1, share->tgt_dbs[roop_count]);
      } else
        tmp_name++;
    }
    if (share->tgt_usernames[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_usernames[%d]=%s", roop_count,
        share->tgt_usernames[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_usernames[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_passwords[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_passwords[%d]=%s", roop_count,
        share->tgt_passwords[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_passwords[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_ssl_cas[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_ssl_cas[%d]=%s", roop_count,
        share->tgt_ssl_cas[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_ssl_cas[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_ssl_capaths[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_ssl_capaths[%d]=%s", roop_count,
        share->tgt_ssl_capaths[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_ssl_capaths[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_ssl_certs[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_ssl_certs[%d]=%s", roop_count,
        share->tgt_ssl_certs[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_ssl_certs[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_ssl_ciphers[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_ssl_ciphers[%d]=%s", roop_count,
        share->tgt_ssl_ciphers[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_ssl_ciphers[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_ssl_keys[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_ssl_keys[%d]=%s", roop_count,
        share->tgt_ssl_keys[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_ssl_keys[roop_count]);
    } else
      tmp_name++;
    tmp_name++;
    *tmp_name = '0' + ((char) share->tgt_ssl_vscs[roop_count]);
    if (share->tgt_default_files[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_default_files[%d]=%s", roop_count,
        share->tgt_default_files[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_default_files[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_default_groups[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_default_groups[%d]=%s", roop_count,
        share->tgt_default_groups[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_default_groups[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_dsns[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_dsns[%d]=%s", roop_count,
        share->tgt_dsns[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_dsns[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_filedsns[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_filedsns[%d]=%s", roop_count,
        share->tgt_filedsns[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_filedsns[roop_count]);
    } else
      tmp_name++;
    if (share->tgt_drivers[roop_count])
    {
      DBUG_PRINT("info",("spider tgt_drivers[%d]=%s", roop_count,
        share->tgt_drivers[roop_count]));
      tmp_name = strmov(tmp_name + 1, share->tgt_drivers[roop_count]);
    } else
      tmp_name++;
    tmp_name++;
    tmp_name++;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    share->conn_keys_hash_value[roop_count] = my_calc_hash(
      &spider_open_connections, (uchar*) share->conn_keys[roop_count],
      share->conn_keys_lengths[roop_count]);
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    share->hs_read_conn_keys[roop_count] = tmp_hs_r_name;
    *tmp_hs_r_name = '0';
    DBUG_PRINT("info",("spider tgt_wrappers[%d]=%s", roop_count,
      share->tgt_wrappers[roop_count]));
    tmp_hs_r_name = strmov(tmp_hs_r_name + 1, share->tgt_wrappers[roop_count]);
    DBUG_PRINT("info",("spider tgt_hosts[%d]=%s", roop_count,
      share->tgt_hosts[roop_count]));
    tmp_hs_r_name = strmov(tmp_hs_r_name + 1, share->tgt_hosts[roop_count]);
    my_sprintf(port_str, (port_str, "%05ld",
      share->hs_read_ports[roop_count]));
    DBUG_PRINT("info",("spider port_str=%s", port_str));
    tmp_hs_r_name = strmov(tmp_hs_r_name + 1, port_str);
    if (share->hs_read_socks[roop_count])
    {
      DBUG_PRINT("info",("spider hs_read_socks[%d]=%s", roop_count,
        share->hs_read_socks[roop_count]));
      tmp_hs_r_name = strmov(tmp_hs_r_name + 1,
        share->hs_read_socks[roop_count]);
    } else
      tmp_hs_r_name++;
    tmp_hs_r_name++;
    tmp_hs_r_name++;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    share->hs_read_conn_keys_hash_value[roop_count] = my_calc_hash(
      &spider_open_connections, (uchar*) share->hs_read_conn_keys[roop_count],
      share->hs_read_conn_keys_lengths[roop_count]);
#endif
    share->hs_write_conn_keys[roop_count] = tmp_hs_w_name;
    *tmp_hs_w_name = '0';
    DBUG_PRINT("info",("spider tgt_wrappers[%d]=%s", roop_count,
      share->tgt_wrappers[roop_count]));
    tmp_hs_w_name = strmov(tmp_hs_w_name + 1, share->tgt_wrappers[roop_count]);
    DBUG_PRINT("info",("spider tgt_hosts[%d]=%s", roop_count,
      share->tgt_hosts[roop_count]));
    tmp_hs_w_name = strmov(tmp_hs_w_name + 1, share->tgt_hosts[roop_count]);
    my_sprintf(port_str, (port_str, "%05ld",
      share->hs_write_ports[roop_count]));
    DBUG_PRINT("info",("spider port_str=%s", port_str));
    tmp_hs_w_name = strmov(tmp_hs_w_name + 1, port_str);
    if (share->hs_write_socks[roop_count])
    {
      DBUG_PRINT("info",("spider hs_write_socks[%d]=%s", roop_count,
        share->hs_write_socks[roop_count]));
      tmp_hs_w_name = strmov(tmp_hs_w_name + 1,
        share->hs_write_socks[roop_count]);
    } else
      tmp_hs_w_name++;
    tmp_hs_w_name++;
    tmp_hs_w_name++;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    share->hs_write_conn_keys_hash_value[roop_count] = my_calc_hash(
      &spider_open_connections, (uchar*) share->hs_write_conn_keys[roop_count],
      share->hs_write_conn_keys_lengths[roop_count]);
#endif
#endif
  }
  for (roop_count2 = 0; roop_count2 < SPIDER_DBTON_SIZE; roop_count2++)
  {
    if (spider_bit_is_set(share->dbton_bitmap, roop_count2))
    {
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      if (spider_dbton[roop_count2].db_access_type ==
        SPIDER_DB_ACCESS_TYPE_SQL)
      {
#endif
        share->use_sql_dbton_ids[share->use_dbton_count] = roop_count2;
        share->sql_dbton_id_to_seq[roop_count2] = share->use_dbton_count;
        share->use_sql_dbton_count++;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      } else {
        share->use_hs_dbton_ids[share->use_hs_dbton_count] = roop_count2;
        share->hs_dbton_id_to_seq[roop_count2] = share->use_hs_dbton_count;
        share->use_hs_dbton_count++;
      }
#endif
      share->use_dbton_ids[share->use_dbton_count] = roop_count2;
      share->dbton_id_to_seq[roop_count2] = share->use_dbton_count;
      share->use_dbton_count++;
    }
  }
  DBUG_RETURN(0);
}

SPIDER_SHARE *spider_create_share(
  const char *table_name,
  TABLE_SHARE *table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value,
#endif
  int *error_num
) {
  int bitmap_size, roop_count;
  uint length;
  int use_table_charset;
  SPIDER_SHARE *share;
  char *tmp_name;
  longlong *tmp_cardinality, *tmp_static_key_cardinality;
  uchar *tmp_cardinality_upd, *tmp_table_mon_mutex_bitmap;
  char buf[MAX_FIELD_WIDTH], *buf_pos;
  char link_idx_str[SPIDER_SQL_INT_LEN];
#ifdef HA_HAS_CHECKSUM_EXTENDED
  bool checksum_support = TRUE;
#endif
  DBUG_ENTER("spider_create_share");
  length = (uint) strlen(table_name);
  bitmap_size = spider_bitmap_size(table_share->fields);
  if (!(share = (SPIDER_SHARE *)
    spider_bulk_malloc(spider_current_trx, 46, MYF(MY_WME | MY_ZEROFILL),
      &share, (uint) (sizeof(*share)),
      &tmp_name, (uint) (length + 1),
      &tmp_static_key_cardinality,
        (uint) (sizeof(*tmp_static_key_cardinality) * table_share->keys),
      &tmp_cardinality,
        (uint) (sizeof(*tmp_cardinality) * table_share->fields),
      &tmp_cardinality_upd,
        (uint) (sizeof(*tmp_cardinality_upd) * bitmap_size),
      &tmp_table_mon_mutex_bitmap,
        (uint) (sizeof(*tmp_table_mon_mutex_bitmap) *
          ((spider_param_udf_table_mon_mutex_count() + 7) / 8)),
      NullS))
  ) {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_alloc_share;
  }

  SPD_INIT_ALLOC_ROOT(&share->mem_root, 4096, 0, MYF(MY_WME));
  share->use_count = 0;
  share->use_dbton_count = 0;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  share->use_hs_dbton_count = 0;
#endif
  share->table_name_length = length;
  share->table_name = tmp_name;
  strmov(share->table_name, table_name);
  share->static_key_cardinality = tmp_static_key_cardinality;
  share->cardinality = tmp_cardinality;
  share->cardinality_upd = tmp_cardinality_upd;
  share->table_mon_mutex_bitmap = tmp_table_mon_mutex_bitmap;
  share->bitmap_size = bitmap_size;
  share->table_share = table_share;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  share->table_name_hash_value = hash_value;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  share->table_path_hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) table_share->path.str, table_share->path.length);
#endif
#endif
#ifndef WITHOUT_SPIDER_BG_SEARCH
  share->table.s = table_share;
  share->table.field = table_share->field;
  share->table.key_info = table_share->key_info;
  share->table.read_set = &table_share->all_set;
#endif

  if (table_share->keys > 0 &&
    !(share->key_hint = new spider_string[table_share->keys])
  ) {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_init_hint_string;
  }
  for (roop_count = 0; roop_count < (int) table_share->keys; roop_count++)
    share->key_hint[roop_count].init_calc_mem(95);
  DBUG_PRINT("info",("spider share->key_hint=%p", share->key_hint));

  if ((*error_num = spider_parse_connect_info(share, table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
    part_info,
#endif
    0)))
    goto error_parse_connect_string;

  for (roop_count = 0; roop_count < (int) share->all_link_count;
    roop_count++)
  {
    my_sprintf(link_idx_str, (link_idx_str, "%010d", roop_count));
    buf_pos = strmov(buf, share->table_name);
    buf_pos = strmov(buf_pos, link_idx_str);
    *buf_pos = '\0';
    spider_set_bit(tmp_table_mon_mutex_bitmap,
      spider_udf_calc_hash(buf, spider_param_udf_table_mon_mutex_count())
    );
  }

  use_table_charset = spider_param_use_table_charset(
    share->use_table_charset);
  if (table_share->table_charset && use_table_charset)
    share->access_charset = table_share->table_charset;
  else
    share->access_charset = system_charset_info;

  if ((*error_num = spider_create_conn_keys(share)))
    goto error_create_conn_keys;

  if (share->table_count_mode & 1)
    share->additional_table_flags |= HA_STATS_RECORDS_IS_EXACT;
  if (share->table_count_mode & 2)
    share->additional_table_flags |= HA_HAS_RECORDS;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&share->mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_share,
    &share->mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_init_mutex;
  }

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&share->sts_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_share_sts,
    &share->sts_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_init_sts_mutex;
  }

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&share->crd_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_share_crd,
    &share->crd_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    goto error_init_crd_mutex;
  }

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(share->lgtm_tblhnd_share =
    spider_get_lgtm_tblhnd_share(tmp_name, length, hash_value, FALSE, TRUE,
    error_num)))
#else
  if (!(share->lgtm_tblhnd_share =
    spider_get_lgtm_tblhnd_share(tmp_name, length, FALSE, TRUE, error_num)))
#endif
  {
    goto error_get_lgtm_tblhnd_share;
  }

  if (!(share->wide_share =
    spider_get_wide_share(share, table_share, error_num)))
    goto error_get_wide_share;

  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    if (spider_bit_is_set(share->dbton_bitmap, roop_count))
    {
      if (!(share->dbton_share[roop_count] =
        spider_dbton[roop_count].create_db_share(share)))
      {
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error_init_dbton;
      }
      if ((*error_num = share->dbton_share[roop_count]->init()))
      {
        goto error_init_dbton;
      }
#ifdef HA_HAS_CHECKSUM_EXTENDED
      if (
        spider_dbton[roop_count].db_access_type == SPIDER_DB_ACCESS_TYPE_SQL &&
        !share->dbton_share[roop_count]->checksum_support()
      ) {
        checksum_support = FALSE;
      }
#endif
    }
  }
#ifdef HA_HAS_CHECKSUM_EXTENDED
  if (checksum_support)
  {
    share->additional_table_flags |=
      HA_HAS_OLD_CHECKSUM |
      HA_HAS_NEW_CHECKSUM;
  }
#endif
  DBUG_RETURN(share);

/*
  roop_count = SPIDER_DBTON_SIZE - 1;
*/
error_init_dbton:
  for (; roop_count >= 0; roop_count--)
  {
    if (share->dbton_share[roop_count])
    {
      delete share->dbton_share[roop_count];
      share->dbton_share[roop_count] = NULL;
    }
  }
  spider_free_wide_share(share->wide_share);
error_get_wide_share:
error_get_lgtm_tblhnd_share:
  pthread_mutex_destroy(&share->crd_mutex);
error_init_crd_mutex:
  pthread_mutex_destroy(&share->sts_mutex);
error_init_sts_mutex:
  pthread_mutex_destroy(&share->mutex);
error_init_mutex:
error_create_conn_keys:
error_parse_connect_string:
error_init_hint_string:
  spider_free_share_alloc(share);
  spider_free(spider_current_trx, share, MYF(0));
error_alloc_share:
  DBUG_RETURN(NULL);
}

SPIDER_SHARE *spider_get_share(
  const char *table_name,
  TABLE *table,
  THD *thd,
  ha_spider *spider,
  int *error_num
) {
  SPIDER_SHARE *share;
  TABLE_SHARE *table_share = table->s;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  uint length, tmp_conn_link_idx = 0, buf_sz;
  char *tmp_name, *tmp_cid;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char *tmp_hs_r_name, *tmp_hs_w_name;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
  uint32 *tmp_hs_r_ret_fields, *tmp_hs_w_ret_fields;
#endif
#endif
  int roop_count;
  double sts_interval;
  int sts_mode;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int sts_sync;
  int auto_increment_mode;
#endif
  double crd_interval;
  int crd_mode;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int crd_sync;
#endif
  char first_byte;
  int semi_table_lock_conn;
  int search_link_idx;
  uint sql_command = thd_sql_command(thd);
  SPIDER_Open_tables_backup open_tables_backup;
  MEM_ROOT mem_root;
  TABLE *table_tables = NULL;
  bool init_mem_root = FALSE;
  bool same_server_link;
  int load_sts_at_startup;
  int load_crd_at_startup;
  user_var_entry *loop_check;
  char *loop_check_buf;
  TABLE_SHARE *top_share;
  LEX_CSTRING lex_str;
  DBUG_ENTER("spider_get_share");
  top_share = spider->wide_handler->top_share;
  length = (uint) strlen(table_name);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) table_name, length);
#endif
  if (top_share)
  {
    lex_str.length = top_share->path.length + SPIDER_SQL_LOP_CHK_PRM_PRF_LEN;
    buf_sz = spider_unique_id.length > SPIDER_SQL_LOP_CHK_PRM_PRF_LEN ?
      top_share->path.length + spider_unique_id.length + 2 :
      lex_str.length + 2;
    loop_check_buf = (char *) my_alloca(buf_sz);
    if (unlikely(!loop_check_buf))
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      DBUG_RETURN(NULL);
    }
    lex_str.str = loop_check_buf + buf_sz - lex_str.length - 2;
    memcpy((void *) lex_str.str,
      SPIDER_SQL_LOP_CHK_PRM_PRF_STR, SPIDER_SQL_LOP_CHK_PRM_PRF_LEN);
    memcpy((void *) (lex_str.str + SPIDER_SQL_LOP_CHK_PRM_PRF_LEN),
      top_share->path.str, top_share->path.length);
    ((char *) lex_str.str)[lex_str.length] = '\0';
    DBUG_PRINT("info",("spider loop check param name=%s", lex_str.str));
    loop_check = get_variable(&thd->user_vars, &lex_str, FALSE);
    if (loop_check && loop_check->type == STRING_RESULT)
    {
      lex_str.length = top_share->path.length + spider_unique_id.length + 1;
      lex_str.str = loop_check_buf + buf_sz - top_share->path.length -
        spider_unique_id.length - 2;
      memcpy((void *) lex_str.str, spider_unique_id.str,
        spider_unique_id.length);
      ((char *) lex_str.str)[lex_str.length - 1] = '-';
      ((char *) lex_str.str)[lex_str.length] = '\0';
      DBUG_PRINT("info",("spider loop check key=%s", lex_str.str));
      DBUG_PRINT("info",("spider loop check param value=%s",
        loop_check->value));
      if (unlikely(strstr(loop_check->value, lex_str.str)))
      {
        *error_num = ER_SPIDER_INFINITE_LOOP_NUM;
        my_printf_error(*error_num, ER_SPIDER_INFINITE_LOOP_STR, MYF(0),
          top_share->db.str, top_share->table_name.str);
        my_afree(loop_check_buf);
        DBUG_RETURN(NULL);
      }
    }
    my_afree(loop_check_buf);
  }
  pthread_mutex_lock(&spider_tbl_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(share = (SPIDER_SHARE*) my_hash_search_using_hash_value(
    &spider_open_tables, hash_value, (uchar*) table_name, length)))
#else
  if (!(share = (SPIDER_SHARE*) my_hash_search(&spider_open_tables,
    (uchar*) table_name, length)))
#endif
  {
    if (!(share = spider_create_share(
      table_name, table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
      table->part_info,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      hash_value,
#endif
      error_num
    ))) {
      goto error_alloc_share;
    }

    uint old_elements = spider_open_tables.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    if (my_hash_insert_with_hash_value(&spider_open_tables, hash_value,
      (uchar*) share))
#else
    if (my_hash_insert(&spider_open_tables, (uchar*) share))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_hash_insert;
    }
    if (spider_open_tables.array.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        spider_open_tables,
        (spider_open_tables.array.max_element - old_elements) *
        spider_open_tables.array.size_of_element);
    }

    spider->share = share;
    spider->conn_link_idx = &tmp_conn_link_idx;

    share->use_count++;
    pthread_mutex_unlock(&spider_tbl_mutex);

    if (!share->link_status_init)
    {
      pthread_mutex_lock(&share->mutex);
      for (roop_count = 0;
        roop_count < (int) spider_param_udf_table_mon_mutex_count();
        roop_count++
      ) {
        if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
          pthread_mutex_lock(&spider_udf_table_mon_mutexes[roop_count]);
      }
      if (!share->link_status_init)
      {
        /*
          The link statuses need to be refreshed from the spider_tables table
          if the operation:
          - Is not a DROP TABLE on a permanent table; or
          - Is an ALTER TABLE.

          Note that SHOW CREATE TABLE is not excluded, because the commands
          that follow it require up-to-date link statuses.
        */
        if ((table_share->tmp_table == NO_TMP_TABLE &&
             sql_command != SQLCOM_DROP_TABLE) ||
            /* for alter change link status */
            sql_command == SQLCOM_ALTER_TABLE)
        {
          SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
          init_mem_root = TRUE;

          if (
            !(table_tables = spider_open_sys_table(
              thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
              SPIDER_SYS_TABLES_TABLE_NAME_LEN, FALSE, &open_tables_backup,
              FALSE, error_num))
          ) {
            for (roop_count = 0;
              roop_count < (int) spider_param_udf_table_mon_mutex_count();
              roop_count++
            ) {
              if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
                pthread_mutex_unlock(&spider_udf_table_mon_mutexes[roop_count]);
            }
            pthread_mutex_unlock(&share->mutex);
            share->init_error = TRUE;
            share->init_error_time = (time_t) time((time_t*) 0);
            share->init = TRUE;
            spider_free_share(share);
            goto error_open_sys_table;
          }
          *error_num = spider_get_link_statuses(table_tables, share,
            &mem_root);
          if (*error_num)
          {
            if (
              *error_num != HA_ERR_KEY_NOT_FOUND &&
              *error_num != HA_ERR_END_OF_FILE
            ) {
              for (roop_count = 0;
                roop_count < (int) spider_param_udf_table_mon_mutex_count();
                roop_count++
              ) {
                if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
                  pthread_mutex_unlock(&spider_udf_table_mon_mutexes[roop_count]);
              }
              pthread_mutex_unlock(&share->mutex);
              share->init_error = TRUE;
              share->init_error_time = (time_t) time((time_t*) 0);
              share->init = TRUE;
              spider_free_share(share);
              spider_close_sys_table(thd, table_tables,
                &open_tables_backup, FALSE);
              table_tables = NULL;
              goto error_open_sys_table;
            }
          } else {
            memcpy(share->alter_table.tmp_link_statuses, share->link_statuses,
              sizeof(long) * share->all_link_count);
            share->link_status_init = TRUE;
          }
          spider_close_sys_table(thd, table_tables,
            &open_tables_backup, FALSE);
          table_tables = NULL;
        }
        share->have_recovery_link = spider_conn_check_recovery_link(share);
        if (init_mem_root)
        {
          free_root(&mem_root, MYF(0));
          init_mem_root = FALSE;
        }
      }
      for (roop_count = 0;
        roop_count < (int) spider_param_udf_table_mon_mutex_count();
        roop_count++
      ) {
        if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
          pthread_mutex_unlock(&spider_udf_table_mon_mutexes[roop_count]);
      }
      pthread_mutex_unlock(&share->mutex);
    }

    semi_table_lock_conn = spider_param_semi_table_lock_connection(thd,
      share->semi_table_lock_conn);
    if (semi_table_lock_conn)
      first_byte = '0' +
        spider_param_semi_table_lock(thd, share->semi_table_lock);
    else
      first_byte = '0';

    if (!(spider->wide_handler->trx = spider_get_trx(thd, TRUE, error_num)))
    {
      share->init_error = TRUE;
      share->init_error_time = (time_t) time((time_t*) 0);
      share->init = TRUE;
      spider_free_share(share);
      goto error_but_no_delete;
    }
    spider->set_error_mode();

#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (!share->sts_spider_init)
    {
      pthread_mutex_lock(&share->mutex);
      if (!share->sts_spider_init)
      {
        if ((*error_num = spider_create_spider_object_for_share(
          spider->wide_handler->trx, share, &share->sts_spider)))
        {
          pthread_mutex_unlock(&share->mutex);
          share->init_error = TRUE;
          share->init_error_time = (time_t) time((time_t*) 0);
          share->init = TRUE;
          spider_free_share(share);
          goto error_sts_spider_init;
        }
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        share->sts_thread = &spider_table_sts_threads[
          hash_value % spider_param_table_sts_thread_count()];
#else
        share->sts_thread = &spider_table_sts_threads[
          my_calc_hash(&spider_open_tables, (uchar*) table_name, length) %
          spider_param_table_sts_thread_count()];
#endif
        share->sts_spider_init = TRUE;
      }
      pthread_mutex_unlock(&share->mutex);
    }

    if (!share->crd_spider_init)
    {
      pthread_mutex_lock(&share->mutex);
      if (!share->crd_spider_init)
      {
        if ((*error_num = spider_create_spider_object_for_share(
          spider->wide_handler->trx, share, &share->crd_spider)))
        {
          pthread_mutex_unlock(&share->mutex);
          share->init_error = TRUE;
          share->init_error_time = (time_t) time((time_t*) 0);
          share->init = TRUE;
          spider_free_share(share);
          goto error_crd_spider_init;
        }
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        share->crd_thread = &spider_table_crd_threads[
          hash_value % spider_param_table_crd_thread_count()];
#else
        share->crd_thread = &spider_table_crd_threads[
          my_calc_hash(&spider_open_tables, (uchar*) table_name, length) %
          spider_param_table_crd_thread_count()];
#endif
        share->crd_spider_init = TRUE;
      }
      pthread_mutex_unlock(&share->mutex);
    }
#endif

#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (
      sql_command != SQLCOM_DROP_TABLE &&
      sql_command != SQLCOM_ALTER_TABLE &&
      sql_command != SQLCOM_SHOW_CREATE &&
      (*error_num = spider_create_mon_threads(spider->wide_handler->trx,
        share))
    ) {
      share->init_error = TRUE;
      share->init_error_time = (time_t) time((time_t*) 0);
      share->init = TRUE;
      spider_free_share(share);
      goto error_but_no_delete;
    }
#endif

    if (!(spider->conn_keys = (char **)
      spider_bulk_alloc_mem(spider_current_trx, 47,
        __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
        &spider->conn_keys, sizeof(char *) * share->link_count,
        &tmp_name, sizeof(char) * share->conn_keys_charlen,
        &spider->conns, sizeof(SPIDER_CONN *) * share->link_count,
        &spider->conn_link_idx, sizeof(uint) * share->link_count,
        &spider->conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        &spider->hs_r_conn_keys, sizeof(char *) * share->link_count,
        &tmp_hs_r_name, sizeof(char) * share->hs_read_conn_keys_charlen,
        &spider->hs_r_conns, sizeof(SPIDER_CONN *) * share->link_count,
        &spider->hs_r_conn_ages, sizeof(ulonglong) * share->link_count,
        &spider->hs_w_conn_keys, sizeof(char *) * share->link_count,
        &tmp_hs_w_name, sizeof(char) * share->hs_write_conn_keys_charlen,
        &spider->hs_w_conns, sizeof(SPIDER_CONN *) * share->link_count,
        &spider->hs_w_conn_ages, sizeof(ulonglong) * share->link_count,
#endif
        &spider->sql_kind, sizeof(uint) * share->link_count,
        &spider->connection_ids, sizeof(ulonglong) * share->link_count,
        &spider->conn_kind, sizeof(uint) * share->link_count,
        &spider->db_request_id, sizeof(ulonglong) * share->link_count,
        &spider->db_request_phase, sizeof(uchar) * share->link_bitmap_size,
        &spider->m_handler_opened, sizeof(uchar) * share->link_bitmap_size,
        &spider->m_handler_id, sizeof(uint) * share->link_count,
        &spider->m_handler_cid, sizeof(char *) * share->link_count,
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        &spider->r_handler_opened, sizeof(uchar) * share->link_bitmap_size,
        &spider->r_handler_id, sizeof(uint) * share->link_count,
        &spider->r_handler_index, sizeof(uint) * share->link_count,
        &spider->w_handler_opened, sizeof(uchar) * share->link_bitmap_size,
        &spider->w_handler_id, sizeof(uint) * share->link_count,
        &spider->w_handler_index, sizeof(uint) * share->link_count,
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
        &spider->do_hs_direct_update, sizeof(uchar) * share->link_bitmap_size,
        &spider->hs_r_ret_fields, sizeof(uint32 *) * share->link_count,
        &spider->hs_w_ret_fields, sizeof(uint32 *) * share->link_count,
        &spider->hs_r_ret_fields_num, sizeof(size_t) * share->link_count,
        &spider->hs_w_ret_fields_num, sizeof(size_t) * share->link_count,
        &tmp_hs_r_ret_fields,
          sizeof(uint32) * share->link_count * table_share->fields,
        &tmp_hs_w_ret_fields,
          sizeof(uint32) * share->link_count * table_share->fields,
        &spider->tmp_column_bitmap, sizeof(uchar) * share->bitmap_size,
#endif
#endif
        &tmp_cid, sizeof(char) * (SPIDER_SQL_HANDLER_CID_LEN + 1) *
          share->link_count,
        &spider->need_mons, sizeof(int) * share->link_count,
        &spider->quick_targets, sizeof(void *) * share->link_count,
        &result_list->upd_tmp_tbls, sizeof(TABLE *) * share->link_count,
        &result_list->upd_tmp_tbl_prms,
          sizeof(TMP_TABLE_PARAM) * share->link_count,
        &result_list->tmp_table_join_first,
          sizeof(uchar) * share->link_bitmap_size,
        &result_list->tmp_table_created,
          sizeof(uchar) * share->link_bitmap_size,
#ifdef HA_CAN_BULK_ACCESS
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        &result_list->hs_r_bulk_open_index,
          sizeof(uchar) * share->link_bitmap_size,
        &result_list->hs_w_bulk_open_index,
          sizeof(uchar) * share->link_bitmap_size,
#endif
#endif
        &result_list->sql_kind_backup, sizeof(uint) * share->link_count,
        &result_list->casual_read, sizeof(int) * share->link_count,
        &spider->dbton_handler,
          sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
        NullS))
    ) {
      share->init_error = TRUE;
      share->init_error_time = (time_t) time((time_t*) 0);
      share->init = TRUE;
      spider_free_share(share);
      goto error_but_no_delete;
    }
    memcpy(tmp_name, share->conn_keys[0], share->conn_keys_charlen);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    memcpy(tmp_hs_r_name, share->hs_read_conn_keys[0],
      share->hs_read_conn_keys_charlen);
    memcpy(tmp_hs_w_name, share->hs_write_conn_keys[0],
      share->hs_write_conn_keys_charlen);
#endif

    spider->conn_keys_first_ptr = tmp_name;
    for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
    {
      spider->conn_keys[roop_count] = tmp_name;
      *tmp_name = first_byte;
      tmp_name += share->conn_keys_lengths[roop_count] + 1;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      spider->hs_r_conn_keys[roop_count] = tmp_hs_r_name;
      tmp_hs_r_name += share->hs_read_conn_keys_lengths[roop_count] + 1;
      spider->hs_w_conn_keys[roop_count] = tmp_hs_w_name;
      tmp_hs_w_name += share->hs_write_conn_keys_lengths[roop_count] + 1;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
      spider->hs_r_ret_fields[roop_count] = tmp_hs_r_ret_fields;
      tmp_hs_r_ret_fields += table_share->fields;
      spider->hs_w_ret_fields[roop_count] = tmp_hs_w_ret_fields;
      tmp_hs_w_ret_fields += table_share->fields;
#endif
#endif
      spider->m_handler_cid[roop_count] = tmp_cid;
      tmp_cid += SPIDER_SQL_HANDLER_CID_LEN + 1;
      result_list->upd_tmp_tbl_prms[roop_count].init();
      result_list->upd_tmp_tbl_prms[roop_count].field_count = 1;
      spider->conn_kind[roop_count] = SPIDER_CONN_KIND_MYSQL;
    }
    spider_trx_set_link_idx_for_all(spider);

    for (roop_count = 0; roop_count < (int) share->use_dbton_count;
      roop_count++)
    {
      uint dbton_id = share->use_dbton_ids[roop_count];
      if (!(spider->dbton_handler[dbton_id] =
        spider_dbton[dbton_id].create_db_handler(spider,
        share->dbton_share[dbton_id])))
      {
        *error_num = HA_ERR_OUT_OF_MEM;
        break;
      }
      if ((*error_num = spider->dbton_handler[dbton_id]->init()))
      {
        break;
      }
    }
    if (roop_count < (int) share->use_dbton_count)
    {
      for (; roop_count >= 0; roop_count--)
      {
        uint dbton_id = share->use_dbton_ids[roop_count];
        if (spider->dbton_handler[dbton_id])
        {
          delete spider->dbton_handler[dbton_id];
          spider->dbton_handler[dbton_id] = NULL;
        }
      }
      share->init_error = TRUE;
      share->init_error_time = (time_t) time((time_t*) 0);
      share->init = TRUE;
      goto error_after_alloc_conn_keys;
    }

    if (
      sql_command != SQLCOM_DROP_TABLE &&
      sql_command != SQLCOM_ALTER_TABLE &&
      sql_command != SQLCOM_SHOW_CREATE
    ) {
      for (
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_count < (int) share->link_count;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (
          !(spider->conns[roop_count] =
            spider_get_conn(share, roop_count, spider->conn_keys[roop_count],
              spider->wide_handler->trx, spider, FALSE, TRUE,
              SPIDER_CONN_KIND_MYSQL,
              error_num))
        ) {
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            *error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                FALSE
              );
          }
          share->init_error = TRUE;
          share->init_error_time = (time_t) time((time_t*) 0);
          share->init = TRUE;
          goto error_after_alloc_dbton_handler;
        }
        spider->conns[roop_count]->error_mode &= spider->error_mode;
      }
    }
    search_link_idx = spider_conn_first_link_idx(thd,
      share->link_statuses, share->access_balances, spider->conn_link_idx,
      share->link_count, SPIDER_LINK_STATUS_OK);
    if (search_link_idx == -1)
    {
      char *db = (char *) my_alloca(
        table_share->db.length + 1 + table_share->table_name.length + 1);
      if (!db)
      {
        *error_num = HA_ERR_OUT_OF_MEM;
        share->init_error = TRUE;
        share->init_error_time = (time_t) time((time_t*) 0);
        share->init = TRUE;
        goto error_after_alloc_dbton_handler;
      }
      char *table_name = db + table_share->db.length + 1;
      memcpy(db, table_share->db.str, table_share->db.length);
      db[table_share->db.length] = '\0';
      memcpy(table_name, table_share->table_name.str,
        table_share->table_name.length);
      table_name[table_share->table_name.length] = '\0';
      my_printf_error(ER_SPIDER_ALL_LINKS_FAILED_NUM,
        ER_SPIDER_ALL_LINKS_FAILED_STR, MYF(0), db, table_name);
      my_afree(db);
      *error_num = ER_SPIDER_ALL_LINKS_FAILED_NUM;
      share->init_error = TRUE;
      share->init_error_time = (time_t) time((time_t*) 0);
      share->init = TRUE;
      goto error_after_alloc_dbton_handler;
    } else if (search_link_idx == -2)
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      share->init_error = TRUE;
      share->init_error_time = (time_t) time((time_t*) 0);
      share->init = TRUE;
      goto error_after_alloc_dbton_handler;
    }
    spider->search_link_idx = search_link_idx;

    same_server_link = spider_param_same_server_link(thd);
    load_sts_at_startup =
      spider_param_load_sts_at_startup(share->load_sts_at_startup);
    load_crd_at_startup =
      spider_param_load_crd_at_startup(share->load_crd_at_startup);
    if (
      sql_command != SQLCOM_DROP_TABLE &&
      sql_command != SQLCOM_ALTER_TABLE &&
      sql_command != SQLCOM_SHOW_CREATE &&
      !spider->error_mode &&
      (
        !same_server_link ||
        load_sts_at_startup ||
        load_crd_at_startup
      )
    ) {
      SPIDER_INIT_ERROR_TABLE *spider_init_error_table;
      sts_interval = spider_param_sts_interval(thd, share->sts_interval);
      sts_mode = spider_param_sts_mode(thd, share->sts_mode);
#ifdef WITH_PARTITION_STORAGE_ENGINE
      sts_sync = spider_param_sts_sync(thd, share->sts_sync);
      auto_increment_mode = spider_param_auto_increment_mode(thd,
        share->auto_increment_mode);
      if (auto_increment_mode == 1)
        sts_sync = 0;
#endif
      crd_interval = spider_param_crd_interval(thd, share->crd_interval);
      crd_mode = spider_param_crd_mode(thd, share->crd_mode);
      if (crd_mode == 3)
        crd_mode = 1;
#ifdef WITH_PARTITION_STORAGE_ENGINE
      crd_sync = spider_param_crd_sync(thd, share->crd_sync);
#endif
      time_t tmp_time = (time_t) time((time_t*) 0);
      pthread_mutex_lock(&share->sts_mutex);
      pthread_mutex_lock(&share->crd_mutex);
      if ((spider_init_error_table =
        spider_get_init_error_table(spider->wide_handler->trx, share, FALSE)))
      {
        DBUG_PRINT("info",("spider diff1=%f",
          difftime(tmp_time, spider_init_error_table->init_error_time)));
        if (difftime(tmp_time,
          spider_init_error_table->init_error_time) <
          spider_param_table_init_error_interval())
        {
          *error_num = spider_init_error_table->init_error;
          if (spider_init_error_table->init_error_with_message)
            my_message(spider_init_error_table->init_error,
              spider_init_error_table->init_error_msg, MYF(0));
          share->init_error = TRUE;
          share->init = TRUE;
          pthread_mutex_unlock(&share->crd_mutex);
          pthread_mutex_unlock(&share->sts_mutex);
          goto error_after_alloc_dbton_handler;
        }
      }

      if (
        (
          !same_server_link ||
          load_sts_at_startup
        ) &&
        (*error_num = spider_get_sts(share, spider->search_link_idx, tmp_time,
          spider, sts_interval, sts_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
          sts_sync,
#endif
          1, HA_STATUS_VARIABLE | HA_STATUS_CONST | HA_STATUS_AUTO))
      ) {
        if (*error_num != ER_SPIDER_SYS_TABLE_VERSION_NUM)
        {
          thd->clear_error();
        } else {
          pthread_mutex_unlock(&share->crd_mutex);
          pthread_mutex_unlock(&share->sts_mutex);
          share->init_error = TRUE;
          share->init_error_time = (time_t) time((time_t*) 0);
          share->init = TRUE;
          goto error_after_alloc_dbton_handler;
        }
      }
      if (
        (
          !same_server_link ||
          load_crd_at_startup
        ) &&
        (*error_num = spider_get_crd(share, spider->search_link_idx, tmp_time,
          spider, table, crd_interval, crd_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
          crd_sync,
#endif
          1))
      ) {
        if (*error_num != ER_SPIDER_SYS_TABLE_VERSION_NUM)
        {
          thd->clear_error();
        } else {
          pthread_mutex_unlock(&share->crd_mutex);
          pthread_mutex_unlock(&share->sts_mutex);
          share->init_error = TRUE;
          share->init_error_time = (time_t) time((time_t*) 0);
          share->init = TRUE;
          goto error_after_alloc_dbton_handler;
        }
      }
      pthread_mutex_unlock(&share->crd_mutex);
      pthread_mutex_unlock(&share->sts_mutex);
    }

    share->init = TRUE;
  } else {
    share->use_count++;
    pthread_mutex_unlock(&spider_tbl_mutex);

    int sleep_cnt = 0;
    while (!share->init)
    {
      // avoid for dead loop
      if (sleep_cnt++ > 1000)
      {
        fprintf(stderr, " [WARN SPIDER RESULT] "
          "Wait share->init too long, table_name %s %s %ld\n",
          share->table_name, share->tgt_hosts[0], share->tgt_ports[0]);
        *error_num = ER_SPIDER_TABLE_OPEN_TIMEOUT_NUM;
        my_printf_error(ER_SPIDER_TABLE_OPEN_TIMEOUT_NUM,
          ER_SPIDER_TABLE_OPEN_TIMEOUT_STR, MYF(0),
          table_share->db.str, table_share->table_name.str);
        spider_free_share(share);
        goto error_but_no_delete;
      }
      my_sleep(10000); // wait 10 ms
    }

    if (!share->link_status_init)
    {
      pthread_mutex_lock(&share->mutex);
      for (roop_count = 0;
        roop_count < (int) spider_param_udf_table_mon_mutex_count();
        roop_count++
      ) {
        if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
          pthread_mutex_lock(&spider_udf_table_mon_mutexes[roop_count]);
      }
      if (!share->link_status_init)
      {
        DBUG_ASSERT(!table_tables);
        /*
          The link statuses need to be refreshed from the spider_tables table
          if the operation:
          - Is not a DROP TABLE on a permanent table; or
          - Is an ALTER TABLE.

          Note that SHOW CREATE TABLE is not excluded, because the commands
          that follow it require up-to-date link statuses.
        */
        if ((table_share->tmp_table == NO_TMP_TABLE &&
             sql_command != SQLCOM_DROP_TABLE) ||
            /* for alter change link status */
            sql_command == SQLCOM_ALTER_TABLE)
        {
          SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
          init_mem_root = TRUE;

          if (
            !(table_tables = spider_open_sys_table(
              thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
              SPIDER_SYS_TABLES_TABLE_NAME_LEN, FALSE, &open_tables_backup,
              FALSE, error_num))
          ) {
            for (roop_count = 0;
              roop_count < (int) spider_param_udf_table_mon_mutex_count();
              roop_count++
            ) {
              if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
                pthread_mutex_unlock(&spider_udf_table_mon_mutexes[roop_count]);
            }
            pthread_mutex_unlock(&share->mutex);
            spider_free_share(share);
            goto error_open_sys_table;
          }
          *error_num = spider_get_link_statuses(table_tables, share,
            &mem_root);
          if (*error_num)
          {
            if (
              *error_num != HA_ERR_KEY_NOT_FOUND &&
              *error_num != HA_ERR_END_OF_FILE
            ) {
              for (roop_count = 0;
                roop_count < (int) spider_param_udf_table_mon_mutex_count();
                roop_count++
              ) {
                if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
                  pthread_mutex_unlock(&spider_udf_table_mon_mutexes[roop_count]);
              }
              pthread_mutex_unlock(&share->mutex);
              spider_free_share(share);
              spider_close_sys_table(thd, table_tables,
                &open_tables_backup, FALSE);
              table_tables = NULL;
              goto error_open_sys_table;
            }
          } else {
            memcpy(share->alter_table.tmp_link_statuses, share->link_statuses,
              sizeof(long) * share->all_link_count);
            share->link_status_init = TRUE;
          }
          spider_close_sys_table(thd, table_tables,
            &open_tables_backup, FALSE);
          table_tables = NULL;
        }
        share->have_recovery_link = spider_conn_check_recovery_link(share);
        if (init_mem_root)
        {
          free_root(&mem_root, MYF(0));
          init_mem_root = FALSE;
        }
      }
      for (roop_count = 0;
        roop_count < (int) spider_param_udf_table_mon_mutex_count();
        roop_count++
      ) {
        if (spider_bit_is_set(share->table_mon_mutex_bitmap, roop_count))
          pthread_mutex_unlock(&spider_udf_table_mon_mutexes[roop_count]);
      }
      pthread_mutex_unlock(&share->mutex);
    }

    semi_table_lock_conn = spider_param_semi_table_lock_connection(thd,
      share->semi_table_lock_conn);
    if (semi_table_lock_conn)
      first_byte = '0' +
        spider_param_semi_table_lock(thd, share->semi_table_lock);
    else
      first_byte = '0';

    spider->share = share;
    if (!(spider->wide_handler->trx = spider_get_trx(thd, TRUE, error_num)))
    {
      spider_free_share(share);
      goto error_but_no_delete;
    }
    spider->set_error_mode();

#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (!share->sts_spider_init)
    {
      pthread_mutex_lock(&share->mutex);
      if (!share->sts_spider_init)
      {
        if ((*error_num = spider_create_spider_object_for_share(
          spider->wide_handler->trx, share, &share->sts_spider)))
        {
          pthread_mutex_unlock(&share->mutex);
          spider_free_share(share);
          goto error_sts_spider_init;
        }
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        share->sts_thread = &spider_table_sts_threads[
          hash_value % spider_param_table_sts_thread_count()];
#else
        share->sts_thread = &spider_table_sts_threads[
          my_calc_hash(&spider_open_tables, (uchar*) table_name, length) %
          spider_param_table_sts_thread_count()];
#endif
        share->sts_spider_init = TRUE;
      }
      pthread_mutex_unlock(&share->mutex);
    }

    if (!share->crd_spider_init)
    {
      pthread_mutex_lock(&share->mutex);
      if (!share->crd_spider_init)
      {
        if ((*error_num = spider_create_spider_object_for_share(
          spider->wide_handler->trx, share, &share->crd_spider)))
        {
          pthread_mutex_unlock(&share->mutex);
          spider_free_share(share);
          goto error_crd_spider_init;
        }
#ifdef HASH_UPDATE_WITH_HASH_VALUE
        share->crd_thread = &spider_table_crd_threads[
          hash_value % spider_param_table_crd_thread_count()];
#else
        share->crd_thread = &spider_table_crd_threads[
          my_calc_hash(&spider_open_tables, (uchar*) table_name, length) %
          spider_param_table_crd_thread_count()];
#endif
        share->crd_spider_init = TRUE;
      }
      pthread_mutex_unlock(&share->mutex);
    }
#endif

#ifndef WITHOUT_SPIDER_BG_SEARCH
    if (
      sql_command != SQLCOM_DROP_TABLE &&
      sql_command != SQLCOM_ALTER_TABLE &&
      sql_command != SQLCOM_SHOW_CREATE &&
      (*error_num = spider_create_mon_threads(spider->wide_handler->trx,
        share))
    ) {
      spider_free_share(share);
      goto error_but_no_delete;
    }
#endif

    if (!(spider->conn_keys = (char **)
      spider_bulk_alloc_mem(spider_current_trx, 49,
        __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
        &spider->conn_keys, sizeof(char *) * share->link_count,
        &tmp_name, sizeof(char) * share->conn_keys_charlen,
        &spider->conns, sizeof(SPIDER_CONN *) * share->link_count,
        &spider->conn_link_idx, sizeof(uint) * share->link_count,
        &spider->conn_can_fo, sizeof(uchar) * share->link_bitmap_size,
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        &spider->hs_r_conn_keys, sizeof(char *) * share->link_count,
        &tmp_hs_r_name, sizeof(char) * share->hs_read_conn_keys_charlen,
        &spider->hs_r_conns, sizeof(SPIDER_CONN *) * share->link_count,
        &spider->hs_r_conn_ages, sizeof(ulonglong) * share->link_count,
        &spider->hs_w_conn_keys, sizeof(char *) * share->link_count,
        &tmp_hs_w_name, sizeof(char) * share->hs_write_conn_keys_charlen,
        &spider->hs_w_conns, sizeof(SPIDER_CONN *) * share->link_count,
        &spider->hs_w_conn_ages, sizeof(ulonglong) * share->link_count,
#endif
        &spider->sql_kind, sizeof(uint) * share->link_count,
        &spider->connection_ids, sizeof(ulonglong) * share->link_count,
        &spider->conn_kind, sizeof(uint) * share->link_count,
        &spider->db_request_id, sizeof(ulonglong) * share->link_count,
        &spider->db_request_phase, sizeof(uchar) * share->link_bitmap_size,
        &spider->m_handler_opened, sizeof(uchar) * share->link_bitmap_size,
        &spider->m_handler_id, sizeof(uint) * share->link_count,
        &spider->m_handler_cid, sizeof(char *) * share->link_count,
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        &spider->r_handler_opened, sizeof(uchar) * share->link_bitmap_size,
        &spider->r_handler_id, sizeof(uint) * share->link_count,
        &spider->r_handler_index, sizeof(uint) * share->link_count,
        &spider->w_handler_opened, sizeof(uchar) * share->link_bitmap_size,
        &spider->w_handler_id, sizeof(uint) * share->link_count,
        &spider->w_handler_index, sizeof(uint) * share->link_count,
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
        &spider->do_hs_direct_update, sizeof(uchar) * share->link_bitmap_size,
        &spider->hs_r_ret_fields, sizeof(uint32 *) * share->link_count,
        &spider->hs_w_ret_fields, sizeof(uint32 *) * share->link_count,
        &spider->hs_r_ret_fields_num, sizeof(size_t) * share->link_count,
        &spider->hs_w_ret_fields_num, sizeof(size_t) * share->link_count,
        &tmp_hs_r_ret_fields,
          sizeof(uint32) * share->link_count * table_share->fields,
        &tmp_hs_w_ret_fields,
          sizeof(uint32) * share->link_count * table_share->fields,
        &spider->tmp_column_bitmap, sizeof(uchar) * share->bitmap_size,
#endif
#endif
        &tmp_cid, sizeof(char) * (SPIDER_SQL_HANDLER_CID_LEN + 1) *
          share->link_count,
        &spider->need_mons, sizeof(int) * share->link_count,
        &spider->quick_targets, sizeof(void *) * share->link_count,
        &result_list->upd_tmp_tbls, sizeof(TABLE *) * share->link_count,
        &result_list->upd_tmp_tbl_prms,
          sizeof(TMP_TABLE_PARAM) * share->link_count,
        &result_list->tmp_table_join_first,
          sizeof(uchar) * share->link_bitmap_size,
        &result_list->tmp_table_created,
          sizeof(uchar) * share->link_bitmap_size,
#ifdef HA_CAN_BULK_ACCESS
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
        &result_list->hs_r_bulk_open_index,
          sizeof(uchar) * share->link_bitmap_size,
        &result_list->hs_w_bulk_open_index,
          sizeof(uchar) * share->link_bitmap_size,
#endif
#endif
        &result_list->sql_kind_backup, sizeof(uint) * share->link_count,
        &result_list->casual_read, sizeof(int) * share->link_count,
        &spider->dbton_handler,
          sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE,
        NullS))
    ) {
      spider_free_share(share);
      goto error_but_no_delete;
    }
    memcpy(tmp_name, share->conn_keys[0], share->conn_keys_charlen);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
    memcpy(tmp_hs_r_name, share->hs_read_conn_keys[0],
      share->hs_read_conn_keys_charlen);
    memcpy(tmp_hs_w_name, share->hs_write_conn_keys[0],
      share->hs_write_conn_keys_charlen);
#endif

    spider->conn_keys_first_ptr = tmp_name;
    for (roop_count = 0; roop_count < (int) share->link_count; roop_count++)
    {
      spider->conn_keys[roop_count] = tmp_name;
      *tmp_name = first_byte;
      tmp_name += share->conn_keys_lengths[roop_count] + 1;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
      spider->hs_r_conn_keys[roop_count] = tmp_hs_r_name;
      tmp_hs_r_name += share->hs_read_conn_keys_lengths[roop_count] + 1;
      spider->hs_w_conn_keys[roop_count] = tmp_hs_w_name;
      tmp_hs_w_name += share->hs_write_conn_keys_lengths[roop_count] + 1;
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
      spider->hs_r_ret_fields[roop_count] = tmp_hs_r_ret_fields;
      tmp_hs_r_ret_fields += table_share->fields;
      spider->hs_w_ret_fields[roop_count] = tmp_hs_w_ret_fields;
      tmp_hs_w_ret_fields += table_share->fields;
#endif
#endif
      spider->m_handler_cid[roop_count] = tmp_cid;
      tmp_cid += SPIDER_SQL_HANDLER_CID_LEN + 1;
      result_list->upd_tmp_tbl_prms[roop_count].init();
      result_list->upd_tmp_tbl_prms[roop_count].field_count = 1;
      spider->conn_kind[roop_count] = SPIDER_CONN_KIND_MYSQL;
    }
    spider_trx_set_link_idx_for_all(spider);

    for (roop_count = 0; roop_count < (int) share->use_dbton_count;
      roop_count++)
    {
      uint dbton_id = share->use_dbton_ids[roop_count];
      if (!(spider->dbton_handler[dbton_id] =
        spider_dbton[dbton_id].create_db_handler(spider,
        share->dbton_share[dbton_id])))
      {
        *error_num = HA_ERR_OUT_OF_MEM;
        break;
      }
      if ((*error_num = spider->dbton_handler[dbton_id]->init()))
      {
        break;
      }
    }
    if (roop_count < (int) share->use_dbton_count)
    {
      for (; roop_count >= 0; roop_count--)
      {
        uint dbton_id = share->use_dbton_ids[roop_count];
        if (spider->dbton_handler[dbton_id])
        {
          delete spider->dbton_handler[dbton_id];
          spider->dbton_handler[dbton_id] = NULL;
        }
      }
      goto error_after_alloc_conn_keys;
    }

    if (
      sql_command != SQLCOM_DROP_TABLE &&
      sql_command != SQLCOM_ALTER_TABLE &&
      sql_command != SQLCOM_SHOW_CREATE
    ) {
      for (
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY);
        roop_count < (int) share->link_count;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count, share->link_count,
          SPIDER_LINK_STATUS_RECOVERY)
      ) {
        if (
          !(spider->conns[roop_count] =
            spider_get_conn(share, roop_count, spider->conn_keys[roop_count],
              spider->wide_handler->trx, spider, FALSE, TRUE,
              SPIDER_CONN_KIND_MYSQL,
              error_num))
        ) {
          if (
            share->monitoring_kind[roop_count] &&
            spider->need_mons[roop_count]
          ) {
            *error_num = spider_ping_table_mon_from_table(
                spider->wide_handler->trx,
                spider->wide_handler->trx->thd,
                share,
                roop_count,
                (uint32) share->monitoring_sid[roop_count],
                share->table_name,
                share->table_name_length,
                spider->conn_link_idx[roop_count],
                NULL,
                0,
                share->monitoring_kind[roop_count],
                share->monitoring_limit[roop_count],
                share->monitoring_flag[roop_count],
                FALSE
              );
          }
          goto error_after_alloc_dbton_handler;
        }
        spider->conns[roop_count]->error_mode &= spider->error_mode;
      }
    }
    search_link_idx = spider_conn_first_link_idx(thd,
      share->link_statuses, share->access_balances, spider->conn_link_idx,
      share->link_count, SPIDER_LINK_STATUS_OK);
    if (search_link_idx == -1)
    {
      char *db = (char *) my_alloca(
        table_share->db.length + 1 + table_share->table_name.length + 1);
      if (!db)
      {
        *error_num = HA_ERR_OUT_OF_MEM;
        goto error_after_alloc_dbton_handler;
      }
      char *table_name = db + table_share->db.length + 1;
      memcpy(db, table_share->db.str, table_share->db.length);
      db[table_share->db.length] = '\0';
      memcpy(table_name, table_share->table_name.str,
        table_share->table_name.length);
      table_name[table_share->table_name.length] = '\0';
      my_printf_error(ER_SPIDER_ALL_LINKS_FAILED_NUM,
        ER_SPIDER_ALL_LINKS_FAILED_STR, MYF(0), db, table_name);
      my_afree(db);
      *error_num = ER_SPIDER_ALL_LINKS_FAILED_NUM;
      goto error_after_alloc_dbton_handler;
    } else if (search_link_idx == -2)
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_after_alloc_dbton_handler;
    }
    spider->search_link_idx = search_link_idx;

    if (share->init_error)
    {
      pthread_mutex_lock(&share->sts_mutex);
      pthread_mutex_lock(&share->crd_mutex);
      if (share->init_error)
      {
        same_server_link = spider_param_same_server_link(thd);
        load_sts_at_startup =
          spider_param_load_sts_at_startup(share->load_sts_at_startup);
        load_crd_at_startup =
          spider_param_load_crd_at_startup(share->load_crd_at_startup);
        if (
          sql_command != SQLCOM_DROP_TABLE &&
          sql_command != SQLCOM_ALTER_TABLE &&
          sql_command != SQLCOM_SHOW_CREATE &&
          !spider->error_mode &&
          (
            !same_server_link ||
            load_sts_at_startup ||
            load_crd_at_startup
          )
        ) {
          SPIDER_INIT_ERROR_TABLE *spider_init_error_table;
          sts_interval = spider_param_sts_interval(thd, share->sts_interval);
          sts_mode = spider_param_sts_mode(thd, share->sts_mode);
#ifdef WITH_PARTITION_STORAGE_ENGINE
          sts_sync = spider_param_sts_sync(thd, share->sts_sync);
          auto_increment_mode = spider_param_auto_increment_mode(thd,
            share->auto_increment_mode);
          if (auto_increment_mode == 1)
            sts_sync = 0;
#endif
          crd_interval = spider_param_crd_interval(thd, share->crd_interval);
          crd_mode = spider_param_crd_mode(thd, share->crd_mode);
          if (crd_mode == 3)
            crd_mode = 1;
#ifdef WITH_PARTITION_STORAGE_ENGINE
          crd_sync = spider_param_crd_sync(thd, share->crd_sync);
#endif
          time_t tmp_time = (time_t) time((time_t*) 0);
          if ((spider_init_error_table =
            spider_get_init_error_table(spider->wide_handler->trx, share,
              FALSE)))
          {
            DBUG_PRINT("info",("spider diff2=%f",
              difftime(tmp_time, spider_init_error_table->init_error_time)));
            if (difftime(tmp_time,
              spider_init_error_table->init_error_time) <
              spider_param_table_init_error_interval())
            {
              *error_num = spider_init_error_table->init_error;
              if (spider_init_error_table->init_error_with_message)
                my_message(spider_init_error_table->init_error,
                  spider_init_error_table->init_error_msg, MYF(0));
              pthread_mutex_unlock(&share->crd_mutex);
              pthread_mutex_unlock(&share->sts_mutex);
              goto error_after_alloc_dbton_handler;
            }
          }

          if (
            (
              !same_server_link ||
              load_sts_at_startup
            ) &&
            (*error_num = spider_get_sts(share, spider->search_link_idx,
              tmp_time, spider, sts_interval, sts_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
              sts_sync,
#endif
              1, HA_STATUS_VARIABLE | HA_STATUS_CONST | HA_STATUS_AUTO))
          ) {
            if (*error_num != ER_SPIDER_SYS_TABLE_VERSION_NUM)
            {
              thd->clear_error();
            } else {
              pthread_mutex_unlock(&share->crd_mutex);
              pthread_mutex_unlock(&share->sts_mutex);
              goto error_after_alloc_dbton_handler;
            }
          }
          if (
            (
              !same_server_link ||
              load_crd_at_startup
            ) &&
            (*error_num = spider_get_crd(share, spider->search_link_idx,
              tmp_time, spider, table, crd_interval, crd_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
              crd_sync,
#endif
              1))
          ) {
            if (*error_num != ER_SPIDER_SYS_TABLE_VERSION_NUM)
            {
              thd->clear_error();
            } else {
              pthread_mutex_unlock(&share->crd_mutex);
              pthread_mutex_unlock(&share->sts_mutex);
              goto error_after_alloc_dbton_handler;
            }
          }
        }
        share->init_error = FALSE;
      }
      pthread_mutex_unlock(&share->crd_mutex);
      pthread_mutex_unlock(&share->sts_mutex);
    }
  }

  DBUG_PRINT("info",("spider share=%p", share));
  DBUG_RETURN(share);

error_after_alloc_dbton_handler:
  for (roop_count = 0; roop_count < (int) share->use_dbton_count; ++roop_count)
  {
    uint dbton_id = share->use_dbton_ids[roop_count];
    if (spider->dbton_handler[dbton_id])
    {
      delete spider->dbton_handler[dbton_id];
      spider->dbton_handler[dbton_id] = NULL;
    }
  }
error_after_alloc_conn_keys:
  spider_free(spider_current_trx, spider->conn_keys, MYF(0));
  spider->conn_keys = NULL;
  spider_free_share(share);
  goto error_but_no_delete;

error_hash_insert:
  spider_free_share_resource_only(share);
error_alloc_share:
  pthread_mutex_unlock(&spider_tbl_mutex);
error_open_sys_table:
#ifndef WITHOUT_SPIDER_BG_SEARCH
error_crd_spider_init:
error_sts_spider_init:
#endif
  if (init_mem_root)
  {
    free_root(&mem_root, MYF(0));
    init_mem_root = FALSE;
  }
error_but_no_delete:
  DBUG_RETURN(NULL);
}

void spider_free_share_resource_only(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_free_share_resource_only");
  spider_free_share_alloc(share);
  pthread_mutex_destroy(&share->crd_mutex);
  pthread_mutex_destroy(&share->sts_mutex);
  pthread_mutex_destroy(&share->mutex);
  spider_free(spider_current_trx, share, MYF(0));
  DBUG_VOID_RETURN;
}

int spider_free_share(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_free_share");
  pthread_mutex_lock(&spider_tbl_mutex);
  bool do_delete_thd = false;
  THD *thd = current_thd;
  if (!--share->use_count)
  {
#ifndef WITHOUT_SPIDER_BG_SEARCH
    spider_free_sts_thread(share);
    spider_free_crd_thread(share);
    spider_free_mon_threads(share);
    if (share->sts_spider_init)
    {
      spider_table_remove_share_from_sts_thread(share);
      spider_free_spider_object_for_share(&share->sts_spider);
    }
    if (share->crd_spider_init)
    {
      spider_table_remove_share_from_crd_thread(share);
      spider_free_spider_object_for_share(&share->crd_spider);
    }
#endif
    if (
      share->sts_init &&
      share->table_share->tmp_table == NO_TMP_TABLE &&
      spider_param_store_last_sts(share->store_last_sts)
    ) {
      if (!thd)
      {
        /* Create a thread for Spider system table update */
        thd = spider_create_thd();
        if (!thd)
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        do_delete_thd = TRUE;
      }
      spider_sys_insert_or_update_table_sts(
        thd,
        share->lgtm_tblhnd_share->table_name,
        share->lgtm_tblhnd_share->table_name_length,
        &share->stat,
        FALSE
      );
    }
    if (
      share->crd_init &&
      share->table_share->tmp_table == NO_TMP_TABLE &&
      spider_param_store_last_crd(share->store_last_crd)
    ) {
      if (!thd)
      {
        /* Create a thread for Spider system table update */
        thd = spider_create_thd();
        if (!thd)
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        do_delete_thd = TRUE;
      }
      spider_sys_insert_or_update_table_crd(
        thd,
        share->lgtm_tblhnd_share->table_name,
        share->lgtm_tblhnd_share->table_name_length,
        share->cardinality,
        share->table_share->fields,
        FALSE
      );
    }
    spider_free_share_alloc(share);
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_open_tables,
      share->table_name_hash_value, (uchar*) share);
#else
    my_hash_delete(&spider_open_tables, (uchar*) share);
#endif
    pthread_mutex_destroy(&share->crd_mutex);
    pthread_mutex_destroy(&share->sts_mutex);
    pthread_mutex_destroy(&share->mutex);
    free_root(&share->mem_root, MYF(0));
    spider_free(spider_current_trx, share, MYF(0));
  }
  if (do_delete_thd)
    spider_destroy_thd(thd);
  pthread_mutex_unlock(&spider_tbl_mutex);
  DBUG_RETURN(0);
}

void spider_update_link_status_for_share(
  const char *table_name,
  uint table_name_length,
  int link_idx,
  long link_status
) {
  SPIDER_SHARE *share;
  DBUG_ENTER("spider_update_link_status_for_share");

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) table_name, table_name_length);
#endif
  pthread_mutex_lock(&spider_tbl_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((share = (SPIDER_SHARE*) my_hash_search_using_hash_value(
    &spider_open_tables, hash_value, (uchar*) table_name,
    table_name_length)))
#else
  if ((share = (SPIDER_SHARE*) my_hash_search(&spider_open_tables,
    (uchar*) table_name, table_name_length)))
#endif
  {
    DBUG_PRINT("info", ("spider share->link_status_init=%s",
      share->link_status_init ? "TRUE" : "FALSE"));
    if (share->link_status_init)
    {
      DBUG_PRINT("info", ("spider share->link_statuses[%d]=%ld",
        link_idx, link_status));
      share->link_statuses[link_idx] = link_status;
    }
  }
  pthread_mutex_unlock(&spider_tbl_mutex);
  DBUG_VOID_RETURN;
}

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
SPIDER_LGTM_TBLHND_SHARE *spider_get_lgtm_tblhnd_share(
  const char *table_name,
  uint table_name_length,
  my_hash_value_type hash_value,
  bool locked,
  bool need_to_create,
  int *error_num
)
#else
SPIDER_LGTM_TBLHND_SHARE *spider_get_lgtm_tblhnd_share(
  const char *table_name,
  uint table_name_length,
  bool locked,
  bool need_to_create,
  int *error_num
)
#endif
{
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share;
  char *tmp_name;
  DBUG_ENTER("spider_get_lgtm_tblhnd_share");

  if (!locked)
    pthread_mutex_lock(&spider_lgtm_tblhnd_share_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(lgtm_tblhnd_share = (SPIDER_LGTM_TBLHND_SHARE*)
    my_hash_search_using_hash_value(
    &spider_lgtm_tblhnd_share_hash, hash_value,
    (uchar*) table_name, table_name_length)))
#else
  if (!(lgtm_tblhnd_share = (SPIDER_LGTM_TBLHND_SHARE*) my_hash_search(
    &spider_lgtm_tblhnd_share_hash,
    (uchar*) table_name, table_name_length)))
#endif
  {
    DBUG_PRINT("info",("spider create new lgtm tblhnd share"));
    if (!(lgtm_tblhnd_share = (SPIDER_LGTM_TBLHND_SHARE *)
      spider_bulk_malloc(spider_current_trx, 244, MYF(MY_WME | MY_ZEROFILL),
        &lgtm_tblhnd_share, (uint) (sizeof(*lgtm_tblhnd_share)),
        &tmp_name, (uint) (table_name_length + 1),
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_share;
    }

    lgtm_tblhnd_share->table_name_length = table_name_length;
    lgtm_tblhnd_share->table_name = tmp_name;
    memcpy(lgtm_tblhnd_share->table_name, table_name,
      lgtm_tblhnd_share->table_name_length);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    lgtm_tblhnd_share->table_path_hash_value = hash_value;
#endif

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&lgtm_tblhnd_share->auto_increment_mutex,
      MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_share_auto_increment,
      &lgtm_tblhnd_share->auto_increment_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_auto_increment_mutex;
    }

    uint old_elements = spider_lgtm_tblhnd_share_hash.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    if (my_hash_insert_with_hash_value(&spider_lgtm_tblhnd_share_hash,
      hash_value, (uchar*) lgtm_tblhnd_share))
#else
    if (my_hash_insert(&spider_lgtm_tblhnd_share_hash,
      (uchar*) lgtm_tblhnd_share))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_hash_insert;
    }
    if (spider_lgtm_tblhnd_share_hash.array.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        spider_lgtm_tblhnd_share_hash,
        (spider_lgtm_tblhnd_share_hash.array.max_element - old_elements) *
        spider_lgtm_tblhnd_share_hash.array.size_of_element);
    }
  }
  if (!locked)
    pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);

  DBUG_PRINT("info",("spider lgtm_tblhnd_share=%p", lgtm_tblhnd_share));
  DBUG_RETURN(lgtm_tblhnd_share);

error_hash_insert:
  pthread_mutex_destroy(&lgtm_tblhnd_share->auto_increment_mutex);
error_init_auto_increment_mutex:
  spider_free(spider_current_trx, lgtm_tblhnd_share, MYF(0));
error_alloc_share:
  if (!locked)
    pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
  DBUG_RETURN(NULL);
}

void spider_free_lgtm_tblhnd_share_alloc(
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share,
  bool locked
) {
  DBUG_ENTER("spider_free_lgtm_tblhnd_share");
  if (!locked)
    pthread_mutex_lock(&spider_lgtm_tblhnd_share_mutex);
#ifdef HASH_UPDATE_WITH_HASH_VALUE
  my_hash_delete_with_hash_value(&spider_lgtm_tblhnd_share_hash,
    lgtm_tblhnd_share->table_path_hash_value, (uchar*) lgtm_tblhnd_share);
#else
  my_hash_delete(&spider_lgtm_tblhnd_share_hash, (uchar*) lgtm_tblhnd_share);
#endif
  pthread_mutex_destroy(&lgtm_tblhnd_share->auto_increment_mutex);
  spider_free(spider_current_trx, lgtm_tblhnd_share, MYF(0));
  if (!locked)
    pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
  DBUG_VOID_RETURN;
}

SPIDER_WIDE_SHARE *spider_get_wide_share(
  SPIDER_SHARE *share,
  TABLE_SHARE *table_share,
  int *error_num
) {
  SPIDER_WIDE_SHARE *wide_share;
  char *tmp_name;
  longlong *tmp_cardinality;
  DBUG_ENTER("spider_get_wide_share");

  pthread_mutex_lock(&spider_wide_share_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(wide_share = (SPIDER_WIDE_SHARE*)
    my_hash_search_using_hash_value(
    &spider_open_wide_share, share->table_path_hash_value,
    (uchar*) table_share->path.str, table_share->path.length)))
#else
  if (!(wide_share = (SPIDER_WIDE_SHARE*) my_hash_search(
    &spider_open_wide_share,
    (uchar*) table_share->path.str, table_share->path.length)))
#endif
  {
    DBUG_PRINT("info",("spider create new wide share"));
    if (!(wide_share = (SPIDER_WIDE_SHARE *)
      spider_bulk_malloc(spider_current_trx, 51, MYF(MY_WME | MY_ZEROFILL),
        &wide_share, sizeof(SPIDER_WIDE_SHARE),
        &tmp_name, (uint) (table_share->path.length + 1),
        &tmp_cardinality,
          (uint) (sizeof(*tmp_cardinality) * table_share->fields),
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_share;
    }

    wide_share->use_count = 0;
    wide_share->table_name_length = table_share->path.length;
    wide_share->table_name = tmp_name;
    memcpy(wide_share->table_name, table_share->path.str,
      wide_share->table_name_length);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    wide_share->table_path_hash_value = share->table_path_hash_value;
#endif
    wide_share->cardinality = tmp_cardinality;

    wide_share->crd_get_time = wide_share->sts_get_time =
      share->crd_get_time;

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&wide_share->sts_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_wide_share_sts,
      &wide_share->sts_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_sts_mutex;
    }

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&wide_share->crd_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_wide_share_crd,
      &wide_share->crd_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_crd_mutex;
    }

    thr_lock_init(&wide_share->lock);

    uint old_elements = spider_open_wide_share.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    if (my_hash_insert_with_hash_value(&spider_open_wide_share,
      share->table_path_hash_value,
      (uchar*) wide_share))
#else
    if (my_hash_insert(&spider_open_wide_share, (uchar*) wide_share))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_hash_insert;
    }
    if (spider_open_wide_share.array.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        spider_open_wide_share,
        (spider_open_wide_share.array.max_element - old_elements) *
        spider_open_wide_share.array.size_of_element);
    }
  }
  wide_share->use_count++;
  pthread_mutex_unlock(&spider_wide_share_mutex);

  DBUG_PRINT("info",("spider wide_share=%p", wide_share));
  DBUG_RETURN(wide_share);

error_hash_insert:
  pthread_mutex_destroy(&wide_share->crd_mutex);
error_init_crd_mutex:
  pthread_mutex_destroy(&wide_share->sts_mutex);
error_init_sts_mutex:
  spider_free(spider_current_trx, wide_share, MYF(0));
error_alloc_share:
  pthread_mutex_unlock(&spider_wide_share_mutex);
  DBUG_RETURN(NULL);
}

int spider_free_wide_share(
  SPIDER_WIDE_SHARE *wide_share
) {
  DBUG_ENTER("spider_free_wide_share");
  pthread_mutex_lock(&spider_wide_share_mutex);
  if (!--wide_share->use_count)
  {
    thr_lock_delete(&wide_share->lock);
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_open_wide_share,
      wide_share->table_path_hash_value, (uchar*) wide_share);
#else
    my_hash_delete(&spider_open_wide_share, (uchar*) wide_share);
#endif
    pthread_mutex_destroy(&wide_share->crd_mutex);
    pthread_mutex_destroy(&wide_share->sts_mutex);
    spider_free(spider_current_trx, wide_share, MYF(0));
  }
  pthread_mutex_unlock(&spider_wide_share_mutex);
  DBUG_RETURN(0);
}

void spider_copy_sts_to_wide_share(
  SPIDER_WIDE_SHARE *wide_share,
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_copy_sts_to_pt_share");
  wide_share->stat = share->stat;
  DBUG_VOID_RETURN;
}

void spider_copy_sts_to_share(
  SPIDER_SHARE *share,
  SPIDER_WIDE_SHARE *wide_share
) {
  DBUG_ENTER("spider_copy_sts_to_share");
  share->stat = wide_share->stat;
  DBUG_VOID_RETURN;
}

void spider_copy_crd_to_wide_share(
  SPIDER_WIDE_SHARE *wide_share,
  SPIDER_SHARE *share,
  int fields
) {
  DBUG_ENTER("spider_copy_crd_to_wide_share");
  memcpy(wide_share->cardinality, share->cardinality,
    sizeof(longlong) * fields);
  DBUG_VOID_RETURN;
}

void spider_copy_crd_to_share(
  SPIDER_SHARE *share,
  SPIDER_WIDE_SHARE *wide_share,
  int fields
) {
  DBUG_ENTER("spider_copy_crd_to_share");
  memcpy(share->cardinality, wide_share->cardinality,
    sizeof(longlong) * fields);
  DBUG_VOID_RETURN;
}

int spider_open_all_tables(
  SPIDER_TRX *trx,
  bool lock
) {
  THD *thd = trx->thd;
  TABLE *table_tables;
  int error_num, *need_mon, mon_val;
  SPIDER_SHARE tmp_share;
  char *db_name, *table_name;
  uint db_name_length, table_name_length;
  char *tmp_connect_info[SPIDER_TMP_SHARE_CHAR_PTR_COUNT];
  uint tmp_connect_info_length[SPIDER_TMP_SHARE_UINT_COUNT];
  long tmp_long[SPIDER_TMP_SHARE_LONG_COUNT];
  longlong tmp_longlong[SPIDER_TMP_SHARE_LONGLONG_COUNT];
  SPIDER_CONN *conn, **conns;
  ha_spider *spider;
  SPIDER_SHARE *share;
  char **connect_info;
  uint *connect_info_length;
  long *long_info;
  longlong *longlong_info;
  MEM_ROOT mem_root;
  SPIDER_Open_tables_backup open_tables_backup;
  DBUG_ENTER("spider_open_all_tables");
  if (
    !(table_tables = spider_open_sys_table(
      thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
      SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, TRUE,
      &error_num))
  )
    DBUG_RETURN(error_num);
  if (
    (error_num = spider_sys_index_first(table_tables, 1))
  ) {
    if (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    {
      table_tables->file->print_error(error_num, MYF(0));
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, TRUE);
      DBUG_RETURN(error_num);
    } else {
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, TRUE);
      DBUG_RETURN(0);
    }
  }

  SPD_INIT_ALLOC_ROOT(&mem_root, 4096, 0, MYF(MY_WME));
  memset((void*)&tmp_share, 0, sizeof(SPIDER_SHARE));
  memset(&tmp_connect_info, 0,
    sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT);
  memset(tmp_connect_info_length, 0,
    sizeof(uint) * SPIDER_TMP_SHARE_UINT_COUNT);
  memset(tmp_long, 0, sizeof(long) * SPIDER_TMP_SHARE_LONG_COUNT);
  memset(tmp_longlong, 0, sizeof(longlong) * SPIDER_TMP_SHARE_LONGLONG_COUNT);
  spider_set_tmp_share_pointer(&tmp_share, (char **) &tmp_connect_info,
    tmp_connect_info_length, tmp_long, tmp_longlong);
  tmp_share.link_statuses[0] = -1;

  do {
    if (
      (error_num = spider_get_sys_tables(
        table_tables, &db_name, &table_name, &mem_root)) ||
      (error_num = spider_get_sys_tables_connect_info(
        table_tables, &tmp_share, 0, &mem_root)) ||
      (error_num = spider_set_connect_info_default(
        &tmp_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
        NULL,
        NULL,
#endif
        NULL
      ))
    ) {
      spider_sys_index_end(table_tables);
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, TRUE);
      spider_free_tmp_share_alloc(&tmp_share);
      free_root(&mem_root, MYF(0));
      DBUG_RETURN(error_num);
    }
    db_name_length = strlen(db_name);
    table_name_length = strlen(table_name);

    if (
      (error_num = spider_set_connect_info_default_db_table(
        &tmp_share,
        db_name,
        db_name_length,
        table_name,
        table_name_length
      )) ||
      (error_num = spider_create_conn_keys(&tmp_share)) ||
/*
      (error_num = spider_db_create_table_names_str(&tmp_share)) ||
*/
      (error_num = spider_create_tmp_dbton_share(&tmp_share))
    ) {
      spider_sys_index_end(table_tables);
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, TRUE);
      spider_free_tmp_share_alloc(&tmp_share);
      free_root(&mem_root, MYF(0));
      DBUG_RETURN(error_num);
    }

    /* create conn */
    if (
      !(conn = spider_get_conn(
        &tmp_share, 0, tmp_share.conn_keys[0], trx, NULL, FALSE, FALSE,
        SPIDER_CONN_KIND_MYSQL, &error_num))
    ) {
      spider_sys_index_end(table_tables);
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, TRUE);
      spider_free_tmp_dbton_share(&tmp_share);
      spider_free_tmp_share_alloc(&tmp_share);
      free_root(&mem_root, MYF(0));
      DBUG_RETURN(error_num);
    }
    conn->error_mode &= spider_param_error_read_mode(thd, 0);
    conn->error_mode &= spider_param_error_write_mode(thd, 0);
    pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
    pthread_mutex_lock(&conn->mta_conn_mutex);
    SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
    conn->need_mon = &mon_val;
    DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = TRUE;
    conn->mta_conn_mutex_unlock_later = TRUE;
    if ((error_num = spider_db_before_query(conn, &mon_val)))
    {
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
      pthread_mutex_unlock(&conn->mta_conn_mutex);
      spider_sys_index_end(table_tables);
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, TRUE);
      spider_free_tmp_dbton_share(&tmp_share);
      spider_free_tmp_share_alloc(&tmp_share);
      free_root(&mem_root, MYF(0));
      DBUG_RETURN(error_num);
    }
    DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
    DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
    conn->mta_conn_mutex_lock_already = FALSE;
    conn->mta_conn_mutex_unlock_later = FALSE;
    SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
    pthread_mutex_unlock(&conn->mta_conn_mutex);

    if (lock && spider_param_use_snapshot_with_flush_tables(thd) == 2)
    {
      if (!(spider = new ha_spider()))
      {
        spider_sys_index_end(table_tables);
        spider_close_sys_table(thd, table_tables,
          &open_tables_backup, TRUE);
        spider_free_tmp_dbton_share(&tmp_share);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      spider->wide_handler->lock_type = TL_READ_NO_INSERT;

      if (!(share = (SPIDER_SHARE *)
        spider_bulk_malloc(spider_current_trx, 52, MYF(MY_WME | MY_ZEROFILL),
          &share, (uint) (sizeof(*share)),
          &connect_info,
            (uint) (sizeof(char *) * SPIDER_TMP_SHARE_CHAR_PTR_COUNT),
          &connect_info_length,
            (uint) (sizeof(uint) * SPIDER_TMP_SHARE_UINT_COUNT),
          &long_info, (uint) (sizeof(long) * SPIDER_TMP_SHARE_LONG_COUNT),
          &longlong_info,
            (uint) (sizeof(longlong) * SPIDER_TMP_SHARE_LONGLONG_COUNT),
          &conns, (uint) (sizeof(SPIDER_CONN *)),
          &need_mon, (uint) (sizeof(int)),
          &spider->conn_link_idx, (uint) (sizeof(uint)),
          &spider->conn_can_fo, (uint) (sizeof(uchar)),
          NullS))
      ) {
        delete spider;
        spider_sys_index_end(table_tables);
        spider_close_sys_table(thd, table_tables,
          &open_tables_backup, TRUE);
        spider_free_tmp_dbton_share(&tmp_share);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      memcpy((void*)share, &tmp_share, sizeof(*share));
      spider_set_tmp_share_pointer(share, connect_info,
        connect_info_length, long_info, longlong_info);
      memcpy(connect_info, &tmp_connect_info, sizeof(char *) *
        SPIDER_TMP_SHARE_CHAR_PTR_COUNT);
      memcpy(connect_info_length, &tmp_connect_info_length, sizeof(uint) *
        SPIDER_TMP_SHARE_UINT_COUNT);
      memcpy(long_info, &tmp_long, sizeof(long) * SPIDER_TMP_SHARE_LONG_COUNT);
      memcpy(longlong_info, &tmp_longlong, sizeof(longlong) *
        SPIDER_TMP_SHARE_LONGLONG_COUNT);
      spider->share = share;
      spider->wide_handler->trx = trx;
      spider->conns = conns;
      spider->need_mons = need_mon;
      spider->conn_link_idx[0] = 0;
      spider->conn_can_fo[0] = 0;
      if ((error_num = spider_create_tmp_dbton_handler(spider)))
      {
        spider_free(trx, share, MYF(0));
        delete spider;
        spider_sys_index_end(table_tables);
        spider_close_sys_table(thd, table_tables,
          &open_tables_backup, TRUE);
        spider_free_tmp_dbton_share(&tmp_share);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        DBUG_RETURN(error_num);
      }

      /* create another conn */
      if (
        (!(conn = spider_get_conn(
        &tmp_share, 0, tmp_share.conn_keys[0], trx, spider, TRUE, FALSE,
        SPIDER_CONN_KIND_MYSQL, &error_num)))
      ) {
        spider_free_tmp_dbton_handler(spider);
        spider_free(trx, share, MYF(0));
        delete spider;
        spider_sys_index_end(table_tables);
        spider_close_sys_table(thd, table_tables,
          &open_tables_backup, TRUE);
        spider_free_tmp_dbton_share(&tmp_share);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        DBUG_RETURN(error_num);
      }
      conn->error_mode &= spider_param_error_read_mode(thd, 0);
      conn->error_mode &= spider_param_error_write_mode(thd, 0);

      spider->next = NULL;
      if (conn->another_ha_last)
      {
        ((ha_spider*) conn->another_ha_last)->next = spider;
      } else {
        conn->another_ha_first = (void*) spider;
      }
      conn->another_ha_last = (void*) spider;

      int appended = 0;
      if ((error_num = spider->dbton_handler[conn->dbton_id]->
        append_lock_tables_list(conn, 0, &appended)))
      {
        spider_free_tmp_dbton_handler(spider);
        spider_free(trx, share, MYF(0));
        delete spider;
        spider_sys_index_end(table_tables);
        spider_close_sys_table(thd, table_tables,
          &open_tables_backup, TRUE);
        spider_free_tmp_dbton_share(&tmp_share);
        spider_free_tmp_share_alloc(&tmp_share);
        free_root(&mem_root, MYF(0));
        DBUG_RETURN(error_num);
      }
    } else {
      spider_free_tmp_dbton_share(&tmp_share);
      spider_free_tmp_share_alloc(&tmp_share);
    }
    error_num = spider_sys_index_next(table_tables);
  } while (error_num == 0);
  free_root(&mem_root, MYF(0));

  spider_sys_index_end(table_tables);
  spider_close_sys_table(thd, table_tables,
    &open_tables_backup, TRUE);
  DBUG_RETURN(0);
}

bool spider_flush_logs(
  handlerton *hton
) {
  int error_num;
  THD* thd = current_thd;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_flush_logs");

  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
  {
    my_errno = error_num;
    DBUG_RETURN(TRUE);
  }
  if (
    spider_param_use_flash_logs(trx->thd) &&
    (
      !trx->trx_consistent_snapshot ||
      !spider_param_use_all_conns_snapshot(trx->thd) ||
      !spider_param_use_snapshot_with_flush_tables(trx->thd)
    )
  ) {
    if (
      (error_num = spider_open_all_tables(trx, FALSE)) ||
      (error_num = spider_trx_all_flush_logs(trx))
    ) {
      my_errno = error_num;
      DBUG_RETURN(TRUE);
    }
  }

  DBUG_RETURN(FALSE);
}

handler* spider_create_handler(
  handlerton *hton,
  TABLE_SHARE *table, 
  MEM_ROOT *mem_root
) {
  DBUG_ENTER("spider_create_handler");
#ifndef WITHOUT_SPIDER_BG_SEARCH
  SPIDER_THREAD *thread = &spider_table_sts_threads[0];
  if (unlikely(thread->init_command))
  {
    THD *thd = current_thd;
    pthread_cond_t *cond = thd->mysys_var->current_cond;
    pthread_mutex_t *mutex = thd->mysys_var->current_mutex;
    /* wait for finishing init_command */
    pthread_mutex_lock(&thread->mutex);
    if (unlikely(thread->init_command))
    {
      thd->mysys_var->current_cond = &thread->sync_cond;
      thd->mysys_var->current_mutex = &thread->mutex;
      pthread_cond_wait(&thread->sync_cond, &thread->mutex);
    }
    pthread_mutex_unlock(&thread->mutex);
    thd->mysys_var->current_cond = cond;
    thd->mysys_var->current_mutex = mutex;
    if (thd->killed)
    {
      DBUG_RETURN(NULL);
    }
  }
#endif
  DBUG_RETURN(new (mem_root) ha_spider(hton, table));
}

int spider_close_connection(
  handlerton* hton,
  THD* thd
) {
  int roop_count = 0;
  SPIDER_CONN *conn;
  SPIDER_TRX *trx;
  DBUG_ENTER("spider_close_connection");
  if (!(trx = (SPIDER_TRX*) thd_get_ha_data(thd, spider_hton_ptr)))
    DBUG_RETURN(0); /* transaction is not started */

  trx->tmp_spider->conns = &conn;
  while ((conn = (SPIDER_CONN*) my_hash_element(&trx->trx_conn_hash,
    roop_count)))
  {
    SPIDER_BACKUP_DASTATUS;
    DBUG_PRINT("info",("spider conn->table_lock=%d", conn->table_lock));
    if (conn->table_lock > 0)
    {
      if (!conn->trx_start)
        conn->disable_reconnect = FALSE;
      if (conn->table_lock != 2)
      {
        spider_db_unlock_tables(trx->tmp_spider, 0);
      }
      conn->table_lock = 0;
    }
    roop_count++;
    SPIDER_CONN_RESTORE_DASTATUS;
  }

  spider_rollback(spider_hton_ptr, thd, TRUE);
  spider_free_trx(trx, TRUE, false);

  DBUG_RETURN(0);
}

void spider_drop_database(
  handlerton *hton,
  char* path
) {
  DBUG_ENTER("spider_drop_database");
  DBUG_VOID_RETURN;
}

bool spider_show_status(
  handlerton *hton,
  THD *thd, 
  stat_print_fn *stat_print,
  enum ha_stat_type stat_type
) {
  DBUG_ENTER("spider_show_status");
  switch (stat_type) {
    case HA_ENGINE_STATUS:
    default:
      DBUG_RETURN(FALSE);
  }
}

int spider_db_done(
  void *p
) {
  int roop_count;
  bool do_delete_thd;
  THD *thd = current_thd, *tmp_thd;
  SPIDER_CONN *conn;
  SPIDER_INIT_ERROR_TABLE *spider_init_error_table;
  SPIDER_TABLE_MON_LIST *table_mon_list;
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share;
  DBUG_ENTER("spider_db_done");

  /* Begin Spider plugin deinit */
  if (thd)
    do_delete_thd = FALSE;
  else
  {
    /* Create a thread for Spider plugin deinit */
    thd = spider_create_thd();
    if (!thd)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    do_delete_thd = TRUE;
  }

  for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; roop_count--)
  {
    if (spider_dbton[roop_count].deinit)
    {
      spider_dbton[roop_count].deinit();
    }
  }

#ifndef WITHOUT_SPIDER_BG_SEARCH
  for (roop_count = spider_param_table_crd_thread_count() - 1;
    roop_count >= 0; roop_count--)
  {
    spider_free_crd_threads(&spider_table_crd_threads[roop_count]);
  }
  for (roop_count = spider_param_table_sts_thread_count() - 1;
    roop_count >= 0; roop_count--)
  {
    spider_free_sts_threads(&spider_table_sts_threads[roop_count]);
  }
  spider_free(NULL, spider_table_sts_threads, MYF(0));
#endif

  for (roop_count = spider_param_udf_table_mon_mutex_count() - 1;
    roop_count >= 0; roop_count--)
  {
    while ((table_mon_list = (SPIDER_TABLE_MON_LIST *) my_hash_element(
      &spider_udf_table_mon_list_hash[roop_count], 0)))
    {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
      my_hash_delete_with_hash_value(
        &spider_udf_table_mon_list_hash[roop_count],
        table_mon_list->key_hash_value, (uchar*) table_mon_list);
#else
      my_hash_delete(&spider_udf_table_mon_list_hash[roop_count],
        (uchar*) table_mon_list);
#endif
      spider_ping_table_free_mon_list(table_mon_list);
    }
    spider_free_mem_calc(spider_current_trx,
      spider_udf_table_mon_list_hash_id,
      spider_udf_table_mon_list_hash[roop_count].array.max_element *
      spider_udf_table_mon_list_hash[roop_count].array.size_of_element);
    my_hash_free(&spider_udf_table_mon_list_hash[roop_count]);
  }
  for (roop_count = spider_param_udf_table_mon_mutex_count() - 1;
    roop_count >= 0; roop_count--)
    pthread_cond_destroy(&spider_udf_table_mon_conds[roop_count]);
  for (roop_count = spider_param_udf_table_mon_mutex_count() - 1;
    roop_count >= 0; roop_count--)
    pthread_mutex_destroy(&spider_udf_table_mon_mutexes[roop_count]);
  spider_free(NULL, spider_udf_table_mon_mutexes, MYF(0));

  pthread_mutex_lock(&spider_allocated_thds_mutex);
  while ((tmp_thd = (THD *) my_hash_element(&spider_allocated_thds, 0)))
  {
    SPIDER_TRX *trx = (SPIDER_TRX *)
                      thd_get_ha_data(tmp_thd, spider_hton_ptr);
    if (trx)
    {
      DBUG_ASSERT(tmp_thd == trx->thd);
      spider_free_trx(trx, FALSE);
      thd_set_ha_data(tmp_thd, spider_hton_ptr, NULL);
    }
    else
      my_hash_delete(&spider_allocated_thds, (uchar *) tmp_thd);
  }
  pthread_mutex_unlock(&spider_allocated_thds_mutex);

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  pthread_mutex_lock(&spider_hs_w_conn_mutex);
  while ((conn = (SPIDER_CONN*) my_hash_element(&spider_hs_w_conn_hash, 0)))
  {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_hs_w_conn_hash,
      conn->conn_key_hash_value, (uchar*) conn);
#else
    my_hash_delete(&spider_hs_w_conn_hash, (uchar*) conn);
#endif
    spider_free_conn(conn);
  }
  pthread_mutex_unlock(&spider_hs_w_conn_mutex);
  pthread_mutex_lock(&spider_hs_r_conn_mutex);
  while ((conn = (SPIDER_CONN*) my_hash_element(&spider_hs_r_conn_hash, 0)))
  {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_hs_r_conn_hash,
      conn->conn_key_hash_value, (uchar*) conn);
#else
    my_hash_delete(&spider_hs_r_conn_hash, (uchar*) conn);
#endif
    spider_free_conn(conn);
  }
  pthread_mutex_unlock(&spider_hs_r_conn_mutex);
#endif
  pthread_mutex_lock(&spider_conn_mutex);
  while ((conn = (SPIDER_CONN*) my_hash_element(&spider_open_connections, 0)))
  {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_open_connections,
      conn->conn_key_hash_value, (uchar*) conn);
#else
    my_hash_delete(&spider_open_connections, (uchar*) conn);
#endif
    spider_free_conn(conn);
  }
  pthread_mutex_unlock(&spider_conn_mutex);
  pthread_mutex_lock(&spider_lgtm_tblhnd_share_mutex);
  while ((lgtm_tblhnd_share = (SPIDER_LGTM_TBLHND_SHARE*) my_hash_element(
    &spider_lgtm_tblhnd_share_hash, 0)))
  {
    spider_free_lgtm_tblhnd_share_alloc(lgtm_tblhnd_share, TRUE);
  }
  pthread_mutex_unlock(&spider_lgtm_tblhnd_share_mutex);
  spider_free_mem_calc(spider_current_trx,
    spider_mon_table_cache_id,
    spider_mon_table_cache.max_element *
    spider_mon_table_cache.size_of_element);
  delete_dynamic(&spider_mon_table_cache);
  spider_free_mem_calc(spider_current_trx,
    spider_allocated_thds_id,
    spider_allocated_thds.array.max_element *
    spider_allocated_thds.array.size_of_element);
  my_hash_free(&spider_allocated_thds);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_free_mem_calc(spider_current_trx,
    spider_hs_w_conn_hash_id,
    spider_hs_w_conn_hash.array.max_element *
    spider_hs_w_conn_hash.array.size_of_element);
  my_hash_free(&spider_hs_w_conn_hash);
  spider_free_mem_calc(spider_current_trx,
    spider_hs_r_conn_hash_id,
    spider_hs_r_conn_hash.array.max_element *
    spider_hs_r_conn_hash.array.size_of_element);
  my_hash_free(&spider_hs_r_conn_hash);
#endif
  spider_free_mem_calc(spider_current_trx,
    spider_open_connections_id,
    spider_open_connections.array.max_element *
    spider_open_connections.array.size_of_element);
  my_hash_free(&spider_open_connections);
  my_hash_free(&spider_ipport_conns);
  spider_free_mem_calc(spider_current_trx,
    spider_lgtm_tblhnd_share_hash_id,
    spider_lgtm_tblhnd_share_hash.array.max_element *
    spider_lgtm_tblhnd_share_hash.array.size_of_element);
  my_hash_free(&spider_lgtm_tblhnd_share_hash);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  spider_free_mem_calc(spider_current_trx,
    spider_open_wide_share_id,
    spider_open_wide_share.array.max_element *
    spider_open_wide_share.array.size_of_element);
  my_hash_free(&spider_open_wide_share);
#endif
  pthread_mutex_lock(&spider_init_error_tbl_mutex);
  while ((spider_init_error_table = (SPIDER_INIT_ERROR_TABLE*)
    my_hash_element(&spider_init_error_tables, 0)))
  {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_init_error_tables,
      spider_init_error_table->table_name_hash_value,
      (uchar*) spider_init_error_table);
#else
    my_hash_delete(&spider_init_error_tables,
      (uchar*) spider_init_error_table);
#endif
    spider_free(NULL, spider_init_error_table, MYF(0));
  }
  pthread_mutex_unlock(&spider_init_error_tbl_mutex);
  spider_free_mem_calc(spider_current_trx,
    spider_init_error_tables_id,
    spider_init_error_tables.array.max_element *
    spider_init_error_tables.array.size_of_element);
  my_hash_free(&spider_init_error_tables);
  spider_free_mem_calc(spider_current_trx,
    spider_open_tables_id,
    spider_open_tables.array.max_element *
    spider_open_tables.array.size_of_element);
  my_hash_free(&spider_open_tables);
  pthread_mutex_destroy(&spider_mem_calc_mutex);
  pthread_mutex_destroy(&spider_mon_table_cache_mutex);
  pthread_mutex_destroy(&spider_allocated_thds_mutex);
  pthread_mutex_destroy(&spider_open_conn_mutex);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  pthread_mutex_destroy(&spider_hs_w_conn_mutex);
  pthread_mutex_destroy(&spider_hs_r_conn_mutex);
#endif
  pthread_mutex_destroy(&spider_conn_mutex);
  pthread_mutex_destroy(&spider_lgtm_tblhnd_share_mutex);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  pthread_mutex_destroy(&spider_wide_share_mutex);
#endif
  pthread_mutex_destroy(&spider_init_error_tbl_mutex);
  pthread_mutex_destroy(&spider_conn_id_mutex);
  pthread_mutex_destroy(&spider_ipport_conn_mutex);
  pthread_mutex_destroy(&spider_thread_id_mutex);
  pthread_mutex_destroy(&spider_tbl_mutex);
#ifndef WITHOUT_SPIDER_BG_SEARCH
  pthread_attr_destroy(&spider_pt_attr);
#endif

  for (roop_count = 0; roop_count < SPIDER_MEM_CALC_LIST_NUM; roop_count++)
  {
    if (spider_alloc_func_name[roop_count])
      DBUG_PRINT("info",("spider %d %s %s %lu %llu %lld %llu %llu %s",
        roop_count,
        spider_alloc_func_name[roop_count],
        spider_alloc_file_name[roop_count],
        spider_alloc_line_no[roop_count],
        spider_total_alloc_mem[roop_count],
        spider_current_alloc_mem[roop_count],
        spider_alloc_mem_count[roop_count],
        spider_free_mem_count[roop_count],
        spider_current_alloc_mem[roop_count] ? "NG" : "OK"
      ));
  }

  /* End Spider plugin deinit */
  if (do_delete_thd)
    spider_destroy_thd(thd);

/*
DBUG_ASSERT(0);
*/
  DBUG_RETURN(0);
}

int spider_panic(
  handlerton *hton,
  ha_panic_function type
) {
  DBUG_ENTER("spider_panic");
  DBUG_RETURN(0);
}

int spider_db_init(
  void *p
) {
  int error_num = HA_ERR_OUT_OF_MEM, roop_count;
  uint dbton_id = 0;
  uchar addr[6];
  handlerton *spider_hton = (handlerton *)p;
  DBUG_ENTER("spider_db_init");
  spider_hton_ptr = spider_hton;

  spider_hton->flags = HTON_TEMPORARY_NOT_SUPPORTED;
#ifdef HTON_CAN_READ_CONNECT_STRING_IN_PARTITION
  spider_hton->flags |= HTON_CAN_READ_CONNECT_STRING_IN_PARTITION;
#endif
  /* spider_hton->db_type = DB_TYPE_SPIDER; */
  /*
  spider_hton->savepoint_offset;
  spider_hton->savepoint_set = spider_savepoint_set;
  spider_hton->savepoint_rollback = spider_savepoint_rollback;
  spider_hton->savepoint_release = spider_savepoint_release;
  spider_hton->create_cursor_read_view = spider_create_cursor_read_view;
  spider_hton->set_cursor_read_view = spider_set_cursor_read_view;
  spider_hton->close_cursor_read_view = spider_close_cursor_read_view;
  */
  spider_hton->panic = spider_panic;
  spider_hton->close_connection = spider_close_connection;
  spider_hton->start_consistent_snapshot = spider_start_consistent_snapshot;
  spider_hton->flush_logs = spider_flush_logs;
  spider_hton->commit = spider_commit;
  spider_hton->rollback = spider_rollback;
#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
  spider_hton->discover_table_structure = spider_discover_table_structure;
#endif
  if (spider_param_support_xa())
  {
    spider_hton->prepare = spider_xa_prepare;
    spider_hton->recover = spider_xa_recover;
    spider_hton->commit_by_xid = spider_xa_commit_by_xid;
    spider_hton->rollback_by_xid = spider_xa_rollback_by_xid;
  }
  spider_hton->create = spider_create_handler;
  spider_hton->drop_database = spider_drop_database;
  spider_hton->show_status = spider_show_status;
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
  spider_hton->create_group_by = spider_create_group_by_handler;
#endif

  if (my_gethwaddr((uchar *) addr))
  {
    my_printf_error(ER_SPIDER_CANT_NUM, ER_SPIDER_CANT_STR1, MYF(0),
      "get hardware address with error ", errno);
  }
  spider_unique_id.str = spider_unique_id_buf;
  spider_unique_id.length = my_sprintf(spider_unique_id_buf,
    (spider_unique_id_buf, "-%02x%02x%02x%02x%02x%02x-%lx-",
      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], (ulong) getpid()));

  memset(&spider_alloc_func_name, 0, sizeof(spider_alloc_func_name));
  memset(&spider_alloc_file_name, 0, sizeof(spider_alloc_file_name));
  memset(&spider_alloc_line_no, 0, sizeof(spider_alloc_line_no));
  memset(&spider_total_alloc_mem, 0, sizeof(spider_total_alloc_mem));
  memset(&spider_current_alloc_mem, 0, sizeof(spider_current_alloc_mem));
  memset(&spider_alloc_mem_count, 0, sizeof(spider_alloc_mem_count));
  memset(&spider_free_mem_count, 0, sizeof(spider_free_mem_count));

#ifndef SPIDER_HAS_NEXT_THREAD_ID
  spd_db_att_thread_id = &thread_id;
#endif
#ifdef SPIDER_XID_USES_xid_cache_iterate
#else
#ifdef XID_CACHE_IS_SPLITTED
  spd_db_att_xid_cache_split_num = &opt_xid_cache_split_num;
  spd_db_att_LOCK_xid_cache = LOCK_xid_cache;
  spd_db_att_xid_cache = xid_cache;
#else
  spd_db_att_LOCK_xid_cache = &LOCK_xid_cache;
  spd_db_att_xid_cache = &xid_cache;
#endif
#endif
  spd_charset_utf8mb3_bin = &my_charset_utf8mb3_bin;
  spd_defaults_extra_file = &my_defaults_extra_file;
  spd_defaults_file = &my_defaults_file;
  spd_mysqld_unix_port = (const char **) &mysqld_unix_port;
  spd_mysqld_port = &mysqld_port;
  spd_abort_loop = &abort_loop;
  spd_tz_system = my_tz_SYSTEM;
  spd_mysqld_server_started = &mysqld_server_started;
  spd_LOCK_server_started = &LOCK_server_started;
  spd_COND_server_started = &COND_server_started;

#ifdef HAVE_PSI_INTERFACE
  init_spider_psi_keys();
#endif

#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (pthread_attr_init(&spider_pt_attr))
    goto error_pt_attr_init;
/*
  if (pthread_attr_setdetachstate(&spider_pt_attr, PTHREAD_CREATE_DETACHED))
    goto error_pt_attr_setstate;
*/
#endif

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_tbl_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_tbl,
    &spider_tbl_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_tbl_mutex_init;
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_thread_id_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_thread_id,
    &spider_thread_id_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_thread_id_mutex_init;
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_conn_id_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_conn_id,
    &spider_conn_id_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_conn_id_mutex_init;
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_ipport_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_ipport_count,
    &spider_ipport_conn_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_ipport_count_mutex_init;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_init_error_tbl_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_init_error_tbl,
    &spider_init_error_tbl_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_init_error_tbl_mutex_init;

#ifdef WITH_PARTITION_STORAGE_ENGINE
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_wide_share_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_wide_share,
    &spider_wide_share_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_wide_share_mutex_init;

#endif
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_lgtm_tblhnd_share_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_lgtm_tblhnd_share,
    &spider_lgtm_tblhnd_share_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_lgtm_tblhnd_share_mutex_init;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_conn,
    &spider_conn_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_conn_mutex_init;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_open_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_open_conn,
    &spider_open_conn_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_open_conn_mutex_init;

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_hs_r_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_hs_r_conn,
    &spider_hs_r_conn_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_hs_r_conn_mutex_init;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_hs_w_conn_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_hs_w_conn,
    &spider_hs_w_conn_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_hs_w_conn_mutex_init;

#endif
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_allocated_thds_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_allocated_thds,
    &spider_allocated_thds_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_allocated_thds_mutex_init;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_mon_table_cache_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mon_table_cache,
    &spider_mon_table_cache_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_mon_table_cache_mutex_init;

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_mem_calc_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_mem_calc,
    &spider_mem_calc_mutex, MY_MUTEX_INIT_FAST))
#endif
    goto error_mem_calc_mutex_init;

  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_open_tables, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_tbl_get_key, 0, 0))
    goto error_open_tables_hash_init;

  spider_alloc_calc_mem_init(spider_open_tables, 143);
  spider_alloc_calc_mem(NULL,
    spider_open_tables,
    spider_open_tables.array.max_element *
    spider_open_tables.array.size_of_element);
  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_init_error_tables, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_tbl_get_key, 0, 0))
    goto error_init_error_tables_hash_init;

  spider_alloc_calc_mem_init(spider_init_error_tables, 144);
  spider_alloc_calc_mem(NULL,
    spider_init_error_tables,
    spider_init_error_tables.array.max_element *
    spider_init_error_tables.array.size_of_element);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if(
    my_hash_init(PSI_INSTRUMENT_ME, &spider_open_wide_share, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_wide_share_get_key, 0, 0)
  )
    goto error_open_wide_share_hash_init;

  spider_alloc_calc_mem_init(spider_open_wide_share, 145);
  spider_alloc_calc_mem(NULL,
    spider_open_wide_share,
    spider_open_wide_share.array.max_element *
    spider_open_wide_share.array.size_of_element);
#endif
  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_lgtm_tblhnd_share_hash,
                   spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_lgtm_tblhnd_share_hash_get_key, 0, 0))
    goto error_lgtm_tblhnd_share_hash_init;

  spider_alloc_calc_mem_init(spider_lgtm_tblhnd_share_hash, 245);
  spider_alloc_calc_mem(NULL,
    spider_lgtm_tblhnd_share_hash,
    spider_lgtm_tblhnd_share_hash.array.max_element *
    spider_lgtm_tblhnd_share_hash.array.size_of_element);
  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_open_connections, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0))
    goto error_open_connections_hash_init;

  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_ipport_conns, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_ipport_conn_get_key,
                   spider_free_ipport_conn, 0))
      goto error_ipport_conn__hash_init;

  spider_alloc_calc_mem_init(spider_open_connections, 146);
  spider_alloc_calc_mem(NULL,
    spider_open_connections,
    spider_open_connections.array.max_element *
    spider_open_connections.array.size_of_element);
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_hs_r_conn_hash, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0))
    goto error_hs_r_conn_hash_init;

  spider_alloc_calc_mem_init(spider_hs_r_conn_hash, 147);
  spider_alloc_calc_mem(NULL,
    spider_hs_r_conn_hash,
    spider_hs_r_conn_hash.array.max_element *
    spider_hs_r_conn_hash.array.size_of_element);
  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_hs_w_conn_hash, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_conn_get_key, 0, 0))
    goto error_hs_w_conn_hash_init;

  spider_alloc_calc_mem_init(spider_hs_w_conn_hash, 148);
  spider_alloc_calc_mem(NULL,
    spider_hs_w_conn_hash,
    spider_hs_w_conn_hash.array.max_element *
    spider_hs_w_conn_hash.array.size_of_element);
#endif
  if (my_hash_init(PSI_INSTRUMENT_ME, &spider_allocated_thds, spd_charset_utf8mb3_bin, 32, 0, 0,
                   (my_hash_get_key) spider_allocated_thds_get_key, 0, 0))
    goto error_allocated_thds_hash_init;

  spider_alloc_calc_mem_init(spider_allocated_thds, 149);
  spider_alloc_calc_mem(NULL,
    spider_allocated_thds,
    spider_allocated_thds.array.max_element *
    spider_allocated_thds.array.size_of_element);

  if (SPD_INIT_DYNAMIC_ARRAY2(&spider_mon_table_cache, sizeof(SPIDER_MON_KEY),
      NULL, 64, 64, MYF(MY_WME)))
    goto error_mon_table_cache_array_init;

  spider_alloc_calc_mem_init(spider_mon_table_cache, 165);
  spider_alloc_calc_mem(NULL,
    spider_mon_table_cache,
    spider_mon_table_cache.max_element *
    spider_mon_table_cache.size_of_element);

  if (!(spider_udf_table_mon_mutexes = (pthread_mutex_t *)
    spider_bulk_malloc(NULL, 53, MYF(MY_WME | MY_ZEROFILL),
      &spider_udf_table_mon_mutexes, (uint) (sizeof(pthread_mutex_t) *
        spider_param_udf_table_mon_mutex_count()),
      &spider_udf_table_mon_conds, (uint) (sizeof(pthread_cond_t) *
        spider_param_udf_table_mon_mutex_count()),
      &spider_udf_table_mon_list_hash, (uint) (sizeof(HASH) *
        spider_param_udf_table_mon_mutex_count()),
      NullS))
  )
    goto error_alloc_mon_mutxes;

  for (roop_count = 0;
    roop_count < (int) spider_param_udf_table_mon_mutex_count();
    roop_count++)
  {
#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&spider_udf_table_mon_mutexes[roop_count],
      MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(spd_key_mutex_udf_table_mon,
      &spider_udf_table_mon_mutexes[roop_count], MY_MUTEX_INIT_FAST))
#endif
      goto error_init_udf_table_mon_mutex;
  }
  for (roop_count = 0;
    roop_count < (int) spider_param_udf_table_mon_mutex_count();
    roop_count++)
  {
#if MYSQL_VERSION_ID < 50500
    if (pthread_cond_init(&spider_udf_table_mon_conds[roop_count], NULL))
#else
    if (mysql_cond_init(spd_key_cond_udf_table_mon,
      &spider_udf_table_mon_conds[roop_count], NULL))
#endif
      goto error_init_udf_table_mon_cond;
  }
  for (roop_count = 0;
    roop_count < (int) spider_param_udf_table_mon_mutex_count();
    roop_count++)
  {
    if (my_hash_init(PSI_INSTRUMENT_ME, &spider_udf_table_mon_list_hash[roop_count],
      spd_charset_utf8mb3_bin, 32, 0, 0,
      (my_hash_get_key) spider_udf_tbl_mon_list_key, 0, 0))
      goto error_init_udf_table_mon_list_hash;

    spider_alloc_calc_mem_init(spider_udf_table_mon_list_hash, 150);
    spider_alloc_calc_mem(NULL,
      spider_udf_table_mon_list_hash,
      spider_udf_table_mon_list_hash[roop_count].array.max_element *
      spider_udf_table_mon_list_hash[roop_count].array.size_of_element);
  }

#ifndef WITHOUT_SPIDER_BG_SEARCH
  if (!(spider_table_sts_threads = (SPIDER_THREAD *)
    spider_bulk_malloc(NULL, 256, MYF(MY_WME | MY_ZEROFILL),
      &spider_table_sts_threads, (uint) (sizeof(SPIDER_THREAD) *
        spider_param_table_sts_thread_count()),
      &spider_table_crd_threads, (uint) (sizeof(SPIDER_THREAD) *
        spider_param_table_crd_thread_count()),
      NullS))
  )
    goto error_alloc_mon_mutxes;
  spider_table_sts_threads[0].init_command = TRUE;

  for (roop_count = 0;
    roop_count < (int) spider_param_table_sts_thread_count();
    roop_count++)
  {
    if ((error_num = spider_create_sts_threads(&spider_table_sts_threads[roop_count])))
    {
      goto error_init_table_sts_threads;
    }
  }
  for (roop_count = 0;
    roop_count < (int) spider_param_table_crd_thread_count();
    roop_count++)
  {
    if ((error_num = spider_create_crd_threads(&spider_table_crd_threads[roop_count])))
    {
      goto error_init_table_crd_threads;
    }
  }
#endif

  spider_dbton_mysql.dbton_id = dbton_id;
  spider_dbton_mysql.db_util->dbton_id = dbton_id;
  spider_dbton[dbton_id] = spider_dbton_mysql;
  ++dbton_id;
  spider_dbton_mariadb.dbton_id = dbton_id;
  spider_dbton_mariadb.db_util->dbton_id = dbton_id;
  spider_dbton[dbton_id] = spider_dbton_mariadb;
  ++dbton_id;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_dbton_handlersocket.dbton_id = dbton_id;
  spider_dbton_handlersocket.db_util->dbton_id = dbton_id;
  spider_dbton[dbton_id] = spider_dbton_handlersocket;
  ++dbton_id;
#endif
#ifdef HAVE_ORACLE_OCI
  spider_dbton_oracle.dbton_id = dbton_id;
  spider_dbton_oracle.db_util->dbton_id = dbton_id;
  spider_dbton[dbton_id] = spider_dbton_oracle;
  ++dbton_id;
#endif
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    if (spider_dbton[roop_count].init)
    {
      if ((error_num = spider_dbton[roop_count].init()))
      {
        goto error_init_dbton;
      }
    }
  }
  DBUG_RETURN(0);

#ifndef WITHOUT_SPIDER_BG_SEARCH
error_init_dbton:
  for (roop_count--; roop_count >= 0; roop_count--)
  {
    if (spider_dbton[roop_count].deinit)
    {
      spider_dbton[roop_count].deinit();
    }
  }
  roop_count = spider_param_table_crd_thread_count() - 1;
error_init_table_crd_threads:
  for (; roop_count >= 0; roop_count--)
  {
    spider_free_crd_threads(&spider_table_crd_threads[roop_count]);
  }
  roop_count = spider_param_table_sts_thread_count() - 1;
error_init_table_sts_threads:
  for (; roop_count >= 0; roop_count--)
  {
    spider_free_sts_threads(&spider_table_sts_threads[roop_count]);
  }
  spider_free(NULL, spider_table_sts_threads, MYF(0));
  roop_count = spider_param_udf_table_mon_mutex_count() - 1;
#endif
error_init_udf_table_mon_list_hash:
  for (; roop_count >= 0; roop_count--)
  {
    spider_free_mem_calc(NULL,
      spider_udf_table_mon_list_hash_id,
      spider_udf_table_mon_list_hash[roop_count].array.max_element *
      spider_udf_table_mon_list_hash[roop_count].array.size_of_element);
    my_hash_free(&spider_udf_table_mon_list_hash[roop_count]);
  }
  roop_count = spider_param_udf_table_mon_mutex_count() - 1;
error_init_udf_table_mon_cond:
  for (; roop_count >= 0; roop_count--)
    pthread_cond_destroy(&spider_udf_table_mon_conds[roop_count]);
  roop_count = spider_param_udf_table_mon_mutex_count() - 1;
error_init_udf_table_mon_mutex:
  for (; roop_count >= 0; roop_count--)
    pthread_mutex_destroy(&spider_udf_table_mon_mutexes[roop_count]);
  spider_free(NULL, spider_udf_table_mon_mutexes, MYF(0));
error_alloc_mon_mutxes:
  spider_free_mem_calc(NULL,
    spider_mon_table_cache_id,
    spider_mon_table_cache.max_element *
    spider_mon_table_cache.size_of_element);
  delete_dynamic(&spider_mon_table_cache);
error_mon_table_cache_array_init:
  spider_free_mem_calc(NULL,
    spider_allocated_thds_id,
    spider_allocated_thds.array.max_element *
    spider_allocated_thds.array.size_of_element);
  my_hash_free(&spider_allocated_thds);
error_allocated_thds_hash_init:
  my_hash_free(&spider_ipport_conns);
error_ipport_conn__hash_init:
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  spider_free_mem_calc(NULL,
    spider_hs_w_conn_hash_id,
    spider_hs_w_conn_hash.array.max_element *
    spider_hs_w_conn_hash.array.size_of_element);
  my_hash_free(&spider_hs_w_conn_hash);
error_hs_w_conn_hash_init:
  spider_free_mem_calc(NULL,
    spider_hs_r_conn_hash_id,
    spider_hs_r_conn_hash.array.max_element *
    spider_hs_r_conn_hash.array.size_of_element);
  my_hash_free(&spider_hs_r_conn_hash);
error_hs_r_conn_hash_init:
#endif
  spider_free_mem_calc(NULL,
    spider_open_connections_id,
    spider_open_connections.array.max_element *
    spider_open_connections.array.size_of_element);
  my_hash_free(&spider_open_connections);
error_open_connections_hash_init:
  spider_free_mem_calc(NULL,
    spider_lgtm_tblhnd_share_hash_id,
    spider_lgtm_tblhnd_share_hash.array.max_element *
    spider_lgtm_tblhnd_share_hash.array.size_of_element);
  my_hash_free(&spider_lgtm_tblhnd_share_hash);
error_lgtm_tblhnd_share_hash_init:
#ifdef WITH_PARTITION_STORAGE_ENGINE
  spider_free_mem_calc(NULL,
    spider_open_wide_share_id,
    spider_open_wide_share.array.max_element *
    spider_open_wide_share.array.size_of_element);
  my_hash_free(&spider_open_wide_share);
error_open_wide_share_hash_init:
#endif
  spider_free_mem_calc(NULL,
    spider_init_error_tables_id,
    spider_init_error_tables.array.max_element *
    spider_init_error_tables.array.size_of_element);
  my_hash_free(&spider_init_error_tables);
error_init_error_tables_hash_init:
  spider_free_mem_calc(NULL,
    spider_open_tables_id,
    spider_open_tables.array.max_element *
    spider_open_tables.array.size_of_element);
  my_hash_free(&spider_open_tables);
error_open_tables_hash_init:
  pthread_mutex_destroy(&spider_mem_calc_mutex);
error_mem_calc_mutex_init:
  pthread_mutex_destroy(&spider_mon_table_cache_mutex);
error_mon_table_cache_mutex_init:
  pthread_mutex_destroy(&spider_allocated_thds_mutex);
error_allocated_thds_mutex_init:
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  pthread_mutex_destroy(&spider_hs_w_conn_mutex);
error_hs_w_conn_mutex_init:
  pthread_mutex_destroy(&spider_hs_r_conn_mutex);
error_hs_r_conn_mutex_init:
#endif
  pthread_mutex_destroy(&spider_open_conn_mutex);
error_open_conn_mutex_init:
  pthread_mutex_destroy(&spider_conn_mutex);
error_conn_mutex_init:
  pthread_mutex_destroy(&spider_lgtm_tblhnd_share_mutex);
error_lgtm_tblhnd_share_mutex_init:
#ifdef WITH_PARTITION_STORAGE_ENGINE
  pthread_mutex_destroy(&spider_wide_share_mutex);
error_wide_share_mutex_init:
#endif
  pthread_mutex_destroy(&spider_init_error_tbl_mutex);
error_init_error_tbl_mutex_init:
  pthread_mutex_destroy(&spider_ipport_conn_mutex);
error_ipport_count_mutex_init:
  pthread_mutex_destroy(&spider_conn_id_mutex);
error_conn_id_mutex_init:
  pthread_mutex_destroy(&spider_thread_id_mutex);
error_thread_id_mutex_init:
  pthread_mutex_destroy(&spider_tbl_mutex);
error_tbl_mutex_init:
#ifndef WITHOUT_SPIDER_BG_SEARCH
/*
error_pt_attr_setstate:
*/
  pthread_attr_destroy(&spider_pt_attr);
error_pt_attr_init:
#endif
  DBUG_RETURN(error_num);
}

char *spider_create_string(
  const char *str,
  uint length
) {
  char *res;
  DBUG_ENTER("spider_create_string");
  if (!(res = (char*) spider_malloc(spider_current_trx, 13, length + 1,
    MYF(MY_WME))))
    DBUG_RETURN(NULL);
  memcpy(res, str, length);
  res[length] = '\0';
  DBUG_RETURN(res);
}

char *spider_create_table_name_string(
  const char *table_name,
  const char *part_name,
  const char *sub_name
) {
  char *res, *tmp;
  uint length = strlen(table_name);
  DBUG_ENTER("spider_create_table_name_string");
  if (part_name)
  {
    length += sizeof("#P#") - 1 + strlen(part_name);
    if (sub_name)
      length += sizeof("#SP#") - 1 + strlen(sub_name);
  }
  if (!(res = (char*) spider_malloc(spider_current_trx, 14, length + 1,
    MYF(MY_WME))))
    DBUG_RETURN(NULL);
  tmp = strmov(res, table_name);
  if (part_name)
  {
    tmp = strmov(tmp, "#P#");
    tmp = strmov(tmp, part_name);
    if (sub_name)
    {
      tmp = strmov(tmp, "#SP#");
      tmp = strmov(tmp, sub_name);
    }
  }
  DBUG_RETURN(res);
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
void spider_get_partition_info(
  const char *table_name,
  uint table_name_length,
  const TABLE_SHARE *table_share,
  partition_info *part_info,
  partition_element **part_elem,
  partition_element **sub_elem
) {
  char tmp_name[FN_REFLEN + 1];
  partition_element *tmp_part_elem = NULL, *tmp_sub_elem = NULL;
  bool tmp_flg = FALSE, tmp_find_flg = FALSE;
  DBUG_ENTER("spider_get_partition_info");
  *part_elem = NULL;
  *sub_elem = NULL;
  if (!part_info)
    DBUG_VOID_RETURN;

  if (!memcmp(table_name + table_name_length - 5, "#TMP#", 5))
    tmp_flg = TRUE;

  DBUG_PRINT("info",("spider table_name=%s", table_name));
  List_iterator<partition_element> part_it(part_info->partitions);
  while ((*part_elem = part_it++))
  {
    if ((*part_elem)->subpartitions.elements)
    {
      List_iterator<partition_element> sub_it((*part_elem)->subpartitions);
      while ((*sub_elem = sub_it++))
      {
        if (SPIDER_create_subpartition_name(
          tmp_name, FN_REFLEN + 1, table_share->path.str,
          (*part_elem)->partition_name, (*sub_elem)->partition_name,
          NORMAL_PART_NAME))
        {
          DBUG_VOID_RETURN;
        }
        DBUG_PRINT("info",("spider tmp_name=%s", tmp_name));
        if (!memcmp(table_name, tmp_name, table_name_length + 1))
          DBUG_VOID_RETURN;
        if (
          tmp_flg &&
          *(tmp_name + table_name_length - 5) == '\0' &&
          !memcmp(table_name, tmp_name, table_name_length - 5)
        ) {
          tmp_part_elem = *part_elem;
          tmp_sub_elem = *sub_elem;
          tmp_flg = FALSE;
          tmp_find_flg = TRUE;
        }
      }
    } else {
      if (SPIDER_create_partition_name(
        tmp_name, FN_REFLEN + 1, table_share->path.str,
        (*part_elem)->partition_name, NORMAL_PART_NAME, TRUE))
      {
        DBUG_VOID_RETURN;
      }
      DBUG_PRINT("info",("spider tmp_name=%s", tmp_name));
      if (!memcmp(table_name, tmp_name, table_name_length + 1))
        DBUG_VOID_RETURN;
      if (
        tmp_flg &&
        *(tmp_name + table_name_length - 5) == '\0' &&
        !memcmp(table_name, tmp_name, table_name_length - 5)
      ) {
        tmp_part_elem = *part_elem;
        tmp_flg = FALSE;
        tmp_find_flg = TRUE;
      }
    }
  }
  if (tmp_find_flg)
  {
    *part_elem = tmp_part_elem;
    *sub_elem = tmp_sub_elem;
    DBUG_PRINT("info",("spider tmp find"));
    DBUG_VOID_RETURN;
  }
  *part_elem = NULL;
  *sub_elem = NULL;
  DBUG_PRINT("info",("spider no hit"));
  DBUG_VOID_RETURN;
}
#endif

int spider_get_sts(
  SPIDER_SHARE *share,
  int link_idx,
  time_t tmp_time,
  ha_spider *spider,
  double sts_interval,
  int sts_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int sts_sync,
#endif
  int sts_sync_level,
  uint flag
) {
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int get_type;
#endif
  int error_num = 0;
  bool need_to_get = TRUE;
  DBUG_ENTER("spider_get_sts");

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (
    sts_sync == 0
  ) {
    /* get */
    get_type = 1;
  } else if (
    !share->wide_share->sts_init
  ) {
    pthread_mutex_lock(&share->wide_share->sts_mutex);
    if (!share->wide_share->sts_init)
    {
      /* get after mutex_lock */
      get_type = 2;
    } else {
      pthread_mutex_unlock(&share->wide_share->sts_mutex);
      /* copy */
      get_type = 0;
    }
  } else if (
    difftime(share->sts_get_time, share->wide_share->sts_get_time) <
      sts_interval
  ) {
    /* copy */
    get_type = 0;
  } else if (
    !pthread_mutex_trylock(&share->wide_share->sts_mutex)
  ) {
    /* get after mutex_trylock */
    get_type = 3;
  } else {
    /* copy */
    get_type = 0;
  }
#endif
  if (
    !share->sts_init &&
    share->table_share->tmp_table == NO_TMP_TABLE &&
    spider_param_load_sts_at_startup(share->load_sts_at_startup) &&
    (!share->init || share->init_error)
  ) {
    error_num = spider_sys_get_table_sts(
      current_thd,
      share->lgtm_tblhnd_share->table_name,
      share->lgtm_tblhnd_share->table_name_length,
      &share->stat,
      FALSE
    );
    if (
      !error_num ||
      (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    )
    need_to_get = FALSE;
  }

  if (need_to_get)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (get_type == 0)
      spider_copy_sts_to_share(share, share->wide_share);
    else {
#endif
      error_num = spider_db_show_table_status(spider, link_idx, sts_mode, flag);
#ifdef WITH_PARTITION_STORAGE_ENGINE
    }
#endif
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (get_type >= 2)
    pthread_mutex_unlock(&share->wide_share->sts_mutex);
#endif
  if (error_num)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    SPIDER_PARTITION_HANDLER *partition_handler =
      spider->partition_handler;
    if (
      !share->wide_share->sts_init &&
      sts_sync >= sts_sync_level &&
      get_type > 1 &&
      partition_handler &&
      partition_handler->handlers &&
      partition_handler->handlers[0] == spider
    ) {
      int roop_count;
      ha_spider *tmp_spider;
      SPIDER_SHARE *tmp_share;
      double tmp_sts_interval;
      int tmp_sts_mode;
      int tmp_sts_sync;
      THD *thd = spider->wide_handler->trx->thd;
      for (roop_count = 1;
        roop_count < (int) partition_handler->no_parts;
        roop_count++)
      {
        tmp_spider =
          (ha_spider *) partition_handler->handlers[roop_count];
        tmp_share = tmp_spider->share;
        tmp_sts_interval = spider_param_sts_interval(thd, share->sts_interval);
        tmp_sts_mode = spider_param_sts_mode(thd, share->sts_mode);
        tmp_sts_sync = spider_param_sts_sync(thd, share->sts_sync);
        spider_get_sts(tmp_share, tmp_spider->search_link_idx,
          tmp_time, tmp_spider, tmp_sts_interval, tmp_sts_mode, tmp_sts_sync,
          1, flag);
        if (share->wide_share->sts_init)
        {
          error_num = 0;
          thd->clear_error();
          get_type = 0;
          spider_copy_sts_to_share(share, share->wide_share);
          break;
        }
      }
    }
    if (error_num)
#endif
      DBUG_RETURN(error_num);
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (sts_sync >= sts_sync_level && get_type > 0)
  {
    spider_copy_sts_to_wide_share(share->wide_share, share);
    share->wide_share->sts_get_time = tmp_time;
    share->wide_share->sts_init = TRUE;
  }
#endif
  share->sts_get_time = tmp_time;
  share->sts_init = TRUE;
  DBUG_RETURN(0);
}

int spider_get_crd(
  SPIDER_SHARE *share,
  int link_idx,
  time_t tmp_time,
  ha_spider *spider,
  TABLE *table,
  double crd_interval,
  int crd_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int crd_sync,
#endif
  int crd_sync_level
) {
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int get_type;
#endif
  int error_num = 0;
  bool need_to_get = TRUE;
  DBUG_ENTER("spider_get_crd");

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (
    crd_sync == 0
  ) {
    /* get */
    get_type = 1;
  } else if (
    !share->wide_share->crd_init
  ) {
    pthread_mutex_lock(&share->wide_share->crd_mutex);
    if (!share->wide_share->crd_init)
    {
      /* get after mutex_lock */
      get_type = 2;
    } else {
      pthread_mutex_unlock(&share->wide_share->crd_mutex);
      /* copy */
      get_type = 0;
    }
  } else if (
    difftime(share->crd_get_time, share->wide_share->crd_get_time) <
      crd_interval
  ) {
    /* copy */
    get_type = 0;
  } else if (
    !pthread_mutex_trylock(&share->wide_share->crd_mutex)
  ) {
    /* get after mutex_trylock */
    get_type = 3;
  } else {
    /* copy */
    get_type = 0;
  }
#endif
  if (
    !share->crd_init &&
    share->table_share->tmp_table == NO_TMP_TABLE &&
    spider_param_load_sts_at_startup(share->load_crd_at_startup)
  ) {
    error_num = spider_sys_get_table_crd(
      current_thd,
      share->lgtm_tblhnd_share->table_name,
      share->lgtm_tblhnd_share->table_name_length,
      share->cardinality,
      table->s->fields,
      FALSE
    );
    if (
      !error_num ||
      (error_num != HA_ERR_KEY_NOT_FOUND && error_num != HA_ERR_END_OF_FILE)
    )
    need_to_get = FALSE;
  }

  if (need_to_get)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (get_type == 0)
      spider_copy_crd_to_share(share, share->wide_share,
        table->s->fields);
    else {
#endif
      error_num = spider_db_show_index(spider, link_idx, table, crd_mode);
#ifdef WITH_PARTITION_STORAGE_ENGINE
    }
#endif
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (get_type >= 2)
    pthread_mutex_unlock(&share->wide_share->crd_mutex);
#endif
  if (error_num)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    SPIDER_PARTITION_HANDLER *partition_handler =
      spider->partition_handler;
    if (
      !share->wide_share->crd_init &&
      crd_sync >= crd_sync_level &&
      get_type > 1 &&
      partition_handler &&
      partition_handler->handlers &&
      partition_handler->handlers[0] == spider
    ) {
      int roop_count;
      ha_spider *tmp_spider;
      SPIDER_SHARE *tmp_share;
      double tmp_crd_interval;
      int tmp_crd_mode;
      int tmp_crd_sync;
      THD *thd = spider->wide_handler->trx->thd;
      for (roop_count = 1;
        roop_count < (int) partition_handler->no_parts;
        roop_count++)
      {
        tmp_spider =
          (ha_spider *) partition_handler->handlers[roop_count];
        tmp_share = tmp_spider->share;
        tmp_crd_interval = spider_param_crd_interval(thd, share->crd_interval);
        tmp_crd_mode = spider_param_crd_mode(thd, share->crd_mode);
        tmp_crd_sync = spider_param_crd_sync(thd, share->crd_sync);
        spider_get_crd(tmp_share, tmp_spider->search_link_idx,
          tmp_time, tmp_spider, table, tmp_crd_interval, tmp_crd_mode,
          tmp_crd_sync, 1);
        if (share->wide_share->crd_init)
        {
          error_num = 0;
          thd->clear_error();
          get_type = 0;
          spider_copy_crd_to_share(share, share->wide_share,
            table->s->fields);
          break;
        }
      }
    }
    if (error_num)
#endif
      DBUG_RETURN(error_num);
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (crd_sync >= crd_sync_level && get_type > 0)
  {
    spider_copy_crd_to_wide_share(share->wide_share, share,
      table->s->fields);
    share->wide_share->crd_get_time = tmp_time;
    share->wide_share->crd_init = TRUE;
  }
#endif
  share->crd_get_time = tmp_time;
  share->crd_init = TRUE;
  DBUG_RETURN(0);
}

void spider_set_result_list_param(
  ha_spider *spider
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_SHARE *share = spider->share;
  THD *thd = spider->wide_handler->trx->thd;
  DBUG_ENTER("spider_set_result_list_param");
  result_list->internal_offset =
    spider_param_internal_offset(thd, share->internal_offset);
  result_list->internal_limit =
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
    spider->wide_handler->info_limit < 9223372036854775807LL ?
    spider->wide_handler->info_limit :
#endif
    spider_param_internal_limit(thd, share->internal_limit);
  result_list->split_read = spider_split_read_param(spider);
  if (spider->support_multi_split_read_sql())
  {
    result_list->multi_split_read =
      spider_param_multi_split_read(thd, share->multi_split_read);
  } else {
    result_list->multi_split_read = 1;
  }
  result_list->max_order =
    spider_param_max_order(thd, share->max_order);
  result_list->quick_mode =
    spider_param_quick_mode(thd, share->quick_mode);
  result_list->quick_page_size =
    spider_param_quick_page_size(thd, share->quick_page_size);
  result_list->quick_page_byte =
    spider_param_quick_page_byte(thd, share->quick_page_byte);
  result_list->low_mem_read =
    spider_param_low_mem_read(thd, share->low_mem_read);
  DBUG_VOID_RETURN;
}

SPIDER_INIT_ERROR_TABLE *spider_get_init_error_table(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  bool create
) {
  SPIDER_INIT_ERROR_TABLE *spider_init_error_table;
  char *tmp_name;
  DBUG_ENTER("spider_get_init_error_table");
  pthread_mutex_lock(&spider_init_error_tbl_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(spider_init_error_table = (SPIDER_INIT_ERROR_TABLE *)
    my_hash_search_using_hash_value(
    &spider_init_error_tables, share->table_name_hash_value,
    (uchar*) share->table_name, share->table_name_length)))
#else
  if (!(spider_init_error_table = (SPIDER_INIT_ERROR_TABLE *) my_hash_search(
    &spider_init_error_tables,
    (uchar*) share->table_name, share->table_name_length)))
#endif
  {
    if (!create)
    {
      pthread_mutex_unlock(&spider_init_error_tbl_mutex);
      DBUG_RETURN(NULL);
    }
    if (!(spider_init_error_table = (SPIDER_INIT_ERROR_TABLE *)
      spider_bulk_malloc(spider_current_trx, 54, MYF(MY_WME | MY_ZEROFILL),
        &spider_init_error_table, (uint) (sizeof(*spider_init_error_table)),
        &tmp_name, (uint) (share->table_name_length + 1),
        NullS))
    ) {
      pthread_mutex_unlock(&spider_init_error_tbl_mutex);
      DBUG_RETURN(NULL);
    }
    memcpy(tmp_name, share->table_name, share->table_name_length);
    spider_init_error_table->table_name = tmp_name;
    spider_init_error_table->table_name_length = share->table_name_length;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    spider_init_error_table->table_name_hash_value =
      share->table_name_hash_value;
#endif
    uint old_elements = spider_init_error_tables.array.max_element;
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    if (my_hash_insert_with_hash_value(&spider_init_error_tables,
      share->table_name_hash_value, (uchar*) spider_init_error_table))
#else
    if (my_hash_insert(&spider_init_error_tables,
      (uchar*) spider_init_error_table))
#endif
    {
      spider_free(trx, spider_init_error_table, MYF(0));
      pthread_mutex_unlock(&spider_init_error_tbl_mutex);
      DBUG_RETURN(NULL);
    }
    if (spider_init_error_tables.array.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        spider_init_error_tables,
        (spider_init_error_tables.array.max_element - old_elements) *
        spider_init_error_tables.array.size_of_element);
    }
  }
  pthread_mutex_unlock(&spider_init_error_tbl_mutex);
  DBUG_RETURN(spider_init_error_table);
}

void spider_delete_init_error_table(
  const char *name
) {
  SPIDER_INIT_ERROR_TABLE *spider_init_error_table;
  uint length = strlen(name);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) name, length);
#endif
  DBUG_ENTER("spider_delete_init_error_table");
  pthread_mutex_lock(&spider_init_error_tbl_mutex);
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if ((spider_init_error_table = (SPIDER_INIT_ERROR_TABLE *)
    my_hash_search_using_hash_value(&spider_init_error_tables, hash_value,
      (uchar*) name, length)))
#else
  if ((spider_init_error_table = (SPIDER_INIT_ERROR_TABLE *) my_hash_search(
    &spider_init_error_tables, (uchar*) name, length)))
#endif
  {
#ifdef HASH_UPDATE_WITH_HASH_VALUE
    my_hash_delete_with_hash_value(&spider_init_error_tables,
      spider_init_error_table->table_name_hash_value,
      (uchar*) spider_init_error_table);
#else
    my_hash_delete(&spider_init_error_tables,
      (uchar*) spider_init_error_table);
#endif
    spider_free(spider_current_trx, spider_init_error_table, MYF(0));
  }
  pthread_mutex_unlock(&spider_init_error_tbl_mutex);
  DBUG_VOID_RETURN;
}

bool spider_check_pk_update(
  TABLE *table
) {
  int roop_count;
  TABLE_SHARE *table_share = table->s;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  DBUG_ENTER("spider_check_pk_update");
  if (table_share->primary_key == MAX_KEY)
    DBUG_RETURN(FALSE);

  key_info = &table_share->key_info[table_share->primary_key];
  key_part = key_info->key_part;
  for (roop_count = 0;
    roop_count < (int) spider_user_defined_key_parts(key_info); roop_count++)
  {
    if (bitmap_is_set(table->write_set,
      key_part[roop_count].field->field_index))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
bool spider_check_hs_pk_update(
  ha_spider *spider,
  key_range *key
) {
  uint roop_count, field_index, set_count = 0;
  TABLE *table = spider->get_table();
  TABLE_SHARE *table_share = table->s;
  SPIDER_SHARE *share = spider->share;
  KEY *key_info;
  KEY_PART_INFO *key_part;
  char buf[MAX_FIELD_WIDTH], buf2[MAX_FIELD_WIDTH];
  spider_string tmp_str(buf, MAX_FIELD_WIDTH, &my_charset_bin),
    tmp_str2(buf2, MAX_FIELD_WIDTH, &my_charset_bin);
  String *str, *str2;
  DBUG_ENTER("spider_check_hs_pk_update");
  tmp_str.init_calc_mem(137);

  if (table_share->primary_key == MAX_KEY)
    DBUG_RETURN(FALSE);
  memset(spider->tmp_column_bitmap, 0, sizeof(uchar) * share->bitmap_size);
  key_info = &table->key_info[table_share->primary_key];
  key_part = key_info->key_part;
  for (roop_count = 0; roop_count < spider_user_defined_key_parts(key_info);
    roop_count++)
  {
    field_index = key_part[roop_count].field->field_index;
    if (bitmap_is_set(table->write_set, field_index))
    {
      DBUG_PRINT("info", ("spider set key_part=%u field_index=%u",
        roop_count, field_index));
      spider_set_bit(spider->tmp_column_bitmap, field_index);
      set_count++;
    }
  }
  DBUG_PRINT("info", ("spider set_count=%u", set_count));

  Field *field;
  uint store_length, length, var_len;
  const uchar *ptr;
  bool key_eq;
  key_part_map tgt_key_part_map = key->keypart_map;
  key_info = &table->key_info[spider->active_index];
  for (
    key_part = key_info->key_part,
    length = 0;
    tgt_key_part_map;
    length += store_length,
    tgt_key_part_map >>= 1,
    key_part++
  ) {
    store_length = key_part->store_length;
    field = key_part->field;
    field_index = field->field_index;
    if (spider_bit_is_set(spider->tmp_column_bitmap, field_index))
    {
      ptr = key->key + length;
      key_eq = (tgt_key_part_map > 1);
      if (key_part->null_bit && *ptr++)
      {
        if (key->flag != HA_READ_KEY_EXACT || !field->is_null())
        {
          DBUG_PRINT("info", ("spider flag=%u is_null=%s",
            key->flag, field->is_null() ? "TRUE" : "FALSE"));
          DBUG_RETURN(TRUE);
        }
      } else {
        if (
          field->type() == MYSQL_TYPE_BLOB ||
          field->real_type() == MYSQL_TYPE_VARCHAR ||
          field->type() == MYSQL_TYPE_GEOMETRY
        ) {
          var_len = uint2korr(ptr);
          tmp_str.set((char *) ptr + HA_KEY_BLOB_LENGTH, var_len,
            &my_charset_bin);
          str = tmp_str.get_str();
        } else {
          str = field->val_str(tmp_str.get_str(), ptr);
          tmp_str.mem_calc();
        }
        str2 = field->val_str(tmp_str2.get_str());
        tmp_str2.mem_calc();
        if (
          str->length() != str2->length() ||
          memcmp(str->ptr(), str2->ptr(), str->length())
        ) {
          DBUG_PRINT("info", ("spider length=%u %u",
            str->length(), str2->length()));
          DBUG_PRINT("info", ("spider length=%s %s",
            str->c_ptr_safe(), str2->c_ptr_safe()));
          DBUG_RETURN(TRUE);
        }
      }
      set_count--;
    }
  }
  DBUG_PRINT("info", ("spider set_count=%u", set_count));
  if (set_count)
  {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}
#endif
#endif

void spider_set_tmp_share_pointer(
  SPIDER_SHARE *tmp_share,
  char **tmp_connect_info,
  uint *tmp_connect_info_length,
  long *tmp_long,
  longlong *tmp_longlong
) {
  DBUG_ENTER("spider_set_tmp_share_pointer");
  tmp_share->link_count = 1;
  tmp_share->all_link_count = 1;
  tmp_share->server_names = &tmp_connect_info[0];
  tmp_share->tgt_table_names = &tmp_connect_info[1];
  tmp_share->tgt_dbs = &tmp_connect_info[2];
  tmp_share->tgt_hosts = &tmp_connect_info[3];
  tmp_share->tgt_usernames = &tmp_connect_info[4];
  tmp_share->tgt_passwords = &tmp_connect_info[5];
  tmp_share->tgt_sockets = &tmp_connect_info[6];
  tmp_share->tgt_wrappers = &tmp_connect_info[7];
  tmp_share->tgt_ssl_cas = &tmp_connect_info[8];
  tmp_share->tgt_ssl_capaths = &tmp_connect_info[9];
  tmp_share->tgt_ssl_certs = &tmp_connect_info[10];
  tmp_share->tgt_ssl_ciphers = &tmp_connect_info[11];
  tmp_share->tgt_ssl_keys = &tmp_connect_info[12];
  tmp_share->tgt_default_files = &tmp_connect_info[13];
  tmp_share->tgt_default_groups = &tmp_connect_info[14];
  tmp_share->tgt_dsns = &tmp_connect_info[15];
  tmp_share->tgt_filedsns = &tmp_connect_info[16];
  tmp_share->tgt_drivers = &tmp_connect_info[17];
  tmp_share->tgt_pk_names = &tmp_connect_info[18];
  tmp_share->tgt_sequence_names = &tmp_connect_info[19];
  tmp_share->static_link_ids = &tmp_connect_info[20];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  tmp_share->hs_read_socks = &tmp_connect_info[21];
  tmp_share->hs_write_socks = &tmp_connect_info[22];
#endif
  tmp_share->tgt_ports = &tmp_long[0];
  tmp_share->tgt_ssl_vscs = &tmp_long[1];
  tmp_share->link_statuses = &tmp_long[2];
  tmp_share->monitoring_binlog_pos_at_failing = &tmp_long[3];
  tmp_share->monitoring_flag = &tmp_long[4];
  tmp_share->monitoring_kind = &tmp_long[5];
#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp_share->monitoring_bg_flag = &tmp_long[6];
  tmp_share->monitoring_bg_kind = &tmp_long[7];
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  tmp_share->use_hs_reads = &tmp_long[8];
  tmp_share->use_hs_writes = &tmp_long[9];
  tmp_share->hs_read_ports = &tmp_long[10];
  tmp_share->hs_write_ports = &tmp_long[11];
  tmp_share->hs_write_to_reads = &tmp_long[12];
#endif
  tmp_share->use_handlers = &tmp_long[13];
  tmp_share->connect_timeouts = &tmp_long[14];
  tmp_long[13] = -1;
  tmp_share->net_read_timeouts = &tmp_long[15];
  tmp_long[14] = -1;
  tmp_share->net_write_timeouts = &tmp_long[16];
  tmp_long[15] = -1;
  tmp_share->access_balances = &tmp_long[17];
  tmp_share->bka_table_name_types = &tmp_long[18];
  tmp_share->strict_group_bys = &tmp_long[19];
  tmp_share->monitoring_limit = &tmp_longlong[0];
  tmp_share->monitoring_sid = &tmp_longlong[1];
#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp_share->monitoring_bg_interval = &tmp_longlong[2];
#endif
  tmp_share->server_names_lengths = &tmp_connect_info_length[0];
  tmp_share->tgt_table_names_lengths = &tmp_connect_info_length[1];
  tmp_share->tgt_dbs_lengths = &tmp_connect_info_length[2];
  tmp_share->tgt_hosts_lengths = &tmp_connect_info_length[3];
  tmp_share->tgt_usernames_lengths = &tmp_connect_info_length[4];
  tmp_share->tgt_passwords_lengths = &tmp_connect_info_length[5];
  tmp_share->tgt_sockets_lengths = &tmp_connect_info_length[6];
  tmp_share->tgt_wrappers_lengths = &tmp_connect_info_length[7];
  tmp_share->tgt_ssl_cas_lengths = &tmp_connect_info_length[8];
  tmp_share->tgt_ssl_capaths_lengths = &tmp_connect_info_length[9];
  tmp_share->tgt_ssl_certs_lengths = &tmp_connect_info_length[10];
  tmp_share->tgt_ssl_ciphers_lengths = &tmp_connect_info_length[11];
  tmp_share->tgt_ssl_keys_lengths = &tmp_connect_info_length[12];
  tmp_share->tgt_default_files_lengths = &tmp_connect_info_length[13];
  tmp_share->tgt_default_groups_lengths = &tmp_connect_info_length[14];
  tmp_share->tgt_dsns_lengths = &tmp_connect_info_length[15];
  tmp_share->tgt_filedsns_lengths = &tmp_connect_info_length[16];
  tmp_share->tgt_drivers_lengths = &tmp_connect_info_length[17];
  tmp_share->tgt_pk_names_lengths = &tmp_connect_info_length[18];
  tmp_share->tgt_sequence_names_lengths = &tmp_connect_info_length[19];
  tmp_share->static_link_ids_lengths = &tmp_connect_info_length[20];
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  tmp_share->hs_read_socks_lengths = &tmp_connect_info_length[21];
  tmp_share->hs_write_socks_lengths = &tmp_connect_info_length[22];
#endif
  tmp_share->server_names_length = 1;
  tmp_share->tgt_table_names_length = 1;
  tmp_share->tgt_dbs_length = 1;
  tmp_share->tgt_hosts_length = 1;
  tmp_share->tgt_usernames_length = 1;
  tmp_share->tgt_passwords_length = 1;
  tmp_share->tgt_sockets_length = 1;
  tmp_share->tgt_wrappers_length = 1;
  tmp_share->tgt_ssl_cas_length = 1;
  tmp_share->tgt_ssl_capaths_length = 1;
  tmp_share->tgt_ssl_certs_length = 1;
  tmp_share->tgt_ssl_ciphers_length = 1;
  tmp_share->tgt_ssl_keys_length = 1;
  tmp_share->tgt_default_files_length = 1;
  tmp_share->tgt_default_groups_length = 1;
  tmp_share->tgt_dsns_length = 1;
  tmp_share->tgt_filedsns_length = 1;
  tmp_share->tgt_drivers_length = 1;
  tmp_share->tgt_pk_names_length = 1;
  tmp_share->tgt_sequence_names_length = 1;
  tmp_share->static_link_ids_length = 1;
  tmp_share->tgt_ports_length = 1;
  tmp_share->tgt_ssl_vscs_length = 1;
  tmp_share->link_statuses_length = 1;
  tmp_share->monitoring_binlog_pos_at_failing_length = 1;
  tmp_share->monitoring_flag_length = 1;
  tmp_share->monitoring_kind_length = 1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp_share->monitoring_bg_flag_length = 1;
  tmp_share->monitoring_bg_kind_length = 1;
#endif
  tmp_share->monitoring_limit_length = 1;
  tmp_share->monitoring_sid_length = 1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp_share->monitoring_bg_interval_length = 1;
#endif
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  tmp_share->hs_read_socks_length = 1;
  tmp_share->hs_write_socks_length = 1;
  tmp_share->use_hs_reads_length = 1;
  tmp_share->use_hs_writes_length = 1;
  tmp_share->hs_read_ports_length = 1;
  tmp_share->hs_write_ports_length = 1;
  tmp_share->hs_write_to_reads_length = 1;
#endif
  tmp_share->use_handlers_length = 1;
  tmp_share->connect_timeouts_length = 1;
  tmp_share->net_read_timeouts_length = 1;
  tmp_share->net_write_timeouts_length = 1;
  tmp_share->access_balances_length = 1;
  tmp_share->bka_table_name_types_length = 1;
  tmp_share->strict_group_bys_length = 1;

#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp_share->monitoring_bg_flag[0] = -1;
  tmp_share->monitoring_bg_kind[0] = -1;
#endif
  tmp_share->monitoring_binlog_pos_at_failing[0] = -1;
  tmp_share->monitoring_flag[0] = -1;
  tmp_share->monitoring_kind[0] = -1;
#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp_share->monitoring_bg_interval[0] = -1;
#endif
  tmp_share->monitoring_limit[0] = -1;
  tmp_share->monitoring_sid[0] = -1;
  tmp_share->bka_engine = NULL;
  tmp_share->use_dbton_count = 0;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  tmp_share->use_hs_dbton_count = 0;
#endif
  DBUG_VOID_RETURN;
}

int spider_create_tmp_dbton_share(
  SPIDER_SHARE *tmp_share
) {
  int error_num;
  uint dbton_id = tmp_share->use_dbton_ids[0];
  DBUG_ENTER("spider_create_tmp_dbton_share");
  if (!(tmp_share->dbton_share[dbton_id] =
    spider_dbton[dbton_id].create_db_share(tmp_share)))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if ((error_num = tmp_share->dbton_share[dbton_id]->init()))
  {
    delete tmp_share->dbton_share[dbton_id];
    tmp_share->dbton_share[dbton_id] = NULL;
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

void spider_free_tmp_dbton_share(
  SPIDER_SHARE *tmp_share
) {
  uint dbton_id = tmp_share->use_dbton_ids[0];
  DBUG_ENTER("spider_free_tmp_dbton_share");
  if (tmp_share->dbton_share[dbton_id])
  {
    delete tmp_share->dbton_share[dbton_id];
    tmp_share->dbton_share[dbton_id] = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_create_tmp_dbton_handler(
  ha_spider *tmp_spider
) {
  int error_num;
  SPIDER_SHARE *tmp_share = tmp_spider->share;
  uint dbton_id = tmp_share->use_dbton_ids[0];
  DBUG_ENTER("spider_create_tmp_dbton_handler");
  if (!(tmp_spider->dbton_handler[dbton_id] =
    spider_dbton[dbton_id].create_db_handler(tmp_spider,
    tmp_share->dbton_share[dbton_id])))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if ((error_num = tmp_spider->dbton_handler[dbton_id]->init()))
  {
    delete tmp_spider->dbton_handler[dbton_id];
    tmp_spider->dbton_handler[dbton_id] = NULL;
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

void spider_free_tmp_dbton_handler(
  ha_spider *tmp_spider
) {
  SPIDER_SHARE *tmp_share = tmp_spider->share;
  uint dbton_id = tmp_share->use_dbton_ids[0];
  DBUG_ENTER("spider_create_tmp_dbton_handler");
  if (tmp_spider->dbton_handler[dbton_id])
  {
    delete tmp_spider->dbton_handler[dbton_id];
    tmp_spider->dbton_handler[dbton_id] = NULL;
  }
  DBUG_VOID_RETURN;
}

TABLE_LIST *spider_get_parent_table_list(
  ha_spider *spider
) {
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_get_parent_table_list");
  DBUG_RETURN(table->pos_in_table_list);
}

List<Index_hint> *spider_get_index_hints(
  ha_spider *spider
  ) {
    TABLE_LIST *table_list = spider_get_parent_table_list(spider);
    DBUG_ENTER("spider_get_index_hint");
    if (table_list)
    {
      DBUG_RETURN(table_list->index_hints);
    }
    DBUG_RETURN(NULL);
}


st_select_lex *spider_get_select_lex(
  ha_spider *spider
) {
  TABLE_LIST *table_list = spider_get_parent_table_list(spider);
  DBUG_ENTER("spider_get_select_lex");
  if (table_list)
  {
    DBUG_RETURN(table_list->select_lex);
  }
  DBUG_RETURN(NULL);
}

void spider_get_select_limit_from_select_lex(
  st_select_lex *select_lex,
  longlong *select_limit,
  longlong *offset_limit
) {
  DBUG_ENTER("spider_get_select_limit_from_select_lex");
  *select_limit = 9223372036854775807LL;
  *offset_limit = 0;
  if (select_lex && select_lex->limit_params.explicit_limit)
  {
    *select_limit = select_lex->limit_params.select_limit ?
      select_lex->limit_params.select_limit->val_int() : 0;
    *offset_limit = select_lex->limit_params.offset_limit ?
      select_lex->limit_params.offset_limit->val_int() : 0;
  }
  DBUG_VOID_RETURN;
}

void spider_get_select_limit(
  ha_spider *spider,
  st_select_lex **select_lex,
  longlong *select_limit,
  longlong *offset_limit
) {
  DBUG_ENTER("spider_get_select_limit");
  *select_lex = spider_get_select_lex(spider);
  spider_get_select_limit_from_select_lex(
    *select_lex, select_limit, offset_limit);
  DBUG_VOID_RETURN;
}

longlong spider_split_read_param(
  ha_spider *spider
) {
  SPIDER_SHARE *share = spider->share;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  THD *thd = spider->wide_handler->trx->thd;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  double semi_split_read;
  longlong split_read;
  DBUG_ENTER("spider_split_read_param");
  result_list->set_split_read_count = 1;
#ifdef INFO_KIND_FORCE_LIMIT_BEGIN
  if (spider->wide_handler->info_limit < 9223372036854775807LL)
  {
    DBUG_PRINT("info",("spider info_limit=%lld",
      spider->wide_handler->info_limit));
    longlong info_limit = spider->wide_handler->info_limit;
    result_list->split_read_base = info_limit;
    result_list->semi_split_read = 0;
    result_list->first_read = info_limit;
    result_list->second_read = info_limit;
    result_list->semi_split_read_base = 0;
    result_list->set_split_read = FALSE;
    DBUG_RETURN(info_limit);
  }
#endif
  if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
  {
    DBUG_RETURN(result_list->semi_split_read_base);
  }
  spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);
  DBUG_PRINT("info",("spider result_list->set_split_read=%s", result_list->set_split_read ? "TRUE" : "FALSE"));
  if (!result_list->set_split_read)
  {
    int bulk_update_mode = spider_param_bulk_update_mode(thd,
      share->bulk_update_mode);
    DBUG_PRINT("info",("spider sql_command=%u",
      spider->wide_handler->sql_command));
    DBUG_PRINT("info",("spider bulk_update_mode=%d", bulk_update_mode));
    DBUG_PRINT("info",("spider support_bulk_update_sql=%s",
      spider->support_bulk_update_sql() ? "TRUE" : "FALSE"));
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
    bool inserting =
      (
#ifdef HS_HAS_SQLCOM
        spider->wide_handler->sql_command == SQLCOM_HS_INSERT ||
#endif
        spider->wide_handler->sql_command == SQLCOM_INSERT ||
        spider->wide_handler->sql_command == SQLCOM_INSERT_SELECT
      );
#endif
    bool updating =
      (
#ifdef HS_HAS_SQLCOM
        spider->wide_handler->sql_command == SQLCOM_HS_UPDATE ||
#endif
        spider->wide_handler->sql_command == SQLCOM_UPDATE ||
        spider->wide_handler->sql_command == SQLCOM_UPDATE_MULTI
      );
    bool deleting =
      (
#ifdef HS_HAS_SQLCOM
        spider->wide_handler->sql_command == SQLCOM_HS_DELETE ||
#endif
        spider->wide_handler->sql_command == SQLCOM_DELETE ||
        spider->wide_handler->sql_command == SQLCOM_DELETE_MULTI
      );
    bool replacing =
      (
        spider->wide_handler->sql_command == SQLCOM_REPLACE ||
        spider->wide_handler->sql_command == SQLCOM_REPLACE_SELECT
      );
    DBUG_PRINT("info",("spider updating=%s", updating ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider deleting=%s", deleting ? "TRUE" : "FALSE"));
    DBUG_PRINT("info",("spider replacing=%s", replacing ? "TRUE" : "FALSE"));
    TABLE *table = spider->get_table();
    if (
#ifdef SPIDER_HAS_GROUP_BY_HANDLER
      (
        inserting &&
        spider->use_fields
      ) ||
#endif
      replacing ||
      (
        (
          updating ||
          deleting
        ) &&
        (
          bulk_update_mode != 2 ||
          !spider->support_bulk_update_sql() ||
          (
            updating &&
            table->triggers &&
#ifdef HA_CAN_FORCE_BULK_UPDATE
            !(table->file->ha_table_flags() & HA_CAN_FORCE_BULK_UPDATE) &&
#endif
            table->triggers->has_triggers(TRG_EVENT_UPDATE, TRG_ACTION_AFTER)
          ) ||
          (
            deleting &&
            table->triggers &&
#ifdef HA_CAN_FORCE_BULK_DELETE
            !(table->file->ha_table_flags() & HA_CAN_FORCE_BULK_DELETE) &&
#endif
            table->triggers->has_triggers(TRG_EVENT_DELETE, TRG_ACTION_AFTER)
          )
        )
      )
    ) {
      /* This case must select by one shot */
      DBUG_PRINT("info",("spider cancel split read"));
      result_list->split_read_base = 9223372036854775807LL;
      result_list->semi_split_read = 0;
      result_list->semi_split_read_limit = 9223372036854775807LL;
      result_list->first_read = 9223372036854775807LL;
      result_list->second_read = 9223372036854775807LL;
      result_list->semi_split_read_base = 0;
      result_list->set_split_read = TRUE;
      DBUG_RETURN(9223372036854775807LL);
    }
#ifdef SPIDER_HAS_EXPLAIN_QUERY
    Explain_query *explain = thd->lex->explain;
    bool filesort = FALSE;
    if (explain)
    {
      DBUG_PRINT("info",("spider explain=%p", explain));
      Explain_select *explain_select = NULL;
      if (select_lex)
      {
        DBUG_PRINT("info",("spider select_lex=%p", select_lex));
        DBUG_PRINT("info",("spider select_number=%u",
          select_lex->select_number));
        explain_select =
          explain->get_select(select_lex->select_number);
      }
      if (explain_select)
      {
        DBUG_PRINT("info",("spider explain_select=%p", explain_select));
        if (explain_select->using_filesort)
        {
          DBUG_PRINT("info",("spider using filesort"));
          filesort = TRUE;
        }
      }
    }
#endif
    result_list->split_read_base =
      spider_param_split_read(thd, share->split_read);
#ifdef SPIDER_HAS_EXPLAIN_QUERY
    if (filesort)
    {
      result_list->semi_split_read = 0;
      result_list->semi_split_read_limit = 9223372036854775807LL;
    } else {
#endif
      result_list->semi_split_read =
        spider_param_semi_split_read(thd, share->semi_split_read);
      result_list->semi_split_read_limit =
        spider_param_semi_split_read_limit(thd, share->semi_split_read_limit);
#ifdef SPIDER_HAS_EXPLAIN_QUERY
    }
#endif
    result_list->first_read =
      spider_param_first_read(thd, share->first_read);
    result_list->second_read =
      spider_param_second_read(thd, share->second_read);
    result_list->semi_split_read_base = 0;
    result_list->set_split_read = TRUE;
  }
  DBUG_PRINT("info",("spider result_list->semi_split_read=%f", result_list->semi_split_read));
  DBUG_PRINT("info",("spider select_lex->explicit_limit=%d", select_lex ? select_lex->limit_params.explicit_limit : 0));
  DBUG_PRINT("info",("spider OPTION_FOUND_ROWS=%s", select_lex && (select_lex->options & OPTION_FOUND_ROWS) ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("spider select_lex->group_list.elements=%u", select_lex ? select_lex->group_list.elements : 0));
  DBUG_PRINT("info",("spider select_lex->with_sum_func=%s", select_lex && select_lex->with_sum_func ? "TRUE" : "FALSE"));
  if (
    result_list->semi_split_read > 0 &&
    select_lex && select_lex->limit_params.explicit_limit &&
    !(select_lex->options & OPTION_FOUND_ROWS) &&
    !select_lex->group_list.elements &&
    !select_lex->with_sum_func
  ) {
    semi_split_read = result_list->semi_split_read *
      (select_limit + offset_limit);
    DBUG_PRINT("info",("spider semi_split_read=%f", semi_split_read));
    if (semi_split_read >= result_list->semi_split_read_limit)
    {
      result_list->semi_split_read_base = result_list->semi_split_read_limit;
      DBUG_RETURN(result_list->semi_split_read_limit);
    } else {
      split_read = (longlong) semi_split_read;
      if (split_read < 0)
      {
        result_list->semi_split_read_base = result_list->semi_split_read_limit;
        DBUG_RETURN(result_list->semi_split_read_limit);
      } else if (split_read == 0)
      {
        result_list->semi_split_read_base = 1;
        DBUG_RETURN(1);
      } else {
        result_list->semi_split_read_base = split_read;
        DBUG_RETURN(split_read);
      }
    }
  } else if (result_list->first_read > 0)
    DBUG_RETURN(result_list->first_read);
  DBUG_RETURN(result_list->split_read_base);
}

longlong spider_bg_split_read_param(
  ha_spider *spider
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_bg_split_read_param");
  if (result_list->semi_split_read_base)
    DBUG_RETURN(result_list->semi_split_read_base);
  DBUG_RETURN(result_list->split_read_base);
}

void spider_first_split_read_param(
  ha_spider *spider
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_first_split_read_param");
  if (result_list->semi_split_read_base)
    result_list->split_read = result_list->semi_split_read_base;
  else if (result_list->second_read > 0)
    result_list->split_read = result_list->first_read;
  else
    result_list->split_read = result_list->split_read_base;
  result_list->set_split_read_count = 1;
  DBUG_VOID_RETURN;
}

void spider_next_split_read_param(
  ha_spider *spider
) {
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  DBUG_ENTER("spider_next_split_read_param");
  if (result_list->semi_split_read_base)
    result_list->split_read = result_list->semi_split_read_base;
  else if (
    result_list->set_split_read_count == 1 &&
    result_list->second_read > 0
  )
    result_list->split_read = result_list->second_read;
  else
    result_list->split_read = result_list->split_read_base;
  result_list->set_split_read_count++;
  DBUG_VOID_RETURN;
}

bool spider_check_direct_order_limit(
  ha_spider *spider
) {
  THD *thd = spider->wide_handler->trx->thd;
  SPIDER_SHARE *share = spider->share;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  DBUG_ENTER("spider_check_direct_order_limit");
  if (spider_check_index_merge(spider->get_table(),
    spider_get_select_lex(spider)))
  {
    DBUG_PRINT("info",("spider set use_index_merge"));
    spider->use_index_merge = TRUE;
  }
  DBUG_PRINT("info",("spider SQLCOM_HA_READ=%s",
    (spider->wide_handler->sql_command == SQLCOM_HA_READ) ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("spider sql_kinds with SPIDER_SQL_KIND_HANDLER=%s",
    (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER) ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("spider use_index_merge=%s",
    spider->use_index_merge ? "TRUE" : "FALSE"));
  DBUG_PRINT("info",("spider is_clone=%s",
    spider->is_clone ? "TRUE" : "FALSE"));
#ifdef HA_CAN_BULK_ACCESS
  DBUG_PRINT("info",("spider is_bulk_access_clone=%s",
    spider->is_bulk_access_clone ? "TRUE" : "FALSE"));
#endif
  if (
    spider->wide_handler->sql_command != SQLCOM_HA_READ &&
    !spider->use_index_merge &&
#ifdef HA_CAN_BULK_ACCESS
    (!spider->is_clone || spider->is_bulk_access_clone)
#else
    !spider->is_clone
#endif
  ) {
    spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);
    bool first_check = TRUE;
    DBUG_PRINT("info",("spider select_lex=%p", select_lex));
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
    DBUG_PRINT("info",("spider leaf_tables.elements=%u",
      select_lex ? select_lex->leaf_tables.elements : 0));
#endif

    if (select_lex && (select_lex->options & SELECT_DISTINCT))
    {
      DBUG_PRINT("info",("spider with distinct"));
      spider->result_list.direct_distinct = TRUE;
    }
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
    spider->result_list.direct_aggregate = TRUE;
#endif
    DBUG_PRINT("info",("spider select_limit=%lld", select_limit));
    DBUG_PRINT("info",("spider offset_limit=%lld", offset_limit));
    if (
#if MYSQL_VERSION_ID < 50500
      !thd->variables.engine_condition_pushdown ||
#else
#ifdef SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
#else
      !(thd->variables.optimizer_switch &
        OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN) ||
#endif
#endif
#ifdef SPIDER_NEED_CHECK_CONDITION_AT_CHECKING_DIRECT_ORDER_LIMIT
      !spider->condition ||
#endif
      !select_lex ||
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
      select_lex->leaf_tables.elements != 1 ||
#endif
      select_lex->table_list.elements != 1
    ) {
      DBUG_PRINT("info",("spider first_check is FALSE"));
      first_check = FALSE;
      spider->result_list.direct_distinct = FALSE;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
      spider->result_list.direct_aggregate = FALSE;
#endif
    } else if (spider_db_append_condition(spider, NULL, 0, TRUE))
    {
      DBUG_PRINT("info",("spider FALSE by condition"));
      first_check = FALSE;
      spider->result_list.direct_distinct = FALSE;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
      spider->result_list.direct_aggregate = FALSE;
#endif
    } else if (spider->sql_kinds & SPIDER_SQL_KIND_HANDLER)
    {
      DBUG_PRINT("info",("spider sql_kinds with SPIDER_SQL_KIND_HANDLER"));
      spider->result_list.direct_distinct = FALSE;
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
      spider->result_list.direct_aggregate = FALSE;
    } else if (
      !select_lex->group_list.elements &&
      !select_lex->with_sum_func
    ) {
      DBUG_PRINT("info",("spider this SQL is not aggregate SQL"));
      spider->result_list.direct_aggregate = FALSE;
    } else {
      ORDER *group;
      for (group = (ORDER *) select_lex->group_list.first; group;
        group = group->next)
      {
        if (spider->print_item_type((*group->item), NULL, NULL, 0))
        {
          DBUG_PRINT("info",("spider aggregate FALSE by group"));
          spider->result_list.direct_aggregate = FALSE;
          break;
        }
      }
      JOIN *join = select_lex->join;
      Item_sum **item_sum_ptr;
      for (item_sum_ptr = join->sum_funcs; *item_sum_ptr; ++item_sum_ptr)
      {
        if (spider->print_item_type(*item_sum_ptr, NULL, NULL, 0))
        {
          DBUG_PRINT("info",("spider aggregate FALSE by not supported"));
          spider->result_list.direct_aggregate = FALSE;
          break;
        }
      }
      if (!spider_all_part_in_order((ORDER *) select_lex->group_list.first,
        spider->get_table()))
      {
        DBUG_PRINT("info",("spider FALSE by group condition"));
        first_check = FALSE;
        spider->result_list.direct_distinct = FALSE;
      }
#endif
    }

    longlong direct_order_limit = spider_param_direct_order_limit(thd,
      share->direct_order_limit);
    DBUG_PRINT("info",("spider direct_order_limit=%lld", direct_order_limit));
    if (direct_order_limit)
    {
      DBUG_PRINT("info",("spider first_check=%s",
        first_check ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("spider (select_lex->options & OPTION_FOUND_ROWS)=%s",
        select_lex && (select_lex->options & OPTION_FOUND_ROWS) ? "TRUE" : "FALSE"));
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
      DBUG_PRINT("info",("spider direct_aggregate=%s",
        spider->result_list.direct_aggregate ? "TRUE" : "FALSE"));
#endif
      DBUG_PRINT("info",("spider select_lex->group_list.elements=%u",
        select_lex ? select_lex->group_list.elements : 0));
      DBUG_PRINT("info",("spider select_lex->with_sum_func=%s",
        select_lex && select_lex->with_sum_func ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("spider select_lex->having=%s",
        select_lex && select_lex->having ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("spider select_lex->order_list.elements=%u",
        select_lex ? select_lex->order_list.elements : 0));
      if (
        !first_check ||
        !select_lex->limit_params.explicit_limit ||
        (select_lex->options & OPTION_FOUND_ROWS) ||
        (
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
          !spider->result_list.direct_aggregate &&
#endif
          (
            select_lex->group_list.elements ||
            select_lex->with_sum_func
          )
        ) ||
        select_lex->having ||
        !select_lex->order_list.elements ||
        select_limit > direct_order_limit - offset_limit
      ) {
        DBUG_PRINT("info",("spider FALSE by select_lex"));
        DBUG_RETURN(FALSE);
      }
      ORDER *order;
      for (order = (ORDER *) select_lex->order_list.first; order;
        order = order->next)
      {
        if (spider->print_item_type((*order->item), NULL, NULL, 0))
        {
          DBUG_PRINT("info",("spider FALSE by order"));
          DBUG_RETURN(FALSE);
        }
      }
      DBUG_PRINT("info",("spider TRUE"));
      spider->result_list.internal_limit = select_limit + offset_limit;
      spider->result_list.split_read = select_limit + offset_limit;
      spider->wide_handler->trx->direct_order_limit_count++;
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_PRINT("info",("spider FALSE by parameter"));
  DBUG_RETURN(FALSE);
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
bool spider_all_part_in_order(
  ORDER *order,
  TABLE *table
) {
  TABLE_LIST *parent;
  partition_info *part_info;
  Field **part_fields;
  ORDER *ptr;
  Item *item;
  Item_field *item_field;
  DBUG_ENTER("spider_all_part_in_order");
  while (TRUE)
  {
    DBUG_PRINT("info", ("spider table_name = %s", table->s->db.str));
    DBUG_PRINT("info",("spider part_info=%p", table->part_info));
    if ((part_info = table->part_info))
    {
      for (part_fields = part_info->full_part_field_array;
        *part_fields; ++part_fields)
      {
        DBUG_PRINT("info", ("spider part_field = %s",
          SPIDER_field_name_str(*part_fields)));
        for (ptr = order; ptr; ptr = ptr->next)
        {
          item = *ptr->item;
          if (item->type() != Item::FIELD_ITEM)
          {
            continue;
          }
          item_field = (Item_field *) item;
          Field *field = item_field->field;
          if (!field)
          {
            continue;
          }
          DBUG_PRINT("info", ("spider field_name = %s.%s",
            field->table->s->db.str, SPIDER_field_name_str(field)));
          if (*part_fields == spider_field_exchange(table->file, field))
          {
            break;
          }
        }
        if (!ptr)
        {
          DBUG_RETURN(FALSE);
        }
      }
    }
    if (!(parent = table->pos_in_table_list->parent_l))
    {
      break;
    }
    table = parent->table;
  }
  DBUG_RETURN(TRUE);
}

Field *spider_field_exchange(
  handler *handler,
  Field *field
) {
  DBUG_ENTER("spider_field_exchange");
#ifdef HA_CAN_BULK_ACCESS
  if (handler->is_bulk_access_clone)
  {
    handler = handler->pt_clone_source_handler;
  }
#endif
  DBUG_PRINT("info",("spider in field=%p", field));
  DBUG_PRINT("info",("spider in field->table=%p", field->table));
    DBUG_PRINT("info",("spider table=%p", handler->get_table()));
    if (field->table != handler->get_table())
      DBUG_RETURN(NULL);
  DBUG_PRINT("info",("spider out field=%p", field));
  DBUG_RETURN(field);
}
#endif

int spider_set_direct_limit_offset(
  ha_spider *spider
) {
#ifndef SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
  THD *thd = spider->wide_handler->trx->thd;
#endif
  st_select_lex *select_lex;
  longlong select_limit;
  longlong offset_limit;
  TABLE_LIST *table_list;
  DBUG_ENTER("spider_set_direct_limit_offset");

  if (spider->result_list.direct_limit_offset)
    DBUG_RETURN(TRUE);

  if (
    spider->partition_handler &&
    !spider->wide_handler_owner
  ) {
    if (spider->partition_handler->owner->
      result_list.direct_limit_offset == TRUE)
    {
      spider->result_list.direct_limit_offset = TRUE;
      DBUG_RETURN(TRUE);
    } else {
      DBUG_RETURN(FALSE);
    }
  }

  if (
    spider->wide_handler->sql_command != SQLCOM_SELECT ||
#ifdef HANDLER_HAS_DIRECT_AGGREGATE
    spider->result_list.direct_aggregate ||
#endif
    spider->result_list.direct_order_limit ||
    spider->prev_index_rnd_init != SPD_RND    // must be RND_INIT and not be INDEX_INIT
  )
    DBUG_RETURN(FALSE);

  spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);

  // limit and offset is non-zero
  if (!(select_limit && offset_limit))
    DBUG_RETURN(FALSE);

  // more than one table
  if (
    !select_lex ||
    select_lex->table_list.elements != 1
  )
    DBUG_RETURN(FALSE);

  table_list = (TABLE_LIST *) select_lex->table_list.first;
  if (table_list->table->file->partition_ht() != spider_hton_ptr)
  {
    DBUG_PRINT("info",("spider ht1=%u ht2=%u",
      table_list->table->file->partition_ht()->slot,
      spider_hton_ptr->slot
    ));
    DBUG_RETURN(FALSE);
  }

  // contain where
  if (
#if MYSQL_VERSION_ID < 50500
    !thd->variables.engine_condition_pushdown ||
#else
#ifdef SPIDER_ENGINE_CONDITION_PUSHDOWN_IS_ALWAYS_ON
#else
    !(thd->variables.optimizer_switch &
      OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN) ||
#endif
#endif
    // conditions is null may be no where condition in rand_init
    spider->wide_handler->condition
  )
    DBUG_RETURN(FALSE);

  // ignore condition like 1=1
#ifdef SPIDER_has_Item_has_subquery
  if (select_lex->where && select_lex->where->has_subquery())
#else
    if (select_lex->where && select_lex->where->with_subquery())
#endif
    DBUG_RETURN(FALSE);

  if (
    select_lex->group_list.elements ||
    select_lex->with_sum_func ||
    select_lex->having ||
    select_lex->order_list.elements
  )
    DBUG_RETURN(FALSE);

  // must not be derived table
  if (SPIDER_get_linkage(select_lex) == DERIVED_TABLE_TYPE)
    DBUG_RETURN(FALSE);

  spider->direct_select_offset = offset_limit;
  spider->direct_current_offset = offset_limit;
  spider->direct_select_limit = select_limit;
  spider->result_list.direct_limit_offset = TRUE;
  DBUG_RETURN(TRUE);
}


bool spider_check_index_merge(
  TABLE *table,
  st_select_lex *select_lex
) {
  uint roop_count;
  JOIN *join;
  DBUG_ENTER("spider_check_index_merge");
  if (!select_lex)
  {
    DBUG_PRINT("info",("spider select_lex is null"));
    DBUG_RETURN(FALSE);
  }
  join = select_lex->join;
  if (!join)
  {
    DBUG_PRINT("info",("spider join is null"));
    DBUG_RETURN(FALSE);
  }
  if (!join->join_tab)
  {
    DBUG_PRINT("info",("spider join->join_tab is null"));
    DBUG_RETURN(FALSE);
  }
  for (roop_count = 0; roop_count < spider_join_table_count(join); ++roop_count)
  {
    JOIN_TAB *join_tab = &join->join_tab[roop_count];
    if (join_tab->table == table)
    {
      DBUG_PRINT("info",("spider join_tab->type=%u", join_tab->type));
      if (
#ifdef SPIDER_HAS_JT_HASH_INDEX_MERGE
        join_tab->type == JT_HASH_INDEX_MERGE ||
#endif
        join_tab->type == JT_INDEX_MERGE
      ) {
        DBUG_RETURN(TRUE);
      }
/*
      DBUG_PRINT("info",("spider join_tab->quick->get_type()=%u",
        join_tab->quick ? join_tab->quick->get_type() : 0));
      if (
        join_tab->quick &&
        join_tab->quick->get_type() == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE
      ) {
        DBUG_RETURN(TRUE);
      }
*/
      DBUG_PRINT("info",("spider join_tab->select->quick->get_type()=%u",
        join_tab->select && join_tab->select->quick ? join_tab->select->quick->get_type() : 0));
      if (
        join_tab->select &&
        join_tab->select->quick &&
        join_tab->select->quick->get_type() == QUICK_SELECT_I::QS_TYPE_INDEX_MERGE
      ) {
        DBUG_RETURN(TRUE);
      }
      break;
    }
  }
  DBUG_RETURN(FALSE);
}

int spider_compare_for_sort(
  SPIDER_SORT *a,
  SPIDER_SORT *b
) {
  DBUG_ENTER("spider_compare_for_sort");
  if (a->sort > b->sort)
    DBUG_RETURN(-1);
  if (a->sort < b->sort)
    DBUG_RETURN(1);
  DBUG_RETURN(0);
}

ulong spider_calc_for_sort(
  uint count,
  ...
) {
  ulong sort = 0;
  va_list args;
  va_start(args, count);
  DBUG_ENTER("spider_calc_for_sort");
  while (count--)
  {
    char *start = va_arg(args, char *), *str;
    uint wild_pos = 0;

    if ((str = start))
    {
      wild_pos = 128;
      for (; *str; str++)
      {
        if (*str == spider_wild_prefix && str[1])
          str++;
        else if (*str == spider_wild_many || *str == spider_wild_one)
        {
          wild_pos = (uint) (str - start) + 1;
          if (wild_pos > 127)
            wild_pos = 127;
          break;
        }
      }
    }
    sort = (sort << 8) + wild_pos;
  }
  va_end(args);
  DBUG_RETURN(sort);
}

double spider_rand(
  uint32 rand_source
) {
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
  struct my_rnd_struct rand;
#else
  struct rand_struct rand;
#endif
  DBUG_ENTER("spider_rand");
  /* generate same as rand function for applications */
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
  my_rnd_init(&rand, (uint32) (rand_source * 65537L + 55555555L),
    (uint32) (rand_source * 268435457L));
#else
  randominit(&rand, (uint32) (rand_source * 65537L + 55555555L),
    (uint32) (rand_source * 268435457L));
#endif
  DBUG_RETURN(my_rnd(&rand));
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_discover_table_structure_internal(
  SPIDER_TRX *trx,
  SPIDER_SHARE *spider_share,
  spider_string *str
) {
  int error_num = 0, roop_count;
  DBUG_ENTER("spider_discover_table_structure_internal");
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    if (spider_bit_is_set(spider_share->dbton_bitmap, roop_count))
    {
      if ((error_num = spider_share->dbton_share[roop_count]->
        discover_table_structure(trx, spider_share, str)))
      {
        continue;
      }
      break;
    }
  }
  DBUG_RETURN(error_num);
}

int spider_discover_table_structure(
  handlerton *hton,
  THD* thd,
  TABLE_SHARE *share,
  HA_CREATE_INFO *info
) {
  int error_num = HA_ERR_WRONG_COMMAND, dummy;
  SPIDER_SHARE *spider_share;
  const char *table_name = share->path.str;
  uint table_name_length = (uint) strlen(table_name);
  SPIDER_TRX *trx;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info = thd->work_part_info;
#endif
  SPIDER_Open_tables_backup open_tables_backup;
  TABLE *table_tables;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  uint str_len;
#endif
  char buf[MAX_FIELD_WIDTH];
  spider_string str(buf, sizeof(buf), system_charset_info);
  DBUG_ENTER("spider_discover_table_structure");
  str.init_calc_mem(229);
  str.length(0);
  if (str.reserve(
    SPIDER_SQL_CREATE_TABLE_LEN + share->db.length +
    SPIDER_SQL_DOT_LEN + share->table_name.length +
    /* SPIDER_SQL_LCL_NAME_QUOTE_LEN */ 4 + SPIDER_SQL_OPEN_PAREN_LEN
  )) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str.q_append(SPIDER_SQL_CREATE_TABLE_STR, SPIDER_SQL_CREATE_TABLE_LEN);
  str.q_append(SPIDER_SQL_LCL_NAME_QUOTE_STR, SPIDER_SQL_LCL_NAME_QUOTE_LEN);
  str.q_append(share->db.str, share->db.length);
  str.q_append(SPIDER_SQL_LCL_NAME_QUOTE_STR, SPIDER_SQL_LCL_NAME_QUOTE_LEN);
  str.q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  str.q_append(SPIDER_SQL_LCL_NAME_QUOTE_STR, SPIDER_SQL_LCL_NAME_QUOTE_LEN);
  str.q_append(share->table_name.str, share->table_name.length);
  str.q_append(SPIDER_SQL_LCL_NAME_QUOTE_STR, SPIDER_SQL_LCL_NAME_QUOTE_LEN);
  str.q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  str_len = str.length();
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value = my_calc_hash(&spider_open_tables,
    (uchar*) table_name, table_name_length);
#endif
  if (!(trx = spider_get_trx(thd, TRUE, &error_num)))
  {
    DBUG_PRINT("info",("spider spider_get_trx error"));
    my_error(error_num, MYF(0));
    DBUG_RETURN(error_num);
  }
  share->table_charset = info->default_table_charset;
  share->comment = info->comment;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (!part_info)
  {
#endif
    if (!(spider_share = spider_create_share(table_name, share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
      NULL,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      hash_value,
#endif
      &error_num
    ))) {
      DBUG_RETURN(error_num);
    }

    error_num = spider_discover_table_structure_internal(trx, spider_share, &str);

    if (!error_num)
    {
      if (
        (table_tables = spider_open_sys_table(
          thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
          SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, FALSE,
          &error_num))
      ) {
#ifdef SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
        if (thd->lex->create_info.or_replace())
        {
          error_num = spider_delete_tables(table_tables,
            spider_share->table_name, &dummy);
        }
        if (!error_num)
        {
#endif
          error_num = spider_insert_tables(table_tables, spider_share);
#ifdef SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
        }
#endif
        spider_close_sys_table(thd, table_tables,
          &open_tables_backup, FALSE);
      }
    }

    spider_free_share_resource_only(spider_share);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  } else {
    char tmp_name[FN_REFLEN + 1];
    List_iterator<partition_element> part_it(part_info->partitions);
    List_iterator<partition_element> part_it2(part_info->partitions);
    partition_element *part_elem, *sub_elem;
    while ((part_elem = part_it++))
    {
      if ((part_elem)->subpartitions.elements)
      {
        List_iterator<partition_element> sub_it((part_elem)->subpartitions);
        while ((sub_elem = sub_it++))
        {
          str.length(str_len);
          if ((error_num = SPIDER_create_subpartition_name(
            tmp_name, FN_REFLEN + 1, table_name,
            (part_elem)->partition_name, (sub_elem)->partition_name,
            NORMAL_PART_NAME)))
          {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          DBUG_PRINT("info",("spider tmp_name=%s", tmp_name));
          if (!(spider_share = spider_create_share(tmp_name, share,
            part_info,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
            hash_value,
#endif
            &error_num
          ))) {
            DBUG_RETURN(error_num);
          }

          error_num = spider_discover_table_structure_internal(
            trx, spider_share, &str);

          spider_free_share_resource_only(spider_share);
          if (!error_num)
            break;
        }
        if (!error_num)
          break;
      } else {
        str.length(str_len);
        if ((error_num = SPIDER_create_partition_name(
          tmp_name, FN_REFLEN + 1, table_name,
          (part_elem)->partition_name, NORMAL_PART_NAME, TRUE)))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        DBUG_PRINT("info",("spider tmp_name=%s", tmp_name));
        if (!(spider_share = spider_create_share(tmp_name, share,
          part_info,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
          hash_value,
#endif
          &error_num
        ))) {
          DBUG_RETURN(error_num);
        }

        error_num = spider_discover_table_structure_internal(
          trx, spider_share, &str);

        spider_free_share_resource_only(spider_share);
        if (!error_num)
          break;
      }
    }
    if (!error_num)
    {
      if (
        !(table_tables = spider_open_sys_table(
          thd, SPIDER_SYS_TABLES_TABLE_NAME_STR,
          SPIDER_SYS_TABLES_TABLE_NAME_LEN, TRUE, &open_tables_backup, FALSE,
          &error_num))
      ) {
        DBUG_RETURN(error_num);
      }
      while ((part_elem = part_it2++))
      {
        if ((part_elem)->subpartitions.elements)
        {
          List_iterator<partition_element> sub_it((part_elem)->subpartitions);
          while ((sub_elem = sub_it++))
          {
            if ((error_num = SPIDER_create_subpartition_name(
              tmp_name, FN_REFLEN + 1, table_name,
              (part_elem)->partition_name, (sub_elem)->partition_name,
              NORMAL_PART_NAME)))
            {
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            }
            DBUG_PRINT("info",("spider tmp_name=%s", tmp_name));
            if (!(spider_share = spider_create_share(tmp_name, share,
              part_info,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
              hash_value,
#endif
              &error_num
            ))) {
              DBUG_RETURN(error_num);
            }

#ifdef SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
            if (thd->lex->create_info.or_replace())
            {
              error_num = spider_delete_tables(table_tables,
                spider_share->table_name, &dummy);
            }
            if (!error_num)
            {
#endif
              error_num = spider_insert_tables(table_tables, spider_share);
#ifdef SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
            }
#endif

            spider_free_share_resource_only(spider_share);
            if (error_num)
              break;
          }
          if (error_num)
            break;
        } else {
          if ((error_num = SPIDER_create_partition_name(
            tmp_name, FN_REFLEN + 1, table_name,
            (part_elem)->partition_name, NORMAL_PART_NAME, TRUE)))
          {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          }
          DBUG_PRINT("info",("spider tmp_name=%s", tmp_name));
          if (!(spider_share = spider_create_share(tmp_name, share,
            part_info,
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
            hash_value,
#endif
            &error_num
          ))) {
            DBUG_RETURN(error_num);
          }

#ifdef SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
          if (thd->lex->create_info.or_replace())
          {
            error_num = spider_delete_tables(table_tables,
              spider_share->table_name, &dummy);
          }
          if (!error_num)
          {
#endif
            error_num = spider_insert_tables(table_tables, spider_share);
#ifdef SPIDER_SUPPORT_CREATE_OR_REPLACE_TABLE
          }
#endif

          spider_free_share_resource_only(spider_share);
          if (error_num)
            break;
        }
      }
      spider_close_sys_table(thd, table_tables,
        &open_tables_backup, FALSE);
    }
  }
#endif

  if (!error_num)
    thd->clear_error();
  else
    DBUG_RETURN(error_num);

  str.length(str.length() - SPIDER_SQL_COMMA_LEN);
  CHARSET_INFO *table_charset;
  if (share->table_charset)
  {
    table_charset = share->table_charset;
  } else {
    table_charset = system_charset_info;
  }
  uint csnamelen = table_charset->cs_name.length;
  uint collatelen = table_charset->coll_name.length;
  if (str.reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_DEFAULT_CHARSET_LEN +
    csnamelen + SPIDER_SQL_COLLATE_LEN + collatelen +
    SPIDER_SQL_CONNECTION_LEN + SPIDER_SQL_VALUE_QUOTE_LEN +
    (share->comment.length * 2)
  )) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str.q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  str.q_append(SPIDER_SQL_DEFAULT_CHARSET_STR, SPIDER_SQL_DEFAULT_CHARSET_LEN);
  str.q_append(table_charset->cs_name.str, csnamelen);
  str.q_append(SPIDER_SQL_COLLATE_STR, SPIDER_SQL_COLLATE_LEN);
  str.q_append(table_charset->coll_name.str, collatelen);
  str.q_append(SPIDER_SQL_COMMENT_STR, SPIDER_SQL_COMMENT_LEN);
  str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str.append_escape_string(share->comment.str, share->comment.length);
  if (str.reserve(SPIDER_SQL_CONNECTION_LEN +
    (SPIDER_SQL_VALUE_QUOTE_LEN * 2) +
    (share->connect_string.length * 2)))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str.q_append(SPIDER_SQL_CONNECTION_STR, SPIDER_SQL_CONNECTION_LEN);
  str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str.append_escape_string(share->connect_string.str,
    share->connect_string.length);
  if (str.reserve(SPIDER_SQL_VALUE_QUOTE_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str.q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  DBUG_PRINT("info",("spider part_info=%p", part_info));
  if (part_info)
  {
    uint part_syntax_len;
    char *part_syntax;
    List_iterator<partition_element> part_it(part_info->partitions);
    partition_element *part_elem, *sub_elem;
    while ((part_elem = part_it++))
    {
      part_elem->engine_type = hton;
      if ((part_elem)->subpartitions.elements)
      {
        List_iterator<partition_element> sub_it((part_elem)->subpartitions);
        while ((sub_elem = sub_it++))
        {
          sub_elem->engine_type = hton;
        }
      }
    }
    if (part_info->fix_parser_data(thd))
    {
      DBUG_RETURN(ER_SPIDER_UNKNOWN_NUM);
    }
    if (!(part_syntax = SPIDER_generate_partition_syntax(thd, part_info,
      &part_syntax_len, FALSE, TRUE, info, NULL, NULL)))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    if (str.reserve(part_syntax_len))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str.q_append(part_syntax, part_syntax_len);
    SPIDER_free_part_syntax(part_syntax, MYF(0));
  }
#endif
  DBUG_PRINT("info",("spider str=%s", str.c_ptr_safe()));

  error_num = share->init_from_sql_statement_string(thd, TRUE, str.ptr(),
    str.length());
  DBUG_RETURN(error_num);
}
#endif

#ifndef WITHOUT_SPIDER_BG_SEARCH
int spider_create_spider_object_for_share(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  ha_spider **spider
) {
  int error_num, roop_count, *need_mons;
  SPIDER_CONN **conns;
  uint *conn_link_idx;
  uchar *conn_can_fo;
  char **conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  char **hs_r_conn_keys;
  char **hs_w_conn_keys;
#endif
  spider_db_handler **dbton_hdl;
  SPIDER_WIDE_HANDLER *wide_handler;
  DBUG_ENTER("spider_create_spider_object_for_share");
  DBUG_PRINT("info",("spider trx=%p", trx));
  DBUG_PRINT("info",("spider share=%p", share));
  DBUG_PRINT("info",("spider spider_ptr=%p", spider));
  DBUG_PRINT("info",("spider spider=%p", (*spider)));

  if (*spider)
  {
    /* already exists */
    DBUG_RETURN(0);
  }
  (*spider) = new (&share->mem_root) ha_spider();
  if (!(*spider))
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_spider_alloc;
  }
  DBUG_PRINT("info",("spider spider=%p", (*spider)));
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  if (!(need_mons = (int *)
    spider_bulk_malloc(spider_current_trx, 255, MYF(MY_WME | MY_ZEROFILL),
      &need_mons, (uint) (sizeof(int) * share->link_count),
      &conns, (uint) (sizeof(SPIDER_CONN *) * share->link_count),
      &conn_link_idx, (uint) (sizeof(uint) * share->link_count),
      &conn_can_fo, (uint) (sizeof(uchar) * share->link_bitmap_size),
      &conn_keys, (uint) (sizeof(char *) * share->link_count),
      &hs_r_conn_keys, (uint) (sizeof(char *) * share->link_count),
      &hs_w_conn_keys, (uint) (sizeof(char *) * share->link_count),
      &dbton_hdl, (uint) (sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE),
      &wide_handler, (uint) sizeof(SPIDER_WIDE_HANDLER),
      NullS))
  )
#else
  if (!(need_mons = (int *)
    spider_bulk_malloc(spider_current_trx, 255, MYF(MY_WME | MY_ZEROFILL),
      &need_mons, (uint) (sizeof(int) * share->link_count),
      &conns, (uint) (sizeof(SPIDER_CONN *) * share->link_count),
      &conn_link_idx, (uint) (sizeof(uint) * share->link_count),
      &conn_can_fo, (uint) (sizeof(uchar) * share->link_bitmap_size),
      &conn_keys, (uint) (sizeof(char *) * share->link_count),
      &dbton_hdl, (uint) (sizeof(spider_db_handler *) * SPIDER_DBTON_SIZE),
      &wide_handler, (uint) sizeof(SPIDER_WIDE_HANDLER),
      NullS))
  )
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_need_mons_alloc;
  }
  DBUG_PRINT("info",("spider need_mons=%p", need_mons));
  (*spider)->wide_handler = wide_handler;
  wide_handler->trx = trx;
  (*spider)->change_table_ptr(&share->table, share->table_share);
  (*spider)->share = share;
  (*spider)->conns = conns;
  (*spider)->conn_link_idx = conn_link_idx;
  (*spider)->conn_can_fo = conn_can_fo;
  (*spider)->need_mons = need_mons;
  (*spider)->conn_keys_first_ptr = share->conn_keys[0];
  (*spider)->conn_keys = conn_keys;
#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
  (*spider)->hs_r_conn_keys = hs_r_conn_keys;
  (*spider)->hs_w_conn_keys = hs_w_conn_keys;
#endif
  (*spider)->dbton_handler = dbton_hdl;
  (*spider)->search_link_idx = -1;
  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; roop_count++)
  {
    if (
      spider_bit_is_set(share->dbton_bitmap, roop_count) &&
      spider_dbton[roop_count].create_db_handler
    ) {
      if (!(dbton_hdl[roop_count] = spider_dbton[roop_count].create_db_handler(
        *spider, share->dbton_share[roop_count])))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error_init_db_handler;
      }
      if ((error_num = dbton_hdl[roop_count]->init()))
        goto error_init_db_handler;
    }
  }
  DBUG_PRINT("info",("spider share=%p", (*spider)->share));
  DBUG_PRINT("info",("spider need_mons=%p", (*spider)->need_mons));
  DBUG_RETURN(0);

error_init_db_handler:
  for (; roop_count >= 0; --roop_count)
  {
    if (
      spider_bit_is_set(share->dbton_bitmap, roop_count) &&
      dbton_hdl[roop_count]
    ) {
      delete dbton_hdl[roop_count];
      dbton_hdl[roop_count] = NULL;
    }
  }
  spider_free(spider_current_trx, (*spider)->need_mons, MYF(0));
error_need_mons_alloc:
  delete (*spider);
  (*spider) = NULL;
error_spider_alloc:
  DBUG_RETURN(error_num);
}

void spider_free_spider_object_for_share(
  ha_spider **spider
) {
  int roop_count;
  SPIDER_SHARE *share = (*spider)->share;
  spider_db_handler **dbton_hdl = (*spider)->dbton_handler;
  DBUG_ENTER("spider_free_spider_object_for_share");
  DBUG_PRINT("info",("spider share=%p", share));
  DBUG_PRINT("info",("spider spider_ptr=%p", spider));
  DBUG_PRINT("info",("spider spider=%p", (*spider)));
  for (roop_count = SPIDER_DBTON_SIZE - 1; roop_count >= 0; --roop_count)
  {
    if (
      spider_bit_is_set(share->dbton_bitmap, roop_count) &&
      dbton_hdl[roop_count]
    ) {
      delete dbton_hdl[roop_count];
      dbton_hdl[roop_count] = NULL;
    }
  }
  spider_free(spider_current_trx, (*spider)->need_mons, MYF(0));
  delete (*spider);
  (*spider) = NULL;
  DBUG_VOID_RETURN;
}

int spider_create_sts_threads(
  SPIDER_THREAD *spider_thread
) {
  int error_num;
  DBUG_ENTER("spider_create_sts_threads");
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_thread->mutex,
    MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_bg_stss,
    &spider_thread->mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_mutex_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_cond_init(&spider_thread->cond, NULL))
#else
  if (mysql_cond_init(spd_key_cond_bg_stss,
    &spider_thread->cond, NULL))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_cond_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_cond_init(&spider_thread->sync_cond, NULL))
#else
  if (mysql_cond_init(spd_key_cond_bg_sts_syncs,
    &spider_thread->sync_cond, NULL))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_sync_cond_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_create(&spider_thread->thread, &spider_pt_attr,
    spider_table_bg_sts_action, (void *) spider_thread)
  )
#else
  if (mysql_thread_create(spd_key_thd_bg_stss, &spider_thread->thread,
    &spider_pt_attr, spider_table_bg_sts_action, (void *) spider_thread)
  )
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_thread_create;
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&spider_thread->sync_cond);
error_sync_cond_init:
  pthread_cond_destroy(&spider_thread->cond);
error_cond_init:
  pthread_mutex_destroy(&spider_thread->mutex);
error_mutex_init:
  DBUG_RETURN(error_num);
}

void spider_free_sts_threads(
  SPIDER_THREAD *spider_thread
) {
  bool thread_killed;
  DBUG_ENTER("spider_free_sts_threads");
  pthread_mutex_lock(&spider_thread->mutex);
  thread_killed = spider_thread->killed;
  spider_thread->killed = TRUE;
  if (!thread_killed)
  {
    if (spider_thread->thd_wait)
    {
      pthread_cond_signal(&spider_thread->cond);
    }
    pthread_cond_wait(&spider_thread->sync_cond, &spider_thread->mutex);
  }
  pthread_mutex_unlock(&spider_thread->mutex);
  pthread_join(spider_thread->thread, NULL);
  pthread_cond_destroy(&spider_thread->sync_cond);
  pthread_cond_destroy(&spider_thread->cond);
  pthread_mutex_destroy(&spider_thread->mutex);
  spider_thread->thd_wait = FALSE;
  spider_thread->killed = FALSE;
  DBUG_VOID_RETURN;
}

int spider_create_crd_threads(
  SPIDER_THREAD *spider_thread
) {
  int error_num;
  DBUG_ENTER("spider_create_crd_threads");
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&spider_thread->mutex,
    MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(spd_key_mutex_bg_crds,
    &spider_thread->mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_mutex_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_cond_init(&spider_thread->cond, NULL))
#else
  if (mysql_cond_init(spd_key_cond_bg_crds,
    &spider_thread->cond, NULL))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_cond_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_cond_init(&spider_thread->sync_cond, NULL))
#else
  if (mysql_cond_init(spd_key_cond_bg_crd_syncs,
    &spider_thread->sync_cond, NULL))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_sync_cond_init;
  }
#if MYSQL_VERSION_ID < 50500
  if (pthread_create(&spider_thread->thread, &spider_pt_attr,
    spider_table_bg_crd_action, (void *) spider_thread)
  )
#else
  if (mysql_thread_create(spd_key_thd_bg_crds, &spider_thread->thread,
    &spider_pt_attr, spider_table_bg_crd_action, (void *) spider_thread)
  )
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_thread_create;
  }
  DBUG_RETURN(0);

error_thread_create:
  pthread_cond_destroy(&spider_thread->sync_cond);
error_sync_cond_init:
  pthread_cond_destroy(&spider_thread->cond);
error_cond_init:
  pthread_mutex_destroy(&spider_thread->mutex);
error_mutex_init:
  DBUG_RETURN(error_num);
}

void spider_free_crd_threads(
  SPIDER_THREAD *spider_thread
) {
  bool thread_killed;
  DBUG_ENTER("spider_free_crd_threads");
  pthread_mutex_lock(&spider_thread->mutex);
  thread_killed = spider_thread->killed;
  spider_thread->killed = TRUE;
  if (!thread_killed)
  {
    if (spider_thread->thd_wait)
    {
      pthread_cond_signal(&spider_thread->cond);
    }
    pthread_cond_wait(&spider_thread->sync_cond, &spider_thread->mutex);
  }
  pthread_mutex_unlock(&spider_thread->mutex);
  pthread_join(spider_thread->thread, NULL);
  pthread_cond_destroy(&spider_thread->sync_cond);
  pthread_cond_destroy(&spider_thread->cond);
  pthread_mutex_destroy(&spider_thread->mutex);
  spider_thread->thd_wait = FALSE;
  spider_thread->killed = FALSE;
  DBUG_VOID_RETURN;
}

void *spider_table_bg_sts_action(
  void *arg
) {
  SPIDER_THREAD *thread = (SPIDER_THREAD *) arg;
  SPIDER_SHARE *share;
  SPIDER_TRX *trx;
  int error_num;
  ha_spider *spider;
  SPIDER_CONN **conns;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_table_bg_sts_action");
  /* init start */
  pthread_mutex_lock(&thread->mutex);
  if (!(thd = spider_create_sys_thd(thread)))
  {
    thread->thd_wait = FALSE;
    thread->killed = FALSE;
    pthread_mutex_unlock(&thread->mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd_proc_info(thd, "Spider table background statistics action handler");
  if (!(trx = spider_get_trx(NULL, FALSE, &error_num)))
  {
    spider_destroy_sys_thd(thd);
    thread->thd_wait = FALSE;
    thread->killed = FALSE;
    pthread_mutex_unlock(&thread->mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  trx->thd = thd;
  /* init end */

  if (thread->init_command)
  {
    uint i = 0;
    tmp_disable_binlog(thd);
    thd->security_ctx->skip_grants();
    thd->client_capabilities |= CLIENT_MULTI_RESULTS;
    if (!(*spd_mysqld_server_started) && !thd->killed)
    {
      pthread_mutex_lock(spd_LOCK_server_started);
      thd->mysys_var->current_cond = spd_COND_server_started;
      thd->mysys_var->current_mutex = spd_LOCK_server_started;
      if (!(*spd_mysqld_server_started) && !thd->killed)
      {
        do
        {
          struct timespec abstime;
          set_timespec_nsec(abstime, 1000);
          error_num = pthread_cond_timedwait(spd_COND_server_started,
            spd_LOCK_server_started, &abstime);
        } while (
          (error_num == ETIMEDOUT || error_num == ETIME) &&
          !(*spd_mysqld_server_started) && !thd->killed && !thread->killed
        );
      }
      pthread_mutex_unlock(spd_LOCK_server_started);
      thd->mysys_var->current_cond = &thread->cond;
      thd->mysys_var->current_mutex = &thread->mutex;
    }
    while (spider_init_queries[i].length && !thd->killed && !thread->killed)
    {
      dispatch_command(COM_QUERY, thd, spider_init_queries[i].str,
        (uint) spider_init_queries[i].length);
      if (unlikely(thd->is_error()))
      {
        fprintf(stderr, "[ERROR] %s\n", spider_stmt_da_message(thd));
        thd->clear_error();
        break;
      }
      ++i;
    }
    thd->mysys_var->current_cond = &thread->cond;
    thd->mysys_var->current_mutex = &thread->mutex;
    thd->client_capabilities -= CLIENT_MULTI_RESULTS;
    reenable_binlog(thd);
    thread->init_command = FALSE;
    pthread_cond_broadcast(&thread->sync_cond);
  }
  if (thd->killed)
  {
    thread->killed = TRUE;
  }
  if (thd->killed)
  {
    thread->killed = TRUE;
  }

  while (TRUE)
  {
    DBUG_PRINT("info",("spider bg sts loop start"));
    if (thread->killed)
    {
      DBUG_PRINT("info",("spider bg sts kill start"));
      trx->thd = NULL;
      spider_free_trx(trx, TRUE);
      spider_destroy_sys_thd(thd);
      pthread_cond_signal(&thread->sync_cond);
      pthread_mutex_unlock(&thread->mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
      set_current_thd(nullptr);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (!thread->queue_first)
    {
      DBUG_PRINT("info",("spider bg sts has no job"));
      thread->thd_wait = TRUE;
      pthread_cond_wait(&thread->cond, &thread->mutex);
      thread->thd_wait = FALSE;
      if (thd->killed)
        thread->killed = TRUE;
      continue;
    }
    share = (SPIDER_SHARE *) thread->queue_first;
    share->sts_working = TRUE;
    pthread_mutex_unlock(&thread->mutex);

    spider = share->sts_spider;
    conns = spider->conns;
    if (spider->search_link_idx < 0)
    {
      spider->wide_handler->trx = trx;
      spider_trx_set_link_idx_for_all(spider);
      spider->search_link_idx = spider_conn_first_link_idx(thd,
        share->link_statuses, share->access_balances, spider->conn_link_idx,
        share->link_count, SPIDER_LINK_STATUS_OK);
    }
    if (spider->search_link_idx >= 0)
    {
      DBUG_PRINT("info",
        ("spider difftime=%f",
          difftime(share->bg_sts_try_time, share->sts_get_time)));
      DBUG_PRINT("info",
        ("spider bg_sts_interval=%f", share->bg_sts_interval));
      if (difftime(share->bg_sts_try_time, share->sts_get_time) >=
        share->bg_sts_interval)
      {
        if (!conns[spider->search_link_idx])
        {
          spider_get_conn(share, spider->search_link_idx,
            share->conn_keys[spider->search_link_idx],
            trx, spider, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL,
            &error_num);
          if (conns[spider->search_link_idx])
          {
            conns[spider->search_link_idx]->error_mode = 0;
          } else {
            spider->search_link_idx = -1;
          }
        }
        DBUG_PRINT("info",
          ("spider search_link_idx=%d", spider->search_link_idx));
        if (spider->search_link_idx >= 0 && conns[spider->search_link_idx])
        {
#ifdef WITH_PARTITION_STORAGE_ENGINE
          if (spider_get_sts(share, spider->search_link_idx,
            share->bg_sts_try_time, spider,
            share->bg_sts_interval, share->bg_sts_mode,
            share->bg_sts_sync,
            2, HA_STATUS_CONST | HA_STATUS_VARIABLE))
#else
          if (spider_get_sts(share, spider->search_link_idx,
            share->bg_sts_try_time, spider,
            share->bg_sts_interval, share->bg_sts_mode,
            2, HA_STATUS_CONST | HA_STATUS_VARIABLE))
#endif
          {
            spider->search_link_idx = -1;
          }
        }
      }
    }
    memset(spider->need_mons, 0, sizeof(int) * share->link_count);
    pthread_mutex_lock(&thread->mutex);
    if (thread->queue_first == thread->queue_last)
    {
      thread->queue_first = NULL;
      thread->queue_last = NULL;
    } else {
      thread->queue_first = share->sts_next;
      share->sts_next->sts_prev = NULL;
      share->sts_next = NULL;
    }
    share->sts_working = FALSE;
    share->sts_wait = FALSE;
    if (thread->first_free_wait)
    {
      pthread_cond_signal(&thread->sync_cond);
      pthread_cond_wait(&thread->cond, &thread->mutex);
      if (thd->killed)
        thread->killed = TRUE;
    }
  }
}

void *spider_table_bg_crd_action(
  void *arg
) {
  SPIDER_THREAD *thread = (SPIDER_THREAD *) arg;
  SPIDER_SHARE *share;
  SPIDER_TRX *trx;
  int error_num;
  ha_spider *spider;
  TABLE *table;
  SPIDER_CONN **conns;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("spider_table_bg_crd_action");
  /* init start */
  pthread_mutex_lock(&thread->mutex);
  if (!(thd = spider_create_sys_thd(thread)))
  {
    thread->thd_wait = FALSE;
    thread->killed = FALSE;
    pthread_mutex_unlock(&thread->mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  SPIDER_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd_proc_info(thd, "Spider table background cardinality action handler");
  if (!(trx = spider_get_trx(NULL, FALSE, &error_num)))
  {
    spider_destroy_sys_thd(thd);
    thread->thd_wait = FALSE;
    thread->killed = FALSE;
    pthread_mutex_unlock(&thread->mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
    set_current_thd(nullptr);
#endif
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  trx->thd = thd;
  /* init end */

  while (TRUE)
  {
    DBUG_PRINT("info",("spider bg crd loop start"));
    if (thread->killed)
    {
      DBUG_PRINT("info",("spider bg crd kill start"));
      trx->thd = NULL;
      spider_free_trx(trx, TRUE);
      spider_destroy_sys_thd(thd);
      pthread_cond_signal(&thread->sync_cond);
      pthread_mutex_unlock(&thread->mutex);
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
      set_current_thd(nullptr);
#endif
      my_thread_end();
      DBUG_RETURN(NULL);
    }
    if (!thread->queue_first)
    {
      DBUG_PRINT("info",("spider bg crd has no job"));
      thread->thd_wait = TRUE;
      pthread_cond_wait(&thread->cond, &thread->mutex);
      thread->thd_wait = FALSE;
      if (thd->killed)
        thread->killed = TRUE;
      continue;
    }
    share = (SPIDER_SHARE *) thread->queue_first;
    share->crd_working = TRUE;
    pthread_mutex_unlock(&thread->mutex);

    table = &share->table;
    spider = share->crd_spider;
    conns = spider->conns;
    if (spider->search_link_idx < 0)
    {
      spider->wide_handler->trx = trx;
      spider_trx_set_link_idx_for_all(spider);
      spider->search_link_idx = spider_conn_first_link_idx(thd,
        share->link_statuses, share->access_balances, spider->conn_link_idx,
        share->link_count, SPIDER_LINK_STATUS_OK);
    }
    if (spider->search_link_idx >= 0)
    {
      DBUG_PRINT("info",
        ("spider difftime=%f",
          difftime(share->bg_crd_try_time, share->crd_get_time)));
      DBUG_PRINT("info",
        ("spider bg_crd_interval=%f", share->bg_crd_interval));
      if (difftime(share->bg_crd_try_time, share->crd_get_time) >=
        share->bg_crd_interval)
      {
        if (!conns[spider->search_link_idx])
        {
          spider_get_conn(share, spider->search_link_idx,
            share->conn_keys[spider->search_link_idx],
            trx, spider, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL,
            &error_num);
          if (conns[spider->search_link_idx])
          {
            conns[spider->search_link_idx]->error_mode = 0;
          } else {
            spider->search_link_idx = -1;
          }
        }
        DBUG_PRINT("info",
          ("spider search_link_idx=%d", spider->search_link_idx));
        if (spider->search_link_idx >= 0 && conns[spider->search_link_idx])
        {
#ifdef WITH_PARTITION_STORAGE_ENGINE
          if (spider_get_crd(share, spider->search_link_idx,
            share->bg_crd_try_time, spider, table,
            share->bg_crd_interval, share->bg_crd_mode,
            share->bg_crd_sync,
            2))
#else
          if (spider_get_crd(share, spider->search_link_idx,
            share->bg_crd_try_time, spider, table,
            share->bg_crd_interval, share->bg_crd_mode,
            2))
#endif
          {
            spider->search_link_idx = -1;
          }
        }
      }
    }
    memset(spider->need_mons, 0, sizeof(int) * share->link_count);
    pthread_mutex_lock(&thread->mutex);
    if (thread->queue_first == thread->queue_last)
    {
      thread->queue_first = NULL;
      thread->queue_last = NULL;
    } else {
      thread->queue_first = share->crd_next;
      share->crd_next->crd_prev = NULL;
      share->crd_next = NULL;
    }
    share->crd_working = FALSE;
    share->crd_wait = FALSE;
    if (thread->first_free_wait)
    {
      pthread_cond_signal(&thread->sync_cond);
      pthread_cond_wait(&thread->cond, &thread->mutex);
      if (thd->killed)
        thread->killed = TRUE;
    }
  }
}

void spider_table_add_share_to_sts_thread(
  SPIDER_SHARE *share
) {
  SPIDER_THREAD *spider_thread = share->sts_thread;
  DBUG_ENTER("spider_table_add_share_to_sts_thread");
  if (
    !share->sts_wait &&
    !pthread_mutex_trylock(&spider_thread->mutex)
  ) {
    if (!share->sts_wait)
    {
      if (spider_thread->queue_last)
      {
        DBUG_PRINT("info",("spider add to last"));
        share->sts_prev = spider_thread->queue_last;
        spider_thread->queue_last->sts_next = share;
      } else {
        spider_thread->queue_first = share;
      }
      spider_thread->queue_last = share;
      share->sts_wait = TRUE;

      if (spider_thread->thd_wait)
      {
        pthread_cond_signal(&spider_thread->cond);
      }
    }
    pthread_mutex_unlock(&spider_thread->mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_table_add_share_to_crd_thread(
  SPIDER_SHARE *share
) {
  SPIDER_THREAD *spider_thread = share->crd_thread;
  DBUG_ENTER("spider_table_add_share_to_crd_thread");
  if (
    !share->crd_wait &&
    !pthread_mutex_trylock(&spider_thread->mutex)
  ) {
    if (!share->crd_wait)
    {
      if (spider_thread->queue_last)
      {
        DBUG_PRINT("info",("spider add to last"));
        share->crd_prev = spider_thread->queue_last;
        spider_thread->queue_last->crd_next = share;
      } else {
        spider_thread->queue_first = share;
      }
      spider_thread->queue_last = share;
      share->crd_wait = TRUE;

      if (spider_thread->thd_wait)
      {
        pthread_cond_signal(&spider_thread->cond);
      }
    }
    pthread_mutex_unlock(&spider_thread->mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_table_remove_share_from_sts_thread(
  SPIDER_SHARE *share
) {
  SPIDER_THREAD *spider_thread = share->sts_thread;
  DBUG_ENTER("spider_table_remove_share_from_sts_thread");
  if (share->sts_wait)
  {
    pthread_mutex_lock(&spider_thread->mutex);
    if (share->sts_wait)
    {
      if (share->sts_working)
      {
        DBUG_PRINT("info",("spider waiting bg sts start"));
        spider_thread->first_free_wait = TRUE;
        pthread_cond_wait(&spider_thread->sync_cond, &spider_thread->mutex);
        spider_thread->first_free_wait = FALSE;
        pthread_cond_signal(&spider_thread->cond);
        DBUG_PRINT("info",("spider waiting bg sts end"));
      }

      if (share->sts_prev)
      {
        if (share->sts_next)
        {
          DBUG_PRINT("info",("spider remove middle one"));
          share->sts_prev->sts_next = share->sts_next;
          share->sts_next->sts_prev = share->sts_prev;
        } else {
          DBUG_PRINT("info",("spider remove last one"));
          share->sts_prev->sts_next = NULL;
          spider_thread->queue_last = share->sts_prev;
        }
      } else if (share->sts_next) {
        DBUG_PRINT("info",("spider remove first one"));
        share->sts_next->sts_prev = NULL;
        spider_thread->queue_first = share->sts_next;
      } else {
        DBUG_PRINT("info",("spider empty"));
        spider_thread->queue_first = NULL;
        spider_thread->queue_last = NULL;
      }
    }
    pthread_mutex_unlock(&spider_thread->mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_table_remove_share_from_crd_thread(
  SPIDER_SHARE *share
) {
  SPIDER_THREAD *spider_thread = share->crd_thread;
  DBUG_ENTER("spider_table_remove_share_from_crd_thread");
  if (share->crd_wait)
  {
    pthread_mutex_lock(&spider_thread->mutex);
    if (share->crd_wait)
    {
      if (share->crd_working)
      {
        DBUG_PRINT("info",("spider waiting bg crd start"));
        spider_thread->first_free_wait = TRUE;
        pthread_cond_wait(&spider_thread->sync_cond, &spider_thread->mutex);
        spider_thread->first_free_wait = FALSE;
        pthread_cond_signal(&spider_thread->cond);
        DBUG_PRINT("info",("spider waiting bg crd end"));
      }

      if (share->crd_prev)
      {
        if (share->crd_next)
        {
          DBUG_PRINT("info",("spider remove middle one"));
          share->crd_prev->crd_next = share->crd_next;
          share->crd_next->crd_prev = share->crd_prev;
        } else {
          DBUG_PRINT("info",("spider remove last one"));
          share->crd_prev->crd_next = NULL;
          spider_thread->queue_last = share->crd_prev;
        }
      } else if (share->crd_next) {
        DBUG_PRINT("info",("spider remove first one"));
        share->crd_next->crd_prev = NULL;
        spider_thread->queue_first = share->crd_next;
      } else {
        DBUG_PRINT("info",("spider empty"));
        spider_thread->queue_first = NULL;
        spider_thread->queue_last = NULL;
      }
    }
    pthread_mutex_unlock(&spider_thread->mutex);
  }
  DBUG_VOID_RETURN;
}
#endif

uchar *spider_duplicate_char(
  uchar *dst,
  uchar esc,
  uchar *src,
  uint src_lgt
) {
  uchar *ed = src + src_lgt;
  DBUG_ENTER("spider_duplicate_char");
  while (src < ed)
  {
    *dst = *src;
    if (*src == esc)
    {
      ++dst;
      *dst = esc;
    }
    ++dst;
    ++src;
  }
  DBUG_RETURN(dst);
}
