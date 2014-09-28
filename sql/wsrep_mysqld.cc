/* Copyright 2008-2014 Codership Oy <http://www.codership.com>

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

#include <mysqld.h>
#include <sql_class.h>
#include <sql_parse.h>
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
#include <cstdio>
#include <cstdlib>
#include "log_event.h"
#include <slave.h>

wsrep_t *wsrep                  = NULL;
my_bool wsrep_emulate_bin_log   = FALSE; // activating parts of binlog interface
#ifdef GTID_SUPPORT
/* Sidno in global_sid_map corresponding to group uuid */
rpl_sidno wsrep_sidno= -1;
#endif /* GTID_SUPPORT */
my_bool wsrep_preordered_opt= FALSE;

/*
 * Begin configuration options and their default values
 */

extern my_bool plugins_are_initialized;
extern uint kill_cached_threads;
extern mysql_cond_t COND_thread_cache;

const char* wsrep_data_home_dir = NULL;
const char* wsrep_dbug_option   = "";

long    wsrep_slave_threads            = 1; // # of slave action appliers wanted
int     wsrep_slave_count_change       = 0; // # of appliers to stop or start
my_bool wsrep_debug                    = 0; // enable debug level logging
my_bool wsrep_convert_LOCK_to_trx      = 1; // convert locking sessions to trx
ulong   wsrep_retry_autocommit         = 5; // retry aborted autocommit trx
my_bool wsrep_auto_increment_control   = 1; // control auto increment variables
my_bool wsrep_drupal_282555_workaround = 1; // retry autoinc insert after dupkey
my_bool wsrep_incremental_data_collection = 0; // incremental data collection
ulong   wsrep_max_ws_size              = 1073741824UL;//max ws (RBR buffer) size
ulong   wsrep_max_ws_rows              = 65536; // max number of rows in ws
int     wsrep_to_isolation             = 0; // # of active TO isolation threads
my_bool wsrep_certify_nonPK            = 1; // certify, even when no primary key
long    wsrep_max_protocol_version     = 3; // maximum protocol version to use
ulong   wsrep_forced_binlog_format     = BINLOG_FORMAT_UNSPEC;
my_bool wsrep_recovery                 = 0; // recovery
my_bool wsrep_replicate_myisam         = 0; // enable myisam replication
my_bool wsrep_log_conflicts            = 0;
ulong   wsrep_mysql_replication_bundle = 0;
my_bool wsrep_desync                   = 0; // desynchronize the node from the
                                            // cluster
my_bool wsrep_load_data_splitting      = 1; // commit load data every 10K intervals
my_bool wsrep_restart_slave            = 0; // should mysql slave thread be
                                            // restarted, if node joins back
my_bool wsrep_restart_slave_activated  = 0; // node has dropped, and slave
                                            // restart will be needed
my_bool wsrep_slave_UK_checks          = 0; // slave thread does UK checks
my_bool wsrep_slave_FK_checks          = 0; // slave thread does FK checks
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
int wsrep_replaying= 0;
ulong  wsrep_running_threads = 0; // # of currently running wsrep threads
ulong  my_bind_addr;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_wsrep_rollback, key_LOCK_wsrep_thd,
  key_LOCK_wsrep_replaying, key_LOCK_wsrep_ready, key_LOCK_wsrep_sst,
  key_LOCK_wsrep_sst_thread, key_LOCK_wsrep_sst_init,
  key_LOCK_wsrep_slave_threads, key_LOCK_wsrep_desync;

PSI_cond_key key_COND_wsrep_rollback, key_COND_wsrep_thd,
  key_COND_wsrep_replaying, key_COND_wsrep_ready, key_COND_wsrep_sst,
  key_COND_wsrep_sst_init, key_COND_wsrep_sst_thread;

static PSI_mutex_info wsrep_mutexes[]=
{
  { &key_LOCK_wsrep_ready, "LOCK_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_LOCK_wsrep_sst_init, "LOCK_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_rollback, "LOCK_wsrep_rollback", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_thd, "THD::LOCK_wsrep_thd", 0},
  { &key_LOCK_wsrep_replaying, "LOCK_wsrep_replaying", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_slave_threads, "LOCK_wsrep_slave_threads", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_desync, "LOCK_wsrep_desync", PSI_FLAG_GLOBAL}
};

static PSI_cond_info wsrep_conds[]=
{
  { &key_COND_wsrep_ready, "COND_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst, "COND_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_init, "COND_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_COND_wsrep_rollback, "COND_wsrep_rollback", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_thd, "THD::COND_wsrep_thd", 0},
  { &key_COND_wsrep_replaying, "COND_wsrep_replaying", PSI_FLAG_GLOBAL}
};
#else
#define mysql_mutex_register(X,Y,Z)
#define mysql_cond_register(X,Y,Z)
#endif

my_bool wsrep_inited                   = 0; // initialized ?

static const wsrep_uuid_t cluster_uuid = WSREP_UUID_UNDEFINED;
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
wsp::node_status local_status;
long             wsrep_protocol_version = 3;

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

static my_bool set_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= reinterpret_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);
  if (hton->set_checkpoint)
  {
    const wsrep_uuid_t* uuid(wsrep_xid_uuid(xid));
    char uuid_str[40] = {0, };
    wsrep_uuid_print(uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Set WSREPXid for InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(xid));
    hton->set_checkpoint(hton, xid);
  }
  return FALSE;
}

void wsrep_set_SE_checkpoint(XID* xid)
{
  plugin_foreach(NULL, set_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN, xid);
}

static my_bool get_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= reinterpret_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);
  if (hton->get_checkpoint)
  {
    hton->get_checkpoint(hton, xid);
    const wsrep_uuid_t* uuid(wsrep_xid_uuid(xid));
    char uuid_str[40] = {0, };
    wsrep_uuid_print(uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Read WSREPXid from InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(xid));
  }
  return FALSE;
}

void wsrep_get_SE_checkpoint(XID* xid)
{
  plugin_foreach(NULL, get_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN, xid);
}

#ifdef GTID_SUPPORT
void wsrep_init_sidno(const wsrep_uuid_t& uuid)
{
  /* generate new Sid map entry from inverted uuid */
  rpl_sid sid;
  wsrep_uuid_t ltid_uuid;
  for (size_t i= 0; i < sizeof(ltid_uuid.data); ++i)
  {
      ltid_uuid.data[i] = ~local_uuid.data[i];
  }
  sid.copy_from(ltid_uuid.data);
  global_sid_lock->wrlock();
  wsrep_sidno= global_sid_map->add_sid(sid);
  WSREP_INFO("inited wsrep sidno %d", wsrep_sidno);
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

  WSREP_INFO("New cluster view: global state: %s:%lld, view# %lld: %s, "
             "number of nodes: %ld, my index: %ld, protocol version %d",
             wsrep_cluster_state_uuid, (long long)view->state_id.seqno,
             (long long)wsrep_cluster_conf_id, wsrep_cluster_status,
             wsrep_cluster_size, wsrep_local_index, view->proto_ver);

  /* Proceed further only if view is PRIMARY */
  if (WSREP_VIEW_PRIMARY != view->status) {
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
      // version change
      if (view->proto_ver != wsrep_protocol_version)
      {
          my_bool wsrep_ready_saved= wsrep_ready;
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
     * with SST */
    WSREP_DEBUG("[debug]: closing client connections for PRIM");
    wsrep_close_client_connections(TRUE);

    ssize_t const req_len= wsrep_sst_prepare (sst_req);

    if (req_len < 0)
    {
      WSREP_ERROR("SST preparation failed: %zd (%s)", -req_len,
                  strerror(-req_len));
      new_status= WSREP_MEMBER_UNDEFINED;
    }
    else
    {
      assert(sst_req != NULL);
      *sst_req_len= req_len;
      new_status= WSREP_MEMBER_JOINER;
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
      XID xid;
      wsrep_xid_init(&xid, &local_uuid, local_seqno);
      wsrep_set_SE_checkpoint(&xid);
      new_status= WSREP_MEMBER_JOINED;
#ifdef GTID_SUPPORT
      wsrep_init_sidno(local_uuid);
#endif /* GTID_SUPPORT */
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
  local_status.set(new_status, view);

  return WSREP_CB_SUCCESS;
}

void wsrep_ready_set (my_bool x)
{
  WSREP_DEBUG("Setting wsrep_ready to %d", x);
  if (mysql_mutex_lock (&LOCK_wsrep_ready)) abort();
  if (wsrep_ready != x)
  {
    wsrep_ready= x;
    mysql_cond_signal (&COND_wsrep_ready);
  }
  mysql_mutex_unlock (&LOCK_wsrep_ready);
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
  bool signal_main= false;
  if (mysql_mutex_lock (&LOCK_wsrep_ready)) abort();
  if (!wsrep_ready)
  {
    wsrep_ready= TRUE;
    mysql_cond_signal (&COND_wsrep_ready);
    signal_main= true;

  }
  local_status.set(WSREP_MEMBER_SYNCED);
  mysql_mutex_unlock (&LOCK_wsrep_ready);

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
    if ((rcode = start_slave_threads(1 /* need mutex */,
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
  XID xid;
  memset(&xid, 0, sizeof(xid));
  xid.formatID= -1;
  wsrep_get_SE_checkpoint(&xid);

  if (xid.formatID == -1)
  {
    WSREP_INFO("Read nil XID from storage engines, skipping position init");
    return;
  }
  else if (!wsrep_is_wsrep_xid(&xid))
  {
    WSREP_WARN("Read non-wsrep XID from storage engines, skipping position init");
    return;
  }

  const wsrep_uuid_t* uuid= wsrep_xid_uuid(&xid);
  const wsrep_seqno_t seqno= wsrep_xid_seqno(&xid);

  char uuid_str[40] = {0, };
  wsrep_uuid_print(uuid, uuid_str, sizeof(uuid_str));
  WSREP_INFO("Initial position: %s:%lld", uuid_str, (long long)seqno);


  if (!memcmp(&local_uuid, &WSREP_UUID_UNDEFINED, sizeof(local_uuid)) &&
      local_seqno == WSREP_SEQNO_UNDEFINED)
  {
    // Initial state
    local_uuid= *uuid;
    local_seqno= seqno;
  }
  else if (memcmp(&local_uuid, uuid, sizeof(local_uuid)) ||
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

  wsrep_causal_reads_update(&global_system_variables);

  mysql_mutex_register("sql", wsrep_mutexes, array_elements(wsrep_mutexes));
  mysql_cond_register("sql", wsrep_conds, array_elements(wsrep_conds));

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
      (void) wsrep_init();
      return rcode;
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

  if (!wsrep_data_home_dir || strlen(wsrep_data_home_dir) == 0)
    wsrep_data_home_dir = mysql_real_data_home;

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

  char inc_addr[512]= { 0, };
  size_t const inc_addr_max= sizeof (inc_addr);
  if ((!wsrep_node_incoming_address ||
       !strcmp (wsrep_node_incoming_address, WSREP_NODE_INCOMING_AUTO)))
  {
    unsigned int my_bind_ip= INADDR_ANY; // default if not set
    if (my_bind_addr_str && strlen(my_bind_addr_str))
    {
      my_bind_ip= wsrep_check_ip(my_bind_addr_str);
    }

    if (INADDR_ANY != my_bind_ip)
    {
      if (INADDR_NONE != my_bind_ip && INADDR_LOOPBACK != my_bind_ip)
      {
        snprintf(inc_addr, inc_addr_max, "%s:%u",
                 my_bind_addr_str, (int)mysqld_port);
      } // else leave inc_addr an empty string - mysqld is not listening for
        // client connections on network interfaces.
    }
    else // mysqld binds to 0.0.0.0, take IP from wsrep_node_address if possible
    {
      size_t const node_addr_len= strlen(node_addr);
      if (node_addr_len > 0)
      {
        const char* const colon= strrchr(node_addr, ':');
        if (strchr(node_addr, ':') == colon) // 1 or 0 ':'
        {
          size_t const ip_len= colon ? colon - node_addr : node_addr_len;
          if (ip_len + 7 /* :55555\0 */ < inc_addr_max)
          {
            memcpy (inc_addr, node_addr, ip_len);
            snprintf(inc_addr + ip_len, inc_addr_max - ip_len, ":%u",
                     (int)mysqld_port);
          }
          else
          {
            WSREP_WARN("Guessing address for incoming client connections: "
                       "address too long.");
            inc_addr[0]= '\0';
          }
        }
        else
        {
          WSREP_WARN("Guessing address for incoming client connections: "
                     "too many colons :) .");
          inc_addr[0]= '\0';
        }
      }

      if (!strlen(inc_addr))
      {
          WSREP_WARN("Guessing address for incoming client connections failed. "
                     "Try setting wsrep_node_incoming_address explicitly.");
      }
    }
  }
  else if (!strchr(wsrep_node_incoming_address, ':')) // no port included
  {
    if ((int)inc_addr_max <=
        snprintf(inc_addr, inc_addr_max, "%s:%u",
                 wsrep_node_incoming_address,(int)mysqld_port))
    {
      WSREP_WARN("Guessing address for incoming client connections: "
                 "address too long.");
      inc_addr[0]= '\0';
    }
  }
  else
  {
    size_t const need = strlen (wsrep_node_incoming_address);
    if (need >= inc_addr_max) {
      WSREP_WARN("wsrep_node_incoming_address too long: %zu", need);
      inc_addr[0]= '\0';
    }
    else {
      memcpy (inc_addr, wsrep_node_incoming_address, need);
    }
  }

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

void wsrep_init_startup (bool first)
{
  if (wsrep_init()) unireg_abort(1);

  wsrep_thr_lock_init(
     (wsrep_thd_is_brute_force_fun)wsrep_thd_is_BF,
     (wsrep_abort_thd_fun)wsrep_abort_thd,
     wsrep_debug, wsrep_convert_LOCK_to_trx,
     (wsrep_on_fun)wsrep_on);

  /* Skip replication start if no cluster address */
  if (!wsrep_cluster_address || strlen(wsrep_cluster_address) == 0) return;

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
}

void wsrep_recover()
{
  if (!memcmp(&local_uuid, &WSREP_UUID_UNDEFINED, sizeof(wsrep_uuid_t)) &&
      local_seqno == -2)
  {
    char uuid_str[40];
    wsrep_uuid_print(&local_uuid, uuid_str, sizeof(uuid_str));
    WSREP_INFO("Position %s:%lld given at startup, skipping position recovery",
               uuid_str, (long long)local_seqno);
    return;
  }
  XID xid;
  memset(&xid, 0, sizeof(xid));
  xid.formatID= -1;
  wsrep_get_SE_checkpoint(&xid);
  char uuid_str[40];
  wsrep_uuid_print(wsrep_xid_uuid(&xid), uuid_str, sizeof(uuid_str));
  WSREP_INFO("Recovered position: %s:%lld", uuid_str,
             (long long)wsrep_xid_seqno(&xid));
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

/* This one is set to true when --wsrep-new-cluster is found in the command
 * line arguments */
static my_bool wsrep_new_cluster= FALSE;
#define WSREP_NEW_CLUSTER "--wsrep-new-cluster"
/* Finds and hides --wsrep-new-cluster from the arguments list
 * by moving it to the end of the list and decrementing argument count */
void wsrep_filter_new_cluster (int* argc, char* argv[])
{
  int i;
  for (i= *argc - 1; i > 0; i--)
  {
    /* make a copy of the argument to convert possible underscores to hyphens.
     * the copy need not to be longer than WSREP_NEW_CLUSTER option */
    char arg[sizeof(WSREP_NEW_CLUSTER) + 1]= { 0, };
    strncpy(arg, argv[i], sizeof(arg) - 1);
    char* underscore(arg);
    while (NULL != (underscore= strchr(underscore, '_'))) *underscore= '-';

    if (!strcmp(arg, WSREP_NEW_CLUSTER))
    {
      wsrep_new_cluster= TRUE;
      *argc -= 1;
      /* preserve the order of remaining arguments AND
       * preserve the original argument pointers - just in case */
      char* wnc= argv[i];
      memmove(&argv[i], &argv[i + 1], (*argc - i)*sizeof(argv[i]));
      argv[*argc]= wnc; /* this will be invisible to the rest of the program */
    }
  }
}

bool wsrep_start_replication()
{
  wsrep_status_t rcode;

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

  if (!wsrep_cluster_address || strlen(wsrep_cluster_address)== 0)
  {
    // if provider is non-trivial, but no address is specified, wait for address
    wsrep_ready_set(FALSE);
    return true;
  }

  bool const bootstrap(TRUE == wsrep_new_cluster);
  wsrep_new_cluster= FALSE;

  WSREP_INFO("Start replication");

  if ((rcode = wsrep->connect(wsrep,
                              wsrep_cluster_name,
                              wsrep_cluster_address,
                              wsrep_sst_donor,
                              bootstrap)))
  {
    if (-ESOCKTNOSUPPORT == rcode)
    {
      DBUG_PRINT("wsrep",("unrecognized cluster address: '%s', rcode: %d",
                          wsrep_cluster_address, rcode));
      WSREP_ERROR("unrecognized cluster address: '%s', rcode: %d",
                  wsrep_cluster_address, rcode);
    }
    else
    {
      DBUG_PRINT("wsrep",("wsrep->connect() failed: %d", rcode));
      WSREP_ERROR("wsrep::connect() failed: %d", rcode);
    }

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

bool wsrep_sync_wait (THD* thd, uint mask)
{
  if ((thd->variables.wsrep_sync_wait & mask) &&
      thd->variables.wsrep_on &&
      !thd->in_active_multi_stmt_transaction() &&
      thd->wsrep_conflict_state != REPLAYING)
  {
    WSREP_DEBUG("wsrep_sync_wait: thd->variables.wsrep_sync_wait = %u, mask = %u",
                thd->variables.wsrep_sync_wait, mask);
    // This allows autocommit SELECTs and a first SELECT after SET AUTOCOMMIT=0
    // TODO: modify to check if thd has locked any rows.
    wsrep_gtid_t  gtid;
    wsrep_status_t ret= wsrep->causal_read (wsrep, &gtid);

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

/*
 * Helpers to deal with TOI key arrays
 */
typedef struct wsrep_key_arr
{
    wsrep_key_t* keys;
    size_t       keys_len;
} wsrep_key_arr_t;


static void wsrep_keys_free(wsrep_key_arr_t* key_arr)
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
        return false;
    }

    return true;
}

/* Prepare key list from db/table and table_list */
static bool wsrep_prepare_keys_for_isolation(THD*              thd,
                                             const char*       db,
                                             const char*       table,
                                             const TABLE_LIST* table_list,
                                             wsrep_key_arr_t*  ka)
{
    ka->keys= 0;
    ka->keys_len= 0;

    extern TABLE* find_temporary_table(THD*, const TABLE_LIST*);

    if (db || table)
    {
        TABLE_LIST tmp_table;
	MDL_request mdl_request;

        memset(&tmp_table, 0, sizeof(tmp_table));
        tmp_table.table_name= (char*)table;
        tmp_table.db= (char*)db;
	tmp_table.mdl_request.init(MDL_key::GLOBAL, (db) ? db :  "",
				   (table) ? table : "",
				   MDL_INTENTION_EXCLUSIVE, MDL_STATEMENT);

        if (!table || !find_temporary_table(thd, &tmp_table))
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
                WSREP_ERROR("Preparing keys for isolation failed");
                goto err;
            }
        }
    }

    for (const TABLE_LIST* table= table_list; table; table= table->next_global)
    {
        if (!find_temporary_table(thd, table))
        {
            wsrep_key_t* tmp;
            tmp= (wsrep_key_t*)my_realloc(
                ka->keys, (ka->keys_len + 1) * sizeof(wsrep_key_t), 
                 MYF(MY_ALLOW_ZERO_PTR));

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
            if (!wsrep_prepare_key_for_isolation(
                    table->db, table->table_name,
                    (wsrep_buf_t*)ka->keys[ka->keys_len - 1].key_parts,
                    &ka->keys[ka->keys_len - 1].key_parts_num))
            {
                WSREP_ERROR("Preparing keys for isolation failed");
                goto err;
            }
        }
    }
    return true;
err:
    wsrep_keys_free(ka);
    return false;
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
  if (open_cached_file(&tmp_io_cache, mysql_tmpdir, TEMP_PREFIX,
                       65536, MYF(MY_WME)))
    return 1;
  int ret(0);

#ifdef GTID_SUPPORT
  if (thd->variables.gtid_next.type == GTID_GROUP)
  {
      Gtid_log_event gtid_ev(thd, FALSE, &thd->variables.gtid_next);
      if (!gtid_ev.is_valid()) ret= 0;
      if (!ret && gtid_ev.write(&tmp_io_cache)) ret= 1;
  }
#endif /* GTID_SUPPORT */

  /* if there is prepare query, add event for it */
  if (!ret && thd->wsrep_TOI_pre_query)
  {
    Query_log_event ev(thd, thd->wsrep_TOI_pre_query,
		       thd->wsrep_TOI_pre_query_len,
		       FALSE, FALSE, FALSE, 0);
    if (ev.write(&tmp_io_cache)) ret= 1;
  }

  /* continue to append the actual query */
  Query_log_event ev(thd, query, query_len, FALSE, FALSE, FALSE, 0);
  if (!ret && ev.write(&tmp_io_cache)) ret= 1;
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

int wsrep_alter_event_query(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;

  if (wsrep_alter_query_string(thd, &log_query))
  {
    WSREP_WARN("events alter string failed: %s", thd->query());
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

    if (!lex->definer)
    {
      /*
        DEFINER-clause is missing; we have to create default definer in
        persistent arena to be PS/SP friendly.
        If this is an ALTER VIEW then the current user should be set as
        the definer.
      */

      if (!(lex->definer= create_default_definer(thd, false)))
      {
        WSREP_WARN("view default definer issue");
      }
    }

    views->algorithm    = lex->create_view_algorithm;
    views->definer.user = lex->definer->user;
    views->definer.host = lex->definer->host;
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

static int wsrep_TOI_begin(THD *thd, char *db_, char *table_,
                           const TABLE_LIST* table_list)
{
  wsrep_status_t ret(WSREP_WARNING);
  uchar* buf(0);
  size_t buf_len(0);
  int buf_err;

  WSREP_DEBUG("TO BEGIN: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_exec_mode, thd->query() );
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
  default:
    buf_err= wsrep_to_buf_helper(thd, thd->query(), thd->query_length(), &buf,
                                 &buf_len);
    break;
  }

  wsrep_key_arr_t key_arr= {0, 0};
  struct wsrep_buf buff = { buf, buf_len };
  if (!buf_err                                                    &&
      wsrep_prepare_keys_for_isolation(thd, db_, table_, table_list, &key_arr)&&
      WSREP_OK == (ret = wsrep->to_execute_start(wsrep, thd->thread_id,
                                                 key_arr.keys, key_arr.keys_len,
                                                 &buff, 1,
                                                 &thd->wsrep_trx_meta)))
  {
    thd->wsrep_exec_mode= TOTAL_ORDER;
    wsrep_to_isolation++;
    my_free(buf);
    wsrep_keys_free(&key_arr);
    WSREP_DEBUG("TO BEGIN: %lld, %d",(long long)wsrep_thd_trx_seqno(thd),
                thd->wsrep_exec_mode);
  }
  else {
    /* jump to error handler in mysql_execute_command() */
    WSREP_WARN("TO isolation failed for: %d, sql: %s. Check wsrep "
               "connection state and retry the query.",
               ret, (thd->query()) ? thd->query() : "void");
    my_error(ER_LOCK_DEADLOCK, MYF(0), "WSREP replication failed. Check "
            "your wsrep connection state and retry the query.");
    if (buf) my_free(buf);
    wsrep_keys_free(&key_arr);
    return -1;
  }
  return 0;
}

static void wsrep_TOI_end(THD *thd) {
  wsrep_status_t ret;
  wsrep_to_isolation--;

  WSREP_DEBUG("TO END: %lld, %d : %s", (long long)wsrep_thd_trx_seqno(thd),
              thd->wsrep_exec_mode, (thd->query()) ? thd->query() : "void");
  
  XID xid;
  wsrep_xid_init(&xid, &thd->wsrep_trx_meta.gtid.uuid,
                 thd->wsrep_trx_meta.gtid.seqno);
  wsrep_set_SE_checkpoint(&xid);
  WSREP_DEBUG("TO END: %lld, update seqno",
              (long long)wsrep_thd_trx_seqno(thd));
  
  if (WSREP_OK == (ret = wsrep->to_execute_end(wsrep, thd->thread_id))) {
    WSREP_DEBUG("TO END: %lld", (long long)wsrep_thd_trx_seqno(thd));
  }
  else {
    WSREP_WARN("TO isolation end failed for: %d, sql: %s",
               ret, (thd->query()) ? thd->query() : "void");
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
    WSREP_WARN("RSU desync failed %d for %s", ret, thd->query());
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return(ret);
  }
  mysql_mutex_lock(&LOCK_wsrep_replaying);
  wsrep_replaying++;
  mysql_mutex_unlock(&LOCK_wsrep_replaying);

  if (wsrep_wait_committing_connections_close(5000))
  {
    /* no can do, bail out from DDL */
    WSREP_WARN("RSU failed due to pending transactions, %s", thd->query());
    mysql_mutex_lock(&LOCK_wsrep_replaying);
    wsrep_replaying--;
    mysql_mutex_unlock(&LOCK_wsrep_replaying);

    ret = wsrep->resync(wsrep);
    if (ret != WSREP_OK)
    {
      WSREP_WARN("resync failed %d for %s", ret, thd->query());
    }
    my_error(ER_LOCK_DEADLOCK, MYF(0));
    return(1);
  }

  wsrep_seqno_t seqno = wsrep->pause(wsrep);
  if (seqno == WSREP_SEQNO_UNDEFINED)
  {
    WSREP_WARN("pause failed %lld for %s", (long long)seqno, thd->query());
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
    WSREP_WARN("resume failed %d for %s", ret, thd->query());
  }
  ret = wsrep->resync(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("resync failed %d for %s", ret, thd->query());
    return;
  }
  thd->variables.wsrep_on = 1;
}

int wsrep_to_isolation_begin(THD *thd, char *db_, char *table_,
                             const TABLE_LIST* table_list)
{
  int ret= 0;

  /*
    No isolation for applier or replaying threads.
   */
  if (thd->wsrep_exec_mode == REPL_RECV)
    return 0;

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);

  if (thd->wsrep_conflict_state == MUST_ABORT)
  {
    WSREP_INFO("thread: %lu, %s has been aborted due to multi-master conflict",
               thd->thread_id, thd->query());
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
    return WSREP_TRX_FAIL;
  }
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  DBUG_ASSERT(thd->wsrep_exec_mode == LOCAL_STATE);
  DBUG_ASSERT(thd->wsrep_trx_meta.gtid.seqno == WSREP_SEQNO_UNDEFINED);

  if (thd->global_read_lock.can_acquire_protection())
  {
    WSREP_DEBUG("Aborting TOI: Global Read-Lock (FTWRL) in place: %s %lu",
                thd->query(), thd->thread_id);
    return -1;
  }

  if (wsrep_debug && thd->mdl_context.has_locks())
  {
    WSREP_DEBUG("thread holds MDL locks at TI begin: %s %lu",
                thd->query(), thd->thread_id);
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
    switch (wsrep_OSU_method_options) {
    case WSREP_OSU_TOI: ret =  wsrep_TOI_begin(thd, db_, table_,
                                               table_list); break;
    case WSREP_OSU_RSU: ret =  wsrep_RSU_begin(thd, db_, table_); break;
    }
    if (!ret)
    {
      thd->wsrep_exec_mode= TOTAL_ORDER;
    }
  }
  return ret;
}

void wsrep_to_isolation_end(THD *thd)
{
  if (thd->wsrep_exec_mode == TOTAL_ORDER)
  {
    switch(wsrep_OSU_method_options)
    {
    case WSREP_OSU_TOI: wsrep_TOI_end(thd); break;
    case WSREP_OSU_RSU: wsrep_RSU_end(thd); break;
    }
    wsrep_cleanup_transaction(thd);
  }
}

#define WSREP_MDL_LOG(severity, msg, req, gra)	                               \
    WSREP_##severity(                                                          \
      "%s\n"                                                                   \
      "request: (%lu \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)\n"      \
      "granted: (%lu \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)",       \
      msg,                                                                     \
      req->thread_id, (long long)wsrep_thd_trx_seqno(req),                     \
      req->wsrep_exec_mode, req->wsrep_query_state, req->wsrep_conflict_state, \
      req->get_command(), req->lex->sql_command, req->query(),		       \
      gra->thread_id, (long long)wsrep_thd_trx_seqno(gra),                     \
      gra->wsrep_exec_mode, gra->wsrep_query_state, gra->wsrep_conflict_state, \
      gra->get_command(), gra->lex->sql_command, gra->query());

bool
wsrep_grant_mdl_exception(MDL_context *requestor_ctx,
                          MDL_ticket *ticket
) {
  if (!WSREP_ON) return FALSE;

  THD *request_thd  = requestor_ctx->get_thd();
  THD *granted_thd  = ticket->get_ctx()->get_thd();
  bool ret          = FALSE;

  mysql_mutex_lock(&request_thd->LOCK_wsrep_thd);
  if (request_thd->wsrep_exec_mode == TOTAL_ORDER ||
      request_thd->wsrep_exec_mode == REPL_RECV)
  {
    mysql_mutex_unlock(&request_thd->LOCK_wsrep_thd);
    WSREP_MDL_LOG(DEBUG, "MDL conflict ", request_thd, granted_thd);
    ticket->wsrep_report(wsrep_debug);

    mysql_mutex_lock(&granted_thd->LOCK_wsrep_thd);
    if (granted_thd->wsrep_exec_mode == TOTAL_ORDER ||
        granted_thd->wsrep_exec_mode == REPL_RECV)
    {
      WSREP_MDL_LOG(INFO, "MDL BF-BF conflict", request_thd, granted_thd);
      ticket->wsrep_report(true);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      ret = TRUE;
    }
    else if (granted_thd->lex->sql_command == SQLCOM_FLUSH)
    {
      WSREP_DEBUG("mdl granted over FLUSH BF");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      ret = TRUE;
    }
    else if (request_thd->lex->sql_command == SQLCOM_DROP_TABLE)
    {
      WSREP_DEBUG("DROP caused BF abort");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      ret = FALSE;
    }
    else if (granted_thd->wsrep_query_state == QUERY_COMMITTING)
    {
      WSREP_DEBUG("mdl granted, but commiting thd abort scheduled");
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      ret = FALSE;
    }
    else
    {
      WSREP_MDL_LOG(DEBUG, "MDL conflict-> BF abort", request_thd, granted_thd);
      ticket->wsrep_report(wsrep_debug);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      ret = FALSE;
    }
  }
  else
  {
    mysql_mutex_unlock(&request_thd->LOCK_wsrep_thd);
  }
  return ret;
}


pthread_handler_t start_wsrep_THD(void *arg)
{
  THD *thd;
  rpl_sql_thread_info sql_info(NULL);
  wsrep_thd_processor_fun processor= (wsrep_thd_processor_fun)arg;

  if (my_thread_init())
  {
    WSREP_ERROR("Could not initialize thread");
    return(NULL);
  }

  if (!(thd= new THD(true)))
  {
    return(NULL);
  }
  mysql_mutex_lock(&LOCK_thread_count);
  thd->thread_id=thread_id++;

  thd->real_id=pthread_self(); // Keep purify happy
  thread_count++;
  thread_created++;
  threads.append(thd);

  my_net_init(&thd->net,(st_vio*) 0, MYF(0));

  DBUG_PRINT("wsrep",(("creating thread %lld"), (long long)thd->thread_id));
  thd->prior_thr_create_utime= thd->start_utime= microsecond_interval_timer();
  (void) mysql_mutex_unlock(&LOCK_thread_count);

  /* from bootstrap()... */
  thd->bootstrap=1;
  thd->max_client_packet_length= thd->net.max_packet;
  thd->security_ctx->master_access= ~(ulong)0;
  thd->system_thread_info.rpl_sql_info= &sql_info;

  /* from handle_one_connection... */
  pthread_detach_this_thread();

  mysql_thread_set_psi_id(thd->thread_id);
  thd->thr_create_utime=  microsecond_interval_timer();
  if (MYSQL_CALLBACK_ELSE(thread_scheduler, init_new_connection_thread, (), 0))
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    statistic_increment(aborted_connects,&LOCK_status);
    MYSQL_CALLBACK(thread_scheduler, end_thread, (thd, 0));

    return(NULL);
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

    return(NULL);
  }

  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;
  thd->security_ctx->skip_grants();

  /* handle_one_connection() again... */
  //thd->version= refresh_version;
  thd->proc_info= 0;
  thd->set_command(COM_SLEEP);
  thd->set_time();
  thd->init_for_queries();

  mysql_mutex_lock(&LOCK_thread_count);
  wsrep_running_threads++;
  mysql_cond_broadcast(&COND_thread_count);
  mysql_mutex_unlock(&LOCK_thread_count);

  processor(thd);

  close_connection(thd, 0);

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

  my_thread_end();
  if (thread_handling > SCHEDULER_ONE_THREAD_PER_CONNECTION)
  {
    mysql_mutex_lock(&LOCK_thread_count);
    delete thd;
    thread_count--;
    mysql_mutex_unlock(&LOCK_thread_count);
  }
  return(NULL);
}


/**/
static bool abort_replicated(THD *thd)
{
  bool ret_code= false;
  if (thd->wsrep_query_state== QUERY_COMMITTING)
  {
    WSREP_DEBUG("aborting replicated trx: %lu", thd->real_id);

    (void)wsrep_abort_thd(thd, thd, TRUE);
    ret_code= true;
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
  ret=  (thd->wsrep_conflict_state == REPLAYING) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  return ret;
}


static inline bool is_committing_connection(THD *thd)
{
  bool ret;

  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
  ret=  (thd->wsrep_query_state == QUERY_COMMITTING) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);

  return ret;
}


static bool have_client_connections()
{
  THD *tmp;

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    DBUG_PRINT("quit",("Informing thread %ld that it's time to die",
                        tmp->thread_id));
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
  thd->killed= KILL_CONNECTION;
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


void wsrep_close_client_connections(my_bool wait_to_end)
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
    DBUG_PRINT("quit",("Informing thread %ld that it's time to die",
                        tmp->thread_id));
    /* We skip slave threads & scheduler on this first loop through. */
    if (!is_client_connection(tmp))
      continue;

    if (is_replaying_connection(tmp))
    {
      tmp->killed= KILL_CONNECTION;
      continue;
    }

    /* replicated transactions must be skipped */
    if (abort_replicated(tmp))
      continue;

    WSREP_DEBUG("closing connection %ld", tmp->thread_id);
    wsrep_close_thread(tmp);
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
#ifndef __bsdi__				// Bug in BSDI kernel
    if (is_client_connection(tmp) &&
        !abort_replicated(tmp)    &&
	!is_replaying_connection(tmp))
    {
      WSREP_INFO("killing local connection: %ld",tmp->thread_id);
      close_connection(tmp,0);
    }
#endif
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
  WSREP_DEBUG("closing applier %ld", thd->thread_id);
  wsrep_close_thread(thd);
}


void wsrep_close_threads(THD *thd)
{
  THD *tmp;
  mysql_mutex_lock(&LOCK_thread_count); // For unlink from list

  I_List_iterator<THD> it(threads);
  while ((tmp=it++))
  {
    DBUG_PRINT("quit",("Informing thread %ld that it's time to die",
                       tmp->thread_id));
    /* We skip slave threads & scheduler on this first loop through. */
    if (tmp->wsrep_applier && tmp != thd)
    {
      WSREP_DEBUG("closing wsrep thread %ld", tmp->thread_id);
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


int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;
  sp_head *sp = thd->lex->sphead;
  ulong saved_mode= thd->variables.sql_mode;
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
      WSREP_WARN("SP create string failed: %s", thd->query());
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
  thd->wsrep_query_state= state;
}


void wsrep_thd_set_conflict_state(THD *thd, enum wsrep_conflict_state state)
{
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
  mysql_mutex_lock(&thd->LOCK_wsrep_thd);
}


void wsrep_thd_UNLOCK(THD *thd)
{
  mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
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


char *wsrep_thd_query(THD *thd)
{
  return (thd) ? thd->query() : NULL;
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
    mysql_mutex_lock(&thd->LOCK_thd_data);
    thd->awake(KILL_QUERY);
    mysql_mutex_unlock(&thd->LOCK_thd_data);
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


extern int
wsrep_trx_order_before(THD *thd1, THD *thd2)
{
    if (wsrep_thd_trx_seqno(thd1) < wsrep_thd_trx_seqno(thd2)) {
        WSREP_DEBUG("BF conflict, order: %lld %lld\n",
                    (long long)wsrep_thd_trx_seqno(thd1),
                    (long long)wsrep_thd_trx_seqno(thd2));
        return 1;
    }
    WSREP_DEBUG("waiting for BF, trx order: %lld %lld\n",
                (long long)wsrep_thd_trx_seqno(thd1),
                (long long)wsrep_thd_trx_seqno(thd2));
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
  TABLE *tmp_table;
  bool is_tmp_table= FALSE;

  for (tmp_table= thd->temporary_tables; tmp_table; tmp_table=tmp_table->next)
  {
      if (!strcmp(src_table->db, tmp_table->s->db.str)     &&
          !strcmp(src_table->table_name, tmp_table->s->table_name.str))
      {
        is_tmp_table= TRUE;
        break;
      }
  }
  if (create_info->options & HA_LEX_CREATE_TMP_TABLE)
  {

    /* CREATE TEMPORARY TABLE LIKE must be skipped from replication */
    WSREP_DEBUG("CREATE TEMPORARY TABLE LIKE... skipped replication\n %s", 
                thd->query());
  }
  else if (!is_tmp_table)
  {
    /* this is straight CREATE TABLE LIKE... eith no tmp tables */
    WSREP_TO_ISOLATION_BEGIN(table->db, table->table_name, NULL);
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
    tbl.table= tmp_table;
    char buf[2048];
    String query(buf, sizeof(buf), system_charset_info);
    query.length(0);  // Have to zero it since constructor doesn't

    (void)  store_create_info(thd, &tbl, &query, NULL, TRUE, FALSE);
    WSREP_DEBUG("TMP TABLE: %s", query.ptr());

    thd->wsrep_TOI_pre_query=     query.ptr();
    thd->wsrep_TOI_pre_query_len= query.length();

    WSREP_TO_ISOLATION_BEGIN(table->db, table->table_name, NULL);

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

    definer_user= lex->definer->user;
    definer_host= lex->definer->host;
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

  LEX_STRING stmt_definition;
  stmt_definition.str= (char*) thd->lex->stmt_definition_begin;
  stmt_definition.length= thd->lex->stmt_definition_end
    - thd->lex->stmt_definition_begin;
  trim_whitespace(thd->charset(), & stmt_definition);

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
