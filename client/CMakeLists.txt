# Copyright (c) 2006, 2015, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2008, 2019, MariaDB Corporation
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

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${PCRE_INCLUDES}
  ${CMAKE_SOURCE_DIR}/mysys_ssl
  ${ZLIB_INCLUDE_DIR}
  ${SSL_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/strings
  ${MY_READLINE_INCLUDE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

INCLUDE_DIRECTORIES(BEFORE
  ${CMAKE_BINARY_DIR}/libmariadb/include
  ${CMAKE_SOURCE_DIR}/libmariadb/include)

## We will need libeay32.dll and ssleay32.dll when running client executables.
COPY_OPENSSL_DLLS(copy_openssl_client)

SET(CLIENT_LIB mariadbclient mysys)

ADD_DEFINITIONS(${SSL_DEFINES})
MYSQL_ADD_EXECUTABLE(mariadb completion_hash.cc mysql.cc readline.cc
                           ${CMAKE_SOURCE_DIR}/sql/sql_string.cc)
TARGET_LINK_LIBRARIES(mariadb ${CLIENT_LIB})
IF(UNIX)
  TARGET_LINK_LIBRARIES(mariadb ${MY_READLINE_LIBRARY})
  SET_TARGET_PROPERTIES(mariadb PROPERTIES ENABLE_EXPORTS TRUE)
ENDIF(UNIX)

MYSQL_ADD_EXECUTABLE(mariadb-test mysqltest.cc ${CMAKE_SOURCE_DIR}/sql/sql_string.cc COMPONENT Test)
SET_SOURCE_FILES_PROPERTIES(mysqltest.cc PROPERTIES COMPILE_FLAGS "-DTHREADS ${PCRE2_DEBIAN_HACK}")
TARGET_LINK_LIBRARIES(mariadb-test  ${CLIENT_LIB} pcre2-posix pcre2-8)
SET_TARGET_PROPERTIES(mariadb-test PROPERTIES ENABLE_EXPORTS TRUE)


MYSQL_ADD_EXECUTABLE(mariadb-check mysqlcheck.c)
TARGET_LINK_LIBRARIES(mariadb-check ${CLIENT_LIB})

MYSQL_ADD_EXECUTABLE(mariadb-dump mysqldump.c ../sql-common/my_user.c)
TARGET_LINK_LIBRARIES(mariadb-dump ${CLIENT_LIB})

MYSQL_ADD_EXECUTABLE(mariadb-import mysqlimport.c)
SET_SOURCE_FILES_PROPERTIES(mysqlimport.c PROPERTIES COMPILE_FLAGS "-DTHREADS")
TARGET_LINK_LIBRARIES(mariadb-import ${CLIENT_LIB})

MYSQL_ADD_EXECUTABLE(mariadb-upgrade mysql_upgrade.c COMPONENT Server)
TARGET_LINK_LIBRARIES(mariadb-upgrade ${CLIENT_LIB})
ADD_DEPENDENCIES(mariadb-upgrade GenFixPrivs)

MYSQL_ADD_EXECUTABLE(mariadb-show mysqlshow.c)
TARGET_LINK_LIBRARIES(mariadb-show ${CLIENT_LIB})

MYSQL_ADD_EXECUTABLE(mariadb-plugin mysql_plugin.c)
TARGET_LINK_LIBRARIES(mariadb-plugin ${CLIENT_LIB})

MYSQL_ADD_EXECUTABLE(mariadb-binlog mysqlbinlog.cc)
TARGET_LINK_LIBRARIES(mariadb-binlog ${CLIENT_LIB} mysys_ssl)

MYSQL_ADD_EXECUTABLE(mariadb-admin mysqladmin.cc ../sql/password.c)
TARGET_LINK_LIBRARIES(mariadb-admin ${CLIENT_LIB} mysys_ssl)

MYSQL_ADD_EXECUTABLE(mariadb-slap mysqlslap.c)
SET_SOURCE_FILES_PROPERTIES(mysqlslap.c PROPERTIES COMPILE_FLAGS "-DTHREADS")
TARGET_LINK_LIBRARIES(mariadb-slap ${CLIENT_LIB})

MYSQL_ADD_EXECUTABLE(mariadb-conv mariadb-conv.cc
                     ${CMAKE_SOURCE_DIR}/sql/sql_string.cc)
TARGET_LINK_LIBRARIES(mariadb-conv mysys strings)

IF(WIN32)
  MYSQL_ADD_EXECUTABLE(echo echo.c COMPONENT Test)
ENDIF(WIN32)

# async_example is just a code example, do not install it.
ADD_EXECUTABLE(async_example async_example.c)
TARGET_LINK_LIBRARIES(async_example ${CLIENT_LIB})

SET_TARGET_PROPERTIES (mariadb-check mariadb-dump mariadb-import mariadb-upgrade mariadb-show mariadb-slap mariadb-plugin async_example
PROPERTIES HAS_CXX TRUE)

FOREACH(t mariadb mariadb-test mariadb-check mariadb-dump mariadb-import mariadb-upgrade mariadb-show mariadb-plugin mariadb-binlog
  mariadb-admin mariadb-slap async_example)
  ADD_DEPENDENCIES(${t} GenError ${CLIENT_LIB})
ENDFOREACH()

ADD_DEFINITIONS(-DHAVE_DLOPEN)
