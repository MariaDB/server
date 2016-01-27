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

  auth_pipd authentication plugin.

  Authentication is successful if the connection is done via a named pip and
  the owner of the client process matches the user name that was used when
  connecting to mysqld.
*/


#include <mysql/plugin_auth.h>
#include <string.h>
#include <lmcons.h>





/**
  perform the named pipe´based authentication

  This authentication callback performs a named pipe based authentication -
  it gets the uid of the client process and considers the user authenticated
  if it uses username of this uid. That is - if the user is already
  authenticated to the OS (if she is logged in) - she can use MySQL as herself
*/

static int pipe_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  unsigned char *pkt;
  PTOKEN_USER pTokenUser= NULL;
  HANDLE hToken;
  MYSQL_PLUGIN_VIO_INFO vio_info;
  DWORD dLength= 0;
  int Ret= CR_ERROR;
  TCHAR username[UNLEN + 1];
  DWORD username_length= UNLEN + 1;
  char domainname[DNLEN + 1];
  DWORD domainsize=DNLEN + 1;
  SID_NAME_USE sidnameuse;

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

  /* get the UID of the client process */
  if (!ImpersonateNamedPipeClient(vio_info.handle))
    return CR_ERROR;
  
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_ALL_ACCESS, TRUE, &hToken))
    goto end;
   
 /* determine length of TokenUser */
 GetTokenInformation(hToken, TokenUser, NULL, 0, &dLength);
 if (!dLength)
   goto end;

 if (!(pTokenUser= (PTOKEN_USER)LocalAlloc(0, dLength)))
   goto end;

 if (!GetTokenInformation(hToken, TokenUser, (PVOID)pTokenUser, dLength, &dLength))
   goto end;

 if (!LookupAccountSid(NULL, pTokenUser->User.Sid, username, &username_length, domainname, &domainsize, &sidnameuse))
   goto end;

 Ret= strcmp(username, info->user_name) ? CR_ERROR : CR_OK;
end:
  if (pTokenUser)
    LocalFree(pTokenUser);
  RevertToSelf();
  /* now it's simple as that */
  return Ret;
}

static struct st_mysql_auth pipe_auth_handler=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  0,
  pipe_auth
};

maria_declare_plugin(socket_auth)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &pipe_auth_handler,
  "windows_pipe",
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

