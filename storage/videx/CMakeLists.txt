# Copyright (c) 2025 Bytedance Ltd. and its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Check necessary dependencies
FIND_PACKAGE(CURL)
IF(NOT CURL_FOUND)
  MESSAGE(STATUS "CURL not found. VIDEX will not be compiled")
  RETURN()
ENDIF()

SET_PACKAGE_PROPERTIES(CURL PROPERTIES TYPE REQUIRED)
SET(CPACK_RPM_videx-engine_PACKAGE_SUMMARY "VIDEX AI-enhanced virtual index engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_videx-engine_PACKAGE_DESCRIPTION "VIDEX enables What-If Analysis via virtual indexes to simulate
query plans, forwarding requests via HTTP to external AI servers for enhanced cost estimation." PARENT_SCOPE)

INCLUDE_DIRECTORIES(
        ${CMAKE_SOURCE_DIR}/sql
        ${CMAKE_SOURCE_DIR}/include
        ${CURL_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
)

SET(VIDEX_SOURCES
        videx_utils.cc
        ha_videx.cc
)

MYSQL_ADD_PLUGIN(videx
        ${VIDEX_SOURCES}
        STORAGE_ENGINE
        LINK_LIBRARIES ${ZLIB_LIBRARIES} ${CURL_LIBRARIES}
        COMPONENT videx-engine
)
