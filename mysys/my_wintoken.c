/* Copyright (c) 2019, IBM.

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

#include "mysys_priv.h"

BOOL my_obtain_privilege(LPCSTR lpPrivilege)
{
  HANDLE hAccessToken;
  TOKEN_PRIVILEGES token;
  BOOL ret_value= FALSE;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hAccessToken))
  {
    return FALSE;
  }

  if (!LookupPrivilegeValue(NULL, lpPrivilege, &token.Privileges[0].Luid))
    return FALSE;

  token.PrivilegeCount= 1;
  token.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  ret_value= AdjustTokenPrivileges(hAccessToken, FALSE, &token, 0, NULL, NULL);

  if (!ret_value || (GetLastError() != ERROR_SUCCESS))
    return FALSE;

  CloseHandle(hAccessToken);
  return TRUE;
}
