/* Copyright (c) 2002, 2012, eperi GmbH.

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


#include "parser.h"
#include <mysql_version.h>
#include <mysql/plugin_encryption_key_management.h>
#include <string.h>

static char* filename;
static char* filekey;

static MYSQL_SYSVAR_STR(filename, filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path and name of the key file.",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(filekey, filekey,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Key to encrypt / decrypt the keyfile.",
  NULL, NULL, "");

static struct st_mysql_sys_var* settings[] = {
  MYSQL_SYSVAR(filename),
  MYSQL_SYSVAR(filekey),
  NULL
};

Dynamic_array<keyentry> keys(static_cast<uint>(0));

static keyentry *get_key(unsigned int key_id)
{
  keyentry *a= keys.front(), *b= keys.back() + 1, *c;
  while (b - a > 1)
  {
    c= a + (b - a)/2;
    if (c->id == key_id)
      return c;
    else if (c->id < key_id)
      a= c;
    else
      b= c;
  }
  return a->id == key_id ? a : 0;
}

/**
   This method is using with the id 0 if exists. 
   This method is used by innobase/xtradb for the key
   rotation feature of encrypting log files.
*/

static unsigned int get_highest_key_used_in_key_file()
{
  return 0;
}

static unsigned int has_key_from_key_file(unsigned int key_id)
{
  keyentry* entry = get_key(key_id);

  return entry != NULL;
}

static unsigned int get_key_size_from_key_file(unsigned int key_id)
{
  keyentry* entry = get_key(key_id);

  return entry ? entry->length : CRYPT_KEY_UNKNOWN;
}

static int get_key_from_key_file(unsigned int key_id, unsigned char* dstbuf,
                                 unsigned buflen)
{
  keyentry* entry = get_key(key_id);

  if (entry != NULL)
  {
    if (buflen < entry->length)
      return CRYPT_BUFFER_TO_SMALL;

    memcpy(dstbuf, entry->key, entry->length);

    return CRYPT_KEY_OK;
  }
  else
    return CRYPT_KEY_UNKNOWN;
}

static int file_key_management_plugin_init(void *p)
{
  Parser parser(filename, filekey);
  return parser.parse(&keys);
}

struct st_mariadb_encryption_key_management file_key_management_plugin= {
  MariaDB_ENCRYPTION_KEY_MANAGEMENT_INTERFACE_VERSION,
  get_highest_key_used_in_key_file,
  has_key_from_key_file,
  get_key_size_from_key_file,
  get_key_from_key_file
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(file_key_management)
{
  MariaDB_ENCRYPTION_KEY_MANAGEMENT_PLUGIN,
  &file_key_management_plugin,
  "file_key_management",
  "Denis Endro eperi GmbH",
  "File-based key management plugin",
  PLUGIN_LICENSE_GPL,
  file_key_management_plugin_init,
  NULL,
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  settings,
  "1.0",
  MariaDB_PLUGIN_MATURITY_ALPHA
}
maria_declare_plugin_end;
