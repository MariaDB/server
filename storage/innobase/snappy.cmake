# Copyright (C) 2015, MariaDB Corporation. All Rights Reserved.
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

SET(WITH_INNODB_SNAPPY AUTO CACHE STRING
  "Build with snappy. Possible values are 'ON', 'OFF', 'AUTO' and default is 'AUTO'")

MACRO (MYSQL_CHECK_SNAPPY)
  IF (WITH_INNODB_SNAPPY STREQUAL "ON" OR WITH_INNODB_SNAPPY STREQUAL "AUTO")
    find_path(SNAPPY_INCLUDE_DIR NAMES snappy-c.h)
    find_library(SNAPPY_LIBRARY NAMES snappy)
    get_filename_component(SNAPPY_LIBDIR ${SNAPPY_LIBRARY} DIRECTORY)
    IF (SNAPPY_INCLUDE_DIR)
      SET(HAVE_SNAPPY_H 1)
    ENDIF()
    CHECK_LIBRARY_EXISTS(snappy snappy_uncompress ${SNAPPY_LIBDIR} HAVE_SNAPPY_SHARED_LIB)
MESSAGE(STATUS "HAVE_SNAPPY_H=${HAVE_SNAPPY_H} HAVE_SNAPPY_SHARED_LIB=${HAVE_SNAPPY_SHARED_LIB} SNAPPY_LIBDIR=${SNAPPY_LIBDIR} ")
    IF(HAVE_SNAPPY_SHARED_LIB AND HAVE_SNAPPY_H)
      ADD_DEFINITIONS(-DHAVE_SNAPPY=1)
      LINK_LIBRARIES(${SNAPPY_LIBRARY})
    ELSE()
      IF (WITH_INNODB_SNAPPY STREQUAL "ON")
	MESSAGE(FATAL_ERROR "Required snappy library is not found")
      ENDIF()
    ENDIF()
  ENDIF()
ENDMACRO()
