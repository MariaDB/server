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
  MDEV-11111: embedded server launcher.

  Instead of linking the server into the application, spawn a private
  mariadbd process, reachable only via a Unix domain socket (named pipe on
  Windows). The application then talks to it with the ordinary client library.

  The server is a separate process, so its exit() calls (unireg_abort etc)
  cannot take the application down.
*/

#ifndef MARIADB_EMBEDDED_LAUNCHER_H
#define MARIADB_EMBEDDED_LAUNCHER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
  Start the server and wait until it accepts connections.

  argv[0] is ignored (mysql_server_init convention), argv[1..argc-1] are
  passed to the server as-is. groups is a NULL-terminated list of the option
  groups that the server reads from option files, as for the embedded library
  that this replaces; NULL means "server" and "embedded". The groups of a
  server (such as [mysqld]) are not read, unless they are in the list.

  The server executable is taken from $MARIADB_EMBEDDED_SERVER, else
  "mariadbd" is looked up in PATH. $MARIADB_EMBEDDED_TIMEOUT (seconds,
  default 60) limits the wait for startup.

  On success sets $MARIADB_UNIX_PORT, which the client library uses as the
  default socket (pipe name on Windows), and returns 0.
  On failure returns non-zero, see mariadb_embedded_error().
*/
int mariadb_embedded_start(int argc, char **argv, char **groups);

/* Shutdown the server gracefully; kill it if it does not exit in time */
void mariadb_embedded_stop(void);

/* Socket path (pipe name on Windows), or NULL if the server is not running */
const char *mariadb_embedded_socket(void);

/* Last error message */
const char *mariadb_embedded_error(void);

/* Register the launcher with libmariadb (embedded_glue.c), so that
   mysql_server_init()/mysql_server_end() start and stop the server */
void mariadb_embedded_register(void);

#ifdef __cplusplus
}
#endif

#endif /* MARIADB_EMBEDDED_LAUNCHER_H */
