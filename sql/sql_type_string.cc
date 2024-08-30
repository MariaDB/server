/*
   Copyright (c) 2019, 2020 MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include "mariadb.h"

#include "sql_class.h"
#include "sql_type_string.h"


uchar *
StringPack::pack(uchar *to, const uchar *from, uint max_length) const
{
  size_t length=      MY_MIN(m_octet_length, max_length);
  size_t local_char_length= char_length();
  DBUG_PRINT("debug", ("length: %zu ", length));

  if (length > local_char_length)
    local_char_length= charset()->charpos(from, from + length,
                                          local_char_length);
  set_if_smaller(length, local_char_length);
 
  /*
     TODO: change charset interface to add a new function that does 
           the following or add a flag to lengthsp to do it itself 
           (this is for not packing padding adding bytes in BINARY 
           fields).
  */
  if (mbmaxlen() == 1)
  {
    while (length && from[length-1] == charset()->pad_char)
      length --;
  }
  else
    length= charset()->lengthsp((const char*) from, length);

  // Length always stored little-endian
  *to++= (uchar) length;
  if (m_octet_length > 255)
    *to++= (uchar) (length >> 8);

  // Store the actual bytes of the string
  memcpy(to, from, length);
  return to+length;
}


const uchar *
StringPack::unpack(uchar *to, const uchar *from, const uchar *from_end,
                   uint param_data) const
{
  uint from_length, length;

  /*
    Compute the declared length of the field on the master. This is
    used to decide if one or two bytes should be read as length.
   */
  if (param_data)
    from_length= (((param_data >> 4) & 0x300) ^ 0x300) + (param_data & 0x00ff);
  else
    from_length= m_octet_length;

  DBUG_PRINT("debug",
             ("param_data: 0x%x, field_length: %u, from_length: %u",
              param_data, m_octet_length, from_length));
  /*
    Compute the actual length of the data by reading one or two bits
    (depending on the declared field length on the master).
   */
  if (from_length > 255)
  {
    if (from + 2 > from_end)
      return 0;
    length= uint2korr(from);
    from+= 2;
  }
  else
  {
    if (from + 1 > from_end)
      return 0;
    length= (uint) *from++;
  }
  if (from + length > from_end || length > m_octet_length)
    return 0;

  memcpy(to, from, length);
  // Pad the string with the pad character of the fields charset
  charset()->fill((char*) to + length,
                  m_octet_length - length,
                  charset()->pad_char);
  return from+length;
}
