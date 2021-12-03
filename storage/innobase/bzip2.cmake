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
# 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

SET(WITH_INNODB_BZIP2 AUTO CACHE STRING
  "Build with bzip2. Possible values are 'ON', 'OFF', 'AUTO' and default is 'AUTO'")

MACRO (MYSQL_CHECK_BZIP2)
  IF (WITH_INNODB_BZIP2 STREQUAL "ON" OR WITH_INNODB_BZIP2 STREQUAL "AUTO")
    CHECK_INCLUDE_FILES(bzlib.h HAVE_BZLIB2_H)
    CHECK_LIBRARY_EXISTS(bz2 BZ2_bzBuffToBuffCompress "" HAVE_BZLIB2_COMPRESS)
    CHECK_LIBRARY_EXISTS(bz2 BZ2_bzBuffToBuffDecompress "" HAVE_BZLIB2_DECOMPRESS)

    IF (HAVE_BZLIB2_COMPRESS AND HAVE_BZLIB2_DECOMPRESS AND HAVE_BZLIB2_H)
      SET(HAVE_INNODB_BZLIB2 TRUE)
      ADD_DEFINITIONS(-DHAVE_BZIP2=1)
      LINK_LIBRARIES(bz2) 
    ELSE()
      IF (WITH_INNODB_BZIP2 STREQUAL "ON")
        MESSAGE(FATAL_ERROR "Required bzip2 library is not found")
      ENDIF()
    ENDIF()
  ENDIF()
  ADD_FEATURE_INFO(INNODB_BZIP2 HAVE_INNODB_BZLIB2
                   "BZIP2 compression in the InnoDB storage engine")
ENDMACRO()
