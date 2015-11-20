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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "wsrep_var.h"

#include <mysqld.h>
#include <sql_class.h>
#include <set_var.h>
#include <sql_acl.h>
#include "wsrep_priv.h"
#include "wsrep_thd.h"
#include "wsrep_xid.h"
#include <my_dir.h>
#include <cstdio>
#include <cstdlib>

const  char* wsrep_provider         = 0;
const  char* wsrep_provider_options = 0;
const  char* wsrep_cluster_address  = 0;
const  char* wsrep_cluster_name     = 0;
const  char* wsrep_node_name        = 0;
const  char* wsrep_node_address     = 0;
const  char* wsrep_node_incoming_address = 0;
const  char* wsrep_start_position   = 0;

int wsrep_init_vars()
{
  wsrep_provider        = my_strdup(WSREP_NONE, MYF(MY_WME));
  wsrep_provider_options= my_strdup("", MYF(MY_WME));
  wsrep_cluster_address = my_strdup("", MYF(MY_WME));
  wsrep_cluster_name    = my_strdup(WSREP_CLUSTER_NAME, MYF(MY_WME));
  wsrep_node_name       = my_strdup("", MYF(MY_WME));
  wsrep_node_address    = my_strdup("", MYF(MY_WME));
  wsrep_node_incoming_address= my_strdup(WSREP_NODE_INCOMING_AUTO, MYF(MY_WME));
  wsrep_start_position  = my_strdup(WSREP_START_POSITION_ZERO, MYF(MY_WME));

  global_system_variables.binlog_format=BINLOG_FORMAT_ROW;
  return 0;
}

bool wsrep_on_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  if (var_type == OPT_GLOBAL) {
    // FIXME: this variable probably should be changed only per session
    thd->variables.wsrep_on = global_system_variables.wsrep_on;
  }
  return false;
}

bool wsrep_causal_reads_update (SV *sv)
{
  if (sv->wsrep_causal_reads) {
    sv->wsrep_sync_wait |= WSREP_SYNC_WAIT_BEFORE_READ;
  } else {
    sv->wsrep_sync_wait &= ~WSREP_SYNC_WAIT_BEFORE_READ;
  }

  return false;
}

bool wsrep_sync_wait_update (sys_var* self, THD* thd, enum_var_type var_type)
{
  if (var_type == OPT_GLOBAL)
    global_system_variables.wsrep_causal_reads =
      MY_TEST(global_system_variables.wsrep_sync_wait & WSREP_SYNC_WAIT_BEFORE_READ);
  else
    thd->variables.wsrep_causal_reads =
      MY_TEST(thd->variables.wsrep_sync_wait & WSREP_SYNC_WAIT_BEFORE_READ);
  return false;
}

static int wsrep_start_position_verify (const char* start_str)
{
  size_t        start_len;
  wsrep_uuid_t  uuid;
  ssize_t       uuid_len;

  start_len = strlen (start_str);
  if (start_len < 34)
    return 1;

  uuid_len = wsrep_uuid_scan (start_str, start_len, &uuid);
  if (uuid_len < 0 || (start_len - uuid_len) < 2)
    return 1;

  if (start_str[uuid_len] != ':') // separator should follow UUID
    return 1;

  char* endptr;
  wsrep_seqno_t const seqno __attribute__((unused)) // to avoid GCC warnings
    (strtoll(&start_str[uuid_len + 1], &endptr, 10));

  if (*endptr == '\0') return 0; // remaining string was seqno

  return 1;
}

bool wsrep_start_position_check (sys_var *self, THD* thd, set_var* var)
{
  char start_pos_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(start_pos_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  start_pos_buf[var->save_result.string_value.length]= 0;

  if (!wsrep_start_position_verify(start_pos_buf)) return 0;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return 1;
}

static
void wsrep_set_local_position(const char* const value, bool const sst)
{
  size_t const value_len = strlen(value);
  wsrep_uuid_t uuid;
  size_t const uuid_len = wsrep_uuid_scan(value, value_len, &uuid);
  wsrep_seqno_t const seqno = strtoll(value + uuid_len + 1, NULL, 10);

  if (sst) {
    wsrep_sst_received (wsrep, uuid, seqno, NULL, 0);
  } else {
    // initialization
    local_uuid = uuid;
    local_seqno = seqno;
  }
}

bool wsrep_start_position_update (sys_var *self, THD* thd, enum_var_type type)
{
  WSREP_INFO ("wsrep_start_position var submitted: '%s'",
              wsrep_start_position);
  // since this value passed wsrep_start_position_check, don't check anything
  // here
  wsrep_set_local_position (wsrep_start_position, true);
  return 0;
}

void wsrep_start_position_init (const char* val)
{
  if (NULL == val || wsrep_start_position_verify (val))
  {
    WSREP_ERROR("Bad initial value for wsrep_start_position: %s", 
                (val ? val : ""));
    return;
  }

  wsrep_set_local_position (val, false);
}

static bool refresh_provider_options()
{
  WSREP_DEBUG("refresh_provider_options: %s", 
              (wsrep_provider_options) ? wsrep_provider_options : "null");
  char* opts= wsrep->options_get(wsrep);
  if (opts)
  {
    if (wsrep_provider_options) my_free((void *)wsrep_provider_options);
    wsrep_provider_options = (char*)my_memdup(opts, strlen(opts) + 1, 
                                              MYF(MY_WME));
  }
  else
  {
    WSREP_ERROR("Failed to get provider options");
    return true;
  }
  return false;
}

static int wsrep_provider_verify (const char* provider_str)
{
  MY_STAT   f_stat;
  char path[FN_REFLEN];

  if (!provider_str || strlen(provider_str)== 0)
    return 1;

  if (!strcmp(provider_str, WSREP_NONE))
    return 0;

  if (!unpack_filename(path, provider_str))
    return 1;

  /* check that provider file exists */
  memset(&f_stat, 0, sizeof(MY_STAT));
  if (!my_stat(path, &f_stat, MYF(0)))
  {
    return 1;
  }
  return 0;
}

bool wsrep_provider_check (sys_var *self, THD* thd, set_var* var)
{
  char wsrep_provider_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(wsrep_provider_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  wsrep_provider_buf[var->save_result.string_value.length]= 0;

  if (!wsrep_provider_verify(wsrep_provider_buf)) return 0;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return 1;
}

bool wsrep_provider_update (sys_var *self, THD* thd, enum_var_type type)
{
  bool rcode= false;

  bool wsrep_on_saved= thd->variables.wsrep_on;
  thd->variables.wsrep_on= false;

  WSREP_DEBUG("wsrep_provider_update: %s", wsrep_provider);

  /* stop replication is heavy operation, and includes closing all client 
     connections. Closing clients may need to get LOCK_global_system_variables
     at least in MariaDB.

     Note: releasing LOCK_global_system_variables may cause race condition, if 
     there can be several concurrent clients changing wsrep_provider
  */
  mysql_mutex_unlock(&LOCK_global_system_variables);
  wsrep_stop_replication(thd);
  mysql_mutex_lock(&LOCK_global_system_variables);

  if (wsrep_inited == 1)
    wsrep_deinit(false);

  char* tmp= strdup(wsrep_provider); // wsrep_init() rewrites provider 
                                     //when fails
  if (wsrep_init())
  {
    my_error(ER_CANT_OPEN_LIBRARY, MYF(0), tmp);
    rcode = true;
  }
  free(tmp);

  // we sure don't want to use old address with new provider
  wsrep_cluster_address_init(NULL);
  wsrep_provider_options_init(NULL);

  thd->variables.wsrep_on= wsrep_on_saved;

  refresh_provider_options();

  return rcode;
}

void wsrep_provider_init (const char* value)
{
  WSREP_DEBUG("wsrep_provider_init: %s -> %s", 
              (wsrep_provider) ? wsrep_provider : "null", 
              (value) ? value : "null");
  if (NULL == value || wsrep_provider_verify (value))
  {
    WSREP_ERROR("Bad initial value for wsrep_provider: %s",
                (value ? value : ""));
    return;
  }

  if (wsrep_provider) my_free((void *)wsrep_provider);
  wsrep_provider = my_strdup(value, MYF(0));
}

bool wsrep_provider_options_check(sys_var *self, THD* thd, set_var* var)
{
  return 0;
}

bool wsrep_provider_options_update(sys_var *self, THD* thd, enum_var_type type)
{
  if (wsrep == NULL)
  {
    my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) not started", MYF(0));
    return true;
  }

  wsrep_status_t ret= wsrep->options_set(wsrep, wsrep_provider_options);
  if (ret != WSREP_OK)
  {
    WSREP_ERROR("Set options returned %d", ret);
    refresh_provider_options();
    return true;
  }
  return refresh_provider_options();
}

void wsrep_provider_options_init(const char* value)
{
  if (wsrep_provider_options && wsrep_provider_options != value) 
    my_free((void *)wsrep_provider_options);
  wsrep_provider_options = (value) ? my_strdup(value, MYF(0)) : NULL;
}

static int wsrep_cluster_address_verify (const char* cluster_address_str)
{
  /* There is no predefined address format, it depends on provider. */
  return 0;
}

bool wsrep_cluster_address_check (sys_var *self, THD* thd, set_var* var)
{
  char addr_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(addr_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  addr_buf[var->save_result.string_value.length]= 0;

  if (!wsrep_cluster_address_verify(addr_buf)) return 0;

 err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return 1;
}

bool wsrep_cluster_address_update (sys_var *self, THD* thd, enum_var_type type)
{
  bool wsrep_on_saved= thd->variables.wsrep_on;
  thd->variables.wsrep_on= false;

  /* stop replication is heavy operation, and includes closing all client 
     connections. Closing clients may need to get LOCK_global_system_variables
     at least in MariaDB.

     Note: releasing LOCK_global_system_variables may cause race condition, if 
     there can be several concurrent clients changing wsrep_provider
  */
  mysql_mutex_unlock(&LOCK_global_system_variables);
  wsrep_stop_replication(thd);

  /*
    Unlock and lock LOCK_wsrep_slave_threads to maintain lock order & avoid
    any potential deadlock.
  */
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  mysql_mutex_lock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);

  if (wsrep_start_replication())
  {
    wsrep_create_rollbacker();
    wsrep_create_appliers(wsrep_slave_threads);
  }

  thd->variables.wsrep_on= wsrep_on_saved;

  return false;
}

void wsrep_cluster_address_init (const char* value)
{
  WSREP_DEBUG("wsrep_cluster_address_init: %s -> %s", 
              (wsrep_cluster_address) ? wsrep_cluster_address : "null", 
              (value) ? value : "null");

  if (wsrep_cluster_address) my_free ((void*)wsrep_cluster_address);
  wsrep_cluster_address = (value) ? my_strdup(value, MYF(0)) : NULL;
}

/* wsrep_cluster_name cannot be NULL or an empty string. */
bool wsrep_cluster_name_check (sys_var *self, THD* thd, set_var* var)
{
  if (!var->save_result.string_value.str ||
      (var->save_result.string_value.length == 0))
  {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             (var->save_result.string_value.str ?
              var->save_result.string_value.str : "NULL"));
    return 1;
  }
  return 0;
}

bool wsrep_cluster_name_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

bool wsrep_node_name_check (sys_var *self, THD* thd, set_var* var)
{
  // TODO: for now 'allow' 0-length string to be valid (default)
  if (!var->save_result.string_value.str)
  {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             (var->save_result.string_value.str ?
              var->save_result.string_value.str : "NULL"));
    return 1;
  }
  return 0;
}

bool wsrep_node_name_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

// TODO: do something more elaborate, like checking connectivity
bool wsrep_node_address_check (sys_var *self, THD* thd, set_var* var)
{
  char addr_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
    goto err;

  memcpy(addr_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  addr_buf[var->save_result.string_value.length]= 0;

  // TODO: for now 'allow' 0-length string to be valid (default)
  return 0;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return 1;
}

bool wsrep_node_address_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}

void wsrep_node_address_init (const char* value)
{
  if (wsrep_node_address && strcmp(wsrep_node_address, value))
    my_free ((void*)wsrep_node_address);

  wsrep_node_address = (value) ? my_strdup(value, MYF(0)) : NULL;
}

bool wsrep_slave_threads_check (sys_var *self, THD* thd, set_var* var)
{
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  wsrep_slave_count_change += (var->save_result.ulonglong_value -
                               wsrep_slave_threads);
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);

  return 0;
}

bool wsrep_slave_threads_update (sys_var *self, THD* thd, enum_var_type type)
{
  if (wsrep_slave_count_change > 0)
  {
    wsrep_create_appliers(wsrep_slave_count_change);
    wsrep_slave_count_change = 0;
  }
  return false;
}

bool wsrep_desync_check (sys_var *self, THD* thd, set_var* var)
{
  bool new_wsrep_desync= (bool) var->save_result.ulonglong_value;
  if (wsrep_desync == new_wsrep_desync) {
    if (new_wsrep_desync) {
      push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                   ER_WRONG_VALUE_FOR_VAR,
                   "'wsrep_desync' is already ON.");
    } else {
      push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                   ER_WRONG_VALUE_FOR_VAR,
                   "'wsrep_desync' is already OFF.");
    }
  }
  return 0;
}

bool wsrep_desync_update (sys_var *self, THD* thd, enum_var_type type)
{
  if (wsrep == NULL)
  {
    my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) not started", MYF(0));
    return true;
  }

  wsrep_status_t ret(WSREP_WARNING);
  if (wsrep_desync) {
    ret = wsrep->desync (wsrep);
    if (ret != WSREP_OK) {
      WSREP_WARN ("SET desync failed %d for schema: %s, query: %s", ret,
                  (thd->db ? thd->db : "(null)"),
                  thd->query());
      my_error (ER_CANNOT_USER, MYF(0), "'desync'", thd->query());
      return true;
    }
  } else {
    ret = wsrep->resync (wsrep);
    if (ret != WSREP_OK) {
      WSREP_WARN ("SET resync failed %d for schema: %s, query: %s", ret,
                  (thd->db ? thd->db : "(null)"),
                  thd->query());
      my_error (ER_CANNOT_USER, MYF(0), "'resync'", thd->query());
      return true;
    }
  }
  return false;
}

static SHOW_VAR wsrep_status_vars[]=
{
  {"connected",         (char*) &wsrep_connected,         SHOW_BOOL},
  {"ready",             (char*) &wsrep_ready,             SHOW_BOOL},
  {"cluster_state_uuid",(char*) &wsrep_cluster_state_uuid,SHOW_CHAR_PTR},
  {"cluster_conf_id",   (char*) &wsrep_cluster_conf_id,   SHOW_LONGLONG},
  {"cluster_status",    (char*) &wsrep_cluster_status,    SHOW_CHAR_PTR},
  {"cluster_size",      (char*) &wsrep_cluster_size,      SHOW_LONG_NOFLUSH},
  {"local_index",       (char*) &wsrep_local_index,       SHOW_LONG_NOFLUSH},
  {"local_bf_aborts",   (char*) &wsrep_show_bf_aborts,    SHOW_SIMPLE_FUNC},
  {"provider_name",     (char*) &wsrep_provider_name,     SHOW_CHAR_PTR},
  {"provider_version",  (char*) &wsrep_provider_version,  SHOW_CHAR_PTR},
  {"provider_vendor",   (char*) &wsrep_provider_vendor,   SHOW_CHAR_PTR},
  {"thread_count",      (char*) &wsrep_running_threads,   SHOW_LONG_NOFLUSH}
};

static int show_var_cmp(const void *var1, const void *var2)
{
  return strcasecmp(((SHOW_VAR*)var1)->name, ((SHOW_VAR*)var2)->name);
}

int wsrep_show_status (THD *thd, SHOW_VAR *var, char *buff,
                       enum enum_var_type scope)
{
  uint i, maxi= SHOW_VAR_FUNC_BUFF_SIZE / sizeof(*var) - 1;
  SHOW_VAR *v= (SHOW_VAR *)buff;

  var->type= SHOW_ARRAY;
  var->value= buff;

  for (i=0; i < array_elements(wsrep_status_vars); i++)
    *v++= wsrep_status_vars[i];

  DBUG_ASSERT(i < maxi);

  if (wsrep != NULL)
  {
    wsrep_stats_var* stats= wsrep->stats_get(wsrep);
    for (wsrep_stats_var *sv= stats;
         i < maxi && sv && sv->name; i++,
           sv++, v++)
    {
      v->name = thd->strdup(sv->name);
      switch (sv->type) {
      case WSREP_VAR_INT64:
        v->value = (char*)thd->memdup(&sv->value._int64, sizeof(longlong));
        v->type  = SHOW_LONGLONG;
        break;
      case WSREP_VAR_STRING:
        v->value = thd->strdup(sv->value._string);
        v->type  = SHOW_CHAR;
        break;
      case WSREP_VAR_DOUBLE:
        v->value = (char*)thd->memdup(&sv->value._double, sizeof(double));
        v->type  = SHOW_DOUBLE;
        break;
      }
    }
    wsrep->stats_free(wsrep, stats);
  }

  my_qsort(buff, i, sizeof(*v), show_var_cmp);

  v->name= 0;                                   // terminator
  return 0;
}

