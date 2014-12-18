/* Copyright (c) 2002, 2012, Oracle and/or its affiliates. All rights reserved.

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


#include <my_global.h>
#include <mysql_version.h>
#include <my_aes.h>

static int file_key_management_plugin_init(void *p)
{
  /* init */
  printf("TO_BE_REMOVED: Loading file_key_management_plugin_init\n");

  /* Setting to MY_AES_METHOD_CBC for page encryption */
  my_aes_init_dynamic_encrypt(MY_AES_ALGORITHM_CBC);

  return 0;
}

static int file_key_management_plugin_deinit(void *p)
{
  /**
   * don't uninstall...
   */
  return 0;
}

struct st_mysql_daemon file_key_management_plugin= {
  MYSQL_DAEMON_INTERFACE_VERSION
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(file_key_management_plugin)
{
  MYSQL_DAEMON_PLUGIN,
  &file_key_management_plugin,
  "file_key_management_plugin",
  "Denis Endro eperi GmbH",
  "File key management plugin",
  PLUGIN_LICENSE_GPL,
  file_key_management_plugin_init,   /* Plugin Init */
  file_key_management_plugin_deinit, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  NULL,	/* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_UNKNOWN
}
maria_declare_plugin_end;
