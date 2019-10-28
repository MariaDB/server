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


template<>
bool UUIDBundle::Fbt::ascii_to_fbt(const char *str, size_t str_length)
{
  if (str_length < 32 || str_length > 3 * binary_length() - 1)
    return true;

  uint oidx= 0;
  for (const char *s= str; s < str + str_length; )
  {
    if (oidx >= binary_length())
      goto err;
    if (*s == '-')
    {
      if (s == str)
        goto err;
      s++;
      continue;
    }
    uint hi, lo;
    if (get_digit(*s++, &hi) || get_digit(*s++, &lo))
      goto err;
    m_buffer[oidx++]= (char) ((hi << 4) + lo);
  }
  return false;
err:
  bzero(m_buffer, sizeof(m_buffer));
  return true;
}

template<>
size_t UUIDBundle::Fbt::to_string(char *dst, size_t dstsize) const
{
  my_uuid2str((const uchar *) m_buffer, dst, 1);
  return MY_UUID_STRING_LENGTH;
}


template<>
const Name &UUIDBundle::Type_handler_fbt::default_value() const
{
  static Name def(STRING_WITH_LEN("00000000-0000-0000-0000-000000000000"));
  return def;
}
