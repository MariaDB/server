# Copyright (c) 2009, 2012, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 


INCLUDE(CMakeParseArguments)

# MYSQL_ADD_PLUGIN(plugin_name source1...sourceN
# [STORAGE_ENGINE]
# [CLIENT]
# [MANDATORY|DEFAULT]
# [STATIC_ONLY|DYNAMIC_ONLY]
# [MODULE_OUTPUT_NAME module_name]
# [STATIC_OUTPUT_NAME static_name]
# [RECOMPILE_FOR_EMBEDDED]
# [LINK_LIBRARIES lib1...libN]
# [DEPENDENCIES target1...targetN]

MACRO(MYSQL_ADD_PLUGIN)
  CMAKE_PARSE_ARGUMENTS(ARG
    "STORAGE_ENGINE;STATIC_ONLY;MODULE_ONLY;MANDATORY;DEFAULT;DISABLED;RECOMPILE_FOR_EMBEDDED;CLIENT"
    "MODULE_OUTPUT_NAME;STATIC_OUTPUT_NAME;COMPONENT;CONFIG"
    "LINK_LIBRARIES;DEPENDENCIES"
    ${ARGN}
  )
  IF(NOT WITHOUT_SERVER OR ARG_CLIENT)

  # Add common include directories
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include 
                    ${CMAKE_SOURCE_DIR}/sql
                    ${PCRE_INCLUDES}
                    ${SSL_INCLUDE_DIRS}
                    ${ZLIB_INCLUDE_DIR})

  LIST(GET ARG_UNPARSED_ARGUMENTS 0 plugin)
  SET(SOURCES ${ARG_UNPARSED_ARGUMENTS})
  LIST(REMOVE_AT SOURCES 0)
  STRING(TOUPPER ${plugin} plugin)
  STRING(TOLOWER ${plugin} target)
  
  IF (ARG_MANDATORY)
    UNSET(PLUGIN_${plugin} CACHE)
    SET(PLUGIN_${plugin} "YES")
  ELSE()
    SET (compat ".")
    # Figure out whether to build plugin.
    # recognize and support the maze of old WITH/WITHOUT combinations
    IF(WITHOUT_${plugin}_STORAGE_ENGINE
       OR WITHOUT_${plugin}
       OR WITHOUT_PLUGIN_${plugin}
       OR WITH_NONE)

      SET(compat "${compat}without")
    ENDIF()
    IF(WITH_${plugin}_STORAGE_ENGINE
       OR WITH_${plugin}
       OR WITH_PLUGIN_${plugin}
       OR WITH_ALL
       OR WITH_MAX
       OR WITH_MAX_NO_NDB
       OR ARG_DEFAULT)

      SET(compat "with${compat}")
    ENDIF()

    IF (ARG_DISABLED)
      SET(howtobuild NO)
    ELSEIF (compat STREQUAL ".")
      SET(howtobuild DYNAMIC)
    ELSEIF (compat STREQUAL "with.")
      IF (NOT ARG_MODULE_ONLY)
        SET(howtobuild STATIC)
      ELSE()
        SET(howtobuild DYNAMIC)
      ENDIF()
    ELSEIF (compat STREQUAL ".without")
      SET(howtobuild NO)
    ELSEIF (compat STREQUAL "with.without")
      SET(howtobuild STATIC)
    ENDIF()

    # NO - not at all
    # YES - static if possible, otherwise dynamic if possible, otherwise abort
    # AUTO - static if possible, otherwise dynamic, if possible
    # STATIC - static if possible, otherwise not at all
    # DYNAMIC - dynamic if possible, otherwise not at all
    SET(PLUGIN_${plugin} ${howtobuild}
      CACHE STRING "How to build plugin ${plugin}. Options are: NO STATIC DYNAMIC YES AUTO.")
  ENDIF()

  IF (NOT PLUGIN_${plugin} MATCHES "^(NO|YES|AUTO|STATIC|DYNAMIC)$")
    MESSAGE(FATAL_ERROR "Invalid value for PLUGIN_${plugin}")
  ENDIF()

  IF(ARG_STORAGE_ENGINE)
    SET(with_var "WITH_${plugin}_STORAGE_ENGINE" )
  ELSE()
    SET(with_var "WITH_${plugin}")
  ENDIF()
  UNSET(${with_var} CACHE)
  
  IF(NOT ARG_DEPENDENCIES)
    SET(ARG_DEPENDENCIES)
  ENDIF()
  
  IF(NOT ARG_MODULE_OUTPUT_NAME)
    IF(ARG_STORAGE_ENGINE)
      SET(ARG_MODULE_OUTPUT_NAME "ha_${target}")
    ELSE()
      SET(ARG_MODULE_OUTPUT_NAME "${target}")
    ENDIF()
  ENDIF()

  # Build either static library or module
  IF (PLUGIN_${plugin} MATCHES "(STATIC|AUTO|YES)" AND NOT ARG_MODULE_ONLY
      AND NOT ARG_CLIENT)

    IF(CMAKE_GENERATOR MATCHES "Makefiles|Ninja")
      # If there is a shared library from previous shared build,
      # remove it. This is done just for mysql-test-run.pl 
      # so it does not try to use stale shared lib as plugin 
      # in test.
      FILE(REMOVE 
        ${CMAKE_CURRENT_BINARY_DIR}/${ARG_MODULE_OUTPUT_NAME}${CMAKE_SHARED_MODULE_SUFFIX})
    ENDIF()

    ADD_LIBRARY(${target} STATIC ${SOURCES})
    DTRACE_INSTRUMENT(${target})
    ADD_DEPENDENCIES(${target} GenError ${ARG_DEPENDENCIES})
    RESTRICT_SYMBOL_EXPORTS(${target})
    IF(WITH_EMBEDDED_SERVER)
      # Embedded library should contain PIC code and be linkable
      # to shared libraries (on systems that need PIC)
      IF(ARG_RECOMPILE_FOR_EMBEDDED OR NOT _SKIP_PIC)
        # Recompile some plugins for embedded
        ADD_CONVENIENCE_LIBRARY(${target}_embedded ${SOURCES})
        RESTRICT_SYMBOL_EXPORTS(${target}_embedded)
        DTRACE_INSTRUMENT(${target}_embedded)   
        IF(ARG_RECOMPILE_FOR_EMBEDDED)
          SET_TARGET_PROPERTIES(${target}_embedded 
            PROPERTIES COMPILE_DEFINITIONS "EMBEDDED_LIBRARY")
        ENDIF()
        ADD_DEPENDENCIES(${target}_embedded GenError)
      ENDIF()
    ENDIF()

    IF(ARG_STATIC_OUTPUT_NAME)
      SET_TARGET_PROPERTIES(${target} PROPERTIES 
      OUTPUT_NAME ${ARG_STATIC_OUTPUT_NAME})
    ENDIF()

    IF(ARG_LINK_LIBRARIES)
      TARGET_LINK_LIBRARIES (${target} ${ARG_LINK_LIBRARIES})
    ENDIF()

    # Update mysqld dependencies
    SET (MYSQLD_STATIC_PLUGIN_LIBS ${MYSQLD_STATIC_PLUGIN_LIBS} 
      ${target} ${ARG_LINK_LIBRARIES} CACHE INTERNAL "" FORCE)

    SET(${with_var} ON CACHE INTERNAL "Link ${plugin} statically to the server" FORCE)

    IF(ARG_MANDATORY)
      SET (mysql_mandatory_plugins  
        "${mysql_mandatory_plugins} builtin_maria_${target}_plugin,")
      SET (mysql_mandatory_plugins ${mysql_mandatory_plugins} PARENT_SCOPE)
    ELSE()
      SET (mysql_optional_plugins  
        "${mysql_optional_plugins} builtin_maria_${target}_plugin,")
      SET (mysql_optional_plugins ${mysql_optional_plugins} PARENT_SCOPE)
    ENDIF()
  ELSEIF(PLUGIN_${plugin} MATCHES "(DYNAMIC|AUTO|YES)"
         AND NOT ARG_STATIC_ONLY AND NOT WITHOUT_DYNAMIC_PLUGINS)

    ADD_VERSION_INFO(${target} MODULE SOURCES)
    ADD_LIBRARY(${target} MODULE ${SOURCES}) 
    DTRACE_INSTRUMENT(${target})

    SET_TARGET_PROPERTIES (${target} PROPERTIES PREFIX "")
    IF (NOT ARG_CLIENT)
      SET_TARGET_PROPERTIES (${target} PROPERTIES
        COMPILE_DEFINITIONS "MYSQL_DYNAMIC_PLUGIN")
    ENDIF()

    TARGET_LINK_LIBRARIES (${target} mysqlservices ${ARG_LINK_LIBRARIES})

    # Server plugins use symbols defined in mysqld executable.
    # Some operating systems like Windows and OSX and are pretty strict about 
    # unresolved symbols. Others are less strict and allow unresolved symbols
    # in shared libraries. On Linux for example, CMake does not even add 
    # executable to the linker command line (it would result into link error). 
    # Thus we skip TARGET_LINK_LIBRARIES on Linux, as it would only generate
    # an additional dependency.
    IF(ARG_RECOMPILE_FOR_EMBEDDED OR ARG_STORAGE_ENGINE)
      IF(MSVC)
        ADD_DEPENDENCIES(${target} gen_mysqld_lib)
        TARGET_LINK_LIBRARIES(${target} mysqld_import_lib)
      ELSEIF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        TARGET_LINK_LIBRARIES (${target} mysqld)
      ENDIF()
    ELSEIF(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WITH_ASAN AND NOT WITH_TSAN)
      TARGET_LINK_LIBRARIES (${target} "-Wl,--no-undefined")
    ENDIF()

    ADD_DEPENDENCIES(${target} GenError ${ARG_DEPENDENCIES})

    SET_TARGET_PROPERTIES(${target} PROPERTIES 
      OUTPUT_NAME "${ARG_MODULE_OUTPUT_NAME}")  
    # Install dynamic library
    IF(ARG_COMPONENT)
      IF(CPACK_COMPONENTS_ALL AND
         NOT CPACK_COMPONENTS_ALL MATCHES ${ARG_COMPONENT}
         AND INSTALL_SYSCONF2DIR)
        IF (ARG_STORAGE_ENGINE)
          SET(ver " = %{version}-%{release}")
        ELSE()
          SET(ver "")
        ENDIF()
        SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} ${ARG_COMPONENT})
        SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} PARENT_SCOPE)

        IF (NOT ARG_CLIENT)
          SET(CPACK_RPM_${ARG_COMPONENT}_PACKAGE_REQUIRES "MariaDB${ver}" PARENT_SCOPE)
        ENDIF()
        # workarounds for cmake issues #13248 and #12864:
        SET(CPACK_RPM_${ARG_COMPONENT}_PACKAGE_PROVIDES "cmake_bug_13248" PARENT_SCOPE)
        SET(CPACK_RPM_${ARG_COMPONENT}_PACKAGE_OBSOLETES "cmake_bug_13248" PARENT_SCOPE)
        SET(CPACK_RPM_${ARG_COMPONENT}_USER_FILELIST ${ignored} PARENT_SCOPE)
        IF(NOT ARG_CLIENT AND UNIX)
          IF (NOT ARG_CONFIG)
            SET(ARG_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/${target}.cnf")
            FILE(WRITE ${ARG_CONFIG} "[mariadb]\nplugin-load-add=${ARG_MODULE_OUTPUT_NAME}.so\n")
          ENDIF()
          INSTALL(FILES ${ARG_CONFIG} COMPONENT ${ARG_COMPONENT} DESTINATION ${INSTALL_SYSCONF2DIR})
          SET(CPACK_RPM_${ARG_COMPONENT}_USER_FILELIST ${ignored} "%config(noreplace) ${INSTALL_SYSCONF2DIR}/*" PARENT_SCOPE)
        ENDIF()
      ENDIF()
    ELSE()
      SET(ARG_COMPONENT Server)
    ENDIF()
    MYSQL_INSTALL_TARGETS(${target} DESTINATION ${INSTALL_PLUGINDIR} COMPONENT ${ARG_COMPONENT})
    #INSTALL_DEBUG_TARGET(${target} DESTINATION ${INSTALL_PLUGINDIR}/debug COMPONENT ${ARG_COMPONENT})
  ENDIF()

  GET_FILENAME_COMPONENT(subpath ${CMAKE_CURRENT_SOURCE_DIR} NAME)
  IF(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mysql-test")
    INSTALL_MYSQL_TEST("${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/" "plugin/${subpath}")
  ENDIF()

  ENDIF(NOT WITHOUT_SERVER OR ARG_CLIENT)
ENDMACRO()


# Add all CMake projects under storage  and plugin 
# subdirectories, configure sql_builtins.cc
MACRO(CONFIGURE_PLUGINS)
  IF(NOT WITHOUT_SERVER)
    FILE(GLOB dirs_storage ${CMAKE_SOURCE_DIR}/storage/*)
  ENDIF()

  FILE(GLOB dirs_plugin ${CMAKE_SOURCE_DIR}/plugin/*)
  FOREACH(dir ${dirs_storage} ${dirs_plugin})
    IF (EXISTS ${dir}/CMakeLists.txt)
      ADD_SUBDIRECTORY(${dir})
    ENDIF()
  ENDFOREACH()

  GET_CMAKE_PROPERTY(ALL_VARS VARIABLES)
  FOREACH (V ${ALL_VARS})
    IF (V MATCHES "^PLUGIN_" AND ${V} MATCHES "YES")
      STRING(SUBSTRING ${V} 7 -1 plugin)
      STRING(TOLOWER ${plugin} target)
      IF (NOT TARGET ${target})
        MESSAGE(FATAL_ERROR "Plugin ${plugin} cannot be built")
      ENDIF()
    ENDIF()
  ENDFOREACH()
ENDMACRO()
