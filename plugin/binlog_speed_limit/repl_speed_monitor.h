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

#ifndef REPL_SPEED_MONITOR_H
#define REPL_SPEED_MONITOR_H

#include "repl_speedlimit_util.h"
#include "my_global.h"
#include "sql_class.h"

#include <vector>

enum rpl_speed_limit_mode_point_t {
  SPEED_LIMIT_MODE_SHARE_BANDWIDTH,
  SPEED_LIMIT_MODE_FIX_BANDWIDTH,
};

extern char rpl_speed_limit_enabled;

extern unsigned long rpl_speed_limit_tick_interval;
extern unsigned long rpl_speed_limit_max_token_ratio;

extern unsigned long rpl_speed_limit_mode;

extern unsigned long rpl_speed_limit_slave_bandwidth;
extern unsigned long rpl_speed_limit_total_bandwidth;

extern unsigned long rpl_speed_limit_trace_level;

extern unsigned long rpl_speed_limit_test_send_len;
extern unsigned long rpl_speed_limit_run_test;


//status:
extern unsigned long rpl_speed_limit_clients;

extern long long rpl_speed_limit_sleep_time;

extern long long rpl_speed_limit_sleep_count;

extern long long rpl_speed_limit_bytes;

extern long long rpl_speed_limit_bandwidth;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key  key_ss_mutex_Speed_monitor_mutex_;
extern PSI_mutex_key  key_ss_mutex_Speed_limit_mutex_;
#endif

struct token_bucket
{
  /* Get token before send, and wait if there is not enough token. */
  int64       token;

  /* Maximum token allowed in bucket. */
  int64       max_token;

  /* Sending speed (bytes per ms) */
  int64       speed;

  /* Last token update time(ms) */
  uint64      last_tick;

  token_bucket() :token(0), max_token(0), speed(0), last_tick(0)
  {}
};

class RplSpeedLimit
{
public:
  static const int kResetToken   = 0x0001;
  static const int kUpdateBucket = 0x0002;

  RplSpeedLimit(THD* thd);
  ~RplSpeedLimit();

  void updateSpeed(bool reset_token,  uint slave_cnt);

  token_bucket* getBucket();

private:
  token_bucket  m_bucket;

  mysql_mutex_t m_mutex;

  /* Maximum token to update */
  int64  m_bak_max_token;

  /* Speed to update */
  int64  m_bak_speed;

  /* Flag whether update is finished */
  int    m_flags;

  THD*   m_thd;
};

class RplSpeedMonitor : public Trace
{
public:
  RplSpeedMonitor();
  ~RplSpeedMonitor();

  void init();
  void cleanup();

  void updateConf(bool need_lock , bool reset_token);

  void setExportStatus();

  void setTraceLevel(unsigned long trace_level)
  {
    trace_level_ = trace_level;
  }

  void enable() { m_enabled = true; }

  void disable() { m_enabled = false; }

  void run_test(THD* thd, ulong run_time);

public:
  bool addSlave(THD* thd);

  void removeSlave(THD* thd);

  bool controlSpeed(THD* thd, ulong len);


private:
  /* Mutex to protect m_lists and m_version */
  mysql_mutex_t m_mutex;

  std::vector<RplSpeedLimit*>* m_lists;

  /* Speed monitor thread */
  pthread_t m_thread;

  volatile bool m_enabled;

  /* Declare them private, so no one can copy the object. */
  RplSpeedMonitor(const RplSpeedMonitor &speed_monitor);

  RplSpeedMonitor& operator=(const RplSpeedMonitor &speed_monitor);
};

#endif /* REPL_SPEED_MONITOR_H */
