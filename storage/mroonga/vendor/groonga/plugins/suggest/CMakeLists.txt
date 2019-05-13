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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
  ${MRUBY_INCLUDE_DIRS}
  ${MESSAGE_PACK_INCLUDE_DIRS})

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/sources.am SUGGEST_SOURCES)
set_source_files_properties(${SUGGEST_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
if(GRN_EMBED)
  add_library(suggest STATIC ${SUGGEST_SOURCES})
  set_target_properties(
    suggest
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
else()
  add_library(suggest MODULE ${SUGGEST_SOURCES})
  set_target_properties(suggest PROPERTIES PREFIX "")
  install(TARGETS suggest DESTINATION "${GRN_RELATIVE_PLUGINS_DIR}/suggest")
endif()
target_link_libraries(suggest libgroonga)
