#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>

int get_evar(char **hostname, char **port, char** username, char ** password)
{

  if (!((*hostname)= getenv("MYSQL_TEST_HOST")))
    (*hostname)= (char *)"127.0.0.1";

  if (!((*port)= getenv("MASTER_MYPORT")))
  {
    if (!((*port)= getenv("MYSQL_TEST_PORT")))
      return 1;
  }

  if (!((*username)= getenv("MYSQL_TEST_USER")))
    (*username)= (char *)"root";

  if (!((*password)= getenv("MYSQL_TEST_PASSWD")))
    (*password)= (char *)"";

  return 0;
}

int main(int argc, char *argv[])
{
  MYSQL *mysql;
  char *host;
  char *user;
  char *passwd;
  char *porta;
  unsigned int port;
  int ret;
  my_bool my_false= 0;

  if (get_evar(&host, &porta, &user, &passwd))
  {
    printf("set environment variable MASTER_MYPORT\n");
    return 1;
  }

  port = atoi(porta);

  mysql_thread_init();

  if (mysql_server_init(-1, NULL, NULL) != 0) {
    printf("mysql_library_init failed");
    return 1;
  }


  mysql = mysql_init(NULL);

  if (!mysql) {
    printf("mysql_init failed");
    return 1;
  }

  if (mysql_options(mysql, MYSQL_OPT_USE_REMOTE_CONNECTION, NULL) != 0) {
    printf("mysql_options MYSQL_OPT_USE_REMOTE_CONNECTION failed: %s\n", mysql_error(mysql));
    return 1;
  }

  mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &my_false);

  if (mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
    printf("mysql_options MYSQL_SET_CHARSET_NAME utf8mb4 failed: %s\n", mysql_error(mysql));
    return 1;
  }

  if ((ret= !mysql_real_connect(mysql, host, user, passwd, NULL, port, NULL,
        CLIENT_FOUND_ROWS | CLIENT_MULTI_RESULTS | CLIENT_REMEMBER_OPTIONS)))
    printf("mysql_real_connect failed: %s\n", mysql_error(mysql));
  mysql_close(mysql);
  mysql_thread_end();
  mysql_library_end();

  return ret;
}
