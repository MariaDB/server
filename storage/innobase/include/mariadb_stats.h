/*****************************************************************************

Copyright (c) 2023, MariaDB Foundation

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#pragma once

#include "ha_handler_stats.h"
#include "my_rdtsc.h"

/* We do not want a dynamic initialization function to be
conditionally invoked on each access to a C++11 extern thread_local. */
#if __cplusplus >= 202002L
# define simple_thread_local constinit thread_local
#else
# define simple_thread_local IF_WIN(__declspec(thread),__thread)
#endif

/** Pointer to handler::active_handler_stats or nullptr (via .tbss) */
extern simple_thread_local ha_handler_stats *mariadb_stats;

/*
  Returns nonzero if MariaDB wants engine status
*/

inline uint mariadb_stats_active()
{
  if (ha_handler_stats *stats= mariadb_stats)
    return stats->active;
  return 0;
}

/* The following functions increment different engine status */

inline void mariadb_increment_pages_accessed(ha_handler_stats *stats)
{
  if (stats)
    stats->pages_accessed++;
}

inline void mariadb_increment_pages_accessed()
{
  mariadb_increment_pages_accessed(mariadb_stats);
}

inline void mariadb_increment_pages_updated(ulonglong count)
{
  if (ha_handler_stats *stats= mariadb_stats)
    stats->pages_updated+= count;
}

inline void mariadb_increment_pages_read(ha_handler_stats *stats)
{
  if (stats)
    stats->pages_read_count++;
}

inline void mariadb_increment_pages_read()
{
  mariadb_increment_pages_read(mariadb_stats);
}

inline void mariadb_increment_undo_records_read()
{
  if (ha_handler_stats *stats= mariadb_stats)
    stats->undo_records_read++;
}

inline void mariadb_increment_pages_prefetched(ulint n_pages)
{
  if (ha_handler_stats *stats= mariadb_stats)
    stats->pages_prefetched += n_pages;
}

/*
  The following has to be identical code as measure() in sql_analyze_stmt.h

  One should only call this if mariadb_stats_active() is true.
*/

inline ulonglong mariadb_measure()
{
#if (MY_TIMER_ROUTINE_CYCLES)
    return my_timer_cycles();
#else
    return my_timer_microseconds();
#endif
}

/*
  Call this only of start_time != 0
  See buf0rea.cc for an example of how to use it efficiently
*/

inline void mariadb_increment_pages_read_time(ulonglong start_time)
{
  ha_handler_stats *stats= mariadb_stats;
  ulonglong end_time= mariadb_measure();
  /* Check that we only call this if active, see example! */
  DBUG_ASSERT(start_time);
  DBUG_ASSERT(stats->active);

  stats->pages_read_time+= (end_time - start_time);
}


/*
  Helper class to set mariadb_stats temporarly for one call in handler.cc
*/

class mariadb_set_stats
{
public:
  mariadb_set_stats(ha_handler_stats *stats)
  {
    mariadb_stats= stats;
  }
  ~mariadb_set_stats()
  {
    mariadb_stats= nullptr;
  }
};
