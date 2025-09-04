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
@file clone/include/clone_client.h
Clone Plugin: Client Interface

*/

#ifndef CLONE_CLIENT_H
#define CLONE_CLIENT_H

#include "clone.h"
#include "clone_hton.h"
#include "clone_status.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/* Namespace for all clone data types */
namespace myclone
{

using Clock = std::chrono::steady_clock;
using Time_Point = std::chrono::time_point<Clock>;

using Time_Msec = std::chrono::milliseconds;
using Time_Sec = std::chrono::seconds;
using Time_Min = std::chrono::minutes;

struct Thread_Info
{
  /** Default constructor */
  Thread_Info() = default;

  /** Copy constructor needed for std::vector. */
  Thread_Info(const Thread_Info &) { reset(); } /* purecov: inspected */

  /** Reset transferred data bytes. */
  void reset() {
    m_last_update = Clock::now();
    m_last_data_bytes = 0;
    m_data_bytes.store(0);
  }

  /** Update transferred data bytes.
  @param[in]	data_bytes	data bytes transferred
  @param[in]	net_bytes	network bytes transferred */
  void update(uint64_t data_bytes) {
    m_data_bytes.fetch_add(data_bytes);
  }

  /** Calculate the expected time for transfer based on target.
  @param[in]	current	current number of transferred data bytes
  @param[in]	prev	previous number of transferred data bytes
  @param[in]	target	target data transfer rate in bytes per second
  @return expected time in milliseconds. */
  uint64_t get_target_time(uint64_t current, uint64_t prev, uint64_t target);

  /** Check target transfer speed and throttle if needed. The thread sleeps
  for appropriate time if the current transfer rate is more than target.
  @param[in]	data_target	target data bytes transfer per second
  */
  void throttle(uint64_t data_target);

  /** Data transfer throttle interval */
  Time_Msec m_interval{100};

  /** Current thread */
  std::thread m_thread;

  /** Last time information was updated. */
  Time_Point m_last_update;

  /** Data bytes at last update. */
  uint64_t m_last_data_bytes{};

  /** Total amount of data transferred. */
  std::atomic<uint64_t> m_data_bytes;
};

/** Thread information vector. */
using Thread_Vector = std::vector<Thread_Info>;

/** Maximum size of history data */
const size_t STAT_HISTORY_SIZE = 16;

/** Client data transfer statistics. */
class Client_Stat
{
 public:
  /** Update statistics data.
  @param[in]	reset		reset all previous history
  @param[in]	threads		all concurrent thread information
  @param[in]	num_workers	current number of worker threads */
  void update(bool reset, const Thread_Vector &threads, uint32_t num_workers);

  /** Get target speed, in case user has specified limits.
  @param[out]	data_speed	target data transfer in bytes/sec */
  void get_target(uint64_t &data_speed) const {
    data_speed = m_target_data_speed.load();
  }

  /** Initialize target speed read by all threads. Adjusted later based on
  maximum bandwidth threads. Zero implies unlimited bandwidth. */
  void init_target() {
    m_target_data_speed.store(0);
  }

  /** Save finished byte stat when thread info is released. It is
  used during clone restart after network failure.
  @param[in]	data_bytes	data bytes to save */
  void save_at_exit(uint64_t data_bytes) {
    m_finished_data_bytes += data_bytes;
  }

  /** Reset history elements.
  @param[in]	init	true, if called during initialization */
  void reset_history(bool init);

 private:
  /** Calculate target for each task based on current performance.
  @param[in]	target_speed	overall target speed in bytes per second
  @param[in]	current_speed	overall current speed in bytes per second
  @param[in]	current_target	current target for a task in bytes per second
  @param[in]	num_tasks	number of clone tasks
  @return target for a task in bytes per second. */
  uint64_t task_target(uint64_t target_speed, uint64_t current_speed,
                       uint64_t current_target, uint32_t num_tasks);

  /** Set target bandwidth for data per thread.
  @param[in]	num_workers	current number of worker threads
  @param[in]	is_reset	if called during stage reset
  @param[in]	data_speed	current data speed in bytes per second */
  void set_target_bandwidth(uint32_t num_workers, bool is_reset,
                            uint64_t data_speed);

 private:
  /** Statistics update interval - 1 sec*/
  const Time_Msec m_interval{1000};

  /** Minimum data transfer rate per task - 1M */
  const uint64_t m_minimum_speed = 1048576;

  /* If stat elements are initialized. */
  bool m_initialized{false};

  /** Starting point for clone data transfer. */
  Time_Point m_start_time;

  /** Last evaluation time */
  Time_Point m_eval_time;

  /** Data transferred at last evaluation time. */
  uint64_t m_eval_data_bytes{};

  /** All data bytes transferred by threads already finished. */
  uint64_t m_finished_data_bytes{};

  /** Data speed history. */
  std::array<uint64_t, STAT_HISTORY_SIZE> m_data_speed_history{};

  /** Current index for history data. */
  size_t m_current_history_index{};

  /** Target data bytes to be transferred per thread per second. */
  std::atomic<uint64_t> m_target_data_speed;
};

class Exec_State
{
 public:
  /** Wait till current state is greater or equal to passed state. It is used
  by worker threads before starting work for certain execution state. The
  state is set by master. Attach to the current state.
  @param state state to wait for on input, attached state on output
  @return error code. */
  int begin_worker(Sub_Command &state);

  /** Detach worker from current execution state.
  @param state state to detach from, must match the current execution state
  @return error code. */
  int end_worker(Sub_Command state);

  /** Wait for all workers to finish current state and set the new state. It is
  used by master to drive state transition.
  @param thd THD to check for interrupt
  @param state state to set.
  @return error code. */
  int switch_state(THD *thd, Sub_Command next_state);

  /** Update current state. Called after acquiring locks for a state.
  @param sub_state execution state
  @return true if successful */
  bool update_current_state(Sub_Command sub_state);

 private:
  /** Protects the state and counters. */
  std::mutex m_mutex;

  /** Condition variable for workers to wait for a state to begin. */
  std::condition_variable m_wait_state;

  /** Condition variable for master to wait for workers to finish state. */
  std::condition_variable m_wait_count;

  /** Current execution state. Protected by m_mutex. */
  Sub_Command m_cur_state= SUBCOM_NONE;

  /** Next execution state. Protected by m_mutex. */
  Sub_Command m_next_state= SUBCOM_NONE;

  /** Worker count within a state. Protected by m_mutex. */
  uint32_t m_count_workers[static_cast<size_t>(SUBCOM_MAX) + 1]= {0};
};

/* Shared client information for multi threaded clone */
struct Client_Share
{
  /** Construct clone client share. Initialize storage handle.
  @param[in]	host	remote host IP address
  @param[in]	port	remote server port
  @param[in]	user	remote user name
  @param[in]	passwd	remote user's password
  @param[in]	dir	target data directory for clone
  @param[in]	mode	client SSL mode */
  Client_Share(const char *host, const uint port, const char *user,
               const char *passwd, const char *dir, int mode)
      : m_host(host),
        m_port(port),
        m_user(user),
        m_passwd(passwd),
        m_data_dir(dir),
        m_protocol_version(CLONE_PROTOCOL_VERSION)
  {
    m_storage_vec.reserve(MAX_CLONE_STORAGE_ENGINE);
    m_threads.resize(1);
    m_stat.init_target();
  }

  /** Remote host name */
  const char *m_host;

  /** Remote port */
  const uint32_t m_port;

  /** Remote user name */
  const char *m_user;

  /** Remote user password */
  const char *m_passwd;

  /** Cloned database directory */
  const char *m_data_dir;

  /** Negotiated protocol version */
  uint32_t m_protocol_version;

  /** Clone storage vector */
  Storage_Vector m_storage_vec;

  /** Thread vector for multi threaded clone. */
  Thread_Vector m_threads;

  /** Data transfer statistics. */
  Client_Stat m_stat;

  /** Execution State. */
  Exec_State m_state;
};

/** For Remote Clone, "Clone Client" is created at recipient. It receives data
over network from remote "Clone Server" and applies to Storage Engines. */
class Client
{
 public:
  /** Construct clone client. Initialize external handle.
  @param[in,out]	thd		server thread handle
  @param[in]		share		shared client information
  @param[in]		index		current thread index
  @param[in]		is_master	if it is master thread */
  Client(THD *thd, Client_Share *share, uint32_t index, bool is_master);

  /** Destructor: Free the transfer buffer, if created. */
  ~Client();

  /** Check if it is the master client object.
  @return true if this is the master object */
  bool is_master() const { return (m_is_master); }

  /** @return current thread information. */
  Thread_Info &get_thread_info() {
    return (m_share->m_threads[m_thread_index]);
  }

  /** Update statistics and tune threads
  @param[in]	is_reset	reset statistics
  @return tuned number of worker threads. */
  uint32_t update_stat(bool is_reset);

  /** Check transfer speed and throttle. */
  void check_and_throttle();

  /** Get Shared area for client tasks
  @return shared client data */
  Client_Share *get_share() { return (m_share); }

  /** Get storage handle vector for data transfer.
  @return storage handle vector */
  Storage_Vector &get_storage_vector() { return (m_share->m_storage_vec); }

  /** Get tasks for different SE
  @return task vector */
  Task_Vector &get_task_vector() { return (m_tasks); }

  /** Get external handle for data transfer. This is file
  or buffer for local clone and network socket to remote server
  for remote clone.
  @param[out]	conn	connection handle to remote server
  @return external handle */
  Data_Link *get_data_link(MYSQL *&conn) {
    conn = m_conn;
    return (&m_ext_link);
  }

  /** Get server thread handle
  @return server thread */
  THD *get_thd() { return (m_server_thd); }

  /** Get target clone data directory
  @return data directory */
  const char *get_data_dir() const { return (m_share->m_data_dir); }

  /** Get clone locator for a storage engine at specified index.
  @param[in]	index	locator index
  @param[out]	loc_len	locator length in bytes
  @return storage locator */
  const uchar *get_locator(uint index, uint &loc_len) const {
    assert(index < m_share->m_storage_vec.size());

    loc_len = m_share->m_storage_vec[index].m_loc_len;
    return (m_share->m_storage_vec[index].m_loc);
  }

  /** Get aligned intermediate buffer for transferring data. Allocate,
  when called for first time.
  @param[in]	len	length of allocated buffer
  @return allocated buffer pointer */
  uchar *get_aligned_buffer(uint32_t len);

  /** Limit total memory used for clone transfer buffer.
  @param[in]	buffer_size	configured buffer size
  @return actual buffer size to allocate. */
  uint32_t limit_buffer(uint32_t buffer_size);

  /* Spawn worker threads.
  @param[in]	num_workers	number of worker threads
  @param[in]	func		worker function */
  template <typename F>
  void spawn_workers(uint32_t num_workers, F func) {
    /* Currently we don't reduce the number of threads. */
    if (!is_master() || num_workers <= m_num_active_workers) {
      return;
    }
    auto &thread_vector = m_share->m_threads;

    while (m_num_active_workers < num_workers) {
      ++m_num_active_workers;
      auto &info = thread_vector[m_num_active_workers];
      info.reset();
      try {
        info.m_thread = std::thread(func, m_share, m_num_active_workers);
      } catch (...) {
        /* purecov: begin deadcode */
        char info_mesg[64];
        snprintf(info_mesg, sizeof(info_mesg), "Failed to spawn worker: %d",
                 m_num_active_workers);
        LogPluginErr(INFORMATION_LEVEL, ER_CLONE_CLIENT_TRACE, info_mesg);

        --m_num_active_workers;
        break;
        /* purecov: end */
      }
    }
  }

  /** Wait for worker threads to finish. */
  void wait_for_workers();

  /** Execute clone moving through all execution states.
  @param cbk callback function for executing one state
  @return error code */
  int execute(std::function<int(Sub_Command)> cbk);

  /** Begin state in PFS table.
  @return error code. */
  int pfs_begin_state();

  /** Change stage in PFS progress table. */
  void pfs_change_stage(uint64_t estimate);

  /** End state in PFS table.
  @param[in]	err_num		error number
  @param[in]	err_mesg	error message */
  void pfs_end_state(uint32_t err_num, const char *err_mesg);

  /** Copy PFS status data safely.
  @param[out]	pfs_data	status data. */
  static void copy_pfs_data(Status_pfs::Data &pfs_data);

  /** Copy PFS progress data safely.
  @param[out]	pfs_data	progress data. */
  static void copy_pfs_data(Progress_pfs::Data &pfs_data);

  /** Update data and network consumed.
  @param[in]	data		data bytes transferred
  @param[in]	data_speed	data transfer speed in bytes/sec
  @param[in]	net_speed	network transfer speed in bytes/sec
  @param[in]	num_workers	number of worker threads */
  static void update_pfs_data(uint64_t data, uint32_t data_speed,
                              uint32_t num_workers);

  /** Init PFS mutex for table. */
  static void init_pfs();

  /** Destroy PFS mutex for table. */
  static void uninit_pfs();

 private:
  /** Begin a clone execution state.
  @param thd THD to check for interrupt
  @param sub_state execution state
  @return error code */
  int exec_begin_state(THD *thd, Sub_Command &sub_state);

  /* End clone execution state.
  @param sub_state execution state
  @return error code */
  int exec_end_state(Sub_Command sub_state);

  /** Check if the state should skipped. Currently only master
  thread needs to take the snapshot.
  @param sub_state execution state
  @return true if the state needs to be skipped */
  bool skip_state(Sub_Command sub_state) const
  {
    return (!is_master() && sub_state == SUBCOM_EXEC_SNAPSHOT);
  }

  /** If PFS table and mutex is initialized. */
  static bool s_pfs_initialized;

 private:
  /** Clone status table data. */
  static Status_pfs::Data s_status_data;

  /** Clone progress table data. */
  static Progress_pfs::Data s_progress_data;

  /** Clone table mutex to protect PFS table data. */
  static mysql_mutex_t s_table_mutex;

  /** Number of concurrent clone clients. */
  static uint32_t s_num_clones;

  /** Interval for attempting re-connect after failure. */
  static Time_Sec s_reconnect_interval;

 private:
  /** Server thread object */
  THD *m_server_thd;

  /** Clone remote client connection */
  MYSQL *m_conn;
  NET_SERVER m_conn_server_extn;

  /** Intermediate buffer for data copy when zero copy is not used. */
  Buffer m_copy_buff;

  /** Buffer holding data for RPC command */
  Buffer m_cmd_buff;

  /** Clone external handle. Data is transferred from
  external handle(network) to storage handle. */
  Data_Link m_ext_link;

  /** If it is the master thread */
  bool m_is_master;

  /** Thread index for multi-threaded clone */
  uint32_t m_thread_index;

  /** Number of active worker tasks. */
  uint32_t m_num_active_workers;

  /** Task IDs for different SE */
  Task_Vector m_tasks;

  /** Storage is initialized */
  bool m_storage_initialized;

  /** Storage is active with locators set */
  bool m_storage_active;

  /** If backup lock is acquired */
  bool m_acquired_backup_lock;

  /** Shared client information */
  Client_Share *m_share;
};

}  // namespace myclone

#endif /* CLONE_CLIENT_H */
