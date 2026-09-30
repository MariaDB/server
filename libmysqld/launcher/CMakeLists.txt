# Copyright (c) 2026, MariaDB plc
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

# MDEV-11111: embedded server as a private mariadbd process.
# Client side only, does not depend on the server sources.
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/libmariadb/include
                    ${CMAKE_BINARY_DIR}/libmariadb/include)
ADD_LIBRARY(mariadb_embedded_launcher STATIC
            embedded_launcher.c embedded_glue.c)
TARGET_LINK_LIBRARIES(mariadb_embedded_launcher ${CMAKE_DL_LIBS})
