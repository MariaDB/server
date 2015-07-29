# Copyright(C) 2013-2015 Brazil
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

if(GRN_WITH_MRUBY)
  include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/../mruby-source/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mruby-source/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mruby-source/mrbgems/mruby-compiler/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../onigmo-source"
  )

  read_file_list("${CMAKE_CURRENT_SOURCE_DIR}/sources.am" MRUBY_SOURCES)
  string(REGEX REPLACE "\\.\\./" "${CMAKE_CURRENT_SOURCE_DIR}/../"
    MRUBY_SOURCES "${MRUBY_SOURCES}")

  read_file_list("${CMAKE_CURRENT_SOURCE_DIR}/built_sources.am"
    MRUBY_BUILT_SOURCES)
  set(mruby_pre_build_timestamp
    "${CMAKE_CURRENT_SOURCE_DIR}/mruby_build.timestamp")
  if(EXISTS "${mruby_pre_build_timestamp}")
    string(REGEX REPLACE "([^;]+)" "${CMAKE_CURRENT_SOURCE_DIR}/\\1"
      MRUBY_BUILT_SOURCES "${MRUBY_BUILT_SOURCES}")
    include_directories(
      "${CMAKE_CURRENT_SOURCE_DIR}/mruby-io/include"
      )
  else()
    set(mruby_build_timestamp
      "${CMAKE_CURRENT_BINARY_DIR}/mruby_build.timestamp")
    if(NOT EXISTS "${mruby_build_timestamp}")
      execute_process(
	COMMAND
	"${RUBY}"
	"${CMAKE_CURRENT_SOURCE_DIR}/mruby_build.rb"
	"${CMAKE_CURRENT_SOURCE_DIR}/build_config.rb"
	"${CMAKE_CURRENT_SOURCE_DIR}/../mruby-source"
	"${CMAKE_CURRENT_BINARY_DIR}/../mruby-build"
	"${CMAKE_CURRENT_SOURCE_DIR}/../onigmo-source"
	"${mruby_build_timestamp}"
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	RESULT_VARIABLE MRUBY_BUILD_RESULT
	)
      if(NOT MRUBY_BUILD_RESULT EQUAL "0")
	message(FATAL_ERROR "Failed to build mruby files")
      endif()
    endif()
    string(REGEX REPLACE "([^;]+)" "${CMAKE_CURRENT_BINARY_DIR}/\\1"
      MRUBY_BUILT_SOURCES "${MRUBY_BUILT_SOURCES}")
    include_directories(
      "${CMAKE_CURRENT_BINARY_DIR}/mruby-io/include"
      )
  endif()

  set(MRUBY_ALL_SOURCES
    ${MRUBY_SOURCES}
    ${MRUBY_BUILT_SOURCES}
    )
  add_library(mruby STATIC ${MRUBY_ALL_SOURCES})
  set_source_files_properties(${MRUBY_ALL_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  set_target_properties(
    mruby
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()
