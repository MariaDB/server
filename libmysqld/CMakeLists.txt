# Copyright (c) 2006, 2011, Oracle and/or its affiliates.
# Copyright (c) 2009, 2020, MariaDB Corporation
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

ADD_DEFINITIONS(-DMYSQL_SERVER -DEMBEDDED_LIBRARY
 ${SSL_DEFINES})

INCLUDE_DIRECTORIES(
${CMAKE_SOURCE_DIR}/include 
${CMAKE_SOURCE_DIR}/libmysqld
${CMAKE_SOURCE_DIR}/sql
${CMAKE_SOURCE_DIR}/tpool
${CMAKE_BINARY_DIR}/sql 
${PCRE_INCLUDES}
${LIBFMT_INCLUDE_DIR}
${ZLIB_INCLUDE_DIR}
${SSL_INCLUDE_DIRS}
${SSL_INTERNAL_INCLUDE_DIRS}
)

SET(GEN_SOURCES
${CMAKE_BINARY_DIR}/sql/sql_yacc.hh 
${CMAKE_BINARY_DIR}/sql/yy_mariadb.cc
${CMAKE_BINARY_DIR}/sql/yy_oracle.hh
${CMAKE_BINARY_DIR}/sql/yy_oracle.cc
${CMAKE_BINARY_DIR}/sql/lex_hash.h 
)

SET_SOURCE_FILES_PROPERTIES(${GEN_SOURCES} PROPERTIES GENERATED TRUE)

SET(SQL_EMBEDDED_SOURCES emb_qcache.cc libmysqld.c lib_sql.cc 
           libmysql.c ../sql-common/errmsg.c
           ../sql-common/client.c
           ../sql-common/my_user.c ../sql-common/pack.c
           ../sql-common/client_plugin.c
           ../sql/password.c ../sql/discover.cc ../sql/derror.cc 
           ../sql/field.cc ../sql/field_conv.cc ../sql/field_comp.cc
           ../sql/filesort_utils.cc ../sql/sql_digest.cc
           ../sql/filesort.cc ../sql/grant.cc
           ../sql/gstream.cc ../sql/slave.cc
           ../sql/signal_handler.cc
           ../sql/handler.cc ../sql/hash_filo.cc ../sql/hostname.cc 
           ../sql/init.cc ../sql/item_buff.cc ../sql/item_cmpfunc.cc 
           ../sql/item.cc ../sql/item_create.cc ../sql/item_func.cc 
           ../sql/item_geofunc.cc ../sql/item_row.cc ../sql/item_strfunc.cc 
           ../sql/item_subselect.cc ../sql/item_sum.cc ../sql/item_timefunc.cc 
           ../sql/item_xmlfunc.cc ../sql/item_jsonfunc.cc
           ../sql/key.cc ../sql/lock.cc ../sql/log.cc 
           ../sql/log_event.cc ../sql/log_event_server.cc 
           ../sql/mf_iocache.cc ../sql/my_decimal.cc 
           ../sql/net_serv.cc ../sql/opt_range.cc ../sql/opt_sum.cc 
           ../sql/parse_file.cc ../sql/procedure.cc ../sql/protocol.cc 
           ../sql/records.cc ../sql/repl_failsafe.cc ../sql/rpl_filter.cc
           ../sql/rpl_record.cc ../sql/des_key_file.cc
           ../sql/rpl_injector.cc ../sql/set_var.cc ../sql/spatial.cc 
           ../sql/sp_cache.cc ../sql/sp.cc ../sql/sp_head.cc 
           ../sql/sp_pcontext.cc ../sql/sp_rcontext.cc ../sql/sql_acl.cc 
           ../sql/sql_analyse.cc ../sql/sql_base.cc ../sql/sql_cache.cc 
           ../sql/sql_class.cc ../sql/sql_crypt.cc ../sql/sql_cursor.cc 
           ../sql/sql_db.cc ../sql/sql_delete.cc ../sql/sql_derived.cc 
           ../sql/sql_do.cc ../sql/sql_error.cc ../sql/sql_handler.cc
            ../sql/sql_get_diagnostics.cc
           ../sql/sql_help.cc ../sql/sql_insert.cc ../sql/datadict.cc
           ../sql/sql_admin.cc ../sql/sql_truncate.cc ../sql/sql_reload.cc
           ../sql/sql_lex.cc ../sql/keycaches.cc
           ../sql/sql_list.cc ../sql/sql_load.cc ../sql/sql_locale.cc 
           ../sql/sql_binlog.cc ../sql/sql_manager.cc
           ../sql/sql_parse.cc ../sql/sql_bootstrap.cc
           ../sql/sql_partition.cc ../sql/sql_plugin.cc 
           ../sql/debug_sync.cc ../sql/debug.cc
           ../sql/opt_table_elimination.cc
           ../sql/sql_prepare.cc ../sql/sql_rename.cc ../sql/sql_repl.cc 
           ../sql/sql_select.cc ../sql/sql_servers.cc
           ../sql/group_by_handler.cc ../sql/derived_handler.cc
           ../sql/select_handler.cc
           ../sql/sql_show.cc ../sql/sql_state.c 
           ../sql/sql_statistics.cc ../sql/sql_string.cc
           ../sql/sql_table.cc ../sql/sql_test.cc
           ../sql/ddl_log.cc
           ../sql/sql_trigger.cc ../sql/sql_udf.cc ../sql/sql_union.cc
           ../sql/sql_update.cc ../sql/sql_view.cc ../sql/sql_profile.cc
           ../sql/gcalc_tools.cc ../sql/gcalc_slicescan.cc
           ../sql/strfunc.cc ../sql/table.cc ../sql/thr_malloc.cc
           ../sql/sql_time.cc ../sql/tztime.cc ../sql/uniques.cc ../sql/unireg.cc
           ../sql/partition_info.cc ../sql/sql_connect.cc 
           ../sql/scheduler.cc ../sql/sql_audit.cc
           ../sql/sql_alter.cc ../sql/sql_partition_admin.cc
           ../sql/event_parse_data.cc
           ../sql/sql_signal.cc
           ../sql/sys_vars.cc
           ${CMAKE_BINARY_DIR}/sql/sql_builtin.cc
           ../sql/mdl.cc ../sql/transaction.cc
           ../sql/sql_join_cache.cc
           ../sql/multi_range_read.cc
           ../sql/opt_index_cond_pushdown.cc
           ../sql/opt_subselect.cc
           ../sql/create_options.cc
           ../sql/rpl_utility.cc
           ../sql/rpl_utility_server.cc
           ../sql/rpl_reporting.cc
           ../sql/sql_expression_cache.cc
           ../sql/my_apc.cc ../sql/my_apc.h
           ../sql/my_json_writer.cc ../sql/my_json_writer.h
	   ../sql/rpl_gtid.cc
           ../sql/sql_explain.cc ../sql/sql_explain.h
           ../sql/sql_analyze_stmt.cc ../sql/sql_analyze_stmt.h
           ../sql/compat56.cc
           ../sql/sql_schema.cc
           ../sql/lex_charset.cc
           ../sql/sql_type.cc ../sql/sql_type.h
           ../sql/sql_mode.cc
           ../sql/sql_type_string.cc
           ../sql/sql_type_json.cc
           ../sql/sql_type_geom.cc
           ../sql/table_cache.cc ../sql/mf_iocache_encr.cc
           ../sql/wsrep_dummy.cc ../sql/encryption.cc
           ../sql/item_windowfunc.cc ../sql/sql_window.cc
           ../sql/sql_cte.cc
           ../sql/sql_sequence.cc ../sql/sql_sequence.h
           ../sql/ha_sequence.cc ../sql/ha_sequence.h
           ../sql/temporary_tables.cc
           ../sql/proxy_protocol.cc ../sql/backup.cc
           ../sql/sql_tvc.cc ../sql/sql_tvc.h
           ../sql/opt_split.cc
           ../sql/rowid_filter.cc ../sql/rowid_filter.h
           ../sql/item_vers.cc
           ../sql/opt_trace.cc
           ../sql/xa.cc
           ../sql/json_table.cc
           ../sql/opt_histogram_json.cc
           ${GEN_SOURCES}
           ${MYSYS_LIBWRAP_SOURCE}
)


ADD_CONVENIENCE_LIBRARY(sql_embedded ${SQL_EMBEDDED_SOURCES})
DTRACE_INSTRUMENT(sql_embedded)
ADD_DEPENDENCIES(sql_embedded GenError GenServerSource)
IF(TARGET pcre2)
  ADD_DEPENDENCIES(sql_embedded pcre2)
ENDIF()
IF(TARGET libfmt)
  ADD_DEPENDENCIES(sql_embedded libfmt)
ENDIF()

# On Windows, static embedded server library is called mysqlserver.lib
# On Unix, it is libmysqld.a
IF(WIN32)
  SET(MYSQLSERVER_OUTPUT_NAME mysqlserver)
  SET(COMPONENT_MYSQLSERVER "Embedded")
  SET(COMPONENT_LIBMYSQLD "Embedded")
ELSE()
  SET(MYSQLSERVER_OUTPUT_NAME mariadbd)
  SET(COMPONENT_MYSQLSERVER "Development")
  SET(COMPONENT_LIBMYSQLD "Server")
ENDIF()


SET(LIBS 
  dbug strings mysys mysys_ssl pcre2-8 vio
  ${ZLIB_LIBRARY} ${SSL_LIBRARIES} 
  ${LIBWRAP} ${LIBCRYPT} ${CMAKE_DL_LIBS}
  ${EMBEDDED_PLUGIN_LIBS}
  sql_embedded
)

# Some storage engine were compiled for embedded specifically
# (with corresponding target ${engine}_embedded)
SET(EMBEDDED_LIBS)
FOREACH(LIB ${LIBS})
  IF(TARGET ${LIB}_embedded)
    LIST(APPEND EMBEDDED_LIBS ${LIB}_embedded)
  ELSE()
    LIST(APPEND EMBEDDED_LIBS ${LIB})
  ENDIF()
ENDFOREACH()

MERGE_LIBRARIES(mysqlserver STATIC ${EMBEDDED_LIBS} 
  OUTPUT_NAME ${MYSQLSERVER_OUTPUT_NAME} COMPONENT ${COMPONENT_MYSQLSERVER})
IF(UNIX)
  INSTALL_SYMLINK(libmysqld.a mysqlserver ${INSTALL_LIBDIR} ${COMPONENT_MYSQLSERVER})
ENDIF()
INSTALL(FILES embedded_priv.h DESTINATION ${INSTALL_INCLUDEDIR}/server/private COMPONENT  ${COMPONENT_MYSQLSERVER})


SET(CLIENT_API_FUNCTIONS_5_1
get_tty_password
mysql_thread_end
mysql_thread_init
myodbc_remove_escape
mysql_affected_rows
mysql_autocommit
mysql_stmt_bind_param
mysql_stmt_bind_result
mysql_change_user
mysql_character_set_name
mysql_close
mysql_commit
mysql_data_seek
mysql_debug
mysql_dump_debug_info
mysql_eof
mysql_errno
mysql_error
mysql_escape_string
mysql_hex_string
mysql_stmt_execute
mysql_stmt_fetch
mysql_stmt_fetch_column
mysql_fetch_field
mysql_fetch_field_direct
mysql_fetch_fields
mysql_fetch_lengths
mysql_fetch_row
mysql_field_count
mysql_field_seek
mysql_field_tell
mysql_free_result
mysql_get_parameters
mysql_get_client_info
mysql_get_host_info
mysql_get_proto_info
mysql_get_server_info
mysql_get_client_version
mysql_get_ssl_cipher
mysql_info
mysql_init
mysql_insert_id
mysql_kill
mysql_set_server_option
mysql_list_dbs
mysql_list_fields
mysql_list_processes
mysql_list_tables
mysql_more_results
mysql_next_result
mysql_num_fields
mysql_num_rows
mysql_options
mysql_stmt_param_count
mysql_stmt_param_metadata
mysql_ping
mysql_stmt_result_metadata
mysql_query
mysql_read_query_result
mysql_real_connect
mysql_real_escape_string
mysql_real_query
mysql_refresh
mysql_rollback
mysql_row_seek
mysql_row_tell
mysql_select_db
mysql_stmt_send_long_data
mysql_send_query
mysql_shutdown
mysql_ssl_set
mysql_stat
mysql_stmt_affected_rows
mysql_stmt_close
mysql_stmt_reset
mysql_stmt_data_seek
mysql_stmt_errno
mysql_stmt_error
mysql_stmt_free_result
mysql_stmt_num_rows
mysql_stmt_row_seek
mysql_stmt_row_tell
mysql_stmt_store_result
mysql_store_result
mysql_thread_id
mysql_thread_safe
mysql_use_result
mysql_warning_count
mysql_stmt_sqlstate
mysql_sqlstate
mysql_get_server_version
mysql_stmt_prepare
mysql_stmt_init
mysql_stmt_insert_id
mysql_stmt_attr_get
mysql_stmt_attr_set
mysql_stmt_field_count
mysql_set_local_infile_default
mysql_set_local_infile_handler
mysql_embedded
mysql_server_init
mysql_server_end
mysql_set_character_set
mysql_get_character_set_info
# These are documented in Paul DuBois' MySQL book,
# so we treat them as part of the de-facto API.
handle_options
load_defaults
free_defaults
my_print_help
)

SET(CLIENT_API_FUNCTIONS_5_5
my_progname
mysql_stmt_next_result
# Charsets
my_charset_bin
my_charset_latin1
my_charset_utf8mb3_general_ci
# Client plugins
mysql_client_find_plugin
mysql_client_register_plugin
mysql_load_plugin
mysql_load_plugin_v
mysql_plugin_options

#dynamic columns api
dynamic_column_create
dynamic_column_create_many
dynamic_column_update
dynamic_column_update_many
dynamic_column_exists
dynamic_column_list
dynamic_column_get
dynamic_column_prepare_decimal
mariadb_dyncol_create_many_num
mariadb_dyncol_create_many_named
mariadb_dyncol_update_many_num
mariadb_dyncol_update_many_named
mariadb_dyncol_exists_num
mariadb_dyncol_exists_named
mariadb_dyncol_free
mariadb_dyncol_list_num
mariadb_dyncol_list_named
mariadb_dyncol_get_num
mariadb_dyncol_get_named
mariadb_dyncol_has_names
mariadb_dyncol_check
mariadb_dyncol_json
mariadb_dyncol_val_str
mariadb_dyncol_val_long
mariadb_dyncol_val_double
mariadb_dyncol_unpack
mariadb_dyncol_unpack_free
mariadb_dyncol_column_cmp_named
mariadb_dyncol_column_count
mariadb_dyncol_prepare_decimal
#
mariadb_deinitialize_ssl
# low-level API to MySQL protocol
mysql_net_read_packet
mysql_net_field_length
# Added in MariaDB-10.0 to stay compatible with MySQL-5.6, yuck!
mysql_options4
)


SET(CLIENT_API_FUNCTIONS_10_5
mariadb_field_attr
)

SET(CLIENT_API_FUNCTIONS
  ${CLIENT_API_FUNCTIONS_5_1}
  ${CLIENT_API_FUNCTIONS_5_5}
  ${CLIENT_API_FUNCTIONS_10_5}
)


# List of exported functions in embedded (client api except client plugin or 
# async (*_start/*_cont functions)

SET(EMBEDDED_API)

FOREACH(f ${CLIENT_API_FUNCTIONS})
  IF(f MATCHES "plugin|_start$|_cont$")
    # Ignore functions, embedded does not export them
  ELSE()
    SET(EMBEDDED_API ${EMBEDDED_API} ${f})
  ENDIF()	
ENDFOREACH()

IF(NOT DISABLE_SHARED)
  MERGE_LIBRARIES(libmysqld SHARED mysqlserver EXPORTS ${EMBEDDED_API}
    COMPONENT  ${COMPONENT_LIBMYSQLD})
  IF(UNIX)
    # Name the shared library, handle versioning (provides same api as client
    # library hence the same version)
    SET_TARGET_PROPERTIES(libmysqld PROPERTIES 
      OUTPUT_NAME mariadbd
      SOVERSION "${SHARED_LIB_MAJOR_VERSION}")
    INSTALL_SYMLINK(libmysqld.so libmysqld ${INSTALL_LIBDIR} ${COMPONENT_LIBMYSQLD})
    # Clean direct output flags, as 2 targets have the same base name
    # libmysqld
    SET_TARGET_PROPERTIES(libmysqld PROPERTIES CLEAN_DIRECT_OUTPUT 1)
    SET_TARGET_PROPERTIES(mysqlserver PROPERTIES CLEAN_DIRECT_OUTPUT 1)
    TARGET_LINK_LIBRARIES(mysqlserver LINK_PRIVATE tpool ${CRC32_LIBRARY})
    IF(LIBMYSQLD_SO_EXTRA_LIBS)
       TARGET_LINK_LIBRARIES(libmysqld LINK_PRIVATE ${LIBMYSQLD_SO_EXTRA_LIBS})
    ENDIF()
  ENDIF()
ENDIF()
