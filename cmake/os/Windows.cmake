# Copyright (c) 2010, 2015, Oracle and/or its affiliates. All rights reserved.
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

# This file includes Windows specific hacks, mostly around compiler flags

INCLUDE (CheckCSourceCompiles)
INCLUDE (CheckCXXSourceCompiles)
INCLUDE (CheckStructHasMember)
INCLUDE (CheckLibraryExists)
INCLUDE (CheckFunctionExists)
INCLUDE (CheckCSourceRuns)
INCLUDE (CheckSymbolExists)
INCLUDE (CheckTypeSize)

IF(MSVC)
  IF(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL ARM64)
   SET(MSVC_ARM64 1)
   SET(MSVC_INTEL 0)
  ELSE()
   SET(MSVC_INTEL 1)
  ENDIF()
ENDIF()

# avoid running system checks by using pre-cached check results
# system checks are expensive on VS since every tiny program is to be compiled in 
# a VC solution.
GET_FILENAME_COMPONENT(_SCRIPT_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)
INCLUDE(${_SCRIPT_DIR}/WindowsCache.cmake)
 

# OS display name (version_compile_os etc).
# Used by the test suite to ignore bugs on some platforms, 
IF(CMAKE_SIZEOF_VOID_P MATCHES 8)
  SET(SYSTEM_TYPE "Win64")
ELSE()
  SET(SYSTEM_TYPE "Win32")
ENDIF()

# Intel compiler is almost Visual C++
# (same compile flags etc). Set MSVC flag
IF(CMAKE_C_COMPILER MATCHES "icl")
 SET(MSVC TRUE)
ENDIF()

IF(MSVC  AND CMAKE_CXX_COMPILER_ID MATCHES Clang)
 SET(CLANG_CL TRUE)
ENDIF()

ADD_DEFINITIONS(-D_CRT_SECURE_NO_DEPRECATE)
ADD_DEFINITIONS(-D_WIN32_WINNT=0x0A00)
# We do not want the windows.h , or winsvc.h macros min/max
ADD_DEFINITIONS(-DNOMINMAX -DNOSERVICE)
# Speed up build process excluding unused header files
ADD_DEFINITIONS(-DWIN32_LEAN_AND_MEAN)
  
# Adjust compiler and linker flags
IF(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
   # mininal architecture flags, i486 enables GCC atomics
  ADD_DEFINITIONS(-march=i486)
ENDIF()

MACRO(ENABLE_SANITIZERS)
  IF(NOT  MSVC)
    MESSAGE(FATAL_ERROR "clang-cl or MSVC necessary to enable asan/ubsan")
  ENDIF()
  # currently, asan is broken with static CRT.
  IF(CLANG_CL AND NOT(MSVC_CRT_TYPE STREQUAL "/MD"))
    SET(MSVC_CRT_TYPE "/MD" CACHE  INTERNAL "" FORCE)
  ENDIF()
  IF(CMAKE_SIZEOF_VOID_P EQUAL 4)
    SET(ASAN_ARCH i386)
  ELSE()
    SET(ASAN_ARCH x86_64)
  ENDIF()

   # After installation, clang lib directory should be added to PATH
  # (e.g C:/Program Files/LLVM/lib/clang/5.0.1/lib/windows)
  SET(SANITIZER_LIBS)
  SET(SANITIZER_LINK_LIBRARIES)
  SET(SANITIZER_COMPILE_FLAGS)
  IF(WITH_ASAN)
    IF(CLANG_CL)
      LIST(APPEND SANITIZER_LIBS
        clang_rt.asan_dynamic-${ASAN_ARCH}.lib clang_rt.asan_dynamic_runtime_thunk-${ASAN_ARCH}.lib)
    ENDIF()
    STRING(APPEND SANITIZER_COMPILE_FLAGS " -fsanitize=address")
  ENDIF()
  IF(WITH_UBSAN)
    STRING(APPEND SANITIZER_COMPILE_FLAGS " -fsanitize=undefined -fno-sanitize=alignment")
  ENDIF()
  FOREACH(lib ${SANITIZER_LIBS})
    FIND_LIBRARY(${lib}_fullpath ${lib})
    IF(NOT ${lib}_fullpath)
      MESSAGE(FATAL_ERROR "Can't enable sanitizer : missing ${lib}")
    ENDIF()
    LIST(APPEND CMAKE_REQUIRED_LIBRARIES ${${lib}_fullpath})
    STRING(APPEND CMAKE_C_STANDARD_LIBRARIES " \"${${lib}_fullpath}\" ")
    STRING(APPEND CMAKE_CXX_STANDARD_LIBRARIES " \"${${lib}_fullpath}\" ")
  ENDFOREACH()
  STRING(APPEND CMAKE_C_FLAGS ${SANITIZER_COMPILE_FLAGS})
  STRING(APPEND CMAKE_CXX_FLAGS ${SANITIZER_COMPILE_FLAGS})
ENDMACRO()


IF(MSVC)
  IF(MSVC_VERSION LESS 1920)
    MESSAGE(FATAL_ERROR "Visual Studio 2019 or later is required")
  ENDIF()
  # Disable mingw based pkg-config found in Strawberry perl
  SET(PKG_CONFIG_EXECUTABLE 0 CACHE INTERNAL "")

  SET(MSVC_CRT_TYPE /MT CACHE STRING
    "Runtime library - specify runtime library for linking (/MT,/MTd,/MD,/MDd)"
  )
  SET(VALID_CRT_TYPES /MTd /MDd /MD /MT)
  IF (NOT ";${VALID_CRT_TYPES};" MATCHES ";${MSVC_CRT_TYPE};")
    MESSAGE(FATAL_ERROR "Invalid value ${MSVC_CRT_TYPE} for MSVC_CRT_TYPE, choose one of /MT,/MTd,/MD,/MDd ")
  ENDIF()

  IF(MSVC_CRT_TYPE MATCHES "/MD")
   # Dynamic runtime (DLLs), need to install CRT libraries.
   SET(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT VCCRT)
   SET(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
   IF(MSVC_CRT_TYPE STREQUAL "/MDd")
     SET (CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
   ENDIF()
   INCLUDE(InstallRequiredSystemLibraries)
  ENDIF()

  IF(WITH_ASAN AND (NOT CLANG_CL))
    SET(DYNAMIC_UCRT_LINK_DEFAULT OFF)
  ELSE()
    SET(DYNAMIC_UCRT_LINK_DEFAULT ON)
  ENDIF()

  OPTION(DYNAMIC_UCRT_LINK "Link Universal CRT dynamically, if MSVC_CRT_TYPE=/MT" ${DYNAMIC_UCRT_LINK_DEFAULT})
  SET(DYNAMIC_UCRT_LINKER_OPTION " /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib")

  # Enable debug info also in Release build,
  # and create PDB to be able to analyze crashes.
  FOREACH(type EXE SHARED MODULE)
   SET(CMAKE_${type}_LINKER_FLAGS_RELEASE
     "${CMAKE_${type}_LINKER_FLAGS_RELEASE} /debug")
   SET(CMAKE_${type}_LINKER_FLAGS_MINSIZEREL
     "${CMAKE_${type}_LINKER_FLAGS_MINSIZEREL} /debug")
  ENDFOREACH()
  
  # Force runtime libraries
  # Compile with /Zi to get debugging information

  FOREACH(lang C CXX)
    SET(CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE} /Zi")
  ENDFOREACH()
  FOREACH(flag
   CMAKE_C_FLAGS CMAKE_CXX_FLAGS
   CMAKE_C_FLAGS_INIT CMAKE_CXX_FLAGS_INIT
   CMAKE_C_FLAGS_RELEASE    CMAKE_C_FLAGS_RELWITHDEBINFO 
   CMAKE_C_FLAGS_DEBUG      CMAKE_C_FLAGS_DEBUG_INIT 
   CMAKE_CXX_FLAGS_RELEASE  CMAKE_CXX_FLAGS_RELWITHDEBINFO
   CMAKE_CXX_FLAGS_DEBUG    CMAKE_CXX_FLAGS_DEBUG_INIT
   CMAKE_C_FLAGS_MINSIZEREL  CMAKE_CXX_FLAGS_MINSIZEREL
   )
   STRING(REGEX REPLACE "/M[TD][d]?"  "${MSVC_CRT_TYPE}" "${flag}"  "${${flag}}" )
   STRING(REPLACE "/ZI " "/Zi "  "${flag}"  "${${flag}}")
   IF((NOT "${${flag}}" MATCHES "/Zi") AND (NOT "${${flag}}" MATCHES "/Z7"))
    STRING(APPEND ${flag} " /Zi")
   ENDIF()
   # Remove inlining flags, added by CMake, if any.
   # Compiler default is fine.
   STRING(REGEX REPLACE "/Ob[0-3]" "" "${flag}"  "${${flag}}" )
  ENDFOREACH()

  # Allow to overwrite the inlining flag
  SET(MSVC_INLINE "" CACHE STRING
    "MSVC Inlining option, either empty, or one of /Ob0,/Ob1,/Ob2,/Ob3")
  IF(MSVC_INLINE MATCHES "/Ob[0-3]")
    ADD_COMPILE_OPTIONS(${MSVC_INLINE})
  ELSEIF(NOT(MSVC_INLINE STREQUAL ""))
    MESSAGE(FATAL_ERROR "Invalid option for MSVC_INLINE")
  ENDIF()

  IF(WITH_ASAN OR WITH_UBSAN)
    # Workaround something Linux specific
    SET(SECURITY_HARDENED 0 CACHE INTERNAL "" FORCE)
    ENABLE_SANITIZERS()
  ENDIF()

  IF(CLANG_CL)
     SET(CLANG_CL_FLAGS
"-Wno-unknown-warning-option -Wno-unused-private-field \
-Wno-unused-parameter -Wno-inconsistent-missing-override \
-Wno-unused-command-line-argument -Wno-pointer-sign \
-Wno-deprecated-register -Wno-missing-braces \
-Wno-unused-function -Wno-unused-local-typedef -msse4.2 "
    )
    IF(CMAKE_SIZEOF_VOID_P MATCHES 8)
      STRING(APPEND CLANG_CL_FLAGS "-mpclmul ")
    ENDIF()
    STRING(APPEND CMAKE_C_FLAGS " ${CLANG_CL_FLAGS} ${MSVC_CRT_TYPE}")
    STRING(APPEND CMAKE_CXX_FLAGS " ${CLANG_CL_FLAGS}  ${MSVC_CRT_TYPE}")
  ENDIF()

  FOREACH(type EXE SHARED MODULE)
   STRING(REGEX REPLACE "/STACK:([^ ]+)" "" CMAKE_${type}_LINKER_FLAGS "${CMAKE_${type}_LINKER_FLAGS}")
   IF(WITH_ASAN)
     SET(build_types RELWITHDEBINFO DEBUG)
   ELSE()
     SET(build_types RELWITHDEBINFO)
   ENDIF()
   FOREACH(btype ${build_types})
     STRING(REGEX REPLACE "/INCREMENTAL:([^ ]+)" "/INCREMENTAL:NO" CMAKE_${type}_LINKER_FLAGS_${btype} "${CMAKE_${type}_LINKER_FLAGS_${btype}}")
     STRING(REGEX REPLACE "/INCREMENTAL$" "/INCREMENTAL:NO" CMAKE_${type}_LINKER_FLAGS_${btype} "${CMAKE_${type}_LINKER_FLAGS_${btype}}")
   ENDFOREACH()
   IF(NOT CLANG_CL)
     STRING(APPEND CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO " /release /OPT:REF,ICF")
   ENDIF()
   IF(DYNAMIC_UCRT_LINK AND (MSVC_CRT_TYPE STREQUAL "/MT"))
     FOREACH(config RELEASE RELWITHDEBINFO DEBUG MINSIZEREL)
       STRING(APPEND CMAKE_${type}_LINKER_FLAGS_${config} ${DYNAMIC_UCRT_LINKER_OPTION})
     ENDFOREACH()
   ENDIF()
  ENDFOREACH()

  
  # Mark 32 bit executables large address aware so they can 
  # use > 2GB address space
  IF(CMAKE_SIZEOF_VOID_P MATCHES 4)
   STRING(APPEND CMAKE_EXE_LINKER_FLAGS " /LARGEADDRESSAWARE")
  ENDIF()
  
  # Speed up multiprocessor build
  IF (NOT CLANG_CL)
    STRING(APPEND CMAKE_C_FLAGS " /MP")
    STRING(APPEND CMAKE_CXX_FLAGS " /MP")
    STRING(APPEND CMAKE_C_FLAGS_RELWITHDEBINFO " /Gw")
    STRING(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " /Gw")
  ENDIF()
  
  #TODO: update the code and remove the disabled warnings
  STRING(APPEND CMAKE_C_FLAGS " /we4700 /we4311 /we4477 /we4302 /we4090")
  STRING(APPEND CMAKE_CXX_FLAGS " /we4099 /we4700 /we4311 /we4477 /we4302 /we4090")
  IF(MSVC_VERSION GREATER 1910  AND NOT CLANG_CL)
    STRING(APPEND CMAKE_CXX_FLAGS " /permissive-")
    STRING(APPEND CMAKE_C_FLAGS " /diagnostics:caret")
    STRING(APPEND CMAKE_CXX_FLAGS " /diagnostics:caret")
  ENDIF()
  ADD_DEFINITIONS(-D_CRT_NONSTDC_NO_WARNINGS)
  IF(MYSQL_MAINTAINER_MODE MATCHES "ERR")
    STRING(APPEND CMAKE_C_FLAGS " /WX")
    STRING(APPEND CMAKE_CXX_FLAGS " /WX")
    FOREACH(type EXE SHARED MODULE)
      FOREACH(cfg RELEASE DEBUG RELWITHDEBINFO)
        SET(CMAKE_${type}_LINKER_FLAGS_${cfg} "${CMAKE_${type}_LINKER_FLAGS_${cfg}} /WX")
      ENDFOREACH()
    ENDFOREACH()
  ENDIF()

  IF(FAST_BUILD)
    STRING (REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  ELSEIF (NOT CLANG_CL)
    STRING(APPEND CMAKE_CXX_FLAGS_RELEASE " /d2OptimizeHugeFunctions")
    STRING(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " /d2OptimizeHugeFunctions")
  ENDIF()
ENDIF()

# Always link with socket/synchronization libraries
STRING(APPEND CMAKE_C_STANDARD_LIBRARIES " ws2_32.lib synchronization.lib")
STRING(APPEND CMAKE_CXX_STANDARD_LIBRARIES " ws2_32.lib synchronization.lib")

# System checks
SET(SIGNAL_WITH_VIO_CLOSE 1) # Something that runtime team needs

# IPv6 constants appeared in Vista SDK first. We need to define them in any case if they are 
# not in headers, to handle dual mode sockets correctly.
CHECK_SYMBOL_EXISTS(IPPROTO_IPV6 "winsock2.h" HAVE_IPPROTO_IPV6)
IF(NOT HAVE_IPPROTO_IPV6)
  SET(HAVE_IPPROTO_IPV6 41)
ENDIF()
CHECK_SYMBOL_EXISTS(IPV6_V6ONLY  "winsock2.h;ws2ipdef.h" HAVE_IPV6_V6ONLY)
IF(NOT HAVE_IPV6_V6ONLY)
  SET(IPV6_V6ONLY 27)
ENDIF()

# Some standard functions exist there under different
# names (e.g popen is _popen or strok_r is _strtok_s)
# If a replacement function exists, HAVE_FUNCTION is
# defined to 1. CMake variable <function_name> will also
# be defined to the replacement name.
# So for example, CHECK_FUNCTION_REPLACEMENT(popen _popen)
# will define HAVE_POPEN to 1 and set variable named popen
# to _popen. If the header template, one needs to have
# cmakedefine popen @popen@ which will expand to 
# define popen _popen after CONFIGURE_FILE

MACRO(CHECK_FUNCTION_REPLACEMENT function replacement)
  STRING(TOUPPER ${function} function_upper)
  CHECK_FUNCTION_EXISTS(${function} HAVE_${function_upper})
  IF(NOT HAVE_${function_upper})
    CHECK_FUNCTION_EXISTS(${replacement}  HAVE_${replacement})
    IF(HAVE_${replacement})
      SET(HAVE_${function_upper} 1 )
      SET(${function} ${replacement})
    ENDIF()
  ENDIF()
ENDMACRO()
MACRO(CHECK_SYMBOL_REPLACEMENT symbol replacement header)
  STRING(TOUPPER ${symbol} symbol_upper)
  CHECK_SYMBOL_EXISTS(${symbol} ${header} HAVE_${symbol_upper})
  IF(NOT HAVE_${symbol_upper})
    CHECK_SYMBOL_EXISTS(${replacement} ${header} HAVE_${replacement})
    IF(HAVE_${replacement})
      SET(HAVE_${symbol_upper} 1)
      SET(${symbol} ${replacement})
    ENDIF()
  ENDIF()
ENDMACRO()

CHECK_SYMBOL_REPLACEMENT(S_IROTH _S_IREAD sys/stat.h)
CHECK_SYMBOL_REPLACEMENT(S_IFIFO _S_IFIFO sys/stat.h)
CHECK_SYMBOL_REPLACEMENT(SIGQUIT SIGTERM signal.h)
CHECK_SYMBOL_REPLACEMENT(SIGPIPE SIGINT signal.h)
CHECK_FUNCTION_REPLACEMENT(popen _popen)
CHECK_FUNCTION_REPLACEMENT(pclose _pclose)
CHECK_FUNCTION_REPLACEMENT(access _access)
CHECK_FUNCTION_REPLACEMENT(strcasecmp _stricmp)
CHECK_FUNCTION_REPLACEMENT(strncasecmp _strnicmp)
CHECK_SYMBOL_REPLACEMENT(snprintf _snprintf stdio.h)
CHECK_FUNCTION_REPLACEMENT(strtok_r strtok_s)
CHECK_FUNCTION_REPLACEMENT(strtoll _strtoi64)
CHECK_FUNCTION_REPLACEMENT(strtoull _strtoui64)
CHECK_FUNCTION_REPLACEMENT(vsnprintf _vsnprintf)
CHECK_TYPE_SIZE(ssize_t SIZE_OF_SSIZE_T)
IF(NOT HAVE_SIZE_OF_SSIZE_T)
 SET(ssize_t SSIZE_T)
ENDIF()

SET(FN_NO_CASE_SENSE 1)
SET(USE_SYMDIR 1)

# Force static C runtime for targets in current directory
# (useful to get rid of MFC dll's dependency, or in installer)
MACRO(FORCE_STATIC_CRT)
  FOREACH(flag
    CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO
    CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_DEBUG_INIT
    CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO
    CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG_INIT
    CMAKE_C_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_MINSIZEREL
  )
    STRING(REGEX REPLACE "/MD[d]?"  "/MT" "${flag}"  "${${flag}}" )
    STRING(REPLACE "${DYNAMIC_UCRT_LINKER_OPTION}" "" "${flag}" "${${flag}}")
  ENDFOREACH()
ENDMACRO()
