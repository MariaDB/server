#ifndef DEBUG_INCLUDED
#define DEBUG_INCLUDED

/* Copyright (c) 2021, MariaDB Corporation

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

/**
  @file

  Declarations for debug_crash_here and other future mariadb server debug
  functionality.
*/

/* debug_crash_here() functionallity.
 See mysql_test/suite/atomic/create_table.test for an example of how it
 can be used
*/

#ifndef DBUG_OFF
void debug_crash_here(const char *keyword);
bool debug_simulate_error(const char *keyword, uint error);
#else
#define debug_crash_here(A) do { } while(0)
#define debug_simulate_error(A, B) 0
#endif

#endif /* DEBUG_INCLUDED */
