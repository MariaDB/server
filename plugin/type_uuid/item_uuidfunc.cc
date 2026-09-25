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
#include "uuid_utils.h"

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

bool is_valid_uuid_format_any(const std::string &str)
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
      if (!isxdigit(static_cast<unsigned char>(str[i])))
      {
        return false;
      }
    }
  }
  return true;
}

static int hex_digit_value(unsigned char ch)
{
  ch= static_cast<unsigned char>(std::tolower(ch));
  return (ch <= '9') ? ch - '0' : ch - 'a' + 10;
}

static int uuid_variant(unsigned char ch)
{
  int value= hex_digit_value(ch);
  if ((value & 0x8) == 0)
    return 0;
  if ((value & 0xc) == 0x8)
    return 2;
  if ((value & 0xe) == 0xc)
    return 6;
  return 7;
}

static int uuid_variant_octet(const std::string &str)
{
  return (hex_digit_value(static_cast<unsigned char>(str[19])) << 4) +
         hex_digit_value(static_cast<unsigned char>(str[20]));
}

static void my_error_incorrect_uuid_value(const std::string &str, int version,
                                          int variant)
{
  my_printf_error(ER_TRUNCATED_WRONG_VALUE,
                  "Incorrect uuid value (version: %d, variant: %d): '%-.128s'",
                  MYF(0), version, variant, str.c_str());
}

version_and_variant return_version(const std::string &str)
{
  version_and_variant result;
  result.error_code= 0;

  if (!is_valid_uuid_format_any(str))
  {
    result.error_code= ER_TRUNCATED_WRONG_VALUE;
    return result;
  }

  int version= hex_digit_value(static_cast<unsigned char>(str[14]));
  int variant= uuid_variant(static_cast<unsigned char>(str[19]));
  result.version= version;
  result.variant= variant;

  // Currently the only supported versions are below 9, as per RFC 9562.
  // If the version is above 8, it's either not a valid UUID or it's a
  // future version that we don't support. 0 is also not a valid version.
  if (version < 1 || version > 8)
  {
    result.error_code= ER_INCORRECT_UUID_VALUE;
    return result;
  }

  // For each valid version, we check the variant and reserved
  // bits to ensure the UUID is well-formed according to the RFC.
  int variant_octet= uuid_variant_octet(str);

  if (version == 8 && variant == 0 && variant_octet != 0)
  {
    result.error_code= ER_INCORRECT_UUID_VARIANT;
    return result;
  }

  // RFC 9562 variant must be 10xx, i.e. nibble 8, 9, a, or b.
  if (variant != 2)
  {
    result.error_code= ER_TRUNCATED_WRONG_VALUE;
    return result;
  }

  return result;
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

  if (!is_valid_uuid_format_any(in_str))
  {
    my_printf_error(ER_TRUNCATED_WRONG_VALUE,
                    "Incorrect uuid value: '%-.128s'", MYF(0), in_str.c_str());
    null_value= true;
    return 0;
  }

  version_and_variant result = return_version(in_str);
  if (result.error_code == ER_TRUNCATED_WRONG_VALUE)
  {
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        "Incorrect uuid value (version or variant check failed, "
                        "version: %d, variant: %d): '%-.128s'",
                        result.version, result.variant, in_str.c_str());
  }
  else if (result.error_code == ER_INCORRECT_UUID_VALUE  || result.error_code == ER_INCORRECT_UUID_VARIANT)
  {
    my_error_incorrect_uuid_value(in_str, result.version, result.variant);
    null_value= true;
    return 0;
  }

  null_value= false;
  return result.version;
}
