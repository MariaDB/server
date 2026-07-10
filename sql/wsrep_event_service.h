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

#ifndef WSREP_EVENT_SERVICE_H
#define WSREP_EVENT_SERVICE_H

/* wsrep-lib */
#include "wsrep/event_service.hpp"

/* implementation */
#include "wsrep_status.h"

class Wsrep_event_service : public wsrep::event_service
{
public:

  void process_event(const std::string& name, const std::string& value)
    override
  {
    if (name == "progress")
    {
      Wsrep_status::report_progress(value);
    }
    else if (name == "event")
    {
      Wsrep_status::report_event(value);
    }
    else
    {
      // not interested in the event
    }
  }

  static wsrep::event_service* instance();
};

#endif /* WSREP_EVENT_SERVICE_H */
