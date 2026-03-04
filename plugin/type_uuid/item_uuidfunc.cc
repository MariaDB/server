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
#include <string>
#include <cctype>

String *Item_func_sys_guid::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  str->alloc(uuid_len()+1);
  str->length(uuid_len());
  str->set_charset(collation.collation);

  uchar buf[MY_UUID_SIZE];
  my_uuid(buf);
  my_uuid2str(buf, const_cast<char *>(str->ptr()), 0);
  return str;
}

static bool is_valid_uuid_format_any(const std::string &str)
{
  if (str.size() != 36)
  {
    return false;
  }
  for (size_t i= 0; i < str.size(); ++i)
  {
    // check if the character is a valid hexadecimal digit
    // or a hyphen at the correct positions (8, 13, 18, 23)
    if (i == 8 || i == 13 || i == 18 || i == 23)
    {
      if (str[i] != '-')
      {
        return false;
      }
    }
    else
    {
      if (!isxdigit(str[i]))
      {
        return false;
      }
    }
  }
  return true;
}

static int return_uuid_version(const std::string &str)
{
  if (!is_valid_uuid_format_any(str))
  {
    return -1;
  }
  if (!isxdigit(str[14]))
  {
    return -1;
  }
  // Currently the only supported versions are below 9, as per RFC 9562.
  // If the version is above 8, it's either not a valid UUID or it's a
  // future version that we don't support, so we return -1.
  // 0 is also not a valid version number for a UUID, so we return -1 for
  // that as well.
  int version= str[14] - '0';
  if (version < 1 || version > 8)
  {
    return -1;
  }
  return (str[14] <= '9') ? (str[14] - '0')
                          : (static_cast<char>(std::tolower(
                                 static_cast<unsigned char>(str[14]))) -
                             'a' + 10);
}

longlong Item_func_uuid_version::val_int()
{
  String in_tmp;
  String *uuid_arg= args[0]->val_str(&in_tmp);
  if (!uuid_arg)
  {
    null_value= true;
    return 0;
  }

  std::string in_str(uuid_arg->ptr(), uuid_arg->length());
  int version= return_uuid_version(in_str);
  if (version < 0)
  {
    my_printf_error(ER_TRUNCATED_WRONG_VALUE,
                    "Incorrect uuid value: '%-.128s'", MYF(0), in_str.c_str());
    null_value= true;
    return 0;
  }

  // For each valid version, we check the variant and reserved
  // bits to ensure the UUID is well-formed according to the RFC.
  const unsigned char variant_ch= static_cast<unsigned char>(
      std::tolower(static_cast<unsigned char>(in_str[19])));

  // RFC 9562 variant must be 10xx, i.e. nibble 8, 9, a, or b.
  if (variant_ch != '8' && variant_ch != '9' && variant_ch != 'a' &&
      variant_ch != 'b')
  {
    my_printf_error(ER_TRUNCATED_WRONG_VALUE,
                    "Incorrect uuid value (variant check failed): '%-.128s'",
                    MYF(0), in_str.c_str());
    null_value= true;
    return 0;
  }

  // Display warning if version 2 as it is deprecated
  if (version == 2)
  {
    my_printf_error(ER_INVALID_CHARACTER_STRING,
                    "Version 2 UUIDs are deprecated", ME_WARNING);
  }

  null_value= false;
  return version;
}
