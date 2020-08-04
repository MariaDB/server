/* Copyright (c) 2006, 2010, Oracle and/or its affiliates.
   Copyright (c) 2011, 2020, MariaDB

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

#ifndef SQL_TIME_INCLUDED
#define SQL_TIME_INCLUDED

#include "sql_basic_types.h"
#include "my_time.h"
#include "mysql_time.h"                         /* timestamp_type */
#include "sql_error.h"                          /* Sql_condition */
#include "structs.h"                            /* INTERVAL */

typedef enum enum_mysql_timestamp_type timestamp_type;
typedef struct st_date_time_format DATE_TIME_FORMAT;
typedef struct st_known_date_time_format KNOWN_DATE_TIME_FORMAT;

/* Flags for calc_week() function.  */
#define WEEK_MONDAY_FIRST    1
#define WEEK_YEAR            2
#define WEEK_FIRST_WEEKDAY   4

ulong convert_period_to_month(ulong period);
ulong convert_month_to_period(ulong month);
void set_current_date(THD *thd, MYSQL_TIME *to);
bool time_to_datetime(MYSQL_TIME *ltime);
bool get_date_from_daynr(long daynr,uint *year, uint *month, uint *day);
my_time_t TIME_to_timestamp(THD *thd, const MYSQL_TIME *t, uint *error_code);
bool str_to_datetime_with_warn(THD *thd,
                               CHARSET_INFO *cs, const char *str, size_t length,
                               MYSQL_TIME *l_time,
                               date_mode_t flags);
bool double_to_datetime_with_warn(THD *thd, double value, MYSQL_TIME *ltime,
                                  date_mode_t fuzzydate,
                                  const TABLE_SHARE *s, const char *name);
bool decimal_to_datetime_with_warn(THD *thd,
                                   const my_decimal *value, MYSQL_TIME *ltime,
                                   date_mode_t fuzzydate,
                                   const TABLE_SHARE *s, const char *name);
bool int_to_datetime_with_warn(THD *thd, const Longlong_hybrid &nr,
                               MYSQL_TIME *ltime,
                               date_mode_t fuzzydate,
                               const TABLE_SHARE *s, const char *name);

bool time_to_datetime(THD *thd, const MYSQL_TIME *tm, MYSQL_TIME *dt);
bool time_to_datetime_with_warn(THD *thd,
                                const MYSQL_TIME *tm, MYSQL_TIME *dt,
                                date_conv_mode_t fuzzydate);

inline void datetime_to_date(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ltime->time_type == MYSQL_TIMESTAMP_DATE ||
              ltime->time_type == MYSQL_TIMESTAMP_DATETIME);
  DBUG_ASSERT(ltime->neg == 0);
  ltime->second_part= ltime->hour= ltime->minute= ltime->second= 0;
  ltime->time_type= MYSQL_TIMESTAMP_DATE;
}
inline void date_to_datetime(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ltime->time_type == MYSQL_TIMESTAMP_DATE ||
              ltime->time_type == MYSQL_TIMESTAMP_DATETIME);
  DBUG_ASSERT(ltime->neg == 0);
  ltime->time_type= MYSQL_TIMESTAMP_DATETIME;
}
void make_truncated_value_warning(THD *thd,
                                  Sql_condition::enum_warning_level level,
                                  const ErrConv *str_val,
                                  timestamp_type time_type,
                                  const char *db_name, const char *table_name,
                                  const char *field_name);

extern DATE_TIME_FORMAT *date_time_format_make(timestamp_type format_type,
					       const char *format_str,
					       uint format_length);
extern DATE_TIME_FORMAT *date_time_format_copy(THD *thd,
					       DATE_TIME_FORMAT *format);
const char *get_date_time_format_str(KNOWN_DATE_TIME_FORMAT *format,
				     timestamp_type type);
bool my_TIME_to_str(const MYSQL_TIME *ltime, String *str, uint dec);

/* MYSQL_TIME operations */
bool date_add_interval(THD *thd, MYSQL_TIME *ltime, interval_type int_type,
                       const INTERVAL &interval, bool push_warn= true);
bool calc_time_diff(const MYSQL_TIME *l_time1, const MYSQL_TIME *l_time2,
                    int l_sign, ulonglong *seconds_out, ulong *microseconds_out);
int append_interval(String *str, interval_type int_type,
                    const INTERVAL &interval);
/**
  Calculate time difference between two MYSQL_TIME values and
  store the result as an out MYSQL_TIME value in MYSQL_TIMESTAMP_TIME format.

  The result can be outside of the supported TIME range.
  For example, calc_time_diff('2002-01-01 00:00:00', '2001-01-01 00:00:00')
  returns '8760:00:00'. So the caller might want to do check_time_range() or
  adjust_time_range_with_warn() on the result of a calc_time_diff() call.

  @param l_time1       - the minuend (TIME/DATE/DATETIME value)
  @param l_time2       - the subtrahend TIME/DATE/DATETIME value
  @param l_sign        -  +1 if absolute values are to be subtracted,
                          or -1 if absolute values are to be added.
  @param[out] l_time3  - the result
  @param fuzzydate     - flags

  @return true         - if TIME_NO_ZERO_DATE was passed in flags and
                         the result appeared to be '00:00:00.000000'.
                         This is important when calc_time_diff() is called
                         when calculating DATE_ADD(TIMEDIFF(...),...)
  @return false        - otherwise
*/
bool calc_time_diff(const MYSQL_TIME *l_time1, const MYSQL_TIME *l_time2,
                    int lsign, MYSQL_TIME *l_time3, date_mode_t fuzzydate);
int my_time_compare(const MYSQL_TIME *a, const MYSQL_TIME *b);
void localtime_to_TIME(MYSQL_TIME *to, struct tm *from);

void calc_time_from_sec(MYSQL_TIME *to, ulong seconds, ulong microseconds);
uint calc_week(const MYSQL_TIME *l_time, uint week_behaviour, uint *year);

int calc_weekday(long daynr,bool sunday_first_day_of_week);
bool parse_date_time_format(timestamp_type format_type, 
                            const char *format, uint format_length,
                            DATE_TIME_FORMAT *date_time_format);

/* convenience wrapper */
inline bool parse_date_time_format(timestamp_type format_type, 
                                   DATE_TIME_FORMAT *date_time_format)
{
  return parse_date_time_format(format_type,
                                date_time_format->format.str,
                                (uint) date_time_format->format.length,
                                date_time_format);
}


extern DATE_TIME_FORMAT global_date_format;
extern DATE_TIME_FORMAT global_datetime_format;
extern DATE_TIME_FORMAT global_time_format;
extern KNOWN_DATE_TIME_FORMAT known_date_time_formats[];
extern LEX_CSTRING interval_type_to_name[];

static inline bool
non_zero_hhmmssuu(const MYSQL_TIME *ltime)
{
  return ltime->hour || ltime->minute || ltime->second || ltime->second_part;
}
static inline bool
non_zero_YYMMDD(const MYSQL_TIME *ltime)
{
  return ltime->year || ltime->month || ltime->day;
}
static inline bool
non_zero_date(const MYSQL_TIME *ltime)
{
  return non_zero_YYMMDD(ltime) ||
         (ltime->time_type == MYSQL_TIMESTAMP_DATETIME &&
          non_zero_hhmmssuu(ltime));
}
static inline bool
check_date(const MYSQL_TIME *ltime, date_conv_mode_t flags, int *was_cut)
{
 return check_date(ltime, non_zero_date(ltime),
                   ulonglong(flags & TIME_MODE_FOR_XXX_TO_DATE), was_cut);
}
bool check_date_with_warn(THD *thd, const MYSQL_TIME *ltime,
                          date_conv_mode_t fuzzy_date, timestamp_type ts_type);
static inline bool
check_date_with_warn(THD *thd, const MYSQL_TIME *ltime,
                          date_mode_t fuzzydate, timestamp_type ts_type)
{
  return check_date_with_warn(thd, ltime, date_conv_mode_t(fuzzydate), ts_type);
}

bool adjust_time_range_with_warn(THD *thd, MYSQL_TIME *ltime, uint dec);

longlong pack_time(const MYSQL_TIME *my_time);
void unpack_time(longlong packed, MYSQL_TIME *my_time,
                 enum_mysql_timestamp_type ts_type);

#endif /* SQL_TIME_INCLUDED */
