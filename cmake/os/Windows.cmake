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

ADD_DEFINITIONS(-D_WINDOWS -D__WIN__ -D_CRT_SECURE_NO_DEPRECATE)
ADD_DEFINITIONS(-D_WIN32_WINNT=0x0A00)
# We do not want the windows.h macros min/max
ADD_DEFINITIONS(-DNOMINMAX)
# Speed up build process excluding unused header files
ADD_DEFINITIONS(-DWIN32_LEAN_AND_MEAN)
  
# Adjust compiler and linker flags
IF(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 4)
   # mininal architecture flags, i486 enables GCC atomics
  ADD_DEFINITIONS(-march=i486)
ENDIF()

IF(MSVC)
  # Disable mingw based pkg-config found in Strawberry perl
  SET(PKG_CONFIG_EXECUTABLE 0 CACHE INTERNAL "")
  SET(MSVC_CRT_TYPE /MT CACHE STRING
    "Runtime library - specify runtime library for linking (/MT,/MTd,/MD,/MDd)"
  )
  SET(VALID_CRT_TYPES /MTd /MDd /MD  /MT)
  IF (NOT ";${VALID_CRT_TYPES};" MATCHES ";${MSVC_CRT_TYPE};")
    MESSAGE(FATAL_ERROR "Invalid value ${MSVC_CRT_TYPE} for MSVC_CRT_TYPE, choose one of /MT,/MTd,/MD,/MDd ")
  ENDIF()

  IF(MSVC_CRT_TYPE MATCHES "/MD")
   # Dynamic runtime (DLLs), need to install CRT libraries.
   SET(CMAKE_INSTALL_MFC_LIBRARIES TRUE)# upgrade wizard
   SET(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT VCCRT)
   SET(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
   SET(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
   IF(MSVC_CRT_TYPE STREQUAL "/MDd")
     SET (CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
   ENDIF()
   INCLUDE(InstallRequiredSystemLibraries)
  ENDIF()

  # Enable debug info also in Release build,
  # and create PDB to be able to analyze crashes.
  FOREACH(type EXE SHARED MODULE)
   SET(CMAKE_${type}_LINKER_FLAGS_RELEASE
     "${CMAKE_${type}_LINKER_FLAGS_RELEASE} /debug")
   SET(CMAKE_${type}_LINKER_FLAGS_MINSIZEREL
     "${CMAKE_${type}_LINKER_FLAGS_MINSIZEREL} /debug")
  ENDFOREACH()
  
  # Force static runtime libraries
  # - Choose debugging information:
  #     /Z7
  #     Produces an .obj file containing full symbolic debugging
  #     information for use with the debugger. The symbolic debugging
  #     information includes the names and types of variables, as well as
  #     functions and line numbers. No .pdb file is produced by the compiler.
  #
  # - Remove preprocessor flag _DEBUG that older cmakes use with Config=Debug,
  #   it is as defined by Debug runtimes itself (/MTd /MDd)

  FOREACH(lang C CXX)
    SET(CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE} /Z7")
  ENDFOREACH()
  FOREACH(flag 
   CMAKE_C_FLAGS_RELEASE    CMAKE_C_FLAGS_RELWITHDEBINFO 
   CMAKE_C_FLAGS_DEBUG      CMAKE_C_FLAGS_DEBUG_INIT 
   CMAKE_CXX_FLAGS_RELEASE  CMAKE_CXX_FLAGS_RELWITHDEBINFO
   CMAKE_CXX_FLAGS_DEBUG    CMAKE_CXX_FLAGS_DEBUG_INIT
   CMAKE_C_FLAGS_MINSIZEREL  CMAKE_CXX_FLAGS_MINSIZEREL
   )
   STRING(REGEX REPLACE "/M[TD][d]?"  "${MSVC_CRT_TYPE}" "${flag}"  "${${flag}}" )
   STRING(REGEX REPLACE "/D[ ]?_DEBUG"  "" "${flag}" "${${flag}}")
   STRING(REPLACE "/Zi"  "/Z7" "${flag}" "${${flag}}")
   IF(NOT "${${flag}}" MATCHES "/Z7")
    STRING(APPEND ${flag} " /Z7")
   ENDIF()
  ENDFOREACH()
  
 
  # Fix CMake's predefined huge stack size
  FOREACH(type EXE SHARED MODULE)
   STRING(REGEX REPLACE "/STACK:([^ ]+)" "" CMAKE_${type}_LINKER_FLAGS "${CMAKE_${type}_LINKER_FLAGS}")
   STRING(REGEX REPLACE "/INCREMENTAL:([^ ]+)" "/INCREMENTAL:NO" CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO}")
   STRING(REGEX REPLACE "/INCREMENTAL$" "/INCREMENTAL:NO" CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO}")
   STRING(REGEX REPLACE "/INCREMENTAL:([^ ]+)" "/INCREMENTAL:NO" CMAKE_${type}_LINKER_FLAGS_DEBUG "${CMAKE_${type}_LINKER_FLAGS_DEBUG}")
   STRING(REGEX REPLACE "/INCREMENTAL$" "/INCREMENTAL:NO" CMAKE_${type}_LINKER_FLAGS_DEBUG "${CMAKE_${type}_LINKER_FLAGS_DEBUG}")
   SET(CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_${type}_LINKER_FLAGS_RELWITHDEBINFO} /OPT:REF /release")
  ENDFOREACH()

  
  # Mark 32 bit executables large address aware so they can 
  # use > 2GB address space
  IF(CMAKE_SIZEOF_VOID_P MATCHES 4)
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LARGEADDRESSAWARE")
  ENDIF()
  
  # Speed up multiprocessor build
  IF (MSVC_VERSION GREATER 1400)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /MP")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
  ENDIF()
  
  #TODO: update the code and remove the disabled warnings
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4805 /wd4996 /we4700 /we4311 /we4477 /we4302 /we4090")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4805 /wd4291 /wd4996 /we4099 /we4700 /we4311 /we4477 /we4302 /we4090")
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Temporarily disable size_t warnings, due to their amount
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4267")
  ENDIF()
  IF(MYSQL_MAINTAINER_MODE MATCHES "ERR")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /WX")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /WX")
    FOREACH(type EXE SHARED MODULE)
      FOREACH(cfg RELEASE DEBUG RELWITHDEBINFO)
        SET(CMAKE_${type}_LINKER_FLAGS_${cfg} "${CMAKE_${type}_LINKER_FLAGS_${cfg}} /WX")
      ENDFOREACH()
    ENDFOREACH()
  ENDIF()
  IF(MSVC_VERSION LESS 1910)
    # Noisy warning C4800: 'type': forcing value to bool 'true' or 'false' (performance warning),
    # removed in VS2017
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4800")
  ELSE()
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /d2OptimizeHugeFunctions")
  ENDIF()
ENDIF()

# Always link with socket library
STRING(APPEND CMAKE_C_STANDARD_LIBRARIES " ws2_32.lib")
STRING(APPEND CMAKE_CXX_STANDARD_LIBRARIES " ws2_32.lib")

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
CHECK_SYMBOL_REPLACEMENT(finite _finite "math;float.h")
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
