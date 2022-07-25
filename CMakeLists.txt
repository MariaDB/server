# Copyright (c) 2006, 2017, Oracle and/or its affiliates.
# Copyright (c) 2008, 2019, MariaDB Corporation.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12)

IF(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # Setting build type to RelWithDebInfo as none was specified.
  # Must occur before PROJECT
  SET(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
   "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel"
   FORCE)
  # Set the possible values of build type for cmake-gui
  SET_PROPERTY(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
   "None" "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
ENDIF()

PROJECT(MySQL)

# explicitly set the policy to OLD
# (cannot use NEW, not everyone is on cmake-2.8.12 yet)
IF(POLICY CMP0022)
  CMAKE_POLICY(SET CMP0022 OLD)
ENDIF()

# We use the LOCATION target property (CMP0026)
# and get_target_property() for non-existent targets (CMP0045)
# and INSTALL_NAME_DIR (CMP0042)
IF(CMAKE_VERSION VERSION_EQUAL "3.0.0" OR
   CMAKE_VERSION VERSION_GREATER "3.0.0")
 CMAKE_POLICY(SET CMP0026 OLD)
 CMAKE_POLICY(SET CMP0045 OLD)
 CMAKE_POLICY(SET CMP0042 OLD)
ENDIF()
IF(POLICY CMP0048)
  CMAKE_POLICY(SET CMP0048 NEW)
ENDIF()
IF(POLICY CMP0054)
  CMAKE_POLICY(SET CMP0054 NEW)
ENDIF()
IF(POLICY CMP0075)
  CMAKE_POLICY(SET CMP0075 NEW)
ENDIF()

MESSAGE(STATUS "Running cmake version ${CMAKE_VERSION}")

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_SOURCE_DIR}/cmake/Internal/CPack)

# Use a default manufacturer if no manufacturer was identified.
IF(NOT DEFINED MANUFACTURER) 
  SET(MANUFACTURER "Built from Source" CACHE  STRING
     "Set the entity that appears as the manufacturer of packages that support a manufacturer field.")
  MARK_AS_ADVANCED(MANUFACTURER)
ENDIF()

# MAX_INDEXES - Set the maximum number of indexes per table, default 64
IF (NOT MAX_INDEXES)
  SET(MAX_INDEXES 64)
ENDIF(NOT MAX_INDEXES)

IF (${MAX_INDEXES} GREATER 128)
  MESSAGE(FATAL_ERROR "MAX_INDEXES values greater than 128 is not supported!")
ENDIF()

IF(UNIX AND NOT APPLE)
  # Note, that generally one should not change settings depending
  # on CMAKE_BUILD_TYPE, because VS and Xcode configure once (with
  # the empty CMAKE_BUILD_TYPE) and the build many times for
  # different build types without re-running cmake!
  # But we only care about WITH_PIC on Unix, where the check for
  # CMAKE_BUILD_TYPE hapen to work.
  IF (CMAKE_BUILD_TYPE MATCHES "Debug")
    SET(WITH_PIC_DEFAULT ON)
  ELSE()
    SET(WITH_PIC_DEFAULT OFF)
  ENDIF()
  # Compiling with PIC speeds up embedded build, on PIC sensitive systems 
  # Predefine it to OFF in release builds, because of the performance penalty
  SET(WITH_PIC ${WITH_PIC_DEFAULT} CACHE BOOL "Compile with PIC.")
  MARK_AS_ADVANCED(WITH_PIC)
ENDIF()

# This is used by TokuDB only
SET(MYSQL_PROJECT_NAME_DOCSTRING "MySQL project name")

SET(CPACK_PACKAGE_NAME "MariaDB")
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MariaDB: a very fast and robust SQL database server")
SET(CPACK_PACKAGE_URL "http://mariadb.org")

IF(BUILD_CONFIG)
  INCLUDE(
  ${CMAKE_SOURCE_DIR}/cmake/build_configurations/${BUILD_CONFIG}.cmake)
ENDIF()

# Include the platform-specific file. To allow exceptions, this code
# looks for files in order of how specific they are. If there is, for
# example, a generic Linux.cmake and a version-specific
# Linux-2.6.28-11-generic, it will pick Linux-2.6.28-11-generic and
# include it. It is then up to the file writer to include the generic
# version if necessary.
FOREACH(_base
    ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_VERSION}-${CMAKE_SYSTEM_PROCESSOR}
    ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_VERSION}
    ${CMAKE_SYSTEM_NAME})
  SET(_file ${CMAKE_SOURCE_DIR}/cmake/os/${_base}.cmake)
  IF(EXISTS ${_file})
    INCLUDE(${_file})
    BREAK()
  ENDIF()
ENDFOREACH()

IF(NOT RPM AND NOT DEB)
FOREACH(tool gtar tar git)
 STRING(TOUPPER ${tool}  TOOL)
 FIND_PROGRAM(${TOOL}_EXECUTABLE ${tool} DOC "path to the executable")
 MARK_AS_ADVANCED(${TOOL}_EXECUTABLE)
ENDFOREACH()
ENDIF()

# Following autotools tradition, add preprocessor definitions
# specified in environment variable CPPFLAGS
IF(DEFINED ENV{CPPFLAGS})
  ADD_DEFINITIONS($ENV{CPPFLAGS})
ENDIF()

SET(MYSQL_MAINTAINER_MODE "AUTO" CACHE STRING "Enable MariaDB maintainer-specific warnings. One of: NO (warnings are disabled) WARN (warnings are enabled) ERR (warnings are errors) AUTO (warnings are errors in Debug only)")

# Packaging
IF (NOT CPACK_GENERATOR)
  IF(WIN32)
    SET(CPACK_GENERATOR "ZIP")
  ELSE()
    SET(CPACK_GENERATOR "TGZ")
  ENDIF(WIN32) 
ENDIF(NOT CPACK_GENERATOR)

INCLUDE(FeatureSummary)
INCLUDE(misc)
INCLUDE(mysql_version)
INCLUDE(cpack_source_ignore_files)
INCLUDE(install_layout)
INCLUDE(wsrep)

# Add macros
INCLUDE(character_sets)
INCLUDE(cpu_info)
INCLUDE(zlib)
INCLUDE(ssl)
INCLUDE(readline)
INCLUDE(libutils)
INCLUDE(dtrace)
INCLUDE(pcre)
INCLUDE(ctest)
INCLUDE(plugin)
INCLUDE(install_macros)
INCLUDE(systemd)
INCLUDE(mysql_add_executable)
INCLUDE(compile_flags)
INCLUDE(crc32-vpmsum)

# Handle options
OPTION(DISABLE_SHARED 
 "Don't build shared libraries, compile code as position-dependent" OFF)
IF(DISABLE_SHARED)
  SET(WITHOUT_DYNAMIC_PLUGINS 1)
ENDIF()
OPTION(ENABLED_PROFILING "Enable profiling" ON)
OPTION(WITHOUT_SERVER "Build only the client library and clients" OFF)
IF(UNIX)
  OPTION(WITH_VALGRIND "Valgrind instrumentation" OFF)
ENDIF()
OPTION (WITH_UNIT_TESTS "Compile MySQL with unit tests" ON)

IF (WITHOUT_SERVER)
  SET (SKIP_COMPONENTS "Server|IniFiles|SuportFiles|Readme")
ELSE()
  SET (SKIP_COMPONENTS "N-O-N-E")
ENDIF()

OPTION(NOT_FOR_DISTRIBUTION "Allow linking with GPLv2-incompatible system libraries. Only set it you never plan to distribute the resulting binaries" OFF)

INCLUDE(check_compiler_flag)
INCLUDE(check_linker_flag)

OPTION(WITH_ASAN "Enable address sanitizer" OFF)
IF (WITH_ASAN)
  # this flag might be set by default on some OS
  MY_CHECK_AND_SET_COMPILER_FLAG("-U_FORTIFY_SOURCE" DEBUG RELWITHDEBINFO)
  MY_CHECK_AND_SET_COMPILER_FLAG("-fsanitize=address -fPIC"
    DEBUG RELWITHDEBINFO)
  SET(HAVE_C_FSANITIZE ${have_C__fsanitize_address__fPIC})
  SET(HAVE_CXX_FSANITIZE ${have_CXX__fsanitize_address__fPIC})
  IF(HAVE_C_FSANITIZE AND HAVE_CXX_FSANITIZE)
    OPTION(WITH_ASAN_SCOPE "Enable -fsanitize-address-use-after-scope" OFF)
    SET(WITH_ASAN_OK 1)
    IF(WITH_ASAN_SCOPE)
      MY_CHECK_AND_SET_COMPILER_FLAG(
        "-fsanitize=address -fsanitize-address-use-after-scope"
	DEBUG RELWITHDEBINFO)
    ENDIF()
  ENDIF()

  IF(NOT WITH_ASAN_OK)
    MESSAGE(FATAL_ERROR "Do not know how to enable address sanitizer")
  ENDIF()
ENDIF()

OPTION(WITH_UBSAN "Enable undefined behavior sanitizer" OFF)
IF (WITH_UBSAN)
  MY_CHECK_AND_SET_COMPILER_FLAG("-fsanitize=undefined -fno-sanitize=alignment -U_FORTIFY_SOURCE -DWITH_UBSAN" DEBUG RELWITHDEBINFO)
ENDIF()

OPTION(WITH_MSAN "Enable memory sanitizer" OFF)
IF (WITH_MSAN)
  MY_CHECK_AND_SET_COMPILER_FLAG("-fsanitize=memory -fsanitize-memory-track-origins -U_FORTIFY_SOURCE" DEBUG RELWITHDEBINFO)
ENDIF()

# enable security hardening features, like most distributions do
# in our benchmarks that costs about ~1% of performance, depending on the load
IF(CMAKE_C_COMPILER_VERSION VERSION_LESS "4.6" OR WITH_ASAN OR WITH_UBSAN)
  SET(security_default OFF)
ELSE()
  SET(security_default ON)
ENDIF()
OPTION(SECURITY_HARDENED "Use security-enhancing compiler features (stack protector, relro, etc)" ${security_default})
IF(SECURITY_HARDENED)
  IF(WITH_ASAN OR WITH_UBSAN)
    MESSAGE(FATAL_ERROR "WITH_ASAN/WITH_UBSAN and SECURITY_HARDENED are mutually exclusive")
  ENDIF()
  # security-enhancing flags
  MY_CHECK_AND_SET_COMPILER_FLAG("-pie -fPIC")
  MY_CHECK_AND_SET_LINKER_FLAG("-Wl,-z,relro,-z,now")
  MY_CHECK_AND_SET_COMPILER_FLAG("-fstack-protector --param=ssp-buffer-size=4")
  MY_CHECK_AND_SET_COMPILER_FLAG("-D_FORTIFY_SOURCE=2" RELEASE RELWITHDEBINFO)
ENDIF()

OPTION(WITH_DBUG_TRACE "Enable DBUG_ENTER()/DBUG_EXIT()" ON)
IF(WITH_DBUG_TRACE)
  ADD_DEFINITIONS(-DDBUG_TRACE)
ENDIF()

# Always enable debug sync for debug builds.
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DENABLED_DEBUG_SYNC")
SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DENABLED_DEBUG_SYNC")
 
OPTION(ENABLE_GCOV "Enable gcov (debug, Linux builds only)" OFF)
IF (ENABLE_GCOV)
  MY_CHECK_AND_SET_COMPILER_FLAG("-fprofile-arcs -ftest-coverage -lgcov" DEBUG)
ENDIF()

MY_CHECK_AND_SET_COMPILER_FLAG(-ggdb3 DEBUG)

SET(ENABLED_LOCAL_INFILE "AUTO" CACHE STRING "If we should should enable LOAD DATA LOCAL by default (OFF/ON/AUTO)")
IF (ENABLED_LOCAL_INFILE MATCHES "^(0|FALSE)$")
  SET(ENABLED_LOCAL_INFILE OFF)
ELSEIF(ENABLED_LOCAL_INFILE MATCHES "^(1|TRUE)$")
  SET(ENABLED_LOCAL_INFILE ON)
ELSEIF (NOT ENABLED_LOCAL_INFILE MATCHES "^(ON|OFF|AUTO)$")
  MESSAGE(FATAL_ERROR "ENABLED_LOCAL_INFILE must be one of OFF, ON, AUTO")
ENDIF()

# Set DBUG_OFF and other optional release-only flags for non-debug project types
FOREACH(BUILD_TYPE RELEASE RELWITHDEBINFO MINSIZEREL)
  FOREACH(LANG C CXX)
    IF (NOT CMAKE_${LANG}_FLAGS_${BUILD_TYPE} MATCHES "DDBUG_" AND
        NOT CMAKE_${LANG}_FLAGS MATCHES "DDBUG_")
      SET(CMAKE_${LANG}_FLAGS_${BUILD_TYPE} 
       "${CMAKE_${LANG}_FLAGS_${BUILD_TYPE}} -DDBUG_OFF")
    ENDIF()
  ENDFOREACH()
ENDFOREACH()

# Add safemutex for debug configurations, except on Windows
# (safemutex has never worked on Windows)
IF(NOT WIN32)
  FOREACH(LANG C CXX)
      SET(CMAKE_${LANG}_FLAGS_DEBUG "${CMAKE_${LANG}_FLAGS_DEBUG} -DSAFE_MUTEX")
  ENDFOREACH()
ENDIF()

# safemalloc can be enabled and disabled independently
SET(WITH_SAFEMALLOC "AUTO" CACHE STRING "Use safemalloc memory debugger. Will result in slower execution. Options are: ON OFF AUTO.")

IF(WITH_SAFEMALLOC MATCHES "ON")
  ADD_DEFINITIONS( -DSAFEMALLOC)
ELSEIF(WITH_SAFEMALLOC MATCHES "AUTO" AND NOT WIN32 AND NOT WITH_VALGRIND)
  SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DSAFEMALLOC")
  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DSAFEMALLOC")
ENDIF()

# Set commonly used variables
IF(WIN32)
  SET(DEFAULT_MYSQL_HOME "C:/Program Files/MariaDB ${MYSQL_BASE_VERSION}")
  SET(SHAREDIR share)
ELSE()
  SET(DEFAULT_MYSQL_HOME ${CMAKE_INSTALL_PREFIX})
  SET(SHAREDIR ${INSTALL_MYSQLSHAREDIRABS})
ENDIF()

SET(DEFAULT_BASEDIR "${DEFAULT_MYSQL_HOME}")
IF(INSTALL_MYSQLDATADIR MATCHES "^/.*")
  SET(MYSQL_DATADIR ${INSTALL_MYSQLDATADIR} CACHE PATH "default MySQL data directory")
ELSE()
  SET(MYSQL_DATADIR "${DEFAULT_MYSQL_HOME}/${INSTALL_MYSQLDATADIR}" CACHE PATH "default MySQL data directory")
ENDIF()
SET(DEFAULT_CHARSET_HOME "${DEFAULT_MYSQL_HOME}")
SET(PLUGINDIR "${DEFAULT_MYSQL_HOME}/${INSTALL_PLUGINDIR}")
IF(INSTALL_SYSCONFDIR AND NOT DEFAULT_SYSCONFDIR AND NOT DEB)
  SET(DEFAULT_SYSCONFDIR "${INSTALL_SYSCONFDIR}")
ENDIF()

SET(TMPDIR ""
  CACHE PATH
  "PATH to MySQL TMP dir. Defaults to the P_tmpdir macro in <stdio.h>")
IF(TMPDIR STREQUAL "")
  # Do not quote it, to refer to the P_tmpdir macro.
  SET(DEFAULT_TMPDIR "P_tmpdir")
ELSE()
  # Quote it, to make it a const char string.
  SET(DEFAULT_TMPDIR "\"${TMPDIR}\"")
ENDIF()

# Run platform tests
INCLUDE(configure.cmake)

# force -DUSE_MYSYS_NEW unless already done by HAVE_CXX_NEW
IF(NOT HAVE_CXX_NEW)
  ADD_DEFINITIONS(-DUSE_MYSYS_NEW)
ENDIF()

# Find header files from the bundled libraries
# (yassl, readline, pcre, etc)
# before the ones installed in the system
SET(CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE ON)

# Common defines and includes
ADD_DEFINITIONS(-DHAVE_CONFIG_H)
IF(_FILE_OFFSET_BITS)
  ADD_DEFINITIONS(-D_FILE_OFFSET_BITS=${_FILE_OFFSET_BITS})
ENDIF()
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/include)

# Add bundled or system zlib.
MYSQL_CHECK_ZLIB_WITH_COMPRESS()
# Add bundled yassl/taocrypt or system openssl.
MYSQL_CHECK_SSL()
# Add readline or libedit.
MYSQL_CHECK_READLINE()

SET(MALLOC_LIBRARY "system")
SET(PYTHON_SHEBANG "/usr/bin/env python")

CHECK_PCRE()

CHECK_SYSTEMD()

IF(CMAKE_CROSSCOMPILING)
  SET(IMPORT_EXECUTABLES "IMPORTFILE-NOTFOUND" CACHE FILEPATH "Path to import_executables.cmake from a native build")
  INCLUDE(${IMPORT_EXECUTABLES})
ENDIF()

#
# Setup maintainer mode options. Platform checks are
# not run with the warning options as to not perturb fragile checks
# (i.e. do not make warnings into errors).
# We have to add MAINTAINER_C_WARNINGS first to ensure that the flags
# given by the invoking user are honored
#
INCLUDE(maintainer)

IF(WITH_UNIT_TESTS)
 ENABLE_TESTING()
 ADD_SUBDIRECTORY(unittest/mytap)
 ADD_SUBDIRECTORY(unittest/strings)
 ADD_SUBDIRECTORY(unittest/examples)
 ADD_SUBDIRECTORY(unittest/mysys)
 ADD_SUBDIRECTORY(unittest/my_decimal)
 ADD_SUBDIRECTORY(unittest/json_lib)
 IF(NOT WITHOUT_SERVER)
   ADD_SUBDIRECTORY(unittest/sql)
 ENDIF()
ENDIF()

INCLUDE(cpack_tgz)
INCLUDE(cpack_rpm)
INCLUDE(cpack_deb)

SET (MYSQLD_STATIC_PLUGIN_LIBS "" CACHE INTERNAL "")

INCLUDE(submodules)
INCLUDE(mariadb_connector_c) # this does ADD_SUBDIRECTORY(libmariadb)

# Add storage engines and plugins.
CONFIGURE_PLUGINS()

ADD_SUBDIRECTORY(include)
ADD_SUBDIRECTORY(dbug)
ADD_SUBDIRECTORY(strings)
ADD_SUBDIRECTORY(vio)
ADD_SUBDIRECTORY(mysys)
ADD_SUBDIRECTORY(mysys_ssl)
ADD_SUBDIRECTORY(client)
ADD_SUBDIRECTORY(extra)
ADD_SUBDIRECTORY(libservices)
ADD_SUBDIRECTORY(sql/share)

IF(NOT WITHOUT_SERVER)
  ADD_SUBDIRECTORY(tests)
  ADD_SUBDIRECTORY(sql)
  OPTION (WITH_EMBEDDED_SERVER "Compile MariaDB with embedded server" OFF)
  IF(WITH_EMBEDDED_SERVER) 
    ADD_SUBDIRECTORY(libmysqld)
    ADD_SUBDIRECTORY(libmysqld/examples)
  ENDIF(WITH_EMBEDDED_SERVER)
  IF(NOT WIN32)
    ADD_FEATURE_INFO(EMBEDDED_SERVER WITH_EMBEDDED_SERVER "Embedded MariaDB Server Library")
  ENDIF()

  IF(WITH_WSREP)
    ADD_SUBDIRECTORY(wsrep)
  ENDIF()

  ADD_SUBDIRECTORY(mysql-test)
  ADD_SUBDIRECTORY(mysql-test/lib/My/SafeProcess)
  ADD_SUBDIRECTORY(sql-bench)
  IF(WIN32)
    ADD_SUBDIRECTORY(win/upgrade_wizard)
    ADD_SUBDIRECTORY(win/packaging)
  ENDIF()
ENDIF()

IF(UNIX)
  ADD_SUBDIRECTORY(man)
ENDIF()

INCLUDE(cmake/abi_check.cmake)
INCLUDE(cmake/tags.cmake)




INCLUDE(for_clients)
ADD_SUBDIRECTORY(scripts)
ADD_SUBDIRECTORY(support-files)

IF(NOT CMAKE_CROSSCOMPILING)
  SET(EXPORTED comp_err comp_sql factorial)
  IF(NOT WITHOUT_SERVER)
    SET(EXPORTED ${EXPORTED} gen_lex_hash gen_lex_token)
  ENDIF()
  # minimal target to build only binaries for export
  ADD_CUSTOM_TARGET(import_executables DEPENDS ${EXPORTED})
  EXPORT(TARGETS ${EXPORTED} FILE ${CMAKE_BINARY_DIR}/import_executables.cmake)
ENDIF()

CONFIGURE_FILE(config.h.cmake   ${CMAKE_BINARY_DIR}/include/my_config.h)
CONFIGURE_FILE(config.h.cmake   ${CMAKE_BINARY_DIR}/include/config.h)
CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/include/mysql_version.h.in
               ${CMAKE_BINARY_DIR}/include/mysql_version.h )
CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/sql/sql_builtin.cc.in
    ${CMAKE_BINARY_DIR}/sql/sql_builtin.cc)
CONFIGURE_FILE(
    ${CMAKE_SOURCE_DIR}/cmake/info_macros.cmake.in
    ${CMAKE_BINARY_DIR}/info_macros.cmake @ONLY)

# Handle the "INFO_*" files.
INCLUDE(${CMAKE_BINARY_DIR}/info_macros.cmake)
# Source: This can be done during the cmake phase, all information is
# available, but should be repeated on each "make" just in case someone
# does "cmake ; make ; git pull ; make".
CREATE_INFO_SRC(${CMAKE_BINARY_DIR}/Docs)
ADD_CUSTOM_TARGET(INFO_SRC ALL
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/info_src.cmake
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
# Build flags: This must be postponed to the make phase.
ADD_CUSTOM_TARGET(INFO_BIN ALL
  COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/info_bin.cmake
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

INSTALL_DOCUMENTATION(README.md CREDITS COPYING THIRDPARTY COMPONENT Readme)

# MDEV-6526 these files are not installed anymore
#INSTALL_DOCUMENTATION(${CMAKE_BINARY_DIR}/Docs/INFO_SRC
#                      ${CMAKE_BINARY_DIR}/Docs/INFO_BIN)

IF(UNIX)
  INSTALL_DOCUMENTATION(Docs/INSTALL-BINARY Docs/README-wsrep COMPONENT Readme)
ENDIF()

INCLUDE(build_depends)

INCLUDE(CPack)

IF(WIN32 AND SIGNCODE)
  # Configure post-install script for authenticode signing
  CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/cmake/sign.cmake.in sign.cmake @ONLY)
  INSTALL(SCRIPT ${PROJECT_BINARY_DIR}/sign.cmake)
ENDIF()

FEATURE_SUMMARY(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES VAR MARIADB_FEATURE_SUMMARY)

OPTION(FEATURE_SUMMARY "Print feature summary at the end of configure step" ON)
IF (FEATURE_SUMMARY)
  MESSAGE("${MARIADB_FEATURE_SUMMARY}")
  SET(FEATURE_SUMMARY OFF CACHE BOOL "" FORCE)
ENDIF()

IF(NON_DISTRIBUTABLE_WARNING)
  MESSAGE(WARNING "
You have linked MariaDB with ${NON_DISTRIBUTABLE_WARNING} libraries!  You may not distribute the resulting binary. If you do, you will put yourself into a legal problem with the Free Software Foundation.")
ENDIF()

IF(NOT WITHOUT_SERVER)
  # Define target for minimal mtr-testable build
  ADD_CUSTOM_TARGET(minbuild)
  ADD_DEPENDENCIES(minbuild
   aria_chk
   aria_pack
   mysql
   mysqladmin
   mysqlbinlog
   mysqlcheck
   mysql_client_test
   mysqldump
   mysqlimport
   mysql_plugin
   mysqlshow
   mysqlslap
   mysqltest
   mysql_tzinfo_to_sql
   mysql_upgrade
   mysqld
   my_print_defaults
   my_safe_process
   myisam_ftdump
   myisamchk
   myisamlog
   myisampack
   perror
   replace)
  IF(WIN32)
    ADD_DEPENDENCIES(minbuild echo my_safe_kill)
  ENDIF()
  ADD_CUSTOM_TARGET(smoketest
    COMMAND perl ./mysql-test-run.pl main.1st
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/mysql-test)
  ADD_DEPENDENCIES(smoketest minbuild)
ENDIF()
