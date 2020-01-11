/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2015 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef MRN_TIME_CONVERTER_HPP_
#define MRN_TIME_CONVERTER_HPP_

#include <mrn_mysql_compat.h>

#include <groonga.h>

namespace mrn {
  class TimeConverter {
  public:
    static const long long int TM_YEAR_BASE = 1900;

    TimeConverter();
    ~TimeConverter();

    long long int mysql_time_to_grn_time(MYSQL_TIME *mysql_time,
                                         bool *truncated);
    long long int mysql_datetime_to_grn_time(long long int mysql_datetime,
                                             bool *truncated);

    long long int tm_to_grn_time(struct tm *time, int usec, bool *truncated);

    void grn_time_to_mysql_time(long long int grn_time, MYSQL_TIME *mysql_time);
    long long int grn_time_to_mysql_datetime(long long int grn_time);

  private:
    time_t tm_to_time_gm(struct tm *time, bool *truncated);
  };
}

#endif /* MRN_TIME_CONVERTER_HPP_ */
