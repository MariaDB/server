/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Kouhei Sutou <kou@clear-code.com>

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

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>

#include "mrn_column_name.hpp"

#include <strfunc.h>

#include <string.h>

// for debug
#define MRN_CLASS_NAME "mrn::ColumnName"

namespace mrn {
  ColumnName::ColumnName(const char *mysql_name)
    : mysql_name_(mysql_name) {
    encode(mysql_name, strlen(mysql_name));
  }

  ColumnName::ColumnName(const LEX_CSTRING &mysql_name)
    : mysql_name_(mysql_name.str) {
    encode(mysql_name.str, mysql_name.length);
  }

  const char *ColumnName::mysql_name() {
    return mysql_name_;
  }

  const char *ColumnName::c_str() {
    return name_;
  }

  size_t ColumnName::length() {
    return length_;
  }

  void ColumnName::encode(const char *mysql_name,
                          size_t mysql_name_length) {
    MRN_DBUG_ENTER_METHOD();
    uint errors;
    length_ = mrn_strconvert(system_charset_info,
                             mysql_name,
                             mysql_name_length,
                             &my_charset_filename,
                             name_,
                             MRN_MAX_PATH_SIZE,
                             &errors);
    name_[length_] = '\0';
    DBUG_VOID_RETURN;
  }
}
