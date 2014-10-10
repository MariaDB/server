# -*- indent-tabs-mode: nil -*-
#
# Copyright(C) 2012-2014 Kouhei Sutou <kou@clear-code.com>
# Copyright(C) 2013 Kentoku SHIBA
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

cmake_minimum_required(VERSION 2.6)
project(mroonga)

if(MSVC)
  if(MSVC_VERSION LESS 1600)
    message(STATUS "Mroonga supports only MSVC 2010 or later")
    return()
  endif()
endif()

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(MRN_BUNDLED FALSE)
else()
  set(MRN_BUNDLED TRUE)
endif()

set(MRN_BUNDLED_GROONGA_RELATIVE_DIR "vendor/groonga")
set(MRN_BUNDLED_GROONGA_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/${MRN_BUNDLED_GROONGA_RELATIVE_DIR}")
if(EXISTS "${MRN_BUNDLED_GROONGA_DIR}")
  set(MRN_GROONGA_BUNDLED TRUE)
  if(MSVC)
    message(STATUS "Bundled Mroonga does not support MSVC yet")
    return()
  endif()
else()
  set(MRN_GROONGA_BUNDLED FALSE)
endif()

set(MRN_PLUGIN_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX})

if(MRN_BUNDLED)
  set(MRN_SOURCE_DIR ${CMAKE_SOURCE_DIR}/storage/mroonga)
else()
  set(MRN_SOURCE_DIR ${CMAKE_SOURCE_DIR})
endif()

file(READ ${MRN_SOURCE_DIR}/version        MRN_VERSION)
file(READ ${MRN_SOURCE_DIR}/version_major  MRN_VERSION_MAJOR)
file(READ ${MRN_SOURCE_DIR}/version_minor  MRN_VERSION_MINOR)
file(READ ${MRN_SOURCE_DIR}/version_micro  MRN_VERSION_MICRO)
file(READ ${MRN_SOURCE_DIR}/version_in_hex MRN_VERSION_IN_HEX)
file(READ ${MRN_SOURCE_DIR}/plugin_version MRN_PLUGIN_VERSION)

if(MRN_GROONGA_BUNDLED)
  add_subdirectory("${MRN_BUNDLED_GROONGA_RELATIVE_DIR}")
else()
  file(READ ${MRN_SOURCE_DIR}/required_groonga_version REQUIRED_GROONGA_VERSION)
  string(STRIP "${REQUIRED_GROONGA_VERSION}" REQUIRED_GROONGA_VERSION)

  file(READ
    ${MRN_SOURCE_DIR}/required_groonga_normalizer_mysql_version
    REQUIRED_GROONGA_NORMALIZER_MYSQL_VERSION)
  string(STRIP
    "${REQUIRED_GROONGA_NORMALIZER_MYSQL_VERSION}"
    REQUIRED_GROONGA_NORMALIZER_MYSQL_VERSION)
endif()

set(MRN_PACKAGE_STRING "${PROJECT_NAME} ${MRN_VERSION}")

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(${MRN_SOURCE_DIR}/build/cmake_modules/ReadFileList.cmake)

set(MRN_C_COMPILE_FLAGS "")
set(MRN_CXX_COMPILE_FLAGS "")

macro(mrn_check_cflag flag)
  set(checking_message "checking for C flag '${flag}'")
  check_c_compiler_flag(${flag} is_available)
  if(${is_available})
    message(STATUS "${checking_message} - available")
    set(MRN_C_COMPILE_FLAGS "${MRN_C_COMPILE_FLAGS} ${flag}")
  else()
    message(STATUS "${checking_message} - not available")
  endif()
endmacro()

macro(mrn_check_cxxflag flag)
  set(checking_message "checking for CXX flag '${flag}'")
  check_cxx_compiler_flag(${flag} is_available)
  if(${is_available})
    message(STATUS "${checking_message} - available")
    set(MRN_CXX_COMPILE_FLAGS "${MRN_CXX_COMPILE_FLAGS} ${flag}")
  else()
    message(STATUS "${checking_message} - not available")
  endif()
endmacro()

macro(mrn_build_flag flag)
  mrn_check_cflag(${flag})
  mrn_check_cxxflag(${flag})
endmacro()

if(MRN_BUNDLED)
  set(MRN_RELATIVE_DIR_PREFIX "${MRN_SOURCE_DIR}/")
else()
  set(MRN_RELATIVE_DIR_PREFIX "")
endif()

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/sources.am MROONGA_SOURCES)
read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/lib/libmrn_no_mysql_sources.am
  LIBMRN_NO_MYSQL_SOURCES)
string(REGEX REPLACE "([^;]+)" "${MRN_RELATIVE_DIR_PREFIX}lib/\\1"
  LIBMRN_NO_MYSQL_SOURCES "${LIBMRN_NO_MYSQL_SOURCES}")
read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/lib/libmrn_need_mysql_sources.am
  LIBMRN_NEED_MYSQL_SOURCES)
string(REGEX REPLACE "([^;]+)" "${MRN_RELATIVE_DIR_PREFIX}lib/\\1"
  LIBMRN_NEED_MYSQL_SOURCES "${LIBMRN_NEED_MYSQL_SOURCES}")
read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/udf/sources.am MRN_UDF_SOURCES)
string(REGEX REPLACE "([^;]+)" "${MRN_RELATIVE_DIR_PREFIX}udf/\\1"
  MRN_UDF_SOURCES "${MRN_UDF_SOURCES}")

if(MRN_BUNDLED)
  set(MYSQL_SOURCE_DIR ${CMAKE_SOURCE_DIR})
  set(MYSQL_BUILD_DIR ${MYSQL_SOURCE_DIR})
  set(MYSQL_CONFIG ${CMAKE_SOURCE_DIR}/scripts/mysql_config)
else()
  set(MYSQL_SOURCE_DIR "/PATH/TO/MYSQL/SOURCE/DIRECTORY/"
    CACHE PATH "MySQL source directory")
  if(NOT EXISTS ${MYSQL_SOURCE_DIR})
    message(FATAL_ERROR
      "MySQL source directory (MYSQL_SOURCE_DIR) doesn't exist: <${MYSQL_SOURCE_DIR}>")
  endif()
  set(MYSQL_BUILD_DIR ${MYSQL_SOURCE_DIR} CACHE PATH "MySQL build directory")
  set(MYSQL_CONFIG "mysql_config" CACHE PATH "mysql-config command path")
endif()
find_path(MYSQL_CONFIG "${MYSQL_CONFIG}")

if(EXISTS "${MYSQL_SOURCE_DIR}/pcre")
  set(MYSQL_REGEX_INCLUDE_DIR "${MYSQL_SOURCE_DIR}/pcre")
else()
  set(MYSQL_REGEX_INCLUDE_DIR "${MYSQL_SOURCE_DIR}/regex")
endif()

set(MYSQL_INCLUDE_DIRS
  "${MYSQL_BUILD_DIR}/include"
  "${MYSQL_SOURCE_DIR}/sql"
  "${MYSQL_SOURCE_DIR}/include"
  "${MYSQL_REGEX_INCLUDE_DIR}"
  "${MYSQL_SOURCE_DIR}"
  CACHE INTERNAL "MySQL include directories")

if(MRN_BUNDLED)
  set(MYSQL_PLUGIN_DIR "${INSTALL_PLUGINDIR}")
  set(MYSQL_SERVICES_LIB_DIR "${MYSQL_BUILD_DIR}/libservices")
  set(MYSQL_CFLAGS "${CMAKE_C_FLAGS}")
  set(MYSQL_VERSION "${MYSQL_BASE_VERSION}")
else()
  macro(SET_MYSQL_CONFIG_VALUE OPTION VARIABLE)
    if(NOT ${VARIABLE})
      execute_process(COMMAND "${MYSQL_CONFIG}" ${OPTION}
       OUTPUT_VARIABLE MYSQL_CONFIG_OUTPUT)
      string(STRIP ${MYSQL_CONFIG_OUTPUT} ${VARIABLE})
    endif()
  endmacro()

  set_mysql_config_value("--plugindir" MYSQL_PLUGIN_DIR)
  set_mysql_config_value("--variable=pkglibdir" MYSQL_PKG_LIB_DIR)
  set(MYSQL_BUILD_LIBSERVICES_DIR "${MYSQL_BUILD_DIR}/libservices")
  if(EXISTS "${MYSQL_BUILD_LIBSERVICES_DIR}/libmysqlservices.a")
    set(MYSQL_SERVICES_LIB_DIR "${MYSQL_BUILD_LIBSERVICES_DIR}")
  else()
    set(MYSQL_SERVICES_LIB_DIR "${MYSQL_PKG_LIB_DIR}")
  endif()
  set_mysql_config_value("--cflags" MYSQL_CFLAGS)
  set_mysql_config_value("--version" MYSQL_VERSION)
endif()

if(${MYSQL_VERSION} VERSION_GREATER "10.0.0" AND
   ${MYSQL_VERSION} VERSION_LESS "10.0.9")
  message(FATAL_ERROR
    "Mroonga doesn't support MariaDB 10.0.0-10.0.8: <${MYSQL_VERSION}>")
  return()
endif()

if(MRN_GROONGA_BUNDLED)
  set(GROONGA_INCLUDE_DIRS "${MRN_BUNDLED_GROONGA_DIR}/include")
  set(GROONGA_LIBRARY_DIRS "${MRN_BUNDLED_GROONGA_DIR}/lib")
  set(GROONGA_LIBRARIES "libgroonga")
  if(EXISTS "${MRN_BUNDLED_GROONGA_DIR}/vendor/plugins/groonga-normalizer-mysql")
    set(GROONGA_NORMALIZER_MYSQL_FOUND TRUE)
  else()
    set(GROONGA_NORMALIZER_MYSQL_FOUND FALSE)
  endif()
else()
  include(FindPkgConfig)
  pkg_check_modules(GROONGA REQUIRED "groonga >= ${REQUIRED_GROONGA_VERSION}")
  pkg_check_modules(GROONGA_NORMALIZER_MYSQL
    "groonga-normalizer-mysql >= ${REQUIRED_GROONGA_NORMALIZER_MYSQL_VERSION}")
endif()

if(GROONGA_NORMALIZER_MYSQL_FOUND AND MRN_GROONGA_BUNDLED)
  read_file_list(${MRN_BUNDLED_GROONGA_DIR}/vendor/plugins/groonga-normalizer-mysql/normalizers/mysql_sources.am MRN_GRN_NORMALIZER_MYSQL_SOURCES)
  string(REGEX REPLACE "([^;]+)" "${MRN_BUNDLED_GROONGA_DIR}/vendor/plugins/groonga-normalizer-mysql/normalizers/\\1"
    MRN_GRN_NORMALIZER_MYSQL_SOURCES "${MRN_GRN_NORMALIZER_MYSQL_SOURCES}")
endif()

include_directories(
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/lib"
  ${MYSQL_INCLUDE_DIRS}
  ${GROONGA_INCLUDE_DIRS})

if(WIN32)
  set(MYSQL_LIBRARY_DIRS
    "${MYSQL_BUILD_DIR}/lib"
    "${MYSQL_BUILD_DIR}/libmysqld")
else()
  set(MYSQL_LIBRARY_DIRS
    "${MYSQL_SERVICES_LIB_DIR}")
endif()
link_directories(
  ${GROONGA_LIBRARY_DIRS}
  ${MYSQL_LIBRARY_DIRS})

if(MRN_BUNDLED)
  if(GROONGA_NORMALIZER_MYSQL_FOUND AND MRN_GROONGA_BUNDLED)
    mysql_add_plugin(ha_mroonga
      "${MROONGA_SOURCES};${MRN_UDF_SOURCES};${MRN_GRN_NORMALIZER_MYSQL_SOURCES};${LIBMRN_NEED_MYSQL_SOURCES};${LIBMRN_NO_MYSQL_SOURCES}"
      STORAGE_ENGINE MODULE_ONLY
      LINK_LIBRARIES ${GROONGA_LIBRARIES}
      MODULE_OUTPUT_NAME "ha_mroonga")
  else()
    mysql_add_plugin(ha_mroonga
      "${MROONGA_SOURCES};${MRN_UDF_SOURCES};${LIBMRN_NEED_MYSQL_SOURCES};${LIBMRN_NO_MYSQL_SOURCES}"
      STORAGE_ENGINE MODULE_ONLY
      LINK_LIBRARIES ${GROONGA_LIBRARIES}
      MODULE_OUTPUT_NAME "ha_mroonga")
  endif()
  set(EXPANDED_INSTALL_LIBDIR "${CMAKE_INSTALL_PREFIX}/${INSTALL_LIBDIR}")
  list(FIND
    CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES
    "${EXPANDED_INSTALL_LIBDIR}"
    EXPANDED_INSTALL_LIBDIR_INDEX_IN_IMPLICIT_LINK_DIRECTORIES)
  if(EXPANDED_INSTALL_LIBDIR_INDEX_IN_IMPLICIT_LINK_DIRECTORIES EQUAL -1)
    set_property(TARGET ha_mroonga APPEND PROPERTY
      INSTALL_RPATH "${EXPANDED_INSTALL_LIBDIR}")
  endif()
else()
  add_library(ha_mroonga MODULE
    ${MROONGA_SOURCES}
    ${MRN_UDF_SOURCES}
    ${LIBMRN_NO_MYSQL_SOURCES}
    ${LIBMRN_NEED_MYSQL_SOURCES})

  set(MYSQL_LIBRARIES "mysqlservices")
  target_link_libraries(ha_mroonga ${GROONGA_LIBRARIES} ${MYSQL_LIBRARIES})

  option(WITH_DEBUG "Enable debug options" OFF)
  if(WITH_DEBUG)
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "SAFE_MUTEX")
    if(CMAKE_COMPILER_IS_GNUCXX)
      set(MRN_C_COMPILE_FLAGS "${MRN_C_COMPILE_FLAGS} -g3 -O0")
      set(MRN_CXX_COMPILE_FLAGS "${MRN_CXX_COMPILE_FLAGS} -g3 -O0")
    endif()
  else()
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "DBUG_OFF")
  endif()

  option(WITH_DEBUG_FULL "Enable full debug options" OFF)
  if(WITH_DEBUG_FULL)
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "SAFE_MUTEX" "SAFEMALLOC")
  endif()

  option(DISABLE_FAST_MUTEXES "Force disabling fast mutex" OFF)
  if(DISABLE_FAST_MUTEXES)
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "FORCE_FAST_MUTEX_DISABLED=1")
  endif()

  option(WITH_FAST_MUTEXES "Enable fast mutex" OFF)
  if(WITH_FAST_MUTEXES)
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "MY_PTHREAD_FASTMUTEX")
  endif()

  if(CMAKE_COMPILER_IS_GNUCXX)
    mrn_build_flag("-Wall")
    mrn_build_flag("-Wextra")
    mrn_build_flag("-Wno-unused-parameter")
    mrn_build_flag("-Wno-strict-aliasing")
    mrn_build_flag("-Wno-deprecated")
    mrn_check_cxxflag("-fno-implicit-templates")
    mrn_check_cxxflag("-fno-exceptions")
    mrn_check_cxxflag("-fno-rtti")
    mrn_check_cxxflag("-felide-constructors")
  endif()
  set_source_files_properties(${MROONGA_SOURCES} PROPERTIES
    COMPILE_FLAGS "${MYSQL_CFLAGS} ${MRN_CXX_COMPILE_FLAGS}")
  set_source_files_properties(${LIBMRN_NEED_MYSQL_SOURCES} PROPERTIES
    COMPILE_FLAGS "${MYSQL_CFLAGS} ${MRN_CXX_COMPILE_FLAGS}")
  set_source_files_properties(${MRN_UDF_SOURCES} PROPERTIES
    COMPILE_FLAGS "${MRN_C_COMPILE_FLAGS}")
  set_source_files_properties(${LIBMRN_NO_MYSQL_SOURCES} PROPERTIES
    COMPILE_FLAGS "${MRN_C_COMPILE_FLAGS}")
  set_property(TARGET ha_mroonga APPEND PROPERTY
    COMPILE_DEFINITIONS "MYSQL_DYNAMIC_PLUGIN")
  set_target_properties(ha_mroonga PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ha_mroonga")

  install(TARGETS ha_mroonga DESTINATION "${MYSQL_PLUGIN_DIR}")
endif()

if(GROONGA_NORMALIZER_MYSQL_FOUND)
  set(WITH_GROONGA_NORMALIZER_MYSQL 1)
  set_property(TARGET ha_mroonga APPEND PROPERTY
    COMPILE_DEFINITIONS "WITH_GROONGA_NORMALIZER_MYSQL=1")
  if(MRN_GROONGA_BUNDLED)
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "GROONGA_NORMALIZER_MYSQL_PLUGIN_IS_BUNDLED_STATIC=1")
  else()
    set(GROONGA_NORMALIZER_MYSQL_PLUGIN_NAME \"normalizers/mysql\")
    set_property(TARGET ha_mroonga APPEND PROPERTY
      COMPILE_DEFINITIONS "GROONGA_NORMALIZER_MYSQL_PLUGIN_NAME=\"normalizers/mysql\"")
  endif()
endif()

set(DEFAULT_PARSER "TokenBigram" CACHE STRING "The default fulltext parser")
set_property(TARGET ha_mroonga APPEND PROPERTY
  COMPILE_DEFINITIONS "MRN_PARSER_DEFAULT=\"${DEFAULT_PARSER}\"")

configure_file(
  "${PROJECT_SOURCE_DIR}/mrn_version.h.in"
  "${PROJECT_BINARY_DIR}/mrn_version.h")

configure_file(
  "${PROJECT_SOURCE_DIR}/config.sh.in"
  "${PROJECT_BINARY_DIR}/config.sh")

set(MRN_TEST_SUITE_DIR "${CMAKE_SOURCE_DIR}/mysql-test/suite/mroonga")
if(NOT EXISTS "${MRN_TEST_SUITE_DIR}")
  set(MRN_TEST_SUITE_DIR "${PROJECT_SOURCE_DIR}/mysql-test/mroonga")
endif()
configure_file(
  "${MRN_TEST_SUITE_DIR}/storage/r/information_schema_plugins.result.in"
  "${MRN_TEST_SUITE_DIR}/storage/r/information_schema_plugins.result"
  NEWLINE_STYLE LF)
configure_file(
  "${MRN_TEST_SUITE_DIR}/storage/r/variable_version.result.in"
  "${MRN_TEST_SUITE_DIR}/storage/r/variable_version.result"
  NEWLINE_STYLE LF)

configure_file(
  "${PROJECT_SOURCE_DIR}/data/install.sql.in"
  "${PROJECT_BINARY_DIR}/data/install.sql")

if(MRN_BUNDLED)
  set(MRN_DATA_DIR "${INSTALL_MYSQLSHAREDIR}/${PROJECT_NAME}")
else()
  set(MRN_DATA_DIR "share/${PROJECT_NAME}")
endif()
install(FILES
  "${PROJECT_BINARY_DIR}/data/install.sql"
  "${PROJECT_SOURCE_DIR}/data/uninstall.sql"
  DESTINATION "${MRN_DATA_DIR}/")
