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

#ifndef WSREP_STORAGE_SERVICE_H
#define WSREP_STORAGE_SERVICE_H

#include "wsrep/storage_service.hpp"
#include "wsrep/client_state.hpp"

class THD;
class Wsrep_server_service;
class Wsrep_storage_service :
  public wsrep::storage_service,
  public wsrep::high_priority_context
{
public:
  Wsrep_storage_service(THD*);
  ~Wsrep_storage_service();
  int start_transaction(const wsrep::ws_handle&);
  void adopt_transaction(const wsrep::transaction&);
  int append_fragment(const wsrep::id&,
                      wsrep::transaction_id,
                      int flags,
                      const wsrep::const_buffer&,
                      const wsrep::xid&);
  int update_fragment_meta(const wsrep::ws_meta&);
  int remove_fragments();
  int commit(const wsrep::ws_handle&, const wsrep::ws_meta&);
  int rollback(const wsrep::ws_handle&, const wsrep::ws_meta&);
  void store_globals();
  void reset_globals();
private:
  friend class Wsrep_server_service;
  THD* m_thd;
};

#endif /* WSREP_STORAGE_SERVICE_H */
