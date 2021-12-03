/*
   Copyright (c) 2011, 2018 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <mysql/plugin_auth_common.h>

struct param {
  unsigned char buf[10240], *ptr;
};


#include "auth_pam_tool.h"


static int roundtrip(struct param *param, const unsigned char *buf,
                     int buf_len, unsigned char **pkt)
{
  unsigned char b=  AP_CONV;
  if (write(1, &b, 1) < 1 || write_string(1, buf, buf_len))
    return -1;
  *pkt= (unsigned char *) param->buf;
  return read_string(0, (char *) param->buf, (int) sizeof(param->buf));
}

typedef struct st_mysql_server_auth_info
{
  /**
    User name as sent by the client and shown in USER().
    NULL if the client packet with the user name was not received yet.
  */
  char *user_name;

  /**
    A corresponding column value from the mysql.user table for the
    matching account name
  */
  char *auth_string;

  /**
    Matching account name as found in the mysql.user table.
    A plugin can override it with another name that will be
    used by MySQL for authorization, and shown in CURRENT_USER()
  */
  char authenticated_as[MYSQL_USERNAME_LENGTH+1]; 
} MYSQL_SERVER_AUTH_INFO;


#include "auth_pam_base.c"


int main(int argc __attribute__((unused)), char **argv __attribute__((unused)))
{
  struct param param;
  MYSQL_SERVER_AUTH_INFO info;
  unsigned char field;
  int res;
  char a_buf[MYSQL_USERNAME_LENGTH + 1 + 1024];

  if ((res= setreuid(0, 0)))
    fprintf(stderr, "Got error %d from setreuid()\n", (int) errno);

  if (read(0, &field, 1) < 1)
    return -1;
#ifndef DBUG_OFF
  pam_debug= field & 1;
#endif
  winbind_hack= field & 2;
  
  PAM_DEBUG((stderr, "PAM: sandbox started [%s].\n", argv[0]));

  info.user_name= a_buf;
  if ((res= read_string(0, info.user_name, sizeof(a_buf))) < 0)
    return -1;
  PAM_DEBUG((stderr, "PAM: sandbox username [%s].\n", info.user_name));

  info.auth_string= info.user_name + res + 1;
  if (read_string(0, info.auth_string, sizeof(a_buf) - 1 - res) < 0)
    return -1;

  PAM_DEBUG((stderr, "PAM: sandbox auth string [%s].\n", info.auth_string));

  if ((res= pam_auth_base(&param, &info)) != CR_OK)
  {
    PAM_DEBUG((stderr, "PAM: auth failed, sandbox closed.\n"));
    return -1;
  }

  if (info.authenticated_as[0])
  {
    PAM_DEBUG((stderr, "PAM: send authenticated_as field.\n"));
    field= AP_AUTHENTICATED_AS;
    if (write(1, &field, 1) < 1 ||
        write_string(1, (unsigned char *) info.authenticated_as,
                     strlen(info.authenticated_as)))
      return -1;
  }
  
  PAM_DEBUG((stderr, "PAM: send OK result.\n"));
  field= AP_EOF;
  if (write(1, &field, 1) != 1)
    return -1;

  PAM_DEBUG((stderr, "PAM: sandbox closed.\n"));
  return 0;
}
