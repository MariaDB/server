# Copyright (c) 2009, 2018, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA 

# Bundled zlib. Builds the bundled copy and points MariaDB::zlib at it.
# Note: we intentionally do NOT touch the standard FindZLIB result variables
# (ZLIB_FOUND, ZLIB_LIBRARIES, ZLIB_INCLUDE_DIR(S), ...). Overwriting them
# confuses other find_package() calls (notably under vcpkg).
MACRO (MYSQL_USE_BUNDLED_ZLIB)
  SET(BUILD_BUNDLED_ZLIB 1)
  SET(ZLIB_BUNDLED_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/zlib ${CMAKE_BINARY_DIR}/zlib
      CACHE INTERNAL "Bundled zlib include directories")
  SET(WITH_ZLIB "bundled" CACHE STRING "Use bundled zlib")
  ADD_SUBDIRECTORY(zlib)
  ADD_LIBRARY(mariadb_zlib INTERFACE)
  TARGET_LINK_LIBRARIES(mariadb_zlib INTERFACE zlib)
  TARGET_INCLUDE_DIRECTORIES(mariadb_zlib INTERFACE ${ZLIB_BUNDLED_INCLUDE_DIR})
ENDMACRO()

# MYSQL_CHECK_ZLIB_WITH_COMPRESS
#
# Provides the following configure options:
# WITH_ZLIB=[bundled|system]
# If this is "bundled", we use bundled zlib.
# Otherwise search for system zlib, and fall back to the bundled copy if it
# is not found (or not usable).
# Defines the MariaDB::zlib target, which points at either the system
# ZLIB::ZLIB or the bundled zlib, and carries the right include directories.

MACRO (MYSQL_CHECK_ZLIB_WITH_COMPRESS)

  IF(WITH_ZLIB STREQUAL "bundled")
    MYSQL_USE_BUNDLED_ZLIB()
  ELSE()
    FIND_PACKAGE(PkgConfig QUIET)
    IF(PKG_CONFIG_FOUND AND (COMMAND PKG_GET_VARIABLE) AND (NOT WIN32))
      PKG_GET_VARIABLE(ZLIB_ROOT zlib prefix)
    ENDIF()
    FIND_PACKAGE(ZLIB)
    IF(ZLIB_FOUND)
     INCLUDE(CheckFunctionExists)
      SET(CMAKE_REQUIRED_LIBRARIES ${ZLIB_LIBRARIES})
      CHECK_FUNCTION_EXISTS(crc32 HAVE_CRC32)
      CHECK_FUNCTION_EXISTS(compressBound HAVE_COMPRESSBOUND)
      CHECK_FUNCTION_EXISTS(deflateBound HAVE_DEFLATEBOUND)
      SET(CMAKE_REQUIRED_LIBRARIES)
      IF(HAVE_CRC32 AND HAVE_COMPRESSBOUND AND HAVE_DEFLATEBOUND)
        SET(WITH_ZLIB "system" CACHE STRING
          "Which zlib to use (possible values are 'bundled' or 'system')")
        ADD_LIBRARY(mariadb_zlib INTERFACE)
        TARGET_LINK_LIBRARIES(mariadb_zlib INTERFACE ZLIB::ZLIB)
      ELSE()
        SET(ZLIB_FOUND FALSE)
        MESSAGE(STATUS "system zlib found but not usable")
      ENDIF()
    ENDIF()
    IF(NOT ZLIB_FOUND)
      MYSQL_USE_BUNDLED_ZLIB()
    ENDIF()
  ENDIF()
  ADD_LIBRARY(MariaDB::zlib ALIAS mariadb_zlib)
  SET(HAVE_COMPRESS 1)
ENDMACRO()
