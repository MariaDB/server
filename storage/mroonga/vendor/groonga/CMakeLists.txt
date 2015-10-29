# Copyright(C) 2012-2015 Brazil
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License version 2.1 as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

# https://buildbot.askmonty.org/buildbot/builders/work-amd64-valgrind/builds/5263/steps/compile/logs/stdio
# says CMake 2.6.2... We want to drop old software support...
cmake_minimum_required(VERSION 2.6.2)
# cmake_minimum_required(VERSION 2.6.4) # CentOS 5
set(GRN_PROJECT_NAME "groonga")
project("${GRN_PROJECT_NAME}")

if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(GRN_BUNDLED FALSE)
else()
  set(GRN_BUNDLED TRUE)
endif()

if(MSVC)
  if(MSVC_VERSION LESS 1800)
    set(GRN_OLD_MSVC_MESSAGE "Groonga supports only MSVC 2013 or later")
    if(GRN_BUNDLED)
      message(STATUS ${GRN_OLD_MSVC_MESSAGE})
      return()
    else()
      message(FATAL_ERROR ${GRN_OLD_MSVC_MESSAGE})
    endif()
  endif()
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_COMPILER_IS_CLANGC ON)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_COMPILER_IS_CLANGCXX ON)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/base_version" VERSION)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/version.sh")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.sh" GRN_VERSION)
else()
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/version.sh")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git" AND EXISTS "/bin/sh")
      execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/version-gen.sh")
      file(READ "${CMAKE_CURRENT_BINARY_DIR}/version.sh" GRN_VERSION)
    else()
      set(GRN_VERSION "${VERSION}")
    endif()
  endif()
endif()
string(REGEX REPLACE "(^.*=|\n)" "" GRN_VERSION "${GRN_VERSION}")

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckLibraryExists)
include(CheckSymbolExists)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(FindPkgConfig)
include(${CMAKE_CURRENT_SOURCE_DIR}/build/cmake_modules/ReadFileList.cmake)

if(DEFINED GRN_EMBED)
  set(GRN_EMBED_DEFAULT ${GRN_EMBED})
else()
  set(GRN_EMBED_DEFAULT OFF)
endif()
option(GRN_EMBED
  "Build as a static library to embed into an application"
  ${GRN_EMBED_DEFAULT})

set(BIN_DIR "bin")
set(SBIN_DIR "sbin")
set(LIB_DIR "lib")
set(INCLUDE_DIR "include")
set(GRN_INCLUDE_DIR "include/groonga")
set(DATA_DIR "share")
set(GRN_DATA_DIR "${DATA_DIR}/${GRN_PROJECT_NAME}")
set(CONFIG_DIR "etc")
set(GRN_CONFIG_DIR "${CONFIG_DIR}/${GRN_PROJECT_NAME}")
set(GRN_CONFIG_PATH "${CMAKE_INSTALL_PREFIX}/${GRN_CONFIG_DIR}/groonga.conf")

set(GRN_LOG_PATH
  "${CMAKE_INSTALL_PREFIX}/var/log/${GRN_PROJECT_NAME}/${GRN_PROJECT_NAME}.log"
  CACHE FILEPATH "log file path")
set(GRN_DEFAULT_ENCODING
  "utf8"
  CACHE STRING "Groonga's default encoding")
set(GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD
  0
  CACHE STRING "groonga default match escalation threshold")
set(GRN_DEFAULT_DOCUMENT_ROOT_BASE
  "html/admin"
  CACHE PATH "groonga default document root base path")
set(GRN_DEFAULT_RELATIVE_DOCUMENT_ROOT
  "share/${GRN_PROJECT_NAME}/${GRN_DEFAULT_DOCUMENT_ROOT_BASE}"
  CACHE PATH "groonga default relative document root")
set(GRN_DEFAULT_DOCUMENT_ROOT
  "${CMAKE_INSTALL_PREFIX}/${GRN_DATA_DIR}/${GRN_DEFAULT_DOCUMENT_ROOT_BASE}"
  CACHE PATH "groonga default document root")
set(GRN_DEFAULT_DB_KEY
  "auto"
  CACHE STRING "Groonga's default DB key management algorithm")
set(GRN_STACK_SIZE
  1024
  CACHE STRING
  "DANGER!!! groonga stack size. Normarlly, you should not change this variable.")
set(GRN_LOCK_TIMEOUT
  10000000
  CACHE STRING
  "timeout to acquire a lock.")
set(GRN_LOCK_WAIT_TIME_NANOSECOND
  1000000
  CACHE STRING
  "wait time in nanosecond to acquire a lock.")
set(GRN_RELATIVE_PLUGINS_DIR
  "${LIB_DIR}/${GRN_PROJECT_NAME}/plugins")
set(GRN_PLUGINS_DIR
  "${CMAKE_INSTALL_PREFIX}/${GRN_RELATIVE_PLUGINS_DIR}")
set(GRN_PLUGIN_SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}")
set(GRN_DLL_FILENAME
  "${CMAKE_SHARED_LIBRARY_PREFIX}groonga${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(GRN_QUERY_EXPANDER_TSV_RELATIVE_SYNONYMS_FILE "synonyms.tsv")
set(GRN_QUERY_EXPANDER_TSV_SYNONYMS_FILE
  "${CMAKE_INSTALL_PREFIX}/${GRN_DATA_DIR}/${GRN_QUERY_EXPANDER_TSV_RELATIVE_SYNONYMS_FILE}")
set(GRN_RELATIVE_RUBY_SCRIPTS_DIR
  "${LIB_DIR}/${GRN_PROJECT_NAME}/scripts/ruby")
set(GRN_RUBY_SCRIPTS_DIR
  "${CMAKE_INSTALL_PREFIX}/${GRN_RELATIVE_RUBY_SCRIPTS_DIR}")


if(CMAKE_COMPILER_IS_GNUCC)
  set(GRN_C_COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS} -std=gnu99")
endif()


if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANGCXX)
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wall")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wextra")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-unused-but-set-variable")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-unused-parameter")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-sign-compare")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-pointer-sign")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-missing-field-initializers")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wformat=2")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wstrict-aliasing=2")
  MY_CHECK_AND_SET_COMPILER_FLAG("-fno-strict-aliasing")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wdisabled-optimization")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wfloat-equal")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wpointer-arith")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wdeclaration-after-statement")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wbad-function-cast")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wcast-align")
  #MY_CHECK_AND_SET_COMPILER_FLAG("-Wredundant-decls")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wwrite-strings")
  MY_CHECK_AND_SET_COMPILER_FLAG("-fexceptions")
  MY_CHECK_AND_SET_COMPILER_FLAG("-fimplicit-templates")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-clobbered")
endif()

if(NOT DEFINED CMAKE_C_COMPILE_OPTIONS_PIC)
  # For old CMake
  if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANGCXX)
    MY_CHECK_AND_SET_COMPILER_FLAG("-fPIC")
  endif()
endif()

option(GRN_WITH_DEBUG "enable debug build." OFF)
if(GRN_WITH_DEBUG)
  if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANGCXX)
    set(GRN_C_COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS} -g3 -O0")
    set(GRN_CXX_COMPILE_FLAGS "${GRN_CXX_COMPILE_FLAGS} -g3 -O0")
  endif()
endif()

add_definitions(
  -DHAVE_CONFIG_H
  )
if(GRN_EMBED)
  add_definitions(-DGRN_EMBEDDED)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANGCXX)
  set(_GNU_SOURCE TRUE)
endif()

include_directories(
  BEFORE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/lib
  )

macro(ac_check_headers header)
  string(REGEX REPLACE "[/.]" "_" output_variable_name ${header})
  string(TOUPPER "${output_variable_name}" output_variable_name)
  set(output_variable_name "HAVE_${output_variable_name}")
  check_include_file(${header} ${output_variable_name})
endmacro()

macro(ac_check_funcs function)
  string(TOUPPER "${function}" output_variable_name)
  set(output_variable_name "HAVE_${output_variable_name}")
  check_function_exists(${function} ${output_variable_name})
endmacro()

macro(ac_check_symbols symbol files)
  string(TOUPPER "${symbol}" output_variable_name)
  set(output_variable_name "HAVE_${output_variable_name}")
  check_symbol_exists(${symbol} ${files} ${output_variable_name})
endmacro()

macro(ac_check_lib library function)
  string(REGEX REPLACE "[/.]" "_" output_variable_base_name ${library})
  string(TOUPPER "${output_variable_base_name}" output_variable_base_name)
  set(output_variable_name "HAVE_LIB${output_variable_base_name}")
  set(location "${ARG2}")
  check_library_exists(${library} ${function} "${location}"
    ${output_variable_name})
  if(${output_variable_name})
    set(${output_variable_base_name}_LIBS "${library}")
  endif()
endmacro()

include(build/ac_macros/check_headers.m4)
include(build/ac_macros/check_functions.m4)

ac_check_symbols(fpclassify math.h)
ac_check_lib(m fpclassify)

ac_check_lib(dl dlopen)
ac_check_lib(execinfo backtrace)
if(HAVE_LIBEXECINFO)
  set(HAVE_BACKTRACE TRUE)
else()
  ac_check_funcs(backtrace)
endif()
ac_check_lib(rt clock_gettime)
if(HAVE_LIBRT)
  set(HAVE_CLOCK_GETTIME TRUE)
endif()
if(GRN_EMBED)
  check_library_exists(stdc++ __cxa_begin_catch "${ARG2}"
    STDCPP)
  if(STDCPP)
    set(STDCPP_LIBS "stdc++")
  endif()
endif()

if(UNIX)
  ac_check_headers(pthread.h)
  ac_check_lib(pthread pthread_mutex_init)
  if(NOT ${HAVE_LIBPTHREAD})
    message(FATAL_ERROR "No libpthread found")
  endif()
  ac_check_funcs(pthread_mutexattr_setpshared)
  ac_check_funcs(pthread_condattr_setpshared)
endif()

option(GRN_WITH_NFKC "use NFKC based UTF8 normalization." ON)

if(WIN32)
  ac_check_headers(winsock2.h)
  if(NOT ${HAVE_WINSOCK2_H})
    message(FATAL_ERROR "No winsock2.h found")
  endif()

  # FIXME: CMake couldn't detect ws2_32.lib on Windows 8 64bit.
  # It may be caused by missing library search path for ws2_32.lib.
  # It seems that Visual Studio (devenv.exe) can find it but link.exe
  # can't. "cmake --build" can find it because it uses Visual Studio
  # internally. So we assume that we always have ws2_32.lib on Windows.
  # ac_check_lib(ws2_32 select)
  set(HAVE_LIBWS2_32 TRUE)
  set(WS2_32_LIBS "ws2_32.lib")

  set(USE_SELECT TRUE)
else()
  ac_check_headers(sys/epoll.h)
  if(${HAVE_SYS_EPOLL_H})
    ac_check_funcs(epoll_create)
    if(${HAVE_EPOLL_CREATE})
      set(USE_EPOLL TRUE)
    endif()
  endif()

  if(NOT USE_EPOLL)
    ac_check_headers(sys/event.h)
    if(${HAVE_SYS_EVENT_H})
      ac_check_funcs(kevent)
      if(${HAVE_KEVENT})
        set(USE_KQUEUE TRUE)
      endif()
    endif()

    if(NOT USE_KQUEUE)
      ac_check_headers(sys/poll.h)
      if(${HAVE_SYS_POLL_H})
        ac_check_funcs(poll)
        if(${HAVE_POLL})
          set(USE_POLL TRUE)
        endif()
      endif()

      if(NOT USE_POLL)
        ac_check_funcs(select)
        if(${HAVE_SELECT})
          set(USE_SELECT TRUE)
          ac_check_headers(sys/select.h)
        endif()

        if(NOT USE_SELECT)
          message(FATAL_ERROR "All epoll/kqueue/poll/select are missing")
        endif()
      endif()
    endif()
  endif()
endif()

set(GRN_WITH_ZLIB "auto"
  CACHE STRING "Support data compression by zlib.")
if(NOT ${GRN_WITH_ZLIB} STREQUAL "no")
  ac_check_lib(z compress)
  if(NOT HAVE_LIBZ)
    if(${GRN_WITH_ZLIB} STREQUAL "yes")
      message(FATAL_ERROR "No libz found")
    endif()
    set(GRN_WITH_ZLIB "no")
  endif()
endif()

set(GRN_WITH_LZ4 "auto"
  CACHE STRING "Support data compression by LZ4.")
if(NOT ${GRN_WITH_LZ4} STREQUAL "no")
  if(NOT DEFINED LIBLZ4_FOUND)
    pkg_check_modules(LIBLZ4 liblz4)
  endif()
  if(LIBLZ4_FOUND)
    set(GRN_WITH_LZ4 TRUE)
  else()
    if(${GRN_WITH_LZ4} STREQUAL "yes")
      message(FATAL_ERROR "No LZ4 found")
    endif()
    set(GRN_WITH_LZ4 FALSE)
  endif()
endif()

set(GRN_WITH_MECAB "auto"
  CACHE STRING "use MeCab for morphological analysis")
if(NOT ${GRN_WITH_MECAB} STREQUAL "no")
  set(GRN_MECAB_CONFIG "mecab-config" CACHE FILEPATH "mecab-config path")
  if(NOT CMAKE_CROSSCOMPILING)
    find_program(GRN_MECAB_CONFIG_ABSOLUTE_PATH "${GRN_MECAB_CONFIG}")
  endif()
  if(EXISTS "${GRN_MECAB_CONFIG_ABSOLUTE_PATH}")
    execute_process(COMMAND "${GRN_MECAB_CONFIG_ABSOLUTE_PATH}" --inc-dir
      OUTPUT_VARIABLE MECAB_INCLUDE_DIRS
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${GRN_MECAB_CONFIG_ABSOLUTE_PATH}" --libs-only-L
      OUTPUT_VARIABLE MECAB_LIBRARY_DIRS
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(MECAB_LIBRARIES "mecab")
    ac_check_lib(${MECAB_LIBRARIES} mecab_new)
    if(HAVE_LIBMECAB)
      set(GRN_WITH_MECAB TRUE)
    else()
      if(${GRN_WITH_MECAB} STREQUAL "yes")
	message(FATAL_ERROR
	  "No MeCab library found: "
	  "include directories: <${MECAB_INCLUDE_DIRS}>, "
	  "library directories: <${MECAB_LIBRARY_DIRS}>")
      endif()
      set(GRN_WITH_MECAB FALSE)
    endif()
  else()
    if(${GRN_WITH_MECAB} STREQUAL "yes")
      message(FATAL_ERROR "No mecab-config found: <${GRN_MECAB_CONFIG}>")
    endif()
    set(GRN_WITH_MECAB FALSE)
  endif()
else()
  set(GRN_WITH_MECAB FALSE)
endif()

set(GRN_WITH_KYTEA "auto"
  CACHE STRING "use KyTea for morphological analysis")
if(NOT ${GRN_WITH_KYTEA} STREQUAL "no")
  if(NOT DEFINED KYTEA_FOUND)
    pkg_check_modules(KYTEA kytea)
  endif()
  if(KYTEA_FOUND)
    set(GRN_WITH_KYTEA TRUE)
  else()
    if(${GRN_WITH_KYTEA} STREQUAL "yes")
      message(FATAL_ERROR "No KyTea found")
    endif()
    set(GRN_WITH_KYTEA FALSE)
  endif()
else()
  set(GRN_WITH_KYTEA FALSE)
endif()

set(GRN_WITH_LIBSTEMMER "auto"
  CACHE STRING "use libstemmer for stemming token filter")
if(NOT ${GRN_WITH_LIBSTEMMER} STREQUAL "no")
  if(NOT ("${GRN_WITH_LIBSTEMMER}" STREQUAL "yes" OR
	"${GRN_WITH_LIBSTEMMER}" STREQUAL "auto"))
    if("${LIBSTEMMER_INCLUDE_DIRS}" STREQUAL "")
      set(LIBSTEMMER_INCLUDE_DIRS "${GRN_WITH_LIBSTEMMER}/include")
    endif()
    if("${LIBSTEMMER_LIBRARY_DIRS}" STREQUAL "")
      set(LIBSTEMMER_LIBRARY_DIRS "${GRN_WITH_LIBSTEMMER}/lib")
    endif()
  endif()
  set(CMAKE_REQUIRED_INCLUDES_SAVE ${CMAKE_REQUIRED_INCLUDES})
  set(CMAKE_REQUIRED_INCLUDES
    ${CMAKE_REQUIRED_INCLUDES}
    ${LIBSTEMMER_INCLUDE_DIRS})
  ac_check_headers(libstemmer.h)
  ac_check_lib(stemmer sb_stemmer_list "${LIBSTEMMER_LIBRARY_DIRS}")
  set(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES_SAVE})
  if(HAVE_LIBSTEMMER_H AND HAVE_LIBSTEMMER)
    set(LIBSTEMMER_LIBRARIES "stemmer")
    set(GRN_WITH_LIBSTEMMER TRUE)
  else()
    if(${GRN_WITH_LIBSTEMMER} STREQUAL "yes")
      message(FATAL_ERROR "No libstemmer found")
    endif()
    set(GRN_WITH_LIBSTEMMER FALSE)
  endif()
else()
  set(GRN_WITH_LIBSTEMMER FALSE)
endif()

set(GRN_WITH_ZEROMQ "auto"
  CACHE STRING "use ZeroMQ for suggestion")
if(NOT ${GRN_WITH_ZEROMQ} STREQUAL "no")
  if(NOT DEFINED ZEROMQ_FOUND)
    pkg_check_modules(ZEROMQ libzmq)
  endif()
  if(ZEROMQ_FOUND)
    set(GRN_WITH_ZEROMQ TRUE)
  else()
    if(${GRN_WITH_ZEROMQ} STREQUAL "yes")
      message(FATAL_ERROR "No ZeroMQ found")
    endif()
    set(GRN_WITH_ZEROMQ FALSE)
  endif()
else()
  set(GRN_WITH_ZEROMQ FALSE)
endif()

set(GRN_WITH_LIBEVENT "auto"
  CACHE STRING "use libevent for suggestion")
if(NOT ${GRN_WITH_LIBEVENT} STREQUAL "no")
  if("${GRN_WITH_LIBEVENT}" STREQUAL "yes" OR
      "${GRN_WITH_LIBEVENT}" STREQUAL "auto")
    set(LIBEVENT_INCLUDE_DIRS "")
    set(LIBEVENT_LIBRARY_DIRS "")
  else()
    set(LIBEVENT_INCLUDE_DIRS "${GRN_WITH_LIBEVENT}/include")
    set(LIBEVENT_LIBRARY_DIRS "${GRN_WITH_LIBEVENT}/lib")
  endif()
  set(CMAKE_REQUIRED_INCLUDES_SAVE ${CMAKE_REQUIRED_INCLUDES})
  ac_check_lib(event event_init "${LIBEVENT_LIBRARY_DIRS}")
  set(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES_SAVE})
  if(HAVE_LIBEVENT)
    set(LIBEVENT_LIBRARIES "event")
    set(GRN_WITH_LIBEVENT TRUE)
  else()
    if(${GRN_WITH_LIBEVENT} STREQUAL "yes")
      message(FATAL_ERROR "No libevent found")
    endif()
    set(GRN_WITH_LIBEVENT FALSE)
  endif()
else()
  set(GRN_WITH_LIBEVENT FALSE)
endif()

set(GRN_WITH_MESSAGE_PACK "auto"
  CACHE STRING "use MessagePack for suggestion")
if(NOT ${GRN_WITH_MESSAGE_PACK} STREQUAL "no")
  if(NOT DEFINED MESSAGE_PACK_FOUND)
    pkg_check_modules(MESSAGE_PACK msgpack)
  endif()
  if(MESSAGE_PACK_FOUND)
    set(GRN_WITH_MESSAGE_PACK TRUE)
  else()
    if("${GRN_WITH_MESSAGE_PACK}" STREQUAL "yes" OR
	"${GRN_WITH_MESSAGE_PACK}" STREQUAL "auto")
      set(MESSAGE_PACK_INCLUDE_DIRS "")
      set(MESSAGE_PACK_LIBRARY_DIRS "")
    else()
      set(MESSAGE_PACK_INCLUDE_DIRS "${GRN_WITH_MESSAGE_PACK}/include")
      set(MESSAGE_PACK_LIBRARY_DIRS "${GRN_WITH_MESSAGE_PACK}/lib")
    endif()
    set(CMAKE_REQUIRED_INCLUDES_SAVE ${CMAKE_REQUIRED_INCLUDES})
    ac_check_lib(msgpack msgpack_version "${MESSAGE_PACK_LIBRARY_DIRS}")
    set(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES_SAVE})
    if(HAVE_LIBMSGPACK)
      set(MESSAGE_PACK_LIBRARIES "msgpack")
      set(GRN_WITH_MESSAGE_PACK TRUE)
    else()
      if(${GRN_WITH_MESSAGE_PACK} STREQUAL "yes")
	message(FATAL_ERROR "No MessagePack found")
      endif()
      set(GRN_WITH_MESSAGE_PACK FALSE)
    endif()
  endif()
else()
  set(GRN_WITH_MESSAGE_PACK FALSE)
endif()

find_program(RUBY NAMES "ruby2.1" "ruby21" "ruby")

option(GRN_WITH_MRUBY "use mruby" OFF)
if(GRN_WITH_MRUBY)
  set(MRUBY_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mruby-source/include")
  set(MRUBY_LIBS mruby)
else()
  set(MRUBY_INCLUDE_DIRS "")
  set(MRUBY_LIBS "")
endif()

# TODO: Support using system Onigmo instead of bundled Onigmo.
# set(GRN_WITH_ONIGMO ON)
# set(ONIGMO_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/onigmo-source")
# set(ONIGMO_LIBS onigmo)

# TODO: It's for Onigmo static link case. If we support system Onigmo,
# we need to remove it and add -DEXPORT to Onigmo build.
add_definitions(-DONIG_EXTERN=extern)

add_subdirectory(vendor)
if(GRN_EMBED)
  add_subdirectory(plugins)
endif()
add_subdirectory(lib)
if(NOT GRN_EMBED)
  add_subdirectory(src)
  add_subdirectory(plugins)
  add_subdirectory(include)
  add_subdirectory(data)
endif()

configure_file(config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)

set(GROONGA "${CMAKE_CURRENT_BINARY_DIR}/src/groonga")
set(GROONGA_SUGGEST_CREATE_DATASET
  "${CMAKE_CURRENT_BINARY_DIR}/src/suggest/groonga-suggest-create-dataset")
set(GROONGA_BENCHMARK "${CMAKE_CURRENT_BINARY_DIR}/src/groonga-benchmark")
configure_file(config.sh.in "${CMAKE_CURRENT_BINARY_DIR}/config.sh" @ONLY)

set(prefix "${CMAKE_INSTALL_PREFIX}")
set(exec_prefix "${prefix}")
set(bindir "${CMAKE_INSTALL_PREFIX}/${BIN_DIR}")
set(sbindir "${CMAKE_INSTALL_PREFIX}/${SBIN_DIR}")
set(libdir "${CMAKE_INSTALL_PREFIX}/${LIB_DIR}")
set(includedir "${CMAKE_INSTALL_PREFIX}/${INCLUDE_DIR}")
set(datarootdir "${CMAKE_INSTALL_PREFIX}/${DATA_DIR}")
set(datadir "${datarootdir}")
set(expanded_pluginsdir "${GRN_PLUGINS_DIR}")
set(GRN_EXPANDED_DEFAULT_DOCUMENT_ROOT "${GRN_DEFAULT_DOCUMENT_ROOT}")
set(EXEEXT "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(groonga.pc.in "${CMAKE_CURRENT_BINARY_DIR}/groonga.pc" @ONLY)

if(NOT GRN_EMBED)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/groonga.pc"
    DESTINATION "${LIB_DIR}/pkgconfig/")
endif()

add_subdirectory(vendor/plugins)
