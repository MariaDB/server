/*
   Copyright (c) 2019, MariaDB Corporation

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
#include <mysql/plugin_function.h>

class Item_func_sysconst_test :public Item_func_sysconst
{
public:
  Item_func_sysconst_test(THD *thd): Item_func_sysconst(thd) {}
  String *val_str(String *str) override
  {
    null_value= str->copy(STRING_WITH_LEN("sysconst_test"), system_charset_info);
    return null_value ? NULL : str;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= MAX_FIELD_NAME * system_charset_info->mbmaxlen;
    set_maybe_null();
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("sysconst_test") };
    return name;
  }
  const char *fully_qualified_func_name() const override
  { return "sysconst_test()"; }
  Item *get_copy(THD *thd) override
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


static Plugin_function
  plugin_descriptor_function_sysconst_test(BUILDER(Create_func_sysconst_test));

/*************************************************************************/

maria_declare_plugin(type_test)
{
  MariaDB_FUNCTION_PLUGIN,       // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_sysconst_test, // pointer to type-specific plugin descriptor
  "sysconst_test",              // plugin name
  "MariaDB Corporation",        // plugin author
  "Function SYSCONST_TEST()",   // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
