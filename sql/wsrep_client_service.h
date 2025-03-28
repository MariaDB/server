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

/** @file wsrep_client_service.h

  This file provides declarations for client service implementation.
  See wsrep/client_service.hpp for interface documentation.
*/

#ifndef WSREP_CLIENT_SERVICE_H
#define WSREP_CLIENT_SERVICE_H

/* wsrep-lib */
#include "wsrep/client_service.hpp"
#include "wsrep/client_state.hpp"
#include "wsrep/exception.hpp" /* not_implemented_error, remove when finished */

class THD;
class Wsrep_client_state;
class Wsrep_high_priority_context;

class Wsrep_client_service : public wsrep::client_service
{
public:
  Wsrep_client_service(THD*, Wsrep_client_state&);

  bool interrupted(wsrep::unique_lock<wsrep::mutex>&) const override;
  void reset_globals() override;
  void store_globals() override;
  int prepare_data_for_replication() override;
  void cleanup_transaction() override;
  bool statement_allowed_for_streaming() const override;
  size_t bytes_generated() const override;
  int prepare_fragment_for_replication(wsrep::mutable_buffer&, size_t&) override;
  int remove_fragments() override;
  void emergency_shutdown() override
  { throw wsrep::not_implemented_error(); }
  void will_replay() override;
  void signal_replayed() override;
  enum wsrep::provider::status replay() override;
  enum wsrep::provider::status replay_unordered() override;
  void wait_for_replayers(wsrep::unique_lock<wsrep::mutex>&) override;
  enum wsrep::provider::status commit_by_xid() override;
  bool is_explicit_xa() override
  {
    return false;
  }
  bool is_prepared_xa() override
  {
    return false;
  }
  bool is_xa_rollback() override
  {
    return false;
  }
  void debug_sync(const char*) override;
  void debug_crash(const char*) override;
  int bf_rollback() override;
private:
  friend class Wsrep_server_service;
  THD* m_thd;
  Wsrep_client_state& m_client_state;
};


#endif /* WSREP_CLIENT_SERVICE_H */
