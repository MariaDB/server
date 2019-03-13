/* Copyright 2008-2017 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#ifndef WSREP_MYSQLD_H
#define WSREP_MYSQLD_H

#include <wsrep.h>

#ifdef WITH_WSREP

#include <mysql/plugin.h>
#include "mysql/service_wsrep.h"

#include <my_global.h>
#include <my_pthread.h>
#include "log.h"
#include "mysqld.h"

typedef struct st_mysql_show_var SHOW_VAR;
#include <sql_priv.h>
#include "mdl.h"
#include "sql_table.h"
#include "wsrep_mysqld_c.h"

#include "wsrep/provider.hpp"
#include "wsrep/streaming_context.hpp"
#include "wsrep_api.h"
#include <vector>
#include "wsrep_server_state.h"

#define WSREP_UNDEFINED_TRX_ID ULONGLONG_MAX

class set_var;
class THD;

enum wsrep_consistency_check_mode {
    NO_CONSISTENCY_CHECK,
    CONSISTENCY_CHECK_DECLARED,
    CONSISTENCY_CHECK_RUNNING,
};

// Global wsrep parameters

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
extern ulong       wsrep_debug;
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
extern long int    wsrep_protocol_version;
extern ulong       wsrep_forced_binlog_format;
extern my_bool     wsrep_desync;
extern ulong       wsrep_reject_queries;
extern my_bool     wsrep_recovery;
extern my_bool     wsrep_replicate_myisam;
extern my_bool     wsrep_log_conflicts;
extern ulong       wsrep_mysql_replication_bundle;
extern my_bool     wsrep_load_data_splitting;
extern my_bool     wsrep_restart_slave;
extern my_bool     wsrep_restart_slave_activated;
extern my_bool     wsrep_slave_FK_checks;
extern my_bool     wsrep_slave_UK_checks;
extern ulong       wsrep_trx_fragment_unit;
extern ulong       wsrep_SR_store_type;
extern uint        wsrep_ignore_apply_errors;
extern ulong       wsrep_running_threads;
extern bool        wsrep_new_cluster;
extern bool        wsrep_gtid_mode;
extern uint32      wsrep_gtid_domain_id;

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
    WSREP_SYNC_WAIT_NONE= 0x0,
    // select, begin
    WSREP_SYNC_WAIT_BEFORE_READ= 0x1,
    WSREP_SYNC_WAIT_BEFORE_UPDATE_DELETE= 0x2,
    WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE= 0x4,
    WSREP_SYNC_WAIT_BEFORE_SHOW= 0x8,
    WSREP_SYNC_WAIT_MAX= 0xF
};

enum enum_wsrep_ignore_apply_error {
    WSREP_IGNORE_ERRORS_NONE= 0x0,
    WSREP_IGNORE_ERRORS_ON_RECONCILING_DDL= 0x1,
    WSREP_IGNORE_ERRORS_ON_RECONCILING_DML= 0x2,
    WSREP_IGNORE_ERRORS_ON_DDL= 0x4,
    WSREP_IGNORE_ERRORS_MAX= 0x7
};

// Streaming Replication
#define WSREP_FRAG_BYTES      0
#define WSREP_FRAG_ROWS       1
#define WSREP_FRAG_STATEMENTS 2

#define WSREP_SR_STORE_NONE   0
#define WSREP_SR_STORE_TABLE  1

extern const char *wsrep_fragment_units[];
extern const char *wsrep_SR_store_types[];

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
extern char*       wsrep_provider_capabilities;
extern char*       wsrep_cluster_capabilities;

int  wsrep_show_status(THD *thd, SHOW_VAR *var, char *buff);
int  wsrep_show_ready(THD *thd, SHOW_VAR *var, char *buff);
void wsrep_free_status(THD *thd);
void wsrep_update_cluster_state_uuid(const char* str);

/* Filters out --wsrep-new-cluster oprtion from argv[]
 * should be called in the very beginning of main() */
void wsrep_filter_new_cluster (int* argc, char* argv[]);

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

/* Recover streaming transactions from fragment storage */
void wsrep_recover_sr_from_storage(THD *);

// Other wsrep global variables
extern my_bool     wsrep_inited; // whether wsrep is initialized ?

extern "C" void wsrep_fire_rollbacker(THD *thd);
extern "C" uint32 wsrep_thd_wsrep_rand(THD *thd);
extern "C" time_t wsrep_thd_query_start(THD *thd);
extern void wsrep_close_client_connections(my_bool wait_to_end,
                                           THD *except_caller_thd= NULL);
extern "C" query_id_t wsrep_thd_wsrep_last_query_id(THD *thd);
extern "C" void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id);

extern int  wsrep_wait_committing_connections_close(int wait_time);
extern void wsrep_close_applier(THD *thd);
extern void wsrep_wait_appliers_close(THD *thd);
extern void wsrep_close_applier_threads(int count);


/* new defines */
extern void wsrep_stop_replication(THD *thd);
extern bool wsrep_start_replication();
extern void wsrep_shutdown_replication();
extern bool wsrep_must_sync_wait (THD* thd, uint mask= WSREP_SYNC_WAIT_BEFORE_READ);
extern bool wsrep_sync_wait (THD* thd, uint mask= WSREP_SYNC_WAIT_BEFORE_READ);
extern enum wsrep::provider::status
wsrep_sync_wait_upto (THD* thd, wsrep_gtid_t* upto, int timeout);
extern void wsrep_last_committed_id (wsrep_gtid_t* gtid);
extern int  wsrep_check_opts();
extern void wsrep_prepend_PATH (const char* path);

/* Other global variables */
extern wsrep_seqno_t wsrep_locked_seqno;
#define WSREP_ON                         \
  ((global_system_variables.wsrep_on) && \
   wsrep_provider                     && \
   strcmp(wsrep_provider, WSREP_NONE))

/* use xxxxxx_NNULL macros when thd pointer is guaranteed to be non-null to
 * avoid compiler warnings (GCC 6 and later) */
#define WSREP_NNULL(thd) \
  (WSREP_ON && thd->variables.wsrep_on)

#define WSREP(thd) \
  (thd && WSREP_NNULL(thd))

#define WSREP_CLIENT_NNULL(thd) \
  (WSREP_NNULL(thd) && thd->wsrep_client_thread)

#define WSREP_CLIENT(thd) \
    (WSREP(thd) && thd->wsrep_client_thread)

#define WSREP_EMULATE_BINLOG_NNULL(thd) \
  (WSREP_NNULL(thd) && wsrep_emulate_bin_log)

#define WSREP_EMULATE_BINLOG(thd) \
  (WSREP(thd) && wsrep_emulate_bin_log)

#define WSREP_BINLOG_FORMAT(my_format)                         \
   ((wsrep_forced_binlog_format != BINLOG_FORMAT_UNSPEC) ?     \
   wsrep_forced_binlog_format : my_format)

// prefix all messages with "WSREP"
#define WSREP_LOG(fun, ...)                                       \
    do {                                                          \
        char msg[1024]= {'\0'};                                  \
        snprintf(msg, sizeof(msg) - 1, ## __VA_ARGS__);           \
        fun("WSREP: %s", msg);                                    \
    } while(0)

#define WSREP_DEBUG(...)                                                \
    if (wsrep_debug)     WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_INFO(...)  WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_WARN(...)  WSREP_LOG(sql_print_warning,     ##__VA_ARGS__)
#define WSREP_ERROR(...) WSREP_LOG(sql_print_error,       ##__VA_ARGS__)

#define WSREP_LOG_CONFLICT_THD(thd, role)                               \
  WSREP_LOG(sql_print_information,                                      \
            "%s: \n "                                                   \
            "  THD: %lu, mode: %s, state: %s, conflict: %s, seqno: %lld\n " \
            "  SQL: %s",                                                \
            role,                                                       \
            thd_get_thread_id(thd),                                     \
            wsrep_thd_client_mode_str(thd),                             \
            wsrep_thd_client_state_str(thd),                            \
            wsrep_thd_transaction_state_str(thd),                       \
            wsrep_thd_trx_seqno(thd),                                   \
            wsrep_thd_query(thd)                                        \
            );

#define WSREP_LOG_CONFLICT(bf_thd, victim_thd, bf_abort)                \
  if (wsrep_debug || wsrep_log_conflicts)                               \
  {                                                                     \
    WSREP_LOG(sql_print_information, "cluster conflict due to %s for threads:", \
              (bf_abort) ? "high priority abort" : "certification failure" \
              );                                                        \
    if (bf_thd)     WSREP_LOG_CONFLICT_THD(bf_thd, "Winning thread");   \
    if (victim_thd) WSREP_LOG_CONFLICT_THD(victim_thd, "Victim thread"); \
    WSREP_LOG(sql_print_information, "context: %s:%d", __FILE__, __LINE__); \
  }

#define WSREP_PROVIDER_EXISTS                                                  \
  (wsrep_provider && strncasecmp(wsrep_provider, WSREP_NONE, FN_REFLEN))

#define WSREP_QUERY(thd) (thd->query())

extern my_bool wsrep_ready_get();
extern void wsrep_ready_wait();

class Ha_trx_info;
struct THD_TRANS;

extern mysql_mutex_t LOCK_wsrep_ready;
extern mysql_cond_t  COND_wsrep_ready;
extern mysql_mutex_t LOCK_wsrep_sst;
extern mysql_cond_t  COND_wsrep_sst;
extern mysql_mutex_t LOCK_wsrep_sst_init;
extern mysql_cond_t  COND_wsrep_sst_init;
extern int wsrep_replaying;
extern mysql_mutex_t LOCK_wsrep_replaying;
extern mysql_cond_t  COND_wsrep_replaying;
extern mysql_mutex_t LOCK_wsrep_slave_threads;
extern mysql_cond_t  COND_wsrep_slave_threads;
extern mysql_mutex_t LOCK_wsrep_cluster_config;
extern mysql_mutex_t LOCK_wsrep_desync;
extern mysql_mutex_t LOCK_wsrep_SR_pool;
extern mysql_mutex_t LOCK_wsrep_SR_store;
extern mysql_mutex_t LOCK_wsrep_config_state;
extern mysql_mutex_t LOCK_wsrep_group_commit;
extern my_bool       wsrep_emulate_bin_log;
extern int           wsrep_to_isolation;
#ifdef GTID_SUPPORT
extern rpl_sidno     wsrep_sidno;
#endif /* GTID_SUPPORT */
extern my_bool       wsrep_preordered_opt;

#ifdef HAVE_PSI_INTERFACE

extern PSI_cond_key  key_COND_wsrep_thd;

extern PSI_mutex_key key_LOCK_wsrep_ready;
extern PSI_mutex_key key_COND_wsrep_ready;
extern PSI_mutex_key key_LOCK_wsrep_sst;
extern PSI_cond_key  key_COND_wsrep_sst;
extern PSI_mutex_key key_LOCK_wsrep_sst_init;
extern PSI_cond_key  key_COND_wsrep_sst_init;
extern PSI_mutex_key key_LOCK_wsrep_sst_thread;
extern PSI_cond_key  key_COND_wsrep_sst_thread;
extern PSI_mutex_key key_LOCK_wsrep_replaying;
extern PSI_cond_key  key_COND_wsrep_replaying;
extern PSI_mutex_key key_LOCK_wsrep_slave_threads;
extern PSI_cond_key  key_COND_wsrep_slave_threads;
extern PSI_mutex_key key_LOCK_wsrep_cluster_config;
extern PSI_mutex_key key_LOCK_wsrep_desync;
extern PSI_mutex_key key_LOCK_wsrep_SR_pool;
extern PSI_mutex_key key_LOCK_wsrep_SR_store;
extern PSI_mutex_key key_LOCK_wsrep_global_seqno;
extern PSI_mutex_key key_LOCK_wsrep_thd_queue;
extern PSI_cond_key  key_COND_wsrep_thd_queue;

extern PSI_file_key key_file_wsrep_gra_log;
#endif /* HAVE_PSI_INTERFACE */
struct TABLE_LIST;
class Alter_info;
int wsrep_to_isolation_begin(THD *thd, const char *db_, const char *table_,
                             const TABLE_LIST* table_list,
                             Alter_info* alter_info= NULL);

void wsrep_to_isolation_end(THD *thd);

bool wsrep_append_SR_keys(THD *thd);
int wsrep_to_buf_helper(
  THD* thd, const char *query, uint query_len, uchar** buf, size_t* buf_len);
int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_create_event_query(THD *thd, uchar** buf, size_t* buf_len);

bool wsrep_stmt_rollback_is_safe(THD* thd);

void wsrep_init_sidno(const wsrep_uuid_t&);
bool wsrep_node_is_donor();
bool wsrep_node_is_synced();

void wsrep_init_SR();
void wsrep_verify_SE_checkpoint(const wsrep_uuid_t& uuid, wsrep_seqno_t seqno);
int wsrep_replay_from_SR_store(THD*, const wsrep_trx_meta_t&);
void wsrep_node_uuid(wsrep_uuid_t&);

class Log_event;
int wsrep_ignored_error_code(Log_event* ev, int error);
int wsrep_must_ignore_error(THD* thd);

bool wsrep_replicate_GTID(THD* thd);

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

extern void
wsrep_handle_mdl_conflict(MDL_context *requestor_ctx,
                          MDL_ticket *ticket,
                          const MDL_key *key);
IO_CACHE * get_trans_log(THD * thd);
bool wsrep_trans_cache_is_empty(THD *thd);
void thd_binlog_flush_pending_rows_event(THD *thd, bool stmt_end);
void thd_binlog_rollback_stmt(THD * thd);
void thd_binlog_trx_reset(THD * thd);

typedef void (*wsrep_thd_processor_fun)(THD*, void *);
class Wsrep_thd_args
{
 public:
 Wsrep_thd_args(wsrep_thd_processor_fun fun, void* args)
   :
  fun_ (fun),
  args_(args)
  { }

  wsrep_thd_processor_fun fun() { return fun_; }

  void* args() { return args_; }

 private:

  Wsrep_thd_args(const Wsrep_thd_args&);
  Wsrep_thd_args& operator=(const Wsrep_thd_args&);

  wsrep_thd_processor_fun fun_;
  void*                    args_;
};

void* start_wsrep_THD(void*);

void wsrep_close_threads(THD *thd);
bool wsrep_is_show_query(enum enum_sql_command command);
void wsrep_replay_transaction(THD *thd);
bool wsrep_create_like_table(THD* thd, TABLE_LIST* table,
                             TABLE_LIST* src_table,
                             HA_CREATE_INFO *create_info);
bool wsrep_node_is_donor();
bool wsrep_node_is_synced();

/**
 * Check if the wsrep provider (ie the Galera library) is capable of
 * doing streaming replication.
 * @return true if SR capable
 */
bool wsrep_provider_is_SR_capable();

/**
 * Initialize WSREP server instance.
 *
 * @return Zero on success, non-zero on error.
 */
int wsrep_init_server();

/**
 * Initialize WSREP globals. This should be done after server initialization
 * is complete and the server has joined to the cluster.
 *
 */
void wsrep_init_globals();

/**
 * Deinit and release WSREP resources.
 */
void wsrep_deinit_server();

/**
 * Convert streaming fragment unit (WSREP_FRAG_BYTES, WSREP_FRAG_ROWS...)
 * to corresponding wsrep-lib fragment_unit
 */
enum wsrep::streaming_context::fragment_unit wsrep_fragment_unit(ulong unit);

#else /* !WITH_WSREP */

/* These macros are needed to compile MariaDB without WSREP support
 * (e.g. embedded) */

#define WSREP(T)  (0)
#define WSREP_ON  (0)
#define WSREP_EMULATE_BINLOG(thd) (0)
#define WSREP_EMULATE_BINLOG_NNULL(thd) (0)
#define WSREP_BINLOG_FORMAT(my_format) ((ulong)my_format)
#define WSREP_PROVIDER_EXISTS (0)
#define wsrep_emulate_bin_log (0)
#define wsrep_to_isolation (0)
#define wsrep_before_SE() (0)
#define wsrep_init_startup(X)
#define wsrep_check_opts() (0)
#define wsrep_thr_init() do {} while(0)
#define wsrep_thr_deinit() do {} while(0)
#define wsrep_init_globals() do {} while(0)
#define wsrep_create_appliers(X) do {} while(0)

#endif /* WITH_WSREP */

#endif /* WSREP_MYSQLD_H */
