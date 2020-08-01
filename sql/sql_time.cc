/* Copyright (c) 2000, 2010, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

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


/* Functions to handle date and time */

#include "mariadb.h"
#include "sql_time.h"
#include "tztime.h"                      // struct Time_zone
#include "sql_class.h"                   // THD
#include <m_ctype.h>


#define MAX_DAY_NUMBER 3652424L

	/* Some functions to calculate dates */

/*
  Name description of interval names used in statements.

  'interval_type_to_name' is ordered and sorted on interval size and
  interval complexity.
  Order of elements in 'interval_type_to_name' should correspond to 
  the order of elements in 'interval_type' enum
  
  See also interval_type, interval_names, append_interval
*/

LEX_CSTRING interval_type_to_name[INTERVAL_LAST] = {
  { STRING_WITH_LEN("YEAR")},
  { STRING_WITH_LEN("QUARTER")},
  { STRING_WITH_LEN("MONTH")},
  { STRING_WITH_LEN("WEEK")},
  { STRING_WITH_LEN("DAY")},
  { STRING_WITH_LEN("HOUR")},
  { STRING_WITH_LEN("MINUTE")},
  { STRING_WITH_LEN("SECOND")},
  { STRING_WITH_LEN("MICROSECOND")},
  { STRING_WITH_LEN("YEAR_MONTH")},
  { STRING_WITH_LEN("DAY_HOUR")},
  { STRING_WITH_LEN("DAY_MINUTE")},
  { STRING_WITH_LEN("DAY_SECOND")},
  { STRING_WITH_LEN("HOUR_MINUTE")},
  { STRING_WITH_LEN("HOUR_SECOND")},
  { STRING_WITH_LEN("MINUTE_SECOND")},
  { STRING_WITH_LEN("DAY_MICROSECOND")},
  { STRING_WITH_LEN("HOUR_MICROSECOND")},
  { STRING_WITH_LEN("MINUTE_MICROSECOND")},
  { STRING_WITH_LEN("SECOND_MICROSECOND")}
};

int append_interval(String *str, interval_type int_type, const INTERVAL &interval)
{
  char buf[64];
  size_t len;
  switch (int_type) {
  case INTERVAL_YEAR:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.year);
    break;
  case INTERVAL_QUARTER:
  case INTERVAL_MONTH:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.month);
    int_type=INTERVAL_MONTH;
    break;
  case INTERVAL_WEEK:
  case INTERVAL_DAY:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.day);
    int_type=INTERVAL_DAY;
    break;
  case INTERVAL_HOUR:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.hour);
    break;
  case INTERVAL_MINUTE:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.minute);
    break;
  case INTERVAL_SECOND:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.second);
    break;
  case INTERVAL_MICROSECOND:
    len= my_snprintf(buf,sizeof(buf),"%u", interval.second_part);
    break;
  case INTERVAL_YEAR_MONTH:
    len= my_snprintf(buf,sizeof(buf),"%u-%02u", interval.day, interval.month);
    break;
  case INTERVAL_DAY_HOUR:
    len= my_snprintf(buf,sizeof(buf),"%u %u", interval.day, interval.hour);
    break;
  case INTERVAL_DAY_MINUTE:
    len= my_snprintf(buf,sizeof(buf),"%u %u:%02u", interval.day, interval.hour, interval.minute);
    break;
  case INTERVAL_DAY_SECOND:
    len= my_snprintf(buf,sizeof(buf),"%u %u:%02u:%02u", interval.day, interval.hour, interval.minute, interval.second);
    break;
  case INTERVAL_HOUR_MINUTE:
    len= my_snprintf(buf,sizeof(buf),"%u:%02u", interval.hour, interval.minute);
    break;
  case INTERVAL_HOUR_SECOND:
    len= my_snprintf(buf,sizeof(buf),"%u:%02u:%02u", interval.hour, interval.minute, interval.second);
    break;
  case INTERVAL_MINUTE_SECOND:
    len= my_snprintf(buf,sizeof(buf),"%u:%02u", interval.minute, interval.second);
    break;
  case INTERVAL_DAY_MICROSECOND:
    len= my_snprintf(buf,sizeof(buf),"%u %u:%02u:%02u.%06u", interval.day, interval.hour, interval.minute, interval.second, interval.second_part);
    break;
  case INTERVAL_HOUR_MICROSECOND:
    len= my_snprintf(buf,sizeof(buf),"%u:%02u:%02u.%06u", interval.hour, interval.minute, interval.second, interval.second_part);
    break;
  case INTERVAL_MINUTE_MICROSECOND:
    len= my_snprintf(buf,sizeof(buf),"%u:%02u.%06u", interval.minute, interval.second, interval.second_part);
    break;
  case INTERVAL_SECOND_MICROSECOND:
    len= my_snprintf(buf,sizeof(buf),"%u.%06u", interval.second, interval.second_part);
    break;
  default:
    DBUG_ASSERT(0);
    len= 0;
  }
  return str->append(buf, len) || str->append(' ') ||
         str->append(interval_type_to_name + int_type);
}


/*
  Calc weekday from daynr
  Returns 0 for monday, 1 for tuesday ...
*/

int calc_weekday(long daynr,bool sunday_first_day_of_week)
{
  DBUG_ENTER("calc_weekday");
  DBUG_RETURN ((int) ((daynr + 5L + (sunday_first_day_of_week ? 1L : 0L)) % 7));
}

/*
  The bits in week_format has the following meaning:
   WEEK_MONDAY_FIRST (0)  If not set	Sunday is first day of week
      		   	  If set	Monday is first day of week
   WEEK_YEAR (1)	  If not set	Week is in range 0-53

   	Week 0 is returned for the the last week of the previous year (for
	a date at start of january) In this case one can get 53 for the
	first week of next year.  This flag ensures that the week is
	relevant for the given year. Note that this flag is only
	releveant if WEEK_JANUARY is not set.

			  If set	 Week is in range 1-53.

	In this case one may get week 53 for a date in January (when
	the week is that last week of previous year) and week 1 for a
	date in December.

  WEEK_FIRST_WEEKDAY (2)  If not set	Weeks are numbered according
			   		to ISO 8601:1988
			  If set	The week that contains the first
					'first-day-of-week' is week 1.
	
	ISO 8601:1988 means that if the week containing January 1 has
	four or more days in the new year, then it is week 1;
	Otherwise it is the last week of the previous year, and the
	next week is week 1.
*/

uint calc_week(const MYSQL_TIME *l_time, uint week_behaviour, uint *year)
{
  uint days;
  ulong daynr=calc_daynr(l_time->year,l_time->month,l_time->day);
  ulong first_daynr=calc_daynr(l_time->year,1,1);
  bool monday_first= MY_TEST(week_behaviour & WEEK_MONDAY_FIRST);
  bool week_year= MY_TEST(week_behaviour & WEEK_YEAR);
  bool first_weekday= MY_TEST(week_behaviour & WEEK_FIRST_WEEKDAY);

  uint weekday=calc_weekday(first_daynr, !monday_first);
  *year=l_time->year;

  if (l_time->month == 1 && l_time->day <= 7-weekday)
  {
    if (!week_year && 
	((first_weekday && weekday != 0) ||
	 (!first_weekday && weekday >= 4)))
      return 0;
    week_year= 1;
    (*year)--;
    first_daynr-= (days=calc_days_in_year(*year));
    weekday= (weekday + 53*7- days) % 7;
  }

  if ((first_weekday && weekday != 0) ||
      (!first_weekday && weekday >= 4))
    days= daynr - (first_daynr+ (7-weekday));
  else
    days= daynr - (first_daynr - weekday);

  if (week_year && days >= 52*7)
  {
    weekday= (weekday + calc_days_in_year(*year)) % 7;
    if ((!first_weekday && weekday < 4) ||
	(first_weekday && weekday == 0))
    {
      (*year)++;
      return 1;
    }
  }
  return days/7+1;
}

	/* Change a daynr to year, month and day */
	/* Daynr 0 is returned as date 00.00.00 */

bool get_date_from_daynr(long daynr,uint *ret_year,uint *ret_month,
			 uint *ret_day)
{
  uint year,temp,leap_day,day_of_year,days_in_year;
  uchar *month_pos;
  DBUG_ENTER("get_date_from_daynr");

  if (daynr < 366 || daynr > MAX_DAY_NUMBER)
    DBUG_RETURN(1);

  year= (uint) (daynr*100 / 36525L);
  temp=(((year-1)/100+1)*3)/4;
  day_of_year=(uint) (daynr - (long) year * 365L) - (year-1)/4 +temp;
  while (day_of_year > (days_in_year= calc_days_in_year(year)))
  {
    day_of_year-=days_in_year;
    (year)++;
  }
  leap_day=0;
  if (days_in_year == 366)
  {
    if (day_of_year > 31+28)
    {
      day_of_year--;
      if (day_of_year == 31+28)
        leap_day=1;		/* Handle leapyears leapday */
    }
  }
  *ret_month=1;
  for (month_pos= days_in_month ;
       day_of_year > (uint) *month_pos ;
       day_of_year-= *(month_pos++), (*ret_month)++)
    ;
  *ret_year=year;
  *ret_day=day_of_year+leap_day;
  DBUG_RETURN(0);
}

	/* Functions to handle periods */

ulong convert_period_to_month(ulong period)
{
  ulong a,b;
  if (period == 0 || period > 999912)
    return 0L;
  if ((a=period/100) < YY_PART_YEAR)
    a+=2000;
  else if (a < 100)
    a+=1900;
  b=period%100;
  return a*12+b-1;
}


ulong convert_month_to_period(ulong month)
{
  ulong year;
  if (month == 0L)
    return 0L;
  if ((year=month/12) < 100)
  {
    year+=(year < YY_PART_YEAR) ? 2000 : 1900;
  }
  return year*100+month%12+1;
}


bool
check_date_with_warn(THD *thd, const MYSQL_TIME *ltime,
                     date_conv_mode_t fuzzydate, timestamp_type ts_type)
{
  int unused;
  if (check_date(ltime, fuzzydate, &unused))
  {
    ErrConvTime str(ltime);
    make_truncated_value_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                                 &str, ts_type, nullptr, nullptr, nullptr);
    return true;
  }
  return false;
}


bool
adjust_time_range_with_warn(THD *thd, MYSQL_TIME *ltime, uint dec)
{
  MYSQL_TIME copy= *ltime;
  ErrConvTime str(&copy);
  int warnings= 0;
  if (check_time_range(ltime, dec, &warnings))
    return true;
  if (warnings)
    thd->push_warning_truncated_wrong_value("time", str.ptr());
  return false;
}

/*
  Convert a string to 8-bit representation,
  for use in str_to_time/str_to_date/str_to_date.
  
  In the future to_ascii() can be extended to convert
  non-ASCII digits to ASCII digits
  (for example, ARABIC-INDIC, DEVANAGARI, BENGALI, and so on)
  so DATE/TIME/DATETIME values understand digits in the
  respected scripts.
*/
static uint
to_ascii(CHARSET_INFO *cs,
         const char *src, size_t src_length,
         char *dst, size_t dst_length)
                     
{
  int cnvres;
  my_wc_t wc;
  const char *srcend= src + src_length;
  char *dst0= dst, *dstend= dst + dst_length - 1;
  while (dst < dstend &&
         (cnvres= cs->mb_wc(&wc,
                            (const uchar*) src,
                            (const uchar*) srcend)) > 0 &&
         wc < 128)
  {
    src+= cnvres;
    *dst++= static_cast<char>(wc);
  }
  *dst= '\0';
  return (uint)(dst - dst0);
}


class TemporalAsciiBuffer: public LEX_CSTRING
{
  char cnv[32];
public:
  TemporalAsciiBuffer(const char *str, size_t length, CHARSET_INFO *cs)
  {
    if ((cs->state & MY_CS_NONASCII) != 0)
    {
      LEX_CSTRING::str= cnv;
      LEX_CSTRING::length= to_ascii(cs, str, length, cnv, sizeof(cnv));
    }
    else
    {
      LEX_CSTRING::str= str;
      LEX_CSTRING::length= length;
    }
  }
};


/* Character set-aware version of ascii_to_datetime_or_date_or_time() */
bool Temporal::str_to_datetime_or_date_or_time(THD *thd, MYSQL_TIME_STATUS *st,
                                               const char *str, size_t length,
                                               CHARSET_INFO *cs,
                                               date_mode_t fuzzydate)
{
  TemporalAsciiBuffer tmp(str, length, cs);
  return ascii_to_datetime_or_date_or_time(st, tmp.str, tmp.length, fuzzydate)||
         add_nanoseconds(thd, &st->warnings, fuzzydate, st->nanoseconds);
}


/* Character set-aware version of str_to_datetime_or_date() */
bool Temporal::str_to_datetime_or_date(THD *thd, MYSQL_TIME_STATUS *status,
                                       const char *str, size_t length,
                                       CHARSET_INFO *cs,
                                       date_mode_t flags)
{
  TemporalAsciiBuffer tmp(str, length, cs);
  return ascii_to_datetime_or_date(status, tmp.str, tmp.length, flags) ||
         add_nanoseconds(thd, &status->warnings, flags, status->nanoseconds);
}


/* Character set-aware version of ascii_to_temporal() */
bool Temporal::str_to_temporal(THD *thd, MYSQL_TIME_STATUS *status,
                               const char *str, size_t length, CHARSET_INFO *cs,
                               date_mode_t flags)
{
  TemporalAsciiBuffer tmp(str, length, cs);
  return ascii_to_temporal(status, tmp.str, tmp.length, flags) ||
         add_nanoseconds(thd, &status->warnings, flags, status->nanoseconds);
}


/* Character set-aware version of str_to_DDhhmmssff() */
bool Interval_DDhhmmssff::str_to_DDhhmmssff(MYSQL_TIME_STATUS *status,
                                            const char *str, size_t length,
                                            CHARSET_INFO *cs, ulong max_hour)
{
  TemporalAsciiBuffer tmp(str, length, cs);
  bool rc= ::str_to_DDhhmmssff(tmp.str, tmp.length, this, UINT_MAX32, status);
  DBUG_ASSERT(status->warnings || !rc);
  return rc;
}


/*
  Convert a timestamp string to a MYSQL_TIME value and produce a warning 
  if string was truncated during conversion.

  NOTE
    See description of str_to_datetime_xxx() for more information.
*/

bool
str_to_datetime_with_warn(THD *thd, CHARSET_INFO *cs,
                          const char *str, size_t length, MYSQL_TIME *to,
                          date_mode_t mode)
{
  Temporal::Warn_push warn(thd, nullptr, nullptr, nullptr, to, mode);
  Temporal_hybrid *t= new(to) Temporal_hybrid(thd, &warn, str, length, cs, mode);
  return !t->is_valid_temporal();
}


bool double_to_datetime_with_warn(THD *thd, double value, MYSQL_TIME *ltime,
                                  date_mode_t fuzzydate,
                                  const TABLE_SHARE *s, const char *field_name)
{
  Temporal::Warn_push warn(thd, s ? s->db.str : nullptr,
                           s ? s->table_name.str : nullptr,
                           field_name, ltime, fuzzydate);
  Temporal_hybrid *t= new (ltime) Temporal_hybrid(thd, &warn, value, fuzzydate);
  return !t->is_valid_temporal();
}


bool decimal_to_datetime_with_warn(THD *thd, const my_decimal *value,
                                   MYSQL_TIME *ltime,
                                   date_mode_t fuzzydate,
                                   const TABLE_SHARE *s, const char *field_name)
{
  Temporal::Warn_push warn(thd, s ? s->db.str : nullptr,
                           s ? s->table_name.str : nullptr,
                           field_name, ltime, fuzzydate);
  Temporal_hybrid *t= new (ltime) Temporal_hybrid(thd, &warn, value, fuzzydate);
  return !t->is_valid_temporal();
}


bool int_to_datetime_with_warn(THD *thd, const Longlong_hybrid &nr,
                               MYSQL_TIME *ltime,
                               date_mode_t fuzzydate,
                               const TABLE_SHARE *s, const char *field_name)
{
  /*
    Note: conversion from an integer to TIME can overflow to '838:59:59.999999',
    so the conversion result can have fractional digits.
  */
  Temporal::Warn_push warn(thd, s ? s->db.str : nullptr,
                           s ? s->table_name.str : nullptr,
                           field_name, ltime, fuzzydate);
  Temporal_hybrid *t= new (ltime) Temporal_hybrid(thd, &warn, nr, fuzzydate);
  return !t->is_valid_temporal();
}


/*
  Convert a datetime from broken-down MYSQL_TIME representation to
  corresponding TIMESTAMP value.

  SYNOPSIS
    TIME_to_timestamp()
      thd             - current thread
      t               - datetime in broken-down representation, 
      error_code      - 0, if the conversion was successful;
                        ER_WARN_DATA_OUT_OF_RANGE, if t contains datetime value
                           which is out of TIMESTAMP range;
                        ER_WARN_INVALID_TIMESTAMP, if t represents value which
                           doesn't exists (falls into the spring time-gap).
   
  RETURN
     Number seconds in UTC since start of Unix Epoch corresponding to t.
     0 - in case of ER_WARN_DATA_OUT_OF_RANGE
*/

my_time_t TIME_to_timestamp(THD *thd, const MYSQL_TIME *t, uint *error_code)
{
  thd->time_zone_used= 1;
  return thd->variables.time_zone->TIME_to_gmt_sec(t, error_code);
}


/*
  Convert a system time structure to TIME
*/

void localtime_to_TIME(MYSQL_TIME *to, struct tm *from)
{
  to->neg=0;
  to->second_part=0;
  to->year=	(int) ((from->tm_year+1900) % 10000);
  to->month=	(int) from->tm_mon+1;
  to->day=	(int) from->tm_mday;
  to->hour=	(int) from->tm_hour;
  to->minute=	(int) from->tm_min;
  to->second=   (int) from->tm_sec;
}


void calc_time_from_sec(MYSQL_TIME *to, ulong seconds, ulong microseconds)
{
  long t_seconds;
  // to->neg is not cleared, it may already be set to a useful value
  to->time_type= MYSQL_TIMESTAMP_TIME;
  to->year= 0;
  to->month= 0;
  to->day= 0;
  to->hour= seconds/3600L;
  t_seconds= seconds%3600L;
  to->minute= t_seconds/60L;
  to->second= t_seconds%60L;
  to->second_part= microseconds;
}


/*
  Parse a format string specification

  SYNOPSIS
    parse_date_time_format()
    format_type		Format of string (time, date or datetime)
    format_str		String to parse
    format_length	Length of string
    date_time_format	Format to fill in

  NOTES
    Fills in date_time_format->positions for all date time parts.

    positions marks the position for a datetime element in the format string.
    The position array elements are in the following order:
    YYYY-DD-MM HH-MM-DD.FFFFFF AM
    0    1  2  3  4  5  6      7

    If positions[0]= 5, it means that year will be the forth element to
    read from the parsed date string.

  RETURN
    0	ok
    1	error
*/

bool parse_date_time_format(timestamp_type format_type, 
			    const char *format, uint format_length,
			    DATE_TIME_FORMAT *date_time_format)
{
  uint offset= 0, separators= 0;
  const char *ptr= format, *format_str;
  const char *end= ptr+format_length;
  uchar *dt_pos= date_time_format->positions;
  /* need_p is set if we are using AM/PM format */
  bool need_p= 0, allow_separator= 0;
  ulong part_map= 0, separator_map= 0;
  const char *parts[16];

  date_time_format->time_separator= 0;
  date_time_format->flag= 0;			// For future

  /*
    Fill position with 'dummy' arguments to found out if a format tag is
    used twice (This limit's the format to 255 characters, but this is ok)
  */
  dt_pos[0]= dt_pos[1]= dt_pos[2]= dt_pos[3]=
    dt_pos[4]= dt_pos[5]= dt_pos[6]= dt_pos[7]= 255;

  for (; ptr != end; ptr++)
  {
    if (*ptr == '%' && ptr+1 != end)
    {
      uint UNINIT_VAR(position);
      switch (*++ptr) {
      case 'y':					// Year
      case 'Y':
	position= 0;
	break;
      case 'c':					// Month
      case 'm':
	position= 1;
	break;
      case 'd':
      case 'e':
	position= 2;
	break;
      case 'h':
      case 'I':
      case 'l':
	need_p= 1;				// Need AM/PM
	/* Fall through */
      case 'k':
      case 'H':
	position= 3;
	break;
      case 'i':
	position= 4;
	break;
      case 's':
      case 'S':
	position= 5;
	break;
      case 'f':
	position= 6;
	if (dt_pos[5] != offset-1 || ptr[-2] != '.')
	  return 1;				// Wrong usage of %f
	break;
      case 'p':					// AM/PM
	if (offset == 0)			// Can't be first
	  return 0;
	position= 7;
	break;
      default:
	return 1;				// Unknown controll char
      }
      if (dt_pos[position] != 255)		// Don't allow same tag twice
	return 1;
      parts[position]= ptr-1;

      /*
	If switching from time to date, ensure that all time parts
	are used
      */
      if (part_map && position <= 2 && !(part_map & (1 | 2 | 4)))
	offset=5;
      part_map|= (ulong) 1 << position;
      dt_pos[position]= offset++;
      allow_separator= 1;
    }
    else
    {
      /*
	Don't allow any characters in format as this could easily confuse
	the date reader
      */
      if (!allow_separator)
	return 1;				// No separator here
      allow_separator= 0;			// Don't allow two separators
      separators++;
      /* Store in separator_map which parts are punct characters */
      if (my_ispunct(&my_charset_latin1, *ptr))
	separator_map|= (ulong) 1 << (offset-1);
      else if (!my_isspace(&my_charset_latin1, *ptr))
	return 1;
    }
  }

  /* If no %f, specify it after seconds.  Move %p up, if necessary */
  if ((part_map & 32) && !(part_map & 64))
  {
    dt_pos[6]= dt_pos[5] +1;
    parts[6]= parts[5];				// For later test in (need_p)
    if (dt_pos[6] == dt_pos[7])			// Move %p one step up if used
      dt_pos[7]++;
  }

  /*
    Check that we have not used a non legal format specifier and that all
    format specifiers have been used

    The last test is to ensure that %p is used if and only if
    it's needed.
  */
  if ((format_type == MYSQL_TIMESTAMP_DATETIME &&
       !test_all_bits(part_map, (1 | 2 | 4 | 8 | 16 | 32))) ||
      (format_type == MYSQL_TIMESTAMP_DATE && part_map != (1 | 2 | 4)) ||
      (format_type == MYSQL_TIMESTAMP_TIME &&
       !test_all_bits(part_map, 8 | 16 | 32)) ||
      !allow_separator ||			// %option should be last
      (need_p && dt_pos[6] +1 != dt_pos[7]) ||
      (need_p ^ (dt_pos[7] != 255)))
    return 1;

  if (dt_pos[6] != 255)				// If fractional seconds
  {
    /* remove fractional seconds from later tests */
    uint pos= dt_pos[6] -1;
    /* Remove separator before %f from sep map */
    separator_map= ((separator_map & ((ulong) (1 << pos)-1)) |
		    ((separator_map & ~((ulong) (1 << pos)-1)) >> 1));
    if (part_map & 64)			      
    {
      separators--;				// There is always a separator
      need_p= 1;				// force use of separators
    }
  }

  /*
    Remove possible separator before %p from sep_map
    (This can either be at position 3, 4, 6 or 7) h.m.d.%f %p
  */
  if (dt_pos[7] != 255)
  {
    if (need_p && parts[7] != parts[6]+2)
      separators--;
  }     
  /*
    Calculate if %p is in first or last part of the datetime field

    At this point we have either %H-%i-%s %p 'year parts' or
    'year parts' &H-%i-%s %p" as %f was removed above
  */
  offset= dt_pos[6] <= 3 ? 3 : 6;
  /* Remove separator before %p from sep map */
  separator_map= ((separator_map & ((ulong) (1 << offset)-1)) |
		  ((separator_map & ~((ulong) (1 << offset)-1)) >> 1));

  format_str= 0;
  switch (format_type) {
  case MYSQL_TIMESTAMP_DATE:
    format_str= known_date_time_formats[INTERNAL_FORMAT].date_format;
    /* fall through */
  case MYSQL_TIMESTAMP_TIME:
    if (!format_str)
      format_str=known_date_time_formats[INTERNAL_FORMAT].time_format;

    /*
      If there is no separators, allow the internal format as we can read
      this.  If separators are used, they must be between each part
    */
    if (format_length == 6 && !need_p &&
	!my_charset_bin.strnncoll(format, 6, format_str, 6))
      return 0;
    if (separator_map == (1 | 2))
    {
      if (format_type == MYSQL_TIMESTAMP_TIME)
      {
	if (*(format+2) != *(format+5))
	  break;				// Error
	/* Store the character used for time formats */
	date_time_format->time_separator= *(format+2);
      }
      return 0;
    }
    break;
  case MYSQL_TIMESTAMP_DATETIME:
    /*
      If there is no separators, allow the internal format as we can read
      this.  If separators are used, they must be between each part.
      Between DATE and TIME we also allow space as separator
    */
    if ((format_length == 12 && !need_p &&
	 !my_charset_bin.strnncoll(
		       format, 12,
		       known_date_time_formats[INTERNAL_FORMAT].datetime_format,
		       12)) ||
	(separators == 5 && separator_map == (1 | 2 | 8 | 16)))
      return 0;
    break;
  default:
    DBUG_ASSERT(0);
    break;
  }
  return 1;					// Error
}


/*
  Create a DATE_TIME_FORMAT object from a format string specification

  SYNOPSIS
    date_time_format_make()
    format_type		Format to parse (time, date or datetime)
    format_str		String to parse
    format_length	Length of string

  NOTES
    The returned object should be freed with my_free()

  RETURN
    NULL ponter:	Error
    new object
*/

DATE_TIME_FORMAT
*date_time_format_make(timestamp_type format_type,
		       const char *format_str, uint format_length)
{
  DATE_TIME_FORMAT tmp;

  if (format_length && format_length < 255 &&
      !parse_date_time_format(format_type, format_str,
			      format_length, &tmp))
  {
    tmp.format.str=    format_str;
    tmp.format.length= format_length;
    return date_time_format_copy((THD *)0, &tmp);
  }
  return 0;
}


/*
  Create a copy of a DATE_TIME_FORMAT object

  SYNOPSIS
    date_and_time_format_copy()
    thd			Set if variable should be allocated in thread mem
    format		format to copy

  NOTES
    The returned object should be freed with my_free()

  RETURN
    NULL ponter:	Error
    new object
*/

DATE_TIME_FORMAT *date_time_format_copy(THD *thd, DATE_TIME_FORMAT *format)
{
  DATE_TIME_FORMAT *new_format;
  size_t length= sizeof(*format) + format->format.length + 1;
  char *format_pos;

  if (thd)
    new_format= (DATE_TIME_FORMAT *) thd->alloc(length);
  else
    new_format=  (DATE_TIME_FORMAT *) my_malloc(key_memory_DATE_TIME_FORMAT,
                                                length, MYF(MY_WME));
  if (new_format)
  {
    /* Put format string after current pos */
    new_format->format.str= format_pos= (char*) (new_format+1);
    memcpy((char*) new_format->positions, (char*) format->positions,
	   sizeof(format->positions));
    new_format->time_separator= format->time_separator;
    /* We make the string null terminated for easy printf in SHOW VARIABLES */
    memcpy(format_pos, format->format.str, format->format.length);
    format_pos[format->format.length]= 0;
    new_format->format.length= format->format.length;
  }
  return new_format;
}


KNOWN_DATE_TIME_FORMAT known_date_time_formats[6]=
{
  {"USA", "%m.%d.%Y", "%Y-%m-%d %H.%i.%s", "%h:%i:%s %p" },
  {"JIS", "%Y-%m-%d", "%Y-%m-%d %H:%i:%s", "%H:%i:%s" },
  {"ISO", "%Y-%m-%d", "%Y-%m-%d %H:%i:%s", "%H:%i:%s" },
  {"EUR", "%d.%m.%Y", "%Y-%m-%d %H.%i.%s", "%H.%i.%s" },
  {"INTERNAL", "%Y%m%d",   "%Y%m%d%H%i%s", "%H%i%s" },
  { 0, 0, 0, 0 }
};


const char *get_date_time_format_str(KNOWN_DATE_TIME_FORMAT *format,
				     timestamp_type type)
{
  switch (type) {
  case MYSQL_TIMESTAMP_DATE:
    return format->date_format;
  case MYSQL_TIMESTAMP_DATETIME:
    return format->datetime_format;
  case MYSQL_TIMESTAMP_TIME:
    return format->time_format;
  default:
    DBUG_ASSERT(0);				// Impossible
    return 0;
  }
}


/**
  Convert TIME/DATE/DATETIME value to String.
  @param l_time   DATE value
  @param OUT str  String to convert to
  @param dec      Number of fractional digits.
*/
bool my_TIME_to_str(const MYSQL_TIME *ltime, String *str, uint dec)
{
  if (str->alloc(MAX_DATE_STRING_REP_LENGTH))
    return true;
  str->set_charset(&my_charset_numeric);
  str->length(my_TIME_to_str(ltime, const_cast<char*>(str->ptr()), dec));
  return false;
}


void make_truncated_value_warning(THD *thd,
                                  Sql_condition::enum_warning_level level,
                                  const ErrConv *sval,
				  timestamp_type time_type,
                                  const char *db_name, const char *table_name,
                                  const char *field_name)
{
  const char *type_str= Temporal::type_name_by_timestamp_type(time_type);
  return thd->push_warning_wrong_or_truncated_value
    (level, time_type <= MYSQL_TIMESTAMP_ERROR, type_str, sval->ptr(),
     db_name, table_name, field_name);
}


/* Daynumber from year 0 to 9999-12-31 */
#define COMBINE(X)                                                      \
               (((((X)->day * 24LL + (X)->hour) * 60LL +                \
                   (X)->minute) * 60LL + (X)->second)*1000000LL +       \
                   (X)->second_part)
#define GET_PART(X, N) X % N ## LL; X/= N ## LL

bool date_add_interval(THD *thd, MYSQL_TIME *ltime, interval_type int_type,
                       const INTERVAL &interval, bool push_warn)
{
  long period, sign;

  sign= (interval.neg == (bool)ltime->neg ? 1 : -1);

  switch (int_type) {
  case INTERVAL_SECOND:
  case INTERVAL_SECOND_MICROSECOND:
  case INTERVAL_MICROSECOND:
  case INTERVAL_MINUTE:
  case INTERVAL_HOUR:
  case INTERVAL_MINUTE_MICROSECOND:
  case INTERVAL_MINUTE_SECOND:
  case INTERVAL_HOUR_MICROSECOND:
  case INTERVAL_HOUR_SECOND:
  case INTERVAL_HOUR_MINUTE:
  case INTERVAL_DAY_MICROSECOND:
  case INTERVAL_DAY_SECOND:
  case INTERVAL_DAY_MINUTE:
  case INTERVAL_DAY_HOUR:
  case INTERVAL_DAY:
  {
    longlong usec, daynr;
    my_bool neg= 0;
    enum enum_mysql_timestamp_type time_type= ltime->time_type;

    if (((ulonglong) interval.day +
         (ulonglong) interval.hour / 24 +
         (ulonglong) interval.minute / 24 / 60 +
         (ulonglong) interval.second / 24 / 60 / 60) > MAX_DAY_NUMBER)
      goto invalid_date;

    if (time_type != MYSQL_TIMESTAMP_TIME)
      ltime->day+= calc_daynr(ltime->year, ltime->month, 1) - 1;

    usec= COMBINE(ltime) + sign*COMBINE(&interval);

    if (usec < 0)
    {
      neg= 1;
      usec= -usec;
    }

    ltime->second_part= GET_PART(usec, 1000000);
    ltime->second= GET_PART(usec, 60);
    ltime->minute= GET_PART(usec, 60);
    ltime->neg^= neg;

    if (time_type == MYSQL_TIMESTAMP_TIME)
    {
      if (usec > TIME_MAX_HOUR)
        goto invalid_date;
      ltime->hour= static_cast<uint>(usec);
      ltime->day= 0;
      return 0;
    }
    else if (ltime->neg)
      goto invalid_date;

    if (int_type != INTERVAL_DAY)
      ltime->time_type= MYSQL_TIMESTAMP_DATETIME; // Return full date

    ltime->hour= GET_PART(usec, 24);
    daynr= usec;

    /* Day number from year 0 to 9999-12-31 */
    if (get_date_from_daynr((long) daynr, &ltime->year, &ltime->month,
                            &ltime->day))
      goto invalid_date;
    break;
  }
  case INTERVAL_WEEK:
    period= (calc_daynr(ltime->year,ltime->month,ltime->day) +
             sign * (long) interval.day);
    /* Daynumber from year 0 to 9999-12-31 */
    if (get_date_from_daynr((long) period,&ltime->year,&ltime->month,
                            &ltime->day))
      goto invalid_date;
    break;
  case INTERVAL_YEAR:
    ltime->year+= sign * (long) interval.year;
    if ((ulong) ltime->year >= 10000L)
      goto invalid_date;
    if (ltime->month == 2 && ltime->day == 29 &&
	calc_days_in_year(ltime->year) != 366)
      ltime->day=28;				// Was leap-year
    break;
  case INTERVAL_YEAR_MONTH:
  case INTERVAL_QUARTER:
  case INTERVAL_MONTH:
    period= (ltime->year*12 + sign * (long) interval.year*12 +
	     ltime->month-1 + sign * (long) interval.month);
    if ((ulong) period >= 120000L)
      goto invalid_date;
    ltime->year= (uint) (period / 12);
    ltime->month= (uint) (period % 12L)+1;
    /* Adjust day if the new month doesn't have enough days */
    if (ltime->day > days_in_month[ltime->month-1])
    {
      ltime->day = days_in_month[ltime->month-1];
      if (ltime->month == 2 && calc_days_in_year(ltime->year) == 366)
	ltime->day++;				// Leap-year
    }
    break;
  default:
    goto null_date;
  }

  if (ltime->time_type != MYSQL_TIMESTAMP_TIME)
    return 0;                                   // Ok

invalid_date:
  if (push_warn)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_DATETIME_FUNCTION_OVERFLOW,
                        ER_THD(thd, ER_DATETIME_FUNCTION_OVERFLOW),
                        ltime->time_type == MYSQL_TIMESTAMP_TIME ?
                        "time" : "datetime");
  }
null_date:
  return 1;
}


/*
  Calculate difference between two datetime values as seconds + microseconds.

  SYNOPSIS
    calc_time_diff()
      l_time1         - TIME/DATE/DATETIME value
      l_time2         - TIME/DATE/DATETIME value
      l_sign          - 1 absolute values are substracted,
                        -1 absolute values are added.
      seconds_out     - Out parameter where difference between
                        l_time1 and l_time2 in seconds is stored.
      microseconds_out- Out parameter where microsecond part of difference
                        between l_time1 and l_time2 is stored.

  NOTE
    This function calculates difference between l_time1 and l_time2 absolute
    values. So one should set l_sign and correct result if he want to take
    signs into account (i.e. for MYSQL_TIME values).

  RETURN VALUES
    Returns sign of difference.
    1 means negative result
    0 means positive result

*/

bool
calc_time_diff(const MYSQL_TIME *l_time1, const MYSQL_TIME *l_time2,
               int l_sign, ulonglong *seconds_out, ulong *microseconds_out)
{
  long days;
  bool neg;
  longlong microseconds;

  /*
    We suppose that if first argument is MYSQL_TIMESTAMP_TIME
    the second argument should be TIMESTAMP_TIME also.
    We should check it before calc_time_diff call.
  */
  if (l_time1->time_type == MYSQL_TIMESTAMP_TIME)  // Time value
    days= (long)l_time1->day - l_sign * (long)l_time2->day;
  else
  {
    days= calc_daynr((uint) l_time1->year,
		     (uint) l_time1->month,
		     (uint) l_time1->day);
    if (l_time2->time_type == MYSQL_TIMESTAMP_TIME)
      days-= l_sign * (long)l_time2->day;
    else
      days-= l_sign*calc_daynr((uint) l_time2->year,
			       (uint) l_time2->month,
			       (uint) l_time2->day);
  }

  microseconds= ((longlong)days * SECONDS_IN_24H +
                 (longlong)(l_time1->hour*3600LL +
                            l_time1->minute*60L +
                            l_time1->second) -
                 l_sign*(longlong)(l_time2->hour*3600LL +
                                   l_time2->minute*60L +
                                   l_time2->second)) * 1000000LL +
                (longlong)l_time1->second_part -
                l_sign*(longlong)l_time2->second_part;

  neg= 0;
  if (microseconds < 0)
  {
    microseconds= -microseconds;
    neg= 1;
  }
  *seconds_out= (ulonglong) microseconds/1000000L;
  *microseconds_out= (ulong) (microseconds%1000000L);
  return neg;
}


bool calc_time_diff(const MYSQL_TIME *l_time1, const MYSQL_TIME *l_time2,
                    int l_sign, MYSQL_TIME *l_time3, date_mode_t fuzzydate)
{
  ulonglong seconds;
  ulong microseconds;
  bzero((char *) l_time3, sizeof(*l_time3));
  l_time3->neg= calc_time_diff(l_time1, l_time2, l_sign,
			       &seconds, &microseconds);
  /*
    For MYSQL_TIMESTAMP_TIME only:
      If first argument was negative and diff between arguments
      is non-zero we need to swap sign to get proper result.
  */
  if (l_time1->neg && (seconds || microseconds))
    l_time3->neg= 1 - l_time3->neg;         // Swap sign of result

  /*
    seconds is longlong, when casted to long it may become a small number
    even if the original seconds value was too large and invalid.
    as a workaround we limit seconds by a large invalid long number
    ("invalid" means > TIME_MAX_SECOND)
  */
  set_if_smaller(seconds, INT_MAX32);
  calc_time_from_sec(l_time3, (ulong) seconds, microseconds);
  return ((fuzzydate & TIME_NO_ZERO_DATE) && (seconds == 0) &&
          (microseconds == 0));
}


/*
  Compares 2 MYSQL_TIME structures

  SYNOPSIS
    my_time_compare()

      a - first time
      b - second time

  RETURN VALUE
   -1   - a < b
    0   - a == b
    1   - a > b

*/

int my_time_compare(const MYSQL_TIME *a, const MYSQL_TIME *b)
{
  ulonglong a_t= pack_time(a);
  ulonglong b_t= pack_time(b);

  if (a_t < b_t)
    return -1;
  if (a_t > b_t)
    return 1;

  return 0;
}


/**
  Convert TIME to DATETIME.
  @param   ltime    The value to convert.
  @return  false on success, true of error (negative time).
*/
bool time_to_datetime(MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ltime->time_type == MYSQL_TIMESTAMP_TIME);
  DBUG_ASSERT(ltime->year == 0);
  DBUG_ASSERT(ltime->month == 0);
  DBUG_ASSERT(ltime->day == 0);
  if (ltime->neg)
    return true;
  uint day= ltime->hour / 24;
  ltime->hour%= 24;
  ltime->month= day / 31;
  ltime->day= day % 31;  
  return false;
}


/*** Conversion from TIME to DATETIME ***/

/*
  Simple case: TIME is within normal 24 hours internal.
  Mix DATE part of ldate and TIME part of ltime together.
*/
static void
mix_date_and_time_simple(MYSQL_TIME *ldate, const MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ldate->time_type == MYSQL_TIMESTAMP_DATE ||
              ldate->time_type == MYSQL_TIMESTAMP_DATETIME);
  ldate->hour= ltime->hour;
  ldate->minute= ltime->minute;
  ldate->second= ltime->second;
  ldate->second_part= ltime->second_part;
  ldate->time_type= MYSQL_TIMESTAMP_DATETIME;
}


/*
  Complex case: TIME is negative or outside of the 24 hour interval.
*/
static void
mix_date_and_time_complex(MYSQL_TIME *ldate, const MYSQL_TIME *ltime)
{
  DBUG_ASSERT(ldate->time_type == MYSQL_TIMESTAMP_DATE ||
              ldate->time_type == MYSQL_TIMESTAMP_DATETIME);
  ulonglong seconds;
  ulong days, useconds;
  int sign= ltime->neg ? 1 : -1;
  ldate->neg= calc_time_diff(ldate, ltime, sign, &seconds, &useconds);

  DBUG_ASSERT(!ldate->neg);
  DBUG_ASSERT(ldate->year > 0);

  days= (long) (seconds / SECONDS_IN_24H);
  calc_time_from_sec(ldate, seconds % SECONDS_IN_24H, useconds);
  get_date_from_daynr(days, &ldate->year, &ldate->month, &ldate->day);
  ldate->time_type= MYSQL_TIMESTAMP_DATETIME;
}


/**
  Mix a date value and a time value.

  @param  IN/OUT  ldate  Date value.
  @param          ltime  Time value.
*/
static void
mix_date_and_time(MYSQL_TIME *to, const MYSQL_TIME *from)
{
  if (!from->neg && from->hour < 24)
    mix_date_and_time_simple(to, from);
  else
    mix_date_and_time_complex(to, from);
}


/**
  Get current date in DATE format
*/
void set_current_date(THD *thd, MYSQL_TIME *to)
{
  thd->variables.time_zone->gmt_sec_to_TIME(to, thd->query_start());
  thd->time_zone_used= 1;
  datetime_to_date(to);
}


/**
  5.5 compatible conversion from TIME to DATETIME
*/
static bool
time_to_datetime_old(THD *thd, const MYSQL_TIME *from, MYSQL_TIME *to)
{
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_TIME);

  if (from->neg)
    return true;

  /* Set the date part */
  uint day= from->hour / 24;
  to->day= day % 31;
  to->month= day / 31;
  to->year= 0;
  /* Set the time part */
  to->hour= from->hour % 24;
  to->minute= from->minute;
  to->second= from->second;
  to->second_part= from->second_part;
  /* set sign and type */
  to->neg= 0;
  to->time_type= MYSQL_TIMESTAMP_DATETIME;
  return false;
}


/**
  Convert time to datetime.

  The time value is added to the current datetime value.
  @param  IN  ltime    Time value to convert from.
  @param  OUT ltime2   Datetime value to convert to.
*/
bool
time_to_datetime(THD *thd, const MYSQL_TIME *from, MYSQL_TIME *to)
{
  if (thd->variables.old_behavior & OLD_MODE_ZERO_DATE_TIME_CAST)
    return time_to_datetime_old(thd, from, to);
  set_current_date(thd, to);
  mix_date_and_time(to, from);
  return false;
}


bool
time_to_datetime_with_warn(THD *thd,
                           const MYSQL_TIME *from, MYSQL_TIME *to,
                           date_conv_mode_t fuzzydate)
{
  int warn= 0;
  DBUG_ASSERT(from->time_type == MYSQL_TIMESTAMP_TIME);
  /*
    After time_to_datetime() we need to do check_date(), as
    the caller may want TIME_NO_ZERO_DATE or TIME_NO_ZERO_IN_DATE.
    Note, the SQL standard time->datetime conversion mode always returns
    a valid date based on CURRENT_DATE. So we need to do check_date()
    only in the old mode.
  */
  if (time_to_datetime(thd, from, to) ||
      ((thd->variables.old_behavior & OLD_MODE_ZERO_DATE_TIME_CAST) &&
        check_date(to, fuzzydate, &warn)))
  {
    ErrConvTime str(from);
    thd->push_warning_truncated_wrong_value("datetime", str.ptr());
    return true;
  }
  return false;
}


longlong pack_time(const MYSQL_TIME *my_time)
{
  return  ((((((my_time->year     * 13ULL +
               my_time->month)    * 32ULL +
               my_time->day)      * 24ULL +
               my_time->hour)     * 60ULL +
               my_time->minute)   * 60ULL +
               my_time->second)   * 1000000ULL +
               my_time->second_part) * (my_time->neg ? -1 : 1);
}

#define get_one(WHERE, FACTOR) WHERE= (ulong)(packed % FACTOR); packed/= FACTOR

void unpack_time(longlong packed, MYSQL_TIME *my_time,
                     enum_mysql_timestamp_type ts_type)
{
  if ((my_time->neg= packed < 0))
    packed= -packed;
  get_one(my_time->second_part, 1000000ULL);
  get_one(my_time->second,           60U);
  get_one(my_time->minute,           60U);
  get_one(my_time->hour,             24U);
  get_one(my_time->day,              32U);
  get_one(my_time->month,            13U);
  my_time->year= (uint)packed;
  my_time->time_type= ts_type;
  switch (ts_type) {
  case MYSQL_TIMESTAMP_TIME:
    my_time->hour+= (my_time->month * 32 + my_time->day) * 24;
    my_time->month= my_time->day= 0;
    break;
  case MYSQL_TIMESTAMP_DATE:
    my_time->hour= my_time->minute= my_time->second= my_time->second_part= 0;
    break;
  case MYSQL_TIMESTAMP_NONE:
  case MYSQL_TIMESTAMP_ERROR:
    DBUG_ASSERT(0);
  case MYSQL_TIMESTAMP_DATETIME:
    break;
  }
}
