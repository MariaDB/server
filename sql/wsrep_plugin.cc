/* Copyright 2016-2019 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "my_global.h"
#include "wsrep_mysqld.h"

#include <mysql/plugin.h>

static int wsrep_plugin_init(void *p)
{
  WSREP_DEBUG("wsrep_plugin_init()");
  int ret= 0;
  wsrep_enable_encryption();
  if (wsrep_startup_state == WSREP_STARTUP_STATE_INIT_BEFORE_SE)
  { 
    if (wsrep_init_startup(true))
      return wsrep_startup_state == WSREP_STARTUP_STATE_MUST_ABORT;
    
    /* After SST has completed we could receive binlog files so reopening 
       binlog index */
    if (opt_bin_log)
    { 
      mysql_mutex_lock(mysql_bin_log.get_log_lock());
      mysql_bin_log.close(LOG_CLOSE_INDEX | LOG_CLOSE_TO_BE_OPENED);
      if (mysql_bin_log.open_index_file(opt_binlog_index_name,
                                       opt_bin_logname, TRUE))
      {
        WSREP_WARN("Failed to reopen binlog index file.");
        ret= 1;
      }
      mysql_mutex_unlock(mysql_bin_log.get_log_lock());
    }
  }
  return ret;
}

static int wsrep_plugin_deinit(void *p)
{
  WSREP_DEBUG("wsrep_plugin_deinit()");
  return 0;
}

struct st_mysql_storage_engine wsrep_plugin= {
  MYSQL_HANDLERTON_INTERFACE_VERSION
};

maria_declare_plugin(wsrep)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &wsrep_plugin,
  "wsrep",
  "Codership Oy",
  "Wsrep replication plugin",
  PLUGIN_LICENSE_GPL,
  wsrep_plugin_init,
  wsrep_plugin_deinit,
  0x0100,
  NULL,                           /* Status variables */
  NULL,                           /* System variables */
  "1.0",                          /* Version (string) */
  MariaDB_PLUGIN_MATURITY_STABLE  /* Maturity */
}
maria_declare_plugin_end;
