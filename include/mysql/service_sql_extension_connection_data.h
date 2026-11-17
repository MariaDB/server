/* Copyright (c) 2026, MariaDB plc

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

#ifndef MYSQL_SERVICE_SQL_EXTENSION_CONNECTION_DATA_INCLUDED
#define MYSQL_SERVICE_SQL_EXTENSION_CONNECTION_DATA_INCLUDED

/**
  @file include/mysql/service_sql_extension_connection_data.h
  SQL extension connection data service: data that a
  MariaDB_SQL_EXTENSION_PLUGIN keeps on the client's connection.
*/

/**
  @defgroup plugin_api_service_sql_extension_connection_data Connection Data Service
  @ingroup plugin_api_services

  Data an SQL extension plugin keeps on the client's connection between
  its statements (see service_sql_extension.h for the rules common to the
  SQL extension services).
  @{
*/

#ifdef __cplusplus
extern "C" {
#endif

extern struct sql_extension_connection_data_service_st {
  void *(*get_connection_data_func)(MYSQL_THD thd);
  int (*set_connection_data_func)(MYSQL_THD thd, void *data,
                                  void (*free_data)(void *data));
} *sql_extension_connection_data_service;

#ifdef MYSQL_DYNAMIC_PLUGIN

#define sql_extension_get_connection_data(thd) \
  sql_extension_connection_data_service->get_connection_data_func(thd)
#define sql_extension_set_connection_data(thd, data, free_data) \
  sql_extension_connection_data_service->set_connection_data_func(thd, data, \
                                                                  free_data)

#else

/**
  The data the plugin keeps on the client's connection between its
  statements (set with sql_extension_set_connection_data()), or NULL when
  it keeps none. Each plugin has its own: another plugin's data, or that
  of an earlier installation of the same plugin, is not returned.
*/
void *sql_extension_get_connection_data(MYSQL_THD thd);
/**
  Keep data on the client's connection for later statements of the
  plugin, replacing what it kept before, which is freed with its free
  function. NULL data drops it (also freed). free_data (may be NULL)
  frees the data when it is replaced or dropped, and when the connection
  ends or changes user (COM_CHANGE_USER). It runs without a statement:
  it must only free memory, without using the THD or any service. While
  a connection keeps data of the plugin, the plugin's library stays
  loaded: UNINSTALL PLUGIN then warns that the plugin is busy, the plugin
  gets no more statements, and its library is unloaded once every such
  connection has ended.

  @return 0, or non-zero after raising an error (out of memory), in which
          case the plugin still owns data and what it kept before is
          unchanged
*/
int sql_extension_set_connection_data(MYSQL_THD thd, void *data,
                                      void (*free_data)(void *data));
#endif /* MYSQL_DYNAMIC_PLUGIN */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* MYSQL_SERVICE_SQL_EXTENSION_CONNECTION_DATA_INCLUDED */
