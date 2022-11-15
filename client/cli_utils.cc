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
   Common functionality shared between all CLI programs.
   Currently covers connecting to the server only.
*/

#include <my_global.h>
#include <my_sys.h>
#include "cli_utils.h"

/*
   A wrapper mysql_real_connect that includes optional
   parameter control be used for interactive command line password
   input.

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
*/
extern "C" MYSQL *cli_connect(MYSQL *mysql, const char *host, const char *user,
                   char **ppasswd, const char *db, unsigned int port,
                   const char *unix_socket, unsigned long client_flag,
                   my_bool tty_password)
{
  MYSQL *ret;
  bool use_tty_prompt= (*ppasswd == nullptr && tty_password);

  if (use_tty_prompt)
    *ppasswd= get_tty_password(NullS);

  ret= mysql_real_connect(mysql, host, user, *ppasswd, db, port, unix_socket,
                          client_flag);
  return ret;
}
