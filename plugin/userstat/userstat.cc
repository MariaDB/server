#include <my_config.h>
#include <mysql/plugin.h>
#include <mysql_version.h>
#include "table.h"
#include "sql_connect.h"
#include "field.h"
#include "sql_const.h"
#include "sql_acl.h"

bool schema_table_store_record(THD *thd, TABLE *table);

#include "client_stats.cc"
#include "index_stats.cc"
#include "table_stats.cc"
#include "user_stats.cc"

static struct st_mysql_information_schema userstat_info=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

maria_declare_plugin(userstat)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &userstat_info,
  "CLIENT_STATISTICS",
  "Percona and Sergei Golubchik",
  "Client Statistics",
  PLUGIN_LICENSE_GPL,
  client_stats_init,
  0,
  0x0200,
  NULL,
  NULL,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &userstat_info,
  "INDEX_STATISTICS",
  "Percona and Sergei Golubchik",
  "Index Statistics",
  PLUGIN_LICENSE_GPL,
  index_stats_init,
  0,
  0x0200,
  NULL,
  NULL,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &userstat_info,
  "TABLE_STATISTICS",
  "Percona and Sergei Golubchik",
  "Table Statistics",
  PLUGIN_LICENSE_GPL,
  table_stats_init,
  0,
  0x0200,
  NULL,
  NULL,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &userstat_info,
  "USER_STATISTICS",
  "Percona and Sergei Golubchik",
  "User Statistics",
  PLUGIN_LICENSE_GPL,
  user_stats_init,
  0,
  0x0200,
  NULL,
  NULL,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

