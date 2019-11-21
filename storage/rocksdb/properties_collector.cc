/*
   Copyright (c) 2015, Facebook, Inc.

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
#ifdef _WIN32
#define _CRT_RAND_S
#include <stdlib.h>
#define rand_r rand_s
#endif
/* This C++ file's header file */
#include "./properties_collector.h"

/* Standard C++ header files */
#include <algorithm>
#include <map>
#include <string>
#include <vector>

/* MySQL header files */
#include "./log.h"
#include "./my_stacktrace.h"
#include "./sql_array.h"

/* MyRocks header files */
#include "./rdb_datadic.h"
#include "./rdb_utils.h"

namespace myrocks {

std::atomic<uint64_t> rocksdb_num_sst_entry_put(0);
std::atomic<uint64_t> rocksdb_num_sst_entry_delete(0);
std::atomic<uint64_t> rocksdb_num_sst_entry_singledelete(0);
std::atomic<uint64_t> rocksdb_num_sst_entry_merge(0);
std::atomic<uint64_t> rocksdb_num_sst_entry_other(0);
my_bool rocksdb_compaction_sequential_deletes_count_sd = false;

Rdb_tbl_prop_coll::Rdb_tbl_prop_coll(Rdb_ddl_manager *const ddl_manager,
                                     const Rdb_compact_params &params,
                                     const uint32_t cf_id,
                                     const uint8_t table_stats_sampling_pct)
    : m_cf_id(cf_id),
      m_ddl_manager(ddl_manager),
      m_last_stats(nullptr),
      m_rows(0l),
      m_window_pos(0l),
      m_deleted_rows(0l),
      m_max_deleted_rows(0l),
      m_file_size(0),
      m_params(params),
      m_cardinality_collector(table_stats_sampling_pct),
      m_recorded(false) {
  DBUG_ASSERT(ddl_manager != nullptr);

  m_deleted_rows_window.resize(m_params.m_window, false);
}

/*
  This function is called by RocksDB for every key in the SST file
*/
rocksdb::Status Rdb_tbl_prop_coll::AddUserKey(const rocksdb::Slice &key,
                                              const rocksdb::Slice &value,
                                              rocksdb::EntryType type,
                                              rocksdb::SequenceNumber seq,
                                              uint64_t file_size) {
  if (key.size() >= 4) {
    AdjustDeletedRows(type);

    m_rows++;

    CollectStatsForRow(key, value, type, file_size);
  }

  return rocksdb::Status::OK();
}

void Rdb_tbl_prop_coll::AdjustDeletedRows(rocksdb::EntryType type) {
  if (m_params.m_window > 0) {
    // record the "is deleted" flag into the sliding window
    // the sliding window is implemented as a circular buffer
    // in m_deleted_rows_window vector
    // the current position in the circular buffer is pointed at by
    // m_rows % m_deleted_rows_window.size()
    // m_deleted_rows is the current number of 1's in the vector
    // --update the counter for the element which will be overridden
    const bool is_delete = (type == rocksdb::kEntryDelete ||
                            (type == rocksdb::kEntrySingleDelete &&
                             rocksdb_compaction_sequential_deletes_count_sd));

    // Only make changes if the value at the current position needs to change
    if (is_delete != m_deleted_rows_window[m_window_pos]) {
      // Set or clear the flag at the current position as appropriate
      m_deleted_rows_window[m_window_pos] = is_delete;
      if (!is_delete) {
        m_deleted_rows--;
      } else if (++m_deleted_rows > m_max_deleted_rows) {
        m_max_deleted_rows = m_deleted_rows;
      }
    }

    if (++m_window_pos == m_params.m_window) {
      m_window_pos = 0;
    }
  }
}

Rdb_index_stats *Rdb_tbl_prop_coll::AccessStats(const rocksdb::Slice &key) {
  GL_INDEX_ID gl_index_id;
  gl_index_id.cf_id = m_cf_id;
  gl_index_id.index_id = rdb_netbuf_to_uint32(reinterpret_cast<const uchar*>(key.data()));

  if (m_last_stats == nullptr || m_last_stats->m_gl_index_id != gl_index_id) {
    m_keydef = nullptr;

    // starting a new table
    // add the new element into m_stats
    m_stats.emplace_back(gl_index_id);
    m_last_stats = &m_stats.back();

    if (m_ddl_manager) {
      // safe_find() returns a std::shared_ptr<Rdb_key_def> with the count
      // incremented (so it can't be deleted out from under us) and with
      // the mutex locked (if setup has not occurred yet).  We must make
      // sure to free the mutex (via unblock_setup()) when we are done
      // with this object.  Currently this happens earlier in this function
      // when we are switching to a new Rdb_key_def and when this object
      // is destructed.
      m_keydef = m_ddl_manager->safe_find(gl_index_id);
      if (m_keydef != nullptr) {
        // resize the array to the number of columns.
        // It will be initialized with zeroes
        m_last_stats->m_distinct_keys_per_prefix.resize(
            m_keydef->get_key_parts());
        m_last_stats->m_name = m_keydef->get_name();
      }
    }
    m_cardinality_collector.Reset();
  }

  return m_last_stats;
}

void Rdb_tbl_prop_coll::CollectStatsForRow(const rocksdb::Slice &key,
                                           const rocksdb::Slice &value,
                                           const rocksdb::EntryType &type,
                                           const uint64_t file_size) {
  auto stats = AccessStats(key);

  stats->m_data_size += key.size() + value.size();

  // Incrementing per-index entry-type statistics
  switch (type) {
    case rocksdb::kEntryPut:
      stats->m_rows++;
      break;
    case rocksdb::kEntryDelete:
      stats->m_entry_deletes++;
      break;
    case rocksdb::kEntrySingleDelete:
      stats->m_entry_single_deletes++;
      break;
    case rocksdb::kEntryMerge:
      stats->m_entry_merges++;
      break;
    case rocksdb::kEntryOther:
      stats->m_entry_others++;
      break;
    default:
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Unexpected entry type found: %u. "
          "This should not happen so aborting the system.",
          type);
      abort();
      break;
  }

  stats->m_actual_disk_size += file_size - m_file_size;
  m_file_size = file_size;

  if (m_keydef != nullptr) {
    m_cardinality_collector.ProcessKey(key, m_keydef.get(), stats);
  }
}

const char *Rdb_tbl_prop_coll::INDEXSTATS_KEY = "__indexstats__";

/*
  This function is called by RocksDB to compute properties to store in sst file
*/
rocksdb::Status Rdb_tbl_prop_coll::Finish(
    rocksdb::UserCollectedProperties *const properties) {
  uint64_t num_sst_entry_put = 0;
  uint64_t num_sst_entry_delete = 0;
  uint64_t num_sst_entry_singledelete = 0;
  uint64_t num_sst_entry_merge = 0;
  uint64_t num_sst_entry_other = 0;

  DBUG_ASSERT(properties != nullptr);

  for (auto it = m_stats.begin(); it != m_stats.end(); it++) {
    num_sst_entry_put += it->m_rows;
    num_sst_entry_delete += it->m_entry_deletes;
    num_sst_entry_singledelete += it->m_entry_single_deletes;
    num_sst_entry_merge += it->m_entry_merges;
    num_sst_entry_other += it->m_entry_others;
  }

  if (!m_recorded) {
    if (num_sst_entry_put > 0) {
      rocksdb_num_sst_entry_put += num_sst_entry_put;
    }

    if (num_sst_entry_delete > 0) {
      rocksdb_num_sst_entry_delete += num_sst_entry_delete;
    }

    if (num_sst_entry_singledelete > 0) {
      rocksdb_num_sst_entry_singledelete += num_sst_entry_singledelete;
    }

    if (num_sst_entry_merge > 0) {
      rocksdb_num_sst_entry_merge += num_sst_entry_merge;
    }

    if (num_sst_entry_other > 0) {
      rocksdb_num_sst_entry_other += num_sst_entry_other;
    }

    for (Rdb_index_stats &stat : m_stats) {
      m_cardinality_collector.AdjustStats(&stat);
    }
    m_recorded = true;
  }
  properties->insert({INDEXSTATS_KEY, Rdb_index_stats::materialize(m_stats)});
  return rocksdb::Status::OK();
}

bool Rdb_tbl_prop_coll::NeedCompact() const {
  return m_params.m_deletes && (m_params.m_window > 0) &&
         (m_file_size > m_params.m_file_size) &&
         (m_max_deleted_rows > m_params.m_deletes);
}

/*
  Returns the same as above, but in human-readable way for logging
*/
rocksdb::UserCollectedProperties Rdb_tbl_prop_coll::GetReadableProperties()
    const {
  std::string s;
#ifdef DBUG_OFF
  s.append("[...");
  s.append(std::to_string(m_stats.size()));
  s.append("  records...]");
#else
  bool first = true;
  for (auto it : m_stats) {
    if (first) {
      first = false;
    } else {
      s.append(",");
    }
    s.append(GetReadableStats(it));
  }
#endif
  return rocksdb::UserCollectedProperties{{INDEXSTATS_KEY, s}};
}

std::string Rdb_tbl_prop_coll::GetReadableStats(const Rdb_index_stats &it) {
  std::string s;
  s.append("(");
  s.append(std::to_string(it.m_gl_index_id.cf_id));
  s.append(", ");
  s.append(std::to_string(it.m_gl_index_id.index_id));
  s.append("):{name:");
  s.append(it.m_name);
  s.append(", size:");
  s.append(std::to_string(it.m_data_size));
  s.append(", m_rows:");
  s.append(std::to_string(it.m_rows));
  s.append(", m_actual_disk_size:");
  s.append(std::to_string(it.m_actual_disk_size));
  s.append(", deletes:");
  s.append(std::to_string(it.m_entry_deletes));
  s.append(", single_deletes:");
  s.append(std::to_string(it.m_entry_single_deletes));
  s.append(", merges:");
  s.append(std::to_string(it.m_entry_merges));
  s.append(", others:");
  s.append(std::to_string(it.m_entry_others));
  s.append(", distincts per prefix: [");
  for (auto num : it.m_distinct_keys_per_prefix) {
    s.append(std::to_string(num));
    s.append(" ");
  }
  s.append("]}");
  return s;
}

/*
  Given the properties of an SST file, reads the stats from it and returns it.
*/

void Rdb_tbl_prop_coll::read_stats_from_tbl_props(
    const std::shared_ptr<const rocksdb::TableProperties> &table_props,
    std::vector<Rdb_index_stats> *const out_stats_vector) {
  DBUG_ASSERT(out_stats_vector != nullptr);
  const auto &user_properties = table_props->user_collected_properties;
  const auto it2 = user_properties.find(std::string(INDEXSTATS_KEY));
  if (it2 != user_properties.end()) {
    auto result MY_ATTRIBUTE((__unused__)) =
        Rdb_index_stats::unmaterialize(it2->second, out_stats_vector);
    DBUG_ASSERT(result == 0);
  }
}

/*
  Serializes an array of Rdb_index_stats into a network string.
*/
std::string Rdb_index_stats::materialize(
    const std::vector<Rdb_index_stats> &stats) {
  String ret;
  rdb_netstr_append_uint16(&ret, INDEX_STATS_VERSION_ENTRY_TYPES);
  for (const auto &i : stats) {
    rdb_netstr_append_uint32(&ret, i.m_gl_index_id.cf_id);
    rdb_netstr_append_uint32(&ret, i.m_gl_index_id.index_id);
    DBUG_ASSERT(sizeof i.m_data_size <= 8);
    rdb_netstr_append_uint64(&ret, i.m_data_size);
    rdb_netstr_append_uint64(&ret, i.m_rows);
    rdb_netstr_append_uint64(&ret, i.m_actual_disk_size);
    rdb_netstr_append_uint64(&ret, i.m_distinct_keys_per_prefix.size());
    rdb_netstr_append_uint64(&ret, i.m_entry_deletes);
    rdb_netstr_append_uint64(&ret, i.m_entry_single_deletes);
    rdb_netstr_append_uint64(&ret, i.m_entry_merges);
    rdb_netstr_append_uint64(&ret, i.m_entry_others);
    for (const auto &num_keys : i.m_distinct_keys_per_prefix) {
      rdb_netstr_append_uint64(&ret, num_keys);
    }
  }

  return std::string((char *)ret.ptr(), ret.length());
}

/**
  @brief
  Reads an array of Rdb_index_stats from a string.
  @return HA_EXIT_FAILURE if it detects any inconsistency in the input
  @return HA_EXIT_SUCCESS if completes successfully
*/
int Rdb_index_stats::unmaterialize(const std::string &s,
                                   std::vector<Rdb_index_stats> *const ret) {
  const uchar *p = rdb_std_str_to_uchar_ptr(s);
  const uchar *const p2 = p + s.size();

  DBUG_ASSERT(ret != nullptr);

  if (p + 2 > p2) {
    return HA_EXIT_FAILURE;
  }

  const int version = rdb_netbuf_read_uint16(&p);
  Rdb_index_stats stats;
  // Make sure version is within supported range.
  if (version < INDEX_STATS_VERSION_INITIAL ||
      version > INDEX_STATS_VERSION_ENTRY_TYPES) {
    // NO_LINT_DEBUG
    sql_print_error(
        "Index stats version %d was outside of supported range. "
        "This should not happen so aborting the system.",
        version);
    abort();
  }

  size_t needed = sizeof(stats.m_gl_index_id.cf_id) +
                  sizeof(stats.m_gl_index_id.index_id) +
                  sizeof(stats.m_data_size) + sizeof(stats.m_rows) +
                  sizeof(stats.m_actual_disk_size) + sizeof(uint64);
  if (version >= INDEX_STATS_VERSION_ENTRY_TYPES) {
    needed += sizeof(stats.m_entry_deletes) +
              sizeof(stats.m_entry_single_deletes) +
              sizeof(stats.m_entry_merges) + sizeof(stats.m_entry_others);
  }

  while (p < p2) {
    if (p + needed > p2) {
      return HA_EXIT_FAILURE;
    }
    rdb_netbuf_read_gl_index(&p, &stats.m_gl_index_id);
    stats.m_data_size = rdb_netbuf_read_uint64(&p);
    stats.m_rows = rdb_netbuf_read_uint64(&p);
    stats.m_actual_disk_size = rdb_netbuf_read_uint64(&p);
    stats.m_distinct_keys_per_prefix.resize(rdb_netbuf_read_uint64(&p));
    if (version >= INDEX_STATS_VERSION_ENTRY_TYPES) {
      stats.m_entry_deletes = rdb_netbuf_read_uint64(&p);
      stats.m_entry_single_deletes = rdb_netbuf_read_uint64(&p);
      stats.m_entry_merges = rdb_netbuf_read_uint64(&p);
      stats.m_entry_others = rdb_netbuf_read_uint64(&p);
    }
    if (p + stats.m_distinct_keys_per_prefix.size() *
                sizeof(stats.m_distinct_keys_per_prefix[0]) >
        p2) {
      return HA_EXIT_FAILURE;
    }
    for (std::size_t i = 0; i < stats.m_distinct_keys_per_prefix.size(); i++) {
      stats.m_distinct_keys_per_prefix[i] = rdb_netbuf_read_uint64(&p);
    }
    ret->push_back(stats);
  }
  return HA_EXIT_SUCCESS;
}

/*
  Merges one Rdb_index_stats into another. Can be used to come up with the stats
  for the index based on stats for each sst
*/
void Rdb_index_stats::merge(const Rdb_index_stats &s, const bool increment,
                            const int64_t estimated_data_len) {
  std::size_t i;

  DBUG_ASSERT(estimated_data_len >= 0);

  m_gl_index_id = s.m_gl_index_id;
  if (m_distinct_keys_per_prefix.size() < s.m_distinct_keys_per_prefix.size()) {
    m_distinct_keys_per_prefix.resize(s.m_distinct_keys_per_prefix.size());
  }
  if (increment) {
    m_rows += s.m_rows;
    m_data_size += s.m_data_size;

    /*
      The Data_length and Avg_row_length are trailing statistics, meaning
      they don't get updated for the current SST until the next SST is
      written.  So, if rocksdb reports the data_length as 0,
      we make a reasoned estimate for the data_file_length for the
      index in the current SST.
    */
    m_actual_disk_size += s.m_actual_disk_size ? s.m_actual_disk_size
                                               : estimated_data_len * s.m_rows;
    m_entry_deletes += s.m_entry_deletes;
    m_entry_single_deletes += s.m_entry_single_deletes;
    m_entry_merges += s.m_entry_merges;
    m_entry_others += s.m_entry_others;
    for (i = 0; i < s.m_distinct_keys_per_prefix.size(); i++) {
      m_distinct_keys_per_prefix[i] += s.m_distinct_keys_per_prefix[i];
    }
  } else {
    m_rows -= s.m_rows;
    m_data_size -= s.m_data_size;
    m_actual_disk_size -= s.m_actual_disk_size ? s.m_actual_disk_size
                                               : estimated_data_len * s.m_rows;
    m_entry_deletes -= s.m_entry_deletes;
    m_entry_single_deletes -= s.m_entry_single_deletes;
    m_entry_merges -= s.m_entry_merges;
    m_entry_others -= s.m_entry_others;
    for (i = 0; i < s.m_distinct_keys_per_prefix.size(); i++) {
      m_distinct_keys_per_prefix[i] -= s.m_distinct_keys_per_prefix[i];
    }
  }
}

Rdb_tbl_card_coll::Rdb_tbl_card_coll(const uint8_t table_stats_sampling_pct)
    : m_table_stats_sampling_pct(table_stats_sampling_pct),
      m_seed(time(nullptr)) {}

bool Rdb_tbl_card_coll::IsSampingDisabled() {
  // Zero means that we'll use all the keys to update statistics.
  return m_table_stats_sampling_pct == 0 ||
         RDB_TBL_STATS_SAMPLE_PCT_MAX == m_table_stats_sampling_pct;
}

bool Rdb_tbl_card_coll::ShouldCollectStats() {
  if (IsSampingDisabled()) {
    return true;  // collect every key
  }

  const int val = rand_r(&m_seed) % (RDB_TBL_STATS_SAMPLE_PCT_MAX -
                                     RDB_TBL_STATS_SAMPLE_PCT_MIN + 1) +
                  RDB_TBL_STATS_SAMPLE_PCT_MIN;

  DBUG_ASSERT(val >= RDB_TBL_STATS_SAMPLE_PCT_MIN);
  DBUG_ASSERT(val <= RDB_TBL_STATS_SAMPLE_PCT_MAX);

  return val <= m_table_stats_sampling_pct;
}

void Rdb_tbl_card_coll::ProcessKey(const rocksdb::Slice &key,
                                   const Rdb_key_def *keydef,
                                   Rdb_index_stats *stats) {
  if (ShouldCollectStats()) {
    std::size_t column = 0;
    bool new_key = true;

    if (!m_last_key.empty()) {
      rocksdb::Slice last(m_last_key.data(), m_last_key.size());
      new_key = (keydef->compare_keys(&last, &key, &column) == 0);
    }

    if (new_key) {
      DBUG_ASSERT(column <= stats->m_distinct_keys_per_prefix.size());

      for (auto i = column; i < stats->m_distinct_keys_per_prefix.size(); i++) {
        stats->m_distinct_keys_per_prefix[i]++;
      }

      // assign new last_key for the next call
      // however, we only need to change the last key
      // if one of the first n-1 columns is different
      // If the n-1 prefix is the same, no sense in storing
      // the new key
      if (column < stats->m_distinct_keys_per_prefix.size()) {
        m_last_key.assign(key.data(), key.size());
      }
    }
  }
}

void Rdb_tbl_card_coll::Reset() { m_last_key.clear(); }

// We need to adjust the index cardinality numbers based on the sampling
// rate so that the output of "SHOW INDEX" command will reflect reality
// more closely. It will still be an approximation, just a better one.
void Rdb_tbl_card_coll::AdjustStats(Rdb_index_stats *stats) {
  if (IsSampingDisabled()) {
    // no sampling was done, return as stats is
    return;
  }
  for (int64_t &num_keys : stats->m_distinct_keys_per_prefix) {
    num_keys = num_keys * 100 / m_table_stats_sampling_pct;
  }
}

}  // namespace myrocks
