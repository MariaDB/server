/* Copyright 2008-2012 Codership Oy <http://www.codership.com>

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
//#include <mysql.h>
#include <sql_priv.h>
#include "../wsrep/wsrep_api.h"
//#include <sql_class.h>

class set_var;
class THD;

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
extern my_bool     wsrep_debug;
extern my_bool     wsrep_convert_LOCK_to_trx;
extern ulong       wsrep_retry_autocommit;
extern my_bool     wsrep_auto_increment_control;
extern my_bool     wsrep_drupal_282555_workaround;
extern my_bool     wsrep_incremental_data_collection;
extern const char* wsrep_sst_method;
extern const char* wsrep_sst_receive_address;
extern       char* wsrep_sst_auth;
extern const char* wsrep_sst_donor;
extern const char* wsrep_start_position;
extern long long   wsrep_max_ws_size;
extern long        wsrep_max_ws_rows;
extern const char* wsrep_notify_cmd;
extern my_bool     wsrep_certify_nonPK;
extern long        wsrep_max_protocol_version;
extern long        wsrep_protocol_version;
extern ulong       wsrep_forced_binlog_format;
extern ulong       wsrep_OSU_method_options;
extern my_bool     wsrep_recovery;
extern my_bool     wsrep_replicate_myisam;

enum enum_wsrep_OSU_method { WSREP_OSU_TOI, WSREP_OSU_RSU };

// MySQL status variables
extern my_bool     wsrep_connected;
extern my_bool     wsrep_ready;
extern const char* wsrep_cluster_state_uuid;
extern long long   wsrep_cluster_conf_id;
extern const char* wsrep_cluster_status;
extern long        wsrep_cluster_size;
extern long        wsrep_local_index;
extern const char* wsrep_provider_name;
extern const char* wsrep_provider_version;
extern const char* wsrep_provider_vendor;
extern int         wsrep_show_status(THD *thd, SHOW_VAR *var, char *buff);

#define WSREP_SST_ADDRESS_AUTO "AUTO"
// MySQL variables funcs

#define CHECK_ARGS   (sys_var *self, THD* thd, set_var *var)
#define UPDATE_ARGS  (sys_var *self, THD* thd, enum_var_type type)
#define DEFAULT_ARGS (THD* thd, enum_var_type var_type)
#define INIT_ARGS    (const char* opt)

extern int  wsrep_init_vars();

extern bool wsrep_on_update                  UPDATE_ARGS;
extern void wsrep_causal_reads_update        UPDATE_ARGS;
extern bool wsrep_start_position_check       CHECK_ARGS;
extern bool wsrep_start_position_update      UPDATE_ARGS;
extern void wsrep_start_position_init        INIT_ARGS;

extern bool wsrep_provider_check             CHECK_ARGS;
extern bool wsrep_provider_update            UPDATE_ARGS;
extern void wsrep_provider_init              INIT_ARGS;

extern bool wsrep_provider_options_check     CHECK_ARGS;
extern bool wsrep_provider_options_update    UPDATE_ARGS;
extern void wsrep_provider_options_init      INIT_ARGS;

extern bool wsrep_cluster_address_check      CHECK_ARGS;
extern bool wsrep_cluster_address_update     UPDATE_ARGS;
extern void wsrep_cluster_address_init       INIT_ARGS;

extern bool wsrep_cluster_name_check         CHECK_ARGS;
extern bool wsrep_cluster_name_update        UPDATE_ARGS;

extern bool wsrep_node_name_check            CHECK_ARGS;
extern bool wsrep_node_name_update           UPDATE_ARGS;

extern bool wsrep_node_address_check         CHECK_ARGS;
extern bool wsrep_node_address_update        UPDATE_ARGS;
extern void wsrep_node_address_init          INIT_ARGS;

extern bool wsrep_sst_method_check           CHECK_ARGS;
extern bool wsrep_sst_method_update          UPDATE_ARGS;
extern void wsrep_sst_method_init            INIT_ARGS;

extern bool wsrep_sst_receive_address_check  CHECK_ARGS;
extern bool wsrep_sst_receive_address_update UPDATE_ARGS;

extern bool wsrep_sst_auth_check             CHECK_ARGS;
extern bool wsrep_sst_auth_update            UPDATE_ARGS;
extern void wsrep_sst_auth_init              INIT_ARGS;

extern bool wsrep_sst_donor_check            CHECK_ARGS;
extern bool wsrep_sst_donor_update           UPDATE_ARGS;


extern bool wsrep_init_first(); // initialize wsrep before storage
                                // engines (true) or after (false)
extern int   wsrep_init();
extern void  wsrep_deinit();
extern void  wsrep_recover();

/* wsrep initialization sequence at startup
 * @param first wsrep_init_first() value */
extern void wsrep_init_startup(bool first);

extern void wsrep_close_client_connections(my_bool wait_to_end);
extern void wsrep_close_applier(THD *thd);
extern void wsrep_wait_appliers_close(THD *thd); 
extern void wsrep_create_appliers(long threads = wsrep_slave_threads);
extern void wsrep_create_rollbacker();
extern void wsrep_kill_mysql(THD *thd);

/* new defines */
extern void wsrep_stop_replication(THD *thd);
extern bool wsrep_start_replication();
extern bool wsrep_causal_wait(THD* thd);
extern int  wsrep_check_opts (int argc, char* const* argv);
extern void wsrep_prepend_PATH (const char* path);

/* Other global variables */
extern wsrep_seqno_t wsrep_locked_seqno;

#define WSREP_ON \
  (global_system_variables.wsrep_on)

#define WSREP(thd) \
  (WSREP_ON && (thd && thd->variables.wsrep_on))

#define WSREP_CLIENT(thd) \
    (WSREP(thd) && thd->wsrep_client_thread)

#define WSREP_EMULATE_BINLOG(thd) \
  (WSREP(thd) && wsrep_emulate_bin_log)

// MySQL logging functions don't seem to understand long long length modifer.
// This is a workaround. It also prefixes all messages with "WSREP"
#define WSREP_LOG(fun, ...)                                       \
    {                                                             \
        char msg[256] = {'\0'};                                   \
        snprintf(msg, sizeof(msg) - 1, ## __VA_ARGS__);           \
        fun("WSREP: %s", msg);                                    \
    }

#define WSREP_DEBUG(...)                                                \
    if (wsrep_debug)     WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_INFO(...)  WSREP_LOG(sql_print_information, ##__VA_ARGS__)
#define WSREP_WARN(...)  WSREP_LOG(sql_print_warning,     ##__VA_ARGS__)
#define WSREP_ERROR(...) WSREP_LOG(sql_print_error,       ##__VA_ARGS__)

/*! Synchronizes applier thread start with init thread */
extern void wsrep_sst_grab();
/*! Init thread waits for SST completion */
extern bool wsrep_sst_wait();
/*! Signals wsrep that initialization is complete, writesets can be applied */
extern void wsrep_sst_continue();

extern void wsrep_SE_init_grab(); /*! grab init critical section */
extern void wsrep_SE_init_wait(); /*! wait for SE init to complete */
extern void wsrep_SE_init_done(); /*! signal that SE init is complte */
extern void wsrep_SE_initialized(); /*! mark SE initialization complete */

extern void wsrep_ready_wait();

enum wsrep_trx_status {
    WSREP_TRX_OK,
    WSREP_TRX_ROLLBACK,
    WSREP_TRX_ERROR,
  };

extern enum wsrep_trx_status
wsrep_run_wsrep_commit(THD *thd, handlerton *hton, bool all);
class Ha_trx_info;
struct THD_TRANS;
void wsrep_register_hton(THD* thd, bool all);

void wsrep_replication_process(THD *thd);
void wsrep_rollback_process(THD *thd);
void wsrep_brute_force_killer(THD *thd);
int  wsrep_hire_brute_force_killer(THD *thd, uint64_t trx_id);
extern "C" bool wsrep_consistency_check(void *thd_ptr);
extern "C" int wsrep_thd_is_brute_force(void *thd_ptr);
extern "C" int wsrep_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr, 
                               my_bool signal);
extern "C" int wsrep_thd_in_locking_session(void *thd_ptr);
void *wsrep_prepare_bf_thd(THD *thd);
void wsrep_return_from_bf_mode(void *shadow, THD *thd);

/* this is visible for client build so that innodb plugin gets this */
typedef struct wsrep_aborting_thd {
  struct wsrep_aborting_thd *next;
  THD *aborting_thd;
} *wsrep_aborting_thd_t;

extern mysql_mutex_t LOCK_wsrep_ready;
extern mysql_cond_t COND_wsrep_ready;
extern mysql_mutex_t LOCK_wsrep_sst;
extern mysql_cond_t COND_wsrep_sst;
extern mysql_mutex_t LOCK_wsrep_sst_init;
extern mysql_cond_t COND_wsrep_sst_init;
extern mysql_mutex_t LOCK_wsrep_rollback;
extern mysql_cond_t COND_wsrep_rollback;
extern int wsrep_replaying;
extern mysql_mutex_t LOCK_wsrep_replaying;
extern mysql_cond_t COND_wsrep_replaying;
extern wsrep_aborting_thd_t wsrep_aborting_thd;
extern MYSQL_PLUGIN_IMPORT my_bool wsrep_debug;
extern my_bool wsrep_convert_LOCK_to_trx;
extern ulong   wsrep_retry_autocommit;
extern my_bool wsrep_emulate_bin_log;
extern my_bool wsrep_auto_increment_control;
extern my_bool wsrep_drupal_282555_workaround;
extern long long wsrep_max_ws_size;
extern long      wsrep_max_ws_rows;
extern int       wsrep_to_isolation;
extern my_bool wsrep_certify_nonPK;

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

struct TABLE_LIST;
int wsrep_to_isolation_begin(THD *thd, char *db_, char *table_,
                             const TABLE_LIST* table_list);
void wsrep_to_isolation_end(THD *thd);

void wsrep_prepare_bf_thd(THD *thd, struct wsrep_thd_shadow*);
void wsrep_return_from_bf_mode(THD *thd, struct wsrep_thd_shadow*);
int wsrep_to_buf_helper(
  THD* thd, const char *query, uint query_len, uchar** buf, uint* buf_len);
int wsrep_create_sp(THD *thd, uchar** buf, uint* buf_len);
int wsrep_create_trigger_query(THD *thd, uchar** buf, uint* buf_len);
int wsrep_create_event_query(THD *thd, uchar** buf, uint* buf_len);

const wsrep_uuid_t* wsrep_cluster_uuid();
struct xid_t;
void wsrep_set_SE_checkpoint(xid_t*);

void wsrep_xid_init(xid_t*, const wsrep_uuid_t*, wsrep_seqno_t);
const wsrep_uuid_t* wsrep_xid_uuid(const xid_t*);
wsrep_seqno_t wsrep_xid_seqno(const xid_t*);
extern "C" int wsrep_is_wsrep_xid(const void* xid);

#endif /* WSREP_MYSQLD_H */
