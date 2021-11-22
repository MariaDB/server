# Copyright (c) 2013, 2017 Percona LLC and/or its affiliates.
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA


OPTION(WITH_MARIABACKUP "Include mariabackup" ON)
ADD_FEATURE_INFO(MARIABACKUP WITH_MARIABACKUP "MariaDB Backup Utility")

IF(NOT WITH_MARIABACKUP)
  RETURN()
ENDIF()

IF(NOT WIN32)
  CHECK_SYMBOL_EXISTS(regcomp regex.h HAVE_SYSTEM_REGEX)
  IF(HAVE_SYSTEM_REGEX)
    ADD_DEFINITIONS(-DHAVE_SYSTEM_REGEX)
  ENDIF()
ENDIF()

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_CURRENT_SOURCE_DIR}/quicklz
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/crc
  )

IF(NOT HAVE_SYSTEM_REGEX)
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/pcre)
ENDIF()

ADD_DEFINITIONS(-UMYSQL_SERVER)
########################################################################
# xtrabackup binary
########################################################################

IF(WIN32)
  SET(NT_SERVICE_SOURCE ${PROJECT_SOURCE_DIR}/sql/nt_servc.cc)
ELSE()
  SET(NT_SERVICE_SOURCE)
ENDIF()

ADD_DEFINITIONS(-DPCRE_STATIC=1 -DHAVE_OPENSSL=1)

MYSQL_ADD_EXECUTABLE(mariabackup
  xtrabackup.cc
  innobackupex.cc
  changed_page_bitmap.cc
  datasink.cc
  ds_buffer.cc
  ds_compress.cc
  ds_local.cc
  ds_stdout.cc
  ds_tmpfile.cc
  ds_xbstream.cc
  fil_cur.cc
  quicklz/quicklz.c
  read_filt.cc
  write_filt.cc
  wsrep.cc
  xbstream_write.cc
  backup_mysql.cc
  backup_copy.cc
  encryption_plugin.cc
  ${PROJECT_SOURCE_DIR}/sql/net_serv.cc
  ${NT_SERVICE_SOURCE}
  ${PROJECT_SOURCE_DIR}/libmysqld/libmysql.c
  COMPONENT backup
  )


# Export all symbols on Unix, for better crash callstacks
SET_TARGET_PROPERTIES(mariabackup PROPERTIES ENABLE_EXPORTS TRUE)
ADD_SUBDIRECTORY(crc)


TARGET_LINK_LIBRARIES(mariabackup sql crc)

IF(NOT HAVE_SYSTEM_REGEX)
  TARGET_LINK_LIBRARIES(mariabackup pcreposix)
ENDIF()


########################################################################
# mbstream binary
########################################################################
MYSQL_ADD_EXECUTABLE(mbstream
  ds_buffer.cc
  ds_local.cc
  ds_stdout.cc
  datasink.cc
  xbstream.cc
  xbstream_read.cc
  xbstream_write.cc
  COMPONENT backup
  )


TARGET_LINK_LIBRARIES(mbstream
  mysys
  crc
)
ADD_DEPENDENCIES(mbstream GenError)

IF(MSVC)
  SET_TARGET_PROPERTIES(mbstream PROPERTIES LINK_FLAGS setargv.obj)
ENDIF()
