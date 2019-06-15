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

/* The C++ file's header */
#include "./event_listener.h"

/* C++ standard header files */
#include <string>
#include <vector>

/* MySQL includes */
#include <my_global.h>
#include <mysql/plugin.h>

/* MyRocks includes */
#include "./ha_rocksdb.h"
#include "./properties_collector.h"
#include "./rdb_datadic.h"

namespace myrocks {

static std::vector<Rdb_index_stats> extract_index_stats(
    const std::vector<std::string> &files,
    const rocksdb::TablePropertiesCollection &props) {
  std::vector<Rdb_index_stats> ret;
  for (auto fn : files) {
    const auto it = props.find(fn);
    DBUG_ASSERT(it != props.end());
    std::vector<Rdb_index_stats> stats;
    Rdb_tbl_prop_coll::read_stats_from_tbl_props(it->second, &stats);
    ret.insert(ret.end(), stats.begin(), stats.end());
  }
  return ret;
}

void Rdb_event_listener::update_index_stats(
    const rocksdb::TableProperties &props) {
  DBUG_ASSERT(m_ddl_manager != nullptr);
  const auto tbl_props =
      std::make_shared<const rocksdb::TableProperties>(props);

  std::vector<Rdb_index_stats> stats;
  Rdb_tbl_prop_coll::read_stats_from_tbl_props(tbl_props, &stats);

  m_ddl_manager->adjust_stats(stats);
}

void Rdb_event_listener::OnCompactionCompleted(
    rocksdb::DB *db, const rocksdb::CompactionJobInfo &ci) {
  DBUG_ASSERT(db != nullptr);
  DBUG_ASSERT(m_ddl_manager != nullptr);

  if (ci.status.ok()) {
    m_ddl_manager->adjust_stats(
        extract_index_stats(ci.output_files, ci.table_properties),
        extract_index_stats(ci.input_files, ci.table_properties));
  }
}

void Rdb_event_listener::OnFlushCompleted(
    rocksdb::DB *db, const rocksdb::FlushJobInfo &flush_job_info) {
  DBUG_ASSERT(db != nullptr);
  update_index_stats(flush_job_info.table_properties);
}

void Rdb_event_listener::OnExternalFileIngested(
    rocksdb::DB *db, const rocksdb::ExternalFileIngestionInfo &info) {
  DBUG_ASSERT(db != nullptr);
  update_index_stats(info.table_properties);
}

void Rdb_event_listener::OnBackgroundError(
    rocksdb::BackgroundErrorReason reason, rocksdb::Status *status) {
  rdb_log_status_error(*status, "Error detected in background");
  // NO_LINT_DEBUG
  sql_print_error("RocksDB: BackgroundErrorReason: %d", (int)reason);
  if (status->IsCorruption()) {
    rdb_persist_corruption_marker();
    abort();
  }
}
}  // namespace myrocks
