/* Copyright (c) 2026, MariaDB plc

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
  MDEV-11111: connect the launcher to libmariadb.

  After mariadb_embedded_register(), the ordinary client API is enough:
  mysql_server_init(argc, argv, groups) starts the server, mysql_real_connect()
  to the local host (no host, "localhost", "." on Windows) reaches it, and
  mysql_server_end() stops it.

  A shared libmariadbd is built with MARIADB_EMBEDDED_AUTOREGISTER, so
  merely loading it registers the hooks; a static build calls
  mariadb_embedded_register() explicitly.
*/

#include <stdio.h>
#include <mysql.h>
#include "embedded_launcher.h"

static int init_hook(int argc, char **argv, char **groups)
{
  if (mariadb_embedded_start(argc, argv, groups))
  {
    fprintf(stderr, "mysql_server_init: %s\n", mariadb_embedded_error());
    return 1;
  }
  return 0;
}

static void end_hook(void)
{
  mariadb_embedded_stop();
}

void mariadb_embedded_register(void)
{
  mariadb_set_embedded_hooks(init_hook, end_hook, mariadb_embedded_socket);
}

#if defined(MARIADB_EMBEDDED_AUTOREGISTER) && defined(__GNUC__)
__attribute__((constructor)) static void autoregister(void)
{
  mariadb_embedded_register();
}
#endif

#if defined(MARIADB_EMBEDDED_AUTOREGISTER) && defined(_WIN32)
#include <windows.h>
/* Loading libmariadbd.dll is enough; it only stores pointers, which is fine
   under the loader lock */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
  (void) h;
  (void) reserved;
  if (reason == DLL_PROCESS_ATTACH)
    mariadb_embedded_register();
  return TRUE;
}
#endif
