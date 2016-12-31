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
#pragma implementation        // gcc: Class implementation
#endif

/* This C++ files header file */
#include "./rdb_cf_manager.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"

namespace myrocks {

/* Check if ColumnFamily name says it's a reverse-ordered CF */
bool Rdb_cf_manager::is_cf_name_reverse(const char* const name)
{
  /* nullptr means the default CF is used.. (TODO: can the default CF be
   * reverse?) */
  if (name && !strncmp(name, "rev:", 4))
    return true;
  else
    return false;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key ex_key_cfm;
#endif

void Rdb_cf_manager::init(
  Rdb_cf_options* const cf_options,
  std::vector<rocksdb::ColumnFamilyHandle*>* const handles)
{
  mysql_mutex_init(ex_key_cfm, &m_mutex, MY_MUTEX_INIT_FAST);

  DBUG_ASSERT(cf_options != nullptr);
  DBUG_ASSERT(handles != nullptr);
  DBUG_ASSERT(handles->size() > 0);

  m_cf_options = cf_options;

  for (auto cfh : *handles) {
    DBUG_ASSERT(cfh != nullptr);
    m_cf_name_map[cfh->GetName()] = cfh;
    m_cf_id_map[cfh->GetID()] = cfh;
  }
}


void Rdb_cf_manager::cleanup()
{
  for (auto it : m_cf_name_map) {
    delete it.second;
  }
  mysql_mutex_destroy(&m_mutex);
}


/**
  Generate Column Family name for per-index column families

  @param res  OUT  Column Family name
*/

void Rdb_cf_manager::get_per_index_cf_name(const std::string& db_table_name,
                                           const char* const index_name,
                                           std::string* const res)
{
  DBUG_ASSERT(index_name != nullptr);
  DBUG_ASSERT(res != nullptr);

  *res = db_table_name + "." + index_name;
}


/*
  @brief
  Find column family by name. If it doesn't exist, create it

  @detail
    See Rdb_cf_manager::get_cf
*/
rocksdb::ColumnFamilyHandle*
Rdb_cf_manager::get_or_create_cf(rocksdb::DB* const rdb,
                                 const char *cf_name,
                                 const std::string& db_table_name,
                                 const char* const index_name,
                                 bool* const is_automatic)
{
  DBUG_ASSERT(rdb != nullptr);
  DBUG_ASSERT(is_automatic != nullptr);

  rocksdb::ColumnFamilyHandle* cf_handle;

  mysql_mutex_lock(&m_mutex);
  *is_automatic= false;
  if (cf_name == nullptr)
    cf_name= DEFAULT_CF_NAME;

  std::string per_index_name;
  if (!strcmp(cf_name, PER_INDEX_CF_NAME))
  {
    get_per_index_cf_name(db_table_name, index_name, &per_index_name);
    cf_name= per_index_name.c_str();
    *is_automatic= true;
  }

  const auto it = m_cf_name_map.find(cf_name);
  if (it != m_cf_name_map.end())
    cf_handle= it->second;
  else
  {
    /* Create a Column Family. */
    const std::string cf_name_str(cf_name);
    rocksdb::ColumnFamilyOptions opts;
    m_cf_options->get_cf_options(cf_name_str, &opts);

    sql_print_information("RocksDB: creating column family %s", cf_name_str.c_str());
    sql_print_information("    write_buffer_size=%ld",    opts.write_buffer_size);
    sql_print_information("    target_file_size_base=%" PRIu64,
                          opts.target_file_size_base);

    const rocksdb::Status s=
      rdb->CreateColumnFamily(opts, cf_name_str, &cf_handle);
    if (s.ok()) {
      m_cf_name_map[cf_handle->GetName()] = cf_handle;
      m_cf_id_map[cf_handle->GetID()] = cf_handle;
    } else {
      cf_handle= nullptr;
    }
  }
  mysql_mutex_unlock(&m_mutex);

  return cf_handle;
}


/*
  Find column family by its cf_name.

  @detail
  dbname.tablename  and index_name are also parameters, because
  cf_name=PER_INDEX_CF_NAME means that column family name is a function
  of table/index name.

  @param out is_automatic  TRUE<=> column family name is auto-assigned based on
                           db_table_name and index_name.
*/

rocksdb::ColumnFamilyHandle*
Rdb_cf_manager::get_cf(const char *cf_name,
                       const std::string& db_table_name,
                       const char* const index_name,
                       bool* const is_automatic) const
{
  DBUG_ASSERT(is_automatic != nullptr);

  rocksdb::ColumnFamilyHandle* cf_handle;

  *is_automatic= false;
  mysql_mutex_lock(&m_mutex);
  if (cf_name == nullptr)
    cf_name= DEFAULT_CF_NAME;

  std::string per_index_name;
  if (!strcmp(cf_name, PER_INDEX_CF_NAME))
  {
    get_per_index_cf_name(db_table_name, index_name, &per_index_name);
    cf_name= per_index_name.c_str();
    *is_automatic= true;
  }

  const auto it = m_cf_name_map.find(cf_name);
  cf_handle = (it != m_cf_name_map.end()) ? it->second : nullptr;

  mysql_mutex_unlock(&m_mutex);

  return cf_handle;
}

rocksdb::ColumnFamilyHandle* Rdb_cf_manager::get_cf(const uint32_t &id) const
{
  rocksdb::ColumnFamilyHandle* cf_handle = nullptr;

  mysql_mutex_lock(&m_mutex);
  const auto it = m_cf_id_map.find(id);
  if (it != m_cf_id_map.end())
    cf_handle = it->second;
  mysql_mutex_unlock(&m_mutex);

  return cf_handle;
}

std::vector<std::string>
Rdb_cf_manager::get_cf_names(void) const
{
  std::vector<std::string> names;

  mysql_mutex_lock(&m_mutex);
  for (auto it : m_cf_name_map) {
    names.push_back(it.first);
  }
  mysql_mutex_unlock(&m_mutex);
  return names;
}

std::vector<rocksdb::ColumnFamilyHandle*>
Rdb_cf_manager::get_all_cf(void) const
{
  std::vector<rocksdb::ColumnFamilyHandle*> list;

  mysql_mutex_lock(&m_mutex);
  for (auto it : m_cf_id_map) {
    list.push_back(it.second);
  }
  mysql_mutex_unlock(&m_mutex);

  return list;
}

}  // namespace myrocks
