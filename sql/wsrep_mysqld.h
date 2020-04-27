/* Copyright 2008-2015 Codership Oy <http://www.codership.com>

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

#include <wsrep.h>

#ifndef WSREP_MYSQLD_H
#define WSREP_MYSQLD_H

#include <mysql/plugin.h>
#include <mysql/service_wsrep.h>

#ifdef WITH_WSREP

typedef struct st_mysql_show_var SHOW_VAR;
#include <sql_priv.h>
//#include "rpl_gtid.h"
#include "../wsrep/wsrep_api.h"
#include "mdl.h"
#include "mysqld.h"
#include "sql_table.h"
#include "wsrep_mysqld_c.h"

#define WSREP_UNDEFINED_TRX_ID ULONGLONG_MAX

class set_var;
class THD;

enum wsrep_consistency_check_mode {
    NO_CONSISTENCY_CHECK,
    CONSISTENCY_CHECK_DECLARED,
    CONSISTENCY_CHECK_RUNNING,
};

struct wsrep_thd_shadow {
  ulonglong            options;
  uint                 server_status;
  enum wsrep_exec_mode wsrep_exec_mode;
  Vio                  *vio;
  ulong                tx_isolation;
  char                 *db;
  size_t               db_length;
  my_hrtime_t          user_time;
  longlong             row_count_func;
};

// Global wsrep parameters
extern wsrep_t*    wsrep;

// MySQL wsrep options
extern const char* wsrep_provider;
extern const char* wsrep_provider_options;
extern const char* wsrep_cluster_name;
extern const char* wsrep_cluster_address;
extern const char* wsrep_node_name;
extern const char* wsrep_node_address;
extern const char* wsrep_node_incoming_address;
extern const char* wsrep_data_home_dir;
extern const char* wsrep_dbug_option;
extern long        wsrep_slave_threads;
extern int         wsrep_slave_count_change;
extern my_bool     wsrep_convert_LOCK_to_trx;
extern ulong       wsrep_retry_autocommit;
extern my_bool     wsrep_auto_increment_control;
extern my_bool     wsrep_incremental_data_collection;
extern const char* wsrep_start_position;
extern ulong       wsrep_max_ws_size;
extern ulong       wsrep_max_ws_rows;
extern const char* wsrep_notify_cmd;
extern long        wsrep_max_protocol_version;
extern ulong       wsrep_forced_binlog_format;
extern my_bool     wsrep_desync;
extern ulong       wsrep_reject_queries;
extern my_bool     wsrep_replicate_myisam;
extern ulong       wsrep_mysql_replication_bundle;
extern my_bool     wsrep_restart_slave;
extern my_bool     wsrep_restart_slave_activated;
extern my_bool     wsrep_slave_FK_checks;
extern my_bool     wsrep_slave_UK_checks;
extern ulong       wsrep_running_threads;
extern bool        wsrep_new_cluster;
extern bool        wsrep_gtid_mode;
extern uint32      wsrep_gtid_domain_id;
extern bool        wsrep_dirty_reads;

enum enum_wsrep_reject_types {
  WSREP_REJECT_NONE,    /* nothing rejected */
  WSREP_REJECT_ALL,     /* reject all queries, with UNKNOWN_COMMAND error */
  WSREP_REJECT_ALL_KILL /* kill existing connections and reject all queries*/
};

enum enum_wsrep_OSU_method {
    WSREP_OSU_TOI,
    WSREP_OSU_RSU,
    WSREP_OSU_NONE,
};

enum enum_wsrep_sync_wait {
    WSREP_SYNC_WAIT_NONE = 0x0,
    // select, begin
    WSREP_SYNC_WAIT_BEFORE_READ = 0x1,
    WSREP_SYNC_WAIT_BEFORE_UPDATE_DELETE = 0x2,
    WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE = 0x4,
    WSREP_SYNC_WAIT_BEFORE_SHOW = 0x8,
    WSREP_SYNC_WAIT_MAX = 0xF
};

// MySQL status variables
extern my_bool     wsrep_connected;
extern my_bool     wsrep_ready;
extern const char* wsrep_cluster_state_uuid;
extern long long   wsrep_cluster_conf_id;
extern const char* wsrep_cluster_status;
extern long        wsrep_cluster_size;
extern long        wsrep_local_index;
extern long long   wsrep_local_bf_aborts;
extern const char* wsrep_provider_name;
extern const char* wsrep_provider_version;
extern const char* wsrep_provider_vendor;

int  wsrep_show_status(THD *thd, SHOW_VAR *var, char *buff,
                       enum enum_var_type scope);
int  wsrep_init();
void wsrep_deinit(bool free_options);

/* Initialize wsrep thread LOCKs and CONDs */
void wsrep_thr_init();
/* Destroy wsrep thread LOCKs and CONDs */
void wsrep_thr_deinit();

void wsrep_recover();
bool wsrep_before_SE(); // initialize wsrep before storage
                        // engines (true) or after (false)
/* wsrep initialization sequence at startup
 * @param before wsrep_before_SE() value */
void wsrep_init_startup(bool before);

// Other wsrep global variables
extern my_bool     wsrep_inited; // whether wsrep is initialized ?


extern "C" void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode);
extern "C" void wsrep_thd_set_query_state(
        THD *thd, enum wsrep_query_state state);

extern "C" void wsrep_thd_set_trx_to_replay(THD *thd, uint64 trx_id);

extern "C" uint32 wsrep_thd_wsrep_rand(THD *thd);
extern "C" time_t wsrep_thd_query_start(THD *thd);
extern "C" query_id_t wsrep_thd_query_id(THD *thd);
extern "C" query_id_t wsrep_thd_wsrep_last_query_id(THD *thd);
extern "C" void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id);

extern int  wsrep_wait_committing_connections_close(int wait_time);
extern void wsrep_close_applier(THD *thd);
extern void wsrep_wait_appliers_close(THD *thd);
extern void wsrep_close_applier_threads(int count);
extern void wsrep_kill_mysql(THD *thd);

/* new defines */
extern void wsrep_stop_replication(THD *thd);
extern bool wsrep_start_replication();
extern bool wsrep_must_sync_wait(THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern bool wsrep_sync_wait(THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern int  wsrep_check_opts();
extern void wsrep_prepend_PATH (const char* path);

/* Other global variables */
extern wsrep_seqno_t wsrep_locked_seqno;

#define WSREP_ON unlikely(global_system_variables.wsrep_on)

#define WSREP(thd) \
  (WSREP_ON && thd->variables.wsrep_on)

#define WSREP_CLIENT(thd) \
    (WSREP(thd) && thd->wsrep_client_thread)

#define WSREP_EMULATE_BINLOG(thd) \
  (WSREP(thd) && wsrep_emulate_bin_log)

#define WSREP_FORMAT(my_format)                           \
  ((wsrep_forced_binlog_format != BINLOG_FORMAT_UNSPEC)   \
    ? wsrep_forced_binlog_format : (ulong)(my_format))

// prefix all messages with "WSREP"
void wsrep_log(void (*fun)(const char *, ...), const char *format, ...);
#define WSREP_LOG(fun, ...) wsrep_log(fun,  ## __VA_ARGS__)
#define WSREP_LOG_CONFLICT_THD(thd, role)                                      \
    WSREP_LOG(sql_print_information, 	                                       \
      "%s: \n "       	                                                       \
      "  THD: %lu, mode: %s, state: %s, conflict: %s, seqno: %lld\n "          \
      "  SQL: %s",							       \
      role, thd_get_thread_id(thd), wsrep_thd_exec_mode_str(thd),            \
      wsrep_thd_query_state_str(thd),                                          \
      wsrep_thd_conflict_state_str(thd), (long long)wsrep_thd_trx_seqno(thd),  \
      wsrep_thd_query(thd)                                                     \
    );

#define WSREP_LOG_CONFLICT(bf_thd, victim_thd, bf_abort)		       \
  if (wsrep_debug || wsrep_log_conflicts)				       \
  {                                                                            \
    WSREP_LOG(sql_print_information, "cluster conflict due to %s for threads:",\
      (bf_abort) ? "high priority abort" : "certification failure"             \
    );                                                                         \
    if (bf_thd != NULL) WSREP_LOG_CONFLICT_THD(bf_thd, "Winning thread");      \
    if (victim_thd) WSREP_LOG_CONFLICT_THD(victim_thd, "Victim thread");       \
  }

#define WSREP_PROVIDER_EXISTS                                                  \
  (wsrep_provider && strncasecmp(wsrep_provider, WSREP_NONE, FN_REFLEN))

#define WSREP_QUERY(thd) (thd->query())

extern my_bool wsrep_ready_get();
extern void wsrep_ready_wait();

class Ha_trx_info;
struct THD_TRANS;
void wsrep_register_hton(THD* thd, bool all);
void wsrep_brute_force_killer(THD *thd);
int  wsrep_hire_brute_force_killer(THD *thd, uint64_t trx_id);

/* this is visible for client build so that innodb plugin gets this */
typedef struct wsrep_aborting_thd {
  struct wsrep_aborting_thd *next;
  THD *aborting_thd;
} *wsrep_aborting_thd_t;

extern mysql_mutex_t LOCK_wsrep_ready;
extern mysql_cond_t  COND_wsrep_ready;
extern mysql_mutex_t LOCK_wsrep_sst;
extern mysql_cond_t  COND_wsrep_sst;
extern mysql_mutex_t LOCK_wsrep_sst_init;
extern mysql_cond_t  COND_wsrep_sst_init;
extern mysql_mutex_t LOCK_wsrep_rollback;
extern mysql_cond_t  COND_wsrep_rollback;
extern int wsrep_replaying;
extern mysql_mutex_t LOCK_wsrep_replaying;
extern mysql_cond_t  COND_wsrep_replaying;
extern mysql_mutex_t LOCK_wsrep_slave_threads;
extern mysql_mutex_t LOCK_wsrep_desync;
extern mysql_mutex_t LOCK_wsrep_config_state;
extern wsrep_aborting_thd_t wsrep_aborting_thd;
extern my_bool       wsrep_emulate_bin_log;
extern int           wsrep_to_isolation;
#ifdef GTID_SUPPORT
extern rpl_sidno     wsrep_sidno;
#endif /* GTID_SUPPORT */
extern my_bool       wsrep_preordered_opt;
extern handlerton    *wsrep_hton;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_LOCK_wsrep_ready;
extern PSI_mutex_key key_COND_wsrep_ready;
extern PSI_mutex_key key_LOCK_wsrep_sst;
extern PSI_cond_key  key_COND_wsrep_sst;
extern PSI_mutex_key key_LOCK_wsrep_sst_init;
extern PSI_cond_key  key_COND_wsrep_sst_init;
extern PSI_mutex_key key_LOCK_wsrep_sst_thread;
extern PSI_cond_key  key_COND_wsrep_sst_thread;
extern PSI_mutex_key key_LOCK_wsrep_rollback;
extern PSI_cond_key  key_COND_wsrep_rollback;
extern PSI_mutex_key key_LOCK_wsrep_replaying;
extern PSI_cond_key  key_COND_wsrep_replaying;
extern PSI_mutex_key key_LOCK_wsrep_slave_threads;
extern PSI_mutex_key key_LOCK_wsrep_desync;

extern PSI_file_key key_file_wsrep_gra_log;
#endif /* HAVE_PSI_INTERFACE */
struct TABLE_LIST;
class Alter_info;
int wsrep_to_isolation_begin(THD *thd, char *db_, char *table_,
                             const TABLE_LIST* table_list,
                             Alter_info* alter_info = NULL);
void wsrep_to_isolation_end(THD *thd);
void wsrep_cleanup_transaction(THD *thd);
int wsrep_to_buf_helper(
  THD* thd, const char *query, uint query_len, uchar** buf, size_t* buf_len);
int wsrep_create_event_query(THD *thd, uchar** buf, size_t* buf_len);

extern bool
wsrep_grant_mdl_exception(MDL_context *requestor_ctx,
                          MDL_ticket *ticket,
                          const MDL_key *key);
IO_CACHE * get_trans_log(THD * thd);
bool wsrep_trans_cache_is_empty(THD *thd);
void thd_binlog_flush_pending_rows_event(THD *thd, bool stmt_end);
void thd_binlog_rollback_stmt(THD * thd);
void thd_binlog_trx_reset(THD * thd);

typedef void (*wsrep_thd_processor_fun)(THD *);
pthread_handler_t start_wsrep_THD(void *arg);
int wsrep_wait_committing_connections_close(int wait_time);
extern void wsrep_close_client_connections(my_bool wait_to_end,
                                           THD *except_caller_thd = NULL);
void wsrep_close_applier(THD *thd);
void wsrep_close_applier_threads(int count);
void wsrep_wait_appliers_close(THD *thd);
void wsrep_kill_mysql(THD *thd);
void wsrep_close_threads(THD *thd);
void wsrep_copy_query(THD *thd);
bool wsrep_is_show_query(enum enum_sql_command command);
void wsrep_replay_transaction(THD *thd);
bool wsrep_create_like_table(THD* thd, TABLE_LIST* table,
                             TABLE_LIST* src_table,
	                     HA_CREATE_INFO *create_info);
bool wsrep_node_is_donor();
bool wsrep_node_is_synced();

typedef struct wsrep_key_arr
{
    wsrep_key_t* keys;
    size_t       keys_len;
} wsrep_key_arr_t;
bool wsrep_prepare_keys_for_isolation(THD*              thd,
                                      const char*       db,
                                      const char*       table,
                                      const TABLE_LIST* table_list,
                                      wsrep_key_arr_t*  ka);
void wsrep_keys_free(wsrep_key_arr_t* key_arr);

#else /* WITH_WSREP */

#define WSREP(T)  (0)
#define WSREP_ON  (0)
#define WSREP_EMULATE_BINLOG(thd) (0)
#define WSREP_CLIENT(thd) (0)
#define WSREP_FORMAT(my_format) ((ulong)my_format)
#define WSREP_PROVIDER_EXISTS (0)
#define wsrep_emulate_bin_log (0)
#define wsrep_xid_seqno(X) (0)
#define wsrep_to_isolation (0)
#define wsrep_init() (1)
#define wsrep_prepend_PATH(X)
#define wsrep_before_SE() (0)
#define wsrep_init_startup(X)
#define wsrep_must_sync_wait(...) (0)
#define wsrep_sync_wait(...) (0)
#define wsrep_to_isolation_begin(...) (0)
#define wsrep_register_hton(...) do { } while(0)
#define wsrep_check_opts() (0)
#define wsrep_stop_replication(X) do { } while(0)
#define wsrep_inited (0)
#define wsrep_deinit(X) do { } while(0)
#define wsrep_recover() do { } while(0)
#define wsrep_slave_threads (1)
#define wsrep_replicate_myisam (0)
#define wsrep_thr_init() do {} while(0)
#define wsrep_thr_deinit() do {} while(0)
#define wsrep_running_threads (0)
#endif /* WITH_WSREP */
#endif /* WSREP_MYSQLD_H */
