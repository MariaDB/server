/* Copyright (c) 2019,2021 MariaDB Corporation

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
#include "sql_type_inet.h"
#include "item_inetfunc.h"
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>

static struct st_mariadb_data_type plugin_descriptor_type_inet6=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  Inet6Bundle::type_handler_fbt()
};


/*************************************************************************/

class Create_func_inet_ntoa : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_inet_ntoa(thd, arg1);
  }
  static Create_func_inet_ntoa s_singleton;
protected:
  Create_func_inet_ntoa() {}
  virtual ~Create_func_inet_ntoa() {}
};


class Create_func_inet_aton : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_inet_aton(thd, arg1);
  }
  static Create_func_inet_aton s_singleton;
protected:
  Create_func_inet_aton() {}
  virtual ~Create_func_inet_aton() {}
};


class Create_func_inet6_aton : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_inet6_aton(thd, arg1);
 } 
  static Create_func_inet6_aton s_singleton;
protected:
  Create_func_inet6_aton() {}
  virtual ~Create_func_inet6_aton() {}
};


class Create_func_inet6_ntoa : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_inet6_ntoa(thd, arg1);
  }
  static Create_func_inet6_ntoa s_singleton;
protected:
  Create_func_inet6_ntoa() {}
  virtual ~Create_func_inet6_ntoa() {}
};


class Create_func_is_ipv4 : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_is_ipv4(thd, arg1);
  }
  static Create_func_is_ipv4 s_singleton;
protected:
  Create_func_is_ipv4() {}
  virtual ~Create_func_is_ipv4() {}
};


class Create_func_is_ipv6 : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_is_ipv6(thd, arg1);
  }
  static Create_func_is_ipv6 s_singleton;
protected:
  Create_func_is_ipv6() {}
  virtual ~Create_func_is_ipv6() {}
};


class Create_func_is_ipv4_compat : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_is_ipv4_compat(thd, arg1);
  }
  static Create_func_is_ipv4_compat s_singleton;
protected:
  Create_func_is_ipv4_compat() {}
  virtual ~Create_func_is_ipv4_compat() {}
};


class Create_func_is_ipv4_mapped : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_is_ipv4_mapped(thd, arg1);
  }
  static Create_func_is_ipv4_mapped s_singleton;
protected:
  Create_func_is_ipv4_mapped() {}
  virtual ~Create_func_is_ipv4_mapped() {}
};


Create_func_inet_ntoa Create_func_inet_ntoa::s_singleton;
Create_func_inet6_aton Create_func_inet6_aton::s_singleton;
Create_func_inet6_ntoa Create_func_inet6_ntoa::s_singleton;
Create_func_inet_aton Create_func_inet_aton::s_singleton;
Create_func_is_ipv4 Create_func_is_ipv4::s_singleton;
Create_func_is_ipv6 Create_func_is_ipv6::s_singleton;
Create_func_is_ipv4_compat Create_func_is_ipv4_compat::s_singleton;
Create_func_is_ipv4_mapped Create_func_is_ipv4_mapped::s_singleton;


#define BUILDER(F) & F::s_singleton


static Plugin_function
  plugin_descriptor_function_inet_aton(BUILDER(Create_func_inet_aton)),
  plugin_descriptor_function_inet_ntoa(BUILDER(Create_func_inet_ntoa)),
  plugin_descriptor_function_inet6_aton(BUILDER(Create_func_inet6_aton)),
  plugin_descriptor_function_inet6_ntoa(BUILDER(Create_func_inet6_ntoa)),
  plugin_descriptor_function_is_ipv4(BUILDER(Create_func_is_ipv4)),
  plugin_descriptor_function_is_ipv6(BUILDER(Create_func_is_ipv6)),
  plugin_descriptor_function_is_ipv4_compat(BUILDER(Create_func_is_ipv4_compat)),
  plugin_descriptor_function_is_ipv4_mapped(BUILDER(Create_func_is_ipv4_mapped));


/*************************************************************************/

maria_declare_plugin(type_inet)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_inet6,// pointer to type-specific plugin descriptor
  "inet6",                      // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type INET6",            // the plugin description
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
  &plugin_descriptor_function_inet_aton, // pointer to type-specific plugin descriptor
  "inet_aton",                  // plugin name
  "MariaDB Corporation",        // plugin author
  "Function INET_ATON()",       // the plugin description
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
  &plugin_descriptor_function_inet_ntoa, // pointer to type-specific plugin descriptor
  "inet_ntoa",                  // plugin name
  "MariaDB Corporation",        // plugin author
  "Function INET_NTOA()",       // the plugin description
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
  &plugin_descriptor_function_inet6_aton, // pointer to type-specific plugin descriptor
  "inet6_aton",                 // plugin name
  "MariaDB Corporation",        // plugin author
  "Function INET6_ATON()",      // the plugin description
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
  &plugin_descriptor_function_inet6_ntoa, // pointer to type-specific plugin descriptor
  "inet6_ntoa",                 // plugin name
  "MariaDB Corporation",        // plugin author
  "Function INET6_NTOA()",      // the plugin description
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
  &plugin_descriptor_function_is_ipv4, // pointer to type-specific plugin descriptor
  "is_ipv4",                    // plugin name
  "MariaDB Corporation",        // plugin author
  "Function IS_IPV4()",         // the plugin description
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
  &plugin_descriptor_function_is_ipv6, // pointer to type-specific plugin descriptor
  "is_ipv6",                    // plugin name
  "MariaDB Corporation",        // plugin author
  "Function IS_IPV6()",         // the plugin description
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
  &plugin_descriptor_function_is_ipv4_compat, // pointer to type-specific plugin descriptor
  "is_ipv4_compat",             // plugin name
  "MariaDB Corporation",        // plugin author
  "Function IS_IPV4_COMPAT()",  // the plugin description
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
  &plugin_descriptor_function_is_ipv4_mapped, // pointer to type-specific plugin descriptor
  "is_ipv4_mapped",             // plugin name
  "MariaDB Corporation",        // plugin author
  "Function IS_IPV4_MAPPED()",// the plugin description
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
