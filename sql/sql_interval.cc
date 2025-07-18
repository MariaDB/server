/* Copyright (C) 2025

This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "sql_interval.h"
#include "m_string.h"
#include "my_time.h"
#include "sql_class.h"
#include "sql_basic_types.h"
#include "structs.h"
#include "mysql/plugin.h"

Interval::Interval()
{
  year= month= day= hour= 0;
  minute= second= second_part= 0;
  neg= false;
}


Interval::Interval(const char *str, size_t length,
                   enum interval_type itype, CHARSET_INFO *cs, uint8 start_prec, uint8 end_prec)
{
  TemporalAsciiBuffer tmp(str, length, cs);
  if (str_to_interval(tmp.str, tmp.length, this, itype,start_prec,end_prec) != 0)
    my_error(ER_INVALID_DEFAULT_PARAM, MYF(0), tmp.str);
}


static my_bool parse_number(ulong *dest, const char **str, const char *end)
{
  const char *start= *str;
  *dest= 0;

  while (*str < end && my_isdigit(&my_charset_latin1, *(*str)))
  {
    *dest= (*dest) * 10 + (*(*str) - '0');
    (*str)++;
  }
  return (*str == start);
}

int str_to_interval(const char *str, size_t length, Interval *to,
                    enum interval_type itype, uint8 start_prec, uint8 end_prec)
{
  const char *p= str;
  const char *end= str + length;
  ulong values[6] = {0}; // Stores the values of temporal components: YEAR, MONTH, DAY, HOUR, MINUTE, SECOND
  uint num_components= 0;
  uint parsed_components= 0;

  *to= Interval();

  while (p < end && my_isspace(&my_charset_latin1, *p))
    p++;

  if (p < end && *p == '-')
  {
    to->neg= true;
    p++;
  }
  else if (p < end && *p == '+')
    p++;

  switch (itype) {
  case INTERVAL_YEAR:
  case INTERVAL_QUARTER:
  case INTERVAL_MONTH:
  case INTERVAL_WEEK:
  case INTERVAL_DAY:
  case INTERVAL_HOUR:
  case INTERVAL_MINUTE:
    num_components= 1;
    break;
  case INTERVAL_SECOND:
    num_components= 1;
    break;
  case INTERVAL_YEAR_MONTH:
  case INTERVAL_DAY_HOUR:
  case INTERVAL_HOUR_MINUTE:
    num_components= 2;
    break;
  case INTERVAL_MINUTE_SECOND:
    num_components= 2;
    break;
  case INTERVAL_DAY_MINUTE:
    num_components= 3;
    break;
  case INTERVAL_HOUR_SECOND:
    num_components= 3;
    break;
  case INTERVAL_DAY_SECOND:
    num_components= 4;
    break;
  default:
    return 1;
  }

  for (uint i= 0; i < num_components; i++)
  {
    while (p < end && !my_isdigit(&my_charset_latin1, *p))
      p++;
    if (p >= end)
      break;

    if (parse_number(&values[i], &p, end))
    {
      if (i == 0)
        return 1;
      break;
    }
    parsed_components++;

    }

  switch (itype) {
  case INTERVAL_YEAR:
    to->year= values[0];
    break;
  case INTERVAL_MONTH:
    to->month= values[0];
    break;
  case INTERVAL_DAY:
    to->day= values[0];
    break;
  case INTERVAL_HOUR:
    to->hour= values[0];
    break;
  case INTERVAL_MINUTE:
    to->minute= values[0];
    break;
  case INTERVAL_SECOND:
    to->second= values[0];
    break;
  case INTERVAL_YEAR_MONTH:
    to->year= values[0];
    to->month= values[1];
    break;
  case INTERVAL_DAY_HOUR:
    to->day= values[0];
    to->hour= values[1];
    break;
  case INTERVAL_DAY_MINUTE:
    to->day= values[0];
    to->hour= values[1];
    to->minute= values[2];
    break;
  case INTERVAL_DAY_SECOND:
    to->day= values[0];
    to->hour= values[1];
    to->minute= values[2];
    to->second= values[3];
    break;
  case INTERVAL_HOUR_MINUTE:
    to->hour= values[0];
    to->minute= values[1];
    break;
  case INTERVAL_HOUR_SECOND:
    to->hour= values[0];
    to->minute= values[1];
    to->second= values[2];
    break;
  case INTERVAL_MINUTE_SECOND:
    to->minute= values[0];
    to->second= values[1];
    break;
  default:
    MY_ASSERT_UNREACHABLE();
    return 1;
  }

  return (parsed_components > 0) ?is_valid_interval(itype,start_prec,end_prec,to)?  0:1 : 1;
}


static uint8_t count_digits(ulong value) {
    return (value == 0) ? 1 : static_cast<uint8_t>(log10(value)) + 1;
}

uint8_t is_valid_interval(interval_type itype,
                          uint8_t start_prec,
                          uint8_t end_prec,
                          const Interval *ival) {
    switch (itype) {
    case INTERVAL_YEAR:
        return count_digits(ival->year) <= start_prec;

    case INTERVAL_MONTH:
        return count_digits(ival->month) <= start_prec;

    case INTERVAL_DAY:
        return count_digits(ival->day) <= start_prec;

    case INTERVAL_HOUR:
        return count_digits(ival->hour) <= start_prec;

    case INTERVAL_MINUTE:
        return count_digits(ival->minute) <= start_prec;

    case INTERVAL_SECOND:
        return count_digits(ival->second) <= start_prec;

    case INTERVAL_YEAR_MONTH:
        return count_digits(ival->year) <= start_prec &&
               count_digits(ival->month) <= end_prec;

    case INTERVAL_DAY_HOUR:
        return count_digits(ival->day) <= start_prec &&
               count_digits(ival->hour) <= end_prec;

    case INTERVAL_HOUR_MINUTE:
        return count_digits(ival->hour) <= start_prec &&
               count_digits(ival->minute) <= end_prec;

    case INTERVAL_MINUTE_SECOND:
        return count_digits(ival->minute) <= start_prec &&
               count_digits(ival->second) <= end_prec;

    case INTERVAL_DAY_MINUTE:
        return count_digits(ival->day) <= start_prec &&
               ival->hour <= 59 &&
               count_digits(ival->minute) <= end_prec;

    case INTERVAL_DAY_SECOND:
        return count_digits(ival->day) <= start_prec &&
               ival->hour <= 59 &&
               ival->minute <= 59 &&
               count_digits(ival->second) <= end_prec;

    case INTERVAL_HOUR_SECOND:
        return count_digits(ival->hour) <= start_prec &&
               ival->minute <= 59 &&
               count_digits(ival->second) <= end_prec;

    default:
        return 0;
    }
}

int interval_to_timeval(const Interval *iv, my_timeval *tm, THD *thd)
{
  longlong total_days= 0;
  total_days+= static_cast<longlong>(iv->year) * 365;
  total_days+= static_cast<longlong>(iv->month) * 30;
  total_days+= static_cast<longlong>(iv->day);

  longlong total_seconds= total_days * 86400;
  total_seconds+= static_cast<longlong>(iv->hour) * 3600;
  total_seconds+= static_cast<longlong>(iv->minute) * 60;
  total_seconds+= static_cast<longlong>(iv->second);

  ulong microseconds= static_cast<ulong>(iv->second_part);

  if (iv->neg)
  {
    total_seconds= -total_seconds;
    if (microseconds)
    {
      total_seconds-= 1;
      microseconds= 1000000 - microseconds;
    }
  }

  tm->tv_sec= total_seconds;
  tm->tv_usec= microseconds;
  return 0;
}

size_t interval_to_string(const Interval *iv, enum interval_type itype,
              char *buf, size_t buf_len)
{
  char *pos= buf;
  char *end= buf + buf_len;

  if (iv->neg && buf_len > 1)
    *pos++= '-';

  switch (itype) {
  case INTERVAL_YEAR:
    pos+= snprintf(pos, end - pos, "%llu",
                   (unsigned long long)iv->year);
    break;
  case INTERVAL_MONTH:
    pos+= snprintf(pos, end - pos, "%llu",
                   (unsigned long long)iv->month);
    break;
  case INTERVAL_YEAR_MONTH:
    pos+= snprintf(pos, end - pos, "%llu-%02llu",
                   (unsigned long long)iv->year,
                   (unsigned long long)iv->month);
    break;
  case INTERVAL_DAY:
    pos+= snprintf(pos, end - pos, "%llu",
                   (unsigned long long)iv->day);
    break;
  case INTERVAL_HOUR:
    pos+= snprintf(pos, end - pos, "%llu",
                   (unsigned long long)iv->hour);
    break;
  case INTERVAL_MINUTE:
    pos+= snprintf(pos, end - pos, "%llu",
                   (unsigned long long)iv->minute);
    break;
  case INTERVAL_SECOND:
      pos+= snprintf(pos, end - pos, "%llu",
                     (unsigned long long)iv->second);
    break;
  case INTERVAL_DAY_HOUR:
    pos+= snprintf(pos, end - pos, "%llu %02llu",
                   (unsigned long long)iv->day,
                   (unsigned long long)iv->hour);
    break;
  case INTERVAL_DAY_MINUTE:
    pos+= snprintf(pos, end - pos, "%llu %02llu:%02llu",
                   (unsigned long long)iv->day,
                   (unsigned long long)iv->hour,
                   (unsigned long long)iv->minute);
    break;
  case INTERVAL_DAY_SECOND:
      pos+= snprintf(pos, end - pos,
                     "%llu %02llu:%02llu:%02llu",
                     (unsigned long long)iv->day,
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    break;
  case INTERVAL_HOUR_MINUTE:
    pos+= snprintf(pos, end - pos, "%llu:%02llu",
                   (unsigned long long)iv->hour,
                   (unsigned long long)iv->minute);
    break;
  case INTERVAL_HOUR_SECOND:
      pos+= snprintf(pos, end - pos, "%llu:%02llu:%02llu",
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    break;
  case INTERVAL_MINUTE_SECOND:
      pos+= snprintf(pos, end - pos, "%llu:%02llu",
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    break;
  default:
    pos+= snprintf(pos, end - pos,
                   "%llu-%02llu-%llu %02llu:%02llu:%02llu.%06llu",
                   (unsigned long long)iv->year,
                   (unsigned long long)iv->month,
                   (unsigned long long)iv->day,
                   (unsigned long long)iv->hour,
                   (unsigned long long)iv->minute,
                   (unsigned long long)iv->second,
                   (unsigned long long)iv->second_part);
  }

  return pos - buf;
}

void timeval_to_interval(const my_timeval &tm, Interval *iv,
                         enum interval_type itype)
{
  *iv= Interval();

  if (tm.tv_sec == 0 && tm.tv_usec == 0)
    return;



  longlong seconds= tm.tv_sec;

  switch (itype) {
  case INTERVAL_YEAR:
  case INTERVAL_MONTH:
  case INTERVAL_YEAR_MONTH: {
    longlong days= seconds / 86400LL;

    if (itype == INTERVAL_YEAR)
      iv->year= static_cast<ulong>(days / 365);
    else if (itype == INTERVAL_MONTH)
      iv->month= static_cast<ulong>(days / 30);
    else if (itype == INTERVAL_YEAR_MONTH)
    {
      iv->year= static_cast<ulong>(days / 365);
      iv->month= static_cast<ulong>((days % 365) / 30);
    }
    break;
  }
  case INTERVAL_DAY:
    iv->day= static_cast<ulong>(seconds / 86400LL);
    break;
  case INTERVAL_HOUR:
    iv->hour= static_cast<ulong>(seconds / 3600LL);
    break;
  case INTERVAL_MINUTE:
    iv->minute= static_cast<ulong>(seconds / 60LL);
    break;
  case INTERVAL_SECOND:
    iv->second= static_cast<ulonglong>(seconds);
    break;
  case INTERVAL_DAY_HOUR:
    iv->day= static_cast<ulong>(seconds / 86400LL);
    seconds%= 86400LL;
    iv->hour= static_cast<ulong>(seconds / 3600LL);
    break;
  case INTERVAL_DAY_MINUTE:
    iv->day= static_cast<ulong>(seconds / 86400LL);
    seconds%= 86400LL;
    iv->hour= static_cast<ulong>(seconds / 3600LL);
    seconds%= 3600LL;
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    break;
  case INTERVAL_DAY_SECOND:
    iv->day= static_cast<ulong>(seconds / 86400LL);
    seconds%= 86400LL;
    iv->hour= static_cast<ulong>(seconds / 3600LL);
    seconds%= 3600LL;
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    seconds%= 60LL;
    iv->second= static_cast<ulonglong>(seconds);
    break;
  case INTERVAL_HOUR_MINUTE:
    iv->hour= static_cast<ulong>(seconds / 3600LL);
    seconds%= 3600LL;
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    break;
  case INTERVAL_HOUR_SECOND:
    iv->hour= static_cast<ulong>(seconds / 3600LL);
    seconds%= 3600LL;
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    seconds%= 60LL;
    iv->second= static_cast<ulonglong>(seconds);
    break;
  case INTERVAL_MINUTE_SECOND:
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    seconds%= 60LL;
    iv->second= static_cast<ulonglong>(seconds);
    break;
  default:
    break;
  }
}

uint calc_interval_display_width(interval_type itype,
                                 uint leading_precision,
                                 uint trailing_precision)
{
  if (itype >= INTERVAL_LAST)
    return 0;

  uint width= INTERVAL_MAX_WIDTH[itype] + leading_precision + trailing_precision;

  return width;
}

uint8 interval_default_length(enum interval_type type) {
  /*
   * For single interval types, the return value indicates the default number of digits accepted.
   * For range interval types, the return value is an 8-bit value that combines two 4-bit numbers:
   *   - The low 4 bits represent the default number of digits for the starting timestamp.
   *   - The high 4 bits represent the default number of digits for the ending timestamp.
   */
  switch(type) {
  case INTERVAL_YEAR:
    return 4;
  case INTERVAL_YEAR_MONTH:
    return 100; // 6 (month) | 4 (year)
  case INTERVAL_MONTH:
    return 6;
  case INTERVAL_DAY:
    return 7;
  case INTERVAL_DAY_HOUR:
    return 135; // 8 (hour) | 7 (day)
  case INTERVAL_DAY_MINUTE:
    return 167; // 10 (minute) | 7 (day)
  case INTERVAL_DAY_SECOND:
    return 199;  // 12 (second) | 7 (day)
  case INTERVAL_HOUR:
    return 8;
  case INTERVAL_HOUR_MINUTE:
    return 168; // 10 (minute) | 2 (hour)
  case INTERVAL_HOUR_SECOND:
    return 200; // 12 (second) | 2 (hour)
  case INTERVAL_MINUTE:
    return 10;
  case INTERVAL_MINUTE_SECOND:
    return 202; // 12 (second) | 10 (minute)
  case INTERVAL_SECOND:
    return 12;

  default:
    return 0;
  }
}
