# Copyright (c) 2013, 2017 Percona LLC and/or its affiliates.
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA


INCLUDE_DIRECTORIES(
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/sql
)


ADD_DEFINITIONS(-UMYSQL_SERVER)


MYSQL_ADD_EXECUTABLE(redo-reader
        main.cpp)


# Export all symbols on Unix, for better crash callstacks
SET_TARGET_PROPERTIES(redo-reader PROPERTIES ENABLE_EXPORTS TRUE)

TARGET_LINK_LIBRARIES(redo-reader sql crc)
