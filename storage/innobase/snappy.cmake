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
# 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

SET(WITH_INNODB_SNAPPY AUTO CACHE STRING
  "Build with snappy. Possible values are 'ON', 'OFF', 'AUTO' and default is 'AUTO'")

MACRO (MYSQL_CHECK_SNAPPY)
  IF (WITH_INNODB_SNAPPY STREQUAL "ON" OR WITH_INNODB_SNAPPY STREQUAL "AUTO")
    CHECK_INCLUDE_FILES(snappy-c.h HAVE_SNAPPY_H)
    CHECK_LIBRARY_EXISTS(snappy snappy_uncompress "" HAVE_SNAPPY_SHARED_LIB)

    IF(HAVE_SNAPPY_SHARED_LIB AND HAVE_SNAPPY_H)
      SET(HAVE_INNODB_SNAPPY TRUE)
      ADD_DEFINITIONS(-DHAVE_SNAPPY=1)
      LINK_LIBRARIES(snappy)
    ELSE()
      IF (WITH_INNODB_SNAPPY STREQUAL "ON")
        MESSAGE(FATAL_ERROR "Required snappy library is not found")
      ENDIF()
    ENDIF()
  ENDIF()
  ADD_FEATURE_INFO(INNODB_SNAPPY HAVE_INNODB_SNAPPY "Snappy compression in the InnoDB storage engine")
ENDMACRO()
