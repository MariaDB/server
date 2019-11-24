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
  ${MRUBY_INCLUDE_DIRS}
  ${MESSAGE_PACK_INCLUDE_DIRS}
  )

add_subdirectory(suggest)

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/groonga_sources.am GROONGA_SOURCES)
add_executable(groonga ${GROONGA_SOURCES})
set_source_files_properties(${GROONGA_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
target_link_libraries(groonga libgroonga)
install(TARGETS groonga DESTINATION ${BIN_DIR})

if(GRN_WITH_MRUBY)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/grndb_sources.am GRNDB_SOURCES)
  add_executable(grndb ${GRNDB_SOURCES})
  set_source_files_properties(${GRNDB_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  set_source_files_properties(${GRNDB_SOURCES}
    PROPERTIES
    COMPILE_DEFINITIONS "${MRUBY_DEFINITIONS}")
  target_link_libraries(grndb libgroonga)
  install(TARGETS grndb DESTINATION ${BIN_DIR})
endif()

if(NOT WIN32)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/grnslap_sources.am GRNSLAP_SOURCES)
  add_executable(grnslap ${GRNSLAP_SOURCES})
  set_source_files_properties(${GRNSLAP_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  target_link_libraries(grnslap libgroonga)
  install(TARGETS grnslap DESTINATION ${BIN_DIR})
endif()

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/groonga_benchmark_sources.am
  GROONGA_BENCHMARK_SOURCES)
add_executable(groonga-benchmark ${GROONGA_BENCHMARK_SOURCES})
set_source_files_properties(${GROONGA_BENCHMARK_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
target_link_libraries(groonga-benchmark libgroonga)
install(TARGETS groonga-benchmark DESTINATION ${BIN_DIR})

