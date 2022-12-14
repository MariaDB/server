/* Copyright (c) 2023, MariaDB Corporation.

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

#ifndef SQL_USED_INCLUDED
#define SQL_USED_INCLUDED

class Sql_used
{
public:
  typedef uint used_t;
  enum { RAND_USED=1, TIME_ZONE_USED=2, QUERY_START_SEC_PART_USED=4,
         THREAD_SPECIFIC_USED=8,
         CHARACTER_SET_COLLATIONS_USED= 16
       };
  used_t used;
  Sql_used()
   :used(0)
  { }
};

#endif // SQL_USED_INCLUDED
