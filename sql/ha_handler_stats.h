#ifndef HA_HANDLER_STATS_INCLUDED
#define HA_HANDLER_STATS_INCLUDED
/*
   Copyright (c) 2023, MariaDB Foundation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/* Definitions for parameters to do with handler-routines */

class ha_handler_stats
{
public:
  ulonglong pages_accessed;              /* Pages accessed from page cache */
  ulonglong pages_updated;               /* Pages changed in page cache */
  ulonglong pages_read_count;            /* Pages read from disk */
  ulonglong pages_read_time;             /* Time reading pages, in microsec. */
  ulonglong undo_records_read;
  ulonglong engine_time;                 /* Time spent in engine in microsec */
  uint      active;                      /* <> 0 if status has to be updated */
#define first_stat pages_accessed
#define last_stat  engine_time
  inline void reset()
  {
    bzero((void*) this, sizeof(*this));
  }
  inline void add(ha_handler_stats *stats)
  {
    ulonglong *to= &first_stat;
    ulonglong *from= &stats->first_stat;
    do
    {
      (*to)+= *from++;
    } while (to++ != &last_stat);
  }
  inline bool has_stats()
  {
    ulonglong *to= &first_stat;
    do
    {
      if (*to)
        return 1;
    } while (to++ != &last_stat);
    return 0;
  }
};
#endif /* HA_HANDLER_STATS_INCLUDED */
