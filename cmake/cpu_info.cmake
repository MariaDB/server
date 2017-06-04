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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 

# Symbols with information about the CPU.

IF(NOT CPU_LEVEL1_DCACHE_LINESIZE)
  IF(CMAKE_CROSSCOMPILING)
   MESSAGE(FATAL_ERROR
   "CPU_LEVEL1_DCACHE_LINESIZE is not defined.  Please specify -DCPU_LEVEL1_DCACHE_LINESIZE "
   "to the size of the level 1 data cache for the destination architecture.")
  ELSE()
    IF(CMAKE_SYSTEM_NAME MATCHES "Darwin")
      FIND_PROGRAM(SYSCTL sysctl)
      MARK_AS_ADVANCED(SYSCTL)

      IF(SYSCTL)
        EXECUTE_PROCESS(
          COMMAND ${SYSCTL} -n hw.cachelinesize
          OUTPUT_VARIABLE CPU_LEVEL1_DCACHE_LINESIZE
          OUTPUT_STRIP_TRAILING_WHITESPACE
          )
      ENDIF()

    ELSE()
      FIND_PROGRAM(GETCONF getconf)
      MARK_AS_ADVANCED(GETCONF)

      IF(GETCONF)
        EXECUTE_PROCESS(
          COMMAND ${GETCONF} LEVEL1_DCACHE_LINESIZE
          OUTPUT_VARIABLE CPU_LEVEL1_DCACHE_LINESIZE
          OUTPUT_STRIP_TRAILING_WHITESPACE
          )
      ENDIF()
    ENDIF()
  ENDIF()

  IF(CPU_LEVEL1_DCACHE_LINESIZE)
    SET(CPU_LEVEL1_DCACHE_LINESIZE "${CPU_LEVEL1_DCACHE_LINESIZE}" CACHE INTERNAL "CPU level 1 Data cache line size")
    MESSAGE(STATUS "Cache line size discovered to be ${CPU_LEVEL1_DCACHE_LINESIZE}")
  ELSE()
    IF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64(le)?")
      # the getconf results for powerpc where added quite recently in glibc
      # 2017-06-09 in commit cdfbe5037f2f67bf5f560b73732b69d0fabe2314 so the
      # result will be 0 with older glibcs
      SET(CPU_LEVEL1_DCACHE_LINESIZE "128" CACHE INTERNAL "CPU level 1 Data cache line size")
    ELSE()
      SET(CPU_LEVEL1_DCACHE_LINESIZE "64" CACHE INTERNAL "CPU level 1 Data cache line size")
    ENDIF()
    MESSAGE(WARNING "Cache line size could not be discovered so assumed to be ${CPU_LEVEL1_DCACHE_LINESIZE}")
  ENDIF()

ENDIF()
