/*
   Portions Copyright (c) 2015-Present, Facebook, Inc.
   Portions Copyright (c) 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <my_config.h>

#include "rdb_mariadb_port.h"
/* This C++ file's header file */
#include "./rdb_perf_context.h"

/* C++ system header files */
#include <string>

/* RocksDB header files */
#include "rocksdb/iostats_context.h"
#include "rocksdb/perf_context.h"

/* MyRocks header files */
#include "./ha_rocksdb_proto.h"

namespace myrocks {

// To add a new metric:
//   1. Update the PC enum in rdb_perf_context.h
//   2. Update sections (A), (B), and (C) below
//   3. Update perf_context.test and show_engine.test

std::string rdb_pc_stat_types[] = {
    // (A) These should be in the same order as the PC enum
    "USER_KEY_COMPARISON_COUNT",
    "BLOCK_CACHE_HIT_COUNT",
    "BLOCK_READ_COUNT",
    "BLOCK_READ_BYTE",
    "BLOCK_READ_TIME",
    "BLOCK_CHECKSUM_TIME",
    "BLOCK_DECOMPRESS_TIME",
    "GET_READ_BYTES",
    "MULTIGET_READ_BYTES",
    "ITER_READ_BYTES",
    "INTERNAL_KEY_SKIPPED_COUNT",
    "INTERNAL_DELETE_SKIPPED_COUNT",
    "INTERNAL_RECENT_SKIPPED_COUNT",
    "INTERNAL_MERGE_COUNT",
    "GET_SNAPSHOT_TIME",
    "GET_FROM_MEMTABLE_TIME",
    "GET_FROM_MEMTABLE_COUNT",
    "GET_POST_PROCESS_TIME",
    "GET_FROM_OUTPUT_FILES_TIME",
    "SEEK_ON_MEMTABLE_TIME",
    "SEEK_ON_MEMTABLE_COUNT",
    "NEXT_ON_MEMTABLE_COUNT",
    "PREV_ON_MEMTABLE_COUNT",
    "SEEK_CHILD_SEEK_TIME",
    "SEEK_CHILD_SEEK_COUNT",
    "SEEK_MIN_HEAP_TIME",
    "SEEK_MAX_HEAP_TIME",
    "SEEK_INTERNAL_SEEK_TIME",
    "FIND_NEXT_USER_ENTRY_TIME",
    "WRITE_WAL_TIME",
    "WRITE_MEMTABLE_TIME",
    "WRITE_DELAY_TIME",
    "WRITE_PRE_AND_POST_PROCESS_TIME",
    "DB_MUTEX_LOCK_NANOS",
    "DB_CONDITION_WAIT_NANOS",
    "MERGE_OPERATOR_TIME_NANOS",
    "READ_INDEX_BLOCK_NANOS",
    "READ_FILTER_BLOCK_NANOS",
    "NEW_TABLE_BLOCK_ITER_NANOS",
    "NEW_TABLE_ITERATOR_NANOS",
    "BLOCK_SEEK_NANOS",
    "FIND_TABLE_NANOS",
    "BLOOM_MEMTABLE_HIT_COUNT",
    "BLOOM_MEMTABLE_MISS_COUNT",
    "BLOOM_SST_HIT_COUNT",
    "BLOOM_SST_MISS_COUNT",
    "KEY_LOCK_WAIT_TIME",
    "KEY_LOCK_WAIT_COUNT",
    "IO_THREAD_POOL_ID",
    "IO_BYTES_WRITTEN",
    "IO_BYTES_READ",
    "IO_OPEN_NANOS",
    "IO_ALLOCATE_NANOS",
    "IO_WRITE_NANOS",
    "IO_READ_NANOS",
    "IO_RANGE_SYNC_NANOS",
    "IO_LOGGER_NANOS"};

#define IO_PERF_RECORD(_field_)                                       \
  do {                                                                \
    if (rocksdb::get_perf_context()->_field_ > 0) {                   \
      counters->m_value[idx] += rocksdb::get_perf_context()->_field_; \
    }                                                                 \
    idx++;                                                            \
  } while (0)
#define IO_STAT_RECORD(_field_)                                          \
  do {                                                                   \
    if (rocksdb::get_iostats_context()->_field_ > 0) {                   \
      counters->m_value[idx] += rocksdb::get_iostats_context()->_field_; \
    }                                                                    \
    idx++;                                                               \
  } while (0)

static void harvest_diffs(Rdb_atomic_perf_counters *const counters) {
  // (C) These should be in the same order as the PC enum
  size_t idx = 0;
  IO_PERF_RECORD(user_key_comparison_count);
  IO_PERF_RECORD(block_cache_hit_count);
  IO_PERF_RECORD(block_read_count);
  IO_PERF_RECORD(block_read_byte);
  IO_PERF_RECORD(block_read_time);
  IO_PERF_RECORD(block_checksum_time);
  IO_PERF_RECORD(block_decompress_time);
  IO_PERF_RECORD(get_read_bytes);
  IO_PERF_RECORD(multiget_read_bytes);
  IO_PERF_RECORD(iter_read_bytes);
  IO_PERF_RECORD(internal_key_skipped_count);
  IO_PERF_RECORD(internal_delete_skipped_count);
  IO_PERF_RECORD(internal_recent_skipped_count);
  IO_PERF_RECORD(internal_merge_count);
  IO_PERF_RECORD(get_snapshot_time);
  IO_PERF_RECORD(get_from_memtable_time);
  IO_PERF_RECORD(get_from_memtable_count);
  IO_PERF_RECORD(get_post_process_time);
  IO_PERF_RECORD(get_from_output_files_time);
  IO_PERF_RECORD(seek_on_memtable_time);
  IO_PERF_RECORD(seek_on_memtable_count);
  IO_PERF_RECORD(next_on_memtable_count);
  IO_PERF_RECORD(prev_on_memtable_count);
  IO_PERF_RECORD(seek_child_seek_time);
  IO_PERF_RECORD(seek_child_seek_count);
  IO_PERF_RECORD(seek_min_heap_time);
  IO_PERF_RECORD(seek_max_heap_time);
  IO_PERF_RECORD(seek_internal_seek_time);
  IO_PERF_RECORD(find_next_user_entry_time);
  IO_PERF_RECORD(write_wal_time);
  IO_PERF_RECORD(write_memtable_time);
  IO_PERF_RECORD(write_delay_time);
  IO_PERF_RECORD(write_pre_and_post_process_time);
  IO_PERF_RECORD(db_mutex_lock_nanos);
  IO_PERF_RECORD(db_condition_wait_nanos);
  IO_PERF_RECORD(merge_operator_time_nanos);
  IO_PERF_RECORD(read_index_block_nanos);
  IO_PERF_RECORD(read_filter_block_nanos);
  IO_PERF_RECORD(new_table_block_iter_nanos);
  IO_PERF_RECORD(new_table_iterator_nanos);
  IO_PERF_RECORD(block_seek_nanos);
  IO_PERF_RECORD(find_table_nanos);
  IO_PERF_RECORD(bloom_memtable_hit_count);
  IO_PERF_RECORD(bloom_memtable_miss_count);
  IO_PERF_RECORD(bloom_sst_hit_count);
  IO_PERF_RECORD(bloom_sst_miss_count);
  IO_PERF_RECORD(key_lock_wait_time);
  IO_PERF_RECORD(key_lock_wait_count);

  IO_STAT_RECORD(thread_pool_id);
  IO_STAT_RECORD(bytes_written);
  IO_STAT_RECORD(bytes_read);
  IO_STAT_RECORD(open_nanos);
  IO_STAT_RECORD(allocate_nanos);
  IO_STAT_RECORD(write_nanos);
  IO_STAT_RECORD(read_nanos);
  IO_STAT_RECORD(range_sync_nanos);
  IO_STAT_RECORD(logger_nanos);
}

#undef IO_PERF_DIFF
#undef IO_STAT_DIFF

static Rdb_atomic_perf_counters rdb_global_perf_counters;

void rdb_get_global_perf_counters(Rdb_perf_counters *const counters) {
  counters->load(rdb_global_perf_counters);
}

void Rdb_perf_counters::load(const Rdb_atomic_perf_counters &atomic_counters) {
  for (int i = 0; i < PC_MAX_IDX; i++) {
    m_value[i] = atomic_counters.m_value[i].load(std::memory_order_relaxed);
  }
}

bool Rdb_io_perf::start(const uint32_t perf_context_level) {
  const rocksdb::PerfLevel perf_level =
      static_cast<rocksdb::PerfLevel>(perf_context_level);

  if (rocksdb::GetPerfLevel() != perf_level) {
    rocksdb::SetPerfLevel(perf_level);
  }

  if (perf_level == rocksdb::kDisable) {
    return false;
  }

  rocksdb::get_perf_context()->Reset();
  rocksdb::get_iostats_context()->Reset();
  return true;
}

void Rdb_io_perf::update_bytes_written(const uint32_t perf_context_level,
                                       ulonglong bytes_written) {
  const rocksdb::PerfLevel perf_level =
      static_cast<rocksdb::PerfLevel>(perf_context_level);
  if (perf_level != rocksdb::kDisable && m_shared_io_perf_write) {
    io_write_bytes += bytes_written;
    io_write_requests += 1;
  }
}

void Rdb_io_perf::end_and_record(const uint32_t perf_context_level) {
  const rocksdb::PerfLevel perf_level =
      static_cast<rocksdb::PerfLevel>(perf_context_level);

  if (perf_level == rocksdb::kDisable) {
    return;
  }

  if (m_atomic_counters) {
    harvest_diffs(m_atomic_counters);
  }
  harvest_diffs(&rdb_global_perf_counters);

  if (m_shared_io_perf_read &&
      (rocksdb::get_perf_context()->block_read_byte != 0 ||
       rocksdb::get_perf_context()->block_read_count != 0 ||
       rocksdb::get_perf_context()->block_read_time != 0)) 
  {
#ifdef MARIAROCKS_NOT_YET
    my_io_perf_t io_perf_read;

    io_perf_read.init();
    io_perf_read.bytes = rocksdb::get_perf_context()->block_read_byte;
    io_perf_read.requests = rocksdb::get_perf_context()->block_read_count;

    /*
      Rocksdb does not distinguish between I/O service and wait time, so just
      use svc time.
     */
    io_perf_read.svc_time_max = io_perf_read.svc_time =
        rocksdb::get_perf_context()->block_read_time;

    m_shared_io_perf_read->sum(io_perf_read);
    m_stats->table_io_perf_read.sum(io_perf_read);
#endif
  }

#ifdef MARIAROCKS_NOT_YET
  if (m_shared_io_perf_write &&
      (io_write_bytes != 0 || io_write_requests != 0)) {
    my_io_perf_t io_perf_write;
    io_perf_write.init();
    io_perf_write.bytes = io_write_bytes;
    io_perf_write.requests = io_write_requests;
    m_shared_io_perf_write->sum(io_perf_write);
    m_stats->table_io_perf_write.sum(io_perf_write);
    io_write_bytes = 0;
    io_write_requests = 0;
  }

  if (m_stats) {
    if (rocksdb::get_perf_context()->internal_key_skipped_count != 0) {
      m_stats->key_skipped +=
          rocksdb::get_perf_context()->internal_key_skipped_count;
    }

    if (rocksdb::get_perf_context()->internal_delete_skipped_count != 0) {
      m_stats->delete_skipped +=
          rocksdb::get_perf_context()->internal_delete_skipped_count;
    }
  }
#endif
}

}  // namespace myrocks
