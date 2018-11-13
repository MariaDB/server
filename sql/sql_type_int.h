/* Copyright (c) 2018, MariaDB

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


class Null_flag
{
protected:
  bool m_is_null;
public:
  bool is_null() const { return m_is_null; }
  Null_flag(bool is_null) :m_is_null(is_null) { }
};


class Longlong
{
protected:
  longlong m_value;
public:
  longlong value() const { return m_value; }
  Longlong(longlong nr) :m_value(nr) { }
};


class Longlong_null: public Longlong, public Null_flag
{
public:
  Longlong_null(longlong nr, bool is_null)
   :Longlong(nr), Null_flag(is_null)
  { }
};


// A longlong/ulonglong hybrid. Good to store results of val_int().
class Longlong_hybrid: public Longlong
{
protected:
  bool m_unsigned;
public:
  Longlong_hybrid(longlong nr, bool unsigned_flag)
   :Longlong(nr), m_unsigned(unsigned_flag)
  { }
  bool is_unsigned() const { return m_unsigned; }
  bool neg() const { return m_value < 0 && !m_unsigned; }
  ulonglong abs() const
  {
    if (m_unsigned)
      return (ulonglong) m_value;
    if (m_value == LONGLONG_MIN) // avoid undefined behavior
      return ((ulonglong) LONGLONG_MAX) + 1;
    return m_value < 0 ? -m_value : m_value;
  }
};


class Longlong_hybrid_null: public Longlong_hybrid,
                            public Null_flag
{
public:
  Longlong_hybrid_null(const Longlong_null &nr, bool unsigned_flag)
   :Longlong_hybrid(nr.value(), unsigned_flag),
    Null_flag(nr.is_null())
  { }
};


#endif // SQL_TYPE_INT_INCLUDED
