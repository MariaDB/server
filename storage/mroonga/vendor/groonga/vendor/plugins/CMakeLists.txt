# Copyright(C) 2013 Brazil
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License version 2.1 as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

file(GLOB
  PLUGIN_CMAKE_LISTS_LIST
  "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
if(PLUGIN_CMAKE_LISTS_LIST)
  foreach(PLUGIN_CMAKE_LISTS "${PLUGIN_CMAKE_LISTS_LIST}")
    string(REGEX REPLACE
      "(^${CMAKE_CURRENT_SOURCE_DIR}/+|/+CMakeLists\\.txt$)" ""
      PLUGIN_DIR
      "${PLUGIN_CMAKE_LISTS}")
    add_subdirectory("${PLUGIN_DIR}")
  endforeach(PLUGIN_CMAKE_LISTS)
endif()
