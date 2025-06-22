/* Copyright (C) 2025 Google Summer of Code 2025

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
                   enum interval_type itype, bool allow_fractional)
{
  if (str_to_interval(str, length, this, itype, allow_fractional) != 0)
    throw std::invalid_argument("Invalid interval format");
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
                    enum interval_type itype, bool allow_fractional)
{
  const char *p= str;
  const char *end= str + length;
  ulong values[6]= {0};
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

    if (i == num_components - 1 &&
        itype != INTERVAL_YEAR && itype != INTERVAL_MONTH &&
        allow_fractional && p < end && *p == '.')
    {
      p++;
      uint digits= 0;

      while (p < end && my_isdigit(&my_charset_latin1, *p) && digits < 6)
      {
        to->second_part= to->second_part * 10 + (*p - '0');
        digits++;
        p++;
      }

      while (digits < 6)
      {
        to->second_part*= 10;
        digits++;
      }
    }
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

  return (parsed_components > 0) ? 0 : 1;
}

int interval_to_timeval(const Interval *iv, my_timeval *tm, uint dec, THD *thd)
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

  if (dec < 6)
  {
    uint factor= log_10_int[6 - dec];
    uint remainder= microseconds % factor;

    if (remainder)
    {
      time_round_mode_t mode= Datetime::default_round_mode(thd);
      uint half= factor / 2;

      if (mode == TIME_FRAC_ROUND)
      {
        if (remainder >= half)
          microseconds+= factor - remainder;
        else
          microseconds-= remainder;
      }
      else
        microseconds-= remainder;

      if (microseconds >= 1000000)
      {
        microseconds-= 1000000;
        total_seconds+= 1;
      }
    }
  }

  tm->tv_sec= total_seconds;
  tm->tv_usec= microseconds;
  return 0;
}

size_t interval_to_string(const Interval *iv, enum interval_type itype,
                          uint decimals, char *buf, size_t buf_len)
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
    if (decimals)
    {
      pos+= snprintf(pos, end - pos, "%llu.%0*llu",
                     (unsigned long long)iv->second, decimals,
                     (unsigned long long)iv->second_part);
    }
    else
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
    if (decimals)
    {
      pos+= snprintf(pos, end - pos,
                     "%llu %02llu:%02llu:%02llu.%0*llu",
                     (unsigned long long)iv->day,
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second, decimals,
                     (unsigned long long)iv->second_part);
    }
    else
    {
      pos+= snprintf(pos, end - pos,
                     "%llu %02llu:%02llu:%02llu",
                     (unsigned long long)iv->day,
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    }
    break;
  case INTERVAL_HOUR_MINUTE:
    pos+= snprintf(pos, end - pos, "%llu:%02llu",
                   (unsigned long long)iv->hour,
                   (unsigned long long)iv->minute);
    break;
  case INTERVAL_HOUR_SECOND:
    if (decimals)
    {
      pos+= snprintf(pos, end - pos,
                     "%llu:%02llu:%02llu.%0*llu",
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second, decimals,
                     (unsigned long long)iv->second_part);
    }
    else
    {
      pos+= snprintf(pos, end - pos, "%llu:%02llu:%02llu",
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    }
    break;
  case INTERVAL_MINUTE_SECOND:
    if (decimals)
    {
      pos+= snprintf(pos, end - pos, "%llu:%02llu.%0*llu",
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second, decimals,
                     (unsigned long long)iv->second_part);
    }
    else
    {
      pos+= snprintf(pos, end - pos, "%llu:%02llu",
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    }
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
                         enum interval_type itype, decimal_digits_t dec)
{
  *iv= Interval();

  if (tm.tv_sec == 0 && tm.tv_usec == 0)
    return;

  longlong total_microseconds= tm.tv_sec;
  total_microseconds*= 1000000LL;
  total_microseconds+= static_cast<longlong>(tm.tv_usec);

  if (total_microseconds < 0)
  {
    total_microseconds= -total_microseconds;
    iv->neg= true;
  }

  if (dec < 6)
  {
    longlong factor= 1;
    for (int i= 0; i < 6 - dec; ++i)
      factor*= 10;
    total_microseconds= (total_microseconds + factor / 2) / factor * factor;
  }

  longlong seconds= total_microseconds / 1000000LL;
  longlong microsecond_part= total_microseconds % 1000000LL;

  switch (itype) {
  case INTERVAL_YEAR:
  case INTERVAL_MONTH:
  case INTERVAL_YEAR_MONTH: {
    longlong days= seconds / 86400LL;
    longlong months= days / 30;  // Approximate 1 month = 30 days

    if (itype == INTERVAL_YEAR)
      iv->year= static_cast<ulong>(months / 12);
    else if (itype == INTERVAL_MONTH)
      iv->month= static_cast<ulong>(months);
    else if (itype == INTERVAL_YEAR_MONTH)
    {
      iv->year= static_cast<ulong>(months / 12);
      iv->month= static_cast<ulong>(months % 12);
    }
    break;
  }
  case INTERVAL_WEEK:
    iv->day= static_cast<ulong>(seconds / (86400LL * 7));
    break;
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
    iv->second_part= static_cast<ulonglong>(microsecond_part);
    break;
  case INTERVAL_MICROSECOND:
    iv->second= static_cast<ulonglong>(seconds);
    iv->second_part= static_cast<ulonglong>(microsecond_part);
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
    iv->second_part= static_cast<ulonglong>(microsecond_part);
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
    iv->second_part= static_cast<ulonglong>(microsecond_part);
    break;
  case INTERVAL_MINUTE_SECOND:
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    seconds%= 60LL;
    iv->second= static_cast<ulonglong>(seconds);
    iv->second_part= static_cast<ulonglong>(microsecond_part);
    break;
  default:
    break;
  }
}

uint calc_interval_display_width(interval_type itype,
                                 uint leading_precision,
                                 uint fractional_precision)
{
  if (itype >= INTERVAL_LAST)
    return 0;

  uint width= INTERVAL_MAX_WIDTH[itype] + leading_precision;

  if (fractional_precision > 0)
  {
    width += 1 + fractional_precision;
  }

  return width;
}