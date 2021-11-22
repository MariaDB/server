/* Copyright 2018 Codership Oy <info@codership.com>

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

#ifndef WSREP_SERVER_STATE_H
#define WSREP_SERVER_STATE_H

/* wsrep-lib */
#include "wsrep/server_state.hpp"
#include "wsrep/provider.hpp"

/* implementation */
#include "wsrep_server_service.h"
#include "wsrep_mutex.h"
#include "wsrep_condition_variable.h"

class Wsrep_server_state : public wsrep::server_state
{
public:
  static void init_once(const std::string& name,
                        const std::string& incoming_address,
                        const std::string& address,
                        const std::string& working_dir,
                        const wsrep::gtid& initial_position,
                        int max_protocol_version);
  static void destroy();

  static Wsrep_server_state& instance()
  {
    return *m_instance;
  }

  static bool is_inited()
  {
    return (m_instance != NULL);
  }

  static wsrep::provider& get_provider()
  {
    return instance().provider();
  }

  static bool has_capability(int capability)
  {
    return (get_provider().capabilities() & capability);
  }
  
  static void init_provider_services();
  static void deinit_provider_services();

  static const wsrep::provider::services& provider_services()
  {
    return m_provider_services;
  }

private:
  Wsrep_server_state(const std::string& name,
                     const std::string& incoming_address,
                     const std::string& address,
                     const std::string& working_dir,
                     const wsrep::gtid& initial_position,
                     int max_protocol_version);
  ~Wsrep_server_state();
  Wsrep_mutex m_mutex;
  Wsrep_condition_variable m_cond;
  Wsrep_server_service m_service;
  static wsrep::provider::services m_provider_services;
  static Wsrep_server_state* m_instance;

};

#endif // WSREP_SERVER_STATE_H
