#ifndef MYSQL_SERVICE_WSREP_INCLUDED
/* Copyright (c) 2015 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file
  wsrep service

  Interface to WSREP functionality in the server.
  For engines that want to support galera.
*/

#ifdef __cplusplus
extern "C" {
#endif

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

enum wsrep_exec_mode {
    /* Transaction processing before replication. */
    LOCAL_STATE,
    /* Slave thread applying write sets from other nodes or replaying thread. */
    REPL_RECV,
    /* Total-order-isolation mode. */
    TOTAL_ORDER,
    /*
      Transaction procession after it has been replicated in prepare stage and
      has passed certification.
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

enum wsrep_trx_status {
    WSREP_TRX_OK,
    WSREP_TRX_CERT_FAIL,      /* certification failure, must abort */
    WSREP_TRX_SIZE_EXCEEDED,  /* trx size exceeded */
    WSREP_TRX_ERROR,          /* native mysql error */
};

struct xid_t;
struct wsrep;
struct wsrep_ws_handle;
struct wsrep_buf;

extern struct wsrep_service_st {
  struct wsrep *              (*get_wsrep_func)();
  my_bool                     (*get_wsrep_certify_nonPK_func)();
  my_bool                     (*get_wsrep_debug_func)();
  my_bool                     (*get_wsrep_drupal_282555_workaround_func)();
  my_bool                     (*get_wsrep_recovery_func)();
  my_bool                     (*get_wsrep_load_data_splitting_func)();
  my_bool                     (*get_wsrep_log_conflicts_func)();
  long                        (*get_wsrep_protocol_version_func)();
  my_bool                     (*wsrep_aborting_thd_contains_func)(THD *thd);
  void                        (*wsrep_aborting_thd_enqueue_func)(THD *thd);
  bool                        (*wsrep_consistency_check_func)(THD *thd);
  int                         (*wsrep_is_wsrep_xid_func)(const struct xid_t *xid);
  void                        (*wsrep_lock_rollback_func)();
  int                         (*wsrep_on_func)(MYSQL_THD);
  void                        (*wsrep_post_commit_func)(THD* thd, bool all);
  bool                        (*wsrep_prepare_key_func)(const unsigned char*, size_t, const unsigned char*, size_t, struct wsrep_buf*, size_t*);
  enum wsrep_trx_status       (*wsrep_run_wsrep_commit_func)(THD *thd, bool all);
  void                        (*wsrep_thd_LOCK_func)(THD *thd);
  void                        (*wsrep_thd_UNLOCK_func)(THD *thd);
  void                        (*wsrep_thd_awake_func)(THD *thd, my_bool signal);
  enum wsrep_conflict_state   (*wsrep_thd_conflict_state_func)(MYSQL_THD, my_bool);
  const char *                (*wsrep_thd_conflict_state_str_func)(THD *thd);
  enum wsrep_exec_mode        (*wsrep_thd_exec_mode_func)(THD *thd);
  const char *                (*wsrep_thd_exec_mode_str_func)(THD *thd);
  enum wsrep_conflict_state   (*wsrep_thd_get_conflict_state_func)(MYSQL_THD);
  my_bool                     (*wsrep_thd_is_BF_func)(MYSQL_THD , my_bool);
  my_bool                     (*wsrep_thd_is_wsrep_func)(MYSQL_THD thd);
  const char *                (*wsrep_thd_query_func)(THD *thd);
  enum wsrep_query_state      (*wsrep_thd_query_state_func)(THD *thd);
  const char *                (*wsrep_thd_query_state_str_func)(THD *thd);
  int                         (*wsrep_thd_retry_counter_func)(THD *thd);
  void                        (*wsrep_thd_set_conflict_state_func)(THD *thd, enum wsrep_conflict_state state);
  bool                        (*wsrep_thd_ignore_table_func)(THD *thd);
  long long                   (*wsrep_thd_trx_seqno_func)(THD *thd);
  struct wsrep_ws_handle *    (*wsrep_thd_ws_handle_func)(THD *thd);
  void                        (*wsrep_thd_auto_increment_variables_func)(THD *thd, unsigned long long *offset, unsigned long long *increment);
  void                        (*wsrep_set_load_multi_commit_func)(THD *thd, bool split);
  bool                        (*wsrep_is_load_multi_commit_func)(THD *thd);
  int                         (*wsrep_trx_is_aborting_func)(MYSQL_THD thd);
  int                         (*wsrep_trx_order_before_func)(MYSQL_THD, MYSQL_THD);
  void                        (*wsrep_unlock_rollback_func)();
  void                        (*wsrep_set_data_home_dir_func)(const char *data_dir);
  my_bool                     (*wsrep_thd_is_applier_func)(MYSQL_THD);
  void                        (*wsrep_report_bf_lock_wait_func)(MYSQL_THD thd,
                                                                unsigned long long trx_id);
} *wsrep_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
#define get_wsrep() wsrep_service->get_wsrep_func()
#define get_wsrep_certify_nonPK() wsrep_service->get_wsrep_certify_nonPK_func()
#define get_wsrep_debug() wsrep_service->get_wsrep_debug_func()
#define get_wsrep_drupal_282555_workaround() wsrep_service->get_wsrep_drupal_282555_workaround_func()
#define get_wsrep_recovery() wsrep_service->get_wsrep_recovery_func()
#define get_wsrep_load_data_splitting() wsrep_service->get_wsrep_load_data_splitting_func()
#define get_wsrep_log_conflicts() wsrep_service->get_wsrep_log_conflicts_func()
#define get_wsrep_protocol_version() wsrep_service->get_wsrep_protocol_version_func()
#define wsrep_aborting_thd_contains(T) wsrep_service->wsrep_aborting_thd_contains_func(T)
#define wsrep_aborting_thd_enqueue(T) wsrep_service->wsrep_aborting_thd_enqueue_func(T)
#define wsrep_consistency_check(T) wsrep_service->wsrep_consistency_check_func(T)
#define wsrep_is_wsrep_xid(X) wsrep_service->wsrep_is_wsrep_xid_func(X)
#define wsrep_lock_rollback() wsrep_service->wsrep_lock_rollback_func()
#define wsrep_on(X) wsrep_service->wsrep_on_func(X)
#define wsrep_post_commit(T,A) wsrep_service->wsrep_post_commit_func(T,A)
#define wsrep_prepare_key(A,B,C,D,E,F) wsrep_service->wsrep_prepare_key_func(A,B,C,D,E,F)
#define wsrep_run_wsrep_commit(T,A) wsrep_service->wsrep_run_wsrep_commit_func(T,A)
#define wsrep_thd_LOCK(T) wsrep_service->wsrep_thd_LOCK_func(T)
#define wsrep_thd_UNLOCK(T) wsrep_service->wsrep_thd_UNLOCK_func(T)
#define wsrep_thd_awake(T,S) wsrep_service->wsrep_thd_awake_func(T,S)
#define wsrep_thd_conflict_state(T,S) wsrep_service->wsrep_thd_conflict_state_func(T,S)
#define wsrep_thd_conflict_state_str(T) wsrep_service->wsrep_thd_conflict_state_str_func(T)
#define wsrep_thd_exec_mode(T) wsrep_service->wsrep_thd_exec_mode_func(T)
#define wsrep_thd_exec_mode_str(T) wsrep_service->wsrep_thd_exec_mode_str_func(T)
#define wsrep_thd_get_conflict_state(T) wsrep_service->wsrep_thd_get_conflict_state_func(T)
#define wsrep_thd_is_BF(T,S) wsrep_service->wsrep_thd_is_BF_func(T,S)
#define wsrep_thd_is_wsrep(T) wsrep_service->wsrep_thd_is_wsrep_func(T)
#define wsrep_thd_query(T) wsrep_service->wsrep_thd_query_func(T)
#define wsrep_thd_query_state(T) wsrep_service->wsrep_thd_query_state_func(T)
#define wsrep_thd_query_state_str(T) wsrep_service->wsrep_thd_query_state_str_func(T)
#define wsrep_thd_retry_counter(T) wsrep_service->wsrep_thd_retry_counter_func(T)
#define wsrep_thd_set_conflict_state(T,S) wsrep_service->wsrep_thd_set_conflict_state_func(T,S)
#define wsrep_thd_ignore_table(T) wsrep_service->wsrep_thd_ignore_table_func(T)
#define wsrep_thd_trx_seqno(T) wsrep_service->wsrep_thd_trx_seqno_func(T)
#define wsrep_thd_ws_handle(T) wsrep_service->wsrep_thd_ws_handle_func(T)
#define wsrep_thd_auto_increment_variables(T,O,I) wsrep_service->wsrep_thd_auto_increment_variables_func(T,O,I)
#define wsrep_set_load_multi_commit(T,S) wsrep_service->wsrep_set_load_multi_commit_func(T,S)
#define wsrep_is_load_multi_commit(T) wsrep_service->wsrep_is_load_multi_commit_func(T)
#define wsrep_trx_is_aborting(T) wsrep_service->wsrep_trx_is_aborting_func(T)
#define wsrep_trx_order_before(T1,T2) wsrep_service->wsrep_trx_order_before_func(T1,T2)
#define wsrep_unlock_rollback() wsrep_service->wsrep_unlock_rollback_func()
#define wsrep_set_data_home_dir(A) wsrep_service->wsrep_set_data_home_dir_func(A)
#define wsrep_thd_is_applier(T) wsrep_service->wsrep_thd_is_applier_func(T)
#define wsrep_report_bf_lock_wait(T,I) wsrep_service->wsrep_report_bf_lock_wait_func(T,I)

#define wsrep_debug get_wsrep_debug()
#define wsrep_log_conflicts get_wsrep_log_conflicts()
#define wsrep_certify_nonPK get_wsrep_certify_nonPK()
#define wsrep_load_data_splitting get_wsrep_load_data_splitting()
#define wsrep_drupal_282555_workaround get_wsrep_drupal_282555_workaround()
#define wsrep_recovery get_wsrep_recovery()
#define wsrep_protocol_version get_wsrep_protocol_version()

#else

extern my_bool wsrep_debug;
extern my_bool wsrep_log_conflicts;
extern my_bool wsrep_certify_nonPK;
extern my_bool wsrep_load_data_splitting;
extern my_bool wsrep_drupal_282555_workaround;
extern my_bool wsrep_recovery;
extern long wsrep_protocol_version;

bool wsrep_consistency_check(THD *thd);
bool wsrep_prepare_key(const unsigned char* cache_key, size_t cache_key_len, const unsigned char* row_id, size_t row_id_len, struct wsrep_buf* key, size_t* key_len);
const char *wsrep_thd_query(THD *thd);
const char *wsrep_thd_conflict_state_str(THD *thd);
const char *wsrep_thd_exec_mode_str(THD *thd);
const char *wsrep_thd_query_state_str(THD *thd);
enum wsrep_conflict_state wsrep_thd_conflict_state(MYSQL_THD thd, my_bool sync);
enum wsrep_conflict_state wsrep_thd_get_conflict_state(MYSQL_THD thd);
enum wsrep_exec_mode wsrep_thd_exec_mode(THD *thd);
enum wsrep_query_state wsrep_thd_query_state(THD *thd);
enum wsrep_trx_status wsrep_run_wsrep_commit(THD *thd, bool all);
int wsrep_is_wsrep_xid(const struct xid_t* xid);
int wsrep_on(MYSQL_THD thd);
int wsrep_thd_retry_counter(THD *thd);
int wsrep_trx_is_aborting(MYSQL_THD thd);
int wsrep_trx_order_before(MYSQL_THD thd1, MYSQL_THD thd2);
long get_wsrep_protocol_version();
long long wsrep_thd_trx_seqno(THD *thd);
my_bool get_wsrep_certify_nonPK();
my_bool get_wsrep_debug();
my_bool get_wsrep_drupal_282555_workaround();
my_bool get_wsrep_recovery();
my_bool get_wsrep_load_data_splitting();
my_bool get_wsrep_log_conflicts();
my_bool wsrep_aborting_thd_contains(THD *thd);
my_bool wsrep_thd_is_BF(MYSQL_THD thd, my_bool sync);
my_bool wsrep_thd_is_wsrep(MYSQL_THD thd);
struct wsrep *get_wsrep();
struct wsrep_ws_handle *wsrep_thd_ws_handle(THD *thd);
void wsrep_thd_auto_increment_variables(THD *thd, unsigned long long *offset, unsigned long long *increment);
void wsrep_set_load_multi_commit(THD *thd, bool split);
bool wsrep_is_load_multi_commit(THD *thd);
void wsrep_aborting_thd_enqueue(THD *thd);
void wsrep_lock_rollback();
void wsrep_post_commit(THD* thd, bool all);
void wsrep_thd_LOCK(THD *thd);
void wsrep_thd_UNLOCK(THD *thd);
void wsrep_thd_awake(THD *thd, my_bool signal);
void wsrep_thd_set_conflict_state(THD *thd, enum wsrep_conflict_state state);
bool wsrep_thd_ignore_table(THD *thd);
void wsrep_unlock_rollback();
void wsrep_set_data_home_dir(const char *data_dir);
my_bool wsrep_thd_is_applier(MYSQL_THD thd);
void wsrep_report_bf_lock_wait(THD *thd,
                               unsigned long long trx_id);
#endif

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_WSREP_INCLUDED
#endif

