/* Copyright (c) 2005, 2017, Oracle and/or its affiliates.
   Copyright (c) 2009, 2017, MariaDB Corporation

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

#ifndef RPL_RLI_H
#define RPL_RLI_H

#include "rpl_tblmap.h"
#include "rpl_reporting.h"
#include "rpl_utility.h"
#include "log.h"                         /* LOG_INFO, MYSQL_BIN_LOG */
#include "sql_class.h"                   /* THD */
#include "log_event.h"
#include "rpl_parallel.h"

struct RPL_TABLE_LIST;
class Master_info;
class Rpl_filter;


/****************************************************************************

  Replication SQL Thread

  Relay_log_info contains:
    - the current relay log
    - the current relay log offset
    - master log name
    - master log sequence corresponding to the last update
    - misc information specific to the SQL thread

  Relay_log_info is initialized from the slave.info file if such
  exists.  Otherwise, data members are intialized with defaults. The
  initialization is done with Relay_log_info::init() call.

  The format of slave.info file:

  relay_log_name
  relay_log_pos
  master_log_name
  master_log_pos

  To clean up, call end_relay_log_info()

*****************************************************************************/

struct rpl_group_info;
struct inuse_relaylog;

class Relay_log_info : public Slave_reporting_capability
{
public:
  /**
     Flags for the state of reading the relay log. Note that these are
     bit masks.
  */
  enum enum_state_flag {
    /** We are inside a group of events forming a statement */
    IN_STMT=1,
    /** We have inside a transaction */
    IN_TRANSACTION=2
  };

  /*
    The SQL thread owns one Relay_log_info, and each client that has
    executed a BINLOG statement owns one Relay_log_info. This function
    returns zero for the Relay_log_info object that belongs to the SQL
    thread and nonzero for Relay_log_info objects that belong to
    clients.
  */
  inline bool belongs_to_client()
  {
    DBUG_ASSERT(sql_driver_thd);
    return !sql_driver_thd->slave_thread;
  }

  /*
    If true, events with the same server id should be replicated. This
    field is set on creation of a relay log info structure by copying
    the value of ::replicate_same_server_id and can be overridden if
    necessary. For example of when this is done, check sql_binlog.cc,
    where the BINLOG statement can be used to execute "raw" events.
   */
  bool replicate_same_server_id;

  /*** The following variables can only be read when protect by data lock ****/

  /*
    info_fd - file descriptor of the info file. set only during
    initialization or clean up - safe to read anytime
    cur_log_fd - file descriptor of the current read  relay log
  */
  File info_fd,cur_log_fd;

  /*
    Protected with internal locks.
    Must get data_lock when resetting the logs.
  */
  MYSQL_BIN_LOG relay_log;
  LOG_INFO linfo;

  /*
   cur_log
     Pointer that either points at relay_log.get_log_file() or
     &rli->cache_buf, depending on whether the log is hot or there was
     the need to open a cold relay_log.

   cache_buf 
     IO_CACHE used when opening cold relay logs.
   */
  IO_CACHE cache_buf,*cur_log;

  /*
    Keeps track of the number of transactions that commits
    before fsyncing. The option --sync-relay-log-info determines 
    how many transactions should commit before fsyncing.
  */ 
  uint sync_counter;

  /*
    Identifies when the recovery process is going on.
    See sql/slave.cc:init_recovery for further details.
  */ 
  bool is_relay_log_recovery;

  /* The following variables are safe to read any time */

  /* IO_CACHE of the info file - set only during init or end */
  IO_CACHE info_file;

  /*
    List of temporary tables used by this connection.
    This is updated when a temporary table is created or dropped by
    a replication thread.

    Not reset when replication ends, to allow one to access the tables
    when replication restarts.

    Protected by data_lock.
  */
  All_tmp_tables_list *save_temporary_tables;

  /*
    standard lock acquisition order to avoid deadlocks:
    run_lock, data_lock, relay_log.LOCK_log, relay_log.LOCK_index
  */
  mysql_mutex_t data_lock, run_lock;
  /*
    start_cond is broadcast when SQL thread is started
    stop_cond - when stopped
    data_cond - when data protected by data_lock changes
  */
  mysql_cond_t start_cond, stop_cond, data_cond;
  /* parent Master_info structure */
  Master_info *mi;

  /*
    List of active relay log files.
    (This can be more than one in case of parallel replication).
  */
  inuse_relaylog *inuse_relaylog_list;
  inuse_relaylog *last_inuse_relaylog;

  /*
    Needed to deal properly with cur_log getting closed and re-opened with
    a different log under our feet
  */
  uint32 cur_log_old_open_count;

  /*
    If on init_info() call error_on_rli_init_info is true that means
    that previous call to init_info() terminated with an error, RESET
    SLAVE must be executed and the problem fixed manually.
   */
  bool error_on_rli_init_info;

  /*
    Let's call a group (of events) :
      - a transaction
      or
      - an autocommiting query + its associated events (INSERT_ID,
    TIMESTAMP...)
    We need these rli coordinates :
    - relay log name and position of the beginning of the group we currently
    are executing. Needed to know where we have to restart when replication has
    stopped in the middle of a group (which has been rolled back by the slave).
    - relay log name and position just after the event we have just
    executed. This event is part of the current group.
    Formerly we only had the immediately above coordinates, plus a 'pending'
    variable, but this dealt wrong with the case of a transaction starting on a
    relay log and finishing (commiting) on another relay log. Case which can
    happen when, for example, the relay log gets rotated because of
    max_binlog_size.

    Note: group_relay_log_name, group_relay_log_pos must only be
    written from the thread owning the Relay_log_info (SQL thread if
    !belongs_to_client(); client thread executing BINLOG statement if
    belongs_to_client()).
  */
  char group_relay_log_name[FN_REFLEN];
  ulonglong group_relay_log_pos;
  char event_relay_log_name[FN_REFLEN];
  ulonglong event_relay_log_pos;
  ulonglong future_event_relay_log_pos;
  /*
    The master log name for current event. Only used in parallel replication.
  */
  char future_event_master_log_name[FN_REFLEN];

  /*
     Original log name and position of the group we're currently executing
     (whose coordinates are group_relay_log_name/pos in the relay log)
     in the master's binlog. These concern the *group*, because in the master's
     binlog the log_pos that comes with each event is the position of the
     beginning of the group.

    Note: group_master_log_name, group_master_log_pos must only be
    written from the thread owning the Relay_log_info (SQL thread if
    !belongs_to_client(); client thread executing BINLOG statement if
    belongs_to_client()).
  */
  char group_master_log_name[FN_REFLEN];
  volatile my_off_t group_master_log_pos;

  /*
    Handling of the relay_log_space_limit optional constraint.
    ignore_log_space_limit is used to resolve a deadlock between I/O and SQL
    threads, the SQL thread sets it to unblock the I/O thread and make it
    temporarily forget about the constraint.
  */
  ulonglong log_space_limit,log_space_total;
  bool ignore_log_space_limit;

  /*
    Used by the SQL thread to instructs the IO thread to rotate 
    the logs when the SQL thread needs to purge to release some
    disk space.
   */
  bool sql_force_rotate_relay;

  time_t last_master_timestamp;
  /*
    The SQL driver thread sets this true while it is waiting at the end of the
    relay log for more events to arrive. SHOW SLAVE STATUS uses this to report
    Seconds_Behind_Master as zero while the SQL thread is so waiting.
  */
  bool sql_thread_caught_up;

  void clear_until_condition();
  /**
    Reset the delay.
    This is used by RESET SLAVE to clear the delay.
  */
  void clear_sql_delay()
  {
    sql_delay= 0;
  }


  /*
    Needed for problems when slave stops and we want to restart it
    skipping one or more events in the master log that have caused
    errors, and have been manually applied by DBA already.
    Must be ulong as it's refered to from set_var.cc
  */
  volatile ulonglong slave_skip_counter;
  ulonglong max_relay_log_size;

  volatile ulong abort_pos_wait;	/* Incremented on change master */
  volatile ulong slave_run_id;		/* Incremented on slave start */
  mysql_mutex_t log_space_lock;
  mysql_cond_t log_space_cond;
  /*
    THD for the main sql thread, the one that starts threads to process
    slave requests. If there is only one thread, then this THD is also
    used for SQL processing.
    A kill sent to this THD will kill the replication.
  */
  THD *sql_driver_thd;
#ifndef DBUG_OFF
  int events_till_abort;
#endif  

  enum_gtid_skip_type gtid_skip_flag;

  /*
    inited changes its value within LOCK_active_mi-guarded critical
    sections  at times of start_slave_threads() (0->1) and end_slave() (1->0).
    Readers may not acquire the mutex while they realize potential concurrency
    issue.
    If not set, the value of other members of the structure are undefined.
  */
  volatile bool inited;
  volatile bool abort_slave;
  volatile bool stop_for_until;
  volatile uint slave_running;

  /* 
     Condition and its parameters from START SLAVE UNTIL clause.
     
     UNTIL condition is tested with is_until_satisfied() method that is 
     called by exec_relay_log_event(). is_until_satisfied() caches the result
     of the comparison of log names because log names don't change very often;
     this cache is invalidated by parts of code which change log names with
     notify_*_log_name_updated() methods. (They need to be called only if SQL
     thread is running).
   */
  
  enum {
    UNTIL_NONE= 0, UNTIL_MASTER_POS, UNTIL_RELAY_POS, UNTIL_GTID
  } until_condition;
  char until_log_name[FN_REFLEN];
  ulonglong until_log_pos;
  /* extension extracted from log_name and converted to int */
  ulong until_log_name_extension;   
  /* 
     Cached result of comparison of until_log_name and current log name
     -2 means unitialised, -1,0,1 are comarison results 
  */
  enum 
  { 
    UNTIL_LOG_NAMES_CMP_UNKNOWN= -2, UNTIL_LOG_NAMES_CMP_LESS= -1,
    UNTIL_LOG_NAMES_CMP_EQUAL= 0, UNTIL_LOG_NAMES_CMP_GREATER= 1
  } until_log_names_cmp_result;
  /* Condition for UNTIL master_gtid_pos. */
  slave_connection_state until_gtid_pos;

  /*
    retried_trans is a cumulative counter: how many times the slave
    has retried a transaction (any) since slave started.
    Protected by data_lock.
  */
  ulong retried_trans;
  /*
    Number of executed events for SLAVE STATUS.
    Protected by slave_executed_entries_lock
  */
  int64 executed_entries;

  /*
    If the end of the hot relay log is made of master's events ignored by the
    slave I/O thread, these two keep track of the coords (in the master's
    binlog) of the last of these events seen by the slave I/O thread. If not,
    ign_master_log_name_end[0] == 0.
    As they are like a Rotate event read/written from/to the relay log, they
    are both protected by rli->relay_log.LOCK_log.
  */
  char ign_master_log_name_end[FN_REFLEN];
  ulonglong ign_master_log_pos_end;
  /* Similar for ignored GTID events. */
  slave_connection_state ign_gtids;

  /* 
    Indentifies where the SQL Thread should create temporary files for the
    LOAD DATA INFILE. This is used for security reasons.
   */ 
  char slave_patternload_file[FN_REFLEN]; 
  size_t slave_patternload_file_size;  

  rpl_parallel parallel;
  /*
    The relay_log_state keeps track of the current binlog state of the
    execution of the relay log. This is used to know where to resume
    current GTID position if the slave thread is stopped and
    restarted.  It is only accessed from the SQL thread, so it does
    not need any locking.
  */
  rpl_binlog_state relay_log_state;
  /*
    The restart_gtid_state is used when the SQL thread restarts on a relay log
    in GTID mode. In multi-domain parallel replication, each domain may have a
    separat position, so some events in more progressed domains may need to be
    skipped. This keeps track of the domains that have not yet reached their
    starting event.
  */
  slave_connection_state restart_gtid_pos;

  Relay_log_info(bool is_slave_recovery);
  ~Relay_log_info();

  /*
    Invalidate cached until_log_name and group_relay_log_name comparison 
    result. Should be called after any update of group_realy_log_name if
    there chances that sql_thread is running.
  */
  inline void notify_group_relay_log_name_update()
  {
    if (until_condition==UNTIL_RELAY_POS)
      until_log_names_cmp_result= UNTIL_LOG_NAMES_CMP_UNKNOWN;
  }

  /*
    The same as previous but for group_master_log_name. 
  */
  inline void notify_group_master_log_name_update()
  {
    if (until_condition==UNTIL_MASTER_POS)
      until_log_names_cmp_result= UNTIL_LOG_NAMES_CMP_UNKNOWN;
  }

  void inc_group_relay_log_pos(ulonglong log_pos,
			       rpl_group_info *rgi,
			       bool skip_lock=0);

  int wait_for_pos(THD* thd, String* log_name, longlong log_pos, 
		   longlong timeout);
  void close_temporary_tables();

  /* Check if UNTIL condition is satisfied. See slave.cc for more. */
  bool is_until_satisfied(Log_event *ev);
  inline ulonglong until_pos()
  {
    DBUG_ASSERT(until_condition == UNTIL_MASTER_POS ||
                until_condition == UNTIL_RELAY_POS);
    return ((until_condition == UNTIL_MASTER_POS) ? group_master_log_pos :
	    group_relay_log_pos);
  }
  inline char *until_name()
  {
    DBUG_ASSERT(until_condition == UNTIL_MASTER_POS ||
                until_condition == UNTIL_RELAY_POS);
    return ((until_condition == UNTIL_MASTER_POS) ? group_master_log_name :
	    group_relay_log_name);
  }
  /**
    Helper function to do after statement completion.

    This function is called from an event to complete the group by
    either stepping the group position, if the "statement" is not
    inside a transaction; or increase the event position, if the
    "statement" is inside a transaction.

    @param event_log_pos
    Master log position of the event. The position is recorded in the
    relay log info and used to produce information for <code>SHOW
    SLAVE STATUS</code>.
  */
  bool stmt_done(my_off_t event_log_pos, THD *thd, rpl_group_info *rgi);
  int alloc_inuse_relaylog(const char *name);
  void free_inuse_relaylog(inuse_relaylog *ir);
  void reset_inuse_relaylog();
  int update_relay_log_state(rpl_gtid *gtid_list, uint32 count);

  /**
     Is the replication inside a group?

     The reader of the relay log is inside a group if either:
     - The IN_TRANSACTION flag is set, meaning we're inside a transaction
     - The IN_STMT flag is set, meaning we have read at least one row from
       a multi-event entry.

     This flag reflects the state of the log 'just now', ie after the last
     read event would be executed.
     This allow us to test if we can stop replication before reading
     the next entry.

     @retval true Replication thread is currently inside a group
     @retval false Replication thread is currently not inside a group
   */
  bool is_in_group() const {
    return (m_flags & (IN_STMT | IN_TRANSACTION));
  }

  /**
     Set the value of a replication state flag.

     @param flag Flag to set
   */
  void set_flag(enum_state_flag flag)
  {
    m_flags|= flag;
  }

  /**
     Get the value of a replication state flag.

     @param flag Flag to get value of

     @return @c true if the flag was set, @c false otherwise.
   */
  bool get_flag(enum_state_flag flag)
  {
    return m_flags & flag;
  }

  /**
     Clear the value of a replication state flag.

     @param flag Flag to clear
   */
  void clear_flag(enum_state_flag flag)
  {
    m_flags&= ~flag;
  }

  /**
    Text used in THD::proc_info when the slave SQL thread is delaying.
  */
  static const char *const state_delaying_string;

  bool flush();

  /**
    Reads the relay_log.info file.
  */
  int init(const char* info_filename);

  /**
    Indicate that a delay starts.

    This does not actually sleep; it only sets the state of this
    Relay_log_info object to delaying so that the correct state can be
    reported by SHOW SLAVE STATUS and SHOW PROCESSLIST.

    Requires rli->data_lock.

    @param delay_end The time when the delay shall end.
  */
  void start_sql_delay(time_t delay_end)
  {
    mysql_mutex_assert_owner(&data_lock);
    sql_delay_end= delay_end;
    thd_proc_info(sql_driver_thd, state_delaying_string);
  }

  int32 get_sql_delay() { return sql_delay; }
  void set_sql_delay(int32 _sql_delay) { sql_delay= _sql_delay; }
  time_t get_sql_delay_end() { return sql_delay_end; }

private:


  /**
    Delay slave SQL thread by this amount, compared to master (in
    seconds). This is set with CHANGE MASTER TO MASTER_DELAY=X.

    Guarded by data_lock.  Initialized by the client thread executing
    START SLAVE.  Written by client threads executing CHANGE MASTER TO
    MASTER_DELAY=X.  Read by SQL thread and by client threads
    executing SHOW SLAVE STATUS.  Note: must not be written while the
    slave SQL thread is running, since the SQL thread reads it without
    a lock when executing Relay_log_info::flush().
  */
  int sql_delay;

  /**
    During a delay, specifies the point in time when the delay ends.

    This is used for the SQL_Remaining_Delay column in SHOW SLAVE STATUS.

    Guarded by data_lock. Written by the sql thread.  Read by client
    threads executing SHOW SLAVE STATUS.
  */
  time_t sql_delay_end;

  /*
    Before the MASTER_DELAY parameter was added (WL#344),
    relay_log.info had 4 lines. Now it has 5 lines.
  */
  static const int LINES_IN_RELAY_LOG_INFO_WITH_DELAY= 5;
  /*
    Hint for when to stop event distribution by sql driver thread.
    The flag is set ON by a non-group event when this event is in the middle
    of a group (e.g a transaction group) so it's too early
    to refresh the current-relay-log vs until-log cached comparison result.
    And it is checked and to decide whether it's a right time to do so
    when the being processed group has been fully scheduled.
  */
  bool until_relay_log_names_defer;

  /*
    Holds the state of the data in the relay log.
    We need this to ensure that we are not in the middle of a
    statement or inside BEGIN ... COMMIT when should rotate the
    relay log.
  */
  uint32 m_flags;
};


/*
  In parallel replication, if we need to re-try a transaction due to a
  deadlock or other temporary error, we may need to go back and re-read events
  out of an earlier relay log.

  This structure keeps track of the relaylogs that are potentially in use.
  Each rpl_group_info has a pointer to one of those, corresponding to the
  first GTID event.

  A pair of reference count keeps track of how long a relay log is potentially
  in use. When the `completed' flag is set, all events have been read out of
  the relay log, but the log might still be needed for retry in worker
  threads.  As worker threads complete an event group, they increment
  atomically the `dequeued_count' with number of events queued. Thus, when
  completed is set and dequeued_count equals queued_count, the relay log file
  is finally done with and can be purged.

  By separating the queued and dequeued count, only the dequeued_count needs
  multi-thread synchronisation; the completed flag and queued_count fields
  are only accessed by the SQL driver thread and need no synchronisation.
*/
struct inuse_relaylog {
  inuse_relaylog *next;
  Relay_log_info *rli;
  /*
    relay_log_state holds the binlog state corresponding to the start of this
    relay log file. It is an array with relay_log_state_count elements.
  */
  rpl_gtid *relay_log_state;
  uint32 relay_log_state_count;
  /* Number of events in this relay log queued for worker threads. */
  int64 queued_count;
  /* Number of events completed by worker threads. */
  volatile int64 dequeued_count;
  /* Set when all events have been read from a relaylog. */
  bool completed;
  char name[FN_REFLEN];
};


/*
  This is data for various state needed to be kept for the processing of
  one event group (transaction) during replication.

  In single-threaded replication, there will be one global rpl_group_info and
  one global Relay_log_info per master connection. They will be linked
  together.

  In parallel replication, there will be one rpl_group_info object for
  each running sql thread, each having their own thd.

  All rpl_group_info will share the same Relay_log_info.
*/

struct rpl_group_info
{
  rpl_group_info *next;             /* For free list in rpl_parallel_thread */
  Relay_log_info *rli;
  THD *thd;
  /*
    Current GTID being processed.
    The sub_id gives the binlog order within one domain_id. A zero sub_id
    means that there is no active GTID.
  */
  uint64 gtid_sub_id;
  rpl_gtid current_gtid;
  uint64 commit_id;
  /*
    This is used to keep transaction commit order.
    We will signal this when we commit, and can register it to wait for the
    commit_orderer of the previous commit to signal us.
  */
  wait_for_commit commit_orderer;
  /*
    If non-zero, the sub_id of a prior event group whose commit we have to wait
    for before committing ourselves. Then wait_commit_group_info points to the
    event group to wait for.

    Before using this, rpl_parallel_entry::last_committed_sub_id should be
    compared against wait_commit_sub_id. Only if last_committed_sub_id is
    smaller than wait_commit_sub_id must the wait be done (otherwise the
    waited-for transaction is already committed, so we would otherwise wait
    for the wrong commit).
  */
  uint64 wait_commit_sub_id;
  rpl_group_info *wait_commit_group_info;
  /*
    This holds a pointer to a struct that keeps track of the need to wait
    for the previous batch of event groups to reach the commit stage, before
    this batch can start to execute.

    (When we execute in parallel the transactions that group committed
    together on the master, we still need to wait for any prior transactions
    to have reached the commit stage).

    The pointed-to gco is only valid for as long as
    gtid_sub_id < parallel_entry->last_committed_sub_id. After that, it can
    be freed by another thread.
  */
  group_commit_orderer *gco;

  struct rpl_parallel_entry *parallel_entry;

  /*
    A container to hold on Intvar-, Rand-, Uservar- log-events in case
    the slave is configured with table filtering rules.
    The withhold events are executed when their parent Query destiny is
    determined for execution as well.
  */
  Deferred_log_events *deferred_events;

  /*
    State of the container: true stands for IRU events gathering, 
    false does for execution, either deferred or direct.
  */
  bool deferred_events_collecting;

  Annotate_rows_log_event *m_annotate_event;

  RPL_TABLE_LIST *tables_to_lock;           /* RBR: Tables to lock  */
  uint tables_to_lock_count;        /* RBR: Count of tables to lock */
  table_mapping m_table_map;      /* RBR: Mapping table-id to table */
  mysql_mutex_t sleep_lock;
  mysql_cond_t sleep_cond;

  /*
    trans_retries varies between 0 to slave_transaction_retries and counts how
    many times the slave has retried the present transaction; gets reset to 0
    when the transaction finally succeeds.
  */
  ulong trans_retries;

  /*
    Used to defer stopping the SQL thread to give it a chance
    to finish up the current group of events.
    The timestamp is set and reset in @c sql_slave_killed().
  */
  time_t last_event_start_time;

  char *event_relay_log_name;
  char event_relay_log_name_buf[FN_REFLEN];
  ulonglong event_relay_log_pos;
  ulonglong future_event_relay_log_pos;
  /*
    The master log name for current event. Only used in parallel replication.
  */
  char future_event_master_log_name[FN_REFLEN];
  bool is_parallel_exec;
  /* When gtid_pending is true, we have not yet done record_gtid(). */
  bool gtid_pending;
  int worker_error;
  /*
    Set true when we signalled that we reach the commit phase. Used to avoid
    counting one event group twice.
  */
  bool did_mark_start_commit;
  /* Copy of flags2 from GTID event. */
  uchar gtid_ev_flags2;
  enum {
    GTID_DUPLICATE_NULL=0,
    GTID_DUPLICATE_IGNORE=1,
    GTID_DUPLICATE_OWNER=2
  };
  /*
    When --gtid-ignore-duplicates, this is set to one of the above three
    values:
    GTID_DUPLICATE_NULL    - Not using --gtid-ignore-duplicates.
    GTID_DUPLICATE_IGNORE  - This gtid already applied, skip the event group.
    GTID_DUPLICATE_OWNER   - We are the current owner of the domain, and must
                             apply the event group and then release the domain.
  */
  uint8 gtid_ignore_duplicate_state;

  /*
    Runtime state for printing a note when slave is taking
    too long while processing a row event.
   */
  time_t row_stmt_start_timestamp;
  bool long_find_row_note_printed;
  /* Needs room for "Gtid D-S-N\x00". */
  char gtid_info_buf[5+10+1+10+1+20+1];

  /* List of not yet committed deletions in mysql.gtid_slave_pos. */
  rpl_slave_state::list_element *pending_gtid_delete_list;
  /* Domain associated with pending_gtid_delete_list. */
  uint32 pending_gtid_delete_list_domain;

  /*
    The timestamp, from the master, of the commit event.
    Used to do delayed update of rli->last_master_timestamp, for getting
    reasonable values out of Seconds_Behind_Master in SHOW SLAVE STATUS.
  */
  time_t last_master_timestamp;

  /*
    Information to be able to re-try an event group in case of a deadlock or
    other temporary error.
  */
  inuse_relaylog *relay_log;
  uint64 retry_start_offset;
  uint64 retry_event_count;
  /*
    If `speculation' is != SPECULATE_NO, then we are optimistically running
    this transaction in parallel, even though it might not be safe (there may
    be a conflict with a prior event group).

    In this case, a conflict can cause other errors than deadlocks (like
    duplicate key for example). So in case of _any_ error, we need to roll
    back and retry the event group.
  */
  enum enum_speculation {
    /*
      This transaction was group-committed together on the master with the
      other transactions with which it is replicated in parallel.
    */
    SPECULATE_NO,
    /*
      We will optimistically try to run this transaction in parallel with
      other transactions, even though it is not known to be conflict free.
      If we get a conflict, we will detect it as a deadlock, roll back and
      retry.
    */
    SPECULATE_OPTIMISTIC,
    /*
      This transaction got a conflict during speculative parallel apply, or
      it was marked on the master as likely to cause a conflict or unsafe to
      speculate. So it will wait for the prior transaction to commit before
      starting to replicate.
    */
    SPECULATE_WAIT
  } speculation;
  enum enum_retry_killed {
    RETRY_KILL_NONE = 0,
    RETRY_KILL_PENDING,
    RETRY_KILL_KILLED
  };
  uchar killed_for_retry;

  rpl_group_info(Relay_log_info *rli_);
  ~rpl_group_info();
  void reinit(Relay_log_info *rli);

  /* 
     Returns true if the argument event resides in the containter;
     more specifically, the checking is done against the last added event.
  */
  bool is_deferred_event(Log_event * ev)
  {
    return deferred_events_collecting ? deferred_events->is_last(ev) : false;
  };
  /* The general cleanup that slave applier may need at the end of query. */
  inline void cleanup_after_query()
  {
    if (deferred_events)
      deferred_events->rewind();
  };
  /* The general cleanup that slave applier may need at the end of session. */
  void cleanup_after_session()
  {
    if (deferred_events)
    {
      delete deferred_events;
      deferred_events= NULL;
    }
  };

  /**
    Save pointer to Annotate_rows event and switch on the
    binlog_annotate_row_events for this sql thread.
    To be called when sql thread receives an Annotate_rows event.
  */
  inline void set_annotate_event(Annotate_rows_log_event *event)
  {
    DBUG_ASSERT(m_annotate_event == NULL);
    m_annotate_event= event;
    this->thd->variables.binlog_annotate_row_events= 1;
  }

  /**
    Returns pointer to the saved Annotate_rows event or NULL if there is
    no saved event.
  */
  inline Annotate_rows_log_event* get_annotate_event()
  {
    return m_annotate_event;
  }

  /**
    Delete saved Annotate_rows event (if any) and switch off the
    binlog_annotate_row_events for this sql thread.
    To be called when sql thread has applied the last (i.e. with
    STMT_END_F flag) rbr event.
  */
  inline void free_annotate_event()
  {
    if (m_annotate_event)
    {
      this->thd->variables.binlog_annotate_row_events= 0;
      delete m_annotate_event;
      m_annotate_event= 0;
    }
  }

  bool get_table_data(TABLE *table_arg, table_def **tabledef_var, TABLE **conv_table_var) const
  {
    DBUG_ASSERT(tabledef_var && conv_table_var);
    for (TABLE_LIST *ptr= tables_to_lock ; ptr != NULL ; ptr= ptr->next_global)
      if (ptr->table == table_arg)
      {
        *tabledef_var= &static_cast<RPL_TABLE_LIST*>(ptr)->m_tabledef;
        *conv_table_var= static_cast<RPL_TABLE_LIST*>(ptr)->m_conv_table;
        DBUG_PRINT("debug", ("Fetching table data for table %s.%s:"
                             " tabledef: %p, conv_table: %p",
                             table_arg->s->db.str, table_arg->s->table_name.str,
                             *tabledef_var, *conv_table_var));
        return true;
      }
    return false;
  }

  void clear_tables_to_lock();
  void cleanup_context(THD *, bool);
  void slave_close_thread_tables(THD *);
  void mark_start_commit_no_lock();
  void mark_start_commit();
  char *gtid_info();
  void unmark_start_commit();

  static void pending_gtid_deletes_free(rpl_slave_state::list_element *list);
  void pending_gtid_deletes_save(uint32 domain_id,
                                 rpl_slave_state::list_element *list);
  void pending_gtid_deletes_put_back();
  void pending_gtid_deletes_clear();

  time_t get_row_stmt_start_timestamp()
  {
    return row_stmt_start_timestamp;
  }

  time_t set_row_stmt_start_timestamp()
  {
    if (row_stmt_start_timestamp == 0)
      row_stmt_start_timestamp= my_time(0);

    return row_stmt_start_timestamp;
  }

  void reset_row_stmt_start_timestamp()
  {
    row_stmt_start_timestamp= 0;
  }

  void set_long_find_row_note_printed()
  {
    long_find_row_note_printed= true;
  }

  void unset_long_find_row_note_printed()
  {
    long_find_row_note_printed= false;
  }

  bool is_long_find_row_note_printed()
  {
    return long_find_row_note_printed;
  }

  inline void inc_event_relay_log_pos()
  {
    if (!is_parallel_exec)
      rli->event_relay_log_pos= future_event_relay_log_pos;
  }
};


/*
  The class rpl_sql_thread_info is the THD::system_thread_info for an SQL
  thread; this is either the driver SQL thread or a worker thread for parallel
  replication.
*/
class rpl_sql_thread_info
{
public:
  char cached_charset[6];
  Rpl_filter* rpl_filter;

  rpl_sql_thread_info(Rpl_filter *filter);

  /*
    Last charset (6 bytes) seen by slave SQL thread is cached here; it helps
    the thread save 3 get_charset() per Query_log_event if the charset is not
    changing from event to event (common situation).
    When the 6 bytes are equal to 0 is used to mean "cache is invalidated".
  */
  void cached_charset_invalidate();
  bool cached_charset_compare(char *charset) const;
};


extern struct rpl_slave_state *rpl_global_gtid_slave_state;
extern gtid_waiting rpl_global_gtid_waiting;

int rpl_load_gtid_slave_state(THD *thd);
int event_group_new_gtid(rpl_group_info *rgi, Gtid_log_event *gev);
void delete_or_keep_event_post_apply(rpl_group_info *rgi,
                                     Log_event_type typ, Log_event *ev);

#endif /* RPL_RLI_H */
