# Copyright (c) 2009, 2010, Oracle and/or its affiliates.
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


# This file exports macros that emulate some functionality found  in GNU libtool
# on Unix systems. One such feature is convenience libraries. In this context,
# convenience library is a static library that can be linked to shared library
# On systems that force position-independent code, linking into shared library
# normally requires compilation with a special flag (often -fPIC). To enable 
# linking static libraries to shared, we compile source files that come into 
# static library with the PIC flag (${CMAKE_SHARED_LIBRARY_C_FLAGS} in CMake)
# Some systems, like Windows or OSX do not need special compilation (Windows 
# never uses PIC and OSX always uses it). 
#
# The intention behind convenience libraries is simplify the build and to reduce
# excessive recompiles.

# Except for convenience libraries, this file provides macros to merge static 
# libraries (we need it for mysqlclient) and to create shared library out of 
# convenience libraries(again, for mysqlclient)

# Following macros are exported
# - ADD_CONVENIENCE_LIBRARY(target source1...sourceN)
# This macro creates convenience library. The functionality is similar to 
# ADD_LIBRARY(target STATIC source1...sourceN), the difference is that resulting 
# library can always be linked to shared library
# 
# - MERGE_LIBRARIES(target [STATIC|SHARED|MODULE]  [linklib1 .... linklibN]
#  [EXPORTS exported_func1 .... exported_func_N]
#  [OUTPUT_NAME output_name]
# This macro merges several static libraries into a single one or creates a shared
# library from several convenience libraries

# Important global flags 
# - WITH_PIC : If set, it is assumed that everything is compiled as position
# independent code (that is CFLAGS/CMAKE_C_FLAGS contain -fPIC or equivalent)
# If defined, ADD_CONVENIENCE_LIBRARY does not add PIC flag to compile flags
#
# - DISABLE_SHARED: If set, it is assumed that shared libraries are not produced
# during the build. ADD_CONVENIENCE_LIBRARY does not add anything to compile flags


GET_FILENAME_COMPONENT(MYSQL_CMAKE_SCRIPT_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)
IF(WIN32 OR CYGWIN OR APPLE OR WITH_PIC OR DISABLE_SHARED OR NOT CMAKE_SHARED_LIBRARY_C_FLAGS)
 SET(_SKIP_PIC 1)
ENDIF()

INCLUDE(CMakeParseArguments)
# CREATE_EXPORTS_FILE (VAR target api_functions)
# Internal macro, used to create source file for shared libraries that 
# otherwise consists entirely of "convenience" libraries. On Windows, 
# also exports API functions as dllexport. On unix, creates a dummy file 
# that references all exports and this prevents linker from creating an 
# empty library(there are unportable alternatives, --whole-archive)
MACRO(CREATE_EXPORTS_FILE VAR TARGET API_FUNCTIONS)
  IF(WIN32)
    SET(DUMMY ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_dummy.c)
    SET(EXPORTS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_exports.def)
    CONFIGURE_FILE_CONTENT("" ${DUMMY})
    SET(CONTENT "EXPORTS\n")
    FOREACH(FUNC ${API_FUNCTIONS})
      SET(CONTENT "${CONTENT} ${FUNC}\n")
    ENDFOREACH()
    CONFIGURE_FILE_CONTENT(${CONTENT} ${EXPORTS})
    SET(${VAR} ${DUMMY} ${EXPORTS})
  ELSE()
    SET(EXPORTS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_exports_file.cc)
    SET(CONTENT)
    FOREACH(FUNC ${API_FUNCTIONS})
      SET(CONTENT "${CONTENT} extern void* ${FUNC}\;\n")
    ENDFOREACH()
    SET(CONTENT "${CONTENT} void *${TARGET}_api_funcs[] = {\n")
    FOREACH(FUNC ${API_FUNCTIONS})
     SET(CONTENT "${CONTENT} &${FUNC},\n")
    ENDFOREACH()
    SET(CONTENT "${CONTENT} (void *)0\n}\;")
    CONFIGURE_FILE_CONTENT(${CONTENT} ${EXPORTS})
    # Avoid "function redeclared as variable" error
    # when using gcc/clang option -flto(link time optimization)
    IF(" ${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} " MATCHES " -flto")
      SET_SOURCE_FILES_PROPERTIES(${EXPORTS} PROPERTIES COMPILE_FLAGS "-fno-lto")
    ENDIF()
    SET(${VAR} ${EXPORTS})
  ENDIF()
ENDMACRO()


# MYSQL_ADD_CONVENIENCE_LIBRARY(name source1...sourceN)
# Create static library that can be linked to shared library.
# On systems that force position-independent code, adds -fPIC or 
# equivalent flag to compile flags.
MACRO(ADD_CONVENIENCE_LIBRARY)
  SET(TARGET ${ARGV0})
  SET(SOURCES ${ARGN})
  LIST(REMOVE_AT SOURCES 0)
  ADD_LIBRARY(${TARGET} STATIC ${SOURCES})
  IF(NOT _SKIP_PIC)
    SET_TARGET_PROPERTIES(${TARGET} PROPERTIES  COMPILE_FLAGS
    "${CMAKE_SHARED_LIBRARY_C_FLAGS}")
  ENDIF()
ENDMACRO()


# Write content to file, using CONFIGURE_FILE
# The advantage compared to FILE(WRITE) is that timestamp
# does not change if file already has the same content
MACRO(CONFIGURE_FILE_CONTENT content file)
 SET(CMAKE_CONFIGURABLE_FILE_CONTENT 
  "${content}\n")
 CONFIGURE_FILE(
  ${MYSQL_CMAKE_SCRIPT_DIR}/configurable_file_content.in
  ${file}
  @ONLY)
ENDMACRO()

# Merge static libraries into a big static lib. The resulting library 
# should not not have dependencies on other static libraries.
# We use it in MariaDB to merge mysys,dbug,vio etc into the embedded server
# mariadbd.

MACRO(MERGE_STATIC_LIBS TARGET OUTPUT_NAME LIBS_TO_MERGE)
  # To produce a library we need at least one source file.
  # It is created by ADD_CUSTOM_COMMAND below and will helps 
  # also help to track dependencies.
  SET(SOURCE_FILE ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_depends.c)
  ADD_LIBRARY(${TARGET} STATIC ${SOURCE_FILE})
  SET_TARGET_PROPERTIES(${TARGET} PROPERTIES OUTPUT_NAME ${OUTPUT_NAME})
  IF(NOT _SKIP_PIC)
    SET_TARGET_PROPERTIES(${TARGET} PROPERTIES  COMPILE_FLAGS
    "${CMAKE_SHARED_LIBRARY_C_FLAGS}")
  ENDIF()

  SET(OSLIBS)
  FOREACH(LIB ${LIBS_TO_MERGE})
    IF(NOT TARGET ${LIB})
       # 3rd party library like libz.so. Make sure that everything
       # that links to our library links to this one as well.
       LIST(APPEND OSLIBS ${LIB})
    ELSE()
      GET_TARGET_PROPERTY(LIB_TYPE ${LIB} TYPE)
      # This is a target in current project
      # (can be a static or shared lib)
      IF(LIB_TYPE STREQUAL "STATIC_LIBRARY")
        SET(STATIC_TGTS ${STATIC_TGTS} ${LIB})
        SET(STATIC_LIBS ${STATIC_LIBS} $<TARGET_FILE:${LIB}>)
        ADD_DEPENDENCIES(${TARGET} ${LIB})
        # Extract dependent OS libraries
        GET_DEPENDEND_OS_LIBS(${LIB} LIB_OSLIBS)
        LIST(APPEND OSLIBS ${LIB_OSLIBS})
      ELSE()
        # This is a shared library our static lib depends on.
        LIST(APPEND OSLIBS ${LIB})
      ENDIF()
    ENDIF()
  ENDFOREACH()
  # With static libraries the order matter to some linkers.
  # REMOVE_DUPLICATES will keep the first entry and because
  # the linker requirement we want to keep the last.
  IF(STATIC_LIBS)
    LIST(REVERSE STATIC_LIBS)
    LIST(REMOVE_DUPLICATES STATIC_LIBS)
    LIST(REVERSE STATIC_LIBS)
  ENDIF()
  IF(OSLIBS)
    LIST(REVERSE OSLIBS)
    LIST(REMOVE_DUPLICATES OSLIBS)
    LIST(REVERSE OSLIBS)
    TARGET_LINK_LIBRARIES(${TARGET} LINK_PRIVATE ${OSLIBS})
  ENDIF()

  # Make the generated dummy source file depended on all static input
  # libs. If input lib changes,the source file is touched
  # which causes the desired effect (relink).
  ADD_CUSTOM_COMMAND( 
    OUTPUT  ${SOURCE_FILE}
    COMMAND ${CMAKE_COMMAND}  -E touch ${SOURCE_FILE}
    DEPENDS ${STATIC_TGTS})

  IF(MSVC)
    # To merge libs, just pass them to lib.exe command line.
    SET(LINKER_EXTRA_FLAGS "")
    FOREACH(LIB ${STATIC_LIBS})
      SET(LINKER_EXTRA_FLAGS "${LINKER_EXTRA_FLAGS} ${LIB}")
    ENDFOREACH()
    SET_TARGET_PROPERTIES(${TARGET} PROPERTIES STATIC_LIBRARY_FLAGS 
      "${LINKER_EXTRA_FLAGS}")
  ELSE()
    IF(APPLE)
      # Use OSX's libtool to merge archives (ihandles universal 
      # binaries properly)
      ADD_CUSTOM_COMMAND(TARGET ${TARGET} POST_BUILD
        COMMAND rm $<TARGET_FILE:${TARGET}>
        COMMAND libtool -static -o $<TARGET_FILE:${TARGET}>
        ${STATIC_LIBS}
      )  
    ELSE()
      # Generic Unix, Cygwin or MinGW. In post-build step, call
      # script, that uses a MRI script to append static archives.
      IF(CMAKE_VERSION VERSION_LESS "3.0")
        SET(MRI_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.mri")
      ELSE()
        SET(MRI_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-$<CONFIG>.mri")
      ENDIF()
      SET(MRI_SCRIPT_TPL "${MRI_SCRIPT}.tpl")

      SET(SCRIPT_CONTENTS "CREATE $<TARGET_FILE:${TARGET}>\n")
      FOREACH(LIB ${STATIC_LIBS})
        SET(SCRIPT_CONTENTS "${SCRIPT_CONTENTS}ADDLIB ${LIB}\n")
      ENDFOREACH()
      FILE(WRITE ${MRI_SCRIPT_TPL} "${SCRIPT_CONTENTS}\nSAVE\nEND\n")
      FILE(GENERATE OUTPUT ${MRI_SCRIPT} INPUT ${MRI_SCRIPT_TPL})

      ADD_CUSTOM_COMMAND(TARGET ${TARGET} POST_BUILD
        DEPENDS ${MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND}
        ARGS
          -DTARGET_SCRIPT="${MRI_SCRIPT}"
          -DTOP_DIR="${CMAKE_BINARY_DIR}"
          -DCMAKE_AR="${CMAKE_AR}"
          -P "${MYSQL_CMAKE_SCRIPT_DIR}/merge_archives_unix.cmake"
        COMMAND ${CMAKE_RANLIB}
        ARGS $<TARGET_FILE:${TARGET}>
      )
      SET_DIRECTORY_PROPERTIES(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES ${MRI_SCRIPT_TPL})
      SET_DIRECTORY_PROPERTIES(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES ${MRI_SCRIPT}.mri)
    ENDIF()
  ENDIF()
ENDMACRO()

# Create libs from libs.
# Merges static libraries, creates shared libraries out of convenience libraries.
# MERGE_LIBRARIES(target [STATIC|SHARED|MODULE] 
#  [linklib1 .... linklibN]
#  [EXPORTS exported_func1 .... exportedFuncN]
#  [OUTPUT_NAME output_name]
#)
MACRO(MERGE_LIBRARIES)
  CMAKE_PARSE_ARGUMENTS(ARG
    "STATIC;SHARED;MODULE;NOINSTALL"
    "OUTPUT_NAME;COMPONENT;VERSION;SOVERSION"
    "EXPORTS"
    ${ARGN}
  )
  LIST(GET ARG_UNPARSED_ARGUMENTS 0 TARGET)
  SET(LIBS ${ARG_UNPARSED_ARGUMENTS})
  LIST(REMOVE_AT LIBS 0)
  IF(ARG_STATIC)
    IF (NOT ARG_OUTPUT_NAME)
      SET(ARG_OUTPUT_NAME ${TARGET})
    ENDIF()
    MERGE_STATIC_LIBS(${TARGET} ${ARG_OUTPUT_NAME} "${LIBS}") 
  ELSEIF(ARG_SHARED OR ARG_MODULE)
    IF(ARG_SHARED)
      SET(LIBTYPE SHARED)
    ELSE()
      SET(LIBTYPE MODULE)
    ENDIF()
    # check for non-PIC libraries
    IF(NOT _SKIP_PIC)
      FOREACH(LIB ${LIBS})
        GET_TARGET_PROPERTY(LTYPE ${LIB} TYPE)
        IF(LTYPE STREQUAL "STATIC_LIBRARY")
          GET_TARGET_PROPERTY(LIB_COMPILE_FLAGS ${LIB} COMPILE_FLAGS)
          STRING(REPLACE "${CMAKE_SHARED_LIBRARY_C_FLAGS}" 
            "<PIC_FLAG>" LIB_COMPILE_FLAGS "${LIB_COMPILE_FLAGS}")
          IF(NOT LIB_COMPILE_FLAGS MATCHES "<PIC_FLAG>")
            MESSAGE(FATAL_ERROR 
            "Attempted to link non-PIC static library ${LIB} to shared library ${TARGET}\n"
            "Please use ADD_CONVENIENCE_LIBRARY, instead of ADD_LIBRARY for ${LIB}"
            )
          ENDIF()
        ENDIF()
      ENDFOREACH()
    ENDIF()
    CREATE_EXPORTS_FILE(SRC ${TARGET} "${ARG_EXPORTS}")
    IF(NOT ARG_NOINSTALL)
      ADD_VERSION_INFO(${TARGET} SHARED SRC)
    ENDIF()
    IF(ARG_VERSION)
      SET(VERS VERSION ${ARG_VERSION})
    ENDIF()
    ADD_LIBRARY(${TARGET} ${LIBTYPE} ${SRC})
    IF (ARG_VERSION)
      SET_TARGET_PROPERTIES(${TARGET} PROPERTIES VERSION  ${ARG_VERSION})
    ENDIF()
    IF (ARG_SOVERSION)
      SET_TARGET_PROPERTIES(${TARGET} PROPERTIES SOVERSION  ${ARG_VERSION})
    ENDIF()
    TARGET_LINK_LIBRARIES(${TARGET} LINK_PRIVATE ${LIBS})
    IF(ARG_OUTPUT_NAME)
      SET_TARGET_PROPERTIES(${TARGET} PROPERTIES OUTPUT_NAME "${ARG_OUTPUT_NAME}")
    ENDIF()
  ELSE()
    MESSAGE(FATAL_ERROR "Unknown library type")
  ENDIF()
  IF(NOT ARG_NOINSTALL)
    IF(ARG_COMPONENT)
      SET(COMP COMPONENT ${ARG_COMPONENT}) 
    ENDIF()
    MYSQL_INSTALL_TARGETS(${TARGET} DESTINATION "${INSTALL_LIBDIR}" ${COMP})
  ENDIF()
  IF(ARG_SHARED AND LINK_FLAG_NO_UNDEFINED)
    # Do not allow undefined symbols in shared libraries
    GET_TARGET_PROPERTY(TARGET_LINK_FLAGS ${TARGET} LINK_FLAGS)
    IF(NOT TARGET_LINK_FLAGS)
      SET(TARGET_LINK_FLAGS)
    ENDIF()
    SET_TARGET_PROPERTIES(${TARGET} PROPERTIES LINK_FLAGS 
      "${TARGET_LINK_FLAGS} ${LINK_FLAG_NO_UNDEFINED}")
  ENDIF() 
ENDMACRO()

FUNCTION(GET_DEPENDEND_OS_LIBS target result)
  GET_TARGET_PROPERTY(DEPS ${target} LINK_LIBRARIES)
  IF(DEPS)
    FOREACH(lib ${DEPS})
      IF(NOT TARGET ${lib})
        SET(ret ${ret} ${lib})
      ENDIF()
    ENDFOREACH()
  ENDIF()
  SET(${result} ${ret} PARENT_SCOPE)
ENDFUNCTION()

INCLUDE(CheckCCompilerFlag)

SET(VISIBILITY_HIDDEN_FLAG)

IF(CMAKE_C_COMPILER_ID MATCHES "SunPro")
  SET(VISIBILITY_HIDDEN_FLAG "-xldscope=hidden")
ELSEIF(UNIX)
  CHECK_C_COMPILER_FLAG("-fvisibility=hidden" HAVE_VISIBILITY_HIDDEN)
  IF(HAVE_VISIBILITY_HIDDEN)
    SET(VISIBILITY_HIDDEN_FLAG "-fvisibility=hidden")
  ENDIF()
ENDIF()

# We try to hide the symbols in bundled libraries to avoid name clashes with
# other libraries like openssl.
FUNCTION(RESTRICT_SYMBOL_EXPORTS target)
  IF(VISIBILITY_HIDDEN_FLAG)
    GET_TARGET_PROPERTY(COMPILE_FLAGS ${target} COMPILE_FLAGS)
    IF(NOT COMPILE_FLAGS)
      # Avoid COMPILE_FLAGS-NOTFOUND
      SET(COMPILE_FLAGS)
    ENDIF()
    SET_TARGET_PROPERTIES(${target} PROPERTIES 
      COMPILE_FLAGS "${COMPILE_FLAGS} ${VISIBILITY_HIDDEN_FLAG}")
  ENDIF()
ENDFUNCTION()

# The MSVC /GL flag, used for link-time code generation
# creates objects files with a format not readable by tools
# i.e exporting all symbols is not possible with IPO
# To workaround this, we disable INTERPROCEDURAL_OPTIMIZATION
# for some static libraries.

FUNCTION (MAYBE_DISABLE_IPO target)
  IF(MSVC AND (NOT CLANG_CL) AND (NOT WITHOUT_DYNAMIC_PLUGINS))
    SET_TARGET_PROPERTIES(${target} PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION OFF
      INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF
      INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
      INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF
      INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
  ENDIF()
ENDFUNCTION()
