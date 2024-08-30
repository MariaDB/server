/* Copyright (c) 2022, MariaDB Corporation.

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

/*
  Trivial framework to add a tracker to a C function
*/

#include "my_rdtsc.h"

struct my_time_tracker
{
  ulonglong counter;
  ulonglong cycles;
};

#ifdef HAVE_TIME_TRACKING
#define START_TRACKING ulonglong my_start_time= my_timer_cycles()
#define END_TRACKING(var) \
  { \
     ulonglong my_end_time= my_timer_cycles();                     \
     (var)->counter++;                                             \
     (var)->cycles+= (unlikely(my_end_time < my_start_time) ?      \
                     my_end_time - my_start_time + ULONGLONG_MAX : \
                     my_end_time - my_start_time);                 \
  }
#else
#define START_TRACKING
#define END_TRACKING(var) do { } while(0)
#endif
