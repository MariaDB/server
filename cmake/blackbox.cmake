# Copyright (c) 2019-2024, Codership Oy <info@codership.com>.
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
#
# Definitions for the Black Box debug tool
# Currently enabled for Galera only
#
IF (WITH_WSREP)
  OPTION(WITH_BLACKBOX "Compile with Black Box debug feature" ON)

  IF(WITH_BLACKBOX)
    ADD_DEFINITIONS(-DWITH_BLACKBOX)
    SET(BB_ROOT ${CMAKE_SOURCE_DIR}/blackbox)
    SET(BB_INCLUDE_DIR ${BB_ROOT}/include)
    SET(BB_SOURCES ${BB_ROOT}/src/blackbox.c)
    SET(BB_LIB blackbox)
    INCLUDE_DIRECTORIES(${BB_INCLUDE_DIR})
    ADD_SUBDIRECTORY(blackbox)
    INSTALL(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/blackbox/src/mariadb-bbtool COMPONENT Server DESTINATION ${INSTALL_BINDIR})
  ENDIF()
ENDIF()
