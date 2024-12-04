/* Copyright (C) 2024 Codership Oy <info@codership.com>

   Created by Zsolt Parragi <zsolt.parragi@percona.com>
   Modified by Jan Lindström <jan.lindstrom@galeracluster.com>

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

#include "wsrep_buffered_error_log.h"
#include "wsrep.h"
#include "wsrep_mysqld.h"

Buffered_error_logger wsrep_buffered_error_log;
const char *wsrep_buffered_error_log_filename= nullptr;
long long wsrep_buffered_error_log_buffer_size= 0;
long long wsrep_buffered_error_log_file_size= 0;
uint wsrep_buffered_error_log_rotations= 0;

void Buffered_error_logger::init(std::size_t buffer_size, std::size_t file_size)
{
  if (buffer_size == 0 || file_size == 0 || wsrep_buffered_error_log_filename == nullptr)
    return;
  if (strlen(wsrep_buffered_error_log_filename) == 0)
    return;

  logger_init_mutexes();
  logfile= logger_open(wsrep_buffered_error_log_filename, file_size,
                       wsrep_buffered_error_log_rotations);

  if (!logfile)
  {
    WSREP_WARN("Could not open buffered error log %s. Buffered error logging disabled.");
    wsrep_debug_mode &= ~WSREP_DEBUG_MODE_BUFFERED;
    return;
  }

  max_file_size= file_size;
  rotations= wsrep_buffered_error_log_rotations;
  resize_buffer(buffer_size);
}

void Buffered_error_logger::resize_buffer(std::size_t buffer_size)
{
  std::lock_guard<std::mutex> lk{data_mtx};

  if (data.get() != nullptr && buffer_size == data->capacity())
    return;

  // Write out what we have currently, that way we don't have
  // to deal with old data in the buffer
  write_to_disk_();

  if (buffer_size == 0)
  {
    data.reset();
    return;
  }

  data_t new_buffer(new std::string());
  new_buffer->reserve(buffer_size);
  data.swap(new_buffer);
}

void Buffered_error_logger::resize_file_size(std::size_t file_size)
{
  std::lock_guard<std::mutex> lk{data_mtx};

  if (file_size == max_file_size)
    return;

  max_file_size= file_size;
  logger_close(logfile);
  logfile= logger_open(wsrep_buffered_error_log_filename, file_size,
                       wsrep_buffered_error_log_rotations);

  if (!logfile)
  {
    WSREP_WARN("Could not open buffered error log %s. Buffered error logging disabled.");
    wsrep_debug_mode &= ~WSREP_DEBUG_MODE_BUFFERED;
  }
}

Buffered_error_logger::~Buffered_error_logger()
{
  write_to_disk();
  if (logfile)
    logger_close(logfile);
  logfile= nullptr;
}

void Buffered_error_logger::log(const char *msg, size_t len)
{
  std::lock_guard<std::mutex> lk{data_mtx};

  if (data.get() == nullptr || data->capacity() == 0)
    return;

  const auto msg_end = data->size() + len;

  if (msg_end > data->capacity() - 1)
    write_to_disk_();

  *data += msg;
}

void Buffered_error_logger::write_to_disk()
{
  if (wsrep_buffered_error_log_buffer_size)
  {
    std::lock_guard<std::mutex> lk{data_mtx};
    write_to_disk_();
  }
}

void Buffered_error_logger::close()
{
  std::lock_guard<std::mutex> lk{data_mtx};
  if (logfile)
  {
    logger_close(logfile);
    logfile= nullptr;
  }
}

bool Buffered_error_logger::is_enabled()
{
  std::lock_guard<std::mutex> lk{data_mtx};
  return data.get() != nullptr && data->size() != 0;
}

void Buffered_error_logger::write_to_disk_()
{
  if (wsrep_buffered_error_log_filename == nullptr ||
      strlen(wsrep_buffered_error_log_filename) == 0)
    return;

  if (data.get() == nullptr || data->size() == 0)
    return;

  const size_t curr_size= data->capacity();

  if (logfile)
    logger_write(logfile, data->data(), data->size());

  data->clear();
  data->reserve(curr_size);
}

/* As this is used in signal handler, no mutexes
   or file rotation is done.
*/
void Buffered_error_logger::write_to_disk_safe()
{
  if (wsrep_buffered_error_log_filename == nullptr ||
      strlen(wsrep_buffered_error_log_filename) == 0)
    return;

  if (!wsrep_buffered_error_log_buffer_size)
    return;

  if (data.get() == nullptr || data->size() == 0)
    return;

  const size_t curr_size= data->capacity();

  if (logfile)
    logger_write(logfile, data->data(), data->size());

  data->clear();
  data->reserve(curr_size);
}

void Buffered_error_logger::rotate(uint n_rotations)
{
  if (!logfile || n_rotations == rotations)
    return;
  std::lock_guard<std::mutex> lk{data_mtx};
  logger_close(logfile);
  logfile= logger_open(wsrep_buffered_error_log_filename,
		       wsrep_buffered_error_log_file_size,
                       wsrep_buffered_error_log_rotations);
  if (!logfile)
  {
    WSREP_WARN("Could not open buffered error log %s. Buffered error logging disabled.");
    wsrep_debug_mode &= ~WSREP_DEBUG_MODE_BUFFERED;
  }
}
