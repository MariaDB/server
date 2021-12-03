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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation  // gcc: Class implementation
#endif

#include <my_global.h>

/* This C++ files header file */
#include "./rdb_cf_options.h"

/* C++ system header files */
#include <string>

/* MySQL header files */
#include "./log.h"

/* RocksDB header files */
#include "rocksdb/utilities/convenience.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./rdb_cf_manager.h"
#include "./rdb_compact_filter.h"

namespace myrocks {

Rdb_pk_comparator Rdb_cf_options::s_pk_comparator;
Rdb_rev_comparator Rdb_cf_options::s_rev_pk_comparator;

bool Rdb_cf_options::init(
    const rocksdb::BlockBasedTableOptions &table_options,
    std::shared_ptr<rocksdb::TablePropertiesCollectorFactory> prop_coll_factory,
    const char *const default_cf_options,
    const char *const override_cf_options) {
  DBUG_ASSERT(default_cf_options != nullptr);
  DBUG_ASSERT(override_cf_options != nullptr);

  m_default_cf_opts.comparator = &s_pk_comparator;
  m_default_cf_opts.compaction_filter_factory.reset(
      new Rdb_compact_filter_factory);

  m_default_cf_opts.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));

  if (prop_coll_factory) {
    m_default_cf_opts.table_properties_collector_factories.push_back(
        prop_coll_factory);
  }

  if (!set_default(std::string(default_cf_options)) ||
      !set_override(std::string(override_cf_options))) {
    return false;
  }

  return true;
}

void Rdb_cf_options::get(const std::string &cf_name,
                         rocksdb::ColumnFamilyOptions *const opts) {
  DBUG_ASSERT(opts != nullptr);

  // Get defaults.
  rocksdb::GetColumnFamilyOptionsFromString(*opts, m_default_config, opts);

  // Get a custom confguration if we have one.
  Name_to_config_t::iterator it = m_name_map.find(cf_name);

  if (it != m_name_map.end()) {
    rocksdb::GetColumnFamilyOptionsFromString(*opts, it->second, opts);
  }
}

void Rdb_cf_options::update(const std::string &cf_name,
                            const std::string &cf_options) {
  DBUG_ASSERT(!cf_name.empty());
  DBUG_ASSERT(!cf_options.empty());

  // Always update. If we didn't have an entry before then add it.
  m_name_map[cf_name] = cf_options;

  DBUG_ASSERT(!m_name_map.empty());
}

bool Rdb_cf_options::set_default(const std::string &default_config) {
  rocksdb::ColumnFamilyOptions options;

  if (!default_config.empty() && !rocksdb::GetColumnFamilyOptionsFromString(
                                      options, default_config, &options)
                                      .ok()) {
    // NO_LINT_DEBUG
    fprintf(stderr, "Invalid default column family config: %s\n",
            default_config.c_str());
    return false;
  }

  m_default_config = default_config;
  return true;
}

// Skip over any spaces in the input string.
void Rdb_cf_options::skip_spaces(const std::string &input, size_t *const pos) {
  DBUG_ASSERT(pos != nullptr);

  while (*pos < input.size() && isspace(input[*pos])) ++(*pos);
}

// Find a valid column family name.  Note that all characters except a
// semicolon are valid (should this change?) and all spaces are trimmed from
// the beginning and end but are not removed between other characters.
bool Rdb_cf_options::find_column_family(const std::string &input,
                                        size_t *const pos,
                                        std::string *const key) {
  DBUG_ASSERT(pos != nullptr);
  DBUG_ASSERT(key != nullptr);

  const size_t beg_pos = *pos;
  size_t end_pos = *pos - 1;

  // Loop through the characters in the string until we see a '='.
  for (; *pos < input.size() && input[*pos] != '='; ++(*pos)) {
    // If this is not a space, move the end position to the current position.
    if (input[*pos] != ' ') end_pos = *pos;
  }

  if (end_pos == beg_pos - 1) {
    // NO_LINT_DEBUG
    sql_print_warning("No column family found (options: %s)", input.c_str());
    return false;
  }

  *key = input.substr(beg_pos, end_pos - beg_pos + 1);
  return true;
}

// Find a valid options portion.  Everything is deemed valid within the options
// portion until we hit as many close curly braces as we have seen open curly
// braces.
bool Rdb_cf_options::find_options(const std::string &input, size_t *const pos,
                                  std::string *const options) {
  DBUG_ASSERT(pos != nullptr);
  DBUG_ASSERT(options != nullptr);

  // Make sure we have an open curly brace at the current position.
  if (*pos < input.size() && input[*pos] != '{') {
    // NO_LINT_DEBUG
    sql_print_warning("Invalid cf options, '{' expected (options: %s)",
                      input.c_str());
    return false;
  }

  // Skip the open curly brace and any spaces.
  ++(*pos);
  skip_spaces(input, pos);

  // Set up our brace_count, the begin position and current end position.
  size_t brace_count = 1;
  const size_t beg_pos = *pos;

  // Loop through the characters in the string until we find the appropriate
  // number of closing curly braces.
  while (*pos < input.size()) {
    switch (input[*pos]) {
      case '}':
        // If this is a closing curly brace and we bring the count down to zero
        // we can exit the loop with a valid options string.
        if (--brace_count == 0) {
          *options = input.substr(beg_pos, *pos - beg_pos);
          ++(*pos);  // Move past the last closing curly brace
          return true;
        }

        break;

      case '{':
        // If this is an open curly brace increment the count.
        ++brace_count;
        break;

      default:
        break;
    }

    // Move to the next character.
    ++(*pos);
  }

  // We never found the correct number of closing curly braces.
  // Generate an error.
  // NO_LINT_DEBUG
  sql_print_warning("Mismatched cf options, '}' expected (options: %s)",
                    input.c_str());
  return false;
}

bool Rdb_cf_options::find_cf_options_pair(const std::string &input,
                                          size_t *const pos,
                                          std::string *const cf,
                                          std::string *const opt_str) {
  DBUG_ASSERT(pos != nullptr);
  DBUG_ASSERT(cf != nullptr);
  DBUG_ASSERT(opt_str != nullptr);

  // Skip any spaces.
  skip_spaces(input, pos);

  // We should now have a column family name.
  if (!find_column_family(input, pos, cf)) return false;

  // If we are at the end of the input then we generate an error.
  if (*pos == input.size()) {
    // NO_LINT_DEBUG
    sql_print_warning("Invalid cf options, '=' expected (options: %s)",
                      input.c_str());
    return false;
  }

  // Skip equal sign and any spaces after it
  ++(*pos);
  skip_spaces(input, pos);

  // Find the options for this column family.  This should be in the format
  // {<options>} where <options> may contain embedded pairs of curly braces.
  if (!find_options(input, pos, opt_str)) return false;

  // Skip any trailing spaces after the option string.
  skip_spaces(input, pos);

  // We should either be at the end of the input string or at a semicolon.
  if (*pos < input.size()) {
    if (input[*pos] != ';') {
      // NO_LINT_DEBUG
      sql_print_warning("Invalid cf options, ';' expected (options: %s)",
                        input.c_str());
      return false;
    }

    ++(*pos);
  }

  return true;
}

bool Rdb_cf_options::parse_cf_options(const std::string &cf_options,
                                      Name_to_config_t *option_map) {
  std::string cf;
  std::string opt_str;
  rocksdb::ColumnFamilyOptions options;

  DBUG_ASSERT(option_map != nullptr);
  DBUG_ASSERT(option_map->empty());

  // Loop through the characters of the string until we reach the end.
  size_t pos = 0;

  while (pos < cf_options.size()) {
    // Attempt to find <cf>={<opt_str>}.
    if (!find_cf_options_pair(cf_options, &pos, &cf, &opt_str)) {
      return false;
    }

    // Generate an error if we have already seen this column family.
    if (option_map->find(cf) != option_map->end()) {
      // NO_LINT_DEBUG
      sql_print_warning(
          "Duplicate entry for %s in override options (options: %s)",
          cf.c_str(), cf_options.c_str());
      return false;
    }

    // Generate an error if the <opt_str> is not valid according to RocksDB.
    if (!rocksdb::GetColumnFamilyOptionsFromString(options, opt_str, &options)
             .ok()) {
      // NO_LINT_DEBUG
      sql_print_warning(
          "Invalid cf config for %s in override options (options: %s)",
          cf.c_str(), cf_options.c_str());
      return false;
    }

    // If everything is good, add this cf/opt_str pair to the map.
    (*option_map)[cf] = opt_str;
  }

  return true;
}

bool Rdb_cf_options::set_override(const std::string &override_config) {
  Name_to_config_t configs;

  if (!parse_cf_options(override_config, &configs)) {
    return false;
  }

  // Everything checked out - make the map live
  m_name_map = configs;

  return true;
}

const rocksdb::Comparator *Rdb_cf_options::get_cf_comparator(
    const std::string &cf_name) {
  if (Rdb_cf_manager::is_cf_name_reverse(cf_name.c_str())) {
    return &s_rev_pk_comparator;
  } else {
    return &s_pk_comparator;
  }
}

std::shared_ptr<rocksdb::MergeOperator> Rdb_cf_options::get_cf_merge_operator(
    const std::string &cf_name) {
  return (cf_name == DEFAULT_SYSTEM_CF_NAME)
             ? std::make_shared<Rdb_system_merge_op>()
             : nullptr;
}

void Rdb_cf_options::get_cf_options(const std::string &cf_name,
                                    rocksdb::ColumnFamilyOptions *const opts) {
  *opts = m_default_cf_opts;
  get(cf_name, opts);

  // Set the comparator according to 'rev:'
  opts->comparator = get_cf_comparator(cf_name);
  opts->merge_operator = get_cf_merge_operator(cf_name);
}

}  // namespace myrocks
