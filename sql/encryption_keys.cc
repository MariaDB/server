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
static plugin_ref encryption_key_manager= 0;
static struct st_mariadb_encryption *handle;

unsigned int get_latest_encryption_key_version()
{
  if (encryption_key_manager)
    return handle->get_latest_key_version();

  return BAD_ENCRYPTION_KEY_VERSION;
}

unsigned int has_encryption_key(uint version)
{
  if (encryption_key_manager)
  {
    uint unused;
    return handle->get_key(version, NULL, &unused) != BAD_ENCRYPTION_KEY_VERSION;
  }

  return 0;
}

uint get_encryption_key(uint version, uchar* key, uint *size)
{
  if (encryption_key_manager)
    return handle->get_key(version, key, size);

  return BAD_ENCRYPTION_KEY_VERSION;
}

int encrypt_data(const uchar* source, uint source_length,
                 uchar* dest, uint* dest_length,
                 const uchar* key, uint key_length,
                 const uchar* iv, uint iv_length,
                 int no_padding, uint key_version)
{
  if (encryption_key_manager)
    return handle->encrypt(source, source_length,
                           dest, dest_length, key, key_length,
                           iv, iv_length, no_padding, key_version);
  return 1;
}


int decrypt_data(const uchar* source, uint source_length,
                 uchar* dest, uint* dest_length,
                 const uchar* key, uint key_length,
                 const uchar* iv, uint iv_length,
                 int no_padding, uint key_version)
{
  if (encryption_key_manager)
    return handle->decrypt(source, source_length,
                           dest, dest_length, key, key_length,
                           iv, iv_length, no_padding, key_version);
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
  handle= (struct st_mariadb_encryption*)
            plugin->plugin->info;

  /* default encryption algorithm */
  if (!handle->encrypt)
    handle->encrypt= (encrypt_decrypt_func)my_aes_encrypt_cbc;
  if (!handle->decrypt)
    handle->decrypt= (encrypt_decrypt_func)my_aes_decrypt_cbc;

  return 0;
}

int finalize_encryption_plugin(st_plugin_int *plugin)
{
  if (plugin->plugin->deinit && plugin->plugin->deinit(NULL))
  {
    DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                           plugin->name.str));
  }
  if (encryption_key_manager)
    plugin_unlock(NULL, encryption_key_manager);
  encryption_key_manager= 0;
  return 0;
}

