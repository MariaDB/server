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
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER
#include <my_global.h>
/* only for the INFORMATION_SCHEMA table: ST_SCHEMA_TABLE, TABLE, Field */
#include <sql_i_s.h>
#include <table.h>
#include <field.h>
#include "parser.h"
#include <mysql/plugin_encryption.h>
#include <mysql/psi/mysql_thread.h>
#include <string.h>

static char* filename;
static char* filekey;
static unsigned long fkm_encryption_algorithm;
static unsigned long digest;
static uint use_pbkdf2;

static const char *fkm_encryption_algorithm_names[]=
{
  "aes_cbc",
#ifdef HAVE_EncryptAes128Ctr
  "aes_ctr",
#endif
  0
};

static const char *digest_names[]=
{
  "sha1",
  "sha224",
  "sha256",
  "sha384",
  "sha512",
  0
};

static TYPELIB fkm_encryption_algorithm_typelib=CREATE_TYPELIB_FOR(fkm_encryption_algorithm_names);

static TYPELIB digest_typelib=CREATE_TYPELIB_FOR(digest_names);

static MYSQL_SYSVAR_STR(filename, filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path and name of the key file",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(filekey, filekey,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Key to encrypt / decrypt the keyfile",
  NULL, NULL, "");

#ifdef HAVE_EncryptAes128Ctr
#define recommendation  ", aes_ctr is the recommended one"
#else
#define recommendation  ""
#endif
static MYSQL_SYSVAR_ENUM(encryption_algorithm, fkm_encryption_algorithm,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Encryption algorithm to use" recommendation,
  NULL, NULL, 0, &fkm_encryption_algorithm_typelib);

static MYSQL_SYSVAR_ENUM(digest, digest,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Digest algorithm used for decrypting the key file. The value of the "
  "openssl enc -md option",
  NULL, NULL, 0, &digest_typelib);

static MYSQL_SYSVAR_UINT(use_pbkdf2, use_pbkdf2,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Number of PBKDF2 iteration used for decrypting the key file. Use 0 to "
  "disable PBKDF2, otherwise it's a value of the openssl enc -iter option",
  NULL, NULL, 0, 0, UINT_MAX, 0);

static struct st_mysql_sys_var* settings[] = {
  MYSQL_SYSVAR(filename),
  MYSQL_SYSVAR(filekey),
  MYSQL_SYSVAR(encryption_algorithm),
  MYSQL_SYSVAR(digest),
  MYSQL_SYSVAR(use_pbkdf2),
  NULL
};

static inline enum my_digest get_digest()
{
  switch (digest)
  {
  case 0: return MY_DIGEST_SHA1;
  case 1: return MY_DIGEST_SHA224;
  case 2: return MY_DIGEST_SHA256;
  case 3: return MY_DIGEST_SHA384;
  case 4: return MY_DIGEST_SHA512;
  default:
  {
    assert(0);
    return MY_DIGEST_SHA1;
  }
  }
}

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_LOCK_keys;
static PSI_rwlock_info all_rwlocks[]=
{ { &key_LOCK_keys, "LOCK_keys", PSI_FLAG_GLOBAL } };
#else
#define key_LOCK_keys 0
#endif

/*
  The loaded keys. Encryption threads look keys up under a read lock;
  a reload parses the new file without the lock and swaps it in under
  the write lock, so the old keys can be freed right away.
*/
class Key_store
{
  keymap *keys= nullptr;
  mysql_rwlock_t lock;
  /*
    load() ran and failed. The INFORMATION_SCHEMA plugin then refuses to
    start too, so that a failed INSTALL SONAME does not leave it installed
    on its own. When the encryption plugin is disabled load() never runs,
    and the table starts empty.
  */
  bool load_failed= false;

  /* @return the parsed key file, or nullptr if it could not be parsed */
  static keymap *parse(myf log_flags)
  {
    keymap *new_keys= new keymap;
    Parser parser(filename, filekey, get_digest(), use_pbkdf2, log_flags);
    if (parser.parse(new_keys))
    {
      wipe_and_delete(new_keys);
      return nullptr;
    }
    return new_keys;
  }

  static void wipe_and_delete(keymap *k)
  {
    for (keymap::iterator id= k->begin(); id != k->end(); ++id)
      for (key_versions::iterator v= id->second.begin();
           v != id->second.end(); ++v)
        secure_zero(&v->second, sizeof v->second);
    delete k;
  }

  /*
    A reload must not drop or change a key version that is already loaded:
    data, redo log or binlogs may still be encrypted with it.
    @return false and the offending id and version if it would
  */
  static bool contains(const keymap &loaded, const keymap &new_keys,
                       uint *bad_id, uint *bad_version)
  {
    for (keymap::const_iterator id= loaded.begin(); id != loaded.end(); ++id)
    {
      keymap::const_iterator new_id= new_keys.find(id->first);
      for (key_versions::const_iterator v= id->second.begin();
           v != id->second.end(); ++v)
      {
        key_versions::const_iterator new_v;
        if (new_id == new_keys.end() ||
            (new_v= new_id->second.find(v->first)) == new_id->second.end() ||
            new_v->second.length != v->second.length ||
            memcmp(new_v->second.key, v->second.key, v->second.length))
        {
          *bad_id= id->first;
          *bad_version= v->first;
          return false;
        }
      }
    }
    return true;
  }

public:
  /* encryption plugin init; @return whether the key file was loaded */
  bool load()
  {
    load_failed= false;
    keymap *new_keys= parse(ME_ERROR_LOG);
    if (!new_keys)
    {
      load_failed= true;
      return false;
    }
#ifdef HAVE_PSI_INTERFACE
    /* the performance schema allows at most 12 characters here */
    mysql_rwlock_register("file_keymgmt", all_rwlocks,
                          array_elements(all_rwlocks));
#endif
    mysql_rwlock_init(key_LOCK_keys, &lock);
    keys= new_keys;
    return true;
  }

  /* encryption plugin deinit */
  void unload()
  {
    if (keys)
    {
      mysql_rwlock_destroy(&lock);
      wipe_and_delete(keys);
      keys= nullptr;
    }
  }

  bool failed() const { return load_failed; }

  /*
    FLUSH FILE_KEY_MANAGEMENT_KEYS: re-read the key file without a restart.
    Details of a bad key file go to the error log only; the client gets
    one error. On any error the loaded keys stay in use.
  */
  int reload()
  {
    if (!keys)
    {
      my_error(ER_ENCRYPTION_KEY_RELOAD_FAILED, MYF(0),
               filename ? filename : "");
      return 1;
    }

    keymap *new_keys= parse(ME_ERROR_LOG_ONLY);
    if (!new_keys)
    {
      my_error(ER_ENCRYPTION_KEY_RELOAD_FAILED, MYF(0), filename);
      return 1;
    }

    uint bad_id, bad_version;
    mysql_rwlock_wrlock(&lock);
    bool ok= contains(*keys, *new_keys, &bad_id, &bad_version);
    if (ok)
      std::swap(keys, new_keys);
    mysql_rwlock_unlock(&lock);

    /* the rejected new keys, or the replaced old ones */
    wipe_and_delete(new_keys);
    if (!ok)
    {
      my_error(ER_ENCRYPTION_KEY_IN_USE, MYF(ME_ERROR_LOG),
               bad_id, bad_version);
      return 1;
    }
    return 0;
  }

  uint latest_version(uint key_id)
  {
    uint version= ENCRYPTION_KEY_VERSION_INVALID;
    mysql_rwlock_rdlock(&lock);
    keymap::const_iterator it= keys->find(key_id);
    if (it != keys->end())
      version= it->second.rbegin()->first;
    mysql_rwlock_unlock(&lock);
    return version;
  }

  uint get_key(uint key_id, uint key_version, uchar *dstbuf, uint *buflen)
  {
    uint rc= ENCRYPTION_KEY_VERSION_INVALID;
    mysql_rwlock_rdlock(&lock);
    keymap::const_iterator it= keys->find(key_id);
    if (it != keys->end())
    {
      key_versions::const_iterator v= it->second.find(key_version);
      if (v != it->second.end())
      {
        const keyentry &entry= v->second;
        if (*buflen < entry.length)
          rc= ENCRYPTION_KEY_BUFFER_TOO_SMALL;
        else
        {
          if (dstbuf)
            memcpy(dstbuf, entry.key, entry.length);
          rc= 0;
        }
        *buflen= entry.length;
      }
    }
    mysql_rwlock_unlock(&lock);
    return rc;
  }

  /* fills INFORMATION_SCHEMA.FILE_KEY_MANAGEMENT_KEYS: never key material */
  int fill(THD *thd, TABLE *table)
  {
    int rc= 0;
    if (!keys)
      return 0;
    mysql_rwlock_rdlock(&lock);
    for (keymap::const_iterator id= keys->begin();
         id != keys->end() && !rc; ++id)
      for (key_versions::const_iterator v= id->second.begin();
           v != id->second.end() && !rc; ++v)
      {
        table->field[0]->store(id->first, true);
        table->field[1]->store(v->first, true);
        table->field[2]->store(v->second.length * 8, true);
        bool latest= v->first == id->second.rbegin()->first;
        table->field[3]->store(latest ? "YES" : "NO", latest ? 3 : 2,
                               system_charset_info);
        rc= schema_table_store_record(thd, table);
      }
    mysql_rwlock_unlock(&lock);
    return rc;
  }
};

static Key_store key_store;

static int reload_keys()
{
  return key_store.reload();
}

namespace Show {

static ST_FIELD_INFO keys_fields_info[] =
{
  Column("KEY_ID",      ULong(), NOT_NULL),
  Column("KEY_VERSION", ULong(), NOT_NULL),
  Column("KEY_LENGTH",  ULong(), NOT_NULL),
  Column("IS_LATEST",   Yes_or_no(), NOT_NULL),
  CEnd()
};

} // namespace Show

static int keys_fill(THD *thd, TABLE_LIST *tables, COND *)
{
  return key_store.fill(thd, tables->table);
}

static int keys_is_init(void *p)
{
  if (key_store.failed())
    return 1;
  ST_SCHEMA_TABLE *is= (ST_SCHEMA_TABLE *) p;
  is->fields_info= Show::keys_fields_info;
  is->fill_table= keys_fill;
  is->reset_table= reload_keys;
  return 0;
}

static struct st_mysql_information_schema keys_is_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

static unsigned int get_latest_version(uint key_id)
{
  return key_store.latest_version(key_id);
}

static unsigned int get_key_from_key_file(unsigned int key_id,
       unsigned int key_version, unsigned char* dstbuf, unsigned *buflen)
{
  return key_store.get_key(key_id, key_version, dstbuf, buflen);
}

// let's simplify the condition below
#ifndef HAVE_EncryptAes128Gcm
#define MY_AES_GCM MY_AES_CTR
#ifndef HAVE_EncryptAes128Ctr
#define MY_AES_CTR MY_AES_CBC
#endif
#endif

static inline enum my_aes_mode mode(int flags)
{
  /*
    If encryption_algorithm is AES_CTR then
      if no-padding, use AES_CTR
      else use AES_GCM (like CTR but appends a "checksum" block)
    else
      use AES_CBC
  */
  if (fkm_encryption_algorithm)
    if (flags & ENCRYPTION_FLAG_NOPAD)
      return MY_AES_CTR;
    else
      return MY_AES_GCM;
  else
    return MY_AES_CBC;
}

static int ctx_init(void *ctx, const unsigned char* key, unsigned int klen,
                    const unsigned char* iv, unsigned int ivlen, int flags,
                    unsigned int key_id, unsigned int key_version)
{
  return my_aes_crypt_init(ctx, mode(flags), flags, key, klen, iv, ivlen);
}

static int ctx_update(void *ctx, const unsigned char *src, unsigned int slen,
  unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_update(ctx, src, slen, dst, dlen);
}


static int ctx_finish(void *ctx, unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_finish(ctx, dst, dlen);
}

static unsigned int get_length(unsigned int slen, unsigned int key_id,
                               unsigned int key_version)
{
  return my_aes_get_size(mode(0), slen);
}

static uint ctx_size(uint, uint)
{
  return my_aes_ctx_size(mode(0));
}

struct st_mariadb_encryption file_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_version,
  get_key_from_key_file,
  ctx_size,
  ctx_init,
  ctx_update,
  ctx_finish,
  get_length
};

static int file_key_management_plugin_init(void *p)
{
  return key_store.load() ? 0 : 1;
}

static int file_key_management_plugin_deinit(void *p)
{
  key_store.unload();
  return 0;
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
  file_key_management_plugin_deinit,
  0x0101 /* 1.1 */,
  NULL,	/* status variables */
  settings,
  "1.1",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &keys_is_descriptor,
  "FILE_KEY_MANAGEMENT_KEYS",
  "MariaDB",
  "Loaded key ids and versions; FLUSH FILE_KEY_MANAGEMENT_KEYS reloads them",
  PLUGIN_LICENSE_GPL,
  keys_is_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
