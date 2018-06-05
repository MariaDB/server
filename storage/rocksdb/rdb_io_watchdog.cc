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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* This C++ file's header */
#include "./rdb_io_watchdog.h"

/* C++ standard header files */
#include <string>
#include <vector>

/* Rdb_io_watchdog doesn't work on Windows [yet] */
#ifdef HAVE_TIMER_DELETE

namespace myrocks {

void Rdb_io_watchdog::expire_io_callback(union sigval timer_data) {
  DBUG_ASSERT(timer_data.sival_ptr != nullptr);

  // The treatment of any pending signal generated by the deleted timer is
  // unspecified. Therefore we still need to handle the rare case where we
  // finished the I/O operation right before the timer was deleted and callback
  // was in flight.
  if (!m_io_in_progress.load()) {
    return;
  }

  // At this point we know that I/O has been stuck in `write()` for more than
  // `m_write_timeout` seconds. We'll log a message and shut down the service.
  // NO_LINT_DEBUG
  sql_print_error("MyRocks has detected a combination of I/O requests which "
                  "have cumulatively been blocking for more than %u seconds. "
                  "Shutting the service down.",
                  m_write_timeout);

  abort();
}

void Rdb_io_watchdog::io_check_callback(union sigval timer_data) {
  RDB_MUTEX_LOCK_CHECK(m_reset_mutex);

  DBUG_ASSERT(timer_data.sival_ptr != nullptr);

  struct sigevent e;

  e.sigev_notify = SIGEV_THREAD;
  e.sigev_notify_function = &Rdb_io_watchdog::expire_io_callback_wrapper;
  e.sigev_value.sival_ptr = this;
  e.sigev_notify_attributes = nullptr;

  int ret = timer_create(CLOCK_MONOTONIC, &e, &m_io_check_watchdog_timer);

  if (unlikely(ret)) {
    // NO_LINT_DEBUG
    sql_print_warning("Creating a watchdog I/O timer failed with %d.", errno);
    RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
    return;
  }

  struct itimerspec timer_spec;
  memset(&timer_spec, 0, sizeof(timer_spec));

  // One time execution only for the watchdog. No interval.
  timer_spec.it_value.tv_sec = m_write_timeout;

  ret = timer_settime(m_io_check_watchdog_timer, 0, &timer_spec, nullptr);

  if (unlikely(ret)) {
    // NO_LINT_DEBUG
    sql_print_warning("Setting time for a watchdog I/O timer failed with %d.",
                      errno);
    RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
    return;
  }

  m_io_in_progress.store(true);

  // Verify the write access to all directories we care about.
  for (const std::string &directory : m_dirs_to_check) {
    ret = check_write_access(directory);

    // We'll log a warning and attept to continue to see if the problem happens
    // in other cases as well.
    if (unlikely(ret != HA_EXIT_SUCCESS)) {
      // NO_LINT_DEBUG
      sql_print_warning("Unable to verify write access to %s (error code %d).",
                        directory.c_str(), ret);
    }
  }

  m_io_in_progress.store(false);

  // Clean up the watchdog timer.
  ret = timer_delete(m_io_check_watchdog_timer);

  if (unlikely(ret)) {
    // NO_LINT_DEBUG
    sql_print_warning("Deleting the watchdog I/O timer failed with %d.", errno);
  }

  m_io_check_watchdog_timer = nullptr;

  RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
}

int Rdb_io_watchdog::check_write_access(const std::string &dirname) const {
  DBUG_ASSERT(!dirname.empty());
  DBUG_ASSERT(m_buf != nullptr);

  const std::string fname = dirname + FN_DIRSEP + RDB_IO_DUMMY_FILE_NAME;

  // O_DIRECT is a key flag here to make sure that we'll bypass the kernel's
  // buffer cache.
  int fd = open(fname.c_str(), O_WRONLY | O_DIRECT | O_CREAT | O_SYNC,
                S_IRWXU | S_IWUSR);

  if (unlikely(fd == -1)) {
    return fd;
  }

  int ret = write(fd, m_buf, RDB_IO_WRITE_BUFFER_SIZE);

  if (unlikely(ret != RDB_IO_WRITE_BUFFER_SIZE)) {
    return ret;
  }

  ret = close(fd);

  if (unlikely(ret)) {
    return ret;
  }

  ret = unlink(fname.c_str());

  if (unlikely(ret)) {
    return ret;
  }

  return HA_EXIT_SUCCESS;
}

int Rdb_io_watchdog::reset_timeout(const uint32_t &write_timeout) {
  // This function will be called either from a thread initializing MyRocks
  // engine or handling system variable changes. We need to account for the
  // possibility of I/O callback executing at the same time. If that happens
  // then we'll wait for it to finish.
  RDB_MUTEX_LOCK_CHECK(m_reset_mutex);

  struct sigevent e;

  // In all the cases all the active timers needs to be stopped.
  int ret = stop_timers();

  if (unlikely(ret)) {
    // NO_LINT_DEBUG
    sql_print_warning("Stopping I/O timers failed with %d.", errno);
    RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
    return ret;
  }

  m_write_timeout = write_timeout;
  m_io_in_progress.store(false);

  // Zero means that the I/O timer will be disabled. Therefore there's nothing
  // for us to do here.
  if (!write_timeout) {
    RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
    return HA_EXIT_SUCCESS;
  }

  free(m_buf);

  ret = posix_memalign(reinterpret_cast<void **>(&m_buf),
                       RDB_IO_WRITE_BUFFER_SIZE, RDB_IO_WRITE_BUFFER_SIZE);

  if (unlikely(ret)) {
    m_buf = nullptr;
    RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
    // NB! The value of errno is not set.
    return ret;
  }

  DBUG_ASSERT(m_buf != nullptr);
  memset(m_buf, 0, RDB_IO_WRITE_BUFFER_SIZE);

  // Common case gets handled here - we'll create a timer with a specific
  // interval to check a set of directories for write access.
  DBUG_ASSERT(m_dirs_to_check.size() > 0);

  e.sigev_notify = SIGEV_THREAD;
  e.sigev_notify_function = &Rdb_io_watchdog::io_check_callback_wrapper;
  e.sigev_value.sival_ptr = this;
  e.sigev_notify_attributes = nullptr;

  ret = timer_create(CLOCK_MONOTONIC, &e, &m_io_check_timer);

  if (unlikely(ret)) {
    // NO_LINT_DEBUG
    sql_print_warning("Creating a I/O timer failed with %d.", errno);
    RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);
    return ret;
  }

  struct itimerspec timer_spec;
  memset(&timer_spec, 0, sizeof(timer_spec));

  // I/O timer will need to execute on a certain interval.
  timer_spec.it_value.tv_sec = m_write_timeout;
  timer_spec.it_interval.tv_sec = m_write_timeout;

  ret = timer_settime(m_io_check_timer, 0, &timer_spec, nullptr);

  if (unlikely(ret)) {
    // NO_LINT_DEBUG
    sql_print_warning("Setting time for a watchdog I/O timer failed with %d.",
                      errno);
  }

  RDB_MUTEX_UNLOCK_CHECK(m_reset_mutex);

  return HA_EXIT_SUCCESS;
}

}  // namespace myrocks

#endif

