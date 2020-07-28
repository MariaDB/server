/* Copyright (c) 2006, 2010, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB

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

#include "my_global.h"                          /* ulong */
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
void time_to_daytime_interval(MYSQL_TIME *l_time);
bool get_date_from_daynr(long daynr,uint *year, uint *month, uint *day);
my_time_t TIME_to_timestamp(THD *thd, const MYSQL_TIME *t, uint *error_code);
bool str_to_datetime_with_warn(CHARSET_INFO *cs, const char *str,
                               uint length, MYSQL_TIME *l_time,
                               ulonglong flags);
bool double_to_datetime_with_warn(double value, MYSQL_TIME *ltime,
                                  ulonglong fuzzydate,
                                  const TABLE_SHARE *s, const char *name);
bool decimal_to_datetime_with_warn(const my_decimal *value, MYSQL_TIME *ltime,
                                   ulonglong fuzzydate,
                                   const TABLE_SHARE *s, const char *name);
bool int_to_datetime_with_warn(bool neg, ulonglong value, MYSQL_TIME *ltime,
                               ulonglong fuzzydate,
                               const TABLE_SHARE *s, const char *name);

bool time_to_datetime(THD *thd, const MYSQL_TIME *tm, MYSQL_TIME *dt);
bool time_to_datetime_with_warn(THD *thd,
                                const MYSQL_TIME *tm, MYSQL_TIME *dt,
                                ulonglong fuzzydate);
/*
  Simply truncate the YYYY-MM-DD part to 0000-00-00
  and change time_type to MYSQL_TIMESTAMP_TIME
*/
inline void datetime_to_time(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ltime->time_type == MYSQL_TIMESTAMP_DATE ||
              ltime->time_type == MYSQL_TIMESTAMP_DATETIME);
  DBUG_ASSERT(ltime->neg == 0);
  ltime->year= ltime->month= ltime->day= 0;
  ltime->time_type= MYSQL_TIMESTAMP_TIME;
}


/**
  Convert DATE/DATETIME to TIME(dec)
  using CURRENT_DATE in a non-old mode,
  or using simple truncation in old mode (OLD_MODE_ZERO_DATE_TIME_CAST).

  @param      thd - the thread to get the variables.old_behaviour value from
  @param      dt  - the DATE of DATETIME value to convert
  @param[out] tm  - store result here
  @param      dec - the desired scale. The fractional part of the result
                    is checked according to this parameter before returning
                    the conversion result. "dec" is important in the corner
                    cases near the max/min limits.
                    If the result is '838:59:59.999999' and the desired scale
                    is less than 6, an error is returned.
                    Note, dec is not important in the
                    OLD_MODE_ZERO_DATE_TIME_CAST old mode.

  - in case of OLD_MODE_ZERO_DATE_TIME_CAST
    the TIME part is simply truncated and "false" is returned.
  - otherwise, the result is calculated effectively similar to:
    TIMEDIFF(dt, CAST(CURRENT_DATE AS DATETIME))
    If the difference fits into the supported TIME range, "false" is returned,
    otherwise a warning is issued and "true" is returned.

  @return false - on success
  @return true  - on error
*/
bool datetime_to_time_with_warn(THD *, const MYSQL_TIME *dt,
                                MYSQL_TIME *tm, uint dec);


inline void datetime_to_date(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ltime->time_type == MYSQL_TIMESTAMP_DATE ||
              ltime->time_type == MYSQL_TIMESTAMP_DATETIME);
  DBUG_ASSERT(ltime->neg == 0);
  ltime->hour= ltime->minute= ltime->second= ltime->second_part= 0;
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

static inline void make_truncated_value_warning(THD *thd,
                Sql_condition::enum_warning_level level, const char *str_val,
                uint str_length, timestamp_type time_type,
                const char *db_name, const char *table_name,
                const char *field_name)
{
  const ErrConvString str(str_val, str_length, &my_charset_bin);
  make_truncated_value_warning(thd, level, &str, time_type, db_name, table_name,
                               field_name);
}

extern DATE_TIME_FORMAT *date_time_format_make(timestamp_type format_type,
					       const char *format_str,
					       uint format_length);
extern DATE_TIME_FORMAT *date_time_format_copy(THD *thd,
					       DATE_TIME_FORMAT *format);
const char *get_date_time_format_str(KNOWN_DATE_TIME_FORMAT *format,
				     timestamp_type type);
bool my_TIME_to_str(const MYSQL_TIME *ltime, String *str, uint dec);

/* MYSQL_TIME operations */
bool date_add_interval(MYSQL_TIME *ltime, interval_type int_type,
                       INTERVAL interval);
bool calc_time_diff(const MYSQL_TIME *l_time1, const MYSQL_TIME *l_time2,
                    int l_sign, longlong *seconds_out, long *microseconds_out);
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
                    int lsign, MYSQL_TIME *l_time3, ulonglong fuzzydate);
int my_time_compare(const MYSQL_TIME *a, const MYSQL_TIME *b);
void localtime_to_TIME(MYSQL_TIME *to, struct tm *from);
void calc_time_from_sec(MYSQL_TIME *to, long seconds, long microseconds);
uint calc_week(MYSQL_TIME *l_time, uint week_behaviour, uint *year);

int calc_weekday(long daynr,bool sunday_first_day_of_week);
bool parse_date_time_format(timestamp_type format_type, 
                            const char *format, uint format_length,
                            DATE_TIME_FORMAT *date_time_format);
/* Character set-aware version of str_to_time() */
bool str_to_time(CHARSET_INFO *cs, const char *str,uint length,
                 MYSQL_TIME *l_time, ulonglong fuzzydate,
                 MYSQL_TIME_STATUS *status);
/* Character set-aware version of str_to_datetime() */
bool str_to_datetime(CHARSET_INFO *cs,
                     const char *str, uint length,
                     MYSQL_TIME *l_time, ulonglong flags,
                     MYSQL_TIME_STATUS *status);

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
extern LEX_STRING interval_type_to_name[];

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
check_date(const MYSQL_TIME *ltime, ulonglong flags, int *was_cut)
{
 return check_date(ltime, non_zero_date(ltime), flags, was_cut);
}
bool check_date_with_warn(const MYSQL_TIME *ltime, ulonglong fuzzy_date,
                          timestamp_type ts_type);
bool make_date_with_warn(MYSQL_TIME *ltime,
                         ulonglong fuzzy_date, timestamp_type ts_type);
bool adjust_time_range_with_warn(MYSQL_TIME *ltime, uint dec);

#endif /* SQL_TIME_INCLUDED */
