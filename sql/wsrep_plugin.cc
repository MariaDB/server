/* Copyright 2016 Codership Oy <http://www.codership.com>

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

#include "wsrep_trans_observer.h"
#include "wsrep_mysqld.h"

#include <mysql/plugin.h>

static int wsrep_plugin_init(void *p)
{
  WSREP_DEBUG("wsrep_plugin_init()");
  return 0;
}

static int wsrep_plugin_deinit(void *p)
{
  WSREP_DEBUG("wsrep_plugin_deinit()");
  return 0;
}

struct Mysql_replication wsrep_plugin= {
  MYSQL_REPLICATION_INTERFACE_VERSION
};

maria_declare_plugin(wsrep)
{
  MYSQL_REPLICATION_PLUGIN,
  &wsrep_plugin,
  "wsrep",
  "Codership Oy",
  "Wsrep replication plugin",
  PLUGIN_LICENSE_GPL,
  wsrep_plugin_init,
  wsrep_plugin_deinit,
  0x0100,
  NULL, /* Status variables */
  NULL, /* System variables */
  "1.0", /* Version (string) */
  MariaDB_PLUGIN_MATURITY_STABLE     /* Maturity */
}
maria_declare_plugin_end;
