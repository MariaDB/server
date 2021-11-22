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

#include "wsrep_allowlist_service.h"

#include "my_global.h"
#include "wsrep_mysqld.h"
#include "wsrep_priv.h"

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
  std::string string_value(value.data());
  return (wsrep_schema->allowlist_check(key, string_value));
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

