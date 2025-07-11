/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com> and logzero <core13@gmx.net>
*/

#ifndef _WIN32
#include <sys/time.h>

static double __attribute__ ((unused)) real_sec_time (void) {
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return tv.tv_usec / 1000000.0 + tv.tv_sec;
}

static double __attribute__ ((unused)) real_usec_time (void) {
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return tv.tv_usec + tv.tv_sec * 1000000.0;
}
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// does not return actual time, use as a stopwatch only
static double real_sec_time (void) {
  LARGE_INTEGER freq, count;

  if (QueryPerformanceFrequency (&freq) && QueryPerformanceCounter (&count))
    return (double) count.QuadPart / (double) freq.QuadPart;
  return 0.0;
}

static double real_usec_time (void) {
  LARGE_INTEGER freq, count;

  if (QueryPerformanceFrequency (&freq) && QueryPerformanceCounter (&count))
    return (double) count.QuadPart / ((double) freq.QuadPart * 1.0E-6);
  return 0.0;
}
#endif
