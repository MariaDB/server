/* Copyright (c) 2019,2024, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER
#include "mariadb.h"
#include "item_uuidfunc.h"

String *Item_func_sys_guid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str->alloc(uuid_len()+1);
  str->length(uuid_len());
  str->set_charset(collation.collation);

  uchar buf[MY_UUID_SIZE];
  my_uuid(buf);
  my_uuid2str(buf, const_cast<char*>(str->ptr()), 0);
  return str;
}

bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_valid_uuid_format_any(const std::string &str) {
    if (str.size() != 36) {
        return false;
    }
    if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
        return false;
    }
    for (size_t i = 0; i < str.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }
        if (!is_hex_char(str[i])) {
            return false;
        }
    }
    return true;
}

int return_uuid_version(const std::string &str) {
    if (!is_valid_uuid_format_any(str)) {
            return -1;
    }
    if (!is_hex_char(str[14])) {
        return -1;
    }
    return (str[14] <= '9') ? (str[14] - '0') : (std::tolower(static_cast<unsigned char>(str[14])) - 'a' + 10);
}

longlong Item_func_uuid_version::val_int() {
  String in_tmp;
  String *uuid_arg = args[0]->val_str(&in_tmp);
  if (!uuid_arg) {
      null_value = true;
      return 0;
  }

  std::string in_str(uuid_arg->ptr(), uuid_arg->length());
  int version = return_uuid_version(in_str);
  if (version < 0) {
      my_printf_error(ER_UNKNOWN_ERROR,
          "uuid_version: not a valid UUID",
          0);
      null_value = true;
      return 0;
  }

  null_value = false;
  return version;
}
