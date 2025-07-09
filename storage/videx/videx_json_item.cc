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

#include "videx_json_item.h"

//test

/**
 * A simple parsing function is written here instead,
 * since rapid_json always encounters strange segmentation faults across platforms,
 *
 * @param json
 * @param code
 * @param message
 * @param data_dict
 * @return
 */
int videx_parse_simple_json(const std::string &json, int &code, std::string &message,
                      std::map<std::string, std::string> &data_dict) {
    try {
        // find code and message
        std::size_t pos_code = json.find("\"code\":");
        std::size_t pos_message = json.find("\"message\":");
        std::size_t pos_data = json.find("\"data\":");

        if (pos_code == std::string::npos || pos_message == std::string::npos || pos_data == std::string::npos) {
            throw std::invalid_argument("Missing essential components in JSON.");
        }

        // parse code
        std::size_t start = json.find_first_of("0123456789", pos_code);
        std::size_t end = json.find(',', start);
        code = std::stoi(json.substr(start, end - start));

        // parse message
        start = json.find('\"', pos_message + 10) + 1;
        end = json.find('\"', start);
        message = json.substr(start, end - start);

        // parse data
        start = json.find('{', pos_data) + 1;
        end = json.find('}', start);
        std::string data_content = json.substr(start, end - start);
        std::istringstream data_stream(data_content);
        std::string line;

        while (std::getline(data_stream, line, ',')) {
            std::size_t colon_pos = line.find(':');
            if (colon_pos == std::string::npos) {
                continue; // Skip malformed line
            }
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // clean key 和 value
            auto trim_quotes_and_space = [](std::string &str) {
                // Trim whitespace and surrounding quotes
                size_t first = str.find_first_not_of(" \t\n\"");
                size_t last = str.find_last_not_of(" \t\n\"");
                if (first == std::string::npos || last == std::string::npos) {
                    str.clear(); // All whitespace or empty
                } else {
                    str = str.substr(first, last - first + 1);
                }
            };

            trim_quotes_and_space(key);
            trim_quotes_and_space(value);

            data_dict[key] = value;
        }

        return 0;
    } catch (std::exception &e) {
        std::cerr << "Failed to parse JSON: " << e.what() << std::endl;
        message = e.what();
        code = -1;
        return 1;
    }
}


/**
 * This function is used to escape double quotes in a string.
 * @param input
 * @param len
 * @return
 */
std::string videx_escape_double_quotes(const std::string &input, size_t len) {
    if (len == std::string::npos) len = input.length();

    //  if (len > input.length()) {
    //    throw std::invalid_argument("Length exceeds input string size");
    //  }

    std::string output = input.substr(0, len);
    size_t pos = output.find('\\');
    while (pos != std::string::npos) {
        output.replace(pos, 1, "\\\\");
        pos = output.find('\\', pos + 2);
    }
    // replace "
    pos = output.find('\"');
    while (pos != std::string::npos) {
        output.replace(pos, 1, "\\\"");
        pos = output.find('\"', pos + 2);
    }

    // replace \n with space
    pos = output.find('\n');
    while (pos != std::string::npos) {
        output.replace(pos, 1, " ");
        pos = output.find('\n', pos + 1);
    }

    // replace \t with space
    pos = output.find('\t');
    while (pos != std::string::npos) {
        output.replace(pos, 1, " ");
        pos = output.find('\t', pos + 1);
    }
    return output;
}
