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

#ifndef WSREP_CLIENT_STATE_H
#define WSREP_CLIENT_STATE_H

/* wsrep-lib */
#include "wsrep/client_state.hpp"
#include "my_global.h"

class THD;

class Wsrep_client_state : public wsrep::client_state
{
public:
  Wsrep_client_state(THD* thd,
                     wsrep::mutex& mutex,
                     wsrep::condition_variable& cond,
                     wsrep::server_state& server_state,
                     wsrep::client_service& client_service,
                     const wsrep::client_id& id)
    : wsrep::client_state(mutex,
                          cond,
                          server_state,
                          client_service,
                          id,
                          wsrep::client_state::m_local)
    , m_thd(thd)
  { }
  THD* thd() { return m_thd; }
private:
  THD* m_thd;
};

#endif /* WSREP_CLIENT_STATE_H */
