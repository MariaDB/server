/* Copyright 2010 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include <mysqld.h>
#include "wsrep_priv.h"
#include "wsrep_utils.h"
#include "wsrep_status.h"

void wsrep_notify_status(enum wsrep::server_state::state status,
                         const wsrep::view* view)
{
  Wsrep_status::report_state(status);

  if (!wsrep_notify_cmd || 0 == strlen(wsrep_notify_cmd))
  {
    WSREP_INFO("wsrep_notify_cmd is not defined, skipping notification.");
    return;
  }

  const long  cmd_len = (1 << 16) - 1;
  char* cmd_ptr = (char*) my_malloc(PSI_NOT_INSTRUMENTED, cmd_len + 1, MYF(MY_WME));
  long  cmd_off = 0;

  if (!cmd_ptr)
    return; // the warning is in the log

  cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off, "%s",
                       wsrep_notify_cmd);

  cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off, " --status %s",
                       to_c_string(status));

  if (view != NULL)
  {
    std::ostringstream uuid;
    uuid << view->state_id().id();
    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --uuid %s", uuid.str().c_str());

    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --primary %s", view->view_seqno().get() >= 0 ? "yes" : "no");

    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --index %zd", view->own_index());

    const std::vector<wsrep::view::member>& members(view->members());
    if (members.size())
    {
      cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off, " --members");

      for (unsigned int i= 0; i < members.size(); i++)
      {
        std::ostringstream id;
        id << members[i].id();
        cmd_off += snprintf(cmd_ptr + cmd_off, cmd_len - cmd_off,
                            "%c%s/%s/%s", i > 0 ? ',' : ' ',
                            id.str().c_str(),
                            members[i].name().c_str(),
                            members[i].incoming().c_str());
      }
    }
  }

  if (cmd_off == cmd_len)
  {
    WSREP_ERROR("Notification buffer too short (%ld). Aborting notification.",
               cmd_len);
    my_free(cmd_ptr);
    return;
  }

  wsp::process p(cmd_ptr, "r", NULL);

  p.wait();
  int err= p.error();

  if (err)
  {
    WSREP_ERROR("Notification command failed: %d (%s): \"%s\"",
                err, strerror(err), cmd_ptr);
  }
  my_free(cmd_ptr);
}

