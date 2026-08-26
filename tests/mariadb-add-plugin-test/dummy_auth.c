/* Dummy server-side authentication plugin, built out-of-tree against an
   installed MariaDB-devel/SDK via mariadb-plugin-config.cmake +
   MARIADB_ADD_PLUGIN(), to exercise that path end to end (MDEV-40608).
   It does not implement a real authentication protocol. */

#include <mysql/plugin_auth.h>

static int dummy_authenticate(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  unsigned char *pkt;
  int pkt_len= vio->read_packet(vio, &pkt);
  if (pkt_len < 0)
    return CR_ERROR;
  info->password_used= PASSWORD_USED_NO;
  return CR_OK;
}

static struct st_mysql_auth dummy_auth_handler=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  "dummy_client_auth",
  dummy_authenticate,
  NULL,
  NULL
};

maria_declare_plugin(dummy_auth)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &dummy_auth_handler,
  "dummy_auth",
  "MariaDB plc",
  "Dummy authentication plugin exercising MARIADB_ADD_PLUGIN (MDEV-40608)",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
