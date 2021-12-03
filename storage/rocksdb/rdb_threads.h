/*
   Portions Copyright (c) 2015-Present, Facebook, Inc.
   Portions Copyright (c) 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */
#pragma once

/* C++ standard header files */
#include <map>
#include <string>

/* MySQL includes */
#include "./my_global.h"
#ifdef _WIN32
#include <my_pthread.h>
/*
 Rocksdb implements their own pthread_key functions
 undefine some my_pthread.h macros
*/
#undef pthread_key_create
#undef pthread_key_delete
#undef pthread_setspecific
#undef pthread_getspecific
#endif
#include <mysql/psi/mysql_table.h>
// #include <mysql/thread_pool_priv.h>

/* MyRocks header files */
#include "./rdb_utils.h"
#include "rocksdb/db.h"

namespace myrocks {

class Rdb_thread {
 private:
  // Disable Copying
  Rdb_thread(const Rdb_thread &);
  Rdb_thread &operator=(const Rdb_thread &);

  // Make sure we run only once
  std::atomic_bool m_run_once;

  pthread_t m_handle;

  std::string m_name;

 protected:
  mysql_mutex_t m_signal_mutex;
  mysql_cond_t m_signal_cond;
  bool m_stop = false;

 public:
  Rdb_thread() : m_run_once(false) {}

#ifdef HAVE_PSI_INTERFACE
  void init(my_core::PSI_mutex_key stop_bg_psi_mutex_key,
            my_core::PSI_cond_key stop_bg_psi_cond_key);
  int create_thread(const std::string &thread_name,
                    my_core::PSI_thread_key background_psi_thread_key);
#else
  void init();
  int create_thread(const std::string &thread_name);
#endif

  virtual void run(void) = 0;

  void signal(const bool stop_thread = false);

  int join()
  {
#ifndef _WIN32
    return pthread_join(m_handle, nullptr);
#else
    /*
      mysys on Windows creates "detached" threads in pthread_create().

      m_handle here is the thread id I(it is not reused by the OS
      thus it is safe to state there can't be other thread with
      the same id at this point).

      If thread is already finished before pthread_join(),
      we get EINVAL, and it is safe to ignore and handle this as success.
    */
    pthread_join(m_handle, nullptr);
    return 0;
#endif
  }

  void setname() {
    /*
      mysql_thread_create() ends up doing some work underneath and setting the
      thread name as "my-func". This isn't what we want. Our intent is to name
      the threads according to their purpose so that when displayed under the
      debugger then they'll be more easily identifiable. Therefore we'll reset
      the name if thread was successfully created.
    */

    /*
      We originally had the creator also set the thread name, but that seems to
      not work correctly in all situations.  Having the created thread do the
      pthread_setname_np resolves the issue.
    */
    DBUG_ASSERT(!m_name.empty());
#ifdef __linux__
    int err = pthread_setname_np(m_handle, m_name.c_str());
    if (err) {
      // NO_LINT_DEBUG
      sql_print_warning(
          "MyRocks: Failed to set name (%s) for current thread, errno=%d,%d",
          m_name.c_str(), errno, err);
    }
#endif
  }

  void uninit();

  virtual ~Rdb_thread() {}

 private:
  static void *thread_func(void *const thread_ptr);
};

/**
  MyRocks background thread control
  N.B. This is on top of RocksDB's own background threads
       (@see rocksdb::CancelAllBackgroundWork())
*/

class Rdb_background_thread : public Rdb_thread {
 private:
  bool m_save_stats = false;

  void reset() {
    mysql_mutex_assert_owner(&m_signal_mutex);
    m_stop = false;
    m_save_stats = false;
  }

 public:
  virtual void run() override;

  void request_save_stats() {
    RDB_MUTEX_LOCK_CHECK(m_signal_mutex);

    m_save_stats = true;

    RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);
  }
};

class Rdb_manual_compaction_thread : public Rdb_thread {
 private:
  struct Manual_compaction_request {
    int mc_id;
    enum mc_state { INITED = 0, RUNNING } state;
    rocksdb::ColumnFamilyHandle *cf;
    rocksdb::Slice *start;
    rocksdb::Slice *limit;
    int concurrency = 0;
  };

  int m_latest_mc_id;
  mysql_mutex_t m_mc_mutex;
  std::map<int, Manual_compaction_request> m_requests;

 public:
  virtual void run() override;
  int request_manual_compaction(rocksdb::ColumnFamilyHandle *cf,
                                rocksdb::Slice *start, rocksdb::Slice *limit,
                                int concurrency = 0);
  bool is_manual_compaction_finished(int mc_id);
  void clear_manual_compaction_request(int mc_id, bool init_only = false);
  void clear_all_manual_compaction_requests();
};

/*
  Drop index thread control
*/

struct Rdb_drop_index_thread : public Rdb_thread {
  virtual void run() override;
};

}  // namespace myrocks
