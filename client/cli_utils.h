#pragma once

#include <mysql.h>
#ifdef __cplusplus
extern "C" {
#endif
MYSQL *cli_connect(MYSQL *mysql, const char *host, const char *user, char **ppasswd,
                  const char *db, unsigned int port, const char *unix_socket, unsigned long client_flag,
                     my_bool tty_password, my_bool allow_credmgr);
#ifdef __cplusplus
}
#endif
