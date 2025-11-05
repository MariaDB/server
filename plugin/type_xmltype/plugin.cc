/* Copyright (c) 2025 MariaDB Corporation

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

#define MYSQL_SERVER
#include "mariadb.h"
#include "sql_class.h"
#include "sql_type_xmltype.h"
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>


static struct st_mariadb_data_type plugin_descriptor_xmltype=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_xmltype
};


maria_declare_plugin(type_xmltype)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_xmltype,   // pointer to type-specific plugin descriptor
  "xmltype",                    // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type XMLTYPE",          // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_ALPHA // Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;

