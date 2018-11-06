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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 

# This file includes GNU/Hurd specific options and quirks, related to system checks

# Something that needs to be set on legacy reasons
SET(_GNU_SOURCE 1)
SET(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} -D_GNU_SOURCE=1)

# Ensure we have clean build for shared libraries
# without unresolved symbols
# Not supported with AddressSanitizer and ThreadSanitizer
IF(NOT WITH_ASAN AND NOT WITH_TSAN)
  SET(LINK_FLAG_NO_UNDEFINED "-Wl,--no-undefined")
ENDIF()

# 64 bit file offset support flag
SET(_FILE_OFFSET_BITS 64)
