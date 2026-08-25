/**/
#ifndef SQL_PARALLEL_THREAD_H
#define SQL_PARALLEL_THREAD_H

#include "mariadb.h"
#include "sql_class.h"
#include "mysqld.h"
#include "sql_error.h"

// PWT Parallel Worker Thread


/*
  Message Types
*/
class pwt_error_message
{
public:
  uint worker_errno;
  uint code;
  Sql_condition::enum_warning_level level;
  char *message;
};

/*
  Event type. Inherits ilink so it can live in an I_List<pwt_queued_event>.
*/
class pwt_queued_event : public ilink
{
public:
    pwt_error_message   *error;
};

class pwt_worker;

class pwt_manager_base : public Sql_alloc
{
protected:
  pwt_worker               *workers;

public:
  pwt_manager_base();
  ~pwt_manager_base();

  THD                      *thd; /* Manager thread */
  uint                     nworkers;

  void record_event(pwt_queued_event *event)
  {
    mysql_mutex_lock(&LOCK_pwt_manager);
    parallel_messages.push_back(event);
    mysql_mutex_unlock(&LOCK_pwt_manager);
  }
  void notify_message_dropped();

  void discard_pending_warnings();      // called on initialization failure
  void process_pending_warnings();      // called at end of normal execution

private:
  mysql_mutex_t            LOCK_pwt_manager;
  I_List<pwt_queued_event> parallel_messages;

  /*
      Set under LOCK_pwt_manager when a worker fails to allocate a queued event.
    The manager surfaces a single ER_OUTOFMEMORY warning so the user sees
    that worker diagnostics were dropped instead of silently disappearing.
  */
  bool                     messages_dropped;

protected: // TODO: can we isolate the following two?
  /*
    Set once the workers have been stopped and pthread_join'd (quiesce_workers).
    Workers read this join's source tables (via their private handlers), so
    they must be reaped before JOIN::join_free()->cleanup() frees those tables;
    quiesce_workers is called from join_free, and again (idempotently) from
    finalize.
  */
  bool                     reaped;
  /*
    Set (under LOCK_data) to a worker's killed_state when that worker exits
    because it was killed -- e.g. a user KILL [QUERY] aimed at a parallel
    worker. The consumer propagates it to the manager's own THD so the join
    aborts with the right error (ER_QUERY_INTERRUPTED) before any result is
    sent, rather than completing and trying to raise the error too late.
  */
  killed_state             kill_signal;
};

#define WORKER_NAME                    "Parallel Worker"
#define WORKER_ID_LENGTH               3
#define WORKER_NAME_LENGTH             15
#define CONNECTION_NAME_THREAD         "For Thread ID"
#define CONNECTION_NAME_THREAD_LENGTH  13
#define THREAD_ID_LENGTH               20         // ull can occupy 20 chars

struct pwt_worker_info
{
  char            conn_name[MAX_THREAD_NAME+1];
  /*
    This is displayed in information_schema.processlist.info
    Currently "Parallel Worker {1..N} For Thread M"
  */
  char            process_list[WORKER_NAME_LENGTH+
                               1+WORKER_ID_LENGTH+1+
                               CONNECTION_NAME_THREAD_LENGTH+
                               1+THREAD_ID_LENGTH+1];
};


class pwt_manager;

class pwt_worker_base : public Sql_alloc
{
public:
  /*
    Intialize the worker and create its THD.
    (this is called from the master thread)
  */
  bool init_worker_thd(pwt_manager *manager_arg, THD *parent_thd,
                       int worker_nr);
  bool create_thread();

  /* This is like a destructor. Called after worker is done. */
  void cleanup_worker();
  virtual ~pwt_worker_base() {}

  THD             *thd;
  pwt_manager     *manager;
  pthread_t       pthread;
  /*
    Guards worker->thd while the worker nulls it on exit, so abort_worker()
    sees either a live THD to awake() or nullptr.
    See parallel_worker_thread_func.
  */
  mysql_mutex_t   LOCK_worker;

  void init_and_run_thread_func();
  virtual void thread_func()= 0;

private:
  pwt_worker_info       info; // Connection/thread name

  friend void *pwt_worker_base_thread_func(void *arg);
};


#endif 
