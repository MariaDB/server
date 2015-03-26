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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef WSREP_MYSQLD_H
#define WSREP_MYSQLD_H

#include "mysqld.h"
typedef struct st_mysql_show_var SHOW_VAR;
#include <sql_priv.h>
//#include "rpl_gtid.h"
#include "../wsrep/wsrep_api.h"

#define WSREP_UNDEFINED_TRX_ID ULONGLONG_MAX

class set_var;
class THD;

enum wsrep_exec_mode {
  /* Transaction processing before replication. */
  LOCAL_STATE,
  /* Slave thread applying write sets from other nodes or replaying thread. */
  REPL_RECV,
  /* Total-order-isolation mode */
  TOTAL_ORDER,
  /*
    Transaction procession after it has been replicated in prepare stage and
    has passed certification
  */
  LOCAL_COMMIT
};

enum wsrep_query_state {
    QUERY_IDLE,
    QUERY_EXEC,
    QUERY_COMMITTING,
    QUERY_EXITING,
    QUERY_ROLLINGBACK,
};

enum wsrep_conflict_state {
    NO_CONFLICT,
    MUST_ABORT,
    ABORTING,
    ABORTED,
    MUST_REPLAY,
    REPLAYING,
    RETRY_AUTOCOMMIT,
    CERT_FAILURE,
};

enum wsrep_consistency_check_mode {
    NO_CONSISTENCY_CHECK,
    CONSISTENCY_CHECK_DECLARED,
    CONSISTENCY_CHECK_RUNNING,
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
extern MYSQL_PLUGIN_IMPORT my_bool wsrep_debug;
extern my_bool     wsrep_convert_LOCK_to_trx;
extern ulong       wsrep_retry_autocommit;
extern my_bool     wsrep_auto_increment_control;
extern my_bool     wsrep_drupal_282555_workaround;
extern my_bool     wsrep_incremental_data_collection;
extern const char* wsrep_start_position;
extern ulong       wsrep_max_ws_size;
extern ulong       wsrep_max_ws_rows;
extern const char* wsrep_notify_cmd;
extern my_bool     wsrep_certify_nonPK;
extern long        wsrep_max_protocol_version;
extern long        wsrep_protocol_version;
extern ulong       wsrep_forced_binlog_format;
extern ulong       wsrep_OSU_method_options;
extern my_bool     wsrep_desync;
extern my_bool     wsrep_recovery;
extern my_bool     wsrep_replicate_myisam;
extern my_bool     wsrep_log_conflicts;
extern ulong       wsrep_mysql_replication_bundle;
extern my_bool     wsrep_load_data_splitting;
extern my_bool     wsrep_restart_slave;
extern my_bool     wsrep_restart_slave_activated;
extern my_bool     wsrep_slave_FK_checks;
extern my_bool     wsrep_slave_UK_checks;
extern bool        wsrep_new_cluster;           // bootstrap the cluster ?

enum enum_wsrep_OSU_method { WSREP_OSU_TOI, WSREP_OSU_RSU };
enum enum_wsrep_sync_wait {
    WSREP_SYNC_WAIT_NONE = 0x0,
    // show, select, begin
    WSREP_SYNC_WAIT_BEFORE_READ = 0x1,
    WSREP_SYNC_WAIT_BEFORE_UPDATE_DELETE = 0x2,
    WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE = 0x4,
    WSREP_SYNC_WAIT_MAX = 0x7
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

int  wsrep_show_status(THD *thd, SHOW_VAR *var, char *buff);
void wsrep_free_status(THD *thd);

int  wsrep_init();
void wsrep_deinit(bool free_options);
void wsrep_recover();
bool wsrep_before_SE(); // initialize wsrep before storage
                        // engines (true) or after (false)
/* wsrep initialization sequence at startup
 * @param before wsrep_before_SE() value */
void wsrep_init_startup(bool before);

// Other wsrep global variables
extern my_bool     wsrep_inited; // whether wsrep is initialized ?

extern "C" enum wsrep_exec_mode wsrep_thd_exec_mode(THD *thd);
extern "C" enum wsrep_conflict_state wsrep_thd_conflict_state(THD *thd);
extern "C" enum wsrep_query_state wsrep_thd_query_state(THD *thd);
extern "C" const char * wsrep_thd_exec_mode_str(THD *thd);
extern "C" const char * wsrep_thd_conflict_state_str(THD *thd);
extern "C" const char * wsrep_thd_query_state_str(THD *thd);
extern "C" wsrep_ws_handle_t* wsrep_thd_ws_handle(THD *thd);

extern "C" void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode);
extern "C" void wsrep_thd_set_query_state(
        THD *thd, enum wsrep_query_state state);
extern "C" void wsrep_thd_set_conflict_state(
        THD *thd, enum wsrep_conflict_state state);

extern "C" void wsrep_thd_set_trx_to_replay(THD *thd, uint64 trx_id);

extern "C" void wsrep_thd_LOCK(THD *thd);
extern "C" void wsrep_thd_UNLOCK(THD *thd);
extern "C" uint32 wsrep_thd_wsrep_rand(THD *thd);
extern "C" time_t wsrep_thd_query_start(THD *thd);
extern "C" my_thread_id wsrep_thd_thread_id(THD *thd);
extern "C" int64_t wsrep_thd_trx_seqno(THD *thd);
extern "C" query_id_t wsrep_thd_query_id(THD *thd);
extern "C" char * wsrep_thd_query(THD *thd);
extern "C" query_id_t wsrep_thd_wsrep_last_query_id(THD *thd);
extern "C" void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id);
extern "C" void wsrep_thd_awake(THD *thd, my_bool signal);
extern "C" int wsrep_thd_retry_counter(THD *thd);


extern void wsrep_close_client_connections(my_bool wait_to_end);
extern int  wsrep_wait_committing_connections_close(int wait_time);
extern void wsrep_close_applier(THD *thd);
extern void wsrep_wait_appliers_close(THD *thd);
extern void wsrep_close_applier_threads(int count);
extern void wsrep_kill_mysql(THD *thd);

/* new defines */
extern void wsrep_stop_replication(THD *thd);
extern bool wsrep_start_replication();
extern bool wsrep_sync_wait (THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern int  wsrep_check_opts (int argc, char* const* argv);
extern void wsrep_prepend_PATH (const char* path);
/* some inline functions are defined in wsrep_mysqld_inl.h */

/* Other global variables */
extern wsrep_seqno_t wsrep_locked_seqno;

#define WSREP_ON \
  (global_system_variables.wsrep_on)

#define WSREP(thd) \
  (WSREP_ON && wsrep && (thd && thd->variables.wsrep_on))

#define WSREP_CLIENT(thd) \
    (WSREP(thd) && thd->wsrep_client_thread)

#define WSREP_EMULATE_BINLOG(thd) \
  (WSREP(thd) && wsrep_emulate_bin_log)

// MySQL logging functions don't seem to understand long long length modifer.
// This is a workaround. It also prefixes all messages with "WSREP"
#define WSREP_LOG(fun, ...)                                       \
    {                                                             \
        char msg[1024] = {'\0'};                                  \
        snprintf(msg, sizeof(msg) - 1, ## __VA_ARGS__);           \
        fun("WSREP: %s", msg);                                    \
    }

#define WSREP_DEBUG(...)                                                \
    if (wsrep_debug)     WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_INFO(...)  WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_WARN(...)  WSREP_LOG(sql_print_warning,     ##__VA_ARGS__)
#define WSREP_ERROR(...) WSREP_LOG(sql_print_error,       ##__VA_ARGS__)

#define WSREP_LOG_CONFLICT_THD(thd, role)                                      \
    WSREP_LOG(sql_print_information, 	                                       \
      "%s: \n "       	                                                       \
      "  THD: %lu, mode: %s, state: %s, conflict: %s, seqno: %lld\n "          \
      "  SQL: %s",							       \
      role, wsrep_thd_thread_id(thd), wsrep_thd_exec_mode_str(thd),            \
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

extern void wsrep_ready_wait();

enum wsrep_trx_status {
    WSREP_TRX_OK,
    WSREP_TRX_CERT_FAIL,      /* certification failure, must abort */
    WSREP_TRX_SIZE_EXCEEDED,  /* trx size exceeded */
    WSREP_TRX_ERROR,          /* native mysql error */
};

extern enum wsrep_trx_status
wsrep_run_wsrep_commit(THD *thd, handlerton *hton, bool all);
class Ha_trx_info;
struct THD_TRANS;
void wsrep_register_hton(THD* thd, bool all);
void wsrep_post_commit(THD* thd, bool all);
void wsrep_brute_force_killer(THD *thd);
int  wsrep_hire_brute_force_killer(THD *thd, uint64_t trx_id);

extern "C" bool wsrep_consistency_check(void *thd_ptr);

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
extern wsrep_aborting_thd_t wsrep_aborting_thd;
extern my_bool       wsrep_emulate_bin_log;
extern int           wsrep_to_isolation;
#ifdef GTID_SUPPORT
extern rpl_sidno     wsrep_sidno;
#endif /* GTID_SUPPORT */
extern my_bool       wsrep_preordered_opt;

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
#endif /* HAVE_PSI_INTERFACE */
struct TABLE_LIST;
int wsrep_to_isolation_begin(THD *thd, char *db_, char *table_,
                             const TABLE_LIST* table_list);
void wsrep_to_isolation_end(THD *thd);
void wsrep_cleanup_transaction(THD *thd);
int wsrep_to_buf_helper(
  THD* thd, const char *query, uint query_len, uchar** buf, size_t* buf_len);
int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_create_event_query(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_alter_event_query(THD *thd, uchar** buf, size_t* buf_len);

#ifdef GTID_SUPPORT
void wsrep_init_sidno(const wsrep_uuid_t&);
#endif /* GTID_SUPPORT */

#endif /* WSREP_MYSQLD_H */
