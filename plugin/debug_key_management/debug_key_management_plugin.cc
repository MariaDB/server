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
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  Debug key management plugin.
  It's used to debug the encryption code with a fixed keys that change
  only on user request.

  It does not support different key ids, the only valid key id is 1.

  THIS IS AN EXAMPLE ONLY! ENCRYPTION KEYS ARE HARD-CODED AND *NOT* SECRET!
  DO NOT USE THIS PLUGIN IN PRODUCTION! EVER!
*/

#include <my_global.h>
#include <mysql/plugin_encryption.h>
#include <string.h>
#include <myisampack.h>

#define KEY_SIZE 16

static uint key_version;

static MYSQL_SYSVAR_UINT(version, key_version, PLUGIN_VAR_RQCMDARG,
  "Latest key version", NULL, NULL, 1, 0, UINT_MAX, 1);

static struct st_mysql_sys_var* sysvars[] = {
  MYSQL_SYSVAR(version),
  NULL
};

static unsigned int get_latest_key_version(unsigned int keyid)
{
  if (keyid != 1)
    return ENCRYPTION_KEY_VERSION_INVALID;

  return key_version;
}

static unsigned int get_key(unsigned int keyid, unsigned int version,
                            unsigned char* dstbuf, unsigned *buflen)
{
  if (keyid != 1)
    return ENCRYPTION_KEY_VERSION_INVALID;

  if (*buflen < KEY_SIZE)
  {
    *buflen= KEY_SIZE;
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }
  *buflen= KEY_SIZE;
  if (!dstbuf)
    return 0;

  memset(dstbuf, 0, KEY_SIZE);
  mi_int4store(dstbuf, version);
  return 0;
}

struct st_mariadb_encryption debug_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_key_version,
  get_key,
  // use default encrypt/decrypt functions
  0, 0, 0, 0, 0
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(debug_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
  &debug_key_management_plugin,
  "debug_key_management",
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
