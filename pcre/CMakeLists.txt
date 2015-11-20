# CMakeLists.txt
#
#
# This file allows building PCRE with the CMake configuration and build
# tool. Download CMake in source or binary form from http://www.cmake.org/
#
# Original listfile by Christian Ehrlicher <Ch.Ehrlicher@gmx.de>
# Refined and expanded by Daniel Richard G. <skunk@iSKUNK.ORG>
# 2007-09-14 mod by Sheri so 7.4 supported configuration options can be entered
# 2007-09-19 Adjusted by PH to retain previous default settings
# 2007-12-26 (a) On UNIX, use names libpcre instead of just pcre
#            (b) Ensure pcretest and pcregrep link with the local library,
#                not a previously-installed one.
#            (c) Add PCRE_SUPPORT_LIBREADLINE, PCRE_SUPPORT_LIBZ, and
#                PCRE_SUPPORT_LIBBZ2.
# 2008-01-20 Brought up to date to include several new features by Christian
#            Ehrlicher.
# 2008-01-22 Sheri added options for backward compatibility of library names
#            when building with minGW:
#            if "ON", NON_STANDARD_LIB_PREFIX causes shared libraries to
#            be built without "lib" as prefix. (The libraries will be named
#            pcre.dll, pcreposix.dll and pcrecpp.dll).
#            if "ON", NON_STANDARD_LIB_SUFFIX causes shared libraries to
#            be built with suffix of "-0.dll". (The libraries will be named
#            libpcre-0.dll, libpcreposix-0.dll and libpcrecpp-0.dll - same names
#            built by default with Configure and Make.
# 2008-01-23 PH removed the automatic build of pcredemo.
# 2008-04-22 PH modified READLINE support so it finds NCURSES when needed.
# 2008-07-03 PH updated for revised UCP property support (change of files)
# 2009-03-23 PH applied Steven Van Ingelgem's patch to change the name
#            CMAKE_BINARY_DIR to PROJECT_BINARY_DIR so that it works when PCRE
#            is included within another project.
# 2009-03-23 PH applied a modified version of Steven Van Ingelgem's patches to
#            add options to stop the building of pcregrep and the tests, and
#            to disable the final configuration report.
# 2009-04-11 PH applied Christian Ehrlicher's patch to show compiler flags that
#            are set by specifying a release type.
# 2010-01-02 PH added test for stdint.h
# 2010-03-02 PH added test for inttypes.h
# 2011-08-01 PH added PCREGREP_BUFSIZE
# 2011-08-22 PH added PCRE_SUPPORT_JIT
# 2011-09-06 PH modified WIN32 ADD_TEST line as suggested by Sergey Cherepanov
# 2011-09-06 PH added PCRE_SUPPORT_PCREGREP_JIT
# 2011-10-04 Sheri added support for including coff data in windows shared libraries
#            compiled with MINGW if pcre.rc and/or pcreposix.rc are placed in
#            the source dir by the user prior to building
# 2011-10-04 Sheri changed various add_test's to use exes' location built instead
#            of DEBUG location only (likely only matters in MSVC)
# 2011-10-04 Sheri added scripts to provide needed variables to RunTest and
#            RunGrepTest (used for UNIX and Msys)
# 2011-10-04 Sheri added scripts to provide needed variables and to execute
#            RunTest.bat in Win32 (for effortless testing with "make test")
# 2011-10-04 Sheri Increased minimum required cmake version
# 2012-01-06 PH removed pcre_info.c and added pcre_string_utils.c
# 2012-01-10 Zoltan Herczeg added libpcre16 support
# 2012-01-13 Stephen Kelly added out of source build support
# 2012-01-17 PH applied Stephen Kelly's patch to parse the version data out
#            of the configure.ac file
# 2012-02-26 PH added support for libedit
# 2012-09-06 PH added support for PCRE_EBCDIC_NL25
# 2012-09-08 ChPe added PCRE32 support
# 2012-10-23 PH added support for VALGRIND and GCOV
# 2012-12-08 PH added patch from Daniel Richard G to quash some MSVC warnings
# 2013-07-01 PH realized that the "support" for GCOV was a total nonsense and
#            so it has been removed.
# 2013-10-08 PH got rid of the "source" command, which is a bash-ism (use ".")
# 2013-11-05 PH added support for PARENS_NEST_LIMIT

PROJECT(PCRE C CXX)

SET(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake) # for FindReadline.cmake

# Enable Unicode properties
OPTION(PCRE_SUPPORT_UNICODE_PROPERTIES "Unicode properties" ON)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include)

# Configuration checks

INCLUDE(CheckIncludeFile)
INCLUDE(CheckIncludeFileCXX)
INCLUDE(CheckFunctionExists)
INCLUDE(CheckTypeSize)

CHECK_INCLUDE_FILE(dirent.h     HAVE_DIRENT_H)
CHECK_INCLUDE_FILE(stdint.h     HAVE_STDINT_H)
CHECK_INCLUDE_FILE(inttypes.h   HAVE_INTTYPES_H)
CHECK_INCLUDE_FILE(sys/stat.h   HAVE_SYS_STAT_H)
CHECK_INCLUDE_FILE(sys/types.h  HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILE(unistd.h     HAVE_UNISTD_H)
CHECK_INCLUDE_FILE(windows.h    HAVE_WINDOWS_H)

CHECK_INCLUDE_FILE_CXX(type_traits.h            HAVE_TYPE_TRAITS_H)
CHECK_INCLUDE_FILE_CXX(bits/type_traits.h       HAVE_BITS_TYPE_TRAITS_H)

CHECK_FUNCTION_EXISTS(bcopy     HAVE_BCOPY)
CHECK_FUNCTION_EXISTS(memmove   HAVE_MEMMOVE)
CHECK_FUNCTION_EXISTS(strerror  HAVE_STRERROR)
CHECK_FUNCTION_EXISTS(strtoll   HAVE_STRTOLL)
CHECK_FUNCTION_EXISTS(strtoq    HAVE_STRTOQ)
CHECK_FUNCTION_EXISTS(_strtoi64 HAVE__STRTOI64)

CHECK_TYPE_SIZE("long long"             LONG_LONG)
CHECK_TYPE_SIZE("unsigned long long"    UNSIGNED_LONG_LONG)

# User-configurable options
#
# (Note: CMakeSetup displays these in alphabetical order, regardless of
# the order we use here)

SET(PCRE_LINK_SIZE "2" CACHE STRING
    "Internal link size (2, 3 or 4 allowed). See LINK_SIZE in config.h.in for details.")

SET(PCRE_PARENS_NEST_LIMIT "250" CACHE STRING
    "Default nested parentheses limit. See PARENS_NEST_LIMIT in config.h.in for details.")

SET(PCRE_MATCH_LIMIT "10000000" CACHE STRING
    "Default limit on internal looping. See MATCH_LIMIT in config.h.in for details.")

SET(PCRE_MATCH_LIMIT_RECURSION "MATCH_LIMIT" CACHE STRING
    "Default limit on internal recursion. See MATCH_LIMIT_RECURSION in config.h.in for details.")

SET(PCREGREP_BUFSIZE "20480" CACHE STRING
    "Buffer size parameter for pcregrep. See PCREGREP_BUFSIZE in config.h.in for details.")

SET(PCRE_NEWLINE "LF" CACHE STRING
    "What to recognize as a newline (one of CR, LF, CRLF, ANY, ANYCRLF).")

# MARIADB: Changed the default from OFF to ON as pcre_test.bat on Windows
# MARIADB: fails complaining about too small stack size on Windows.
SET(PCRE_NO_RECURSE ON CACHE BOOL
    "If ON, then don't use stack recursion when matching. See NO_RECURSE in config.h.in for details.")

SET(PCRE_POSIX_MALLOC_THRESHOLD "10" CACHE STRING
    "Threshold for malloc() usage. See POSIX_MALLOC_THRESHOLD in config.h.in for details.")

#MARIADB
#SET(PCRE_SUPPORT_JIT OFF CACHE BOOL
#    "Enable support for Just-in-time compiling.")

#MARIADB
#SET(PCRE_SUPPORT_PCREGREP_JIT ON CACHE BOOL
#    "Enable use of Just-in-time compiling in pcregrep.")

SET(PCRE_SUPPORT_UNICODE_PROPERTIES OFF CACHE BOOL
    "Enable support for Unicode properties (if set, UTF support will be enabled as well).")

SET(PCRE_SUPPORT_BSR_ANYCRLF OFF CACHE BOOL
    "ON=Backslash-R matches only LF CR and CRLF, OFF=Backslash-R matches all Unicode Linebreaks")

OPTION(PCRE_SHOW_REPORT    "Show the final configuration report" OFF)
OPTION(PCRE_BUILD_PCREGREP "Build pcregrep" ON)
OPTION(PCRE_BUILD_TESTS    "Build the tests" ON)

IF (MINGW)
  OPTION(NON_STANDARD_LIB_PREFIX
         "ON=Shared libraries built in mingw will be named pcre.dll, etc., instead of libpcre.dll, etc."
         OFF)

  OPTION(NON_STANDARD_LIB_SUFFIX
         "ON=Shared libraries built in mingw will be named libpcre-0.dll, etc., instead of libpcre.dll, etc."
         OFF)
ENDIF(MINGW)

# Prepare build configuration

SET(pcre_have_type_traits 0)
SET(pcre_have_bits_type_traits 0)

IF(HAVE_TYPE_TRAITS_H)
        SET(pcre_have_type_traits 1)
ENDIF(HAVE_TYPE_TRAITS_H)

IF(HAVE_BITS_TYPE_TRAITS_H)
        SET(pcre_have_bits_type_traits 1)
ENDIF(HAVE_BITS_TYPE_TRAITS_H)

SET(pcre_have_long_long 0)
SET(pcre_have_ulong_long 0)

IF(HAVE_LONG_LONG)
        SET(pcre_have_long_long 1)
ENDIF(HAVE_LONG_LONG)

IF(HAVE_UNSIGNED_LONG_LONG)
        SET(pcre_have_ulong_long 1)
ENDIF(HAVE_UNSIGNED_LONG_LONG)

SET(PCRE_STATIC 1)
SET(SUPPORT_PCRE8 1)

IF(PCRE_SUPPORT_BSR_ANYCRLF)
        SET(BSR_ANYCRLF 1)
ENDIF(PCRE_SUPPORT_BSR_ANYCRLF)

SET(SUPPORT_UTF 1)
SET(PCRE_SUPPORT_UTF ON)

IF(PCRE_SUPPORT_UNICODE_PROPERTIES)
        SET(SUPPORT_UCP 1)
ENDIF(PCRE_SUPPORT_UNICODE_PROPERTIES)

#MARIADB
#IF(PCRE_SUPPORT_JIT)
#        SET(SUPPORT_JIT 1)
#ENDIF(PCRE_SUPPORT_JIT)

#MARIADB
#IF(PCRE_SUPPORT_PCREGREP_JIT)
#        SET(SUPPORT_PCREGREP_JIT 1)
#ENDIF(PCRE_SUPPORT_PCREGREP_JIT)

SET(NEWLINE "")

IF(PCRE_NEWLINE STREQUAL "LF")
        SET(NEWLINE "10")
ENDIF(PCRE_NEWLINE STREQUAL "LF")
IF(PCRE_NEWLINE STREQUAL "CR")
        SET(NEWLINE "13")
ENDIF(PCRE_NEWLINE STREQUAL "CR")
IF(PCRE_NEWLINE STREQUAL "CRLF")
        SET(NEWLINE "3338")
ENDIF(PCRE_NEWLINE STREQUAL "CRLF")
IF(PCRE_NEWLINE STREQUAL "ANY")
        SET(NEWLINE "-1")
ENDIF(PCRE_NEWLINE STREQUAL "ANY")
IF(PCRE_NEWLINE STREQUAL "ANYCRLF")
        SET(NEWLINE "-2")
ENDIF(PCRE_NEWLINE STREQUAL "ANYCRLF")

IF(NEWLINE STREQUAL "")
        MESSAGE(FATAL_ERROR "The PCRE_NEWLINE variable must be set to one of the following values: \"LF\", \"CR\", \"CRLF\", \"ANY\", \"ANYCRLF\".")
ENDIF(NEWLINE STREQUAL "")

IF(PCRE_NO_RECURSE)
        SET(NO_RECURSE 1)
ENDIF(PCRE_NO_RECURSE)

# Output files
CONFIGURE_FILE(config-cmake.h.in
               ${PROJECT_BINARY_DIR}/config.h
               @ONLY)

# Parse version numbers and date out of configure.ac

file(STRINGS ${PROJECT_SOURCE_DIR}/configure.ac
  configure_lines
  LIMIT_COUNT 50 # Read only the first 50 lines of the file
)

set(SEARCHED_VARIABLES "pcre_major" "pcre_minor" "pcre_prerelease" "pcre_date")
foreach(configure_line ${configure_lines})
    foreach(_substitution_variable ${SEARCHED_VARIABLES})
        string(TOUPPER ${_substitution_variable} _substitution_variable_upper)
        if (NOT ${_substitution_variable_upper})
            string(REGEX MATCH "m4_define\\(${_substitution_variable}, \\[(.*)\\]" MACTHED_STRING ${configure_line})
            if (CMAKE_MATCH_1)
                set(${_substitution_variable_upper} ${CMAKE_MATCH_1})
            endif()
        endif()
    endforeach()
endforeach()

CONFIGURE_FILE(pcre.h.in
               ${PROJECT_BINARY_DIR}/pcre.h
               @ONLY)

# Character table generation
  CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/pcre_chartables.c.dist
                    ${PROJECT_BINARY_DIR}/pcre_chartables.c
                    COPYONLY)

# Source code

SET(PCRE_HEADERS ${PROJECT_BINARY_DIR}/pcre.h)

SET(PCRE_SOURCES
  pcre_byte_order.c
  pcre_chartables.c
  pcre_compile.c
  pcre_config.c
  pcre_dfa_exec.c
  pcre_exec.c
  pcre_fullinfo.c
  pcre_get.c
  pcre_globals.c
  pcre_jit_compile.c
  pcre_maketables.c
  pcre_newline.c
  pcre_ord2utf8.c
  pcre_refcount.c
  pcre_string_utils.c
  pcre_study.c
  pcre_tables.c
  pcre_ucd.c
  pcre_valid_utf8.c
  pcre_version.c
  pcre_xclass.c
)

SET(PCREPOSIX_HEADERS pcreposix.h)

SET(PCREPOSIX_SOURCES pcreposix.c)

# Build setup

ADD_DEFINITIONS(-DHAVE_CONFIG_H)

IF(MSVC)
        ADD_DEFINITIONS(-D_CRT_SECURE_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS)
ENDIF(MSVC)

SET(CMAKE_INCLUDE_CURRENT_DIR 1)

SET(targets)

# Libraries
# pcre
ADD_CONVENIENCE_LIBRARY(pcre ${PCRE_HEADERS} ${PCRE_SOURCES} ${PROJECT_BINARY_DIR}/config.h)
SET(targets ${targets} pcre)
ADD_CONVENIENCE_LIBRARY(pcreposix ${PCREPOSIX_HEADERS} ${PCREPOSIX_SOURCES})
SET(targets ${targets} pcreposix)
TARGET_LINK_LIBRARIES(pcreposix pcre)

# Executables

# Removed by PH (2008-01-23) because pcredemo shouldn't really be built
# automatically, and it gave trouble in some environments anyway.
# ADD_EXECUTABLE(pcredemo pcredemo.c)
# TARGET_LINK_LIBRARIES(pcredemo pcreposix)
# IF(NOT BUILD_SHARED_LIBS)
#     # make sure to not use declspec(dllimport) in static mode on windows
#         SET_TARGET_PROPERTIES(pcredemo PROPERTIES COMPILE_FLAGS "-DPCRE_STATIC")
# ENDIF(NOT BUILD_SHARED_LIBS)

IF(PCRE_BUILD_PCREGREP)
  ADD_EXECUTABLE(pcregrep pcregrep.c)
  SET(targets ${targets} pcregrep)
  TARGET_LINK_LIBRARIES(pcregrep pcreposix ${PCREGREP_LIBS})
ENDIF(PCRE_BUILD_PCREGREP)

# Testing
IF(PCRE_BUILD_TESTS)
  ENABLE_TESTING()

  SET(PCRETEST_SOURCES pcretest.c)
  LIST(APPEND PCRETEST_SOURCES pcre_printint.c)

  ADD_EXECUTABLE(pcretest ${PCRETEST_SOURCES})
  SET(targets ${targets} pcretest)
  LIST(APPEND PCRETEST_LIBS pcreposix pcre)
  TARGET_LINK_LIBRARIES(pcretest ${PCRETEST_LIBS})

#MARIADB
#  IF(PCRE_SUPPORT_JIT)
#    ADD_EXECUTABLE(pcre_jit_test pcre_jit_test.c)
#    SET(targets ${targets} pcre_jit_test)
#    SET(PCRE_JIT_TEST_LIBS )
#    LIST(APPEND PCRE_JIT_TEST_LIBS pcre)
#    TARGET_LINK_LIBRARIES(pcre_jit_test ${PCRE_JIT_TEST_LIBS})
#  ENDIF(PCRE_SUPPORT_JIT)

  # exes in Debug location tested by the RunTest shell script
  # via "make test"
  IF(PCRE_BUILD_PCREGREP)
    GET_TARGET_PROPERTY(PCREGREP_EXE pcregrep DEBUG_LOCATION)
  ENDIF(PCRE_BUILD_PCREGREP)

  GET_TARGET_PROPERTY(PCRETEST_EXE pcretest DEBUG_LOCATION)
  STRING(REGEX REPLACE "[^/]+$" "" PCRETEST_DIR ${PCRETEST_EXE})

# =================================================
  # Write out a CTest configuration file
  #
  FILE(WRITE ${PROJECT_BINARY_DIR}/CTestCustom.ctest
  "# This is a generated file.
MESSAGE(\"When testing is complete, review test output in the
\\\"${PROJECT_BINARY_DIR}/Testing/Temporary\\\" folder.\")
MESSAGE(\" \")
")

  FILE(WRITE ${PROJECT_BINARY_DIR}/pcre_test.sh
  "#! /bin/sh
# This is a generated file.
srcdir=${PROJECT_SOURCE_DIR}
pcretest=${PCRETEST_EXE}
cd ${PCRETEST_DIR}
. ${PROJECT_SOURCE_DIR}/RunTest
if test \"$?\" != \"0\"; then exit 1; fi
# End
")

  IF(UNIX)
    ADD_TEST(pcre_test      sh ${PROJECT_BINARY_DIR}/pcre_test.sh)
  ENDIF(UNIX)

  IF(PCRE_BUILD_PCREGREP)
    FILE(WRITE ${PROJECT_BINARY_DIR}/pcre_grep_test.sh
    "#! /bin/sh
# This is a generated file.
srcdir=${PROJECT_SOURCE_DIR}
pcregrep=${PCREGREP_EXE}
pcretest=${PCRETEST_EXE}
cd ${PCRETEST_DIR}
. ${PROJECT_SOURCE_DIR}/RunGrepTest
if test \"$?\" != \"0\"; then exit 1; fi
# End
")

    IF(UNIX)
      ADD_TEST(pcre_grep_test sh ${PROJECT_BINARY_DIR}/pcre_grep_test.sh)
    ENDIF(UNIX)
  ENDIF(PCRE_BUILD_PCREGREP)

  IF(WIN32)
    # Provide environment for executing the bat file version of RunTest
    FILE(TO_NATIVE_PATH ${PROJECT_SOURCE_DIR} winsrc)
    FILE(TO_NATIVE_PATH ${PROJECT_BINARY_DIR} winbin)
    FILE(TO_NATIVE_PATH ${PCRETEST_EXE} winexe)

    FILE(WRITE ${PROJECT_BINARY_DIR}/pcre_test.bat
    "\@REM This is a generated file.
\@echo off
setlocal
SET srcdir=\"${winsrc}\"
SET pcretest=\"${winexe}\"
if not [%CMAKE_CONFIG_TYPE%]==[] SET pcretest=\"${winbin}\\%CMAKE_CONFIG_TYPE%\\pcretest.exe\"
call %srcdir%\\RunTest.Bat
if errorlevel 1 exit /b 1
echo RunTest.bat tests successfully completed
")

  ADD_TEST(NAME pcre_test_bat
  COMMAND pcre_test.bat)
  SET_TESTS_PROPERTIES(pcre_test_bat PROPERTIES
  PASS_REGULAR_EXPRESSION "RunTest\\.bat tests successfully completed")

    IF("$ENV{OSTYPE}" STREQUAL "msys")
      # Both the sh and bat file versions of RunTest are run if make test is used
      # in msys
      ADD_TEST(pcre_test_sh    sh.exe ${PROJECT_BINARY_DIR}/pcre_test.sh)
      IF(PCRE_BUILD_PCREGREP)
        ADD_TEST(pcre_grep_test  sh.exe ${PROJECT_BINARY_DIR}/pcre_grep_test.sh)
      ENDIF(PCRE_BUILD_PCREGREP)
    ENDIF("$ENV{OSTYPE}" STREQUAL "msys")

  ENDIF(WIN32)

  # Changed to accommodate testing whichever location was just built

#MARIADB
#  IF(PCRE_SUPPORT_JIT)
#    ADD_TEST(pcre_jit_test         pcre_jit_test)
#  ENDIF(PCRE_SUPPORT_JIT)

ENDIF(PCRE_BUILD_TESTS)

MARK_AS_ADVANCED(PCREGREP_BUFSIZE)
MARK_AS_ADVANCED(PCRE_BUILD_PCREGREP)
MARK_AS_ADVANCED(PCRE_BUILD_TESTS)
MARK_AS_ADVANCED(PCRE_LINK_SIZE)
MARK_AS_ADVANCED(PCRE_MATCH_LIMIT)
MARK_AS_ADVANCED(PCRE_MATCH_LIMIT_RECURSION)
MARK_AS_ADVANCED(PCRE_NEWLINE)
MARK_AS_ADVANCED(PCRE_NO_RECURSE)
MARK_AS_ADVANCED(PCRE_POSIX_MALLOC_THRESHOLD)
MARK_AS_ADVANCED(PCRE_SUPPORT_BSR_ANYCRLF)
MARK_AS_ADVANCED(PCRE_SUPPORT_UNICODE_PROPERTIES)


IF(PCRE_SHOW_REPORT)
  STRING(TOUPPER "${CMAKE_BUILD_TYPE}" buildtype)
  IF (CMAKE_C_FLAGS)
    SET(cfsp " ")
  ENDIF(CMAKE_C_FLAGS)
  IF (CMAKE_CXX_FLAGS)
    SET(cxxfsp " ")
  ENDIF(CMAKE_CXX_FLAGS)
  MESSAGE(STATUS "")
  MESSAGE(STATUS "")
  MESSAGE(STATUS "PCRE configuration summary:")
  MESSAGE(STATUS "")
#  MESSAGE(STATUS "  Install prefix .................. : ${CMAKE_INSTALL_PREFIX}")
  MESSAGE(STATUS "  C compiler ...................... : ${CMAKE_C_COMPILER}")
#  MESSAGE(STATUS "  C++ compiler .................... : ${CMAKE_CXX_COMPILER}")
  MESSAGE(STATUS "  C compiler flags ................ : ${CMAKE_C_FLAGS}${cfsp}${CMAKE_C_FLAGS_${buildtype}}")
#  MESSAGE(STATUS "  Enable JIT compiling support .... : ${PCRE_SUPPORT_JIT}")
  MESSAGE(STATUS "  Unicode properties .............. : ${PCRE_SUPPORT_UNICODE_PROPERTIES}")
  MESSAGE(STATUS "  Newline char/sequence ........... : ${PCRE_NEWLINE}")
  MESSAGE(STATUS "  \\R matches only ANYCRLF ......... : ${PCRE_SUPPORT_BSR_ANYCRLF}")
#  MESSAGE(STATUS "  EBCDIC coding ................... : ${PCRE_EBCDIC}")
#  MESSAGE(STATUS "  EBCDIC coding with NL=0x25 ...... : ${PCRE_EBCDIC_NL25}")
#  MESSAGE(STATUS "  Rebuild char tables ............. : ${PCRE_REBUILD_CHARTABLES}")
  MESSAGE(STATUS "  No stack recursion .............. : ${PCRE_NO_RECURSE}")
  MESSAGE(STATUS "  POSIX mem threshold ............. : ${PCRE_POSIX_MALLOC_THRESHOLD}")
  MESSAGE(STATUS "  Internal link size .............. : ${PCRE_LINK_SIZE}")
  MESSAGE(STATUS "  Parentheses nest limit .......... : ${PCRE_PARENS_NEST_LIMIT}")
  MESSAGE(STATUS "  Match limit ..................... : ${PCRE_MATCH_LIMIT}")
  MESSAGE(STATUS "  Match limit recursion ........... : ${PCRE_MATCH_LIMIT_RECURSION}")
#  MESSAGE(STATUS "  Build shared libs ............... : ${BUILD_SHARED_LIBS}")
#  MESSAGE(STATUS "  Build static libs ............... : ${BUILD_STATIC_LIBS}")
  MESSAGE(STATUS "  Build pcregrep .................. : ${PCRE_BUILD_PCREGREP}")
#  MESSAGE(STATUS "  Enable JIT in pcregrep .......... : ${PCRE_SUPPORT_PCREGREP_JIT}")
  MESSAGE(STATUS "  Buffer size for pcregrep ........ : ${PCREGREP_BUFSIZE}")
  MESSAGE(STATUS "  Build tests (implies pcretest  .. : ${PCRE_BUILD_TESTS}")
  MESSAGE(STATUS "               and pcregrep)")

  MESSAGE(STATUS "")
ENDIF(PCRE_SHOW_REPORT)

INSTALL(FILES ${CMAKE_BINARY_DIR}/pcre/pcre.h DESTINATION ${INSTALL_INCLUDEDIR}/private COMPONENT Development)

# end CMakeLists.txt
