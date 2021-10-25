/* Copyright (c) 2019,2021 MariaDB Corporation

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
#include "my_net.h"
#include "sql_class.h" // THD, SORT_FIELD_ATTR
#include "opt_range.h" // SEL_ARG
#include "sql_type_uuid.h"


static bool get_digit(char ch, uint *val)
{
  if (ch >= '0' && ch <= '9')
  {
    *val= (uint) ch - '0';
    return false;
  }
  if (ch >= 'a' && ch <= 'f')
  {
    *val= (uint) ch - 'a' + 0x0a;
    return false;
  }
  if (ch >= 'A' && ch <= 'F')
  {
    *val= (uint) ch - 'A' + 0x0a;
    return false;
  }
  return true;
}


static bool get_digit(uint *val, const char *str, const char *end)
{
  if (str >= end)
    return true;
  return get_digit(*str, val);
}


static size_t skip_hyphens(const char *str, const char *end)
{
  const char *str0= str;
  for ( ; str < end; str++)
  {
    if (str[0] != '-')
      break;
  }
  return str - str0;
}


static const char *get_two_digits(char *val, const char *str, const char *end)
{
  uint hi, lo;
  if (get_digit(&hi, str++, end))
    return NULL;
  str+= skip_hyphens(str, end);
  if (get_digit(&lo, str++, end))
    return NULL;
  *val= (char) ((hi << 4) + lo);
  return str;
}


bool UUID::ascii_to_fbt(const char *str, size_t str_length)
{
  const char *end= str + str_length;
  /*
    The format understood:
    - Hyphen is not allowed on the first and the last position.
    - Otherwise, hyphens are allowed on any (odd and even) position,
      with any amount.
  */
  if (str_length < 32)
    goto err;

  for (uint oidx= 0; oidx < binary_length(); oidx++)
  {
    if (!(str= get_two_digits(&m_buffer[oidx], str, end)))
      goto err;
    // Allow hypheps after two digits, but not after the last digit
    if (oidx + 1 < binary_length())
      str+= skip_hyphens(str, end);
  }
  if (str < end)
    goto err; // Some input left
  return false;
err:
  bzero(m_buffer, sizeof(m_buffer));
  return true;
}

size_t UUID::to_string(char *dst, size_t dstsize) const
{
  my_uuid2str((const uchar *) m_buffer, dst, 1);
  return MY_UUID_STRING_LENGTH;
}


const Name &UUID::default_value()
{
  static Name def(STRING_WITH_LEN("00000000-0000-0000-0000-000000000000"));
  return def;
}
