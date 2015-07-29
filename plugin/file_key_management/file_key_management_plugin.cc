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
#include <mysql/plugin_encryption.h>
#include <string.h>

static char* filename;
static char* filekey;
static unsigned long encryption_algorithm;

static const char *encryption_algorithm_names[]=
{
  "aes_cbc",
#ifdef HAVE_EncryptAes128Ctr
  "aes_ctr",
#endif
  0
};

static TYPELIB encryption_algorithm_typelib=
{
  array_elements(encryption_algorithm_names)-1,"",
  encryption_algorithm_names, NULL
};


static MYSQL_SYSVAR_STR(filename, filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path and name of the key file.",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(filekey, filekey,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Key to encrypt / decrypt the keyfile.",
  NULL, NULL, "");

#ifdef HAVE_EncryptAes128Ctr
#define recommendation  ", aes_ctr is the recommended one"
#else
#define recommendation  ""
#endif
static MYSQL_SYSVAR_ENUM(encryption_algorithm, encryption_algorithm,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Encryption algorithm to use" recommendation ".",
  NULL, NULL, 0, &encryption_algorithm_typelib);

static struct st_mysql_sys_var* settings[] = {
  MYSQL_SYSVAR(filename),
  MYSQL_SYSVAR(filekey),
  MYSQL_SYSVAR(encryption_algorithm),
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

/* the version is always the same, no automatic key rotation */
static unsigned int get_latest_version(uint key_id)
{
  return  get_key(key_id) ? 1 : ENCRYPTION_KEY_VERSION_INVALID;
}

static unsigned int get_key_from_key_file(unsigned int key_id,
       unsigned int key_version, unsigned char* dstbuf, unsigned *buflen)
{
  if (key_version != 1)
    return ENCRYPTION_KEY_VERSION_INVALID;

  keyentry* entry = get_key(key_id);

  if (entry == NULL)
    return ENCRYPTION_KEY_VERSION_INVALID;

  if (*buflen < entry->length)
  {
    *buflen= entry->length;
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }

  *buflen= entry->length;
  if (dstbuf)
    memcpy(dstbuf, entry->key, entry->length);

  return 0;
}

struct st_mariadb_encryption file_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_version,
  get_key_from_key_file,
  0,0
};

#ifdef HAVE_EncryptAes128Gcm
/*
  use AES-CTR when cyphertext length must be the same as plaintext length,
  and AES-GCM when cyphertext can be longer than plaintext.
*/
static int ctr_gcm_encrypt(const unsigned char* src, unsigned int slen,
            unsigned char* dst, unsigned int* dlen,
            const unsigned char* key, unsigned int klen,
            const unsigned char* iv, unsigned int ivlen,
            int no_padding, unsigned int keyid, unsigned int key_version)
{
  return (no_padding ? my_aes_encrypt_ctr : my_aes_encrypt_gcm)
            (src, slen, dst, dlen, key, klen, iv, ivlen);
}

static int ctr_gcm_decrypt(const unsigned char* src, unsigned int slen,
            unsigned char* dst, unsigned int* dlen,
            const unsigned char* key, unsigned int klen,
            const unsigned char* iv, unsigned int ivlen,
            int no_padding, unsigned int keyid, unsigned int key_version)
{
  return (no_padding ? my_aes_decrypt_ctr : my_aes_decrypt_gcm)
            (src, slen, dst, dlen, key, klen, iv, ivlen);
}
#endif

static int file_key_management_plugin_init(void *p)
{
  Parser parser(filename, filekey);
  switch (encryption_algorithm) {
  case 0: // AES_CBC
    file_key_management_plugin.encrypt=
      (encrypt_decrypt_func)my_aes_encrypt_cbc;
    file_key_management_plugin.decrypt=
      (encrypt_decrypt_func)my_aes_decrypt_cbc;
    break;
#ifdef HAVE_EncryptAes128Ctr
  case 1: // AES_CTR
#ifdef HAVE_EncryptAes128Gcm
    file_key_management_plugin.encrypt= ctr_gcm_encrypt;
    file_key_management_plugin.decrypt= ctr_gcm_decrypt;
#else
    file_key_management_plugin.encrypt=
      (encrypt_decrypt_func)my_aes_encrypt_ctr;
    file_key_management_plugin.decrypt=
      (encrypt_decrypt_func)my_aes_decrypt_ctr;
#endif
    break;
#endif
  default:
    return 1; // cannot happen
  }
  return parser.parse(&keys);
}

/*
  Plugin library descriptor
*/
maria_declare_plugin(file_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
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
