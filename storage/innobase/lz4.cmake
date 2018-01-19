# Copyright (C) 2014, SkySQL Ab. All Rights Reserved.
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

SET(WITH_INNODB_LZ4 AUTO CACHE STRING
  "Build with lz4. Possible values are 'ON', 'OFF', 'AUTO' and default is 'AUTO'")

MACRO (MYSQL_CHECK_LZ4)
  IF (WITH_INNODB_LZ4 STREQUAL "ON" OR WITH_INNODB_LZ4 STREQUAL "AUTO")
    find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
    find_library(LZ4_LIBRARY NAMES lz4)
    IF (LZ4_LIBRARY)
    get_filename_component(LZ4_LIBDIR ${LZ4_LIBRARY} DIRECTORY)
#MESSAGE(STATUS "LZ4_INCLUDE_DIR=${LZ4_INCLUDE_DIR} LZ4_LIBRARY=${LZ4_LIBRARY} LZ4_LIBDIR=${LZ4_LIBDIR}")
    IF (LZ4_INCLUDE_DIR)
      SET(HAVE_LZ4_H YES)
    ENDIF()
    CHECK_LIBRARY_EXISTS(lz4 LZ4_compress_limitedOutput ${LZ4_LIBDIR} HAVE_LZ4_SHARED_LIB)
    CHECK_LIBRARY_EXISTS(lz4 LZ4_compress_default ${LZ4_LIBDIR} HAVE_LZ4_COMPRESS_DEFAULT)

    IF (HAVE_LZ4_SHARED_LIB AND HAVE_LZ4_H)
      ADD_DEFINITIONS(-DHAVE_LZ4=1)
      IF (HAVE_LZ4_COMPRESS_DEFAULT)
	ADD_DEFINITIONS(-DHAVE_LZ4_COMPRESS_DEFAULT=1)
      ENDIF()
      LINK_LIBRARIES(innobase ${LZ4_LIBRARY})
    ELSE()
      IF (WITH_INNODB_LZ4 STREQUAL "ON")
	MESSAGE(FATAL_ERROR "Required lz4 library is not found")
      ENDIF()
    ENDIF()
    ENDIF()
  ENDIF()
ENDMACRO()

MACRO (MYSQL_CHECK_LZ4_STATIC)
 IF (WITH_INNODB_LZ4 STREQUAL "ON" OR WITH_INNODB_LZ4 STREQUAL "AUTO")
   CHECK_INCLUDE_FILES(lz4.h HAVE_LZ4_H)
   CHECK_LIBRARY_EXISTS(liblz4.a LZ4_compress_limitedOutput "" HAVE_LZ4_LIB)
   CHECK_LIBRARY_EXISTS(liblz3.a LZ4_compress_default "" HAVE_LZ4_COMPRESS_DEFAULT)

   IF(HAVE_LZ4_LIB AND HAVE_LZ4_H)
     ADD_DEFINITIONS(-DHAVE_LZ4=1)
      IF (HAVE_LZ4_COMPRESS_DEFAULT)
	ADD_DEFINITIONS(-DHAVE_LZ4_COMPRESS_DEFAULT=1)
      ENDIF()
     LINK_LIBRARIES(liblz4.a)
   ELSE()
      IF (WITH_INNODB_LZ4 STREQUAL "ON")
	MESSAGE(FATAL_ERROR "Required lz4 library is not found")
      ENDIF()
   ENDIF()
 ENDIF()
ENDMACRO()
