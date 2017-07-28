/*
   Copyright (c) 2014, SkySQL Ab

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation // gcc: Class implementation
#endif

/* This C++ files header file */
#include "./rdb_cf_manager.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"
#include "./rdb_psi.h"

namespace myrocks {

/* Check if ColumnFamily name says it's a reverse-ordered CF */
bool Rdb_cf_manager::is_cf_name_reverse(const char *const name) {
  /* nullptr means the default CF is used.. (TODO: can the default CF be
   * reverse?) */
  return (name && !strncmp(name, "rev:", 4));
}

void Rdb_cf_manager::init(
    std::unique_ptr<Rdb_cf_options> cf_options,
    std::vector<rocksdb::ColumnFamilyHandle *> *const handles) {
  mysql_mutex_init(rdb_cfm_mutex_key, &m_mutex, MY_MUTEX_INIT_FAST);

  DBUG_ASSERT(cf_options != nullptr);
  DBUG_ASSERT(handles != nullptr);
  DBUG_ASSERT(handles->size() > 0);

  m_cf_options = std::move(cf_options);

  for (auto cfh : *handles) {
    DBUG_ASSERT(cfh != nullptr);
    m_cf_name_map[cfh->GetName()] = cfh;
    m_cf_id_map[cfh->GetID()] = cfh;
  }
}

void Rdb_cf_manager::cleanup() {
  for (auto it : m_cf_name_map) {
    delete it.second;
  }
  mysql_mutex_destroy(&m_mutex);
  m_cf_options = nullptr;
}

/*
  @brief
  Find column family by name. If it doesn't exist, create it

  @detail
    See Rdb_cf_manager::get_cf
*/
rocksdb::ColumnFamilyHandle *
Rdb_cf_manager::get_or_create_cf(rocksdb::DB *const rdb,
                                 const std::string &cf_name_arg) {
  DBUG_ASSERT(rdb != nullptr);

  rocksdb::ColumnFamilyHandle *cf_handle = nullptr;

  if (cf_name_arg == PER_INDEX_CF_NAME) {
    // per-index column families is no longer supported.
    my_error(ER_PER_INDEX_CF_DEPRECATED, MYF(0));
    return nullptr;
  }

  const std::string &cf_name =
      cf_name_arg.empty() ? DEFAULT_CF_NAME : cf_name_arg;

  RDB_MUTEX_LOCK_CHECK(m_mutex);

  const auto it = m_cf_name_map.find(cf_name);

  if (it != m_cf_name_map.end()) {
    cf_handle = it->second;
  } else {
    /* Create a Column Family. */
    rocksdb::ColumnFamilyOptions opts;
    m_cf_options->get_cf_options(cf_name, &opts);

    // NO_LINT_DEBUG
    sql_print_information("RocksDB: creating a column family %s",
                          cf_name.c_str());
    sql_print_information("    write_buffer_size=%ld", opts.write_buffer_size);
    sql_print_information("    target_file_size_base=%" PRIu64,
                          opts.target_file_size_base);

    const rocksdb::Status s =
        rdb->CreateColumnFamily(opts, cf_name, &cf_handle);

    if (s.ok()) {
      m_cf_name_map[cf_handle->GetName()] = cf_handle;
      m_cf_id_map[cf_handle->GetID()] = cf_handle;
    } else {
      cf_handle = nullptr;
    }
  }

  RDB_MUTEX_UNLOCK_CHECK(m_mutex);

  return cf_handle;
}

/*
  Find column family by its cf_name.
*/

rocksdb::ColumnFamilyHandle *
Rdb_cf_manager::get_cf(const std::string &cf_name_arg) const {
  rocksdb::ColumnFamilyHandle *cf_handle;

  RDB_MUTEX_LOCK_CHECK(m_mutex);

  std::string cf_name = cf_name_arg.empty() ? DEFAULT_CF_NAME : cf_name_arg;

  const auto it = m_cf_name_map.find(cf_name);
  cf_handle = (it != m_cf_name_map.end()) ? it->second : nullptr;

  if (!cf_handle) {
    // NO_LINT_DEBUG
    sql_print_warning("Column family '%s' not found.", cf_name.c_str());
  }

  RDB_MUTEX_UNLOCK_CHECK(m_mutex);

  return cf_handle;
}

rocksdb::ColumnFamilyHandle *Rdb_cf_manager::get_cf(const uint32_t &id) const {
  rocksdb::ColumnFamilyHandle *cf_handle = nullptr;

  RDB_MUTEX_LOCK_CHECK(m_mutex);
  const auto it = m_cf_id_map.find(id);
  if (it != m_cf_id_map.end())
    cf_handle = it->second;
  RDB_MUTEX_UNLOCK_CHECK(m_mutex);

  return cf_handle;
}

std::vector<std::string> Rdb_cf_manager::get_cf_names(void) const {
  std::vector<std::string> names;

  RDB_MUTEX_LOCK_CHECK(m_mutex);
  for (auto it : m_cf_name_map) {
    names.push_back(it.first);
  }
  RDB_MUTEX_UNLOCK_CHECK(m_mutex);

  return names;
}

std::vector<rocksdb::ColumnFamilyHandle *>
Rdb_cf_manager::get_all_cf(void) const {
  std::vector<rocksdb::ColumnFamilyHandle *> list;

  RDB_MUTEX_LOCK_CHECK(m_mutex);

  for (auto it : m_cf_id_map) {
    DBUG_ASSERT(it.second != nullptr);
    list.push_back(it.second);
  }

  RDB_MUTEX_UNLOCK_CHECK(m_mutex);

  return list;
}

} // namespace myrocks
