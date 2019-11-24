/*
   Portions Copyright (c) 2016-Present, Facebook, Inc.
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
#pragma once

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation  // gcc: Class implementation
#endif

/* C++ system header files */
#include <time.h>
#include <string>
#include <ctime>

/* RocksDB includes */
#include "rocksdb/compaction_filter.h"

/* MyRocks includes */
#include "./ha_rocksdb_proto.h"
#include "./rdb_datadic.h"

namespace myrocks {

class Rdb_compact_filter : public rocksdb::CompactionFilter {
 public:
  Rdb_compact_filter(const Rdb_compact_filter &) = delete;
  Rdb_compact_filter &operator=(const Rdb_compact_filter &) = delete;

  explicit Rdb_compact_filter(uint32_t _cf_id) : m_cf_id(_cf_id) {}
  ~Rdb_compact_filter() {
    // Increment stats by num expired at the end of compaction
    rdb_update_global_stats(ROWS_EXPIRED, m_num_expired);
  }

  // keys are passed in sorted order within the same sst.
  // V1 Filter is thread safe on our usage (creating from Factory).
  // Make sure to protect instance variables when switching to thread
  // unsafe in the future.
  virtual bool Filter(int level, const rocksdb::Slice &key,
                      const rocksdb::Slice &existing_value,
                      std::string *new_value,
                      bool *value_changed) const override {
    DBUG_ASSERT(key.size() >= sizeof(uint32));

    GL_INDEX_ID gl_index_id;
    gl_index_id.cf_id = m_cf_id;
    gl_index_id.index_id = rdb_netbuf_to_uint32((const uchar *)key.data());
    DBUG_ASSERT(gl_index_id.index_id >= 1);

    if (gl_index_id != m_prev_index) {
      m_should_delete =
          rdb_get_dict_manager()->is_drop_index_ongoing(gl_index_id);

      if (!m_should_delete) {
        get_ttl_duration_and_offset(gl_index_id, &m_ttl_duration,
                                    &m_ttl_offset);

        if (m_ttl_duration != 0 && m_snapshot_timestamp == 0) {
          /*
            For efficiency reasons, we lazily call GetIntProperty to get the
            oldest snapshot time (occurs once per compaction).
          */
          rocksdb::DB *const rdb = rdb_get_rocksdb_db();
          if (!rdb->GetIntProperty(rocksdb::DB::Properties::kOldestSnapshotTime,
                                   &m_snapshot_timestamp) ||
              m_snapshot_timestamp == 0) {
            m_snapshot_timestamp = static_cast<uint64_t>(std::time(nullptr));
          }

#ifndef DBUG_OFF
          int snapshot_ts = rdb_dbug_set_ttl_snapshot_ts();
          if (snapshot_ts) {
            m_snapshot_timestamp =
                static_cast<uint64_t>(std::time(nullptr)) + snapshot_ts;
          }
#endif
        }
      }

      m_prev_index = gl_index_id;
    }

    if (m_should_delete) {
      m_num_deleted++;
      return true;
    } else if (m_ttl_duration > 0 &&
               should_filter_ttl_rec(key, existing_value)) {
      m_num_expired++;
      return true;
    }

    return false;
  }

  virtual bool IgnoreSnapshots() const override { return true; }

  virtual const char *Name() const override { return "Rdb_compact_filter"; }

  void get_ttl_duration_and_offset(const GL_INDEX_ID &gl_index_id,
                                   uint64 *ttl_duration,
                                   uint32 *ttl_offset) const {
    DBUG_ASSERT(ttl_duration != nullptr);
    /*
      If TTL is disabled set ttl_duration to 0.  This prevents the compaction
      filter from dropping expired records.
    */
    if (!rdb_is_ttl_enabled()) {
      *ttl_duration = 0;
      return;
    }

    /*
      If key is part of system column family, it's definitely not a TTL key.
    */
    rocksdb::ColumnFamilyHandle *s_cf = rdb_get_dict_manager()->get_system_cf();
    if (s_cf == nullptr || gl_index_id.cf_id == s_cf->GetID()) {
      *ttl_duration = 0;
      return;
    }

    struct Rdb_index_info index_info;
    if (!rdb_get_dict_manager()->get_index_info(gl_index_id, &index_info)) {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Could not get index information "
          "for Index Number (%u,%u)",
          gl_index_id.cf_id, gl_index_id.index_id);
    }

#ifndef DBUG_OFF
    if (rdb_dbug_set_ttl_ignore_pk() &&
        index_info.m_index_type == Rdb_key_def::INDEX_TYPE_PRIMARY) {
      *ttl_duration = 0;
      return;
    }
#endif

    *ttl_duration = index_info.m_ttl_duration;
    if (Rdb_key_def::has_index_flag(index_info.m_index_flags,
                                    Rdb_key_def::TTL_FLAG)) {
      *ttl_offset = Rdb_key_def::calculate_index_flag_offset(
          index_info.m_index_flags, Rdb_key_def::TTL_FLAG);
    }
  }

  bool should_filter_ttl_rec(const rocksdb::Slice &key,
                             const rocksdb::Slice &existing_value) const {
    uint64 ttl_timestamp;
    Rdb_string_reader reader(&existing_value);
    if (!reader.read(m_ttl_offset) || reader.read_uint64(&ttl_timestamp)) {
      std::string buf;
      buf = rdb_hexdump(existing_value.data(), existing_value.size(),
                        RDB_MAX_HEXDUMP_LEN);
      // NO_LINT_DEBUG
      sql_print_error(
          "Decoding ttl from PK value failed in compaction filter, "
          "for index (%u,%u), val: %s",
          m_prev_index.cf_id, m_prev_index.index_id, buf.c_str());
      abort();
    }

    /*
      Filter out the record only if it is older than the oldest snapshot
      timestamp.  This prevents any rows from expiring in the middle of
      long-running transactions.
    */
    return ttl_timestamp + m_ttl_duration <= m_snapshot_timestamp;
  }

 private:
  // Column family for this compaction filter
  const uint32_t m_cf_id;
  // Index id of the previous record
  mutable GL_INDEX_ID m_prev_index = {0, 0};
  // Number of rows deleted for the same index id
  mutable uint64 m_num_deleted = 0;
  // Number of rows expired for the TTL index
  mutable uint64 m_num_expired = 0;
  // Current index id should be deleted or not (should be deleted if true)
  mutable bool m_should_delete = false;
  // TTL duration for the current index if TTL is enabled
  mutable uint64 m_ttl_duration = 0;
  // TTL offset for all records in the current index
  mutable uint32 m_ttl_offset = 0;
  // Oldest snapshot timestamp at the time a TTL index is discovered
  mutable uint64_t m_snapshot_timestamp = 0;
};

class Rdb_compact_filter_factory : public rocksdb::CompactionFilterFactory {
 public:
  Rdb_compact_filter_factory(const Rdb_compact_filter_factory &) = delete;
  Rdb_compact_filter_factory &operator=(const Rdb_compact_filter_factory &) =
      delete;
  Rdb_compact_filter_factory() {}

  ~Rdb_compact_filter_factory() {}

  const char *Name() const override { return "Rdb_compact_filter_factory"; }

  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context &context) override {
    return std::unique_ptr<rocksdb::CompactionFilter>(
        new Rdb_compact_filter(context.column_family_id));
  }
};

}  // namespace myrocks
