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

#ifndef MYSQL_SERVICE_WSREP_INCLUDED
#define MYSQL_SERVICE_WSREP_INCLUDED

/**
  @file
  wsrep service

  Interface to WSREP functionality in the server.
  For engines that want to support galera.
*/

/**
  @defgroup plugin_api_service_wsrep WSREP service
  @ingroup plugin_api_services
  Access to WSREP functionality

  @{
*/

enum Wsrep_service_key_type
{
    WSREP_SERVICE_KEY_SHARED,
    WSREP_SERVICE_KEY_REFERENCE,
    WSREP_SERVICE_KEY_UPDATE,
    WSREP_SERVICE_KEY_EXCLUSIVE
};


/** The bits in the bitmask for disabling temporarily some asserts */
#define  WSREP_ASSERT_INNODB_TRX   1
#if (defined (MYSQL_DYNAMIC_PLUGIN) && defined(MYSQL_SERVICE_WSREP_DYNAMIC_INCLUDED)) || (!defined(MYSQL_DYNAMIC_PLUGIN) && defined(MYSQL_SERVICE_WSREP_STATIC_INCLUDED))

#else

#include <my_pthread.h>
#ifdef __cplusplus
#endif

struct xid_t;
struct wsrep_ws_handle;
struct wsrep_buf;

/** Must match to definition in sql/mysqld.h */
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
  int                         (*wsrep_thd_TRYLOCK_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_UNLOCK_func)(const MYSQL_THD thd);
  const char *                (*wsrep_thd_query_func)(const MYSQL_THD thd);
  int                         (*wsrep_thd_retry_counter_func)(const MYSQL_THD thd);
  bool                        (*wsrep_thd_ignore_table_func)(MYSQL_THD thd);
  long long                   (*wsrep_thd_trx_seqno_func)(const MYSQL_THD thd);
  my_bool                     (*wsrep_thd_is_aborting_func)(const MYSQL_THD thd);
  my_bool                     (*wsrep_thd_in_rollback_func)(const THD *thd);
  void                        (*wsrep_set_data_home_dir_func)(const char *data_dir);
  my_bool                     (*wsrep_thd_is_BF_func)(const MYSQL_THD thd, my_bool sync);
  my_bool                     (*wsrep_thd_is_local_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_self_abort_func)(MYSQL_THD thd);
  int                         (*wsrep_thd_append_key_func)(MYSQL_THD thd, const struct wsrep_key* key,
                                                           int n_keys, enum Wsrep_service_key_type);
  int                         (*wsrep_thd_append_table_key_func)(MYSQL_THD thd, const char* db,
                                                           const char* table, enum Wsrep_service_key_type);
  my_bool                     (*wsrep_thd_is_local_transaction)(const MYSQL_THD thd);
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
  void                        (*wsrep_report_bf_lock_wait_func)(const MYSQL_THD thd,
                                                                unsigned long long trx_id);
  void                        (*wsrep_thd_kill_LOCK_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_kill_UNLOCK_func)(const MYSQL_THD thd);
  void                        (*wsrep_thd_set_wsrep_PA_unsafe_func)(MYSQL_THD thd);
  uint32                      (*wsrep_get_domain_id_func)();
} *wsrep_service;

#endif

#ifdef MYSQL_DYNAMIC_PLUGIN

#define MYSQL_SERVICE_WSREP_DYNAMIC_INCLUDED
#define get_wsrep_recovery() wsrep_service->get_wsrep_recovery_func()
#define wsrep_consistency_check(thd) wsrep_service->wsrep_consistency_check_func(thd)
#define wsrep_is_wsrep_xid(xid) wsrep_service->wsrep_is_wsrep_xid_func(xid)
#define wsrep_xid_seqno(xid) wsrep_service->wsrep_xid_seqno_func(xid)
#define wsrep_xid_uuid(xid) wsrep_service->wsrep_xid_uuid_func(xid)
/**
   Return true if wsrep is enabled for a thd. This means that
   wsrep is enabled globally and the thd has wsrep on
*/
#define wsrep_on(thd) (thd) && WSREP_ON && wsrep_service->wsrep_on_func(thd)
#define wsrep_prepare_key_for_innodb(A,B,C,D,E,F,G) wsrep_service->wsrep_prepare_key_for_innodb_func(A,B,C,D,E,F,G)
/** Lock thd wsrep lock */
#define wsrep_thd_LOCK(thd) wsrep_service->wsrep_thd_LOCK_func(thd)
/**
  Try thd wsrep lock.
  @return non-zero if lock could not be taken.
*/
#define wsrep_thd_TRYLOCK(thd) wsrep_service->wsrep_thd_TRYLOCK_func(thd)
/** Unlock thd wsrep lock */
#define wsrep_thd_UNLOCK(thd) wsrep_service->wsrep_thd_UNLOCK_func(thd)
#define wsrep_thd_kill_LOCK(thd) wsrep_service->wsrep_thd_kill_LOCK_func(thd)
#define wsrep_thd_kill_UNLOCK(thd) wsrep_service->wsrep_thd_kill_UNLOCK_func(thd)
#define wsrep_thd_query(thd) wsrep_service->wsrep_thd_query_func(thd)
#define wsrep_thd_retry_counter(thd) wsrep_service->wsrep_thd_retry_counter_func(thd)
#define wsrep_thd_ignore_table(thd) wsrep_service->wsrep_thd_ignore_table_func(thd)
#define wsrep_thd_trx_seqno(thd) wsrep_service->wsrep_thd_trx_seqno_func(thd)
#define wsrep_set_data_home_dir(A) wsrep_service->wsrep_set_data_home_dir_func(A)
/** @retval true if thd is in BF mode, either high_priority or TOI */
#define wsrep_thd_is_BF(thd,sync) wsrep_service->wsrep_thd_is_BF_func(thd,sync)
#define wsrep_thd_is_aborting(thd) wsrep_service->wsrep_thd_is_aborting_func(thd)
/** @retval true if thd is in rollback */
#define wsrep_thd_in_rollback_func(thd) wsrep_service->wsrep_thd_in_rollback_func(thd)
/** @retval true if thd is in replicating mode */
#define wsrep_thd_is_local(thd) wsrep_service->wsrep_thd_is_local_func(thd)
#define wsrep_thd_self_abort(thd) wsrep_service->wsrep_thd_self_abort_func(thd)
#define wsrep_thd_append_key(thd,key,nkeys,key_type) wsrep_service->wsrep_thd_append_key_func(thd,key,nkeys,key_type)
#define wsrep_thd_append_table_key(thd,db,table,key) wsrep_service->wsrep_thd_append_table_key_func(thd,db,table,key)
#define wsrep_thd_is_local_transaction(thd) wsrep_service->wsrep_thd_is_local_transaction_func(thd)
/** @return thd client state string */
#define wsrep_thd_client_state_str(thd) wsrep_service->wsrep_thd_client_state_str_func(thd)
/** @return thd client mode string */
#define wsrep_thd_client_mode_str(thd) wsrep_service->wsrep_thd_client_mode_str_func(thd)
/** @return thd transaction state string */
#define wsrep_thd_transaction_state_str(thd) wsrep_service->wsrep_thd_transaction_state_str_func(thd)
/** @return current transaction id */
#define wsrep_thd_transaction_id(thd) wsrep_service->wsrep_thd_transaction_id_func(thd)
/** Mark thd own transaction as aborted */
#define wsrep_thd_bf_abort(bf_thd,victim_thd,signal) wsrep_service->wsrep_thd_bf_abort_func(bf_thd,victim_thd,signal)
#define wsrep_thd_order_before(left,right) wsrep_service->wsrep_thd_order_before_func(left,right)
#define wsrep_handle_SR_rollback(BF_thd,victim_thd) wsrep_service->wsrep_handle_SR_rollback_func(BF_thd,victim_thd)
#define wsrep_thd_skip_locking(thd) wsrep_service->wsrep_thd_skip_locking_func(thd)
#define wsrep_get_sr_table_name() wsrep_service->wsrep_get_sr_table_name_func()
#define wsrep_get_debug() wsrep_service->wsrep_get_debug_func()
#define wsrep_commit_ordered(thd) wsrep_service->wsrep_commit_ordered_func(thd)
/**
  @return true if thd is in high priority mode
  @todo: rename to is_high_priority()
 */
#define wsrep_thd_is_applying(thd) wsrep_service->wsrep_thd_is_applying_func(thd)
#define wsrep_OSU_method_get(thd) wsrep_service->wsrep_OSU_method_get_func(thd)
#define wsrep_thd_has_ignored_error(thd) wsrep_service->wsrep_thd_has_ignored_error_func(thd)
#define wsrep_thd_set_ignored_error(thd,val) wsrep_service->wsrep_thd_set_ignored_error_func(thd,val)
#define wsrep_report_bf_lock_wait(thd,trx_id) wsrep_service->wsrep_report_bf_lock_wait(thd,trx_id)
#define wsrep_thd_set_PA_unsafe(thd) wsrep_service->wsrep_thd_set_PA_unsafe_func(thd)
#define wsrep_get_domain_id(thd) wsrep_service->wsrep_get_domain_id_func(thd)
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
/* Try thd wsrep lock. Return non-zero if lock could not be taken. */
extern "C" int wsrep_thd_TRYLOCK(const MYSQL_THD thd);
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
/* Return true if thd is in TOI mode */
extern "C" my_bool wsrep_thd_is_toi(const MYSQL_THD thd);
/* Return true if thd is in replicating TOI mode */
extern "C" my_bool wsrep_thd_is_local_toi(const MYSQL_THD thd);
/* Return true if thd is in RSU mode */
extern "C" my_bool wsrep_thd_is_in_rsu(const MYSQL_THD thd);
/* Return true if thd is in BF mode, either high_priority or TOI */
extern "C" my_bool wsrep_thd_is_BF(const MYSQL_THD thd, my_bool sync);
/* Return true if thd is streaming in progress */
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
/* Return true if thd is a WSREP applier rolling back a transaction locally */
extern "C" my_bool wsrep_thd_in_rollback(const MYSQL_THD thd);

struct wsrep_key;
struct wsrep_key_array;
extern "C" int wsrep_thd_append_key(MYSQL_THD thd,
                                    const struct wsrep_key* key,
                                    int n_keys,
                                    enum Wsrep_service_key_type);

extern "C" int wsrep_thd_append_table_key(MYSQL_THD thd,
                                    const char* db,
                                    const char* table,
                                    enum Wsrep_service_key_type);

extern "C" my_bool wsrep_thd_is_local_transaction(const MYSQL_THD thd);

extern const char* wsrep_sr_table_name_full;

extern "C" const char* wsrep_get_sr_table_name();

extern "C" my_bool wsrep_get_debug();

extern "C" void wsrep_commit_ordered(MYSQL_THD thd);
extern "C" my_bool wsrep_thd_is_applying(const MYSQL_THD thd);
extern "C" ulong wsrep_OSU_method_get(const MYSQL_THD thd);
extern "C" my_bool wsrep_thd_has_ignored_error(const MYSQL_THD thd);
extern "C" void wsrep_thd_set_ignored_error(MYSQL_THD thd, my_bool val);
extern "C" void wsrep_report_bf_lock_wait(const THD *thd,
                                          unsigned long long trx_id);
/* declare parallel applying unsafety for the THD */
extern "C" void wsrep_thd_set_PA_unsafe(MYSQL_THD thd);
extern "C" uint32 wsrep_get_domain_id();
#endif

/** @} */
#endif /* MYSQL_SERVICE_WSREP_INCLUDED */
