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

#ifndef VIDEX_JSON_ITEM_H
#define VIDEX_JSON_ITEM_H

#include <iostream>
#include <string>
#include <map>
#include <sstream>
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include "sql_string.h"
#include <fstream>

typedef std::map<std::string, std::string> VidexStringMap;

inline bool videx_contains_key(const VidexStringMap &myMap, const std::string &key) {
    return myMap.find(key) != myMap.end();
}

int videx_parse_simple_json(const std::string &json, int &code, std::string &message,
                      std::map<std::string, std::string> &data_dict);


std::string videx_escape_double_quotes(const std::string &input,
                               size_t len = std::string::npos);

class VidexJsonItem {
public:
    std::string item_type;
    std::map<std::string, std::string> properties;
    std::list<VidexJsonItem> data;
    int depth;

    VidexJsonItem()
            : item_type("empty"), depth(0) {}

    /// specify item_type
    VidexJsonItem(const std::string &item_type, int depth)
            : item_type(item_type), depth(depth) {}

    /// create a new VidexJsonItem，插入data，然后返回这个VidexJsonItem的引用
    VidexJsonItem *create(const std::string &new_item_type) {
        data.push_back(VidexJsonItem(new_item_type, depth + 1));
        return &data.back();
    }

    VidexJsonItem *create(const std::string &item_type, const char *prompt) {
        VidexJsonItem newOne = VidexJsonItem(item_type, depth + 1);
        newOne.add_property("prompt", prompt);
        data.push_back(newOne);
        return &data.back();
    }

    /// add to properties
    void add_property(const std::string &key, const std::string &value) {
        properties[key] = videx_escape_double_quotes(value);
    }

    void add_property(const std::string &key, const char *value) {
        if (value != NULL) {
            properties[key] = videx_escape_double_quotes(value);
        } else {
            properties[key] = "NULL";
        }
    }

    void add_property(const std::string &key, const Simple_cstring &value) {
        if (value.is_set() && value.ptr() != NULL) {
            properties[key] = videx_escape_double_quotes(value.ptr(), value.length());
        } else {
            properties[key] = "NULL";
        }
    }

    void add_property(const std::string &key, const String &value) {
        if (!value.is_alloced() || !value.ptr() || !value.alloced_length() ||
            (value.alloced_length() < (value.length() + 1))) {
            properties[key] = "NULL";
        } else {
            properties[key] = videx_escape_double_quotes(value.ptr(), value.length());
        }
    }

    void add_property(const std::string &key, const String *value) {
        if (value == NULL) {
            properties[key] = "NULL";
        } else {
            add_property(key, *value);
        }
    }

    template<typename V>
    // Except for string which might be empty and needs to be converted to NULL separately,
    // all other values can be handled using this function.
    void add_property_nonan(const std::string &key, V value) {
        std::stringstream ss;
        ss << value;
        properties[key] = ss.str();
    }

    std::string to_json() const {
        std::string json = "{";

        json += "\"item_type\":\"" + item_type + "\",";

        json += "\"properties\":{";
        for (std::map<std::string, std::string>::const_iterator it =
                properties.begin();
             it != properties.end(); ++it) {
            json += "\"" + it->first + "\":\"" + it->second + "\",";
        }
        if (!properties.empty()) {
            json.erase(json.length() - 1);  // remove trailing comma
        }
        json += "},";

        json += "\"data\":[";
        for (std::list<VidexJsonItem>::const_iterator it = data.begin();
             it != data.end(); ++it) {
            json += it->to_json() + ",";
        }
        if (!data.empty()) {
            json.erase(json.length() - 1);  // remove trailing comma
        }
        json += "]}";

        return json;
    }
};

/**
 * construct a basic request, and other parameters can be conveniently added externally.
*/
inline VidexJsonItem construct_request(const std::string &db_name,
                                       const std::string &table_name,
                                       const std::string &function,
                                       const std::string &target_storage_engine = "INNODB") {
    VidexJsonItem req("videx_request", 0);
    req.add_property("dbname", db_name);
    req.add_property("table_name", table_name);
    req.add_property("function", function);
    req.add_property("target_storage_engine", target_storage_engine);
    return req;
}

#endif  // VIDEX_JSON_ITEM_H
