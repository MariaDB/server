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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file

  Encryption Plugin API.

  This file defines the API for server plugins that manage encryption
  keys for MariaDB on-disk data encryption.
*/

#define MYSQL_PLUGIN_ENCRYPTION_INCLUDED

#include <mysql/plugin.h>

#define MariaDB_ENCRYPTION_INTERFACE_VERSION 0x0200

/**
  Encryption plugin descriptor
*/
struct st_mariadb_encryption
{
  int interface_version;                        /**< version plugin uses */

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

  encrypt_decrypt_func encrypt;
  encrypt_decrypt_func decrypt;
};
#endif

