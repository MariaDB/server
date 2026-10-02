# Copyright (c) 2026, MariaDB plc
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

# MDEV-11111: the embedded server is a private mariadbd process, started by
# libmariadbd (built by Connector/C, with WITH_EMBEDDED_SERVER). These are
# unmodified client programs, which only use the client API.
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/libmariadb/include
                    ${CMAKE_BINARY_DIR}/libmariadb/include)
IF(TARGET libmariadbd)
  ADD_EXECUTABLE(embedded_app_test EXCLUDE_FROM_ALL embedded_app_test.c)
  TARGET_LINK_LIBRARIES(embedded_app_test libmariadbd)
  ADD_EXECUTABLE(embedded_auth_test EXCLUDE_FROM_ALL embedded_auth_test.c)
  TARGET_LINK_LIBRARIES(embedded_auth_test libmariadbd)
ENDIF()
