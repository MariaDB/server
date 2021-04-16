# Copyright (c) 2006, 2011, Oracle and/or its affiliates. All rights reserved.
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

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/libmysqld/include
                    ${PCRE_INCLUDES}
                    ${CMAKE_SOURCE_DIR}/sql
                    ${MY_READLINE_INCLUDE_DIR}
                    )


ADD_DEFINITIONS(-DEMBEDDED_LIBRARY -UMYSQL_CLIENT)


MYSQL_ADD_EXECUTABLE(mariadb-embedded ../../client/completion_hash.cc
               ../../client/mysql.cc ../../client/readline.cc
               COMPONENT Client)
TARGET_LINK_LIBRARIES(mariadb-embedded mysqlserver)
IF(UNIX)
  TARGET_LINK_LIBRARIES(mariadb-embedded ${MY_READLINE_LIBRARY})
ENDIF(UNIX)

MYSQL_ADD_EXECUTABLE(mariadb-test-embedded ../../client/mysqltest.cc
  COMPONENT Test)
TARGET_LINK_LIBRARIES(mariadb-test-embedded mysqlserver pcre2-posix pcre2-8)
SET_SOURCE_FILES_PROPERTIES(../../client/mysqltest.cc PROPERTIES COMPILE_FLAGS "${PCRE2_DEBIAN_HACK}")

IF(CMAKE_GENERATOR MATCHES "Xcode")
# It does not seem possible to tell Xcode the resulting target might need
# to be linked with C++ runtime. The project needs to have at least one C++
# file. Add a dummy one.
  ADD_CUSTOM_COMMAND(OUTPUT
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_client_test_embedded_dummy.cc
   COMMAND ${CMAKE_COMMAND} -E touch
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_client_test_embedded_dummy.cc
  )
  MYSQL_ADD_EXECUTABLE(mariadb-client-test-embedded
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_client_test_embedded_dummy.cc
    ../../tests/mysql_client_test.c)
ELSE()
  MYSQL_ADD_EXECUTABLE(mariadb-client-test-embedded ../../tests/mysql_client_test.c
    COMPONENT Test)
  SET_TARGET_PROPERTIES(mariadb-client-test-embedded  PROPERTIES HAS_CXX TRUE)
ENDIF()
TARGET_LINK_LIBRARIES(mariadb-client-test-embedded mysqlserver)

IF(UNIX)
SET_TARGET_PROPERTIES(mariadb-embedded PROPERTIES ENABLE_EXPORTS TRUE)
SET_TARGET_PROPERTIES(mariadb-test-embedded PROPERTIES ENABLE_EXPORTS TRUE)
SET_TARGET_PROPERTIES(mariadb-client-test-embedded PROPERTIES ENABLE_EXPORTS TRUE)
ENDIF()
