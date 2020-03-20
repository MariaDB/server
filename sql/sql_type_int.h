/* Copyright (c) 2006, 2010, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_TYPE_INT_INCLUDED
#define SQL_TYPE_INT_INCLUDED


// A longlong/ulonglong hybrid. Good to store results of val_int().
class Longlong_hybrid
{
protected:
  longlong m_value;
  bool m_unsigned;
  int cmp_signed(const Longlong_hybrid& other) const
  {
    return m_value < other.m_value ? -1 : m_value == other.m_value ? 0 : 1;
  }
  int cmp_unsigned(const Longlong_hybrid& other) const
  {
    return (ulonglong) m_value < (ulonglong) other.m_value ? -1 :
            m_value == other.m_value ? 0 : 1;
  }
public:
  Longlong_hybrid(longlong nr, bool unsigned_flag)
   :m_value(nr), m_unsigned(unsigned_flag)
  { }
  longlong value() const { return m_value; }
  bool is_unsigned() const { return m_unsigned; }
  bool is_unsigned_outside_of_signed_range() const
  {
    return m_unsigned && ((ulonglong) m_value) > (ulonglong) LONGLONG_MAX;
  }
  bool neg() const { return m_value < 0 && !m_unsigned; }
  ulonglong abs() const
  {
    if (m_unsigned)
      return (ulonglong) m_value;
    if (m_value == LONGLONG_MIN) // avoid undefined behavior
      return ((ulonglong) LONGLONG_MAX) + 1;
    return m_value < 0 ? -m_value : m_value;
  }
  int cmp(const Longlong_hybrid& other) const
  {
    if (m_unsigned == other.m_unsigned)
      return m_unsigned ? cmp_unsigned(other) : cmp_signed(other);
    if (is_unsigned_outside_of_signed_range())
      return 1;
    if (other.is_unsigned_outside_of_signed_range())
      return -1;
    /*
      The unsigned argument is in the range 0..LONGLONG_MAX.
      The signed argument is in the range LONGLONG_MIN..LONGLONG_MAX.
      Safe to compare as signed.
    */
    return cmp_signed(other);
  }
  bool operator==(const Longlong_hybrid &nr) const
  {
    return cmp(nr) == 0;
  }
  bool operator==(ulonglong nr) const
  {
    return cmp(Longlong_hybrid((longlong) nr, true)) == 0;
  }
  bool operator==(uint nr) const
  {
    return cmp(Longlong_hybrid((longlong) nr, true)) == 0;
  }
  bool operator==(longlong nr) const
  {
    return cmp(Longlong_hybrid(nr, false)) == 0;
  }
  bool operator==(int nr) const
  {
    return cmp(Longlong_hybrid(nr, false)) == 0;
  }
};

#endif // SQL_TYPE_INT_INCLUDED
