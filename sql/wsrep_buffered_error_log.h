/* Copyright (C) 2024 Codership Oy <info@codership.com>

   Created by Zsolt Parragi <zsolt.parragi@percona.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */
#ifndef WSREP_BUFFERED_ERROR_LOG_H
#define WSREP_BUFFERED_ERROR_LOG_H 1

#include <stdio.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdio>
#include <string>

/** Stores log messages in a fixed size buffer, which can be dumped by large
 * chunks instead of line by line, to increase performance for high throughput
 * scenarios.
 *
 * The buffer is also dumped by the crash reporting code and during shutdown to
 * ensure that nothing is lost, only delayed.
 */
class Buffered_error_logger {
  // We are using a pointer to string to guarantee that we can decrease the size
  // of the buffer
  using data_t = std::unique_ptr<std::string>;
  // memory buffer storing the log messages
  std::mutex data_mtx;
  data_t data;

 public:
  void resize(std::size_t buffer_size);

  ~Buffered_error_logger();

  void log(const char *msg, size_t len);

  void write_to_disk();

  void close();

  bool is_enabled();

 private:
  // requires holding data_mtx
  void write_to_disk_();
};

#endif /* WSREP_BUFFERED_ERROR_LOG */
