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
 * The server defines many client functions that are used in mariadb-backup and a number of storage engine plugins.
 * That can cause a problem if a plugin loads libmariadb/libmysql or a library that has dependency on them.
 * Known cases are Replication and the ODBC driver.
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
#define mysql_real_escape_string MARIADB_ADD_PREFIX(mysql_real_escape_string)
#define mysql_get_server_version MARIADB_ADD_PREFIX(mysql_get_server_version)
#define mysql_error             MARIADB_ADD_PREFIX(mysql_error)
#define mysql_errno             MARIADB_ADD_PREFIX(mysql_errno)
#define mysql_num_fields        MARIADB_ADD_PREFIX(mysql_num_fields)
#define mysql_num_rows          MARIADB_ADD_PREFIX(mysql_num_rows)
#define mysql_options4          MARIADB_ADD_PREFIX(mysql_options4)
#define mysql_fetch_fields      MARIADB_ADD_PREFIX(mysql_fetch_fields)
#define mysql_fetch_lengths     MARIADB_ADD_PREFIX(mysql_fetch_lengths)
#define mysql_fetch_row         MARIADB_ADD_PREFIX(mysql_fetch_row)
#define mysql_affected_rows     MARIADB_ADD_PREFIX(mysql_affected_rows)
#define mysql_store_result      MARIADB_ADD_PREFIX(mysql_store_result)
#define mysql_use_result        MARIADB_ADD_PREFIX(mysql_use_result)
#define mysql_select_db         MARIADB_ADD_PREFIX(mysql_select_db)
#define mysql_get_ssl_cipher    MARIADB_ADD_PREFIX(mysql_get_ssl_cipher)
#define mysql_ssl_set           MARIADB_ADD_PREFIX(mysql_ssl_set)
#define mysql_client_register_plugin MARIADB_ADD_PREFIX(mysql_client_register_plugin)

// Generic API
#define mariadb_connection             MARIADB_ADD_PREFIX(mariadb_connection)
#define mariadb_field_attr             MARIADB_ADD_PREFIX(mariadb_field_attr)
#define mysql_autocommit               MARIADB_ADD_PREFIX(mysql_autocommit)
#define mysql_change_user              MARIADB_ADD_PREFIX(mysql_change_user)
#define mysql_character_set_name       MARIADB_ADD_PREFIX(mysql_character_set_name)
#define mysql_commit                   MARIADB_ADD_PREFIX(mysql_commit)
#define mysql_data_seek                MARIADB_ADD_PREFIX(mysql_data_seek)
#define mysql_debug                    MARIADB_ADD_PREFIX(mysql_debug)
#define mysql_dump_debug_info          MARIADB_ADD_PREFIX(mysql_dump_debug_info)
#define mysql_embedded                 MARIADB_ADD_PREFIX(mysql_embedded)
#define mysql_eof                      MARIADB_ADD_PREFIX(mysql_eof)
#define mysql_escape_string            MARIADB_ADD_PREFIX(mysql_escape_string)
#define mysql_fetch_field              MARIADB_ADD_PREFIX(mysql_fetch_field)
#define mysql_fetch_field_direct       MARIADB_ADD_PREFIX(mysql_fetch_field_direct)
#define mysql_field_count              MARIADB_ADD_PREFIX(mysql_field_count)
#define mysql_field_seek               MARIADB_ADD_PREFIX(mysql_field_seek)
#define mysql_field_tell               MARIADB_ADD_PREFIX(mysql_field_tell)
#define mysql_get_character_set_info   MARIADB_ADD_PREFIX(mysql_get_character_set_info)
#define mysql_get_client_info          MARIADB_ADD_PREFIX(mysql_get_client_info)
#define mysql_get_client_version       MARIADB_ADD_PREFIX(mysql_get_client_version)
#define mysql_get_host_info            MARIADB_ADD_PREFIX(mysql_get_host_info)
#define mysql_get_parameters           MARIADB_ADD_PREFIX(mysql_get_parameters)
#define mysql_get_proto_info           MARIADB_ADD_PREFIX(mysql_get_proto_info)
#define mysql_get_server_info          MARIADB_ADD_PREFIX(mysql_get_server_info)
#define mysql_get_server_name          MARIADB_ADD_PREFIX(mysql_get_server_name)
#define mysql_hex_string               MARIADB_ADD_PREFIX(mysql_hex_string)
#define mysql_info                     MARIADB_ADD_PREFIX(mysql_info)
#define mysql_insert_id                MARIADB_ADD_PREFIX(mysql_insert_id)
#define mysql_kill                     MARIADB_ADD_PREFIX(mysql_kill)
#define mysql_list_dbs                 MARIADB_ADD_PREFIX(mysql_list_dbs)
#define mysql_list_fields              MARIADB_ADD_PREFIX(mysql_list_fields)
#define mysql_list_processes           MARIADB_ADD_PREFIX(mysql_list_processes)
#define mysql_list_tables              MARIADB_ADD_PREFIX(mysql_list_tables)
#define mysql_more_results             MARIADB_ADD_PREFIX(mysql_more_results)
#define mysql_net_field_length         MARIADB_ADD_PREFIX(mysql_net_field_length)
#define mysql_net_read_packet          MARIADB_ADD_PREFIX(mysql_net_read_packet)
#define mysql_next_result              MARIADB_ADD_PREFIX(mysql_next_result)
#define mysql_ping                     MARIADB_ADD_PREFIX(mysql_ping)
#define mysql_query                    MARIADB_ADD_PREFIX(mysql_query)
#define mysql_read_query_result        MARIADB_ADD_PREFIX(mysql_read_query_result)
#define mysql_refresh                  MARIADB_ADD_PREFIX(mysql_refresh)
#define mysql_rollback                 MARIADB_ADD_PREFIX(mysql_rollback)
#define mysql_row_seek                 MARIADB_ADD_PREFIX(mysql_row_seek)
#define mysql_row_tell                 MARIADB_ADD_PREFIX(mysql_row_tell)
#define mysql_server_end               MARIADB_ADD_PREFIX(mysql_server_end)
#define mysql_server_init              MARIADB_ADD_PREFIX(mysql_server_init)
#define mysql_set_local_infile_default MARIADB_ADD_PREFIX(mysql_set_local_infile_default)
#define mysql_set_local_infile_handler MARIADB_ADD_PREFIX(mysql_set_local_infile_handler)
#define mysql_set_server_option        MARIADB_ADD_PREFIX(mysql_set_server_option)
#define mysql_shutdown                 MARIADB_ADD_PREFIX(mysql_shutdown)
#define mysql_sqlstate                 MARIADB_ADD_PREFIX(mysql_sqlstate)
#define mysql_stat                     MARIADB_ADD_PREFIX(mysql_stat)
#define mysql_thread_end               MARIADB_ADD_PREFIX(mysql_thread_end)
#define mysql_thread_id                MARIADB_ADD_PREFIX(mysql_thread_id)
#define mysql_thread_init              MARIADB_ADD_PREFIX(mysql_thread_init)
#define mysql_thread_safe              MARIADB_ADD_PREFIX(mysql_thread_safe)
#define mysql_warning_count            MARIADB_ADD_PREFIX(mysql_warning_count)

// Dynamic Column API
#define mariadb_dyncol_check             MARIADB_ADD_PREFIX(mariadb_dyncol_check)
#define mariadb_dyncol_column_cmp_named  MARIADB_ADD_PREFIX(mariadb_dyncol_column_cmp_named)
#define mariadb_dyncol_column_count      MARIADB_ADD_PREFIX(mariadb_dyncol_column_count)
#define mariadb_dyncol_create_many_named MARIADB_ADD_PREFIX(mariadb_dyncol_create_many_named)
#define mariadb_dyncol_create_many_num   MARIADB_ADD_PREFIX(mariadb_dyncol_create_many_num)
#define mariadb_dyncol_exists_named      MARIADB_ADD_PREFIX(mariadb_dyncol_exists_named)
#define mariadb_dyncol_exists_num        MARIADB_ADD_PREFIX(mariadb_dyncol_exists_num)
#define mariadb_dyncol_free              MARIADB_ADD_PREFIX(mariadb_dyncol_free)
#define mariadb_dyncol_get_named         MARIADB_ADD_PREFIX(mariadb_dyncol_get_named)
#define mariadb_dyncol_get_num           MARIADB_ADD_PREFIX(mariadb_dyncol_get_num)
#define mariadb_dyncol_has_names         MARIADB_ADD_PREFIX(mariadb_dyncol_has_names)
#define mariadb_dyncol_json              MARIADB_ADD_PREFIX(mariadb_dyncol_json)
#define mariadb_dyncol_list_named        MARIADB_ADD_PREFIX(mariadb_dyncol_list_named)
#define mariadb_dyncol_list_num          MARIADB_ADD_PREFIX(mariadb_dyncol_list_num)
#define mariadb_dyncol_unpack            MARIADB_ADD_PREFIX(mariadb_dyncol_unpack)
#define mariadb_dyncol_update_many_named MARIADB_ADD_PREFIX(mariadb_dyncol_update_many_named)
#define mariadb_dyncol_update_many_num   MARIADB_ADD_PREFIX(mariadb_dyncol_update_many_num)
#define mariadb_dyncol_val_double        MARIADB_ADD_PREFIX(mariadb_dyncol_val_double)
#define mariadb_dyncol_val_long          MARIADB_ADD_PREFIX(mariadb_dyncol_val_long)
#define mariadb_dyncol_val_str           MARIADB_ADD_PREFIX(mariadb_dyncol_val_str)

// Prepared Statement API
#define mysql_stmt_affected_rows   MARIADB_ADD_PREFIX(mysql_stmt_affected_rows)
#define mysql_stmt_attr_get        MARIADB_ADD_PREFIX(mysql_stmt_attr_get)
#define mysql_stmt_attr_set        MARIADB_ADD_PREFIX(mysql_stmt_attr_set)
#define mysql_stmt_bind_param      MARIADB_ADD_PREFIX(mysql_stmt_bind_param)
#define mysql_stmt_bind_result     MARIADB_ADD_PREFIX(mysql_stmt_bind_result)
#define mysql_stmt_close           MARIADB_ADD_PREFIX(mysql_stmt_close)
#define mysql_stmt_data_seek       MARIADB_ADD_PREFIX(mysql_stmt_data_seek)
#define mysql_stmt_errno           MARIADB_ADD_PREFIX(mysql_stmt_errno)
#define mysql_stmt_error           MARIADB_ADD_PREFIX(mysql_stmt_error)
#define mysql_stmt_execute         MARIADB_ADD_PREFIX(mysql_stmt_execute)
#define mysql_stmt_fetch           MARIADB_ADD_PREFIX(mysql_stmt_fetch)
#define mysql_stmt_fetch_column    MARIADB_ADD_PREFIX(mysql_stmt_fetch_column)
#define mysql_stmt_field_count     MARIADB_ADD_PREFIX(mysql_stmt_field_count)
#define mysql_stmt_free_result     MARIADB_ADD_PREFIX(mysql_stmt_free_result)
#define mysql_stmt_init            MARIADB_ADD_PREFIX(mysql_stmt_init)
#define mysql_stmt_insert_id       MARIADB_ADD_PREFIX(mysql_stmt_insert_id)
#define mysql_stmt_next_result     MARIADB_ADD_PREFIX(mysql_stmt_next_result)
#define mysql_stmt_num_rows        MARIADB_ADD_PREFIX(mysql_stmt_num_rows)
#define mysql_stmt_param_count     MARIADB_ADD_PREFIX(mysql_stmt_param_count)
#define mysql_stmt_param_metadata  MARIADB_ADD_PREFIX(mysql_stmt_param_metadata)
#define mysql_stmt_prepare         MARIADB_ADD_PREFIX(mysql_stmt_prepare)
#define mysql_stmt_reset           MARIADB_ADD_PREFIX(mysql_stmt_reset)
#define mysql_stmt_result_metadata MARIADB_ADD_PREFIX(mysql_stmt_result_metadata)
#define mysql_stmt_row_seek        MARIADB_ADD_PREFIX(mysql_stmt_row_seek)
#define mysql_stmt_row_tell        MARIADB_ADD_PREFIX(mysql_stmt_row_tell)
#define mysql_stmt_send_long_data  MARIADB_ADD_PREFIX(mysql_stmt_send_long_data)
#define mysql_stmt_sqlstate        MARIADB_ADD_PREFIX(mysql_stmt_sqlstate)
#define mysql_stmt_store_result    MARIADB_ADD_PREFIX(mysql_stmt_store_result)

// Implementation Functions
#define bin2decimal                MARIADB_ADD_PREFIX(bin2decimal)
#define decimal_bin_size           MARIADB_ADD_PREFIX(decimal_bin_size)
#define decimal_size               MARIADB_ADD_PREFIX(decimal_size)
#define decimal2string             MARIADB_ADD_PREFIX(decimal2string)
#define dynamic_column_create      MARIADB_ADD_PREFIX(dynamic_column_create)
#define dynamic_column_create_many MARIADB_ADD_PREFIX(dynamic_column_create_many)
#define dynamic_column_get         MARIADB_ADD_PREFIX(dynamic_column_get)
#define dynamic_column_list        MARIADB_ADD_PREFIX(dynamic_column_list)
#define dynamic_column_update      MARIADB_ADD_PREFIX(dynamic_column_update)
#define dynamic_column_update_many MARIADB_ADD_PREFIX(dynamic_column_update_many)
#define free_rows                  MARIADB_ADD_PREFIX(free_rows)
#define init_client_errs           MARIADB_ADD_PREFIX(init_client_errs)
#define mpvio_info                 MARIADB_ADD_PREFIX(mpvio_info)
#define mysql_client_plugin_deinit MARIADB_ADD_PREFIX(mysql_client_plugin_deinit)
#define mysql_client_plugin_init   MARIADB_ADD_PREFIX(mysql_client_plugin_init)
#define mysql_close_slow_part      MARIADB_ADD_PREFIX(mysql_close_slow_part)

#define net_field_length MARIADB_ADD_PREFIX(net_field_length)
#define read_user_name   MARIADB_ADD_PREFIX(read_user_name)
#define run_plugin_auth  MARIADB_ADD_PREFIX(run_plugin_auth)
#define unpack_fields    MARIADB_ADD_PREFIX(unpack_fields)

// Implementation variables
#define client_errors            MARIADB_ADD_PREFIX(client_errors)
#define mariadb_deinitialize_ssl MARIADB_ADD_PREFIX(mariadb_deinitialize_ssl)
#define max_allowed_packet       MARIADB_ADD_PREFIX(max_allowed_packet)
#define mysql_client_builtins    MARIADB_ADD_PREFIX(mysql_client_builtins)
#define mysql_port               MARIADB_ADD_PREFIX(mysql_port)
#define mysql_unix_port          MARIADB_ADD_PREFIX(mysql_unix_port)
#define net_buffer_length        MARIADB_ADD_PREFIX(net_buffer_length)
#define plugin_list              MARIADB_ADD_PREFIX(plugin_list)

#endif // !EMBEDDED_LIBRARY && !MYSQL_DYNAMIC_PLUGIN

#endif // !MARIADB_CAPI_RENAME_INCLUDED
