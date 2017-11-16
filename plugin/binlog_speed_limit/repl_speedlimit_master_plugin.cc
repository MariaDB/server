/* Copyright (c) 2015, 2017, Tencent.
Use is subject to license terms

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

#include "repl_speedlimit_plugin_vars.h"
#include "sql_class.h"                          // THD

extern RplSpeedMonitor  speed_monitor;

int repl_speedlimit_binlog_dump_start(
        Binlog_transmit_param *param,
        const char *log_file,
        my_off_t log_pos)
{

  THD* thd = current_thd;
  if (speed_monitor.addSlave(thd))
    return 1;
  return 0;
}

int repl_speedlimit_binlog_dump_end(Binlog_transmit_param *param)
{
  THD* thd = current_thd;
  speed_monitor.removeSlave(thd);
  return 0;
}


int repl_speedlimit_before_send_event(
        Binlog_transmit_param *param,
        unsigned char *packet, unsigned long len,
        const char *log_file, my_off_t log_pos)
{
  THD* thd = current_thd;
  if (speed_monitor.controlSpeed(thd, len))
    return 1;
  return 0;
}

Binlog_transmit_observer transmit_observer = {
  sizeof(Binlog_transmit_observer),   // len
  repl_speedlimit_binlog_dump_start,  // start
  repl_speedlimit_binlog_dump_end,    // stop
  NULL,                               // reserve_header
  repl_speedlimit_before_send_event,  // before_send_event
  NULL,                               // after_send_event
  NULL,                               // reset
};

static int repl_speedlimit_master_plugin_init(void *p)
{
#ifdef HAVE_PSI_INTERFACE
  init_psi_keys();
#endif
  speed_monitor.init();

  if (register_binlog_transmit_observer(&transmit_observer, p))
    return 1;

  sql_print_information("register speedlimit master plugin OK");
  return 0;
}

static int repl_speedlimit_master_plugin_deinit(void *p)
{
  speed_monitor.cleanup();
  if (unregister_binlog_transmit_observer(&transmit_observer, p))
  {
    sql_print_error("unregister_binlog_transmit_observer failed");
    return 1;
  }

  sql_print_information("unregister speedlimit master plugin OK");
  return 0;
}

struct Mysql_replication repl_speedlimit_master_plugin= {
  MYSQL_REPLICATION_INTERFACE_VERSION
};

DEF_SHOW_FUNC(clients, SHOW_LONG);

/* plugin status variables */
SHOW_VAR repl_speed_limit_status_vars[] = {
  { "repl_speed_limit_master_clients",
  (char*)&SHOW_FNAME(clients),
  SHOW_FUNC },
  { "repl_speed_limit_master_sleep_time",
  (char*)&rpl_speed_limit_sleep_time,
  SHOW_LONGLONG },
  { "repl_speed_limit_master_sleep_count",
  (char*)&rpl_speed_limit_sleep_count,
  SHOW_LONGLONG },
  { "repl_speed_limit_master_bytes_send",
  (char*)&rpl_speed_limit_bytes,
  SHOW_LONGLONG },
  { "repl_speed_limit_master_bandwidth",
  (char*)&rpl_speed_limit_bandwidth,
  SHOW_LONGLONG },
  { NULL, NULL, SHOW_LONG },
};

/*
  Plugin library descriptor
*/
mysql_declare_plugin(repl_speedlimit_master)
{
  MYSQL_REPLICATION_PLUGIN,
  &repl_speedlimit_master_plugin,
  "repl_speedlimit_master",
  "zhiyangli",
  "replication speed limit in master",
  PLUGIN_LICENSE_GPL,
  repl_speedlimit_master_plugin_init,     /* Plugin init */
  repl_speedlimit_master_plugin_deinit,   /* Plugin deinit */
  0x0100,                                 /* Plugin version*/
  repl_speed_limit_status_vars,           /* status variables */
  repl_speed_limit_system_vars,           /* system variables */
  NULL,                                   /* config options */
  0,                                      /* flags */
}
mysql_declare_plugin_end;
