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

#ifndef mariadb_stats_h
#define mariadb_stats_h

/* Include file to handle mariadbd handler specific stats */

#include "ha_handler_stats.h"
#include "my_rdtsc.h"

/* Not active threads are ponting to this structure */
extern thread_local ha_handler_stats mariadb_dummy_stats;

/* Points to either THD->handler_stats or mariad_dummy_stats */
extern thread_local ha_handler_stats *mariadb_stats;

/*
  Returns 1 if MariaDB wants engine status
*/

inline bool mariadb_stats_active()
{
  return mariadb_stats->active != 0;
}

inline bool mariadb_stats_active(ha_handler_stats *stats)
{
  return stats->active != 0;
}

/* The following functions increment different engine status */

inline void mariadb_increment_pages_accessed()
{
  mariadb_stats->pages_accessed++;
}

inline void mariadb_increment_pages_updated(ulonglong count)
{
  mariadb_stats->pages_updated+= count;
}

inline void mariadb_increment_pages_read()
{
  mariadb_stats->pages_read_count++;
}

inline void mariadb_increment_undo_records_read()
{
  mariadb_stats->undo_records_read++;
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
  DBUG_ASSERT(mariadb_stats_active(stats));

  stats->pages_read_time+= (end_time - start_time);
}


/*
  Helper class to set mariadb_stats temporarly for one call in handler.cc
*/

class mariadb_set_stats
{
public:
  uint flag;
  mariadb_set_stats(ha_handler_stats *stats)
  {
    mariadb_stats= stats ? stats : &mariadb_dummy_stats;
  }
  ~mariadb_set_stats()
  {
    mariadb_stats= &mariadb_dummy_stats;
  }
};

#endif /* mariadb_stats_h */
