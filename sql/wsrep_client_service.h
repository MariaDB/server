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

  This file provides declaratios for client service implementation.
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

  bool interrupted(wsrep::unique_lock<wsrep::mutex>&) const;
  void reset_globals();
  void store_globals();
  int prepare_data_for_replication();
  void cleanup_transaction();
  bool statement_allowed_for_streaming() const;
  size_t bytes_generated() const;
  int prepare_fragment_for_replication(wsrep::mutable_buffer&, size_t&);
  int remove_fragments();
  void emergency_shutdown()
  { throw wsrep::not_implemented_error(); }
  void will_replay();
  void signal_replayed();
  enum wsrep::provider::status replay();
  enum wsrep::provider::status replay_unordered();
  void wait_for_replayers(wsrep::unique_lock<wsrep::mutex>&);
  enum wsrep::provider::status commit_by_xid();
  bool is_explicit_xa()
  {
    return false;
  }
  bool is_xa_rollback()
  {
    return false;
  }
  void debug_sync(const char*);
  void debug_crash(const char*);
  int bf_rollback();
private:
  friend class Wsrep_server_service;
  THD* m_thd;
  Wsrep_client_state& m_client_state;
};


#endif /* WSREP_CLIENT_SERVICE_H */
