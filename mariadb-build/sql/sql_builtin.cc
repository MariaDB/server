/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA
*/

/*
  Note that sql_builtin.cc is automatically built by sql_bultin.cc.in
  and cmake/plugin.cmake
*/

#include <my_global.h>
#include <mysql/plugin.h>

typedef struct st_maria_plugin builtin_maria_plugin[];

#ifdef _MSC_VER
extern "C"
#else
extern
#endif
builtin_maria_plugin 
   builtin_maria_csv_plugin,
 builtin_maria_heap_plugin,
 builtin_maria_aria_plugin,
 builtin_maria_myisam_plugin,
 builtin_maria_myisammrg_plugin,
 builtin_maria_parquet_plugin,
 builtin_maria_type_assoc_array_plugin,
 builtin_maria_type_cursor_plugin,
 builtin_maria_type_geom_plugin,
 builtin_maria_type_inet_plugin,
 builtin_maria_type_uuid_plugin,
 builtin_maria_type_xmltype_plugin,
 builtin_maria_userstat_plugin,
 builtin_maria_sql_sequence_plugin,
#ifndef EMBEDDED_LIBRARY
 builtin_maria_online_alter_log_plugin,
#endif
  builtin_maria_innobase_plugin,
 builtin_maria_perfschema_plugin,
 builtin_maria_sequence_plugin,
 builtin_maria_auth_mysql_sha2_plugin,
 builtin_maria_auth_parsec_plugin,
 builtin_maria_auth_socket_plugin,
 builtin_maria_feedback_plugin,
 builtin_maria_user_variables_plugin,
#ifndef EMBEDDED_LIBRARY
 builtin_maria_thread_pool_info_plugin,
#endif
 builtin_maria_partition_plugin,

  builtin_maria_binlog_plugin, builtin_maria_mhnsw_plugin,
  builtin_maria_mysql_password_plugin;

struct st_maria_plugin *mysql_optional_plugins[]=
{
   builtin_maria_innobase_plugin,
 builtin_maria_perfschema_plugin,
 builtin_maria_sequence_plugin,
 builtin_maria_auth_mysql_sha2_plugin,
 builtin_maria_auth_parsec_plugin,
 builtin_maria_auth_socket_plugin,
 builtin_maria_feedback_plugin,
 builtin_maria_user_variables_plugin,
#ifndef EMBEDDED_LIBRARY
 builtin_maria_thread_pool_info_plugin,
#endif
 builtin_maria_partition_plugin,
 0
};

struct st_maria_plugin *mysql_mandatory_plugins[]=
{
  builtin_maria_binlog_plugin, builtin_maria_mysql_password_plugin,
  builtin_maria_mhnsw_plugin,
   builtin_maria_csv_plugin,
 builtin_maria_heap_plugin,
 builtin_maria_aria_plugin,
 builtin_maria_myisam_plugin,
 builtin_maria_myisammrg_plugin,
 builtin_maria_parquet_plugin,
 builtin_maria_type_assoc_array_plugin,
 builtin_maria_type_cursor_plugin,
 builtin_maria_type_geom_plugin,
 builtin_maria_type_inet_plugin,
 builtin_maria_type_uuid_plugin,
 builtin_maria_type_xmltype_plugin,
 builtin_maria_userstat_plugin,
 builtin_maria_sql_sequence_plugin,
#ifndef EMBEDDED_LIBRARY
 builtin_maria_online_alter_log_plugin,
#endif
 0
};
