#include "sql_parallel_thread.h"
#include "mariadb.h"
#include "mysqld_error.h"
#include "sql_class.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_select.h"
#include "sql_parallel_workers.h"
#include "debug_sync.h"
#include "transaction.h"

extern MYSQL_THD create_background_thd();
extern void *thd_attach_thd(MYSQL_THD thd);
extern void thd_detach_thd(void *save);
extern void destroy_background_thd(MYSQL_THD thd);

/**
  @brief
    Save error condition into pwt_queued_event object.

  @return
    true      an error occurred
    false     error or warning is provided in *event
*/

static
bool save_error_to_queued_event(THD *thd, pwt_queued_event **event, uint error,
                                 Sql_condition::enum_warning_level level,
                                 const char *msg)
{
  DBUG_EXECUTE_IF("pwt_error_to_queue_oom",
                  { *event= nullptr; return true; });
  *event= (pwt_queued_event*) my_malloc(key_memory_pwt_queued_event,
                                        sizeof(pwt_queued_event),
                                        MYF(0));
  if (!*event)
    return true;
  (*event)->error= (pwt_error_message*) my_malloc(key_memory_pwt_error_message,
                                                  sizeof(pwt_error_message),
                                                  MYF(0));
  if (!(*event)->error)
  {
    my_free(*event);
    *event= nullptr;
    return true;
  }
  (*event)->error->level= level;
  if (level == Sql_condition::enum_warning_level::WARN_LEVEL_ERROR)
    (*event)->error->worker_errno= thd->killed_errno();
  else
    (*event)->error->worker_errno= 0;
  (*event)->error->code= error;
  (*event)->error->message= (char *) my_malloc(key_memory_pwt_error_message,
                                               strlen(msg)+1,
                                               MYF(0));
  if (!(*event)->error->message)
  {
    my_free((*event)->error);
    my_free(*event);
    *event= nullptr;
    return true;
  }
  strmake((*event)->error->message, msg, strlen(msg));
  return false;
}

/*
  Capture errors and warnings and redirect them to worker->manager.
*/

class PWT_error_handler : public Internal_error_handler
{
  pwt_manager_base *manager;
  pwt_worker_base *worker;
public:
  PWT_error_handler(pwt_manager_base *manager_arg, pwt_worker_base *worker_arg) :
     manager(manager_arg), worker(worker_arg) {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sql_state,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl) override
  {
    /*
      A genuine error (not a warning) raised while the worker runs the query
      -- e.g. a WHERE/projection/join evaluation error -- must abort the whole
      query. Trip fatal_error so the worker stops producing and the manager
      aborts instead of sending a truncated result. The error text is still
      relayed to the manager below and surfaced from finalize.

      Exclude a killed worker's own ER_QUERY_INTERRUPTED: a KILL is reported
      separately via kill_signal (which also carries the kill type, so a
      KILL vs KILL QUERY of a worker maps to dropping the manager connection
      vs just its query). Tripping fatal_error here would make the manager
      take the generic-error path and lose that distinction.
    */
    if (*level == Sql_condition::WARN_LEVEL_ERROR && !thd->killed)
    {
      //TODO: and we don't save which error we've got?
      //  Do we save it right below? If yes, why don't we
      //  enqueue it first?
      //  An error appears to have multiple elements, so we contain it, then
      //  queue it.
      worker->on_fatal_error();
    }
    pwt_queued_event *event;
    if (save_error_to_queued_event(thd, &event, sql_errno, *level, msg))
    {
      /*
        Couldn't allocate the queued event. The worker THD's diagnostics
        area is discarded when the worker exits, so flag the manager so it
        can surface a single ER_OUTOFMEMORY warning to the user instead of
        letting this condition vanish.
      */
      manager->notify_message_dropped();
      return true;
    }
    manager->record_event(event);
    return true;                // no further processing in worker thread
  }

};


void pwt_manager_base::notify_message_dropped()
{
  mysql_mutex_lock(&LOCK_pwt_manager);
  messages_dropped= true;
  mysql_mutex_unlock(&LOCK_pwt_manager);
}


#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_pwt;
static PSI_thread_info all_pwt_threads[]=
{
  { &key_thread_pwt, WORKER_NAME, PSI_FLAG_GLOBAL},
};

static PSI_mutex_key key_mutex_pwt_LOCK_worker,
                     key_mutex_pwt_LOCK_manager;

static PSI_mutex_info all_pwt_mutexes[]=
{
  { &key_mutex_pwt_LOCK_manager,  "pwt_manager::LOCK_pwt_manager",  0},
  { &key_mutex_pwt_LOCK_worker,   "pwt_worker::LOCK_worker",        0},
};

void pwt_threads_init_psi_keys(void)
{
  const char *category= "sql";
  PSI_server->register_thread(category, all_pwt_threads, 
                              array_elements(all_pwt_threads));
  mysql_mutex_register(category, all_pwt_mutexes, 
                       array_elements(all_pwt_mutexes));
}
#endif


void *pwt_worker_base_thread_func(void *arg)
{
  DBUG_ENTER("parallel_worker_thread_func");
  pwt_worker_base *worker= (pwt_worker_base*) arg;
  worker->init_and_run_thread_func();
  return nullptr;
}


bool pwt_worker_base::create_thread()
{
  if (mysql_thread_create(key_thread_pwt, &pthread, /*pthread_attr_t*/ nullptr,
                          pwt_worker_base_thread_func, this))
    return 1;
  thread_started= true;
  return 0;
}


void pwt_worker_base::init_and_run_thread_func()
{
  PWT_error_handler error_handler(manager_base, this);

  /*
    Set current_thd and thread local storage (my_thread_var) for our new THD
    to ensure they have their own local objects/errors/warnings etc
  */
  void *save= thd_attach_thd(thd);
  /*
    create_background_thd()'s THD(0) never ran the connection-setup path that
    allocates debug_sync_control, so DEBUG_SYNC actions on this worker (e.g. the
    pwt_worker_pause_before_signal sync point used by the tests) would assert on
    a NULL control block. Initialise it here, on the worker thread, so the
    MY_THREAD_SPECIFIC allocation is charged to this THD and freed symmetrically
    by ~THD (debug_sync_end_thread) when destroy_background_thd() runs below.
    No-op in non-DEBUG_SYNC builds and when debug_sync is inactive.
  */
#ifdef ENABLED_DEBUG_SYNC
  if (!thd->debug_sync_control)
    debug_sync_init_thread(thd);
#endif
  my_thread_set_name(thd->connection_name.str);
  THD_STAGE_INFO(thd, stage_sending_data);
  thd->push_internal_handler(&error_handler);

  DBUG_EXECUTE_IF("pwt_error_to_queue_oom",
  {
    push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                 "This is an example warning to show we can push a "
                 "warning from a worker thread to its manager ");
  });
#ifdef ENABLED_DEBUG_SYNC
  /*
    we can't sync on the managers or our THD, spin the whole thing about
    and use the global signal pool, NO_CLEAR_EVENT is needed because we have
    multiple workers and the wrong one will likely consume the signal.
  */
  DBUG_EXECUTE_IF("pwt_worker_pause_before_signal",
    DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(
      "now SIGNAL pwt_worker_paused WAIT_FOR pwt_worker_continue NO_CLEAR_EVENT"
      ))););
  
  // TODO: what is this for? Test coverage for the above?
  // yes
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  if (thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
  }
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
#endif

  thread_func();

  // manager needs to see this as atomic
  mysql_mutex_lock(&LOCK_worker);
  /*
    LOCK_thd_kill is the canonical guard for thd->killed; a user-issued
    KILL on this worker's thread_id goes through THD::awake() which holds
    LOCK_thd_kill but not LOCK_worker, so we must nest both to get a
    race-free snapshot for the manager.
  */
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
  thd->pop_internal_handler();       // maybe not needed

  /*
    Null it while we still hold LOCK_worker, and tear the THD down from a local
    copy afterwards. abort_worker() reads this member under the same lock and
    calls awake() on it, so it has to see either a THD that is still alive --
    the lock keeps us out of the teardown below until it has -- or nullptr.
    Leaving the member set would let it awake() a THD this thread has already
    destroyed.
  */
  THD *worker_thd= thd;
  thd= nullptr;
  mysql_mutex_unlock(&LOCK_worker);

  // This is similar to what destroy_worker_thd() does:
  server_threads.erase(worker_thd);
  /*
    executing thd_detach_thd sets my_thread_var to null, stopping our ability
    use the normal mutex mechanisms, so we operate this outside the locked
    region on a copy of our THD pointer
  */
  thd_detach_thd(save);

  destroy_background_thd(worker_thd);
}

/**
  @brief
    Empty warning/error queue from workers into our manager thread

 
  @description
    Surface errors/warnings the workers queued via PWT_error_handler. A worker
    error that mattered to the result has already aborted the join during
    execution (fatal_error or a propagated kill), so thd is already in error by
    the time we get here; raising another error would trip the "can't overwrite
    status" assertion in the diagnostics area. So only raise a queued ERROR
    when thd is not already in error -- otherwise keep it as a warning. Plain
    warnings are always safe to add.

  @parameters
    skip_interrupted          whether or not to ignore a deluge of interrupted
                              errors
*/
void pwt_manager_base::process_pending_warnings(bool skip_interrupted)
{
  bool surface_drop;
  mysql_mutex_lock(&LOCK_pwt_manager);
  surface_drop= messages_dropped;
  messages_dropped= false;
  pwt_queued_event *event;
  while ((event= parallel_messages.get()))
  {
    if (pwt_error_message *err= event->error)
    {
      // if called during initialization, skip processing
      if (!skip_interrupted || err->code != ER_QUERY_INTERRUPTED)
      {
        if (err->level == Sql_condition::enum_warning_level::WARN_LEVEL_ERROR &&
            !thd->is_error())
          my_message_sql(err->code, err->message, MYF(0));
        else
          push_warning(thd, Sql_condition::WARN_LEVEL_WARN, err->code,
                     err->message);
      }
      my_free(err->message);
      my_free(err);
    }
    my_free(event);
  }
  mysql_mutex_unlock(&LOCK_pwt_manager);

  if (surface_drop)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_OUTOFMEMORY,
                        "Parallel worker diagnostics were dropped due to "
                        "memory allocation failure");
  }
}


bool pwt_worker_base::init_worker_thd(THD *parent_thd, int worker_nr)
{
  /* First, do things that may fail early. */
  LEX_CSTRING new_db;
  if (parent_thd->db.str)
  {
    // Explicit call in ~THD/THD::free_connection()/my_free, so we do this
    if (!(new_db.str= my_strndup(key_memory_pwt_db, parent_thd->db.str,
                                 parent_thd->db.length,
                                 MYF(MY_WME | ME_FATAL))))
      return true;  // OOM
    new_db.length= parent_thd->db.length;
  }
  else
  {
    new_db.str= nullptr;
    new_db.length= 0;
  }

  thd= create_background_thd();
  if (!thd)
  {
    my_free(const_cast<char*>(new_db.str));
    my_error(ER_INTERNAL_ERROR, MYF(0),
            "init_parallel_workers: failed to create worker thread THD");
    return true; // Failed
  }
  thd->db= new_db;
  mysql_mutex_init(key_mutex_pwt_LOCK_worker, &LOCK_worker, 
                   MY_MUTEX_INIT_FAST);
  //mysql_cond_init(key_COND_pwt_worker, &COND_worker, nullptr);

  thd->system_thread= SYSTEM_THREAD_GENERIC;
  size_t len= my_snprintf(info.conn_name, MAX_THREAD_NAME,
                          WORKER_NAME);
  thd->connection_name.str= info.conn_name;
  thd->connection_name.length= len;

  thd->security_ctx= parent_thd->security_ctx;
  thd->set_command(parent_thd->get_command());
  thd->start_utime= parent_thd->start_utime;

  /*
    A worker evaluates this session's expressions, so it is running this
    session's query and has to say so. Items that hold a value for the
    duration of one statement keep it against the query id they computed it
    under: Item_func_curtime, and the NOW/SYSDATE family with it, recomputes
    only when thd->query_id has moved past its own last_query_id. A
    background THD starts at query id 0, which is also the value that field
    is born with, so a copy that had not been evaluated before it was cloned
    looked up to date to the worker and handed out its uninitialized
    MYSQL_TIME -- what the is_valid_value_slow() assertion in Time::Time()
    catches.
  */
  thd->query_id= parent_thd->query_id;
  thd->thread_id= next_thread_id();
  my_snprintf(info.process_list, sizeof(info.process_list),
              WORKER_NAME " %u " CONNECTION_NAME_THREAD " %llu",
              worker_nr, parent_thd->thread_id);
  thd->query_string= CSET_STRING(info.process_list,
                                 strlen(info.process_list),
                                 thd->query_charset());

  thd->userstat_running= parent_thd->userstat_running;

  // TODO: it is OK that we insert before starting the OS thread, right?
  /*
    Visible in the processlist before its thread exists, and so killable before
    it exists. That is safe, and worth saying why rather than leaving it as a
    question: create_background_thd() returns a fully built THD, and thread_id
    and query_string are set above, so nothing here is half-made. A KILL that
    lands in the window sets thd->killed and little else -- system_thread is
    SYSTEM_THREAD_GENERIC so awake() leaves mysys_var->abort alone, there is no
    condition to signal yet, and no engine transaction to interrupt -- and the
    worker acts on it as soon as it starts, which is what we want it to do.

    What the window costs is that every path out of here has to erase it again.
    release_worker() is that path, and it is the only one.
  */
  server_threads.insert(thd);  // +information_schema.processlist

  /*
    From here the worker holds a THD and LOCK_worker, and only release_worker()
    gives them back. Nothing between the mutex init above and this line may
    fail, or the flag would not cover what was taken.
  */
  inited= true;
  return false; // Ok
}

/*
  Destroy the THD init_worker_thd() built, for a worker whose thread never ran.
  Nulls it, so it stays true that a null thd means nobody has one to destroy.
*/

void pwt_worker_base::destroy_worker_thd()
{
  if (!thd)
    return;
  server_threads.erase(thd);
  /*
    destroy_background_thd() requires current_thd to be NULL because it
    re-attaches the background THD to this thread's TLS. We are running on
    the user's query thread (current_thd == manager thd), so save/null/
    restore around the call. Mirrors the create_background_thd() pattern.
  */
  THD *save_thd= current_thd;
  set_current_thd(nullptr);
  destroy_background_thd(thd);
  set_current_thd(save_thd);
  thd= nullptr;
}

/*
  @brief
    Give back everything this worker holds. See reap_worker()/abort_worker().

  @description
    Three stages of startup, one teardown:

      not initialised    nothing was taken, so there is nothing to give back
      no thread started  the THD is still ours to destroy, and so is whatever
                         a run would have released
      thread started     wait for it; it destroyed its own THD on the way out,
                         which is what nulling thd told us
*/

void pwt_worker_base::release_worker(bool abort)
{
  if (!inited)
    return;

  if (thread_started)
  {
    if (abort)
    {
      /*
        The worker may already be tearing itself down: init_and_run_thread_func
        nulls thd and destroys the THD under LOCK_worker. Take that lock and
        only awake() if it has not entered its exit section yet; if it has, it
        is on its way out and the join below reaps it.
      */
      mysql_mutex_lock(&LOCK_worker);
      if (thd)
        thd->awake(ABORT_QUERY);
      mysql_mutex_unlock(&LOCK_worker);
    }
    pthread_join(pthread, nullptr);
    thread_started= false;
    DBUG_ASSERT(!thd);              // it destroyed its own before it exited
  }
  else
  {
    cleanup_without_run();
    destroy_worker_thd();
  }

  mysql_mutex_destroy(&LOCK_worker);
  // mysql_cond_destroy(&COND_worker);
  inited= false;
}

pwt_manager_base::pwt_manager_base() : 
    messages_dropped(false)
{
  mysql_mutex_init(key_mutex_pwt_LOCK_manager, &LOCK_pwt_manager,
                    MY_MUTEX_INIT_SLOW);
}

pwt_manager_base::~pwt_manager_base()
{
  mysql_mutex_destroy(&LOCK_pwt_manager);
}
