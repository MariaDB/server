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

#ifndef VIDEX_LOG_UTILS
#define VIDEX_LOG_UTILS

#include <unordered_map>
#include <iostream>
#include <sstream>
#include <my_base.h>
#include "sql/key.h"
#include "sql_string.h"
#include <string>
#include "join_optimizer/bit_utils.h"
#include "sql/current_thd.h"
#include "sql/field.h"
#include "sql/item_func.h"
#include "sql/sql_class.h"
#include "videx_json_item.h"

#define FUNC_FILE_LINE __PRETTY_FUNCTION__, __FILE__, __LINE__


class VidexLogUtils {
private:
    int count = 0;
    std::string tag = "^_^";
    bool enable_cout = true;
    bool enable_trace = false;
    std::unordered_map<std::string, std::string> data;
public:

    /**
     * whether to show cout
     * @param p_show_cout
     */
    void set_cout(bool p_show_cout) {
        this->enable_cout = p_show_cout;
    }

    /**
     * whether to show trace
     * @param p_enable_trace
     */
    void set_enable_trace(bool p_enable_trace) {
        this->enable_trace = p_enable_trace;
    }

    /**
     * set tag and to be displayed in markHaFuncPassby
     * @param new_tag
     */
    void set_tag(const std::string &new_tag) {
        this->tag = new_tag;
    }

    void markHaFuncPassby(const std::string &func, const std::string &file, const int line,
                          const std::string &others = "", bool silent = true);

    void markPassbyUnexpected(const std::string &func, const std::string &file, const int line);

    void NotMarkPassby(const std::string &, const std::string &, const int );
    
    template<typename V>
    void markPassby_DBTB_otherType(const std::string &func, const std::string &file, const int line, 
    const std::string &db_name, const std::string &tb_name, 
    V value) {
        // markHaFuncPassby(func, file, line, std::to_string(value));
        std::ostringstream oss;
        oss << "db=" << db_name << ", tb=" << tb_name << ", value=" << value;
        markHaFuncPassby(func, file, line, oss.str(), false);
    }

    template<typename V>
    void markPassby_otherType(const std::string &func, const std::string &file, const int line, V value) {
        // markHaFuncPassby(func, file, line, std::to_string(value));
        std::ostringstream oss;
        oss << value;
        markHaFuncPassby(func, file, line, oss.str());
    }

    void markRecordInRange([[maybe_unused]]const std::string &func, [[maybe_unused]]const std::string &file,
                           [[maybe_unused]]const int line, key_range *min_key, key_range *max_key,
                           KEY *key, VidexJsonItem *req_json);
};


extern VidexLogUtils videx_log_ins;


#endif // VIDEX_LOG_UTILS