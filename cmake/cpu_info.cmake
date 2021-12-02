# Copyright (c) 2009, 2011, 2012 Oracle and/or its affiliates. All rights reserved.
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

# Symbols with information about the CPU.

IF(NOT DEFINED CPU_LEVEL1_DCACHE_LINESIZE)

IF(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  FIND_PROGRAM(SYSCTL sysctl)
  MARK_AS_ADVANCED(SYSCTL)

  IF(SYSCTL)
    EXECUTE_PROCESS(
      COMMAND ${SYSCTL} -n hw.cachelinesize
      OUTPUT_VARIABLE CPU_LEVEL1_DCACHE_LINESIZE
      )
  ENDIF()

ELSE()
  FIND_PROGRAM(GETCONF getconf)
  MARK_AS_ADVANCED(GETCONF)

  IF(GETCONF)
    EXECUTE_PROCESS(
      COMMAND ${GETCONF} LEVEL1_DCACHE_LINESIZE
      OUTPUT_VARIABLE CPU_LEVEL1_DCACHE_LINESIZE
      )
  ENDIF()
ENDIF()

ENDIF()
