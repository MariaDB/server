/*
   Copyright (c) 2015 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/*
  A class for tracking time it takes to do a certain action
*/
class Exec_time_tracker
{
protected:
  ulonglong count;
  ulonglong cycles;
  ulonglong last_start;
public:
  Exec_time_tracker() : count(0), cycles(0) {}
  
  // interface for collecting time
  void start_tracking()
  {
    last_start= my_timer_cycles();
  }

  void stop_tracking()
  {
    count++;
    cycles += my_timer_cycles()- last_start;
  }

  // interface for getting the time
  ulonglong get_loops() { return count; }
  double get_time_ms()
  {
    // convert 'cycles' to milliseconds.
    return 1000 * ((double)cycles) / sys_timer_info.cycles.frequency;
  }
};


/*
  A class for counting certain actions (in all queries), and optionally
  collecting the timings (in ANALYZE queries).
*/

class Time_and_counter_tracker: public Exec_time_tracker
{
public: 
  const bool timed;
  
  Time_and_counter_tracker(bool timed_arg) : timed(timed_arg)
  {}
   
  /* Loops are counted in both ANALYZE and regular queries, as this is cheap */
  void incr_loops() { count++; }
  
  /*
    Unlike Exec_time_tracker::stop_tracking, we don't increase loops.
  */
  void stop_tracking()
  {
    cycles += my_timer_cycles()- last_start;
  }
};

#define ANALYZE_START_TRACKING(tracker) \
  { \
    (tracker)->incr_loops(); \
    if (unlikely((tracker)->timed)) \
    { (tracker)->start_tracking(); } \
  }

#define ANALYZE_STOP_TRACKING(tracker) \
  if (unlikely((tracker)->timed)) \
  { (tracker)->stop_tracking(); }


