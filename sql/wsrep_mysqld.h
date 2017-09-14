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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <wsrep.h>

#ifndef WSREP_MYSQLD_H
#define WSREP_MYSQLD_H

#include <mysql/plugin.h>
//#include <mysql/service_wsrep.h>

#include <my_global.h>
#include <my_pthread.h>

#ifdef WITH_WSREP

typedef struct st_mysql_show_var SHOW_VAR;
#include <sql_priv.h>
//#include "rpl_gtid.h"
#include "../wsrep/wsrep_api.h"
#include "mdl.h"
#include "mysqld.h"
#include "log.h"
#include "sql_table.h"

#define WSREP_UNDEFINED_TRX_ID ULONGLONG_MAX

class set_var;
class THD;

#define SEPPO_ADDED
#ifdef SEPPO_ADDED
enum wsrep_exec_mode {
    LOCAL_STATE,
    REPL_RECV,
    TOTAL_ORDER,
    LOCAL_COMMIT,
    LOCAL_ROLLBACK
};

enum wsrep_query_state {
    QUERY_IDLE,
    QUERY_EXEC,
    QUERY_COMMITTING,
    QUERY_EXITING
};

enum wsrep_conflict_state {
    NO_CONFLICT,
    MUST_ABORT,
    ABORTING,
    ABORTED,
    MUST_REPLAY,
    REPLAYING,
    RETRY_AUTOCOMMIT,
    CERT_FAILURE
};

#endif

enum wsrep_consistency_check_mode {
    NO_CONSISTENCY_CHECK,
    CONSISTENCY_CHECK_DECLARED,
    CONSISTENCY_CHECK_RUNNING,
};

#ifdef OLD_MARIADB
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
#endif
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
//extern int        wsrep_protocol_version;
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
extern ulong       wsrep_trx_fragment_size;
extern ulong       wsrep_trx_fragment_unit;
extern ulong       wsrep_SR_store_type;
extern uint        wsrep_ignore_apply_errors;
extern ulong       wsrep_running_threads;
extern bool        wsrep_new_cluster;
extern bool        wsrep_gtid_mode;
extern uint32      wsrep_gtid_domain_id;

enum enum_wsrep_OSU_method {
    WSREP_OSU_TOI,
    WSREP_OSU_RSU,
    WSREP_OSU_NBO,
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

enum enum_wsrep_ignore_apply_error {
    WSREP_IGNORE_ERRORS_NONE = 0x0,
    WSREP_IGNORE_ERRORS_ON_RECONCILING_DDL = 0x1,
    WSREP_IGNORE_ERRORS_ON_RECONCILING_DML = 0x2,
    WSREP_IGNORE_ERRORS_ON_DDL = 0x4,
    WSREP_IGNORE_ERRORS_MAX = 0x7
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

//int  wsrep_show_status(THD *thd, SHOW_VAR *var, char *buff,
//                       enum enum_var_type scope);
int  wsrep_show_status(THD *thd, SHOW_VAR *var, char *buff);
void wsrep_free_status(THD *thd);
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

extern "C" {
  enum wsrep_exec_mode wsrep_thd_exec_mode(THD *thd);
  enum wsrep_conflict_state wsrep_thd_conflict_state(THD *thd);
  void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode);
  void wsrep_thd_set_conflict_state(
        THD *thd, enum wsrep_conflict_state state);
//void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode);
//void wsrep_thd_set_conflict_state(
//         THD *thd, enum wsrep_conflict_state state);

enum wsrep_query_state wsrep_thd_query_state(THD *thd);

const char * wsrep_thd_exec_mode_str(THD *thd);
const char * wsrep_thd_conflict_state_str(THD *thd);
const char * wsrep_thd_query_state_str(THD *thd);
wsrep_ws_handle_t* wsrep_thd_ws_handle(THD *thd);
}
#ifdef OUT
void wsrep_thd_set_trx_to_replay(THD *thd, uint64 trx_id);

void wsrep_fire_rollbacker(THD *thd);
uint32 wsrep_thd_wsrep_rand(THD *thd);
time_t wsrep_thd_query_start(THD *thd);
my_thread_id wsrep_thd_thread_id(THD *thd);
int64_t wsrep_thd_trx_seqno(const THD *thd);
query_id_t wsrep_thd_query_id(THD *thd);
wsrep_trx_id_t wsrep_thd_next_trx_id(THD *thd);
wsrep_trx_id_t wsrep_thd_trx_id(THD *thd);
query_id_t wsrep_thd_wsrep_last_query_id(THD *thd);
void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id);

void wsrep_thd_last_written_gtid(THD *thd, wsrep_gtid_t *t);
ulong wsrep_thd_trx_fragment_size(THD *thd);

void wsrep_thd_xid(const void *thd_ptr, void *xid, size_t xid_size);

extern int  wsrep_wait_committing_connections_close(int wait_time);
extern void wsrep_close_applier(THD *thd);
extern void wsrep_wait_appliers_close(THD *thd);
extern void wsrep_close_applier_threads(int count);
extern void wsrep_kill_mysql(THD *thd);


/* new defines */
extern void wsrep_stop_replication(THD *thd);
extern bool wsrep_start_replication();
extern void wsrep_shutdown_replication();
extern bool wsrep_must_sync_wait(THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern bool wsrep_sync_wait(THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern wsrep_status_t wsrep_sync_wait_upto (THD* thd, wsrep_gtid_t* upto, int timeout);
extern void wsrep_last_committed_id (wsrep_gtid_t* gtid);
extern int  wsrep_check_opts();
extern void wsrep_prepend_PATH (const char* path);


#else
extern "C" void wsrep_thd_set_trx_to_replay(THD *thd, uint64 trx_id);

extern "C" void wsrep_fire_rollbacker(THD *thd);
extern "C" uint32 wsrep_thd_wsrep_rand(THD *thd);
extern "C" time_t wsrep_thd_query_start(THD *thd);
extern "C" my_thread_id wsrep_thd_thread_id(THD *thd);
extern "C" int64_t wsrep_thd_trx_seqno(const THD *thd);
extern "C" query_id_t wsrep_thd_query_id(THD *thd);
extern "C" wsrep_trx_id_t wsrep_thd_next_trx_id(THD *thd);
extern "C" wsrep_trx_id_t wsrep_thd_trx_id(THD *thd);
extern "C" char * wsrep_thd_query(THD *thd);
extern "C" query_id_t wsrep_thd_wsrep_last_query_id(THD *thd);
extern "C" void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id);

extern "C" void wsrep_thd_last_written_gtid(THD *thd, wsrep_gtid_t *t);
extern "C" ulong wsrep_thd_trx_fragment_size(THD *thd);
extern void wsrep_thd_xid(const void *thd_ptr, void *xid, size_t xid_size);

extern int  wsrep_wait_committing_connections_close(int wait_time);
extern void wsrep_close_applier(THD *thd);
extern void wsrep_wait_appliers_close(THD *thd);
extern void wsrep_close_applier_threads(int count);
extern void wsrep_kill_mysql(THD *thd);


/* new defines */
extern void wsrep_stop_replication(THD *thd);
extern bool wsrep_start_replication();
extern void wsrep_shutdown_replication();
extern bool wsrep_must_sync_wait(THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern bool wsrep_sync_wait(THD* thd, uint mask = WSREP_SYNC_WAIT_BEFORE_READ);
extern wsrep_status_t wsrep_sync_wait_upto (THD* thd, wsrep_gtid_t* upto, int timeout);
extern void wsrep_last_committed_id (wsrep_gtid_t* gtid);
extern int  wsrep_check_opts();
extern void wsrep_prepend_PATH (const char* path);
#endif


/* Other global variables */
extern wsrep_seqno_t wsrep_locked_seqno;

/* use xxxxxx_NNULL macros when thd pointer is guaranteed to be non-null to
 * avoid compiler warnings (GCC 6 and later) */
#define WSREP_NNULL(thd) \
  (WSREP_ON && wsrep && thd->variables.wsrep_on)

#define WSREP_ON                         \
  (global_system_variables.wsrep_on)

#define WSREP_ON_NEW                     \
  ((global_system_variables.wsrep_on) && \
   wsrep_provider                     && \
   strcmp(wsrep_provider, WSREP_NONE))

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

#define WSREP_FORMAT(my_format)                           \
  ((wsrep_forced_binlog_format != BINLOG_FORMAT_UNSPEC)   \
    ? wsrep_forced_binlog_format : (ulong)(my_format))

// prefix all messages with "WSREP"
#define WSREP_LOG(fun, ...)                                       \
    do {                                                          \
        char msg[1024] = {'\0'};                                  \
        snprintf(msg, sizeof(msg) - 1, ## __VA_ARGS__);           \
        fun("WSREP: %s", msg);                                    \
    } while(0)

#define WSREP_DEBUG(...)                                                \
    if (wsrep_debug)     WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_INFO(...)  WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_WARN(...)  WSREP_LOG(sql_print_warning,     ##__VA_ARGS__)
#define WSREP_ERROR(...) WSREP_LOG(sql_print_error,       ##__VA_ARGS__)


#define WSREP_LOG_CONFLICT_THD(thd, role)                                      \
    WSREP_LOG(sql_print_information, 	                                       \
      "%s: \n "       	                                                       \
      "  THD: %lld, mode: %s, state: %s, conflict: %s, seqno: %lld\n "         \
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
    WSREP_LOG(sql_print_information, "context: %s:%d", __FILE__, __LINE__);    \
  }

#define WSREP_PROVIDER_EXISTS                                                  \
  (wsrep_provider && strncasecmp(wsrep_provider, WSREP_NONE, FN_REFLEN))

#define WSREP_QUERY(thd) (thd->query())

extern void wsrep_ready_wait();

#ifndef OUT
enum wsrep_trx_status {
    WSREP_TRX_OK,
    WSREP_TRX_CERT_FAIL,      /* certification failure, must abort */
    WSREP_TRX_SIZE_EXCEEDED,  /* trx size exceeded */
    WSREP_TRX_ERROR,          /* native mysql error */
};
#endif
static inline
wsrep_status_t wsrep_trx_status_to_wsrep_status(wsrep_trx_status status)
{
  switch (status)
  {
  case WSREP_TRX_OK:
    return WSREP_OK;
  case WSREP_TRX_CERT_FAIL:
  case WSREP_TRX_ERROR:
    return WSREP_TRX_FAIL;
  case WSREP_TRX_SIZE_EXCEEDED:
    return WSREP_SIZE_EXCEEDED;
  }
  return WSREP_NOT_IMPLEMENTED;
}

class Ha_trx_info;
struct THD_TRANS;
void wsrep_register_hton(THD* thd, bool all);
void wsrep_register_hton(THD* thd, bool all, bool force);
void wsrep_post_commit(THD* thd, bool all);
void wsrep_post_rollback(THD* thd);
void wsrep_brute_force_killer(THD *thd);
int  wsrep_hire_brute_force_killer(THD *thd, uint64_t trx_id);

//extern "C" bool wsrep_consistency_check(void *thd_ptr);

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
extern mysql_mutex_t LOCK_wsrep_desync;
extern mysql_mutex_t LOCK_wsrep_SR_pool;
extern mysql_mutex_t LOCK_wsrep_SR_store;
extern mysql_mutex_t LOCK_wsrep_thd_pool;
extern mysql_mutex_t LOCK_wsrep_config_state;
extern my_bool       wsrep_emulate_bin_log;
extern int           wsrep_to_isolation;
#ifdef GTID_SUPPORT
extern rpl_sidno     wsrep_sidno;
#endif /* GTID_SUPPORT */
extern my_bool       wsrep_preordered_opt;
extern handlerton    *wsrep_hton;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_LOCK_wsrep_thd;
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
extern PSI_mutex_key key_LOCK_wsrep_desync;
extern PSI_mutex_key key_LOCK_wsrep_SR_pool;
extern PSI_mutex_key key_LOCK_wsrep_SR_store;
extern PSI_mutex_key key_LOCK_wsrep_thd_pool;
extern PSI_mutex_key key_LOCK_wsrep_nbo;
extern PSI_cond_key  key_COND_wsrep_nbo;
extern PSI_mutex_key key_LOCK_wsrep_global_seqno;
extern PSI_mutex_key key_LOCK_wsrep_thd_queue;
extern PSI_cond_key  key_COND_wsrep_thd_queue;

extern PSI_file_key key_file_wsrep_gra_log;
#endif /* HAVE_PSI_INTERFACE */
struct TABLE_LIST;
int wsrep_to_isolation_begin(THD *thd, const char *db_, const char *table_,
                             const TABLE_LIST* table_list);

void wsrep_begin_nbo_unlock(THD*);
void wsrep_end_nbo_lock(THD*, const TABLE_LIST *table_list);

void wsrep_to_isolation_end(THD *thd);

void wsrep_cleanup_transaction(THD *thd);
int wsrep_to_buf_helper(
  THD* thd, const char *query, uint query_len, uchar** buf, size_t* buf_len);
int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_create_event_query(THD *thd, uchar** buf, size_t* buf_len);

bool wsrep_stmt_rollback_is_safe(THD* thd);


wsrep_cb_status_t
wsrep_view_handler_cb (void*                    app_ctx,
                       void*                    recv_ctx,
                       const wsrep_view_info_t* view,
                       /* TODO: These are unused, should be removed?*/
                       const char*              state,
                       size_t                   state_len);

void wsrep_init_sidno(const wsrep_uuid_t&);
bool wsrep_node_is_donor();
bool wsrep_node_is_synced();

void wsrep_init_schema();
void wsrep_init_SR();
void wsrep_recover_view();
int wsrep_replay_from_SR_store(THD*, const wsrep_trx_meta_t&);
void wsrep_node_uuid(wsrep_uuid_t&);

class Log_event;
int wsrep_ignored_error_code(Log_event* ev, int error);
int wsrep_must_ignore_error(THD* thd);

bool wsrep_replicate_GTID(THD* thd);

/*
 * Helper class for non-blocking operations.
 */
class Wsrep_nbo_ctx
{
 public:
 Wsrep_nbo_ctx(const void* buf, size_t buf_len,
               uint32_t flags, const wsrep_trx_meta_t& meta) :
    buf_    (0),
    buf_len_(buf_len),
    flags_  (flags),
    meta_   (meta),
    mutex_  (),
    cond_   (),
    ready_  (false),
    executing_(false),
    toi_released_(false)
    {
      mysql_mutex_init(key_LOCK_wsrep_nbo, &mutex_, MY_MUTEX_INIT_FAST);
      mysql_cond_init(key_COND_wsrep_nbo, &cond_, NULL);

      if ((buf_ = malloc(buf_len_)) == 0) {
        throw std::exception();
      }
      memcpy(buf_, buf, buf_len);
    }


  ~Wsrep_nbo_ctx() {
    free(buf_);
  }

  const void* buf() { return buf_; }
  size_t buf_len() { return buf_len_; }
  uint32_t flags() { return flags_; }
  const wsrep_trx_meta_t& meta() { return meta_; }

  void wait_sync()
  {
    mysql_mutex_lock(&mutex_);
    while (ready_ == false)
    {
      mysql_cond_wait(&cond_, &mutex_);
    }
    mysql_mutex_unlock(&mutex_);
  }

  void signal()
  {
    mysql_mutex_lock(&mutex_);
    ready_ = true;
    mysql_cond_signal(&cond_);
    mysql_mutex_unlock(&mutex_);
  }

  bool ready() const { return ready_; }

  void set_executing(bool val) { executing_ = val; }

  bool executing() const { return executing_; }

  void set_toi_released(bool val) { toi_released_ = true; }

  bool toi_released() const { return toi_released_; }

 private:
  void*            buf_;
  size_t           buf_len_;
  uint32_t         flags_;
  wsrep_trx_meta_t meta_;
  mysql_mutex_t    mutex_;
  mysql_cond_t     cond_;
  bool             ready_;
  bool             executing_;
  bool             toi_released_;
};

/*
  Convenience macros for determining NBO start and END
*/
#define WSREP_NBO_START(flags_) \
((flags_ & WSREP_FLAG_ISOLATION) && (flags_ & WSREP_FLAG_TRX_START) && \
 !(flags_ & WSREP_FLAG_TRX_END))

#define WSREP_NBO_END(flags_) \
 ((flags_ & WSREP_FLAG_ISOLATION) && !(flags_ & WSREP_FLAG_TRX_START) && \
  (flags_ & WSREP_FLAG_TRX_END))

#define WSREP_TOI(flags_) \
  ((flags_ & WSREP_FLAG_ISOLATION) && (flags_ & WSREP_FLAG_TRX_START) && \
   (flags_ & WSREP_FLAG_TRX_END))

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

extern bool
wsrep_grant_mdl_exception(MDL_context *requestor_ctx,
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

extern void wsrep_close_client_connections(my_bool wait_to_end,
                                           THD *except_caller_thd = NULL);
int wsrep_wait_committing_connections_close(int wait_time);
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

#define WSREP_BINLOG_FORMAT(my_format)                         \
   ((wsrep_forced_binlog_format != BINLOG_FORMAT_UNSPEC) ?     \
   wsrep_forced_binlog_format : my_format)

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
#define WSREP_BINLOG_FORMAT(my_format) my_format

#endif /* WITH_WSREP */
#endif /* WSREP_MYSQLD_H */
