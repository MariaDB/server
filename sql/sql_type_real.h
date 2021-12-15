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

#ifndef SQL_TYPE_REAL_INCLUDED
#define SQL_TYPE_REAL_INCLUDED

#include <cmath>

class Float
{
  float m_value;
public:
  Float(float nr)
   :m_value(nr)
  {
    DBUG_ASSERT(!std::isnan(nr));
    DBUG_ASSERT(!std::isinf(nr));
  }
  Float(double nr)
   :m_value((float) nr)
  {
    DBUG_ASSERT(!std::isnan(nr));
    DBUG_ASSERT(!std::isinf(nr));
    DBUG_ASSERT(nr <= FLT_MAX);
    DBUG_ASSERT(nr >= -FLT_MAX);
  }
  Float(const uchar *ptr)
  {
    float4get(m_value, ptr);
  }
  bool to_string(String *to, uint dec) const;
};


#endif // SQL_TYPE_REAL_INCLUDED
