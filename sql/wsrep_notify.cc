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

#include <mysqld.h>
#include "wsrep_priv.h"
#include "wsrep_utils.h"


static const char* _status_str(wsrep_member_status_t status)
{
  switch (status)
  {
  case WSREP_MEMBER_UNDEFINED: return "Undefined";
  case WSREP_MEMBER_JOINER:    return "Joiner";
  case WSREP_MEMBER_DONOR:     return "Donor";
  case WSREP_MEMBER_JOINED:    return "Joined";
  case WSREP_MEMBER_SYNCED:    return "Synced";
  default:                     return "Error(?)";
  }
}

void wsrep_notify_status (wsrep_member_status_t    status,
                          const wsrep_view_info_t* view)
{
  if (!wsrep_notify_cmd || 0 == strlen(wsrep_notify_cmd))
  {
    WSREP_INFO("wsrep_notify_cmd is not defined, skipping notification.");
    return;
  }

  const long  cmd_len = (1 << 16) - 1;
  char* cmd_ptr = (char*) my_malloc(cmd_len + 1, MYF(MY_WME));
  long  cmd_off = 0;

  if (!cmd_ptr)
    return; // the warning is in the log

  cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off, "%s",
                       wsrep_notify_cmd);

  if (status >= WSREP_MEMBER_UNDEFINED && status < WSREP_MEMBER_ERROR)
  {
    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off, " --status %s",
                         _status_str(status));
  }
  else
  {
    /* here we preserve provider error codes */
    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --status 'Error(%d)'", status);
  }

  if (0 != view)
  {
    char uuid_str[40];

    wsrep_uuid_print (&view->state_id.uuid, uuid_str, sizeof(uuid_str));
    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --uuid %s", uuid_str);

    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --primary %s", view->view >= 0 ? "yes" : "no");

    cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                         " --index %d", view->my_idx);

    if (view->memb_num)
    {
        cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off, " --members");

        for (int i = 0; i < view->memb_num; i++)
        {
            wsrep_uuid_print (&view->members[i].id, uuid_str, sizeof(uuid_str));
            cmd_off += snprintf (cmd_ptr + cmd_off, cmd_len - cmd_off,
                                 "%c%s/%s/%s", i > 0 ? ',' : ' ',
                                 uuid_str, view->members[i].name,
                                 view->members[i].incoming);
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
  int err = p.error();

  if (err)
  {
    WSREP_ERROR("Notification command failed: %d (%s): \"%s\"",
                err, strerror(err), cmd_ptr);
  }
  my_free(cmd_ptr);
}

