/*
   Copyright (c) 2015, 2020, MariaDB Corporation.

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

/* fake microseconds as cycles if cycles isn't available */

static inline double timer_tracker_frequency()
{
#if (MY_TIMER_ROUTINE_CYCLES)
  return static_cast<double>(sys_timer_info.cycles.frequency);
#else
  return static_cast<double>(sys_timer_info.microseconds.frequency);
#endif
}


class Gap_time_tracker;
void attach_gap_time_tracker(THD *thd, Gap_time_tracker *gap_tracker, ulonglong timeval);
void process_gap_time_tracker(THD *thd, ulonglong timeval);

/*
  A class for tracking time it takes to do a certain action
*/
class Exec_time_tracker
{
protected:
  ulonglong count;
  ulonglong cycles;
  ulonglong last_start;

  ulonglong measure() const
  {
#if (MY_TIMER_ROUTINE_CYCLES)
    return my_timer_cycles();
#else
    return my_timer_microseconds();
#endif
  }

  void cycles_stop_tracking(THD *thd)
  {
    ulonglong end= measure();
    cycles += end - last_start;

    process_gap_time_tracker(thd, end);
    if (my_gap_tracker)
      attach_gap_time_tracker(thd, my_gap_tracker, end);
  }

  /*
    The time spent after stop_tracking() call on this object and any
    subsequent time tracking call will be billed to this tracker.
  */
  Gap_time_tracker *my_gap_tracker;
public:
  Exec_time_tracker() : count(0), cycles(0), my_gap_tracker(NULL) {}

  void set_gap_tracker(Gap_time_tracker *gap_tracker)
  {
    my_gap_tracker= gap_tracker;
  }

  // interface for collecting time
  void start_tracking(THD *thd)
  {
    last_start= measure();
    process_gap_time_tracker(thd, last_start);
  }

  void stop_tracking(THD *thd)
  {
    count++;
    cycles_stop_tracking(thd);
  }

  // interface for getting the time
  ulonglong get_loops() const { return count; }

  inline double cycles_to_ms(ulonglong cycles_arg) const
  {
    // convert 'cycles' to milliseconds.
    return 1000.0 * static_cast<double>(cycles_arg) /
      timer_tracker_frequency();
  }

  double get_time_ms() const
  {
    return cycles_to_ms(cycles);
  }
  ulonglong get_cycles() const
  {
    return cycles;
  }

  bool has_timed_statistics() const { return cycles > 0; }
};


/*
  Tracker for time spent between the calls to Exec_time_tracker's {start|
  stop}_tracking().

  @seealso Gap_time_tracker_data in sql_class.h
*/
class Gap_time_tracker
{
  ulonglong cycles;
public:
  Gap_time_tracker() : cycles(0) {}

  void log_time(ulonglong start, ulonglong end) {
    cycles += end - start;
  }

  double get_time_ms() const
  {
    // convert 'cycles' to milliseconds.
    return 1000.0 * static_cast<double>(cycles) / timer_tracker_frequency();
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
  void stop_tracking(THD *thd)
  {
    cycles_stop_tracking(thd);
  }
};

#define ANALYZE_START_TRACKING(thd, tracker) \
  { \
    (tracker)->incr_loops(); \
    if (unlikely((tracker)->timed)) \
    { (tracker)->start_tracking(thd); } \
  }

#define ANALYZE_STOP_TRACKING(thd, tracker) \
  if (unlikely((tracker)->timed)) \
  { (tracker)->stop_tracking(thd); }


/*
  Just a counter to increment one value. Wrapped in a class to be uniform
  with other counters used by ANALYZE.
*/

class Counter_tracker
{
public:
  Counter_tracker() : r_scans(0) {}
  ha_rows r_scans;

  inline void on_scan_init() { r_scans++; }

  bool has_scans() const { return (r_scans != 0); }
  ha_rows get_loops() const { return r_scans; }
};


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
  Table_access_tracker() : r_scans(0), r_rows(0), r_rows_after_where(0)
  {}

  ha_rows r_scans; /* how many scans were ran on this join_tab */
  ha_rows r_rows; /* How many rows we've got after that */
  ha_rows r_rows_after_where; /* Rows after applying attached part of WHERE */

  double get_avg_rows() const
  {
    return r_scans
      ? static_cast<double>(r_rows) / static_cast<double>(r_scans)
      : 0;
  }

  double get_filtered_after_where() const
  {
    return r_rows > 0
      ? static_cast<double>(r_rows_after_where) /
        static_cast<double>(r_rows)
      : 1.0;
  }

  inline void on_scan_init() { r_scans++; }
  inline void on_record_read() { r_rows++; }
  inline void on_record_after_where() { r_rows_after_where++; }

  bool has_scans() const { return (r_scans != 0); }
  ha_rows get_loops() const { return r_scans; }
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
    sort_buffer_size(0),
    r_using_addons(false),
    r_packed_addon_fields(false),
    r_sort_keys_packed(false)
  {}
  
  /* Functions that filesort uses to report various things about its execution */

  inline void report_use(THD *thd, ha_rows r_limit_arg)
  {
    if (!time_tracker.get_loops())
      r_limit= r_limit_arg;
    else
      r_limit= (r_limit != r_limit_arg)? 0: r_limit_arg;

    ANALYZE_START_TRACKING(thd, &time_tracker);
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
  inline void report_merge_passes_at_end(THD *thd, ulong passes)
  {
    ANALYZE_STOP_TRACKING(thd, &time_tracker);
    sort_passes += passes;
  }

  inline void report_sort_buffer_size(size_t bufsize)
  {
    if (sort_buffer_size)
      sort_buffer_size= ulonglong(-1); // multiple buffers of different sizes
    else
      sort_buffer_size= bufsize;
  }

  inline void report_addon_fields_format(bool addons_packed)
  {
    r_using_addons= true;
    r_packed_addon_fields= addons_packed;
  }
  inline void report_sort_keys_format(bool sort_keys_packed)
  {
    r_sort_keys_packed= sort_keys_packed;
  }

  void get_data_format(String *str);

  /* Functions to get the statistics */
  void print_json_members(Json_writer *writer);

  ulonglong get_r_loops() const { return time_tracker.get_loops(); }
  double get_avg_examined_rows() const
  {
    return static_cast<double>(r_examined_rows) /
      static_cast<double>(get_r_loops());
  }
  double get_avg_returned_rows() const
  {
    return static_cast<double>(r_output_rows) /
      static_cast<double>(get_r_loops());
  }
  double get_r_filtered() const
  {
    return r_examined_rows > 0
      ? static_cast<double>(r_sorted_rows) /
        static_cast<double>(r_examined_rows)
      : 1.0;
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
  bool r_using_addons;
  bool r_packed_addon_fields;
  bool r_sort_keys_packed;
};


/**
  A class to collect data about how rowid filter is executed.

  It stores information about how rowid filter container is filled,
  containers size and observed selectivity.

  The observed selectivity is calculated in this way.
  Some elements elem_set are checked if they belong to container.
  Observed selectivity is calculated as the count of elem_set
  elements that belong to container devided by all elem_set elements.
*/

class Rowid_filter_tracker : public Sql_alloc
{
private:
  /* A member to track the time to fill the rowid filter */
  Time_and_counter_tracker time_tracker;

  /* Size of the rowid filter container buffer */
  size_t container_buff_size;

  /* Count of elements that were used to fill the rowid filter container */
  uint container_elements;

  /* Elements counts used for observed selectivity calculation */
  uint n_checks;
  uint n_positive_checks;
public:
  Rowid_filter_tracker(bool do_timing) :
    time_tracker(do_timing), container_buff_size(0),
    container_elements(0), n_checks(0), n_positive_checks(0)
  {}

  inline void start_tracking(THD *thd)
  {
    ANALYZE_START_TRACKING(thd, &time_tracker);
  }

  inline void stop_tracking(THD *thd)
  {
    ANALYZE_STOP_TRACKING(thd, &time_tracker);
  }

  /* Save container buffer size in bytes */
  inline void report_container_buff_size(uint elem_size)
  {
   container_buff_size= container_elements * elem_size / 8;
  }

  Time_and_counter_tracker *get_time_tracker()
  {
    return &time_tracker;
  }

  double get_time_fill_container_ms() const
  {
    return time_tracker.get_time_ms();
  }

  void increment_checked_elements_count(bool was_checked)
  {
    n_checks++;
    if (was_checked)
     n_positive_checks++;
  }

  inline void set_container_elements_count(uint elements)
  { container_elements= elements; }

  uint get_container_elements() const { return container_elements; }

  uint get_container_lookups() { return n_checks; }

  double get_r_selectivity_pct() const
  {
    return n_checks ? static_cast<double>(n_positive_checks) /
                      static_cast<double>(n_checks) : 0;
  }

  size_t get_container_buff_size() const { return container_buff_size; }
};
