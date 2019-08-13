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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#pragma once

/* C++ system header files */
#include <string>
#include <unordered_map>

/* RocksDB header files */
#include "rocksdb/table.h"
#include "rocksdb/utilities/options_util.h"

/* MyRocks header files */
#include "./rdb_comparator.h"

namespace myrocks {

/*
  Per-column family options configs.

  Per-column family option can be set
  - Globally (the same value applies to all column families)
  - Per column family: there is a {cf_name -> value} map,
    and also there is a default value which applies to column
    families not found in the map.
*/
class Rdb_cf_options {
 public:
  using Name_to_config_t = std::unordered_map<std::string, std::string>;

  Rdb_cf_options(const Rdb_cf_options &) = delete;
  Rdb_cf_options &operator=(const Rdb_cf_options &) = delete;
  Rdb_cf_options() = default;

  void get(const std::string &cf_name,
           rocksdb::ColumnFamilyOptions *const opts);

  void update(const std::string &cf_name, const std::string &cf_options);

  bool init(const rocksdb::BlockBasedTableOptions &table_options,
            std::shared_ptr<rocksdb::TablePropertiesCollectorFactory>
                prop_coll_factory,
            const char *const default_cf_options,
            const char *const override_cf_options);

  const rocksdb::ColumnFamilyOptions &get_defaults() const {
    return m_default_cf_opts;
  }

  static const rocksdb::Comparator *get_cf_comparator(
      const std::string &cf_name);

  std::shared_ptr<rocksdb::MergeOperator> get_cf_merge_operator(
      const std::string &cf_name);

  void get_cf_options(const std::string &cf_name,
                      rocksdb::ColumnFamilyOptions *const opts)
      MY_ATTRIBUTE((__nonnull__));

  static bool parse_cf_options(const std::string &cf_options,
                               Name_to_config_t *option_map);

 private:
  bool set_default(const std::string &default_config);
  bool set_override(const std::string &overide_config);

  /* Helper string manipulation functions */
  static void skip_spaces(const std::string &input, size_t *const pos);
  static bool find_column_family(const std::string &input, size_t *const pos,
                                 std::string *const key);
  static bool find_options(const std::string &input, size_t *const pos,
                           std::string *const options);
  static bool find_cf_options_pair(const std::string &input, size_t *const pos,
                                   std::string *const cf,
                                   std::string *const opt_str);

 private:
  static Rdb_pk_comparator s_pk_comparator;
  static Rdb_rev_comparator s_rev_pk_comparator;

  /* CF name -> value map */
  Name_to_config_t m_name_map;

  /* The default value (if there is only one value, it is stored here) */
  std::string m_default_config;

  rocksdb::ColumnFamilyOptions m_default_cf_opts;
};

}  // namespace myrocks
