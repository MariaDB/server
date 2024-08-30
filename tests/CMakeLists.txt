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

ADD_DEFINITIONS("-DMYSQL_CLIENT")


INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/client)
INCLUDE_DIRECTORIES(BEFORE
  ${CMAKE_BINARY_DIR}/libmariadb/include
  ${CMAKE_SOURCE_DIR}/libmariadb/include)

MYSQL_ADD_EXECUTABLE(mariadb-client-test mysql_client_test.c COMPONENT Test)
SET(CLIENT_LIB mariadbclient mysys)

TARGET_LINK_LIBRARIES(mariadb-client-test ${CLIENT_LIB})
ADD_DEPENDENCIES(mariadb-client-test GenError ${CLIENT_LIB})

IF(WITH_UNIT_TESTS)
  ADD_EXECUTABLE(bug25714 bug25714.c)
  TARGET_LINK_LIBRARIES(bug25714 ${CLIENT_LIB})
  ADD_DEPENDENCIES(bug25714 GenError ${CLIENT_LIB})
ENDIF()

CHECK_INCLUDE_FILE(event.h HAVE_EVENT_H)
FIND_LIBRARY(EVENT_LIBRARY event)
MARK_AS_ADVANCED(EVENT_LIBRARY)
IF(HAVE_EVENT_H AND EVENT_LIBRARY)
  ADD_EXECUTABLE(async_queries async_queries.c)
  TARGET_LINK_LIBRARIES(async_queries ${CLIENT_LIB} ${EVENT_LIBRARY})
  ADD_DEPENDENCIES(async_queries GenError ${CLIENT_LIB})
ENDIF()
