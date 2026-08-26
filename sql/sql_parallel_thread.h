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
public:
  pwt_manager_base();
  ~pwt_manager_base();

  THD                      *thd; /* Manager thread */

  void discard_pending_warnings();      // called on initialization failure
  void process_pending_warnings();      // called at end of normal executio

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


class pwt_manager;

/*
  Inherit from this your worker threads.
*/
class pwt_worker_base : public Sql_alloc
{
public:
  /* Intialize the worker and create its THD (call from master thread) */
  bool init_worker_thd(pwt_manager *manager_arg, THD *parent_thd,
                       int worker_nr);

  /* Create and run the thread */
  bool create_thread();

  /*
    This will be run by the worker thread with all environment properly set up.
    One can produce warnings/errors and get them in the master thread.
  */
  virtual void thread_func()= 0;
  virtual ~pwt_worker_base() {}

  void abort_worker();

  void join_worker_thread() { pthread_join(pthread, nullptr); }

  /*
    Do cleanup if init_worker_thd() succeeded but then there was some error.
    TODO: simpler cleanup.
  */
  void destroy_worker_thd();
  /* This is like a destructor. Called after worker is done. */
  void cleanup_worker();

  THD             *thd;
  pwt_manager     *manager;

private:
  /*
    Guards worker->thd while the worker nulls it on exit, so abort_worker()
    sees either a live THD to awake() or nullptr.
    See parallel_worker_thread_func.
  */
  mysql_mutex_t   LOCK_worker;

  pwt_worker_info   info; // Connection/thread name
  pthread_t         pthread;

  void init_and_run_thread_func();
  friend void *pwt_worker_base_thread_func(void *arg);
};


#endif
