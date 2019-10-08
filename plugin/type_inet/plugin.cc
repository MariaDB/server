/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation
   Copyright (c) 2019 MariaDB Corporation

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
#include <mysql/plugin_function_collection.h>


Type_handler_inet6 type_handler_inet6;


static struct st_mariadb_data_type plugin_descriptor_type_inet6=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_inet6
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


static Native_func_registry func_array[] =
{
  {{STRING_WITH_LEN("INET_ATON")}, BUILDER(Create_func_inet_aton)},
  {{STRING_WITH_LEN("INET_NTOA")}, BUILDER(Create_func_inet_ntoa)},
  {{STRING_WITH_LEN("INET6_ATON")}, BUILDER(Create_func_inet6_aton)},
  {{STRING_WITH_LEN("INET6_NTOA")}, BUILDER(Create_func_inet6_ntoa)},
  {{STRING_WITH_LEN("IS_IPV4")}, BUILDER(Create_func_is_ipv4)},
  {{STRING_WITH_LEN("IS_IPV6")}, BUILDER(Create_func_is_ipv6)},
  {{STRING_WITH_LEN("IS_IPV4_COMPAT")}, BUILDER(Create_func_is_ipv4_compat)},
  {{STRING_WITH_LEN("IS_IPV4_MAPPED")}, BUILDER(Create_func_is_ipv4_mapped)}
};


static Plugin_function_collection
  plugin_descriptor_function_collection_inet(
    MariaDB_FUNCTION_COLLECTION_INTERFACE_VERSION,
    Native_func_registry_array(func_array, array_elements(func_array)));

/*************************************************************************/

maria_declare_plugin(type_inet)
{
  MariaDB_FUNCTION_COLLECTION_PLUGIN, // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_collection_inet, // pointer to type-specific plugin descriptor
  "func_inet",                  // plugin name
  "MariaDB Corporation",        // plugin author
  "Function collection test",   // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB veriosn
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_inet6,// pointer to type-specific plugin descriptor
  type_handler_inet6.name().ptr(),// plugin name
  "MariaDB Corporation",        // plugin author
  "Data type TEST_INT8",        // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB veriosn
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
