# TODO: Copyrights

SET(CPACK_RPM_rocksdb-engine_PACKAGE_SUMMARY "RocksDB storage engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_rocksdb-engine_PACKAGE_DESCRIPTION "The RocksDB storage engine is a high performance storage engine, aimed
at maximising storage efficiency while maintaining InnoDB-like performance." PARENT_SCOPE)

MACRO(SKIP_ROCKSDB_PLUGIN msg)
  MESSAGE_ONCE(SKIP_ROCKSDB_PLUGIN "Can't build rocksdb engine - ${msg}")
  ADD_FEATURE_INFO(ROCKSDB "OFF" "Storage Engine")
  RETURN()
ENDMACRO()

IF (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rocksdb/CMakeLists.txt")
  SKIP_ROCKSDB_PLUGIN("Missing CMakeLists.txt in rocksdb directory. Try \"git submodule update\".")
ENDIF()

CHECK_LIBRARY_EXISTS(rt timer_delete "" HAVE_TIMER_DELETE)
IF (HAVE_TIMER_DELETE)
  ADD_DEFINITIONS(-DHAVE_TIMER_DELETE)
ENDIF(HAVE_TIMER_DELETE)

CHECK_FUNCTION_EXISTS(sched_getcpu  HAVE_SCHED_GETCPU)
IF(HAVE_SCHED_GETCPU)
  ADD_DEFINITIONS(-DHAVE_SCHED_GETCPU=1 -DROCKSDB_SCHED_GETCPU_PRESENT)
ENDIF()

IF(WITH_VALGRIND)
  ADD_DEFINITIONS(-DROCKSDB_VALGRIND_RUN=1)
ENDIF()

# We've had our builders hang during the build process. This prevents MariaRocks
# to be built on 32 bit intel OS kernels.
IF(CMAKE_SYSTEM_PROCESSOR MATCHES "i[36]86")
  SKIP_ROCKSDB_PLUGIN("Intel 32 bit not supported.")
ENDIF()

# Due to retrieved data being incorrect endian
include(TestBigEndian)
test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
  SKIP_ROCKSDB_PLUGIN("Big Endian not supported.")
endif()

#
# Also, disable building on 32-bit Windows
#
IF (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  SKIP_ROCKSDB_PLUGIN("32-Bit Windows are temporarily disabled")
ENDIF()
IF(MSVC_ARM64)
 SKIP_ROCKSDB_PLUGIN("Windows ARM64 not supported")
ENDIF()

#
# Also, disable on ARM64 when not Linux
# Requires submodule update to v6.16.3
# containing commit https://github.com/facebook/rocksdb/commit/ee4bd4780b321ddb5f92a0f4eb956f2a2ebd60dc
#
IF(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  SKIP_ROCKSDB_PLUGIN("ARM64 disabled on all except Linux")
ENDIF()

# This plugin needs recent C++ compilers (it is using C++11 features)
# Skip build for the old compilers
SET(CXX11_FLAGS)
SET(OLD_COMPILER_MSG "requires c++11 -capable compiler (minimal supported versions are g++ 4.8, clang 3.3, VS2015)")

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  EXECUTE_PROCESS(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
  IF (GCC_VERSION VERSION_LESS 4.8)
    SKIP_ROCKSDB_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
  SET(CXX11_FLAGS "-std=c++11")
  IF (GCC_VERSION VERSION_LESS 5.0)
    SET(CXX11_FLAGS "-std=c++11 -Wno-missing-field-initializers")
  ENDIF()
ELSEIF (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  IF ((CMAKE_CXX_COMPILER_VERSION AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS  3.3) OR
     (CLANG_VERSION_STRING AND CLANG_VERSION_STRING VERSION_LESS  3.3))
    SKIP_ROCKSDB_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
  SET(CXX11_FLAGS "-std=c++11 -stdlib=libstdc++")
  IF(MSVC)
    # clang-cl does not work yet
    SKIP_ROCKSDB_PLUGIN("Clang-cl is not supported")
  ENDIF()
ELSEIF(MSVC)
  IF (MSVC_VERSION LESS 1900)
    SKIP_ROCKSDB_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
ELSE()
  SKIP_ROCKSDB_PLUGIN("Compiler not supported")
ENDIF()

IF(CMAKE_VERSION GREATER 3.0)
  SET(CMAKE_CXX_STANDARD  11)
ELSEIF(CXX11_FLAGS)
  ADD_DEFINITIONS(${CXX11_FLAGS})
ENDIF()

SET(ROCKSDB_SE_SOURCES
  rdb_mariadb_server_port.cc
  rdb_mariadb_server_port.h
  ha_rocksdb.cc
  ha_rocksdb.h
  rdb_i_s.cc
  rdb_i_s.h
  rdb_io_watchdog.h
  rdb_io_watchdog.cc
  rdb_mutex_wrapper.cc
  rdb_mutex_wrapper.h
  rdb_index_merge.cc
  rdb_index_merge.h
  properties_collector.cc
  properties_collector.h
  rdb_datadic.cc
  rdb_datadic.h
  rdb_cf_manager.cc
  rdb_cf_manager.h
  rdb_utils.cc rdb_utils.h
  rdb_threads.cc
  rdb_threads.h
  rdb_psi.h
  rdb_psi.cc
  rdb_sst_info.cc
  rdb_sst_info.h
  rdb_converter.cc
  rdb_converter.h
)

# MariaDB: the following is added in build_rocksdb.cmake, when appropriate:
# This is a strong requirement coming from RocksDB. No conditional checks here.
#ADD_DEFINITIONS(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX
#)

if (HAVE_GCC_C11_ATOMICS_WITH_LIBATOMIC)
  SET(ATOMIC_EXTRA_LIBS -latomic)
else()
  SET(ATOMIC_EXTRA_LIBS)
endif()

# don't use compression providers, there are standalone executables below
GET_PROPERTY(dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
LIST(REMOVE_ITEM dirs ${CMAKE_SOURCE_DIR}/include/providers)
SET_PROPERTY(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${dirs}")

MYSQL_ADD_PLUGIN(rocksdb ${ROCKSDB_SE_SOURCES} MODULE_ONLY STORAGE_ENGINE
                 LINK_LIBRARIES ${ATOMIC_EXTRA_LIBS}
                 COMPONENT rocksdb-engine)

IF(NOT TARGET rocksdb)
  # Bail out if compilation with rocksdb engine is not requested
  RETURN()
ENDIF()

CHECK_CXX_SOURCE_COMPILES("
#if defined(_MSC_VER) && !defined(__thread)
#define __thread __declspec(thread)
#endif
int main() {
  static __thread int tls;
  tls=0;
  return tls;
}
" HAVE_THREAD_LOCAL)
if(HAVE_THREAD_LOCAL)
  ADD_DEFINITIONS(-DROCKSDB_SUPPORT_THREAD_LOCAL)
else()
  MESSAGE(SEND_ERROR "The compiler failed the check for ROCKSDB_SUPPORT_THREAD_LOCAL. "
                     "MyRocks requires that feature.")
endif()

INCLUDE(build_rocksdb.cmake)

ADD_CONVENIENCE_LIBRARY(rocksdb_aux_lib
  ha_rocksdb_proto.h
  logger.h
  rdb_comparator.h
  rdb_cf_options.cc
  rdb_cf_options.h
  event_listener.cc
  event_listener.h
  rdb_perf_context.cc
  rdb_perf_context.h
  rdb_buff.h
  rdb_mariadb_port.h
  nosql_access.cc nosql_access.h
)

ADD_DEPENDENCIES(rocksdb_aux_lib GenError)

# MARIAROCKS-TODO: how to properly depend on -lrt ?
TARGET_LINK_LIBRARIES(rocksdb_aux_lib rocksdblib ${ZLIB_LIBRARY})
if (UNIX AND NOT APPLE)
  TARGET_LINK_LIBRARIES(rocksdb_aux_lib -lrt)
endif()
TARGET_LINK_LIBRARIES(rocksdb_aux_lib ${ATOMIC_EXTRA_LIBS})

# IF (WITH_JEMALLOC)
#  FIND_LIBRARY(JEMALLOC_LIBRARY
#    NAMES libjemalloc${PIC_EXT}.a jemalloc
#    HINTS ${WITH_JEMALLOC}/lib)
#  SET(rocksdb_static_libs ${rocksdb_static_libs}
#    ${JEMALLOC_LIBRARY})
#  ADD_DEFINITIONS(-DROCKSDB_JEMALLOC)
#  ADD_DEFINITIONS(-DROCKSDB_MALLOC_USABLE_SIZE)
# ENDIF()

# MariaDB: Q: why does the upstream add libunwind for a particular 
# storage engine?
#IF (WITH_UNWIND)
#  FIND_LIBRARY(UNWIND_LIBRARY
#    NAMES libunwind${PIC_EXT}.a unwind
#    HINTS ${WITH_UNWIND}/lib)
#  SET(rocksdb_static_libs ${rocksdb_static_libs}
#    ${UNWIND_LIBRARY})
#ENDIF()


TARGET_LINK_LIBRARIES(rocksdb rocksdb_aux_lib)

CHECK_FUNCTION_EXISTS(sched_getcpu  HAVE_SCHED_GETCPU)
IF(HAVE_SCHED_GETCPU)
  ADD_DEFINITIONS(-DHAVE_SCHED_GETCPU=1)
# MariaDB: don't do this:
#  ADD_DEFINITIONS(-DZSTD_STATIC_LINKING_ONLY)
ENDIF()

IF (WITH_TBB)
  FIND_LIBRARY(TBB_LIBRARY
    NAMES libtbb${PIC_EXT}.a tbb
    HINTS ${WITH_TBB}/lib)
  SET(rocksdb_static_libs ${rocksdb_static_libs}
    ${TBB_LIBRARY})
  ADD_DEFINITIONS(-DTBB)
ENDIF()

#
# MariaDB: Dynamic plugin build is not suitable with unittest ATM
#
#IF(WITH_UNIT_TESTS AND WITH_EMBEDDED_SERVER)
#  ADD_SUBDIRECTORY(unittest)
#ENDIF()

if (UNIX AND NOT APPLE)
  SET(rocksdb_static_libs ${rocksdb_static_libs} "-lrt")
endif()


ADD_LIBRARY(rocksdb_tools STATIC
   rocksdb/tools/ldb_tool.cc
   rocksdb/tools/ldb_cmd.cc
   rocksdb/tools/sst_dump_tool.cc
)

MYSQL_ADD_EXECUTABLE(sst_dump rocksdb/tools/sst_dump.cc COMPONENT rocksdb-engine)
TARGET_LINK_LIBRARIES(sst_dump rocksdblib)

MYSQL_ADD_EXECUTABLE(mariadb-ldb tools/mysql_ldb.cc COMPONENT rocksdb-engine)
TARGET_LINK_LIBRARIES(mariadb-ldb rocksdb_tools rocksdb_aux_lib dbug)

CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/myrocks_hotbackup.py
  ${CMAKE_CURRENT_BINARY_DIR}/myrocks_hotbackup @ONLY)
INSTALL_SCRIPT(${CMAKE_CURRENT_BINARY_DIR}/myrocks_hotbackup COMPONENT rocksdb-engine)

IF(MSVC)
  # RocksDB, the storage engine, overdoes "const" by adding
  # additional const qualifiers to parameters of the overriden virtual functions
  # This creates a lot of warnings, that we silence here.
  ADD_DEFINITIONS(/wd4373)
  # Some checks in C++ runtime that make debug build much slower
  ADD_DEFINITIONS(-D_ITERATOR_DEBUG_LEVEL=0)

    # Temporarily disable "conversion from size_t .." warnings
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267")
  ENDIF()
ELSEIF(CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  SET_TARGET_PROPERTIES(rocksdb_tools sst_dump mariadb-ldb PROPERTIES COMPILE_FLAGS "-Wno-error")
ENDIF()

IF(GIT_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/rocksdb/.git)
  EXECUTE_PROCESS(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/rocksdb
       OUTPUT_VARIABLE OUT RESULT_VARIABLE RES)
  IF(RES EQUAL 0)
    STRING(REGEX REPLACE "\n$" "" ROCKSDB_GIT_HASH "${OUT}")
  ENDIF()
ENDIF()
IF(ROCKSDB_GIT_HASH OR
  (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/rdb_source_revision.h))
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/rdb_source_revision.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/rdb_source_revision.h )
ENDIF()
