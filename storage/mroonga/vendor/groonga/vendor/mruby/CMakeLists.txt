# Copyright(C) 2013-2016 Brazil
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

if(GRN_WITH_MRUBY)
  set(MRUBY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mruby-source")

  include_directories(
    "${MRUBY_SOURCE_DIR}/include"
    "${MRUBY_SOURCE_DIR}/src"
    "${MRUBY_SOURCE_DIR}/mrbgems/mruby-compiler/core"
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
    set(MRUBY_LEGAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/LEGAL")
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
	"${MRUBY_SOURCE_DIR}"
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
    set(MRUBY_LEGAL_FILE "${CMAKE_CURRENT_BINARY_DIR}/LEGAL")
    string(REGEX REPLACE "([^;]+)" "${CMAKE_CURRENT_BINARY_DIR}/\\1"
      MRUBY_BUILT_SOURCES "${MRUBY_BUILT_SOURCES}")
    include_directories(
      "${CMAKE_CURRENT_BINARY_DIR}/mruby-io/include"
      )
  endif()
  file(WRITE
    "${CMAKE_CURRENT_BINARY_DIR}/mruby-file-stat/src/config.h"
    "")

  set(MRUBY_ALL_SOURCES
    ${MRUBY_SOURCES}
    ${MRUBY_BUILT_SOURCES}
    )
  add_library(mruby STATIC ${MRUBY_ALL_SOURCES})
  set_target_properties(
    mruby
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

  set(MRUBY_C_COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_CLANGCC)
    set(MRUBY_C_COMPILE_FLAGS
      "${MRUBY_C_COMPILE_FLAGS} -Wno-float-equal")
    set(MRUBY_C_COMPILE_FLAGS
      "${MRUBY_C_COMPILE_FLAGS} -Wno-bad-function-cast")
  endif()
  if(WIN32)
    set(MRUBY_DEFINITIONS ${MRUBY_DEFINITIONS} MRB_BUILD_AS_DLL MRB_CORE)
  endif()
  set_source_files_properties(${MRUBY_ALL_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${MRUBY_C_COMPILE_FLAGS}")
  set_property(SOURCE ${MRUBY_ALL_SOURCES}
    PROPERTY COMPILE_DEFINITIONS ${MRUBY_DEFINITIONS})

  install(FILES
    "${MRUBY_LEGAL_FILE}"
    DESTINATION "${GRN_DATA_DIR}/mruby")
endif()
