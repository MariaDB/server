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

#ifndef SQL_INTERVAL_H
#define SQL_INTERVAL_H

#include "structs.h"
#include "sql_type.h"

#define INTERVAL_YEAR_DIGITS    4   // Max: 9999 (4 digits)
#define INTERVAL_MONTH_DIGITS   6   // Max: 119988 months (9999*12)
#define INTERVAL_DAY_DIGITS     7   // Max: 3719628 days (9999*12*31)
#define INTERVAL_HOUR_DIGITS    8   // Max: 89271072 hours
#define INTERVAL_MINUTE_DIGITS 10   // Max: 5266993248 minutes
#define INTERVAL_SECOND_DIGITS 12   // Max: 310752601632 seconds
#define INTERVAL_FRAC_DIGITS    6   // Max: 999999 microseconds

/* Maximum values for interval components */
#define INTERVAL_MONTH_MAX   11   /* 0-11 months */
#define INTERVAL_HOUR_MAX    23   /* 0-23 hours */
#define INTERVAL_MINUTE_MAX  59   /* 0-59 minutes */
#define INTERVAL_SECOND_MAX  59   /* 0-59 seconds */
#define INTERVAL_FRAC_MAX    999999 /* 0-999999 microseconds */
#define INTERVAL_FRAC_MAX_FACTOR (INTERVAL_FRAC_MAX + 1)

#define MAX_INTERVAL_STRING_REP_LENGTH 30

constexpr uint8_t INTERVAL_MAX_WIDTH[INTERVAL_LAST]= {
  /* YEAR                  */ 1,  // [-]Y
  /* QUARTER               */ 1,  // [-]Q (not supported)
  /* MONTH                 */ 1,  // [-]M
  /* WEEK                  */ 1,  // [-]W (not supported)
  /* DAY                   */ 1,  // [-]D
  /* HOUR                  */ 1,  // [-]H
  /* MINUTE                */ 1,  // [-]M
  /* SECOND                */ 2,  // [-]S.ffffff
  /* MICROSECOND           */ 1,  // [-]U (not supported)
  /* YEAR_MONTH            */ 4,  // [-]Y-MM
  /* DAY_HOUR              */ 4,  // [-]D HH
  /* DAY_MINUTE            */ 7,  // [-]D HH:MM
  /* DAY_SECOND            */ 10, // [-]D HH:MM:SS.ffffff
  /* HOUR_MINUTE           */ 4,  // [-]H:MM
  /* HOUR_SECOND           */ 7,  // [-]H:MM:SS.ffffff
  /* MINUTE_SECOND         */ 4,  // [-]M:SS.ffffff
  /* DAY_MICROSECOND       */ 10, // (not supported)
  /* HOUR_MICROSECOND      */ 7,  // (not supported)
  /* MINUTE_MICROSECOND    */ 4,  // (not supported)
  /* SECOND_MICROSECOND    */ 1   // [-]S (not supported)
};

const LEX_CSTRING interval_type_names[INTERVAL_LAST] = {
  {STRING_WITH_LEN("YEAR")},
  {STRING_WITH_LEN("QUARTER")},
  {STRING_WITH_LEN("MONTH")},
  {STRING_WITH_LEN("WEEK")},
  {STRING_WITH_LEN("DAY")},
  {STRING_WITH_LEN("HOUR")},
  {STRING_WITH_LEN("MINUTE")},
  {STRING_WITH_LEN("SECOND")},
  {STRING_WITH_LEN("MICROSECOND")},
  {STRING_WITH_LEN("YEAR TO MONTH")},
  {STRING_WITH_LEN("DAY TO HOUR")},
  {STRING_WITH_LEN("DAY TO MINUTE")},
  {STRING_WITH_LEN("DAY TO SECOND")},
  {STRING_WITH_LEN("HOUR TO MINUTE")},
  {STRING_WITH_LEN("HOUR TO SECOND")},
  {STRING_WITH_LEN("MINUTE TO SECOND")},
  {STRING_WITH_LEN("DAY TO MICROSECOND")},
  {STRING_WITH_LEN("HOUR TO MICROSECOND")},
  {STRING_WITH_LEN("MINUTE TO MICROSECOND")},
  {STRING_WITH_LEN("SECOND TO MICROSECOND")}
};

class Interval;
class Sec6;
class Temporal;
class Native;
uint calc_interval_display_width(interval_type itype,
                                        uint leading_precision,
                                        uint fractional_precision);
class Interval_native;
class Interval :public INTERVAL
{
public:
  enum interval_type m_interval_type;
  uint8 start_prec;
  uint8 end_prec;
  Interval(const Interval& other);
  Interval& operator=(const Interval& other);  Interval();
  Interval(const char *str, size_t length,
                     enum interval_type itype, CHARSET_INFO *cs, uint8 start_prec, uint8 end_prec);
  Interval(const Sec6 &sec6,enum interval_type itype, uint8 start_prec, uint8 end_prec);
  Interval(const Longlong_hybrid nr,
           enum interval_type itype,
           uint8 start_prec,
           uint8 end_prec);
  Interval(double nr,
           enum interval_type itype,
           uint8 start_prec,
           uint8 end_prec);
  Interval(const my_decimal *d,
           enum interval_type itype,
           uint8 start_prec,
           uint8 end_prec);
  Interval(THD *thd, Item *item);

  time_round_mode_t default_round_mode(THD *thd);

  Interval& floor();

  Interval& ceil();

  Interval &round(THD *thd, uint dec, time_round_mode_t mode)
  {
    switch (mode.mode()) {
    case time_round_mode_t::FRAC_NONE:
      second_part = 0;
      break;
    case time_round_mode_t::FRAC_TRUNCATE:
      if (dec < 6) {
        ulonglong factor = 1;
        for (uint i = 0; i < 6 - dec; ++i) {
          factor *= 10;
        }
        second_part = second_part - (second_part % factor);
      }
      break;
    case time_round_mode_t::FRAC_ROUND:
      if (dec < 6) {
        ulonglong factor = 1;
        for (uint i = 0; i < 6 - dec; ++i) {
          factor *= 10;
        }
        ulonglong remainder = second_part % factor;
        ulonglong half = factor / 2;
        if (remainder < half) {
          second_part = second_part - remainder;
        } else {
          second_part = second_part - remainder + factor;
          if (second_part >= 1000000) {
            second_part -= 1000000;
            ++second;
          }
        }
      }
      break;
    }
    return *this;
  }

    bool to_bool() const;

    my_timeval to_TIMEVAL() const;

    longlong to_longlong() const;

    double to_double() const;

    String *to_string(String *str, uint dec, bool append_mode) const;

    my_decimal *to_decimal(my_decimal *dec) const;

    bool to_native(Native *to, uint decimals) const;

    Interval(Interval_native val);

    Interval(Native *val);

    int cmp(const Interval &other) const;

    void toggle_sign();

    void reset()
    {
      m_interval_type = INTERVAL_LAST;
      start_prec = 0;
      end_prec = 0;
      second_part = 0;
      second = 0;
      minute = 0;
      hour = 0;
      day = 0;
      month = 0;
      year = 0;
    }
};

int str_to_interval(const char *str, size_t length, Interval *to,
                    enum interval_type itype, uint8 start_prec, uint8 end_prec);

int Sec6_to_interval(const Sec6 &sec6, Interval *to,
                    enum interval_type itype, uint8 start_prec, uint8 end_prec);

bool is_valid_interval(interval_type itype,
                          uint8_t start_prec,
                          uint8_t end_prec,
                          const Interval *ival);

int interval_to_timeval(const Interval* iv, my_timeval* tm, THD *thd);

void timeval_to_interval(const my_timeval &tm, Interval *iv, enum interval_type itype);

size_t interval_to_string(const Interval *iv, enum interval_type itype,
                              char *buf, size_t buf_len);

uint8 interval_default_length(enum interval_type type);

void get_interval_default_precision(interval_type type, uint8 *default_start, uint8 *default_end);

void mysql_time_to_interval(const MYSQL_TIME &from,
                            bool subtract,
                            INTERVAL *to);
const Type_handler *interval_type_to_handler_type(interval_type type);


void add_intervals(const Interval *iv1, const Interval *iv2, Interval *out);

bool has_day(interval_type t);

bool has_hour(interval_type t);

bool has_minute(interval_type t);

bool has_second(interval_type t);

bool has_microsecond(interval_type t);

bool interval_multiply(const Interval *iv, double factor, Interval *result);
bool interval_divide(const Interval *iv, double divisor, Interval *result);

interval_type merge_intervals(interval_type a, interval_type b);


#endif  // SQL_INTERVAL_H