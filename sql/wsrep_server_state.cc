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

#include "my_global.h"
#include "wsrep_api.h"
#include "wsrep_server_state.h"
#include "wsrep_allowlist_service.h"
#include "wsrep_binlog.h" /* init/deinit group commit */

#include "my_stacktrace.h" /* my_safe_printf_stderr() */

mysql_mutex_t LOCK_wsrep_server_state;
mysql_cond_t  COND_wsrep_server_state;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_wsrep_server_state;
PSI_cond_key  key_COND_wsrep_server_state;
#endif

wsrep::provider::services Wsrep_server_state::m_provider_services;

Wsrep_server_state::Wsrep_server_state(const std::string& name,
                                       const std::string& incoming_address,
                                       const std::string& address,
                                       const std::string& working_dir,
                                       const wsrep::gtid& initial_position,
                                       int max_protocol_version)
  : wsrep::server_state(m_mutex,
                        m_cond,
                        m_service,
                        NULL,
                        name,
                        incoming_address,
                        address,
                        working_dir,
                        initial_position,
                        max_protocol_version,
                        wsrep::server_state::rm_sync)
  , m_mutex(&LOCK_wsrep_server_state)
  , m_cond(&COND_wsrep_server_state)
  , m_service(*this)
{ }

Wsrep_server_state::~Wsrep_server_state() = default;

void Wsrep_server_state::init_once(const std::string& name,
                                   const std::string& incoming_address,
                                   const std::string& address,
                                   const std::string& working_dir,
                                   const wsrep::gtid& initial_position,
                                   int max_protocol_version)
{
  if (m_instance == 0)
  {
    mysql_mutex_init(key_LOCK_wsrep_server_state, &LOCK_wsrep_server_state,
                     MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_COND_wsrep_server_state, &COND_wsrep_server_state, 0);
    m_instance = new Wsrep_server_state(name,
                                        incoming_address,
                                        address,
                                        working_dir,
                                        initial_position,
                                        max_protocol_version);
  }
}

void Wsrep_server_state::destroy()
{
  if (m_instance)
  {
    delete m_instance;
    m_instance= 0;
    mysql_mutex_destroy(&LOCK_wsrep_server_state);
    mysql_cond_destroy(&COND_wsrep_server_state);
  }
}

void Wsrep_server_state::init_provider_services()
{
  m_provider_services.allowlist_service= wsrep_allowlist_service_init();
}

void Wsrep_server_state::deinit_provider_services()
{
  if (m_provider_services.allowlist_service)
    wsrep_allowlist_service_deinit();
  m_provider_services= wsrep::provider::services();
}

void Wsrep_server_state::handle_fatal_signal()
{
  if (m_instance && m_instance->is_provider_loaded())
  {
    /* Galera background threads are still running and the logging may be
       relatively verbose in case of networking error. Silence all wsrep
       logging before shutting down networking to avoid garbling signal
       handler output. */
    my_safe_printf_stderr("WSREP: Suppressing further logging\n");
    wsrep_suppress_error_logging();

    /* Shut down all communication with other nodes to fail silently. */
    my_safe_printf_stderr("WSREP: Shutting down network communications\n");
    if (m_instance->provider().set_node_isolation(
          wsrep::provider::node_isolation::isolated)) {
      my_safe_printf_stderr("WSREP: Galera library does not support node isolation\n");
    }
    my_safe_printf_stderr("\n");
  }
}
