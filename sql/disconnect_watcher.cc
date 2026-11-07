/* Copyright (c) 2026, MariaDB

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

#include "mariadb.h"
#include "disconnect_watcher.h"

#ifdef HAVE_DISCONNECT_WATCHER

#include "sql_class.h"
#include "sql_show.h"
#include "sql_array.h"
#include "wsrep_mysqld.h"
#include <atomic>

/*
  Like KILL, the watcher finds a THD by thread id, which returns it under
  LOCK_thd_kill, so it can't be deleted, and never touches the socket. A stale
  event finds no THD, or hits a reused id, which in the worst case takes 2^32 /
  (number of connections) new ones. The owner unregisters the socket before
  closing it.
*/

/* Set at shutdown, events are ignored from then on */
static std::atomic<bool> stopping{false};

static pthread_t watcher_thread;
static bool thread_started;
/* The thread can't be stopped, leave it */
static bool wake_failed;

static bool is_socket_vio(Vio *vio)
{
  switch (vio_type(vio)) {
  case VIO_TYPE_TCPIP:
  case VIO_TYPE_SOCKET:
  case VIO_TYPE_SSL:
    return true;
  default:
    return false;
  }
}


static void join_watcher()
{
  if (thread_started)
  {
    pthread_join(watcher_thread, nullptr);
    thread_started= false;
  }
}


/**
  A connection that can't be watched is not aborted when its client is gone,
  say so once.

  @param func  name of the failed call
  @param err   system error of the call
*/
static void watch_failed(const char *func, uint err)
{
  static std::atomic<bool> logged{false};
  if (logged.exchange(true))
    return;
#ifdef _WIN32
  sql_print_warning("Can't watch a client connection for disconnects: "
                    "%s failed (error: %u), it is not aborted when the "
                    "client is gone", func, err);
#else
  sql_print_warning("Can't watch a client connection for disconnects: "
                    "%s failed (errno: %iE), it is not aborted when the "
                    "client is gone", func, (int) err);
#endif
}


/**
  A call of the watcher thread failed. Running out of memory is logged once
  and retried, anything else is a bug.

  @param func       name of the failed call
  @param err        system error of the call
  @param transient  the error can go away, so the watcher does not abort
*/
static void handle_os_error(const char *func, uint err, bool transient)
{
  /* Used by the watcher thread only */
  static bool logged;
  if (transient)
  {
    if (logged)
      return;
    logged= true;
  }
#ifdef _WIN32
  sql_print_error("The client disconnect watcher: %s failed "
                  "(error: %u)", func, err);
#else
  sql_print_error("The client disconnect watcher: %s failed "
                  "(errno: %iE)", func, (int) err);
#endif
  if (!transient)
    abort();
}


/**
  The socket reported an error or hangup, abort the statement like KILL does.

  @param id  id of the connection, which may be gone already
*/
static void kill_disconnect(my_thread_id id)
{
  /* Shutdown kills the connections itself, OFF also ends the watches made */
  if (abort_loop || !opt_disconnect_watcher)
    return;
  THD *thd= find_thread_by_id(id, false);
  if (!thd)
    return;

  mysql_mutex_lock(&thd->LOCK_thd_data);
  /* Galera aborts its transactions by its own rules, not by a bare kill flag */
  if (!WSREP(thd))
    thd->set_client_gone();
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
}


/*
  Events handled at once. After each round the watcher waits DELAY_MS, so that
  a connection that quits removes its socket before the FIN is handled. On
  Windows it is a Sleep(), about 15 ms.
*/
static const int BATCH= 1024;
static const int DELAY_MS= 5;

#if defined(__linux__) || defined(DISCONNECT_WATCHER_KQUEUE)

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

/*
  One epoll set (kqueue on BSD) with all client sockets, and a thread of its
  own, as aborting a statement takes locks and can wait for the victim. The
  thread is stopped through a pipe that is in the set too.
*/
static std::atomic<int> poll_fd{-1};
static int wake_pipe[2]= {-1, -1};

/* A connection has no state here, the object only tells that it is watched */
struct Disconnect_watch {};
static Disconnect_watch watched;

/**
  Make the set of events. @return its descriptor, -1 on error
*/
static int poll_create();

/**
  Add the read end of the wake_pipe to the set
*/
static int poll_add_wake();

/**
  Wait for events and abort the statements of the connections that have one.

  @return false when the thread has to stop
*/
static bool handle_events();

#ifdef __linux__

#include <sys/epoll.h>

/*
  Oneshot mode. EPOLLERR and EPOLLHUP are always reported, EPOLLRDHUP alone
  tells a FIN, which is all a crashed client can leave. Thread ids are below
  2^32, so all ones is the wake pipe.
*/
static const uint64_t WAKE_EVENT= ~0ULL;

static int poll_create()
{
  return epoll_create1(EPOLL_CLOEXEC);
}


static int poll_add_wake()
{
  epoll_event ev= {};
  ev.events= EPOLLIN;
  ev.data.u64= WAKE_EVENT;
  return epoll_ctl(poll_fd, EPOLL_CTL_ADD, wake_pipe[0], &ev);
}


static bool handle_events()
{
  /* Static, off the stack */
  static epoll_event events[BATCH];
  int n= epoll_wait(poll_fd, events, BATCH, -1);
  if (n < 0 && errno != EINTR)
    handle_os_error("epoll_wait", errno, errno == ENOMEM);
  for (int i= 0; i < n; i++)
  {
    if (events[i].data.u64 == WAKE_EVENT)
      return false;
    kill_disconnect(events[i].data.u64);
  }
  return true;
}


Disconnect_watch *disconnect_watcher_add(my_thread_id thread_id, Vio *vio)
{
  if (stopping || poll_fd < 0 || !vio || !is_socket_vio(vio))
    return nullptr;
  DBUG_ASSERT(thread_id != WAKE_EVENT);
  epoll_event ev= {};
  ev.events= EPOLLONESHOT | EPOLLRDHUP;
  ev.data.u64= thread_id;
  if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, vio_fd(vio), &ev))
  {
    watch_failed("epoll_ctl", errno);
    return nullptr;
  }
  return &watched;
}


void disconnect_watcher_remove(Disconnect_watch **handle, Vio *vio)
{
  if (!*handle)
    return;
  *handle= nullptr;
  /* The socket is still open. An event that is dequeued already is safe. */
  if (poll_fd >= 0 && vio)
    epoll_ctl(poll_fd, EPOLL_CTL_DEL, vio_fd(vio), nullptr);
}

#else

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

/*
  EV_CLEAR reports a disconnect once. macOS has a socket filter, private
  to XNU like libdispatch uses it, that reports a reset and a read shutdown,
  never data. Elsewhere a read filter fires on data too, so its low water mark
  is out of reach, which leaves EV_EOF and errors.
*/
#ifdef __APPLE__
#ifndef EVFILT_SOCK
#define EVFILT_SOCK (-13)
#endif
#ifndef NOTE_CONNRESET
#define NOTE_CONNRESET 0x00000001
#endif
#ifndef NOTE_READCLOSED
#define NOTE_READCLOSED 0x00000002
#endif
#define WATCH_FILTER EVFILT_SOCK
#else
#define WATCH_FILTER EVFILT_READ
#endif

static bool client_gone(const struct kevent &e)
{
#ifdef __APPLE__
  if (e.fflags & (NOTE_CONNRESET | NOTE_READCLOSED))
    return true;
#endif
  return e.flags & (EV_EOF | EV_ERROR);
}


static int poll_create()
{
  int fd= kqueue();
  if (fd >= 0)
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}


static int poll_add_wake()
{
  struct kevent ev;
  EV_SET(&ev, wake_pipe[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
  return kevent(poll_fd, &ev, 1, nullptr, 0, nullptr);
}


static bool handle_events()
{
  /* Static, off the stack */
  static struct kevent events[BATCH];
  int n= kevent(poll_fd, nullptr, 0, events, BATCH, nullptr);
  if (n < 0 && errno != EINTR)
    handle_os_error("kevent", errno, errno == ENOMEM);
  for (int i= 0; i < n; i++)
  {
    if (events[i].ident == (uintptr_t) wake_pipe[0])
      return false;
    if (client_gone(events[i]))
      kill_disconnect((uintptr_t) events[i].udata);
  }
  return true;
}


Disconnect_watch *disconnect_watcher_add(my_thread_id thread_id, Vio *vio)
{
  if (stopping || poll_fd < 0 || !vio || !is_socket_vio(vio))
    return nullptr;
  struct kevent ev;
  void *udata= (void *) (uintptr_t) thread_id;
#ifdef __APPLE__
  EV_SET(&ev, vio_fd(vio), WATCH_FILTER, EV_ADD | EV_CLEAR,
         NOTE_CONNRESET | NOTE_READCLOSED, 0, udata);
#else
  EV_SET(&ev, vio_fd(vio), WATCH_FILTER, EV_ADD | EV_CLEAR, NOTE_LOWAT,
         0x7fffffff, udata);
#endif
  if (kevent(poll_fd, &ev, 1, nullptr, 0, nullptr))
  {
    watch_failed("kevent", errno);
    return nullptr;
  }
  return &watched;
}


void disconnect_watcher_remove(Disconnect_watch **handle, Vio *vio)
{
  if (!*handle)
    return;
  *handle= nullptr;
  /* The socket is still open, so the filter is there. Else ENOENT is fine. */
  if (poll_fd >= 0 && vio)
  {
    struct kevent ev;
    EV_SET(&ev, vio_fd(vio), WATCH_FILTER, EV_DELETE, 0, 0, nullptr);
    kevent(poll_fd, &ev, 1, nullptr, 0, nullptr);
  }
}

#endif


static void *disconnect_watcher_main(void *)
{
  my_thread_init();
  my_thread_set_name("disconn_watcher");
  struct pollfd stop_pipe= {wake_pipe[0], POLLIN, 0};
  while (handle_events())
    /* A FIN can be seen before COM_QUIT is read, let it remove the watch */
    if (poll(&stop_pipe, 1, DELAY_MS) > 0)
      break;
  my_thread_end();
  return nullptr;
}


static void close_poll()
{
  int fd= poll_fd.exchange(-1);
  if (fd >= 0)
    close(fd);
  for (int &p : wake_pipe)
  {
    if (p >= 0)
      close(p);
    p= -1;
  }
}


/**
  Make the pipe that wakes the thread up. @return errno, 0 if there is none
*/
static int make_wake_pipe()
{
  if (pipe(wake_pipe))
    return errno;
  fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);
  return 0;
}


int disconnect_watcher_start()
{
  int err= make_wake_pipe();
  if (!err && ((poll_fd= poll_create()) < 0 || poll_add_wake()))
    err= errno;
  if (!err && !(err= mysql_thread_create(key_thread_disconnect_watcher,
                                         &watcher_thread, nullptr,
                                         disconnect_watcher_main, nullptr)))
  {
    thread_started= true;
    return 0;
  }
  sql_print_warning("Can't start the client disconnect watcher (errno: %iE), "
                    "disconnect_watcher is OFF", err);
  close_poll();
  return -1;
}


/* Events are ignored from now on, and the thread is told to exit */
void disconnect_watcher_stop()
{
  if (!stopping.exchange(true) && wake_pipe[1] >= 0)
  {
    char c= 0;
    ssize_t n;
    while ((n= write(wake_pipe[1], &c, 1)) < 0 && errno == EINTR)
      ;
    if (n != 1)
    {
      sql_print_warning("Can't stop the client disconnect watcher (errno: %iE)",
                        errno);
      wake_failed= true;
    }
  }
}


bool disconnect_watcher_running()
{
  return !stopping && thread_started;
}


void disconnect_watcher_end()
{
  disconnect_watcher_stop();
  if (wake_failed)
    return;
  join_watcher();
  close_poll();
}


#elif defined(_WIN32)

/*
  A socket is watched with WSAEventSelect(FD_CLOSE), and a wait completion
  packet posts its event to an I/O completion port, taken by a thread of its
  own. The completion has the connection id by value, kill_disconnect()
  ignores ids it does not find. The packets are in ntdll, not in the SDK. AFD
  polling is not used, a pending poll makes select() lose the data
  notification. A named pipe is watched by the handle of its client process.
*/


static HANDLE iocp;
static const ULONG_PTR STOP_KEY= ~ULONG_PTR(0);

typedef LONG (NTAPI *func_NtCreateWaitCompletionPacket)(PHANDLE, ACCESS_MASK,
                                                         PVOID);
typedef LONG (NTAPI *func_NtAssociateWaitCompletionPacket)(
  HANDLE, HANDLE, HANDLE, PVOID, PVOID, LONG, ULONG_PTR, PBOOLEAN);
typedef LONG (NTAPI *func_NtCancelWaitCompletionPacket)(HANDLE, BOOLEAN);
/* Not in the SDK, found in ntdll, which every process has loaded */
#define NTDLL_FUNCTION(name) \
  static func_##name my_##name= (func_##name) \
    GetProcAddress(GetModuleHandle("ntdll"), #name)
NTDLL_FUNCTION(NtCreateWaitCompletionPacket);
NTDLL_FUNCTION(NtAssociateWaitCompletionPacket);
NTDLL_FUNCTION(NtCancelWaitCompletionPacket);

struct Disconnect_watch
{
  /* The event of a socket, the client process of a pipe */
  HANDLE handle;
  /* The wait completion packet of the handle */
  HANDLE wait_completion;
  /* The client process of a pipe, 0 for a socket */
  DWORD pid;
};

static void watch_loop()
{
  static OVERLAPPED_ENTRY entries[BATCH];
  ULONG n;
  for (;;)
  {
    if (!GetQueuedCompletionStatusEx(iocp, entries, BATCH, &n, INFINITE, FALSE))
    {
      uint err= GetLastError();
      handle_os_error("GetQueuedCompletionStatusEx", err,
                      err == ERROR_NOT_ENOUGH_MEMORY ||
                      err == ERROR_OUTOFMEMORY ||
                      err == ERROR_NO_SYSTEM_RESOURCES);
      if (stopping)
        return;
      /* Give the transient error time to go away, before the retry */
      Sleep(DELAY_MS);
      continue;
    }
    for (ULONG i= 0; i < n; i++)
    {
      if (stopping)
        return;
      kill_disconnect(entries[i].lpCompletionKey);
    }
    /* A FIN can be seen before COM_QUIT is read, let it remove the watch */
    Sleep(DELAY_MS);
  }
}


static void *disconnect_watcher_main(void *)
{
  my_thread_init();
  my_thread_set_name("disconn_watcher");
  watch_loop();
  my_thread_end();
  return nullptr;
}


static void close_handles()
{
  if (iocp)
    CloseHandle(iocp);
  iocp= nullptr;
}


int disconnect_watcher_start()
{
  uint err= ERROR_PROC_NOT_FOUND;
  if (my_NtCreateWaitCompletionPacket && my_NtAssociateWaitCompletionPacket &&
      my_NtCancelWaitCompletionPacket)
  {
    if (!(iocp= CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)))
      err= GetLastError();
    else if (!(err= mysql_thread_create(key_thread_disconnect_watcher,
                                        &watcher_thread, nullptr,
                                        disconnect_watcher_main, nullptr)))
    {
      thread_started= true;
      return 0;
    }
  }
  sql_print_warning("Can't start the client disconnect watcher (error: %u), "
                    "disconnect_watcher is OFF", err);
  close_handles();
  return -1;
}


/* Events are ignored from now on, and the thread is told to exit */
void disconnect_watcher_stop()
{
  if (!stopping.exchange(true) && iocp &&
      !PostQueuedCompletionStatus(iocp, 0, STOP_KEY, nullptr))
  {
    sql_print_warning("Can't stop the client disconnect watcher (error: %u)",
                      uint(GetLastError()));
    wake_failed= true;
  }
}


bool disconnect_watcher_running()
{
  return !stopping && thread_started;
}


/**
  Open the client process of a local named pipe, for waiting. If the server
  may not open it, the client is impersonated for that.

  @param pipe   server end of the pipe
  @param pid    process id of the client
  @param error  [out] system error if the process can't be opened, else 0

  @return handle of the process, or nullptr
*/
static HANDLE open_client_process(HANDLE pipe, DWORD pid, DWORD *error)
{
  HANDLE process= OpenProcess(SYNCHRONIZE, FALSE, pid);
  *error= process ? 0 : GetLastError();
  if (!process && *error == ERROR_ACCESS_DENIED &&
      ImpersonateNamedPipeClient(pipe))
  {
    process= OpenProcess(SYNCHRONIZE, FALSE, pid);
    *error= process ? 0 : GetLastError();
    if (!RevertToSelf())
    {
      sql_print_error("RevertToSelf() failed (error: %u)",
                      uint(GetLastError()));
      abort();
    }
  }
  return process;
}


/*
  Process handles of pipe clients, shared by their connections, so that a
  process is opened, with impersonation maybe, once. The handle keeps its pid
  from being reused. There are few processes, so the search is linear.
*/
struct Client_process
{
  DWORD pid;
  HANDLE handle;
  /* The connections that wait for the handle */
  uint ref_count;
};
static SRWLOCK processes_lock= SRWLOCK_INIT;
static Dynamic_array<Client_process> processes(PSI_INSTRUMENT_MEM, 0, 8);


/**
  Find a client process, the caller holds processes_lock.

  @param pid  process id of the client

  @return the entry, or nullptr
*/
static Client_process *find_client_process(DWORD pid)
{
  for (size_t i= 0; i < processes.size(); i++)
    if (processes[i].pid == pid)
      return &processes[i];
  return nullptr;
}


/**
  Get the process handle of a pipe client for one more connection, opening it
  for the first. Pair with release_client_process().

  @param pid    process id of the client
  @param pipe   server end of the pipe, to impersonate the client if needed
  @param error  [out] error of opening the process, if it failed

  @return handle of the process, or nullptr
*/
static HANDLE acquire_client_process(DWORD pid, HANDLE pipe, DWORD *error)
{
  AcquireSRWLockExclusive(&processes_lock);
  Client_process *p= find_client_process(pid);
  if (p)
  {
    p->ref_count++;
    HANDLE h= p->handle;
    ReleaseSRWLockExclusive(&processes_lock);
    return h;
  }
  ReleaseSRWLockExclusive(&processes_lock);

  HANDLE process= open_client_process(pipe, pid, error);
  if (!process)
    return nullptr;
  AcquireSRWLockExclusive(&processes_lock);
  p= find_client_process(pid);
  /* Another connection was faster */
  if (p)
  {
    CloseHandle(process);
    process= p->handle;
    p->ref_count++;
  }
  else if (processes.append(Client_process{pid, process, 1}))
  {
    CloseHandle(process);
    process= nullptr;
    *error= ERROR_OUTOFMEMORY;
  }
  ReleaseSRWLockExclusive(&processes_lock);
  return process;
}


/**
  Release a connection's hold of its client process, the last one closes it.

  @param pid  process id of the client
*/
static void release_client_process(DWORD pid)
{
  HANDLE h= nullptr;
  AcquireSRWLockExclusive(&processes_lock);
  Client_process *p= find_client_process(pid);
  if (p && --p->ref_count == 0)
  {
    h= p->handle;
    processes.del(p - processes.front());
  }
  ReleaseSRWLockExclusive(&processes_lock);
  if (h)
    CloseHandle(h);
}


/**
  Post the connection id to the port when a handle is signaled.

  @param id      id of the connection
  @param handle  event of a socket, or process of a pipe client

  @return the watch, or nullptr if the wait can't be set up
*/
static Disconnect_watch *add_wait(my_thread_id id, HANDLE handle)
{
  Disconnect_watch *w= new (std::nothrow) Disconnect_watch();
  if (!w)
    return nullptr;
  w->handle= handle;
  w->pid= 0;
  LONG status= my_NtCreateWaitCompletionPacket(
    &w->wait_completion, GENERIC_READ | GENERIC_WRITE, nullptr);
  if (status < 0)
  {
    watch_failed("NtCreateWaitCompletionPacket", status);
    delete w;
    return nullptr;
  }
  BOOLEAN signaled;
  status= my_NtAssociateWaitCompletionPacket(
    w->wait_completion, iocp, handle, (PVOID) (ULONG_PTR) id, nullptr, 0, 0,
    &signaled);
  if (status >= 0)
    return w;
  watch_failed("NtAssociateWaitCompletionPacket", status);
  CloseHandle(w->wait_completion);
  delete w;
  return nullptr;
}


/**
  Watch the client process of a named pipe connection, unless it is remote.

  @param id    id of the connection
  @param pipe  server end of the pipe

  @return the watch, or nullptr if the client is not watched
*/
static Disconnect_watch *add_pipe(my_thread_id id, HANDLE pipe)
{
  char host[256];
  ULONG pid;
  if (GetNamedPipeClientComputerNameA(pipe, host, sizeof(host)) ||
      GetLastError() != ERROR_PIPE_LOCAL ||
      !GetNamedPipeClientProcessId(pipe, &pid))
    return nullptr;
  DWORD error;
  HANDLE process= acquire_client_process(pid, pipe, &error);
  if (!process)
  {
    /*
      A process of another user needs impersonation, which needs the
      SeImpersonatePrivilege (a service has it), else the error is 1346.
    */
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      sql_print_warning("Can't open the process of a named pipe client "
                        "(error: %u), its connections are not aborted when "
                        "it exits", uint(error));
    return nullptr;
  }
  Disconnect_watch *w= add_wait(id, process);
  if (w)
    w->pid= pid;
  else
    release_client_process(pid);
  return w;
}


Disconnect_watch *disconnect_watcher_add(my_thread_id thread_id, Vio *vio)
{
  if (!vio || !thread_started || stopping)
    return nullptr;
  if (vio_type(vio) == VIO_TYPE_NAMEDPIPE)
    return add_pipe(thread_id, vio->hPipe);
  if (!is_socket_vio(vio))
    return nullptr;
  HANDLE h= CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!h)
  {
    watch_failed("CreateEvent", GetLastError());
    return nullptr;
  }
  if (WSAEventSelect(vio_fd(vio), h, FD_CLOSE))
    watch_failed("WSAEventSelect", WSAGetLastError());
  else if (Disconnect_watch *w= add_wait(thread_id, h))
    return w;
  CloseHandle(h);
  return nullptr;
}


void disconnect_watcher_remove(Disconnect_watch **handle, Vio *vio)
{
  Disconnect_watch *w= *handle;
  if (!w)
    return;
  *handle= nullptr;
  /*
    Cancelling the packet drops a queued completion. One that is taken already
    is harmless, COM_QUIT is not aborted and an ending connection is killed
    already. The event selection ends before its event is closed.
  */
  if (!w->pid && vio)
    WSAEventSelect(vio_fd(vio), nullptr, 0);
  my_NtCancelWaitCompletionPacket(w->wait_completion, TRUE);
  CloseHandle(w->wait_completion);
  if (w->pid)
    release_client_process(w->pid);
  else
    CloseHandle(w->handle);
  delete w;
}


/* No connection is left to add a wait. Joins the thread. */
void disconnect_watcher_end()
{
  disconnect_watcher_stop();
  if (wake_failed)
    return;
  join_watcher();
  DBUG_ASSERT(processes.is_empty());
  processes.free_memory();
  close_handles();
}

#else
#error "The disconnect watcher needs an implementation for this platform"
#endif

#endif
