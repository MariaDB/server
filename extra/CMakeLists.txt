# Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.
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

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include ${ZLIB_INCLUDE_DIR})

# Default install component for the files is Server here
SET(MYSQL_INSTALL_COMPONENT Server)

IF(NOT CMAKE_CROSSCOMPILING OR DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
 ADD_EXECUTABLE(comp_err comp_err.c)
 TARGET_LINK_LIBRARIES(comp_err mysys)
ENDIF()

# Generate mysqld_error.h
# Try not to change its timestamp if not necessary(as touching
# mysqld_error.h results in rebuild of the almost whole server)
# To preserve timestamp, first generate a temp header file, then copy it
# to mysqld_error.h using cmake -E copy_if_different
ADD_CUSTOM_COMMAND(OUTPUT ${CMAKE_BINARY_DIR}/include/mysqld_error.h.tmp
   COMMAND comp_err
	--charset=${PROJECT_SOURCE_DIR}/sql/share/charsets
	--out-dir=${CMAKE_BINARY_DIR}/sql/share/
	--header_file=${CMAKE_BINARY_DIR}/include/mysqld_error.h.tmp
	--name_file=${CMAKE_BINARY_DIR}/include/mysqld_ername.h.tmp
	--state_file=${CMAKE_BINARY_DIR}/include/sql_state.h.tmp
	--in_file=${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt
   COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/include/mysqld_error.h.tmp ${CMAKE_BINARY_DIR}/include/mysqld_error.h
   COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/include/mysqld_ername.h.tmp ${CMAKE_BINARY_DIR}/include/mysqld_ername.h
   COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/include/sql_state.h.tmp ${CMAKE_BINARY_DIR}/include/sql_state.h
   DEPENDS comp_err ${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt)

ADD_CUSTOM_TARGET(GenError
                  ALL
                  DEPENDS
                  ${CMAKE_BINARY_DIR}/include/mysqld_error.h.tmp)

MYSQL_ADD_EXECUTABLE(my_print_defaults my_print_defaults.c COMPONENT Client)
TARGET_LINK_LIBRARIES(my_print_defaults  mysys)

MYSQL_ADD_EXECUTABLE(perror perror.c)
ADD_DEPENDENCIES(perror GenError)
TARGET_LINK_LIBRARIES(perror mysys)

IF(UNIX)
MYSQL_ADD_EXECUTABLE(resolveip resolveip.c)
TARGET_LINK_LIBRARIES(resolveip mysys)
IF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  INCLUDE(CheckFunctionExists)
  INCLUDE(CheckLibraryExists)
  MY_SEARCH_LIBS(inet_aton  "nsl;socket;resolv" SOLARIS_NSL)
  TARGET_LINK_LIBRARIES(resolveip ${SOLARIS_NSL})
ENDIF()
ENDIF()

IF(WITH_INNOBASE_STORAGE_ENGINE)
  # Add path to the InnoDB headers
  INCLUDE_DIRECTORIES(
    ${CMAKE_SOURCE_DIR}/storage/innobase/include
    ${CMAKE_SOURCE_DIR}/sql)

  # We use the InnoDB code directly in case the code changes.
  ADD_DEFINITIONS("-DUNIV_INNOCHECKSUM")

  SET(INNOBASE_SOURCES
      ../storage/innobase/buf/buf0checksum.cc
      ../storage/innobase/ut/ut0ut.cc
      ../storage/innobase/buf/buf0buf.cc
      ../storage/innobase/page/page0zip.cc
      ../storage/innobase/fil/fil0crypt.cc
      )


  MYSQL_ADD_EXECUTABLE(innochecksum innochecksum.cc ${INNOBASE_SOURCES})
  TARGET_LINK_LIBRARIES(innochecksum mysys mysys_ssl)
  ADD_DEPENDENCIES(innochecksum GenError)
ENDIF()

set(SQL_PARSER_SOURCES frm_parser.cc 
        ${CMAKE_SOURCE_DIR}/sql/sql_class.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_show.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_plugin.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_string.cc
        ${CMAKE_SOURCE_DIR}/sql/net_serv.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_state.c
 ########${CMAKE_SOURCE_DIR}/sql/mysqld.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_cache.cc
 ########${CMAKE_SOURCE_DIR}/sql/mdl.cc
        ${CMAKE_SOURCE_DIR}/sql/handler.cc
        ${CMAKE_SOURCE_DIR}/sql/log.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_error.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_type.cc
        ${CMAKE_SOURCE_DIR}/sql/table.cc
        ${CMAKE_SOURCE_DIR}/sql/field.cc
        ${CMAKE_SOURCE_DIR}/sql/rpl_utility_server.cc
        ${CMAKE_SOURCE_DIR}/sql/opt_trace.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_profile.cc
  #######${CMAKE_SOURCE_DIR}/libmysqld/lib_sql.cc #!!!!!!!!!!!!!!!!!!!!!!
        ${CMAKE_SOURCE_DIR}/sql/protocol.cc
        ${CMAKE_SOURCE_DIR}/sql/thr_malloc.cc
        ${CMAKE_SOURCE_DIR}/sql/session_tracker.cc
        ${CMAKE_SOURCE_DIR}/sql/backup.cc
        ${CMAKE_SOURCE_DIR}/sql/xa.cc
        ${CMAKE_SOURCE_DIR}/sql/my_apc.cc
        ${CMAKE_SOURCE_DIR}/sql/item.cc
        ${CMAKE_SOURCE_DIR}/sql/item_func.cc
        ${CMAKE_SOURCE_DIR}/sql/item_subselect.cc
        #${CMAKE_SOURCE_DIR}/sql/sql_lex.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_signal.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_base.cc
        ${CMAKE_SOURCE_DIR}/sql/lock.cc
        ${CMAKE_SOURCE_DIR}/sql/temporary_tables.cc
        ${CMAKE_SOURCE_DIR}/sql/repl_failsafe.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_union.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_acl.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_handler.cc
        ${CMAKE_SOURCE_DIR}/sql/rpl_record.cc
        ${CMAKE_SOURCE_DIR}/sql/log_event_server.cc
        ${CMAKE_SOURCE_DIR}/sql/rpl_rli.cc
        ${CMAKE_SOURCE_DIR}/sql/rpl_filter.cc
 ######${CMAKE_SOURCE_DIR}/sql/slave.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_audit.cc
        ${CMAKE_SOURCE_DIR}/sql/sp_head.cc
        ${CMAKE_SOURCE_DIR}/sql/sp_rcontext.cc
        ${CMAKE_SOURCE_DIR}/sql/sp_cache.cc
        ${CMAKE_SOURCE_DIR}/sql/transaction.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_select.cc
        ${CMAKE_SOURCE_DIR}/sql/debug_sync.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_parse.cc
 #######${CMAKE_SOURCE_DIR}/sql/sql_connect.cc
        ${CMAKE_SOURCE_DIR}/sql/sql_list.cc
        ${CMAKE_BINARY_DIR}/sql/yy_mariadb.hh)

MYSQL_ADD_EXECUTABLE(frm_parser ${SQL_PARSER_SOURCES})
target_compile_definitions(frm_parser PUBLIC -DMYSQL_SERVER -DHAVE_EVENT_SCHEDULER)
TARGET_INCLUDE_DIRECTORIES(frm_parser PUBLIC ${CMAKE_SOURCE_DIR}/sql)
TARGET_LINK_LIBRARIES(frm_parser mysys)

MYSQL_ADD_EXECUTABLE(replace replace.c COMPONENT Client)
TARGET_LINK_LIBRARIES(replace mysys)

IF(UNIX)
  MYSQL_ADD_EXECUTABLE(resolve_stack_dump resolve_stack_dump.c)
  TARGET_LINK_LIBRARIES(resolve_stack_dump mysys)

  MYSQL_ADD_EXECUTABLE(mariadb-waitpid mysql_waitpid.c COMPONENT Client)
  TARGET_LINK_LIBRARIES(mariadb-waitpid mysys)

  MYSQL_ADD_EXECUTABLE(mariadbd-safe-helper mysqld_safe_helper.c COMPONENT Server)
  TARGET_LINK_LIBRARIES(mariadbd-safe-helper mysys)
ENDIF()
