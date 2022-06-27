/* Copyright (c) 2019,2021, MariaDB Corporation

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
#include "sql_type_uuid.h"
#include "item_uuidfunc.h"
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>

static struct st_mariadb_data_type plugin_descriptor_type_uuid=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  UUIDBundle::type_handler_fbt()
};

/*************************************************************************/

class Create_func_uuid : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override
  {
    DBUG_ENTER("Create_func_uuid::create");
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    thd->lex->safe_to_cache_query= 0;
    DBUG_RETURN(new (thd->mem_root) Item_func_uuid(thd));
  }
  static Create_func_uuid s_singleton;

protected:
  Create_func_uuid() {}
  virtual ~Create_func_uuid() {}
};


class Create_func_sys_guid : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override
  {
    DBUG_ENTER("Create_func_sys_guid::create");
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    thd->lex->safe_to_cache_query= 0;
    DBUG_RETURN(new (thd->mem_root) Item_func_sys_guid(thd));
  }
  static Create_func_sys_guid s_singleton;

protected:
  Create_func_sys_guid() {}
  virtual ~Create_func_sys_guid() {}
};

Create_func_uuid Create_func_uuid::s_singleton;
Create_func_sys_guid Create_func_sys_guid::s_singleton;

static Plugin_function
  plugin_descriptor_function_uuid(&Create_func_uuid::s_singleton),
  plugin_descriptor_function_sys_guid(&Create_func_sys_guid::s_singleton);

/*************************************************************************/

maria_declare_plugin(type_uuid)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_uuid, // pointer to type-specific plugin descriptor
  "uuid",                       // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type UUID",             // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE// Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_uuid,// pointer to type-specific plugin descriptor
  "uuid",                       // plugin name
  "MariaDB Corporation",        // plugin author
  "Function UUID()",            // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE// Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_sys_guid,// pointer to type-specific plugin descriptor
  "sys_guid",                   // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SYS_GUID()",        // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE// Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
