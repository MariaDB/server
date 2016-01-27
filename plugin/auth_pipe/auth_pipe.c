/* Copyright (C) 2015 Vladislav Vaintroub, Georg Richter and Monty Program Ab
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; version 2 of the
    License.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA */

/**
  @file

  auth_pipe authentication plugin.

  Authentication is successful if the connection is done via a named pipe 
  pipe peer name matches mysql user name
*/

#include <mysql/plugin_auth.h>
#include <string.h>
#include <lmcons.h>


/**
  This authentication callback obtains user name using named pipe impersonation
*/
static int pipe_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  unsigned char *pkt;
  MYSQL_PLUGIN_VIO_INFO vio_info;
  char username[UNLEN + 1];
  size_t username_length;
  int ret;

  /* no user name yet ? read the client handshake packet with the user name */
  if (info->user_name == 0)
  {
    if (vio->read_packet(vio, &pkt) < 0)
      return CR_ERROR;
  }
  info->password_used= PASSWORD_USED_NO_MENTION;
  vio->info(vio, &vio_info);
  if (vio_info.protocol != MYSQL_VIO_PIPE)
    return CR_ERROR;

  /* Impersonate the named pipe peer, and retrieve the user name */
  if (!ImpersonateNamedPipeClient(vio_info.handle))
    return CR_ERROR;

  username_length= sizeof(username) - 1;
  ret= CR_ERROR;
  if (GetUserName(username, &username_length))
  {
    /* Always compare names case-insensitive on Windows.*/
    if (_stricmp(username, info->user_name) == 0)
      ret= CR_OK;
  }
  RevertToSelf();

  return ret;
}

static struct st_mysql_auth pipe_auth_handler=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  0,
  pipe_auth
};

maria_declare_plugin(auth_named_pipe)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &pipe_auth_handler,
  "named_pipe",
  "Vladislav Vaintroub, Georg Richter",
  "Windows named pipe based authentication",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

