
if(POLICY CMP0042)
  cmake_policy(SET CMP0042 NEW)
endif()

SET(ROCKSDB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rocksdb)

INCLUDE_DIRECTORIES(
  ${ROCKSDB_SOURCE_DIR}
  ${ROCKSDB_SOURCE_DIR}/include
  ${ROCKSDB_SOURCE_DIR}/third-party/gtest-1.7.0/fused-src
)

list(APPEND CMAKE_MODULE_PATH "${ROCKSDB_SOURCE_DIR}/cmake/modules/")

if(WIN32)
  # include(${ROCKSDB_SOURCE_DIR}/thirdparty.inc)
else()
  option(WITH_ROCKSDB_JEMALLOC "build RocksDB with JeMalloc" OFF)
  if(WITH_ROCKSDB_JEMALLOC)
    find_package(JeMalloc REQUIRED)
    add_definitions(-DROCKSDB_JEMALLOC)
    include_directories(${JEMALLOC_INCLUDE_DIR})
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # FreeBSD has jemaloc as default malloc
    add_definitions(-DROCKSDB_JEMALLOC)
    set(WITH_JEMALLOC ON)
  endif()
endif()


# Optional compression libraries.

foreach(compression_lib LZ4 BZIP2 ZSTD snappy)
  FIND_PACKAGE(${compression_lib} QUIET)

  SET(WITH_ROCKSDB_${compression_lib} AUTO CACHE STRING
  "Build RocksDB  with ${compression_lib} compression. Possible values are 'ON', 'OFF', 'AUTO' and default is 'AUTO'")

  if(${WITH_ROCKSDB_${compression_lib}} STREQUAL "ON"  AND NOT ${${compression_lib}_FOUND})
    MESSAGE(FATAL_ERROR
      "${compression_lib} library was not found, but WITH_ROCKSDB${compression_lib} option is ON.\
      Either set WITH_ROCKSDB${compression_lib} to OFF, or make sure ${compression_lib} is installed")
  endif()
endforeach()

if(LZ4_FOUND AND (NOT WITH_ROCKSDB_LZ4 STREQUAL "OFF"))
  add_definitions(-DLZ4)
  include_directories(${LZ4_INCLUDE_DIR})
  list(APPEND THIRDPARTY_LIBS ${LZ4_LIBRARY})
endif()

if(BZIP2_FOUND AND (NOT WITH_ROCKSDB_BZIP2 STREQUAL "OFF"))
  add_definitions(-DBZIP2)
  include_directories(${BZIP2_INCLUDE_DIR})
  list(APPEND THIRDPARTY_LIBS ${BZIP2_LIBRARIES})
endif()

if(SNAPPY_FOUND  AND (NOT WITH_ROCKSDB_SNAPPY STREQUAL "OFF"))
  add_definitions(-DSNAPPY)
  include_directories(${SNAPPY_INCLUDE_DIR})
  list(APPEND THIRDPARTY_LIBS ${SNAPPY_LIBRARIES})
endif()

if(ZSTD_FOUND AND (NOT WITH_ROCKSDB_ZSTD STREQUAL "OFF"))
  add_definitions(-DZSTD)
  include_directories(${ZSTD_INCLUDE_DIR})
  list(APPEND THIRDPARTY_LIBS ${ZSTD_LIBRARY})
endif()

add_definitions(-DZLIB)
list(APPEND THIRDPARTY_LIBS ${ZLIB_LIBRARY})

if(CMAKE_SYSTEM_NAME MATCHES "Cygwin")
  add_definitions(-fno-builtin-memcmp -DCYGWIN)
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
  add_definitions(-DOS_MACOSX)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_definitions(-DOS_LINUX)
elseif(CMAKE_SYSTEM_NAME MATCHES "SunOS")
  add_definitions(-DOS_SOLARIS)
elseif(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  add_definitions(-DOS_FREEBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "NetBSD")
  add_definitions(-DOS_NETBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  add_definitions(-DOS_OPENBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "DragonFly")
  add_definitions(-DOS_DRAGONFLYBSD)
elseif(CMAKE_SYSTEM_NAME MATCHES "Android")
  add_definitions(-DOS_ANDROID)
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  add_definitions(-DOS_WIN)
endif()

IF(MSVC)
  add_definitions(/wd4244)
ENDIF()
if(NOT WIN32)
  add_definitions(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX)
endif()

option(WITH_FALLOCATE "build with fallocate" ON)

if(WITH_FALLOCATE AND UNIX)
  include(CheckCSourceCompiles)
  CHECK_C_SOURCE_COMPILES("
#include <fcntl.h>
#include <linux/falloc.h>
int main() {
 int fd = open(\"/dev/null\", 0);
 fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, 0, 1024);
}
" HAVE_FALLOCATE)
  if(HAVE_FALLOCATE)
    add_definitions(-DROCKSDB_FALLOCATE_PRESENT)
  endif()
endif()

include(CheckFunctionExists)
CHECK_FUNCTION_EXISTS(malloc_usable_size HAVE_MALLOC_USABLE_SIZE)
if(HAVE_MALLOC_USABLE_SIZE)
  add_definitions(-DROCKSDB_MALLOC_USABLE_SIZE)
endif()

include_directories(${ROCKSDB_SOURCE_DIR})
include_directories(${ROCKSDB_SOURCE_DIR}/include)
include_directories(SYSTEM ${ROCKSDB_SOURCE_DIR}/third-party/gtest-1.7.0/fused-src)

find_package(Threads REQUIRED)
if(WIN32)
  set(SYSTEM_LIBS ${SYSTEM_LIBS} Shlwapi.lib Rpcrt4.lib)
else()
  set(SYSTEM_LIBS ${CMAKE_THREAD_LIBS_INIT} ${LIBRT})
endif()

set(ROCKSDB_LIBS rocksdblib})
set(LIBS ${ROCKSDB_LIBS} ${THIRDPARTY_LIBS} ${SYSTEM_LIBS})

#add_subdirectory(${ROCKSDB_SOURCE_DIR}/tools)

# Main library source code

set(ROCKSDB_SOURCES
        cache/clock_cache.cc
        cache/lru_cache.cc
        cache/sharded_cache.cc
        db/builder.cc
        db/c.cc
        db/column_family.cc
        db/compacted_db_impl.cc
        db/compaction.cc
        db/compaction_iterator.cc
        db/compaction_job.cc
        db/compaction_picker.cc
        db/compaction_picker_universal.cc
        db/convenience.cc
        db/db_filesnapshot.cc
        db/db_impl.cc
        db/db_impl_write.cc
        db/db_impl_compaction_flush.cc
        db/db_impl_files.cc
        db/db_impl_open.cc
        db/db_impl_debug.cc
        db/db_impl_experimental.cc
        db/db_impl_readonly.cc
        db/db_info_dumper.cc
        db/db_iter.cc
        db/dbformat.cc
        db/event_helpers.cc
        db/experimental.cc
        db/external_sst_file_ingestion_job.cc
        db/file_indexer.cc
        db/flush_job.cc
        db/flush_scheduler.cc
        db/forward_iterator.cc
        db/internal_stats.cc
        db/log_reader.cc
        db/log_writer.cc
        db/managed_iterator.cc
        db/memtable.cc
        db/memtable_list.cc
        db/merge_helper.cc
        db/merge_operator.cc
        db/range_del_aggregator.cc
        db/repair.cc
        db/snapshot_impl.cc
        db/table_cache.cc
        db/table_properties_collector.cc
        db/transaction_log_impl.cc
        db/version_builder.cc
        db/version_edit.cc
        db/version_set.cc
        db/wal_manager.cc
        db/write_batch.cc
        db/write_batch_base.cc
        db/write_controller.cc
        db/write_thread.cc
        env/env.cc
        env/env_chroot.cc
        env/env_hdfs.cc
        env/memenv.cc
        memtable/hash_cuckoo_rep.cc
        memtable/hash_linklist_rep.cc
        memtable/hash_skiplist_rep.cc
        memtable/memtable_allocator.cc
        memtable/skiplistrep.cc
        memtable/vectorrep.cc
        monitoring/histogram.cc
        monitoring/histogram_windowing.cc
        monitoring/instrumented_mutex.cc
        monitoring/iostats_context.cc
        monitoring/perf_context.cc
        monitoring/perf_level.cc
        monitoring/statistics.cc
        monitoring/thread_status_impl.cc
        monitoring/thread_status_updater.cc
        monitoring/thread_status_util.cc
        monitoring/thread_status_util_debug.cc
        options/cf_options.cc
        options/db_options.cc
        options/options.cc
        options/options_helper.cc
        options/options_parser.cc
        options/options_sanity_check.cc
        port/stack_trace.cc
        table/adaptive_table_factory.cc
        table/block.cc
        table/block_based_filter_block.cc
        table/block_based_table_builder.cc
        table/block_based_table_factory.cc
        table/block_based_table_reader.cc
        table/block_builder.cc
        table/block_prefix_index.cc
        table/bloom_block.cc
        table/cuckoo_table_builder.cc
        table/cuckoo_table_factory.cc
        table/cuckoo_table_reader.cc
        table/flush_block_policy.cc
        table/format.cc
        table/full_filter_block.cc
        table/get_context.cc
        table/index_builder.cc
        table/iterator.cc
        table/merging_iterator.cc
        table/meta_blocks.cc
        table/partitioned_filter_block.cc
        table/persistent_cache_helper.cc
        table/plain_table_builder.cc
        table/plain_table_factory.cc
        table/plain_table_index.cc
        table/plain_table_key_coding.cc
        table/plain_table_reader.cc
        table/sst_file_writer.cc
        table/table_properties.cc
        table/two_level_iterator.cc
        tools/db_bench_tool.cc
        tools/dump/db_dump_tool.cc
        tools/ldb_cmd.cc
        tools/ldb_tool.cc
        tools/sst_dump_tool.cc
        util/arena.cc
        util/auto_roll_logger.cc
        util/bloom.cc
        util/coding.cc
        util/compaction_job_stats_impl.cc
        util/comparator.cc
        util/concurrent_arena.cc
        util/crc32c.cc
        util/delete_scheduler.cc
        util/dynamic_bloom.cc
        util/event_logger.cc
        util/file_reader_writer.cc
        util/file_util.cc
        util/filename.cc
        util/filter_policy.cc
        util/hash.cc
        util/log_buffer.cc
        util/murmurhash.cc
        util/random.cc
        util/rate_limiter.cc
        util/slice.cc
        util/sst_file_manager_impl.cc
        util/status.cc
        util/status_message.cc
        util/string_util.cc
        util/sync_point.cc
        util/testutil.cc
        util/thread_local.cc
        util/threadpool_imp.cc
        util/transaction_test_util.cc
        util/xxhash.cc
        utilities/backupable/backupable_db.cc
        utilities/blob_db/blob_db.cc
        utilities/checkpoint/checkpoint_impl.cc
        utilities/col_buf_decoder.cc
        utilities/col_buf_encoder.cc
        utilities/column_aware_encoding_util.cc
        utilities/compaction_filters/remove_emptyvalue_compactionfilter.cc
        utilities/date_tiered/date_tiered_db_impl.cc
        utilities/document/document_db.cc
        utilities/document/json_document.cc
        utilities/document/json_document_builder.cc
        utilities/env_mirror.cc
        utilities/env_timed.cc
        utilities/geodb/geodb_impl.cc
        utilities/leveldb_options/leveldb_options.cc
        utilities/lua/rocks_lua_compaction_filter.cc
        utilities/memory/memory_util.cc
        utilities/merge_operators/max.cc
        utilities/merge_operators/put.cc
        utilities/merge_operators/string_append/stringappend.cc
        utilities/merge_operators/string_append/stringappend2.cc
        utilities/merge_operators/uint64add.cc
        utilities/option_change_migration/option_change_migration.cc
        utilities/options/options_util.cc
        utilities/persistent_cache/block_cache_tier.cc
        utilities/persistent_cache/block_cache_tier_file.cc
        utilities/persistent_cache/block_cache_tier_metadata.cc
        utilities/persistent_cache/persistent_cache_tier.cc
        utilities/persistent_cache/volatile_tier_impl.cc
        utilities/redis/redis_lists.cc
        utilities/simulator_cache/sim_cache.cc
        utilities/spatialdb/spatial_db.cc
        utilities/table_properties_collectors/compact_on_deletion_collector.cc
        utilities/transactions/optimistic_transaction_db_impl.cc
        utilities/transactions/optimistic_transaction_impl.cc
        utilities/transactions/transaction_base.cc
        utilities/transactions/transaction_db_impl.cc
        utilities/transactions/transaction_db_mutex_impl.cc
        utilities/transactions/transaction_impl.cc
        utilities/transactions/transaction_lock_mgr.cc
        utilities/transactions/transaction_util.cc
        utilities/ttl/db_ttl_impl.cc
        utilities/write_batch_with_index/write_batch_with_index.cc
        utilities/write_batch_with_index/write_batch_with_index_internal.cc
)

if(WIN32)
  list(APPEND ROCKSDB_SOURCES
    port/win/io_win.cc
    port/win/env_win.cc
    port/win/env_default.cc
    port/win/port_win.cc
    port/win/win_logger.cc
    port/win/win_thread.cc
    port/win/xpress_win.cc)
else()
  list(APPEND ROCKSDB_SOURCES
    port/port_posix.cc
    env/env_posix.cc
    env/io_posix.cc)
endif()
SET(SOURCES)
FOREACH(s ${ROCKSDB_SOURCES})
  list(APPEND SOURCES ${ROCKSDB_SOURCE_DIR}/${s})
ENDFOREACH()

IF(CMAKE_VERSION VERSION_GREATER "2.8.10")
  STRING(TIMESTAMP GIT_DATE_TIME "%Y-%m-%d %H:%M:%S")
ENDIF()

CONFIGURE_FILE(${ROCKSDB_SOURCE_DIR}/util/build_version.cc.in build_version.cc @ONLY)
INCLUDE_DIRECTORIES(${ROCKSDB_SOURCE_DIR}/util)
list(APPEND SOURCES ${CMAKE_CURRENT_BINARY_DIR}/build_version.cc)

ADD_CONVENIENCE_LIBRARY(rocksdblib ${SOURCES})
target_link_libraries(rocksdblib ${THIRDPARTY_LIBS} ${SYSTEM_LIBS})
IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_target_properties(rocksdblib PROPERTIES COMPILE_FLAGS "-fPIC -fno-builtin-memcmp -frtti")
endif()
