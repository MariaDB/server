# Copyright (c) 2009, 2011, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

INCLUDE(CMakeParseArguments)

FUNCTION (INSTALL_DEBUG_SYMBOLS)
 IF(MSVC)
   CMAKE_PARSE_ARGUMENTS(ARG
  ""
  "COMPONENT;INSTALL_LOCATION"
  ""
  ${ARGN}
  )

  IF(NOT ARG_COMPONENT)
    SET(ARG_COMPONENT DebugBinaries)
  ENDIF()
  IF(NOT ARG_INSTALL_LOCATION)
    SET(ARG_INSTALL_LOCATION lib)
  ENDIF()
  SET(targets ${ARG_UNPARSED_ARGUMENTS})
  FOREACH(target ${targets})
    GET_TARGET_PROPERTY(target_type ${target} TYPE)
    IF(target_type MATCHES "STATIC")
      RETURN()
    ENDIF()
    set(comp "")

    IF(target STREQUAL "server"
       OR target STREQUAL "mariadbd")
      SET(comp Server)
    ENDIF()

    INSTALL(FILES $<TARGET_PDB_FILE:${target}> DESTINATION symbols COMPONENT Debuginfo)
    IF(comp)
      INSTALL(FILES $<TARGET_PDB_FILE:${target}> DESTINATION ${ARG_INSTALL_LOCATION} COMPONENT ${comp})
    ENDIF()
  ENDFOREACH()
  ENDIF()
ENDFUNCTION()

# Installs manpage for given file (either script or executable)
#
FUNCTION(INSTALL_MANPAGE file)
  IF(NOT UNIX)
    RETURN()
  ENDIF()
  GET_FILENAME_COMPONENT(file_name "${file}" NAME)
  SET(GLOB_EXPR 
    ${CMAKE_SOURCE_DIR}/man/*${file}man.1*
    ${CMAKE_SOURCE_DIR}/man/*${file}man.8*
    ${CMAKE_BINARY_DIR}/man/*${file}man.1*
    ${CMAKE_BINARY_DIR}/man/*${file}man.8*
   )
  IF(MYSQL_DOC_DIR)
    SET(GLOB_EXPR
      ${MYSQL_DOC_DIR}/man/*${file}man.1*
      ${MYSQL_DOC_DIR}/man/*${file}man.8*
      ${MYSQL_DOC_DIR}/man/*${file}.1*
      ${MYSQL_DOC_DIR}/man/*${file}.8*
      ${GLOB_EXPR}
      )
  ENDIF()

  FILE(GLOB_RECURSE MANPAGES ${GLOB_EXPR})

  IF(MANPAGES)
    LIST(GET MANPAGES 0 MANPAGE)
    STRING(REPLACE "${file}man.1" "${file}.1" MANPAGE "${MANPAGE}")
    STRING(REPLACE "${file}man.8" "${file}.8" MANPAGE "${MANPAGE}")
    IF(MANPAGE MATCHES "${file}.1")
      SET(SECTION man1)
    ELSE()
      SET(SECTION man8)
    ENDIF()
    INSTALL(FILES "${MANPAGE}" DESTINATION "${INSTALL_MANDIR}/${SECTION}"
      COMPONENT ManPages)
  ENDIF()
ENDFUNCTION()

FUNCTION(INSTALL_SCRIPT)
  CMAKE_PARSE_ARGUMENTS(ARG
  ""
  "DESTINATION;COMPONENT"
  ""
  ${ARGN}
  )

  SET(script ${ARG_UNPARSED_ARGUMENTS})
  IF(NOT ARG_DESTINATION)
    SET(ARG_DESTINATION ${INSTALL_BINDIR})
  ENDIF()
  SET(COMP ${ARG_COMPONENT})

  IF (COMP MATCHES ${SKIP_COMPONENTS})
    RETURN()
  ENDIF()

  INSTALL(PROGRAMS ${script} DESTINATION ${ARG_DESTINATION} COMPONENT ${COMP})
  get_filename_component(dest "${script}" NAME)
  INSTALL_MANPAGE(${dest})
ENDFUNCTION()


FUNCTION(INSTALL_DOCUMENTATION)
  CMAKE_PARSE_ARGUMENTS(ARG "" "COMPONENT" "" ${ARGN})
  SET(files ${ARG_UNPARSED_ARGUMENTS})
  IF(NOT ARG_COMPONENT)
    SET(ARG_COMPONENT Server)
  ENDIF()
  IF (ARG_COMPONENT MATCHES "Readme")
    SET(destination ${INSTALL_DOCREADMEDIR})
  ELSE()
    SET(destination ${INSTALL_DOCDIR})
  ENDIF()

  IF (ARG_COMPONENT MATCHES ${SKIP_COMPONENTS})
    RETURN()
  ENDIF()

  STRING(TOUPPER ${ARG_COMPONENT} COMPUP)
  IF(CPACK_COMPONENT_${COMPUP}_GROUP)
    SET(group ${CPACK_COMPONENT_${COMPUP}_GROUP})
  ELSE()
    SET(group ${ARG_COMPONENT})
  ENDIF()

  IF(RPM)
    SET(destination "${destination}/MariaDB-${group}-${VERSION}")
  ELSEIF(DEB)
    SET(destination "${destination}/mariadb-${group}")
  ENDIF()

  INSTALL(FILES ${files} DESTINATION ${destination} COMPONENT ${ARG_COMPONENT})
ENDFUNCTION()


# Install symbolic link to CMake target.
# the link is created in the current build directory
# and extension will be the same as for target file.
MACRO(INSTALL_SYMLINK linkname target destination component)
IF(UNIX)
  SET(output ${CMAKE_CURRENT_BINARY_DIR}/${linkname})
  ADD_CUSTOM_COMMAND(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} ARGS -E remove -f ${linkname}
    COMMAND ${CMAKE_COMMAND} ARGS -E create_symlink
      $<TARGET_FILE_NAME:${target}>
      ${linkname}
    DEPENDS ${target}
    )

  ADD_CUSTOM_TARGET(symlink_${linkname}
    ALL
    DEPENDS ${output})
  SET_TARGET_PROPERTIES(symlink_${linkname} PROPERTIES CLEAN_DIRECT_OUTPUT 1)
  IF(CMAKE_GENERATOR MATCHES "Xcode")
    # For Xcode, replace project config with install config
    STRING(REPLACE "${CMAKE_CFG_INTDIR}"
      "\${CMAKE_INSTALL_CONFIG_NAME}" output ${output})
  ENDIF()
  INSTALL(FILES ${output} DESTINATION ${destination} COMPONENT ${component})
ENDIF()
ENDMACRO()

IF(WIN32)
  OPTION(SIGNCODE "Sign executables and dlls with digital certificate" OFF)
  MARK_AS_ADVANCED(SIGNCODE)
  IF(SIGNCODE)
   SET(SIGNTOOL_PARAMETERS
     /a /t http://timestamp.globalsign.com/?signature=sha2
     CACHE STRING "parameters for signtool (list)")
    IF(NOT SIGNTOOL_EXECUTABLE)
      FILE(GLOB path_list
        "$ENV{ProgramFiles} (x86)/Windows Kits/*/bin/*/x64"
        "$ENV{ProgramFiles} (x86)/Windows Kits/*/App Certification Kit"
      )
      FIND_PROGRAM(SIGNTOOL_EXECUTABLE signtool
        PATHS ${path_list}
      )
      IF(NOT SIGNTOOL_EXECUTABLE)
        MESSAGE(FATAL_ERROR
        "signtool is not found. Signing executables not possible")
      ENDIF()
      MARK_AS_ADVANCED(SIGNTOOL_EXECUTABLE  SIGNTOOL_PARAMETERS)
    ENDIF()
  ENDIF()
ENDIF()


FUNCTION(SIGN_TARGET target)
   IF(NOT SIGNCODE)
     RETURN()
   ENDIF()
   GET_TARGET_PROPERTY(target_type ${target} TYPE)
   IF((NOT target_type) OR (target_type MATCHES "STATIC"))
     RETURN()
   ENDIF()
    # Mark executable for signing by creating empty *.signme file
    # The actual signing happens in preinstall step
    # (which traverses
    ADD_CUSTOM_COMMAND(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E touch "$<TARGET_FILE:${target}>.signme"
   )
ENDFUNCTION()

# Installs targets, also installs pdbs on Windows.
#
#

FUNCTION(MYSQL_INSTALL_TARGETS)
  CMAKE_PARSE_ARGUMENTS(ARG
  ""
  "DESTINATION;COMPONENT"
  ""
  ${ARGN}
  )
  IF(ARG_COMPONENT)
    SET(COMP COMPONENT ${ARG_COMPONENT})
  ELSE()
    MESSAGE(FATAL_ERROR "COMPONENT argument required")
  ENDIF()

  SET(TARGETS ${ARG_UNPARSED_ARGUMENTS})
  IF(NOT TARGETS)
    MESSAGE(FATAL_ERROR "Need target list for MYSQL_INSTALL_TARGETS")
  ENDIF()
  IF(NOT ARG_DESTINATION)
     MESSAGE(FATAL_ERROR "Need DESTINATION parameter for MYSQL_INSTALL_TARGETS")
  ENDIF()

  FOREACH(target ${TARGETS})
    # If signing is required, sign executables before installing
    IF(SIGNCODE)
      SIGN_TARGET(${target} ${COMP})
    ENDIF()
    # Install man pages on Unix
    IF(UNIX)
      INSTALL_MANPAGE($<TARGET_FILE:${target}>)
    ENDIF()
  ENDFOREACH()

  INSTALL(TARGETS ${TARGETS} DESTINATION ${ARG_DESTINATION} ${COMP})
  INSTALL_DEBUG_SYMBOLS(${TARGETS} ${COMP} INSTALL_LOCATION ${ARG_DESTINATION})

ENDFUNCTION()

# Optionally install mysqld/client/embedded from debug build run. outside of the current build dir
# (unless multi-config generator is used like Visual Studio or Xcode).
# For Makefile generators we default Debug build directory to ${buildroot}/../debug.
GET_FILENAME_COMPONENT(BINARY_PARENTDIR ${CMAKE_BINARY_DIR} PATH)
SET(DEBUGBUILDDIR "${BINARY_PARENTDIR}/debug" CACHE INTERNAL "Directory of debug build")

FUNCTION(INSTALL_MYSQL_TEST from to)
  IF(INSTALL_MYSQLTESTDIR)
    INSTALL(
      DIRECTORY ${from}
      DESTINATION "${INSTALL_MYSQLTESTDIR}/${to}"
      USE_SOURCE_PERMISSIONS
      COMPONENT Test
      PATTERN "var" EXCLUDE
      PATTERN "lib/My/SafeProcess" EXCLUDE
      PATTERN "lib/t*" EXCLUDE
      PATTERN "CPack" EXCLUDE
      PATTERN "CMake*" EXCLUDE
      PATTERN "cmake_install.cmake" EXCLUDE
      PATTERN "mtr.out*" EXCLUDE
      PATTERN ".cvsignore" EXCLUDE
      PATTERN "*.am" EXCLUDE
      PATTERN "*.in" EXCLUDE
      PATTERN "Makefile" EXCLUDE
      PATTERN "*.vcxproj" EXCLUDE
      PATTERN "*.vcxproj.filters" EXCLUDE
      PATTERN "*.vcxproj.user" EXCLUDE
      PATTERN "CTest*" EXCLUDE
      PATTERN "*~" EXCLUDE
    )
  ENDIF()
ENDFUNCTION()
