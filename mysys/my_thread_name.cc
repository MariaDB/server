/* Copyright 2024, MariaDB.

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

#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>
#include "mysql/psi/psi.h"
#include <stdio.h>

#ifdef _WIN32
#define MAX_THREAD_NAME 256
typedef HRESULT (*func_SetThreadDescription)(HANDLE,PCWSTR);
#elif defined(__linux__)
#define MAX_THREAD_NAME 16
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#endif

#if defined(HAVE_PSI_THREAD_INTERFACE) && !defined DBUG_OFF
/**
  Check that the name is consistent with PSI.
  Require that the name matches the last part of PSI's class name
  (e.g. "thread/sql/main" -> "main").

  We drop the namespace prefix, because these thread names are
  truncated to 15 characters on Linux, and something like "innodb/" would
  already take up about half of that.
*/
static void dbug_verify_thread_name(const char *name)
{
  const char *psi_name= NULL;
  const char *thread_class_name= PSI_THREAD_CALL(get_thread_class_name)();
  if (thread_class_name)
  {
    /* Remove the path prefix */
    const char *last_part= strrchr(thread_class_name, '/');
    if (last_part)
      psi_name= last_part + 1;
    else
      psi_name= thread_class_name;
  }
  if (psi_name && strcmp(psi_name, name))
  {
    fprintf(stderr, "ERROR: my_thread_set_name() mismatch: PSI name '%s' != '%s'\n",
            psi_name, name);
    abort();
  }
  size_t len= strlen(name);
  if (len > 15)
  {
    /* Linux can' handle "long" (> 15 chars) names */
    fprintf(stderr, "ERROR: my_thread_set_name() name too long: '%s'\n", name);
    abort();
  }
}
#else
#define dbug_verify_thread_name(name) do {} while (0)
#endif

/* Set current thread name for debugger/profiler and similar tools. */
extern "C" void my_thread_set_name(const char *name)
{
  dbug_verify_thread_name(name);

#ifdef _WIN32
  /*
    SetThreadDescription might not be there on older Windows versions.
    Load it dynamically.
  */
  static func_SetThreadDescription my_SetThreadDescription=
      (func_SetThreadDescription) GetProcAddress(GetModuleHandle("kernel32"),
                                                 "SetThreadDescription");
  if (!my_SetThreadDescription)
    return;
  wchar_t wname[MAX_THREAD_NAME];
  wname[0]= 0;
  MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, MAX_THREAD_NAME);
  my_SetThreadDescription(GetCurrentThread(), wname);
#elif defined __linux__
  char shortname[MAX_THREAD_NAME];
  snprintf(shortname, MAX_THREAD_NAME, "%s", name);
  pthread_setname_np(pthread_self(), shortname);
#elif defined __NetBSD__
  pthread_setname_np(pthread_self(), "%s", (void *) name);
#elif defined __FreeBSD__ || defined __OpenBSD__
  pthread_set_name_np(pthread_self(), name);
#elif defined __APPLE__
  pthread_setname_np(name);
#else
  (void) name;
#endif
}
