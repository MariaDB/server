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

#ifndef WSREP_SERVER_SERVICE_H
#define WSREP_SERVER_SERVICE_H

/* wsrep-lib */
#include "wsrep/server_service.hpp"
#include "wsrep/exception.hpp" // not_impemented_error(), remove when finished
#include "wsrep/storage_service.hpp"

class Wsrep_server_state;


/* wsrep::server_service interface implementation */
class Wsrep_server_service : public wsrep::server_service
{
public:
  Wsrep_server_service(Wsrep_server_state& server_state)
    : m_server_state(server_state)
  { }

  wsrep::storage_service* storage_service(wsrep::client_service&);

  wsrep::storage_service* storage_service(wsrep::high_priority_service&);

  void release_storage_service(wsrep::storage_service*);

  wsrep::high_priority_service*
  streaming_applier_service(wsrep::client_service&);

  wsrep::high_priority_service*
  streaming_applier_service(wsrep::high_priority_service&);

  void release_high_priority_service(wsrep::high_priority_service*);

  void background_rollback(wsrep::client_state&);

  void bootstrap();
  void log_message(enum wsrep::log::level, const char*);

  void log_dummy_write_set(wsrep::client_state&, const wsrep::ws_meta&)
  { throw wsrep::not_implemented_error(); }

  void log_view(wsrep::high_priority_service*, const wsrep::view&);

  void recover_streaming_appliers(wsrep::client_service&);
  void recover_streaming_appliers(wsrep::high_priority_service&);
  wsrep::view get_view(wsrep::client_service&, const wsrep::id& own_id);

  wsrep::gtid get_position(wsrep::client_service&);

  void log_state_change(enum wsrep::server_state::state,
                        enum wsrep::server_state::state);

  bool sst_before_init() const;

  std::string sst_request();
  int start_sst(const std::string&, const wsrep::gtid&, bool);

  int wait_committing_transactions(int);

  void debug_sync(const char*);
private:
  Wsrep_server_state& m_server_state;
};


#endif /* WSREP_SERVER_SERVICE */
