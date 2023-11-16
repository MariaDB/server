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
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/strings
  ${CMAKE_SOURCE_DIR}/mysys_ssl
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

MYSQL_ADD_EXECUTABLE(mariadb-frm mariafrm.cc ${CMAKE_SOURCE_DIR}/sql/compat56.cc)

SET_TARGET_PROPERTIES (mariadb-frm PROPERTIES HAS_CXX TRUE)

TARGET_LINK_LIBRARIES(mariadb-frm mysys strings)
