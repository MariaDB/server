#ifndef MYSQL_PLUGIN_ENCRYPTION_KEY_MANAGEMENT_INCLUDED
/* Copyright (C) 2014 Sergei Golubchik and MariaDB

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

  Encryption key Management Plugin API.

  This file defines the API for server plugins that manage encryption
  keys for MariaDB on-disk data encryption.
*/

#define MYSQL_PLUGIN_ENCRYPTION_KEY_MANAGEMENT_INCLUDED

#include <mysql/plugin.h>

#define MariaDB_ENCRYPTION_KEY_MANAGEMENT_INTERFACE_VERSION 0x0100

#define BAD_ENCRYPTION_KEY_VERSION (~0U)

/**
  Encryption key management plugin descriptor
*/
struct st_mariadb_encryption_key_management
{
  int interface_version;                        /**< version plugin uses */

  /**
    function returning latest key version.

    @return a version or BAD_ENCRYPTION_KEY_VERSION to indicate an error.
  */
  unsigned int (*get_latest_key_version)();

  /** function returning if a key of the given version exists */
  unsigned int (*has_key_version)(unsigned int version);

  /** function returning the key size in bytes */
  unsigned int (*get_key_size)(unsigned int version);

  /**
    function returning a key for a key version

    the key is put in 'key' buffer, that has size of 'keybufsize' bytes.

    @return 0 on success, non-zero on failure
  */
  int (*get_key)(unsigned int version, unsigned char* key, unsigned int keybufsize);

  /**
    function returning an IV for a key version

    the IV is put in 'iv' buffer, that has size of 'ivbufsize' bytes.

    @return 0 on success, non-zero on failure
  */
  int (*get_iv)(unsigned int version, unsigned char* iv, unsigned int ivbufsize);
};
#endif

