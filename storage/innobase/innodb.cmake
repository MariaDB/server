# Copyright (c) 2006, 2016, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2017, 2020, MariaDB Corporation.
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

# This is the CMakeLists for InnoDB

INCLUDE(CheckFunctionExists)
INCLUDE(CheckCSourceCompiles)
INCLUDE(CheckCSourceRuns)
INCLUDE(lz4.cmake)
INCLUDE(lzo.cmake)
INCLUDE(lzma.cmake)
INCLUDE(bzip2.cmake)
INCLUDE(snappy.cmake)
INCLUDE(numa)
INCLUDE(TestBigEndian)

MYSQL_CHECK_LZ4()
MYSQL_CHECK_LZO()
MYSQL_CHECK_LZMA()
MYSQL_CHECK_BZIP2()
MYSQL_CHECK_SNAPPY()
MYSQL_CHECK_NUMA()

INCLUDE(${MYSQL_CMAKE_SCRIPT_DIR}/compile_flags.cmake)

IF(CMAKE_CROSSCOMPILING)
  # Use CHECK_C_SOURCE_COMPILES instead of CHECK_C_SOURCE_RUNS when
  # cross-compiling. Not as precise, but usually good enough.
  # This only make sense for atomic tests in this file, this trick doesn't
  # work in a general case.
  MACRO(CHECK_C_SOURCE SOURCE VAR)
    CHECK_C_SOURCE_COMPILES("${SOURCE}" "${VAR}")
  ENDMACRO()
ELSE()
  MACRO(CHECK_C_SOURCE SOURCE VAR)
    CHECK_C_SOURCE_RUNS("${SOURCE}" "${VAR}")
  ENDMACRO()
ENDIF()

# OS tests
IF(UNIX)
  IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    ADD_DEFINITIONS("-DUNIV_LINUX -D_GNU_SOURCE=1")

    CHECK_INCLUDE_FILES (libaio.h HAVE_LIBAIO_H)
    CHECK_LIBRARY_EXISTS(aio io_queue_init "" HAVE_LIBAIO)

    IF(HAVE_LIBAIO_H AND HAVE_LIBAIO)
      ADD_DEFINITIONS(-DLINUX_NATIVE_AIO=1)
      LINK_LIBRARIES(aio)
    ENDIF()
    IF(HAVE_LIBNUMA)
      LINK_LIBRARIES(numa)
    ENDIF()
  ELSEIF(CMAKE_SYSTEM_NAME MATCHES "HP*")
    ADD_DEFINITIONS("-DUNIV_HPUX")
  ELSEIF(CMAKE_SYSTEM_NAME STREQUAL "AIX")
    ADD_DEFINITIONS("-DUNIV_AIX")
  ELSEIF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    ADD_DEFINITIONS("-DUNIV_SOLARIS")
  ENDIF()
ENDIF()

OPTION(INNODB_COMPILER_HINTS "Compile InnoDB with compiler hints" ON)
MARK_AS_ADVANCED(INNODB_COMPILER_HINTS)

IF(INNODB_COMPILER_HINTS)
   ADD_DEFINITIONS("-DCOMPILER_HINTS")
ENDIF()
ADD_FEATURE_INFO(INNODB_COMPILER_HINTS INNODB_COMPILER_HINTS "InnoDB compiled with compiler hints")

# Enable InnoDB's UNIV_DEBUG in debug builds
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DUNIV_DEBUG")

OPTION(WITH_INNODB_AHI "Include innodb_adaptive_hash_index" ON)
OPTION(WITH_INNODB_ROOT_GUESS "Cache index root block descriptors" ON)
IF(WITH_INNODB_AHI)
  ADD_DEFINITIONS(-DBTR_CUR_HASH_ADAPT -DBTR_CUR_ADAPT)
  IF(NOT WITH_INNODB_ROOT_GUESS)
    MESSAGE(WARNING "WITH_INNODB_AHI implies WITH_INNODB_ROOT_GUESS")
    SET(WITH_INNODB_ROOT_GUESS ON)
  ENDIF()
ELSEIF(WITH_INNODB_ROOT_GUESS)
  ADD_DEFINITIONS(-DBTR_CUR_ADAPT)
ENDIF()
ADD_FEATURE_INFO(INNODB_AHI WITH_INNODB_AHI "InnoDB Adaptive Hash Index")
ADD_FEATURE_INFO(INNODB_ROOT_GUESS WITH_INNODB_ROOT_GUESS
                 "Cache index root block descriptors in InnoDB")

OPTION(WITH_INNODB_EXTRA_DEBUG "Enable extra InnoDB debug checks" OFF)
IF(WITH_INNODB_EXTRA_DEBUG)
  ADD_DEFINITIONS(-DUNIV_ZIP_DEBUG)
ENDIF()
ADD_FEATURE_INFO(INNODB_EXTRA_DEBUG WITH_INNODB_EXTRA_DEBUG "Extra InnoDB debug checks")


CHECK_FUNCTION_EXISTS(sched_getcpu  HAVE_SCHED_GETCPU)
IF(HAVE_SCHED_GETCPU)
 ADD_DEFINITIONS(-DHAVE_SCHED_GETCPU=1)
ENDIF()

CHECK_FUNCTION_EXISTS(nanosleep HAVE_NANOSLEEP)
IF(HAVE_NANOSLEEP)
 ADD_DEFINITIONS(-DHAVE_NANOSLEEP=1)
ENDIF()

IF(HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE)
 ADD_DEFINITIONS(-DHAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE=1)
ENDIF()

IF (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wconversion -Wno-sign-conversion")
  SET_SOURCE_FILES_PROPERTIES(fts/fts0pars.cc
    PROPERTIES COMPILE_FLAGS -Wno-conversion)
ENDIF()

IF(NOT MSVC)
  # Work around MDEV-18417, MDEV-18656, MDEV-18417
  IF(WITH_ASAN AND CMAKE_COMPILER_IS_GNUCC AND
     CMAKE_C_COMPILER_VERSION VERSION_LESS "6.0.0")
    SET_SOURCE_FILES_PROPERTIES(trx/trx0rec.cc PROPERTIES COMPILE_FLAGS -O1)
  ENDIF()
ENDIF()

CHECK_FUNCTION_EXISTS(vasprintf  HAVE_VASPRINTF)

SET(MUTEXTYPE "event" CACHE STRING "Mutex type: event, sys or futex")

IF(MUTEXTYPE MATCHES "event")
  ADD_DEFINITIONS(-DMUTEX_EVENT)
ELSEIF(MUTEXTYPE MATCHES "futex" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  ADD_DEFINITIONS(-DMUTEX_FUTEX)
ELSE()
   ADD_DEFINITIONS(-DMUTEX_SYS)
ENDIF()

# Include directories under innobase
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/storage/innobase/include
		    ${CMAKE_SOURCE_DIR}/storage/innobase/handler)

# Sun Studio bug with -xO2
IF(CMAKE_CXX_COMPILER_ID MATCHES "SunPro"
	AND CMAKE_CXX_FLAGS_RELEASE MATCHES "O2"
	AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
	# Sun Studio 12 crashes with -xO2 flag, but not with higher optimization
	# -xO3
	SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_SOURCE_DIR}/rem/rem0rec.cc
    PROPERTIES COMPILE_FLAGS -xO3)
ENDIF()


IF(MSVC)
  # Avoid "unreferenced label" warning in generated file
  GET_FILENAME_COMPONENT(_SRC_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)
  SET_SOURCE_FILES_PROPERTIES(${_SRC_DIR}/pars/pars0grm.c
          PROPERTIES COMPILE_FLAGS "/wd4102")
  SET_SOURCE_FILES_PROPERTIES(${_SRC_DIR}/pars/lexyy.c
          PROPERTIES COMPILE_FLAGS "/wd4003")
ENDIF()

# Include directories under innobase
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/storage/innobase/include
                    ${CMAKE_SOURCE_DIR}/storage/innobase/handler
                    ${CMAKE_SOURCE_DIR}/libbinlogevents/include )
