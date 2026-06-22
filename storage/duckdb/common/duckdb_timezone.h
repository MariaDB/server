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

#ifndef DUCKDB_TIMEZONE_H
#define DUCKDB_TIMEZONE_H

#include <map>
#include <string>

class THD;

namespace myduck
{
constexpr long days_at_timestart= 719528;

class TimeZoneOffsetHelper
{
public:
  static void init_timezone();

  static std::string get_name_by_offset(int64_t offset, std::string &warn_msg);

private:
  static void add_timezone(int64_t offset, const std::string &name);
  static std::map<int64_t, std::string> timezone_offset_map;
};

/** Get duckdb timezone name for current thread.
  @param thd   THD
  @param warn_msg  output warning message
  @return timezone name suitable for DuckDB
*/
std::string get_timezone_according_thd(THD *thd, std::string &warn_msg);

} // namespace myduck

#endif // DUCKDB_TIMEZONE_H
