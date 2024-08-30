#ifndef MYSQL_PLUGIN_ENCRYPTION_INCLUDED
/* Copyright (C) 2014, 2015 Sergei Golubchik and MariaDB

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
  @file

  Encryption Plugin API.

  This file defines the API for server plugins that manage encryption
  keys for MariaDB on-disk data encryption.
*/

#define MYSQL_PLUGIN_ENCRYPTION_INCLUDED

#include <mysql/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MariaDB_ENCRYPTION_INTERFACE_VERSION 0x0300

/**
  Encryption plugin descriptor
*/
struct st_mariadb_encryption
{
  int interface_version;                        /**< version plugin uses */

  /*********** KEY MANAGEMENT ********************************************/

  /**
    function returning latest key version for a given key id

    @return a version or ENCRYPTION_KEY_VERSION_INVALID to indicate an error.
  */
  unsigned int (*get_latest_key_version)(unsigned int key_id);

  /**
    function returning a key for a key version

    @param version      the requested key version
    @param key          the key will be stored there. Can be NULL -
                        in which case no key will be returned
    @param key_length   in: key buffer size
                        out: the actual length of the key

    This method can be used to query the key length - the required
    buffer size - by passing key==NULL.

    If the buffer size is less than the key length the content of the
    key buffer is undefined (the plugin is free to partially fill it with
    the key data or leave it untouched).

    @return 0 on success, or
            ENCRYPTION_KEY_VERSION_INVALID, ENCRYPTION_KEY_BUFFER_TOO_SMALL
            or any other non-zero number for errors
  */
  unsigned int (*get_key)(unsigned int key_id, unsigned int version,
                          unsigned char *key, unsigned int *key_length);

  /*********** ENCRYPTION ************************************************/
  /*
    the caller uses encryption as follows:
      1. create the encryption context object of the crypt_ctx_size() bytes.
      2. initialize it with crypt_ctx_init().
      3. repeat crypt_ctx_update() until there are no more data to encrypt.
      4. write the remaining output bytes and destroy the context object
         with crypt_ctx_finish().
  */

  /**
    returns the size of the encryption context object in bytes
  */
  unsigned int (*crypt_ctx_size)(unsigned int key_id, unsigned int key_version);
  /**
    initializes the encryption context object.
  */
  int (*crypt_ctx_init)(void *ctx, const unsigned char* key, unsigned int klen,
                        const unsigned char* iv, unsigned int ivlen,
                        int flags, unsigned int key_id,
                        unsigned int key_version);
  /**
    processes (encrypts or decrypts) a chunk of data

    writes the output to the dst buffer. note that it might write
    more bytes that were in the input. or less. or none at all.

    dlen points to the starting lenght of the output buffer. Upon return, it
    should be set to the number of bytes written.
  */
  int (*crypt_ctx_update)(void *ctx, const unsigned char* src, unsigned int slen,
                          unsigned char* dst, unsigned int* dlen);
  /**
    writes the remaining output bytes and destroys the encryption context

    crypt_ctx_update might've cached part of the output in the context,
    this method will flush these data out.
  */
  int (*crypt_ctx_finish)(void *ctx, unsigned char* dst, unsigned int* dlen);
  /**
    returns the length of the encrypted data

    it returns the exact length, given only the source length.
    which means, this API only supports encryption algorithms where
    the length of the encrypted data only depends on the length of the
    input (a.k.a. compression is not supported).
  */
  unsigned int (*encrypted_length)(unsigned int slen, unsigned int key_id, unsigned int key_version);
};

#ifdef __cplusplus
}
#endif
#endif
