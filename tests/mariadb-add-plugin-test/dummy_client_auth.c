/* Dummy client-side authentication plugin, built out-of-tree via
   MARIADB_ADD_PLUGIN(... CLIENT ...) to check that keyword still works for
   external plugins (MDEV-40608). It does not implement a real
   authentication protocol, and (unlike a server plugin) can't be
   exercised via mariadbd --plugin-load-add - client plugins load only
   during an actual connection handshake. */

#include <mysql/client_plugin.h>
#include <mysql/plugin_auth.h>

static int dummy_client_authenticate(MYSQL_PLUGIN_VIO *vio, struct st_mysql *mysql)
{
  return CR_ERROR;
}

mysql_declare_client_plugin(AUTHENTICATION)
  "dummy_client_auth",
  "MariaDB plc",
  "Dummy client authentication plugin exercising MARIADB_ADD_PLUGIN CLIENT (MDEV-40608)",
  {0, 1, 0},
  "GPL",
  NULL,
  NULL,
  NULL,
  NULL,
  dummy_client_authenticate,
  NULL
mysql_end_client_plugin;
