#ifndef MARIADB_PLUGIN_FUNCTION_COLLECTION_INCLUDED
#define MARIADB_PLUGIN_FUNCTION_COLLECTION_INCLUDED
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

  Data Type Plugin API.

  This file defines the API for server plugins that manage function collections.
*/

#ifdef __cplusplus

#include <mysql/plugin.h>

/*
  API for data type plugins. (MariaDB_FUNCTION_COLLECTION_PLUGIN)
*/
#define MariaDB_FUNCTION_COLLECTION_INTERFACE_VERSION (MYSQL_VERSION_ID << 8)


class Native_func_registry_array
{
  const Native_func_registry *m_elements;
  size_t m_count;
public:
  Native_func_registry_array()
   :m_elements(NULL),
    m_count(0)
  { }
  Native_func_registry_array(const Native_func_registry *elements, size_t count)
   :m_elements(elements),
    m_count(count)
  { }
  const Native_func_registry& element(size_t i) const
  {
    DBUG_ASSERT(i < m_count);
    return m_elements[i];
  }
  size_t count() const { return m_count; }
};


class Plugin_function_collection
{
  int m_interface_version;
  const Native_func_registry_array m_native_func_registry_array;
  HASH m_hash;
public:
  bool init();
  void deinit()
  {
    my_hash_free(&m_hash);
  }
  static int init_plugin(st_plugin_int *plugin)
  {
    Plugin_function_collection *coll=
      reinterpret_cast<Plugin_function_collection*>(plugin->plugin->info);
    return coll->init();
  }
  static int deinit_plugin(st_plugin_int *plugin)
  {
    Plugin_function_collection *coll=
      reinterpret_cast<Plugin_function_collection*>(plugin->plugin->info);
    coll->deinit();
    return 0;
  }
public:
  Plugin_function_collection(int interface_version,
                             const Native_func_registry_array &nfra)
   :m_interface_version(interface_version),
    m_native_func_registry_array(nfra)
  {
    bzero((void*) &m_hash, sizeof(m_hash));
  }
  Create_func *find_native_function_builder(THD *thd,
                                            const LEX_CSTRING &name) const;
};


/**
  Data type plugin descriptor
*/

#endif /* __cplusplus */

#endif /* MARIADB_PLUGIN_FUNCTION_COLLECTION_INCLUDED */
