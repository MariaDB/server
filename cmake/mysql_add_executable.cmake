# Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
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

# Add executable plus some additional MySQL specific stuff
# Usage (same as for standard CMake's ADD_EXECUTABLE)
#
# MYSQL_ADD_EXECUTABLE(target source1...sourceN)
#
# MySQL specifics:
# - instruct CPack to install executable under ${CMAKE_INSTALL_PREFIX}/bin directory
# On Windows :
# - add version resource
# - instruct CPack to do autenticode signing if SIGNCODE is set

INCLUDE(CMakeParseArguments)

FUNCTION (MYSQL_ADD_EXECUTABLE)
  # Pass-through arguments for ADD_EXECUTABLE
  CMAKE_PARSE_ARGUMENTS(ARG
   "WIN32;MACOSX_BUNDLE;EXCLUDE_FROM_ALL"
   "DESTINATION;COMPONENT"
   ""
   ${ARGN}
  )
  LIST(GET ARG_UNPARSED_ARGUMENTS 0 target)
  LIST(REMOVE_AT  ARG_UNPARSED_ARGUMENTS 0)

  SET(sources ${ARG_UNPARSED_ARGUMENTS})
  ADD_VERSION_INFO(${target} EXECUTABLE sources)

  IF(MSVC)
    # Add compatibility manifest, to fix GetVersionEx on Windows 8.1 and later
    IF (CMAKE_VERSION VERSION_GREATER 3.3)
      SET(sources ${sources} ${PROJECT_SOURCE_DIR}/cmake/win_compatibility.manifest)
    ENDIF()
  ENDIF()

  IF (ARG_WIN32)
    SET(WIN32 WIN32)
  ELSE()
    UNSET(WIN32)
  ENDIF()
  IF (ARG_MACOSX_BUNDLE)
    SET(MACOSX_BUNDLE MACOSX_BUNDLE)
  ELSE()
    UNSET(MACOSX_BUNDLE)
  ENDIF()
  IF (ARG_EXCLUDE_FROM_ALL)
    SET(EXCLUDE_FROM_ALL EXCLUDE_FROM_ALL)
  ELSE()
    UNSET(EXCLUDE_FROM_ALL)
  ENDIF()

  ADD_EXECUTABLE(${target} ${WIN32} ${MACOSX_BUNDLE} ${EXCLUDE_FROM_ALL} ${sources})

  # tell CPack where to install
  IF(NOT ARG_EXCLUDE_FROM_ALL)
    IF(NOT ARG_DESTINATION)
      SET(ARG_DESTINATION ${INSTALL_BINDIR})
    ENDIF()
    IF(ARG_COMPONENT)
      SET(COMP ${ARG_COMPONENT})
    ELSEIF(MYSQL_INSTALL_COMPONENT)
      SET(COMP ${MYSQL_INSTALL_COMPONENT})
    ELSE()
      SET(COMP Client)
    ENDIF()
    IF (COMP MATCHES ${SKIP_COMPONENTS})
      RETURN()
    ENDIF()

    IF (WITH_STRIPPED_CLIENT AND NOT target STREQUAL mariadbd)
      INSTALL(CODE "SET(CMAKE_INSTALL_DO_STRIP 1)" COMPONENT ${COMP})
      SET(reset_strip ON)
    ENDIF()

    MYSQL_INSTALL_TARGETS(${target} DESTINATION ${ARG_DESTINATION} COMPONENT ${COMP})
    
    IF (reset_strip)
      INSTALL(CODE "SET(CMAKE_INSTALL_DO_STRIP 0)" COMPONENT ${COMP})
    ENDIF()
  ENDIF()

  # create MySQL named "legacy links"
  GET_SYMLINK(${target} link)
  IF(link)
    IF(UNIX)
      ADD_CUSTOM_COMMAND(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
         ${target} ${link}
        COMMENT "Creating ${link} link"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR})
      INSTALL(PROGRAMS
         ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${link}
         DESTINATION
         ${ARG_DESTINATION}
         COMPONENT ${COMP})
    ELSE()
      # Windows note:
      # Here, hardlinks are used, because cmake can't install symlinks.
      # In packages, there are won't be links, just copies.
      SET(link ${link}.exe)
      ADD_CUSTOM_COMMAND(TARGET ${target} POST_BUILD
        COMMAND cmake -E remove -f ${link}
        COMMAND mklink /H ${link} $<TARGET_FILE_NAME:${target}>
        COMMENT "Creating ${link} link"
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>)
      INSTALL(PROGRAMS $<TARGET_FILE_DIR:${target}>/${link} DESTINATION ${ARG_DESTINATION} COMPONENT ${COMP})
    ENDIF()
  ENDIF()
ENDFUNCTION()
