/*
  MDEV-11111: smoke test for the embedded launcher.
  Usage: launcher_test [server options...]
  Starts the server, runs a query through the normal client library over the
  private socket, stops the server.
*/
#include <stdio.h>
#include <stdlib.h>
#include <mysql.h>
#include "embedded_launcher.h"
#ifdef _WIN32
#include <windows.h>
#define Sleep_or_pause() Sleep(1000)
#else
#include <unistd.h>
#define Sleep_or_pause() sleep(1)
#endif

int main(int argc, char **argv)
{
  MYSQL *m;
  MYSQL_RES *res;
  MYSQL_ROW row;
  int rc= 1;

  if (mariadb_embedded_start(argc, argv, NULL))
  {
    fprintf(stderr, "start failed: %s\n", mariadb_embedded_error());
    return 1;
  }
  printf("socket: %s\n", mariadb_embedded_socket());

  mysql_library_init(0, NULL, NULL); /* reads MARIADB_UNIX_PORT */
  m= mysql_init(NULL);
#ifdef _WIN32
  /* libmariadb ignores MARIADB_UNIX_PORT for named pipes, pass it explicitly */
  const char *host= ".";
#else
  const char *host= "localhost"; /* default socket: $MARIADB_UNIX_PORT */
#endif
  if (!mysql_real_connect(m, host, "root", NULL, NULL, 0,
                          mariadb_embedded_socket(), 0))
    fprintf(stderr, "connect failed: %s\n", mysql_error(m));
  else if (mysql_query(m, "SELECT @@version, 1+1") ||
           !(res= mysql_store_result(m)))
    fprintf(stderr, "query failed: %s\n", mysql_error(m));
  else
  {
    row= mysql_fetch_row(res);
    printf("version %s, 1+1=%s\n", row[0], row[1]);
    mysql_free_result(res);
    rc= 0;
  }
  if (getenv("LAUNCHER_TEST_HANG")) /* to kill us and watch the server */
    for (;;)
      Sleep_or_pause();
  mysql_close(m);
  mysql_library_end();
  mariadb_embedded_stop();
  puts(rc ? "FAIL" : "OK");
  return rc;
}
