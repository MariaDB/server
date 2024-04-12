#ifndef SQL_TYPE_TIMESTAMP_WITH_TZ_H
#define SQL_TYPE_TIMESTAMP_WITH_TZ_H
/*
   Copyright (c) 2024, MariaDB Corporation.

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

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "structs.h"

class Native;
class Time_zone;

/*
  Derive from Timeval rather than Timestamp, because
  Timestamp in many contexts treats {tv_sec=0, tv_usec=0} as a zero date.
  Timestamp_with_tz never treats {0,0} as a zero date.
*/
class Timestamp_with_tz: public Timeval
{
protected:
  const Time_zone *m_tz;

public:
  Timestamp_with_tz()
   :Timeval(0,0), m_tz(nullptr)
  { }
  Timestamp_with_tz(const Timeval &tv, const Time_zone *tz)
   :Timeval(tv), m_tz(tz)
  { }
  static constexpr size_t native_size_std()
  {
    return 8 /* 4 + 3 + 1 */ + 10/*time zone */;
  }
  static Timeval make_timeval_from_native_without_tz(const Native &native,
                                                     decimal_digit_t dec);
  static Timestamp_with_tz make_from_native_with_tz(THD *thd,
                                                    const Native &native);
  const Time_zone *tz() const { return m_tz; }
  uint make_sort_key(uchar *to, size_t to_size, decimal_digit_t dec);
  int cmp(const Timestamp_with_tz &rhs) const
  {
    return tv_sec < rhs.tv_sec ? -1:
           tv_sec > rhs.tv_sec ? +1:
           tv_usec < rhs.tv_usec ? -1 :
           tv_usec > rhs.tv_usec ? +1 : 0;
  }
  bool to_bool() const
  {
    return tv_sec != 0 || tv_usec != 0;
  }
  bool to_native(Native *to, decimal_digit_t decimals) const;
  String *val_str(String *to, decimal_digit_t decimals) const;
  bool get_date(MYSQL_TIME *ltime) const;
  longlong to_longlong() const;
  double to_double() const { return (double) to_longlong(); }
  my_decimal *to_decimal(my_decimal *to) const;
};


class Timestamp_with_tz_null: public Timestamp_with_tz,
                              public Null_flag
{
public:
  Timestamp_with_tz_null()
   :Timestamp_with_tz({0,0}, nullptr), Null_flag(true)
  { }
  Timestamp_with_tz_null(const Time_zone *tz)
   :Timestamp_with_tz({0,0}, tz), Null_flag(true)
  { }
  Timestamp_with_tz_null(const Timeval &tv, const Time_zone *tz)
   :Timestamp_with_tz(tv, tz), Null_flag(false)
  { }
  Timestamp_with_tz_null(THD *thd, const Native &native,
                         const Type_handler *th, decimal_digit_t dec);
  Timestamp_with_tz_null(THD *thd, const class Datetime &dt,
                         const Time_zone *tz);
  Timestamp_with_tz_null & set_tz(const Time_zone *tz)
  {
    m_tz= tz;
    return *this;
  }
  bool to_native(Native *to, decimal_digit_t decimals) const
  {
    return is_null() ? true : Timestamp_with_tz::to_native(to, decimals);
  }
  bool to_bool() const
  {
    return is_null() ? false : Timestamp_with_tz::to_bool();
  }
  String *val_str(String *to, decimal_digit_t decimals) const
  {
    return is_null() ? nullptr : Timestamp_with_tz::val_str(to, decimals);
  }
  bool get_date(MYSQL_TIME *ltime) const
  {
    return is_null() ? true : Timestamp_with_tz::get_date(ltime);
  }
  longlong to_longlong() const
  {
    return is_null() ? 0 : Timestamp_with_tz::to_longlong();
  }
  double to_double() const
  {
    return is_null() ? 0 : Timestamp_with_tz::to_double();
  }
  my_decimal *to_decimal(my_decimal *to) const
  {
    return is_null() ? nullptr : Timestamp_with_tz::to_decimal(to);
  }
};


#endif //  SQL_TYPE_TIMESTAMP_WITH_TZ_H
