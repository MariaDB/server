/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <my_global.h>
#include "hlindex.h"
#include "fts.h"
#include "create_options.h"

ha_create_table_option fts_index_options[]=
{
  HA_IOPTION_END
};

static struct st_mysql_storage_engine fts_daemon=
{ MYSQL_DAEMON_INTERFACE_VERSION };

st_plugin_int *fts_plugin;

#define fts_deinit 0
#define fts_sys_vars 0

struct hlindexton fts_hton=
{
  {0, 0, 0,
  nullptr,                        /* close_connection */
  nullptr,                        /* savepoint_set */
  nullptr,
  nullptr,                        /*savepoint_rollback_can_release_mdl*/
  nullptr,                        /*savepoint_release*/
  nullptr, nullptr,
  nullptr,                        /* prepare */
  nullptr,                        /* recover */
  nullptr, nullptr,               /* commit/rollback_by_xid */
  nullptr, nullptr,               /* recover_rollback_by_xid/recovery_done */
  nullptr, nullptr, nullptr,      /* snapshot, commit/prepare_ordered */
  nullptr, nullptr},              /* checkpoint, versioned */
  fts_index_options,              /* options */
  nullptr,                        /* tabledef */
  nullptr                         /* XXX create */
};

static int fts_init(void *p)
{
  fts_plugin= (st_plugin_int *)p;
  fts_plugin->data= &fts_hton;
  if (setup_transaction_participant(fts_plugin))
    return 1;
  return resolve_sysvar_table_options(fts_index_options); // XXX move it out
}

maria_declare_plugin(fts)
{
  MYSQL_DAEMON_PLUGIN,
  &fts_daemon, "fts", "MariaDB plc",
  "A plugin for fts index algorithm",
  PLUGIN_LICENSE_GPL, fts_init, fts_deinit, 0x0100, NULL,
  fts_sys_vars, "1.0", MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
