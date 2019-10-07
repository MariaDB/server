/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#define MYSQL_SERVER

#include <my_global.h>
#include <sql_class.h>
#include <mysql/plugin_function_collection.h>

class Item_func_sysconst_test :public Item_func_sysconst
{
public:
  Item_func_sysconst_test(THD *thd): Item_func_sysconst(thd) {}
  String *val_str(String *str)
  {
    null_value= str->copy(STRING_WITH_LEN("sysconst_test"), system_charset_info);
    return null_value ? NULL : str;
  }
  bool fix_length_and_dec()
  {
    max_length= MAX_FIELD_NAME * system_charset_info->mbmaxlen;
    maybe_null= true;
    return false;
  }
  const char *func_name() const { return "sysconst_test"; }
  const char *fully_qualified_func_name() const { return "sysconst_test()"; }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_func_sysconst_test>(thd, this); }
};


class Create_func_sysconst_test : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override;
  static Create_func_sysconst_test s_singleton;
protected:
  Create_func_sysconst_test() {}
};


Create_func_sysconst_test Create_func_sysconst_test::s_singleton;

Item* Create_func_sysconst_test::create_builder(THD *thd)
{
  return new (thd->mem_root) Item_func_sysconst_test(thd);
}


#define BUILDER(F) & F::s_singleton


static Native_func_registry func_array[] =
{
  {{STRING_WITH_LEN("SYSCONST_TEST")}, BUILDER(Create_func_sysconst_test)}
};


static Plugin_function_collection
  plugin_descriptor_function_collection_test(
    MariaDB_FUNCTION_COLLECTION_INTERFACE_VERSION,
    Native_func_registry_array(func_array, array_elements(func_array)));

/*************************************************************************/

maria_declare_plugin(type_test)
{
  MariaDB_FUNCTION_COLLECTION_PLUGIN, // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_collection_test, // pointer to type-specific plugin descriptor
  "func_test",                  // plugin name
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
}
maria_declare_plugin_end;
