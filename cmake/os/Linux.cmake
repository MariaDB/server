
# Copyright (c) 2010, 2013, Oracle and/or its affiliates. All rights reserved.
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

# This file includes Linux specific options and quirks, related to system checks

INCLUDE(CheckSymbolExists)

# Something that needs to be set on legacy reasons
SET(TARGET_OS_LINUX 1)
SET(_GNU_SOURCE 1)
SET(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} -D_GNU_SOURCE=1)

# Fix CMake (< 2.8) flags. -rdynamic exports too many symbols.
FOREACH(LANG C CXX)
  STRING(REPLACE "-rdynamic" ""
  CMAKE_SHARED_LIBRARY_LINK_${LANG}_FLAGS
  "${CMAKE_SHARED_LIBRARY_LINK_${LANG}_FLAGS}"
  )
ENDFOREACH()

# Ensure we have clean build for shared libraries
# without unresolved symbols
# Not supported with the clang sanitizers
IF(NOT WITH_ASAN AND NOT WITH_TSAN AND NOT WITH_UBSAN)
  SET(LINK_FLAG_NO_UNDEFINED "-Wl,--no-undefined")
ENDIF()

# 64 bit file offset support flag
SET(_FILE_OFFSET_BITS 64)
