/* Copyright (c) 2017, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
@file clone/src/clone_client.cc
Clone Plugin: Client implementation

*/
#include <inttypes.h>

#include "clone_client.h"
#include "clone_os.h"

#include "my_byteorder.h"
// #include "sql/sql_thd_internal_api.h"
#include "sql_string.h"
#include <functional>
#include <sstream>

/* Namespace for all clone data types */
namespace myclone
{

/** Default timeout is 300 seconds */
/** Minimum interval is 5 seconds. The actual value could be more based on
MySQL connect_timeout configuration. */
Time_Sec Client::s_reconnect_interval{5};

uint64_t Thread_Info::get_target_time(uint64_t current, uint64_t prev,
                                      uint64_t target)
{
  /* Target zero implies no throttling. */
  if (target == 0)
    return target;

  assert(current >= prev);
  auto bytes= current - prev;
  auto target_time_ms= (bytes * 1000) / target;
  return target_time_ms;
}

void Thread_Info::throttle(uint64_t data_target)
{
  auto cur_time= Clock::now();
  auto duration=
      std::chrono::duration_cast<Time_Msec>(cur_time - m_last_update);

  /* Check only at specific intervals. */
  if (duration < m_interval)
    return;

  /* Find the amount of time we should have taken based on the targets. */
  auto target_ms= get_target_time(m_data_bytes, m_last_data_bytes,
                                  data_target);
  auto duration_ms= static_cast<uint64_t>(duration.count());

  /* Sleep for the remaining time to throttle clone data transfer. */
  if (target_ms > duration_ms)
  {
    auto sleep_ms= target_ms - duration_ms;

    /* Don't sleep for more than 1 second so that we don't get into
    network timeout and can respond to abort/shutdown request. */
    if (sleep_ms > 1000)
    {
      sleep_ms= 1000;
      /* Lower check interval as we need to sleep more. This way
      we sleep more frequently. */
      m_interval= m_interval / 2;
    }
    Time_Msec sleep_time(sleep_ms);
    std::this_thread::sleep_for(sleep_time);
  }
  else
    /* Reset interval back to default 100ms. */
    m_interval= Time_Msec{100};

  m_last_data_bytes= m_data_bytes;
  m_last_update= Clock::now();
}

void Client_Stat::update(bool reset, const Thread_Vector &threads,
                         uint32_t num_workers)
{
  /* Ignore reset requests when stat is not initialized. */
  if (!m_initialized && reset)
    return;

  auto cur_time= Clock::now();

  /* Start time is set at first call. */
  if (!m_initialized)
  {
    m_start_time= cur_time;
    m_initialized= true;
    reset_history(true);
    set_target_bandwidth(num_workers, true, 0);
    return;
  }

  auto duration_ms =
      std::chrono::duration_cast<Time_Msec>(cur_time - m_eval_time);
  if (duration_ms < m_interval && !reset)
    return;

  m_eval_time= cur_time;
  uint64_t value_ms= duration_ms.count();

  uint64_t data_bytes= m_finished_data_bytes;

  /* Evaluate total data and network bytes transferred till now. */
  for (uint32_t index= 0; index <= num_workers; ++index)
  {
    auto &thread_info= threads[index];
    data_bytes+= thread_info.m_data_bytes;
  }

  /* Evaluate the transfer speed from last evaluation time. */
  auto cur_index= m_current_history_index % STAT_HISTORY_SIZE;
  ++m_current_history_index;

  uint64_t data_speed{};
  if (value_ms == 0)
    /* We might be too early here during reset. */
    assert(reset);
  else
  {
    /* Update PFS in bytes per second. */
    assert(data_bytes >= m_eval_data_bytes);
    auto data_inc= data_bytes - m_eval_data_bytes;
    data_speed= (data_inc * 1000) / value_ms;
    Client::update_pfs_data(data_inc,
                            static_cast<uint32_t>(data_speed),
                            num_workers);
  }

  /* Calculate speed in MiB per second. */
  auto data_speed_mib= data_speed / (1024 * 1024);

  m_data_speed_history[cur_index]= data_speed_mib;

  /* Set currently evaluated data. */
  m_eval_data_bytes= data_bytes;

  if (reset)
  {
    /* Convert to Mebibytes (MiB) */
    auto total_data_mb= data_bytes / (1024 * 1024);

    /* Find and log cumulative data transfer rate. */
    duration_ms =
        std::chrono::duration_cast<Time_Msec>(cur_time - m_start_time);
    value_ms= duration_ms.count();

    data_speed_mib= (value_ms == 0) ? 0 : (total_data_mb * 1000) / value_ms;

    /* Log current speed. */
    const size_t MESG_SZ= 128;
    char info_mesg[MESG_SZ];

    snprintf(info_mesg, MESG_SZ,
             "Total Data: %" PRIu64 " MiB @ %" PRIu64 " MiB/sec",
             total_data_mb, data_speed_mib);

    LogPluginErr(INFORMATION_LEVEL, ER_CLONE_CLIENT_TRACE, info_mesg);
    reset_history(false);
  }

  if (num_workers != 0)
    /* Set targets for all tasks. */
    set_target_bandwidth(num_workers, reset, data_speed);
}

uint64_t Client_Stat::task_target(uint64_t target_speed, uint64_t current_speed,
                                  uint64_t current_target, uint32_t num_tasks)
{
  assert(num_tasks > 0);

  /* Zero is special value indicating unlimited bandwidth. */
  if (target_speed == 0)
    return 0;

  /* Estimate number of active tasks based on current performance. If target is
  not set yet, start by assuming all active thread. */
  auto active_tasks =
      (current_target == 0) ? num_tasks : (current_speed / current_target);

  /* Keep the value within current boundary. */
  if (active_tasks == 0)
    active_tasks= 1;
  else if (active_tasks > num_tasks)
    active_tasks= num_tasks;

  auto task_target= target_speed / active_tasks;

  /* Don't set anything lower than a minimum threshold. Protection against
  bad configuration asking too many threads and very less bandwidth. */
  if (task_target < m_minimum_speed)
    task_target= m_minimum_speed;

  return task_target;
}

void Client_Stat::set_target_bandwidth(uint32_t num_workers, bool is_reset,
                                       uint64_t data_speed)
{
  uint64_t data_target= clone_max_io_bandwidth * 1024 * 1024;
  if (!is_reset)
    data_target=
        task_target(data_target, data_speed, m_target_data_speed, num_workers);
  m_target_data_speed.store(data_target);
}

void Client_Stat::reset_history(bool init)
{
  m_data_speed_history.fill(0);
  m_current_history_index= 0;

  /* Set evaluation results during initialization. */
  if (init)
  {
    m_eval_data_bytes= 0;
    m_finished_data_bytes= 0;
    m_eval_time= Clock::now();
  }
}

inline void net_server_ext_init(NET_SERVER *ns)
{
  ns->m_user_data= nullptr;
  ns->m_before_header= nullptr;
  ns->m_after_header= nullptr;
}

Client::Client(THD *thd, Client_Share *share, uint32_t index, bool is_master)
    : m_server_thd(thd),
      m_conn(),
      m_is_master(is_master),
      m_thread_index(index),
      m_num_active_workers(),
      m_storage_initialized(false),
      m_storage_active(false),
      m_acquired_backup_lock(false),
      m_share(share)
{
  m_ext_link.set_socket(MYSQL_INVALID_SOCKET);
  /* Master must be at index zero */
  if (is_master)
  {
    assert(index == 0);
    m_thread_index= 0;
  }

  /* Reset thread statistics. */
  auto &info= get_thread_info();
  info.reset();

  m_tasks.reserve(MAX_CLONE_STORAGE_ENGINE);

  m_copy_buff.init();
  m_cmd_buff.init();

  net_server_ext_init(&m_conn_server_extn);
}

Client::~Client()
{
  assert(!m_storage_initialized);
  assert(!m_storage_active);
  m_copy_buff.free();
  m_cmd_buff.free();
}

uint32_t Client::update_stat(bool is_reset)
{
  /* Statistics is updated by master task. */
  if (!is_master())
    return m_num_active_workers;

  auto &stat= m_share->m_stat;
  stat.update(is_reset, m_share->m_threads, m_num_active_workers);
  return m_num_active_workers;
}

void Client::check_and_throttle()
{
  uint64_t data_speed{};
  m_share->m_stat.get_target(data_speed);
  get_thread_info().throttle(data_speed);
}

uchar *Client::get_aligned_buffer(uint32_t len)
{
  auto err= m_copy_buff.allocate(len + CLONE_OS_ALIGN);

  if (err != 0)
    return nullptr;

  /* Align buffer to CLONE_OS_ALIGN[4K] for O_DIRECT */
  auto buf_ptr= clone_os_align(m_copy_buff.m_buffer);
  return buf_ptr;
}

void Client::wait_for_workers()
{
  if (!is_master())
  {
    assert(m_num_active_workers == 0);
    return;
  }
  /* Wait for concurrent worker tasks to finish. */
  auto &thread_vector= m_share->m_threads;
  assert(thread_vector.size() > m_num_active_workers);
  auto &stat= m_share->m_stat;

  while (m_num_active_workers > 0)
  {
    auto &info= thread_vector[m_num_active_workers];
    info.m_thread.join();

    /* Save all transferred bytes by the thread. */
    stat.save_at_exit(info.m_data_bytes);
    info.reset();

    --m_num_active_workers;
  }
  /* Save all transferred bytes by master thread. */
  auto &info= get_thread_info();
  stat.save_at_exit(info.m_data_bytes);
  info.reset();

  /* Reset stat and tuning information for next cycle after restart. */
  stat.reset_history(false);
}

int Client::pfs_begin_state()
{
  if (!is_master())
    return 0;

  mysql_mutex_lock(&s_table_mutex);
  /* Check and exit if concurrent clone in progress. */
  if (s_num_clones != 0)
  {
    mysql_mutex_unlock(&s_table_mutex);
    assert(s_num_clones == 1);
    my_error(ER_CLONE_TOO_MANY_CONCURRENT_CLONES, MYF(0), 1);
    return ER_CLONE_TOO_MANY_CONCURRENT_CLONES;
  }
  s_num_clones= 1;
  s_status_data.begin(1, get_thd(), m_share->m_host, m_share->m_port,
                      get_data_dir());
  s_progress_data.init_stage(get_data_dir());
  mysql_mutex_unlock(&s_table_mutex);

  return 0;
}

void Client::pfs_change_stage(uint64_t estimate)
{
  if (!is_master())
    return;

  mysql_mutex_lock(&s_table_mutex);
  s_progress_data.end_stage(false, get_data_dir());
  s_progress_data.begin_stage(1, get_data_dir(), m_num_active_workers + 1,
                              estimate);
  s_status_data.write(false);
  mysql_mutex_unlock(&s_table_mutex);
}

void Client::pfs_end_state(uint32_t err_num, const char *err_mesg)
{
  if (!is_master())
    return;

  mysql_mutex_lock(&s_table_mutex);
  assert(s_num_clones == 1);

  const bool provisioning= (get_data_dir() == nullptr);
  const bool failed= (err_num != 0);

  /* In case provisioning is successful, clone operation is still
  in progress and will continue after restart. */
  if (!provisioning || failed)
    s_num_clones= 0;

  s_progress_data.end_stage(failed, get_data_dir());
  s_status_data.end(err_num, err_mesg, provisioning);
  mysql_mutex_unlock(&s_table_mutex);
}

void Client::copy_pfs_data(Status_pfs::Data &pfs_data)
{
  mysql_mutex_lock(&s_table_mutex);
  /* If clone operation is started skip recovering previous data. */
  if (s_num_clones == 0)
    s_status_data.recover();

  pfs_data= s_status_data;
  mysql_mutex_unlock(&s_table_mutex);
}

void Client::copy_pfs_data(Progress_pfs::Data &pfs_data)
{
  mysql_mutex_lock(&s_table_mutex);
  pfs_data= s_progress_data;
  mysql_mutex_unlock(&s_table_mutex);
}

void Client::update_pfs_data(uint64_t data, uint32_t data_speed,
                             uint32_t num_workers)
{
  s_progress_data.update_data(data, 0, data_speed, 0, 1);
}

bool Client::s_pfs_initialized= false;

void Client::init_pfs()
{
  mysql_mutex_init(PSI_NOT_INSTRUMENTED, &s_table_mutex, MY_MUTEX_INIT_FAST);
  /* Recover PFS data. */
  s_progress_data.read();
  s_status_data.read();
  s_pfs_initialized= true;
}

void Client::uninit_pfs()
{
  if (s_pfs_initialized)
    mysql_mutex_destroy(&s_table_mutex);

  s_pfs_initialized= false;
}

uint32_t Client::limit_buffer(uint32_t buffer_size)
{
  /* Limit total buffer size to 128 M */
  const uint32_t max_buffer_size= 128 * 1024 * 1024;
  auto num_tasks= 1;
  auto limit= max_buffer_size / num_tasks;
  if (buffer_size > limit)
    buffer_size= limit;
  return buffer_size;
}

const char *sub_command_str(Sub_Command sub_com)
{
  const char *ret= "";
  switch(sub_com)
  {
    case SUBCOM_NONE:
      ret= "COM_EXECUTE: SUBCOM_NONE";
      break;
    case SUBCOM_EXEC_CONCURRENT:
      ret= "COM_EXECUTE: SUBCOM_EXEC_CONCURRENT";
      break;
    case SUBCOM_EXEC_BLOCK_NT_DML:
      ret= "COM_EXECUTE: SUBCOM_EXEC_BLOCK_NT_DML";
      break;
    case SUBCOM_EXEC_BLOCK_DDL:
      ret= "COM_EXECUTE: SUBCOM_EXEC_BLOCK_DDL";
      break;
    case SUBCOM_EXEC_SNAPSHOT:
      ret= "COM_EXECUTE: SUBCOM_EXEC_SNAPSHOT";
      break;
    case SUBCOM_EXEC_END:
      return "COM_EXECUTE: SUBCOM_EXEC_END";
      break;
    case SUBCOM_MAX:
      assert(false);
      ret= "COM_EXECUTE: SUBCOM_MAX";
      break;
  }
  return ret;
}

int Exec_State::begin_worker(Sub_Command &state)
{
  std::unique_lock lock(m_mutex);
  /* Wait till the execution state has reached desired state. */
  state= m_cur_state;
  auto cur_index= static_cast<size_t>(m_cur_state);
  ++m_count_workers[cur_index];

  return 0;
}

int Exec_State::end_worker(Sub_Command state)
{
  std::unique_lock lock(m_mutex);
  auto cur_index= static_cast<size_t>(state);

  assert(state == m_cur_state || m_cur_state == SUBCOM_MAX);
  assert(m_count_workers[cur_index] > 0);

  if (!(--m_count_workers[cur_index]))
    m_wait_count.notify_one();

  return 0;
}

int Exec_State::switch_state(THD *thd, Sub_Command next_state)
{
  std::unique_lock lock(m_mutex);
  assert(m_cur_state <= next_state);
  auto cur_index= static_cast<size_t>(m_cur_state);

  auto cond_fn= [&]
  {
    return (m_count_workers[cur_index] == 0);
  };
  int err= 0;
  bool result= false;
  Time_Sec m_interval{1};

  while (!result && next_state != SUBCOM_MAX)
  {
    result= m_wait_count.wait_for(lock, m_interval, cond_fn);
    if (thd_killed(thd))
    {
      my_error(ER_QUERY_INTERRUPTED, MYF(0));
      err= ER_QUERY_INTERRUPTED;
      break;
    }
  }

  if (err || next_state == SUBCOM_MAX)
  {
    m_next_state= SUBCOM_MAX;
    m_cur_state= SUBCOM_MAX;
    lock.unlock();
    m_wait_state.notify_all();
  }
  else
    /* The current state would be set later after appropriate locks are
    acquired handled by COM_RES_LOCKED. */
    m_next_state= next_state;

  return err;
}

bool Exec_State::update_current_state(Sub_Command sub_state)
{
  bool success= true;
  std::unique_lock lock(m_mutex);

  assert(m_cur_state <= m_next_state);
  assert(sub_state == m_next_state);

  if (sub_state != m_next_state)
  {
    m_next_state= SUBCOM_MAX;
    m_cur_state= SUBCOM_MAX;
    success= false;
  }
  else if (m_cur_state != m_next_state)
  {
    m_cur_state= m_next_state;
    lock.unlock();
    m_wait_state.notify_all();
  }
  return success;
}

int Client::exec_begin_state(THD *thd, Sub_Command &sub_state)
{
  auto &exec_state= m_share->m_state;
  if (is_master())
    return exec_state.switch_state(thd, sub_state);

  return exec_state.begin_worker(sub_state);
}

int Client::exec_end_state(Sub_Command sub_state)
{
  auto &exec_state= m_share->m_state;
  if (is_master())
  {
    exec_state.update_current_state(sub_state);
    return 0;
  }
  return exec_state.end_worker(sub_state);
}

int Client::execute(std::function<int(Sub_Command)> cbk)
{
  int err= 0;
  auto cur_st= static_cast<uchar>(SUBCOM_EXEC_CONCURRENT);
  auto end_st= static_cast<uchar>(SUBCOM_EXEC_END);

  for (;;)
  {
    auto sub_state= static_cast<Sub_Command>(cur_st);
    auto local_err= exec_begin_state(get_thd(), sub_state);

    /* We might have attached to a different state. */
    cur_st= static_cast<uchar>(sub_state);

    if (cur_st > end_st)
      break;
    assert(cur_st <= end_st);

    if (!local_err && !skip_state(sub_state))
      local_err= cbk(sub_state);

    exec_end_state(sub_state);
    /* In case of any error, jump to the final state. */
    if (local_err)
    {
      assert(!err);
      cur_st= end_st;
      err= local_err;
    }
    ++cur_st;
  }
  return err;
}

}  // namespace myclone
