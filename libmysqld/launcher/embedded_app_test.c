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
  MDEV-11111: an embedded server application, written as it always was:
  nothing but the client API, linked with libmariadbd instead of libmariadb.
  Usage: embedded_app_test [server options...]
*/
#include <stdio.h>
#include <mysql.h>

int main(int argc, char **argv)
{
  MYSQL *m;
  MYSQL_RES *res;
  MYSQL_ROW row;
  int rc= 1;

  if (mysql_server_init(argc, argv, NULL))
    return 1;
  m= mysql_init(NULL);
  if (!mysql_real_connect(m, NULL, "root", NULL, NULL, 0, NULL, 0))
    fprintf(stderr, "connect failed: %s\n", mysql_error(m));
  else if (mysql_query(m, "SELECT @@version, CURRENT_USER()") ||
           !(res= mysql_store_result(m)))
    fprintf(stderr, "query failed: %s\n", mysql_error(m));
  else
  {
    row= mysql_fetch_row(res);
    printf("version %s, current_user %s\n", row[0], row[1]);
    mysql_free_result(res);
    rc= 0;
  }
  mysql_close(m);
  mysql_server_end();
  puts(rc ? "FAIL" : "OK");
  return rc;
}
