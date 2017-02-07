# TODO: Copyrights
IF(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/rocksdb/Makefile AND GIT_EXECUTABLE)
  EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" submodule init
                  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" submodule update
                  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
ENDIF()

IF (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rocksdb/Makefile")
  MESSAGE(SEND_ERROR "Missing Makefile in rocksdb directory. Try \"git submodule update\".")
ENDIF()

MACRO(SKIP_ROCKSDB_PLUGIN msg)
  MESSAGE_ONCE(SKIP_ROCKSDB_PLUGIN "Can't build rocksdb engine - ${msg}")
  RETURN()
ENDMACRO()


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
ELSEIF (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  IF ((CMAKE_CXX_COMPILER_VERSION AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS  3.3) OR
     (CLANG_VERSION_STRING AND CLANG_VERSION_STRING VERSION_LESS  3.3))
    SKIP_ROCKSDB_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
  SET(CXX11_FLAGS "-std=c++11 -stdlib=libstdc++")
ELSEIF(MSVC)
  IF (MSVC_VERSION LESS 1900)
    SKIP_ROCKSDB_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
ELSE()
  SKIP_ROCKSDB_PLUGIN("Compiler not supported")
ENDIF()

IF(CXX11_FLAGS)
  ADD_DEFINITIONS(${CXX11_FLAGS})
ENDIF()

SET(ROCKSDB_SE_SOURCES
  rdb_mariadb_server_port.cc
  rdb_mariadb_server_port.h
  ha_rocksdb.cc
  ha_rocksdb.h
  rdb_i_s.cc
  rdb_i_s.h
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
)

MYSQL_ADD_PLUGIN(rocksdb_se ${ROCKSDB_SE_SOURCES} STORAGE_ENGINE MODULE_OUTPUT_NAME ha_rocksdb)

IF(NOT TARGET rocksdb_se)
  # Bail out if compilation with rocksdb engine is not requested
  RETURN()
ENDIF()

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
  rdb_sst_info.cc
  rdb_sst_info.h
  rdb_buff.h
  rdb_mariadb_port.h)

ADD_DEPENDENCIES(rocksdb_aux_lib GenError)

TARGET_LINK_LIBRARIES(rocksdb_aux_lib rocksdblib ${ZLIB_LIBRARY})
TARGET_LINK_LIBRARIES(rocksdb_se rocksdb_aux_lib)

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")

  # MARIAROCKS_NOT_YET: Add -frtti flag when compiling RocksDB files.
  # TODO: is this the right way to do this?
  #  - SQL layer and storage/rocksdb/*.cc are compiled with -fnortti
  #  - RocksDB files are compiled with "-fnortti ... -frtti" 
  #  - This causes RocksDB headers to be compiled with different settings:
  #     = with RTTI when compiling RocksDB
  #     = without RTTI when compiling storage/rocksdb/*.cc
  # 
  #  (facebook/mysql-5.6 just compiles everything without -f*rtti, which means
  #  everything is compiled with -frtti) 
  #
  #  (also had to add -frtti above, because something that event_listener.cc
  #  includes requires it. So, now everything in MariaRocks is compiled with
  #  -frtti)
  set_source_files_properties(event_listener.cc rdb_cf_options.cc
    PROPERTIES COMPILE_FLAGS -frtti)
ENDIF()

CHECK_FUNCTION_EXISTS(sched_getcpu  HAVE_SCHED_GETCPU)
IF(HAVE_SCHED_GETCPU)
  ADD_DEFINITIONS(-DHAVE_SCHED_GETCPU=1)
ENDIF()

IF(WITH_UNIT_TESTS AND WITH_EMBEDDED_SERVER)
  ADD_SUBDIRECTORY(unittest)
ENDIF()

ADD_LIBRARY(rocksdb_tools STATIC
   rocksdb/tools/ldb_tool.cc
   rocksdb/tools/ldb_cmd.cc
   rocksdb/tools/sst_dump_tool.cc
)

MYSQL_ADD_EXECUTABLE(sst_dump rocksdb/tools/sst_dump.cc)
TARGET_LINK_LIBRARIES(sst_dump rocksdblib)

MYSQL_ADD_EXECUTABLE(ldb rocksdb/tools/ldb.cc)
TARGET_LINK_LIBRARIES(ldb rocksdb_tools rocksdblib)

MYSQL_ADD_EXECUTABLE(mysql_ldb tools/mysql_ldb.cc)
TARGET_LINK_LIBRARIES(mysql_ldb rocksdb_tools rocksdb_aux_lib)

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  SET_TARGET_PROPERTIES(rocksdb_tools sst_dump ldb mysql_ldb PROPERTIES COMPILE_FLAGS -frtti)
ENDIF()
IF(MSVC)
  # RocksDB, the storage engine, overdoes "const" by adding
  # additional const qualifiers to parameters of the overriden virtual functions
  # This creates a lot of warnings, that we silence here.
  ADD_DEFINITIONS(/wd4373)

  # Some checks in C++ runtime that make debug build much slower
  ADD_DEFINITIONS(-D_ITERATOR_DEBUG_LEVEL=0)
ENDIF()

# Optional compression libraries.
#
# TODO: search compression libraries properly.
# Use FIND_PACKAGE, CHECK_LIBRARY_EXISTS etc
IF(MARIAROCKS_NOT_YET)
IF (NOT "$ENV{WITH_SNAPPY}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_SNAPPY}/libsnappy${PIC_EXT}.a)
  ADD_DEFINITIONS(-DSNAPPY)
ENDIF()

IF (NOT "$ENV{WITH_LZ4}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_LZ4}/liblz4${PIC_EXT}.a)
  ADD_DEFINITIONS(-DLZ4)
ENDIF()

IF (NOT "$ENV{WITH_BZ2}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_BZ2}/libbz2${PIC_EXT}.a)
  ADD_DEFINITIONS(-DBZIP2)
ENDIF()

# link ZSTD only if instructed
IF (NOT "$ENV{WITH_ZSTD}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_ZSTD}/libzstd${PIC_EXT}.a)
  ADD_DEFINITIONS(-DZSTD)
ENDIF()
ENDIF(MARIAROCKS_NOT_YET)
