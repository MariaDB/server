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
#pragma once

/* C++ system header files */
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

/* RocksDB header files */
#include "rocksdb/db.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"

namespace myrocks {

class Rdb_ddl_manager;
class Rdb_key_def;

extern std::atomic<uint64_t> rocksdb_num_sst_entry_put;
extern std::atomic<uint64_t> rocksdb_num_sst_entry_delete;
extern std::atomic<uint64_t> rocksdb_num_sst_entry_singledelete;
extern std::atomic<uint64_t> rocksdb_num_sst_entry_merge;
extern std::atomic<uint64_t> rocksdb_num_sst_entry_other;
extern my_bool rocksdb_compaction_sequential_deletes_count_sd;

struct Rdb_compact_params {
  uint64_t m_deletes, m_window, m_file_size;
};

struct Rdb_index_stats {
  enum {
    INDEX_STATS_VERSION_INITIAL = 1,
    INDEX_STATS_VERSION_ENTRY_TYPES = 2,
  };
  GL_INDEX_ID m_gl_index_id;
  int64_t m_data_size, m_rows, m_actual_disk_size;
  int64_t m_entry_deletes, m_entry_single_deletes;
  int64_t m_entry_merges, m_entry_others;
  std::vector<int64_t> m_distinct_keys_per_prefix;
  std::string m_name;  // name is not persisted

  static std::string materialize(const std::vector<Rdb_index_stats> &stats);
  static int unmaterialize(const std::string &s,
                           std::vector<Rdb_index_stats> *const ret);

  Rdb_index_stats() : Rdb_index_stats({0, 0}) {}
  explicit Rdb_index_stats(GL_INDEX_ID gl_index_id)
      : m_gl_index_id(gl_index_id),
        m_data_size(0),
        m_rows(0),
        m_actual_disk_size(0),
        m_entry_deletes(0),
        m_entry_single_deletes(0),
        m_entry_merges(0),
        m_entry_others(0) {}

  void merge(const Rdb_index_stats &s, const bool increment = true,
             const int64_t estimated_data_len = 0);
};

// The helper class to calculate index cardinality
class Rdb_tbl_card_coll {
 public:
  explicit Rdb_tbl_card_coll(const uint8_t table_stats_sampling_pct);

 public:
  void ProcessKey(const rocksdb::Slice &key, const Rdb_key_def *keydef,
                  Rdb_index_stats *stats);
  /*
   * Resets the state of the collector to start calculating statistics for a
   * next index.
   */
  void Reset();

  /*
   * Cardinality statistics might be calculated using some sampling strategy.
   * This method adjusts gathered statistics according to the sampling
   * strategy used. Note that adjusted cardinality value is just an estimate
   * and can return a value exeeding number of rows in a table, so the
   * returned value should be capped by row count before using it by
   * an optrimizer or displaying it to a clent.
   */
  void AdjustStats(Rdb_index_stats *stats);

 private:
  bool ShouldCollectStats();
  bool IsSampingDisabled();

 private:
  std::string m_last_key;
  uint8_t m_table_stats_sampling_pct;
  unsigned int m_seed;
};

class Rdb_tbl_prop_coll : public rocksdb::TablePropertiesCollector {
 public:
  Rdb_tbl_prop_coll(Rdb_ddl_manager *const ddl_manager,
                    const Rdb_compact_params &params, const uint32_t cf_id,
                    const uint8_t table_stats_sampling_pct);

  /*
    Override parent class's virtual methods of interest.
  */

  virtual rocksdb::Status AddUserKey(const rocksdb::Slice &key,
                                     const rocksdb::Slice &value,
                                     rocksdb::EntryType type,
                                     rocksdb::SequenceNumber seq,
                                     uint64_t file_size) override;

  virtual rocksdb::Status Finish(
      rocksdb::UserCollectedProperties *properties) override;

  virtual const char *Name() const override { return "Rdb_tbl_prop_coll"; }

  rocksdb::UserCollectedProperties GetReadableProperties() const override;

  bool NeedCompact() const override;

 public:
  uint64_t GetMaxDeletedRows() const { return m_max_deleted_rows; }

  static void read_stats_from_tbl_props(
      const std::shared_ptr<const rocksdb::TableProperties> &table_props,
      std::vector<Rdb_index_stats> *out_stats_vector);

 private:
  static std::string GetReadableStats(const Rdb_index_stats &it);

  bool ShouldCollectStats();
  void CollectStatsForRow(const rocksdb::Slice &key,
                          const rocksdb::Slice &value,
                          const rocksdb::EntryType &type,
                          const uint64_t file_size);
  Rdb_index_stats *AccessStats(const rocksdb::Slice &key);
  void AdjustDeletedRows(rocksdb::EntryType type);

 private:
  uint32_t m_cf_id;
  std::shared_ptr<const Rdb_key_def> m_keydef;
  Rdb_ddl_manager *m_ddl_manager;
  std::vector<Rdb_index_stats> m_stats;
  Rdb_index_stats *m_last_stats;
  static const char *INDEXSTATS_KEY;

  // last added key
  std::string m_last_key;

  // floating window to count deleted rows
  std::vector<bool> m_deleted_rows_window;
  uint64_t m_rows, m_window_pos, m_deleted_rows, m_max_deleted_rows;
  uint64_t m_file_size;
  Rdb_compact_params m_params;
  Rdb_tbl_card_coll m_cardinality_collector;
  bool m_recorded;
};

class Rdb_tbl_prop_coll_factory
    : public rocksdb::TablePropertiesCollectorFactory {
 public:
  Rdb_tbl_prop_coll_factory(const Rdb_tbl_prop_coll_factory &) = delete;
  Rdb_tbl_prop_coll_factory &operator=(const Rdb_tbl_prop_coll_factory &) =
      delete;

  explicit Rdb_tbl_prop_coll_factory(Rdb_ddl_manager *ddl_manager)
      : m_ddl_manager(ddl_manager) {}

  /*
    Override parent class's virtual methods of interest.
  */

  virtual rocksdb::TablePropertiesCollector *CreateTablePropertiesCollector(
      rocksdb::TablePropertiesCollectorFactory::Context context) override {
    return new Rdb_tbl_prop_coll(m_ddl_manager, m_params,
                                 context.column_family_id,
                                 m_table_stats_sampling_pct);
  }

  virtual const char *Name() const override {
    return "Rdb_tbl_prop_coll_factory";
  }

 public:
  void SetCompactionParams(const Rdb_compact_params &params) {
    m_params = params;
  }

  void SetTableStatsSamplingPct(const uint8_t table_stats_sampling_pct) {
    m_table_stats_sampling_pct = table_stats_sampling_pct;
  }

 private:
  Rdb_ddl_manager *const m_ddl_manager;
  Rdb_compact_params m_params;
  uint8_t m_table_stats_sampling_pct;
};

}  // namespace myrocks
