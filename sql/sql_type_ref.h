/* Copyright (c) 2018, 2025, MariaDB

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

#ifndef SQL_TYPE_REF_INCLUDED
#define SQL_TYPE_REF_INCLUDED

#include "sql_type_int.h" // Null_flag


class Type_ref_null: public Null_flag
{
  ulonglong m_value;
public:
  Type_ref_null()
   :Null_flag(true),
    m_value(0)
  { }
  Type_ref_null(ulonglong value)
   :Null_flag(false),
    m_value(value)
  { }
  ulonglong value() const { return m_value; }
  bool operator<(ulonglong val) const
  {
    return !m_is_null && m_value < val;
  }
};

#endif // SQL_TYPE_REF_INCLUDED
