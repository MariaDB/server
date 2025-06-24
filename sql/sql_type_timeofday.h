/* Copyright (c) 2024, MariaDB

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

#ifndef SQL_TYPE_TIMEOFDAY_INCLUDED
#define SQL_TYPE_TIMEOFDAY_INCLUDED

#include "my_time.h" // TIME_MAX_MINUTE

/*
  This class stores a time of the day with
  fractional precision up to 6 digits.
*/
class TimeOfDay6
{
  uint m_hour;    // 0..23
  uint m_minute;  // 0..59
  uint m_second;  // 0..59
  uint m_usecond; // 0..999999
  bool is_valid_time_of_day6() const
  {
    return m_hour <= 23 &&
           m_minute <= TIME_MAX_MINUTE &&
           m_second <= TIME_MAX_SECOND &&
           m_usecond <= TIME_MAX_SECOND_PART;
  }
public:
  TimeOfDay6()
   :m_hour(0), m_minute(0), m_second(0), m_usecond(0)
  { }
  // This constructor assumes the caller passes valid 'hh:mm:ss.ff' values
  TimeOfDay6(uint hour, uint minute, uint second, uint usecond)
   :m_hour(hour), m_minute(minute), m_second(second), m_usecond(usecond)
  {
    DBUG_ASSERT(is_valid_time_of_day6());
  }
  uint hour() const { return m_hour; }
  uint minute() const { return m_minute; }
  uint second() const { return m_second; }
  uint usecond() const { return m_usecond; }
  /*
    Return the last time of the day for the given precision, e.g.:
    - '23:59:59.000000' for decimals==0
    - '23:59:59.999000' for decimals==3
    - '23:59:59.999999' for decimals==6
  */
  static TimeOfDay6 end_of_day(decimal_digits_t decimals)
  {
    long rem= my_time_fraction_remainder(TIME_MAX_SECOND_PART, decimals);
    DBUG_ASSERT(rem >= 0 && rem <= TIME_MAX_SECOND_PART);
    return TimeOfDay6(23, TIME_MAX_MINUTE, TIME_MAX_SECOND,
                      (uint) (TIME_MAX_SECOND_PART - (uint) rem));
  }
};

#endif // SQL_TYPE_TIMEOFDAY_INCLUDED
