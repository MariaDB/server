/* Copyright (C) 2020 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
*/

/*
  main() function for the server on Windows is implemented here.
  The core functionality is implemented elsewhere, in mysqld_main(), and running as
  service is done here.

  Main tasks of the service are

  1. Report current status back to service control manager. Here we're
  providing callbacks so code outside of winmain.cc can call it
  (via mysqld_set_service_status_callback())

  2. React to notification, the only one we care about is the "stop"
  notification. we initiate shutdown, when instructed.

  Note that our service might not be too Windows-friendly, as it might take
  a while to startup (recovery), and a while to shut down(innodb cleanups).

  Most of the code more of less standard service stuff, taken from Microsoft
  docs examples.

  Notable oddity in running services, is that we do not know for sure,
  whether we should run as a service or not (there is no --service parameter that
  would tell).Heuristics are used, and if the last command line argument is
  valid service name, we try to run as service, but fallback to usual process
  if this fails.

  As an example, even if mysqld.exe is started  with command line like "mysqld.exe --help",
  it is entirely possible that mysqld.exe run as service "--help".

  Apart from that, now deprecated and obsolete service registration/removal functionality is
  still provided (mysqld.exe --install/--remove)
*/

#include <my_global.h>
#include <mysqld.h>
#include <log.h>

#include <stdio.h>
#include <windows.h>
#include <string>
#include <cassert>
#include <winservice.h>

static SERVICE_STATUS svc_status{SERVICE_WIN32_OWN_PROCESS};
static SERVICE_STATUS_HANDLE svc_status_handle;
static char *svc_name;

static char **save_argv;
static int save_argc;

static int install_service(int argc, char **argv, const char *name);
static int remove_service(const char *name);

/*
  Report service status to SCM. This function is indirectly invoked
  by the server to report state transitions.

  1. from START_PENDING to SERVICE_RUNNING, when we start accepting user connections
  2. from SERVICE_RUNNING to STOP_PENDING, when we start shutdown
  3. from STOP_PENDING to SERVICE_STOPPED, in mysqld_exit()
     sometimes also START_PENDING to SERVICE_STOPPED, on startup errors
*/
static void report_svc_status(DWORD current_state, DWORD exit_code, DWORD wait_hint)
{
  if (!svc_status_handle)
    return;

  static DWORD check_point= 1;
  if (current_state != (DWORD)-1)
    svc_status.dwCurrentState= current_state;
  svc_status.dwWaitHint= wait_hint;

  if (exit_code)
  {
    svc_status.dwWin32ExitCode= ERROR_SERVICE_SPECIFIC_ERROR;
    svc_status.dwServiceSpecificExitCode= exit_code;
  }
  else
  {
    svc_status.dwWin32ExitCode= 0;
  }

  if (current_state == SERVICE_START_PENDING)
    svc_status.dwControlsAccepted= 0;
  else
    svc_status.dwControlsAccepted= SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN;

  if ((current_state == SERVICE_RUNNING) || (current_state == SERVICE_STOPPED))
    svc_status.dwCheckPoint= 0;
  else
    svc_status.dwCheckPoint= check_point++;

  SetServiceStatus(svc_status_handle, &svc_status);
}

/* Report unexpected errors. */
static void svc_report_event(const char *svc_name, const char *command)
{
  char buffer[80];
  sprintf_s(buffer, "mariadb service %s, %s failed with %d",
      svc_name, command, GetLastError());
  OutputDebugString(buffer);
}

/*
  Service control function.
  Reacts to service stop, initiates shutdown.
*/
static void WINAPI svc_ctrl_handle(DWORD cntrl)
{
  switch (cntrl)
  {
  case SERVICE_CONTROL_SHUTDOWN:
  case SERVICE_CONTROL_STOP:
    sql_print_information(
      "Windows service \"%s\":  received %s",
      svc_name,
      cntrl == SERVICE_CONTROL_STOP? "SERVICE_CONTROL_STOP": "SERVICE_CONTROL_SHUTDOWN");

    /* The below will also set the status to STOP_PENDING. */
    mysqld_win_initiate_shutdown();
    break;

  case SERVICE_CONTROL_INTERROGATE:
  default:
    break;
  }
}

/* Service main routine, mainly runs mysqld_main() */
static void WINAPI svc_main(DWORD svc_argc, char **svc_argv)
{
  /* Register the handler function for the service */
  char *name= svc_argv[0];

  svc_status_handle= RegisterServiceCtrlHandler(name, svc_ctrl_handle);
  if (!svc_status_handle)
  {
    svc_report_event(name, "RegisterServiceCtrlHandler");
    return;
  }
  report_svc_status(SERVICE_START_PENDING, NO_ERROR, 0);

  /* Make server report service status via our callback.*/
  mysqld_set_service_status_callback(report_svc_status);

  /* This would add service name entry to load_defaults.*/
  mysqld_win_set_service_name(name);

  /*
   Do not pass the service name parameter (last on the command line)
   to mysqld_main(), it is unaware of it.
  */
  save_argv[save_argc - 1]= 0;
  mysqld_main(save_argc - 1, save_argv);
}

/*
  This start the service. Sometimes it will fail, because
  currently we do not know for sure whether we run as service or not.
  If this fails, the fallback is to run as normal process.
*/
static int run_as_service(char *name)
{
  SERVICE_TABLE_ENTRY stb[]= {{name, svc_main}, {0, 0}};
  if (!StartServiceCtrlDispatcher(stb))
  {
    assert(GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
    return -1;
  }
  return 0;
}

/*
  Check for valid existing service name.
  Part of our guesswork, whether we run as service or not.
*/
static bool is_existing_service(const char *name)
{
  if (strchr(name, '\\') || strchr(name, '/'))
  {
    /* Invalid characters in service name */
    return false;
  }

  SC_HANDLE sc_service= 0, scm= 0;
  bool ret= ((scm= OpenSCManager(0, 0, SC_MANAGER_ENUMERATE_SERVICE)) != 0) &&
       ((sc_service= OpenService(scm, name, SERVICE_QUERY_STATUS)) != 0);

  if (sc_service)
    CloseServiceHandle(sc_service);
  if (scm)
    CloseServiceHandle(scm);

  return ret;
}

/*
  If service name is not given to --install/--remove
  it is assumed to be "MySQL" (traditional handling)
*/
static const char *get_svc_name(const char *arg)
{
  return arg ? arg : "MySQL";
}

/*
  Main function on Windows.
  Runs mysqld as normal process, or as a service.

  Plus, the obsolete functionality to register/remove services.
*/
__declspec(dllexport) int mysqld_win_main(int argc, char **argv)
{
  save_argv= argv;
  save_argc= argc;

   /*
     If no special arguments are given, service name is nor present
     run as normal program.
   */
  if (argc == 1)
    return mysqld_main(argc, argv);

  auto cmd= argv[1];

  /* Handle install/remove */
  if (!strcmp(cmd, "--install") || !strcmp(cmd, "--install-manual"))
    return install_service(argc, argv, get_svc_name(argv[2]));

  if (!strcmp(cmd, "--remove"))
    return remove_service(get_svc_name(argv[2]));

  /* Try to run as service, and fallback to mysqld_main(), if this fails */
  svc_name= argv[argc - 1];
  if (is_existing_service(svc_name) && !run_as_service(svc_name))
    return 0;
  svc_name= 0;

  /* Run as normal program.*/
  return mysqld_main(argc, argv);
}


/*
  Register/remove services functionality.
  This is kept for backward compatibility only, and is
  superseeded by much more versatile mysql_install_db.exe

  "mysqld --remove=svc" has no advantage over
  OS own "sc delete svc"
*/
static void ATTRIBUTE_NORETURN die(const char *func, const char *name)
{
  DWORD err= GetLastError();
  fprintf(stderr, "FATAL ERROR : %s failed (%lu)\n", func, err);
  switch (err)
  {
  case ERROR_SERVICE_EXISTS:
    fprintf(stderr, "Service %s already exists.\n", name);
    break;
  case ERROR_SERVICE_DOES_NOT_EXIST:
    fprintf(stderr, "Service %s does not exist.\n", name);
    break;
  case ERROR_ACCESS_DENIED:
    fprintf(stderr, "Access is denied. "
        "Make sure to run as elevated admin user.\n");
    break;
  case ERROR_INVALID_NAME:
    fprintf(stderr, "Invalid service name '%s'\n", name);
  default:
    break;
  }
  exit(1);
}

static inline std::string quoted(const char *src)
{
  std::string s;
  s.append("\"").append(src).append("\"");
  return s;
}

static int install_service(int argc, char **argv, const char *name)
{
  std::string cmdline;

  char path[MAX_PATH];
  auto nSize = GetModuleFileName(0, path, sizeof(path));

  if (nSize == (DWORD) sizeof(path) && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    die("GetModuleName", name);

  cmdline.append(quoted(path));

  const char *user= 0;
  // mysqld --install[-manual] name ...[--local-service]
  if (argc > 2)
  {
    for (int i= 3; argv[i]; i++)
    {
      if (!strcmp(argv[i], "--local-service"))
        user= "NT AUTHORITY\\LocalService";
      else
      {
        cmdline.append(" ").append(quoted(argv[i]));
      }
    }
  }
  cmdline.append(" ").append(quoted(name));

  DWORD start_type;
  if (!strcmp(argv[1], "--install-manual"))
    start_type= SERVICE_DEMAND_START;
  else
    start_type= SERVICE_AUTO_START;

  SC_HANDLE scm, sc_service;
  if (!(scm= OpenSCManager(0, 0, SC_MANAGER_CREATE_SERVICE)))
    die("OpenSCManager", name);

  if (!(sc_service= CreateService(
      scm, name, name, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, start_type, SERVICE_ERROR_NORMAL,
      cmdline.c_str(), 0, 0, 0, user, 0)))
    die("CreateService", name);

  char description[]= "MariaDB database server";
  SERVICE_DESCRIPTION sd= {description};
  ChangeServiceConfig2(sc_service, SERVICE_CONFIG_DESCRIPTION, &sd);

  CloseServiceHandle(sc_service);
  CloseServiceHandle(scm);

  printf("Service '%s' successfully installed.\n", name);
  return 0;
}

static int remove_service(const char *name)
{
  SC_HANDLE scm, sc_service;

  if (!(scm= OpenSCManager(0, 0, SC_MANAGER_CREATE_SERVICE)))
    die("OpenSCManager", name);

  if (!(sc_service= OpenService(scm, name, DELETE)))
    die("OpenService", name);

  if (!DeleteService(sc_service))
    die("DeleteService", name);

  CloseServiceHandle(sc_service);
  CloseServiceHandle(scm);

  printf("Service '%s' successfully deleted.\n", name);
  return 0;
}
