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

#warning TODO rename to follow single consistent style

/* there can be only one encryption plugin enabled */
static plugin_ref encryption_key_manager= 0;
struct encryption_keys_service_st encryption_keys_handler;

unsigned int has_key(uint version)
{
  uint unused;
  return get_encryption_key(version, NULL, &unused) != BAD_ENCRYPTION_KEY_VERSION;
}

uint no_key()
{
  return BAD_ENCRYPTION_KEY_VERSION;
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
  if (encryption_key_manager)
    return 1;

  if (plugin->plugin->init && plugin->plugin->init(plugin))
  {
    sql_print_error("Plugin '%s' init function returned error.",
                    plugin->name.str);
    return 1;
  }

  encryption_key_manager= plugin_lock(NULL, plugin_int_to_ref(plugin));
  st_mariadb_encryption *handle=
    (struct st_mariadb_encryption*) plugin->plugin->info;

  encryption_keys_handler.encrypt_data_func=
    handle->encrypt ? handle->encrypt
                    : (encrypt_decrypt_func)my_aes_encrypt_cbc;

  encryption_keys_handler.decrypt_data_func=
    handle->decrypt ? handle->decrypt
                    : (encrypt_decrypt_func)my_aes_decrypt_cbc;

  encryption_keys_handler.get_encryption_key_func=
    handle->get_key;

  encryption_keys_handler.get_latest_encryption_key_version_func=
    handle->get_latest_key_version; // must be the last

  return 0;
}

int finalize_encryption_plugin(st_plugin_int *plugin)
{
  encryption_keys_handler.encrypt_data_func= no_crypt;
  encryption_keys_handler.decrypt_data_func= no_crypt;
  encryption_keys_handler.has_encryption_key_func= has_key;
  encryption_keys_handler.get_encryption_key_func=
      (uint (*)(uint, uchar*, uint*))no_key;
  encryption_keys_handler.get_latest_encryption_key_version_func= no_key;

  if (plugin && plugin->plugin->deinit && plugin->plugin->deinit(NULL))
  {
    DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                           plugin->name.str));
  }
  if (encryption_key_manager)
    plugin_unlock(NULL, encryption_key_manager);
  encryption_key_manager= 0;
  return 0;
}

