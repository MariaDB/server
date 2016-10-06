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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* The C++ file's header */
#include "./event_listener.h"

/* C++ standard header files */
#include <string>
#include <vector>

/* MySQL includes */
#include <mysql/plugin.h>
#include <my_global.h>

/* MyRocks includes */
#include "./ha_rocksdb.h"
#include "./properties_collector.h"
#include "./rdb_datadic.h"

namespace myrocks {

static std::vector<Rdb_index_stats>
extract_index_stats(
  const std::vector<std::string>& files,
  const rocksdb::TablePropertiesCollection& props
) {
  std::vector<Rdb_index_stats> ret;
  for (auto fn : files) {
    auto it = props.find(fn);
    DBUG_ASSERT(it != props.end());
    std::vector<Rdb_index_stats> stats;
    Rdb_tbl_prop_coll::read_stats_from_tbl_props(it->second, &stats);
    ret.insert(ret.end(), stats.begin(), stats.end());
  }
  return ret;
}

void Rdb_event_listener::OnCompactionCompleted(
  rocksdb::DB *db,
  const rocksdb::CompactionJobInfo& ci
) {
  DBUG_ASSERT(db != nullptr);
  DBUG_ASSERT(m_ddl_manager != nullptr);

  if (ci.status.ok()) {
    m_ddl_manager->adjust_stats(
      extract_index_stats(ci.output_files, ci.table_properties),
      extract_index_stats(ci.input_files, ci.table_properties));
  }
}

void Rdb_event_listener::OnFlushCompleted(
  rocksdb::DB* db,
  const rocksdb::FlushJobInfo& flush_job_info
) {
  DBUG_ASSERT(db != nullptr);
  DBUG_ASSERT(m_ddl_manager != nullptr);

  auto tbl_props = std::make_shared<const rocksdb::TableProperties>(
    flush_job_info.table_properties);

  std::vector<Rdb_index_stats> stats;
  Rdb_tbl_prop_coll::read_stats_from_tbl_props(tbl_props, &stats);
  m_ddl_manager->adjust_stats(stats);
}

}  // namespace myrocks
