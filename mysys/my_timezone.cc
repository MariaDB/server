/*
   Copyright (c) 2023 MariaDB

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

#include <config.h>
#include <my_global.h>
#include <my_sys.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <icu.h>
#include <objbase.h>
#define MAX_TZ_ABBR 64

static bool use_icu_for_tzinfo;

/*
  Retrieve GMT offset and timezone abbreviation using ICU.
*/
static void icu_get_tzinfo(time_t t, my_tz* tz)
{
  UErrorCode status= U_ZERO_ERROR;
  const char *locale= nullptr;
  UCalendar* cal= ucal_open(nullptr, -1, locale, UCAL_GREGORIAN, &status);
  ucal_setMillis(cal, 1000.0 * t, &status);
  int32_t zone_offset= ucal_get(cal, UCAL_ZONE_OFFSET, &status);
  int32_t dst_offset= ucal_get(cal, UCAL_DST_OFFSET, &status);
  tz->seconds_offset= (zone_offset + dst_offset) / 1000;

  UChar u_tz_abbr[MAX_TZ_ABBR];
  ucal_getTimeZoneDisplayName(cal,
                              dst_offset ? UCAL_SHORT_DST : UCAL_SHORT_STANDARD,
                              locale, u_tz_abbr, MAX_TZ_ABBR, &status);
  ucal_close(cal);
  size_t num;
  wcstombs_s(&num, tz->abbreviation, sizeof(tz->abbreviation),
      (wchar_t *) u_tz_abbr, sizeof(u_tz_abbr));
}

/*
  Return GMT offset and TZ abbreviation using Windows C runtime.

  Only used if TZ environment variable is set, and there is no
  corresponding timezone in ICU data.
*/
static void win_get_tzinfo(time_t t, my_tz* tz)
{
  struct tm local_time;
  localtime_r(&t, &local_time);
  int is_dst= local_time.tm_isdst?1:0;
  tz->seconds_offset= (long)(_mkgmtime(&local_time) - t);
  snprintf(tz->abbreviation, sizeof(tz->abbreviation), "%s", _tzname[is_dst]);
}

#define MAX_TIMEZONE_LEN 128

/*
  Synchronizes C runtime timezone with ICU timezone.

  Must be called after tzset().

  If TZ environment variable is set, tries to find ICU
  timezone matching the variable value.If such timezone
  is found, it is set as default timezone for ICU.

  @return 0  success
          -1 otherwise
*/
static int sync_icu_timezone()
{
  const char *tz_env= getenv("TZ");
  UErrorCode ec= U_ZERO_ERROR;
  UEnumeration *en= nullptr;
  int ret= -1;

  if (!tz_env)
  {
    /* TZ environment variable not set - use default timezone*/
    return 0;
  }

  int timezone_offset_ms = -1000 * _timezone;
  int dst_offset_ms = -1000 * _dstbias *(_daylight != 0);

  /*
    Find ICU timezone with the same UTC and DST offsets
    and name as C runtime.
  */
  en= ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_ANY, nullptr,
                                     &timezone_offset_ms, &ec);
  if (U_FAILURE(ec))
    return -1;

  for (;;)
  {
    int32_t len;
    const char *tzid= uenum_next(en, &len, &ec);
    if (U_FAILURE(ec))
      break;

    if (!tzid)
      break;
    UChar u_tzid[MAX_TIMEZONE_LEN];
    u_uastrncpy(u_tzid, tzid, MAX_TIMEZONE_LEN);
    int32_t dst_savings= ucal_getDSTSavings(u_tzid, &ec);
    if (U_FAILURE(ec))
      break;

    if (dst_savings == dst_offset_ms)
    {
      if (tz_env && !strcmp(tzid, tz_env))
      {
        /*
          Found timezone ID that matches TZ env.var
          exactly, e.g PST8PDT
        */
        UChar u_tzid[MAX_TIMEZONE_LEN];
        u_uastrncpy(u_tzid, tzid, MAX_TIMEZONE_LEN);
        ucal_setDefaultTimeZone(u_tzid, &ec);
        ret= 0;
        break;
      }
    }
  }
  uenum_close(en);
  return ret;
}
#endif /* _WIN32 */

/**
  Initialize time conversion information

  @param[out] sys_timezone     name of the current timezone
  @param[in]  sys_timezone_len length of the 'sys_timezone' buffer
  
*/
extern "C" void my_tzset()
{
  tzset();
#ifdef _WIN32
  /*
    CoInitializeEx is needed by ICU on older Windows 10, until
    version 1903.
  */
  (void) CoInitializeEx(NULL, COINITBASE_MULTITHREADED);
  use_icu_for_tzinfo= !sync_icu_timezone();
#endif
}

/**
  Retrieve current timezone name
  @param[out] sys_timezone - buffer to receive timezone name
  @param[in]  size - size of sys_timezone buffer
*/
extern "C" void my_tzname(char* sys_timezone, size_t size)
{
#ifdef _WIN32
  if (use_icu_for_tzinfo)
  {
    /* TZ environment variable not set - return default timezone name*/
    UChar default_tzname[MAX_TIMEZONE_LEN];
    UErrorCode ec;
    int32_t len=
        ucal_getDefaultTimeZone(default_tzname, MAX_TIMEZONE_LEN, &ec);
    if (U_SUCCESS(ec))
    {
      u_austrncpy(sys_timezone, default_tzname, (int32_t) size);
      return;
    }
    use_icu_for_tzinfo= false;
  }
#endif
  struct tm tm;
  time_t t;
  tzset();
  t= time(NULL);
  localtime_r(&t, &tm);
  const char *tz_name= tzname[tm.tm_isdst != 0 ? 1 : 0];
  snprintf(sys_timezone, size, "%s", tz_name);
}

/**
  Return timezone information (GMT offset, timezone abbreviation)
  corresponding to specific timestamp.

  @param[in]   t          timestamp (seconds since Unix epoch)
  @param[out]  gmt_offset offset from GMT, in seconds
  @param[out]  abbr       buffer to receive time zone abbreviation
  @param[in]   abbr_size  size of the abbr buffer
*/
void my_tzinfo(time_t t, struct my_tz* tz)
{
#ifdef _WIN32
  if (use_icu_for_tzinfo)
    icu_get_tzinfo(t, tz);
  else
    win_get_tzinfo(t, tz);
#else
  struct tm tm_local_time;
  localtime_r(&t, &tm_local_time);
  snprintf(tz->abbreviation, sizeof(tz->abbreviation), "%s", tm_local_time.tm_zone);
  tz->seconds_offset= tm_local_time.tm_gmtoff;
#endif
}
