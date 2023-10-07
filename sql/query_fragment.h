/*
   Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* File that includes common types used globally in MariaDB */

#ifndef QUERY_FRAGMENT_INCLUDED
#define QUERY_FRAGMENT_INCLUDED

/*
  A helper class to calculate offset and length of a query fragment
  - outside of SP
  - inside an SP
  - inside a compound block
*/
class Query_fragment
{
  uint m_pos;
  uint m_length;
  void set(size_t pos, size_t length)
  {
    DBUG_ASSERT(pos < UINT_MAX32);
    DBUG_ASSERT(length < UINT_MAX32);
    m_pos= (uint) pos;
    m_length= (uint) length;
  }
public:
  Query_fragment(uint pos, uint length)
   :m_pos(pos), m_length(length)
  { }
  Query_fragment(THD *thd, class sp_head *sphead,
                 const char *start, const char *end);
  uint pos() const { return m_pos; }
  uint length() const { return m_length; }
  uint end() const { return m_pos + m_length; }
};

#endif
