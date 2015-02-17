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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

add_definitions(
  -DDISABLE_GEMS
  )

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}/../mruby-${MRUBY_VERSION}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../mruby-${MRUBY_VERSION}/src"
  )

if(GRN_WITH_MRUBY)
  read_file_list("${CMAKE_CURRENT_SOURCE_DIR}/sources.am" MRUBY_SOURCES)
  string(REGEX REPLACE "\\.\\./" "${CMAKE_CURRENT_SOURCE_DIR}/../"
    MRUBY_SOURCES "${MRUBY_SOURCES}")
  add_library(mruby OBJECT ${MRUBY_SOURCES})
  set_source_files_properties(${MRUBY_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  set_target_properties(
    mruby
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()
