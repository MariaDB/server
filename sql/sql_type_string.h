/* Copyright (c) 2019 MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef SQL_TYPE_STRING_INCLUDED
#define SQL_TYPE_STRING_INCLUDED

class StringPack
{
  CHARSET_INFO *m_cs;
  uint32 m_octet_length;
  CHARSET_INFO *charset() const { return m_cs; }
  uint mbmaxlen() const { return m_cs->mbmaxlen; };
  uint32 char_length() const { return m_octet_length / mbmaxlen(); }
public:
  StringPack(CHARSET_INFO *cs, uint32 octet_length)
   :m_cs(cs),
    m_octet_length(octet_length)
  { }
  uchar *pack(uchar *to, const uchar *from, uint max_length) const;
  const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                      uint param_data) const;
public:
  static uint max_packed_col_length(uint max_length)
  {
    return (max_length > 255 ? 2 : 1) + max_length;
  }
  static uint packed_col_length(const uchar *data_ptr, uint length)
  {
    if (length > 255)
      return uint2korr(data_ptr)+2;
    return (uint) *data_ptr + 1;
  }
};


#endif // SQL_TYPE_STRING_INCLUDED
