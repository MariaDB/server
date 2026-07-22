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

void Repl_semi_sync_slave::slave_stop(Master_info *mi)
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
    kill_connection(mi);
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


void Repl_semi_sync_slave::kill_connection(Master_info *mi)
{
  MYSQL *mysql= mi->mysql;
  if (!mysql)
    return;

  char kill_buffer[30];
  MYSQL *kill_mysql = NULL;
  size_t kill_buffer_length;

  kill_mysql = mysql_init(kill_mysql);

  setup_mysql_connection_for_master(kill_mysql, mi, m_kill_conn_timeout);
  mysql_options(kill_mysql, MYSQL_OPT_WRITE_TIMEOUT, &m_kill_conn_timeout);

  bool ret= (!mysql_real_connect(kill_mysql, mysql->host,
            mysql->user, mysql->passwd,0, mysql->port, mysql->unix_socket, 0));
  if (DBUG_IF("semisync_slave_failed_kill") || ret)
  {
    sql_print_information("cannot connect to master to kill slave io_thread's "
                          "connection");
    goto failed_graceful_kill;
  }

  kill_buffer_length= my_snprintf(kill_buffer, 30, "KILL %lu",
                                mysql->thread_id);
  if (mysql_real_query(kill_mysql, kill_buffer, (ulong)kill_buffer_length))
  {
    sql_print_information(
        "Failed to gracefully kill our active semi-sync connection with "
        "primary. Silently closing the connection.");
    goto failed_graceful_kill;
  }

end:
  mysql_close(kill_mysql);
  return;

failed_graceful_kill:
  /*
    If we fail to issue `KILL` on the primary to kill the active semi-sync
    connection; we need to locally clean up our side of the connection. This
    is because mysql_close will send COM_QUIT on the active semi-sync
    connection, causing the primary to error.
  */
  net_clear(&(mysql->net), 0);
  end_server(mysql);
  goto end;
}

int Repl_semi_sync_slave::request_transmit(Master_info *mi)
{
  MYSQL *mysql= mi->mysql;
  MYSQL_RES *res= 0;
  MYSQL_ROW row;
  const char *query;

  if (!get_slave_enabled())
    return 0;

  query= "SHOW VARIABLES LIKE 'rpl_semi_sync_master_enabled'";
  if (mysql_real_query(mysql, query, (ulong)strlen(query)) ||
      !(res= mysql_store_result(mysql)))
  {
    sql_print_error("Execution failed on master: %s, error :%s", query, mysql_error(mysql));
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
  if (mysql_real_query(mysql, query, (ulong)strlen(query)))
  {
    sql_print_error("%s on master failed", query);
    set_slave_enabled(0);
    return 1;
  }
  mi->semi_sync_reply_enabled= 1;
  /* Inform net_server that pkt_nr can come out of order */
  mi->mysql->net.pkt_nr_can_be_reset= 1;
  mysql_free_result(mysql_store_result(mysql));

  return 0;
}

int Repl_semi_sync_slave::slave_reply(Master_info *mi)
{
  MYSQL* mysql= mi->mysql;
  const char *binlog_filename= const_cast<char *>(mi->master_log_name);
  my_off_t binlog_filepos= mi->master_log_pos;
  NET *net= &mysql->net;
  uchar reply_buffer[REPLY_MAGIC_NUM_LEN
                     + REPLY_BINLOG_POS_LEN
                     + REPLY_BINLOG_NAME_LEN];
  int reply_res = 0;
  size_t name_len = strlen(binlog_filename);
  DBUG_ENTER("Repl_semi_sync_slave::slave_reply");
  DBUG_ASSERT(get_slave_enabled() && mi->semi_sync_reply_enabled);

  /* Prepare the buffer of the reply. */
  reply_buffer[REPLY_MAGIC_NUM_OFFSET] = k_packet_magic_num;
  int8store(reply_buffer + REPLY_BINLOG_POS_OFFSET, binlog_filepos);
  memcpy(reply_buffer + REPLY_BINLOG_NAME_OFFSET,
         binlog_filename,
         name_len + 1 /* including trailing '\0' */);

  DBUG_PRINT("semisync", ("%s: reply (%s, %lu)",
                          "Repl_semi_sync_slave::slave_reply",
                          binlog_filename, (ulong)binlog_filepos));

  /*
    We have to do a net_clear() as with semi-sync the slave_reply's are
    interleaved with data from the master and then the net->pkt_nr
    cannot be kept in sync. Better to start pkt_nr from 0 again.
  */
  net_clear(net, 0);
  /* Send the reply. */
  reply_res = my_net_write(net, reply_buffer,
                           name_len + REPLY_BINLOG_NAME_OFFSET);
  if (!reply_res)
  {
    reply_res= DBUG_IF("semislave_failed_net_flush") || net_flush(net);
    if (!reply_res)
      rpl_semi_sync_slave_send_ack++;
  }
  DBUG_RETURN(reply_res);
}
