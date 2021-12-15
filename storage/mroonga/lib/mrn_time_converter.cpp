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

#include "mrn_time_converter.hpp"

#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

#include <limits>

// for debug
#define MRN_CLASS_NAME "mrn::TimeConverter"

namespace mrn {
  TimeConverter::TimeConverter() {
  }

  TimeConverter::~TimeConverter() {
  }

  time_t TimeConverter::tm_to_time_gm(struct tm *time, bool *truncated) {
    MRN_DBUG_ENTER_METHOD();
    *truncated = true;
    struct tm gmdate;
    time->tm_yday = -1;
    time->tm_isdst = -1;
    time_t sec_t = mktime(time);
    if (time->tm_yday == -1) {
      DBUG_RETURN(-1);
    }
    if (!gmtime_r(&sec_t, &gmdate)) {
      DBUG_RETURN(-1);
    }
    int32 mrn_utc_diff_in_seconds =
      (
        time->tm_mday > 25 && gmdate.tm_mday == 1 ? -1 :
        time->tm_mday == 1 && gmdate.tm_mday > 25 ? 1 :
        time->tm_mday - gmdate.tm_mday
        ) * 24 * 60 * 60 +
      (time->tm_hour - gmdate.tm_hour) * 60 * 60 +
      (time->tm_min - gmdate.tm_min) * 60 +
      (time->tm_sec - gmdate.tm_sec);
    DBUG_PRINT("info", ("mroonga: time->tm_year=%d", time->tm_year));
    DBUG_PRINT("info", ("mroonga: time->tm_mon=%d", time->tm_mon));
    DBUG_PRINT("info", ("mroonga: time->tm_mday=%d", time->tm_mday));
    DBUG_PRINT("info", ("mroonga: time->tm_hour=%d", time->tm_hour));
    DBUG_PRINT("info", ("mroonga: time->tm_min=%d", time->tm_min));
    DBUG_PRINT("info", ("mroonga: time->tm_sec=%d", time->tm_sec));
    DBUG_PRINT("info", ("mroonga: mrn_utc_diff_in_seconds=%d",
                        mrn_utc_diff_in_seconds));
    if (mrn_utc_diff_in_seconds > 0) {
      if (sec_t > std::numeric_limits<time_t>::max() - mrn_utc_diff_in_seconds) {
        DBUG_RETURN(-1);
      }
    } else {
      if (sec_t < std::numeric_limits<time_t>::min() - mrn_utc_diff_in_seconds) {
        DBUG_RETURN(-1);
      }
    }
    *truncated = false;
    DBUG_RETURN(sec_t + mrn_utc_diff_in_seconds);
  }

  long long int TimeConverter::tm_to_grn_time(struct tm *time, int usec,
                                              bool *truncated) {
    MRN_DBUG_ENTER_METHOD();

    long long int sec = tm_to_time_gm(time, truncated);

    DBUG_PRINT("info", ("mroonga: sec=%lld", sec));
    DBUG_PRINT("info", ("mroonga: usec=%d", usec));

    long long int grn_time = *truncated ? 0 : GRN_TIME_PACK(sec, usec);

    DBUG_RETURN(grn_time);
  }

  long long int TimeConverter::mysql_time_to_grn_time(MYSQL_TIME *mysql_time,
                                                      bool *truncated) {
    MRN_DBUG_ENTER_METHOD();

    int usec = mysql_time->second_part;
    long long int grn_time = 0;

    *truncated = false;
    switch (mysql_time->time_type) {
    case MYSQL_TIMESTAMP_DATE:
      {
        DBUG_PRINT("info", ("mroonga: MYSQL_TIMESTAMP_DATE"));
        struct tm date;
        memset(&date, 0, sizeof(struct tm));
        date.tm_year = mysql_time->year - TM_YEAR_BASE;
        if (mysql_time->month > 0) {
          date.tm_mon = mysql_time->month - 1;
        } else {
          date.tm_mon = 0;
          *truncated = true;
        }
        if (mysql_time->day > 0) {
          date.tm_mday = mysql_time->day;
        } else {
          date.tm_mday = 1;
          *truncated = true;
        }
        DBUG_PRINT("info", ("mroonga: tm_year=%d", date.tm_year));
        DBUG_PRINT("info", ("mroonga: tm_mon=%d", date.tm_mon));
        DBUG_PRINT("info", ("mroonga: tm_mday=%d", date.tm_mday));
        bool tm_truncated = false;
        grn_time = tm_to_grn_time(&date, usec, &tm_truncated);
        if (tm_truncated) {
          *truncated = true;
        }
      }
      break;
    case MYSQL_TIMESTAMP_DATETIME:
      {
        DBUG_PRINT("info", ("mroonga: MYSQL_TIMESTAMP_DATETIME"));
        struct tm datetime;
        memset(&datetime, 0, sizeof(struct tm));
        datetime.tm_year = mysql_time->year - TM_YEAR_BASE;
        if (mysql_time->month > 0) {
          datetime.tm_mon = mysql_time->month - 1;
        } else {
          datetime.tm_mon = 0;
          *truncated = true;
        }
        if (mysql_time->day > 0) {
          datetime.tm_mday = mysql_time->day;
        } else {
          datetime.tm_mday = 1;
          *truncated = true;
        }
        datetime.tm_hour = mysql_time->hour;
        datetime.tm_min = mysql_time->minute;
        datetime.tm_sec = mysql_time->second;
        DBUG_PRINT("info", ("mroonga: tm_year=%d", datetime.tm_year));
        DBUG_PRINT("info", ("mroonga: tm_mon=%d", datetime.tm_mon));
        DBUG_PRINT("info", ("mroonga: tm_mday=%d", datetime.tm_mday));
        DBUG_PRINT("info", ("mroonga: tm_hour=%d", datetime.tm_hour));
        DBUG_PRINT("info", ("mroonga: tm_min=%d", datetime.tm_min));
        DBUG_PRINT("info", ("mroonga: tm_sec=%d", datetime.tm_sec));
        bool tm_truncated = false;
        grn_time = tm_to_grn_time(&datetime, usec, &tm_truncated);
        if (tm_truncated) {
          *truncated = true;
        }
      }
      break;
    case MYSQL_TIMESTAMP_TIME:
      {
        DBUG_PRINT("info", ("mroonga: MYSQL_TIMESTAMP_TIME"));
        int sec =
          mysql_time->hour * 60 * 60 +
          mysql_time->minute * 60 +
          mysql_time->second;
        DBUG_PRINT("info", ("mroonga: sec=%d", sec));
        grn_time = GRN_TIME_PACK(sec, usec);
        if (mysql_time->neg) {
          grn_time = -grn_time;
        }
      }
      break;
    default:
      DBUG_PRINT("info", ("mroonga: default"));
      grn_time = 0;
      break;
    }

    DBUG_RETURN(grn_time);
  }

  void TimeConverter::grn_time_to_mysql_time(long long int grn_time,
                                             MYSQL_TIME *mysql_time) {
    MRN_DBUG_ENTER_METHOD();
    long long int sec;
    int usec;
    GRN_TIME_UNPACK(grn_time, sec, usec);
    DBUG_PRINT("info", ("mroonga: sec=%lld", sec));
    DBUG_PRINT("info", ("mroonga: usec=%d", usec));
    switch (mysql_time->time_type) {
    case MYSQL_TIMESTAMP_DATE:
      {
        DBUG_PRINT("info", ("mroonga: MYSQL_TIMESTAMP_DATE"));
        struct tm date;
        time_t sec_t = sec;
        // TODO: Add error check
        gmtime_r(&sec_t, &date);
        DBUG_PRINT("info", ("mroonga: tm_year=%d", date.tm_year));
        mysql_time->year = date.tm_year + TM_YEAR_BASE;
        DBUG_PRINT("info", ("mroonga: tm_mon=%d", date.tm_mon));
        mysql_time->month = date.tm_mon + 1;
        DBUG_PRINT("info", ("mroonga: tm_mday=%d", date.tm_mday));
        mysql_time->day = date.tm_mday;
      }
      break;
    case MYSQL_TIMESTAMP_DATETIME:
      {
        DBUG_PRINT("info", ("mroonga: MYSQL_TIMESTAMP_DATETIME"));
        struct tm date;
        time_t sec_t = sec;
        // TODO: Add error check
        gmtime_r(&sec_t, &date);
        DBUG_PRINT("info", ("mroonga: tm_year=%d", date.tm_year));
        mysql_time->year = date.tm_year + TM_YEAR_BASE;
        DBUG_PRINT("info", ("mroonga: tm_mon=%d", date.tm_mon));
        mysql_time->month = date.tm_mon + 1;
        DBUG_PRINT("info", ("mroonga: tm_mday=%d", date.tm_mday));
        mysql_time->day = date.tm_mday;
        DBUG_PRINT("info", ("mroonga: tm_hour=%d", date.tm_hour));
        mysql_time->hour = date.tm_hour;
        DBUG_PRINT("info", ("mroonga: tm_min=%d", date.tm_min));
        mysql_time->minute = date.tm_min;
        DBUG_PRINT("info", ("mroonga: tm_sec=%d", date.tm_sec));
        mysql_time->second = date.tm_sec;
        mysql_time->second_part = usec;
      }
      break;
    case MYSQL_TIMESTAMP_TIME:
      DBUG_PRINT("info", ("mroonga: MYSQL_TIMESTAMP_TIME"));
      if (sec < 0) {
        mysql_time->neg = true;
        sec = -sec;
      }
      mysql_time->hour = static_cast<unsigned int>(sec / 60 / 60);
      mysql_time->minute = sec / 60 % 60;
      mysql_time->second = sec % 60;
      mysql_time->second_part = usec;
      break;
    default:
      DBUG_PRINT("info", ("mroonga: default"));
      break;
    }
    DBUG_VOID_RETURN;
  }

  long long int TimeConverter::mysql_datetime_to_grn_time(long long int mysql_datetime,
                                                          bool *truncated) {
    MRN_DBUG_ENTER_METHOD();

    MYSQL_TIME mysql_time;
    mysql_time.time_type   = MYSQL_TIMESTAMP_DATETIME;
    mysql_time.neg         = 0;
    mysql_time.second_part = 0;
    mysql_time.second      = (mysql_datetime % 100);
    mysql_time.minute      = (mysql_datetime / 100 % 100);
    mysql_time.hour        = (mysql_datetime / 10000 % 100);
    mysql_time.day         = (mysql_datetime / 1000000 % 100);
    mysql_time.month       = (mysql_datetime / 100000000 % 100);
    mysql_time.year        = (mysql_datetime / 10000000000LL % 10000);

    long long int grn_time = mysql_time_to_grn_time(&mysql_time, truncated);

    DBUG_RETURN(grn_time);
  }

  long long int TimeConverter::grn_time_to_mysql_datetime(long long int grn_time) {
    MRN_DBUG_ENTER_METHOD();

    MYSQL_TIME mysql_time;
    mysql_time.time_type = MYSQL_TIMESTAMP_DATETIME;

    grn_time_to_mysql_time(grn_time, &mysql_time);

    long long int mysql_datetime =
      (mysql_time.second *           1) +
      (mysql_time.minute *         100) +
      (mysql_time.hour   *       10000) +
      (mysql_time.day    *     1000000) +
      (mysql_time.month  *   100000000) +
      (mysql_time.year   * 10000000000LL);

    DBUG_RETURN(mysql_datetime);
  }
}
