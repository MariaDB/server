/*
   Copyright (c) 2007, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
  Functions to autenticate and handle reqests for a connection
*/

#include "mariadb.h"
#include "mysqld.h"
#include "sql_priv.h"
#ifndef __WIN__
#include <netdb.h>        // getservbyname, servent
#endif
#include "sql_audit.h"
#include "sql_connect.h"
#include "probes_mysql.h"
#include "sql_parse.h"                          // sql_command_flags,
                                                // execute_init_command,
                                                // do_command
#include "sql_db.h"                             // mysql_change_db
#include "hostname.h" // inc_host_errors, ip_to_hostname,
                      // reset_host_errors
#include "sql_acl.h"  // acl_getroot, NO_ACCESS, SUPER_ACL
#include "sql_callback.h"
#include "wsrep_mysqld.h"
#include "proxy_protocol.h"

HASH global_user_stats, global_client_stats, global_table_stats;
HASH global_index_stats;
/* Protects the above global stats */
extern mysql_mutex_t LOCK_global_user_client_stats;
extern mysql_mutex_t LOCK_global_table_stats;
extern mysql_mutex_t LOCK_global_index_stats;
extern vio_keepalive_opts opt_vio_keepalive;

/*
  Get structure for logging connection data for the current user
*/

#ifndef NO_EMBEDDED_ACCESS_CHECKS
static HASH hash_user_connections;

int get_or_create_user_conn(THD *thd, const char *user,
                            const char *host,
                            const USER_RESOURCES *mqh)
{
  int return_val= 0;
  size_t temp_len, user_len;
  char temp_user[USER_HOST_BUFF_SIZE];
  struct  user_conn *uc;

  DBUG_ASSERT(user != 0);
  DBUG_ASSERT(host != 0);
  DBUG_ASSERT(thd->user_connect == 0);

  user_len= strlen(user);
  temp_len= (strmov(strmov(temp_user, user)+1, host) - temp_user)+1;
  mysql_mutex_lock(&LOCK_user_conn);
  if (!(uc = (struct  user_conn *) my_hash_search(&hash_user_connections,
					       (uchar*) temp_user, temp_len)))
  {
    /* First connection for user; Create a user connection object */
    if (!(uc= ((struct user_conn*)
	       my_malloc(sizeof(struct user_conn) + temp_len+1,
			 MYF(MY_WME)))))
    {
      /* MY_WME ensures an error is set in THD. */
      return_val= 1;
      goto end;
    }
    uc->user=(char*) (uc+1);
    memcpy(uc->user,temp_user,temp_len+1);
    uc->host= uc->user + user_len +  1;
    uc->len= (uint)temp_len;
    uc->connections= uc->questions= uc->updates= uc->conn_per_hour= 0;
    uc->user_resources= *mqh;
    uc->reset_utime= thd->thr_create_utime;
    if (my_hash_insert(&hash_user_connections, (uchar*) uc))
    {
      /* The only possible error is out of memory, MY_WME sets an error. */
      my_free(uc);
      return_val= 1;
      goto end;
    }
  }
  thd->user_connect=uc;
  uc->connections++;
end:
  mysql_mutex_unlock(&LOCK_user_conn);
  return return_val;
}


/*
  check if user has already too many connections
  
  SYNOPSIS
  check_for_max_user_connections()
  thd			Thread handle
  uc			User connect object

  NOTES
    If check fails, we decrease user connection count, which means one
    shouldn't call decrease_user_connections() after this function.

  RETURN
    0	ok
    1	error
*/

int check_for_max_user_connections(THD *thd, USER_CONN *uc)
{
  int error= 1;
  Host_errors errors;
  DBUG_ENTER("check_for_max_user_connections");

  mysql_mutex_lock(&LOCK_user_conn);

  /* Root is not affected by the value of max_user_connections */
  if (global_system_variables.max_user_connections &&
      !uc->user_resources.user_conn &&
      global_system_variables.max_user_connections < uc->connections &&
      !(thd->security_ctx->master_access & SUPER_ACL))
  {
    my_error(ER_TOO_MANY_USER_CONNECTIONS, MYF(0), uc->user);
    error=1;
    errors.m_max_user_connection= 1;
    goto end;
  }
  time_out_user_resource_limits(thd, uc);
  if (uc->user_resources.user_conn &&
      uc->user_resources.user_conn < uc->connections)
  {
    my_error(ER_USER_LIMIT_REACHED, MYF(0), uc->user,
             "max_user_connections",
             (long) uc->user_resources.user_conn);
    error= 1;
    errors.m_max_user_connection= 1;
    goto end;
  }
  if (uc->user_resources.conn_per_hour &&
      uc->user_resources.conn_per_hour <= uc->conn_per_hour)
  {
    my_error(ER_USER_LIMIT_REACHED, MYF(0), uc->user,
             "max_connections_per_hour",
             (long) uc->user_resources.conn_per_hour);
    error=1;
    errors.m_max_user_connection_per_hour= 1;
    goto end;
  }
  uc->conn_per_hour++;
  error= 0;

end:
  if (unlikely(error))
  {
    uc->connections--; // no need for decrease_user_connections() here
    /*
      The thread may returned back to the pool and assigned to a user
      that doesn't have a limit. Ensure the user is not using resources
      of someone else.
    */
    thd->user_connect= NULL;
  }
  mysql_mutex_unlock(&LOCK_user_conn);
  if (unlikely(error))
  {
    inc_host_errors(thd->main_security_ctx.ip, &errors);
  }
  DBUG_RETURN(error);
}


/*
  Decrease user connection count

  SYNOPSIS
    decrease_user_connections()
    uc			User connection object

  NOTES
    If there is a n user connection object for a connection
    (which only happens if 'max_user_connections' is defined or
    if someone has created a resource grant for a user), then
    the connection count is always incremented on connect.

    The user connect object is not freed if some users has
    'max connections per hour' defined as we need to be able to hold
    count over the lifetime of the connection.
*/

void decrease_user_connections(USER_CONN *uc)
{
  DBUG_ENTER("decrease_user_connections");
  mysql_mutex_lock(&LOCK_user_conn);
  DBUG_ASSERT(uc->connections);
  if (!--uc->connections && !mqh_used)
  {
    /* Last connection for user; Delete it */
    (void) my_hash_delete(&hash_user_connections,(uchar*) uc);
  }
  mysql_mutex_unlock(&LOCK_user_conn);
  DBUG_VOID_RETURN;
}


/*
  Reset per-hour user resource limits when it has been more than
  an hour since they were last checked

  SYNOPSIS:
    time_out_user_resource_limits()
    thd			Thread handler
    uc			User connection details

  NOTE:
    This assumes that the LOCK_user_conn mutex has been acquired, so it is
    safe to test and modify members of the USER_CONN structure.
*/

void time_out_user_resource_limits(THD *thd, USER_CONN *uc)
{
  ulonglong check_time= thd->start_utime;
  DBUG_ENTER("time_out_user_resource_limits");

  /* If more than a hour since last check, reset resource checking */
  if (check_time  - uc->reset_utime >= 3600000000ULL)
  {
    uc->questions=0;
    uc->updates=0;
    uc->conn_per_hour=0;
    uc->reset_utime= check_time;
  }

  DBUG_VOID_RETURN;
}

/*
  Check if maximum queries per hour limit has been reached
  returns 0 if OK.
*/

bool check_mqh(THD *thd, uint check_command)
{
  bool error= 0;
  USER_CONN *uc=thd->user_connect;
  DBUG_ENTER("check_mqh");
  DBUG_ASSERT(uc != 0);

  mysql_mutex_lock(&LOCK_user_conn);

  time_out_user_resource_limits(thd, uc);

  /* Check that we have not done too many questions / hour */
  if (uc->user_resources.questions &&
      uc->questions++ >= uc->user_resources.questions)
  {
    my_error(ER_USER_LIMIT_REACHED, MYF(0), uc->user, "max_queries_per_hour",
             (long) uc->user_resources.questions);
    error=1;
    goto end;
  }
  if (check_command < (uint) SQLCOM_END)
  {
    /* Check that we have not done too many updates / hour */
    if (uc->user_resources.updates &&
        (sql_command_flags[check_command] & CF_CHANGES_DATA) &&
	uc->updates++ >= uc->user_resources.updates)
    {
      my_error(ER_USER_LIMIT_REACHED, MYF(0), uc->user, "max_updates_per_hour",
               (long) uc->user_resources.updates);
      error=1;
      goto end;
    }
  }
end:
  mysql_mutex_unlock(&LOCK_user_conn);
  DBUG_RETURN(error);
}

#endif /* NO_EMBEDDED_ACCESS_CHECKS */

/*
  Check for maximum allowable user connections, if the mysqld server is
  started with corresponding variable that is greater then 0.
*/

extern "C" uchar *get_key_conn(user_conn *buff, size_t *length,
			      my_bool not_used __attribute__((unused)))
{
  *length= buff->len;
  return (uchar*) buff->user;
}


extern "C" void free_user(struct user_conn *uc)
{
  my_free(uc);
}


void init_max_user_conn(void)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  my_hash_init(&hash_user_connections, system_charset_info, max_connections,
               0, 0, (my_hash_get_key) get_key_conn,
               (my_hash_free_key) free_user, 0);
#endif
}


void free_max_user_conn(void)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  my_hash_free(&hash_user_connections);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
}


void reset_mqh(LEX_USER *lu, bool get_them= 0)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  mysql_mutex_lock(&LOCK_user_conn);
  if (lu)  // for GRANT
  {
    USER_CONN *uc;
    size_t temp_len=lu->user.length+lu->host.length+2;
    char temp_user[USER_HOST_BUFF_SIZE];

    memcpy(temp_user,lu->user.str,lu->user.length);
    memcpy(temp_user+lu->user.length+1,lu->host.str,lu->host.length);
    temp_user[lu->user.length]='\0'; temp_user[temp_len-1]=0;
    if ((uc = (struct  user_conn *) my_hash_search(&hash_user_connections,
                                                   (uchar*) temp_user,
                                                   temp_len)))
    {
      uc->questions=0;
      get_mqh(temp_user,&temp_user[lu->user.length+1],uc);
      uc->updates=0;
      uc->conn_per_hour=0;
    }
  }
  else
  {
    /* for FLUSH PRIVILEGES and FLUSH USER_RESOURCES */
    for (uint idx=0;idx < hash_user_connections.records; idx++)
    {
      USER_CONN *uc=(struct user_conn *)
        my_hash_element(&hash_user_connections, idx);
      if (get_them)
	get_mqh(uc->user,uc->host,uc);
      uc->questions=0;
      uc->updates=0;
      uc->conn_per_hour=0;
    }
  }
  mysql_mutex_unlock(&LOCK_user_conn);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
}

/*****************************************************************************
 Handle users statistics
*****************************************************************************/

/* 'mysql_system_user' is used for when the user is not defined for a THD. */
static const char mysql_system_user[]= "#mysql_system#";

// Returns 'user' if it's not NULL.  Returns 'mysql_system_user' otherwise.
static const char * get_valid_user_string(const char* user)
{
  return user ? user : mysql_system_user;
}

/*
  Returns string as 'IP' for the client-side of the connection represented by
  'client'. Does not allocate memory. May return "".
*/

static const char *get_client_host(THD *client)
{
  return client->security_ctx->host_or_ip[0] ?
    client->security_ctx->host_or_ip :
    client->security_ctx->host ? client->security_ctx->host : "";
}

extern "C" uchar *get_key_user_stats(USER_STATS *user_stats, size_t *length,
                                     my_bool not_used __attribute__((unused)))
{
  *length= user_stats->user_name_length;
  return (uchar*) user_stats->user;
}

void free_user_stats(USER_STATS* user_stats)
{
  my_free(user_stats);
}

void init_user_stats(USER_STATS *user_stats,
                     const char *user,
                     size_t user_length,
                     const char *priv_user,
                     uint total_connections,
                     uint total_ssl_connections,
                     uint concurrent_connections,
                     time_t connected_time,
                     double busy_time,
                     double cpu_time,
                     ulonglong bytes_received,
                     ulonglong bytes_sent,
                     ulonglong binlog_bytes_written,
                     ha_rows rows_sent,
                     ha_rows rows_read,
                     ha_rows rows_inserted,
                     ha_rows rows_deleted,
                     ha_rows rows_updated,
                     ulonglong select_commands,
                     ulonglong update_commands,
                     ulonglong other_commands,
                     ulonglong commit_trans,
                     ulonglong rollback_trans,
                     ulonglong denied_connections,
                     ulonglong lost_connections,
                     ulonglong max_statement_time_exceeded,
                     ulonglong access_denied_errors,
                     ulonglong empty_queries)
{
  DBUG_ENTER("init_user_stats");
  DBUG_PRINT("enter", ("user: %s  priv_user: %s", user, priv_user));

  user_length= MY_MIN(user_length, sizeof(user_stats->user)-1);
  memcpy(user_stats->user, user, user_length);
  user_stats->user[user_length]= 0;
  user_stats->user_name_length= (uint)user_length;
  strmake_buf(user_stats->priv_user, priv_user);

  user_stats->total_connections= total_connections;
  user_stats->total_ssl_connections=  total_ssl_connections;
  user_stats->concurrent_connections= concurrent_connections;
  user_stats->connected_time= connected_time;
  user_stats->busy_time= busy_time;
  user_stats->cpu_time= cpu_time;
  user_stats->bytes_received= bytes_received;
  user_stats->bytes_sent= bytes_sent;
  user_stats->binlog_bytes_written= binlog_bytes_written;
  user_stats->rows_sent= rows_sent;
  user_stats->rows_read= rows_read;
  user_stats->rows_inserted= rows_inserted;
  user_stats->rows_deleted= rows_deleted;
  user_stats->rows_updated= rows_updated;
  user_stats->select_commands= select_commands;
  user_stats->update_commands= update_commands;
  user_stats->other_commands= other_commands;
  user_stats->commit_trans= commit_trans;
  user_stats->rollback_trans= rollback_trans;
  user_stats->denied_connections= denied_connections;
  user_stats->lost_connections= lost_connections;
  user_stats->max_statement_time_exceeded= max_statement_time_exceeded;
  user_stats->access_denied_errors= access_denied_errors;
  user_stats->empty_queries= empty_queries;
  DBUG_VOID_RETURN;
}


void init_global_user_stats(void)
{
  my_hash_init(&global_user_stats, system_charset_info, max_connections,
               0, 0, (my_hash_get_key) get_key_user_stats,
               (my_hash_free_key) free_user_stats, 0);
}

void init_global_client_stats(void)
{
  my_hash_init(&global_client_stats, system_charset_info, max_connections,
               0, 0, (my_hash_get_key) get_key_user_stats,
               (my_hash_free_key) free_user_stats, 0);
}

extern "C" uchar *get_key_table_stats(TABLE_STATS *table_stats, size_t *length,
                                      my_bool not_used __attribute__((unused)))
{
  *length= table_stats->table_name_length;
  return (uchar*) table_stats->table;
}

extern "C" void free_table_stats(TABLE_STATS* table_stats)
{
  my_free(table_stats);
}

void init_global_table_stats(void)
{
  my_hash_init(&global_table_stats, system_charset_info, max_connections,
               0, 0, (my_hash_get_key) get_key_table_stats,
               (my_hash_free_key) free_table_stats, 0);
}

extern "C" uchar *get_key_index_stats(INDEX_STATS *index_stats, size_t *length,
                                     my_bool not_used __attribute__((unused)))
{
  *length= index_stats->index_name_length;
  return (uchar*) index_stats->index;
}

extern "C" void free_index_stats(INDEX_STATS* index_stats)
{
  my_free(index_stats);
}

void init_global_index_stats(void)
{
  my_hash_init(&global_index_stats, system_charset_info, max_connections,
               0, 0, (my_hash_get_key) get_key_index_stats,
               (my_hash_free_key) free_index_stats, 0);
}


void free_global_user_stats(void)
{
  my_hash_free(&global_user_stats);
}

void free_global_table_stats(void)
{
  my_hash_free(&global_table_stats);
}

void free_global_index_stats(void)
{
  my_hash_free(&global_index_stats);
}

void free_global_client_stats(void)
{
  my_hash_free(&global_client_stats);
}

/*
  Increments the global stats connection count for an entry from
  global_client_stats or global_user_stats. Returns 0 on success
  and 1 on error.
*/

static bool increment_count_by_name(const char *name, size_t name_length,
                                   const char *role_name,
                                   HASH *users_or_clients, THD *thd)
{
  USER_STATS *user_stats;

  if (!(user_stats= (USER_STATS*) my_hash_search(users_or_clients, (uchar*) name,
                                              name_length)))
  {
    /* First connection for this user or client */
    if (!(user_stats= ((USER_STATS*)
                       my_malloc(sizeof(USER_STATS),
                                 MYF(MY_WME | MY_ZEROFILL)))))
      return TRUE;                              // Out of memory

    init_user_stats(user_stats, name, name_length, role_name,
                    0, 0, 0,   // connections
                    0, 0, 0,   // time
                    0, 0, 0,   // bytes sent, received and written
                    0, 0,      // rows sent and read
                    0, 0, 0,   // rows inserted, deleted and updated
                    0, 0, 0,   // select, update and other commands
                    0, 0,      // commit and rollback trans
                    thd->status_var.access_denied_errors,
                    0,         // lost connections
                    0,         // max query timeouts
                    0,         // access denied errors
                    0);        // empty queries

    if (my_hash_insert(users_or_clients, (uchar*)user_stats))
    {
      my_free(user_stats);
      return TRUE;                              // Out of memory
    }
  }
  user_stats->total_connections++;
  if (thd->net.vio && thd->net.vio->type == VIO_TYPE_SSL)
    user_stats->total_ssl_connections++;
  return FALSE;
}


/*
  Increments the global user and client stats connection count.

  @param use_lock  if true, LOCK_global_user_client_stats will be locked

  @retval 0 ok
  @retval 1 error.
*/

#ifndef EMBEDDED_LIBRARY
static bool increment_connection_count(THD* thd, bool use_lock)
{
  const char *user_string= get_valid_user_string(thd->main_security_ctx.user);
  const char *client_string= get_client_host(thd);
  bool return_value= FALSE;

  if (!thd->userstat_running)
    return FALSE;

  if (use_lock)
    mysql_mutex_lock(&LOCK_global_user_client_stats);

  if (increment_count_by_name(user_string, strlen(user_string), user_string,
                              &global_user_stats, thd))
  {
    return_value= TRUE;
    goto end;
  }
  if (increment_count_by_name(client_string, strlen(client_string),
                              user_string, &global_client_stats, thd))
  {
    return_value= TRUE;
    goto end;
  }

end:
  if (use_lock)
    mysql_mutex_unlock(&LOCK_global_user_client_stats);
  return return_value;
}
#endif

/*
  Used to update the global user and client stats
*/

static void update_global_user_stats_with_user(THD *thd,
                                               USER_STATS *user_stats,
                                               time_t now)
{
  DBUG_ASSERT(thd->userstat_running);

  user_stats->connected_time+= now - thd->last_global_update_time;
  user_stats->busy_time+=  (thd->status_var.busy_time -
                            thd->org_status_var.busy_time);
  user_stats->cpu_time+=   (thd->status_var.cpu_time -
                            thd->org_status_var.cpu_time); 
  /*
    This is handle specially as bytes_received is incremented BEFORE
    org_status_var is copied.
  */
  user_stats->bytes_received+= (thd->org_status_var.bytes_received-
                                thd->start_bytes_received);
  user_stats->bytes_sent+= (thd->status_var.bytes_sent -
                            thd->org_status_var.bytes_sent);
  user_stats->binlog_bytes_written+=
    (thd->status_var.binlog_bytes_written -
     thd->org_status_var.binlog_bytes_written);
  /* We are not counting rows in internal temporary tables here ! */
  user_stats->rows_read+=      (thd->status_var.rows_read -
                                thd->org_status_var.rows_read);
  user_stats->rows_sent+=      (thd->status_var.rows_sent -
                                thd->org_status_var.rows_sent);
  user_stats->rows_inserted+=  (thd->status_var.ha_write_count -
                                thd->org_status_var.ha_write_count);
  user_stats->rows_deleted+=   (thd->status_var.ha_delete_count -
                                thd->org_status_var.ha_delete_count);
  user_stats->rows_updated+=   (thd->status_var.ha_update_count -
                                thd->org_status_var.ha_update_count);
  user_stats->select_commands+= thd->select_commands;
  user_stats->update_commands+= thd->update_commands;
  user_stats->other_commands+=  thd->other_commands;
  user_stats->commit_trans+=   (thd->status_var.ha_commit_count -
                                thd->org_status_var.ha_commit_count);
  user_stats->rollback_trans+= (thd->status_var.ha_rollback_count +
                                thd->status_var.ha_savepoint_rollback_count -
                                thd->org_status_var.ha_rollback_count -
                                thd->org_status_var.
                                ha_savepoint_rollback_count);
  user_stats->access_denied_errors+=
    (thd->status_var.access_denied_errors -
     thd->org_status_var.access_denied_errors);
  user_stats->empty_queries+=   (thd->status_var.empty_queries -
                                 thd->org_status_var.empty_queries);

  /* The following can only contain 0 or 1 and then connection ends */
  user_stats->denied_connections+= thd->status_var.access_denied_errors;
  user_stats->lost_connections+=   thd->status_var.lost_connections;
  user_stats->max_statement_time_exceeded+= thd->status_var.max_statement_time_exceeded;
}


/*  Updates the global stats of a user or client */
void update_global_user_stats(THD *thd, bool create_user, time_t now)
{
  const char *user_string, *client_string;
  USER_STATS *user_stats;
  size_t user_string_length, client_string_length;
  DBUG_ASSERT(thd->userstat_running);

  user_string= get_valid_user_string(thd->main_security_ctx.user);
  user_string_length= strlen(user_string);
  client_string= get_client_host(thd);
  client_string_length= strlen(client_string);

  mysql_mutex_lock(&LOCK_global_user_client_stats);

  // Update by user name
  if ((user_stats= (USER_STATS*) my_hash_search(&global_user_stats,
                                             (uchar*) user_string,
                                             user_string_length)))
  {
    /* Found user. */
    update_global_user_stats_with_user(thd, user_stats, now);
  }
  else
  {
    /* Create the entry */
    if (create_user)
    {
      increment_count_by_name(user_string, user_string_length, user_string,
                              &global_user_stats, thd);
    }
  }

  /* Update by client IP */
  if ((user_stats= (USER_STATS*)my_hash_search(&global_client_stats,
                                            (uchar*) client_string,
                                            client_string_length)))
  {
    // Found by client IP
    update_global_user_stats_with_user(thd, user_stats, now);
  }
  else
  {
    // Create the entry
    if (create_user)
    {
      increment_count_by_name(client_string, client_string_length,
                              user_string, &global_client_stats, thd);
    }
  }
  /* Reset variables only used for counting */
  thd->select_commands= thd->update_commands= thd->other_commands= 0;
  thd->last_global_update_time= now;

  mysql_mutex_unlock(&LOCK_global_user_client_stats);
}


/**
  Set thread character set variables from the given ID

  @param  thd         thread handle
  @param  cs_number   character set and collation ID

  @retval  0  OK; character_set_client, collation_connection and
              character_set_results are set to the new value,
              or to the default global values.

  @retval  1  error, e.g. the given ID is not supported by parser.
              Corresponding SQL error is sent.
*/

bool thd_init_client_charset(THD *thd, uint cs_number)
{
  CHARSET_INFO *cs;
  /*
   Use server character set and collation if
   - opt_character_set_client_handshake is not set
   - client has not specified a character set
   - client character set doesn't exists in server
  */
  if (!opt_character_set_client_handshake ||
      !(cs= get_charset(cs_number, MYF(0))))
  {
    thd->update_charset(global_system_variables.character_set_client,
                        global_system_variables.collation_connection,
                        global_system_variables.character_set_results);
  }
  else
  {
    if (!is_supported_parser_charset(cs))
    {
      /* Disallow non-supported parser character sets: UCS2, UTF16, UTF32 */
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "character_set_client",
               cs->csname);
      return true;
    }
    thd->update_charset(cs,cs,cs);
  }
  return false;
}


/*
  Initialize connection threads
*/

#ifndef EMBEDDED_LIBRARY
bool init_new_connection_handler_thread()
{
  pthread_detach_this_thread();
  if (my_thread_init())
  {
    statistic_increment(aborted_connects,&LOCK_status);
    statistic_increment(connection_errors_internal, &LOCK_status);
    return 1;
  }
  DBUG_EXECUTE_IF("simulate_failed_connection_1", return(1); );
  return 0;
}

/**
  Set client address during authentication.

  Initializes THD::main_security_ctx and THD::peer_port.
  Optionally does ip to hostname translation.

  @param thd   current THD handle
  @param addr  peer address (can be NULL, if 'ip' is set)
  @param ip    peer address as string (can be NULL if 'addr' is set)
  @param port  peer port
  @param check_proxy_networks if true, and host is in
               'proxy_protocol_networks' list, skip
               "host not privileged" check
  @param[out] host_errors - number of connect
              errors for this host

  @retval 0 ok, 1 error
*/
int thd_set_peer_addr(THD *thd,
  sockaddr_storage *addr,
  const char *ip,
  uint port,
  bool check_proxy_networks,
  uint *host_errors)
{
  *host_errors= 0;

  thd->peer_port= port;

  char ip_string[128];
  if (!ip)
  {
    void *addr_data;
    if (addr->ss_family == AF_UNIX)
    {
        /* local connection */
        my_free((void *)thd->main_security_ctx.ip);
        thd->main_security_ctx.host_or_ip= thd->main_security_ctx.host = my_localhost;
        thd->main_security_ctx.ip= 0;
        return 0;
    }
    else if (addr->ss_family == AF_INET)
      addr_data= &((struct sockaddr_in *)addr)->sin_addr;
    else
      addr_data= &((struct sockaddr_in6 *)addr)->sin6_addr;
    if (!inet_ntop(addr->ss_family,addr_data, ip_string, sizeof(ip_string)))
    {
      DBUG_ASSERT(0);
      return 1;
    }
    ip= ip_string;
  }

  my_free((void *)thd->main_security_ctx.ip);
  if (!(thd->main_security_ctx.ip = my_strdup(ip, MYF(MY_WME))))
  {
    /*
    No error accounting per IP in host_cache,
    this is treated as a global server OOM error.
    TODO: remove the need for my_strdup.
    */
    statistic_increment(aborted_connects, &LOCK_status);
    statistic_increment(connection_errors_internal, &LOCK_status);
    return 1; /* The error is set by my_strdup(). */
  }
  thd->main_security_ctx.host_or_ip = thd->main_security_ctx.ip;
  if (!(specialflag & SPECIAL_NO_RESOLVE))
  {
    int rc;

    rc = ip_to_hostname(addr,
      thd->main_security_ctx.ip,
      &thd->main_security_ctx.host,
      host_errors);

    /* Cut very long hostnames to avoid possible overflows */
    if (thd->main_security_ctx.host)
    {
      if (thd->main_security_ctx.host != my_localhost)
        ((char*)thd->main_security_ctx.host)[MY_MIN(strlen(thd->main_security_ctx.host),
          HOSTNAME_LENGTH)] = 0;
      thd->main_security_ctx.host_or_ip = thd->main_security_ctx.host;
    }

    if (rc == RC_BLOCKED_HOST)
    {
      /* HOST_CACHE stats updated by ip_to_hostname(). */
      my_error(ER_HOST_IS_BLOCKED, MYF(0), thd->main_security_ctx.host_or_ip);
      return 1;
    }
  }
  DBUG_PRINT("info", ("Host: %s  ip: %s",
    (thd->main_security_ctx.host ?
      thd->main_security_ctx.host : "unknown host"),
      (thd->main_security_ctx.ip ?
        thd->main_security_ctx.ip : "unknown ip")));
  if ((!check_proxy_networks || !is_proxy_protocol_allowed((struct sockaddr *) addr)) 
      && acl_check_host(thd->main_security_ctx.host, thd->main_security_ctx.ip))
  {
    /* HOST_CACHE stats updated by acl_check_host(). */
    my_error(ER_HOST_NOT_PRIVILEGED, MYF(0),
      thd->main_security_ctx.host_or_ip);
    return 1;
  }
  return 0;
}

/*
  Perform handshake, authorize client and update thd ACL variables.

  SYNOPSIS
    check_connection()
    thd  thread handle

  RETURN
     0  success, thd is updated.
     1  error
*/

static int check_connection(THD *thd)
{
  uint connect_errors= 0;
  int auth_rc;
  NET *net= &thd->net;

  DBUG_PRINT("info",
             ("New connection received on %s", vio_description(net->vio)));

#ifdef SIGNAL_WITH_VIO_CLOSE
  thd->set_active_vio(net->vio);
#endif

  if (!thd->main_security_ctx.host)         // If TCP/IP connection
  {
    my_bool peer_rc;
    char ip[NI_MAXHOST];
    uint16 peer_port;

    peer_rc= vio_peer_addr(net->vio, ip, &peer_port, NI_MAXHOST);

    /*
    ===========================================================================
    DEBUG code only (begin)
    Simulate various output from vio_peer_addr().
    ===========================================================================
    */

    DBUG_EXECUTE_IF("vio_peer_addr_error",
                    {
                      peer_rc= 1;
                    }
                    );
    DBUG_EXECUTE_IF("vio_peer_addr_fake_ipv4",
                    {
                      struct sockaddr *sa= (sockaddr *) &net->vio->remote;
                      sa->sa_family= AF_INET;
                      struct in_addr *ip4= &((struct sockaddr_in *) sa)->sin_addr;
                      /* See RFC 5737, 192.0.2.0/24 is reserved. */
                      const char* fake= "192.0.2.4";
                      inet_pton(AF_INET,fake, ip4);
                      strcpy(ip, fake);
                      peer_rc= 0;
                    }
                    );

#ifdef HAVE_IPV6
    DBUG_EXECUTE_IF("vio_peer_addr_fake_ipv6",
                    {
                      struct sockaddr_in6 *sa= (sockaddr_in6 *) &net->vio->remote;
                      sa->sin6_family= AF_INET6;
                      struct in6_addr *ip6= & sa->sin6_addr;
                      /* See RFC 3849, ipv6 2001:DB8::/32 is reserved. */
                      const char* fake= "2001:db8::6:6";
                      /* inet_pton(AF_INET6, fake, ip6); not available on Windows XP. */
                      ip6->s6_addr[ 0] = 0x20;
                      ip6->s6_addr[ 1] = 0x01;
                      ip6->s6_addr[ 2] = 0x0d;
                      ip6->s6_addr[ 3] = 0xb8;
                      ip6->s6_addr[ 4] = 0x00;
                      ip6->s6_addr[ 5] = 0x00;
                      ip6->s6_addr[ 6] = 0x00;
                      ip6->s6_addr[ 7] = 0x00;
                      ip6->s6_addr[ 8] = 0x00;
                      ip6->s6_addr[ 9] = 0x00;
                      ip6->s6_addr[10] = 0x00;
                      ip6->s6_addr[11] = 0x00;
                      ip6->s6_addr[12] = 0x00;
                      ip6->s6_addr[13] = 0x06;
                      ip6->s6_addr[14] = 0x00;
                      ip6->s6_addr[15] = 0x06;
                      strcpy(ip, fake);
                      peer_rc= 0;
                    }
                    );
#endif /* HAVE_IPV6 */

    /*
    ===========================================================================
    DEBUG code only (end)
    ===========================================================================
    */

    if (peer_rc)
    {
      /*
        Since we can not even get the peer IP address,
        there is nothing to show in the host_cache,
        so increment the global status variable for peer address errors.
      */
      statistic_increment(connection_errors_peer_addr, &LOCK_status);
      my_error(ER_BAD_HOST_ERROR, MYF(0));
      return 1;
    }

    if (thd_set_peer_addr(thd, &net->vio->remote, ip, peer_port,
                          true, &connect_errors))
      return 1;
  }
  else /* Hostname given means that the connection was on a socket */
  {
    DBUG_PRINT("info",("Host: %s", thd->main_security_ctx.host));
    thd->main_security_ctx.host_or_ip= thd->main_security_ctx.host;
    thd->main_security_ctx.ip= 0;
    /* Reset sin_addr */
    bzero((char*) &net->vio->remote, sizeof(net->vio->remote));
  }
  vio_keepalive(net->vio, TRUE);
  vio_set_keepalive_options(net->vio, &opt_vio_keepalive);

  if (unlikely(thd->packet.alloc(thd->variables.net_buffer_length)))
  {
    /*
      Important note:
      net_buffer_length is a SESSION variable,
      so it may be tempting to account OOM conditions per IP in the HOST_CACHE,
      in case some clients are more demanding than others ...
      However, this session variable is *not* initialized with a per client
      value during the initial connection, it is initialized from the
      GLOBAL net_buffer_length variable from the server.
      Hence, there is no reason to account on OOM conditions per client IP,
      we count failures in the global server status instead.
    */
    statistic_increment(aborted_connects,&LOCK_status);
    statistic_increment(connection_errors_internal, &LOCK_status);
    return 1; /* The error is set by alloc(). */
  }

  auth_rc= acl_authenticate(thd, 0);
  if (auth_rc == 0 && connect_errors != 0)
  {
    /*
      A client connection from this IP was successful,
      after some previous failures.
      Reset the connection error counter.
    */
    reset_host_connect_errors(thd->main_security_ctx.ip);
  }

  return auth_rc;
}


/*
  Setup thread to be used with the current thread

  SYNOPSIS
    bool setup_connection_thread_globals()
    thd    Thread/connection handler

  RETURN
    0   ok
    1   Error (out of memory)
        In this case we will close the connection and increment status
*/

bool setup_connection_thread_globals(THD *thd)
{
  if (thd->store_globals())
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    statistic_increment(aborted_connects,&LOCK_status);
    statistic_increment(connection_errors_internal, &LOCK_status);
    thd->scheduler->end_thread(thd, 0);
    return 1;                                   // Error
  }
  return 0;
}


/*
  Autenticate user, with error reporting

  SYNOPSIS
   login_connection()
   thd        Thread handler

  NOTES
    Connection is not closed in case of errors

  RETURN
    0    ok
    1    error
*/

bool login_connection(THD *thd)
{
  NET *net= &thd->net;
  int error= 0;
  DBUG_ENTER("login_connection");
  DBUG_PRINT("info", ("login_connection called by thread %lu",
                      (ulong) thd->thread_id));

  /* Use "connect_timeout" value during connection phase */
  my_net_set_read_timeout(net, connect_timeout);
  my_net_set_write_timeout(net, connect_timeout);

  error= check_connection(thd);
  thd->protocol->end_statement();

  if (unlikely(error))
  {						// Wrong permissions
#ifdef _WIN32
    if (vio_type(net->vio) == VIO_TYPE_NAMEDPIPE)
      my_sleep(1000);				/* must wait after eof() */
#endif
    statistic_increment(aborted_connects,&LOCK_status);
    error=1;
    goto exit;
  }
  /* Connect completed, set read/write timeouts back to default */
  my_net_set_read_timeout(net, thd->variables.net_read_timeout);
  my_net_set_write_timeout(net, thd->variables.net_write_timeout);

  /*  Updates global user connection stats. */
  if (increment_connection_count(thd, TRUE))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int) (2*sizeof(USER_STATS)));
    error= 1;
    goto exit;
  }

exit:
  mysql_audit_notify_connection_connect(thd);
  DBUG_RETURN(error);
}


/*
  Close an established connection

  NOTES
    This mainly updates status variables
*/

void end_connection(THD *thd)
{
  NET *net= &thd->net;
#ifdef WITH_WSREP
  if (WSREP(thd))
  {
    wsrep_status_t rcode= wsrep->free_connection(wsrep, thd->thread_id);
    if (rcode) {
      WSREP_WARN("wsrep failed to free connection context: %lld  code: %d",
                 (longlong) thd->thread_id, rcode);
    }
  }
  thd->wsrep_client_thread= 0;
#endif
  plugin_thdvar_cleanup(thd);

  if (thd->user_connect)
  {
    /*
      We decrease this variable early to make it easy to log again quickly.
      This code is not critical as we will in any case do this test
      again in thd->cleanup()
    */
    decrease_user_connections(thd->user_connect);
    /*
      The thread may returned back to the pool and assigned to a user
      that doesn't have a limit. Ensure the user is not using resources
      of someone else.
    */
    thd->user_connect= NULL;
  }

  if (unlikely(thd->killed) || (net->error && net->vio != 0))
  {
    statistic_increment(aborted_threads,&LOCK_status);
    status_var_increment(thd->status_var.lost_connections);
  }

  if (likely(!thd->killed) && (net->error && net->vio != 0))
    thd->print_aborted_warning(1, thd->get_stmt_da()->is_error()
             ? thd->get_stmt_da()->message() : ER_THD(thd, ER_UNKNOWN_ERROR));
}


/*
  Initialize THD to handle queries
*/

void prepare_new_connection_state(THD* thd)
{
  Security_context *sctx= thd->security_ctx;

  if (thd->client_capabilities & CLIENT_COMPRESS)
    thd->net.compress=1;				// Use compression

  /*
    Much of this is duplicated in create_embedded_thd() for the
    embedded server library.
    TODO: refactor this to avoid code duplication there
  */
  thd->proc_info= 0;
  thd->set_command(COM_SLEEP);
  thd->init_for_queries();

  if (opt_init_connect.length && !(sctx->master_access & SUPER_ACL))
  {
    execute_init_command(thd, &opt_init_connect, &LOCK_sys_init_connect);
    if (unlikely(thd->is_error()))
    {
      Host_errors errors;
      thd->set_killed(KILL_CONNECTION);
      thd->print_aborted_warning(0, "init_connect command failed");
      sql_print_warning("%s", thd->get_stmt_da()->message());

      /*
        now let client to send its first command,
        to be able to send the error back
      */
      NET *net= &thd->net;
      thd->lex->current_select= 0;
      my_net_set_read_timeout(net, thd->variables.net_wait_timeout);
      thd->clear_error();
      net_new_transaction(net);
      ulong packet_length= my_net_read(net);
      /*
        If my_net_read() failed, my_error() has been already called,
        and the main Diagnostics Area contains an error condition.
      */
      if (packet_length != packet_error)
        my_error(ER_NEW_ABORTING_CONNECTION, MYF(0),
                 thd->thread_id,
                 thd->db.str ? thd->db.str : "unconnected",
                 sctx->user ? sctx->user : "unauthenticated",
                 sctx->host_or_ip, "init_connect command failed");
      thd->server_status&= ~SERVER_STATUS_CLEAR_SET;
      thd->protocol->end_statement();
      thd->killed = KILL_CONNECTION;
      errors.m_init_connect= 1;
      inc_host_errors(thd->main_security_ctx.ip, &errors);
      return;
    }

    thd->proc_info=0;
    thd->init_for_queries();
  }
}


/*
  Thread handler for a connection

  SYNOPSIS
    handle_one_connection()
    arg		Connection object (THD)

  IMPLEMENTATION
    This function (normally) does the following:
    - Initialize thread
    - Initialize THD to be used with this thread
    - Authenticate user
    - Execute all queries sent on the connection
    - Take connection down
    - End thread  / Handle next connection using thread from thread cache
*/

pthread_handler_t handle_one_connection(void *arg)
{
  CONNECT *connect= (CONNECT*) arg;

  mysql_thread_set_psi_id(connect->thread_id);

  do_handle_one_connection(connect);
  return 0;
}

bool thd_prepare_connection(THD *thd)
{
  bool rc;
  lex_start(thd);
  rc= login_connection(thd);
  if (rc)
    return rc;

  MYSQL_CONNECTION_START(thd->thread_id, &thd->security_ctx->priv_user[0],
                         (char *) thd->security_ctx->host_or_ip);

  prepare_new_connection_state(thd);
#ifdef WITH_WSREP
  thd->wsrep_client_thread= 1;
#endif /* WITH_WSREP */
  return FALSE;
}

bool thd_is_connection_alive(THD *thd)
{
  NET *net= &thd->net;
  if (likely(!net->error &&
             net->vio != 0 &&
             thd->killed < KILL_CONNECTION))
    return TRUE;
  return FALSE;
}


void do_handle_one_connection(CONNECT *connect)
{
  ulonglong thr_create_utime= microsecond_interval_timer();
  THD *thd;
  if (connect->scheduler->init_new_connection_thread() ||
      !(thd= connect->create_thd(NULL)))
  {
    scheduler_functions *scheduler= connect->scheduler;
    connect->close_with_error(0, 0, ER_OUT_OF_RESOURCES);
    scheduler->end_thread(0, 0);
    return;
  }

  /*
    If a thread was created to handle this connection:
    increment slow_launch_threads counter if it took more than
    slow_launch_time seconds to create the thread.
  */

  if (connect->prior_thr_create_utime)
  {
    ulong launch_time= (ulong) (thr_create_utime -
                                connect->prior_thr_create_utime);
    if (launch_time >= slow_launch_time*1000000L)
      statistic_increment(slow_launch_threads, &LOCK_status);
  }
  delete connect;

  /* Make THD visible in show processlist */
  add_to_active_threads(thd);
  
  thd->thr_create_utime= thr_create_utime;
  /* We need to set this because of time_out_user_resource_limits */
  thd->start_utime= thr_create_utime;

  /*
    handle_one_connection() is normally the only way a thread would
    start and would always be on the very high end of the stack ,
    therefore, the thread stack always starts at the address of the
    first local variable of handle_one_connection, which is thd. We
    need to know the start of the stack so that we could check for
    stack overruns.
  */
  thd->thread_stack= (char*) &thd;
  if (setup_connection_thread_globals(thd))
    return;

  for (;;)
  {
    bool create_user= TRUE;

    mysql_socket_set_thread_owner(thd->net.vio->mysql_socket);
    if (thd_prepare_connection(thd))
    {
      create_user= FALSE;
      goto end_thread;
    }      

    while (thd_is_connection_alive(thd))
    {
      mysql_audit_release(thd);
      if (do_command(thd))
	break;
    }
    end_connection(thd);

#ifdef WITH_WSREP
  if (WSREP(thd))
  {
    mysql_mutex_lock(&thd->LOCK_thd_data);
    thd->wsrep_query_state= QUERY_EXITING;
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
#endif
end_thread:
    close_connection(thd);

    if (thd->userstat_running)
      update_global_user_stats(thd, create_user, time(NULL));

    if (thd->scheduler->end_thread(thd, 1))
      return;                                 // Probably no-threads

    /*
      If end_thread() returns, this thread has been schedule to
      handle the next connection.
    */
    thd= current_thd;
    thd->thread_stack= (char*) &thd;
  }
}
#endif /* EMBEDDED_LIBRARY */


/* Handling of CONNECT objects */

/*
  Close connection without error and delete the connect object
  This and close_with_error are only called if we didn't manage to
  create a new thd object.
*/

void CONNECT::close_and_delete()
{
  DBUG_ENTER("close_and_delete");

  if (vio)
    vio_close(vio);
  if (thread_count_incremented)
    dec_connection_count(scheduler);
  statistic_increment(connection_errors_internal, &LOCK_status);
  statistic_increment(aborted_connects,&LOCK_status);

  delete this;
  DBUG_VOID_RETURN;
}

/*
  Close a connection with a possible error to the end user
  Alse deletes the connection object, like close_and_delete()
*/

void CONNECT::close_with_error(uint sql_errno,
                               const char *message, uint close_error)
{
  THD *thd= create_thd(NULL);
  if (thd)
  {
    if (sql_errno)
      net_send_error(thd, sql_errno, message, NULL);
    close_connection(thd, close_error);
    delete thd;
    set_current_thd(0);
  }
  close_and_delete();
}


CONNECT::~CONNECT()
{
  if (vio)
    vio_delete(vio);
}


/* Reuse or create a THD based on a CONNECT object */

THD *CONNECT::create_thd(THD *thd)
{
  bool res, thd_reused= thd != 0;
  DBUG_ENTER("create_thd");

  DBUG_EXECUTE_IF("simulate_failed_connection_2", DBUG_RETURN(0); );

  if (thd)
  {
    /* reuse old thd */
    thd->reset_for_reuse();
    /*
      reset tread_id's, but not thread_dbug_id's as the later isn't allowed
      to change as there is already structures in thd marked with the old
      value.
    */
    thd->thread_id= thd->variables.pseudo_thread_id= thread_id;
  }
  else if (!(thd= new THD(thread_id)))
    DBUG_RETURN(0);

  set_current_thd(thd);
  res= my_net_init(&thd->net, vio, thd, MYF(MY_THREAD_SPECIFIC));
  vio= 0;                              // Vio now handled by thd

  if (unlikely(res || thd->is_error()))
  {
    if (!thd_reused)
      delete thd;
    set_current_thd(0);
    DBUG_RETURN(0);
  }

  init_net_server_extension(thd);

  thd->security_ctx->host= host;
  thd->extra_port=         extra_port;
  thd->scheduler=          scheduler;
  thd->real_id=            real_id;
  DBUG_RETURN(thd);
}
