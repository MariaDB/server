/* Copyright (c) 2024, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Test plugin: two plugins with ASCII names and one with a non-ASCII name.
  Used by mysql-test/suite/plugins/t/plugin_name_ascii.test
  The non-ASCII plugin name "plügïn_non_ascii" is represented as raw UTF-8 literal.
*/

#include <mysql/plugin.h>

#if !defined(__attribute__) && (defined(__cplusplus) || !defined(__GNUC__)  || __GNUC__ == 2 && __GNUC_MINOR__ < 8)
#define __attribute__(A)
#endif

static struct st_mysql_daemon plugin1_descriptor = { MYSQL_DAEMON_INTERFACE_VERSION };
static struct st_mysql_daemon plugin2_descriptor = { MYSQL_DAEMON_INTERFACE_VERSION };
static struct st_mysql_daemon plugin3_descriptor = { MYSQL_DAEMON_INTERFACE_VERSION };

static int plugin1_init(void *p __attribute__((unused))) { return 0; }
static int plugin1_deinit(void *p __attribute__((unused))) { return 0; }
static int plugin2_init(void *p __attribute__((unused))) { return 0; }
static int plugin2_deinit(void *p __attribute__((unused))) { return 0; }
static int plugin3_init(void *p __attribute__((unused))) { return 0; }
static int plugin3_deinit(void *p __attribute__((unused))) { return 0; }

maria_declare_plugin(plugin_name_non_ascii)
{
    MYSQL_DAEMON_PLUGIN,
    &plugin1_descriptor,
    "plugin_one",
    "MariaDB",
    "Plugin with ASCII name - one",
    PLUGIN_LICENSE_GPL,
    plugin1_init,
    plugin1_deinit,
    0x0100,
    NULL,
    NULL,
    "1.0",
    MariaDB_PLUGIN_MATURITY_STABLE
},
{
    MYSQL_DAEMON_PLUGIN,
    &plugin2_descriptor,
    "plugin_two",
    "MariaDB",
    "Plugin with ASCII name - two",
    PLUGIN_LICENSE_GPL,
    plugin2_init,
    plugin2_deinit,
    0x0100,
    NULL,
    NULL,
    "1.0",
    MariaDB_PLUGIN_MATURITY_STABLE
},
{
    MYSQL_DAEMON_PLUGIN,
    &plugin3_descriptor,
    "plügïn_non_ascii",  /* "plügïn_non_ascii" in raw UTF-8 literal */
    "MariaDB",
    "Plugin with non-ASCII name",
    PLUGIN_LICENSE_GPL,
    plugin3_init,
    plugin3_deinit,
    0x0100,
    NULL,
    NULL,
    "1.0",
    MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
