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


#include <my_global.h>
#include <mysql_version.h>
#include <mysql/plugin_encryption_key_management.h>
#include <my_aes.h>
#include "sql_class.h"
#include "KeySingleton.h"
#include "EncKeys.h"

/* Encryption for tables and columns */
static char* filename = NULL;
static char* filekey = NULL;

static MYSQL_SYSVAR_STR(filename, filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path and name of the key file.",
  NULL, NULL, NULL);

static MYSQL_SYSVAR_STR(filekey, filekey,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Key to encrypt / decrypt the keyfile.",
  NULL, NULL, NULL);

static struct st_mysql_sys_var* settings[] = {
  MYSQL_SYSVAR(filename),
  MYSQL_SYSVAR(filekey),
  NULL
};



/**
   This method is using with the id 0 if exists. 
   This method is used by innobase/xtradb for the key
   rotation feature of encrypting log files.
*/

static unsigned int get_highest_key_used_in_key_file()
{
  if (KeySingleton::getInstance().hasKey(0))
  {
      return 0;
  }
  else
    return CRYPT_KEY_UNKNOWN;
}

static unsigned int has_key_from_key_file(unsigned int keyID)
{
  keyentry* entry = KeySingleton::getInstance().getKeys(keyID);

  return entry != NULL;
}

static unsigned int get_key_size_from_key_file(unsigned int keyID)
{
  keyentry* entry = KeySingleton::getInstance().getKeys(keyID);

  if (entry != NULL)
  {
    char* keyString = entry->key;
    size_t key_len = strlen(keyString)/2;

    return key_len;
  }
  else
  {
    return CRYPT_KEY_UNKNOWN;
  }
}

static int get_key_from_key_file(unsigned int keyID, unsigned char* dstbuf,
                                 unsigned buflen)
{
  keyentry* entry = KeySingleton::getInstance().getKeys((int)keyID);

  if (entry != NULL)
  {
    char* keyString = entry->key;
    size_t key_len = strlen(keyString)/2;

    if (buflen < key_len)
    {
      return CRYPT_BUFFER_TO_SMALL;
    }

    my_aes_hex2uint(keyString, (unsigned char*)dstbuf, key_len);

    return CRYPT_KEY_OK;
  }
  else
  {
    return CRYPT_KEY_UNKNOWN;
  }
}

static int get_iv_from_key_file(unsigned int keyID, unsigned char* dstbuf,
                                unsigned buflen)
{
  keyentry* entry = KeySingleton::getInstance().getKeys((int)keyID);

  if (entry != NULL)
  {
    char* ivString = entry->iv;
    size_t iv_len = strlen(ivString)/2;

    if (buflen < iv_len)
    {
      return CRYPT_BUFFER_TO_SMALL;
    }

    my_aes_hex2uint(ivString, (unsigned char*)dstbuf, iv_len);

    return CRYPT_KEY_OK;
  }
  else
  {
    return CRYPT_KEY_UNKNOWN;
  }
}


static int file_key_management_plugin_init(void *p)
{
  /* init */
  
  if (current_aes_dynamic_method == MY_AES_ALGORITHM_NONE)
  {
    sql_print_error("No encryption method choosen with --encryption-algorithm. "
                    "file_key_management_plugin disabled");
    return 1;
  }

  if (filename == NULL || strcmp("", filename) == 0)
  {
    sql_print_error("Parameter file_key_management_plugin_filename is required");

    return 1;
  }

  KeySingleton::getInstance(filename, filekey);

  return 0;
}

static int file_key_management_plugin_deinit(void *p)
{
  KeySingleton::getInstance().~KeySingleton();

  return 0;
}

struct st_mariadb_encryption_key_management file_key_management_plugin= {
  MariaDB_ENCRYPTION_KEY_MANAGEMENT_INTERFACE_VERSION,
  get_highest_key_used_in_key_file,
  has_key_from_key_file,
  get_key_size_from_key_file,
  get_key_from_key_file,
  get_iv_from_key_file
};

/*
  Plugin library descriptor
*/
maria_declare_plugin(file_key_management_plugin)
{
  MariaDB_ENCRYPTION_KEY_MANAGEMENT_PLUGIN,
  &file_key_management_plugin,
  "file_key_management_plugin",
  "Denis Endro eperi GmbH",
  "File key management plugin",
  PLUGIN_LICENSE_GPL,
  file_key_management_plugin_init,   /* Plugin Init */
  file_key_management_plugin_deinit, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  settings,
  "1.0",
  MariaDB_PLUGIN_MATURITY_UNKNOWN
}
maria_declare_plugin_end;
