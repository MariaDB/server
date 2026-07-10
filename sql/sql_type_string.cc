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


// Trim trailing spaces for CHAR or 0x00 bytes for BINARY
uint StringPack::rtrimmed_length(const char *from) const
{
  DBUG_PRINT("debug", ("length: %u ", (uint) m_octet_length));

  if (mbmaxlen() > 1)
  {
    /*
      Suppose we have CHAR(100) CHARACTER SET utf8mb4.
      Its octet_length is 400.
      - In case of ASCII characters only, the leftmost 100 bytes
        contain real data, the other 300 bytes are padding spaces.
      - In case of 100 2-byte characters, the leftmost 200 bytes
        contain real data, the other 200 bytes are padding spaces.
      - All 400 bytes contain real data (without padding spaces)
        only in case of 100 4-byte characters, which is a rare scenario.

      There are two approaches possible to trim the data:
      1. Left-to-right: call charpos() to find the end of the 100th
         character, then switch to a right-to-left loop to trim trailing spaces.
      2. Right-to-left: trim characters from the position "from+400" towards
         the beginning.

      N1 should be faster in an average case, and is much faster for pure ASCII.
    */
    size_t length= charset()->charpos(from, from+m_octet_length, char_length());
    return (uint) charset()->lengthsp((const char*) from, length);
  }

  /*
     TODO: change charset interface to add a new function that does 
           the following or add a flag to lengthsp to do it itself 
           (this is for not packing padding adding bytes in BINARY 
           fields).
  */
  size_t length= m_octet_length;
  while (length && from[length - 1] == charset()->pad_char)
    length --;
  return (uint) length;
}


uchar *
StringPack::pack(uchar *to, const uchar *from) const
{
  size_t length= rtrimmed_length((const char *) from);

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
