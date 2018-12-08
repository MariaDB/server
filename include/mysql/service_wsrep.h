#ifndef MYSQL_SERVICE_WSREP_INCLUDED
#define MYSQL_SERVICE_WSREP_INCLUDED

#if (defined (MYSQL_DYNAMIC_PLUGIN) && defined(MYSQL_SERVICE_WSREP_DYNAMIC_INCLUDED)) || (!defined(MYSQL_DYNAMIC_PLUGIN) && defined(MYSQL_SERVICE_WSREP_STATIC_INCLUDED))

#else

/* Copyright (c) 2015 MariaDB Corporation Ab
                 2018 Codership Oy <info@codership.com>
*/
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

extern struct wsrep_service_st {
  my_bool                     (*get_wsrep_certify_nonPK_func)();
  my_bool                     (*get_wsrep_debug_func)();
  my_bool                     (*get_wsrep_drupal_282555_workaround_func)();
  my_bool                     (*get_wsrep_recovery_func)();
  my_bool                     (*get_wsrep_load_data_splitting_func)();
  my_bool                     (*get_wsrep_log_conflicts_func)();
  long                        (*get_wsrep_protocol_version_func)();
  bool                        (*wsrep_consistency_check_func)(THD *thd);
  int                         (*wsrep_is_wsrep_xid_func)(const void *xid);
  long long                   (*wsrep_xid_seqno_func)(const struct xid_t *xid);
  const unsigned char*        (*wsrep_xid_uuid_func)(const struct xid_t *xid);
  my_bool                     (*wsrep_on_func)(const void *);
  bool                        (*wsrep_prepare_key_for_innodb_func)(THD* thd, const unsigned char*, size_t, const unsigned char*, size_t, struct wsrep_buf*, size_t*);
  void                        (*wsrep_thd_LOCK_func)(const void* thd_ptr);
  void                        (*wsrep_thd_UNLOCK_func)(const void* thd_ptr);
  void                        (*wsrep_thd_awake_func)(const void* thd, my_bool signal);
  my_thread_id                (*wsrep_thd_thread_id_func)(const void* thd);
  my_bool                     (*wsrep_thd_is_wsrep_on_func)(const void* thd);
  const char *                (*wsrep_thd_query_func)(const void* thd);
  int                         (*wsrep_thd_retry_counter_func)(THD *thd);
  bool                        (*wsrep_thd_ignore_table_func)(THD *thd);
  long long                   (*wsrep_thd_trx_seqno_func)(const void* thd);
  my_bool                     (*wsrep_thd_is_aborting_func)(const void* thd);
  my_bool                     (*wsrep_trx_order_before_func)(const void* left_ptr, const void* right_ptr);
  void                        (*wsrep_thd_xid_func)(const void *thd_ptr, void *xid, size_t xid_size);
  //  void                        (*wsrep_unlock_rollback_func)();
  void                        (*wsrep_set_data_home_dir_func)(const char *data_dir);
} *wsrep_service;

#define MYSQL_SERVICE_WSREP_INCLUDED
#endif

#ifdef MYSQL_DYNAMIC_PLUGIN

#define MYSQL_SERVICE_WSREP_DYNAMIC_INCLUDED
#define get_wsrep_certify_nonPK() wsrep_service->get_wsrep_certify_nonPK_func()
#define get_wsrep_debug() wsrep_service->get_wsrep_debug_func()
#define get_wsrep_drupal_282555_workaround() wsrep_service->get_wsrep_drupal_282555_workaround_func()
#define get_wsrep_recovery() wsrep_service->get_wsrep_recovery_func()
#define get_wsrep_load_data_splitting() wsrep_service->get_wsrep_load_data_splitting_func()
#define get_wsrep_log_conflicts() wsrep_service->get_wsrep_log_conflicts_func()
#define get_wsrep_protocol_version() wsrep_service->get_wsrep_protocol_version_func()
#define wsrep_consistency_check(T) wsrep_service->wsrep_consistency_check_func(T)
#define wsrep_is_wsrep_xid(X) wsrep_service->wsrep_is_wsrep_xid_func(X)
#define wsrep_xid_seqno(X) wsrep_service->wsrep_xid_seqno_func(X)
#define wsrep_xid_uuid(X) wsrep_service->wsrep_xid_uuid_func(X)
#define wsrep_on(X) wsrep_service->wsrep_on_func(X)
#define wsrep_prepare_key_for_innodb(A,B,C,D,E,F,G) wsrep_service->wsrep_prepare_key_for_innodb_func(A,B,C,D,E,F.G)
#define wsrep_thd_LOCK(T) wsrep_service->wsrep_thd_LOCK_func(T)
#define wsrep_thd_UNLOCK(T) wsrep_service->wsrep_thd_UNLOCK_func(T)
#define wsrep_thd_awake(T,S) wsrep_service->wsrep_thd_awake_func(T,S)
#define wsrep_thd_thread_id(T) wsrep_service->wsrep_thd_thread_id_func(T)
#define wsrep_thd_is_wsrep_on(T) wsrep_service->wsrep_thd_is_wsrep_on_func(T)
#define wsrep_thd_query(T) wsrep_service->wsrep_thd_query_func(T)
#define wsrep_thd_retry_counter(T) wsrep_service->wsrep_thd_retry_counter_func(T)
#define wsrep_thd_ignore_table(T) wsrep_service->wsrep_thd_ignore_table_func(T)
#define wsrep_thd_trx_seqno(T) wsrep_service->wsrep_thd_trx_seqno_func(T)
#define wsrep_trx_order_before(T1,T2) wsrep_service->wsrep_trx_order_before_func(T1,T2)
#define wsrep_thd_xid(T,X,S) wsrep_service->wsrep_thd_xid_func(T,X,S)
//#define wsrep_unlock_rollback() wsrep_service->wsrep_unlock_rollback_func()
#define wsrep_set_data_home_dir(A) wsrep_service->wsrep_set_data_home_dir_func(A)

#define wsrep_thd_is_BF(T,S) wsrep_service->wsrep_thd_is_BF_func(T,S)

#else

#define MYSQL_SERVICE_WSREP_STATIC_INCLUDED
extern my_bool wsrep_debug;
extern my_bool wsrep_log_conflicts;
extern my_bool wsrep_certify_nonPK;
extern my_bool wsrep_load_data_splitting;
extern my_bool wsrep_drupal_282555_workaround;
extern my_bool wsrep_recovery;
extern long wsrep_protocol_version;

extern "C" bool wsrep_consistency_check(THD *thd);
bool wsrep_prepare_key_for_innodb(THD* thd, const unsigned char* cache_key, size_t cache_key_len, const unsigned char* row_id, size_t row_id_len, struct wsrep_buf* key, size_t* key_len);
extern "C" const char *wsrep_thd_query(const void* thd);
extern "C" int wsrep_is_wsrep_xid(const void* xid);
extern "C" long long wsrep_xid_seqno(const struct xid_t* xid);
const unsigned char* wsrep_xid_uuid(const struct xid_t* xid);
my_bool wsrep_trx_order_before(const void* thd1, const void* thd2);
long get_wsrep_protocol_version();
extern "C" long long wsrep_thd_trx_seqno(const void* thd);
my_bool get_wsrep_certify_nonPK();
my_bool get_wsrep_debug();
my_bool get_wsrep_drupal_282555_workaround();
my_bool get_wsrep_recovery();
my_bool get_wsrep_load_data_splitting();
my_bool get_wsrep_log_conflicts();
my_bool wsrep_aborting_thd_contains(THD *thd);
struct wsrep_ws_handle *wsrep_thd_ws_handle(THD *thd);
void wsrep_aborting_thd_enqueue(THD *thd);
void wsrep_post_commit(THD* thd, bool all);
bool wsrep_thd_ignore_table(THD *thd);
//void wsrep_unlock_rollback();
void wsrep_set_data_home_dir(const char *data_dir);


/* from mysql wsrep-lib */
#include "my_global.h"
#include "my_pthread.h"
/* Must match to definition in sql/mysqld.h */
typedef int64 query_id_t;

extern "C" my_bool wsrep_global_on();

/* Return true if wsrep is enabled for a thd. This means that
   wsrep is enabled globally and the thd has wsrep on */
extern "C" my_bool wsrep_on(const void* thd);
/* Lock thd wsrep lock */
extern "C" void wsrep_thd_LOCK(const void* thd);
/* Unlock thd wsrep lock */
extern "C" void wsrep_thd_UNLOCK(const void* thd);

/* Return true if thd has wsrep_on */
extern "C" my_bool wsrep_thd_is_wsrep_on(const void* thd);
/* Return thd id */
extern "C" my_thread_id wsrep_thd_thread_id(const void* thd);

/* Return thd client state string */
extern "C" const char* wsrep_thd_client_state_str(const void* thd);
/* Return thd client mode string */
extern "C" const char* wsrep_thd_client_mode_str(const void* thd);
/* Return thd transaction state string */
extern "C" const char* wsrep_thd_transaction_state_str(const void* thd);

/* Return current query id */
extern "C" query_id_t wsrep_thd_query_id(const void* thd);
/* Return current transaction id */
extern "C" query_id_t wsrep_thd_transaction_id(const void* thd);
/* Mark thd own transaction as aborted */
extern "C" void wsrep_thd_self_abort(void *thd);
/* Return true if thd is in replicating mode */
extern "C" my_bool wsrep_thd_is_local(const void *thd);
/* Return true if thd is in high priority mode */
/* todo: rename to is_high_priority() */
extern "C" my_bool wsrep_thd_is_applying(const void *thd);
/* Return true if thd is in TOI mode */
extern "C" my_bool wsrep_thd_is_toi(const void* thd);
/* Return true if thd is in replicating TOI mode */
extern "C" my_bool wsrep_thd_is_local_toi(const void* thd);
/* Return true if thd is in RSU mode */
extern "C" my_bool wsrep_thd_is_in_rsu(const void* thd);
/* Return true if thd is in BF mode, either high_priority or TOI */
extern "C" my_bool wsrep_thd_is_BF(const void* thd, my_bool sync);
/* Return true if thd is streaming */
extern "C" my_bool wsrep_thd_is_SR(const void* thd);
extern "C" void wsrep_handle_SR_rollback(void *BF_thd_ptr, void *victim_thd_ptr);
/* Return XID associated to thd */
extern "C" void wsrep_thd_xid(const void* thd, void* xid, size_t size);
/* Return thd retry counter */
extern "C" int wsrep_thd_retry_counter(THD*);
/* Signal thd to awake from wait */
extern "C" void wsrep_thd_awake(const void* thd, my_bool signal);
/* BF abort victim_thd */
extern "C" my_bool wsrep_thd_bf_abort(const void* bf_thd, void* victim_thd,
                                      my_bool signal);
/* Return true if left thd is ordered before right thd */
extern "C" my_bool wsrep_thd_order_before(const void* left, const void* right);
/* Return true if thd should skip locking. This means that the thd
   is operating on shared resource inside commit order critical section. */
extern "C" my_bool wsrep_thd_skip_locking(const void*);
/* Return true if thd is aborting */
extern "C" my_bool wsrep_thd_is_aborting(const void*);


enum Wsrep_key_type
{
    wsrep_key_shared,
    wsrep_key_semi_shared,
    wsrep_key_semi_exclusive,
    wsrep_key_exclusive
};
struct wsrep_key;
struct wsrep_key_array;
extern "C" int wsrep_thd_append_key(void*,
                                    const struct wsrep_key* key,
                                    int n_keys,
                                    enum Wsrep_key_type);
#ifdef __cplusplus
#include <string>
extern const std::string sr_table_name_full_str;
#endif /* __cplusplus */

#endif
#endif /* MYSQL_SERVICE_WSREP_INCLUDED */
