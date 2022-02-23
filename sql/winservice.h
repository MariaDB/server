/*
   Copyright (c) 2011, 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Extract properties of a windows service binary path
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4995)
#endif
#include <winsvc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef struct mysqld_service_properties_st
{
  char mysqld_exe[MAX_PATH];
  char inifile[MAX_PATH];
  char datadir[MAX_PATH];
  int  version_major;
  int  version_minor;
  int  version_patch;
} mysqld_service_properties;

extern int get_mysql_service_properties(const wchar_t *bin_path,
  mysqld_service_properties *props);


#if !defined(UNICODE)
/*
  The following wrappers workaround Windows bugs
  with CreateService/OpenService with ANSI codepage UTF8.

  Apparently, these function in ANSI mode, for this codepage only
  do *not* behave as expected (as-if string parameters were
  converted to UTF16 and "wide" function were called)
*/
#include <malloc.h>
static inline wchar_t* awstrdup(const char *str)
{
  if (!str)
    return NULL;
  size_t len= strlen(str) + 1;
  wchar_t *wstr= (wchar_t *) malloc(sizeof(wchar_t)*len);
  if (MultiByteToWideChar(GetACP(), 0, str, (int)len, wstr, (int)len) == 0)
  {
    free(wstr);
    return NULL;
  }
  return wstr;
}

#define AWSTRDUP(dest, src)                                                   \
  dest= awstrdup(src);                                                        \
  if (src && !dest)                                                           \
  {                                                                           \
    ok= FALSE;                                                                \
    last_error = ERROR_OUTOFMEMORY;                                           \
    goto end;                                                                 \
  }

static inline SC_HANDLE my_OpenService(SC_HANDLE hSCManager, LPCSTR lpServiceName, DWORD dwDesiredAccess)
{
  wchar_t *w_ServiceName= NULL;
  BOOL ok=TRUE;
  DWORD last_error=0;
  SC_HANDLE sch=NULL;

  AWSTRDUP(w_ServiceName, lpServiceName);
  sch= OpenServiceW(hSCManager, w_ServiceName, dwDesiredAccess);
  if (!sch)
  {
    ok= FALSE;
    last_error= GetLastError();
  }

end:
  free(w_ServiceName);
  if (!ok)
    SetLastError(last_error);
  return sch;
}

static inline SC_HANDLE my_CreateService(SC_HANDLE hSCManager,
  LPCSTR lpServiceName, LPCSTR lpDisplayName,
  DWORD dwDesiredAccess,  DWORD dwServiceType,
  DWORD dwStartType, DWORD dwErrorControl,
  LPCSTR lpBinaryPathName, LPCSTR lpLoadOrderGroup,
  LPDWORD lpdwTagId, LPCSTR lpDependencies,
  LPCSTR lpServiceStartName, LPCSTR lpPassword)
{
  wchar_t *w_ServiceName= NULL;
  wchar_t *w_DisplayName= NULL;
  wchar_t *w_BinaryPathName= NULL;
  wchar_t *w_LoadOrderGroup= NULL;
  wchar_t *w_Dependencies= NULL;
  wchar_t *w_ServiceStartName= NULL;
  wchar_t *w_Password= NULL;
  SC_HANDLE sch = NULL;
  DWORD last_error=0;
  BOOL ok= TRUE;

  AWSTRDUP(w_ServiceName,lpServiceName);
  AWSTRDUP(w_DisplayName,lpDisplayName);
  AWSTRDUP(w_BinaryPathName, lpBinaryPathName);
  AWSTRDUP(w_LoadOrderGroup, lpLoadOrderGroup);
  AWSTRDUP(w_Dependencies, lpDependencies);
  AWSTRDUP(w_ServiceStartName, lpServiceStartName);
  AWSTRDUP(w_Password, lpPassword);

  sch= CreateServiceW(
      hSCManager, w_ServiceName, w_DisplayName, dwDesiredAccess, dwServiceType,
      dwStartType, dwErrorControl, w_BinaryPathName, w_LoadOrderGroup,
      lpdwTagId, w_Dependencies, w_ServiceStartName, w_Password);
  if(!sch)
  {
    ok= FALSE;
    last_error= GetLastError();
  }

end:
  free(w_ServiceName);
  free(w_DisplayName);
  free(w_BinaryPathName);
  free(w_LoadOrderGroup);
  free(w_Dependencies);
  free(w_ServiceStartName);
  free(w_Password);

  if (!ok)
    SetLastError(last_error);
  return sch;
}

static inline BOOL my_ChangeServiceConfig(SC_HANDLE hService, DWORD dwServiceType,
   DWORD dwStartType, DWORD dwErrorControl,
   LPCSTR lpBinaryPathName, LPCSTR lpLoadOrderGroup,
   LPDWORD lpdwTagId, LPCSTR lpDependencies,
   LPCSTR lpServiceStartName, LPCSTR lpPassword,
   LPCSTR lpDisplayName)
{
  wchar_t *w_DisplayName= NULL;
  wchar_t *w_BinaryPathName= NULL;
  wchar_t *w_LoadOrderGroup= NULL;
  wchar_t *w_Dependencies= NULL;
  wchar_t *w_ServiceStartName= NULL;
  wchar_t *w_Password= NULL;
  DWORD last_error=0;
  BOOL ok= TRUE;

  AWSTRDUP(w_DisplayName, lpDisplayName);
  AWSTRDUP(w_BinaryPathName, lpBinaryPathName);
  AWSTRDUP(w_LoadOrderGroup, lpLoadOrderGroup);
  AWSTRDUP(w_Dependencies, lpDependencies);
  AWSTRDUP(w_ServiceStartName, lpServiceStartName);
  AWSTRDUP(w_Password, lpPassword);

  ok= ChangeServiceConfigW(
      hService, dwServiceType, dwStartType, dwErrorControl, w_BinaryPathName,
      w_LoadOrderGroup, lpdwTagId, w_Dependencies, w_ServiceStartName,
      w_Password, w_DisplayName);
  if (!ok)
  {
    last_error= GetLastError();
  }

end:
  free(w_DisplayName);
  free(w_BinaryPathName);
  free(w_LoadOrderGroup);
  free(w_Dependencies);
  free(w_ServiceStartName);
  free(w_Password);

  if (last_error)
    SetLastError(last_error);
  return ok;
}
#undef AWSTRDUP

#undef OpenService
#define OpenService my_OpenService
#undef ChangeServiceConfig
#define ChangeServiceConfig my_ChangeServiceConfig
#undef CreateService
#define CreateService my_CreateService
#endif

#ifdef __cplusplus
}
#endif
