/*
  Copyright (c) 2015 MariaDB Corporation

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

/**
  Debug key management plugin.
  It's used to debug the encryption code with a fixed keys that change
  only on user request.

  THIS IS AN EXAMPLE ONLY! ENCRYPTION KEYS ARE HARD-CODED AND *NOT* SECRET!
  DO NOT USE THIS PLUGIN IN PRODUCTION! EVER!
*/

#include <my_global.h>
#include <mysql/plugin_encryption_key_management.h>
#include <string.h>
#include <myisampack.h>

static uint key_version;

static MYSQL_SYSVAR_UINT(version, key_version, PLUGIN_VAR_RQCMDARG,
  "Latest key version", NULL, NULL, 1, 0, UINT_MAX, 1);

static struct st_mysql_sys_var* sysvars[] = {
  MYSQL_SYSVAR(version),
  NULL
};

static unsigned int get_latest_key_version()
{
  return key_version;
}

static int get_key(unsigned int version, unsigned char* dstbuf, unsigned buflen)
{
  if (buflen < 4)
    return 1;
  memset(dstbuf, 0, buflen);
  mi_int4store(dstbuf, version);
  return 0;
}

static unsigned int has_key(unsigned int ver)
{
  return 1;
}

static unsigned int get_key_size(unsigned int ver)
{
  return 16;
}

static int get_iv(unsigned int ver, unsigned char* dstbuf, unsigned buflen)
{
  return 0; // to be removed
}

struct st_mariadb_encryption_key_management debug_key_management_plugin= {
  MariaDB_ENCRYPTION_KEY_MANAGEMENT_INTERFACE_VERSION,
  get_latest_key_version,
  has_key,
  get_key_size,
  get_key,
  get_iv
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(debug_key_management_plugin)
{
  MariaDB_ENCRYPTION_KEY_MANAGEMENT_PLUGIN,
  &debug_key_management_plugin,
  "debug_key_management_plugin",
  "Sergei Golubchik",
  "Debug key management plugin",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  sysvars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
