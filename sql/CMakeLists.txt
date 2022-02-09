# Copyright (c) 2006, 2014, Oracle and/or its affiliates.
# Copyright (c) 2010, 2022, MariaDB Corporation.
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


IF(WITH_WSREP AND NOT EMBEDDED_LIBRARY)
  SET(WSREP_SOURCES
    wsrep_client_service.cc
    wsrep_high_priority_service.cc
    wsrep_server_service.cc
    wsrep_storage_service.cc
    wsrep_server_state.cc
    wsrep_status.cc
    wsrep_utils.cc
    wsrep_xid.cc
    wsrep_check_opts.cc
    wsrep_mysqld.cc
    wsrep_notify.cc
    wsrep_sst.cc
    wsrep_var.cc
    wsrep_binlog.cc
    wsrep_applier.cc
    wsrep_thd.cc
    wsrep_schema.cc
    wsrep_plugin.cc
    service_wsrep.cc
  )
  MYSQL_ADD_PLUGIN(wsrep ${WSREP_SOURCES} MANDATORY NOT_EMBEDDED LINK_LIBRARIES wsrep-lib wsrep_api_v26)
  IF(VISIBILITY_HIDDEN_FLAG AND TARGET wsrep)
    # wsrep_info plugin needs some wsrep symbols from inside mysqld
    # we have to remove -fvisibility=hidden from wsrep
    GET_TARGET_PROPERTY(f wsrep COMPILE_FLAGS)
    STRING(REPLACE "${VISIBILITY_HIDDEN_FLAG}" "" f ${f})
    SET_TARGET_PROPERTIES(wsrep PROPERTIES COMPILE_FLAGS "${f}")
  ENDIF()
ELSE()
  ADD_LIBRARY(wsrep STATIC wsrep_dummy.cc)
  ADD_DEPENDENCIES(wsrep GenError)
ENDIF()

INCLUDE_DIRECTORIES(
${CMAKE_SOURCE_DIR}/include
${CMAKE_SOURCE_DIR}/sql
${LIBFMT_INCLUDE_DIR}
${PCRE_INCLUDES}
${ZLIB_INCLUDE_DIR}
${SSL_INCLUDE_DIRS}
${CMAKE_BINARY_DIR}/sql
${CMAKE_SOURCE_DIR}/tpool
)

ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lex_token.h
  COMMAND gen_lex_token > lex_token.h
  DEPENDS gen_lex_token
)

FIND_PACKAGE(BISON 2.4)

ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.yy
         ${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.yy
  COMMAND ${CMAKE_COMMAND} "-DVAL1=ORACLE" "-DVAL2=MARIADB"
   "-DOUT1=${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.yy"
   "-DOUT2=${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.yy"
   "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/sql_yacc.yy"
    "-DBISON_VERSION=${BISON_VERSION}"
   -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_yy_files.cmake
  COMMENT "Building yy_mariadb.yy and yy_oracle.yy from sql_yacc.yy"
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sql_yacc.yy
          ${CMAKE_CURRENT_SOURCE_DIR}/gen_yy_files.cmake
)

ADD_DEFINITIONS(-DMYSQL_SERVER -DHAVE_EVENT_SCHEDULER)

IF(SSL_DEFINES)
 ADD_DEFINITIONS(${SSL_DEFINES})
ENDIF()

SET (SQL_SOURCE
               ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.cc
               ${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.cc
              ../sql-common/client.c compat56.cc derror.cc des_key_file.cc
               discover.cc ../sql-common/errmsg.c
               field.cc field_conv.cc field_comp.cc
               filesort_utils.cc
               filesort.cc gstream.cc
               signal_handler.cc
               handler.cc
               hostname.cc init.cc item.cc item_buff.cc item_cmpfunc.cc
               item_create.cc item_func.cc item_geofunc.cc item_row.cc
               item_strfunc.cc item_subselect.cc item_sum.cc item_timefunc.cc
               key.cc log.cc lock.cc
               log_event.cc log_event_server.cc
               rpl_record.cc rpl_reporting.cc
               log_event_old.cc rpl_record_old.cc
               mf_iocache.cc my_decimal.cc
               mysqld.cc net_serv.cc  keycaches.cc
               ../sql-common/client_plugin.c
               opt_range.cc opt_sum.cc
               ../sql-common/pack.c parse_file.cc password.c procedure.cc
               protocol.cc records.cc repl_failsafe.cc rpl_filter.cc
               session_tracker.cc
               set_var.cc
               slave.cc sp.cc sp_cache.cc sp_head.cc sp_pcontext.cc
               sp_rcontext.cc spatial.cc sql_acl.cc sql_analyse.cc sql_base.cc
               sql_cache.cc sql_class.cc sql_client.cc sql_crypt.cc
               sql_cursor.cc sql_db.cc sql_delete.cc sql_derived.cc
               sql_digest.cc sql_do.cc
               sql_error.cc sql_handler.cc sql_get_diagnostics.cc
               sql_help.cc sql_insert.cc sql_lex.cc
               sql_list.cc sql_load.cc sql_manager.cc
               sql_parse.cc sql_bootstrap.cc
               sql_partition.cc sql_plugin.cc sql_prepare.cc sql_rename.cc
               debug_sync.cc debug.cc
               sql_repl.cc sql_select.cc sql_show.cc sql_state.c
               group_by_handler.cc derived_handler.cc select_handler.cc
               sql_statistics.cc sql_string.cc lex_string.h
               sql_table.cc sql_test.cc sql_trigger.cc sql_udf.cc sql_union.cc
               ddl_log.cc ddl_log.h
               sql_update.cc sql_view.cc strfunc.cc table.cc thr_malloc.cc
               sql_time.cc tztime.cc unireg.cc item_xmlfunc.cc
               uniques.cc
               rpl_tblmap.cc sql_binlog.cc event_scheduler.cc
               event_data_objects.cc
               event_queue.cc event_db_repository.cc
               events.cc ../sql-common/my_user.c
               partition_info.cc rpl_utility.cc rpl_utility_server.cc
               rpl_injector.cc sql_locale.cc
               rpl_rli.cc rpl_mi.cc sql_servers.cc sql_audit.cc
               sql_connect.cc scheduler.cc sql_partition_admin.cc
               sql_profile.cc event_parse_data.cc sql_alter.cc
               sql_signal.cc mdl.cc sql_admin.cc
               transaction.cc sys_vars.cc sql_truncate.cc datadict.cc
               sql_reload.cc

               # added in MariaDB:
               grant.cc
               sql_explain.cc
               sql_analyze_stmt.cc
               sql_join_cache.cc
               create_options.cc multi_range_read.cc
               opt_histogram_json.cc
               opt_index_cond_pushdown.cc opt_subselect.cc
               opt_table_elimination.cc sql_expression_cache.cc
               gcalc_slicescan.cc gcalc_tools.cc
               my_apc.cc mf_iocache_encr.cc item_jsonfunc.cc
               my_json_writer.cc
               rpl_gtid.cc rpl_parallel.cc
               semisync.cc semisync_master.cc semisync_slave.cc
               semisync_master_ack_receiver.cc
               sql_schema.cc
               lex_charset.cc
               sql_type.cc sql_mode.cc sql_type_json.cc
               sql_type_string.cc
               sql_type_geom.cc
               item_windowfunc.cc sql_window.cc
	       sql_cte.cc
               item_vers.cc
               sql_sequence.cc sql_sequence.h ha_sequence.h
               sql_tvc.cc sql_tvc.h
               opt_split.cc
               rowid_filter.cc rowid_filter.h
               opt_trace.cc
               table_cache.cc encryption.cc temporary_tables.cc
               json_table.cc
               proxy_protocol.cc backup.cc xa.cc
               ${CMAKE_CURRENT_BINARY_DIR}/lex_hash.h
               ${CMAKE_CURRENT_BINARY_DIR}/lex_token.h
               ${GEN_SOURCES}
               ${MYSYS_LIBWRAP_SOURCE}
)

IF ((CMAKE_SYSTEM_NAME MATCHES "Linux" OR
     CMAKE_SYSTEM_NAME MATCHES "SunOS" OR
     WIN32 OR
     HAVE_KQUEUE)
    AND (NOT DISABLE_THREADPOOL))
 ADD_DEFINITIONS(-DHAVE_POOL_OF_THREADS)
 IF(WIN32)
   SET(SQL_SOURCE ${SQL_SOURCE} threadpool_win.cc threadpool_winsockets.cc threadpool_winsockets.h)
 ENDIF()
 SET(SQL_SOURCE ${SQL_SOURCE} threadpool_generic.cc)
 SET(SQL_SOURCE ${SQL_SOURCE} threadpool_common.cc)
 MYSQL_ADD_PLUGIN(thread_pool_info thread_pool_info.cc DEFAULT STATIC_ONLY NOT_EMBEDDED)
ENDIF()

IF(WIN32)
  SET(SQL_SOURCE ${SQL_SOURCE} handle_connections_win.cc winmain.cc)
ENDIF()

MYSQL_ADD_PLUGIN(partition ha_partition.cc STORAGE_ENGINE DEFAULT STATIC_ONLY
RECOMPILE_FOR_EMBEDDED)
MYSQL_ADD_PLUGIN(sql_sequence ha_sequence.cc STORAGE_ENGINE MANDATORY STATIC_ONLY
RECOMPILE_FOR_EMBEDDED)

ADD_LIBRARY(sql STATIC ${SQL_SOURCE})
MAYBE_DISABLE_IPO(sql)
DTRACE_INSTRUMENT(sql)
TARGET_LINK_LIBRARIES(sql
  mysys mysys_ssl dbug strings vio pcre2-8
  tpool
  ${LIBWRAP} ${LIBCRYPT} ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT}
  ${SSL_LIBRARIES}
  ${LIBSYSTEMD})

IF(TARGET pcre2)
  ADD_DEPENDENCIES(sql pcre2)
ENDIF()

FOREACH(se aria partition perfschema sql_sequence wsrep)
  # These engines are used directly in sql sources.
  IF(TARGET ${se})
    TARGET_LINK_LIBRARIES(sql ${se})
  ENDIF()
ENDFOREACH()

IF(WIN32)
  SET(MYSQLD_SOURCE main.cc  message.rc)
ELSE()
  SET(MYSQLD_SOURCE main.cc ${DTRACE_PROBES_ALL})
ENDIF()

IF(MSVC OR CMAKE_SYSTEM_NAME MATCHES AIX)
  SET(libs_to_export_symbols sql mysys dbug strings)
  # Create shared library of already compiled object
  # Export all symbols from selected libraries, to be used
  # by plugins
  IF(MSVC)
    SET(VERSIONINFO_RC ${PROJECT_BINARY_DIR}/versioninfo_dll.rc)
  ELSE()
    SET(VERSIONINFO_RC)
  ENDIF()
  ADD_LIBRARY(server SHARED
    $<TARGET_OBJECTS:sql>
    $<TARGET_OBJECTS:mysys>
    $<TARGET_OBJECTS:dbug>
    $<TARGET_OBJECTS:strings>
    ${VERSIONINFO_RC}
  )

  # We need to add all dependencies of sql/mysys/dbug/strings
  # to link the shared library
  SET(all_deps)
  FOREACH(lib ${libs_to_export_symbols})
    GET_TARGET_PROPERTY(deps ${lib} LINK_LIBRARIES)
    IF(deps)
      LIST(APPEND all_deps ${deps})
    ENDIF()
  ENDFOREACH()
  LIST(REMOVE_DUPLICATES all_deps)
  FOREACH(lib ${libs_to_export_symbols})
    LIST(REMOVE_ITEM all_deps ${lib})
  ENDFOREACH()

  TARGET_LINK_LIBRARIES(server PRIVATE
   ${all_deps}
   sql_builtins
  )
  IF(MSVC)
    IF(NOT WITHOUT_DYNAMIC_PLUGINS)
     SET_TARGET_PROPERTIES(server PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
    ENDIF()
  ELSE()
    SET_TARGET_PROPERTIES(server PROPERTIES AIX_EXPORT_ALL_SYMBOLS TRUE)
  ENDIF()
  MYSQL_INSTALL_TARGETS(server DESTINATION  ${INSTALL_BINDIR} COMPONENT Server)
ENDIF()

ADD_LIBRARY(sql_builtins STATIC ${CMAKE_CURRENT_BINARY_DIR}/sql_builtin.cc)
TARGET_LINK_LIBRARIES(sql_builtins ${MYSQLD_STATIC_PLUGIN_LIBS})

MYSQL_ADD_EXECUTABLE(mariadbd ${MYSQLD_SOURCE} DESTINATION ${INSTALL_SBINDIR} COMPONENT Server)

IF(APPLE)
  # Add CoreServices framework since some dloadable plugins may need it
  FIND_LIBRARY(CORESERVICES NAMES CoreServices)
  IF(CORESERVICES)
    TARGET_LINK_LIBRARIES(mariadbd LINK_PRIVATE ${CORESERVICES})
  ENDIF()
ENDIF()

IF(NOT WITHOUT_DYNAMIC_PLUGINS)
  IF(NOT MSVC)
    SET_TARGET_PROPERTIES(mariadbd PROPERTIES ENABLE_EXPORTS TRUE)
  ENDIF()
  GET_TARGET_PROPERTY(mysqld_link_flags mariadbd LINK_FLAGS)
  IF(NOT mysqld_link_flags)
    SET(mysqld_link_flags)
  ENDIF()
ENDIF(NOT WITHOUT_DYNAMIC_PLUGINS)

IF(MSVC OR CMAKE_SYSTEM_NAME MATCHES AIX)
  TARGET_LINK_LIBRARIES(mariadbd server)
ELSE()
  TARGET_LINK_LIBRARIES(mariadbd LINK_PRIVATE sql sql_builtins)
ENDIF()

# Provide plugins with minimal set of libraries
SET(INTERFACE_LIBS ${LIBRT})
IF(INTERFACE_LIBS)
  TARGET_LINK_LIBRARIES(mariadbd LINK_PUBLIC ${INTERFACE_LIBS})
ENDIF()

# On Solaris, some extra effort is required in order to get dtrace probes
# from static libraries
DTRACE_INSTRUMENT_STATIC_LIBS(mariadbd
 "sql;mysys;mysys_ssl;${MYSQLD_STATIC_PLUGIN_LIBS}")


SET(WITH_MYSQLD_LDFLAGS "" CACHE STRING "Additional linker flags for mysqld")
MARK_AS_ADVANCED(WITH_MYSQLD_LDFLAGS)
IF(WITH_MYSQLD_LDFLAGS)
  GET_TARGET_PROPERTY(MYSQLD_LINK_FLAGS mariadbd LINK_FLAGS)
  IF(NOT MYSQLD_LINK_FLAGS)
    SET(MYSQLD_LINK_FLAGS)
  ENDIF()
  SET_TARGET_PROPERTIES(mariadbd PROPERTIES LINK_FLAGS
     "${MYSQLD_LINK_FLAGS} ${WITH_MYSQLD_LDFLAGS}")
ENDIF()


# Handle out-of-source build from source package with possibly broken
# bison. Copy bison output to from source to build directory, if not already
# there
IF (NOT BISON_FOUND)
  IF (NOT ${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_BINARY_DIR})
    FOREACH(file yy_mariadb.cc yy_mariadb.hh yy_oracle.cc yy_oracle.hh)
    IF(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${file} AND (NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${file}))
        CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${file}
        ${CMAKE_CURRENT_BINARY_DIR}/${file} COPYONLY)
    ENDIF()
    ENDFOREACH()
  ENDIF()

  IF(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.cc)
     # Output files are missing, bail out.
      SET(ERRMSG
         "Bison (GNU parser generator) is required to build MySQL."
         "Please install bison."
      )
      IF(WIN32)
       SET(ERRMSG ${ERRMSG}
       "You can download bison from http://gnuwin32.sourceforge.net/packages/bison.htm "
       "Choose 'Complete package, except sources' installation. We recommend to "
       "install bison into a directory without spaces, e.g C:\\GnuWin32.")
      ENDIF()
      MESSAGE(FATAL_ERROR ${ERRMSG})
  ENDIF()
ELSE()
  CONFIGURE_FILE(myskel.m4.in myskel.m4)
  BISON_TARGET(gen_mariadb_cc_hh ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.yy
                                 ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.cc
               COMPILE_FLAGS "-p MYSQL -S ${CMAKE_CURRENT_BINARY_DIR}/myskel.m4")
  BISON_TARGET(gen_oracle_cc_hh ${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.yy
                                ${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.cc
               COMPILE_FLAGS "-p ORA -S ${CMAKE_CURRENT_BINARY_DIR}/myskel.m4")
ENDIF()

IF(NOT CMAKE_CROSSCOMPILING OR DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
  ADD_EXECUTABLE(gen_lex_token gen_lex_token.cc
   ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.hh)
  ADD_EXECUTABLE(gen_lex_hash gen_lex_hash.cc)
ENDIF()

ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lex_hash.h
  COMMAND gen_lex_hash > lex_hash.h
  DEPENDS gen_lex_hash
)

MYSQL_ADD_EXECUTABLE(mariadb-tzinfo-to-sql tztime.cc)
SET_TARGET_PROPERTIES(mariadb-tzinfo-to-sql PROPERTIES COMPILE_FLAGS "-DTZINFO2SQL")
TARGET_LINK_LIBRARIES(mariadb-tzinfo-to-sql mysys mysys_ssl)

ADD_CUSTOM_TARGET(
        GenServerSource
        DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/lex_hash.h
        ${CMAKE_CURRENT_BINARY_DIR}/lex_token.h
        ${CMAKE_CURRENT_BINARY_DIR}/yy_mariadb.cc
        ${CMAKE_CURRENT_BINARY_DIR}/yy_oracle.cc
)
ADD_DEPENDENCIES(sql GenServerSource)

IF(TARGET libfmt)
  ADD_DEPENDENCIES(sql libfmt)
ENDIF()

IF(WIN32 OR HAVE_DLOPEN AND NOT DISABLE_SHARED)
  ADD_LIBRARY(udf_example MODULE udf_example.c udf_example.def)
  SET_TARGET_PROPERTIES(udf_example PROPERTIES PREFIX "")
  TARGET_LINK_LIBRARIES(udf_example strings)
ENDIF()

CONFIGURE_FILE(
  ${CMAKE_SOURCE_DIR}/cmake/make_dist.cmake.in
  ${CMAKE_BINARY_DIR}/make_dist.cmake @ONLY)

ADD_CUSTOM_TARGET(dist
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/make_dist.cmake
  DEPENDS ${CMAKE_BINARY_DIR}/sql/yy_mariadb.cc ${CMAKE_BINARY_DIR}/sql/yy_mariadb.hh
  DEPENDS ${CMAKE_BINARY_DIR}/sql/yy_oracle.cc ${CMAKE_BINARY_DIR}/sql/yy_oracle.hh
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

ADD_CUSTOM_TARGET(distclean
  COMMAND ${CMAKE_COMMAND} -E echo  WARNING: distclean target is not functional
  COMMAND ${CMAKE_COMMAND} -E echo  Use 'git clean -Xdf' instead
  VERBATIM
  )

# Install initial database (default on windows, optional target elsewhere)
IF(TARGET mariadbd AND (NOT CMAKE_CROSSCOMPILING OR DEFINED CMAKE_CROSSCOMPILING_EMULATOR))
  IF(GENERATOR_IS_MULTI_CONFIG)
    SET (CONFIG_PARAM -DCONFIG=${CMAKE_CFG_INTDIR})
  ENDIF()
  MAKE_DIRECTORY(${CMAKE_CURRENT_BINARY_DIR}/data)
  ADD_CUSTOM_COMMAND(
     OUTPUT initdb.dep
     COMMAND ${CMAKE_COMMAND} -E remove_directory data
     COMMAND ${CMAKE_COMMAND} -E make_directory data
     COMMAND ${CMAKE_COMMAND} -E chdir data ${CMAKE_COMMAND}
     ${CONFIG_PARAM}
     -DTOP_SRCDIR="${CMAKE_BINARY_DIR}"
     -DBINDIR="${CMAKE_CURRENT_BINARY_DIR}"
     -DMYSQLD_EXECUTABLE="$<TARGET_FILE:mariadbd>"
     -DCMAKE_CFG_INTDIR="${CMAKE_CFG_INTDIR}"
     -P ${CMAKE_SOURCE_DIR}/cmake/create_initial_db.cmake
     COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/initdb.dep
     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/
     DEPENDS mariadbd
  )
  IF(WIN32)
    SET(ALL_ON_WINDOWS ALL)
  ELSE()
    SET(ALL_ON_WINDOWS)
  ENDIF()
  ADD_CUSTOM_TARGET(initial_database
    ${ALL_ON_WINDOWS}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/initdb.dep
  )
ENDIF()

IF(WIN32)
  SET(my_bootstrap_sql ${CMAKE_CURRENT_BINARY_DIR}/my_bootstrap.sql)
  FILE(TO_NATIVE_PATH ${my_bootstrap_sql} native_outfile)

  # Create bootstrapper SQL script
  ADD_CUSTOM_COMMAND(OUTPUT
    ${my_bootstrap_sql}
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/scripts
    cmd /c copy mysql_system_tables.sql+mysql_system_tables_data.sql+fill_help_tables.sql+mysql_performance_tables.sql+mysql_test_db.sql+mysql_sys_schema.sql ${native_outfile}
    DEPENDS
    ${CMAKE_SOURCE_DIR}/scripts/mysql_system_tables.sql
    ${CMAKE_SOURCE_DIR}/scripts/mysql_system_tables_data.sql
    ${CMAKE_SOURCE_DIR}/scripts/fill_help_tables.sql
    ${CMAKE_SOURCE_DIR}/scripts/mysql_performance_tables.sql
    ${CMAKE_SOURCE_DIR}/scripts/mysql_test_db.sql
    ${CMAKE_BINARY_DIR}/scripts/mysql_sys_schema.sql
  )

  ADD_CUSTOM_COMMAND(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mysql_bootstrap_sql.c
    COMMAND comp_sql
    mysql_bootstrap_sql
    ${CMAKE_CURRENT_BINARY_DIR}/my_bootstrap.sql
    mysql_bootstrap_sql.c
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS comp_sql ${my_bootstrap_sql}
  )

  MYSQL_ADD_EXECUTABLE(mariadb-install-db
    mysql_install_db.cc
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_bootstrap_sql.c
    password.c
    COMPONENT Server
  )
  SET_TARGET_PROPERTIES(mariadb-install-db PROPERTIES COMPILE_DEFINITIONS
   "INSTALL_PLUGINDIR=${INSTALL_PLUGINDIR};INSTALL_SHAREDIR=${INSTALL_SHAREDIR}")
  TARGET_LINK_LIBRARIES(mariadb-install-db mysys mysys_ssl shlwapi)

  ADD_LIBRARY(winservice STATIC winservice.c)
  TARGET_LINK_LIBRARIES(winservice shell32)

  MYSQL_ADD_EXECUTABLE(mariadb-upgrade-service
    mysql_upgrade_service.cc
    upgrade_conf_file.cc
    COMPONENT Server)
  TARGET_LINK_LIBRARIES(mariadb-upgrade-service mysys winservice)
ENDIF(WIN32)

INSTALL(DIRECTORY . DESTINATION ${INSTALL_INCLUDEDIR}/server/private COMPONENT Development
  FILES_MATCHING PATTERN "*.h"
  PATTERN share EXCLUDE
  PATTERN CMakeFiles EXCLUDE)
