#ifndef MARIADB_PLUGIN_FUNCTION_INCLUDED
#define MARIADB_PLUGIN_FUNCTION_INCLUDED
/* Copyright (C) 2019, Alexander Barkov and MariaDB

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

/**
  @file

  Function Plugin API.

  This file defines the API for server plugins that manage functions.
*/

#ifdef __cplusplus

#include <mysql/plugin.h>

/*
  API for function plugins. (MariaDB_FUNCTION_PLUGIN)
*/
#define MariaDB_FUNCTION_INTERFACE_VERSION (MYSQL_VERSION_ID << 8)


class Plugin_function
{
  int m_interface_version;
  Create_func *m_builder;
public:
  Plugin_function(Create_func *builder)
   :m_interface_version(MariaDB_FUNCTION_INTERFACE_VERSION),
    m_builder(builder)
  { }
  Create_func *create_func()
  {
    return m_builder;
  }
};


template<class BASE, class ITEM, Create_func::Type routine_type>
class TFuncPluginCreate : public BASE
{
public:
  using BASE::BASE;
  Item *create_3_arg(THD *thd, Item *arg1, Item *arg2, Item *arg3) override
  {
    return new (thd->mem_root) ITEM(thd, arg1, arg2, arg3);
  }
  Create_func::Type type() const override
  {
    return routine_type;
  }
  static Plugin_function *plugin_descriptor()
  {
    static TFuncPluginCreate creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};



#endif /* __cplusplus */

#endif /* MARIADB_PLUGIN_FUNCTION_INCLUDED */
