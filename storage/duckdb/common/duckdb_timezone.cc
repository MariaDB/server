/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "tztime.h"
#include "my_time.h"

#include "duckdb_timezone.h"

#include <ctime>
#include <sstream>

namespace myduck
{

std::map<int64_t, std::string> TimeZoneOffsetHelper::timezone_offset_map;

void TimeZoneOffsetHelper::init_timezone()
{
  add_timezone(50400, "Etc/GMT-14");
  add_timezone(46800, "Etc/GMT-13");
  add_timezone(43200, "Etc/GMT-12");
  add_timezone(39600, "Etc/GMT-11");
  add_timezone(36000, "Etc/GMT-10");
  add_timezone(32400, "Etc/GMT-9");
  add_timezone(28800, "Etc/GMT-8");
  add_timezone(25200, "Etc/GMT-7");
  add_timezone(21600, "Etc/GMT-6");
  add_timezone(18000, "Etc/GMT-5");
  add_timezone(14400, "Etc/GMT-4");
  add_timezone(10800, "Etc/GMT-3");
  add_timezone(7200, "Etc/GMT-2");
  add_timezone(3600, "Etc/GMT-1");
  add_timezone(0, "Etc/GMT");
  add_timezone(-3600, "Etc/GMT+1");
  add_timezone(-7200, "Etc/GMT+2");
  add_timezone(-10800, "Etc/GMT+3");
  add_timezone(-14400, "Etc/GMT+4");
  add_timezone(-18000, "Etc/GMT+5");
  add_timezone(-21600, "Etc/GMT+6");
  add_timezone(-25200, "Etc/GMT+7");
  add_timezone(-28800, "Etc/GMT+8");
  add_timezone(-32400, "Etc/GMT+9");
  add_timezone(-36000, "Etc/GMT+10");
  add_timezone(-39600, "Etc/GMT+11");
  add_timezone(-43200, "Etc/GMT+12");
}

std::string TimeZoneOffsetHelper::get_name_by_offset(int64_t offset,
                                                     std::string &warn_msg)
{
  auto it= timezone_offset_map.find(offset);
  if (it != timezone_offset_map.end())
  {
    return it->second;
  }
  else
  {
    std::ostringstream osst;
    osst << "Can't find corresponding duckdb time_zone, using Etc/GMT.";
    warn_msg= osst.str();
    return "Etc/GMT";
  }
}

void TimeZoneOffsetHelper::add_timezone(int64_t offset,
                                        const std::string &name)
{
  timezone_offset_map[offset]= name;
}

/**
  Compute system timezone offset in seconds using localtime.
*/
static my_time_t get_system_timezone_offset()
{
  time_t now= time(nullptr);
  struct tm l_time;
  localtime_r(&now, &l_time);

  MYSQL_TIME t;
  t.year= (uint) l_time.tm_year + 1900;
  t.month= (uint) l_time.tm_mon + 1;
  t.day= (uint) l_time.tm_mday;
  t.hour= (uint) l_time.tm_hour;
  t.minute= (uint) l_time.tm_min;
  t.second= (uint) l_time.tm_sec;
  t.time_type= MYSQL_TIMESTAMP_DATETIME;
  t.neg= false;
  t.second_part= 0;

  /* Compute seconds since 1970-01-01 00:00:00 */
  my_time_t days= calc_daynr((uint) t.year, (uint) t.month, (uint) t.day) -
                  (my_time_t) days_at_timestart;
  my_time_t seconds=
      days * SECONDS_IN_24H +
      ((int64_t) t.hour * 3600 + (int64_t) (t.minute * 60 + t.second));

  /* Get my_time_t via system time zone. */
  long not_used_tz;
  uint not_used_err;
  my_system_gmt_sec(&t, &not_used_tz, &not_used_err);

  return (seconds - now);
}

std::string get_timezone_according_thd(THD *thd, std::string &warn_msg)
{
  Time_zone *tz= thd->variables.time_zone;
  const String *tz_name= tz->get_name();
  std::string name_str(tz_name->ptr(), tz_name->length());

  /* SYSTEM timezone */
  if (tz == my_tz_SYSTEM || name_str == "SYSTEM")
  {
    my_time_t offset= get_system_timezone_offset();
    return TimeZoneOffsetHelper::get_name_by_offset(offset, warn_msg);
  }

  /* Offset timezone like +08:00 */
  if (name_str.size() >= 3 && (name_str[0] == '+' || name_str[0] == '-'))
  {
    /* Parse offset from "+HH:MM" format */
    int hours= 0, minutes= 0;
    char sign= name_str[0];
    if (sscanf(name_str.c_str() + 1, "%d:%d", &hours, &minutes) >= 1)
    {
      int64_t offset= (int64_t) hours * 3600 + (int64_t) minutes * 60;
      if (sign == '-')
        offset= -offset;
      return TimeZoneOffsetHelper::get_name_by_offset(offset, warn_msg);
    }
  }

  /* Named timezone — pass through directly to DuckDB */
  return name_str;
}

} // namespace myduck
