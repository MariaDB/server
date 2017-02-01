
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
  option(WITH_ROCKSDB_SNAPPY "build RocksDB with SNAPPY" OFF)
  if(WITH_ROCKSDB_SNAPPY)
    find_package(snappy REQUIRED)
    add_definitions(-DSNAPPY)
    include_directories(${SNAPPY_INCLUDE_DIR})
    list(APPEND THIRDPARTY_LIBS ${SNAPPY_LIBRARIES})
  endif()
endif()





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
  set(SYSTEM_LIBS ${CMAKE_THREAD_LIBS_INIT})
endif()

set(ROCKSDB_LIBS rocksdblib})
set(LIBS ${ROCKSDB_LIBS} ${THIRDPARTY_LIBS} ${SYSTEM_LIBS})

#add_subdirectory(${ROCKSDB_SOURCE_DIR}/tools)

# Main library source code

set(ROCKSDB_SOURCES
        db/auto_roll_logger.cc
        db/builder.cc
        db/c.cc
        db/column_family.cc
        db/compacted_db_impl.cc
        db/compaction.cc
        db/compaction_iterator.cc
        db/compaction_job.cc
        db/compaction_picker.cc
        db/convenience.cc
        db/dbformat.cc
        db/db_filesnapshot.cc
        db/db_impl.cc
        db/db_impl_debug.cc
        db/db_impl_experimental.cc
        db/db_impl_readonly.cc
        db/db_info_dumper.cc
        db/db_iter.cc
        db/event_helpers.cc
        db/external_sst_file_ingestion_job.cc
        db/experimental.cc
        db/filename.cc
        db/file_indexer.cc
        db/flush_job.cc
        db/flush_scheduler.cc
        db/forward_iterator.cc
        db/internal_stats.cc
        db/log_reader.cc
        db/log_writer.cc
        db/managed_iterator.cc
        db/memtable.cc
        db/memtable_allocator.cc
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
        db/xfunc_test_points.cc
        memtable/hash_cuckoo_rep.cc
        memtable/hash_linklist_rep.cc
        memtable/hash_skiplist_rep.cc
        memtable/skiplistrep.cc
        memtable/vectorrep.cc
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
        table/iterator.cc
        table/merger.cc
        table/sst_file_writer.cc
        table/meta_blocks.cc
        table/plain_table_builder.cc
        table/plain_table_factory.cc
        table/plain_table_index.cc
        table/plain_table_key_coding.cc
        table/plain_table_reader.cc
        table/persistent_cache_helper.cc
        table/table_properties.cc
        table/two_level_iterator.cc
        tools/sst_dump_tool.cc
        tools/db_bench_tool.cc
        tools/dump/db_dump_tool.cc
        util/arena.cc
        util/bloom.cc
        util/cf_options.cc
        util/clock_cache.cc
        util/coding.cc
        util/compaction_job_stats_impl.cc
        util/comparator.cc
        util/concurrent_arena.cc
        util/crc32c.cc
        util/db_options.cc
        util/delete_scheduler.cc
        util/dynamic_bloom.cc
        util/env.cc
        util/env_chroot.cc
        util/env_hdfs.cc
        util/event_logger.cc
        util/file_util.cc
        util/file_reader_writer.cc
        util/sst_file_manager_impl.cc
        util/filter_policy.cc
        util/hash.cc
        util/histogram.cc
        util/histogram_windowing.cc
        util/instrumented_mutex.cc
        util/iostats_context.cc
        util/lru_cache.cc
        tools/ldb_cmd.cc
        tools/ldb_tool.cc
        util/logging.cc
        util/log_buffer.cc
        util/memenv.cc
        util/murmurhash.cc
        util/options.cc
        util/options_helper.cc
        util/options_parser.cc
        util/options_sanity_check.cc
        util/perf_context.cc
        util/perf_level.cc
        util/random.cc
        util/rate_limiter.cc
        util/sharded_cache.cc
        util/slice.cc
        util/statistics.cc
        util/status.cc
        util/status_message.cc
        util/string_util.cc
        util/sync_point.cc
        util/testutil.cc
        util/thread_local.cc
        util/threadpool_imp.cc
        util/thread_status_impl.cc
        util/thread_status_updater.cc
        util/thread_status_util.cc
        util/thread_status_util_debug.cc
        util/transaction_test_util.cc
        util/xfunc.cc
        util/xxhash.cc
        utilities/backupable/backupable_db.cc
        utilities/blob_db/blob_db.cc
        utilities/checkpoint/checkpoint.cc
        utilities/compaction_filters/remove_emptyvalue_compactionfilter.cc
        utilities/date_tiered/date_tiered_db_impl.cc
        utilities/document/document_db.cc
        utilities/document/json_document.cc
        utilities/document/json_document_builder.cc
        utilities/env_mirror.cc
        utilities/env_registry.cc
        utilities/geodb/geodb_impl.cc
        utilities/leveldb_options/leveldb_options.cc
        utilities/lua/rocks_lua_compaction_filter.cc
        utilities/memory/memory_util.cc
        utilities/merge_operators/string_append/stringappend.cc
        utilities/merge_operators/string_append/stringappend2.cc
        utilities/merge_operators/put.cc
        utilities/merge_operators/max.cc
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
        utilities/transactions/optimistic_transaction_impl.cc
        utilities/transactions/optimistic_transaction_db_impl.cc
        utilities/transactions/transaction_base.cc
        utilities/transactions/transaction_impl.cc
        utilities/transactions/transaction_db_impl.cc
        utilities/transactions/transaction_db_mutex_impl.cc
        utilities/transactions/transaction_lock_mgr.cc
        utilities/transactions/transaction_util.cc
        utilities/ttl/db_ttl_impl.cc
        utilities/write_batch_with_index/write_batch_with_index.cc
        utilities/write_batch_with_index/write_batch_with_index_internal.cc
        utilities/col_buf_encoder.cc
        utilities/col_buf_decoder.cc
        utilities/column_aware_encoding_util.cc
)

if(WIN32)
  list(APPEND ROCKSDB_SOURCES
    port/win/io_win.cc
    port/win/env_win.cc
    port/win/env_default.cc
    port/win/port_win.cc
    port/win/win_logger.cc
    port/win/xpress_win.cc)
else()
  list(APPEND ROCKSDB_SOURCES
    port/port_posix.cc
    util/env_posix.cc
    util/io_posix.cc)
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

ADD_CONVENIENCE_LIBRARY(rocksdblib STATIC ${SOURCES})
target_link_libraries(rocksdblib ${THIRDPARTY_LIBS} ${SYSTEM_LIBS})
if(CMAKE_COMPILER_IS_GNUCXX)
  set_target_properties(rocksdblib PROPERTIES COMPILE_FLAGS "-fno-builtin-memcmp -frtti")
endif()
