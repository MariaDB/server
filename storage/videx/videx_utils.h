/* Copyright (c) 2024 Bytedance Ltd. and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file videx_utils.h
 *
 * @brief
 * Lightweight JSON utilities for MariaDB storage engine development:
 * - VidexJsonItem class: Hierarchical JSON builder.
 * - Simple parser: Extracts code/message/data from flat JSON structures, at most 2-level nested.
 * @note
 * 1. Parser supports max 2-level nested JSON structures
 * 2. Builder automatically escapes special characters (\"\\)
 * 3. Use construct_request() for standard API request templates
 * 4. Cross-platform: Avoids rapid_json segmentation faults on macOS
 * 5. Serializes key_range to JSON object
 *
 * @see
 * - Implementation: videx_utils.cc
 */


#ifndef VIDEX_UTILS
#define VIDEX_UTILS

#include <my_global.h>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <sstream>
#include <string>
#include <my_base.h>
#include <key.h>
#include <structs.h>
#include <field.h>
#include <table.h>
#include <mysqld.h>
#include <map>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <list>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sql_string.h>
#include <fstream>

typedef std::map<std::string, std::string> VidexStringMap;

static inline bool videx_contains_key(const VidexStringMap &myMap,
                               const std::string &key)
{
  return myMap.find(key) != myMap.end();
}

int videx_parse_simple_json(const std::string &json, int &code,
                            std::string &message,
                            std::map<std::string, std::string> &data_dict);

std::string videx_escape_double_quotes(const std::string &input,
                                       size_t len= std::string::npos);

class VidexJsonItem
{
public:
  std::string item_type;
  std::map<std::string, std::string> properties;
  std::list<VidexJsonItem> data;
  int depth;

  VidexJsonItem() : item_type("empty"), depth(0) {}

  VidexJsonItem(const std::string &item_type, int depth)
      : item_type(item_type), depth(depth) {}

  VidexJsonItem *create(const std::string &new_item_type);

  VidexJsonItem *create(const std::string &item_type, const char *prompt);

  void add_property(const std::string &key, const std::string &value);

  void add_property(const std::string &key, const char *value);

  void add_property(const std::string &key, const String &value);

  void add_property(const std::string &key, const String *value);

  // Except for string which might be empty and needs to be converted to NULL
  // separately, all other values can be handled using this function.
  template <typename V>
  void add_property_nonan(const std::string &key, V value);

  std::string to_json() const;
};

/**
construct a basic request, and other parameters can be conveniently added
externally. */

static inline VidexJsonItem
construct_request(const std::string &db_name, const std::string &table_name,
                  const std::string &function,
                  const std::string &target_storage_engine= "INNODB")
{
  VidexJsonItem req("videx_request", 0);
  req.add_property("dbname", db_name);
  req.add_property("table_name", table_name);
  req.add_property("function", function);
  req.add_property("target_storage_engine", target_storage_engine);
  return req;
}

/**
  Serializes min/max key bounds for a given index into `req_json`.
  Also prints a concise human-readable summary for debugging.

  @param min_key Minimum key range.
  @param max_key Maximum key range.
  @param key Index key information.
  @param req_json JSON object to store serialized key range.
*/
void serializeKeyRangeToJson(const key_range *min_key,
                             const key_range *max_key, KEY *key,
                             VidexJsonItem *req_json);

#endif // VIDEX_UTILS
