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
#include "my_decimal.h"


Interval::Interval(const Interval &other)
  : INTERVAL(other),
    m_interval_type(other.m_interval_type),
    start_prec(other.start_prec),
    end_prec(other.end_prec)
{}


Interval& Interval::operator=(const Interval &other)
{
  if (this != &other)
  {
    static_cast<INTERVAL&>(*this)= other;

    m_interval_type= other.m_interval_type;
    start_prec= other.start_prec;
    end_prec= other.end_prec;
  }
  return *this;
}


Interval::Interval()
  : m_interval_type(INTERVAL_LAST),
    start_prec(0),
    end_prec(0)
{
  year= month= day= hour= 0;
  minute= second= second_part= 0;
  neg= false;
}


Interval::Interval(const char *str, size_t length,
                  enum interval_type itype, CHARSET_INFO *cs, uint8 start_prec, uint8 end_prec)
    : m_interval_type(itype),
      start_prec(start_prec),
      end_prec(end_prec)
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

void Interval::toggle_sign()
{
  this->neg = !this->neg;
}

my_timeval Interval::to_TIMEVAL() const
{
  my_timeval tm={};
  interval_to_timeval(this, &tm, current_thd);
  return tm;
}

Interval& Interval::floor()
{
  second_part= 0;
  return *this;
}


Interval& Interval::ceil()
{
  if (second_part)
  {
    second_part= 0;
    Interval temp;
    temp.reset();
    temp.m_interval_type= INTERVAL_SECOND;
    temp.second= 1;
    temp.neg= neg;
    add_intervals(this, &temp, this);
  }
  return *this;
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

String *Interval::to_string(String *str, uint dec, bool append_mode) const
{
  uint field_length= calc_interval_display_width(m_interval_type, start_prec, end_prec);

  if (!append_mode)
  {
    str->alloc(field_length + 1);
    char *buf= (char *)str->ptr();
    size_t len= interval_to_string(this, m_interval_type, buf, field_length + 1);
    str->length(len);
  }
  else
  {
    size_t orig_len= str->length();
    str->realloc_with_extra(orig_len + field_length + 1);
    char *buf= (char *)str->ptr() + orig_len;
    size_t len= interval_to_string(this, m_interval_type, buf, field_length + 1);
    str->length(orig_len + len);
  }

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

  to->year= to->month= to->day= to->hour= to->minute= to->second= to->second_part= 0;
  to->neg= false;

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
  total_days+= static_cast<longlong>(iv->year) * 360;
  total_days+= static_cast<longlong>(iv->month) * 30;
  total_days+= static_cast<longlong>(iv->day);

  longlong total_seconds= total_days * 86400;
  total_seconds+= static_cast<longlong>(iv->hour) * 3600;
  total_seconds+= static_cast<longlong>(iv->minute) * 60;
  total_seconds+= static_cast<longlong>(iv->second);

  ulong microseconds= static_cast<ulong>(iv->second_part);

  tm->tv_sec= total_seconds;
  tm->tv_usec= microseconds;
  if (iv->neg)
  {
    tm->tv_sec= -total_seconds;
  }
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

  if (seconds < 0)
  {
    seconds= -seconds;
    iv->neg= true;
  }
  switch (itype) {
  case INTERVAL_YEAR:
  case INTERVAL_MONTH:
  case INTERVAL_YEAR_MONTH: {
    longlong days= seconds / 86400LL;

    if (itype == INTERVAL_YEAR)
      iv->year= static_cast<ulong>(days / 360);
    else if (itype == INTERVAL_MONTH)
      iv->month= static_cast<ulong>(days / 30);
    else if (itype == INTERVAL_YEAR_MONTH)
    {
      iv->year= static_cast<ulong>(days / 360);
      iv->month= static_cast<ulong>((days % 360) / 30);
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


void get_interval_default_precision(interval_type type,
                                   uint8 *default_start,
                                   uint8 *default_end)
{
  switch(type) {
  case INTERVAL_YEAR:
    *default_start= INTERVAL_YEAR_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_YEAR_MONTH:
    *default_start= INTERVAL_YEAR_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_MONTH:
    *default_start= INTERVAL_MONTH_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_DAY:
    *default_start= INTERVAL_DAY_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_DAY_HOUR:
    *default_start= INTERVAL_DAY_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_DAY_MINUTE:
    *default_start= INTERVAL_DAY_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_DAY_SECOND:
    *default_start= INTERVAL_DAY_DIGITS;
    *default_end= INTERVAL_FRAC_DIGITS;
    break;
  case INTERVAL_HOUR:
    *default_start= INTERVAL_HOUR_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_HOUR_MINUTE:
    *default_start= INTERVAL_HOUR_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_HOUR_SECOND:
    *default_start= INTERVAL_HOUR_DIGITS;
    *default_end= INTERVAL_FRAC_DIGITS;
    break;
  case INTERVAL_MINUTE:
    *default_start= INTERVAL_MINUTE_DIGITS;
    *default_end= 0;
    break;
  case INTERVAL_MINUTE_SECOND:
    *default_start= INTERVAL_MINUTE_DIGITS;
    *default_end= INTERVAL_FRAC_DIGITS;
    break;
  case INTERVAL_SECOND:
    *default_start= INTERVAL_SECOND_DIGITS;
    *default_end= INTERVAL_FRAC_DIGITS;
    break;
  default:
    *default_start= 0;
    *default_end= 0;
    break;
  }
}


void mysql_time_to_interval(const MYSQL_TIME &from,
                            bool subtract,
                            INTERVAL *to)
{
  to->year= from.year;
  to->month= from.month;
  to->day= from.day;
  to->hour= from.hour;
  to->minute= from.minute;
  to->second= from.second;
  to->second_part= from.second_part;
  to->neg= subtract ? !from.neg : from.neg;
}

const Type_handler *interval_type_to_handler_type(interval_type type)
{
  switch (type)
  {
  case INTERVAL_YEAR:
    return &type_handler_interval_year;
  case INTERVAL_QUARTER:
    return &type_handler_interval_quarter;
  case INTERVAL_MONTH:
    return &type_handler_interval_month;
  case INTERVAL_WEEK:
    return &type_handler_interval_week;
  case INTERVAL_DAY:
    return &type_handler_interval_day;
  case INTERVAL_HOUR:
    return &type_handler_interval_hour;
  case INTERVAL_MINUTE:
    return &type_handler_interval_minute;
  case INTERVAL_SECOND:
    return &type_handler_interval_second;
  case INTERVAL_MICROSECOND:
    return &type_handler_interval_microsecond;

  case INTERVAL_YEAR_MONTH:
    return &type_handler_interval_year_month;
  case INTERVAL_DAY_HOUR:
    return &type_handler_interval_day_hour;
  case INTERVAL_DAY_MINUTE:
    return &type_handler_interval_day_minute;
  case INTERVAL_DAY_SECOND:
    return &type_handler_interval_day_second;
  case INTERVAL_HOUR_MINUTE:
    return &type_handler_interval_hour_minute;
  case INTERVAL_HOUR_SECOND:
    return &type_handler_interval_hour_second;
  case INTERVAL_MINUTE_SECOND:
    return &type_handler_interval_minute_second;
  case INTERVAL_DAY_MICROSECOND:
    return &type_handler_interval_day_microsecond;
  case INTERVAL_HOUR_MICROSECOND:
    return &type_handler_interval_hour_microsecond;
  case INTERVAL_MINUTE_MICROSECOND:
    return &type_handler_interval_minute_microsecond;
  case INTERVAL_SECOND_MICROSECOND:
    return &type_handler_interval_second_microsecond;

  case INTERVAL_LAST:
  default:
    return &type_handler_interval_common;
  }
}


void add_intervals(const Interval *iv1, const Interval *iv2, Interval *out)
{
  interval_type merged_type= out->m_interval_type;

  longlong years= 0, months= 0;
  longlong days= 0, hours= 0, minutes= 0, seconds= 0, micros= 0;

  auto add_components= [](const Interval *iv, longlong &y, longlong &m,
                          longlong &d, longlong &h, longlong &min,
                          longlong &s, longlong &mic) {
    longlong sign= iv->neg ? -1 : 1;
    y+= sign * iv->year;
    m+= sign * iv->month;
    d+= sign * iv->day;
    h+= sign * iv->hour;
    min+= sign * iv->minute;
    s+= sign * iv->second;
    mic+= sign * iv->second_part;
  };

  add_components(iv1, years, months, days, hours, minutes, seconds, micros);
  add_components(iv2, years, months, days, hours, minutes, seconds, micros);

  longlong total_months= years * 12 + months;
  if (total_months < 0)
  {
    total_months= -total_months;
    out->neg= true;
  }
  longlong norm_years= total_months / 12;
  longlong norm_months= total_months % 12;

  constexpr longlong USECS_PER_DAY= 86400000000LL;
  constexpr longlong USECS_PER_HOUR= 3600000000LL;
  constexpr longlong USECS_PER_MINUTE= 60000000LL;
  constexpr longlong USECS_PER_SECOND= 1000000LL;

  longlong total_micros= days * USECS_PER_DAY +
                         hours * USECS_PER_HOUR +
                         minutes * USECS_PER_MINUTE +
                         seconds * USECS_PER_SECOND +
                         micros;

  if (total_micros < 0)
  {
    out->neg= true;
    total_micros= -total_micros;
  }

  switch (merged_type) {
  case INTERVAL_YEAR:
    out->year= norm_years;
    break;
  case INTERVAL_MONTH:
    out->month= total_months;
    break;
  case INTERVAL_YEAR_MONTH:
    out->year= norm_years;
    out->month= norm_months;
    break;
  default:
    if (has_day(merged_type))
    {
      out->day= total_micros / USECS_PER_DAY;
      total_micros %= USECS_PER_DAY;
    }
    if (has_hour(merged_type))
    {
      out->hour= total_micros / USECS_PER_HOUR;
      total_micros %= USECS_PER_HOUR;
    }
    if (has_minute(merged_type))
    {
      out->minute= total_micros / USECS_PER_MINUTE;
      total_micros %= USECS_PER_MINUTE;
    }
    if (has_second(merged_type))
    {
      out->second= total_micros / USECS_PER_SECOND;
      total_micros %= USECS_PER_SECOND;
      out->second_part= total_micros;
    }
    if (has_microsecond(merged_type))
    {
      out->second_part= total_micros;
    }
    break;
  }
}


bool has_day(interval_type t)
{
  return t == INTERVAL_DAY || t == INTERVAL_DAY_HOUR ||
         t == INTERVAL_DAY_MINUTE || t == INTERVAL_DAY_SECOND ||
         t == INTERVAL_DAY_MICROSECOND;
}


bool has_hour(interval_type t)
{
  return t == INTERVAL_HOUR || t == INTERVAL_DAY_HOUR ||
         t == INTERVAL_HOUR_MINUTE || t == INTERVAL_HOUR_SECOND ||
         t == INTERVAL_HOUR_MICROSECOND || t == INTERVAL_DAY_MINUTE ||
         t == INTERVAL_DAY_SECOND || t == INTERVAL_DAY_MICROSECOND;
}


bool has_minute(interval_type t)
{
  return t == INTERVAL_MINUTE || t == INTERVAL_MINUTE_SECOND ||
         t == INTERVAL_MINUTE_MICROSECOND || t == INTERVAL_DAY_MINUTE ||
         t == INTERVAL_HOUR_MINUTE || t == INTERVAL_DAY_SECOND ||
         t == INTERVAL_HOUR_SECOND || t == INTERVAL_DAY_MICROSECOND ||
         t == INTERVAL_HOUR_MICROSECOND;
}


bool has_second(interval_type t)
{
  return t == INTERVAL_SECOND || t == INTERVAL_DAY_SECOND ||
         t == INTERVAL_HOUR_SECOND || t == INTERVAL_MINUTE_SECOND ||
         t == INTERVAL_SECOND_MICROSECOND;
}


bool has_microsecond(interval_type t)
{
  return t == INTERVAL_MICROSECOND || t == INTERVAL_DAY_MICROSECOND ||
         t == INTERVAL_HOUR_MICROSECOND || t == INTERVAL_MINUTE_MICROSECOND ||
         t == INTERVAL_SECOND_MICROSECOND;
}


static longlong interval_to_usec(const Interval *iv)
{
  return (iv->day * 86400000000LL) +
         (iv->hour * 3600000000LL) +
         (iv->minute * 60000000LL) +
         (iv->second * 1000000LL) +
         iv->second_part;
}


static bool is_year_month_type(interval_type type)
{
  switch (type) {
  case INTERVAL_YEAR:
  case INTERVAL_QUARTER:
  case INTERVAL_MONTH:
  case INTERVAL_YEAR_MONTH:
    return true;
  default:
    return false;
  }
}


static void usec_to_interval(longlong usec, interval_type type, Interval *out)
{
  out->m_interval_type= type;
  out->neg= (usec < 0);
  if (out->neg)
    usec= -usec;

  out->year= out->month= out->day= 0;
  out->hour= out->minute= out->second= 0;
  out->second_part= 0;

  if (has_day(type))
  {
    out->day= usec / 86400000000LL;
    usec %= 86400000000LL;
  }

  if (has_hour(type))
  {
    out->hour= usec / 3600000000LL;
    usec %= 3600000000LL;
  }

  if (has_minute(type))
  {
    out->minute= usec / 60000000LL;
    usec %= 60000000LL;
  }

  if (has_second(type))
  {
    out->second= usec / 1000000LL;
    usec %= 1000000LL;
    out->second_part= usec;
  }

  if (has_microsecond(type))
  {
    out->second_part= usec;
  }
}


bool interval_multiply(const Interval *iv, double factor, Interval *result)
{
  if (is_year_month_type(iv->m_interval_type))
  {
    long total_months= iv->year * 12 + iv->month;
    if (iv->neg)
      total_months= -total_months;

    double res= static_cast<double>(total_months) * factor;
    longlong rounded= static_cast<longlong>(res + (res > 0 ? 0.5 : -0.5));

    if (llabs(rounded) > 120000LL)
      return true;

    result->m_interval_type= iv->m_interval_type;
    result->neg= (rounded < 0);
    if (result->neg)
      rounded= -rounded;

    result->year= static_cast<uint>(rounded / 12);
    result->month= static_cast<uint>(rounded % 12);
    result->day= result->hour= result->minute= 0;
    result->second= result->second_part= 0;
    return false;
  }

  longlong usec= interval_to_usec(iv);
  if (iv->neg)
    usec= -usec;

  double total= static_cast<double>(usec) * factor;
  longlong rounded= static_cast<longlong>(total + (total > 0 ? 0.5 : -0.5));

  usec_to_interval(rounded, iv->m_interval_type, result);
  return false;
}


bool interval_divide(const Interval *iv, double divisor, Interval *result)
{
  if (fabs(divisor) < 1e-20)
    return true;

  if (is_year_month_type(iv->m_interval_type))
  {
    long total_months= iv->year * 12 + iv->month;
    if (iv->neg)
      total_months= -total_months;

    double res= static_cast<double>(total_months) / divisor;
    longlong rounded= static_cast<longlong>(res + (res > 0 ? 0.5 : -0.5));

    if (llabs(rounded) > 120000LL)
      return true;

    result->m_interval_type= iv->m_interval_type;
    result->neg= (rounded < 0);
    if (result->neg)
      rounded= -rounded;

    result->year= static_cast<uint>(rounded / 12);
    result->month= static_cast<uint>(rounded % 12);
    result->day= result->hour= result->minute= 0;
    result->second= result->second_part= 0;
    return false;
  }

  longlong usec= interval_to_usec(iv);
  if (iv->neg)
    usec= -usec;

  double total= static_cast<double>(usec) / divisor;
  longlong rounded= static_cast<longlong>(total + (total > 0 ? 0.5 : -0.5));

  usec_to_interval(rounded, iv->m_interval_type, result);
  return false;
}


static interval_type merge_base_intervals(interval_type a, interval_type b)
{
  switch (a) {
  case INTERVAL_YEAR:
    switch (b) {
    case INTERVAL_MONTH:
      return INTERVAL_YEAR_MONTH;
    default:
      break;
    }
    break;

  case INTERVAL_DAY:
    switch (b) {
    case INTERVAL_HOUR:
      return INTERVAL_DAY_HOUR;
    case INTERVAL_MINUTE:
      return INTERVAL_DAY_MINUTE;
    case INTERVAL_SECOND:
      return INTERVAL_DAY_SECOND;
    case INTERVAL_MICROSECOND:
      return INTERVAL_DAY_MICROSECOND;
    default:
      break;
    }
    break;

  case INTERVAL_HOUR:
    switch (b) {
    case INTERVAL_MINUTE:
      return INTERVAL_HOUR_MINUTE;
    case INTERVAL_SECOND:
      return INTERVAL_HOUR_SECOND;
    case INTERVAL_MICROSECOND:
      return INTERVAL_HOUR_MICROSECOND;
    default:
      break;
    }
    break;

  case INTERVAL_MINUTE:
    switch (b) {
    case INTERVAL_SECOND:
      return INTERVAL_MINUTE_SECOND;
    case INTERVAL_MICROSECOND:
      return INTERVAL_MINUTE_MICROSECOND;
    default:
      break;
    }
    break;

  case INTERVAL_SECOND:
    switch (b) {
    case INTERVAL_MICROSECOND:
      return INTERVAL_SECOND_MICROSECOND;
    default:
      break;
    }
    break;

  default:
    break;
  }

  return INTERVAL_LAST;
}


static enum interval_type get_start_unit(enum interval_type type)
{
  switch (type) {
  case INTERVAL_YEAR:
  case INTERVAL_QUARTER:
  case INTERVAL_MONTH:
  case INTERVAL_WEEK:
  case INTERVAL_DAY:
  case INTERVAL_HOUR:
  case INTERVAL_MINUTE:
  case INTERVAL_SECOND:
  case INTERVAL_MICROSECOND:
    return type;

  case INTERVAL_YEAR_MONTH:
    return INTERVAL_YEAR;
  case INTERVAL_DAY_HOUR:
    return INTERVAL_DAY;
  case INTERVAL_DAY_MINUTE:
    return INTERVAL_DAY;
  case INTERVAL_DAY_SECOND:
    return INTERVAL_DAY;
  case INTERVAL_DAY_MICROSECOND:
    return INTERVAL_DAY;
  case INTERVAL_HOUR_MINUTE:
    return INTERVAL_HOUR;
  case INTERVAL_HOUR_SECOND:
    return INTERVAL_HOUR;
  case INTERVAL_HOUR_MICROSECOND:
    return INTERVAL_HOUR;
  case INTERVAL_MINUTE_SECOND:
    return INTERVAL_MINUTE;
  case INTERVAL_MINUTE_MICROSECOND:
    return INTERVAL_MINUTE;
  case INTERVAL_SECOND_MICROSECOND:
    return INTERVAL_SECOND;
  default:
    return INTERVAL_LAST;
  }
}


static enum interval_type get_end_unit(enum interval_type type)
{
  switch (type) {
  case INTERVAL_YEAR:
  case INTERVAL_QUARTER:
  case INTERVAL_MONTH:
  case INTERVAL_WEEK:
  case INTERVAL_DAY:
  case INTERVAL_HOUR:
  case INTERVAL_MINUTE:
  case INTERVAL_SECOND:
  case INTERVAL_MICROSECOND:
    return type;

  case INTERVAL_YEAR_MONTH:
    return INTERVAL_MONTH;
  case INTERVAL_DAY_HOUR:
    return INTERVAL_HOUR;
  case INTERVAL_DAY_MINUTE:
    return INTERVAL_MINUTE;
  case INTERVAL_DAY_SECOND:
    return INTERVAL_SECOND;
  case INTERVAL_DAY_MICROSECOND:
    return INTERVAL_MICROSECOND;
  case INTERVAL_HOUR_MINUTE:
    return INTERVAL_MINUTE;
  case INTERVAL_HOUR_SECOND:
    return INTERVAL_SECOND;
  case INTERVAL_HOUR_MICROSECOND:
    return INTERVAL_MICROSECOND;
  case INTERVAL_MINUTE_SECOND:
    return INTERVAL_SECOND;
  case INTERVAL_MINUTE_MICROSECOND:
    return INTERVAL_MICROSECOND;
  case INTERVAL_SECOND_MICROSECOND:
    return INTERVAL_MICROSECOND;
  default:
    return INTERVAL_LAST;
  }
}


interval_type merge_intervals(interval_type a, interval_type b)
{
  enum interval_type aa= std::min(get_start_unit(a), get_start_unit(b));
  enum interval_type bb= std::max(get_end_unit(a), get_end_unit(b));

  if (aa != bb)
    return merge_base_intervals(aa, bb);
  else
    return aa;
}