/* Copyright 2011 Codership Oy <http://www.codership.com>
   Copyright 2014 SkySQL Ab.

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

#include "mysqld.h"
#include "sys_vars_shared.h"
#include "wsrep.h"
#include "wsrep_sst.h"

extern char *my_bind_addr_str;

int wsrep_check_opts()
{
  if (wsrep_slave_threads > 1)
  {
    sys_var *autoinc_lock_mode=
      intern_find_sys_var(STRING_WITH_LEN("innodb_autoinc_lock_mode"));
    bool is_null;
    if (autoinc_lock_mode &&
        autoinc_lock_mode->val_int(&is_null, 0, OPT_GLOBAL, 0) != 2)
    {
      WSREP_ERROR("Parallel applying (wsrep_slave_threads > 1) requires"
                  " innodb_autoinc_lock_mode = 2.");
      return 1;
    }
  }

  if (locked_in_memory)
  {
    WSREP_ERROR("Memory locking is not supported (locked_in_memory=ON)");
    return 1;
  }

  if (!strcasecmp(wsrep_sst_method, "mysqldump"))
  {
    if (my_bind_addr_str &&
        (!strcasecmp(my_bind_addr_str, "127.0.0.1") ||
         !strcasecmp(my_bind_addr_str, "localhost")))
    {
      WSREP_WARN("wsrep_sst_method is set to 'mysqldump' yet "
                  "mysqld bind_address is set to '%s', which makes it "
                  "impossible to receive state transfer from another "
                  "node, since mysqld won't accept such connections. "
                  "If you wish to use mysqldump state transfer method, "
                  "set bind_address to allow mysql client connections "
                  "from other cluster members (e.g. 0.0.0.0).",
                  my_bind_addr_str);
    }
  }
  else
  {
    // non-mysqldump SST requires wsrep_cluster_address on startup
    if (!wsrep_cluster_address || !wsrep_cluster_address[0])
    {
      WSREP_ERROR ("%s SST method requires wsrep_cluster_address to be "
                   "configured on startup.", wsrep_sst_method);
      return 1;
    }
  }

  if (strcasecmp(wsrep_sst_receive_address, "AUTO"))
  {
    if (!strncasecmp(wsrep_sst_receive_address, STRING_WITH_LEN("127.0.0.1")) ||
        !strncasecmp(wsrep_sst_receive_address, STRING_WITH_LEN("localhost")))
    {
      WSREP_WARN("wsrep_sst_receive_address is set to '%s' which "
                 "makes it impossible for another host to reach this "
                 "one. Please set it to the address which this node "
                 "can be connected at by other cluster members.",
                 wsrep_sst_receive_address);
    }
  }

  if (strcasecmp(wsrep_provider, "NONE"))
  {
    if (global_system_variables.binlog_format != BINLOG_FORMAT_ROW)
    {
      WSREP_ERROR("Only binlog_format = 'ROW' is currently supported. "
                  "Configured value: '%s'. Please adjust your "
                  "configuration.",
                  binlog_format_names[global_system_variables.binlog_format]);

      return 1;
    }
  }
  return 0;
}

