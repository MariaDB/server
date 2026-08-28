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

/*
  This is a manager for worker threads.

  One can create worker threads (inherited from pwt_worker_base).
  - Worker threads will have the minimum THD environment
  - They will be visible in the INFORMATION_SCHEMA.PROCESSLIST
  - Warnings/errors emitted in the worker thread are recorded and can be
    replayed in their manager using process_pending_warnings().

  Not included here are:
  - pwt_manager_base doesn't keep track of the child threads.
  - We don't record if there was a fatal error, it just calls
      virtual pwt_worker_base->on_fatal_error().
  - Implementation of "error in a worker causes all others to stop"
    is also out of scope.
*/

class pwt_manager_base : public Sql_alloc
{
public:
  pwt_manager_base();
  ~pwt_manager_base();

  THD                      *thd; /* Manager thread */

  void process_pending_warnings(bool skip_interrupted);

  // These are used by class PWT_error_handler:
  void record_event(pwt_queued_event *event)
  {
    mysql_mutex_lock(&LOCK_pwt_manager);
    parallel_messages.push_back(event);
    mysql_mutex_unlock(&LOCK_pwt_manager);
  }
  void notify_message_dropped();

private:
  mysql_mutex_t            LOCK_pwt_manager;
  I_List<pwt_queued_event> parallel_messages;

  /*
    Set under LOCK_pwt_manager when a worker fails to allocate a queued event.
    The manager surfaces a single ER_OUTOFMEMORY warning so the user sees
    that worker diagnostics were dropped instead of silently disappearing.
  */
  bool                     messages_dropped;
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
  char            process_list[WORKER_NAME_LENGTH + 1 +
                               WORKER_ID_LENGTH   + 1 +
                               CONNECTION_NAME_THREAD_LENGTH + 1 +
                               THREAD_ID_LENGTH + 1];
};


/*
  Inherit from this your worker threads.
*/
class pwt_worker_base : public Sql_alloc
{
public:
  /*
    A team is built as an array and taken apart again from whatever stage the
    build reached, so a worker has to be able to say what it holds before
    anything has been done to it.
  */
  pwt_worker_base(pwt_manager_base *manager_arg):
    thd(nullptr), manager_base(manager_arg), inited(false),
    thread_started(false)
  {}
  virtual ~pwt_worker_base() {}

  /* Intialize the worker and create its THD (call from master thread) */
  bool init_worker_thd(THD *parent_thd, int worker_nr);

  /* Create and run the thread */
  bool create_thread();

  /*
    This will be run by the worker thread with all environment properly set up.
    One can produce warnings/errors and get them in the master thread.
  */
  virtual void thread_func()= 0;

  /* This will be invoked when a fatal error occurs */
  virtual void on_fatal_error() = 0;

  /*
    Release whatever thread_func() releases at the end of a run, for a worker
    whose thread never ran and so never got to do it itself.
  */
  virtual void cleanup_without_run() {}

  /*
    @brief
      Release this worker: everything init_worker_thd() and create_thread()
      gave it, from whatever stage its startup reached.

    @description
      The worker knows which stage that was, so the caller does not have to:
      one that was never initialised, one holding a THD but no thread, and one
      with a thread running are all released by the same call. It is also
      idempotent, so unwinding a half-built team is a loop over every worker
      rather than a loop that has to be bounded by how far the build got.

      reap_worker() waits for a running thread to finish on its own, which is
      the normal end of a query -- a worker that stops because it was asked to
      raises no error. abort_worker() tells it to stop first, and it exits with
      ER_QUERY_INTERRUPTED. That difference is the caller's to make, which is
      why it is two names and not a parameter.
  */
  void reap_worker()  { release_worker(false); }
  void abort_worker() { release_worker(true); }

  THD             *thd;
private:
  pwt_manager_base     *manager_base;

  /*
    What this worker holds, and so what release_worker() has to give back.
    'inited' says init_worker_thd() ran to the end: there is a THD registered
    in server_threads and LOCK_worker is live. 'thread_started' says
    create_thread() did too, which changes who destroys the THD -- a worker
    that ran destroys its own on the way out.
  */
  bool            inited;
  bool            thread_started;

  /*
    Guards worker->thd while the worker nulls it on exit, so abort_worker()
    sees either a live THD to awake() or nullptr.
    See parallel_worker_thread_func.
  */
  mysql_mutex_t   LOCK_worker;

  pwt_worker_info   info; // Connection/thread name
  pthread_t         pthread;

  void release_worker(bool abort);
  void destroy_worker_thd();
  void init_and_run_thread_func();
  friend void *pwt_worker_base_thread_func(void *arg);
};


#endif
