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

MACRO (MYSQL_CHECK_LZ4)

CHECK_INCLUDE_FILES(lz4.h HAVE_LZ4_H)
CHECK_LIBRARY_EXISTS(lz4 LZ4_compress_limitedOutput "" HAVE_LZ4_SHARED_LIB)

IF (HAVE_LZ4_SHARED_LIB AND HAVE_LZ4_H)
  ADD_DEFINITIONS(-DHAVE_LZ4=1)
  LINK_LIBRARIES(lz4) 
ENDIF()
ENDMACRO()

MACRO (MYSQL_CHECK_LZ4_STATIC)
 
 CHECK_INCLUDE_FILES(lz4.h HAVE_LZ4_H)
 CHECK_LIBRARY_EXISTS(liblz4.a LZ4_compress_limitedOutput "" HAVE_LZ4_LIB)

 IF(HAVE_LZ4_LIB AND HAVE_LZ4_H)
   ADD_DEFINITIONS(-DHAVE_LZ4=1)
   LINK_LIBRARIES(liblz4.a)
 ENDIF()
ENDMACRO()