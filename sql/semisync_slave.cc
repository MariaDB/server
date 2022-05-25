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

Repl_semi_sync_slave repl_semisync_slave;

my_bool rpl_semi_sync_slave_enabled= 0;

char rpl_semi_sync_slave_delay_master;
my_bool rpl_semi_sync_slave_status= 0;
ulong rpl_semi_sync_slave_trace_level;

/*
  indicate whether or not the slave should send a reply to the master.

  This is set to true in repl_semi_slave_read_event if the current
  event read is the last event of a transaction. And the value is
  checked in repl_semi_slave_queue_event.
*/
bool semi_sync_need_reply= false;
unsigned int rpl_semi_sync_slave_kill_conn_timeout;
unsigned long long rpl_semi_sync_slave_send_ack = 0;

int Repl_semi_sync_slave::init_object()
{
  int result= 0;

  m_init_done = true;

  /* References to the parameter works after set_options(). */
  set_slave_enabled(rpl_semi_sync_slave_enabled);
  set_trace_level(rpl_semi_sync_slave_trace_level);
  set_delay_master(rpl_semi_sync_slave_delay_master);
  set_kill_conn_timeout(rpl_semi_sync_slave_kill_conn_timeout);

  return result;
}

int Repl_semi_sync_slave::slave_read_sync_header(const uchar *header,
                                                 unsigned long total_len,
                                                 int  *semi_flags,
                                                 const uchar **payload,
                                                 unsigned long *payload_len)
{
  int read_res = 0;
  DBUG_ENTER("Repl_semi_sync_slave::slave_read_sync_header");

  if (rpl_semi_sync_slave_status)
  {
    if (!DBUG_IF("semislave_corrupt_log")
        && header[0] == k_packet_magic_num)
    {
      semi_sync_need_reply  = (header[1] & k_packet_flag_sync);
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
  } else {
    *payload= header;
    *payload_len= total_len;
  }

  DBUG_RETURN(read_res);
}

int Repl_semi_sync_slave::slave_start(Master_info *mi)
{
  bool semi_sync= get_slave_enabled();

  sql_print_information("Slave I/O thread: Start %s replication to\
 master '%s@%s:%d' in log '%s' at position %lu",
			semi_sync ? "semi-sync" : "asynchronous",
			const_cast<char *>(mi->user), mi->host, mi->port,
			const_cast<char *>(mi->master_log_name),
                        (unsigned long)(mi->master_log_pos));

  if (semi_sync && !rpl_semi_sync_slave_status)
    rpl_semi_sync_slave_status= 1;

  /*clear the counter*/
  rpl_semi_sync_slave_send_ack= 0;
  return 0;
}

int Repl_semi_sync_slave::slave_stop(Master_info *mi)
{
  if (get_slave_enabled())
    kill_connection(mi->mysql);

  if (rpl_semi_sync_slave_status)
    rpl_semi_sync_slave_status= 0;

  return 0;
}

int Repl_semi_sync_slave::reset_slave(Master_info *mi)
{
  return 0;
}

void Repl_semi_sync_slave::kill_connection(MYSQL *mysql)
{
  if (!mysql)
    return;

  char kill_buffer[30];
  MYSQL *kill_mysql = NULL;
  size_t kill_buffer_length;

  kill_mysql = mysql_init(kill_mysql);
  mysql_options(kill_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &m_kill_conn_timeout);
  mysql_options(kill_mysql, MYSQL_OPT_READ_TIMEOUT, &m_kill_conn_timeout);
  mysql_options(kill_mysql, MYSQL_OPT_WRITE_TIMEOUT, &m_kill_conn_timeout);

  bool ret= (!mysql_real_connect(kill_mysql, mysql->host,
            mysql->user, mysql->passwd,0, mysql->port, mysql->unix_socket, 0));
  if (DBUG_IF("semisync_slave_failed_kill") || ret)
  {
    sql_print_information("cannot connect to master to kill slave io_thread's "
                          "connection");
    goto failed_graceful_kill;
  }

  DBUG_EXECUTE_IF("slave_delay_killing_semisync_connection", my_sleep(400000););

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
    return 1;
  }

  row= mysql_fetch_row(res);
  if (DBUG_IF("master_not_support_semisync") || !row)
  {
    /* Master does not support semi-sync */
    sql_print_warning("Master server does not support semi-sync, "
                      "fallback to asynchronous replication");
    rpl_semi_sync_slave_status= 0;
    mysql_free_result(res);
    return 0;
  }
  mysql_free_result(res);

  /*
   Tell master dump thread that we want to do semi-sync
   replication
  */
  query= "SET @rpl_semi_sync_slave= 1";
  if (mysql_real_query(mysql, query, (ulong)strlen(query)))
  {
    sql_print_error("Set 'rpl_semi_sync_slave=1' on master failed");
    return 1;
  }
  mysql_free_result(mysql_store_result(mysql));
  rpl_semi_sync_slave_status= 1;

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

  if (rpl_semi_sync_slave_status && semi_sync_need_reply)
  {
    /* Prepare the buffer of the reply. */
    reply_buffer[REPLY_MAGIC_NUM_OFFSET] = k_packet_magic_num;
    int8store(reply_buffer + REPLY_BINLOG_POS_OFFSET, binlog_filepos);
    memcpy(reply_buffer + REPLY_BINLOG_NAME_OFFSET,
           binlog_filename,
           name_len + 1 /* including trailing '\0' */);

    DBUG_PRINT("semisync", ("%s: reply (%s, %lu)",
                            "Repl_semi_sync_slave::slave_reply",
                            binlog_filename, (ulong)binlog_filepos));

    net_clear(net, 0);
    /* Send the reply. */
    reply_res = my_net_write(net, reply_buffer,
                             name_len + REPLY_BINLOG_NAME_OFFSET);
    if (!reply_res)
    {
      reply_res = (DBUG_IF("semislave_failed_net_flush") || net_flush(net));
      if (reply_res)
        sql_print_error("Semi-sync slave net_flush() reply failed");
      rpl_semi_sync_slave_send_ack++;
    }
    else
    {
      sql_print_error("Semi-sync slave send reply failed: %s (%d)",
                      net->last_error, net->last_errno);
    }
  }

  DBUG_RETURN(reply_res);
}
