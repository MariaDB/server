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

#include <my_global.h>
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



/*
 WEAK_SYMBOL(return_type, function_name, argument_type1,..,argument_typeN)

 Declare and load function pointer from kernel32. The name of the static 
 variable that holds the function pointer is my_<original function name>
 This should be combined with 
 #define <original function name> my_<original function name>
 so that one could use Widows APIs transparently, without worrying whether
 they are present in a particular version or not.

 Of course, prior to use of any function there should be a check for correct
 Windows version, or check whether function pointer is not NULL.
*/
#define WEAK_SYMBOL(return_type, function, ...) \
  typedef return_type (WINAPI *pFN_##function)(__VA_ARGS__); \
  static pFN_##function my_##function = (pFN_##function) \
    (GetProcAddress(GetModuleHandle("kernel32"),#function))


WEAK_SYMBOL(BOOL, SetThreadpoolStackInformation, PTP_POOL, 
  PTP_POOL_STACK_INFORMATION);
#define SetThreadpoolStackInformation my_SetThreadpoolStackInformation

/* Log a warning */
static void tp_log_warning(const char *msg, const char *fct)
{
  sql_print_warning("Threadpool: %s. %s failed (last error %d)",msg, fct,
    GetLastError());
}


static PTP_POOL pool;
static TP_CALLBACK_ENVIRON callback_environ;
static DWORD fls;

static bool skip_completion_port_on_success = false;

/*
  Threadpool callbacks.

  io_completion_callback  - handle client request
  timer_callback - handle wait timeout (kill connection)
  shm_read_callback, shm_close_callback - shared memory stuff
  login_callback - user login (submitted as threadpool work)

*/

static void CALLBACK timer_callback(PTP_CALLBACK_INSTANCE instance, 
  PVOID context, PTP_TIMER timer);

static void CALLBACK io_completion_callback(PTP_CALLBACK_INSTANCE instance, 
  PVOID context,  PVOID overlapped,  ULONG io_result, ULONG_PTR nbytes, PTP_IO io);


static void CALLBACK work_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work);

static void CALLBACK shm_read_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID Context, PTP_WAIT wait,TP_WAIT_RESULT wait_result);

static void CALLBACK shm_close_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID Context, PTP_WAIT wait,TP_WAIT_RESULT wait_result);

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
  virtual int init();
  virtual int start_io();
  virtual void set_io_timeout(int sec);
  virtual void wait_begin(int type);
  virtual void wait_end();

  ulonglong timeout;
  enum_vio_type vio_type;
  HANDLE handle;
  OVERLAPPED overlapped;
  PTP_CALLBACK_INSTANCE callback_instance;
  PTP_IO  io;
  PTP_TIMER timer;
  PTP_WAIT shm_read;
  PTP_WORK  work;
  bool long_callback;

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
  SubmitThreadpoolWork(((TP_connection_win *)c)->work);
}


TP_connection_win::TP_connection_win(CONNECT *c) :
  TP_connection(c),
  timeout(ULONGLONG_MAX), 
  callback_instance(0),
  io(0),
  shm_read(0),
  timer(0),
  work(0)
{
}

#define CHECK_ALLOC_ERROR(op) if (!(op))  {tp_log_warning("Allocation failed", #op); DBUG_ASSERT(0); return -1; }

int TP_connection_win::init()
{

  memset(&overlapped, 0, sizeof(OVERLAPPED));
  Vio *vio = connect->vio;
  switch ((vio_type =  vio->type))
  {
  case VIO_TYPE_SSL:
  case VIO_TYPE_TCPIP:
    handle= (HANDLE)mysql_socket_getfd(vio->mysql_socket);
    break;
  case VIO_TYPE_NAMEDPIPE:
    handle= (HANDLE)vio->hPipe;
    break;
  case VIO_TYPE_SHARED_MEMORY:
    handle= vio->event_server_wrote;
    break;
  default:
    abort();
  }

  if (vio_type == VIO_TYPE_SHARED_MEMORY)
  {
    CHECK_ALLOC_ERROR(shm_read=  CreateThreadpoolWait(shm_read_callback, this, &callback_environ));
  }
  else
  {
    /* Performance tweaks (s. MSDN documentation)*/
    UCHAR flags= FILE_SKIP_SET_EVENT_ON_HANDLE;
    if (skip_completion_port_on_success)
    {
      flags |= FILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
    }
    (void)SetFileCompletionNotificationModes(handle, flags);
    /* Assign io completion callback */
    CHECK_ALLOC_ERROR(io= CreateThreadpoolIo(handle, io_completion_callback, this, &callback_environ));
  }

  CHECK_ALLOC_ERROR(timer= CreateThreadpoolTimer(timer_callback, this,  &callback_environ));
  CHECK_ALLOC_ERROR(work= CreateThreadpoolWork(work_callback, this, &callback_environ));
  return 0;
}


/*
  Start asynchronous read
*/
int TP_connection_win::start_io()
{
  DWORD num_bytes = 0;
  static char c;
  WSABUF buf;
  buf.buf= &c;
  buf.len= 0;
  DWORD flags=0;
  DWORD last_error= 0;

  int retval;
  if (shm_read)
  {
    SetThreadpoolWait(shm_read, handle, NULL);
    return 0;
  }
  StartThreadpoolIo(io);

  if (vio_type == VIO_TYPE_TCPIP || vio_type == VIO_TYPE_SSL)
  {
    /* Start async io (sockets). */
    if (WSARecv((SOCKET)handle , &buf, 1, &num_bytes, &flags,
          &overlapped,  NULL) == 0)
    {
       retval= last_error= 0;
    }
    else
    {
      retval= -1;
      last_error=  WSAGetLastError();
    }
  }
  else
  {
    /* Start async io (named pipe) */
    if (ReadFile(handle, &c, 0, &num_bytes,&overlapped))
    {
      retval= last_error= 0;
    }
    else
    {
      retval= -1;
      last_error= GetLastError();
    }
  }

  if (retval == 0 || last_error == ERROR_MORE_DATA)
  {
    /*
      IO successfully finished (synchronously). 
      If skip_completion_port_on_success is set, we need to handle it right 
      here, because completion callback would not be executed by the pool.
    */
    if(skip_completion_port_on_success)
    {
      CancelThreadpoolIo(io);
      io_completion_callback(callback_instance, this, &overlapped, last_error, 
        num_bytes, io);
    }
    return 0;
  }

  if(last_error == ERROR_IO_PENDING)
  {
    return 0;
  }

  /* Some error occurred */
  CancelThreadpoolIo(io);
  return -1;
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

  if (shm_read)
    CloseThreadpoolWait(shm_read);

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
    if (!long_callback)
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
extern ulong thread_created;
static void pre_callback(PVOID context, PTP_CALLBACK_INSTANCE instance)
{
  if (FlsGetValue(fls) == NULL)
  {
    /* Running in new  worker thread*/
    FlsSetValue(fls, (void *)1);
    statistic_increment(thread_created, &LOCK_status);
    InterlockedIncrement((volatile long *)&tp_stats.num_worker_threads);
    my_thread_init();
  }
  TP_connection_win *c = (TP_connection_win *)context;
  c->callback_instance = instance;
  c->long_callback = false;
}


/*
  Decrement number of threads when a thread exits . 
  On Windows, FlsAlloc() provides the thread destruction callbacks.
*/
static VOID WINAPI thread_destructor(void *data)
{
  if(data)
  {
    InterlockedDecrement((volatile long *)&tp_stats.num_worker_threads);
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


/*
  Shared memory read callback.
  Invoked when read event is set on connection.
*/

static void CALLBACK shm_read_callback(PTP_CALLBACK_INSTANCE instance,
  PVOID context, PTP_WAIT wait,TP_WAIT_RESULT wait_result)
{
  TP_connection_win *c= (TP_connection_win *)context;
  /* Disarm wait. */
  SetThreadpoolWait(wait, NULL, NULL);

  /* 
    This is an autoreset event, and one wakeup is eaten already by threadpool,
    and the current state is "not set". Thus we need to reset the event again, 
    or vio_read will hang.
  */
  SetEvent(c->handle);
  tp_callback(instance, context);
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

  if (threadpool_max_threads)
  {
    SetThreadpoolThreadMaximum(pool, threadpool_max_threads);
  }

  if (threadpool_min_threads)
  {
    if (!SetThreadpoolThreadMinimum(pool, threadpool_min_threads))
    {
      tp_log_warning("Can't set threadpool minimum threads",
        "SetThreadpoolThreadMinimum");
    }
  }

  /*
    Control stack size (OS must be Win7 or later)
  */
  if (SetThreadpoolStackInformation)
  {
    TP_POOL_STACK_INFORMATION stackinfo;
    stackinfo.StackCommit = 0;
    stackinfo.StackReserve = (SIZE_T)my_thread_stack_size;
    if (!SetThreadpoolStackInformation(pool, &stackinfo))
    {
      tp_log_warning("Can't set threadpool stack size",
        "SetThreadpoolStackInformation");
    }
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
