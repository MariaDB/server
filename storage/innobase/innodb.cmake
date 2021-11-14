# Copyright (c) 2006, 2016, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2017, 2019, MariaDB Corporation.
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
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)

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

## MySQL 5.7 LZ4 (not needed)
##IF(LZ4_INCLUDE_DIR AND LZ4_LIBRARY)
##  ADD_DEFINITIONS(-DHAVE_LZ4=1)
##  INCLUDE_DIRECTORIES(${LZ4_INCLUDE_DIR})
##ENDIF()

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

SET(MUTEXTYPE "event" CACHE STRING "Mutex type: event, sys or futex")

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
# After: WL#5825 Using C++ Standard Library with MySQL code
#       we no longer use -fno-exceptions
#	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")

# Add -Wconversion if compiling with GCC
## As of Mar 15 2011 this flag causes 3573+ warnings. If you are reading this
## please fix them and enable the following code:
#SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wconversion")

  IF (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR
      CMAKE_SYSTEM_PROCESSOR MATCHES "i386" AND
      CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.6)
    INCLUDE(CheckCXXCompilerFlag)
    CHECK_CXX_COMPILER_FLAG("-fno-builtin-memcmp" HAVE_NO_BUILTIN_MEMCMP)
    IF (HAVE_NO_BUILTIN_MEMCMP)
      # Work around http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43052
      SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_SOURCE_DIR}/rem/rem0cmp.cc
	PROPERTIES COMPILE_FLAGS -fno-builtin-memcmp)
    ENDIF()
  ENDIF()
ENDIF()

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

OPTION(WITH_INNODB_BUG_ENDIAN_CRC32 "Weaken innodb_checksum_algorithm=crc32 by supporting upgrade from big-endian systems running 5.6/10.0/10.1" ${IS_BIG_ENDIAN})
IF(WITH_INNODB_BUG_ENDIAN_CRC32)
  ADD_DEFINITIONS(-DINNODB_BUG_ENDIAN_CRC32)
ENDIF()
ADD_FEATURE_INFO(INNODB_BUG_ENDIAN_CRC32 WITH_INNODB_BUG_ENDIAN_CRC32 "Big endian weaker CRC32 to support upgrade from big-endian systems running 5.6/10.0/10.1")

OPTION(WITH_INNODB_EXTRA_DEBUG "Enable extra InnoDB debug checks" OFF)
IF(WITH_INNODB_EXTRA_DEBUG)
  IF(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    MESSAGE(FATAL_ERROR "WITH_INNODB_EXTRA_DEBUG can be enabled only in debug builds")
  ENDIF()

  SET(EXTRA_DEBUG_FLAGS "")
  IF(WITH_INNODB_AHI)
    SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_AHI_DEBUG")
  ENDIF()
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_DDL_DEBUG")
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_DEBUG_FILE_ACCESSES")
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_ZIP_DEBUG")

  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${EXTRA_DEBUG_FLAGS}")
  SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${EXTRA_DEBUG_FLAGS}")
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

IF(NOT MSVC)
  # Work around MDEV-18417, MDEV-18656, MDEV-18417
  IF(WITH_ASAN AND CMAKE_COMPILER_IS_GNUCC AND
     CMAKE_C_COMPILER_VERSION VERSION_LESS "6.0.0")
    SET_SOURCE_FILES_PROPERTIES(trx/trx0rec.cc PROPERTIES COMPILE_FLAGS -O1)
  ENDIF()

  # workaround for old gcc on x86, gcc atomic ops only work under -march=i686
  IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "i686" AND CMAKE_COMPILER_IS_GNUCC AND
     CMAKE_C_COMPILER_VERSION VERSION_LESS "4.4.0")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=i686")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=i686")
  ENDIF()

# Only use futexes on Linux if GCC atomics are available
IF(NOT MSVC AND NOT CMAKE_CROSSCOMPILING)
  CHECK_C_SOURCE_RUNS(
  "
  #include <stdio.h>
  #include <unistd.h>
  #include <errno.h>
  #include <assert.h>
  #include <linux/futex.h>
  #include <unistd.h>
  #include <sys/syscall.h>

   int futex_wait(int* futex, int v) {
	return(syscall(SYS_futex, futex, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0));
   }

   int futex_signal(int* futex) {
	return(syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 0));
   }

  int main() {
	int	ret;
	int	m = 1;

	/* It is setup to fail and return EWOULDBLOCK. */
	ret = futex_wait(&m, 0);
	assert(ret == -1 && errno == EWOULDBLOCK);
	/* Shouldn't wake up any threads. */
	assert(futex_signal(&m) == 0);

	return(0);
  }"
  HAVE_IB_LINUX_FUTEX)
ENDIF()

IF(HAVE_IB_LINUX_FUTEX)
  ADD_DEFINITIONS(-DHAVE_IB_LINUX_FUTEX=1)
ENDIF()

ENDIF(NOT MSVC)

CHECK_FUNCTION_EXISTS(vasprintf  HAVE_VASPRINTF)

SET(MUTEXTYPE "event" CACHE STRING "Mutex type: event, sys or futex")

IF(MUTEXTYPE MATCHES "event")
  ADD_DEFINITIONS(-DMUTEX_EVENT)
ELSEIF(MUTEXTYPE MATCHES "futex" AND DEFINED HAVE_IB_LINUX_FUTEX)
  ADD_DEFINITIONS(-DMUTEX_FUTEX)
ELSE()
   ADD_DEFINITIONS(-DMUTEX_SYS)
ENDIF()

OPTION(WITH_INNODB_DISALLOW_WRITES "InnoDB freeze writes patch from Google" ${WITH_WSREP})
IF (WITH_INNODB_DISALLOW_WRITES)
  ADD_DEFINITIONS(-DWITH_INNODB_DISALLOW_WRITES)
ENDIF()
ADD_FEATURE_INFO(INNODB_DISALLOW_WRITES WITH_INNODB_DISALLOW_WRITES "Expose innodb_disallow_writes switch to stop innodb from writing to disk")


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

# Removing compiler optimizations for innodb/mem/* files on 64-bit Windows
# due to 64-bit compiler error, See MySQL Bug #19424, #36366, #34297
IF (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	SET_SOURCE_FILES_PROPERTIES(mem/mem0mem.cc mem/mem0pool.cc
				    PROPERTIES COMPILE_FLAGS -Od)
ENDIF()

# Avoid generating Hardware Capabilities due to crc32 instructions
IF(CMAKE_SYSTEM_NAME MATCHES "SunOS" AND CMAKE_SYSTEM_PROCESSOR MATCHES "i386")
  MY_CHECK_CXX_COMPILER_FLAG("-Wa,-nH")
  IF(have_CXX__Wa__nH)
    ADD_COMPILE_FLAGS(
      ut/ut0crc32.cc
      COMPILE_FLAGS "-Wa,-nH"
    )
  ENDIF()
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
