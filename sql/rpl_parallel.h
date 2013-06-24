#ifndef RPL_PARALLEL_H
#define RPL_PARALLEL_H

#include "log_event.h"


struct rpl_parallel;
struct rpl_parallel_entry;
struct rpl_parallel_thread_pool;

class Relay_log_info;
struct rpl_parallel_thread {
  bool delay_start;
  bool running;
  bool stop;
  bool free;
  mysql_mutex_t LOCK_rpl_thread;
  mysql_cond_t COND_rpl_thread;
  struct rpl_parallel_thread *next;             /* For free list. */
  struct rpl_parallel_thread_pool *pool;
  THD *thd;
  struct rpl_parallel_entry *current_entry;
  struct queued_event {
    queued_event *next;
    Log_event *ev;
    Relay_log_info *rli;
  } *event_queue, *last_in_queue;
  rpl_parallel_thread *wait_for; /* ToDo: change this ... */
};


struct rpl_parallel_thread_pool {
  uint32 count;
  struct rpl_parallel_thread **threads;
  struct rpl_parallel_thread *free_list;
  mysql_mutex_t LOCK_rpl_thread_pool;
  mysql_cond_t COND_rpl_thread_pool;
  bool changing;
  bool inited;

  rpl_parallel_thread_pool();
  int init(uint32 size);
  void destroy();
  struct rpl_parallel_thread *get_thread(rpl_parallel_entry *entry);
};


struct rpl_parallel_entry {
  uint32 domain_id;
  uint32 last_server_id;
  uint64 last_seq_no;
  uint64 last_commit_id;
  bool active;
  rpl_parallel_thread *rpl_thread;
};
struct rpl_parallel {
  HASH domain_hash;
  rpl_parallel_entry *current;

  rpl_parallel();
  ~rpl_parallel();
  rpl_parallel_entry *find(uint32 domain_id);
  bool do_event(Relay_log_info *rli, Log_event *ev, THD *thd);
};


extern struct rpl_parallel_thread_pool global_rpl_thread_pool;


extern int rpl_parallel_change_thread_count(rpl_parallel_thread_pool *pool,
                                            uint32 new_count,
                                            bool skip_check= false);

#endif  /* RPL_PARALLEL_H */
