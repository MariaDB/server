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

/*
  OS Credential manager support for MariaDB command line clients

  Credential  manager can be considered as secure key-value store
  with usual operation of get/set/delete. There is one such store
  per OS user.

  The key in these key-value operations is generated based on
  host, user, port and socket name. e.g. for a connection to
  localhost with user root, on port 10000
  the key will be MARIADB/root@localhost:1000

  Note : currently, only Windows is supported. It should be relatively
  easy to implement macOS keychain.
*/

#include <my_global.h>
#include <my_sys.h>
#ifdef _WIN32
#include <wincred.h>
#endif

/*
  Create connection string key, given
  parameters such as host, port or user name.

  @param out output buffer
  @param sz  output buffer size
  @param host host name
  @param user user name
  @param port port number
  @param socket socket name

  @return pointer to out buffer
*/
char *credmgr_make_target(char *out, size_t sz, const char *host,
                         const char *user, uint port,
                         const char *unix_socket)
{
  const char *end= out + sz;
  out+= my_snprintf(out, sz, "MARIADB/%s@%s", user ? user : "",
                    host ? host : "localhost");
  if (port)
    out+= my_snprintf(out, (size_t) (end - out), ":%u", port);
  if (unix_socket)
    out+= my_snprintf(out, (size_t) (end - out), "?socket=%s", unix_socket);
  return out;
}

/*
   Retrieve password from credential manager

   Windows Credentials UI and command line tools 'cmdkey' use UTF-16LE for
   passwords even if API allows for opaque "blobs" We need to store/read
   password in UTF-16 for interoperability.

   @param target_name  credential manager key, see credmgr_make_target

   @return stored password, as C string, or null pointer. The memory is
           allocated using my_malloc, and should be freed by caller.
*/
char *credmgr_get_password(const char *target_name)
{
#ifdef _WIN32
  CREDENTIALA *cred;
  if (!CredReadA(target_name, CRED_TYPE_GENERIC, 0, &cred))
    return nullptr;
  size_t sz= cred->CredentialBlobSize * 2 + 1;
  char *b= (char *) my_malloc(0, sz, MY_WME | MY_ZEROFILL);
  if (!b)
    return nullptr;
  if (!WideCharToMultiByte(CP_UTF8, 0, (LPCWCH) cred->CredentialBlob,
                           cred->CredentialBlobSize / 2, b, (int) sz - 1, 0,
                           0))
  {
    my_free(b);
    b= nullptr;
    DBUG_ASSERT(0);
  }
  CredFree(cred);
  return (char *) b;
#else
#error "Not implemented"
#endif
}

/*
  Remove password from credential manager
  @param  target_name  credential manager key, see credmgr_make_target
*/
void credmgr_remove_password(const char *target_name)
{
#ifdef _WIN32
  CredDelete(target_name, CRED_TYPE_GENERIC, 0);
#else
#error "Not implemented"
#endif
}

/*
  Save password to credential manager
  @param target_name  credential manager key, see credmgr_make_target
  @param password     password to store
*/
void credmgr_save_password(const char *target_name,
                                  const char *password)
{
#ifdef _WIN32
  if (!password || !password[0])
    return;

  size_t len= strlen(password) + 1;
  wchar_t *wstr= (wchar_t *) my_malloc(0, sizeof(wchar_t) * len, MY_WME);
  if (!wstr)
    return;
  if (MultiByteToWideChar(CP_UTF8, 0, password, (int) len, wstr, (int) len) ==
      0)
  {
    my_free(wstr);
    return;
  }
  CREDENTIAL cred= {0};
  cred.Type= CRED_TYPE_GENERIC;
  cred.TargetName= (LPSTR) target_name;
  cred.CredentialBlobSize= 2 * (DWORD) wcslen(wstr);
  cred.CredentialBlob= (LPBYTE) wstr;
  cred.Persist= CRED_PERSIST_LOCAL_MACHINE;
  BOOL ok= ::CredWriteA(&cred, 0);
  DBUG_ASSERT(ok);
#else
#error "Not implemented"
#endif
}

