# Copyright(C) 2015 Brazil
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
  )

set(GRN_FUNCTIONS_PLUGIN_DIR "${GRN_RELATIVE_PLUGINS_DIR}/functions")

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/vector_sources.am
  VECTOR_SOURCES)
set_source_files_properties(${VECTOR_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
if(GRN_EMBED)
  add_library(vector_functions STATIC ${VECTOR_SOURCES})
  set_target_properties(
    vector_functions
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
else()
  add_library(vector_functions MODULE ${VECTOR_SOURCES})
  set_target_properties(vector_functions PROPERTIES
    PREFIX ""
    OUTPUT_NAME "vector")
  install(TARGETS vector_functions DESTINATION "${GRN_FUNCTIONS_PLUGIN_DIR}")
endif()
target_link_libraries(vector_functions libgroonga)
