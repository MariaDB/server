# Copyright(C) 2013-2015  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
# MA 02110-1335  USA

cmake_minimum_required(VERSION 2.6)
set(GROONGA_NORMALIZER_MYSQL_PROJECT_NAME "groonga-normalizer-mysql")
project("${GROONGA_NORMALIZER_MYSQL_PROJECT_NAME}")

if(DEFINED GROONGA_NORMALIZER_MYSQL_EMBED)
  set(GROONGA_NORMALIZER_MYSQL_EMBED_DEFAULT
    ${GROONGA_NORMALIZER_MYSQL_EMBED})
else()
  set(GROONGA_NORMALIZER_MYSQL_EMBED_DEFAULT OFF)
endif()
option(GROONGA_NORMALIZER_MYSQL_EMBED
  "Build as a static library to embed into an application"
  ${GROONGA_NORMALIZER_MYSQL_EMBED_DEFAULT})

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version_full" VERSION)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(GROONGA_NORMALIZER_MYSQL_BUNDLED FALSE)
else()
  set(GROONGA_NORMALIZER_MYSQL_BUNDLED TRUE)
endif()

if(GROONGA_NORMALIZER_MYSQL_BUNDLED)
  set(GROONGA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
  set(GROONGA_LIBRARY_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/lib")
  set(GROONGA_LIBRARIES "libgroonga")
  set(GROONGA_PLUGINS_DIR "${GRN_RELATIVE_PLUGINS_DIR}")
else()
  include(FindPkgConfig)
  include(${CMAKE_CURRENT_SOURCE_DIR}/build/cmake_modules/ReadFileList.cmake)

  file(READ
    ${CMAKE_CURRENT_SOURCE_DIR}/required_groonga_version
    GROONGA_REQUIRED_VERSION)
  string(STRIP "${GROONGA_REQUIRED_VERSION}" GROONGA_REQUIRED_VERSION)

  pkg_check_modules(GROONGA REQUIRED "groonga >= ${GROONGA_REQUIRED_VERSION}")
  _pkgconfig_invoke(groonga GROONGA PLUGINS_DIR "" --variable=pluginsdir)
endif()

include_directories(
  ${CMAKE_BINARY_DIR}
  ${GROONGA_INCLUDE_DIRS})

link_directories(
  ${GROONGA_LIBRARY_DIRS})

add_subdirectory(normalizers)

if(NOT GROONGA_NORMALIZER_MYSQL_EMBED)
  configure_file(
    groonga-normalizer-mysql.pc.in
    "${CMAKE_CURRENT_BINARY_DIR}/groonga-normalizer-mysql.pc"
    @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/groonga-normalizer-mysql.pc"
    DESTINATION "lib/pkgconfig/")
endif()

install(FILES
  "README.md"
  "doc/text/lgpl-2.0.txt"
  DESTINATION "share/${GROONGA_NORMALIZER_MYSQL_PROJECT_NAME}")
