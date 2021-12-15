/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2013 Kentoku SHIBA
  Copyright(C) 2011-2012 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_INDEX_COLUMN_NAME_HPP_
#define MRN_INDEX_COLUMN_NAME_HPP_

#include <mrn_constants.hpp>

namespace mrn {
  class IndexColumnName {
  public:
    IndexColumnName(const char *table_name, const char *mysql_column_name);
    const char *c_str();
    size_t length();
  private:
    const char *table_name_;
    const char *mysql_column_name_;
    char name_[MRN_MAX_KEY_SIZE];
    size_t length_;

    uint encode(uchar *encoded_start, uchar *encoded_end,
                const uchar *mysql_string_start, const uchar *mysql_string_end);
  };
}

#endif /* MRN_INDEX_COLUMN_NAME_HPP_ */
