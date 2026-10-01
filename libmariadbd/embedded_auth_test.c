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
  MDEV-11111: authentication in the embedded server. The password is not
  checked (the client is the process that started the server), but the
  account of the user name decides the privileges; an unknown name is root.
  Needs a data directory with grant tables.
  Usage: embedded_auth_test [server options...]
*/
#include <stdio.h>
#include <string.h>
#include <mysql.h>

static int failures;

static void check(int ok, const char *what)
{
  printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    failures++;
}

static MYSQL *connect_as(const char *user, const char *pass)
{
  MYSQL *m= mysql_init(NULL);
  if (!mysql_real_connect(m, NULL, user, pass, NULL, 0, NULL, 0))
  {
    fprintf(stderr, "  connect as %s: %s\n", user, mysql_error(m));
    mysql_close(m);
    return NULL;
  }
  return m;
}

static int one_string(MYSQL *m, const char *query, char *out, size_t size)
{
  MYSQL_RES *res;
  MYSQL_ROW row;
  if (mysql_query(m, query) || !(res= mysql_store_result(m)))
    return 1;
  row= mysql_fetch_row(res);
  snprintf(out, size, "%s", row && row[0] ? row[0] : "");
  mysql_free_result(res);
  return 0;
}

int main(int argc, char **argv)
{
  MYSQL *m;
  char buf[128];

  if (mysql_server_init(argc, argv, NULL))
    return 1;

  m= connect_as("root", NULL);
  check(m != NULL, "root connects without password");
  if (!m)
    return 1;
  check(!mysql_query(m, "CREATE USER IF NOT EXISTS lim@localhost "
                        "IDENTIFIED BY 'secret'") &&
        !mysql_query(m, "CREATE DATABASE IF NOT EXISTS emb_auth") &&
        !mysql_query(m, "CREATE TABLE IF NOT EXISTS emb_auth.t (a INT)") &&
        !mysql_query(m, "GRANT SELECT ON emb_auth.* TO lim@localhost"),
        "GRANT and CREATE USER work (grant tables are in use)");
  mysql_close(m);

  m= connect_as("lim", "wrong password");
  check(m != NULL, "existing account connects with a wrong password");
  if (m)
  {
    check(!one_string(m, "SELECT CURRENT_USER()", buf, sizeof(buf)) &&
          !strcmp(buf, "lim@localhost"), "the session is lim@localhost");
    check(!mysql_query(m, "SELECT * FROM emb_auth.t") &&
          (mysql_free_result(mysql_store_result(m)), 1),
          "lim may SELECT what was granted");
    check(mysql_query(m, "CREATE TABLE emb_auth.t2 (a INT)") &&
          mysql_errno(m) == 1142 /* ER_TABLEACCESS_DENIED_ERROR */,
          "lim may not CREATE, privileges are enforced");

    check(!mysql_change_user(m, "root", "whatever", NULL) &&
          !one_string(m, "SELECT CURRENT_USER()", buf, sizeof(buf)) &&
          !strcmp(buf, "root@localhost"), "COM_CHANGE_USER to root");
    mysql_close(m);
  }

  m= connect_as("no_such_user", "x");
  check(m != NULL, "an unknown name connects");
  if (m)
  {
    check(!one_string(m, "SELECT CURRENT_USER()", buf, sizeof(buf)) &&
          !strcmp(buf, "root@localhost"), "the unknown name is root");
    mysql_query(m, "DROP USER lim@localhost");
    mysql_query(m, "DROP DATABASE emb_auth");
    mysql_close(m);
  }

  mysql_server_end();
  puts(failures ? "FAIL" : "OK");
  return failures != 0;
}
