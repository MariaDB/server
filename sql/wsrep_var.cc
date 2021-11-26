/* Copyright 2008-2021 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

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
#include "wsrep_trans_observer.h"

ulong   wsrep_reject_queries;

int wsrep_init_vars()
{
  wsrep_provider        = my_strdup(PSI_INSTRUMENT_ME, WSREP_NONE, MYF(MY_WME));
  wsrep_provider_options= my_strdup(PSI_INSTRUMENT_ME, "", MYF(MY_WME));
  wsrep_cluster_address = my_strdup(PSI_INSTRUMENT_ME, "", MYF(MY_WME));
  wsrep_cluster_name    = my_strdup(PSI_INSTRUMENT_ME, WSREP_CLUSTER_NAME, MYF(MY_WME));
  wsrep_node_name       = my_strdup(PSI_INSTRUMENT_ME, "", MYF(MY_WME));
  wsrep_node_address    = my_strdup(PSI_INSTRUMENT_ME, "", MYF(MY_WME));
  wsrep_node_incoming_address= my_strdup(PSI_INSTRUMENT_ME, WSREP_NODE_INCOMING_AUTO, MYF(MY_WME));
  if (wsrep_gtid_mode)
    wsrep_start_position  = my_strdup(PSI_INSTRUMENT_ME, WSREP_START_POSITION_ZERO_GTID, MYF(MY_WME));
  else
    wsrep_start_position  = my_strdup(PSI_INSTRUMENT_ME, WSREP_START_POSITION_ZERO, MYF(MY_WME));
  return 0;
}

static int get_provider_option_value(const char* opts,
                                     const char* opt_name,
                                     ulong* opt_value)
{
  int ret= 1;
  ulong opt_value_tmp;
  char *opt_value_str, *s, *opts_copy= my_strdup(PSI_INSTRUMENT_ME, opts, MYF(MY_WME));

  if ((opt_value_str= strstr(opts_copy, opt_name)) == NULL)
    goto end;
  opt_value_str= strtok_r(opt_value_str, "=", &s);
  if (opt_value_str == NULL) goto end;
  opt_value_str= strtok_r(NULL, ";", &s);
  if (opt_value_str == NULL) goto end;

  opt_value_tmp= strtoul(opt_value_str, NULL, 10);
  if (errno == ERANGE) goto end;

  *opt_value= opt_value_tmp;
  ret= 0;

end:
  my_free(opts_copy);
  return ret;
}

static bool refresh_provider_options()
{
  WSREP_DEBUG("refresh_provider_options: %s",
              (wsrep_provider_options) ? wsrep_provider_options : "null");

  try
  {
    std::string opts= Wsrep_server_state::instance().provider().options();
    wsrep_provider_options_init(opts.c_str());
    get_provider_option_value(wsrep_provider_options,
                              (char*)"repl.max_ws_size",
                              &wsrep_max_ws_size);
    return false;
  }
  catch (...)
  {
    WSREP_ERROR("Failed to get provider options");
    return true;
  }
}

void wsrep_set_wsrep_on(THD* thd)
{
  if (thd)
    thd->wsrep_was_on= WSREP_ON_;
  WSREP_PROVIDER_EXISTS_= wsrep_provider &&
    strncasecmp(wsrep_provider, WSREP_NONE, FN_REFLEN);
  WSREP_ON_= global_system_variables.wsrep_on && WSREP_PROVIDER_EXISTS_;
}

bool wsrep_on_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  if (var_type == OPT_GLOBAL)
  {
    my_bool saved_wsrep_on= global_system_variables.wsrep_on;

    thd->variables.wsrep_on= global_system_variables.wsrep_on;

    // If wsrep has not been inited we need to do it now
    if (global_system_variables.wsrep_on && wsrep_provider && !wsrep_inited)
    {
      // wsrep_init() rewrites provide if it fails
      char* tmp= strdup(wsrep_provider);
      mysql_mutex_unlock(&LOCK_global_system_variables);

      if (wsrep_init())
      {
        my_error(ER_CANT_OPEN_LIBRARY, MYF(0), tmp, my_error, "wsrep_init failed");
        //rcode= true;
        saved_wsrep_on= false;
      }

      free(tmp);
      mysql_mutex_lock(&LOCK_global_system_variables);
    }

    thd->variables.wsrep_on= global_system_variables.wsrep_on= saved_wsrep_on;
  }

  wsrep_set_wsrep_on(thd);

  if (var_type == OPT_GLOBAL)
  {
    if (thd->variables.wsrep_on &&
        thd->wsrep_cs().state() == wsrep::client_state::s_none)
    {
      wsrep_open(thd);
      wsrep_before_command(thd);
    }
  }

  return false;
}

bool wsrep_on_check(sys_var *self, THD* thd, set_var* var)
{
  bool new_wsrep_on= (bool)var->save_result.ulonglong_value;

  if (check_has_super(self, thd, var))
    return true;

  if (new_wsrep_on)
  {
    if (!WSREP_PROVIDER_EXISTS)
    {
      my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) can't be enabled "
                 "if the wsrep_provider is unset or set to 'none'", MYF(0));
      return true;
    }

    if (var->type == OPT_SESSION &&
        !global_system_variables.wsrep_on)
    {
      my_message(ER_WRONG_ARGUMENTS,
                 "Can't enable @@session.wsrep_on, "
                 "while @@global.wsrep_on is disabled", MYF(0));
      return true;
    }
  }

  if (thd->in_active_multi_stmt_transaction())
  {
    my_error(ER_CANT_DO_THIS_DURING_AN_TRANSACTION, MYF(0));
    return true;
  }

  if (var->type == OPT_GLOBAL)
  {
    /*
      The global value is about to change. Cleanup
      the transaction state and close the client
      state. wsrep_on_update() will take care of
      reopening it should wsrep_on be re-enabled.
     */
    if (global_system_variables.wsrep_on && !new_wsrep_on)
    {
      wsrep_commit_empty(thd, true);
      wsrep_after_statement(thd);
      wsrep_after_command_ignore_result(thd);
      wsrep_close(thd);
      wsrep_cleanup(thd);
    }
  }

  return false;
}

bool wsrep_causal_reads_update (sys_var *self, THD* thd, enum_var_type var_type)
{
  if (thd->variables.wsrep_causal_reads) {
    thd->variables.wsrep_sync_wait |= WSREP_SYNC_WAIT_BEFORE_READ;
  } else {
    thd->variables.wsrep_sync_wait &= ~WSREP_SYNC_WAIT_BEFORE_READ;
  }

  // update global settings too.
  if (global_system_variables.wsrep_causal_reads) {
      global_system_variables.wsrep_sync_wait |= WSREP_SYNC_WAIT_BEFORE_READ;
  } else {
      global_system_variables.wsrep_sync_wait &= ~WSREP_SYNC_WAIT_BEFORE_READ;
  }

  return false;
}

bool wsrep_sync_wait_update (sys_var* self, THD* thd, enum_var_type var_type)
{
  thd->variables.wsrep_causal_reads= thd->variables.wsrep_sync_wait &
          WSREP_SYNC_WAIT_BEFORE_READ;

  // update global settings too
  global_system_variables.wsrep_causal_reads= global_system_variables.wsrep_sync_wait &
          WSREP_SYNC_WAIT_BEFORE_READ;

  return false;
}

template<typename T>
static T parse_value(char** startptr, char** endptr)
{
   T val= strtoll(*startptr, *&endptr, 10);
   *startptr= *endptr;
   return val;
}

/*
  Verify the format of the given UUID:seqno.

  @return
    true                    Fail
    false                   Pass
*/
static
bool wsrep_start_position_verify (const char* start_str)
{
  size_t        start_len;
  wsrep_uuid_t  uuid;
  ssize_t       uuid_len;

  // Check whether it has minimum acceptable length.
  start_len= strlen (start_str);
  if (start_len < 34)
    return true;

  /*
    Parse the input to check whether UUID length is acceptable
    and seqno has been provided.
  */
  uuid_len= wsrep_uuid_scan (start_str, start_len, &uuid);
  if (uuid_len < 0 || (start_len - uuid_len) < 2)
    return true;

  // Separator must follow the UUID.
  if (start_str[uuid_len] != ':')
    return true;

  char* endptr;
  char* startptr= (char *)start_str + uuid_len + 1;
  wsrep_seqno_t const seqno(parse_value<uint64_t>(&startptr, &endptr));

  // Do not allow seqno < -1
  if (seqno < -1)
    return true;

  // Start parsing native GTID part
  if (*startptr == ',')
  {
    startptr++;
    uint32_t domain  __attribute__((unused))
      (parse_value<uint32_t>(&startptr, &endptr));
    if (*endptr != '-') return true;
    startptr++;
    uint32_t server  __attribute__((unused))
      (parse_value<uint32_t>(&startptr, &endptr));
    if (*endptr != '-') return true;
    startptr++;
    uint64_t seq  __attribute__((unused))
      (parse_value<uint64_t>(&startptr, &endptr));
  }

  // Remaining string was seqno.
  if (*endptr == '\0') return false;

  return true;
}


static
bool wsrep_set_local_position(THD* thd, const char* const value,
                              size_t length, bool const sst)
{
  char* endptr;
  char* startptr;
  wsrep_uuid_t uuid;
  size_t const uuid_len= wsrep_uuid_scan(value, length, &uuid);
  startptr= (char *)value + uuid_len + 1;
  wsrep_seqno_t const seqno= parse_value<uint64_t>(&startptr, &endptr);

  if (*startptr == ',')
  {
    startptr++;
    wsrep_gtid_server.domain_id= parse_value<uint32_t>(&startptr, &endptr);
    startptr++;
    wsrep_gtid_server.server_id= parse_value<uint32_t>(&startptr, &endptr);
    startptr++;
    wsrep_gtid_server.seqno(parse_value<uint64_t>(&startptr, &endptr));
  }

  char start_pos_buf[FN_REFLEN];
  memcpy(start_pos_buf, value, length);
  start_pos_buf[length]='\0';

  // If both are same as WSREP_START_POSITION_ZERO just set local
  if (!strcmp(start_pos_buf, WSREP_START_POSITION_ZERO) &&
      !strcmp(wsrep_start_position, WSREP_START_POSITION_ZERO))
    goto set;
  else
    WSREP_INFO("SST setting local position to %s current %s", start_pos_buf, wsrep_start_position);

  if (sst)
    return (wsrep_sst_received (thd, uuid, seqno, NULL, 0));

set:
  local_uuid= uuid;
  local_seqno= seqno;

  return false;
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


  WSREP_DEBUG("SST wsrep_start_position check for new position %s old %s",
	     start_pos_buf, wsrep_start_position);

  // Verify the format.
  if (wsrep_start_position_verify(start_pos_buf)) return true;


  // Give error if position is updated when wsrep is not enabled or
  // provider is not loaded.
  if ((!WSREP_ON || !Wsrep_server_state::instance().is_provider_loaded())
      && strcmp(start_pos_buf, WSREP_START_POSITION_ZERO))
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WRONG_VALUE_FOR_VAR,
                 "Cannot set 'wsrep_start_position' because "
                 "wsrep is switched off or provider is not loaded");
    goto err;
  }

  /*
    As part of further verification, we try to update the value and catch
    errors (if any) only when value actually has been changed.
  */
  if (wsrep_set_local_position(thd, var->save_result.string_value.str,
                               var->save_result.string_value.length,
                               true))
    goto err;

  return false;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return true;
}

bool wsrep_start_position_update (sys_var *self, THD* thd, enum_var_type type)
{
  // Print a confirmation that wsrep_start_position has been updated.
  WSREP_INFO ("wsrep_start_position set to '%s'", wsrep_start_position);
  return false;
}

bool wsrep_start_position_init (const char* val)
{
  if (NULL == val || wsrep_start_position_verify (val))
  {
    WSREP_ERROR("Bad initial value for wsrep_start_position: %s",
                (val ? val : ""));
    return true;
  }

  if (wsrep_set_local_position (NULL, val, strlen(val), false))
  {
    WSREP_ERROR("Failed to set initial wsep_start_position: %s", val);
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

  if (MY_S_ISDIR(f_stat.st_mode))
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


  WSREP_DEBUG("wsrep_provider_update: %s", wsrep_provider);

  /* stop replication is heavy operation, and includes closing all client
     connections. Closing clients may need to get LOCK_global_system_variables
     at least in MariaDB.

     Note: releasing LOCK_global_system_variables may cause race condition, if
     there can be several concurrent clients changing wsrep_provider
  */
  mysql_mutex_unlock(&LOCK_global_system_variables);
  wsrep_stop_replication(thd);

  /* provider status variables are allocated in provider library
     and need to freed here, otherwise a dangling reference to
     wsrep_status_vars would remain in THD
  */
  wsrep_free_status(thd);

  if (wsrep_inited == 1)
    wsrep_deinit(false);

  char* tmp= strdup(wsrep_provider); // wsrep_init() rewrites provider
                                     //when fails

  if (wsrep_init())
  {
    my_error(ER_CANT_OPEN_LIBRARY, MYF(0), tmp, my_error, "wsrep_init failed");
    rcode= true;
  }
  free(tmp);

  // we sure don't want to use old address with new provider
  wsrep_cluster_address_init(NULL);
  wsrep_provider_options_init(NULL);
  if (!rcode)
    refresh_provider_options();

  wsrep_set_wsrep_on(thd);
  mysql_mutex_lock(&LOCK_global_system_variables);

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
  wsrep_provider= my_strdup(PSI_INSTRUMENT_MEM, value, MYF(0));
  wsrep_set_wsrep_on(NULL);
}

bool wsrep_provider_options_check(sys_var *self, THD* thd, set_var* var)
{
  if (!WSREP_ON)
  {
    my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) not started", MYF(0));
    return true;
  }
  return false;
}

bool wsrep_provider_options_update(sys_var *self, THD* thd, enum_var_type type)
{
  if (wsrep_provider_options)
  {
    enum wsrep::provider::status ret=
      Wsrep_server_state::instance().provider().options(wsrep_provider_options);
    if (ret)
    {
      WSREP_ERROR("Set options returned %d", ret);
      goto err;
    }

    return refresh_provider_options();
  }
err:
  refresh_provider_options();
  return true;
}

void wsrep_provider_options_init(const char* value)
{
  if (wsrep_provider_options && wsrep_provider_options != value)
    my_free((void *)wsrep_provider_options);
  wsrep_provider_options= value ? my_strdup(PSI_INSTRUMENT_MEM, value, MYF(0)) : NULL;
}

bool wsrep_reject_queries_update(sys_var *self, THD* thd, enum_var_type type)
{
    switch (wsrep_reject_queries) {
        case WSREP_REJECT_NONE:
            WSREP_INFO("Allowing client queries due to manual setting");
            break;
        case WSREP_REJECT_ALL:
            WSREP_INFO("Rejecting client queries due to manual setting");
            break;
        case WSREP_REJECT_ALL_KILL:
            /* close all client connections, but this one */
            wsrep_close_client_connections(FALSE, thd);
            WSREP_INFO("Rejecting client queries and killing connections due to manual setting");
            break;
        default:
          WSREP_INFO("Unknown value for wsrep_reject_queries: %lu",
                     wsrep_reject_queries);
            return true;
    }
    return false;
}

bool wsrep_debug_update(sys_var *self, THD* thd, enum_var_type type)
{
  // Give warnings if wsrep_debug is set and wsrep is disabled or
  // provider is not loaded, it will not have any effect
  if ((!WSREP_ON || !Wsrep_server_state::instance().is_provider_loaded())
      && wsrep_debug)
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_WRONG_VALUE_FOR_VAR,
                 "Setting 'wsrep_debug' has no effect because "
                 "wsrep is switched off");
    wsrep_debug= 0;
  }
  else
    Wsrep_server_state::instance().debug_log_level(wsrep_debug);

  return false;
}

bool
wsrep_gtid_seq_no_check(sys_var *self, THD *thd, set_var *var)
{
  ulonglong new_wsrep_gtid_seq_no= var->save_result.ulonglong_value;
  if (wsrep_gtid_mode && new_wsrep_gtid_seq_no > wsrep_gtid_server.seqno())
    return false;
  return true;
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
      (var->save_result.string_value.length >= sizeof(addr_buf))) // safety
    goto err;

  strmake(addr_buf, var->save_result.string_value.str,
          MY_MIN(sizeof(addr_buf)-1, var->save_result.string_value.length));

  if (!wsrep_cluster_address_verify(addr_buf))
    return 0;

 err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return 1;
}

bool wsrep_cluster_address_update (sys_var *self, THD* thd, enum_var_type type)
{
  if (!Wsrep_server_state::instance().is_provider_loaded())
  {
    WSREP_INFO("WSREP (galera) provider is not loaded, can't re(start) replication.");
    return false;
  }

  /* stop replication is heavy operation, and includes closing all client
     connections. Closing clients may need to get LOCK_global_system_variables
     at least in MariaDB.
  */
  char *tmp= my_strdup(PSI_INSTRUMENT_ME, wsrep_cluster_address, MYF(MY_WME));
  WSREP_DEBUG("wsrep_cluster_address_update: %s", wsrep_cluster_address);
  mysql_mutex_unlock(&LOCK_global_system_variables);

  mysql_mutex_lock(&LOCK_wsrep_cluster_config);
  wsrep_stop_replication(thd);

  if (*tmp && wsrep_start_replication(tmp))
  {
    wsrep_create_rollbacker();
    WSREP_DEBUG("Cluster address update creating %ld applier threads running %lu",
	    wsrep_slave_threads, wsrep_running_applier_threads);
    wsrep_create_appliers(wsrep_slave_threads);
  }
  mysql_mutex_unlock(&LOCK_wsrep_cluster_config);

  mysql_mutex_lock(&LOCK_global_system_variables);
  if (strcmp(tmp, wsrep_cluster_address))
  {
    my_free((void*)wsrep_cluster_address);
    wsrep_cluster_address= tmp;
  }
  else
    my_free(tmp);

  return false;
}

void wsrep_cluster_address_init (const char* value)
{
  WSREP_DEBUG("wsrep_cluster_address_init: %s -> %s",
              (wsrep_cluster_address) ? wsrep_cluster_address : "null",
              (value) ? value : "null");

  my_free(const_cast<char*>(wsrep_cluster_address));
  wsrep_cluster_address= my_strdup(PSI_INSTRUMENT_MEM, safe_str(value), MYF(0));
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

  wsrep_node_address= value ? my_strdup(PSI_INSTRUMENT_MEM, value, MYF(0)) : NULL;
}

static void wsrep_slave_count_change_update ()
{
  wsrep_slave_count_change = (wsrep_slave_threads - wsrep_running_applier_threads);
  WSREP_DEBUG("Change on slave threads: New %ld old %lu difference %d",
	  wsrep_slave_threads, wsrep_running_applier_threads, wsrep_slave_count_change);
}

bool wsrep_slave_threads_update (sys_var *self, THD* thd, enum_var_type type)
{
  if (!wsrep_cluster_address_exists())
    return false;

  mysql_mutex_unlock(&LOCK_global_system_variables);
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  mysql_mutex_lock(&LOCK_global_system_variables);
  bool res= false;

  wsrep_slave_count_change_update();

  if (wsrep_slave_count_change > 0)
  {
    WSREP_DEBUG("Creating %d applier threads, total %ld", wsrep_slave_count_change, wsrep_slave_threads);
    res= wsrep_create_appliers(wsrep_slave_count_change, true);
    WSREP_DEBUG("Running %lu applier threads", wsrep_running_applier_threads);
    wsrep_slave_count_change = 0;
  }

  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);

  return res;
}

bool wsrep_desync_check (sys_var *self, THD* thd, set_var* var)
{
  if (!WSREP_ON)
  {
    my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) not started", MYF(0));
    return true;
  }

  if (thd->global_read_lock.is_acquired())
  {
    my_message (ER_CANNOT_USER, "Global read lock acquired. Can't set 'wsrep_desync'", MYF(0));
    return true;
  }

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
    return false;
  }
  int ret= 1;
  if (new_wsrep_desync) {
    ret= Wsrep_server_state::instance().provider().desync();
    if (ret) {
      WSREP_WARN ("SET desync failed %d for schema: %s, query: %s", ret,
                  thd->db.str, wsrep_thd_query(thd));
      my_error (ER_CANNOT_USER, MYF(0), "'desync'", thd->query());
      return true;
    }
  } else {
    THD_STAGE_INFO(thd, stage_waiting_flow);
    ret= Wsrep_server_state::instance().provider().resync();
    if (ret != WSREP_OK) {
      WSREP_WARN ("SET resync failed %d for schema: %s, query: %s", ret,
                  thd->get_db(), wsrep_thd_query(thd));
      my_error (ER_CANNOT_USER, MYF(0), "'resync'", thd->query());
      return true;
    }
  }
  return false;
}

bool wsrep_desync_update (sys_var *self, THD* thd, enum_var_type type)
{
  return false;
}

bool wsrep_trx_fragment_size_check (sys_var *self, THD* thd, set_var* var)
{
  if (var->value == NULL) {
    return false;
  }

  const ulong new_trx_fragment_size= var->value->val_uint();

  if (!WSREP(thd) && new_trx_fragment_size > 0) {
    push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                  ER_WRONG_VALUE_FOR_VAR,
                  "Cannot set 'wsrep_trx_fragment_size' to a value other than "
                  "0 because wsrep is switched off.");
    return true;
  }

  if (new_trx_fragment_size > 0 && !wsrep_provider_is_SR_capable()) {
    push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                  ER_WRONG_VALUE_FOR_VAR,
                  "Cannot set 'wsrep_trx_fragment_size' to a value other than "
                  "0 because the wsrep_provider does not support streaming "
                  "replication.");
    return true;
  }

  if (wsrep_protocol_version < 4  && new_trx_fragment_size > 0) {
    push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                  ER_WRONG_VALUE_FOR_VAR,
                  "Cannot set 'wsrep_trx_fragment_size' to a value other than "
                  "0 because cluster is not yet operating in Galera 4 mode.");
    return true;
  }

  return false;
}

bool wsrep_trx_fragment_size_update(sys_var* self, THD *thd, enum_var_type)
{
  WSREP_DEBUG("wsrep_trx_fragment_size_update: %llu",
              thd->variables.wsrep_trx_fragment_size);

  // Give error if wsrep_trx_fragment_size is set and wsrep is disabled or
  // provider is not loaded
  if (!WSREP_ON || !Wsrep_server_state::instance().is_provider_loaded())
  {
    push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                  ER_WRONG_VALUE_FOR_VAR,
                  "Cannot set 'wsrep_trx_fragment_size' because "
                  "wsrep is switched off");
    return true;
  }

  if (thd->variables.wsrep_trx_fragment_size)
  {
    return thd->wsrep_cs().enable_streaming(
      wsrep_fragment_unit(thd->variables.wsrep_trx_fragment_unit),
      size_t(thd->variables.wsrep_trx_fragment_size));
  }
  else
  {
    thd->wsrep_cs().disable_streaming();
    return false;
  }
}

bool wsrep_trx_fragment_unit_update(sys_var* self, THD *thd, enum_var_type)
{
  WSREP_DEBUG("wsrep_trx_fragment_unit_update: %lu",
              thd->variables.wsrep_trx_fragment_unit);

  // Give error if wsrep_trx_fragment_unit is set and wsrep is disabled or
  // provider is not loaded
  if (!WSREP_ON || !Wsrep_server_state::instance().is_provider_loaded())
  {
    push_warning (thd, Sql_condition::WARN_LEVEL_WARN,
                  ER_WRONG_VALUE_FOR_VAR,
                  "Cannot set 'wsrep_trx_fragment_unit' because "
                  "wsrep is switched off");
    return true;
  }

  if (thd->variables.wsrep_trx_fragment_size)
  {
    return thd->wsrep_cs().enable_streaming(
      wsrep_fragment_unit(thd->variables.wsrep_trx_fragment_unit),
      size_t(thd->variables.wsrep_trx_fragment_size));
  }
  return false;
}

bool wsrep_max_ws_size_check(sys_var *self, THD* thd, set_var* var)
{
  if (!WSREP_ON)
  {
    my_message(ER_WRONG_ARGUMENTS, "WSREP (galera) not started", MYF(0));
    return true;
  }
  return false;
}

bool wsrep_max_ws_size_update(sys_var *self, THD *thd, enum_var_type)
{
  char max_ws_size_opt[128];
  my_snprintf(max_ws_size_opt, sizeof(max_ws_size_opt),
              "repl.max_ws_size=%d", wsrep_max_ws_size);
  enum wsrep::provider::status ret= Wsrep_server_state::instance().provider().options(max_ws_size_opt);
  if (ret)
  {
    WSREP_ERROR("Set options returned %d", ret);
    return true;
  }
  return refresh_provider_options();
}

bool wsrep_mode_check(sys_var *self, THD* thd, set_var* var)
{
  return false;
}

#if UNUSED /* eaec266eb16c (Sergei Golubchik  2014-09-28) */
static SHOW_VAR wsrep_status_vars[]=
{
  {"connected",         (char*) &wsrep_connected,         SHOW_BOOL},
  {"ready",             (char*) &wsrep_show_ready,        SHOW_FUNC},
  {"cluster_state_uuid",(char*) &wsrep_cluster_state_uuid,SHOW_CHAR_PTR},
  {"cluster_conf_id",   (char*) &wsrep_cluster_conf_id,   SHOW_LONGLONG},
  {"cluster_status",    (char*) &wsrep_cluster_status,    SHOW_CHAR_PTR},
  {"cluster_size",      (char*) &wsrep_cluster_size,      SHOW_LONG_NOFLUSH},
  {"local_index",       (char*) &wsrep_local_index,       SHOW_LONG_NOFLUSH},
  {"local_bf_aborts",   (char*) &wsrep_show_bf_aborts,    SHOW_FUNC},
  {"provider_name",     (char*) &wsrep_provider_name,     SHOW_CHAR_PTR},
  {"provider_version",  (char*) &wsrep_provider_version,  SHOW_CHAR_PTR},
  {"provider_vendor",   (char*) &wsrep_provider_vendor,   SHOW_CHAR_PTR},
  {"provider_capabilities", (char*) &wsrep_provider_capabilities, SHOW_CHAR_PTR},
  {"thread_count",      (char*) &wsrep_running_threads,   SHOW_LONG_NOFLUSH},
  {"applier_thread_count", (char*)&wsrep_running_applier_threads, SHOW_LONG_NOFLUSH},
  {"rollbacker_thread_count", (char *)&wsrep_running_rollbacker_threads, SHOW_LONG_NOFLUSH},
};

static int show_var_cmp(const void *var1, const void *var2)
{
  return strcasecmp(((SHOW_VAR*)var1)->name, ((SHOW_VAR*)var2)->name);
}

/*
 * Status variables stuff below
 */
static inline void
wsrep_assign_to_mysql (SHOW_VAR* mysql, wsrep_stats_var* wsrep_var)
{
  mysql->name= wsrep_var->name;
  switch (wsrep_var->type) {
  case WSREP_VAR_INT64:
    mysql->value= (char*) &wsrep_var->value._int64;
    mysql->type= SHOW_LONGLONG;
    break;
  case WSREP_VAR_STRING:
    mysql->value= (char*) &wsrep_var->value._string;
    mysql->type= SHOW_CHAR_PTR;
    break;
  case WSREP_VAR_DOUBLE:
    mysql->value= (char*) &wsrep_var->value._double;
    mysql->type= SHOW_DOUBLE;
    break;
  }
}
#endif /* UNUSED */

#if DYNAMIC
// somehow this mysql status thing works only with statically allocated arrays.
static SHOW_VAR*          mysql_status_vars= NULL;
static int                mysql_status_len= -1;
#else
static SHOW_VAR           mysql_status_vars[512 + 1];
static const int          mysql_status_len= 512;
#endif

static void export_wsrep_status_to_mysql(THD* thd)
{
  int wsrep_status_len, i;

  thd->wsrep_status_vars= Wsrep_server_state::instance().status();

  wsrep_status_len= thd->wsrep_status_vars.size();

#if DYNAMIC
  if (wsrep_status_len != mysql_status_len) {
    void* tmp= realloc (mysql_status_vars,
                         (wsrep_status_len + 1) * sizeof(SHOW_VAR));
    if (!tmp) {

      sql_print_error ("Out of memory for wsrep status variables."
                       "Number of variables: %d", wsrep_status_len);
      return;
    }

    mysql_status_len= wsrep_status_len;
    mysql_status_vars= (SHOW_VAR*)tmp;
  }
  /* @TODO: fix this: */
#else
  if (mysql_status_len < wsrep_status_len) wsrep_status_len= mysql_status_len;
#endif

  for (i= 0; i < wsrep_status_len; i++)
  {
    mysql_status_vars[i].name= (char*)thd->wsrep_status_vars[i].name().c_str();
    mysql_status_vars[i].value= (char*)thd->wsrep_status_vars[i].value().c_str();
    mysql_status_vars[i].type= SHOW_CHAR;
  }

  mysql_status_vars[wsrep_status_len].name  = NullS;
  mysql_status_vars[wsrep_status_len].value = NullS;
  mysql_status_vars[wsrep_status_len].type  = SHOW_LONG;
}

int wsrep_show_status (THD *thd, SHOW_VAR *var, void *,
                       system_status_var *, enum_var_type)
{
  /* Note that we should allow show status like 'wsrep%' even
  when WSREP(thd) is false. */
  if (WSREP_ON)
  {
    export_wsrep_status_to_mysql(thd);
    var->type= SHOW_ARRAY;
    var->value= (char *) &mysql_status_vars;
  }
  else
  {
    var->type= SHOW_CHAR;
    var->value= (char*) "0";
  }
  return 0;
}

void wsrep_free_status (THD* thd)
{
  thd->wsrep_status_vars.clear();
}

bool wsrep_gtid_domain_id_update(sys_var* self, THD *thd, enum_var_type)
{
  WSREP_DEBUG("wsrep_gtid_domain_id_update: %llu",
              wsrep_gtid_domain_id);
  wsrep_gtid_server.domain_id= wsrep_gtid_domain_id;
  return false;
}

