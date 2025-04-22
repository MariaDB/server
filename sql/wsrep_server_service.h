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

  wsrep::storage_service* storage_service(wsrep::client_service&) override;

  wsrep::storage_service* storage_service(wsrep::high_priority_service&) override;

  void release_storage_service(wsrep::storage_service*) override;

  wsrep::high_priority_service*
  streaming_applier_service(wsrep::client_service&) override;

  wsrep::high_priority_service*
  streaming_applier_service(wsrep::high_priority_service&) override;

  void release_high_priority_service(wsrep::high_priority_service*) override;

  void background_rollback(wsrep::unique_lock<wsrep::mutex> &,
                           wsrep::client_state &) override;

  void bootstrap() override;
  void log_message(enum wsrep::log::level, const char*) override;

  void log_dummy_write_set(wsrep::client_state&, const wsrep::ws_meta&) override
  { throw wsrep::not_implemented_error(); }

  void log_view(wsrep::high_priority_service*, const wsrep::view&) override;

  void recover_streaming_appliers(wsrep::client_service&) override;
  void recover_streaming_appliers(wsrep::high_priority_service&) override;
  wsrep::view get_view(wsrep::client_service&, const wsrep::id& own_id) override;

  wsrep::gtid get_position(wsrep::client_service&) override;
  void set_position(wsrep::client_service&, const wsrep::gtid&) override;

  void log_state_change(enum wsrep::server_state::state,
                        enum wsrep::server_state::state) override;

  bool sst_before_init() const override;

  std::string sst_request() override;
  int start_sst(const std::string&, const wsrep::gtid&, bool) override;

  int wait_committing_transactions(int) override;

  void debug_sync(const char*) override;
private:
  Wsrep_server_state& m_server_state;
};

/**
   Helper method to create new streaming applier.

   @param orig_thd Original thd context to copy operation context from.
   @param ctx Context string for debug logging.
 */
class Wsrep_applier_service;
Wsrep_applier_service*
wsrep_create_streaming_applier(THD *orig_thd, const char *ctx);

/**
   Helper method to create new storage service.

   @param orig_thd Original thd context to copy operation context from.
   @param ctx Context string for debug logging.
*/
class Wsrep_storage_service;
Wsrep_storage_service*
wsrep_create_storage_service(THD *orig_thd, const char *ctx);

/**
   Suppress all error logging from wsrep/Galera library.
 */
void wsrep_suppress_error_logging();
#endif /* WSREP_SERVER_SERVICE */
