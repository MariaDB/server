/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2013 Kentoku SHIBA
  Copyright(C) 2011-2013 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef MRN_PARAMETERS_PARSER_HPP_
#define MRN_PARAMETERS_PARSER_HPP_

#include <mrn_mysql.h>
#include <my_list.h>

namespace mrn {
  class ParametersParser {
  public:
    ParametersParser(const char *input, unsigned int input_length);
    ~ParametersParser();
    void parse();
    const char *operator[](const char *key);

  private:
    const char *input_;
    unsigned int input_length_;

    LIST *parameters_;

    bool is_white_space(char character) {
      switch (character) {
      case ' ':
      case '\r':
      case '\n':
      case '\t':
        return true;
        break;
      default:
        return false;
        break;
      }
    };
    const char *parse_value(const char *current, const char *end,
                            const char *key, unsigned int key_length);
  };
}

#endif /* MRN_PARAMETERS_PARSER_HPP_ */
