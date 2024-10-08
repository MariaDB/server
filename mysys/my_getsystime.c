/* Copyright (c) 2004, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009-2011 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


#include "mysys_priv.h"
#include "my_static.h"

#ifdef _WIN32
#define OFFSET_TO_EPOC 116444736000000000LL
static ulonglong query_performance_frequency=1;
#endif

#ifdef HAVE_LINUX_UNISTD_H
#include <linux/unistd.h>
#endif

/* For CYGWIN */
#if !defined(CLOCK_THREAD_CPUTIME_ID) && defined(CLOCK_THREAD_CPUTIME)
#define CLOCK_THREAD_CPUTIME_ID CLOCK_THREAD_CPUTIME
#endif

/*
  return number of nanoseconds since unspecified (but always the same)
  point in the past

  NOTE:
  Thus to get the current time we should use the system function
  with the highest possible resolution

  The value is not anchored to any specific point in time (e.g. epoch) nor
  is it subject to resetting or drifting by way of adjtime() or settimeofday(),
  and thus it is *NOT* appropriate for getting the current timestamp. It can be
  used for calculating time intervals, though.
*/

ulonglong my_interval_timer()
{
#ifdef HAVE_CLOCK_GETTIME
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec*1000000000ULL+tp.tv_nsec;
#elif defined(HAVE_GETHRTIME)
  return gethrtime();
#elif defined(_WIN32)
  DBUG_ASSERT(query_performance_frequency);
  LARGE_INTEGER t_cnt;
  QueryPerformanceCounter(&t_cnt);
  return (t_cnt.QuadPart / query_performance_frequency * 1000000000ULL) +
            ((t_cnt.QuadPart % query_performance_frequency) * 1000000000ULL /
             query_performance_frequency);
#else
  /* TODO: check for other possibilities for hi-res timestamping */
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec*1000000000ULL+tv.tv_usec*1000ULL;
#endif
}


/* Return current time in HRTIME_RESOLUTION (microseconds) since epoch */

my_hrtime_t my_hrtime()
{
  my_hrtime_t hrtime;
#if defined(_WIN32)
  ulonglong newtime;
  GetSystemTimePreciseAsFileTime((FILETIME*)&newtime);
  hrtime.val= (newtime - OFFSET_TO_EPOC)/10;
#elif defined(HAVE_CLOCK_GETTIME)
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  hrtime.val= tp.tv_sec*1000000ULL+tp.tv_nsec/1000ULL;
#else
  struct timeval t;
  /* The following loop is here because gettimeofday may fail */
  while (gettimeofday(&t, NULL) != 0) {}
  hrtime.val= t.tv_sec*1000000ULL + t.tv_usec;
#endif
  DBUG_EXECUTE_IF("system_time_plus_one_hour", hrtime.val += 3600*1000000ULL;);
  DBUG_EXECUTE_IF("system_time_minus_one_hour", hrtime.val -= 3600*1000000ULL;);
  return hrtime;
}

#ifdef _WIN32

/*
  Low accuracy, "coarse" timer.
  Has lower latency than my_hrtime(). Used in situations, where microsecond
  precision is not needed, e.g in Windows pthread_cond_timedwait, where POSIX 
  interface needs nanoseconds, yet the underlying Windows function only
  accepts milliseconds.
*/
my_hrtime_t my_hrtime_coarse()
{
  my_hrtime_t hrtime;
  ulonglong t;
  GetSystemTimeAsFileTime((FILETIME*)&t);
  hrtime.val= (t - OFFSET_TO_EPOC)/10;
  return hrtime;
}

#endif

void my_time_init()
{
#ifdef _WIN32
  compile_time_assert(sizeof(LARGE_INTEGER) ==
                      sizeof(query_performance_frequency));
  QueryPerformanceFrequency((LARGE_INTEGER *)&query_performance_frequency);
  DBUG_ASSERT(query_performance_frequency);
#endif
}


/*
  Return cpu time in 1/10th on a microsecond (1e-7 s)
*/

ulonglong my_getcputime()
{
#ifdef CLOCK_THREAD_CPUTIME_ID
  struct timespec tp;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tp))
    return 0;
  return (ulonglong)tp.tv_sec*10000000+(ulonglong)tp.tv_nsec/100;
#elif defined(__NR_clock_gettime)
  struct timespec tp;
  if (syscall(__NR_clock_gettime, CLOCK_THREAD_CPUTIME_ID, &tp))
    return 0;
  return (ulonglong)tp.tv_sec*10000000+(ulonglong)tp.tv_nsec/100;
#endif /* CLOCK_THREAD_CPUTIME_ID */
  return 0;
}
