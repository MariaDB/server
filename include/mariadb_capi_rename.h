/* Copyright (c) 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* Renaming C API symbols inside server
 * client.c defines a number of functions from the C API, that are used in replication, in number of storage engine plugins, mariadb-backup.
 * That can cause a problem if a plugin loads libmariadb/libmysql or a library, that has dependency on them. The known case is ODBC driver.
 * Thus the header re-names those functions for internal use.
 */

#ifndef MARIADB_CAPI_RENAME_INCLUDED
#define MARIADB_CAPI_RENAME_INCLUDED

#if !defined(EMBEDDED_LIBRARY) && !defined(MYSQL_DYNAMIC_PLUGIN)

#define MARIADB_ADD_PREFIX(_SYMBOL) server_##_SYMBOL
#define mysql_real_connect      MARIADB_ADD_PREFIX(mysql_real_connect)
#define mysql_init              MARIADB_ADD_PREFIX(mysql_init)
#define mysql_close             MARIADB_ADD_PREFIX(mysql_close)
#define mysql_options           MARIADB_ADD_PREFIX(mysql_options)
#define mysql_load_plugin       MARIADB_ADD_PREFIX(mysql_load_plugin)
#define mysql_load_plugin_v     MARIADB_ADD_PREFIX(mysql_load_plugin_v)
#define mysql_client_find_plugin MARIADB_ADD_PREFIX(mysql_client_find_plugin)
#define mysql_real_query        MARIADB_ADD_PREFIX(mysql_real_query)
#define mysql_send_query        MARIADB_ADD_PREFIX(mysql_send_query)
#define mysql_free_result       MARIADB_ADD_PREFIX(mysql_free_result)
#define mysql_get_socket        MARIADB_ADD_PREFIX(mysql_get_socket)
#define mysql_set_character_set MARIADB_ADD_PREFIX(mysql_set_character_set)
#define mysql_get_server_version MARIADB_ADD_PREFIX(mysql_get_server_version)
#define mysql_error             MARIADB_ADD_PREFIX(mysql_error)
#define mysql_errno             MARIADB_ADD_PREFIX(mysql_errno)
#define mysql_num_fields        MARIADB_ADD_PREFIX(mysql_num_fields)
#define mysql_num_rows          MARIADB_ADD_PREFIX(mysql_num_rows)
#define mysql_options4          MARIADB_ADD_PREFIX(mysql_options4)
#define mysql_fetch_lengths     MARIADB_ADD_PREFIX(mysql_fetch_lengths)
#define mysql_fetch_row         MARIADB_ADD_PREFIX(mysql_fetch_row)
#define mysql_affected_rows     MARIADB_ADD_PREFIX(mysql_affected_rows)
#define mysql_store_result      MARIADB_ADD_PREFIX(mysql_store_result)
#define mysql_select_db         MARIADB_ADD_PREFIX(mysql_select_db)
#define mysql_get_ssl_cipher    MARIADB_ADD_PREFIX(mysql_get_ssl_cipher)
#define mysql_ssl_set           MARIADB_ADD_PREFIX(mysql_ssl_set)
#define mysql_client_register_plugin MARIADB_ADD_PREFIX(mysql_client_register_plugin)

#endif // !EMBEDDED_LIBRARY && !MYSQL_DYNAMIC_PLUGIN

#endif // !MARIADB_CAPI_RENAME_INCLUDED
