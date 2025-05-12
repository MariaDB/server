
if(POLICY CMP0042)
  cmake_policy(SET CMP0042 NEW)
endif()

SET(ROCKSDB_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rocksdb)

INCLUDE_DIRECTORIES(
  ${CMAKE_CURRENT_BINARY_DIR}
  ${ROCKSDB_SOURCE_DIR}
  ${ROCKSDB_SOURCE_DIR}/include
  ${ROCKSDB_SOURCE_DIR}/third-party/gtest-1.7.0/fused-src
)

list(APPEND CMAKE_MODULE_PATH "${ROCKSDB_SOURCE_DIR}/cmake/modules/")

if(WIN32)
  # include(${ROCKSDB_SOURCE_DIR}/thirdparty.inc)
else()
  option(WITH_ROCKSDB_JEMALLOC "build RocksDB with JeMalloc" OFF)
  if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # FreeBSD has jemaloc as default malloc
    add_definitions(-DROCKSDB_JEMALLOC)
    ADD_DEFINITIONS(-DROCKSDB_MALLOC_USABLE_SIZE)
    set(WITH_JEMALLOC ON)
  elseif(WITH_ROCKSDB_JEMALLOC)
    find_package(JeMalloc REQUIRED)
    add_definitions(-DROCKSDB_JEMALLOC)
    ADD_DEFINITIONS(-DROCKSDB_MALLOC_USABLE_SIZE)
    include_directories(${JEMALLOC_INCLUDE_DIR})
  endif()
endif()


# Optional compression libraries.

include(CheckFunctionExists)
macro(check_lib package)
  SET(WITH_ROCKSDB_${package} AUTO CACHE STRING
        "Build RocksDB  with ${package} compression. Possible values are 'ON', 'OFF', 'AUTO' and default is 'AUTO'")

  STRING(TOUPPER ${package} var)
  IF (NOT ${WITH_ROCKSDB_${package}} STREQUAL "OFF")
    FIND_PACKAGE(${package} QUIET)
    SET(HAVE_ROCKSDB_${package} TRUE)
    IF (${${package}_FOUND})
      IF(${ARGC} GREATER 1)
        SET(CMAKE_REQUIRED_LIBRARIES ${${var}_LIBRARIES})
        CHECK_FUNCTION_EXISTS(${ARGV1} ${package}_VALID)
        UNSET(CMAKE_REQUIRED_LIBRARIES)
      ELSE()
        SET(${package}_VALID TRUE)
      ENDIF()
    ENDIF()
  ENDIF()
  ADD_FEATURE_INFO(ROCKSDB_${package} HAVE_ROCKSDB_${package} "${package} Compression in the RocksDB storage engine")

  IF(${${package}_VALID})
    MESSAGE_ONCE(rocksdb_${package} "Found ${package}: ${${var}_LIBRARIES}")
    add_definitions(-D${var})
    include_directories(${${var}_INCLUDE_DIR})
    list(APPEND THIRDPARTY_LIBS ${${var}_LIBRARIES})
  ELSEIF(${${package}_FOUND})
    MESSAGE_ONCE(rocksdb_${package} "Found unusable ${package}: ${${var}_LIBRARIES} [${ARGV1}]")
  ELSE()
    MESSAGE_ONCE(rocksdb_${package} "Could NOT find ${package}")
  ENDIF()

  IF (${WITH_ROCKSDB_${package}} STREQUAL "ON"  AND NOT ${${package}_FOUND})
    MESSAGE(FATAL_ERROR
      "${package} library was not found, but WITH_ROCKSDB_${package} option is ON.\
      Either set WITH_ROCKSDB_${package} to OFF, or make sure ${package} is installed")
  endif()
endmacro()

check_lib(LZ4)
check_lib(BZip2)
check_lib(Snappy)
check_lib(ZSTD ZDICT_trainFromBuffer)

add_definitions(-DZLIB)
list(APPEND THIRDPARTY_LIBS ${ZLIB_LIBRARIES})
ADD_FEATURE_INFO(ROCKSDB_ZLIB "ON" "zlib Compression in the RocksDB storage engine")

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

include(CheckCCompilerFlag)
# ppc64 or ppc64le or powerpc64 (BSD)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64")
  CHECK_C_COMPILER_FLAG("-maltivec" HAS_ALTIVEC)
  if(HAS_ALTIVEC)
    message(STATUS " HAS_ALTIVEC yes")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -maltivec")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -maltivec")
  endif(HAS_ALTIVEC)
  if(NOT CMAKE_C_FLAGS MATCHES "m(cpu|tune)")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=power8")
  endif()
  if(NOT CMAKE_CXX_FLAGS MATCHES "m(cpu|tune)")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=power8")
  endif()
  ADD_DEFINITIONS(-DHAVE_POWER8 -DHAS_ALTIVEC)
endif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64")

option(WITH_FALLOCATE "build with fallocate" ON)

if(WITH_FALLOCATE AND UNIX)
  include(CheckCSourceCompiles)
  CHECK_C_SOURCE_COMPILES("
#define _GNU_SOURCE
#include <fcntl.h>
int main() {
 int fd = open(\"/dev/null\", 0);
 fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, 0, 1024);
}
" HAVE_FALLOCATE)
  if(HAVE_FALLOCATE)
    add_definitions(-DROCKSDB_FALLOCATE_PRESENT)
  endif()
endif()

CHECK_FUNCTION_EXISTS(malloc_usable_size HAVE_MALLOC_USABLE_SIZE)
if(HAVE_MALLOC_USABLE_SIZE)
  add_definitions(-DROCKSDB_MALLOC_USABLE_SIZE)
endif()

include_directories(${ROCKSDB_SOURCE_DIR})
include_directories(${ROCKSDB_SOURCE_DIR}/include)
include_directories(SYSTEM ${ROCKSDB_SOURCE_DIR}/third-party/gtest-1.7.0/fused-src)

find_package(Threads REQUIRED)
if(WIN32)
  set(SYSTEM_LIBS ${SYSTEM_LIBS} ${ATOMIC_EXTRA_LIBS} Shlwapi.lib Rpcrt4.lib)
else()
  set(SYSTEM_LIBS ${SYSTEM_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBRT} ${CMAKE_DL_LIBS} ${ATOMIC_EXTRA_LIBS})
endif()

set(ROCKSDB_LIBS rocksdblib})
set(LIBS ${ROCKSDB_LIBS} ${THIRDPARTY_LIBS} ${SYSTEM_LIBS})

#add_subdirectory(${ROCKSDB_SOURCE_DIR}/tools)

# Main library source code
#  Note : RocksDB has a lot of unittests. We should not include these files
#  in the build, because 1. they are not needed and 2. gtest causes warnings
#  in windows build, which are treated as errors and cause the build to fail.
#
#  Unit tests themselves:
#  - *_test.cc
#  - *_bench.cc
set(ROCKSDB_SOURCES
        cache/cache.cc
        cache/cache_entry_roles.cc
        cache/cache_key.cc
        cache/cache_reservation_manager.cc
        cache/clock_cache.cc
        cache/lru_cache.cc
        cache/sharded_cache.cc
        db/arena_wrapped_db_iter.cc
        db/blob/blob_fetcher.cc
        db/blob/blob_file_addition.cc
        db/blob/blob_file_builder.cc
        db/blob/blob_file_builder.cc
        db/blob/blob_file_cache.cc
        db/blob/blob_file_garbage.cc
        db/blob/blob_file_meta.cc
        db/blob/blob_file_reader.cc
        db/blob/blob_garbage_meter.cc
        db/blob/blob_log_format.cc
        db/blob/blob_log_sequential_reader.cc
        db/blob/blob_log_writer.cc
        db/blob/prefetch_buffer_collection.cc
        db/builder.cc
        db/c.cc
        db/column_family.cc
        db/compaction/compaction.cc
        db/compaction/compaction_iterator.cc
        db/compaction/compaction_job.cc
        db/compaction/compaction_picker.cc
        db/compaction/compaction_picker_fifo.cc
        db/compaction/compaction_picker_level.cc
        db/compaction/compaction_picker_universal.cc
        db/compaction/sst_partitioner.cc
        db/convenience.cc
        db/db_filesnapshot.cc
        db/dbformat.cc
        db/db_impl/compacted_db_impl.cc
        db/db_impl/db_impl.cc
        db/db_impl/db_impl_compaction_flush.cc
        db/db_impl/db_impl_debug.cc
        db/db_impl/db_impl_experimental.cc
        db/db_impl/db_impl_files.cc
        db/db_impl/db_impl_open.cc
        db/db_impl/db_impl_readonly.cc
        db/db_impl/db_impl_secondary.cc
        db/db_impl/db_impl_write.cc
        db/db_info_dumper.cc
        db/db_iter.cc
        db/error_handler.cc
        db/event_helpers.cc
        db/experimental.cc
        db/external_sst_file_ingestion_job.cc
        db/file_indexer.cc
        db/flush_job.cc
        db/flush_scheduler.cc
        db/forward_iterator.cc
        db/import_column_family_job.cc
        db/internal_stats.cc
        db/log_reader.cc
        db/logs_with_prep_tracker.cc
        db/log_writer.cc
        db/malloc_stats.cc
        db/memtable.cc
        db/memtable_list.cc
        db/merge_helper.cc
        db/merge_operator.cc
        db/output_validator.cc
        db/periodic_work_scheduler.cc
        db/range_del_aggregator.cc
        db/range_tombstone_fragmenter.cc
        db/repair.cc
        db/snapshot_impl.cc
        db/table_cache.cc
        db/table_properties_collector.cc
        db/transaction_log_impl.cc
        db/trim_history_scheduler.cc
        db/version_builder.cc
        db/version_edit.cc
        db/version_edit_handler.cc
        db/version_set.cc
        db/wal_edit.cc
        db/wal_manager.cc
        db/write_batch_base.cc
        db/write_batch.cc
        db/write_controller.cc
        db/write_thread.cc
        env/composite_env.cc
        env/env.cc
        env/env_chroot.cc
        env/env_encryption.cc
        env/env_hdfs.cc
        env/file_system.cc
        env/file_system_tracer.cc
        env/fs_remap.cc
        env/mock_env.cc
        env/unique_id_gen.cc
        file/delete_scheduler.cc
        file/filename.cc
        file/file_prefetch_buffer.cc
        file/file_util.cc
        file/line_file_reader.cc
        file/random_access_file_reader.cc
        file/readahead_raf.cc
        file/read_write_util.cc
        file/sequence_file_reader.cc
        file/sst_file_manager_impl.cc
        file/writable_file_writer.cc
        logging/auto_roll_logger.cc
        logging/event_logger.cc
        logging/log_buffer.cc
        memory/arena.cc
        memory/concurrent_arena.cc
        memory/jemalloc_nodump_allocator.cc
        memtable/alloc_tracker.cc
        memtable/hash_linklist_rep.cc
        memtable/hash_skiplist_rep.cc
        memtable/skiplistrep.cc
        memtable/vectorrep.cc
        memtable/write_buffer_manager.cc
        monitoring/histogram.cc
        monitoring/histogram_windowing.cc
        monitoring/in_memory_stats_history.cc
        monitoring/instrumented_mutex.cc
        monitoring/iostats_context.cc
        monitoring/perf_context.cc
        monitoring/perf_level.cc
        monitoring/persistent_stats_history.cc
        monitoring/statistics.cc
        monitoring/thread_status_impl.cc
        monitoring/thread_status_updater.cc
        monitoring/thread_status_util.cc
        monitoring/thread_status_util_debug.cc
        options/cf_options.cc
        options/configurable.cc
        options/customizable.cc
        options/db_options.cc
        options/options.cc
        options/options_helper.cc
        options/options_parser.cc
        port/stack_trace.cc
        table/adaptive/adaptive_table_factory.cc
        table/block_based/binary_search_index_reader.cc
        table/block_based/block_based_filter_block.cc
        table/block_based/block_based_table_builder.cc
        table/block_based/block_based_table_factory.cc
        table/block_based/block_based_table_iterator.cc
        table/block_based/block_based_table_reader.cc
        table/block_based/block_builder.cc
        table/block_based/block.cc
        table/block_based/block_prefetcher.cc
        table/block_based/block_prefix_index.cc
        table/block_based/data_block_footer.cc
        table/block_based/data_block_hash_index.cc
        table/block_based/filter_block_reader_common.cc
        table/block_based/filter_policy.cc
        table/block_based/flush_block_policy.cc
        table/block_based/full_filter_block.cc
        table/block_based/hash_index_reader.cc
        table/block_based/index_builder.cc
        table/block_based/index_reader_common.cc
        table/block_based/parsed_full_filter_block.cc
        table/block_based/partitioned_filter_block.cc
        table/block_based/partitioned_index_iterator.cc
        table/block_based/partitioned_index_reader.cc
        table/block_based/reader_common.cc
        table/block_based/uncompression_dict_reader.cc
        table/block_fetcher.cc
        table/cuckoo/cuckoo_table_builder.cc
        table/cuckoo/cuckoo_table_factory.cc
        table/cuckoo/cuckoo_table_reader.cc
        table/format.cc
        table/get_context.cc
        table/iterator.cc
        table/merging_iterator.cc
        table/meta_blocks.cc
        table/persistent_cache_helper.cc
        table/plain/plain_table_bloom.cc
        table/plain/plain_table_builder.cc
        table/plain/plain_table_factory.cc
        table/plain/plain_table_index.cc
        table/plain/plain_table_key_coding.cc
        table/plain/plain_table_reader.cc
        table/sst_file_dumper.cc
        table/sst_file_reader.cc
        table/sst_file_writer.cc
        table/table_factory.cc
        table/table_properties.cc
        table/two_level_iterator.cc
        table/unique_id.cc
        test_util/sync_point.cc
        test_util/sync_point_impl.cc
        test_util/testutil.cc
        test_util/transaction_test_util.cc
        tools/block_cache_analyzer/block_cache_trace_analyzer.cc
        tools/dump/db_dump_tool.cc
        tools/ldb_cmd.cc
        tools/ldb_tool.cc
        tools/sst_dump_tool.cc
        tools/trace_analyzer_tool.cc
        trace_replay/block_cache_tracer.cc
        trace_replay/io_tracer.cc
        trace_replay/trace_record.cc
        trace_replay/trace_record_handler.cc
        trace_replay/trace_record_result.cc
        trace_replay/trace_replay.cc
        util/coding.cc
        util/compaction_job_stats_impl.cc
        util/comparator.cc
        util/compression_context_cache.cc
        util/concurrent_task_limiter_impl.cc
        util/crc32c.cc
        util/dynamic_bloom.cc
        util/file_checksum_helper.cc
        util/hash.cc
        utilities/backupable/backupable_db.cc
        utilities/blob_db/blob_compaction_filter.cc
        utilities/blob_db/blob_db.cc
        utilities/blob_db/blob_db_impl.cc
        utilities/blob_db/blob_db_impl_filesnapshot.cc
        utilities/blob_db/blob_dump_tool.cc
        utilities/blob_db/blob_file.cc
        utilities/checkpoint/checkpoint_impl.cc
        utilities/compaction_filters.cc
        utilities/compaction_filters/remove_emptyvalue_compactionfilter.cc
        utilities/debug.cc
        utilities/env_mirror.cc
        utilities/env_timed.cc
        utilities/leveldb_options/leveldb_options.cc
        utilities/memory/memory_util.cc
        utilities/merge_operators/bytesxor.cc
        utilities/merge_operators.cc
        utilities/merge_operators/max.cc
        utilities/merge_operators/put.cc
        utilities/merge_operators/sortlist.cc
        utilities/merge_operators/string_append/stringappend2.cc
        utilities/merge_operators/string_append/stringappend.cc
        utilities/merge_operators/uint64add.cc
        utilities/object_registry.cc
        utilities/option_change_migration/option_change_migration.cc
        utilities/options/options_util.cc
        utilities/persistent_cache/block_cache_tier.cc
        utilities/persistent_cache/block_cache_tier_file.cc
        utilities/persistent_cache/block_cache_tier_metadata.cc
        utilities/persistent_cache/persistent_cache_tier.cc
        utilities/persistent_cache/volatile_tier_impl.cc
        utilities/simulator_cache/cache_simulator.cc
        utilities/simulator_cache/sim_cache.cc
        utilities/table_properties_collectors/compact_on_deletion_collector.cc
        utilities/trace/file_trace_reader_writer.cc
        utilities/trace/replayer_impl.cc
        utilities/transactions/lock/lock_manager.cc
        utilities/transactions/lock/point/point_lock_manager.cc
        utilities/transactions/lock/point/point_lock_tracker.cc
        utilities/transactions/optimistic_transaction.cc
        utilities/transactions/optimistic_transaction_db_impl.cc
        utilities/transactions/pessimistic_transaction.cc
        utilities/transactions/pessimistic_transaction_db.cc
        utilities/transactions/snapshot_checker.cc
        utilities/transactions/transaction_base.cc
        utilities/transactions/transaction_db_mutex_impl.cc
        utilities/transactions/transaction_util.cc
        utilities/transactions/write_prepared_txn.cc
        utilities/transactions/write_prepared_txn_db.cc
        utilities/transactions/write_unprepared_txn.cc
        utilities/transactions/write_unprepared_txn_db.cc
        utilities/ttl/db_ttl_impl.cc
        utilities/wal_filter.cc
        utilities/write_batch_with_index/write_batch_with_index.cc
        utilities/write_batch_with_index/write_batch_with_index_internal.cc
        util/murmurhash.cc
        util/random.cc
        util/rate_limiter.cc
        util/regex.cc
        util/ribbon_config.cc
        util/slice.cc
        util/status.cc
        util/string_util.cc
        util/thread_local.cc
        util/threadpool_imp.cc
        util/xxhash.cc
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
    env/io_posix.cc
    env/fs_posix.cc)
  # ppc64 or ppc64le
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64")
    enable_language(ASM)
    list(APPEND ROCKSDB_SOURCES
      util/crc32c_ppc.c
      util/crc32c_ppc_asm.S)
  endif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64")
  # aarch
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64")
    INCLUDE(CheckCXXCompilerFlag)
    CHECK_CXX_COMPILER_FLAG("-march=armv8-a+crc+crypto" HAS_ARMV8_CRC)
    if(HAS_ARMV8_CRC)
      message(STATUS " HAS_ARMV8_CRC yes")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+crc+crypto -Wno-unused-function")
      list(APPEND ROCKSDB_SOURCES
        util/crc32c_arm64.cc)
    endif(HAS_ARMV8_CRC)
  endif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64")
endif()
SET(SOURCES)
FOREACH(s ${ROCKSDB_SOURCES})
  list(APPEND SOURCES ${ROCKSDB_SOURCE_DIR}/${s})
ENDFOREACH()

if(MSVC)
  add_definitions(-DHAVE_SSE42 -DHAVE_PCLMUL)
  # Workaround broken compilation with -DWIN32_LEAN_AND_MEAN
  # (https://github.com/facebook/rocksdb/issues/4344)
  set_source_files_properties(${ROCKSDB_SOURCE_DIR}/port/win/env_win.cc
      PROPERTIES COMPILE_FLAGS "/FI\"windows.h\" /FI\"winioctl.h\"")

  # Workaround Win8.1 SDK bug, that breaks /permissive-
  string(REPLACE "/permissive-" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
else()
  set(CMAKE_REQUIRED_FLAGS "-msse4.2 -mpclmul ${CXX11_FLAGS}")

  CHECK_CXX_SOURCE_COMPILES("
#include <cstdint>
#include <nmmintrin.h>
#include <wmmintrin.h>
int main() {
  volatile uint32_t x = _mm_crc32_u32(0, 0);
  const auto a = _mm_set_epi64x(0, 0);
  const auto b = _mm_set_epi64x(0, 0);
  const auto c = _mm_clmulepi64_si128(a, b, 0x00);
  auto d = _mm_cvtsi128_si64(c);
}
" HAVE_SSE42)
  if(HAVE_SSE42)
    set_source_files_properties(${ROCKSDB_SOURCE_DIR}/util/crc32c.cc
      PROPERTIES COMPILE_FLAGS "-DHAVE_SSE42 -DHAVE_PCLMUL -msse4.2 -mpclmul")
  endif()
  unset(CMAKE_REQUIRED_FLAGS)
endif()

IF(CMAKE_VERSION VERSION_GREATER "2.8.10")
  STRING(TIMESTAMP GIT_DATE_TIME "%Y-%m-%d %H:%M:%S")
ENDIF()

# psergey-added:
SET(GIT_MOD 0)
CONFIGURE_FILE(${ROCKSDB_SOURCE_DIR}/util/build_version.cc.in build_version.cc @ONLY)

list(APPEND SOURCES ${CMAKE_CURRENT_BINARY_DIR}/build_version.cc)

ADD_CONVENIENCE_LIBRARY(rocksdblib ${SOURCES})
target_link_libraries(rocksdblib ${THIRDPARTY_LIBS} ${SYSTEM_LIBS})
IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_target_properties(rocksdblib PROPERTIES COMPILE_FLAGS "-fPIC -fno-builtin-memcmp -Wno-error")
endif()
