/*
   Portions Copyright (c) 2015-Present, Facebook, Inc.
   Portions Copyright (c) 2012,2013 Monty Program Ab

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
#pragma once

/* C++ standard header files */
#include <atomic>
#include <cstdint>
#include <string>

/* MySQL header files */
#include <my_global.h>
#include "./handler.h"

#include "rdb_mariadb_port.h"

namespace myrocks {

enum {
  PC_USER_KEY_COMPARISON_COUNT = 0,
  PC_BLOCK_CACHE_HIT_COUNT,
  PC_BLOCK_READ_COUNT,
  PC_BLOCK_READ_BYTE,
  PC_BLOCK_READ_TIME,
  PC_BLOCK_CHECKSUM_TIME,
  PC_BLOCK_DECOMPRESS_TIME,
  PC_GET_READ_BYTES,
  PC_MULTIGET_READ_BYTES,
  PC_ITER_READ_BYTES,
  PC_KEY_SKIPPED,
  PC_DELETE_SKIPPED,
  PC_RECENT_SKIPPED,
  PC_MERGE,
  PC_GET_SNAPSHOT_TIME,
  PC_GET_FROM_MEMTABLE_TIME,
  PC_GET_FROM_MEMTABLE_COUNT,
  PC_GET_POST_PROCESS_TIME,
  PC_GET_FROM_OUTPUT_FILES_TIME,
  PC_SEEK_ON_MEMTABLE_TIME,
  PC_SEEK_ON_MEMTABLE_COUNT,
  PC_NEXT_ON_MEMTABLE_COUNT,
  PC_PREV_ON_MEMTABLE_COUNT,
  PC_SEEK_CHILD_SEEK_TIME,
  PC_SEEK_CHILD_SEEK_COUNT,
  PC_SEEK_MIN_HEAP_TIME,
  PC_SEEK_MAX_HEAP_TIME,
  PC_SEEK_INTERNAL_SEEK_TIME,
  PC_FIND_NEXT_USER_ENTRY_TIME,
  PC_WRITE_WAL_TIME,
  PC_WRITE_MEMTABLE_TIME,
  PC_WRITE_DELAY_TIME,
  PC_WRITE_PRE_AND_POST_PROCESSS_TIME,
  PC_DB_MUTEX_LOCK_NANOS,
  PC_DB_CONDITION_WAIT_NANOS,
  PC_MERGE_OPERATOR_TIME_NANOS,
  PC_READ_INDEX_BLOCK_NANOS,
  PC_READ_FILTER_BLOCK_NANOS,
  PC_NEW_TABLE_BLOCK_ITER_NANOS,
  PC_NEW_TABLE_ITERATOR_NANOS,
  PC_BLOCK_SEEK_NANOS,
  PC_FIND_TABLE_NANOS,
  PC_BLOOM_MEMTABLE_HIT_COUNT,
  PC_BLOOM_MEMTABLE_MISS_COUNT,
  PC_BLOOM_SST_HIT_COUNT,
  PC_BLOOM_SST_MISS_COUNT,
  PC_KEY_LOCK_WAIT_TIME,
  PC_KEY_LOCK_WAIT_COUNT,
  PC_IO_THREAD_POOL_ID,
  PC_IO_BYTES_WRITTEN,
  PC_IO_BYTES_READ,
  PC_IO_OPEN_NANOS,
  PC_IO_ALLOCATE_NANOS,
  PC_IO_WRITE_NANOS,
  PC_IO_READ_NANOS,
  PC_IO_RANGE_SYNC_NANOS,
  PC_IO_LOGGER_NANOS,
  PC_MAX_IDX
};

class Rdb_perf_counters;

/*
  A collection of performance counters that can be safely incremented by
  multiple threads since it stores atomic datapoints.
*/
struct Rdb_atomic_perf_counters {
  std::atomic_ullong m_value[PC_MAX_IDX];
};

/*
  A collection of performance counters that is meant to be incremented by
  a single thread.
*/
class Rdb_perf_counters {
  Rdb_perf_counters(const Rdb_perf_counters &) = delete;
  Rdb_perf_counters &operator=(const Rdb_perf_counters &) = delete;

 public:
  Rdb_perf_counters() = default;
  uint64_t m_value[PC_MAX_IDX];

  void load(const Rdb_atomic_perf_counters &atomic_counters);
};

extern std::string rdb_pc_stat_types[PC_MAX_IDX];

/*
  Perf timers for data reads
 */
class Rdb_io_perf {
  // Context management
  Rdb_atomic_perf_counters *m_atomic_counters = nullptr;
  my_io_perf_atomic_t *m_shared_io_perf_read = nullptr;
  my_io_perf_atomic_t *m_shared_io_perf_write = nullptr;
  ha_statistics *m_stats = nullptr;

  uint64_t io_write_bytes;
  uint64_t io_write_requests;

 public:
  Rdb_io_perf(const Rdb_io_perf &) = delete;
  Rdb_io_perf &operator=(const Rdb_io_perf &) = delete;

  void init(Rdb_atomic_perf_counters *const atomic_counters,
            my_io_perf_atomic_t *const shared_io_perf_read,
            my_io_perf_atomic_t *const shared_io_perf_write,
            ha_statistics *const stats) {
    DBUG_ASSERT(atomic_counters != nullptr);
    DBUG_ASSERT(shared_io_perf_read != nullptr);
    DBUG_ASSERT(shared_io_perf_write != nullptr);
    DBUG_ASSERT(stats != nullptr);

    m_atomic_counters = atomic_counters;
    m_shared_io_perf_read = shared_io_perf_read;
    m_shared_io_perf_write = shared_io_perf_write;
    m_stats = stats;

    io_write_bytes = 0;
    io_write_requests = 0;
  }

  bool start(const uint32_t perf_context_level);
  void update_bytes_written(const uint32_t perf_context_level,
                            ulonglong bytes_written);
  void end_and_record(const uint32_t perf_context_level);

  explicit Rdb_io_perf()
      : m_atomic_counters(nullptr),
        m_shared_io_perf_read(nullptr),
        m_stats(nullptr),
        io_write_bytes(0),
        io_write_requests(0) {}
};

}  // namespace myrocks
