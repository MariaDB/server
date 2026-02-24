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


if(MSVC)
  if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL ARM64)
   set(MSVC_ARM64 1)
   set(MSVC_INTEL 0)
  else()
   set(MSVC_INTEL 1)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL Clang)
   set(CLANG_CL TRUE)
  endif()
endif()

# avoid running system checks by using pre-cached check results
# system checks are expensive on VS since every tiny program is to be compiled in 
# a VC solution.
get_filename_component(_SCRIPT_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)
include(${_SCRIPT_DIR}/WindowsCache.cmake)

# OS display name (version_compile_os etc).
# Used by the test suite to ignore bugs on some platforms
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(SYSTEM_TYPE "Win64")
else()
  set(SYSTEM_TYPE "Win32")
endif()

function(find_asan_runtime result_list)
  set(${result_list} "" PARENT_SCOPE)
  if(CMAKE_C_COMPILER_VERSION)
    set(CLANG_VERSION "${CMAKE_C_COMPILER_VERSION}")
  else()
    return()
  endif()

  get_filename_component(CLANG_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(LLVM_ROOT "${CLANG_BIN_DIR}" DIRECTORY)

  # Determine target architecture
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" --version
    OUTPUT_VARIABLE CLANG_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(CLANG_VERSION_OUTPUT MATCHES "x86_64")
    set(ARCH_SUFFIX "x86_64")
  elseif(CLANG_VERSION_OUTPUT MATCHES "i686|i386")
    set(ARCH_SUFFIX "i386")
  elseif(CLANG_VERSION_OUTPUT MATCHES "aarch64")
    set(ARCH_SUFFIX "aarch64")
  else()
    message(FATAL_ERROR "unknown arch")
  endif()

  string(REGEX MATCH "^[0-9]+" CLANG_MAJOR_VERSION "${CMAKE_C_COMPILER_VERSION}")
  set(CLANG_VERSION_DIR "${LLVM_ROOT}/lib/clang/${CLANG_MAJOR_VERSION}")

  set(out)
  foreach(name clang_rt.asan_dynamic-${ARCH_SUFFIX}.lib
               clang_rt.asan_dynamic_runtime_thunk-${ARCH_SUFFIX}.lib)
    set(path "${CLANG_VERSION_DIR}/lib/windows/${name}")
    if(EXISTS "${path}")
      list(APPEND out ${path})
    else()
      message(FATAL_ERROR "expected library ${path} not found")
    ENDIF()
  endforeach()
  set(${result_list} ${out} PARENT_SCOPE)
endfunction()

macro(enable_sanitizers)
  # Remove the runtime checks from the compiler flags
  # ASAN does the same thing, in many cases better
  foreach(lang C CXX)
    foreach(suffix "_DEBUG" "_DEBUG_INIT")
      string(REGEX REPLACE "/RTC[1su]" "" CMAKE_${lang}_FLAGS${suffix} "${CMAKE_${lang}_FLAGS${suffix}}")
    endforeach()
  endforeach()

  if(WITH_ASAN)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>)
  endif()
  if(WITH_UBSAN)
    include(CheckCCompilerFlag)
    check_c_compiler_flag(/fsanitize=undefined HAVE_fsanitize_undefined)
    if (HAVE_fsanitize_undefined)
      add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=undefined>)
    else()
      message(FATAL_ERROR "UBSAN not supported by this compiler yet")
    endif()
  endif()
  if(CLANG_CL)
    find_asan_runtime(asan_libs)
    foreach(lib ${asan_libs})
      link_libraries(${lib})
      string(APPEND CMAKE_C_STANDARD_LIBRARIES " \"${lib}\"")
      string(APPEND CMAKE_CXX_STANDARD_LIBRARIES " \"${lib}\"")
    endforeach()
  else()
    add_link_options(/INCREMENTAL:NO)
  endif()
endmacro()


if(MSVC)
  # Disable mingw based pkg-config found in Strawberry perl
  set(PKG_CONFIG_EXECUTABLE 0 CACHE INTERNAL "")

  if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
  endif()

  if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    # Dynamic runtime (DLLs), need to install CRT libraries.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT VCCRT)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
    if(CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDebugDLL")
      set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
    endif()
    include(InstallRequiredSystemLibraries)
  endif()

  # Compile with /Zi to get debugging information
  if (NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
    add_link_options(/DEBUG) # Ensure debugging info at link time
  endif()

  if(WITH_ASAN OR WITH_UBSAN)
    # Workaround something Linux specific
    set(SECURITY_HARDENED 0 CACHE INTERNAL "" FORCE)
    enable_sanitizers()
  endif()

  add_compile_definitions(
    _CRT_SECURE_NO_DEPRECATE
    _CRT_NONSTDC_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    # We do not want the windows.h , or winsvc.h macros min/max
    NOMINMAX NOSERVICE
    # Speed up build process excluding unused header files
    WIN32_LEAN_AND_MEAN
  )
  if(CLANG_CL)
    add_compile_options(
      -Wno-unknown-warning-option
      -Wno-unused-private-field
      -Wno-unused-parameter
      -Wno-inconsistent-missing-override
      -Wno-unused-command-line-argument
      -Wno-pointer-sign
      -Wno-deprecated-register
      -Wno-missing-braces
      -Wno-unused-function
      -Wno-unused-local-typedef
      -Wno-microsoft-static-assert
      -Wno-c++17-extensions
      -msse4.2
    )
    if((CMAKE_SIZEOF_VOID_P MATCHES 8) AND MSVC_INTEL)
      add_compile_options(-mpclmul)
    endif()
  endif()

  # Mark 32 bit executables large address aware so they can 
  # use > 2GB address space
  if(CMAKE_SIZEOF_VOID_P MATCHES 4)
    add_link_options(/LARGEADDRESSAWARE)
  endif()

  # RelWithDebInfo is deoptimized wrt inlining.
  # Fix it to default
  foreach(lang C CXX)
    foreach(suffix "_RELWITHDEBINFO" "_RELWITHDEBINFO_INIT")
      string(REGEX REPLACE "/Ob[0-1]" "" CMAKE_${lang}_FLAGS${suffix} "${CMAKE_${lang}_FLAGS${suffix}}")
    endforeach()
  endforeach()

  if(NOT CLANG_CL)
    add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:/INCREMENTAL:NO;/RELEASE;/OPT:REF,ICF>")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:$<$<CONFIG:Release,RelWithDebInfo>:/Gw>>)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP>)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/we4099;/we4700;/we4311;/we4477;/we4302;/we4090>")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/diagnostics:caret>)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
    if(NOT FAST_BUILD)
      add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:$<$<COMPILE_LANGUAGE:C,CXX>:/d2OptimizeHugeFunctions>>)
    endif()
  endif()

  if(MYSQL_MAINTAINER_MODE MATCHES "ERR")
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
    add_link_options(/WX)
  endif()
endif()

# avoid running system checks by using pre-cached check results
# system checks are expensive on VS generator
get_filename_component(_SCRIPT_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)
include(${_SCRIPT_DIR}/WindowsCache.cmake)

# this is out of place, not really a system check
set(FN_NO_CASE_SENSE 1)
set(USE_SYMDIR 1)
set(HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT 1)

