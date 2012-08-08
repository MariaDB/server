/* Copyright 2008 Codership Oy <http://www.codership.com>

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
#include "wsrep_priv.h"
#include <cstdio>
#include <cstdlib>
#include "log_event.h"

wsrep_t *wsrep                  = NULL;
my_bool wsrep_emulate_bin_log   = FALSE; // activating parts of binlog interface

/*
 * Begin configuration options and their default values
 */

const char* wsrep_data_home_dir = NULL;

#define WSREP_NODE_INCOMING_AUTO "AUTO"
const char* wsrep_node_incoming_address = WSREP_NODE_INCOMING_AUTO;
const char* wsrep_dbug_option   = "";

long    wsrep_slave_threads            = 1; // # of slave action appliers wanted
my_bool wsrep_debug                    = 0; // enable debug level logging
my_bool wsrep_convert_LOCK_to_trx      = 1; // convert locking sessions to trx
ulong   wsrep_retry_autocommit         = 5; // retry aborted autocommit trx
my_bool wsrep_auto_increment_control   = 1; // control auto increment variables
my_bool wsrep_drupal_282555_workaround = 1; // retry autoinc insert after dupkey
my_bool wsrep_incremental_data_collection = 0; // incremental data collection
long long wsrep_max_ws_size            = 1073741824LL; //max ws (RBR buffer) size
long    wsrep_max_ws_rows              = 65536; // max number of rows in ws
int     wsrep_to_isolation             = 0; // # of active TO isolation threads
my_bool wsrep_certify_nonPK            = 1; // certify, even when no primary key
long    wsrep_max_protocol_version     = 2; // maximum protocol version to use
ulong   wsrep_forced_binlog_format     = BINLOG_FORMAT_UNSPEC;
my_bool wsrep_recovery                 = 0; // recovery
my_bool wsrep_replicate_myisam         = 0; // enable myisam replication

/*
 * End configuration options
 */

static wsrep_uuid_t cluster_uuid = WSREP_UUID_UNDEFINED;
const wsrep_uuid_t* wsrep_cluster_uuid()
{
  return &cluster_uuid;
}
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
const char* wsrep_provider_name      = provider_name;
const char* wsrep_provider_version   = provider_version;
const char* wsrep_provider_vendor    = provider_vendor;
/* End wsrep status variables */


wsrep_uuid_t     local_uuid   = WSREP_UUID_UNDEFINED;
wsrep_seqno_t    local_seqno  = WSREP_SEQNO_UNDEFINED;
wsp::node_status local_status;
long             wsrep_protocol_version = 2;

// action execute callback
extern wsrep_status_t wsrep_apply_cb(void *ctx,
                                     const void* buf, size_t buf_len,
                                     wsrep_seqno_t global_seqno);

extern wsrep_status_t wsrep_commit_cb  (void *ctx,
                                        wsrep_seqno_t global_seqno,
                                        bool commit);

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

static void wsrep_log_states (wsrep_log_level_t level,
                              wsrep_uuid_t* group_uuid,
                              wsrep_seqno_t group_seqno,
                              wsrep_uuid_t* node_uuid,
                              wsrep_seqno_t node_seqno)
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
  if (hton->db_type == DB_TYPE_INNODB)
  {
    const wsrep_uuid_t* uuid(wsrep_xid_uuid(xid));
    char uuid_str[40] = {0, };
    wsrep_uuid_print(uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Set WSREPXid for InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(xid));
    hton->wsrep_set_checkpoint(hton, xid);
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
  if (hton->db_type == DB_TYPE_INNODB)
  {
    hton->wsrep_get_checkpoint(hton, xid);
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

static void wsrep_view_handler_cb (void* app_ctx,
                                   void* recv_ctx,
                                   const wsrep_view_info_t* view,
                                   const char* state,
                                   size_t state_len,
                                   void** sst_req,
                                   ssize_t* sst_req_len)
{
  wsrep_member_status_t new_status= local_status.get();

  if (memcmp(&cluster_uuid, &view->uuid, sizeof(wsrep_uuid_t)))
  {
    cluster_uuid= view->uuid;
    wsrep_uuid_print (&cluster_uuid, cluster_uuid_str,
                      sizeof(cluster_uuid_str));
  }

  wsrep_cluster_conf_id= view->view;
  wsrep_cluster_status= cluster_status_str[view->status];
  wsrep_cluster_size= view->memb_num;
  wsrep_local_index= view->my_idx;

  WSREP_INFO("New cluster view: global state: %s:%lld, view# %lld: %s, "
             "number of nodes: %ld, my index: %ld, protocol version %d",
             wsrep_cluster_state_uuid, (long long)view->seqno,
             (long long)wsrep_cluster_conf_id, wsrep_cluster_status,
             wsrep_cluster_size, wsrep_local_index, view->proto_ver);

  /* Proceed further only if view is PRIMARY */
  if (WSREP_VIEW_PRIMARY != view->status) {
    wsrep_ready= FALSE;
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
      // version change
      if (view->proto_ver != wsrep_protocol_version)
      {
          my_bool wsrep_ready_saved= wsrep_ready;
          wsrep_ready= FALSE;
          WSREP_INFO("closing client connections for "
                     "protocol change %ld -> %d",
                     wsrep_protocol_version, view->proto_ver);
          wsrep_close_client_connections(TRUE);
          wsrep_protocol_version= view->proto_ver;
          wsrep_ready= wsrep_ready_saved;
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
    wsrep_ready= FALSE;

    /* Close client connections to ensure that they don't interfere
     * with SST */
    WSREP_DEBUG("[debug]: closing client connections for PRIM");
    wsrep_close_client_connections(TRUE);

    *sst_req_len= wsrep_sst_prepare (sst_req);

    if (*sst_req_len < 0)
    {
      int err = *sst_req_len;
      WSREP_ERROR("SST preparation failed: %d (%s)", -err, strerror(-err));
      new_status= WSREP_MEMBER_UNDEFINED;
    }
    else
    {
      new_status= WSREP_MEMBER_JOINER;
    }
  }
  else
  {
    /*
     *  NOTE: Initialize wsrep_group_uuid here only if it wasn't initialized
     *  before - OR - it was reinitilized on startup (lp:992840)
     */
    if (!memcmp (&local_uuid, &WSREP_UUID_UNDEFINED, sizeof(wsrep_uuid_t)) ||
	0 == wsrep_cluster_conf_id)
    {
      if (wsrep_init_first())
      {
        wsrep_SE_init_grab();
        // Signal init thread to continue
        wsrep_sst_complete (&cluster_uuid, view->seqno, false);
        // and wait for SE initialization
        wsrep_SE_init_wait();
      }
      else
      {
        local_uuid=  cluster_uuid;
        local_seqno= view->seqno;
      }
      /* Init storage engine XIDs from first view */
      XID xid;
      wsrep_xid_init(&xid, &local_uuid, local_seqno);
      wsrep_set_SE_checkpoint(&xid);
      new_status= WSREP_MEMBER_JOINED;
    }
    else // just some sanity check
    {
      if (memcmp (&local_uuid, &cluster_uuid, sizeof (wsrep_uuid_t)))
      {
        WSREP_ERROR("Undetected state gap. Can't continue.");
        wsrep_log_states (WSREP_LOG_FATAL, &cluster_uuid, view->seqno,
                          &local_uuid, -1);
        abort();
      }
    }
  }

  if (wsrep_auto_increment_control)
  {
    global_system_variables.auto_increment_offset= view->my_idx + 1;
    global_system_variables.auto_increment_increment= view->memb_num;
  }

out:

  local_status.set(new_status, view);
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
  if (mysql_mutex_lock (&LOCK_wsrep_ready)) abort();
  if (!wsrep_ready)
  {
    wsrep_ready= TRUE;
    mysql_cond_signal (&COND_wsrep_ready);
  }
  local_status.set(WSREP_MEMBER_SYNCED);
  mysql_mutex_unlock (&LOCK_wsrep_ready);
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


int wsrep_init()
{
  int rcode= -1;

  wsrep_ready= FALSE;
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

  if (strlen(wsrep_provider)== 0 ||
      !strcmp(wsrep_provider, WSREP_NONE))
  {
    // enable normal operation in case no provider is specified
    wsrep_ready= TRUE;
    global_system_variables.wsrep_on = 0;
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

  struct wsrep_init_args wsrep_args;

  if (!wsrep_data_home_dir || strlen(wsrep_data_home_dir) == 0)
    wsrep_data_home_dir = mysql_real_data_home;

  if (strcmp (wsrep_provider, WSREP_NONE) &&
      (!wsrep_node_incoming_address ||
       !strcmp (wsrep_node_incoming_address, WSREP_NODE_INCOMING_AUTO))) {
    static char inc_addr[256];
    size_t inc_addr_max = sizeof (inc_addr);
    size_t ret = default_address (inc_addr, inc_addr_max);
    if (ret > 0 && ret < inc_addr_max) {
      wsrep_node_incoming_address = inc_addr;
    }
    else {
      wsrep_node_incoming_address = NULL;
    }
  }

  char node_addr[256] = {0, };
  if (!wsrep_node_address || !strcmp(wsrep_node_address, ""))
  {
    size_t node_addr_max= sizeof(node_addr);
    size_t ret= default_ip(node_addr, node_addr_max);
    if (!(ret > 0 && ret < node_addr_max))
    {
      WSREP_WARN("Failed to autoguess base node address");
      node_addr[0]= 0;
    }
  }
  else if (wsrep_node_address)
  {
    strncpy(node_addr, wsrep_node_address, sizeof(node_addr) - 1);
  }

  wsrep_args.data_dir        = wsrep_data_home_dir;
  wsrep_args.node_name       = (wsrep_node_name) ? wsrep_node_name : "";
  wsrep_args.node_address    = node_addr;
  wsrep_args.node_incoming   = wsrep_node_incoming_address;
  wsrep_args.options         = (wsrep_provider_options) ?
                                wsrep_provider_options : "";
  wsrep_args.proto_ver       = wsrep_max_protocol_version;

  wsrep_args.state_uuid      = &local_uuid;
  wsrep_args.state_seqno     = local_seqno;

  wsrep_args.logger_cb       = wsrep_log_cb;
  wsrep_args.view_handler_cb = wsrep_view_handler_cb;
  wsrep_args.apply_cb        = wsrep_apply_cb;
  wsrep_args.commit_cb       = wsrep_commit_cb;
  wsrep_args.sst_donate_cb   = wsrep_sst_donate_cb;
  wsrep_args.synced_cb       = wsrep_synced_cb;

  rcode = wsrep->init(wsrep, &wsrep_args);

  if (rcode)
  {
    DBUG_PRINT("wsrep",("wsrep::init() failed: %d", rcode));
    WSREP_ERROR("wsrep::init() failed: %d, must shutdown", rcode);
    free(wsrep);
    wsrep = NULL;
  }

  return rcode;
}

extern "C" int wsrep_on(void *);

void wsrep_init_startup (bool first)
{
  if (wsrep_init()) unireg_abort(1);

  wsrep_thr_lock_init(wsrep_thd_is_brute_force, wsrep_abort_thd,
                      wsrep_debug, wsrep_convert_LOCK_to_trx, wsrep_on);

  /* Skip replication start if no cluster address */
  if (!wsrep_cluster_address || strlen(wsrep_cluster_address) == 0) return;

  if (first) wsrep_sst_grab(); // do it so we can wait for SST below

  if (!wsrep_start_replication()) unireg_abort(1);

  wsrep_create_rollbacker();
  wsrep_create_appliers(1);

  if (first && !wsrep_sst_wait()) unireg_abort(1);// wait until SST is completed
}


void wsrep_deinit()
{
  wsrep_unload(wsrep);
  wsrep= 0;
  provider_name[0]=    '\0';
  provider_version[0]= '\0';
  provider_vendor[0]=  '\0';
}

void wsrep_recover()
{
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


bool wsrep_start_replication()
{
  wsrep_status_t rcode;

  /*
    if provider is trivial, don't even try to connect,
    but resume local node operation
  */
  if (strlen(wsrep_provider)== 0 ||
      !strcmp(wsrep_provider, WSREP_NONE))
  {
    // enable normal operation in case no provider is specified
    wsrep_ready = TRUE;
    return true;
  }

  if (!wsrep_cluster_address || strlen(wsrep_cluster_address)== 0)
  {
    // if provider is non-trivial, but no address is specified, wait for address
    wsrep_ready = FALSE;
    return true;
  }

  WSREP_INFO("Start replication");

  if ((rcode = wsrep->connect(wsrep,
                              wsrep_cluster_name,
                              wsrep_cluster_address,
                              wsrep_sst_donor)))
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

    uint64_t caps = wsrep->capabilities (wsrep);

    wsrep_incremental_data_collection =
        (caps & WSREP_CAP_WRITE_SET_INCREMENTS);

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

bool
wsrep_causal_wait (THD* thd)
{
  if (thd->variables.wsrep_causal_reads && thd->variables.wsrep_on &&
      !thd->in_active_multi_stmt_transaction() &&
      thd->wsrep_conflict_state != REPLAYING)
  {
    // This allows autocommit SELECTs and a first SELECT after SET AUTOCOMMIT=0
    // TODO: modify to check if thd has locked any rows.
    wsrep_seqno_t  seqno;
    wsrep_status_t ret= wsrep->causal_read (wsrep, &seqno);

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
        msg= "consistent reads by wsrep backend. "
             "Please unset wsrep_causal_reads variable.";
        err= ER_NOT_SUPPORTED_YET;
        break;
      default:
        msg= "Causal wait failed.";
        err= ER_ERROR_ON_READ;
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
        my_free((wsrep_key_part_t*)key_arr->keys[i].key_parts);
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
                                            wsrep_key_part_t* key,
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
    {
        *key_len= 0;
        if (db)
        {
            // sql_print_information("%s.%s", db, table);
            if (db)
            {
                key[*key_len].buf= db;
                key[*key_len].buf_len= strlen(db);
                ++(*key_len);
                if (table)
                {
                    key[*key_len].buf=     table;
                    key[*key_len].buf_len= strlen(table);
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
        bzero((char*) &tmp_table,sizeof(tmp_table));
        tmp_table.table_name= (char*)db;
        tmp_table.db= (char*)table;
        if (!table || !find_temporary_table(thd, &tmp_table))
        {
            if (!(ka->keys= (wsrep_key_t*)my_malloc(sizeof(wsrep_key_t), MYF(0))))
            {
                sql_print_error("Can't allocate memory for key_array");
                goto err;
            }
            ka->keys_len= 1;
            if (!(ka->keys[0].key_parts= (wsrep_key_part_t*)
                  my_malloc(sizeof(wsrep_key_part_t)*2, MYF(0))))
            {
                sql_print_error("Can't allocate memory for key_parts");
                goto err;
            }
            ka->keys[0].key_parts_len= 2;
            if (!wsrep_prepare_key_for_isolation(
                    db, table,
                    (wsrep_key_part_t*)ka->keys[0].key_parts,
                    &ka->keys[0].key_parts_len))
            {
                sql_print_error("Preparing keys for isolation failed");
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
                ka->keys, (ka->keys_len + 1) * sizeof(wsrep_key_t), MYF(0));
            if (!tmp)
            {
                sql_print_error("Can't allocate memory for key_array");
                goto err;
            }
            ka->keys= tmp;
            if (!(ka->keys[ka->keys_len].key_parts= (wsrep_key_part_t*)
                  my_malloc(sizeof(wsrep_key_part_t)*2, MYF(0))))
            {
                sql_print_error("Can't allocate memory for key_parts");
                goto err;
            }
            ka->keys[ka->keys_len].key_parts_len= 2;
            ++ka->keys_len;
            if (!wsrep_prepare_key_for_isolation(
                    table->db, table->table_name,
                    (wsrep_key_part_t*)ka->keys[ka->keys_len - 1].key_parts,
                    &ka->keys[ka->keys_len - 1].key_parts_len))
            {
                sql_print_error("Preparing keys for isolation failed");
                goto err;
            }
        }
    }
    return true;
err:
    wsrep_keys_free(ka);
    return false;
}



bool wsrep_prepare_key_for_innodb(const uchar* cache_key,
				  size_t cache_key_len,
                                  const uchar* row_id,
                                  size_t row_id_len,
                                  wsrep_key_part_t* key,
                                  size_t* key_len)
{
    if (*key_len < 3) return false;

    *key_len= 0;
    switch (wsrep_protocol_version)
    {
    case 0:
    {
        key[*key_len].buf     = cache_key;
        key[*key_len].buf_len = cache_key_len;
        ++(*key_len);
        break;
    }
    case 1:
    case 2:
    {
        key[*key_len].buf     = cache_key;
        key[*key_len].buf_len = strlen( (char*)cache_key );
        ++(*key_len);
        key[*key_len].buf     = cache_key + strlen( (char*)cache_key ) + 1;
        key[*key_len].buf_len = strlen( (char*)(key[*key_len].buf) );
        ++(*key_len);
        break;
    }
    default:
        return false;
    }

    key[*key_len].buf     = row_id;
    key[*key_len].buf_len = row_id_len;
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
    THD* thd, const char *query, uint query_len, uchar** buf, uint* buf_len)
{
  IO_CACHE tmp_io_cache;
  if (open_cached_file(&tmp_io_cache, mysql_tmpdir, TEMP_PREFIX,
                       65536, MYF(MY_WME)))
    return 1;
  Query_log_event ev(thd, query, query_len, FALSE, FALSE, FALSE, 0);
  int ret(0);
  if (ev.write(&tmp_io_cache)) ret= 1;
  if (!ret && wsrep_write_cache(&tmp_io_cache, buf, buf_len)) ret= 1;
  close_cached_file(&tmp_io_cache);
  return ret;
}

#include "sql_show.h"
static int
create_view_query(THD *thd, uchar** buf, uint* buf_len)
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

      if (!(lex->definer= create_default_definer(thd)))
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
  uint buf_len(0);
  int buf_err;

  WSREP_DEBUG("TO BEGIN: %lld, %d : %s", (long long)thd->wsrep_trx_seqno,
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
  default:
    buf_err= wsrep_to_buf_helper(thd, thd->query(), thd->query_length(), &buf, 
                                 &buf_len);
    break;
  }

  wsrep_key_arr_t key_arr= {0, 0};
  if (!buf_err                                                    &&
      wsrep_prepare_keys_for_isolation(thd, db_, table_, table_list, &key_arr)&&
      WSREP_OK == (ret = wsrep->to_execute_start(wsrep, thd->thread_id,
                                                 key_arr.keys, key_arr.keys_len,
                                                 buf, buf_len,
                                                 &thd->wsrep_trx_seqno)))
  {
    thd->wsrep_exec_mode= TOTAL_ORDER;
    wsrep_to_isolation++;
    if (buf) my_free(buf);
    wsrep_keys_free(&key_arr);
    WSREP_DEBUG("TO BEGIN: %lld, %d",(long long)thd->wsrep_trx_seqno,
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
  WSREP_DEBUG("TO END: %lld, %d : %s", (long long)thd->wsrep_trx_seqno,
              thd->wsrep_exec_mode, (thd->query()) ? thd->query() : "void")
    if (WSREP_OK == (ret = wsrep->to_execute_end(wsrep, thd->thread_id))) {
      WSREP_DEBUG("TO END: %lld", (long long)thd->wsrep_trx_seqno);
    }
    else {
      WSREP_WARN("TO isolation end failed for: %d, sql: %s",
                 ret, (thd->query()) ? thd->query() : "void");
    }
}

static int wsrep_RSU_begin(THD *thd, char *db_, char *table_) 
{
  wsrep_status_t ret(WSREP_WARNING);
  WSREP_DEBUG("RSU BEGIN: %lld, %d : %s", (long long)thd->wsrep_trx_seqno,
               thd->wsrep_exec_mode, thd->query() );

  ret = wsrep->desync(wsrep);
  if (ret != WSREP_OK)
  {
    WSREP_WARN("desync failed %d for %s", ret, thd->query());
    return(ret);
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
  WSREP_DEBUG("RSU END: %lld, %d : %s", (long long)thd->wsrep_trx_seqno,
               thd->wsrep_exec_mode, thd->query() );

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
  return;
}

int wsrep_to_isolation_begin(THD *thd, char *db_, char *table_,
                             const TABLE_LIST* table_list)
{
  int ret= 0;
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

void wsrep_to_isolation_end(THD *thd) {
  if (thd->wsrep_exec_mode==TOTAL_ORDER)
  {
    switch(wsrep_OSU_method_options)
    {
    case WSREP_OSU_TOI: return wsrep_TOI_end(thd);
    case WSREP_OSU_RSU: return wsrep_RSU_end(thd);
    }
  }
}

#define WSREP_MDL_LOG(severity, msg, req, gra)	                               \
    WSREP_##severity(                                                          \
      "%s\n"                                                                   \
      "request: (%lu \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)\n"      \
      "granted: (%lu \tseqno %lld \twsrep (%d, %d, %d) cmd %d %d \t%s)",       \
      msg,                                                                     \
      req->thread_id, (long long)req->wsrep_trx_seqno,                         \
      req->wsrep_exec_mode, req->wsrep_query_state, req->wsrep_conflict_state, \
      req->command, req->lex->sql_command, req->query(),                       \
      gra->thread_id, (long long)gra->wsrep_trx_seqno,                         \
      gra->wsrep_exec_mode, gra->wsrep_query_state, gra->wsrep_conflict_state, \
      gra->command, gra->lex->sql_command, gra->query());

bool
wsrep_grant_mdl_exception(MDL_context *requestor_ctx,
                          MDL_ticket *ticket
) {
  if (!WSREP_ON) return FALSE;

  THD *request_thd  = requestor_ctx->get_thd();
  THD *granted_thd  = ticket->get_ctx()->get_thd();

  mysql_mutex_lock(&request_thd->LOCK_wsrep_thd);
  if (request_thd->wsrep_exec_mode == TOTAL_ORDER ||
      request_thd->wsrep_exec_mode == REPL_RECV)
  {
    mysql_mutex_unlock(&request_thd->LOCK_wsrep_thd);
    WSREP_MDL_LOG(DEBUG, "MDL conflict ", request_thd, granted_thd);

    mysql_mutex_lock(&granted_thd->LOCK_wsrep_thd);
    if (granted_thd->wsrep_exec_mode == TOTAL_ORDER ||
        granted_thd->wsrep_exec_mode == REPL_RECV)
    {
      WSREP_MDL_LOG(INFO, "MDL BF-BF conflict", request_thd, granted_thd);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      return TRUE;
    }
    else if (granted_thd->lex->sql_command == SQLCOM_FLUSH)
    {
      WSREP_DEBUG("mdl granted over FLUSH BF");
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      return TRUE;
    } 
    else if (request_thd->lex->sql_command == SQLCOM_DROP_TABLE) 
    {
      WSREP_DEBUG("DROP caused BF abort");
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      return FALSE;
    } 
    else if (granted_thd->wsrep_query_state == QUERY_COMMITTING) 
    {
      WSREP_DEBUG("mdl granted, but commiting thd abort scheduled");
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      return  FALSE;
    }
    else 
    {
      WSREP_MDL_LOG(DEBUG, "MDL conflict-> BF abort", request_thd, granted_thd);
      mysql_mutex_unlock(&granted_thd->LOCK_wsrep_thd);
      wsrep_abort_thd((void*)request_thd, (void*)granted_thd, 1);
      return FALSE;
    }
  }
  else
  {
    mysql_mutex_unlock(&request_thd->LOCK_wsrep_thd);
  }
  return FALSE;
}
