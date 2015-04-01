/* Copyright (C) 2015 MariaDB

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
#include <mysql/plugin_encryption.h>
#include "log.h"
#include "sql_plugin.h"
#include <my_crypt.h>

/* there can be only one encryption plugin enabled */
static plugin_ref encryption_manager= 0;
struct encryption_service_st encryption_handler;

unsigned int has_key(uint version)
{
  uint unused;
  return encryption_key_get(version, NULL, &unused) != ENCRYPTION_KEY_VERSION_INVALID;
}

uint no_key()
{
  return ENCRYPTION_KEY_VERSION_INVALID;
}

static int no_crypt(const uchar* source, uint source_length,
                    uchar* dest, uint* dest_length,
                    const uchar* key, uint key_length,
                    const uchar* iv, uint iv_length,
                    int no_padding, uint key_version)
{
  return 1;
}


int initialize_encryption_plugin(st_plugin_int *plugin)
{
  if (encryption_manager)
    return 1;

  if (plugin->plugin->init && plugin->plugin->init(plugin))
  {
    sql_print_error("Plugin '%s' init function returned error.",
                    plugin->name.str);
    return 1;
  }

  encryption_manager= plugin_lock(NULL, plugin_int_to_ref(plugin));
  st_mariadb_encryption *handle=
    (struct st_mariadb_encryption*) plugin->plugin->info;

  encryption_handler.encryption_encrypt_func=
    handle->encrypt ? handle->encrypt
                    : (encrypt_decrypt_func)my_aes_encrypt_cbc;

  encryption_handler.encryption_decrypt_func=
    handle->decrypt ? handle->decrypt
                    : (encrypt_decrypt_func)my_aes_decrypt_cbc;

  encryption_handler.encryption_key_get_func=
    handle->get_key;

  encryption_handler.encryption_key_get_latest_version_func=
    handle->get_latest_key_version; // must be the last

  return 0;
}

int finalize_encryption_plugin(st_plugin_int *plugin)
{
  encryption_handler.encryption_encrypt_func= no_crypt;
  encryption_handler.encryption_decrypt_func= no_crypt;
  encryption_handler.encryption_key_exists_func= has_key;
  encryption_handler.encryption_key_get_func=
      (uint (*)(uint, uchar*, uint*))no_key;
  encryption_handler.encryption_key_get_latest_version_func= no_key;

  if (plugin && plugin->plugin->deinit && plugin->plugin->deinit(NULL))
  {
    DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                           plugin->name.str));
  }
  if (encryption_manager)
    plugin_unlock(NULL, encryption_manager);
  encryption_manager= 0;
  return 0;
}

