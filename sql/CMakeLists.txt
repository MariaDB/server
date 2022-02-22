# Copyright (c) 2006, 2014, Oracle and/or its affiliates.
# Copyright (c) 2010, 2018, MariaDB Corporation
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
  MYSQL_ADD_PLUGIN(wsrep ${WSREP_SOURCES} MANDATORY NOT_EMBEDDED EXPORT_SYMBOLS LINK_LIBRARIES wsrep-lib wsrep_api_v26)
ELSE()
  ADD_LIBRARY(wsrep STATIC wsrep_dummy.cc)
  ADD_DEPENDENCIES(wsrep GenError)
ENDIF()

INCLUDE_DIRECTORIES(
${CMAKE_SOURCE_DIR}/include
${CMAKE_SOURCE_DIR}/sql 
${PCRE_INCLUDES}
${ZLIB_INCLUDE_DIR}
${SSL_INCLUDE_DIRS}
${CMAKE_BINARY_DIR}/sql
)






ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lex_token.h
  COMMAND gen_lex_token > lex_token.h
  DEPENDS gen_lex_token
)

ADD_DEFINITIONS(-DMYSQL_SERVER -DHAVE_EVENT_SCHEDULER)

IF(SSL_DEFINES)
 ADD_DEFINITIONS(${SSL_DEFINES})
ENDIF()

SET (SQL_SOURCE
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
               log_event.cc rpl_record.cc rpl_reporting.cc
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
               debug_sync.cc
               sql_repl.cc sql_select.cc sql_show.cc sql_state.c
               group_by_handler.cc derived_handler.cc select_handler.cc
               sql_statistics.cc sql_string.cc lex_string.h
               sql_table.cc sql_test.cc sql_trigger.cc sql_udf.cc sql_union.cc
               sql_update.cc sql_view.cc strfunc.cc table.cc thr_malloc.cc 
               sql_time.cc tztime.cc unireg.cc item_xmlfunc.cc 
               uniques.cc
               rpl_tblmap.cc sql_binlog.cc event_scheduler.cc event_data_objects.cc
               event_queue.cc event_db_repository.cc 
               sql_tablespace.cc events.cc ../sql-common/my_user.c 
               partition_info.cc rpl_utility.cc rpl_injector.cc sql_locale.cc
               rpl_rli.cc rpl_mi.cc sql_servers.cc sql_audit.cc
               sql_connect.cc scheduler.cc sql_partition_admin.cc
               sql_profile.cc event_parse_data.cc sql_alter.cc
               sql_signal.cc mdl.cc sql_admin.cc
               transaction.cc sys_vars.cc sql_truncate.cc datadict.cc
               sql_reload.cc item_inetfunc.cc

               # added in MariaDB:
               sql_explain.cc
               sql_analyze_stmt.cc
               sql_join_cache.cc
               create_options.cc multi_range_read.cc
               opt_index_cond_pushdown.cc opt_subselect.cc
               opt_table_elimination.cc sql_expression_cache.cc
               gcalc_slicescan.cc gcalc_tools.cc
               ../sql-common/mysql_async.c
               my_apc.cc mf_iocache_encr.cc item_jsonfunc.cc
               my_json_writer.cc
               rpl_gtid.cc rpl_parallel.cc
               semisync.cc semisync_master.cc semisync_slave.cc
               semisync_master_ack_receiver.cc
               sql_schema.cc
               sql_type.cc sql_mode.cc sql_type_json.cc
               item_windowfunc.cc sql_window.cc
	       sql_cte.cc
               item_vers.cc
               sql_sequence.cc sql_sequence.h ha_sequence.h
               sql_tvc.cc sql_tvc.h
               opt_split.cc
               rowid_filter.cc rowid_filter.h
               opt_trace.cc
               table_cache.cc encryption.cc temporary_tables.cc
               proxy_protocol.cc backup.cc xa.cc
               ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc.cc
               ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc_ora.cc
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
   SET(SQL_SOURCE ${SQL_SOURCE} threadpool_win.cc)
 ENDIF()
 SET(SQL_SOURCE ${SQL_SOURCE} threadpool_generic.cc)
 SET(SQL_SOURCE ${SQL_SOURCE} threadpool_common.cc)
ENDIF()

IF(WIN32)
  SET(SQL_SOURCE ${SQL_SOURCE} handle_connections_win.cc)
ENDIF()

MYSQL_ADD_PLUGIN(partition ha_partition.cc STORAGE_ENGINE DEFAULT STATIC_ONLY
RECOMPILE_FOR_EMBEDDED)
MYSQL_ADD_PLUGIN(sql_sequence ha_sequence.cc STORAGE_ENGINE MANDATORY STATIC_ONLY
RECOMPILE_FOR_EMBEDDED)

ADD_LIBRARY(sql STATIC ${SQL_SOURCE})
DTRACE_INSTRUMENT(sql)
TARGET_LINK_LIBRARIES(sql
  mysys mysys_ssl dbug strings vio pcre
  ${LIBWRAP} ${LIBCRYPT} ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT}
  ${SSL_LIBRARIES}
  ${LIBSYSTEMD})

FOREACH(se aria partition perfschema sql_sequence wsrep)
  # These engines are used directly in sql sources.
  IF(TARGET ${se})
    TARGET_LINK_LIBRARIES(sql ${se})
  ENDIF()
ENDFOREACH()

IF(WIN32)
  SET(MYSQLD_SOURCE main.cc nt_servc.cc message.rc)
  TARGET_LINK_LIBRARIES(sql psapi)
ELSE()
  SET(MYSQLD_SOURCE main.cc ${DTRACE_PROBES_ALL})
ENDIF()


IF(MSVC AND NOT WITHOUT_DYNAMIC_PLUGINS)

  # mysqld.exe must to export symbols from some specific libs.
  # These symbols are used by dynamic plugins, that "link" to mysqld.
  #
  # To do that, we
  #
  # 1. Generate mysqld_lib.def text file with all symbols from static
  # libraries mysys, dbug, strings, sql.
  # 2. Then we call
  #  lib.exe /DEF:mysqld_lib.def ...
  #  to create import library mysqld_lib.lib and export library mysqld_lib.exp
  # 3. mysqld.exe links with mysqld_lib.exp (exporting symbols)
  # 4. plugins link with mysqld_lib.lib (importing symbols)
  #
  # We do not not regenerate .def, .lib and .exp
  # without necessity.E.g source modifications, that do not
  # change list of exported symbols, will not result in a relink for plugins.

  SET(MYSQLD_DEF ${CMAKE_CURRENT_BINARY_DIR}/mysqld_lib${CMAKE_CFG_INTDIR}.def)
  SET(MYSQLD_EXP ${CMAKE_CURRENT_BINARY_DIR}/mysqld_lib${CMAKE_CFG_INTDIR}.exp)
  SET(MYSQLD_LIB ${CMAKE_CURRENT_BINARY_DIR}/mysqld_lib${CMAKE_CFG_INTDIR}.lib)
  SET(MYSQLD_CORELIBS sql mysys dbug strings)
  FOREACH (CORELIB ${MYSQLD_CORELIBS})
    SET (LIB_LOCATIONS ${LIB_LOCATIONS} $<TARGET_FILE:${CORELIB}>)
  ENDFOREACH (CORELIB)

  SET(_PLATFORM x86)
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(_PLATFORM  x64)
  ENDIF()
  # Create a cmake script to generate import and export libs
  # from a .def file
  SET(CMAKE_CONFIGURABLE_FILE_CONTENT "
    IF ((mysqld_lib\${CFG}.def IS_NEWER_THAN mysqld_lib\${CFG}.lib) OR
        (mysqld_lib\${CFG}.def IS_NEWER_THAN mysqld_lib\${CFG}.exp))
      FILE(REMOVE mysqld_lib\${CFG}.lib mysqld_lib\${CFG}.exp)
      SET(ENV{VS_UNICODE_OUTPUT})
      EXECUTE_PROCESS (
        COMMAND \"${CMAKE_LINKER}\" /lib /NAME:mysqld.exe \"/DEF:${CMAKE_CURRENT_BINARY_DIR}/mysqld_lib\${CFG}.def\" /MACHINE:${_PLATFORM}
        RESULT_VARIABLE ret)
      IF(NOT ret EQUAL 0)
        MESSAGE(FATAL_ERROR \"process failed ret=\${ret}\")
      ENDIF()
    ENDIF()
  ")

  CONFIGURE_FILE(
    ${PROJECT_SOURCE_DIR}/cmake/configurable_file_content.in
    make_mysqld_lib.cmake
    @ONLY)

  IF(CMAKE_VERSION VERSION_GREATER "3.2.0")
    SET(MYSQLD_LIB_BYPRODUCTS  BYPRODUCTS ${MYSQLD_DEF} ${MYSQLD_LIB} ${MYSQLD_EXP})
  ENDIF()

  ADD_CUSTOM_COMMAND(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mysqld_lib.stamp
    ${MYSQLD_LIB_BYPRODUCTS}
    COMMENT "Generating ${MYSQLD_DEF}, ${MYSQLD_LIB}, ${MYSQLD_EXP}"
    COMMAND cscript //nologo ${PROJECT_SOURCE_DIR}/win/create_def_file.js
            ${_PLATFORM} /forLib ${LIB_LOCATIONS} > ${MYSQLD_DEF}.tmp
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MYSQLD_DEF}.tmp ${MYSQLD_DEF}
    COMMAND ${CMAKE_COMMAND} -E remove ${MYSQLD_DEF}.tmp
    COMMAND ${CMAKE_COMMAND}  "-DCFG=${CMAKE_CFG_INTDIR}" -P make_mysqld_lib.cmake
    COMMAND ${CMAKE_COMMAND} -E touch mysqld_lib.stamp
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${MYSQLD_CORELIBS}
  )

  ADD_CUSTOM_TARGET(gen_mysqld_lib DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mysqld_lib.stamp)
  ADD_LIBRARY(mysqld_import_lib UNKNOWN IMPORTED GLOBAL)
  SET_TARGET_PROPERTIES(mysqld_import_lib PROPERTIES IMPORTED_LOCATION ${MYSQLD_LIB})
ENDIF()

ADD_LIBRARY(sql_builtins STATIC ${CMAKE_CURRENT_BINARY_DIR}/sql_builtin.cc)
TARGET_LINK_LIBRARIES(sql_builtins ${MYSQLD_STATIC_PLUGIN_LIBS})

MYSQL_ADD_EXECUTABLE(mysqld ${MYSQLD_SOURCE} DESTINATION ${INSTALL_SBINDIR} COMPONENT Server)

IF(APPLE) 
  # Add CoreServices framework since some dloadable plugins may need it 
  FIND_LIBRARY(CORESERVICES NAMES CoreServices) 
  IF(CORESERVICES) 
    TARGET_LINK_LIBRARIES(mysqld LINK_PRIVATE ${CORESERVICES})
  ENDIF() 
ENDIF() 

IF(NOT WITHOUT_DYNAMIC_PLUGINS)
  IF(NOT MSVC)
    SET_TARGET_PROPERTIES(mysqld PROPERTIES ENABLE_EXPORTS TRUE)
  ENDIF()
  GET_TARGET_PROPERTY(mysqld_link_flags mysqld LINK_FLAGS)
  IF(NOT mysqld_link_flags)
    SET(mysqld_link_flags)
  ENDIF()
  IF (MINGW OR CYGWIN)
    SET_TARGET_PROPERTIES(mysqld PROPERTIES LINK_FLAGS "${mysqld_link_flags} -Wl,--export-all-symbols")
  ENDIF()
  IF(MSVC)
    SET_TARGET_PROPERTIES(mysqld PROPERTIES LINK_FLAGS "${mysqld_link_flags} \"${MYSQLD_EXP}\"")
    ADD_DEPENDENCIES(mysqld gen_mysqld_lib)
  ENDIF()
ENDIF(NOT WITHOUT_DYNAMIC_PLUGINS)

TARGET_LINK_LIBRARIES(mysqld LINK_PRIVATE sql sql_builtins)


# Provide plugins with minimal set of libraries
SET(INTERFACE_LIBS ${LIBRT})
IF(INTERFACE_LIBS)
  TARGET_LINK_LIBRARIES(mysqld LINK_PUBLIC ${INTERFACE_LIBS})
ENDIF()

# On Solaris, some extra effort is required in order to get dtrace probes
# from static libraries
DTRACE_INSTRUMENT_STATIC_LIBS(mysqld 
 "sql;mysys;mysys_ssl;${MYSQLD_STATIC_PLUGIN_LIBS}")
 

SET(WITH_MYSQLD_LDFLAGS "" CACHE STRING "Additional linker flags for mysqld")
MARK_AS_ADVANCED(WITH_MYSQLD_LDFLAGS)
IF(WITH_MYSQLD_LDFLAGS)
  GET_TARGET_PROPERTY(MYSQLD_LINK_FLAGS mysqld LINK_FLAGS)
  IF(NOT MYSQLD_LINK_FLAGS)
    SET(MYSQLD_LINK_FLAGS) 
  ENDIF() 
  SET_TARGET_PROPERTIES(mysqld PROPERTIES LINK_FLAGS 
     "${MYSQLD_LINK_FLAGS} ${WITH_MYSQLD_LDFLAGS}")
ENDIF()

FIND_PACKAGE(BISON 2.0)


# Handle out-of-source build from source package with possibly broken 
# bison. Copy bison output to from source to build directory, if not already 
# there
IF (NOT BISON_FOUND)
  IF (NOT ${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_CURRENT_BINARY_DIR})
    FOREACH(file sql_yacc.cc sql_yacc.hh sql_yacc_ora.cc sql_yacc_ora.hh)
    IF(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${file} AND (NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${file}))
        CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${file}
        ${CMAKE_CURRENT_BINARY_DIR}/${file} COPYONLY)
    ENDIF()
    ENDFOREACH()
  ENDIF()

  IF(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc.cc)
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
  BISON_TARGET(gen_sql_yacc ${CMAKE_CURRENT_SOURCE_DIR}/sql_yacc.yy ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc.cc
             COMPILE_FLAGS "-p MYSQL")

  BISON_TARGET(gen_sql_yacc_ora ${CMAKE_CURRENT_SOURCE_DIR}/sql_yacc_ora.yy ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc_ora.cc
             COMPILE_FLAGS "-p ORA")
ENDIF()

IF(NOT CMAKE_CROSSCOMPILING)
  ADD_EXECUTABLE(gen_lex_token gen_lex_token.cc
   ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc.hh)
  ADD_EXECUTABLE(gen_lex_hash gen_lex_hash.cc)
ENDIF()

ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lex_hash.h
  COMMAND gen_lex_hash > lex_hash.h
  DEPENDS gen_lex_hash
)

MYSQL_ADD_EXECUTABLE(mysql_tzinfo_to_sql tztime.cc COMPONENT Server)
SET_TARGET_PROPERTIES(mysql_tzinfo_to_sql PROPERTIES COMPILE_FLAGS "-DTZINFO2SQL")
TARGET_LINK_LIBRARIES(mysql_tzinfo_to_sql mysys mysys_ssl)

ADD_CUSTOM_TARGET( 
        GenServerSource
        DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/lex_hash.h
        ${CMAKE_CURRENT_BINARY_DIR}/lex_token.h
        ${CMAKE_CURRENT_BINARY_DIR}/sql_yacc_ora.cc
)
ADD_DEPENDENCIES(sql GenServerSource)

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
  DEPENDS ${CMAKE_BINARY_DIR}/sql/sql_yacc.cc ${CMAKE_BINARY_DIR}/sql/sql_yacc.hh
  DEPENDS ${CMAKE_BINARY_DIR}/sql/sql_yacc_ora.cc ${CMAKE_BINARY_DIR}/sql/sql_yacc_ora.hh
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

ADD_CUSTOM_TARGET(distclean
  COMMAND ${CMAKE_COMMAND} -E echo  WARNING: distclean target is not functional
  COMMAND ${CMAKE_COMMAND} -E echo  Use 'git clean -Xdf' instead
  VERBATIM
  )

IF(INSTALL_LAYOUT STREQUAL "STANDALONE")

# Install initial database on windows
IF(WIN32 AND TARGET mysqld AND NOT CMAKE_CROSSCOMPILING)

  IF(MSVC_IDE OR CMAKE_GENERATOR MATCHES "Xcode")
    SET (CONFIG_PARAM -DCONFIG=${CMAKE_CFG_INTDIR})
  ENDIF()
  MAKE_DIRECTORY(${CMAKE_CURRENT_BINARY_DIR}/data)
  ADD_CUSTOM_COMMAND(
     OUTPUT initdb.dep
     COMMAND ${CMAKE_COMMAND} -E remove_directory data
     COMMAND ${CMAKE_COMMAND} -E make_directory data
     COMMAND ${CMAKE_COMMAND} -E chdir data ${CMAKE_COMMAND}
     ${CONFIG_PARAM}
     -DTOP_SRCDIR="${CMAKE_SOURCE_DIR}"
     -DBINDIR="${CMAKE_CURRENT_BINARY_DIR}"
     -DMYSQLD_EXECUTABLE="$<TARGET_FILE:mysqld>"
     -DCMAKE_CFG_INTDIR="${CMAKE_CFG_INTDIR}"
     -P ${CMAKE_SOURCE_DIR}/cmake/create_initial_db.cmake
     COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/initdb.dep
     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/
     DEPENDS mysqld
  )
  ADD_CUSTOM_TARGET(initial_database  
    ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/initdb.dep
  )
ELSE()
  # Not windows or cross compiling, just install an empty directory
  INSTALL(FILES ${DUMMY_FILE} DESTINATION data/mysql COMPONENT DataFiles)
ENDIF(WIN32 AND TARGET mysqld AND NOT CMAKE_CROSSCOMPILING)
ENDIF(INSTALL_LAYOUT STREQUAL "STANDALONE")

IF(WIN32)
  SET(my_bootstrap_sql ${CMAKE_CURRENT_BINARY_DIR}/my_bootstrap.sql)
  FILE(TO_NATIVE_PATH ${my_bootstrap_sql} native_outfile)

  # Create bootstrapper SQL script
  ADD_CUSTOM_COMMAND(OUTPUT    
    ${my_bootstrap_sql}
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_SOURCE_DIR}/scripts
    cmd /c copy mysql_system_tables.sql+mysql_system_tables_data.sql+fill_help_tables.sql+mysql_performance_tables.sql+mysql_test_db.sql ${native_outfile}
    DEPENDS
    ${CMAKE_SOURCE_DIR}/scripts/mysql_system_tables.sql
    ${CMAKE_SOURCE_DIR}/scripts/mysql_system_tables_data.sql
    ${CMAKE_SOURCE_DIR}/scripts/fill_help_tables.sql
    ${CMAKE_SOURCE_DIR}/scripts/mysql_performance_tables.sql
    ${CMAKE_SOURCE_DIR}/scripts/mysql_test_db.sql
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

  MYSQL_ADD_EXECUTABLE(mysql_install_db 
    mysql_install_db.cc
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_bootstrap_sql.c
    COMPONENT Server
  )
  SET_TARGET_PROPERTIES(mysql_install_db PROPERTIES COMPILE_FLAGS -DINSTALL_PLUGINDIR=${INSTALL_PLUGINDIR})
  TARGET_LINK_LIBRARIES(mysql_install_db mysys shlwapi)

  ADD_LIBRARY(winservice STATIC winservice.c)
  TARGET_LINK_LIBRARIES(winservice shell32)

  MYSQL_ADD_EXECUTABLE(mysql_upgrade_service
    mysql_upgrade_service.cc
    upgrade_conf_file.cc
    COMPONENT Server)
  TARGET_LINK_LIBRARIES(mysql_upgrade_service mysys winservice)
ENDIF(WIN32)

INSTALL(DIRECTORY . DESTINATION ${INSTALL_INCLUDEDIR}/server/private COMPONENT Development
  FILES_MATCHING PATTERN "*.h"
  PATTERN share EXCLUDE
  PATTERN CMakeFiles EXCLUDE)
