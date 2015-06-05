# Copyright(C) 2012-2013 Brazil
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
  ${MRUBY_INCLUDE_DIRS})

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/sources.am TABLE_SOURCES)
add_library(table MODULE ${TABLE_SOURCES})
set_source_files_properties(${TABLE_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
set_target_properties(table PROPERTIES PREFIX "")
target_link_libraries(table libgroonga)
install(TARGETS table DESTINATION "${GRN_RELATIVE_PLUGINS_DIR}/table")
