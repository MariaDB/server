# TODO: Copyrights

IF (NOT EXISTS "${CMAKE_SOURCE_DIR}/rocksdb/Makefile")
  MESSAGE(SEND_ERROR "Missing Makefile in rocksdb directory. Try \"git submodule update\".")
ENDIF()

CHECK_FUNCTION_EXISTS(sched_getcpu  HAVE_SCHED_GETCPU)
IF(HAVE_SCHED_GETCPU)
  ADD_DEFINITIONS(-DHAVE_SCHED_GETCPU=1)
ENDIF()

# get a list of rocksdb library source files
# run with env -i to avoid passing variables
EXECUTE_PROCESS(
  COMMAND env -i ${CMAKE_SOURCE_DIR}/storage/rocksdb/get_rocksdb_files.sh
  OUTPUT_VARIABLE SCRIPT_OUTPUT
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# split the list into lines
STRING(REGEX MATCHALL "[^\n]+" ROCKSDB_LIB_SOURCES ${SCRIPT_OUTPUT})

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/rocksdb
  ${CMAKE_SOURCE_DIR}/rocksdb/include
  ${CMAKE_SOURCE_DIR}/rocksdb/third-party/gtest-1.7.0/fused-src
)

ADD_DEFINITIONS(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_LINUX
  -DZLIB)

CHECK_FUNCTION_EXISTS(fallocate HAVE_FALLOCATE)
IF(HAVE_FALLOCATE)
  ADD_DEFINITIONS(-DROCKSDB_FALLOCATE_PRESENT)
ENDIF()

SET(ROCKSDB_SOURCES
  ha_rocksdb.cc ha_rocksdb.h ha_rocksdb_proto.h
  logger.h
  rdb_comparator.h
  rdb_datadic.cc rdb_datadic.h
  rdb_cf_options.cc rdb_cf_options.h
  rdb_cf_manager.cc rdb_cf_manager.h
  properties_collector.cc properties_collector.h
  event_listener.cc event_listener.h
  rdb_i_s.cc rdb_i_s.h
  rdb_index_merge.cc rdb_index_merge.h
  rdb_perf_context.cc rdb_perf_context.h
  rdb_mutex_wrapper.cc rdb_mutex_wrapper.h
  rdb_psi.h rdb_psi.cc
  rdb_sst_info.cc rdb_sst_info.h
  rdb_utils.cc rdb_utils.h rdb_buff.h
  rdb_threads.cc rdb_threads.h
  ${ROCKSDB_LIB_SOURCES}
)

IF(WITH_FB_TSAN)
  SET(PIC_EXT "_pic")
ELSE()
  SET(PIC_EXT "")
ENDIF()

SET(rocksdb_static_libs )
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

SET(rocksdb_static_libs ${rocksdb_static_libs} ${ZLIB_LIBRARY} "-lrt")

MYSQL_ADD_PLUGIN(rocksdb_se ${ROCKSDB_SOURCES} STORAGE_ENGINE DEFAULT STATIC_ONLY
  LINK_LIBRARIES ${rocksdb_static_libs}
)

IF(WITH_EMBEDDED_SERVER)
  ADD_SUBDIRECTORY(unittest)
ENDIF()

IF (WITH_ROCKSDB_SE_STORAGE_ENGINE)
  # TODO: read this file list from src.mk:TOOL_SOURCES
  SET(ROCKSDB_TOOL_SOURCES
    ${CMAKE_SOURCE_DIR}/rocksdb/tools/ldb_tool.cc
    ${CMAKE_SOURCE_DIR}/rocksdb/tools/ldb_cmd.cc
    ${CMAKE_SOURCE_DIR}/rocksdb/tools/sst_dump_tool.cc
  )
  MYSQL_ADD_EXECUTABLE(sst_dump ${CMAKE_SOURCE_DIR}/rocksdb/tools/sst_dump.cc ${ROCKSDB_TOOL_SOURCES})
  TARGET_LINK_LIBRARIES(sst_dump rocksdb_se)

  MYSQL_ADD_EXECUTABLE(ldb ${CMAKE_SOURCE_DIR}/rocksdb/tools/ldb.cc ${ROCKSDB_TOOL_SOURCES})
  TARGET_LINK_LIBRARIES(ldb rocksdb_se)

  MYSQL_ADD_EXECUTABLE(mysql_ldb ${CMAKE_SOURCE_DIR}/storage/rocksdb/tools/mysql_ldb.cc ${ROCKSDB_TOOL_SOURCES})
  TARGET_LINK_LIBRARIES(mysql_ldb rocksdb_se)
ENDIF()
