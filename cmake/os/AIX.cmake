# Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
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


#Enable 64 bit file offsets
SET(_LARGE_FILES 1)
SET(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> -X32_64 qc <TARGET> <LINK_FLAGS> <OBJECTS>")
SET(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> -X32_64 q  <TARGET> <LINK_FLAGS> <OBJECTS>")
SET(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> -X32_64 qc <TARGET> <LINK_FLAGS> <OBJECTS>")
SET(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> -X32_64 q  <TARGET> <LINK_FLAGS> <OBJECTS>")

IF(__AIX_COMPILER_XL)
# Fix xlC oddity - it complains about same inline function defined multiple times
# in different compilation units  
INCLUDE(CheckCXXCompilerFlag)
 CHECK_CXX_COMPILER_FLAG("-qstaticinline" HAVE_QSTATICINLINE)
 IF(HAVE_QSTATICINLINE)
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -qstaticinline")
 ENDIF()
ELSE()
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGE_FILES -maix64 -pthread -mcmodel=large")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGE_FILES -maix64 -pthread -mcmodel=large")
ENDIF()

# make it WARN by default, not AUTO (that implies -Werror)
SET(MYSQL_MAINTAINER_MODE "WARN" CACHE STRING "Enable MariaDB maintainer-specific warnings. One of: NO (warnings are disabled) WARN (warnings are enabled) ERR (warnings are errors) AUTO (warnings are errors in Debug only)")
