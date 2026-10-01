/*
  MDEV-11111: smoke test for the embedded server.
  Usage: launcher_test [server options...]
  Uses only the ordinary client API: mysql_server_init() starts the server,
  a connection to "localhost" reaches it, mysql_server_end() stops it.
*/
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#define pause_1s() Sleep(1000)
#else
#include <unistd.h>
#define pause_1s() sleep(1)
#endif
#include <mysql.h>
#include "embedded_launcher.h"

int main(int argc, char **argv)
{
  MYSQL *m;
  MYSQL_RES *res;
  MYSQL_ROW row;
  int rc= 1;

  mariadb_embedded_register(); /* not needed with a shared libmariadbd */
  if (mysql_server_init(argc, argv, NULL))
    return 1;

  m= mysql_init(NULL);
  if (!mysql_real_connect(m, "localhost", "root", NULL, NULL, 0, NULL, 0))
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
  if (getenv("LAUNCHER_TEST_HANG")) /* to kill us and watch the server */
    for (;;)
      pause_1s();
  mysql_close(m);
  mysql_server_end();
  puts(rc ? "FAIL" : "OK");
  return rc;
}
