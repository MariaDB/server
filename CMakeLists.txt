# Copyright (c) 2006, 2014, Oracle and/or its affiliates.
# Copyright (c) 2008, 2014, Monty Program Ab
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
# Avoid warnings in higher versions
if("${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}" GREATER 2.6)
 CMAKE_POLICY(VERSION 2.8)
endif()

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

MESSAGE(STATUS "Running cmake version ${CMAKE_VERSION}")

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

# Distinguish between community and non-community builds, with the
# default being a community build. This does not impact the feature
# set that will be compiled in; it's merely provided as a hint to
# custom packaging steps.
OPTION(COMMUNITY_BUILD "Set to true if this is a community build" ON) 

# Use a default manufacturer if no manufacturer was identified.
IF(NOT DEFINED MANUFACTURER) 
  SET(MANUFACTURER "Built from Source" CACHE  STRING
     "Set the entity that appears as the manufacturer of packages that support a manufacturer field.")
  MARK_AS_ADVANCED(MANUFACTURER)
ENDIF()

SET(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
   "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel")

# MAX_INDEXES - Set the maximum number of indexes per table, default 64U
IF (NOT MAX_INDEXES)
  SET(MAX_INDEXES 64U)
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

# Optionally set project name, e.g.
# foo.xcodeproj (mac) or foo.sln (windows)
SET(MYSQL_PROJECT_NAME_DOCSTRING "MySQL project name")
IF(DEFINED MYSQL_PROJECT_NAME)
  SET(MYSQL_PROJECT_NAME ${MYSQL_PROJECT_NAME} CACHE STRING
      ${MYSQL_PROJECT_NAME_DOCSTRING} FORCE)
ELSE()
  SET(MYSQL_PROJECT_NAME "MySQL" CACHE STRING
      ${MYSQL_PROJECT_NAME_DOCSTRING} FORCE)
  MARK_AS_ADVANCED(MYSQL_PROJECT_NAME)
ENDIF()
PROJECT(${MYSQL_PROJECT_NAME})

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


# Following autotools tradition, add preprocessor definitions
# specified in environment variable CPPFLAGS
IF(DEFINED ENV{CPPFLAGS})
  ADD_DEFINITIONS($ENV{CPPFLAGS})
ENDIF()

SET(MYSQL_MAINTAINER_MODE "AUTO" CACHE STRING "MySQL maintainer-specific development environment. Options are: ON OFF AUTO.")

# Packaging
IF (NOT CPACK_GENERATOR)
  IF(WIN32)
    SET(CPACK_GENERATOR "ZIP")
  ELSE()
    SET(CPACK_GENERATOR "TGZ")
  ENDIF(WIN32) 
ENDIF(NOT CPACK_GENERATOR)

INCLUDE(mysql_version)
INCLUDE(cpack_source_ignore_files)
INCLUDE(install_layout)
INCLUDE(wsrep)
INCLUDE(cpack_rpm)
INCLUDE(cpack_deb)

# Add macros
INCLUDE(character_sets)
INCLUDE(cpu_info)
INCLUDE(zlib)
INCLUDE(ssl)
INCLUDE(readline)
INCLUDE(libutils)
INCLUDE(dtrace)
INCLUDE(jemalloc)
INCLUDE(pcre)
INCLUDE(ctest)
INCLUDE(plugin)
INCLUDE(install_macros)
INCLUDE(systemd)
INCLUDE(mysql_add_executable)

# Handle options
OPTION(DISABLE_SHARED 
 "Don't build shared libraries, compile code as position-dependent" OFF)
IF(DISABLE_SHARED)
  SET(WITHOUT_DYNAMIC_PLUGINS 1)
ENDIF()
OPTION(ENABLED_PROFILING "Enable profiling" ON)
OPTION(CYBOZU "" OFF)
OPTION(WITHOUT_SERVER "Build only the client library and clients" OFF)
IF(UNIX)
  OPTION(WITH_VALGRIND "Valgrind instrumentation" OFF)
ENDIF()
OPTION (WITH_UNIT_TESTS "Compile MySQL with unit tests" ON)
MARK_AS_ADVANCED(CYBOZU)

OPTION(NOT_FOR_DISTRIBUTION "Allow linking with GPLv2-incompatible system libraries. Only set it you never plan to distribute the resulting binaries" OFF)

INCLUDE(check_compiler_flag)

OPTION(WITH_ASAN "Enable address sanitizer" OFF)
IF (WITH_ASAN)
  # gcc 4.8.1 and new versions of clang
  MY_CHECK_AND_SET_COMPILER_FLAG("-fsanitize=address -O1 -Wno-error -fPIC"
    DEBUG RELWITHDEBINFO)
  IF(HAVE_C_FSANITIZE AND HAVE_CXX_FSANITIZE)
    SET(WITH_ASAN_OK 1)
  ELSE()
    # older versions of clang
    MY_CHECK_AND_SET_COMPILER_FLAG("-faddress-sanitizer -O1 -fPIC"
      DEBUG RELWITHDEBINFO)
    IF(HAVE_C_FADDRESS AND HAVE_CXX_FADDRESS)
      SET(WITH_ASAN_OK 1)
    ENDIF()
  ENDIF()

  IF(NOT WITH_ASAN_OK)
    MESSAGE(FATAL_ERROR "Do not know how to enable address sanitizer")
  ENDIF()
ENDIF()

# enable security hardening features, like most distributions do
# in our benchmarks that costs about ~1% of performance, depending on the load
IF(CMAKE_C_COMPILER_VERSION VERSION_LESS "4.6")
  SET(security_default OFF)
ELSE()
  SET(security_default ON)
ENDIF()
OPTION(SECURITY_HARDENED "Use security-enhancing compiler features (stack protector, relro, etc)" ${security_default})
IF(SECURITY_HARDENED)
  # security-enhancing flags
  MY_CHECK_AND_SET_COMPILER_FLAG("-pie -fPIC")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wl,-z,relro,-z,now")
  MY_CHECK_AND_SET_COMPILER_FLAG("-fstack-protector --param=ssp-buffer-size=4")
  MY_CHECK_AND_SET_COMPILER_FLAG("-D_FORTIFY_SOURCE=2" RELEASE RELWITHDEBINFO)
ENDIF()

OPTION(ENABLE_DEBUG_SYNC "Enable debug sync (debug builds only)" ON) 
IF(ENABLE_DEBUG_SYNC) 
  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DENABLED_DEBUG_SYNC") 
  SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DENABLED_DEBUG_SYNC") 
ENDIF() 
 
OPTION(ENABLE_GCOV "Enable gcov (debug, Linux builds only)" OFF)
IF (ENABLE_GCOV)
  MY_CHECK_AND_SET_COMPILER_FLAG("-fprofile-arcs -ftest-coverage -lgcov" DEBUG)
ENDIF()

MY_CHECK_AND_SET_COMPILER_FLAG(-ggdb3 DEBUG)

OPTION(ENABLED_LOCAL_INFILE
 "If we should should enable LOAD DATA LOCAL by default" ${IF_WIN})

OPTION(WITH_FAST_MUTEXES "Compile with fast mutexes" OFF)
MARK_AS_ADVANCED(WITH_FAST_MUTEXES)

OPTION(WITH_INNODB_DISALLOW_WRITES "InnoDB freeze writes patch from Google" ${WITH_WSREP})
IF (WITH_INNODB_DISALLOW_WRITES)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DWITH_INNODB_DISALLOW_WRITES")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWITH_INNODB_DISALLOW_WRITES")
ENDIF()

# Set DBUG_OFF and other optional release-only flags for non-debug project types
FOREACH(BUILD_TYPE RELEASE RELWITHDEBINFO MINSIZEREL)
  FOREACH(LANG C CXX)
    IF (NOT CMAKE_${LANG}_FLAGS_${BUILD_TYPE} MATCHES "DDBUG_" AND
        NOT CMAKE_${LANG}_FLAGS MATCHES "DDBUG_")
      SET(CMAKE_${LANG}_FLAGS_${BUILD_TYPE} 
       "${CMAKE_${LANG}_FLAGS_${BUILD_TYPE}} -DDBUG_OFF")
    ENDIF()
    IF(WITH_FAST_MUTEXES)
      SET(CMAKE_${LANG}_FLAGS_${BUILD_TYPE} 
        "${CMAKE_${LANG}_FLAGS_${BUILD_TYPE}} -DMY_PTHREAD_FASTMUTEX=1")
    ENDIF()
  ENDFOREACH()
ENDFOREACH()

# Add safemutex for debug configurations, except on Windows
# (safemutex has never worked on Windows)
IF(NOT WIN32 AND NOT WITH_INNODB_MEMCACHED)
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
  SET(DEFAULT_MYSQL_HOME "C:/MariaDB${MYSQL_BASE_VERSION}")
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
IF(INSTALL_SYSCONFDIR AND NOT DEFAULT_SYSCONFDIR)
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
# (jemalloc, yassl, readline, pcre, etc)
# before the ones installed in the system
SET(CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE ON)

# Common defines and includes
ADD_DEFINITIONS(-DHAVE_CONFIG_H)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/include)

# Add bundled or system zlib.
MYSQL_CHECK_ZLIB_WITH_COMPRESS()
# Add bundled yassl/taocrypt or system openssl.
MYSQL_CHECK_SSL()
# Add readline or libedit.
MYSQL_CHECK_READLINE()

SET(MALLOC_LIBRARY "system")
CHECK_JEMALLOC()

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
 IF(NOT WITHOUT_SERVER)
   ADD_SUBDIRECTORY(unittest/sql)
 ENDIF()
ENDIF()

IF(NOT WITHOUT_SERVER)
 SET (MYSQLD_STATIC_PLUGIN_LIBS "" CACHE INTERNAL "")
 # Add storage engines and plugins.
 CONFIGURE_PLUGINS()
ENDIF()

ADD_SUBDIRECTORY(include)
ADD_SUBDIRECTORY(dbug)
ADD_SUBDIRECTORY(strings)
ADD_SUBDIRECTORY(vio)
ADD_SUBDIRECTORY(mysys)
ADD_SUBDIRECTORY(mysys_ssl)
ADD_SUBDIRECTORY(libmysql)
ADD_SUBDIRECTORY(client)
ADD_SUBDIRECTORY(extra)
ADD_SUBDIRECTORY(libservices)
ADD_SUBDIRECTORY(sql/share)

IF(NOT WITHOUT_SERVER)
  ADD_SUBDIRECTORY(tests)
  ADD_SUBDIRECTORY(sql)
  OPTION (WITH_EMBEDDED_SERVER "Compile MySQL with embedded server" OFF)
  IF(WITH_EMBEDDED_SERVER) 
   ADD_SUBDIRECTORY(libmysqld)
   ADD_SUBDIRECTORY(libmysqld/examples)
  ENDIF(WITH_EMBEDDED_SERVER)

  IF(WITH_WSREP)
    ADD_SUBDIRECTORY(wsrep)
  ENDIF()

  ADD_SUBDIRECTORY(mysql-test)
  ADD_SUBDIRECTORY(mysql-test/lib/My/SafeProcess)
  ADD_SUBDIRECTORY(sql-bench)
ENDIF()

IF(UNIX)
  ADD_SUBDIRECTORY(man)
ENDIF()

INCLUDE(cmake/abi_check.cmake)
INCLUDE(cmake/tags.cmake)

IF(WIN32)
  ADD_SUBDIRECTORY(win/upgrade_wizard)
  ADD_SUBDIRECTORY(win/packaging)
ENDIF()


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

IF(DEB)
  CONFIGURE_FILE(
    ${CMAKE_SOURCE_DIR}/debian/mariadb-server-10.1.files.in
    ${CMAKE_SOURCE_DIR}/debian/mariadb-server-10.1.files)
ENDIF(DEB)

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

INSTALL_DOCUMENTATION(README CREDITS COPYING COPYING.LESSER COPYING.thirdparty
                      EXCEPTIONS-CLIENT COMPONENT Readme)
# MDEV-6526 these files are not installed anymore
#INSTALL_DOCUMENTATION(${CMAKE_BINARY_DIR}/Docs/INFO_SRC
#                      ${CMAKE_BINARY_DIR}/Docs/INFO_BIN)

IF(UNIX)
  INSTALL_DOCUMENTATION(Docs/INSTALL-BINARY COMPONENT Readme)
  INSTALL_DOCUMENTATION(Docs/INSTALL-BINARY Docs/README-wsrep COMPONENT Readme)
ENDIF()

INCLUDE(CPack)

IF(NON_DISTRIBUTABLE_WARNING)
  MESSAGE(WARNING "
You have linked MariaDB with GPLv3 libraries!  You may not distribute the resulting binary. If you do, you will put yourself into a legal problem with Free Software Foundation.")
ENDIF()
