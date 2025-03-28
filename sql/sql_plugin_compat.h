/* Copyright (C) 2013 Sergei Golubchik and Monty Program Ab

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

/* old plugin api structures, used for backward compatibility */

/**************************************************************/
/* Authentication API, version 0x0100 *************************/
#define MIN_AUTHENTICATION_INTERFACE_VERSION 0x0100

struct MYSQL_SERVER_AUTH_INFO_0x0100 {
  const char *user_name;
  unsigned int user_name_length;
  const char *auth_string;
  unsigned long auth_string_length;
  char authenticated_as[49]; 
  char external_user[512];
  int  password_used;
  const char *host_or_ip;
  unsigned int host_or_ip_length;
#ifdef MYSQL_SERVER
  void downgrade(MYSQL_SERVER_AUTH_INFO *latest);
  void upgrade(MYSQL_SERVER_AUTH_INFO *latest);
#endif
};

struct st_mysql_auth_0x0100
{
  int interface_version;
  const char *client_auth_plugin;
  int (*authenticate_user)(MYSQL_PLUGIN_VIO *vio, struct MYSQL_SERVER_AUTH_INFO_0x0100 *info);
};
/**************************************************************/

