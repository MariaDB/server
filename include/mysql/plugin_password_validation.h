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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef MYSQL_PLUGIN_PASSWORD_VALIDATION_INCLUDED
#define MYSQL_PLUGIN_PASSWORD_VALIDATION_INCLUDED

/**
  @file

  Password Validation Plugin API.

  This file defines the API for server password validation plugins.
*/


#include <mysql/plugin.h>

/**
   @defgroup password_validation_plugin_data Password Validation Plugin
   @ingroup plugin_types
   Password validation plugin API (@ref MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION)

   This file defines the API for server plugins that perform password validation.
   @{
*/

#ifdef __cplusplus
extern "C" {
#endif

#define MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION 0x0101

/**
  Password validation plugin descriptor
*/
struct st_mariadb_password_validation
{
  int interface_version;                        /**< version plugin uses */
  /**
    Function provided by the plugin which should perform password validation
    and return 0 if the password has passed the validation.
  */
  int (*validate_password)(const MYSQL_CONST_LEX_STRING *username,
                           const MYSQL_CONST_LEX_STRING *password,
                           const MYSQL_CONST_LEX_STRING *hostname);
};

#ifdef __cplusplus
}
#endif

/** @} */

#endif

