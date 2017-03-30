/* Copyright (c) 2016, MariaDB corporation. All rights
   reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <mysql_version.h>
#include <mysqld.h>
#include "sql_plugin.h"                         // st_plugin_int
#include "sql_class.h"

/*
  Disable __attribute__() on non-gcc compilers.
*/
#if !defined(__attribute__) && !defined(__GNUC__)
#define __attribute__(A)
#endif

static int forced_versioning_init(void *p __attribute__ ((unused)))
{

  DBUG_ENTER("forced_versioning_init");
  mysql_mutex_lock(&LOCK_global_system_variables);
  global_system_variables.vers_force= true;
  global_system_variables.vers_hide= VERS_HIDE_FULL;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  DBUG_RETURN(0);
}

static int forced_versioning_deinit(void *p __attribute__ ((unused)))
{
  DBUG_ENTER("forced_versioning_deinit");
  mysql_mutex_lock(&LOCK_global_system_variables);
  global_system_variables.vers_force= false;
  global_system_variables.vers_hide= VERS_HIDE_AUTO;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  DBUG_RETURN(0);
}


struct st_mysql_daemon forced_versioning_plugin=
{ MYSQL_DAEMON_INTERFACE_VERSION  };

/*
  Plugin library descriptor
*/

maria_declare_plugin(forced_versioning)
{
  MYSQL_DAEMON_PLUGIN,
  &forced_versioning_plugin,
  "forced_versioning",
  "Natsys Lab",
  "Enable System Vesioning for all newly created tables",
  PLUGIN_LICENSE_GPL,
  forced_versioning_init, /* Plugin Init */
  forced_versioning_deinit, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  "1.0",                      /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL /* maturity */
}
maria_declare_plugin_end;
