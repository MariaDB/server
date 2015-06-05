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

void init_io_cache_encryption();

/* there can be only one encryption plugin enabled */
static plugin_ref encryption_manager= 0;
struct encryption_service_st encryption_handler;

unsigned int has_key_id(uint id)
{
  return encryption_key_get_latest_version(id) != ENCRYPTION_KEY_VERSION_INVALID;
}

unsigned int has_key_version(uint id, uint version)
{
  uint unused;
  return encryption_key_get(id, version, NULL, &unused) != ENCRYPTION_KEY_VERSION_INVALID;
}

uint no_key(uint)
{
  return ENCRYPTION_KEY_VERSION_INVALID;
}

static int no_crypt(const uchar* source, uint source_length,
                    uchar* dest, uint* dest_length,
                    const uchar* key, uint key_length,
                    const uchar* iv, uint iv_length,
                    int no_padding, uint key_id, uint key_version)
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

  init_io_cache_encryption();

  return 0;
}

int finalize_encryption_plugin(st_plugin_int *plugin)
{
  encryption_handler.encryption_encrypt_func= no_crypt;
  encryption_handler.encryption_decrypt_func= no_crypt;
  encryption_handler.encryption_key_id_exists_func= has_key_id;
  encryption_handler.encryption_key_version_exists_func= has_key_version;
  encryption_handler.encryption_key_get_func=
      (uint (*)(uint, uint, uchar*, uint*))no_key;
  encryption_handler.encryption_key_get_latest_version_func= no_key;

  if (plugin && plugin->plugin->deinit && plugin->plugin->deinit(NULL))
  {
    DBUG_PRINT("warning", ("Plugin '%s' deinit function returned error.",
                           plugin->name.str));
  }
  if (encryption_manager)
    plugin_unlock(NULL, encryption_manager);
  encryption_manager= 0;
  init_io_cache_encryption();
  return 0;
}

/******************************************************************
  Encryption Scheme service
******************************************************************/
static uint scheme_get_key(st_encryption_scheme *scheme,
                           st_encryption_scheme_key *key)
{
  if (scheme->locker)
    scheme->locker(scheme, 0);

  // Check if we already have key
  for (uint i = 0; i < array_elements(scheme->key); i++)
  {
    if (scheme->key[i].version == 0) // no more keys
      break;

    if (scheme->key[i].version == key->version)
    {
      *key= scheme->key[i];
      if (scheme->locker)
        scheme->locker(scheme, 1);
      return 0;
    }
  }

  // Not found!
  scheme->keyserver_requests++;

  uchar global_key[MY_AES_MAX_KEY_LENGTH];
  uint  global_key_len= sizeof(global_key), key_len;

  uint rc = encryption_key_get(scheme->key_id, key->version,
                              global_key, & global_key_len);
  if (rc)
    goto ret;

  /* Now generate the local key by encrypting IV using the global key */
  rc = my_aes_encrypt_ecb(scheme->iv, sizeof(scheme->iv), key->key, &key_len,
                          global_key, global_key_len, NULL, 0, 1);

  DBUG_ASSERT(key_len == sizeof(key->key));

  if (rc)
    goto ret;

  // Rotate keys to make room for a new
  for (uint i = array_elements(scheme->key) - 1; i; i--)
    scheme->key[i] = scheme->key[i - 1];

  scheme->key[0]= *key;

ret:
  if (scheme->locker)
    scheme->locker(scheme, 1);
  return rc;
}

int do_crypt(const unsigned char* src, unsigned int slen,
             unsigned char* dst, unsigned int* dlen,
             struct st_encryption_scheme *scheme,
             unsigned int key_version, unsigned int i32_1,
             unsigned int i32_2, unsigned long long i64,
             encrypt_decrypt_func crypt)
{
  compile_time_assert(ENCRYPTION_SCHEME_KEY_INVALID ==
                      (int)ENCRYPTION_KEY_VERSION_INVALID);

  // Maybe temporal solution for MDEV-8173
  // Rationale: scheme->type is currently global/object
  // and when used here might not represent actual state
  // of smaller granularity objects e.g. InnoDB page state
  // as type is stored to tablespace (FIL) and could represent
  // state where key rotation is trying to reach
  //DBUG_ASSERT(scheme->type == 1);

  if (key_version == ENCRYPTION_KEY_VERSION_INVALID ||
      key_version == ENCRYPTION_KEY_NOT_ENCRYPTED)
    return ENCRYPTION_SCHEME_KEY_INVALID;

  st_encryption_scheme_key key;
  key.version= key_version;
  uint rc= scheme_get_key(scheme, &key);
  if (rc)
    return (int)rc;

  unsigned char iv[4 + 4 + 8];
  int4store(iv + 0, i32_1);
  int4store(iv + 4, i32_2);
  int8store(iv + 8, i64);

  return crypt(src, slen, dst, dlen, key.key, sizeof(key.key),
               iv, sizeof(iv), 1, scheme->key_id, key_version);
}

int encryption_scheme_encrypt(const unsigned char* src, unsigned int slen,
                              unsigned char* dst, unsigned int* dlen,
                              struct st_encryption_scheme *scheme,
                              unsigned int key_version, unsigned int i32_1,
                              unsigned int i32_2, unsigned long long i64)
{
  return do_crypt(src, slen, dst, dlen, scheme, key_version, i32_1,
                  i32_2, i64, encryption_handler.encryption_encrypt_func);
}


int encryption_scheme_decrypt(const unsigned char* src, unsigned int slen,
                              unsigned char* dst, unsigned int* dlen,
                              struct st_encryption_scheme *scheme,
                              unsigned int key_version, unsigned int i32_1,
                              unsigned int i32_2, unsigned long long i64)
{
  return do_crypt(src, slen, dst, dlen, scheme, key_version, i32_1,
                  i32_2, i64, encryption_handler.encryption_decrypt_func);
}
