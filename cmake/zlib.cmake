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

MACRO (MYSQL_USE_BUNDLED_ZLIB)
  SET(ZLIB_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/zlib ${CMAKE_BINARY_DIR}/zlib)
  SET(BUILD_BUNDLED_ZLIB 1)
  SET(ZLIB_LIBRARIES zlib CACHE INTERNAL "Bundled zlib library")
  # temporarily define ZLIB_LIBRARY and ZLIB_INCLUDE_DIR for libmariadb
  SET(ZLIB_LIBRARY ${ZLIB_LIBRARIES})
  SET(ZLIB_INCLUDE_DIR ${ZLIB_INCLUDE_DIRS})
  SET(ZLIB_FOUND  TRUE)
  IF(VCPKG_INSTALLED_DIR)
    # Avoid errors in vcpkg's FIND_PACKAGE
    # for packages dependend on zlib
    SET(CMAKE_DISABLE_FIND_PACKAGE_ZLIB 1)
  ENDIF()
  SET(WITH_ZLIB "bundled" CACHE STRING "Use bundled zlib")
  ADD_SUBDIRECTORY(zlib)
ENDMACRO()

# MYSQL_CHECK_ZLIB_WITH_COMPRESS
#
# Provides the following configure options:
# WITH_ZLIB_BUNDLED
# If this is set,we use bundled zlib
# If this is not set,search for system zlib. 
# if system zlib is not found, use bundled copy
# ZLIB_LIBRARIES, ZLIB_INCLUDE_DIRS
# are set after this macro has run

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
      ELSE()
        SET(ZLIB_FOUND FALSE CACHE INTERNAL "Zlib found but not usable")
        MESSAGE(STATUS "system zlib found but not usable")
      ENDIF()
    ENDIF()
    IF(NOT ZLIB_FOUND)
      MYSQL_USE_BUNDLED_ZLIB()
    ENDIF()
  ENDIF()
  SET(HAVE_COMPRESS 1)
ENDMACRO()
