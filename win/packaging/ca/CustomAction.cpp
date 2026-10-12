/* Copyright 2010, Oracle and/or its affiliates. All rights reserved.

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

#ifndef UNICODE
#define UNICODE
#endif

#undef NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winreg.h>
#include <msi.h>
#include <msiquery.h>
#include <string.h>
#include <strsafe.h>
#include <assert.h>
#include <stdarg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <winservice.h>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>

using namespace std;


#define ONE_MB 1048576

/*
  Limits of property values. Values that go to fixed size APIs, or have
  natural limits elsewhere, are rejected if they are longer, instead of
  being silently truncated.
*/
/* The longest service name accepted by CreateService() */
static const size_t MAX_SERVICENAME_LEN= 256;
/* TCP port has 5 digits at most */
static const size_t MAX_PORT_LEN= 5;
/* Innodb buffer pool size is a number of megabytes */
static const size_t MAX_BUFFERPOOLSIZE_LEN= 15;
/* Directory is listed with FindFirstFile(), "*.*" has to fit into MAX_PATH */
static const size_t MAX_DIRECTORY_LEN= MAX_PATH - 4;
/*
  The password ends up in a command line, which is limited to 32767
  characters, and escaping at most doubles it.
*/
static const size_t MAX_PASSWORD_LEN= 8192;

UINT ExecRemoveDataDirectory(std::wstring dir)
{
  /* Strip stray backslash */
  if (!dir.empty() && dir.back() == L'\\')
    dir.pop_back();
  if (dir.empty())
    return ERROR_INVALID_PARAMETER;

  /*
    pFrom is a list of names, every name ends with NUL, and the list ends
    with one more. The explicit NUL, together with the one that string
    always has, makes it a list of a single name.
  */
  dir.push_back(L'\0');

  SHFILEOPSTRUCTW fileop;
  fileop.hwnd= NULL;    /* no status display */
  fileop.wFunc= FO_DELETE;  /* delete operation */
  fileop.pFrom= dir.c_str();  /* source file name, double null terminated */
  fileop.pTo= NULL;    /* no destination needed */
  fileop.fFlags= FOF_NOCONFIRMATION|FOF_SILENT;  /* do not prompt the user */

  fileop.fAnyOperationsAborted= FALSE;
  fileop.lpszProgressTitle= NULL;
  fileop.hNameMappings= NULL;

  return SHFileOperationW(&fileop);
}

// Write a message to the MSI log
static void WriteMsiLog(MSIHANDLE hInstall, const wchar_t *str)
{
  PMSIHANDLE hRecord= MsiCreateRecord(1);
  if (hRecord)
  {
    MsiRecordSetStringW(hRecord, 0, str);
    MsiProcessMessage(hInstall, INSTALLMESSAGE_INFO, hRecord);
  }
}

// Log a formatted message, prefixed with the function name, to the MSI log.
static void LogMessage(MSIHANDLE hInstall, const char *function,
                       const wchar_t *format, ...)
{
  wchar_t buf[1024];
  int prefix_len= _snwprintf_s(buf, _TRUNCATE, L"MariaDB_CA %S:", function);
  if (prefix_len < 0)
    prefix_len= 0;
  va_list args;
  va_start(args, format);
  _vsnwprintf_s(buf + prefix_len, ARRAYSIZE(buf) - prefix_len, _TRUNCATE,
                format, args);
  va_end(args);
  WriteMsiLog(hInstall, buf);
}

/* Needs hInstall in scope, the format has to be a wide string literal. */
#define Log(...) LogMessage(hInstall, __FUNCTION__, __VA_ARGS__)

/*
  Get the value of an MSI property, whatever its length is.
  A property that is not set has an empty value.

  Returns false (and logs) if the value can't be retrieved. Callers must not
  use the property then, a truncated value, e.g path, is worse than no value.
*/
static bool GetProperty(MSIHANDLE hInstall, const wchar_t *name,
                        std::wstring &value)
{
  wchar_t buf[MAX_PATH];
  DWORD len= MAX_PATH;
  UINT er= MsiGetPropertyW(hInstall, name, buf, &len);
  if (er == ERROR_SUCCESS)
  {
    value.assign(buf, len);
    return true;
  }
  if (er == ERROR_MORE_DATA)
  {
    /* len is the required length, without the terminating NUL */
    std::wstring tmp(len + 1, L'\0');
    len++;
    er= MsiGetPropertyW(hInstall, name, &tmp[0], &len);
    if (er == ERROR_SUCCESS)
    {
      tmp.resize(len);
      value.swap(tmp);
      return true;
    }
  }
  Log(L"Cannot get property %s, error %u", name, er);
  value.clear();
  return false;
}

/*
  Convert to a narrow string in the ANSI codepage, the form of paths in
  mysqld_service_properties, to be able to compare with them.
*/
static std::string ToAnsi(const std::wstring &s)
{
  int len= WideCharToMultiByte(CP_ACP, 0, s.c_str(), (int)s.length(), NULL, 0,
                               NULL, NULL);
  std::string result(len > 0 ? len : 0, '\0');
  if (len > 0)
    WideCharToMultiByte(CP_ACP, 0, s.c_str(), (int)s.length(), &result[0], len,
                        NULL, NULL);
  return result;
}

extern "C" UINT __stdcall RemoveDataDirectory(MSIHANDLE hInstall)
{
  std::wstring dir;
  UINT er;

  Log(L"Initialized.");

  /* Never delete, if the directory is not known for sure. */
  if (!GetProperty(hInstall, L"CustomActionData", dir) || dir.empty())
  {
    Log(L"Directory to remove is not known, nothing is removed");
    return ERROR_INSTALL_FAILURE;
  }

  er= ExecRemoveDataDirectory(dir);
  Log(L"SHFileOperation returned %d", er);
  return er;
}

/*
  Escape command line parameter for pass to CreateProcess().

  The caller adds double quotation marks before and after the string, so
  the backslashes at the end of the string are always escaped, the
  closing quotation mark would be taken as escaped otherwise.
*/
static std::wstring EscapeCommandLine(const std::wstring &in)
{
  std::wstring out;
  for (size_t i= 0;; i++)
  {
    size_t n_backslashes= 0;
    while (i < in.length() && in[i] == L'\\')
    {
      i++;
      n_backslashes++;
    }

    if (i == in.length())
    {
      /*
        Escape all backslashes, but let the terminating double quotation mark
        that caller adds be interpreted as a metacharacter.
      */
      out.append(2*n_backslashes, L'\\');
      break;
    }

    if (in[i] == L'"')
    {
      /*
        Escape all backslashes and the following double quotation mark.
      */
      out.append(2*n_backslashes + 1, L'\\');
      out.push_back(L'"');
    }
    else
    {
      /* Backslashes aren't special here. */
      out.append(n_backslashes, L'\\');
      out.push_back(in[i]);
    }
  }
  return out;
}

bool IsDirectoryEmptyOrNonExisting(const std::wstring &dir) {
  WIN32_FIND_DATAW data;
  HANDLE h;
  bool empty= true;
  h= FindFirstFileW((dir + L"*.*").c_str(), &data);
  if (h != INVALID_HANDLE_VALUE)
  {
    for (;;)
    {
      if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L".."))
      {
        empty= false;
        break;
      }
      if (!FindNextFileW(h, &data))
        break;
    }
    FindClose(h);
  }
  return empty;
}

extern "C" UINT __stdcall CheckInstallDirectory(MSIHANDLE hInstall)
{
  std::wstring path;

  if (!GetProperty(hInstall, L"INSTALLDIR", path))
    return ERROR_INSTALL_FAILURE;

  if (path.length() > MAX_DIRECTORY_LEN)
  {
    MsiSetPropertyW(hInstall, L"INSTALLDIRERROR",
                    L"Installation directory path is too long");
  }
  else if (!IsDirectoryEmptyOrNonExisting(path))
  {
    wchar_t msg[2*MAX_PATH];
    swprintf(msg,ARRAYSIZE(msg), L"Installation directory '%s' exists and is not empty. Choose a "
                  "different install directory",path.c_str());
    MsiSetPropertyW(hInstall,L"INSTALLDIRERROR", msg);
  }
  else
  {
    MsiSetPropertyW(hInstall, L"INSTALLDIRERROR", L"");
  }
  return ERROR_SUCCESS;
}

/*
  Check for valid data directory is empty during install
  A valid data directory is non-existing, or empty.

  In addition, it must be different from any directories that
  are going to be installed. This is required. because the full
  directory is removed on a feature uninstall, and we do not want
  it to be lib or bin.
*/
extern "C" UINT __stdcall CheckDataDirectory(MSIHANDLE hInstall)
{
  std::wstring datadir;
  std::wstring installdir;

  if (!GetProperty(hInstall, L"DATADIR", datadir) ||
      !GetProperty(hInstall, L"INSTALLDIR", installdir))
    return ERROR_INSTALL_FAILURE;

  Log(L"Checking files in %s", datadir.c_str());
  if (datadir.length() > MAX_DIRECTORY_LEN)
  {
    MsiSetPropertyW(hInstall, L"DATADIRERROR",
                    L"data directory path is too long");
    return ERROR_SUCCESS;
  }

  if (!IsDirectoryEmptyOrNonExisting(datadir))
  {
    Log(L"DATADIR is NOT empty");
    MsiSetPropertyW(hInstall, L"DATADIRERROR",
                    L"data directory exists and is not empty");
    return ERROR_SUCCESS;
  }
  Log(L"DATADIR is empty or non-existent");
  MsiSetPropertyW(hInstall, L"DATADIRERROR", L"");
  Log(L"INSTALLDIR check");

  if (!_wcsicmp(datadir.c_str(), installdir.c_str()))
  {
    MsiSetPropertyW(hInstall, L"DATADIRERROR", L"data directory can not be "
                                    L"installation root directory");
    return ERROR_SUCCESS;
  }
  for (auto dir :
       {L"bin\\", L"include\\",
        L"lib\\", L"share\\"})
  {
    std::wstring localpath= installdir + dir;
    if (!_wcsnicmp(datadir.c_str(), localpath.c_str(), localpath.length()))
    {
      wchar_t msg[MAX_PATH]= L"data directory conflicts with '";
      wcsncat_s(msg, dir, wcslen(dir) - 1);
      wcscat_s(msg, L"' directory, which is part of this installation");
      MsiSetPropertyW(hInstall, L"DATADIRERROR", msg);
      return ERROR_SUCCESS;
    }
  }
  return ERROR_SUCCESS;
}


bool CheckServiceExists(const wchar_t *name)
{
   SC_HANDLE manager =0, service=0;
   manager = OpenSCManager( NULL, NULL, SC_MANAGER_CONNECT);
   if (!manager)
   {
     return false;
   }

   service = OpenService(manager, name, SC_MANAGER_CONNECT);
   if(service)
     CloseServiceHandle(service);
   CloseServiceHandle(manager);

   return service?true:false;
}

/* User in rollback of create database custom action */
bool ExecRemoveService(const wchar_t *name)
{
   SC_HANDLE manager =0, service=0;
   manager = OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS);
   bool ret;
   if (!manager)
   {
     return false;
   }
   service = OpenService(manager, name, DELETE);
   if(service)
   {
     ret= DeleteService(service);
   }
   else
   {
     ret= false;
   }
   CloseServiceHandle(manager);
   return ret;
}

/* Find whether TCP port is in use by trying to bind to the port. */
static bool IsPortInUse(unsigned short port)
{
  struct addrinfo* ai, * a;
  struct addrinfo hints {};

  char port_buf[NI_MAXSERV];
  SOCKET ip_sock = INVALID_SOCKET;
  hints.ai_flags = AI_PASSIVE;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  snprintf(port_buf, NI_MAXSERV, "%u", (unsigned)port);

  if (getaddrinfo(NULL, port_buf, &hints, &ai))
  {
    return false;
  }

  /*
   Prefer IPv6 socket to IPv4, since we'll use IPv6 dual socket,
   which coveres both IP versions.
  */
  for (a = ai; a; a = a->ai_next)
  {
    if (a->ai_family == AF_INET6 &&
      (ip_sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) != INVALID_SOCKET)
    {
      break;
    }
  }

  if (ip_sock == INVALID_SOCKET)
  {
    for (a = ai; a; a = a->ai_next)
    {
      if (ai->ai_family == AF_INET &&
        (ip_sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) != INVALID_SOCKET)
      {
        break;
      }
    }
  }

  if (ip_sock == INVALID_SOCKET)
  {
    return false;
  }

  /* Use SO_EXCLUSIVEADDRUSE to prevent multiple binding. */
  int arg = 1;
  setsockopt(ip_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&arg, sizeof(arg));

  /* Allow dual socket, so that IPv4 and IPv6 are both covered.*/
  if (a->ai_family == AF_INET6)
  {
    arg = 0;
    setsockopt(ip_sock, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&arg, sizeof(arg));
  }

  bool in_use = false;
  if (bind(ip_sock, a->ai_addr, (int)a->ai_addrlen) == SOCKET_ERROR)
  {
    DWORD last_error = WSAGetLastError();
    in_use = (last_error ==  WSAEADDRINUSE || last_error == WSAEACCES);
  }

  freeaddrinfo(ai);
  closesocket(ip_sock);
  return in_use;
}


/*
  Check if TCP port is free
*/
bool IsPortFree(unsigned short port)
{
  WORD wVersionRequested = MAKEWORD(2, 2);
  WSADATA wsaData;
  WSAStartup(wVersionRequested, &wsaData);
  bool in_use = IsPortInUse(port);
  WSACleanup();
  return !in_use;
}


/*
   Helper function used in filename normalization.
   Removes leading quote and terminates string at the position of the next one
   (if applicable, does not change string otherwise). Returns modified string
*/
wchar_t *strip_quotes(wchar_t *s)
{
   if (s && (*s == L'"'))
   {
     s++;
     wchar_t *p = wcschr(s, L'"');
     if(p)
       *p = 0;
   }
   return s;
}


/*
  Checks  for consistency of service configuration.

  It can happen that SERVICENAME or DATADIR
  MSI properties are in inconsistent state after somebody upgraded database
  We catch this case during uninstall. In particular, either service is not
  removed even if SERVICENAME was set (but this name is reused by someone else)
  or data directory is not removed (if it is used by someone else). To find out
  whether service name and datadirectory are in use For every service,
  configuration is read and checked as follows:

  - look if a service has to do something with mysql
  - If so, check its name against SERVICENAME. if match, check binary path
    against INSTALLDIR\bin.  If binary path does not match, then service runs
    under different  installation and won't be removed.
  - Check options file for datadir and look if this is inside this
    installation's datadir don't remove datadir if this is the case.

  "Don't remove" in this context means that custom action is removing
  SERVICENAME property or CLEANUPDATA property, which later on in course of
  installation mean, that either datadir or service is kept.
*/

void CheckServiceConfig(
  MSIHANDLE hInstall,         /* MSI handle */
  const wchar_t *my_servicename,    /* SERVICENAME property in this installation*/
  const wchar_t *datadir,           /* DATADIR property in this installation*/
  const wchar_t *bindir,            /* INSTALLDIR\bin */
  const wchar_t *other_servicename, /* Service to check against */
  QUERY_SERVICE_CONFIGW * config /* Other service's config */
  )
{

  bool same_bindir = false;
  wchar_t * commandline= config->lpBinaryPathName;
  int numargs;
  wchar_t **argv= CommandLineToArgvW(commandline, &numargs);
  wchar_t current_datadir_buf[MAX_PATH]={0};
  wchar_t normalized_current_datadir[MAX_PATH+1]= {0};
  wchar_t *current_datadir;
  wchar_t *defaults_file;
  bool is_my_service;

  Log(L"CommandLine= %s", commandline);
  if(!argv  ||  !argv[0]  || ! wcsstr(argv[0], L"mysqld"))
  {
    goto end;
  }

  Log(L"MySQL/MariaDB service %s found: CommandLine= %s",
    other_servicename, commandline);
  if (wcsstr(argv[0], bindir))
  {
   Log(L"executable under bin directory");
    same_bindir = true;
  }

  is_my_service = (_wcsicmp(my_servicename, other_servicename) == 0);
  if(!is_my_service)
  {
    Log(L"service does not match current service");
    /*
      TODO probably the best thing possible would be to add temporary
      row to MSI ServiceConfig table with remove on uninstall
    */
  }
  else if (!same_bindir)
  {
    Log(L"Service name matches, but not the executable path directory, mine is %s",
      bindir);
    MsiSetPropertyW(hInstall,L"SERVICENAME", L"");
  }

  /* Check if data directory is used */
  if(!datadir || numargs <= 1 ||  wcsncmp(argv[1],L"--defaults-file=",16) != 0)
  {
    goto end;
  }

  current_datadir= current_datadir_buf;
  defaults_file= argv[1]+16;
  defaults_file= strip_quotes(defaults_file);

  Log(L"parsed defaults file is %s", defaults_file);

  if (GetPrivateProfileStringW(L"mysqld", L"datadir", NULL, current_datadir,
    MAX_PATH, defaults_file) == 0)
  {
    Log(L"Cannot find datadir in ini file '%s'", defaults_file);
    goto end;
  }

  Log(L"datadir from defaults-file is %s", current_datadir);
  strip_quotes(current_datadir);

  /* Convert to Windows path */
  if (GetFullPathNameW(current_datadir, MAX_PATH, normalized_current_datadir,
    NULL))
  {
    /* Add backslash to be compatible with directory formats in MSI */
    wcscat_s(normalized_current_datadir, L"\\");
    Log(L"normalized current datadir is '%s'",
      normalized_current_datadir);
  }

  if (_wcsicmp(datadir, normalized_current_datadir) == 0 && !same_bindir)
  {
    Log(L"database directory from current installation, but different mysqld.exe");
    MsiSetPropertyW(hInstall,L"CLEANUPDATA", L"");
  }

end:
  LocalFree((HLOCAL)argv);
}

/*
  Checks if database directory or service are modified by user
  For example, service may point to different mysqld.exe that it was originally
  installed, or some different service might use this database directory. This
  would normally mean user has done an upgrade of the database and in this case
  uninstall should neither delete service nor database directory.

  If this function find that service is modified by user (mysqld.exe used by
  service does not point to the installation bin directory), MSI public variable
  SERVICENAME is removed, if DATADIR is used by some other service, variables
  DATADIR and CLEANUPDATA are removed.

  The effect of variable removal is that service does not get uninstalled and
  datadir is not touched by uninstallation.

  Note that this function is running without elevation and does not use anything
  that would require special privileges.

*/
extern "C" UINT CheckDBInUse(MSIHANDLE hInstall)
{
  static BYTE buf[256*1024]; /* largest possible buffer for EnumServices */
  static char config_buffer[8*1024]; /*largest buffer for QueryServiceConfig */
  UINT er = ERROR_SUCCESS;
  std::wstring servicename;
  std::wstring datadir;
  std::wstring installdir;
  std::wstring bindir;
  SC_HANDLE scm    = NULL;
  ULONG  bufsize   = sizeof(buf);
  ULONG  bufneed   = 0x00;
  ULONG  num_services = 0x00;
  LPENUM_SERVICE_STATUS_PROCESS info = NULL;
  BOOL ok;

  Log(L"Initialized.");
  /*
    Do not guess. Without INSTALLDIR, bindir would be just "bin\", which
    would match services of other installations.
  */
  if (!GetProperty(hInstall, L"SERVICENAME", servicename) ||
      !GetProperty(hInstall, L"DATADIR", datadir) ||
      !GetProperty(hInstall, L"INSTALLDIR", installdir) ||
      installdir.empty())
  {
    Log(L"SERVICENAME, DATADIR or INSTALLDIR is not known, checking nothing");
    return ERROR_INSTALL_FAILURE;
  }
  /* INSTALLDIR ends with backslash */
  bindir= installdir + L"bin\\";

  Log(L"SERVICENAME=%s, DATADIR=%s, bindir=%s", servicename.c_str(),
      datadir.c_str(), bindir.c_str());

  scm = OpenSCManager(NULL, NULL,
    SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
  if (scm == NULL)
  {
    Log(L"OpenSCManager failed with %u", GetLastError());
    er= ERROR_INSTALL_FAILURE;
    goto LExit;
  }

  ok = EnumServicesStatusExW(  scm,
    SC_ENUM_PROCESS_INFO,
    SERVICE_WIN32,
    SERVICE_STATE_ALL,
    buf,
    bufsize,
    &bufneed,
    &num_services,
    NULL,
    NULL);
  if(!ok)
  {
    Log(L"EnumServicesStatusExW failed with %u", GetLastError());
    er= ERROR_INSTALL_FAILURE;
    goto LExit;
  }
  info = (LPENUM_SERVICE_STATUS_PROCESS)buf;
  for (ULONG i=0; i < num_services; i++)
  {
    SC_HANDLE service= OpenServiceW(scm, info[i].lpServiceName,
      SERVICE_QUERY_CONFIG);
    if (!service)
      continue;
    Log(L"Checking Service %s", info[i].lpServiceName);
    QUERY_SERVICE_CONFIGW *config=
      (QUERY_SERVICE_CONFIGW *)(void *)config_buffer;
    DWORD needed;
    BOOL ok= QueryServiceConfigW(service, config,sizeof(config_buffer),
      &needed);
    CloseServiceHandle(service);
    if (ok)
    {
       CheckServiceConfig(hInstall, servicename.c_str(), datadir.c_str(),
         bindir.c_str(), info[i].lpServiceName, config);
    }
  }

LExit:
  if(scm)
    CloseServiceHandle(scm);
  return er;
}

/*
  Get maximum size of the buffer process can allocate.
  this is calculated as min(RAM,virtualmemorylimit)
  For 32bit processes, virtual address memory is 2GB (x86 OS)
  or 4GB(x64 OS).

  Fragmentation due to loaded modules, heap  and stack
  limit maximum size of continuous memory block further,
  so that limit for 32 bit process is about 1200 on 32 bit OS
  or 2000 MB on 64 bit OS(found experimentally).
*/
unsigned long long GetMaxBufferSize(unsigned long long totalPhys)
{
#ifdef _M_IX86
  BOOL wow64;
  if (IsWow64Process(GetCurrentProcess(), &wow64))
    return min(totalPhys, 2000ULL*ONE_MB);
  else
    return min(totalPhys, 1200ULL*ONE_MB);
#else
  return totalPhys;
#endif
}


/*
  Magic undocumented number for bufferpool minimum,
  allows innodb to start also for all page sizes.
*/
static constexpr unsigned long long minBufferpoolMB= 20;

/*
  Checks SERVICENAME, PORT and BUFFERSIZE parameters
*/
extern "C" UINT  __stdcall CheckDatabaseProperties (MSIHANDLE hInstall)
{
  std::wstring ServiceName;
  std::wstring SkipNetworking;
  std::wstring QuickConfig;
  std::wstring Password;
  std::wstring EscapedPassword;
  std::wstring Port;
  std::wstring BufferPoolSize;
  wchar_t invalidValueMsg[256];
  const wchar_t *ErrorMsg=0;
  UINT er= ERROR_SUCCESS;

  Log(L"Initialized.");

  if (!GetProperty(hInstall, L"SERVICENAME", ServiceName) ||
      !GetProperty(hInstall, L"PASSWORD", Password) ||
      !GetProperty(hInstall, L"SKIPNETWORKING", SkipNetworking) ||
      !GetProperty(hInstall, L"PORT", Port) ||
      !GetProperty(hInstall, L"STDCONFIG", QuickConfig) ||
      !GetProperty(hInstall, L"BUFFERPOOLSIZE", BufferPoolSize))
  {
    er= ERROR_INSTALL_FAILURE;
    ErrorMsg= L"Internal error, can't read installation properties.";
    goto LExit;
  }

  if(!ServiceName.empty())
  {
    if(ServiceName.length() > MAX_SERVICENAME_LEN)
    {
      ErrorMsg= L"Invalid service name. The maximum length is 256 characters.";
      goto LExit;
    }
    if(ServiceName.find_first_of(L"\\/'\"") != std::wstring::npos)
    {
      ErrorMsg =
        L"Invalid service name. Forward slash and back slash are forbidden. "
        L"Single and double quotes are also not permitted.";
      goto LExit;
    }
    if(CheckServiceExists(ServiceName.c_str()))
    {
      ErrorMsg=
        L"A service with the same name already exists. "
        L"Please use a different name.";
      goto LExit;
    }
  }

  if(Password.length() > MAX_PASSWORD_LEN)
  {
    ErrorMsg= L"The password is too long. The maximum length is 8192 characters.";
    goto LExit;
  }
  EscapedPassword= EscapeCommandLine(Password);
  MsiSetPropertyW(hInstall,L"ESCAPEDPASSWORD",EscapedPassword.c_str());

  if(SkipNetworking.empty() && !Port.empty())
  {
    bool haveInvalidPort= false;

    /* Strip trailing spaces */
    while(!Port.empty() && Port.back() == L' ')
      Port.pop_back();

    if(Port.length() > MAX_PORT_LEN || Port.length() < 4)
      haveInvalidPort = true;
    else
    {
      for (wchar_t c : Port)
      {
        if(c < L'0' || c > L'9')
        {
          haveInvalidPort=true;
          break;
        }
      }
    }
    if(!haveInvalidPort && _wtoi(Port.c_str()) > 65535)
      haveInvalidPort= true;
    if (haveInvalidPort)
    {
      ErrorMsg =
        L"Invalid port number. Please use a number between 1025 and 65535.";
      goto LExit;
    }

    unsigned short port = (unsigned short)_wtoi(Port.c_str());
    if (!IsPortFree(port))
    {
      ErrorMsg =
        L"The TCP Port you selected is already in use. "
        L"Please choose a different port.";
      goto LExit;
    }
  }

  if(!QuickConfig.empty())
  {
     MEMORYSTATUSEX memstatus;
     memstatus.dwLength =sizeof(memstatus);

     if (!GlobalMemoryStatusEx(&memstatus))
     {
       Log(L"Error %u from GlobalMemoryStatusEx", GetLastError());
       er= ERROR_INSTALL_FAILURE;
       goto LExit;
     }
     /* Strip trailing spaces */
     while(!BufferPoolSize.empty() && BufferPoolSize.back() == L' ')
       BufferPoolSize.pop_back();

     unsigned long long availableMemory=
       GetMaxBufferSize(memstatus.ullTotalPhys)/ONE_MB;
     swprintf_s(invalidValueMsg,
        L"Invalid buffer pool size. Please use a number between %llu and %llu",
         minBufferpoolMB, availableMemory);
     if (BufferPoolSize.empty() || BufferPoolSize.length() > MAX_BUFFERPOOLSIZE_LEN)
     {
       ErrorMsg= invalidValueMsg;
       goto LExit;
     }

     MsiSetPropertyW(hInstall, L"BUFFERPOOLSIZE", BufferPoolSize.c_str());
     wchar_t *end;
     unsigned long long sz = wcstoull(BufferPoolSize.c_str(), &end, 10);
     if (sz > availableMemory || sz < minBufferpoolMB || *end)
     {
       if (*end == 0)
       {
         if(sz > availableMemory)
         {
           swprintf_s(invalidValueMsg,
             L"Value for buffer pool size is too large."
             L"Only approximately %llu MB is available for allocation."
             L"Please use a number between %llu and %llu.",
             availableMemory, minBufferpoolMB, availableMemory);
         }
         else if(sz < minBufferpoolMB)
         {
           swprintf_s(invalidValueMsg,
             L"Value for buffer pool size is too small."
             L"Please use a number between %llu and %llu.",
             minBufferpoolMB, availableMemory);
         }
       }
       ErrorMsg= invalidValueMsg;
       goto LExit;
     }
  }
LExit:
  MsiSetPropertyW (hInstall, L"WarningText", ErrorMsg);
  return er;
}

/*
  Sets Innodb buffer pool size (1/8 of RAM by default), if not already specified
  via command line.
  Calculates innodb log file size as min(100, innodb buffer pool size/4)
*/
extern "C" UINT __stdcall PresetDatabaseProperties(MSIHANDLE hInstall)
{
  unsigned long long InnodbBufferPoolSize= 256;
  unsigned long long InnodbLogFileSize= 100;
  std::wstring BufferPoolSizeParam;
  wchar_t buff[32];
  UINT er = ERROR_SUCCESS;
  MEMORYSTATUSEX memstatus;
  Log(L"Initialized.");

  /* Check if bufferpoolsize parameter was given on the command line*/
  if (!GetProperty(hInstall, L"BUFFERPOOLSIZE", BufferPoolSizeParam))
  {
    er= ERROR_INSTALL_FAILURE;
    goto LExit;
  }

  if (!BufferPoolSizeParam.empty())
  {
    if (BufferPoolSizeParam.length() > MAX_BUFFERPOOLSIZE_LEN)
    {
      Log(L"BUFFERPOOLSIZE is too long, the maximum is %u characters",
          (unsigned)MAX_BUFFERPOOLSIZE_LEN);
      er= ERROR_INSTALL_FAILURE;
      goto LExit;
    }
    Log(L"BUFFERPOOLSIZE=%s, len=%u", BufferPoolSizeParam.c_str(),
        (unsigned)BufferPoolSizeParam.length());
    InnodbBufferPoolSize= _wtoi64(BufferPoolSizeParam.c_str());
  }
  else
  {
    memstatus.dwLength = sizeof(memstatus);
    if (!GlobalMemoryStatusEx(&memstatus))
    {
       Log(L"Error %u from GlobalMemoryStatusEx",GetLastError());
       er= ERROR_INSTALL_FAILURE;
       goto LExit;
    }
    unsigned long long totalPhys= memstatus.ullTotalPhys;
    /* Give innodb 12.5% of available physical memory. */
    InnodbBufferPoolSize= totalPhys/ONE_MB/8;
 #ifdef _M_IX86
    /*
      For 32 bit processes, take virtual address space limitation into account.
      Do not try to use more than 3/4 of virtual address space, even if there
      is plenty of physical memory.
    */
    InnodbBufferPoolSize= min(GetMaxBufferSize(totalPhys)/ONE_MB*3/4,
      InnodbBufferPoolSize);
 #endif
    swprintf_s(buff, L"%llu",InnodbBufferPoolSize);
    MsiSetPropertyW(hInstall, L"BUFFERPOOLSIZE", buff);
  }
  InnodbLogFileSize = min(100, 2 * InnodbBufferPoolSize);
  swprintf_s(buff, L"%llu",InnodbLogFileSize);
  MsiSetPropertyW(hInstall, L"LOGFILESIZE", buff);

LExit:
  return er;
}

static bool FindErrorLog(const std::wstring &dir, std::wstring &ErrorLogFile)
{
  WIN32_FIND_DATAW FindFileData;
  HANDLE hFind= FindFirstFileW((dir + L"\\*.err").c_str(), &FindFileData);
  if (hFind == INVALID_HANDLE_VALUE)
    return false;
  ErrorLogFile= dir + L"\\" + FindFileData.cFileName;
  FindClose(hFind);
  return true;
}

static void DumpErrorLog(MSIHANDLE hInstall, const std::wstring &dir)
{
  std::wstring filepath;
  if (!FindErrorLog(dir, filepath))
    return;
  FILE *f= _wfopen(filepath.c_str(), L"r");
  if (!f)
    return;
  char buf[2048];
  Log(L"=== dumping error log %s === ",filepath.c_str());
  while (fgets(buf, sizeof(buf), f))
  {
     /* Strip off EOL chars. */
     size_t len = strlen(buf);
     if (len > 0 && buf[len-1] == '\n')
       buf[--len]= 0;
     if (len > 0 && buf[len-1] == '\r')
       buf[--len]= 0;
     Log(L"%S",buf);
  }
  fclose(f);
  Log(L"=== end of error log ===");
}

/* Remove service and data directory created by CreateDatabase operation */
extern "C" UINT __stdcall CreateDatabaseRollback(MSIHANDLE hInstall)
{
  std::wstring data;
  std::wstring service;
  std::wstring dir;

  Log(L"Initialized.");

  if (!GetProperty(hInstall, L"CustomActionData", data))
    return ERROR_INSTALL_FAILURE;

  /* Property is encoded as [SERVICENAME]\[DBLOCATION] */
  size_t pos= data.find(L'\\');
  if (pos == std::wstring::npos)
  {
    service= data;
  }
  else
  {
    service= data.substr(0, pos);
    dir= data.substr(pos + 1);
  }

  if(!service.empty())
  {
    ExecRemoveService(service.c_str());
  }
  if(!dir.empty())
  {
    DumpErrorLog(hInstall,dir);
    ExecRemoveDataDirectory(dir);
  }
  return ERROR_SUCCESS;
}


/*
  Enables/disables optional "Launch upgrade wizard" checkbox at the end of
  installation
*/
extern "C" UINT __stdcall CheckServiceUpgrades(MSIHANDLE hInstall)
{
  UINT er = ERROR_SUCCESS;
  std::wstring installerVersion;
  std::wstring installDirW;
  std::string installDir;
  int installerMajorVersion, installerMinorVersion, installerPatchVersion;
  bool upgradableServiceFound=false;
  LPENUM_SERVICE_STATUS_PROCESSW info;
  DWORD bufsize;
  int index;
  BOOL ok;
  SC_HANDLE scm = NULL;

  Log(L"Initialized.");
  if (!GetProperty(hInstall, L"ProductVersion", installerVersion) ||
      !GetProperty(hInstall, L"INSTALLDIR", installDirW))
  {
    er= ERROR_INSTALL_FAILURE;
    goto LExit;
  }
  if (swscanf(installerVersion.c_str(),L"%d.%d.%d",
    &installerMajorVersion, &installerMinorVersion, &installerPatchVersion) !=3)
  {
    assert(FALSE);
  }
  installDir= ToAnsi(installDirW);


  scm = OpenSCManager(NULL, NULL,
    SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
  if (scm == NULL)
  {
    Log(L"OpenSCManager failed with %u", GetLastError());
    er= ERROR_INSTALL_FAILURE;
    goto LExit;
  }

  static BYTE buf[64*1024];
  static BYTE config_buffer[8*1024];

  bufsize= sizeof(buf);
  DWORD bufneed;
  DWORD num_services;
  ok= EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,  SERVICE_WIN32,
    SERVICE_STATE_ALL,  buf, bufsize,  &bufneed, &num_services, NULL, NULL);
  if(!ok)
  {
    Log(L"EnumServicesStatusEx failed with %u", GetLastError());
    er= ERROR_INSTALL_FAILURE;
    goto LExit;
  }
  info =
    (LPENUM_SERVICE_STATUS_PROCESSW)buf;
  index=-1;
  for (ULONG i=0; i < num_services; i++)
  {
    SC_HANDLE service= OpenServiceW(scm, info[i].lpServiceName,
      SERVICE_QUERY_CONFIG);
    if (!service)
      continue;
    QUERY_SERVICE_CONFIGW *config=
      (QUERY_SERVICE_CONFIGW*)(void *)config_buffer;
    DWORD needed;
    ok= QueryServiceConfigW(service, config,sizeof(config_buffer),
      &needed) && (config->dwStartType != SERVICE_DISABLED);
    CloseServiceHandle(service);
    if (ok)
    {
       mysqld_service_properties props;
       if (get_mysql_service_properties(config->lpBinaryPathName, &props))
                  continue;
        /*
          Only look for services that have mysqld.exe outside of the current
          installation directory.
        */
       if(installDir.empty() || strstr(props.mysqld_exe, installDir.c_str()) == 0)
        {
           Log(L"found service %s, major=%d, minor=%d",
            info[i].lpServiceName, props.version_major, props.version_minor);
          if(props.version_major < installerMajorVersion
            || (props.version_major == installerMajorVersion &&
                props.version_minor <= installerMinorVersion))
          {
            upgradableServiceFound= true;
            break;
          }
       }
    }
  }

  if(!upgradableServiceFound)
  {
    /* Disable optional checkbox at the end of installation */
    MsiSetPropertyW(hInstall, L"WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT", L"");
    MsiSetPropertyW(hInstall, L"WIXUI_EXITDIALOGOPTIONALCHECKBOX",L"");
  }
  else
  {
    MsiSetPropertyW(hInstall, L"UpgradableServiceFound", L"1");
    MsiSetPropertyW(hInstall, L"WIXUI_EXITDIALOGOPTIONALCHECKBOX",L"1");
  }
LExit:
  if(scm)
    CloseServiceHandle(scm);
  return er;
}

/* Fail on purpose, to test rollback. Scheduled with TESTFAIL=1 in msiexec */
extern "C" UINT __stdcall FakeFailure(MSIHANDLE hInstall)
{
  Log(L"Failing on purpose");
  return ERROR_INSTALL_FAILURE;
}
