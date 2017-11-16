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

#include <my_sys.h>
#include "repl_speed_monitor.h"
#include "sql_base.h"

#define MIN_SPEED   (32LL)       //32 bytes per ms(32k/s)
#define MAX_SLEEP_TIME 2000


pthread_key(RplSpeedLimit*, THD_RPL_SPEED_LIMIT);

/* variables */
char rpl_speed_limit_enabled;

unsigned long rpl_speed_limit_tick_interval;
unsigned long rpl_speed_limit_max_token_ratio;

unsigned long rpl_speed_limit_mode;
unsigned long rpl_speed_limit_slave_bandwidth;
unsigned long rpl_speed_limit_total_bandwidth;

unsigned long rpl_speed_limit_trace_level;

unsigned long rpl_speed_limit_test_send_len;
unsigned long rpl_speed_limit_run_test;

/* status */
unsigned long rpl_speed_limit_clients = 0;

long long rpl_speed_limit_sleep_time = 0;

long long rpl_speed_limit_sleep_count = 0;

long long rpl_speed_limit_bytes = 0;

long long rpl_speed_limit_bandwidth = 0;


#ifdef HAVE_PSI_INTERFACE

extern PSI_mutex_key key_ss_mutex_Speed_monitor_mutex_;
extern PSI_mutex_key key_ss_mutex_Speed_limit_mutex_;

#endif

RplSpeedLimit::RplSpeedLimit(THD* thd)
  :  m_flags(0),
     m_thd(thd)
{
  mysql_mutex_init(key_ss_mutex_Speed_limit_mutex_,
                   &m_mutex, MY_MUTEX_INIT_FAST);
}

RplSpeedLimit::~RplSpeedLimit()
{
  mysql_mutex_destroy(&m_mutex);
}

void RplSpeedLimit::updateSpeed(bool reset_token, uint slave_cnt)
{
  int64 bandwidth = 0;
  mysql_mutex_lock(&m_mutex);

  switch (rpl_speed_limit_mode)
  {
  case SPEED_LIMIT_MODE_FIX_BANDWIDTH:
    bandwidth = (rpl_speed_limit_slave_bandwidth * 1024);
    break;
  case SPEED_LIMIT_MODE_SHARE_BANDWIDTH:
    bandwidth = (rpl_speed_limit_total_bandwidth * 1024) / slave_cnt;
    break;
  default:
    DBUG_ASSERT(0);
    break;
  }
  m_bak_speed = bandwidth / 1000;  //bytes per ms
  if (m_bak_speed < MIN_SPEED)
    m_bak_speed = MIN_SPEED;
  //total bytes of one second
  m_bak_max_token = m_bak_speed * rpl_speed_limit_max_token_ratio * 10;//10 = 1000ms /100%
  m_flags |= kUpdateBucket;
  if (reset_token)
    m_flags |= kResetToken;

  mysql_mutex_unlock(&m_mutex);
}

token_bucket* RplSpeedLimit::getBucket()
{
  if (m_flags & kUpdateBucket)
  {
    mysql_mutex_lock(&m_mutex);
    m_bucket.max_token = m_bak_max_token;
    m_bucket.speed = m_bak_speed;
    if (m_flags & kResetToken)
    {
      m_bucket.token = 0;
      m_bucket.last_tick = get_current_ms();
    }
    else if (m_bucket.token > m_bucket.max_token)
    {
      m_bucket.token = m_bucket.max_token;
      m_bucket.last_tick = get_current_ms();
    }
    m_flags = 0;

    mysql_mutex_unlock(&m_mutex);
  }
  return &m_bucket;
}

bool speed_monitor_thread_enabled = true;
bool speed_monitor_thread_running = false;

/* Monitor speed thread handler. */
void* speed_monitor_handler(void* arg)
{
  my_thread_init();
  speed_monitor_thread_running = true;

  /* Default monitor internal is 5s. */
  const uint64 monitor_interval = 5;
  const uint64 max_sleep_time = 500;
  static uint64 send_bytes = rpl_speed_limit_bytes;
  uint64 sleep_time;

  while (speed_monitor_thread_enabled)
  {
    send_bytes = rpl_speed_limit_bytes - send_bytes;
    rpl_speed_limit_bandwidth = send_bytes / monitor_interval / 1024;
    send_bytes = rpl_speed_limit_bytes;

    sleep_time = monitor_interval * 1000;
    DBUG_ASSERT(sleep_time % max_sleep_time == 0);

    while (sleep_time >= max_sleep_time)
    {
      sleep_ms(max_sleep_time);
      sleep_time -= max_sleep_time;

      if (!speed_monitor_thread_enabled)
        break;
    }

    DBUG_ASSERT(sleep_time == 0 || !speed_monitor_thread_enabled);
  }

  my_thread_end();
  speed_monitor_thread_running = false;

  return NULL;
}

RplSpeedMonitor::RplSpeedMonitor()
  : m_enabled(false)
{}

RplSpeedMonitor::~RplSpeedMonitor()
{}

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_binlog_speed_limit;

static PSI_thread_info all_binlog_speed_limit_threads[]=
{
  { &key_thread_binlog_speed_limit, "binlog_speed_limit_background",
    PSI_FLAG_GLOBAL },
};
#endif /* HAVE_PSI_INTERFACE */

void RplSpeedMonitor::init()
{
  static const char* kWho = "RplSpeedMonitor::init";
  function_enter(kWho);

  m_lists = new std::vector<RplSpeedLimit*>();

  /* Mutex initialization can only be done after MY_INIT(). */
  mysql_mutex_init(key_ss_mutex_Speed_monitor_mutex_,
    &m_mutex, MY_MUTEX_INIT_FAST);

#ifdef HAVE_PSI_INTERFACE
  if (PSI_server)
    PSI_server->register_thread("binlog_speed_limit", all_binlog_speed_limit_threads,
                                array_elements(all_binlog_speed_limit_threads));
#endif

  pthread_key_create(&THD_RPL_SPEED_LIMIT, NULL);

  if (mysql_thread_create(key_thread_binlog_speed_limit, &m_thread,
                          NULL, speed_monitor_handler, NULL))
  {
    sql_print_error("Start speed monitor thread failed.");
  }

  function_exit(kWho);
}

void RplSpeedMonitor::cleanup()
{
  static const char* kWho = "RplSpeedMonitor::cleanup";
  function_enter(kWho);

  pthread_key_delete(THD_RPL_SPEED_LIMIT);
  mysql_mutex_destroy(&m_mutex);

  for (uint i = 0; i < m_lists->size(); i++)
  {
    RplSpeedLimit* ptr = m_lists->at(i);
    delete ptr;
  }
  delete m_lists;

  /* Wait for moniotr thread to exit. */
  speed_monitor_thread_enabled = false;
  while (speed_monitor_thread_running) {
    sleep_ms(500);
  }

  function_exit(kWho);
}

void RplSpeedMonitor::updateConf(bool need_lock, bool reset_token)
{
  static const char* kWho = "RplSpeedMonitor::updateConf";
  function_enter(kWho);

  if (need_lock)
    mysql_mutex_lock(&m_mutex);

  for (uint i = 0; i < m_lists->size(); i++)
  {
    RplSpeedLimit* ptr = m_lists->at(i);
    ptr->updateSpeed(reset_token, m_lists->size());
  }

  if (need_lock)
    mysql_mutex_unlock(&m_mutex);

  function_exit(kWho);
}

void RplSpeedMonitor::setExportStatus()
{
  mysql_mutex_lock(&m_mutex);

  rpl_speed_limit_clients = (int)m_lists->size();

  mysql_mutex_unlock(&m_mutex);
}

bool RplSpeedMonitor::addSlave(THD* thd)
{
  static const char* kWho = "RplSpeedMonitor::addSlave";
  function_enter(kWho);

  if (!m_enabled)
    return function_exit(kWho, false);

  RplSpeedLimit* info_ptr = new RplSpeedLimit(thd);
  if (!info_ptr)
    return function_exit(kWho, false);

  if (pthread_setspecific(THD_RPL_SPEED_LIMIT, info_ptr))
  {
    free(info_ptr);
    return function_exit(kWho, true);
  }

  mysql_mutex_lock(&m_mutex);

  m_lists->push_back(info_ptr);

  if (rpl_speed_limit_mode == SPEED_LIMIT_MODE_SHARE_BANDWIDTH)
    updateConf(false, false);
  else
    info_ptr->updateSpeed(true, m_lists->size());

  mysql_mutex_unlock(&m_mutex);

  sql_print_information("Start speed limit to slave (server_id: %d)",
            thd->variables.server_id);

  return function_exit(kWho, false);
}

void RplSpeedMonitor::removeSlave(THD* thd)
{
  static const char* kWho = "RplSpeedMonitor::removeSlave";
  function_enter(kWho);

  RplSpeedLimit* info = (RplSpeedLimit*)pthread_getspecific(THD_RPL_SPEED_LIMIT);
  if (info)
  {
    mysql_mutex_lock(&m_mutex);

    std::vector<RplSpeedLimit*>::iterator iter =
    std::find(m_lists->begin(), m_lists->end(), info);

    if (iter != m_lists->end())
      m_lists->erase(iter);

    if (rpl_speed_limit_mode == SPEED_LIMIT_MODE_SHARE_BANDWIDTH)
      updateConf(false, false);

    mysql_mutex_unlock(&m_mutex);

    free(info);
    pthread_setspecific(THD_RPL_SPEED_LIMIT, NULL);
    sql_print_information("Stop speed limit to slave (server_id: %d)",
              thd->variables.server_id);
  }

  function_exit(kWho);
}

bool RplSpeedMonitor::controlSpeed(THD* thd, ulong len)
{
  if (!m_enabled)
    return false;

  RplSpeedLimit* info_ptr = (RplSpeedLimit*)pthread_getspecific(THD_RPL_SPEED_LIMIT);
  if (!info_ptr)
    return false;

  token_bucket* bucket = info_ptr->getBucket();

  if (trace_level_ & kTraceDetail)
    sql_print_information("server_id: %u >> token: %lld, max_token: %lld, speed: %lld, len: %lu",
          thd->variables.server_id, bucket->token, bucket->max_token, bucket->speed, len);

  DBUG_ASSERT(bucket->token <= bucket->max_token);

  /* fast path */
  if (bucket->token >= (int64)len)
  {
    bucket->token -= len;
    my_atomic_add64(&rpl_speed_limit_bytes, len);
    return false;
  }

  /* slow patch */
  do
  {
    uint64 now = get_current_ms();
    /* Make sure time is incremental */
    if (unlikely(now < bucket->last_tick))
      bucket->last_tick = now;

    /* Add token into bucket according to eclapsed time */
    bucket->token += (now - bucket->last_tick) * bucket->speed;
    bucket->last_tick = now;

    if (bucket->token >= (int64)len) {
      bucket->token -= len;
      my_atomic_add64(&rpl_speed_limit_bytes, len);
      break;
    }

    /* Sleep to get enough token */
    int64 sleep_time =((int64)len - bucket->token) / bucket->speed + 1;
    if (sleep_time < (int64)rpl_speed_limit_tick_interval)
      sleep_time = rpl_speed_limit_tick_interval;

    my_atomic_add64(&rpl_speed_limit_sleep_time, sleep_time);
    my_atomic_add64(&rpl_speed_limit_sleep_count, 1);

    /* End sleep once plugin is disabled */
    while (sleep_time > MAX_SLEEP_TIME)
    {
      sleep_ms(MAX_SLEEP_TIME);
      sleep_time -= MAX_SLEEP_TIME;
      if (!m_enabled)
        return false;
    }
    sleep_ms(sleep_time);

  } while (1);

  /* Adjust token if token is greater than max_token, when
   * we haven't reached slow path for a long time.
   * Note: we don't adjust token just after adding, because
   * if required token is greater than max_token, it will
   * never get it.
   */
  if (bucket->token > bucket->max_token)
    bucket->token = bucket->max_token;

  return false;
}

void RplSpeedMonitor::run_test(THD* thd, ulong run_time)
{
  const ulong send_len = rpl_speed_limit_test_send_len;
  int64 total_len = 0;

  this->addSlave(thd);

  uint64 cur_ms = get_current_ms();
  uint64 end_ms = cur_ms + run_time * 1000;

  while (cur_ms < end_ms)
  {
    if (this->controlSpeed(thd, send_len))
    {
      sql_print_information("controlSpeed Failed!");
      continue;
    }
    total_len += send_len;
    cur_ms = get_current_ms();
  }
  this->removeSlave(thd);

  sql_print_information("send len: %lu, total len: %lld bytes, time used:"
    "%lu sec(expect: %lu K/ps, actual: %lu K/ps)",
    send_len, total_len, run_time,
    rpl_speed_limit_slave_bandwidth,
    (unsigned long)((double)total_len / run_time / 1024.0));
}
