#ifndef MYSQL_PLUGIN_AUTH_INCLUDED
/* Copyright (C) 2010 Sergei Golubchik and Monty Program Ab
   Copyright (c) 2010, Oracle and/or its affiliates.

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

  Authentication Plugin API.

  This file defines the API for server authentication plugins.
*/

#define MYSQL_PLUGIN_AUTH_INCLUDED

#include <mysql/plugin.h>

#define MYSQL_AUTHENTICATION_INTERFACE_VERSION 0x0202

#include <mysql/plugin_auth_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* defines for MYSQL_SERVER_AUTH_INFO.password_used */

#define PASSWORD_USED_NO         0
#define PASSWORD_USED_YES        1
#define PASSWORD_USED_NO_MENTION 2


/**
  Provides server plugin access to authentication information
*/
typedef struct st_mysql_server_auth_info
{
  /**
    User name as sent by the client and shown in USER().
    NULL if the client packet with the user name was not received yet.
  */
  const char *user_name;

  /**
    Length of user_name
  */
  unsigned int user_name_length;

  /**
    A corresponding column value from the mysql.user table for the
    matching account name or the preprocessed value, if preprocess_hash
    method is not NULL
  */
  const char *auth_string;

  /**
    Length of auth_string
  */
  unsigned long auth_string_length;

  /**
    Matching account name as found in the mysql.user table.
    A plugin can override it with another name that will be
    used by MySQL for authorization, and shown in CURRENT_USER()
  */
  char authenticated_as[MYSQL_USERNAME_LENGTH+1]; 


  /**
    The unique user name that was used by the plugin to authenticate.
    Not used by the server.
    Available through the @@EXTERNAL_USER variable.
  */  
  char external_user[MYSQL_USERNAME_LENGTH+1];

  /**
    This only affects the "Authentication failed. Password used: %s"
    error message. has the following values : 
    0 : %s will be NO.
    1 : %s will be YES.
    2 : there will be no %s.
    Set it as appropriate or ignore at will.
  */
  int  password_used;

  /**
    Set to the name of the connected client host, if it can be resolved, 
    or to its IP address otherwise.
  */
  const char *host_or_ip;

  /**
    Length of host_or_ip
  */
  unsigned int host_or_ip_length;

  /**
    Current THD pointer (to use with various services)
  */
  MYSQL_THD thd;

} MYSQL_SERVER_AUTH_INFO;

/**
  Server authentication plugin descriptor
*/
struct st_mysql_auth
{
  int interface_version;                        /**< version plugin uses */
  /**
    A plugin that a client must use for authentication with this server
    plugin. Can be NULL to mean "any plugin".
  */
  const char *client_auth_plugin;
  /**
    Function provided by the plugin which should perform authentication (using
    the vio functions if necessary) and return 0 if successful. The plugin can
    also fill the info.authenticated_as field if a different username should be
    used for authorization.
  */
  int (*authenticate_user)(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info);
  /**
    Create a password hash (or digest) out of a plain-text password

    Used in SET PASSWORD, GRANT, and CREATE USER to convert user specified
    plain-text password into a value that will be stored in mysql.user table.

    @see preprocess_hash

    @param password        plain-text password
    @param password_length plain-text password length
    @param hash            the digest will be stored there
    @param hash_length     in: hash buffer size
                           out: the actual length of the hash

    @return 0 for ok, 1 for error

    Can be NULL, in this case one will not be able to use SET PASSWORD or
    PASSWORD('...') in GRANT, CREATE USER, ALTER USER.
  */
  int (*hash_password)(const char *password, size_t password_length,
                       char *hash, size_t *hash_length);

  /**
    Prepare the password hash for authentication.

    Password hash is stored in the authentication_string column of the
    mysql.user table in a text form. If a plugin needs to preprocess the
    value somehow before the authentication (e.g. convert from hex or base64
    to binary), it can do it in this method. This way the conversion
    will happen only once, not for every authentication attempt.

    The value written to the out buffer will be cached and later made
    available to the authenticate_user() method in the
    MYSQL_SERVER_AUTH_INFO::auth_string[] buffer.

    @return 0 for ok, 1 for error

    Can be NULL, in this case the mysql.user.authentication_string value will
    be given to the authenticate_user() method as is, unconverted.
  */
  int (*preprocess_hash)(const char *hash, size_t hash_length,
                         unsigned char *out, size_t *out_length);
};

#ifdef __cplusplus
}
#endif

#endif

