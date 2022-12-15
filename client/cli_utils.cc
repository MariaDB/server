/* Copyright (c) 2022 MariaDB

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
   Common functionality shared between command line utilities
   that are part of MariaDB distribution.
*/

#include <my_global.h>
#include <my_sys.h>
#include "cli_utils.h"
#include "credmgr.h"

#ifdef CREDMGR_SUPPORTED
#include <mysqld_error.h>
#endif

/*
  Connect to server. A wrapper for mysql_read_connect.

  Will ask password interactively, if required.

  On systems with credential manager (currently Windows only)
  might query and update password in credential manager.

  When using credential manager, following rules are in place

  1. Password is provided via command line
     If MARIADB_SAVE_CREDMGR_PASSWORD is set, and connection can be established
     password is saved in credential manager.

  2. Password is NOT set on the command line, interactive authentication is NOT requested
     Password is read from credential manager

  3. Interactive authentication is requested - "-p" option for command line client.
     - Password is read from credential manager, and if it exists an attempt is made
     to connect with the stored password. 
     If password does not exist in credential manager, or attempt to connect with stored
     password fails, interactive passwqord prompt is presented.
     Upon successfull connection, password is stored in credential manager.

  4. If password was read from credential manager in any of the above steps, and
     attempt to connect with that password fails, saved credentials are removed.

  @param mysql    pointer to MYSQL object. @see mysql_real_connect
  @param host     host name. @see mysql_real_connect
  @param user     user name. @see mysql_real_connect
  @param passwd   password @see mysql_real_connect
  @param db       database name. @see mysql_real_connect
  @param port     port number. @see mysql_real_connect
  @param unix_socket  unix socket name. @see mysql_real_connect
  @param client_flag  client flags. @see mysql_real_connect
  @param tty_password  if true, and password is NULL, prompt for
                       password on terminal
  @param allow_credmgr allow reading/storing password using credential manager
*/
extern "C" MYSQL *cli_connect(MYSQL *mysql, const char *host, const char *user,
                   char **ppasswd, const char *db, unsigned int port,
                   const char *unix_socket, unsigned long client_flag,
                   my_bool tty_password)
{
  MYSQL *ret;
  bool use_tty_prompt= (*ppasswd == nullptr && tty_password);

#ifdef CREDMGR_SUPPORTED
  char target_name[FN_REFLEN];
  credmgr_make_target(target_name, sizeof(target_name), host, user, port,
                      unix_socket);
  bool use_credmgr_password= false;
  bool save_credmgr_password= getenv("MARIADB_CREDMGR_SAVE_PASSWORD") != nullptr;
  if (!*ppasswd)
  {
    save_credmgr_password = true;
    /* Interactive login or use credential manager if OS supports it.*/
    *ppasswd= credmgr_get_password(target_name);
    if (*ppasswd)
    {
      use_credmgr_password= true;
      use_tty_prompt= false;
    }
  }

retry_with_tty_prompt:
#endif

  if (use_tty_prompt)
    *ppasswd= get_tty_password(NullS);

  ret= mysql_real_connect(mysql, host, user, *ppasswd, db, port, unix_socket,
                          client_flag);

#ifdef CREDMGR_SUPPORTED
  if (!ret)
  {
    DBUG_ASSERT(mysql);
    if (use_credmgr_password)
    {
      if (mysql_errno(mysql) == ER_ACCESS_DENIED_ERROR)
        credmgr_remove_password(target_name);
      use_credmgr_password= false;
      if (tty_password && use_tty_prompt)
        goto retry_with_tty_prompt;
    }
    return ret;
  }
  if (save_credmgr_password)
  {
    credmgr_save_password(target_name, *ppasswd);
  }
#endif
  return ret;
}
