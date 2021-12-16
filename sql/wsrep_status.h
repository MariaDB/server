/* Copyright 2021 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef WSREP_STATUS_H
#define WSREP_STATUS_H

/* wsrep-lib */
#include "wsrep/reporter.hpp"

/* implementation */
#include "wsrep_mutex.h"

class Wsrep_status
{
public:
  static void init_once(const std::string& file_name);
  static void destroy();

  static void report_state(enum wsrep::server_state::state const state)
  {
    if (!Wsrep_status::m_instance) return;

    Wsrep_status::m_instance->report_state(state);
  }

  static void report_progress(const std::string& progress)
  {
    if (!Wsrep_status::m_instance) return;

    Wsrep_status::m_instance->report_progress(progress);
  }

  static void report_log_msg(wsrep::reporter::log_level level,
                             const char* tag, size_t tag_len,
                             const char* buf, size_t buf_len,
                             double const tstamp = wsrep::reporter::undefined);

  static bool is_instance_initialized()
  {
      return m_instance;
  }

private:
  Wsrep_status(const std::string& file_name);

  static Wsrep_mutex*     m_mutex;
  static wsrep::reporter* m_instance;
};

#endif /* WSREP_STATUS_H */
