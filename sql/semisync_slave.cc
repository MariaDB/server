/* Copyright (c) 2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include <my_global.h>
#include "semisync_slave.h"
#include "debug_sync.h"

Repl_semi_sync_slave repl_semisync_slave;

my_bool global_rpl_semi_sync_slave_enabled= 0;
char rpl_semi_sync_slave_delay_master;
ulong rpl_semi_sync_slave_trace_level;
unsigned int rpl_semi_sync_slave_kill_conn_timeout;
unsigned long long rpl_semi_sync_slave_send_ack = 0;

int Repl_semi_sync_slave::init_object()
{
  int result= 0;

  m_init_done = true;

  /* References to the parameter works after set_options(). */
  set_trace_level(rpl_semi_sync_slave_trace_level);
  set_delay_master(rpl_semi_sync_slave_delay_master);
  set_kill_conn_timeout(rpl_semi_sync_slave_kill_conn_timeout);
  return result;
}

static bool local_semi_sync_enabled;

int rpl_semi_sync_enabled(THD *thd, SHOW_VAR *var, void *buff,
                          system_status_var *status_var,
                          enum_var_type scope)
{
  local_semi_sync_enabled= repl_semisync_slave.get_slave_enabled();
  var->type= SHOW_BOOL;
  var->value= (char*) &local_semi_sync_enabled;
  return 0;
}


int Repl_semi_sync_slave::slave_read_sync_header(const uchar *header,
                                                 unsigned long total_len,
                                                 int  *semi_flags,
                                                 const uchar **payload,
                                                 unsigned long *payload_len)
{
  int read_res = 0;
  DBUG_ENTER("Repl_semi_sync_slave::slave_read_sync_header");

  if (get_slave_enabled())
  {
    if (!DBUG_IF("semislave_corrupt_log")
        && header[0] == k_packet_magic_num)
    {
      bool semi_sync_need_reply  = (header[1] & k_packet_flag_sync);
      *payload_len = total_len - 2;
      *payload     = header + 2;

      DBUG_PRINT("semisync", ("%s: reply - %d",
                              "Repl_semi_sync_slave::slave_read_sync_header",
                              semi_sync_need_reply));

      if (semi_sync_need_reply)
        *semi_flags |= SEMI_SYNC_NEED_ACK;
      if (is_delay_master())
        *semi_flags |= SEMI_SYNC_SLAVE_DELAY_SYNC;
    }
    else
    {
      sql_print_error("Missing magic number for semi-sync packet, packet "
                      "len: %lu", total_len);
      read_res = -1;
    }
  }
  else
  {
    *payload= header;
    *payload_len= total_len;
  }

  DBUG_RETURN(read_res);
}

/*
  Set default semisync variables and print some replication info to the log

  Note that the main setup is done in request_transmit()
*/

void Repl_semi_sync_slave::slave_start(Master_info *mi)
{

  /*
    Set semi_sync_enabled at slave start. This is not changed until next
    slave start or reconnect.
  */
  bool semi_sync= global_rpl_semi_sync_slave_enabled;

  set_slave_enabled(semi_sync);
  mi->semi_sync_reply_enabled= 0;

  sql_print_information("Slave I/O thread: Start %s replication to\
 master '%s@%s:%d' in log '%s' at position %lu",
			semi_sync ? "semi-sync" : "asynchronous",
			const_cast<char *>(mi->user), mi->host, mi->port,
			const_cast<char *>(mi->master_log_name),
                        (unsigned long)(mi->master_log_pos));

  /*clear the counter*/
  rpl_semi_sync_slave_send_ack= 0;
}

void Repl_semi_sync_slave::
  slave_stop(Master_info *mi, Remote_event_stream &mysql)
{
  if (get_slave_enabled())
  {
#ifdef ENABLED_DEBUG_SYNC
  /*
    TODO: Remove after MDEV-28141
  */
  DBUG_EXECUTE_IF("delay_semisync_kill_connection_for_mdev_28141", {
    const char act[]= "now "
                      "signal at_semisync_kill_connection "
                      "wait_for continue_semisync_kill_connection";
    DBUG_ASSERT(debug_sync_service);
    DBUG_ASSERT(!debug_sync_set_action(mi->io_thd, STRING_WITH_LEN(act)));
  };);
#endif
    kill_connection(mi, mysql);
  }

  set_slave_enabled(0);
}

void Repl_semi_sync_slave::slave_reconnect(Master_info *mi)
{
  /*
    Start semi-sync either if it globally enabled or if was enabled
    before the reconnect.
  */
  if (global_rpl_semi_sync_slave_enabled || get_slave_enabled())
    slave_start(mi);
}


void Repl_semi_sync_slave::
  kill_connection(Master_info *mi, Remote_event_stream &mysql)
{
  if (!mysql)
    return;

  char kill_buffer[30];
  bool ret;
  size_t kill_buffer_length;

  auto kill_mysql= Semi_sync_graceful_killer(
    setup_mysql_connection_for_master(mi), m_kill_conn_timeout);
  if (!kill_mysql)
    return;

  ret= kill_mysql.connect();
  if (DBUG_IF("semisync_slave_failed_kill") || ret)
  {
    sql_print_information("cannot connect to master to kill slave io_thread's "
                          "connection");
    return;
  }

  kill_buffer_length= my_snprintf(kill_buffer, 30, "KILL %lu",
                                mysql.thread_id());
  if (kill_mysql.real_query(kill_buffer, kill_buffer_length))
    sql_print_information(
        "Failed to gracefully kill our active semi-sync connection with "
        "primary. Silently closing the connection.");
}

int Repl_semi_sync_slave::
  request_transmit(Master_info *mi, Remote_event_stream &mysql)
{
  MYSQL_RES *res= 0;
  MYSQL_ROW row;
  const char *query;

  if (!get_slave_enabled())
    return 0;

  query= "SHOW VARIABLES LIKE 'rpl_semi_sync_master_enabled'";
  if (mysql.real_query(query, strlen(query)) ||
      !(res= mysql.store_result()))
  {
    sql_print_error("Execution failed on master: %s, error :%s",
      query, mysql.errmsg());
    set_slave_enabled(0);
    return 1;
  }

  row= mysql_fetch_row(res);
  if (DBUG_IF("master_not_support_semisync") || (!row || ! row[1]))
  {
    /* Master does not support semi-sync */
    if (!row)
      sql_print_warning("Master server does not support semi-sync, "
                        "fallback to asynchronous replication");
    set_slave_enabled(0);
    mysql_free_result(res);
    return 0;
  }
  if (strcmp(row[1], "ON"))
    sql_print_information("Slave has semi-sync enabled but master server does "
                          "not. Semi-sync will be activated when master "
                          "enables it");
  mysql_free_result(res);

  /*
   Tell master dump thread that we want to do semi-sync
   replication. This is done by setting a thread local variable in
   the master connection.
  */
  query= "SET @rpl_semi_sync_slave= 1";
  if (mysql.real_query(query, strlen(query)))
  {
    sql_print_error("%s on master failed", query);
    set_slave_enabled(0);
    return 1;
  }
  mi->semi_sync_reply_enabled= 1;
  Remote_event_stream::free_result(mysql.store_result());

  return 0;
}

bool Repl_semi_sync_slave::
  slave_reply(Master_info *mi, Remote_event_stream &mysql)
{
  const char *binlog_filename= const_cast<char *>(mi->master_log_name);
  my_off_t binlog_filepos= mi->master_log_pos;
  DBUG_ENTER("Repl_semi_sync_slave::slave_reply");
  DBUG_ASSERT(get_slave_enabled() && mi->semi_sync_reply_enabled);

  DBUG_PRINT("semisync", ("%s: reply (%s, %lu)",
                          "Repl_semi_sync_slave::slave_reply",
                          binlog_filename, (ulong)binlog_filepos));

  bool reply_res= mysql.semisync_ack(binlog_filename, binlog_filepos) ||
    DBUG_IF("semislave_failed_net_flush");
    if (!reply_res)
      rpl_semi_sync_slave_send_ack++;
  DBUG_RETURN(reply_res);
}
