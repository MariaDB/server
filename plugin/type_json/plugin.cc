#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "sql_type_json.h"
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>

static struct st_mariadb_data_type plugin_descriptor_json=
{
    MariaDB_DATA_TYPE_INTERFACE_VERSION,
    &type_handler_json
};

maria_declare_plugin(type_json)
{
  MariaDB_DATA_TYPE_PLUGIN, // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_json,  // pointer to type-specific plugin descriptor
  "json",                   // plugin name
  "MariaDB Corporation",    // plugin author
  "Data type json",         // the plugin description
  PLUGIN_LICENSE_GPL,       // the plugin license (see include/mysql/plugin.h)
  0,                        // Pointer to plugin initialization function
  0,                        // Pointer to plugin deinitialization function
  0x0100,                   // Numeric version 0xAABB means AA.BB version
  NULL,                     // Status variables
  NULL,                     // System variables
  "1.0",                    // String version representation
  MariaDB_PLUGIN_MATURITY_GAMMA // Maturity(see include/mysql/plugin.h)
}
maria_declare_plugin_end;

