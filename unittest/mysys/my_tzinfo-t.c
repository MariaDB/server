/*
   Copyright (c) 2023, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */

/*
  This file tests my_tzset/my_get_tzinfo APIs
*/
#include <my_global.h>
#include <my_sys.h>
#include "tap.h"
#include <stdlib.h>

/**
  Seconds since epoch used for "summer" timestamp
  Corresponds to Jul 22 2023 04:26:40 GMT
  Used to test timezone daylight savings UTC offset and DST abbreviation
*/
#define SUMMER_TIMESTAMP   1690000000

/**
  Seconds since epoch used for "winter" timestamp
  Corresponds to Nov 14 2023 22:13:20 GMT+
  Used to test standard (no daylight savings) UTC offset and abbreviation
*/
#define WINTER_TIMESTAMP   1700000000

#ifdef _WIN32
#define timegm _mkgmtime
#endif
/**
  Check expected offset from UTC, corresponding to specific
  timestamp.

  On Windows, it is possible that my_get_tzinfo() is using
  ICU to calculate offset.This function rechecks that value is the
  same when using C runtime's _mkgmtime().

  Elsewhere, my_get_tzinfo is taking this value from non-standard glibc
  extension struct tm::tm_gmtoff. This function rechecks that the value
  is the same if calculated with timegm().
*/
static void check_utc_offset(time_t t, long expected, const char *comment)
{
#if defined _WIN32 || defined __linux__
  struct tm local_time;
  long offset;
  localtime_r(&t, &local_time);
  offset= (long) (timegm(&local_time) - t);
  ok(offset == expected, "%s: Offset for timestamp %lld is %ld/%ld", comment,
      (long long) t,expected, offset);
#else
  skip(1, "no utc offset check");
#endif
}

/**
  Test my_tzset/my_get_tzinfo functions for a single named timezone.

  @param[in] tz_env. Timezone name, as used for TZ env.variable

  @param[in] expected_tznames. Expected "sytem_timezone" names.
             For example, expected names for timezone PST8PDT can
             be PST8PDT, PST or PDT

  @param[in] summer_gmt_off. Expected UTC offset, for SUMMER_TIMESTAMP
             For timezones with DST savings on northern hemisphere
             it is the expected DST offset from UTC

  @param[in] summer_time_abbr . Expected standard abbreviation
             corresponding to SUMMER_TIMESTAMP. For example, it is
             "PDT" for timezone PST8PDT

  @param[in] winter_gmt_off. Expected UTC offset, for WINTER_TIMESTAMP

  @param[in] winter_time_abbr . Expected standard abbreviation
             corresponding to WINTER_TIMESTAMP. For example, it is
             "PST" for timezone PST8PDT.
*/
void test_timezone(const char *tz_env, const char **expected_tznames,
                   long summer_gmt_off, const char *summer_time_abbr,
                   long winter_gmt_off, const char *winter_time_abbr)
{
  char timezone_name[64];
  int found;
  struct my_tz tz;

  setenv("TZ", tz_env, 1);
  my_tzset();
  my_tzname(timezone_name, sizeof(timezone_name));

  /* Check expected timezone names. */
  found= 0;
  for (int i= 0; expected_tznames[i]; i++)
  {
    if (!strcmp(expected_tznames[i], timezone_name))
    {
      found= 1;
      break;
    }
  }
  ok(found, "%s: timezone_name = %s", tz_env, timezone_name);
  my_tzinfo(SUMMER_TIMESTAMP, &tz);
  ok(summer_gmt_off == tz.seconds_offset, "%s: Summer GMT offset %ld", tz_env, tz.seconds_offset);
  check_utc_offset(SUMMER_TIMESTAMP,tz.seconds_offset, tz_env);

  ok(!strcmp(summer_time_abbr, tz.abbreviation), "%s: Summer time abbreviation %s",
     tz_env, tz.abbreviation);
  my_tzinfo(WINTER_TIMESTAMP, &tz);
  ok(winter_gmt_off == tz.seconds_offset, "%s: Winter GMT offset  %ld", tz_env, tz.seconds_offset);
  check_utc_offset(WINTER_TIMESTAMP, tz.seconds_offset, tz_env);
  ok(!strcmp(winter_time_abbr, tz.abbreviation), "%s: Winter time abbreviation %s",
     tz_env, tz.abbreviation);
}

/* Check default timezone.*/
static void test_default_timezone()
{
  char timezone_name[64];

  time_t timestamps[]= {SUMMER_TIMESTAMP, WINTER_TIMESTAMP, time(NULL)};
  size_t i;
  struct my_tz tz;
#ifdef _WIN32
  (void) putenv("TZ=");
#else
  unsetenv("TZ");
#endif

  my_tzset();
  my_tzname(timezone_name, sizeof(timezone_name));
#ifdef _WIN32
  /* Expect timezone name like Europe/Berlin */
  ok(strstr(timezone_name, "/") != NULL, "Default timezone name %s",
     timezone_name);
#else
  skip(1, "no test for default timezone name %s", timezone_name);
#endif

  for (i = 0; i < array_elements(timestamps); i++)
  {
    my_tzinfo(timestamps[i], &tz);
    ok(tz.seconds_offset % 60 == 0,
       "GMT offset is whole number of minutes %ld", tz.seconds_offset);
    check_utc_offset(timestamps[i], tz.seconds_offset, timezone_name);
    ok(strlen(tz.abbreviation) < 8, "tz abbreviation %s", tz.abbreviation);
  }
}

int main(int argc __attribute__((unused)), char *argv[])
{
  const char *PST8PDT_names[]= {"PST", "PDT", "PST8PDT", NULL};
  const char *GMT_names[]= {"GMT", "Etc/UTC", NULL};
  const char *GST_minus1GDT_names[]= {"GST", "GDT", NULL};
  const char *IST_names[]= {"IST",NULL};
  MY_INIT(argv[0]);

  plan(38);
  test_default_timezone();

  /*
    Test PST8PDT timezone
    Standard timezone, supported everywhere. Note - this one is supported by
    ICU, so it would be using ICU for calculation on Windows
  */
  test_timezone("PST8PDT", PST8PDT_names, -25200, "PDT", -28800, "PST");

  /*
    Test GMT. Supported by ICU, would be using ICU for calculations
  */
  test_timezone("GMT", GMT_names, 0, "GMT", 0, "GMT");

  /*
    Non-standard "Germany" timezone, taken from Windows tzset() documentation
    example. Unsupported by ICU, will be using C runtime on Windows for
    abbreviations, and offset calculations.
  */
  test_timezone("GST-1GDT", GST_minus1GDT_names, 7200, "GDT", 3600, "GST");

  /* India */
  test_timezone("IST-5:30", IST_names, 19800, "IST", 19800, "IST");

  my_end(0);
  return exit_status();
}
