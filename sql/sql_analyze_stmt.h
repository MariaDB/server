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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*

== ANALYZE-stmt classes ==

This file contains classes for supporting "ANALYZE statement" feature. These are 
a set of data structures that can be used to store the data about how the 
statement executed.

There are two kinds of data collection:

1. Various counters. We assume that incrementing counters has very low
overhead. Because of that, execution code increments counters unconditionally
(even when not running "ANALYZE $statement" commands. You run regular SELECT/
UPDATE/DELETE/etc and the counters are incremented).

As a free bonus, this lets us print detailed information into the slow query
log, should the query be slow.

2. Timing data. Measuring the time it took to run parts of query has noticeable
overhead. Because of that, we measure the time only when running "ANALYZE
$stmt").

*/

/*
  A class for tracking time it takes to do a certain action
*/
class Exec_time_tracker
{
protected:
  ulonglong count;
  ulonglong cycles;
  ulonglong last_start;

  void cycles_stop_tracking()
  {
    ulonglong end= my_timer_cycles();
    cycles += end - last_start;
    if (unlikely(end < last_start))
      cycles += ULONGLONG_MAX;
  }
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
    cycles_stop_tracking();
  }

  // interface for getting the time
  ulonglong get_loops() const { return count; }
  double get_time_ms() const
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
    cycles_stop_tracking();
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

/*
  A class for collecting read statistics.
  
  The idea is that we run several scans. Each scans gets rows, and then filters
  some of them out.  We count scans, rows, and rows left after filtering.

  (note: at the moment, the class is not actually tied to a physical table. 
   It can be used to track reading from files, buffers, etc).
*/

class Table_access_tracker 
{
public:
  Table_access_tracker() :
    r_scans(0), r_rows(0), /*r_rows_after_table_cond(0),*/
    r_rows_after_where(0)
  {}

  ha_rows r_scans; /* How many scans were ran on this join_tab */
  ha_rows r_rows; /* How many rows we've got after that */
  ha_rows r_rows_after_where; /* Rows after applying attached part of WHERE */

  bool has_scans() { return (r_scans != 0); }
  ha_rows get_loops() { return r_scans; }
  double get_avg_rows()
  {
    return r_scans ? ((double)r_rows / r_scans): 0;
  }

  double get_filtered_after_where()
  {
    double r_filtered;
    if (r_rows > 0)
      r_filtered= (double)r_rows_after_where / r_rows;
    else
      r_filtered= 1.0;

    return r_filtered;
  }
  
  inline void on_scan_init() { r_scans++; }
  inline void on_record_read() { r_rows++; }
  inline void on_record_after_where() { r_rows_after_where++; }
};


class Json_writer;

/*
  This stores the data about how filesort executed.

  A few things from here (e.g. r_used_pq, r_limit) belong to the query plan,
  however, these parameters are calculated right during the execution so we 
  can't easily put them into the query plan.

  The class is designed to handle multiple invocations of filesort().
*/

class Filesort_tracker : public Sql_alloc
{
public:
  Filesort_tracker(bool do_timing) :
    time_tracker(do_timing), r_limit(0), r_used_pq(0),
    r_examined_rows(0), r_sorted_rows(0), r_output_rows(0),
    sort_passes(0),
    sort_buffer_size(0)
  {}
  
  /* Functions that filesort uses to report various things about its execution */

  inline void report_use(ha_rows r_limit_arg)
  {
    if (!time_tracker.get_loops())
      r_limit= r_limit_arg;
    else
      r_limit= (r_limit != r_limit_arg)? 0: r_limit_arg;

    ANALYZE_START_TRACKING(&time_tracker);
  }
  inline void incr_pq_used() { r_used_pq++; }

  inline void report_row_numbers(ha_rows examined_rows, 
                                 ha_rows sorted_rows,
                                 ha_rows returned_rows) 
  { 
    r_examined_rows += examined_rows;
    r_sorted_rows   += sorted_rows;
    r_output_rows   += returned_rows;
  }

  inline void report_merge_passes_at_start(ulong passes)
  {
    sort_passes -= passes;
  }
  inline void report_merge_passes_at_end(ulong passes)
  {
    ANALYZE_STOP_TRACKING(&time_tracker);
    sort_passes += passes;
  }

  inline void report_sort_buffer_size(size_t bufsize)
  {
    if (sort_buffer_size)
      sort_buffer_size= ulonglong(-1); // multiple buffers of different sizes
    else
      sort_buffer_size= bufsize;
  }
  
  /* Functions to get the statistics */
  void print_json_members(Json_writer *writer);
  
  ulonglong get_r_loops() const { return time_tracker.get_loops(); }
  double get_avg_examined_rows() 
  { 
    return ((double)r_examined_rows) / get_r_loops();
  }
  double get_avg_returned_rows()
  { 
    return ((double)r_output_rows) / get_r_loops(); 
  }
  double get_r_filtered()
  {
    if (r_examined_rows > 0)
      return ((double)r_sorted_rows / r_examined_rows);
    else
      return 1.0;
  }
private:
  Time_and_counter_tracker time_tracker;

  //ulonglong r_loops; /* How many times filesort was invoked */
  /*
    LIMIT is typically a constant. There is never "LIMIT 0".
      HA_POS_ERROR means we never had a limit
      0            means different values of LIMIT were used in 
                   different filesort invocations
      other value  means the same LIMIT value was used every time.
  */
  ulonglong r_limit;
  ulonglong r_used_pq; /* How many times PQ was used */

  /* How many rows were examined (before checking the select->cond) */
  ulonglong r_examined_rows;
  
  /* 
    How many rows were put into sorting (this is examined_rows minus rows that
    didn't pass the WHERE condition)
  */
  ulonglong r_sorted_rows;

  /*
    How many rows were returned. This is equal to r_sorted_rows, unless there
    was a LIMIT N clause in which case filesort would not have returned more
    than N rows.
  */
  ulonglong r_output_rows;

  /* How many sorts in total (divide by r_count to get the average) */
  ulonglong sort_passes;
  
  /* 
    0              - means not used (or not known 
    (ulonglong)-1  - multiple
    other          - value
  */
  ulonglong sort_buffer_size;
};

