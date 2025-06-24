/* Copyright (c) 2004, 2006 MySQL AB
   Use is subject to license terms

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

#ifndef _mysql_time_h_
#define _mysql_time_h_

/*
  Portable time_t replacement.
  For 32 bit systems holds seconds for 1970 - 2038-01-19
  For 64 bit systems holds seconds for 1970 - 2106-02-07

  Using the system built in time_t is not an option as
  we rely on the above requirements in the time functions   
*/

#ifndef MYSQL_ABI_CHECK
/*
  long is 64bit on all 64bit systems, except Windows where it is 32 bit.
  long is 32bit on all 32bit systems.
  The following code ensures that my_time_t is 64 bit on all 64 bit
  systems.
*/
#ifdef _WIN64
typedef long long my_time_t;
#else
typedef long my_time_t;
#endif
#endif /* MYSQL_ABI_CHECK */

/*
  Time declarations shared between the server and client API:
  you should not add anything to this header unless it's used
  (and hence should be visible) in mysql.h.
  If you're looking for a place to add new time-related declaration,
  it's most likely my_time.h. See also "C API Handling of Date
  and Time Values" chapter in documentation.
*/

enum enum_mysql_timestamp_type
{
  MYSQL_TIMESTAMP_NONE= -2, MYSQL_TIMESTAMP_ERROR= -1,
  MYSQL_TIMESTAMP_DATE= 0, MYSQL_TIMESTAMP_DATETIME= 1, MYSQL_TIMESTAMP_TIME= 2
};


/*
  Structure which is used to represent datetime values inside MySQL.

  We assume that values in this structure are normalized, i.e. year <= 9999,
  month <= 12, day <= 31, hour <= 23, hour <= 59, hour <= 59. Many functions
  in server such as my_system_gmt_sec() or make_time() family of functions
  rely on this (actually now usage of make_*() family relies on a bit weaker
  restriction). Also functions that produce MYSQL_TIME as result ensure this.
  There is one exception to this rule though if this structure holds time
  value (time_type == MYSQL_TIMESTAMP_TIME) days and hour member can hold
  bigger values.
*/
typedef struct st_mysql_time
{
  unsigned int  year, month, day, hour, minute, second;
  unsigned long second_part;
  my_bool       neg;
  enum enum_mysql_timestamp_type time_type;
} MYSQL_TIME;

#endif /* _mysql_time_h_ */
