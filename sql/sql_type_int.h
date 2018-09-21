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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef SQL_TYPE_INT_INCLUDED
#define SQL_TYPE_INT_INCLUDED


// A longlong/ulonglong hybrid. Good to store results of val_int().
class Longlong_hybrid
{
protected:
  longlong m_value;
  bool m_unsigned;
public:
  Longlong_hybrid(longlong nr, bool unsigned_flag)
   :m_value(nr), m_unsigned(unsigned_flag)
  { }
  longlong value() const { return m_value; }
  bool is_unsigned() const { return m_unsigned; }
  bool neg() const { return m_value < 0 && !m_unsigned; }
  ulonglong abs() const
  {
    return neg() ? (ulonglong) -m_value : (ulonglong) m_value;
  }
};

#endif // SQL_TYPE_INT_INCLUDED
