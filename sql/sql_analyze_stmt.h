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


typedef enum 
{
  EXPL_NO_TMP_TABLE=0,
  EXPL_TMP_TABLE_BUFFER,
  EXPL_TMP_TABLE_GROUP,
  EXPL_TMP_TABLE_DISTINCT
} enum_tmp_table_use;


typedef enum 
{
  EXPL_ACTION_EOF, /* not-an-action */
  EXPL_ACTION_FILESORT,
  EXPL_ACTION_TEMPTABLE,
  EXPL_ACTION_REMOVE_DUPS,
} enum_qep_action;


/*
  This is to track how a JOIN object has resolved ORDER/GROUP BY/DISTINCT
  
  We are not tied to the query plan at all, because query plan does not have 
  sufficient information. *A lot* of decisions about ordering/grouping are 
  made at very late stages (in JOIN::exec, JOIN::init_execution, in
  create_sort_index and even in create_tmp_table).

  The idea is that operations that happen during select execution will report
  themselves. We have these operations:
  - Sorting with filesort()
  - Duplicate row removal (the one done by remove_duplicates()).
  - Use of temporary table to buffer the result.

  There is also "Selection" operation, done by do_select(). It reads rows,
  there are several distinct cases:
   1. doing the join operation on the base tables
   2. reading the temporary table
   3. reading the filesort output
  it would be nice to build execution graph, e.g.

    Select(JOIN op) -> temp.table -> filesort -> Select(filesort result)

  the problem is that there is no way to tell what a do_select() call will do.

  Our solution is not to have explicit selection operations. We make these
  assumptions about the query plan:
  - Select(JOIN op) is the first operation in the query plan
  - Unless the first recorded operation is filesort(). filesort() is unable 
    read result of a select, so when we find it first, the query plan is:

    filesort(first join table) -> Select(JOIN op) -> ...

  the other popular query plan is:

    Select (JOIN op) -> temp.table -> filesort() -> ...

///TODO: handle repeated execution with subselects!
*/

class Sort_and_group_tracker : public Sql_alloc
{
  enum { MAX_QEP_ACTIONS = 5 };

  /* Query actions in the order they were made. */
  enum_qep_action qep_actions[MAX_QEP_ACTIONS];
  
  /* Number for the next action */
  int cur_action;

  /*
    Non-zero means there was already an execution which had
    #total_actions actions
  */
  int total_actions;

  int get_n_actions()
  {
    return total_actions? total_actions: cur_action;
  }

  /*
    TRUE<=>there were executions which took different sort/buffer/de-duplicate
    routes. The counter values are not meaningful.
  */
  bool varied_executions;

  /* Details about query actions */
  union 
  {
    Filesort_tracker *filesort_tracker;
    enum_tmp_table_use tmp_table;
  } 
  qep_actions_data[MAX_QEP_ACTIONS];
  
  Filesort_tracker *dummy_fsort_tracker;
  bool is_analyze;
public:
  Sort_and_group_tracker(bool is_analyze_arg) :
    cur_action(0), total_actions(0), varied_executions(false),
    dummy_fsort_tracker(NULL),
    is_analyze(is_analyze_arg)
  {}

  /*************** Reporting interface ***************/
  /* Report that join execution is started */
  void report_join_start()
  {
    if (!total_actions && cur_action != 0)
    {
      /* This is a second execution */
      total_actions= cur_action;
    }
    cur_action= 0;
  }

  /* 
    Report that a temporary table is created. The next step is to write to the
    this tmp. table
  */
  void report_tmp_table(TABLE *tbl);

  /* 
    Report that we are doing a filesort. 
      @return 
        Tracker object to be used with filesort
  */
  Filesort_tracker *report_sorting(THD *thd);
  
  /*
    Report that remove_duplicates() is invoked [on a temp. table].
    We don't collect any statistics on this operation, yet.
  */
  void report_duplicate_removal();
  
  friend class Iterator;
  /*************** Statistics retrieval interface ***************/
  bool had_varied_executions() { return varied_executions; }

  class Iterator 
  {
    Sort_and_group_tracker *owner;
    int idx;
  public:
    Iterator(Sort_and_group_tracker *owner_arg) : 
      owner(owner_arg), idx(owner_arg->get_n_actions() - 1)
    {}

    enum_qep_action get_next(Filesort_tracker **tracker/*,
                             enum_tmp_table_use *tmp_table_use*/)
    {
      /* Walk back through the array... */
      if (idx < 0)
        return EXPL_ACTION_EOF;
      switch (owner->qep_actions[idx])
      {
        case EXPL_ACTION_FILESORT:
          *tracker= owner->qep_actions_data[idx].filesort_tracker;
          break;
        case EXPL_ACTION_TEMPTABLE:
          //*tmp_table_use= tmp_table_kind[tmp_table_idx++];
          break;
        default:
          break;
      }
      return owner->qep_actions[idx--];
    }

    bool is_last_element() { return idx == -1; }
  };
};

