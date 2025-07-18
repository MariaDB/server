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

constexpr uint8_t INTERVAL_MAX_WIDTH[INTERVAL_LAST]= {
  /* YEAR                  */ 1,  // [-]Y
  /* QUARTER               */ 1,  // [-]Q (not supported)
  /* MONTH                 */ 1,  // [-]M
  /* WEEK                  */ 1,  // [-]W (not supported)
  /* DAY                   */ 1,  // [-]D
  /* HOUR                  */ 1,  // [-]H
  /* MINUTE                */ 1,  // [-]M
  /* SECOND                */ 1,  // [-]S
  /* MICROSECOND           */ 1,  // [-]U (not supported)
  /* YEAR_MONTH            */ 2,  // [-]Y-MM
  /* DAY_HOUR              */ 2,  // [-]D HH
  /* DAY_MINUTE            */ 5,  // [-]D HH:MM
  /* DAY_SECOND            */ 8, // [-]D HH:MM:SS
  /* HOUR_MINUTE           */ 2,  // [-]H:MM
  /* HOUR_SECOND           */ 5,  // [-]H:MM:SS
  /* MINUTE_SECOND         */ 2,  // [-]M:SS
  /* DAY_MICROSECOND       */ 10, // (not supported)
  /* HOUR_MICROSECOND      */ 7,  // (not supported)
  /* MINUTE_MICROSECOND    */ 4,  // (not supported)
  /* SECOND_MICROSECOND    */ 1   // [-]S (not supported)
};


uint calc_interval_display_width(interval_type itype,
                                        uint leading_precision,
                                        uint fractional_precision);

class Interval : public INTERVAL {
public:
  Interval();
  Interval(const char *str, size_t length,
                     enum interval_type itype, CHARSET_INFO *cs, uint8 start_prec, uint8 end_prec);
};

int str_to_interval(const char *str, size_t length, Interval *to,
                    enum interval_type itype, uint8 start_prec, uint8 end_prec);

uint8_t is_valid_interval(interval_type itype,
                          uint8_t start_prec,
                          uint8_t end_prec,
                          const Interval *ival);

int interval_to_timeval(const Interval* iv, my_timeval* tm, THD *thd);

void timeval_to_interval(const my_timeval &tm, Interval *iv, enum interval_type itype);

size_t interval_to_string(const Interval *iv, enum interval_type itype,
                              char *buf, size_t buf_len);

uint8 interval_default_length(enum interval_type type);
#endif  // SQL_INTERVAL_H