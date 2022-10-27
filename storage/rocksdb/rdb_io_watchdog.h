/*
   Copyright (c) 2017, Facebook, Inc.

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
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <atomic>
#include <string>
#include <vector>

/* MySQL header files */
#include "./my_global.h"
#include "./my_stacktrace.h"

/* MyRocks header files */
#include "./rdb_utils.h"

namespace myrocks {

// Rdb_io_watchdog does not support Windows ATM.
#ifdef HAVE_TIMER_DELETE

class Rdb_io_watchdog {
  const int RDB_IO_WRITE_BUFFER_SIZE = 4096;
  const char *const RDB_IO_DUMMY_FILE_NAME = "myrocks_io_watchdog_write_file";

 private:
  timer_t m_io_check_timer, m_io_check_watchdog_timer;
  std::atomic<bool> m_io_in_progress;
  std::vector<std::string> m_dirs_to_check;
  uint32_t m_write_timeout;
  mysql_mutex_t m_reset_mutex;
  char *m_buf;

  int check_write_access(const std::string &dirname) const;
  void io_check_callback(union sigval timer_data);
  void expire_io_callback(union sigval timer_data);

  int stop_timers() {
    int ret = 0;

    if (m_io_check_watchdog_timer) {
      ret = timer_delete(m_io_check_watchdog_timer);

      if (!ret) {
        m_io_check_watchdog_timer = nullptr;
      }
    }

    if (m_io_check_timer && !ret) {
      ret = timer_delete(m_io_check_timer);

      if (!ret) {
        m_io_check_timer = nullptr;
      }
    }

    return ret;
  }

  static void io_check_callback_wrapper(union sigval timer_data) {
    Rdb_io_watchdog *io_watchdog =
        static_cast<Rdb_io_watchdog *>(timer_data.sival_ptr);
    DBUG_ASSERT(io_watchdog != nullptr);

    io_watchdog->io_check_callback(timer_data);
  }

  static void expire_io_callback_wrapper(union sigval timer_data) {
    Rdb_io_watchdog *io_watchdog =
        static_cast<Rdb_io_watchdog *>(timer_data.sival_ptr);
    DBUG_ASSERT(io_watchdog != nullptr);

    io_watchdog->expire_io_callback(timer_data);
  }

 public:
  explicit Rdb_io_watchdog(std::vector<std::string> &&directories)
      : m_io_check_timer(nullptr),
        m_io_check_watchdog_timer(nullptr),
        m_io_in_progress(false),
        m_dirs_to_check(std::move(directories)),
        m_buf(nullptr) {
    DBUG_ASSERT(m_dirs_to_check.size() > 0);
    mysql_mutex_init(0, &m_reset_mutex, MY_MUTEX_INIT_FAST);
  }

  ~Rdb_io_watchdog() {
    // We're shutting down. Ignore errors possibly coming from timer deletion.
    static_cast<void>(stop_timers());
    mysql_mutex_destroy(&m_reset_mutex);
    free(m_buf);
  }

  int reset_timeout(const uint32_t write_timeout);

  Rdb_io_watchdog(const Rdb_io_watchdog &) = delete;
  Rdb_io_watchdog &operator=(const Rdb_io_watchdog &) = delete;
};

#endif 
}  // namespace myrocks
