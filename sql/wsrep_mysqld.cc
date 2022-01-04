/* Copyright 2008-2021 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.x1

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include <sql_plugin.h> // SHOW_MY_BOOL
#include <mysqld.h>
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
#include "wsrep_xid.h"
#include <cstdio>
#include <cstdlib>
#include "log_event.h"
#include "sql_plugin.h"                         /* wsrep_plugins_pre_init() */
#include <vector>

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
const char *wsrep_data_home_dir;
const char *wsrep_dbug_option;
const char *wsrep_notify_cmd;

my_bool wsrep_debug;                            // Enable debug level logging
my_bool wsrep_convert_LOCK_to_trx;              // Convert locking sessions to trx
my_bool wsrep_auto_increment_control;           // Control auto increment variables
my_bool wsrep_drupal_282555_workaround;         // Retry autoinc insert after dupkey
my_bool wsrep_certify_nonPK;                    // Certify, even when no primary key
ulong   wsrep_certification_rules      = WSREP_CERTIFICATION_RULES_STRICT;
my_bool wsrep_recovery;                         // Recovery
my_bool wsrep_replicate_myisam;                 // Enable MyISAM replication
my_bool wsrep_log_conflicts;
my_bool wsrep_load_data_splitting;              // Commit load data every 10K intervals
my_bool wsrep_slave_UK_checks;                  // Slave thread does UK checks
my_bool wsrep_slave_FK_checks;                  // Slave thread does FK checks
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
long wsrep_max_protocol_version= 3;             // Maximum protocol version to use

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
mysql_mutex_t LOCK_wsrep_rollback;
mysql_cond_t  COND_wsrep_rollback;
wsrep_aborting_thd_t wsrep_aborting_thd= NULL;
mysql_mutex_t LOCK_wsrep_replaying;
mysql_cond_t  COND_wsrep_replaying;
mysql_mutex_t LOCK_wsrep_slave_threads;
mysql_mutex_t LOCK_wsrep_desync;
mysql_mutex_t LOCK_wsrep_config_state;

int wsrep_replaying= 0;
ulong  wsrep_running_threads = 0; // # of currently running wsrep
				  // # threads
ulong  wsrep_running_applier_threads = 0; // # of running applier threads
ulong  wsrep_running_rollbacker_threads = 0; // # of running
					     // # rollbacker threads
ulong  my_bind_addr;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_wsrep_rollback,
  key_LOCK_wsrep_replaying, key_LOCK_wsrep_ready, key_LOCK_wsrep_sst,
  key_LOCK_wsrep_sst_thread, key_LOCK_wsrep_sst_init,
  key_LOCK_wsrep_slave_threads, key_LOCK_wsrep_desync,
  key_LOCK_wsrep_config_state;

PSI_cond_key key_COND_wsrep_rollback,
  key_COND_wsrep_replaying, key_COND_wsrep_ready, key_COND_wsrep_sst,
  key_COND_wsrep_sst_init, key_COND_wsrep_sst_thread;

PSI_file_key key_file_wsrep_gra_log;

static PSI_mutex_info wsrep_mutexes[]=
{
  { &key_LOCK_wsrep_ready, "LOCK_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_LOCK_wsrep_sst_init, "LOCK_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_rollback, "LOCK_wsrep_rollback", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_replaying, "LOCK_wsrep_replaying", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_slave_threads, "LOCK_wsrep_slave_threads", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_desync, "LOCK_wsrep_desync", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_config_state, "LOCK_wsrep_config_state", PSI_FLAG_GLOBAL}
};

static PSI_cond_info wsrep_conds[]=
{
  { &key_COND_wsrep_ready, "COND_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst, "COND_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_init, "COND_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_COND_wsrep_rollback, "COND_wsrep_rollback", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_replaying, "COND_wsrep_replaying", PSI_FLAG_GLOBAL}
};

static PSI_file_info wsrep_files[]=
{
  { &key_file_wsrep_gra_log, "wsrep_gra_log", 0}
};

PSI_thread_key key_wsrep_sst_joiner, key_wsrep_sst_donor,
  key_wsrep_rollbacker, key_wsrep_applier;

static PSI_thread_info wsrep_threads[]=
{
 {&key_wsrep_sst_joiner, "wsrep_sst_joiner_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_sst_donor, "wsrep_sst_donor_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_rollbacker, "wsrep_rollbacker_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_applier, "wsrep_applier_thread", PSI_FLAG_GLOBAL}
};

#endif /* HAVE_PSI_INTERFACE */

my_bool wsrep_inited                   = 0; // initialized ?

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
/* End wsrep status variables */

wsrep_uuid_t     local_uuid   = WSREP_UUID_UNDEFINED;
wsrep_seqno_t    local_seqno  = WSREP_SEQNO_UNDEFINED;
long             wsrep_protocol_version = 3;

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
                              const wsrep_uuid_t* const node_uuid,
                              wsrep_seqno_t       const node_seqno)
{
  char uuid_str[37];
  char msg[256];

  wsrep_uuid_print (group_uuid, uuid_str, sizeof(uuid_str));
  snprintf (msg, 255, "WSREP: Group state: %s:%lld",
            uuid_str, (long long)group_seqno);
  wsrep_log_cb (level, msg);

  wsrep_uuid_print (node_uuid, uuid_str, sizeof(uuid_str));
  snprintf (msg, 255, "WSREP: Local state: %s:%lld",
            uuid_str, (long long)node_seqno);
  wsrep_log_cb (level, msg);
}

#ifdef GTID_SUPPORT
void wsrep_init_sidno(const wsrep_uuid_t& wsrep_uuid)
{
  /* generate new Sid map entry from inverted uuid */
  rpl_sid sid;
  wsrep_uuid_t ltid_uuid;

  for (size_t i= 0; i < sizeof(ltid_uuid.data); ++i)
  {
      ltid_uuid.data[i] = ~wsrep_uuid.data[i];
  }

  sid.copy_from(ltid_uuid.data);
  global_sid_lock->wrlock();
  wsrep_sidno= global_sid_map->add_sid(sid);
  WSREP_INFO("Initialized wsrep sidno %d", wsrep_sidno);
  global_sid_lock->unlock();
}
#endif /* GTID_SUPPORT */

static wsrep_cb_status_t
wsrep_view_handler_cb (void*                    app_ctx,
                       void*                    recv_ctx,
                       const wsrep_view_info_t* view,
                       const char*              state,
                       size_t                   state_len,
                       void**                   sst_req,
                       size_t*                  sst_req_len)
{
  *sst_req     = NULL;
  *sst_req_len = 0;

  wsrep_member_status_t memb_status= wsrep_config_state->get_status();

  if (memcmp(&cluster_uuid, &view->state_id.uuid, sizeof(wsrep_uuid_t)))
  {
    memcpy(&cluster_uuid, &view->state_id.uuid, sizeof(cluster_uuid));

    wsrep_uuid_print (&cluster_uuid, cluster_uuid_str,
                      sizeof(cluster_uuid_str));
  }

  wsrep_cluster_conf_id= view->view;
  wsrep_cluster_status= cluster_status_str[view->status];
  wsrep_cluster_size= view->memb_num;
  wsrep_local_index= view->my_idx;

  WSREP_INFO("New cluster view: global state: %s:%lld, view# %lld: %s, "
             "number of nodes: %ld, my index: %ld, protocol version %d",
             wsrep_cluster_state_uuid, (long long)view->state_id.seqno,
             (long long)wsrep_cluster_conf_id, wsrep_cluster_status,
             wsrep_cluster_size, wsrep_local_index, view->proto_ver);

  /* Proceed further only if view is PRIMARY */
  if (WSREP_VIEW_PRIMARY != view->status)
  {
#ifdef HAVE_QUERY_CACHE
    // query cache must be initialised by now
    query_cache.flush();
#endif /* HAVE_QUERY_CACHE */

    wsrep_ready_set(FALSE);
    memb_status= WSREP_MEMBER_UNDEFINED;
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

  if (view->state_gap)
  {
    WSREP_WARN("Gap in state sequence. Need state transfer.");

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
        WSREP_DEBUG("[debug]: closing client connections for PRIM");
        wsrep_close_client_connections(FALSE);
    }

    ssize_t const req_len= wsrep_sst_prepare (sst_req);

    if (req_len < 0)
    {
      WSREP_ERROR("SST preparation failed: %zd (%s)", -req_len,
                  strerror(-req_len));
      memb_status= WSREP_MEMBER_UNDEFINED;
    }
    else
    {
      assert(sst_req != NULL);
      *sst_req_len= req_len;
      memb_status= WSREP_MEMBER_JOINER;
    }
  }
  else
  {
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
        wsrep_SE_init_wait();
      }
      else
      {
        local_uuid=  cluster_uuid;
        local_seqno= view->state_id.seqno;
      }
      /* Init storage engine XIDs from first view */
      wsrep_set_SE_checkpoint(local_uuid, local_seqno);
#ifdef GTID_SUPPORT
      wsrep_init_sidno(local_uuid);
#endif /* GTID_SUPPORT */
      memb_status= WSREP_MEMBER_JOINED;
    }

    // just some sanity check
    if (memcmp (&local_uuid, &cluster_uuid, sizeof (wsrep_uuid_t)))
    {
      WSREP_ERROR("Undetected state gap. Can't continue.");
      wsrep_log_states(WSREP_LOG_FATAL, &cluster_uuid, view->state_id.seqno,
                       &local_uuid, -1);
      unireg_abort(1);
    }
  }

  if (wsrep_auto_increment_control)
  {
    global_system_variables.auto_increment_offset= view->my_idx + 1;
    global_system_variables.auto_increment_increment= view->memb_num;
  }

  { /* capabilities may be updated on new configuration */
    uint64_t const caps(wsrep->capabilities (wsrep));

    my_bool const idc((caps & WSREP_CAP_INCREMENTAL_WRITESET) != 0);
    if (TRUE == wsrep_incremental_data_collection && FALSE == idc)
    {
      WSREP_WARN("Unsupported protocol downgrade: "
                 "incremental data collection disabled. Expect abort.");
    }
    wsrep_incremental_data_collection = idc;
  }

out:
  if (view->status == WSREP_VIEW_PRIMARY) wsrep_startup= FALSE;
  wsrep_config_state->set(memb_status, view);

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

static void wsrep_synced_cb(void* app_ctx)
{
  WSREP_INFO("Synchronized with group, ready for connections");
  my_bool signal_main= wsrep_ready_set(TRUE);
  wsrep_config_state->set(WSREP_MEMBER_SYNCED);

  if (signal_main)
  {
      wsrep_SE_init_grab();
      // Signal mysqld init thread to continue
      wsrep_sst_complete (&local_uuid, local_seqno, false);
      // and wait for SE initialization
      wsrep_SE_init_wait();
  }
  if (wsrep_restart_slave_activated)
  {
    int rcode;
    WSREP_INFO("MySQL slave restart");
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
}

static void wsrep_init_position()
{
  /* read XIDs from storage engines */
  wsrep_uuid_t uuid;
  wsrep_seqno_t seqno;
  wsrep_get_SE_checkpoint(uuid, seqno);

  if (!memcmp(&uuid, &WSREP_UUID_UNDEFINED, sizeof(wsrep_uuid_t)))
  {
    WSREP_INFO("Read nil XID from storage engines, skipping position init");
    return;
  }

  char uuid_str[40] = {0, };
  wsrep_uuid_print(&uuid, uuid_str, sizeof(uuid_str));
  WSREP_INFO("Initial position: %s:%lld", uuid_str, (long long)seqno);

  if (!memcmp(&local_uuid, &WSREP_UUID_UNDEFINED, sizeof(local_uuid)) &&
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
      wsrep_ready_set(FALSE);
      wsrep->free(wsrep);
      free(wsrep);
      wsrep = NULL;
    }
    else
    {
      wsrep_inited= 1;
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

  struct wsrep_gtid const state_id = { local_uuid, local_seqno };

  wsrep_args.data_dir        = wsrep_data_home_dir;
  wsrep_args.node_name       = (wsrep_node_name) ? wsrep_node_name : "";
  wsrep_args.node_address    = node_addr;
  wsrep_args.node_incoming   = inc_addr;
  wsrep_args.options         = (wsrep_provider_options) ?
                                wsrep_provider_options : "";
  wsrep_args.proto_ver       = wsrep_max_protocol_version;

  wsrep_args.state_id        = &state_id;

  wsrep_args.logger_cb       = wsrep_log_cb;
  wsrep_args.view_handler_cb = wsrep_view_handler_cb;
  wsrep_args.apply_cb        = wsrep_apply_cb;
  wsrep_args.commit_cb       = wsrep_commit_cb;
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
  } else {
    wsrep_inited= 1;
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
  mysql_thread_register("sql", wsrep_threads, array_elements(wsrep_threads));
#endif

  mysql_mutex_init(key_LOCK_wsrep_ready, &LOCK_wsrep_ready, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_ready, &COND_wsrep_ready, NULL);
  mysql_mutex_init(key_LOCK_wsrep_sst, &LOCK_wsrep_sst, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_sst, &COND_wsrep_sst, NULL);
  mysql_mutex_init(key_LOCK_wsrep_sst_init, &LOCK_wsrep_sst_init, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_sst_init, &COND_wsrep_sst_init, NULL);
  mysql_mutex_init(key_LOCK_wsrep_rollback, &LOCK_wsrep_rollback, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_rollback, &COND_wsrep_rollback, NULL);
  mysql_mutex_init(key_LOCK_wsrep_replaying, &LOCK_wsrep_replaying, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_replaying, &COND_wsrep_replaying, NULL);
  mysql_mutex_init(key_LOCK_wsrep_slave_threads, &LOCK_wsrep_slave_threads, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_desync, &LOCK_wsrep_desync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_config_state, &LOCK_wsrep_config_state, MY_MUTEX_INIT_FAST);

  DBUG_VOID_RETURN;
}

/* This is wrapper for wsrep_break_lock in thr_lock.c */
static int wsrep_thr_abort_thd(void *bf_thd_ptr, void *victim_thd_ptr, my_bool signal)
{
  THD* victim_thd= (THD *) victim_thd_ptr;
  /* We need to lock THD::LOCK_thd_data to protect victim
  from concurrent usage or disconnect or delete. */
  mysql_mutex_lock(&victim_thd->LOCK_thd_data);
  int res= wsrep_abort_thd(bf_thd_ptr, victim_thd_ptr, signal);
  return res;
}


void wsrep_init_startup (bool first)
{
  if (wsrep_init()) unireg_abort(1);

  wsrep_thr_lock_init(
     (wsrep_thd_is_brute_force_fun)wsrep_thd_is_BF,
     (wsrep_abort_thd_fun)wsrep_thr_abort_thd,
     wsrep_debug, wsrep_convert_LOCK_to_trx,
     (wsrep_on_fun)wsrep_on);

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
  wsrep_unload(wsrep);
  wsrep= 0;
  provider_name[0]=    '\0';
  provider_version[0]= '\0';
  provider_vendor[0]=  '\0';

  wsrep_inited= 0;

  if (free_options)
  {
    wsrep_sst_auth_free();
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
  mysql_mutex_destroy(&LOCK_wsrep_rollback);
  mysql_cond_destroy(&COND_wsrep_rollback);
  mysql_mutex_destroy(&LOCK_wsrep_replaying);
  mysql_cond_destroy(&COND_wsrep_replaying);
  mysql_mutex_destroy(&LOCK_wsrep_slave_threads);
  mysql_mutex_destroy(&LOCK_wsrep_desync);
  mysql_mutex_destroy(&LOCK_wsrep_config_state);
  delete wsrep_config_state;
  wsrep_config_state= 0;                        // Safety
}

void wsrep_recover()
{
  char uuid_str[40];

  if (!memcmp(&local_uuid, &WSREP_UUID_UNDEFINED, sizeof(wsrep_uuid_t)) &&
      local_seqno == -2)
  {
    wsrep_uuid_print(&local_uuid, uuid_str, sizeof(uuid_str));
    WSREP_INFO("Position %s:%lld given at startup, skipping position recovery",
               uuid_str, (long long)local_seqno);
    return;
  }
  wsrep_uuid_t uuid;
  wsrep_seqno_t seqno;
  wsrep_get_SE_checkpoint(uuid, seqno);
  wsrep_uuid_print(&uuid, uuid_str, sizeof(uuid_str));
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

  wsrep_close_client_connections(TRUE);

  /* wait until appliers have stopped */
  wsrep_wait_appliers_close(thd);

  return;
}

bool wsrep_start_replication()
{
  wsrep_status_t rcode;

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

  bool const bootstrap= wsrep_new_cluster;

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
  return (thd->variables.wsrep_sync_wait & mask) &&
    thd->variables.wsrep_on &&
    !(thd->variables.wsrep_dirty_reads &&
      !is_update_query(thd->lex->sql_command)) &&
    !thd->in_active_multi_stmt_transaction() &&
    thd->wsrep_conflict_state != REPLAYING &&
    thd->wsrep_sync_wait_gtid.seqno == WSREP_SEQNO_UNDEFINED;
}

bool wsrep_sync_wait (THD* thd, uint mask)
{
  if (wsrep_must_sync_wait(thd, mask))
  {
    WSREP_DEBUG("wsrep_sync_wait: thd->variables.wsrep_sync_wait = %u, mask = %u",
                thd->variables.wsrep_sync_wait, mask);
    // This allows autocommit SELECTs and a first SELECT after SET AUTOCOMMIT=0
    // TODO: modify to check if thd has locked any rows.
    wsrep_status_t ret= wsrep->causal_read (wsrep, &thd->wsrep_sync_wait_gtid);

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
  {
    *key_len= 0;
    if (db)
    {
      // sql_print_information("%s.%s", db, table);
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
    break;
  }
  default:
    return false;
  }
  return true;
}


static bool wsrep_prepare_key_for_isolation(const char* db,
                                            const char* table,
                                            wsrep_key_arr_t* ka)
{
  wsrep_key_t* tmp;

  if (!ka->keys)
    tmp= (wsrep_key_t*)my_malloc((ka->keys_len + 1) * sizeof(wsrep_key_t),
                                 MYF(0));
  else
    tmp= (wsrep_key_t*)my_realloc(ka->keys,
                                 (ka->keys_len + 1) * sizeof(wsrep_key_t),
                                 MYF(0));

  if (!tmp)
  {
    WSREP_ERROR("Can't allocate memory for key_array");
    return false;
  }
  ka->keys= tmp;
  if (!(ka->keys[ka->keys_len].key_parts= (wsrep_buf_t*)
        my_malloc(sizeof(wsrep_buf_t)*2, MYF(0))))
  {
    WSREP_ERROR("Can't allocate memory for key_parts");
    return false;
  }
  ka->keys[ka->keys_len].key_parts_num= 2;
  ++ka->keys_len;
  if (!wsrep_prepare_key_for_isolation(db, table,
                                       (wsrep_buf_t*)ka->keys[ka->keys_len - 1].key_parts,
                                       &ka->keys[ka->keys_len - 1].key_parts_num))
  {
    WSREP_ERROR("Preparing keys for isolation failed");
    return false;
  }

  return true;
}


static bool wsrep_prepare_keys_for_alter_add_fk(char* child_table_db,
                                                Alter_info* alter_info,
                                                wsrep_key_arr_t* ka)
{
  Key *key;
  List_iterator<Key> key_iterator(alter_info->key_list);
  while ((key= key_iterator++))
  {
    if (key->type == Key::FOREIGN_KEY)
    {
      Foreign_key *fk_key= (Foreign_key *)key;
      const char *db_name= fk_key->ref_db.str;
      const char *table_name= fk_key->ref_table.str;
      if (!db_name)
      {
        db_name= child_table_db;
      }
      if (!wsrep_prepare_key_for_isolation(db_name, table_name, ka))
      {
        return false;
      }
    }
  }
  return true;
}


static bool wsrep_prepare_keys_for_isolation(THD*              thd,
                                             const char*       db,
                                             const char*       table,
                                             const TABLE_LIST* table_list,
                                             Alter_info*       alter_info,
                                             wsrep_key_arr_t*  ka)
{
  ka->keys= 0;
  ka->keys_len= 0;

  if (db || table)
  {
    if (!wsrep_prepare_key_for_isolation(db, table, ka))
      goto err;
  }

  for (const TABLE_LIST* table= table_list; table; table= table->next_global)
  {
    if (!wsrep_prepare_key_for_isolation(table->db, table->table_name, ka))
      goto err;
  }

  if (alter_info && (alter_info->flags & (Alter_info::ADD_FOREIGN_KEY)))
  {
    if (!wsrep_prepare_keys_for_alter_add_fk(table_list->db, alter_info, ka))
      goto err;
  }

  return false;

err:
  wsrep_keys_free(ka);
  return true;
}


/* Prepare key list from db/table and table_list */
bool wsrep_prepare_keys_for_isolation(THD*              thd,
                                      const char*       db,
                                      const char*       table,
                                      const TABLE_LIST* table_list,
                                      wsrep_key_arr_t*  ka)
{
  return wsrep_prepare_keys_for_isolation(thd, db, table, table_list, NULL, ka);
}


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
  Log_event_writer writer(&tmp_io_cache);
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
               (thd->db ? thd->db : "(null)"), thd->query());
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

    String buff;
    const LEX_STRING command[3]=
      {{ C_STRING_WITH_LEN("CREATE ") },
       { C_STRING_WITH_LEN("ALTER ") },
       { C_STRING_WITH_LEN("CREATE OR REPLACE ") }};

    buff.append(command[thd->lex->create_view_mode].str,
                command[thd->lex->create_view_mode].length);

    LEX_USER *definer;

    if (lex->definer)
    {
      definer= get_current_user(thd, lex->definer);
    }
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

    views->algorithm    = lex->create_view_algorithm;
    views->view_suid    = lex->create_view_suid;
    views->with_check   = lex->create_view_check;

    view_store_options(thd, views, &buff);
    buff.append(STRING_WITH_LEN("VIEW "));
    /* Test if user supplied a db (ie: we did not use thd->db) */
    if (views->db && views->db[0] &&
        (thd->db == NULL || strcmp(views->db, thd->db)))
    {
      append_identifier(thd, &buff, views->db,
                        views->db_length);
      buff.append('.');
    }
    append_identifier(thd, &buff, views->table_name,
                      views->table_name_length);
    if (lex->view_list.elements)
    {
      List_iterator_fast<LEX_STRING> names(lex->view_list);
      LEX_STRING *name;
      int i;

      for (i= 0; (name= names++); i++)
      {
        buff.append(i ? ", " : "(");
        append_identifier(thd, &buff, name->str, name->length);
      }
      buff.append(')');
    }
    buff.append(STRING_WITH_LEN(" AS "));
    //buff.append(views->source.str, views->source.length);
    buff.append(thd->lex->create_view_select.str,
                thd->lex->create_view_select.length);
    //int errcode= query_error_code(thd, TRUE);
    //if (thd->binlog_query(THD::STMT_QUERY_TYPE,
    //                      buff.ptr(), buff.length(), FALSE, FALSE, FALSE, errcod
    return wsrep_to_buf_helper(thd, buff.ptr(), buff.length(), buf, buf_len);
}

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

  DBUG_ASSERT(!lex->create_info.tmp_table());

  bool found_temp_table= false;
  for (TABLE_LIST* table= first_table; table; table= table->next_global)
  {
    if (thd->find_temporary_table(table->db, table->table_name))
    {
      found_temp_table= true;
      break;
    }
  }

  if (found_temp_table)
  {
    buff.append("DROP TABLE ");
    if (lex->check_exists)
      buff.append("IF EXISTS ");

    for (TABLE_LIST* table= first_table; table; table= table->next_global)
    {
      if (!thd->find_temporary_table(table->db, table->table_name))
      {
        append_identifier(thd, &buff, table->db, strlen(table->db));
        buff.append(".");
        append_identifier(thd, &buff, table->table_name,
                          strlen(table->table_name));
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


/* Forward declarations. */
static int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len);
static int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len);

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
    /*
      If mariadb master has replicated a CTAS, we should not replicate the create table
      part separately as TOI, but to replicate both create table and following inserts
      as one write set.
      Howver, if CTAS creates empty table, we should replicate the create table alone
      as TOI. We have to do relay log event lookup to see if row events follow the
      create table event.
    */
    if (thd->slave_thread && !(thd->rgi_slave->gtid_ev_flags2 & Gtid_log_event::FL_STANDALONE))
    {
      /* this is CTAS, either empty or populated table */
      ulonglong event_size = 0;
      enum Log_event_type ev_type= wsrep_peak_event(thd->rgi_slave, &event_size);
      switch (ev_type)
      {
      case QUERY_EVENT:
        /* CTAS with empty table, we replicate create table as TOI */
        break;

      case TABLE_MAP_EVENT:
        WSREP_DEBUG("replicating CTAS of empty table as TOI");
        // fall through
      case WRITE_ROWS_EVENT:
        /* CTAS with populated table, we replicate later at commit time */
        WSREP_DEBUG("skipping create table of CTAS replication");
        return false;

      default:
        WSREP_WARN("unexpected async replication event: %d", ev_type);
      }
      return true;
    }
    /* no next async replication event */
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

    DBUG_ASSERT(first_table);

    if (thd->find_temporary_table(first_table))
    {
      return false;
    }
    return true;

  case SQLCOM_DROP_TRIGGER:
    DBUG_ASSERT(table_list);
    if (thd->find_temporary_table(table_list))
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
        if (!thd->find_temporary_table(table->db, table->table_name))
        {
          return true;
        }
      }
    }
    return !(table || table_list);
  }
}

/*
  returns: 
   0: statement was replicated as TOI
   1: TOI replication was skipped
  -1: TOI replication failed 
 */
static int wsrep_TOI_begin(THD *thd, char *db_, char *table_,
                           const TABLE_LIST* table_list,
                           Alter_info* alter_info)
{
  wsrep_status_t ret(WSREP_WARNING);
  uchar* buf(0);
  size_t buf_len(0);
  int buf_err;
  int rc= 0;

  if (wsrep_can_run_in_toi(thd, db_, table_, table_list) == false)
  {
    WSREP_DEBUG("No TOI for %s", WSREP_QUERY(thd));
    return 1;
  }

  WSREP_DEBUG("TO BEGIN: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_exec_mode, wsrep_thd_query(thd));

  switch (thd->lex->sql_command)
  {
  case SQLCOM_CREATE_VIEW:
    buf_err= create_view_query(thd, &buf, &buf_len);
    break;
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_CREATE_SPFUNCTION:
    buf_err= wsrep_create_sp(thd, &buf, &buf_len);
    break;
  case SQLCOM_CREATE_TRIGGER:
    buf_err= wsrep_create_trigger_query(thd, &buf, &buf_len);
    break;
  case SQLCOM_CREATE_EVENT:
    buf_err= wsrep_create_event_query(thd, &buf, &buf_len);
    break;
  case SQLCOM_ALTER_EVENT:
    buf_err= wsrep_alter_event_query(thd, &buf, &buf_len);
    break;
  case SQLCOM_DROP_TABLE:
    buf_err= wsrep_drop_table_query(thd, &buf, &buf_len);
    break;
  case SQLCOM_KILL:
    WSREP_DEBUG("KILL as TOI: %s", thd->query());
    buf_err= wsrep_to_buf_helper(thd, thd->query(), thd->query_length(),
                                 &buf, &buf_len);
    break;
  case SQLCOM_CREATE_ROLE:
    if (sp_process_definer(thd))
    {
      WSREP_WARN("Failed to set CREATE ROLE definer for TOI.");
    }
    /* fallthrough */
  default:
    buf_err= wsrep_to_buf_helper(thd, thd->query(), thd->query_length(),
                                 &buf, &buf_len);
    break;
  }

  wsrep_key_arr_t key_arr= {0, 0};
  struct wsrep_buf buff = { buf, buf_len };
  if (!buf_err                                                                  &&
      !wsrep_prepare_keys_for_isolation(thd, db_, table_,
                                        table_list, alter_info, &key_arr)       &&
      key_arr.keys_len > 0                                                      &&
      WSREP_OK == (ret = wsrep->to_execute_start(wsrep, thd->thread_id,
						 key_arr.keys, key_arr.keys_len,
						 &buff, 1,
						 &thd->wsrep_trx_meta)))
  {
    thd->wsrep_exec_mode= TOTAL_ORDER;
    wsrep_to_isolation++;
    wsrep_keys_free(&key_arr);
    WSREP_DEBUG("TO BEGIN: %lld, %d",(long long)wsrep_thd_trx_seqno(thd),
		thd->wsrep_exec_mode);
  }
  else if (key_arr.keys_len > 0) {
    /* jump to error handler in mysql_execute_command() */
    WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. Check wsrep "
               "connection state and retry the query.",
               ret,
               (thd->db ? thd->db : "(null)"),
               (thd->query()) ? thd->query() : "void");
    my_message(ER_LOCK_DEADLOCK, "WSREP replication failed. Check "
               "your wsrep connection state and retry the query.", MYF(0));
    wsrep_keys_free(&key_arr);
    rc= -1;
  }
  else {
    /* non replicated DDL, affecting temporary tables only */
    WSREP_DEBUG("TO isolation skipped for: %d, sql: %s."
		"Only temporary tables affected.",
		ret, (thd->query()) ? thd->query() : "void");
    rc= 1;
  }
  if (buf) my_free(buf);
  return rc;
}

static void wsrep_TOI_end(THD *thd) {
  wsrep_status_t ret;
  wsrep_to_isolation--;

  WSREP_DEBUG("TO END: %lld, %d: %s", (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_exec_mode, wsrep_thd_query(thd));

  wsrep_set_SE_checkpoint(thd->wsrep_trx_meta.gtid.uuid,
                          thd->wsrep_trx_meta.gtid.seqno);
  WSREP_DEBUG("TO END: %lld, update seqno",
              (long long)wsrep_thd_trx_seqno(thd));

  if (WSREP_OK == (ret = wsrep->to_execute_end(wsrep, thd->thread_id))) {
    WSREP_DEBUG("TO END: %lld", (long long)wsrep_thd_trx_seqno(thd));
  }
  else {
    WSREP_WARN("TO isolation end failed for: %d, schema: %s, sql: %s",
               ret,
               (thd->db ? thd->db : "(null)"),
               (thd->query()) ? thd->query() : "void");
  }
}

static int wsrep_RSU_begin(THD *thd, char *db_, char *table_)
{
  wsrep_status_t ret(WSREP_WARNING);
  WSREP_DEBUG("RSU BEGIN: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
               thd->wsrep_exec_mode, thd->query() );

  ret = wsrep->desync(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("RSU desync failed %d for schema: %s, query: %s",
               ret, (thd->db ? thd->db : "(null)"), thd->query());
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
               (thd->db ? thd->db : "(null)"),
               thd->query());
    mysql_mutex_lock(&LOCK_wsrep_replaying);
    wsrep_replaying--;
    mysql_mutex_unlock(&LOCK_wsrep_replaying);

    ret = wsrep->resync(wsrep);
    if (ret != WSREP_OK)
    {
      WSREP_WARN("resync failed %d for schema: %s, query: %s",
                 ret, (thd->db ? thd->db : "(null)"), thd->query());
    }

    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return(-1);
  }

  wsrep_seqno_t seqno = wsrep->pause(wsrep);
  if (seqno == WSREP_SEQNO_UNDEFINED)
  {
    WSREP_WARN("pause failed %lld for schema: %s, query: %s", (long long)seqno,
               (thd->db ? thd->db : "(null)"),
               thd->query());
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
               (thd->db ? thd->db : "(null)"),
               thd->query());
  }

  ret = wsrep->resync(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("resync failed %d for schema: %s, query: %s", ret,
               (thd->db ? thd->db : "(null)"), thd->query());
    return;
  }

  thd->variables.wsrep_on = 1;
}

int wsrep_to_isolation_begin(THD *thd, char *db_, char *table_,
                             const TABLE_LIST* table_list,
                             Alter_info* alter_info)
{
  int ret= 0;

  /*
    No isolation for applier or replaying threads.
   */
  if (thd->wsrep_exec_mode == REPL_RECV)
    return 0;

  mysql_mutex_lock(&thd->LOCK_thd_data);

  if (thd->wsrep_conflict_state == MUST_ABORT)
  {
    WSREP_INFO("thread: %lld  schema: %s  query: %s has been aborted due to multi-master conflict",
               (longlong) thd->thread_id,
               (thd->db ? thd->db : "(null)"),
               thd->query());
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return WSREP_TRX_FAIL;
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE);
  DBUG_ASSERT(thd->wsrep_trx_meta.gtid.seqno == WSREP_SEQNO_UNDEFINED);

  if (thd->global_read_lock.can_acquire_protection())
  {
    WSREP_DEBUG("Aborting TOI: Global Read-Lock (FTWRL) in place: %s %lld",
                thd->query(), (longlong) thd->thread_id);
    return -1;
  }

  if (wsrep_debug && thd->mdl_context.has_locks())
  {
    WSREP_DEBUG("thread holds MDL locks at TI begin: %s %lld",
                thd->query(), (longlong) thd->thread_id);
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
      ret= wsrep_TOI_begin(thd, db_, table_, table_list, alter_info);
      break;
    case WSREP_OSU_RSU:
      ret= wsrep_RSU_begin(thd, db_, table_);
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
    case WSREP_OSU_TOI: wsrep_TOI_end(thd); break;
    case WSREP_OSU_RSU: wsrep_RSU_end(thd); break;
    default:
      WSREP_WARN("Unsupported wsrep OSU method at isolation end: %lu",
                 thd->variables.wsrep_OSU_method);
      break;
    }
    wsrep_cleanup_transaction(thd);
  }
}

#define WSREP_MDL_LOG(severity, msg, schema, schema_len, req, gra)             \
    WSREP_##severity(                                                          \
      "%s\n"                                                                   \
      "schema:  %.*s\n"                                                        \
      "request: (%lld \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)\n"      \
      "granted: (%lld \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)",       \
      msg, schema_len, schema,                                                 \
      (longlong) req->thread_id, (long long)wsrep_thd_trx_seqno(req),                     \
      req->wsrep_exec_mode, req->wsrep_query_state, req->wsrep_conflict_state, \
      req->get_command(), req->lex->sql_command, req->query(),                 \
      (longlong) gra->thread_id, (long long)wsrep_thd_trx_seqno(gra),                     \
      gra->wsrep_exec_mode, gra->wsrep_query_state, gra->wsrep_conflict_state, \
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

  mysql_mutex_lock(&request_thd->LOCK_thd_data);

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
  if (request_thd->wsrep_exec_mode == TOTAL_ORDER ||
      request_thd->wsrep_exec_mode == REPL_RECV)
  {
    mysql_mutex_unlock(&request_thd->LOCK_thd_data);
    WSREP_MDL_LOG(DEBUG, "MDL conflict ", schema, schema_len,
                  request_thd, granted_thd);
    ticket->wsrep_report(wsrep_debug);

    mysql_mutex_lock(&granted_thd->LOCK_thd_data);
    if (granted_thd->wsrep_exec_mode == TOTAL_ORDER ||
        granted_thd->wsrep_exec_mode == REPL_RECV)
    {
      WSREP_MDL_LOG(INFO, "MDL BF-BF conflict", schema, schema_len,
                    request_thd, granted_thd);
      ticket->wsrep_report(true);
      mysql_mutex_unlock(&granted_thd->LOCK_thd_data);
      ret= true;
    }
    else if (granted_thd->lex->sql_command == SQLCOM_FLUSH ||
             granted_thd->mdl_context.has_explicit_locks())
    {
      WSREP_DEBUG("BF thread waiting for FLUSH");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_thd_data);
      ret= false;
    }
    else
    {
      /* Print some debug information. */
      if (wsrep_debug)
      {
        if (request_thd->lex->sql_command == SQLCOM_DROP_TABLE)
        {
          WSREP_DEBUG("DROP caused BF abort, conf %d", granted_thd->wsrep_conflict_state);
        }
        else if (granted_thd->wsrep_query_state == QUERY_COMMITTING)
        {
          WSREP_DEBUG("MDL granted, but committing thd abort scheduled");
        }
        else
        {
          WSREP_MDL_LOG(DEBUG, "MDL conflict-> BF abort", schema, schema_len,
                        request_thd, granted_thd);
        }
        ticket->wsrep_report(true);
      }

      /* This will call wsrep_abort_transaction so we should hold
      THD::LOCK_thd_data to protect victim from concurrent usage
      or disconnect or delete. */
      if (request_thd->wsrep_exec_mode == REPL_RECV)
	DEBUG_SYNC(request_thd, "wsrep_after_granted_lock");

      wsrep_abort_thd((void *) request_thd, (void *) granted_thd, true);
      ret= false;
    }
  }
  else
  {
    mysql_mutex_unlock(&request_thd->LOCK_thd_data);
  }

  return ret;
}


pthread_handler_t start_wsrep_THD(void *arg)
{
  THD *thd;
  wsrep_thread_args* args= (wsrep_thread_args*)arg;
  wsrep_thd_processor_fun processor= args->processor;

  if (my_thread_init() || (!(thd= new THD(next_thread_id(), true))))
  {
    goto error;
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

  switch (args->thread_type) {
    case WSREP_APPLIER_THREAD:
      wsrep_running_applier_threads++;
      break;
    case WSREP_ROLLBACKER_THREAD:
      wsrep_running_rollbacker_threads++;
      break;
    default:
      WSREP_ERROR("Incorrect wsrep thread type: %d", args->thread_type);
      break;
  }

  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  processor(thd);

  close_connection(thd, 0);

  mysql_mutex_lock(&LOCK_thread_count);
  DBUG_ASSERT(wsrep_running_threads > 0);
  wsrep_running_threads--;

  switch (args->thread_type) {
    case WSREP_APPLIER_THREAD:
      DBUG_ASSERT(wsrep_running_applier_threads > 0);
      wsrep_running_applier_threads--;
      break;
    case WSREP_ROLLBACKER_THREAD:
      DBUG_ASSERT(wsrep_running_rollbacker_threads > 0);
      wsrep_running_rollbacker_threads--;
      break;
    default:
      WSREP_ERROR("Incorrect wsrep thread type: %d", args->thread_type);
      break;
  }

  my_free(args);

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

  if (thread_handling > SCHEDULER_ONE_THREAD_PER_CONNECTION)
  {
    mysql_mutex_lock(&LOCK_thread_count);
    thd->unlink();
    mysql_mutex_unlock(&LOCK_thread_count);
    delete thd;
  }
  my_thread_end();
  return(NULL);

error:
  WSREP_ERROR("Failed to create/initialize system thread");

  my_free(args);

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
  mysql_mutex_lock(&thd->LOCK_thd_data);
  if (thd->wsrep_query_state== QUERY_COMMITTING)
  {
    WSREP_DEBUG("aborting replicated trx: %llu", (ulonglong)(thd->real_id));

    (void)wsrep_abort_thd(thd, thd, TRUE);
    ret_code= true;
  }
  else
    mysql_mutex_unlock(&thd->LOCK_thd_data);
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

  mysql_mutex_lock(&thd->LOCK_thd_data);
  ret=  (thd->wsrep_conflict_state == REPLAYING) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  return ret;
}


static inline bool is_committing_connection(THD *thd)
{
  bool ret;

  mysql_mutex_lock(&thd->LOCK_thd_data);
  ret=  (thd->wsrep_query_state == QUERY_COMMITTING) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  return ret;
}


static bool have_client_connections()
{
  THD *tmp;

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    DBUG_PRINT("quit",("Informing thread %lld that it's time to die",
                       (longlong) tmp->thread_id));
    if (is_client_connection(tmp) && tmp->killed == KILL_CONNECTION)
    {
      WSREP_DEBUG("Informing thread %lld that it's time to die",
                  (longlong)tmp->thread_id);
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
      mysql_mutex_unlock(&LOCK_thread_count);
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


void wsrep_close_client_connections(my_bool wait_to_end, THD *except_caller_thd)
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
    WSREP_DEBUG("Informing thread %lld that it's time to die",
                (longlong)tmp->thread_id);
    /* We skip slave threads & scheduler on this first loop through. */
    if (!is_client_connection(tmp))
      continue;

    if (tmp == except_caller_thd)
    {
      DBUG_ASSERT(is_client_connection(tmp));
      continue;
    }

    if (is_replaying_connection(tmp))
    {
      tmp->set_killed(KILL_CONNECTION);
      continue;
    }

    /* replicated transactions must be skipped and aborted
    with wsrep_abort_thd. */
    if (abort_replicated(tmp))
      continue;

    WSREP_DEBUG("closing connection %lld", (longlong) tmp->thread_id);

    /*
      instead of wsrep_close_thread() we do now  soft kill by
      THD::awake(). Here also victim needs to be protected from
      concurrent usage or disconnect or delete.
    */
    mysql_mutex_lock(&tmp->LOCK_thd_data);

    tmp->awake(KILL_CONNECTION);

    mysql_mutex_unlock(&tmp->LOCK_thd_data);

  }
  mysql_mutex_unlock(&LOCK_thread_count);

  if (thread_count)
    sleep(2);                               // Give threads time to die

  mysql_mutex_lock(&LOCK_thread_count);
  /*
    Force remaining threads to die by closing the connection to the client
  */

  I_List_iterator<THD> it2(threads);
  while ((tmp=it2++))
  {
    if (is_client_connection(tmp) &&
        !abort_replicated(tmp)    &&
        !is_replaying_connection(tmp) &&
        tmp != except_caller_thd)
    {
      WSREP_INFO("killing local connection: %lld", (longlong) tmp->thread_id);
      close_connection(tmp,0);
    }
  }

  DBUG_PRINT("quit",("Waiting for threads to die (count=%u)",thread_count));
  WSREP_DEBUG("waiting for client connections to close: %u", thread_count);

  while (wait_to_end && have_client_connections())
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
  while (wsrep_running_threads > 1)
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


static int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;
  sp_head *sp = thd->lex->sphead;
  sql_mode_t saved_mode= thd->variables.sql_mode;
  String retstr(64);
  retstr.set_charset(system_charset_info);

  log_query.set_charset(system_charset_info);

  if (sp->m_type == TYPE_ENUM_FUNCTION)
  {
    sp_returns_type(thd, retstr, sp);
  }

  if (!show_create_sp(thd, &log_query,
                     sp->m_type,
                     (sp->m_explicit_name ? sp->m_db.str : NULL),
                     (sp->m_explicit_name ? sp->m_db.length : 0),
                     sp->m_name.str, sp->m_name.length,
                     sp->m_params.str, sp->m_params.length,
                     retstr.c_ptr(), retstr.length(),
                     sp->m_body.str, sp->m_body.length,
                     sp->m_chistics, &(thd->lex->definer->user),
                     &(thd->lex->definer->host),
                     saved_mode))
  {
    WSREP_WARN("SP create string failed: schema: %s, query: %s",
               (thd->db ? thd->db : "(null)"), thd->query());
    return 1;
  }

  return wsrep_to_buf_helper(thd, log_query.ptr(), log_query.length(), buf, buf_len);
}


extern int wsrep_on(THD *thd)
{
  return (int)(WSREP(thd));
}


extern "C" bool wsrep_thd_is_wsrep_on(THD *thd)
{
  return thd->variables.wsrep_on;
}


bool wsrep_consistency_check(THD *thd)
{
  return thd->wsrep_consistency_check == CONSISTENCY_CHECK_RUNNING;
}


extern "C" void wsrep_thd_set_exec_mode(THD *thd, enum wsrep_exec_mode mode)
{
  thd->wsrep_exec_mode= mode;
}


extern "C" void wsrep_thd_set_query_state(
	THD *thd, enum wsrep_query_state state)
{
  /* async slave thread should never flag IDLE state, as it may
     give rollbacker thread chance to interfere and rollback async slave
     transaction.
     in fact, async slave thread is never idle as it reads complete
     transactions from relay log and applies them, as a whole.
     BF abort happens voluntarily by async slave thread.
  */
  if (thd->slave_thread && state == QUERY_IDLE) {
    WSREP_DEBUG("Skipping IDLE state change for slave SQL");
    return;
  }
  thd->wsrep_query_state= state;
}


void wsrep_thd_set_conflict_state(THD *thd, enum wsrep_conflict_state state)
{
  mysql_mutex_assert_owner(&thd->LOCK_thd_data);
  thd->wsrep_conflict_state= state;
}


enum wsrep_exec_mode wsrep_thd_exec_mode(THD *thd)
{
  return thd->wsrep_exec_mode;
}


const char *wsrep_thd_exec_mode_str(THD *thd)
{
  return 
    (!thd) ? "void" :
    (thd->wsrep_exec_mode == LOCAL_STATE)  ? "local"         :
    (thd->wsrep_exec_mode == REPL_RECV)    ? "applier"       :
    (thd->wsrep_exec_mode == TOTAL_ORDER)  ? "total order"   : 
    (thd->wsrep_exec_mode == LOCAL_COMMIT) ? "local commit"  : "void";
}


enum wsrep_query_state wsrep_thd_query_state(THD *thd)
{
  return thd->wsrep_query_state;
}


const char *wsrep_thd_query_state_str(THD *thd)
{
  return 
    (!thd) ? "void" : 
    (thd->wsrep_query_state == QUERY_IDLE)        ? "idle"          :
    (thd->wsrep_query_state == QUERY_EXEC)        ? "executing"     :
    (thd->wsrep_query_state == QUERY_COMMITTING)  ? "committing"    :
    (thd->wsrep_query_state == QUERY_EXITING)     ? "exiting"       : 
    (thd->wsrep_query_state == QUERY_ROLLINGBACK) ? "rolling back"  : "void";
}


enum wsrep_conflict_state wsrep_thd_get_conflict_state(THD *thd)
{
  return thd->wsrep_conflict_state;
}


const char *wsrep_thd_conflict_state_str(THD *thd)
{
  return 
    (!thd) ? "void" :
    (thd->wsrep_conflict_state == NO_CONFLICT)      ? "no conflict"  :
    (thd->wsrep_conflict_state == MUST_ABORT)       ? "must abort"   :
    (thd->wsrep_conflict_state == ABORTING)         ? "aborting"     :
    (thd->wsrep_conflict_state == MUST_REPLAY)      ? "must replay"  : 
    (thd->wsrep_conflict_state == REPLAYING)        ? "replaying"    : 
    (thd->wsrep_conflict_state == RETRY_AUTOCOMMIT) ? "retrying"     : 
    (thd->wsrep_conflict_state == CERT_FAILURE)     ? "cert failure" : "void";
}


wsrep_ws_handle_t* wsrep_thd_ws_handle(THD *thd)
{
  return &thd->wsrep_ws_handle;
}


void wsrep_thd_LOCK(THD *thd)
{
  mysql_mutex_lock(&thd->LOCK_thd_data);
}


void wsrep_thd_UNLOCK(THD *thd)
{
  mysql_mutex_unlock(&thd->LOCK_thd_data);
}


extern "C" time_t wsrep_thd_query_start(THD *thd) 
{
  return thd->query_start();
}


extern "C" uint32 wsrep_thd_wsrep_rand(THD *thd) 
{
  return thd->wsrep_rand;
}

longlong wsrep_thd_trx_seqno(THD *thd)
{
  return (thd) ? thd->wsrep_trx_meta.gtid.seqno : WSREP_SEQNO_UNDEFINED;
}


extern "C" query_id_t wsrep_thd_query_id(THD *thd) 
{
  return thd->query_id;
}


const char *wsrep_thd_query(THD *thd)
{
  if (thd)
  {
    switch(thd->lex->sql_command)
    {
    case SQLCOM_CREATE_USER:
      return "CREATE USER";
    case SQLCOM_GRANT:
      return "GRANT";
    case SQLCOM_REVOKE:
      return "REVOKE";
    case SQLCOM_SET_OPTION:
      if (thd->lex->definer)
	return "SET PASSWORD";
      /* fallthrough */
    default:
      if (thd->query())
        return thd->query();
    }
  }
  return "NULL";
}


extern "C" query_id_t wsrep_thd_wsrep_last_query_id(THD *thd) 
{
  return thd->wsrep_last_query_id;
}


extern "C" void wsrep_thd_set_wsrep_last_query_id(THD *thd, query_id_t id) 
{
  thd->wsrep_last_query_id= id;
}


extern "C" void wsrep_thd_awake(THD *thd, my_bool signal)
{
  if (signal)
  {
    /* Here we should hold THD::LOCK_thd_data to
    protect from concurrent usage. */
    mysql_mutex_assert_owner(&thd->LOCK_thd_data);
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


extern "C" bool wsrep_thd_ignore_table(THD *thd)
{
  return thd->wsrep_ignore_table;
}


extern int
wsrep_trx_order_before(THD *thd1, THD *thd2)
{
  const longlong trx1_seqno= wsrep_thd_trx_seqno(thd1);
  const longlong trx2_seqno= wsrep_thd_trx_seqno(thd2);
  WSREP_DEBUG("BF conflict, order: %lld %lld\n",
              trx1_seqno, trx2_seqno);

  if (trx1_seqno == WSREP_SEQNO_UNDEFINED ||
      trx2_seqno == WSREP_SEQNO_UNDEFINED)
    return 1; /* trx is not yet replicated */
  else if (trx1_seqno < trx2_seqno)
    return 1;

  return 0;
}


int wsrep_trx_is_aborting(THD *thd_ptr)
{
	if (thd_ptr) {
		if ((((THD *)thd_ptr)->wsrep_conflict_state == MUST_ABORT) ||
		    (((THD *)thd_ptr)->wsrep_conflict_state == ABORTING)) {
		  return 1;
		}
	}
	return 0;
}


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
    /* this is straight CREATE TABLE LIKE... eith no tmp tables */
    WSREP_TO_ISOLATION_BEGIN(table->db, table->table_name, NULL);
  }
  else
  {
    /* Non-MERGE tables ignore this call. */
    if (src_table->table->file->extra(HA_EXTRA_ADD_CHILDREN_LIST))
      return (true);

    char buf[2048];
    String query(buf, sizeof(buf), system_charset_info);
    query.length(0);  // Have to zero it since constructor doesn't

    int result __attribute__((unused))=
      show_create_table(thd, src_table, &query, NULL, WITH_DB_NAME);
    WSREP_DEBUG("TMP TABLE: %s ret_code %d", query.ptr(), result);

    thd->wsrep_TOI_pre_query=     query.ptr();
    thd->wsrep_TOI_pre_query_len= query.length();

    WSREP_TO_ISOLATION_BEGIN(table->db, table->table_name, NULL);

    thd->wsrep_TOI_pre_query=      NULL;
    thd->wsrep_TOI_pre_query_len= 0;

    /* Non-MERGE tables ignore this call. */
    src_table->table->file->extra(HA_EXTRA_DETACH_CHILDREN);
  }

  return(false);

wsrep_error_label:
  thd->wsrep_TOI_pre_query= NULL;
  return (true);
}


static int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len)
{
  LEX *lex= thd->lex;
  String stmt_query;

  LEX_STRING definer_user;
  LEX_STRING definer_host;

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

  const LEX_STRING command[3]=
      {{ C_STRING_WITH_LEN("CREATE ") },
       { C_STRING_WITH_LEN("ALTER ") },
       { C_STRING_WITH_LEN("CREATE OR REPLACE ") }};
  stmt_query.append(command[thd->lex->create_view_mode].str,
                    command[thd->lex->create_view_mode].length);

  append_definer(thd, &stmt_query, &definer_user, &definer_host);

  LEX_STRING stmt_definition;
  uint not_used;
  stmt_definition.str= (char*) thd->lex->stmt_definition_begin;
  stmt_definition.length= thd->lex->stmt_definition_end
    - thd->lex->stmt_definition_begin;
  trim_whitespace(thd->charset(), &stmt_definition, &not_used);

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

void wsrep_lock_rollback()
{
  mysql_mutex_lock(&LOCK_wsrep_rollback);
}

void wsrep_unlock_rollback()
{
  mysql_cond_signal(&COND_wsrep_rollback);
  mysql_mutex_unlock(&LOCK_wsrep_rollback);
}

my_bool wsrep_aborting_thd_contains(THD *thd)
{
  wsrep_aborting_thd_t abortees = wsrep_aborting_thd;
  while (abortees)
  {
    if (abortees->aborting_thd == thd)
      return true;
    abortees = abortees->next;
  }
  return false;
}

void wsrep_aborting_thd_enqueue(THD *thd)
{
  wsrep_aborting_thd_t aborting = (wsrep_aborting_thd_t)
          my_malloc(sizeof(struct wsrep_aborting_thd), MYF(0));
  aborting->aborting_thd  = thd;
  aborting->next          = wsrep_aborting_thd;
  wsrep_aborting_thd      = aborting;
}

bool wsrep_node_is_donor()
{
  return (WSREP_ON) ? (wsrep_config_state->get_status() == 2) : false;
}

bool wsrep_node_is_synced()
{
  return (WSREP_ON) ? (wsrep_config_state->get_status() == 4) : false;
}
