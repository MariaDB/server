/*
   Copyright (c) 2001, 2011, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  All of the functions defined in this file which are not used (the ones to
  handle failsafe) are not used; their code has not been updated for more
  than one year now so should be considered as BADLY BROKEN. Do not enable
  it. The used functions (to handle LOAD DATA FROM MASTER, plus some small
  functions like register_slave()) are working.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_parse.h"                          // check_access
#ifdef HAVE_REPLICATION

#include "repl_failsafe.h"
#include "sql_acl.h"                            // REPL_SLAVE_ACL
#include "sql_repl.h"
#include "slave.h"
#include "rpl_mi.h"
#include "rpl_filter.h"
#include "log_event.h"
#include <mysql.h>
#include "semisync_master.h"

Atomic_counter<uint32_t> binlog_dump_thread_count;
ulong rpl_status=RPL_NULL;
mysql_mutex_t LOCK_rpl_status;

const char *rpl_role_type[] = {"MASTER","SLAVE",NullS};
TYPELIB rpl_role_typelib = {array_elements(rpl_role_type)-1,"",
			    rpl_role_type, NULL};

const char* rpl_status_type[]=
{
  "AUTH_MASTER","IDLE_SLAVE","ACTIVE_SLAVE","LOST_SOLDIER","TROOP_SOLDIER",
  "RECOVERY_CAPTAIN","NULL",NullS
};

/*
  All of the functions defined in this file which are not used (the ones to
  handle failsafe) are not used; their code has not been updated for more than
  one year now so should be considered as BADLY BROKEN. Do not enable it.
  The used functions (to handle LOAD DATA FROM MASTER, plus some small
  functions like register_slave()) are working.
*/

void change_rpl_status(ulong from_status, ulong to_status)
{
  mysql_mutex_lock(&LOCK_rpl_status);
  if (rpl_status == from_status || rpl_status == RPL_ANY)
    rpl_status = to_status;
  mysql_mutex_unlock(&LOCK_rpl_status);
}


#define get_object(p, obj, msg) \
{\
  uint len = (uint)*p++;  \
  if (p + len > p_end || len >= sizeof(obj)) \
  {\
    errmsg= msg;\
    goto err; \
  }\
  ::strmake(obj, (char*) p, len); \
  p+= len; \
}\


void THD::unregister_slave()
{
  if (auto old_si= slave_info)
  {
    mysql_mutex_lock(&LOCK_thd_data);
    slave_info= 0;
    mysql_mutex_unlock(&LOCK_thd_data);
    my_free(old_si);
    binlog_dump_thread_count--;
  }
}


/**
  Register slave

  @return
    0	ok
  @return
    1	Error.   Error message sent to client
*/

int THD::register_slave(uchar *packet, size_t packet_length)
{
  Slave_info *si;
  uchar *p= packet, *p_end= packet + packet_length;
  const char *errmsg= "Wrong parameters to function register_slave";

  if (check_access(this, PRIV_COM_REGISTER_SLAVE, any_db.str, NULL,NULL,0,0))
    return 1;
  if (!(si= (Slave_info*)my_malloc(key_memory_SLAVE_INFO, sizeof(Slave_info),
                                   MYF(MY_WME|MY_ZEROFILL))))
    return 1;
  si->sync_status.store(Slave_info::SYNC_STATUS_INITIALIZING,
                        std::memory_order_relaxed);

  variables.server_id= si->server_id= uint4korr(p);
  p+= 4;
  get_object(p,si->host, "Failed to register slave: too long 'report-host'");
  get_object(p,si->user, "Failed to register slave: too long 'report-user'");
  get_object(p,si->password, "Failed to register slave; too long 'report-password'");
  if (p+10 > p_end)
    goto err;
  si->port= uint2korr(p);
  p += 2;
  /* 
     We need to by pass the bytes used in the fake rpl_recovery_rank
     variable. It was removed in patch for BUG#13963. But this would 
     make a server with that patch unable to connect to an old master.
     See: BUG#49259
  */
  // si->rpl_recovery_rank= uint4korr(p);
  p += 4;
  if (!(si->master_id= uint4korr(p)))
    si->master_id= global_system_variables.server_id;

  if (!*si->host)
    ::strmake(si->host, main_security_ctx.host_or_ip, sizeof(si->host));

  unregister_slave();
  mysql_mutex_lock(&LOCK_thd_data);
  slave_info= si;
  mysql_mutex_unlock(&LOCK_thd_data);
  binlog_dump_thread_count++;
  return 0;

err:
  delete si;
  my_message(ER_UNKNOWN_ERROR, errmsg, MYF(0)); /* purecov: inspected */
  return 1;
}


bool THD::is_binlog_dump_thread()
{
  mysql_mutex_lock(&LOCK_thd_data);
  bool res= slave_info != NULL;
  mysql_mutex_unlock(&LOCK_thd_data);

  return res;
}


static my_bool show_slave_hosts_callback(THD *thd, Protocol *protocol)
{
  my_bool res= FALSE;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  String gtid_sent, gtid_ack;
  const char *sync_str;
  const char *err_msg= NULL;
  if (const Slave_info *si= thd->slave_info)
  {
    protocol->prepare_for_resend();
    protocol->store(si->server_id);
    protocol->store(si->host, strlen(si->host), &my_charset_bin);
    if (opt_show_slave_auth_info)
    {
      protocol->store(si->user, safe_strlen(si->user), &my_charset_bin);
      protocol->store(si->password, safe_strlen(si->password), &my_charset_bin);
    }
    protocol->store((uint32) si->port);
    protocol->store(si->master_id);

    if (gtid_state_from_binlog_pos(si->gtid_pos_sent.log_file,
                                   (uint32) si->gtid_pos_sent.log_pos.load(
                                       std::memory_order_relaxed),
                                   &gtid_sent, &err_msg))
    {
      DBUG_ASSERT(err_msg);
      gtid_sent.free();

      if (global_system_variables.log_warnings >= 2)
        push_warning_printf(
            current_thd, Sql_condition::WARN_LEVEL_WARN,
            ER_MASTER_CANNOT_RECONSTRUCT_GTID_STATE_FOR_BINLOG_POS,
            ER_THD(current_thd,
                   ER_MASTER_CANNOT_RECONSTRUCT_GTID_STATE_FOR_BINLOG_POS),
            si->gtid_pos_sent.log_pos.load(std::memory_order_relaxed),
            si->gtid_pos_sent.log_file, err_msg);
    }
    protocol->store_string_or_null(gtid_sent.ptr(), &my_charset_bin);

    if (rpl_semi_sync_master_enabled && thd->semi_sync_slave)
    {
      if (gtid_state_from_binlog_pos(si->gtid_pos_ack.log_file,
                                     (uint32) si->gtid_pos_ack.log_pos.load(
                                         std::memory_order_relaxed),
                                     &gtid_ack, &err_msg))
      {
        DBUG_ASSERT(err_msg);
        gtid_ack.free();

        if (global_system_variables.log_warnings >= 2)
        {
          push_warning_printf(
              current_thd, Sql_condition::WARN_LEVEL_NOTE,
              ER_MASTER_CANNOT_RECONSTRUCT_GTID_STATE_FOR_BINLOG_POS,
              ER_THD(current_thd,
                     ER_MASTER_CANNOT_RECONSTRUCT_GTID_STATE_FOR_BINLOG_POS),
              si->gtid_pos_ack.log_pos.load(std::memory_order_relaxed),
              si->gtid_pos_ack.log_file, err_msg);
        }
      }
    }
    protocol->store_string_or_null(gtid_ack.ptr(), &my_charset_bin);

    sync_str= si->get_sync_status_str();
    protocol->store(sync_str, safe_strlen(sync_str), &my_charset_bin);

    res= protocol->write();
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  return res;
}


/**
  Execute a SHOW SLAVE HOSTS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_slave_hosts(THD* thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("show_slave_hosts");

  field_list.push_back(new (mem_root)
                       Item_return_int(thd, "Server_id", 10,
                                       MYSQL_TYPE_LONG),
                       thd->mem_root);
  field_list.push_back(new (mem_root)
                       Item_empty_string(thd, "Host", 20),
                       thd->mem_root);
  if (opt_show_slave_auth_info)
  {
    field_list.push_back(new (mem_root) Item_empty_string(thd, "User", 20),
                         thd->mem_root);
    field_list.push_back(new (mem_root) Item_empty_string(thd, "Password", 20),
                         thd->mem_root);
  }
  field_list.push_back(new (mem_root)
                       Item_return_int(thd, "Port", 7, MYSQL_TYPE_LONG),
                       thd->mem_root);
  field_list.push_back(new (mem_root)
                       Item_return_int(thd, "Master_id", 10, MYSQL_TYPE_LONG),
                       thd->mem_root);

 /* Length matches GTID_IO_Pos of SHOW SLAVE STATUS on slave */
 field_list.push_back(new (mem_root)
                        Item_empty_string(thd, "Gtid_Pos_Sent", 30),
                        thd->mem_root);

 field_list.push_back(new (mem_root)
                        Item_empty_string(thd, "Gtid_Pos_Ack", 30),
                        thd->mem_root);

  /* For the length, use the size of the longest possible value */
 field_list.push_back(new (mem_root) Item_empty_string(
                          thd, "Sync_Status", sizeof("Semi-sync Active")),
                      thd->mem_root);

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  if (server_threads.iterate(show_slave_hosts_callback, protocol))
    DBUG_RETURN(true);

  my_eof(thd);
  DBUG_RETURN(FALSE);
}

#endif /* HAVE_REPLICATION */

