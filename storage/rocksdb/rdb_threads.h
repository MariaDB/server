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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
#pragma once

/* C++ standard header files */
#include <string>

/* MySQL includes */
#include "./my_global.h"
#include <mysql/psi/mysql_table.h>
#include <mysql/thread_pool_priv.h>

/* MyRocks header files */
#include "./rdb_utils.h"

namespace myrocks {

class Rdb_thread {
private:
  // Disable Copying
  Rdb_thread(const Rdb_thread &);
  Rdb_thread &operator=(const Rdb_thread &);

  // Make sure we run only once
  std::atomic_bool m_run_once;

  pthread_t m_handle;

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

  void signal(const bool &stop_thread = false);

  int join() { return pthread_join(m_handle, nullptr); }

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
    mysql_mutex_lock(&m_signal_mutex);
    m_save_stats = true;
    mysql_mutex_unlock(&m_signal_mutex);
  }
};

/*
  Drop index thread control
*/

struct Rdb_drop_index_thread : public Rdb_thread {
  virtual void run() override;
};

} // namespace myrocks
