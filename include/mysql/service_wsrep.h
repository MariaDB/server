#ifndef MYSQL_SERVICE_WSREP_INCLUDED
#define MYSQL_SERVICE_WSREP_INCLUDED

enum Wsrep_service_key_type
{
    WSREP_SERVICE_KEY_SHARED,
    WSREP_SERVICE_KEY_REFERENCE,
    WSREP_SERVICE_KEY_UPDATE,
    WSREP_SERVICE_KEY_EXCLUSIVE
};

#if (defined (MYSQL_DYNAMIC_PLUGIN) && defined(MYSQL_SERVICE_WSREP_DYNAMIC_INCLUDED)) || (!defined(MYSQL_DYNAMIC_PLUGIN) && defined(MYSQL_SERVICE_WSREP_STATIC_INCLUDED))

#else

/* Copyright (c) 2015, 2020, MariaDB Corporation Ab
                 2018 Codership Oy <info@codership.com>

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
#include <my_pthread.h>
#ifdef __cplusplus
#endif

struct xid_t;
struct wsrep_ws_handle;
struct wsrep_buf;

/* Must match to definition in sql/mysqld.h */
typedef int64 query_id_t;


extern struct wsrep_service_st {
  my_bool                     (*get_wsrep_recovery_func)();
  bool                        (*wsrep_consistency_check_func)(MYSQL_THD thd);
  int                         (*wsrep_is_wsrep_xid_func)(const void *xid);
  long long                   (*wsrep_xid_seqno_func)(const struct xid_t *xid);
  const unsigned char*        (*wsrep_xid_uuid_func)(const struct xid_t *xid);
  my_bool                     (*wsrep_on_func)(const MYSQL_THD thd);
  bool                        (*wsrep_prepare_key_for_innodb_func)(MYSQL_THD thd, const unsigned char*, size_t, const unsigned char*, size_t, struct wsrep_buf*, size_t*);
  void                        (*wsrep_thd_LOCK_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_UNLOCK_func)(const MYSQL_THD thd);
  const char *                (*wsrep_thd_query_func)(const MYSQL_THD thd);
  int                         (*wsrep_thd_retry_counter_func)(const MYSQL_THD thd);
  bool                        (*wsrep_thd_ignore_table_func)(MYSQL_THD thd);
  long long                   (*wsrep_thd_trx_seqno_func)(const MYSQL_THD thd);
  my_bool                     (*wsrep_thd_is_aborting_func)(const MYSQL_THD thd);
  void                        (*wsrep_set_data_home_dir_func)(const char *data_dir);
  my_bool                     (*wsrep_thd_is_BF_func)(const MYSQL_THD thd, my_bool sync);
  my_bool                     (*wsrep_thd_is_local_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_self_abort_func)(MYSQL_THD thd);
  int                         (*wsrep_thd_append_key_func)(MYSQL_THD thd, const struct wsrep_key* key,
                                                           int n_keys, enum Wsrep_service_key_type);
  const char*                 (*wsrep_thd_client_state_str_func)(const MYSQL_THD thd);
  const char*                 (*wsrep_thd_client_mode_str_func)(const MYSQL_THD thd);
  const char*                 (*wsrep_thd_transaction_state_str_func)(const MYSQL_THD thd);
  query_id_t                  (*wsrep_thd_transaction_id_func)(const MYSQL_THD thd);
  my_bool                     (*wsrep_thd_bf_abort_func)(MYSQL_THD bf_thd,
                                                         MYSQL_THD victim_thd,
                                                         my_bool signal);
  my_bool                     (*wsrep_thd_order_before_func)(const MYSQL_THD left, const MYSQL_THD right);
  void                        (*wsrep_handle_SR_rollback_func)(MYSQL_THD BF_thd, MYSQL_THD victim_thd);
  my_bool                     (*wsrep_thd_skip_locking_func)(const MYSQL_THD thd);
  const char*                 (*wsrep_get_sr_table_name_func)();
  my_bool                     (*wsrep_get_debug_func)();
  void                        (*wsrep_commit_ordered_func)(MYSQL_THD thd);
  my_bool                     (*wsrep_thd_is_applying_func)(const MYSQL_THD thd);
  ulong                       (*wsrep_OSU_method_get_func)(const MYSQL_THD thd);
  my_bool                     (*wsrep_thd_has_ignored_error_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_set_ignored_error_func)(MYSQL_THD thd, my_bool val);
  bool                        (*wsrep_thd_set_wsrep_aborter_func)(MYSQL_THD bf_thd, MYSQL_THD thd);
  void                        (*wsrep_report_bf_lock_wait_func)(const MYSQL_THD thd,
                                                                unsigned long long trx_id);
  void                        (*wsrep_thd_kill_LOCK_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_kill_UNLOCK_func)(const MYSQL_THD thd);
} *wsrep_service;

#define MYSQL_SERVICE_WSREP_INCLUDED
#endif

#ifdef MYSQL_DYNAMIC_PLUGIN

#define MYSQL_SERVICE_WSREP_DYNAMIC_INCLUDED
#define get_wsrep_recovery() wsrep_service->get_wsrep_recovery_func()
#define wsrep_consistency_check(T) wsrep_service->wsrep_consistency_check_func(T)
#define wsrep_is_wsrep_xid(X) wsrep_service->wsrep_is_wsrep_xid_func(X)
#define wsrep_xid_seqno(X) wsrep_service->wsrep_xid_seqno_func(X)
#define wsrep_xid_uuid(X) wsrep_service->wsrep_xid_uuid_func(X)
#define wsrep_on(thd) (thd) && WSREP_ON && wsrep_service->wsrep_on_func(thd)
#define wsrep_prepare_key_for_innodb(A,B,C,D,E,F,G) wsrep_service->wsrep_prepare_key_for_innodb_func(A,B,C,D,E,F,G)
#define wsrep_thd_LOCK(T) wsrep_service->wsrep_thd_LOCK_func(T)
#define wsrep_thd_UNLOCK(T) wsrep_service->wsrep_thd_UNLOCK_func(T)
#define wsrep_thd_kill_LOCK(T) wsrep_service->wsrep_thd_kill_LOCK_func(T)
#define wsrep_thd_kill_UNLOCK(T) wsrep_service->wsrep_thd_kill_UNLOCK_func(T)
#define wsrep_thd_query(T) wsrep_service->wsrep_thd_query_func(T)
#define wsrep_thd_retry_counter(T) wsrep_service->wsrep_thd_retry_counter_func(T)
#define wsrep_thd_ignore_table(T) wsrep_service->wsrep_thd_ignore_table_func(T)
#define wsrep_thd_trx_seqno(T) wsrep_service->wsrep_thd_trx_seqno_func(T)
#define wsrep_set_data_home_dir(A) wsrep_service->wsrep_set_data_home_dir_func(A)
#define wsrep_thd_is_BF(T,S) wsrep_service->wsrep_thd_is_BF_func(T,S)
#define wsrep_thd_is_aborting(T) wsrep_service->wsrep_thd_is_aborting_func(T)
#define wsrep_thd_is_local(T) wsrep_service->wsrep_thd_is_local_func(T)
#define wsrep_thd_self_abort(T) wsrep_service->wsrep_thd_self_abort_func(T)
#define wsrep_thd_append_key(T,W,N,K) wsrep_service->wsrep_thd_append_key_func(T,W,N,K)
#define wsrep_thd_client_state_str(T) wsrep_service->wsrep_thd_client_state_str_func(T)
#define wsrep_thd_client_mode_str(T) wsrep_service->wsrep_thd_client_mode_str_func(T)
#define wsrep_thd_transaction_state_str(T) wsrep_service->wsrep_thd_transaction_state_str_func(T)
#define wsrep_thd_transaction_id(T) wsrep_service->wsrep_thd_transaction_id_func(T)
#define wsrep_thd_bf_abort(T,T2,S) wsrep_service->wsrep_thd_bf_abort_func(T,T2,S)
#define wsrep_thd_order_before(L,R) wsrep_service->wsrep_thd_order_before_func(L,R)
#define wsrep_handle_SR_rollback(B,V) wsrep_service->wsrep_handle_SR_rollback_func(B,V)
#define wsrep_thd_skip_locking(T) wsrep_service->wsrep_thd_skip_locking_func(T)
#define wsrep_get_sr_table_name() wsrep_service->wsrep_get_sr_table_name_func()
#define wsrep_get_debug() wsrep_service->wsrep_get_debug_func()
#define wsrep_commit_ordered(T) wsrep_service->wsrep_commit_ordered_func(T)
#define wsrep_thd_is_applying(T) wsrep_service->wsrep_thd_is_applying_func(T)
#define wsrep_OSU_method_get(T) wsrep_service->wsrep_OSU_method_get_func(T)
#define wsrep_thd_has_ignored_error(T) wsrep_service->wsrep_thd_has_ignored_error_func(T)
#define wsrep_thd_set_ignored_error(T,V) wsrep_service->wsrep_thd_set_ignored_error_func(T,V)
#define wsrep_thd_set_wsrep_aborter(T) wsrep_service->wsrep_thd_set_wsrep_aborter_func(T1, T2)
#define wsrep_report_bf_lock_wait(T,I) wsrep_service->wsrep_report_bf_lock_wait(T,I)
#else

#define MYSQL_SERVICE_WSREP_STATIC_INCLUDED
extern ulong   wsrep_debug;
extern my_bool wsrep_log_conflicts;
extern my_bool wsrep_certify_nonPK;
extern my_bool wsrep_load_data_splitting;
extern my_bool wsrep_drupal_282555_workaround;
extern my_bool wsrep_recovery;
extern long wsrep_protocol_version;

extern "C" bool wsrep_consistency_check(MYSQL_THD thd);
bool wsrep_prepare_key_for_innodb(MYSQL_THD thd, const unsigned char* cache_key, size_t cache_key_len, const unsigned char* row_id, size_t row_id_len, struct wsrep_buf* key, size_t* key_len);
extern "C" const char *wsrep_thd_query(const MYSQL_THD thd);
extern "C" int wsrep_is_wsrep_xid(const void* xid);
extern "C" long long wsrep_xid_seqno(const struct xid_t* xid);
const unsigned char* wsrep_xid_uuid(const struct xid_t* xid);
extern "C" long long wsrep_thd_trx_seqno(const MYSQL_THD thd);
my_bool get_wsrep_recovery();
bool wsrep_thd_ignore_table(MYSQL_THD thd);
void wsrep_set_data_home_dir(const char *data_dir);

/* from mysql wsrep-lib */
#include "my_global.h"
#include "my_pthread.h"

/* Return true if wsrep is enabled for a thd. This means that
   wsrep is enabled globally and the thd has wsrep on */
extern "C" my_bool wsrep_on(const MYSQL_THD thd);
/* Lock thd wsrep lock */
extern "C" void wsrep_thd_LOCK(const MYSQL_THD thd);
/* Unlock thd wsrep lock */
extern "C" void wsrep_thd_UNLOCK(const MYSQL_THD thd);

extern "C" void wsrep_thd_kill_LOCK(const MYSQL_THD thd);
extern "C" void wsrep_thd_kill_UNLOCK(const MYSQL_THD thd);

/* Return thd client state string */
extern "C" const char* wsrep_thd_client_state_str(const MYSQL_THD thd);
/* Return thd client mode string */
extern "C" const char* wsrep_thd_client_mode_str(const MYSQL_THD thd);
/* Return thd transaction state string */
extern "C" const char* wsrep_thd_transaction_state_str(const MYSQL_THD thd);

/* Return current transaction id */
extern "C" query_id_t wsrep_thd_transaction_id(const MYSQL_THD thd);
/* Mark thd own transaction as aborted */
extern "C" void wsrep_thd_self_abort(MYSQL_THD thd);
/* Return true if thd is in replicating mode */
extern "C" my_bool wsrep_thd_is_local(const MYSQL_THD thd);
/* Return true if thd is in high priority mode */
/* todo: rename to is_high_priority() */
extern "C" my_bool wsrep_thd_is_applying(const MYSQL_THD thd);
/* set wsrep_aborter for the target THD */
extern "C" bool wsrep_thd_set_wsrep_aborter(MYSQL_THD bf_thd, MYSQL_THD victim_thd);
/* Return true if thd is in TOI mode */
extern "C" my_bool wsrep_thd_is_toi(const MYSQL_THD thd);
/* Return true if thd is in replicating TOI mode */
extern "C" my_bool wsrep_thd_is_local_toi(const MYSQL_THD thd);
/* Return true if thd is in RSU mode */
extern "C" my_bool wsrep_thd_is_in_rsu(const MYSQL_THD thd);
/* Return true if thd is in BF mode, either high_priority or TOI */
extern "C" my_bool wsrep_thd_is_BF(const MYSQL_THD thd, my_bool sync);
/* Return true if thd is streaming */
extern "C" my_bool wsrep_thd_is_SR(const MYSQL_THD thd);
extern "C" void wsrep_handle_SR_rollback(MYSQL_THD BF_thd, MYSQL_THD victim_thd);
/* Return thd retry counter */
extern "C" int wsrep_thd_retry_counter(const MYSQL_THD thd);
/* BF abort victim_thd */
extern "C" my_bool wsrep_thd_bf_abort(MYSQL_THD bf_thd,
                                      MYSQL_THD victim_thd,
                                      my_bool signal);
/* Return true if left thd is ordered before right thd */
extern "C" my_bool wsrep_thd_order_before(const MYSQL_THD left, const MYSQL_THD right);
/* Return true if thd should skip locking. This means that the thd
   is operating on shared resource inside commit order critical section. */
extern "C" my_bool wsrep_thd_skip_locking(const MYSQL_THD thd);
/* Return true if thd is aborting */
extern "C" my_bool wsrep_thd_is_aborting(const MYSQL_THD thd);

struct wsrep_key;
struct wsrep_key_array;
extern "C" int wsrep_thd_append_key(MYSQL_THD thd,
                                    const struct wsrep_key* key,
                                    int n_keys,
                                    enum Wsrep_service_key_type);

extern const char* wsrep_sr_table_name_full;

extern "C" const char* wsrep_get_sr_table_name();

extern "C" my_bool wsrep_get_debug();

extern "C" void wsrep_commit_ordered(MYSQL_THD thd);
extern "C" my_bool wsrep_thd_is_applying(const MYSQL_THD thd);
extern "C" ulong wsrep_OSU_method_get(const MYSQL_THD thd);
extern "C" my_bool wsrep_thd_has_ignored_error(const MYSQL_THD thd);
extern "C" void wsrep_thd_set_ignored_error(MYSQL_THD thd, my_bool val);
extern "C" bool wsrep_thd_set_wsrep_aborter(MYSQL_THD bf_thd, MYSQL_THD victim_thd);
extern "C" void wsrep_report_bf_lock_wait(const THD *thd,
                                          unsigned long long trx_id);
#endif
#endif /* MYSQL_SERVICE_WSREP_INCLUDED */
