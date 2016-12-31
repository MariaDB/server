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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
#pragma once

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

/* C++ system header files */
#include <string>

/* RocksDB includes */
#include "rocksdb/compaction_filter.h"

/* MyRocks includes */
#include "./ha_rocksdb_proto.h"
#include "./rdb_datadic.h"

namespace myrocks {

class Rdb_compact_filter : public rocksdb::CompactionFilter
{
 public:
  Rdb_compact_filter(const Rdb_compact_filter&) = delete;
  Rdb_compact_filter& operator=(const Rdb_compact_filter&) = delete;

  explicit Rdb_compact_filter(uint32_t _cf_id) : m_cf_id(_cf_id) {}
  ~Rdb_compact_filter() {}

  // keys are passed in sorted order within the same sst.
  // V1 Filter is thread safe on our usage (creating from Factory).
  // Make sure to protect instance variables when switching to thread
  // unsafe in the future.
  virtual bool Filter(int level,
                      const rocksdb::Slice& key,
                      const rocksdb::Slice& existing_value,
                      std::string* new_value,
                      bool* value_changed) const override
  {
    DBUG_ASSERT(key.size() >= sizeof(uint32));

    GL_INDEX_ID gl_index_id;
    gl_index_id.cf_id= m_cf_id;
    gl_index_id.index_id= rdb_netbuf_to_uint32((const uchar*)key.data());
    DBUG_ASSERT(gl_index_id.index_id >= 1);

    if (gl_index_id != m_prev_index)  // processing new index id
    {
      if (m_num_deleted > 0)
      {
        m_num_deleted= 0;
      }
      m_should_delete=
          rdb_get_dict_manager()->is_drop_index_ongoing(gl_index_id);
      m_prev_index= gl_index_id;
    }

    if (m_should_delete)
    {
      m_num_deleted++;
    }

    return m_should_delete;
  }

  virtual bool IgnoreSnapshots() const override { return true; }

  virtual const char* Name() const override
  {
    return "Rdb_compact_filter";
  }

 private:
  // Column family for this compaction filter
  const uint32_t m_cf_id;
  // Index id of the previous record
  mutable GL_INDEX_ID m_prev_index= {0, 0};
  // Number of rows deleted for the same index id
  mutable uint64 m_num_deleted= 0;
  // Current index id should be deleted or not (should be deleted if true)
  mutable bool m_should_delete= false;
};

class Rdb_compact_filter_factory : public rocksdb::CompactionFilterFactory
{
 public:
  Rdb_compact_filter_factory(const Rdb_compact_filter_factory&) = delete;
  Rdb_compact_filter_factory& operator=(const Rdb_compact_filter_factory&) = delete;
  Rdb_compact_filter_factory() {}

  ~Rdb_compact_filter_factory() {}

  const char* Name() const override
  {
    return "Rdb_compact_filter_factory";
  }

  std::unique_ptr<rocksdb::CompactionFilter> CreateCompactionFilter(
      const rocksdb::CompactionFilter::Context& context) override
  {
    return std::unique_ptr<rocksdb::CompactionFilter>(
      new Rdb_compact_filter(context.column_family_id));
  }
};

}  // namespace myrocks
