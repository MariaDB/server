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
#include "structs.h"
#include "mysql/plugin.h"
#include "sql_type.h"

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
  str_to_interval(tmp.str, tmp.length, this, itype, start_prec, end_prec);
  if (!is_valid_interval(itype, start_prec, end_prec, this))
    my_error(ER_INVALID_DEFAULT_PARAM, MYF(0));
}

Interval::Interval(const Sec6 &sec6, enum interval_type itype, uint8 start_prec, uint8 end_prec)
{
  Sec6_to_interval(sec6, this, itype, start_prec, end_prec);
  if (!is_valid_interval(itype, start_prec, end_prec, this))
    my_error(ER_INVALID_DEFAULT_PARAM, MYF(0));
}

Interval::Interval(const Longlong_hybrid nr,
         enum interval_type itype,
         uint8 start_prec,
         uint8 end_prec)
    : Interval(Sec6(nr), itype, start_prec, end_prec) {}

Interval::Interval(double nr,
         enum interval_type itype,
         uint8 start_prec,
         uint8 end_prec)
    : Interval(Sec6(nr), itype, start_prec, end_prec) {}

Interval::Interval(const my_decimal *d,
         enum interval_type itype,
         uint8 start_prec,
         uint8 end_prec)
    : Interval(Sec6(d), itype, start_prec, end_prec) {}
Interval::Interval(THD *thd, Item *item) {  }
Interval::Interval(Interval_native val) {  }
Interval::Interval(Native *val) {  }

int Interval::cmp(const Interval &other) const
{
  my_timeval tm1= this->to_TIMEVAL(), tm2= other.to_TIMEVAL();
  return tm1.tv_sec < tm2.tv_sec ? -1 : tm1.tv_sec > tm2.tv_sec ? 1 : tm1.tv_usec < tm2.tv_usec ? -1 : tm1.tv_usec > tm2.tv_usec ? 1 : 0;
}

my_timeval Interval::to_TIMEVAL() const
{
  my_timeval tm={};
  interval_to_timeval(this, &tm, current_thd);
  return tm;
}

bool Interval::to_bool() const
{
  return (bool)is_valid_interval(m_interval_type, start_prec, end_prec, this);
}

longlong Interval::to_longlong() const
{
  my_timeval tm= to_TIMEVAL();
  return tm.tv_sec;
}

double Interval::to_double() const
{
  my_timeval tm= to_TIMEVAL();
  double d= tm.tv_sec + (tm.tv_usec / INTERVAL_FRAC_MAX_FACTOR);
  return d;
}

String *Interval::to_string(String *str, uint dec) const
{
  uint field_length= calc_interval_display_width(m_interval_type, start_prec, end_prec);
  str->alloc( field_length+ 1);
  char *buf= (char *)str->ptr();

  size_t len= interval_to_string(this, m_interval_type, buf, end_prec);
  str->length(len);
  str->set_charset(&my_charset_numeric);
  return str;
}


my_decimal *Interval::to_decimal(my_decimal *dec) const
{
  my_timeval tm= to_TIMEVAL();
  my_decimal d;
  return seconds2my_decimal((tm.tv_sec < 0), tm.tv_sec,
                           (ulong)tm.tv_usec, &d);
}

bool Interval::to_native(Native *to, uint decimals) const
{
  my_timeval tm= to_TIMEVAL();
  uint len= my_interval_binary_length(decimals);
  my_interval_to_binary(&tm, (uchar *) to->ptr(), decimals);
  to->length(len);
  return false;
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
  case INTERVAL_MONTH:
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

    if (i == num_components - 1)
      {
      if (p < end && *p == '.')
      {
        if (end_prec == 0)
          return 1;

        p++;

        if (p >= end || !my_isdigit(&my_charset_latin1, *p))
          return 1;

        ulong frac = 0;
        int total_digits = 0;
        int stored_digits = 0;

        while (p < end && my_isdigit(&my_charset_latin1, *p))
        {
          total_digits++;
          if (stored_digits < 6)
          {
            frac = frac * 10 + (*p - '0');
            stored_digits++;
          }
          p++;
        }

        while (stored_digits < 6)
        {
          frac *= 10;
          stored_digits++;
        }

        to->second_part = frac;
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
  to->round(current_thd, end_prec, to->default_round_mode(current_thd));

  return 0;
}

int Sec6_to_interval(const Sec6 &sec6, Interval *to,
                    enum interval_type itype, uint8 start_prec, uint8 end_prec) {
    to->year = to->month = to->day = to->hour = to->minute = to->second = to->second_part = 0;
    to->neg = sec6.neg();
    ulonglong value = sec6.sec();

    switch (itype) {
    case INTERVAL_YEAR: {
      to->year = value;
      break;
    }
      case INTERVAL_MONTH: {
      to->month = value;
      break;
    }
      case INTERVAL_DAY: {
      to->day = value;
      break;
      }
      case INTERVAL_HOUR: {
      to->hour = value;
      break;
    }
      case INTERVAL_MINUTE:
    {
      to->minute = value;
      break;
    }
      case INTERVAL_SECOND:
      {
      to->second = value;
      to->second_part = sec6.usec();
      break;
      }
        case INTERVAL_YEAR_MONTH:
      {
            to->month = value % 100;
            to->year = value / 100;
            break;
        }
        case INTERVAL_DAY_HOUR:
      {
            to->hour = value % 100;
            to->day = value / 100;
            break;
        }
        case INTERVAL_DAY_MINUTE:
      {
            to->minute = value % 100;
            value /= 100;
            to->hour = value % 100;
            value /= 100;
            to->day = value;
            break;
        }
        case INTERVAL_DAY_SECOND:
      {
            to->second = value % 100;
            value /= 100;
            to->minute = value % 100;
            value /= 100;
            to->hour = value % 100;
            value /= 100;
            to->day = value;
            to->second_part = sec6.usec();
            break;
        }
        case INTERVAL_HOUR_MINUTE: {
            to->minute = value % 100;
            to->hour = value / 100;
            break;
        }
        case INTERVAL_HOUR_SECOND: {
            to->second = value % 100;
            value /= 100;
            to->minute = value % 100;
            value /= 100;
            to->hour = value;
            to->second_part = sec6.usec();

            break;
        }
        case INTERVAL_MINUTE_SECOND: {
            to->second = value % 100;
            to->minute = value / 100;
            to->second_part = sec6.usec();

            break;
        }
        default:
            return 1;
    }
   to->round(current_thd, end_prec, to->default_round_mode(current_thd));

    return 0;
}

static uint8_t count_digits(ulong value)
{
  int digits= 0;
  while (value)
  {
    value/= 10;
    digits++;
  }
  return digits;
}

bool is_valid_interval(interval_type itype,
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
        return ival->second_part <= INTERVAL_FRAC_MAX;

    case INTERVAL_YEAR_MONTH:
        return count_digits(ival->year) <= start_prec &&
               ival->month <= INTERVAL_MONTH_MAX;

    case INTERVAL_DAY_HOUR:
        return count_digits(ival->day) <= start_prec &&
               ival->hour <= INTERVAL_HOUR_MAX;

    case INTERVAL_HOUR_MINUTE:
        return count_digits(ival->hour) <= start_prec &&
               ival->minute <= INTERVAL_MINUTE_MAX;

    case INTERVAL_MINUTE_SECOND:
        return count_digits(ival->minute) <= start_prec &&
               ival->second <= INTERVAL_SECOND_MAX;

    case INTERVAL_DAY_MINUTE:
        return count_digits(ival->day) <= start_prec &&
               ival->hour <= INTERVAL_HOUR_MAX &&
               ival->minute <= INTERVAL_MINUTE_MAX;

    case INTERVAL_DAY_SECOND:
        return count_digits(ival->day) <= start_prec &&
               ival->hour <= INTERVAL_HOUR_MAX &&
               ival->minute <= INTERVAL_MINUTE_MAX &&
               ival->second <= INTERVAL_SECOND_MAX;


    case INTERVAL_HOUR_SECOND:
        return count_digits(ival->hour) <= start_prec &&
               ival->minute <= INTERVAL_MINUTE_MAX &&
               ival->second <= INTERVAL_SECOND_MAX;


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
    if (iv->second_part) {
      pos += snprintf(pos, end - pos, "%llu.%llu",
                     (unsigned long long)iv->second,
                     (unsigned long long)iv->second_part);
    } else {
      pos += snprintf(pos, end - pos, "%llu",
                     (unsigned long long)iv->second);
    }
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
    if (iv->second_part) {
      pos += snprintf(pos, end - pos,
                     "%llu %02llu:%02llu:%02llu.%06llu",
                     (unsigned long long)iv->day,
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second,
                     (unsigned long long)iv->second_part);
    } else {
      pos += snprintf(pos, end - pos,
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
    if (iv->second_part) {
      pos += snprintf(pos, end - pos, "%llu:%02llu:%02llu.%06llu",
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second,
                     (unsigned long long)iv->second_part);
    } else {
      pos += snprintf(pos, end - pos, "%llu:%02llu:%02llu",
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    }
    break;
  case INTERVAL_MINUTE_SECOND:
    if (iv->second_part) {
      pos += snprintf(pos, end - pos, "%llu:%02llu.%06llu",
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second,
                     (unsigned long long)iv->second_part);
    } else {
      pos += snprintf(pos, end - pos, "%llu:%02llu",
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    }
    break;
  default:
    if (iv->second_part) {
      pos += snprintf(pos, end - pos,
                     "%llu-%02llu-%llu %02llu:%02llu:%02llu.%06llu",
                     (unsigned long long)iv->year,
                     (unsigned long long)iv->month,
                     (unsigned long long)iv->day,
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second,
                     (unsigned long long)iv->second_part);
    } else {
      pos += snprintf(pos, end - pos,
                     "%llu-%02llu-%llu %02llu:%02llu:%02llu",
                     (unsigned long long)iv->year,
                     (unsigned long long)iv->month,
                     (unsigned long long)iv->day,
                     (unsigned long long)iv->hour,
                     (unsigned long long)iv->minute,
                     (unsigned long long)iv->second);
    }
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
  ulong microseconds= tm.tv_usec;

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
    iv->second_part= static_cast<ulonglong>(microseconds);
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
    iv->second_part= static_cast<ulonglong>(microseconds);
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
    iv->second_part= static_cast<ulonglong>(microseconds);
    break;
  case INTERVAL_MINUTE_SECOND:
    iv->minute= static_cast<ulonglong>(seconds / 60LL);
    seconds%= 60LL;
    iv->second= static_cast<ulonglong>(seconds);
    iv->second_part= static_cast<ulonglong>(microseconds);
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

  uint width= INTERVAL_MAX_WIDTH[itype] + leading_precision + /*trailing_precision*/ INTERVAL_FRAC_DIGITS + (trailing_precision > 0);

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
    return INTERVAL_YEAR_DIGITS;
  case INTERVAL_YEAR_MONTH:
    return (INTERVAL_MONTH_DIGITS << 4) | INTERVAL_YEAR_DIGITS;
  case INTERVAL_MONTH:
    return INTERVAL_MONTH_DIGITS;
  case INTERVAL_DAY:
    return INTERVAL_DAY_DIGITS;
  case INTERVAL_DAY_HOUR:
    return (INTERVAL_HOUR_DIGITS << 4) | INTERVAL_DAY_DIGITS;
  case INTERVAL_DAY_MINUTE:
    return (INTERVAL_MINUTE_DIGITS << 4) | INTERVAL_DAY_DIGITS;
  case INTERVAL_DAY_SECOND:
    return (INTERVAL_FRAC_DIGITS << 4) | INTERVAL_DAY_DIGITS;
  case INTERVAL_HOUR:
    return INTERVAL_HOUR_DIGITS;
  case INTERVAL_HOUR_MINUTE:
    return (INTERVAL_MINUTE_DIGITS << 4) | INTERVAL_HOUR_DIGITS;
  case INTERVAL_HOUR_SECOND:
    return (INTERVAL_FRAC_DIGITS << 4) | INTERVAL_HOUR_DIGITS;
  case INTERVAL_MINUTE:
    return INTERVAL_MINUTE_DIGITS;
  case INTERVAL_MINUTE_SECOND:
    return (INTERVAL_FRAC_DIGITS << 4) | INTERVAL_MINUTE_DIGITS;
  case INTERVAL_SECOND:
    return INTERVAL_FRAC_DIGITS;
  default:
    return 0;
  }
}
