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

RplSpeedMonitor  speed_monitor;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_ss_mutex_Speed_monitor_mutex_;
PSI_mutex_key key_ss_mutex_Speed_limit_mutex_;

static PSI_mutex_info all_speedlimit_mutexes[] =
{
  { &key_ss_mutex_Speed_monitor_mutex_, "Speed_monitor::m_mutex", 0 },
  { &key_ss_mutex_Speed_limit_mutex_ ,  "Speed_limit::m_mutex",   0 }
};

void init_psi_keys(void)
{
  const char* category = "speedlimit";
  int count;

  if (PSI_server == NULL)
    return;

  count = array_elements(all_speedlimit_mutexes);
  PSI_server->register_mutex(category, all_speedlimit_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */

/*
 * speedlimit system variables
 * NOTE: must match order of rpl_semi_sync_master_wait_point_t
 */
static const char *rpl_speed_limit_mode_names[] =
{
  "SHARE_BANDWIDTH",  /* shared bandwidth */
  "FIX_BANDWIDTH",    /* fixed bandwidth  */
  NullS
};

static TYPELIB rpl_speed_limit_mode_typelib =
{
  array_elements(rpl_speed_limit_mode_names) - 1,
  "",
  rpl_speed_limit_mode_names,
  NULL
};

static void fix_rpl_speed_limit_mode(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static void fix_rpl_speed_limit_enabled(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static void fix_rpl_speed_limit_slave_bandwidth(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static void fix_rpl_speed_limit_total_bandwidth(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static void fix_rpl_speed_limit_max_token_ratio(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static void fix_rpl_speed_limit_trace_level(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static void fix_rpl_speed_limit_run_test(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val);

static int check_rpl_speed_limit_run_test(MYSQL_THD thd,
  SYS_VAR *var,
  void *save,
  struct st_mysql_value *value);


static MYSQL_SYSVAR_ENUM(mode, rpl_speed_limit_mode,
  PLUGIN_VAR_RQCMDARG,
  "Mode of speed limit: SHARE_BANDWIDTH-> total/N slaves;"
  "FIX_BANDWIDTH: bandwidth of slave is fix;",
  NULL,             // check
  fix_rpl_speed_limit_mode,   // update
  SPEED_LIMIT_MODE_FIX_BANDWIDTH,
  &rpl_speed_limit_mode_typelib);

static MYSQL_SYSVAR_BOOL(enabled, rpl_speed_limit_enabled,
  PLUGIN_VAR_OPCMDARG,
  "Enable replication speed limit (disabled by default). ",
  NULL,               // check
  &fix_rpl_speed_limit_enabled, // update
  0);

static MYSQL_SYSVAR_ULONG(tick_interval, rpl_speed_limit_tick_interval,
  PLUGIN_VAR_OPCMDARG,
  "Min sleep interval(ms)",
  NULL,   // check
  NULL, // update
  20, 5, 500, 1);

static MYSQL_SYSVAR_ULONG(max_token_ratio, rpl_speed_limit_max_token_ratio,
  PLUGIN_VAR_OPCMDARG,
  "max token of bucket/generate token",
  NULL,                 // check
  fix_rpl_speed_limit_max_token_ratio,// update
  150, 100, 1000, 1);

static MYSQL_SYSVAR_ULONG(slave_bandwidth, rpl_speed_limit_slave_bandwidth,
  PLUGIN_VAR_OPCMDARG,
  "target bandwidth to limit each slave(K/s)",
  NULL,
  fix_rpl_speed_limit_slave_bandwidth,
  10*1024UL, 1024UL, 1024UL * 1024UL, 1); //def: 10M, min: 1M, max:1G

static MYSQL_SYSVAR_ULONG(total_bandwidth, rpl_speed_limit_total_bandwidth,
  PLUGIN_VAR_OPCMDARG,
  "total bandwidth shared by all slaves(K/s)",
  NULL,
  fix_rpl_speed_limit_total_bandwidth,
  50*1024UL, 10*1024UL, 4 * 1024UL*1024UL, 1); //def: 50M min: 10M, max: 4G

static MYSQL_SYSVAR_ULONG(trace_level, rpl_speed_limit_trace_level,
  PLUGIN_VAR_OPCMDARG,
  "The tracing level for replication speedlimit.",
  NULL,               // check
  &fix_rpl_speed_limit_trace_level, // update
  0, 0, ~0UL, 1);

static MYSQL_SYSVAR_ULONG(test_send_len, rpl_speed_limit_test_send_len,
  PLUGIN_VAR_OPCMDARG,
  "send len of bytes on the running test.",
  NULL,             // check
  NULL,             // update
  100, 0, 2048*1024UL, 1);

static MYSQL_SYSVAR_ULONG(run_test, rpl_speed_limit_run_test,
  PLUGIN_VAR_OPCMDARG,
  "The time of running test.",
  &check_rpl_speed_limit_run_test,      // check
  &fix_rpl_speed_limit_run_test,        // update
  0, 0, 100UL, 1);

SYS_VAR* repl_speed_limit_system_vars[] = {
  MYSQL_SYSVAR(enabled),
  MYSQL_SYSVAR(mode),
  MYSQL_SYSVAR(tick_interval),
  MYSQL_SYSVAR(max_token_ratio),
  MYSQL_SYSVAR(slave_bandwidth),
  MYSQL_SYSVAR(total_bandwidth),
  MYSQL_SYSVAR(trace_level),
  MYSQL_SYSVAR(test_send_len),
  MYSQL_SYSVAR(run_test),
  NULL,
};

static void fix_rpl_speed_limit_mode(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(unsigned long*)ptr = *(unsigned long*)val;
  speed_monitor.updateConf(true, false);
}

static void fix_rpl_speed_limit_enabled(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(char *)ptr = *(char *)val;
  if (rpl_speed_limit_enabled)
    speed_monitor.enable();
  else
    speed_monitor.disable();
  return;
}

static void fix_rpl_speed_limit_slave_bandwidth(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(unsigned long*)ptr = *(unsigned long*)val;
  if (rpl_speed_limit_mode == SPEED_LIMIT_MODE_FIX_BANDWIDTH)
    speed_monitor.updateConf(true, false);
}

static void fix_rpl_speed_limit_total_bandwidth(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(unsigned long*)ptr = *(unsigned long*)val;
  if (rpl_speed_limit_mode == SPEED_LIMIT_MODE_SHARE_BANDWIDTH)
    speed_monitor.updateConf(true, false);
}

static void fix_rpl_speed_limit_max_token_ratio(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(unsigned long*)ptr = *(unsigned long*)val;
  speed_monitor.updateConf(true, false);
}

static void fix_rpl_speed_limit_trace_level(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(unsigned long *)ptr = *(unsigned long *)val;
  speed_monitor.setTraceLevel(rpl_speed_limit_trace_level);
  return;
}

static void fix_rpl_speed_limit_run_test(MYSQL_THD thd,
  SYS_VAR *var,
  void *ptr,
  const void *val)
{
  *(unsigned long *)ptr = *(unsigned long *)val;
  if (rpl_speed_limit_run_test > 0)
  {
    THD* thd = current_thd;
    speed_monitor.run_test(thd, rpl_speed_limit_run_test);

    rpl_speed_limit_run_test = 0; //reset to zero
  }
  return;
}

static int check_rpl_speed_limit_run_test(MYSQL_THD thd,
  SYS_VAR *var,
  void *save,
  struct st_mysql_value *value)
{
  /* Run test only if fix_bandwidth and enabled. */
  if (rpl_speed_limit_mode != SPEED_LIMIT_MODE_FIX_BANDWIDTH ||
    !rpl_speed_limit_enabled)
    return 1;
  return value->val_int(value, (long long*)save);
}
