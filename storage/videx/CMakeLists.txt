# Copyright (c) 2006, 2023, Oracle and/or its affiliates.
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

INCLUDE_DIRECTORIES(
        ${CMAKE_SOURCE_DIR}/sql
        ${CMAKE_SOURCE_DIR}/include
        ${CURL_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
)

SET(VIDEX_SOURCES
        videx_json_item.cc
        # videx_log_utils.cc
        ha_videx.cc
)

IF (PLUGIN_VIDEX STREQUAL "NO")
        ADD_FEATURE_INFO(VIDEX "OFF" "Storage Engine")
        RETURN()
ENDIF()

IF (PLUGIN_VIDEX STREQUAL "STATIC" OR PLUGIN_VIDEX STREQUAL "YES")
        MESSAGE(STATUS "Building VIDEX as static plugin (default)")
        MYSQL_ADD_PLUGIN(videx
                ${VIDEX_SOURCES}
                STORAGE_ENGINE
                STATIC_ONLY
                LINK_LIBRARIES ${ZLIB_LIBRARIES} ${CURL_LIBRARIES}
        )
ELSEIF (PLUGIN_VIDEX STREQUAL "DYNAMIC")
        MESSAGE(STATUS "Building VIDEX as dynamic plugin only")
        MYSQL_ADD_PLUGIN(videx
                ${VIDEX_SOURCES}
                STORAGE_ENGINE
                MODULE_ONLY
                LINK_LIBRARIES ${ZLIB_LIBRARIES} ${CURL_LIBRARIES}
        )
ENDIF()