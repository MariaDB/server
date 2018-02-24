/* Copyright 2008-2015 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.x1

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <sql_plugin.h> // SHOW_MY_BOOL
#include "mariadb.h"
#include <mysqld.h>
#include <transaction.h>
#include <sql_class.h>
#include <sql_parse.h>
#include <sql_base.h> /* find_temporary_table() */
#include "slave.h"
#include "rpl_mi.h"
#include "sql_repl.h"
#include "rpl_filter.h"
#include "sql_callback.h"
#include "sp_head.h"
#include "sql_show.h"
#include "sp.h"
#include "wsrep_priv.h"
#include "wsrep_thd.h"
#include "wsrep_sst.h"
#include "wsrep_utils.h"
#include "wsrep_var.h"
#include "wsrep_binlog.h"
#include "wsrep_applier.h"
#include "wsrep_sr.h"
#include "wsrep_sr_file.h"
#include "wsrep_sr_table.h"
#include "wsrep_schema.h"
#include "wsrep_thd_pool.h"
#include "wsrep_xid.h"
#include "wsrep_trans_observer.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include "log_event.h"
#include <slave.h>
#include "sql_plugin.h"                         /* wsrep_plugins_pre_init() */

wsrep_t *wsrep                  = NULL;
/*
  wsrep_emulate_bin_log is a flag to tell that binlog has not been configured.
  wsrep needs to get binlog events from transaction cache even when binlog is
  not enabled, wsrep_emulate_bin_log opens needed code paths to make this
  possible
*/
my_bool wsrep_emulate_bin_log   = FALSE; // activating parts of binlog interface
#ifdef GTID_SUPPORT
/* Sidno in global_sid_map corresponding to group uuid */
rpl_sidno wsrep_sidno= -1;
#endif /* GTID_SUPPORT */
my_bool wsrep_preordered_opt= FALSE;

/*
 * Begin configuration options
 */

extern my_bool plugins_are_initialized;
extern uint kill_cached_threads;
extern mysql_cond_t COND_thread_cache;

/* System variables. */
const char *wsrep_provider;
const char *wsrep_provider_options;
const char *wsrep_cluster_address;
const char *wsrep_cluster_name;
const char *wsrep_node_name;
const char *wsrep_node_address;
const char *wsrep_node_incoming_address;
const char *wsrep_start_position;
ulong   wsrep_reject_queries;
const char *wsrep_data_home_dir;
const char *wsrep_dbug_option;
const char *wsrep_notify_cmd;
const char *wsrep_sst_method;
const char *wsrep_sst_receive_address;
const char *wsrep_sst_donor;
const char *wsrep_sst_auth;
my_bool wsrep_debug;                            // Enable debug level logging
my_bool wsrep_convert_LOCK_to_trx;              // Convert locking sessions to trx
my_bool wsrep_auto_increment_control;           // Control auto increment variables
my_bool wsrep_drupal_282555_workaround;         // Retry autoinc insert after dupkey
my_bool wsrep_certify_nonPK;                    // Certify, even when no primary key
my_bool wsrep_recovery;                         // Recovery
my_bool wsrep_replicate_myisam;                 // Enable MyISAM replication
my_bool wsrep_log_conflicts;
my_bool wsrep_load_data_splitting= 0;           // Commit load data every 10K intervals
my_bool wsrep_slave_UK_checks;                  // Slave thread does UK checks
my_bool wsrep_slave_FK_checks;                  // Slave thread does FK checks
my_bool wsrep_sst_donor_rejects_queries;
my_bool wsrep_restart_slave;                    // Should mysql slave thread be
                                                // restarted, when node joins back?
my_bool wsrep_desync;                           // De(re)synchronize the node from the
                                                // cluster
long wsrep_slave_threads;                       // No. of slave appliers threads
ulong wsrep_retry_autocommit;                   // Retry aborted autocommit trx
ulong wsrep_max_ws_size;                        // Max allowed ws (RBR buffer) size
ulong wsrep_max_ws_rows;                        // Max number of rows in ws
ulong wsrep_forced_binlog_format;
ulong wsrep_mysql_replication_bundle;
bool wsrep_gtid_mode;                           // Use wsrep_gtid_domain_id
                                                // for galera transactions?
uint32 wsrep_gtid_domain_id;                    // gtid_domain_id for galera
                                                // transactions

/* Other configuration variables and their default values. */
my_bool wsrep_incremental_data_collection= 0;   // Incremental data collection
my_bool wsrep_restart_slave_activated= 0;       // Node has dropped, and slave
                                                // restart will be needed
bool wsrep_new_cluster= false;                  // Bootstrap the cluster?
int wsrep_slave_count_change= 0;                // No. of appliers to stop/start
int wsrep_to_isolation= 0;                      // No. of active TO isolation threads
long wsrep_max_protocol_version= 4;             // Maximum protocol version to use
long int  wsrep_protocol_version= wsrep_max_protocol_version;
ulong wsrep_trx_fragment_size= 0;               // size limit for fragmenting
                                                // 0 = no fragmenting
ulong wsrep_trx_fragment_unit= WSREP_FRAG_BYTES;
                                                // unit for fragment size
ulong wsrep_SR_store_type= WSREP_SR_STORE_TABLE;
uint  wsrep_ignore_apply_errors= 0;


/*
 * End configuration options
 */

/*
 * Other wsrep global variables.
 */

mysql_mutex_t LOCK_wsrep_ready;
mysql_cond_t  COND_wsrep_ready;
mysql_mutex_t LOCK_wsrep_sst;
mysql_cond_t  COND_wsrep_sst;
mysql_mutex_t LOCK_wsrep_sst_init;
mysql_cond_t  COND_wsrep_sst_init;
mysql_mutex_t LOCK_wsrep_replaying;
mysql_cond_t  COND_wsrep_replaying;
mysql_mutex_t LOCK_wsrep_slave_threads;
mysql_mutex_t LOCK_wsrep_desync;
mysql_mutex_t LOCK_wsrep_config_state;
mysql_mutex_t LOCK_wsrep_SR_pool;
mysql_mutex_t LOCK_wsrep_SR_store;
mysql_mutex_t LOCK_wsrep_thd_pool;

int wsrep_replaying= 0;
ulong  wsrep_running_threads = 0; // # of currently running wsrep threads
ulong  my_bind_addr;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_wsrep_thd,
  key_LOCK_wsrep_replaying, key_LOCK_wsrep_ready, key_LOCK_wsrep_sst,
  key_LOCK_wsrep_sst_thread, key_LOCK_wsrep_sst_init,
  key_LOCK_wsrep_slave_threads, key_LOCK_wsrep_desync,
  key_LOCK_wsrep_config_state,
  key_LOCK_wsrep_SR_pool,
  key_LOCK_wsrep_SR_store, key_LOCK_wsrep_thd_pool, key_LOCK_wsrep_nbo,
  key_LOCK_wsrep_thd_queue;

PSI_cond_key key_COND_wsrep_thd,
  key_COND_wsrep_replaying, key_COND_wsrep_ready, key_COND_wsrep_sst,
  key_COND_wsrep_sst_init, key_COND_wsrep_sst_thread,
  key_COND_wsrep_nbo, key_COND_wsrep_thd_queue;
  

PSI_file_key key_file_wsrep_gra_log;

static PSI_mutex_info wsrep_mutexes[]=
{
  { &key_LOCK_wsrep_ready, "LOCK_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_LOCK_wsrep_sst_init, "LOCK_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_thd, "THD::LOCK_wsrep_thd", 0},
  { &key_LOCK_wsrep_replaying, "LOCK_wsrep_replaying", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_slave_threads, "LOCK_wsrep_slave_threads", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_desync, "LOCK_wsrep_desync", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_config_state, "LOCK_wsrep_config_state", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_SR_pool, "LOCK_wsrep_SR_pool", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_SR_store, "LOCK_wsrep_SR_store", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_thd_pool, "LOCK_wsrep_thd_pool", PSI_FLAG_GLOBAL}
};

static PSI_cond_info wsrep_conds[]=
{
  { &key_COND_wsrep_thd, "COND_wsrep_thd", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_ready, "COND_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst, "COND_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_init, "COND_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_COND_wsrep_replaying, "COND_wsrep_replaying", PSI_FLAG_GLOBAL}
};

static PSI_file_info wsrep_files[]=
{
  { &key_file_wsrep_gra_log, "wsrep_gra_log", 0}
};
#endif

my_bool wsrep_inited                   = 0; // initialized ?

static wsrep_uuid_t node_uuid= WSREP_UUID_UNDEFINED;
static wsrep_uuid_t cluster_uuid = WSREP_UUID_UNDEFINED;
static char         cluster_uuid_str[40]= { 0, };
static const char*  cluster_status_str[WSREP_VIEW_MAX] =
{
    "Primary",
    "non-Primary",
    "Disconnected"
};

static char provider_name[256]= { 0, };
static char provider_version[256]= { 0, };
static char provider_vendor[256]= { 0, };

/*
 * wsrep status variables
 */
my_bool     wsrep_connected          = FALSE;
my_bool     wsrep_ready              = FALSE; // node can accept queries
const char* wsrep_cluster_state_uuid = cluster_uuid_str;
long long   wsrep_cluster_conf_id    = WSREP_SEQNO_UNDEFINED;
const char* wsrep_cluster_status = cluster_status_str[WSREP_VIEW_DISCONNECTED];
long        wsrep_cluster_size       = 0;
long        wsrep_local_index        = -1;
long long   wsrep_local_bf_aborts    = 0;
const char* wsrep_provider_name      = provider_name;
const char* wsrep_provider_version   = provider_version;
const char* wsrep_provider_vendor    = provider_vendor;
char* wsrep_provider_capabilities    = NULL;
char* wsrep_cluster_capabilities     = NULL;
/* End wsrep status variables */

wsrep_uuid_t               local_uuid        = WSREP_UUID_UNDEFINED;
wsrep_seqno_t              local_seqno       = WSREP_SEQNO_UNDEFINED;
wsp::node_status           local_status;
static wsrep_view_status_t local_view_status = WSREP_VIEW_NON_PRIMARY;

class SR_storage_file  *wsrep_SR_store_file = NULL;
class SR_storage_table *wsrep_SR_store_table = NULL;
class SR_storage       *wsrep_SR_store = NULL;

/*
 */
#define WSREP_THD_POOL_SIZE 16
Wsrep_thd_pool* wsrep_thd_pool= 0;
Wsrep_schema *wsrep_schema= 0;

wsp::Config_state *wsrep_config_state;

// Boolean denoting if server is in initial startup phase. This is needed
// to make sure that main thread waiting in wsrep_sst_wait() is signaled
// if there was no state gap on receiving first view event.
static my_bool   wsrep_startup = TRUE;


static void wsrep_log_cb(wsrep_log_level_t level, const char *msg) {
  switch (level) {
  case WSREP_LOG_INFO:
    sql_print_information("WSREP: %s", msg);
    break;
  case WSREP_LOG_WARN:
    sql_print_warning("WSREP: %s", msg);
    break;
  case WSREP_LOG_ERROR:
  case WSREP_LOG_FATAL:
    sql_print_error("WSREP: %s", msg);
    break;
  case WSREP_LOG_DEBUG:
    if (wsrep_debug) sql_print_information ("[Debug] WSREP: %s", msg);
  default:
    break;
  }
}

void wsrep_log(void (*fun)(const char *, ...), const char *format, ...)
{
  va_list args;
  char msg[1024];
  va_start(args, format);
  vsnprintf(msg, sizeof(msg) - 1, format, args);
  va_end(args);
  (fun)("WSREP: %s", msg);
}


static void wsrep_log_states (wsrep_log_level_t   const level,
                              const wsrep_uuid_t* const group_uuid,
                              wsrep_seqno_t       const group_seqno,
                              const wsrep_uuid_t* const loc_uuid,
                              wsrep_seqno_t       const loc_seqno)
{
  char uuid_str[37];
  char msg[256];

  wsrep_uuid_print (group_uuid, uuid_str, sizeof(uuid_str));
  snprintf (msg, 255, "WSREP: Group state: %s:%lld",
            uuid_str, (long long)group_seqno);
  wsrep_log_cb (level, msg);

  wsrep_uuid_print (loc_uuid, uuid_str, sizeof(uuid_str));
  snprintf (msg, 255, "WSREP: Local state: %s:%lld",
            uuid_str, (long long)loc_seqno);
  wsrep_log_cb (level, msg);
}

#ifdef GTID_SUPPORT
void wsrep_init_sidno(const wsrep_uuid_t& wsrep_uuid)
{
  /* generate new Sid map entry from inverted uuid */
  rpl_sid sid;
  if (wsrep_protocol_version >= 4)
  {
    sid.copy_from(wsrep_uuid.data);
  }
  else
  {
    wsrep_uuid_t ltid_uuid;
    for (size_t i= 0; i < sizeof(ltid_uuid.data); ++i)
    {
      ltid_uuid.data[i] = ~wsrep_uuid.data[i];
    }
    sid.copy_from(ltid_uuid.data);
  }
  global_sid_lock->wrlock();
  wsrep_sidno= global_sid_map->add_sid(sid);
  WSREP_INFO("Initialized wsrep sidno %d", wsrep_sidno);
  global_sid_lock->unlock();
}
#endif /* GTID_SUPPORT */

void wsrep_init_schema()
{
  DBUG_ASSERT(!wsrep_schema);

  WSREP_INFO("wsrep_init_schema_and_SR %p %p", wsrep_schema, wsrep_SR_store);
  if (!wsrep_schema)
  {
    if (wsrep_before_SE()) {
      DBUG_ASSERT(!wsrep_thd_pool);
      wsrep_thd_pool= new Wsrep_thd_pool(WSREP_THD_POOL_SIZE);
    }
    wsrep_schema= new Wsrep_schema(wsrep_thd_pool);
    if (wsrep_schema->init())
    {
      WSREP_ERROR("Failed to init wsrep schema");
      unireg_abort(1);
    }
  }
}

void wsrep_init_SR()
{
  /* initialize SR pools, now that innodb has initialized */
  if (wsrep_SR_store && wsrep_SR_store->init(wsrep_cluster_state_uuid,
                                             wsrep_schema)) {
    WSREP_ERROR("wsrep SR persistency store initialization failed");
    unireg_abort(1);
  }
  else {
    if (wsrep_SR_store && wsrep_SR_store->restore(0)) {
      WSREP_ERROR("wsrep SR persistency restore failed");
      unireg_abort(1);
    }
  }
}

int wsrep_replay_from_SR_store(THD* thd, const wsrep_trx_meta_t& meta)
{
  DBUG_ENTER("wsrep_replay_from_SR_store");
  if (!wsrep_SR_store) {
    WSREP_ERROR("no SR persistency store defined, can't replay");
    DBUG_RETURN(1);
  }

  int ret= wsrep_SR_store->replay_trx(thd, meta);
  DBUG_RETURN(ret);
}

static void wsrep_rollback_SR_connections()
{
  THD *tmp;
  mysql_mutex_lock(&LOCK_thread_count);

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    mysql_mutex_lock(&tmp->LOCK_wsrep_thd);
    if (tmp->wsrep_client_thread && tmp->wsrep_is_streaming())
    {
      tmp->set_wsrep_conflict_state(MUST_ABORT);
      if (tmp->wsrep_query_state() == QUERY_IDLE)
      {
        wsrep_fire_rollbacker(tmp);
      }
      /*
        No need to send rollback fragment for this trx: slaves rollback
        all SR transactions whose master goes non-Primary.
      */
      tmp->wsrep_SR_rollback_replicated_for_trx= tmp->wsrep_trx_id();
    }
    mysql_mutex_unlock(&tmp->LOCK_wsrep_thd);
  }

  mysql_mutex_unlock(&LOCK_thread_count);
}

/** Export the WSREP provider's capabilities as a human readable string.
 * The result is saved in a dynamically allocated string of the form:
 * :cap1:cap2:cap3:
 */
static void wsrep_capabilities_export(wsrep_cap_t const cap, char** str)
{
  static const char* names[] =
  {
    /* Keep in sync with wsrep/wsrep_api.h WSREP_CAP_* macros. */
    "MULTI_MASTER",
    "CERTIFICATION",
    "PARALLEL_APPLYING",
    "TRX_REPLAY",
    "ISOLATION",
    "PAUSE",
    "CAUSAL_READS",
    "CAUSAL_TRX",
    "INCREMENTAL_WRITESET",
    "SESSION_LOCKS",
    "DISTRIBUTED_LOCKS",
    "CONSISTENCY_CHECK",
    "UNORDERED",
    "ANNOTATION",
    "PREORDERED",
    "STREAMING",
    "SNAPSHOT",
    "NBO",
  };

  std::string s;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    if (cap & (1ULL << i))
    {
      if (s.empty())
      {
        s = ":";
      }
      s += names[i];
      s += ":";
    }
  }

  /* A read from the string pointed to by *str may be started at any time,
   * so it must never point to free(3)d memory or non '\0' terminated string. */

  char* const previous = *str;

  *str = strdup(s.c_str());

  if (previous != NULL)
  {
    free(previous);
  }
}

wsrep_cb_status_t
wsrep_connected_handler_cb(void* app_ctx,
                           const wsrep_view_info_t* initial_view)
{
  if (initial_view->my_idx < 0) {
    WSREP_ERROR("Invalid index %d in initial view", initial_view->my_idx);
    return WSREP_CB_FAILURE;
  }

  node_uuid= initial_view->members[initial_view->my_idx].id;
  cluster_uuid= initial_view->state_id.uuid;
  wsrep_cluster_status= cluster_status_str[initial_view->status];

  char node_uuid_str[WSREP_UUID_STR_LEN + 1];
  (void)wsrep_uuid_print(&node_uuid, node_uuid_str, sizeof(node_uuid_str));
  (void)wsrep_uuid_print(&cluster_uuid, cluster_uuid_str,
                         sizeof(cluster_uuid_str));

  WSREP_INFO("Connected to cluster %s with id: %s",
             cluster_uuid_str, node_uuid_str);
  return WSREP_CB_SUCCESS;
}

static wsrep_cb_status_t
wsrep_view_handler_cb(void*                    app_ctx,
                      void*                    recv_ctx,
                      const wsrep_view_info_t* view,
                      /* TODO: These are unused, should be removed?*/
                      const char*              state,
                      size_t                   state_len)
{
  /* Allow calling view handler from non-applier threads */
  struct st_my_thread_var* tmp_thread_var= 0;
  if (!my_thread_var) {
    my_thread_init();
    tmp_thread_var= my_thread_var;
  }
 
  wsrep_cb_status_t ret= WSREP_CB_SUCCESS;
  wsrep_member_status_t new_status= local_status.get();

  if (memcmp(&cluster_uuid, &view->state_id.uuid, sizeof(wsrep_uuid_t)))
  {
    memcpy((wsrep_uuid_t*)&cluster_uuid, &view->state_id.uuid,
           sizeof(cluster_uuid));

    wsrep_uuid_print (&cluster_uuid, cluster_uuid_str,
                      sizeof(cluster_uuid_str));
  }

  wsrep_cluster_conf_id= view->view;
  wsrep_cluster_status= cluster_status_str[view->status];
  wsrep_cluster_size= view->memb_num;
  wsrep_local_index= view->my_idx;

  if (wsrep_cluster_size > 0) {
    WSREP_INFO("New cluster view: global state: %s:%lld, view# %lld: %s, "
               "number of nodes: %ld, my index: %ld, protocol version %d",
               wsrep_cluster_state_uuid, (long long)view->state_id.seqno,
               (long long)wsrep_cluster_conf_id, wsrep_cluster_status,
               wsrep_cluster_size, wsrep_local_index, view->proto_ver);
  }
  else {
    WSREP_INFO("Provider closed.");
  }

  /* Proceed further only if view is PRIMARY */
  if (WSREP_VIEW_PRIMARY != view->status)
  {
#ifdef HAVE_QUERY_CACHE
    // query cache must be initialised by now
    query_cache.flush();
#endif /* HAVE_QUERY_CACHE */

    wsrep_ready_set(FALSE);
    new_status= WSREP_MEMBER_UNDEFINED;
    /* Always record local_uuid and local_seqno in non-prim since this
     * may lead to re-initializing provider and start position is
     * determined according to these variables */
    // WRONG! local_uuid should be the last primary configuration uuid we were
    // a member of. local_seqno should be updated in commit calls.
    // local_uuid= cluster_uuid;
    // local_seqno= view->first - 1;
    goto out;
  }

  switch (view->proto_ver)
  {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4: // SR and view callback change
      // version change
      if (view->proto_ver != wsrep_protocol_version)
      {
          my_bool wsrep_ready_saved= wsrep_ready_get();
          wsrep_ready_set(FALSE);
          WSREP_INFO("closing client connections for "
                     "protocol change %ld -> %d",
                     wsrep_protocol_version, view->proto_ver);
          wsrep_close_client_connections(TRUE);
          wsrep_protocol_version= view->proto_ver;
          wsrep_ready_set(wsrep_ready_saved);
      }
      break;
  default:
      WSREP_ERROR("Unsupported application protocol version: %d",
                  view->proto_ver);
      unireg_abort(1);
  }

  if (memcmp(&cluster_uuid, &view->state_id.uuid, sizeof(wsrep_uuid_t)))
  {
    memcpy((wsrep_uuid_t*)&cluster_uuid, &view->state_id.uuid,
           sizeof(cluster_uuid));

    wsrep_uuid_print (&cluster_uuid, cluster_uuid_str,
                      sizeof(cluster_uuid_str));
  }

  /*
   *  NOTE: Initialize wsrep_group_uuid here only if it wasn't initialized
   *  before - OR - it was reinitilized on startup (lp:992840)
   */
  if (wsrep_startup)
  {
      if (wsrep_before_SE())
      {
        wsrep_SE_init_grab();
        // Signal mysqld init thread to continue
        wsrep_sst_complete (&cluster_uuid, view->state_id.seqno, false);
        // and wait for SE initialization
        if (wsrep_SE_init_wait())
        {
          ret= WSREP_CB_FAILURE;
          goto out;
        }
      }

      wsrep_seqno_t seqno;

      /* Init storage engine XIDs from first view */
      if (view->memb_num == 1)
      {
        seqno= view->state_id.seqno;
        wsrep_set_SE_checkpoint(WSREP_UUID_UNDEFINED, WSREP_SEQNO_UNDEFINED);
        wsrep_set_SE_checkpoint(cluster_uuid, seqno);
      }
      else
      {
        // must get from state transfer
        wsrep_uuid_t unused;
        wsrep_get_SE_checkpoint(unused, seqno);
      }

      wsrep_verify_SE_checkpoint(cluster_uuid, seqno);
      new_status= WSREP_MEMBER_JOINED;
#ifdef GTID_SUPPORT
      wsrep_init_sidno(local_uuid);
#endif /* GTID_SUPPORT */
  }
  else
  {
      wsrep_get_SE_checkpoint(local_uuid, local_seqno);

      // just a sanity check
      if (wsrep_uuid_compare(&local_uuid, &cluster_uuid) != 0)
      {
          WSREP_ERROR("Undetected state gap. Can't continue.");
          wsrep_log_states(WSREP_LOG_FATAL, &cluster_uuid, view->state_id.seqno,
                           &local_uuid, local_seqno);
          unireg_abort(1);
      }
  }

  if (wsrep_auto_increment_control && view->my_idx >= 0)
  {
      global_system_variables.auto_increment_offset= view->my_idx + 1;
      global_system_variables.auto_increment_increment= view->memb_num;
  }

  { /* capabilities may be updated on new configuration */
      wsrep_cap_t const caps(view->capabilities);

      my_bool const idc((caps & WSREP_CAP_INCREMENTAL_WRITESET) != 0);
      if (TRUE == wsrep_incremental_data_collection && FALSE == idc)
      {
        WSREP_WARN("Unsupported protocol downgrade: "
                   "incremental data collection disabled. Expect abort.");
      }
      wsrep_incremental_data_collection = idc;

      wsrep_capabilities_export(caps, &wsrep_cluster_capabilities);
  }

  /*
    Initialize wsrep schema and SR
  */
  if (!wsrep_schema) {
    wsrep_init_schema();
  }

  if (wsrep_schema->store_view(view)) {
    WSREP_ERROR("Storing view failed");
    unireg_abort(1);
  }

  /*
    If the recv_ctx is a pointer to thd object we need to store globals
    here as wsrep_schema->store_view() uses temporary thd object and
    writes over thread locals.
   */
  if (recv_ctx) ((THD*) recv_ctx)->store_globals();

  if (wsrep_startup == TRUE) {
    wsrep_init_SR();
  }

  trim_SR_pool((THD*)recv_ctx, view->members, view->memb_num);

  /*
    Transitioning from non-primary to primary view
   */
  if (local_view_status != WSREP_VIEW_PRIMARY) {
    wsrep_rollback_SR_connections();
  }

out:
  if (view->status == WSREP_VIEW_PRIMARY) wsrep_startup= FALSE;
  local_status.set(new_status, view);
  local_view_status = view->status;

  if (tmp_thread_var) {
    my_thread_end();
  }

  return ret;
}

/* Verifies that SE position is consistent with the group position
 * and initializes other variables */
void wsrep_verify_SE_checkpoint(const wsrep_uuid_t& uuid,
                                wsrep_seqno_t const seqno)
{
  wsrep_get_SE_checkpoint(local_uuid, local_seqno);

  if (memcmp(&local_uuid, &uuid, sizeof (wsrep_uuid_t)) ||
             local_seqno > seqno)
  {
    WSREP_ERROR("Failed to update SE checkpoint. Can't continue.");
    wsrep_log_states(WSREP_LOG_FATAL, &uuid, seqno,
                     &local_uuid, local_seqno);
    assert(0);
    unireg_abort(1);
  }

#ifdef GTID_SUPPORT
  wsrep_init_sidno(local_uuid);
#endif
}

static wsrep_cb_status_t
wsrep_sst_request_cb (void**                   sst_req,
                      size_t*                  sst_req_len)
{
  *sst_req     = NULL;
  *sst_req_len = 0;

  wsrep_member_status_t new_status= local_status.get();

  WSREP_INFO("Preparing to receive SST.");

  /* After that wsrep will call wsrep_sst_prepare. */
  /* keep ready flag 0 until we receive the snapshot */
  wsrep_ready_set(FALSE);

  /* Close client connections to ensure that they don't interfere
   * with SST. Necessary only if storage engines are initialized
   * before SST.
   * TODO: Just killing all ongoing transactions should be enough
   * since wsrep_ready is OFF and no new transactions can start.
   */
  if (!wsrep_before_SE())
  {
    WSREP_DEBUG("[debug]: closing client connections for SST");
    wsrep_close_client_connections(TRUE);
  }

  ssize_t const req_len = wsrep_sst_prepare (sst_req);

  if (req_len < 0)
  {
    WSREP_ERROR("SST preparation failed: %zd (%s)", -req_len,
                strerror(-req_len));
    new_status= WSREP_MEMBER_UNDEFINED;
  }
  else
  {
    assert(*sst_req != NULL || 0 == req_len);
    *sst_req_len= req_len;
    new_status= WSREP_MEMBER_JOINER;
  }

  local_status.set(new_status);

  return WSREP_CB_SUCCESS;
}

my_bool wsrep_ready_set (my_bool x)
{
  WSREP_DEBUG("Setting wsrep_ready to %d", x);
  if (mysql_mutex_lock (&LOCK_wsrep_ready)) abort();
  my_bool ret= (wsrep_ready != x);
  if (ret)
  {
    wsrep_ready= x;
    mysql_cond_signal (&COND_wsrep_ready);
  }
  mysql_mutex_unlock (&LOCK_wsrep_ready);
  return ret;
}

my_bool wsrep_ready_get (void)
{
  if (mysql_mutex_lock (&LOCK_wsrep_ready)) abort();
  my_bool ret= wsrep_ready;
  mysql_mutex_unlock (&LOCK_wsrep_ready);
  return ret;
}

int wsrep_show_ready(THD *thd, SHOW_VAR *var, char *buff)
{
  var->type= SHOW_MY_BOOL;
  var->value= buff;
  *((my_bool *)buff)= wsrep_ready_get();
  return 0;
}

// Wait until wsrep has reached ready state
void wsrep_ready_wait ()
{
  if (mysql_mutex_lock (&LOCK_wsrep_ready)) abort();
  while (!wsrep_ready)
  {
    WSREP_INFO("Waiting to reach ready state");
    mysql_cond_wait (&COND_wsrep_ready, &LOCK_wsrep_ready);
  }
  WSREP_INFO("ready state reached");
  mysql_mutex_unlock (&LOCK_wsrep_ready);
}

static wsrep_cb_status_t wsrep_synced_cb(void* app_ctx)
{
  WSREP_INFO("Synchronized with group, ready for connections");
  wsrep_config_state->set(WSREP_MEMBER_SYNCED);
  my_bool signal_main= wsrep_ready_set(TRUE);
  local_status.set(WSREP_MEMBER_SYNCED);

  if (signal_main)
  {
      wsrep_SE_init_grab();
      // Signal mysqld init thread to continue
      wsrep_sst_complete (&local_uuid, local_seqno, false);
      // and wait for SE initialization
      if (wsrep_SE_init_wait())
      {
        return WSREP_CB_FAILURE;
      }
  }
  if (wsrep_restart_slave_activated)
  {
    int rcode;
    WSREP_INFO("MariaDB slave restart");
    wsrep_restart_slave_activated= FALSE;

    mysql_mutex_lock(&LOCK_active_mi);
    if ((rcode = start_slave_threads(0,
                                     1 /* need mutex */,
                                     0 /* no wait for start*/,
                                     active_mi,
                                     master_info_file,
                                     relay_log_info_file,
                                     SLAVE_SQL)))
    {
      WSREP_WARN("Failed to create slave threads: %d", rcode);
    }
    mysql_mutex_unlock(&LOCK_active_mi);

  }
  return WSREP_CB_SUCCESS;
}

static void wsrep_init_position()
{
  /* read XIDs from storage engines */
  wsrep_uuid_t uuid;
  wsrep_seqno_t seqno;
  wsrep_get_SE_checkpoint(uuid, seqno);

  if (wsrep_uuid_compare(&uuid, &WSREP_UUID_UNDEFINED) == 0)
  {
    WSREP_INFO("Read nil XID from storage engines, skipping position init");
    return;
  }

  char uuid_str[40] = {0, };
  wsrep_uuid_print(&uuid, uuid_str, sizeof(uuid_str));
  WSREP_INFO("Storage engines initial position: %s:%lld",
             uuid_str, (long long)seqno);

  if (wsrep_uuid_compare(&local_uuid, &WSREP_UUID_UNDEFINED) == 0 &&
      local_seqno == WSREP_SEQNO_UNDEFINED)
  {
    // Initial state
    local_uuid= uuid;
    local_seqno= seqno;
  }
  else if (memcmp(&local_uuid, &uuid, sizeof(local_uuid)) ||
           local_seqno != seqno)
  {
    WSREP_WARN("Initial position was provided by configuration or SST, "
               "avoiding override");
  }
}

extern char* my_bind_addr_str;

int wsrep_init()
{
  int rcode= -1;
  DBUG_ASSERT(wsrep_inited == 0);

  if (strcmp(wsrep_start_position, WSREP_START_POSITION_ZERO) &&
      wsrep_start_position_init(wsrep_start_position))
  {
    assert(0);
    return 1;
  }

  wsrep_sst_auth_init();

  wsrep_ready_set(FALSE);
  assert(wsrep_provider);

  wsrep_init_position();

  if ((rcode= wsrep_load(wsrep_provider, &wsrep, wsrep_log_cb)) != WSREP_OK)
  {
    if (strcasecmp(wsrep_provider, WSREP_NONE))
    {
      WSREP_ERROR("wsrep_load(%s) failed: %s (%d). Reverting to no provider.",
                  wsrep_provider, strerror(rcode), rcode);
      strcpy((char*)wsrep_provider, WSREP_NONE); // damn it's a dirty hack
      return wsrep_init();
    }
    else /* this is for recursive call above */
    {
      WSREP_ERROR("Could not revert to no provider: %s (%d). Need to abort.",
                  strerror(rcode), rcode);
      unireg_abort(1);
    }
  }

  if (!WSREP_PROVIDER_EXISTS)
  {
    // enable normal operation in case no provider is specified
    wsrep_ready_set(TRUE);
    wsrep_inited= 1;
    global_system_variables.wsrep_on = 0;
    wsrep_init_args args;
    args.logger_cb = wsrep_log_cb;
    args.options = (wsrep_provider_options) ?
            wsrep_provider_options : "";
    rcode = wsrep->init(wsrep, &args);
    if (rcode)
    {
      DBUG_PRINT("wsrep",("wsrep::init() failed: %d", rcode));
      WSREP_ERROR("wsrep::init() failed: %d, must shutdown", rcode);
      wsrep->free(wsrep);
      free(wsrep);
      wsrep = NULL;
    }
    return rcode;
  }
  else
  {
    global_system_variables.wsrep_on = 1;
    strncpy(provider_name,
            wsrep->provider_name,    sizeof(provider_name) - 1);
    strncpy(provider_version,
            wsrep->provider_version, sizeof(provider_version) - 1);
    strncpy(provider_vendor,
            wsrep->provider_vendor,  sizeof(provider_vendor) - 1);
  }

  if (wsrep_gtid_mode && opt_bin_log && !opt_log_slave_updates)
  {
    WSREP_ERROR("Option --log-slave-updates is required if "
                "binlog is enabled, GTID mode is on and wsrep provider "
                "is specified");
    return -1;
  }

  if (!wsrep_data_home_dir || strlen(wsrep_data_home_dir) == 0)
    wsrep_data_home_dir = mysql_real_data_home;

  /* Initialize node address */
  char node_addr[512]= { 0, };
  size_t const node_addr_max= sizeof(node_addr) - 1;
  if (!wsrep_node_address || !strcmp(wsrep_node_address, ""))
  {
    size_t const ret= wsrep_guess_ip(node_addr, node_addr_max);
    if (!(ret > 0 && ret < node_addr_max))
    {
      WSREP_WARN("Failed to guess base node address. Set it explicitly via "
                 "wsrep_node_address.");
      node_addr[0]= '\0';
    }
  }
  else
  {
    strncpy(node_addr, wsrep_node_address, node_addr_max);
  }

  /* Initialize node's incoming address */
  char inc_addr[512]= { 0, };
  size_t const inc_addr_max= sizeof (inc_addr);

  /*
    In case wsrep_node_incoming_address is either not set or set to AUTO,
    we need to use mysqld's my_bind_addr_str:mysqld_port, lastly fallback
    to wsrep_node_address' value if mysqld's bind-address is not set either.
  */
  if ((!wsrep_node_incoming_address ||
       !strcmp (wsrep_node_incoming_address, WSREP_NODE_INCOMING_AUTO)))
  {
    bool is_ipv6= false;
    unsigned int my_bind_ip= INADDR_ANY; // default if not set

    if (my_bind_addr_str && strlen(my_bind_addr_str))
    {
      my_bind_ip= wsrep_check_ip(my_bind_addr_str, &is_ipv6);
    }

    if (INADDR_ANY != my_bind_ip)
    {
      /*
        If its a not a valid address, leave inc_addr as empty string. mysqld
        is not listening for client connections on network interfaces.
      */
      if (INADDR_NONE != my_bind_ip && INADDR_LOOPBACK != my_bind_ip)
      {
        const char *fmt= (is_ipv6) ? "[%s]:%u" : "%s:%u";
        snprintf(inc_addr, inc_addr_max, fmt, my_bind_addr_str, mysqld_port);
      }
    }
    else /* mysqld binds to 0.0.0.0, try taking IP from wsrep_node_address. */
    {
      size_t const node_addr_len= strlen(node_addr);
      if (node_addr_len > 0)
      {
        wsp::Address addr(node_addr);

        if (!addr.is_valid())
        {
          WSREP_DEBUG("Could not parse node address : %s", node_addr);
          WSREP_WARN("Guessing address for incoming client connections failed. "
                     "Try setting wsrep_node_incoming_address explicitly.");
          goto done;
        }

        const char *fmt= (addr.is_ipv6()) ? "[%s]:%u" : "%s:%u";
        snprintf(inc_addr, inc_addr_max, fmt, addr.get_address(),
                 (int) mysqld_port);
      }
    }
  }
  else
  {
    wsp::Address addr(wsrep_node_incoming_address);

    if (!addr.is_valid())
    {
      WSREP_WARN("Could not parse wsrep_node_incoming_address : %s",
                 wsrep_node_incoming_address);
      goto done;
    }

    /*
      In case port is not specified in wsrep_node_incoming_address, we use
      mysqld_port.
    */
    int port= (addr.get_port() > 0) ? addr.get_port() : (int) mysqld_port;
    const char *fmt= (addr.is_ipv6()) ? "[%s]:%u" : "%s:%u";

    snprintf(inc_addr, inc_addr_max, fmt, addr.get_address(), port);
  }

done:
  struct wsrep_init_args wsrep_args;
  memset(&wsrep_args, 0, sizeof(wsrep_args));

  struct wsrep_gtid const state_id = { local_uuid, local_seqno };

  wsrep_args.app_ctx         = 0;
  wsrep_args.data_dir        = wsrep_data_home_dir;
  wsrep_args.node_name       = (wsrep_node_name) ? wsrep_node_name : "";
  wsrep_args.node_address    = node_addr;
  wsrep_args.node_incoming   = inc_addr;
  wsrep_args.options         = (wsrep_provider_options) ?
                                wsrep_provider_options : "";
  wsrep_args.proto_ver       = wsrep_max_protocol_version;

  wsrep_args.state_id        = &state_id;

  wsrep_args.logger_cb       = wsrep_log_cb;
  wsrep_args.connected_cb    = wsrep_connected_handler_cb;
  wsrep_args.view_cb         = wsrep_view_handler_cb;
  wsrep_args.sst_request_cb  = wsrep_sst_request_cb;
  wsrep_args.apply_cb        = wsrep_apply_cb;
  wsrep_args.unordered_cb    = wsrep_unordered_cb;
  wsrep_args.sst_donate_cb   = wsrep_sst_donate_cb;
  wsrep_args.synced_cb       = wsrep_synced_cb;

  rcode = wsrep->init(wsrep, &wsrep_args);

  if (rcode)
  {
    DBUG_PRINT("wsrep",("wsrep::init() failed: %d", rcode));
    WSREP_ERROR("wsrep::init() failed: %d, must shutdown", rcode);
    wsrep->free(wsrep);
    free(wsrep);
    wsrep = NULL;
    return rcode;
  }

  if (!wsrep_provider_is_SR_capable() &&
      global_system_variables.wsrep_trx_fragment_size > 0)
  {
    WSREP_ERROR("The WSREP provider (%s) does not support streaming "
                "replication but wsrep_trx_fragment_size is set to a "
                "value other than 0 (%lu). Cannot continue. Either set "
                "wsrep_trx_fragment_size to 0 or use wsrep_provider that "
                "supports streaming replication.",
                wsrep_provider, global_system_variables.wsrep_trx_fragment_size);
    wsrep->free(wsrep);
    free(wsrep);
    wsrep = NULL;
    return -1;
  }
  wsrep_inited= 1;

  wsrep_capabilities_export(wsrep->capabilities(wsrep),
                            &wsrep_provider_capabilities);

  WSREP_DEBUG("SR storage init for: %s",
              (wsrep_SR_store_type == WSREP_SR_STORE_TABLE) ? "table" :
              (wsrep_SR_store_type == WSREP_SR_STORE_FILE) ? "file" : "void");

  switch (wsrep_SR_store_type)
  {
  case WSREP_SR_STORE_FILE:
    wsrep_SR_store = wsrep_SR_store_file =
      new SR_storage_file(mysql_real_data_home_ptr,
                          1024,
                          wsrep_cluster_state_uuid);
    break;
  case WSREP_SR_STORE_TABLE:
    wsrep_SR_store = wsrep_SR_store_table = new SR_storage_table();
    break;
  case WSREP_SR_STORE_NONE: break;
  }

  return rcode;
}


/* Initialize wsrep thread LOCKs and CONDs */
void wsrep_thr_init()
{
  DBUG_ENTER("wsrep_thr_init");
  wsrep_config_state = new wsp::Config_state;
#ifdef HAVE_PSI_INTERFACE
  mysql_mutex_register("sql", wsrep_mutexes, array_elements(wsrep_mutexes));
  mysql_cond_register("sql", wsrep_conds, array_elements(wsrep_conds));
  mysql_file_register("sql", wsrep_files, array_elements(wsrep_files));
#endif

  mysql_mutex_init(key_LOCK_wsrep_ready, &LOCK_wsrep_ready, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_ready, &COND_wsrep_ready, NULL);
  mysql_mutex_init(key_LOCK_wsrep_sst, &LOCK_wsrep_sst, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_sst, &COND_wsrep_sst, NULL);
  mysql_mutex_init(key_LOCK_wsrep_sst_init, &LOCK_wsrep_sst_init, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_sst_init, &COND_wsrep_sst_init, NULL);
  mysql_mutex_init(key_LOCK_wsrep_replaying, &LOCK_wsrep_replaying, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_replaying, &COND_wsrep_replaying, NULL);
  mysql_mutex_init(key_LOCK_wsrep_slave_threads, &LOCK_wsrep_slave_threads, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_desync, &LOCK_wsrep_desync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_config_state, &LOCK_wsrep_config_state, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_SR_pool,
                   &LOCK_wsrep_SR_pool, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_SR_store,
                   &LOCK_wsrep_SR_store, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_thd_pool,
                   &LOCK_wsrep_thd_pool, MY_MUTEX_INIT_FAST);
  DBUG_VOID_RETURN;
}

extern int wsrep_on(void *);

void wsrep_init_startup (bool first)
{
  if (wsrep_init()) unireg_abort(1);

#ifdef OLD_MARIADB
  wsrep_thr_lock_init(
     (wsrep_thd_is_brute_force_fun)wsrep_thd_is_BF,
     (wsrep_abort_thd_fun)wsrep_abort_thd,
     wsrep_debug, wsrep_convert_LOCK_to_trx,
     (wsrep_on_fun)wsrep_on);
#endif
  wsrep_thr_lock_init(wsrep_thd_is_BF, wsrep_abort_thd,
                      wsrep_debug, wsrep_convert_LOCK_to_trx, wsrep_on);

  /*
    Pre-initialize global_system_variables.table_plugin with a dummy engine
    (placeholder) required during the initialization of wsrep threads (THDs).
    (see: plugin_thdvar_init())
    Note: This only needs to be done for rsync & xtrabackup based SST methods.
    In case of mysqldump SST method, the wsrep threads are created after the
    server plugins & global system variables are initialized.
  */
  if (wsrep_before_SE())
    wsrep_plugins_pre_init();

  /* Skip replication start if dummy wsrep provider is loaded */
  if (!strcmp(wsrep_provider, WSREP_NONE)) return;

  /* Skip replication start if no cluster address */
  if (!wsrep_cluster_address || wsrep_cluster_address[0] == 0) return;

  if (first) wsrep_sst_grab(); // do it so we can wait for SST below

  if (!wsrep_start_replication()) unireg_abort(1);

  wsrep_create_rollbacker();
  wsrep_create_appliers(1);

  if (first && !wsrep_sst_wait()) unireg_abort(1);// wait until SST is completed
}


void wsrep_deinit(bool free_options)
{
  DBUG_ASSERT(wsrep_inited == 1);
  delete wsrep_schema;
  wsrep_schema= 0;
  WSREP_DEBUG("wsrep_deinit, free %d", free_options);
  delete wsrep_thd_pool;
  wsrep_thd_pool= 0;

  wsrep_unload(wsrep);
  wsrep= 0;
  provider_name[0]=    '\0';
  provider_version[0]= '\0';
  provider_vendor[0]=  '\0';

  wsrep_inited= 0;

  if (wsrep_provider_capabilities != NULL)
  {
    char* p = wsrep_provider_capabilities;
    wsrep_provider_capabilities = NULL;
    free(p);
    
    if (free_options)
    {
      wsrep_sst_auth_free();
    }
  }
}

/* Destroy wsrep thread LOCKs and CONDs */
void wsrep_thr_deinit()
{
  if (!wsrep_config_state)
    return;                                     // Never initialized
  mysql_mutex_destroy(&LOCK_wsrep_ready);
  mysql_cond_destroy(&COND_wsrep_ready);
  mysql_mutex_destroy(&LOCK_wsrep_sst);
  mysql_cond_destroy(&COND_wsrep_sst);
  mysql_mutex_destroy(&LOCK_wsrep_sst_init);
  mysql_cond_destroy(&COND_wsrep_sst_init);
  mysql_mutex_destroy(&LOCK_wsrep_replaying);
  mysql_cond_destroy(&COND_wsrep_replaying);
  mysql_mutex_destroy(&LOCK_wsrep_slave_threads);
  mysql_mutex_destroy(&LOCK_wsrep_desync);
  mysql_mutex_destroy(&LOCK_wsrep_config_state);
  mysql_mutex_destroy(&LOCK_wsrep_SR_pool);
  mysql_mutex_destroy(&LOCK_wsrep_SR_store);
  mysql_mutex_destroy(&LOCK_wsrep_thd_pool);

  delete wsrep_config_state;
  wsrep_config_state= 0;                        // Safety

  if (wsrep_cluster_capabilities != NULL)
  {
    char* p = wsrep_cluster_capabilities;
    wsrep_cluster_capabilities = NULL;
    free(p);
  }
}

void wsrep_recover()
{
  wsrep_uuid_t uuid;
  wsrep_seqno_t seqno;
  wsrep_get_SE_checkpoint(uuid, seqno);
  char uuid_str[40] = {0, };;
  wsrep_uuid_print(&uuid, uuid_str, sizeof(uuid_str));

  if (wsrep_uuid_compare(&local_uuid, &WSREP_UUID_UNDEFINED) == 0 &&
      local_seqno == -2)
  {
    wsrep_uuid_print(&local_uuid, uuid_str, sizeof(uuid_str));
    WSREP_INFO("Position %s:%lld given at startup, skipping position recovery",
               uuid_str, (long long)local_seqno);
    return;
  }
  WSREP_INFO("Recovered position: %s:%lld", uuid_str, (long long)seqno);
}


void wsrep_stop_replication(THD *thd)
{
  WSREP_INFO("Stop replication");
  if (!wsrep)
  {
    WSREP_INFO("Provider was not loaded, in stop replication");
    return;
  }

  /* disconnect from group first to get wsrep_ready == FALSE */
  WSREP_DEBUG("Provider disconnect");
  wsrep->disconnect(wsrep);

  wsrep_connected= FALSE;

  /* my connection, should not terminate with wsrep_close_client_connection(),
     make transaction to rollback
   */
  if (thd && !thd->wsrep_applier) trans_rollback(thd);
  wsrep_close_client_connections(TRUE, thd);
 
  /* wait until appliers have stopped */
  wsrep_wait_appliers_close(thd);

  node_uuid= WSREP_UUID_UNDEFINED;

  delete wsrep_schema;
  wsrep_schema= 0;

  delete wsrep_thd_pool;
  wsrep_thd_pool= 0;

  return;
}

void wsrep_shutdown_replication()
{
  WSREP_INFO("Shutdown replication");
  if (!wsrep)
  {
    WSREP_INFO("Provider was not loaded, in shutdown replication");
    return;
  }

  /* disconnect from group first to get wsrep_ready == FALSE */
  WSREP_DEBUG("Provider disconnect");
  wsrep->disconnect(wsrep);
  wsrep_connected= FALSE;

  wsrep_close_client_connections(TRUE);
  wsrep_close_SR_transactions(NULL);

  /* wait until appliers have stopped */
  wsrep_wait_appliers_close(NULL);

  node_uuid= WSREP_UUID_UNDEFINED;

  if (current_thd)
  {
    /* Undocking the thread specific data. */
    my_pthread_setspecific_ptr(THR_THD, NULL);
    //my_pthread_setspecific_ptr(THR_MALLOC, NULL);
  }
}

bool wsrep_start_replication()
{
  wsrep_status_t rcode;
  WSREP_DEBUG("wsrep_start_replication");

  /* wsrep provider must be loaded. */
  DBUG_ASSERT(wsrep);

  /*
    if provider is trivial, don't even try to connect,
    but resume local node operation
  */
  if (!WSREP_PROVIDER_EXISTS)
  {
    // enable normal operation in case no provider is specified
    wsrep_ready_set(TRUE);
    return true;
  }

  if (!wsrep_cluster_address || wsrep_cluster_address[0]== 0)
  {
    // if provider is non-trivial, but no address is specified, wait for address
    wsrep_ready_set(FALSE);
    return true;
  }

  /*
    With mysqldump etc SST THD pool must be initialized before starting
    replication in order to avoid deadlock between THD pool initialization
    and possible causal read of status variables.

    On the other hand, with SST methods that require starting wsrep first
    plugins are not necessarily initialized at this point, so THD pool
    initialization must be postponed until plugin init has been done and
    before wsrep schema is initialized.
   */
  if (!wsrep_before_SE()) {
    DBUG_ASSERT(!wsrep_thd_pool);
    wsrep_thd_pool= new Wsrep_thd_pool(WSREP_THD_POOL_SIZE);
  }
  wsrep_init_SR_pool();
  wsrep_startup= TRUE;

  bool const bootstrap(TRUE == wsrep_new_cluster);
  wsrep_new_cluster= FALSE;

  WSREP_INFO("Start replication");

  if (wsrep_new_cluster)
  {
    WSREP_INFO("'wsrep-new-cluster' option used, bootstrapping the cluster");
    wsrep_new_cluster= false;
  }

  if ((rcode = wsrep->connect(wsrep,
                              wsrep_cluster_name,
                              wsrep_cluster_address,
                              wsrep_sst_donor,
                              bootstrap)))
  {
    DBUG_PRINT("wsrep",("wsrep->connect(%s) failed: %d",
                        wsrep_cluster_address, rcode));
    WSREP_ERROR("wsrep::connect(%s) failed: %d",
                wsrep_cluster_address, rcode);
    return false;
  }
  else
  {
    wsrep_connected= TRUE;

    char* opts= wsrep->options_get(wsrep);
    if (opts)
    {
      wsrep_provider_options_init(opts);
      free(opts);
    }
    else
    {
      WSREP_WARN("Failed to get wsrep options");
    }
  }

  return true;
}

bool wsrep_must_sync_wait (THD* thd, uint mask)
{
  bool ret;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  ret= (thd->variables.wsrep_sync_wait & mask) &&
    thd->variables.wsrep_on &&
    !thd->in_active_multi_stmt_transaction() &&
    thd->wsrep_conflict_state() != REPLAYING &&
    thd->wsrep_sync_wait_gtid.seqno == WSREP_SEQNO_UNDEFINED;
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  return ret;
}

bool wsrep_sync_wait (THD* thd, uint mask)
{
  if (wsrep_must_sync_wait(thd, mask))
  {
    WSREP_DEBUG("wsrep_sync_wait: thd->variables.wsrep_sync_wait = %u, mask = %u",
                thd->variables.wsrep_sync_wait, mask);
    // This allows autocommit SELECTs and a first SELECT after SET AUTOCOMMIT=0
    // TODO: modify to check if thd has locked any rows.
    wsrep_status_t ret = wsrep_sync_wait_upto(thd, NULL, -1);

    if (unlikely(WSREP_OK != ret))
    {
      const char* msg;
      int err;

      // Possibly relevant error codes:
      // ER_CHECKREAD, ER_ERROR_ON_READ, ER_INVALID_DEFAULT, ER_EMPTY_QUERY,
      // ER_FUNCTION_NOT_DEFINED, ER_NOT_ALLOWED_COMMAND, ER_NOT_SUPPORTED_YET,
      // ER_FEATURE_DISABLED, ER_QUERY_INTERRUPTED

      switch (ret)
      {
      case WSREP_NOT_IMPLEMENTED:
        msg= "synchronous reads by wsrep backend. "
             "Please unset wsrep_causal_reads variable.";
        err= ER_NOT_SUPPORTED_YET;
        break;
      default:
        msg= "Synchronous wait failed.";
        err= ER_LOCK_WAIT_TIMEOUT; // NOTE: the above msg won't be displayed
                                   //       with ER_LOCK_WAIT_TIMEOUT
      }

      my_error(err, MYF(0), msg);

      return true;
    }
  }

  return false;
}

wsrep_status_t wsrep_sync_wait_upto (THD*          thd,
                                     wsrep_gtid_t* upto,
                                     int           timeout)
{
  return wsrep->sync_wait(wsrep,
                          upto,
                          timeout,
                          &thd->wsrep_sync_wait_gtid);
}

void wsrep_keys_free(wsrep_key_arr_t* key_arr)
{
    for (size_t i= 0; i < key_arr->keys_len; ++i)
    {
        my_free((void*)key_arr->keys[i].key_parts);
    }
    my_free(key_arr->keys);
    key_arr->keys= 0;
    key_arr->keys_len= 0;
}


/*!
 * @param db      Database string
 * @param table   Table string
 * @param key     Array of wsrep_key_t
 * @param key_len In: number of elements in key array, Out: number of
 *                elements populated
 *
 * @return true if preparation was successful, otherwise false.
 */

static bool wsrep_prepare_key_for_isolation(const char* db,
                                           const char* table,
                                           wsrep_buf_t* key,
                                           size_t* key_len)
{
    if (*key_len < 2) return false;

    switch (wsrep_protocol_version)
    {
    case 0:
        *key_len= 0;
        break;
    case 1:
    case 2:
    case 3:
    case 4:
    {
        *key_len= 0;
        if (db)
        {
            // sql_print_information("%s.%s", db, table);
            if (db)
            {
                key[*key_len].ptr= db;
                key[*key_len].len= strlen(db);
                ++(*key_len);
                if (table)
                {
                    key[*key_len].ptr= table;
                    key[*key_len].len= strlen(table);
                    ++(*key_len);
                }
            }
        }
        break;
    }
    default:
        assert(0);
        WSREP_ERROR("Unsupported protocol version: %ld", wsrep_protocol_version);
        unireg_abort(1);
        return false;
    }

    return true;
}

/*
 * Prepare key list from db/table and table_list
 *
 * Return zero in case of success, 1 in case of failure.
 */
bool wsrep_prepare_keys_for_isolation(THD*              thd,
                                      const char*       db,
                                      const char*       table,
                                      const TABLE_LIST* table_list,
                                      wsrep_key_arr_t*  ka)
{
  ka->keys= 0;
  ka->keys_len= 0;

  if (db || table)
  {
    if (!(ka->keys= (wsrep_key_t*)my_malloc(sizeof(wsrep_key_t), MYF(0))))
    {
      WSREP_ERROR("Can't allocate memory for key_array");
      goto err;
    }
    ka->keys_len= 1;
    if (!(ka->keys[0].key_parts= (wsrep_buf_t*)
          my_malloc(sizeof(wsrep_buf_t)*2, MYF(0))))
    {
      WSREP_ERROR("Can't allocate memory for key_parts");
      goto err;
    }
    ka->keys[0].key_parts_num= 2;
    if (!wsrep_prepare_key_for_isolation(
                                         db, table,
                                         (wsrep_buf_t*)ka->keys[0].key_parts,
                                         &ka->keys[0].key_parts_num))
    {
      WSREP_ERROR("Preparing keys for isolation failed (1)");
      goto err;
    }
  }

  for (const TABLE_LIST* table= table_list; table; table= table->next_global)
  {
    wsrep_key_t* tmp;
    tmp= (wsrep_key_t*)my_realloc(ka->keys,
                                  (ka->keys_len + 1) * sizeof(wsrep_key_t),
                                  MY_ALLOW_ZERO_PTR);
    if (!tmp)
    {
      WSREP_ERROR("Can't allocate memory for key_array");
      goto err;
    }
    ka->keys= tmp;
    if (!(ka->keys[ka->keys_len].key_parts= (wsrep_buf_t*)
          my_malloc(sizeof(wsrep_buf_t)*2, MYF(0))))
    {
      WSREP_ERROR("Can't allocate memory for key_parts");
      goto err;
    }
    ka->keys[ka->keys_len].key_parts_num= 2;
    ++ka->keys_len;
    if (!wsrep_prepare_key_for_isolation(table->db.str, table->table_name.str,
                                         (wsrep_buf_t*)ka->keys[ka->keys_len - 1].key_parts,
                                         &ka->keys[ka->keys_len - 1].key_parts_num))
    {
      WSREP_ERROR("Preparing keys for isolation failed (2)");
      goto err;
    }
  }
  return false;
err:
    wsrep_keys_free(ka);
    return true;
}

#ifdef OUT
bool wsrep_prepare_key(const uchar* cache_key, size_t cache_key_len,
                       const uchar* row_id, size_t row_id_len,
                       wsrep_buf_t* key, size_t* key_len)
{
    if (*key_len < 3) return false;

    *key_len= 0;
    switch (wsrep_protocol_version)
    {
    case 0:
    {
        key[0].ptr = cache_key;
        key[0].len = cache_key_len;

        *key_len = 1;
        break;
    }
    case 1:
    case 2:
    case 3:
    case 4:
    {
        key[0].ptr = cache_key;
        key[0].len = strlen( (char*)cache_key );

        key[1].ptr = cache_key + strlen( (char*)cache_key ) + 1;
        key[1].len = strlen( (char*)(key[1].ptr) );

        *key_len = 2;
        break;
    }
    default:
        return false;
    }

    key[*key_len].ptr = row_id;
    key[*key_len].len = row_id_len;
    ++(*key_len);

    return true;
}
#endif

bool wsrep_prepare_key_for_innodb(THD* thd,
                                  const uchar* cache_key,
                                  size_t cache_key_len,
                                  const uchar* row_id,
                                  size_t row_id_len,
                                  wsrep_buf_t* key,
                                  size_t* key_len)
{

  if (*key_len < 3) return false;

    *key_len= 0;
    switch (wsrep_protocol_version)
    {
    case 0:
    {
        key[0].ptr = cache_key;
        key[0].len = cache_key_len;

        *key_len = 1;
        break;
    }
    case 1:
    case 2:
    case 3:
    case 4:
    {
        key[0].ptr = cache_key;
        key[0].len = strlen( (char*)cache_key );

        key[1].ptr = cache_key + strlen( (char*)cache_key ) + 1;
        key[1].len = strlen( (char*)(key[1].ptr) );

        *key_len = 2;
        break;
    }
    default:
        assert(0);
        WSREP_ERROR("Unsupported protocol version: %ld", wsrep_protocol_version);
        unireg_abort(1);
        return false;
    }

    key[*key_len].ptr = row_id;
    key[*key_len].len = row_id_len;
    ++(*key_len);

    return true;
}

bool wsrep_append_SR_keys(THD* thd)
{
  wsrep_key_set::iterator db_it;
  for (db_it = thd->wsrep_SR_keys.begin();
       db_it != thd->wsrep_SR_keys.end();
       ++db_it)
  {
    const std::string& db(db_it->first);
    wsrep_key_set::mapped_type& table_names(db_it->second);
    wsrep_key_set::mapped_type::iterator table_it;
    for (table_it = table_names.begin();
         table_it != table_names.end();
         ++table_it)
    {
      size_t parts_len(2);
      wsrep_buf_t parts[2];
      if (!wsrep_prepare_key_for_isolation(db.c_str(),
                                           (*table_it).c_str(),
                                           parts,
                                           &parts_len))
      {
        WSREP_ERROR("Failed to prepare key for streaming transaction, %s",
                    thd->query());
        return false;
      }

      wsrep_key_t key = {parts, parts_len};
      if (wsrep->append_key(wsrep,
                            &thd->wsrep_ws_handle,
                            &key,
                            1,
                            WSREP_KEY_SHARED,
                            true))
      {
        WSREP_ERROR("Failed to append key for streaming transaction, %s",
                    thd->query());
        return false;
      }
    }
  }

  return true;
}

/*
 * Construct Query_log_Event from thd query and serialize it
 * into buffer.
 *
 * Return 0 in case of success, 1 in case of error.
 */
int wsrep_to_buf_helper(
    THD* thd, const char *query, uint query_len, uchar** buf, size_t* buf_len)
{
  IO_CACHE tmp_io_cache;
  Log_event_writer writer(&tmp_io_cache, 0);
  if (open_cached_file(&tmp_io_cache, mysql_tmpdir, TEMP_PREFIX,
                       65536, MYF(MY_WME)))
    return 1;
  int ret(0);
  enum enum_binlog_checksum_alg current_binlog_check_alg=
    (enum_binlog_checksum_alg) binlog_checksum_options;

  Format_description_log_event *tmp_fd= new Format_description_log_event(4);
  tmp_fd->checksum_alg= current_binlog_check_alg;
  writer.write(tmp_fd);
  delete tmp_fd;

#ifdef GTID_SUPPORT
  if (thd->variables.gtid_next.type == GTID_GROUP)
  {
      Gtid_log_event gtid_ev(thd, FALSE, &thd->variables.gtid_next);
      if (!gtid_ev.is_valid()) ret= 0;
      if (!ret && writer.write(&gtid_ev)) ret= 1;
  }
#endif /* GTID_SUPPORT */
  if (wsrep_gtid_mode && thd->variables.gtid_seq_no)
  {
    Gtid_log_event gtid_event(thd, thd->variables.gtid_seq_no,
                          thd->variables.gtid_domain_id,
                          true, LOG_EVENT_SUPPRESS_USE_F,
                          true, 0);
    gtid_event.server_id= thd->variables.server_id;
    if (!gtid_event.is_valid()) ret= 0;
    ret= writer.write(&gtid_event);
  }

  /* if there is prepare query, add event for it */
  if (!ret && thd->wsrep_TOI_pre_query)
  {
    Query_log_event ev(thd, thd->wsrep_TOI_pre_query,
		       thd->wsrep_TOI_pre_query_len,
		       FALSE, FALSE, FALSE, 0);
    ev.checksum_alg= current_binlog_check_alg;
    if (writer.write(&ev)) ret= 1;
  }

  /* continue to append the actual query */
  Query_log_event ev(thd, query, query_len, FALSE, FALSE, FALSE, 0);
  ev.checksum_alg= current_binlog_check_alg;
  if (!ret && writer.write(&ev)) ret= 1;
  if (!ret && wsrep_write_cache_buf(&tmp_io_cache, buf, buf_len)) ret= 1;
  close_cached_file(&tmp_io_cache);
  return ret;
}

static int
wsrep_alter_query_string(THD *thd, String *buf)
{
  /* Append the "ALTER" part of the query */
  if (buf->append(STRING_WITH_LEN("ALTER ")))
    return 1;
  /* Append definer */
  append_definer(thd, buf, &(thd->lex->definer->user), &(thd->lex->definer->host));
  /* Append the left part of thd->query after event name part */
  if (buf->append(thd->lex->stmt_definition_begin,
                  thd->lex->stmt_definition_end -
                  thd->lex->stmt_definition_begin))
    return 1;

  return 0;
}

static int wsrep_alter_event_query(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;

  if (wsrep_alter_query_string(thd, &log_query))
  {
    WSREP_WARN("events alter string failed: schema: %s, query: %s",
               thd->get_db(), thd->query());
    return 1;
  }
  return wsrep_to_buf_helper(thd, log_query.ptr(), log_query.length(), buf, buf_len);
}

#include "sql_show.h"
static int
create_view_query(THD *thd, uchar** buf, size_t* buf_len)
{
    LEX *lex= thd->lex;
    SELECT_LEX *select_lex= &lex->select_lex;
    TABLE_LIST *first_table= select_lex->table_list.first;
    TABLE_LIST *views = first_table;
    LEX_USER *definer;
    String buff;
    const LEX_CSTRING command[3]=
      {{ STRING_WITH_LEN("CREATE ") },
       { STRING_WITH_LEN("ALTER ") },
       { STRING_WITH_LEN("CREATE OR REPLACE ") }};

    buff.append(&command[thd->lex->create_view->mode]);

    if (lex->definer)
      definer= get_current_user(thd, lex->definer);
    else
    {
      /*
        DEFINER-clause is missing; we have to create default definer in
        persistent arena to be PS/SP friendly.
        If this is an ALTER VIEW then the current user should be set as
        the definer.
      */
      definer= create_default_definer(thd, false);
    }

    if (definer)
    {
      views->definer.user = definer->user;
      views->definer.host = definer->host;
    } else {
      WSREP_ERROR("Failed to get DEFINER for VIEW.");
      return 1;
    }

    views->algorithm    = lex->create_view->algorithm;
    views->view_suid    = lex->create_view->suid;
    views->with_check   = lex->create_view->check;

    view_store_options(thd, views, &buff);
    buff.append(STRING_WITH_LEN("VIEW "));
    /* Test if user supplied a db (ie: we did not use thd->db) */
    if (views->db.str && views->db.str[0] &&
        (thd->db.str == NULL || cmp(&views->db, &thd->db)))
    {
      append_identifier(thd, &buff, &views->db);
      buff.append('.');
    }
    append_identifier(thd, &buff, &views->table_name);
    if (lex->view_list.elements)
    {
      List_iterator_fast<LEX_CSTRING> names(lex->view_list);
      LEX_CSTRING *name;
      int i;

      for (i= 0; (name= names++); i++)
      {
        buff.append(i ? ", " : "(");
        append_identifier(thd, &buff, name);
      }
      buff.append(')');
    }
    buff.append(STRING_WITH_LEN(" AS "));
    //buff.append(views->source.str, views->source.length);
    buff.append(thd->lex->create_view->select.str,
                thd->lex->create_view->select.length);
    //int errcode= query_error_code(thd, TRUE);
    //if (thd->binlog_query(THD::STMT_QUERY_TYPE,
    //                      buff.ptr(), buff.length(), FALSE, FALSE, FALSE, errcod
    return wsrep_to_buf_helper(thd, buff.ptr(), buff.length(), buf, buf_len);
}

/* Forward declarations. */
int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len);
int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len);
/*
  Rewrite DROP TABLE for TOI. Temporary tables are eliminated from
  the query as they are visible only to client connection.

  TODO: See comments for sql_base.cc:drop_temporary_table() and refine
  the function to deal with transactional locked tables.
 */
static int wsrep_drop_table_query(THD* thd, uchar** buf, size_t* buf_len)
{

  LEX* lex= thd->lex;
  SELECT_LEX* select_lex= &lex->select_lex;
  TABLE_LIST* first_table= select_lex->table_list.first;
  String buff;

  bool found_temp_table= false;
  for (TABLE_LIST* table= first_table; table; table= table->next_global)
  {
    if (thd->find_temporary_table(table->db.str, table->table_name.str))
    {
      found_temp_table= true;
      break;
    }
  }

  if (found_temp_table)
  {
    buff.append("DROP TABLE ");
    if (lex->create_info.if_exists())
      buff.append("IF EXISTS ");

    for (TABLE_LIST* table= first_table; table; table= table->next_global)
    {
      if (!thd->find_temporary_table(table->db.str, table->table_name.str))
      {
        append_identifier(thd, &buff, table->db.str, table->db.length);
        buff.append(".");
        append_identifier(thd, &buff, table->table_name.str, table->table_name.length);
        buff.append(",");
      }
    }

    /* Chop the last comma */
    buff.chop();
    buff.append(" /* generated by wsrep */");

    WSREP_DEBUG("Rewrote '%s' as '%s'", thd->query(), buff.ptr());

    return wsrep_to_buf_helper(thd, buff.ptr(), buff.length(), buf, buf_len);
  }
  else
  {
    return wsrep_to_buf_helper(thd, thd->query(), thd->query_length(),
                               buf, buf_len);
  }
}

static int wsrep_TOI_event_buf(THD* thd, uchar** buf, size_t* buf_len)
{
  int err;
  switch (thd->lex->sql_command)
  {
  case SQLCOM_CREATE_VIEW:
    err= create_view_query(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_CREATE_SPFUNCTION:
    err= wsrep_create_sp(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_TRIGGER:
    err= wsrep_create_trigger_query(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_EVENT:
    err= wsrep_create_event_query(thd, buf, buf_len);
    break;
  case SQLCOM_ALTER_EVENT:
    err= wsrep_alter_event_query(thd, buf, buf_len);
    break;
  case SQLCOM_DROP_TABLE:
    err= wsrep_drop_table_query(thd, buf, buf_len);
    break;
  default:
    err= wsrep_to_buf_helper(thd, thd->query(), thd->query_length(), buf,
                             buf_len);
    break;
  }

  return err;
}

/*
  Decide if statement should run in TOI.

  Look if table or table_list contain temporary tables. If the
  statement affects only temporary tables,   statement should not run
  in TOI. If the table list contains mix of regular and temporary tables
  (DROP TABLE, OPTIMIZE, ANALYZE), statement should be run in TOI but
  should be rewritten at later time for replication to contain only
  non-temporary tables.
 */
static bool wsrep_can_run_in_toi(THD *thd, const char *db, const char *table,
                                 const TABLE_LIST *table_list)
{
  DBUG_ASSERT(!table || db);
  DBUG_ASSERT(table_list || db);

  LEX* lex= thd->lex;
  SELECT_LEX* select_lex= &lex->select_lex;
  TABLE_LIST* first_table= select_lex->table_list.first;

  switch (lex->sql_command)
  {
  case SQLCOM_CREATE_TABLE:
    DBUG_ASSERT(!table_list);
    if (thd->lex->create_info.options & HA_LEX_CREATE_TMP_TABLE)
    {
      return false;
    }
    return true;

  case SQLCOM_CREATE_VIEW:

    DBUG_ASSERT(!table_list);
    DBUG_ASSERT(first_table); /* First table is view name */
    /*
      If any of the remaining tables refer to temporary table error
      is returned to client, so TOI can be skipped
    */
    for (TABLE_LIST* it= first_table->next_global; it; it= it->next_global)
    {
      if (thd->find_temporary_table(it))
      {
        return false;
      }
    }
    return true;

  case SQLCOM_CREATE_TRIGGER:

    DBUG_ASSERT(!table_list);
    DBUG_ASSERT(first_table);

    if (thd->find_temporary_table(first_table))
    {
      return false;
    }
    return true;

  default:
    if (table && !thd->find_temporary_table(db, table))
    {
      return true;
    }

    if (table_list)
    {
      for (TABLE_LIST* table= first_table; table; table= table->next_global)
      {
        if (!thd->find_temporary_table(table->db.str, table->table_name.str))
        {
          return true;
        }
      }
    }
    return !(table || table_list);
  }
}

static bool wsrep_can_run_in_nbo(THD *thd)
{
    switch (thd->lex->sql_command)
    {
    case SQLCOM_ALTER_TABLE:
      /*
        CREATE INDEX and DROP INDEX are mapped to ALTER TABLE internally
      */
    case SQLCOM_CREATE_INDEX:
    case SQLCOM_DROP_INDEX:
      switch (thd->lex->alter_info.requested_lock)
      {
      case Alter_info::ALTER_TABLE_LOCK_SHARED:
      case Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE:
        return true;
      default:
        return false;
      }
    case SQLCOM_OPTIMIZE:
        return true;
    default:
        break; /* Keep compiler happy */
    }
    return false;
}

static void wsrep_TOI_begin_failed(THD* thd, const wsrep_buf_t* const err)
{
  if (wsrep_thd_trx_seqno(thd) > 0)
  {
    /* GTID was granted and TO acquired - need to log event and release TO */
    if (wsrep_emulate_bin_log) wsrep_thd_binlog_trx_reset(thd);
    if (wsrep_write_dummy_event(thd, "TOI begin failed")) { goto fail; }
    wsrep_xid_init(&thd->wsrep_xid,
                   thd->wsrep_trx_meta.gtid.uuid,
                   thd->wsrep_trx_meta.gtid.seqno);
    //if (tc_log) tc_log->commit(thd, true);
    if (tc_log)
    {
      tc_log->log_and_order(thd, thd->transaction.xid_state.xid.get_my_xid(),
                            true, false, false);
    }
    wsrep_status_t const rcode=
      wsrep->to_execute_end(wsrep, thd->thread_id, err);
    if (WSREP_OK != rcode)
    {
      WSREP_ERROR("Leaving critical section for failed TOI failed: thd: %llu, "
                  "schema: %s, SQL: %s, rcode: %d",
                  (long long)thd->real_id, (thd->db.str ? thd->db.str : "(null)"),
                  thd->query(), rcode);
      goto fail;
    }
  }
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  wsrep_cleanup_transaction(thd);
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  return;
fail:
  WSREP_ERROR("Failed to release TOI resources. Need to abort.");
  unireg_abort(1);
}

/*
  returns:
   0: statement was replicated as TOI
   1: TOI replication was skipped
  -1: TOI replication failed
  -2: NBO begin failed
 */
static int wsrep_TOI_begin(THD *thd, const char *db_, const char *table_,
                           const TABLE_LIST* table_list)
{
  DBUG_ASSERT(thd->variables.wsrep_OSU_method == WSREP_OSU_TOI ||
              thd->variables.wsrep_OSU_method == WSREP_OSU_NBO);

  if (wsrep_can_run_in_toi(thd, db_, table_, table_list) == false)
  {
    WSREP_DEBUG("No TOI for %s", WSREP_QUERY(thd));
    return 1;
  }

  if (thd->variables.wsrep_OSU_method == WSREP_OSU_NBO &&
      (wsrep->capabilities(wsrep) & WSREP_CAP_NBO) == 0)
  {
    const char* const msg=
      "wsrep_OSU_method NBO is not supported by wsrep provider";
    WSREP_DEBUG("%s", msg);
    my_message(ER_NOT_SUPPORTED_YET, msg, MYF(0));
    return -1;
  }

  bool can_run_in_nbo(wsrep_can_run_in_nbo(thd));
  if (can_run_in_nbo                  == false &&
      thd->variables.wsrep_OSU_method == WSREP_OSU_NBO)
  {
    WSREP_DEBUG("wsrep_OSU_method NBO not supported for %s",
                WSREP_QUERY(thd));
    my_message(ER_NOT_SUPPORTED_YET,
               "wsrep_OSU_method NBO not supported for query",
               MYF(0));
    return -1;
  }

  bool run_in_nbo= (thd->variables.wsrep_OSU_method == WSREP_OSU_NBO &&
                    can_run_in_nbo);

  uint32_t flags= (run_in_nbo ? WSREP_FLAG_TRX_START :
                   WSREP_FLAG_TRX_START | WSREP_FLAG_TRX_END);
  wsrep_status_t ret;
  uchar* buf= 0;
  size_t buf_len(0);
  int buf_err;
  int rc;
  time_t wait_start;

  buf_err= wsrep_TOI_event_buf(thd, &buf, &buf_len);
  if (buf_err) {
    WSREP_ERROR("Failed to create TOI event buf: %d", buf_err);
    my_message(ER_UNKNOWN_ERROR,
               "WSREP replication failed to prepare TOI event buffer. "
               "Check your query.",
               MYF(0));
    return -1;
  }
  struct wsrep_buf buff= { buf, buf_len };

  wsrep_key_arr_t key_arr= {0, 0};
  if (wsrep_prepare_keys_for_isolation(thd, db_, table_, table_list, &key_arr)) {
    WSREP_ERROR("Failed to prepare keys for isolation");
    my_message(ER_UNKNOWN_ERROR,
               "WSREP replication failed to prepare keys. Check your query.",
               MYF(0));
    rc= -1;
    goto out;
  }

  /* wsrep_can_run_in_toi() should take care of checking that
     DDLs with only temp tables should not be TOId at all */
  DBUG_ASSERT(key_arr.keys_len > 0);
  if (key_arr.keys_len == 0)
  {
    /* non replicated DDL, affecting temporary tables only */
    WSREP_DEBUG("TO isolation skipped, sql: %s."
                "Only temporary tables affected.", WSREP_QUERY(thd));
    rc= 1;
    goto out;
  }

  thd_proc_info(thd, "acquiring total order isolation");
  wait_start= time(NULL);
  do
  {
    ret= wsrep->to_execute_start(wsrep,
                                 thd->thread_id,
                                 key_arr.keys,
                                 key_arr.keys_len,
                                 &buff,
                                 1,
                                 flags,
                                 &thd->wsrep_trx_meta);

    if (thd->killed != NOT_KILLED) break;

    if (ret == WSREP_TRX_FAIL)
    {
      WSREP_DEBUG("to_execute_start() failed for %lld: %s, NBO: %s, seqno: %lld",
                  thd->thread_id, WSREP_QUERY(thd), run_in_nbo ? "yes" : "no",
                  (long long)wsrep_thd_trx_seqno(thd));
      if (ulong(time(NULL) - wait_start) < thd->variables.lock_wait_timeout)
      {
        usleep(100000);
      }
      else
      {
        my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
        break;
      }
      if (run_in_nbo) /* will loop */
      {
         wsrep_TOI_begin_failed(thd, NULL /* failed repl/certification doesn't mean error in execution */);
      }
    }
  } while (ret == WSREP_TRX_FAIL && run_in_nbo);

  if (ret != WSREP_OK) {
    /* jump to error handler in mysql_execute_command() */
    switch (ret)
    {
    case WSREP_SIZE_EXCEEDED:
      WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. "
                 "Maximum size exceeded.",
                 ret,
                 (thd->db.str ? thd->db.str : "(null)"),
                 WSREP_QUERY(thd));
      my_error(ER_ERROR_DURING_COMMIT, MYF(0), WSREP_SIZE_EXCEEDED);
      break;
    default:
      WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. "
                 "Check wsrep connection state and retry the query.",
                 ret,
                 (thd->db.str ? thd->db.str : "(null)"),
                 WSREP_QUERY(thd));
      if (!thd->is_error())
      {
        my_error(ER_LOCK_DEADLOCK, MYF(0), "WSREP replication failed. Check "
                 "your wsrep connection state and retry the query.");
      }
    }
    rc= -1;
  }
  else {

    try {
      /*
        Allocate dummy thd->wsrep_nbo_ctx to track execution state
        in mysql_execute_command().
      */
      if (run_in_nbo) {
        thd->wsrep_nbo_ctx= new Wsrep_nbo_ctx(0, 0, 0, wsrep_trx_meta_t());
      }
      thd->wsrep_exec_mode= TOTAL_ORDER;
      ++wsrep_to_isolation;
      WSREP_DEBUG("TO BEGIN(%lld): %lld, %d, %s",
                  thd->thread_id,
                  (long long)wsrep_thd_trx_seqno(thd),
                  thd->wsrep_exec_mode, WSREP_QUERY(thd));
      rc= 0;

    }
    catch (std::bad_alloc& e) {
      rc= -2;
    }
  }

 out:
  if (buf) my_free(buf);
  if (key_arr.keys_len) wsrep_keys_free(&key_arr);

  switch(rc)
  {
  case 0:
    break;
  case -2:
  {
    const char* const err_str= "Failed to allocate NBO context object.";
    wsrep_buf_t const err= { err_str, strlen(err_str) };
    wsrep_TOI_begin_failed(thd, &err);
    break;
  }
  default:
    wsrep_TOI_begin_failed(thd, NULL);
  }

  return rc;
}

static void wsrep_TOI_end(THD *thd) {
  wsrep_status_t ret;
  wsrep_to_isolation--;

  WSREP_DEBUG("TO END: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_exec_mode, WSREP_QUERY(thd));

  if (wsrep_thd_trx_seqno(thd) != WSREP_SEQNO_UNDEFINED)
  {
    wsrep_set_SE_checkpoint(thd->wsrep_trx_meta.gtid.uuid,
                            thd->wsrep_trx_meta.gtid.seqno);
    WSREP_DEBUG("TO END: %lld, update seqno",
                (long long)wsrep_thd_trx_seqno(thd));

    if (thd->is_error() && !wsrep_must_ignore_error(thd))
    {
      wsrep_apply_error err;
      err.store(thd);

      wsrep_buf_t const tmp= err.get_buf();
      ret= wsrep->to_execute_end(wsrep, thd->thread_id, &tmp);
    }
    else
    {
      ret= wsrep->to_execute_end(wsrep, thd->thread_id, NULL);
    }

    if (WSREP_OK == ret)
    {
      WSREP_DEBUG("TO END: %lld", (long long)wsrep_thd_trx_seqno(thd));
    }
    else
    {
      WSREP_WARN("TO isolation end failed for: %d, schema: %s, sql: %s",
                 ret, (thd->db.str ? thd->db.str : "(null)"), WSREP_QUERY(thd));
    }
  }

  if (thd->wsrep_nbo_ctx)
  {
    delete thd->wsrep_nbo_ctx;
    thd->wsrep_nbo_ctx= NULL;
  }
}

static int wsrep_RSU_begin(THD *thd, const char *db_, const char *table_)
{
  wsrep_status_t ret(WSREP_WARNING);
  WSREP_DEBUG("RSU BEGIN: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_exec_mode, WSREP_QUERY(thd));

  ret = wsrep->desync(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("RSU desync failed %d for schema: %s, query: %s",
               ret, thd->get_db(), thd->query());
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return(ret);
  }

  mysql_mutex_lock(&LOCK_wsrep_replaying);
  wsrep_replaying++;
  mysql_mutex_unlock(&LOCK_wsrep_replaying);

  if (wsrep_wait_committing_connections_close(5000))
  {
    /* no can do, bail out from DDL */
    WSREP_WARN("RSU failed due to pending transactions, schema: %s, query %s",
               thd->get_db(), thd->query());
    mysql_mutex_lock(&LOCK_wsrep_replaying);
    wsrep_replaying--;
    mysql_mutex_unlock(&LOCK_wsrep_replaying);

    ret = wsrep->resync(wsrep);
    if (ret != WSREP_OK)
    {
      WSREP_WARN("resync failed %d for schema: %s, query: %s",
                 ret, thd->get_db(), thd->query());
    }

    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return(1);
  }

  wsrep_seqno_t seqno = wsrep->pause(wsrep);
  if (seqno == WSREP_SEQNO_UNDEFINED)
  {
    WSREP_WARN("pause failed %lld for schema: %s, query: %s", (long long)seqno,
               thd->get_db(), thd->query());
    return(1);
  }
  WSREP_DEBUG("paused at %lld", (long long)seqno);
  thd->variables.wsrep_on = 0;
  return 0;
}

static void wsrep_RSU_end(THD *thd)
{
  wsrep_status_t ret(WSREP_WARNING);
  WSREP_DEBUG("RSU END: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
               thd->wsrep_exec_mode, thd->query() );


  mysql_mutex_lock(&LOCK_wsrep_replaying);
  wsrep_replaying--;
  mysql_mutex_unlock(&LOCK_wsrep_replaying);

  ret = wsrep->resume(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("resume failed %d for schema: %s, query: %s", ret,
               thd->get_db(), thd->query());
  }

  ret = wsrep->resync(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("resync failed %d for schema: %s, query: %s", ret,
               thd->get_db(), thd->query());
    return;
  }

  thd->variables.wsrep_on = 1;
}

int wsrep_to_isolation_begin(THD *thd, const char *db_, const char *table_,
                             const TABLE_LIST* table_list)
{
  /*
    No isolation for applier or replaying threads.
   */
  if (thd->wsrep_exec_mode == REPL_RECV) return 0;

  int ret= 0;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);

  if (thd->wsrep_conflict_state() == MUST_ABORT)
  {
    WSREP_INFO("thread: %lld  schema: %s  query: %s has been aborted due to multi-master conflict",
               (longlong) thd->thread_id, thd->get_db(), thd->query());
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    return WSREP_TRX_FAIL;
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE);
  DBUG_ASSERT(thd->wsrep_trx_meta.gtid.seqno == WSREP_SEQNO_UNDEFINED);

  if (thd->global_read_lock.can_acquire_protection())
  {
    WSREP_DEBUG("Aborting TOI: Global Read-Lock (FTWRL) in place: %s %lld",
                WSREP_QUERY(thd), thd->thread_id);
    return -1;
  }

  if (wsrep_debug && thd->mdl_context.has_locks())
  {
    WSREP_DEBUG("thread holds MDL locks at TI begin: %s %lld",
                WSREP_QUERY(thd), thd->thread_id);
  }

  /*
    It makes sense to set auto_increment_* to defaults in TOI operations.
    Must be done before wsrep_TOI_begin() since Query_log_event encapsulating
    TOI statement and auto inc variables for wsrep replication is constructed
    there. Variables are reset back in THD::reset_for_next_command() before
    processing of next command.
   */
  if (wsrep_auto_increment_control)
  {
    thd->variables.auto_increment_offset = 1;
    thd->variables.auto_increment_increment = 1;
  }

  if (thd->variables.wsrep_on && thd->wsrep_exec_mode==LOCAL_STATE)
  {
    switch (thd->variables.wsrep_OSU_method) {
    case WSREP_OSU_TOI:
    case WSREP_OSU_NBO:
      ret=  wsrep_TOI_begin(thd, db_, table_, table_list);
      break;
    case WSREP_OSU_RSU:
      ret=  wsrep_RSU_begin(thd, db_, table_);
      break;
    default:
      WSREP_ERROR("Unsupported OSU method: %lu",
                  thd->variables.wsrep_OSU_method);
      ret= -1;
      break;
    }
    switch (ret) {
    case 0:  thd->wsrep_exec_mode= TOTAL_ORDER; break;
    case 1:
      /* TOI replication skipped, treat as success */
      ret = 0;
      break;
    case -1:
      /* TOI replication failed, treat as error */
      break;
    }
  }
  return ret;
}

void wsrep_to_isolation_end(THD *thd)
{
  if (thd->wsrep_exec_mode == TOTAL_ORDER)
  {
    switch(thd->variables.wsrep_OSU_method)
    {
    case WSREP_OSU_TOI:
    case WSREP_OSU_NBO:
      wsrep_TOI_end(thd);
      break;
    case WSREP_OSU_RSU:
      wsrep_RSU_end(thd);
      break;
    default:
      WSREP_WARN("Unsupported wsrep OSU method at isolation end: %lu",
                 thd->variables.wsrep_OSU_method);
      break;
    }
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    wsrep_cleanup_transaction(thd);
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
}

void wsrep_begin_nbo_unlock(THD* thd)
{
  DBUG_ASSERT(thd->wsrep_nbo_ctx);
  if (thd->wsrep_exec_mode == TOTAL_ORDER)
  {
    if (wsrep->to_execute_end(wsrep, thd->thread_id, NULL) != WSREP_OK) {
      WSREP_ERROR("Non-blocking operation failed to release provider "
                  "resources, cannot continue");
      unireg_abort(1);
    }
  }
  else if (thd->wsrep_exec_mode == REPL_RECV) {
    thd->wsrep_nbo_ctx->signal();
  }
  thd->wsrep_nbo_ctx->set_toi_released(true);
}

void wsrep_end_nbo_lock(THD* thd, const TABLE_LIST *table_list)
{
  DBUG_ASSERT(thd->wsrep_nbo_ctx);

  // Release TOI critical section if not released yet. This
  // may happen if operation fails in early phase.
  if (thd->wsrep_nbo_ctx->toi_released() == false) {
    wsrep_begin_nbo_unlock(thd);
  }

  DBUG_ASSERT(thd->wsrep_exec_mode == TOTAL_ORDER ||
              thd->wsrep_exec_mode == REPL_RECV);
  wsrep_status_t ret;
  uint32_t flags= WSREP_FLAG_TRX_END;

  wsrep_key_arr_t key_arr= {0, 0};

  if (wsrep_prepare_keys_for_isolation(thd, NULL, NULL, table_list, &key_arr))
  {
    WSREP_ERROR("Failed to prepare keys for NBO end. This is fatal, must abort");
    unireg_abort(1);

  }
  thd_proc_info(thd, "acquiring total order isolation for NBO end");

  DBUG_ASSERT(key_arr.keys_len > 0);

  time_t wait_start= time(NULL);
  while ((ret= wsrep->to_execute_start(wsrep, thd->thread_id,
                                       key_arr.keys, key_arr.keys_len, 0, 0,
                                       flags,
                                       &thd->wsrep_trx_meta))
         == WSREP_CONN_FAIL) {
    if (thd->killed != NOT_KILLED) {
      WSREP_ERROR("Non-blocking operation end failed to sync with group, "
                  "thd killed %d", thd->killed);
      /* Error handling happens outside of while() */
      break;
    }
    usleep(100000);
    if (ulong(time(NULL) - wait_start) >= thd->variables.lock_wait_timeout)
    {
      WSREP_ERROR("Lock wait timeout while waiting NBO end to replicate.");
      break;
    }
  }

  if (ret != WSREP_OK)
  {
    WSREP_ERROR("Failed to acquire total order isolation for non-blocking DDL "
                "end event, provider returned error code %d: "
                "(schema: %s, query: %s)",
                ret, (thd->db.str ? thd->db.str : "(null)"),
                WSREP_QUERY(thd));
    thd->get_stmt_da()->set_overwrite_status(true);
    my_error(ER_ERROR_DURING_COMMIT, MYF(0), ret);
    thd->get_stmt_da()->set_overwrite_status(false);
    WSREP_ERROR("This will leave database in inconsistent state since DDL "
                "execution cannot be terminated in order. Node must rejoin "
                "the cluster via SST");
    wsrep->free_connection(wsrep, thd->thread_id);
    wsrep->disconnect(wsrep);
    // We let the operation to finish out of order in order to release
    // all resources properly. However GTID is cleared so that the
    // event won't be binlogged with incorrect GTID.
    thd->wsrep_trx_meta.gtid= WSREP_GTID_UNDEFINED;
  }

  thd->wsrep_nbo_ctx->set_toi_released(false);
}

#define WSREP_MDL_LOG(severity, msg, schema, schema_len, req, gra)             \
    WSREP_##severity(                                                          \
      "%s\n"                                                                   \
      "schema:  %.*s\n"                                                        \
      "request: (%lld \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)\n"     \
      "granted: (%lld \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)",      \
      msg, schema_len, schema,                                                 \
      (longlong) req->thread_id, (long long)wsrep_thd_trx_seqno(req),          \
      req->wsrep_exec_mode, req->wsrep_query_state_unsafe(),                   \
      req->wsrep_conflict_state_unsafe(),                                      \
      req->get_command(), req->lex->sql_command, req->query(),                 \
      (longlong) gra->thread_id, (long long)wsrep_thd_trx_seqno(gra),          \
      gra->wsrep_exec_mode, gra->wsrep_query_state_unsafe(),                   \
      gra->wsrep_conflict_state_unsafe(),                                      \
      gra->get_command(), gra->lex->sql_command, gra->query());

/**
  Check if request for the metadata lock should be granted to the requester.

  @param  requestor_ctx        The MDL context of the requestor
  @param  ticket               MDL ticket for the requested lock

  @retval TRUE   Lock request can be granted
  @retval FALSE  Lock request cannot be granted
*/

bool wsrep_grant_mdl_exception(MDL_context *requestor_ctx,
                               MDL_ticket *ticket,
                               const MDL_key *key)
{
  /* Fallback to the non-wsrep behaviour */
  if (!WSREP_ON) return FALSE;

  THD *request_thd= requestor_ctx->get_thd();
  THD *granted_thd= ticket->get_ctx()->get_thd();
  bool ret= false;

  const char* schema= key->db_name();
  int schema_len= key->db_name_length();

  /*
    We consider granting MDL exceptions only for appliers (BF THD) and ones
    executing under TOI mode.

    Rules:
    1. If granted/owner THD is also an applier (BF THD) or one executing
       under TOI mode, then we grant the requested lock to the requester
       THD.
       @return true

    2. If granted/owner THD is executing a FLUSH command or already has an
       explicit lock, then do not grant the requested lock to the requester
       THD and it has to wait.
       @return false

    3. In all other cases the granted/owner THD is aborted and the requested
       lock is not granted to the requester THD, thus it has to wait.
       @return false
  */
  mysql_mutex_lock(&request_thd->LOCK_wsrep_thd);
  if (request_thd->wsrep_exec_mode == TOTAL_ORDER ||
      request_thd->wsrep_exec_mode == REPL_RECV) {

    mysql_mutex_unlock(&request_thd->LOCK_wsrep_thd);
    WSREP_MDL_LOG(DEBUG, "MDL conflict ", schema, schema_len,
                  request_thd, granted_thd);
    ticket->wsrep_report(wsrep_debug);

    mysql_mutex_lock(&granted_thd->LOCK_wsrep_thd);
    if (granted_thd->wsrep_exec_mode == TOTAL_ORDER ||
        granted_thd->wsrep_exec_mode == REPL_RECV)
    {
      if (wsrep_thd_is_SR((void*)granted_thd) &&
          !wsrep_thd_is_SR((void*)request_thd))
      {
        WSREP_MDL_LOG(INFO, "MDL conflict, DDL vs SR", 
                      schema, schema_len, request_thd, granted_thd);
        mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
        wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
        ret = FALSE;
      }
      else
      {
        WSREP_MDL_LOG(INFO, "MDL BF-BF conflict", schema, schema_len,
                      request_thd, granted_thd);
        ticket->wsrep_report(true);
        mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
        ret = TRUE;
      }
    }
    else if (granted_thd->lex->sql_command == SQLCOM_FLUSH ||
             granted_thd->mdl_context.has_explicit_locks())
    {
      WSREP_DEBUG("BF thread waiting for FLUSH");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      ret = FALSE;
    }
    else if (request_thd->lex->sql_command == SQLCOM_DROP_TABLE)
    {
      WSREP_DEBUG("DROP caused BF abort");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      ret = FALSE;
    }
    else if (granted_thd->wsrep_query_state() == QUERY_COMMITTING)
    {
      WSREP_DEBUG("mdl granted, but commiting thd abort scheduled");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      ret = FALSE;
    }
    else
    {
      WSREP_MDL_LOG(DEBUG, "MDL conflict-> BF abort", schema, schema_len,
                    request_thd, granted_thd);
      ticket->wsrep_report(wsrep_debug);
      //if (granted_thd->wsrep_conflict_state == CERT_FAILURE)
      switch (granted_thd->wsrep_conflict_state())
      {
      case CERT_FAILURE:
      {
        WSREP_DEBUG("MDL granted is aborting because of cert failure");
        sleep(20);
        mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
        ret = TRUE;
        break;
      }
      case ABORTING:
      {
        WSREP_DEBUG("MDL granted is aborting %d",
                    granted_thd->wsrep_conflict_state());
        mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
        ret = TRUE;
        break;
      }
      case MUST_ABORT:
      case ABORTED:
      case MUST_REPLAY:
      case REPLAYING:
      case RETRY_AUTOCOMMIT:
        WSREP_DEBUG("MDL granted is in %d state",
                    granted_thd->wsrep_conflict_state());
        // fall through
      case NO_CONFLICT:
      {
        mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
        wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
        ret = FALSE;
        break;
      }
      }
    }
  }
  else
  {
    mysql_mutex_unlock(&request_thd->LOCK_wsrep_thd);
  }
  return ret;
}

void
wsrep_last_committed_id(wsrep_gtid_t* gtid)
{
  wsrep->last_committed_id(wsrep, gtid);
}

void
wsrep_node_uuid(wsrep_uuid_t& uuid)
{
  uuid = node_uuid;
}

bool wsrep_node_is_donor()
{
  return (WSREP_ON) ? (local_status.get() == 2) : false;
}

bool wsrep_node_is_synced()
{
  return (WSREP_ON) ? (local_status.get() == 4) : false;
}

int wsrep_must_ignore_error(THD* thd)
{
  const int error= thd->get_stmt_da()->sql_errno();
  const uint flags= sql_command_flags[thd->lex->sql_command];

  DBUG_ASSERT(error);
  DBUG_ASSERT((thd->wsrep_exec_mode == TOTAL_ORDER) ||
              (thd->wsrep_exec_mode == REPL_RECV && thd->wsrep_apply_toi));

  if ((wsrep_ignore_apply_errors & WSREP_IGNORE_ERRORS_ON_DDL))
    goto ignore_error;

  if ((flags & CF_WSREP_MAY_IGNORE_ERRORS) &&
      (wsrep_ignore_apply_errors & WSREP_IGNORE_ERRORS_ON_RECONCILING_DDL))
  {
    switch (error)
    {
    case ER_DB_DROP_EXISTS:
    case ER_BAD_TABLE_ERROR:
    case ER_CANT_DROP_FIELD_OR_KEY:
      goto ignore_error;
    }
  }

  return 0;

ignore_error:
  WSREP_WARN("Ignoring error '%s' on query. "
             "Default database: '%s'. Query: '%s', Error_code: %d",
             thd->get_stmt_da()->message(),
             print_slave_db_safe(thd->db.str),
             thd->query(),
             error);
  return 1;
}

int wsrep_ignored_error_code(Log_event* ev, int error)
{
  const THD* thd= ev->thd;

  DBUG_ASSERT(error);
  DBUG_ASSERT(thd->wsrep_exec_mode == REPL_RECV && !thd->wsrep_apply_toi);

  if ((wsrep_ignore_apply_errors & WSREP_IGNORE_ERRORS_ON_RECONCILING_DML))
  {
    const int ev_type= ev->get_type_code();
    if (ev_type == DELETE_ROWS_EVENT && error == ER_KEY_NOT_FOUND)
      goto ignore_error;
  }

  return 0;

ignore_error:
  WSREP_WARN("Ignoring error '%s' on %s event. Error_code: %d",
             thd->get_stmt_da()->message(),
             ev->get_type_str(),
             error);
  return 1;
}

void *start_wsrep_THD(void *arg)
{
  THD *thd;
  //  wsrep_thd_processor_fun processor= (wsrep_thd_processor_fun)arg;

  Wsrep_thd_args* thd_args= (Wsrep_thd_args*) arg;
  
  if (my_thread_init())
  {
    WSREP_ERROR("Could not initialize thread");
    delete thd_args;
    return(NULL);
  }

  if (!(thd= new THD(next_thread_id(), true)))
  {
    delete thd_args;
    return(NULL);
  }

  mysql_mutex_lock(&LOCK_thread_count);

  if (wsrep_gtid_mode)
  {
    /* Adjust domain_id. */
    thd->variables.gtid_domain_id= wsrep_gtid_domain_id;
  }

  thd->real_id=pthread_self(); // Keep purify happy
  thread_created++;
  threads.append(thd);

  my_net_init(&thd->net,(st_vio*) 0, thd, MYF(0));

  DBUG_PRINT("wsrep",(("creating thread %lld"), (long long)thd->thread_id));
  WSREP_DEBUG("Creating wsrep system thread: %lld", thd->thread_id);
  thd->prior_thr_create_utime= thd->start_utime= microsecond_interval_timer();
  (void) mysql_mutex_unlock(&LOCK_thread_count);

  /* from bootstrap()... */
  thd->bootstrap=1;
  thd->max_client_packet_length= thd->net.max_packet;
  thd->security_ctx->master_access= ~(ulong)0;

  /* from handle_one_connection... */
  pthread_detach_this_thread();

  mysql_thread_set_psi_id(thd->thread_id);
  thd->thr_create_utime=  microsecond_interval_timer();
  if (MYSQL_CALLBACK_ELSE(thread_scheduler, init_new_connection_thread, (), 0))
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    statistic_increment(aborted_connects,&LOCK_status);
    MYSQL_CALLBACK(thread_scheduler, end_thread, (thd, 0));
    delete thd_args;
    goto error;
  }

// </5.1.17>
  /*
    handle_one_connection() is normally the only way a thread would
    start and would always be on the very high end of the stack ,
    therefore, the thread stack always starts at the address of the
    first local variable of handle_one_connection, which is thd. We
    need to know the start of the stack so that we could check for
    stack overruns.
  */
  DBUG_PRINT("wsrep", ("handle_one_connection called by thread %lld\n",
                       (long long)thd->thread_id));
  /* now that we've called my_thread_init(), it is safe to call DBUG_* */

  thd->thread_stack= (char*) &thd;
  if (thd->store_globals())
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    statistic_increment(aborted_connects,&LOCK_status);
    MYSQL_CALLBACK(thread_scheduler, end_thread, (thd, 0));
    delete thd;
    delete thd_args;
    goto error;
  }

  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;
  thd->security_ctx->skip_grants();

  /* handle_one_connection() again... */
  //thd->version= refresh_version;
  thd->proc_info= 0;
  thd->set_command(COM_SLEEP);
  thd->init_for_queries();

  mysql_mutex_lock(&LOCK_thread_count);
  wsrep_running_threads++;
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  thd_args->fun()(thd, thd_args->args());

  thd->store_globals();
  
  WSREP_DEBUG("wsrep system thread: %lld closing", thd->thread_id);
  close_connection(thd, 0);
  delete thd_args;
  
  mysql_mutex_lock(&LOCK_thread_count);
  wsrep_running_threads--;
  WSREP_DEBUG("wsrep running threads now: %lu", wsrep_running_threads);
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  // Note: We can't call THD destructor without crashing
  // if plugins have not been initialized. However, in most of the
  // cases this means that pre SE initialization SST failed and
  // we are going to exit anyway.
  if (plugins_are_initialized)
  {
    net_end(&thd->net);
    MYSQL_CALLBACK(thread_scheduler, end_thread, (thd, 1));
  }
  else
  {
    // TODO: lightweight cleanup to get rid of:
    // 'Error in my_thread_global_end(): 2 threads didn't exit'
    // at server shutdown
  }

  unlink_not_visible_thd(thd);
  delete thd;
  my_thread_end();
  return(NULL);

error:
  WSREP_ERROR("Failed to create/initialize system thread");

  /* Abort if its the first applier/rollbacker thread. */
  if (!mysqld_server_initialized)
    unireg_abort(1);
  else
    return NULL;
}


/**/
static bool abort_replicated(THD *thd)
{
  bool ret_code= false;
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  if (thd->wsrep_query_state()== QUERY_COMMITTING)
  {
    WSREP_DEBUG("aborting replicated trx: %llu", (ulonglong)(thd->real_id));

    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    (void)wsrep_abort_thd(thd, thd, TRUE);
    ret_code= true;
  }
  else
  {
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
  return ret_code;
}


/**/
static inline bool is_client_connection(THD *thd)
{
  return (thd->wsrep_client_thread && thd->variables.wsrep_on);
}


static inline bool is_replaying_connection(THD *thd)
{
  bool ret;

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  ret=  (thd->wsrep_conflict_state() == REPLAYING) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  return ret;
}


static inline bool is_committing_connection(THD *thd)
{
  bool ret;

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  ret=  (thd->wsrep_query_state() == QUERY_COMMITTING) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  return ret;
}


static bool have_client_connections(THD *except_thd)
{
  THD *tmp;

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    if (tmp == except_thd)
        continue;

    DBUG_PRINT("quit",("Informing thread %lld that it's time to die",
                       (longlong) tmp->thread_id));
    if (is_client_connection(tmp) && tmp->killed == KILL_CONNECTION)
    {
      (void)abort_replicated(tmp);
      return true;
    }
  }
  return false;
}

static void wsrep_close_thread(THD *thd)
{
  thd->set_killed(KILL_CONNECTION);
  MYSQL_CALLBACK(thread_scheduler, post_kill_notification, (thd));
  if (thd->mysys_var)
  {
    thd->mysys_var->abort=1;
    mysql_mutex_lock(&thd->mysys_var->mutex);
    if (thd->mysys_var->current_cond)
    {
      mysql_mutex_lock(thd->mysys_var->current_mutex);
      mysql_cond_broadcast(thd->mysys_var->current_cond);
      mysql_mutex_unlock(thd->mysys_var->current_mutex);
    }
    mysql_mutex_unlock(&thd->mysys_var->mutex);
  }
}


static my_bool have_committing_connections()
{
  THD *tmp;
  mysql_mutex_lock(&LOCK_thread_count); // For unlink from list

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    if (!is_client_connection(tmp))
      continue;

    if (is_committing_connection(tmp))
    {
      return TRUE;
    }
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  return FALSE;
}


int wsrep_wait_committing_connections_close(int wait_time)
{
  int sleep_time= 100;

  while (have_committing_connections() && wait_time > 0)
  {
    WSREP_DEBUG("wait for committing transaction to close: %d", wait_time);
    my_sleep(sleep_time);
    wait_time -= sleep_time;
  }
  if (have_committing_connections())
  {
    return 1;
  }
  return 0;
}


void wsrep_close_client_connections(my_bool wait_to_end, THD* except_caller_thd)
{
  /*
    First signal all threads that it's time to die
  */

  THD *tmp;
  mysql_mutex_lock(&LOCK_thread_count); // For unlink from list

  bool kill_cached_threads_saved= kill_cached_threads;
  kill_cached_threads= true; // prevent future threads caching
  mysql_cond_broadcast(&COND_thread_cache); // tell cached threads to die

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    DBUG_PRINT("quit",("Informing thread %lld that it's time to die",
                       (longlong) tmp->thread_id));
    /* We skip slave threads, scheduler & caller on this first loop through. */
    if (!is_client_connection(tmp))
      continue;

    if (tmp == except_caller_thd)
    {
      DBUG_ASSERT(is_client_connection(tmp));
      /* Even though we don't kill the caller we must release resources
       * it might have allocated with the provider */
      wsrep_status_t rcode= wsrep->free_connection(wsrep, tmp->thread_id);
      if (rcode) {
        WSREP_WARN("wsrep failed to free connection context: %lld, code: %d",
                   tmp->thread_id, rcode);
      }
      continue;
    }

    if (is_replaying_connection(tmp))
    {
      tmp->set_killed(KILL_CONNECTION);
      continue;
    }

    /* replicated transactions must be skipped */
    if (abort_replicated(tmp))
      continue;

    WSREP_DEBUG("closing connection %lld", (longlong) tmp->thread_id);
    wsrep_close_thread(tmp);
  }
  mysql_mutex_unlock(&LOCK_thread_count);

  /*
    Sleep for couple of seconds to give threads time to die.
   */
  int max_sleeps= 200;
  while (--max_sleeps > 0 && thread_count > 0)
    my_sleep(10000);

  mysql_mutex_lock(&LOCK_thread_count);
  /*
    Force remaining threads to die by closing the connection to the client
  */

  I_List_iterator<THD> it2(threads);
  while ((tmp=it2++))
  {
#ifndef __bsdi__				// Bug in BSDI kernel
    if (is_client_connection(tmp) &&
        !abort_replicated(tmp)    &&
        !is_replaying_connection(tmp) &&
        tmp != except_caller_thd)
    {
      WSREP_INFO("killing local connection: %lld", (longlong) tmp->thread_id);
      close_connection(tmp,0);
    }
#endif
  }

  DBUG_PRINT("quit",("Waiting for threads to die (count=%u)",thread_count));
  WSREP_DEBUG("waiting for client connections to close: %u", thread_count);

  while (wait_to_end && have_client_connections(except_caller_thd))
  {
    mysql_cond_wait(&COND_thread_count, &LOCK_thread_count);
    DBUG_PRINT("quit",("One thread died (count=%u)", thread_count));
  }

  kill_cached_threads= kill_cached_threads_saved;

  mysql_mutex_unlock(&LOCK_thread_count);

  /* All client connection threads have now been aborted */
}


void wsrep_close_applier(THD *thd)
{
  WSREP_DEBUG("closing applier %lld", (longlong) thd->thread_id);
  wsrep_close_thread(thd);
}


void wsrep_close_threads(THD *thd)
{
  THD *tmp;
  mysql_mutex_lock(&LOCK_thread_count); // For unlink from list

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    DBUG_PRINT("quit",("Informing thread %lld that it's time to die",
                       (longlong) tmp->thread_id));
    /* We skip slave threads & scheduler on this first loop through. */
    if (tmp->wsrep_applier && tmp != thd)
    {
      WSREP_DEBUG("closing wsrep thread %lld", (longlong) tmp->thread_id);
      wsrep_close_thread (tmp);
    }
  }

  mysql_mutex_unlock(&LOCK_thread_count);
}

void wsrep_wait_appliers_close(THD *thd)
{
  /* Wait for wsrep appliers to gracefully exit */
  mysql_mutex_lock(&LOCK_thread_count);
  while (wsrep_running_threads > 2)
  // Rollbacker and post rollbacker threads need to be killed explicitly.
    
  // 1 is for rollbacker thread which needs to be killed explicitly.
  // This gotta be fixed in a more elegant manner if we gonna have arbitrary
  // number of non-applier wsrep threads.
  {
    if (thread_handling > SCHEDULER_ONE_THREAD_PER_CONNECTION)
    {
      mysql_mutex_unlock(&LOCK_thread_count);
      my_sleep(100);
      mysql_mutex_lock(&LOCK_thread_count);
    }
    else
      mysql_cond_wait(&COND_thread_count,&LOCK_thread_count);
    DBUG_PRINT("quit",("One applier died (count=%u)",thread_count));
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  /* Now kill remaining wsrep threads: rollbacker */
  wsrep_close_threads (thd);
  /* and wait for them to die */
  mysql_mutex_lock(&LOCK_thread_count);
  while (wsrep_running_threads > 0)
  {
   if (thread_handling > SCHEDULER_ONE_THREAD_PER_CONNECTION)
    {
      mysql_mutex_unlock(&LOCK_thread_count);
      my_sleep(100);
      mysql_mutex_lock(&LOCK_thread_count);
    }
    else
      mysql_cond_wait(&COND_thread_count,&LOCK_thread_count);
    DBUG_PRINT("quit",("One thread died (count=%u)",thread_count));
  }
  mysql_mutex_unlock(&LOCK_thread_count);

  /* All wsrep applier threads have now been aborted. However, if this thread
     is also applier, we are still running...
  */
}


void wsrep_kill_mysql(THD *thd)
{
  if (mysqld_server_started)
  {
    if (!shutdown_in_progress)
    {
      WSREP_INFO("starting shutdown");
      kill_mysql();
    }
  }
  else
  {
    unireg_abort(1);
  }
}


int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;
  sp_head *sp = thd->lex->sphead;
  sql_mode_t saved_mode= thd->variables.sql_mode;
  String retstr(64);
  LEX_CSTRING returns= empty_clex_str;
  retstr.set_charset(system_charset_info);

  log_query.set_charset(system_charset_info);

  if (sp->m_handler->type() == TYPE_ENUM_FUNCTION)
  {
    sp_returns_type(thd, retstr, sp);
    returns= retstr.lex_cstring();
  }
  if (sp->m_handler->
      show_create_sp(thd, &log_query,
                     sp->m_explicit_name ? sp->m_db : null_clex_str,
                     sp->m_name, sp->m_params, returns,
                     sp->m_body, sp->chistics(),
                     thd->lex->definer[0],
                     thd->lex->create_info,
                     saved_mode))
  {
    WSREP_WARN("SP create string failed: schema: %s, query: %s",
               thd->get_db(), thd->query());
    return 1;
  }

  return wsrep_to_buf_helper(thd, log_query.ptr(), log_query.length(), buf, buf_len);
}


//extern int wsrep_on(THD *thd)
extern int wsrep_on(void *thd_ptr)
{
  THD* thd = (THD*)thd_ptr;
  return (int)(WSREP(thd));
}


bool wsrep_thd_is_wsrep_on(THD *thd)
{
  return thd->variables.wsrep_on;
}


bool wsrep_consistency_check(THD *thd)
{
  return thd->wsrep_consistency_check == CONSISTENCY_CHECK_RUNNING;
}


void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode)
{
  thd->wsrep_exec_mode= mode;
}


void wsrep_thd_set_conflict_state(THD *thd, enum wsrep_conflict_state state)
{
  thd->set_wsrep_conflict_state(state);
}


enum wsrep_exec_mode wsrep_thd_exec_mode(THD *thd)
{
  return thd->wsrep_exec_mode;
}


const char *wsrep_thd_exec_mode_str(THD *thd)
{
  switch (thd->wsrep_exec_mode)
  {
  case LOCAL_STATE:    return "local";
  case REPL_RECV:      return "applier";
  case TOTAL_ORDER:    return "total order";
  case LOCAL_COMMIT:   return "local commit";
  case LOCAL_ROLLBACK: return "local rollback";
  }
  return "void";

}


enum wsrep_query_state wsrep_thd_query_state(THD *thd)
{
  return thd->wsrep_query_state();
}


const char *wsrep_thd_query_state_str(THD *thd)
{
  switch (thd->wsrep_query_state_unsafe())
  {
  case QUERY_IDLE:           return "idle";
  case QUERY_EXEC:           return "executing";
  case QUERY_COMMITTING:     return "committing";
  case QUERY_ORDERED_COMMIT: return "ordered_commit";
  case QUERY_EXITING:        return "exiting";

  }
  return "void";
}

enum wsrep_conflict_state wsrep_thd_get_conflict_state(THD *thd)
{
  return thd->wsrep_conflict_state();
}


const char *wsrep_thd_conflict_state_str(THD *thd)
{
  switch (thd->wsrep_conflict_state_unsafe())
  {
  case NO_CONFLICT:      return "no conflict";
  case MUST_ABORT:       return "must abort";
  case ABORTING:         return "aborting";
  case ABORTED:          return "aborted";
  case MUST_REPLAY:      return "must replay";
  case REPLAYING:        return "replaying";
  case RETRY_AUTOCOMMIT: return "retrying";
  case CERT_FAILURE:     return "cert failure";
  }
  return "void";
}


wsrep_ws_handle_t* wsrep_thd_ws_handle(THD *thd)
{
  return &thd->wsrep_ws_handle;
}


void wsrep_thd_LOCK(THD *thd)
{
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
}


void wsrep_thd_UNLOCK(THD *thd)
{
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
}


time_t wsrep_thd_query_start(THD *thd) 
{
  return thd->query_start();
}


uint32 wsrep_thd_wsrep_rand(THD *thd) 
{
  return thd->wsrep_rand;
}

my_thread_id wsrep_thd_thread_id(THD *thd)
{
  return thd->thread_id;
}

int64_t wsrep_thd_trx_seqno(const THD *thd)
{
  return (thd) ? thd->wsrep_trx_meta.gtid.seqno : WSREP_SEQNO_UNDEFINED;
}


query_id_t wsrep_thd_query_id(THD *thd) 
{
  return thd->query_id;
}

wsrep_trx_id_t wsrep_thd_next_trx_id(THD *thd)
{
  return thd->wsrep_next_trx_id();
}
wsrep_trx_id_t wsrep_thd_trx_id(THD *thd)
{
  return thd->wsrep_trx_id();
}

char *wsrep_thd_query(THD *thd)
{
  return (thd) ? thd->query() : NULL;
}


query_id_t wsrep_thd_wsrep_last_query_id(THD *thd) 
{
  return thd->wsrep_last_query_id;
}


void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id) 
{
  thd->wsrep_last_query_id= id;
}


void wsrep_thd_awake(THD *thd, my_bool signal)
{
  if (signal)
  {
    thd->awake(KILL_QUERY);
  }
  else
  {
    mysql_mutex_lock(&LOCK_wsrep_replaying);
    mysql_cond_broadcast(&COND_wsrep_replaying);
    mysql_mutex_unlock(&LOCK_wsrep_replaying);
  }
}


int wsrep_thd_retry_counter(THD *thd)
{
  return(thd->wsrep_retry_counter);
}


bool wsrep_thd_ignore_table(THD *thd)
{
  return thd->wsrep_ignore_table;
}


extern int
//wsrep_trx_order_before(void *thd1, void *thd2)
wsrep_trx_order_before(THD *thd1, THD *thd2)
{
    if (wsrep_thd_trx_seqno((THD*)thd1) < wsrep_thd_trx_seqno((THD*)thd2)) {
        WSREP_DEBUG("BF conflict, order: %lld %lld\n",
                    (long long)wsrep_thd_trx_seqno((THD*)thd1),
                    (long long)wsrep_thd_trx_seqno((THD*)thd2));
        return 1;
    }
    WSREP_DEBUG("waiting for BF, trx order: %lld %lld\n",
                (long long)wsrep_thd_trx_seqno((THD*)thd1),
                (long long)wsrep_thd_trx_seqno((THD*)thd2));
    return 0;
}

int
wsrep_trx_is_aborting(THD *thd_ptr)
{
  if (thd_ptr) {
    if ((((THD *)thd_ptr)->wsrep_conflict_state() == MUST_ABORT) ||
        (((THD *)thd_ptr)->wsrep_conflict_state() == ABORTING)) {
      return 1;
    }
  }
  return 0;
}

void
wsrep_thd_last_written_gtid(THD *thd, wsrep_gtid_t* gtid)
{
  *gtid = WSREP_GTID_UNDEFINED;
  if (thd)
  {
    *gtid = thd->wsrep_last_written_gtid;
  }
}

ulong
wsrep_thd_trx_fragment_size(THD *thd)
{
  if (thd)
    return thd->variables.wsrep_trx_fragment_size;
  return 0;
}

bool
wsrep_thd_is_streaming(THD* thd)
{
  return thd && thd->wsrep_is_streaming();
}

#if 0
my_bool wsrep_thd_no_gaps(const void *thd_ptr)
{
  return ((THD*)thd_ptr)->wsrep_no_gaps;
}
#endif // 0

void wsrep_copy_query(THD *thd)
{
  thd->wsrep_retry_command   = thd->get_command();
  thd->wsrep_retry_query_len = thd->query_length();
  if (thd->wsrep_retry_query) {
	  my_free(thd->wsrep_retry_query);
  }
  thd->wsrep_retry_query     = (char *)my_malloc(
                                 thd->wsrep_retry_query_len + 1, MYF(0));
  strncpy(thd->wsrep_retry_query, thd->query(), thd->wsrep_retry_query_len);
  thd->wsrep_retry_query[thd->wsrep_retry_query_len] = '\0';
}


bool wsrep_is_show_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command >= 0 && command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_STATUS_COMMAND) != 0;
}

bool wsrep_create_like_table(THD* thd, TABLE_LIST* table,
                             TABLE_LIST* src_table,
                             HA_CREATE_INFO *create_info)
{
  if (create_info->tmp_table())
  {
    /* CREATE TEMPORARY TABLE LIKE must be skipped from replication */
    WSREP_DEBUG("CREATE TEMPORARY TABLE LIKE... skipped replication\n %s", 
                thd->query());
  }
  else if (!(thd->find_temporary_table(src_table)))
  {
    /* this is straight CREATE TABLE LIKE... with no tmp tables */
    WSREP_TO_ISOLATION_BEGIN(table->db.str, table->table_name.str, NULL);
  }
  else
  {
    /* here we have CREATE TABLE LIKE <temporary table> 
       the temporary table definition will be needed in slaves to
       enable the create to succeed
     */
    TABLE_LIST tbl;
    bzero((void*) &tbl, sizeof(tbl));
    tbl.db= src_table->db;
    tbl.table_name= tbl.alias= src_table->table_name;
    tbl.table= src_table->table;
    char buf[2048];
    String query(buf, sizeof(buf), system_charset_info);
    query.length(0);  // Have to zero it since constructor doesn't

    (void)  show_create_table(thd, &tbl, &query, NULL, WITH_DB_NAME);
    WSREP_DEBUG("TMP TABLE: %s", query.ptr());

    thd->wsrep_TOI_pre_query=     query.ptr();
    thd->wsrep_TOI_pre_query_len= query.length();

    WSREP_TO_ISOLATION_BEGIN(table->db.str, table->table_name.str, NULL);

    thd->wsrep_TOI_pre_query=      NULL;
    thd->wsrep_TOI_pre_query_len= 0;
  }

  return(false);

error:
  thd->wsrep_TOI_pre_query= NULL;
  return (true);
}


int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len)
{
  LEX *lex= thd->lex;
  String stmt_query;

  LEX_CSTRING definer_user;
  LEX_CSTRING definer_host;

  if (!lex->definer)
  {
    if (!thd->slave_thread)
    {
      if (!(lex->definer= create_default_definer(thd, false)))
        return 1;
    }
  }

  if (lex->definer)
  {
    /* SUID trigger. */
    LEX_USER *d= get_current_user(thd, lex->definer);

    if (!d)
      return 1;

    definer_user= d->user;
    definer_host= d->host;
  }
  else
  {
    /* non-SUID trigger. */

    definer_user.str= 0;
    definer_user.length= 0;

    definer_host.str= 0;
    definer_host.length= 0;
  }

  stmt_query.append(STRING_WITH_LEN("CREATE "));

  append_definer(thd, &stmt_query, &definer_user, &definer_host);

  LEX_CSTRING stmt_definition;
  stmt_definition.str= (char*) thd->lex->stmt_definition_begin;
  stmt_definition.length= thd->lex->stmt_definition_end
    - thd->lex->stmt_definition_begin;
  trim_whitespace(thd->charset(), &stmt_definition);

  stmt_query.append(stmt_definition.str, stmt_definition.length);

  return wsrep_to_buf_helper(thd, stmt_query.c_ptr(), stmt_query.length(),
                             buf, buf_len);
}

/***** callbacks for wsrep service ************/

my_bool get_wsrep_debug()
{
  return wsrep_debug;
}

my_bool get_wsrep_load_data_splitting()
{
  return wsrep_load_data_splitting;
}

long get_wsrep_protocol_version()
{
  return wsrep_protocol_version;
}

my_bool get_wsrep_drupal_282555_workaround()
{
  return wsrep_drupal_282555_workaround;
}

my_bool get_wsrep_recovery()
{
  return wsrep_recovery;
}

my_bool get_wsrep_log_conflicts()
{
  return wsrep_log_conflicts;
}

wsrep_t *get_wsrep()
{
  return wsrep;
}

my_bool get_wsrep_certify_nonPK()
{
  return wsrep_certify_nonPK;
}

bool wsrep_provider_is_SR_capable()
{
  return wsrep->capabilities(wsrep) & WSREP_CAP_STREAMING;
}


int wsrep_ordered_commit_if_no_binlog(THD* thd)
{
  return 0;
  if (!(wsrep_emulate_bin_log && thd->wsrep_trx_must_order_commit()))
  {
    return 0;
  }
  int ret= 0;
  switch (thd->wsrep_exec_mode)
  {
  case LOCAL_STATE:
  case TOTAL_ORDER:
    /* Statement commit may get us here */
    break;
  case LOCAL_COMMIT:
    ret= wsrep_ordered_commit(thd, true, wsrep_apply_error());
    break;
  case REPL_RECV:
  {
    wsrep_buf_t const err= { NULL, 0 };
    wsrep_status_t rcode=
      wsrep->commit_order_leave(wsrep, &thd->wsrep_ws_handle, &err);
    if (rcode != WSREP_OK)
    {
      DBUG_ASSERT(rcode == WSREP_NODE_FAIL);
      WSREP_ERROR("Failed to leave commit order critical section (WOKINB), "
                  "rcode: %d", rcode);
      ret= 1;
    }
    if (!ret)
    {
      mysql_mutex_lock(&thd->LOCK_wsrep_thd);
      thd->set_wsrep_query_state(QUERY_ORDERED_COMMIT);
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    }
    break;
  }
  default:
    DBUG_ASSERT(0);
    WSREP_WARN("Call to wsrep_commit_order_leave_if_no_binlog called in %s",
               wsrep_thd_exec_mode_str(thd));
    break;
  }
  return ret;
}

wsrep_status_t wsrep_tc_log_commit(THD* thd)
{
  if (wsrep_before_commit(thd, true))
  {
    return WSREP_TRX_FAIL;
  }
  if (binlog_hton->commit(binlog_hton, thd, true))
  {
    WSREP_ERROR("Binlog hton commit fail");
    return WSREP_TRX_FAIL;
  }

  if (wsrep_after_commit(thd, true))
  {
    return WSREP_TRX_FAIL;
  }

  /* Set wsrep transaction id if not set. */
  if (thd->wsrep_trx_id() == WSREP_UNDEFINED_TRX_ID)
  {
    if (thd->wsrep_next_trx_id() == WSREP_UNDEFINED_TRX_ID)
    {
      thd->set_wsrep_next_trx_id(thd->query_id);
    }
    DBUG_ASSERT(thd->wsrep_next_trx_id() != WSREP_UNDEFINED_TRX_ID);

    wsrep_ws_handle_for_trx(&thd->wsrep_ws_handle, thd->wsrep_next_trx_id());
  }
  DBUG_ASSERT(thd->wsrep_trx_id() != WSREP_UNDEFINED_TRX_ID);

  return WSREP_OK;
}
