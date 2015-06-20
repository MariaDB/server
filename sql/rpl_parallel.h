#ifndef RPL_PARALLEL_H
#define RPL_PARALLEL_H

#include "log_event.h"


struct rpl_parallel;
struct rpl_parallel_entry;
struct rpl_parallel_thread_pool;

class Relay_log_info;
struct inuse_relaylog;


/*
  Structure used to keep track of the parallel replication of a batch of
  event-groups that group-committed together on the master.

  It is used to ensure that every event group in one batch has reached the
  commit stage before the next batch starts executing.

  Note the lifetime of this structure:

   - It is allocated when the first event in a new batch of group commits
     is queued, from the free list rpl_parallel_entry::gco_free_list.

   - The gco for the batch currently being queued is owned by
     rpl_parallel_entry::current_gco. The gco for a previous batch that has
     been fully queued is owned by the gco->prev_gco pointer of the gco for
     the following batch.

   - The worker thread waits on gco->COND_group_commit_orderer for
     rpl_parallel_entry::count_committing_event_groups to reach wait_count
     before starting; the first waiter links the gco into the next_gco
     pointer of the gco of the previous batch for signalling.

   - When an event group reaches the commit stage, it signals the
     COND_group_commit_orderer if its gco->next_gco pointer is non-NULL and
     rpl_parallel_entry::count_committing_event_groups has reached
     gco->next_gco->wait_count.

   - The gco lives until all its event groups have completed their commit.
     This is detected by rpl_parallel_entry::last_committed_sub_id being
     greater than or equal gco->last_sub_id. Once this happens, the gco is
     freed. Note that since update of last_committed_sub_id can happen
     out-of-order, the thread that frees a given gco can be for any later
     event group, not necessarily an event group from the gco being freed.
*/
struct group_commit_orderer {
  /* Wakeup condition, used with rpl_parallel_entry::LOCK_parallel_entry. */
  mysql_cond_t COND_group_commit_orderer;
  uint64 wait_count;
  group_commit_orderer *prev_gco;
  group_commit_orderer *next_gco;
  /*
    The sub_id of last event group in the previous GCO.
    Only valid if prev_gco != NULL.
  */
  uint64 prior_sub_id;
  /*
    The sub_id of the last event group in this GCO. Only valid when next_gco
    is non-NULL.
  */
  uint64 last_sub_id;
  bool installed;
};


struct rpl_parallel_thread {
  bool delay_start;
  bool running;
  bool stop;
  mysql_mutex_t LOCK_rpl_thread;
  mysql_cond_t COND_rpl_thread;
  mysql_cond_t COND_rpl_thread_queue;
  struct rpl_parallel_thread *next;             /* For free list. */
  struct rpl_parallel_thread_pool *pool;
  THD *thd;
  /*
    Who owns the thread, if any (it's a pointer into the
    rpl_parallel_entry::rpl_threads array.
  */
  struct rpl_parallel_thread **current_owner;
  /* The rpl_parallel_entry of the owner. */
  rpl_parallel_entry *current_entry;
  struct queued_event {
    queued_event *next;
    /*
      queued_event can hold either an event to be executed, or just a binlog
      position to be updated without any associated event.
    */
    enum queued_event_t {
      QUEUED_EVENT,
      QUEUED_POS_UPDATE,
      QUEUED_MASTER_RESTART
    } typ;
    union {
      Log_event *ev;                            /* QUEUED_EVENT */
      rpl_parallel_entry *entry_for_queued;     /* QUEUED_POS_UPDATE and
                                                   QUEUED_MASTER_RESTART */
    };
    rpl_group_info *rgi;
    inuse_relaylog *ir;
    ulonglong future_event_relay_log_pos;
    char event_relay_log_name[FN_REFLEN];
    char future_event_master_log_name[FN_REFLEN];
    ulonglong event_relay_log_pos;
    my_off_t future_event_master_log_pos;
    size_t event_size;
  } *event_queue, *last_in_queue;
  uint64 queued_size;
  /* These free lists are protected by LOCK_rpl_thread. */
  queued_event *qev_free_list;
  rpl_group_info *rgi_free_list;
  group_commit_orderer *gco_free_list;
  /*
    These free lists are local to the thread, so need not be protected by any
    lock. They are moved to the global free lists in batches in the function
    batch_free(), to reduce LOCK_rpl_thread contention.

    The lists are not NULL-terminated (as we do not need to traverse them).
    Instead, if they are non-NULL, the loc_XXX_last_ptr_ptr points to the
    `next' pointer of the last element, which is used to link into the front
    of the global freelists.
  */
  queued_event *loc_qev_list, **loc_qev_last_ptr_ptr;
  size_t loc_qev_size;
  uint64 qev_free_pending;
  rpl_group_info *loc_rgi_list, **loc_rgi_last_ptr_ptr;
  group_commit_orderer *loc_gco_list, **loc_gco_last_ptr_ptr;
  /* These keep track of batch update of inuse_relaylog refcounts. */
  inuse_relaylog *accumulated_ir_last;
  uint64 accumulated_ir_count;

  void enqueue(queued_event *qev)
  {
    if (last_in_queue)
      last_in_queue->next= qev;
    else
      event_queue= qev;
    last_in_queue= qev;
    queued_size+= qev->event_size;
  }

  void dequeue1(queued_event *list)
  {
    DBUG_ASSERT(list == event_queue);
    event_queue= last_in_queue= NULL;
  }

  void dequeue2(size_t dequeue_size)
  {
    queued_size-= dequeue_size;
  }

  queued_event *get_qev_common(Log_event *ev, ulonglong event_size);
  queued_event *get_qev(Log_event *ev, ulonglong event_size,
                        Relay_log_info *rli);
  queued_event *retry_get_qev(Log_event *ev, queued_event *orig_qev,
                              const char *relay_log_name,
                              ulonglong event_pos, ulonglong event_size);
  /*
    Put a qev on the local free list, to be later released to the global free
    list by batch_free().
  */
  void loc_free_qev(queued_event *qev);
  /*
    Release an rgi immediately to the global free list. Requires holding the
    LOCK_rpl_thread mutex.
  */
  void free_qev(queued_event *qev);
  rpl_group_info *get_rgi(Relay_log_info *rli, Gtid_log_event *gtid_ev,
                          rpl_parallel_entry *e, ulonglong event_size);
  /*
    Put an gco on the local free list, to be later released to the global free
    list by batch_free().
  */
  void loc_free_rgi(rpl_group_info *rgi);
  /*
    Release an rgi immediately to the global free list. Requires holding the
    LOCK_rpl_thread mutex.
  */
  void free_rgi(rpl_group_info *rgi);
  group_commit_orderer *get_gco(uint64 wait_count, group_commit_orderer *prev,
                                uint64 first_sub_id);
  /*
    Put a gco on the local free list, to be later released to the global free
    list by batch_free().
  */
  void loc_free_gco(group_commit_orderer *gco);
  /*
    Move all local free lists to the global ones. Requires holding
    LOCK_rpl_thread.
  */
  void batch_free();
  /* Update inuse_relaylog refcounts with what we have accumulated so far. */
  void inuse_relaylog_refcount_update();
};


struct rpl_parallel_thread_pool {
  uint32 count;
  struct rpl_parallel_thread **threads;
  struct rpl_parallel_thread *free_list;
  mysql_mutex_t LOCK_rpl_thread_pool;
  mysql_cond_t COND_rpl_thread_pool;
  bool inited;

  rpl_parallel_thread_pool();
  int init(uint32 size);
  void destroy();
  struct rpl_parallel_thread *get_thread(rpl_parallel_thread **owner,
                                         rpl_parallel_entry *entry);
  void release_thread(rpl_parallel_thread *rpt);
};


struct rpl_parallel_entry {
  mysql_mutex_t LOCK_parallel_entry;
  mysql_cond_t COND_parallel_entry;
  uint32 domain_id;
  uint64 last_commit_id;
  bool active;
  /*
    Set when SQL thread is shutting down, and no more events can be processed,
    so worker threads must force abort any current transactions without
    waiting for event groups to complete.
  */
  bool force_abort;
  /*
    Set in wait_for_workers_idle() to show that it is waiting, so that
    finish_event_group knows to signal it when last_committed_sub_id is
    increased.
  */
  bool need_sub_id_signal;
  /*
   At STOP SLAVE (force_abort=true), we do not want to process all events in
   the queue (which could unnecessarily delay stop, if a lot of events happen
   to be queued). The stop_count provides a safe point at which to stop, so
   that everything before becomes committed and nothing after does. The value
   corresponds to group_commit_orderer::wait_count; if wait_count is less than
   or equal to stop_count, we execute the associated event group, else we
   skip it (and all following) and stop.
  */
  uint64 stop_count;

  /*
    Cyclic array recording the last rpl_thread_max worker threads that we
    queued event for. This is used to limit how many workers a single domain
    can occupy (--slave-domain-parallel-threads).

    Note that workers are never explicitly deleted from the array. Instead,
    we need to check (under LOCK_rpl_thread) that the thread still belongs
    to us before re-using (rpl_thread::current_owner).
  */
  rpl_parallel_thread **rpl_threads;
  uint32 rpl_thread_max;
  uint32 rpl_thread_idx;
  /*
    The sub_id of the last transaction to commit within this domain_id.
    Must be accessed under LOCK_parallel_entry protection.

    Event groups commit in order, so the rpl_group_info for an event group
    will be alive (at least) as long as
    rpl_group_info::gtid_sub_id > last_committed_sub_id. This can be used to
    safely refer back to previous event groups if they are still executing,
    and ignore them if they completed, without requiring explicit
    synchronisation between the threads.
  */
  uint64 last_committed_sub_id;
  /*
    The sub_id of the last event group in this replication domain that was
    queued for execution by a worker thread.
  */
  uint64 current_sub_id;
  rpl_group_info *current_group_info;
  /*
    If we get an error in some event group, we set the sub_id of that event
    group here. Then later event groups (with higher sub_id) can know not to
    try to start (event groups that already started will be rolled back when
    wait_for_prior_commit() returns error).
    The value is ULONGLONG_MAX when no error occured.
  */
  uint64 stop_on_error_sub_id;
  /* Total count of event groups queued so far. */
  uint64 count_queued_event_groups;
  /*
    Count of event groups that have started (but not necessarily completed)
    the commit phase. We use this to know when every event group in a previous
    batch of master group commits have started committing on the slave, so
    that it is safe to start executing the events in the following batch.
  */
  uint64 count_committing_event_groups;
  /* The group_commit_orderer object for the events currently being queued. */
  group_commit_orderer *current_gco;

  rpl_parallel_thread * choose_thread(rpl_group_info *rgi, bool *did_enter_cond,
                                      PSI_stage_info *old_stage, bool reuse);
  int queue_master_restart(rpl_group_info *rgi,
                           Format_description_log_event *fdev);
};
struct rpl_parallel {
  HASH domain_hash;
  rpl_parallel_entry *current;
  bool sql_thread_stopping;

  rpl_parallel();
  ~rpl_parallel();
  void reset();
  rpl_parallel_entry *find(uint32 domain_id);
  void wait_for_done(THD *thd, Relay_log_info *rli);
  void stop_during_until();
  bool workers_idle();
  int wait_for_workers_idle(THD *thd);
  int do_event(rpl_group_info *serial_rgi, Log_event *ev, ulonglong event_size);
};


extern struct rpl_parallel_thread_pool global_rpl_thread_pool;


extern int rpl_parallel_activate_pool(rpl_parallel_thread_pool *pool);
extern int rpl_parallel_inactivate_pool(rpl_parallel_thread_pool *pool);
extern bool process_gtid_for_restart_pos(Relay_log_info *rli, rpl_gtid *gtid);

#endif  /* RPL_PARALLEL_H */
