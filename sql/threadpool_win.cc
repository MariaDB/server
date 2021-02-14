/* Copyright (C) 2012 Monty Program Ab

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

#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif

#define _WIN32_WINNT 0x0601

#include "mariadb.h"
#include <violite.h>
#include <sql_priv.h>
#include <sql_class.h>
#include <my_pthread.h>
#include <scheduler.h>
#include <sql_connect.h>
#include <mysqld.h>
#include <debug_sync.h>
#include <threadpool.h>
#include <windows.h>
#include <set_var.h>

#include "threadpool_winsockets.h"

/* Log a warning */
static void tp_log_warning(const char *msg, const char *fct)
{
  sql_print_warning("Threadpool: %s. %s failed (last error %d)",msg, fct,
    GetLastError());
}


static PTP_POOL pool;
static TP_CALLBACK_ENVIRON callback_environ;
static DWORD fls;

PTP_CALLBACK_ENVIRON get_threadpool_win_callback_environ()
{
  return pool? &callback_environ: 0;
}

/*
  Threadpool callbacks.

  io_completion_callback  - handle client request
  timer_callback - handle wait timeout (kill connection)
  login_callback - user login (submitted as threadpool work)

*/

static void CALLBACK timer_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID context, PTP_TIMER timer);

static void CALLBACK io_completion_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID context,  PVOID overlapped,  ULONG io_result, ULONG_PTR nbytes, PTP_IO io);


static void CALLBACK work_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work);

static void pre_callback(PVOID context, PTP_CALLBACK_INSTANCE instance);

/* Get current time as Windows time */
static ulonglong now()
{
  ulonglong current_time;
  GetSystemTimeAsFileTime((PFILETIME)&current_time);
  return current_time;
}

struct TP_connection_win:public TP_connection
{
public:
  TP_connection_win(CONNECT*);
  ~TP_connection_win();
  int init() override;
  void init_vio(st_vio *vio) override;
  int start_io() override;
  void set_io_timeout(int sec) override;
  void wait_begin(int type) override;
  void wait_end() override;

  ulonglong timeout=ULLONG_MAX;
  OVERLAPPED overlapped{};
  PTP_CALLBACK_INSTANCE callback_instance{};
  PTP_IO io{};
  PTP_TIMER timer{};
  PTP_WORK work{};
  bool long_callback{};
  win_aiosocket sock;
};

struct TP_connection *new_TP_connection(CONNECT *connect)
{
  TP_connection *c = new (std::nothrow) TP_connection_win(connect);
  if (!c || c->init())
  {
    delete c;
    return 0;
  }
  return c;
}

void TP_pool_win::add(TP_connection *c)
{
  if(FlsGetValue(fls))
  {
    /* Inside threadpool(), execute callback directly. */
    tp_callback(c);
  }
  else
  {
    SubmitThreadpoolWork(((TP_connection_win *)c)->work);
  }
}

void TP_pool_win::resume(TP_connection* c)
{
  DBUG_ASSERT(c->state == TP_STATE_RUNNING);
  SubmitThreadpoolWork(((TP_connection_win*)c)->work);
}

#define CHECK_ALLOC_ERROR(op)                                                 \
  do                                                                          \
  {                                                                           \
    if (!(op))                                                                \
    {                                                                         \
      tp_log_warning("Allocation failed", #op);                               \
    }                                                                         \
  } while (0)

TP_connection_win::TP_connection_win(CONNECT *c) :
    TP_connection(c)
{
  /* Assign io completion callback */
  HANDLE h= c->vio_type == VIO_TYPE_NAMEDPIPE ? c->pipe
                                              : (HANDLE)mysql_socket_getfd(c->sock);

  CHECK_ALLOC_ERROR(io=CreateThreadpoolIo(h, io_completion_callback, this, &callback_environ));
  CHECK_ALLOC_ERROR(timer= CreateThreadpoolTimer(timer_callback, this, &callback_environ));
  CHECK_ALLOC_ERROR(work= CreateThreadpoolWork(work_callback, this, &callback_environ));
}

int TP_connection_win::init()
{
  return !io  || !timer || !work ;
}

void TP_connection_win::init_vio(st_vio* vio)
{
  sock.init(vio);
}

/*
  Start asynchronous read
*/
int TP_connection_win::start_io()
{
  StartThreadpoolIo(io);
  if (sock.begin_read())
  {
    /* Some error occurred */
    CancelThreadpoolIo(io);
    return -1;
  }
  return 0;
}

/*
  Recalculate wait timeout, maybe reset timer.
*/
void TP_connection_win::set_io_timeout(int timeout_sec)
{
  ulonglong old_timeout= timeout;
  ulonglong new_timeout = now() + 10000000LL * timeout_sec;

  if (new_timeout < old_timeout)
  {
    SetThreadpoolTimer(timer, (PFILETIME)&new_timeout, 0, 1000);
  }
  /*  new_timeout > old_timeout case is handled by expiring timer. */
  timeout = new_timeout;
}


TP_connection_win::~TP_connection_win()
{
  if (io)
    CloseThreadpoolIo(io);

  if (work)
    CloseThreadpoolWork(work);

  if (timer)
  {
    SetThreadpoolTimer(timer, 0, 0, 0);
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
    CloseThreadpoolTimer(timer);
  }
}

void TP_connection_win::wait_begin(int type)
{
  /*
    Signal to the threadpool whenever callback can run long. Currently, binlog
    waits are a good candidate, its waits are really long
  */
  if (type == THD_WAIT_BINLOG)
  {
    if (!long_callback && callback_instance)
    {
      CallbackMayRunLong(callback_instance);
      long_callback= true;
    }
  }
}

void TP_connection_win::wait_end()
{
  /* Do we need to do anything ? */
}

/*
  This function should be called first whenever a callback is invoked in the
  threadpool, does my_thread_init() if not yet done
*/
void tp_win_callback_prolog()
{
  if (FlsGetValue(fls) == NULL)
  {
    /* Running in new  worker thread*/
    FlsSetValue(fls, (void *)1);
    thread_created++;
    tp_stats.num_worker_threads++;
    my_thread_init();
  }
}

extern ulong thread_created;
static void pre_callback(PVOID context, PTP_CALLBACK_INSTANCE instance)
{
  tp_win_callback_prolog();
  TP_connection_win *c = (TP_connection_win *)context;
  c->callback_instance = instance;
  c->long_callback = false;
}


/*
  Decrement number of threads when a thread exits.
  On Windows, FlsAlloc() provides the thread destruction callbacks.
*/
static VOID WINAPI thread_destructor(void *data)
{
  if(data)
  {
    tp_stats.num_worker_threads--;
    my_thread_end();
  }
}



static inline void tp_callback(PTP_CALLBACK_INSTANCE instance, PVOID context)
{
  pre_callback(context, instance);
  tp_callback((TP_connection *)context);
}


/*
  Handle read completion/notification.
*/
static VOID CALLBACK io_completion_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID context,  PVOID overlapped,  ULONG io_result, ULONG_PTR nbytes, PTP_IO io)
{
  TP_connection_win *c= (TP_connection_win *)context;

  /* How many bytes were preread into read buffer */
  c->sock.end_read((ULONG)nbytes, io_result);

  /*
    Execute high priority connections immediately.
    'Yield' in case of low priority connections, i.e SubmitThreadpoolWork (with the same callback)
    which makes Windows threadpool place the items at the end of its internal work queue.
  */
  if (c->priority == TP_PRIORITY_HIGH)
    tp_callback(instance, context);
  else
    SubmitThreadpoolWork(c->work);
}


/*
  Timer callback.
  Invoked when connection times out (wait_timeout)
*/
static VOID CALLBACK timer_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID parameter, PTP_TIMER timer)
{
  TP_connection_win *c = (TP_connection_win *)parameter;
  if (c->timeout <= now())
  {
    tp_timeout_handler(c);
  }
  else
  {
    /*
      Reset timer.
      There is a tiny possibility of a race condition, since the value of timeout
      could have changed to smaller value in the thread doing io callback.

      Given the relative unimportance of the wait timeout, we accept race
      condition.
      */
    SetThreadpoolTimer(timer, (PFILETIME)&c->timeout, 0, 1000);
  }
}

static void CALLBACK work_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
  tp_callback(instance, context);
}

TP_pool_win::TP_pool_win()
{}

int TP_pool_win::init()
{
  fls= FlsAlloc(thread_destructor);
  pool= CreateThreadpool(NULL);

  if (!pool)
  {
    sql_print_error("Can't create threadpool. "
      "CreateThreadpool() failed with %d. Likely cause is memory pressure",
      GetLastError());
    return -1;
  }

  InitializeThreadpoolEnvironment(&callback_environ);
  SetThreadpoolCallbackPool(&callback_environ, pool);

  if (IS_SYSVAR_AUTOSIZE(&threadpool_max_threads))
  {
    /*
     Nr 500 comes from Microsoft documentation,
     there is no API for GetThreadpoolThreadMaxThreads()
    */
    SYSVAR_AUTOSIZE(threadpool_max_threads,500);
  }
  else
  {
    SetThreadpoolThreadMaximum(pool, threadpool_max_threads);
  }

  if (IS_SYSVAR_AUTOSIZE(&threadpool_min_threads))
  {
    SYSVAR_AUTOSIZE(threadpool_min_threads,1);
  }
  else
  {
    if (!SetThreadpoolThreadMinimum(pool, threadpool_min_threads))
    {
      tp_log_warning("Can't set threadpool minimum threads",
        "SetThreadpoolThreadMinimum");
    }
  }


  if (IS_SYSVAR_AUTOSIZE(&global_system_variables.threadpool_priority))
  {
    /*
     There is a notable overhead for "auto" priority implementation,
     use "high" which handles socket IO callbacks as they come
     without rescheduling to work queue.
    */
    SYSVAR_AUTOSIZE(global_system_variables.threadpool_priority,
                    TP_PRIORITY_HIGH);
  }

  TP_POOL_STACK_INFORMATION stackinfo;
  stackinfo.StackCommit = 0;
  stackinfo.StackReserve = (SIZE_T)my_thread_stack_size;
  if (!SetThreadpoolStackInformation(pool, &stackinfo))
  {
    tp_log_warning("Can't set threadpool stack size",
      "SetThreadpoolStackInformation");
  }
  return 0;
}


/**
  Scheduler callback : Destroy the scheduler.
*/
TP_pool_win::~TP_pool_win()
{
  if (!pool)
    return;
  DestroyThreadpoolEnvironment(&callback_environ);
  SetThreadpoolThreadMaximum(pool, 0);
  CloseThreadpool(pool);
  if (!tp_stats.num_worker_threads)
    FlsFree(fls);
}
/**
  Sets the number of idle threads the thread pool maintains in anticipation of new
  requests.
*/
int TP_pool_win::set_min_threads(uint val)
{
  SetThreadpoolThreadMinimum(pool, val);
  return 0;
}

int TP_pool_win::set_max_threads(uint val)
{
  SetThreadpoolThreadMaximum(pool, val);
  return 0;
}


TP_connection *TP_pool_win::new_connection(CONNECT *connect)
{
  TP_connection *c= new (std::nothrow) TP_connection_win(connect);
  if (!c )
    return 0;
  if (c->init())
  {
    delete c;
    return 0;
  }
  return c;
}

