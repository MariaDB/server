/* Copyright (c) 2015, 2017, Tencent.
Use is subject to license terms

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include "repl_speedlimit_util.h"

const unsigned long Trace::kTraceGeneral  = 0x0001;
const unsigned long Trace::kTraceDetail   = 0x0010;
const unsigned long Trace::kTraceFunction = 0x0020;
const unsigned long Trace::kTraceTimer    = 0x0040;


uint64 get_current_ms()
{
#ifdef _WIN32
  ulonglong newtime;
  GetSystemTimeAsFileTime((FILETIME*)&newtime);
  return (newtime / 10000);
#else
  ulonglong newtime;
  struct timespec ts;

  while (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
  {}

  newtime = (ulonglong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

  return newtime;
#endif
}

void sleep_ms(int64 micro_second)
{
#if defined(__WIN__)
  Sleep((DWORD)micro_second);      /* Sleep() has millisecond arg */
#else
  ::usleep(micro_second * 1000UL);
#endif
}
