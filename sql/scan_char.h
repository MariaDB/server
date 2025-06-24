/* Copyright (c) 2024, MariaDB Corporation.

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

#ifndef SCAN_CHAR_H
#define SCAN_CHAR_H


/**
  A helper class to store the head character of a string,
  with help of a charlen() call.
*/
class Scan_char
{
  const char *m_ptr; // The start of the character
  int m_length;      // The result:
                     // >0   - the character octet length
                     // <=0  - an error (e.g. end of input, wrong byte sequence)
public:
  Scan_char(CHARSET_INFO *const cs, const char *str, const char *end)
   :m_ptr(str), m_length(cs->charlen(str, end))
  { }
  // Compare if two non-erroneous characters are equal
  bool eq(const Scan_char &rhs) const
  {
    DBUG_ASSERT(m_length > 0);
    DBUG_ASSERT(rhs.m_length > 0);
    return m_length == rhs.m_length &&
           !memcmp(m_ptr, rhs.m_ptr, (size_t) m_length);
  }
  // Compare if two possibly erroneous characters are equal
  bool eq_safe(const Scan_char &rhs) const
  {
    return m_length == rhs.m_length && m_length > 0 &&
           !memcmp(m_ptr, rhs.m_ptr, (size_t) m_length);
  }
  const char *ptr() const { return m_ptr; }
  int length() const { return m_length; }
};


#endif // SCAN_CHAR_H
