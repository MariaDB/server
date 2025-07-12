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

#DISABLE_MISSING_PROFILE_WARNING() # not exist in mariadb
ADD_DEFINITIONS(-DMYSQL_SERVER)
ADD_DEFINITIONS(-DMUTEX_FUTEX) # what is this for?

INCLUDE_DIRECTORIES(
        ${CMAKE_SOURCE_DIR}/sql
        ${CMAKE_SOURCE_DIR}/include
        # ${CMAKE_SOURCE_DIR}/sql/auth
        # ${CMAKE_SOURCE_DIR}/extra/rapidjson/include
        # ${CMAKE_SOURCE_DIR}/extra/curl/curl-8.1.2/include
        ${CURL_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
)

IF (WITH_VIDEX_STORAGE_ENGINE AND NOT WITHOUT_VIDEX_STORAGE_ENGINE)
  # Check if the dynamic library is explicitly specified
  IF (PLUGIN_VIDEX STREQUAL "DYNAMIC")
    # Build only the dynamic library
    MESSAGE(STATUS "Building VIDEX as dynamic plugin only")
    MYSQL_ADD_PLUGIN(videx
        #     videx_json_item.cc
        #     videx_log_utils.cc
            ha_videx.cc
            STORAGE_ENGINE
            MODULE_ONLY
            LINK_LIBRARIES ${ZLIB_LIBRARIES} ${CURL_LIBRARIES}
            )
  ELSE ()
    # Default: Build both static and dynamic libraries
    MESSAGE(STATUS "Building VIDEX as static plugin (default)")
    MYSQL_ADD_PLUGIN(videx_static
            # videx_json_item.cc
            # videx_log_utils.cc
            ha_videx.cc
            STORAGE_ENGINE
            DEFAULT
            STATIC_ONLY
            LINK_LIBRARIES ${ZLIB_LIBRARIES} ${CURL_LIBRARIES}
            )
    target_compile_definitions(videx_static PRIVATE STATIC_VIDEX)

    MESSAGE(STATUS "Building VIDEX as dynamic plugin")
    # Generate the dynamic library at the same time
    MYSQL_ADD_PLUGIN(videx
        #     videx_json_item.cc
        #     videx_log_utils.cc
            ha_videx.cc
            STORAGE_ENGINE
            MODULE_ONLY
            LINK_LIBRARIES ${ZLIB_LIBRARIES} ${CURL_LIBRARIES}
            )
  ENDIF ()
ENDIF ()