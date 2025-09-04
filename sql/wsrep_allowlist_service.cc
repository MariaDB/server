/* Copyright 2021-2022 Codership Oy <info@codership.com>

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

#include "wsrep_allowlist_service.h"

#include "my_global.h"
#include "wsrep_mysqld.h"
#include "wsrep_priv.h"
#include "wsrep_schema.h"

#include <algorithm>
#include <memory>
#include <vector>

class Wsrep_allowlist_service : public wsrep::allowlist_service
{
public:
    bool allowlist_cb(wsrep::allowlist_service::allowlist_key key,
                      const wsrep::const_buffer& value) WSREP_NOEXCEPT override;
};

bool Wsrep_allowlist_service::allowlist_cb (
  wsrep::allowlist_service::allowlist_key key,
  const wsrep::const_buffer& value)
  WSREP_NOEXCEPT
{
  // Allow all connections if user has not given list of
  // allowed addresses or stored them on mysql.wsrep_allowlist
  // table. Note that table is available after SEs are initialized.
  bool res=true; 
  std::string string_value(value.data());
  if (wsrep_schema)
  {
    res= wsrep_schema->allowlist_check(key, string_value);
  }
  // If wsrep_schema is not initialized check if user has given
  // list of addresses where connections are allowed
  else if (wsrep_allowlist && wsrep_allowlist[0] != '\0')
  {
    res= false; // Allow only given addresses
    std::vector<std::string> allowlist;
    wsrep_split_allowlist(allowlist);
    for(auto allowed : allowlist)
    {
      if (!string_value.compare(allowed))
      {
        res= true; // Address found allow connection
        break;
      }
    }
  }
  return res;
}

std::unique_ptr<wsrep::allowlist_service> entrypoint;

wsrep::allowlist_service* wsrep_allowlist_service_init()
{
  entrypoint = std::unique_ptr<wsrep::allowlist_service>(new Wsrep_allowlist_service);
  return entrypoint.get();
}

void wsrep_allowlist_service_deinit()
{
  entrypoint.reset();
}

